/**
 * @file douyin_store.hpp
 * @brief 抖音 P0 存储层：SQLite（WAL）+ users / follows / videos 访问器。
 *
 * @details 复用 `go2cpp::storage::DB` + `storage::Session`（orm.hpp），不引入
 *          第三方库。users 表通过 ORM 宏映射（`Insert` 回填自增 id、`Find`
 *          按主键、`Query` 按 email）；follows 是复合主键、videos 带热度排序
 *          与游标分页，二者直接用参数化 SQL。
 *
 * ## videos 的排序与游标（C3/C4）
 *
 * - 热度：`score = like_count*2 + comment_count*3 + 0.1*created_at/3600`。
 *   需求公式为 `like_count*2 + comment_count*3 - age_hours*0.1`，其中
 *   `age_hours = (now - created_at)/3600`，展开后 `-now*0.1/3600` 是**每次
 *   查询都相同的全局常数**，不改变任何两行的相对顺序，因此 SQL 里用上面
 *   这个与 now 无关的等价形式，翻页时排序稳定（不受请求时刻影响）。
 * - 同分按 `id` 倒序，保证全序唯一。
 * - 游标：十进制串 `<created_at>-<id>`（例 `1762260000-42`），取上一页最后
 *   一条视频的两个字段。服务端按稳定全序定位该条，返回它之后的 `limit` 条；
 *   若该视频已被删除（游标失效），返回空页与空 `next_cursor`。
 * - `next_cursor` 只在「还有下一页」时非空，客户端拿到空串即停止翻页。
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

/// @brief `videos` 表里的一行视频（不含作者名，作者由 service 查 users 补齐）。
struct StoredVideo {
    std::int64_t id{0};             ///< 自增主键。
    std::int64_t user_id{0};        ///< 作者 id。
    std::string title;              ///< 标题。
    std::string url;                ///< 视频地址（只存引用，不存字节）。
    std::int64_t like_count{0};     ///< 点赞数。
    std::int64_t comment_count{0};  ///< 评论数。
    std::int64_t created_at{0};     ///< Unix 秒。
};

/**
 * @brief 编码视频分页游标：`"<created_at>-<id>"`（十进制，两个非负整数）。
 * @param video 游标指向的视频（通常是本页最后一条）。
 * @return 形如 `1762260000-42` 的串；`id<=0` 时返回空串（表示没有更多）。
 */
std::string EncodeVideoCursor(const StoredVideo& video);

/**
 * @brief 解码视频分页游标。
 * @param cursor 非空游标串。
 * @param created_at 输出 created_at。
 * @param id 输出 id。
 * @return 格式合法且两个数均 >0 时返回 true；否则 false（调用方回 400）。
 */
bool DecodeVideoCursor(const std::string& cursor, std::int64_t* created_at,
                       std::int64_t* id);

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

    /**
     * @brief 只取昵称（直接用 `DB::Query`，游标在函数返回时 finalize）。
     * @details `FindUserById` 走 ORM 会话的语句缓存，单行查询提前结束时会
     *          残留读游标（`ForEachRow` 不 reset），在 WAL 下表现为「读快照
     *          悬挂」：feed 看不到其它进程的新提交、user 写触发
     *          `SQLITE_BUSY_SNAPSHOT`。feed / video 的「作者名」查询用本方法，
     *          每次都结束读事务。
     * @param id 用户 id。
     * @param name 成功时写昵称。
     * @param error 数据库错误时写文本（未找到时为空）。
     * @return 找到返回 true。
     */
    bool FindUserName(std::int64_t id, std::string* name, std::string* error);

    /**
     * @brief 清空 ORM 语句缓存（finalize 全部缓存语句，结束悬挂的读游标）。
     * @details 写操作（注册/关注/取关）前调用，避免 WAL 下跨进程
     *          `SQLITE_BUSY_SNAPSHOT`。只影响性能（下次使用重建语句），不改数据。
     */
    void ResetStatements();

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

    /**
     * @brief 发布视频。
     * @param user_id 作者 id。
     * @param title 标题（非空由 service 校验）。
     * @param url 地址（非空由 service 校验）。
     * @param error 失败时写错误文本。
     * @return 新视频 id；失败返回 -1。
     */
    std::int64_t CreateVideo(std::int64_t user_id, const std::string& title,
                             const std::string& url, std::string* error);

    /// @brief 按 id 查视频；未找到返回 false 且 error 为空。
    bool FindVideo(std::int64_t id, StoredVideo* out, std::string* error);

    /**
     * @brief 作者自己的视频，按 `created_at` 倒序（同秒按 id 倒序）分页。
     * @param user_id 作者 id。
     * @param cursor 空串表示第一页；否则为上一页的 `next_cursor`。
     * @param limit 期望条数（<=0 返回空页）。
     * @param out 追加到末尾（不清空调用方已有内容）。
     * @param next_cursor 还有下一页时写游标，否则写空串。
     * @param error 失败时写错误文本。
     * @return 成功返回 true。
     */
    bool ListByUser(std::int64_t user_id, const std::string& cursor,
                    std::int64_t limit, std::vector<StoredVideo>* out,
                    std::string* next_cursor, std::string* error);

    /**
     * @brief 删除视频，仅作者本人可删。
     * @param id 视频 id。
     * @param user_id 请求者 id（同时作为 SQL 条件）。
     * @param error 失败时写错误文本。
     * @return 受影响行数（0 = 不存在或非作者）；出错返回 -1。
     */
    int DeleteVideo(std::int64_t id, std::int64_t user_id, std::string* error);

    /**
     * @brief 推荐流：热度序（见文件头公式）+ 游标分页。
     * @param cursor 空串表示第一页。
     * @param limit 期望条数。
     * @param out 追加到末尾。
     * @param next_cursor 还有下一页时写游标，否则空串。
     * @param error 失败时写错误文本。
     */
    bool ListFeed(const std::string& cursor, std::int64_t limit,
                  std::vector<StoredVideo>* out, std::string* next_cursor,
                  std::string* error);

    /**
     * @brief 关注流：只含 `user_id` 已关注作者的视频，排序与游标同 `ListFeed`。
     * @param user_id 请求者 id。
     */
    bool ListFollowingFeed(std::int64_t user_id, const std::string& cursor,
                           std::int64_t limit, std::vector<StoredVideo>* out,
                           std::string* next_cursor, std::string* error);

   private:
    std::unique_ptr<go2cpp::storage::DB> m_db;
    std::unique_ptr<go2cpp::storage::Session> m_session;
};

}  // namespace douyin
