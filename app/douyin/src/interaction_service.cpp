/**
 * @file interaction_service.cpp
 * @brief 抖音 P0 interaction 服务（29104）：点赞 / 取消赞 / 评论 / 评论列表。
 *
 * @details 路由：
 *          - POST   /api/like              点赞（Bearer；幂等）
 *          - DELETE /api/like              取消赞（Bearer；幂等）
 *          - POST   /api/comment           发评论（Bearer；1~500 字）
 *          - GET    /api/comment/:video_id 评论列表（倒序 + cursor 分页）
 *          - GET    /health, /metrics
 *
 *          计数同步：每次点赞/取消/评论后调用 `Store::UpdateVideoCounts`
 *          （单事务内按 likes/comments 实表重算），因此 `GET /api/video/:id`
 *          读到的 like_count / comment_count 与实表一致。
 *
 * @author douyin demo
 * @date 2026-11-03
 * @copyright MIT License
 */

#include "douyin_auth.hpp"
#include "douyin_http.hpp"
#include "douyin_proto.hpp"
#include "douyin_store.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::uint16_t kDefaultPort = 29104;
constexpr const char* kServiceName = "interaction";
constexpr std::int64_t kDefaultPageSize = 20;
constexpr std::int64_t kMaxPageSize = 100;
constexpr std::size_t kMaxCommentChars = 500;

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
    if (end == text.c_str() || value <= 0) {
        return kDefaultPageSize;
    }
    if (value > kMaxPageSize) {
        return kMaxPageSize;
    }
    return static_cast<std::int64_t>(value);
}

/// @brief 读取评论游标；空串合法，格式非法返回 false。
bool ReadCursor(const douyin::HttpRequest& request, std::string* cursor) {
    *cursor = douyin::QueryParam(request, "cursor");
    if (cursor->empty()) {
        return true;
    }
    std::int64_t created_at = 0;
    std::int64_t id = 0;
    return douyin::DecodeCommentCursor(*cursor, &created_at, &id);
}

/**
 * @brief 从 JSON 文本取整数字段（如 `{"video_id":12}`）。
 * @return 字段存在且能解析出正数时返回 true。
 */
bool JsonIntField(std::string_view body, std::string_view key, std::int64_t* out) {
    const std::string needle = "\"" + std::string(key) + "\"";
    std::size_t pos = body.find(needle);
    if (pos == std::string_view::npos) {
        return false;
    }
    pos = body.find(':', pos + needle.size());
    if (pos == std::string_view::npos) {
        return false;
    }
    ++pos;
    while (pos < body.size() && (body[pos] == ' ' || body[pos] == '\t')) {
        ++pos;
    }
    if (pos >= body.size()) {
        return false;
    }
    const std::string rest(body.substr(pos));
    char* end = nullptr;
    const long long value = std::strtoll(rest.c_str(), &end, 10);
    if (end == rest.c_str()) {
        return false;
    }
    *out = static_cast<std::int64_t>(value);
    return true;
}

/// @brief 从 `/api/comment/<video_id>` 解析视频 id；非数字返回 false。
bool ParseVideoPath(const std::string& path, std::int64_t* id) {
    constexpr std::string_view kPrefix = "/api/comment/";
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

/// @brief UTF-8 码点个数（按首字节计；非法字节各算一个）。
std::size_t Utf8Length(const std::string& text) {
    std::size_t count = 0;
    for (const char c : text) {
        if ((static_cast<unsigned char>(c) & 0xC0U) != 0x80U) {
            ++count;
        }
    }
    return count;
}

/// @brief 是否全为空白（评论内容只有空格/制表/换行时按空处理）。
bool IsBlank(const std::string& text) {
    for (const char c : text) {
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
            return false;
        }
    }
    return true;
}

/// @brief 存储行 → 对外 Comment。
douyin::Comment PublicComment(const douyin::StoredComment& stored) {
    douyin::Comment comment;
    comment.id = stored.id;
    comment.video_id = stored.video_id;
    comment.user_id = stored.user_id;
    comment.content = stored.content;
    comment.created_at = stored.created_at;
    return comment;
}

