// Copyright 2026 Go2Cpp Authors. All rights reserved.
/**
 * @file 03-partial-body.cpp
 * @brief 真实场景：客户端声明了 Content-Length，却没发满就断连/卡住。
 *
 * ## 场景
 * 手机上传 1 MiB 文件，走到一半进电梯断网（TCP FIN/RST）；或客户端被系统
 * 挂起，连接还在但不再发数据。服务端如果「读到多少算多少」，就会把半截
 * 文件写盘并回 200，用户看到上传成功但文件已损坏。
 *
 * ## 默认行为（file:line）
 * - `http::Parser::feed()` 在 body 未满时返回 `ParseState::NeedMore`
 *   （include/go2cpp/network/http.hpp:213、225-229），**不会**把半截 body
 *   标成 Complete；`feed()` 契约见 http.hpp:255-268；
 * - douyin_http 的 `ReadRequestBody()` 明确：对端提前关闭返回 `kClosed`，
 *   「回 400，**不要**把半个 body 交给 handler」（app/douyin/include/douyin_http.hpp:498-501、507-541）；
 *   超过 30s 总预算返回 `kTimeout`（douyin_http.hpp:500、88）；
 * - 编排层只有 `kOk` 才进 handler（douyin_http.hpp:604-616）：
 *   短 body → 400「请求体不完整」；超时 → 408；
 * - 因此**默认不落盘**：handler 根本没被调用，半截 body 不会变成文件。
 *
 * ## 可选策略
 * 1. **重试**：客户端整体重发（幂等上传可带 Idempotency-Key）；服务端不做
 *    半截文件的后续处理，直接 400 让客户端重来。
 * 2. **续传**：服务端显式开启「临时文件 + 已收偏移」（默认关闭！），客户端用
 *    `Content-Range: bytes 100-1023/1024` 从断点续传；校验总长度/哈希后才
 *    原子 rename 成正式文件。
 * 3. **记录**：把 `path / declared_length / received_bytes / reason` 写审计日志，
 *    用于统计断连率与定位网络问题；**不写业务数据文件**。
 *
 * ## 如何选择
 * - 小文件/表单 → 重试（最简单，服务端保持无状态）；
 * - 大文件/弱网 → 续传（必须配套临时文件、校验、过期清理，成本最高）；
 * - 只想知道断连率 → 记录即可，不要为了「尽量保存」而把半截文件交给业务；
 * - 任何策略都不能改变默认前提：**不完整 = 不提交**。
 *
 * 本示例用真实 UNIX socketpair 模拟「发一半断开」和「发一半卡住」，
 * 并用真实 `http::Parser` 解析；文件落盘与否用真实文件系统断言。
 */
#include "go2cpp/http.hpp"

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

namespace {

using namespace std::chrono_literals;

struct Outcome {
    int status{0};           ///< 0 表示连接异常退出且未写响应。
    bool handler_called{false};
    std::size_t declared{0};
    std::size_t received{0};
    std::string reason;
};

/// @brief 记录 partial 事件（对应「记录」策略；只写日志，不写业务文件）。
struct AuditLog {
    std::uint64_t partial_events{0};

