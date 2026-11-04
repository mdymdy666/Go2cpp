/**
 * @file feed_service.cpp
 * @brief 抖音 P0 feed 服务（29103）：推荐流 / 关注流。
 *
 * @details 路由：
 *          - GET /api/feed            推荐流（无需鉴权）
 *          - GET /api/feed/following  关注流（Bearer；只含已关注作者）
 *          - GET /health, /metrics
 *
 *          两个接口都支持 `?cursor=&limit=`：`limit` 缺省 20、上限 100
 *          （<=0 或非数字按缺省处理）；响应统一为
 *          `{"ok":true,"items":[{"video":{...},"author":"..."}],"next_cursor":"..."}`。
 *          排序与游标语义见 douyin_store.hpp 文件头（热度序 + `<created_at>-<id>`）。
 *
 * @author douyin demo
 * @date 2026-11-03
 * @copyright MIT License
 */

#include "douyin_auth.hpp"
#include "douyin_http.hpp"
#include "douyin_proto.hpp"
#include "douyin_store.hpp"
#include "douyin_tag.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::uint16_t kDefaultPort = 29103;
constexpr const char* kServiceName = "feed";
constexpr std::int64_t kDefaultPageSize = 20;
constexpr std::int64_t kMaxPageSize = 100;

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
    // 推荐流游标改为「随机种子 + 起始下标」，不再按 created_at/id 定位。
    std::uint64_t seed = 0;
    std::size_t index = 0;
    return douyin::DecodeFeedCursor(*cursor, &seed, &index);
}

/// @brief 把视频行 + 作者名编成一条 feed JSON。
std::string ToFeedJson(const douyin::StoredVideo& stored,
                       const std::string& author) {
    douyin::Video video;
    video.id = stored.id;
    video.user_id = stored.user_id;
    video.title = stored.title;
    video.url = stored.url;
    video.like_count = stored.like_count;
    video.comment_count = stored.comment_count;
    video.created_at = stored.created_at;
    video.tags = douyin::ExtractTags(stored.title);

    douyin::FeedItem item;
    item.video = std::move(video);
    item.author = author;
    return douyin::ToJson(item);
}

/**
 * @brief 查询视频并补齐作者名，输出统一响应。
 * @param store 存储。
 * @param videos 已按热度序切好的一页。
 * @param next_cursor 下一页游标（空串表示没有）。
 * @param response 输出。
 */
bool WriteFeed(douyin::Store& store, const std::vector<douyin::StoredVideo>& videos,
               const std::string& next_cursor, douyin::HttpResponse* response) {
    std::string body = "{\"ok\":true,\"items\":[";
    for (std::size_t i = 0; i < videos.size(); ++i) {
        std::string author;
        std::string error;
        if (!store.FindUserName(videos[i].user_id, &author, &error) &&
            !error.empty()) {
            std::fprintf(stderr, "[%s] 查询作者失败: %s\n", kServiceName,
                         error.c_str());
            return Fail(response, 500, "数据库错误");
        }
        if (i != 0) {
            body += ',';
        }
        body += ToFeedJson(videos[i], author);
    }
    body += "],\"next_cursor\":\"" + douyin::JsonEscape(next_cursor) + "\"}";
    response->status = 200;
    response->body = std::move(body);
    return true;
}

/// @brief 推荐流。
bool HandleFeed(douyin::Store& store, const douyin::HttpRequest& request,
                douyin::HttpResponse* response) {
    std::string cursor;
    if (!ReadCursor(request, &cursor)) {
        return Fail(response, 400, "cursor 非法");
    }
    const std::int64_t limit = ParseLimit(request);

    std::vector<douyin::StoredVideo> videos;
    std::string next_cursor;
    std::string error;
    if (!store.ListFeed(cursor, limit, &videos, &next_cursor, &error)) {
        std::fprintf(stderr, "[%s] 查询推荐流失败: %s\n", kServiceName,
                     error.c_str());
        return Fail(response, 500, "数据库错误");
    }
    return WriteFeed(store, videos, next_cursor, response);
}

/// @brief 关注流（只含已关注作者）。
bool HandleFollowingFeed(douyin::Store& store, const douyin::HttpRequest& request,
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
    if (!store.ListFollowingFeed(actor, cursor, limit, &videos, &next_cursor,
                                 &error)) {
        std::fprintf(stderr, "[%s] 查询关注流失败: %s\n", kServiceName,
                     error.c_str());
        return Fail(response, 500, "数据库错误");
    }
    return WriteFeed(store, videos, next_cursor, response);
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
    if (request.path == "/api/feed") {
        if (request.method == "GET") {
            return HandleFeed(store, request, response);
        }
        return Fail(response, 405, "方法不允许");
    }
    if (request.path == "/api/feed/following") {
        if (request.method == "GET") {
            return HandleFollowingFeed(store, request, response);
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
