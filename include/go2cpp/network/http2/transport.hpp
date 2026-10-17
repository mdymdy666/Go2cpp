/**
 * @file transport.hpp
 * @brief HTTP/2 连接层的 IO 抽象。
 *
 * ## 为什么要抽象 IO
 *
 * 连接层里 90% 的代码是**协议逻辑**（握手、帧循环、流表、状态推进），只有
 * 最后一步"从哪读字节、往哪写字节"与环境相关。把这一步抽成 `Transport`，
 * 就能：
 *
 * 1. 用**内存缓冲**在单线程里完整测试连接层，不需要真实 socket、不需要 Fiber；
 * 2. 生产环境换成本项目的 `IOManager` + Fiber 实现，连接层代码一行不改。
 *
 * 这也正是使用者说的「把并发部分改改」——**并发只体现在 Transport 的实现里**，
 * 协议逻辑保持与并发无关。
 *
 * `Read` 的语义必须是**阻塞到至少有一个字节或连接关闭**：这是同步风格的前提。
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace go2cpp::net::http2 {

/** @brief 连接的字节流接口。 */
class Transport {
public:
    Transport() = default;
    virtual ~Transport() = default;
    Transport(const Transport&) = delete;
    Transport& operator=(const Transport&) = delete;

    /**
     * @brief 读取最多 `capacity` 字节。
     * @return 实际读到的字节数；0 表示对端关闭或暂时无数据（调用方应重试或断开）。
     * @note 实现必须**阻塞**到至少一个字节可用或连接关闭，否则上层的同步
     *       循环会忙转。
     */
    virtual std::size_t Read(std::uint8_t* buffer, std::size_t capacity) = 0;

    /**
     * @brief 写入全部 `length` 字节。
     * @return 实际写入的字节数；小于 `length` 表示写失败或对端关闭。
     */
    virtual std::size_t Write(const std::uint8_t* data, std::size_t length) = 0;

    /** @brief 对端是否已关闭（读侧 EOF）。 */
    virtual bool Closed() const noexcept { return false; }
};

/**
 * @brief 内存传输：用于测试与协议回放。
 *
 * 两个方向各有独立缓冲：
 * - `inbound`：对端发来的字节，本端 `Read` 消费它；
 * - `outbound`：本端 `Write` 写入的字节，测试侧重断言它。
 *
 * 典型用法（模拟一个对端）：
 * ```
 * MemoryTransport client_side, server_side;
 * client_side.PushInbound(serverBytes);   // 把"服务端发来的"喂给客户端
 * Connection c(&client_side, Role::kClient, cfg);
 * c.Handshake(nullptr);
 * auto bytes = client_side.TakeOutbound(); // 检查客户端发出的字节
 * ```
 */
class MemoryTransport final : public Transport {
public:
    std::size_t Read(std::uint8_t* buffer, std::size_t capacity) override {
        if (buffer == nullptr || capacity == 0) {
            return 0;
        }
        const std::size_t available = m_inbound.size() - m_read_offset;
        const std::size_t take = (available < capacity) ? available : capacity;
        for (std::size_t i = 0; i < take; ++i) {
            buffer[i] = m_inbound[m_read_offset + i];
        }
        m_read_offset += take;
        return take;
    }

    std::size_t Write(const std::uint8_t* data, std::size_t length) override {
        if (data == nullptr || length == 0) {
            return 0;
        }
        m_outbound.insert(m_outbound.end(), data, data + length);
        return length;
    }

    bool Closed() const noexcept override {
        return m_eof && m_read_offset >= m_inbound.size();
    }

    /** @brief 追加"对端发来"的字节。 */
    void PushInbound(const std::vector<std::uint8_t>& bytes) {
        m_inbound.insert(m_inbound.end(), bytes.begin(), bytes.end());
    }
    void PushInbound(const std::uint8_t* data, std::size_t length) {
        m_inbound.insert(m_inbound.end(), data, data + length);
    }

    /** @brief 标记对端已关闭（读完后 Read 返回 0）。 */
    void MarkEof() noexcept { m_eof = true; }

    /** @brief 取走本端写出的字节（清空 outbound）。 */
    std::vector<std::uint8_t> TakeOutbound() {
        auto out = std::move(m_outbound);
        m_outbound.clear();
        return out;
    }

    /** @brief 已写出的字节数（不清空）。 */
    std::size_t OutboundSize() const noexcept { return m_outbound.size(); }
    /** @brief 还能读到的字节数。 */
    std::size_t InboundRemaining() const noexcept {
        return m_inbound.size() - m_read_offset;
    }

private:
    std::vector<std::uint8_t> m_inbound;
    std::size_t m_read_offset{0};
    std::vector<std::uint8_t> m_outbound;
    bool m_eof{false};
};

}  // namespace go2cpp::net::http2
