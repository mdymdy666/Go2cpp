/**
 * @file admin_service.cpp
 * @brief 抖音 P0 运营端 admin 服务（29107）：举报处理 + 后台页面。
 *
 * @details 路由（全部由网关 `/api/admin` 前缀转发，网关只要求登录，
 *          **admin 身份在本服务内校验**）：
 *          - `GET  /api/admin/reports?status=&cursor=&limit=` 举报列表；
 *          - `POST /api/admin/reports/:id/handle` 处理举报
 *            （body `{"action":"dismiss"|"delete_target"}`）；
 *          - `GET  /api/admin/stats` 待处理 / 今日新增 / 已处理；
 *          - `GET  /admin` 运营页（静态 HTML，页面数据走上面的接口）。
 *
 *          鉴权：Bearer token → `VerifyToken` → user_id ∈
 *          `DOUYIN_ADMIN_USER_IDS`（缺省 1）。无 token / 无效 token 回 401，
 *          已登录但非 admin 回 403。
 *
 *          软删除复用 `videos.deleted_at` / `comments.deleted_at`：目标一旦
 *          标记删除，视频详情、feed、评论列表等既有查询全部按 404/不可见
 *          处理；评论删除后同步重算所属视频的 `comment_count`。
 *
 * @note 存储层复用 `douyin::Store` 打开的同一个 SQLite 连接（WAL），
 *       admin 专用查询通过 `Store::Database()` 直发 SQL；不改动
 *       `douyin_store.*`，避免与其它服务并发改动冲突。
 *
 * @author douyin demo
 * @date 2026-11-06
 * @copyright MIT License
 */

#include "douyin_admin_page.hpp"
#include "douyin_auth.hpp"
#include "douyin_http.hpp"
#include "douyin_store.hpp"
#include "go2cpp/storage/db.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using douyin::HttpRequest;
using douyin::HttpResponse;
using go2cpp::storage::DB;
using go2cpp::storage::Rows;

/// @brief 默认监听端口；网关 admin 上游固定 29107。
constexpr std::uint16_t kDefaultPort = 29107;
/// @brief 服务名（日志 / metrics 用）。
constexpr const char* kServiceName = "admin";
/// @brief 列表默认每页条数。
constexpr std::int64_t kDefaultPageSize = 20;
/// @brief 列表每页上限。
constexpr std::int64_t kMaxPageSize = 100;

/// @brief 单线程事件循环上的计数器（无需原子）。
struct ServiceMetrics {
    std::uint64_t requests{0};  ///< 已处理请求数。
    std::uint64_t errors{0};    ///< 已返回的错误响应数。
};

ServiceMetrics g_metrics;

/// @brief 当前 Unix 秒。
std::int64_t NowSeconds() {
    return static_cast<std::int64_t>(std::time(nullptr));
}

/// @brief 读环境变量；未设置或空串都返回空串。
std::string EnvOrEmpty(const char* name) {
    const char* value = std::getenv(name);
    return value == nullptr ? std::string() : std::string(value);
}

/**
 * @brief 管理员 id 集合（`DOUYIN_ADMIN_USER_IDS`，逗号分隔）。
 *
 * @details 与 interaction 服务同一套环境变量语义：缺省 `1`（演示环境首个
 *          注册用户即管理员）；非法片段直接忽略，不会把 0 / 负数当 admin。
 *          进程内只解析一次（首次调用），运行期不可变。
 *
 * @return 管理员 id 列表；可能为空（环境变量全是非法值时）。
 *
 * ## 定位
 * 运营端权限边界的**唯一来源**。错了会怎样：普通用户拿到举报列表、
 * 甚至批量删内容。
 */
const std::vector<std::int64_t>& AdminUserIds() {
    static const std::vector<std::int64_t> ids = [] {
        std::string text = EnvOrEmpty("DOUYIN_ADMIN_USER_IDS");
        if (text.empty()) {
            text = "1";
        }
        std::vector<std::int64_t> parsed;
        std::size_t pos = 0;
        while (pos <= text.size()) {
            const std::size_t comma = text.find(',', pos);
            const std::string part = text.substr(
                pos, comma == std::string::npos ? std::string::npos
                                                : comma - pos);
            if (!part.empty()) {
                char* end = nullptr;
                const long long value = std::strtoll(part.c_str(), &end, 10);
                if (end != part.c_str() && *end == '\0' && value > 0) {
                    parsed.push_back(static_cast<std::int64_t>(value));
                }
            }
            if (comma == std::string::npos) {
                break;
            }
            pos = comma + 1;
        }
        return parsed;
    }();
    return ids;
}

