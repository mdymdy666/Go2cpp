/**
 * @file debug_server.cpp
 * @brief DebugServer 实现：Unix domain socket + 稳定命令协议。
 */

#include "go2cpp/debug/debug_server.hpp"
#include "go2cpp/debug/debug_fiber.hpp"

#include "go2cpp/json.hpp"
#include "go2cpp/network/socket.hpp"
#include "go2cpp/tools/strings.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <thread>

#include <dirent.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

namespace go2cpp::debug {
namespace {

// 半包/慢连接防护（P2-1）：首个字节 100ms，整条命令 1s。
constexpr std::size_t kMaxRequestBytes = 4096;
constexpr int kRequestFirstByteTimeoutMs = 100;
constexpr int kRequestTotalTimeoutMs = 1000;
// 错误帧的优雅关闭：最多 drain 64KiB / 1s 后 close，避免 RST 冲掉响应（P2-2）。
constexpr std::size_t kMaxDrainBytes = 64U * 1024U;
constexpr int kDrainTimeoutMs = 1000;
// 单条响应上限：超大 dump 在服务端就拒绝，避免构造/传输无界 payload（P2-4）。
constexpr std::size_t kMaxResponseBytes = 64ULL * 1024ULL * 1024ULL;
// 并发订阅上限（P2-3）：每个订阅一个 OS 线程，必须有上限。
constexpr std::size_t kMaxSubscriptions = 16;
/// 已预留的订阅线程数（fetch_add 先占名额，线程结束时释放）。
std::atomic<std::size_t> g_subscription_threads{0};

/// @brief 给 accept 出来的连接补 `TCP_NODELAY`（B-02）。
/// @details DebugServer 当前监听 Unix domain socket（`/tmp/go2cpp-<pid>.sock`），
///          Nagle 算法只存在于 TCP，对 AF_UNIX 设置会返回 ENOPROTOOPT；因此这里
///          先按 `SO_DOMAIN` 判断，只在 TCP 域上设置——既不产生无效系统调用，
///          也保证将来改成 TCP 监听时不会漏掉。
/// @return 1 = 已设置成功；0 = 显式关闭/设置失败；-1 = 非 TCP 域或查询失败。
int ApplyAcceptNoDelay(int fd) noexcept {
    int domain = 0;
    socklen_t length = sizeof(domain);
    if (::getsockopt(fd, SOL_SOCKET, SO_DOMAIN, &domain, &length) != 0) {
        return -1;
    }
    if (domain != AF_INET && domain != AF_INET6) {
        return -1;
    }
    if (!go2cpp::net::Socket::TcpNoDelayDefault()) {
        return 0;
    }
    return go2cpp::net::Socket::SetTcpNoDelay(fd) ? 1 : 0;
}

const char* const kKnownComponents[] = {
    "scheduler", "fiber", "channel", "parking_condition", "mutex",
    "condition_variable", "io", "events", "runtime",
};

std::string Trim(const std::string& input) {
    return AsciiTrim(input, " \t\r\n");
}

std::string Upper(std::string value) {
    for (char& ch : value) {
        if (ch >= 'a' && ch <= 'z') {
            ch = static_cast<char>(ch - 'a' + 'A');
        }
    }
    return value;
}

std::string OkResponse(const std::string& payload) {
    return std::string(kDebugProtocol) + " OK " +
           std::to_string(payload.size()) + "\n" + payload;
}

std::string ErrorResponse(const std::string& code,
                          const std::string& message) {
    return std::string(kDebugProtocol) + " ERR " + code + " " + message + "\n";
}

/// PUSH 帧头：`GO2CPP-DEBUG/1 PUSH <bytes>\n<payload>`（与 OK 帧同构）。
std::string PushResponse(const std::string& payload) {
    return std::string(kDebugProtocol) + " PUSH " +
           std::to_string(payload.size()) + "\n" + payload;
}

/// `SUBSCRIBE` 的参数（topic + 轮询间隔 + 单帧上限）。
struct SubscriptionOptions {
    bool valid{false};
    std::string error;
    std::string topic{"events"};
    std::chrono::milliseconds interval{20};
    std::size_t max_events{256};
};

std::vector<std::string> SplitTokens(const std::string& input) {
    std::vector<std::string> tokens;
    std::size_t index = 0;
    while (index < input.size()) {
        while (index < input.size() &&
               (input[index] == ' ' || input[index] == '\t')) {
            ++index;
        }
        const std::size_t begin = index;
        while (index < input.size() && input[index] != ' ' &&
               input[index] != '\t') {
            ++index;
        }
        if (index > begin) {
            tokens.push_back(input.substr(begin, index - begin));
        }
    }
    return tokens;
}

SubscriptionOptions ParseSubscriptionOptions(const std::string& argument) {
    SubscriptionOptions options;
    const std::vector<std::string> tokens = SplitTokens(argument);
    if (tokens.empty()) {
        options.error = "missing-topic";
        return options;
    }
    const std::string topic = tokens[0];
    if (topic != "events" && topic != "event" && topic != "events:all" &&
        topic != "*") {
        options.error = "unknown-topic";
        return options;
    }
    options.topic = "events";
    if (tokens.size() >= 2) {
        try {
            const unsigned long long interval = std::stoull(tokens[1]);
            if (interval == 0) {
                options.error = "invalid-interval";
                return options;
            }
            options.interval = std::chrono::milliseconds(
                static_cast<long long>(std::min<unsigned long long>(
                    interval, 5000ULL)));
        } catch (...) {
            options.error = "invalid-interval";
            return options;
        }
    }
    if (tokens.size() >= 3) {
        try {
            const unsigned long long maximum = std::stoull(tokens[2]);
            if (maximum == 0) {
                options.error = "invalid-max-events";
                return options;
            }
            options.max_events = static_cast<std::size_t>(
                std::min<unsigned long long>(maximum, 4096ULL));
        } catch (...) {
            options.error = "invalid-max-events";
            return options;
        }
    }
    options.valid = true;
    return options;
}

/// 事件行格式与 `EventRing::DumpText` 保持一致（末尾追加 pid）。
std::string FormatEventLine(const EventRecord& event) {
    std::string line;
    line += std::to_string(event.seq);
    line += ' ';
    line += std::to_string(event.ts_ns);
    line += ' ';
    line += std::to_string(event.tid);
    line += ' ';
    line += SchedEventCodeName(static_cast<SchedEventCode>(event.code));
    line += ' ';
    line += std::to_string(event.gid);
    line += ' ';
    line += std::to_string(event.arg2);
    line += ' ';
    line += std::to_string(event.pid);
    line += '\n';
    return line;
}

std::string BuildSubscriptionHeader(const SubscriptionOptions& options,
                                    const EventCursor& cursor_start) {
    std::string payload;
    payload += "schema=go2cpp.dump.events.subscription/1\n";
    payload += "version=1\n";
    payload += "topic=";
    payload += options.topic;
    payload += "\n";
    payload += "protocol=GO2CPP-DEBUG-PUSH/1\n";
    payload += "interval_ms=" + std::to_string(options.interval.count()) + "\n";
    payload += "max_events=" + std::to_string(options.max_events) + "\n";
    // v6：游标主键是时间戳；同时导出 thread_id/seq 作为同一纳秒的
    // 打破并列信息（客户端只需回传这三个值即可无重无漏）。
    payload += "cursor_start=" + std::to_string(cursor_start.ts_ns) + "\n";
    payload += "cursor_start_thread_id=" +
               std::to_string(cursor_start.thread_id) + "\n";
    payload += "cursor_start_seq=" +
               std::to_string(cursor_start.local_seq) + "\n";
    payload += "capacity=" + std::to_string(EventRing::kCapacity) + "\n";
    payload += "critical_capacity=" +
               std::to_string(EventRing::kCriticalCapacity) + "\n";
    payload += "consistency=per_call\n";
    return payload;
}

std::string BuildPushPayload(const std::vector<EventRecord>& events,
                             std::uint64_t dropped_gap,
                             std::uint64_t dropped_backpressure,
                             std::uint64_t published_total,
                             const EventCursor& cursor) {
    const std::uint64_t first_seq = events.front().seq;
    const std::uint64_t last_seq = events.back().seq;
    const std::uint64_t dropped_total = dropped_gap + dropped_backpressure;
    std::string payload;
    payload += "schema=go2cpp.dump.events.push/1\n";
    payload += "version=1\n";
    // first_seq/last_seq 是首尾事件的**per-thread** 序号（兼容旧字段）。
    payload += "first_seq=" + std::to_string(first_seq) + "\n";
    payload += "last_seq=" + std::to_string(last_seq) + "\n";
    payload += "first_ts_ns=" + std::to_string(events.front().ts_ns) + "\n";
    payload += "last_ts_ns=" + std::to_string(events.back().ts_ns) + "\n";
    payload += "event_count=" + std::to_string(events.size()) + "\n";
    payload += "dropped_total=" + std::to_string(dropped_total) + "\n";
    payload += "dropped_gap=" + std::to_string(dropped_gap) + "\n";
    payload += "dropped_backpressure=" +
               std::to_string(dropped_backpressure) + "\n";
    // seq_last 自 v6 起是累计发布事件数（仍单调），不再是全局 seq。
    payload += "seq_last=" + std::to_string(published_total) + "\n";
    payload += "cursor=" + std::to_string(cursor.ts_ns) + "\n";
    payload += "cursor_thread_id=" + std::to_string(cursor.thread_id) + "\n";
    payload += "cursor_seq=" + std::to_string(cursor.local_seq) + "\n";
    payload += "# seq ts_ns tid code gid arg2 pid\n";
    for (const auto& event : events) {
        payload += FormatEventLine(event);
    }
    return payload;
}

enum class SendResult { kOk, kBackpressure, kFailed };

/**
 * @brief 带 SO_SNDTIMEO 上限的发送（ServeSubscription 专用）。
 * @details
 * - `kOk`：整段写完；
 * - `kBackpressure`：**一个字节都没写出去**（内核缓冲满）→ 调用方丢弃这一批，
 *   帧边界仍然完整；
 * - `kFailed`：写了一半或连接错误 → 帧边界已破坏，调用方必须关闭连接，
 *   绝不把半帧继续留在流里。
 */
SendResult SendBounded(int fd, const std::string& data, std::size_t* written) {
    std::size_t offset = 0;
    while (offset < data.size()) {
        const ssize_t sent = ::send(fd, data.data() + offset,
                                    data.size() - offset, MSG_NOSIGNAL);
        if (sent > 0) {
            offset += static_cast<std::size_t>(sent);
            continue;
        }
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (written != nullptr) {
                *written = offset;
            }
            return offset == 0 ? SendResult::kBackpressure
                               : SendResult::kFailed;
        }
        if (written != nullptr) {
            *written = offset;
        }
        return SendResult::kFailed;
    }
    if (written != nullptr) {
        *written = offset;
    }
    return SendResult::kOk;
}

/// 阻塞发送（仅用于订阅建立时的 OK 头；调用前已设置 SO_SNDTIMEO）。
bool SendBlocking(int fd, const std::string& data) {
    std::size_t offset = 0;
    while (offset < data.size()) {
        const ssize_t sent = ::send(fd, data.data() + offset,
                                    data.size() - offset, MSG_NOSIGNAL);
        if (sent <= 0) {
            return false;
        }
        offset += static_cast<std::size_t>(sent);
    }
    return true;
}

/// 客户端是否还活着：`recv(MSG_PEEK)` 返回 0 表示对端关闭。
bool ClientAlive(int fd) {
    char byte = 0;
    const ssize_t received = ::recv(fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
    if (received == 0) {
        return false;
    }
    if (received < 0) {
        return errno == EAGAIN || errno == EWOULDBLOCK;
    }
    // 客户端发了意料之外的数据：连接仍存活，忽略即可。
    return true;
}

std::uint64_t NowNs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

/// 尽力读完并丢弃对端剩余字节（最多 max_bytes / timeout_ms）。
/// 错误帧必须先 shutdown(SHUT_WR) 再 drain：socket 上残留未读数据时
/// close 会发 RST，内核会丢弃已写出的响应，客户端只能看到 ConnectionReset。
void DrainClient(int fd, std::size_t max_bytes, int timeout_ms) {
    std::size_t drained = 0;
    const std::uint64_t deadline =
        NowNs() + static_cast<std::uint64_t>(timeout_ms) * 1000000ULL;
    char buffer[4096];
    while (drained < max_bytes && NowNs() < deadline) {
        const ssize_t received = ::recv(fd, buffer, sizeof(buffer), 0);
        if (received > 0) {
            drained += static_cast<std::size_t>(received);
            continue;
        }
        if (received == 0) {
            break;  // 对端已关闭写端。
        }
        if (errno == EINTR) {
            continue;
        }
        break;  // EAGAIN（读超时）或连接错误。
    }
}

[[maybe_unused]] bool IsKnownComponent(const std::string& name) {
    for (const char* known : kKnownComponents) {
        if (name == known) {
            return true;
        }
    }
    return false;
}

// CLI 的 sections 名称到组件名的稳定映射（只做别名，不改输出 schema）。
std::string MapSectionToComponent(const std::string& section) {
    if (section == "scheduler" || section == "goroutines" ||
        section == "gmp") {
        return "scheduler";
    }
    if (section == "locks" || section == "sync" || section == "mutex") {
        return "mutex";
    }
    if (section == "channel" || section == "channels") {
        return "channel";
    }
    if (section == "io" || section == "netpoll") {
        return "io";
    }
    if (section == "fiber" || section == "fibers") {
        return "fiber";
    }
    if (section == "events") {
        return "events";
    }
    return {};
}

}  // namespace

DebugServer& DebugServer::Instance() {
    static DebugServer* instance = new DebugServer();
    return *instance;
}

DebugServer::DebugServer() { InstallBuiltins(); }

DebugServer::~DebugServer() { Stop(); }

void DebugServer::InstallBuiltins() {
    // runtime 与 events 始终可用：它们是「服务自描述」与「事件导出」的入口。
    m_components.emplace(
        "runtime",
        std::make_pair(std::string("runtime/build flags and pid"),
                       DebugProvider([] { return DumpRuntimeInfo(); })));
    m_components.emplace(
        "events",
        std::make_pair(std::string("scheduler park/wake/runqueue event ring"),
                       DebugProvider([] { return DumpEvents(0); })));
}

std::string DebugServer::DefaultPath() {
    return "/tmp/go2cpp-" + std::to_string(::getpid()) + ".sock";
}

bool DebugServer::Start(const std::string& path) {
    if (running()) {
        return true;
    }
#if !(defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP)
    (void)path;  // OFF 构建不监听 socket，参数仅用于保持公开接口稳定。
    {
        std::lock_guard<std::mutex> lock(m_error_mutex);
        m_last_error = "debug-dump-disabled";
    }
    return false;
#else
    const std::string socket_path = path.empty() ? DefaultPath() : path;
    if (socket_path.size() >= sizeof(sockaddr_un::sun_path)) {
        std::lock_guard<std::mutex> lock(m_error_mutex);
        m_last_error = "socket-path-too-long";
        return false;
    }
    const int listen_fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listen_fd < 0) {
        std::lock_guard<std::mutex> lock(m_error_mutex);
        m_last_error = std::string("socket: ") + std::strerror(errno);
        return false;
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, socket_path.c_str(),
                socket_path.size() + 1U);
    // 允许覆盖遗留 socket；不是 socket 的路径也会被移除（与常见调试
    // 工具一致：路径由调用方显式指定）。
    (void)::unlink(socket_path.c_str());
    if (::bind(listen_fd, reinterpret_cast<const sockaddr*>(&address),
               sizeof(address)) != 0) {
        const int saved = errno;
        ::close(listen_fd);
        std::lock_guard<std::mutex> lock(m_error_mutex);
        m_last_error = std::string("bind: ") + std::strerror(saved);
        return false;
    }
    (void)::chmod(socket_path.c_str(), 0600);
    if (::listen(listen_fd, 16) != 0) {
        const int saved = errno;
        ::close(listen_fd);
        (void)::unlink(socket_path.c_str());
        std::lock_guard<std::mutex> lock(m_error_mutex);
        m_last_error = std::string("listen: ") + std::strerror(saved);
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(m_path_mutex);
        m_path = socket_path;
    }
    {
        std::lock_guard<std::mutex> lock(m_error_mutex);
        m_last_error.clear();
    }
    m_listen_fd = listen_fd;
    m_stop.store(false, std::memory_order_release);
    m_started_ns = NowNs();
    m_running.store(true, std::memory_order_release);
    try {
        m_thread = std::thread([this] { Run(); });
    } catch (...) {
        m_running.store(false, std::memory_order_release);
        ::close(listen_fd);
        m_listen_fd = -1;
        (void)::unlink(socket_path.c_str());
        std::lock_guard<std::mutex> lock(m_error_mutex);
        m_last_error = "thread-start-failed";
        return false;
    }
    // HTTP 探针是附加能力：绑定失败只记录 http_last_error，不影响 socket。
    (void)StartHttp();
    return true;
#endif
}

