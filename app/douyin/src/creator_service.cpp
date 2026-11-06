/**
 * @file creator_service.cpp
 * @brief 抖音「创作者中心」服务（29108）：本人数据总览 / 互动趋势 / TOP 作品。
 *
 * @details 路由（全部 Bearer，且只允许查看本人数据）：
 *          - GET /api/creator/overview        总播放量（估算）/ 点赞 / 评论 / 粉丝 / 作品数
 *          - GET /api/creator/trend?days=7|30 按天聚合的新增点赞 / 评论 / 粉丝
 *          - GET /api/creator/top-videos?limit=5 按互动量排序的作品
 *          - GET /creator                     创作者中心页面（HTML，无鉴权）
 *          - GET /health, /health/ready       由 HttpServer 内建
 *
 * @note 本服务**只读**数据库：不建自己的表、不写任何业务数据；`EnsureSchema`
 *       只为与其它服务共用同一个建表入口（幂等，不会改已有数据）。
 *       `likes` / `comments` / `follows` 三张表的按天聚合在一条 SQL 内完成
 *       （UNION ALL + GROUP BY date），不按天循环查询，避免 N+1。
 *
 * @author douyin demo
 * @date 2026-11-06
 * @copyright MIT License
 */

#include "douyin_auth.hpp"
#include "douyin_creator_page.hpp"
#include "douyin_http.hpp"
#include "douyin_store.hpp"
#include "go2cpp/storage/db.hpp"

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

constexpr std::uint16_t kDefaultPort = 29108;
constexpr const char* kServiceName = "creator";
constexpr std::int64_t kDefaultTopLimit = 5;
constexpr std::int64_t kMaxTopLimit = 50;
constexpr std::int64_t kSecondsPerDay = 86400;
/// @brief 趋势窗口只允许 7 / 30 天：其它值一律 400，避免生成超长响应。
constexpr std::int64_t kShortWindowDays = 7;
constexpr std::int64_t kLongWindowDays = 30;

/// @brief 当前 Unix 秒。
std::int64_t NowSeconds() {
    return static_cast<std::int64_t>(std::time(nullptr));
}

/// @brief 统一错误响应：`{"ok":false,"error":"...","code":N}`。
bool Fail(douyin::HttpResponse* response, int status, std::string_view message) {
    response->status = status;
    response->body = douyin::ErrorBody(status, message);
    response->headers["Cache-Control"] = "no-store";
    return true;
}

/**
 * @brief 解析并校验 Bearer token。
 *
 * @param request [in] 入站请求——借用，调用期间有效；读 Authorization 头。
 * @param response [out] 失败时写入 401 JSON；成功保持原值。
 * @param user_id [out] 成功时写入 token 里的用户 id；失败保持原值。
 * @return 成功返回 true；失败返回 false 且 response 已写好 401。
 *
 * ## 定位
 * 本服务直连（绕过网关）时的唯一鉴权入口：网关 `require_auth` 只是第一道
 * 闸门，直接访问 29108 的请求仍必须在服务内被拒绝。
 */
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
 * @brief 严格解析非负十进制整数（不接受符号、空白、尾随垃圾与溢出）。
 * @param text [in] 待解析文本——借用，调用期间有效。
 * @param out [out] 成功时写入；失败保持原值。
 * @return 全串都是数字且不溢出返回 true。
 */
bool ParseInt64(std::string_view text, std::int64_t* out) {
    if (text.empty() || out == nullptr) {
        return false;
    }
    const char* const begin = text.data();
    const char* const end = begin + text.size();
    std::int64_t value = 0;
    const auto result = std::from_chars(begin, end, value, 10);
    if (result.ec != std::errc() || result.ptr != end) {
        return false;
    }
    *out = value;
    return true;
}

