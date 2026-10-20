#pragma once

/**
 * @file sync_channel.hpp
 * @brief 可记录/可重放的 Go 风格 channel（record/replay 的语义层）。
 *
 * ## 为什么不直接包装 `go2cpp::Channel<T>`
 *
 * 语义级重放要求「所有影响交错的决定点」都可观测、可注入：某次交接把值给
 * 了谁、谁因此被唤醒、唤醒是否丢失。这些决定点在 `Channel<T>` 内部，外部
 * 只能看到操作最终返回，拿不到配对与唤醒关系。因此本文件实现一个**同语义
 * 的最小 channel**，把决定点收拢到一处：
 *
 * - 等待者有自己的 `core::ParkingCondition`（与 `Channel<T>` 相同的 park 原语，
 *   受管 G 会真正让出 M）；
 * - 交接/入队/关闭都在组锁内完成，**事件也在锁内写**，因此记录的
 *   `seq` 顺序等于线性化顺序；
 * - 每个等待者带 `xfer_id`，send/recv 两侧共享，重放可校验配对。
 *
 * ## 两种模式
 *
 * - `kLive`：真实并发 + 真实 park/wake；每个决定点写事件。
 * - `kReplay`：单线程重跑；操作结果从记录的 `(gid, op_index)` 查表，阻塞
 *   操作调用 `ReplaySource::ParkCurrent()` 把 CPU 交还引擎，由引擎按
 *   `kWake` 事件恢复。
 *
 * ## 语义范围（v2）
 *
 * - 无缓冲 rendezvous 与有缓冲（FIFO）两种；`Close` 唤醒所有等待者；
 * - 值类型：整型（i64 槽位）与 string/bytes/结构体（`ValueCodec` 序列化钩子）；
 * - `SyncSelect`（见 `select.hpp`）支持多 channel 竞争、default、timeout、
 *   Context 取消；select 的最终决策记录为一条 `kSelect`。
 *
 * ## 组锁与 select
 *
 * 参与同一个 select 的 channel 必须绑定同一个 `SelectGroup`（构造参数）。
 * 组内所有 channel 操作串行化，select 才能在锁内原子地「轮询所有 case +
 * 登记等待」。不在 group 里的 channel 行为与 v1 完全一致。
 */

#include "go2cpp/concurrency/core/parking_condition.hpp"

#include "replay/format.hpp"
#include "replay/gid.hpp"
#include "replay/recorder.hpp"
#include "replay/replay_source.hpp"
#include "replay/select_state.hpp"
#include "replay/value_codec.hpp"

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <utility>

namespace go2cpp::replay {

/**
 * @brief 可记录/可重放的 channel。
 *
 * @tparam T 消息类型；默认 `ValueCodec<T>` 决定序列化方式。
 * @tparam Codec 值编解码器，可自定义。
 *
 * 生命周期：live 模式下 `Recorder` 必须比 channel 活得久；replay 模式下
 * `ReplaySource` 必须比 channel 活得久。两种模式都**不可拷贝**。
 */
template <typename T, typename Codec = ValueCodec<T>>
class SyncChannel {
    static_assert(!std::is_reference<T>::value,
                  "SyncChannel<T> does not support reference message types");

public:
    using value_type = T;
    using codec_type = Codec;

    /// 接收结果；`decision == Decision::kOk` 时 `value` 有效。
    struct RecvResult {
        Decision decision{Decision::kClosed};
        T value{};
        bool ok() const noexcept { return decision == Decision::kOk; }
    };

    /// live 模式构造；`capacity == 0` 为无缓冲 rendezvous。
    SyncChannel(std::uint32_t channel_id, Recorder& recorder,
                std::size_t capacity = 0, SelectGroup* group = nullptr)
        : m_id(channel_id),
          m_mode(Mode::kLive),
          m_recorder(&recorder),
          m_capacity(capacity),
          m_group(group) {}