/// @brief 是否是管理员。
bool IsAdminUser(std::int64_t user_id) {
    if (user_id <= 0) {
        return false;
    }
    for (const std::int64_t id : AdminUserIds()) {
        if (id == user_id) {
            return true;
        }
    }
    return false;
}

/// @brief 统一错误响应。
bool Fail(HttpResponse* response, int status, std::string_view message) {
    response->status = status;
    response->body = douyin::ErrorBody(status, message);
    ++g_metrics.errors;
    return true;
}

/**
 * @brief 要求「已登录且是管理员」。
 *
 * @param request [in] 原始请求；只读 Authorization 头。
 * @param response [out] 失败时写 401（无/坏 token）或 403（非 admin）。
 * @param actor [out] 成功时写管理员 user_id；失败时保持原值。
 * @return true=通过，调用方继续；false=已写响应，调用方**必须**直接返回 true。
 *
 * ## 定位
 * 所有 `/api/admin` 接口的权限闸门。错了会怎样：非 admin 拿到全部举报数据。
 *
 * ## 并发/失败语义
 * - 调用线程：HTTP 事件循环单线程，无需加锁；
 * - 401/403 严格区分：无凭证是「未登录」，有凭证但不是管理员是「无权」。
 */
bool RequireAdmin(const HttpRequest& request, HttpResponse* response,
                  std::int64_t* actor) {
    const std::string bearer = douyin::BearerToken(request);
    if (bearer.empty()) {
        (void)Fail(response, 401, "请先登录");
        return false;
    }
    douyin::TokenInfo info;
    std::string error;
    if (!douyin::VerifyToken(bearer, NowSeconds(), &info, &error)) {
        (void)Fail(response, 401, "登录已过期，请重新登录");
        return false;
    }
    if (!IsAdminUser(info.user_id)) {
        (void)Fail(response, 403, "需要管理员权限");
        return false;
    }
    *actor = info.user_id;
    return true;
}

/// @brief 解析正整数 id；空串 / 非数字 / 溢出 / 0 都返回 false。
bool ParsePositiveId(std::string_view text, std::int64_t* out) {
    if (text.empty() || text.size() > 18) {
        return false;
    }
    std::int64_t value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10 + (c - '0');
    }
    if (value <= 0) {
        return false;
    }
    *out = value;
    return true;
}

/**
 * @brief 从 `/api/admin/reports/<id>/handle` 解析举报 id。
 * @param path [in] 不含 query 的请求路径。
 * @param id [out] 命中时写举报 id（>0）；未命中保持原值。
 * @return 命中返回 true；其它路径返回 false（调用方继续别的分支）。
 */
bool ParseHandlePath(std::string_view path, std::int64_t* id) {
    constexpr std::string_view kPrefix = "/api/admin/reports/";
    constexpr std::string_view kSuffix = "/handle";
    if (path.size() <= kPrefix.size() + kSuffix.size() ||
        path.compare(0, kPrefix.size(), kPrefix) != 0) {
        return false;
    }
    if (path.compare(path.size() - kSuffix.size(), kSuffix.size(), kSuffix) !=
        0) {
        return false;
    }
    const std::string_view id_text =
        path.substr(kPrefix.size(), path.size() - kPrefix.size() - kSuffix.size());
    return ParsePositiveId(id_text, id);
}

/// @brief 解析 `limit`：缺省 20，夹紧到 [1,100]；非数字按缺省处理。
std::int64_t ParseLimit(const HttpRequest& request) {
    const std::string text = douyin::QueryParam(request, "limit");
    if (text.empty()) {
        return kDefaultPageSize;
    }
    char* end = nullptr;
    const long long value = std::strtoll(text.c_str(), &end, 10);
    if (end == text.c_str() || value <= 0) {
        return kDefaultPageSize;
    }
    return value > kMaxPageSize ? kMaxPageSize
                                : static_cast<std::int64_t>(value);
}