void DebugServer::Stop() {
    if (!running() && m_listen_fd < 0 && !http_running()) {
        StopHttp();
        return;
    }
    m_stop.store(true, std::memory_order_release);
    m_running.store(false, std::memory_order_release);
    if (m_thread.joinable()) {
        m_thread.join();
    }
    if (m_listen_fd >= 0) {
        ::close(m_listen_fd);
        m_listen_fd = -1;
    }
    std::string path;
    {
        std::lock_guard<std::mutex> lock(m_path_mutex);
        path.swap(m_path);
    }
    if (!path.empty()) {
        (void)::unlink(path.c_str());
    }
    StopHttp();
}

std::string DebugServer::path() const {
    std::lock_guard<std::mutex> lock(m_path_mutex);
    return m_path;
}

std::string DebugServer::last_error() const {
    std::lock_guard<std::mutex> lock(m_error_mutex);
    return m_last_error;
}

void DebugServer::Run() noexcept {
    while (!m_stop.load(std::memory_order_acquire)) {
        pollfd descriptor{};
        descriptor.fd = m_listen_fd;
        descriptor.events = POLLIN;
        const int ready = ::poll(&descriptor, 1, 100);
        if (ready <= 0) {
            continue;
        }
        const int client = ::accept(m_listen_fd, nullptr, nullptr);
        if (client < 0) {
            continue;
        }
        m_last_accept_nodelay.store(ApplyAcceptNoDelay(client),
                                    std::memory_order_relaxed);
        HandleClient(client);
        ::close(client);
    }
}

