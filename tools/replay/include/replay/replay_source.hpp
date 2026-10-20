#pragma once

/**
 * @file replay_source.hpp
 * @brief replay 模式下 channel / clock / context 读取记录事实的抽象接口。
 *
 * `SyncChannel<T>`、`SyncSelect`、`ReplayClock`、`ReplayContext` 同时服务
 * 两种模式：
 *   - **live**：真实并发执行，操作结果由运行时决定，逐个决定点写入记录；
 *   - **replay**：单线程按记录重跑，操作结果**只从记录读取**。
 *
 * 本接口是 replay 侧的窄边界，由 `ReplayEngine` 实现；`SyncChannel` 只依赖
 * 这里的几个方法，因此不把 ucontext/文件读取拖进 channel 实现。
 */

#include "replay/format.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace go2cpp::replay {

class ReplayTimerService;

namespace detail {
inline thread_local std::uint32_t t_current_gid = 0;
}  // namespace detail

/**
 * @brief replay 模式下「下一个决定点」的来源。
 *
 * 调用约定：所有方法只在**当前正在重放的 fiber** 上下文里调用（单线程），
 * 因此实现不需要加锁。
 */
class ReplaySource {
public:
    virtual ~ReplaySource() = default;

    /**
     * @brief 取当前 gid 的下一个 channel 操作事件（send/recv/select/close）。
     *
     * 返回的指针由引擎持有，生命周期覆盖整个重放过程；越界或类型不符时
     * 返回 nullptr，并把 `LastError()` 置为可读原因。
     */
    virtual const Event* NextOperation(std::uint32_t gid) = 0;

    /**
     * @brief 取当前 gid 的下一个 `kTimer(kSleep)` 事件。
     *
     * 没有对应事件时返回 nullptr；`ReplayClock` 会把这种情况记为错误（记录
     * 与程序不一致），但不会阻塞重放。
     */
    virtual const Event* NextSleep(std::uint32_t gid) {
        (void)gid;
        return nullptr;
    }

    /// 当前虚拟时间（纳秒，相对录制起点）。默认 0。
    virtual std::uint64_t VirtualNowNs() const noexcept { return 0; }

    /// 推进虚拟时间（只增不减）。默认空操作。
    virtual void AdvanceVirtualTime(std::uint64_t virtual_ns) { (void)virtual_ns; }

    /**
     * @brief 查询某个 ctx 是否已取消/到期（消费记录中对应的 `kCtx` 事件）。
     *
     * 返回 `Decision::kCancelled` / `kTimedOut`；未取消返回 `Decision::kOk`。
     */
    virtual Decision ContextDecision(std::uint32_t ctx_id) {
        (void)ctx_id;
        return Decision::kOk;
    }

    /**
     * @brief 当前正在重放的 fiber 的 gid。
     *
     * **不能用 thread_local**：单线程上多个 fiber 交替执行，thread_local 会
     * 被后一个 fiber 覆盖。引擎按 ucontext 的 current() 返回。
     */
    virtual std::uint32_t CurrentFiberGid() const noexcept = 0;

    /**
     * @brief 当前 fiber 在某个阻塞操作上挂起，把 CPU 交还引擎。
     *
     * 引擎会在记录中的 `kWake(gid)` 事件处恢复它。返回即表示唤醒已发生。
     */
    virtual void ParkCurrent(std::uint32_t gid, const Event& operation) = 0;

    /**
     * @brief 报告一次操作的观测结果（用于与记录交叉校验）。
     * @note 只做校验与统计，不参与控制流。
     */
    virtual void RecordResult(std::uint32_t gid, const Event& operation,
                              std::int64_t value, const std::string& data) = 0;

    /// 最近一次错误的可读描述；空表示无错误。
    virtual const char* LastError() const = 0;

    /**
     * @brief 安装 timer 回放服务（`ReplayTimerService` 构造时自动调用）。
     * @details 引擎在遍历事件流时把 `kTimer` 事件派发给它，从而让
     *          timer 回调与 channel/select 的交错顺序完全由记录决定。
     */
    virtual void InstallTimerService(ReplayTimerService* service) {
        (void)service;
    }

    /// 当前安装的 timer 回放服务；默认 nullptr。
    virtual ReplayTimerService* TimerService() { return nullptr; }

    /**
     * @brief replay：强制 `CurrentFiberGid()` 的返回值（timer 回调用）。
     * @details timer 回调在引擎线程执行，不在任何 fiber 栈上；如果没有
     *          这个覆盖，回调里的 channel 操作会拿到 `kNoChannel`，
     *          无法消费记录。传 `kNoChannel` 表示清除覆盖。
     */
    virtual void SetCurrentGidOverride(std::uint32_t gid) { (void)gid; }

    /// 记录中的全部事件（replay 引擎提供；默认空）。
    virtual const std::vector<Event>& Events() const {
        static const std::vector<Event> kEmpty;
        return kEmpty;
    }
};

/// 线程本地当前 gid（live 模式由 demo 的 fiber 入口设置）。
inline std::uint32_t CurrentGid() noexcept { return detail::t_current_gid; }
/// 设置当前线程的 gid；`ScopedGid` 的底层入口。
inline void SetCurrentGid(std::uint32_t gid) noexcept { detail::t_current_gid = gid; }

/// RAII：进入 fiber 时设置 gid，离开时恢复旧值（支持嵌套/线程复用）。
class ScopedGid {
public:
    explicit ScopedGid(std::uint32_t gid) noexcept
        : m_previous(CurrentGid()) {
        SetCurrentGid(gid);
    }
    ~ScopedGid() { SetCurrentGid(m_previous); }
    ScopedGid(const ScopedGid&) = delete;
    ScopedGid& operator=(const ScopedGid&) = delete;

private:
    std::uint32_t m_previous;
};

}  // namespace go2cpp::replay
