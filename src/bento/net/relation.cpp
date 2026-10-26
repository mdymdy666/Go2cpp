/**
 * @file relation.cpp
 * @brief 因果依赖水位与 pending 队列实现。
 * @author go2cpp contributors
 * @date 2026-10-26
 * @copyright MIT License
 */

#include "go2cpp/bento/net/relation.hpp"

#include <utility>

namespace go2cpp::bento {

bool Relation::Enqueue(std::uint32_t dep_seq, DeliverFn deliver) {
    if (!deliver) {
        return false;
    }
    if (IsReady(dep_seq)) {
        return false;
    }
    if (m_pending.size() >= kMaxPending) {
        // 丢最旧（依赖值最小）的一项：它最不可能在近期满足，且保留新项
        // 更符合「尽快跟上发送端最新状态」的实时语义。
        m_pending.erase(m_pending.begin());
        ++m_dropped;
    }
    m_pending.emplace(dep_seq, std::move(deliver));
    return true;
}

std::size_t Relation::Flush() {
    std::size_t executed = 0;
    while (!m_pending.empty()) {
        auto it = m_pending.begin();
        if (!IsReady(it->first)) {
            break;
        }
        // 先摘出再执行：回调里可能再次 Enqueue/Flush，不能持有迭代器。
        DeliverFn deliver = std::move(it->second);
        m_pending.erase(it);
        deliver();
        ++executed;
    }
    return executed;
}

std::size_t Relation::OnBarrier(std::uint32_t barrier_seq) {
    AdvanceUnreliable(barrier_seq);
    return Flush();
}

}  // namespace go2cpp::bento
