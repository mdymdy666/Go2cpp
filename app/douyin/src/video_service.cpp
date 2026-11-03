/**
 * @file video_service.cpp
 * @brief 抖音 P0 video 服务（29102）：发布 / 详情 / 我的视频 / 删除。
 *
 * @details 路由：
 *          - POST   /api/video            发布（Bearer；缺 title/url → 400）
 *          - GET    /api/video/:id        详情（含 author 名 / like_count /
 *                                         comment_count；不存在 → 404）
 *          - GET    /api/video/mine       我的视频（Bearer；cursor 分页、
 *                                         热度序；响应
 *                                         `{"ok":true,"items":[{"video":...}],
 *                                         "next_cursor":"..."}`）
 *          - DELETE /api/video/:id        删除（Bearer；非作者 403、不存在 404）
 *          - GET    /health, /metrics
 *
 * @author douyin demo
 * @date 2026-11-03
 * @copyright MIT License
 */

#include "douyin_auth.hpp"
#include "douyin_http.hpp"
#include "douyin_proto.hpp"
#include "douyin_store.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::uint16_t kDefaultPort = 29102;
constexpr const char* kServiceName = "video";
constexpr std::int64_t kDefaultPageSize = 20;
constexpr std::int64_t kMaxPageSize = 100;
constexpr std::size_t kMaxTitleBytes = 256;
constexpr std::size_t kMaxUrlBytes = 2048;

/// @brief 单线程事件循环上的计数器（无需原子）。
struct ServiceMetrics {
    std::uint64_t requests{0};
    std::uint64_t errors{0};
};

ServiceMetrics g_metrics;

/// @brief 当前 Unix 秒。
std::int64_t NowSeconds() {
    return static_cast<std::int64_t>(std::time(nullptr));
}

/// @brief 统一错误响应。
bool Fail(douyin::HttpResponse* response, int status, std::string_view message) {
    response->status = status;
    response->body = douyin::ErrorBody(status, message);
    ++g_metrics.errors;
    return true;
}

/// @brief 解析并校验 Bearer token。
bool RequireToken(const douyin::HttpRequest& request, douyin::HttpResponse* response,
                  std::int64_t* user_id) {
    const std::string token = douyin::BearerToken(request);
    if (token.empty()) {
        Fail(response, 401, "缺少 Bearer token");
        return false;
    }
    douyin::TokenInfo info;
    std::string error;
    if (!douyin::VerifyToken(token, NowSeconds(), &info, &error)) {
        Fail(response, 401, error.empty() ? "token 无效" : error);
        return false;
    }
    *user_id = info.user_id;
    return true;
}

/// @brief 把存储行转成对外 Video。
douyin::Video PublicVideo(const douyin::StoredVideo& stored) {
    douyin::Video video;
    video.id = stored.id;
    video.user_id = stored.user_id;
    video.title = stored.title;
    video.url = stored.url;
    video.like_count = stored.like_count;
    video.comment_count = stored.comment_count;
    video.created_at = stored.created_at;
    return video;
}

/// @brief 解析 `limit`：缺省 20，夹紧到 [1,100]；非数字按缺省处理。
std::int64_t ParseLimit(const douyin::HttpRequest& request) {
    const std::string text = douyin::QueryParam(request, "limit");
    if (text.empty()) {
        return kDefaultPageSize;
    }
    char* end = nullptr;
    const long long value = std::strtoll(text.c_str(), &end, 10);
    if (end == text.c_str()) {
        return kDefaultPageSize;
    }
    if (value <= 0) {
        return kDefaultPageSize;
    }
    if (value > kMaxPageSize) {
        return kMaxPageSize;
    }
    return static_cast<std::int64_t>(value);
}

/// @brief 读取 `cursor`；空串合法（第一页），格式非法返回 false。
bool ReadCursor(const douyin::HttpRequest& request, std::string* cursor) {
    *cursor = douyin::QueryParam(request, "cursor");
    if (cursor->empty()) {
        return true;
    }
    std::int64_t created_at = 0;
    std::int64_t id = 0;
    return douyin::DecodeVideoCursor(*cursor, &created_at, &id);
}

/// @brief 从 `/api/video/<id>` 解析 id；`/api/video/mine` 等非数字路径返回 false。
bool ParseVideoPath(const std::string& path, std::int64_t* id) {
    constexpr std::string_view kPrefix = "/api/video/";
    if (path.size() <= kPrefix.size() ||
        path.compare(0, kPrefix.size(), kPrefix.data(), kPrefix.size()) != 0) {
        return false;
    }
    const std::string rest = path.substr(kPrefix.size());
    if (rest.empty() || rest.size() > 18) {
        return false;
    }
    for (const char c : rest) {
        if (c < '0' || c > '9') {
            return false;
        }
    }
    *id = std::atoll(rest.c_str());
    return *id > 0;
}

/// @brief 发布视频。
bool HandleCreate(douyin::Store& store, const douyin::HttpRequest& request,
                  douyin::HttpResponse* response) {
    std::int64_t actor = 0;
    if (!RequireToken(request, response, &actor)) {
        return true;
    }
    std::string title;
    std::string url;
    if (!douyin::JsonStringField(request.body, "title", &title) ||
        !douyin::JsonStringField(request.body, "url", &url) || title.empty() ||
        url.empty()) {
        return Fail(response, 400, "title/url 不能为空");
    }
    if (title.size() > kMaxTitleBytes || url.size() > kMaxUrlBytes) {
        return Fail(response, 400, "字段过长");
    }

    std::string error;
    const std::int64_t id = store.CreateVideo(actor, title, url, &error);
    if (id <= 0) {
        std::fprintf(stderr, "[%s] 发布视频失败: %s\n", kServiceName, error.c_str());
        return Fail(response, 500, "数据库错误");
    }

    douyin::StoredVideo stored;
    stored.id = id;
    stored.user_id = actor;
    stored.title = title;
    stored.url = url;
    stored.created_at = NowSeconds();
    response->status = 200;
    response->body = "{\"ok\":true,\"video\":" +
                     douyin::ToJson(PublicVideo(stored)) + "}";
    return true;
}

