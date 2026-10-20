/**
 * @file doctor.cpp
 * @brief `go2cpp-cli doctor`：Doctor（G2CD/1）客户端。
 *
 * ## 协议与传输
 *
 * - 帧格式见 `include/go2cpp/debug/diagnostic_protocol.hpp`；
 * - 传输：Unix domain socket，默认 `/tmp/go2cpp-<pid>-doctor.sock`；
 * - 每条命令一个短连接（与 DebugServer 的模型一致），响应按 `stream_id`
 *   匹配，支持分块（`chunked`/`last_chunk`）。
 *
 * ## 并发
 *
 * `--concurrency N` 会开 N 条连接、每条一个 `stream_id`，用于验收
 * 「单线程命令平面 + 响应按 id 匹配」。即使传输层并发，目标进程里的 Doctor
 * 执行线程仍然串行（`concurrency_peak` 恒为 1）。
 *
 * ## 退出码
 *
 * 沿用 CLI 的稳定契约（`cli.hpp`）：
 * - 0 成功；1 协议/超时/IO；2 用法；3 连接不上（Doctor 未启用）；
 * - 5 目标返回 error 帧，或命令明确 `verdict=refused`。
 */

#include "doctor.hpp"

#include "cli.hpp"
#include "go2cpp/debug/diagnostic_protocol.hpp"
#include "help.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace go2cpp::cli {
namespace {

using Clock = std::chrono::steady_clock;
namespace protocol = go2cpp::debug;

int RemainingMs(Clock::time_point deadline) {
    const auto now = Clock::now();
    if (now >= deadline) {
        return 0;
    }
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)
            .count();
    return remaining <= 0 ? 1 : static_cast<int>(remaining);
}

int PollFd(int fd, short events, int timeout_ms) {
    pollfd entry{};
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

bool SendAll(int fd, const std::string& bytes, Clock::time_point deadline) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const ssize_t sent =
            ::send(fd, bytes.data() + offset, bytes.size() - offset,
                   MSG_NOSIGNAL);
        if (sent > 0) {
            offset += static_cast<std::size_t>(sent);
            continue;
        }
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            const int remaining = RemainingMs(deadline);
            if (remaining == 0 || PollFd(fd, POLLOUT, remaining) <= 0) {
                return false;
            }
            continue;
        }
        if (sent < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

int Connect(const std::string& path, int timeout_ms, std::string* error) {
    if (path.size() >= sizeof(sockaddr_un::sun_path)) {
        *error = "socket 路径过长（上限 108 字节）";
        return -1;
    }
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        *error = std::string("socket: ") + std::strerror(errno);
        return -1;
    }
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        (void)::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1U);
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&address),
                  sizeof(address)) != 0) {
        if (errno != EINPROGRESS) {
            const int saved = errno;
            ::close(fd);
            *error = (saved == ENOENT)
                         ? "Doctor 未启用：socket 不存在（目标进程需要 "
                           "-DGO2CPP_DIAGNOSTIC=ON 并调用 "
                           "DiagnosticAgent::Start()）"
                         : std::string("connect: ") + std::strerror(saved);
            return -1;
        }
        if (PollFd(fd, POLLOUT, timeout_ms) <= 0) {
            ::close(fd);
            *error = "connect 超时";
            return -1;
        }
        int socket_error = 0;
        socklen_t length = sizeof(socket_error);
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &length) !=
                0 ||
            socket_error != 0) {
            ::close(fd);
            *error = std::string("connect: ") +
                     std::strerror(socket_error == 0 ? errno : socket_error);
            return -1;
        }
    }
    return fd;
}

struct CallResult {
    int exit_code{0};
    std::string payload;
    std::string error;
};

