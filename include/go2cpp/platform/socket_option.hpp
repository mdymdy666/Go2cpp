/**
 * @file socket_option.hpp
 * @brief 跨平台 socket 选项——`SO_REUSEADDR` 的**语义反转**封装。
 *
 * ## 为什么必须封装（调查表 N1 风险）
 * | 平台 | `SO_REUSEADDR` 语义 |
 * |---|---|
 * | **Linux** | 只允许绑 `TIME_WAIT` 状态的端口——**安全**（防止误绑活跃连接） |
 * | **Windows** | 允许绑**已被其他 socket 占用**的端口——**危险**（两个进程能绑同一端口，流量随机分发） |
 *
 * 更糟的是：Windows 上 `SO_REUSEADDR` 会让**攻击者抢先绑定**你的端口（端口劫持）。
 *
 * ## 正确做法
 * - **Linux**：`SO_REUSEADDR`（复用 `TIME_WAIT`，重启服务不失败）；
 * - **Windows**：`SO_EXCLUSIVEADDRUSE`（**独占**，别人绑不上）；
 *   代价：`TIME_WAIT` 期间重启会 `WSAEADDRINUSE` → 配合 `SO_LINGER{1, 0}`（关闭时 RST，**跳过 `TIME_WAIT`**）。
 *
 * 详见 `docs/reports/cross-platform-syscall-audit.md`（TOP9: SO_REUSEADDR 语义反转）。
 */
#pragma once

namespace go2cpp::platform {

/**
 * @brief 给监听 socket 设置「可重启」语义（跨平台正确版本）。
 *
 * @param fd [in] socket 描述符（Linux fd / Windows `SOCKET` 转 int）。
 * @param error [out] 失败时写错误码（Linux `errno` / Windows `WSAGetLastError`）；成功时保持原值。
 * @return true=设置成功；false=失败（error 已写）。
 *
 * ## 语义
 * - Linux：`SO_REUSEADDR = 1`；
 * - Windows：`SO_EXCLUSIVEADDRUSE = 1` + `SO_LINGER {l_onoff=1, l_linger=0}`。
 *
 * ## 边界
 * - **必须在 `bind()` 之前调用**（两个平台都要求）；
 * - Windows 的 `SO_EXCLUSIVEADDRUSE` **不能与 `SO_REUSEADDR` 同时设**（会 `WSAEINVAL`）。
 */
bool SetReuseAddr(int fd, int* error = nullptr);

}  // namespace go2cpp::platform
