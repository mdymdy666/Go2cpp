/**
 * @file debug_server.cpp
 * @brief DebugServer 实现：Unix domain socket + 稳定命令协议。
 */

#include "go2cpp/debug/debug_server.hpp"

#include "go2cpp/json.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <thread>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
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

const char* const kKnownComponents[] = {
    "scheduler", "fiber", "channel", "parking_condition", "mutex",
    "condition_variable", "io", "events", "runtime",
};

std::string Trim(const std::string& input) {
    std::size_t begin = 0;
    while (begin < input.size() &&
           (input[begin] == ' ' || input[begin] == '\t' ||
            input[begin] == '\r' || input[begin] == '\n')) {
        ++begin;
    }
    std::size_t end = input.size();
    while (end > begin &&
           (input[end - 1] == ' ' || input[end - 1] == '\t' ||
            input[end - 1] == '\r' || input[end - 1] == '\n')) {
        --end;
    }
    return input.substr(begin, end - begin);
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
                                    std::uint64_t cursor_start) {
    std::string payload;
    payload += "schema=go2cpp.dump.events.subscription/1\n";
    payload += "version=1\n";
    payload += "topic=";
    payload += options.topic;
    payload += "\n";
    payload += "protocol=GO2CPP-DEBUG-PUSH/1\n";
    payload += "interval_ms=" + std::to_string(options.interval.count()) + "\n";
    payload += "max_events=" + std::to_string(options.max_events) + "\n";
    payload += "cursor_start=" + std::to_string(cursor_start) + "\n";
    payload += "capacity=" + std::to_string(EventRing::kCapacity) + "\n";
    payload += "critical_capacity=" +
               std::to_string(EventRing::kCriticalCapacity) + "\n";
    payload += "consistency=per_call\n";
    return payload;
}

std::string BuildPushPayload(const std::vector<EventRecord>& events,
                             std::uint64_t dropped_gap,
                             std::uint64_t dropped_backpressure,
                             std::uint64_t seq_last) {
    const std::uint64_t first_seq = events.front().seq;
    const std::uint64_t last_seq = events.back().seq;
    const std::uint64_t dropped_total = dropped_gap + dropped_backpressure;
    std::string payload;
    payload += "schema=go2cpp.dump.events.push/1\n";
    payload += "version=1\n";
    payload += "first_seq=" + std::to_string(first_seq) + "\n";
    payload += "last_seq=" + std::to_string(last_seq) + "\n";
    payload += "event_count=" + std::to_string(events.size()) + "\n";
    payload += "dropped_total=" + std::to_string(dropped_total) + "\n";
    payload += "dropped_gap=" + std::to_string(dropped_gap) + "\n";
    payload += "dropped_backpressure=" +
               std::to_string(dropped_backpressure) + "\n";
    payload += "seq_last=" + std::to_string(seq_last) + "\n";
    payload += "cursor=" + std::to_string(last_seq) + "\n";
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

bool IsKnownComponent(const std::string& name) {
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
    return true;
#endif
}

void DebugServer::Stop() {
    if (!running() && m_listen_fd < 0) {
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
    std::uint64_t cursor = EventRing::PublishSeq();
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
    while (!m_stop.load(std::memory_order_acquire)) {
        const std::uint64_t published = EventRing::PublishSeq();
        if (published <= cursor) {
            if (!ClientAlive(fd)) {
                break;
            }
            std::this_thread::sleep_for(options.interval);
            continue;
        }
        std::vector<EventRecord> events =
            EventRing::ReadSince(cursor, options.max_events);
        if (events.empty()) {
            // 发布序号已前进但环形里暂时看不到记录：写入者可能刚拿到 seq、
            // 槽位还没发布。短暂重试；连续多轮为空才按「已被覆盖」跳过。
            if (++idle_rounds < 100) {
                if (!ClientAlive(fd)) {
                    break;
                }
                std::this_thread::sleep_for(options.interval);
                continue;
            }
            dropped_gap_total += published - cursor;
            cursor = published;
            idle_rounds = 0;
            continue;
        }
        idle_rounds = 0;
        if (events.front().seq > cursor + 1) {
            dropped_gap_total += events.front().seq - cursor - 1;
        }
        const std::uint64_t seq_last = events.back().seq;
        const std::string payload = BuildPushPayload(
            events, dropped_gap_total, dropped_backpressure_total, published);
        std::size_t written = 0;
        const SendResult result = SendBounded(fd, PushResponse(payload), &written);
        if (result == SendResult::kFailed) {
            // 帧边界已破坏：关闭连接，让客户端重连（不要留半帧）。
            break;
        }
        if (result == SendResult::kBackpressure) {
            // 背压策略：丢弃最旧的一整批并推进游标，后续帧如实报告。
            dropped_backpressure_total += events.size();
            cursor = seq_last;
            if (!ClientAlive(fd)) {
                break;
            }
            std::this_thread::sleep_for(std::min<std::chrono::milliseconds>(
                options.interval, std::chrono::milliseconds(50)));
            continue;
        }
        cursor = seq_last;
        // 还有积压就立即继续发下一帧；没有则等待新事件。
        if (EventRing::PublishSeq() <= cursor) {
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

}  // namespace go2cpp::debug
