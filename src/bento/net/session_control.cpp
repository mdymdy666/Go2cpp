/**
 * @file session_control.cpp
 * @brief 通用会话控制权仲裁实现：唯一控制器、可替换策略、租约回收、变更回调与审计。
 * @author go2cpp contributors
 * @date 2026-10-27
 * @copyright MIT License
 */

#include "go2cpp/bento/net/session_control.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <sstream>
#include <utility>

#include "go2cpp/bento/net/protocol.hpp"

namespace go2cpp::bento {

const char* ControlModeName(ControlMode mode) noexcept {
    switch (mode) {
        case ControlMode::kNone:
            return "none";
        case ControlMode::kView:
            return "view";
        case ControlMode::kInteract:
            return "interact";
    }
    return "unknown";
}

const char* ControlChangeReasonName(ControlChangeReason reason) noexcept {
    switch (reason) {
        case ControlChangeReason::kJoined:
            return "joined";
        case ControlChangeReason::kLeft:
            return "left";
        case ControlChangeReason::kGranted:
            return "granted";
        case ControlChangeReason::kRequested:
            return "requested";
        case ControlChangeReason::kPreempted:
            return "preempted";
        case ControlChangeReason::kRevoked:
            return "revoked";
        case ControlChangeReason::kDisconnected:
            return "disconnected";
        case ControlChangeReason::kLeaseExpired:
            return "lease_expired";
    }
    return "unknown";
}

std::shared_ptr<const IControlPolicy> MakeControlPolicy(ControlPolicy policy) {
    switch (policy) {
        case ControlPolicy::kPreempt:
            return std::make_shared<PreemptControlPolicy>();
        case ControlPolicy::kFirstCome:
            break;
    }
    return std::make_shared<FirstComeControlPolicy>();
}

std::string ControlSnapshot::ToJson() const {
    std::ostringstream out;
    out << '{';
    out << "\"controller\":" << controller;
    out << ",\"generation\":" << generation;
    out << ",\"lease_remaining_ms\":" << lease_remaining_ms;
    out << ",\"participants\":[";
    for (std::size_t i = 0; i < participants.size(); ++i) {
        if (i != 0) {
            out << ',';
        }
        const ControlParticipant& participant = participants[i];
        out << "{\"peer_id\":" << participant.peer_id << ",\"mode\":\""
            << ControlModeName(participant.mode) << "\",\"controller\":"
            << (participant.controller ? "true" : "false") << ",\"revoked\":"
            << (participant.revoked ? "true" : "false") << '}';
    }
    out << "]}";
    return out.str();
}

SessionControl::SessionControl(SessionControlConfig config)
    : SessionControl(std::move(config), nullptr) {}

SessionControl::SessionControl(SessionControlConfig config,
                               std::shared_ptr<const IControlPolicy> policy)
    : m_config(std::move(config)), m_policy(std::move(policy)) {
    // 0 个参与者会让 Join 永远失败，钳到 1；默认模式为 kNone 时加入即无权限，
    // 对调用方没有意义，统一按 kView 处理。
    if (m_config.max_participants == 0) {
        m_config.max_participants = 1;
    }
    if (m_config.default_mode == ControlMode::kNone) {
        m_config.default_mode = ControlMode::kView;
    }
    // 未显式注入策略时，按 config.policy 选内置实现；显式注入优先，
    // 使应用无需改库源码即可替换冲突规则。
    if (!m_policy) {
        m_policy = MakeControlPolicy(m_config.policy);
    }
}

ControlChange SessionControl::AppendChange(std::uint32_t peer_id, ControlMode from,
                                           ControlMode to, ControlChangeReason reason,
                                           std::uint32_t actor, std::uint64_t timestamp_ms) {
    ++m_generation;
    ++m_stats.changes;
    ControlChange change;
    change.peer_id = peer_id;
    change.from = from;
    change.to = to;
    change.reason = reason;
    change.controller = m_controller;
    change.generation = m_generation;
    // 审计字段：seq 与 generation 同步；actor 标识发起者；时间戳由调用方
    // 每次操作只取一次（一次抢占的 2 条事件共享同一毫秒，便于关联）。
    change.seq = m_generation;
    change.actor = actor;
    change.timestamp_ms = timestamp_ms;
    change.session_id = m_config.session_id;
    if (m_config.audit_capacity != 0) {
        if (m_audit.size() >= m_config.audit_capacity) {
            m_audit.pop_front();
        }
        m_audit.push_back(change);
    }
    return change;
}

void SessionControl::Dispatch(const ControlChange* changes, std::size_t count) {
    if (!on_change || count == 0) {
        return;
    }
    // 锁外调用：回调里可以再次查询/操作本类。
    for (std::size_t i = 0; i < count; ++i) {
        on_change(changes[i]);
    }
}

bool SessionControl::Join(std::uint32_t peer_id) {
    if (peer_id == 0) {
        return false;
    }
    std::array<ControlChange, 2> changes;
    std::size_t count = 0;
    bool joined = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_participants.find(peer_id) != m_participants.end()) {
            return false;
        }
        if (m_participants.size() >= m_config.max_participants) {
            return false;
        }
        const std::uint64_t now_ms = NowMs();
        ControlMode assigned = m_config.default_mode;
        // 撤销按 peer 身份持久化：曾 Revoke 的 peer 重连也不得直接拿控制权。
        const bool is_revoked = m_revoked_peers.count(peer_id) != 0;
        if (assigned == ControlMode::kInteract && (is_revoked || m_controller != 0)) {
            assigned = ControlMode::kView;
        }
        Entry entry;
        entry.mode = assigned;
        m_participants.emplace(peer_id, entry);
        if (assigned == ControlMode::kInteract) {
            m_controller = peer_id;
            m_lease_deadline_ms =
                m_config.lease_ms == 0 ? 0 : now_ms + m_config.lease_ms;
        }
        changes[count++] = AppendChange(peer_id, ControlMode::kNone, assigned,
                                        ControlChangeReason::kJoined, peer_id, now_ms);
        joined = true;
    }
    Dispatch(changes.data(), count);
    return joined;
}

