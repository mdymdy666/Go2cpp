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

/// @brief `comments` 表里的一行评论（C5）。
struct StoredComment {
    std::int64_t id{0};          ///< 自增主键。
    std::int64_t video_id{0};    ///< 评论的视频。
    std::int64_t user_id{0};     ///< 评论者。
    std::string content;         ///< 正文（1~500 字，由 service 校验）。
    std::int64_t created_at{0};  ///< Unix 秒。
    std::int64_t parent_id{0};   ///< 二级回复的父评论 id；0=顶层评论。
    std::int64_t like_count{0};  ///< 评论点赞数；由 `FillCommentLikes` 回填。
    bool liked{false};           ///< 当前查看者是否点过赞；匿名查看恒为 false。
};

/// @brief `notifications` 表里的一行通知（消息中心，第 2 轮新增）。
/// @note `actor_name` 不是表列：查询时 `LEFT JOIN users` 回填，避免写入端
///       冗余昵称、改名后不同步。
struct StoredNotification {
    std::int64_t id{0};          ///< 自增主键。
    std::int64_t user_id{0};     ///< 接收者（通知归属人）。
    std::int64_t actor_id{0};    ///< 触发者（点赞/评论/关注的人）。
    std::string actor_name;      ///< 触发者昵称（JOIN 回填；可能为空）。
    std::string kind;            ///< 类型：like / comment / follow / mention。
    std::string target_type;     ///< 目标类型：video / comment / user。
    std::int64_t target_id{0};   ///< 目标 id（配合 target_type）。
    std::string content;         ///< 摘要（评论/标题截断；follow 为空）。
    std::int64_t created_at{0};  ///< Unix 秒。
    std::int64_t read_at{0};     ///< 已读 Unix 秒；0 = 未读。
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

/**
 * @brief 编码评论分页游标：`"<created_at>-<id>"`，语义与视频游标一致。
 * @param comment 游标指向的评论（通常是本页最后一条）。
 * @return 形如 `1762260000-42` 的串；`id<=0` 时返回空串。
 */
std::string EncodeCommentCursor(const StoredComment& comment);

/// @brief 解码评论分页游标；规则同 `DecodeVideoCursor`。
bool DecodeCommentCursor(const std::string& cursor, std::int64_t* created_at,
                         std::int64_t* id);

/**
 * @brief 编码通知分页游标：`"<created_at>-<id>"`，与评论/视频游标同构。
 * @param notification [in] 游标指向的通知（通常是本页最后一条）。
 * @return 形如 `1762260000-42` 的串；`id<=0` 时返回空串（表示没有更多）。
 */
std::string EncodeNotificationCursor(const StoredNotification& notification);

/**
 * @brief 解码通知分页游标。
 * @param cursor [in] 非空游标串，来自 `EncodeNotificationCursor`。
 * @param created_at [out] 输出 created_at；不可为 nullptr。
 * @param id [out] 输出 id；不可为 nullptr。
 * @return 格式合法且两个数均 >0 时返回 true；否则 false（调用方回 400）。
 */
bool DecodeNotificationCursor(const std::string& cursor,
                              std::int64_t* created_at, std::int64_t* id);

/**
 * @brief 编码推荐流随机游标：`"r<seed_hex>_<index>"`。
 * @param seed [in] 洗牌种子（首屏随机生成，后续页原样带回）。
 * @param index [in] 下一页起始下标（已消费条数）。
 * @return 形如 `r1a2b3c_20` 的串；**不要**手写该格式——解码失败即 400。
 */
std::string EncodeFeedCursor(std::uint64_t seed, std::size_t index);

/**
 * @brief 解码推荐流随机游标。
 * @param cursor [in] 非空游标串，来自 `EncodeFeedCursor`。
 * @param seed [out] 输出洗牌种子；不可为 nullptr。
 * @param index [out] 输出起始下标；不可为 nullptr。
 * @return 格式合法返回 true；否则 false（调用方回 400，**不要**回 500）。
 */
bool DecodeFeedCursor(const std::string& cursor, std::uint64_t* seed,
                      std::size_t* index);

/// @brief `reports` 表里的一行举报（内容治理，第 3 轮新增）。
/// @note 唯一键 `(reporter_id, target_type, target_id)`：同一个人对同一个
///       目标只能举报一次，重复提交返回已有记录而不是新增。
struct StoredReport {
    std::int64_t id{0};           ///< 自增主键。
    std::int64_t reporter_id{0};  ///< 举报者 id。
    std::string target_type;      ///< 目标类型：video / comment / user。
    std::int64_t target_id{0};    ///< 目标 id（配合 target_type）。
    std::string reason;           ///< 举报原因（短标签，1~50 字）。
    std::string detail;           ///< 补充说明（可空，<=500 字）。
    std::string status;           ///< pending / handled。
    std::int64_t created_at{0};   ///< Unix 秒。
};

/// @brief `blocks` 表里的一行拉黑记录（第 3 轮新增）。
/// @note `blocked_name` 不是表列：查询时 `LEFT JOIN users` 回填，避免
///       冗余昵称、改名后不同步。
struct StoredBlock {
    std::int64_t id{0};             ///< 自增主键。
    std::int64_t user_id{0};        ///< 拉黑者。
    std::int64_t blocked_id{0};     ///< 被拉黑者。
    std::string blocked_name;       ///< 被拉黑者昵称（JOIN 回填；可能为空）。
    std::int64_t created_at{0};     ///< Unix 秒。
};

/// @brief `upload_sessions` 表里的一行分片上传会话（断点续传状态）。
/// @note `upload_id` 是服务端生成的 UUID v4，同时是继续上传的唯一凭据；
///       `path` 只由服务端拼出（`web/videos/.upload_<upload_id>.part`），
///       绝不接受客户端传路径。
struct StoredUpload {
    std::string upload_id;           ///< UUID v4 文本（36 字符，小写）。
    std::int64_t user_id{0};         ///< 发起者 id；只有本人能续传/完成。
    std::string path;                ///< `.part` 暂存文件的绝对路径。
    std::string title;               ///< 标题（init 时可选，finish 时覆盖）。
    std::int64_t total{0};           ///< 声明的总字节数（1 ~ 32 MiB）。
    std::int64_t received_bytes{0};  ///< 已确认落盘的**连续前缀**字节数。
    std::int64_t created_at{0};      ///< Unix 秒。
};

/// @brief rooms 表里的一行直播间（C7）。
struct StoredRoom {
    std::int64_t id{0};            ///< 自增主键。
    std::int64_t user_id{0};       ///< 房主 id。
    std::string title;             ///< 房间标题。
    int status{0};                 ///< 0=关播，1=直播中。
    std::int64_t viewer_count{0};  ///< 当前观众数（原子增减，>=0）。
    std::int64_t created_at{0};    ///< Unix 秒。
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
     * @brief 底层 SQLite 连接（借用；仅 Open 成功后非空）。
     * @details 供同进程的横向组件复用同一连接（例如幂等中间件写
     *          `idempotency_keys` 表），避免第二个连接在 WAL 下多一份锁竞争。
     * @return 连接指针；未 Open 或已失败时返回 nullptr，调用方不得 Close。
     */
    go2cpp::storage::DB* Database() noexcept { return m_db.get(); }

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
     * @brief 关注数 / 粉丝数（单条 SQL 两个 COUNT 子查询）。
     * @param id 用户 id。
     * @param followers [out] 粉丝数——成功时写入；失败时保持原值。
     * @param following [out] 关注数——成功时写入；失败时保持原值。
     * @param error [out] 失败时写错误文本。
     * @return 成功返回 true。
     */
    bool FollowCounts(std::int64_t id, std::int64_t* followers,
                      std::int64_t* following, std::string* error);

