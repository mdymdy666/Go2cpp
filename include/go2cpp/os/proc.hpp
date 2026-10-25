/**
 * @file proc.hpp
 * @brief `go2cpp::os` 的**多进程**能力：启动、等待、回收、信号、状态查询与资源调整。
 *
 * ## 模块定位
 *
 * 本文件是 `go2cpp::os` 进程子层的唯一对外入口，替代 MCP 里两处手写的
 * `fork + pipe + poll + SIGKILL`（`mcp/src/util/process.cpp` 与
 * `mcp/src/tools/robot_control.cpp` 的调用方）。上游是业务代码、`Supervisor`
 * 与 `ProcessPool`；下游是 Linux 的 `fork`/`execve`/`wait4`/`kill`/`/proc`。
 *
 * ## 与 Hook / Fiber 的边界（必读）
 *
 * 项目的 `go2cpp_hook` 会拦截 `read`/`write`/`close`/`poll`/`fcntl`/`dup` 等
 * libc 符号，并在 Fiber 中把当前 Fiber park 给 `IOManager`。**本模块刻意不参与
 * 协程调度**：所有 fd 级的读/写/关闭/轮询都走 `::syscall(SYS_*)` 裸调用，
 * 等价于 hook 提供的 `read_f`/`write_f`/`poll_f`，但**不依赖 `go2cpp_hook`
 * 共享库**（Hook-off 构建同样可用，`file.cpp` 已确立同一先例）。
 *
 * 直接后果是：**在 Fiber 里调用本模块的阻塞 API 会阻塞它所在的 worker 线程**
 * （M），而不是 park Fiber。这与 `os::File` 的语义一致。需要异步等待的调用方
 * 应把进程操作放到独立线程，或在 Fiber 里直接对 `native_fd()` 调用 libc 的
 * `poll`/`read`（走 hook 路径，见 `docs/os-process.md` 的 R20 一节）。
 *
 * `fork`/`execve`/`wait4`/`kill`/`pipe2`/`socketpair` 不在 `HookOp` 列表内，
 * 本模块仍用裸调用，原因不是 Hook，而是**子进程上下文的 async-signal-safe
 * 约束**：`fork` 之后只能调用不分配内存、不拿锁的系统调用。
 *
 * ## 统一超时语义（全文一致）
 *
 * 所有接受 `std::chrono::milliseconds timeout` 的接口：
 * - `timeout == 0`：立即返回（非阻塞探测）；
 * - `timeout < 0`（即 `kWaitForever`）：永久等待，直到条件满足或出错；
 * - `timeout > 0`：最多等待该毫秒数，到期返回 `ProcErrorCode::kTimeout`。
 *
 * ## 错误与所有权
 *
 * - 所有失败路径返回 `Result<T>`/`ProcError`，**不把 `errno` 留给调用方猜**；
 *   `ProcError::system_error` 保留原始 errno 供诊断，但错误分类由 `code` 决定。
 * - `Process` 是 **RAII 句柄**：析构时若子进程尚未回收，会 `SIGKILL` 并
 *   `wait`（兜底，避免僵尸）。主动回收请调用 `Wait`/`TryWait`/`Run`。
 * - 子进程的 stdin/stdout/stderr 三条通道归 `Process` 所有；`Close()` 只关
 *   该流，不杀进程。
 *
 * ## 阅读建议
 *
 * 先读 `ProcessOptions` 的字段注释，再看 `Process::Spawn` 的错误归因
 * （exec 失败通过 CLOEXEC 的错误管道回传 errno），然后看 `Run` 如何同时
 * 排空 stdout/stderr 并处理超时（避免「子进程写满管道 → 父进程等 wait」死锁）。
 */
#pragma once

#include <sys/types.h>

#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace go2cpp::os {

class MessageChannel;
class ProcessBackend;

// ---------------------------------------------------------------------------
// 时间常量
// ---------------------------------------------------------------------------

/// 永久等待：所有 `timeout` 参数传它表示「直到条件满足」。
inline constexpr std::chrono::milliseconds kWaitForever{-1};
/// 立即返回：非阻塞探测。
inline constexpr std::chrono::milliseconds kNoWait{0};

/** @brief 判断超时参数是否为「永久等待」。 */
inline constexpr bool IsWaitForever(std::chrono::milliseconds timeout) noexcept {
    return timeout.count() < 0;
}

// ---------------------------------------------------------------------------
// 错误码与结果类型（R17）
// ---------------------------------------------------------------------------

