#pragma once

/**
 * @file diagnostic_agent.hpp
 * @brief Doctor：在线诊断 + 受控解锁的进程内代理（单线程命令平面）。
 *
 * ## 定位
 *
 * `DebugServer`（`GO2CPP_DEBUG_DUMP=ON`）提供的是**只读**快照与文本 dump；
 * Doctor 在其上补三件 DebugServer 明确不做的事：
 *
 * 1. **受控唤醒**（force-wake）：只作用于白名单里「可安全重试」的等待；
 * 2. **等待图**（deadlock-report）：把 scheduler / ParkingCondition /
 *    Channel / Mutex 的等待关系拼成「谁在等谁」；
 * 3. **Mutex 级受控解锁**（mutex-unlock）：仅当 owner 已被证明不可能再访问
 *    临界区时，才走既有 `Mutex::Unlock()` 释放一次；其余情况一律 `refused`。
 *
 * ## 线程模型：单线程命令平面
 *
 * ```text
 *   transport（Unix socket / QUIC stream / topic 字节流）
 *        |  Recv 帧（只做 I/O，不碰运行时状态）
 *   [接收线程] --enqueue--> [命令队列] --dequeue--> [执行线程（唯一）]
 *                                                        |
 *                                                  执行命令 + 采集快照
 *                                                        |
 *   transport.Send（按 stream_id / peer 回写，分块）
 * ```
 *
 * - **执行线程只有一个**：所有诊断操作串行执行，因此同一份 `status` 里的
 *   多个字段是「同一时刻的相邻读取」，不会与另一条诊断命令交叉；
 * - 接收线程只做 `Recv` 与入队，不读运行时状态，因此长命令执行期间新命令
 *   仍能被接收、按到达顺序排队，不会丢；
 * - 两条线程都**不经过 Scheduler**：即使所有 M 都 park、runqueue 为空，
 *   Doctor 的执行线程仍能响应（它只阻塞在条件变量/transport 上）。
 *
 * ## 为什么必须有「队列」而不是收到命令就执行
 *
 * 传输层可能同时收到 10 条命令（QUIC 的 10 条 stream）。若直接在接收线程里
 * 执行，快照就会与其它诊断命令互相穿插；队列把「接收」与「执行」解耦，
 * 执行侧严格串行，响应按 `stream_id` 原路返回。
 *
 * ## 传输层：可插拔
 *
 * `DiagnosticTransport` 只要求 `Recv(envelope, timeout)` / `Send(envelope)`。
 * 内置实现：
 *
 * - `UnixSocketDiagnosticTransport`：本地默认，`/tmp/go2cpp-<pid>-doctor.sock`，
 *   每连接一条命令（短连接），帧协议见 `diagnostic_protocol.hpp`；
 * - `MemoryDiagnosticTransport`：进程内测试用（无 socket、无文件）；
 * - QUIC / 自定义实现：见 `docs/diagnostic-channel.md`。用户的「网络 topic
 *   传输字节流」只需实现这两个方法并把字节流按帧格式切分即可。
 *
 * ## 安全白名单（关键）
 *
 * `force-wake` **不接受任意地址或任意对象**，只接受四个 scope：
 *
 * | scope | 允许的对象 | 为什么安全 |
 * |---|---|---|
 * | `scheduler-idle` | Scheduler 的空闲 worker | 只唤醒 worker 重新检查 runqueue，不改状态字 |
 * | `parking-condition` | 已注册的 `ParkingCondition` | `notify_all` 是标准条件变量语义，等待方**重检谓词**后继续等或返回 |
 * | `channel` | 已注册的 `Channel<T>` | 只发 spurious wake，不伪造数据、不关闭 channel；阻塞方重检条件 |
 * | `all` | 以上三者 | 组合 |
 *
 * 以下情况一律 `refused`，不「硬来」：
 *
 * - `scope=mutex`：Mutex 等待者醒来后不会重试同一把锁（Go 风格 FIFO 交接），
 *   盲目唤醒会破坏锁状态；正确做法是 `mutex-unlock`；
 * - `mutex-unlock`：owner 仍存活 / owner 不可知 / 没有等待者；
 * - 未注册的对象：`not-registered`。
 *
 * ## 默认零开销
 *
 * 本文件与 `diagnostic_agent.cpp` 只在 `-DGO2CPP_DIAGNOSTIC=ON` 时编译进
 * `go2cpp_debug`。OFF（默认）构建没有 Doctor 代码、没有线程、没有 socket；
 * 实例化后也只有在 `Start()` 之后才创建线程。
 *
 * ## 已知边界（如实声明）
 *
 * 1. Doctor 的**执行线程**不经过 Scheduler，但它调用的快照接口（如
 *    `Scheduler::machines()`）会取运行时内部互斥量。如果业务在持有该锁时
 *    永久卡死（不是 park，而是原生死锁），命令会等待该锁而挂起；此时
 *    `status` 也可能挂起。本代理不解决「运行时内部锁本身被卡死」的情形。
 * 2. 等待图只能到**资源级**：现有内省没有「G 当前阻塞在哪个 Mutex」的权威
 *    记录（`Mutex` 的 owner 是近似值），因此对 Mutex 边标注
 *    `waiter_ids=unavailable`，不虚构 G→Mutex 的边。ParkingCondition 的
 *    等待节点会带受管 G 的 gid（本轮补充）。
 * 3. `mutex-unlock` 只接受**注册名**，不接受裸地址：任意地址解引用会变成
 *    「诊断工具把目标进程打崩」的捷径。
 * 4. QUIC transport 的完整实现依赖 `-DGO2CPP_WITH_PICOQUIC=ON`；本版本提供
 *    协议与接口，QUIC 适配的落地范围见 `docs/diagnostic-channel.md`。
 */