bool SessionControl::RemoveParticipant(std::uint32_t peer_id, ControlChangeReason reason,
                                       std::uint32_t actor) {
    std::array<ControlChange, 2> changes;
    std::size_t count = 0;
    bool removed = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto found = m_participants.find(peer_id);
        if (found == m_participants.end()) {
            return false;
        }
        const std::uint64_t now_ms = NowMs();
        const ControlMode from = found->second.mode;
        if (m_controller == peer_id) {
            m_controller = 0;
            m_lease_deadline_ms = 0;
        }
        m_participants.erase(found);
        if (reason == ControlChangeReason::kDisconnected) {
            ++m_stats.disconnects;
        }
        changes[count++] =
            AppendChange(peer_id, from, ControlMode::kNone, reason, actor, now_ms);
        removed = true;
    }
    Dispatch(changes.data(), count);
    return removed;
}

bool SessionControl::Leave(std::uint32_t peer_id) {
    return RemoveParticipant(peer_id, ControlChangeReason::kLeft, peer_id);
}

bool SessionControl::OnPeerDisconnected(std::uint32_t peer_id) {
    // 断开是被动事件，actor 记 0（系统检测）。
    return RemoveParticipant(peer_id, ControlChangeReason::kDisconnected, 0);
}

bool SessionControl::Grant(std::uint32_t peer_id) {
    return Grant(peer_id, ControlMode::kView, 0);
}

bool SessionControl::Grant(std::uint32_t peer_id, ControlMode mode) {
    return Grant(peer_id, mode, 0);
}

