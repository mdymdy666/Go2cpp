/**
 * @file test_tcp_proxy.cpp
 * @brief 回环 TCP 代理的真实场景测试：100KB 回显、半关闭、并发隔离、超时与上限。
 * @details 场景与 JinShan 的对应关系：
 *          - 场景 1/2/4/6/7：Viewer 经本地回环连上「被改写的 invitation」，
 *            代理把字节原样转给远端 RDP 服务；本测试用 echo / 请求-响应
 *            两种后端模拟 RDP 的双向字节流。
 *          - 场景 3：多个 Viewer 各自一条 1:1 管道，断言不串流
 *            （JinShan RISK-030 的反例）。
 *          - 场景 5/6：慢/死连接必须有界退出（JinShan AX-014/AX-020）。
 * @author go2cpp contributors
 * @date 2026-10-27
 * @copyright MIT License
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cerrno>
#include <cstring>
#include <functional>
#include <thread>
#include <vector>

#include "go2cpp/bento/net/tcp_proxy.hpp"
#include "test_util.hpp"

namespace {

using go2cpp::bento::TcpProxy;
using go2cpp::bento::TcpProxyConfig;

constexpr std::size_t kEchoBytes = 100 * 1024;
constexpr std::size_t kConcurrentBytes = 64 * 1024;
constexpr std::size_t kHalfCloseBytes = 32 * 1024;

std::uint8_t PatternByte(std::uint32_t seed, std::size_t index) {
    return static_cast<std::uint8_t>((seed * 131U + index * 17U + (index >> 8U) * 7U) & 0xFFU);
}

std::vector<std::uint8_t> MakePattern(std::uint32_t seed, std::size_t bytes) {
    std::vector<std::uint8_t> data(bytes);
    for (std::size_t i = 0; i < bytes; ++i) {
        data[i] = PatternByte(seed, i);
    }
    return data;
}

int ConnectLocal(std::uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1 ||
        ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

bool SendAll(int fd, const std::uint8_t* data, std::size_t bytes) {
    std::size_t sent = 0;
    while (sent < bytes) {
        const ssize_t written = ::send(fd, data + sent, bytes - sent, MSG_NOSIGNAL);
        if (written > 0) {
            sent += static_cast<std::size_t>(written);
        } else if (written < 0 && (errno == EINTR || errno == EAGAIN)) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

/// 读满 bytes 字节；返回实际读到的字节数（EOF 会提前返回）。
std::size_t RecvAtMost(int fd, std::uint8_t* out, std::size_t bytes) {
    std::size_t received = 0;
    while (received < bytes) {
        const ssize_t got = ::recv(fd, out + received, bytes - received, 0);
        if (got > 0) {
            received += static_cast<std::size_t>(got);
        } else if (got < 0 && errno == EINTR) {
            continue;
        } else {
            break;
        }
    }
    return received;
}

/// 简单 TCP 后端。kEcho：收到什么回什么；kSummary：读到 EOF 后回 8B 长度 + 8B 摘要。
class TestServer final {
   public:
    enum class Mode { kEcho, kSummary };

    explicit TestServer(Mode mode) : m_mode(mode) {
        m_listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (m_listen_fd < 0) {
            return;
        }
        int reuse = 1;
        (void)::setsockopt(m_listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(m_listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
            ::listen(m_listen_fd, 8) != 0) {
            ::close(m_listen_fd);
            m_listen_fd = -1;
            return;
        }
        sockaddr_in actual{};
        socklen_t length = sizeof(actual);
        if (::getsockname(m_listen_fd, reinterpret_cast<sockaddr*>(&actual), &length) == 0) {
            m_port = ntohs(actual.sin_port);
        }
        m_thread = std::thread([this]() { Run(); });
    }

    ~TestServer() {
        Stop();
    }

    TestServer(const TestServer&) = delete;
    TestServer& operator=(const TestServer&) = delete;

    std::uint16_t port() const noexcept {
        return m_port;
    }

    void Stop() {
        m_stop.store(true);
        if (m_listen_fd >= 0) {
            (void)::shutdown(m_listen_fd, SHUT_RDWR);
            (void)::close(m_listen_fd);
            m_listen_fd = -1;
        }
        if (m_thread.joinable()) {
            m_thread.join();
        }
    }

   private:
    void Run() {
        while (!m_stop.load()) {
            sockaddr_in peer{};
            socklen_t length = sizeof(peer);
            const int fd = ::accept(m_listen_fd, reinterpret_cast<sockaddr*>(&peer), &length);
            if (fd < 0) {
                if (m_stop.load()) {
                    return;
                }
                continue;
            }
            // 每条连接独立线程：并发场景需要同时服务多个连接。
            // 捕获 mode 的副本而不是 this，避免 TestServer 析构后线程访问悬空成员。
            const Mode mode = m_mode;
            std::thread([mode, fd]() {
                if (mode == Mode::kEcho) {
                    ServeEcho(fd);
                } else {
                    ServeSummary(fd);
                }
                (void)::close(fd);
            }).detach();
        }
    }

    static void ServeEcho(int fd) {
        std::vector<std::uint8_t> buffer(32 * 1024);
        for (;;) {
            const ssize_t received = ::recv(fd, buffer.data(), buffer.size(), 0);
            if (received <= 0) {
                return;
            }
            std::size_t sent = 0;
            while (sent < static_cast<std::size_t>(received)) {
                const ssize_t written =
                    ::send(fd, buffer.data() + sent,
                           static_cast<std::size_t>(received) - sent, MSG_NOSIGNAL);
                if (written <= 0) {
                    return;
                }
                sent += static_cast<std::size_t>(written);
            }
        }
    }

    static void ServeSummary(int fd) {
        std::vector<std::uint8_t> buffer(32 * 1024);
        std::uint64_t total = 0;
        std::uint64_t digest = 1469598103934665603ULL;  // FNV-1a 64 位
        for (;;) {
            const ssize_t received = ::recv(fd, buffer.data(), buffer.size(), 0);
            if (received <= 0) {
                break;
            }
            for (ssize_t i = 0; i < received; ++i) {
                digest ^= buffer[static_cast<std::size_t>(i)];
                digest *= 1099511628211ULL;
            }
            total += static_cast<std::uint64_t>(received);
        }
        std::uint8_t reply[16];
        for (int i = 0; i < 8; ++i) {
            reply[i] = static_cast<std::uint8_t>(total >> (56 - i * 8));
            reply[8 + i] = static_cast<std::uint8_t>(digest >> (56 - i * 8));
        }
        std::size_t sent = 0;
        while (sent < sizeof(reply)) {
            const ssize_t written = ::send(fd, reply + sent, sizeof(reply) - sent, MSG_NOSIGNAL);
            if (written <= 0) {
                return;
            }
            sent += static_cast<std::size_t>(written);
        }
        (void)::shutdown(fd, SHUT_WR);
    }

    Mode m_mode;
    int m_listen_fd = -1;
    std::uint16_t m_port = 0;
    std::atomic<bool> m_stop{false};
    std::thread m_thread;
};

/// 驱动 proxy 直到 client_finished 或超时。
void DriveProxy(TcpProxy& proxy, const std::atomic<bool>& client_finished, int timeout_ms) {
    const std::uint64_t deadline =
        go2cpp::bento::NowMs() + static_cast<std::uint64_t>(timeout_ms);
    while (go2cpp::bento::NowMs() < deadline && !client_finished.load()) {
        proxy.Poll(2);
    }
    // 收尾：让代理把残留数据/EOF 处理完。
    for (int i = 0; i < 20; ++i) {
        proxy.Poll(1);
    }
}

std::uint64_t ReadU64(const std::uint8_t* in) {
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value = (value << 8) | in[i];
    }
    return value;
}

std::uint64_t Fnv1a(const std::vector<std::uint8_t>& data) {
    std::uint64_t digest = 1469598103934665603ULL;
    for (const std::uint8_t byte : data) {
        digest ^= byte;
        digest *= 1099511628211ULL;
    }
    return digest;
}

/// 场景 1：100KB 双向回显，逐字节一致。
void TestEcho100K() {
    TestServer server(TestServer::Mode::kEcho);
    BENTO_CHECK(server.port() != 0);

    TcpProxyConfig config;
    config.listen_address = "127.0.0.1";
    config.target_address = "127.0.0.1";
    config.target_port = server.port();
    TcpProxy proxy(config);
    std::string error;
    BENTO_CHECK(proxy.Open(&error));
    BENTO_CHECK(proxy.local_port() != 0);

    std::atomic<bool> finished{false};
    std::atomic<bool> ok{false};
    std::thread client([&]() {
        int fd = ConnectLocal(proxy.local_port());
        if (fd < 0) {
            finished.store(true);
            return;
        }
        const std::vector<std::uint8_t> payload = MakePattern(11, kEchoBytes);
        std::vector<std::uint8_t> echoed(kEchoBytes, 0);
        bool good = SendAll(fd, payload.data(), payload.size());
        if (good) {
            good = RecvAtMost(fd, echoed.data(), echoed.size()) == kEchoBytes;
        }
        if (good) {
            good = std::memcmp(payload.data(), echoed.data(), kEchoBytes) == 0;
        }
        ::close(fd);
        ok.store(good);
        finished.store(true);
    });

    DriveProxy(proxy, finished, 15000);
    client.join();
    const auto stats = proxy.Stats();
    BENTO_CHECK(ok.load());
    BENTO_CHECK_EQ(stats.accepted, 1U);
    BENTO_CHECK(stats.dial_ok >= 1);
    BENTO_CHECK(stats.bytes_to_target >= kEchoBytes);
    BENTO_CHECK(stats.bytes_to_client >= kEchoBytes);
    BENTO_CHECK_EQ(stats.dial_failed, 0U);
    // B-02：每条 accept 出来的连接都必须成功设置 TCP_NODELAY。
    BENTO_CHECK_EQ(stats.nodelay_applied, stats.accepted);
    BENTO_CHECK_EQ(stats.nodelay_failed, 0U);
    std::printf("[INFO] echo_100k: to_target=%llu to_client=%llu\n",
                static_cast<unsigned long long>(stats.bytes_to_target),
                static_cast<unsigned long long>(stats.bytes_to_client));
    proxy.Close();
}

/// 场景 2：客户端半关闭后仍能收到后端完整响应（请求-响应协议的真实形态）。
void TestHalfCloseRequestResponse() {
    TestServer server(TestServer::Mode::kSummary);
    BENTO_CHECK(server.port() != 0);

    TcpProxyConfig config;
    config.target_address = "127.0.0.1";
    config.target_port = server.port();
    config.half_close_timeout_ms = 5000;
    TcpProxy proxy(config);
    std::string error;
    BENTO_CHECK(proxy.Open(&error));

    std::atomic<bool> finished{false};
    std::atomic<bool> ok{false};
    std::thread client([&]() {
        int fd = ConnectLocal(proxy.local_port());
        if (fd < 0) {
            finished.store(true);
            return;
        }
        const std::vector<std::uint8_t> payload = MakePattern(22, kHalfCloseBytes);
        bool good = SendAll(fd, payload.data(), payload.size());
        if (good) {
            // 半关闭写方向：后端会读到 EOF 再回响应。
            good = ::shutdown(fd, SHUT_WR) == 0;
        }
        std::uint8_t reply[16] = {};
        if (good) {
            good = RecvAtMost(fd, reply, sizeof(reply)) == sizeof(reply);
        }
        if (good) {
            good = ReadU64(reply) == kHalfCloseBytes && ReadU64(reply + 8) == Fnv1a(payload);
        }
        ::close(fd);
        ok.store(good);
        finished.store(true);
    });

    DriveProxy(proxy, finished, 15000);
    client.join();
    const auto stats = proxy.Stats();
    BENTO_CHECK(ok.load());
    BENTO_CHECK(stats.half_closes >= 1);
    BENTO_CHECK_EQ(stats.idle_timeouts, 0U);
    std::printf("[INFO] half_close: half_closes=%llu to_client=%llu\n",
                static_cast<unsigned long long>(stats.half_closes),
                static_cast<unsigned long long>(stats.bytes_to_client));
    proxy.Close();
}

/// 场景 3：3 条并发连接各自 1:1 管道，数据互不串流。
void TestConcurrentIsolation() {
    TestServer server(TestServer::Mode::kEcho);
    BENTO_CHECK(server.port() != 0);

    TcpProxyConfig config;
    config.target_address = "127.0.0.1";
    config.target_port = server.port();
    config.max_connections = 8;
    TcpProxy proxy(config);
    std::string error;
    BENTO_CHECK(proxy.Open(&error));

    constexpr int kClients = 3;
    std::atomic<int> finished{0};
    std::vector<std::atomic<bool>> results(kClients);
    std::vector<std::thread> clients;
    for (int index = 0; index < kClients; ++index) {
        results[static_cast<std::size_t>(index)].store(false);
        clients.emplace_back([&, index]() {
            int fd = ConnectLocal(proxy.local_port());
            if (fd < 0) {
                finished.fetch_add(1);
                return;
            }
            const std::vector<std::uint8_t> payload =
                MakePattern(100 + static_cast<std::uint32_t>(index), kConcurrentBytes);
            std::vector<std::uint8_t> echoed(kConcurrentBytes, 0);
            bool good = SendAll(fd, payload.data(), payload.size());
            if (good) {
                good = RecvAtMost(fd, echoed.data(), echoed.size()) == kConcurrentBytes;
            }
            if (good) {
                good = std::memcmp(payload.data(), echoed.data(), kConcurrentBytes) == 0;
            }
            ::close(fd);
            results[static_cast<std::size_t>(index)].store(good);
            finished.fetch_add(1);
        });
    }
    const std::uint64_t deadline = go2cpp::bento::NowMs() + 20000;
    while (go2cpp::bento::NowMs() < deadline && finished.load() < kClients) {
        proxy.Poll(2);
    }
    for (int i = 0; i < 20; ++i) {
        proxy.Poll(1);
    }
    for (auto& thread : clients) {
        thread.join();
    }
    for (int index = 0; index < kClients; ++index) {
        BENTO_CHECK(results[static_cast<std::size_t>(index)].load());
    }
    const auto stats = proxy.Stats();
    BENTO_CHECK_EQ(stats.accepted, static_cast<std::uint64_t>(kClients));
    BENTO_CHECK(stats.bytes_to_target >= kConcurrentBytes * kClients);
    std::printf("[INFO] concurrent: connections=%d bytes_to_target=%llu\n", kClients,
                static_cast<unsigned long long>(stats.bytes_to_target));
    proxy.Close();
}

/// 场景 4：目标端口不可达 → 立即失败并计数，客户端读到 EOF，不悬挂。
void TestDialFailure() {
    // 先占一个端口再关掉，确保连接被拒绝（而不是被占用）。
    std::uint16_t dead_port = 0;
    {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
            sockaddr_in actual{};
            socklen_t length = sizeof(actual);
            if (::getsockname(fd, reinterpret_cast<sockaddr*>(&actual), &length) == 0) {
                dead_port = ntohs(actual.sin_port);
            }
        }
        ::close(fd);
    }
    BENTO_CHECK(dead_port != 0);

    TcpProxyConfig config;
    config.target_address = "127.0.0.1";
    config.target_port = dead_port;
    TcpProxy proxy(config);
    std::string error;
    BENTO_CHECK(proxy.Open(&error));

    const int fd = ConnectLocal(proxy.local_port());
    BENTO_CHECK(fd >= 0);
    (void)::shutdown(fd, SHUT_WR);
    std::uint8_t byte = 0;
    for (int i = 0; i < 200; ++i) {
        proxy.Poll(2);
        if (proxy.Stats().dial_failed > 0) {
            break;
        }
    }
    // 代理已关闭该连接，客户端应读到 EOF。
    const ssize_t got = ::recv(fd, &byte, 1, 0);
    BENTO_CHECK(got == 0);
    const auto stats = proxy.Stats();
    BENTO_CHECK(stats.dial_failed >= 1);
    BENTO_CHECK_EQ(stats.bytes_to_target, 0U);
    ::close(fd);
    std::printf("[INFO] dial_failed: count=%llu\n",
                static_cast<unsigned long long>(stats.dial_failed));
    proxy.Close();
}

/// 场景 5：空闲连接在有界时间内被回收（不死耗 fd）。
void TestIdleTimeout() {
    TestServer server(TestServer::Mode::kEcho);
    TcpProxyConfig config;
    config.target_address = "127.0.0.1";
    config.target_port = server.port();
    config.idle_timeout_ms = 250;
    config.half_close_timeout_ms = 0;
    TcpProxy proxy(config);
    std::string error;
    BENTO_CHECK(proxy.Open(&error));

    const int fd = ConnectLocal(proxy.local_port());
    BENTO_CHECK(fd >= 0);
    // 不发数据，等 idle 超时。
    for (int i = 0; i < 300; ++i) {
        proxy.Poll(2);
        if (proxy.Stats().idle_timeouts > 0) {
            break;
        }
    }
    const auto stats = proxy.Stats();
    BENTO_CHECK(stats.idle_timeouts >= 1);
    BENTO_CHECK_EQ(proxy.connection_count(), 0U);
    std::uint8_t byte = 0;
    BENTO_CHECK_EQ(::recv(fd, &byte, 1, 0), 0);
    ::close(fd);
    std::printf("[INFO] idle: timeouts=%llu\n",
                static_cast<unsigned long long>(stats.idle_timeouts));
    proxy.Close();
}

/// 场景 6：连接上限生效，超出者被拒且不影响已有连接。
void TestConnectionLimit() {
    TestServer server(TestServer::Mode::kEcho);
    TcpProxyConfig config;
    config.target_address = "127.0.0.1";
    config.target_port = server.port();
    config.max_connections = 1;
    TcpProxy proxy(config);
    std::string error;
    BENTO_CHECK(proxy.Open(&error));

    const int first = ConnectLocal(proxy.local_port());
    BENTO_CHECK(first >= 0);
    for (int i = 0; i < 50; ++i) {
        proxy.Poll(2);
        if (proxy.connection_count() >= 1) {
            break;
        }
    }
    BENTO_CHECK_EQ(proxy.connection_count(), 1U);

    const int second = ConnectLocal(proxy.local_port());
    BENTO_CHECK(second >= 0);
    for (int i = 0; i < 100; ++i) {
        proxy.Poll(2);
        if (proxy.Stats().rejected > 0) {
            break;
        }
    }
    const auto stats = proxy.Stats();
    BENTO_CHECK(stats.rejected >= 1);
    BENTO_CHECK_EQ(proxy.connection_count(), 1U);
    ::close(first);
    ::close(second);
    proxy.Close();
    std::printf("[INFO] limit: rejected=%llu\n",
                static_cast<unsigned long long>(stats.rejected));
}

/// 场景 7：代理关闭后不再接受连接（Close 幂等）。
void TestCloseIsIdempotent() {
    TcpProxyConfig config;
    config.target_address = "127.0.0.1";
    config.target_port = 9;
    TcpProxy proxy(config);
    std::string error;
    BENTO_CHECK(proxy.Open(&error));
    const std::uint16_t port = proxy.local_port();
    BENTO_CHECK(port != 0);
    proxy.Close();
    proxy.Close();
    BENTO_CHECK(!proxy.is_open());
    BENTO_CHECK_EQ(proxy.Poll(0), 0);
    BENTO_CHECK(proxy.Open(&error));
    BENTO_CHECK(proxy.local_port() != 0);
    proxy.Close();
}

}  // namespace

int main() {
    TestEcho100K();
    TestHalfCloseRequestResponse();
    TestConcurrentIsolation();
    TestDialFailure();
    TestIdleTimeout();
    TestConnectionLimit();
    TestCloseIsIdempotent();
    return bento_test::Finish("test_tcp_proxy");
}