#include "go2cpp/debug/diagnostic_protocol.hpp"

#include "go2cpp/concurrency/channel.hpp"
#include "go2cpp/concurrency/core/parking_condition.hpp"
#include "go2cpp/concurrency/scheduler/scheduler.hpp"
#include "go2cpp/debug/debug_dump.hpp"
#include "go2cpp/debug/debug_server.hpp"
#include "go2cpp/sync.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace go2cpp::debug {

// ---------------------------------------------------------------------------
// 传输层
// ---------------------------------------------------------------------------

/// 一次收发携带的 transport 元数据（不进入线上帧格式）。
struct DiagnosticEnvelope {
    DiagnosticFrame frame;
    /// transport 私有的回信目标：Unix socket = 连接 id；QUIC = stream 句柄；
    /// Memory = 固定值。Agent 原样回传，不解释它的含义。
    std::uint64_t peer{0};
};

/**
 * @brief 可插拔传输层接口。
 *
 * ## 约定
 *
 * - `Recv` 必须在 `timeout_ms` 内返回；超时返回 false 且 `error` 为空
 *   （Doctor 用它做周期性的死锁检测，不允许无限阻塞）；
 * - `Send` 与 `Recv` 可能在不同线程被调用（接收线程 / 执行线程），实现
 *   必须自己保证线程安全；
 * - `Start` 失败时返回 false 并填 `error`，Doctor 不启动任何线程。
 *
 * ## 自定义实现（用户的 topic 字节流）
 *
 * 用户的「网络 topic 传输字节流」项目把字节流当作可靠有序通道即可：
 *
 * ```text
 * topic 消息 payload  :=  G2CD 帧（可粘包/拆包）
 * 对端              :=  按 24 字节头 + payload_len 切帧
 * ```
 *
 * 实现方只需要：从 topic 收字节 -> `DiagnosticFrameParser` -> `envelope.frame`；
 * 把 `Send` 的帧编码后写入 topic。`peer` 用 topic session id 即可。
 */
class DiagnosticTransport {
public:
    virtual ~DiagnosticTransport() = default;

