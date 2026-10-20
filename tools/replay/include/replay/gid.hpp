#pragma once

/**
 * @file gid.hpp
 * @brief 逻辑 fiber id（gid）解析：录制端按 `Task::id()` 反查，重放端用
 *        `ReplaySource::CurrentFiberGid()`。
 *
 * 独立成文件是因为 `SyncChannel` 与 `ReplayClock` 都需要它，而后者不应
 * 依赖整个 channel 实现。
 */

#include "go2cpp/concurrency/scheduler/scheduler.hpp"

#include "replay/replay_source.hpp"

#include <cstdint>
#include <mutex>
#include <unordered_map>

namespace go2cpp::replay {
namespace detail {

/// 每个 OS 线程最近一次解析结果，避免热路径反复查表。
struct FiberGidCache {
    std::uint64_t task_id{0};
    std::uint32_t gid{0};
    bool valid{false};
};

inline thread_local FiberGidCache t_gid_cache;

struct GidRegistry {
    std::mutex mutex;
    std::unordered_map<std::uint64_t, std::uint32_t> map;
};

inline GidRegistry& Registry() {
    static GidRegistry registry;
    return registry;
}

/**
 * @brief 把调度器 Task id 绑定到逻辑 gid。
 *
 * fiber 会在 park 后被唤醒到**另一个 M 线程**上，thread_local 保存的 gid
 * 会失效；因此 live 模式必须按 `Task::id()` 反查逻辑 gid。
 */
inline void RegisterFiberGid(std::uint64_t task_id, std::uint32_t gid) {
    std::lock_guard<std::mutex> lock(Registry().mutex);
    Registry().map[task_id] = gid;
    t_gid_cache.task_id = task_id;
    t_gid_cache.gid = gid;
    t_gid_cache.valid = true;
}

/// 解析当前执行上下文对应的逻辑 gid。
inline std::uint32_t ResolveGid() {
    go2cpp::Task* const task = go2cpp::Scheduler::current_task_ptr();
    if (task == nullptr) {
        return CurrentGid();  // replay：单线程，thread_local 即可
    }
    const std::uint64_t task_id = task->id();
    if (t_gid_cache.valid && t_gid_cache.task_id == task_id) {
        return t_gid_cache.gid;
    }
    {
        std::lock_guard<std::mutex> lock(Registry().mutex);
        const auto found = Registry().map.find(task_id);
        if (found != Registry().map.end()) {
            t_gid_cache.task_id = task_id;
            t_gid_cache.gid = found->second;
            t_gid_cache.valid = true;
            return found->second;
        }
    }
    return CurrentGid();
}

}  // namespace detail

/**
 * @brief live 模式 fiber 入口：登记 `Task::id() → gid` 并设置线程本地 gid。
 *
 * 跨线程迁移后 `ResolveGid()` 会按 `Task::id()` 重新查到同一个逻辑 gid。
 */
class ScopedFiberGid {
public:
    explicit ScopedFiberGid(std::uint32_t gid) : m_guard(gid) {
        go2cpp::Task* const task = go2cpp::Scheduler::current_task_ptr();
        if (task != nullptr) {
            detail::RegisterFiberGid(task->id(), gid);
        }
    }

private:
    ScopedGid m_guard;
};

}  // namespace go2cpp::replay
