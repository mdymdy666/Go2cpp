/**
 * @file http2_h2c_server.cpp
 * @brief 明文 HTTP/2（h2c）服务器示例 —— 用于与 curl 互通验证。
 *
 * ## 为什么需要这个示例
 *
 * 之前的测试都是「我的客户端 ↔ 我的服务端」，只能证明自洽。要证明**互操作**，
 * 必须让一个**独立实现**来对话。`curl` 内置 nghttp2，是最方便的验证工具：
 *
 * ```sh
 * # 终端 1
 * ./build-quality/go2cpp_http2_h2c_server 18080
 *
 * # 终端 2（--http2-prior-knowledge 表示不走 TLS、直接用明文 h2c）
 * curl -v --http2-prior-knowledge http://127.0.0.1:18080/hello
 * ```
 *
 * 如果 curl 能正确打印出响应体，说明我们的 **SETTINGS 交换、HPACK 编码、
 * 帧层、流状态机**都被独立实现接受了。
 *
 * ## 实现说明
 *
 * 主线程阻塞在 `accept`，每个连接交给一个 Fiber 处理（`Scheduler::spawn`）。
 * 连接内的 HTTP/2 逻辑全部是**同步写法**，IO 由 `FiberTransport` 在
 * `Socket::recv/send` 上挂起 —— 与 Go 中每个连接一个 goroutine 的写法一致。
 */
#include "go2cpp/network/http2/connection.hpp"
#include "go2cpp/network/http2/fiber_transport.hpp"
#include "go2cpp/network/socket.hpp"
#include "go2cpp/scheduler.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

using namespace go2cpp::net::http2;
using go2cpp::net::IPv4Address;
using go2cpp::net::Socket;
using go2cpp::scheduler::Scheduler;
using go2cpp::scheduler::SchedulerConfig;
using namespace std::chrono_literals;

/// 处理单个连接（在一个 Fiber 里跑）。
void ServeConnection(Socket::Ptr socket, int connection_id) {
    FiberTransport transport(socket, 30s);
    Connection connection(&transport, Role::kServer);
    std::string error;
    if (connection.Handshake(&error) != ConnStatus::kOk) {
        std::fprintf(stderr, "[server] 连接 %d 握手失败: %s\n", connection_id,
                     error.c_str());
        return;
    }
    std::printf("[server] 连接 %d 握手完成\n", connection_id);

    IncomingEvent event;
    for (;;) {
        const auto status = connection.ReadEvent(&event, &error);
        if (status != ConnStatus::kOk) {
            if (status == ConnStatus::kClosed) {
                std::printf("[server] 连接 %d 对端关闭\n", connection_id);
            } else {
                std::fprintf(stderr, "[server] 连接 %d 读事件失败: %s\n", connection_id,
                             error.c_str());
            }
            return;
        }
        if (event.kind == IncomingEvent::Kind::kGoAway) {
            std::printf("[server] 连接 %d 收到 GOAWAY\n", connection_id);
            return;
        }
        if (event.kind != IncomingEvent::Kind::kHeaders) {
            continue;
        }

        // 打印请求信息，便于与 curl 的输出对照
        std::string method;
        std::string path;
        std::string authority;
        for (const auto& field : event.headers) {
            if (field.name == ":method") method = field.value;
            if (field.name == ":path") path = field.value;
            if (field.name == ":authority") authority = field.value;
        }
        std::printf("[server] 连接 %d 请求: %s %s (authority=%s) stream=%u\n",
                    connection_id, method.c_str(), path.c_str(), authority.c_str(),
                    event.stream_id);

        // 回一个简单响应
        const std::string body =
            "hello from go2cpp http2\nmethod=" + method + "\npath=" + path + "\n";
        const std::vector<HeaderField> headers = {
            {":status", "200"},
            {"content-type", "text/plain; charset=utf-8"},
            {"server", "go2cpp-http2"},
        };
        if (connection.SendResponseHeaders(event.stream_id, headers, false, &error) !=
            ConnStatus::kOk) {
            std::fprintf(stderr, "[server] 连接 %d 发响应头失败: %s\n", connection_id,
                         error.c_str());
            return;
        }
        if (connection.WriteData(event.stream_id,
                                 std::vector<std::uint8_t>(body.begin(), body.end()), true,
                                 &error) != ConnStatus::kOk) {
            std::fprintf(stderr, "[server] 连接 %d 发响应体失败: %s\n", connection_id,
                         error.c_str());
            return;
        }
        std::printf("[server] 连接 %d 已响应 %zu 字节\n", connection_id, body.size());
    }
}

}  // namespace

int main(int argc, char** argv) {
    const int port = (argc > 1) ? std::atoi(argv[1]) : 18080;

    SchedulerConfig config;
    config.processor_count = 2;
    config.min_workers = 2;
    config.max_workers = 4;
    config.collect_metrics = false;
    Scheduler scheduler(config);
    scheduler.start();

    auto address = IPv4Address::Create("127.0.0.1", static_cast<std::uint16_t>(port));
    if (!address) {
        std::fprintf(stderr, "[server] 地址无效\n");
        return 1;
    }
    auto listener = Socket::CreateTCP(AF_INET);
    if (!listener || !listener->bind(*address) || !listener->listen(128)) {
        std::fprintf(stderr, "[server] 监听 %s 失败\n", address->to_string().c_str());
        return 1;
    }
    std::printf("[server] 监听 %s（明文 HTTP/2，h2c）\n", address->to_string().c_str());
    std::printf("[server] 用 curl 验证: curl -v --http2-prior-knowledge "
                "http://127.0.0.1:%d/hello\n", port);
    std::fflush(stdout);

    std::atomic<int> next_connection{1};
    for (;;) {
        auto socket = listener->accept();
        if (!socket) {
            // accept 失败（例如被信号打断）后继续
            continue;
        }
        const int id = next_connection.fetch_add(1);
        scheduler.spawn([socket, id] { ServeConnection(socket, id); });
    }
    return 0;
}