    /// 建立/绑定传输（Unix socket 在这里 listen；QUIC 在这里建连）。
    virtual bool Start(std::string* error) {
        (void)error;
        return true;
    }
    /// 停止并释放资源（幂等）。
    virtual void Stop() {}
    /// 非阻塞/带超时收一帧。
    virtual bool Recv(DiagnosticEnvelope& envelope, int timeout_ms,
                      std::string* error) = 0;
    /// 发送一帧（线程安全）。
    virtual bool Send(const DiagnosticEnvelope& envelope,
                      std::string* error) = 0;
    /// 人类可读的名字（`unix` / `memory` / `quic` / 自定义）。
    virtual const char* name() const noexcept = 0;
    /// 当前是否已启动。
    virtual bool running() const noexcept = 0;
};

/// 进程内内存 transport：测试与「把 Doctor 嵌进宿主消息循环」用。
class MemoryDiagnosticTransport final : public DiagnosticTransport {
public:
    /// 客户端侧注入一条请求（可在任意线程调用）。
    bool PushRequest(DiagnosticFrame frame);
    /// 取走一条响应（没有时返回 false）。测试用。
    bool PopResponse(DiagnosticFrame& frame);

    bool Start(std::string* error) override;
    void Stop() override;
    bool Recv(DiagnosticEnvelope& envelope, int timeout_ms,
              std::string* error) override;
    bool Send(const DiagnosticEnvelope& envelope, std::string* error) override;
    const char* name() const noexcept override { return "memory"; }
    bool running() const noexcept override { return m_running.load(); }

    std::size_t pending_requests() const;

private:
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<DiagnosticFrame> m_requests;
    std::deque<DiagnosticFrame> m_responses;
    std::atomic<bool> m_running{false};
    std::uint64_t m_next_peer{1};
};

/**
 * @brief Unix domain socket 传输（本地默认实现）。
 *
 * - 路径：`/tmp/go2cpp-<pid>-doctor.sock`（可用 `GO2CPP_DOCTOR_SOCKET` 覆盖）；
 * - 每连接一条命令：`accept` -> 读一个完整帧 -> 入队 -> 执行线程 `Send`
 *   回写 -> 关闭连接。与既有 `DebugServer` 的短连接模型一致；
 * - `Recv` 里用 `poll` 同时监听 listen fd 与所有活跃连接，每个连接最多
 *   缓冲一帧；超过 16 MiB 或解析失败立即关闭，不拖垮进程。
 */
class UnixSocketDiagnosticTransport final : public DiagnosticTransport {
public:
    explicit UnixSocketDiagnosticTransport(std::string path,
                                           unsigned int mode = 0600U);
    ~UnixSocketDiagnosticTransport() override;

    bool Start(std::string* error) override;
    void Stop() override;
    bool Recv(DiagnosticEnvelope& envelope, int timeout_ms,
              std::string* error) override;
    bool Send(const DiagnosticEnvelope& envelope, std::string* error) override;
    const char* name() const noexcept override { return "unix"; }
    bool running() const noexcept override { return m_running.load(); }

    const std::string& path() const noexcept { return m_path; }
    std::string last_error() const;

private:
    struct Connection;
    void DropConnection(std::uint64_t peer);
    bool TryReadFrame(Connection& connection);

    std::string m_path;
    unsigned int m_mode{0600U};
    int m_listen_fd{-1};
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stop{false};
    mutable std::mutex m_mutex;
    std::vector<std::shared_ptr<Connection>> m_connections;
    std::uint64_t m_next_peer{1};
    std::string m_last_error;
};

// ---------------------------------------------------------------------------
// 命令与配置
// ---------------------------------------------------------------------------