void DebugServer::HandleClient(int client_fd) noexcept {
    // 读取阶段：首个字节最多等 kRequestFirstByteTimeoutMs；已经有部分数据后
    // 允许继续读，但整条命令不超过 kRequestTotalTimeoutMs。这样「连接后不发」
    // 或「每 99ms 续一个字节」都不能把单线程 accept 循环拖住（P2-1）。
    // 发送阶段仍给 5s：大 dump 需要时间。
    timeval timeout{};
    timeout.tv_sec = 0;
    timeout.tv_usec = kRequestFirstByteTimeoutMs * 1000;
    (void)::setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                       sizeof(timeout));
    timeout.tv_sec = 5;
    timeout.tv_usec = 0;
    (void)::setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout,
                       sizeof(timeout));

    std::string request;
    request.reserve(128);
    char buffer[512];
    bool frame_too_long = false;
    bool request_timeout = false;
    bool peer_closed = false;
    const std::uint64_t read_start_ns = NowNs();
    std::uint64_t last_data_ns = read_start_ns;
    while (request.size() <= kMaxRequestBytes) {
        const ssize_t received = ::recv(client_fd, buffer, sizeof(buffer), 0);
        if (received > 0) {
            request.append(buffer, static_cast<std::size_t>(received));
            last_data_ns = NowNs();
            if (request.size() > kMaxRequestBytes) {
                // 超长帧：停止读取，回 ERR 后优雅关闭（P2-2）。
                frame_too_long = true;
                break;
            }
            if (request.find('\n') != std::string::npos) {
                break;
            }
            continue;
        }
        if (received == 0) {
            peer_closed = true;
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            const std::uint64_t now = NowNs();
            // 半包：SO_RCVTIMEO 已经等了 100ms 没有新字节 → 直接断连。
            // 这样一条只发了部分命令的连接最多占用 accept 循环 100ms（P2-1）。
            if (now - last_data_ns >=
                static_cast<std::uint64_t>(kRequestFirstByteTimeoutMs) * 1000000ULL) {
                request_timeout = true;
                break;
            }
            // 持续滴灌（间隔 <100ms）但始终没有换行：整条命令最多 1s。
            if (now - read_start_ns >=
                static_cast<std::uint64_t>(kRequestTotalTimeoutMs) * 1000000ULL) {
                request_timeout = true;
                break;
            }
            continue;
        }
        break;
    }
    if (peer_closed && request.empty()) {
        // 只 connect 不发命令（活性探测/健康检查）：静默关闭。
        return;
    }
    const std::size_t newline = request.find('\n');
    if (newline != std::string::npos) {
        request.resize(newline);
    }

    std::string response;
    bool detached = false;
    bool graceful_close = false;
    if (frame_too_long) {
        response = ErrorResponse("bad-command", "frame-too-long");
        graceful_close = true;
    } else if (request_timeout) {
        response = ErrorResponse("bad-command", "request-timeout");
        graceful_close = true;
    } else {
        try {
            const std::string trimmed = Trim(request);
            if (!trimmed.empty() && trimmed.front() == '{') {
                response = HandleJsonCommand(trimmed);
            } else {
                const std::size_t space = trimmed.find_first_of(" \t");
                const std::string command = Upper(trimmed.substr(
                    0, space == std::string::npos ? trimmed.size() : space));
                if (command == "SUBSCRIBE") {
                    // 长连接：把这条连接交给独立线程，accept 循环继续服务
                    // 其它客户端（DUMP/STATUS 不被订阅阻塞）。每个订阅一个
                    // OS 线程，因此必须先预留名额，超过上限直接拒绝（P2-3）。
                    ++m_requests;
                    const std::string argument =
                        space == std::string::npos
                            ? std::string()
                            : Trim(trimmed.substr(space));
                    const std::size_t active =
                        g_subscription_threads.fetch_add(
                            1, std::memory_order_acq_rel) +
                        1;
                    if (active > kMaxSubscriptions) {
                        (void)g_subscription_threads.fetch_sub(
                            1, std::memory_order_acq_rel);
                        response = ErrorResponse(
                            "too-many-subscribers",
                            "subscription limit " +
                                std::to_string(kMaxSubscriptions) +
                                " reached; retry after a subscriber disconnects");
                    } else {
                        const int subscription_fd = ::dup(client_fd);
                        if (subscription_fd < 0) {
                            (void)g_subscription_threads.fetch_sub(
                                1, std::memory_order_acq_rel);
                            response =
                                ErrorResponse("internal", "dup-failed");
                        } else {
                            try {
                                std::thread([this, subscription_fd, argument] {
                                    struct Release {
                                        ~Release() {
                                            g_subscription_threads.fetch_sub(
                                                1,
                                                std::memory_order_acq_rel);
                                        }
                                    } release;
                                    ServeSubscription(subscription_fd,
                                                      argument);
                                }).detach();
                                detached = true;
                            } catch (...) {
                                ::close(subscription_fd);
                                (void)g_subscription_threads.fetch_sub(
                                    1, std::memory_order_acq_rel);
                                response = ErrorResponse(
                                    "internal", "thread-start-failed");
                            }
                        }
                    }
                } else {
                    response = HandleCommand(request);
                }
            }
        } catch (...) {
            response = ErrorResponse("internal", "handler-exception");
        }
    }
    if (detached) {
        // 连接所有权已移交给订阅线程；这里不能再关闭 client_fd 之外的
        // 东西，也不能再发送普通响应。
        return;
    }
    if (response.size() > kMaxResponseBytes) {
        // 超大响应在服务端拒绝：避免把 46.5MiB 级 payload 塞给 CLI 的
        // 16MiB 上限（P2-4），同时把错误明确交给调用方。
        response = ErrorResponse(
            "payload-too-large",
            "response exceeds " + std::to_string(kMaxResponseBytes) +
                " bytes; request a single component (JSON <component>) or "
                "use EVENTS <limit>");
        graceful_close = true;
    }
    std::size_t offset = 0;
    while (offset < response.size()) {
        const ssize_t sent = ::send(client_fd, response.data() + offset,
                                    response.size() - offset, MSG_NOSIGNAL);
        if (sent <= 0) {
            break;
        }
        offset += static_cast<std::size_t>(sent);
    }
    if (graceful_close) {
        // 先关写端（客户端读到 ERR 后看到 EOF），再把对端剩余字节 drain
        // 掉：close 前没有未读数据，内核就不会发 RST 把响应冲掉（P2-2）。
        (void)::shutdown(client_fd, SHUT_WR);
        DrainClient(client_fd, kMaxDrainBytes, kDrainTimeoutMs);
    }
}
void DebugServer::ServeSubscription(int fd,
                                    const std::string& argument) noexcept {
    m_subscribers.fetch_add(1, std::memory_order_relaxed);
    struct SubscriberGuard {
        std::atomic<std::uint64_t>* counter;
        ~SubscriberGuard() { counter->fetch_sub(1, std::memory_order_relaxed); }
    } guard{&m_subscribers};

    const SubscriptionOptions options = ParseSubscriptionOptions(argument);
    if (!options.valid) {
        (void)SendBlocking(fd, ErrorResponse("bad-command", options.error));
        ::close(fd);
        return;
    }
    if (!EventRing::Enabled()) {
        (void)SendBlocking(fd, ErrorResponse("disabled", "debug-dump-disabled"));
        ::close(fd);
        return;
    }
    timeval timeout{};
    timeout.tv_sec = 2;
    timeout.tv_usec = 0;
    (void)::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    // v6：订阅起点是当前最新事件的时间戳游标，而不是全局 seq。
    EventCursor cursor = EventRing::CurrentCursor();
    if (!SendBlocking(fd,
                      OkResponse(BuildSubscriptionHeader(options, cursor)))) {
        ::close(fd);
        return;
    }
    // 订阅建立后把发送超时收紧到 100ms：慢客户端最多让本线程每轮多等
    // 100ms，绝不会把事件记录方或 accept 循环拖住。
    timeout.tv_sec = 0;
    timeout.tv_usec = 100 * 1000;
    (void)::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    std::uint64_t dropped_gap_total = 0;
    std::uint64_t dropped_backpressure_total = 0;
    std::uint64_t idle_rounds = 0;
    // 同一线程的 local seq 跳号才能精确计数（环形覆盖）；
    // 跨线程事件的丢失无法从时间戳游标反推，这里只做尽力统计。
    std::map<std::uint32_t, std::uint64_t> last_seq_by_thread;
    if (cursor.thread_id != 0U) {
        last_seq_by_thread[cursor.thread_id] = cursor.local_seq;
    }
    while (!m_stop.load(std::memory_order_acquire)) {
        const EventCursor latest = EventRing::CurrentCursor();
        if (EventCursorEqual(latest, cursor)) {
            if (!ClientAlive(fd)) {
                break;
            }
            std::this_thread::sleep_for(options.interval);
            continue;
        }
        std::vector<EventRecord> events =
            EventRing::ReadSince(cursor, options.max_events);
        if (events.empty()) {
            // 最新游标已前进但环形里暂时看不到记录：写入者可能刚拿到时间戳、
            // 槽位还没发布。短暂重试；连续多轮为空才按「已被覆盖」跳过。
            if (++idle_rounds < 100) {
                if (!ClientAlive(fd)) {
                    break;
                }
                std::this_thread::sleep_for(options.interval);
                continue;
            }
            cursor = latest;
            idle_rounds = 0;
            continue;
        }
        idle_rounds = 0;
        for (const auto& event : events) {
            const auto found = last_seq_by_thread.find(event.thread_id);
            if (found != last_seq_by_thread.end() &&
                event.seq > found->second + 1U) {
                dropped_gap_total += event.seq - found->second - 1U;
            }
            if (found == last_seq_by_thread.end() ||
                event.seq > found->second) {
                last_seq_by_thread[event.thread_id] = event.seq;
            }
        }
        const EventCursor next_cursor = CursorOf(events.back());
        const std::uint64_t published = EventRing::PublishedEvents();
        const std::string payload = BuildPushPayload(
            events, dropped_gap_total, dropped_backpressure_total, published,
            next_cursor);
        std::size_t written = 0;
        const SendResult result = SendBounded(fd, PushResponse(payload), &written);
        if (result == SendResult::kFailed) {
            // 帧边界已破坏：关闭连接，让客户端重连（不要留半帧）。
            break;
        }
        if (result == SendResult::kBackpressure) {
            // 背压策略：丢弃最旧的一整批并推进游标，后续帧如实报告。
            dropped_backpressure_total += events.size();
            cursor = next_cursor;
            if (!ClientAlive(fd)) {
                break;
            }
            std::this_thread::sleep_for(std::min<std::chrono::milliseconds>(
                options.interval, std::chrono::milliseconds(50)));
            continue;
        }
        cursor = next_cursor;
        // 还有积压就立即继续发下一帧；没有则等待新事件。
        if (EventCursorEqual(EventRing::CurrentCursor(), cursor)) {
            std::this_thread::sleep_for(options.interval);
        }
    }
    ::close(fd);
}