/**
 * @brief `os` 进程/通信子层的稳定错误码。
 *
 * @details 错误码只描述**语义分类**，具体系统原因保留在 `ProcError::system_error`。
 *          这样调用方可以按类别分支，又不会在不同内核版本/文件系统上失配。
 *          逐项含义：
 *
 * | 错误码 | 含义 | 典型 errno |
 * |---|---|---|
 * | `kInvalidArgument` | 参数非法（空 argv、负大小、越界优先级） | `EINVAL` |
 * | `kSpawnFailed` | fork/pipe/重定向失败，子进程未创建 | `EAGAIN`/`EMFILE`/`ENOMEM` |
 * | `kExecFailed` | 子进程已创建但 exec 失败（由错误管道回传） | `ENOENT`/`EACCES`/`ENOEXEC` |
 * | `kWaitFailed` | `wait4` 失败或句柄不可等待 | `ECHILD`/`EINVAL` |
 * | `kTimeout` | 超时；对 `Wait` 表示进程仍存活，对通道表示暂无数据 | `ETIMEDOUT` |
 * | `kNotRunning` | 进程已退出/已回收，操作无意义 | `ESRCH` |
 * | `kPermissionDenied` | 无权限（降权、信号、OOM 调整） | `EPERM`/`EACCES` |
 * | `kUnsupported` | 平台/内核/权限不支持该能力（如非进程组调用 `KillTree`） | `ENOSYS`/`EOPNOTSUPP` |
 * | `kResourceLimit` | 资源配额耗尽（fd/rlimit/内存） | `EMFILE`/`ENFILE`/`ENOMEM` |
 * | `kIoFailed` | 通用 IO 失败 | `EIO`/`EPIPE` |
 * | `kClosed` | 通道已关闭 / 对端 EOF | — |
 * | `kAgain` | 非阻塞调用暂无数据 | `EAGAIN` |
 * | `kQueueFull` | 消息队列/流控缓冲已满 | `ENOBUFS` |
 * | `kChannelClosed` | 消息通道在操作中被对端关闭 | `EPIPE` |
 * | `kShmFailed` | 共享内存创建/映射/名称非法 | `EEXIST`/`ENOENT` |
 * | `kMqFailed` | 消息队列创建/打开失败 | `ENOENT`/`EEXIST` |
 * | `kPtyFailed` | PTY 创建/配置失败 | `ENODEV`/`EIO` |
 * | `kInternal` | 不应发生的内部不一致（断言失败、状态机违约） | — |
 */
enum class ProcErrorCode : std::uint8_t {
    kOk = 0,
    kInvalidArgument,
    kSpawnFailed,
    kExecFailed,
    kWaitFailed,
    kTimeout,
    kNotRunning,
    kPermissionDenied,
    kUnsupported,
    kResourceLimit,
    kIoFailed,
    kClosed,
    kAgain,
    kQueueFull,
    kChannelClosed,
    kShmFailed,
    kMqFailed,
    kPtyFailed,
    kInternal,
};

/** @brief 错误码转文本（英文短名，用于日志与测试断言）。 */
const char* ToString(ProcErrorCode code) noexcept;

/**
 * @brief 结构化错误：语义分类 + errno + 中文诊断 + 上下文。
 *
 * @details `context` 记录出错的操作（如 `"execve(/bin/ls)"`、`"mq_send"`），
 *          避免调用方只能看到「失败了」而不知道在哪一步。`message` 是面向人
 *          的中文说明，**不包含**动态 errno 文本（那由 `ToString()` 拼接，
 *          保证 `strerror` 不在构造期被调用，线程安全）。
 */
struct ProcError final {
    ProcErrorCode code{ProcErrorCode::kOk};  ///< 语义分类；ok() 只看它
    int system_error{0};                     ///< 原始 errno；0 表示无系统错误
    std::string message;                     ///< 中文诊断
    std::string context;                     ///< 出错的操作/对象，便于定位

    bool ok() const noexcept { return code == ProcErrorCode::kOk; }
    explicit operator bool() const noexcept { return ok(); }

    /** @brief 组合成一行诊断：`[code] message (errno=N: strerror) @context`。 */
    std::string ToString() const;
};

/** @brief 构造一个错误。 */
ProcError MakeError(ProcErrorCode code, int system_error, std::string message,
                    std::string context = {});

/** @brief 以 errno 构造错误，`message` 为中文动作描述。 */
ProcError MakeErrnoError(ProcErrorCode code, int system_error,
                         std::string context = {});

/**
 * @brief 轻量结果类型：`Result<T>` 持有值或 `ProcError`。
 *
 * @details 不抛异常：失败路径全部用返回值表达，与项目 `FileStatus` 的风格一致。
 *          `value()`/`operator*` 要求先 `ok()`；失败时是调用方违约（`assert` 后
 *          返回未定义值），文档与测试都按此约束使用。
 *          没有默认构造：成功必须带值，失败必须带错误，避免「空成功」。
 */
template <typename T>
class Result final {
public:
    Result(T value) : m_value(std::move(value)) {}          // NOLINT: 隐式构造便于 return
    Result(ProcError error) : m_error(std::move(error)) {}  // NOLINT: 隐式构造便于 return

    bool ok() const noexcept { return m_value.has_value(); }
    explicit operator bool() const noexcept { return ok(); }

    const ProcError& error() const noexcept { return m_error; }

