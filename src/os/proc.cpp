/**
 * @file proc.cpp
 * @brief 进程子层实现：`fork`/`execve`/`wait4`、`/proc` 解析、`Run` 输出泵、
 *        统计与默认配置。
 *
 * ## fork 之后为什么全部用裸 syscall
 *
 * `fork` 在多线程进程里只复制调用线程；子进程若调用任何可能拿锁/分配内存的
 * libc 函数（包括被 Hook 包装的 `read`/`close`），都可能永久死锁。因此
 * `ChildRun` 里只调用 `::syscall(SYS_*)` 与纯栈操作，所有字符串/argv/envp/路径
 * 都在 `fork` 之前准备好。
 *
 * ## exec 失败如何归因
 *
 * 父进程建一条 `O_CLOEXEC` 的错误管道：`execve` 成功后内核自动关闭写端，
 * 父进程读到 EOF（0 字节）即成功；失败时子进程把 `errno` 写进管道再
 * `_exit(127)`，父进程据此返回 `kExecFailed + errno`（而不是只看到一个
 * 神秘的退出码 127）。
 */
#include "go2cpp/os/proc.hpp"

#include "go2cpp/config/config.hpp"
#include "go2cpp/os/channel.hpp"
#include "go2cpp/os/pty.hpp"
#include "raw_io.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <mutex>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <sstream>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

// glibc 的 <unistd.h> 是否声明 environ 因特性宏而异；显式声明一次，避免在
// 函数/匿名命名空间里写 `extern` 时被解析成内部命名空间变量（链接期会变成
// 一个只在本 TU 可见的符号，导致 DT_TEXTREL / undefined reference）。
extern char** environ;

namespace go2cpp::os {

// ===========================================================================
// 错误与状态文本
// ===========================================================================

const char* ToString(ProcErrorCode code) noexcept {
    switch (code) {
        case ProcErrorCode::kOk: return "ok";
        case ProcErrorCode::kInvalidArgument: return "invalid_argument";
        case ProcErrorCode::kSpawnFailed: return "spawn_failed";
        case ProcErrorCode::kExecFailed: return "exec_failed";
        case ProcErrorCode::kWaitFailed: return "wait_failed";
        case ProcErrorCode::kTimeout: return "timeout";
        case ProcErrorCode::kNotRunning: return "not_running";
        case ProcErrorCode::kPermissionDenied: return "permission_denied";
        case ProcErrorCode::kUnsupported: return "unsupported";
        case ProcErrorCode::kResourceLimit: return "resource_limit";
        case ProcErrorCode::kIoFailed: return "io_failed";
        case ProcErrorCode::kClosed: return "closed";
        case ProcErrorCode::kAgain: return "again";
        case ProcErrorCode::kQueueFull: return "queue_full";
        case ProcErrorCode::kChannelClosed: return "channel_closed";
        case ProcErrorCode::kShmFailed: return "shm_failed";
        case ProcErrorCode::kMqFailed: return "mq_failed";
        case ProcErrorCode::kPtyFailed: return "pty_failed";
        case ProcErrorCode::kInternal: return "internal";
    }
    return "unknown";
}

const char* ToString(LifecycleState state) noexcept {
    switch (state) {
        case LifecycleState::kCreated: return "created";
        case LifecycleState::kRunning: return "running";
        case LifecycleState::kTerminating: return "terminating";
        case LifecycleState::kKilled: return "killed";
        case LifecycleState::kReaped: return "reaped";
        case LifecycleState::kFailed: return "failed";
    }
    return "unknown";
}

const char* ToString(ProcEventType type) noexcept {
    switch (type) {
        case ProcEventType::kSpawn: return "spawn";
        case ProcEventType::kSpawnFailed: return "spawn_failed";
        case ProcEventType::kExit: return "exit";
        case ProcEventType::kTimeout: return "timeout";
        case ProcEventType::kKill: return "kill";
        case ProcEventType::kCondition: return "condition";
        case ProcEventType::kQuotaExceeded: return "quota_exceeded";
        case ProcEventType::kReap: return "reap";
        case ProcEventType::kIoError: return "io_error";
    }
    return "unknown";
}

ProcError MakeError(ProcErrorCode code, int system_error, std::string message,
                    std::string context) {
    ProcError error;
    error.code = code;
    error.system_error = system_error;
    error.message = std::move(message);
    error.context = std::move(context);
    return error;
}

ProcError MakeErrnoError(ProcErrorCode code, int system_error, std::string context) {
    char text[256] = {};
    const char* message = ::strerror_r(system_error, text, sizeof(text));
    return MakeError(code, system_error, message != nullptr ? message : "系统错误",
                     std::move(context));
}

std::string ProcError::ToString() const {
    std::string text = "[";
    text += go2cpp::os::ToString(code);
    text += "] ";
    text += message;
    if (system_error != 0) {
        char buffer[256] = {};
        const char* reason = ::strerror_r(system_error, buffer, sizeof(buffer));
        text += " (errno=";
        text += std::to_string(system_error);
        text += ": ";
        text += reason != nullptr ? reason : "?";
        text += ")";
    }
    if (!context.empty()) {
        text += " @";
        text += context;
    }
    return text;
}

const char* SignalName(int signal) noexcept {
    switch (signal) {
        case 0: return "SIG0";
        case SIGHUP: return "SIGHUP";
        case SIGINT: return "SIGINT";
        case SIGQUIT: return "SIGQUIT";
        case SIGILL: return "SIGILL";
        case SIGTRAP: return "SIGTRAP";
        case SIGABRT: return "SIGABRT";
        case SIGBUS: return "SIGBUS";
        case SIGFPE: return "SIGFPE";
        case SIGKILL: return "SIGKILL";
        case SIGUSR1: return "SIGUSR1";
        case SIGSEGV: return "SIGSEGV";
        case SIGUSR2: return "SIGUSR2";
        case SIGPIPE: return "SIGPIPE";
        case SIGALRM: return "SIGALRM";
        case SIGTERM: return "SIGTERM";
        case SIGCHLD: return "SIGCHLD";
        case SIGCONT: return "SIGCONT";
        case SIGSTOP: return "SIGSTOP";
        case SIGTSTP: return "SIGTSTP";
        case SIGTTIN: return "SIGTTIN";
        case SIGTTOU: return "SIGTTOU";
        default: break;
    }
    static thread_local char unknown[32];
    std::snprintf(unknown, sizeof(unknown), "SIG%d", signal);
    return unknown;
}

std::string ExitStatus::ToString() const {
    if (!reaped) {
        return timed_out ? "not_reaped(timed_out)" : "not_reaped";
    }
    std::string text;
    if (exited) {
        text = "exited(" + std::to_string(exit_code) + ")";
    } else if (signaled) {
        text = "signaled(";
        text += SignalName(signal);
        text += ")";
    } else if (stopped) {
        text = "stopped(";
        text += SignalName(signal);
        text += ")";
    } else {
        text = "unknown";
    }
    if (timed_out) {
        text += " timed_out";
    }
    return text;
}

// ===========================================================================
// 全局状态：统计、事件/行 sink、默认日志
// ===========================================================================

namespace {

struct GlobalState final {
    std::atomic<std::uint64_t> spawned{0};
    std::atomic<std::uint64_t> spawn_failed{0};
    std::atomic<std::uint64_t> exited{0};
    std::atomic<std::uint64_t> signaled{0};
    std::atomic<std::uint64_t> timed_out{0};
    std::atomic<std::uint64_t> killed{0};
    std::atomic<std::uint64_t> conditions{0};
    std::atomic<std::uint64_t> reaped{0};
    std::atomic<std::uint64_t> live{0};
    std::atomic<bool> default_logging{false};

