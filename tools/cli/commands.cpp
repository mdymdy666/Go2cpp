/**
 * @file commands.cpp
 * @brief 五个子命令的实现。
 *
 * @details 对接面（2026-10-20 已按交付物对齐）：
 *
 * - `dump` / `status`：DebugServer `GO2CPP-DEBUG/1` 文本协议
 *   （`include/go2cpp/debug/debug_server.hpp`）。dump 用 `JSON ALL` / `DUMP ALL`，
 *   指定 sections 时改为逐组件 `JSON <component>`；status 用 `STATUS` +
 *   `JSON scheduler`。
 * - `migrate dump` / `migrate restore`：外部可执行 `migrate`
 *   （`tools/migration`，产物名 `migrate`），子命令 `dump|restore <file>`。
 * - `replay play`：外部可执行 `replay`（`tools/replay`，产物名 `replay`），
 *   传 `play <文件>` 子命令，stdout 输出 JSON 报告；CLI 解析
 *   `replay_executed`，未真正重放时以退出码 3 拒绝。
 * - `replay record`：设置 `GO2CPP_REPLAY=ON` 与 `GO2CPP_REPLAY_RECORD` 后
 *   fork/exec；当前重放后端只提供 demo 记录器，通用注入待对齐。
 *
 * ## 边界与陷阱
 *
 * - DebugServer 一个连接只处理一条命令，`DebugClient::Call` 每次新建连接；
 *   status 需要两次调用（STATUS + JSON scheduler）。
 * - `replay record` 拒绝覆盖已存在的 `--out`（不再启动前删除）：否则用户误指
 *   一个已有文件（甚至是被记录程序本身）会被不可恢复地删除。
 * - 子进程退出码原样返回；被信号杀死返回 128+signo。
 * - 后端 stdout 在 `replay play` 中被捕获用于解析 JSON 报告；stderr 仍继承
 *   终端 —— 错误信息不该藏进缓冲区。
 */

#include "commands.hpp"

#include "args.hpp"
#include "cli.hpp"
#include "formats.hpp"
#include "help.hpp"
#include "transport.hpp"

#include "nlohmann/json.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace go2cpp::cli {
namespace {

constexpr int kDefaultTimeoutMs = 5000;
constexpr int kMaxTimeoutMs = 600000;
constexpr std::uint64_t kMaxFileBytes = 64ULL * 1024ULL * 1024ULL;
/// 后端 stdout 捕获上限：重放报告 JSON 默认不含 timeline，16MiB 足够。
constexpr std::uint64_t kMaxCaptureBytes = 16ULL * 1024ULL * 1024ULL;

std::string EnvNonEmpty(const char* name) {
    const char* value = std::getenv(name);
    return (value != nullptr && *value != '\0') ? std::string(value) : std::string();
}

std::string AbsolutePath(const std::string& path) {
    std::error_code error;
    const auto absolute = std::filesystem::absolute(path, error);
    if (error) {
        throw CliError(ExitCode::kFailure,
                       "无法把路径解析为绝对路径：'" + path + "'：" + error.message());
    }
    return absolute.lexically_normal().string();
}

bool IsExecutableFile(const std::string& path) {
    struct stat info {};
    if (::stat(path.c_str(), &info) != 0) {
        return false;
    }
    return S_ISREG(info.st_mode) && ::access(path.c_str(), X_OK) == 0;
}

std::string FindInPath(const std::string& name) {
    if (name.empty()) {
        return {};
    }
    if (name.find('/') != std::string::npos) {
        return IsExecutableFile(name) ? AbsolutePath(name) : std::string();
    }
    const std::string path_list = EnvNonEmpty("PATH");
    if (path_list.empty()) {
        return {};
    }
    std::size_t begin = 0;
    while (begin <= path_list.size()) {
        const auto end = path_list.find(':', begin);
        std::string directory = path_list.substr(
            begin, end == std::string::npos ? std::string::npos : end - begin);
        if (directory.empty()) {
            directory = ".";
        }
        const std::string candidate = directory + "/" + name;
        if (IsExecutableFile(candidate)) {
            return AbsolutePath(candidate);
        }
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1;
    }
    return {};
}

/// go2cpp-cli 自身所在目录；读不到 /proc/self/exe 时返回空（非 Linux 平台）。
std::string SelfDirectory() {
    char buffer[4096];
    const ssize_t length = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (length <= 0) {
        return {};
    }
    buffer[length] = '\0';
    return std::filesystem::path(buffer).parent_path().string();
}

/**
 * @brief 查找外部后端可执行文件。
 * @param names 候选文件名（第一个是推荐名）。
 * @param env_name 显式覆盖的环境变量名。
 * @details 优先级：环境变量 > 与 CLI 同目录 > PATH。显式指定但不可执行时立即
 *          报错，不静默回退 —— 否则会跑错版本。
 */
std::string FindBackend(const std::vector<const char*>& names, const char* env_name) {
    const std::string explicit_path = EnvNonEmpty(env_name);
    if (!explicit_path.empty()) {
        if (!IsExecutableFile(explicit_path)) {
            throw CliError(ExitCode::kUnavailable,
                           std::string(env_name) + " 指向的后端不存在或不可执行：'" +
                               explicit_path + "'");
        }
        return AbsolutePath(explicit_path);
    }
    const std::string sibling = SelfDirectory();
    if (!sibling.empty()) {
        for (const char* name : names) {
            const std::string candidate = sibling + "/" + name;
            if (IsExecutableFile(candidate)) {
                return candidate;
            }
        }
    }
    for (const char* name : names) {
        const std::string found = FindInPath(name);
        if (!found.empty()) {
            return found;
        }
    }
    return {};
}

std::string FindExecutable(const std::string& name) {
    if (name.find('/') != std::string::npos) {
        if (!IsExecutableFile(name)) {
            throw CliError(ExitCode::kFailure,
                           "找不到可执行文件或不可执行：'" + name + "'（检查路径与 +x 权限）");
        }
        return AbsolutePath(name);
    }
    const std::string found = FindInPath(name);
    if (found.empty()) {
        throw CliError(ExitCode::kFailure, "在 PATH 中找不到可执行文件 '" + name + "'");
    }
    return found;
}

std::string RenderJson(const nlohmann::json& value) {
    return value.dump(2) + "\n";
}

/// 把内容写到 `out`；`out` 为空或 "-" 表示 stdout。
void WriteOutput(const std::string& out, const std::string& content) {
    if (out.empty() || out == "-") {
        std::fwrite(content.data(), 1, content.size(), stdout);
        if (!content.empty() && content.back() != '\n') {
            std::fputc('\n', stdout);
        }
        std::fflush(stdout);
        return;
    }
    WriteTextFileAtomic(out, content);
    std::printf("已写入 %s（%s）\n", out.c_str(), FormatBytes(content.size()).c_str());
    std::fflush(stdout);
}

const nlohmann::json* Dig(const nlohmann::json& root,
                          std::initializer_list<const char*> path) {
    const nlohmann::json* node = &root;
    for (const char* key : path) {
        if (!node->is_object()) {
            return nullptr;
        }
        const auto it = node->find(key);
        if (it == node->end()) {
            return nullptr;
        }
        node = &(*it);
    }
    return node;
}

std::optional<double> FindNumber(const nlohmann::json& root,
                                 std::initializer_list<const char*> path) {
    const nlohmann::json* node = Dig(root, path);
    if (node != nullptr && node->is_number()) {
        return node->get<double>();
    }
    return std::nullopt;
}

std::string FormatCount(double value) {
    if (std::isfinite(value) && value == std::floor(value) && std::fabs(value) < 1e15) {
        return std::to_string(static_cast<long long>(value));
    }
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.1f", value);
    return buffer;
}

/// 解析 DebugServer 的 text payload（每行 `key=value`）。
std::map<std::string, std::string> ParseKeyValueText(const std::string& text) {
    std::map<std::string, std::string> values;
    std::size_t begin = 0;
    while (begin <= text.size()) {
        const auto newline = text.find('\n', begin);
        const std::string line = Trim(text.substr(
            begin, newline == std::string::npos ? std::string::npos : newline - begin));
        if (!line.empty()) {
            const auto equal = line.find('=');
            if (equal != std::string::npos) {
                values[line.substr(0, equal)] = line.substr(equal + 1);
            }
        }
        if (newline == std::string::npos) {
            break;
        }
        begin = newline + 1;
    }
    return values;
}

/**
 * @brief dump 的组件选择。
 * @details 三个任务语义名映射到 DebugServer 的稳定组件名：
 *          `goroutines` → scheduler + fiber；`scheduler` → scheduler；
 *          `locks` → mutex + channel + parking_condition。也接受任意稳定组件名。
 */
struct ComponentSelection {
    bool all{true};
    std::vector<std::string> names;
};

ComponentSelection ParseSections(const std::string& text) {
    static const std::vector<std::string> kKnown = {
        "scheduler", "fiber", "channel", "parking_condition", "mutex",
        "condition_variable", "io", "events", "runtime"};
    ComponentSelection selection;
    if (text.empty()) {
        throw CliError(ExitCode::kUsage,
                       "选项 '--sections' 为空；可选 all、goroutines、scheduler、locks 或组件名");
    }
    if (text == "all") {
        return selection;
    }
    if (text == "goroutines") {
        selection.all = false;
        selection.names = {"scheduler", "fiber"};
        return selection;
    }
    if (text == "locks") {
        selection.all = false;
        selection.names = {"mutex", "channel", "parking_condition"};
        return selection;
    }

    selection.all = false;
    std::size_t begin = 0;
    while (begin <= text.size()) {
        const auto end = text.find(',', begin);
        const std::string item = Trim(text.substr(
            begin, end == std::string::npos ? std::string::npos : end - begin));
        if (!item.empty()) {
            if (item == "all") {
                return ComponentSelection{};
            }
            std::string component = item;
            if (item == "scheduler") {
                component = "scheduler";
            }
            if (std::find(kKnown.begin(), kKnown.end(), component) == kKnown.end()) {
                throw CliError(ExitCode::kUsage,
                               "未知 section '" + item +
                                   "'；可选 all、goroutines、scheduler、locks，或组件名：" +
                                   "scheduler/fiber/channel/parking_condition/mutex/"
                                   "condition_variable/io/events/runtime");
            }
            if (std::find(selection.names.begin(), selection.names.end(), component) ==
                selection.names.end()) {
                selection.names.push_back(component);
            }
        }
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1;
    }
    if (selection.names.empty()) {
        throw CliError(ExitCode::kUsage,
                       "选项 '--sections' 没有有效项；可选 all、goroutines、scheduler、locks 或组件名");
    }
    return selection;
}

nlohmann::json ParseJsonPayload(const std::string& payload, const char* what) {
    try {
        return nlohmann::json::parse(payload);
    } catch (const std::exception& error) {
        throw CliError(ExitCode::kFailure,
                       std::string("DebugServer 返回的 ") + what + " 不是合法 JSON：" +
                           error.what());
    }
}

nlohmann::json ParseJsonText(const std::string& content, const std::string& path,
                             const char* what) {
    try {
        return nlohmann::json::parse(content);
    } catch (const std::exception& error) {
        throw CliError(ExitCode::kFormat,
                       std::string(what) + " JSON 解析失败：'" + path + "'：" + error.what());
    }
}

std::string StateOf(const nlohmann::json& item) {
    if (!item.is_object()) {
        return {};
    }
    const auto it = item.find("state");
    if (it == item.end()) {
        return {};
    }
    if (it->is_string()) {
        return it->get<std::string>();
    }
    if (it->is_number_integer()) {
        return std::to_string(it->get<long long>());
    }
    return it->dump();
}

std::string ReasonOf(const nlohmann::json& item) {
    if (!item.is_object()) {
        return {};
    }
    const auto it = item.find("suspend_reason");
    if (it == item.end()) {
        return {};
    }
    if (it->is_string()) {
        return it->get<std::string>();
    }
    if (it->is_number_integer()) {
        return std::to_string(it->get<long long>());
    }
    return it->dump();
}

std::string EventTypeOf(const nlohmann::json& item) {
    if (!item.is_object()) {
        return {};
    }
    for (const char* key : {"type", "event", "name", "kind"}) {
        const auto it = item.find(key);
        if (it == item.end()) {
            continue;
        }
        if (it->is_string()) {
            return it->get<std::string>();
        }
        if (it->is_number_integer()) {
            return std::to_string(it->get<long long>());
        }
    }
    return {};
}

std::string CompactJson(const nlohmann::json& value, std::size_t limit = 240) {
    std::string text;
    try {
        text = value.dump();
    } catch (const std::exception& error) {
        text = std::string("<dump 失败: ") + error.what() + ">";
    }
    if (text.size() > limit) {
        text = text.substr(0, limit) + "...";
    }
    return text;
}

/// 统计文本行数，并判断前 4 KiB 是否含 NUL（用于区分文本/二进制记录）。
void CountLinesAndBinary(const std::string& path, std::uint64_t* lines, bool* binary) {
    *lines = 0;
    *binary = false;
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return;
    }
    std::vector<char> buffer(64 * 1024);
    bool first_chunk = true;
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = static_cast<std::size_t>(input.gcount());
        if (count == 0) {
            break;
        }
        *lines += static_cast<std::uint64_t>(
            std::count(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(count), '\n'));
        if (first_chunk) {
            *binary = std::find(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(count),
                                '\0') != buffer.begin() + static_cast<std::ptrdiff_t>(count);
            first_chunk = false;
        }
    }
}

