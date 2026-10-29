#pragma once

/**
 * @file command.hpp
 * @brief MOD-26：通用命令协议与执行器（帧编解码、白名单、输出回调、
 *        超时杀进程树）。
 * @details 本模块把「执行一条命令」拆成可独立测试的两半：
 *
 *          - **协议层**：`Command`（请求）与 `CommandResult`（结果）的
 *            二进制帧编解码，全部整数大端（与 `protocol.hpp` 一致），
 *            解码对固定头、每段长度、总长做精确校验：请求帧总长超过
 *            `kCommandMaxFrameBytes`、结果帧总长超过
 *            `kCommandResultMaxFrameBytes`、或结果单路输出超过
 *            `kCommandMaxOutputCeiling` 一律拒绝，不会按对端声明的大小分配内存；
 *          - **执行层**：`CommandExecutor` 基于 `go2cpp::os::proc` 的
 *            `Process` 启动子进程，边读 stdout/stderr 边等退出，超时后
 *            `SIGTERM` → 宽限 → `SIGKILL` **杀整个进程组**（spawn 时
 *            `new_session = true`，因此子进程即组长）。
 *
 * ## 安全模型（默认拒绝）
 *
 * | 防线 | 默认值 | 说明 |
 * |---|---|---|
 * | 命令白名单 | 空 = 全拒绝 | `allow_all=false` 时只执行 `allowed_commands` 中的命令 |
 * | 内置危险命令 | 生效 | `sh`/`rm`/`dd`/`sudo`/`kill`… 即使 `allow_all` 也拒绝，除非 `allow_dangerous` |
 * | 自定义拒绝表 | 空 | `denied_commands` 优先级最高，任何开关都不能绕过 |
 * | cwd 沙箱 | 不限制 | `allowed_cwd_roots` 非空时 cwd 必须落在某个根下且为绝对路径；cwd 为空直接拒绝（否则子进程继承服务进程 cwd，沙箱形同虚设） |
 * | 参数长度 | 64 项 × 4KB | 超限在**帧解码之前**就被拒绝（`kInvalidArgument`） |
 * | 超时 | 30s（上限 10min） | 到点先 `SIGTERM` 整组，`kill_grace_ms` 后 `SIGKILL`；`timeout_ms=0`（不限时）只有策略显式打开 `allow_unlimited_timeout` 才允许 |
 * | 输出上限 | 每流 1MB（可配，硬上限 4MB） | 超出部分丢弃并置 `output_truncated`，不会阻塞子进程 |
 *
 * **不经过 shell**：argv 原样交给 `execve`，因此参数里的 `;`、`|`、`$()`
 * 不会被解释，不存在命令注入面。真正的风险是「执行了没被允许的程序」，
 * 由上面的白名单兜住。
 *
 * ## 帧格式（大端）
 *
 * 请求帧（type=1）：
 *
 * | 偏移 | 长度 | 字段 |
 * |---:|---:|---|
 * | 0 | 2 | magic `'B' 'C'` |
 * | 2 | 1 | version = 1 |
 * | 3 | 1 | type = 1 |
 * | 4 | 2 | id_len（<= 64） |
 * | 6 | 2 | argc（<= 64） |
 * | 8 | 4 | timeout_ms |
 * | 12 | 2 | cwd_len（<= 1024） |
 * | 14 | 2 | reserved = 0 |
 * | 16 | 4×argc | 每个 argv 的长度 u32 |
 * | … | 变长 | id、cwd、argv[0..argc-1] |
 *
 * 结果帧（type=2）：
 *
 * | 偏移 | 长度 | 字段 |
 * |---:|---:|---|
 * | 0 | 2 | magic `'B' 'C'` |
 * | 2 | 1 | version = 1 |
 * | 3 | 1 | type = 2 |
 * | 4 | 1 | status（`CommandStatus`） |
 * | 5 | 1 | flags：bit0 timed_out、bit1 output_truncated |
 * | 6 | 2 | id_len |
 * | 8 | 4 | exit_code（i32，被信号终止时为 -1） |
 * | 12 | 4 | stdout_len |
 * | 16 | 4 | stderr_len |
 * | 20 | 2 | message_len（<= 1024） |
 * | 22 | 2 | reserved = 0 |
 * | 24 | 8 | duration_ms |
 * | 32 | 变长 | id、stdout、stderr、message |
 *
 * ## 与其他模块的边界
 *
 * - 依赖 `go2cpp::os::proc`（进程/管道）；不依赖 Bento 链路——命令帧既可
 *   以走可靠流，也可以走任意自带完整性保护的信道。
 * - 执行器是**同步阻塞**的：在 Fiber 里调用会阻塞当前 M（与 `os::proc`
 *   的边界一致）。需要异步调用方应把执行放到独立线程。
 *
 * @author go2cpp contributors
 * @date 2026-10-28
 * @copyright MIT License
 */