    std::mutex sink_mutex;
    ProcEventSink event_sink;
    StderrLineSink stderr_sink;
};

GlobalState& State() {
    static GlobalState state;
    return state;
}

void EmitEvent(const ProcEvent& event) {
    ProcEventSink sink;
    {
        std::lock_guard<std::mutex> lock(State().sink_mutex);
        sink = State().event_sink;
    }
    if (!sink) {
        return;
    }
    try {
        sink(event);
    } catch (...) {
        // 观测是扩展点：一个坏 sink 不能把进程操作带崩。
    }
}

void EmitStderrLine(const StderrLine& line) {
    StderrLineSink sink;
    {
        std::lock_guard<std::mutex> lock(State().sink_mutex);
        sink = State().stderr_sink;
    }
    if (!sink) {
        return;
    }
    try {
        sink(line);
    } catch (...) {
    }
}

void DefaultEventLog(const ProcEvent& event) {
    std::fprintf(stderr,
                 "[os.proc] event=%s pid=%d pgid=%d exit=%d signal=%d reason=%s\n",
                 ToString(event.type), static_cast<int>(event.pid),
                 static_cast<int>(event.pgid), event.exit_code, event.signal,
                 event.reason.c_str());
}

void DefaultStderrLog(const StderrLine& line) {
    std::fprintf(stderr, "[os.proc stderr pid=%d pgid=%d] %s%s\n",
                 static_cast<int>(line.pid), static_cast<int>(line.pgid),
                 line.text.c_str(), line.partial ? " <partial>" : "");
}

void EmitSpawnEvent(pid_t pid, pid_t pgid) {
    ProcEvent event;
    event.type = ProcEventType::kSpawn;
    event.pid = pid;
    event.pgid = pgid;
    event.when = std::chrono::system_clock::now();
    EmitEvent(event);
}

void EmitExitEvents(pid_t pid, pid_t pgid, const ExitStatus& status) {
    ProcEvent event;
    event.type = ProcEventType::kExit;
    event.pid = pid;
    event.pgid = pgid;
    event.exit_code = status.exit_code;
    event.signal = status.signal;
    event.when = std::chrono::system_clock::now();
    EmitEvent(event);

    ProcEvent reap;
    reap.type = ProcEventType::kReap;
    reap.pid = pid;
    reap.pgid = pgid;
    reap.exit_code = status.exit_code;
    reap.signal = status.signal;
    reap.when = event.when;
    EmitEvent(reap);
}

void RecordExitStats(const ExitStatus& status) {
    if (status.exited) {
        State().exited.fetch_add(1, std::memory_order_relaxed);
    } else if (status.signaled) {
        State().signaled.fetch_add(1, std::memory_order_relaxed);
    }
    State().reaped.fetch_add(1, std::memory_order_relaxed);
    const std::uint64_t live = State().live.load(std::memory_order_relaxed);
    if (live > 0) {
        State().live.fetch_sub(1, std::memory_order_relaxed);
    }
}

// ===========================================================================
// /proc 与文件工具
// ===========================================================================

std::size_t PageSize() {
    static const std::size_t size = [] {
        const long value = ::sysconf(_SC_PAGESIZE);
        return value > 0 ? static_cast<std::size_t>(value) : 4096u;
    }();
    return size;
}

long ClockTicks() {
    static const long ticks = [] {
        const long value = ::sysconf(_SC_CLK_TCK);
        return value > 0 ? value : 100L;
    }();
    return ticks;
}

time_t BootTime() {
    static std::atomic<time_t> cached{0};
    time_t value = cached.load(std::memory_order_acquire);
    if (value != 0) {
        return value;
    }
    const int fd = static_cast<int>(::syscall(SYS_openat, AT_FDCWD, "/proc/stat",
                                              O_RDONLY | O_CLOEXEC, 0));
    if (fd < 0) {
        return 0;
    }
    std::string content;
    char buffer[4096];
    for (;;) {
        const ssize_t count = detail::RawRead(fd, buffer, sizeof(buffer));
        if (count > 0) {
            content.append(buffer, static_cast<std::size_t>(count));
            continue;
        }
        if (count == 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        break;
    }
    detail::RawClose(fd);
    const std::string key = "\nbtime ";
    std::size_t position = content.find(key);
    if (position == std::string::npos) {
        position = content.find("btime ");
        if (position == std::string::npos) {
            return 0;
        }
        position += 6;
    } else {
        position += key.size();
    }
    value = static_cast<time_t>(std::strtoll(content.c_str() + position, nullptr, 10));
    cached.store(value, std::memory_order_release);
    return value;
}

bool ReadProcStat(pid_t pid, ProcessInfo* info) {
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/%d/stat", static_cast<int>(pid));
    const int fd = static_cast<int>(::syscall(SYS_openat, AT_FDCWD, path,
                                              O_RDONLY | O_CLOEXEC, 0));
    if (fd < 0) {
        return false;
    }
    std::string content;
    char buffer[2048];
    for (;;) {
        const ssize_t count = detail::RawRead(fd, buffer, sizeof(buffer));
        if (count > 0) {
            content.append(buffer, static_cast<std::size_t>(count));
            continue;
        }
        if (count == 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        break;
    }
    detail::RawClose(fd);
    const std::size_t close = content.rfind(')');
    if (close == std::string::npos || close + 2 >= content.size()) {
        return false;
    }
    std::istringstream stream(content.substr(close + 2));
    char state = '?';
    long long ppid = 0;
    long long pgrp = 0;
    long long session = 0;
    long long tty = 0;
    long long tpgid = 0;
    unsigned long long flags = 0;
    unsigned long long minflt = 0;
    unsigned long long cminflt = 0;
    unsigned long long majflt = 0;
    unsigned long long cmajflt = 0;
    unsigned long long utime = 0;
    unsigned long long stime = 0;
    long long cutime = 0;
    long long cstime = 0;
    long long priority = 0;
    long long nice = 0;
    long long threads = 0;
    long long itreal = 0;
    long long starttime = 0;
    long long vsize = 0;
    long long rss = 0;
    stream >> state >> ppid >> pgrp >> session >> tty >> tpgid >> flags >> minflt >>
        cminflt >> majflt >> cmajflt >> utime >> stime >> cutime >> cstime >> priority >>
        nice >> threads >> itreal >> starttime >> vsize >> rss;
    if (stream.fail()) {
        return false;
    }
    const long ticks = ClockTicks();
    info->pid = pid;
    info->ppid = static_cast<pid_t>(ppid);
    info->pgid = static_cast<pid_t>(pgrp);
    info->state = state;
    info->cpu_user_ms = static_cast<std::uint64_t>(utime) * 1000u /
                        static_cast<std::uint64_t>(ticks);
    info->cpu_system_ms = static_cast<std::uint64_t>(stime) * 1000u /
                          static_cast<std::uint64_t>(ticks);
    info->start_ticks = static_cast<std::uint64_t>(starttime);
    info->vsize_bytes = vsize > 0 ? static_cast<std::uint64_t>(vsize) : 0;
    info->rss_bytes = rss > 0 ? static_cast<std::uint64_t>(rss) * PageSize() : 0;
    info->threads = static_cast<int>(threads);
    info->alive = state != 'Z' && state != 'X';
    info->zombie = state == 'Z';
    const time_t boot = BootTime();
    const auto seconds = static_cast<std::time_t>(starttime / ticks);
    info->started_at = std::chrono::system_clock::from_time_t(boot + seconds);
    return true;
}

std::size_t CountDirectoryEntries(const std::string& path) {
    DIR* directory = ::opendir(path.c_str());
    if (directory == nullptr) {
        return 0;
    }
    std::size_t count = 0;
    while (struct dirent* entry = ::readdir(directory)) {
        const char* name = entry->d_name;
        if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) {
            continue;
        }
        ++count;
    }
    ::closedir(directory);
    return count;
}

std::size_t CountFdsOf(pid_t pid) {
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/%d/fd", static_cast<int>(pid));
    return CountDirectoryEntries(path);
}

int ReadIntConfig(const char* name, int fallback) {
    auto variable = go2cpp::config::Config::Instance().Lookup<int>(name, fallback, "");
    return variable ? variable->GetValue() : fallback;
}

bool ReadBoolConfig(const char* name, bool fallback) {
    auto variable = go2cpp::config::Config::Instance().Lookup<bool>(name, fallback, "");
    return variable ? variable->GetValue() : fallback;
}

std::string ReadStringConfig(const char* name, const char* fallback) {
    auto variable = go2cpp::config::Config::Instance().Lookup<std::string>(name, fallback, "");
    return variable ? variable->GetValue() : std::string(fallback);
}

std::uint64_t ReadUInt64Config(const char* name, std::uint64_t fallback) {
    auto variable = go2cpp::config::Config::Instance().Lookup<std::int64_t>(
        name, static_cast<std::int64_t>(fallback), "");
    if (!variable) {
        return fallback;
    }
    const std::int64_t value = variable->GetValue();
    return value > 0 ? static_cast<std::uint64_t>(value) : 0;
}

ProcErrorCode SpawnErrnoCode(int error) {
    switch (error) {
        case EMFILE:
        case ENFILE:
        case EAGAIN:
        case ENOMEM:
            return ProcErrorCode::kResourceLimit;
        case EACCES:
        case EPERM:
            return ProcErrorCode::kPermissionDenied;
        default:
            return ProcErrorCode::kSpawnFailed;
    }
}

std::string BuildExecPath(const ProcessOptions& options) {
    const std::string& program = options.argv[0];
    if (!options.use_path_search || program.find('/') != std::string::npos) {
        return program;
    }
    std::string path_value;
    const auto environment = [] {
        std::vector<std::string> entries;
        for (char** item = ::environ; item != nullptr && *item != nullptr; ++item) {
            entries.emplace_back(*item);
        }
        return entries;
    }();
    for (const auto& entry : environment) {
        if (entry.rfind("PATH=", 0) == 0) {
            path_value = entry.substr(5);
            break;
        }
    }
    if (path_value.empty()) {
        path_value = "/usr/local/bin:/usr/bin:/bin";
    }
    std::size_t begin = 0;
    while (begin <= path_value.size()) {
        const std::size_t end = path_value.find(':', begin);
        const std::string directory =
            path_value.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
        if (!directory.empty()) {
            const std::string candidate = directory + "/" + program;
            if (::access(candidate.c_str(), X_OK) == 0) {
                return candidate;
            }
        }
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1;
    }
    return {};
}

std::vector<std::string> BuildEnvironment(const ProcessOptions& options) {
    std::vector<std::string> result;
    if (!options.env_clear) {
        for (char** item = ::environ; item != nullptr && *item != nullptr; ++item) {
            result.emplace_back(*item);
        }
    }
    for (const auto& item : options.env) {
        const std::size_t equals = item.find('=');
        const std::string key = equals == std::string::npos ? item : item.substr(0, equals);
        if (key.empty()) {
            continue;
        }
        std::string value;
        if (equals != std::string::npos) {
            value = item.substr(equals + 1);
        } else {
            const char* inherited = ::getenv(key.c_str());
            if (inherited == nullptr) {
                continue;
            }
            value = inherited;
        }
        const std::string entry = key + "=" + value;
        bool replaced = false;
        for (auto& existing : result) {
            if (existing.size() > key.size() && existing.compare(0, key.size() + 1,
                                                                 key + "=") == 0) {
                existing = entry;
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            result.push_back(entry);
        }
    }
    return result;
}

// ===========================================================================
// 子进程执行（fork 之后只允许裸 syscall）
// ===========================================================================

struct ChildPlan final {
    const char* exec_path{nullptr};
    char* const* argv{nullptr};
    char* const* envp{nullptr};
    int stdin_fd{-1};
    int stdout_fd{-1};
    int stderr_fd{-1};
    int error_fd{-1};
    const char* cwd{nullptr};
    bool new_session{false};
    bool set_umask{false};
    mode_t umask_value{022};
    bool set_nice{false};
    int nice_value{0};
    bool set_affinity{false};
    cpu_set_t affinity{};
    bool set_sched{false};
    int sched_policy{0};
    int sched_priority{0};
    bool set_groups{false};
    const gid_t* groups{nullptr};
    std::size_t group_count{0};
    bool set_gid{false};
    gid_t gid_value{0};
    bool set_uid{false};
    uid_t uid_value{0};
    const RLimitSetting* rlimits{nullptr};
    std::size_t rlimit_count{0};
    bool set_oom{false};
    char oom_text[16]{};
    bool close_extra_fds{true};
    unsigned close_limit{1024};
};

[[noreturn]] void ChildFail(int error_fd) noexcept {
    const int saved = errno;
    (void)::syscall(SYS_write, error_fd, &saved, sizeof(saved));
    (void)::syscall(SYS_exit_group, 127);
    __builtin_unreachable();
}

[[noreturn]] void ChildRun(const ChildPlan& plan) noexcept {
    int error_fd = plan.error_fd;
    if (error_fd < 0) {
        error_fd = 3;
    }
    // error fd 可能在 0..2（父进程 stdio 关闭时 pipe2 会复用），先挪到 >=10。
    if (error_fd < 3) {
        const int moved = static_cast<int>(
            ::syscall(SYS_fcntl, error_fd, F_DUPFD_CLOEXEC, 10L));
        if (moved < 0) {
            (void)::syscall(SYS_exit_group, 127);
            __builtin_unreachable();
        }
        error_fd = moved;
    }

    // 把三条源 fd 挪到 >=10，避免 dup2 顺序互相覆盖（源 fd 可能恰好是 0/1/2）。
    int sources[3] = {plan.stdin_fd, plan.stdout_fd, plan.stderr_fd};
    int temporary[3] = {-1, -1, -1};
    for (int index = 0; index < 3; ++index) {
        if (sources[index] < 0) {
            continue;
        }
        if (sources[index] <= 2) {
            temporary[index] = static_cast<int>(
                ::syscall(SYS_fcntl, sources[index], F_DUPFD_CLOEXEC, 10L));
            if (temporary[index] < 0) {
                ChildFail(error_fd);
            }
        } else {
            temporary[index] = sources[index];
        }
    }
    for (int index = 0; index < 3; ++index) {
        if (temporary[index] >= 0 &&
            ::syscall(SYS_dup2, temporary[index], index) < 0) {
            ChildFail(error_fd);
        }
    }
    // error fd 固定到 3，close_range 从 4 开始，不会误关它。
    if (error_fd != 3) {
        if (::syscall(SYS_dup3, error_fd, 3, O_CLOEXEC) < 0) {
            if (::syscall(SYS_dup2, error_fd, 3) < 0) {
                (void)::syscall(SYS_exit_group, 127);
                __builtin_unreachable();
            }
            (void)::syscall(SYS_fcntl, 3, F_SETFD, FD_CLOEXEC);
        }
        error_fd = 3;
    }
    if (plan.close_extra_fds) {
        if (::syscall(SYS_close_range, 4u, ~0u, 0u) < 0) {
            for (unsigned fd = 4; fd < plan.close_limit; ++fd) {
                (void)::syscall(SYS_close, fd);
            }
        }
    }

    if (plan.new_session && ::syscall(SYS_setsid) < 0) {
        ChildFail(error_fd);
    }
    if (plan.cwd != nullptr && ::syscall(SYS_chdir, plan.cwd) < 0) {
        ChildFail(error_fd);
    }
    if (plan.set_umask) {
        (void)::syscall(SYS_umask, static_cast<unsigned>(plan.umask_value));
    }
    for (std::size_t index = 0; index < plan.rlimit_count; ++index) {
        struct ::rlimit limit {};
        limit.rlim_cur = static_cast<rlim_t>(plan.rlimits[index].soft);
        limit.rlim_max = static_cast<rlim_t>(plan.rlimits[index].hard);
        if (::syscall(SYS_setrlimit, plan.rlimits[index].resource, &limit) < 0) {
            ChildFail(error_fd);
        }
    }
    if (plan.set_nice &&
        ::syscall(SYS_setpriority, PRIO_PROCESS, 0, plan.nice_value) < 0) {
        ChildFail(error_fd);
    }
    if (plan.set_affinity &&
        ::syscall(SYS_sched_setaffinity, 0, sizeof(cpu_set_t), &plan.affinity) < 0) {
        ChildFail(error_fd);
    }
    if (plan.set_sched) {
        struct sched_param parameter {};
        parameter.sched_priority = plan.sched_priority;
        if (::syscall(SYS_sched_setscheduler, 0, plan.sched_policy, &parameter) < 0) {
            ChildFail(error_fd);
        }
    }
    if (plan.set_oom) {
        const int fd = static_cast<int>(::syscall(
            SYS_openat, AT_FDCWD, "/proc/self/oom_score_adj", O_WRONLY | O_CLOEXEC, 0));
        if (fd < 0) {
            ChildFail(error_fd);
        }
        const std::size_t length = std::strlen(plan.oom_text);
        if (::syscall(SYS_write, fd, plan.oom_text, length) < 0) {
            (void)::syscall(SYS_close, fd);
            ChildFail(error_fd);
        }
        (void)::syscall(SYS_close, fd);
    }
    if (plan.set_groups &&
        ::syscall(SYS_setgroups, plan.group_count, plan.groups) < 0) {
        ChildFail(error_fd);
    }
    if (plan.set_gid && ::syscall(SYS_setgid, plan.gid_value) < 0) {
        ChildFail(error_fd);
    }
    if (plan.set_uid && ::syscall(SYS_setuid, plan.uid_value) < 0) {
        ChildFail(error_fd);
    }

    (void)::syscall(SYS_execve, plan.exec_path, plan.argv, plan.envp);
    ChildFail(error_fd);
}

// ===========================================================================
// 真实后端
// ===========================================================================

struct FdOwner final {
    std::vector<int> owned;

    ~FdOwner() {
        for (const int fd : owned) {
            if (fd >= 0) {
                detail::RawClose(fd);
            }
        }
    }

    int Keep(int fd) {
        owned.push_back(fd);
        return fd;
    }

    int Release(int fd) {
        for (auto& item : owned) {
            if (item == fd) {
                item = -1;
                break;
            }
        }
        return fd;
    }
};

class RealBackend final : public ProcessBackend {
public:
    Result<SpawnedProcess> Spawn(const ProcessOptions& options) override;
    Result<ExitStatus> Wait(pid_t pid, std::chrono::milliseconds timeout) override;
    Result<void> Kill(pid_t pid, int signal) override;
    Result<ProcessInfo> Query(pid_t pid) override;
    bool Alive(pid_t pid) override;

private:
    static Result<ExitStatus> WaitFor(pid_t pid, std::chrono::milliseconds timeout);
};

Result<SpawnedProcess> RealBackend::Spawn(const ProcessOptions& options) {
    if (options.argv.empty() || options.argv[0].empty()) {
        return MakeError(ProcErrorCode::kInvalidArgument, EINVAL, "argv 不能为空",
                         "Process::Spawn");
    }
    if (options.nice < -20 || options.nice > 19) {
        return MakeError(ProcErrorCode::kInvalidArgument, EINVAL,
                         "nice 必须在 -20..19", "Process::Spawn");
    }
    if (options.sched_policy != -1 && options.sched_policy != SCHED_OTHER &&
        options.sched_policy != SCHED_BATCH) {
        return MakeError(ProcErrorCode::kInvalidArgument, EINVAL,
                         "只支持 SCHED_OTHER/SCHED_BATCH", "Process::Spawn");
    }
    if (options.oom_score_adj != -2147483647 - 1 &&
        (options.oom_score_adj < -1000 || options.oom_score_adj > 1000)) {
        return MakeError(ProcErrorCode::kInvalidArgument, EINVAL,
                         "oom_score_adj 必须在 -1000..1000", "Process::Spawn");
    }
    if (options.use_pty && (options.pty_columns <= 0 || options.pty_rows <= 0)) {
        return MakeError(ProcErrorCode::kInvalidArgument, EINVAL,
                         "PTY 行列必须为正", "Process::Spawn");
    }

    const std::string exec_path = BuildExecPath(options);
    if (exec_path.empty()) {
        return MakeError(ProcErrorCode::kExecFailed, ENOENT,
                         "在 PATH 中找不到可执行文件: " + options.argv[0],
                         "Process::Spawn");
    }

    std::vector<std::string> environment = BuildEnvironment(options);
    std::vector<char*> envp;
    envp.reserve(environment.size() + 1);
    for (auto& item : environment) {
        envp.push_back(item.data());
    }
    envp.push_back(nullptr);
    std::vector<char*> argv;
    argv.reserve(options.argv.size() + 1);
    for (const auto& item : options.argv) {
        argv.push_back(const_cast<char*>(item.c_str()));
    }
    argv.push_back(nullptr);

    FdOwner owner;
    std::vector<int> child_fds;
    int child_stdin = -1;
    int child_stdout = -1;
    int child_stderr = -1;
    int parent_stdin = -1;
    int parent_stdout = -1;
    int parent_stderr = -1;
    bool use_pty = options.use_pty;

    Pty pty;
    if (use_pty) {
        auto opened = Pty::Open(options.pty_columns, options.pty_rows, false, &pty);
        if (!opened.ok()) {
            return opened.error();
        }
        child_stdin = pty.slave_fd();
        child_stdout = pty.slave_fd();
        child_stderr = pty.slave_fd();
        // master 必须移交给 owner：Pty 对象在函数返回时析构，若不 Release
        // 会把刚交给 Process 的 fd 关掉（表现为通道立即 EOF）。
        const int master = pty.ReleaseMaster();
        parent_stdin = master;
        parent_stdout = master;
        parent_stderr = master;
        owner.Keep(master);
    } else {
        const auto setup_stream = [&](StdioMode mode, int target, const std::string& path,
                                      bool append, int* child_fd, int* parent_fd) -> ProcError {
            *child_fd = -1;
            *parent_fd = -1;
            switch (mode) {
                case StdioMode::kPipe: {
                    int fds[2] = {-1, -1};
                    if (detail::RawPipe2(fds, O_CLOEXEC) != 0) {
                        return MakeErrnoError(SpawnErrnoCode(errno), errno, "pipe2");
                    }
                    owner.Keep(fds[0]);
                    owner.Keep(fds[1]);
                    if (target == STDIN_FILENO) {
                        *child_fd = fds[0];
                        *parent_fd = fds[1];
                    } else {
                        *child_fd = fds[1];
                        *parent_fd = fds[0];
                    }
                    return MakeError(ProcErrorCode::kOk, 0, "");
                }
                case StdioMode::kInherit:
                    *child_fd = target;
                    return MakeError(ProcErrorCode::kOk, 0, "");
                case StdioMode::kNull: {
                    const int flags = target == STDIN_FILENO ? O_RDONLY : O_WRONLY;
                    const int fd = static_cast<int>(::syscall(
                        SYS_openat, AT_FDCWD, "/dev/null", flags | O_CLOEXEC, 0));
                    if (fd < 0) {
                        return MakeErrnoError(SpawnErrnoCode(errno), errno, "open(/dev/null)");
                    }
                    owner.Keep(fd);
                    *child_fd = fd;
                    return MakeError(ProcErrorCode::kOk, 0, "");
                }
                case StdioMode::kFile: {
                    int flags = O_CLOEXEC;
                    if (target == STDIN_FILENO) {
                        flags |= O_RDONLY;
                    } else {
                        flags |= O_WRONLY | O_CREAT;
                        flags |= append ? O_APPEND : O_TRUNC;
                    }
                    const int fd = static_cast<int>(::syscall(
                        SYS_openat, AT_FDCWD, path.c_str(), flags, 0644));
                    if (fd < 0) {
                        return MakeErrnoError(SpawnErrnoCode(errno), errno,
                                              "open(" + path + ")");
                    }
                    owner.Keep(fd);
                    *child_fd = fd;
                    return MakeError(ProcErrorCode::kOk, 0, "");
                }
                case StdioMode::kPty:
                    return MakeError(ProcErrorCode::kInvalidArgument, EINVAL,
                                     "StdioMode::kPty 只能通过 use_pty 使用",
                                     "Process::Spawn");
            }
            return MakeError(ProcErrorCode::kInternal, 0, "未知 stdio 模式", "Process::Spawn");
        };

        ProcError error = setup_stream(options.stdin_mode, STDIN_FILENO,
                                       options.stdin_path, false, &child_stdin,
                                       &parent_stdin);
        if (!error.ok()) {
            return error;
        }
        error = setup_stream(options.stdout_mode, STDOUT_FILENO, options.stdout_path,
                             options.stdout_append, &child_stdout, &parent_stdout);
        if (!error.ok()) {
            return error;
        }
        if (options.stderr_to_stdout) {
            child_stderr = child_stdout;
        } else {
            error = setup_stream(options.stderr_mode, STDERR_FILENO, options.stderr_path,
                                 options.stderr_append, &child_stderr, &parent_stderr);
            if (!error.ok()) {
                return error;
            }
        }
    }

    int error_pipe[2] = {-1, -1};
    if (detail::RawPipe2(error_pipe, O_CLOEXEC) != 0) {
        return MakeErrnoError(SpawnErrnoCode(errno), errno, "pipe2(error)");
    }
    owner.Keep(error_pipe[0]);
    owner.Keep(error_pipe[1]);
    const int child_error = error_pipe[1];
    const int parent_error = error_pipe[0];

    ChildPlan plan;
    plan.exec_path = exec_path.c_str();
    plan.argv = argv.data();
    plan.envp = envp.data();
    plan.stdin_fd = child_stdin;
    plan.stdout_fd = child_stdout;
    plan.stderr_fd = child_stderr;
    plan.error_fd = child_error;
    plan.cwd = options.cwd.empty() ? nullptr : options.cwd.c_str();
    plan.new_session = options.new_session;
    plan.set_umask = options.set_umask;
    plan.umask_value = options.umask_value;
    plan.set_nice = options.nice != 0;
    plan.nice_value = options.nice;
    plan.set_affinity = !options.cpu_affinity.empty();
    CPU_ZERO(&plan.affinity);
    for (const int cpu : options.cpu_affinity) {
        if (cpu < 0 || cpu >= CPU_SETSIZE) {
            return MakeError(ProcErrorCode::kInvalidArgument, EINVAL,
                             "CPU 亲和性索引越界", "Process::Spawn");
        }
        CPU_SET(cpu, &plan.affinity);
    }
    plan.set_sched = options.sched_policy != -1;
    plan.sched_policy = options.sched_policy;
    plan.sched_priority = options.sched_priority;
    plan.set_groups = options.set_groups;
    plan.groups = options.supplementary_groups.empty()
                      ? nullptr
                      : options.supplementary_groups.data();
    plan.group_count = options.supplementary_groups.size();
    plan.set_gid = options.gid.has_value();
    plan.gid_value = options.gid.value_or(0);
    plan.set_uid = options.uid.has_value();
    plan.uid_value = options.uid.value_or(0);
    plan.rlimits = options.rlimits.empty() ? nullptr : options.rlimits.data();
    plan.rlimit_count = options.rlimits.size();
    plan.set_oom = options.oom_score_adj != -2147483647 - 1;
    if (plan.set_oom) {
        std::snprintf(plan.oom_text, sizeof(plan.oom_text), "%d", options.oom_score_adj);
    }
    plan.close_extra_fds = options.close_extra_fds;
    const long open_max = ::sysconf(_SC_OPEN_MAX);
    plan.close_limit = static_cast<unsigned>(
        std::min<long>(open_max > 0 ? open_max : 1024, 65536));

    const pid_t pid = ::fork();
    if (pid < 0) {
        return MakeErrnoError(SpawnErrnoCode(errno), errno, "fork");
    }
    if (pid == 0) {
        // 子进程：关闭父进程侧 fd（0/1/2 之外的），保留子进程要用的。
        if (parent_stdin > 2 && parent_stdin != child_stdin) {
            (void)detail::RawClose(parent_stdin);
        }
        if (parent_stdout > 2 && parent_stdout != child_stdout &&
            parent_stdout != parent_stdin) {
            (void)detail::RawClose(parent_stdout);
        }
        if (parent_stderr > 2 && parent_stderr != child_stderr &&
            parent_stderr != parent_stdout && parent_stderr != parent_stdin) {
            (void)detail::RawClose(parent_stderr);
        }
        (void)detail::RawClose(parent_error);
        ChildRun(plan);
    }

    // 父进程：关闭子进程侧 fd，保留父侧。
    if (use_pty) {
        // PTY：child_stdin/child_stdout/child_stderr 是**同一个从端 fd**。
        // 这里必须只关闭一次：如果按三条流各关一次，第 2、3 次 close 在
        // 单线程下只是 EBADF；但在并发 Spawn 时，第一次 close 释放的
        // fd 编号可能立刻被另一个线程的 posix_openpt/open 复用，随后
        // 的多余 close 就会误关那个线程刚打开的 fd（表现为 master 读取
        // EBADF 或通道被 closed，见 tests/os/test_os_pty.cpp 场景 4）。
        // ReleaseSlave() 把所有权转移出来，Pty 析构不再重复关闭。
        const int pty_slave = pty.ReleaseSlave();
        if (pty_slave > 2) {
            detail::RawClose(pty_slave);
        }
    } else {
        if (child_stdin > 2 && child_stdin != parent_stdin) {
            detail::RawClose(child_stdin);
            owner.Release(child_stdin);
        }
        if (child_stdout > 2 && child_stdout != parent_stdout) {
            detail::RawClose(child_stdout);
            owner.Release(child_stdout);
        }
        if (child_stderr > 2 && child_stderr != parent_stderr && child_stderr != child_stdout) {
            detail::RawClose(child_stderr);
            owner.Release(child_stderr);
        }
    }
    detail::RawClose(child_error);
    owner.Release(child_error);

    int child_errno = 0;
    std::size_t received = 0;
    while (received < sizeof(child_errno)) {
        const ssize_t count = detail::RawRead(parent_error,
                                               reinterpret_cast<char*>(&child_errno) + received,
                                               sizeof(child_errno) - received);
        if (count > 0) {
            received += static_cast<std::size_t>(count);
            continue;
        }
        if (count == 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        break;
    }
    detail::RawClose(parent_error);
    owner.Release(parent_error);
    if (received == sizeof(child_errno)) {
        int status = 0;
        while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
        return MakeError(ProcErrorCode::kExecFailed, child_errno,
                         "execve 失败: " + exec_path, "Process::Spawn");
    }

    // 父侧 fd 设为非阻塞；FdChannel 构造时还会再确认一次（幂等）。
    for (const int fd : {parent_stdin, parent_stdout, parent_stderr}) {
        if (fd >= 0) {
            (void)detail::SetNonBlocking(fd);
        }
    }

    SpawnedProcess spawned;
    spawned.pid = pid;
    spawned.ppid = ::getpid();
    spawned.pgid = ::getpgid(pid);
    if (spawned.pgid < 0) {
        spawned.pgid = pid;
    }
    spawned.stdin_fd = parent_stdin;
    spawned.stdout_fd = parent_stdout;
    spawned.stderr_fd = parent_stderr;
    spawned.pty = use_pty;
    spawned.started_monotonic = std::chrono::steady_clock::now();
    spawned.started_wall = std::chrono::system_clock::now();

    ProcessInfo info;
    if (ReadProcStat(pid, &info)) {
        spawned.started_wall = info.started_at;
        spawned.ppid = info.ppid;
        spawned.pgid = info.pgid;
    }
    if (parent_stdin >= 0) {
        owner.Release(parent_stdin);
    }
    if (parent_stdout >= 0) {
        owner.Release(parent_stdout);
    }
    if (parent_stderr >= 0) {
        owner.Release(parent_stderr);
    }
    return spawned;
}

Result<ExitStatus> RealBackend::WaitFor(pid_t pid, std::chrono::milliseconds timeout) {
    if (pid <= 0) {
        return MakeError(ProcErrorCode::kInvalidArgument, EINVAL, "pid 无效", "wait");
    }
    const bool forever = timeout.count() < 0;
    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + (forever ? std::chrono::milliseconds(0) : timeout);
    int pidfd = -1;
#ifdef SYS_pidfd_open
    pidfd = static_cast<int>(::syscall(SYS_pidfd_open, pid, 0));
#endif
    for (;;) {
        int status = 0;
        const pid_t result = ::waitpid(pid, &status, WNOHANG);
        if (result == pid) {
            if (pidfd >= 0) {
                detail::RawClose(pidfd);
            }
            ExitStatus exit;
            exit.reaped = true;
            if (WIFEXITED(status)) {
                exit.exited = true;
                exit.exit_code = WEXITSTATUS(status);
            }
            if (WIFSIGNALED(status)) {
                exit.signaled = true;
                exit.signal = WTERMSIG(status);
                exit.core_dumped = WCOREDUMP(status) != 0;
            }
            if (WIFSTOPPED(status)) {
                exit.stopped = true;
                exit.signal = WSTOPSIG(status);
            }
            return exit;
        }
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (pidfd >= 0) {
                detail::RawClose(pidfd);
            }
            const ProcErrorCode code =
                errno == ECHILD ? ProcErrorCode::kWaitFailed : ProcErrorCode::kWaitFailed;
            return MakeErrnoError(code, errno, "waitpid");
        }
        if (!forever && std::chrono::steady_clock::now() >= deadline) {
            if (pidfd >= 0) {
                detail::RawClose(pidfd);
            }
            return MakeError(ProcErrorCode::kTimeout, ETIMEDOUT, "等待子进程退出超时",
                             "wait");
        }
        if (pidfd >= 0) {
            struct pollfd descriptor {};
            descriptor.fd = pidfd;
            descriptor.events = POLLIN;
            int wait_ms = 50;
            if (!forever) {
                const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - std::chrono::steady_clock::now());
                wait_ms = static_cast<int>(std::max<std::int64_t>(0, remaining.count()));
            }
            const int ready = detail::RawPoll(&descriptor, 1, wait_ms);
            if (ready < 0 && errno != EINTR) {
                detail::RawClose(pidfd);
                pidfd = -1;
                continue;
            }
            continue;
        }
        // 内核不支持 pidfd 时的回退：1ms 睡眠轮询（有界忙等，文档写明）。
        (void)detail::RawNanosleep(0, 1000 * 1000);
    }
}

Result<ExitStatus> RealBackend::Wait(pid_t pid, std::chrono::milliseconds timeout) {
    return WaitFor(pid, timeout);
}

Result<void> RealBackend::Kill(pid_t pid, int signal) {
    if (pid == 0 || signal <= 0) {
        return MakeError(ProcErrorCode::kInvalidArgument, EINVAL, "pid/signal 无效", "kill");
    }
    for (;;) {
        if (::kill(pid, signal) == 0) {
            return OkResult();
        }
        if (errno == EINTR) {
            continue;
        }
        const int error = errno;
        if (error == ESRCH) {
            return MakeError(ProcErrorCode::kNotRunning, error, "进程不存在", "kill");
        }
        if (error == EPERM) {
            return MakeError(ProcErrorCode::kPermissionDenied, error, "无权限发送信号", "kill");
        }
        return MakeErrnoError(ProcErrorCode::kIoFailed, error, "kill");
    }
}

Result<ProcessInfo> RealBackend::Query(pid_t pid) {
    ProcessInfo info;
    if (!ReadProcStat(pid, &info)) {
        return MakeErrnoError(errno == 0 ? ProcErrorCode::kNotRunning : ProcErrorCode::kIoFailed,
                              errno == 0 ? ESRCH : errno, "/proc/<pid>/stat");
    }
    info.started_monotonic = std::chrono::steady_clock::now();
    return info;
}

bool RealBackend::Alive(pid_t pid) {
    ProcessInfo info;
    if (ReadProcStat(pid, &info)) {
        return info.alive;
    }
    return false;
}

}  // namespace

// ===========================================================================
// Process::Impl
// ===========================================================================

struct Process::Impl final {
    ProcessOptions options;
    std::shared_ptr<ProcessBackend> backend;
    pid_t pid{-1};
    pid_t ppid{-1};
    pid_t pgid{-1};
    LifecycleState state{LifecycleState::kCreated};
    std::optional<ExitStatus> exit_status;
    ProcError last_error;
    std::string exec_path;
    std::chrono::steady_clock::time_point started_monotonic{};
    std::chrono::system_clock::time_point started_wall{};
    std::shared_ptr<FdChannel> stdin_channel;
    std::shared_ptr<FdChannel> stdout_channel;
    std::shared_ptr<FdChannel> stderr_channel;
    ProcessInfo last_info;
    std::uint64_t peak_rss_bytes{0};
    bool valid{false};
    bool channels_closed{false};