/**
 * @brief 读取分页游标（上一页最后一条举报 id）。
 * @param request [in] 原始请求。
 * @param cursor [out] 空串时写 0（第一页）；合法时写 >0 的 id。
 * @return 合法（含空串）返回 true；非数字 / 0 / 溢出返回 false（回 400）。
 */
bool ReadCursor(const HttpRequest& request, std::int64_t* cursor) {
    const std::string text = douyin::QueryParam(request, "cursor");
    *cursor = 0;
    if (text.empty()) {
        return true;
    }
    return ParsePositiveId(text, cursor);
}

/// @brief Unix 秒 → 本地时间 `YYYY-MM-DD HH:MM:SS`；失败返回空串。
std::string FormatTime(std::int64_t seconds) {
    const std::time_t t = static_cast<std::time_t>(seconds);
    std::tm local{};
    if (localtime_r(&t, &local) == nullptr) {
        return std::string();
    }
    char buffer[32];
    if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &local) ==
        0) {
        return std::string();
    }
    return std::string(buffer);
}

/// @brief 本地「今天 00:00:00」的 Unix 秒（统计今日新增用）。
std::int64_t TodayStartSeconds() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    if (localtime_r(&now, &local) == nullptr) {
        return 0;
    }
    local.tm_hour = 0;
    local.tm_min = 0;
    local.tm_sec = 0;
    return static_cast<std::int64_t>(std::mktime(&local));
}

/// @brief 运营端看到的一行举报（含举报人昵称与目标摘要）。
struct ReportRow {
    std::int64_t id{0};             ///< 举报 id。
    std::int64_t reporter_id{0};    ///< 举报人 id。
    std::string reporter_name;      ///< 举报人昵称（LEFT JOIN 回填，可能为空）。
    std::string target_type;        ///< video / comment / user。
    std::int64_t target_id{0};      ///< 目标 id。
    std::string target_summary;     ///< 目标摘要：视频标题 / 评论正文 / 用户名。
    std::string reason;             ///< 举报原因。
    std::string detail;             ///< 补充说明（可空）。
    std::string status;             ///< pending / handled。
    std::int64_t created_at{0};     ///< Unix 秒。
};

/// @brief 列表 / 单条共用的 SELECT 列（顺序必须与 `ReadReportRow` 一致）。
constexpr const char* kReportSelect =
    "SELECT r.id, r.reporter_id, COALESCE(u.name, ''), r.target_type,"
    " r.target_id,"
    " COALESCE(CASE r.target_type"
    "   WHEN 'video' THEN (SELECT v.title FROM videos v WHERE v.id = r.target_id)"
    "   WHEN 'comment' THEN"
    "     (SELECT c.content FROM comments c WHERE c.id = r.target_id)"
    "   WHEN 'user' THEN (SELECT x.name FROM users x WHERE x.id = r.target_id)"
    "   ELSE '' END, ''),"
    " r.reason, r.detail, r.status, r.created_at"
    " FROM reports r LEFT JOIN users u ON u.id = r.reporter_id";

/// @brief 读当前行（列顺序见 `kReportSelect`）。
void ReadReportRow(Rows* rows, ReportRow* out) {
    out->id = rows->Int64(0);
    out->reporter_id = rows->Int64(1);
    out->reporter_name = rows->Text(2);
    out->target_type = rows->Text(3);
    out->target_id = rows->Int64(4);
    out->target_summary = rows->Text(5);
    out->reason = rows->Text(6);
    out->detail = rows->Text(7);
    out->status = rows->Text(8);
    out->created_at = rows->Int64(9);
}

