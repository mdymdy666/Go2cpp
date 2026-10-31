#pragma once

/**
 * @file stream_table.hpp
 * @brief 流表：流状态机 + 连接/流两级窗口 + 依赖树加权调度。
 *
 * @details 本文件是应用层框架的**核心纯逻辑层**（无 IO、无锁、不碰 socket）：
 *          它回答三个问题——
 *
 *          1. **这条帧能不能发/收**（流状态机，RFC 7540 §5.1 的裁剪版）；
 *          2. **现在还能发多少字节**（连接窗口 + 流窗口，RFC 7540 §5.2）；
 *          3. **下一条该发哪条流**（依赖树 + 权重的加权公平调度，§5.3）。
 *
 *          把这三件事从 `Connection` 里剥出来，是为了能用**纯内存、无网络**
 *          的方式穷举测试状态转换与调度公平性——并发问题留下的 bug 最容易
 *          躲进「连接层 + 真实 socket」的组合里。
 *
 * ## 流状态
 *
 * ```
 *            +------+
 *            | idle |
 *            +------+
 *               | send/recv HEADERS
 *               v
 *          +----------+  send ES      +-------------------+
 *          |   open   | -------------> | half_closed_local |
 *          +----------+                +-------------------+
 *            |      | recv ES                 |
 *            |      v                         | recv ES
 *            |  +--------------------+        v
 *            |  | half_closed_remote |     +--------+
 *            |  +--------------------+     | closed |
 *            |      | send ES               +--------+
 *            +------+-------------------------^
 *                   | RST_STREAM / 任一侧关闭
 * ```
 *
 *          `closed` 之后所有操作返回 false；`RST_STREAM` 可从任意状态进入
 *          `closed`。非法转换**不修改状态**（预检语义），调用方据此回
 *          RST_STREAM 或 GOAWAY。
 *
 * ## 两级窗口
 *
 *          - 连接窗口（stream 0）：所有流共享，防止某条流把整条连接吃满；
 *          - 流窗口：每条流独立，防止大资源饿死小 KV。
 *
 *          发送 DATA 前必须 `min(连接窗口, 流窗口) >= 载荷长度`；接收 DATA
 *          时先从两级窗口扣减，上层消费后再用 `Acknowledge*` 归还并发
 *          `WINDOW_UPDATE`。窗口增量为 0 或让窗口超过 `2^31-1` 都是流控错误。
 *
 * ## 优先级：依赖树 + 权重
 *
 *          HTTP/2 的优先级是一棵依赖树：每条流挂在某个父节点下（0 表示根），
 *          并带 1..256 的权重。本实现的调度规则（文档 §4.3 有完整说明）：
 *
 *          1. **父优先**：若某流的祖先也有待发数据且窗口允许，则子流本轮
 *             不参与调度（严格依赖语义）；
 *          2. **加权公平**：其余候选按「虚拟完成时间」`finish` 选择——
 *             `finish += 已发字节 * 2^20 / weight`，每次选 `finish` 最小者。
 *             长期字节数之比收敛到权重之比（WFQ），且无需浮点；
 *          3. 权重变更不重排已完成流量，只影响后续调度。
 *
 *          依赖树带**环检测**：把 A 挂到 A 的后代下会被拒绝（返回 false），
 *          不会让调度器陷入死循环。孤儿（父不存在）挂到根。
 *
 * ## 发送源（FrameProducer）
 *
 *          流表不保存「待发数据」本身，而是保存一个**生产者**：连接层在
 *          窗口允许时调用 `Produce(max_payload, &frame)` 现场生成一帧。
 *          这样 1 GiB 资源不需要先全部驻留内存，也天然支持断点续传。
 *          `Stream::pending` 缓存「已生产、尚未发出」的唯一一帧；优先级
 *          调度与流控都针对这一帧做判定。
 *
 * ## 线程模型
 *
 *          非线程安全。`Connection` 用一把 IO 锁串行化对它的全部访问；
 *          单独使用流表时由调用方保证单驱动者。
 *
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "go2cpp/app/frame.hpp"

namespace go2cpp::app {

/// @brief 流状态（RFC 7540 §5.1 的裁剪版）。
enum class StreamState : std::uint8_t {
    kIdle = 0,              ///< 尚未打开。
    kOpen = 1,              ///< 双向打开。
    kHalfClosedLocal = 2,   ///< 本端已发 END_STREAM。
    kHalfClosedRemote = 3,  ///< 对端已发 END_STREAM。
    kClosed = 4,            ///< 已关闭（正常或 RST）。
};

/// @brief 流状态名。
const char* StreamStateName(StreamState state) noexcept;

/// @brief 流承载的业务类型（由 HEADERS 里的 `app.kind` 决定）。
enum class StreamKind : std::uint8_t {
    kControl = 0,   ///< 控制流（索引/PING 等）。
    kKv = 1,        ///< KV 流。
    kResource = 2,  ///< 资源传输流。
    kBytes = 3,     ///< 裸字节流。
};

/// @brief 流类型名。
const char* StreamKindName(StreamKind kind) noexcept;

/// @brief 流类型对应的 `app.kind` 键值。
const char* StreamKindKey(StreamKind kind) noexcept;

/// @brief 从 `app.kind` 键值解析流类型。
bool ParseStreamKind(std::string_view key, StreamKind* kind) noexcept;

/**
 * @brief 帧生产者：按需生成下一条帧。
 *
 * @details 生产者由连接层在**窗口允许时**调用，生成的帧直接进入发送调度。
 *          实现必须遵守：
 *          - 载荷长度不超过 `max_payload`（包含帧自身协议头）；
 *          - DATA 帧的流 id 由流表填写，生产者不需要设置；
 *          - 最后一帧必须置 `END_STREAM`，之后 `done()` 返回 true；
 *          - `Produce` 返回 false 表示暂时无帧（`done()` 为 true 表示已结束）。
 */