    /// replay 模式构造（容量不参与重放，仅用于 `Capacity()` 查询）。
    SyncChannel(std::uint32_t channel_id, ReplaySource& source,
                std::size_t capacity = 0, SelectGroup* group = nullptr)
        : m_id(channel_id),
          m_mode(Mode::kReplay),
          m_source(&source),
          m_capacity(capacity),
          m_group(nullptr) {
        (void)group;  // replay 不需要组锁：决策全部来自记录
    }

    ~SyncChannel() = default;
    SyncChannel(const SyncChannel&) = delete;
    SyncChannel& operator=(const SyncChannel&) = delete;

    std::uint32_t id() const noexcept { return m_id; }
    std::size_t Capacity() const noexcept { return m_capacity; }
    SelectGroup* group() const noexcept { return m_group; }
    bool replay_mode() const noexcept { return m_mode == Mode::kReplay; }

    /// 当前缓冲中的元素个数（live 模式加锁读取；replay 模式返回 0）。
    std::size_t Len() const {
        if (m_mode != Mode::kLive) {
            return 0;
        }
        auto lock = LockOp();
        return m_buffer.size();
    }

    bool IsClosed() const {
        if (m_mode != Mode::kLive) {
            return m_closed;
        }
        auto lock = LockOp();
        return m_closed;
    }

    /**
     * @brief 发送一个值。
     *
     * live：可能真正 park；replay：结果完全来自记录。
     * @return `kOk` / `kClosed` / `kAlreadyClosed`。
     */
    Decision Send(const T& value) {
        if (m_mode == Mode::kReplay) {
            return ReplayOperation(EventType::kSend, m_source->NextOperation(Gid()));
        }
        return LiveSend(value);
    }

    /**
     * @brief 接收一个值。
     * @return `kOk` 时 `value` 有效；`kClosed` 表示 channel 已关闭且无值。
     */
    RecvResult Recv() {
        if (m_mode == Mode::kReplay) {
            const Event* event = m_source->NextOperation(Gid());
            const Decision decision = ReplayOperation(EventType::kRecv, event);
            RecvResult result;
            result.decision = decision;
            if (event != nullptr && decision == Decision::kOk) {
                result.value = Codec::FromValue(event->value, event->data);
            }
            return result;
        }
        return LiveRecv();
    }

    /// 关闭 channel；重复关闭返回 `kAlreadyClosed`（live 模式写相应事件）。
    Decision Close() {
        if (m_mode == Mode::kReplay) {
            return ReplayOperation(EventType::kClose, m_source->NextOperation(Gid()));
        }
        return LiveClose();
    }

    // ---------------------------------------------------------------------
    // select 内部接口（由 `SyncSelect` 在**持有组锁**时调用）
    // ---------------------------------------------------------------------

    /// 尝试在本次 select 里完成一次 recv；成功时结果写入 `state`。
    bool SelectTryRecvLocked(SelectState& state, std::uint32_t case_index) {
        if (!m_buffer.empty()) {
            const T value = m_buffer.front();
            m_buffer.pop_front();
            RefillBufferLocked();
            if (!state.Claim(case_index)) {
                return false;
            }
            state.int_value = Codec::ToInt(value);
            state.data = Codec::ToBytes(value);
            state.selected_channel = m_id;
            state.SetDecision(Decision::kOk);
            return true;
        }
        WaiterPtr sender = PopUsableLocked(m_senders);
        if (sender != nullptr) {
            if (sender->select != nullptr) {
                SelectState& other = *sender->select;
                if (!other.Claim(sender->select_case)) {
                    return false;
                }
                if (!state.Claim(case_index)) {
                    return false;
                }
                const std::uint32_t xfer = m_recorder->NextXferId();
                const T value = sender->value;
                CompleteSelectSendLocked(sender, xfer);
                state.int_value = Codec::ToInt(value);
                state.data = Codec::ToBytes(value);
                state.selected_channel = m_id;
                state.xfer_id = xfer;
                state.SetDecision(Decision::kOk);
                return true;
            }
            const std::uint32_t xfer = m_recorder->NextXferId();
            const T value = sender->value;
            if (!state.Claim(case_index)) {
                return false;
            }
            state.int_value = Codec::ToInt(value);
            state.data = Codec::ToBytes(value);
            state.selected_channel = m_id;
            state.xfer_id = xfer;
            state.SetDecision(Decision::kOk);
            WakeLocked(sender, ParkReason::kSend, xfer);
            return true;
        }
        if (m_closed) {
            if (!state.Claim(case_index)) {
                return false;
            }
            state.selected_channel = m_id;
            state.SetDecision(Decision::kClosed);
            return true;
        }
        return false;
    }