/// @brief 点赞（幂等）。
bool HandleLike(douyin::Store& store, const douyin::HttpRequest& request,
                douyin::HttpResponse* response) {
    std::int64_t actor = 0;
    if (!RequireToken(request, response, &actor)) {
        return true;
    }
    std::int64_t video_id = 0;
    if (!JsonIntField(request.body, "video_id", &video_id) || video_id <= 0) {
        return Fail(response, 400, "video_id 非法");
    }
    douyin::StoredVideo video;
    std::string error;
    if (!store.FindVideo(video_id, &video, &error)) {
        if (!error.empty()) {
            std::fprintf(stderr, "[%s] 查询视频失败: %s\n", kServiceName,
                         error.c_str());
            return Fail(response, 500, "数据库错误");
        }
        return Fail(response, 404, "视频不存在");
    }
    const int inserted = store.Like(video_id, actor, &error);
    if (inserted < 0) {
        std::fprintf(stderr, "[%s] 点赞失败: %s\n", kServiceName, error.c_str());
        return Fail(response, 500, "数据库错误");
    }
    if (!store.UpdateVideoCounts(video_id, &error)) {
        std::fprintf(stderr, "[%s] 计数同步失败: %s\n", kServiceName,
                     error.c_str());
        return Fail(response, 500, "数据库错误");
    }
    const std::int64_t like_count = store.LikeCount(video_id, &error);
    if (like_count < 0) {
        std::fprintf(stderr, "[%s] 查询点赞数失败: %s\n", kServiceName,
                     error.c_str());
        return Fail(response, 500, "数据库错误");
    }
    response->status = 200;
    response->body = std::string("{\"ok\":true,\"liked\":true,\"inserted\":") +
                     (inserted > 0 ? "true" : "false") + ",\"video_id\":" +
                     std::to_string(video_id) + ",\"like_count\":" +
                     std::to_string(like_count) + "}";
    return true;
}

/// @brief 取消赞（幂等）。
bool HandleUnlike(douyin::Store& store, const douyin::HttpRequest& request,
                  douyin::HttpResponse* response) {
    std::int64_t actor = 0;
    if (!RequireToken(request, response, &actor)) {
        return true;
    }
    std::int64_t video_id = 0;
    if (!JsonIntField(request.body, "video_id", &video_id) || video_id <= 0) {
        return Fail(response, 400, "video_id 非法");
    }
    douyin::StoredVideo video;
    std::string error;
    if (!store.FindVideo(video_id, &video, &error)) {
        if (!error.empty()) {
            std::fprintf(stderr, "[%s] 查询视频失败: %s\n", kServiceName,
                         error.c_str());
            return Fail(response, 500, "数据库错误");
        }
        return Fail(response, 404, "视频不存在");
    }
    if (!store.Unlike(video_id, actor, &error)) {
        std::fprintf(stderr, "[%s] 取消赞失败: %s\n", kServiceName,
                     error.c_str());
        return Fail(response, 500, "数据库错误");
    }
    if (!store.UpdateVideoCounts(video_id, &error)) {
        std::fprintf(stderr, "[%s] 计数同步失败: %s\n", kServiceName,
                     error.c_str());
        return Fail(response, 500, "数据库错误");
    }
    const std::int64_t like_count = store.LikeCount(video_id, &error);
    if (like_count < 0) {
        std::fprintf(stderr, "[%s] 查询点赞数失败: %s\n", kServiceName,
                     error.c_str());
        return Fail(response, 500, "数据库错误");
    }
    response->status = 200;
    response->body = std::string("{\"ok\":true,\"liked\":false,\"video_id\":") +
                     std::to_string(video_id) + ",\"like_count\":" +
                     std::to_string(like_count) + "}";
    return true;
}

