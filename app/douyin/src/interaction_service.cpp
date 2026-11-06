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
#include "go2cpp/storage/db.hpp"
#include "idempotency.hpp"

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
/// @brief 幂等记录默认 TTL：24 小时（与 PRD 的「重发窗口」一致）。
constexpr std::int64_t kDefaultIdempotencyTtlSeconds = 24 * 60 * 60;

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

/**
 * @brief 读幂等 TTL 环境变量（秒）。
 * @return 正整数秒；变量缺失、非数字、<=0 时回落到 24h，避免 0 TTL 把
 *         幂等直接关掉（0 会让所有记录一写入就过期）。
 * @note 环境变量 `DOUYIN_IDEMPOTENCY_TTL_SECONDS` 只用于测试缩短 TTL，
 *       生产保持默认 24h。
 */
std::int64_t IdempotencyTtlSeconds() {
    const char* env = std::getenv("DOUYIN_IDEMPOTENCY_TTL_SECONDS");
    if (env == nullptr || *env == '\0') {
        return kDefaultIdempotencyTtlSeconds;
    }
    char* end = nullptr;
    const long long value = std::strtoll(env, &end, 10);
    if (end == env || value <= 0) {
        return kDefaultIdempotencyTtlSeconds;
    }
    return static_cast<std::int64_t>(value);
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

/**
 * @brief 尝试解析可选的 Bearer token，用于「当前查看者」相关字段。
 * @param request [in] 原始请求；Authorization 头可为空。
 * @return 有效 token 的 user_id；无 token / 签名非法 / 过期一律返回 0（匿名）。
 * @note 评论列表是公开接口，token 无效**不应**回 401，否则匿名与过期用户
 *       连评论都看不到；这里只影响 `liked` 字段。
 */
std::int64_t ViewerId(const douyin::HttpRequest& request) {
    const std::string token = douyin::BearerToken(request);
    if (token.empty()) {
        return 0;
    }
    douyin::TokenInfo info;
    std::string error;
    if (!douyin::VerifyToken(token, NowSeconds(), &info, &error)) {
        return 0;
    }
    return info.user_id;
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

/**
 * @brief 从 `/api/comment/<id>/like` 解析评论 id。
 * @param path [in] 请求路径。
 * @param id [out] 成功时写评论 id（>0）；失败时保持原值。
 * @return 路径形态匹配且 id 为纯数字时返回 true；否则 false。
 * @note 与 `ParseVideoPath` 分开：后者会把 `<id>/like` 判为非法，
 *       所以路由必须先测本函数再测 `ParseVideoPath`。
 */
bool ParseCommentLikePath(const std::string& path, std::int64_t* id) {
    constexpr std::string_view kPrefix = "/api/comment/";
    constexpr std::string_view kSuffix = "/like";
    if (path.size() <= kPrefix.size() + kSuffix.size()) {
        return false;
    }
    if (path.compare(0, kPrefix.size(), kPrefix.data(), kPrefix.size()) != 0) {
        return false;
    }
    if (path.compare(path.size() - kSuffix.size(), kSuffix.size(),
                     kSuffix.data(), kSuffix.size()) != 0) {
        return false;
    }
    const std::string rest =
        path.substr(kPrefix.size(), path.size() - kPrefix.size() - kSuffix.size());
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
    comment.parent_id = stored.parent_id;
    comment.like_count = stored.like_count;
    comment.liked = stored.liked;
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

    // step3: 可选 parent_id——必须是同一视频下的顶层评论；对回复再回复时
    //        提升到它所属的顶层评论，保证只有两级、不产生悬挂链。
    std::int64_t parent_id = 0;
    if (JsonIntField(request.body, "parent_id", &parent_id) && parent_id != 0) {
        if (parent_id < 0) {
            return Fail(response, 400, "parent_id 非法");
        }
        douyin::StoredComment parent;
        if (!store.FindComment(parent_id, &parent, &error)) {
            if (!error.empty()) {
                std::fprintf(stderr, "[%s] 查询父评论失败: %s\n", kServiceName,
                             error.c_str());
                return Fail(response, 500, "数据库错误");
            }
            return Fail(response, 404, "父评论不存在");
        }
        if (parent.video_id != video_id) {
            return Fail(response, 400, "父评论不属于该视频");
        }
        if (parent.parent_id != 0) {
            parent_id = parent.parent_id;
        }
    }
    // 写前清空 ORM 语句缓存：FindComment 提前结束会残留读游标，
    // 多服务共享 WAL 库时会让本进程的写触发 SQLITE_BUSY_SNAPSHOT。
    store.ResetStatements();
    const std::int64_t id =
        store.AddComment(video_id, actor, content, &error, parent_id);
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
    stored.parent_id = parent_id;

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

    // 评论点赞：先把各顶层评论的回复收进扁平数组并记录区间，再对
    // comments / all_replies 各做一次批量回填（固定 4 条 SQL，不随页大小增长）。
    std::vector<std::pair<std::size_t, std::size_t>> spans;
    std::vector<douyin::StoredComment> all_replies;
    spans.reserve(comments.size());
    for (std::size_t i = 0; i < comments.size(); ++i) {
        std::vector<douyin::StoredComment> replies;
        if (!store.ListReplies(video_id, comments[i].id, 50, &replies, &error)) {
            std::fprintf(stderr, "[%s] 查询回复失败: %s\n", kServiceName,
                         error.c_str());
            replies.clear();
        }
        const std::size_t begin = all_replies.size();
        for (auto& reply : replies) {
            all_replies.push_back(std::move(reply));
        }
        spans.emplace_back(begin, all_replies.size());
    }
    // 回填失败只记日志、不阻断：点赞数缺失不该让整页评论 500。
    const std::int64_t viewer = ViewerId(request);
    if (!store.FillCommentLikes(viewer, &comments, &error)) {
        std::fprintf(stderr, "[%s] 回填评论点赞失败: %s\n", kServiceName,
                     error.c_str());
    } else if (!store.FillCommentLikes(viewer, &all_replies, &error)) {
        std::fprintf(stderr, "[%s] 回填回复点赞失败: %s\n", kServiceName,
                     error.c_str());
    }

    std::string body = "{\"ok\":true,\"items\":[";
    for (std::size_t i = 0; i < comments.size(); ++i) {
        if (i != 0) {
            body += ',';
        }
        body += "{\"comment\":" +
                douyin::ToJson(PublicComment(comments[i]));
        body += ",\"replies\":[";
        const std::size_t end = i < spans.size() ? spans[i].second : 0;
        std::size_t j = i < spans.size() ? spans[i].first : 0;
        bool first = true;
        for (; j < end; ++j) {
            if (!first) {
                body += ',';
            }
            first = false;
            body += douyin::ToJson(PublicComment(all_replies[j]));
        }
        body += "]}";
    }
    body += "],\"next_cursor\":\"" + douyin::JsonEscape(next_cursor) + "\"}";
    response->status = 200;
    response->body = std::move(body);
    return true;
}

/**
 * @brief 评论点赞 / 取消点赞（幂等）。
 *
 * @param store [in,out] 已打开的存储；借用，生命周期由 main 持有。
 * @param request [in] 原始请求；必须带有效 Bearer token。
 * @param comment_id [in] 目标评论 id（>0），由 `ParseCommentLikePath` 解析。
 * @param like [in] true=点赞，false=取消点赞。
 * @param response [out] 成功写 `{"ok":true,"liked":bool,"like_count":N}`；
 *        失败写统一错误体。不可为 nullptr。
 * @return 恒为 true（表示已处理）；路由层据此区分「未命中」。
 *
 * ## 定位
 * 评论点赞的唯一入口。错了会怎样：计数与按钮状态不一致，或重复点赞把
 * 计数刷高。
 *
 * ## 步骤
 * step1. 校验 token；失败直接 401（评论点赞必须登录）。
 * step2. 确认评论存在；不存在 404，数据库错误 500。
 * step3. 写 comment_likes（INSERT OR IGNORE / DELETE，均幂等）。
 * step4. 回填最新计数并输出；回填失败按 500，否则前端会显示错误数字。
 */
bool HandleCommentLike(douyin::Store& store, const douyin::HttpRequest& request,
                       std::int64_t comment_id, bool like,
                       douyin::HttpResponse* response) {
    // step1: 鉴权。
    std::int64_t actor = 0;
    if (!RequireToken(request, response, &actor)) {
        return true;
    }
    // step2: 评论必须存在（否则点赞记录会挂到不存在的评论上）。
    douyin::StoredComment comment;
    std::string error;
    if (!store.FindComment(comment_id, &comment, &error)) {
        if (!error.empty()) {
            std::fprintf(stderr, "[%s] 查询评论失败: %s\n", kServiceName,
                         error.c_str());
            return Fail(response, 500, "数据库错误");
        }
        return Fail(response, 404, "评论不存在");
    }
    // step3: 幂等写入/删除。
    if (like) {
        if (store.LikeComment(comment_id, actor, &error) < 0) {
            std::fprintf(stderr, "[%s] 评论点赞失败: %s\n", kServiceName,
                         error.c_str());
            return Fail(response, 500, "数据库错误");
        }
    } else if (!store.UnlikeComment(comment_id, actor, &error)) {
        std::fprintf(stderr, "[%s] 取消评论点赞失败: %s\n", kServiceName,
                     error.c_str());
        return Fail(response, 500, "数据库错误");
    }
    // step4: 以库为准回填计数，避免前端自己加减算出偏差。
    std::vector<douyin::StoredComment> one;
    one.push_back(std::move(comment));
    if (!store.FillCommentLikes(actor, &one, &error)) {
        std::fprintf(stderr, "[%s] 回填评论点赞失败: %s\n", kServiceName,
                     error.c_str());
        return Fail(response, 500, "数据库错误");
    }
    response->status = 200;
    response->body = std::string("{\"ok\":true,\"liked\":") +
                     (like ? "true" : "false") + ",\"like_count\":" +
                     std::to_string(one[0].like_count) + "}";
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

    // 评论点赞必须放在 /api/comment/<id> 之前判断：后者的 id 解析只接受
    // 纯数字，`<id>/like` 会落到这里并 405/404。
    std::int64_t like_comment_id = 0;
    if (ParseCommentLikePath(request.path, &like_comment_id)) {
        if (request.method == "POST") {
            return HandleCommentLike(store, request, like_comment_id, true,
                                     response);
        }
        if (request.method == "DELETE") {
            return HandleCommentLike(store, request, like_comment_id, false,
                                     response);
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

    // 幂等中间件：和业务共用同一个 SQLite 连接（WAL 下少一份锁竞争），
    // 表前缀 idempotency_，挂在整个路由外层（POST /api/comment、/api/like
    // 自动受保护；GET/HEAD 在中间件内部直通）。
    douyin::IdempotencyOptions idempotency_options;
    idempotency_options.ttl_seconds = IdempotencyTtlSeconds();
    douyin::IdempotencyMiddleware idempotency(store.Database(), idempotency_options);
    if (!idempotency.EnsureSchema(&error)) {
        std::fprintf(stderr, "[%s] %s\n", kServiceName, error.c_str());
        return 1;
    }

    douyin::HttpServer server(kServiceName, port);
    if (!server.Start(&error)) {
        std::fprintf(stderr, "[%s] %s\n", kServiceName, error.c_str());
        return 1;
    }
    // 就绪探针：核心表可读才算 ready；只读，不改任何数据。
    // 表被删/连接失效时返回 false，`/health/ready` 回 503。
    server.SetReadinessProbe([&store](std::string* detail) {
        if (!store.Ok() || store.Database() == nullptr) {
            if (detail != nullptr) {
                *detail = "db not open";
            }
            return false;
        }
        auto rows = store.Database()->Query("SELECT 1 FROM likes LIMIT 1");
        if (!rows) {
            if (detail != nullptr) {
                *detail = "db prepare failed";
            }
            return false;
        }
        (void)rows.Value().Next();
        if (rows.Value().Err()) {
            if (detail != nullptr) {
                *detail = "db query failed";
            }
            return false;
        }
        return true;
    });
    std::printf("[%s] 监听 0.0.0.0:%u db=%s idempotency_ttl=%llds\n", kServiceName,
                static_cast<unsigned>(port), db_path.c_str(),
                static_cast<long long>(idempotency_options.ttl_seconds));
    std::fflush(stdout);

    const douyin::HttpHandler route = [&store](const douyin::HttpRequest& request,
                                               douyin::HttpResponse* response) {
        return Handle(store, request, response);
    };
    server.ServeForever([&idempotency, &route](const douyin::HttpRequest& request,
                                               douyin::HttpResponse* response) {
        return idempotency.Handle(request, response, route);
    });
    return 0;
}
