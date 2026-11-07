/**
 * @file wake_event.cpp
 * @brief 跨平台唤醒事件实现（Linux eventfd / Windows CreateEventW）。
 *
 * ## 契约差异（重要）
 * | 平台 | 底层 | 多次 Notify |
 * |---|---|---|
 * | Linux | `eventfd` | **累积**（Drain 一次读走全部计数） |
 * | Windows | `CreateEventW(FALSE)` | **合并**（多次 SetEvent 只算一次） |
 *
 * 调用方**不要依赖通知次数**——Notify 只是提示，真正的状态从共享数据结构读。
 */
#include "go2cpp/platform/wake_event.hpp"

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#include <sys/eventfd.h>
#include <cerrno>
#endif

namespace go2cpp::platform {

WakeEvent::~WakeEvent() {
#if defined(_WIN32)
  if (m_handle >= 0) {
    ::CloseHandle(reinterpret_cast<HANDLE>(m_handle));
  }
#else
  if (m_handle >= 0) {
    ::close(static_cast<int>(m_handle));
  }
#endif
  m_handle = -1;
}

bool WakeEvent::Open(std::string* err) {
  if (m_handle >= 0) {
    return true;  // 已打开，幂等
  }
#if defined(_WIN32)
  // 自动重置（bManualReset=FALSE）：Wait 成功后自动清信号。
  HANDLE h = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (h == nullptr) {
    if (err != nullptr) {
      *err = "CreateEventW failed: " + std::to_string(::GetLastError());
    }
    return false;
  }
  m_handle = reinterpret_cast<std::intptr_t>(h);
#else
  const int fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (fd < 0) {
    if (err != nullptr) {
      *err = "eventfd failed: errno=" + std::to_string(errno);
    }
    return false;
  }
  m_handle = static_cast<std::intptr_t>(fd);
#endif
  return true;
}

void WakeEvent::Notify() noexcept {
  if (m_handle < 0) {
    return;
  }
#if defined(_WIN32)
  ::SetEvent(reinterpret_cast<HANDLE>(m_handle));
#else
  // eventfd 写 8 字节计数值 1（累积）。
  const std::uint64_t one = 1;
  ssize_t n = ::write(static_cast<int>(m_handle), &one, sizeof(one));
  (void)n;  // 非阻塞写，EAGAIN 表示计数已满（保守忽略）
#endif
}

void WakeEvent::Drain() noexcept {
  if (m_handle < 0) {
    return;
  }
#if defined(_WIN32)
  // 自动重置事件：WaitForSingleObject(0) 成功后自动清信号。
  (void)::WaitForSingleObject(reinterpret_cast<HANDLE>(m_handle), 0);
#else
  std::uint64_t value = 0;
  ssize_t n = ::read(static_cast<int>(m_handle), &value, sizeof(value));
  (void)n;
#endif
}

std::intptr_t WakeEvent::NativeHandle() const noexcept { return m_handle; }

}  // namespace go2cpp::platform
