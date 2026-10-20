#pragma once

/**
 * @file transport.hpp
 * @brief 与 DebugServer 的跨进程传输层：Unix socket + `GO2CPP-DEBUG/1` 文本协议。
 *
 * @details 已按 `6192f825` 交付的
 *          `include/go2cpp/debug/debug_server.hpp` 对齐（2026-10-20 16:30 版本）。
 *          协议是**文本命令 + 定长头 + 精确字节 payload**，不是 JSON 帧：
 *
 * ## 协议原文（服务端定义，CLI 只做客户端）
 *
 * 请求：一行 UTF-8 命令，`\n` 结尾（服务端上限 4096 字节）：
 *
 * ```text
 * PING                      -> pong
 * LIST                      -> 组件清单
 * STATUS                    -> pid/uptime/路径/组件数/构建开关（key=value）
 * DUMP <component>|ALL      -> text（key=value，含 [component name] 段）
 * TEXT <component>|ALL      -> 同 DUMP
 * JSON <component>|ALL      -> JSON 对象
 * EVENTS [limit]            -> 事件环 text
 * HELP                      -> 命令列表
 * ```
 *
 * 响应：
 *
 * ```text
 * GO2CPP-DEBUG/1 OK <payload_bytes>\n<payload>
 * GO2CPP-DEBUG/1 ERR <code> <message>\n
 * ```
 *
 * `payload_bytes` 是 payload 的**精确字节数**（UTF-8），客户端先读头行，
 * 再按字节数读 payload。
 *
 * ## 连接模型
 *
 * 服务端 `HandleClient` **一个连接只处理一条命令**：读一行、响应、关闭。
 * 因此 `Call()` 每次都新建连接，返回前关闭；多个请求（例如 status 需要
 * `STATUS` + `JSON scheduler`）会开多个连接，这是协议允许的正常用法。
 *
 * ## 错误映射
 *
 * - `ERR <code>` → `DebugError`（继承 `CliError`，退出码 `kRemote`=5），
 *   调用方可用 `remote_code()` 区分 `unknown-component` 等可降级场景；
 * - 连接失败（socket 不存在/拒绝/权限/超时）→ `kUnavailable`=3；
 * - 头行不是 `GO2CPP-DEBUG/1`、payload 字节数非法、连接提前关闭 → `kFailure`=1。
 *
 * ## 边界与陷阱
 *
 * - payload 上限 16 MiB，超过即断开并报错，避免异常服务撑爆内存。
 * - socket 路径超过 `sun_path`（108 字节）提前报错，不截断（截断会连错路径）。
 * - 本文件只支持 POSIX；`transport.cpp` 对非 POSIX 平台编译期 `#error`。
 */

#include "cli.hpp"

#include <cstdint>
#include <string>

namespace go2cpp::cli {

/// 服务端协议版本；必须与 `go2cpp/debug/debug_server.hpp` 的 kDebugProtocol 一致。
inline constexpr const char* kDebugProtocol = "GO2CPP-DEBUG/1";

/// 一次 DebugServer 调用的目标。
struct DebugTarget {
    int pid{0};              ///< 目标进程 pid（用于默认路径与提示）。
    std::string socket_path; ///< 实际连接的 socket 路径。
    int timeout_ms{5000};    ///< 整个请求（连接 + 发送 + 读头 + 读 payload）的总超时。
};

/**
 * @brief 服务端明确返回 `ERR <code> <message>`。
 * @details 继承 `CliError`，所以未捕获时仍按退出码 5 输出；
 *          `remote_code()` 供调用方对 `unknown-component` 做降级。
 */
class DebugError : public CliError {
public:
    DebugError(std::string remote_code, const std::string& message);

    /// 服务端错误码（`bad-command` / `unknown-component` / `disabled` / `internal`）。
    const std::string& remote_code() const noexcept { return m_remote_code; }

private:
    std::string m_remote_code;
};

/// 成功响应。
struct DebugReply {
    std::string payload; ///< `OK` 后的 payload（text 或 JSON 字符串）。
};

/**
 * @brief 解析目标：校验进程存在并确定 socket 路径与超时。
 * @throws CliError `kUnavailable`：进程不存在；`kFailure`：路径过长。
 */
DebugTarget ResolveDebugTarget(int pid, int timeout_ms);

/// 默认 socket 路径（不访问文件系统）。
std::string DefaultSocketPathForPid(int pid);

/**
 * @brief 单次请求客户端；每个 `Call` 建一条连接并关闭。
 * @note 非线程安全；一个实例可以顺序调用多次。
 */
class DebugClient {
public:
    explicit DebugClient(DebugTarget target);
    ~DebugClient();

    DebugClient(const DebugClient&) = delete;
    DebugClient& operator=(const DebugClient&) = delete;

    /**
     * @brief 发送一行命令并读取完整响应。
     * @param command 不含换行；由调用方保证是合法命令。
     * @return payload。
     * @throws CliError / DebugError，见文件头错误映射。
     */
    DebugReply Call(const std::string& command);

private:
    DebugTarget m_target;
};

}  // namespace go2cpp::cli
