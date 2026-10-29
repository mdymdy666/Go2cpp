/**
 * @file command.cpp
 * @brief MOD-26：命令帧编解码、白名单校验与进程执行器实现。
 * @details 帧格式见 `command.hpp` 的文件头；本文件只做三件事：
 *
 *          1. 逐字段编解码（大端、精确长度）；
 *          2. 策略校验（长度/白名单/cwd 沙箱/上限）；
 *          3. 基于 `go2cpp::os::proc` 的 `Process` 执行：边读两路管道边等
 *             退出，超时按 `SIGTERM → 宽限 → SIGKILL` 终止**整个进程组**。
 *
 *          执行器不经过 shell，argv 原样交给 execve；因此 `;`/`|`/`$()`
 *          等字符没有特殊含义，注入面只剩「执行了未授权的程序」，由白名单
 *          与内置危险命令表兜住。
 *
 * @author go2cpp contributors
 * @date 2026-10-28
 * @copyright MIT License
 */

#include "go2cpp/bento/net/command.hpp"

#include "go2cpp/os/proc.hpp"

#include <poll.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace go2cpp::bento {
namespace {

// ---------------------------------------------------------------------------
// 大端读写（与 protocol.cpp 保持同一风格，不依赖主机字节序）
// ---------------------------------------------------------------------------

/// 写 16 位大端。
void WriteU16(std::uint8_t* out, std::uint16_t value) noexcept {
    out[0] = static_cast<std::uint8_t>(value >> 8);
    out[1] = static_cast<std::uint8_t>(value);
}

/// 写 32 位大端。
void WriteU32(std::uint8_t* out, std::uint32_t value) noexcept {
    out[0] = static_cast<std::uint8_t>(value >> 24);
    out[1] = static_cast<std::uint8_t>(value >> 16);
    out[2] = static_cast<std::uint8_t>(value >> 8);
    out[3] = static_cast<std::uint8_t>(value);
}

/// 写 64 位大端。
void WriteU64(std::uint8_t* out, std::uint64_t value) noexcept {
    for (int i = 7; i >= 0; --i) {
        out[7 - i] = static_cast<std::uint8_t>(value >> (i * 8));
    }
}

/// 读 16 位大端。
std::uint16_t ReadU16(const std::uint8_t* in) noexcept {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(in[0]) << 8) |
                                      static_cast<std::uint16_t>(in[1]));
}

/// 读 32 位大端。
std::uint32_t ReadU32(const std::uint8_t* in) noexcept {
    return (static_cast<std::uint32_t>(in[0]) << 24) | (static_cast<std::uint32_t>(in[1]) << 16) |
           (static_cast<std::uint32_t>(in[2]) << 8) | static_cast<std::uint32_t>(in[3]);
}

/// 读 64 位大端。
std::uint64_t ReadU64(const std::uint8_t* in) noexcept {
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value = (value << 8) | static_cast<std::uint64_t>(in[i]);
    }
    return value;
}

// ---------------------------------------------------------------------------
// 字符串/路径辅助
// ---------------------------------------------------------------------------

/// 是否包含 NUL（argv/cwd/id 都不允许，否则语义会在 C 边界被截断）。
bool HasNul(std::string_view text) noexcept {
    return text.find('\0') != std::string_view::npos;
}

/// 取路径的 basename（只按 '/' 切分；Windows 风格路径不在本模块的允许面内）。
std::string_view BaseName(std::string_view path) noexcept {
    const std::size_t pos = path.find_last_of('/');
    return pos == std::string_view::npos ? path : path.substr(pos + 1);
}

/// 转小写（仅 ASCII；命令名都是 ASCII）。
std::string ToLowerAscii(std::string_view text) {
    std::string lowered;
    lowered.reserve(text.size());
    for (const char ch : text) {
        if (ch >= 'A' && ch <= 'Z') {
            lowered.push_back(static_cast<char>(ch - 'A' + 'a'));
        } else {
            lowered.push_back(ch);
        }
    }
    return lowered;
}

/// argv[0] 是否命中列表中的某一项（原文或 basename 任一相等即命中）。
bool MatchesCommand(std::string_view argv0, const std::vector<std::string>& list) {
    const std::string_view base = BaseName(argv0);
    for (const std::string& entry : list) {
        if (entry.empty()) {
            continue;
        }
        const std::string_view candidate(entry);
        if (candidate == argv0 || candidate == base) {
            return true;
        }
    }
    return false;
}