class FrameProducer {
   public:
    virtual ~FrameProducer() = default;

    /// @brief 生成下一帧。
    virtual bool Produce(std::uint32_t max_payload, Frame* out) = 0;

    /// @brief 是否已经全部生产完毕。
    virtual bool done() const = 0;

    /// @brief 已生产的字节数（统计用）。
    virtual std::uint64_t produced_bytes() const { return 0; }
};

/// @brief 帧生产者的唯一所有权指针。
using FrameProducerPtr = std::unique_ptr<FrameProducer>;

/**
 * @brief 一条流的状态、窗口、优先级与发送源。
 *
 * @note 由 `StreamTable` 创建并持有；外部只通过指针访问，不直接构造。
 */
struct Stream {
    std::uint32_t id = 0;                      ///< 流 id。
    StreamState state = StreamState::kIdle;    ///< 当前状态。
    StreamKind kind = StreamKind::kKv;         ///< 业务类型。
    std::uint16_t weight = 16;                 ///< 权重（1..256）。
    std::uint32_t parent_id = 0;               ///< 依赖树父节点（0=根）。
    bool local = false;                        ///< 是否本端发起。
    bool opened = false;                       ///< 是否已收到/发出 HEADERS。
    bool local_closed = false;                 ///< 本端发送方向已关闭。
    bool remote_closed = false;                ///< 对端发送方向已关闭。
    bool reset = false;                        ///< 是否被 RST_STREAM 重置。
    bool producer_done = false;                ///< 发送源是否已结束。

    std::int64_t send_window = 0;              ///< 本端可发送窗口。
    std::int64_t recv_window = 0;              ///< 对端可发送窗口（本端接收额度）。
    std::uint64_t consumed_since_update = 0;   ///< 已消费未归还的接收字节。
    std::uint64_t bytes_sent = 0;              ///< 本流发送字节。
    std::uint64_t bytes_received = 0;          ///< 本流接收字节。
    std::uint64_t frames_sent = 0;             ///< 本流发送帧数。
    std::uint64_t frames_received = 0;         ///< 本流接收帧数。
    std::uint64_t opened_at_ms = 0;            ///< 打开时刻（毫秒）。
    std::uint64_t closed_at_ms = 0;            ///< 关闭时刻（毫秒）。
    std::uint32_t reset_error = 0;             ///< RST_STREAM 错误码。
    std::int64_t finish = 0;                   ///< WFQ 虚拟完成时间。

    FrameProducerPtr producer;                 ///< 发送源。
    Frame pending;                             ///< 已生产未发出的帧（缓冲复用）。
    bool has_pending = false;                  ///< pending 是否有效。

    /// @brief 内部索引哨兵：不在对应容器里。
    static constexpr std::uint32_t kNoSlot = 0xFFFFFFFFu;

    std::uint32_t ready_slot = kNoSlot;        ///< ready 堆内下标（流表维护）。
    std::uint32_t producer_slot = kNoSlot;     ///< 生产者列表内下标（流表维护）。
    std::uint32_t idle_slot = kNoSlot;         ///< 待补产列表内下标（流表维护）。