    void CloseAllChannels() noexcept {
        channels_closed = true;
        if (stdin_channel) {
            stdin_channel->Close();
        }
        if (stdout_channel) {
            stdout_channel->Close();
        }
        if (stderr_channel) {
            stderr_channel->Close();
        }
    }

    void RecordExit(const ExitStatus& status) noexcept {
        exit_status = status;
        state = LifecycleState::kReaped;
        last_info.alive = false;
        last_info.zombie = false;
        last_info.state = 'X';
        RecordExitStats(status);
        EmitExitEvents(pid, pgid, status);
    }

    void Teardown() noexcept {
        if (valid && !exit_status && backend) {
            (void)backend->Kill(pid, SIGKILL);
            const auto waited = backend->Wait(pid, std::chrono::milliseconds(5000));
            if (waited.ok()) {
                ExitStatus status = waited.value();
                status.wall_ms = static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - started_monotonic)
                        .count());
                try {
                    RecordExit(status);
                } catch (...) {
                }
            } else {
                State().live.fetch_sub(1, std::memory_order_relaxed);
            }
        }
        CloseAllChannels();
        valid = false;
    }

    Result<ExitStatus> WaitImpl(std::chrono::milliseconds timeout) {
        if (!valid) {
            return MakeError(ProcErrorCode::kNotRunning, ESRCH, "进程未启动", "Process::Wait");
        }
        if (exit_status) {
            return *exit_status;
        }
        auto waited = backend->Wait(pid, timeout);
        if (!waited.ok()) {
            last_error = waited.error();
            return waited.error();
        }
        ExitStatus status = waited.value();
        status.wall_ms = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started_monotonic)
                .count());
        RecordExit(status);
        return status;
    }

    bool TryWaitImpl(ExitStatus* out, ProcError* error) {
        if (!valid) {
            if (error) {
                *error = MakeError(ProcErrorCode::kNotRunning, ESRCH, "进程未启动",
                                   "Process::TryWait");
            }
            return false;
        }
        if (exit_status) {
            if (out) {
                *out = *exit_status;
            }
            return true;
        }
        auto waited = backend->Wait(pid, std::chrono::milliseconds(0));
        if (waited.ok()) {
            ExitStatus status = waited.value();
            status.wall_ms = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - started_monotonic)
                    .count());
            RecordExit(status);
            if (out) {
                *out = status;
            }
            return true;
        }
        if (waited.error().code == ProcErrorCode::kTimeout) {
            return false;
        }
        last_error = waited.error();
        if (error) {
            *error = waited.error();
        }
        return false;
    }

    Result<void> KillImpl(int signal) {
        if (!valid) {
            return MakeError(ProcErrorCode::kNotRunning, ESRCH, "进程未启动", "Process::Kill");
        }
        if (exit_status) {
            return MakeError(ProcErrorCode::kNotRunning, ESRCH, "进程已回收", "Process::Kill");
        }
        if (signal <= 0) {
            return MakeError(ProcErrorCode::kInvalidArgument, EINVAL, "信号无效", "Process::Kill");
        }
        auto killed = backend->Kill(pid, signal);
        if (!killed.ok()) {
            last_error = killed.error();
            return killed.error();
        }
        State().killed.fetch_add(1, std::memory_order_relaxed);
        ProcEvent event;
        event.type = ProcEventType::kKill;
        event.pid = pid;
        event.pgid = pgid;
        event.signal = signal;
        event.when = std::chrono::system_clock::now();
        EmitEvent(event);
        if (signal == SIGKILL) {
            state = LifecycleState::kKilled;
        } else if (signal == SIGTERM && state == LifecycleState::kRunning) {
            state = LifecycleState::kTerminating;
        }
        return OkResult();
    }
};

