#include "go2cpp/network/http.hpp"
#include "go2cpp/network/socket.hpp"
#include "go2cpp/network/websocket.hpp"

#include <array>
#include <iostream>
#include <thread>

/**
 * @brief 展示最小的线程 WebSocket 客户端/服务端闭环。
 * @details 真实程序通常由 accept 循环产生 Socket，再交给 Scheduler/IOManager；
 *          这里用 socketpair 把握手、文本消息和关闭流程压缩到一个可复现示例。
 */
int main() {
    int pair[2]{};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) return 1;
    auto server_socket = go2cpp::Socket::FromFd(pair[0], AF_UNIX, SOCK_STREAM);
    auto client_socket = go2cpp::Socket::FromFd(pair[1], AF_UNIX, SOCK_STREAM);
    bool server_ok = false;
    std::thread server([&] {
        go2cpp::http::Parser parser;
        go2cpp::http::Request request;
        std::array<char, 256> buffer{};
        for (;;) {
            const auto received = server_socket->recv(buffer.data(), buffer.size(), 0,
                                                      std::chrono::seconds(2));
            if (!received.ok() || received.transferred == 0) return;
            const auto parsed = parser.feed(
                std::string_view(buffer.data(), static_cast<std::size_t>(received.transferred)), request);
            if (parsed.state == go2cpp::http::ParseState::Error) return;
            if (parsed.state == go2cpp::http::ParseState::Complete) break;
        }
        go2cpp::WebSocketResult handshake;
        auto ws = go2cpp::WebSocket::AcceptWithPending(
            server_socket, request, parser.take_remaining(), {}, &handshake);
        if (!ws || !handshake.ok()) return;
        go2cpp::WebSocketMessage message;
        if (!ws->read_message(message).ok() || !message.text()) return;
        std::cout << "server received: " << message.payload << '\n';
        server_ok = ws->send_text("echo: " + message.payload).ok();
        ws->close();
    });

    go2cpp::WebSocketResult handshake;
    auto client = go2cpp::WebSocket::Connect(
        client_socket, "localhost", "/demo", {}, &handshake);
    if (!client || !handshake.ok() || !client->send_text("hello").ok()) {
        client_socket->close();
        server.join();
        return 1;
    }
    go2cpp::WebSocketMessage response;
    const bool received = client->read_message(response).ok() && response.text();
    if (received) std::cout << "client received: " << response.payload << '\n';
    client->close();
    server.join();
    return received && server_ok ? 0 : 1;
}
