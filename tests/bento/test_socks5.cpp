/**
 * @file test_socks5.cpp
 * @brief bento::socks5 骨架测试：协议编解码 + 真实回环握手/CONNECT/中继/超时。
 * @author go2cpp contributors
 * @date 2026-10-30
 * @copyright MIT License
 *
 * @details 覆盖：
 *          - 协议层：问候与请求的半包/粘包/非法版本/非法命令/未知 ATYP/空域名/
 *            IPv4/域名/IPv6 解析、成功与失败响应编码；
 *          - 服务端：无认证握手、拒绝无 0x00 的客户端、IPv4/域名/IPv6 目标、
 *            CONNECT 成功与失败码、非法握手直接断开、握手超时、256KB 流水线
 *            中继、半关闭传播、6 路并发、优雅 Stop/Close。
 *
 *          所有网络用例只用 127.0.0.1/::1 回环端口（监听端口由内核分配），
 *          不依赖外部网络；IPv6 不可用时该用例记 SKIP（CTest 记为 Skipped）。
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "go2cpp/bento/net/socks5.hpp"
#include "test_util.hpp"

namespace {

using go2cpp::bento::socks5::EncodeSocks5MethodSelection;
using go2cpp::bento::socks5::EncodeSocks5Reply;
using go2cpp::bento::socks5::ParseSocks5Greeting;
using go2cpp::bento::socks5::ParseSocks5Request;
using go2cpp::bento::socks5::Socks5AddressType;
using go2cpp::bento::socks5::Socks5AddressTypeName;
using go2cpp::bento::socks5::Socks5Command;
using go2cpp::bento::socks5::Socks5CommandName;
using go2cpp::bento::socks5::Socks5Greeting;
using go2cpp::bento::socks5::Socks5ParseState;
using go2cpp::bento::socks5::Socks5ReplyCode;
using go2cpp::bento::socks5::Socks5ReplyCodeName;
using go2cpp::bento::socks5::Socks5Request;
using go2cpp::bento::socks5::Socks5Server;
using go2cpp::bento::socks5::Socks5ServerConfig;

using std::chrono::milliseconds;
using std::chrono::steady_clock;

/// 单次阻塞 IO 的超时；足够覆盖 WSL 回环上的调度抖动。
constexpr int kIoTimeoutMs = 3000;

void SetSocketTimeout(int fd, int timeout_ms) {
    timeval timeout{};
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = static_cast<suseconds_t>((timeout_ms % 1000) * 1000);
    (void)::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    (void)::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
}

/// 写满整个缓冲；任何错误返回 false。
bool WriteAll(int fd, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    std::size_t offset = 0;
    while (offset < size) {
        const ssize_t written = ::send(fd, bytes + offset, size - offset, MSG_NOSIGNAL);
        if (written > 0) {
            offset += static_cast<std::size_t>(written);
            continue;
        }
        if (written < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

/// 尽量读满；短读/EOF/超时都会提前返回（返回值即实际字节数）。
std::size_t ReadExact(int fd, void* data, std::size_t size) {
    auto* bytes = static_cast<std::uint8_t*>(data);
    std::size_t offset = 0;
    while (offset < size) {
        const ssize_t count = ::recv(fd, bytes + offset, size - offset, 0);
        if (count > 0) {
            offset += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
    return offset;
}

/// 测试用 SOCKS5 客户端：阻塞 socket + 超时。
class SocksClient {
   public:
    SocksClient() = default;
    ~SocksClient() { Close(); }
    SocksClient(const SocksClient&) = delete;
    SocksClient& operator=(const SocksClient&) = delete;

    bool Connect(std::uint16_t port, int timeout_ms = kIoTimeoutMs) {
        m_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (m_fd < 0) {
            return false;
        }
        SetSocketTimeout(m_fd, timeout_ms);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        return ::connect(m_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
    }

    bool Send(const void* data, std::size_t size) {
        return m_fd >= 0 && WriteAll(m_fd, data, size);
    }
    bool Send(const std::vector<std::uint8_t>& bytes) {
        return Send(bytes.data(), bytes.size());
    }

    std::size_t RecvExact(void* data, std::size_t size) {
        return m_fd >= 0 ? ReadExact(m_fd, data, size) : 0;
    }

    /// 读完整响应（含 BND.ADDR/BND.PORT）；返回原始字节，便于断言线格式。
    bool RecvReply(std::vector<std::uint8_t>& reply) {
        std::uint8_t header[4] = {};
        if (RecvExact(header, sizeof(header)) != sizeof(header)) {
            return false;
        }
        reply.assign(header, header + sizeof(header));
        std::size_t address_length = 0;
        switch (header[3]) {
            case 1:
                address_length = 4;
                break;
            case 4:
                address_length = 16;
                break;
            case 3: {
                std::uint8_t length = 0;
                if (RecvExact(&length, 1) != 1) {
                    return false;
                }
                reply.push_back(length);
                address_length = length;
                break;
            }
            default:
                return false;
        }
        std::vector<std::uint8_t> tail(address_length + 2);
        if (RecvExact(tail.data(), tail.size()) != tail.size()) {
            return false;
        }
        reply.insert(reply.end(), tail.begin(), tail.end());
        return true;
    }

    /// 读到 EOF 返回 true；期间到期的残余数据被丢弃。
    bool WaitForEof(int timeout_ms) {
        if (m_fd < 0) {
            return false;
        }
        SetSocketTimeout(m_fd, 100);
        const auto deadline = steady_clock::now() + milliseconds(timeout_ms);
        std::uint8_t scratch[256];
        while (steady_clock::now() < deadline) {
            const ssize_t count = ::recv(m_fd, scratch, sizeof(scratch), 0);
            if (count == 0) {
                return true;
            }
            if (count > 0) {
                continue;
            }
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            return false;
        }
        return false;
    }

    bool ShutdownWrite() { return m_fd >= 0 && ::shutdown(m_fd, SHUT_WR) == 0; }

    void Close() {
        if (m_fd >= 0) {
            ::close(m_fd);
            m_fd = -1;
        }
    }

   private:
    int m_fd = -1;
};

/// 回环 echo 服务器：每个连接一个 worker，收到什么写回什么。
class EchoServer {
   public:
    ~EchoServer() { Stop(); }
    EchoServer(const EchoServer&) = delete;
    EchoServer& operator=(const EchoServer&) = delete;
    EchoServer() = default;

    bool Start(bool ipv6 = false) {
        m_listen_fd = ::socket(ipv6 ? AF_INET6 : AF_INET, SOCK_STREAM, 0);
        if (m_listen_fd < 0) {
            return false;
        }
        const int yes = 1;
        (void)::setsockopt(m_listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        if (ipv6) {
            sockaddr_in6 address{};
            address.sin6_family = AF_INET6;
            address.sin6_addr = in6addr_loopback;
            address.sin6_port = 0;
            if (::bind(m_listen_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
                ::listen(m_listen_fd, 32) != 0) {
                return false;
            }
            socklen_t length = sizeof(address);
            if (::getsockname(m_listen_fd, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
                return false;
            }
            m_port = ntohs(address.sin6_port);
        } else {
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            address.sin_port = 0;
            if (::bind(m_listen_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
                ::listen(m_listen_fd, 32) != 0) {
                return false;
            }
            socklen_t length = sizeof(address);
            if (::getsockname(m_listen_fd, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
                return false;
            }
            m_port = ntohs(address.sin_port);
        }
        m_thread = std::thread([this]() { AcceptLoop(); });
        return true;
    }

    void Stop() {
        if (m_stopped) {
            return;
        }
        m_stopped = true;
        m_stop.store(true);
        if (m_thread.joinable()) {
            m_thread.join();
        }
        for (auto& worker : m_workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        if (m_listen_fd >= 0) {
            ::close(m_listen_fd);
            m_listen_fd = -1;
        }
    }

    std::uint16_t port() const { return m_port; }

   private:
    void AcceptLoop() {
        while (!m_stop.load()) {
            pollfd entry{};
            entry.fd = m_listen_fd;
            entry.events = POLLIN;
            const int rc = ::poll(&entry, 1, 50);
            if (rc <= 0) {
                continue;
            }
            const int client_fd = ::accept(m_listen_fd, nullptr, nullptr);
            if (client_fd < 0) {
                if (errno == EINTR) {
                    continue;
                }
                break;
            }
            m_workers.emplace_back([this, client_fd]() { Serve(client_fd); });
        }
    }

    void Serve(int fd) {
        // recv 用短超时以便响应 Stop；send 给足时间，避免大块回显被误判超时。
        timeval recv_timeout{};
        recv_timeout.tv_sec = 0;
        recv_timeout.tv_usec = 100 * 1000;
        (void)::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &recv_timeout, sizeof(recv_timeout));
        timeval send_timeout{};
        send_timeout.tv_sec = 2;
        send_timeout.tv_usec = 0;
        (void)::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout));

        std::vector<std::uint8_t> buffer(64 * 1024);
        while (!m_stop.load()) {
            const ssize_t count = ::recv(fd, buffer.data(), buffer.size(), 0);
            if (count == 0) {
                break;
            }
            if (count < 0) {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                    continue;
                }
                break;
            }
            if (!WriteAll(fd, buffer.data(), static_cast<std::size_t>(count))) {
                break;
            }
        }
        ::close(fd);
    }

    int m_listen_fd = -1;
    std::uint16_t m_port = 0;
    std::thread m_thread;
    std::vector<std::thread> m_workers;
    std::atomic<bool> m_stop{false};
    bool m_stopped = false;
};

/// 绑定 127.0.0.1:0 拿到一个端口后立即关闭：用于制造「连接被拒绝」。
std::uint16_t FindClosedPort() {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return 0;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    std::uint16_t port = 0;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
        socklen_t length = sizeof(address);
        if (::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) == 0) {
            port = ntohs(address.sin_port);
        }
    }
    ::close(fd);
    return port;
}

bool Ipv6LoopbackAvailable() {
    const int fd = ::socket(AF_INET6, SOCK_STREAM, 0);
    if (fd < 0) {
        return false;
    }
    sockaddr_in6 address{};
    address.sin6_family = AF_INET6;
    address.sin6_addr = in6addr_loopback;
    address.sin6_port = 0;
    const bool available =
        ::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
    ::close(fd);
    return available;
}

std::vector<std::uint8_t> MakeConnectIpv4(const std::string& host, std::uint16_t port) {
    std::vector<std::uint8_t> request{0x05, 0x01, 0x00, 0x01};
    in_addr address{};
    if (::inet_pton(AF_INET, host.c_str(), &address) != 1) {
        return {};
    }
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(&address);
    request.insert(request.end(), bytes, bytes + 4);
    request.push_back(static_cast<std::uint8_t>((port >> 8) & 0xFF));
    request.push_back(static_cast<std::uint8_t>(port & 0xFF));
    return request;
}

std::vector<std::uint8_t> MakeConnectIpv6(const std::string& host, std::uint16_t port) {
    std::vector<std::uint8_t> request{0x05, 0x01, 0x00, 0x04};
    in6_addr address{};
    if (::inet_pton(AF_INET6, host.c_str(), &address) != 1) {
        return {};
    }
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(&address);
    request.insert(request.end(), bytes, bytes + 16);
    request.push_back(static_cast<std::uint8_t>((port >> 8) & 0xFF));
    request.push_back(static_cast<std::uint8_t>(port & 0xFF));
    return request;
}

std::vector<std::uint8_t> MakeConnectDomain(const std::string& host, std::uint16_t port) {
    std::vector<std::uint8_t> request{0x05, 0x01, 0x00, 0x03};
    request.push_back(static_cast<std::uint8_t>(host.size()));
    request.insert(request.end(), host.begin(), host.end());
    request.push_back(static_cast<std::uint8_t>((port >> 8) & 0xFF));
    request.push_back(static_cast<std::uint8_t>(port & 0xFF));
    return request;
}

/// 握手：仅提供 0x00 之外的方法时返回 false（服务端回 05 FF）。
bool DoHandshake(SocksClient& client, std::vector<std::uint8_t> methods = {0x00}) {
    std::vector<std::uint8_t> greeting{0x05, static_cast<std::uint8_t>(methods.size())};
    greeting.insert(greeting.end(), methods.begin(), methods.end());
    if (!client.Send(greeting)) {
        return false;
    }
    std::uint8_t response[2] = {};
    if (client.RecvExact(response, 2) != 2) {
        return false;
    }
    return response[0] == 0x05 && response[1] == 0x00;
}

bool StartSocksServer(Socks5Server& server) {
    std::string error;
    const bool opened = server.Open(&error);
    BENTO_CHECK(opened);
    if (!opened) {
        std::fprintf(stderr, "[FAIL] socks5 Open: %s\n", error.c_str());
        return false;
    }
    const bool started = server.Start(&error);
    BENTO_CHECK(started);
    if (!started) {
        std::fprintf(stderr, "[FAIL] socks5 Start: %s\n", error.c_str());
        server.Close();
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// 协议层
// ---------------------------------------------------------------------------

void TestProtocolParsing() {
    Socks5Greeting greeting;
    std::uint8_t greeting_bytes[] = {0x05, 0x02, 0x00, 0x02};
    BENTO_CHECK(ParseSocks5Greeting(greeting_bytes, 1, greeting).state ==
                Socks5ParseState::kIncomplete);
    BENTO_CHECK(ParseSocks5Greeting(greeting_bytes, 2, greeting).state ==
                Socks5ParseState::kIncomplete);
    auto greeting_result = ParseSocks5Greeting(greeting_bytes, 4, greeting);
    BENTO_CHECK(greeting_result.state == Socks5ParseState::kComplete);
    BENTO_CHECK_EQ(greeting_result.consumed, static_cast<std::size_t>(4));
    BENTO_CHECK_EQ(greeting.methods.size(), static_cast<std::size_t>(2));
    BENTO_CHECK(greeting.OffersNoAuthentication());

    std::uint8_t bad_version_greeting[] = {0x04, 0x01, 0x00};
    BENTO_CHECK(ParseSocks5Greeting(bad_version_greeting, 3, greeting).state ==
                Socks5ParseState::kError);
    std::uint8_t zero_methods[] = {0x05, 0x00};
    BENTO_CHECK(ParseSocks5Greeting(zero_methods, 2, greeting).state ==
                Socks5ParseState::kError);
    BENTO_CHECK(ParseSocks5Greeting(nullptr, 0, greeting).state == Socks5ParseState::kIncomplete);

    const auto selection = EncodeSocks5MethodSelection(true);
    BENTO_CHECK_EQ(selection.size(), static_cast<std::size_t>(2));
    BENTO_CHECK_EQ(selection[0], 0x05);
    BENTO_CHECK_EQ(selection[1], 0x00);
    BENTO_CHECK_EQ(EncodeSocks5MethodSelection(false)[1], 0xFF);

    // IPv4 请求 + 半包。
    const auto ipv4_request = MakeConnectIpv4("127.0.0.1", 8080);
    BENTO_CHECK_EQ(ipv4_request.size(), static_cast<std::size_t>(10));
    Socks5Request request;
    auto request_result = ParseSocks5Request(ipv4_request.data(), ipv4_request.size() - 1, request);
    BENTO_CHECK(request_result.state == Socks5ParseState::kIncomplete);
    request_result = ParseSocks5Request(ipv4_request.data(), ipv4_request.size(), request);
    BENTO_CHECK(request_result.state == Socks5ParseState::kComplete);
    BENTO_CHECK_EQ(request_result.consumed, static_cast<std::size_t>(10));
    BENTO_CHECK_EQ(request.host, std::string("127.0.0.1"));
    BENTO_CHECK_EQ(request.port, static_cast<std::uint16_t>(8080));
    BENTO_CHECK_EQ(request.Target(), std::string("127.0.0.1:8080"));

    // 域名请求 + 粘包尾巴：只消耗请求本身。
    const auto domain_request = MakeConnectDomain("example.com", 443);
    std::vector<std::uint8_t> sticky = domain_request;
    sticky.push_back(0xAA);
    request_result = ParseSocks5Request(sticky.data(), sticky.size(), request);
    BENTO_CHECK(request_result.state == Socks5ParseState::kComplete);
    BENTO_CHECK_EQ(request_result.consumed, domain_request.size());
    BENTO_CHECK(request.address_type == Socks5AddressType::kDomainName);
    BENTO_CHECK_EQ(request.host, std::string("example.com"));
    BENTO_CHECK_EQ(request.port, static_cast<std::uint16_t>(443));

    // IPv6 请求。
    const auto ipv6_request = MakeConnectIpv6("::1", 80);
    request_result = ParseSocks5Request(ipv6_request.data(), ipv6_request.size(), request);
    BENTO_CHECK(request_result.state == Socks5ParseState::kComplete);
    BENTO_CHECK(request.address_type == Socks5AddressType::kIpv6);
    BENTO_CHECK_EQ(request.host, std::string("::1"));
    BENTO_CHECK_EQ(request.Target(), std::string("[::1]:80"));

    // CMD 不是 CONNECT。
    auto bind_request = ipv4_request;
    bind_request[1] = 0x02;
    request_result = ParseSocks5Request(bind_request.data(), bind_request.size(), request);
    BENTO_CHECK(request_result.state == Socks5ParseState::kError);
    BENTO_CHECK(request_result.error == Socks5ReplyCode::kCommandNotSupported);

    // 未知 ATYP。
    auto bad_atyp = ipv4_request;
    bad_atyp[3] = 0x09;
    request_result = ParseSocks5Request(bad_atyp.data(), bad_atyp.size(), request);
    BENTO_CHECK(request_result.state == Socks5ParseState::kError);
    BENTO_CHECK(request_result.error == Socks5ReplyCode::kAddressTypeNotSupported);

    // RSV 非 0。
    auto bad_rsv = ipv4_request;
    bad_rsv[2] = 0x01;
    request_result = ParseSocks5Request(bad_rsv.data(), bad_rsv.size(), request);
    BENTO_CHECK(request_result.state == Socks5ParseState::kError);
    BENTO_CHECK(request_result.error == Socks5ReplyCode::kGeneralFailure);

    // 空域名：不查 DNS 直接判主机不可达。
    std::uint8_t empty_domain[] = {0x05, 0x01, 0x00, 0x03, 0x00};
    request_result = ParseSocks5Request(empty_domain, 5, request);
    BENTO_CHECK(request_result.state == Socks5ParseState::kError);
    BENTO_CHECK(request_result.error == Socks5ReplyCode::kHostUnreachable);

    // 响应编码：成功 IPv4 = 10 字节，IPv6 = 22 字节。
    std::uint8_t ipv4_address[] = {127, 0, 0, 1};
    const auto success = EncodeSocks5Reply(Socks5ReplyCode::kSucceeded,
                                           Socks5AddressType::kIpv4, ipv4_address, 4, 1234);
    BENTO_CHECK_EQ(success.size(), static_cast<std::size_t>(10));
    BENTO_CHECK_EQ(success[0], 0x05);
    BENTO_CHECK_EQ(success[1], 0x00);
    BENTO_CHECK_EQ(success[2], 0x00);
    BENTO_CHECK_EQ(success[3], 0x01);
    BENTO_CHECK_EQ(success[8], 0x04);
    BENTO_CHECK_EQ(success[9], 0xD2);

    std::uint8_t ipv6_address[16] = {};
    const auto ipv6_reply = EncodeSocks5Reply(Socks5ReplyCode::kSucceeded,
                                              Socks5AddressType::kIpv6, ipv6_address, 16, 443);
    BENTO_CHECK_EQ(ipv6_reply.size(), static_cast<std::size_t>(22));
    BENTO_CHECK_EQ(ipv6_reply[3], 0x04);

    // 失败响应固定 0.0.0.0:0，不泄漏内部地址。
    const auto refused = EncodeSocks5Reply(Socks5ReplyCode::kConnectionRefused,
                                           Socks5AddressType::kIpv4, ipv4_address, 4, 1234);
    BENTO_CHECK_EQ(refused.size(), static_cast<std::size_t>(10));
    BENTO_CHECK_EQ(refused[1], 0x05);
    BENTO_CHECK_EQ(refused[3], 0x01);
    BENTO_CHECK_EQ(refused[4], 0x00);
    BENTO_CHECK_EQ(refused[5], 0x00);
    BENTO_CHECK_EQ(refused[6], 0x00);
    BENTO_CHECK_EQ(refused[7], 0x00);
    BENTO_CHECK_EQ(refused[8], 0x00);
    BENTO_CHECK_EQ(refused[9], 0x00);

    // 成功但地址参数不自洽：保留成功码并降级为 0.0.0.0:0。
    const auto degraded = EncodeSocks5Reply(Socks5ReplyCode::kSucceeded,
                                            Socks5AddressType::kIpv4, nullptr, 0, 0);
    BENTO_CHECK_EQ(degraded.size(), static_cast<std::size_t>(10));
    BENTO_CHECK_EQ(degraded[1], 0x00);
    BENTO_CHECK_EQ(degraded[4], 0x00);

    BENTO_CHECK_EQ(std::string(Socks5ReplyCodeName(Socks5ReplyCode::kConnectionRefused)),
                   std::string("connection refused"));
    BENTO_CHECK_EQ(std::string(Socks5CommandName(Socks5Command::kConnect)),
                   std::string("CONNECT"));
    BENTO_CHECK_EQ(std::string(Socks5AddressTypeName(Socks5AddressType::kDomainName)),
                   std::string("domain"));
    BENTO_CHECK_EQ(std::string(Socks5ReplyCodeName(static_cast<Socks5ReplyCode>(0x7F))),
                   std::string("Unknown"));
}

// ---------------------------------------------------------------------------
// 握手
// ---------------------------------------------------------------------------

void TestGreetingNoAuth() {
    Socks5ServerConfig config;
    config.handshake_timeout_ms = 3000;
    Socks5Server server(config);
    if (!StartSocksServer(server)) {
        return;
    }

    SocksClient client;
    BENTO_CHECK(client.Connect(server.local_port()));
    BENTO_CHECK(DoHandshake(client));

    server.Stop();
    const auto stats = server.Stats();
    BENTO_CHECK_EQ(stats.accepted, static_cast<std::uint64_t>(1));
    BENTO_CHECK_EQ(stats.handshake_ok, static_cast<std::uint64_t>(1));
    server.Close();
}

void TestGreetingRejectsNoAuth() {
    Socks5ServerConfig config;
    config.handshake_timeout_ms = 3000;
    Socks5Server server(config);
    if (!StartSocksServer(server)) {
        return;
    }

    SocksClient client;
    BENTO_CHECK(client.Connect(server.local_port()));
    const std::uint8_t greeting[] = {0x05, 0x01, 0x02};  // 只提供用户名/密码
    BENTO_CHECK(client.Send(greeting, sizeof(greeting)));
    std::uint8_t response[2] = {};
    BENTO_CHECK_EQ(client.RecvExact(response, sizeof(response)), sizeof(response));
    BENTO_CHECK_EQ(response[0], 0x05);
    BENTO_CHECK_EQ(response[1], 0xFF);
    BENTO_CHECK(client.WaitForEof(2000));

    server.Stop();
    const auto stats = server.Stats();
    BENTO_CHECK_EQ(stats.auth_rejected, static_cast<std::uint64_t>(1));
    BENTO_CHECK_EQ(stats.handshake_ok, static_cast<std::uint64_t>(0));
    BENTO_CHECK_EQ(stats.closed, static_cast<std::uint64_t>(1));
    server.Close();
}

// ---------------------------------------------------------------------------
// CONNECT 与中继
// ---------------------------------------------------------------------------

void TestConnectIpv4Echo() {
    EchoServer echo;
    BENTO_CHECK(echo.Start());
    Socks5ServerConfig config;
    config.handshake_timeout_ms = 3000;
    config.connect_timeout_ms = 3000;
    Socks5Server server(config);
    if (!StartSocksServer(server)) {
        echo.Stop();
        return;
    }

    SocksClient client;
    BENTO_CHECK(client.Connect(server.local_port()));
    BENTO_CHECK(DoHandshake(client));
    BENTO_CHECK(client.Send(MakeConnectIpv4("127.0.0.1", echo.port())));
    std::vector<std::uint8_t> reply;
    BENTO_CHECK(client.RecvReply(reply));
    BENTO_CHECK_EQ(reply.size(), static_cast<std::size_t>(10));
    BENTO_CHECK_EQ(reply[1], 0x00);
    BENTO_CHECK_EQ(reply[3], 0x01);
    const std::uint16_t bound_port =
        static_cast<std::uint16_t>((static_cast<std::uint16_t>(reply[8]) << 8) | reply[9]);
    BENTO_CHECK(bound_port != 0);  // BND.PORT 是上游 socket 的本地端口。

    const std::string payload = "hello socks5 over ipv4";
    BENTO_CHECK(client.Send(payload.data(), payload.size()));
    std::string received(payload.size(), '\0');
    BENTO_CHECK_EQ(client.RecvExact(received.data(), received.size()), payload.size());
    BENTO_CHECK_EQ(received, payload);

    server.Stop();
    const auto stats = server.Stats();
    BENTO_CHECK_EQ(stats.connect_ok, static_cast<std::uint64_t>(1));
    BENTO_CHECK(stats.bytes_to_upstream >= payload.size());
    BENTO_CHECK(stats.bytes_to_client >= payload.size());
    server.Close();
    echo.Stop();
}

void TestConnectDomainEcho() {
    EchoServer echo;
    BENTO_CHECK(echo.Start());
    Socks5ServerConfig config;
    config.handshake_timeout_ms = 3000;
    config.connect_timeout_ms = 3000;
    Socks5Server server(config);
    if (!StartSocksServer(server)) {
        echo.Stop();
        return;
    }

    SocksClient client;
    BENTO_CHECK(client.Connect(server.local_port()));
    BENTO_CHECK(DoHandshake(client));
    BENTO_CHECK(client.Send(MakeConnectDomain("localhost", echo.port())));
    std::vector<std::uint8_t> reply;
    BENTO_CHECK(client.RecvReply(reply));
    BENTO_CHECK_EQ(reply[1], 0x00);
    BENTO_CHECK_EQ(reply[3], 0x01);

    const std::string payload = "domain resolution works";
    BENTO_CHECK(client.Send(payload.data(), payload.size()));
    std::string received(payload.size(), '\0');
    BENTO_CHECK_EQ(client.RecvExact(received.data(), received.size()), payload.size());
    BENTO_CHECK_EQ(received, payload);

    server.Stop();
    const auto stats = server.Stats();
    BENTO_CHECK_EQ(stats.connect_ok, static_cast<std::uint64_t>(1));
    server.Close();
    echo.Stop();
}

void TestConnectRefused() {
    const std::uint16_t closed_port = FindClosedPort();
    BENTO_CHECK(closed_port != 0);
    Socks5ServerConfig config;
    config.handshake_timeout_ms = 3000;
    config.connect_timeout_ms = 2000;
    Socks5Server server(config);
    if (!StartSocksServer(server)) {
        return;
    }

    SocksClient client;
    BENTO_CHECK(client.Connect(server.local_port()));
    BENTO_CHECK(DoHandshake(client));
    BENTO_CHECK(client.Send(MakeConnectIpv4("127.0.0.1", closed_port)));
    std::vector<std::uint8_t> reply;
    BENTO_CHECK(client.RecvReply(reply));
    BENTO_CHECK_EQ(reply[1], 0x05);  // connection refused
    BENTO_CHECK(client.WaitForEof(2000));

    server.Stop();
    const auto stats = server.Stats();
    BENTO_CHECK_EQ(stats.connect_failed, static_cast<std::uint64_t>(1));
    BENTO_CHECK_EQ(stats.connect_ok, static_cast<std::uint64_t>(0));
    server.Close();
}

void TestCommandNotSupported() {
    Socks5ServerConfig config;
    config.handshake_timeout_ms = 3000;
    Socks5Server server(config);
    if (!StartSocksServer(server)) {
        return;
    }

    SocksClient client;
    BENTO_CHECK(client.Connect(server.local_port()));
    BENTO_CHECK(DoHandshake(client));
    const std::uint8_t bind_request[] = {0x05, 0x02, 0x00, 0x01, 127, 0, 0, 1, 0x1F, 0x90};
    BENTO_CHECK(client.Send(bind_request, sizeof(bind_request)));
    std::vector<std::uint8_t> reply;
    BENTO_CHECK(client.RecvReply(reply));
    BENTO_CHECK_EQ(reply[1], 0x07);  // command not supported
    BENTO_CHECK(client.WaitForEof(2000));

    server.Stop();
    BENTO_CHECK_EQ(server.Stats().request_errors, static_cast<std::uint64_t>(1));
    server.Close();
}

void TestAddressTypeNotSupported() {
    Socks5ServerConfig config;
    config.handshake_timeout_ms = 3000;
    Socks5Server server(config);
    if (!StartSocksServer(server)) {
        return;
    }

    SocksClient client;
    BENTO_CHECK(client.Connect(server.local_port()));
    BENTO_CHECK(DoHandshake(client));
    const std::uint8_t unsupported_atyp[] = {0x05, 0x01, 0x00, 0x09};
    BENTO_CHECK(client.Send(unsupported_atyp, sizeof(unsupported_atyp)));
    std::vector<std::uint8_t> reply;
    BENTO_CHECK(client.RecvReply(reply));
    BENTO_CHECK_EQ(reply[1], 0x08);  // address type not supported
    BENTO_CHECK(client.WaitForEof(2000));

    server.Stop();
    BENTO_CHECK_EQ(server.Stats().request_errors, static_cast<std::uint64_t>(1));
    server.Close();
}

// ---------------------------------------------------------------------------
// 非法请求与超时
// ---------------------------------------------------------------------------

void TestMalformedHandshake() {
    Socks5ServerConfig config;
    config.handshake_timeout_ms = 3000;
    Socks5Server server(config);
    if (!StartSocksServer(server)) {
        return;
    }

    SocksClient client;
    BENTO_CHECK(client.Connect(server.local_port()));
    const std::uint8_t bad_version[] = {0x04, 0x01, 0x00};
    BENTO_CHECK(client.Send(bad_version, sizeof(bad_version)));
    // 版本非法时直接断开，不写任何响应。
    BENTO_CHECK(client.WaitForEof(2000));

    server.Stop();
    BENTO_CHECK_EQ(server.Stats().handshake_failed, static_cast<std::uint64_t>(1));
    server.Close();
}

void TestHandshakeTimeout() {
    Socks5ServerConfig config;
    config.handshake_timeout_ms = 200;
    config.close_flush_timeout_ms = 200;
    Socks5Server server(config);
    if (!StartSocksServer(server)) {
        return;
    }

    SocksClient client;
    BENTO_CHECK(client.Connect(server.local_port()));
    // 连上但一个字节都不发：服务端必须在握手超时后主动关闭。
    BENTO_CHECK(client.WaitForEof(3000));

    server.Stop();
    const auto stats = server.Stats();
    BENTO_CHECK_EQ(stats.handshake_timeouts, static_cast<std::uint64_t>(1));
    BENTO_CHECK_EQ(stats.handshake_ok, static_cast<std::uint64_t>(0));
    server.Close();
}

// ---------------------------------------------------------------------------
// 大流量、半关闭、并发
// ---------------------------------------------------------------------------

void TestRelayLargeTransfer() {
    EchoServer echo;
    BENTO_CHECK(echo.Start());
    Socks5ServerConfig config;
    config.handshake_timeout_ms = 5000;
    config.connect_timeout_ms = 5000;
    config.idle_timeout_ms = 5000;
    Socks5Server server(config);
    if (!StartSocksServer(server)) {
        echo.Stop();
        return;
    }

    SocksClient client;
    BENTO_CHECK(client.Connect(server.local_port(), 5000));
    BENTO_CHECK(DoHandshake(client));
    BENTO_CHECK(client.Send(MakeConnectIpv4("127.0.0.1", echo.port())));
    std::vector<std::uint8_t> reply;
    BENTO_CHECK(client.RecvReply(reply));
    BENTO_CHECK_EQ(reply[1], 0x00);

    constexpr std::size_t kPayload = 256 * 1024;
    std::vector<std::uint8_t> payload(kPayload);
    for (std::size_t index = 0; index < kPayload; ++index) {
        payload[index] = static_cast<std::uint8_t>((index * 31U + 7U) & 0xFFU);
    }
    // 发送与接收必须同时进行：socks 中继的 pending 缓冲与内核缓冲合计有限，
    // 只发不收会在 64KB 级别卡住（这正是背压，不是 bug）。
    std::atomic<bool> send_ok{false};
    std::thread sender([&client, &payload, &send_ok]() {
        send_ok.store(client.Send(payload));
    });
    std::vector<std::uint8_t> received(kPayload);
    const std::size_t received_bytes = client.RecvExact(received.data(), kPayload);
    sender.join();

    BENTO_CHECK(send_ok.load());
    BENTO_CHECK_EQ(received_bytes, kPayload);
    BENTO_CHECK(std::memcmp(received.data(), payload.data(), kPayload) == 0);

    server.Stop();
    const auto stats = server.Stats();
    BENTO_CHECK(stats.bytes_to_upstream >= kPayload);
    BENTO_CHECK(stats.bytes_to_client >= kPayload);
    server.Close();
    echo.Stop();
}

void TestHalfClose() {
    EchoServer echo;
    BENTO_CHECK(echo.Start());
    Socks5ServerConfig config;
    config.handshake_timeout_ms = 3000;
    config.connect_timeout_ms = 3000;
    config.idle_timeout_ms = 3000;
    Socks5Server server(config);
    if (!StartSocksServer(server)) {
        echo.Stop();
        return;
    }

    SocksClient client;
    BENTO_CHECK(client.Connect(server.local_port()));
    BENTO_CHECK(DoHandshake(client));
    BENTO_CHECK(client.Send(MakeConnectIpv4("127.0.0.1", echo.port())));
    std::vector<std::uint8_t> reply;
    BENTO_CHECK(client.RecvReply(reply));
    BENTO_CHECK_EQ(reply[1], 0x00);

    const std::string payload = "half-close-payload";
    BENTO_CHECK(client.Send(payload.data(), payload.size()));
    BENTO_CHECK(client.ShutdownWrite());  // 发 FIN，但仍期望读到回显

    std::string received(payload.size(), '\0');
    BENTO_CHECK_EQ(client.RecvExact(received.data(), received.size()), payload.size());
    BENTO_CHECK_EQ(received, payload);
    // 上游 echo 收到 FIN 后关闭；中继传播 EOF，客户端随后读到 EOF。
    BENTO_CHECK(client.WaitForEof(3000));

    server.Stop();
    server.Close();
    echo.Stop();
}

void TestConcurrentRelays() {
    EchoServer echo;
    BENTO_CHECK(echo.Start());
    Socks5ServerConfig config;
    config.handshake_timeout_ms = 5000;
    config.connect_timeout_ms = 5000;
    config.idle_timeout_ms = 5000;
    config.max_connections = 32;
    Socks5Server server(config);
    if (!StartSocksServer(server)) {
        echo.Stop();
        return;
    }

    constexpr int kClients = 6;
    constexpr std::size_t kPayload = 16 * 1024;
    std::atomic<int> succeeded{0};
    std::vector<std::thread> clients;
    clients.reserve(kClients);
    for (int index = 0; index < kClients; ++index) {
        clients.emplace_back([&server, &echo, &succeeded, index]() {
            std::vector<std::uint8_t> payload(kPayload);
            for (std::size_t offset = 0; offset < kPayload; ++offset) {
                payload[offset] =
                    static_cast<std::uint8_t>((offset * 13U + static_cast<std::size_t>(index)) &
                                              0xFFU);
            }
            SocksClient client;
            if (!client.Connect(server.local_port(), 5000)) {
                return;
            }
            if (!DoHandshake(client)) {
                return;
            }
            if (!client.Send(MakeConnectIpv4("127.0.0.1", echo.port()))) {
                return;
            }
            std::vector<std::uint8_t> reply;
            if (!client.RecvReply(reply) || reply[1] != 0x00) {
                return;
            }
            if (!client.Send(payload)) {
                return;
            }
            std::vector<std::uint8_t> received(kPayload);
            if (client.RecvExact(received.data(), kPayload) != kPayload) {
                return;
            }
            if (std::memcmp(received.data(), payload.data(), kPayload) != 0) {
                return;
            }
            succeeded.fetch_add(1);
        });
    }
    for (auto& thread : clients) {
        thread.join();
    }
    BENTO_CHECK_EQ(succeeded.load(), kClients);

    server.Stop();
    const auto stats = server.Stats();
    BENTO_CHECK(stats.accepted >= static_cast<std::uint64_t>(kClients));
    BENTO_CHECK_EQ(stats.connect_ok, static_cast<std::uint64_t>(kClients));
    server.Close();
    echo.Stop();
}

// ---------------------------------------------------------------------------
// IPv6（环境不支持时 SKIP）
// ---------------------------------------------------------------------------

void TestIpv6Echo() {
    if (!Ipv6LoopbackAvailable()) {
        bento_test::Skip("IPv6 loopback (::1) is not available");
        return;
    }
    EchoServer echo;
    BENTO_CHECK(echo.Start(true));
    Socks5ServerConfig config;
    config.handshake_timeout_ms = 3000;
    config.connect_timeout_ms = 3000;
    Socks5Server server(config);
    if (!StartSocksServer(server)) {
        echo.Stop();
        return;
    }

    SocksClient client;
    BENTO_CHECK(client.Connect(server.local_port()));
    BENTO_CHECK(DoHandshake(client));
    BENTO_CHECK(client.Send(MakeConnectIpv6("::1", echo.port())));
    std::vector<std::uint8_t> reply;
    BENTO_CHECK(client.RecvReply(reply));
    BENTO_CHECK_EQ(reply[1], 0x00);
    // 上游是 IPv6，BND.ADDR 必须是 16 字节 IPv6（总长 22）。
    BENTO_CHECK_EQ(reply.size(), static_cast<std::size_t>(22));
    BENTO_CHECK_EQ(reply[3], 0x04);

    const std::string payload = "ipv6 relay";
    BENTO_CHECK(client.Send(payload.data(), payload.size()));
    std::string received(payload.size(), '\0');
    BENTO_CHECK_EQ(client.RecvExact(received.data(), received.size()), payload.size());
    BENTO_CHECK_EQ(received, payload);

    server.Stop();
    BENTO_CHECK_EQ(server.Stats().connect_ok, static_cast<std::uint64_t>(1));
    server.Close();
    echo.Stop();
}

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------

void TestGracefulStopAndClose() {
    Socks5ServerConfig config;
    Socks5Server server(config);
    std::string error;
    BENTO_CHECK(server.Open(&error));
    const std::uint16_t port = server.local_port();
    BENTO_CHECK(port != 0);
    BENTO_CHECK(server.Start(&error));
    BENTO_CHECK(server.Start(&error) == false);  // 已在运行
    server.Stop();
    server.Stop();  // 幂等
    server.Close();
    server.Close();  // 幂等
    BENTO_CHECK(!server.is_open());
    BENTO_CHECK_EQ(server.connection_count(), static_cast<std::size_t>(0));

    // 端口已释放：同端口能重新监听。
    Socks5ServerConfig reopened;
    reopened.listen_port = port;
    Socks5Server second(reopened);
    BENTO_CHECK(second.Open(&error));
    BENTO_CHECK_EQ(second.local_port(), port);
    second.Close();
}

}  // namespace

int main() {
    TestProtocolParsing();
    TestGreetingNoAuth();
    TestGreetingRejectsNoAuth();
    TestConnectIpv4Echo();
    TestConnectDomainEcho();
    TestConnectRefused();
    TestCommandNotSupported();
    TestAddressTypeNotSupported();
    TestMalformedHandshake();
    TestHandshakeTimeout();
    TestRelayLargeTransfer();
    TestHalfClose();
    TestConcurrentRelays();
    TestIpv6Echo();
    TestGracefulStopAndClose();
    return bento_test::Finish("test_socks5");
}