bool SessionControl::Grant(std::uint32_t peer_id, ControlMode mode, std::uint32_t actor) {
    if (peer_id == 0) {
        return false;
    }
    std::array<ControlChange, 2> changes;
    std::size_t count = 0;
    bool granted = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto found = m_participants.find(peer_id);
        if (found == m_participants.end()) {
            if (m_participants.size() >= m_config.max_participants) {
                return false;
            }
            found = m_participants.emplace(peer_id, Entry{}).first;
        }
        Entry& entry = found->second;
        const ControlMode from = entry.mode;
        // 时钟惰性获取：Grant(view) 无状态变化时不付 clock_gettime。
        std::uint64_t now_ms = 0;
        const auto ensure_now = [&now_ms]() {
            if (now_ms == 0) {
                now_ms = NowMs();
            }
            return now_ms;
        };
        // 显式授权即解除 Revoke 的禁入标记（撤销状态按 peer 身份持久化）。
        m_revoked_peers.erase(peer_id);
        if (mode == ControlMode::kInteract) {
            std::uint32_t previous_id = 0;
            ControlMode previous_mode = ControlMode::kView;
            if (m_controller != 0 && m_controller != peer_id) {
                previous_id = m_controller;
                const auto previous = m_participants.find(previous_id);
                if (previous != m_participants.end()) {
                    previous_mode = previous->second.mode;
                    previous->second.mode = ControlMode::kView;
                }
                ++m_stats.preemptions;
            }
            entry.mode = ControlMode::kInteract;
            m_controller = peer_id;
            m_lease_deadline_ms =
                m_config.lease_ms == 0 ? 0 : ensure_now() + m_config.lease_ms;
            // 状态迁移完成后才生成事件：两条事件的 controller 都是「变化后」的
            // 新控制器；被抢者的事件先于请求者的事件（generation 严格递增）。
            if (previous_id != 0 && previous_mode != ControlMode::kView) {
                changes[count++] = AppendChange(previous_id, previous_mode, ControlMode::kView,
                                                ControlChangeReason::kPreempted, actor,
                                                ensure_now());
            }
            if (from != ControlMode::kInteract) {
                changes[count++] =
                    AppendChange(peer_id, from, ControlMode::kInteract,
                                 ControlChangeReason::kGranted, actor, ensure_now());
            }
        } else {
            if (m_controller == peer_id) {
                m_controller = 0;
                m_lease_deadline_ms = 0;
            }
            entry.mode = mode;
            if (from != mode) {
                changes[count++] = AppendChange(peer_id, from, mode,
                                                ControlChangeReason::kGranted, actor,
                                                ensure_now());
            }
        }
        ++m_stats.grants;
        granted = true;
    }
    Dispatch(changes.data(), count);
    return granted;
}

bool SessionControl::Revoke() {
    std::array<ControlChange, 2> changes;
    std::size_t count = 0;
    bool revoked = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_controller == 0) {
            return false;
        }
        const std::uint32_t target = m_controller;
        const auto found = m_participants.find(target);
        if (found != m_participants.end()) {
            const std::uint64_t now_ms = NowMs();
            const ControlMode from = found->second.mode;
            found->second.mode = ControlMode::kView;
            // 管理员强制收回：按 peer 身份禁止再次请求（跨 Leave/重连保持），
            // 直到显式 Grant 解除。
            m_revoked_peers.insert(target);
            m_controller = 0;
            m_lease_deadline_ms = 0;
            ++m_stats.revocations;
            changes[count++] = AppendChange(target, from, ControlMode::kView,
                                            ControlChangeReason::kRevoked, 0, now_ms);
            revoked = true;
        }
    }
    Dispatch(changes.data(), count);
    return revoked;
}

bool SessionControl::Revoke(std::uint32_t peer_id) {
    return Revoke(peer_id, 0);
}

bool SessionControl::Revoke(std::uint32_t peer_id, std::uint32_t actor) {
    if (peer_id == 0) {
        return false;
    }
    std::array<ControlChange, 2> changes;
    std::size_t count = 0;
    bool revoked = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto found = m_participants.find(peer_id);
        if (found == m_participants.end()) {
            return false;
        }
        // 撤销状态按 peer 身份持久化：Leave/断开后重连 Join 也不会清除。
        m_revoked_peers.insert(peer_id);
        if (m_controller == peer_id) {
            const ControlMode from = found->second.mode;
            found->second.mode = ControlMode::kView;
            m_controller = 0;
            m_lease_deadline_ms = 0;
            ++m_stats.revocations;
            changes[count++] = AppendChange(peer_id, from, ControlMode::kView,
                                            ControlChangeReason::kRevoked, actor, NowMs());
        }
        // 幂等：已经处于撤销态时不再重复回调（非控制器路径不产事件）。
        revoked = true;
    }
    Dispatch(changes.data(), count);
    return revoked;
}