    /// 尝试在本次 select 里完成一次 send；成功时结果写入 `state`。
    bool SelectTrySendLocked(SelectState& state, std::uint32_t case_index,
                             const T& value) {
        const std::int64_t int_value = Codec::ToInt(value);
        const std::string bytes = Codec::ToBytes(value);
        if (m_closed) {
            if (!state.Claim(case_index)) {
                return false;
            }
            state.int_value = int_value;
            state.data = bytes;
            state.selected_channel = m_id;
            state.SetDecision(Decision::kClosed);
            return true;
        }
        WaiterPtr receiver = PopUsableLocked(m_receivers);
        if (receiver != nullptr) {
            if (receiver->select != nullptr) {
                SelectState& other = *receiver->select;
                if (!other.Claim(receiver->select_case)) {
                    return false;
                }
                if (!state.Claim(case_index)) {
                    return false;
                }
                const std::uint32_t xfer = m_recorder->NextXferId();
                CompleteSelectRecvLocked(receiver, value, xfer);
                state.int_value = int_value;
                state.data = bytes;
                state.selected_channel = m_id;
                state.xfer_id = xfer;
                state.SetDecision(Decision::kOk);
                return true;
            }
            if (!state.Claim(case_index)) {
                return false;
            }
            const std::uint32_t xfer = m_recorder->NextXferId();
            state.int_value = int_value;
            state.data = bytes;
            state.selected_channel = m_id;
            state.xfer_id = xfer;
            state.SetDecision(Decision::kOk);
            receiver->value = value;
            WakeLocked(receiver, ParkReason::kRecv, xfer);
            return true;
        }
        if (m_buffer.size() < m_capacity) {
            if (!state.Claim(case_index)) {
                return false;
            }
            m_buffer.push_back(value);
            state.int_value = int_value;
            state.data = bytes;
            state.selected_channel = m_id;
            state.SetDecision(Decision::kOk);
            return true;
        }
        return false;
    }

    /// 在 channel 的 recv 队列里登记一个 select 等待者。
    SelectWaiterToken SelectArmRecvLocked(SelectState& state,
                                         std::uint32_t case_index) {
        WaiterPtr waiter = std::make_shared<Waiter>();
        waiter->gid = state.gid;
        waiter->op_index = state.op_index;
        waiter->reason = ParkReason::kSelect;
        waiter->select = &state;
        waiter->select_case = case_index;
        m_receivers.push_back(waiter);
        return waiter;
    }

    /// 在 channel 的 send 队列里登记一个 select 等待者。
    SelectWaiterToken SelectArmSendLocked(SelectState& state,
                                         std::uint32_t case_index,
                                         const T& value) {
        WaiterPtr waiter = std::make_shared<Waiter>();
        waiter->gid = state.gid;
        waiter->op_index = state.op_index;
        waiter->reason = ParkReason::kSelect;
        waiter->select = &state;
        waiter->select_case = case_index;
        waiter->value = value;
        m_senders.push_back(waiter);
        return waiter;
    }