/// 回调异常隔离：回调是扩展点，不允许它打断子进程回收。
template <typename Fn, typename... Args>
void InvokeSafely(const Fn& fn, Args&&... args) noexcept {
    if (!fn) {
        return;
    }
    try {
        fn(std::forward<Args>(args)...);
    } catch (...) {
        // 扩展点异常一律吞掉：回收子进程与返回结果优先。
    }
}

// ---------------------------------------------------------------------------
// 长度计算
// ---------------------------------------------------------------------------

/// 计算请求帧总长度；非法时返回 0。
std::size_t CommandFrameSize(const Command& command) noexcept {
    if (command.argv.empty() || command.argv.size() > kCommandMaxArgv ||
        command.id.size() > kCommandMaxIdBytes || command.cwd.size() > kCommandMaxCwdBytes) {
        return 0;
    }
    if (HasNul(command.id) || HasNul(command.cwd)) {
        return 0;
    }
    std::size_t total =
        kCommandFrameHeaderBytes + 4 * command.argv.size() + command.id.size() + command.cwd.size();
    for (const std::string& arg : command.argv) {
        if (arg.size() > kCommandMaxArgBytes || HasNul(arg)) {
            return 0;
        }
        total += arg.size();
    }
    if (total > kCommandMaxFrameBytes) {
        return 0;
    }
    return total;
}

// ---------------------------------------------------------------------------
// 输出泵：把一路管道读到上限内，并触发回调
// ---------------------------------------------------------------------------

/// 一次输出泵的读缓冲大小（16KiB）。原来每次在栈上开 64KiB，两个泵
/// 叠加加上 Execute 的局部量在小栈 Fiber（常见 64KiB）上会顶到栈顶；
/// 16KiB 单块缓冲既能一次排空 64KiB 管道，又留出足够栈余量。
inline constexpr std::size_t kCommandPumpBufferBytes = 16 * 1024;

/// 一路输出管道的读取状态。
struct StreamPump final {
    int fd{-1};                          ///< 读端 fd；-1 表示无。
    std::string* sink{nullptr};          ///< 输出缓冲。
    const std::function<void(std::string_view)>* callback{nullptr};  ///< 可选回调。
    std::size_t limit{0};                ///< 缓冲上限（字节）。
    bool truncated{false};               ///< 是否发生截断。
    bool eof{false};                     ///< 是否读到 EOF/错误。
    std::uint64_t dropped_bytes{0};      ///< 被丢弃的字节数。
};

/// 按上限追加；超出部分计入 dropped_bytes。
void AppendLimited(StreamPump* pump, std::string_view data) {
    const std::size_t room =
        pump->sink->size() < pump->limit ? pump->limit - pump->sink->size() : 0;
    const std::size_t take = std::min(room, data.size());
    if (take > 0) {
        pump->sink->append(data.data(), take);
    }
    if (take < data.size()) {
        pump->truncated = true;
        pump->dropped_bytes += static_cast<std::uint64_t>(data.size() - take);
    }
}

