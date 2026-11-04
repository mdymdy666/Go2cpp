#pragma once

/**
 * @file realtime_room.hpp
 * @brief 通用实时房间组件：房间聚合任意连接 + 每连接有界队列的异步广播，
 *        并提供可复用的「单调序号 + 环形缓冲」断线续传日志。
 *
 * @details 从 `app/bilibili` 的弹幕房间抽出的**业务无关**版本：房间不再绑定
 *          video_id，连接不再绑定 WebSocket，发送动作由业务通过 `SendFn` 注入。
 *          形状是「一个 hub 管若干房间，一个房间管若干连接，一条连接一个
 *          发送线程 + 一个有界队列」：
 *
 *          - `Broadcast()` 只入队、不碰 socket：慢连接填满自己的队列后被踢出，
 *            房间内其它连接不受影响；
 *          - 队列长度由 `RoomConfig::send_queue_cap` 控制（默认 256）；
 *          - 所有公开方法线程安全；`Leave()` / `CloseAll()` 幂等。
 *
 *          `SeqJournal` 是同一组件内的续传基础件：广播消息分配单调递增的
 *          `seq` 并保留最近 N 条，客户端重连时用 `last_seq` 取回漏掉的区间；
 *          窗口已滚出时由调用方回 `resume_gap` 让客户端全量刷新。
 *
 * ## SendFn 约定
 *
 *          返回 true 表示这条消息已交给对端，false（或抛异常）表示连接已坏，
 *          hub 会停止该连接的发送线程并在后续操作中摘除它。`SendFn` 在**该连接
 *          的发送线程**上执行，因此允许阻塞（例如写一个慢 socket）；但不要回调
 *          同一个 `RoomHub`——`Leave()` / `CloseAll()` 会等发送线程退出。
 *
 * ## 用法
 *
 * ```cpp
 * go2cpp::app::realtime::RoomHub hub;
 * const auto id = hub.Join("lobby", [this](std::string_view text) {
 *     return socket->Send(text);     // 业务自己决定怎么发
 * });
 * hub.Broadcast("lobby", R"({"type":"ready"})");
 * hub.Leave(id);                     // 停线程并从房间摘除
 * ```
 *
 * @author go2cpp contributors
 * @date 2026-11-03
 * @copyright MIT License
 */

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace go2cpp::app::realtime {

/// @brief hub 级配置。
struct RoomConfig {
    /// @brief 每条连接的发送队列上限；队列满即踢出该连接（0 视为 1）。
    std::size_t send_queue_cap{256};
};

/**
 * @brief 单房间的「单调序号 + 环形缓冲」，为 WebSocket 断线续传提供重放窗口。
 *
 * 每条广播经 `Assign()` 分配一个从 1 开始、只增不减的序号，并把 `"seq":N`
 * 注入 JSON 对象首部（形如 `{"seq":7,"type":"danmaku",...}`）。文本消息同时
 * 写入长度为 `capacity` 的环形缓冲，客户端重连时携带 `last_seq`，
 * 用 `Since()` 取回 `seq > last_seq` 的消息。
 *
 * `last_seq` 早于缓冲起点（消息已滚出）或大于当前最大序号（服务端重启后
 * 序号回退）时，`Since()` 置 `gap=true`，调用方应回 `{"type":"resume_gap",
 * "earliest":x}` 让客户端全量刷新，而不是静默丢一段。
 *
 * 线程安全：**本类不持锁**，由持有者用同一把锁保护（`bili::RoomHub` 的
 * `m_mutex`）；`Assign()` 与 `Since()` 必须串行，否则会拿到撕裂的快照。
 */
class SeqJournal {
   public:
    /// @brief 默认环形缓冲容量：1000 条（约数百 KB 文本）。
    static constexpr std::size_t kDefaultCapacity = 1000;

    /// @brief 构造；`capacity` 为 0 时按 1 处理（至少保留最后一条）。
    explicit SeqJournal(std::size_t capacity = kDefaultCapacity)
        : m_capacity(capacity == 0 ? 1 : capacity) {}

    /**
     * @brief 为一条广播分配序号、注入 `"seq"` 字段并写入环形缓冲。
     * @param payload [in] 广播正文——借用，必须是 JSON 对象（首字符 `{`）；
     *        非对象按原样保存（客户端拿不到 seq，续传不可用，属调用方错误）。
     * @param seq [out] 成功时写入本次分配的序号（从 1 开始单调递增）；
     *        调用方可传 nullptr 表示不关心。
     * @return 实际要发给客户端的文本：`{"seq":N,` + `payload` 去掉首个 `{`。
     * ## 定位
     * 序号分配与缓冲写入的**唯一入口**。先分配后写缓冲的顺序不能换：反了会让
     * 缓冲里出现比计数器更大的序号，重连补发时出现空洞或重复。
     * ## 步骤
     * step1. 取当前计数作为本次序号并自增（互斥由调用方保证）；
     * step2. payload 是 JSON 对象则把 `"seq":N,` 插到首字符之后，否则原样；
     * step3. 追加到环形缓冲，超出容量从队首淘汰。
     * ## 并发/失败语义
     * - 调用约束：调用方必须持锁；`Assign()` 与 `Since()` 互斥。
     * - 原子性：序号与缓冲在同一次调用内一致更新，不会出现「有号无消息」。
     * - 失败语义：不抛异常（仅内存不足时 std::bad_alloc）。
     * - 不变量：返回文本携带的 seq 与缓冲中的序号一致；缓冲按 seq 严格递增。
     */
    std::string Assign(std::string_view payload, std::uint64_t* seq) {
        const std::uint64_t assigned = m_next_seq++;
        if (seq != nullptr) {
            *seq = assigned;
        }
        std::string message;
        if (payload.size() >= 2 && payload.front() == '{') {
            const std::string seq_text = std::to_string(assigned);
            message.reserve(payload.size() + seq_text.size() + 10);
            message.append("{\"seq\":");
            message.append(seq_text);
            message.push_back(',');
            message.append(payload.substr(1));
        } else {
            message.assign(payload);
        }
        m_buffer.emplace_back(assigned, std::move(message));
        while (m_buffer.size() > m_capacity) {
            m_buffer.pop_front();
        }
        return m_buffer.back().second;
    }

    /// @brief `Since()` 的结果：可补发窗口或「必须全量刷新」信号。
    struct Replay {
        /// @brief true：`last_seq` 已滚出缓冲或大于最大序号，调用方回 resume_gap。
        bool gap{false};
        /// @brief 缓冲内最小序号；缓冲为空时为 0。
        std::uint64_t earliest{0};
        /// @brief 当前已分配的最大序号；尚无消息时为 0。
        std::uint64_t latest{0};
        /// @brief 需要补发的消息（seq 严格递增、均大于 last_seq）；gap 时为空。
        std::vector<std::string> messages;
    };

    /**
     * @brief 取回 `seq > last_seq` 的可补发消息，并判断是否已出现空洞。
     * @param last_seq [in] 客户端已收到的最大序号——0 表示「从最早一条开始」；
     *        由调用方保证来自可信解析（非负整数）。
     * @return 补发快照：`gap=false` 时 `messages` 覆盖 `(last_seq, latest]` 的全部
     *         消息；`gap=true` 时 `messages` 为空，调用方**必须**改用
     *         `resume_gap` 让客户端全量刷新，**不要**静默只补一部分。
     * ## 定位
     * 续传的唯一判定点。写错会导致客户端漏消息却以为连续（数据一致性事故），
     * 或把已送达消息重复补发（弹幕重复上屏）。
     * ## 步骤
     * step1. `last_seq` 大于最大序号 → 服务端序号回退（进程重启），判 gap；
     * step2. `last_seq + 1` 小于缓冲起点 → 窗口已滚出，判 gap；
     * step3. 否则按序拷贝所有 `seq > last_seq` 的消息。
     * ## 并发/失败语义
     * - 调用约束：调用方必须持锁；返回值是调用时刻的快照，之后的新广播不包含。
     * - 原子性：earliest / latest / messages 来自同一次遍历，互相自洽。
     * - 失败语义：不抛异常；空缓冲返回 `earliest=0, latest=0, messages={}`。
     * - 不变量：`gap=false` 时 messages 恰好是 `(last_seq, latest]` 的连续子序列。
     */
    Replay Since(std::uint64_t last_seq) const {
        Replay replay;
        replay.latest = latest();
        replay.earliest = earliest();
        if (last_seq > replay.latest) {
            replay.gap = true;
            return replay;
        }
        if (replay.earliest != 0 && last_seq + 1 < replay.earliest) {
            replay.gap = true;
            return replay;
        }
        replay.messages.reserve(m_buffer.size());
        for (const auto& entry : m_buffer) {
            if (entry.first > last_seq) {
                replay.messages.push_back(entry.second);
            }
        }
        return replay;
    }

    /// @brief 已分配的最大序号（无消息为 0）。
    std::uint64_t latest() const noexcept {
        return m_next_seq == 0 ? 0 : m_next_seq - 1;
    }

    /// @brief 缓冲内最小序号（空缓冲为 0）。
    std::uint64_t earliest() const noexcept {
        return m_buffer.empty() ? 0 : m_buffer.front().first;
    }

    /// @brief 当前缓冲条数。
    std::size_t size() const noexcept { return m_buffer.size(); }

    /// @brief 环形缓冲容量。
    std::size_t capacity() const noexcept { return m_capacity; }

   private:
    std::size_t m_capacity;
    std::uint64_t m_next_seq{1};
    std::deque<std::pair<std::uint64_t, std::string>> m_buffer;
};

/// @brief 房间注册表：room → 连接集合，所有方法线程安全。
class RoomHub {
   public:
    /// @brief 连接 id；进程内单调递增、不复用，0 表示无效。
    using ConnId = std::uint64_t;

    /// @brief 业务注入的发送动作：把一条完整消息发给该连接，失败返回 false。
    using SendFn = std::function<bool(std::string_view)>;

    /// @brief 构造；cfg 为空时使用默认配置（队列上限 256）。
    explicit RoomHub(RoomConfig cfg = {});

    /// @brief 析构：等价于 `CloseAll()`，会等待所有发送线程退出。
    ~RoomHub();

    RoomHub(const RoomHub&) = delete;
    RoomHub& operator=(const RoomHub&) = delete;

    /// @brief 加入房间并启动该连接的发送线程；`send` 为空时返回 0。
    ConnId Join(std::string_view room, SendFn send);

    /// @brief 离开房间并停止发送线程（幂等）；id 不存在时无操作。
    void Leave(ConnId id);

    /// @brief 向房间内所有连接入队一条消息；返回成功入队的连接数。
    std::size_t Broadcast(std::string_view room, std::string_view message);

    /// @brief 房间内仍在线的连接数（已被踢出、等待回收的不计）。
    std::size_t Online(std::string_view room) const;

    /// @brief 所有房间在线连接总数。
    std::size_t TotalOnline() const;

    /// @brief 停止并移除全部连接（幂等）。
    void CloseAll();

   private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace go2cpp::app::realtime
