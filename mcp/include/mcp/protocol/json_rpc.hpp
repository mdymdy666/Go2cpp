/**
 * @file json_rpc.hpp
 * @brief JSON-RPC 2.0 消息模型，以及 MCP 侧使用的请求解析/响应构造工具。
 *
 * @details
 * ## 为什么单独一个头文件
 * MCP 建立在 JSON-RPC 2.0 之上：一条消息要么是「请求」（有 id，等响应），
 * 要么是「通知」（无 id，不等响应），要么是「响应」。传输层只搬运字符串，
 * 这里负责把字符串变成类型化的请求、并把结果编码回标准帧，避免在
 * server 分发逻辑里反复手写 jsonrpc 字段。
 *
 * ## 边界与陷阱
 * - `id` 允许是字符串、数字或 null；**通知的判定只看「有没有 id 键」**，
 *   不能用 `id.is_null()` 判断（客户端显式传 `"id": null` 也是通知，但
 *   数字 0 是合法 id，不能当假值）。
 * - 解析失败时必须回一个带 `id: null` 的错误响应，否则客户端会一直等。
 * - MCP 的 `params` 缺省是对象；这里不替调用方补默认字段。
 */

#pragma once

#include <string>
#include <string_view>

#include "nlohmann/json.hpp"

namespace go2cpp::mcp {

using json = nlohmann::json;

/// JSON-RPC 协议版本，固定字符串。
inline constexpr std::string_view kJsonRpcVersion = "2.0";

/**
 * @brief JSON-RPC 2.0 标准错误码 + MCP 使用的服务端错误码。
 *
 * 负数区是协议保留区；-32000..-32099 留给实现（这里用 -32001 表示「工具不存在」，
 * 与 MCP 规范示例一致）。
 */
enum class RpcError : int {
    kParseError = -32700,      ///< 收到非法 JSON
    kInvalidRequest = -32600,  ///< 合法 JSON 但不是合法请求对象
    kMethodNotFound = -32601,  ///< 方法未实现
    kInvalidParams = -32602,   ///< 参数不满足方法签名
    kInternalError = -32603,   ///< 服务端内部异常
    kServerError = -32000,     ///< 通用服务端错误
    kToolNotFound = -32001,    ///< tools/call 指定的工具不存在
};

/**
 * @brief 解析后的一条请求。
 *
 * `params` 缺省为对象，调用方可按 `contains()` 判断字段是否存在；`id` 原样保留，
 * 以便响应里回填同一个 id（字符串或数字都不丢精度）。
 */
struct Request {
    json id = nullptr;                 ///< 响应回填用；通知为 null
    std::string method;                ///< 方法名，如 "tools/call"
    json params = json::object();      ///< 参数对象
    bool notification = false;         ///< 原始消息没有 id 键

    /**
     * @brief 判断该请求是否需要响应。
     * @return 通知返回 false，请求返回 true。
     */
    bool ExpectsResponse() const noexcept { return !notification; }
};

/// 解析结果：成功时 `ok == true`，失败时 `error` 是一个可直接写出的错误响应。
struct ParseOutcome {
    bool ok = false;
    Request request;
    json error = nullptr;
};

/**
 * @brief 解析一条 JSON-RPC 请求。
 * @param document 已解析的 JSON 值。
 * @return 成功返回请求对象；失败返回可直接发送的错误响应。
 */
ParseOutcome ParseRequest(const json& document);

/**
 * @brief 构造成功响应。
 * @param id 请求 id（原样回填）。
 * @param result 结果对象。
 */
json MakeResult(const json& id, json result);

/**
 * @brief 构造错误响应。
 * @param id 请求 id；无法解析时传 nullptr。
 * @param code 错误码。
 * @param message 人类可读错误信息。
 * @param data 可选的附加数据。
 */
json MakeError(const json& id, RpcError code, std::string message, json data = nullptr);

/**
 * @brief 构造通知帧（无 id）。
 */
json MakeNotification(std::string method, json params = json::object());

}  // namespace go2cpp::mcp