/// @brief 单条举报序列化为 JSON 对象。
std::string ReportJson(const ReportRow& item) {
    return std::string("{\"id\":") + std::to_string(item.id) +
           ",\"reporter_id\":" + std::to_string(item.reporter_id) +
           ",\"reporter_name\":\"" +
           douyin::JsonEscape(item.reporter_name) + "\"" +
           ",\"target_type\":\"" + douyin::JsonEscape(item.target_type) +
           "\"" + ",\"target_id\":" + std::to_string(item.target_id) +
           ",\"target_summary\":\"" +
           douyin::JsonEscape(item.target_summary) + "\"" +
           ",\"reason\":\"" + douyin::JsonEscape(item.reason) + "\"" +
           ",\"detail\":\"" + douyin::JsonEscape(item.detail) + "\"" +
           ",\"status\":\"" + douyin::JsonEscape(item.status) + "\"" +
           ",\"created_at\":" + std::to_string(item.created_at) +
           ",\"created_at_text\":\"" + FormatTime(item.created_at) + "\"}";
}

/**
 * @brief 查询举报列表（按 id 倒序，`cursor` 为上一页最后一条 id）。
 *
 * @param db [in] 已打开的存储连接；不可为 nullptr，借用。
 * @param status [in] 只取 `pending` 或 `handled`（调用方已白名单校验）。
 * @param cursor [in] 0=第一页；>0=只取 id 小于它的行。
 * @param limit [in] 本页最多条数（>0，<=100）。
 * @param out [out] 追加到末尾；不清空调用方已有内容。
 * @param has_more [out] 是否还有下一页；成功时写入。
 * @param error [out] 失败时写错误文本；成功时保持原值。
 * @return true=成功（可能是空列表）；false=数据库错误。
 *
 * ## 定位
 * 运营列表的唯一查询点；分页靠 id 而非 offset，避免处理过程中
 * 新举报插入导致翻页漂移。
 *
 * ## 并发/失败语义
 * - 调用线程：HTTP 事件循环单线程；DB 内部自带互斥。
 * - 失败语义：SQL 出错返回 false，**不**部分写入 out（先攒再换）。
 * - 不变量：`has_more=true` 时 `out` 恰好 limit 条，且最后一条 id 可作游标。
 */
bool ListReports(DB* db, const std::string& status, std::int64_t cursor,
                 std::int64_t limit, std::vector<ReportRow>* out,
                 bool* has_more, std::string* error) {
    *has_more = false;
    const std::int64_t fetch = limit + 1;
    auto result =
        cursor > 0
            ? db->Query(std::string(kReportSelect) +
                            " WHERE r.status = ? AND r.id < ?"
                            " ORDER BY r.id DESC LIMIT ?",
                        status, cursor, fetch)
            : db->Query(std::string(kReportSelect) +
                            " WHERE r.status = ?"
                            " ORDER BY r.id DESC LIMIT ?",
                        status, fetch);
    if (!result) {
        *error = "查询举报列表失败: " + result.Err().ToString();
        return false;
    }
    std::vector<ReportRow> rows;
    Rows& cursor_rows = result.Value();
    while (cursor_rows.Next()) {
        ReportRow item;
        ReadReportRow(&cursor_rows, &item);
        rows.push_back(std::move(item));
    }
    if (cursor_rows.Err()) {
        *error = "遍历举报列表失败: " + cursor_rows.Err().ToString();
        return false;
    }
    if (static_cast<std::int64_t>(rows.size()) > limit) {
        rows.pop_back();
        *has_more = true;
    }
    out->insert(out->end(), std::make_move_iterator(rows.begin()),
                std::make_move_iterator(rows.end()));
    return true;
}

/**
 * @brief 按 id 查一条举报（含举报人昵称与目标摘要）。
 *
 * @param db [in] 已打开的存储连接；不可为 nullptr，借用。
 * @param id [in] 举报 id（>0）。
 * @param out [out] 命中时写字；未命中保持原值。
 * @param found [out] 是否命中；成功时写入。
 * @param error [out] 失败时写错误文本。
 * @return true=查询成功（found 区分命中）；false=数据库错误。
 */
bool LoadReport(DB* db, std::int64_t id, ReportRow* out, bool* found,
                std::string* error) {
    *found = false;
    auto result = db->Query(std::string(kReportSelect) + " WHERE r.id = ?", id);
    if (!result) {
        *error = "查询举报失败: " + result.Err().ToString();
        return false;
    }
    Rows& rows = result.Value();
    if (rows.Next()) {
        ReadReportRow(&rows, out);
        *found = true;
    }
    if (rows.Err()) {
        *error = "读取举报失败: " + rows.Err().ToString();
        return false;
    }
    return true;
}

