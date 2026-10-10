#include "go2cpp/thread_policy.hpp"

namespace go2cpp {
namespace {

thread_local scheduler::Scheduler* s_policy_scheduler = nullptr;
thread_local ThreadParticipationMode s_policy_participation =
    ThreadParticipationMode::kUnmanaged;
thread_local ThreadHookMode s_policy_hook = ThreadHookMode::kInherit;
thread_local bool s_runtime_worker = false;

/// 函数功能：完成 restore_policy 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] previous 调用方传入的参数，具体约束以头文件声明为准。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
void restore_policy(const ThreadPolicySnapshot& previous) noexcept {
    s_policy_scheduler = previous.scheduler;
    s_policy_participation = previous.participation;
    s_policy_hook = previous.hook;
    s_runtime_worker = previous.runtime_worker;
}

}  // namespace

/// 函数功能：完成 CurrentThreadPolicy 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] 无；该函数仅使用所属对象或线程局部状态。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
ThreadPolicySnapshot CurrentThreadPolicy() noexcept {
    return ThreadPolicySnapshot{s_policy_scheduler, s_policy_participation,
                               s_policy_hook, s_runtime_worker};
}

/// 函数功能：完成 ThreadParticipatesInGMP 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] 无；该函数仅使用所属对象或线程局部状态。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
bool ThreadParticipatesInGMP() noexcept {
    return s_policy_scheduler != nullptr &&
           s_policy_participation == ThreadParticipationMode::kGmpEligible;
}

/// 函数功能：完成 ScopedThreadParticipation 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] scheduler 调用方传入的参数，具体约束以头文件声明为准。
/// @param[in] mode 调用方传入的参数，具体约束以头文件声明为准。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
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

/// 函数功能：完成 ScopedThreadParticipation 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] scheduler 调用方传入的参数，具体约束以头文件声明为准。
/// @param[in] mode 调用方传入的参数，具体约束以头文件声明为准。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
ScopedThreadParticipation::ScopedThreadParticipation(
    scheduler::Scheduler& scheduler, ThreadParticipationMode mode) noexcept
    : ScopedThreadParticipation(&scheduler, mode) {}

/// 函数功能：完成 ScopedThreadParticipation 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] 无；该函数仅使用所属对象或线程局部状态。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
ScopedThreadParticipation::~ScopedThreadParticipation() {
    if (m_active) {
        restore_policy(m_previous);
    }
}

ThreadHookMode CurrentThreadHookMode() noexcept { return s_policy_hook; }

/// 函数功能：完成 SetCurrentThreadHookMode 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] mode 调用方传入的参数，具体约束以头文件声明为准。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
void SetCurrentThreadHookMode(ThreadHookMode mode) noexcept {
    s_policy_hook = mode;
}

/// 函数功能：完成 ScopedThreadHookMode 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] mode 调用方传入的参数，具体约束以头文件声明为准。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
ScopedThreadHookMode::ScopedThreadHookMode(ThreadHookMode mode) noexcept
    : m_previous(CurrentThreadHookMode()), m_active(true) {
    SetCurrentThreadHookMode(mode);
}

/// 函数功能：完成 ScopedThreadHookMode 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] 无；该函数仅使用所属对象或线程局部状态。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
ScopedThreadHookMode::~ScopedThreadHookMode() {
    if (m_active) {
        SetCurrentThreadHookMode(m_previous);
    }
}

namespace thread_policy::detail {

/// 函数功能：完成 EnterRuntimeWorker 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] scheduler 调用方传入的参数，具体约束以头文件声明为准。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
void EnterRuntimeWorker(scheduler::Scheduler* scheduler) noexcept {
    s_policy_scheduler = scheduler;
    s_policy_participation = scheduler != nullptr
                                 ? ThreadParticipationMode::kGmpEligible
                                 : ThreadParticipationMode::kUnmanaged;
    s_runtime_worker = scheduler != nullptr;
}

/// 函数功能：完成 LeaveRuntimeWorker 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] 无；该函数仅使用所属对象或线程局部状态。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
void LeaveRuntimeWorker() noexcept {
    s_policy_scheduler = nullptr;
    s_policy_participation = ThreadParticipationMode::kUnmanaged;
    s_runtime_worker = false;
}

/// 函数功能：完成 HookAllowed 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] process_enabled 调用方传入的参数，具体约束以头文件声明为准。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
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
