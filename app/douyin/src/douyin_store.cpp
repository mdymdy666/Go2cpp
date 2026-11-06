/**
 * @file douyin_store.cpp
 * @brief SQLite 存储实现：users 走 ORM，follows 走参数化 SQL。
 *
 * @author douyin demo
 * @date 2026-11-03
 * @copyright MIT License
 */

#include "douyin_store.hpp"

#include "douyin_tag.hpp"
#include "go2cpp/storage/db.hpp"
#include "go2cpp/storage/orm.hpp"

#include <algorithm>
#include <unordered_map>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace douyin {

/// @brief users 表的 ORM 映射结构（列名与 REQUIREMENTS.md §4 一致）。
struct UserRow {
    std::int64_t id{0};
    std::string name;
    std::string email;
    std::string password_hash;
    std::string salt;
    std::int64_t created_at{0};
};

/// @brief 判断文本是否为非空纯数字（游标校验用）。
bool IsAllDigits(std::string_view text) {
    if (text.empty()) {
        return false;
    }
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
    }
    return true;
}

std::string EncodeVideoCursor(const StoredVideo& video) {
    if (video.id <= 0) {
        return std::string();
    }
    return std::to_string(video.created_at) + "-" + std::to_string(video.id);
}

bool DecodeVideoCursor(const std::string& cursor, std::int64_t* created_at,
                       std::int64_t* id) {
    const std::size_t dash = cursor.rfind('-');
    if (dash == std::string::npos) {
        return false;
    }
    const std::string_view created_text(cursor.data(), dash);
    const std::string_view id_text(cursor.data() + dash + 1,
                                   cursor.size() - dash - 1);
    if (!IsAllDigits(created_text) || !IsAllDigits(id_text)) {
        return false;
    }
    const std::int64_t created_value = std::atoll(std::string(created_text).c_str());
    const std::int64_t id_value = std::atoll(std::string(id_text).c_str());
    if (created_value <= 0 || id_value <= 0) {
        return false;
    }
    if (created_at != nullptr) {
        *created_at = created_value;
    }
    if (id != nullptr) {
        *id = id_value;
    }
    return true;
}

/**
 * @brief 生成推荐流随机种子：random_device 与纳秒时钟混合。
 *
 * @note 只用 random_device 时，某些平台（含部分 libstdc++ 构建）会退化成
 *       确定性序列，导致每次刷新顺序相同；混入纳秒时间可避免这一退化。
 * @return 非零种子（0 也可用，只是洗牌结果固定，不影响正确性）。
 */
std::uint64_t RandomFeedSeed() {
    std::random_device device;
    const auto ticks = std::chrono::high_resolution_clock::now().time_since_epoch();
    const std::uint64_t nano = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(ticks).count());
    const std::uint64_t rnd = (static_cast<std::uint64_t>(device()) << 32) ^
                              static_cast<std::uint64_t>(device());
    return rnd ^ (nano + 0x9e3779b97f4a7c15ULL + (rnd << 6) + (rnd >> 2));
}

std::string EncodeFeedCursor(std::uint64_t seed, std::size_t index) {
    constexpr char kHex[] = "0123456789abcdef";
    std::string text;
    text.reserve(18);
    do {
        text.push_back(kHex[seed & 0xFU]);
        seed >>= 4;
    } while (seed != 0);
    std::reverse(text.begin(), text.end());
    return "r" + text + "_" + std::to_string(index);
}

bool DecodeFeedCursor(const std::string& cursor, std::uint64_t* seed,
                      std::size_t* index) {
    if (seed == nullptr || index == nullptr) {
        return false;
    }
    if (cursor.size() < 4 || cursor.front() != 'r') {
        return false;
    }
    const std::size_t underscore = cursor.find('_');
    if (underscore == std::string::npos || underscore <= 1 ||
        underscore + 2 > cursor.size()) {
        return false;
    }
    std::uint64_t seed_value = 0;
    for (std::size_t i = 1; i < underscore; ++i) {
        const char c = cursor[i];
        std::uint64_t digit = 0;
        if (c >= '0' && c <= '9') {
            digit = static_cast<std::uint64_t>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            digit = static_cast<std::uint64_t>(c - 'a') + 10U;
        } else {
            return false;
        }
        if (seed_value >> 60 != 0) {
            return false;
        }
        seed_value = (seed_value << 4) | digit;
    }
    std::uint64_t index_value = 0;
    for (std::size_t i = underscore + 1; i < cursor.size(); ++i) {
        const char c = cursor[i];
        if (c < '0' || c > '9') {
            return false;
        }
        index_value = index_value * 10U + static_cast<std::uint64_t>(c - '0');
        if (index_value > (1ULL << 32)) {
            return false;
        }
    }
    *seed = seed_value;
    *index = static_cast<std::size_t>(index_value);
    return true;
}

std::string EncodeCommentCursor(const StoredComment& comment) {
    if (comment.id <= 0) {
        return std::string();
    }
    return std::to_string(comment.created_at) + "-" + std::to_string(comment.id);
}

bool DecodeCommentCursor(const std::string& cursor, std::int64_t* created_at,
                         std::int64_t* id) {
    return DecodeVideoCursor(cursor, created_at, id);
}

/**
 * @brief 编码 hot 排序游标：`h<like_count>-<created_at>-<id>`。
 * @param comment [in] 游标指向的评论（通常是本页最后一条，其 like_count
 *        必须已由 hot 查询回填）。
 * @return 形如 `h7-1762260000-42` 的串；`id<=0` 时返回空串。
 * @note 与 new 游标（`<created_at>-<id>`）格式不同，主动加 `h` 前缀，
 *       两种 sort 的游标串用会解码失败回 400，而不是静默跳页。
 */
std::string EncodeHotCommentCursor(const StoredComment& comment) {
    if (comment.id <= 0) {
        return std::string();
    }
    return "h" + std::to_string(comment.like_count) + "-" +
           std::to_string(comment.created_at) + "-" +
           std::to_string(comment.id);
}

/**
 * @brief 解码 hot 排序游标。
 * @param cursor [in] 非空游标串，必须匹配 `h<非负整数>-<正整数>-<正整数>`。
 * @param like_count [out] 成功时写点赞数（可为 0）；失败保持原值。
 * @param created_at [out] 成功时写 created_at；失败保持原值。
 * @param id [out] 成功时写 id；失败保持原值。
 * @return 格式合法返回 true；否则 false（调用方回 400）。
 */
bool DecodeHotCommentCursor(const std::string& cursor,
                            std::int64_t* like_count,
                            std::int64_t* created_at, std::int64_t* id) {
    if (cursor.size() < 5 || cursor[0] != 'h') {
        return false;
    }
    const std::size_t first = cursor.find('-', 1);
    if (first == std::string::npos) {
        return false;
    }
    const std::size_t second = cursor.find('-', first + 1);
    if (second == std::string::npos) {
        return false;
    }
    const std::string_view like_text(cursor.data() + 1, first - 1);
    const std::string_view created_text(cursor.data() + first + 1,
                                        second - first - 1);
    const std::string_view id_text(cursor.data() + second + 1,
                                   cursor.size() - second - 1);
    if (!IsAllDigits(like_text) || !IsAllDigits(created_text) ||
        !IsAllDigits(id_text)) {
        return false;
    }
    const std::int64_t like_value =
        std::atoll(std::string(like_text).c_str());
    const std::int64_t created_value =
        std::atoll(std::string(created_text).c_str());
    const std::int64_t id_value = std::atoll(std::string(id_text).c_str());
    if (like_value < 0 || created_value <= 0 || id_value <= 0) {
        return false;
    }
    if (like_count != nullptr) {
        *like_count = like_value;
    }
    if (created_at != nullptr) {
        *created_at = created_value;
    }
    if (id != nullptr) {
        *id = id_value;
    }
    return true;
}

std::string EncodeNotificationCursor(const StoredNotification& notification) {
    if (notification.id <= 0) {
        return std::string();
    }
    return std::to_string(notification.created_at) + "-" +
           std::to_string(notification.id);
}

bool DecodeNotificationCursor(const std::string& cursor,
                              std::int64_t* created_at, std::int64_t* id) {
    return DecodeVideoCursor(cursor, created_at, id);
}

