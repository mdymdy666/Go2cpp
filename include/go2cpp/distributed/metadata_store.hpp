#pragma once

/**
 * @file metadata_store.hpp
 * @brief 带 revision 与 Watch 的元数据 KV：协调者状态之外的通用小对象存储。
 *
 * ## 为什么需要它
 *
 * 协调者快照、节点注册表、配置版本号这类「小对象 + 需要变更通知」的数据，
 * 用 `storage::KV` 存得下，但缺两样东西：**每次写返回单调 revision** 与
 * **前缀订阅 + 游标重放**。本文件补的就是这两样，语义对齐
 * `storage::Watcher`（`include/go2cpp/storage/watch.hpp`）：
 *
 * - 写入按全局单调 revision 排序，Put/Delete 成功后 revision 恰好 +1；
 * - 订阅者按 key 前缀匹配，事件携带 revision；
 * - 每个订阅维护自己的游标（已派发到哪个 revision），可用 `Replay()` 从
 *   任意历史 revision 重放；
 * - 回调异常被吞掉并计数，**不会**破坏 store 自身状态。
 *
 * ## 与 `storage::Watcher` 的差异（以及为什么）
 *
 * 1. `Watcher` 依赖 `storage::KV`（独立目标 `go2cpp_kv`，`GO2CPP_WITH_KV`
 *    默认 ON 但只支持 POSIX），而 `distributed` 库目前不链接存储层。
 *    为避免把 SQLite/KV 变成分布式模块的强制依赖，本实现自带一套最小
 *    WAL（追加写 + CRC32 + 截断恢复），格式见 `metadata_store.cpp` 顶部注释。
 *    设计参数与 `Watcher` 一致：同步/异步派发、有界事件历史、游标重放。
 * 2. `Watcher` 的订阅 ID 由 KV 里的 `watch/<id>/offset` 持久化；本实现把
 *    游标留在内存（订阅是进程内的），只持久化数据与 revision。跨重启的
 *    订阅恢复不在本模块范围内（调用方重建订阅后用 `Replay()` 补事件）。
 *
 * ## 持久化与失败语义
 *
 * - `wal_path` 为空 = 纯内存 store（测试/单进程缓存）；
 * - 非空时每次写追加一条 WAL 记录，`fsync_on_write=true`（默认）时写完
 *   `fdatasync`，保证「Put 返回成功 = 崩溃后能读回」；
 * - WAL 尾部不完整（断电写一半）在恢复时**截断到最后一条完整记录**并
 *   继续启动；中间的 CRC 错误视为损坏，`Open` 返回失败（不能静默跳过，
 *   否则 revision 会断裂）。
 *
 * ## 线程模型
 *
 * 所有公开方法线程安全。默认异步派发：写线程只入队，单后台线程按 revision
 * 顺序调用回调；回调里可以安全地再次 `Put`（不会死锁）。设置
 * `sync_callbacks=true` 时回调在写线程内同步执行，**不得**在回调里写同一个
 * store（会在非递归互斥量上死锁）。`Flush()` 等待已入队事件派发完毕。
 *
 * @author go2cpp contributors
 * @date 2026-11-04
 * @copyright MIT License
 */

#include "go2cpp/distributed/types.hpp"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace go2cpp::distributed {

/// 一条元数据记录（`Get` / `List` 的返回）。
struct MetadataEntry {
    std::string key;
    std::string value;
    std::uint64_t revision{0};  ///< 最后一次写入该 key 的全局 revision。
};

/// 一次变更事件（Watch 回调 / `Replay` 的载荷）。
struct MetadataEvent {
    std::uint64_t revision{0};
    std::string key;
    std::string value;   ///< 删除事件为空。
    bool deleted{false};
};

/// `MetadataStore` 打开参数。
struct MetadataStoreOptions {
    /// WAL 文件路径；空 = 纯内存（不持久化）。
    std::string wal_path;
    /// true：每次写 `fdatasync`（崩溃不丢已返回成功的写）；false：由内核回写。
    bool fsync_on_write{true};
    /// 内存中保留的可重放事件条数上限（0 = 不保留历史，`Replay` 只能重放游标之后的新事件）。
    std::size_t max_events{4096};
    /// true：回调在写线程内同步执行（顺序最强，禁止回调内写）；false：后台线程派发。
    bool sync_callbacks{false};
    /// 异步模式下待派发队列上限；超出时写线程阻塞（背压，不丢事件）。
    std::size_t queue_capacity{65536};
};