/// Doctor 配置。
struct DiagnosticAgentConfig {
    /// Unix socket 路径；空 = `/tmp/go2cpp-<pid>-doctor.sock`。
    std::string socket_path;
    /// 接收超时（毫秒）；同时是死锁检测的唤醒粒度。
    int recv_timeout_ms{100};
    /// 命令队列上限；满时立即回 `queue-full`，不无限堆积。
    std::size_t max_queue{1024};
    /// 是否启用「全局无进展」检测（默认开）。
    bool enable_deadlock_detection{true};
    /// 连续无进展多久判定为 deadlock（默认 5 秒）。
    std::chrono::milliseconds deadlock_threshold{5000};
    /// 检测到 deadlock 时是否自动 `force-wake all`（默认关：只报告）。
    bool auto_force_wake{false};
    /// 检测采样间隔（默认 500ms；与 recv_timeout 取较大者生效）。
    std::chrono::milliseconds detect_interval{500};
    /// 响应分块大小（字节）；0 表示不分块。
    std::size_t chunk_bytes{kDiagnosticDefaultChunk};
    /// 本地 socket 权限（八进制）；默认 0600。
    unsigned int socket_mode{0600};
};

/// 一条命令的执行结果。
struct DiagnosticCommandResult {
    bool ok{false};
    /// 结果文本（text 模式）或 JSON（json=true）。
    std::string payload;
    bool json{false};
    /// 失败时的稳定错误码（`unknown-command` / `refused` / ...）。
    std::string error_code;
    std::string error_message;
    /// 命令从入队到开始执行的等待时间。
    std::uint64_t queue_wait_us{0};
    /// 命令执行耗时。
    std::uint64_t duration_us{0};
};

/// Doctor 自身统计（只读快照）。
struct DiagnosticAgentStats {
    std::uint64_t accepted{0};       ///< 收到的请求帧数。
    std::uint64_t executed{0};       ///< 执行完成的命令数。
    std::uint64_t failed{0};         ///< 返回错误的命令数。
    std::uint64_t rejected{0};       ///< 队列满等原因直接拒绝的请求数。
    std::uint64_t dropped{0};        ///< 响应发送失败的请求数。
    std::uint64_t queue_depth{0};    ///< 当前排队数。
    std::uint64_t max_queue_depth{0};
    std::uint64_t concurrent_peak{0}; ///< 同时执行命令数的峰值（期望恒为 1）。
    std::uint64_t deadlock_events{0};
    std::uint64_t auto_force_wakes{0};
    /// 已执行的 `mutex-unlock --force` 次数（危险操作审计；默认拒绝时不计）。
    std::uint64_t forced_unlocks{0};
    std::uint64_t fiber_jobs{0};
    std::uint64_t fiber_failures{0};
};

/// 死锁检测判定。
enum class DeadlockVerdict {
    kUnknown,      ///< 没有注册 scheduler 或数据不足。
    kIdle,         ///< 无任务、无等待者：空闲不是死锁。
    kProgressing,  ///< 指纹在变化：有进展。
    kStalled,      ///< 无进展但未到阈值。
    kDeadlock,     ///< 超过阈值且存在等待者、所有 M 都不在 Running。
};

// ---------------------------------------------------------------------------
// DiagnosticAgent
// ---------------------------------------------------------------------------

/**
 * @brief Doctor 单例：单线程命令平面 + 可插拔 transport + 白名单唤醒。
 *
 * ## 生命周期
 *
 * `Instance()` 返回堆上常驻单例（不参与静态析构顺序）；`Start()` 启动
 * 接收/执行两个线程并绑定 transport；`Stop()` 幂等停止。**未调用 `Start()`
 * 时没有线程**，但仍可用 `Execute()` 做进程内诊断（测试/嵌入）。
 *
 * ## 注册
 *
 * 所有可观测/可唤醒对象都必须显式注册；未注册的对象不会被 `force-wake` /
 * `mutex-unlock` 触碰。注册只保存指针（Channel 保存 weak_ptr），不延长
 * 生命周期；调用方必须保证对象活到 `Stop()` 或进程结束。
 *
 * ## 线程安全
 *
 * 注册、`Start/Stop`、`Execute` 可从任意线程调用；`Execute` 内部用执行锁
 * 串行化，并统计 `concurrent_peak`（正常情况下恒为 1）。
 */
