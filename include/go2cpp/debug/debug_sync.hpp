#pragma once

/**
 * @file debug_sync.hpp
 * @brief 同步原语的调试代理（P3 锁观测）。
 *
 * 设计依据：DebugTooling/design/09-debug-class-conventions.md §2.4（代理方案）。
 *
 * 为什么不是继承：`sync::Mutex` / `ConditionVariable` / `WaitGroup` 都是 `final`，
 *   且**不修改 `final`**（那等于改原框架）。因此这里：
 *     - `Debug*` 代理**组合持有**原类引用；
 *     - 原类内部**新增私有 `Debug*SnapshotInternal()` 方法**并由本代理 `friend` 调用；
 *     - 每个原类净增 2 行声明 + 5~10 行实现，**语义与开销完全不变**。
 */

#include "go2cpp/sync.hpp"

#include <cstddef>

namespace go2cpp::debug {

/**
 * @brief `sync::Mutex` 的调试代理。
 * 依赖：调用方保证被包装的 `Mutex` 生命周期长于本代理。
 */
class DebugMutex final {
public:
    explicit DebugMutex(sync::Mutex& mutex) noexcept : m_mutex(&mutex) {}
    explicit DebugMutex(sync::Mutex* mutex) noexcept : m_mutex(mutex) {}

    /// 采集只读快照；空指针时返回默认（held=false, waiters=0）。
    sync::MutexDebugSnapshot Snapshot() const {
        if (m_mutex == nullptr) {
            return {};
        }
        return m_mutex->DebugSnapshotInternal();
    }
    sync::MutexDebugSnapshot Dump() const { return Snapshot(); }

    /// 是否已被持有（等价于 `!TryLock()` 的观测版；不改变锁状态）。
    bool Held() const { return Snapshot().held; }
    /// 等待者数量。
    std::size_t Waiters() const { return Snapshot().waiters; }

private:
    sync::Mutex* m_mutex{nullptr};
};

/**
 * @brief `sync::ConditionVariable` 的调试代理。
 */
class DebugConditionVariable final {
public:
    explicit DebugConditionVariable(sync::ConditionVariable& cond) noexcept : m_cond(&cond) {}
    explicit DebugConditionVariable(sync::ConditionVariable* cond) noexcept : m_cond(cond) {}

    sync::ConditionVariableDebugSnapshot Snapshot() const {
        if (m_cond == nullptr) {
            return {};
        }
        return m_cond->DebugSnapshotInternal();
    }
    sync::ConditionVariableDebugSnapshot Dump() const { return Snapshot(); }
    std::size_t Waiters() const { return Snapshot().waiters; }

private:
    sync::ConditionVariable* m_cond{nullptr};
};

}  // namespace go2cpp::debug
