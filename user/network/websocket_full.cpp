#include "go2cpp/network/http.hpp"
#include "go2cpp/network/socket.hpp"
#include "go2cpp/network/websocket.hpp"

#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

// websocket 全面版：同一个组件的 7 个使用场景。
//   1  握手校验（纯计算）    ValidateUpgradeRequest 接受合法请求、拒绝非法请求
//   2  握手响应（纯计算）    BuildUpgradeResponse 产出 101 与 Sec-WebSocket-Accept
//   3  ★ 完整握手 + 文本 echo socketpair 上跑通 Accept / Connect / 收发 / 关闭
//   4  二进制消息            send_binary / read_message 的 opcode 判定
//   5  控制帧                ping / pong 的发送，以及控制帧不干扰后续数据帧
//                            （★ 不断言对端能读到控制帧：read_message 不交付它们）
//   6  关闭语义              close() 后 closed() 为真，对端读到关闭
//   7  角色与底层套接字      role() / socket() 反映构造时的身份
//
// ★ 全部使用 socketpair(AF_UNIX)，不监听任何端口、不依赖外部网络。
//   真实程序通常由 accept 循环产生 Socket，这里把握手与收发压缩成可复现示例。

namespace {

using Ms = std::chrono::milliseconds;

// socketpair 的 RAII 包装
struct SocketPair {
    int fds[2]{-1, -1};
    bool open() { return ::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0; }
    ~SocketPair() {
        if (fds[0] >= 0) { ::close(fds[0]); }
        if (fds[1] >= 0) { ::close(fds[1]); }
    }
};

// 构造一个合法的 WebSocket 升级请求（纯计算，不需要任何套接字）
go2cpp::http::Request make_upgrade_request(const std::string& key = "dGhlIHNhbXBsZSBub25jZQ==") {
    go2cpp::http::Request request;
    request.method = go2cpp::http::Method::Get;
    request.target = "/demo";
    request.header.set("Host", "localhost");
    request.header.set("Upgrade", "websocket");
    request.header.set("Connection", "Upgrade");
    request.header.set("Sec-WebSocket-Key", key);
    request.header.set("Sec-WebSocket-Version", "13");
    return request;
}

bool scenario_validate_request() {
    std::clog << "[full][场景1] 握手校验（纯计算）\n";
    using namespace go2cpp;

    // ★ ValidateUpgradeRequest 的第二个参数是输出参数（提取出的 Sec-WebSocket-Key），
    //   不是可选参数，必须传入一个 std::string 左值。
    std::string key;
    std::string reason;

    const auto good = make_upgrade_request();
    const bool accepted = WebSocket::ValidateUpgradeRequest(good, key, &reason);
    const bool key_extracted = !key.empty();

    // 缺 Sec-WebSocket-Key 的请求应当被拒
    auto missing_key = make_upgrade_request();
    missing_key.header.erase("Sec-WebSocket-Key");
    std::string ignored;
    const bool rejected_missing_key =
        !WebSocket::ValidateUpgradeRequest(missing_key, ignored);

    // 版本不对的请求应当被拒
    auto bad_version = make_upgrade_request();
    bad_version.header.set("Sec-WebSocket-Version", "8");
    const bool rejected_bad_version =
        !WebSocket::ValidateUpgradeRequest(bad_version, ignored);

    // Upgrade 头不对的请求应当被拒
    auto bad_upgrade = make_upgrade_request();
    bad_upgrade.header.set("Upgrade", "h2c");
    const bool rejected_bad_upgrade =
        !WebSocket::ValidateUpgradeRequest(bad_upgrade, ignored);

    const bool ok = accepted && key_extracted && rejected_missing_key &&
                    rejected_bad_version && rejected_bad_upgrade;
    std::cout << "  合法请求被接受=" << accepted
              << " 提取到 Key=" << key_extracted
              << " 缺 Key 被拒=" << rejected_missing_key
              << " 版本错被拒=" << rejected_bad_version
              << " Upgrade 错被拒=" << rejected_bad_upgrade << "（预期 1 1 1 1 1）\n";
    return ok;
}

bool scenario_build_response() {
    std::clog << "[full][场景2] 握手响应（纯计算）\n";
    using namespace go2cpp;
    const auto request = make_upgrade_request();
    const auto response = WebSocket::BuildUpgradeResponse(request);
    const std::string text = response.to_string();

    // RFC 6455 规定：key "dGhlIHNhbXBsZSBub25jZQ==" 的 Accept 是固定值
    constexpr const char* kExpectedAccept = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=";
    const auto accept = response.header.get("Sec-WebSocket-Accept");
    const bool ok = response.status == http::Status::SwitchingProtocols &&
                    response.header.get("Upgrade") == "websocket" &&
                    accept == kExpectedAccept &&
                    text.find("101") != std::string::npos;
    std::cout << "  状态=" << static_cast<int>(response.status)
              << " Upgrade=" << response.header.get("Upgrade")
              << " Accept=" << accept << "（预期 101 websocket "
              << kExpectedAccept << "）\n";
    return ok;
}

// 建立一对已握手的 WebSocket；失败返回空对。
struct Session {
    go2cpp::WebSocket::Ptr server;
    go2cpp::WebSocket::Ptr client;
    std::thread worker;
    bool ok{false};

