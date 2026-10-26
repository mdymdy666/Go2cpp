#pragma once

/**
 * @file udp_proxy.hpp
 * @brief 用户态弱网代理：为无 CAP_NET_ADMIN 的环境模拟丢包 + 单向延迟。
 * @details 代理监听 127.0.0.1 的临时端口，客户端连代理，代理把包转发给
 *          服务端。每个方向独立丢弃 loss% 的包，其余包延迟 delay_ms 后
 *          转发。用于在 WSL 无法执行 tc netem 时仍能验证可靠层的重传与
 *          不可靠层的 DropStale。
 *
 *          线程模型：代理独占一个后台线程与一个 UDP socket；主线程只读取
 *          Start() 之后的端口和原子统计。不要在同一 UdpProxy 上调用两次
 *          Start()。
 *
 * @author go2cpp contributors
 * @date 2026-10-26
 * @copyright MIT License
 */

#include <atomic>
#include <cstdint>
#include <map>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "go2cpp/bento/net/link.hpp"

namespace bento_test {

/// @brief 单进程 UDP 弱网代理。
class UdpProxy final {
   public:
    /// 延迟队列上限：超过后按丢包处理，避免弱网下内存无限增长。
    static constexpr std::size_t kMaxDelayedPackets = 4096;

    /// @param server 服务端地址（127.0.0.1:port）。
    /// @param loss_percent 每个方向的丢包百分比（0..100）。
    /// @param delay_ms 每个方向的固定延迟（0 表示立即转发）。
    /// @param seed 丢包随机种子，测试可复现。
    UdpProxy(const sockaddr_in& server, int loss_percent, int delay_ms, std::uint32_t seed)
        : m_server(server),
          m_loss_percent(loss_percent),
          m_delay_ms(delay_ms),
          m_random(seed == 0 ? 0x9E3779B9U : seed),
          m_buffer(65536) {}

    ~UdpProxy() {
        Stop();
    }

    UdpProxy(const UdpProxy&) = delete;
    UdpProxy& operator=(const UdpProxy&) = delete;

    /// @brief 绑定临时端口并启动转发线程。
    bool Start(std::string* error) {
        if (!m_socket.Open("127.0.0.1", 0, error)) {
            return false;
        }
        m_thread = std::thread([this]() { Run(); });
        return true;
    }

    /// @brief 停止并 join 转发线程；幂等。
    void Stop() {
        if (!m_thread.joinable()) {
            return;
        }
        m_stop.store(true, std::memory_order_relaxed);
        m_thread.join();
    }

    std::uint16_t port() const noexcept {
        return m_socket.local_port();
    }
    std::uint64_t forwarded() const noexcept {
        return m_forwarded.load(std::memory_order_relaxed);
    }
    std::uint64_t dropped() const noexcept {
        return m_dropped.load(std::memory_order_relaxed);
    }

   private:
    struct DelayedPacket {
        sockaddr_in destination{};
        std::vector<std::uint8_t> data;
    };

    void Run() {
        while (!m_stop.load(std::memory_order_relaxed)) {
            const std::uint64_t now = go2cpp::bento::NowMs();
            while (!m_delayed.empty() && m_delayed.begin()->first <= now) {
                auto node = m_delayed.extract(m_delayed.begin());
                DelayedPacket& packet = node.mapped();
                if (m_socket.SendTo(packet.destination, packet.data.data(), packet.data.size())) {
                    m_forwarded.fetch_add(1, std::memory_order_relaxed);
                }
            }
            const int ready = m_socket.WaitReadable(1);
            if (ready <= 0) {
                continue;
            }
            for (;;) {
                sockaddr_in from{};
                const int bytes = m_socket.RecvFrom(m_buffer.data(), m_buffer.size(), &from);
                if (bytes <= 0) {
                    break;
                }
                HandlePacket(from, m_buffer.data(), static_cast<std::size_t>(bytes));
            }
        }
    }

    void HandlePacket(const sockaddr_in& from, const std::uint8_t* data, std::size_t bytes) {
        sockaddr_in target{};
        if (go2cpp::bento::UdpSocket::SamePeer(from, m_server)) {
            if (!m_have_client) {
                return;
            }
            target = m_client;
        } else {
            if (!m_have_client) {
                m_client = from;
                m_have_client = true;
            } else if (!go2cpp::bento::UdpSocket::SamePeer(from, m_client)) {
                return;  // 只服务一个客户端，避免被第三方注入
            }
            target = m_server;
        }

        if (m_loss_percent > 0 && static_cast<int>(m_random() % 100U) < m_loss_percent) {
            m_dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (m_delay_ms <= 0) {
            if (m_socket.SendTo(target, data, bytes)) {
                m_forwarded.fetch_add(1, std::memory_order_relaxed);
            } else {
                m_dropped.fetch_add(1, std::memory_order_relaxed);
            }
            return;
        }
        if (m_delayed.size() >= kMaxDelayedPackets) {
            m_dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        DelayedPacket packet;
        packet.destination = target;
        packet.data.assign(data, data + bytes);
        m_delayed.emplace(go2cpp::bento::NowMs() + static_cast<std::uint64_t>(m_delay_ms),
                          std::move(packet));
    }

    go2cpp::bento::UdpSocket m_socket;
    sockaddr_in m_server{};
    sockaddr_in m_client{};
    bool m_have_client = false;
    int m_loss_percent = 0;
    int m_delay_ms = 0;
    std::mt19937 m_random;
    std::vector<std::uint8_t> m_buffer;
    std::multimap<std::uint64_t, DelayedPacket> m_delayed;
    std::atomic<bool> m_stop{false};
    std::atomic<std::uint64_t> m_forwarded{0};
    std::atomic<std::uint64_t> m_dropped{0};
    std::thread m_thread;
};

}  // namespace bento_test
