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
namespace {

/// @brief UTF-8 → UTF-16。
std::wstring ToWide(const std::string& s) {
  if (s.empty()) return {};
  const int need = ::MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                         static_cast<int>(s.size()), nullptr, 0);
  if (need <= 0) return {};
  std::wstring out(static_cast<std::size_t>(need), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                        out.data(), need);
  return out;
}

/// @brief 按 Windows 命令行规则给参数加引号。
std::wstring QuoteArg(const std::wstring& arg) {
  if (arg.find_first_of(L" \t\"") == std::wstring::npos) return arg;
  std::wstring out;
  out.push_back(L'"');
  for (wchar_t ch : arg) {
    if (ch == L'"') out.push_back(L'\\');
    out.push_back(ch);
  }
  out.push_back(L'"');
  return out;
}

/// @brief 打开进程句柄（失败返回 nullptr + 错误码）。
HANDLE OpenForQuery(pid_t pid, DWORD access, int* error) {
  HANDLE h = ::OpenProcess(access, FALSE, static_cast<DWORD>(pid));
  if (h == nullptr && error != nullptr) {
    *error = static_cast<int>(::GetLastError());
  }
  return h;
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