/**
 * @brief 软删除被举报目标，并在同一事务里重算评论数。
 *
 * @param db [in] 已打开的存储连接；不可为 nullptr，借用。
 * @param target_type [in] `video` 或 `comment`（`user` 由调用方挡掉）。
 * @param target_id [in] 目标 id（>0）。
 * @param now [in] 删除时刻的 Unix 秒，写入 `deleted_at`。
 * @param deleted [out] 本次是否真的删了行（false=目标已不存在/已删）。
 * @param target_missing [out] 目标当前是否已不可见（不存在或已删）。
 * @param error [out] 失败时写错误文本。
 * @return true=提交成功；false=已回滚，库内状态不变。
 *
 * ## 定位
 * 举报处理里**唯一**会改内容可见性的路径。错了会怎样：删了评论却不
 * 重算 `comment_count`，或事务泄漏把 admin 连接钉在旧快照上。
 *
 * ## 步骤
 * step1. 开事务：软删与计数必须同生共死。
 * step2. video：一条 UPDATE 带 `deleted_at IS NULL`，changes 决定是否真删。
 * step3. comment：先查所属 video_id（只认未删评论），再软删 + 重算该视频
 *         的 comment_count（与 interaction 删除评论的语义一致）。
 * step4. 提交；任何一步失败先 Rollback 再返回 false。
 *
 * ## 并发/失败语义
 * - 调用线程：HTTP 事件循环单线程；同一连接不会被并发使用。
 * - 失败语义：失败时已 Rollback，调用方回 500 即可，**不要**重试删除。
 * - 不变量：返回 true 且 *deleted=true 时，该目标已被 `deleted_at` 标记。
 */
bool ApplyDeleteTarget(DB* db, const std::string& target_type,
                       std::int64_t target_id, std::int64_t now,
                       bool* deleted, bool* target_missing,
                       std::string* error) {
    *deleted = false;
    *target_missing = false;
    // step1: 事务边界——删内容与更新计数必须一起成功或一起失败。
    auto begin = db->Begin();
    if (begin) {
        *error = "开始事务失败: " + begin.ToString();
        return false;
    }
    bool ok = false;
    do {
        if (target_type == "video") {
            // step2: 只删未删过的行；changes=0 说明目标已删/不存在。
            auto updated = db->Exec(
                "UPDATE videos SET deleted_at = ?"
                " WHERE id = ? AND deleted_at IS NULL",
                now, target_id);
            if (!updated) {
                *error = "软删视频失败: " + updated.Err().ToString();
                break;
            }
            *deleted = updated.Value().changes > 0;
            *target_missing = !*deleted;
        } else if (target_type == "comment") {
            // step3: 先拿所属视频，评论删掉后列表按 comment_count 展示。
            auto found = db->Query(
                "SELECT video_id FROM comments"
                " WHERE id = ? AND deleted_at IS NULL",
                target_id);
            if (!found) {
                *error = "查询评论失败: " + found.Err().ToString();
                break;
            }
            std::int64_t video_id = 0;
            bool has_video = false;
            if (found.Value().Next()) {
                video_id = found.Value().Int64(0);
                has_video = true;
            }
            if (found.Value().Err()) {
                *error = "读取评论失败: " + found.Value().Err().ToString();
                break;
            }
            if (!has_video) {
                *target_missing = true;
            } else {
                auto updated = db->Exec(
                    "UPDATE comments SET deleted_at = ?"
                    " WHERE id = ? AND deleted_at IS NULL",
                    now, target_id);
                if (!updated) {
                    *error = "软删评论失败: " + updated.Err().ToString();
                    break;
                }
                *deleted = updated.Value().changes > 0;
                if (*deleted) {
                    auto counted = db->Exec(
                        "UPDATE videos SET comment_count ="
                        " (SELECT COUNT(*) FROM comments"
                        "  WHERE video_id = ? AND deleted_at IS NULL)"
                        " WHERE id = ?",
                        video_id, video_id);
                    if (!counted) {
                        *error = "重算评论数失败: " + counted.Err().ToString();
                        break;
                    }
                }
            }
        } else {
            *error = "该目标类型不支持删除内容";
            break;
        }
        ok = true;
    } while (false);
    if (!ok) {
        (void)db->Rollback();
        return false;
    }
    // step4: 提交；失败再 Rollback 一次（Commit 失败时事务可能仍打开）。
    const auto commit = db->Commit();
    if (commit) {
        *error = "提交事务失败: " + commit.ToString();
        (void)db->Rollback();
        return false;
    }
    return true;
}

