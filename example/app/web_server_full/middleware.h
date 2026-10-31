#pragma once

/**
 * @file middleware.h
 * @brief 请求处理中间件链：异常恢复、请求 ID、访问日志、CORS。
 * @details 顺序即洋葱模型：先 Use 的在最外层。推荐 Recover -> RequestId ->
 *          AccessLog -> CORS，这样异常响应也带请求 ID/CORS 头，访问日志
 *          也能记录到耗时。
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include <functional>
#include <string>
#include <vector>

#include "go2cpp/network/http.hpp"

namespace app {

/// @brief 链上的下一环（或最终 handler）。
using Next = std::function<go2cpp::http::Response(const go2cpp::http::Request&)>;

/// @brief 中间件：接收请求与 next，必须调用 next 才能继续。
using Middleware = std::function<go2cpp::http::Response(const go2cpp::http::Request&, const Next&)>;

/// @brief 固定顺序的中间件链；启动后只读，可被多个 worker 线程并发 Run。
class MiddlewareChain final {
   public:
    void Use(Middleware middleware);
    /// @brief 依次执行中间件，最后调用 terminal；任一中间件抛异常会向上抛。
    go2cpp::http::Response Run(const go2cpp::http::Request& request, const Next& terminal) const;

   private:
    std::vector<Middleware> m_middlewares;
};

/// @brief 构造 JSON 错误响应（含 ok=false 与 error 字段）。
go2cpp::http::Response MakeJsonError(go2cpp::http::Status status, const std::string& message);

/// @brief 捕获下游异常并记录日志，返回 500 JSON；最外层使用。
Middleware MakeRecoverMiddleware();

/// @brief 为每个响应生成 X-Request-Id；便于把客户端问题与服务端日志对上。
Middleware MakeRequestIdMiddleware();

/// @brief 记录方法/路径/状态/耗时，并回写 X-Response-Time-Ms。
Middleware MakeAccessLogMiddleware();

/// @brief 允许跨域并处理 OPTIONS 预检。
Middleware MakeCorsMiddleware();

}  // namespace app