/// 运行期统计（快照语义：`GetStats()` 拷贝一份）。
struct MetadataStoreStats {
    std::uint64_t puts{0};
    std::uint64_t deletes{0};
    std::uint64_t delivered{0};        ///< 回调调用次数（含 Replay）。
    std::uint64_t matched{0};          ///< 命中订阅的事件数。
    std::uint64_t replayed{0};         ///< `Replay` 派发的事件数。
    std::uint64_t callback_errors{0};  ///< 回调抛异常的次数。
    std::uint64_t recover_records{0};  ///< 启动时从 WAL 恢复的记录数。
    std::uint64_t recovered_bytes{0};  ///< 启动时读取的 WAL 字节数。
    std::uint64_t truncated_bytes{0};  ///< 恢复时被截断的不完整尾部字节数。
    std::uint64_t last_revision{0};
    std::size_t subscriptions{0};
    std::size_t entries{0};
    std::size_t history{0};
};

/**
 * @brief 带 revision 与 Watch 的元数据 KV。
 *
 * 生命周期：`Open()` 返回的实例持有 WAL 文件与（异步模式下的）派发线程；
 * 析构时先 `Flush()` 再停止派发线程，保证已成功的写一定被派发。
 * 不可拷贝/移动。
 */
class MetadataStore {
  public:
    /// 事件回调；在异步派发线程（或同步模式的写线程）中调用。
    using Callback = std::function<void(const MetadataEvent &)>;

    /**
     * @brief 打开一个 store（内存或 WAL 持久化）。
     *
     * @param options [in] 打开参数；`wal_path` 为空时纯内存。按值持有，调用方
     *                返回后可以立即释放。
     *
     * @return
     * - 成功：返回已恢复完毕的实例（revision 已推进到 WAL 最后一条）；调用方
     *   **必须**检查 `ok()`，失败时 `value` 为空指针；
     * - 失败：`kUnavailable`（文件打不开 / 写 WAL 失败）或 `kProtocolError`
     *   （WAL 中间记录 CRC 错误，无法安全恢复）；调用方**不要**继续使用，
     *   按错误信息修复或改用内存模式。
     *
     * ## 定位
     * store 的**唯一构造入口**。恢复失败必须显式报错：如果静默跳过坏记录，
     * revision 会不连续，Watch 的「按 revision 顺序投递」与协调者的单调性
     * 都会失去依据。
     *
     * ## 步骤
     * step1. 校验参数（`wal_path` 非空时打开/创建文件）；
     * step2. 逐条扫描 WAL：校验魔数/长度/CRC，重建 map 与 revision；
     * step3. 尾部不完整 → 截断到最后一个完整记录；中间损坏 → 直接失败；
     * step4. 启动异步派发线程（非同步模式）。
     *
     * ## 并发/失败语义
     * - 调用约束：构造期单线程，不得与其他实例共享同一 WAL 文件（无文件锁）；
     * - 失败语义：返回 `Result` 而不抛异常；失败时不留下半初始化对象；
     * - 不变量：成功返回时 `Revision()` == WAL 中最大 revision。
     */
    static Result<std::unique_ptr<MetadataStore>> Open(const MetadataStoreOptions &options);

    /// @brief 纯内存 store 的便捷入口（等价于 `Open({})`）。
    static Result<std::unique_ptr<MetadataStore>> OpenMemory();

    ~MetadataStore();

    MetadataStore(const MetadataStore &) = delete;
    MetadataStore &operator=(const MetadataStore &) = delete;
    MetadataStore(MetadataStore &&) = delete;
    MetadataStore &operator=(MetadataStore &&) = delete;

    /// @brief 读取 key 的当前值；不存在返回 `kNotFound`。
    Result<MetadataEntry> Get(std::string_view key) const;

    /// @brief 按 key 字典序列出前缀匹配的全部记录（前缀为空 = 全部）。
    std::vector<MetadataEntry> List(std::string_view prefix) const;