/// @brief 今天 00:00:00 的本地 Unix 秒（用于趋势窗口起点）。
std::int64_t LocalMidnight(std::int64_t now) {
    const std::time_t seconds = static_cast<std::time_t>(now);
    std::tm local{};
    if (localtime_r(&seconds, &local) == nullptr) {
        return now;
    }
    local.tm_hour = 0;
    local.tm_min = 0;
    local.tm_sec = 0;
    local.tm_isdst = -1;
    return static_cast<std::int64_t>(std::mktime(&local));
}

/// @brief 把 Unix 秒格式化成 `YYYY-MM-DD`（本地时区，与 SQL 的 localtime 一致）。
std::string FormatDay(std::int64_t seconds) {
    const std::time_t stamp = static_cast<std::time_t>(seconds);
    std::tm local{};
    if (localtime_r(&stamp, &local) == nullptr) {
        return std::string();
    }
    char buffer[16] = {};
    if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%d", &local) == 0) {
        return std::string();
    }
    return std::string(buffer);
}

/// @brief 创作者总览（overview 接口的返回值）。
struct CreatorOverview {
    std::int64_t play_count{0};     ///< 总播放量（点赞 + 评论的估算值）。
    std::int64_t like_count{0};     ///< 作品收到的有效点赞总数。
    std::int64_t comment_count{0};  ///< 作品下未删除的评论总数。
    std::int64_t follower_count{0}; ///< 粉丝数。
    std::int64_t video_count{0};    ///< 未删除的作品数。
};

/// @brief 趋势里的一天。
struct TrendPoint {
    std::string day;              ///< `YYYY-MM-DD`。
    std::int64_t likes{0};        ///< 当天新增点赞。
    std::int64_t comments{0};     ///< 当天新增评论。
    std::int64_t followers{0};    ///< 当天新增粉丝。
};

/// @brief TOP 作品条目。
struct TopVideo {
    std::int64_t id{0};            ///< 视频 id。
    std::string title;             ///< 标题。
    std::string url;               ///< 播放地址。
    std::string cover_url;         ///< 封面地址（可为空）。
    std::int64_t created_at{0};    ///< 发布时间（Unix 秒）。
    std::int64_t like_count{0};    ///< 有效点赞数。
    std::int64_t comment_count{0}; ///< 未删除评论数。
    std::int64_t score{0};         ///< 互动量 = 点赞 × 2 + 评论 × 3。
};

/**
 * @brief 读取本人总览：一条 SQL 聚合作品数 / 点赞 / 评论 / 粉丝。
 *
 * @param store [in] 已打开的存储层——借用，调用期间有效。
 * @param user_id [in] 创作者 id（>0，已由 token 校验）。
 * @param out [out] 成功时整体写入；失败保持原值。
 * @param error [out] 失败时写错误文本；成功保持原值。
 * @return 成功返回 true；数据库不可用或查询失败返回 false。
 *
 * ## 定位
 * 概览数字的唯一来源；`total_plays` 不是真实播放埋点，而是按
 * 「点赞 + 评论」估算（PRD 允许），页面已标注「估算」。
 *
 * ## 并发/失败语义
 * - 调用约束：HTTP 事件循环单线程调用；DB 内部自带锁，无额外同步；
 * - 原子性：四个 COUNT 在同一条 SQL 内读取同一快照；
 * - 失败语义：返回 false 时调用方回 500，**不要**回 0 数字。
 */