/// @brief 发评论。
bool HandleComment(douyin::Store& store, const douyin::HttpRequest& request,
                   douyin::HttpResponse* response) {
    std::int64_t actor = 0;
    if (!RequireToken(request, response, &actor)) {
        return true;
    }
    std::int64_t video_id = 0;
    std::string content;
    if (!JsonIntField(request.body, "video_id", &video_id) ||
        !douyin::JsonStringField(request.body, "content", &content)) {
        return Fail(response, 400, "video_id/content 不能为空");
    }
    if (video_id <= 0) {
        return Fail(response, 400, "video_id 非法");
    }
    if (content.empty() || IsBlank(content)) {
        return Fail(response, 400, "评论内容不能为空");
    }
    if (Utf8Length(content) > kMaxCommentChars) {
        return Fail(response, 400, "评论超过 500 字");
    }
    douyin::StoredVideo video;
    std::string error;
    if (!store.FindVideo(video_id, &video, &error)) {
        if (!error.empty()) {
            std::fprintf(stderr, "[%s] 查询视频失败: %s\n", kServiceName,
                         error.c_str());
            return Fail(response, 500, "数据库错误");
        }
        return Fail(response, 404, "视频不存在");
    }

    const std::int64_t id = store.AddComment(video_id, actor, content, &error);
    if (id <= 0) {
        std::fprintf(stderr, "[%s] 发评论失败: %s\n", kServiceName,
                     error.c_str());
        return Fail(response, 500, "数据库错误");
    }
    if (!store.UpdateVideoCounts(video_id, &error)) {
        std::fprintf(stderr, "[%s] 计数同步失败: %s\n", kServiceName,
                     error.c_str());
        return Fail(response, 500, "数据库错误");
    }

    douyin::StoredComment stored;
    stored.id = id;
    stored.video_id = video_id;
    stored.user_id = actor;
    stored.content = content;
    stored.created_at = NowSeconds();

    response->status = 200;
    response->body = "{\"ok\":true,\"comment\":" +
                     douyin::ToJson(PublicComment(stored)) + "}";
    return true;
}

/// @brief 评论列表（倒序 + cursor 分页）。
bool HandleComments(douyin::Store& store, const douyin::HttpRequest& request,
                    std::int64_t video_id, douyin::HttpResponse* response) {
    douyin::StoredVideo video;
    std::string error;
    if (!store.FindVideo(video_id, &video, &error)) {
        if (!error.empty()) {
            std::fprintf(stderr, "[%s] 查询视频失败: %s\n", kServiceName,
                         error.c_str());
            return Fail(response, 500, "数据库错误");
        }
        return Fail(response, 404, "视频不存在");
    }
    std::string cursor;
    if (!ReadCursor(request, &cursor)) {
        return Fail(response, 400, "cursor 非法");
    }
    const std::int64_t limit = ParseLimit(request);

    std::vector<douyin::StoredComment> comments;
    std::string next_cursor;
    if (!store.ListComments(video_id, cursor, limit, &comments, &next_cursor,
                            &error)) {
        if (error == "cursor 非法") {
            return Fail(response, 400, "cursor 非法");
        }
        std::fprintf(stderr, "[%s] 查询评论失败: %s\n", kServiceName,
                     error.c_str());
        return Fail(response, 500, "数据库错误");
    }

    std::string body = "{\"ok\":true,\"items\":[";
    for (std::size_t i = 0; i < comments.size(); ++i) {
        if (i != 0) {
            body += ',';
        }
        body += "{\"comment\":" +
                douyin::ToJson(PublicComment(comments[i])) + "}";
    }
    body += "],\"next_cursor\":\"" + douyin::JsonEscape(next_cursor) + "\"}";
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
    if (request.path == "/api/like") {
        if (request.method == "POST") {
            return HandleLike(store, request, response);
        }
        if (request.method == "DELETE") {
            return HandleUnlike(store, request, response);
        }
        return Fail(response, 405, "方法不允许");
    }
    if (request.path == "/api/comment") {
        if (request.method == "POST") {
            return HandleComment(store, request, response);
        }
        return Fail(response, 405, "方法不允许");
    }

    std::int64_t video_id = 0;
    if (!ParseVideoPath(request.path, &video_id)) {
        return false;
    }
    if (request.method == "GET") {
        return HandleComments(store, request, video_id, response);
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
