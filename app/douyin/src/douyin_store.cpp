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

#include <ctime>
#include <memory>
#include <string>
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
    "CREATE INDEX IF NOT EXISTS idx_follows_followee ON follows(followee_id);";

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

}  // namespace douyin