bool LoadOverview(douyin::Store& store, std::int64_t user_id,
                  CreatorOverview* out, std::string* error) {
    if (store.Database() == nullptr) {
        if (error != nullptr) {
            *error = "数据库尚未打开";
        }
        return false;
    }
    // 一条 SQL 取四个数：作品数、点赞（JOIN 未删除视频）、未删除评论、粉丝数。
    auto rows = store.Database()->Query(
        "SELECT"
        " (SELECT COUNT(*) FROM videos WHERE user_id = ? AND deleted_at IS NULL),"
        " (SELECT COUNT(*) FROM likes l JOIN videos v ON v.id = l.video_id"
        "   WHERE v.user_id = ? AND v.deleted_at IS NULL),"
        " (SELECT COUNT(*) FROM comments c JOIN videos v ON v.id = c.video_id"
        "   WHERE v.user_id = ? AND v.deleted_at IS NULL AND c.deleted_at IS NULL),"
        " (SELECT COUNT(*) FROM follows WHERE followee_id = ?)",
        user_id, user_id, user_id, user_id);
    if (!rows) {
        if (error != nullptr) {
            *error = "统计总览失败: " + rows.Err().ToString();
        }
        return false;
    }
    if (!rows.Value().Next()) {
        if (error != nullptr) {
            *error = "统计总览失败: 空结果";
        }
        return false;
    }
    CreatorOverview overview;
    overview.video_count = rows.Value().Int64(0);
    overview.like_count = rows.Value().Int64(1);
    overview.comment_count = rows.Value().Int64(2);
    overview.follower_count = rows.Value().Int64(3);
    overview.play_count = overview.like_count + overview.comment_count;
    *out = overview;
    return true;
}

/**
 * @brief 读取按天聚合的互动趋势（昨天 0 点前的窗口不返回）。
 *
 * @param store [in] 已打开的存储层——借用。
 * @param user_id [in] 创作者 id（>0）。
 * @param days [in] 窗口天数，只允许 7 或 30；调用方先行校验。
 * @param now [in] 当前 Unix 秒，用于确定「今天 0 点」。
 * @param out [out] 成功时按日期升序写满 `days` 个点（空白天补 0）；失败未定义。
 * @param error [out] 失败时写错误文本。
 * @return 成功返回 true。
 *
 * ## 定位
 * 趋势图唯一数据源。三种来源用 `UNION ALL` 合成一张临时表，再**单条**
 * `GROUP BY date` 汇总；没有「每天一条 SQL」的 N+1。
 *
 * ## 步骤
 * step1. 计算今天 0 点与窗口起点（含今天，共 days 天）；
 * step2. 一条 SQL 按本地日期分组统计点赞 / 评论 / 粉丝；
 * step3. 把结果放进 map，再按日期升序补满空白天（不查数据库）。
 *
 * ## 并发/失败语义
 * - 调用约束：HTTP 事件循环单线程；
 * - 失败语义：SQL 失败返回 false，调用方回 500，**不要**返回半张图；
 * - 不变量：成功时 `out->size() == days`，且日期严格升序、无重复。
 */