// ===========================================================================
// Process 公共接口
// ===========================================================================

Process::Process() noexcept = default;

Process::~Process() noexcept {
    if (m_impl) {
        m_impl->Teardown();
    }
}

Process::Process(Process&& other) noexcept : m_impl(std::move(other.m_impl)) {}

Process& Process::operator=(Process&& other) noexcept {
    if (this != &other) {
        if (m_impl) {
            m_impl->Teardown();
        }
        m_impl = std::move(other.m_impl);
    }
    return *this;
}

Result<void> Process::Spawn(const ProcessOptions& options, Process* out,
                            std::shared_ptr<ProcessBackend> backend) {
    if (out == nullptr) {
        return MakeError(ProcErrorCode::kInvalidArgument, EINVAL, "输出指针为空",
                         "Process::Spawn");
    }
    *out = Process();
    if (options.argv.empty() || options.argv[0].empty()) {
        State().spawn_failed.fetch_add(1, std::memory_order_relaxed);
        ProcEvent event;
        event.type = ProcEventType::kSpawnFailed;
        event.reason = "argv 为空";
        event.when = std::chrono::system_clock::now();
        EmitEvent(event);
        return MakeError(ProcErrorCode::kInvalidArgument, EINVAL, "argv 不能为空",
                         "Process::Spawn");
    }
    if (!backend) {
        backend = DefaultProcessBackend();
    }
    auto spawned = backend->Spawn(options);
    if (!spawned.ok()) {
        State().spawn_failed.fetch_add(1, std::memory_order_relaxed);
        ProcEvent event;
        event.type = ProcEventType::kSpawnFailed;
        event.reason = spawned.error().message;
        event.when = std::chrono::system_clock::now();
        EmitEvent(event);
        auto failed = std::make_unique<Impl>();
        failed->backend = std::move(backend);
        failed->options = options;
        failed->state = LifecycleState::kFailed;
        failed->last_error = spawned.error();
        out->m_impl = std::move(failed);
        return spawned.error();
    }
    SpawnedProcess handle = std::move(spawned.value());
    auto impl = std::make_unique<Impl>();
    impl->options = options;
    impl->backend = std::move(backend);
    impl->pid = handle.pid;
    impl->ppid = handle.ppid;
    impl->pgid = handle.pgid;
    impl->state = LifecycleState::kRunning;
    impl->exec_path = options.argv[0];
    impl->started_monotonic = handle.started_monotonic;
    impl->started_wall = handle.started_wall;
    impl->valid = true;
    ChannelOptions channel_options;
    channel_options.framing = Framing::kRaw;
    if (handle.pty) {
        auto master = std::make_shared<FdChannel>(handle.stdin_fd, true, channel_options);
        impl->stdin_channel = master;
        impl->stdout_channel = master;
        impl->stderr_channel = master;
    } else {
        impl->stdin_channel = std::make_shared<FdChannel>(
            handle.stdin_fd, handle.stdin_fd >= 0, channel_options);
        impl->stdout_channel = std::make_shared<FdChannel>(
            handle.stdout_fd, handle.stdout_fd >= 0, channel_options);
        impl->stderr_channel = std::make_shared<FdChannel>(
            handle.stderr_fd, handle.stderr_fd >= 0, channel_options);
    }
    ProcessInfo info;
    if (ReadProcStat(handle.pid, &info)) {
        info.started_monotonic = handle.started_monotonic;
        impl->last_info = info;
        impl->peak_rss_bytes = info.rss_bytes;
    } else {
        impl->last_info.pid = handle.pid;
        impl->last_info.ppid = handle.ppid;
        impl->last_info.pgid = handle.pgid;
        impl->last_info.started_monotonic = handle.started_monotonic;
        impl->last_info.started_at = handle.started_wall;
    }
    State().spawned.fetch_add(1, std::memory_order_relaxed);
    State().live.fetch_add(1, std::memory_order_relaxed);
    EmitSpawnEvent(handle.pid, handle.pgid);
    out->m_impl = std::move(impl);
    return OkResult();
}

