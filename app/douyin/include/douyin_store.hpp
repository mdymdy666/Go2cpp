/**
 * @file douyin_store.hpp
 * @brief 抖音 P0 存储层：SQLite（WAL）+ users / follows 访问器。
 *
 * @details 复用 `go2cpp::storage::DB` + `storage::Session`（orm.hpp），不引入
 *          第三方库。users 表通过 ORM 宏映射（`Insert` 回填自增 id、`Find`
 *          按主键、`Query` 按 email）；follows 是复合主键，直接用参数化 SQL。
 *
 * @author douyin demo
 * @date 2026-11-03
 * @copyright MIT License
 */

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace go2cpp {
namespace storage {
class DB;
class Session;
}  // namespace storage
}  // namespace go2cpp

namespace douyin {

/// @brief 数据库里的一行用户（含密码散列与盐，只供内部使用）。
struct StoredUser {
    std::int64_t id{0};           ///< 自增主键。
    std::string name;             ///< 昵称。
    std::string email;            ///< 唯一邮箱。
    std::string password_hash;    ///< SHA-256(salt + password) 的 hex。
    std::string salt;             ///< 每用户随机 16B 的 hex。
    std::int64_t created_at{0};   ///< Unix 秒。
};

/// @brief `CreateUser` 的结果：成功 / email 冲突 / 其它错误。
enum class CreateUserResult {
    kOk = 0,      ///< 插入成功。
    kConflict = 1,///< email 已存在（UNIQUE 约束）。
    kError = 2,   ///< 其它数据库错误。
};

/// @brief SQLite 连接 + 建表 + users/follows 访问器。
/// @note 同一个 `Store` 由 HTTP 事件循环单线程调用；`DB`/`Session` 自身另有锁。
class Store {
   public:
    /// @brief 默认数据库路径（相对 app/douyin/ 工作目录）。
    static constexpr const char* kDefaultPath = "data/douyin.db";

    Store();
    ~Store();
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;

    /**
     * @brief 打开数据库并设置 WAL / busy_timeout。
     * @param path 文件路径；父目录需已存在（或由调用方创建）。
     * @param error 失败时写入错误文本。
     * @return 成功返回 true。
     */
    bool Open(const std::string& path, std::string* error);

    /**
     * @brief 建表 + 索引（`CREATE TABLE IF NOT EXISTS`），幂等。
     * @note 建表 SQL 与 REQUIREMENTS.md §4 一致。
     */
    bool EnsureSchema(std::string* error);

    /// @brief 连接是否已打开。
    bool Ok() const noexcept;

    /**
     * @brief 创建用户。
     * @param name 昵称。
     * @param email 唯一邮箱。
     * @param password_hash 密码散列（hex）。
     * @param salt 随机盐（hex）。
     * @param out 成功时填好 id/created_at（其余字段回填同一份输入）。
     * @param error 失败（kError）时写入错误文本。
     */
    CreateUserResult CreateUser(const std::string& name, const std::string& email,
                                const std::string& password_hash,
                                const std::string& salt, StoredUser* out,
                                std::string* error);

    /// @brief 按 email 查用户；找到返回 true，未找到返回 false 且 error 为空。
    bool FindUserByEmail(const std::string& email, StoredUser* out,
                         std::string* error);

    /// @brief 按 id 查用户；找到返回 true，未找到返回 false 且 error 为空。
    bool FindUserById(std::int64_t id, StoredUser* out, std::string* error);

    /// @brief 用户总数；出错返回 -1 并写 error。
    std::int64_t CountUsers(std::string* error);

    /// @brief 关注（幂等：`INSERT OR IGNORE`）。
    bool Follow(std::int64_t follower, std::int64_t followee, std::string* error);

    /// @brief 取关（幂等）。
    bool Unfollow(std::int64_t follower, std::int64_t followee, std::string* error);

    /// @brief 我关注的人（按关注时间倒序）。
    bool Following(std::int64_t id, std::vector<std::int64_t>* out,
                   std::string* error);

    /// @brief 关注我的人（按关注时间倒序）。
    bool Followers(std::int64_t id, std::vector<std::int64_t>* out,
                   std::string* error);

   private:
    std::unique_ptr<go2cpp::storage::DB> m_db;
    std::unique_ptr<go2cpp::storage::Session> m_session;
};

}  // namespace douyin