/// 发一条命令并收完整响应（分块归并）。
CallResult Call(const std::string& path, const std::string& command,
                std::uint32_t stream_id, int timeout_ms, bool json) {
    CallResult result;
    std::string connect_error;
    const int fd = Connect(path, timeout_ms, &connect_error);
    if (fd < 0) {
        result.exit_code = static_cast<int>(ExitCode::kUnavailable);
        result.error = connect_error;
        return result;
    }
    const auto deadline =
        Clock::now() + std::chrono::milliseconds(timeout_ms);

    protocol::DiagnosticFrame request;
    request.kind = protocol::DiagnosticFrameKind::kRequest;
    request.stream_id = stream_id;
    request.payload = command;
    if (json && command.find("json=1") == std::string::npos) {
        request.payload += " json=1";
    }
    const std::string bytes = protocol::EncodeDiagnosticFrame(request);
    if (bytes.empty() || !SendAll(fd, bytes, deadline)) {
        ::close(fd);
        result.exit_code = static_cast<int>(ExitCode::kFailure);
        result.error = "发送命令失败";
        return result;
    }

    protocol::DiagnosticFrameParser parser;
    bool complete = false;
    std::string assembled;
    bool was_error = false;
    while (!complete && RemainingMs(deadline) > 0) {
        const int remaining = RemainingMs(deadline);
        const int ready = PollFd(fd, POLLIN, remaining);
        if (ready <= 0) {
            break;
        }
        char buffer[65536];
        const ssize_t received = ::recv(fd, buffer, sizeof(buffer), 0);
        if (received <= 0) {
            break;
        }
        if (!parser.Feed(buffer, static_cast<std::size_t>(received))) {
            result.exit_code = static_cast<int>(ExitCode::kFailure);
            result.error = "帧解析失败：" + parser.error();
            break;
        }
        protocol::DiagnosticFrame frame;
        while (parser.Next(frame)) {
            if (frame.stream_id != stream_id) {
                continue;
            }
            assembled += frame.payload;
            was_error = was_error ||
                        frame.kind == protocol::DiagnosticFrameKind::kError;
            if (!frame.chunked() || frame.last_chunk()) {
                complete = true;
                break;
            }
        }
        if (parser.failed()) {
            result.exit_code = static_cast<int>(ExitCode::kFailure);
            result.error = "帧解析失败：" + parser.error();
            break;
        }
    }
    ::close(fd);
    if (!complete && result.error.empty()) {
        result.exit_code = static_cast<int>(ExitCode::kFailure);
        result.error = "等待响应超时";
        return result;
    }
    result.payload = std::move(assembled);
    if (was_error) {
        result.exit_code = static_cast<int>(ExitCode::kRemote);
    } else if (result.payload.find("verdict=refused") != std::string::npos ||
               result.payload.find("\"verdict\":\"refused\"") !=
                   std::string::npos) {
        result.exit_code = static_cast<int>(ExitCode::kRemote);
    }
    return result;
}

std::string ResolveSocket(const std::string& explicit_path, int pid,
                          bool* ok) {
    *ok = true;
    if (!explicit_path.empty()) {
        return explicit_path;
    }
    if (const char* environment = std::getenv("GO2CPP_DOCTOR_SOCKET")) {
        if (*environment != '\0') {
            return environment;
        }
    }
    if (pid > 0) {
        return "/tmp/go2cpp-" + std::to_string(pid) + "-doctor.sock";
    }
    *ok = false;
    return {};
}

int ParseInt(const std::string& text, int minimum, int maximum,
             const char* name, bool* ok) {
    try {
        const long long value = std::stoll(text);
        if (value < minimum || value > maximum) {
            *ok = false;
            return 0;
        }
        *ok = true;
        return static_cast<int>(value);
    } catch (...) {
        *ok = false;
        std::fprintf(stderr, "go2cpp-cli: %s 需要整数: %s\n", name,
                     text.c_str());
        return 0;
    }
}