    /**
     * @brief 写入/覆盖一个 key，返回本次写入的全局 revision。
     *
     * @param key [in] 非空；长度上限 1 MiB。
     * @param value [in] 任意字节（允许为空）；长度上限 1 MiB。
     * @param revision [out] 可为 nullptr；成功时写本次 revision，失败保持原值。
     *
     * @return `kOk`；`kInvalidArgument`（key 为空/超长）；`kUnavailable`
     *         （WAL 写失败，内存状态未改变）。
     *
     * ## 定位
     * store 的**唯一写入口**（`Delete` 共用同一提交路径）。失败时不能部分
     * 生效：先追加 WAL 成功后才更新内存，保证「返回成功 = 可恢复」。
     *
     * ## 步骤
     * step1. 参数校验（锁外，避免为非法请求抢锁）；
     * step2. 持锁分配 revision、追加 WAL（含 CRC），失败直接返回且不动内存；
     * step3. 更新 map 与事件历史、入队/派发事件；
     * step4. 释放锁后（同步模式）调用回调；回调异常计数并吞掉。
     *
     * ## 并发/失败语义
     * - 调用约束：任意线程；WAL 追加在锁内串行，因此 revision 顺序 == 落盘顺序；
     * - 原子性：单条记录一次 `write`，恢复时 CRC 校验整条；
     * - 不变量：同一 key 的 revision 严格递增；`Revision()` 只增不减。
     */
    Outcome Put(std::string_view key, std::string_view value,
                std::uint64_t *revision = nullptr);

    /**
     * @brief 删除一个 key（逻辑删除事件，revision 同样 +1）。
     *
     * @param key [in] 非空；不存在时也返回 `kOk`（幂等删除），但仍消耗一个
     *                revision——调用方可以用它做「删除屏障」。
     * @param revision [out] 可为 nullptr；成功时为本次 revision。
     * @return `kOk` / `kInvalidArgument` / `kUnavailable`（失败时内存未变）。
     */
    Outcome Delete(std::string_view key, std::uint64_t *revision = nullptr);

    /// @brief 当前最大 revision（空 store 为 0）。
    std::uint64_t Revision() const;

    /**
     * @brief 订阅一个 key 前缀，返回订阅 id（0 = 失败）。
     *
     * @param prefix [in] 前缀；空串匹配全部 key。
     * @param callback [in] 非空；在异步派发线程（或同步模式的写线程）调用。
     *                 回调抛出的任何异常都被捕获并计数，不会传播到写路径。
     * @param stable_id [in] 0 = 自动分配；非 0 时复用该 id（重复订阅会替换
     *                 回调并保留已派发游标，便于重建订阅后 `Replay`）。
     *
     * @return 订阅 id（非 0）；`callback` 为空返回 0。
     *
     * ## 定位
     * 唯一的订阅注册点。订阅只从注册时刻起接收**新**事件；历史补发走
     * `Replay()`，两者分工避免「注册即重放」带来的重复投递。
     *
     * ## 步骤
     * step1. 校验回调非空；
     * step2. 持锁分配/复用 id，`cursor` 初始化为当前 revision（只收新事件）；
     * step3. 返回 id（调用方可用它 `Unwatch` / `Replay`）。
     *
     * ## 并发/失败语义
     * - 调用约束：任意线程；注册完成前的事件不会被投递；
     * - 不变量：同一订阅的事件按 revision 严格递增投递。
     */
    std::uint64_t Watch(std::string_view prefix, Callback callback,
                        std::uint64_t stable_id = 0);

    /// @brief 取消订阅；id 不存在返回 false（幂等）。
    bool Unwatch(std::uint64_t id);

    /**
     * @brief 把历史中 revision >= `from_revision` 且匹配订阅前缀的事件补发给该订阅。
     *
     * @param id [in] `Watch()` 返回的订阅 id；不存在返回 `kNotFound`。
     * @param from_revision [in] 起始 revision（含）。早于历史窗口的事件无法
     *                      补发，返回 `kNotFound` 并由调用方决定是否重建全量。
     *
     * @return `kOk` / `kNotFound` / `kClosed`（store 正在关闭）。
     *
     * ## 定位
     * 「写路径不重放、读路径按需重放」的分界点。回调在调用线程执行（即使
     * 异步模式），异常同样被吞掉并计数。
     *
     * ## 并发/失败语义
     * - 调用约束：任意线程；与派发线程通过 `m_dispatch_mutex` 串行，保证
     *   重放事件与实时事件不会交叉乱序；
     * - 失败语义：返回错误时游标不变（`kNotFound` 时调用方应重建订阅）。
     */
    Outcome Replay(std::uint64_t id, std::uint64_t from_revision);

