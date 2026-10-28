#pragma once

/**
 * @file mapping.hpp
 * @brief 错误映射扩展点：HTTP 状态码、日志等级、JSON 序列化。
 *
 * @details 默认策略只依赖错误码与错误图本身，不绑定任何 HTTP/日志库；
 *          调用方可以只替换其中一项（例如接入自己的日志等级枚举），其余项
 *          继续走默认实现。三类映射各自是 `std::function`，便于测试注入与
 *          在配置驱动的服务里运行时替换。
 *
 * ## JSON 形状
 *
 * ```json
 * {"code":15,"code_name":"WRAPPED_ERROR","type":"bento.WrappedError",
 *  "parent":"bento.Error","depth":1,"wrapping":true,
 *  "message":"ctx: cause","causes":[...]}
 * ```
 *
 * - 环/共享子图：同一对象只在首次出现处展开，后续出现序列化为 `null`；
 * - 深度上限 64；超过时当前节点追加 `"truncated":true`，不再展开；
 * - `Message()`/`UnwrapAll()` 抛异常时分别降级为 `"<message unavailable>"` /
 *   `"children_error":true`，序列化本身不抛异常（std::string 分配失败除外）。
 *
 * @author go2cpp contributors
 * @date 2026-10-28
 * @copyright MIT License
 */

#include "go2cpp/bento/error/error.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace go2cpp::bento::error {

/// @brief 与常见日志库对齐的等级；只表达“建议以什么级别记录”。
enum class LogLevel : std::uint8_t {
    kTrace = 0,
    kDebug = 1,
    kInfo = 2,
    kWarn = 3,
    kError = 4,
    kFatal = 5,
};

/// @brief 日志等级名字（大写），用于日志/诊断输出。
std::string_view LogLevelName(LogLevel level) noexcept;

using HttpStatusPolicy = std::function<int(ErrorCode)>;   ///< 错误码 -> HTTP 状态。
using LogLevelPolicy = std::function<LogLevel(ErrorCode)>; ///< 错误码 -> 日志等级。
using JsonPolicy = std::function<std::string(const ErrorPtr&)>; ///< 错误 -> JSON 文本。

/**
 * @brief 可注入的映射策略集合。
 * @details 任一项为空时对应入口自动回退到默认实现，允许只覆盖一项。
 */
struct MappingPolicy {
    HttpStatusPolicy http_status; ///< 空 = 默认 HTTP 映射。
    LogLevelPolicy log_level;     ///< 空 = 默认日志等级映射。
    JsonPolicy json;              ///< 空 = 默认 JSON 序列化。
};

/// @brief 构造一份全部使用默认实现的策略。
MappingPolicy MakeDefaultMappingPolicy();

/// @brief 进程级默认策略（函数内静态，线程安全初始化）。
const MappingPolicy& DefaultMappingPolicy();

/// @brief 默认 HTTP 映射；未知/私有码返回 500。
int DefaultHttpStatus(ErrorCode code) noexcept;

/// @brief 默认日志等级映射；未知/私有码返回 kError。
LogLevel DefaultLogLevel(ErrorCode code) noexcept;

/// @brief 默认 JSON 序列化；空指针返回字面量 `null`。
std::string DefaultJson(const ErrorPtr& error);

/// @brief 错误码 -> HTTP 状态；空策略项回退默认实现。
int MapHttpStatus(ErrorCode code, const MappingPolicy& policy = DefaultMappingPolicy());

/// @brief 错误 -> HTTP 状态；空错误视为 `kOk`。
int MapHttpStatus(const ErrorPtr& error, const MappingPolicy& policy = DefaultMappingPolicy());

/// @brief 错误码 -> 日志等级；空策略项回退默认实现。
LogLevel MapLogLevel(ErrorCode code, const MappingPolicy& policy = DefaultMappingPolicy());

/// @brief 错误 -> 日志等级；空错误视为 `kOk`（kInfo）。
LogLevel MapLogLevel(const ErrorPtr& error, const MappingPolicy& policy = DefaultMappingPolicy());

/// @brief 错误 -> JSON；空策略项回退默认实现。
std::string MapJson(const ErrorPtr& error, const MappingPolicy& policy = DefaultMappingPolicy());

} // namespace go2cpp::bento::error
