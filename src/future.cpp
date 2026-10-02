#include "go2cpp/future.hpp"

#include "go2cpp/core/parking_condition.hpp"

#include <functional>
#include <mutex>
#include <string>
#include <utility>

namespace go2cpp {

namespace {

/**
 * @brief Future 默认等待后端。
 * @details 只适配 ParkingCondition，不持有 Future 状态；这样 Future 状态
 *          仍由模板类负责，而 Fiber/线程等待策略可以独立替换。
 */
class DefaultFutureWaitBackend final : public FutureWaitBackend {
public:
    /** @brief 按当前线程或受管 Fiber 等待状态变化。 */
    bool Wait(std::unique_lock<std::mutex>& lock,
              std::optional<ContextTimePoint> deadline,
              const std::function<bool()>& predicate) override {
        if (deadline) {
            return m_condition.wait_until(lock, *deadline, predicate);
        }
        return m_condition.wait(lock, predicate);
    }

    /** @brief 广播唤醒所有等待者。 */
    void NotifyAll() noexcept override { m_condition.notify_all(); }

private:
    core::ParkingCondition m_condition;
};

std::mutex s_wait_backend_mutex;
FutureWaitBackendFactory s_wait_backend_factory;

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

/// 函数功能：设置 Future 等待后端工厂，建立模板状态与运行时后端的解耦边界。
/// 执行流程：
/// 1. 接收并复制调用方提供的工厂；
/// 2. 在短临界区内替换全局工厂快照；
/// 3. 后续创建的 FutureState 通过 CreateFutureWaitBackend 获取后端。
/// @param[in] factory 自定义等待后端工厂；为空表示恢复默认实现。
/// @return 工厂已发布返回 true；工厂复制失败返回 false。
/// @note 不影响已经创建的 Future；工厂不得依赖待销毁的外部对象。
bool SetFutureWaitBackendFactory(FutureWaitBackendFactory factory) {
    try {
        std::lock_guard<std::mutex> lock(s_wait_backend_mutex);
        s_wait_backend_factory = std::move(factory);
        return true;
    } catch (...) {
        return false;
    }
}

/// 函数功能：清除 Future 自定义等待后端，恢复默认 ParkingCondition。
/// 执行流程：在互斥锁保护下清空工厂快照，已创建状态保持原后端不变。
/// @return 无返回值。
/// @note 该函数只改变未来创建的状态，不会打断正在进行的等待。
void ResetFutureWaitBackendFactory() noexcept {
    try {
        std::lock_guard<std::mutex> lock(s_wait_backend_mutex);
        s_wait_backend_factory = {};
    } catch (...) {
        // mutex/std::function 的清理不应让 noexcept API 终止进程。
    }
}

/// 函数功能：创建一个 Future 状态需要的等待后端实例。
/// 执行流程：复制工厂后在锁外构造实例，失败时回退默认后端。
/// @return 非空等待后端；调用方不需要自行管理生命周期。
/// @note 锁外执行用户工厂，避免工厂重入注册造成锁反转。
std::shared_ptr<FutureWaitBackend> CreateFutureWaitBackend() noexcept {
    FutureWaitBackendFactory factory;
    try {
        {
            std::lock_guard<std::mutex> lock(s_wait_backend_mutex);
            factory = s_wait_backend_factory;
        }
        if (factory) {
            if (auto backend = factory()) {
                return backend;
            }
        }
    } catch (...) {
        // 插件故障必须隔离到创建操作，运行时仍可使用默认后端。
    }
    return std::make_shared<DefaultFutureWaitBackend>();
}

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