    T& value() noexcept {
        assert(m_value.has_value());
        return *m_value;
    }
    const T& value() const noexcept {
        assert(m_value.has_value());
        return *m_value;
    }
    T* operator->() noexcept { return &value(); }
    const T* operator->() const noexcept { return &value(); }
    T& operator*() noexcept { return value(); }
    const T& operator*() const noexcept { return value(); }

    /** @brief 取出值（仅右值可调用；失败时返回默认构造值）。 */
    T Take() noexcept { return m_value ? std::move(*m_value) : T{}; }

    T value_or(T fallback) const {
        return m_value ? *m_value : std::move(fallback);
    }

private:
    std::optional<T> m_value;  ///< 有值表示成功；失败时为空
    ProcError m_error;         ///< 失败原因；成功时 code == kOk
};

/** @brief `Result<void>`：只表达成功/失败。 */
template <>
class Result<void> final {
public:
    Result() = default;
    Result(ProcError error) : m_error(std::move(error)) {}  // NOLINT: 隐式构造便于 return

    bool ok() const noexcept { return m_error.ok(); }
    explicit operator bool() const noexcept { return ok(); }
    const ProcError& error() const noexcept { return m_error; }

private:
    ProcError m_error;  ///< 成功时 code == kOk
};

/** @brief 构造一个「成功」的 void 结果。 */
inline Result<void> OkResult() noexcept { return Result<void>(); }

// ---------------------------------------------------------------------------
// 进程生命周期状态（R16）
// ---------------------------------------------------------------------------

/**
 * @brief `Process`/`Supervisor` 的显式生命周期状态。
 *
 * @details 迁移规则（完整图见 `docs/os-process.md`）：
 *
 * ```
 * kCreated ──Spawn 成功──> kRunning
 * kRunning ──Terminate/Kill 发 TERM──> kTerminating
 * kTerminating ──宽限期到/收到 KILL──> kKilled
 * kRunning/kTerminating/kKilled ──wait 成功──> kReaped
 * kCreated ──Spawn 失败──> kFailed
 * ```
 *
 * 迁移只由 `Process` 内部与 `Supervisor` 驱动；重复 `Terminate` 幂等。
 */
enum class LifecycleState : std::uint8_t {
    kCreated,      ///< 已构造但未启动（默认状态）
    kRunning,      ///< 子进程已创建、尚未回收
    kTerminating,  ///< 已发送 SIGTERM，等待宽限期
    kKilled,       ///< 已发送 SIGKILL，等待回收
    kReaped,       ///< wait 成功，退出状态可用
    kFailed,       ///< Spawn 失败，不持有子进程
};

/** @brief 生命周期状态转文本。 */
const char* ToString(LifecycleState state) noexcept;

// ---------------------------------------------------------------------------
// 进程信息与退出状态（R3/R5）
// ---------------------------------------------------------------------------

/**
 * @brief 进程运行状态快照（数据来源 `/proc/<pid>/stat`）。
 *
 * @details 字段对应 `proc(5)` 的列（1-based）：
 * - `state`：第 3 列（R/S/D/Z/T/t/X）；
 * - `ppid`：第 4 列；`pgid`：第 5 列；
 * - `cpu_user_ms`/`cpu_system_ms`：第 14/15 列（jiffies，除以 `_SC_CLK_TCK`）；
 * - `rss_bytes`：第 24 列（页数 × `sysconf(_SC_PAGESIZE)`）；
 * - `vsize_bytes`：第 23 列；
 * - `threads`：第 20 列；
 * - `start_ticks`：第 22 列（自系统启动的 jiffies）；
 * - `started_at`：由 `/proc/stat` 的 `btime` 与 `start_ticks` 换算的墙钟时间。
 *
 * 进程已回收后无法再读 `/proc`，此时返回 `Process` 缓存的最后快照
 * （`alive == false`、`state == 'X'`）。
 */
struct ProcessInfo final {
    pid_t pid{-1};
    pid_t ppid{-1};
    pid_t pgid{-1};
    char state{'?'};                      ///< `/proc/<pid>/stat` 的状态字符
    std::uint64_t rss_bytes{0};           ///< 常驻集（RSS）
    std::uint64_t vsize_bytes{0};         ///< 虚拟地址空间
    std::uint64_t cpu_user_ms{0};         ///< 用户态累计 CPU
    std::uint64_t cpu_system_ms{0};       ///< 内核态累计 CPU
    std::uint64_t start_ticks{0};         ///< 自开机起的启动 jiffies
    std::chrono::system_clock::time_point started_at{};      ///< 墙钟启动时间
    std::chrono::steady_clock::time_point started_monotonic{};  ///< 单调时钟启动时间
    int threads{0};                       ///< 线程数
    bool alive{false};                    ///< 快照时进程是否仍存在（非僵尸）
    bool zombie{false};                   ///< 是否僵尸（已退出但未 wait）

    /** @brief 累计 CPU 毫秒。 */
    std::uint64_t cpu_total_ms() const noexcept { return cpu_user_ms + cpu_system_ms; }
};

