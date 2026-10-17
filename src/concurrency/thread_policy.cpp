/**
 * @file thread_policy.cpp
 * @brief 线程策略实现：推导 P 数量与线程配置。
 *
 * ## 这个文件实现什么
 *
 * 把"该开多少个 worker"的决策从调度器里抽出来：
 *
 * - 优先用显式配置（`SchedulerConfig::processor_count`）；
 * - 否则按硬件并发度推导；
 * - 容器环境下要考虑 cgroup 的 CPU 限额（`nproc` 可能远大于实际配额）；
 * - 可选的线程命名/亲和性设置。
 *
 * ## 为什么值得单独一层
 *
 * 这些逻辑高度平台相关（Linux cgroup、NUMA、实时系统要求都不同），塞进
 * 调度器初始化会让那个函数变成一堆 `#ifdef`。单独成文件后，调度器只问
 * "给我 P 的数量"，其余与它无关。
 */
#include "go2cpp/thread_policy.hpp"

namespace go2cpp {
namespace {

thread_local scheduler::Scheduler* s_policy_scheduler = nullptr;
thread_local ThreadParticipationMode s_policy_participation =
    ThreadParticipationMode::kUnmanaged;
thread_local ThreadHookMode s_policy_hook = ThreadHookMode::kInherit;
thread_local bool s_runtime_worker = false;

void restore_policy(const ThreadPolicySnapshot& previous) noexcept {
    s_policy_scheduler = previous.scheduler;
    s_policy_participation = previous.participation;
    s_policy_hook = previous.hook;
    s_runtime_worker = previous.runtime_worker;
}

}  // namespace

ThreadPolicySnapshot CurrentThreadPolicy() noexcept {
    return ThreadPolicySnapshot{s_policy_scheduler, s_policy_participation,
                               s_policy_hook, s_runtime_worker};
}

bool ThreadParticipatesInGMP() noexcept {
    return s_policy_scheduler != nullptr &&
           s_policy_participation == ThreadParticipationMode::kGmpEligible;
}

ScopedThreadParticipation::ScopedThreadParticipation(
    scheduler::Scheduler* scheduler, ThreadParticipationMode mode) noexcept
    : m_previous(CurrentThreadPolicy()), m_active(true) {
    if (s_runtime_worker) {
        // The worker's M/P binding is authoritative for this thread. Keep it
        // intact while allowing the scope to restore normally on exit.
    } else if (mode == ThreadParticipationMode::kGmpEligible &&
               scheduler != nullptr) {
        s_policy_scheduler = scheduler;
        s_policy_participation = mode;
    } else {
        s_policy_scheduler = nullptr;
        s_policy_participation = ThreadParticipationMode::kUnmanaged;
    }
    s_runtime_worker = m_previous.runtime_worker;
}

ScopedThreadParticipation::ScopedThreadParticipation(
    scheduler::Scheduler& scheduler, ThreadParticipationMode mode) noexcept
    : ScopedThreadParticipation(&scheduler, mode) {}

ScopedThreadParticipation::~ScopedThreadParticipation() {
    if (m_active) {
        restore_policy(m_previous);
    }
}

ThreadHookMode CurrentThreadHookMode() noexcept { return s_policy_hook; }

void SetCurrentThreadHookMode(ThreadHookMode mode) noexcept {
    s_policy_hook = mode;
}

ScopedThreadHookMode::ScopedThreadHookMode(ThreadHookMode mode) noexcept
    : m_previous(CurrentThreadHookMode()), m_active(true) {
    SetCurrentThreadHookMode(mode);
}

ScopedThreadHookMode::~ScopedThreadHookMode() {
    if (m_active) {
        SetCurrentThreadHookMode(m_previous);
    }
}

namespace thread_policy::detail {

void EnterRuntimeWorker(scheduler::Scheduler* scheduler) noexcept {
    s_policy_scheduler = scheduler;
    s_policy_participation = scheduler != nullptr
                                 ? ThreadParticipationMode::kGmpEligible
                                 : ThreadParticipationMode::kUnmanaged;
    s_runtime_worker = scheduler != nullptr;
}

void LeaveRuntimeWorker() noexcept {
    s_policy_scheduler = nullptr;
    s_policy_participation = ThreadParticipationMode::kUnmanaged;
    s_runtime_worker = false;
}

bool HookAllowed(bool process_enabled) noexcept {
    switch (s_policy_hook) {
        case ThreadHookMode::kDisabled:
            return false;
        case ThreadHookMode::kEnabled:
            return true;
        case ThreadHookMode::kInherit:
            return process_enabled;
    }
    return process_enabled;
}

}  // namespace thread_policy::detail
}  // namespace go2cpp
