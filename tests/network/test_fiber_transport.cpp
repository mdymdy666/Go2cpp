/**
 * @file test_fiber_transport.cpp
 * @brief 在**真实 socket + Fiber** 上验证 FiberTransport 与连接层。
 *
 * ## 这个测试补的是什么
 *
 * `test_connection.cpp` 用 `MemoryTransport` 验证了协议逻辑，但它**没有验证
 * "IO 真的能工作"**：内存缓冲不会因为 TCP 分段而"读到一半"，也不会因为
 * `send` 只写出一部分而出错。
 *
 * 本测试用 `socketpair` + 本项目真实的 `Socket`（内部走 hook + IOManager）：
 * - 客户端与服务端各跑在一个 Fiber 里；
 * - 字节真的经过内核 socket 缓冲区；
 * - 验证握手、请求、响应三个闭环在真实 IO 上都成立。
 *
 * 这同时验证了「连接层只依赖 Transport」的设计确实能接上真实网络，且
 * **连接层代码一行没改**。
 */
#include "go2cpp/network/http2/connection.hpp"
#include "go2cpp/network/http2/fiber_transport.hpp"
#include "go2cpp/network/socket.hpp"
#include "go2cpp/scheduler.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace {

using namespace go2cpp::net::http2;
using go2cpp::net::Socket;
using go2cpp::scheduler::Scheduler;
using go2cpp::scheduler::SchedulerConfig;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

int g_failures = 0;

void Check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "[fiber-transport] FAILED: %s\n", what);
        ++g_failures;
    }
}

/// 创建一对已连接的 Unix 域流式 socket。
bool MakeSocketPair(Socket::Ptr* first, Socket::Ptr* second) {
    int fds[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        return false;
    }
    *first = Socket::FromFd(fds[0], AF_UNIX, SOCK_STREAM, 0, true);
    *second = Socket::FromFd(fds[1], AF_UNIX, SOCK_STREAM, 0, true);
    return *first != nullptr && *second != nullptr;
}

// ---------------------------------------------------------------------------
// 场景 1：FiberTransport 的读写往返（最小验证）
// ---------------------------------------------------------------------------
void ScenarioTransportRoundTrip() {
    Socket::Ptr a;
    Socket::Ptr b;
    Check(MakeSocketPair(&a, &b), "socketpair failed");
    if (!a || !b) {
        return;
    }
    FiberTransport ta(a, 5000ms);
    FiberTransport tb(b, 5000ms);

    std::atomic<int> written{0};
    std::atomic<int> read_bytes{0};
    std::string received;

    SchedulerConfig config;
    config.processor_count = 2;
    config.min_workers = 2;
    config.max_workers = 2;
    config.collect_metrics = false;
    Scheduler scheduler(config);
    scheduler.start();
    std::this_thread::sleep_for(50ms);

    auto writer = scheduler.spawn([&] {
        const std::string payload = "hello-fiber-transport";
        written = static_cast<int>(ta.Write(
            reinterpret_cast<const std::uint8_t*>(payload.data()), payload.size()));
    });
    auto reader = scheduler.spawn([&] {
        std::uint8_t buffer[64] = {};
        const auto got = tb.Read(buffer, sizeof(buffer));
        read_bytes = static_cast<int>(got);
        received.assign(reinterpret_cast<char*>(buffer), got);
    });

    Check(reader->wait_for(10s), "reader did not finish");
    Check(writer->wait_for(10s), "writer did not finish");
    (void)scheduler.shutdown_for(5s);

    Check(written.load() == 21, "Write should report full length");
    Check(read_bytes.load() == 21, "Read should get all bytes");
    Check(received == "hello-fiber-transport", "payload mismatch over real socket");
}