/**
 * @brief `GET /api/admin/reports?status=&cursor=&limit=20`。
 *
 * @param store [in,out] 已打开的存储；借用。
 * @param request [in] 原始请求；必须带管理员 Bearer token。
 * @param response [out] 成功写
 *        `{ok,status,count,next_cursor,items:[...]}`；失败写统一错误体。
 * @return 恒为 true（已处理）。
 *
 * ## 定位
 * 运营后台列表的唯一入口。错了会怎样：管理员看不到新举报，或翻页重复/丢行。
 *
 * ## 步骤
 * step1. 鉴权（401/403 严格区分）与参数校验（status 白名单、cursor 数字）。
 * step2. 查 limit+1 条判断是否还有下一页。
 * step3. 组 JSON：next_cursor 只在 has_more 时给最后一条 id。
 *
 * ## 失败语义
 * - 数据库错误回 500，不返回半截列表（先查完再序列化）。
 */
bool HandleListReports(douyin::Store& store, const HttpRequest& request,
                       HttpResponse* response) {
    // step1: 权限与参数。
    std::int64_t actor = 0;
    if (!RequireAdmin(request, response, &actor)) {
        return true;
    }
    (void)actor;
    std::string status = douyin::QueryParam(request, "status");
    if (status.empty()) {
        status = "pending";
    }
    if (status != "pending" && status != "handled") {
        return Fail(response, 400, "status 非法（pending / handled）");
    }
    std::int64_t cursor = 0;
    if (!ReadCursor(request, &cursor)) {
        return Fail(response, 400, "cursor 非法（应为上一页最后一条举报 id）");
    }
    const std::int64_t limit = ParseLimit(request);
    // step2: 多取一条判断下一页。
    std::vector<ReportRow> items;
    bool has_more = false;
    std::string error;
    if (!ListReports(store.Database(), status, cursor, limit, &items, &has_more,
                     &error)) {
        std::fprintf(stderr, "[%s] %s\n", kServiceName, error.c_str());
        return Fail(response, 500, "数据库错误");
    }
    // step3: 组 JSON。
    std::string body = "{\"ok\":true,\"status\":\"" + status +
                       "\",\"count\":" + std::to_string(items.size()) +
                       ",\"next_cursor\":\"";
    body += has_more && !items.empty() ? std::to_string(items.back().id) : "";
    body += "\",\"items\":[";
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i != 0) {
            body += ',';
        }
        body += ReportJson(items[i]);
    }
    body += "]}";
    response->status = 200;
    response->body = std::move(body);
    return true;
}

/**
 * @brief `POST /api/admin/reports/:id/handle`，body
 *        `{"action":"dismiss"|"delete_target"}`。
 *
 * @param store [in,out] 已打开的存储；借用。
 * @param request [in] 原始请求；必须带管理员 Bearer token。
 * @param report_id [in] 路径里的举报 id（>0，已校验）。
 * @param response [out] 成功写
 *        `{ok,id,action,status,deleted,target_missing}`；失败写统一错误体。
 * @return 恒为 true（已处理）。
 *
 * ## 定位
 * 举报从 pending → handled 的**唯一**提交点，也是运营端删内容的唯一入口。
 * 错了会怎样：举报永远处理不掉，或误删未举报内容。
 *
 * ## 步骤
 * step1. 鉴权 + action 白名单（非法 400）。
 * step2. 查举报：不存在 404、已处理 409（避免重复操作产生歧义）。
 * step3. delete_target：用户类型不支持删除（400）；视频/评论走软删事务。
 * step4. 置 `status='handled'`；changes=0 表示并发下已被别人处理 → 409。
 *
 * ## 并发/失败语义
 * - 删除与置 handled 不是同一事务：删成功但标记失败时，重试会以
 *   `target_missing=true` 继续置 handled，可自愈。
 * - 不变量：返回 200 时该举报在库里必为 handled。
 */