/**
 * @brief 子进程的退出状态（`wait4` 的结果 + 超时标记）。
 *
 * @details `reaped == false` 表示尚未回收（通常只在超时返回值里出现）。
 *          被信号杀死时 `signaled == true`，`signal` 是信号编号（用
 *          `SignalName` 转文本）；正常退出时 `exited == true`。
 */
struct ExitStatus final {
    bool reaped{false};      ///< 已 wait 成功（状态有效）
    bool exited{false};      ///< WIFEXITED
    bool signaled{false};    ///< WIFSIGNALED
    bool core_dumped{false}; ///< WCOREDUMP
    bool stopped{false};     ///< WIFSTOPPED（本模块默认不跟踪停止状态）
    bool timed_out{false};   ///< 由 Run/Supervisor 标记：超时后被阶梯终止
    int exit_code{0};        ///< WEXITSTATUS（仅 exited 有效）
    int signal{0};           ///< WTERMSIG（仅 signaled 有效）
    std::uint64_t wall_ms{0};///< 从 spawn 到回收的墙钟毫秒

    /** @brief 是否「正常退出且退出码为 0」。 */
    bool ok() const noexcept { return reaped && exited && exit_code == 0 && !timed_out; }

    /** @brief 人类可读：`exited(0)` / `signaled(SIGKILL)` / `running`。 */
    std::string ToString() const;
};

/** @brief 信号编号转文本（未知返回 `SIG<number>` 的静态缓冲）。 */
const char* SignalName(int signal) noexcept;

// ---------------------------------------------------------------------------
// 资源用量与配额（R25）
// ---------------------------------------------------------------------------

/** @brief 单个受控进程的资源用量快照。 */
struct ResourceUsage final {
    std::size_t open_fds{0};             ///< `/proc/<pid>/fd` 条目数
    std::uint64_t peak_rss_bytes{0};     ///< 观测到的峰值 RSS（由 Process 累计）
    std::uint64_t cpu_user_ms{0};
    std::uint64_t cpu_system_ms{0};
    std::uint64_t stdout_bytes{0};       ///< Run/Supervisor 已收集的 stdout 字节
    std::uint64_t stderr_bytes{0};       ///< Run/Supervisor 已收集的 stderr 字节
    std::uint64_t dropped_output_bytes{0};  ///< 超过 max_output_bytes 被丢弃的字节
};

/** @brief 配额超限时的动作。 */
enum class QuotaAction : std::uint8_t {
    kIgnore,  ///< 只记录，不处理
    kWarn,    ///< 记录事件（`kQuotaExceeded`），不销毁
    kKill,    ///< 触发阶梯终止
};

/**
 * @brief 每进程资源配额（0 表示该项不限）。
 *
 * @details 由 `Supervisor` 在每次条件求值 tick 时检查；`QuotaAction::kKill`
 *          复用 `Terminate(grace)` 的 TERM→KILL 阶梯。输出字节由 Supervisor
 *          的收集器统计（`Run` 的 `max_output_bytes` 是单次执行的截断上限，
 *          与本配额独立）。
 */
struct QuotaLimits final {
    std::uint64_t max_rss_bytes{0};
    std::size_t max_open_fds{0};
    std::size_t max_output_bytes{0};
    std::uint64_t max_cpu_ms{0};
    QuotaAction action{QuotaAction::kIgnore};
};

// ---------------------------------------------------------------------------
// 启动参数（R1/R2/R24）
// ---------------------------------------------------------------------------

/** @brief stdio 的处理方式。 */
enum class StdioMode : std::uint8_t {
    kPipe,     ///< 建立管道，父进程通过 `MessageChannel` 读写
    kInherit,  ///< 直接继承父进程对应的 fd
    kNull,     ///< 重定向到 `/dev/null`
    kFile,     ///< 重定向到 `stdin_path`/`stdout_path`/`stderr_path`
    kPty,      ///< 合并到 PTY 从端（仅对三流同时生效，见 `use_pty`）
};

/** @brief 标准流标识（用于 `Process::native_fd`）。 */
enum class StdioStream : std::uint8_t { kStdin, kStdout, kStderr };

/** @brief 一条 `setrlimit` 设置。 */
struct RLimitSetting final {
    int resource{0};           ///< `RLIMIT_*` 常量
    std::uint64_t soft{0};     ///< 软上限；`RLIM_INFINITY` 用 `~std::uint64_t{0}`
    std::uint64_t hard{0};     ///< 硬上限
};

/**
 * @brief 子进程启动参数。
 *
 * @details 设计原则：
 * - **argv 数组直接 exec，永不经过 shell**（R24）：`argv[0]` 是程序路径
 *   （默认 `execve`，不做 PATH 搜索；`use_path_search = true` 时父进程按
 *   `PATH` 解析成绝对路径再 exec，仍不经过 shell）。
 * - **env 的两种模式**：`env_clear == false`（默认）时在父环境基础上用 `env`
 *   覆盖；`env_clear == true` 时**只**传入 `env` 列出的键值（白名单）。
 *   `env` 项必须形如 `KEY=VALUE`；只有 `KEY` 时从父环境取同名值。
 * - **安全默认**：`close_extra_fds = true` 关闭子进程除 0/1/2 外的所有 fd；
 *   管道全部 `O_CLOEXEC`；不创建会话时 `KillTree` 会拒绝执行，避免误杀调用方
 *   所在的进程组。
 * - **失败语义**：任何一项设置在内核不支持/无权限时，`Spawn` 失败并回传
 *   errno（通过 CLOEXEC 错误管道），**不会静默忽略**。若希望「尽力而为」，
 *   调用方可以自行清理对应字段。
 */