bool DebugServer::Register(const std::string& name,
                           const std::string& description,
                           DebugProvider provider) {
    if (name.empty() || !provider) {
        return false;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    m_components[name] = std::make_pair(description, std::move(provider));
    return true;
}

bool DebugServer::Unregister(const std::string& name) {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_components.erase(name) > 0U;
}

std::vector<std::string> DebugServer::Components() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<std::string> names;
    names.reserve(m_components.size());
    for (const auto& entry : m_components) {
        names.push_back(entry.first);
    }
    return names;
}

DumpPayload DebugServer::Dump(const std::string& component) const {
    const bool all = component.empty() || component == "all" ||
                     component == "*" || component == "ALL";
    // 在锁内复制 provider，锁外调用：provider 可能读取任意运行时状态，
    // 不应阻塞注册/注销。
    std::map<std::string, std::pair<std::string, DebugProvider>> selected;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (all) {
            selected = m_components;
        } else {
            const auto found = m_components.find(component);
            if (found == m_components.end()) {
                return DumpPayload::Unavailable("unknown-component");
            }
            selected.emplace(found->first, found->second);
        }
    }

    if (!all) {
        try {
            return selected.begin()->second.second();
        } catch (...) {
            return DumpPayload::Unavailable("provider-exception");
        }
    }

    // 一致性探针覆盖**全部** provider 的执行窗口：先取开始序号，
    // 逐个采集组件，最后取结束序号。两者不同说明期间有并发事件。
    ConsistencyStamp stamp = BeginConsistency();
    const std::uint64_t steady_ns = NowNs();
    const std::uint64_t wall_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    std::vector<std::pair<std::string, DumpPayload>> parts;
    parts.reserve(selected.size());
    for (const auto& entry : selected) {
        DumpPayload part;
        try {
            part = entry.second.second();
        } catch (...) {
            part = DumpPayload::Unavailable("provider-exception");
        }
        parts.emplace_back(entry.first, std::move(part));
    }
    EndConsistency(stamp);

    std::vector<std::string> unavailable;
    for (const char* known : kKnownComponents) {
        if (selected.find(known) == selected.end()) {
            unavailable.emplace_back(known);
        }
    }
    std::sort(unavailable.begin(), unavailable.end());

    DumpPayload payload;
    payload.text = "schema=go2cpp.dump.all/1\n";
    payload.text += "version=1\n";
    payload.text += "pid=" + std::to_string(::getpid()) + "\n";
    payload.text += "steady_ns=" + std::to_string(steady_ns) + "\n";
    payload.text += "wall_unix_ns=" + std::to_string(wall_ns) + "\n";
    payload.text += "consistent=per_call\n";
    payload.text += "seq_at_start=" + std::to_string(stamp.seq_start) + "\n";
    payload.text += "seq_at_end=" + std::to_string(stamp.seq_end) + "\n";
    payload.text += std::string("changed_during_dump=") +
                    (stamp.Changed() ? "true" : "false") + "\n";
    payload.text += std::string("event_seq_supported=") +
                    (stamp.supported ? "true" : "false") + "\n";
    payload.text += "component_count=" + std::to_string(parts.size()) + "\n";
    payload.text += "unavailable=";
    for (std::size_t i = 0; i < unavailable.size(); ++i) {
        if (i != 0) {
            payload.text += ',';
        }
        payload.text += unavailable[i];
    }
    payload.text += "\n";

    payload.json = "{\"schema\":\"go2cpp.dump.all/1\",\"version\":1";
    payload.json += ",\"pid\":" + std::to_string(::getpid());
    payload.json += ",\"steady_ns\":" + std::to_string(steady_ns);
    payload.json += ",\"wall_unix_ns\":" + std::to_string(wall_ns);
    payload.json += ",\"consistent\":\"per_call\"";
    payload.json += JsonConsistency(stamp);
    payload.json += ",\"components\":{";
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i != 0) {
            payload.json += ',';
        }
        payload.json += "\"" + JsonEscape(parts[i].first) + "\":";
        const DumpPayload& part = parts[i].second;
        payload.json += part.available
                            ? part.json
                            : "{\"available\":false,\"reason\":\"" +
                                  JsonEscape(part.reason) + "\"}";
    }
    payload.json += "},\"unavailable\":[";
    for (std::size_t i = 0; i < unavailable.size(); ++i) {
        if (i != 0) {
            payload.json += ',';
        }
        payload.json += "\"" + JsonEscape(unavailable[i]) + "\"";
    }
    payload.json += "]}";

    // 逐组件追加 text 段（json 已在上面按同一顺序拼好）。
    for (const auto& part : parts) {
        payload.text += "\n[component ";
        payload.text += part.first;
        payload.text += "]\n";
        payload.text += part.second.text;
    }
    return payload;
}

