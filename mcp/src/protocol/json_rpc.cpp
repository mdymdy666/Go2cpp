/**
 * @file json_rpc.cpp
 * @brief JSON-RPC 2.0 解析与编码实现。
 */

#include "mcp/protocol/json_rpc.hpp"

namespace go2cpp::mcp {

namespace {

/// 从文档里取 id；缺省时返回 null（响应回填 null，符合规范）。
json ExtractId(const json& document) {
    const auto it = document.find("id");
    return it == document.end() ? json(nullptr) : *it;
}

/// 校验 id 只能是字符串、数字或 null；其他类型视为非法请求。
bool IsValidId(const json& id) {
    return id.is_null() || id.is_string() || id.is_number();
}

}  // namespace

ParseOutcome ParseRequest(const json& document) {
    ParseOutcome outcome;

    if (!document.is_object()) {
        outcome.error = MakeError(nullptr, RpcError::kParseError, "message must be a JSON object");
        return outcome;
    }

    const json id = ExtractId(document);
    if (!IsValidId(id)) {
        outcome.error = MakeError(nullptr, RpcError::kInvalidRequest, "id must be string, number or null");
        return outcome;
    }

    const auto version = document.find("jsonrpc");
    if (version == document.end() || !version->is_string() ||
        version->get<std::string>() != kJsonRpcVersion) {
        outcome.error = MakeError(id, RpcError::kInvalidRequest, "jsonrpc must be \"2.0\"");
        return outcome;
    }

    const auto method = document.find("method");
    if (method == document.end() || !method->is_string() || method->get<std::string>().empty()) {
        outcome.error = MakeError(id, RpcError::kInvalidRequest, "method must be a non-empty string");
        return outcome;
    }

    Request request;
    request.id = id;
    // 「有没有 id 键」才是通知判定依据：显式 "id": null 按规范算请求，但
    // 我们这里保持宽松——null id 无法回填响应，按通知处理不会让客户端等死。
    request.notification = (document.find("id") == document.end()) || id.is_null();
    request.method = method->get<std::string>();

    const auto params = document.find("params");
    if (params != document.end() && !params->is_null()) {
        if (!params->is_object() && !params->is_array()) {
            outcome.error = MakeError(id, RpcError::kInvalidParams, "params must be object or array");
            return outcome;
        }
        request.params = *params;
    }

    outcome.ok = true;
    outcome.request = std::move(request);
    return outcome;
}

json MakeResult(const json& id, json result) {
    json response = json::object();
    response["jsonrpc"] = std::string(kJsonRpcVersion);
    response["id"] = id;
    response["result"] = std::move(result);
    return response;
}

json MakeError(const json& id, RpcError code, std::string message, json data) {
    json response = json::object();
    response["jsonrpc"] = std::string(kJsonRpcVersion);
    response["id"] = id;
    json error = json::object();
    error["code"] = static_cast<int>(code);
    error["message"] = std::move(message);
    if (!data.is_null()) {
        error["data"] = std::move(data);
    }
    response["error"] = std::move(error);
    return response;
}

json MakeNotification(std::string method, json params) {
    json message = json::object();
    message["jsonrpc"] = std::string(kJsonRpcVersion);
    message["method"] = std::move(method);
    if (!params.is_null()) {
        message["params"] = std::move(params);
    }
    return message;
}

}  // namespace go2cpp::mcp