    /// 撤销一个未命中的登记（已命中时 waiter 已被队列弹出，空操作）。
    void SelectDisarmLocked(const SelectWaiterToken& token) {
        if (!token) {
            return;
        }
        auto* raw = static_cast<Waiter*>(token.get());
        RemoveWaiterLocked(m_receivers, raw);
        RemoveWaiterLocked(m_senders, raw);
    }

private:
    enum class Mode { kLive, kReplay };

    /// 一个等待者：send 等待「值被取走」，recv 等待「值被填入」。
    struct Waiter {
        std::uint32_t gid{0};
        std::uint32_t op_index{0};
        ParkReason reason{ParkReason::kOther};
        T value{};
        bool done{false};
        Decision decision{Decision::kOk};
        std::uint32_t xfer_id{0};
        core::ParkingCondition cv;
        SelectState* select{nullptr};   ///< 非空表示这是 select 的 case 等待者
        std::uint32_t select_case{0};
    };
    using WaiterPtr = std::shared_ptr<Waiter>;

    /// replay 模式取当前 fiber 的 gid；live 模式按 Task::id() 反查。
    std::uint32_t Gid() const noexcept {
        return m_mode == Mode::kReplay ? m_source->CurrentFiberGid()
                                       : detail::ResolveGid();
    }

    std::unique_lock<std::mutex> LockOp() const {
        if (m_group != nullptr) {
            return std::unique_lock<std::mutex>(m_group->mutex());
        }
        return std::unique_lock<std::mutex>(m_mutex);
    }

    void WriteOp(EventType type, std::uint32_t gid, std::uint32_t op_index,
                 Decision decision, std::uint8_t blocked, std::int64_t value,
                 std::uint32_t xfer_id, const std::string& data = {}) {
        m_recorder->Write(MakeOpEvent(type, gid, m_id, op_index, decision, blocked,
                                      value, xfer_id, data));
    }

    void WritePark(EventType type, std::uint32_t gid, std::uint32_t channel,
                   ParkReason reason, std::uint32_t op_index,
                   std::uint32_t xfer_id) {
        m_recorder->Write(MakeParkEvent(type, gid, channel, reason, op_index,
                                        xfer_id));
    }

    /// 唤醒一个等待者：写 kWake + 按注入开关决定是否真的 notify。
    void WakeLocked(const WaiterPtr& waiter, ParkReason reason,
                    std::uint32_t xfer_id) {
        waiter->done = true;
        waiter->decision = Decision::kOk;
        waiter->xfer_id = xfer_id;
        if (m_recorder->ShouldDropWake(waiter->gid, m_id)) {
            // 故障注入：只写注入 note（ShouldDropWake 内部已写），不写 kWake、
            // 不 notify。等待者从此永远不被唤醒 —— 记录里缺这半次交接。
            return;
        }
        WritePark(EventType::kWake, waiter->gid, m_id, reason, waiter->op_index,
                  xfer_id);
        waiter->cv.notify_one();
    }

    /// 从等待队列取出一个「仍然有效」的等待者，跳过已被其它 case 选中的
    /// select 等待者与已完成的普通等待者。
    static WaiterPtr PopUsableLocked(std::deque<WaiterPtr>& queue) {
        while (!queue.empty()) {
            WaiterPtr waiter = queue.front();
            queue.pop_front();
            if (waiter->select != nullptr) {
                if (!waiter->select->Decided()) {
                    return waiter;
                }
                continue;
            }
            if (!waiter->done) {
                return waiter;
            }
        }
        return nullptr;
    }

    static void RemoveWaiterLocked(std::deque<WaiterPtr>& queue, Waiter* raw) {
        for (auto it = queue.begin(); it != queue.end(); ++it) {
            if (it->get() == raw) {
                queue.erase(it);
                return;
            }
        }
    }

