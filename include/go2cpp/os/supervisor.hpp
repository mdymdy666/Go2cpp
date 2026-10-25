/**
 * @file supervisor.hpp
 * @brief 单线程进程监督器：超时阶梯、条件触发、输出泵、配额与优雅停机（R13-R16/R25）。
 *
 * ## 为什么必须单线程
 *
 * 每个受控进程都有「stdout/stderr 两条管道 + 一个退出状态 + 若干定时条件」。
 * 如果每个进程一个线程，10 个进程就是 30 个阻塞点；更糟的是**回收与 kill 的
 * 竞争**会散落在多个线程里。`Supervisor` 把全部受控进程放进**一个**事件循环：
 * `ppoll` 一次等待所有 fd + 一个 `timerfd` + 一个 `eventfd`，没有忙等，也没有
 * 跨线程竞态。
 *
 * ## 事件循环与唤醒源
 *
 * ```
 * ppoll([timerfd, eventfd, child stdout, child stderr, ...])
 *   ├─ timerfd    : 最近的 deadline / 条件 tick / 配额检查
 *   ├─ eventfd    : Stop() 从其他线程请求停止
 *   └─ child fd   : 有输出可读；退出后读到 EOF
 * ```
 *
 * 每轮循环按固定顺序处理：**读输出 → 查退出 → 判定超时/条件/配额 → 执行
 * 阶梯终止 → 回收**。这个顺序保证「超时时已经收到的输出不会丢」。
 *
 * ## 状态机（R16）
 *
 * ```
 * Spawn 成功:        kCreated -> kRunning
 * deadline/条件/Stop: kRunning -> kTerminating   (发 SIGTERM)
 * grace 到期:        kTerminating -> kKilled     (发 SIGKILL)
 * wait 成功:         任意 -> kReaped
 * ```
 *
 * `Terminate` 幂等：重复请求只会在 grace 到期后补一次 SIGKILL。
 *
 * ## 线程模型
 *
 * - `Spawn`/`Add`/`PollOnce`/`Run`/`Wait`/`Snapshot` **只能在同一个线程**调用
 *   （通常是主线程或专用监督线程）；
 * - `Stop()` **线程安全**：设置原子标志并写 eventfd；真正的终止清理发生在
 *   事件循环线程；
 * - `NotifyHeartbeat()` 线程安全（原子时间戳），供心跳线程/网络回调使用。
 *
 * ## 输出与日志
 *
 * - stdout/stderr 被持续排空（避免子进程写满管道死锁），保留上限
 *   `max_output_bytes`（0 = 不限）；超出部分丢弃并计数；
 * - `forward_stderr == true` 时按行转发到 `StderrLineSink`（全局 sink 与
 *   `options.stderr_sink` 二选一，后者优先），带 pid/pgid；
 * - `options.event_sink` 非空时覆盖全局事件 sink，便于测试捕获。
 *
 * ## 与 Fiber 的关系
 *
 * `Run`/`PollOnce` 会阻塞调用线程（`ppoll`）。**不要把它放进 Fiber**；
 * 需要协程化时用独立线程跑 `Run`，或把 fd 交给 `IOManager`（见文档 R20）。
 */
#pragma once

#include "go2cpp/os/proc.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace go2cpp::os {

using ManagedId = std::uint64_t;

/**
 * @brief 条件求值时的上下文快照。
 *
 * @details 引用字段（`info`/`stdout_tail`/`stderr_tail`）只在 `Evaluate` 调用
 *          期间有效，条件实现**不得**保存指针。
 */
struct ConditionContext final {
    pid_t pid{-1};
    const ProcessInfo* info{nullptr};
    std::chrono::steady_clock::time_point now{};
    std::chrono::steady_clock::time_point started{};
    std::chrono::steady_clock::time_point last_heartbeat{};
    std::chrono::steady_clock::time_point last_stdout{};
    std::chrono::steady_clock::time_point last_stderr{};
    std::string_view stdout_tail;   ///< 最近 4 KiB stdout
    std::string_view stderr_tail;   ///< 最近 4 KiB stderr
    std::uint64_t stdout_bytes{0};
    std::uint64_t stderr_bytes{0};
};