struct ProcessOptions final {
    /// argv；`argv[0]` 必填。空列表或空字符串返回 `kInvalidArgument`。
    std::vector<std::string> argv;

    /// 环境变量列表（`KEY=VALUE` 或 `KEY`）。语义见上文。
    std::vector<std::string> env;
    /// true：子进程环境 = 仅 `env`（白名单）；false：父环境 + `env` 覆盖。
    bool env_clear{false};

    /// 工作目录；空表示继承父进程 cwd。
    std::string cwd;

    /// nice 值（-20..19）；0 表示不调整（不是「设为 0」）。
    int nice{0};
    /// CPU 亲和性；空表示不调整。
    std::vector<int> cpu_affinity;
    /// 调度策略：-1 = 不调整；否则 `SCHED_OTHER`/`SCHED_BATCH`。
    int sched_policy{-1};
    /// 调度优先级（仅 SCHED_OTHER/BATCH 允许 0）。
    int sched_priority{0};

    /// 单次执行超时（0 = 不限时；供 `Run`/`Supervisor`/`ProcessPool` 使用）。
    std::chrono::milliseconds timeout{0};
    /// TERM → KILL 宽限期。
    std::chrono::milliseconds grace{5000};

    /// 是否 `setsid()` 创建新会话（`KillTree` 的前提）。
    bool new_session{false};

    /// 是否设置 umask（`umask == 0` 也需要显式打开，避免歧义）。
    bool set_umask{false};
    mode_t umask_value{022};

    /// OOM score 调整（-1000..1000）；`INT32_MIN` 表示不调整。
    int oom_score_adj{-2147483647 - 1};

    /// 降权目标（需要 CAP_SETUID/CAP_SETGID）。
    std::optional<uid_t> uid;
    std::optional<gid_t> gid;
    /// 附加组；`set_groups == true` 时调用 `setgroups`。
    std::vector<gid_t> supplementary_groups;
    bool set_groups{false};

    /// rlimit 设置列表（按顺序应用）。
    std::vector<RLimitSetting> rlimits;

    /// 子进程是否关闭 3 以上的全部 fd（默认 true）。
    bool close_extra_fds{true};

    /// 是否按 PATH 解析 `argv[0]`（父进程解析，仍不经过 shell）。
    bool use_path_search{false};

    /// 三条标准流模式（`use_pty` 为 true 时被忽略）。
    StdioMode stdin_mode{StdioMode::kPipe};
    StdioMode stdout_mode{StdioMode::kPipe};
    StdioMode stderr_mode{StdioMode::kPipe};
    /// kFile 模式使用的路径。
    std::string stdin_path;
    std::string stdout_path;
    std::string stderr_path;
    bool stdout_append{false};
    bool stderr_append{false};
    /// 把 stderr 合并到 stdout 管道（两者都必须是 kPipe/kInherit 等）。
    bool stderr_to_stdout{false};

    /// 使用 PTY（子进程 0/1/2 都接从端，父进程用 master 双向通信）。
    bool use_pty{false};
    int pty_columns{80};
    int pty_rows{24};

    /// 父进程侧保留的输出上限（0 = 不限）；超出部分被丢弃并计数。
    std::size_t max_output_bytes{0};
    /// 是否把 stderr 按行转发到 `StderrLineSink`（R18）。
    bool forward_stderr{false};
};

// ---------------------------------------------------------------------------
// 可注入后端与时钟（R21）
// ---------------------------------------------------------------------------

/** @brief `Spawn` 成功后后端交回的句柄。 */
struct SpawnedProcess final {
    pid_t pid{-1};
    pid_t ppid{-1};
    pid_t pgid{-1};
    int stdin_fd{-1};   ///< 父进程侧写端；-1 表示该流不是管道
    int stdout_fd{-1};  ///< 父进程侧读端
    int stderr_fd{-1};
    bool pty{false};    ///< stdin/stdout 共用 PTY master
    std::chrono::steady_clock::time_point started_monotonic{};
    std::chrono::system_clock::time_point started_wall{};
};

/**
 * @brief 进程后端接口：真实实现走 `fork/execve/wait4`，测试可注入假实现。
 *
 * @details 假后端用于验证「fork 失败归因」「超时状态机」等难以在真实内核上
 *          稳定复现的路径（R21）。`Process` 只通过本接口操作子进程，因此注入
 *          后端时不会碰到 `/proc` 或真实信号。
 */
