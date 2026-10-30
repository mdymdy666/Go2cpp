/**
 * @file test_ws_server.cpp
 * @brief bento::WsServer 骨架测试：握手/400 拒绝/文本/二进制/Ping-Pong/Close/并发/优雅停止。
 * @details 客户端分两类：
 *          - `BaseWsClient`：直接复用 base `net::WebSocket::Connect()`，验证端到端协议；
 *          - `RawWsClient`：手写 Upgrade 与 RFC 6455 帧，用来观察 base 自动应答的
 *            Pong 与 Close 帧字节（base 的 read_message 会把 Pong 吞掉）。
 * @author go2cpp contributors
 * @date 2026-10-30
 * @copyright MIT License
 */

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "go2cpp/bento/net/ws_server.hpp"
#include "test_util.hpp"

namespace {

using go2cpp::bento::WsConnection;
using go2cpp::bento::WsServer;
using go2cpp::bento::WsServerConfig;
using go2cpp::net::DialTCP;
using go2cpp::net::Socket;
using go2cpp::net::WebSocket;
using go2cpp::net::WebSocketConfig;
using go2cpp::net::WebSocketMessage;
using go2cpp::net::WebSocketOpcode;
using go2cpp::net::WebSocketResult;

using std::chrono::milliseconds;
using std::chrono::steady_clock;

/// 轮询等待谓词为真；超时返回 false（最后一次检查结果）。
template <typename Predicate>
bool WaitFor(Predicate done, int timeout_ms) {
    const auto deadline = steady_clock::now() + milliseconds(timeout_ms);
    while (steady_clock::now() < deadline) {
        if (done()) {
            return true;
        }
        std::this_thread::sleep_for(milliseconds(2));
    }
    return done();
}

/// 从 Close 帧 payload 取 16 位关闭码；不足 2 字节返回 0。
std::uint16_t CloseCodeOf(const std::string& payload) {
    if (payload.size() < 2) {
        return 0;
    }
    const auto high = static_cast<unsigned>(static_cast<unsigned char>(payload[0]));
    const auto low = static_cast<unsigned>(static_cast<unsigned char>(payload[1]));
    return static_cast<std::uint16_t>((high << 8U) | low);
}

/// base 客户端：直接用 net::WebSocket 完成握手与收发。
class BaseWsClient {
public:
    BaseWsClient() = default;
    ~BaseWsClient() { Reset(); }
    BaseWsClient(const BaseWsClient&) = delete;
    BaseWsClient& operator=(const BaseWsClient&) = delete;

    bool Connect(std::uint16_t port, const std::string& path,
                 milliseconds timeout = milliseconds(5000)) {
        m_socket = DialTCP("127.0.0.1", port, timeout);
        if (!m_socket) {
            return false;
        }
        (void)m_socket->set_recv_timeout(timeout);
        (void)m_socket->set_send_timeout(timeout);
        WebSocketConfig config;
        config.io_timeout = timeout;
        m_websocket = WebSocket::Connect(
            m_socket, "127.0.0.1:" + std::to_string(port), path, config, nullptr);
        return static_cast<bool>(m_websocket);
    }

    bool SendText(std::string_view payload) {
        return m_websocket && m_websocket->send_text(payload).ok();
    }

    bool SendBinary(std::string_view payload) {
        return m_websocket && m_websocket->send_binary(payload).ok();
    }

    bool Read(WebSocketMessage& message) {
        return m_websocket && m_websocket->read_message(message).ok();
    }

    void Reset() {
        if (m_websocket) {
            (void)m_websocket->close(1000);
            m_websocket.reset();
        }
        if (m_socket) {
            (void)m_socket->close();
            m_socket.reset();
        }
    }

private:
    Socket::Ptr m_socket;
    WebSocket::Ptr m_websocket;
};

/// 裸帧客户端：手写 Upgrade 与客户端掩码帧，能观察 base 自动应答的 Pong/Close。
class RawWsClient {
public:
    /// 手写的一帧（只保留测试关心的字段）。
    struct RawFrame {
        bool fin{false};
        WebSocketOpcode opcode{WebSocketOpcode::Text};
        std::string payload;
    };

    RawWsClient() = default;
    ~RawWsClient() { Reset(); }
    RawWsClient(const RawWsClient&) = delete;
    RawWsClient& operator=(const RawWsClient&) = delete;

