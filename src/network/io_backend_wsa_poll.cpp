/**
 * @file io_backend_wsa_poll.cpp
 * @brief Windows 的 IOBackend 实现——`WSAPoll` + 短超时轮询（**短期方案**）。
 *
 * ## 为什么是这个方案
 * `epoll` 是**就绪通知**，`IOCP` 是**完成通知**——后者无法平移（见
 * `docs/reports/cross-platform-syscall-audit.md` TOP2）。
 * 短期用 `WSAPoll`（语义最接近 `poll`）先把 Windows 跑起来；
 * 长期再写 IOCP 的 completion 后端（`docs/reports/go-cross-platform-study.md` §2 Q2）。
 *
 * ## 契约差异（相对 epoll）
 * | 项 | epoll | 本实现（WSAPoll） |
 * |---|---|---|
 * | 唤醒 poller | `eventfd` 可被 `epoll_wait` 监听 | **`WSAPoll` 无法监听 `HANDLE`** → 用 **10ms 短超时轮询**（最大唤醒延迟 10ms） |
 * | 边缘触发 | 支持 | **不支持**（`WSAPoll` 只有水平触发） |
 * | 一次性 | 支持 `EPOLLONESHOT` | 不支持 |
 * | 最大 fd 数 | 大 | `WSAPoll` 单次上限受 `FD_SETSIZE` 影响（本实现分批） |
 *
 * ## 边界与陷阱
 * - **10ms 轮询是真实延迟**：`wake()` 后最长 10ms 才被处理——对延迟敏感的场景需注意；
 * - `WSAPoll` 的已知 bug：连接失败时可能不报 `POLLERR`（Windows 8+ 已修）；
 * - 只接受 socket（`WSAPoll` 不支持普通文件句柄）。
 */
#if !defined(__linux__)

#include <algorithm>
#include <cerrno>
#include <vector>

#include "go2cpp/network/io_backend.hpp"
#include "go2cpp/platform/wake_event.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

namespace go2cpp::io {
namespace {

constexpr int kPollSliceMs = 10;  // 短超时轮询：wake 的最大延迟

class WsaPollBackend final : public IOBackend {
 public:
  bool initialize(int* error) noexcept override {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
      if (error != nullptr) *error = WSAGetLastError();
      return false;
    }
    m_wsa_ready = true;
    return m_wake.Open();
  }

  int add_or_modify(int fd, std::uint32_t interest, std::uint64_t token,
                    bool modify) noexcept override {
    if (fd < 0) return WSAEINVAL;
    for (auto& entry : m_entries) {
      if (entry.fd == fd) {
        if (!modify) return WSAEEXIST;
        entry.interest = interest;
        entry.token = token;
        return 0;
      }
    }
    if (modify) return WSAENOENT;
    m_entries.push_back(Entry{fd, interest, token});
    return 0;
  }

  int remove(int fd) noexcept override {
    for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
      if (it->fd == fd) {
        m_entries.erase(it);
        return 0;
      }
    }
    return 0;  // 不存在也成功（契约）
  }

  int wait(BackendEvent* output, std::size_t capacity,
           int timeout_ms) noexcept override {
    if (output == nullptr || capacity == 0) return -WSAEINVAL;
    // 分批：WSAPoll 一次最多处理 m_scratch.size() 个
    const std::size_t batch = std::min(capacity, m_entries.size());
    m_scratch.resize(batch);
    for (std::size_t i = 0; i < batch; ++i) {
      m_scratch[i].fd = static_cast<SOCKET>(m_entries[i].fd);
      m_scratch[i].events = InterestToEvents(m_entries[i].interest);
      m_scratch[i].revents = 0;
    }
    // 把用户超时切成 10ms 片，保证 wake() 最多 10ms 生效
    int remaining = timeout_ms;
    for (;;) {
      const int slice = (remaining < 0) ? kPollSliceMs
                                        : std::min(remaining, kPollSliceMs);
      const int rc = ::WSAPoll(m_scratch.data(), static_cast<ULONG>(batch), slice);
      if (rc > 0) {
        std::size_t n = 0;
        for (std::size_t i = 0; i < batch && n < capacity; ++i) {
          if (m_scratch[i].revents != 0) {
            output[n].token = m_entries[i].token;
            output[n].events = EventsToReady(m_scratch[i].revents);
            ++n;
          }
        }
        return static_cast<int>(n);
      }
      if (rc == 0) {
        // 超时片到了：检查是否被 wake
        if (m_wakeup_pending.exchange(false)) return 0;
        if (remaining > 0) {
          remaining -= slice;
          if (remaining == 0) return 0;
        }
        continue;
      }
      return -WSAGetLastError();
    }
  }

  void wake() noexcept override {
    m_wakeup_pending.store(true);
    m_wake.Notify();
  }

  void drain_wake() noexcept override {
    m_wakeup_pending.store(false);
    m_wake.Drain();
  }

  void shutdown() noexcept override {
    m_entries.clear();
    m_scratch.clear();
    m_wake = go2cpp::platform::WakeEvent{};
    if (m_wsa_ready) {
      WSACleanup();
      m_wsa_ready = false;
    }
  }

 private:
  struct Entry {
    int fd;
    std::uint32_t interest;
    std::uint64_t token;
  };

  static short InterestToEvents(std::uint32_t interest) noexcept {
    short ev = 0;
    if (interest & 0x01U) ev |= POLLRDNORM;
    if (interest & 0x02U) ev |= POLLWRNORM;
    return ev;
  }

  static std::uint32_t EventsToReady(short revents) noexcept {
    std::uint32_t ready = 0;
    if (revents & (POLLRDNORM | POLLIN | POLLHUP | POLLERR)) ready |= 0x01U;
    if (revents & (POLLWRNORM | POLLOUT)) ready |= 0x02U;
    return ready;
  }

  std::vector<Entry> m_entries;
  std::vector<WSAPOLLFD> m_scratch;
  go2cpp::platform::WakeEvent m_wake;
  std::atomic<bool> m_wakeup_pending{false};
  bool m_wsa_ready = false;
};

}  // namespace

bool RegisterWsaPollBackend() {
  return RegisterBackend("wsa_poll", [] { return std::make_shared<WsaPollBackend>(); });
}

}  // namespace go2cpp::io

#endif  // !defined(__linux__)
