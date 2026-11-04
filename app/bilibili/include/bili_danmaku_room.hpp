/**
 * @file bili_danmaku_room.hpp
 * @brief 弹幕房间：按 video_id 聚合 WebSocket 连接，异步有界广播，
 *        并用单调序号 + 环形缓冲支持断线续传（Resume）。
 *
 * @details 每条连接一个发送线程 + 有界队列（默认 256 条）。`Broadcast()`
 *          只做入队，不碰 socket，因此一个慢连接不会拖住房间内其它连接；
 *          队列满时由 `RoomConnection::Enqueue` 主动踢出该连接。
 *
 *          每条广播先经 `SeqJournal::Assign()` 分配房间内单调递增的 `seq`
 *          （从 1 开始），再写入容量 `journal_capacity`（默认 1000）的环形
 *          缓冲。客户端重连时在握手 query 带 `last_seq=N`，`JoinWithResume()`
 *          在**同一把锁内**完成「补发 (N, latest] → 登记连接」，因此补发消息
 *          一定排在后续实时消息之前，不会乱序。
 *
 * @author bili demo
 * @date 2026-11-03
 * @copyright MIT License
 */

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "go2cpp/app/realtime_room.hpp"
#include "go2cpp/network/websocket.hpp"

namespace bili {

/// @brief 一条已升级的 WS 连接：发送线程 + 有界发送队列。
class RoomConnection {
   public:
    using Ptr = std::shared_ptr<RoomConnection>;

    /// @brief 构造；`max_queue` 为积压上限，超过即踢出。
    RoomConnection(go2cpp::net::WebSocket::Ptr websocket, std::string peer,
                   std::size_t max_queue = 256);
    ~RoomConnection();
    RoomConnection(const RoomConnection&) = delete;
    RoomConnection& operator=(const RoomConnection&) = delete;

    /// @brief 启动发送线程（Join 房间前调用一次）。
    void Start();

    /// @brief 入队一条完整 JSON 文本；队列满返回 false 并踢出本连接。
    bool Enqueue(std::string message);

    /// @brief 主动关闭（幂等）：置停止位、唤醒发送线程、发 Close 帧。
    void Close();

    /// @brief 连接是否已关闭。
    bool closed() const noexcept { return m_closed.load(std::memory_order_acquire); }

    /// @brief 是否因塞满队列被踢。
    bool slow_consumer() const noexcept {
        return m_slow.load(std::memory_order_acquire);
    }

    /// @brief 唯一连接 id（限流/日志用）。
    std::uint64_t id() const noexcept { return m_id; }

    /// @brief 对端地址（日志用）。
    const std::string& peer() const noexcept { return m_peer; }

    /// @brief 已成功发出的消息条数。
    std::uint64_t sent() const noexcept { return m_sent.load(std::memory_order_relaxed); }

   private:
    void SendLoop();

    go2cpp::net::WebSocket::Ptr m_websocket;
    std::string m_peer;
    std::size_t m_max_queue;
    std::uint64_t m_id;

    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<std::string> m_queue;
    bool m_stopping{false};

    std::thread m_thread;
    std::atomic<bool> m_closed{false};
    std::atomic<bool> m_slow{false};
    std::atomic<std::uint64_t> m_sent{0};
};

/// @brief video_id → 连接集合；所有方法线程安全。
class RoomHub {
   public:
    /// @brief 构造；`journal_capacity` 是每房间续传缓冲条数（0 视为 1）。
    explicit RoomHub(std::size_t journal_capacity =
                         go2cpp::app::realtime::SeqJournal::kDefaultCapacity);

    /// @brief 一次续传的判定结果，供服务层回 resume_gap 与写日志。
    struct ResumeReport {
        /// @brief 调用方是否带了 last_seq（false 表示普通新连接，不补发）。
        bool resumed{false};
        /// @brief true：last_seq 已滚出缓冲或大于最大序号，应让客户端全量刷新。
        bool gap{false};
        /// @brief 缓冲内最小序号（无缓冲为 0）；gap 时客户端应据此全量刷新。
        std::uint64_t earliest{0};
        /// @brief 补发判定时刻的最大序号。
        std::uint64_t latest{0};
        /// @brief 实际入队成功的补发条数（非 gap 时等于 (last_seq, latest] 的条数）。
        std::size_t replayed{0};
    };

    /// @brief 加入房间（不补发历史）；连接为空返回 false。
    bool Join(std::int64_t video_id, RoomConnection::Ptr connection);