#include <sys/types.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace go2cpp::bento {

// ---------------------------------------------------------------------------
// 协议常量与上限
// ---------------------------------------------------------------------------

/// 命令帧版本。
inline constexpr std::uint8_t kCommandProtocolVersion = 1;
/// 请求帧 magic 首字节。
inline constexpr std::uint8_t kCommandFrameMagic0 = 'B';
/// 请求帧 magic 次字节。
inline constexpr std::uint8_t kCommandFrameMagic1 = 'C';
/// 请求帧类型。
inline constexpr std::uint8_t kCommandFrameTypeRequest = 1;
/// 结果帧类型。
inline constexpr std::uint8_t kCommandFrameTypeResult = 2;
/// 请求帧固定头长度。
inline constexpr std::size_t kCommandFrameHeaderBytes = 16;
/// 结果帧固定头长度。
inline constexpr std::size_t kCommandResultFrameHeaderBytes = 32;
/// 请求帧硬上限（64KB；正常请求远小于它）。
inline constexpr std::size_t kCommandMaxFrameBytes = 64 * 1024;
/// 结果帧硬上限（16MB：两路输出各 4MB + 头与诊断信息）。
inline constexpr std::size_t kCommandResultMaxFrameBytes = 16 * 1024 * 1024;
/// 请求 id 最大字节数。
inline constexpr std::size_t kCommandMaxIdBytes = 64;
/// argv 最大项数（含 argv[0]）。
inline constexpr std::size_t kCommandMaxArgv = 64;
/// 单个 argv 项最大字节数。
inline constexpr std::size_t kCommandMaxArgBytes = 4096;
/// cwd 最大字节数。
inline constexpr std::size_t kCommandMaxCwdBytes = 1024;
/// 结果 message 最大字节数。
inline constexpr std::size_t kCommandMaxMessageBytes = 1024;
/// 默认超时（毫秒）。
inline constexpr std::uint32_t kCommandDefaultTimeoutMs = 30 * 1000;
/// 超时硬上限（毫秒，10 分钟）。
inline constexpr std::uint32_t kCommandMaxTimeoutMs = 600 * 1000;
/// 默认每路输出上限（1MB）。
inline constexpr std::size_t kCommandDefaultMaxOutputBytes = 1u << 20;
/// 每路输出上限的硬上限（4MB）。
inline constexpr std::size_t kCommandMaxOutputCeiling = 4u << 20;
/// 默认 TERM → KILL 宽限（毫秒）。
inline constexpr std::uint32_t kCommandDefaultKillGraceMs = 300;
/// 轮询间隔上限（毫秒）：执行器等子进程输出/退出时的最大睡眠。
inline constexpr std::uint32_t kCommandPollIntervalMs = 5;

// ---------------------------------------------------------------------------
// 状态与数据结构
// ---------------------------------------------------------------------------

/**
 * @brief 一次命令执行的结果分类。
 *
 * @details 与退出码正交：`kNonZeroExit` 表示进程正常结束但 `exit_code != 0`；
 *          被信号杀死是 `kAborted`；超时被杀是 `kTimeout`（即使信号是 KILL）。
 */
enum class CommandStatus : std::uint8_t {
    kOk = 0,              ///< 正常退出且退出码为 0。
    kInvalidArgument = 1, ///< 请求非法（空 argv、超长、超时/输出上限越界）。
    kDenied = 2,          ///< 被白名单/拒绝表拦下，未启动进程。
    kSpawnFailed = 3,     ///< fork/exec/管道失败，进程未创建。
    kTimeout = 4,         ///< 超时，已按阶梯杀死进程组。
    kAborted = 5,         ///< 被信号终止（非本模块超时触发）。
    kNonZeroExit = 6,     ///< 正常退出但退出码非 0。
    kInternal = 7,        ///< 内部不一致（wait 未返回状态等）。
};