    void Record(const std::string& path, std::size_t declared,
                std::size_t received, const std::string& reason) {
        ++partial_events;
        std::cout << "[记录] path=" << path << " declared=" << declared
                  << " received=" << received << " reason=" << reason
                  << "（审计日志，不落业务盘）\n";
    }
};

/// @brief 服务端写错误响应（400/408），忽略对端已关闭导致的 send 失败。
void WriteError(int fd, int status, std::string_view reason) {
    go2cpp::http::Response response;
    response.status = static_cast<go2cpp::http::Status>(status);
    response.header.set("Content-Type", "text/plain");
    response.body = std::string(reason);
    response.close = true;
    const std::string text = response.to_string();
    std::size_t sent = 0;
    while (sent < text.size()) {
        const ssize_t n = ::send(fd, text.data() + sent, text.size() - sent,
                                 MSG_NOSIGNAL);
        if (n <= 0) {
            return;
        }
        sent += static_cast<std::size_t>(n);
    }
}

/// @brief 服务端：读一个请求；body 不完整时按 EOF/超时分别回 400/408。
/// @param fd 已连接 socket（调用方负责 close）。
/// @param timeout_ms 单请求读取总预算（示例 300ms；douyin_http 默认 30s）。
/// @param file_path 业务落盘路径；只有 Complete 才会创建。
/// @param audit 审计记录器。
Outcome ServeOne(int fd, int timeout_ms, const std::string& file_path,
                 AuditLog* audit) {
    Outcome outcome;
    go2cpp::http::Parser parser;
    go2cpp::http::Request request;
    char buffer[4096];
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    bool eof = false;
    // Parser 在 NeedMore 时不会把半截 body 写进 request.body（http.hpp:225-229），
    // 所以这里自己累计原始字节，减去头部长度，得到「已收 body 字节」。
    std::string raw;
    std::size_t header_bytes = 0;

    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            break;  // 超时：douyin_http 侧对应 kTimeout -> 408。
        }
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        pollfd entry{fd, POLLIN, 0};
        const int ready = ::poll(&entry, 1, static_cast<int>(remaining.count()));
        if (ready == 0) {
            break;
        }
        if (ready < 0) {
            outcome.reason = "poll error";
            return outcome;
        }
        const ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
        if (n == 0) {
            eof = true;  // 对端关闭（客户端断连）。
            break;
        }
        if (n < 0) {
            outcome.reason = "recv error";
            return outcome;
        }
        raw.append(buffer, static_cast<std::size_t>(n));
        if (header_bytes == 0) {
            const std::size_t header_end = raw.find("\r\n\r\n");
            if (header_end != std::string::npos) {
                header_bytes = header_end + 4;
            }
        }
        const auto parsed =
            parser.feed(std::string_view(buffer, static_cast<std::size_t>(n)), request);
        if (parsed.state == go2cpp::http::ParseState::Error) {
            outcome.status = 400;
            outcome.reason = "parse error";
            outcome.received = request.body.size();
            WriteError(fd, 400, "请求行或请求头非法");
            return outcome;
        }
        if (parsed.state == go2cpp::http::ParseState::Complete) {
            // 只有完整请求才允许落盘（douyin_http.hpp:604-616 的 kOk 分支）。
            outcome.declared = request.body.size();
            outcome.received = request.body.size();
            {
                std::ofstream out(file_path, std::ios::binary);
                out << request.body;
            }
            outcome.handler_called = true;
            outcome.status = 200;
            return outcome;
        }
        // NeedMore：继续等下一段。
    }

    // 未 Complete：根据 EOF 还是超时给不同状态码（douyin_http.hpp:608-615）。
    outcome.received =
        header_bytes > 0 && raw.size() > header_bytes ? raw.size() - header_bytes : 0;
    outcome.declared = 1024;  // 请求头里声明的 Content-Length（示例固定）。
    if (eof) {
        outcome.status = 400;
        outcome.reason = "请求体不完整（对端提前关闭）";
    } else {
        outcome.status = 408;
        outcome.reason = "请求体读取超时（30 秒预算，示例 300ms）";
    }
    if (audit != nullptr) {
        audit->Record("/upload", outcome.declared, outcome.received,
                      outcome.reason);
    }
    // 真把错误响应写回客户端（对端已断开时 send 会失败，无害）。
    WriteError(fd, outcome.status, outcome.reason);
    return outcome;
}

/// @brief 构造一个 Content-Length=1024 的 POST 请求，body 只带 body_bytes 字节。
std::string MakeRequest(std::size_t body_bytes) {
    return "POST /upload HTTP/1.1\r\nHost: demo\r\n"
           "Content-Length: 1024\r\nContent-Type: application/octet-stream\r\n\r\n" +
           std::string(body_bytes, 'A');
}

/// @brief 客户端线程：发送 request 后按 close_after 决定直接关闭还是保持。
void SendAndClose(int fd, std::string request, bool close_after) {
    std::size_t sent = 0;
    while (sent < request.size()) {
        const ssize_t n =
            ::send(fd, request.data() + sent, request.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) {
            break;
        }
        sent += static_cast<std::size_t>(n);
    }
    if (close_after) {
        ::close(fd);
    }
}

int ReadStatus(int fd) {
    char buffer[512];
    const ssize_t n = ::recv(fd, buffer, sizeof(buffer) - 1, 0);
    if (n <= 0) {
        return 0;
    }
    buffer[n] = '\0';
    int status = 0;
    if (std::sscanf(buffer, "HTTP/1.1 %d", &status) != 1) {
        return 0;
    }
    return status;
}

std::string TempDir() {
    const auto dir = std::filesystem::temp_directory_path() / "go2cpp_partial_demo";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir.string();
}

}  // namespace