class ProcessBackend {
public:
    virtual ~ProcessBackend() = default;

    /// 启动子进程；失败必须返回 `ProcError`（`kSpawnFailed`/`kExecFailed`）。
    virtual Result<SpawnedProcess> Spawn(const ProcessOptions& options) = 0;

    /// 等待退出；超时返回 `kTimeout`（进程仍存活）。
    virtual Result<ExitStatus> Wait(pid_t pid, std::chrono::milliseconds timeout) = 0;

    /// 发送信号。
    virtual Result<void> Kill(pid_t pid, int signal) = 0;

    /// 查询状态。
    virtual Result<ProcessInfo> Query(pid_t pid) = 0;

    /// 是否仍存活（非僵尸）。
    virtual bool Alive(pid_t pid) = 0;
};

/** @brief 默认（真实 POSIX）后端。 */
std::shared_ptr<ProcessBackend> DefaultProcessBackend();

/**
 * @brief 可注入时钟；默认实现是 `steady_clock` + `ppoll` 睡眠。
 *
 * @details `Supervisor` 的所有 deadline/条件 tick 都从这里取时间，测试用假
 *          时钟可以在毫秒内跨越数秒的宽限期而不真实等待。
 */
class Clock {
public:
    virtual ~Clock() = default;
    virtual std::chrono::steady_clock::time_point Now() const = 0;
    virtual void SleepFor(std::chrono::milliseconds duration) = 0;
};

/** @brief 真实时钟。 */
std::shared_ptr<Clock> MakeRealClock();

// ---------------------------------------------------------------------------
// Process（R1-R5, R17, R19）
// ---------------------------------------------------------------------------

/**
 * @brief 子进程 RAII 句柄（可移动，不可复制）。
 *
 * @details ## 所有权与回收规则
 * - `Spawn` 成功后由本对象持有子进程；**谁持有 `Process`，谁负责 `Wait`**。
 * - 析构兜底：若仍存活，发送 `SIGKILL` 并 `Wait`，保证不留僵尸；这是最后
 *   防线，不是正常路径（正常路径应显式 `Wait`/`Run`，以便拿到退出码）。
 * - `CloseChannels()` 只关闭三条流，不影响子进程；子进程会收到 `EPIPE`。
 *
 * ## 线程模型
 * - **非线程安全**：同一 `Process` 不应被多线程并发操作；
 * - `Wait` 可以在一个线程阻塞，`Kill` 从另一个线程发信号是安全的（POSIX
 *   语义），但本对象的状态记录不保证并发一致（文档不推荐）。
 *
 * ## 与 Fiber 的边界
 * `Wait`/`Run`/通道读写都是**阻塞调用**（裸 syscall），在 Fiber 里会阻塞 M。
 * 需要协程友好的等待请把进程放到独立线程，或使用 `native_fd()` 自行接入
 * `IOManager`（见 `docs/os-process.md` R20）。
 */
class Process final {
public:
    Process() noexcept;
    ~Process() noexcept;

    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;
    Process(Process&& other) noexcept;
    Process& operator=(Process&& other) noexcept;

    /**
     * @brief 启动子进程（输出参数形式；避免 `Result<Process>` 的不完整类型问题）。
     * @param options 启动参数。
     * @param out 成功时写入新进程；失败时保持无效状态。
     * @param backend 可注入后端；空表示 `DefaultProcessBackend()`。
     * @return 成功或 `ProcError`（`kSpawnFailed`/`kExecFailed`/`kInvalidArgument`）。
     *
     * @note 失败时 `*out` 保持 `valid() == false`，可直接复用。
     */
    static Result<void> Spawn(const ProcessOptions& options, Process* out,
                              std::shared_ptr<ProcessBackend> backend = nullptr);

    // ---- 基本信息 ----
    pid_t pid() const noexcept;
    pid_t ppid() const noexcept;
    pid_t pgid() const noexcept;
    bool valid() const noexcept;
    bool reaped() const noexcept;
    LifecycleState state() const noexcept;
    const std::string& exec_path() const noexcept;
    const ProcError& last_error() const noexcept;

    /**
     * @brief 查询 `/proc/<pid>/stat` 快照。
     * @details 已回收时返回缓存的最后快照（`alive == false`）；从未启动返回
     *          `kNotRunning`。
     */
    Result<ProcessInfo> Info() const;

    /**
     * @brief 阻塞/带超时等待子进程退出并回收。
     * @param timeout 统一超时语义；`kWaitForever` 永久等待。
     * @return 成功时 `ExitStatus`；超时返回 `kTimeout`（进程仍存活）；
     *         `wait4` 失败返回 `kWaitFailed`。
     *
     * @warning 如果子进程正在大量写 stdout/stderr 且父进程没有读取，管道可能
     *          被写满，子进程无法退出。`Wait` **不会**替调用方排空输出；需要
     *          自动排空请用 `Run`，或先调用 `CloseChannels()`（子进程会收到
     *          `EPIPE`）。
     */
    Result<ExitStatus> Wait(std::chrono::milliseconds timeout = kWaitForever);

