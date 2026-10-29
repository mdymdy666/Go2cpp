/**
 * @file session_table.cpp
 * @brief 线程安全会话表实现：1:N 注册、上限拒绝与租约过期清理。
 * @author go2cpp contributors
 * @date 2026-10-28
 * @copyright MIT License
 */

#include "go2cpp/bento/net/session_table.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace go2cpp::bento {

namespace {

/// 饱和加法：`now + lease_ms` 在 now 接近 UINT64_MAX 时不能回绕成小值，
/// 否则租约会在“未来”变成过去，条目被误判为立即过期。
std::uint64_t SaturatingAddMs(std::uint64_t now_ms, std::uint64_t lease_ms) noexcept {
    const std::uint64_t max = std::numeric_limits<std::uint64_t>::max();
    return now_ms > max - lease_ms ? max : now_ms + lease_ms;
}

}  // namespace

const char* SessionStatusName(SessionStatus status) noexcept {
    switch (status) {
        case SessionStatus::kOk:
            return "ok";
        case SessionStatus::kNotFound:
            return "not_found";
        case SessionStatus::kAlreadyExists:
            return "already_exists";
        case SessionStatus::kFull:
            return "full";
        case SessionStatus::kExpired:
            return "expired";
        case SessionStatus::kInvalidArgument:
            return "invalid_argument";
        case SessionStatus::kKeyLimit:
            return "key_limit";
    }
    return "unknown";
}

bool SessionEntry::HasClient(std::uint64_t client_id) const noexcept {
    return std::any_of(clients.begin(), clients.end(),
                       [client_id](const SessionClient& client) {
                           return client.client_id == client_id;
                       });
}

SessionTable::SessionTable(SessionTableConfig config) : m_config(config) {
    // 0 会让所有 Add 永久失败，钳到 1；lease_ms == 0 有明确语义（永不过期），
    // 不做钳制。
    if (m_config.max_clients_per_key == 0) {
        m_config.max_clients_per_key = 1;
    }
    if (m_config.max_keys == 0) {
        m_config.max_keys = 1;
    }
}

void SessionTable::RefreshLeaseLocked(SessionEntry& entry, std::uint64_t now_ms) {
    if (m_config.lease_ms == 0) {
        // 永不过期：不建索引，deadline 固定 0。
        entry.lease_deadline_ms = 0;
        return;
    }
    // 先摘掉旧索引项再写新 deadline，避免索引里残留旧值导致提前删除。
    if (entry.lease_deadline_ms != 0) {
        m_expiries.erase({entry.lease_deadline_ms, entry.key});
    }
    entry.lease_deadline_ms = SaturatingAddMs(now_ms, m_config.lease_ms);
    m_expiries.emplace(entry.lease_deadline_ms, entry.key);
}

std::size_t SessionTable::RemoveExpiredLocked(std::uint64_t now_ms) {
    std::size_t removed = 0;
    // 索引按 deadline 升序；队头未到期就不可能有其他条目到期，直接返回。
    while (!m_expiries.empty()) {
        const auto expiry = m_expiries.begin();
        if (expiry->first > now_ms) {  // IsExpired 的判定是 now_ms >= deadline
            break;
        }
        const std::uint64_t deadline = expiry->first;
        const std::string key = expiry->second;  // erase 后引用失效，先拷出
        const auto entry = m_entries.find(key);
        // 防御陈旧索引项：条目可能已被显式删除。即便条目仍在，也要确认
        // 它的 deadline 与索引项一致，避免误删刚被续期的条目。
        if (entry != m_entries.end() && entry->second.lease_deadline_ms == deadline) {
            m_entries.erase(entry);
            ++m_stats.expirations;
            ++removed;
        }
        m_expiries.erase(expiry);
    }
    return removed;
}

void SessionTable::EraseEntryLocked(EntryMap::iterator entry) {
    if (entry->second.lease_deadline_ms != 0) {
        m_expiries.erase({entry->second.lease_deadline_ms, entry->first});
    }
    m_entries.erase(entry);
}

