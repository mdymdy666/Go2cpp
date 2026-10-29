/**
 * @file test_command.cpp
 * @brief MOD-26 命令协议与执行器测试：帧边界、白名单、真实执行、超时杀
 *        进程树、输出截断、cwd 沙箱与「不经过 shell」的注入面验证。
 * @details 覆盖 16 个场景：
 *          1.  请求/结果帧往返与非法帧拒绝（含超过 64KiB 硬上限的请求帧）；
 *          2.  白名单/拒绝表/内置危险命令的优先级；
 *          3.  参数边界（空 argv、超长、NUL、超时、相对 cwd、timeout=0）；
 *          4.  cwd 规范化后的沙箱判定（含符号链接与空 cwd）；
 *          5.  正常执行 + stdout/exit code；
 *          6.  stderr + 非零退出码；
 *          7.  四个回调的触发与顺序；
 *          8.  超时后杀进程（耗时上限、SIGTERM 诊断）；
 *          9.  超时后杀**进程树**（孙进程消失）；
 *          10. 输出上限截断（stdout + stderr）；
 *          11. spawn 失败（exec 不存在的程序）；
 *          12. cwd 沙箱下的真实执行与越界拒绝；
 *          13. 参数原样传递（不经 shell，`$()` 不被解释）；
 *          14. 默认全拒绝；
 *          15. 关闭 PATH 搜索时只接受可执行文件路径；
 *          16. 反复执行后的 fd/僵尸稳定性。
 * @author go2cpp contributors
 * @date 2026-10-28
 * @copyright MIT License
 */

#include <dirent.h>
#include <sys/types.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include "go2cpp/bento/net/command.hpp"
#include "test_util.hpp"