std::string DebugServer::HandleCommand(const std::string& line) {
    const std::string trimmed = Trim(line);
    if (trimmed.empty()) {
        return ErrorResponse("bad-command", "empty-command");
    }
    const std::size_t space = trimmed.find_first_of(" \t");
    const std::string command =
        Upper(trimmed.substr(0, space == std::string::npos ? trimmed.size()
                                                           : space));
    const std::string argument =
        space == std::string::npos ? std::string() : Trim(trimmed.substr(space));
    ++m_requests;

    if (command == "PING") {
        return OkResponse("pong");
    }
    if (command == "HELP") {
        return OkResponse(
            "commands: PING | LIST | STATUS | DUMP <component>|ALL | "
            "TEXT <component>|ALL | JSON <component>|ALL | EVENTS [limit] | "
            "VIEW <name> [gid] | VIEWS | "
            "SUBSCRIBE <topic> [interval_ms] [max_events] | HELP\n"
            "views: gmp | timeline <gid> | sched | park | timer | primitives\n"
            "protocol: GO2CPP-DEBUG/1\npush: GO2CPP-DEBUG-PUSH/1\n");
    }
    if (command == "STATUS") {
        const std::uint64_t now = NowNs();
        std::string status;
        status += "pid=" + std::to_string(::getpid()) + "\n";
        status += "protocol=";
        status += kDebugProtocol;
        status += "\n";
        status += "debug_dump=";
        status += EventRing::Enabled() ? "on" : "off";
        status += "\n";
        status += "running=";
        status += running() ? "true" : "false";
        status += "\n";
        status += "path=" + path() + "\n";
        status += "uptime_ns=" +
                  std::to_string(m_started_ns == 0U ? 0U : now - m_started_ns) +
                  "\n";
        status += "components=" + std::to_string(Components().size()) + "\n";
        status += "subscribers=" + std::to_string(m_subscribers.load()) + "\n";
        status += "requests=" + std::to_string(m_requests.load()) + "\n";
        const int nodelay = m_last_accept_nodelay.load(std::memory_order_relaxed);
        status += "accept_nodelay=";
        status += nodelay < 0 ? "unknown" : (nodelay == 0 ? "off" : "on");
        status += "\n";
        return OkResponse(status);
    }
    if (command == "LIST") {
        const auto registered = Components();
        std::string listing;
        listing += "protocol=";
        listing += kDebugProtocol;
        listing += "\n";
        for (const auto& name : registered) {
            listing += name + "\tavailable\n";
        }
        for (const char* known : kKnownComponents) {
            if (std::find(registered.begin(), registered.end(), known) ==
                registered.end()) {
                listing += std::string(known) + "\tunavailable\treason=not_registered\n";
            }
        }
        return OkResponse(listing);
    }
    if (command == "SUBSCRIBE") {
        // HandleClient 会在 socket 路径上拦截 SUBSCRIBE 并转成长连接；
        // 进程内调用没有连接可推送，明确报错而不是假装成功。
        return ErrorResponse("bad-command", "subscribe-requires-socket");
    }
    if (command == "VIEWS") {
        return OkResponse(
            "gmp\ntimeline\t--gid <gid>\nsched\npark\ntimer\n"
            "primitives\nschema: go2cpp.dump.view.<name>/1\n");
    }
    if (command == "VIEW") {
        const std::vector<std::string> tokens = SplitTokens(argument);
        if (tokens.empty()) {
            return ErrorResponse("bad-command", "missing-view");
        }
        const std::string& view = tokens[0];
        if (!IsKnownViewName(view)) {
            return ErrorResponse("unknown-view", view + "-not-supported");
        }
        std::uint64_t gid = 0;
        if (tokens.size() > 1) {
            try {
                std::size_t consumed = 0;
                gid = static_cast<std::uint64_t>(
                    std::stoull(tokens[1], &consumed));
                if (consumed != tokens[1].size()) {
                    return ErrorResponse("bad-command", "invalid-gid");
                }
            } catch (...) {
                return ErrorResponse("bad-command", "invalid-gid");
            }
        }
        if (view == "timeline" && gid == 0) {
            return ErrorResponse("bad-command", "missing-gid");
        }
        // 逐组件采集：不同 provider 各自独立加锁，避免在 server 锁内
        // 递归调用 Dump()。未注册的组件按空 JSON 处理。
        const DumpPayload scheduler_payload = Dump("scheduler");
        const DumpPayload timer_payload = Dump("timer");
        const DumpPayload io_payload = Dump("io");
        return OkResponse(BuildViewJson(
            view, gid,
            scheduler_payload.available ? scheduler_payload.json : std::string(),
            timer_payload.available ? timer_payload.json : std::string(),
            io_payload.available ? io_payload.json : std::string()));
    }
    if (command == "EVENTS") {
        std::size_t limit = 0;
        if (!argument.empty()) {
            try {
                limit = static_cast<std::size_t>(std::stoull(argument));
            } catch (...) {
                return ErrorResponse("bad-command", "invalid-limit");
            }
        }
        return OkResponse(DumpEvents(limit).text);
    }
    if (command == "DUMP" || command == "TEXT" || command == "JSON") {
        const std::string component = argument.empty() ? "all" : argument;
        // 组件名大小写敏感（稳定协议）：只接受精确名字；ALL/* 表示聚合。
        const std::string lookup =
            (component == "all" || component == "ALL" || component == "*")
                ? std::string("all")
                : component;
        if (lookup != "all") {
            const auto registered = Components();
            if (std::find(registered.begin(), registered.end(), lookup) ==
                registered.end()) {
                return ErrorResponse("unknown-component",
                                     lookup + "-not-registered");
            }
        }
        const DumpPayload payload = Dump(lookup);
        return OkResponse(command == "JSON" ? payload.json : payload.text);
    }
    return ErrorResponse("bad-command", "unknown-command");
}