/// 尽量把该管道读空（最多 16 次 read / 256KiB，避免单路饿死另一路）。
void PumpOnce(StreamPump* pump, char* buffer, std::size_t buffer_size) {
    if (pump == nullptr || pump->fd < 0 || pump->eof || buffer == nullptr ||
        buffer_size == 0) {
        return;
    }
    for (int round = 0; round < 16; ++round) {
        const ssize_t count = ::read(pump->fd, buffer, buffer_size);
        if (count > 0) {
            const std::string_view view(buffer, static_cast<std::size_t>(count));
            AppendLimited(pump, view);
            if (pump->callback != nullptr) {
                InvokeSafely(*pump->callback, view);
            }
            continue;
        }
        if (count == 0) {
            pump->eof = true;
            return;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        }
        // 其他读错误按 EOF 处理：继续等进程退出，由退出状态给结论。
        pump->eof = true;
        return;
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// 状态文本
// ---------------------------------------------------------------------------

const char* CommandStatusToString(CommandStatus status) noexcept {
    switch (status) {
        case CommandStatus::kOk:
            return "ok";
        case CommandStatus::kInvalidArgument:
            return "invalid-argument";
        case CommandStatus::kDenied:
            return "denied";
        case CommandStatus::kSpawnFailed:
            return "spawn-failed";
        case CommandStatus::kTimeout:
            return "timeout";
        case CommandStatus::kAborted:
            return "aborted";
        case CommandStatus::kNonZeroExit:
            return "non-zero-exit";
        case CommandStatus::kInternal:
            return "internal";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// 内置危险命令表
// ---------------------------------------------------------------------------

const std::vector<std::string>& BuiltinDangerousCommands() {
    // 只列「一旦被远程调用就等价于交出一个 shell / 破坏系统 / 打通网络」的
    // 程序。列表是安全下限，不是完整清单；生产部署必须依赖白名单而不是它。
    static const std::vector<std::string> kDangerous = {
        // 解释器/shell：参数就是任意代码。
        "sh",       "bash",     "dash",     "zsh",      "ksh",      "csh",
        "tcsh",     "fish",     "busybox",  "env",      "xargs",    "nohup",
        "setsid",   "timeout",  "chroot",   "unshare",  "nsenter",  "script",
        "python",   "python3",  "perl",     "ruby",     "node",     "php",
        // 破坏性文件操作。
        "rm",       "rmdir",    "unlink",   "shred",    "truncate", "dd",
        "mkfs",     "fdisk",    "parted",   "wipefs",   "fallocate",
        // 权限/属主/挂载。
        "mount",    "umount",   "chmod",    "chown",    "chgrp",    "chattr",
        // 进程/系统控制。
        "kill",     "pkill",    "killall",  "renice",   "shutdown", "reboot",
        "halt",     "poweroff", "systemctl","service",  "systemd-run",
        "crontab",  "at",       "batch",    "iptables", "ip6tables","nft",
        "ip",       "modprobe", "insmod",   "rmmod",    "sysctl",   "docker",
        "podman",   "sudo",     "su",       "doas",     "pkexec",   "git",
        "make",     "cmake",    "ninja",    "tee",
        // 网络出口。
        "nc",       "ncat",     "netcat",   "socat",    "telnet",   "ssh",
        "scp",      "sftp",     "curl",     "wget",
    };
    return kDangerous;
}

// ---------------------------------------------------------------------------
// 策略校验
// ---------------------------------------------------------------------------

CommandStatus ValidateCommand(const Command& command, const CommandPolicy& policy,
                              std::string* reason) {
    const auto fail = [reason](CommandStatus status, const char* why) {
        if (reason != nullptr) {
            *reason = why;
        }
        return status;
    };

    if (policy.max_output_bytes == 0 || policy.max_output_bytes > kCommandMaxOutputCeiling) {
        return fail(CommandStatus::kInvalidArgument, "策略 max_output_bytes 越界");
    }
    if (policy.max_timeout_ms > kCommandMaxTimeoutMs) {
        return fail(CommandStatus::kInvalidArgument, "策略 max_timeout_ms 超过硬上限");
    }
    if (command.id.size() > kCommandMaxIdBytes) {
        return fail(CommandStatus::kInvalidArgument, "id 超长");
    }
    if (HasNul(command.id)) {
        return fail(CommandStatus::kInvalidArgument, "id 含 NUL");
    }
    if (command.argv.empty()) {
        return fail(CommandStatus::kInvalidArgument, "argv 为空");
    }
    if (command.argv.size() > kCommandMaxArgv) {
        return fail(CommandStatus::kInvalidArgument, "argv 项数超限");
    }
    for (const std::string& arg : command.argv) {
        if (arg.empty()) {
            return fail(CommandStatus::kInvalidArgument, "argv 含空项");
        }
        if (arg.size() > kCommandMaxArgBytes) {
            return fail(CommandStatus::kInvalidArgument, "argv 单项超长");
        }
        if (HasNul(arg)) {
            return fail(CommandStatus::kInvalidArgument, "argv 含 NUL");
        }
    }
    if (command.timeout_ms > policy.max_timeout_ms) {
        return fail(CommandStatus::kInvalidArgument, "timeout 超过策略上限");
    }
    if (command.timeout_ms == 0 && !policy.allow_unlimited_timeout) {
        // 0 表示不限时；若默认放行，远端填 0 就能绕过 max_timeout_ms。
        return fail(CommandStatus::kInvalidArgument,
                    "timeout=0 需要策略显式打开 allow_unlimited_timeout");
    }
    if (CommandFrameSize(command) == 0) {
        return fail(CommandStatus::kInvalidArgument, "请求帧超出长度上限");
    }
    if (command.cwd.size() > kCommandMaxCwdBytes) {
        return fail(CommandStatus::kInvalidArgument, "cwd 超长");
    }
    if (HasNul(command.cwd)) {
        return fail(CommandStatus::kInvalidArgument, "cwd 含 NUL");
    }
    if (!command.cwd.empty() && command.cwd.front() != '/') {
        return fail(CommandStatus::kInvalidArgument, "cwd 必须是绝对路径");
    }

    // cwd 沙箱：先规范化再比较，避免 `a/../../etc` 绕过前缀检查。
    if (!policy.allowed_cwd_roots.empty()) {
        if (command.cwd.empty()) {
            // 空 cwd 会让子进程继承服务进程的 cwd，等于绕过沙箱。
            return fail(CommandStatus::kDenied, "启用 cwd 沙箱时 cwd 不能为空");
        }
        std::error_code ec;
        std::filesystem::path cwd_path = std::filesystem::weakly_canonical(command.cwd, ec);
        if (ec) {
            cwd_path = std::filesystem::path(command.cwd).lexically_normal();
        }
        bool inside = false;
        for (const std::string& root : policy.allowed_cwd_roots) {
            if (root.empty() || root.front() != '/') {
                continue;
            }
            std::error_code root_ec;
            std::filesystem::path root_path = std::filesystem::weakly_canonical(root, root_ec);
            if (root_ec) {
                root_path = std::filesystem::path(root).lexically_normal();
            }
            const std::filesystem::path relative = cwd_path.lexically_relative(root_path);
            if (relative.empty()) {
                continue;
            }
            const auto first = relative.begin();
            if (first == relative.end()) {
                continue;
            }
            if (first->string() != "..") {
                inside = true;
                break;
            }
        }
        if (!inside) {
            return fail(CommandStatus::kDenied, "cwd 不在允许的根目录下");
        }
    }

    const std::string& executable = command.argv[0];
    // 自定义拒绝表优先级最高：即使 allow_all / allow_dangerous 也不能绕过。
    if (MatchesCommand(executable, policy.denied_commands)) {
        return fail(CommandStatus::kDenied, "命令在自定义拒绝表中");
    }
    if (policy.use_builtin_deny && !policy.allow_dangerous) {
        const std::string base = ToLowerAscii(BaseName(executable));
        const std::vector<std::string>& dangerous = BuiltinDangerousCommands();
        if (std::find(dangerous.begin(), dangerous.end(), base) != dangerous.end()) {
            return fail(CommandStatus::kDenied, "命令属于内置危险命令表");
        }
    }
    if (!policy.allow_all) {
        if (policy.allowed_commands.empty()) {
            return fail(CommandStatus::kDenied, "白名单为空（默认拒绝）");
        }
        if (!MatchesCommand(executable, policy.allowed_commands)) {
            return fail(CommandStatus::kDenied, "命令不在白名单中");
        }
    }
    return CommandStatus::kOk;
}

// ---------------------------------------------------------------------------
// 请求帧编解码
// ---------------------------------------------------------------------------

std::vector<std::uint8_t> EncodeCommand(const Command& command) {
    const std::size_t total = CommandFrameSize(command);
    if (total == 0) {
        return {};
    }
    std::vector<std::uint8_t> frame(total, 0);
    frame[0] = kCommandFrameMagic0;
    frame[1] = kCommandFrameMagic1;
    frame[2] = kCommandProtocolVersion;
    frame[3] = kCommandFrameTypeRequest;
    WriteU16(frame.data() + 4, static_cast<std::uint16_t>(command.id.size()));
    WriteU16(frame.data() + 6, static_cast<std::uint16_t>(command.argv.size()));
    WriteU32(frame.data() + 8, command.timeout_ms);
    WriteU16(frame.data() + 12, static_cast<std::uint16_t>(command.cwd.size()));
    WriteU16(frame.data() + 14, 0);
    std::size_t offset = kCommandFrameHeaderBytes;
    for (const std::string& arg : command.argv) {
        WriteU32(frame.data() + offset, static_cast<std::uint32_t>(arg.size()));
        offset += 4;
    }
    std::memcpy(frame.data() + offset, command.id.data(), command.id.size());
    offset += command.id.size();
    std::memcpy(frame.data() + offset, command.cwd.data(), command.cwd.size());
    offset += command.cwd.size();
    for (const std::string& arg : command.argv) {
        std::memcpy(frame.data() + offset, arg.data(), arg.size());
        offset += arg.size();
    }
    return frame;
}

bool DecodeCommand(const std::uint8_t* data, std::size_t size, Command* out,
                   std::string* error) {
    const auto fail = [error](const char* why) {
        if (error != nullptr) {
            *error = why;
        }
        return false;
    };
    if (data == nullptr || out == nullptr) {
        return fail("data/out 为空");
    }
    if (size < kCommandFrameHeaderBytes) {
        return fail("帧短于固定头");
    }
    if (size > kCommandMaxFrameBytes) {
        // 字段各自合法也可能拼出 >64KiB 的帧；硬上限必须在解析前拒绝。
        return fail("帧超过请求帧硬上限");
    }
    if (data[0] != kCommandFrameMagic0 || data[1] != kCommandFrameMagic1) {
        return fail("magic 不匹配");
    }
    if (data[2] != kCommandProtocolVersion) {
        return fail("版本不支持");
    }
    if (data[3] != kCommandFrameTypeRequest) {
        return fail("帧类型不是请求");
    }
    const std::uint16_t id_len = ReadU16(data + 4);
    const std::uint16_t argc = ReadU16(data + 6);
    const std::uint32_t timeout_ms = ReadU32(data + 8);
    const std::uint16_t cwd_len = ReadU16(data + 12);
    const std::uint16_t reserved = ReadU16(data + 14);
    if (reserved != 0) {
        return fail("保留字段非 0");
    }
    if (static_cast<std::size_t>(id_len) > kCommandMaxIdBytes) {
        return fail("id 超长");
    }
    if (argc == 0 || static_cast<std::size_t>(argc) > kCommandMaxArgv) {
        return fail("argc 越界");
    }
    if (static_cast<std::size_t>(cwd_len) > kCommandMaxCwdBytes) {
        return fail("cwd 超长");
    }
    const std::size_t lengths_offset =
        kCommandFrameHeaderBytes + 4 * static_cast<std::size_t>(argc);
    if (size < lengths_offset) {
        return fail("argv 长度表越界");
    }
    std::size_t payload = static_cast<std::size_t>(id_len) + static_cast<std::size_t>(cwd_len);
    std::vector<std::uint32_t> lengths(argc, 0);
    for (std::size_t i = 0; i < argc; ++i) {
        lengths[i] = ReadU32(data + kCommandFrameHeaderBytes + 4 * i);
        if (static_cast<std::size_t>(lengths[i]) > kCommandMaxArgBytes) {
            return fail("argv 单项超长");
        }
        payload += lengths[i];
    }
    if (size != lengths_offset + payload) {
        return fail("帧总长与字段不符");
    }
    Command decoded;
    decoded.timeout_ms = timeout_ms;
    std::size_t offset = lengths_offset;
    decoded.id.assign(reinterpret_cast<const char*>(data + offset), id_len);
    offset += id_len;
    decoded.cwd.assign(reinterpret_cast<const char*>(data + offset), cwd_len);
    offset += cwd_len;
    decoded.argv.reserve(argc);
    for (std::size_t i = 0; i < argc; ++i) {
        const std::size_t length = lengths[i];
        decoded.argv.emplace_back(reinterpret_cast<const char*>(data + offset), length);
        offset += length;
    }
    // 编码器拒绝 NUL（C 边界会截断），解码器必须同样拒绝，否则会出现
    // 「解得出来但再也编不回去」的非规范帧。输出流允许 NUL，id/cwd/argv 不允许。
    if (HasNul(decoded.id) || HasNul(decoded.cwd)) {
        return fail("id/cwd 含 NUL");
    }
    for (const std::string& arg : decoded.argv) {
        if (HasNul(arg)) {
            return fail("argv 含 NUL");
        }
    }
    // 解码成功后才写 out：失败路径不改动调用方的对象。
    *out = std::move(decoded);
    return true;
}

// ---------------------------------------------------------------------------
// 结果帧编解码
// ---------------------------------------------------------------------------

std::vector<std::uint8_t> EncodeCommandResult(const CommandResult& result) {
    if (result.id.size() > kCommandMaxIdBytes ||
        result.message.size() > kCommandMaxMessageBytes) {
        return {};
    }
    // status 是枚举，越界值编码后会被自己的解码器拒绝，破坏往返不变式。
    if (static_cast<std::uint8_t>(result.status) >
        static_cast<std::uint8_t>(CommandStatus::kInternal)) {
        return {};
    }
    // 单路输出硬上限；否则 16MiB 总上限内可以塞进 8MiB 的 stdout。
    if (result.standard_output.size() > kCommandMaxOutputCeiling ||
        result.standard_error.size() > kCommandMaxOutputCeiling) {
        return {};
    }
    if (HasNul(result.id) || HasNul(result.message)) {
        return {};
    }
    const std::size_t total = kCommandResultFrameHeaderBytes + result.id.size() +
                              result.standard_output.size() + result.standard_error.size() +
                              result.message.size();
    if (total > kCommandResultMaxFrameBytes) {
        return {};
    }
    std::vector<std::uint8_t> frame(total, 0);
    frame[0] = kCommandFrameMagic0;
    frame[1] = kCommandFrameMagic1;
    frame[2] = kCommandProtocolVersion;
    frame[3] = kCommandFrameTypeResult;
    frame[4] = static_cast<std::uint8_t>(result.status);
    std::uint8_t flags = 0;
    if (result.timed_out) {
        flags |= 0x01;
    }
    if (result.output_truncated) {
        flags |= 0x02;
    }
    frame[5] = flags;
    WriteU16(frame.data() + 6, static_cast<std::uint16_t>(result.id.size()));
    WriteU32(frame.data() + 8, static_cast<std::uint32_t>(result.exit_code));
    WriteU32(frame.data() + 12, static_cast<std::uint32_t>(result.standard_output.size()));
    WriteU32(frame.data() + 16, static_cast<std::uint32_t>(result.standard_error.size()));
    WriteU16(frame.data() + 20, static_cast<std::uint16_t>(result.message.size()));
    WriteU16(frame.data() + 22, 0);
    WriteU64(frame.data() + 24, result.duration_ms);
    std::size_t offset = kCommandResultFrameHeaderBytes;
    std::memcpy(frame.data() + offset, result.id.data(), result.id.size());
    offset += result.id.size();
    std::memcpy(frame.data() + offset, result.standard_output.data(),
                result.standard_output.size());
    offset += result.standard_output.size();
    std::memcpy(frame.data() + offset, result.standard_error.data(),
                result.standard_error.size());
    offset += result.standard_error.size();
    std::memcpy(frame.data() + offset, result.message.data(), result.message.size());
    return frame;
}

bool DecodeCommandResult(const std::uint8_t* data, std::size_t size, CommandResult* out,
                         std::string* error) {
    const auto fail = [error](const char* why) {
        if (error != nullptr) {
            *error = why;
        }
        return false;
    };
    if (data == nullptr || out == nullptr) {
        return fail("data/out 为空");
    }
    if (size < kCommandResultFrameHeaderBytes) {
        return fail("帧短于固定头");
    }
    if (size > kCommandResultMaxFrameBytes) {
        // 硬上限先于任何字段解析：避免按对端声明的大小分配内存。
        return fail("帧超过结果帧硬上限");
    }
    if (data[0] != kCommandFrameMagic0 || data[1] != kCommandFrameMagic1) {
        return fail("magic 不匹配");
    }
    if (data[2] != kCommandProtocolVersion) {
        return fail("版本不支持");
    }
    if (data[3] != kCommandFrameTypeResult) {
        return fail("帧类型不是结果");
    }
    const std::uint8_t status = data[4];
    const std::uint8_t flags = data[5];
    const std::uint16_t id_len = ReadU16(data + 6);
    const std::uint32_t exit_code = ReadU32(data + 8);
    const std::uint32_t out_len = ReadU32(data + 12);
    const std::uint32_t err_len = ReadU32(data + 16);
    const std::uint16_t msg_len = ReadU16(data + 20);
    const std::uint16_t reserved = ReadU16(data + 22);
    const std::uint64_t duration_ms = ReadU64(data + 24);
    if (reserved != 0) {
        return fail("保留字段非 0");
    }
    if ((flags & ~0x03) != 0) {
        return fail("flags 含未知位");
    }
    if (status > static_cast<std::uint8_t>(CommandStatus::kInternal)) {
        return fail("status 越界");
    }
    if (static_cast<std::size_t>(id_len) > kCommandMaxIdBytes) {
        return fail("id 超长");
    }
    if (static_cast<std::size_t>(msg_len) > kCommandMaxMessageBytes) {
        return fail("message 超长");
    }
    if (static_cast<std::size_t>(out_len) > kCommandMaxOutputCeiling ||
        static_cast<std::size_t>(err_len) > kCommandMaxOutputCeiling) {
        return fail("单路输出超过硬上限");
    }
    const std::size_t payload = static_cast<std::size_t>(id_len) + out_len + err_len + msg_len;
    if (size != kCommandResultFrameHeaderBytes + payload) {
        return fail("帧总长与字段不符");
    }
    CommandResult decoded;
    decoded.status = static_cast<CommandStatus>(status);
    decoded.exit_code = static_cast<int>(exit_code);
    decoded.timed_out = (flags & 0x01) != 0;
    decoded.output_truncated = (flags & 0x02) != 0;
    decoded.duration_ms = duration_ms;
    std::size_t offset = kCommandResultFrameHeaderBytes;
    decoded.id.assign(reinterpret_cast<const char*>(data + offset), id_len);
    offset += id_len;
    decoded.standard_output.assign(reinterpret_cast<const char*>(data + offset), out_len);
    offset += out_len;
    decoded.standard_error.assign(reinterpret_cast<const char*>(data + offset), err_len);
    offset += err_len;
    decoded.message.assign(reinterpret_cast<const char*>(data + offset), msg_len);
    // 与编码器对称：id/message 含 NUL 的帧不可重编码。两路输出是任意字节，不查。
    if (HasNul(decoded.id) || HasNul(decoded.message)) {
        return fail("id/message 含 NUL");
    }
    *out = std::move(decoded);
    return true;
}

// ---------------------------------------------------------------------------
// 执行器
// ---------------------------------------------------------------------------

CommandExecutor::CommandExecutor(CommandPolicy policy) : m_policy(std::move(policy)) {}

CommandResult CommandExecutor::Execute(const Command& command) const {
    return Execute(command, CommandCallbacks{});
}

CommandResult CommandExecutor::Execute(const Command& command,
                                       const CommandCallbacks& callbacks) const {
    CommandResult result;
    result.id = command.id;

    std::string reason;
    const CommandStatus validation = ValidateCommand(command, m_policy, &reason);
    if (validation != CommandStatus::kOk) {
        result.status = validation;
        result.message = reason;
        InvokeSafely(callbacks.on_exit, result);
        return result;
    }

    go2cpp::os::ProcessOptions options;
    options.argv = command.argv;
    options.cwd = command.cwd;
    options.use_path_search = m_policy.use_path_search;
    // 新会话 = 子进程成为进程组组长，KillTree 才有意义（否则 os::proc 会
    // 拒绝向调用方所在进程组发信号，这正是我们要的防误杀语义）。
    options.new_session = true;
    options.stdin_mode = go2cpp::os::StdioMode::kNull;
    options.stdout_mode = go2cpp::os::StdioMode::kPipe;
    options.stderr_mode = go2cpp::os::StdioMode::kPipe;
    options.close_extra_fds = true;
    options.timeout = std::chrono::milliseconds(0);  // 由本模块自己控制超时阶梯。
    options.grace = std::chrono::milliseconds(m_policy.kill_grace_ms);

    go2cpp::os::Process process;
    const auto spawn_result = go2cpp::os::Process::Spawn(options, &process);
    if (!spawn_result.ok()) {
        result.status = CommandStatus::kSpawnFailed;
        result.message = spawn_result.error().ToString();
        InvokeSafely(callbacks.on_exit, result);
        return result;
    }

    InvokeSafely(callbacks.on_spawn, process.pid());

    StreamPump stdout_pump;
    stdout_pump.fd = process.native_fd(go2cpp::os::StdioStream::kStdout);
    stdout_pump.sink = &result.standard_output;
    stdout_pump.callback = &callbacks.on_stdout;
    stdout_pump.limit = m_policy.max_output_bytes;

    StreamPump stderr_pump;
    stderr_pump.fd = process.native_fd(go2cpp::os::StdioStream::kStderr);
    stderr_pump.sink = &result.standard_error;
    stderr_pump.callback = &callbacks.on_stderr;
    stderr_pump.limit = m_policy.max_output_bytes;

    const auto start = std::chrono::steady_clock::now();
    go2cpp::os::ExitStatus exit_status;
    bool reaped = false;
    bool wait_failed = false;
    bool timed_out = false;
    bool term_sent = false;
    bool kill_sent = false;
    std::chrono::steady_clock::time_point term_time = start;
    go2cpp::os::ProcError wait_error;
    std::array<char, kCommandPumpBufferBytes> read_buffer;

    // 终止整组；进程组还没建立/已退出时退回单进程 Kill，避免把错误吞掉。
    const auto terminate_tree = [&process](int signal) {
        const auto tree = process.KillTree(signal);
        if (!tree.ok() && process.Alive()) {
            (void)process.Kill(signal);
        }
    };

    while (true) {
        PumpOnce(&stdout_pump, read_buffer.data(), read_buffer.size());
        PumpOnce(&stderr_pump, read_buffer.data(), read_buffer.size());
        const bool both_eof = stdout_pump.eof && stderr_pump.eof;
        if (!reaped) {
            if (both_eof) {
                // 两路都 EOF 后不会再有输出；用 Process::Wait（pidfd）等退出，
                // 不再空转 5ms 轮询——否则每条有输出的短命令都白白多等一个 tick。
                auto slice = std::chrono::milliseconds(kCommandPollIntervalMs);
                if (command.timeout_ms > 0 && !term_sent) {
                    const auto used = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start);
                    const auto remaining =
                        static_cast<std::int64_t>(command.timeout_ms) - used.count();
                    slice = std::chrono::milliseconds(std::max<std::int64_t>(remaining, 0));
                    slice = std::min(slice,
                                     std::chrono::milliseconds(kCommandPollIntervalMs));
                }
                auto waited = process.Wait(slice);
                if (waited.ok()) {
                    exit_status = waited.value();
                    reaped = true;
                } else if (waited.error().code != go2cpp::os::ProcErrorCode::kTimeout) {
                    wait_error = waited.error();
                    wait_failed = true;
                }
            } else {
                reaped = process.TryWait(&exit_status, &wait_error);
                if (!reaped && wait_error.code != go2cpp::os::ProcErrorCode::kOk &&
                    wait_error.code != go2cpp::os::ProcErrorCode::kTimeout) {
                    wait_failed = true;
                }
            }
        }
        if (reaped || wait_failed) {
            break;
        }
        const auto now = std::chrono::steady_clock::now();
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start);
        if (command.timeout_ms > 0 &&
            elapsed.count() >= static_cast<std::int64_t>(command.timeout_ms)) {
            if (!term_sent) {
                timed_out = true;
                term_sent = true;
                term_time = now;
                terminate_tree(SIGTERM);
            } else if (!kill_sent &&
                       std::chrono::duration_cast<std::chrono::milliseconds>(now - term_time)
                               .count() >= static_cast<std::int64_t>(m_policy.kill_grace_ms)) {
                kill_sent = true;
                terminate_tree(SIGKILL);
            }
        }
        if (both_eof) {
            continue;  // Wait 已经等过一片，不要再 poll 已 EOF 的 fd。
        }
        // 等到数据可读或 5ms 到期。
        pollfd fds[2] = {};
        nfds_t count = 0;
        if (!stdout_pump.eof && stdout_pump.fd >= 0) {
            fds[count].fd = stdout_pump.fd;
            fds[count].events = POLLIN;
            ++count;
        }
        if (!stderr_pump.eof && stderr_pump.fd >= 0) {
            fds[count].fd = stderr_pump.fd;
            fds[count].events = POLLIN;
            ++count;
        }
        (void)::poll(fds, count, static_cast<int>(kCommandPollIntervalMs));
    }

    // 进程已回收，但后台孙进程可能仍持有写端：补读几轮后主动关闭父侧管道，
    // 避免在「sh 启动后台进程并退出」这类场景里永远等不到 EOF。
    // 8 轮 × 每轮最多 256KB/路，足以覆盖两路输出上限（各 4MB）内的残余数据。
    for (int round = 0; round < 8; ++round) {
        PumpOnce(&stdout_pump, read_buffer.data(), read_buffer.size());
        PumpOnce(&stderr_pump, read_buffer.data(), read_buffer.size());
        if (stdout_pump.eof && stderr_pump.eof) {
            break;
        }
        pollfd fds[2] = {};
        nfds_t count = 0;
        if (!stdout_pump.eof && stdout_pump.fd >= 0) {
            fds[count].fd = stdout_pump.fd;
            fds[count].events = POLLIN;
            ++count;
        }
        if (!stderr_pump.eof && stderr_pump.fd >= 0) {
            fds[count].fd = stderr_pump.fd;
            fds[count].events = POLLIN;
            ++count;
        }
        (void)::poll(fds, count, 1);
    }
    process.CloseChannels();

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    result.duration_ms = elapsed.count() > 0 ? static_cast<std::uint64_t>(elapsed.count()) : 0;
    result.timed_out = timed_out;
    result.output_truncated = stdout_pump.truncated || stderr_pump.truncated;
    result.exit_code = exit_status.exited ? exit_status.exit_code : -1;

    if (wait_failed) {
        result.status = CommandStatus::kInternal;
        result.message = "等待子进程失败: " + wait_error.ToString();
    } else if (timed_out) {
        result.status = CommandStatus::kTimeout;
        result.message = kill_sent ? "命令超时，已 SIGKILL 整个进程组"
                                   : "命令超时，已 SIGTERM 整个进程组";
    } else if (exit_status.signaled) {
        result.status = CommandStatus::kAborted;
        result.message = std::string("命令被信号终止（") +
                         go2cpp::os::SignalName(exit_status.signal) + "）";
    } else if (!exit_status.exited) {
        result.status = CommandStatus::kInternal;
        result.message = "wait 未返回有效退出状态";
    } else if (exit_status.exit_code == 0) {
        result.status = CommandStatus::kOk;
    } else {
        result.status = CommandStatus::kNonZeroExit;
        result.message = "命令以非 0 退出码结束";
    }

    InvokeSafely(callbacks.on_exit, result);
    return result;
}

}  // namespace go2cpp::bento