    bool Handshake(std::uint16_t port, const std::string& path) {
        m_socket = DialTCP("127.0.0.1", port, milliseconds(3000));
        if (!m_socket) {
            return false;
        }
        (void)m_socket->set_recv_timeout(milliseconds(3000));
        (void)m_socket->set_send_timeout(milliseconds(3000));
        const std::string request =
            "GET " + path +
            " HTTP/1.1\r\n"
            "Host: 127.0.0.1:" +
            std::to_string(port) +
            "\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n";
        if (!SendAll(request)) {
            return false;
        }
        std::string response;
        if (!ReadUntilHeaderEnd(response, 4096)) {
            return false;
        }
        return response.rfind("HTTP/1.1 101", 0) == 0;
    }

    /// 发送一帧；客户端帧按 RFC 6455 必须带掩码（固定掩码即可，测试不校验随机性）。
    bool SendFrame(WebSocketOpcode opcode, std::string_view payload, bool fin = true) {
        if (!m_socket) {
            return false;
        }
        std::string frame;
        frame.push_back(static_cast<char>((fin ? 0x80U : 0x00U) |
                                          static_cast<std::uint8_t>(opcode)));
        const std::size_t length = payload.size();
        constexpr std::uint8_t kMaskBit = 0x80U;
        if (length <= 125) {
            frame.push_back(static_cast<char>(kMaskBit | static_cast<std::uint8_t>(length)));
        } else if (length <= 0xFFFFU) {
            frame.push_back(static_cast<char>(kMaskBit | 126U));
            frame.push_back(static_cast<char>((length >> 8U) & 0xFFU));
            frame.push_back(static_cast<char>(length & 0xFFU));
        } else {
            frame.push_back(static_cast<char>(kMaskBit | 127U));
            for (int shift = 56; shift >= 0; shift -= 8) {
                frame.push_back(
                    static_cast<char>((static_cast<std::uint64_t>(length) >> shift) & 0xFFU));
            }
        }
        const std::array<std::uint8_t, 4> mask{0x11U, 0x22U, 0x33U, 0x44U};
        frame.append(reinterpret_cast<const char*>(mask.data()), mask.size());
        for (std::size_t index = 0; index < payload.size(); ++index) {
            frame.push_back(static_cast<char>(static_cast<std::uint8_t>(payload[index]) ^
                                             mask[index % mask.size()]));
        }
        return SendAll(frame);
    }

    /// 读一帧；服务端发来的帧不允许带掩码，这里照常解析以发现协议错误。
    bool ReadFrame(RawFrame& frame) {
        std::array<std::uint8_t, 2> header{};
        if (!ReadExact(header.data(), header.size())) {
            return false;
        }
        frame.fin = (header[0] & 0x80U) != 0;
        frame.opcode = static_cast<WebSocketOpcode>(header[0] & 0x0FU);
        const bool masked = (header[1] & 0x80U) != 0;
        std::uint64_t length = header[1] & 0x7FU;
        if (length == 126U) {
            std::array<std::uint8_t, 2> extended{};
            if (!ReadExact(extended.data(), extended.size())) {
                return false;
            }
            length = (static_cast<std::uint64_t>(extended[0]) << 8U) | extended[1];
        } else if (length == 127U) {
            std::array<std::uint8_t, 8> extended{};
            if (!ReadExact(extended.data(), extended.size())) {
                return false;
            }
            length = 0;
            for (const auto byte : extended) {
                length = (length << 8U) | byte;
            }
        }
        std::array<std::uint8_t, 4> mask{};
        if (masked && !ReadExact(mask.data(), mask.size())) {
            return false;
        }
        frame.payload.resize(static_cast<std::size_t>(length));
        if (length != 0 && !ReadExact(frame.payload.data(), frame.payload.size())) {
            return false;
        }
        if (masked) {
            for (std::size_t index = 0; index < frame.payload.size(); ++index) {
                frame.payload[index] = static_cast<char>(
                    static_cast<std::uint8_t>(frame.payload[index]) ^ mask[index % mask.size()]);
            }
        }
        return true;
    }

