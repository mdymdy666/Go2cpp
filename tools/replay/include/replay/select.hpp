#pragma once

/**
 * @file select.hpp
 * @brief Go 风格 `select`：多 channel 竞争、default、timeout、Context 取消。
 *
 * ## 记录什么
 *
 * 一次 select 只记**一条 `kSelect`**（不是每个 case 一条 send/recv）：
 *   - `chosen`：选中的 case 下标（**决策**本身）；
 *   - `channel`：选中 case 的 channel id；default/timeout/ctx 为 `kNoChannel`；
 *   - `decision`：`ok/closed/timed-out/cancelled`；
 *   - `value`/`data`：选中 recv 时收到的值，或选中 send 时发出的值；
 *   - `cases`：每个 case 的 `(channel, kind)`，用于重放校验与诊断；
 *   - `blocked`：0=所有 case 立即就绪，1=经过 `kPark` 等待。
 *
 * 若 select 的交接唤醒了某个 parked sender/receiver，channel 侧照常写一条
 * `kWake`；若 select 自己 park 后被唤醒，唤醒方（channel / timeout / ctx）
 * 写一条 `kWake(gid, kNoChannel, reason=kSelect/kTimeout/kContext)`。
 *
 * ## 重放
 *
 * replay 模式**不做任何轮询**：`Wait()` 直接取 `(gid, op_index)` 对应的
 * `kSelect`，按记录的 `chosen`/`decision`/值返回。因此重放不会因为真实
 * 调度不同而选中另一个 case。
 *
 * ## 一次性
 *
 * 一个 `SyncSelect` 对象只应 `Wait()` 一次（与 Go 的 select 语句对应）。
 * 需要再次 select 时新建对象；`Add*` 在 `Wait()` 之后再调用会报错。
 */

#include "replay/clock.hpp"
#include "replay/context.hpp"
#include "replay/select_state.hpp"
#include "replay/sync_channel.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace go2cpp::replay {

class SyncSelect {
public:
    /// live 模式：channel 必须绑定同一个 `group`，`recorder` 记录决策。
    SyncSelect(Recorder& recorder, ReplayClock& clock, SelectGroup& group)
        : m_recorder(&recorder), m_clock(&clock), m_group(&group) {}

    /// replay 模式：决策全部来自记录。
    explicit SyncSelect(ReplaySource& source) : m_source(&source) {}

    SyncSelect(const SyncSelect&) = delete;
    SyncSelect& operator=(const SyncSelect&) = delete;

    /// 添加 `case v = <-ch:`；`out` 可为 nullptr（只取值不保存）。
    template <typename Channel>
    int AddRecv(Channel& channel, typename Channel::value_type* out) {
        if (!CheckChannel(channel)) {
            return -1;
        }
        if (m_waited) {
            m_error = "SyncSelect: AddRecv after Wait";
            return -1;
        }
        const std::uint32_t index = NextCaseIndex();
        Case item;
        item.kind = SelectCaseKind::kRecv;
        item.channel = channel.id();
        item.try_case = [this, &channel, index]() {
            return channel.SelectTryRecvLocked(m_state, index);
        };
        item.arm = [this, &channel, index]() {
            return channel.SelectArmRecvLocked(m_state, index);
        };
        item.disarm = [&channel](const SelectWaiterToken& token) {
            channel.SelectDisarmLocked(token);
        };
        item.set_out = [out](std::int64_t value, const std::string& data) {
            if (out != nullptr) {
                *out = Channel::codec_type::FromValue(value, data);
            }
        };
        m_cases.push_back(std::move(item));
        m_state.cases.push_back(SelectCaseRecord{channel.id(), SelectCaseKind::kRecv});
        return static_cast<int>(index);
    }

    /// 添加 `case ch <- v:`。
    template <typename Channel>
    int AddSend(Channel& channel, const typename Channel::value_type& value) {
        if (!CheckChannel(channel)) {
            return -1;
        }
        if (m_waited) {
            m_error = "SyncSelect: AddSend after Wait";
            return -1;
        }
        const std::uint32_t index = NextCaseIndex();
        Case item;
        item.kind = SelectCaseKind::kSend;
        item.channel = channel.id();
        item.try_case = [this, &channel, index, value]() {
            return channel.SelectTrySendLocked(m_state, index, value);
        };
        item.arm = [this, &channel, index, value]() {
            return channel.SelectArmSendLocked(m_state, index, value);
        };
        item.disarm = [&channel](const SelectWaiterToken& token) {
            channel.SelectDisarmLocked(token);
        };
        m_cases.push_back(std::move(item));
        m_state.cases.push_back(SelectCaseRecord{channel.id(), SelectCaseKind::kSend});
        return static_cast<int>(index);
    }