bool SessionControl::RequestControl(std::uint32_t peer_id) {
    if (peer_id == 0) {
        return false;
    }
    std::array<ControlChange, 2> changes;
    std::size_t count = 0;
    bool granted = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        ++m_stats.requests;
        const auto found = m_participants.find(peer_id);
        if (found == m_participants.end()) {
            // 未加入的 peer 不能直接请求：必须先 Join，避免未知 id 进入状态表。
            ++m_stats.denials;
            return false;
        }
        Entry& entry = found->second;
        // kNone 表示「无任何权限」：Grant(peer, kNone) 收回权限后，请求必须
        // 被拒绝（策略不得绕过权限），需显式 Grant 恢复 kView/kInteract。
        if (entry.mode == ControlMode::kNone) {
            ++m_stats.denials;
            return false;
        }
        if (m_revoked_peers.count(peer_id) != 0) {
            ++m_stats.denials;
            return false;
        }
        // 时钟惰性获取：只有成功/续租路径才会写 deadline 或产事件。
        std::uint64_t now_ms = 0;
        const auto ensure_now = [&now_ms]() {
            if (now_ms == 0) {
                now_ms = NowMs();
            }
            return now_ms;
        };
        if (m_controller == peer_id) {
            // 幂等请求 = 续租。
            entry.mode = ControlMode::kInteract;
            m_lease_deadline_ms =
                m_config.lease_ms == 0 ? 0 : ensure_now() + m_config.lease_ms;
            granted = true;
        } else if (m_controller == 0) {
            const ControlMode from = entry.mode;
            entry.mode = ControlMode::kInteract;
            m_controller = peer_id;
            m_lease_deadline_ms =
                m_config.lease_ms == 0 ? 0 : ensure_now() + m_config.lease_ms;
            changes[count++] = AppendChange(peer_id, from, ControlMode::kInteract,
                                            ControlChangeReason::kRequested, peer_id,
                                            ensure_now());
            granted = true;
        } else {
            // 冲突路径交给注入的策略；状态迁移（降级原控制器、审计、统计）
            // 仍由仲裁器统一执行，策略只回答「批准/拒绝」。
            ControlRequestContext context;
            context.peer_id = peer_id;
            context.peer_mode = entry.mode;
            context.controller = m_controller;
            const auto current = m_participants.find(m_controller);
            if (current != m_participants.end()) {
                context.controller_mode = current->second.mode;
            }
            context.generation = m_generation;
            context.requests = m_stats.requests;
            context.participants = m_participants.size();
            const bool approved =
                m_policy != nullptr && m_policy->Decide(context) == ControlDecision::kGrant;
            if (approved) {
                std::uint32_t previous_id = 0;
                ControlMode previous_mode = ControlMode::kView;
                const auto previous = m_participants.find(m_controller);
                if (previous != m_participants.end()) {
                    previous_id = m_controller;
                    previous_mode = previous->second.mode;
                    previous->second.mode = ControlMode::kView;
                }
                ++m_stats.preemptions;
                const ControlMode from = entry.mode;
                entry.mode = ControlMode::kInteract;
                m_controller = peer_id;
                m_lease_deadline_ms =
                    m_config.lease_ms == 0 ? 0 : ensure_now() + m_config.lease_ms;
                // 先降级事件、后请求事件；两者都携带变化后的新控制器。
                if (previous_id != 0 && previous_mode != ControlMode::kView) {
                    changes[count++] =
                        AppendChange(previous_id, previous_mode, ControlMode::kView,
                                     ControlChangeReason::kPreempted, peer_id, ensure_now());
                }
                changes[count++] =
                    AppendChange(peer_id, from, ControlMode::kInteract,
                                 ControlChangeReason::kRequested, peer_id, ensure_now());
                granted = true;
            } else {
                ++m_stats.denials;
            }
        }
    }
    Dispatch(changes.data(), count);
    return granted;
}