    /**
     * @brief 加入房间并（可选）补发 `seq > last_seq` 的缓冲消息。
     * @param video_id [in] 房间号；不存在时创建。
     * @param connection [in] 连接对象——所有权共享，不可为空；空则返回 false。
     * @param resume [in] 是否处理续传：false 等价于 `Join()`，不补发。
     * @param last_seq [in] 客户端已收到的最大序号；`resume=false` 时忽略。
     * @param report [out] 成功时写入判定结果；可为 nullptr（不关心）。
     * @return true：已登记（gap 时也已登记，调用方需回 resume_gap）；
     *         false：connection 为空，未登记——调用方**不要**继续入队。
     * ## 定位
     * 续传的唯一入口。补发必须与登记在同一临界区内完成：若先登记再补发，
     * 并发广播会把实时消息插到补发前面，客户端会看到序号倒退/乱序。
     * ## 步骤
     * step1. 持 hub 锁；连接已存在则直接返回（幂等，避免重复补发）；
     * step2. 取（必要时创建）本房间 journal，计算 `Since(last_seq)` 快照；
     * step3. gap 时不补发，只填 report，仍登记连接（由服务层回 resume_gap）；
     * step4. 非 gap 时按序把补发消息 Enqueue 到该连接（此时连接尚未登记，
     *        并发广播拿不到锁，因此不会插队）；
     * step5. 登记连接并回填 replayed。
     * ## 并发/失败语义
     * - 调用约束：任意线程；持 hub 锁调用 `Enqueue`（只入队，不碰 socket）。
     * - 原子性：补发与登记原子完成，之后的广播必然排在补发之后。
     * - 失败语义：不抛异常；补发途中队列满会踢出该连接并提前结束补发。
     * - 不变量：非 gap 时补发条数 == report.latest - last_seq（队列未溢出时）。
     */
    bool JoinWithResume(std::int64_t video_id, RoomConnection::Ptr connection,
                        bool resume, std::uint64_t last_seq,
                        ResumeReport* report);

    /// @brief 离开房间（幂等）。
    void Leave(std::int64_t video_id, const RoomConnection::Ptr& connection);

    /**
     * @brief 分配房间序号并向所有连接广播（含离线缓冲）。
     * @param video_id [in] 房间号；无连接时仍分配 seq 并写入缓冲，保证序号连续。
     * @param payload [in] JSON 对象正文——借用，函数内拷贝；非对象时无法注入
     *        `"seq"`（调用方错误，客户端将无法续传）。
     * @return 成功入队的连接数；**不代表已送达**。
     * ## 定位
     * 房间广播的唯一入口。序号分配、缓冲写入、逐连接入队必须在同一把锁内，
     * 否则会出现「缓冲里没有但已发出」或「序号乱序」的窗口，续传直接失效。
     * ## 步骤
     * step1. 持 hub 锁，取（必要时创建）journal 并 Assign 分配 seq；
     * step2. 若房间无连接，仅保留缓冲（seq 仍连续）；
     * step3. 逐连接 Enqueue 带 seq 的文本，统计成功数。
     * ## 并发/失败语义
     * - 调用约束：任意线程；`Enqueue` 只入队不 I/O，允许持锁调用。
     * - 原子性：seq、缓冲、入队在同一临界区内一次完成。
     * - 失败语义：不抛异常；慢连接队列满只减少返回值。
     * - 不变量：任意两次 Broadcast 的 seq 严格递增且无空洞。
     */
    std::size_t Broadcast(std::int64_t video_id, std::string_view payload);

    /**
     * @brief 广播一条**不参与续传**的瞬时状态帧（如 online 在线数）。
     *
     * 这类帧是派生状态：连接加入/离开时重新计算，重连后必然重新广播最新值，
     * 补发旧值只会让客户端显示过期在线数。因此它们不分配 seq、不写缓冲，
     * 只入队给当前在线连接；`seq` 流专用于不可重建的业务消息（弹幕）。
     *
     * @param video_id [in] 房间号；无连接时直接返回 0。
     * @param payload [in] JSON 对象正文——借用，函数内拷贝。
     * @return 成功入队的连接数；不代表已送达。
     */
    std::size_t BroadcastEphemeral(std::int64_t video_id, std::string_view payload);

    /// @brief 房间在线数。
    std::size_t OnlineCount(std::int64_t video_id) const;

    /// @brief 所有房间在线总数。
    std::size_t OnlineTotal() const;

    /// @brief 房间当前最大序号（无消息为 0）。
    std::uint64_t LatestSeq(std::int64_t video_id) const;

    /// @brief 每房间续传缓冲容量。
    std::size_t journal_capacity() const noexcept { return m_journal_capacity; }

   private:
    /// @brief 持锁取（必要时创建）房间 journal；调用方必须已持有 m_mutex。
    go2cpp::app::realtime::SeqJournal& JournalLocked(std::int64_t video_id);

    std::size_t m_journal_capacity;
    mutable std::mutex m_mutex;
    std::map<std::int64_t, std::vector<RoomConnection::Ptr>> m_rooms;
    std::map<std::int64_t, go2cpp::app::realtime::SeqJournal> m_journals;
};

}  // namespace bili