class DiagnosticAgent final {
public:
    /// 进程内唯一实例（堆上常驻）。
    static DiagnosticAgent& Instance();

    ~DiagnosticAgent();
    DiagnosticAgent(const DiagnosticAgent&) = delete;
    DiagnosticAgent& operator=(const DiagnosticAgent&) = delete;

    /**
     * @brief 启动 Doctor（创建接收/执行线程并启动 transport）。
     * @param config 配置；默认 Unix socket。
     * @param transport 可选的传输实现；空表示按 `config.socket_path` 建
     *        Unix socket。必须在 `Start()` 前设置。
     * @return 已运行或本次启动成功返回 true。
     */
    bool Start(const DiagnosticAgentConfig& config = {});
    bool Start(const DiagnosticAgentConfig& config,
               std::shared_ptr<DiagnosticTransport> transport);
    void Stop();

    bool running() const noexcept { return m_running.load(); }
    std::string socket_path() const;
    std::string last_error() const;
    const DiagnosticAgentConfig& config() const noexcept { return m_config; }

    /// 当前 transport 名字（未启动时为空）。
    std::string transport_name() const;

    // ---- 注册（线程安全） ----

    /// 注册调度器（可多个）；同时把 scheduler 桥接到 DebugServer 的
    /// `scheduler` 组件（若尚未注册），使 `dump` 也能看到它。
    bool RegisterScheduler(const std::string& name, Scheduler& scheduler);
    /// 注册 ParkingCondition（可唤醒、可 dump）。
    bool RegisterParkingCondition(const std::string& name,
                                  core::ParkingCondition& condition);
    /**
     * @brief 注册 Channel（保存 weak_ptr：销毁后自动 unavailable）。
     * @note 同时调用 `DebugServer::RegisterChannel`，让 `dump channel` 复用
     *       既有 schema，不另造一份。
     */
    template <typename T>
    bool RegisterChannel(const std::string& name,
                         std::shared_ptr<Channel<T>> channel) {
        if (name.empty() || !channel) {
            return false;
        }
        std::weak_ptr<Channel<T>> weak = channel;
        auto wake = [weak]() -> std::size_t {
            const auto locked = weak.lock();
            return locked ? locked->ForceWakeWaiters() : 0U;
        };
        auto describe = [weak]() -> std::string {
            const auto locked = weak.lock();
            if (!locked) {
                return "unavailable";
            }
            const auto snapshot = locked->DebugSnapshot();
            return "send_waiters=" + std::to_string(snapshot.send_waiters) +
                   " recv_waiters=" + std::to_string(snapshot.recv_waiters) +
                   " capacity=" + std::to_string(snapshot.capacity) +
                   " length=" + std::to_string(snapshot.length) +
                   " closed=" + (snapshot.closed ? "true" : "false");
        };
        auto dump = [weak]() -> DumpPayload {
            const auto locked = weak.lock();
            if (!locked) {
                return DumpPayload::Unavailable("destroyed");
            }
            return DumpChannel(locked->DebugSnapshot());
        };
        DebugServer::Instance().Register(name, "Channel<T> waiters", dump);
        return AddWakeTarget(name, WakeSupport::kChannel, std::move(wake),
                             std::move(describe));
    }
    /// 注册 Mutex（只用于 dump / 等待图 / `mutex-unlock`；**不可 force-wake**）。
    bool RegisterMutex(const std::string& name, sync::Mutex& mutex);

    /// 注册可在诊断 Fiber 里执行的函数（`inject-fiber fn=<name>`）。
    bool RegisterFiberFunction(const std::string& name,
                               std::function<std::string()> function);
    /// 列出已注册的 inject-fiber 函数名。
    std::vector<std::string> FiberFunctions() const;

