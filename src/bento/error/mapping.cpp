/**
 * @file mapping.cpp
 * @brief 错误映射默认实现：HTTP、日志等级与 JSON。
 *
 * @details 默认映射保持“纯函数”特性：不读全局配置、不写日志，方便单元测试
 *          与在任意上下文复用。JSON 序列化对深链与自指图都设了边界，保证
 *          恶意/异常错误对象不会把格式化路径拖成栈溢出或死循环。
 *
 * @author go2cpp contributors
 * @date 2026-10-28
 * @copyright MIT License
 */

#include "go2cpp/bento/error/mapping.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace go2cpp::bento::error {

namespace {

/// JSON 最大嵌套层数；超过后当前节点只保留自身字段并标记 truncated。
constexpr std::size_t kMaxJsonDepth = 64;

/// @brief 追加 JSON 字符串转义（含控制字符与中文 UTF-8 透传）。
void AppendJsonEscaped(std::string& out, std::string_view text) {
    static constexpr char kHexDigits[] = "0123456789abcdef";
    for (const char raw : text) {
        const auto ch = static_cast<unsigned char>(raw);
        switch (ch) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (ch < 0x20U) {
                    out += "\\u00";
                    out += kHexDigits[(ch >> 4U) & 0x0FU];
                    out += kHexDigits[ch & 0x0FU];
                } else {
                    out += static_cast<char>(ch);
                }
                break;
        }
    }
}

/// @brief 递归追加一个错误节点；visited 同时承担防环与共享子图去重。
void AppendErrorJson(std::string& out, const ErrorPtr& error, std::size_t depth,
                     std::unordered_set<const Error*>& visited) {
    if (!error || !visited.insert(error.get()).second) {
        out += "null";
        return;
    }

    std::string message;
    try {
        message = error->Message();
    } catch (...) {
        message = "<message unavailable>";
    }

    out += "{\"code\":";
    out += std::to_string(static_cast<std::int32_t>(error->Code()));
    out += ",\"code_name\":\"";
    out += ErrorCodeName(error->Code());
    out += "\",\"type\":\"";
    AppendJsonEscaped(out, error->TypeName());
    out += "\",\"parent\":\"";
    AppendJsonEscaped(out, error->ParentTypeName());
    out += "\",\"depth\":";
    out += std::to_string(static_cast<unsigned>(error->InheritanceDepth()));
    out += ",\"wrapping\":";
    out += error->IsWrapping() ? "true" : "false";
    out += ",\"message\":\"";
    AppendJsonEscaped(out, message);
    out += "\"";

    if (error->IsWrapping()) {
        if (depth >= kMaxJsonDepth) {
            out += ",\"truncated\":true";
        } else {
            std::vector<ErrorPtr> children;
            bool children_available = true;
            try {
                children = error->UnwrapAll();
            } catch (...) {
                children_available = false;
            }
            if (!children_available) {
                out += ",\"children_error\":true";
            } else if (!children.empty()) {
                out += ",\"causes\":[";
                bool first = true;
                for (const ErrorPtr& child : children) {
                    if (!child) {
                        continue;
                    }
                    if (!first) {
                        out += ',';
                    }
                    first = false;
                    AppendErrorJson(out, child, depth + 1, visited);
                }
                out += ']';
            }
        }
    }
    out += '}';
}

} // namespace

std::string_view LogLevelName(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::kTrace:
            return "TRACE";
        case LogLevel::kDebug:
            return "DEBUG";
        case LogLevel::kInfo:
            return "INFO";
        case LogLevel::kWarn:
            return "WARN";
        case LogLevel::kError:
            return "ERROR";
        case LogLevel::kFatal:
            return "FATAL";
    }
    return "ERROR";
}

int DefaultHttpStatus(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::kOk:
            return 200;
        case ErrorCode::kCancelled:
            return 499; // nginx 约定：客户端主动断开。
        case ErrorCode::kUnknown:
            return 500;
        case ErrorCode::kInvalidArgument:
            return 400;
        case ErrorCode::kDeadlineExceeded:
            return 504;
        case ErrorCode::kNotFound:
            return 404;
        case ErrorCode::kAlreadyExists:
            return 409;
        case ErrorCode::kPermissionDenied:
            return 403;
        case ErrorCode::kResourceExhausted:
            return 429;
        case ErrorCode::kFailedPrecondition:
            return 412;
        case ErrorCode::kOutOfRange:
            return 400;
        case ErrorCode::kUnimplemented:
            return 501;
        case ErrorCode::kInternal:
            return 500;
        case ErrorCode::kUnavailable:
            return 503;
        case ErrorCode::kDataLoss:
            return 500;
        case ErrorCode::kWrappedError:
            return 500;
    }
    return 500;
}

LogLevel DefaultLogLevel(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::kOk:
            return LogLevel::kInfo;
        case ErrorCode::kCancelled:
            return LogLevel::kDebug; // 客户端取消通常不是服务端故障。
        case ErrorCode::kUnknown:
            return LogLevel::kError;
        case ErrorCode::kInvalidArgument:
        case ErrorCode::kNotFound:
        case ErrorCode::kAlreadyExists:
        case ErrorCode::kPermissionDenied:
        case ErrorCode::kResourceExhausted:
        case ErrorCode::kFailedPrecondition:
        case ErrorCode::kOutOfRange:
        case ErrorCode::kUnimplemented:
        case ErrorCode::kUnavailable:
        case ErrorCode::kDeadlineExceeded:
            return LogLevel::kWarn;
        case ErrorCode::kInternal:
        case ErrorCode::kDataLoss:
        case ErrorCode::kWrappedError:
            return LogLevel::kError;
    }
    return LogLevel::kError;
}

std::string DefaultJson(const ErrorPtr& error) {
    if (!error) {
        return "null";
    }
    std::string out;
    out.reserve(256);
    std::unordered_set<const Error*> visited;
    AppendErrorJson(out, error, 0, visited);
    return out;
}

MappingPolicy MakeDefaultMappingPolicy() {
    MappingPolicy policy;
    policy.http_status = [](ErrorCode code) { return DefaultHttpStatus(code); };
    policy.log_level = [](ErrorCode code) { return DefaultLogLevel(code); };
    policy.json = [](const ErrorPtr& error) { return DefaultJson(error); };
    return policy;
}

const MappingPolicy& DefaultMappingPolicy() {
    static const MappingPolicy policy = MakeDefaultMappingPolicy();
    return policy;
}

int MapHttpStatus(ErrorCode code, const MappingPolicy& policy) {
    if (policy.http_status) {
        return policy.http_status(code);
    }
    return DefaultHttpStatus(code);
}

int MapHttpStatus(const ErrorPtr& error, const MappingPolicy& policy) {
    return MapHttpStatus(error ? error->Code() : ErrorCode::kOk, policy);
}

LogLevel MapLogLevel(ErrorCode code, const MappingPolicy& policy) {
    if (policy.log_level) {
        return policy.log_level(code);
    }
    return DefaultLogLevel(code);
}

LogLevel MapLogLevel(const ErrorPtr& error, const MappingPolicy& policy) {
    return MapLogLevel(error ? error->Code() : ErrorCode::kOk, policy);
}

std::string MapJson(const ErrorPtr& error, const MappingPolicy& policy) {
    if (policy.json) {
        return policy.json(error);
    }
    return DefaultJson(error);
}

} // namespace go2cpp::bento::error