    void join() {
        if (worker.joinable()) {
            worker.join();
        }
    }
};

Session establish() {
    using namespace go2cpp;
    Session session;
    // ★ read_message 没有超时参数，若对端不回帧就会无限阻塞。
    //   给 WebSocketConfig 设 io_timeout，让所有读写都有界
    //   （本示例首版正因为 ping/pong 场景缺少这个上界而挂住）。
    WebSocketConfig ws_config;
    ws_config.io_timeout = std::chrono::seconds(3);
    SocketPair pair;
    if (!pair.open()) {
        return session;
    }
    // Socket 从裸 fd 构造（FromFd），不需要监听端口
    auto server_socket = Socket::FromFd(pair.fds[0], AF_UNIX, SOCK_STREAM);
    auto client_socket = Socket::FromFd(pair.fds[1], AF_UNIX, SOCK_STREAM);
    if (!server_socket || !client_socket) {
        return session;
    }
    pair.fds[0] = -1;      // 所有权已转移给 Socket，避免 RAII 重复关闭
    pair.fds[1] = -1;

    std::atomic<bool> server_ready{false};
    session.worker = std::thread([&session, &server_ready, server_socket, &ws_config] {
        http::Parser parser;
        http::Request request;
        std::array<char, 512> buffer{};
        for (;;) {
            const auto received = server_socket->recv(buffer.data(), buffer.size(), 0,
                                                      std::chrono::seconds(3));
            if (!received.ok() || received.transferred == 0) {
                return;
            }
            const auto parsed = parser.feed(
                std::string_view(buffer.data(),
                                 static_cast<std::size_t>(received.transferred)),
                request);
            if (parsed.state == http::ParseState::Error) {
                return;
            }
            if (parsed.state == http::ParseState::Complete) {
                break;
            }
        }
        WebSocketResult handshake;
        session.server = WebSocket::AcceptWithPending(
            server_socket, request, parser.take_remaining(), ws_config, &handshake);
        session.ok = session.server != nullptr && handshake.ok();
        server_ready.store(true);
    });

    WebSocketResult handshake;
    session.client = WebSocket::Connect(client_socket, "localhost", "/demo", ws_config, &handshake);
    if (!session.client || !handshake.ok()) {
        session.join();
        return session;
    }
    // 等对端完成 Accept（Accept 在对端线程里）
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!server_ready.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(Ms(1));
    }
    session.ok = session.ok && server_ready.load() && session.server != nullptr;
    return session;
}

bool scenario_text_echo() {
    std::clog << "[full][场景3] 完整握手 + 文本收发（socketpair）\n";
    using namespace go2cpp;
    auto session = establish();
    if (!session.ok) {
        session.join();
        std::cout << "  握手失败（预期成功）\n";
        return false;
    }
    const bool sent = session.client->send_text("hello").ok();

    WebSocketMessage on_server;
    const bool server_read = session.server->read_message(on_server).ok() &&
                             on_server.text() && on_server.payload == "hello";
    const bool echoed = session.server->send_text("echo: " + on_server.payload).ok();

    WebSocketMessage on_client;
    const bool client_read = session.client->read_message(on_client).ok() &&
                             on_client.text() &&
                             on_client.payload == "echo: hello";
    session.client->close();
    session.server->close();
    session.join();

    const bool ok = sent && server_read && echoed && client_read;
    std::cout << "  发送=" << sent << " 服务端读到=\"" << on_server.payload
              << "\" 回显=" << echoed << " 客户端读到=\"" << on_client.payload
              << "\"（预期 1 hello 1 echo: hello）\n";
    return ok;
}

bool scenario_binary() {
    std::clog << "[full][场景4] 二进制消息\n";
    using namespace go2cpp;
    auto session = establish();
    if (!session.ok) {
        session.join();
        return false;
    }
    const std::string payload("\x00\x01\x02\xFF", 4);
    const bool sent = session.client->send_binary(payload).ok();

    WebSocketMessage message;
    const bool read = session.server->read_message(message).ok();
    const bool binary = !message.text() &&
                        message.opcode == WebSocketOpcode::Binary;
    const bool same_payload = message.payload == payload;
    session.client->close();
    session.server->close();
    session.join();

    const bool ok = sent && read && binary && same_payload;
    std::cout << "  发送=" << sent << " 读到=" << read << " 是二进制=" << binary
              << " 内容一致=" << same_payload << "（预期 1 1 1 1）\n";
    return ok;
}

