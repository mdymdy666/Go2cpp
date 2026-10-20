/**
 * @file transport.cpp
 * @brief `transport.hpp` 的实现：非阻塞 connect + poll 超时 + `GO2CPP-DEBUG/1` 编解码。
 *
 * @details 所有阻塞点共享同一个 deadline（连接、发送命令行、读头行、读 payload），
 *          任意阶段超时都给出「哪一步超时」的明确文案。目标进程可能被 SIGSTOP、
 *          可能 bind 了但未 accept —— 没有超时的调试工具在 CI 里就是挂死源。
 *
 * ## 为什么每次 Call 新建连接
 *
 * 服务端 `HandleClient` 读一行、响应一次即关闭。复用 fd 会在第二次请求时读到
 * EOF，错误信息会变成「服务端关闭连接」而不是真正的业务错误。因此客户端按
 * 服务端契约使用短连接。
 *
 * ## 边界与陷阱
 *
 * - `connect` 返回 `EINPROGRESS` 后必须用 `getsockopt(SO_ERROR)` 取真实错误；
 *   直接看 poll 的 revents 会把「连接被拒」误报成「可写」。
 * - `MSG_NOSIGNAL` 防止对端关闭时 SIGPIPE（`main` 另有全局忽略）。
 * - 头行的 `payload_bytes` 必须是纯十进制数字且 ≤ 16 MiB；不做「读多少算多少」
 *   的降级，否则截断的 payload 会被当成合法 JSON 解析，错误位置极难定位。
 * - payload 比声明的短（对端提前关闭）直接报错，并带上已读/期望字节数。
 */

#include "transport.hpp"

#include "cli.hpp"

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#if !defined(__linux__) && !defined(__APPLE__)
#error "go2cpp-cli 的 DebugServer 传输层只支持 POSIX（Linux/macOS）"
#endif

namespace go2cpp::cli {
namespace {

constexpr std::size_t kMaxPayloadBytes = 16ULL * 1024ULL * 1024ULL;
constexpr std::size_t kMaxHeadLineBytes = 4096;

using Clock = std::chrono::steady_clock;

int RemainingMs(Clock::time_point deadline) {
    const auto now = Clock::now();
    if (now >= deadline) {
        return 0;
    }
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
    return remaining <= 0 ? 1 : static_cast<int>(remaining);
}

/// 返回 poll 结果：>0 就绪，0 超时，-1 错误（EINTR 已重试）。
int PollOne(int fd, short events, int timeout_ms) {
    struct pollfd entry {};
    entry.fd = fd;
    entry.events = events;
    for (;;) {
        const int result = ::poll(&entry, 1, timeout_ms);
        if (result < 0 && errno == EINTR) {
            continue;
        }
        return result;
    }
}

bool TraceEnabled() {
    const char* value = std::getenv("GO2CPP_CLI_TRACE");
    return value != nullptr && *value != '\0' && std::strcmp(value, "0") != 0;
}

std::string TruncateForError(const std::string& text, std::size_t limit = 160) {
    std::string visible;
    visible.reserve(limit);
    for (char character : text) {
        if (visible.size() >= limit) {
            visible += "...";
            break;
        }
        if (character == '\n') {
            visible += "\\n";
        } else if (character == '\r') {
            visible += "\\r";
        } else {
            visible.push_back(character);
        }
    }
    return visible;
}

[[noreturn]] void ThrowConnectError(int error_code, const DebugTarget& target) {
    const std::string& path = target.socket_path;
    switch (error_code) {
        case ENOENT:
            throw CliError(
                ExitCode::kUnavailable,
                "DebugServer 未启用：socket '" + path + "' 不存在。\n"
                "  目标进程需要以 GO2CPP_DEBUG_DUMP=ON 构建，并在启动后调用\n"
                "  go2cpp::debug::DebugServer::Instance().Start()（或以 DebugServer::DefaultPath()\n"
                "  指定的路径启动；CLI 也可用 GO2CPP_DEBUG_SOCKET 覆盖）。");
        case ECONNREFUSED:
            throw CliError(ExitCode::kUnavailable,
                           "无法连接 DebugServer '" + path + "'：连接被拒绝（目标进程 " +
                               std::to_string(target.pid) +
                               " 可能已退出，或 DebugServer 已停止监听）。");
        case EACCES:
        case EPERM:
            throw CliError(ExitCode::kUnavailable,
                           "无权限连接 DebugServer '" + path + "'：" + std::strerror(error_code) +
                               "（服务端通常把 socket 设为 0600，需以同一用户运行 CLI）");
        case ETIMEDOUT:
            throw CliError(ExitCode::kUnavailable,
                           "连接 DebugServer 超时（" + std::to_string(target.timeout_ms) +
                               "ms）：'" + path + "'");
        default:
            throw CliError(ExitCode::kUnavailable,
                           "无法连接 DebugServer '" + path + "'：" + std::strerror(error_code));
    }
}

}  // namespace

std::string DefaultSocketPathForPid(int pid) {
    return "/tmp/go2cpp-" + std::to_string(pid) + ".sock";
}

DebugTarget ResolveDebugTarget(int pid, int timeout_ms) {
    DebugTarget target;
    target.pid = pid;
    target.timeout_ms = timeout_ms;

    const char* socket_override = std::getenv("GO2CPP_DEBUG_SOCKET");
    if (socket_override != nullptr && *socket_override != '\0') {
        target.socket_path = socket_override;
    } else {
        const std::string proc_entry = "/proc/" + std::to_string(pid);
        if (::access(proc_entry.c_str(), F_OK) != 0) {
            throw CliError(ExitCode::kUnavailable,
                           "进程 " + std::to_string(pid) + " 不存在（" + proc_entry +
                               " 不可访问）；请确认 --pid 指向正在运行的 Go2Cpp 程序");
        }
        target.socket_path = DefaultSocketPathForPid(pid);
    }

    if (target.socket_path.size() >= sizeof(sockaddr_un::sun_path)) {
        throw CliError(ExitCode::kFailure,
                       "socket 路径过长（" + std::to_string(target.socket_path.size()) +
                           " 字节，上限 " +
                           std::to_string(sizeof(sockaddr_un::sun_path) - 1) + "）：'" +
                           target.socket_path + "'");
    }
    return target;
}

DebugError::DebugError(std::string remote_code, const std::string& message)
    : CliError(ExitCode::kRemote,
               "DebugServer 返回错误 [" + remote_code + "]：" + message),
      m_remote_code(std::move(remote_code)) {}

DebugClient::DebugClient(DebugTarget target) : m_target(std::move(target)) {}

DebugClient::~DebugClient() = default;

DebugReply DebugClient::Call(const std::string& command) {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        throw CliError(ExitCode::kFailure,
                       "创建 Unix socket 失败：" + std::string(std::strerror(errno)));
    }

