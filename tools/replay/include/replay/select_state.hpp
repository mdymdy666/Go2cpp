#pragma once

/**
 * @file select_state.hpp
 * @brief select 的共享状态与组锁。
 *
 * ## 为什么需要组锁
 *
 * Go 的 select 要**原子地**在多个 channel 上「轮询 + 登记等待」。如果每个
 * channel 各用一把锁，select 就必须按固定顺序同时持有 N 把锁，并且唤醒方
 * 还要跨锁撤销其它 channel 上的登记 —— 这是 Go runtime 里最复杂的部分之一。
 *
 * 本实现选择更小、可验证的边界：**参与同一个 select 的 channel 必须绑定
 * 同一个 `SelectGroup`**，组内所有 channel 操作（普通 send/recv/close 与
 * select）共用这一把组锁。语义等价，代价是组内操作串行化（demo 规模足够；
 * 文档 §限制 明确声明）。
 *
 * 不在任何 group 里的 channel 仍使用自己的 `m_mutex`，行为与 v1 完全一致。
 */

#include "go2cpp/concurrency/core/parking_condition.hpp"

#include "replay/format.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace go2cpp::replay {

/// 参与 select 的 channel 共享的一把组锁。
class SelectGroup {
public:
    std::mutex& mutex() noexcept { return m_mutex; }

private:
    std::mutex m_mutex;
};

/**
 * @brief 一次 select 调用的共享状态。
 *
 * 生命周期：由 `SyncSelect` 持有（栈上），所有 case 的 channel 只保存它的
 * 裸指针。select 返回前一定先撤销所有登记，因此指针不会悬空。
 *
 * 并发约定：
 * - `claimed` 是**唯一的胜者判定**（CAS）。channel 侧在组锁内 claim；
 *   context 的 Cancel 可能在另一个线程、只持 ctx 锁，因此用原子操作。
 * - `int_value/data/xfer_id/selected_channel` 只由 claim 成功者在 claim
 *   之后写入。channel claim 发生在组锁内，select 醒来时重新持组锁，故安全；
 *   context 只写 `decision`（atomic），不写值。
 */
class SelectState {
public:
    static constexpr std::int32_t kUndecided = -1;

    SelectState() = default;
    SelectState(const SelectState&) = delete;
    SelectState& operator=(const SelectState&) = delete;

    core::ParkingCondition cv;
    std::atomic<std::int32_t> claimed{kUndecided};
    std::atomic<std::uint8_t> decision{static_cast<std::uint8_t>(Decision::kOk)};

    std::uint32_t gid{0};
    std::uint32_t op_index{0};
    std::int64_t int_value{0};
    std::string data;
    std::uint32_t xfer_id{0};
    std::uint32_t selected_channel{kNoChannel};
    std::vector<SelectCaseRecord> cases;

    /// 尝试把本次 select 判给 `case_index`；返回是否成功（只有一个赢家）。
    bool Claim(std::uint32_t case_index) noexcept {
        std::int32_t expected = kUndecided;
        return claimed.compare_exchange_strong(
            expected, static_cast<std::int32_t>(case_index),
            std::memory_order_acq_rel, std::memory_order_acquire);
    }

    bool Decided() const noexcept {
        return claimed.load(std::memory_order_acquire) != kUndecided;
    }
    std::int32_t Selected() const noexcept {
        return claimed.load(std::memory_order_acquire);
    }
    void Notify() noexcept { cv.notify_one(); }
    void SetDecision(Decision value) noexcept {
        decision.store(static_cast<std::uint8_t>(value), std::memory_order_release);
    }
    Decision GetDecision() const noexcept {
        return static_cast<Decision>(decision.load(std::memory_order_acquire));
    }
};

/// select 登记在 channel 队列里的 token；`SyncSelect` 用它撤销未命中的登记。
using SelectWaiterToken = std::shared_ptr<void>;

}  // namespace go2cpp::replay
