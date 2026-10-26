#pragma once

/**
 * @file fault_proxy.hpp
 * @brief 单进程 UDP 故障注入代理：对客户端→服务端的加密 STREAM 包执行
 *        篡改 / 截断 / 重放 / 明文注入，用于验证链路层会拒绝坏包且
 *        可靠流仍能 100% 送达。
 * @details 代理与 UdpProxy 相同：监听临时端口，客户端连代理，代理把包
 *          转发给服务端。只对客户端发往服务端方向、外层 type=STREAM 且
 *          带 kFlagEncrypted 的包按计划注入故障；握手包与 ACK 原样转发。
 *
 *          线程模型：独占一个转发线程；统计为原子计数，主线程可随时读。
 *
 * @author go2cpp contributors
 * @date 2026-10-26
 * @copyright MIT License
 */

#include <atomic>
#include <cstdint>
#include <map>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "go2cpp/bento/net/link.hpp"
#include "go2cpp/bento/net/protocol.hpp"

namespace bento_test {

class FaultProxy final {
   public:
    enum class Fault : std::uint8_t {
        kNone = 0,
        kTamperTag = 1,   ///< 翻转密文帧最后一个 tag 字节。
        kTruncate = 2,    ///< 截断成 16B（结构长度不足 -> malformed）。
        kReplay = 3,      ///< 原包立即转发，副本 50ms 后重放。
        kPlaintext = 4,   ///< 转发原包，并注入一个明文 STREAM 帧。
    };

    FaultProxy(const sockaddr_in& server, std::vector<Fault> plan)
        : m_server(server), m_plan(std::move(plan)), m_buffer(65536) {}

    ~FaultProxy() {
        Stop();
    }

    FaultProxy(const FaultProxy&) = delete;
    FaultProxy& operator=(const FaultProxy&) = delete;

    bool Start(std::string* error) {
        if (!m_socket.Open("127.0.0.1", 0, error)) {
            return false;
        }
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
        return m_socket.local_port();
    }
    std::uint64_t forwarded() const noexcept {
        return m_forwarded.load(std::memory_order_relaxed);
    }
    std::uint64_t tampered() const noexcept {
        return m_tampered.load(std::memory_order_relaxed);
    }
    std::uint64_t truncated() const noexcept {
        return m_truncated.load(std::memory_order_relaxed);
    }
    std::uint64_t replayed() const noexcept {
        return m_replayed.load(std::memory_order_relaxed);
    }
    std::uint64_t injected() const noexcept {
        return m_injected.load(std::memory_order_relaxed);
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
                Forward(node.mapped().destination, node.mapped().data.data(),
                        node.mapped().data.size());
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
        bool to_server = false;
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
                return;
            }
            target = m_server;
            to_server = true;
        }

        if (to_server && bytes >= go2cpp::bento::kFrameHeaderBytes) {
            go2cpp::bento::FrameHeader header;
            if (go2cpp::bento::DecodeHeader(data, bytes, &header) &&
                header.type == go2cpp::bento::FrameType::kStream &&
                (header.flags & go2cpp::bento::kFlagEncrypted) != 0) {
                const std::size_t index = m_stream_index++;
                const Fault fault = index < m_plan.size() ? m_plan[index] : Fault::kNone;
                if (ApplyFault(fault, data, bytes)) {
                    return;
                }
            }
        }
        Forward(target, data, bytes);
    }

    bool ApplyFault(Fault fault, const std::uint8_t* data, std::size_t bytes) {
        switch (fault) {
            case Fault::kNone:
                return false;
            case Fault::kTamperTag: {
                std::vector<std::uint8_t> copy(data, data + bytes);
                if (copy.empty()) {
                    return false;
                }
                copy.back() = static_cast<std::uint8_t>(copy.back() ^ 0x80U);
                Forward(m_server, copy.data(), copy.size());
                m_tampered.fetch_add(1, std::memory_order_relaxed);
                return true;
            }
            case Fault::kTruncate: {
                const std::size_t keep =
                    std::min<std::size_t>(bytes, go2cpp::bento::kFrameHeaderBytes + 4);
                Forward(m_server, data, keep);
                m_truncated.fetch_add(1, std::memory_order_relaxed);
                return true;
            }
            case Fault::kReplay: {
                Forward(m_server, data, bytes);
                if (m_delayed.size() < kMaxDelayedPackets) {
                    DelayedPacket packet;
                    packet.destination = m_server;
                    packet.data.assign(data, data + bytes);
                    m_delayed.emplace(go2cpp::bento::NowMs() + 50, std::move(packet));
                }
                m_replayed.fetch_add(1, std::memory_order_relaxed);
                return true;
            }
            case Fault::kPlaintext: {
                Forward(m_server, data, bytes);
                go2cpp::bento::FrameHeader header;
                header.type = go2cpp::bento::FrameType::kStream;
                header.channel = 0;
                header.flags = 0;
                header.seq = 1;
                const std::uint8_t payload[4] = {'p', 'l', 'a', 'i'};
                const std::vector<std::uint8_t> frame =
                    go2cpp::bento::BuildFrame(header, nullptr, payload, sizeof(payload));
                if (!frame.empty()) {
                    Forward(m_server, frame.data(), frame.size());
                }
                m_injected.fetch_add(1, std::memory_order_relaxed);
                return true;
            }
        }
        return false;
    }

    void Forward(const sockaddr_in& target, const std::uint8_t* data, std::size_t bytes) {
        if (m_socket.SendTo(target, data, bytes)) {
            m_forwarded.fetch_add(1, std::memory_order_relaxed);
        }
    }

    static constexpr std::size_t kMaxDelayedPackets = 256;

    go2cpp::bento::UdpSocket m_socket;
    sockaddr_in m_server{};
    sockaddr_in m_client{};
    bool m_have_client = false;
    std::vector<Fault> m_plan;
    std::size_t m_stream_index = 0;
    std::vector<std::uint8_t> m_buffer;
    std::multimap<std::uint64_t, DelayedPacket> m_delayed;
    std::atomic<bool> m_stop{false};
    std::atomic<std::uint64_t> m_forwarded{0};
    std::atomic<std::uint64_t> m_tampered{0};
    std::atomic<std::uint64_t> m_truncated{0};
    std::atomic<std::uint64_t> m_replayed{0};
    std::atomic<std::uint64_t> m_injected{0};
    std::thread m_thread;
};

}  // namespace bento_test
