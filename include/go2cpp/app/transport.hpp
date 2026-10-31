#pragma once

/**
 * @file transport.hpp
 * @brief 应用层传输抽象：字节流 + 进程内配对实现。
 *
 * @details `ITransport` 只承诺「有序字节流」这一件事：
 *
 *          - `WriteAll`：写完全部字节，失败返回 false；
 *          - `Read`：最多读 `capacity` 字节，返回 >0=字节数、0=超时、<0=关闭；
 *          - `Close`/`closed`：幂等关闭。
 *
 *          帧边界由上层的 `FrameDecoder` 负责，传输层**不看**字节内容。这与
 *          Bento 的 `ITransport` 是同一套哲学，但刻意不直接复用：应用层需要
 *          一个**零依赖、可在任意线程/协程里实现**的最小接口，Bento 的接口
 *          带连接端点与半关闭语义，套在 UDP 消息通道上并不自然。
 *
 * ## 已提供的实现
 *
 *          - `MemoryTransport`：一对带条件变量的内存管道，用于确定性测试；
 *          - `bento_link_transport.hpp`（可选）：把 Bento Link 的 STREAM 通道
 *            适配成 `ITransport`（UDP 可靠 + 不可靠混合）；
 *          - `http_tunnel.hpp`（可选）：把一批帧塞进 HTTP 请求/响应 body，
 *            用 `bento::HttpServer`（TCP）承载。
 *
 * ## 线程模型
 *
 *          `MemoryTransport` 的两个端点可以各自被一个线程读写；`Read` 与
 *          `WriteAll` 跨线程安全（内部互斥 + 条件变量）。`Close` 会唤醒
 *          所有等待者。`ITransport` 接口本身不规定线程模型，实现必须在
 *          文档里写清楚——这是部署时最容易踩的坑。
 *
 * ## 关闭语义
 *
 *          一对 `MemoryTransport` 中，任意一端 `Close()` 后：
 *          - 该端 `Read` 立即返回 -1；
 *          - 对端仍可读完已入队的数据，随后 `Read` 返回 -1；
 *          - 对端 `WriteAll` 返回 false（数据不会悄悄丢失）。
 *
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace go2cpp::app {

/**
 * @brief 字节流传输抽象。
 *
 * @note 所有方法都**不抛异常**；实现在遇到内部错误时通过返回值报告。
 */
class ITransport {
   public:
    virtual ~ITransport() = default;

    /**
     * @brief 写完全部字节。
     * @return 成功返回 true；对端关闭/写失败返回 false。
     */
    virtual bool WriteAll(const std::uint8_t* data, std::size_t size) = 0;

    /**
     * @brief 读最多 `capacity` 字节。
     * @param data 输出缓冲（不可为 nullptr，除非 capacity 为 0）。
     * @param capacity 缓冲容量。
     * @param timeout_ms 等待预算（0 = 非阻塞尝试一次）。
     * @return >0 实际读取字节数；0 超时/暂无数据；-1 已关闭或错误。
     */
    virtual int Read(std::uint8_t* data, std::size_t capacity,
                     std::uint32_t timeout_ms) = 0;

    /// @brief 关闭（幂等）。
    virtual void Close() = 0;

    /// @brief 是否已关闭。
    virtual bool closed() const = 0;

    /// @brief 连接信息（日志用；可为空）。
    virtual std::string Describe() const { return {}; }
};

/// @brief 传输对象的唯一所有权指针。
using TransportPtr = std::unique_ptr<ITransport>;

/**
 * @brief 进程内配对传输（仅测试/本机管道用）。
 *
 * @details 两端共享一个有界队列（每个方向一个）；队列上限用于模拟 TCP
 *          背压：写满后 `WriteAll` 阻塞等待读端消费，直到超时返回 false。
 *          默认容量 8 MiB，足以让测试不阻塞，又能暴露「只写不读」的死锁。
 */
class MemoryTransport final : public ITransport {
   public:
    /// @brief 一对互相连接的端点。
    using Pair = std::pair<std::unique_ptr<MemoryTransport>,
                           std::unique_ptr<MemoryTransport>>;

    /**
     * @brief 创建一对已连接的端点。
     * @param capacity 每个方向允许积压的最大字节数（0 = 默认 8 MiB）。
     */
    static Pair CreatePair(std::size_t capacity = 0);

    ~MemoryTransport() override;

    MemoryTransport(const MemoryTransport&) = delete;
    MemoryTransport& operator=(const MemoryTransport&) = delete;
    MemoryTransport(MemoryTransport&&) = delete;
    MemoryTransport& operator=(MemoryTransport&&) = delete;

    bool WriteAll(const std::uint8_t* data, std::size_t size) override;
    int Read(std::uint8_t* data, std::size_t capacity,
             std::uint32_t timeout_ms) override;
    void Close() override;
    bool closed() const override;
    std::string Describe() const override;

    /// @brief 本端待读取的字节数。
    std::size_t pending() const;

   private:
    struct Shared;
    MemoryTransport(std::shared_ptr<Shared> inbox, std::shared_ptr<Shared> outbox,
                    bool owner);

    std::shared_ptr<Shared> m_inbox;   ///< 对端写入、本端读取。
    std::shared_ptr<Shared> m_outbox;  ///< 本端写入、对端读取。
    std::atomic<bool> m_closed{false}; ///< 本端是否已关闭。
};

/**
 * @brief 把一段字节按固定大小切块写入传输（用于把帧流喂给消息型通道）。
 * @return 成功写入全部字节返回 true。
 */
bool WriteChunked(ITransport& transport, const std::uint8_t* data, std::size_t size,
                  std::size_t chunk_size);

}  // namespace go2cpp::app