Result<Process> Spawn(const ProcessOptions& options,
                      std::shared_ptr<ProcessBackend> backend) {
    Process process;
    auto status = Process::Spawn(options, &process, std::move(backend));
    if (!status.ok()) {
        return status.error();
    }
    return process;
}

pid_t Process::pid() const noexcept { return m_impl ? m_impl->pid : -1; }
pid_t Process::ppid() const noexcept { return m_impl ? m_impl->ppid : -1; }
pid_t Process::pgid() const noexcept { return m_impl ? m_impl->pgid : -1; }
bool Process::valid() const noexcept { return m_impl && m_impl->valid; }
bool Process::reaped() const noexcept {
    return m_impl && m_impl->exit_status.has_value();
}
LifecycleState Process::state() const noexcept {
    return m_impl ? m_impl->state : LifecycleState::kCreated;
}
const std::string& Process::exec_path() const noexcept {
    static const std::string empty;
    return m_impl ? m_impl->exec_path : empty;
}
const ProcError& Process::last_error() const noexcept {
    static const ProcError empty;
    return m_impl ? m_impl->last_error : empty;
}

Result<ProcessInfo> Process::Info() const {
    if (!m_impl || !m_impl->valid) {
        return MakeError(ProcErrorCode::kNotRunning, ESRCH, "进程未启动", "Process::Info");
    }
    if (m_impl->exit_status) {
        ProcessInfo cached = m_impl->last_info;
        cached.alive = false;
        cached.zombie = false;
        cached.state = 'X';
        return cached;
    }
    auto queried = m_impl->backend->Query(m_impl->pid);
    if (!queried.ok()) {
        if (queried.error().code == ProcErrorCode::kNotRunning && m_impl->exit_status) {
            return m_impl->last_info;
        }
        return queried.error();
    }
    ProcessInfo info = queried.value();
    info.started_monotonic = m_impl->started_monotonic;
    info.started_at = m_impl->started_wall;
    m_impl->last_info = info;
    m_impl->peak_rss_bytes = std::max(m_impl->peak_rss_bytes, info.rss_bytes);
    return info;
}