    /// 等待对端关闭：recv 返回 0（EOF）或非超时错误即视为已断开。
    bool WaitForEof(int timeout_ms) {
        if (!m_socket) {
            return false;
        }
        (void)m_socket->set_recv_timeout(milliseconds(100));
        const auto deadline = steady_clock::now() + milliseconds(timeout_ms);
        while (steady_clock::now() < deadline) {
            char buffer[64];
            const auto count = m_socket->recv(buffer, sizeof(buffer));
            if (count.transferred == 0) {
                return true;
            }
            if (count.transferred < 0 && count.error != ETIMEDOUT && count.error != EAGAIN) {
                return true;
            }
        }
        return false;
    }

    void Reset() {
        if (m_socket) {
            (void)m_socket->close();
            m_socket.reset();
        }
    }

private:
    bool SendAll(std::string_view data) {
        std::size_t offset = 0;
        while (offset < data.size()) {
            const auto count = m_socket->send(data.data() + offset, data.size() - offset);
            if (count.transferred <= 0) {
                return false;
            }
            offset += static_cast<std::size_t>(count.transferred);
        }
        return true;
    }

    bool ReadExact(void* data, std::size_t size) {
        auto* output = static_cast<std::uint8_t*>(data);
        std::size_t offset = 0;
        while (offset < size) {
            const auto count = m_socket->recv(output + offset, size - offset);
            if (count.transferred <= 0) {
                return false;
            }
            offset += static_cast<std::size_t>(count.transferred);
        }
        return true;
    }

    /// 逐字节读到 `\r\n\r\n`，避免把紧随其后的帧字节一起读进响应。
    bool ReadUntilHeaderEnd(std::string& response, std::size_t limit) {
        while (response.size() < limit) {
            char byte = 0;
            if (!ReadExact(&byte, 1)) {
                return false;
            }
            response.push_back(byte);
            if (response.size() >= 4 &&
                response.compare(response.size() - 4, 4, "\r\n\r\n") == 0) {
                return true;
            }
        }
        return false;
    }

    Socket::Ptr m_socket;
};

/// 读一段 HTTP 响应（到 `\r\n\r\n` 或超时），用于验证 400 拒绝路径。
std::string ReadHttpResponseHints(const Socket::Ptr& socket) {
    std::string response;
    for (;;) {
        char buffer[512];
        const auto count = socket->recv(buffer, sizeof(buffer));
        if (count.transferred <= 0) {
            return response;
        }
        response.append(buffer, static_cast<std::size_t>(count.transferred));
        if (response.find("\r\n\r\n") != std::string::npos || response.size() > 4096) {
            return response;
        }
    }
}

/// 测试用 echo 服务端：记录握手数/路径/消息数，Text/Binary 原样回显。
class EchoServer {
public:
    bool Start(WsServerConfig config = {}) {
        m_server.OnConnection([this](const WsConnection::Ptr& connection) {
            m_connections.fetch_add(1, std::memory_order_acq_rel);
            std::lock_guard<std::mutex> lock(m_mutex);
            m_last_path = connection->path();
            m_last_peer = connection->peer();
        });
        m_server.OnMessage(
            [this](const WsConnection::Ptr& connection, const WebSocketMessage& message) {
                m_messages.fetch_add(1, std::memory_order_acq_rel);
                if (message.opcode == WebSocketOpcode::Text) {
                    (void)connection->SendText(message.payload);
                } else {
                    (void)connection->SendBinary(message.payload);
                }
            });
        if (!m_server.Listen("127.0.0.1:0", config).ok()) {
            return false;
        }
        m_port = m_server.Port();
        return m_server.Start().ok();
    }

    void Stop() { m_server.Stop(); }

    WsServer& server() { return m_server; }
    std::uint16_t port() const { return m_port; }
    int connections() const { return m_connections.load(std::memory_order_acquire); }
    int messages() const { return m_messages.load(std::memory_order_acquire); }

    std::string last_path() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_last_path;
    }
    std::string last_peer() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_last_peer;
    }

private:
    WsServer m_server;
    std::uint16_t m_port{0};
    std::atomic<int> m_connections{0};
    std::atomic<int> m_messages{0};
    mutable std::mutex m_mutex;
    std::string m_last_path;
    std::string m_last_peer;
};

// ---------------------------------------------------------------------------
// 用例
// ---------------------------------------------------------------------------