namespace {

/// @brief 建表 + 索引脚本（幂等）。
constexpr const char* kSchemaSql =
    "CREATE TABLE IF NOT EXISTS users ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  name TEXT NOT NULL,"
    "  email TEXT NOT NULL UNIQUE,"
    "  password_hash TEXT NOT NULL,"
    "  salt TEXT NOT NULL,"
    "  created_at INTEGER NOT NULL,"
    // 演示用金币余额：注册即送 1000，送礼当场扣减（事务见 SendGift）。
    "  coins INTEGER NOT NULL DEFAULT 1000"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_users_email ON users(email);"
    // @mention 按昵称精确查人：没有索引就是每条评论一次全表扫。
    "CREATE INDEX IF NOT EXISTS idx_users_name ON users(name);"
    "CREATE TABLE IF NOT EXISTS follows ("
    "  follower_id INTEGER NOT NULL,"
    "  followee_id INTEGER NOT NULL,"
    "  created_at INTEGER NOT NULL,"
    "  PRIMARY KEY (follower_id, followee_id)"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_follows_followee ON follows(followee_id);"
    "CREATE TABLE IF NOT EXISTS videos ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  user_id INTEGER NOT NULL,"
    "  title TEXT NOT NULL,"
    "  url TEXT NOT NULL,"
    "  cover_url TEXT NOT NULL DEFAULT '',"
    "  clip_start_ms INTEGER NOT NULL DEFAULT 0,"
    "  clip_end_ms INTEGER NOT NULL DEFAULT 0,"
    "  like_count INTEGER NOT NULL DEFAULT 0,"
    "  comment_count INTEGER NOT NULL DEFAULT 0,"
    "  created_at INTEGER NOT NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_videos_user ON videos(user_id, created_at DESC);"
    "CREATE INDEX IF NOT EXISTS idx_videos_created ON videos(created_at DESC);"
    "CREATE TABLE IF NOT EXISTS likes ("
    "  video_id INTEGER NOT NULL,"
    "  user_id INTEGER NOT NULL,"
    "  created_at INTEGER NOT NULL,"
    "  PRIMARY KEY (video_id, user_id)"
    ");"
    // 主页「喜欢」tab 按用户查、按点赞时间倒序：主键前缀是 video_id，帮不上忙，
    // 必须补一条 (user_id, created_at) 索引，否则每页都全表扫 likes。
    "CREATE INDEX IF NOT EXISTS idx_likes_user "
    "ON likes(user_id, created_at DESC);"
    "CREATE TABLE IF NOT EXISTS comments ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  video_id INTEGER NOT NULL,"
    "  user_id INTEGER NOT NULL,"
    "  content TEXT NOT NULL,"
    "  created_at INTEGER NOT NULL,"
    "  parent_id INTEGER NOT NULL DEFAULT 0,"
    "  pinned_at INTEGER NOT NULL DEFAULT 0"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_comments_video "
    "ON comments(video_id, created_at DESC);"
    "CREATE TABLE IF NOT EXISTS rooms ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  user_id INTEGER NOT NULL,"
    "  title TEXT NOT NULL,"
    "  status INTEGER NOT NULL DEFAULT 0,"
    "  viewer_count INTEGER NOT NULL DEFAULT 0,"
    "  created_at INTEGER NOT NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_rooms_status ON rooms(status, created_at DESC);"
    "CREATE TABLE IF NOT EXISTS room_viewers ("
    "  room_id INTEGER NOT NULL,"
    "  user_id INTEGER NOT NULL,"
    "  created_at INTEGER NOT NULL,"
    "  PRIMARY KEY (room_id, user_id)"
    ");"
    "CREATE TABLE IF NOT EXISTS upload_sessions ("
    "  upload_id TEXT PRIMARY KEY,"
    "  user_id INTEGER NOT NULL,"
    "  path TEXT NOT NULL,"
    "  title TEXT NOT NULL DEFAULT '',"
    "  total INTEGER NOT NULL,"
    "  received_bytes INTEGER NOT NULL DEFAULT 0,"
    "  created_at INTEGER NOT NULL"
    ");"
    "CREATE TABLE IF NOT EXISTS favorites ("
    "  user_id INTEGER NOT NULL,"
    "  video_id INTEGER NOT NULL,"
    "  created_at INTEGER NOT NULL,"
    "  UNIQUE(user_id, video_id)"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_favorites_user "
    "ON favorites(user_id, created_at DESC);"
    "CREATE TABLE IF NOT EXISTS comment_likes ("
    "  comment_id INTEGER NOT NULL,"
    "  user_id INTEGER NOT NULL,"
    "  created_at INTEGER NOT NULL,"
    "  PRIMARY KEY (comment_id, user_id)"
    ");"
    // 消息通知：read_at=0 表示未读。两个索引各司其职：
    //   unread   —— 未读计数与「只看未读」，前缀 (user_id, read_at) 精确命中；
    //   timeline —— 通知列表按时间倒序，直接走索引顺序，避免临时排序。
    "CREATE TABLE IF NOT EXISTS notifications ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  user_id INTEGER NOT NULL,"
    "  actor_id INTEGER NOT NULL,"
    "  kind TEXT NOT NULL,"
    "  target_type TEXT NOT NULL,"
    "  target_id INTEGER NOT NULL,"
    "  content TEXT NOT NULL DEFAULT '',"
    "  created_at INTEGER NOT NULL,"
    "  read_at INTEGER NOT NULL DEFAULT 0"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_notifications_unread "
    "ON notifications(user_id, read_at, created_at);"
    "CREATE INDEX IF NOT EXISTS idx_notifications_timeline "
    "ON notifications(user_id, created_at DESC, id DESC);"
    // 分类计数：`GROUP BY kind` 直接按索引顺序分组，不建临时 B 树。
    "CREATE INDEX IF NOT EXISTS idx_notifications_kind "
    "ON notifications(user_id, read_at, kind);"
    // 举报：唯一键挡住「同一个人对同一目标反复举报」；status 留给运营后台，
    // 用户接口不暴露举报列表。
    "CREATE TABLE IF NOT EXISTS reports ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  reporter_id INTEGER NOT NULL,"
    "  target_type TEXT NOT NULL,"
    "  target_id INTEGER NOT NULL,"
    "  reason TEXT NOT NULL,"
    "  detail TEXT NOT NULL DEFAULT '',"
    "  status TEXT NOT NULL DEFAULT 'pending',"
    "  created_at INTEGER NOT NULL,"
    "  UNIQUE(reporter_id, target_type, target_id)"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_reports_status "
    "ON reports(status, created_at DESC);"
    // 拉黑：唯一键 (user_id, blocked_id) 同时是幂等键；索引前缀 user_id
    // 让「取我的黑名单」和 NOT IN 子查询都只扫本人这几行。
    "CREATE TABLE IF NOT EXISTS blocks ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  user_id INTEGER NOT NULL,"
    "  blocked_id INTEGER NOT NULL,"
    "  created_at INTEGER NOT NULL,"
    "  UNIQUE(user_id, blocked_id)"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_blocks_user "
    "ON blocks(user_id, created_at DESC);"
    "CREATE TABLE IF NOT EXISTS live_messages ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  room_id INTEGER NOT NULL,"
    "  user_id INTEGER NOT NULL,"
    "  content TEXT NOT NULL,"
    "  kind TEXT NOT NULL DEFAULT 'chat',"
    "  created_at INTEGER NOT NULL"
    ");"
    // 增量拉取与首屏都按 (room_id, id DESC) 取：一条索引同时服务
    // `id > after` 的区间扫描与 `ORDER BY id ASC LIMIT n`。
    "CREATE INDEX IF NOT EXISTS idx_live_messages_room "
    "ON live_messages(room_id, id DESC);"
    "CREATE TABLE IF NOT EXISTS gifts ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  name TEXT NOT NULL,"
    "  price_coins INTEGER NOT NULL,"
    "  icon TEXT NOT NULL"
    ");"
    "CREATE TABLE IF NOT EXISTS gift_records ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  room_id INTEGER NOT NULL,"
    "  sender_id INTEGER NOT NULL,"
    "  gift_id INTEGER NOT NULL,"
    "  count INTEGER NOT NULL,"
    "  created_at INTEGER NOT NULL"
    ");"
    // 贡献榜按房间 GROUP BY sender_id：索引前缀 room_id 把扫描限制在本房间，
    // 避免全表聚合。
    "CREATE INDEX IF NOT EXISTS idx_gift_records_room "
    "ON gift_records(room_id, sender_id);"
    // 搜索历史（第 11 轮）：UNIQUE(user_id, keyword) 是「同一用户同一词只留
    // 最近一次」的幂等键，写入走 ON CONFLICT DO UPDATE；两条索引分别服务
    // 「我的历史倒序」与「最近 7 天热搜聚合」。
    "CREATE TABLE IF NOT EXISTS search_history ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  user_id INTEGER NOT NULL,"
    "  keyword TEXT NOT NULL,"
    "  created_at INTEGER NOT NULL,"
    "  UNIQUE(user_id, keyword)"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_search_history_user "
    "ON search_history(user_id, created_at DESC);"
    "CREATE INDEX IF NOT EXISTS idx_search_history_created "
    "ON search_history(created_at DESC);";

/// @brief 把 `Error` 写成可读文本。
std::string DescribeError(const go2cpp::storage::Error& error) {
    return error.ToString();
}

/// @brief 把 `UserRow` 拷贝成对外的 `StoredUser`。
void CopyRow(const UserRow& row, StoredUser* out) {
    out->id = row.id;
    out->name = row.name;
    out->email = row.email;
    out->password_hash = row.password_hash;
    out->salt = row.salt;
    out->created_at = row.created_at;
}

/// @brief videos 查询的固定列顺序（与 `ReadVideoRow` 的列下标一一对应）。
constexpr const char* kVideoColumns =
    "id, user_id, title, url, like_count, comment_count, created_at, "
    "cover_url, clip_start_ms, clip_end_ms";

/// @brief 热度排序子句：与需求公式只差全局常数，排序等价且与 now 无关。
constexpr const char* kVideoOrder =
    " ORDER BY (like_count * 2 + comment_count * 3 + 0.1 * created_at / 3600.0)"
    " DESC, id DESC";

/// @brief 读当前行（列顺序必须为 `kVideoColumns`）。
void ReadVideoRow(go2cpp::storage::Rows* rows, StoredVideo* out) {
    out->id = rows->Int64(0);
    out->user_id = rows->Int64(1);
    out->title = rows->Text(2);
    out->url = rows->Text(3);
    out->like_count = rows->Int64(4);
    out->comment_count = rows->Int64(5);
    out->created_at = rows->Int64(6);
    out->cover_url = rows->Text(7);
    out->clip_start_ms = rows->Int64(8);
    out->clip_end_ms = rows->Int64(9);
}

/// @brief comments 查询的固定列顺序（与 `ReadCommentRow` 的下标对应）。
constexpr const char* kCommentColumns =
    "id, video_id, user_id, content, created_at, parent_id, pinned_at";

/// @brief 评论点赞数相关子查询（hot 排序与游标比较共用同一表达式）。
/// @note 评论点赞数不落 comments 列（第 1 轮设计），只能按主键回查
///       `comment_likes`；每页最多 100 条评论，COUNT 走主键前缀，代价可控。
constexpr const char* kCommentLikeExpr =
    "(SELECT COUNT(*) FROM comment_likes cl WHERE cl.comment_id = c.id)";

/// @brief hot 排序的固定列顺序：前 7 列同 `kCommentColumns`，第 8 列为点赞数。
constexpr const char* kHotCommentColumns =
    "c.id, c.video_id, c.user_id, c.content, c.created_at, c.parent_id, "
    "c.pinned_at, "
    "(SELECT COUNT(*) FROM comment_likes cl WHERE cl.comment_id = c.id)";

/// @brief 读当前行（列顺序必须为 `kCommentColumns`）。
void ReadCommentRow(go2cpp::storage::Rows* rows, StoredComment* out) {
    out->id = rows->Int64(0);
    out->video_id = rows->Int64(1);
    out->user_id = rows->Int64(2);
    out->content = rows->Text(3);
    out->created_at = rows->Int64(4);
    out->parent_id = rows->Int64(5);
    out->pinned_at = rows->Int64(6);
}

/**
 * @brief 读一页评论：多取一条判断 next_cursor，再裁剪到 limit。
 * @param rows [in,out] 已执行成功的查询结果集，借用。
 * @param limit [in] 期望条数；<=0 时直接返回 true。
 * @param next_cursor [out] 还有下一页时写游标，否则空串。
 * @param out [out] 追加到末尾。
 * @param error [out] 遍历失败时写错误文本。
 * @return 成功返回 true。
 */
bool ReadCommentPage(go2cpp::storage::Rows* rows, std::int64_t limit,
                     std::string* next_cursor,
                     std::vector<StoredComment>* out, std::string* error) {
    std::vector<StoredComment> page;
    while (rows->Next()) {
        StoredComment comment;
        ReadCommentRow(rows, &comment);
        page.push_back(std::move(comment));
    }
    if (rows->Err()) {
        if (error != nullptr) {
            *error = "遍历评论失败: " + DescribeError(rows->Err());
        }
        return false;
    }
    if (static_cast<std::int64_t>(page.size()) > limit) {
        page.resize(static_cast<std::size_t>(limit));
        if (!page.empty()) {
            *next_cursor = EncodeCommentCursor(page.back());
        }
    }
    for (StoredComment& comment : page) {
        out->push_back(std::move(comment));
    }
    return true;
}

/**
 * @brief 读一页 hot 排序评论：多取一条判断 next_cursor，再裁剪到 limit。
 * @param rows [in,out] 已执行成功的查询结果集（列顺序为 kHotCommentColumns：
 *        前 7 列同 kCommentColumns，第 8 列为点赞数）。
 * @param limit [in] 期望条数；<=0 时直接返回 true。
 * @param next_cursor [out] 还有下一页时写 hot 游标，否则空串。
 * @param out [out] 追加到末尾；每条评论的 like_count 已按计数回填。
 * @param error [out] 遍历失败时写错误文本。
 * @return 成功返回 true。
 */
bool ReadHotCommentPage(go2cpp::storage::Rows* rows, std::int64_t limit,
                        std::string* next_cursor,
                        std::vector<StoredComment>* out,
                        std::string* error) {
    std::vector<StoredComment> page;
    while (rows->Next()) {
        StoredComment comment;
        ReadCommentRow(rows, &comment);
        comment.like_count = rows->Int64(7);
        page.push_back(std::move(comment));
    }
    if (rows->Err()) {
        if (error != nullptr) {
            *error = "遍历评论失败: " + DescribeError(rows->Err());
        }
        return false;
    }
    if (static_cast<std::int64_t>(page.size()) > limit) {
        page.resize(static_cast<std::size_t>(limit));
        if (!page.empty()) {
            *next_cursor = EncodeHotCommentCursor(page.back());
        }
    }
    for (StoredComment& comment : page) {
        out->push_back(std::move(comment));
    }
    return true;
}

/**
 * @brief 把搜索词包成 LIKE 模式 `%q%`，转义 `%` `_` `\`。
 * @note SQL 侧用 `ESCAPE '\'` 声明转义符。
 */
std::string LikePattern(const std::string& query) {
    std::string escaped;
    escaped.reserve(query.size() + 2);
    for (const char c : query) {
        if (c == '\\' || c == '%' || c == '_') {
            escaped += '\\';
        }
        escaped += c;
    }
    return "%" + escaped + "%";
}

/// @brief 前缀匹配模式：转义 `%`/`_`/`\` 后追加 `%`（如 `美` → `美%`）。
std::string LikePrefixPattern(const std::string& prefix) {
    std::string escaped;
    escaped.reserve(prefix.size() + 1);
    for (const char c : prefix) {
        if (c == '\\' || c == '%' || c == '_') {
            escaped += '\\';
        }
        escaped += c;
    }
    escaped += '%';
    return escaped;
}

/// @brief 执行查询并把全部视频按已排好的顺序装进 `all`。
template <typename... Args>
bool LoadVideos(go2cpp::storage::DB& db, const std::string& sql,
                std::vector<StoredVideo>* all, std::string* error,
                const Args&... args) {
    auto rows = db.Query(sql, args...);
    if (!rows) {
        if (error != nullptr) {
            *error = "查询视频失败: " + DescribeError(rows.Err());
        }
        return false;
    }
    while (rows.Value().Next()) {
        StoredVideo video;
        ReadVideoRow(&rows.Value(), &video);
        all->push_back(std::move(video));
    }
    if (rows.Value().Err()) {
        if (error != nullptr) {
            *error = "遍历视频失败: " + DescribeError(rows.Value().Err());
        }
        return false;
    }
    return true;
}

/**
 * @brief 在全序结果里按游标切出一页。
 * @details 游标指向上一页最后一条；这里按 id 精确定位（created_at 一并校验）。
 *          找不到（该视频已删除）时输出空页与空 `next_cursor`。
 */
void PageVideos(const std::vector<StoredVideo>& all, const std::string& cursor,
                std::int64_t limit, std::vector<StoredVideo>* out,
                std::string* next_cursor) {
    next_cursor->clear();
    if (limit <= 0) {
        return;
    }
    std::size_t start = 0;
    if (!cursor.empty()) {
        std::int64_t cursor_created = 0;
        std::int64_t cursor_id = 0;
        if (!DecodeVideoCursor(cursor, &cursor_created, &cursor_id)) {
            return;
        }
        start = all.size();
        for (std::size_t i = 0; i < all.size(); ++i) {
            if (all[i].id == cursor_id && all[i].created_at == cursor_created) {
                start = i + 1;
                break;
            }
        }
        if (start >= all.size()) {
            return;
        }
    }
    const std::size_t remaining = all.size() - start;
    const std::size_t take = remaining < static_cast<std::size_t>(limit)
                                 ? remaining
                                 : static_cast<std::size_t>(limit);
    for (std::size_t i = 0; i < take; ++i) {
        out->push_back(all[start + i]);
    }
    if (take == static_cast<std::size_t>(limit) && start + take < all.size()) {
        *next_cursor = EncodeVideoCursor(out->back());
    }
}

}  // namespace
}  // namespace douyin

GO2CPP_ORM_TABLE(douyin::UserRow, "users",
                 GO2CPP_ORM_COLUMN(douyin::UserRow, id).PrimaryKey().AutoIncrement(),
                 GO2CPP_ORM_COLUMN(douyin::UserRow, name).NotNull(),
                 GO2CPP_ORM_COLUMN(douyin::UserRow, email).NotNull().Unique(),
                 GO2CPP_ORM_COLUMN(douyin::UserRow, password_hash).NotNull(),
                 GO2CPP_ORM_COLUMN(douyin::UserRow, salt).NotNull(),
                 GO2CPP_ORM_COLUMN(douyin::UserRow, created_at).NotNull())