    /// @brief 已打开（不是 idle/closed）。
    bool active() const noexcept { return opened && state != StreamState::kClosed; }
    /// @brief 是否已关闭。
    bool closed() const noexcept { return state == StreamState::kClosed; }
    /// @brief 是否还有未发送的帧。
    bool has_pending_frame() const noexcept { return has_pending; }
    /// @brief 待发帧的载荷字节数。
    std::uint32_t pending_bytes() const noexcept {
        return has_pending ? pending.Length() : 0u;
    }
    /// @brief 当前状态是否允许发送。
    bool can_send() const noexcept {
        return opened && !local_closed && !reset;
    }
    /// @brief 当前状态是否允许接收。
    bool can_receive() const noexcept {
        return opened && !remote_closed && !reset;
    }
};

/// @brief 流表配置。
struct StreamTableConfig {
    bool client = true;                        ///< true=本端流 id 用奇数（HTTP/2 约定）。
    std::uint32_t max_concurrent_streams = 256;         ///< 并发流上限。
    std::uint32_t max_retired_streams = 256;            ///< 已关闭流保留上限。
    std::int64_t initial_stream_window = 65535;         ///< 初始流窗口。
    std::int64_t connection_window = 1024 * 1024;       ///< 连接级发送窗口。
    std::uint32_t max_frame_payload = 256 * 1024;       ///< 单帧载荷上限。
    std::uint16_t default_weight = 16;                  ///< 默认权重。
    std::uint32_t max_stream_id = kMaxStreamId;         ///< 最大流 id。
};

/// @brief 流表状态名（调试用）。
const char* StreamTableStateName(StreamState state) noexcept;

/**
 * @brief 流表：状态机 + 窗口 + 依赖树调度。
 */
class StreamTable {
   public:
    /// @brief 流 id -> 流对象。
    using StreamMap = std::map<std::uint32_t, std::unique_ptr<Stream>>;

    /// @brief 构造；`config` 非法（窗口<=0/权重=0）时回退默认值。
    explicit StreamTable(StreamTableConfig config = {});

    /// @brief 当前配置（只读）。
    const StreamTableConfig& config() const noexcept { return m_config; }

    // ---- 创建 / 查找 / 删除 ----

    /**
     * @brief 创建一条本端发起的流（id 自动分配）。
     * @param kind 业务类型。
     * @param producer 发送源；可为空（稍后直接 `SendHeaders`）。
     * @param weight 权重（1..256；0=默认）。
     * @param parent 依赖树父节点（0=根）。
     * @param error 非空时写入失败原因。
     * @return 新建的流；失败返回 nullptr。
     */
    Stream* CreateLocal(StreamKind kind, FrameProducerPtr producer = nullptr,
                        std::uint16_t weight = 0, std::uint32_t parent = 0,
                        std::string* error = nullptr);

    /**
     * @brief 创建一条对端发起的流（由收到的 HEADERS 触发）。
     * @param id 对端分配的流 id。
     * @param kind 业务类型。
     * @param error 非空时写入失败原因。
     * @return 新建的流；失败返回 nullptr。
     */
    Stream* CreateRemote(std::uint32_t id, StreamKind kind, std::string* error = nullptr);

    /// @brief 查找流。
    Stream* Find(std::uint32_t id) noexcept;
    const Stream* Find(std::uint32_t id) const noexcept;

    /// @brief 删除流（迟到帧会被当作 closed 忽略）。
    bool Erase(std::uint32_t id) noexcept;

    /// @brief 全部流（含已关闭但尚未清理的）。
    const StreamMap& streams() const noexcept { return m_streams; }

    // ---- 状态机 ----

    /// @brief 发送 HEADERS；成功返回 true。
    bool SendHeaders(Stream& stream, bool end_stream) noexcept;
    /// @brief 接收 HEADERS；成功返回 true。
    bool ReceiveHeaders(Stream& stream, bool end_stream) noexcept;
    /// @brief 发送 DATA（只检查状态，不扣窗口）；成功返回 true。
    bool SendData(Stream& stream, std::size_t bytes, bool end_stream) noexcept;
    /// @brief 接收 DATA（检查状态与接收窗口，扣减窗口）；成功返回 true。
    bool ReceiveData(Stream& stream, std::size_t bytes, bool end_stream) noexcept;
    /// @brief 重置单条流；成功返回 true。
    bool Reset(Stream& stream, ErrorCode error) noexcept;
    /// @brief 正常关闭流。
    bool Close(Stream& stream) noexcept;

    // ---- 发送源与调度 ----

    /// @brief 给流挂接发送源（替换旧的）。
    void AttachProducer(Stream& stream, FrameProducerPtr producer) noexcept;