/// 用例 1：握手成功 + OnConnection 拿到 path/peer + 一条文本往返。
void TestHandshake() {
    EchoServer echo;
    BENTO_CHECK(echo.Start());
    if (echo.port() == 0) {
        return;
    }

    BaseWsClient client;
    BENTO_CHECK(client.Connect(echo.port(), "/chat"));
    BENTO_CHECK(WaitFor([&]() { return echo.connections() == 1; }, 3000));
    BENTO_CHECK_EQ(echo.last_path(), std::string("/chat"));
    BENTO_CHECK(!echo.last_peer().empty());
    BENTO_CHECK(echo.server().Stats().handshakes == 1);

    BENTO_CHECK(client.SendText("hello"));
    WebSocketMessage reply;
    BENTO_CHECK(client.Read(reply));
    BENTO_CHECK(reply.opcode == WebSocketOpcode::Text);
    BENTO_CHECK_EQ(reply.payload, std::string("hello"));

    client.Reset();
    echo.Stop();
    BENTO_CHECK_EQ(echo.server().ActiveConnections(), std::size_t{0});
}

/// 用例 2：普通 HTTP 请求被 400 拒绝，不进入 WebSocket 状态，也不计入握手。
void TestRejectNonWebSocket() {
    EchoServer echo;
    BENTO_CHECK(echo.Start());
    if (echo.port() == 0) {
        return;
    }

    auto socket = DialTCP("127.0.0.1", echo.port(), milliseconds(3000));
    BENTO_CHECK(static_cast<bool>(socket));
    if (socket) {
        (void)socket->set_recv_timeout(milliseconds(3000));
        const std::string request = "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
        const auto sent = socket->send(request.data(), request.size());
        BENTO_CHECK(sent.transferred == static_cast<ssize_t>(request.size()));
        const std::string response = ReadHttpResponseHints(socket);
        BENTO_CHECK(response.rfind("HTTP/1.1 400", 0) == 0);
        (void)socket->close();
    }

    BENTO_CHECK(WaitFor([&]() { return echo.server().Stats().rejected_handshakes >= 1; }, 3000));
    BENTO_CHECK(echo.server().Stats().handshakes == 0);
    BENTO_CHECK(WaitFor([&]() { return echo.server().ActiveConnections() == 0; }, 3000));
    echo.Stop();
}

/// 用例 3：100 条文本消息回环（任务要求的 100 条）。
void TestTextEcho100() {
    EchoServer echo;
    BENTO_CHECK(echo.Start());
    if (echo.port() == 0) {
        return;
    }

    BaseWsClient client;
    BENTO_CHECK(client.Connect(echo.port(), "/echo"));
    constexpr int kMessages = 100;
    int matched = 0;
    bool ok = true;
    for (int index = 0; index < kMessages; ++index) {
        const std::string payload = "msg-" + std::to_string(index);
        WebSocketMessage reply;
        if (!client.SendText(payload) || !client.Read(reply) ||
            reply.opcode != WebSocketOpcode::Text || reply.payload != payload) {
            ok = false;
            break;
        }
        ++matched;
    }
    BENTO_CHECK(ok);
    BENTO_CHECK_EQ(matched, kMessages);
    BENTO_CHECK(echo.server().Stats().received_messages == kMessages);
    BENTO_CHECK_EQ(echo.messages(), kMessages);

    client.Reset();
    echo.Stop();
}

/// 用例 4：二进制消息回环（含 0x00 字节、1KB 负载与空二进制）。
void TestBinaryEcho() {
    EchoServer echo;
    BENTO_CHECK(echo.Start());
    if (echo.port() == 0) {
        return;
    }

    BaseWsClient client;
    BENTO_CHECK(client.Connect(echo.port(), "/binary"));

    std::string payload(1027, '\0');
    for (std::size_t index = 0; index < payload.size(); ++index) {
        payload[index] = static_cast<char>(index % 256U);
    }
    BENTO_CHECK(client.SendBinary(payload));
    WebSocketMessage reply;
    BENTO_CHECK(client.Read(reply));
    BENTO_CHECK(reply.opcode == WebSocketOpcode::Binary);
    BENTO_CHECK_EQ(reply.payload, payload);

    // 空二进制消息也必须原样回环。
    BENTO_CHECK(client.SendBinary(""));
    BENTO_CHECK(client.Read(reply));
    BENTO_CHECK(reply.opcode == WebSocketOpcode::Binary);
    BENTO_CHECK(reply.payload.empty());

    BENTO_CHECK(echo.server().Stats().received_messages == 2);
    client.Reset();
    echo.Stop();
}