Result<ExitStatus> Process::Wait(std::chrono::milliseconds timeout) {
    if (!m_impl) {
        return MakeError(ProcErrorCode::kNotRunning, ESRCH, "进程未启动", "Process::Wait");
    }
    return m_impl->WaitImpl(timeout);
}

bool Process::TryWait(ExitStatus* out, ProcError* error) {
    if (!m_impl) {
        if (error) {
            *error = MakeError(ProcErrorCode::kNotRunning, ESRCH, "进程未启动",
                               "Process::TryWait");
        }
        return false;
    }
    return m_impl->TryWaitImpl(out, error);
}

Result<void> Process::Kill(int signal) {
    if (!m_impl) {
        return MakeError(ProcErrorCode::kNotRunning, ESRCH, "进程未启动", "Process::Kill");
    }
    return m_impl->KillImpl(signal);
}

Result<void> Process::Terminate(std::chrono::milliseconds grace) {
    if (!m_impl || !m_impl->valid) {
        return MakeError(ProcErrorCode::kNotRunning, ESRCH, "进程未启动",
                         "Process::Terminate");
    }
    if (m_impl->exit_status) {
        return OkResult();
    }
    if (grace.count() < 0) {
        grace = std::chrono::milliseconds(5000);
    }
    if (m_impl->state == LifecycleState::kRunning) {
        auto killed = m_impl->KillImpl(SIGTERM);
        if (!killed.ok() && killed.error().code != ProcErrorCode::kNotRunning) {
            return killed.error();
        }
    }
    auto waited = m_impl->WaitImpl(grace);
    if (waited.ok()) {
        return OkResult();
    }
    if (waited.error().code != ProcErrorCode::kTimeout) {
        return waited.error();
    }
    m_impl->state = LifecycleState::kKilled;
    auto killed = m_impl->backend->Kill(m_impl->pid, SIGKILL);
    if (!killed.ok() && killed.error().code != ProcErrorCode::kNotRunning) {
        return killed.error();
    }
    State().killed.fetch_add(1, std::memory_order_relaxed);
    auto waited_again = m_impl->WaitImpl(std::chrono::milliseconds(5000));
    if (!waited_again.ok()) {
        return waited_again.error();
    }
    return OkResult();
}

Result<void> Process::KillTree(int signal) {
    if (!m_impl || !m_impl->valid) {
        return MakeError(ProcErrorCode::kNotRunning, ESRCH, "进程未启动",
                         "Process::KillTree");
    }
    if (m_impl->exit_status) {
        return MakeError(ProcErrorCode::kNotRunning, ESRCH, "进程已回收",
                         "Process::KillTree");
    }
    if (m_impl->pgid <= 0 || m_impl->pgid != m_impl->pid) {
        return MakeError(ProcErrorCode::kUnsupported, EINVAL,
                         "进程不是组长（Spawn 时需 new_session=true），拒绝 KillTree 以免误杀调用方进程组",
                         "Process::KillTree");
    }
    if (signal <= 0) {
        return MakeError(ProcErrorCode::kInvalidArgument, EINVAL, "信号无效",
                         "Process::KillTree");
    }
    auto killed = m_impl->backend->Kill(-m_impl->pgid, signal);
    if (!killed.ok()) {
        return killed.error();
    }
    State().killed.fetch_add(1, std::memory_order_relaxed);
    return OkResult();
}

bool Process::Alive() const noexcept {
    if (!m_impl || !m_impl->valid || m_impl->exit_status) {
        return false;
    }
    return m_impl->backend->Alive(m_impl->pid);
}

Result<void> Process::SetNice(int nice) {
    if (!m_impl || !m_impl->valid || m_impl->exit_status) {
        return MakeError(ProcErrorCode::kNotRunning, ESRCH, "进程未启动或已回收",
                         "Process::SetNice");
    }
    if (nice < -20 || nice > 19) {
        return MakeError(ProcErrorCode::kInvalidArgument, EINVAL, "nice 必须在 -20..19",
                         "Process::SetNice");
    }
    if (::syscall(SYS_setpriority, PRIO_PROCESS, m_impl->pid, nice) != 0) {
        const int error = errno;
        return MakeErrnoError(error == EPERM ? ProcErrorCode::kPermissionDenied
                                             : ProcErrorCode::kIoFailed,
                              error, "setpriority");
    }
    return OkResult();
}

Result<void> Process::SetAffinity(const std::vector<int>& cpus) {
    if (!m_impl || !m_impl->valid || m_impl->exit_status) {
        return MakeError(ProcErrorCode::kNotRunning, ESRCH, "进程未启动或已回收",
                         "Process::SetAffinity");
    }
    if (cpus.empty()) {
        return MakeError(ProcErrorCode::kInvalidArgument, EINVAL, "CPU 列表为空",
                         "Process::SetAffinity");
    }
    cpu_set_t set;
    CPU_ZERO(&set);
    for (const int cpu : cpus) {
        if (cpu < 0 || cpu >= CPU_SETSIZE) {
            return MakeError(ProcErrorCode::kInvalidArgument, EINVAL, "CPU 索引越界",
                             "Process::SetAffinity");
        }
        CPU_SET(cpu, &set);
    }
    if (::syscall(SYS_sched_setaffinity, m_impl->pid, sizeof(set), &set) != 0) {
        const int error = errno;
        return MakeErrnoError(error == EPERM ? ProcErrorCode::kPermissionDenied
                                             : ProcErrorCode::kIoFailed,
                              error, "sched_setaffinity");
    }
    return OkResult();
}

