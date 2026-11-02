/**
 * @file http2_h2c_client.cpp
 * @brief 明文 HTTP/2（h2c）客户端示例 —— 用于反向互通验证。
 *
 * ## 它验证什么
 *
 * 之前用 curl 验证了「**别人能读我**」（curl 当客户端、我的服务端）。
 * 这个程序验证反方向：「**我能读别人**」—— 我的 `StartRequest` + HPACK 解码 +
 * 帧解析 + 流状态机，能不能正确理解一个**独立实现**发来的响应。
 *
 * 配套的独立实现是 `.dsh_tmp/minimal_h2c_server.py`（用 socket + struct 手工
 * 拼 HTTP/2 帧，不依赖任何 h2 库）。用法：
 *
 * ```sh
 * # 终端 1
 * python3 minimal_h2c_server.py 18081 --seconds 20
 *
 * # 终端 2
 * ./build-quality/go2cpp_http2_h2c_client 18081 /from-go2cpp
 * ```
 *
 * 预期输出里应能看到 `:status = 200` 与 `hello from python h2c server`。
 */
#include "go2cpp/network/http2/connection.hpp"
#include "go2cpp/network/http2/fiber_transport.hpp"
#include "go2cpp/network/socket.hpp"
#include "go2cpp/scheduler.hpp"

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

int RunClient(int port, const std::string& path) {
    auto address = IPv4Address::Create("127.0.0.1", static_cast<std::uint16_t>(port));
    if (!address) {
        std::fprintf(stderr, "[client] 地址无效\n");
        return 1;
    }
    auto socket = Socket::CreateTCP(AF_INET);
    if (!socket) {
        std::fprintf(stderr, "[client] 创建 socket 失败\n");
        return 1;
    }
    if (!socket->connect(*address, 5000ms)) {
        std::fprintf(stderr, "[client] 连接 %s 失败\n", address->to_string().c_str());
        return 1;
    }
    std::printf("[client] 已连接 %s\n", address->to_string().c_str());

    FiberTransport transport(socket, 10s);
    Connection connection(&transport, Role::kClient);
    std::string error;
    if (connection.Handshake(&error) != ConnStatus::kOk) {
        std::fprintf(stderr, "[client] 握手失败: %s\n", error.c_str());
        return 1;
    }
    std::printf("[client] 握手完成\n");

    std::uint32_t stream_id = 0;
    const std::vector<HeaderField> request = {
        {":method", "GET"},
        {":scheme", "http"},
        {":path", path},
        {":authority", "127.0.0.1:" + std::to_string(port)},
        {"user-agent", "go2cpp-http2-client"},
    };
    if (connection.StartRequest(request, true, &stream_id, &error) != ConnStatus::kOk) {
        std::fprintf(stderr, "[client] 发请求失败: %s\n", error.c_str());
        return 1;
    }
    std::printf("[client] 已发送请求 %s（stream=%u）\n", path.c_str(), stream_id);

    bool got_status = false;
    std::string reported_status;
    std::string body;
    IncomingEvent event;
    for (int guard = 0; guard < 16; ++guard) {
        const auto status = connection.ReadEvent(&event, &error);
        if (status != ConnStatus::kOk) {
            if (status == ConnStatus::kClosed) {
                std::printf("[client] 对端关闭\n");
            } else {
                std::fprintf(stderr, "[client] 读事件失败: %s\n", error.c_str());
            }
            break;
        }
        if (event.kind == IncomingEvent::Kind::kHeaders) {
            for (const auto& field : event.headers) {
                std::printf("[client] 响应头: %s: %s\n", field.name.c_str(),
                            field.value.c_str());
                if (field.name == ":status") {
                    got_status = true;
                    reported_status = field.value;
                }
            }
        } else if (event.kind == IncomingEvent::Kind::kData) {
            body.append(event.data.begin(), event.data.end());
            std::printf("[client] 收到 DATA %zu 字节 (end_stream=%d)\n", event.data.size(),
                        event.end_stream ? 1 : 0);
            if (event.end_stream) {
                break;
            }
        } else if (event.kind == IncomingEvent::Kind::kGoAway) {
            std::printf("[client] 收到 GOAWAY\n");
            break;
        }
    }

    std::printf("\n[client] === 结果 ===\n");
    std::printf("[client] :status = %s\n", got_status ? reported_status.c_str() : "(缺失)");
    std::printf("[client] body = %s\n", body.c_str());
    if (!got_status || reported_status != "200") {
        std::fprintf(stderr, "[client] 未拿到 200 状态\n");
        return 1;
    }
    if (body.find("hello from python h2c server") == std::string::npos) {
        std::fprintf(stderr, "[client] body 内容不符\n");
        return 1;
    }
    std::printf("[client] 反向互通验证通过\n");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const int port = (argc > 1) ? std::atoi(argv[1]) : 18081;
    const std::string path = (argc > 2) ? argv[2] : "/from-go2cpp";

    SchedulerConfig config;
    config.processor_count = 1;
    config.min_workers = 1;
    config.max_workers = 1;
    config.collect_metrics = false;
    Scheduler scheduler(config);
    scheduler.start();

    std::printf("[client] 连接到 127.0.0.1:%d，请求 %s\n", port, path.c_str());
    int result = 1;
    // 整个客户端逻辑跑在一个 Fiber 里：Socket::connect/recv 会挂起协程，
    // 这是本项目 IO 模型的正常用法。
    auto task = scheduler.spawn([&] { result = RunClient(port, path); });
    const bool finished = task->wait_for(30s);
    (void)scheduler.shutdown_for(5s);
    if (!finished) {
        std::fprintf(stderr, "[client] 客户端 Fiber 超时\n");
        return 1;
    }
    return result;
}