    /**
     * @brief 等待已入队的异步事件全部派发完毕。
     *
     * @return true = 已排空；false = 超时（仍有事件在队列里）。
     *
     * 同步模式无队列，恒返回 true。析构内部也会调用一次（不设超时）。
     */
    bool Flush();

    /// @brief 统计快照。
    MetadataStoreStats GetStats() const;

    /// @brief 最近一次失败的可读原因（无失败为空）；线程安全（返回拷贝）。
    std::string LastError() const;

    /// @brief WAL 是否可用（内存模式恒为 true）。
    bool persistent() const noexcept;

  private:
    /// 一个活跃订阅：前缀 + 回调 + 已派发到的 revision（游标）。
    struct Subscription {
        std::string prefix;
        Callback callback;
        std::uint64_t cursor{0};
    };

    explicit MetadataStore(MetadataStoreOptions options);

    /// @brief 打开并恢复 WAL；失败返回 false 并写 `m_last_error`。
    bool OpenWal();
    /// @brief 追加一条记录并（可选）fdatasync；调用方必须持有 `m_mutex`。
    bool AppendWalLocked(const MetadataEvent &event);
    /// @brief 关闭 WAL 文件（幂等）。
    void CloseWal();
    /// @brief 异步派发线程主体。
    void DispatchLoop();
    /// @brief 把一个事件投递给所有匹配订阅；调用方必须持有 `m_dispatch_mutex`。
    void DeliverLocked(const MetadataEvent &event, std::uint64_t *matched);
    /// @brief 把事件放入派发队列并唤醒派发线程；调用方必须持有 `m_mutex`。
    void EnqueueLocked(const MetadataEvent &event);
    /// @brief 队列满时等待派发线程腾出空间；**不得**在持有 `m_mutex` 时调用。
    void AwaitQueueSpace();

    MetadataStoreOptions m_options;

    mutable std::mutex m_mutex;
    std::map<std::string, MetadataEntry> m_entries;
    std::deque<MetadataEvent> m_history;  ///< 最近 `max_events` 条，供 Replay。
    std::uint64_t m_revision{0};
    std::uint64_t m_next_subscription{1};
    std::map<std::uint64_t, Subscription> m_subscriptions;

    // 派发状态：异步线程与同步路径共用计数器，`Flush()` 依据两者差值等待。
    mutable std::mutex m_dispatch_mutex;  ///< 串行化所有回调调用（含 Replay）。
    mutable std::mutex m_queue_mutex;  ///< mutable：`GetStats()`（const）也要读计数。
    std::condition_variable m_queue_cv;
    /// 队列腾出空间时唤醒背压中的写者。**必须与 `m_queue_cv` 分开**：同一个
    /// cv 上的 notify_one 可能唤醒错误的一方（写者被唤醒后谓词不成立又睡回去，
    /// 真正该醒的派发线程反而收不到通知），事件会滞留在队列里。
    std::condition_variable m_space_cv;
    std::condition_variable m_flush_cv;
    std::deque<MetadataEvent> m_queue;
    std::uint64_t m_enqueued{0};
    std::uint64_t m_delivered{0};
    std::uint64_t m_matched{0};
    std::uint64_t m_replayed{0};
    std::uint64_t m_callback_errors{0};
    std::uint64_t m_puts{0};
    std::uint64_t m_deletes{0};
    std::thread m_dispatcher;
    bool m_stop_dispatcher{false};
    bool m_closed{false};

    int m_wal_fd{-1};
    std::uint64_t m_recover_records{0};
    std::uint64_t m_recovered_bytes{0};
    std::uint64_t m_truncated_bytes{0};
    std::string m_last_error;
    /// 最近一次打开/恢复失败的分类：IO/权限 → kUnavailable，
    /// WAL 内容损坏/超限 → kProtocolError。
    Status m_last_status{Status::kOk};
};

} // namespace go2cpp::distributed
