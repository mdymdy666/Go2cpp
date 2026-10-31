#pragma once

/**
 * @file router.h
 * @brief 方法 + 路径匹配的极简路由器，支持 `:name` 路径参数。
 * @details 只实现示例需要的两种段：字面量与 `:param`。匹配结果通过
 *          `params` 返回而不是写回 Request，因为 HttpServer 交付的是
 *          const 请求；调用方需要的话自行复制 Request 再填 path_values。
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "go2cpp/network/http.hpp"

namespace app {

/// @brief 路由处理函数，返回完整响应。
using Handler = std::function<go2cpp::http::Response(const go2cpp::http::Request&)>;

/// @brief 线程安全的路由表；注册发生在启动期，匹配发生在 worker 线程。
class Router final {
   public:
    /// @brief 注册路由；pattern 形如 `/api/items/:id`。
    void Add(go2cpp::http::Method method, std::string pattern, Handler handler);

    /// @brief 匹配请求方法与路径；命中时填 handler 与路径参数。
    bool Match(const go2cpp::http::Request& request, Handler* handler,
               std::map<std::string, std::string>* params) const;

    /// @brief 路由表文本（每行 "METHOD pattern"），用于 /api/routes 与启动日志。
    std::string Describe() const;

   private:
    struct Route {
        go2cpp::http::Method method{go2cpp::http::Method::Get};
        std::vector<std::string> segments;
        std::string pattern;
        Handler handler;
    };

    /// @brief 按 '/' 切段并丢弃空段，`/a//b/` 与 `/a/b` 等价。
    static std::vector<std::string> SplitPath(std::string_view path);
    /// @brief 逐段匹配：`:name` 捕获非空段，其余要求字面量相等。
    static bool MatchSegments(const std::vector<std::string>& pattern,
                              const std::vector<std::string>& path,
                              std::map<std::string, std::string>* params);

    mutable std::mutex m_mutex;      ///< 保护 m_routes；临界区内不调用用户 handler。
    std::vector<Route> m_routes;     ///< 注册顺序即匹配顺序。
};

}  // namespace app
