#pragma once

#include <cstdint>

namespace go2cpp {

namespace scheduler { class Scheduler; }

enum class ThreadParticipationMode : std::uint8_t {
    kUnmanaged,
    kGmpEligible,

    Unmanaged = kUnmanaged,
    GmpEligible = kGmpEligible,
};

enum class ThreadHookMode : std::uint8_t {
    kInherit,
    kDisabled,
    kEnabled,

    Inherit = kInherit,
    Disabled = kDisabled,
    Enabled = kEnabled,
};

// 快照只用于观测。scheduler 指针不拥有对象，仅在关联 Scheduler 存活时有效；
// 查询快照不会把线程挂接为 worker，也不会预留 P。
struct ThreadPolicySnapshot {
    scheduler::Scheduler* scheduler{nullptr};
    ThreadParticipationMode participation{ThreadParticipationMode::kUnmanaged};
    ThreadHookMode hook{ThreadHookMode::kInherit};
    bool runtime_worker{false};

    bool participates_in_gmp() const noexcept {
        return scheduler != nullptr &&
               participation == ThreadParticipationMode::kGmpEligible;
    }
};

ThreadPolicySnapshot CurrentThreadPolicy() noexcept;
inline ThreadPolicySnapshot current_thread_policy() noexcept {
    return CurrentThreadPolicy();
}

bool ThreadParticipatesInGMP() noexcept;
inline bool thread_participates_in_gmp() noexcept {
    return ThreadParticipatesInGMP();
}

// 在一个作用域内把当前 OS 线程标记为可参与调度器策略。它不会创建 M、
// 绑定 P、安装 Scheduler::current_scheduler() 或执行队列任务。由于需要
// 恢复线程局部状态，移动/析构必须发生在同一 OS 线程。
/**
 * 单个 OS 线程的 GMP 参与策略作用域。
 *
 * 依赖：Scheduler 身份和线程局部状态；对上层提供临时标记，使外部线程
 * 可以声明自己是否允许参与 GMP。该类不创建 M/P、不执行任务，析构时恢复
 * 进入前的 TLS 状态。
 */
class ScopedThreadParticipation final {
public:
    explicit ScopedThreadParticipation(
        scheduler::Scheduler* scheduler,
        ThreadParticipationMode mode = ThreadParticipationMode::kGmpEligible)
        noexcept;
    explicit ScopedThreadParticipation(
        scheduler::Scheduler& scheduler,
        ThreadParticipationMode mode = ThreadParticipationMode::kGmpEligible)
        noexcept;
    ~ScopedThreadParticipation();

    ScopedThreadParticipation(const ScopedThreadParticipation&) = delete;
    ScopedThreadParticipation& operator=(const ScopedThreadParticipation&) = delete;
    // TLS 恢复绑定到创建它的 OS 线程；不要把作用域移动到其他线程或 Fiber。
    ScopedThreadParticipation(ScopedThreadParticipation&&) = delete;
    ScopedThreadParticipation& operator=(ScopedThreadParticipation&&) = delete;

private:
    ThreadPolicySnapshot m_previous{};
    bool m_active{false};
};

ThreadHookMode CurrentThreadHookMode() noexcept;
void SetCurrentThreadHookMode(ThreadHookMode mode) noexcept;

// 线程级 Hook 覆盖。kInherit 跟随进程级开关；kDisabled 禁止本线程新的
// 协作式接纳；kEnabled 让本线程进入 Hook 路径。由于状态是线程局部的，
// 移动/析构必须发生在同一 OS 线程。
/**
 * 单个 OS 线程的 Hook 开关作用域。
 *
 * 依赖：进程级 Hook 开关和线程局部覆盖；对上层提供继承、禁用或启用 Hook
 * 的临时策略，析构时恢复之前的 TLS 状态。
 */
class ScopedThreadHookMode final {
public:
    explicit ScopedThreadHookMode(ThreadHookMode mode) noexcept;
    ~ScopedThreadHookMode();

    ScopedThreadHookMode(const ScopedThreadHookMode&) = delete;
    ScopedThreadHookMode& operator=(const ScopedThreadHookMode&) = delete;
    // TLS 恢复绑定到创建它的 OS 线程；不要把作用域移动到其他线程或 Fiber。
    ScopedThreadHookMode(ScopedThreadHookMode&&) = delete;
    ScopedThreadHookMode& operator=(ScopedThreadHookMode&&) = delete;

private:
    ThreadHookMode m_previous{ThreadHookMode::kInherit};
    bool m_active{false};
};

namespace thread_policy::detail {

// 由调度器 worker 进入/退出时调用。这是源码级 Hook，不是 ABI 承诺；外部
// 线程应使用 ScopedThreadParticipation，并通过未来后端 API 显式执行调度任务。
void EnterRuntimeWorker(scheduler::Scheduler* scheduler) noexcept;
void LeaveRuntimeWorker() noexcept;

// 合并进程级 Hook 开关与当前线程覆盖选项。
bool HookAllowed(bool process_enabled) noexcept;

}  // namespace thread_policy::detail

}  // namespace go2cpp