namespace {

using go2cpp::bento::Command;
using go2cpp::bento::CommandCallbacks;
using go2cpp::bento::CommandExecutor;
using go2cpp::bento::CommandPolicy;
using go2cpp::bento::CommandResult;
using go2cpp::bento::CommandStatus;
using go2cpp::bento::DecodeCommand;
using go2cpp::bento::DecodeCommandResult;
using go2cpp::bento::EncodeCommand;
using go2cpp::bento::EncodeCommandResult;
using go2cpp::bento::ValidateCommand;

/// 创建唯一的临时目录；失败返回空路径。
std::filesystem::path MakeTempDir(const char* tag) {
    const std::filesystem::path base = std::filesystem::temp_directory_path();
    for (int index = 0; index < 100; ++index) {
        const std::filesystem::path dir =
            base / (std::string("bento_") + tag + "_" + std::to_string(::getpid()) + "_" +
                    std::to_string(index));
        std::error_code ec;
        if (std::filesystem::create_directories(dir, ec) && !ec) {
            return dir;
        }
    }
    return {};
}

/// 递归删除临时目录。
void RemoveTree(const std::filesystem::path& dir) {
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

/// 构造只允许 `names` 的策略（其余默认：内置危险表生效、全拒绝兜底）。
CommandPolicy PolicyFor(std::initializer_list<const char*> names) {
    CommandPolicy policy;
    for (const char* name : names) {
        policy.allowed_commands.emplace_back(name);
    }
    return policy;
}

/// 从命令输出首行解析 pid；失败返回 -1。
pid_t ParsePid(const std::string& text) {
    const std::size_t end = text.find_first_of("\r\n");
    const std::string line = text.substr(0, end);
    if (line.empty()) {
        return -1;
    }
    try {
        const long value = std::stol(line);
        if (value > 0) {
            return static_cast<pid_t>(value);
        }
    } catch (...) {
        return -1;
    }
    return -1;
}

/// 在 timeout_ms 内等待 /proc/<pid> 消失；返回是否消失。
bool WaitProcessGone(pid_t pid, int timeout_ms) {
    const std::string proc = "/proc/" + std::to_string(static_cast<long>(pid));
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (!std::filesystem::exists(proc)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return !std::filesystem::exists(proc);
}

/// 统计本进程的僵尸子进程数（扫描 /proc/<pid>/stat 的 state + ppid）。
int CountZombieChildren() {
    DIR* dir = ::opendir("/proc");
    if (dir == nullptr) {
        return 0;
    }
    int zombies = 0;
    const pid_t self = ::getpid();
    while (dirent* entry = ::readdir(dir)) {
        const char* name = entry->d_name;
        if (name[0] < '0' || name[0] > '9') {
            continue;
        }
        std::ifstream in(std::string("/proc/") + name + "/stat");
        std::string line;
        if (!std::getline(in, line)) {
            continue;
        }
        const std::size_t close_paren = line.rfind(')');
        if (close_paren == std::string::npos) {
            continue;
        }
        char state = '\0';
        int ppid = 0;
        if (std::sscanf(line.c_str() + close_paren + 1, " %c %d", &state, &ppid) == 2 &&
            state == 'Z' && ppid == static_cast<int>(self)) {
            ++zombies;
        }
    }
    ::closedir(dir);
    return zombies;
}

/// 本进程当前 fd 数（含 opendir 自身的 fd，多次采样口径一致）。
std::size_t CountFds() {
    DIR* dir = ::opendir("/proc/self/fd");
    if (dir == nullptr) {
        return 0;
    }
    std::size_t count = 0;
    while (::readdir(dir) != nullptr) {
        ++count;
    }
    ::closedir(dir);
    return count;
}

/// 手工拼一个请求帧：字段各自合法，但总长可超过 64KiB 硬上限。
std::vector<std::uint8_t> MakeRequestFrame(std::size_t argc, std::size_t arg_bytes) {
    const std::size_t total =
        go2cpp::bento::kCommandFrameHeaderBytes + 4 * argc + argc * arg_bytes;
    std::vector<std::uint8_t> frame(total, 'a');
    frame[0] = go2cpp::bento::kCommandFrameMagic0;
    frame[1] = go2cpp::bento::kCommandFrameMagic1;
    frame[2] = go2cpp::bento::kCommandProtocolVersion;
    frame[3] = go2cpp::bento::kCommandFrameTypeRequest;
    frame[4] = 0;
    frame[5] = 0;  // id_len
    frame[6] = static_cast<std::uint8_t>(argc >> 8);
    frame[7] = static_cast<std::uint8_t>(argc);
    frame[8] = 0;
    frame[9] = 0;
    frame[10] = 0x13;
    frame[11] = 0x88;  // timeout = 5000
    frame[12] = 0;
    frame[13] = 0;  // cwd_len
    frame[14] = 0;
    frame[15] = 0;  // reserved
    for (std::size_t i = 0; i < argc; ++i) {
        const std::uint32_t length = static_cast<std::uint32_t>(arg_bytes);
        const std::size_t offset = go2cpp::bento::kCommandFrameHeaderBytes + 4 * i;
        frame[offset] = static_cast<std::uint8_t>(length >> 24);
        frame[offset + 1] = static_cast<std::uint8_t>(length >> 16);
        frame[offset + 2] = static_cast<std::uint8_t>(length >> 8);
        frame[offset + 3] = static_cast<std::uint8_t>(length);
    }
    return frame;
}

/// 场景 1：请求帧往返 + 各类非法帧。
void TestRequestFrameRoundTrip() {
    Command request;
    request.id = "req-1";
    request.argv = {"/bin/echo", "hello", "世界"};
    request.timeout_ms = 5000;
    request.cwd = "/tmp";

    const std::vector<std::uint8_t> frame = EncodeCommand(request);
    BENTO_CHECK(!frame.empty());

    Command decoded;
    std::string error;
    BENTO_CHECK(DecodeCommand(frame.data(), frame.size(), &decoded, &error));
    BENTO_CHECK_EQ(decoded.id, request.id);
    BENTO_CHECK_EQ(decoded.argv.size(), request.argv.size());
    BENTO_CHECK_EQ(decoded.timeout_ms, request.timeout_ms);
    BENTO_CHECK_EQ(decoded.cwd, request.cwd);
    for (std::size_t i = 0; i < decoded.argv.size(); ++i) {
        BENTO_CHECK_EQ(decoded.argv[i], request.argv[i]);
    }

    // 截断、magic 篡改、尾部多余字节都必须被拒绝。
    BENTO_CHECK(!DecodeCommand(frame.data(), frame.size() - 1, &decoded, &error));
    std::vector<std::uint8_t> broken = frame;
    broken[0] = 'X';
    BENTO_CHECK(!DecodeCommand(broken.data(), broken.size(), &decoded, &error));
    broken = frame;
    broken.push_back(0);
    BENTO_CHECK(!DecodeCommand(broken.data(), broken.size(), &decoded, &error));
    // NULL 输入。
    BENTO_CHECK(!DecodeCommand(nullptr, 0, &decoded, &error));

    // 非法请求无法编码。
    Command empty_argv;
    empty_argv.id = "x";
    BENTO_CHECK(EncodeCommand(empty_argv).empty());
    Command long_id;
    long_id.id = std::string(go2cpp::bento::kCommandMaxIdBytes + 1, 'a');
    long_id.argv = {"echo"};
    BENTO_CHECK(EncodeCommand(long_id).empty());
    Command long_arg;
    long_arg.id = "x";
    long_arg.argv = {"echo", std::string(go2cpp::bento::kCommandMaxArgBytes + 1, 'b')};
    BENTO_CHECK(EncodeCommand(long_arg).empty());

    // NUL 会在 C 边界截断语义，编码与校验都必须拒绝。
    Command nul_arg;
    nul_arg.id = "x";
    nul_arg.argv = {"echo", std::string("a\0b", 3)};
    BENTO_CHECK(EncodeCommand(nul_arg).empty());
    Command nul_id;
    nul_id.id = std::string("a\0b", 3);
    nul_id.argv = {"echo"};
    BENTO_CHECK(EncodeCommand(nul_id).empty());

    // 各字段合法但总长 > 64KiB：解码必须在分配/解析前按硬上限拒绝。
    const std::vector<std::uint8_t> oversized = MakeRequestFrame(17, 3855);
    BENTO_CHECK(oversized.size() > go2cpp::bento::kCommandMaxFrameBytes);
    Command oversized_out;
    BENTO_CHECK(!DecodeCommand(oversized.data(), oversized.size(), &oversized_out, &error));

    // 解码器与编码器对称：argv 里的 NUL 必须拒绝（否则解析出来却编不回去）。
    std::vector<std::uint8_t> nul_frame = frame;
    const std::size_t nul_offset =
        go2cpp::bento::kCommandFrameHeaderBytes + 4 * request.argv.size() + request.id.size() +
        request.cwd.size() + request.argv[0].size() + 1;
    BENTO_CHECK(nul_offset < nul_frame.size());
    nul_frame[nul_offset] = 0;
    BENTO_CHECK(!DecodeCommand(nul_frame.data(), nul_frame.size(), &decoded, &error));
}

/// 场景 1（续）：结果帧往返 + 非法帧。
void TestResultFrameRoundTrip() {
    CommandResult result;
    result.id = "res-1";
    result.status = CommandStatus::kNonZeroExit;
    result.exit_code = 7;
    result.timed_out = true;
    result.output_truncated = true;
    result.duration_ms = 1234;
    result.standard_output = "out";
    result.standard_error = "err";
    result.message = "测试消息";

    const std::vector<std::uint8_t> frame = EncodeCommandResult(result);
    BENTO_CHECK(!frame.empty());

    CommandResult decoded;
    std::string error;
    BENTO_CHECK(DecodeCommandResult(frame.data(), frame.size(), &decoded, &error));
    BENTO_CHECK_EQ(decoded.id, result.id);
    BENTO_CHECK_EQ(decoded.status, result.status);
    BENTO_CHECK_EQ(decoded.exit_code, result.exit_code);
    BENTO_CHECK_EQ(decoded.timed_out, result.timed_out);
    BENTO_CHECK_EQ(decoded.output_truncated, result.output_truncated);
    BENTO_CHECK_EQ(decoded.duration_ms, result.duration_ms);
    BENTO_CHECK_EQ(decoded.standard_output, result.standard_output);
    BENTO_CHECK_EQ(decoded.standard_error, result.standard_error);
    BENTO_CHECK_EQ(decoded.message, result.message);

    BENTO_CHECK(!DecodeCommandResult(frame.data(), frame.size() - 1, &decoded, &error));
    std::vector<std::uint8_t> broken = frame;
    broken[5] |= 0x80;  // 未知 flag 位。
    BENTO_CHECK(!DecodeCommandResult(broken.data(), broken.size(), &decoded, &error));
    broken = frame;
    broken[4] = 99;  // status 越界。
    BENTO_CHECK(!DecodeCommandResult(broken.data(), broken.size(), &decoded, &error));
    broken = frame;
    broken[3] = 1;  // 把结果帧伪装成请求帧。
    BENTO_CHECK(!DecodeCommandResult(broken.data(), broken.size(), &decoded, &error));

    // 越界 status 不能编码出「自己解不回来」的帧。
    CommandResult bad_status = result;
    bad_status.status = static_cast<CommandStatus>(99);
    BENTO_CHECK(EncodeCommandResult(bad_status).empty());
    // 单路输出超过 4MiB 硬上限：编码拒绝（解码侧也要拒绝，见下）。
    CommandResult huge = result;
    huge.standard_output.assign(go2cpp::bento::kCommandMaxOutputCeiling + 1, 'x');
    BENTO_CHECK(EncodeCommandResult(huge).empty());
    // 单路输出 4MiB+1 且总长精确匹配：解码必须按单路上限拒绝，而不是先
    // 分配 4MiB 字符串再交给调用方。
    std::vector<std::uint8_t> forged(
        go2cpp::bento::kCommandResultFrameHeaderBytes +
            go2cpp::bento::kCommandMaxOutputCeiling + 1,
        0);
    forged[0] = go2cpp::bento::kCommandFrameMagic0;
    forged[1] = go2cpp::bento::kCommandFrameMagic1;
    forged[2] = go2cpp::bento::kCommandProtocolVersion;
    forged[3] = go2cpp::bento::kCommandFrameTypeResult;
    const std::uint32_t oversized_len =
        static_cast<std::uint32_t>(go2cpp::bento::kCommandMaxOutputCeiling + 1);
    forged[12] = static_cast<std::uint8_t>(oversized_len >> 24);
    forged[13] = static_cast<std::uint8_t>(oversized_len >> 16);
    forged[14] = static_cast<std::uint8_t>(oversized_len >> 8);
    forged[15] = static_cast<std::uint8_t>(oversized_len);
    BENTO_CHECK(!DecodeCommandResult(forged.data(), forged.size(), &decoded, &error));

    // 结果帧 id/message 的 NUL 同样拒绝（输出流是任意字节，不在此列）。
    std::vector<std::uint8_t> nul_message = frame;
    const std::size_t message_offset = go2cpp::bento::kCommandResultFrameHeaderBytes +
                                       result.id.size() + result.standard_output.size() +
                                       result.standard_error.size();
    BENTO_CHECK(message_offset < nul_message.size());
    nul_message[message_offset] = 0;
    BENTO_CHECK(!DecodeCommandResult(nul_message.data(), nul_message.size(), &decoded, &error));
}

/// 场景 2：白名单、拒绝表、内置危险命令的优先级。
void TestPolicyValidation() {
    Command echo;
    echo.id = "v";
    echo.argv = {"echo", "x"};

    std::string reason;
    const CommandPolicy deny_all;
    BENTO_CHECK_EQ(ValidateCommand(echo, deny_all, &reason), CommandStatus::kDenied);
    BENTO_CHECK(!reason.empty());

    const CommandPolicy allow_echo = PolicyFor({"echo"});
    BENTO_CHECK_EQ(ValidateCommand(echo, allow_echo, &reason), CommandStatus::kOk);

    // 自定义拒绝表优先级最高。
    CommandPolicy deny_listed = allow_echo;
    deny_listed.allow_all = true;
    deny_listed.allow_dangerous = true;
    deny_listed.denied_commands = {"echo"};
    BENTO_CHECK_EQ(ValidateCommand(echo, deny_listed, &reason), CommandStatus::kDenied);

    // 内置危险命令：allow_all 也拒绝，显式 allow_dangerous 才放行。
    Command shell;
    shell.id = "sh";
    shell.argv = {"sh", "-c", "true"};
    CommandPolicy allow_all;
    allow_all.allow_all = true;
    BENTO_CHECK_EQ(ValidateCommand(shell, allow_all, &reason), CommandStatus::kDenied);
    allow_all.allow_dangerous = true;
    BENTO_CHECK_EQ(ValidateCommand(shell, allow_all, &reason), CommandStatus::kOk);

    // 大小写/路径变体不能绕过内置表。
    Command upper_shell;
    upper_shell.argv = {"/BIN/SH", "-c", "true"};
    CommandPolicy dangerous_off = allow_all;
    dangerous_off.allow_dangerous = false;
    BENTO_CHECK_EQ(ValidateCommand(upper_shell, dangerous_off, &reason),
                   CommandStatus::kDenied);

    // timeout=0 表示不限时：默认必须拒绝，否则可绕过 max_timeout_ms。
    CommandPolicy echo_policy = PolicyFor({"echo"});
    Command unlimited = echo;
    unlimited.timeout_ms = 0;
    BENTO_CHECK_EQ(ValidateCommand(unlimited, echo_policy, &reason),
                   CommandStatus::kInvalidArgument);
    echo_policy.allow_unlimited_timeout = true;
    BENTO_CHECK_EQ(ValidateCommand(unlimited, echo_policy, &reason), CommandStatus::kOk);
    // 策略自身超过硬上限时整体拒绝。
    CommandPolicy bad_timeout_policy = PolicyFor({"echo"});
    bad_timeout_policy.max_timeout_ms = go2cpp::bento::kCommandMaxTimeoutMs + 1;
    BENTO_CHECK_EQ(ValidateCommand(echo, bad_timeout_policy, &reason),
                   CommandStatus::kInvalidArgument);

    // denied_commands 命中的是 basename，路径变体同样拒绝。
    CommandPolicy deny_echo = allow_all;
    deny_echo.denied_commands = {"echo"};
    Command path_echo;
    path_echo.argv = {"/bin/echo", "x"};
    BENTO_CHECK_EQ(ValidateCommand(path_echo, deny_echo, &reason), CommandStatus::kDenied);

    // 内置表本身可查询，且包含常见的危险程序。
    const std::vector<std::string>& dangerous = go2cpp::bento::BuiltinDangerousCommands();
    BENTO_CHECK(dangerous.size() >= 30);
    for (const char* name : {"sh", "rm", "dd", "sudo", "kill", "curl"}) {
        bool found = false;
        for (const std::string& entry : dangerous) {
            if (entry == name) {
                found = true;
                break;
            }
        }
        BENTO_CHECK(found);
    }
}

/// 场景 3/4：参数边界与 cwd 沙箱。
void TestArgumentBounds() {
    const CommandPolicy allow_echo = PolicyFor({"echo"});
    std::string reason;

    Command command;
    command.id = "bounds";
    command.argv = {};
    BENTO_CHECK_EQ(ValidateCommand(command, allow_echo, &reason), CommandStatus::kInvalidArgument);

    command.argv = {"echo"};
    command.argv.resize(go2cpp::bento::kCommandMaxArgv + 1, "x");
    BENTO_CHECK_EQ(ValidateCommand(command, allow_echo, &reason), CommandStatus::kInvalidArgument);

    command.argv = {"echo", std::string(go2cpp::bento::kCommandMaxArgBytes + 1, 'a')};
    BENTO_CHECK_EQ(ValidateCommand(command, allow_echo, &reason), CommandStatus::kInvalidArgument);

    command.argv = {"echo", std::string("a\0b", 3)};
    BENTO_CHECK_EQ(ValidateCommand(command, allow_echo, &reason), CommandStatus::kInvalidArgument);

    command.argv = {"echo"};
    command.timeout_ms = go2cpp::bento::kCommandMaxTimeoutMs + 1;
    BENTO_CHECK_EQ(ValidateCommand(command, allow_echo, &reason), CommandStatus::kInvalidArgument);

    command.timeout_ms = 0;
    BENTO_CHECK_EQ(ValidateCommand(command, allow_echo, &reason), CommandStatus::kInvalidArgument);

    command.timeout_ms = 1000;
    command.cwd = "relative/path";
    BENTO_CHECK_EQ(ValidateCommand(command, allow_echo, &reason), CommandStatus::kInvalidArgument);

    command.cwd = std::string("a\0b", 3);
    BENTO_CHECK_EQ(ValidateCommand(command, allow_echo, &reason), CommandStatus::kInvalidArgument);

    command.cwd = "/tmp";
    CommandPolicy sandboxed = allow_echo;
    sandboxed.allowed_cwd_roots = {"/tmp"};
    BENTO_CHECK_EQ(ValidateCommand(command, sandboxed, &reason), CommandStatus::kOk);

    command.cwd = "/tmp/../etc";
    BENTO_CHECK_EQ(ValidateCommand(command, sandboxed, &reason), CommandStatus::kDenied);

    command.cwd = "/etc";
    BENTO_CHECK_EQ(ValidateCommand(command, sandboxed, &reason), CommandStatus::kDenied);

    // 空 cwd 会继承服务进程 cwd，启用沙箱时必须拒绝。
    command.cwd = "";
    BENTO_CHECK_EQ(ValidateCommand(command, sandboxed, &reason), CommandStatus::kDenied);

    // 策略本身非法也应返回 kInvalidArgument，而不是放行。
    CommandPolicy bad_policy = allow_echo;
    bad_policy.max_output_bytes = 0;
    BENTO_CHECK_EQ(ValidateCommand(command, bad_policy, &reason),
                   CommandStatus::kInvalidArgument);
}

/// 场景 5：正常执行，捕获 stdout 与退出码。
void TestExecuteEcho() {
    const CommandExecutor executor(PolicyFor({"echo"}));
    Command command;
    command.id = "echo";
    command.argv = {"echo", "hello"};
    command.timeout_ms = 5000;
    const CommandResult result = executor.Execute(command);
    BENTO_CHECK_EQ(result.status, CommandStatus::kOk);
    BENTO_CHECK_EQ(result.exit_code, 0);
    BENTO_CHECK_EQ(result.standard_output, std::string("hello\n"));
    BENTO_CHECK(result.standard_error.empty());
    BENTO_CHECK(!result.timed_out);
    BENTO_CHECK(!result.output_truncated);
    BENTO_CHECK_EQ(result.id, std::string("echo"));
}

/// 场景 6：stderr 捕获与非零退出码。
void TestExecuteStderrAndFailure() {
    const CommandExecutor executor(PolicyFor({"ls"}));
    Command command;
    command.id = "ls";
    command.argv = {"ls", "/bento-no-such-path-xyz"};
    command.timeout_ms = 5000;
    const CommandResult result = executor.Execute(command);
    BENTO_CHECK_EQ(result.status, CommandStatus::kNonZeroExit);
    BENTO_CHECK(result.exit_code != 0);
    BENTO_CHECK(!result.standard_error.empty());
    BENTO_CHECK(result.standard_output.empty());
}

/// 场景 7：四个回调都触发，stdout 内容拼接正确。
void TestExecuteCallbacks() {
    const CommandExecutor executor(PolicyFor({"echo"}));
    Command command;
    command.id = "callbacks";
    command.argv = {"echo", "callback"};
    command.timeout_ms = 5000;

    pid_t spawned = -1;
    std::string stdout_text;
    std::string stderr_text;
    bool exited = false;
    CommandResult captured;
    CommandCallbacks callbacks;
    callbacks.on_spawn = [&spawned](pid_t pid) { spawned = pid; };
    callbacks.on_stdout = [&stdout_text](std::string_view text) { stdout_text.append(text); };
    callbacks.on_stderr = [&stderr_text](std::string_view text) { stderr_text.append(text); };
    callbacks.on_exit = [&exited, &captured](const CommandResult& result) {
        exited = true;
        captured = result;
    };

    const CommandResult result = executor.Execute(command, callbacks);
    BENTO_CHECK(spawned > 0);
    BENTO_CHECK_EQ(stdout_text, std::string("callback\n"));
    BENTO_CHECK(stderr_text.empty());
    BENTO_CHECK(exited);
    BENTO_CHECK_EQ(captured.status, CommandStatus::kOk);
    BENTO_CHECK_EQ(result.status, CommandStatus::kOk);
}

/// 场景 8：超时后杀进程，耗时有上限。
void TestExecuteTimeout() {
    const CommandExecutor executor(PolicyFor({"sleep"}));
    Command command;
    command.id = "timeout";
    command.argv = {"sleep", "5"};
    command.timeout_ms = 200;
    const auto start = std::chrono::steady_clock::now();
    const CommandResult result = executor.Execute(command);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - start)
                             .count();
    BENTO_CHECK_EQ(result.status, CommandStatus::kTimeout);
    BENTO_CHECK(result.timed_out);
    BENTO_CHECK(elapsed >= 150);
    BENTO_CHECK(elapsed < 3000);
    BENTO_CHECK(result.message.find("SIGTERM") != std::string::npos);
    BENTO_CHECK_EQ(CountZombieChildren(), 0);
}

/// 场景 9：超时杀整个进程组（后台孙进程也被杀）。
void TestExecuteTimeoutKillsTree() {
    CommandPolicy policy;
    policy.allow_all = true;
    policy.allow_dangerous = true;
    const CommandExecutor executor(policy);
    Command command;
    command.id = "tree";
    command.argv = {"sh", "-c", "sleep 30 & echo $!; wait"};
    command.timeout_ms = 400;
    const CommandResult result = executor.Execute(command);
    BENTO_CHECK_EQ(result.status, CommandStatus::kTimeout);
    const pid_t child = ParsePid(result.standard_output);
    BENTO_CHECK(child > 0);
    if (child > 0) {
        BENTO_CHECK(WaitProcessGone(child, 2000));
    }
    BENTO_CHECK_EQ(CountZombieChildren(), 0);
}

/// 场景 10：输出上限截断且不阻塞（stdout 与 stderr 两路）。
void TestOutputTruncation() {
    const std::filesystem::path dir = MakeTempDir("cmd_out");
    BENTO_CHECK(!dir.empty());
    if (dir.empty()) {
        return;
    }
    const std::filesystem::path file = dir / "big.bin";
    const std::size_t file_size = 200 * 1024;
    {
        std::ofstream out(file, std::ios::binary);
        const std::vector<char> block(file_size, 'A');
        out.write(block.data(), static_cast<std::streamsize>(block.size()));
    }
    CommandPolicy policy = PolicyFor({"cat"});
    policy.max_output_bytes = 1024;
    const CommandExecutor executor(policy);
    Command command;
    command.id = "truncate";
    command.argv = {"cat", file.string()};
    command.timeout_ms = 5000;
    std::size_t callback_bytes = 0;
    CommandCallbacks callbacks;
    callbacks.on_stdout = [&callback_bytes](std::string_view text) {
        callback_bytes += text.size();
    };
    const CommandResult result = executor.Execute(command, callbacks);
    BENTO_CHECK_EQ(result.status, CommandStatus::kOk);
    BENTO_CHECK_EQ(result.standard_output.size(), static_cast<std::size_t>(1024));
    BENTO_CHECK(result.output_truncated);
    BENTO_CHECK(result.standard_error.empty());
    // 回调拿到的是线上原始内容：即使缓冲截断，也必须完整排空，否则子进程会阻塞。
    BENTO_CHECK_EQ(callback_bytes, file_size);

    // stderr 同样有每路上限。
    CommandPolicy stderr_policy;
    stderr_policy.allowed_commands = {"sh"};
    stderr_policy.allow_dangerous = true;
    stderr_policy.max_output_bytes = 512;
    const CommandExecutor stderr_executor(stderr_policy);
    Command stderr_command;
    stderr_command.id = "stderr-truncate";
    stderr_command.argv = {"sh", "-c", "cat '" + file.string() + "' >&2"};
    stderr_command.timeout_ms = 5000;
    const CommandResult stderr_result = stderr_executor.Execute(stderr_command);
    BENTO_CHECK_EQ(stderr_result.status, CommandStatus::kOk);
    BENTO_CHECK(stderr_result.standard_output.empty());
    BENTO_CHECK_EQ(stderr_result.standard_error.size(), static_cast<std::size_t>(512));
    BENTO_CHECK(stderr_result.output_truncated);
    RemoveTree(dir);
}

/// 场景 11：exec 不存在的程序 → kSpawnFailed。
void TestSpawnFailure() {
    const CommandExecutor executor(PolicyFor({"bento-no-such-command-xyz"}));
    Command command;
    command.id = "missing";
    command.argv = {"bento-no-such-command-xyz"};
    command.timeout_ms = 5000;
    const CommandResult result = executor.Execute(command);
    BENTO_CHECK_EQ(result.status, CommandStatus::kSpawnFailed);
    BENTO_CHECK(!result.message.empty());
}

/// 场景 12：cwd 沙箱下的真实执行与越界拒绝。
void TestCwdSandboxExecution() {
    const std::filesystem::path dir = MakeTempDir("cmd_cwd");
    BENTO_CHECK(!dir.empty());
    if (dir.empty()) {
        return;
    }
    CommandPolicy policy = PolicyFor({"pwd"});
    policy.allowed_cwd_roots = {dir.string()};
    const CommandExecutor executor(policy);
    Command command;
    command.id = "pwd";
    command.argv = {"pwd"};
    command.timeout_ms = 5000;
    command.cwd = dir.string();
    const CommandResult result = executor.Execute(command);
    BENTO_CHECK_EQ(result.status, CommandStatus::kOk);
    std::string printed = result.standard_output;
    while (!printed.empty() && (printed.back() == '\n' || printed.back() == '\r')) {
        printed.pop_back();
    }
    BENTO_CHECK_EQ(printed, std::filesystem::weakly_canonical(dir).string());

    command.cwd = "/etc";
    const CommandResult denied = executor.Execute(command);
    BENTO_CHECK_EQ(denied.status, CommandStatus::kDenied);
    BENTO_CHECK(denied.standard_output.empty());

    // 空 cwd 会继承服务进程 cwd，不能借它绕过沙箱。
    command.cwd = "";
    BENTO_CHECK_EQ(executor.Execute(command).status, CommandStatus::kDenied);

    // 符号链接逃逸：root/escape -> /etc，规范化后必须判定为越界。
    const std::filesystem::path escape = dir / "escape";
    std::error_code ec;
    std::filesystem::create_directory_symlink("/etc", escape, ec);
    if (!ec) {
        command.cwd = escape.string();
        BENTO_CHECK_EQ(executor.Execute(command).status, CommandStatus::kDenied);
    }

    // 根内的不存在目录：沙箱允许但 spawn 失败，不能误报 kOk。
    command.cwd = (dir / "does-not-exist").string();
    BENTO_CHECK_EQ(executor.Execute(command).status, CommandStatus::kSpawnFailed);
    RemoveTree(dir);
}

/// 场景 13：参数原样传递，不经 shell 解释。
void TestNoShellInterpretation() {
    const CommandExecutor executor(PolicyFor({"echo"}));
    Command command;
    command.id = "literal";
    command.argv = {"echo", "$(id); rm -rf /"};
    command.timeout_ms = 5000;
    const CommandResult result = executor.Execute(command);
    BENTO_CHECK_EQ(result.status, CommandStatus::kOk);
    BENTO_CHECK_EQ(result.standard_output, std::string("$(id); rm -rf /\n"));
}

/// 追加场景：默认全拒绝策略连 echo 都不执行。
void TestDefaultPolicyDenies() {
    const CommandExecutor executor;
    Command command;
    command.id = "denied";
    command.argv = {"echo", "x"};
    command.timeout_ms = 5000;
    const CommandResult result = executor.Execute(command);
    BENTO_CHECK_EQ(result.status, CommandStatus::kDenied);
    BENTO_CHECK(result.standard_output.empty());
}

/// 场景 15：关闭 PATH 搜索后只接受可执行文件路径。
void TestPathSearchDisabled() {
    CommandPolicy policy;
    policy.allow_all = true;
    policy.use_path_search = false;
    const CommandExecutor executor(policy);

    Command absolute;
    absolute.argv = {"/bin/true"};
    absolute.timeout_ms = 5000;
    BENTO_CHECK_EQ(executor.Execute(absolute).status, CommandStatus::kOk);

    Command bare;
    bare.argv = {"true"};
    bare.timeout_ms = 5000;
    BENTO_CHECK_EQ(executor.Execute(bare).status, CommandStatus::kSpawnFailed);
}

/// 场景 16：反复执行（含超时）后 fd 不增长、无僵尸残留。
void TestFdAndZombieStability() {
    CommandPolicy policy = PolicyFor({"echo", "sleep"});
    policy.kill_grace_ms = 20;
    const CommandExecutor executor(policy);
    Command echo;
    echo.argv = {"echo", "x"};
    echo.timeout_ms = 5000;
    Command sleepy;
    sleepy.argv = {"sleep", "5"};
    sleepy.timeout_ms = 15;

    const std::size_t fds_before = CountFds();
    for (int i = 0; i < 20; ++i) {
        BENTO_CHECK_EQ(executor.Execute(echo).status, CommandStatus::kOk);
    }
    for (int i = 0; i < 5; ++i) {
        BENTO_CHECK_EQ(executor.Execute(sleepy).status, CommandStatus::kTimeout);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    BENTO_CHECK(CountFds() <= fds_before + 2);
    BENTO_CHECK_EQ(CountZombieChildren(), 0);
}

}  // namespace

int main() {
    TestRequestFrameRoundTrip();
    TestResultFrameRoundTrip();
    TestPolicyValidation();
    TestArgumentBounds();
    TestExecuteEcho();
    TestExecuteStderrAndFailure();
    TestExecuteCallbacks();
    TestExecuteTimeout();
    TestExecuteTimeoutKillsTree();
    TestOutputTruncation();
    TestSpawnFailure();
    TestCwdSandboxExecution();
    TestNoShellInterpretation();
    TestDefaultPolicyDenies();
    TestPathSearchDisabled();
    TestFdAndZombieStability();
    return bento_test::Finish("test_command");
}