/**
 * @brief 启动外部后端并等待，stdio 继承终端。
 * @param environment 追加的环境变量（在子进程中 setenv）。
 * @return 子进程退出码；被信号杀死时返回 128+signo。
 * @throws CliError 后端无法执行（`kUnavailable`）。
 */
int RunBackend(const std::string& program, const std::vector<std::string>& arguments,
               const std::vector<std::pair<std::string, std::string>>& environment = {}) {
    int error_pipe[2] = {-1, -1};
    if (::pipe(error_pipe) != 0) {
        throw CliError(ExitCode::kFailure,
                       "创建子进程错误管道失败：" + std::string(std::strerror(errno)));
    }
    ::fcntl(error_pipe[0], F_SETFD, FD_CLOEXEC);
    ::fcntl(error_pipe[1], F_SETFD, FD_CLOEXEC);

    std::fflush(nullptr);
    const pid_t child = ::fork();
    if (child < 0) {
        const int saved = errno;
        ::close(error_pipe[0]);
        ::close(error_pipe[1]);
        throw CliError(ExitCode::kFailure, "fork 失败：" + std::string(std::strerror(saved)));
    }
    if (child == 0) {
        ::close(error_pipe[0]);
        for (const auto& entry : environment) {
            ::setenv(entry.first.c_str(), entry.second.c_str(), 1);
        }
        std::vector<char*> argv;
        argv.reserve(arguments.size() + 2);
        argv.push_back(const_cast<char*>(program.c_str()));
        for (const std::string& argument : arguments) {
            argv.push_back(const_cast<char*>(argument.c_str()));
        }
        argv.push_back(nullptr);
        ::execv(program.c_str(), argv.data());
        const int saved = errno;
        const ssize_t ignored = ::write(error_pipe[1], &saved, sizeof(saved));
        (void)ignored;
        ::_exit(127);
    }

    ::close(error_pipe[1]);
    int child_errno = 0;
    ssize_t read_count = 0;
    do {
        read_count = ::read(error_pipe[0], &child_errno, sizeof(child_errno));
    } while (read_count < 0 && errno == EINTR);
    ::close(error_pipe[0]);

    int status = 0;
    for (;;) {
        const pid_t waited = ::waitpid(child, &status, 0);
        if (waited >= 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        throw CliError(ExitCode::kFailure,
                       "waitpid 失败：" + std::string(std::strerror(errno)));
    }
    if (read_count == static_cast<ssize_t>(sizeof(child_errno))) {
        throw CliError(ExitCode::kUnavailable,
                       "无法启动后端 '" + program + "'：" + std::string(std::strerror(child_errno)));
    }
    if (WIFSIGNALED(status)) {
        const int signal_number = WTERMSIG(status);
        std::fprintf(stderr, "go2cpp-cli: 后端被信号 %d（%s）终止\n", signal_number,
                     strsignal(signal_number));
        return 128 + signal_number;
    }
    return WEXITSTATUS(status);
}

/// 后端捕获结果：退出码 + 完整 stdout（stderr 仍继承终端）。
struct CapturedBackend {
    int exit_code{0};
    std::string output;
};

/**
 * @brief 启动外部后端并捕获其 stdout。
 *
 * `replay play` 必须把引擎的 JSON 报告解析回来（`replay_executed`），
 * 不能像其它后端那样让 stdout 直接继承终端。stderr 仍继承终端：
 * 错误信息不应藏进缓冲区。最多保留 `kMaxCaptureBytes`，超出部分丢弃，
 * 防止畸形后端用无限输出打爆 CLI。
 *
 * @return 子进程退出码 + stdout；被信号杀死时退出码为 128+signo。
 * @throws CliError 后端无法执行（`kUnavailable`）或管道/fork 失败（`kFailure`）。
 */
CapturedBackend RunBackendCapture(const std::string& program,
                                  const std::vector<std::string>& arguments) {
    int output_pipe[2] = {-1, -1};
    if (::pipe(output_pipe) != 0) {
        throw CliError(ExitCode::kFailure,
                       "创建子进程输出管道失败：" + std::string(std::strerror(errno)));
    }
    int error_pipe[2] = {-1, -1};
    if (::pipe(error_pipe) != 0) {
        const int saved = errno;
        ::close(output_pipe[0]);
        ::close(output_pipe[1]);
        throw CliError(ExitCode::kFailure,
                       "创建子进程错误管道失败：" + std::string(std::strerror(saved)));
    }
    ::fcntl(output_pipe[0], F_SETFD, FD_CLOEXEC);
    ::fcntl(error_pipe[0], F_SETFD, FD_CLOEXEC);
    ::fcntl(error_pipe[1], F_SETFD, FD_CLOEXEC);

    std::fflush(nullptr);
    const pid_t child = ::fork();
    if (child < 0) {
        const int saved = errno;
        ::close(output_pipe[0]);
        ::close(output_pipe[1]);
        ::close(error_pipe[0]);
        ::close(error_pipe[1]);
        throw CliError(ExitCode::kFailure, "fork 失败：" + std::string(std::strerror(saved)));
    }
    if (child == 0) {
        ::close(output_pipe[0]);
        ::close(error_pipe[0]);
        if (::dup2(output_pipe[1], STDOUT_FILENO) < 0) {
            const int saved = errno;
            const ssize_t ignored = ::write(error_pipe[1], &saved, sizeof(saved));
            (void)ignored;
            ::_exit(127);
        }
        ::close(output_pipe[1]);
        std::vector<char*> argv;
        argv.reserve(arguments.size() + 2);
        argv.push_back(const_cast<char*>(program.c_str()));
        for (const std::string& argument : arguments) {
            argv.push_back(const_cast<char*>(argument.c_str()));
        }
        argv.push_back(nullptr);
        ::execv(program.c_str(), argv.data());
        const int saved = errno;
        const ssize_t ignored = ::write(error_pipe[1], &saved, sizeof(saved));
        (void)ignored;
        ::_exit(127);
    }

    ::close(output_pipe[1]);
    ::close(error_pipe[1]);

    int child_errno = 0;
    ssize_t error_read = 0;
    do {
        error_read = ::read(error_pipe[0], &child_errno, sizeof(child_errno));
    } while (error_read < 0 && errno == EINTR);
    ::close(error_pipe[0]);

    CapturedBackend result;
    char buffer[64 * 1024];
    for (;;) {
        const ssize_t count = ::read(output_pipe[0], buffer, sizeof(buffer));
        if (count > 0) {
            const std::size_t received = static_cast<std::size_t>(count);
            if (result.output.size() < kMaxCaptureBytes) {
                const std::size_t room = kMaxCaptureBytes - result.output.size();
                result.output.append(buffer, std::min(received, room));
            }
            continue;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
    ::close(output_pipe[0]);

    int status = 0;
    for (;;) {
        const pid_t waited = ::waitpid(child, &status, 0);
        if (waited >= 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        throw CliError(ExitCode::kFailure,
                       "waitpid 失败：" + std::string(std::strerror(errno)));
    }
    if (error_read == static_cast<ssize_t>(sizeof(child_errno))) {
        throw CliError(ExitCode::kUnavailable,
                       "无法启动后端 '" + program + "'：" +
                           std::string(std::strerror(child_errno)));
    }
    if (WIFSIGNALED(status)) {
        const int signal_number = WTERMSIG(status);
        std::fprintf(stderr, "go2cpp-cli: 后端被信号 %d（%s）终止\n", signal_number,
                     strsignal(signal_number));
        result.exit_code = 128 + signal_number;
        return result;
    }
    result.exit_code = WEXITSTATUS(status);
    return result;
}

// ---------------------------------------------------------------------------
// dump / status
// ---------------------------------------------------------------------------

int RunDumpImpl(const std::vector<std::string>& args) {
    const ParsedArgs parsed = ParseArgs(
        args,
        {
            {"--pid", "PID", "目标进程 pid", true},
            {"--out", "FILE", "输出文件，默认 -（stdout）"},
            {"--sections", "LIST", "all|goroutines|scheduler|locks|组件名"},
            {"--view", "NAME", "gmp|timeline|sched|park|timer|primitives"},
            {"--gid", "GID", "timeline 视图的目标 G"},
            {"--format", "FMT", "json 或 text，默认 json"},
            {"--timeout-ms", "N", "总超时毫秒，默认 5000"},
        },
        0, 0);
    if (parsed.help_requested) {
        PrintDumpHelp();
        return 0;
    }

    const int pid = ParseIntOption(parsed.Get("--pid"), "--pid", 1, 2147483647);
    const int timeout_ms =
        ParseIntOption(parsed.GetOr("--timeout-ms", "5000"), "--timeout-ms", 1, kMaxTimeoutMs);
    const std::string format = parsed.GetOr("--format", "json");
    if (format != "json" && format != "text") {
        throw CliError(ExitCode::kUsage,
                       "选项 '--format' 只接受 json 或 text，实际 '" + format + "'");
    }
    const ComponentSelection selection = ParseSections(parsed.GetOr("--sections", "all"));
    const bool as_json = format == "json";

    DebugClient client(ResolveDebugTarget(pid, timeout_ms));
    std::string content;

    // 视图模式：一条 `VIEW <name> [gid]` 请求，服务端返回结构化 JSON。
    const std::string view = parsed.Get("--view");
    if (!view.empty()) {
        static const std::vector<std::string> kKnownViews = {
            "gmp", "timeline", "sched", "park", "timer", "primitives"};
        if (parsed.Has("--sections")) {
            throw CliError(ExitCode::kUsage,
                           "选项 '--view' 与 '--sections' 不能同时使用");
        }
        if (!as_json) {
            throw CliError(ExitCode::kUsage,
                           "选项 '--view' 只输出 JSON；不要与 '--format text' 同时使用");
        }
        if (std::find(kKnownViews.begin(), kKnownViews.end(), view) ==
            kKnownViews.end()) {
            throw CliError(ExitCode::kUsage,
                           "未知视图 '" + view +
                               "'；可选 gmp/timeline/sched/park/timer/primitives");
        }
        std::uint64_t gid = 0;
        if (parsed.Has("--gid")) {
            gid = static_cast<std::uint64_t>(
                ParseIntOption(parsed.Get("--gid"), "--gid", 0, 2147483647));
        }
        if (view == "timeline" && gid == 0) {
            throw CliError(ExitCode::kUsage,
                           "timeline 视图需要 --gid <GID>");
        }
        std::string command = "VIEW " + view;
        if (gid != 0) {
            command += " " + std::to_string(gid);
        }
        try {
            const DebugReply reply = client.Call(command);
            content = RenderJson(ParseJsonPayload(reply.payload, "VIEW JSON"));
        } catch (const DebugError& error) {
            if (error.remote_code() == "unknown-view") {
                throw CliError(
                    ExitCode::kUnavailable,
                    "目标进程不支持视图 '" + view +
                        "'；请确认它以 GO2CPP_DEBUG_DUMP=ON 构建并运行当前版本。");
            }
            if (error.remote_code() == "bad-command") {
                throw CliError(ExitCode::kUsage,
                               "视图请求被拒绝：" + std::string(error.what()) +
                                   "\n  提示：timeline 需要 --gid <GID>。");
            }
            throw;
        }
        WriteOutput(parsed.GetOr("--out", "-"), content);
        return 0;
    }

    if (selection.all) {
        const DebugReply reply = client.Call(as_json ? "JSON ALL" : "DUMP ALL");
        if (as_json) {
            content = RenderJson(ParseJsonPayload(reply.payload, "dump JSON"));
        } else {
            content = reply.payload;
        }
    } else {
        // 逐组件请求：这样「目标进程未注册该组件」能准确报出来，而不是被 ALL
        // 静默省略。任一组件不可用即整体失败，避免输出半份 dump。
        nlohmann::json components = nlohmann::json::object();
        std::string text;
        text += "schema=go2cpp-cli.dump.selection/1\n";
        text += "version=1\n";
        text += "component_count=" + std::to_string(selection.names.size()) + "\n";
        for (const std::string& name : selection.names) {
            DebugReply reply;
            try {
                reply = client.Call((as_json ? "JSON " : "DUMP ") + name);
            } catch (const DebugError& error) {
                if (error.remote_code() == "unknown-component") {
                    throw CliError(ExitCode::kUnavailable,
                                   "目标进程未注册组件 '" + name +
                                       "'；DebugServer 的 LIST 命令可查看实际可用组件。\n"
                                       "  程序中需要调用 go2cpp::debug::RegisterXxx(\"" +
                                       name + "\", ...) 才会暴露该组件。");
                }
                throw;
            }
            if (as_json) {
                components[name] = ParseJsonPayload(reply.payload, ("JSON " + name).c_str());
            } else {
                text += "\n[component " + name + "]\n";
                text += reply.payload;
            }
        }
        if (as_json) {
            nlohmann::json aggregate;
            aggregate["schema"] = "go2cpp-cli.dump.selection/1";
            aggregate["version"] = 1;
            aggregate["components"] = std::move(components);
            content = RenderJson(aggregate);
        } else {
            content = std::move(text);
        }
    }

    WriteOutput(parsed.GetOr("--out", "-"), content);
    return 0;
}

std::string TaskDistribution(const nlohmann::json& tasks, std::size_t* count) {
    *count = 0;
    if (!tasks.is_array()) {
        return {};
    }
    *count = tasks.size();
    std::map<std::string, std::uint64_t> states;
    for (const nlohmann::json& task : tasks) {
        const std::string state = StateOf(task);
        if (!state.empty()) {
            ++states[state];
        }
    }
    std::string text;
    for (const auto& [state, amount] : states) {
        if (!text.empty()) {
            text += ", ";
        }
        text += state + "=" + std::to_string(amount);
    }
    return text;
}

void PrintStatus(const std::map<std::string, std::string>& status,
                 const nlohmann::json& scheduler, bool has_scheduler, int pid) {
    int missing = 0;
    std::printf("目标进程: pid=%d\n", pid);

    const auto protocol = status.find("protocol");
    std::printf("协议: %s\n", protocol == status.end() ? "(未知)" : protocol->second.c_str());

    const auto uptime = status.find("uptime_ns");
    const auto running = status.find("running");
    const auto components = status.find("components");
    if (uptime != status.end() || running != status.end() || components != status.end()) {
        std::printf("DebugServer: ");
        std::printf("%s", running != status.end() ? running->second.c_str() : "?");
        if (uptime != status.end()) {
            std::printf("，uptime %s", FormatDurationNs(std::strtoull(uptime->second.c_str(), nullptr, 10)).c_str());
        }
        if (components != status.end()) {
            std::printf("，组件 %s 个", components->second.c_str());
        }
        std::printf("\n");
    } else {
        std::printf("DebugServer: (响应缺少 running/uptime_ns/components)\n");
        ++missing;
    }
    const auto path = status.find("path");
    if (path != status.end()) {
        std::printf("路径: %s\n", path->second.c_str());
    }

    if (!has_scheduler) {
        std::printf(
            "调度器: (目标进程未注册 scheduler 组件；在程序中调用 "
            "go2cpp::debug::RegisterScheduler(\"scheduler\", scheduler) 后可用)\n");
        std::printf("内存: (DebugServer 未提供内存统计；可用 `dump --sections runtime` 查看运行信息)\n");
        return;
    }

    const auto processor_count = FindNumber(scheduler, {"processor_count"});
    const auto worker_count = FindNumber(scheduler, {"worker_count"});
    const auto running_flag = Dig(scheduler, {"running"});
    std::printf("调度器: processors=%s workers=%s running=%s\n",
                processor_count ? FormatCount(*processor_count).c_str() : "?",
                worker_count ? FormatCount(*worker_count).c_str() : "?",
                (running_flag != nullptr && running_flag->is_boolean())
                    ? (running_flag->get<bool>() ? "true" : "false")
                    : "?");

    std::size_t task_count = 0;
    const nlohmann::json* tasks = Dig(scheduler, {"tasks"});
    const std::string distribution = TaskDistribution(tasks != nullptr ? *tasks : nlohmann::json(), &task_count);
    if (tasks != nullptr && tasks->is_array()) {
        std::printf("任务: %zu 个", task_count);
        if (!distribution.empty()) {
            std::printf("（%s）", distribution.c_str());
        }
        std::printf("\n");
    } else {
        std::printf("任务: (scheduler 响应缺少 tasks 数组)\n");
        ++missing;
    }

    const auto local_total = FindNumber(scheduler, {"runqueue", "local_total"});
    const auto global_queue = FindNumber(scheduler, {"runqueue", "global"});
    const nlohmann::json* processors = Dig(scheduler, {"processors"});
    std::string line = "runqueue: ";
    line += local_total ? "local=" + FormatCount(*local_total) : "local=?";
    line += global_queue ? " global=" + FormatCount(*global_queue) : " global=?";
    if (processors != nullptr && processors->is_array()) {
        line += " per_p=[";
        for (std::size_t index = 0; index < processors->size(); ++index) {
            if (index != 0) {
                line += ", ";
            }
            const auto queued = FindNumber((*processors)[index], {"queued"});
            line += queued ? FormatCount(*queued) : "?";
        }
        line += "]";
    }
    std::printf("%s\n", line.c_str());
    if (!local_total || !global_queue) {
        ++missing;
    }

    const auto task_runs = FindNumber(scheduler, {"metrics", "task_runs"});
    const auto completions = FindNumber(scheduler, {"metrics", "task_completions"});
    const auto resume_ns = FindNumber(scheduler, {"metrics", "fiber_resume_ns"});
    const auto local_pops = FindNumber(scheduler, {"metrics", "local_queue_pops"});
    const auto steal_pops = FindNumber(scheduler, {"metrics", "steal_pops"});
    if (task_runs || completions || resume_ns) {
        std::string metrics = "调度指标: ";
        metrics += task_runs ? "task_runs=" + FormatCount(*task_runs) : "task_runs=?";
        metrics += completions ? " completions=" + FormatCount(*completions) : " completions=?";
        if (resume_ns) {
            metrics += " resume=" + FormatDurationNs(static_cast<std::uint64_t>(*resume_ns));
            if (task_runs && *task_runs > 0) {
                metrics += "（avg " +
                           FormatDurationNs(static_cast<std::uint64_t>(*resume_ns / *task_runs)) +
                           "/run）";
            }
        }
        if (local_pops) {
            metrics += " local_pops=" + FormatCount(*local_pops);
        }
        if (steal_pops) {
            metrics += " steal=" + FormatCount(*steal_pops);
        }
        std::printf("%s\n", metrics.c_str());
        if (!task_runs || !completions) {
            ++missing;
        }
    } else {
        std::printf("调度指标: (scheduler 响应缺少 metrics)\n");
        ++missing;
    }

    std::printf("内存: (DebugServer 未提供内存统计；可用 `dump --sections runtime` 查看运行信息)\n");
    if (missing > 0) {
        std::printf("\n注: 有 %d 处字段缺失；DebugServer 的 schema 见 "
                    "include/go2cpp/debug/debug_dump.hpp。\n",
                    missing);
    }
}

int RunStatusImpl(const std::vector<std::string>& args) {
    const ParsedArgs parsed = ParseArgs(
        args,
        {
            {"--pid", "PID", "目标进程 pid", true},
            {"--json", "", "输出 status + scheduler 的 JSON"},
            {"--timeout-ms", "N", "总超时毫秒，默认 5000"},
        },
        0, 0);
    if (parsed.help_requested) {
        PrintStatusHelp();
        return 0;
    }

    const int pid = ParseIntOption(parsed.Get("--pid"), "--pid", 1, 2147483647);
    const int timeout_ms =
        ParseIntOption(parsed.GetOr("--timeout-ms", "5000"), "--timeout-ms", 1, kMaxTimeoutMs);

    DebugClient client(ResolveDebugTarget(pid, timeout_ms));
    const DebugReply status_reply = client.Call("STATUS");
    const std::map<std::string, std::string> status = ParseKeyValueText(status_reply.payload);

    nlohmann::json scheduler;
    bool has_scheduler = true;
    try {
        const DebugReply reply = client.Call("JSON scheduler");
        scheduler = ParseJsonPayload(reply.payload, "scheduler JSON");
    } catch (const DebugError& error) {
        if (error.remote_code() == "unknown-component") {
            has_scheduler = false;
        } else {
            throw;
        }
    }

    if (parsed.Has("--json")) {
        nlohmann::json out;
        out["status"] = status;
        if (has_scheduler) {
            out["scheduler"] = scheduler;
        } else {
            out["scheduler"] = nullptr;
        }
        WriteOutput("-", RenderJson(out));
        return 0;
    }
    PrintStatus(status, scheduler, has_scheduler, pid);
    return 0;
}

// ---------------------------------------------------------------------------
// replay
// ---------------------------------------------------------------------------

struct RecordParse {
    bool help{false};
    std::string out;
    std::vector<std::string> command;
};

RecordParse ParseRecordArgs(const std::vector<std::string>& args) {
    RecordParse parsed;
    bool saw_out = false;
    bool after_double_dash = false;
    bool seen_executable = false;

    const auto take_out_value = [&](const std::string& token, std::size_t& index) {
        if (saw_out) {
            throw CliError(ExitCode::kUsage, "选项 '--out' 重复出现");
        }
        if (StartsWith(token, "--out=")) {
            parsed.out = token.substr(6);
            saw_out = true;
            return;
        }
        if (index + 1 >= args.size()) {
            throw CliError(ExitCode::kUsage, "选项 '--out' 缺少值（应为 <FILE>）");
        }
        parsed.out = args[++index];
        saw_out = true;
    };

    for (std::size_t index = 0; index < args.size(); ++index) {
        const std::string& token = args[index];
        if (after_double_dash) {
            parsed.command.push_back(token);
            seen_executable = true;
            continue;
        }
        if (token == "--") {
            after_double_dash = true;
            continue;
        }
        if (!seen_executable) {
            if (token == "-h" || token == "--help") {
                parsed.help = true;
                return parsed;
            }
            if (token == "--out" || StartsWith(token, "--out=")) {
                take_out_value(token, index);
                continue;
            }
            if (StartsWith(token, "--") || (token.size() > 1 && token[0] == '-')) {
                throw CliError(ExitCode::kUsage,
                               "未知选项 '" + token +
                                   "'；replay record 只接受 --out，其余参数属于被记录的程序（可用 -- 分隔）");
            }
            parsed.command.push_back(token);
            seen_executable = true;
            continue;
        }
        // 已确定 <exe>：为支持 `record <exe> [args...] --out <file>` 形态，
        // `--out` 仍按 CLI 选项处理；其余 token 原样透传。
        if (token == "--out" || StartsWith(token, "--out=")) {
            take_out_value(token, index);
            continue;
        }
        parsed.command.push_back(token);
    }
    if (parsed.command.empty()) {
        throw CliError(ExitCode::kUsage,
                       "缺少要记录的可执行文件：用法 `go2cpp-cli replay record <exe> [args...] --out <file>`");
    }
    return parsed;
}

int RunReplayRecord(const std::vector<std::string>& args) {
    const RecordParse parsed = ParseRecordArgs(args);
    if (parsed.help) {
        PrintReplayHelp();
        return 0;
    }
    if (parsed.out.empty()) {
        throw CliError(ExitCode::kUsage,
                       "缺少必需选项 '--out <FILE>'（例如 `replay record ./app --out run.g2cr`）");
    }
    if (parsed.out == "-") {
        throw CliError(ExitCode::kUsage,
                       "replay record 的 '--out' 必须是文件路径，不能是 '-'");
    }

    const std::string executable = FindExecutable(parsed.command.front());
    const std::string out_absolute = AbsolutePath(parsed.out);

    const std::filesystem::path parent = std::filesystem::path(out_absolute).parent_path();
    std::error_code filesystem_error;
    if (!std::filesystem::is_directory(parent, filesystem_error) || filesystem_error) {
        throw CliError(ExitCode::kFailure,
                       "输出目录不存在：'" + parent.string() + "'");
    }
    // 绝不在启动被记录程序之前删除 --out（P2-5）：
    // 1) 用户可能误指一个已有文件（甚至是被记录程序本身），删除不可恢复；
    // 2) 「旧记录掩盖本次没有记录」应当用「已存在即拒绝」解决，而不是删除。
    if (std::filesystem::is_directory(out_absolute, filesystem_error) && !filesystem_error) {
        throw CliError(ExitCode::kUsage,
                       "'--out' 指向目录：'" + out_absolute + "'（需要文件路径）");
    }
    filesystem_error.clear();
    if (std::filesystem::exists(out_absolute, filesystem_error) && !filesystem_error) {
        throw CliError(
            ExitCode::kUsage,
            "记录文件已存在，拒绝覆盖：'" + out_absolute + "'\n"
            "  replay record 会在被记录程序退出后写入该路径，启动前删除会不可恢复地\n"
            "  销毁原文件。请先删除/改名该文件，或改用新的 --out 路径。");
    }

    int error_pipe[2] = {-1, -1};
    if (::pipe(error_pipe) != 0) {
        throw CliError(ExitCode::kFailure,
                       "创建子进程错误管道失败：" + std::string(std::strerror(errno)));
    }
    ::fcntl(error_pipe[0], F_SETFD, FD_CLOEXEC);
    ::fcntl(error_pipe[1], F_SETFD, FD_CLOEXEC);

    std::fflush(nullptr);
    const pid_t child = ::fork();
    if (child < 0) {
        const int saved = errno;
        ::close(error_pipe[0]);
        ::close(error_pipe[1]);
        throw CliError(ExitCode::kFailure, "fork 失败：" + std::string(std::strerror(saved)));
    }
    if (child == 0) {
        ::close(error_pipe[0]);
        ::setenv("GO2CPP_REPLAY", "ON", 1);
        ::setenv("GO2CPP_REPLAY_RECORD", out_absolute.c_str(), 1);
        std::vector<char*> argv;
        argv.reserve(parsed.command.size() + 1);
        for (const std::string& argument : parsed.command) {
            argv.push_back(const_cast<char*>(argument.c_str()));
        }
        argv.push_back(nullptr);
        ::execv(executable.c_str(), argv.data());
        const int saved = errno;
        const ssize_t ignored = ::write(error_pipe[1], &saved, sizeof(saved));
        (void)ignored;
        ::_exit(127);
    }

    ::close(error_pipe[1]);
    int child_errno = 0;
    ssize_t read_count = 0;
    do {
        read_count = ::read(error_pipe[0], &child_errno, sizeof(child_errno));
    } while (read_count < 0 && errno == EINTR);
    ::close(error_pipe[0]);

    int status = 0;
    for (;;) {
        const pid_t waited = ::waitpid(child, &status, 0);
        if (waited >= 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        throw CliError(ExitCode::kFailure,
                       "waitpid 失败：" + std::string(std::strerror(errno)));
    }
    if (read_count == static_cast<ssize_t>(sizeof(child_errno))) {
        throw CliError(ExitCode::kFailure,
                       "无法启动 '" + parsed.command.front() + "'：" +
                           std::string(std::strerror(child_errno)));
    }

    struct stat info {};
    const bool record_exists =
        ::stat(out_absolute.c_str(), &info) == 0 && S_ISREG(info.st_mode) && info.st_size > 0;
    std::uint64_t lines = 0;
    bool binary = false;
    if (record_exists) {
        CountLinesAndBinary(out_absolute, &lines, &binary);
    }

    if (WIFSIGNALED(status)) {
        const int signal_number = WTERMSIG(status);
        std::fprintf(stderr, "go2cpp-cli: 程序被信号 %d（%s）终止\n", signal_number,
                     strsignal(signal_number));
        if (record_exists) {
            std::printf("记录文件: %s（%s，可能不完整）\n", out_absolute.c_str(),
                        FormatBytes(static_cast<std::uint64_t>(info.st_size)).c_str());
        }
        return 128 + signal_number;
    }

    const int exit_code = WEXITSTATUS(status);
    std::printf("程序退出码: %d\n", exit_code);
    if (record_exists) {
        if (binary) {
            std::printf("记录文件: %s（%s，二进制）\n", out_absolute.c_str(),
                        FormatBytes(static_cast<std::uint64_t>(info.st_size)).c_str());
        } else {
            std::printf("记录文件: %s（%s，%llu 行）\n", out_absolute.c_str(),
                        FormatBytes(static_cast<std::uint64_t>(info.st_size)).c_str(),
                        static_cast<unsigned long long>(lines));
        }
    }
    std::fflush(stdout);

    if (exit_code != 0) {
        return exit_code;
    }
    if (!record_exists) {
        throw CliError(
            ExitCode::kUnavailable,
            "程序成功退出，但没有生成重放记录文件 '" + out_absolute + "'。\n"
            "  当前 tools/replay 提供的是 demo 记录器（go2cpp_replay_record <file>，固定场景），\n"
            "  尚未实现通用的 GO2CPP_REPLAY 注入；该约定待重放服务（2c3fb6f2）对齐。\n"
            "  记录文件格式为二进制 G2CRPLY（见 docs/cli.md）。");
    }
    return 0;
}

int RunReplayPlay(const std::vector<std::string>& args) {
    const ParsedArgs parsed = ParseArgs(args, {}, 1, 1);
    if (parsed.help_requested) {
        PrintReplayHelp();
        return 0;
    }
    const std::string& path = parsed.positional.front();
    const FileProbe probe = ProbeFile(path, kMaxFileBytes);
    if (probe.format != FileFormat::kReplayBinary) {
        throw CliError(ExitCode::kFormat,
                       "记录文件格式不匹配：'" + probe.path + "'\n"
                       "  期望重放记录（魔数 G2CRPLY，由 tools/replay 的 Recorder 生成）；实际：" +
                           probe.detail);
    }

    const std::string backend = FindBackend(
        {"go2cpp-replay", "go2cpp_replay", "replay"}, "GO2CPP_REPLAY_TOOL");
    if (backend.empty()) {
        throw CliError(
            ExitCode::kUnavailable,
            "重放引擎未就绪：找不到 'replay' 后端。\n"
            "  已校验记录文件 '" + probe.path + "'（" + FormatBytes(probe.size) + "，" +
                probe.detail + "），\n"
            "  但离线重放需要 tools/replay 构建出的 replay 可执行文件：\n"
            "    cmake -S tools/replay -B build-replay && cmake --build build-replay -j\n"
            "  也可设置 GO2CPP_REPLAY_TOOL=<path> 指向它。");
    }
    // 必须把 `play` 子命令传给引擎：只传记录文件路径时，引擎会退化执行
    // `info`（静态分析），CLI 却返回成功——重放语义完全丢失（P1-2）。
    // 环境变量（GO2CPP_REPLAY_DROP_WAKE 等故障注入参数）由 fork/exec 原样
    // 继承给后端；CLI 不过滤/清空环境，replay 端 RunPlay 负责解析（D4）。
    const CapturedBackend backend_result =
        RunBackendCapture(backend, {"play", AbsolutePath(probe.path)});
    if (!backend_result.output.empty()) {
        const std::size_t written =
            std::fwrite(backend_result.output.data(), 1, backend_result.output.size(), stdout);
        (void)written;
        std::fflush(stdout);
    }
    if (backend_result.exit_code != 0) {
        // 退出码 3 = 引擎没有可用的 RECORD_FIBER_ENTRY；2 = 格式/用法错误；
        // 1 = stuck/不一致。原样透传给 CI。
        return backend_result.exit_code;
    }
    // 退出码 0 还不够：必须确认引擎真的执行了重放，而不是只做了静态分析。
    bool replay_executed = false;
    try {
        const nlohmann::json report = nlohmann::json::parse(backend_result.output);
        replay_executed = report.value("replay_executed", false);
    } catch (const std::exception&) {
        throw CliError(
            ExitCode::kUnavailable,
            "重放引擎的输出不是可解析的 JSON 报告：'" + probe.path + "'\n"
            "  需要 tools/replay 的 `replay play <file>`（输出 replay_executed 字段）。\n"
            "  确认 GO2CPP_REPLAY_TOOL 指向当前 tools/replay 的构建。");
    }
    if (!replay_executed) {
        // 后端退化为静态分析（例如把 `play` 当成记录文件路径）时，绝不能
        // 把「未重放」当成功上报告给 CI。
        throw CliError(
            ExitCode::kUnavailable,
            "重放引擎没有执行重放（replay_executed=false）：'" + probe.path + "'\n"
            "  后端可能把 `play` 当成记录文件路径而退化为静态分析；\n"
            "  确认 GO2CPP_REPLAY_TOOL 指向支持 `replay play <file>` 的构建。");
    }
    return 0;
}

int RunReplayImpl(const std::vector<std::string>& args) {
    if (args.empty() || args.front() == "-h" || args.front() == "--help") {
        PrintReplayHelp();
        return 0;
    }
    const std::string subcommand = args.front();
    const std::vector<std::string> rest(args.begin() + 1, args.end());
    if (subcommand == "record") {
        return RunReplayRecord(rest);
    }
    if (subcommand == "play") {
        return RunReplayPlay(rest);
    }
    throw CliError(ExitCode::kUsage,
                   "未知的 replay 子命令 '" + subcommand + "'；可选 record 或 play");
}

// ---------------------------------------------------------------------------
// migrate
// ---------------------------------------------------------------------------

/// 校验迁移快照版本：v3（G2CMIG03）是当前写版本，v2（G2CMIG02）保留兼容读；
/// v1（G2CMIG01）已由 tools/migration 移除。只做头部版本检查，完整 CRC/段表/
/// 签名校验交给后端的 `migrate verify`。
void RequireMigrationSupported(const FileProbe& probe) {
    if (probe.format != FileFormat::kMigrationBinary) {
        return;
    }
    const std::uint32_t version = MigrationFormatVersion(probe.head);
    if (version != 3 && version != 2) {
        throw CliError(ExitCode::kFormat,
                       "迁移快照版本不支持：'" + probe.path + "' 的 version=" +
                           std::to_string(version) +
                           "；CLI 接受 v3（魔数 G2CMIG03，当前写版本）与 v2（G2CMIG02，兼容读）。");
    }
}

int RunMigrateDumpImpl(const std::vector<std::string>& args) {
    const ParsedArgs parsed = ParseArgs(
        args,
        {
            {"--pid", "PID", "目标进程 pid", true},
            {"--gid", "GID", "要导出的协程 id", true},
            {"--out", "FILE", "输出快照文件（必填）", true},
        },
        0, 0);
    if (parsed.help_requested) {
        PrintMigrateHelp();
        return 0;
    }

    const int pid = ParseIntOption(parsed.Get("--pid"), "--pid", 1, 2147483647);
    const int gid = ParseIntOption(parsed.Get("--gid"), "--gid", 0, 2147483647);
    const std::string out = parsed.Get("--out");
    if (out.empty() || out == "-") {
        throw CliError(ExitCode::kUsage,
                       "migrate dump 需要 '--out <FILE>' 文件路径（快照是二进制，不能写 stdout）");
    }
    const std::string out_absolute = AbsolutePath(out);
    const std::filesystem::path parent = std::filesystem::path(out_absolute).parent_path();
    std::error_code filesystem_error;
    if (!std::filesystem::is_directory(parent, filesystem_error) || filesystem_error) {
        throw CliError(ExitCode::kFailure, "输出目录不存在：'" + parent.string() + "'");
    }

    const std::string proc_entry = "/proc/" + std::to_string(pid);
    if (::access(proc_entry.c_str(), F_OK) != 0) {
        throw CliError(ExitCode::kUnavailable,
                       "进程 " + std::to_string(pid) + " 不存在（" + proc_entry +
                           " 不可访问）；请确认 --pid 指向正在运行的 Go2Cpp 程序");
    }

    const std::string backend =
        FindBackend({"go2cpp-migrate", "go2cpp_migrate", "migrate"}, "GO2CPP_MIGRATE_TOOL");
    if (backend.empty()) {
        throw CliError(
            ExitCode::kUnavailable,
            "迁移工具未就绪：找不到 'migrate' 后端。\n"
            "  构建方式：\n"
            "    cmake -S tools/migration -B build-migration\n"
            "    cmake --build build-migration -j\n"
            "  也可设置 GO2CPP_MIGRATE_TOOL=<path> 指向 build-migration/migrate。");
    }

    // 当前 tools/migration 是 demo：dump 只写它自己进程 A 的快照，不接受 pid/gid。
    // 这里把两者通过环境变量交给后端（后端当前忽略），并在文档中标注待对齐。
    std::fprintf(stderr,
                 "go2cpp-cli: 注意：当前迁移后端（tools/migration demo）不支持按 pid/gid "
                 "抓取运行中的协程；--pid=%d --gid=%d 通过 MIGRATION_PID/MIGRATION_GID 传递，"
                 "待正式迁移接口对齐。\n",
                 pid, gid);
    std::fflush(stderr);
    const int exit_code = RunBackend(
        backend, {"dump", out_absolute},
        {{"MIGRATION_PID", std::to_string(pid)}, {"MIGRATION_GID", std::to_string(gid)}});
    if (exit_code != 0) {
        return exit_code;
    }

    const FileProbe probe = ProbeFile(out_absolute, kMaxFileBytes);
    if (probe.format != FileFormat::kMigrationBinary) {
        throw CliError(ExitCode::kFailure,
                       "迁移后端返回成功，但输出文件不是有效快照：'" + out_absolute +
                           "'（" + probe.detail + "）");
    }
    RequireMigrationSupported(probe);
    std::printf("迁移快照: %s（%s，%s）\n", out_absolute.c_str(),
                FormatBytes(probe.size).c_str(), probe.detail.c_str());
    std::fflush(stdout);
    return 0;
}

int RunMigrateRestoreImpl(const std::vector<std::string>& args) {
    const ParsedArgs parsed = ParseArgs(args, {}, 1, 1);
    if (parsed.help_requested) {
        PrintMigrateHelp();
        return 0;
    }
    const std::string& path = parsed.positional.front();
    const FileProbe probe = ProbeFile(path, kMaxFileBytes);
    if (probe.format != FileFormat::kMigrationBinary) {
        throw CliError(ExitCode::kFormat,
                       "迁移文件格式不匹配：'" + probe.path + "'\n"
                       "  期望迁移快照（魔数 G2CMIG03，兼容 v2 G2CMIG02，由 tools/migration 生成）；实际：" +
                           probe.detail);
    }
    RequireMigrationSupported(probe);

    const std::string backend =
        FindBackend({"go2cpp-migrate", "go2cpp_migrate", "migrate"}, "GO2CPP_MIGRATE_TOOL");
    if (backend.empty()) {
        throw CliError(
            ExitCode::kUnavailable,
            "迁移工具未就绪：找不到 'migrate' 后端。\n"
            "  已校验迁移文件 '" + probe.path + "'（" + FormatBytes(probe.size) + "，" +
                probe.detail + "），\n"
            "  但恢复需要 tools/migration 构建出的 migrate 可执行文件：\n"
            "    cmake -S tools/migration -B build-migration && cmake --build build-migration -j\n"
            "  也可设置 GO2CPP_MIGRATE_TOOL=<path> 指向它。\n"
            "  注意：v3/v2 都允许运行间 PIE/ASLR 地址变化，但仍要求生成方与恢复方\n"
            "  是同一可执行文件（ELF build-id 校验）；SO 版本可以不同。");
    }
    return RunBackend(backend, {"restore", AbsolutePath(probe.path)});
}

int RunMigrateImpl(const std::vector<std::string>& args) {
    if (args.empty() || args.front() == "-h" || args.front() == "--help") {
        PrintMigrateHelp();
        return 0;
    }
    const std::string subcommand = args.front();
    const std::vector<std::string> rest(args.begin() + 1, args.end());
    if (subcommand == "dump") {
        return RunMigrateDumpImpl(rest);
    }
    if (subcommand == "restore") {
        return RunMigrateRestoreImpl(rest);
    }
    throw CliError(ExitCode::kUsage,
                   "未知的 migrate 子命令 '" + subcommand + "'；可选 dump 或 restore");
}

// ---------------------------------------------------------------------------
// debug（离线分析）
// ---------------------------------------------------------------------------

struct TraceStats {
    std::uint64_t records{0};
    std::uint64_t input{0};
    std::uint64_t output{0};
    std::uint64_t unparsed{0};
    std::map<std::string, std::uint64_t> event_types;
    std::vector<std::string> timeline;
};

TraceStats ParseTraceText(const std::string& content, std::size_t top) {
    TraceStats stats;
    std::size_t line_begin = 0;
    while (line_begin <= content.size()) {
        const auto newline = content.find('\n', line_begin);
        const auto line_end = newline == std::string::npos ? content.size() : newline;
        const std::string line = Trim(
            std::string_view(content).substr(line_begin, line_end - line_begin));
        if (!line.empty() && line.front() == '#') {
            std::size_t position = 1;
            while (position < line.size() && line[position] >= '0' && line[position] <= '9') {
                ++position;
            }
            if (position > 1) {
                ++stats.records;
                const std::string rest = Trim(line.substr(position));
                std::size_t cursor = 0;
                while (cursor < rest.size() && rest[cursor] != ' ' && rest[cursor] != '\t') {
                    ++cursor;
                }
                const std::string direction = rest.substr(0, cursor);
                while (cursor < rest.size() && (rest[cursor] == ' ' || rest[cursor] == '\t')) {
                    ++cursor;
                }
                const std::size_t event_begin = cursor;
                while (cursor < rest.size() && rest[cursor] != ' ' && rest[cursor] != '\t') {
                    ++cursor;
                }
                const std::string event = rest.substr(event_begin, cursor - event_begin);
                if (direction == "INPUT") {
                    ++stats.input;
                } else if (direction == "OUTPUT") {
                    ++stats.output;
                } else {
                    ++stats.unparsed;
                }
                if (!event.empty()) {
                    ++stats.event_types[event];
                }
                if (top == 0 || stats.timeline.size() < top) {
                    stats.timeline.push_back(line);
                }
            }
        }
        if (newline == std::string::npos) {
            break;
        }
        line_begin = newline + 1;
    }
    return stats;
}

void PrintJsonDumpStats(const FileProbe& probe, const nlohmann::json& root, std::size_t top,
                        bool as_json) {
    nlohmann::json stats = nlohmann::json::object();
    stats["file"] = probe.path;
    stats["size_bytes"] = probe.size;
    stats["format"] = FileFormatName(probe.format);

    if (root.is_object()) {
        if (root.contains("schema") && root["schema"].is_string()) {
            stats["schema"] = root["schema"];
        }
        nlohmann::json keys = nlohmann::json::array();
        for (auto it = root.begin(); it != root.end(); ++it) {
            keys.push_back(it.key());
        }
        stats["top_level_keys"] = keys;
    }

    std::uint64_t fiber_count = 0;
    nlohmann::json fiber_states = nlohmann::json::object();
    nlohmann::json suspend_reasons = nlohmann::json::object();
    const nlohmann::json* fibers = Dig(root, {"fibers"});
    if (fibers == nullptr) {
        fibers = Dig(root, {"tasks"});
    }
    if (fibers == nullptr) {
        fibers = Dig(root, {"goroutines"});
    }
    if (fibers != nullptr && fibers->is_array()) {
        fiber_count = static_cast<std::uint64_t>(fibers->size());
        for (const nlohmann::json& fiber : *fibers) {
            const std::string state = StateOf(fiber);
            if (!state.empty()) {
                fiber_states[state] = fiber_states.value(state, 0) + 1;
            }
            const std::string reason = ReasonOf(fiber);
            if (!reason.empty()) {
                suspend_reasons[reason] = suspend_reasons.value(reason, 0) + 1;
            }
        }
    }
    stats["fiber_count"] = fiber_count;
    stats["fiber_states"] = fiber_states;
    stats["suspend_reasons"] = suspend_reasons;

    std::uint64_t event_count = 0;
    nlohmann::json event_types = nlohmann::json::object();
    nlohmann::json timeline = nlohmann::json::array();
    const nlohmann::json* events = Dig(root, {"events"});
    if (events == nullptr) {
        events = Dig(root, {"trace"});
    }
    if (events != nullptr && events->is_array()) {
        event_count = static_cast<std::uint64_t>(events->size());
        for (const nlohmann::json& event : *events) {
            const std::string type = EventTypeOf(event);
            if (!type.empty()) {
                event_types[type] = event_types.value(type, 0) + 1;
            }
            if (top == 0 || timeline.size() < top) {
                timeline.push_back(event);
            }
        }
    }
    stats["event_count"] = event_count;
    stats["event_types"] = event_types;
    stats["timeline"] = timeline;

    if (as_json) {
        std::printf("%s", RenderJson(stats).c_str());
        return;
    }

    std::printf("文件: %s（%s）\n", probe.path.c_str(), FormatBytes(probe.size).c_str());
    std::printf("格式: %s\n", probe.detail.c_str());
    if (stats.contains("schema")) {
        std::printf("schema: %s\n", stats["schema"].get<std::string>().c_str());
    }
    if (stats.contains("top_level_keys")) {
        std::string joined;
        for (const auto& key : stats["top_level_keys"]) {
            if (!joined.empty()) {
                joined += ", ";
            }
            joined += key.get<std::string>();
        }
        std::printf("顶层字段: %s\n", joined.c_str());
    }
    if (fiber_count == 0) {
        std::printf("协程/任务: 0 条（或响应中没有 fibers/tasks/goroutines 数组）\n");
    } else {
        std::string distribution;
        for (auto it = fiber_states.begin(); it != fiber_states.end(); ++it) {
            if (!distribution.empty()) {
                distribution += ", ";
            }
            distribution += it.key() + "=" + std::to_string(it.value().get<int>());
        }
        std::printf("协程/任务: %llu 条", static_cast<unsigned long long>(fiber_count));
        if (!distribution.empty()) {
            std::printf("；状态: %s", distribution.c_str());
        }
        std::printf("\n");
    }
    if (event_count > 0) {
        std::string distribution;
        for (auto it = event_types.begin(); it != event_types.end(); ++it) {
            if (!distribution.empty()) {
                distribution += ", ";
            }
            distribution += it.key() + "=" + std::to_string(it.value().get<int>());
        }
        std::printf("事件: %llu 条", static_cast<unsigned long long>(event_count));
        if (!distribution.empty()) {
            std::printf("；类型: %s", distribution.c_str());
        }
        std::printf("\n");
        if (!timeline.empty()) {
            std::printf("时间线（前 %zu 条）:\n", timeline.size());
            for (const auto& event : timeline) {
                std::printf("  %s\n", CompactJson(event).c_str());
            }
        }
    } else {
        std::printf("事件: 0 条（或响应中没有 events/trace 数组）\n");
    }
}

void PrintTraceDumpStats(const FileProbe& probe, const TraceStats& stats, bool as_json) {
    if (as_json) {
        nlohmann::json out = nlohmann::json::object();
        out["file"] = probe.path;
        out["size_bytes"] = probe.size;
        out["format"] = FileFormatName(probe.format);
        out["records"] = stats.records;
        out["input"] = stats.input;
        out["output"] = stats.output;
        out["unparsed"] = stats.unparsed;
        nlohmann::json types = nlohmann::json::object();
        for (const auto& [name, count] : stats.event_types) {
            types[name] = count;
        }
        out["event_types"] = types;
        nlohmann::json timeline = nlohmann::json::array();
        for (const std::string& line : stats.timeline) {
            timeline.push_back(line);
        }
        out["timeline"] = timeline;
        std::printf("%s", RenderJson(out).c_str());
        return;
    }

    std::printf("文件: %s（%s）\n", probe.path.c_str(), FormatBytes(probe.size).c_str());
    std::printf("格式: %s\n", probe.detail.c_str());
    std::printf("记录: %llu 条（INPUT %llu / OUTPUT %llu",
                static_cast<unsigned long long>(stats.records),
                static_cast<unsigned long long>(stats.input),
                static_cast<unsigned long long>(stats.output));
    if (stats.unparsed > 0) {
        std::printf(" / 无法分类 %llu", static_cast<unsigned long long>(stats.unparsed));
    }
    std::printf("）\n");
    if (!stats.event_types.empty()) {
        std::string distribution;
        for (const auto& [name, count] : stats.event_types) {
            if (!distribution.empty()) {
                distribution += ", ";
            }
            distribution += name + "=" + std::to_string(count);
        }
        std::printf("事件类型: %s\n", distribution.c_str());
    }
    if (!stats.timeline.empty()) {
        std::printf("时间线（前 %zu 条）:\n", stats.timeline.size());
        for (const std::string& line : stats.timeline) {
            std::printf("  %s\n", line.c_str());
        }
    }
}

void PrintIniDumpStats(const FileProbe& probe, bool as_json) {
    const std::string content = ReadTextFile(probe.path, kMaxFileBytes);
    std::uint64_t section_count = 0;
    std::uint64_t key_count = 0;
    std::vector<std::string> sections;
    std::size_t line_begin = 0;
    while (line_begin <= content.size()) {
        const auto newline = content.find('\n', line_begin);
        const auto line_end = newline == std::string::npos ? content.size() : newline;
        const std::string line = Trim(
            std::string_view(content).substr(line_begin, line_end - line_begin));
        if (!line.empty() && line.front() != '#' && line.front() != ';') {
            if (line.size() >= 2 && line.front() == '[' && line.back() == ']') {
                ++section_count;
                sections.push_back(line.substr(1, line.size() - 2));
            } else if (line.find('=') != std::string::npos) {
                ++key_count;
            }
        }
        if (newline == std::string::npos) {
            break;
        }
        line_begin = newline + 1;
    }

    if (as_json) {
        nlohmann::json out = nlohmann::json::object();
        out["file"] = probe.path;
        out["size_bytes"] = probe.size;
        out["format"] = FileFormatName(probe.format);
        out["sections"] = section_count;
        out["keys"] = key_count;
        out["section_names"] = sections;
        std::printf("%s", RenderJson(out).c_str());
        return;
    }

    std::printf("文件: %s（%s）\n", probe.path.c_str(), FormatBytes(probe.size).c_str());
    std::printf("格式: %s\n", probe.detail.c_str());
    std::printf("节: %llu 个；键值行: %llu 行\n",
                static_cast<unsigned long long>(section_count),
                static_cast<unsigned long long>(key_count));
    if (!sections.empty()) {
        std::string joined;
        for (const std::string& name : sections) {
            if (!joined.empty()) {
                joined += ", ";
            }
            joined += name;
        }
        std::printf("节列表: %s\n", joined.c_str());
    }
}

/// 读小端 u64（二进制头解析用）。
std::uint64_t ReadLeU64(const std::string& head, std::size_t offset) {
    if (head.size() < offset + 8) {
        return 0;
    }
    std::uint64_t value = 0;
    for (int index = 0; index < 8; ++index) {
        value |= static_cast<std::uint64_t>(static_cast<unsigned char>(head[offset + index]))
                 << (8 * index);
    }
    return value;
}

void PrintReplayBinaryStats(const FileProbe& probe, std::size_t top, bool as_json) {
    (void)top;
    const std::string head = ReadFilePrefix(probe.path, 32);
    const std::uint16_t version = ReplayFormatVersion(head);
    const std::uint16_t header_size =
        static_cast<std::uint16_t>(static_cast<unsigned char>(head[10]) |
                                   (static_cast<std::uint16_t>(
                                        static_cast<unsigned char>(head[11]))
                                    << 8));
    const std::uint64_t producer = ReadLeU64(head, 24);

    if (as_json) {
        nlohmann::json out;
        out["file"] = probe.path;
        out["size_bytes"] = probe.size;
        out["format"] = "replay-binary";
        out["format_version"] = version;
        out["header_size"] = header_size;
        out["producer_id"] = producer;
        std::printf("%s", RenderJson(out).c_str());
        return;
    }
    std::printf("文件: %s（%s）\n", probe.path.c_str(), FormatBytes(probe.size).c_str());
    std::printf("格式: %s\n", probe.detail.c_str());
    std::printf("记录头: version=%u header_size=%u producer_id=%llu\n",
                static_cast<unsigned>(version), static_cast<unsigned>(header_size),
                static_cast<unsigned long long>(producer));
    std::printf("说明: 事件级统计由 tools/replay 的 `replay` 工具输出（CLI 不重新实现格式解析）。\n");
}

void PrintMigrationBinaryStats(const FileProbe& probe, bool as_json) {
    const std::string head = ReadFilePrefix(probe.path, 128);
    const std::uint32_t version = MigrationFormatVersion(head);
    const std::uint64_t fiber_id = ReadLeU64(head, 44);
    const std::uint64_t stack_size = ReadLeU64(head, 60);
    const std::uint32_t section_count = static_cast<std::uint32_t>(
        static_cast<unsigned char>(head[20]) |
        (static_cast<std::uint32_t>(static_cast<unsigned char>(head[21])) << 8) |
        (static_cast<std::uint32_t>(static_cast<unsigned char>(head[22])) << 16) |
        (static_cast<std::uint32_t>(static_cast<unsigned char>(head[23])) << 24));

    if (as_json) {
        nlohmann::json out;
        out["file"] = probe.path;
        out["size_bytes"] = probe.size;
        out["format"] = "migration-binary";
        out["format_version"] = version;
        out["section_count"] = section_count;
        out["fiber_id"] = fiber_id;
        out["stack_size"] = stack_size;
        std::printf("%s", RenderJson(out).c_str());
        return;
    }
    std::printf("文件: %s（%s）\n", probe.path.c_str(), FormatBytes(probe.size).c_str());
    std::printf("格式: %s\n", probe.detail.c_str());
    std::printf("快照头: version=%u sections=%u fiber_id=%llu stack_size=%s\n",
                static_cast<unsigned>(version), static_cast<unsigned>(section_count),
                static_cast<unsigned long long>(fiber_id),
                FormatBytes(stack_size).c_str());
    std::printf("说明: 用 `migrate restore <file>` 恢复，或用后端的 `verify` 子命令做只读校验。\n");
}

int RunDebugImpl(const std::vector<std::string>& args) {
    const ParsedArgs parsed = ParseArgs(
        args,
        {
            {"--top", "N", "时间线显示前 N 条，默认 20；0 表示全部"},
            {"--json", "", "以 JSON 输出统计"},
        },
        1, 1);
    if (parsed.help_requested) {
        PrintDebugHelp();
        return 0;
    }

    const int top_value = ParseIntOption(parsed.GetOr("--top", "20"), "--top", 0, 100000);
    const auto top = static_cast<std::size_t>(top_value);
    const bool as_json = parsed.Has("--json");
    const FileProbe probe = ProbeFile(parsed.positional.front(), kMaxFileBytes);

    switch (probe.format) {
        case FileFormat::kJson: {
            const std::string content = ReadTextFile(probe.path, kMaxFileBytes);
            const nlohmann::json root = ParseJsonText(content, probe.path, "dump");
            PrintJsonDumpStats(probe, root, top, as_json);
            return 0;
        }
        case FileFormat::kTraceText: {
            const std::string content = ReadTextFile(probe.path, kMaxFileBytes);
            PrintTraceDumpStats(probe, ParseTraceText(content, top), as_json);
            return 0;
        }
        case FileFormat::kIniText: {
            PrintIniDumpStats(probe, as_json);
            return 0;
        }
        case FileFormat::kReplayBinary:
            PrintReplayBinaryStats(probe, top, as_json);
            return 0;
        case FileFormat::kMigrationBinary:
            PrintMigrationBinaryStats(probe, as_json);
            return 0;
        case FileFormat::kUnknown:
            break;
    }
    throw CliError(ExitCode::kFormat,
                   "无法识别的文件格式：'" + probe.path + "'\n  实际：" + probe.detail +
                       "\n  支持 JSON dump、'# go2cpp-trace' 文本、ini dump、"
                       "G2CRPLY 重放记录与 G2CMIG03/G2CMIG02 迁移快照");
}

}  // namespace

// 公开入口只做转调；实现放在匿名命名空间，避免符号外泄。
int RunDump(const std::vector<std::string>& args) { return RunDumpImpl(args); }
int RunStatus(const std::vector<std::string>& args) { return RunStatusImpl(args); }
int RunReplay(const std::vector<std::string>& args) { return RunReplayImpl(args); }
int RunMigrate(const std::vector<std::string>& args) { return RunMigrateImpl(args); }
int RunDebug(const std::vector<std::string>& args) { return RunDebugImpl(args); }

}  // namespace go2cpp::cli
