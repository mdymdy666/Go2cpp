/**
 * @file session_control.cpp
 * @brief 通用会话控制权仲裁实现：唯一控制器、租约回收、变更回调。
 * @author go2cpp contributors
 * @date 2026-10-27
 * @copyright MIT License
 */

#include "go2cpp/bento/net/session_control.hpp"

#include <algorithm>
#include <limits>
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

SessionControl::SessionControl(SessionControlConfig config) : m_config(config) {
    // 0 个参与者会让 Join 永远失败，钳到 1；默认模式为 kNone 时加入即无权限，
    // 对调用方没有意义，统一按 kView 处理。
    if (m_config.max_participants == 0) {
        m_config.max_participants = 1;
    }
    if (m_config.default_mode == ControlMode::kNone) {
        m_config.default_mode = ControlMode::kView;
    }
}

void SessionControl::AppendChange(std::vector<ControlChange>* out, std::uint32_t peer_id,
                                  ControlMode from, ControlMode to,
                                  ControlChangeReason reason) {
    ++m_generation;
    ++m_stats.changes;
    ControlChange change;
    change.peer_id = peer_id;
    change.from = from;
    change.to = to;
    change.reason = reason;
    change.controller = m_controller;
    change.generation = m_generation;
    out->push_back(change);
}

void SessionControl::Dispatch(const std::vector<ControlChange>& changes) {
    if (!on_change) {
        return;
    }
    // 锁外调用：回调里可以再次查询/操作本类。
    for (const ControlChange& change : changes) {
        on_change(change);
    }
}

bool SessionControl::Join(std::uint32_t peer_id) {
    if (peer_id == 0) {
        return false;
    }
    std::vector<ControlChange> changes;
    bool joined = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_participants.find(peer_id) != m_participants.end()) {
            return false;
        }
        if (m_participants.size() >= m_config.max_participants) {
            return false;
        }
        ControlMode assigned = m_config.default_mode;
        if (assigned == ControlMode::kInteract && m_controller != 0) {
            assigned = ControlMode::kView;
        }
        Entry entry;
        entry.mode = assigned;
        m_participants.emplace(peer_id, entry);
        if (assigned == ControlMode::kInteract) {
            m_controller = peer_id;
            m_lease_deadline_ms = m_config.lease_ms == 0 ? 0 : NowMs() + m_config.lease_ms;
        }
        AppendChange(&changes, peer_id, ControlMode::kNone, assigned,
                     ControlChangeReason::kJoined);
        joined = true;
    }
    Dispatch(changes);
    return joined;
}

bool SessionControl::RemoveParticipant(std::uint32_t peer_id, ControlChangeReason reason) {
    std::vector<ControlChange> changes;
    bool removed = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto found = m_participants.find(peer_id);
        if (found == m_participants.end()) {
            return false;
        }
        const ControlMode from = found->second.mode;
        if (m_controller == peer_id) {
            m_controller = 0;
            m_lease_deadline_ms = 0;
        }
        m_participants.erase(found);
        if (reason == ControlChangeReason::kDisconnected) {
            ++m_stats.disconnects;
        }
        AppendChange(&changes, peer_id, from, ControlMode::kNone, reason);
        removed = true;
    }
    Dispatch(changes);
    return removed;
}

bool SessionControl::Leave(std::uint32_t peer_id) {
    return RemoveParticipant(peer_id, ControlChangeReason::kLeft);
}

bool SessionControl::OnPeerDisconnected(std::uint32_t peer_id) {
    return RemoveParticipant(peer_id, ControlChangeReason::kDisconnected);
}

bool SessionControl::Grant(std::uint32_t peer_id) {
    return Grant(peer_id, ControlMode::kView);
}

bool SessionControl::Grant(std::uint32_t peer_id, ControlMode mode) {
    if (peer_id == 0) {
        return false;
    }
    std::vector<ControlChange> changes;
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
        // 显式授权即解除 Revoke 的禁入标记。
        entry.revoked = false;
        if (mode == ControlMode::kInteract) {
            if (m_controller != 0 && m_controller != peer_id) {
                const auto previous = m_participants.find(m_controller);
                if (previous != m_participants.end()) {
                    const ControlMode previous_mode = previous->second.mode;
                    previous->second.mode = ControlMode::kView;
                    if (previous_mode != ControlMode::kView) {
                        AppendChange(&changes, m_controller, previous_mode, ControlMode::kView,
                                     ControlChangeReason::kPreempted);
                    }
                }
                ++m_stats.preemptions;
            }
            entry.mode = ControlMode::kInteract;
            m_controller = peer_id;
            m_lease_deadline_ms =
                m_config.lease_ms == 0 ? 0 : NowMs() + m_config.lease_ms;
            if (from != ControlMode::kInteract) {
                AppendChange(&changes, peer_id, from, ControlMode::kInteract,
                             ControlChangeReason::kGranted);
            }
        } else {
            if (m_controller == peer_id) {
                m_controller = 0;
                m_lease_deadline_ms = 0;
            }
            entry.mode = mode;
            if (from != mode) {
                AppendChange(&changes, peer_id, from, mode, ControlChangeReason::kGranted);
            }
        }
        ++m_stats.grants;
        granted = true;
    }
    Dispatch(changes);
    return granted;
}

