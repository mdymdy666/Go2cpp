/**
 * @file token_pairing.cpp
 * @brief (key, token) 1:N 配对表的实现。
 * @details 三张表在每次改动后保持同步：
 *
 *          - m_by_token：token -> Slot，主索引，承担 Find/Release/size；
 *          - m_by_key：key -> token 集合，二级索引，承担 List；
 *          - m_purged：过期槽累计回收计数。
 *
 *          所有改动都在同一把 mutex 内完成，因此「主索引有、二级索引无」
 *          这类中间状态不会暴露给其他线程。过期的判定统一用
 *          Slot::IsExpired(NowMs())，时间戳来自 steady_clock，不受系统时间
 *          调整影响。
 *
 *          本文件不写任何日志：token 是 bearer secret，日志里只能出现
 *          key + peer_id。
 *
 * @author go2cpp contributors
 * @date 2026-10-30
 * @copyright MIT License
 */

#include "go2cpp/bento/net/token_pairing.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <random>

#include "go2cpp/bento/net/protocol.hpp"

namespace go2cpp::bento {

namespace {

/// Issue 在极低概率的 token 碰撞下的重试次数。64 bit 熵下两次都碰撞的
/// 概率约为 2^-128，设置上限只是为了不让失败路径变成死循环。
constexpr int kIssueAttempts = 2;

/// 十六进制字符表。只输出小写，与 IsValidToken 的严格校验保持一致。
constexpr char kHexDigits[] = "0123456789abcdef";

/// 每线程复用一个 random_device，避免每次 GenerateToken 都重新打开熵源。
/// random_device 自身不保证线程安全，thread_local 保证每个线程各用各的。
std::random_device& ThreadRandomDevice() {
    static thread_local std::random_device device;
    return device;
}

/// 生成一个 8 字节随机值。libstdc++ 的 random_device 每次调用返回一个
/// 均匀分布的 32 位值，两次拼成 64 位，减少一半的熵源读取次数。
std::uint64_t RandomU64() {
    std::random_device& device = ThreadRandomDevice();
    const std::uint64_t high = static_cast<std::uint64_t>(device());
    const std::uint64_t low = static_cast<std::uint64_t>(device());
    return (high << 32) | low;
}

}  // namespace

TokenPairing::TokenPairing(TokenPairingConfig config)
    : m_max_slots(config.max_slots), m_ttl_ms(config.ttl_ms) {}

std::string TokenPairing::GenerateToken() {
    const std::uint64_t value = RandomU64();
    std::string token(kTokenHexChars, '0');
    for (std::size_t index = 0; index < kTokenBytes; ++index) {
        // 高位在前，保证同一随机值总是编码成同一文本（便于测试复现）。
        const std::uint8_t byte =
            static_cast<std::uint8_t>(value >> (8U * (kTokenBytes - 1U - index)));
        token[2U * index] = kHexDigits[byte >> 4U];
        token[2U * index + 1U] = kHexDigits[byte & 0x0FU];
    }
    return token;
}

bool TokenPairing::IsValidToken(std::string_view token) noexcept {
    if (token.size() != kTokenHexChars) {
        return false;
    }
    for (const char character : token) {
        const bool is_digit = character >= '0' && character <= '9';
        const bool is_lower_hex = character >= 'a' && character <= 'f';
        if (!is_digit && !is_lower_hex) {
            return false;
        }
    }
    return true;
}

bool TokenPairing::IsValidKey(std::string_view key) noexcept {
    return !key.empty() && key.size() <= kMaxKeyBytes;
}

std::optional<Slot> TokenPairing::Pair(std::string_view key, std::string_view token,
                                       PeerId peer_id) {
    if (!IsValidKey(key) || !IsValidToken(token)) {
        return std::nullopt;
    }
    // 锁外构造字符串：unordered_map 在 C++17 没有异构查找，必须持有 key 的
    // 所有权；把分配放在锁外可以缩短临界区。
    std::string token_text(token);
    const std::uint64_t now_ms = NowMs();

    std::lock_guard<std::mutex> lock(m_mutex);
    // token 已存在时直接拒绝（即使它已过期但尚未回收）：复用别人的 bearer
    // token 没有合理业务场景，拒绝可以避免调用方把冲突当成成功。
    if (m_by_token.find(token_text) != m_by_token.end()) {
        return std::nullopt;
    }
    if (m_by_token.size() >= m_max_slots) {
        // 容量压力下才做全表过期回收：正常路径（未满）保持 O(1)。
        PurgeExpiredLocked(now_ms);
        if (m_by_token.size() >= m_max_slots) {
            return std::nullopt;
        }
    }

    Slot slot;
    slot.key = std::string(key);
    slot.token = token_text;
    slot.peer_id = peer_id;
    slot.created_ms = now_ms;
    // steady_clock 毫秒数距离 uint64 上限还有上百万年，不做饱和处理。
    slot.expires_ms = m_ttl_ms == 0 ? 0 : now_ms + m_ttl_ms;

    m_by_key[slot.key].insert(token_text);
    m_by_token.emplace(token_text, slot);
    return slot;
}

std::optional<Slot> TokenPairing::Issue(std::string_view key, PeerId peer_id) {
    if (!IsValidKey(key)) {
        return std::nullopt;
    }
    for (int attempt = 0; attempt < kIssueAttempts; ++attempt) {
        std::optional<Slot> slot = Pair(key, GenerateToken(), peer_id);
        if (slot.has_value()) {
            return slot;
        }
        // 失败原因只有 token 碰撞或容量满；后者重试也不会成功，但重试一次
        // 的成本远低于为区分原因再加一条查询路径。
    }
    return std::nullopt;
}

std::optional<Slot> TokenPairing::Find(std::string_view key, std::string_view token) const {
    if (!IsValidKey(key) || !IsValidToken(token)) {
        return std::nullopt;
    }
    std::string token_text(token);
    const std::uint64_t now_ms = NowMs();

    std::lock_guard<std::mutex> lock(m_mutex);
    const auto position = m_by_token.find(token_text);
    if (position == m_by_token.end()) {
        return std::nullopt;
    }
    if (std::string_view(position->second.key) != key) {
        return std::nullopt;
    }
    if (position->second.IsExpired(now_ms)) {
        return std::nullopt;
    }
    return position->second;
}

bool TokenPairing::Release(std::string_view token) {
    if (!IsValidToken(token)) {
        return false;
    }
    std::string token_text(token);
    const std::uint64_t now_ms = NowMs();

    std::lock_guard<std::mutex> lock(m_mutex);
    const auto position = m_by_token.find(token_text);
    if (position == m_by_token.end()) {
        return false;
    }
    const bool live = !position->second.IsExpired(now_ms);
    RemoveKeyRefLocked(position->second.key, token_text);
    m_by_token.erase(position);
    if (!live) {
        // Release 也是一条回收路径：过期的 token 返回 false，但槽位被回收，
        // 计入 purged 才能让「过期量」与「释放失败」区分开。
        ++m_purged;
    }
    return live;
}

std::vector<Slot> TokenPairing::List(std::string_view key) const {
    if (!IsValidKey(key)) {
        return {};
    }
    std::string key_text(key);
    const std::uint64_t now_ms = NowMs();

    std::lock_guard<std::mutex> lock(m_mutex);
    const auto position = m_by_key.find(key_text);
    if (position == m_by_key.end()) {
        return {};
    }
    std::vector<Slot> slots;
    slots.reserve(position->second.size());
    for (const std::string& token : position->second) {
        const auto slot_position = m_by_token.find(token);
        if (slot_position == m_by_token.end()) {
            // 正常路径不会出现；防御性跳过，避免索引不一致时解引用空项。
            continue;
        }
        if (slot_position->second.IsExpired(now_ms)) {
            continue;
        }
        slots.push_back(slot_position->second);
    }
    std::sort(slots.begin(), slots.end(), [](const Slot& first, const Slot& second) {
        if (first.created_ms != second.created_ms) {
            return first.created_ms < second.created_ms;
        }
        return first.token < second.token;
    });
    return slots;
}

std::size_t TokenPairing::PurgeExpired() {
    const std::uint64_t now_ms = NowMs();
    std::lock_guard<std::mutex> lock(m_mutex);
    return PurgeExpiredLocked(now_ms);
}

std::size_t TokenPairing::size() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_by_token.size();
}

std::size_t TokenPairing::PurgeExpiredLocked(std::uint64_t now_ms) {
    std::size_t removed = 0;
    for (auto position = m_by_token.begin(); position != m_by_token.end();) {
        if (!position->second.IsExpired(now_ms)) {
            ++position;
            continue;
        }
        RemoveKeyRefLocked(position->second.key, position->first);
        position = m_by_token.erase(position);
        ++removed;
    }
    m_purged += removed;
    return removed;
}

void TokenPairing::RemoveKeyRefLocked(const std::string& key, const std::string& token) {
    const auto position = m_by_key.find(key);
    if (position == m_by_key.end()) {
        return;
    }
    position->second.erase(token);
    if (position->second.empty()) {
        m_by_key.erase(position);
    }
}

}  // namespace go2cpp::bento