    /**
     * @brief 非阻塞回收。
     * @param out 已退出时写入状态。
     * @param error 可选错误输出；`ECHILD` 等写入此处。
     * @return true 表示本次拿到了退出状态；false 表示尚未退出或出错
     *         （`error->ok()` 为 false 时是出错）。
     */
    bool TryWait(ExitStatus* out, ProcError* error = nullptr);

    // ---- 信号控制 ----
    /// 发送任意信号；进程已退出返回 `kNotRunning`。
    Result<void> Kill(int signal);
    /// 阶梯终止：SIGTERM → 等待 `grace` → SIGKILL → 回收。幂等。
    Result<void> Terminate(std::chrono::milliseconds grace = std::chrono::milliseconds(5000));
    /**
     * @brief 向整个进程组发信号。
     * @details **仅当 `pgid == pid`**（即 spawn 时 `new_session = true`）时执行；
     *          否则返回 `kUnsupported`，避免误杀调用方所在进程组。
     */
    Result<void> KillTree(int signal);
    /// 是否仍存活（僵尸视为 false）。
    bool Alive() const noexcept;

    // ---- 运行中调整（R2）----
    Result<void> SetNice(int nice);
    Result<void> SetAffinity(const std::vector<int>& cpus);
    Result<void> SetScheduling(int policy, int priority);
    Result<void> SetRlimit(const RLimitSetting& limit);
    Result<void> SetOomScore(int score);

    // ---- 标准流 ----
    /**
     * @brief 父进程侧通道。
     * @details 当对应流不是 `kPipe` 时返回一个**已关闭**的通道，读写会返回
     *          `kClosed`（接口保持引用形式，调用方无需判空）。
     */
    MessageChannel& stdin_channel() noexcept;
    MessageChannel& stdout_channel() noexcept;
    MessageChannel& stderr_channel() noexcept;
    MessageChannel& channel(StdioStream stream) noexcept;

    /// 父进程侧 fd；-1 表示该流不是管道/已被关闭。
    int native_fd(StdioStream stream) const noexcept;

    /// 关闭三条流（幂等）。
    void CloseChannels() noexcept;

    /** @brief 资源用量（实时采样；`peak_rss_bytes` 为历史峰值）。 */
    ResourceUsage Usage() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;  ///< PIMPL：隐藏 sys 类型与通道实现
};

/**
 * @brief 启动子进程的便捷形式（`Process` 完整定义之后）。
 * @return 成功返回可移动的 `Process`；失败返回错误。
 */
Result<Process> Spawn(const ProcessOptions& options,
                      std::shared_ptr<ProcessBackend> backend = nullptr);

// ---------------------------------------------------------------------------
// 一次性执行：收集输出 + 超时阶梯（R6/R13）
// ---------------------------------------------------------------------------

/** @brief `Run` 的结果。 */
struct RunResult final {
    ExitStatus status;             ///< 退出状态（含 timed_out）
    std::string standard_output;   ///< 收集到的 stdout
    std::string standard_error;    ///< 收集到的 stderr
    bool output_truncated{false};  ///< 是否因 max_output_bytes 截断
    std::uint64_t dropped_output_bytes{0};  ///< 被丢弃的字节数
    std::uint64_t stdout_bytes{0}; ///< 实际收到的 stdout 字节（含被丢弃部分）
    std::uint64_t stderr_bytes{0}; ///< 实际收到的 stderr 字节
};

/**
 * @brief 启动子进程、写入标准输入、收集两条输出流、超时阶梯终止。
 *
 * @details 这个函数解决两个经典死锁：
 * 1. 「父进程写满 stdin → 子进程不读；父进程等子进程输出」——stdin 写端
 *    与两条读端一起进入 `poll` 状态机，交替推进；
 * 2. 「子进程写满 stdout → 父进程却在 `waitpid`」——每轮先排空两条管道，
 *    再用 `WNOHANG` 探测退出，退出后继续读到 EOF。
 *
 * 超时执行阶梯：`SIGTERM` → 等待 `options.grace` → `SIGKILL`，回收后把
 * `status.timed_out` 置 true，**已收集的输出照常返回**。
 *
 * @param options 启动参数（`stdin_mode` 必须为 `kPipe` 才能写入 `input`）。
 * @param input 写入子进程 stdin 的数据；写完立即关闭写端（EOF）。
 * @param on_spawn 子进程创建成功后的回调（可用于登记 pid 以便外部强杀）；
 *                 回调异常会被隔离。
 */
Result<RunResult> Run(const ProcessOptions& options, std::string_view input = {},
                      std::function<void(pid_t)> on_spawn = {});

// ---------------------------------------------------------------------------
// 观测：事件、stderr 行、统计（R18/R19）
// ---------------------------------------------------------------------------