/// 状态转文本（诊断/日志用）。
const char* CommandStatusToString(CommandStatus status) noexcept;

/** @brief 一条命令请求（协议层的值对象）。 */
struct Command final {
    /// 请求 id（回显到结果，最长 `kCommandMaxIdBytes`）。
    std::string id;
    /// argv；`argv[0]` 是命令名或绝对路径，**不经过 shell**。
    std::vector<std::string> argv;
    /// 超时毫秒；0 = 不限时，但必须由策略显式打开
    /// `allow_unlimited_timeout`（否则拒绝），超过策略的 `max_timeout_ms`
    /// 也会被拒绝。
    std::uint32_t timeout_ms{kCommandDefaultTimeoutMs};
    /// 工作目录；空 = 继承执行器进程的 cwd，非空时必须是绝对路径。
    std::string cwd;
};

/** @brief 一次命令执行的结果（同时也是结果帧的值对象）。 */
struct CommandResult final {
    /// 回显请求 id。
    std::string id;
    /// 结果分类。
    CommandStatus status{CommandStatus::kInvalidArgument};
    /// 退出码；正常退出时有意义，被信号终止时为 -1。
    int exit_code{-1};
    /// 是否因超时被终止。
    bool timed_out{false};
    /// 输出是否因上限被截断。
    bool output_truncated{false};
    /// 从 spawn 到回收的墙钟毫秒。
    std::uint64_t duration_ms{0};
    /// 捕获的 stdout（可能被截断）。
    std::string standard_output;
    /// 捕获的 stderr（可能被截断）。
    std::string standard_error;
    /// 人类可读诊断（拒绝原因、spawn 错误、信号编号等）。
    std::string message;
};

/**
 * @brief 执行策略：白名单、拒绝表、cwd 沙箱与各类上限。
 *
 * @details 默认构造是**全拒绝**：`allow_all == false` 且 `allowed_commands`
 *          为空，任何命令都会得到 `kDenied`。这与「默认拒绝危险命令」的
 *          验收要求一致，也避免忘了配置就变成远程 shell。
 */
struct CommandPolicy final {
    /// 是否放行所有命令（仍然受内置危险命令表与 `denied_commands` 约束）。
    bool allow_all{false};
    /// 是否允许执行内置危险命令表里的程序（默认 false）。
    bool allow_dangerous{false};
    /// 是否启用内置危险命令表（默认 true；关掉后只剩下面的两张表）。
    bool use_builtin_deny{true};
    /// 允许执行的命令名或路径（比较 argv[0] 原文与其 basename）。
    std::vector<std::string> allowed_commands;
    /// 额外拒绝的命令名或路径；优先级最高，任何开关都不能绕过。
    std::vector<std::string> denied_commands;
    /// 允许的 cwd 根目录（绝对路径）；空表示不限制。非空时：
    /// 请求 cwd 必须非空、绝对、规范化后落在某个根下（`..` 与符号链接
    /// 都会被解析），空 cwd 直接拒绝。
    std::vector<std::string> allowed_cwd_roots;
    /// 每路输出（stdout、stderr 各自）保留的最大字节数。
    std::size_t max_output_bytes{kCommandDefaultMaxOutputBytes};
    /// 允许的最大超时（毫秒）。
    std::uint32_t max_timeout_ms{kCommandMaxTimeoutMs};
    /// 是否允许 `timeout_ms == 0`（不限时）。默认 false：否则远端只要把
    /// 超时填 0 就能绕过 `max_timeout_ms`，把执行线程永久占住。
    bool allow_unlimited_timeout{false};
    /// TERM → KILL 的宽限毫秒数。
    std::uint32_t kill_grace_ms{kCommandDefaultKillGraceMs};
    /// 是否按 PATH 解析 argv[0]（false = 只接受可执行文件路径）。
    /// @note 高安全场景建议置 false 并使用绝对路径：服务进程的 PATH 若包含
    ///       可写目录，攻击者可通过植入同名程序绕过 basename 白名单。
    bool use_path_search{true};
};