Result<void> Process::SetScheduling(int policy, int priority) {
    if (!m_impl || !m_impl->valid || m_impl->exit_status) {
        return MakeError(ProcErrorCode::kNotRunning, ESRCH, "进程未启动或已回收",
                         "Process::SetScheduling");
    }
    if (policy != SCHED_OTHER && policy != SCHED_BATCH) {
        return MakeError(ProcErrorCode::kUnsupported, EINVAL,
                         "只支持 SCHED_OTHER/SCHED_BATCH", "Process::SetScheduling");
    }
    if (priority != 0) {
        return MakeError(ProcErrorCode::kInvalidArgument, EINVAL,
                         "SCHED_OTHER/BATCH 的优先级必须为 0", "Process::SetScheduling");
    }
    struct sched_param parameter {};
    parameter.sched_priority = priority;
    if (::syscall(SYS_sched_setscheduler, m_impl->pid, policy, &parameter) != 0) {
        const int error = errno;
        return MakeErrnoError(error == EPERM ? ProcErrorCode::kPermissionDenied
                                             : ProcErrorCode::kIoFailed,
                              error, "sched_setscheduler");
    }
    return OkResult();
}

Result<void> Process::SetRlimit(const RLimitSetting& limit) {
    if (!m_impl || !m_impl->valid || m_impl->exit_status) {
        return MakeError(ProcErrorCode::kNotRunning, ESRCH, "进程未启动或已回收",
                         "Process::SetRlimit");
    }
    if (limit.resource <= 0) {
        return MakeError(ProcErrorCode::kInvalidArgument, EINVAL, "rlimit resource 非法",
                         "Process::SetRlimit");
    }
    struct ::rlimit native {};
    native.rlim_cur = static_cast<rlim_t>(limit.soft);
    native.rlim_max = static_cast<rlim_t>(limit.hard);
    if (::syscall(SYS_prlimit64, m_impl->pid, limit.resource, &native, nullptr) != 0) {
        const int error = errno;
        return MakeErrnoError(error == EPERM ? ProcErrorCode::kPermissionDenied
                                             : ProcErrorCode::kIoFailed,
                              error, "prlimit64");
    }
    return OkResult();
}

Result<void> Process::SetOomScore(int score) {
    if (!m_impl || !m_impl->valid || m_impl->exit_status) {
        return MakeError(ProcErrorCode::kNotRunning, ESRCH, "进程未启动或已回收",
                         "Process::SetOomScore");
    }
    if (score < -1000 || score > 1000) {
        return MakeError(ProcErrorCode::kInvalidArgument, EINVAL,
                         "oom_score_adj 必须在 -1000..1000", "Process::SetOomScore");
    }
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/%d/oom_score_adj",
                  static_cast<int>(m_impl->pid));
    const int fd = static_cast<int>(::syscall(SYS_openat, AT_FDCWD, path,
                                              O_WRONLY | O_CLOEXEC, 0));
    if (fd < 0) {
        const int error = errno;
        return MakeErrnoError(error == EACCES || error == EPERM
                                  ? ProcErrorCode::kPermissionDenied
                                  : ProcErrorCode::kIoFailed,
                              error, "open(oom_score_adj)");
    }
    char text[16];
    const int length = std::snprintf(text, sizeof(text), "%d", score);
    const ssize_t written = detail::RawWrite(fd, text, static_cast<std::size_t>(length));
    const int saved = errno;
    detail::RawClose(fd);
    if (written != static_cast<ssize_t>(length)) {
        return MakeErrnoError(ProcErrorCode::kIoFailed, saved, "write(oom_score_adj)");
    }
    return OkResult();
}

namespace {
MessageChannel& ClosedChannel() {
    static FdChannel channel(-1, false);
    channel.Close();
    return channel;
}
}  // namespace

MessageChannel& Process::channel(StdioStream stream) noexcept {
    if (!m_impl) {
        return ClosedChannel();
    }
    switch (stream) {
        case StdioStream::kStdin:
            return m_impl->stdin_channel ? *m_impl->stdin_channel : ClosedChannel();
        case StdioStream::kStdout:
            return m_impl->stdout_channel ? *m_impl->stdout_channel : ClosedChannel();
        case StdioStream::kStderr:
            return m_impl->stderr_channel ? *m_impl->stderr_channel : ClosedChannel();
    }
    return ClosedChannel();
}

MessageChannel& Process::stdin_channel() noexcept { return channel(StdioStream::kStdin); }
MessageChannel& Process::stdout_channel() noexcept { return channel(StdioStream::kStdout); }
MessageChannel& Process::stderr_channel() noexcept { return channel(StdioStream::kStderr); }

int Process::native_fd(StdioStream stream) const noexcept {
    if (!m_impl) {
        return -1;
    }
    switch (stream) {
        case StdioStream::kStdin:
            return m_impl->stdin_channel ? m_impl->stdin_channel->native_fd() : -1;
        case StdioStream::kStdout:
            return m_impl->stdout_channel ? m_impl->stdout_channel->native_fd() : -1;
        case StdioStream::kStderr:
            return m_impl->stderr_channel ? m_impl->stderr_channel->native_fd() : -1;
    }
    return -1;
}

void Process::CloseChannels() noexcept {
    if (m_impl) {
        m_impl->CloseAllChannels();
    }
}

ResourceUsage Process::Usage() const {
    ResourceUsage usage;
    if (!m_impl) {
        return usage;
    }
    usage.peak_rss_bytes = m_impl->peak_rss_bytes;
    if (m_impl->valid && !m_impl->exit_status) {
        usage.open_fds = CountFdsOf(m_impl->pid);
        auto info = Info();
        if (info.ok()) {
            usage.cpu_user_ms = info.value().cpu_user_ms;
            usage.cpu_system_ms = info.value().cpu_system_ms;
            usage.peak_rss_bytes = std::max(usage.peak_rss_bytes, info.value().rss_bytes);
        }
    }
    if (m_impl->stdout_channel) {
        usage.stdout_bytes = m_impl->stdout_channel->stats().bytes_read;
    }
    if (m_impl->stderr_channel) {
        usage.stderr_bytes = m_impl->stderr_channel->stats().bytes_read;
    }
    return usage;
}

// ===========================================================================
// Run
// ===========================================================================

namespace {

void ForwardStderrText(pid_t pid, pid_t pgid, std::string* partial,
                       const char* data, std::size_t size, bool flush) {
    for (std::size_t index = 0; index < size; ++index) {
        const char character = data[index];
        if (character == '\n') {
            StderrLine line;
            line.pid = pid;
            line.pgid = pgid;
            line.text = *partial;
            line.partial = false;
            EmitStderrLine(line);
            partial->clear();
        } else {
            partial->push_back(character);
        }
    }
    if (flush && !partial->empty()) {
        StderrLine line;
        line.pid = pid;
        line.pgid = pgid;
        line.text = std::move(*partial);
        line.partial = true;
        partial->clear();
        EmitStderrLine(line);
    }
}

}  // namespace

