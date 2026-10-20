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
 *
 * ## 本文件定义什么
 * - `Transport`：连接的字节流接口（`Read` / `Write` / `Closed`），
 *   是协议层与 IO 之间唯一的缝；
 * - `MemoryTransport`：内存实现，用于单测与协议回放（收发双向独立缓冲）。
 *
 * ## 阅读建议
 * 先读 `Transport` 的三条契约（Read 必须阻塞到至少 1 字节、Write 必须写完、
 * Closed 的语义），再看 `MemoryTransport` 如何用它做测试替身。
 *
 * ## 边界与陷阱
 * - `Read` 返回 0 表示「对端关闭或读失败」，上层统一翻译成 `kClosed`；
 *   实现**不允许**用 0 表示「暂时没数据」，否则上层同步循环会忙转；
 * - `MemoryTransport::Read` 是**非阻塞**的（没数据就返回 0），因此它只适合
 *   「数据已预置」的测试，不能用来模拟等待；
 * - QUIC 复用同一接口，但要求 datagram 语义：一次 `Read` 必须返回一个完整包。
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace go2cpp::net::http2 {

/** @brief 连接的字节流接口。 */
/**
 * @brief 连接的字节流接口。
 *
 * ## 定位
 * 被 `http2::Connection` 以指针持有（构造注入）；实现者包括 `FiberTransport`
 * （生产，Socket + Fiber）、`TlsTransport`（TLS + ALPN）、`MemoryTransport`
 * （测试）以及 QUIC 封装的字节通道。把 IO 抽出来后连接层可脱离网络测试。
 *
 * ## 不变量
 * - `Read` 只在「至少 1 字节可用」或「连接关闭」时返回，绝不忙返回 0；
 * - `Write` 必须写完 `length`，返回小于 `length` 即视为连接不可用；
 * - 实现方不拥有 `buffer`/`data`，不得保存指针；
 * - 所有权：`Connection` 不删除 `Transport`，调用方保证其生命周期更长。
 */
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
     * @brief 带等待上限的 `Read`：最多阻塞 `timeout_ms` 毫秒。
     *
     * @param timeout_ms 等待上限（毫秒）。`0` 表示只做一次非阻塞尝试；
     *                   负数表示不限（与 `Read` 等价）。
     * @return 与 `Read` 相同：>0 读到的字节数；0 表示超时、对端关闭或暂时无数据。
     *
     * ## 为什么需要它
     * `Read` 的契约是「阻塞到有数据或连接关闭」，QUIC 的 `Poll` 若直接调用它，
     * `Send`/`Receive` 的 deadline 就会被一次长时间阻塞击穿（审计 D02：5s 超时
     * 实测 8s 才返回）。协议层在带 deadline 的路径上改调本方法，把剩余预算交给
     * Transport，由 Transport 用 `poll()` / Fiber park-timeout 等机制兑现。
     *
     * ## 默认实现与实现者义务
     * 默认实现忽略超时、直接调用 `Read`，以保持既有实现源码兼容；**凡可能阻塞
     * 的实现（UDP/TLS/Fiber）都应覆盖本方法并遵守 `timeout_ms`**，否则超时语义
     * 仍会退化为旧行为（一次阻塞读可超出 deadline）。`Closed()` 依旧用于区分
     * 「等待超时」与「对端关闭」。
     */
    virtual std::size_t ReadWithTimeout(std::uint8_t* buffer, std::size_t capacity,
                                        int /*timeout_ms*/) {
        return Read(buffer, capacity);
    }

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
 *
 * ## 定位
 * 测试替身：`tests/network/test_http2_*.cpp` 与 fiber/框架测试用它模拟对端
 * （`PushInbound()` 预置「对端发来的」字节，`TakeOutbound()` 检查「我方写出的」）；
 * 生产路径不会构造它。
 *
 * ## 不变量
 * - `m_read_offset <= m_inbound.size()`，读游标只前进；
 * - `Closed()` 只有在 `MarkEof()` 且 inbound 读空之后才为真；
 * - 两个方向完全独立：`Write()` 不影响 `Read()` 的游标。
 */
class MemoryTransport final : public Transport {
public:
    /**
     * @brief 读取最多 capacity 字节（内存实现，不阻塞）。
     *
     * ## 步骤
     * step1. 空指针 / 零容量 -> 返回 0，不推进游标；
     * step2. 取「剩余字节」与 `capacity` 的较小值；
     * step3. 逐字节拷贝并推进 `m_read_offset`（不用 memcpy 以免额外头文件）；
     * step4. 返回实际字节数；缓冲读空即返回 0（此时 `Closed()` 可能仍为假）。
     */
    std::size_t Read(std::uint8_t* buffer, std::size_t capacity) override {
        if (buffer == nullptr || capacity == 0) {
            return 0;
        }
        // step2: 计算还能读多少，并截到 capacity。
        const std::size_t available = m_inbound.size() - m_read_offset;
        const std::size_t take = (available < capacity) ? available : capacity;
        // step3: 拷贝并推进读游标。
        for (std::size_t i = 0; i < take; ++i) {
            buffer[i] = m_inbound[m_read_offset + i];
        }
        m_read_offset += take;
        return take;
    }

    /**
     * @brief 写入全部 length 字节（内存实现不会短写）。
     *
     * ## 步骤
     * step1. 空指针 / 零长度 -> 返回 0，不改变 outbound；
     * step2. 追加到 `m_outbound`，返回 `length`（测试侧用 `TakeOutbound()` 取走）。
     */
    std::size_t Write(const std::uint8_t* data, std::size_t length) override {
        if (data == nullptr || length == 0) {
            return 0;
        }
        // step2: 追加到 outbound；内存实现保证一次写满。
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
    /// 「对端发来」的字节；PushInbound() 追加，Read() 按 m_read_offset 消费。
    std::vector<std::uint8_t> m_inbound;
    /// 读游标；Read() 推进，InboundRemaining()/Closed() 读取。
    std::size_t m_read_offset{0};
    /// 「本端写出」的字节；Write() 追加，TakeOutbound() 取走并清空。
    std::vector<std::uint8_t> m_outbound;
    /// 对端关闭标记；MarkEof() 置位，Closed() 在缓冲读空后才返回真。
    bool m_eof{false};
};

}  // namespace go2cpp::net::http2