/// 内置危险命令表（basename，全小写）；文档与测试通过它确认默认拒绝面。
const std::vector<std::string>& BuiltinDangerousCommands();

/**
 * @brief 校验一条命令能否在当前策略下执行。
 * @param command 待校验请求。
 * @param policy 执行策略。
 * @param reason 可选输出：被拒绝/非法时写入中文原因。
 * @return `kOk` 表示可以执行；否则是 `kInvalidArgument` 或 `kDenied`。
 */
CommandStatus ValidateCommand(const Command& command, const CommandPolicy& policy,
                              std::string* reason = nullptr);

/**
 * @brief 执行期回调（全部可选，在 `Execute` 的调用线程同步调用）。
 *
 * @details 回调里抛出的异常会被隔离（吞掉），不会影响子进程回收；回调里
 *          不要递归调用同一个执行器的 `Execute`（同步阻塞会叠加超时计时）。
 */
struct CommandCallbacks final {
    /// 子进程创建成功后立即回调一次（用于登记 pid/观测）。
    std::function<void(pid_t)> on_spawn;
    /// stdout 每读到一段就回调（数据可能在输出上限之外，仍是线上原始内容）。
    /// @warning `string_view` 只在下一次读取/回调返回前有效；需要保留请自行拷贝。
    std::function<void(std::string_view)> on_stdout;
    /// stderr 每读到一段就回调。
    /// @warning 生命周期约束同 `on_stdout`。
    std::function<void(std::string_view)> on_stderr;
    /// 结果确定后回调一次（含被拒绝/启动失败的情况）。
    /// @warning `result` 是 `Execute` 内的局部对象，回调返回后即失效。
    std::function<void(const CommandResult&)> on_exit;
};

// ---------------------------------------------------------------------------
// 帧编解码
// ---------------------------------------------------------------------------

/**
 * @brief 编码命令请求帧。
 * @return 成功返回完整帧；参数非法（空 argv、超长、总帧超限）返回空 vector。
 */
std::vector<std::uint8_t> EncodeCommand(const Command& command);

/**
 * @brief 解码命令请求帧。
 * @param data 帧起始地址。
 * @param size 帧字节数。
 * @param out 成功时写入请求。
 * @param error 可选输出：失败原因。
 * @return 成功返回 true；任何长度/字段违规返回 false 且不改动 `*out`。
 */
bool DecodeCommand(const std::uint8_t* data, std::size_t size, Command* out,
                   std::string* error = nullptr);

/**
 * @brief 编码命令结果帧。
 * @return 成功返回完整帧；字段超限返回空 vector。
 */
std::vector<std::uint8_t> EncodeCommandResult(const CommandResult& result);

/**
 * @brief 解码命令结果帧。
 * @return 成功返回 true；失败返回 false 并（可选）写 `error`。
 */
bool DecodeCommandResult(const std::uint8_t* data, std::size_t size, CommandResult* out,
                         std::string* error = nullptr);

// ---------------------------------------------------------------------------
// 执行器
// ---------------------------------------------------------------------------

/**
 * @brief 命令执行器：校验 → spawn → 边读边等 → 超时杀进程树 → 汇总结果。
 *
 * @details 执行器本身无状态（只有一份不可变策略），`Execute` 可在多个线程
 *          上并发调用；每次调用创建独立的子进程与管道。
 *
 *          超时阶梯：到点对子进程组发 `SIGTERM`，等待 `kill_grace_ms` 后仍
 *          存活则发 `SIGKILL`。即使子进程忽略 TERM，孙进程也会被一起杀掉
 *          （spawn 时 `new_session = true`，子进程是组长）。
 */
class CommandExecutor final {
public:
    /// 用给定策略构造；缺省策略是全拒绝，必须显式配置白名单。
    explicit CommandExecutor(CommandPolicy policy = CommandPolicy{});

    /// 执行命令（无回调）。
    CommandResult Execute(const Command& command) const;

    /// 执行命令并触发回调。
    CommandResult Execute(const Command& command, const CommandCallbacks& callbacks) const;

    /// 当前策略（只读）。
    const CommandPolicy& policy() const noexcept { return m_policy; }

private:
    /// 不可变执行策略；构造后不再修改，`Execute` 只读。
    CommandPolicy m_policy;
};

}  // namespace go2cpp::bento