Result<RunResult> Run(const ProcessOptions& options, std::string_view input,
                      std::function<void(pid_t)> on_spawn) {
    if (options.argv.empty()) {
        return MakeError(ProcErrorCode::kInvalidArgument, EINVAL, "argv 不能为空", "Run");
    }
    ProcessOptions run_options = options;
    Process process;
    auto spawned = Process::Spawn(run_options, &process);
    if (!spawned.ok()) {
        return spawned.error();
    }
    if (on_spawn) {
        try {
            on_spawn(process.pid());
        } catch (...) {
        }
    }

    RunResult result;
    MessageChannel& stdin_channel = process.stdin_channel();
    MessageChannel& stdout_channel = process.stdout_channel();
    MessageChannel& stderr_channel = process.stderr_channel();
    const int stdout_fd = stdout_channel.native_fd();
    const int stderr_fd = stderr_channel.native_fd();
    const int stdin_fd = stdin_channel.native_fd();
    bool stdin_open = stdin_fd >= 0;
    std::size_t input_offset = 0;
    bool term_sent = false;
    bool kill_sent = false;
    auto term_deadline = std::chrono::steady_clock::now();
    const auto started = std::chrono::steady_clock::now();
    const bool has_timeout = run_options.timeout.count() > 0;
    const auto deadline = started + (has_timeout ? run_options.timeout
                                                 : std::chrono::milliseconds(0));
    const auto grace = run_options.grace.count() > 0 ? run_options.grace
                                                     : std::chrono::milliseconds(5000);
    std::string stderr_partial;
    // 堆分配而不是栈数组：Run 可能被 Fiber 调用，而 Fiber 栈远小于普通
    // 线程栈；64 KiB 的栈缓冲会直接触发 guard page（实测 SegFault）。
    std::vector<std::byte> buffer(65536);
    bool stdout_eof = false;
    bool stderr_eof = false;

    if (stdin_open && input.empty()) {
        stdin_channel.Close();
        stdin_open = false;
    }

    const auto drain = [&](MessageChannel& channel, bool is_stderr) -> bool {
        bool eof = false;
        for (;;) {
            auto read = channel.Read(MutableByteSpan(buffer.data(), buffer.size()),
                                     std::chrono::milliseconds(0));
            if (!read.ok()) {
                if (read.error().code == ProcErrorCode::kClosed) {
                    eof = true;
                }
                break;
            }
            const std::size_t count = read.value().bytes;
            if (count == 0) {
                break;
            }
            std::string* sink = is_stderr ? &result.standard_error : &result.standard_output;
            std::uint64_t* counter = is_stderr ? &result.stderr_bytes : &result.stdout_bytes;
            *counter += count;
            if (run_options.max_output_bytes == 0 ||
                sink->size() < run_options.max_output_bytes) {
                const std::size_t room =
                    run_options.max_output_bytes == 0
                        ? count
                        : std::min(count, run_options.max_output_bytes - sink->size());
                sink->append(reinterpret_cast<const char*>(buffer.data()), room);
                if (room < count) {
                    result.output_truncated = true;
                    result.dropped_output_bytes += count - room;
                }
            } else {
                result.output_truncated = true;
                result.dropped_output_bytes += count;
            }
            if (is_stderr) {
                ForwardStderrText(process.pid(), process.pgid(), &stderr_partial,
                                  reinterpret_cast<const char*>(buffer.data()), count, false);
            }
        }
        return eof;
    };

    for (;;) {
        stdout_eof = drain(stdout_channel, false) || stdout_eof;
        stderr_eof = drain(stderr_channel, true) || stderr_eof;

        if (stdin_open && input_offset < input.size()) {
            const ssize_t count = detail::RawWrite(
                stdin_fd, input.data() + input_offset, input.size() - input_offset);
            if (count > 0) {
                input_offset += static_cast<std::size_t>(count);
            } else if (count < 0 && errno == EINTR) {
                continue;
            } else if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                // 留给下面的 poll(POLLOUT)。
            } else {
                stdin_channel.Close();
                stdin_open = false;
            }
        }
        if (stdin_open && input_offset >= input.size()) {
            stdin_channel.Close();
            stdin_open = false;
        }

        ExitStatus status;
        ProcError wait_error;
        if (process.TryWait(&status, &wait_error)) {
            result.status = status;
            break;
        }
        if (!wait_error.ok()) {
            (void)process.Terminate(grace);
            return wait_error;
        }

        const auto now = std::chrono::steady_clock::now();
        if (has_timeout && !term_sent && now >= deadline) {
            term_sent = true;
            term_deadline = now + grace;
            State().timed_out.fetch_add(1, std::memory_order_relaxed);
            ProcEvent event;
            event.type = ProcEventType::kTimeout;
            event.pid = process.pid();
            event.pgid = process.pgid();
            event.reason = "Run 超时，发送 SIGTERM";
            event.when = std::chrono::system_clock::now();
            EmitEvent(event);
            (void)process.Kill(SIGTERM);
        } else if (term_sent && !kill_sent && now >= term_deadline) {
            kill_sent = true;
            ProcEvent event;
            event.type = ProcEventType::kKill;
            event.pid = process.pid();
            event.pgid = process.pgid();
            event.signal = SIGKILL;
            event.reason = "宽限期已过，发送 SIGKILL";
            event.when = std::chrono::system_clock::now();
            EmitEvent(event);
            (void)process.Kill(SIGKILL);
        }

        struct pollfd descriptors[3] {};
        nfds_t descriptor_count = 0;
        if (stdout_fd >= 0 && !stdout_eof) {
            descriptors[descriptor_count].fd = stdout_fd;
            descriptors[descriptor_count].events = POLLIN;
            ++descriptor_count;
        }
        if (stderr_fd >= 0 && !stderr_eof) {
            descriptors[descriptor_count].fd = stderr_fd;
            descriptors[descriptor_count].events = POLLIN;
            ++descriptor_count;
        }
        if (stdin_open && input_offset < input.size()) {
            descriptors[descriptor_count].fd = stdin_fd;
            descriptors[descriptor_count].events = POLLOUT;
            ++descriptor_count;
        }
        int wait_ms = 5;
        if (has_timeout && !term_sent) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - now);
            wait_ms = static_cast<int>(
                std::max<std::int64_t>(1, std::min<std::int64_t>(5, remaining.count())));
        }
        const int ready = detail::RawPoll(descriptors, descriptor_count, wait_ms);
        if (ready < 0 && errno != EINTR) {
            break;
        }
    }

    // 回收后把管道里剩余的数据读到 EOF（最多再等 500ms）。
    const auto drain_deadline = std::chrono::steady_clock::now() +
                                std::chrono::milliseconds(500);
    while ((!stdout_eof || !stderr_eof) &&
           std::chrono::steady_clock::now() < drain_deadline) {
        const bool out_done = drain(stdout_channel, false);
        const bool err_done = drain(stderr_channel, true);
        stdout_eof = stdout_eof || out_done;
        stderr_eof = stderr_eof || err_done;
        if (stdout_eof && stderr_eof) {
            break;
        }
        (void)detail::RawNanosleep(0, 1000 * 1000);
    }
    if (!stdout_eof) {
        (void)drain(stdout_channel, false);
    }
    if (!stderr_eof) {
        (void)drain(stderr_channel, true);
    }
    if (!stderr_partial.empty()) {
        ForwardStderrText(process.pid(), process.pgid(), &stderr_partial, "", 0, true);
    }
    if (!result.status.reaped) {
        auto waited = process.Wait(std::chrono::milliseconds(5000));
        if (!waited.ok()) {
            (void)process.Terminate(grace);
            auto retry = process.Wait(std::chrono::milliseconds(5000));
            if (!retry.ok()) {
                return retry.error();
            }
            result.status = retry.value();
        } else {
            result.status = waited.value();
        }
    }
    result.status.timed_out = term_sent;
    if (term_sent) {
        // Run 的超时由这里统一归因（Process::Wait 不认识 timeout 语义）。
        result.status.timed_out = true;
    }
    return result;
}

// ===========================================================================
// 统计/日志公共接口
// ===========================================================================

ProcStatsSnapshot StatsSnapshot() {
    ProcStatsSnapshot snapshot;
    snapshot.spawned = State().spawned.load(std::memory_order_relaxed);
    snapshot.spawn_failed = State().spawn_failed.load(std::memory_order_relaxed);
    snapshot.exited = State().exited.load(std::memory_order_relaxed);
    snapshot.signaled = State().signaled.load(std::memory_order_relaxed);
    snapshot.timed_out = State().timed_out.load(std::memory_order_relaxed);
    snapshot.killed = State().killed.load(std::memory_order_relaxed);
    snapshot.conditions = State().conditions.load(std::memory_order_relaxed);
    snapshot.reaped = State().reaped.load(std::memory_order_relaxed);
    snapshot.live = State().live.load(std::memory_order_relaxed);
    snapshot.open_fds = CountOpenFds();
    return snapshot;
}

void ResetStats() {
    State().spawned.store(0, std::memory_order_relaxed);
    State().spawn_failed.store(0, std::memory_order_relaxed);
    State().exited.store(0, std::memory_order_relaxed);
    State().signaled.store(0, std::memory_order_relaxed);
    State().timed_out.store(0, std::memory_order_relaxed);
    State().killed.store(0, std::memory_order_relaxed);
    State().conditions.store(0, std::memory_order_relaxed);
    State().reaped.store(0, std::memory_order_relaxed);
    State().live.store(0, std::memory_order_relaxed);
}

void SetEventSink(ProcEventSink sink) {
    std::lock_guard<std::mutex> lock(State().sink_mutex);
    State().event_sink = std::move(sink);
}

void SetStderrLineSink(StderrLineSink sink) {
    std::lock_guard<std::mutex> lock(State().sink_mutex);
    State().stderr_sink = std::move(sink);
}

void EnableDefaultLogging(bool enable) {
    if (enable) {
        SetEventSink(DefaultEventLog);
        SetStderrLineSink(DefaultStderrLog);
    } else {
        SetEventSink(nullptr);
        SetStderrLineSink(nullptr);
    }
}

std::size_t CountOpenFds() {
    return CountDirectoryEntries("/proc/self/fd");
}

void IgnoreSigPipe() {
    struct sigaction action {};
    action.sa_handler = SIG_IGN;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    (void)::sigaction(SIGPIPE, &action, nullptr);
}

void Cleanup() {
    SetEventSink(nullptr);
    SetStderrLineSink(nullptr);
    State().default_logging.store(false, std::memory_order_relaxed);
    ResetStats();
}

// ===========================================================================
// 默认后端 / 时钟 / 配置
// ===========================================================================

std::shared_ptr<ProcessBackend> DefaultProcessBackend() {
    static const std::shared_ptr<ProcessBackend> backend = std::make_shared<RealBackend>();
    return backend;
}

namespace {

class RealClock final : public Clock {
public:
    std::chrono::steady_clock::time_point Now() const override {
        return std::chrono::steady_clock::now();
    }

    void SleepFor(std::chrono::milliseconds duration) override {
        if (duration.count() <= 0) {
            return;
        }
        struct timespec request {};
        request.tv_sec = static_cast<time_t>(duration.count() / 1000);
        request.tv_nsec = static_cast<long>((duration.count() % 1000) * 1000000);
        while (detail::RawNanosleep(request.tv_sec, request.tv_nsec) != 0 &&
               errno == EINTR) {
        }
    }
};

}  // namespace

std::shared_ptr<Clock> MakeRealClock() {
    static const std::shared_ptr<Clock> clock = std::make_shared<RealClock>();
    return clock;
}

ProcessDefaults LoadProcessDefaults() {
    ProcessDefaults defaults;
    defaults.grace = std::chrono::milliseconds(
        ReadIntConfig("os.proc.default_grace_ms", 5000));
    defaults.timeout = std::chrono::milliseconds(
        ReadIntConfig("os.proc.default_timeout_ms", 0));
    const int output_limit = ReadIntConfig("os.proc.max_output_bytes", 1 << 20);
    defaults.max_output_bytes =
        output_limit > 0 ? static_cast<std::size_t>(output_limit) : 0;
    defaults.supervisor_tick = std::chrono::milliseconds(
        ReadIntConfig("os.proc.supervisor_tick_ms", 50));
    const int workers = ReadIntConfig("os.proc.pool_workers", 4);
    defaults.pool_workers = workers > 0 ? static_cast<std::size_t>(workers) : 1;
    const int queue = ReadIntConfig("os.proc.pool_queue", 64);
    defaults.pool_queue = queue > 0 ? static_cast<std::size_t>(queue) : 1;
    defaults.forward_stderr = ReadBoolConfig("os.proc.forward_stderr", false);
    const int max_fds = ReadIntConfig("os.proc.max_open_fds", 0);
    defaults.max_open_fds = max_fds > 0 ? static_cast<std::size_t>(max_fds) : 0;
    defaults.max_rss_bytes = ReadUInt64Config("os.proc.max_rss_bytes", 0);
    const std::string action = ReadStringConfig("os.proc.quota_action", "ignore");
    if (action == "kill") {
        defaults.quota_action = QuotaAction::kKill;
    } else if (action == "warn") {
        defaults.quota_action = QuotaAction::kWarn;
    } else {
        defaults.quota_action = QuotaAction::kIgnore;
    }
    return defaults;
}

}  // namespace go2cpp::os
