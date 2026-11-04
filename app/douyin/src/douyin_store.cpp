/**
 * @file douyin_store.cpp
 * @brief SQLite 存储实现：users 走 ORM，follows 走参数化 SQL。
 *
 * @author douyin demo
 * @date 2026-11-03
 * @copyright MIT License
 */

#include "douyin_store.hpp"

#include "go2cpp/storage/db.hpp"
#include "go2cpp/storage/orm.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <memory>
#include <random>
#include <string>
#include <string_view>
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

namespace {

/// @brief 建表 + 索引脚本（幂等）。
constexpr const char* kSchemaSql =
    "CREATE TABLE IF NOT EXISTS users ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  name TEXT NOT NULL,"
    "  email TEXT NOT NULL UNIQUE,"
    "  password_hash TEXT NOT NULL,"
    "  salt TEXT NOT NULL,"
    "  created_at INTEGER NOT NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_users_email ON users(email);"
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
    "CREATE TABLE IF NOT EXISTS comments ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  video_id INTEGER NOT NULL,"
    "  user_id INTEGER NOT NULL,"
    "  content TEXT NOT NULL,"
    "  created_at INTEGER NOT NULL,"
    "  parent_id INTEGER NOT NULL DEFAULT 0"
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
    "ON favorites(user_id, created_at DESC);";

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
    "id, user_id, title, url, like_count, comment_count, created_at";

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
}

/// @brief comments 查询的固定列顺序（与 `ReadCommentRow` 的下标对应）。
constexpr const char* kCommentColumns =
    "id, video_id, user_id, content, created_at, parent_id";

/// @brief 读当前行（列顺序必须为 `kCommentColumns`）。
void ReadCommentRow(go2cpp::storage::Rows* rows, StoredComment* out) {
    out->id = rows->Int64(0);
    out->video_id = rows->Int64(1);
    out->user_id = rows->Int64(2);
    out->content = rows->Text(3);
    out->created_at = rows->Int64(4);
    out->parent_id = rows->Int64(5);
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
    if (auto wal = m_db->SetJournalMode("WAL"); wal) {
        if (error != nullptr) {
            *error = "设置 WAL 失败: " + DescribeError(wal);
        }
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
    // step3: 列补齐后再建索引（放在建表脚本里会在旧库上报 no such column）。
    (void)m_db->Exec(
        "CREATE INDEX IF NOT EXISTS idx_comments_parent "
        "ON comments(video_id, parent_id, created_at)");
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
    if (!Ok()) {
        if (error != nullptr) {
            *error = "数据库尚未打开";
        }
        return -1;
    }
    auto inserted = m_db->Exec(
        "INSERT INTO videos(user_id, title, url, like_count, comment_count, "
        "created_at) VALUES(?, ?, ?, 0, 0, ?)",
        user_id, title, url, static_cast<std::int64_t>(std::time(nullptr)));
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
                     std::string* error) {
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
    const std::string sql =
        std::string("SELECT ") + kVideoColumns + " FROM videos";
    if (!LoadVideos(*m_db, sql, &all, error)) {
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
    const std::string sql = std::string("SELECT ") + kVideoColumns +
                            " FROM videos WHERE user_id IN (SELECT followee_id "
                            "FROM follows WHERE follower_id = ?)" +
                            kVideoOrder;
    if (!LoadVideos(*m_db, sql, &all, error, user_id)) {
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
        "WHERE f.user_id = ? ORDER BY f.created_at DESC, v.id DESC LIMIT ?",
        user_id, limit);
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
                         std::int64_t limit, std::vector<StoredComment>* out,
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
        !DecodeCommentCursor(cursor, &cursor_created, &cursor_id)) {
        if (error != nullptr) {
            *error = "cursor 非法";
        }
        return false;
    }

    std::string sql = std::string("SELECT ") + kCommentColumns +
                      " FROM comments WHERE video_id = ? AND parent_id = 0";
    if (has_cursor) {
        sql += " AND (created_at < ? OR (created_at = ? AND id < ?))";
    }
    sql += " ORDER BY created_at DESC, id DESC LIMIT ?";

    auto rows = has_cursor
                    ? m_db->Query(sql, video_id, cursor_created, cursor_created,
                                  cursor_id, limit + 1)
                    : m_db->Query(sql, video_id, limit + 1);
    if (!rows) {
        if (error != nullptr) {
            *error = "查询评论失败: " + DescribeError(rows.Err());
        }
        return false;
    }
    std::vector<StoredComment> page;
    while (rows.Value().Next()) {
        StoredComment comment;
        ReadCommentRow(&rows.Value(), &comment);
        page.push_back(std::move(comment));
    }
    if (rows.Value().Err()) {
        if (error != nullptr) {
            *error = "遍历评论失败: " + DescribeError(rows.Value().Err());
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

bool Store::ListReplies(std::int64_t video_id, std::int64_t parent_id,
                        std::int64_t limit, std::vector<StoredComment>* out,
                        std::string* error) {
    if (!Ok()) {
        if (error != nullptr) {
            *error = "数据库尚未打开";
        }
        return false;
    }
    if (limit <= 0) {
        return true;
    }
    auto rows = m_db->Query(std::string("SELECT ") + kCommentColumns +
                                " FROM comments WHERE video_id = ? AND parent_id = ?"
                                " ORDER BY created_at ASC, id ASC LIMIT ?",
                            video_id, parent_id, limit);
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
// C6 搜索
// ---------------------------------------------------------------------------

bool Store::SearchUsers(const std::string& query, std::int64_t limit,
                        std::int64_t offset, std::vector<StoredUser>* out,
                        std::string* error) {
    if (!Ok()) {
        if (error != nullptr) {
            *error = "数据库尚未打开";
        }
        return false;
    }
    const std::string pattern = LikePattern(query);
    auto rows = m_db->Query(
        "SELECT id, name, created_at FROM users"
        " WHERE lower(name) LIKE lower(?) ESCAPE '\\'"
        " ORDER BY id DESC LIMIT ? OFFSET ?",
        pattern, limit, offset);
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
                         std::string* error) {
    if (!Ok()) {
        if (error != nullptr) {
            *error = "数据库尚未打开";
        }
        return false;
    }
    const std::string pattern = LikePattern(query);
    auto rows = m_db->Query(
        std::string("SELECT ") + kVideoColumns +
            " FROM videos WHERE lower(title) LIKE lower(?) ESCAPE '\\'"
            " ORDER BY id DESC LIMIT ? OFFSET ?",
        pattern, limit, offset);
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