    /**
     * @brief 为流生产下一帧（若尚未有 pending，且窗口足够）。
     * @return 是否已就绪。
     */
    bool FillPending(Stream& stream, std::uint32_t max_payload = 0);

    /**
     * @brief 选择下一条可发送的流，并确保它有 pending 帧。
     * @details 内部先对所有流尝试 `FillPending`（窗口允许时），再按
     *          「父优先 + WFQ」选择。返回 nullptr 表示当前没有可发送的流
     *          （全部完成或被流控阻塞）。
     */
    Stream* PickNext(std::uint32_t max_payload = 0);

    /// @brief 是否还有未完成的发送源或待发帧。
    bool HasWork() const noexcept;

    /// @brief 已就绪（pending 且窗口允许）的流数量。
    std::size_t ReadyCount() const noexcept;

    /// @brief 因窗口不足而阻塞的流数量。
    std::size_t BlockedCount() const noexcept;

    /// @brief 扣除一条 DATA 帧的发送窗口与统计（发送成功后调用）。
    bool ConsumeSend(Stream& stream, std::size_t bytes) noexcept;

    /**
     * @brief 取走并清空流的待发帧（发送成功后调用）。
     * @details 释放 `has_pending` 并移出 ready 堆，**保留 payload 缓冲容量**，
     *          下一帧生产时零分配复用。这是热路径「每帧零 malloc」的关键。
     */
    void ConsumePending(Stream& stream) noexcept;

    /// @brief 把流重新放回 ready 堆（发送回滚等罕见路径用）。
    void Requeue(Stream& stream) noexcept;

    /// @brief WFQ 信用推进（发送成功后调用，参数为已发字节）。
    void ChargeFinish(Stream& stream, std::size_t bytes) noexcept;

    // ---- 窗口 ----

    /// @brief 连接级发送窗口。
    std::int64_t connection_send_window() const noexcept { return m_conn_send_window; }

    /// @brief 该流的有效发送窗口：`min(连接窗口, 流窗口)`。
    std::int64_t SendWindow(const Stream& stream) const noexcept;

    /// @brief 收到 WINDOW_UPDATE；`stream_id==0` 表示连接窗口。
    bool OnWindowUpdate(std::uint32_t stream_id, std::uint32_t increment,
                        std::string* error = nullptr);

    /// @brief 对端 SETTINGS 调整初始流窗口（RFC 7540 §6.9.2）。
    bool SetInitialWindowSize(std::int64_t new_window, std::string* error = nullptr);

    /**
     * @brief 上层消费了流上的接收数据，归还接收窗口。
     * @return 需要发送的流级 WINDOW_UPDATE 增量（0 = 暂不需要）。
     */
    std::uint32_t AcknowledgeReceived(Stream& stream, std::size_t bytes) noexcept;

    /**
     * @brief 上层消费了连接上的接收数据，归还连接窗口。
     * @return 需要发送的连接级 WINDOW_UPDATE 增量（0 = 暂不需要）。
     */
    std::uint32_t AcknowledgeConnection(std::size_t bytes) noexcept;

    // ---- 优先级 ----

    /**
     * @brief 更新依赖关系与权重（等价 PRIORITY 帧）。
     * @return 父节点不存在或形成环时返回 false（不修改）。
     */
    bool SetPriority(std::uint32_t id, std::uint32_t parent, std::uint16_t weight,
                     std::string* error = nullptr);

    /// @brief `ancestor` 是否是 `id` 的祖先（环检测用）。
    bool IsAncestor(std::uint32_t ancestor, std::uint32_t id) const noexcept;

    /// @brief 有效权重（1..256）。
    std::uint16_t WeightOf(std::uint32_t id) const noexcept;

    // ---- 关闭 ----

    /**
     * @brief 记录 GOAWAY（之后拒绝新建本端流）。
     */
    void GoAway(std::uint32_t last_stream_id, ErrorCode error, std::string debug = {});

    /// @brief 是否已发送/收到 GOAWAY。
    bool goaway() const noexcept { return m_goaway; }
    /// @brief GOAWAY 的最后流 id。
    std::uint32_t goaway_last_stream_id() const noexcept { return m_goaway_last_stream_id; }
    /// @brief GOAWAY 错误码。
    ErrorCode goaway_error() const noexcept { return m_goaway_error; }
    /// @brief GOAWAY 调试字符串。
    const std::string& goaway_debug() const noexcept { return m_goaway_debug; }

    // ---- 统计 ----

