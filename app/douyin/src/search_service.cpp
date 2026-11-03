/**
 * @file search_service.cpp
 * @brief 抖音 P0 search 服务（29106）：综合搜索（user/video/live）。
 *
 * @details 路由：
 *          - GET /api/search?q=&type=&limit=&cursor=
 *
 *          `q` 非空（空/全空白 → 400）；`type` ∈ user/video/live/all
 *          （缺省 all）；`limit` 缺省 20、上限 100。匹配规则：`name`/`title`
 *          包含 q（`lower(...) LIKE lower('%q%')`，大小写不敏感；`%`/`_`
 *          按字面量转义）。排序按 id 倒序；`cursor` 是十进制 offset（空串表示
 *          第一页），`next_cursor` 只在还有下一页时非空。
 *
 *          live 表由 C7 创建，本阶段没有房间数据：`type=live` 与 `type=all`
 *          的 live 部分都返回空列表，不查询数据库、也不报错。
 *
 * @author douyin demo
 * @date 2026-11-03
 * @copyright MIT License
 */

#include "douyin_http.hpp"
#include "douyin_proto.hpp"
#include "douyin_store.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::uint16_t kDefaultPort = 29106;
constexpr const char* kServiceName = "search";
constexpr std::int64_t kDefaultPageSize = 20;
constexpr std::int64_t kMaxPageSize = 100;

/// @brief 单线程事件循环上的计数器（无需原子）。
struct ServiceMetrics {
    std::uint64_t requests{0};
    std::uint64_t errors{0};
};

ServiceMetrics g_metrics;

/// @brief 统一错误响应。
bool Fail(douyin::HttpResponse* response, int status, std::string_view message) {
    response->status = status;
    response->body = douyin::ErrorBody(status, message);
    ++g_metrics.errors;
    return true;
}

/// @brief 十六进制数字 → 0~15；非法返回 -1。
int HexDigit(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/// @brief 最小 URL 解码（`%XX` 与 `+`）；非法序列按字面量保留。
std::string UrlDecode(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '+') {
            out += ' ';
            continue;
        }
        if (text[i] == '%' && i + 2 < text.size()) {
            const int high = HexDigit(text[i + 1]);
            const int low = HexDigit(text[i + 2]);
            if (high >= 0 && low >= 0) {
                out += static_cast<char>(high * 16 + low);
                i += 2;
                continue;
            }
        }
        out += text[i];
    }
    return out;
}

/// @brief 解析 `limit`：缺省 20，夹紧到 [1,100]；非数字按缺省处理。
std::int64_t ParseLimit(const douyin::HttpRequest& request) {
    const std::string text = douyin::QueryParam(request, "limit");
    if (text.empty()) {
        return kDefaultPageSize;
    }
    char* end = nullptr;
    const long long value = std::strtoll(text.c_str(), &end, 10);
    if (end == text.c_str() || value <= 0) {
        return kDefaultPageSize;
    }
    if (value > kMaxPageSize) {
        return kMaxPageSize;
    }
    return static_cast<std::int64_t>(value);
}

/// @brief 读取 offset 游标；空串 → 0，非法（非纯数字/溢出）→ false。
bool ReadOffset(const douyin::HttpRequest& request, std::int64_t* offset) {
    const std::string text = douyin::QueryParam(request, "cursor");
    *offset = 0;
    if (text.empty()) {
        return true;
    }
    if (text.size() > 18) {
        return false;
    }
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
    }
    *offset = std::atoll(text.c_str());
    // 防御超大 offset：demo 数据量很小，超过 10 万没有实际意义。
    return *offset <= 100000;
}

/// @brief 搜索词是否为空或全空白。
bool IsBlank(const std::string& text) {
    for (const char c : text) {
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
            return false;
        }
    }
    return true;
}

/// @brief 一条结果项。
struct ResultItem {
    std::string type;
    std::int64_t id{0};
    std::string title;
};

/// @brief 结果项 → JSON（复用共享 IDL 的 SearchResult 序列化）。
std::string ToItemJson(const ResultItem& item) {
    douyin::SearchResult result;
    result.type = item.type;
    result.id = item.id;
    result.title = item.title;
    return douyin::ToJson(result);
}