namespace douyin {

Store::Store() = default;
Store::~Store() = default;

bool Store::Open(const std::string& path, std::string* error) {
    auto opened = go2cpp::storage::DB::Open(path);
    if (!opened) {
        if (error != nullptr) {
            *error = "打开数据库失败: " + DescribeError(opened.Err());
        }
        return false;
    }
    m_db = std::move(opened).Value();
    if (auto busy = m_db->SetBusyTimeout(5000); busy) {
        if (error != nullptr) {
            *error = "设置 busy_timeout 失败: " + DescribeError(busy);
        }
        return false;
    }
    // 多进程共用同一 SQLite 文件（六个服务都开同一个 db）时，另一个进程
    // 可能正在初始化/持锁；PRAGMA journal_mode=WAL 不经过 busy handler，
    // 会直接返回 SQLITE_BUSY。这里有限重试（最多 2 s），避免启动竞态把
    // 整个服务打挂（实测 user+live 同时启动必现）。
    bool wal_ready = false;
    for (int attempt = 0; attempt < 40; ++attempt) {
        auto wal = m_db->SetJournalMode("WAL");
        if (!wal) {
            wal_ready = true;
            break;
        }
        if (attempt == 39) {
            if (error != nullptr) {
                *error = "设置 WAL 失败: " + DescribeError(wal);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (!wal_ready) {
        return false;
    }
    m_session = std::make_unique<go2cpp::storage::Session>(*m_db);
    return true;
}

bool Store::EnsureSchema(std::string* error) {
    if (!Ok()) {
        if (error != nullptr) {
            *error = "数据库尚未打开";
        }
        return false;
    }
    auto executed = m_db->Exec(kSchemaSql);
    if (!executed) {
        if (error != nullptr) {
            *error = "建表失败: " + DescribeError(executed.Err());
        }
        return false;
    }
    // step2: 旧库迁移——CREATE TABLE IF NOT EXISTS 不会给已有表加列，
    //        所以先补 parent_id；列已存在时 ALTER 报 duplicate column，属预期。
    (void)m_db->Exec(
        "ALTER TABLE comments ADD COLUMN parent_id INTEGER NOT NULL DEFAULT 0");
    // step3: 第 6 轮评论置顶：旧库补 pinned_at（同上，重复 ALTER 报
    //        duplicate column，属预期）。
    (void)m_db->Exec(
        "ALTER TABLE comments ADD COLUMN pinned_at INTEGER NOT NULL DEFAULT 0");
    // step5: 第 7 轮发布链路——videos 补封面与裁剪区间。旧库重复 ALTER 报
    //        duplicate column，属预期；新库由建表脚本直接带列。
    (void)m_db->Exec(
        "ALTER TABLE videos ADD COLUMN cover_url TEXT NOT NULL DEFAULT ''");
    (void)m_db->Exec(
        "ALTER TABLE videos ADD COLUMN clip_start_ms INTEGER NOT NULL DEFAULT 0");
    (void)m_db->Exec(
        "ALTER TABLE videos ADD COLUMN clip_end_ms INTEGER NOT NULL DEFAULT 0");
    // step4: 列补齐后再建索引（放在建表脚本里会在旧库上报 no such column）。
    (void)m_db->Exec(
        "CREATE INDEX IF NOT EXISTS idx_comments_parent "
        "ON comments(video_id, parent_id, created_at)");
    // 置顶评论只占极少数行：按 (video_id, pinned_at DESC) 直接取，
    // 不参与主列表的 created_at 扫描。
    (void)m_db->Exec(
        "CREATE INDEX IF NOT EXISTS idx_comments_pinned "
        "ON comments(video_id, pinned_at DESC, id DESC)");
    // step6: 第 10 轮直播互动——users 补 coins（旧库默认补 1000；列已存在时
    //        ALTER 报 duplicate column，属预期）。
    (void)m_db->Exec(
        "ALTER TABLE users ADD COLUMN coins INTEGER NOT NULL DEFAULT 1000");
    // 礼物价目表是静态字典：固定 id + INSERT OR IGNORE，重复启动不产生副本，
    // 也不覆盖运营后来改过的价格。
    (void)m_db->Exec(
        "INSERT OR IGNORE INTO gifts(id, name, price_coins, icon) VALUES"
        "(1, '小心心', 1, '\u2764\ufe0f'),"
        "(2, '棒棒糖', 10, '\U0001F36D'),"
        "(3, '鲜花', 30, '\U0001F339'),"
        "(4, '火箭', 100, '\U0001F680'),"
        "(5, '城堡', 500, '\U0001F3F0')");
    return true;
}

bool Store::Ok() const noexcept {
    return m_db != nullptr && m_session != nullptr && m_db->IsOpen();
}

CreateUserResult Store::CreateUser(const std::string& name, const std::string& email,
                                   const std::string& password_hash,
                                   const std::string& salt, StoredUser* out,
                                   std::string* error) {
    UserRow row;
    row.name = name;
    row.email = email;
    row.password_hash = password_hash;
    row.salt = salt;
    row.created_at = static_cast<std::int64_t>(std::time(nullptr));

    auto inserted = m_session->Insert(row);
    if (!inserted) {
        if (inserted.Err().IsConstraint()) {
            return CreateUserResult::kConflict;
        }
        if (error != nullptr) {
            *error = "插入用户失败: " + DescribeError(inserted.Err());
        }
        return CreateUserResult::kError;
    }
    if (out != nullptr) {
        CopyRow(row, out);
    }
    return CreateUserResult::kOk;
}

bool Store::FindUserByEmail(const std::string& email, StoredUser* out,
                            std::string* error) {
    if (!Ok()) {
        if (error != nullptr) {
            *error = "数据库尚未打开";
        }
        return false;
    }
    auto found = m_session->Query<UserRow>().Where("email = ?", email).First();
    if (!found) {
        if (found.Err().Code() == go2cpp::storage::kErrNotFound) {
            return false;
        }
        if (error != nullptr) {
            *error = "查询用户失败: " + DescribeError(found.Err());
        }
        return false;
    }
    CopyRow(found.Value(), out);
    return true;
}

bool Store::FindUserById(std::int64_t id, StoredUser* out, std::string* error) {
    if (!Ok()) {
        if (error != nullptr) {
            *error = "数据库尚未打开";
        }
        return false;
    }
    auto found = m_session->Find<UserRow>(id);
    if (!found) {
        if (found.Err().Code() == go2cpp::storage::kErrNotFound) {
            return false;
        }
        if (error != nullptr) {
            *error = "查询用户失败: " + DescribeError(found.Err());
        }
        return false;
    }
    CopyRow(found.Value(), out);
    return true;
}

bool Store::FindUserName(std::int64_t id, std::string* name, std::string* error) {
    if (!Ok()) {
        if (error != nullptr) {
            *error = "数据库尚未打开";
        }
        return false;
    }
    auto rows = m_db->Query("SELECT name FROM users WHERE id = ?", id);
    if (!rows) {
        if (error != nullptr) {
            *error = "查询用户名失败: " + DescribeError(rows.Err());
        }
        return false;
    }
    if (!rows.Value().Next()) {
        if (rows.Value().Err() && error != nullptr) {
            *error = "遍历用户名失败: " + DescribeError(rows.Value().Err());
        }
        return false;
    }
    if (name != nullptr) {
        *name = rows.Value().Text(0);
    }
    return true;
}

bool Store::FindUserByName(const std::string& name, StoredUser* out,
                           std::string* error) {
    if (!Ok()) {
        if (error != nullptr) {
            *error = "数据库尚未打开";
        }
        return false;
    }
    if (name.empty()) {
        return false;
    }
    // 昵称不唯一：取 id 最小（最早注册）的那个，保证同一份评论文本每次
    // 解析结果一致；走 idx_users_name 精确匹配，不做 LIKE。
    auto rows = m_db->Query(
        "SELECT id, name, email, password_hash, salt, created_at FROM users "
        "WHERE name = ? ORDER BY id ASC LIMIT 1",
        name);
    if (!rows) {
        if (error != nullptr) {
            *error = "查询用户失败: " + DescribeError(rows.Err());
        }
        return false;
    }
    if (!rows.Value().Next()) {
        if (rows.Value().Err() && error != nullptr) {
            *error = "遍历用户失败: " + DescribeError(rows.Value().Err());
        }
        return false;
    }
    if (out != nullptr) {
        out->id = rows.Value().Int64(0);
        out->name = rows.Value().Text(1);
        out->email = rows.Value().Text(2);
        out->password_hash = rows.Value().Text(3);
        out->salt = rows.Value().Text(4);
        out->created_at = rows.Value().Int64(5);
    }
    return true;
}


void Store::ResetStatements() {
    if (m_session != nullptr) {
        m_session->ClearStatementCache();
    }
}

std::int64_t Store::CountUsers(std::string* error) {
    if (!Ok()) {
        if (error != nullptr) {
            *error = "数据库尚未打开";
        }
        return -1;
    }
    auto counted = m_session->Count<UserRow>();
    if (!counted) {
        if (error != nullptr) {
            *error = "统计用户失败: " + DescribeError(counted.Err());
        }
        return -1;
    }
    return counted.Value();
}

bool Store::Follow(std::int64_t follower, std::int64_t followee, std::string* error) {
    if (!Ok()) {
        if (error != nullptr) {
            *error = "数据库尚未打开";
        }
        return false;
    }
    auto executed = m_db->Exec(
        "INSERT OR IGNORE INTO follows(follower_id, followee_id, created_at) "
        "VALUES(?, ?, ?)",
        follower, followee, static_cast<std::int64_t>(std::time(nullptr)));
    if (!executed) {
        if (error != nullptr) {
            *error = "关注失败: " + DescribeError(executed.Err());
        }
        return false;
    }
    return true;
}

bool Store::Unfollow(std::int64_t follower, std::int64_t followee, std::string* error) {
    if (!Ok()) {
        if (error != nullptr) {
            *error = "数据库尚未打开";
        }
        return false;
    }
    auto executed = m_db->Exec(
        "DELETE FROM follows WHERE follower_id = ? AND followee_id = ?", follower,
        followee);
    if (!executed) {
        if (error != nullptr) {
            *error = "取关失败: " + DescribeError(executed.Err());
        }
        return false;
    }
    return true;
}

namespace {

/// @brief 查询 id 列表的公共实现。
bool QueryIds(go2cpp::storage::DB& db, const std::string& sql, std::int64_t id,
              std::vector<std::int64_t>* out, std::string* error) {
    auto rows = db.Query(sql, id);
    if (!rows) {
        if (error != nullptr) {
            *error = "查询关注列表失败: " + rows.Err().ToString();
        }
        return false;
    }
    while (rows.Value().Next()) {
        out->push_back(rows.Value().Int64(0));
    }
    if (rows.Value().Err()) {
        if (error != nullptr) {
            *error = "遍历关注列表失败: " + rows.Value().Err().ToString();
        }
        return false;
    }
    return true;
}

}  // namespace

bool Store::Following(std::int64_t id, std::vector<std::int64_t>* out,
                      std::string* error) {
    if (!Ok()) {
        if (error != nullptr) {
            *error = "数据库尚未打开";
        }
        return false;
    }
    return QueryIds(
        *m_db,
        "SELECT followee_id FROM follows WHERE follower_id = ? "
        "ORDER BY created_at DESC, followee_id DESC",
        id, out, error);
}

bool Store::Followers(std::int64_t id, std::vector<std::int64_t>* out,
                      std::string* error) {
    if (!Ok()) {
        if (error != nullptr) {
            *error = "数据库尚未打开";
        }
        return false;
    }
    return QueryIds(
        *m_db,
        "SELECT follower_id FROM follows WHERE followee_id = ? "
        "ORDER BY created_at DESC, follower_id DESC",
        id, out, error);
}

bool Store::FollowCounts(std::int64_t id, std::int64_t* followers,
                         std::int64_t* following, std::string* error) {
    if (!Ok()) {
        if (error != nullptr) {
            *error = "数据库尚未打开";
        }
        return false;
    }
    auto rows = m_db->Query(
        "SELECT"
        " (SELECT COUNT(*) FROM follows WHERE followee_id = ?),"
        " (SELECT COUNT(*) FROM follows WHERE follower_id = ?)",
        id, id);
    if (!rows) {
        if (error != nullptr) {
            *error = "统计关注失败: " + DescribeError(rows.Err());
        }
        return false;
    }
    if (!rows.Value().Next()) {
        if (error != nullptr) {
            *error = "统计关注失败: 空结果";
        }
        return false;
    }
    *followers = rows.Value().Int64(0);
    *following = rows.Value().Int64(1);
    return true;
}

bool Store::IsFollowing(std::int64_t follower, std::int64_t followee, bool* out,
                        std::string* error) {
    if (!Ok()) {
        if (error != nullptr) {
            *error = "数据库尚未打开";
        }
        return false;
    }
    auto rows = m_db->Query(
        "SELECT 1 FROM follows WHERE follower_id = ? AND followee_id = ?", follower,
        followee);
    if (!rows) {
        if (error != nullptr) {
            *error = "查询关注关系失败: " + DescribeError(rows.Err());
        }
        return false;
    }
    *out = rows.Value().Next();
    return true;
}

std::int64_t Store::CreateVideo(std::int64_t user_id, const std::string& title,
                                const std::string& url, std::string* error) {
    return CreateVideo(user_id, title, url, std::string(), 0, 0, error);
}

std::int64_t Store::CreateVideo(std::int64_t user_id, const std::string& title,
                                const std::string& url, const std::string& cover_url,
                                std::int64_t clip_start_ms, std::int64_t clip_end_ms,
                                std::string* error) {
    if (!Ok()) {
        if (error != nullptr) {
            *error = "数据库尚未打开";
        }
        return -1;
    }
    auto inserted = m_db->Exec(
        "INSERT INTO videos(user_id, title, url, cover_url, clip_start_ms, "
        "clip_end_ms, like_count, comment_count, created_at) "
        "VALUES(?, ?, ?, ?, ?, ?, 0, 0, ?)",
        user_id, title, url, cover_url, clip_start_ms, clip_end_ms,
        static_cast<std::int64_t>(std::time(nullptr)));
    if (!inserted) {
        if (error != nullptr) {
            *error = "插入视频失败: " + DescribeError(inserted.Err());
        }
        return -1;
    }
    return inserted.Value().last_insert_row_id;
}

bool Store::FindVideo(std::int64_t id, StoredVideo* out, std::string* error) {
    if (!Ok()) {
        if (error != nullptr) {
            *error = "数据库尚未打开";
        }
        return false;
    }
    auto found = m_db->Query(
        std::string("SELECT ") + kVideoColumns + " FROM videos WHERE id = ?",
        id);
    if (!found) {
        if (error != nullptr) {
            *error = "查询视频失败: " + DescribeError(found.Err());
        }
        return false;
    }
    if (!found.Value().Next()) {
        if (found.Value().Err()) {
            if (error != nullptr) {
                *error = "遍历视频失败: " + DescribeError(found.Value().Err());
            }
        }
        return false;
    }
    ReadVideoRow(&found.Value(), out);
    return true;
}

bool Store::ListByUser(std::int64_t user_id, const std::string& cursor,
                       std::int64_t limit, std::vector<StoredVideo>* out,
                       std::string* next_cursor, std::string* error) {
    next_cursor->clear();
    if (!Ok()) {
        if (error != nullptr) {
            *error = "数据库尚未打开";
        }
        return false;
    }
    if (limit <= 0) {
        return true;
    }
    const bool has_cursor = !cursor.empty();
    std::int64_t cursor_created = 0;
    std::int64_t cursor_id = 0;
    if (has_cursor &&
        !DecodeVideoCursor(cursor, &cursor_created, &cursor_id)) {
        if (error != nullptr) {
            *error = "cursor 非法";
        }
        return false;
    }

    // 时间倒序 keyset 分页：多取一条判断是否还有下一页。
    std::string sql = std::string("SELECT ") + kVideoColumns +
                      " FROM videos WHERE user_id = ?";
    if (has_cursor) {
        sql += " AND (created_at < ? OR (created_at = ? AND id < ?))";
    }
    sql += " ORDER BY created_at DESC, id DESC LIMIT ?";

    auto rows = has_cursor
                    ? m_db->Query(sql, user_id, cursor_created, cursor_created,
                                  cursor_id, limit + 1)
                    : m_db->Query(sql, user_id, limit + 1);
    if (!rows) {
        if (error != nullptr) {
            *error = "查询我的视频失败: " + DescribeError(rows.Err());
        }
        return false;
    }
    std::vector<StoredVideo> page;
    while (rows.Value().Next()) {
        StoredVideo video;
        ReadVideoRow(&rows.Value(), &video);
        page.push_back(std::move(video));
    }
    if (rows.Value().Err()) {
        if (error != nullptr) {
            *error = "遍历我的视频失败: " + DescribeError(rows.Value().Err());
        }
        return false;
    }
    if (static_cast<std::int64_t>(page.size()) > limit) {
        page.resize(static_cast<std::size_t>(limit));
        if (!page.empty()) {
            *next_cursor = EncodeVideoCursor(page.back());
        }
    }
    for (StoredVideo& video : page) {
        out->push_back(std::move(video));
    }
    return true;
}

int Store::DeleteVideo(std::int64_t id, std::int64_t user_id,
                       std::string* error) {
    if (!Ok()) {
        if (error != nullptr) {
            *error = "数据库尚未打开";
        }
        return -1;
    }
    auto deleted = m_db->Exec(
        "DELETE FROM videos WHERE id = ? AND user_id = ?", id, user_id);
    if (!deleted) {
        if (error != nullptr) {
            *error = "删除视频失败: " + DescribeError(deleted.Err());
        }
        return -1;
    }
    return deleted.Value().changes;
}

bool Store::ListFeed(const std::string& cursor, std::int64_t limit,
                     std::vector<StoredVideo>* out, std::string* next_cursor,
                     std::string* error, std::int64_t viewer_id) {
    next_cursor->clear();
    if (!Ok()) {
        if (error != nullptr) {
            *error = "数据库尚未打开";
        }
        return false;
    }
    if (limit <= 0) {
        return true;
    }
    std::uint64_t seed = 0;
    std::size_t start = 0;
    if (!cursor.empty()) {
        if (!DecodeFeedCursor(cursor, &seed, &start)) {
            if (error != nullptr) {
                *error = "cursor 非法";
            }
            return false;
        }
    } else {
        seed = RandomFeedSeed();
    }
    // 随机推荐：池子是全库视频，**不按作者过滤**——本人视频也在池中，
    // 因此随机首屏能刷到自己（旧热度序会把 0 互动的新视频挤到末页）。
    std::vector<StoredVideo> all;
    std::string sql =
        std::string("SELECT ") + kVideoColumns + " FROM videos";
    if (viewer_id > 0) {
        // 拉黑过滤：被拉黑作者的视频不进候选池，随机洗牌前就剔掉，
        // 否则分页后再过滤会出现「这一页不满 20 条」的空洞。
        sql += " WHERE user_id NOT IN"
               " (SELECT blocked_id FROM blocks WHERE user_id = ?)";
        if (!LoadVideos(*m_db, sql, &all, error, viewer_id)) {
            return false;
        }
    } else if (!LoadVideos(*m_db, sql, &all, error)) {
        return false;
    }
    std::mt19937_64 engine(seed);
    std::shuffle(all.begin(), all.end(), engine);
    if (start >= all.size()) {
        return true;
    }
    const std::size_t remaining = all.size() - start;
    const std::size_t take = remaining < static_cast<std::size_t>(limit)
                                 ? remaining
                                 : static_cast<std::size_t>(limit);
    for (std::size_t i = 0; i < take; ++i) {
        out->push_back(all[start + i]);
    }
    if (start + take < all.size()) {
        *next_cursor = EncodeFeedCursor(seed, start + take);
    }
    return true;
}

bool Store::ListFollowingFeed(std::int64_t user_id, const std::string& cursor,
                              std::int64_t limit,
                              std::vector<StoredVideo>* out,
                              std::string* next_cursor, std::string* error) {
    if (!Ok()) {
        if (error != nullptr) {
            *error = "数据库尚未打开";
        }
        return false;
    }
    std::vector<StoredVideo> all;
    // 拉黑过滤：关注流里被拉黑作者同样不出现（不必先取关，拉黑立即生效）。
    const std::string sql = std::string("SELECT ") + kVideoColumns +
                            " FROM videos WHERE user_id IN (SELECT followee_id "
                            "FROM follows WHERE follower_id = ?)"
                            " AND user_id NOT IN (SELECT blocked_id FROM blocks"
                            " WHERE user_id = ?)" +
                            kVideoOrder;
    if (!LoadVideos(*m_db, sql, &all, error, user_id, user_id)) {
        return false;
    }
    PageVideos(all, cursor, limit, out, next_cursor);
    return true;
}

// ---------------------------------------------------------------------------
// C5 互动：likes / comments / 计数同步
// ---------------------------------------------------------------------------

int Store::Like(std::int64_t video_id, std::int64_t user_id, std::string* error) {
    if (!Ok()) {
        if (error != nullptr) {
            *error = "数据库尚未打开";
        }
        return -1;
    }
    auto inserted = m_db->Exec(
        "INSERT OR IGNORE INTO likes(video_id, user_id, created_at) VALUES(?, ?, ?)",
        video_id, user_id, static_cast<std::int64_t>(std::time(nullptr)));
    if (!inserted) {
        if (error != nullptr) {
            *error = "点赞失败: " + DescribeError(inserted.Err());
        }
        return -1;
    }
    return inserted.Value().changes > 0 ? 1 : 0;
}

bool Store::Unlike(std::int64_t video_id, std::int64_t user_id,
                   std::string* error) {
    if (!Ok()) {
        if (error != nullptr) {
            *error = "数据库尚未打开";
        }
        return false;
    }
    auto deleted = m_db->Exec(
        "DELETE FROM likes WHERE video_id = ? AND user_id = ?", video_id, user_id);
    if (!deleted) {
        if (error != nullptr) {
            *error = "取消赞失败: " + DescribeError(deleted.Err());
        }
        return false;
    }
    return true;
}

std::int64_t Store::LikeCount(std::int64_t video_id, std::string* error) {
    if (!Ok()) {
        if (error != nullptr) {
            *error = "数据库尚未打开";
        }
        return -1;
    }
    auto rows = m_db->Query("SELECT COUNT(*) FROM likes WHERE video_id = ?",
                            video_id);
    if (!rows) {
        if (error != nullptr) {
            *error = "统计点赞失败: " + DescribeError(rows.Err());
        }
        return -1;
    }
    if (!rows.Value().Next()) {
        if (error != nullptr) {
            *error = "统计点赞失败: 空结果";
        }
        return -1;
    }
    return rows.Value().Int64(0);
}

bool Store::HasLiked(std::int64_t video_id, std::int64_t user_id,
                     std::string* error) {
    if (!Ok()) {
        if (error != nullptr) {
            *error = "数据库尚未打开";
        }
        return false;
    }
    auto rows = m_db->Query(
        "SELECT 1 FROM likes WHERE video_id = ? AND user_id = ?", video_id,
        user_id);
    if (!rows) {
        if (error != nullptr) {
            *error = "查询点赞失败: " + DescribeError(rows.Err());
        }
        return false;
    }
    return rows.Value().Next();
}

// ---------------------------------------------------------------------------
// C8 收藏：favorites（UNIQUE(user_id, video_id) 保证不成对重复）
// ---------------------------------------------------------------------------

int Store::ToggleFavorite(std::int64_t video_id, std::int64_t user_id,
                          std::string* error) {
    if (!Ok()) {
        if (error != nullptr) {
            *error = "数据库尚未打开";
        }
        return -1;
    }
    // step1: 先按唯一键删除——changes>0 说明本来已收藏，本次语义是取消；
    //        省掉「先查后写」的两次往返，也避免查与写之间的状态漂移。
    auto removed = m_db->Exec(
        "DELETE FROM favorites WHERE user_id = ? AND video_id = ?", user_id,
        video_id);
    if (!removed) {
        if (error != nullptr) {
            *error = "取消收藏失败: " + DescribeError(removed.Err());
        }
        return -1;
    }
    if (removed.Value().changes > 0) {
        return 0;
    }
    // step2: 原本没收藏 → 插入。INSERT OR IGNORE 兜底并发重复（唯一键冲突
    //        时 changes=0，但当前状态仍是「已收藏」，所以统一返回 1）。
    auto inserted = m_db->Exec(
        "INSERT OR IGNORE INTO favorites(user_id, video_id, created_at) "
        "VALUES(?, ?, ?)",
        user_id, video_id, static_cast<std::int64_t>(std::time(nullptr)));
    if (!inserted) {
        if (error != nullptr) {
            *error = "收藏失败: " + DescribeError(inserted.Err());
        }
        return -1;
    }
    return 1;
}

bool Store::HasFavorited(std::int64_t video_id, std::int64_t user_id,
                         std::string* error) {
    if (!Ok()) {
        if (error != nullptr) {
            *error = "数据库尚未打开";
        }
        return false;
    }
    auto rows = m_db->Query(
        "SELECT 1 FROM favorites WHERE user_id = ? AND video_id = ?", user_id,
        video_id);
    if (!rows) {
        if (error != nullptr) {
            *error = "查询收藏失败: " + DescribeError(rows.Err());
        }
        return false;
    }
    return rows.Value().Next();
}

bool Store::ListFavorites(std::int64_t user_id, std::int64_t limit,
                          std::vector<StoredVideo>* out, std::string* error) {
    if (!Ok()) {
        if (error != nullptr) {
            *error = "数据库尚未打开";
        }
        return false;
    }
    if (limit <= 0) {
        return true;
    }
    // 列顺序必须与 ReadVideoRow / kVideoColumns 一致；排序键是收藏时间，
    // 而不是视频发布时间——「最近收藏的排前面」才符合用户预期。
    auto rows = m_db->Query(
        "SELECT v.id, v.user_id, v.title, v.url, v.like_count, v.comment_count, "
        "v.created_at FROM favorites f JOIN videos v ON v.id = f.video_id "
        "WHERE f.user_id = ? AND v.user_id NOT IN (SELECT blocked_id FROM blocks"
        " WHERE user_id = ?) ORDER BY f.created_at DESC, v.id DESC LIMIT ?",
        user_id, user_id, limit);
    if (!rows) {
        if (error != nullptr) {
            *error = "查询收藏列表失败: " + DescribeError(rows.Err());
        }
        return false;
    }
    while (rows.Value().Next()) {
        StoredVideo video;
        ReadVideoRow(&rows.Value(), &video);
        out->push_back(std::move(video));
    }
    if (rows.Value().Err()) {
        if (error != nullptr) {
            *error = "遍历收藏列表失败: " + DescribeError(rows.Value().Err());
        }
        return false;
    }
    return true;
}

namespace {

/**
 * @brief 主页时间线的三类数据源。
 */
enum class TimelineKind {
    kWorks = 0,     ///< 作者作品：按 videos.created_at 倒序。
    kLiked = 1,     ///< 用户点赞：按 likes.created_at 倒序。
    kFavorite = 2,  ///< 用户收藏：按 favorites.created_at 倒序。
};

/// @brief 三类时间线各自的游标前缀，防止 A tab 的游标被 B tab 复用。
char TimelineTag(TimelineKind kind) {
    switch (kind) {
        case TimelineKind::kWorks:
            return 'w';
        case TimelineKind::kLiked:
            return 'l';
        case TimelineKind::kFavorite:
            return 'f';
    }
    return 'w';
}

/// @brief 编码带类型前缀的时间线游标：`w<created_at>-<video_id>`。
std::string EncodeTimelineCursor(TimelineKind kind, std::int64_t created_at,
                                 std::int64_t video_id) {
    if (created_at <= 0 || video_id <= 0) {
        return std::string();
    }
    return std::string(1, TimelineTag(kind)) + std::to_string(created_at) + "-" +
           std::to_string(video_id);
}

/// @brief 解码时间线游标；前缀不匹配一律视为非法（调用方回 400）。
bool DecodeTimelineCursor(TimelineKind kind, const std::string& cursor,
                          std::int64_t* created_at, std::int64_t* video_id) {
    if (cursor.size() < 3 || cursor.front() != TimelineTag(kind)) {
        return false;
    }
    return DecodeVideoCursor(cursor.substr(1), created_at, video_id);
}

/// @brief 时间线的 FROM/WHERE 前半段：来源表 + 主人过滤（拉黑条件在后面追加）。
std::string TimelineFromClause(TimelineKind kind) {
    switch (kind) {
        case TimelineKind::kWorks:
            return " FROM videos v WHERE v.user_id = ?";
        case TimelineKind::kLiked:
            return " FROM likes a JOIN videos v ON v.id = a.video_id"
                   " WHERE a.user_id = ?";
        case TimelineKind::kFavorite:
            return " FROM favorites a JOIN videos v ON v.id = a.video_id"
                   " WHERE a.user_id = ?";
    }
    return " FROM videos v WHERE v.user_id = ?";
}

/// @brief 排序/游标的「发生时间」列：作品取发布时间，喜欢/收藏取动作时间。
const char* TimelineTimeColumn(TimelineKind kind) {
    return kind == TimelineKind::kWorks ? "v.created_at" : "a.created_at";
}

/**
 * @brief 双向拉黑过滤片段：两个占位符都填 `viewer_id`。
 * @note 匿名（viewer_id=0）时两个子查询都为空集，条件恒真——与既有
 *       「匿名无个人黑名单」语义一致，不额外分支。
 */
constexpr const char* kTimelineBlockFilter =
    " AND v.user_id NOT IN (SELECT blocked_id FROM blocks WHERE user_id = ?)"
    " AND NOT EXISTS (SELECT 1 FROM blocks b WHERE b.user_id = v.user_id"
    " AND b.blocked_id = ?)";

/**
 * @brief 主页时间线通用查询：作品 / 喜欢 / 收藏，keyset 分页 + 双向拉黑过滤。
 *
 * @param db [in] 已打开的数据库；不可为 nullptr，调用前须确认 `Ok()`。
 * @param kind [in] 数据源类型，决定来源表、时间列与游标前缀。
 * @param owner_id [in] 主页主人 id（>0）；喜欢/收藏时是动作发起者。
 * @param viewer_id [in] 查看者 id；0=匿名，不做个人黑名单过滤。
 * @param cursor [in] 空串=第一页；否则必须是同 kind 的 `next_cursor`。
 * @param limit [in] 本页最多条数（>0）；调用方已 clamp。
 * @param out [out] 追加到末尾（不清空调用方已有内容）；失败时内容可能已部分写入。
 * @param next_cursor [out] 还有下一页时写游标，否则写空串。
 * @param total [out] 同过滤条件下的总条数；可为 nullptr 表示不统计。
 * @param error [out] 失败时写错误文本；成功时保持原值。
 * @return true=成功（可能空页）；false=cursor 前缀/格式非法或数据库错误。
 *
 * ## 定位
 * 主页三个 tab 的唯一分页实现；三个 tab 的差异只有「来源表 + 时间列」，
 * 合成一处避免三份 SQL 各自漏掉拉黑过滤。错了会怎样：拉黑后仍能从主页
 * 看到对方视频，或翻页时同一秒的多条重复/漏出。
 *
 * ## 并发/失败语义
 * - 调用约束：在服务请求线程内串行调用，不跨线程共享游标。
 * - 失败语义：返回 false 时 `next_cursor` 为空、`out` 可能已追加部分行；
 *   调用方应回 400/500 并丢弃本页。
 * - 不变量：成功时 `out` 中每条的 `video.created_at` 是视频发布时间，
 *   游标时间来自第 8 列（点赞/收藏时间），两者不可混用。
 */
bool QueryTimeline(go2cpp::storage::DB* db, TimelineKind kind,
                   std::int64_t owner_id, std::int64_t viewer_id,
                   const std::string& cursor, std::int64_t limit,
                   std::vector<StoredVideo>* out, std::string* next_cursor,
                   std::int64_t* total, std::string* error) {
    // step1: 拼公共 FROM/WHERE；拉黑过滤对三个 tab 一视同仁。
    const std::string from = TimelineFromClause(kind) + kTimelineBlockFilter;
    // step2: 首页顺带统计总数（同一 where，参数顺序与列表一致）。
    if (total != nullptr) {
        auto counted = db->Query("SELECT COUNT(*)" + from, owner_id, viewer_id,
                                 viewer_id);
        if (!counted) {
            if (error != nullptr) {
                *error = "统计主页视频失败: " + DescribeError(counted.Err());
            }
            return false;
        }
        if (!counted.Value().Next()) {
            if (error != nullptr) {
                *error = "统计主页视频失败: 无结果行";
            }
            return false;
        }
        *total = counted.Value().Int64(0);
    }
    // step3: 游标解码——前缀必须与 kind 匹配，防止三个 tab 的游标串用。
    const bool has_cursor = !cursor.empty();
    std::int64_t cursor_time = 0;
    std::int64_t cursor_id = 0;
    if (has_cursor &&
        !DecodeTimelineCursor(kind, cursor, &cursor_time, &cursor_id)) {
        if (error != nullptr) {
            *error = "cursor 非法";
        }
        return false;
    }
    // step4: 时间倒序 keyset 分页；多取一条判断是否还有下一页。
    const char* time_col = TimelineTimeColumn(kind);
    std::string sql =
        std::string("SELECT v.id, v.user_id, v.title, v.url, v.like_count, "
                    "v.comment_count, v.created_at, ") +
        time_col + from;
    if (has_cursor) {
        sql += std::string(" AND (") + time_col + " < ? OR (" + time_col +
               " = ? AND v.id < ?))";
    }
    sql += std::string(" ORDER BY ") + time_col + " DESC, v.id DESC LIMIT ?";

    auto rows = has_cursor
                    ? db->Query(sql, owner_id, viewer_id, viewer_id,
                                cursor_time, cursor_time, cursor_id, limit + 1)
                    : db->Query(sql, owner_id, viewer_id, viewer_id, limit + 1);
    if (!rows) {
        if (error != nullptr) {
            *error = "查询主页视频失败: " + DescribeError(rows.Err());
        }
        return false;
    }
    // step5: 每行同时记住视频（前 7 列）与动作时间（第 8 列），游标用后者。
    struct TimelineRow {
        StoredVideo video;
        std::int64_t activity_at{0};
    };
    std::vector<TimelineRow> page;
    while (rows.Value().Next()) {
        TimelineRow row;
        ReadVideoRow(&rows.Value(), &row.video);
        row.activity_at = rows.Value().Int64(7);
        page.push_back(std::move(row));
    }
    if (rows.Value().Err()) {
        if (error != nullptr) {
            *error = "遍历主页视频失败: " + DescribeError(rows.Value().Err());
        }
        return false;
    }
    // step6: 截断到 limit；只有确实还有下一页时才给游标。
    if (static_cast<std::int64_t>(page.size()) > limit) {
        page.resize(static_cast<std::size_t>(limit));
        if (!page.empty()) {
            *next_cursor = EncodeTimelineCursor(kind, page.back().activity_at,
                                                page.back().video.id);
        }
    }
    for (TimelineRow& row : page) {
        out->push_back(std::move(row.video));
    }
    return true;
}

}  // namespace

bool Store::ListUserVideos(std::int64_t owner_id, std::int64_t viewer_id,
                           const std::string& cursor, std::int64_t limit,
                           std::vector<StoredVideo>* out,
                           std::string* next_cursor, std::int64_t* total,
                           std::string* error) {
    next_cursor->clear();
    if (!Ok()) {
        if (error != nullptr) {
            *error = "数据库尚未打开";
        }
        return false;
    }
    if (limit <= 0) {
        if (total != nullptr) {
            *total = 0;
        }
        return true;
    }
    return QueryTimeline(m_db.get(), TimelineKind::kWorks, owner_id, viewer_id,
                         cursor, limit, out, next_cursor, total, error);
}

bool Store::ListLikedVideos(std::int64_t owner_id, std::int64_t viewer_id,
                            const std::string& cursor, std::int64_t limit,
                            std::vector<StoredVideo>* out,
                            std::string* next_cursor, std::int64_t* total,
                            std::string* error) {
    next_cursor->clear();
    if (!Ok()) {
        if (error != nullptr) {
            *error = "数据库尚未打开";
        }
        return false;
    }
    if (limit <= 0) {
        if (total != nullptr) {
            *total = 0;
        }
        return true;
    }
    return QueryTimeline(m_db.get(), TimelineKind::kLiked, owner_id, viewer_id,
                         cursor, limit, out, next_cursor, total, error);
}

bool Store::ListFavoriteVideos(std::int64_t owner_id, std::int64_t viewer_id,
                               const std::string& cursor, std::int64_t limit,
                               std::vector<StoredVideo>* out,
                               std::string* next_cursor, std::int64_t* total,
                               std::string* error) {
    next_cursor->clear();
    if (!Ok()) {
        if (error != nullptr) {
            *error = "数据库尚未打开";
        }
        return false;
    }
    if (limit <= 0) {
        if (total != nullptr) {
            *total = 0;
        }
        return true;
    }
    return QueryTimeline(m_db.get(), TimelineKind::kFavorite, owner_id, viewer_id,
                         cursor, limit, out, next_cursor, total, error);
}

std::int64_t Store::AddComment(std::int64_t video_id, std::int64_t user_id,
                               const std::string& content, std::string* error,
                               std::int64_t parent_id) {
    if (!Ok()) {
        if (error != nullptr) {
            *error = "数据库尚未打开";
        }
        return -1;
    }
    auto inserted = m_db->Exec(
        "INSERT INTO comments(video_id, user_id, content, created_at, parent_id) "
        "VALUES(?, ?, ?, ?, ?)",
        video_id, user_id, content,
        static_cast<std::int64_t>(std::time(nullptr)), parent_id);
    if (!inserted) {
        if (error != nullptr) {
            *error = "插入评论失败: " + DescribeError(inserted.Err());
        }
        return -1;
    }
    return inserted.Value().last_insert_row_id;
}

bool Store::ListComments(std::int64_t video_id, const std::string& cursor,
                         std::int64_t limit, const std::string& sort,
                         std::vector<StoredComment>* out,
                         std::string* next_cursor, std::string* error,
                         std::int64_t viewer_id) {
    next_cursor->clear();
    if (!Ok()) {
        if (error != nullptr) {
            *error = "数据库尚未打开";
        }
        return false;
    }
    if (limit <= 0) {
        return true;
    }
    // step1: sort 只接受 new / hot；其它值回 400，不静默按 new 处理
    //        （否则客户端以为在按热度翻页，实际拿到时间序）。
    if (sort != "new" && sort != "hot") {
        if (error != nullptr) {
            *error = "sort 非法";
        }
        return false;
    }
    const bool hot = sort == "hot";
    const bool has_cursor = !cursor.empty();
    std::int64_t cursor_like = 0;
    std::int64_t cursor_created = 0;
    std::int64_t cursor_id = 0;
    if (has_cursor) {
        // step2: 两种游标格式互斥：hot 的 `h...` 交给 hot 解码器；new 的
        //        `<created>-<id>` 交给共用解码器。串用在这里就失败。
        const bool decoded =
            hot ? DecodeHotCommentCursor(cursor, &cursor_like, &cursor_created,
                                         &cursor_id)
                : DecodeCommentCursor(cursor, &cursor_created, &cursor_id);
        if (!decoded) {
            if (error != nullptr) {
                *error = "cursor 非法";
            }
            return false;
        }
    }
    // step3: 条件写成「?=0 关闭」，new 与 hot 各一条固定参数个数的 SQL，
    //        避免 2(sort) × 2(cursor) × 2(blocked) 的八份拼接分支漂移。
    const std::int64_t has_cursor_flag = has_cursor ? 1 : 0;
    const std::int64_t want = limit + 1;
    if (hot) {
        // hot：点赞数降序，同点赞数按时间降序、id 降序。
        std::string sql = std::string("SELECT ") + kHotCommentColumns +
                          " FROM comments c"
                          " WHERE c.video_id = ? AND c.parent_id = 0"
                          " AND c.pinned_at = 0"
                          " AND (? = 0 OR c.user_id NOT IN"
                          " (SELECT blocked_id FROM blocks WHERE user_id = ?))"
                          " AND (? = 0 OR ";
        sql += kCommentLikeExpr;
        sql += " < ? OR (";
        sql += kCommentLikeExpr;
        sql += " = ? AND (c.created_at < ? OR (c.created_at = ?"
               " AND c.id < ?)))) ORDER BY ";
        sql += kCommentLikeExpr;
        sql += " DESC, c.created_at DESC, c.id DESC LIMIT ?";
        auto rows = m_db->Query(
            sql, video_id, (viewer_id > 0 ? 1 : 0), viewer_id,
            has_cursor_flag, cursor_like, cursor_like, cursor_created,
            cursor_created, cursor_id, want);
        if (!rows) {
            if (error != nullptr) {
                *error = "查询评论失败: " + DescribeError(rows.Err());
            }
            return false;
        }
        return ReadHotCommentPage(&rows.Value(), limit, next_cursor, out, error);
    }
    // new：时间倒序（与第 1 轮行为一致，索引 idx_comments_video 直接可用）。
    std::string sql = std::string("SELECT ") + kCommentColumns +
                      " FROM comments"
                      " WHERE video_id = ? AND parent_id = 0"
                      " AND pinned_at = 0"
                      " AND (? = 0 OR user_id NOT IN"
                      " (SELECT blocked_id FROM blocks WHERE user_id = ?))"
                      " AND (? = 0 OR created_at < ?"
                      " OR (created_at = ? AND id < ?))"
                      " ORDER BY created_at DESC, id DESC LIMIT ?";
    auto rows = m_db->Query(
        sql, video_id, (viewer_id > 0 ? 1 : 0), viewer_id, has_cursor_flag,
        cursor_created, cursor_created, cursor_id, want);
    if (!rows) {
        if (error != nullptr) {
            *error = "查询评论失败: " + DescribeError(rows.Err());
        }
        return false;
    }
    return ReadCommentPage(&rows.Value(), limit, next_cursor, out, error);
}

bool Store::ListPinnedComments(std::int64_t video_id, std::int64_t viewer_id,
                               std::vector<StoredComment>* out,
                               std::string* error) {
    if (!Ok()) {
        if (error != nullptr) {
            *error = "数据库尚未打开";
        }
        return false;
    }
    // 置顶条数按产品惯例给个上限（20），避免异常数据把首屏撑爆；
    // 置顶评论不占分页 limit，第一页固定额外返回。
    constexpr std::int64_t kMaxPinned = 20;
    std::string sql = std::string("SELECT ") + kCommentColumns +
                      " FROM comments WHERE video_id = ? AND pinned_at > 0";
    if (viewer_id > 0) {
        sql += " AND user_id NOT IN"
               " (SELECT blocked_id FROM blocks WHERE user_id = ?)";
    }
    sql += " ORDER BY pinned_at DESC, id DESC LIMIT ?";
    auto rows = viewer_id > 0
                    ? m_db->Query(sql, video_id, viewer_id, kMaxPinned)
                    : m_db->Query(sql, video_id, kMaxPinned);
    if (!rows) {
        if (error != nullptr) {
            *error = "查询置顶评论失败: " + DescribeError(rows.Err());
        }
        return false;
    }
    while (rows.Value().Next()) {
        StoredComment comment;
        ReadCommentRow(&rows.Value(), &comment);
        out->push_back(std::move(comment));
    }
    if (rows.Value().Err()) {
        if (error != nullptr) {
            *error = "遍历置顶评论失败: " + DescribeError(rows.Value().Err());
        }
        return false;
    }
    return true;
}

bool Store::ListReplies(std::int64_t video_id, std::int64_t parent_id,
                        std::int64_t limit, std::vector<StoredComment>* out,
                        std::string* error, std::int64_t viewer_id) {
    if (!Ok()) {
        if (error != nullptr) {
            *error = "数据库尚未打开";
        }
        return false;
    }
    if (limit <= 0) {
        return true;
    }
    std::string sql = std::string("SELECT ") + kCommentColumns +
                      " FROM comments WHERE video_id = ? AND parent_id = ?";
    if (viewer_id > 0) {
        sql += " AND user_id NOT IN"
               " (SELECT blocked_id FROM blocks WHERE user_id = ?)";
        sql += " ORDER BY created_at ASC, id ASC LIMIT ?";
        auto rows = m_db->Query(sql, video_id, parent_id, viewer_id, limit);
        if (!rows) {
            if (error != nullptr) {
                *error = "查询回复失败: " + DescribeError(rows.Err());
            }
            return false;
        }
        while (rows.Value().Next()) {
            StoredComment comment;
            ReadCommentRow(&rows.Value(), &comment);
            out->push_back(std::move(comment));
        }
        if (rows.Value().Err()) {
            if (error != nullptr) {
                *error = "遍历回复失败: " + DescribeError(rows.Value().Err());
            }
            return false;
        }
        return true;
    }
    sql += " ORDER BY created_at ASC, id ASC LIMIT ?";
    auto rows = m_db->Query(sql, video_id, parent_id, limit);
    if (!rows) {
        if (error != nullptr) {
            *error = "查询回复失败: " + DescribeError(rows.Err());
        }
        return false;
    }
    while (rows.Value().Next()) {
        StoredComment comment;
        ReadCommentRow(&rows.Value(), &comment);
        out->push_back(std::move(comment));
    }
    if (rows.Value().Err()) {
        if (error != nullptr) {
            *error = "遍历回复失败: " + DescribeError(rows.Value().Err());
        }
        return false;
    }
    return true;
}

bool Store::FindComment(std::int64_t id, StoredComment* out,
                        std::string* error) {
    if (!Ok()) {
        if (error != nullptr) {
            *error = "数据库尚未打开";
        }
        return false;
    }
    auto rows = m_db->Query(std::string("SELECT ") + kCommentColumns +
                                " FROM comments WHERE id = ?",
                            id);
    if (!rows) {
        if (error != nullptr) {
            *error = "查询评论失败: " + DescribeError(rows.Err());
        }
        return false;
    }
    if (!rows.Value().Next()) {
        return false;
    }
    ReadCommentRow(&rows.Value(), out);
    return true;
}

int Store::DeleteComment(std::int64_t id, std::int64_t user_id,
                         std::string* error) {
    if (!Ok()) {
        if (error != nullptr) {
            *error = "数据库尚未打开";
        }
        return -1;
    }
    auto deleted = m_db->Exec("DELETE FROM comments WHERE id = ? AND user_id = ?",
                              id, user_id);
    if (!deleted) {
        if (error != nullptr) {
            *error = "删除评论失败: " + DescribeError(deleted.Err());
        }
        return -1;
    }
    return deleted.Value().changes;
}

int Store::SetCommentPinned(std::int64_t id, std::int64_t pinned_at,
                            std::string* error) {
    if (!Ok()) {
        if (error != nullptr) {
            *error = "数据库尚未打开";
        }
        return -1;
    }
    // step1: 参数闸门——负时间是脏数据，宁可不写。
    if (id <= 0 || pinned_at < 0) {
        if (error != nullptr) {
            *error = "置顶参数非法";
        }
        return -1;
    }
    // step2: 单条 UPDATE；SQLite 的 changes 统计「匹配行」，同值重复置顶
    //        也返回 1，正好对上「幂等」语义（0 只表示评论不存在）。
    auto updated = m_db->Exec(
        "UPDATE comments SET pinned_at = ? WHERE id = ?", pinned_at, id);
    if (!updated) {
        if (error != nullptr) {
            *error = "更新置顶状态失败: " + DescribeError(updated.Err());
        }
        return -1;
    }
    return updated.Value().changes;
}

bool Store::UpdateVideoCounts(std::int64_t video_id, std::string* error) {
    if (!Ok()) {
        if (error != nullptr) {
            *error = "数据库尚未打开";
        }
        return false;
    }
    if (auto begin_error = m_db->Begin(); begin_error) {
        if (error != nullptr) {
            *error = "计数事务开始失败: " + begin_error.ToString();
        }
        return false;
    }
    auto updated = m_db->Exec(
        "UPDATE videos SET"
        " like_count = (SELECT COUNT(*) FROM likes WHERE video_id = ?),"
        " comment_count = (SELECT COUNT(*) FROM comments WHERE video_id = ?)"
        " WHERE id = ?",
        video_id, video_id, video_id);
    if (!updated) {
        (void)m_db->Rollback();
        if (error != nullptr) {
            *error = "更新计数失败: " + DescribeError(updated.Err());
        }
        return false;
    }
    if (auto commit_error = m_db->Commit(); commit_error) {
        if (error != nullptr) {
            *error = "计数事务提交失败: " + commit_error.ToString();
        }
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// 评论点赞：comment_likes（主键即幂等键，计数用 GROUP BY 批量算）
// ---------------------------------------------------------------------------

int Store::LikeComment(std::int64_t comment_id, std::int64_t user_id,
                       std::string* error) {
    if (!Ok()) {
        if (error != nullptr) {
            *error = "数据库尚未打开";
        }
        return -1;
    }
    // step1: INSERT OR IGNORE —— 主键冲突说明已经点过，changes=0 即幂等命中。
    auto inserted = m_db->Exec(
        "INSERT OR IGNORE INTO comment_likes(comment_id, user_id, created_at)"
        " VALUES(?, ?, ?)",
        comment_id, user_id, static_cast<std::int64_t>(std::time(nullptr)));
    if (!inserted) {
        if (error != nullptr) {
            *error = "评论点赞失败: " + DescribeError(inserted.Err());
        }
        return -1;
    }
    return inserted.Value().changes > 0 ? 1 : 0;
}

bool Store::UnlikeComment(std::int64_t comment_id, std::int64_t user_id,
                          std::string* error) {
    if (!Ok()) {
        if (error != nullptr) {
            *error = "数据库尚未打开";
        }
        return false;
    }
    auto deleted = m_db->Exec(
        "DELETE FROM comment_likes WHERE comment_id = ? AND user_id = ?",
        comment_id, user_id);
    if (!deleted) {
        if (error != nullptr) {
            *error = "取消评论点赞失败: " + DescribeError(deleted.Err());
        }
        return false;
    }
    return true;
}

bool Store::FillCommentLikes(std::int64_t viewer_id,
                             std::vector<StoredComment>* comments,
                             std::string* error) {
    // step1: 空页直接返回——不产生任何 SQL，也不当作错误。
    if (comments == nullptr || comments->empty()) {
        return true;
    }
    if (!Ok()) {
        if (error != nullptr) {
            *error = "数据库尚未打开";
        }
        return false;
    }
    // step2: 拼 IN 列表。元素全是库内自增 id，无用户输入，不存在注入面；
    //        一条 GROUP BY 拿到整页计数，避免 N+1。
    std::string ids;
    ids.reserve(comments->size() * 8);
    for (std::size_t i = 0; i < comments->size(); ++i) {
        if (i != 0) {
            ids += ',';
        }
        ids += std::to_string((*comments)[i].id);
    }
    std::unordered_map<std::int64_t, std::int64_t> counts;
    {
        auto rows = m_db->Query(
            "SELECT comment_id, COUNT(*) FROM comment_likes WHERE comment_id IN (" +
            ids + ") GROUP BY comment_id");
        if (!rows) {
            if (error != nullptr) {
                *error = "统计评论点赞失败: " + DescribeError(rows.Err());
            }
            return false;
        }
        while (rows.Value().Next()) {
            counts[rows.Value().Int64(0)] = rows.Value().Int64(1);
        }
        if (rows.Value().Err()) {
            if (error != nullptr) {
                *error = "遍历评论点赞失败: " + DescribeError(rows.Value().Err());
            }
            return false;
        }
    }
    // step3: 查看者已赞集合——匿名（viewer<=0）跳过这条 SQL。
    std::unordered_map<std::int64_t, bool> liked;
    if (viewer_id > 0) {
        auto rows = m_db->Query(
            "SELECT comment_id FROM comment_likes WHERE user_id = ?"
            " AND comment_id IN (" + ids + ")",
            viewer_id);
        if (!rows) {
            if (error != nullptr) {
                *error = "查询已赞评论失败: " + DescribeError(rows.Err());
            }
            return false;
        }
        while (rows.Value().Next()) {
            liked[rows.Value().Int64(0)] = true;
        }
        if (rows.Value().Err()) {
            if (error != nullptr) {
                *error = "遍历已赞评论失败: " + DescribeError(rows.Value().Err());
            }
            return false;
        }
    }
    // step4: 回填；没有记录的评论保持 0 赞 / 未赞。
    for (auto& comment : *comments) {
        const auto count_it = counts.find(comment.id);
        comment.like_count = count_it == counts.end() ? 0 : count_it->second;
        comment.liked = viewer_id > 0 && liked.find(comment.id) != liked.end();
    }
    return true;
}

// ---------------------------------------------------------------------------
// 消息通知中心：notifications
// ---------------------------------------------------------------------------

namespace {

/// @brief 合法通知类型；未知 kind 拒绝，避免脏数据让前端渲染成未知文案。
bool IsValidNotificationKind(std::string_view kind) {
    return kind == "like" || kind == "comment" || kind == "follow" ||
           kind == "mention" || kind == "system";
}

/// @brief 合法通知目标类型（`system` 只用于系统通知，可无具体目标）。
bool IsValidNotificationTargetType(std::string_view type) {
    return type == "video" || type == "comment" || type == "user" ||
           type == "system";
}

}  // namespace

bool Store::AddNotification(std::int64_t user_id, std::int64_t actor_id,
                            const std::string& kind,
                            const std::string& target_type,
                            std::int64_t target_id,
                            const std::string& content, std::string* error) {
    // step1: 参数校验——脏数据写进去只能靠人工清理，宁可不写。
    //        system 是唯一允许「无真人触发者 / 无具体目标」的类型：
    //        系统广播由服务端发起，actor_id=0、target_id=0 表示无。
    const bool is_system = kind == "system";
    if (user_id <= 0 || actor_id < 0 || target_id < 0 ||
        (!is_system && (actor_id == 0 || target_id == 0)) ||
        !IsValidNotificationKind(kind) ||
        !IsValidNotificationTargetType(target_type)) {
        if (error != nullptr) {
            *error = "通知参数非法";
        }
        return false;
    }
    if (!Ok()) {
        if (error != nullptr) {
            *error = "数据库尚未打开";
        }
        return false;
    }
    // step2: 单条 INSERT；created_at 用当前 Unix 秒（与其它表一致）。
    auto inserted = m_db->Exec(
        "INSERT INTO notifications(user_id, actor_id, kind, target_type, "
        "target_id, content, created_at, read_at) VALUES(?, ?, ?, ?, ?, ?, ?, 0)",
        user_id, actor_id, kind, target_type, target_id, content,
        static_cast<std::int64_t>(std::time(nullptr)));
    if (!inserted) {
        if (error != nullptr) {
            *error = "写通知失败: " + DescribeError(inserted.Err());
        }
        return false;
    }
    return true;
}

bool Store::ListNotifications(std::int64_t user_id, const std::string& kind,
                              const std::string& cursor, std::int64_t limit,
                              std::vector<StoredNotification>* out,
                              std::string* next_cursor, std::string* error) {
    next_cursor->clear();
    if (!Ok()) {
        if (error != nullptr) {
            *error = "数据库尚未打开";
        }
        return false;
    }
    if (limit <= 0) {
        return true;
    }
    const bool has_cursor = !cursor.empty();
    const bool has_kind = !kind.empty();
    std::int64_t cursor_created = 0;
    std::int64_t cursor_id = 0;
    if (has_cursor &&
        !DecodeNotificationCursor(cursor, &cursor_created, &cursor_id)) {
        if (error != nullptr) {
            *error = "cursor 非法";
        }
        return false;
    }

    // step1: 一条 JOIN 取整页 + 触发者昵称；多取一条判断是否还有下一页。
    std::string sql =
        "SELECT n.id, n.user_id, n.actor_id, COALESCE(u.name, ''), n.kind,"
        " n.target_type, n.target_id, n.content, n.created_at, n.read_at"
        " FROM notifications n LEFT JOIN users u ON u.id = n.actor_id"
        " WHERE n.user_id = ?"
        // 被拉黑者触发的通知不展示：列表与未读数用同一个条件，避免徽章
        // 数字比列表多。
        " AND n.actor_id NOT IN (SELECT blocked_id FROM blocks"
        " WHERE user_id = ?)";
    // kind 已由服务层按白名单校验，但仍走参数绑定，不拼进 SQL 文本。
    if (has_kind) {
        sql += " AND n.kind = ?";
    }
    if (has_cursor) {
        sql += " AND (n.created_at < ? OR (n.created_at = ? AND n.id < ?))";
    }
    sql += " ORDER BY n.created_at DESC, n.id DESC LIMIT ?";

    // 4 种参数组合各调一次，保持参数绑定顺序与 SQL 占位符一一对应。
    auto rows = [&]() {
        if (has_kind) {
            if (has_cursor) {
                return m_db->Query(sql, user_id, user_id, kind, cursor_created,
                                   cursor_created, cursor_id, limit + 1);
            }
            return m_db->Query(sql, user_id, user_id, kind, limit + 1);
        }
        if (has_cursor) {
            return m_db->Query(sql, user_id, user_id, cursor_created,
                               cursor_created, cursor_id, limit + 1);
        }
        return m_db->Query(sql, user_id, user_id, limit + 1);
    }();
    if (!rows) {
        if (error != nullptr) {
            *error = "查询通知失败: " + DescribeError(rows.Err());
        }
        return false;
    }
    std::vector<StoredNotification> page;
    while (rows.Value().Next()) {
        StoredNotification item;
        item.id = rows.Value().Int64(0);
        item.user_id = rows.Value().Int64(1);
        item.actor_id = rows.Value().Int64(2);
        item.actor_name = rows.Value().Text(3);
        item.kind = rows.Value().Text(4);
        item.target_type = rows.Value().Text(5);
        item.target_id = rows.Value().Int64(6);
        item.content = rows.Value().Text(7);
        item.created_at = rows.Value().Int64(8);
        item.read_at = rows.Value().Int64(9);
        page.push_back(std::move(item));
    }
    if (rows.Value().Err()) {
        if (error != nullptr) {
            *error = "遍历通知失败: " + DescribeError(rows.Value().Err());
        }
        return false;
    }
    // step2: 多取的一条只用于判断「还有下一页」，不返回给调用方。
    if (static_cast<std::int64_t>(page.size()) > limit) {
        page.resize(static_cast<std::size_t>(limit));
        if (!page.empty()) {
            *next_cursor = EncodeNotificationCursor(page.back());
        }
    }
    for (StoredNotification& item : page) {
        out->push_back(std::move(item));
    }
    return true;
}

std::int64_t Store::CountUnreadNotifications(std::int64_t user_id,
                                             std::string* error) {
    if (!Ok()) {
        if (error != nullptr) {
            *error = "数据库尚未打开";
        }
        return -1;
    }
    auto rows = m_db->Query(
        "SELECT COUNT(*) FROM notifications WHERE user_id = ? AND read_at = 0"
        " AND actor_id NOT IN (SELECT blocked_id FROM blocks WHERE user_id = ?)",
        user_id, user_id);
    if (!rows) {
        if (error != nullptr) {
            *error = "统计未读通知失败: " + DescribeError(rows.Err());
        }
        return -1;
    }
    if (!rows.Value().Next()) {
        if (error != nullptr) {
            *error = "统计未读通知失败: 空结果";
        }
        return -1;
    }
    return rows.Value().Int64(0);
}

bool Store::CountUnreadNotificationsByKind(
    std::int64_t user_id, NotificationUnreadCounts* out, std::string* error) {
    if (out == nullptr) {
        if (error != nullptr) {
            *error = "输出参数为空";
        }
        return false;
    }
    if (!Ok()) {
        if (error != nullptr) {
            *error = "数据库尚未打开";
        }
        return false;
    }
    // step1: 一条 GROUP BY 拿到全部 kind 的未读数；输出行数 = kind 数。
    //        过滤条件与 ListNotifications / CountUnreadNotifications 完全一致，
    //        保证 tab 角标与列表条数对得上。
    auto rows = m_db->Query(
        "SELECT kind, COUNT(*) FROM notifications"
        " WHERE user_id = ? AND read_at = 0"
        " AND actor_id NOT IN (SELECT blocked_id FROM blocks WHERE user_id = ?)"
        " GROUP BY kind",
        user_id, user_id);
    if (!rows) {
        if (error != nullptr) {
            *error = "统计分类未读失败: " + DescribeError(rows.Err());
        }
        return false;
    }
    NotificationUnreadCounts counts;
    while (rows.Value().Next()) {
        const std::string kind = rows.Value().Text(0);
        const std::int64_t value = rows.Value().Int64(1);
        if (value <= 0) {
            continue;
        }
        // all 含历史脏 kind：徽章不能比列表少，否则未读永远清不掉。
        counts.all += value;
        if (kind == "like") {
            counts.like = value;
        } else if (kind == "comment") {
            counts.comment = value;
        } else if (kind == "follow") {
            counts.follow = value;
        } else if (kind == "mention") {
            counts.mention = value;
        } else if (kind == "system") {
            counts.system = value;
        }
    }
    if (rows.Value().Err()) {
        if (error != nullptr) {
            *error = "遍历分类未读失败: " + DescribeError(rows.Value().Err());
        }
        return false;
    }
    // step2: 整体写入，避免调用方看到半更新的计数。
    *out = counts;
    return true;
}

std::int64_t Store::MarkNotificationsRead(std::int64_t user_id,
                                          const std::vector<std::int64_t>& ids,
                                          bool all, const std::string& kind,
                                          std::string* error) {
    if (!Ok()) {
        if (error != nullptr) {
            *error = "数据库尚未打开";
        }
        return -1;
    }
    const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
    // step1: 单类已读（tab 右上角「全部已读」）——kind 优先于 all/ids，
    //        一条 UPDATE 命中 (user_id, read_at, kind) 索引。
    if (!kind.empty()) {
        auto updated = m_db->Exec(
            "UPDATE notifications SET read_at = ?"
            " WHERE user_id = ? AND read_at = 0 AND kind = ?",
            now, user_id, kind);
        if (!updated) {
            if (error != nullptr) {
                *error = "标记通知已读失败: " + DescribeError(updated.Err());
            }
            return -1;
        }
        return updated.Value().changes;
    }
    // step2: 全部已读——一条 UPDATE 命中 (user_id, read_at) 索引前缀。
    if (all) {
        auto updated = m_db->Exec(
            "UPDATE notifications SET read_at = ?"
            " WHERE user_id = ? AND read_at = 0",
            now, user_id);
        if (!updated) {
            if (error != nullptr) {
                *error = "标记通知已读失败: " + DescribeError(updated.Err());
            }
            return -1;
        }
        return updated.Value().changes;
    }
    // step2: 按 id 标记——只拼库内自增 id（无用户输入），最多 100 个。
    std::string id_list;
    id_list.reserve(ids.size() * 8);
    std::size_t valid = 0;
    for (const std::int64_t id : ids) {
        if (id <= 0) {
            continue;
        }
        if (valid++ != 0) {
            id_list += ',';
        }
        id_list += std::to_string(id);
    }
    if (valid == 0) {
        return 0;
    }
    // WHERE 里带 user_id：ids 混入别人的通知时天然 0 行受影响。
    auto updated = m_db->Exec(
        "UPDATE notifications SET read_at = ? WHERE user_id = ?"
        " AND read_at = 0 AND id IN (" + id_list + ")",
        now, user_id);
    if (!updated) {
        if (error != nullptr) {
            *error = "标记通知已读失败: " + DescribeError(updated.Err());
        }
        return -1;
    }
    return updated.Value().changes;
}

// ---------------------------------------------------------------------------
// 举报 / 拉黑：reports / blocks
// ---------------------------------------------------------------------------

namespace {

/// @brief 合法举报目标类型。
bool IsValidReportTarget(std::string_view type) {
    return type == "video" || type == "comment" || type == "user";
}

/// @brief 读当前行（列顺序见 `Store::AddReport` / `FindReport` 的 SELECT）。
void ReadReportRow(go2cpp::storage::Rows* rows, StoredReport* out) {
    out->id = rows->Int64(0);
    out->reporter_id = rows->Int64(1);
    out->target_type = rows->Text(2);
    out->target_id = rows->Int64(3);
    out->reason = rows->Text(4);
    out->detail = rows->Text(5);
    out->status = rows->Text(6);
    out->created_at = rows->Int64(7);
}

}  // namespace

Store::ReportResult Store::AddReport(std::int64_t reporter_id,
                                     const std::string& target_type,
                                     std::int64_t target_id,
                                     const std::string& reason,
                                     const std::string& detail,
                                     StoredReport* out, std::string* error) {
    // step1: 参数校验——脏数据进库只能人工清理，宁可不写。
    if (reporter_id <= 0 || target_id <= 0 || reason.empty() ||
        !IsValidReportTarget(target_type)) {
        if (error != nullptr) {
            *error = "举报参数非法";
        }
        return ReportResult::kError;
    }
    if (!Ok()) {
        if (error != nullptr) {
            *error = "数据库尚未打开";
        }
        return ReportResult::kError;
    }
    const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
    // step2: 直接 INSERT，让唯一键决定是否重复——先查后插在并发下会双写。
    auto inserted = m_db->Exec(
        "INSERT INTO reports(reporter_id, target_type, target_id, reason,"
        " detail, status, created_at) VALUES(?, ?, ?, ?, ?, 'pending', ?)",
        reporter_id, target_type, target_id, reason, detail, now);
    if (!inserted) {
        // step3: 唯一键冲突 = 已经举报过；把已有记录返回，接口保持幂等。
        if (inserted.Err().IsConstraint()) {
            if (FindReport(reporter_id, target_type, target_id, out, error)) {
                return ReportResult::kDuplicate;
            }
            if (error != nullptr && error->empty()) {
                *error = "查询已有举报失败";
            }
            return ReportResult::kError;
        }
        if (error != nullptr) {
            *error = "写举报失败: " + DescribeError(inserted.Err());
        }
        return ReportResult::kError;
    }
    if (out != nullptr) {
        out->id = inserted.Value().last_insert_row_id;
        out->reporter_id = reporter_id;
        out->target_type = target_type;
        out->target_id = target_id;
        out->reason = reason;
        out->detail = detail;
        out->status = "pending";
        out->created_at = now;
    }
    return ReportResult::kCreated;
}

bool Store::FindReport(std::int64_t reporter_id,
                       const std::string& target_type, std::int64_t target_id,
                       StoredReport* out, std::string* error) {
    if (!Ok()) {
        if (error != nullptr) {
            *error = "数据库尚未打开";
        }
        return false;
    }
    auto rows = m_db->Query(
        "SELECT id, reporter_id, target_type, target_id, reason, detail,"
        " status, created_at FROM reports"
        " WHERE reporter_id = ? AND target_type = ? AND target_id = ?",
        reporter_id, target_type, target_id);
    if (!rows) {
        if (error != nullptr) {
            *error = "查询举报失败: " + DescribeError(rows.Err());
        }
        return false;
    }
    if (!rows.Value().Next()) {
        return false;
    }
    if (out != nullptr) {
        ReadReportRow(&rows.Value(), out);
    }
    return true;
}

int Store::BlockUser(std::int64_t user_id, std::int64_t blocked_id,
                     std::string* error) {
    if (user_id <= 0 || blocked_id <= 0 || user_id == blocked_id) {
        if (error != nullptr) {
            *error = "拉黑参数非法";
        }
        return -1;
    }
    if (!Ok()) {
        if (error != nullptr) {
            *error = "数据库尚未打开";
        }
        return -1;
    }
    auto inserted = m_db->Exec(
        "INSERT OR IGNORE INTO blocks(user_id, blocked_id, created_at)"
        " VALUES(?, ?, ?)",
        user_id, blocked_id, static_cast<std::int64_t>(std::time(nullptr)));
    if (!inserted) {
        if (error != nullptr) {
            *error = "拉黑失败: " + DescribeError(inserted.Err());
        }
        return -1;
    }
    return inserted.Value().changes > 0 ? 1 : 0;
}

bool Store::UnblockUser(std::int64_t user_id, std::int64_t blocked_id,
                        std::string* error) {
    if (!Ok()) {
        if (error != nullptr) {
            *error = "数据库尚未打开";
        }
        return false;
    }
    auto deleted = m_db->Exec(
        "DELETE FROM blocks WHERE user_id = ? AND blocked_id = ?", user_id,
        blocked_id);
    if (!deleted) {
        if (error != nullptr) {
            *error = "取消拉黑失败: " + DescribeError(deleted.Err());
        }
        return false;
    }
    return true;
}

bool Store::IsBlocked(std::int64_t user_id, std::int64_t blocked_id,
                      bool* out, std::string* error) {
    if (!Ok()) {
        if (error != nullptr) {
            *error = "数据库尚未打开";
        }
        return false;
    }
    auto rows = m_db->Query(
        "SELECT 1 FROM blocks WHERE user_id = ? AND blocked_id = ? LIMIT 1",
        user_id, blocked_id);
    if (!rows) {
        if (error != nullptr) {
            *error = "查询拉黑状态失败: " + DescribeError(rows.Err());
        }
        return false;
    }
    *out = rows.Value().Next();
    if (rows.Value().Err()) {
        if (error != nullptr) {
            *error = "遍历拉黑状态失败: " + DescribeError(rows.Value().Err());
        }
        return false;
    }
    return true;
}

bool Store::ListBlocks(std::int64_t user_id,
                       std::vector<StoredBlock>* out, std::string* error) {
    if (!Ok()) {
        if (error != nullptr) {
            *error = "数据库尚未打开";
        }
        return false;
    }
    auto rows = m_db->Query(
        "SELECT b.id, b.user_id, b.blocked_id, COALESCE(u.name, ''),"
        " b.created_at FROM blocks b LEFT JOIN users u ON u.id = b.blocked_id"
        " WHERE b.user_id = ? ORDER BY b.created_at DESC, b.id DESC",
        user_id);
    if (!rows) {
        if (error != nullptr) {
            *error = "查询黑名单失败: " + DescribeError(rows.Err());
        }
        return false;
    }
    while (rows.Value().Next()) {
        StoredBlock item;
        item.id = rows.Value().Int64(0);
        item.user_id = rows.Value().Int64(1);
        item.blocked_id = rows.Value().Int64(2);
        item.blocked_name = rows.Value().Text(3);
        item.created_at = rows.Value().Int64(4);
        out->push_back(std::move(item));
    }
    if (rows.Value().Err()) {
        if (error != nullptr) {
            *error = "遍历黑名单失败: " + DescribeError(rows.Value().Err());
        }
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// C6 搜索
// ---------------------------------------------------------------------------

bool Store::SearchUsers(const std::string& query, std::int64_t limit,
                        std::int64_t offset, std::vector<StoredUser>* out,
                        std::string* error, std::int64_t viewer_id) {
    if (!Ok()) {
        if (error != nullptr) {
            *error = "数据库尚未打开";
        }
        return false;
    }
    const std::string pattern = LikePattern(query);
    std::string sql =
        "SELECT id, name, created_at FROM users"
        " WHERE lower(name) LIKE lower(?) ESCAPE '\\'";
    if (viewer_id > 0) {
        sql += " AND id NOT IN (SELECT blocked_id FROM blocks WHERE user_id = ?)";
        sql += " ORDER BY id DESC LIMIT ? OFFSET ?";
        auto rows = m_db->Query(sql, pattern, viewer_id, limit, offset);
        if (!rows) {
            if (error != nullptr) {
                *error = "搜索用户失败: " + DescribeError(rows.Err());
            }
            return false;
        }
        while (rows.Value().Next()) {
            StoredUser user;
            user.id = rows.Value().Int64(0);
            user.name = rows.Value().Text(1);
            user.created_at = rows.Value().Int64(2);
            out->push_back(std::move(user));
        }
        if (rows.Value().Err()) {
            if (error != nullptr) {
                *error = "遍历用户失败: " + DescribeError(rows.Value().Err());
            }
            return false;
        }
        return true;
    }
    sql += " ORDER BY id DESC LIMIT ? OFFSET ?";
    auto rows = m_db->Query(sql, pattern, limit, offset);
    if (!rows) {
        if (error != nullptr) {
            *error = "搜索用户失败: " + DescribeError(rows.Err());
        }
        return false;
    }
    while (rows.Value().Next()) {
        StoredUser user;
        user.id = rows.Value().Int64(0);
        user.name = rows.Value().Text(1);
        user.created_at = rows.Value().Int64(2);
        out->push_back(std::move(user));
    }
    if (rows.Value().Err()) {
        if (error != nullptr) {
            *error = "遍历用户失败: " + DescribeError(rows.Value().Err());
        }
        return false;
    }
    return true;
}

bool Store::ListAllVideos(std::vector<StoredVideo>* out, std::string* error) {
    if (!Ok()) {
        if (error != nullptr) {
            *error = "数据库尚未打开";
        }
        return false;
    }
    auto rows = m_db->Query(std::string("SELECT ") + kVideoColumns +
                            " FROM videos ORDER BY id ASC");
    if (!rows) {
        if (error != nullptr) {
            *error = "查询视频失败: " + DescribeError(rows.Err());
        }
        return false;
    }
    while (rows.Value().Next()) {
        StoredVideo video;
        ReadVideoRow(&rows.Value(), &video);
        out->push_back(std::move(video));
    }
    if (rows.Value().Err()) {
        if (error != nullptr) {
            *error = "遍历视频失败: " + DescribeError(rows.Value().Err());
        }
        return false;
    }
    return true;
}

bool Store::SearchVideos(const std::string& query, std::int64_t limit,
                         std::int64_t offset, std::vector<StoredVideo>* out,
                         std::string* error, std::int64_t viewer_id) {
    if (!Ok()) {
        if (error != nullptr) {
            *error = "数据库尚未打开";
        }
        return false;
    }
    const std::string pattern = LikePattern(query);
    std::string sql = std::string("SELECT ") + kVideoColumns +
                      " FROM videos WHERE lower(title) LIKE lower(?)"
                      " ESCAPE '\\'";
    if (viewer_id > 0) {
        sql += " AND user_id NOT IN"
               " (SELECT blocked_id FROM blocks WHERE user_id = ?)";
        sql += " ORDER BY id DESC LIMIT ? OFFSET ?";
        auto rows = m_db->Query(sql, pattern, viewer_id, limit, offset);
        if (!rows) {
            if (error != nullptr) {
                *error = "搜索视频失败: " + DescribeError(rows.Err());
            }
            return false;
        }
        while (rows.Value().Next()) {
            StoredVideo video;
            ReadVideoRow(&rows.Value(), &video);
            out->push_back(std::move(video));
        }
        if (rows.Value().Err()) {
            if (error != nullptr) {
                *error = "遍历视频失败: " + DescribeError(rows.Value().Err());
            }
            return false;
        }
        return true;
    }
    sql += " ORDER BY id DESC LIMIT ? OFFSET ?";
    auto rows = m_db->Query(sql, pattern, limit, offset);
    if (!rows) {
        if (error != nullptr) {
            *error = "搜索视频失败: " + DescribeError(rows.Err());
        }
        return false;
    }
    while (rows.Value().Next()) {
        StoredVideo video;
        ReadVideoRow(&rows.Value(), &video);
        out->push_back(std::move(video));
    }
    if (rows.Value().Err()) {
        if (error != nullptr) {
            *error = "遍历视频失败: " + DescribeError(rows.Value().Err());
        }
        return false;
    }
    return true;
}

// ---- 搜索增强（第 11 轮）：历史 / 热搜 / 建议 ----

bool Store::RecordSearchHistory(std::int64_t user_id,
                                const std::string& keyword,
                                std::string* error) {
    if (!Ok()) {
        if (error != nullptr) {
            *error = "数据库尚未打开";
        }
        return false;
    }
    const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
    // 用 INSERT OR REPLACE 而非 ON CONFLICT DO UPDATE：SQLite 的 REPLACE 在
    // UNIQUE(user_id, keyword) 冲突时删除旧行再插入，新行 id 更大。created_at
    // 只有秒级精度，同一秒内「先搜 A 再搜 B 再搜 A」用 UPDATE 无法体现最近性；
    // 靠 id 递增后，`ORDER BY created_at DESC, id DESC` 在秒内也严格正确。
    // 行数仍是「用户数 × 不同关键词数」，历史列表天然去重。
    auto executed = m_db->Exec(
        "INSERT OR REPLACE INTO search_history(user_id, keyword, created_at) "
        "VALUES(?, ?, ?)",
        user_id, keyword, now);
    if (!executed) {
        if (error != nullptr) {
            *error = "记录搜索历史失败: " + DescribeError(executed.Err());
        }
        return false;
    }
    return true;
}

bool Store::ListSearchHistory(std::int64_t user_id, std::int64_t limit,
                              std::vector<SearchHistoryEntry>* out,
                              std::string* error) {
    if (!Ok()) {
        if (error != nullptr) {
            *error = "数据库尚未打开";
        }
        return false;
    }
    auto rows = m_db->Query(
        "SELECT keyword, created_at FROM search_history WHERE user_id = ? "
        "ORDER BY created_at DESC, id DESC LIMIT ?",
        user_id, limit);
    if (!rows) {
        if (error != nullptr) {
            *error = "查询搜索历史失败: " + DescribeError(rows.Err());
        }
        return false;
    }
    while (rows.Value().Next()) {
        SearchHistoryEntry item;
        item.keyword = rows.Value().Text(0);
        item.created_at = rows.Value().Int64(1);
        out->push_back(std::move(item));
    }
    if (rows.Value().Err()) {
        if (error != nullptr) {
            *error = "遍历搜索历史失败: " + DescribeError(rows.Value().Err());
        }
        return false;
    }
    return true;
}

int Store::DeleteSearchHistory(std::int64_t user_id,
                               const std::string& keyword,
                               std::string* error) {
    if (!Ok()) {
        if (error != nullptr) {
            *error = "数据库尚未打开";
        }
        return -1;
    }
    auto executed = keyword.empty()
                        ? m_db->Exec(
                              "DELETE FROM search_history WHERE user_id = ?",
                              user_id)
                        : m_db->Exec(
                              "DELETE FROM search_history "
                              "WHERE user_id = ? AND keyword = ?",
                              user_id, keyword);
    if (!executed) {
        if (error != nullptr) {
            *error = "删除搜索历史失败: " + DescribeError(executed.Err());
        }
        return -1;
    }
    return executed.Value().changes;
}

bool Store::SearchHotKeywords(std::int64_t since, std::int64_t limit,
                              std::vector<SearchHotKeyword>* out,
                              std::string* error) {
    if (!Ok()) {
        if (error != nullptr) {
            *error = "数据库尚未打开";
        }
        return false;
    }
    // 单条 GROUP BY：最近 7 天窗口内按关键词聚合；同次数按最近一次搜索时间
    // 倒序，保证结果稳定（不依赖 SQLite 的哈希分组顺序）。
    auto rows = m_db->Query(
        "SELECT keyword, COUNT(*) AS hits FROM search_history "
        "WHERE created_at >= ? GROUP BY keyword "
        "ORDER BY hits DESC, MAX(created_at) DESC LIMIT ?",
        since, limit);
    if (!rows) {
        if (error != nullptr) {
            *error = "查询热搜失败: " + DescribeError(rows.Err());
        }
        return false;
    }
    while (rows.Value().Next()) {
        SearchHotKeyword item;
        item.keyword = rows.Value().Text(0);
        item.count = rows.Value().Int64(1);
        out->push_back(std::move(item));
    }
    if (rows.Value().Err()) {
        if (error != nullptr) {
            *error = "遍历热搜失败: " + DescribeError(rows.Value().Err());
        }
        return false;
    }
    return true;
}

bool Store::SearchSuggest(const std::string& prefix, std::int64_t limit,
                          std::vector<std::string>* out, std::string* error) {
    if (!Ok()) {
        if (error != nullptr) {
            *error = "数据库尚未打开";
        }
        return false;
    }
    if (prefix.empty() || limit <= 0) {
        return true;
    }
    const std::string pattern = LikePrefixPattern(prefix);
    // 并集去重：三个来源共用一个小向量做线性判重（limit <= 20，无需哈希）。
    auto add_unique = [out, limit](const std::string& text) {
        if (text.empty() || static_cast<std::int64_t>(out->size()) >= limit) {
            return;
        }
        for (const std::string& existing : *out) {
            if (existing == text) {
                return;
            }
        }
        out->push_back(text);
    };
    // 来源 1：热搜（历史聚合，前缀匹配，次数降序）。
    auto hot = m_db->Query(
        "SELECT keyword FROM search_history WHERE keyword LIKE ? ESCAPE '\\' "
        "GROUP BY keyword ORDER BY COUNT(*) DESC, MAX(created_at) DESC LIMIT ?",
        pattern, limit);
    if (!hot) {
        if (error != nullptr) {
            *error = "查询建议（热搜）失败: " + DescribeError(hot.Err());
        }
        return false;
    }
    while (hot.Value().Next()) {
        add_unique(hot.Value().Text(0));
    }
    if (hot.Value().Err()) {
        if (error != nullptr) {
            *error = "遍历建议（热搜）失败: " + DescribeError(hot.Value().Err());
        }
        return false;
    }
    // 来源 2：用户昵称（前缀匹配，最近注册优先）。
    auto users = m_db->Query(
        "SELECT name FROM users WHERE name LIKE ? ESCAPE '\\' "
        "ORDER BY id DESC LIMIT ?",
        pattern, limit);
    if (!users) {
        if (error != nullptr) {
            *error = "查询建议（用户）失败: " + DescribeError(users.Err());
        }
        return false;
    }
    while (users.Value().Next()) {
        add_unique(users.Value().Text(0));
    }
    if (users.Value().Err()) {
        if (error != nullptr) {
            *error = "遍历建议（用户）失败: " + DescribeError(users.Value().Err());
        }
        return false;
    }
    // 来源 3：话题（从视频标题提取，复用唯一提取点 ExtractTags）。
    // 先用 `%q%` 让 SQLite 过滤掉绝大多数标题，再在内存里做前缀判定。
    const std::string contains = "%" + pattern.substr(0, pattern.size() - 1) + "%";
    auto videos = m_db->Query(
        "SELECT title FROM videos WHERE title LIKE ? ESCAPE '\\' LIMIT ?",
        contains, limit * 20);
    if (!videos) {
        if (error != nullptr) {
            *error = "查询建议（话题）失败: " + DescribeError(videos.Err());
        }
        return false;
    }
    while (videos.Value().Next()) {
        for (const std::string& tag : ExtractTags(videos.Value().Text(0))) {
            if (tag.compare(0, prefix.size(), prefix) == 0) {
                add_unique(tag);
            }
        }
    }
    if (videos.Value().Err()) {
        if (error != nullptr) {
            *error = "遍历建议（话题）失败: " + DescribeError(videos.Value().Err());
        }
        return false;
    }
    return true;
}

// ---- C7 直播：rooms + room_viewers ----

namespace {

/// @brief rooms 查询的固定列顺序（与 `ReadRoomRow` 的下标对应）。
constexpr const char* kRoomColumns =
    "id, user_id, title, status, viewer_count, created_at";

/// @brief 读当前行（列顺序必须为 `kRoomColumns`）。
void ReadRoomRow(go2cpp::storage::Rows* rows, StoredRoom* out) {
    out->id = rows->Int64(0);
    out->user_id = rows->Int64(1);
    out->title = rows->Text(2);
    out->status = static_cast<int>(rows->Int64(3));
    out->viewer_count = rows->Int64(4);
    out->created_at = rows->Int64(5);
}

}  // namespace

std::int64_t Store::CreateRoom(std::int64_t user_id, const std::string& title,
                               std::string* error) {
    if (!Ok()) {
        if (error != nullptr) {
            *error = "数据库尚未打开";
        }
        return -1;
    }
    const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
    auto inserted = m_db->Exec(
        "INSERT INTO rooms(user_id, title, status, viewer_count, created_at) "
        "VALUES(?, ?, 0, 0, ?)",
        user_id, title, now);
    if (!inserted) {
        if (error != nullptr) {
            *error = "创建房间失败: " + DescribeError(inserted.Err());
        }
        return -1;
    }
    return inserted.Value().last_insert_row_id;
}

bool Store::FindRoom(std::int64_t id, StoredRoom* out, std::string* error) {
    if (!Ok()) {
        if (error != nullptr) {
            *error = "数据库尚未打开";
        }
        return false;
    }
    auto found = m_db->Query(
        std::string("SELECT ") + kRoomColumns + " FROM rooms WHERE id = ?", id);
    if (!found) {
        if (error != nullptr) {
            *error = "查询房间失败: " + DescribeError(found.Err());
        }
        return false;
    }
    if (!found.Value().Next()) {
        if (found.Value().Err()) {
            if (error != nullptr) {
                *error = "遍历房间失败: " + DescribeError(found.Value().Err());
            }
        }
        return false;
    }
    ReadRoomRow(&found.Value(), out);
    return true;
}

bool Store::ListRooms(int status, std::vector<StoredRoom>* out,
                      std::string* error) {
    if (!Ok()) {
        if (error != nullptr) {
            *error = "数据库尚未打开";
        }
        return false;
    }
    auto rows = status < 0
                    ? m_db->Query(std::string("SELECT ") + kRoomColumns +
                                  " FROM rooms ORDER BY created_at DESC, id DESC")
                    : m_db->Query(std::string("SELECT ") + kRoomColumns +
                                      " FROM rooms WHERE status = ?"
                                      " ORDER BY created_at DESC, id DESC",
                                  static_cast<std::int64_t>(status));
    if (!rows) {
        if (error != nullptr) {
            *error = "查询房间列表失败: " + DescribeError(rows.Err());
        }
        return false;
    }
    while (rows.Value().Next()) {
        StoredRoom room;
        ReadRoomRow(&rows.Value(), &room);
        out->push_back(std::move(room));
    }
    if (rows.Value().Err()) {
        if (error != nullptr) {
            *error = "遍历房间失败: " + DescribeError(rows.Value().Err());
        }
        return false;
    }
    return true;
}

int Store::UpdateRoomStatus(std::int64_t id, std::int64_t user_id, int status,
                            std::string* error) {
    if (!Ok()) {
        if (error != nullptr) {
            *error = "数据库尚未打开";
        }
        return -1;
    }
    auto updated = m_db->Exec(
        "UPDATE rooms SET status = ? WHERE id = ? AND user_id = ?", status, id,
        user_id);
    if (!updated) {
        if (error != nullptr) {
            *error = "更新房间状态失败: " + DescribeError(updated.Err());
        }
        return -1;
    }
    return updated.Value().changes;
}

int Store::EnterRoom(std::int64_t id, std::int64_t user_id, std::string* error) {
    if (!Ok()) {
        if (error != nullptr) {
            *error = "数据库尚未打开";
        }
        return -1;
    }
    auto exists = m_db->Query("SELECT 1 FROM rooms WHERE id = ?", id);
    if (!exists) {
        if (error != nullptr) {
            *error = "查询房间失败: " + DescribeError(exists.Err());
        }
        return -1;
    }
    if (!exists.Value().Next()) {
        if (error != nullptr) {
            *error = "房间不存在";
        }
        return -1;
    }

    go2cpp::storage::Transaction tx(*m_db);
    const go2cpp::storage::Error began = tx.Begin();
    if (!began.ok()) {
        if (error != nullptr) {
            *error = "开启事务失败: " + began.ToString();
        }
        return -1;
    }
    const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
    auto inserted = m_db->Exec(
        "INSERT OR IGNORE INTO room_viewers(room_id, user_id, created_at) "
        "VALUES(?, ?, ?)",
        id, user_id, now);
    if (!inserted) {
        if (error != nullptr) {
            *error = "记录进房失败: " + DescribeError(inserted.Err());
        }
        return -1;
    }
    if (inserted.Value().changes == 0) {
        const go2cpp::storage::Error committed = tx.Commit();
        if (!committed.ok()) {
            if (error != nullptr) {
                *error = "提交事务失败: " + committed.ToString();
            }
            return -1;
        }
        return 0;
    }
    auto bumped = m_db->Exec(
        "UPDATE rooms SET viewer_count = viewer_count + 1 WHERE id = ?", id);
    if (!bumped) {
        if (error != nullptr) {
            *error = "更新观众数失败: " + DescribeError(bumped.Err());
        }
        return -1;
    }
    const go2cpp::storage::Error committed = tx.Commit();
    if (!committed.ok()) {
        if (error != nullptr) {
            *error = "提交事务失败: " + committed.ToString();
        }
        return -1;
    }
    return 1;
}

int Store::LeaveRoom(std::int64_t id, std::int64_t user_id, std::string* error) {
    if (!Ok()) {
        if (error != nullptr) {
            *error = "数据库尚未打开";
        }
        return -1;
    }
    go2cpp::storage::Transaction tx(*m_db);
    const go2cpp::storage::Error began = tx.Begin();
    if (!began.ok()) {
        if (error != nullptr) {
            *error = "开启事务失败: " + began.ToString();
        }
        return -1;
    }
    auto removed = m_db->Exec(
        "DELETE FROM room_viewers WHERE room_id = ? AND user_id = ?", id,
        user_id);
    if (!removed) {
        if (error != nullptr) {
            *error = "删除进房记录失败: " + DescribeError(removed.Err());
        }
        return -1;
    }
    if (removed.Value().changes == 0) {
        const go2cpp::storage::Error committed = tx.Commit();
        if (!committed.ok()) {
            if (error != nullptr) {
                *error = "提交事务失败: " + committed.ToString();
            }
            return -1;
        }
        return 0;
    }
    auto lowered = m_db->Exec(
        "UPDATE rooms SET viewer_count = MAX(0, viewer_count - 1) WHERE id = ?",
        id);
    if (!lowered) {
        if (error != nullptr) {
            *error = "更新观众数失败: " + DescribeError(lowered.Err());
        }
        return -1;
    }
    const go2cpp::storage::Error committed = tx.Commit();
    if (!committed.ok()) {
        if (error != nullptr) {
            *error = "提交事务失败: " + committed.ToString();
        }
        return -1;
    }
    return 1;
}

// ---- 分片上传（断点续传）：upload_sessions 持久化 --------------------------


std::int64_t Store::AppendLiveMessage(std::int64_t room_id,
                                     std::int64_t user_id,
                                     const std::string& content,
                                     const std::string& kind,
                                     std::string* error) {
    // 写前清 ORM 语句缓存：任何未 finalize 的读游标都会让本连接停留在
    // 旧快照，写时直接报 SQLITE_BUSY_SNAPSHOT（实测跨进程拉黑后必现）。
    ResetStatements();
    if (!Ok()) {
        if (error != nullptr) {
            *error = "数据库尚未打开";
        }
        return -1;
    }
    const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
    auto inserted = m_db->Exec(
        "INSERT INTO live_messages(room_id, user_id, content, kind, created_at)"
        " VALUES(?, ?, ?, ?, ?)",
        room_id, user_id, content, kind, now);
    if (!inserted) {
        if (error != nullptr) {
            *error = "写入直播消息失败: " + DescribeError(inserted.Err());
        }
        return -1;
    }
    return inserted.Value().last_insert_row_id;
}

bool Store::ListLiveMessages(std::int64_t room_id, std::int64_t after_id,
                             std::int64_t limit, std::int64_t owner_id,
                             std::vector<StoredLiveMessage>* out,
                             std::string* error) {
    if (!Ok()) {
        if (error != nullptr) {
            *error = "数据库尚未打开";
        }
        return false;
    }
    const std::string base =
        "SELECT m.id, m.room_id, m.user_id, COALESCE(u.name, ''), m.content,"
        " m.kind, m.created_at FROM live_messages m"
        " LEFT JOIN users u ON u.id = m.user_id"
        " WHERE m.room_id = ? AND m.id > ?";
    auto rows = owner_id > 0
                    ? m_db->Query(base +
                                      " AND m.user_id NOT IN"
                                      " (SELECT blocked_id FROM blocks"
                                      " WHERE user_id = ?)"
                                      " ORDER BY m.id ASC LIMIT ?",
                                  room_id, after_id, owner_id, limit)
                    : m_db->Query(base + " ORDER BY m.id ASC LIMIT ?", room_id,
                                  after_id, limit);
    if (!rows) {
        if (error != nullptr) {
            *error = "查询直播消息失败: " + DescribeError(rows.Err());
        }
        return false;
    }
    while (rows.Value().Next()) {
        StoredLiveMessage message;
        message.id = rows.Value().Int64(0);
        message.room_id = rows.Value().Int64(1);
        message.user_id = rows.Value().Int64(2);
        message.user_name = rows.Value().Text(3);
        message.content = rows.Value().Text(4);
        message.kind = rows.Value().Text(5);
        message.created_at = rows.Value().Int64(6);
        out->push_back(std::move(message));
    }
    if (rows.Value().Err()) {
        if (error != nullptr) {
            *error = "遍历直播消息失败: " + DescribeError(rows.Value().Err());
        }
        return false;
    }
    return true;
}

bool Store::ListGifts(std::vector<StoredGift>* out, std::string* error) {
    if (!Ok()) {
        if (error != nullptr) {
            *error = "数据库尚未打开";
        }
        return false;
    }
    auto rows = m_db->Query(
        "SELECT id, name, price_coins, icon FROM gifts ORDER BY id ASC");
    if (!rows) {
        if (error != nullptr) {
            *error = "查询礼物列表失败: " + DescribeError(rows.Err());
        }
        return false;
    }
    while (rows.Value().Next()) {
        StoredGift gift;
        gift.id = rows.Value().Int64(0);
        gift.name = rows.Value().Text(1);
        gift.price_coins = rows.Value().Int64(2);
        gift.icon = rows.Value().Text(3);
        out->push_back(std::move(gift));
    }
    if (rows.Value().Err()) {
        if (error != nullptr) {
            *error = "遍历礼物列表失败: " + DescribeError(rows.Value().Err());
        }
        return false;
    }
    return true;
}

bool Store::FindGift(std::int64_t id, StoredGift* out, std::string* error) {
    if (!Ok()) {
        if (error != nullptr) {
            *error = "数据库尚未打开";
        }
        return false;
    }
    auto found = m_db->Query(
        "SELECT id, name, price_coins, icon FROM gifts WHERE id = ?", id);
    if (!found) {
        if (error != nullptr) {
            *error = "查询礼物失败: " + DescribeError(found.Err());
        }
        return false;
    }
    if (!found.Value().Next()) {
        if (found.Value().Err()) {
            if (error != nullptr) {
                *error = "遍历礼物失败: " + DescribeError(found.Value().Err());
            }
        }
        return false;
    }
    out->id = found.Value().Int64(0);
    out->name = found.Value().Text(1);
    out->price_coins = found.Value().Int64(2);
    out->icon = found.Value().Text(3);
    return true;
}

bool Store::FindUserCoins(std::int64_t user_id, std::int64_t* out,
                          std::string* error) {
    if (!Ok()) {
        if (error != nullptr) {
            *error = "数据库尚未打开";
        }
        return false;
    }
    auto rows = m_db->Query("SELECT coins FROM users WHERE id = ?", user_id);
    if (!rows) {
        if (error != nullptr) {
            *error = "查询余额失败: " + DescribeError(rows.Err());
        }
        return false;
    }
    if (!rows.Value().Next()) {
        if (rows.Value().Err()) {
            if (error != nullptr) {
                *error = "遍历余额失败: " + DescribeError(rows.Value().Err());
            }
        }
        return false;
    }
    *out = rows.Value().Int64(0);
    return true;
}

int Store::SendGift(std::int64_t room_id, std::int64_t sender_id,
                    const StoredGift& gift, std::int64_t count,
                    const std::string& content, std::int64_t* out_coins,
                    std::int64_t* out_message_id, std::string* error) {
    if (!Ok()) {
        if (error != nullptr) {
            *error = "数据库尚未打开";
        }
        return -1;
    }
    if (gift.id <= 0 || gift.price_coins <= 0 || count <= 0 || count > 99) {
        if (error != nullptr) {
            *error = "礼物参数非法";
        }
        return -1;
    }
    // 事务前清 ORM 游标：否则 BEGIN IMMEDIATE 会带着旧读快照升级，
    // WAL 下返回 SQLITE_BUSY_SNAPSHOT，表现为「送礼偶发 500」。
    ResetStatements();
    const std::int64_t total = gift.price_coins * count;
    const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
    // step1: 显式事务——扣款、记录、聊天消息必须同生共死；任何一步失败
    //        都靠 Transaction 析构回滚，绝不允许「扣了钱没记录」。
    go2cpp::storage::Transaction tx(*m_db);
    if (auto begun = tx.Begin(); begun) {
        if (error != nullptr) {
            *error = "开始事务失败: " + DescribeError(begun);
        }
        return -1;
    }
    // step2: 条件扣款是余额校验的唯一依据：WHERE coins >= total 让
    //        「查余额→再扣」的竞态窗口消失，同时省掉一次 SELECT。
    auto debited = m_db->Exec(
        "UPDATE users SET coins = coins - ? WHERE id = ? AND coins >= ?", total,
        sender_id, total);
    if (!debited) {
        if (error != nullptr) {
            *error = "扣减余额失败: " + DescribeError(debited.Err());
        }
        return -1;
    }
    if (debited.Value().changes == 0) {
        (void)tx.Rollback();
        if (error != nullptr) {
            *error = "余额不足";
        }
        return 0;
    }
    // step3: 写礼物记录（榜单数据源）。
    auto record = m_db->Exec(
        "INSERT INTO gift_records(room_id, sender_id, gift_id, count, created_at)"
        " VALUES(?, ?, ?, ?, ?)",
        room_id, sender_id, gift.id, count, now);
    if (!record) {
        if (error != nullptr) {
            *error = "写入礼物记录失败: " + DescribeError(record.Err());
        }
        return -1;
    }
    // step4: 聊天消息与礼物记录同一事务，轮询端看到一个原子事件。
    auto message = m_db->Exec(
        "INSERT INTO live_messages(room_id, user_id, content, kind, created_at)"
        " VALUES(?, ?, ?, 'gift', ?)",
        room_id, sender_id, content, now);
    if (!message) {
        if (error != nullptr) {
            *error = "写入礼物消息失败: " + DescribeError(message.Err());
        }
        return -1;
    }
    // step5: 事务内读回余额，保证返回值与已提交状态一致。
    auto coins = m_db->Query("SELECT coins FROM users WHERE id = ?", sender_id);
    if (!coins) {
        if (error != nullptr) {
            *error = "查询余额失败: " + DescribeError(coins.Err());
        }
        return -1;
    }
    if (!coins.Value().Next()) {
        if (error != nullptr) {
            *error = "用户不存在";
        }
        return -1;
    }
    const std::int64_t left = coins.Value().Int64(0);
    if (auto committed = tx.Commit(); committed) {
        if (error != nullptr) {
            *error = "提交事务失败: " + DescribeError(committed);
        }
        return -1;
    }
    if (out_coins != nullptr) {
        *out_coins = left;
    }
    if (out_message_id != nullptr) {
        *out_message_id = message.Value().last_insert_row_id;
    }
    return 1;
}

bool Store::ListGiftRanking(std::int64_t room_id, std::int64_t limit,
                            std::vector<StoredGiftRank>* out,
                            std::string* error) {
    if (!Ok()) {
        if (error != nullptr) {
            *error = "数据库尚未打开";
        }
        return false;
    }
    // 单条 SQL：JOIN 礼物价目 + LEFT JOIN 昵称 + GROUP BY 聚合；
    // 不在服务层做「先查记录再逐人查用户」的 N+1。
    auto rows = m_db->Query(
        "SELECT r.sender_id, COALESCE(u.name, ''),"
        " SUM(g.price_coins * r.count) AS coins, SUM(r.count) AS gift_count"
        " FROM gift_records r JOIN gifts g ON g.id = r.gift_id"
        " LEFT JOIN users u ON u.id = r.sender_id"
        " WHERE r.room_id = ? GROUP BY r.sender_id"
        " ORDER BY coins DESC, r.sender_id ASC LIMIT ?",
        room_id, limit);
    if (!rows) {
        if (error != nullptr) {
            *error = "查询贡献榜失败: " + DescribeError(rows.Err());
        }
        return false;
    }
    while (rows.Value().Next()) {
        StoredGiftRank rank;
        rank.user_id = rows.Value().Int64(0);
        rank.user_name = rows.Value().Text(1);
        rank.coins = rows.Value().Int64(2);
        rank.gift_count = rows.Value().Int64(3);
        out->push_back(std::move(rank));
    }
    if (rows.Value().Err()) {
        if (error != nullptr) {
            *error = "遍历贡献榜失败: " + DescribeError(rows.Value().Err());
        }
        return false;
    }
    return true;
}

bool Store::CreateUploadSession(const StoredUpload& session, std::string* error) {
    if (!Ok()) {
        if (error != nullptr) {
            *error = "数据库尚未打开";
        }
        return false;
    }
    auto inserted = m_db->Exec(
        "INSERT INTO upload_sessions(upload_id, user_id, path, title, total, "
        "received_bytes, created_at) VALUES(?, ?, ?, ?, ?, ?, ?)",
        session.upload_id, session.user_id, session.path, session.title,
        session.total, session.received_bytes, session.created_at);
    if (!inserted) {
        if (error != nullptr) {
            *error = "插入上传会话失败: " + DescribeError(inserted.Err());
        }
        return false;
    }
    return true;
}

bool Store::FindUploadSession(const std::string& upload_id, StoredUpload* out,
                              std::string* error) {
    if (!Ok()) {
        if (error != nullptr) {
            *error = "数据库尚未打开";
        }
        return false;
    }
    auto rows = m_db->Query(
        "SELECT upload_id, user_id, path, title, total, received_bytes, created_at "
        "FROM upload_sessions WHERE upload_id = ?",
        upload_id);
    if (!rows) {
        if (error != nullptr) {
            *error = "查询上传会话失败: " + DescribeError(rows.Err());
        }
        return false;
    }
    if (!rows.Value().Next()) {
        if (rows.Value().Err() && error != nullptr) {
            *error = "遍历上传会话失败: " + DescribeError(rows.Value().Err());
        }
        return false;
    }
    if (out != nullptr) {
        out->upload_id = rows.Value().Text(0);
        out->user_id = rows.Value().Int64(1);
        out->path = rows.Value().Text(2);
        out->title = rows.Value().Text(3);
        out->total = rows.Value().Int64(4);
        out->received_bytes = rows.Value().Int64(5);
        out->created_at = rows.Value().Int64(6);
    }
    return true;
}

bool Store::UpdateUploadProgress(const std::string& upload_id,
                                 std::int64_t received_bytes, std::string* error) {
    if (!Ok()) {
        if (error != nullptr) {
            *error = "数据库尚未打开";
        }
        return false;
    }
    auto updated = m_db->Exec(
        "UPDATE upload_sessions SET received_bytes = ? WHERE upload_id = ?",
        received_bytes, upload_id);
    if (!updated) {
        if (error != nullptr) {
            *error = "更新上传进度失败: " + DescribeError(updated.Err());
        }
        return false;
    }
    return true;
}

bool Store::DeleteUploadSession(const std::string& upload_id, std::string* error) {
    if (!Ok()) {
        if (error != nullptr) {
            *error = "数据库尚未打开";
        }
        return false;
    }
    auto removed =
        m_db->Exec("DELETE FROM upload_sessions WHERE upload_id = ?", upload_id);
    if (!removed) {
        if (error != nullptr) {
            *error = "删除上传会话失败: " + DescribeError(removed.Err());
        }
        return false;
    }
    return true;
}

}  // namespace douyin
