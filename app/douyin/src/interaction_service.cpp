/**
 * @file interaction_service.cpp
 * @brief 抖音 P0 interaction 服务（29104）：点赞 / 取消赞 / 评论 / 评论列表。
 *
 * @details 路由：
 *          - POST   /api/like              点赞（Bearer；幂等）
 *          - DELETE /api/like              取消赞（Bearer；幂等）
 *          - POST   /api/comment           发评论（Bearer；1~500 字）
 *          - GET    /api/comment/:video_id 评论列表（倒序 + cursor 分页）
 *          - GET    /api/notifications          通知列表（Bearer；倒序 + 游标）
 *          - POST   /api/notifications/read     标记已读（Bearer；ids 或 all）
 *          - GET    /api/notifications/unread_count 未读数（Bearer）
 *          - GET    /health, /metrics
 *
 *          通知写入点（第 2 轮）：视频被赞 / 评论被赞 / 视频被评论（含回复
 *          父评论作者）；全部「自己触发自己」不写，重复点赞不重复写。
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

/**
 * @brief 把 UTF-8 文本截断到最多 `max_chars` 个码点（超出补省略号）。
 * @param text [in] 任意 UTF-8 文本；按字节扫描，不切开多字节字符。
 * @param max_chars [in] 允许的最大码点数；0 表示全部截掉。
 * @return 截断后的新串；未超长时原样返回。
 * @note 只用于通知摘要（界面单行展示），正文仍以完整内容入库。
 */
std::string ClampText(const std::string& text, std::size_t max_chars) {
    std::size_t chars = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if ((static_cast<unsigned char>(text[i]) & 0xC0U) != 0x80U) {
            if (chars == max_chars) {
                return text.substr(0, i) + "…";
            }
            ++chars;
        }
    }
    return text;
}

/**
 * @brief 写一条通知（附属动作）。
 * @param store [in,out] 已打开的存储；借用。
 * @param user_id [in] 接收者 id（>0），由调用方保证「不是触发者本人」。
 * @param actor_id [in] 触发者 id（>0）。
 * @param kind [in] `like` / `comment` / `follow` / `mention`。
 * @param target_type [in] `video` / `comment` / `user`。
 * @param target_id [in] 目标 id（>0）。
 * @param content [in] 已截断的摘要（可为空）。
 * @return 无。
 *
 * ## 定位
 * 服务层唯一的通知写入出口；失败只记日志，**不回滚**已完成的点赞/评论。
 * 错了会怎样：通知丢一条（可接受），但主操作绝不能被通知拖成 500。
 */
void AppendNotification(douyin::Store& store, std::int64_t user_id,
                        std::int64_t actor_id, const char* kind,
                        const char* target_type, std::int64_t target_id,
                        const std::string& content) {
    std::string error;
    if (!store.AddNotification(user_id, actor_id, kind, target_type, target_id,
                               content, &error)) {
        std::fprintf(stderr, "[%s] 写通知失败: %s\n", kServiceName,
                     error.c_str());
    }
}

/**
 * @brief 解析 JSON 整数数组字段（如 `{"ids":[1,2,3]}`）。
 * @param body [in] 原始请求体；不是合法 JSON 时按解析失败处理。
 * @param key [in] 字段名（不含引号）。
 * @param out [out] 追加解析出的非负整数；失败时内容未定义。
 * @return 字段存在、数组合法且元素均为十进制整数时 true；否则 false。
 */
bool JsonIntArrayField(std::string_view body, std::string_view key,
                       std::vector<std::int64_t>* out) {
    const std::string needle = "\"" + std::string(key) + "\"";
    std::size_t pos = body.find(needle);
    if (pos == std::string_view::npos) {
        return false;
    }
    pos = body.find('[', pos + needle.size());
    if (pos == std::string_view::npos) {
        return false;
    }
    ++pos;
    while (pos < body.size()) {
        while (pos < body.size() &&
               (body[pos] == ' ' || body[pos] == '\t' || body[pos] == ',' ||
                body[pos] == '\r' || body[pos] == '\n')) {
            ++pos;
        }
        if (pos >= body.size()) {
            return false;
        }
        if (body[pos] == ']') {
            return true;
        }
        if (body[pos] < '0' || body[pos] > '9') {
            return false;
        }
        std::int64_t value = 0;
        while (pos < body.size() && body[pos] >= '0' && body[pos] <= '9') {
            value = value * 10 + (body[pos] - '0');
            if (value > 9007199254740991LL) {
                return false;
            }
            ++pos;
        }
        out->push_back(value);
    }
    return false;
}

