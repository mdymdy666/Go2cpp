#pragma once

/**
 * @file context.hpp
 * @brief 可记录/可重放的 Context：显式取消、deadline、select 的 `<-ctx.Done()`。
 *
 * - **live**：`Cancel()` 立刻写 `kCtx(kCancelled)` 并唤醒所有在 select 里等待
 *   本 ctx 的 fiber；`SetDeadline()` 只记录 deadline（虚拟纳秒），由等待方
 *   在超时时判定，不启动真实定时器线程。
 * - **replay**：状态只从记录的 `kCtx` 事件读取；`Cancel()` 不再影响控制流，
 *   因为 select 的决策已经由 `kSelect` 记录固定。
 *
 * 锁顺序：**select 组锁 → ctx 锁**（select poll/arm 时）；`Cancel()` 只持
 * ctx 锁，用 `SelectState::Claim()` 的原子 CAS 判定赢家，不再回头拿组锁，
 * 因此不存在反向等待。
 */

#include "replay/clock.hpp"
#include "replay/format.hpp"
#include "replay/replay_source.hpp"
#include "replay/select_state.hpp"

#include <cstdint>
#include <mutex>
#include <vector>

namespace go2cpp::replay {

class ReplayContext {
public:
    /// live 模式：取消/deadline 会写进 `recorder`。
    ReplayContext(Recorder& recorder, ReplayClock& clock, std::uint32_t ctx_id)
        : m_recorder(&recorder), m_clock(&clock), m_id(ctx_id) {}

    /// replay 模式：只读记录。
    ReplayContext(ReplaySource& source, std::uint32_t ctx_id)
        : m_source(&source), m_id(ctx_id) {}

    ReplayContext(const ReplayContext&) = delete;
    ReplayContext& operator=(const ReplayContext&) = delete;

    std::uint32_t id() const noexcept { return m_id; }

    /// 显式取消。重复调用无副作用。
    void Cancel() {
        // 整个遍历必须持 ctx 锁：select 被唤醒后会先 Disarm 再销毁自己的
        // SelectState；如果在锁外遍历拷贝，Cancel 可能拿到悬空的 state。
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_cancelled) {
            return;
        }
        m_cancelled = true;
        if (m_recorder != nullptr) {
            m_recorder->Write(MakeCtxEvent(
                m_id, ContextEventKind::kCancelled,
                m_recorder->NextCtxIndex(m_id)));
        }
        for (const auto& waiter : m_waiters) {
            if (!waiter.state->Claim(waiter.case_index)) {
                continue;
            }
            waiter.state->SetDecision(Decision::kCancelled);
            if (m_recorder != nullptr) {
                // 取消唤醒也是重放的驱动点：没有这条 kWake，select 的
                // kPark 在记录里就没有配对，静态/完整重放都会判 stuck。
                m_recorder->Write(MakeParkEvent(
                    EventType::kWake, waiter.state->gid, kNoChannel,
                    ParkReason::kContext, waiter.state->op_index, 0));
            }
            waiter.state->Notify();
        }
        m_waiters.clear();
    }

    /// 设置 deadline（虚拟纳秒；一般取 `clock.NowNs() + timeout`）。
    void SetDeadline(std::uint64_t deadline_ns) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_has_deadline && deadline_ns >= m_deadline_ns) {
            return;
        }
        m_has_deadline = true;
        m_deadline_ns = deadline_ns;
        if (m_recorder != nullptr) {
            m_recorder->Write(MakeCtxEvent(
                m_id, ContextEventKind::kDeadlineSet,
                m_recorder->NextCtxIndex(m_id)));
        }
    }

    bool HasDeadline() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_has_deadline;
    }

    std::uint64_t DeadlineNs() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_deadline_ns;
    }

    bool Cancelled() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_cancelled;
    }

    /// deadline 是否已到期（按虚拟时钟）。
    bool Expired(std::uint64_t now_ns) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_has_deadline && now_ns >= m_deadline_ns;
    }

    /// 当前状态：`kOk` / `kCancelled` / `kTimedOut`。
    Decision Err() const {
        if (m_source != nullptr) {
            return m_source->ContextDecision(m_id);
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_cancelled) {
            return Decision::kCancelled;
        }
        if (m_has_deadline && m_clock->NowNs() >= m_deadline_ns) {
            return Decision::kTimedOut;
        }
        return Decision::kOk;
    }

    /**
     * @brief select poll 用：非阻塞地取当前状态。
     *
     * 返回 `kOk` 表示尚未触发，调用方应登记等待；否则返回取消/超时。
     */
    Decision Peek() const { return Err(); }

    /// 把某个 select 登记为「等待本 ctx」。
    void Arm(SelectState& state, std::uint32_t case_index) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_waiters.push_back(WaiterRef{&state, case_index});
    }

    /// 撤销登记（select 醒来后无条件调用）。
    void Disarm(SelectState& state) {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto it = m_waiters.begin(); it != m_waiters.end();) {
            if (it->state == &state) {
                it = m_waiters.erase(it);
            } else {
                ++it;
            }
        }
    }

    /// 记录一次 deadline 到期（select 判定后调用；只写一次）。
    void NoteDeadlineExceeded() {
        if (m_recorder == nullptr) {
            return;
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_deadline_recorded) {
            return;
        }
        m_deadline_recorded = true;
        m_recorder->Write(MakeCtxEvent(
            m_id, ContextEventKind::kDeadlineExceeded,
            m_recorder->NextCtxIndex(m_id)));
    }

private:
    struct WaiterRef {
        SelectState* state;
        std::uint32_t case_index;
    };

    Recorder* const m_recorder{nullptr};
    ReplaySource* const m_source{nullptr};
    ReplayClock* const m_clock{nullptr};
    const std::uint32_t m_id{0};

    mutable std::mutex m_mutex;
    bool m_cancelled{false};
    bool m_has_deadline{false};
    std::uint64_t m_deadline_ns{0};
    bool m_deadline_recorded{false};
    std::vector<WaiterRef> m_waiters;
};

}  // namespace go2cpp::replay