/// 用例 5：双向 Ping/Pong。服务端主动 ping，客户端回 pong；客户端再 ping，服务端自动 pong。
void TestPingPong() {
    WsServer server;
    server.OnConnection([](const WsConnection::Ptr& connection) {
        (void)connection->Ping("server-ping");
    });
    server.OnMessage([](const WsConnection::Ptr& connection, const WebSocketMessage& message) {
        if (message.opcode == WebSocketOpcode::Text) {
            (void)connection->SendText(message.payload);
        }
    });
    BENTO_CHECK(server.Listen("127.0.0.1:0").ok());
    BENTO_CHECK(server.Start().ok());
    if (server.Port() == 0) {
        return;
    }

    RawWsClient client;
    BENTO_CHECK(client.Handshake(server.Port(), "/ping"));
    RawWsClient::RawFrame frame;

    // 服务端在 OnConnection 里主动发 Ping。
    BENTO_CHECK(client.ReadFrame(frame));
    BENTO_CHECK(frame.opcode == WebSocketOpcode::Ping);
    BENTO_CHECK_EQ(frame.payload, std::string("server-ping"));
    BENTO_CHECK(client.SendFrame(WebSocketOpcode::Pong, "server-ping"));

    // 客户端发 Ping，base 的 auto_pong 必须回同 payload 的 Pong。
    BENTO_CHECK(client.SendFrame(WebSocketOpcode::Ping, "client-ping"));
    BENTO_CHECK(client.ReadFrame(frame));
    BENTO_CHECK(frame.opcode == WebSocketOpcode::Pong);
    BENTO_CHECK_EQ(frame.payload, std::string("client-ping"));

    // Ping/Pong 之后连接仍可正常收发。
    BENTO_CHECK(client.SendFrame(WebSocketOpcode::Text, "after-ping"));
    BENTO_CHECK(client.ReadFrame(frame));
    BENTO_CHECK(frame.opcode == WebSocketOpcode::Text);
    BENTO_CHECK_EQ(frame.payload, std::string("after-ping"));

    client.Reset();
    server.Stop();
}

/// 用例 6：Close 握手双向验证（客户端发起 / 服务端发起）。
void TestCloseHandshake() {
    // 6a：客户端发 Close，服务端回同 code/reason 的 Close 并断开。
    {
        EchoServer echo;
        BENTO_CHECK(echo.Start());
        if (echo.port() == 0) {
            return;
        }
        RawWsClient client;
        BENTO_CHECK(client.Handshake(echo.port(), "/close"));
        std::string close_payload;
        close_payload.push_back(static_cast<char>(1000 >> 8));
        close_payload.push_back(static_cast<char>(1000 & 0xFF));
        close_payload += "bye";
        BENTO_CHECK(client.SendFrame(WebSocketOpcode::Close, close_payload));

        RawWsClient::RawFrame frame;
        BENTO_CHECK(client.ReadFrame(frame));
        BENTO_CHECK(frame.opcode == WebSocketOpcode::Close);
        BENTO_CHECK_EQ(CloseCodeOf(frame.payload), static_cast<std::uint16_t>(1000));
        BENTO_CHECK_EQ(frame.payload.substr(2), std::string("bye"));
        BENTO_CHECK(client.WaitForEof(3000));
        BENTO_CHECK(WaitFor([&]() { return echo.server().ActiveConnections() == 0; }, 3000));
        echo.Stop();
    }

    // 6b：服务端主动 Close(1001)，客户端读到关闭帧。
    {
        WsServer server;
        server.OnMessage(
            [](const WsConnection::Ptr& connection, const WebSocketMessage& message) {
                if (message.opcode == WebSocketOpcode::Text && message.payload == "shutdown") {
                    (void)connection->Close(1001, "server bye");
                }
            });
        BENTO_CHECK(server.Listen("127.0.0.1:0").ok());
        BENTO_CHECK(server.Start().ok());
        if (server.Port() == 0) {
            return;
        }
        RawWsClient client;
        BENTO_CHECK(client.Handshake(server.Port(), "/shutdown"));
        BENTO_CHECK(client.SendFrame(WebSocketOpcode::Text, "shutdown"));
        RawWsClient::RawFrame frame;
        BENTO_CHECK(client.ReadFrame(frame));
        BENTO_CHECK(frame.opcode == WebSocketOpcode::Close);
        BENTO_CHECK_EQ(CloseCodeOf(frame.payload), static_cast<std::uint16_t>(1001));
        BENTO_CHECK_EQ(frame.payload.substr(2), std::string("server bye"));
        client.Reset();
        server.Stop();
    }
}

