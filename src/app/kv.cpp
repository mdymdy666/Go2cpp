/**
 * @file kv.cpp
 * @brief 有序 KV 表：文本/二进制编码实现。
 *
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include "go2cpp/app/kv.hpp"

#include "go2cpp/app/frame.hpp"

#include <algorithm>
#include <cstdio>

namespace go2cpp::app {
namespace {

constexpr std::size_t kMaxEntries = 1024u * 1024u;
constexpr std::size_t kMaxAtomBytes = 16u * 1024u * 1024u;
constexpr std::size_t kMaxBinaryBytes = 256u * 1024u * 1024u;
constexpr std::size_t kMaxTextBytes = 256u * 1024u * 1024u;

/// @brief varint 编码后的字节数。
std::size_t VarintSize(std::uint64_t value) noexcept {
    std::size_t bytes = 1;
    while (value >= 0x80u) {
        value >>= 7;
        ++bytes;
    }
    return bytes;
}

void SetError(std::string* error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
}

}  // namespace

KvTable::KvTable(std::initializer_list<Entry> entries) {
    for (const Entry& entry : entries) {
        Set(entry.first, entry.second);
    }
}

const std::string* KvTable::Find(std::string_view key) const noexcept {
    const std::size_t index = Locate(key);
    return index == kNoIndex ? nullptr : &m_entries[index].second;
}

std::uint64_t KvTable::HashKey(std::string_view key) noexcept {
    // FNV-1a 64：每字节一次异或 + 一次乘法，短键上比通用哈希更便宜，
    // 且不依赖标准库实现（跨编译器结果一致，便于复现基准）。
    std::uint64_t hash = 1469598103934665603ULL;  // FNV offset basis
    for (char c : key) {
        hash ^= static_cast<std::uint8_t>(c);
        hash *= 1099511628211ULL;  // FNV prime
    }
    return hash;
}

std::size_t KvTable::Locate(std::string_view key) const noexcept {
    if (m_index.empty()) {
        for (std::size_t i = 0; i < m_entries.size(); ++i) {
            if (m_entries[i].first == key) {
                return i;
            }
        }
        return kNoIndex;
    }
    const std::size_t mask = m_index.size() - 1;
    const std::uint64_t hash = HashKey(key);
    std::size_t slot = static_cast<std::size_t>(hash) & mask;
    // 负载因子 <= 0.5，探测链上必然存在空槽，循环一定终止。
    for (;;) {
        const IndexSlot& probe = m_index[slot];
        if (probe.entry_plus_one == 0) {
            return kNoIndex;
        }
        if (probe.hash == hash && m_entries[probe.entry_plus_one - 1].first == key) {
            return static_cast<std::size_t>(probe.entry_plus_one - 1);
        }
        slot = (slot + 1) & mask;
    }
}

void KvTable::IndexInsert(std::size_t entry_index) noexcept {
    if (m_index.empty()) {
        return;
    }
    const std::uint64_t hash = HashKey(m_entries[entry_index].first);
    const std::size_t mask = m_index.size() - 1;
    std::size_t slot = static_cast<std::size_t>(hash) & mask;
    while (m_index[slot].entry_plus_one != 0) {
        slot = (slot + 1) & mask;
    }
    m_index[slot].hash = hash;
    m_index[slot].entry_plus_one = static_cast<std::uint32_t>(entry_index + 1);
    ++m_index_used;
}

void KvTable::RebuildIndex() {
    if (m_entries.size() <= kLinearScanLimit) {
        // 小表：保留已分配的容量（下次变大时直接复用），但按「未启用」处理。
        m_index.clear();
        m_index_used = 0;
        return;
    }
    std::size_t capacity = 16;
    while (capacity < m_entries.size() * 2) {
        capacity <<= 1;
    }
    m_index.assign(capacity, IndexSlot{});
    m_index_used = 0;
    for (std::size_t i = 0; i < m_entries.size(); ++i) {
        IndexInsert(i);
    }
}

std::string KvTable::Get(std::string_view key, std::string_view fallback) const {
    const std::string* value = Find(key);
    return value != nullptr ? *value : std::string(fallback);
}

bool KvTable::Set(std::string key, std::string value) {
    const std::size_t existing = Locate(key);
    if (existing != kNoIndex) {
        m_entries[existing].second = std::move(value);
        return false;
    }
    m_entries.emplace_back(std::move(key), std::move(value));
    if (m_entries.size() > kLinearScanLimit) {
        // 第 9 条起建索引；之后每次新增，负载超过 0.5 才整体重建。
        if (m_index.empty() || (m_index_used + 1) * 2 > m_index.size()) {
            RebuildIndex();
        } else {
            IndexInsert(m_entries.size() - 1);
        }
    }
    return true;
}

bool KvTable::Erase(std::string_view key) {
    const std::size_t index = Locate(key);
    if (index == kNoIndex) {
        return false;
    }
    m_entries.erase(m_entries.begin() + static_cast<std::ptrdiff_t>(index));
    if (m_entries.size() > kLinearScanLimit) {
        // erase 会移动后续条目，下标索引整体失效；删除不是热路径，直接重建。
        RebuildIndex();
    } else {
        m_index.clear();
        m_index_used = 0;
    }
    return true;
}

bool KvTable::PrintableAscii() const noexcept {
    for (const Entry& entry : m_entries) {
        for (char c : entry.first) {
            const unsigned char uc = static_cast<unsigned char>(c);
            if (uc < 0x20 || uc > 0x7e) {
                return false;
            }
        }
        for (char c : entry.second) {
            const unsigned char uc = static_cast<unsigned char>(c);
            if (uc < 0x20 || uc > 0x7e) {
                return false;
            }
        }
    }
    return true;
}

std::string EscapeKvText(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (char c : value) {
        switch (c) {
            case '\\':
                out += "\\\\";
                break;
            case ';':
                out += "\\;";
                break;
            case '=':
                out += "\\=";
                break;
            default:
                out.push_back(c);
                break;
        }
    }
    return out;
}

std::string KvTable::ToText() const {
    std::string out;
    for (std::size_t i = 0; i < m_entries.size(); ++i) {
        if (i != 0) {
            out.push_back(';');
        }
        out += EscapeKvText(m_entries[i].first);
        out.push_back('=');
        out += EscapeKvText(m_entries[i].second);
    }
    return out;
}

bool KvTable::ParseText(std::string_view text, KvTable* out, std::string* error) {
    if (out == nullptr) {
        SetError(error, "output table is null");
        return false;
    }
    if (text.size() > kMaxTextBytes) {
        SetError(error, "kv text too large");
        return false;
    }
    KvTable parsed;
    std::string key;
    std::string value;
    bool in_value = false;
    bool has_item = false;
    bool escaped = false;
    auto flush = [&]() -> bool {
        if (!has_item) {
            return true;
        }
        if (key.empty() && !in_value) {
            SetError(error, "empty key");
            return false;
        }
        if (key.empty()) {
            SetError(error, "empty key");
            return false;
        }
        parsed.Set(std::move(key), std::move(value));
        key.clear();
        value.clear();
        in_value = false;
        has_item = false;
        return true;
    };
    for (char c : text) {
        if (escaped) {
            escaped = false;
            if (in_value) {
                value.push_back(c);
            } else {
                key.push_back(c);
            }
            has_item = true;
            continue;
        }
        if (c == '\\') {
            escaped = true;
            continue;
        }
        if (c == ';') {
            if (!flush()) {
                return false;
            }
            continue;
        }
        if (c == '=' && !in_value) {
            in_value = true;
            has_item = true;
            continue;
        }
        if (in_value) {
            value.push_back(c);
        } else {
            key.push_back(c);
        }
        has_item = true;
    }
    if (escaped) {
        // 结尾孤立反斜杠：按字面量处理（宽松），不报错。
        if (in_value) {
            value.push_back('\\');
        } else {
            key.push_back('\\');
        }
        has_item = true;
    }
    if (!flush()) {
        return false;
    }
    if (parsed.size() > kMaxEntries) {
        SetError(error, "too many kv entries");
        return false;
    }
    *out = std::move(parsed);
    return true;
}

std::size_t KvTable::BinarySize() const noexcept {
    std::size_t size = VarintSize(m_entries.size());
    for (const Entry& entry : m_entries) {
        size += VarintSize(entry.first.size()) + entry.first.size();
        size += VarintSize(entry.second.size()) + entry.second.size();
        size += 2;  // 经验上留出 codepoint 抖动，仅用于预估。
    }
    return size;
}

std::vector<std::uint8_t> KvTable::ToBinary() const {
    std::vector<std::uint8_t> out;
    out.reserve(BinarySize());
    PutVarint(out, m_entries.size());
    for (const Entry& entry : m_entries) {
        PutString(out, entry.first);
        PutString(out, entry.second);
    }
    return out;
}

bool KvTable::ParseBinary(const std::uint8_t* data, std::size_t size, KvTable* out,
                          std::string* error) {
    if (out == nullptr || (data == nullptr && size != 0)) {
        SetError(error, "invalid kv binary input");
        return false;
    }
    if (size > kMaxBinaryBytes) {
        SetError(error, "kv binary too large");
        return false;
    }
    std::size_t offset = 0;
    std::uint64_t count = 0;
    std::size_t consumed = 0;
    if (!GetVarint(data, size, &count, &consumed)) {
        SetError(error, "truncated kv entry count");
        return false;
    }
    if (count > kMaxEntries) {
        SetError(error, "too many kv entries");
        return false;
    }
    offset += consumed;
    KvTable parsed;
    for (std::uint64_t i = 0; i < count; ++i) {
        std::string key;
        std::string value;
        if (!GetString(data + offset, size - offset, &key, &consumed, kMaxAtomBytes)) {
            SetError(error, "truncated kv key");
            return false;
        }
        offset += consumed;
        if (!GetString(data + offset, size - offset, &value, &consumed, kMaxAtomBytes)) {
            SetError(error, "truncated kv value");
            return false;
        }
        offset += consumed;
        parsed.Set(std::move(key), std::move(value));
    }
    if (offset != size) {
        SetError(error, "trailing bytes in kv payload");
        return false;
    }
    *out = std::move(parsed);
    return true;
}

std::string PrintablePreview(std::string_view value, std::size_t max_bytes) {
    std::string out;
    const std::size_t limit = std::min(value.size(), max_bytes);
    out.reserve(limit + 3);
    for (std::size_t i = 0; i < limit; ++i) {
        const unsigned char c = static_cast<unsigned char>(value[i]);
        if (c >= 0x20 && c <= 0x7e) {
            out.push_back(static_cast<char>(c));
        } else {
            char buffer[5];
            std::snprintf(buffer, sizeof(buffer), "\\x%02x", c);
            out += buffer;
        }
    }
    if (value.size() > limit) {
        out += "...";
    }
    return out;
}

}  // namespace go2cpp::app