/**
 * @brief 字段是否为 JSON `true`（如 `{"all":true}`）。
 * @param body [in] 原始请求体。
 * @param key [in] 字段名（不含引号）。
 * @return 字段存在且值为 true 时返回 true；其余情况（含字段缺失）返回 false。
 */
bool JsonBoolTrue(std::string_view body, std::string_view key) {
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
    return body.compare(pos, 4, "true") == 0;
}

/// @brief 点赞（幂等）。

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
    // 防骚扰：被视频作者拉黑后不能再点赞；自己拉黑作者时也要先取消。
    {
        bool blocked = false;
        if (!store.IsBlocked(video.user_id, actor, &blocked, &error)) {
            std::fprintf(stderr, "[%s] 查询拉黑状态失败: %s\n", kServiceName,
                         error.c_str());
            return Fail(response, 500, "数据库错误");
        }
        if (blocked) {
            return Fail(response, 403, "对方已把你拉黑");
        }
        if (!store.IsBlocked(actor, video.user_id, &blocked, &error)) {
            std::fprintf(stderr, "[%s] 查询拉黑状态失败: %s\n", kServiceName,
                         error.c_str());
            return Fail(response, 500, "数据库错误");
        }
        if (blocked) {
            return Fail(response, 403, "你已拉黑对方，请先取消拉黑");
        }
    }
    const int inserted = store.Like(video_id, actor, &error);
    if (inserted < 0) {
        std::fprintf(stderr, "[%s] 点赞失败: %s\n", kServiceName, error.c_str());
        return Fail(response, 500, "数据库错误");
    }
    // 视频点赞通知：只有「本次新插入」才写（重复点赞幂等），自己赞自己不写。
    if (inserted > 0 && video.user_id != actor) {
        AppendNotification(store, video.user_id, actor, "like", "video",
                           video_id, ClampText(video.title, 60));
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

    // step2.5: 防骚扰——被视频作者拉黑后不能再在其视频下评论（403），
    //          只做展示过滤的话，拉黑等于没拉。
    {
        bool blocked = false;
        if (!store.IsBlocked(video.user_id, actor, &blocked, &error)) {
            std::fprintf(stderr, "[%s] 查询拉黑状态失败: %s\n", kServiceName,
                         error.c_str());
            return Fail(response, 500, "数据库错误");
        }
        if (blocked) {
            return Fail(response, 403, "对方已把你拉黑");
        }
        if (!store.IsBlocked(actor, video.user_id, &blocked, &error)) {
            std::fprintf(stderr, "[%s] 查询拉黑状态失败: %s\n", kServiceName,
                         error.c_str());
            return Fail(response, 500, "数据库错误");
        }
        if (blocked) {
            return Fail(response, 403, "你已拉黑对方，请先取消拉黑");
        }
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
    // 评论通知：视频作者一条；若是回复，父评论作者再一条（去重、跳过自己）。
    const std::string summary = ClampText(content, 60);
    if (video.user_id != actor) {
        AppendNotification(store, video.user_id, actor, "comment", "video",
                           video_id, summary);
    }
    if (parent_id != 0) {
        douyin::StoredComment parent_author;
        std::string parent_error;
        if (store.FindComment(parent_id, &parent_author, &parent_error) &&
            parent_author.user_id != actor &&
            parent_author.user_id != video.user_id) {
            AppendNotification(store, parent_author.user_id, actor, "comment",
                               "comment", parent_id, summary);
        }
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
    // 拉黑过滤（第 3 轮）：拉黑区里这个作者的视频连同评论区一起不可见，
    // 与 feed/详情的 404 语义保持一致。
    const std::int64_t viewer = ViewerId(request);
    if (viewer > 0) {
        bool blocked = false;
        // 双向：viewer 拉黑作者，或作者拉黑 viewer，评论区都不可见。
        if (!store.IsBlocked(viewer, video.user_id, &blocked, &error)) {
            std::fprintf(stderr, "[%s] 查询拉黑状态失败: %s\n", kServiceName,
                         error.c_str());
            return Fail(response, 500, "数据库错误");
        }
        if (!blocked &&
            !store.IsBlocked(video.user_id, viewer, &blocked, &error)) {
            std::fprintf(stderr, "[%s] 查询拉黑状态失败: %s\n", kServiceName,
                         error.c_str());
            return Fail(response, 500, "数据库错误");
        }
        if (blocked) {
            return Fail(response, 404, "视频不存在");
        }
    }
    std::string cursor;
    if (!ReadCursor(request, &cursor)) {
        return Fail(response, 400, "cursor 非法");
    }
    const std::int64_t limit = ParseLimit(request);

    std::vector<douyin::StoredComment> comments;
    std::string next_cursor;
    if (!store.ListComments(video_id, cursor, limit, &comments, &next_cursor,
                            &error, viewer)) {
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
        if (!store.ListReplies(video_id, comments[i].id, 50, &replies, &error,
                               viewer)) {
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
    // step3: 幂等写入/删除；只有「本次新插入」才写通知（重复点赞不重复写）。
    if (like) {
        const int inserted = store.LikeComment(comment_id, actor, &error);
        if (inserted < 0) {
            std::fprintf(stderr, "[%s] 评论点赞失败: %s\n", kServiceName,
                         error.c_str());
            return Fail(response, 500, "数据库错误");
        }
        // step3.1: 自己赞自己不产生通知；通知失败不影响点赞结果。
        if (inserted > 0 && comment.user_id != actor) {
            AppendNotification(store, comment.user_id, actor, "like", "comment",
                               comment_id, ClampText(comment.content, 60));
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

/**
 * @brief 单条举报序列化为 JSON。
 * @param item [in] 存储层一行；文本字段全部转义。
 * @return 单行 JSON 对象。
 */
std::string ReportJson(const douyin::StoredReport& item) {
    return std::string("{\"id\":") + std::to_string(item.id) +
           ",\"reporter_id\":" + std::to_string(item.reporter_id) +
           ",\"target_type\":\"" + douyin::JsonEscape(item.target_type) +
           "\",\"target_id\":" + std::to_string(item.target_id) +
           ",\"reason\":\"" + douyin::JsonEscape(item.reason) +
           "\",\"detail\":\"" + douyin::JsonEscape(item.detail) +
           "\",\"status\":\"" + douyin::JsonEscape(item.status) +
           "\",\"created_at\":" + std::to_string(item.created_at) + "}";
}

/**
 * @brief 举报：`POST /api/report`，body `{target_type,target_id,reason,detail}`。
 *
 * @param store [in,out] 已打开的存储；借用。
 * @param request [in] 原始请求；必须带有效 Bearer token。
 * @param response [out] 成功写 `{ok,duplicate,report}`；失败写统一错误体。
 * @return 恒为 true（已处理）。
 *
 * ## 定位
 * 举报的唯一入口。错了会怎样：重复举报刷爆运营表，或举报指向不存在的
 * 目标/自己的内容。
 *
 * ## 步骤
 * step1. 鉴权——举报必须实名登录，匿名 401。
 * step2. 校验 target_type / target_id / reason / detail 长度。
 * step3. 确认目标存在（不存在 404），并拒绝「举报自己」（400）。
 * step4. 写库：首次 201；唯一键冲突返回已有记录 200 + duplicate=true。
 */
bool HandleReport(douyin::Store& store, const douyin::HttpRequest& request,
                  douyin::HttpResponse* response) {
    // step1: 鉴权。
    std::int64_t actor = 0;
    if (!RequireToken(request, response, &actor)) {
        return true;
    }
    // step2: 字段校验（长度按 UTF-8 码点，不按字节）。
    std::string target_type;
    if (!douyin::JsonStringField(request.body, "target_type", &target_type) ||
        (target_type != "video" && target_type != "comment" &&
         target_type != "user")) {
        return Fail(response, 400, "target_type 非法（video/comment/user）");
    }
    std::int64_t target_id = 0;
    if (!JsonIntField(request.body, "target_id", &target_id) || target_id <= 0) {
        return Fail(response, 400, "target_id 非法");
    }
    std::string reason;
    if (!douyin::JsonStringField(request.body, "reason", &reason) ||
        IsBlank(reason)) {
        return Fail(response, 400, "reason 不能为空");
    }
    if (Utf8Length(reason) > 50) {
        return Fail(response, 400, "reason 超过 50 字");
    }
    std::string detail;
    if (douyin::JsonStringField(request.body, "detail", &detail) &&
        Utf8Length(detail) > 500) {
        return Fail(response, 400, "detail 超过 500 字");
    }
    // step3: 目标存在性 + 不能举报自己（自己的视频/评论/账号都不行）。
    std::string error;
    if (target_type == "video") {
        douyin::StoredVideo video;
        if (!store.FindVideo(target_id, &video, &error)) {
            if (!error.empty()) {
                std::fprintf(stderr, "[%s] 查询举报目标失败: %s\n", kServiceName,
                             error.c_str());
                return Fail(response, 500, "数据库错误");
            }
            return Fail(response, 404, "视频不存在");
        }
        if (video.user_id == actor) {
            return Fail(response, 400, "不能举报自己");
        }
    } else if (target_type == "comment") {
        douyin::StoredComment comment;
        if (!store.FindComment(target_id, &comment, &error)) {
            if (!error.empty()) {
                std::fprintf(stderr, "[%s] 查询举报目标失败: %s\n", kServiceName,
                             error.c_str());
                return Fail(response, 500, "数据库错误");
            }
            return Fail(response, 404, "评论不存在");
        }
        if (comment.user_id == actor) {
            return Fail(response, 400, "不能举报自己");
        }
    } else {
        douyin::StoredUser target;
        if (!store.FindUserById(target_id, &target, &error)) {
            if (!error.empty()) {
                std::fprintf(stderr, "[%s] 查询举报目标失败: %s\n", kServiceName,
                             error.c_str());
                return Fail(response, 500, "数据库错误");
            }
            return Fail(response, 404, "用户不存在");
        }
        if (target_id == actor) {
            return Fail(response, 400, "不能举报自己");
        }
    }
    // step4: 写库。写前清空 ORM 语句缓存，避免读游标残留导致 BUSY_SNAPSHOT。
    store.ResetStatements();
    douyin::StoredReport report;
    const douyin::Store::ReportResult result = store.AddReport(
        actor, target_type, target_id, reason, detail, &report, &error);
    if (result == douyin::Store::ReportResult::kError) {
        std::fprintf(stderr, "[%s] 写举报失败: %s\n", kServiceName, error.c_str());
        return Fail(response, 500, "数据库错误");
    }
    const bool duplicate = result == douyin::Store::ReportResult::kDuplicate;
    response->status = duplicate ? 200 : 201;
    response->body = std::string("{\"ok\":true,\"duplicate\":") +
                     (duplicate ? "true" : "false") + ",\"report\":" +
                     ReportJson(report) + "}";
    return true;
}

/**
 * @brief 单条通知序列化为 JSON。
 * @param item [in] 存储层一行（actor_name 已由 JOIN 回填）。
 * @return 单行 JSON 对象；文本字段全部转义。
 */
std::string NotificationJson(const douyin::StoredNotification& item) {
    return std::string("{\"id\":") + std::to_string(item.id) +
           ",\"user_id\":" + std::to_string(item.user_id) +
           ",\"actor_id\":" + std::to_string(item.actor_id) +
           ",\"actor_name\":\"" + douyin::JsonEscape(item.actor_name) + "\"" +
           ",\"kind\":\"" + douyin::JsonEscape(item.kind) + "\"" +
           ",\"target_type\":\"" + douyin::JsonEscape(item.target_type) + "\"" +
           ",\"target_id\":" + std::to_string(item.target_id) +
           ",\"content\":\"" + douyin::JsonEscape(item.content) + "\"" +
           ",\"created_at\":" + std::to_string(item.created_at) +
           ",\"read_at\":" + std::to_string(item.read_at) + "}";
}

/**
 * @brief 通知列表：`GET /api/notifications?cursor=&limit=20`。
 *
 * @param store [in,out] 已打开的存储；借用。
 * @param request [in] 原始请求；必须带有效 Bearer token。
 * @param response [out] 成功写 `{ok,items,unread_count,next_cursor}`；
 *        失败写统一错误体。不可为 nullptr。
 * @return 恒为 true（已处理）。
 *
 * ## 定位
 * 消息中心唯一列表入口。错了会怎样：别人能看到不属于自己的通知
 * （鉴权/过滤错），或分页重复/漏条。
 *
 * ## 步骤
 * step1. 鉴权——通知是私密数据，匿名必须 401，不能像评论列表那样降级。
 * step2. 校验游标格式；非法回 400（不是 500）。
 * step3. 列表查询（1 条 SQL，JOIN 回填昵称）。
 * step4. 未读数（1 条 COUNT，走索引）；两次查询固定条数，不随页大小增长。
 */
bool HandleNotifications(douyin::Store& store, const douyin::HttpRequest& request,
                         douyin::HttpResponse* response) {
    // step1: 鉴权。
    std::int64_t viewer = 0;
    if (!RequireToken(request, response, &viewer)) {
        return true;
    }
    // step2: 游标。
    const std::string cursor_text = douyin::QueryParam(request, "cursor");
    std::int64_t cursor_created = 0;
    std::int64_t cursor_id = 0;
    if (!cursor_text.empty() &&
        !douyin::DecodeNotificationCursor(cursor_text, &cursor_created,
                                          &cursor_id)) {
        return Fail(response, 400, "cursor 非法");
    }
    // step3: 列表。
    const std::int64_t limit = ParseLimit(request);
    std::vector<douyin::StoredNotification> items;
    std::string next_cursor;
    std::string error;
    if (!store.ListNotifications(viewer, cursor_text, limit, &items, &next_cursor,
                                 &error)) {
        std::fprintf(stderr, "[%s] 查询通知失败: %s\n", kServiceName,
                     error.c_str());
        return Fail(response, 500, "数据库错误");
    }
    // step4: 未读数——固定再多一条 COUNT，不做 N+1。
    const std::int64_t unread = store.CountUnreadNotifications(viewer, &error);
    if (unread < 0) {
        std::fprintf(stderr, "[%s] 统计未读失败: %s\n", kServiceName,
                     error.c_str());
        return Fail(response, 500, "数据库错误");
    }
    std::string body = "{\"ok\":true,\"items\":[";
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i != 0) {
            body += ',';
        }
        body += NotificationJson(items[i]);
    }
    body += "],\"unread_count\":" + std::to_string(unread) +
            ",\"next_cursor\":\"" + douyin::JsonEscape(next_cursor) + "\"}";
    response->status = 200;
    response->body = std::move(body);
    return true;
}

/**
 * @brief 未读数：`GET /api/notifications/unread_count`。
 * @param store [in,out] 已打开的存储；借用。
 * @param request [in] 原始请求；必须带有效 Bearer token。
 * @param response [out] 成功写 `{ok,unread_count}`。不可为 nullptr。
 * @return 恒为 true。
 *
 * ## 定位
 * 顶部徽章的唯一数据源；每次轮询只跑一条索引 COUNT。
 */
bool HandleUnreadCount(douyin::Store& store, const douyin::HttpRequest& request,
                       douyin::HttpResponse* response) {
    std::int64_t viewer = 0;
    if (!RequireToken(request, response, &viewer)) {
        return true;
    }
    std::string error;
    const std::int64_t unread = store.CountUnreadNotifications(viewer, &error);
    if (unread < 0) {
        std::fprintf(stderr, "[%s] 统计未读失败: %s\n", kServiceName,
                     error.c_str());
        return Fail(response, 500, "数据库错误");
    }
    response->status = 200;
    response->body = std::string("{\"ok\":true,\"unread_count\":") +
                     std::to_string(unread) + "}";
    return true;
}

/**
 * @brief 标记已读：`POST /api/notifications/read`，body `{"ids":[...]}`
 *        或 `{"all":true}`。
 *
 * @param store [in,out] 已打开的存储；借用。
 * @param request [in] 原始请求；必须带有效 Bearer token。
 * @param response [out] 成功写 `{ok,updated,unread_count}`。不可为 nullptr。
 * @return 恒为 true。
 *
 * ## 定位
 * 未读归零的唯一入口。错了会怎样：把别人的通知标成已读（越权写），
 * 或徽章数字与列表不一致。
 *
 * ## 步骤
 * step1. 鉴权。
 * step2. 解析 body：all=true 优先；否则必须给合法 ids 数组（空数组合法）。
 * step3. UPDATE 带 user_id 条件，天然只能改自己的。
 * step4. 回填最新未读数，前端直接用，不再发第二次请求。
 */
bool HandleNotificationsRead(douyin::Store& store,
                             const douyin::HttpRequest& request,
                             douyin::HttpResponse* response) {
    // step1: 鉴权。
    std::int64_t viewer = 0;
    if (!RequireToken(request, response, &viewer)) {
        return true;
    }
    // step2: body。
    const bool all = JsonBoolTrue(request.body, "all");
    std::vector<std::int64_t> ids;
    if (!all && !JsonIntArrayField(request.body, "ids", &ids)) {
        return Fail(response, 400, "ids 或 all 必须提供");
    }
    // step3: 标记。
    std::string error;
    const std::int64_t updated =
        store.MarkNotificationsRead(viewer, ids, all, &error);
    if (updated < 0) {
        std::fprintf(stderr, "[%s] 标记通知已读失败: %s\n", kServiceName,
                     error.c_str());
        return Fail(response, 500, "数据库错误");
    }
    // step4: 未读数。
    const std::int64_t unread = store.CountUnreadNotifications(viewer, &error);
    if (unread < 0) {
        std::fprintf(stderr, "[%s] 统计未读失败: %s\n", kServiceName,
                     error.c_str());
        return Fail(response, 500, "数据库错误");
    }
    response->status = 200;
    response->body = std::string("{\"ok\":true,\"updated\":") +
                     std::to_string(updated) + ",\"unread_count\":" +
                     std::to_string(unread) + "}";
    return true;
}

/// @brief 总路由。
bool Handle(douyin::Store& store, const douyin::HttpRequest& request,
            douyin::HttpResponse* response) {
    ++g_metrics.requests;
    // 请求边界上不应存在未提交事务：清一次 ORM 语句缓存，避免上一请求
    // 残留的读游标把本连接钉在旧 WAL 快照上（实测：拉黑后通知/评论仍见旧数据）。
    store.ResetStatements();
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
    if (request.path == "/api/report") {
        if (request.method == "POST") {
            return HandleReport(store, request, response);
        }
        return Fail(response, 405, "方法不允许");
    }
    if (request.path == "/api/comment") {
        if (request.method == "POST") {
            return HandleComment(store, request, response);
        }
        return Fail(response, 405, "方法不允许");
    }
    // 通知接口：精确路径匹配，必须在评论路径解析之前判断，避免被当成视频 id。
    if (request.path == "/api/notifications") {
        if (request.method == "GET") {
            return HandleNotifications(store, request, response);
        }
        return Fail(response, 405, "方法不允许");
    }
    if (request.path == "/api/notifications/unread_count") {
        if (request.method == "GET") {
            return HandleUnreadCount(store, request, response);
        }
        return Fail(response, 405, "方法不允许");
    }
    if (request.path == "/api/notifications/read") {
        if (request.method == "POST") {
            return HandleNotificationsRead(store, request, response);
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