int main() {
    const std::string dir = TempDir();
    const std::string partial_file = dir + "/partial.bin";
    const std::string complete_file = dir + "/complete.bin";
    AuditLog audit;
    bool ok = true;

    // 场景 A：发一半就断开 —— 真实 TCP FIN，服务端读到 EOF 时仍 NeedMore。
    {
        int sv[2];
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
            return 1;
        }
        std::thread sender(SendAndClose, sv[0], MakeRequest(100), true);
        const Outcome outcome = ServeOne(sv[1], 300, partial_file, &audit);
        sender.join();
        ::close(sv[1]);
        const bool file_exists = std::filesystem::exists(partial_file);
        std::cout << "[断开] status=" << outcome.status
                  << " received=" << outcome.received
                  << " handler=" << outcome.handler_called
                  << " file_exists=" << file_exists
                  << "（预期 400 / 部分字节 / 0 / 0）\n";
        ok = ok && outcome.status == 400 && !outcome.handler_called &&
             !file_exists && audit.partial_events == 1;
    }

    // 场景 B：发一半后不发也不关（客户端被挂起）—— 总预算到期回 408。
    {
        int sv[2];
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
            return 1;
        }
        std::thread sender(SendAndClose, sv[0], MakeRequest(100), false);
        const Outcome outcome = ServeOne(sv[1], 300, partial_file, &audit);
        sender.join();
        // 客户端保持连接，真实读回服务端写的 408（证明超时路径确实回响应）。
        const int client_status = ReadStatus(sv[0]);
        ::close(sv[0]);
        ::close(sv[1]);
        const bool file_exists = std::filesystem::exists(partial_file);
        std::cout << "[超时] status=" << outcome.status
                  << " received=" << outcome.received
                  << " handler=" << outcome.handler_called
                  << " file_exists=" << file_exists
                  << " client_status=" << client_status
                  << "（预期 408 / 部分字节 / 0 / 0 / 408）\n";
        ok = ok && outcome.status == 408 && !outcome.handler_called &&
             !file_exists && client_status == 408 && audit.partial_events == 2;
    }

    // 场景 C：完整请求 —— 只有这里才落盘。
    {
        int sv[2];
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
            return 1;
        }
        std::thread sender(SendAndClose, sv[0], MakeRequest(1024), true);
        const Outcome outcome = ServeOne(sv[1], 500, complete_file, &audit);
        sender.join();
        ::close(sv[1]);
        const bool file_exists = std::filesystem::exists(complete_file);
        const auto size = file_exists ? std::filesystem::file_size(complete_file) : 0;
        std::cout << "[完整] status=" << outcome.status
                  << " handler=" << outcome.handler_called
                  << " file_size=" << size << "（预期 200 / 1 / 1024）\n";
        ok = ok && outcome.status == 200 && outcome.handler_called &&
             file_exists && size == 1024;
    }

    // 场景 D：可选策略「重试」——客户端发现 400 后整体重发一次成功。
    {
        int sv[2];
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
            return 1;
        }
        std::thread sender(SendAndClose, sv[0], MakeRequest(100), true);
        const Outcome first = ServeOne(sv[1], 300, partial_file, &audit);
        sender.join();
        ::close(sv[1]);

        // 重试：完整重发，不依赖服务端保留半截文件。
        int retry[2];
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, retry) != 0) {
            return 1;
        }
        std::thread retry_sender(SendAndClose, retry[0], MakeRequest(1024), true);
        const Outcome second = ServeOne(retry[1], 500, complete_file, &audit);
        retry_sender.join();
        ::close(retry[1]);
        std::cout << "[重试] first_status=" << first.status
                  << " second_status=" << second.status
                  << " second_handler=" << second.handler_called
                  << "（预期 400 / 200 / 1）\n";
        ok = ok && first.status == 400 && second.status == 200 &&
             second.handler_called;
    }

    // 场景 E：续传的边界说明（真跑一次 Content-Range 解析，不落半截文件）。
    {
        go2cpp::http::Parser parser;
        go2cpp::http::Request request;
        const std::string header =
            "PUT /upload HTTP/1.1\r\nHost: demo\r\n"
            "Content-Range: bytes 100-1023/1024\r\nContent-Length: 924\r\n\r\n";
        const auto parsed =
            parser.feed(header + std::string(924, 'B'), request);
        const bool resume_ok = parsed.state == go2cpp::http::ParseState::Complete &&
                               request.header.get("Content-Range") ==
                                   "bytes 100-1023/1024";
        std::cout << "[续传] state=" << static_cast<int>(parsed.state)
                  << " content_range=\""
                  << request.header.get("Content-Range") << "\""
                  << "（预期 Complete / bytes 100-1023/1024；"
                     "续传需服务端显式开启临时文件，默认不保存半截）\n";
        ok = ok && resume_ok;
    }

    std::cout << "[审计] partial_events=" << audit.partial_events
              << "（预期 3：A/B/D 各一次）\n";
    ok = ok && audit.partial_events == 3;

    std::cout << (ok ? "[partial-body] 通过\n" : "[partial-body] 失败\n");
    return ok ? 0 : 1;
}