SessionStatus SessionTable::AddServer(const std::string& key, std::uint64_t server_id,
                                      std::uint64_t now_ms) {
    if (key.empty() || server_id == 0) {
        return SessionStatus::kInvalidArgument;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    RemoveExpiredLocked(now_ms);
    // lower_bound 一次定位：既判重复，又作为插入 hint 省掉第二次树查找。
    const auto position = m_entries.lower_bound(key);
    if (position != m_entries.end() && position->first == key) {
        ++m_stats.duplicate_rejects;
        return SessionStatus::kAlreadyExists;
    }
    if (m_entries.size() >= m_config.max_keys) {
        ++m_stats.key_limit_rejects;
        return SessionStatus::kKeyLimit;
    }

    SessionEntry entry;
    entry.key = key;
    entry.server_id = server_id;
    entry.created_ms = now_ms;
    RefreshLeaseLocked(entry, now_ms);
    m_entries.emplace_hint(position, key, std::move(entry));
    ++m_stats.servers_added;
    return SessionStatus::kOk;
}

SessionStatus SessionTable::AddClient(const std::string& key, std::uint64_t client_id,
                                      std::uint64_t now_ms) {
    if (key.empty() || client_id == 0) {
        return SessionStatus::kInvalidArgument;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    RemoveExpiredLocked(now_ms);
    const auto it = m_entries.find(key);
    if (it == m_entries.end()) {
        ++m_stats.not_found;
        return SessionStatus::kNotFound;
    }
    if (it->second.HasClient(client_id)) {
        ++m_stats.duplicate_rejects;
        return SessionStatus::kAlreadyExists;
    }
    if (it->second.clients.size() >= m_config.max_clients_per_key) {
        ++m_stats.full_rejects;
        return SessionStatus::kFull;
    }

    SessionClient client;
    client.client_id = client_id;
    client.joined_ms = now_ms;
    it->second.clients.push_back(client);
    RefreshLeaseLocked(it->second, now_ms);
    ++m_stats.clients_added;
    return SessionStatus::kOk;
}

bool SessionTable::Remove(const std::string& key, std::uint64_t now_ms) {
    if (key.empty()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    RemoveExpiredLocked(now_ms);
    const auto it = m_entries.find(key);
    if (it == m_entries.end()) {
        ++m_stats.not_found;
        return false;
    }
    EraseEntryLocked(it);
    ++m_stats.removals;
    return true;
}

SessionStatus SessionTable::RemoveClient(const std::string& key, std::uint64_t client_id,
                                         std::uint64_t now_ms) {
    if (key.empty() || client_id == 0) {
        return SessionStatus::kInvalidArgument;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    RemoveExpiredLocked(now_ms);
    const auto it = m_entries.find(key);
    if (it == m_entries.end()) {
        ++m_stats.not_found;
        return SessionStatus::kNotFound;
    }
    auto& clients = it->second.clients;
    const auto client_it =
        std::find_if(clients.begin(), clients.end(), [client_id](const SessionClient& client) {
            return client.client_id == client_id;
        });
    if (client_it == clients.end()) {
        ++m_stats.not_found;
        return SessionStatus::kNotFound;
    }
    clients.erase(client_it);
    RefreshLeaseLocked(it->second, now_ms);
    ++m_stats.removals;
    return SessionStatus::kOk;
}

SessionStatus SessionTable::RemoveServer(const std::string& key, std::uint64_t server_id,
                                         std::uint64_t now_ms) {
    if (key.empty() || server_id == 0) {
        return SessionStatus::kInvalidArgument;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    RemoveExpiredLocked(now_ms);
    const auto it = m_entries.find(key);
    if (it == m_entries.end() || it->second.server_id != server_id) {
        ++m_stats.not_found;
        return SessionStatus::kNotFound;
    }
    EraseEntryLocked(it);
    ++m_stats.removals;
    return SessionStatus::kOk;
}

bool SessionTable::Touch(const std::string& key, std::uint64_t now_ms) {
    if (key.empty()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    RemoveExpiredLocked(now_ms);
    const auto it = m_entries.find(key);
    if (it == m_entries.end()) {
        ++m_stats.not_found;
        return false;
    }
    RefreshLeaseLocked(it->second, now_ms);
    ++m_stats.touches;
    return true;
}

std::optional<SessionEntry> SessionTable::Find(const std::string& key,
                                               std::uint64_t now_ms) const {
    if (key.empty()) {
        return std::nullopt;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_entries.find(key);
    if (it == m_entries.end() || it->second.IsExpired(now_ms)) {
        return std::nullopt;
    }
    return it->second;
}

std::vector<SessionEntry> SessionTable::List(std::uint64_t now_ms) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<SessionEntry> entries;
    entries.reserve(m_entries.size());
    for (const auto& pair : m_entries) {
        if (!pair.second.IsExpired(now_ms)) {
            entries.push_back(pair.second);
        }
    }
    return entries;
}

bool SessionTable::Contains(const std::string& key, std::uint64_t now_ms) const {
    if (key.empty()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_entries.find(key);
    return it != m_entries.end() && !it->second.IsExpired(now_ms);
}

std::size_t SessionTable::size() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_entries.size();
}

bool SessionTable::empty() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_entries.empty();
}

std::size_t SessionTable::Tick(std::uint64_t now_ms) {
    std::lock_guard<std::mutex> lock(m_mutex);
    return RemoveExpiredLocked(now_ms);
}

SessionTableStats SessionTable::stats() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_stats;
}

}  // namespace go2cpp::bento