bool LoadTrend(douyin::Store& store, std::int64_t user_id, std::int64_t days,
               std::int64_t now, std::vector<TrendPoint>* out,
               std::string* error) {
    if (store.Database() == nullptr) {
        if (error != nullptr) {
            *error = "数据库尚未打开";
        }
        return false;
    }
    // step1: 今天 0 点往前推 days-1 天；窗口包含今天。
    const std::int64_t today = LocalMidnight(now);
    const std::int64_t start = today - (days - 1) * kSecondsPerDay;
    // step2: 单条 SQL：三个来源 UNION ALL 后按日期 GROUP BY。
    const std::string sql =
        "SELECT day, SUM(likes), SUM(comments), SUM(followers) FROM ("
        " SELECT date(l.created_at, 'unixepoch', 'localtime') AS day,"
        "        1 AS likes, 0 AS comments, 0 AS followers"
        "   FROM likes l JOIN videos v ON v.id = l.video_id"
        "  WHERE v.user_id = ? AND v.deleted_at IS NULL AND l.created_at >= ?"
        " UNION ALL"
        " SELECT date(c.created_at, 'unixepoch', 'localtime'), 0, 1, 0"
        "   FROM comments c JOIN videos v ON v.id = c.video_id"
        "  WHERE v.user_id = ? AND v.deleted_at IS NULL"
        "    AND c.deleted_at IS NULL AND c.created_at >= ?"
        " UNION ALL"
        " SELECT date(f.created_at, 'unixepoch', 'localtime'), 0, 0, 1"
        "   FROM follows f WHERE f.followee_id = ? AND f.created_at >= ?"
        ") GROUP BY day ORDER BY day";
    auto rows = store.Database()->Query(sql, user_id, start, user_id, start,
                                        user_id, start);
    if (!rows) {
        if (error != nullptr) {
            *error = "统计趋势失败: " + rows.Err().ToString();
        }
        return false;
    }
    std::vector<TrendPoint> aggregated;
    while (rows.Value().Next()) {
        TrendPoint point;
        point.day = rows.Value().Text(0);
        point.likes = rows.Value().Int64(1);
        point.comments = rows.Value().Int64(2);
        point.followers = rows.Value().Int64(3);
        aggregated.push_back(std::move(point));
    }
    if (rows.Value().Err()) {
        if (error != nullptr) {
            *error = "遍历趋势失败: " + rows.Value().Err().ToString();
        }
        return false;
    }
    // step3: 按日期升序补满窗口；缺失的白天写 0（前端柱状图需要连续 X 轴）。
    std::vector<TrendPoint> filled;
    filled.reserve(static_cast<std::size_t>(days));
    std::size_t cursor = 0;
    for (std::int64_t offset = 0; offset < days; ++offset) {
        TrendPoint point;
        point.day = FormatDay(start + offset * kSecondsPerDay);
        while (cursor < aggregated.size() && aggregated[cursor].day < point.day) {
            ++cursor;  // 窗口外的脏数据（理论上不会出现）直接跳过。
        }
        if (cursor < aggregated.size() && aggregated[cursor].day == point.day) {
            point.likes = aggregated[cursor].likes;
            point.comments = aggregated[cursor].comments;
            point.followers = aggregated[cursor].followers;
        }
        filled.push_back(std::move(point));
    }
    *out = std::move(filled);
    return true;
}

/**
 * @brief 读取本人互动量最高的作品（TOP N）。
 *
 * @param store [in] 已打开的存储层——借用。
 * @param user_id [in] 创作者 id（>0）。
 * @param limit [in] 最多返回条数（1~50，调用方已夹紧）。
 * @param out [out] 成功时按互动量降序写入；失败未定义。
 * @param error [out] 失败时写错误文本。
 * @return 成功返回 true（可能是空列表）。
 *
 * ## 定位
 * TOP 榜唯一数据源。计数用 `likes` / `comments` 实表实时聚合（而不是
 * `videos.like_count` 缓存列），保证算法与页面展示一致；CTE 让点赞/评论
 * 子查询只在一条 SQL 内执行，不是 N+1。
 *
 * ## 并发/失败语义
 * - 调用约束：HTTP 事件循环单线程；
 * - 排序：互动量降序，同分按 id 降序（结果稳定、可复现）；
 * - 失败语义：SQL 失败返回 false，调用方回 500。
 */
bool LoadTopVideos(douyin::Store& store, std::int64_t user_id, std::int64_t limit,
                   std::vector<TopVideo>* out, std::string* error) {
    if (store.Database() == nullptr) {
        if (error != nullptr) {
            *error = "数据库尚未打开";
        }
        return false;
    }
    auto rows = store.Database()->Query(
        "WITH stats AS ("
        " SELECT v.id, v.title, v.url, v.cover_url, v.created_at,"
        "        (SELECT COUNT(*) FROM likes l WHERE l.video_id = v.id) AS likes,"
        "        (SELECT COUNT(*) FROM comments c WHERE c.video_id = v.id"
        "           AND c.deleted_at IS NULL) AS comments"
        "   FROM videos v"
        "  WHERE v.user_id = ? AND v.deleted_at IS NULL)"
        " SELECT id, title, url, cover_url, created_at, likes, comments,"
        "        likes * 2 + comments * 3 AS score"
        "  FROM stats ORDER BY score DESC, id DESC LIMIT ?",
        user_id, limit);
    if (!rows) {
        if (error != nullptr) {
            *error = "统计 TOP 作品失败: " + rows.Err().ToString();
        }
        return false;
    }
    std::vector<TopVideo> videos;
    while (rows.Value().Next()) {
        TopVideo video;
        video.id = rows.Value().Int64(0);
        video.title = rows.Value().Text(1);
        video.url = rows.Value().Text(2);
        video.cover_url = rows.Value().Text(3);
        video.created_at = rows.Value().Int64(4);
        video.like_count = rows.Value().Int64(5);
        video.comment_count = rows.Value().Int64(6);
        video.score = rows.Value().Int64(7);
        videos.push_back(std::move(video));
    }
    if (rows.Value().Err()) {
        if (error != nullptr) {
            *error = "遍历 TOP 作品失败: " + rows.Value().Err().ToString();
        }
        return false;
    }
    *out = std::move(videos);
    return true;
}

