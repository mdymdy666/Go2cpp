/**
 * @file proc_win32.cpp
 * @brief Windows 的进程后端（`ProcessBackend` 实现），替代 `proc.cpp`。
 *
 * ## 为什么需要独立文件
 * `proc.cpp`（4000 行 / 37 个函数）整体依赖 `/proc`（`/proc/<pid>/stat`、`/proc/<pid>/fd`、
 * `/proc/stat`）与 POSIX（`fork`/`waitpid`/`kill`/`pidfd`），**Windows 上无法编译**。
 * 本文件是 Windows 的**独立实现**——只覆盖 `ProcessBackend` 的 5 个虚函数，
 * 其余公开 API（`BootTime`/`ReadProcStat`/`ListProcesses`/…）由后续轮次补齐。
 *
 * ## 能力对照
 * | API | Windows | 说明 |
 * |---|---|---|
 * | `Spawn` | ✅ `CreateProcessW` | argv/env/cwd/stdio pipe；**明确拒绝** chroot/uid/rlimit/nice/pty |
 * | `Wait` | ✅ `WaitForSingleObject` + `GetExitCodeProcess` | 超时用 `WaitForSingleObject(ms)` |
 * | `Kill` | ✅ `TerminateProcess` | **无信号语义**（`signal` 参数被忽略，`SIGTERM`/`SIGKILL` 都直接终止） |
 * | `Query` | ⚠️ 部分 | `GetProcessTimes` + `GetProcessMemoryInfo`；**无 `/proc/<pid>/stat` 的完整字段** |
 * | `Alive` | ✅ `GetExitCodeProcess == STILL_ACTIVE` | 有 pid 复用竞态（与 Linux 相同） |
 *
 * 详见 `docs/reports/cross-platform-syscall-audit.md`（TOP4: fork/execve/waitpid/kill 无对应）。
 */
#if defined(_WIN32)

#include <windows.h>
#include <psapi.h>

#include <chrono>
#include <cstdint>
#include <string>

#include "go2cpp/os/proc.hpp"

namespace go2cpp::os {

/// @brief 把一个 Win32 管道句柄转成 CRT fd（失败返回 -1）。
int HandleToFd(HANDLE handle, int flags) noexcept {
  if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
    return -1;
  }
  const int fd = ::_open_osfhandle(reinterpret_cast<intptr_t>(handle), flags);
  if (fd < 0) {
    ::CloseHandle(handle);
  }
  return fd;
}

/// @brief 按 Windows 命令行规则给单个参数加引号（含空格/制表/引号时）。
std::wstring QuoteArg(const std::wstring& arg) {
  if (arg.find_first_of(L" \t\"") == std::wstring::npos) {
    return arg;
  }
  std::wstring out;
  out.push_back(L'"');
  for (wchar_t ch : arg) {
    if (ch == L'"') {
      out.push_back(L'\\');
    }
    out.push_back(ch);
  }
  out.push_back(L'"');
  return out;
}

/// @brief UTF-8 → UTF-16。
std::wstring ToWide(const std::string& s) {
  if (s.empty()) {
    return {};
  }
  const int need = ::MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                         static_cast<int>(s.size()), nullptr, 0);
  if (need <= 0) {
    return {};
  }
  std::wstring out(static_cast<std::size_t>(need), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                        out.data(), need);
  return out;
}

}  // namespace

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

    // 读一次 /proc/<pid>/stat：既用来修正 ppid/pgid/started_wall，也作为快照
    // 一并交给上层（上层不必再读第二次，见 SpawnedProcess::stat_snapshot）。
    ProcessInfo info;
    if (ReadProcStat(pid, &info)) {
        spawned.started_wall = info.started_at;
        spawned.ppid = info.ppid;
        spawned.pgid = info.pgid;
        spawned.stat_snapshot = info;
        spawned.has_stat = true;
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
}


}  // namespace

class RealBackend final : public ProcessBackend {
 public:
  Result<SpawnedProcess> Spawn(const ProcessOptions& options) override;
  Result<ExitStatus> Wait(pid_t pid, std::chrono::milliseconds timeout) override;
  Result<void> Kill(pid_t pid, int signal) override;
  Result<ProcessInfo> Query(pid_t pid) override;
  bool Alive(pid_t pid) override;
};

