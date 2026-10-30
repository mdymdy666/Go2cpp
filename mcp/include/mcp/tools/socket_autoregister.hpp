/**
 * @file socket_autoregister.hpp
 * @brief 把 `go2cpp::net::Socket` 的生命周期自动同步到 MCP `SocketRegistry`。
 * @author go2cpp contributors
 * @date 2026-10-30
 * @copyright MIT License
 *
 * @details
 * 背景：`src/network/socket.cpp` 在创建/关闭/移交 fd 时提供一个可选的进程级
 * 观察者槽位（C 链接，默认空）。本模块在 MCP 启动时安装该观察者，于是业务
 * 通过 `go2cpp::net::Socket` / `ListenTCP` / `DialTCP` 创建的 socket（含
 * accept 出来的连接）会自动进入 `SocketRegistry` 的可关闭白名单，
 * `socket_list` 立即可见、`socket_close` 可以关闭。
 *
 * ## 依赖方向
 *
 * **network ← mcp**，而不是反向。`src/network/socket.cpp` 不 include 任何
 * MCP 头、不链接 `mcp_core`；它只导出
 * `go2cpp_network_set_socket_observer(void (*)(int, int))`。本头文件手写这条
 * C ABI 声明（不 include network 头），保证基座在 `GO2CPP_WITH_MCP=OFF` 时
 * 完全不感知 MCP。
 *
 * ## 语义与边界
 *
 * - **只覆盖安装之后**创建的 `go2cpp::net::Socket`：安装前已存在的 socket
 *   不会被补登记（它们仍以 `registered=false` 只读展示，可用
 *   `SocketRegistry::Register()` 显式登记）。
 * - **跳过非 socket**：观察者会对每个 `Socket` 构造回调，但 `Socket` 可以
 *   包装 socketpair/管道 fd；`SocketRegistry::Register()` 内部用
 *   `/proc/self/fd` 校验 `socket:[inode]`，非 socket 直接忽略。
 * - **关闭在 `::close(fd)` 之前通知**：fd 仍指向原 socket，注册表用 inode
 *   校验身份；fd 号被内核复用给新 socket 时不会误注销新条目（ABA 保护）。
 * - **`release()` 发 detached 事件**：fd 所有权移交调用方，只移出白名单，
 *   不写 closed 事件（fd 仍存活）。
 * - **MCP 自身 fd 可排除**：HTTP 传输的监听 socket 与接入连接调用
 *   `ExcludeSocketFromAutoRegister()`，避免 `socket_close` 关闭服务自身。
 *   排除标记在该 fd 收到 closed/detached 事件时自动清除，防止 fd 复用后
 *   错误地跳过新 socket。
 * - 观察者回调是 noexcept 的 best-effort 同步：注册表满、内存分配失败等
 *   只会导致该 fd 未登记，**不会**影响 socket 的创建/关闭/收发。
 */

#pragma once

#include <cstddef>

namespace go2cpp::mcp::tools {

/// 观察者事件码：与 `src/network/socket.cpp` 的 C ABI 对齐（改动必须同步，
/// `test_socket_autoregister` 会断言这些常量与基座一致）。
inline constexpr int kSocketObserverCreated = 1;   ///< fd 已创建（含 accept）。
inline constexpr int kSocketObserverClosed = 2;    ///< fd 即将关闭，仍然有效。
inline constexpr int kSocketObserverDetached = 3;  ///< release()：fd 已移交。

/// 自动登记条目的 label（`socket_list` 里可与显式登记区分）。
inline constexpr const char* kSocketAutoRegisterLabel =
    "auto:go2cpp::net::Socket";

/// 观察者函数指针类型（与基座 C ABI 完全一致）。
using SocketObserverFn = void (*)(int fd, int event);

/// 基座导出的 C 链接安装函数（定义在 `src/network/socket.cpp`）。
extern "C" void go2cpp_network_set_socket_observer(SocketObserverFn observer);

/**
 * @brief 安装自动登记观察者；幂等，重复调用只更新函数指针。
 * @note 只在 MCP 工具注册路径调用（`RegisterSocketTools`），未启用 MCP 的
 *       进程永远不会安装，基座热路径只剩一次原子读。
 */
void InstallSocketAutoRegister();

/// 卸载观察者，并把所有自动登记条目移出白名单（测试/进程收尾用）。
void UninstallSocketAutoRegister();

/**
 * @brief 把 MCP 自身的 fd（监听 socket、接入连接）排除在自动登记之外。
 * @details 已自动登记时立即移出白名单（不关闭 fd）。排除标记在这个 fd
 *          收到 closed/detached 事件时清除，避免 fd 号复用后误伤新 socket。
 */
void ExcludeSocketFromAutoRegister(int fd);

/// 撤销排除；该 fd 下次创建时会重新自动登记。
void IncludeSocketInAutoRegister(int fd);

/**
 * @brief 忘记一个已由外部关闭的自动登记 fd（`socket_close` 工具路径）。
 * @details 工具直接 `::close(fd)`、绕过 `Socket` 对象，不会触发观察者的
 *          closed 事件；这里同步移除自动登记集合与排除标记，避免 fd 号
 *          被复用后 `IsSocketAutoRegistered()`/注销逻辑误判。
 */
void ForgetAutoRegisteredSocket(int fd);

/// 当前自动登记且仍在白名单内的 fd 数量。
std::size_t AutoRegisteredSocketCount();

/// fd 是否由自动登记写入白名单。
bool IsSocketAutoRegistered(int fd);

}  // namespace go2cpp::mcp::tools
