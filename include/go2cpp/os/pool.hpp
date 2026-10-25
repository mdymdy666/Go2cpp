/**
 * @file pool.hpp
 * @brief 固定大小进程池：有界排队、拒绝策略、缩扩容与异常补位（R22）。
 *
 * ## 池里放的是什么
 *
 * **不是线程池**：池的「worker」是允许同时运行的**子进程槽位**。每个 worker
 * 线程一次只执行一个 `Run`（启动一个子进程 → 收集输出 → 回收），因此
 * `workers = N` 意味着最多 N 个子进程并发。任务排队是有界的。
 *
 * ## 为什么复用 `Run` 而不是自己 fork
 *
 * `Run` 已经处理了「stdin 写入与 stdout/stderr 排空并行」「超时阶梯」
 * 「wait 回收」三件最容易写错的事；进程池只负责调度与生命周期。
 * 每个 worker 的异常（spawn 失败、子进程被信号杀死）只影响该任务结果，
 * **worker 线程继续接下一个任务** —— 这就是 R22 的「异常退出自动补位」。
 *
 * ## 排队与拒绝
 *
 * - `queue_capacity` 是排队上限（不含正在运行的）；
 * - `block_when_full == false`（默认）：队列满时 `Submit` 返回 `kQueueFull`；
 * - `block_when_full == true`：`Submit` 等待 `submit_timeout`（0 = 永久），
 *   超时返回 `kTimeout`。
 *
 * ## 缩扩容
 *
 * - `Resize(n)` 扩容立即创建线程；缩容只更新目标值，`index >= n` 的 worker 在
 *   **队列为空**时自行退出（正在运行的任务不会被中断），`Stats().workers`
 *   随实际存活线程数下降；
 * - 队列非空时多余 worker 仍会领任务：缩容不会丢弃已排队任务，也不会因为
 *   「合格 worker 恰好都在忙」而让队列滞留；
 * - 缩容是异步生效的：`Resize` 返回时目标值已更新，线程退出在其下一次醒来
 *   （队列空）时完成，调用方可轮询 `Stats().workers` 观察；
 * - 扩容复用已退出线程腾出的槽位（线程身份 index 随之复用），槽位表不会
 *   无限增长。
 *
 * ## Shutdown
 *
 * 1. 停止接受新任务；
 * 2. 唤醒所有 worker；
 * 3. 等待运行中的任务最多 `grace`；
 * 4. 仍未结束的进程被 `SIGKILL`（通过 `kill(pid)` 直接发信号），worker 随后
 *    在 `Run` 内回收到退出状态并退出；
 * 5. join 全部线程（含缩容后已退出、待 join 的线程），队列中未开始的任务
 *    标记为取消（`Stats().cancelled`），`Stats().workers` 归零。
 *
 * 因此 `Shutdown` 的阻塞时间有上界（`grace` + 一次系统调用），不会永久挂起。
 */
#pragma once

#include "go2cpp/os/proc.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace go2cpp::os {

/** @brief 进程池配置。 */
struct PoolOptions final {
    /// 并发子进程槽位（>0）。
    std::size_t workers{4};
    /// 排队上限（>0）。
    std::size_t queue_capacity{64};
    /// 队列满时是否阻塞提交（false = 直接拒绝）。
    bool block_when_full{false};
    /// `block_when_full` 时的最长等待（0 = 永久）。
    std::chrono::milliseconds submit_timeout{0};
    /// 任务自身未设 timeout 时使用的默认值（0 = 不限时；不推荐）。
    std::chrono::milliseconds default_timeout{0};
    /// 超时/关闭时的 TERM→KILL 宽限期。
    std::chrono::milliseconds grace{5000};
    /// 是否把 stderr 逐行转发。
    bool forward_stderr{false};
    /// 每个任务保留的输出上限（0 = 不限）。
    std::size_t max_output_bytes{1u << 20};
};

/** @brief 提交给池的一个任务。 */
struct PoolTask final {
    ProcessOptions options;
    /// 覆盖 `options.timeout`；0 时依次回退到池默认值。
    std::chrono::milliseconds timeout{0};
    /// 写入子进程 stdin 的内容。
    std::string input;
    /// 完成回调（在 worker 线程执行；异常被隔离）。
    std::function<void(const struct PoolTaskResult&)> on_complete;
};

/** @brief 任务结果。 */
struct PoolTaskResult final {
    std::uint64_t id{0};
    RunResult run;
    ProcError error;  ///< spawn/执行失败时的错误（`run` 此时无意义）
};

/** @brief 池的累计统计。 */
struct PoolStats final {
    std::size_t workers{0};      ///< 当前存活 worker 线程数（缩容退出后下降）
    std::size_t idle{0};         ///< 空闲 worker
    std::size_t running{0};      ///< 正在执行任务的 worker
    std::size_t queued{0};       ///< 排队任务
    std::uint64_t submitted{0};
    std::uint64_t completed{0};
    std::uint64_t rejected{0};   ///< 队列满拒绝
    std::uint64_t cancelled{0};  ///< Shutdown 时未执行
    std::uint64_t failed{0};     ///< spawn/执行失败
    std::uint64_t timed_out{0};  ///< 子进程超时
};

/**
 * @brief 固定大小进程池（可移动，不可复制）。
 *
 * @details 线程安全：`Submit`/`SubmitAndWait`/`Stats`/`Resize` 可从多线程调用
 *          （含多个线程并发 `Resize`）；`Start`/`Shutdown` 不应与自身并发
 *          （文档约束）。
 */
class ProcessPool final {
public:
    ProcessPool() noexcept;
    ~ProcessPool();

    ProcessPool(const ProcessPool&) = delete;
    ProcessPool& operator=(const ProcessPool&) = delete;
    ProcessPool(ProcessPool&&) noexcept;
    ProcessPool& operator=(ProcessPool&&) noexcept;

    /// 启动池；重复调用返回 `kInvalidArgument`。
    Result<void> Start(const PoolOptions& options);
    bool running() const;

    /**
     * @brief 提交任务。
     * @return 任务 id；队列满返回 `kQueueFull`，已关闭返回 `kNotRunning`。
     */
    Result<std::uint64_t> Submit(PoolTask task);

    /**
     * @brief 提交并等待结果。
     * @param timeout 等待任务完成的总时长（0 = 立即检查；<0 = 永久）。
     * @note 内部在调用线程等待条件变量，不占用 worker。
     */
    Result<PoolTaskResult> SubmitAndWait(PoolTask task,
                                         std::chrono::milliseconds timeout = kWaitForever);

    /**
     * @brief 调整并发槽位；0 返回 `kInvalidArgument`。
     * @note 扩容立即生效；缩容是异步的——目标值立即更新，多余 worker 在队列
     *       为空后退出，`Stats().workers` 随之下降（见文件头「缩扩容」）。
     */
    Result<void> Resize(std::size_t workers);

    /// 优雅关闭（见文件头五步）；可重复调用。
    void Shutdown(std::chrono::milliseconds grace = std::chrono::milliseconds(5000));

    PoolStats Stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace go2cpp::os