    /// 完成一个 select 的 recv case：写入接收值并唤醒 select。
    void CompleteSelectRecvLocked(const WaiterPtr& waiter, const T& value,
                                  std::uint32_t xfer_id) {
        SelectState& state = *waiter->select;
        state.int_value = Codec::ToInt(value);
        state.data = Codec::ToBytes(value);
        state.selected_channel = m_id;
        state.xfer_id = xfer_id;
        state.SetDecision(Decision::kOk);
        if (!m_recorder->ShouldDropWake(state.gid, m_id)) {
            WritePark(EventType::kWake, state.gid, m_id, ParkReason::kSelect,
                      state.op_index, xfer_id);
            state.Notify();
        }
    }

    /// 完成一个 select 的 send case（对方是 recv-select）。
    void CompleteSelectSendLocked(const WaiterPtr& waiter, std::uint32_t xfer_id) {
        SelectState& state = *waiter->select;
        state.int_value = Codec::ToInt(waiter->value);
        state.data = Codec::ToBytes(waiter->value);
        state.selected_channel = m_id;
        state.xfer_id = xfer_id;
        state.SetDecision(Decision::kOk);
        if (!m_recorder->ShouldDropWake(state.gid, m_id)) {
            WritePark(EventType::kWake, state.gid, m_id, ParkReason::kSelect,
                      state.op_index, xfer_id);
            state.Notify();
        }
    }

    /// 有缓冲 channel 的「取走一个值后让等待中的 sender 补位」。
    void RefillBufferLocked() {
        if (m_senders.empty()) {
            return;
        }
        WaiterPtr sender = PopUsableLocked(m_senders);
        if (sender == nullptr) {
            return;
        }
        if (sender->select != nullptr) {
            if (sender->select->Claim(sender->select_case)) {
                m_buffer.push_back(sender->value);
                CompleteSelectSendLocked(sender, 0);
            }
            return;
        }
        m_buffer.push_back(sender->value);
        WakeLocked(sender, ParkReason::kSend, 0);
    }

    Decision LiveSend(const T& value) {
        const std::uint32_t gid = detail::ResolveGid();
        const std::uint32_t op_index = m_recorder->NextOpIndex(gid);
        const std::int64_t int_value = Codec::ToInt(value);
        const std::string bytes = Codec::ToBytes(value);
        auto lock = LockOp();

        for (;;) {
            if (m_closed) {
                WriteOp(EventType::kSend, gid, op_index, Decision::kClosed, 0, 0, 0);
                return Decision::kClosed;
            }
            // step1: 有等待的接收者 → 直接会合交接（绕过缓冲，与 Go hchan 一致）。
            WaiterPtr receiver = PopUsableLocked(m_receivers);
            if (receiver != nullptr) {
                if (receiver->select != nullptr) {
                    SelectState& state = *receiver->select;
                    if (!state.Claim(receiver->select_case)) {
                        continue;  // 该 select 已被别的 case 选中
                    }
                    const std::uint32_t xfer = m_recorder->NextXferId();
                    state.int_value = int_value;
                    state.data = bytes;
                    state.selected_channel = m_id;
                    state.xfer_id = xfer;
                    state.SetDecision(Decision::kOk);
                    WriteOp(EventType::kSend, gid, op_index, Decision::kOk, 0,
                            int_value, xfer, bytes);
                    if (!m_recorder->ShouldDropWake(state.gid, m_id)) {
                        WritePark(EventType::kWake, state.gid, m_id,
                                  ParkReason::kSelect, state.op_index, xfer);
                        state.Notify();
                    }
                    return Decision::kOk;
                }
                const std::uint32_t xfer = m_recorder->NextXferId();
                WriteOp(EventType::kSend, gid, op_index, Decision::kOk, 0,
                        int_value, xfer, bytes);
                receiver->value = value;
                WakeLocked(receiver, ParkReason::kRecv, xfer);
                return Decision::kOk;
            }
            // step2: 有缓冲空间 → 入缓冲。
            if (m_buffer.size() < m_capacity) {
                m_buffer.push_back(value);
                WriteOp(EventType::kSend, gid, op_index, Decision::kOk, 0,
                        int_value, 0, bytes);
                return Decision::kOk;
            }
            // step3: 无接收者且无空间 → 登记并 park。
            WaiterPtr waiter = std::make_shared<Waiter>();
            waiter->gid = gid;
            waiter->op_index = op_index;
            waiter->reason = ParkReason::kSend;
            waiter->value = value;
            m_senders.push_back(waiter);
            WritePark(EventType::kPark, gid, m_id, ParkReason::kSend, op_index, 0);
            waiter->cv.wait(lock, [&waiter] { return waiter->done; });
            WriteOp(EventType::kSend, gid, op_index, waiter->decision, 1,
                    int_value, waiter->xfer_id, bytes);
            return waiter->decision;
        }
    }

