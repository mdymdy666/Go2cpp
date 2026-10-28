#pragma once

/**
 * @file memory_transport.hpp
 * @brief `ITransport` 的进程内实现（JinShan MOD-09）：一对配对的内存管道，仅供测试。
 * @author go2cpp contributors
 * @date 2026-10-28
 * @copyright MIT License
 *
 * @details `MemoryTransport::CreatePair()` 返回两个已互相连接的端点，语义与
 *          一条 TCP 连接一致：
 *           - 双向字节流：A 写入的字节由 B 读出，反之亦然；
 *           - 半关闭：A `ShutdownWrite()` 后 B 读到 EOF，但 B 仍可写回 A；
 *           - `Close()` 幂等；关闭后对端读到 EOF，对端写入得到 `kClosed`；
 *           - `Read`/`Write` 支持 deadline 超时（基于 `condition_variable`）。
 *
 * ## 仅测试用
 *
 * 本实现不涉及网络、不做进程间通信，也不提供生产级的容量/流控策略。它的
 * 价值是让上层协议（帧、会话、转发）能在**无 socket、无端口、无权限**的
 * 环境里做确定性测试。**不要**在生产路径里用它替代真实传输：队列无上限，
 * 写入方比读取方快时会一直占用内存。
 *
 * ## 并发模型
 *
 * 两个端点共享一个带 `std::mutex` 的状态，每个方向一个 `std::deque`，用
 * `std::condition_variable` 唤醒等待方。任意一端可以被不同线程同时读写；
 * `Close`/`ShutdownWrite` 也会持锁并广播，因此不会与阻塞中的读端死锁。
 *
 * ## 与 TCP 的语义差异
 *
 * | 行为 | TCP | MemoryTransport |
 * |---|---|---|
 * | 写入缓冲 | 内核有限缓冲，满则 EAGAIN | 无上限 deque，总是全写 |
 * | RST / 连接重置 | 可能发生 | 不存在 |
 * | 地址解析 | 需要 | 忽略 `Endpoint` |
 * | `Connect` | 建立连接 | 已连接，返回 `kOk` |
 *
 * 这些差异是有意的：测试关注的是**上层协议逻辑**，不是内核行为；内核行为由
 * `TcpTransport` 的测试覆盖。
 */

#include <memory>
#include <utility>

#include "go2cpp/bento/net/transport.hpp"

namespace go2cpp::bento::net {

/**
 * @brief 进程内配对传输（仅测试用）。
 *
 * @details 对象只能由 `CreatePair()` 创建，不能单独构造出「半条连接」。
 *          两个端点生命周期独立：一端析构等价于 `Close()`，另一端会读到
 *          EOF；共享状态由 `shared_ptr` 管理，最后一方析构时释放。
 */
class MemoryTransport final : public ITransport {
public:
    /// 一对互相连接的端点（first 为 A 侧，second 为 B 侧）。
    using Pair = std::pair<std::unique_ptr<MemoryTransport>, std::unique_ptr<MemoryTransport>>;

    /**
     * @brief 创建一对已连接的端点。
     * @return `{a, b}`；两端的读写方向互为镜像。
     */
    static Pair CreatePair();

    ~MemoryTransport() override;

    MemoryTransport(const MemoryTransport&) = delete;
    MemoryTransport& operator=(const MemoryTransport&) = delete;
    MemoryTransport(MemoryTransport&&) = delete;
    MemoryTransport& operator=(MemoryTransport&&) = delete;

    /**
     * @brief 空操作式连接：端点创建时已经连接。
     * @param endpoint 忽略（保留接口一致性）。
     * @return 未关闭时 `kOk`；已关闭时 `kClosed`。
     */
    Result Connect(const Endpoint& endpoint, std::chrono::milliseconds timeout) override;

    /// @copydoc ITransport::Read
    Result Read(std::uint8_t* buffer, std::size_t capacity,
                std::chrono::milliseconds timeout) override;

    /**
     * @brief 把字节追加到对端可读队列。
     * @return 成功时 `kOk` 且 `bytes == size`；本端已关闭/已半关闭，或对端
     *         已完全关闭时返回 `kClosed`（errno `EPIPE`/`ENOTCONN`）。
     *
     * @note 队列无上限，正常情况下不会部分写；保留部分写语义只是为了与
     *       `ITransport` 契约一致。
     */
    Result Write(const std::uint8_t* data, std::size_t size,
                 std::chrono::milliseconds timeout) override;

    /// @copydoc ITransport::ShutdownWrite
    Result ShutdownWrite() override;

    /// @copydoc ITransport::Close
    Result Close() override;

private:
    /// 两个端点共享的连接状态；定义在 `memory_transport.cpp`。
    struct SharedState;

    MemoryTransport(std::shared_ptr<SharedState> state, bool side_a) noexcept;

    std::shared_ptr<SharedState> m_state;  ///< 共享队列与关闭标志。
    bool m_side_a = true;                  ///< true = A 侧，false = B 侧。
};

}  // namespace go2cpp::bento::net