int RunConcurrent(const std::string& socket_path, const std::string& command,
                  int concurrency, int timeout_ms, bool json) {
    std::atomic<int> failures{0};
    std::mutex output_mutex;
    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(concurrency));
    const auto started = Clock::now();
    for (int index = 0; index < concurrency; ++index) {
        threads.emplace_back([&, index] {
            const auto result =
                Call(socket_path, command,
                     static_cast<std::uint32_t>(1000 + index), timeout_ms, json);
            if (result.exit_code != 0) {
                failures.fetch_add(1, std::memory_order_relaxed);
                std::lock_guard<std::mutex> lock(output_mutex);
                std::fprintf(stderr, "FAIL stream=%d: %s\n", 1000 + index,
                             result.error.empty() ? result.payload.c_str()
                                                  : result.error.c_str());
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             Clock::now() - started)
                             .count();
    if (failures.load() != 0) {
        std::printf("FAIL concurrency=%d failures=%d\n", concurrency,
                    failures.load());
        return static_cast<int>(ExitCode::kFailure);
    }
    std::printf("PASS concurrency=%d elapsed_ms=%lld\n", concurrency,
                static_cast<long long>(elapsed));
    return 0;
}

}  // namespace

int RunDoctor(const std::vector<std::string>& args) {
    std::string socket_path;
    int pid = 0;
    int timeout_ms = 5000;
    int concurrency = 0;
    bool json = false;
    std::vector<std::string> command_parts;
    for (std::size_t index = 0; index < args.size(); ++index) {
        const std::string& argument = args[index];
        if (argument == "--help" || argument == "-h") {
            PrintDoctorHelp();
            return 0;
        }
        if (argument == "--json") {
            json = true;
            continue;
        }
        if (argument == "--command") {
            if (index + 1 >= args.size()) {
                std::fprintf(stderr, "go2cpp-cli: --command 缺少参数\n");
                return static_cast<int>(ExitCode::kUsage);
            }
            command_parts.clear();
            command_parts.push_back(args[++index]);
            continue;
        }
        if (argument == "--transport") {
            if (index + 1 >= args.size()) {
                std::fprintf(stderr, "go2cpp-cli: --transport 缺少参数\n");
                return static_cast<int>(ExitCode::kUsage);
            }
            const std::string transport = args[++index];
            if (transport == "quic") {
                // 协议（stream_id/分块）已定稿，picoquic 适配尚未实现；
                // 明确报错，绝不静默退回 Unix socket。
                std::fprintf(
                    stderr,
                    "go2cpp-cli: doctor: QUIC transport 尚未实现（协议与 "
                    "stream 映射见 docs/diagnostic-channel.md §9；构建需要 "
                    "-DGO2CPP_DIAGNOSTIC_QUIC=ON 与 picoquic）\n");
                return static_cast<int>(ExitCode::kUnavailable);
            }
            if (transport != "unix") {
                std::fprintf(stderr,
                             "go2cpp-cli: 未知 transport '%s'（unix|quic）\n",
                             transport.c_str());
                return static_cast<int>(ExitCode::kUsage);
            }
            continue;
        }
        if (argument == "--pid" || argument == "--socket" ||
            argument == "--timeout-ms" || argument == "--concurrency") {
            if (index + 1 >= args.size()) {
                std::fprintf(stderr, "go2cpp-cli: %s 缺少参数\n",
                             argument.c_str());
                return static_cast<int>(ExitCode::kUsage);
            }
            bool ok = false;
            if (argument == "--socket") {
                socket_path = args[++index];
                ok = true;
            } else if (argument == "--pid") {
                pid = ParseInt(args[++index], 1, 2147483647, "--pid", &ok);
            } else if (argument == "--timeout-ms") {
                timeout_ms =
                    ParseInt(args[++index], 1, 600000, "--timeout-ms", &ok);
            } else {
                concurrency =
                    ParseInt(args[++index], 1, 256, "--concurrency", &ok);
            }
            if (!ok) {
                return static_cast<int>(ExitCode::kUsage);
            }
            continue;
        }
        if (StartsWith(argument, "--")) {
            std::fprintf(stderr, "go2cpp-cli: 未知选项 %s\n", argument.c_str());
            return static_cast<int>(ExitCode::kUsage);
        }
        command_parts.push_back(argument);
    }

    bool socket_ok = true;
    const std::string resolved = ResolveSocket(socket_path, pid, &socket_ok);
    if (!socket_ok) {
        std::fprintf(stderr,
                     "go2cpp-cli: doctor 需要 --socket 或 --pid（或环境变量 "
                     "GO2CPP_DOCTOR_SOCKET）\n");
        return static_cast<int>(ExitCode::kUsage);
    }

    std::string command;
    for (const auto& part : command_parts) {
        if (!command.empty()) {
            command.push_back(' ');
        }
        command += part;
    }
    if (command.empty()) {
        command = "status";
    }

    if (concurrency > 0) {
        return RunConcurrent(resolved, command, concurrency, timeout_ms, json);
    }
    const CallResult result = Call(resolved, command, 1, timeout_ms, json);
    if (!result.payload.empty()) {
        std::fputs(result.payload.c_str(), stdout);
        if (result.payload.back() != '\n') {
            std::fputc('\n', stdout);
        }
    }
    if (!result.error.empty()) {
        std::fprintf(stderr, "go2cpp-cli: doctor: %s\n", result.error.c_str());
    }
    return result.exit_code;
}

}  // namespace go2cpp::cli