/**
 * @brief 健康检查谓词。
 *
 * @details `Evaluate` 返回 true 表示健康；返回 false 表示「条件触发，应销毁
 *          该进程」，并在 `reason` 写入原因。**每个 Condition 实例只挂一个
 *          进程**（内部可保存失败计数等状态）；需要复用时给每个进程创建独立
 *          实例。
 *
 *          Supervisor 会隔离异常：`Evaluate` 抛出异常视为「本次不触发」并记录
 *          事件，不会让整个监督循环崩溃。
 */
class Condition {
public:
    virtual ~Condition() = default;

    /// 求值周期；Supervisor 取所有条件与 `tick` 的最小值作为定时器周期。
    virtual std::chrono::milliseconds Interval() const noexcept {
        return std::chrono::milliseconds(1000);
    }

    /// 返回 true 健康；false 触发销毁并写 reason。
    virtual bool Evaluate(const ConditionContext& context, std::string* reason) = 0;

    /// 人类可读描述（事件/日志用）。
    virtual std::string Describe() const = 0;
};

/**
 * @brief 心跳丢失：`now - last_heartbeat > timeout` 即触发。
 * @param timeout 允许的心跳间隔。
 * @param interval 求值周期。
 * @note 从未收到心跳时以 `started` 为基准，因此「子进程一直不心跳」也会触发。
 */
std::shared_ptr<Condition> MakeHeartbeatLostCondition(
    std::chrono::milliseconds timeout,
    std::chrono::milliseconds interval = std::chrono::milliseconds(100));

/// RSS 超阈值即触发（读 `/proc/<pid>/stat`）。
std::shared_ptr<Condition> MakeRssCondition(
    std::uint64_t max_rss_bytes,
    std::chrono::milliseconds interval = std::chrono::milliseconds(200));

/**
 * @brief 连续失败探测器：`probe` 连续 `failures` 次返回 false 才触发。
 * @details 用于「连续 K 次探测失败」语义；单次抖动不会杀进程。
 */
std::shared_ptr<Condition> MakeProbeFailureCondition(
    int failures, std::function<bool(const ConditionContext&)> probe,
    std::chrono::milliseconds interval = std::chrono::milliseconds(500));

/**
 * @brief 输出匹配：在最近输出里找到 `needle` 即触发（错误模式检测）。
 * @param stderr_only true 只查 stderr，false 查 stdout+stderr。
 */
std::shared_ptr<Condition> MakeOutputMatchCondition(
    std::string needle, bool stderr_only = false,
    std::chrono::milliseconds interval = std::chrono::milliseconds(100));

/** @brief Supervisor 配置。 */
struct SupervisorOptions final {
    /// 每个进程保留的输出上限（字节）；0 = 不限。
    std::size_t max_output_bytes{1u << 20};
    /// 是否把子进程 stderr 逐行转发（R18）。
    bool forward_stderr{true};
    /// 基础 tick（条件/配额/输出泵的最小周期）。
    std::chrono::milliseconds tick{50};
    /// `ManagedSpec::grace == 0` 时使用的宽限期。
    std::chrono::milliseconds default_grace{5000};
    /// 默认配额（`ManagedSpec::quotas` 未覆盖时使用）。
    QuotaLimits quotas{};
    /// 可注入时钟；空 = `MakeRealClock()`。
    std::shared_ptr<Clock> clock;
    /// 可注入后端；空 = 默认后端（`ManagedSpec::backend` 可再覆盖）。
    std::shared_ptr<ProcessBackend> backend;
    /// 事件 sink 覆盖（空 = 全局）。
    ProcEventSink event_sink;
    /// stderr 行 sink 覆盖（空 = 全局）。
    StderrLineSink stderr_sink;
    /// 是否收集输出（关闭可省内存；`CollectedStdout` 会为空）。
    bool capture_output{true};
};

/** @brief 每个受管进程的规格。 */
struct ManagedSpec final {
    ProcessOptions options;
    /// 绝对 deadline（从 Spawn 起算）；0 = 用 `options.timeout`；都 0 = 不限时。
    std::chrono::milliseconds deadline{0};
    /// TERM→KILL 宽限期；0 = 用 `SupervisorOptions::default_grace`。
    std::chrono::milliseconds grace{0};
    /// 条件列表（每个条件实例只挂本进程）。
    std::vector<std::shared_ptr<Condition>> conditions;
    /// 配额覆盖；全 0 时使用 Supervisor 默认。
    QuotaLimits quotas{};
    /// 可注入后端；空 = Supervisor 默认。
    std::shared_ptr<ProcessBackend> backend;
    /// 退出回调（在监督线程执行；异常被隔离）。
    std::function<void(const ExitStatus& status, const std::string& reason)> on_exit;
};