/**
 * @brief 校验可选的 `user_id` query 参数：只允许「本人」。
 *
 * @param request [in] 入站请求——借用；读 `user_id` query。
 * @param viewer_id [in] 当前 token 的用户 id（>0）。
 * @param response [out] 越权/非法时写 400/403；合法保持原值。
 * @return 合法（参数缺省视为本人）返回 true；已写错误响应返回 false，调用方
 *         应直接结束请求。
 *
 * ## 定位
 * 「只能看自己」的显式越权检查点：接口本身没有 `:id` 路径参数，但客户端
 * 可能带 `?user_id=别人`；这里统一 403，避免以后加参数时漏掉校验。
 */
bool CheckTargetUser(const douyin::HttpRequest& request, std::int64_t viewer_id,
                     douyin::HttpResponse* response) {
    const std::string text = douyin::QueryParam(request, "user_id");
    if (text.empty()) {
        return true;
    }
    std::int64_t target = 0;
    if (!ParseInt64(text, &target) || target <= 0) {
        Fail(response, 400, "user_id 非法");
        return false;
    }
    if (target != viewer_id) {
        Fail(response, 403, "只能查看自己的数据");
        return false;
    }
    return true;
}

/// @brief 处理 overview 接口；调用前已认证且已校验 user_id。
bool HandleOverview(douyin::Store& store, std::int64_t user_id,
                    douyin::HttpResponse* response) {
    CreatorOverview overview;
    std::string error;
    if (!LoadOverview(store, user_id, &overview, &error)) {
        std::fprintf(stderr, "[%s] %s\n", kServiceName, error.c_str());
        return Fail(response, 500, "统计数据失败");
    }
    std::string body = "{\"ok\":true,\"overview\":{";
    body += "\"total_plays\":" + std::to_string(overview.play_count);
    body += ",\"total_likes\":" + std::to_string(overview.like_count);
    body += ",\"total_comments\":" + std::to_string(overview.comment_count);
    body += ",\"followers\":" + std::to_string(overview.follower_count);
    body += ",\"video_count\":" + std::to_string(overview.video_count);
    body += ",\"plays_estimated\":true}}";
    response->status = 200;
    response->headers["Cache-Control"] = "no-store";
    response->body = std::move(body);
    return true;
}