std::string DebugServer::HandleJsonCommand(const std::string& line) {
    std::int64_t id = 0;
    const auto fail = [&id](const char* code, const std::string& message) {
        return std::string("{\"id\":") + std::to_string(id) +
               ",\"ok\":false,\"error\":{\"code\":\"" +
               JsonEscape(code) + "\",\"message\":\"" + JsonEscape(message) +
               "\"}}\n";
    };
    const auto parsed = go2cpp::ParseJson(line);
    if (!parsed.ok()) {
        return fail("protocol", "invalid JSON request");
    }
    const Json& request = parsed.value;
    if (!request.is_object()) {
        return fail("protocol", "request must be a JSON object");
    }
    if (request.contains("id")) {
        if (!request["id"].is_number_integer()) {
            return fail("protocol", "id must be an integer");
        }
        id = request["id"].get<std::int64_t>();
    }
    const std::string cmd = request.value("cmd", std::string{});
    try {
        if (cmd == "status") {
            Json data = Json::object();
            data["pid"] = static_cast<std::int64_t>(::getpid());
            Json goroutines = Json::object();
            Json runqueue = Json::object();
            std::int64_t running = 0;
            std::int64_t waiting = 0;
            std::int64_t total = 0;
            std::int64_t runnable_total = 0;
            std::int64_t runnable_global = 0;
            Json per_processor = Json::array();
            const DumpPayload scheduler_payload = Dump("scheduler");
            if (scheduler_payload.available) {
                const auto snapshot = go2cpp::ParseJson(scheduler_payload.json);
                if (snapshot.ok()) {
                    const Json& sched = snapshot.value;
                    if (sched.contains("tasks") && sched["tasks"].is_array()) {
                        total = static_cast<std::int64_t>(sched["tasks"].size());
                        for (const auto& task : sched["tasks"]) {
                            const std::string state =
                                task.value("state", std::string{});
                            if (state == "kRunning") {
                                ++running;
                            } else if (state == "kWaiting") {
                                ++waiting;
                            }
                        }
                    }
                    runnable_total = sched.value("runnable_total", 0);
                    runnable_global = sched.value("runnable_global", 0);
                    if (sched.contains("processors") &&
                        sched["processors"].is_array()) {
                        for (const auto& processor : sched["processors"]) {
                            per_processor.push_back(
                                processor.value("queued", 0));
                        }
                    }
                    data["scheduler"] = sched;
                } else {
                    data["scheduler"] = nullptr;
                }
            } else {
                data["scheduler"] = nullptr;
            }
            goroutines["total"] = total;
            goroutines["running"] = running;
            goroutines["runnable"] = runnable_total;
            goroutines["blocked"] = waiting;
            runqueue["total"] = runnable_total;
            runqueue["global"] = runnable_global;
            runqueue["per_p"] = std::move(per_processor);
            data["goroutines"] = std::move(goroutines);
            data["runqueue"] = std::move(runqueue);
            return "{\"id\":" + std::to_string(id) +
                   ",\"ok\":true,\"data\":" + data.dump() + "}\n";
        }
        if (cmd == "dump") {
            std::string component = "all";
            if (request.contains("sections") &&
                request["sections"].is_array()) {
                for (const auto& section : request["sections"]) {
                    if (!section.is_string()) {
                        continue;
                    }
                    const std::string mapped =
                        MapSectionToComponent(section.get<std::string>());
                    if (!mapped.empty()) {
                        component = mapped;
                        break;
                    }
                }
            }
            const std::string format =
                request.value("format", std::string("text"));
            const DumpPayload payload = Dump(component);
            std::string data;
            if (format == "json") {
                data = payload.available
                           ? payload.json
                           : "{\"available\":false,\"reason\":\"" +
                                 JsonEscape(payload.reason) + "\"}";
            } else {
                data = "{\"format\":\"text\",\"component\":\"" +
                       JsonEscape(component) + "\",\"text\":\"" +
                       JsonEscape(payload.text) + "\"}";
            }
            return "{\"id\":" + std::to_string(id) +
                   ",\"ok\":true,\"data\":" + data + "}\n";
        }
        if (cmd == "migrate.dump") {
            return fail("not_supported",
                        "migrate.dump backend is not delivered by this runtime");
        }
        return fail("unknown_cmd", cmd.empty() ? "missing cmd" : cmd);
    } catch (...) {
        return fail("internal", "json-handler-exception");
    }
}

// ---------------------------------------------------------------------------
// 便捷注册
// ---------------------------------------------------------------------------

bool RegisterScheduler(const std::string& name, Scheduler& scheduler) {
    if (name.empty()) {
        return false;
    }
    return DebugServer::Instance().Register(
        name, "scheduler M/P/G snapshots and runqueue lengths",
        [&scheduler]() {
            DumpOptions options;
            options.include_fibers = true;
            return DumpScheduler(scheduler, options);
        });
}

bool RegisterIOManager(const std::string& name, io::IOManager& manager) {
    if (name.empty()) {
        return false;
    }
    return DebugServer::Instance().Register(
        name, "IOManager fd slots, waiters, epoll registrations",
        [&manager]() { return DumpIOManager(manager); });
}

bool RegisterMutex(const std::string& name, sync::Mutex& mutex) {
    if (name.empty()) {
        return false;
    }
    return DebugServer::Instance().Register(
        name, "sync::Mutex owner/waiters/state bits",
        [&mutex]() { return DumpMutex(mutex); });
}

bool RegisterConditionVariable(const std::string& name,
                               sync::ConditionVariable& condition) {
    if (name.empty()) {
        return false;
    }
    return DebugServer::Instance().Register(
        name, "sync::ConditionVariable waiters",
        [&condition]() { return DumpConditionVariable(condition); });
}

bool RegisterParkingCondition(const std::string& name,
                              core::ParkingCondition& condition) {
    if (name.empty()) {
        return false;
    }
    return DebugServer::Instance().Register(
        name, "ParkingCondition waiters and node states",
        [&condition]() { return DumpParkingCondition(condition); });
}

bool RegisterFiber(const std::string& name, Fiber& fiber) {
    if (name.empty()) {
        return false;
    }
    return DebugServer::Instance().Register(
        name, "Fiber state, parent, stack range and committed bytes",
        [&fiber]() { return DumpFiber(fiber); });
}

bool RegisterEventRing(const std::string& name) {
    if (name.empty()) {
        return false;
    }
    return DebugServer::Instance().Register(
        name, "scheduler park/wake/runqueue event ring",
        []() { return DumpEvents(0); });
}


// ===========================================================================
// HTTP 探针端点（GO2CPP-DEBUG-HTTP/1）
// ===========================================================================
//
// 设计取舍（见 docs/reports/debug-mcp-integration.md）：
// - 复用 DebugServer 已有的组件注册表与 Dump()，不新增观测代码；
// - 单线程 accept + 每连接独立处理：调试查询低频，避免线程池与背压问题；
// - 每个连接有 1s 读写 deadline，客户端断连不会拖住 accept 循环；
// - 全部端点只读：不触发调度器/锁/IO 状态变化。

#if defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP

namespace {

/// 默认端口落在 39xxx 段，避开 MCP 监控 Manager 的 9101。
constexpr std::uint16_t kDefaultDebugHttpPort = 39101;
constexpr std::size_t kMaxHttpRequestBytes = 8192;
constexpr std::size_t kMaxHttpResponseBytes = 8ULL * 1024ULL * 1024ULL;
constexpr int kHttpIoTimeoutMs = 1000;

std::string HttpQueryValue(const std::string& query, const std::string& key) {
    std::size_t begin = 0;
    while (begin <= query.size()) {
        const std::size_t end = query.find('&', begin);
        const std::string pair = query.substr(
            begin, end == std::string::npos ? std::string::npos : end - begin);
        const std::size_t equal = pair.find('=');
        if (equal != std::string::npos && pair.substr(0, equal) == key) {
            return pair.substr(equal + 1);
        }
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1;
    }
    return {};
}

std::size_t HttpQuerySize(const std::string& query, const std::string& key,
                          std::size_t fallback, std::size_t maximum) {
    const std::string text = HttpQueryValue(query, key);
    if (text.empty()) {
        return fallback;
    }
    try {
        std::size_t consumed = 0;
        const unsigned long long value = std::stoull(text, &consumed);
        if (consumed != text.size()) {
            return fallback;
        }
        return static_cast<std::size_t>(std::min<unsigned long long>(
            value == 0 ? 1ULL : value, static_cast<unsigned long long>(maximum)));
    } catch (...) {
        return fallback;
    }
}

Json ParseJsonOrNull(const std::string& text) {
    const JsonParseResult parsed = ParseJson(text);
    if (!parsed.ok()) {
        return Json();
    }
    return parsed.value;
}

std::string ComponentSchema(const Json& value) {
    if (!value.is_object() || !value.contains("schema") ||
        !value["schema"].is_string()) {
        return {};
    }
    return value["schema"].get<std::string>();
}

bool SchemaMentions(const Json& value, const char* needle) {
    return ComponentSchema(value).find(needle) != std::string::npos;
}

/// 资源注册表按 kind 的存活计数（只读；不保存对象指针）。
Json ResourceKindCounts() {
    Json counts = Json::object();
    const std::vector<ResourceInfo> resources = SnapshotResources();
    for (const ResourceInfo& info : resources) {
        if (!info.alive) {
            continue;
        }
        const char* name = ResourceKindName(info.kind);
        const std::string key = name == nullptr ? "kUnknown" : name;
        counts[key] = counts.value(key, 0U) + 1U;
    }
    return counts;
}

/// 读 /proc/self 的 RSS/VSZ/线程/fd；失败字段留空（不编造）。
Json ProcessStats() {
    Json out = Json::object();
    std::ifstream status("/proc/self/status");
    std::string line;
    const auto take_kb = [&out](const std::string& text, const char* prefix,
                                const char* field) {
        if (text.rfind(prefix, 0) != 0) {
            return;
        }
        const std::string rest = text.substr(std::strlen(prefix));
        try {
            out[field] = std::stoull(rest);
        } catch (...) {
        }
    };
    while (std::getline(status, line)) {
        take_kb(line, "VmRSS:", "vm_rss_kb");
        take_kb(line, "VmSize:", "vm_size_kb");
        take_kb(line, "VmHWM:", "vm_hwm_kb");
        take_kb(line, "Threads:", "threads");
    }
    std::size_t fd_count = 0;
    if (DIR* dir = ::opendir("/proc/self/fd")) {
        while (::readdir(dir) != nullptr) {
            ++fd_count;
        }
        ::closedir(dir);
        // 去掉 "." 与 ".." 两个目录项。
        fd_count = fd_count >= 2 ? fd_count - 2 : 0;
    }
    out["fd_count"] = fd_count;
    rlimit limit{};
    if (::getrlimit(RLIMIT_NOFILE, &limit) == 0) {
        out["fd_limit"] = static_cast<std::uint64_t>(limit.rlim_cur);
    }
    return out;
}

/// 读满一个 HTTP 请求头（最多 kMaxHttpRequestBytes / 1s）。
std::string ReadHttpRequest(int fd) {
    std::string data;
    const std::uint64_t deadline =
        NowNs() + static_cast<std::uint64_t>(kHttpIoTimeoutMs) * 1000000ULL;
    while (data.size() < kMaxHttpRequestBytes) {
        if (data.find("\r\n\r\n") != std::string::npos) {
            break;
        }
        if (NowNs() >= deadline) {
            break;
        }
        pollfd descriptor{};
        descriptor.fd = fd;
        descriptor.events = POLLIN;
        if (::poll(&descriptor, 1, 100) <= 0) {
            continue;
        }
        char buffer[2048];
        const ssize_t received = ::recv(fd, buffer, sizeof(buffer), 0);
        if (received <= 0) {
            break;
        }
        data.append(buffer, static_cast<std::size_t>(received));
    }
    return data;
}

/// 发送一个最小 HTTP/1.1 响应；head_only 时只发头。
bool SendHttpResponse(int fd, int status, const char* reason,
                      const std::string& body, bool head_only) {
    std::string response = "HTTP/1.1 " + std::to_string(status) + " " + reason +
                           "\r\nContent-Type: application/json; charset=utf-8\r\n"
                           "Cache-Control: no-store\r\n"
                           "Content-Length: " + std::to_string(body.size()) +
                           "\r\nConnection: close\r\n\r\n";
    if (!head_only) {
        response += body;
    }
    std::size_t sent = 0;
    while (sent < response.size()) {
        const ssize_t written =
            ::send(fd, response.data() + sent, response.size() - sent,
                   MSG_NOSIGNAL);
        if (written <= 0) {
            return false;
        }
        sent += static_cast<std::size_t>(written);
    }
    return true;
}

bool KnownHttpEndpoint(const std::string& path) {
    static const char* const kEndpoints[] = {
        "/debug/fibers", "/debug/scheduler", "/debug/sync",
        "/debug/resources", "/debug/events", "/debug/dump",
    };
    for (const char* endpoint : kEndpoints) {
        if (path == endpoint) {
            return true;
        }
    }
    return false;
}

}  // namespace

std::uint16_t DebugServer::DefaultHttpPort() {
    const char* env = std::getenv("GO2CPP_DEBUG_HTTP_PORT");
    if (env == nullptr || *env == '\0') {
        return kDefaultDebugHttpPort;
    }
    try {
        std::size_t consumed = 0;
        const long value = std::stol(env, &consumed);
        if (consumed != std::strlen(env)) {
            return kDefaultDebugHttpPort;
        }
        if (value == 0) {
            return 0;  // 显式禁用 HTTP 探针
        }
        if (value > 0 && value <= 65535) {
            return static_cast<std::uint16_t>(value);
        }
    } catch (...) {
    }
    return kDefaultDebugHttpPort;
}

std::string DebugServer::http_last_error() const {
    std::lock_guard<std::mutex> lock(m_http_error_mutex);
    return m_http_last_error;
}

bool DebugServer::StartHttp() {
    if (http_running()) {
        return true;
    }
    const std::uint16_t port = DefaultHttpPort();
    if (port == 0) {
        std::lock_guard<std::mutex> lock(m_http_error_mutex);
        m_http_last_error = "http-disabled";
        return false;
    }
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        std::lock_guard<std::mutex> lock(m_http_error_mutex);
        m_http_last_error = std::string("http-socket: ") + std::strerror(errno);
        return false;
    }
    int reuse = 1;
    (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(fd, reinterpret_cast<const sockaddr*>(&address),
               sizeof(address)) != 0) {
        const int saved = errno;
        ::close(fd);
        std::lock_guard<std::mutex> lock(m_http_error_mutex);
        m_http_last_error = std::string("http-bind: ") + std::strerror(saved);
        return false;
    }
    if (::listen(fd, 8) != 0) {
        const int saved = errno;
        ::close(fd);
        std::lock_guard<std::mutex> lock(m_http_error_mutex);
        m_http_last_error = std::string("http-listen: ") + std::strerror(saved);
        return false;
    }
    m_http_listen_fd = fd;
    m_http_port.store(port, std::memory_order_release);
    m_http_stop.store(false, std::memory_order_release);
    m_http_running.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(m_http_error_mutex);
        m_http_last_error.clear();
    }
    try {
        m_http_thread = std::thread([this] { HttpRun(); });
    } catch (...) {
        m_http_running.store(false, std::memory_order_release);
        m_http_port.store(0, std::memory_order_release);
        ::close(fd);
        m_http_listen_fd = -1;
        std::lock_guard<std::mutex> lock(m_http_error_mutex);
        m_http_last_error = "http-thread-start-failed";
        return false;
    }
    return true;
}

void DebugServer::StopHttp() {
    m_http_stop.store(true, std::memory_order_release);
    if (m_http_thread.joinable()) {
        m_http_thread.join();
    }
    m_http_running.store(false, std::memory_order_release);
    m_http_port.store(0, std::memory_order_release);
    if (m_http_listen_fd >= 0) {
        ::close(m_http_listen_fd);
        m_http_listen_fd = -1;
    }
}

void DebugServer::HttpRun() noexcept {
    while (!m_http_stop.load(std::memory_order_acquire)) {
        pollfd descriptor{};
        descriptor.fd = m_http_listen_fd;
        descriptor.events = POLLIN;
        if (::poll(&descriptor, 1, 100) <= 0) {
            continue;
        }
        const int client = ::accept(m_http_listen_fd, nullptr, nullptr);
        if (client < 0) {
            continue;
        }
        timeval timeout{};
        timeout.tv_sec = 1;
        (void)::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                           sizeof(timeout));
        (void)::setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout,
                           sizeof(timeout));
        HttpHandleClient(client);
        ::close(client);
    }
}

void DebugServer::HttpHandleClient(int client_fd) noexcept {
    const std::string request = ReadHttpRequest(client_fd);
    const std::size_t line_end = request.find("\r\n");
    const std::string first_line =
        line_end == std::string::npos ? request : request.substr(0, line_end);
    const std::size_t first_space = first_line.find(' ');
    const std::size_t second_space = first_space == std::string::npos
                                         ? std::string::npos
                                         : first_line.find(' ', first_space + 1);
    if (first_space == std::string::npos || second_space == std::string::npos) {
        (void)SendHttpResponse(client_fd, 400, "Bad Request",
                               R"({"ok":false,"error":"bad-request"})", false);
        return;
    }
    const std::string method = first_line.substr(0, first_space);
    const std::string target =
        first_line.substr(first_space + 1, second_space - first_space - 1);
    const bool head_only = method == "HEAD";
    if (method != "GET" && method != "HEAD") {
        (void)SendHttpResponse(client_fd, 405, "Method Not Allowed",
                               R"({"ok":false,"error":"method-not-allowed"})",
                               false);
        return;
    }
    if (target.empty() || target[0] != '/') {
        (void)SendHttpResponse(client_fd, 400, "Bad Request",
                               R"({"ok":false,"error":"bad-target"})", false);
        return;
    }
    const std::size_t path_end = target.find('?');
    const std::string path = target.substr(0, path_end);
    if (!KnownHttpEndpoint(path)) {
        const std::string not_found =
            std::string(R"({"ok":false,"error":"unknown-endpoint","endpoints":[)")
            + R"("/debug/fibers","/debug/scheduler","/debug/sync",)"
            + R"("/debug/resources","/debug/events","/debug/dump"]})";
        (void)SendHttpResponse(client_fd, 404, "Not Found", not_found,
                               head_only);
        return;
    }
    const std::string body = HandleHttpRequest(target);
    (void)SendHttpResponse(client_fd, 200, "OK", body, head_only);
}

