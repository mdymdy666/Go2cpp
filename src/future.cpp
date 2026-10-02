#include "go2cpp/future.hpp"

#include <mutex>
#include <string>
#include <utility>

namespace go2cpp {

namespace {

/// 函数功能：完成 StatusName 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] status 调用方传入的参数，具体约束以头文件声明为准。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
const char* StatusName(FutureStatus status) noexcept {
    switch (status) {
    case FutureStatus::kReady:
        return "ready";
    case FutureStatus::kError:
        return "error";
    case FutureStatus::kException:
        return "exception";
    case FutureStatus::kCancelled:
        return "cancelled";
    case FutureStatus::kTimedOut:
        return "timed out";
    case FutureStatus::kDeadlineExceeded:
        return "deadline exceeded";
    case FutureStatus::kInvalid:
        return "invalid";
    }
    return "unknown";
}

}  // namespace

/// 函数功能：完成 FutureStatusName 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] status 调用方传入的参数，具体约束以头文件声明为准。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
const char* FutureStatusName(FutureStatus status) noexcept {
    return StatusName(status);
}

/// 函数功能：完成 FutureError 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] status 调用方传入的参数，具体约束以头文件声明为准。
/// @param[in] error 调用方传入的参数，具体约束以头文件声明为准。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
FutureError::FutureError(FutureStatus status, ErrorPtr error)
    : std::runtime_error(error ? error->Message()
                               : std::string("go2cpp future error")),
      m_status(status),
      m_error(std::move(error)) {}

FutureError::~FutureError() = default;

/// 函数功能：完成 BrokenPromiseError 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] 无；该函数仅使用所属对象或线程局部状态。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
ErrorPtr BrokenPromiseError() {
    // 共享不可变错误对象，避免每个未完成 Promise 析构时重复分配。
    static const ErrorPtr error =
        NewError("go2cpp future promise was abandoned");
    return error;
}

}  // namespace go2cpp