/// @brief 搜索路由。
bool HandleSearch(douyin::Store& store, const douyin::HttpRequest& request,
                  douyin::HttpResponse* response) {
    const std::string query = UrlDecode(douyin::QueryParam(request, "q"));
    if (IsBlank(query)) {
        return Fail(response, 400, "q 不能为空");
    }

    std::string type = douyin::QueryParam(request, "type");
    if (type.empty()) {
        type = "all";
    }
    if (type != "user" && type != "video" && type != "live" && type != "all") {
        return Fail(response, 400, "type 非法（user/video/live/all）");
    }

    std::int64_t offset = 0;
    if (!ReadOffset(request, &offset)) {
        return Fail(response, 400, "cursor 非法");
    }
    const std::int64_t limit = ParseLimit(request);

    std::string error;
    std::vector<ResultItem> items;
    bool has_more = false;

    if (type == "live") {
        // C7 之前没有 rooms 表，固定空列表。
    } else if (type == "user" || type == "video") {
        const std::int64_t want = limit + 1;
        if (type == "user") {
            std::vector<douyin::StoredUser> users;
            if (!store.SearchUsers(query, want, offset, &users, &error)) {
                std::fprintf(stderr, "[%s] 搜索用户失败: %s\n", kServiceName,
                             error.c_str());
                return Fail(response, 500, "数据库错误");
            }
            if (static_cast<std::int64_t>(users.size()) > limit) {
                has_more = true;
                users.resize(static_cast<std::size_t>(limit));
            }
            for (const douyin::StoredUser& user : users) {
                items.push_back(ResultItem{"user", user.id, user.name});
            }
        } else {
            std::vector<douyin::StoredVideo> videos;
            if (!store.SearchVideos(query, want, offset, &videos, &error)) {
                std::fprintf(stderr, "[%s] 搜索视频失败: %s\n", kServiceName,
                             error.c_str());
                return Fail(response, 500, "数据库错误");
            }
            if (static_cast<std::int64_t>(videos.size()) > limit) {
                has_more = true;
                videos.resize(static_cast<std::size_t>(limit));
            }
            for (const douyin::StoredVideo& video : videos) {
                items.push_back(ResultItem{"video", video.id, video.title});
            }
        }
    } else {
        // all：用户在前、视频在后（固定顺序），live 为空。
        const std::int64_t span = offset + limit + 1;
        std::vector<douyin::StoredUser> users;
        std::vector<douyin::StoredVideo> videos;
        if (!store.SearchUsers(query, span, 0, &users, &error)) {
            std::fprintf(stderr, "[%s] 搜索用户失败: %s\n", kServiceName,
                         error.c_str());
            return Fail(response, 500, "数据库错误");
        }
        if (!store.SearchVideos(query, span, 0, &videos, &error)) {
            std::fprintf(stderr, "[%s] 搜索视频失败: %s\n", kServiceName,
                         error.c_str());
            return Fail(response, 500, "数据库错误");
        }
        std::vector<ResultItem> combined;
        combined.reserve(users.size() + videos.size());
        for (const douyin::StoredUser& user : users) {
            combined.push_back(ResultItem{"user", user.id, user.name});
        }
        for (const douyin::StoredVideo& video : videos) {
            combined.push_back(ResultItem{"video", video.id, video.title});
        }
        const std::size_t start = static_cast<std::size_t>(offset);
        if (start < combined.size()) {
            const std::size_t remaining = combined.size() - start;
            const std::size_t take =
                remaining < static_cast<std::size_t>(limit)
                    ? remaining
                    : static_cast<std::size_t>(limit);
            for (std::size_t i = 0; i < take; ++i) {
                items.push_back(combined[start + i]);
            }
            has_more = start + take < combined.size();
        }
    }

    std::string body = "{\"ok\":true,\"items\":[";
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i != 0) {
            body += ',';
        }
        body += ToItemJson(items[i]);
    }
    body += "],\"next_cursor\":\"";
    if (has_more) {
        body += std::to_string(offset + limit);
    }
    body += "\"}";
    response->status = 200;
    response->body = std::move(body);
    return true;
}

/// @brief 总路由。
bool Handle(douyin::Store& store, const douyin::HttpRequest& request,
            douyin::HttpResponse* response) {
    ++g_metrics.requests;
    if (request.method == "GET" && request.path == "/health") {
        response->status = 200;
        response->body = std::string("{\"ok\":true,\"service\":\"") +
                         kServiceName + "\"}";
        return true;
    }
    if (request.method == "GET" && request.path == "/metrics") {
        response->status = 200;
        response->body = std::string("{\"ok\":true,\"service\":\"") +
                         kServiceName +
                         "\",\"requests\":" + std::to_string(g_metrics.requests) +
                         ",\"errors\":" + std::to_string(g_metrics.errors) + "}";
        return true;
    }
    if (request.path == "/api/search") {
        if (request.method == "GET") {
            return HandleSearch(store, request, response);
        }
        return Fail(response, 405, "方法不允许");
    }
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    std::uint16_t port = kDefaultPort;
    if (argc > 1) {
        port = static_cast<std::uint16_t>(std::atoi(argv[1]));
    }

    std::string db_path = douyin::Store::kDefaultPath;
    if (const char* env = std::getenv("DOUYIN_DB"); env != nullptr && *env != '\0') {
        db_path = env;
    }
    {
        const std::filesystem::path parent =
            std::filesystem::path(db_path).parent_path();
        if (!parent.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(parent, ec);
        }
    }

    douyin::Store store;
    std::string error;
    if (!store.Open(db_path, &error)) {
        std::fprintf(stderr, "[%s] %s\n", kServiceName, error.c_str());
        return 1;
    }
    if (!store.EnsureSchema(&error)) {
        std::fprintf(stderr, "[%s] %s\n", kServiceName, error.c_str());
        return 1;
    }

    douyin::HttpServer server(kServiceName, port);
    if (!server.Start(&error)) {
        std::fprintf(stderr, "[%s] %s\n", kServiceName, error.c_str());
        return 1;
    }
    std::printf("[%s] 监听 0.0.0.0:%u db=%s\n", kServiceName,
                static_cast<unsigned>(port), db_path.c_str());
    std::fflush(stdout);

    server.ServeForever([&store](const douyin::HttpRequest& request,
                                 douyin::HttpResponse* response) {
        return Handle(store, request, response);
    });
    return 0;
}