/// @brief 处理 trend 接口：`days` 只接受 7 / 30，缺省 7。
bool HandleTrend(douyin::Store& store, const douyin::HttpRequest& request,
                 std::int64_t user_id, douyin::HttpResponse* response) {
    std::int64_t days = kShortWindowDays;
    const std::string text = douyin::QueryParam(request, "days");
    if (!text.empty()) {
        if (!ParseInt64(text, &days) ||
            (days != kShortWindowDays && days != kLongWindowDays)) {
            return Fail(response, 400, "days 只支持 7 或 30");
        }
    }
    std::vector<TrendPoint> points;
    std::string error;
    if (!LoadTrend(store, user_id, days, NowSeconds(), &points, &error)) {
        std::fprintf(stderr, "[%s] %s\n", kServiceName, error.c_str());
        return Fail(response, 500, "统计数据失败");
    }
    std::string body = "{\"ok\":true,\"days\":" + std::to_string(days) +
                       ",\"points\":[";
    for (std::size_t i = 0; i < points.size(); ++i) {
        if (i != 0) {
            body += ',';
        }
        body += "{\"date\":\"" + douyin::JsonEscape(points[i].day) + "\"";
        body += ",\"likes\":" + std::to_string(points[i].likes);
        body += ",\"comments\":" + std::to_string(points[i].comments);
        body += ",\"followers\":" + std::to_string(points[i].followers) + "}";
    }
    body += "]}";
    response->status = 200;
    response->headers["Cache-Control"] = "no-store";
    response->body = std::move(body);
    return true;
}

/// @brief 处理 top-videos 接口：`limit` 缺省 5，夹紧到 [1,50]。
bool HandleTopVideos(douyin::Store& store, const douyin::HttpRequest& request,
                     std::int64_t user_id, douyin::HttpResponse* response) {
    std::int64_t limit = kDefaultTopLimit;
    const std::string text = douyin::QueryParam(request, "limit");
    if (!text.empty()) {
        if (!ParseInt64(text, &limit) || limit <= 0) {
            return Fail(response, 400, "limit 非法");
        }
        if (limit > kMaxTopLimit) {
            limit = kMaxTopLimit;
        }
    }
    std::vector<TopVideo> videos;
    std::string error;
    if (!LoadTopVideos(store, user_id, limit, &videos, &error)) {
        std::fprintf(stderr, "[%s] %s\n", kServiceName, error.c_str());
        return Fail(response, 500, "统计数据失败");
    }
    std::string body = "{\"ok\":true,\"limit\":" + std::to_string(limit) +
                       ",\"count\":" + std::to_string(videos.size()) +
                       ",\"videos\":[";
    for (std::size_t i = 0; i < videos.size(); ++i) {
        if (i != 0) {
            body += ',';
        }
        body += "{\"id\":" + std::to_string(videos[i].id);
        body += ",\"title\":\"" + douyin::JsonEscape(videos[i].title) + "\"";
        body += ",\"url\":\"" + douyin::JsonEscape(videos[i].url) + "\"";
        body += ",\"cover_url\":\"" + douyin::JsonEscape(videos[i].cover_url) + "\"";
        body += ",\"created_at\":" + std::to_string(videos[i].created_at);
        body += ",\"like_count\":" + std::to_string(videos[i].like_count);
        body += ",\"comment_count\":" + std::to_string(videos[i].comment_count);
        body += ",\"score\":" + std::to_string(videos[i].score) + "}";
    }
    body += "]}";
    response->status = 200;
    response->headers["Cache-Control"] = "no-store";
    response->body = std::move(body);
    return true;
}

/**
 * @brief 全部路由的入口：页面 + 三个只读统计接口。
 *
 * @param store [in] 已打开的存储层——借用，调用期间有效。
 * @param request [in] 入站请求——借用；读 method / path / query / Authorization。
 * @param response [out] 出站响应——命中路由时整体写入。
 * @return true 表示已处理（含 4xx/5xx）；false 交给 HttpServer 回 404。
 *
 * ## 定位
 * creator 服务的唯一路由分发点；`/creator` 页面无鉴权（页面自己带 token
 * 调 API），`/api/creator/` 下的接口必须先过 `RequireToken`。
 *
 * ## 步骤
 * step0. `/health` 返回服务存活 JSON（与其它服务同一契约）；
 * step1. `/creator` / `/creator.html`：GET/HEAD 返回 HTML，其它方法 405；
 * step2. 非 `/api/creator/` 前缀返回 false（404）；
 * step3. 非 GET/HEAD 的 API 请求回 405（不做方法语义重写）；
 * step4. Bearer 校验失败直接结束（401）；
 * step5. 可选 `user_id` 越权检查（403）；
 * step6. 按路径分派 overview / trend / top-videos，未知路径 404。
 *
 * ## 并发/失败语义
 * - 调用约束：HttpServer 串行事件循环调用，无并发写；
 * - 失败语义：所有错误都写进 response，不抛异常；
 * - 不变量：`/api/creator/` 下的响应一定经过 token 校验。
 */