bool SessionControl::Renew(std::uint32_t peer_id) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_controller == 0 || m_controller != peer_id || m_config.lease_ms == 0) {
        return false;
    }
    m_lease_deadline_ms = NowMs() + m_config.lease_ms;
    return true;
}

ControlMode SessionControl::mode(std::uint32_t peer_id) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto found = m_participants.find(peer_id);
    return found == m_participants.end() ? ControlMode::kNone : found->second.mode;
}

bool SessionControl::is_controller(std::uint32_t peer_id) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return peer_id != 0 && m_controller == peer_id;
}

bool SessionControl::revoked(std::uint32_t peer_id) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    // 身份级撤销：参与者已 Leave/断开时仍返回 true（重连不得绕过）。
    return m_revoked_peers.count(peer_id) != 0;
}

std::uint32_t SessionControl::controller() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_controller;
}

bool SessionControl::has_controller() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_controller != 0;
}

std::vector<std::uint32_t> SessionControl::participants() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<std::uint32_t> ids;
    ids.reserve(m_participants.size());
    for (const auto& entry : m_participants) {
        ids.push_back(entry.first);
    }
    return ids;
}

ControlSnapshot SessionControl::Snapshot() const {
    // 锁外取时钟（m_config 构造后只读），临界区里只剩纯内存拷贝。
    const std::uint64_t now_ms = m_config.lease_ms == 0 ? 0 : NowMs();
    std::lock_guard<std::mutex> lock(m_mutex);
    ControlSnapshot snapshot;
    snapshot.controller = m_controller;
    snapshot.generation = m_generation;
    if (m_controller != 0 && m_config.lease_ms != 0 && m_lease_deadline_ms != 0) {
        if (m_lease_deadline_ms > now_ms) {
            const std::uint64_t remaining = m_lease_deadline_ms - now_ms;
            snapshot.lease_remaining_ms = static_cast<std::uint32_t>(
                std::min<std::uint64_t>(remaining, std::numeric_limits<std::uint32_t>::max()));
        }
    }
    snapshot.participants.reserve(m_participants.size());
    for (const auto& entry : m_participants) {
        ControlParticipant participant;
        participant.peer_id = entry.first;
        participant.mode = entry.second.mode;
        participant.controller = entry.first == m_controller;
        participant.revoked = m_revoked_peers.count(entry.first) != 0;
        snapshot.participants.push_back(participant);
    }
    snapshot.audit.assign(m_audit.begin(), m_audit.end());
    return snapshot;
}

std::uint64_t SessionControl::generation() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_generation;
}

SessionControlStats SessionControl::stats() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_stats;
}

const char* SessionControl::policy_name() const noexcept {
    // m_policy 只在构造期赋值，之后只读，无需加锁。
    return m_policy != nullptr ? m_policy->name() : "none";
}

bool SessionControl::Tick(std::uint64_t now_ms) {
    std::array<ControlChange, 2> changes;
    std::size_t count = 0;
    bool reclaimed = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_controller == 0 || m_config.lease_ms == 0 || m_lease_deadline_ms == 0 ||
            now_ms < m_lease_deadline_ms) {
            return false;
        }
        const std::uint32_t expired = m_controller;
        const auto found = m_participants.find(expired);
        // 先清空控制器再生成事件：`ControlChange::controller` 的契约是
        // 「变化后的唯一控制器」，过期后必须为 0（广播端据此收敛状态）。
        m_controller = 0;
        m_lease_deadline_ms = 0;
        if (found != m_participants.end()) {
            const ControlMode from = found->second.mode;
            found->second.mode = ControlMode::kView;
            ++m_stats.lease_expirations;
            changes[count++] =
                AppendChange(expired, from, ControlMode::kView,
                             ControlChangeReason::kLeaseExpired, 0, now_ms);
            reclaimed = true;
        }
    }
    Dispatch(changes.data(), count);
    return reclaimed;
}

bool SessionControl::Tick() {
    return Tick(NowMs());
}

}  // namespace go2cpp::bento
