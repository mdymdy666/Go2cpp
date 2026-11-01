/**
 * @file test_rpc_websocket.cpp
 * @brief WebSocket 传输适配器端到端测试（socketpair，不占 TCP 端口）。
 *
 * @details 用一条 AF_UNIX socketpair 完成真实 WebSocket 握手，然后把
 *          `net::WebSocket` 包成 `rpc::WebSocketTransport` 交给 RPC 客户端/
 *          服务端。验证：普通调用、流式、心跳、关闭唤醒全部走 WS 消息路径。
 *
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include "rpc_test_support.hpp"

#include <sys/socket.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "go2cpp/network/http.hpp"
#include "go2cpp/network/socket.hpp"
#include "go2cpp/network/websocket.hpp"
#include "go2cpp/rpc/websocket_transport.hpp"

namespace {

using namespace go2cpp;
using namespace rpctest;

}  // namespace

int main() {
    Announce("websocket: handshake + rpc call + stream");
    rpc::Server server;
    UserService service;
    if (!server.Register("User", &service) || !server.Listen("127.0.0.1:0")) {
        std::printf("[rpc-ws] 服务端启动失败\n");
        return 1;
    }

    int pair[2]{};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) {
        std::printf("[rpc-ws] socketpair 失败\n");
        return 1;
    }
    auto server_socket = net::Socket::FromFd(pair[0], AF_UNIX, SOCK_STREAM);
    auto client_socket = net::Socket::FromFd(pair[1], AF_UNIX, SOCK_STREAM);

    std::string server_error;
    std::thread ws_server([&server, server_socket, &server_error]() {
        http::Parser parser;
        http::Request request;
        std::array<char, 2048> buffer{};
        bool complete = false;
        while (!complete) {
            const net::SocketResult received =
                server_socket->recv(buffer.data(), buffer.size(), 0, std::chrono::seconds(5));
            if (!received.ok() || received.transferred == 0) {
                server_error = "handshake recv failed";
                return;
            }
            const auto parsed =
                parser.feed(std::string_view(buffer.data(),
                                             static_cast<std::size_t>(received.transferred)),
                            request);
            if (parsed.state == http::ParseState::Error) {
                server_error = "http parse error";
                return;
            }
            complete = parsed.state == http::ParseState::Complete;
        }
        net::WebSocketResult handshake;
        auto websocket = net::WebSocket::Accept(server_socket, request, {}, &handshake);
        if (!websocket || !handshake.ok()) {
            server_error = "websocket accept failed: " + handshake.message;
            return;
        }
        if (!server.AddConnection(
                std::make_unique<rpc::WebSocketTransport>(std::move(websocket)))) {
            server_error = "server.AddConnection failed";
        }
    });

    net::WebSocketResult client_handshake;
    auto websocket_client =
        net::WebSocket::Connect(client_socket, "localhost", "/rpc", {}, &client_handshake);
    if (!websocket_client || !client_handshake.ok()) {
        std::printf("[rpc-ws] 客户端握手失败: %s\n", client_handshake.message.c_str());
        server.Stop();
        ws_server.join();
        return 1;
    }

    rpc::Client client(std::make_unique<rpc::WebSocketTransport>(websocket_client));
    RPC_CHECK(client.connected());

    const User user = client.Call<User>("User.Get", UserId{9});
    RPC_CHECK_EQ(user.id, static_cast<std::int64_t>(9));
    RPC_CHECK_EQ(user.name, std::string("user-9"));

    std::int64_t sum = 0;
    RPC_CHECK(client.TryCall("User.Add", &sum, static_cast<std::int64_t>(40),
                             static_cast<std::int64_t>(2))
                  .ok());
    RPC_CHECK_EQ(sum, static_cast<std::int64_t>(42));
    RPC_CHECK(client.Ping().ok());

    auto stream = client.OpenStream("User.Watch", WatchRequest{64, 0});
    RPC_CHECK(stream.valid());
    ChatMessage message;
    rpc::Status status;
    std::int32_t expected = 0;
    while (stream.Next(&message, &status, std::chrono::milliseconds(3000))) {
        RPC_CHECK_EQ(message.seq, expected);
        ++expected;
    }
    RPC_CHECK(status.ok());
    RPC_CHECK_EQ(expected, static_cast<std::int32_t>(64));

    client.Close();
    server.Stop();
    ws_server.join();
    RPC_CHECK(server_error.empty());
    if (!server_error.empty()) {
        std::printf("[rpc-ws] server error: %s\n", server_error.c_str());
    }
    return Summary("websocket");
}
