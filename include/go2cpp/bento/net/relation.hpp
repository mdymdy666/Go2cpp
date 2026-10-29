#pragma once

/**
 * @file relation.hpp
 * @brief Bento 协议的因果依赖（dep_seq）与屏障（BARRIER）原语。
 * @details 不可靠通道（DATAGRAM）的 seq 水位由 Relation 统一记录。到达的
 *          DATAGRAM 若带 dep_seq 且依赖未满足，就先挂到 pending 队列，
 *          等水位推进或 BARRIER 作废后释放。实现只有两个约束：
 *
 *          1. pending 队列有上限（kMaxPending），满时丢最旧依赖项并计数，
 *             避免对端用永不满足的依赖把接收端内存撑爆；
 *          2. Flush 只执行依赖已就绪的回调，回调内部允许再次调用
 *             AdvanceUnreliable/Flush（由 UnreliableChannel 用重入标志保护）。
 *
 *          BARRIER 的语义是「seq 之前的不可靠数据永久作废」：水位被抬到
 *          barrier_seq，所有依赖 <= barrier_seq 的 pending 项都会释放，
 *          其中自身 seq 已被水位越过的项由通道按 DropStale 丢弃。
 *
 *          SACK 多块扩展（见 protocol.hpp）只作用于可靠通道的重传判定：
 *          扩展块表达的「对端已收到」不会推进这里的 m_watermark，dep_seq
 *          仍只由 AdvanceUnreliable/OnBarrier 移动。两套语义保持独立，
 *          避免可靠通道的乱序缓存影响不可靠流的因果水位。
 *
 * @author go2cpp contributors
 * @date 2026-10-26
 * @copyright MIT License
 */

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>

#include "go2cpp/bento/net/protocol.hpp"

namespace go2cpp::bento {

/// @brief 因果依赖水位与 pending 队列。非线程安全，只在 Link 的单线程循环中使用。
class Relation final {
   public:
    /// pending 回调：由通道捕获自身的 seq/payload，执行真正的交付检查。
    using DeliverFn = std::function<void()>;

    /// pending 上限。超过后丢弃依赖值最小（最旧）的项。
    static constexpr std::size_t kMaxPending = 128;

    Relation() = default;
    ~Relation() = default;

    Relation(const Relation&) = delete;
    Relation& operator=(const Relation&) = delete;

    /// @brief 判断依赖是否已满足。dep_seq == 0 表示无依赖，恒为 true。
    bool IsReady(std::uint32_t dep_seq) const noexcept {
        return dep_seq == 0 || !SeqGreater(dep_seq, m_watermark);
    }

    /// @brief 不可靠 seq 被采纳后推进水位（只增不减，回绕安全）。
    void AdvanceUnreliable(std::uint32_t seq) noexcept {
        if (SeqGreater(seq, m_watermark)) {
            m_watermark = seq;
        }
    }

    /// @brief 挂起一个依赖未满足的交付回调。
    /// @return true 表示成功入队；false 表示 dep 已就绪（调用方应直接交付）。
    bool Enqueue(std::uint32_t dep_seq, DeliverFn deliver);

    /// @brief 执行所有依赖已就绪的回调。
    /// @return 本次执行的回调数。
    std::size_t Flush();

    /// @brief 处理 BARRIER：把水位抬到 barrier_seq，然后释放就绪的 pending。
    /// @return 本次释放的回调数。
    /// @note 不标 noexcept：Flush 会执行用户回调，可能分配内存或抛异常。
    std::size_t OnBarrier(std::uint32_t barrier_seq);

    /// @brief 当前不可靠 seq 水位。
    std::uint32_t watermark() const noexcept {
        return m_watermark;
    }

    /// @brief 当前挂起项数。
    std::size_t pending() const noexcept {
        return m_pending.size();
    }

    /// @brief 因 pending 满而丢弃的项数。
    std::uint64_t dropped() const noexcept {
        return m_dropped;
    }

    /// @brief 清空水位与 pending（测试/重连复用）。
    void Reset() noexcept {
        m_pending.clear();
        m_watermark = 0;
        m_dropped = 0;
    }

   private:
    /// 按依赖值升序排列，Flush 只需看队首；同依赖保持插入顺序。
    std::multimap<std::uint32_t, DeliverFn> m_pending;
    std::uint32_t m_watermark = 0;
    std::uint64_t m_dropped = 0;
};

}  // namespace go2cpp::bento