    // ---- 注销（测试/嵌入方在对象销毁前必须调用） ----
    //
    // 注册只保存指针，不延长生命周期；生产代码通常让被观测对象活到进程结束，
    // 但测试与热替换场景会先销毁对象。这些接口只移除注册项，不触碰对象。
    bool UnregisterTarget(const std::string& name);
    bool UnregisterScheduler(const std::string& name);
    bool UnregisterMutex(const std::string& name);
    void ResetRegistrations();

    /**
     * @brief 在 Scheduler 里注入一个常驻「诊断 Fiber」。
     *
     * 该 Fiber 只 park 在 Doctor 私有的 `ParkingCondition` 上，不占 runqueue；
     * 命令到达时由执行线程 `notify` + `Scheduler::ForceWakeIdle()` 唤醒，
     * 在**受管 G 的 GMP 上下文**里执行注册函数（能读 `current_task()` /
     * `current_processor_id()` / 遍历 runqueue）。
     *
     * @note 每个 Agent 只保留一个诊断 Fiber；重复调用返回 false。
     * @note 该 Fiber 是普通 `spawn` 的 G，与业务 G 共享 P 的 runqueue；
     *       它只在命令执行期间短暂运行，不会长期占用 P。
     */
    bool InstallDiagnosticFiber(const std::string& name, Scheduler& scheduler);
    bool diagnostic_fiber_installed() const;
    /// 让诊断 Fiber 退出（尽力而为；不等待）。
    void StopDiagnosticFiber();

    // ---- 命令入口 ----

    /**
     * @brief 直接执行一条命令（同步、串行）。
     * @note 供测试与「已在自己的单线程消息循环里」的嵌入方使用；不在队列里，
     *       但仍与 Doctor 执行线程互斥。
     */
    DiagnosticCommandResult Execute(const std::string& command);

    /// 统计快照。
    DiagnosticAgentStats stats() const;
    /// 当前死锁判定。
    DeadlockVerdict deadlock_verdict() const;
    /// 当前「无进展」时长（毫秒）。
    std::uint64_t no_progress_ms() const;

    /// 默认 socket 路径（不访问文件系统）。
    static std::string DefaultSocketPath();
    /// 解析 `GO2CPP_DOCTOR_SOCKET` 覆盖后的路径。
    static std::string ResolveSocketPath(const std::string& configured);

    /// 命令列表（`help` 的原样文本）。
    static std::string HelpText();

private:
    /// 唤醒能力白名单（决定 force-wake 是否允许触碰）。
    enum class WakeSupport {
        kNone,             ///< 不可唤醒（Mutex 等）
        kSchedulerIdle,    ///< Scheduler 空闲等待
        kParkingCondition, ///< ParkingCondition 条件等待
        kChannel,          ///< Channel 阻塞收发
    };

    struct WakeTarget {
        std::string name;
        WakeSupport support{WakeSupport::kNone};
        std::function<std::size_t()> wake;
        std::function<std::string()> describe;
    };

    struct QueuedCommand {
        DiagnosticEnvelope envelope;
        std::chrono::steady_clock::time_point enqueued;
    };

    struct MutexRegistration {
        std::string name;
        sync::Mutex* mutex{nullptr};
    };

    struct FiberJob {
        bool stop{false};
        bool has_job{false};
        bool done{false};
        std::string function_name;
        std::function<std::string()> function;
        std::string result;
        bool ok{true};
        std::string error;
    };

    DiagnosticAgent();

    bool AddWakeTarget(const std::string& name, WakeSupport support,
                       std::function<std::size_t()> wake,
                       std::function<std::string()> describe);
    void ReceiverMain();
    void ExecutorMain();
    void HandleFrame(const DiagnosticEnvelope& envelope);
    DiagnosticCommandResult RunCommand(const std::string& command,
                                       std::uint64_t queue_wait_us);
    bool SendResult(const DiagnosticEnvelope& request,
                    const DiagnosticCommandResult& result);

