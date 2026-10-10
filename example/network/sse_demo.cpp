#include "go2cpp/network/sse.hpp"

#include <sys/socket.h>

#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

/**
 * @brief 演示不建立网络连接时的事件编码和增量解析。
 * @details 这适合新手先理解 SSE 线协议，再把同一事件交给 SseStream。
 */
void demo_event_and_parser() {
    go2cpp::SseEvent event;
    event.id = "demo-1";
    event.event = "message";
    event.data = "第一行\n第二行";
    event.retry_ms = 2000;

    std::string wire;
    const auto encoded = event.encode(wire);
    std::cout << "[sse] encode ok=" << encoded.ok() << "\n" << wire;

    go2cpp::SseParser parser;
    std::vector<go2cpp::SseEvent> events;
    // 模拟 TCP 半包：解析器不假设一次 recv 就是一条完整事件。
    parser.feed("id: demo-1\nevent: message\ndata: 第一", events);
    parser.feed("行\ndata: 第二行\n\n", events);
    for (const auto& decoded : events) {
        std::cout << "[sse] parsed id=" << decoded.id
                  << " event=" << decoded.event
                  << " data=" << decoded.data << '\n';
    }

    events.clear();
    parser.feed("id: checkpoint-2\n\n", events);
    std::cout << "[sse] metadata-only events=" << events.size()
              << " last-id=" << parser.last_event_id() << '\n';
}

/**
 * @brief 演示普通线程中的 SseStream 生命周期。
 * @details socketpair 只用于演示，真实服务端应把已接受的 TCP Socket
 *          交给 Stream；Stream 的写入在 IOManager Fiber 中同样适用。
 */
void demo_stream() {
    int pair[2]{};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) {
        std::cerr << "[sse] socketpair failed\n";
        return;
    }
    auto server = go2cpp::net::Socket::FromFd(pair[0], AF_UNIX, SOCK_STREAM);
    auto client = go2cpp::net::Socket::FromFd(pair[1], AF_UNIX, SOCK_STREAM);

    go2cpp::SseStreamOptions options;
    options.heartbeat_interval = std::chrono::seconds(10);
    options.write_timeout = std::chrono::seconds(2);
    go2cpp::SseStream stream(server, go2cpp::Context::Background(), options);
    const auto started = stream.start();
    std::cout << "[sse] stream start=" << started.ok() << '\n';

    go2cpp::SseEvent event;
    event.id = "demo-2";
    event.data = "hello from go2cpp";
    const auto sent = stream.send(event);
    std::cout << "[sse] event send=" << sent.ok() << '\n';
    const auto heartbeat = stream.comment("manual heartbeat");
    std::cout << "[sse] comment send=" << heartbeat.ok() << '\n';

    char buffer[4096]{};
    const auto received = client->recv(buffer, sizeof(buffer), 0,
                                       std::chrono::seconds(2));
    if (received.ok()) {
        std::cout << "[sse] raw response bytes=" << received.transferred << '\n';
    }
    (void)stream.close();
    server->close();
    client->close();
}

/** @brief 演示 Context 取消后，流不再接受新的事件。 */
void demo_cancellation() {
    int pair[2]{};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) return;
    auto server = go2cpp::net::Socket::FromFd(pair[0], AF_UNIX, SOCK_STREAM);
    auto client = go2cpp::net::Socket::FromFd(pair[1], AF_UNIX, SOCK_STREAM);
    auto cancel_pair = go2cpp::Context::WithCancel(go2cpp::Context::Background());
    go2cpp::SseStream stream(server, cancel_pair.first);
    const auto started = stream.start();
    cancel_pair.second();
    go2cpp::SseEvent event;
    event.data = "must not be delivered";
    const auto result = stream.send(event);
    std::cout << "[sse] cancelled start=" << started.ok()
              << " send-ok=" << result.ok() << '\n';
    server->close();
    client->close();
}

}  // namespace

/**
 * @brief SSE 入门示例入口。
 * @return 成功返回 0；示例中的网络错误只打印并结束，便于直接运行。
 */
int main() {
    demo_event_and_parser();
    demo_stream();
    demo_cancellation();
    return 0;
}