/// @brief 视频详情（附作者名）。
bool HandleDetail(douyin::Store& store, std::int64_t id,
                  douyin::HttpResponse* response) {
    douyin::StoredVideo stored;
    std::string error;
    if (!store.FindVideo(id, &stored, &error)) {
        if (!error.empty()) {
            std::fprintf(stderr, "[%s] 查询视频失败: %s\n", kServiceName,
                         error.c_str());
            return Fail(response, 500, "数据库错误");
        }
        return Fail(response, 404, "视频不存在");
    }

    std::string author;
    if (!store.FindUserName(stored.user_id, &author, &error) && !error.empty()) {
        std::fprintf(stderr, "[%s] 查询作者失败: %s\n", kServiceName, error.c_str());
        return Fail(response, 500, "数据库错误");
    }

    response->status = 200;
    response->body = "{\"ok\":true,\"video\":" +
                     douyin::ToJson(PublicVideo(stored)) + ",\"author\":\"" +
                     douyin::JsonEscape(author) + "\"}";
    return true;
}

/// @brief 我的视频（热度序 + cursor 分页）。
bool HandleMine(douyin::Store& store, const douyin::HttpRequest& request,
                douyin::HttpResponse* response) {
    std::int64_t actor = 0;
    if (!RequireToken(request, response, &actor)) {
        return true;
    }
    std::string cursor;
    if (!ReadCursor(request, &cursor)) {
        return Fail(response, 400, "cursor 非法");
    }
    const std::int64_t limit = ParseLimit(request);

    std::vector<douyin::StoredVideo> videos;
    std::string next_cursor;
    std::string error;
    if (!store.ListByUser(actor, cursor, limit, &videos, &next_cursor, &error)) {
        std::fprintf(stderr, "[%s] 查询我的视频失败: %s\n", kServiceName,
                     error.c_str());
        return Fail(response, 500, "数据库错误");
    }

    std::string body = "{\"ok\":true,\"items\":[";
    for (std::size_t i = 0; i < videos.size(); ++i) {
        if (i != 0) {
            body += ',';
        }
        body += "{\"video\":" + douyin::ToJson(PublicVideo(videos[i])) + "}";
    }
    body += "],\"next_cursor\":\"" + douyin::JsonEscape(next_cursor) + "\"}";
    response->status = 200;
    response->body = std::move(body);
    return true;
}

/// @brief 删除视频（仅作者）。
bool HandleDelete(douyin::Store& store, const douyin::HttpRequest& request,
                  std::int64_t id, douyin::HttpResponse* response) {
    std::int64_t actor = 0;
    if (!RequireToken(request, response, &actor)) {
        return true;
    }
    douyin::StoredVideo stored;
    std::string error;
    if (!store.FindVideo(id, &stored, &error)) {
        if (!error.empty()) {
            std::fprintf(stderr, "[%s] 查询视频失败: %s\n", kServiceName,
                         error.c_str());
            return Fail(response, 500, "数据库错误");
        }
        return Fail(response, 404, "视频不存在");
    }
    if (stored.user_id != actor) {
        return Fail(response, 403, "只有作者可以删除");
    }
    const int changed = store.DeleteVideo(id, actor, &error);
    if (changed < 0) {
        std::fprintf(stderr, "[%s] 删除视频失败: %s\n", kServiceName,
                     error.c_str());
        return Fail(response, 500, "数据库错误");
    }
    if (changed == 0) {
        return Fail(response, 404, "视频不存在");
    }
    response->status = 200;
    response->body = "{\"ok\":true,\"deleted\":true,\"id\":" +
                     std::to_string(id) + "}";
    return true;
}

/// @brief 总路由。
bool Handle(douyin::Store& store, const douyin::HttpRequest& request,
            douyin::HttpResponse* response) {
    ++g_metrics.requests;
    if (request.method == "GET" && request.path == "/health") {
        response->status = 200;
        response->body = std::string("{\"ok\":true,\"service\":\"") + kServiceName + "\"}";
        return true;
    }
    if (request.method == "GET" && request.path == "/metrics") {
        response->status = 200;
        response->body = std::string("{\"ok\":true,\"service\":\"") + kServiceName +
                         "\",\"requests\":" + std::to_string(g_metrics.requests) +
                         ",\"errors\":" + std::to_string(g_metrics.errors) + "}";
        return true;
    }
    if (request.path == "/api/video") {
        if (request.method == "POST") {
            return HandleCreate(store, request, response);
        }
        return Fail(response, 405, "方法不允许");
    }
    if (request.path == "/api/video/mine") {
        if (request.method == "GET") {
            return HandleMine(store, request, response);
        }
        return Fail(response, 405, "方法不允许");
    }

    std::int64_t id = 0;
    if (!ParseVideoPath(request.path, &id)) {
        return false;
    }
    if (request.method == "GET") {
        return HandleDetail(store, id, response);
    }
    if (request.method == "DELETE") {
        return HandleDelete(store, request, id, response);
    }
    return Fail(response, 405, "方法不允许");
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
        const std::filesystem::path parent = std::filesystem::path(db_path).parent_path();
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