// ---------------------------------------------------------------------------
// 场景 2：真实 socket 上的完整 HTTP/2 请求/响应闭环
// ---------------------------------------------------------------------------
void ScenarioHttp2OverSocket() {
    Socket::Ptr server_socket;
    Socket::Ptr client_socket;
    Check(MakeSocketPair(&server_socket, &client_socket), "socketpair failed");
    if (!server_socket || !client_socket) {
        return;
    }
    FiberTransport server_transport(server_socket, 10s);
    FiberTransport client_transport(client_socket, 10s);

    Connection server(&server_transport, Role::kServer);
    Connection client(&client_transport, Role::kClient);

    std::atomic<bool> server_ok{false};
    std::atomic<bool> client_ok{false};
    std::string server_error;
    std::string client_error;
    std::string seen_path;
    std::string seen_method;
    std::string response_body;

    SchedulerConfig config;
    config.processor_count = 2;
    config.min_workers = 2;
    config.max_workers = 2;
    config.collect_metrics = false;
    Scheduler scheduler(config);
    scheduler.start();
    std::this_thread::sleep_for(50ms);

    // 服务端：握手 -> 读请求 HEADERS -> 回 200 + body
    auto server_task = scheduler.spawn([&] {
        if (server.Handshake(&server_error) != ConnStatus::kOk) {
            return;
        }
        IncomingEvent event;
        for (int guard = 0; guard < 10; ++guard) {
            const auto status = server.ReadEvent(&event, &server_error);
            if (status != ConnStatus::kOk) {
                return;
            }
            if (event.kind == IncomingEvent::Kind::kHeaders) {
                for (const auto& field : event.headers) {
                    if (field.name == ":path") seen_path = field.value;
                    if (field.name == ":method") seen_method = field.value;
                }
                const std::string body = "pong-from-server";
                if (server.SendResponseHeaders(
                        event.stream_id,
                        {{":status", "200"}, {"content-type", "text/plain"}}, false,
                        &server_error) != ConnStatus::kOk) {
                    return;
                }
                if (server.WriteData(
                        event.stream_id,
                        std::vector<std::uint8_t>(body.begin(), body.end()), true,
                        &server_error) != ConnStatus::kOk) {
                    return;
                }
                server_ok = true;
                return;
            }
        }
    });

    // 客户端：握手 -> 发请求 -> 读响应
    auto client_task = scheduler.spawn([&] {
        if (client.Handshake(&client_error) != ConnStatus::kOk) {
            return;
        }
        std::uint32_t stream_id = 0;
        if (client.StartRequest({{":method", "GET"},
                                 {":scheme", "http"},
                                 {":path", "/ping"},
                                 {":authority", "localhost"}},
                                true, &stream_id, &client_error) != ConnStatus::kOk) {
            return;
        }
        bool got_headers = false;
        bool got_body = false;
        IncomingEvent event;
        for (int guard = 0; guard < 10 && !(got_headers && got_body); ++guard) {
            if (client.ReadEvent(&event, &client_error) != ConnStatus::kOk) {
                return;
            }
            if (event.kind == IncomingEvent::Kind::kHeaders) {
                for (const auto& field : event.headers) {
                    if (field.name == ":status") {
                        got_headers = (field.value == "200");
                    }
                }
            } else if (event.kind == IncomingEvent::Kind::kData) {
                response_body.assign(event.data.begin(), event.data.end());
                got_body = true;
            }
        }
        client_ok = (got_headers && got_body);
    });

    Check(client_task->wait_for(20s), "client fiber timed out");
    Check(server_task->wait_for(20s), "server fiber timed out");
    (void)scheduler.shutdown_for(10s);

    if (!client_error.empty()) {
        std::fprintf(stderr, "[fiber-transport] client error: %s\n", client_error.c_str());
    }
    if (!server_error.empty()) {
        std::fprintf(stderr, "[fiber-transport] server error: %s\n", server_error.c_str());
    }
    Check(server_ok.load(), "server side did not complete");
    Check(client_ok.load(), "client side did not complete");
    Check(seen_method == "GET", "server should see :method GET");
    Check(seen_path == "/ping", "server should see :path /ping");
    Check(response_body == "pong-from-server", "client should get the response body");
    std::printf("[fiber-transport] 真实 socket 闭环: method=%s path=%s body=%s\n",
                seen_method.c_str(), seen_path.c_str(), response_body.c_str());
}

}  // namespace

int main() {
    std::printf("=== FiberTransport 真实 IO 测试 ===\n");
    std::printf("[fiber-transport][场景1] Transport 读写往返\n");
    ScenarioTransportRoundTrip();
    std::printf("[fiber-transport][场景2] 真实 socket 上的 HTTP/2 闭环\n");
    ScenarioHttp2OverSocket();

    if (g_failures == 0) {
        std::printf("\n[fiber-transport] failures=0\n");
    } else {
        std::printf("\n[fiber-transport] failures=%d\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