    /// 添加 `default:`；最多一个，且通常放在最后。
    int AddDefault() {
        if (m_default_index >= 0) {
            m_error = "SyncSelect: duplicate default case";
            return -1;
        }
        if (m_waited) {
            m_error = "SyncSelect: AddDefault after Wait";
            return -1;
        }
        const std::uint32_t index = NextCaseIndex();
        Case item;
        item.kind = SelectCaseKind::kDefault;
        m_cases.push_back(std::move(item));
        m_state.cases.push_back(SelectCaseRecord{kNoChannel, SelectCaseKind::kDefault});
        m_default_index = static_cast<int>(index);
        return m_default_index;
    }

    /// 添加 `<-time.After(timeout_ns):`；虚拟时间，不依赖真实定时器。
    int AddTimeout(std::uint64_t timeout_ns) {
        if (m_timeout_index >= 0) {
            m_error = "SyncSelect: duplicate timeout case";
            return -1;
        }
        if (m_waited) {
            m_error = "SyncSelect: AddTimeout after Wait";
            return -1;
        }
        m_timeout_ns = timeout_ns;
        const std::uint32_t index = NextCaseIndex();
        Case item;
        item.kind = SelectCaseKind::kTimeout;
        m_cases.push_back(std::move(item));
        m_state.cases.push_back(SelectCaseRecord{kNoChannel, SelectCaseKind::kTimeout});
        m_timeout_index = static_cast<int>(index);
        return m_timeout_index;
    }

    /// 添加 `<-ctx.Done():`。
    int AddContext(ReplayContext& ctx) {
        if (m_waited) {
            m_error = "SyncSelect: AddContext after Wait";
            return -1;
        }
        const std::uint32_t index = NextCaseIndex();
        Case item;
        item.kind = SelectCaseKind::kContext;
        item.channel = kNoChannel;
        item.ctx = &ctx;
        item.try_case = [this, &ctx, index]() {
            const Decision decision = ctx.Peek();
            if (decision == Decision::kOk) {
                return false;
            }
            if (!m_state.Claim(index)) {
                return false;
            }
            if (decision == Decision::kTimedOut) {
                ctx.NoteDeadlineExceeded();
            }
            m_state.SetDecision(decision);
            m_state.selected_channel = kNoChannel;
            return true;
        };
        item.arm = [this, &ctx, index]() {
            ctx.Arm(m_state, index);
            return SelectWaiterToken{};
        };
        item.disarm = [this, &ctx](const SelectWaiterToken&) {
            ctx.Disarm(m_state);
        };
        m_cases.push_back(std::move(item));
        m_state.cases.push_back(SelectCaseRecord{kNoChannel, SelectCaseKind::kContext});
        return static_cast<int>(index);
    }

    /**
     * @brief 执行 select，返回选中的 case 下标。
     *
     * 失败（参数错误 / 记录不匹配）返回 -1，原因见 `error()`。
     */
    int Wait() {
        if (m_waited) {
            m_error = "SyncSelect: Wait called twice";
            return -1;
        }
        m_waited = true;
        for (const auto& item : m_cases) {
            if (!item.try_case && (item.kind == SelectCaseKind::kRecv ||
                                   item.kind == SelectCaseKind::kSend ||
                                   item.kind == SelectCaseKind::kContext)) {
                m_error = "SyncSelect: case has no poll function";
                return -1;
            }
        }
        return m_source != nullptr ? WaitReplay() : WaitLive();
    }

    int chosen() const noexcept { return m_chosen; }
    Decision decision() const noexcept { return m_decision; }
    const std::string& error() const noexcept { return m_error; }

    /// 选中的 recv case 收到的值（bytes 类型）；i64 时为空。
    const std::string& received_data() const noexcept { return m_state.data; }

private:
    struct Case {
        SelectCaseKind kind{SelectCaseKind::kRecv};
        std::uint32_t channel{kNoChannel};
        std::function<bool()> try_case;                       ///< 持组锁 poll
        std::function<SelectWaiterToken()> arm;               ///< 持组锁登记
        std::function<void(const SelectWaiterToken&)> disarm; ///< 持组锁撤销
        std::function<void(std::int64_t, const std::string&)> set_out;
        ReplayContext* ctx{nullptr};
    };