/// 用例 7：多客户端并发回环，验证每条连接独立串行、统计准确。
void TestConcurrentClients() {
    EchoServer echo;
    WsServerConfig config;
    config.worker_threads = 8;
    BENTO_CHECK(echo.Start(config));
    if (echo.port() == 0) {
        return;
    }

    constexpr int kClients = 8;
    constexpr int kMessages = 20;
    std::atomic<int> failures{0};
    std::vector<std::thread> clients;
    clients.reserve(kClients);
    for (int client_index = 0; client_index < kClients; ++client_index) {
        clients.emplace_back([&, client_index]() {
            BaseWsClient client;
            if (!client.Connect(echo.port(), "/concurrent")) {
                failures.fetch_add(1, std::memory_order_acq_rel);
                return;
            }
            for (int message_index = 0; message_index < kMessages; ++message_index) {
                const std::string payload = "c" + std::to_string(client_index) + "-m" +
                                            std::to_string(message_index);
                WebSocketMessage reply;
                if (!client.SendText(payload) || !client.Read(reply) ||
                    reply.opcode != WebSocketOpcode::Text || reply.payload != payload) {
                    failures.fetch_add(1, std::memory_order_acq_rel);
                    return;
                }
            }
            client.Reset();
        });
    }
    for (auto& client : clients) {
        client.join();
    }

    BENTO_CHECK_EQ(failures.load(), 0);
    BENTO_CHECK_EQ(echo.connections(), kClients);
    BENTO_CHECK(echo.server().Stats().handshakes == static_cast<std::uint64_t>(kClients));
    BENTO_CHECK(echo.server().Stats().received_messages ==
                static_cast<std::uint64_t>(kClients * kMessages));
    BENTO_CHECK(WaitFor([&]() { return echo.server().ActiveConnections() == 0; }, 3000));
    echo.Stop();
}

/// 用例 8：空闲连接下 Stop 必须快速返回，端口可复用，Stop 幂等。
void TestGracefulStop() {
    EchoServer echo;
    BENTO_CHECK(echo.Start());
    if (echo.port() == 0) {
        return;
    }
    BaseWsClient client;
    BENTO_CHECK(client.Connect(echo.port(), "/idle"));
    BENTO_CHECK(client.SendText("still-here"));
    WebSocketMessage reply;
    BENTO_CHECK(client.Read(reply));
    BENTO_CHECK_EQ(reply.payload, std::string("still-here"));

    const auto begin = steady_clock::now();
    echo.Stop();
    const auto elapsed =
        std::chrono::duration_cast<milliseconds>(steady_clock::now() - begin).count();
    BENTO_CHECK(elapsed < 3000);
    BENTO_CHECK_EQ(echo.server().ActiveConnections(), std::size_t{0});

    // Stop 后客户端看到连接断开（base 的 read_message 返回 Io/Closed）。
    WebSocketMessage message;
    BENTO_CHECK(!client.Read(message));
    client.Reset();

    // 幂等：再次 Stop 不应崩溃或阻塞。
    echo.Stop();

    // Stop 释放端口后可以重新 Listen/Start。
    WsServer& server = echo.server();
    BENTO_CHECK(server.Listen("127.0.0.1:0").ok());
    BENTO_CHECK(server.Start().ok());
    server.Stop();
}

}  // namespace

int main() {
    TestHandshake();
    TestRejectNonWebSocket();
    TestTextEcho100();
    TestBinaryEcho();
    TestPingPong();
    TestCloseHandshake();
    TestConcurrentClients();
    TestGracefulStop();

    std::printf("bento_ws_server: %d checks, %d failures, %d skips\n", bento_test::g_checks,
                bento_test::g_failures, bento_test::g_skips);
    if (bento_test::g_failures != 0) {
        std::fprintf(stderr, "bento_ws_server: FAILED (%d)\n", bento_test::g_failures);
        return 1;
    }
    return 0;
}