/** @brief 受管进程的只读快照。 */
struct ManagedSnapshot final {
    ManagedId id{0};
    pid_t pid{-1};
    pid_t pgid{-1};
    LifecycleState state{LifecycleState::kCreated};
    bool terminated_by_condition{false};
    bool terminated_by_quota{false};
    bool timed_out{false};
    std::string trigger_reason;
    std::optional<ExitStatus> exit_status;
    ResourceUsage usage;
    std::chrono::steady_clock::time_point started{};
    std::chrono::milliseconds deadline{0};
};

/** @brief Supervisor 累计统计。 */
struct SupervisorStats final {
    std::uint64_t spawned{0};
    std::uint64_t completed{0};
    std::uint64_t timed_out{0};
    std::uint64_t condition_triggered{0};
    std::uint64_t quota_triggered{0};
    std::uint64_t reaped{0};
    std::size_t active{0};
    std::size_t peak_active{0};
};

/**
 * @brief 单线程进程监督器。
 *
 * @details 使用方式（完整示例见 `docs/os-process.md`）：
 *
 * ```cpp
 * Supervisor supervisor;
 * ManagedSpec spec;
 * spec.options.argv = {"/bin/sh", "-c", "while :; do echo tick; sleep 0.2; done"};
 * spec.deadline = std::chrono::seconds(2);
 * auto id = supervisor.Spawn(spec);
 * supervisor.Run();                       // 阻塞到全部退出
 * auto out = supervisor.CollectedStdout(*id);
 * ```
 */
class Supervisor final {
public:
    explicit Supervisor(SupervisorOptions options = {});
    ~Supervisor();

    Supervisor(const Supervisor&) = delete;
    Supervisor& operator=(const Supervisor&) = delete;
    Supervisor(Supervisor&&) noexcept;
    Supervisor& operator=(Supervisor&&) noexcept;

    /**
     * @brief 启动并接管一个新进程。
     * @return 受管 id；失败返回 `ProcError`（spawn 失败时不登记）。
     */
    Result<ManagedId> Spawn(const ManagedSpec& spec);

    /// 接管一个已启动的 `Process`（`Process` 必须尚未被回收）。
    Result<ManagedId> Add(Process&& process, ManagedSpec spec);

    /// 记录一次心跳（线程安全）。
    Result<void> NotifyHeartbeat(ManagedId id);

    /**
     * @brief 请求停止全部受管进程（线程安全、幂等）。
     * @param grace 覆盖每个进程的宽限期；0 表示用各自配置。
     * @details 只设置标志并唤醒事件循环；真正的 SIGTERM/回收由监督线程
     *          完成。`Run` 返回后保证没有僵尸。
     */
    void Stop(std::chrono::milliseconds grace = std::chrono::milliseconds(0));

    /**
     * @brief 执行一轮事件循环。
     * @param max_wait 本轮最长等待时间（仍会被最近 deadline 提前唤醒）。
     * @return true 表示还有活跃进程（应继续循环）；false 表示全部结束或已停止。
     * @note 只能在监督线程调用；可被测试用来逐步推进（配合注入时钟）。
     */
    bool PollOnce(std::chrono::milliseconds max_wait = std::chrono::milliseconds(100));

    /// 循环 `PollOnce` 直到没有活跃进程（或 Stop 后清理完毕）。
    void Run();

    /// 是否还有活跃进程。
    bool active() const;
    std::size_t active_count() const;

    /// 快照；id 不存在返回 nullopt。
    std::optional<ManagedSnapshot> Snapshot(ManagedId id) const;

    /// 已收集的输出（可能被 `max_output_bytes` 截断）。
    std::string CollectedStdout(ManagedId id) const;
    std::string CollectedStderr(ManagedId id) const;

    /**
     * @brief 等待指定进程结束（内部驱动事件循环）。
     * @return 成功返回退出状态；超时返回 `kTimeout`（进程仍在监督）。
     */
    Result<ExitStatus> Wait(ManagedId id,
                            std::chrono::milliseconds timeout = kWaitForever);

    SupervisorStats stats() const;

    /// 覆盖事件 sink（空 = 恢复全局）。
    void SetEventSink(ProcEventSink sink);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace go2cpp::os
