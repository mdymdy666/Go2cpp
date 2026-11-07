/**
 * @file last_error.cpp
 * @brief 跨平台错误码实现（Linux errno / Windows GetLastError / WSAGetLastError）。
 */
#include "go2cpp/platform/last_error.hpp"

#if defined(_WIN32)
#include <windows.h>
#include <winsock2.h>  // WSAGetLastError
#else
#include <cerrno>
#include <cstring>
#endif

namespace go2cpp::platform {

int LastErrorCode() noexcept {
#if defined(_WIN32)
  return static_cast<int>(::GetLastError());
#else
  return errno;
#endif
}

int LastSocketErrorCode() noexcept {
#if defined(_WIN32)
  return ::WSAGetLastError();
#else
  return errno;
#endif
}

std::string LastErrorMessage(int code) {
#if defined(_WIN32)
  // 系统错误：FormatMessageW → UTF-8
  wchar_t* buf = nullptr;
  const DWORD len = ::FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, static_cast<DWORD>(code), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
      reinterpret_cast<wchar_t*>(&buf), 0, nullptr);
  std::string out;
  if (len > 0 && buf != nullptr) {
    // 宽字符 → UTF-8（简单转换，够用）
    const int need = ::WideCharToMultiByte(CP_UTF8, 0, buf, static_cast<int>(len),
                                           nullptr, 0, nullptr, nullptr);
    if (need > 0) {
      out.resize(static_cast<std::size_t>(need));
      ::WideCharToMultiByte(CP_UTF8, 0, buf, static_cast<int>(len), out.data(),
                            need, nullptr, nullptr);
    }
    ::LocalFree(buf);
  }
  if (out.empty()) {
    out = "error " + std::to_string(code);
  }
  return out;
#else
  return std::string(::strerror(code));
#endif
}

bool IsWouldBlock(int code) noexcept {
#if defined(_WIN32)
  return code == WSAEWOULDBLOCK;
#else
  return code == EAGAIN || code == EWOULDBLOCK;
#endif
}

bool IsInterrupted(int code) noexcept {
#if defined(_WIN32)
  return code == WSAEINTR;
#else
  return code == EINTR;
#endif
}

}  // namespace go2cpp::platform
