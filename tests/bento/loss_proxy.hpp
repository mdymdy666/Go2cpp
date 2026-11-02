#pragma once

/**
 * @file loss_proxy.hpp
 * @brief 单进程双向 UDP 丢包代理：按固定概率丢弃两个方向的包，用于验证
 *        FEC/重传在弱网下的行为。
 * @details 与 `fault_proxy.hpp` 的区别：那个代理做「定向篡改/重放」，这里做
 *          **随机丢包**（可复现的 xorshift 序列），并且双向都丢。
 *
 *          拓扑：
 *            client --(front socket)--> proxy --(back socket)--> server
 *            client <--(front socket)-- proxy <--(back socket)-- server
 *          front 的端口就是 `port()`；server 看到的源地址是 back 的端口，
 *          因此回包会回到代理，再由代理转发给最近一次发来数据的客户端。
 *
 *          线程模型：独占一个转发线程，poll 两个 fd；统计是普通成员，
 *          只在转发线程写、测试线程在 Stop() 之后读（或读原子量）。
 *
 * @author go2cpp contributors
 * @date 2026-11-02
 * @copyright MIT License
 */

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>

namespace bento_test {

class LossProxy final {
   public:
    /// @param server_port 真实服务端端口（127.0.0.1）。
    /// @param loss_rate 每个方向的丢包概率（0..1）。
    /// @param seed 丢包序列种子（0 用默认值），同一 seed 可复现。
    LossProxy(std::uint16_t server_port, double loss_rate, std::uint32_t seed)
        : m_loss(loss_rate), m_rng(seed != 0 ? seed : 0x9E3779B9u) {
        std::memset(&m_server, 0, sizeof(m_server));
        m_server.sin_family = AF_INET;
        m_server.sin_port = htons(server_port);
        m_server.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    }

    ~LossProxy() {
        Stop();
    }

    LossProxy(const LossProxy&) = delete;
    LossProxy& operator=(const LossProxy&) = delete;

    bool Start(std::string* error) {
        m_front = MakeSocket(error);
        if (m_front < 0) {
            return false;
        }
        m_back = MakeSocket(error);
        if (m_back < 0) {
            return false;
        }
        sockaddr_in local{};
        socklen_t len = sizeof(local);
        if (::getsockname(m_front, reinterpret_cast<sockaddr*>(&local), &len) != 0) {
            if (error != nullptr) {
                *error = "LossProxy getsockname failed";
            }
            return false;
        }
        m_port = ntohs(local.sin_port);
        m_thread = std::thread([this]() { Run(); });
        return true;
    }

    void Stop() {
        if (!m_thread.joinable()) {
            return;
        }
        m_stop.store(true, std::memory_order_relaxed);
        m_thread.join();
    }

    std::uint16_t port() const noexcept {
        return m_port;
    }
    std::uint64_t forwarded() const noexcept {
        return m_forwarded.load(std::memory_order_relaxed);
    }
    std::uint64_t dropped() const noexcept {
        return m_dropped.load(std::memory_order_relaxed);
    }

   private:
    static int MakeSocket(std::string* error) {
        const int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0) {
            if (error != nullptr) {
                *error = "LossProxy socket failed";
            }
            return -1;
        }
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_port = 0;
        local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(fd, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
            if (error != nullptr) {
                *error = "LossProxy bind failed";
            }
            ::close(fd);
            return -1;
        }
        return fd;
    }

    bool Drop() {
        m_rng ^= m_rng << 13;
        m_rng ^= m_rng >> 7;
        m_rng ^= m_rng << 17;
        const double r = static_cast<double>(m_rng % 1000000u) / 1000000.0;
        return r < m_loss;
    }

    void Run() {
        pollfd fds[2] = {{m_front, POLLIN, 0}, {m_back, POLLIN, 0}};
        std::uint8_t buffer[2048];
        while (!m_stop.load(std::memory_order_relaxed)) {
            const int ready = ::poll(fds, 2, 20);
            if (ready <= 0) {
                continue;
            }
            if ((fds[0].revents & POLLIN) != 0) {
                Relay(m_front, m_back, buffer, true);
            }
            if ((fds[1].revents & POLLIN) != 0) {
                Relay(m_back, m_front, buffer, false);
            }
        }
    }

    void Relay(int in, int out, std::uint8_t* buffer, bool from_client) {
        for (;;) {
            sockaddr_in from{};
            socklen_t from_len = sizeof(from);
            const ssize_t n = ::recvfrom(in, buffer, 2048, MSG_DONTWAIT,
                                         reinterpret_cast<sockaddr*>(&from), &from_len);
            if (n <= 0) {
                return;
            }
            if (from_client) {
                m_client = from;
                m_have_client = true;
            } else if (!m_have_client) {
                continue;
            }
            if (Drop()) {
                m_dropped.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            const sockaddr_in& to = from_client ? m_server : m_client;
            (void)::sendto(out, buffer, static_cast<std::size_t>(n), 0,
                           reinterpret_cast<const sockaddr*>(&to), sizeof(to));
            m_forwarded.fetch_add(1, std::memory_order_relaxed);
        }
    }

    double m_loss = 0.0;
    std::uint32_t m_rng = 0x9E3779B9u;
    int m_front = -1;
    int m_back = -1;
    std::uint16_t m_port = 0;
    sockaddr_in m_server{};
    sockaddr_in m_client{};
    bool m_have_client = false;
    std::thread m_thread;
    std::atomic<bool> m_stop{false};
    std::atomic<std::uint64_t> m_forwarded{0};
    std::atomic<std::uint64_t> m_dropped{0};
};

}  // namespace bento_test