    const auto close_fd = [fd]() { ::close(fd); };

    struct sockaddr_un address {};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, m_target.socket_path.data(), m_target.socket_path.size());
    address.sun_path[m_target.socket_path.size()] = '\0';
    const auto address_length =
        static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + m_target.socket_path.size() + 1);

    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        const int saved = errno;
        close_fd();
        throw CliError(ExitCode::kFailure,
                       "设置 socket 非阻塞模式失败：" + std::string(std::strerror(saved)));
    }

    const auto deadline = Clock::now() + std::chrono::milliseconds(m_target.timeout_ms);
    const int connect_result =
        ::connect(fd, reinterpret_cast<const struct sockaddr*>(&address), address_length);
    if (connect_result != 0 && errno != EINPROGRESS) {
        const int saved = errno;
        close_fd();
        ThrowConnectError(saved, m_target);
    }
    if (connect_result != 0) {
        const int poll_result = PollOne(fd, POLLOUT, RemainingMs(deadline));
        if (poll_result == 0) {
            close_fd();
            throw CliError(ExitCode::kUnavailable,
                           "连接 DebugServer 超时（" + std::to_string(m_target.timeout_ms) +
                               "ms）：'" + m_target.socket_path + "'");
        }
        if (poll_result < 0) {
            const int saved = errno;
            close_fd();
            throw CliError(ExitCode::kFailure,
                           "等待连接 DebugServer 时出错：" + std::string(std::strerror(saved)));
        }
        int socket_error = 0;
        socklen_t length = sizeof(socket_error);
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &length) != 0) {
            const int saved = errno;
            close_fd();
            throw CliError(ExitCode::kFailure,
                           "读取 socket 错误状态失败：" + std::string(std::strerror(saved)));
        }
        if (socket_error != 0) {
            close_fd();
            ThrowConnectError(socket_error, m_target);
        }
    }

    // ---- 发送命令行 ----
    const std::string request = command + "\n";
    if (TraceEnabled()) {
        std::fprintf(stderr, "[go2cpp-cli trace] -> %s", request.c_str());
    }
    std::size_t sent = 0;
    while (sent < request.size()) {
        const ssize_t written =
            ::send(fd, request.data() + sent, request.size() - sent, MSG_NOSIGNAL);
        if (written > 0) {
            sent += static_cast<std::size_t>(written);
            continue;
        }
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            const int remaining = RemainingMs(deadline);
            if (remaining <= 0 || PollOne(fd, POLLOUT, remaining) == 0) {
                close_fd();
                throw CliError(ExitCode::kFailure,
                               "发送命令超时（" + std::to_string(m_target.timeout_ms) + "ms）：'" +
                                   m_target.socket_path + "'");
            }
            continue;
        }
        const int saved = errno;
        close_fd();
        throw CliError(ExitCode::kFailure,
                       "发送命令失败：" + std::string(std::strerror(saved)));
    }

    // ---- 读头行（逐字节累计，头行很短）----
    std::string head;
    char chunk[512];
    while (head.find('\n') == std::string::npos) {
        if (head.size() > kMaxHeadLineBytes) {
            close_fd();
            throw CliError(ExitCode::kFailure,
                           "DebugServer 头行超过 " + std::to_string(kMaxHeadLineBytes) +
                               " 字节，协议不匹配；开头: " + TruncateForError(head));
        }
        const ssize_t received = ::recv(fd, chunk, sizeof(chunk), 0);
        if (received > 0) {
            head.append(chunk, static_cast<std::size_t>(received));
            continue;
        }
        if (received == 0) {
            close_fd();
            throw CliError(ExitCode::kFailure,
                           "DebugServer 在返回响应头前关闭了连接：'" + m_target.socket_path + "'");
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            const int remaining = RemainingMs(deadline);
            if (remaining <= 0 || PollOne(fd, POLLIN, remaining) == 0) {
                close_fd();
                throw CliError(ExitCode::kUnavailable,
                               "等待 DebugServer 响应超时（" +
                                   std::to_string(m_target.timeout_ms) + "ms）：'" +
                                   m_target.socket_path + "'");
            }
            continue;
        }
        const int saved = errno;
        close_fd();
        throw CliError(ExitCode::kFailure,
                       "读取响应头失败：" + std::string(std::strerror(saved)));
    }
    const std::size_t newline = head.find('\n');
    const std::string head_line = head.substr(0, newline);
    const std::string buffered = head.substr(newline + 1);

    if (TraceEnabled()) {
        std::fprintf(stderr, "[go2cpp-cli trace] <- %s\n", TruncateForError(head_line, 256).c_str());
    }

    // ---- 解析头行 ----
    const std::string prefix = std::string(kDebugProtocol) + " ";
    if (!StartsWith(head_line, prefix)) {
        close_fd();
        throw CliError(ExitCode::kFailure,
                       "DebugServer 响应头不是 " + std::string(kDebugProtocol) +
                           "；实际: " + TruncateForError(head_line) +
                           "\n  协议可能不匹配（见 docs/cli.md「与 DebugServer 的对接点」）。");
    }
    const std::string rest = head_line.substr(prefix.size());

    if (StartsWith(rest, "ERR ")) {
        const std::string detail = rest.substr(4);
        const std::size_t space = detail.find(' ');
        const std::string remote_code =
            space == std::string::npos ? detail : detail.substr(0, space);
        const std::string message =
            space == std::string::npos ? std::string("(no message)") : detail.substr(space + 1);
        close_fd();
        throw DebugError(remote_code, message);
    }
    if (!StartsWith(rest, "OK ")) {
        close_fd();
        throw CliError(ExitCode::kFailure,
                       "DebugServer 响应头缺少 OK/ERR：'" + TruncateForError(head_line) + "'");
    }

    const std::string size_text = rest.substr(3);
    if (size_text.empty()) {
        close_fd();
        throw CliError(ExitCode::kFailure, "DebugServer 响应头缺少 payload 字节数");
    }
    std::size_t payload_size = 0;
    for (char character : size_text) {
        if (character < '0' || character > '9') {
            close_fd();
            throw CliError(ExitCode::kFailure,
                           "DebugServer 响应头的 payload 字节数非法：'" + size_text + "'");
        }
        payload_size = payload_size * 10 + static_cast<std::size_t>(character - '0');
        if (payload_size > kMaxPayloadBytes) {
            close_fd();
            throw CliError(ExitCode::kFailure,
                           "DebugServer payload 超过上限 " + FormatBytes(kMaxPayloadBytes) +
                               "，已断开");
        }
    }

    // ---- 读 payload（头行后可能已经顺带读到一部分）----
    std::string payload = buffered;
    if (payload.size() > payload_size) {
        payload.resize(payload_size);
    }
    while (payload.size() < payload_size) {
        const ssize_t received = ::recv(fd, chunk, sizeof(chunk), 0);
        if (received > 0) {
            const std::size_t wanted =
                payload_size - payload.size() < static_cast<std::size_t>(received)
                    ? payload_size - payload.size()
                    : static_cast<std::size_t>(received);
            payload.append(chunk, wanted);
            continue;
        }
        if (received == 0) {
            close_fd();
            throw CliError(ExitCode::kFailure,
                           "DebugServer 提前关闭连接：payload 期望 " +
                               std::to_string(payload_size) + " 字节，实际收到 " +
                               std::to_string(payload.size()) + " 字节");
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            const int remaining = RemainingMs(deadline);
            if (remaining <= 0 || PollOne(fd, POLLIN, remaining) == 0) {
                close_fd();
                throw CliError(ExitCode::kUnavailable,
                               "读取 payload 超时（" + std::to_string(m_target.timeout_ms) +
                                   "ms）：已读 " + std::to_string(payload.size()) + "/" +
                                   std::to_string(payload_size) + " 字节");
            }
            continue;
        }
        const int saved = errno;
        close_fd();
        throw CliError(ExitCode::kFailure,
                       "读取 payload 失败：" + std::string(std::strerror(saved)));
    }

    close_fd();
    return DebugReply{std::move(payload)};
}

}  // namespace go2cpp::cli