bool Handle(douyin::Store& store, const douyin::HttpRequest& request,
            douyin::HttpResponse* response) {
    const std::string_view path(request.path);
    // step0: `/health` 与其它服务保持同一契约（accept_all 用它等启动）。
    if (request.method == "GET" && path == "/health") {
        response->status = 200;
        response->body = "{\"ok\":true,\"service\":\"creator\"}";
        return true;
    }
    // step1: 页面是无鉴权静态壳，token 由浏览器 localStorage 带给 API。
    if (path == "/creator" || path == "/creator.html") {
        if (request.method != "GET" && request.method != "HEAD") {
            return Fail(response, 405, "方法不允许");
        }
        response->status = 200;
        response->content_type = "text/html; charset=utf-8";
        response->headers["Cache-Control"] = "no-cache";
        response->body = douyin::kCreatorPageHtml;
        return true;
    }
    // step2: 只接管自己的 API 前缀，其余路径交给 404。
    constexpr std::string_view kApiPrefix = "/api/creator/";
    if (path.size() <= kApiPrefix.size() ||
        path.compare(0, kApiPrefix.size(), kApiPrefix) != 0) {
        return false;
    }
    // step3: 统计接口只有读语义。
    if (request.method != "GET" && request.method != "HEAD") {
        return Fail(response, 405, "方法不允许");
    }
    // step4: 服务内再做一次鉴权，保证直连 29108 也不能越权。
    std::int64_t user_id = 0;
    if (!RequireToken(request, response, &user_id)) {
        return true;
    }
    // step5: 显式越权：客户端带别人的 user_id 一律 403。
    if (!CheckTargetUser(request, user_id, response)) {
        return true;
    }
    // step6: 分派三个统计接口。
    const std::string_view name = path.substr(kApiPrefix.size());
    if (name == "overview") {
        return HandleOverview(store, user_id, response);
    }
    if (name == "trend") {
        return HandleTrend(store, request, user_id, response);
    }
    if (name == "top-videos") {
        return HandleTopVideos(store, request, user_id, response);
    }
    return Fail(response, 404, "not found");
}

/// @brief 解析命令行端口（缺省 29108）；非法时退出。
std::uint16_t ParsePort(int argc, char** argv) {
    if (argc <= 1) {
        return kDefaultPort;
    }
    char* end = nullptr;
    const long value = std::strtol(argv[1], &end, 10);
    if (end == argv[1] || *end != '\0' || value <= 0 || value > 65535) {
        std::fprintf(stderr, "[%s] 非法端口：%s\n", kServiceName, argv[1]);
        std::exit(2);
    }
    return static_cast<std::uint16_t>(value);
}

}  // namespace

int main(int argc, char** argv) {
    const std::uint16_t port = ParsePort(argc, argv);

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
    // 就绪探针：videos 表可读即 ready（本服务只读统计，无写依赖）。
    server.SetReadinessProbe([&store](std::string* detail) {
        if (!store.Ok() || store.Database() == nullptr) {
            if (detail != nullptr) {
                *detail = "db not open";
            }
            return false;
        }
        auto rows = store.Database()->Query("SELECT 1 FROM videos LIMIT 1");
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
    std::printf("[%s] 监听 0.0.0.0:%u db=%s\n", kServiceName,
                static_cast<unsigned>(port), db_path.c_str());
    std::fflush(stdout);

    server.ServeForever([&store](const douyin::HttpRequest& request,
                                 douyin::HttpResponse* response) {
        return Handle(store, request, response);
    });
    return 0;
}
