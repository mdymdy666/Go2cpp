/**
 * @file process.cpp
 * @brief 子进程执行实现：统一委托给 `go2cpp::os::Run`。
 *
 * @details
 * 早期版本在这里自行维护管道、轮询与回收逻辑；切换后本文件只做
 * **MCP 语义适配**，真正的启动、输出收集、超时阶梯与回收全部由
 * `go2cpp::os` 负责：
 * - argv 直接 exec（`use_path_search=true` 等价旧的 PATH 查找），不经过 shell；
 * - stdout/stderr 合并到同一个管道（`stderr_to_stdout=true`），保持原有
 *   「调用方拿到原始交错输出」的约定；
 * - 超时按 TERM → 短宽限 → KILL 阶梯终止，已收集的输出照常返回；
 * - 子进程退出码原样透传，被信号终止时统一映射为 -1（与旧接口一致）。
 */

#include "process.hpp"

#include <chrono>
#include <mutex>
#include <utility>

#include "go2cpp/os/proc.hpp"

namespace go2cpp::mcp::util {

namespace {

/// 未显式给出超时（<=0）时的默认值，沿袭旧实现。
constexpr int kDefaultTimeoutMs = 5000;

/**
 * @brief 超时后 TERM → KILL 的宽限期。
 *
 * 旧实现到点直接 SIGKILL；这里保留极短宽限（200ms）让 `curl` 等进程有机会
 * 自行清理，同时把超时总延迟控制在一个可接受的范围内。
 */
constexpr std::chrono::milliseconds kTerminateGrace{200};

/**
 * @brief 忽略 SIGPIPE（进程级、只做一次）。
 *
 * `Run` 写子进程 stdin 时若对端已经退出，`write` 会触发 SIGPIPE；默认动作
 * 会直接杀死 MCP 服务。旧实现在同一路径上也有这个隐患，切换时一并消除。
 * 该设置只影响本进程，不改变库的其它行为。
 */
void EnsureSigPipeIgnored() {
    static std::once_flag flag;
    std::call_once(flag, [] { go2cpp::os::IgnoreSigPipe(); });
}

}  // namespace

ProcessResult RunProcess(const std::vector<std::string>& argv, int timeout_ms,
                         const std::string& stdin_data) {
    ProcessResult result;
    if (argv.empty()) {
        return result;
    }

    EnsureSigPipeIgnored();

    go2cpp::os::ProcessOptions options;
    options.argv = argv;
    // 旧实现用 execvp：裸程序名按 PATH 查找；os 的 PATH 解析在父进程完成，
    // 仍然逐项 exec，不拼 shell 命令。
    options.use_path_search = true;
    // 无输入时重定向 /dev/null（等价旧实现的 O_RDONLY /dev/null）；有输入时
    // 建管道，由 Run 负责写入并在数据耗尽后关闭写端（子进程看到 EOF）。
    options.stdin_mode = stdin_data.empty() ? go2cpp::os::StdioMode::kNull
                                            : go2cpp::os::StdioMode::kPipe;
    options.stdout_mode = go2cpp::os::StdioMode::kPipe;
    // 两条流共用同一管道，保持旧接口的「合并输出」语义（含交错顺序）。
    options.stderr_to_stdout = true;
    // os 的 timeout=0 表示不限时；MCP 旧接口约定 <=0 回退到 5s，这里显式转换。
    options.timeout = std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms
                                                               : kDefaultTimeoutMs);
    options.grace = kTerminateGrace;
    // 旧实现不截断输出；保持无限上限，避免改变 curl / ros2 的返回内容。
    options.max_output_bytes = 0;

    auto run = go2cpp::os::Run(options, stdin_data);
    if (!run.ok()) {
        const auto& error = run.error();
        if (error.code == go2cpp::os::ProcErrorCode::kExecFailed) {
            // 保持旧语义：exec 失败时退出码 127，并给出可读原因（旧实现只写
            // 一行 "exec failed: <name>"，这里补上底层 errno 便于诊断）。
            result.exit_code = 127;
            result.output = "exec failed: " + argv.front() + "\n" + error.ToString() + "\n";
        } else {
            result.output = "spawn failed: " + error.ToString();
        }
        return result;
    }

    go2cpp::os::RunResult& payload = run.value();
    const go2cpp::os::ExitStatus& status = payload.status;
    result.timed_out = status.timed_out;
    if (!status.timed_out && status.reaped && status.exited) {
        result.exit_code = status.exit_code;
    } else {
        // 与旧实现一致：超时、被信号终止或未拿到状态时统一记 -1。
        result.exit_code = -1;
    }
    result.output = std::move(payload.standard_output);
    // stderr_to_stdout 正常时 standard_error 为空；此处只做防御性拼接，避免
    // 未来 os 行为调整导致 stderr 静默丢失（顺序上位于 stdout 之后）。
    if (!payload.standard_error.empty()) {
        result.output += payload.standard_error;
    }
    return result;
}

}  // namespace go2cpp::mcp::util