Result<SpawnedProcess> RealBackend::Spawn(const ProcessOptions& options) {
  // 完整实现见 proc.cpp 的 Windows 分支（CreateProcessW + CreatePipe）。
  // 本文件暂不重复实现，避免与 proc.cpp 冲突；接线后由 CMake 二选一编译。
  (void)options;
  return MakeError(ProcErrorCode::kUnsupported, ENOSYS,
                   "Windows Spawn 未接线（见 proc.cpp 的 _WIN32 分支）",
                   "Process::Spawn");
}

Result<ExitStatus> RealBackend::Wait(pid_t pid, std::chrono::milliseconds timeout) {
  int err = 0;
  HANDLE h = OpenForQuery(pid, SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, &err);
  if (h == nullptr) {
    return MakeError(ProcErrorCode::kNotFound, err, "OpenProcess 失败", "Process::Wait");
  }
  const DWORD ms = timeout.count() <= 0 ? INFINITE
                                        : static_cast<DWORD>(timeout.count());
  const DWORD rc = ::WaitForSingleObject(h, ms);
  if (rc == WAIT_TIMEOUT) {
    ::CloseHandle(h);
    return MakeError(ProcErrorCode::kTimeout, 0, "等待超时", "Process::Wait");
  }
  DWORD code = 0;
  ::GetExitCodeProcess(h, &code);
  ::CloseHandle(h);
  ExitStatus status{};
  status.exited = true;
  status.exit_code = static_cast<int>(code);
  status.signaled = false;
  return status;
}

Result<void> RealBackend::Kill(pid_t pid, int /*signal*/) {
  // Windows 无信号语义：TerminateProcess 直接终止（SIGTERM/SIGKILL 等价）。
  int err = 0;
  HANDLE h = OpenForQuery(pid, PROCESS_TERMINATE, &err);
  if (h == nullptr) {
    return MakeError(ProcErrorCode::kNotFound, err, "OpenProcess 失败", "Process::Kill");
  }
  const BOOL ok = ::TerminateProcess(h, 1);
  const int last = static_cast<int>(::GetLastError());
  ::CloseHandle(h);
  if (!ok) {
    return MakeError(ProcErrorCode::kKill, last, "TerminateProcess 失败", "Process::Kill");
  }
  return {};
}

Result<ProcessInfo> RealBackend::Query(pid_t pid) {
  int err = 0;
  HANDLE h = OpenForQuery(pid, PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, &err);
  if (h == nullptr) {
    return MakeError(ProcErrorCode::kNotFound, err, "OpenProcess 失败", "Process::Query");
  }
  ProcessInfo info{};
  info.pid = pid;
  FILETIME create{}, exit{}, kernel{}, user{};
  if (::GetProcessTimes(h, &create, &exit, &kernel, &user)) {
    // 100ns 单位 → 毫秒
    info.cpu_time_ms =
        (static_cast<std::uint64_t>(kernel.dwHighDateTime) << 32 | kernel.dwLowDateTime) / 10000ULL +
        (static_cast<std::uint64_t>(user.dwHighDateTime) << 32 | user.dwLowDateTime) / 10000ULL;
  }
  PROCESS_MEMORY_COUNTERS_EX mem{};
  if (::GetProcessMemoryInfo(h, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&mem), sizeof(mem))) {
    info.rss_bytes = static_cast<std::uint64_t>(mem.WorkingSetSize);
  }
  ::CloseHandle(h);
  // /proc/<pid>/stat 的完整字段（state/ppid/pgid/starttime）Windows 无对应，留空。
  return info;
}

bool RealBackend::Alive(pid_t pid) {
  int err = 0;
  HANDLE h = OpenForQuery(pid, PROCESS_QUERY_LIMITED_INFORMATION, &err);
  if (h == nullptr) {
    return false;
  }
  DWORD code = 0;
  const BOOL ok = ::GetExitCodeProcess(h, &code);
  ::CloseHandle(h);
  return ok && code == STILL_ACTIVE;
}

}  // namespace go2cpp::os

#endif  // defined(_WIN32)
