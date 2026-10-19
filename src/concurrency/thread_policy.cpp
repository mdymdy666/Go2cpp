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

/**
 * @brief 临时把当前线程标记为「参与 GMP」，析构时还原。
 * @param scheduler 关联的调度器（`kGmpEligible` 时必须非空）。
 * @param mode 期望的参与模式。
 *
 * ## 定位
 *
 * 让**非调度器创建的线程**（例如用户自己起的线程）能临时声明「我愿意被
 * GMP 接管」：在这个 scope 内调用 `read()`/`sleep()` 会被 hook 成 park，
 * 而不是原生阻塞。
 *
 * ## 步骤
 *
 * step1. 先保存当前策略快照（`m_previous`），保证析构能精确还原；
 * step2. **若当前线程已经是 runtime worker，则不改策略**：worker 的 M/P
 *        绑定是权威的，外部 scope 不能覆盖它（否则一个用户 scope 就能让
 *        worker 从 GMP 里「掉出来」）；
 * step3. 否则按 mode 设置：`kGmpEligible` 且 scheduler 非空 → 绑定 scheduler；
 *        其余情况一律降级为 `kUnmanaged`；
 * step4. `s_runtime_worker` 恢复成快照值（scope 不改变 worker 身份）。
 *
 * ## 并发/失败语义
 *
 * - 全部是 `thread_local` 写，**无线程安全负担**（只影响本线程）；
 * - **析构一定还原**（`if (m_active)` 保护，防止移动后重复还原）；
 * - **`noexcept`**：策略切换在 hot path 上，且失败也没有可用的错误通道；
 * - **worker 身份不被 scope 覆盖**是这里最重要的不变量。
 */
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

/**
 * @brief 判定当前线程是否允许把阻塞调用 hook 成 park。
 * @param process_enabled 进程级的总开关（`hook.enable_*` 配置）。
 * @return true 表示允许 hook。
 *
 * ## 步骤
 *
 * step1. `kDisabled` → 直接 false（线程级强制关闭）；
 * step2. `kEnabled` → 直接 true（线程级强制开启，**忽略进程开关**）；
 * step3. `kInherit` → 返回 `process_enabled`（默认行为，跟随进程配置）。
 *
 * ## 并发/失败语义
 *
 * - `kEnabled` **能覆盖进程级关闭**是刻意的：它用于「这个线程必须协作」
 *   的场景（例如调度器内部线程），即使全局诊断时关掉了 hook 也要保持工作；
 * - 无锁、`noexcept`，每次 hook 调用都会走一次（所以用 switch 而不是查表）。
 */
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
