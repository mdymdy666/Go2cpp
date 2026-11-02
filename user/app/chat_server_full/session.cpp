/**
 * @file session.cpp
 * @brief SessionTable 的实现：加入/离开/恢复/房间/超时。
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include "session.h"

#include <set>
#include <utility>

namespace chat {
namespace {

constexpr const char* kDefaultRoom = "lobby";

}  // namespace

SessionTable::SessionTable(std::uint32_t reconnect_grace_ms) : m_grace_ms(reconnect_grace_ms) {}

std::string SessionTable::NextTokenLocked() {
    m_next_token += 1;
    return "tok-" + std::to_string(m_next_token);
}

JoinResult SessionTable::Join(PeerId peer, const std::string& name, const std::string& token,
                              std::uint64_t now_ms) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto existing = m_online.find(peer);
    if (existing != m_online.end()) {
        JoinResult result;
        result.ok = true;
        result.resumed = true;
        result.token = existing->second.token;
        result.session = existing->second;
        return result;
    }

    if (!token.empty()) {
        const auto offline = m_offline.find(token);
        if (offline != m_offline.end() && offline->second.expires_ms > now_ms) {
            if (m_names.find(offline->second.name) != m_names.end()) {
                JoinResult result;
                result.error = "会话昵称已被在线用户占用，请重新 JOIN";
                return result;
            }
            SessionInfo session;
            session.peer = peer;
            session.name = offline->second.name;
            session.room = offline->second.room;
            session.token = token;
            session.last_seen_ms = now_ms;
            m_offline.erase(offline);
            m_names.emplace(session.name, peer);
            m_online.emplace(peer, session);
            JoinResult result;
            result.ok = true;
            result.resumed = true;
            result.token = token;
            result.session = session;
            return result;
        }
    }

    if (m_names.find(name) != m_names.end()) {
        JoinResult result;
        result.error = "昵称已被占用: " + name;
        return result;
    }
    SessionInfo session;
    session.peer = peer;
    session.name = name;
    session.room = kDefaultRoom;
    session.token = NextTokenLocked();
    session.last_seen_ms = now_ms;
    m_names.emplace(session.name, peer);
    m_online.emplace(peer, session);

    JoinResult result;
    result.ok = true;
    result.token = session.token;
    result.session = session;
    return result;
}

std::optional<SessionInfo> SessionTable::Leave(PeerId peer, std::uint64_t now_ms) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto found = m_online.find(peer);
    if (found == m_online.end()) {
        return std::nullopt;
    }
    const SessionInfo snapshot = found->second;
    m_names.erase(snapshot.name);
    OfflineEntry offline;
    offline.name = snapshot.name;
    offline.room = snapshot.room;
    offline.expires_ms = now_ms + m_grace_ms;
    m_offline[snapshot.token] = std::move(offline);
    m_online.erase(found);
    return snapshot;
}

std::optional<SessionInfo> SessionTable::Resume(const std::string& token, PeerId peer,
                                                std::uint64_t now_ms) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (token.empty() || m_online.find(peer) != m_online.end()) {
        return std::nullopt;
    }

    const auto offline = m_offline.find(token);
    if (offline != m_offline.end()) {
        if (offline->second.expires_ms <= now_ms) {
            return std::nullopt;
        }
        if (m_names.find(offline->second.name) != m_names.end()) {
            return std::nullopt;
        }
        SessionInfo session;
        session.peer = peer;
        session.name = offline->second.name;
        session.room = offline->second.room;
        session.token = token;
        session.last_seen_ms = now_ms;
        m_offline.erase(offline);
        m_names.emplace(session.name, peer);
        m_online.emplace(peer, session);
        return session;
    }

    // 旧连接可能还没被链路层判超时（UDP 无 FIN）：同一 token 的新连接直接
    // 接管会话，旧 peer 的后续消息因不在 m_online 而被忽略，等待自然超时。
    for (auto entry = m_online.begin(); entry != m_online.end(); ++entry) {
        if (entry->second.token != token) {
            continue;
        }
        const std::string name = entry->second.name;
        const std::string room = entry->second.room;
        m_names.erase(name);
        m_online.erase(entry);
        SessionInfo session;
        session.peer = peer;
        session.name = name;
        session.room = room;
        session.token = token;
        session.last_seen_ms = now_ms;
        m_names.emplace(name, peer);
        m_online.emplace(peer, session);
        return session;
    }
    return std::nullopt;
}

bool SessionTable::Touch(PeerId peer, std::uint64_t now_ms) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto found = m_online.find(peer);
    if (found == m_online.end()) {
        return false;
    }
    found->second.last_seen_ms = now_ms;
    return true;
}

std::optional<SessionInfo> SessionTable::SetRoom(PeerId peer, const std::string& room,
                                                 std::uint64_t now_ms) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto found = m_online.find(peer);
    if (found == m_online.end()) {
        return std::nullopt;
    }
    const std::string previous = found->second.room;
    if (previous == room) {
        return found->second;
    }
    found->second.room = room;
    found->second.last_seen_ms = now_ms;
    return found->second;
}

std::optional<SessionInfo> SessionTable::Get(PeerId peer) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto found = m_online.find(peer);
    if (found == m_online.end()) {
        return std::nullopt;
    }
    return found->second;
}

std::optional<SessionInfo> SessionTable::FindByName(const std::string& name) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto peer = m_names.find(name);
    if (peer == m_names.end()) {
        return std::nullopt;
    }
    const auto session = m_online.find(peer->second);
    if (session == m_online.end()) {
        return std::nullopt;
    }
    return session->second;
}

std::vector<SessionInfo> SessionTable::Online() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<SessionInfo> sessions;
    sessions.reserve(m_online.size());
    for (const auto& entry : m_online) {
        sessions.push_back(entry.second);
    }
    return sessions;
}

std::vector<SessionInfo> SessionTable::InRoom(const std::string& room) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<SessionInfo> sessions;
    for (const auto& entry : m_online) {
        if (entry.second.room == room) {
            sessions.push_back(entry.second);
        }
    }
    return sessions;
}

std::vector<std::string> SessionTable::Rooms() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::set<std::string> rooms{kDefaultRoom};
    for (const auto& entry : m_online) {
        rooms.insert(entry.second.room);
    }
    return std::vector<std::string>(rooms.begin(), rooms.end());
}

std::vector<SessionInfo> SessionTable::ExpireIdle(std::uint64_t now_ms,
                                                  std::uint32_t timeout_ms) {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<SessionInfo> expired;
    for (auto entry = m_online.begin(); entry != m_online.end();) {
        const std::uint64_t last = entry->second.last_seen_ms;
        if (now_ms > last && now_ms - last > timeout_ms) {
            const SessionInfo snapshot = entry->second;
            OfflineEntry offline;
            offline.name = snapshot.name;
            offline.room = snapshot.room;
            offline.expires_ms = now_ms + m_grace_ms;
            m_offline[snapshot.token] = std::move(offline);
            m_names.erase(snapshot.name);
            expired.push_back(snapshot);
            entry = m_online.erase(entry);
            continue;
        }
        ++entry;
    }
    return expired;
}

std::size_t SessionTable::PurgeOffline(std::uint64_t now_ms) {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::size_t removed = 0;
    for (auto entry = m_offline.begin(); entry != m_offline.end();) {
        if (entry->second.expires_ms <= now_ms) {
            entry = m_offline.erase(entry);
            ++removed;
            continue;
        }
        ++entry;
    }
    return removed;
}

std::size_t SessionTable::online_count() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_online.size();
}

std::size_t SessionTable::offline_count() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_offline.size();
}

}  // namespace chat
