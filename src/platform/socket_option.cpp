/**
 * @file socket_option.cpp
 * @brief `socket_option.hpp` 的平台实现。
 */
#include "go2cpp/platform/socket_option.hpp"

#if defined(_WIN32)
#include <winsock2.h>
#else
#include <sys/socket.h>
#include <cerrno>
#endif

namespace go2cpp::platform {

bool SetReuseAddr(int fd, int* error) {
#if defined(_WIN32)
  // Windows：SO_REUSEADDR 允许绑已占端口（危险）→ 用 SO_EXCLUSIVEADDRUSE。
  BOOL exclusive = TRUE;
  if (::setsockopt(static_cast<SOCKET>(fd), SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                   reinterpret_cast<const char*>(&exclusive),
                   sizeof(exclusive)) != 0) {
    if (error != nullptr) *error = ::WSAGetLastError();
    return false;
  }
  // SO_LINGER{1,0}：close 时发 RST，跳过 TIME_WAIT（否则重启会 WSAEADDRINUSE）。
  struct linger lg {};
  lg.l_onoff = 1;
  lg.l_linger = 0;
  if (::setsockopt(static_cast<SOCKET>(fd), SOL_SOCKET, SO_LINGER,
                   reinterpret_cast<const char*>(&lg), sizeof(lg)) != 0) {
    if (error != nullptr) *error = ::WSAGetLastError();
    return false;
  }
  return true;
#else
  int enable = 1;
  if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable)) != 0) {
    if (error != nullptr) *error = errno;
    return false;
  }
  return true;
#endif
}

}  // namespace go2cpp::platform