    /// @brief 活跃（open/half-closed）流数量。
    std::size_t active_streams() const noexcept;
    /// @brief 流表中全部流数量（含待清理的 closed 流）。
    std::size_t stream_count() const noexcept { return m_streams.size(); }
    /// @brief 累计创建的流数量。
    std::uint64_t total_created() const noexcept { return m_total_created; }
    /// @brief 累计关闭的流数量。
    std::uint64_t total_closed() const noexcept { return m_total_closed; }
    /// @brief 累计发送字节（载荷）。
    std::uint64_t total_bytes_sent() const noexcept { return m_total_bytes_sent; }
    /// @brief 累计接收字节（载荷）。
    std::uint64_t total_bytes_received() const noexcept { return m_total_bytes_received; }
    /// @brief 累计 RST_STREAM 次数。
    std::uint64_t total_resets() const noexcept { return m_total_resets; }
    /// @brief 已清理的已关闭流数量。
    std::uint64_t retired_streams() const noexcept { return m_retired_streams; }
    /// @brief 等待发送的流数量。
    std::size_t pending_streams() const noexcept;
    /// @brief 最近一次本地流 id。
    std::uint32_t last_local_stream_id() const noexcept { return m_last_local_id; }

    /// @brief 执行一次已关闭流清理，保留 `max_retired_streams` 条。
    void PruneClosedStreams();

    /// @brief 重置统计（不影响流）。
    void ResetStats() noexcept;


   private:
    void RefreshState(Stream& stream) noexcept;
    bool HasReadyAncestor(const Stream& stream) const noexcept;
    bool IsReady(const Stream& stream) const noexcept;
    void MarkClosed(Stream& stream) noexcept;
    std::uint32_t AllocateLocalId();
    void RecordCreated();
    std::size_t LiveStreamCount() const noexcept;

    // ---- ready 最小堆（按 (finish, id)）与生产者索引 ----
    void HeapSwap(std::size_t a, std::size_t b) noexcept;
    void SiftUp(std::size_t i) noexcept;
    void SiftDown(std::size_t i) noexcept;
    void PushReady(Stream& stream) noexcept;
    void RemoveReady(Stream& stream) noexcept;
    void RefillReady(std::uint32_t max_payload);
    void AddProducer(Stream& stream) noexcept;
    void RemoveProducerAt(std::size_t index) noexcept;
    void AddIdle(Stream& stream) noexcept;
    void RemoveIdleAt(std::size_t index) noexcept;
    void DrainIdle(std::uint32_t max_payload);
    void DetachIndexes(Stream& stream) noexcept;

    StreamTableConfig m_config;              ///< 配置。
    StreamMap m_streams;                     ///< 流表。
    std::vector<Stream*> m_ready;            ///< ready 最小堆（就绪流，仅堆内有效）。
    std::vector<Stream*> m_producers;        ///< 挂有发送源的流（生产候选）。
    std::vector<Stream*> m_idle;             ///< 无待发帧、等待补产的流（通常 1 条）。
    std::vector<Stream*> m_held;             ///< PickNext 的父优先暂存（复用）。
    bool m_refill_hint = true;               ///< 连接级窗口恢复后提示 refill 一次。
    std::deque<std::uint32_t> m_closed_order;///< 关闭顺序（O(1) 淘汰）。
    std::size_t m_live_streams = 0;          ///< 非 closed 流计数。
    std::uint32_t m_next_local_id = 1;       ///< 下一个本端流 id。
    std::uint32_t m_last_local_id = 0;       ///< 最近分配的本端流 id。
    std::int64_t m_conn_send_window = 0;     ///< 连接级发送窗口。
    std::int64_t m_conn_recv_window = 0;     ///< 连接级接收窗口。
    std::int64_t m_initial_window = 0;       ///< 初始流窗口。
    std::uint64_t m_conn_consumed = 0;       ///< 连接级已消费未归还字节。
    std::uint64_t m_total_created = 0;       ///< 累计创建。
    std::uint64_t m_total_closed = 0;        ///< 累计关闭。
    std::uint64_t m_total_bytes_sent = 0;    ///< 累计发送。
    std::uint64_t m_total_bytes_received = 0;///< 累计接收。
    std::uint64_t m_total_resets = 0;        ///< 累计重置。
    std::uint64_t m_retired_streams = 0;     ///< 已清理数量。
    bool m_goaway = false;                   ///< GOAWAY 标志。
    std::uint32_t m_goaway_last_stream_id = 0;  ///< GOAWAY 最后流 id。
    ErrorCode m_goaway_error = ErrorCode::kNoError;  ///< GOAWAY 错误码。
    std::string m_goaway_debug;              ///< GOAWAY 调试字符串。
};

}  // namespace go2cpp::app