    RecvResult LiveRecv() {
        const std::uint32_t gid = detail::ResolveGid();
        const std::uint32_t op_index = m_recorder->NextOpIndex(gid);
        auto lock = LockOp();

        for (;;) {
            // step1: 缓冲非空 → 取队首，并让一个等待中的发送者补位。
            if (!m_buffer.empty()) {
                const T value = m_buffer.front();
                m_buffer.pop_front();
                RefillBufferLocked();
                WriteOp(EventType::kRecv, gid, op_index, Decision::kOk, 0,
                        Codec::ToInt(value), 0, Codec::ToBytes(value));
                return RecvResult{Decision::kOk, value};
            }
            // step2: 有等待中的发送者 → 直接取它的值会合。
            WaiterPtr sender = PopUsableLocked(m_senders);
            if (sender != nullptr) {
                if (sender->select != nullptr) {
                    SelectState& state = *sender->select;
                    if (!state.Claim(sender->select_case)) {
                        continue;  // 该 select 已被别的 case 选中
                    }
                    const T value = sender->value;
                    const std::uint32_t xfer = m_recorder->NextXferId();
                    state.int_value = Codec::ToInt(value);
                    state.data = Codec::ToBytes(value);
                    state.selected_channel = m_id;
                    state.xfer_id = xfer;
                    state.SetDecision(Decision::kOk);
                    WriteOp(EventType::kRecv, gid, op_index, Decision::kOk, 0,
                            Codec::ToInt(value), xfer, Codec::ToBytes(value));
                    if (!m_recorder->ShouldDropWake(state.gid, m_id)) {
                        WritePark(EventType::kWake, state.gid, m_id,
                                  ParkReason::kSelect, state.op_index, xfer);
                        state.Notify();
                    }
                    return RecvResult{Decision::kOk, value};
                }
                const std::uint32_t xfer = m_recorder->NextXferId();
                const T value = sender->value;
                WriteOp(EventType::kRecv, gid, op_index, Decision::kOk, 0,
                        Codec::ToInt(value), xfer, Codec::ToBytes(value));
                WakeLocked(sender, ParkReason::kSend, xfer);
                return RecvResult{Decision::kOk, value};
            }
            // step3: 已关闭 → 立即返回关闭。
            if (m_closed) {
                WriteOp(EventType::kRecv, gid, op_index, Decision::kClosed, 0, 0, 0);
                return RecvResult{Decision::kClosed, T{}};
            }
            // step4: 无值可拿 → 登记并 park。
            WaiterPtr waiter = std::make_shared<Waiter>();
            waiter->gid = gid;
            waiter->op_index = op_index;
            waiter->reason = ParkReason::kRecv;
            m_receivers.push_back(waiter);
            WritePark(EventType::kPark, gid, m_id, ParkReason::kRecv, op_index, 0);
            waiter->cv.wait(lock, [&waiter] { return waiter->done; });
            WriteOp(EventType::kRecv, gid, op_index, waiter->decision, 1,
                    waiter->decision == Decision::kOk ? Codec::ToInt(waiter->value) : 0,
                    waiter->xfer_id,
                    waiter->decision == Decision::kOk ? Codec::ToBytes(waiter->value)
                                                      : std::string{});
            return RecvResult{waiter->decision, waiter->value};
        }
    }