bool HandleReportAction(douyin::Store& store, const HttpRequest& request,
                        std::int64_t report_id, HttpResponse* response) {
    // step1: 权限与 action。
    std::int64_t actor = 0;
    if (!RequireAdmin(request, response, &actor)) {
        return true;
    }
    (void)actor;
    std::string action;
    if (!douyin::JsonStringField(request.body, "action", &action) ||
        (action != "dismiss" && action != "delete_target")) {
        return Fail(response, 400, "action 非法（dismiss / delete_target）");
    }
    // step2: 读举报，区分不存在 / 已处理。
    ReportRow row;
    bool found = false;
    std::string error;
    if (!LoadReport(store.Database(), report_id, &row, &found, &error)) {
        std::fprintf(stderr, "[%s] %s\n", kServiceName, error.c_str());
        return Fail(response, 500, "数据库错误");
    }
    if (!found) {
        return Fail(response, 404, "举报不存在");
    }
    if (row.status == "handled") {
        return Fail(response, 409, "举报已处理");
    }
    // step3: 需要时软删目标（用户类型没有 deleted_at，只能忽略）。
    bool deleted = false;
    bool target_missing = false;
    if (action == "delete_target") {
        if (row.target_type == "user") {
            return Fail(response, 400,
                        "用户类型不支持删除内容（请选择忽略）");
        }
        if (!ApplyDeleteTarget(store.Database(), row.target_type, row.target_id,
                               NowSeconds(), &deleted, &target_missing,
                               &error)) {
            std::fprintf(stderr, "[%s] %s\n", kServiceName, error.c_str());
            return Fail(response, 500, "删除目标失败");
        }
    }
    // step4: 置 handled；changes=0 是并发被抢先，按 409 返回。
    auto marked = store.Database()->Exec(
        "UPDATE reports SET status = 'handled'"
        " WHERE id = ? AND status = 'pending'",
        report_id);
    if (!marked) {
        std::fprintf(stderr, "[%s] 更新举报状态失败: %s\n", kServiceName,
                     marked.Err().ToString().c_str());
        return Fail(response, 500, "数据库错误");
    }
    if (marked.Value().changes == 0) {
        return Fail(response, 409, "举报已被其他管理员处理");
    }
    response->status = 200;
    response->body =
        std::string("{\"ok\":true,\"id\":") + std::to_string(report_id) +
        ",\"action\":\"" + action + "\",\"status\":\"handled\"" +
        ",\"deleted\":" + (deleted ? "true" : "false") +
        ",\"target_missing\":" + (target_missing ? "true" : "false") + "}";
    return true;
}

/**
 * @brief `GET /api/admin/stats`：待处理 / 今日新增 / 已处理总数。
 *
 * @param store [in,out] 已打开的存储；借用。
 * @param request [in] 原始请求；必须带管理员 Bearer token。
 * @param response [out] 成功写 `{ok,pending,today,handled}`；失败写统一错误体。
 * @return 恒为 true（已处理）。
 *
 * ## 定位
 * 后台顶部三张数字卡的数据源。错了会怎样：运营判断「今天有没有积压」失真。
 *
 * ## 步骤
 * step1. 鉴权。
 * step2. 一条 SQL 里做三个 COUNT（同一快照，三个数互相自洽）。
 *
 * ## 失败语义
 * 数据库错误回 500；不返回部分统计。
 */