    /**
     * @brief 是否已关注（用于个人主页按钮初始状态）。
     * @param follower 关注者（通常为当前登录用户）。
     * @param followee 被关注者。
     * @param out [out] 结果——成功时写入。
     * @param error [out] 数据库错误时写文本。
     * @return 查询成功返回 true（未关注时 out=false 且 error 为空）。
     */
    bool IsFollowing(std::int64_t follower, std::int64_t followee, bool* out,
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
     * @brief 取全部视频（按 id 升序），供话题聚合在内存里过滤。
     *
     * @param out [out] 追加到末尾（不清空调用方已有内容）；失败时内容未定义。
     * @param error [out] 失败时写错误文本。
     * @return 成功返回 true（可能是空库）。
     *
     * ## 定位
     * 话题聚合的唯一全量入口：demo 视频量级是几十条，全表扫描比维护
     * `video_tags` 表更简单。错了会怎样：话题页漏视频/重复计数。
     */
    bool ListAllVideos(std::vector<StoredVideo>* out, std::string* error);

    /**
     * @brief 删除视频，仅作者本人可删。
     * @param id 视频 id。
     * @param user_id 请求者 id（同时作为 SQL 条件）。
     * @param error 失败时写错误文本。
     * @return 受影响行数（0 = 不存在或非作者）；出错返回 -1。
     */
    int DeleteVideo(std::int64_t id, std::int64_t user_id, std::string* error);

    /**
     * @brief 推荐流：**随机序** + 游标分页（候选池=全库视频，不做作者过滤，
     *        所以当前用户自己的视频也在池里，能刷到自己）。
     *
     * @details 首屏生成随机 seed，用 seed 确定性洗牌全库；游标携带
     *          `(seed, 已消费条数)`，下一页用同一 seed 重新洗牌后从该位置续取，
     *          因此翻页不重复、不漏条，且每次首屏顺序都不同。等价于
     *          `ORDER BY RANDOM() LIMIT n`，但保持了可续页性。
     *
     * @param cursor [in] 空串=第一页；否则必须是 `EncodeFeedCursor` 产物，
     *        格式非法返回 false——调用方**必须**回 400 而不是 500。
     * @param limit [in] 期望条数；<=0 返回空页，**不要**当错误。
     * @param out [out] 追加到末尾；失败时内容未定义。
     * @param next_cursor [out] 还有下一页时写游标，否则空串。
     * @param error [out] 失败时写错误文本；成功时保持原值。
     * @return true=成功（可能空页）；false=cursor 非法或数据库错误。
     */
    bool ListFeed(const std::string& cursor, std::int64_t limit,
                  std::vector<StoredVideo>* out, std::string* next_cursor,
                  std::string* error, std::int64_t viewer_id = 0);

    /**
     * @brief 关注流：只含 `user_id` 已关注作者的视频，排序与游标同 `ListFeed`。
     * @param user_id 请求者 id。
     */
    bool ListFollowingFeed(std::int64_t user_id, const std::string& cursor,
                           std::int64_t limit, std::vector<StoredVideo>* out,
                           std::string* next_cursor, std::string* error);

    // ---- C5 互动：likes / comments / 计数同步（只加不改既有） ----

    /**
     * @brief 点赞（幂等：`INSERT OR IGNORE`，主键 `(video_id,user_id)`）。
     * @return 1=本次新插入；0=已点过（计数不变）；-1=数据库错误。
     */
    int Like(std::int64_t video_id, std::int64_t user_id, std::string* error);

    /// @brief 取消赞（幂等：未点过也不报错）；成功返回 true。
    bool Unlike(std::int64_t video_id, std::int64_t user_id, std::string* error);

    /// @brief 点赞数；出错返回 -1。
    std::int64_t LikeCount(std::int64_t video_id, std::string* error);

    /// @brief 是否已点赞；数据库错误时返回 false（error 非空）。
    bool HasLiked(std::int64_t video_id, std::int64_t user_id, std::string* error);

    // ---- C8 收藏：favorites（只加不改既有） ----

    /**
     * @brief 切换收藏：已收藏则取消，未收藏则写入（表上有 `UNIQUE(user_id,video_id)`）。
     * @param video_id [in] 视频 id；调用方**必须**先确认视频存在。
     * @param user_id [in] 收藏者 id（登录用户）。
     * @param error [out] 失败时写错误文本；成功时保持原值。
     * @return
     * - 1：本次新增收藏（调用方回 `favorited:true`）；
     * - 0：本次取消收藏（调用方回 `favorited:false`）；
     * - -1：数据库错误——调用方**必须**回 500，不要翻转前端状态。
     *
     * ## 定位
     * 收藏是「单用户 × 单视频」的二元状态，这里是唯一提交点；错了会怎样：
     * 重复行让收藏列表出现同一条视频两次，或前端按钮与库里状态相反。
     */
    int ToggleFavorite(std::int64_t video_id, std::int64_t user_id,
                       std::string* error);

    /// @brief 是否已收藏；数据库错误时返回 false（error 非空）。
    bool HasFavorited(std::int64_t video_id, std::int64_t user_id,
                      std::string* error);

    /**
     * @brief 当前用户的收藏列表，按收藏时间倒序（同秒按视频 id 倒序）。
     * @param user_id [in] 收藏者 id。
     * @param limit [in] 最多返回条数；<=0 返回空列表（不当成错误）。
     * @param out [out] 追加到末尾（不清空调用方已有内容）；失败时内容未定义。
     * @param error [out] 失败时写错误文本；成功时保持原值。
     * @return 成功返回 true（可能是空列表）。
     *
     * ## 定位
     * 「我的收藏」唯一数据源；用 INNER JOIN videos 而不是 LEFT JOIN：
     * 视频被作者删除后收藏记录还在，LEFT JOIN 会返回空 url 的卡片（前端
     * 只渲染出黑块），不如直接不出现。
     */
    bool ListFavorites(std::int64_t user_id, std::int64_t limit,
                       std::vector<StoredVideo>* out, std::string* error);

    /**
     * @brief 发评论（`parent_id` 非 0 表示二级回复）。
     * @param parent_id 父评论 id；0=顶层评论（父评论归属由 service 校验）。
     * @return 新评论 id；失败返回 -1。
     */
    std::int64_t AddComment(std::int64_t video_id, std::int64_t user_id,
                            const std::string& content, std::string* error,
                            std::int64_t parent_id = 0);

    /**
     * @brief 某视频的顶层评论（`parent_id=0`），`created_at` 倒序 keyset 分页。
     * @param cursor 空串表示第一页；否则为本页最后一条的游标。
     * @param out 追加到末尾。
     * @param next_cursor 还有下一页时写游标，否则空串。
     */
    bool ListComments(std::int64_t video_id, const std::string& cursor,
                      std::int64_t limit, std::vector<StoredComment>* out,
                      std::string* next_cursor, std::string* error,
                      std::int64_t viewer_id = 0);

    /**
     * @brief 某条顶层评论的二级回复，按 `created_at` 正序（同秒按 id 正序）。
     * @param video_id 视频 id（与 parent_id 同时作为 SQL 条件，防止跨视频串号）。
     * @param parent_id 顶层评论 id。
     * @param limit 最多返回条数（调用方传上限）。
     * @param out 追加到末尾。
     * @param error 失败时写错误文本。
     * @return 成功返回 true。
     */
    bool ListReplies(std::int64_t video_id, std::int64_t parent_id,
                     std::int64_t limit, std::vector<StoredComment>* out,
                     std::string* error, std::int64_t viewer_id = 0);

    /// @brief 按 id 查评论；未找到返回 false 且 error 为空。
    bool FindComment(std::int64_t id, StoredComment* out, std::string* error);

    /// @brief 删除评论（仅作者）；返回受影响行数，出错 -1。
    int DeleteComment(std::int64_t id, std::int64_t user_id, std::string* error);

    /**
     * @brief 按 likes/comments 实表重算 `videos.like_count/comment_count`。
     * @note 单事务内完成，供点赞/评论/取消赞后调用。
     */
    bool UpdateVideoCounts(std::int64_t video_id, std::string* error);

    // ---- 评论点赞：comment_likes（只加不改既有） ----

    /**
     * @brief 给评论点赞（幂等：`INSERT OR IGNORE`，主键 `(comment_id,user_id)`）。
     *
     * @param comment_id [in] 评论 id；调用方**必须**先确认评论存在。
     * @param user_id [in] 点赞者 id（登录用户）。
     * @param error [out] 失败时写错误文本；成功时保持原值。
     * @return
     * - 1：本次新插入（前端应显示已点赞）；
     * - 0：已经点过（幂等，计数不变）；
     * - -1：数据库错误——调用方**必须**回 500，不要翻转前端状态。
     *
     * ## 定位
     * 评论点赞的唯一提交点；错了会怎样：重复行把计数算成 2 次，
     * 或按钮状态与库里相反。
     */
    int LikeComment(std::int64_t comment_id, std::int64_t user_id,
                    std::string* error);

    /**
     * @brief 取消评论点赞（幂等：没点过也不报错）。
     * @param comment_id [in] 评论 id。
     * @param user_id [in] 点赞者 id。
     * @param error [out] 失败时写错误文本；成功时保持原值。
     * @return 成功返回 true（含「本来就没点」）。
     */
    bool UnlikeComment(std::int64_t comment_id, std::int64_t user_id,
                       std::string* error);

    /**
     * @brief 批量回填评论的 `like_count` 与 `liked`（两条 SQL 覆盖整页）。
     *
     * @param viewer_id [in] 当前查看者 id；<=0 表示匿名（`liked` 全 false）。
     * @param comments [in,out] 评论列表，不可为 nullptr；进入时读 `id`，
     *        返回时写 `like_count`/`liked`。空列表直接返回 true。
     * @param error [out] 失败时写错误文本；成功时保持原值。
     * @return 成功返回 true；失败时 `comments` 里已写入的值未定义。
     *
     * ## 定位
     * 评论列表的**唯一**批量计数入口。逐条查会变成 N+1（一页 50 条 =
     * 100 次往返），这里固定 2 条 SQL（`GROUP BY` 计数 + 查看者已赞集合）。
     * 错了会怎样：前端每条评论显示 0 赞 / 点赞状态错乱。
     *
     * ## 性能
     * `comment_likes` 主键前缀 `comment_id` 命中索引；IN 列表长度等于本页
     * 评论数（<=100），SQL 文本只含整数，不拼接用户输入。
     */
    bool FillCommentLikes(std::int64_t viewer_id,
                          std::vector<StoredComment>* comments,
                          std::string* error);

    // ---- 消息通知中心：notifications（第 2 轮新增，只加不改既有） ----

    /**
     * @brief 写一条通知（「该不该写」由服务层判定，存储层只负责落库）。
     *
     * @param user_id [in] 接收者 id；必须 > 0。
     * @param actor_id [in] 触发者 id；必须 > 0。
     * @param kind [in] 通知类型，只接受 `like` / `comment` / `follow` /
     *        `mention`；其它值视为脏数据拒绝。
     * @param target_type [in] 目标类型：`video` / `comment` / `user`。
     * @param target_id [in] 目标 id（> 0）。
     * @param content [in] 摘要文本（可为空）；存储层不截断，调用方负责长度。
     * @param error [out] 失败时写错误文本；成功时保持原值。
     * @return 成功返回 true。
     *
     * ## 定位
     * 通知写入的**唯一**提交点。错了会怎样：面板出现不存在的人/视频，
     * 或未读计数虚高。
     *
     * ## 并发/失败语义
     * - 调用线程：HTTP 事件循环单线程；无需锁。
     * - 失败语义：返回 false 时调用方**不要**回滚主操作（点赞/评论已生效），
     *   只记日志——丢一条通知不影响数据一致性。
     */
    bool AddNotification(std::int64_t user_id, std::int64_t actor_id,
                         const std::string& kind,
                         const std::string& target_type,
                         std::int64_t target_id, const std::string& content,
                         std::string* error);

    /**
     * @brief 通知列表：按时间倒序 keyset 分页，一次 JOIN 回填触发者昵称。
     *
     * @param user_id [in] 接收者 id（只返回这个人的通知）。
     * @param cursor [in] 空串=第一页；否则为本页最后一条的游标。
     * @param limit [in] 期望条数；<=0 返回空页（不当成错误）。
     * @param out [out] 追加到末尾（不清空调用方已有内容）。
     * @param next_cursor [out] 还有下一页时写游标，否则空串。
     * @param error [out] 失败时写错误文本。
     * @return 成功返回 true；cursor 非法时 error 写 `cursor 非法`。
     *
     * ## 性能
     * 固定 1 条 SQL（`LIMIT limit+1` 判断下一页），命中
     * `idx_notifications_timeline(user_id, created_at DESC, id DESC)`；
     * 昵称用 JOIN 而不是逐条查，避免 N+1。
     */
    bool ListNotifications(std::int64_t user_id, const std::string& cursor,
                           std::int64_t limit,
                           std::vector<StoredNotification>* out,
                           std::string* next_cursor, std::string* error);

    /**
     * @brief 未读数：`SELECT COUNT(*) ... WHERE user_id=? AND read_at=0`。
     * @param user_id [in] 接收者 id。
     * @param error [out] 失败时写错误文本。
     * @return 未读数（>=0）；出错返回 -1。
     *
     * ## 性能
     * 单条 COUNT，命中 `idx_notifications_unread(user_id, read_at, ...)`，
     * 不扫描全表。
     */
    std::int64_t CountUnreadNotifications(std::int64_t user_id,
                                          std::string* error);

    /**
     * @brief 标记已读：按 id 列表或全部。
     *
     * @param user_id [in] 接收者 id；SQL 里同时作为条件，**别人的通知改不到**。
     * @param ids [in] 要标记的通知 id；`all=true` 时忽略。
     * @param all [in] true=把这个人的全部未读标成已读。
     * @param error [out] 失败时写错误文本。
     * @return 实际更新的行数（>=0）；出错返回 -1。
     *
     * ## 定位
     * 未读数归零的唯一入口。错了会怎样：徽章数字与列表状态不一致。
     */
    std::int64_t MarkNotificationsRead(std::int64_t user_id,
                                       const std::vector<std::int64_t>& ids,
                                       bool all, std::string* error);

    // ---- 举报 / 拉黑：reports / blocks（第 3 轮新增，只加不改既有） ----

    /// @brief `AddReport` 的结果：新建 / 重复 / 失败。
    enum class ReportResult { kCreated = 0, kDuplicate = 1, kError = 2 };

    /**
     * @brief 写一条举报（同一个人对同一目标只能一条，唯一键兜底）。
     *
     * @param reporter_id [in] 举报者 id（>0，登录用户）。
     * @param target_type [in] 目标类型：`video` / `comment` / `user`；
     *        其它值视为脏数据，**不要**入库。
     * @param target_id [in] 目标 id（>0；目标是否存在由服务层先查）。
     * @param reason [in] 原因标签（1~50 字，服务层校验，存储层不截断）。
     * @param detail [in] 补充说明（可空，<=500 字由服务层校验）。
     * @param out [out] 成功（含重复）时写库里的那条举报；可为 nullptr。
     * @param error [out] 失败时写错误文本；成功时保持原值。
     * @return
     * - `ReportResult::kCreated`：本次新插入（服务层回 201）；
     * - `ReportResult::kDuplicate`：唯一键冲突，`out` 是已有记录（回 200）；
     * - `ReportResult::kError`：参数非法或数据库错误（服务层回 500/400）。
     *
     * ## 定位
     * 举报写入的唯一提交点。错了会怎样：同一个人刷出成千上万条重复举报，
     * 运营后台无法按目标聚合。
     */
    ReportResult AddReport(std::int64_t reporter_id,
                           const std::string& target_type,
                           std::int64_t target_id, const std::string& reason,
                           const std::string& detail, StoredReport* out,
                           std::string* error);

    /// @brief 按唯一键查举报；未找到返回 false 且 error 为空。
    bool FindReport(std::int64_t reporter_id, const std::string& target_type,
                    std::int64_t target_id, StoredReport* out,
                    std::string* error);

    /**
     * @brief 拉黑用户（幂等：`INSERT OR IGNORE`，唯一键 `(user_id, blocked_id)`）。
     * @param user_id [in] 拉黑者（登录用户）。
     * @param blocked_id [in] 被拉黑者；**不能等于** user_id（服务层先挡）。
     * @param error [out] 失败时写错误文本；成功时保持原值。
     * @return 1=本次新增；0=之前已拉黑；-1=数据库错误。
     */
    int BlockUser(std::int64_t user_id, std::int64_t blocked_id,
                  std::string* error);

    /**
     * @brief 取消拉黑（幂等：没拉黑也不报错）。
     * @param user_id [in] 拉黑者。
     * @param blocked_id [in] 被拉黑者。
     * @param error [out] 失败时写错误文本。
     * @return 成功返回 true（含「本来就没拉黑」）。
     */
    bool UnblockUser(std::int64_t user_id, std::int64_t blocked_id,
                     std::string* error);

    /// @brief 是否已拉黑；数据库错误时返回 false（error 非空）。
    bool IsBlocked(std::int64_t user_id, std::int64_t blocked_id, bool* out,
                   std::string* error);

    /**
     * @brief 黑名单列表：按拉黑时间倒序，一次 JOIN 回填昵称。
     * @param user_id [in] 拉黑者。
     * @param out [out] 追加到末尾（不清空调用方已有内容）。
     * @param error [out] 失败时写错误文本。
     * @return 成功返回 true（可能是空列表）。
     */
    bool ListBlocks(std::int64_t user_id, std::vector<StoredBlock>* out,
                    std::string* error);

    // ---- C6 搜索：name/title 包含匹配（大小写不敏感） ----

    /**
     * @brief 搜索用户：`lower(name) LIKE lower('%q%')`，按 id 倒序。
     * @param limit 最多返回条数。
     * @param offset 跳过条数（游标分页）。
     * @param out 追加到末尾；password_hash/salt 留空。
     */
    bool SearchUsers(const std::string& query, std::int64_t limit,
                     std::int64_t offset, std::vector<StoredUser>* out,
                     std::string* error, std::int64_t viewer_id = 0);

    /// @brief 搜索视频：`lower(title) LIKE lower('%q%')`，按 id 倒序。
    bool SearchVideos(const std::string& query, std::int64_t limit,
                      std::int64_t offset, std::vector<StoredVideo>* out,
                      std::string* error, std::int64_t viewer_id = 0);

    // ---- C7 直播：rooms + room_viewers（只加不改既有） ----

    /**
     * @brief 创建直播间（status=0 关播，viewer_count=0）。
     * @return 新房间 id；失败返回 -1。
     */
    std::int64_t CreateRoom(std::int64_t user_id, const std::string& title,
                            std::string* error);

    /// @brief 按 id 查房间；未找到返回 false 且 error 为空。
    bool FindRoom(std::int64_t id, StoredRoom* out, std::string* error);

    /**
     * @brief 房间列表，按 `created_at DESC, id DESC`。
     * @param status <0 表示不限；0/1 只列该状态。
     */
    bool ListRooms(int status, std::vector<StoredRoom>* out, std::string* error);

    /**
     * @brief 开播/关播，仅房主可改（user_id 同时作为 SQL 条件）。
     * @return 受影响行数（0 = 不存在或非房主）；出错 -1。
     */
    int UpdateRoomStatus(std::int64_t id, std::int64_t user_id, int status,
                         std::string* error);

    /**
     * @brief 进房：room_viewers 唯一约束保证幂等，只有首次进入才 +1。
     * @return 1=本次新进；0=已在该房间（不变）；-1=数据库错误或房间不存在。
     */
    int EnterRoom(std::int64_t id, std::int64_t user_id, std::string* error);

    /**
     * @brief 出房：只有确实在房间内才 -1，且 viewer_count 不会为负。
     * @return 1=本次离开；0=本不在房间（不变）；-1=数据库错误。
     */
    int LeaveRoom(std::int64_t id, std::int64_t user_id, std::string* error);

    // ---- 分片上传（断点续传）：upload_sessions 持久化 ----

    /**
     * @brief 插入一条上传会话（upload_id 为主键，冲突即失败）。
     * @param session [in] 已填好全部字段的会话；upload_id 由 service 生成。
     * @param error [out] 失败时写错误文本。
     * @return 成功返回 true。
     */
    bool CreateUploadSession(const StoredUpload& session, std::string* error);

    /**
     * @brief 按 upload_id 查上传会话。
     * @param upload_id [in] UUID v4 文本。
     * @param out [out] 找到时写入；未找到保持原值。
     * @param error [out] 数据库错误时写文本（未找到时为空）。
     * @return 找到返回 true。
     */
    bool FindUploadSession(const std::string& upload_id, StoredUpload* out,
                           std::string* error);

    /**
     * @brief 更新已收字节数（每个分片落盘后调用一次）。
     * @param upload_id [in] 会话 id。
     * @param received_bytes [in] 新的连续前缀字节数（>=0，<=total）。
     * @param error [out] 失败时写错误文本。
     * @return 成功返回 true。
     */
    bool UpdateUploadProgress(const std::string& upload_id,
                              std::int64_t received_bytes, std::string* error);

    /**
     * @brief 删除上传会话（finish 成功后调用）。
     * @param error [out] 失败时写错误文本。
     * @return 成功返回 true（会话本就不存在也算成功）。
     */
    bool DeleteUploadSession(const std::string& upload_id, std::string* error);

   private:
    std::unique_ptr<go2cpp::storage::DB> m_db;
    std::unique_ptr<go2cpp::storage::Session> m_session;
};

}  // namespace douyin
