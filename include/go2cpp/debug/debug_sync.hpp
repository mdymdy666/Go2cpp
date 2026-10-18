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
 *
 * ## 模块定位
 *
 * `debug` 的同步原语观测层：用组合代理读取 `sync::Mutex`/
 * `ConditionVariable`/`WaitGroup` 的内部快照，不改变原语语义。上游是调试
 * CLI 与测试，下游是 `sync` 的 `DebugSnapshotInternal()`。
 *
 * ## 本文件定义什么
 *
 * - `DebugMutex`：mutex 的 held/waiters 快照；
 * - `DebugConditionVariable`：条件变量的 waiters 快照；
 * - `DebugWaitGroup`：WaitGroup 的 count/waiters 快照。
 *
 * ## 阅读建议
 *
 * 三个代理结构相同：构造绑定引用/指针 → `Snapshot()` 转调内部快照。
 * 重点看空指针时返回默认快照的约定。
 *
 * ## 边界与陷阱
 *
 * 代理**不拥有**被包装对象，生命周期必须由调用方保证；`Held()`/`Waiters()`
 * 是瞬时观测，不构成同步原语，不能用来实现互斥逻辑。
 */

#include "go2cpp/sync.hpp"

#include <cstddef>

namespace go2cpp::debug {

/**
 * @brief `sync::Mutex` 的调试代理。
 * 依赖：调用方保证被包装的 `Mutex` 生命周期长于本代理。
 *
 * ## 定位
 * 供调试 CLI/测试只读观察 mutex 状态；所有方法都不加锁、不改变锁状态。
 *
 * ## 不变量
 * `m_mutex == nullptr` 时 `Snapshot()` 返回默认值（held=false, waiters=0）。
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
    sync::Mutex* m_mutex{nullptr}; ///< 被观测的 mutex（不拥有）；构造时绑定，Snapshot 转调
};

/**
 * @brief `sync::ConditionVariable` 的调试代理。
 *
 * ## 定位
 * 只读观察条件变量的等待者数量；不参与 wait/notify。
 *
 * ## 不变量
 * `m_cond == nullptr` 时 `Snapshot()` 返回默认快照。
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
    sync::ConditionVariable* m_cond{nullptr}; ///< 被观测的条件变量（不拥有）；构造时绑定，Snapshot 转调
};


/**
 * @brief `sync::WaitGroup` 的调试代理。
 *
 * ## 定位
 * 只读观察 WaitGroup 的计数与等待者；不参与 Add/Done/Wait。
 *
 * ## 不变量
 * `m_group == nullptr` 时 `Snapshot()` 返回默认快照。
 */
class DebugWaitGroup final {
public:
    explicit DebugWaitGroup(sync::WaitGroup& group) noexcept : m_group(&group) {}
    explicit DebugWaitGroup(sync::WaitGroup* group) noexcept : m_group(group) {}

    sync::WaitGroupDebugSnapshot Snapshot() const {
        if (m_group == nullptr) {
            return {};
        }
        return m_group->DebugSnapshotInternal();
    }
    sync::WaitGroupDebugSnapshot Dump() const { return Snapshot(); }
    std::int64_t Count() const { return Snapshot().count; }
    std::size_t Waiters() const { return Snapshot().waiters; }

private:
    sync::WaitGroup* m_group{nullptr}; ///< 被观测的 WaitGroup（不拥有）；构造时绑定，Snapshot 转调
};

}  // namespace go2cpp::debug
