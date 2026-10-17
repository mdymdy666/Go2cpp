/**
 * @file picoquic_hq_client.cpp
 * @brief 用薄封装连本地 picoquic server，验证握手 + 收发（hq-interop 协议）。
 *
 * 这个 example 同时是 `PicoquicConnection` 的**用法示范**与**端到端验证**：
 * 1. 用 `UdpTransport` 提供 datagram 语义的 `Transport`
 * 2. `Connect` 建立连接
 * 3. 反复 `Poll` 直到 `HandshakeComplete`
 * 4. `WriteStream` 发 HTTP/0.9 请求
 * 5. `Poll` + `ReadStream` 收响应
 *
 * **前置**：本地已起一个 picoquic server，例如
 * ```
 * picoquicdemo -p 14433 -c cert.pem -k key.pem -w /path/to/www
 * ```
 */

#include "go2cpp/network/http2/transport.hpp"
#include "go2cpp/network/quic/picoquic_connection.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

/**
 * @brief UDP 版 `Transport` —— 每个 `Read` 返回**一个** datagram。
 *
 * 这是 QUIC 的硬要求：QUIC 基于 UDP，每一"包"必须保持边界，不能像 TCP 那样
 * 把多段数据合并。所以这里用 `SOCK_DGRAM`，且 `Read` 一次只调一次 `recv`。
 */
class UdpTransport : public go2cpp::net::http2::Transport {
public:
    ~UdpTransport() override {
        if (m_socket >= 0) {
            ::close(m_socket);
        }
    }

    bool Open(const char* host, int port, int timeout_ms) {
        m_socket = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (m_socket < 0) {
            std::printf("[example] socket failed\n");
            return false;
        }
        struct timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
        ::setsockopt(m_socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        std::memset(&m_peer, 0, sizeof(m_peer));
        m_peer.sin_family = AF_INET;
        m_peer.sin_port = htons(static_cast<uint16_t>(port));
        if (inet_pton(AF_INET, host, &m_peer.sin_addr) != 1) {
            std::printf("[example] bad host\n");
            return false;
        }
        if (::connect(m_socket, reinterpret_cast<struct sockaddr*>(&m_peer),
                      sizeof(m_peer)) != 0) {
            std::printf("[example] connect failed\n");
            return false;
        }
        return true;
    }

    std::size_t Read(std::uint8_t* buffer, std::size_t capacity) override {
        const ssize_t n = ::recv(m_socket, buffer, capacity, 0);
        if (n < 0) {
            return 0;  // 超时也算"暂时没数据"，由上层决定是否继续
        }
        return static_cast<std::size_t>(n);
    }

    std::size_t Write(const std::uint8_t* data, std::size_t length) override {
        const ssize_t n = ::send(m_socket, data, length, 0);
        return n < 0 ? 0 : static_cast<std::size_t>(n);
    }

private:
    int m_socket{-1};
    struct sockaddr_in m_peer {};
};

}  // namespace

int main(int argc, char** argv) {
    const char* host = (argc > 1) ? argv[1] : "127.0.0.1";
    const int port = (argc > 2) ? std::atoi(argv[2]) : 14433;
    const char* alpn = (argc > 3) ? argv[3] : "hq-interop";

    UdpTransport transport;
    if (!transport.Open(host, port, /*timeout_ms=*/50)) {
        return 1;
    }

    go2cpp::net::quic::PicoquicConfig config;
    config.alpn = alpn;
    config.sni = host;
    config.verify_peer = false;  // 测试环境自签名证书

    std::string error;
    auto connection = go2cpp::net::quic::PicoquicConnection::Connect(&transport, config, &error);
    if (connection == nullptr) {
        std::printf("[example] Connect failed: %s\n", error.c_str());
        return 1;
    }
    std::printf("[example] connection created\n");

    // 1) 驱动握手（最多 500 次 Poll，每次最多阻塞 50ms）
    for (int i = 0; i < 500 && !connection->HandshakeComplete(); ++i) {
        if (!connection->Poll(&error)) {
            std::printf("[example] Poll failed during handshake: %s\n", error.c_str());
            return 1;
        }
    }
    if (!connection->HandshakeComplete()) {
        std::printf("[example] handshake timeout\n");
        return 1;
    }
    std::printf("[example] handshake complete\n");

    // 2) 发 HTTP/0.9 请求（hq-interop）
    const char* request = "GET /index.html\r\n";
    const std::size_t sent = connection->WriteStream(
        0, reinterpret_cast<const std::uint8_t*>(request), std::strlen(request),
        /*fin=*/true, &error);
    if (sent == 0) {
        std::printf("[example] WriteStream failed: %s\n", error.c_str());
        return 1;
    }
    std::printf("[example] request sent (%zu bytes)\n", sent);

    // 3) 收响应
    std::uint8_t buffer[4096];
    std::string body;
    for (int i = 0; i < 500 && body.empty(); ++i) {
        if (!connection->Poll(&error)) {
            std::printf("[example] Poll failed while waiting data: %s\n", error.c_str());
            break;
        }
        const std::size_t n = connection->ReadStream(0, buffer, sizeof(buffer));
        if (n > 0) {
            body.append(reinterpret_cast<const char*>(buffer), n);
        }
    }

    if (body.empty()) {
        std::printf("[example] no data received\n");
        return 1;
    }
    std::printf("[example] received %zu bytes:\n%s\n", body.size(), body.c_str());

    const bool ok = body.find("hello from picoquic") != std::string::npos;
    std::printf("[example] === %s ===\n", ok ? "PASS" : "FAIL (content mismatch)");
    return ok ? 0 : 1;
}