    std::uint32_t NextCaseIndex() { return static_cast<std::uint32_t>(m_cases.size()); }

    template <typename Channel>
    bool CheckChannel(Channel& channel) {
        if (m_source != nullptr) {
            return true;  // replay：只按记录返回，不要求组锁
        }
        if (m_group == nullptr || channel.group() != m_group) {
            m_error = "SyncSelect: channel " + std::to_string(channel.id()) +
                      " is not bound to this SelectGroup";
            return false;
        }
        return true;
    }

    void WriteSelect(std::uint32_t gid, std::uint32_t op_index, int chosen,
                     std::uint8_t blocked) {
        m_recorder->Write(MakeSelectEvent(
            gid, m_state.selected_channel, op_index, m_state.GetDecision(), blocked,
            static_cast<std::uint16_t>(chosen), m_state.int_value, m_state.data,
            m_state.cases, m_state.xfer_id));
    }

    /// 把选中 case 的值写给调用方（record 的等待路径也必须写）。
    void DeliverOut(int chosen) {
        if (chosen < 0 || static_cast<std::size_t>(chosen) >= m_cases.size()) {
            return;
        }
        auto& item = m_cases[static_cast<std::size_t>(chosen)];
        if (item.set_out) {
            item.set_out(m_state.int_value, m_state.data);
        }
    }

    void WriteWake(std::uint32_t gid, ParkReason reason, std::uint32_t op_index) {
        m_recorder->Write(MakeParkEvent(EventType::kWake, gid, kNoChannel, reason,
                                        op_index, 0));
    }

    int WaitLive() {
        if (m_group == nullptr || m_recorder == nullptr || m_clock == nullptr) {
            m_error = "SyncSelect: live mode requires group/recorder/clock";
            return -1;
        }
        std::unique_lock<std::mutex> lock(m_group->mutex());
        const std::uint32_t gid = detail::ResolveGid();
        const std::uint32_t op_index = m_recorder->NextOpIndex(gid);
        m_state.gid = gid;
        m_state.op_index = op_index;

        // 1) 轮询所有可立即完成的 case（按声明顺序；Go 语义允许任意顺序）。
        int chosen = -1;
        for (std::size_t index = 0; index < m_cases.size(); ++index) {
            if (m_cases[index].try_case && m_cases[index].try_case()) {
                chosen = static_cast<int>(index);
                break;
            }
        }
        // 2) 有 default 且都未就绪 → 选 default。
        if (chosen < 0 && m_default_index >= 0) {
            if (m_state.Claim(static_cast<std::uint32_t>(m_default_index))) {
                m_state.SetDecision(Decision::kOk);
                m_state.selected_channel = kNoChannel;
                chosen = m_default_index;
            }
        }
        if (chosen >= 0) {
            DeliverOut(chosen);
            WriteSelect(gid, op_index, chosen, 0);
            m_chosen = chosen;
            m_decision = m_state.GetDecision();
            return chosen;
        }

        // 3) 需要等待：先写 kPark（重放的驱动点），再登记。
        m_recorder->Write(MakeParkEvent(EventType::kPark, gid, kNoChannel,
                                        ParkReason::kSelect, op_index, 0));
        std::vector<SelectWaiterToken> tokens;
        tokens.reserve(m_cases.size());
        for (auto& item : m_cases) {
            tokens.push_back(item.arm ? item.arm() : SelectWaiterToken{});
        }

        // 4) 计算等待截止时间（timeout 与 ctx deadline 取最早）。
        bool has_deadline = false;
        std::uint64_t deadline_ns = 0;
        const std::uint64_t start_ns = m_clock->NowNs();
        if (m_timeout_ns > 0) {
            has_deadline = true;
            deadline_ns = start_ns + m_timeout_ns;
        }
        for (const auto& item : m_cases) {
            if (item.kind == SelectCaseKind::kContext && item.ctx != nullptr &&
                item.ctx->HasDeadline()) {
                const std::uint64_t ctx_deadline = item.ctx->DeadlineNs();
                if (!has_deadline || ctx_deadline < deadline_ns) {
                    has_deadline = true;
                    deadline_ns = ctx_deadline;
                }
            }
        }

        // 5) 等待直到被 claim 或截止时间到。
        for (;;) {
            if (m_state.Decided()) {
                break;
            }
            if (has_deadline) {
                const std::uint64_t now = m_clock->NowNs();
                if (now >= deadline_ns) {
                    break;
                }
                const auto wait_ns = std::chrono::nanoseconds(deadline_ns - now);
                m_state.cv.wait_until(
                    lock,
                    core::ParkingCondition::Clock::now() + wait_ns,
                    [this]() { return m_state.Decided(); });
            } else {
                m_state.cv.wait(lock, [this]() { return m_state.Decided(); });
            }
        }

        // 6) 撤销所有未命中的登记。
        for (std::size_t index = 0; index < m_cases.size(); ++index) {
            if (m_cases[index].disarm) {
                m_cases[index].disarm(tokens[index]);
            }
        }

        // 7) 未被任何 channel claim → 判定 timeout / ctx deadline。
        chosen = m_state.Selected();
        if (chosen < 0) {
            const std::uint64_t after_ns = m_clock->NowNs();
            for (std::size_t index = 0; index < m_cases.size(); ++index) {
                const auto& item = m_cases[index];
                if (item.kind != SelectCaseKind::kContext || item.ctx == nullptr ||
                    !item.ctx->HasDeadline() || after_ns < item.ctx->DeadlineNs()) {
                    continue;
                }
                if (m_state.Claim(static_cast<std::uint32_t>(index))) {
                    item.ctx->NoteDeadlineExceeded();
                    m_state.SetDecision(Decision::kTimedOut);
                    m_state.selected_channel = kNoChannel;
                    m_recorder->Write(MakeTimerEvent(
                        gid, TimerKind::kTimeoutSelected, 0, op_index,
                        "ctx-deadline"));
                    WriteWake(gid, ParkReason::kTimeout, op_index);
                    chosen = static_cast<int>(index);
                    break;
                }
            }
        }
        if (chosen < 0 && m_timeout_index >= 0) {
            if (m_state.Claim(static_cast<std::uint32_t>(m_timeout_index))) {
                m_state.SetDecision(Decision::kTimedOut);
                m_state.selected_channel = kNoChannel;
                m_recorder->Write(MakeTimerEvent(gid, TimerKind::kTimeoutSelected,
                                                 m_timeout_ns, op_index, "timeout"));
                WriteWake(gid, ParkReason::kTimeout, op_index);
                chosen = m_timeout_index;
            }
        }
        if (chosen < 0) {
            m_error = "SyncSelect: woken without a decision";
            return -1;
        }

        // 8) 写最终决策（blocked=1，因为确实经过了 kPark）。
        DeliverOut(chosen);
        WriteSelect(gid, op_index, chosen, 1);
        m_chosen = chosen;
        m_decision = m_state.GetDecision();
        return chosen;
    }