bool HandleStats(douyin::Store& store, const HttpRequest& request,
                 HttpResponse* response) {
    // step1: 权限。
    std::int64_t actor = 0;
    if (!RequireAdmin(request, response, &actor)) {
        return true;
    }
    (void)actor;
    // step2: 三个 COUNT 同一快照。
    auto result = store.Database()->Query(
        "SELECT"
        " (SELECT COUNT(*) FROM reports WHERE status = 'pending'),"
        " (SELECT COUNT(*) FROM reports WHERE created_at >= ?),"
        " (SELECT COUNT(*) FROM reports WHERE status = 'handled')",
        TodayStartSeconds());
    if (!result) {
        std::fprintf(stderr, "[%s] 统计查询失败: %s\n", kServiceName,
                     result.Err().ToString().c_str());
        return Fail(response, 500, "数据库错误");
    }
    Rows& rows = result.Value();
    std::int64_t pending = 0;
    std::int64_t today = 0;
    std::int64_t handled = 0;
    if (rows.Next()) {
        pending = rows.Int64(0);
        today = rows.Int64(1);
        handled = rows.Int64(2);
    }
    if (rows.Err()) {
        std::fprintf(stderr, "[%s] 统计读取失败: %s\n", kServiceName,
                     rows.Err().ToString().c_str());
        return Fail(response, 500, "数据库错误");
    }
    response->status = 200;
    response->body = std::string("{\"ok\":true,\"pending\":") +
                     std::to_string(pending) +
                     ",\"today\":" + std::to_string(today) +
                     ",\"handled\":" + std::to_string(handled) + "}";
    return true;
}

/**
 * @brief 业务路由分发：命中返回 true 并填响应，未命中返回 false（服务器 404）。
 *
 * @param store [in,out] 已打开的存储；借用。
 * @param request [in] 已解析请求。
 * @param response [out] 待发送响应。
 * @return 恒为 true（本服务没有让服务器兜底 404 的路径，未命中显式回 404）。
 */
bool Handle(douyin::Store& store, const HttpRequest& request,
            HttpResponse* response) {
    // 请求边界清一次 ORM 语句缓存，避免读游标把连接钉在旧 WAL 快照上。
    store.ResetStatements();
    ++g_metrics.requests;

    if (request.method == "GET" && request.path == "/health") {
        response->status = 200;
        response->body = std::string("{\"ok\":true,\"service\":\"") +
                         kServiceName + "\"}";
        return true;
    }
    if (request.method == "GET" && request.path == "/metrics") {
        response->status = 200;
        response->body =
            std::string("{\"ok\":true,\"service\":\"") + kServiceName +
            "\",\"requests\":" + std::to_string(g_metrics.requests) +
            ",\"errors\":" + std::to_string(g_metrics.errors) + "}";
        return true;
    }
    // 运营页：静态 HTML，不校验 admin（数据接口才校验）。
    if ((request.method == "GET" || request.method == "HEAD") &&
        (request.path == "/admin" || request.path == "/admin/" ||
         request.path == "/admin.html")) {
        response->status = 200;
        response->content_type = "text/html; charset=utf-8";
        response->headers["Cache-Control"] = "no-cache";
        response->body = douyin::kAdminPageHtml;
        return true;
    }
    if (request.path == "/api/admin/reports") {
        if (request.method == "GET") {
            return HandleListReports(store, request, response);
        }
        return Fail(response, 405, "方法不允许");
    }
    if (request.path == "/api/admin/stats") {
        if (request.method == "GET") {
            return HandleStats(store, request, response);
        }
        return Fail(response, 405, "方法不允许");
    }
    std::int64_t report_id = 0;
    if (ParseHandlePath(request.path, &report_id)) {
        if (request.method == "POST") {
            return HandleReportAction(store, request, report_id, response);
        }
        return Fail(response, 405, "方法不允许");
    }
    return Fail(response, 404, "未知路径");
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
    // 就绪探针：reports 表可读才算 ready；只读，不改任何数据。
    server.SetReadinessProbe([&store](std::string* detail) {
        if (!store.Ok() || store.Database() == nullptr) {
            if (detail != nullptr) {
                *detail = "db not open";
            }
            return false;
        }
        auto rows = store.Database()->Query("SELECT 1 FROM reports LIMIT 1");
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
    std::fprintf(stderr, "[%s] admin_user_ids=%zu\n", kServiceName,
                 AdminUserIds().size());
    std::printf("[%s] 监听 0.0.0.0:%u db=%s\n", kServiceName,
                static_cast<unsigned>(port), db_path.c_str());
    std::fflush(stdout);

    server.ServeForever([&store](const HttpRequest& request,
                                 HttpResponse* response) {
        return Handle(store, request, response);
    });
    return 0;
}