    // 命令实现
    DiagnosticCommandResult CmdStatus(const std::string& argument);
    DiagnosticCommandResult CmdDump(const std::string& argument);
    DiagnosticCommandResult CmdForceWake(const std::string& argument);
    DiagnosticCommandResult CmdDeadlockReport(const std::string& argument);
    DiagnosticCommandResult CmdMutexUnlock(const std::string& argument);
    DiagnosticCommandResult CmdInjectFiber(const std::string& argument);
    DiagnosticCommandResult CmdStats(const std::string& argument);
    DiagnosticCommandResult Fail(std::string code, std::string message) const;

    // 内部工具
    void MaybeDetectDeadlock();
    std::uint64_t ProgressFingerprint();
    std::string BuildDeadlockText(bool json);
    std::size_t ForceWakeScope(const std::string& scope, bool* refused,
                               std::string* reason, std::string* detail);
    bool RunOnDiagnosticFiber(const std::string& function_name,
                              std::string* output, std::string* error,
                              std::uint64_t timeout_ms);
    void DiagnosticFiberLoop();

    mutable std::mutex m_lifecycle_mutex;
    DiagnosticAgentConfig m_config;
    std::shared_ptr<DiagnosticTransport> m_transport;
    std::shared_ptr<MemoryDiagnosticTransport> m_own_transport;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stop{false};
    std::uint64_t m_start_ns{0};
    std::string m_socket_path;
    std::string m_last_error;
    mutable std::mutex m_error_mutex;

    std::thread m_receiver;
    std::thread m_executor;
    std::mutex m_queue_mutex;
    std::condition_variable m_queue_cv;
    std::deque<QueuedCommand> m_queue;
    std::atomic<std::size_t> m_queue_depth{0};

    mutable std::mutex m_exec_mutex;
    std::atomic<std::uint32_t> m_concurrent{0};

    mutable std::mutex m_registry_mutex;
    std::vector<std::pair<std::string, Scheduler*>> m_schedulers;
    std::vector<std::shared_ptr<WakeTarget>> m_targets;
    std::vector<MutexRegistration> m_mutexes;
    std::vector<std::pair<std::string, std::function<std::string()>>>
        m_fiber_functions;

    std::atomic<std::uint64_t> m_accepted{0};
    std::atomic<std::uint64_t> m_executed{0};
    std::atomic<std::uint64_t> m_failed{0};
    std::atomic<std::uint64_t> m_rejected{0};
    std::atomic<std::uint64_t> m_dropped{0};
    std::atomic<std::uint64_t> m_max_queue_depth{0};
    std::atomic<std::uint64_t> m_concurrent_peak{0};
    std::atomic<std::uint64_t> m_deadlock_events{0};
    std::atomic<std::uint64_t> m_auto_force_wakes{0};
    std::atomic<std::uint64_t> m_forced_unlocks{0};
    std::atomic<std::uint64_t> m_fiber_jobs{0};
    std::atomic<std::uint64_t> m_fiber_failures{0};

    // 死锁检测状态（只由执行线程写；读接口是原子的）。
    std::atomic<std::uint64_t> m_last_progress_ns{0};
    std::atomic<std::uint64_t> m_last_fingerprint{0};
    std::atomic<std::uint64_t> m_no_progress_ms{0};
    std::atomic<DeadlockVerdict> m_verdict{DeadlockVerdict::kUnknown};
    std::uint64_t m_last_detect_ns{0};
    std::uint64_t m_last_deadlock_event_ns{0};

    // 诊断 Fiber 状态。
    mutable std::mutex m_fiber_mutex;
    core::ParkingCondition m_fiber_wake;
    core::ParkingCondition m_fiber_done;
    FiberJob m_fiber_job;
    std::shared_ptr<Task> m_fiber_task;
    Scheduler* m_fiber_scheduler{nullptr};
    std::string m_fiber_name;
    std::atomic<bool> m_fiber_installed{false};
};

}  // namespace go2cpp::debug
