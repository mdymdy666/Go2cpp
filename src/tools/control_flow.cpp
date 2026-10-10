#include "go2cpp/control_flow.hpp"

#include <mutex>

namespace go2cpp {
namespace detail {

struct PanicState {
    mutable std::mutex mutex;
    PanicInfo info;
    bool active{false};
    bool recovered{false};
};

thread_local unsigned int s_defer_depth = 0;
thread_local std::exception_ptr s_defer_exception;

class DeferCallbackScope final {
public:
    /// 函数功能：执行 DeferCallbackScope，完成本函数所属模块的单步操作。
    /// 执行流程：
    /// 1. 校验传入参数以及当前对象/线程状态；
    /// 2. 按状态机规则获取必要的锁并更新内部数据；
    /// 3. 发布结果、唤醒等待者并保持资源生命周期完整。
    /// @param[in] s_defer_depth 调用方传入的参数，具体约束以头文件声明为准。
    /// @return 通过返回值或对象状态报告执行结果；void/构造析构函数无返回值。
    /// @note 函数不改变公开接口；异常、取消和并发边界由实现中的保护路径处理。
    DeferCallbackScope() noexcept : m_previous(s_defer_depth) {
        ++s_defer_depth;
    }

    ~DeferCallbackScope() noexcept { s_defer_depth = m_previous; }

private:
    unsigned int m_previous;
};

bool InDeferCallback() noexcept { return s_defer_depth != 0; }

/// 函数功能：完成 RecordDeferException 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] exception 调用方传入的参数，具体约束以头文件声明为准。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
void RecordDeferException(std::exception_ptr exception) noexcept {
    s_defer_exception = std::move(exception);
}

/// 函数功能：完成 LastDeferException 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] 无；该函数仅使用所属对象或线程局部状态。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
std::exception_ptr LastDeferException() noexcept {
    return s_defer_exception;
}

void ClearDeferException() noexcept { s_defer_exception = {}; }

}  // namespace detail

defer::~defer() noexcept { run_now(); }

/// 函数功能：完成 run_now 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] 无；该函数仅使用所属对象或线程局部状态。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
void defer::run_now() noexcept {
    if (!m_active) {
        return;
    }
    m_active = false;
    if (!m_callable) {
        return;
    }
    detail::DeferCallbackScope scope;
    m_callable->run();
}

/// 函数功能：完成 panic 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] 无；该函数仅使用所属对象或线程局部状态。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
panic::panic() : m_state(std::make_shared<detail::PanicState>()) {}

panic::~panic() = default;

/// 函数功能：完成 call 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] message 调用方传入的参数，具体约束以头文件声明为准。
/// @param[in] code 调用方传入的参数，具体约束以头文件声明为准。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
void panic::call(std::string message, int code) {
    PanicInfo info;
    info.raised = true;
    info.message = std::move(message);
    info.code = code;
    call_info(std::move(info));
}

/// 函数功能：完成 call 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] message 调用方传入的参数，具体约束以头文件声明为准。
/// @param[in] code 调用方传入的参数，具体约束以头文件声明为准。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
void panic::call(const char* message, int code) {
    call(message == nullptr ? std::string{} : std::string(message), code);
}

/// 函数功能：完成 call_info 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] info 调用方传入的参数，具体约束以头文件声明为准。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
void panic::call_info(PanicInfo info) {
    if (!m_state) {
        m_state = std::make_shared<detail::PanicState>();
    }
    std::lock_guard<std::mutex> lock(m_state->mutex);
    m_state->info = std::move(info);
    m_state->active = true;
    m_state->recovered = false;
}

/// 函数功能：完成 active 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] 无；该函数仅使用所属对象或线程局部状态。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
bool panic::active() const noexcept {
    if (!m_state) {
        return false;
    }
    std::lock_guard<std::mutex> lock(m_state->mutex);
    return m_state->active;
}

/// 函数功能：完成 recovered 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] 无；该函数仅使用所属对象或线程局部状态。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
bool panic::recovered() const noexcept {
    if (!m_state) {
        return false;
    }
    std::lock_guard<std::mutex> lock(m_state->mutex);
    return m_state->recovered;
}

/// 函数功能：完成 info 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] 无；该函数仅使用所属对象或线程局部状态。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
PanicInfo panic::info() const {
    if (!m_state) {
        return {};
    }
    std::lock_guard<std::mutex> lock(m_state->mutex);
    return m_state->info;
}

/// 函数功能：完成 clear 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] 无；该函数仅使用所属对象或线程局部状态。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
void panic::clear() noexcept {
    if (!m_state) {
        return;
    }
    std::lock_guard<std::mutex> lock(m_state->mutex);
    m_state->info.payload.reset();
    m_state->info.message.clear();
    m_state->info.code = 0;
    m_state->info.raised = false;
    m_state->active = false;
    m_state->recovered = false;
}

/// 函数功能：完成 bind 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] source 调用方传入的参数，具体约束以头文件声明为准。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
bool recover::bind(panic& source) noexcept {
    m_state = source.m_state;
    return !m_state.expired();
}

/// 函数功能：完成 active 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] 无；该函数仅使用所属对象或线程局部状态。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
bool recover::active() const noexcept {
    const auto state = m_state.lock();
    if (!state) {
        return false;
    }
    std::lock_guard<std::mutex> lock(state->mutex);
    return state->active;
}

/// 函数功能：完成 take 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] 无；该函数仅使用所属对象或线程局部状态。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
std::optional<PanicInfo> recover::take() const {
    if (!detail::InDeferCallback()) {
        return std::nullopt;
    }
    const auto state = m_state.lock();
    if (!state) {
        return std::nullopt;
    }
    std::lock_guard<std::mutex> lock(state->mutex);
    if (!state->active) {
        return std::nullopt;
    }
    PanicInfo result = std::move(state->info);
    state->info = {};
    state->active = false;
    state->recovered = true;
    return result;
}

/// 函数功能：完成 operator 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] 无；该函数仅使用所属对象或线程局部状态。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
bool recover::operator()() const noexcept {
    if (!detail::InDeferCallback()) {
        return false;
    }
    const auto state = m_state.lock();
    if (!state) {
        return false;
    }
    std::lock_guard<std::mutex> lock(state->mutex);
    if (!state->active) {
        return false;
    }
    state->info.payload.reset();
    state->info.message.clear();
    state->info.code = 0;
    state->info.raised = false;
    state->active = false;
    state->recovered = true;
    return true;
}

/// 函数功能：完成 peek 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] 无；该函数仅使用所属对象或线程局部状态。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
PanicInfo recover::peek() const {
    const auto state = m_state.lock();
    if (!state) {
        return {};
    }
    std::lock_guard<std::mutex> lock(state->mutex);
    return state->info;
}

/// 函数功能：完成 LastDeferException 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] 无；该函数仅使用所属对象或线程局部状态。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
std::exception_ptr LastDeferException() noexcept {
    return detail::LastDeferException();
}

/// 函数功能：完成 ClearDeferException 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] 无；该函数仅使用所属对象或线程局部状态。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
void ClearDeferException() noexcept {
    detail::ClearDeferException();
}

}  // namespace go2cpp
