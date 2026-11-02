/**
 * @file router.cpp
 * @brief Router 的匹配与注册实现。
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include "router.h"

#include <sstream>
#include <utility>

namespace app {
namespace {

/// @brief 方法名，用于 Describe() 与 405 提示。
std::string MethodText(go2cpp::http::Method method) {
    return go2cpp::http::method_name(method);
}

}  // namespace

std::vector<std::string> Router::SplitPath(std::string_view path) {
    std::vector<std::string> segments;
    std::string current;
    for (const char ch : path) {
        if (ch == '/') {
            if (!current.empty()) {
                segments.push_back(current);
                current.clear();
            }
            continue;
        }
        current.push_back(ch);
    }
    if (!current.empty()) {
        segments.push_back(std::move(current));
    }
    return segments;
}

bool Router::MatchSegments(const std::vector<std::string>& pattern,
                           const std::vector<std::string>& path,
                           std::map<std::string, std::string>* params) {
    if (pattern.size() != path.size()) {
        return false;
    }
    for (std::size_t index = 0; index < pattern.size(); ++index) {
        const std::string& expected = pattern[index];
        if (expected.size() > 1 && expected.front() == ':') {
            if (path[index].empty()) {
                return false;
            }
            if (params != nullptr) {
                params->emplace(expected.substr(1), path[index]);
            }
            continue;
        }
        if (expected != path[index]) {
            return false;
        }
    }
    return true;
}

void Router::Add(go2cpp::http::Method method, std::string pattern, Handler handler) {
    Route route;
    route.method = method;
    route.segments = SplitPath(pattern);
    route.pattern = std::move(pattern);
    route.handler = std::move(handler);
    std::lock_guard<std::mutex> lock(m_mutex);
    m_routes.push_back(std::move(route));
}

bool Router::Match(const go2cpp::http::Request& request, Handler* handler,
                   std::map<std::string, std::string>* params) const {
    const std::vector<std::string> path = SplitPath(request.path());
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const Route& route : m_routes) {
        if (route.method != request.method) {
            continue;
        }
        std::map<std::string, std::string> captured;
        if (!MatchSegments(route.segments, path, &captured)) {
            continue;
        }
        if (handler != nullptr) {
            *handler = route.handler;
        }
        if (params != nullptr) {
            *params = std::move(captured);
        }
        return true;
    }
    return false;
}

std::string Router::Describe() const {
    std::ostringstream out;
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const Route& route : m_routes) {
        out << MethodText(route.method) << ' ' << route.pattern << '\n';
    }
    return out.str();
}

}  // namespace app
