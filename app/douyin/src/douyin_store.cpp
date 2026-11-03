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

#include <cstdlib>
#include <ctime>
#include <memory>
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
    "CREATE INDEX IF NOT EXISTS idx_videos_created ON videos(created_at DESC);";

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
    if (!Ok()) {
        if (error != nullptr) {
            *error = "数据库尚未打开";
        }
        return false;
    }
    std::vector<StoredVideo> all;
    const std::string sql =
        std::string("SELECT ") + kVideoColumns + " FROM videos" + kVideoOrder;
    if (!LoadVideos(*m_db, sql, &all, error)) {
        return false;
    }
    PageVideos(all, cursor, limit, out, next_cursor);
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

}  // namespace douyin