bool SessionControl::Revoke() {
    std::vector<ControlChange> changes;
    bool revoked = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_controller == 0) {
            return false;
        }
        const std::uint32_t target = m_controller;
        const auto found = m_participants.find(target);
        if (found != m_participants.end()) {
            const ControlMode from = found->second.mode;
            found->second.mode = ControlMode::kView;
            // 管理员强制收回：同时禁止其再次请求，直到 Grant 解除。
            found->second.revoked = true;
            m_controller = 0;
            m_lease_deadline_ms = 0;
            ++m_stats.revocations;
            AppendChange(&changes, target, from, ControlMode::kView,
                         ControlChangeReason::kRevoked);
            revoked = true;
        }
    }
    Dispatch(changes);
    return revoked;
}

bool SessionControl::Revoke(std::uint32_t peer_id) {
    if (peer_id == 0) {
        return false;
    }
    std::vector<ControlChange> changes;
    bool revoked = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto found = m_participants.find(peer_id);
        if (found == m_participants.end()) {
            return false;
        }
        const bool was_revoked = found->second.revoked;
        found->second.revoked = true;
        if (m_controller == peer_id) {
            const ControlMode from = found->second.mode;
            found->second.mode = ControlMode::kView;
            m_controller = 0;
            m_lease_deadline_ms = 0;
            ++m_stats.revocations;
            AppendChange(&changes, peer_id, from, ControlMode::kView,
                         ControlChangeReason::kRevoked);
        }
        // 幂等：已经处于撤销态时不再重复回调。
        (void)was_revoked;
        revoked = true;
    }
    Dispatch(changes);
    return revoked;
}

bool SessionControl::RequestControl(std::uint32_t peer_id) {
    if (peer_id == 0) {
        return false;
    }
    std::vector<ControlChange> changes;
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
        if (entry.revoked) {
            ++m_stats.denials;
            return false;
        }
        if (m_controller == peer_id) {
            // 幂等请求 = 续租。
            entry.mode = ControlMode::kInteract;
            m_lease_deadline_ms =
                m_config.lease_ms == 0 ? 0 : NowMs() + m_config.lease_ms;
            granted = true;
        } else if (m_controller == 0) {
            const ControlMode from = entry.mode;
            entry.mode = ControlMode::kInteract;
            m_controller = peer_id;
            m_lease_deadline_ms =
                m_config.lease_ms == 0 ? 0 : NowMs() + m_config.lease_ms;
            AppendChange(&changes, peer_id, from, ControlMode::kInteract,
                         ControlChangeReason::kRequested);
            granted = true;
        } else if (m_config.policy == ControlPolicy::kPreempt) {
            const auto previous = m_participants.find(m_controller);
            if (previous != m_participants.end()) {
                const ControlMode previous_mode = previous->second.mode;
                previous->second.mode = ControlMode::kView;
                if (previous_mode != ControlMode::kView) {
                    AppendChange(&changes, m_controller, previous_mode, ControlMode::kView,
                                 ControlChangeReason::kPreempted);
                }
            }
            ++m_stats.preemptions;
            const ControlMode from = entry.mode;
            entry.mode = ControlMode::kInteract;
            m_controller = peer_id;
            m_lease_deadline_ms =
                m_config.lease_ms == 0 ? 0 : NowMs() + m_config.lease_ms;
            AppendChange(&changes, peer_id, from, ControlMode::kInteract,
                         ControlChangeReason::kRequested);
            granted = true;
        } else {
            ++m_stats.denials;
        }
    }
    Dispatch(changes);
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
    const auto found = m_participants.find(peer_id);
    return found != m_participants.end() && found->second.revoked;
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
    std::lock_guard<std::mutex> lock(m_mutex);
    ControlSnapshot snapshot;
    snapshot.controller = m_controller;
    snapshot.generation = m_generation;
    if (m_controller != 0 && m_config.lease_ms != 0 && m_lease_deadline_ms != 0) {
        const std::uint64_t now = NowMs();
        if (m_lease_deadline_ms > now) {
            const std::uint64_t remaining = m_lease_deadline_ms - now;
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
        participant.revoked = entry.second.revoked;
        snapshot.participants.push_back(participant);
    }
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

bool SessionControl::Tick(std::uint64_t now_ms) {
    std::vector<ControlChange> changes;
    bool reclaimed = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_controller == 0 || m_config.lease_ms == 0 || m_lease_deadline_ms == 0 ||
            now_ms < m_lease_deadline_ms) {
            return false;
        }
        const auto found = m_participants.find(m_controller);
        if (found != m_participants.end()) {
            const ControlMode from = found->second.mode;
            found->second.mode = ControlMode::kView;
            ++m_stats.lease_expirations;
            AppendChange(&changes, found->first, from, ControlMode::kView,
                         ControlChangeReason::kLeaseExpired);
            reclaimed = true;
        }
        m_controller = 0;
        m_lease_deadline_ms = 0;
    }
    Dispatch(changes);
    return reclaimed;
}

bool SessionControl::Tick() {
    return Tick(NowMs());
}

}  // namespace go2cpp::bento
