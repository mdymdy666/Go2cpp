/**
 * @file kv_stream.cpp
 * @brief KV 流实现：小表单帧、大表流式、每键一流。
 *
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include "go2cpp/app/kv_stream.hpp"

#include <algorithm>
#include <utility>

namespace go2cpp::app {
namespace {

bool IsReservedKey(std::string_view key) noexcept {
    return key.rfind("app.", 0) == 0;
}

void SetError(std::string* error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
}

}  // namespace

const char* KvModeName(KvMode mode) noexcept {
    switch (mode) {
        case KvMode::kSingle:
            return "single";
        case KvMode::kStream:
            return "stream";
        case KvMode::kPerKey:
            return "perkey";
    }
    return "single";
}

bool ParseKvMode(std::string_view name, KvMode* mode) noexcept {
    if (mode == nullptr) {
        return false;
    }
    if (name == "single") {
        *mode = KvMode::kSingle;
        return true;
    }
    if (name == "stream") {
        *mode = KvMode::kStream;
        return true;
    }
    if (name == "perkey") {
        *mode = KvMode::kPerKey;
        return true;
    }
    return false;
}

KvProducer::KvProducer(KvTable table, std::size_t small_threshold,
                       std::size_t small_value_limit, KvMode forced_mode)
    : m_table(std::move(table)),
      m_small_threshold(small_threshold),
      m_small_value_limit(small_value_limit),
      m_mode(forced_mode),
      m_forced_mode(forced_mode) {
    m_key_count = 0;
    for (const auto& entry : m_table.entries()) {
        if (!IsReservedKey(entry.first)) {
            ++m_key_count;
        }
    }
}

void KvProducer::Partition(std::uint32_t max_payload) {
    KvTable small;
    KvTable large;
    for (const auto& entry : m_table.entries()) {
        if (IsReservedKey(entry.first)) {
            continue;
        }
        if (entry.second.size() > m_small_value_limit) {
            large.Set(entry.first, entry.second);
        } else {
            small.Set(entry.first, entry.second);
        }
    }
    // 为保留键（app.kind/app.kv.mode/app.kv.keys/app.kv.partial）预留 256B，
    // 否则「刚好装满 max_payload」的 HEADERS 加上保留键后会超限而发不出去。
    const std::size_t frame_limit =
        max_payload > 512 ? static_cast<std::size_t>(max_payload) - 256
                          : static_cast<std::size_t>(max_payload) / 2;
    const std::size_t single_limit = std::min<std::size_t>(
        frame_limit, m_small_threshold == 0 ? frame_limit : m_small_threshold);
    m_mode = m_forced_mode == KvMode::kPerKey ? KvMode::kPerKey : KvMode::kSingle;
    if (large.empty() && small.ToBinary().size() <= single_limit) {
        m_header_table = std::move(small);
        m_body.clear();
        return;
    }
    // 需要分区：HEADERS 装小值，DATA 装大值。HEADERS 自身也必须装得下。
    m_mode = m_forced_mode == KvMode::kPerKey ? KvMode::kPerKey : KvMode::kStream;
    if (small.ToBinary().size() > frame_limit) {
        // 小值太多：按值长度从大到小移入大值区，直到 HEADERS 可装下。
        while (!small.empty() && small.ToBinary().size() > frame_limit) {
            const std::pair<std::string, std::string>* victim = nullptr;
            for (const auto& entry : small.entries()) {
                if (victim == nullptr || entry.second.size() > victim->second.size()) {
                    victim = &entry;
                }
            }
            if (victim == nullptr) {
                break;
            }
            large.Set(victim->first, victim->second);
            small.Erase(victim->first);
        }
    }
    m_header_table = std::move(small);
    m_body = large.ToBinary();
}

bool KvProducer::ProduceHeaders(std::uint32_t max_payload, Frame* out) {
    m_header_table.Set(kAppKindKey, StreamKindKey(StreamKind::kKv));
    m_header_table.Set(kKvModeKey, KvModeName(m_mode));
    m_header_table.Set(kKvKeysKey, std::to_string(m_key_count));
    if (!m_body.empty()) {
        m_header_table.Set(kKvPartialKey, "1");
    }
    m_header_table.ToBinaryInto(&out->payload);
    if (out->payload.size() > max_payload ||
        out->payload.size() > kMaxFramePayloadLimit) {
        out->payload.clear();
        m_done = true;
        return false;
    }
    out->type = FrameType::kHeaders;
    out->flags = kFlagEndHeaders;
    m_headers_sent = true;
    m_produced_bytes += out->payload.size();
    if (m_body.empty()) {
        out->SetFlag(kFlagEndStream);
        m_done = true;
    }
    return true;
}

bool KvProducer::ProduceData(std::uint32_t max_payload, Frame* out) {
    if (m_body_offset >= m_body.size()) {
        m_done = true;
        return false;
    }
    const std::size_t take = std::min<std::size_t>(max_payload, m_body.size() - m_body_offset);
    out->type = FrameType::kData;
    out->flags = 0;
    out->payload.assign(m_body.begin() + static_cast<std::ptrdiff_t>(m_body_offset),
                        m_body.begin() +
                            static_cast<std::ptrdiff_t>(m_body_offset + take));
    m_body_offset += take;
    m_produced_bytes += take;
    if (m_body_offset >= m_body.size()) {
        out->SetFlag(kFlagEndStream);
        m_done = true;
    }
    return true;
}

bool KvProducer::Produce(std::uint32_t max_payload, Frame* out) {
    if (m_done || out == nullptr) {
        return false;
    }
    if (!m_headers_sent) {
        if (max_payload == 0) {
            return false;
        }
        Partition(max_payload);
        return ProduceHeaders(max_payload, out);
    }
    return ProduceData(max_payload, out);
}

// ---------------------------------------------------------------------------
// KvAssembler
// ---------------------------------------------------------------------------

KvAssembler::KvAssembler(std::size_t max_bytes) : m_max_bytes(max_bytes) {}

bool KvAssembler::OnHeaders(const KvTable& table, std::string* error) {
    if (m_has_headers) {
        m_error = "duplicate kv headers";
        SetError(error, m_error);
        return false;
    }
    if (const std::string* kind = table.Find(kAppKindKey); kind != nullptr &&
        *kind != StreamKindKey(StreamKind::kKv)) {
        m_error = "kv headers carry non-kv app.kind";
        SetError(error, m_error);
        return false;
    }
    m_headers = table;
    m_has_headers = true;
    const std::string mode = table.Get(kKvModeKey, "single");
    if (!ParseKvMode(mode, &m_mode)) {
        m_mode = KvMode::kStream;
    }
    return true;
}

bool KvAssembler::OnData(const std::uint8_t* data, std::size_t size,
                         std::string* error) {
    if (!m_has_headers) {
        m_error = "kv data before headers";
        SetError(error, m_error);
        return false;
    }
    if (data == nullptr && size != 0) {
        m_error = "kv data is null";
        SetError(error, m_error);
        return false;
    }
    if (m_buffer.size() + size > m_max_bytes) {
        m_error = "kv body exceeds max bytes";
        SetError(error, m_error);
        return false;
    }
    m_buffer.insert(m_buffer.end(), data, data + size);
    return true;
}

bool KvAssembler::Finish(KvTable* out, std::string* error) {
    if (out == nullptr) {
        m_error = "output kv table is null";
        SetError(error, m_error);
        return false;
    }
    if (!m_has_headers) {
        m_error = "kv stream ended without headers";
        SetError(error, m_error);
        return false;
    }
    KvTable merged = m_headers;
    if (!m_buffer.empty()) {
        KvTable body;
        std::string parse_error;
        if (!KvTable::ParseBinary(m_buffer.data(), m_buffer.size(), &body, &parse_error)) {
            m_error = "kv body parse failed: " + parse_error;
            SetError(error, m_error);
            return false;
        }
        for (const auto& entry : body.entries()) {
            merged.Set(entry.first, entry.second);
        }
    } else if (merged.Find(kKvPartialKey) != nullptr) {
        // partial 但没有 body：允许（空大值区），继续按 HEADERS 完成。
    }
    StripReserved(&merged);
    *out = std::move(merged);
    return true;
}

void KvAssembler::StripReserved(KvTable* table) {
    if (table == nullptr) {
        return;
    }
    std::vector<std::string> keys;
    keys.reserve(table->size());
    for (const auto& entry : table->entries()) {
        if (IsReservedKey(entry.first)) {
            keys.push_back(entry.first);
        }
    }
    for (const std::string& key : keys) {
        table->Erase(key);
    }
}

// ---------------------------------------------------------------------------
// KvBatchCollector
// ---------------------------------------------------------------------------

KvBatchCollector::KvBatchCollector(std::size_t expected) : m_expected(expected) {}

void KvBatchCollector::Add(std::uint32_t stream_id, KvTable table) {
    m_entries.emplace_back(stream_id, std::move(table));
}

KvTable KvBatchCollector::Merge() const {
    KvTable merged;
    for (const auto& entry : m_entries) {
        for (const auto& item : entry.second.entries()) {
            merged.Set(item.first, item.second);
        }
    }
    return merged;
}

}  // namespace go2cpp::app
