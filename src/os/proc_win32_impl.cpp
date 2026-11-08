/**
 * @file proc_win32_impl.cpp
 * @brief `Process::Impl`（PIMPL）与 `Process` 的构造/析构/简单成员的 Windows 版。
 *
 * ## 与 Linux 版的关系
 * `proc.cpp` 里 `Impl` 的 18 个字段**全部是跨平台类型**（`ProcessOptions`/`FdChannel`/
 * `ProcessInfo`/`ProcError`/`chrono`），所以**原样复制**即可；差异只在
 * 「怎么填这些字段」（`Spawn`/`Wait`/`Kill` 的实现，见 `proc_win32.cpp`）。
 *
 * ## 本文件覆盖
 * - `Process::Impl` 定义（与 Linux 版逐字段一致）；
 * - `Process::Process()` / `~Process()` / 移动构造与赋值；
 * - 简单成员：`pid`/`ppid`/`pgid`/`valid`/`reaped`/`state`。
 *
 * 复杂成员（`Spawn`/`Wait`/`Kill`/`Terminate`/`Info`/`TryWait`）在后续轮次补。
 */
#if defined(_WIN32)

#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "go2cpp/os/channel.hpp"
#include "go2cpp/os/proc.hpp"

namespace go2cpp::os {

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
    if (stdin_channel) stdin_channel->Close();
    if (stdout_channel) stdout_channel->Close();
    if (stderr_channel) stderr_channel->Close();
  }
};

Process::Process() noexcept = default;

Process::~Process() noexcept {
  if (m_impl && m_impl->valid && m_impl->state != LifecycleState::kExited &&
      m_impl->state != LifecycleState::kSignaled) {
    // 析构不杀进程（与 Linux 版一致：交由调用方显式 Kill/Terminate）。
  }
}

Process::Process(Process&& other) noexcept : m_impl(std::move(other.m_impl)) {}

Process& Process::operator=(Process&& other) noexcept {
  if (this != &other) {
    m_impl = std::move(other.m_impl);
  }
  return *this;
}

pid_t Process::pid() const noexcept { return m_impl ? m_impl->pid : -1; }
pid_t Process::ppid() const noexcept { return m_impl ? m_impl->ppid : -1; }
pid_t Process::pgid() const noexcept { return m_impl ? m_impl->pgid : -1; }
bool Process::valid() const noexcept { return m_impl && m_impl->valid; }

bool Process::reaped() const noexcept {
  if (!m_impl) return false;
  return m_impl->state == LifecycleState::kExited ||
         m_impl->state == LifecycleState::kSignaled;
}

LifecycleState Process::state() const noexcept {
  return m_impl ? m_impl->state : LifecycleState::kCreated;
}

}  // namespace go2cpp::os

#endif  // defined(_WIN32)