    Decision LiveClose() {
        const std::uint32_t gid = detail::ResolveGid();
        const std::uint32_t op_index = m_recorder->NextOpIndex(gid);
        auto lock = LockOp();
        if (m_closed) {
            WriteOp(EventType::kClose, gid, op_index, Decision::kAlreadyClosed, 0, 0, 0);
            return Decision::kAlreadyClosed;
        }
        m_closed = true;
        WriteOp(EventType::kClose, gid, op_index, Decision::kOk, 0, 0, 0);
        // 所有等待者以「已关闭」结束；发送者失败，接收者拿到 closed。
        for (auto& sender : m_senders) {
            if (sender->done) {
                continue;
            }
            sender->done = true;
            sender->decision = Decision::kClosed;
            if (sender->select != nullptr) {
                if (sender->select->Claim(sender->select_case)) {
                    sender->select->SetDecision(Decision::kClosed);
                    if (!m_recorder->ShouldDropWake(sender->select->gid, m_id)) {
                        WritePark(EventType::kWake, sender->select->gid, m_id,
                                  ParkReason::kClose, sender->select->op_index, 0);
                        sender->select->Notify();
                    }
                }
                continue;
            }
            if (!m_recorder->ShouldDropWake(sender->gid, m_id)) {
                WritePark(EventType::kWake, sender->gid, m_id, ParkReason::kClose,
                          sender->op_index, 0);
                sender->cv.notify_one();
            }
        }
        for (auto& receiver : m_receivers) {
            if (receiver->done) {
                continue;
            }
            receiver->done = true;
            receiver->decision = Decision::kClosed;
            if (receiver->select != nullptr) {
                if (receiver->select->Claim(receiver->select_case)) {
                    receiver->select->SetDecision(Decision::kClosed);
                    if (!m_recorder->ShouldDropWake(receiver->select->gid, m_id)) {
                        WritePark(EventType::kWake, receiver->select->gid, m_id,
                                  ParkReason::kClose, receiver->select->op_index, 0);
                        receiver->select->Notify();
                    }
                }
                continue;
            }
            if (!m_recorder->ShouldDropWake(receiver->gid, m_id)) {
                WritePark(EventType::kWake, receiver->gid, m_id, ParkReason::kClose,
                          receiver->op_index, 0);
                receiver->cv.notify_one();
            }
        }
        m_senders.clear();
        m_receivers.clear();
        return Decision::kOk;
    }

    /// replay 模式：按记录返回操作结果；阻塞操作把 CPU 交还引擎。
    Decision ReplayOperation(EventType expected, const Event* event) {
        const std::uint32_t gid = m_source->CurrentFiberGid();
        if (event == nullptr) {
            m_source->RecordResult(gid, Event{}, 0, {});
            return Decision::kStuck;
        }
        if (event->type != expected || event->channel != m_id) {
            // 记录与程序不一致：返回 stuck，引擎最终会把 LastError 报出来。
            return Decision::kStuck;
        }
        if (event->blocked != 0U) {
            m_source->ParkCurrent(gid, *event);
        }
        m_source->RecordResult(gid, *event, event->value, event->data);
        return event->decision;
    }

    const std::uint32_t m_id;
    const Mode m_mode;
    Recorder* const m_recorder{nullptr};
    ReplaySource* const m_source{nullptr};

    mutable std::mutex m_mutex;
    const std::size_t m_capacity{0};  ///< 0=无缓冲；>0 时走 FIFO 缓冲路径
    SelectGroup* const m_group{nullptr};
    std::deque<T> m_buffer;
    std::deque<WaiterPtr> m_senders;
    std::deque<WaiterPtr> m_receivers;
    bool m_closed{false};
};

}  // namespace go2cpp::replay