/** @brief 结构化事件类型。 */
enum class ProcEventType : std::uint8_t {
    kSpawn,         ///< 启动成功
    kSpawnFailed,   ///< 启动失败
    kExit,          ///< 正常退出/被信号终止
    kTimeout,       ///< 超时触发
    kKill,          ///< 主动发送信号
    kCondition,     ///< 条件触发销毁
    kQuotaExceeded, ///< 资源配额超限
    kReap,          ///< wait 成功回收
    kIoError,       ///< 通道/管道 IO 错误
};

/** @brief 事件转文本。 */
const char* ToString(ProcEventType type) noexcept;

/** @brief 一条结构化事件。 */
struct ProcEvent final {
    ProcEventType type{ProcEventType::kSpawn};
    pid_t pid{-1};
    pid_t pgid{-1};
    int exit_code{0};
    int signal{0};
    std::string reason;  ///< 触发原因（超时/条件/配额）
    std::chrono::system_clock::time_point when{};
};

/** @brief 事件接收器；在调用线程同步执行，异常会被吞掉（隔离扩展点）。 */
using ProcEventSink = std::function<void(const ProcEvent&)>;

/** @brief 子进程 stderr 的一行（或尾部不完整的片段）。 */
struct StderrLine final {
    pid_t pid{-1};
    pid_t pgid{-1};
    std::string text;      ///< 行内容（不含 `\n`）
    bool partial{false};   ///< true：EOF 前的最后一段，没有换行
};

/** @brief stderr 行接收器。 */
using StderrLineSink = std::function<void(const StderrLine&)>;

/** @brief 进程子层的累计统计快照。 */
struct ProcStatsSnapshot final {
    std::uint64_t spawned{0};       ///< Spawn 成功次数
    std::uint64_t spawn_failed{0};  ///< Spawn 失败次数
    std::uint64_t exited{0};        ///< 正常退出次数
    std::uint64_t signaled{0};      ///< 被信号终止次数
    std::uint64_t timed_out{0};     ///< 超时次数
    std::uint64_t killed{0};        ///< 主动 Kill/Terminate 次数
    std::uint64_t conditions{0};    ///< 条件触发次数
    std::uint64_t reaped{0};        ///< wait 成功次数
    std::uint64_t live{0};          ///< 当前未回收的进程数
    std::size_t open_fds{0};        ///< 当前进程打开的 fd 数（`/proc/self/fd`）
};

/** @brief 读取统计快照（线程安全，原子读）。 */
ProcStatsSnapshot StatsSnapshot();
/** @brief 清零累计统计（`live` 重新计算）。 */
void ResetStats();
/** @brief 设置事件接收器（传空清除）。 */
void SetEventSink(ProcEventSink sink);
/** @brief 设置 stderr 行接收器（传空清除）。 */
void SetStderrLineSink(StderrLineSink sink);
/**
 * @brief 打开/关闭默认日志：把事件与 stderr 行写到 stderr。
 * @details 默认关闭；`Supervisor` 的 `forward_stderr` 不依赖它（有独立开关）。
 */
void EnableDefaultLogging(bool enable);
/**
 * @brief 统计当前进程打开的 fd 数。
 * @details 读 `/proc/self/fd` 目录；失败返回 0。测试用它在操作前后断言无泄漏。
 */
std::size_t CountOpenFds();
/**
 * @brief 忽略 `SIGPIPE`（写已关闭的管道时返回 `EPIPE` 而不是杀死进程）。
 *
 * @details **本模块不会擅自修改全局信号处理**：`Run`/`FdChannel` 内部也不改。
 *          任何会写管道/消息队列的程序都应在启动时显式调用一次；否则对端
 *          关闭时 `write` 会触发 `SIGPIPE`，进程按默认动作直接终止。
 *          测试与示例在 `main` 开头调用它。
 */
void IgnoreSigPipe();
/** @brief 清理全局状态（事件/行接收器、统计、通道注册表缓存）。 */
void Cleanup();

// ---------------------------------------------------------------------------
// 配置默认值（R2/R13/R25）
// ---------------------------------------------------------------------------

/**
 * @brief 从 `go2cpp::config` 的 `os.proc.*` 读取的默认值。
 *
 * @details 键与默认值见 `src/os/config.cpp`。构造函数不会自动读取配置；
 *          需要配置驱动的调用方显式调用 `LoadProcessDefaults()`，避免「静态
 *          注册顺序 / 未链接 config」导致的隐藏耦合。
 */
struct ProcessDefaults final {
    std::chrono::milliseconds grace{5000};
    std::chrono::milliseconds timeout{0};
    std::size_t max_output_bytes{1 << 20};
    std::chrono::milliseconds supervisor_tick{50};
    std::size_t pool_workers{4};
    std::size_t pool_queue{64};
    bool forward_stderr{false};
    std::size_t max_open_fds{0};
    std::uint64_t max_rss_bytes{0};
    QuotaAction quota_action{QuotaAction::kIgnore};
};

/** @brief 读取 `os.proc.*`；缺失时回退到结构体默认值。 */
ProcessDefaults LoadProcessDefaults();

}  // namespace go2cpp::os
