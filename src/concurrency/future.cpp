/**
 * @file future.cpp
 * @brief Future 的与类型无关部分：等待后端注册与结果封装。
 *
 * ## 这个文件实现什么
 *
 * `future.hpp` 里大部分逻辑是模板（要按类型实例化），本文件只放
 * **与类型无关**的部分：
 *
 * - `FutureWaitBackend` 的全局注册表：默认实现基于 `ParkingCondition`，
 *   插件可以在创建新 Promise 之前替换成别的调度器/事件循环的等待原语；
 * - 计时辅助（`SaturatingDeadline` 一类）：把相对超时换算成绝对时间点，
 *   用饱和加法避免溢出成"已过期"；
 * - 错误构造与状态映射。
 *
 * ## 为什么后端要可替换
 *
 * Future 不应该假设自己跑在本项目的 Fiber 里：把它嵌进别的运行时（或纯线程
 * 程序）时，等待方式必须能换。后端只负责"等待"和"广播"，不拥有状态。
 */
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
std::shared_ptr<const FutureWaitBackendFactory> s_wait_backend_factory;

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
        auto next = std::make_shared<const FutureWaitBackendFactory>(
            std::move(factory));
        std::lock_guard<std::mutex> lock(s_wait_backend_mutex);
        s_wait_backend_factory = std::move(next);
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
        std::shared_ptr<const FutureWaitBackendFactory> old;
        {
            std::lock_guard<std::mutex> lock(s_wait_backend_mutex);
            old.swap(s_wait_backend_factory);
        }
        // 在锁外销毁用户工厂，避免析构函数重入注册接口时死锁。
        old.reset();
    } catch (...) {
        // mutex/std::function 的清理不应让 noexcept API 终止进程。
    }
}

/// 函数功能：创建一个 Future 状态需要的等待后端实例。
/// 执行流程：复制工厂后在锁外构造实例，失败时回退默认后端。
/// @return 非空等待后端；调用方不需要自行管理生命周期。
/// @note 锁外执行用户工厂，避免工厂重入注册造成锁反转。
std::shared_ptr<FutureWaitBackend> CreateFutureWaitBackend() noexcept {
    std::shared_ptr<const FutureWaitBackendFactory> factory_snapshot;
    try {
        {
            std::lock_guard<std::mutex> lock(s_wait_backend_mutex);
            factory_snapshot = s_wait_backend_factory;
        }
        if (factory_snapshot && *factory_snapshot) {
            if (auto backend = (*factory_snapshot)()) {
                return backend;
            }
        }
    } catch (...) {
        // 插件故障必须隔离到创建操作，运行时仍可使用默认后端。
    }
    return std::make_shared<DefaultFutureWaitBackend>();
}

const char* FutureStatusName(FutureStatus status) noexcept {
    return StatusName(status);
}

FutureError::FutureError(FutureStatus status, ErrorPtr error)
    : std::runtime_error(error ? error->Message()
                               : std::string("go2cpp future error")),
      m_status(status),
      m_error(std::move(error)) {}

FutureError::~FutureError() = default;

ErrorPtr BrokenPromiseError() {
    // 共享不可变错误对象，避免每个未完成 Promise 析构时重复分配。
    static const ErrorPtr error =
        NewError("go2cpp future promise was abandoned");
    return error;
}

}  // namespace go2cpp
