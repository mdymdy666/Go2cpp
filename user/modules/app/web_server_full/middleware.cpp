/**
 * @file middleware.cpp
 * @brief 中间件链与四个内置中间件的实现。
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include "middleware.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <string>
#include <utility>

#include "go2cpp/reflect/json.hpp"
#include "logger.h"

namespace app {

void MiddlewareChain::Use(Middleware middleware) {
    if (middleware) {
        m_middlewares.push_back(std::move(middleware));
    }
}

go2cpp::http::Response MiddlewareChain::Run(const go2cpp::http::Request& request,
                                            const Next& terminal) const {
    // 递归展开中间件；std::function 捕获自身，生命周期覆盖整次调用。
    std::function<go2cpp::http::Response(std::size_t, const go2cpp::http::Request&)> step;
    step = [this, &terminal, &step](std::size_t index, const go2cpp::http::Request& current) {
        if (index >= m_middlewares.size()) {
            return terminal(current);
        }
        const Middleware& middleware = m_middlewares[index];
        return middleware(current, [index, &step](const go2cpp::http::Request& next_request) {
            return step(index + 1, next_request);
        });
    };
    return step(0, request);
}

go2cpp::http::Response MakeJsonError(go2cpp::http::Status status, const std::string& message) {
    std::string body;
    go2cpp::reflect::JsonWriter writer(body);
    writer.BeginObject();
    writer.Key("ok");
    writer.Bool(false);
    writer.Key("error");
    writer.String(message);
    writer.Key("status");
    writer.Int(static_cast<std::int64_t>(status));
    writer.EndObject();
    go2cpp::http::Response response;
    response.status = status;
    response.header.set("Content-Type", "application/json; charset=utf-8");
    response.body = std::move(body);
    return response;
}

Middleware MakeRecoverMiddleware() {
    return [](const go2cpp::http::Request& request, const Next& next) {
        try {
            return next(request);
        } catch (const std::exception& error) {
            LogError("未捕获异常 " + go2cpp::http::method_name(request.method) + " " + request.target +
                     ": " + error.what());
            return MakeJsonError(go2cpp::http::Status::InternalError, "internal server error");
        } catch (...) {
            LogError("未捕获的非标准异常 " + go2cpp::http::method_name(request.method) + " " +
                     request.target);
            return MakeJsonError(go2cpp::http::Status::InternalError, "internal server error");
        }
    };
}

Middleware MakeRequestIdMiddleware() {
    auto counter = std::make_shared<std::atomic<std::uint64_t>>(0);
    return [counter](const go2cpp::http::Request& request, const Next& next) {
        const std::uint64_t id = counter->fetch_add(1, std::memory_order_relaxed) + 1;
        char text[32];
        std::snprintf(text, sizeof(text), "req-%08llx",
                      static_cast<unsigned long long>(id));
        auto response = next(request);
        response.header.set("X-Request-Id", text);
        return response;
    };
}

Middleware MakeAccessLogMiddleware() {
    return [](const go2cpp::http::Request& request, const Next& next) {
        const auto started = std::chrono::steady_clock::now();
        auto response = next(request);
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                                 std::chrono::steady_clock::now() - started)
                                 .count();
        const auto millis = static_cast<double>(elapsed) / 1000.0;
        char info[64];
        std::snprintf(info, sizeof(info), "%.3fms", millis);
        response.header.set("X-Response-Time-Ms", info);
        const std::string line = go2cpp::http::method_name(request.method) + " " + request.target + " -> " +
                                 std::to_string(static_cast<int>(response.status)) + " " +
                                 info;
        if (static_cast<int>(response.status) >= 500) {
            LogWarn(line);
        } else {
            LogInfo(line);
        }
        return response;
    };
}

Middleware MakeCorsMiddleware() {
    return [](const go2cpp::http::Request& request, const Next& next) {
        if (request.method == go2cpp::http::Method::Options) {
            go2cpp::http::Response response;
            response.status = go2cpp::http::Status::NoContent;
            response.header.set("Access-Control-Allow-Origin", "*");
            response.header.set("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
            response.header.set("Access-Control-Allow-Headers", "Content-Type");
            return response;
        }
        auto response = next(request);
        response.header.set("Access-Control-Allow-Origin", "*");
        return response;
    };
}

}  // namespace app
