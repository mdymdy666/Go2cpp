/**
 * @file last_error.hpp
 * @brief 跨平台「最近一次错误」——统一 `errno` / `GetLastError` / `WSAGetLastError`。
 *
 * ## 定位
 * Linux 的错误码在 `errno`；Windows **有两套**：
 * - 系统调用：`GetLastError()`；
 * - **网络（Winsock）**：`WSAGetLastError()`。
 *
 * **混用会误判**（例如把 `WSAEWOULDBLOCK=10035` 当成 `ERROR_...`）。本模块把三条路径统一。
 *
 * ## 边界与陷阱
 * - `LastErrorCode()` **必须紧跟**失败调用之后读，中间任何函数调用都可能覆盖它；
 * - `IsWouldBlock()` 在 Linux 同时认 `EAGAIN` 和 `EWOULDBLOCK`（多数平台相等，但不保证）；
 * - `LastErrorMessage()` 返回 UTF-8（Windows 内部用 `FormatMessageW` 再转码）。
 */
#pragma once

#include <cstdint>
#include <string>

namespace go2cpp::platform {

/// @brief 读最近一次错误码（Linux `errno` / Windows `GetLastError`）。
/// @note 网络路径在 Windows 上应改用 `LastSocketErrorCode()`。
int LastErrorCode() noexcept;

/// @brief 读最近一次**网络**错误码（Linux 同 `errno`；Windows `WSAGetLastError()`）。
int LastSocketErrorCode() noexcept;

/// @brief 把错误码转成人类可读文本（UTF-8）。
std::string LastErrorMessage(int code);

/// @brief 是否是「会重试」的阻塞错误（Linux `EAGAIN/EWOULDBLOCK`；Windows `WSAEWOULDBLOCK`）。
bool IsWouldBlock(int code) noexcept;

/// @brief 是否是「被打断」（Linux `EINTR`；Windows `WSAEINTR`）。
bool IsInterrupted(int code) noexcept;

}  // namespace go2cpp::platform