std::string DebugServer::HandleHttpRequest(const std::string& target) const {
    const std::size_t question = target.find('?');
    const std::string path = target.substr(0, question);
    const std::string query =
        question == std::string::npos ? std::string() : target.substr(question + 1);
    const std::uint64_t now = NowNs();

    Json out = Json::object();
    out["version"] = 1;
    out["pid"] = static_cast<std::int64_t>(::getpid());
    out["steady_ns"] = now;
    out["uptime_ns"] = m_started_ns == 0U ? 0U : now - m_started_ns;
    out["endpoint"] = path;

    if (path == "/debug/fibers") {
        out["schema"] = "go2cpp.debug.fibers/1";
        const auto entries = DebugRegistry::Instance().Snapshot();
        Json fibers = Json::array();
        for (const auto& entry : entries) {
            Json item = Json::object();
            item["id"] = entry.first;
            item["name"] = entry.second.name;
            item["description"] = entry.second.description;
            item["file"] = entry.second.file;
            item["line"] = entry.second.line;
            item["present"] = entry.second.present;
            fibers.push_back(std::move(item));
        }
        std::size_t fiber_count = entries.size();
        Json components = Json::object();
        Json scheduler_summary = Json::object();
        for (const std::string& name : Components()) {
            const DumpPayload payload = Dump(name);
            if (!payload.available) {
                continue;
            }
            const Json value = ParseJsonOrNull(payload.json);
            if (value.is_null()) {
                continue;
            }
            if (!SchemaMentions(value, "scheduler") &&
                !SchemaMentions(value, "fiber")) {
                continue;
            }
            components[name] = value;
            if (value.contains("tasks") && value["tasks"].is_array()) {
                fiber_count = std::max(fiber_count, value["tasks"].size());
            }
            if (SchemaMentions(value, "scheduler")) {
                scheduler_summary = value;
            }
        }
        out["ok"] = true;
        out["fiber_count"] = fiber_count;
        out["registry_count"] = entries.size();
        out["fibers"] = std::move(fibers);
        out["scheduler_summary"] = std::move(scheduler_summary);
        out["components"] = std::move(components);
        if (fiber_count == 0) {
            out["hint"] =
                "没有可观测 fiber：业务需创建 Fiber/DebugFiber 并注册 "
                "DebugRegistry，或用 RegisterScheduler 注册 scheduler 组件";
        }
        return out.dump(2);
    }

    if (path == "/debug/scheduler") {
        out["schema"] = "go2cpp.debug.scheduler/1";
        Json components = Json::object();
        Json primary = Json::object();
        for (const std::string& name : Components()) {
            const DumpPayload payload = Dump(name);
            if (!payload.available) {
                continue;
            }
            const Json value = ParseJsonOrNull(payload.json);
            if (value.is_null() || !SchemaMentions(value, "scheduler")) {
                continue;
            }
            components[name] = value;
            if (primary.empty()) {
                primary = value;
            }
        }
        const bool has_components = !components.empty();
        out["ok"] = true;
        out["scheduler"] = std::move(primary);
        out["components"] = std::move(components);
        out["available"] = has_components;
        if (!has_components) {
            out["hint"] =
                "未注册 scheduler 组件：进程内调用 "
                "debug::RegisterScheduler(\"scheduler\", scheduler)";
        }
        return out.dump(2);
    }

    if (path == "/debug/sync") {
        out["schema"] = "go2cpp.debug.sync/1";
        Json components = Json::object();
        std::size_t primitives = 0;
        std::uint64_t total_waiters = 0;
        for (const std::string& name : Components()) {
            const DumpPayload payload = Dump(name);
            if (!payload.available) {
                continue;
            }
            const Json value = ParseJsonOrNull(payload.json);
            if (value.is_null()) {
                continue;
            }
            if (!SchemaMentions(value, "mutex") &&
                !SchemaMentions(value, "condition") &&
                !SchemaMentions(value, "parking") &&
                !SchemaMentions(value, "waitgroup")) {
                continue;
            }
            ++primitives;
            if (value.contains("waiters") && value["waiters"].is_number_unsigned()) {
                total_waiters += value["waiters"].get<std::uint64_t>();
            }
            components[name] = value;
        }
        out["ok"] = true;
        out["primitives"] = primitives;
        out["total_waiters"] = total_waiters;
        out["components"] = std::move(components);
        out["resources"] = ResourceKindCounts();
        out["resource_alive"] = ResourceAliveCount();
        if (primitives == 0) {
            out["hint"] =
                "未注册同步原语组件：用 debug::RegisterMutex / "
                "RegisterConditionVariable / RegisterParkingCondition 注册";
        }
        // 慢查询保护：只读资源注册表，不等待任何锁；本端点不会阻塞同步原语。
        return out.dump(2);
    }

    if (path == "/debug/resources") {
        out["schema"] = "go2cpp.debug.resources/1";
        out["process"] = ProcessStats();
        Json registry = Json::object();
        registry["alive_count"] = ResourceAliveCount();
        registry["by_kind"] = ResourceKindCounts();
        registry["stack_samples"] = StackSampleCount();
        registry["fiber_chain_samples"] = FiberChainCount();
        out["resources"] = std::move(registry);
        Json ring = Json::object();
        ring["enabled"] = EventRing::Enabled();
        ring["threads"] = EventRing::ThreadCount();
        ring["dropped_threads"] = static_cast<std::uint64_t>(EventRing::DroppedThreads());
        ring["published_events"] = EventRing::PublishedEvents();
        out["event_ring"] = std::move(ring);
        out["ok"] = true;
        return out.dump(2);
    }

    if (path == "/debug/events") {
        out["schema"] = "go2cpp.debug.events/1";
        const std::size_t limit = HttpQuerySize(query, "limit", 64, 1024);
        const Json events = ParseJsonOrNull(EventRing::DumpJson(limit));
        out["ok"] = true;
        out["limit"] = limit;
        out["enabled"] = EventRing::Enabled();
        out["published_events"] = EventRing::PublishedEvents();
        out["events"] = events.is_null() ? Json::object() : events;
        return out.dump(2);
    }

    if (path == "/debug/dump") {
        out["schema"] = "go2cpp.debug.dump/1";
        const DumpPayload payload = Dump("all");
        const Json data = ParseJsonOrNull(payload.json);
        out["ok"] = !data.is_null();
        if (data.is_null()) {
            out["error"] = "dump-json-invalid";
            out["text"] = payload.text;
        } else {
            out["dump"] = data;
        }
        if (HttpQueryValue(query, "format") == "text") {
            out["text"] = payload.text;
        }
        return out.dump(2);
    }

    out["ok"] = false;
    out["error"] = "unknown-endpoint";
    return out.dump(2);
}

#else  // GO2CPP_DEBUG_DUMP=OFF

std::uint16_t DebugServer::DefaultHttpPort() {
    const char* env = std::getenv("GO2CPP_DEBUG_HTTP_PORT");
    if (env == nullptr || *env == '\0') {
        return 39101;
    }
    try {
        const long value = std::stol(env);
        if (value >= 0 && value <= 65535) {
            return static_cast<std::uint16_t>(value);
        }
    } catch (...) {
    }
    return 39101;
}

std::string DebugServer::http_last_error() const { return "http-disabled"; }

bool DebugServer::StartHttp() { return false; }
void DebugServer::StopHttp() {}
void DebugServer::HttpRun() noexcept {}
void DebugServer::HttpHandleClient(int) noexcept {}

std::string DebugServer::HandleHttpRequest(const std::string&) const {
    return R"({"ok":false,"error":"debug-dump-disabled"})";
}

#endif  // GO2CPP_DEBUG_DUMP


}  // namespace go2cpp::debug
