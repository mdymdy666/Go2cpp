/**
 * @file api.cpp
 * @brief Api 的 handler 实现：参数校验、JSON 编码、异常与广播入口。
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include "api.h"

#include <cstdlib>
#include <functional>
#include <stdexcept>
#include <string>
#include <utility>

#include "go2cpp/reflect/json.hpp"
#include "logger.h"
#include "middleware.h"
#include "sse_hub.h"

namespace app {
namespace {

using go2cpp::reflect::JsonValue;
using go2cpp::reflect::JsonParser;
using go2cpp::reflect::JsonWriter;
using go2cpp::http::Method;
using go2cpp::http::Request;
using go2cpp::http::Response;
using go2cpp::http::Status;

/// @brief 用 JsonWriter 组装单行 JSON 对象；失败时返回空串。
std::string JsonObject(const std::function<void(JsonWriter&)>& fill) {
    std::string body;
    JsonWriter writer(body);
    writer.BeginObject();
    fill(writer);
    writer.EndObject();
    return writer.ok() ? body : std::string{};
}

/// @brief 构造 JSON 响应；编码失败降级为 500 文本，避免发送坏 JSON。
Response JsonResponse(Status status, const std::string& body) {
    if (body.empty()) {
        return MakeJsonError(Status::InternalError, "JSON 编码失败");
    }
    Response response;
    response.status = status;
    response.header.set("Content-Type", "application/json; charset=utf-8");
    response.body = body;
    return response;
}

/// @brief 读取查询参数首值；不存在返回 fallback。
std::string QueryValue(const Request& request, const std::string& key,
                       const std::string& fallback) {
    go2cpp::http::QueryValues params;
    if (!go2cpp::http::decode_query(request.query(), params)) {
        return fallback;
    }
    const auto found = params.find(key);
    if (found == params.end() || found->second.empty()) {
        return fallback;
    }
    return found->second.front();
}

}  // namespace

Api::Api() : m_started(std::chrono::steady_clock::now()) {}

void Api::NoteRequest() { m_requests.fetch_add(1, std::memory_order_acq_rel); }

void Api::SetSseHub(SseHub* hub) { m_sse = hub; }

std::int64_t Api::uptime_seconds() const {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::steady_clock::now() - m_started)
        .count();
}

void Api::Register(Router& router) {
    m_router = &router;
    router.Add(Method::Get, "/health", [this](const Request& request) { return Health(request); });
    router.Add(Method::Get, "/healthz", [this](const Request& request) { return Health(request); });
    router.Add(Method::Get, "/api/echo", [this](const Request& request) { return Echo(request); });
    router.Add(Method::Post, "/api/echo",
               [this](const Request& request) { return EchoPost(request); });
    router.Add(Method::Get, "/api/items/:id",
               [this](const Request& request) { return Item(request); });
    router.Add(Method::Get, "/api/stats", [this](const Request& request) { return Stats(request); });
    router.Add(Method::Get, "/api/time", [this](const Request& request) { return Time(request); });
    router.Add(Method::Get, "/api/routes",
               [this](const Request& request) { return Routes(request); });
    router.Add(Method::Get, "/api/boom", [this](const Request& request) { return Boom(request); });
    router.Add(Method::Post, "/api/broadcast",
               [this](const Request& request) { return Broadcast(request); });
}

Response Api::Health(const Request&) {
    const std::string body = JsonObject([this](JsonWriter& writer) {
        writer.Key("status");
        writer.String("ok");
        writer.Key("uptime_s");
        writer.Int(uptime_seconds());
        writer.Key("requests");
        writer.Uint(request_count());
        writer.Key("version");
        writer.String("web_server_full/1.0");
    });
    return JsonResponse(Status::OK, body);
}

Response Api::Echo(const Request& request) {
    std::string name = QueryValue(request, "name", "world");
    if (name.size() > 64) {
        return MakeJsonError(Status::BadRequest, "name 不能超过 64 字节");
    }
    const std::string times_text = QueryValue(request, "times", "1");
    if (times_text.size() > 2 || times_text.find_first_not_of("0123456789") != std::string::npos) {
        return MakeJsonError(Status::BadRequest, "times 必须是整数");
    }
    const int times = std::atoi(times_text.c_str());
    if (times < 1 || times > 5) {
        return MakeJsonError(Status::BadRequest, "times 范围 1..5");
    }
    const std::string body = JsonObject([&](JsonWriter& writer) {
        writer.Key("ok");
        writer.Bool(true);
        writer.Key("name");
        writer.String(name);
        writer.Key("times");
        writer.Int(times);
        writer.Key("source");
        writer.String("query");
    });
    return JsonResponse(Status::OK, body);
}

Response Api::EchoPost(const Request& request) {
    if (request.body.size() > 1024) {
        return MakeJsonError(Status::PayloadTooLarge, "POST body 不能超过 1 KiB");
    }
    std::string field = "raw";
    JsonValue parsed;
    std::string ignored;
    if (!request.body.empty() && JsonParser::Parse(request.body, parsed, nullptr, &ignored)) {
        if (const JsonValue* name = parsed.Find("name"); name != nullptr &&
                                                       name->kind == JsonValue::Kind::kString) {
            field = name->text;
        }
    }
    const std::string body = JsonObject([&](JsonWriter& writer) {
        writer.Key("ok");
        writer.Bool(true);
        writer.Key("name");
        writer.String(field.size() > 64 ? field.substr(0, 64) : field);
        writer.Key("body_bytes");
        writer.Uint(static_cast<std::uint64_t>(request.body.size()));
        writer.Key("source");
        writer.String("body");
    });
    return JsonResponse(Status::OK, body);
}

Response Api::Item(const Request& request) {
    const std::string id = request.path_value("id");
    if (id.empty()) {
        return MakeJsonError(Status::BadRequest, "缺少路径参数 id");
    }
    const std::string body = JsonObject([&](JsonWriter& writer) {
        writer.Key("ok");
        writer.Bool(true);
        writer.Key("id");
        writer.String(id);
        writer.Key("name");
        writer.String("item-" + id);
    });
    return JsonResponse(Status::OK, body);
}

Response Api::Stats(const Request&) {
    const std::size_t subscribers = (m_sse != nullptr) ? m_sse->subscriber_count() : 0;
    const std::string body = JsonObject([&](JsonWriter& writer) {
        writer.Key("requests");
        writer.Uint(request_count());
        writer.Key("uptime_s");
        writer.Int(uptime_seconds());
        writer.Key("sse_subscribers");
        writer.Uint(static_cast<std::uint64_t>(subscribers));
    });
    return JsonResponse(Status::OK, body);
}

Response Api::Time(const Request&) {
    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
    const std::string body = JsonObject([&](JsonWriter& writer) {
        writer.Key("epoch_ms");
        writer.Int(now_ms);
        writer.Key("uptime_s");
        writer.Int(uptime_seconds());
    });
    return JsonResponse(Status::OK, body);
}

Response Api::Routes(const Request&) {
    const std::string text = (m_router != nullptr) ? m_router->Describe() : std::string{};
    std::string body;
    JsonWriter writer(body);
    writer.BeginObject();
    writer.Key("ok");
    writer.Bool(true);
    writer.Key("routes");
    writer.BeginArray();
    std::size_t begin = 0;
    while (begin < text.size()) {
        const std::size_t end = text.find('\n', begin);
        const std::string line = text.substr(begin, end == std::string::npos ? std::string::npos
                                                                            : end - begin);
        writer.String(line);
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1;
    }
    writer.EndArray();
    writer.EndObject();
    return JsonResponse(Status::OK, writer.ok() ? body : std::string{});
}

Response Api::Boom(const Request&) {
    // 故意抛异常：验证 Recover 中间件把异常转成 500 JSON，而不是穿透
    // HttpServer 的 worker 线程（那会直接 terminate）。
    throw std::runtime_error("demo exception from /api/boom");
}

Response Api::Broadcast(const Request& request) {
    if (m_sse == nullptr) {
        return MakeJsonError(Status::ServiceUnavailable, "SSE 未启用");
    }
    if (request.body.size() > 4096) {
        return MakeJsonError(Status::PayloadTooLarge, "广播 body 不能超过 4 KiB");
    }
    JsonValue parsed;
    std::string error;
    if (!JsonParser::Parse(request.body, parsed, nullptr, &error)) {
        return MakeJsonError(Status::BadRequest, "body 不是合法 JSON: " + error);
    }
    const JsonValue* event = parsed.Find("event");
    const JsonValue* data = parsed.Find("data");
    if (event == nullptr || event->kind != JsonValue::Kind::kString ||
        event->text.size() > 64) {
        return MakeJsonError(Status::BadRequest, "event 必须是不超过 64 字节的字符串");
    }
    const std::string payload = (data != nullptr && data->kind == JsonValue::Kind::kString)
                                    ? data->text
                                    : std::string{};
    if (payload.size() > 2048) {
        return MakeJsonError(Status::BadRequest, "data 不能超过 2048 字节");
    }
    m_sse->Publish(event->text, payload);
    LogInfo("SSE 广播 event=" + event->text + " bytes=" + std::to_string(payload.size()));
    const std::string body = JsonObject([&](JsonWriter& writer) {
        writer.Key("ok");
        writer.Bool(true);
        writer.Key("delivered");
        writer.Uint(static_cast<std::uint64_t>(m_sse->subscriber_count()));
    });
    return JsonResponse(Status::OK, body);
}

}  // namespace app