    int WaitReplay() {
        const std::uint32_t gid = m_source->CurrentFiberGid();
        const Event* event = m_source->NextOperation(gid);
        if (event == nullptr) {
            m_error = "SyncSelect: no recorded select operation";
            return -1;
        }
        if (event->type != EventType::kSelect) {
            m_error = std::string("SyncSelect: expected select record, got ") +
                      EventTypeName(event->type);
            return -1;
        }
        if (event->blocked != 0U) {
            m_source->ParkCurrent(gid, *event);
        }
        m_source->RecordResult(gid, *event, event->value, event->data);
        m_state.int_value = event->value;
        m_state.data = event->data;
        m_state.selected_channel = event->channel;
        m_state.xfer_id = event->xfer_id;
        const int chosen = static_cast<int>(event->chosen);
        if (chosen >= 0 && static_cast<std::size_t>(chosen) < m_cases.size() &&
            m_cases[static_cast<std::size_t>(chosen)].set_out) {
            m_cases[static_cast<std::size_t>(chosen)].set_out(event->value,
                                                              event->data);
        }
        m_chosen = chosen;
        m_decision = event->decision;
        return chosen;
    }

    Recorder* const m_recorder{nullptr};
    ReplayClock* const m_clock{nullptr};
    ReplaySource* const m_source{nullptr};
    SelectGroup* const m_group{nullptr};

    SelectState m_state;
    std::vector<Case> m_cases;
    int m_default_index{-1};
    int m_timeout_index{-1};
    std::uint64_t m_timeout_ns{0};
    int m_chosen{-1};
    Decision m_decision{Decision::kOk};
    bool m_waited{false};
    std::string m_error;
};

}  // namespace go2cpp::replay