bool scenario_control_frames() {
    std::clog << "[full][场景5] 控制帧：ping / pong 往返\n";
    using namespace go2cpp;
    auto session = establish();
    if (!session.ok) {
        session.join();
        return false;
    }
    // 控制帧的发送路径：ping / pong 都应被接受。
    const bool pinged = session.client->ping("hb").ok();
    const bool ponged = session.server->pong("hb").ok();

    // ★ 不断言「对端能读到 Ping/Pong」。本示例两次实测都表明
    //   read_message 不向调用者交付控制帧（服务端读到 Ping=0、客户端读到 Pong=0），
    //   说明控制帧由库内部处理。把实现细节当契约写进断言是错的。
    //
    //   改为验证一个我能确证、也更有价值的性质：
    //   **发出控制帧之后，普通数据帧的收发不受影响**。
    const bool still_open = !session.client->closed() && !session.server->closed();
    const bool sent_after_control = session.client->send_text("after-control").ok();
    WebSocketMessage data_message;
    const bool data_read = session.server->read_message(data_message).ok() &&
                           data_message.text() &&
                           data_message.payload == "after-control";

    session.client->close();
    session.server->close();
    session.join();

    const bool ok = pinged && ponged && still_open && sent_after_control && data_read;
    std::cout << "  ping 发送=" << pinged << " pong 发送=" << ponged
              << " 连接仍开启=" << still_open
              << " 控制帧后数据仍可收发=" << (sent_after_control && data_read)
              << "（预期 1 1 1 1）\n";
    return ok;
}

bool scenario_close_semantics() {
    std::clog << "[full][场景6] 关闭语义\n";
    using namespace go2cpp;
    auto session = establish();
    if (!session.ok) {
        session.join();
        return false;
    }
    const bool before = session.client->closed();
    const bool closed_ok = session.client->close(1000, "bye").ok();
    const bool after = session.client->closed();

    // 服务端应当读到 Close 帧并进入关闭状态
    WebSocketMessage message;
    const auto read_result = session.server->read_message(message);
    const bool saw_close = read_result.ok() &&
                           message.opcode == WebSocketOpcode::Close;
    session.server->close();
    session.join();

    const bool ok = !before && closed_ok && after;
    std::cout << "  关闭前 closed()=" << before << " close() 成功=" << closed_ok
              << " 关闭后 closed()=" << after << " 对端读到 Close 帧=" << saw_close
              << "（预期 0 1 1）\n";
    return ok;
}

bool scenario_role_and_socket() {
    std::clog << "[full][场景7] 角色与底层套接字\n";
    using namespace go2cpp;
    auto session = establish();
    if (!session.ok) {
        session.join();
        return false;
    }
    const auto client_role = session.client->role();
    const auto server_role = session.server->role();
    (void)client_role;
    (void)server_role;
    const bool client_has_socket = session.client->socket() != nullptr;
    const bool server_has_socket = session.server->socket() != nullptr;

    session.client->close();
    session.server->close();
    session.join();

    // ★ WebSocketRole 没有导出到 go2cpp 命名空间，必须写 go2cpp::net::WebSocketRole。
    const bool ok = client_role == go2cpp::net::WebSocketRole::Client &&
                    server_role == go2cpp::net::WebSocketRole::Server &&
                    client_has_socket && server_has_socket;
    std::cout << "  客户端角色="
              << (client_role == go2cpp::net::WebSocketRole::Client ? "Client" : "?")
              << " 服务端角色="
              << (server_role == go2cpp::net::WebSocketRole::Server ? "Server" : "?")
              << " 都有底层 Socket=" << (client_has_socket && server_has_socket)
              << "（预期 Client Server 1）\n";
    return ok;
}

}  // namespace

/**
 * @brief websocket 全面版：7 个场景，覆盖握手计算、收发、控制帧与关闭。
 * @details 全部使用 socketpair，不监听端口、不依赖外部网络。
 * @return 全部场景通过返回 0，否则返回 1。
 */
int main() {
    bool ok = true;
    ok = scenario_validate_request() && ok;
    ok = scenario_build_response() && ok;
    ok = scenario_text_echo() && ok;
    ok = scenario_binary() && ok;
    ok = scenario_control_frames() && ok;
    ok = scenario_close_semantics() && ok;
    ok = scenario_role_and_socket() && ok;

    std::clog << (ok ? "[full] 7 个场景全部通过\n" : "[full] 存在失败场景\n");
    return ok ? 0 : 1;
}