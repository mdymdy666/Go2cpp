/**
 * @file hpack.cpp
 * @brief HPACK 实现（RFC 7541）。仅作协议逻辑，不涉及 IO 或线程。
 */
#include "go2cpp/network/http2/hpack.hpp"
#include "go2cpp/network/http2/huffman.hpp"

#include <array>
#include <cstring>

namespace go2cpp::net::http2 {

const char* ToString(HpackStatus status) noexcept {
    switch (status) {
        case HpackStatus::kOk: return "ok";
        case HpackStatus::kNeedMore: return "need-more";
        case HpackStatus::kIndexOutOfRange: return "index-out-of-range";
        case HpackStatus::kInvalidInteger: return "invalid-integer";
        case HpackStatus::kTruncatedString: return "truncated-string";
        case HpackStatus::kHuffmanUnsupported: return "huffman-unsupported";
        case HpackStatus::kHuffmanInvalid: return "huffman-invalid";
        case HpackStatus::kInvalidTableSize: return "invalid-table-size";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// 整数（RFC 7541 §5.1）
// ---------------------------------------------------------------------------

HpackStatus DecodeInteger(const std::uint8_t* in, std::size_t length,
                          unsigned prefix_bits, std::uint64_t* out,
                          std::size_t* consumed) noexcept {
    if (in == nullptr || out == nullptr || consumed == nullptr ||
        prefix_bits == 0 || prefix_bits > 8) {
        return HpackStatus::kInvalidInteger;
    }
    const std::uint64_t prefix_max = (prefix_bits >= 8)
                                         ? 0xFFULL
                                         : ((1ULL << prefix_bits) - 1ULL);
    std::uint64_t value = static_cast<std::uint64_t>(in[0]) & prefix_max;
    if (value < prefix_max) {
        *out = value;
        *consumed = 1;
        return HpackStatus::kOk;
    }
    // 前缀位全 1：后续 7 位一组，直到最高位为 0
    std::size_t index = 1;
    std::uint64_t shift = 0;
    for (;;) {
        if (index >= length) {
            return HpackStatus::kNeedMore;
        }
        const std::uint8_t byte = in[index++];
        // 防止 shift 溢出（最多 64 位）
        if (shift >= 63 && (byte & 0x7F) != 0) {
            return HpackStatus::kInvalidInteger;
        }
        value += static_cast<std::uint64_t>(byte & 0x7F) << shift;
        shift += 7;
        if ((byte & 0x80) == 0) {
            break;
        }
        if (index > length + 10) {
            return HpackStatus::kInvalidInteger;
        }
    }
    *out = value;
    *consumed = index;
    return HpackStatus::kOk;
}

HpackStatus EncodeInteger(std::uint64_t value, unsigned prefix_bits,
                          std::uint8_t first_byte_high_bits, std::uint8_t* out,
                          std::size_t out_length, std::size_t* written) noexcept {
    if (out == nullptr || written == nullptr || prefix_bits == 0 || prefix_bits > 8) {
        return HpackStatus::kInvalidInteger;
    }
    const std::uint64_t prefix_max = (prefix_bits >= 8)
                                         ? 0xFFULL
                                         : ((1ULL << prefix_bits) - 1ULL);
    std::size_t index = 0;
    if (value < prefix_max) {
        if (out_length < 1) {
            return HpackStatus::kNeedMore;
        }
        out[index++] = static_cast<std::uint8_t>(first_byte_high_bits | value);
        *written = index;
        return HpackStatus::kOk;
    }
    if (out_length < 1) {
        return HpackStatus::kNeedMore;
    }
    out[index++] = static_cast<std::uint8_t>(first_byte_high_bits | prefix_max);
    std::uint64_t rest = value - prefix_max;
    while (rest >= 128) {
        if (index >= out_length) {
            return HpackStatus::kNeedMore;
        }
        out[index++] = static_cast<std::uint8_t>((rest % 128) + 128);
        rest /= 128;
    }
    if (index >= out_length) {
        return HpackStatus::kNeedMore;
    }
    out[index++] = static_cast<std::uint8_t>(rest);
    *written = index;
    return HpackStatus::kOk;
}

// ---------------------------------------------------------------------------
// 静态表（RFC 7541 附录 A，61 项）
// ---------------------------------------------------------------------------

namespace {

constexpr std::size_t kStaticCount = 61;

const std::array<HeaderField, kStaticCount + 1>& StaticTable() {
    static const std::array<HeaderField, kStaticCount + 1> table = {{
        {"", ""},  // 占位，索引 0 不使用
        {":authority", ""},
        {":method", "GET"},
        {":method", "POST"},
        {":path", "/"},
        {":path", "/index.html"},
        {":scheme", "http"},
        {":scheme", "https"},
        {":status", "200"},
        {":status", "204"},
        {":status", "206"},
        {":status", "304"},
        {":status", "400"},
        {":status", "404"},
        {":status", "500"},
        {"accept-charset", ""},
        {"accept-encoding", "gzip, deflate"},
        {"accept-language", ""},
        {"accept-ranges", ""},
        {"accept", ""},
        {"access-control-allow-origin", ""},
        {"age", ""},
        {"allow", ""},
        {"authorization", ""},
        {"cache-control", ""},
        {"content-disposition", ""},
        {"content-encoding", ""},
        {"content-language", ""},
        {"content-length", ""},
        {"content-location", ""},
        {"content-range", ""},
        {"content-type", ""},
        {"cookie", ""},
        {"date", ""},
        {"etag", ""},
        {"expect", ""},
        {"expires", ""},
        {"from", ""},
        {"host", ""},
        {"if-match", ""},
        {"if-modified-since", ""},
        {"if-none-match", ""},
        {"if-range", ""},
        {"if-unmodified-since", ""},
        {"last-modified", ""},
        {"link", ""},
        {"location", ""},
        {"max-forwards", ""},
        {"proxy-authenticate", ""},
        {"proxy-authorization", ""},
        {"range", ""},
        {"referer", ""},
        {"refresh", ""},
        {"retry-after", ""},
        {"server", ""},
        {"set-cookie", ""},
        {"strict-transport-security", ""},
        {"transfer-encoding", ""},
        {"user-agent", ""},
        {"vary", ""},
        {"via", ""},
        {"www-authenticate", ""},
    }};
    return table;
}

}  // namespace

std::size_t StaticTableSize() noexcept { return kStaticCount; }

const HeaderField* StaticTableEntry(std::size_t index) noexcept {
    if (index == 0 || index > kStaticCount) {
        return nullptr;
    }
    return &StaticTable()[index];
}

std::size_t FindStaticIndex(const std::string& name,
                            const std::string& value) noexcept {
    for (std::size_t i = 1; i <= kStaticCount; ++i) {
        const auto& e = StaticTable()[i];
        if (e.name == name && e.value == value) {
            return i;
        }
    }
    return 0;
}

std::size_t FindStaticNameIndex(const std::string& name) noexcept {
    for (std::size_t i = 1; i <= kStaticCount; ++i) {
        if (StaticTable()[i].name == name) {
            return i;
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// 动态表（RFC 7541 §2.3.2 / §4）
// ---------------------------------------------------------------------------

DynamicTable::DynamicTable(std::size_t max_size) noexcept : m_max_size(max_size) {}

std::size_t DynamicTable::EntrySize(const std::string& name,
                                    const std::string& value) noexcept {
    // RFC 7541 §4.1：每个条目的开销是 name + value + 32
    return name.size() + value.size() + 32;
}

void DynamicTable::SetMaxSize(std::size_t max_size) noexcept {
    m_max_size = max_size;
    EvictUntilFit(0);
}

void DynamicTable::Clear() noexcept {
    m_entries.clear();
    m_size = 0;
}

void DynamicTable::EvictUntilFit(std::size_t incoming_size) noexcept {
    // FIFO：表尾是最旧条目，从尾部逐出
    while (!m_entries.empty() && m_size + incoming_size > m_max_size) {
        m_size -= EntrySize(m_entries.back().name, m_entries.back().value);
        m_entries.pop_back();
    }
}

bool DynamicTable::Add(std::string name, std::string value) {
    const std::size_t incoming = EntrySize(name, value);
    if (incoming > m_max_size) {
        // 条目本身超过上限：清空表且不插入（RFC 7541 §4.4）
        m_entries.clear();
        m_size = 0;
        return false;
    }
    EvictUntilFit(incoming);
    m_entries.insert(m_entries.begin(), HeaderField{std::move(name), std::move(value)});
    m_size += incoming;
    return true;
}

const HeaderField* DynamicTable::Get(std::size_t relative_index) const noexcept {
    if (relative_index == 0 || relative_index > m_entries.size()) {
        return nullptr;
    }
    return &m_entries[relative_index - 1];
}

// ---------------------------------------------------------------------------
// 解码器
// ---------------------------------------------------------------------------

Decoder::Decoder(std::size_t max_table_size) noexcept
    : m_table(max_table_size), m_settings_max(max_table_size) {}

void Decoder::SetMaxTableSize(std::size_t max_size) noexcept {
    m_settings_max = max_size;
    m_table.SetMaxSize(max_size);
}

namespace {

/// 解析字符串字面量（§5.2）。当前不支持 Huffman。
HpackStatus DecodeString(const std::uint8_t* in, std::size_t length,
                         std::size_t consumed_prefix, std::string* out,
                         std::size_t* consumed, std::string* error) {
    if (consumed_prefix >= length) {
        return HpackStatus::kNeedMore;
    }
    const std::uint8_t first = in[consumed_prefix];
    const bool huffman = (first & 0x80U) != 0;
    std::uint64_t str_len = 0;
    std::size_t header_bytes = 0;
    const auto status = DecodeInteger(in + consumed_prefix, length - consumed_prefix, 7,
                                      &str_len, &header_bytes);
    if (status != HpackStatus::kOk) {
        return status;
    }
    const std::size_t total = consumed_prefix + header_bytes;
    if (str_len > length - total) {
        return HpackStatus::kTruncatedString;
    }
    if (huffman) {
        std::string decoded;
        const auto huffman_status =
            HuffmanDecode(in + total, static_cast<std::size_t>(str_len), &decoded);
        if (huffman_status != HuffmanStatus::kOk) {
            if (error != nullptr) {
                *error = "HPACK Huffman 字符串解码失败: ";
                *error += ToString(huffman_status);
            }
            return HpackStatus::kHuffmanInvalid;
        }
        if (decoded.size() != str_len) {
            // 解码后长度与声明的编码长度不同是正常现象（Huffman 是变长压缩），
            // 这里只保留原编码长度用于 consumed 计算，不做长度相等断言。
        }
        *out = std::move(decoded);
        *consumed = total + static_cast<std::size_t>(str_len);
        return HpackStatus::kOk;
    }
    out->assign(reinterpret_cast<const char*>(in + total), static_cast<std::size_t>(str_len));
    *consumed = total + static_cast<std::size_t>(str_len);
    return HpackStatus::kOk;
}

/// 解析「名字」：索引或字面量。
HpackStatus DecodeName(const std::uint8_t* in, std::size_t length,
                       std::size_t consumed_prefix, std::string* name,
                       std::size_t* consumed, const DynamicTable& table,
                       std::string* error) {
    if (consumed_prefix >= length) {
        return HpackStatus::kNeedMore;
    }
    std::uint64_t index = 0;
    std::size_t index_bytes = 0;
    const auto status = DecodeInteger(in + consumed_prefix, length - consumed_prefix, 7,
                                      &index, &index_bytes);
    if (status != HpackStatus::kOk) {
        return status;
    }
    const std::size_t after_index = consumed_prefix + index_bytes;
    if (index == 0) {
        // 字面量名字
        return DecodeString(in, length, after_index, name, consumed, error);
    }
    // 索引名字
    if (index <= StaticTableSize()) {
        *name = StaticTableEntry(static_cast<std::size_t>(index))->name;
        *consumed = after_index;
        return HpackStatus::kOk;
    }
    const auto* entry = table.Get(static_cast<std::size_t>(index) - StaticTableSize());
    if (entry == nullptr) {
        if (error != nullptr) {
            *error = "HPACK 名字索引越界: " + std::to_string(index);
        }
        return HpackStatus::kIndexOutOfRange;
    }
    *name = entry->name;
    *consumed = after_index;
    return HpackStatus::kOk;
}

}  // namespace

HpackStatus Decoder::DecodeBlock(const std::uint8_t* in, std::size_t length,
                                 std::vector<HeaderField>* out,
                                 std::string* error) {
    if (in == nullptr || out == nullptr) {
        return HpackStatus::kInvalidInteger;
    }
    std::size_t offset = 0;
    while (offset < length) {
        const std::uint8_t byte = in[offset];
        // §6.3 动态表大小更新：001xxxxx
        if ((byte & 0xE0U) == 0x20U) {
            std::uint64_t new_size = 0;
            std::size_t consumed = 0;
            const auto status = DecodeInteger(in + offset, length - offset, 5, &new_size,
                                              &consumed);
            if (status != HpackStatus::kOk) {
                return status;
            }
            if (new_size > m_settings_max) {
                if (error != nullptr) {
                    *error = "HPACK 表大小更新超过 SETTINGS 上限: " +
                             std::to_string(new_size) + " > " + std::to_string(m_settings_max);
                }
                return HpackStatus::kInvalidTableSize;
            }
            m_table.SetMaxSize(static_cast<std::size_t>(new_size));
            offset += consumed;
            continue;
        }
        // §6.1 索引字段：1xxxxxxx
        if ((byte & 0x80U) != 0) {
            std::uint64_t index = 0;
            std::size_t consumed = 0;
            const auto status = DecodeInteger(in + offset, length - offset, 7, &index,
                                              &consumed);
            if (status != HpackStatus::kOk) {
                return status;
            }
            if (index == 0) {
                return HpackStatus::kIndexOutOfRange;
            }
            if (index <= StaticTableSize()) {
                out->push_back(*StaticTableEntry(static_cast<std::size_t>(index)));
            } else {
                const auto* entry = m_table.Get(static_cast<std::size_t>(index) - StaticTableSize());
                if (entry == nullptr) {
                    if (error != nullptr) {
                        *error = "HPACK 索引越界: " + std::to_string(index);
                    }
                    return HpackStatus::kIndexOutOfRange;
                }
                out->push_back(*entry);
            }
            offset += consumed;
            continue;
        }
        // §6.2.1 含增量索引的字面量：01xxxxxx（前缀 6 位）
        // §6.2.2 不含索引的字面量：0000xxxx（前缀 4 位）
        // §6.2.3 绝不索引的字面量：0001xxxx（前缀 4 位）
        unsigned prefix_bits = 0;
        bool add_to_table = false;
        if ((byte & 0xC0U) == 0x40U) {
            prefix_bits = 6;
            add_to_table = true;
        } else if ((byte & 0xF0U) == 0x00U) {
            prefix_bits = 4;
        } else if ((byte & 0xF0U) == 0x10U) {
            prefix_bits = 4;
        } else {
            if (error != nullptr) {
                *error = "HPACK 无法识别的表示首字节: " + std::to_string(byte);
            }
            return HpackStatus::kInvalidInteger;
        }

        std::uint64_t name_index = 0;
        std::size_t name_index_bytes = 0;
        auto status = DecodeInteger(in + offset, length - offset, prefix_bits, &name_index,
                                    &name_index_bytes);
        if (status != HpackStatus::kOk) {
            return status;
        }
        std::size_t cursor = offset + name_index_bytes;
        std::string name;
        if (name_index == 0) {
            std::size_t name_consumed = 0;
            status = DecodeString(in, length, cursor, &name, &name_consumed, error);
            if (status != HpackStatus::kOk) {
                return status;
            }
            cursor = name_consumed;
        } else {
            if (name_index <= StaticTableSize()) {
                name = StaticTableEntry(static_cast<std::size_t>(name_index))->name;
            } else {
                const auto* entry =
                    m_table.Get(static_cast<std::size_t>(name_index) - StaticTableSize());
                if (entry == nullptr) {
                    if (error != nullptr) {
                        *error = "HPACK 名字索引越界: " + std::to_string(name_index);
                    }
                    return HpackStatus::kIndexOutOfRange;
                }
                name = entry->name;
            }
        }
        std::string value;
        std::size_t value_consumed = 0;
        status = DecodeString(in, length, cursor, &value, &value_consumed, error);
        if (status != HpackStatus::kOk) {
            return status;
        }
        if (add_to_table) {
            m_table.Add(name, value);
        }
        out->push_back(HeaderField{std::move(name), std::move(value)});
        offset = value_consumed;
    }
    return HpackStatus::kOk;
}

// ---------------------------------------------------------------------------
// 编码器
// ---------------------------------------------------------------------------

Encoder::Encoder(std::size_t max_table_size) noexcept
    : m_table(max_table_size), m_settings_max(max_table_size) {}

HpackStatus Encoder::AppendTableSizeUpdate(std::size_t new_size,
                                           std::vector<std::uint8_t>* out) {
    if (out == nullptr) {
        return HpackStatus::kInvalidTableSize;
    }
    if (new_size > m_settings_max) {
        return HpackStatus::kInvalidTableSize;
    }
    std::uint8_t buffer[16] = {};
    std::size_t written = 0;
    const auto status = EncodeInteger(new_size, 5, 0x20U, buffer, sizeof(buffer), &written);
    if (status != HpackStatus::kOk) {
        return status;
    }
    out->insert(out->end(), buffer, buffer + written);
    m_table.SetMaxSize(new_size);
    return HpackStatus::kOk;
}

HpackStatus Encoder::EncodeBlock(const std::vector<HeaderField>& fields,
                                 std::vector<std::uint8_t>* out,
                                 std::string* error) {
    if (out == nullptr) {
        return HpackStatus::kInvalidInteger;
    }
    std::uint8_t buffer[16] = {};
    for (const auto& field : fields) {
        // 1) 完全匹配静态表 -> 索引
        const auto exact = FindStaticIndex(field.name, field.value);
        if (exact != 0) {
            std::size_t written = 0;
            const auto status = EncodeInteger(exact, 7, 0x80U, buffer, sizeof(buffer), &written);
            if (status != HpackStatus::kOk) {
                return status;
            }
            out->insert(out->end(), buffer, buffer + written);
            continue;
        }
        // 2) 名字命中静态表 -> 含增量索引的字面量（复用名字索引）
        const auto name_index = FindStaticNameIndex(field.name);
        std::size_t written = 0;
        auto status = EncodeInteger(name_index, 6, 0x40U, buffer, sizeof(buffer), &written);
        if (status != HpackStatus::kOk) {
            return status;
        }
        out->insert(out->end(), buffer, buffer + written);
        if (name_index == 0) {
            // 名字也要字面量写入
            status = EncodeInteger(field.name.size(), 7, 0x00U, buffer, sizeof(buffer),
                                   &written);
            if (status != HpackStatus::kOk) {
                return status;
            }
            out->insert(out->end(), buffer, buffer + written);
            out->insert(out->end(), field.name.begin(), field.name.end());
        }
        // 值：编码端可选 Huffman（§5.2）。仅当结果不更长时才用，避免膨胀。
        std::vector<std::uint8_t> huffman_value;
        bool use_huffman = false;
        if (HuffmanEncode(field.value, &huffman_value) == HuffmanStatus::kOk) {
            use_huffman = huffman_value.size() < field.value.size();
        }
        const std::size_t value_len = use_huffman ? huffman_value.size() : field.value.size();
        const std::uint8_t value_prefix = use_huffman ? 0x80U : 0x00U;
        status = EncodeInteger(value_len, 7, value_prefix, buffer, sizeof(buffer), &written);
        if (status != HpackStatus::kOk) {
            return status;
        }
        out->insert(out->end(), buffer, buffer + written);
        if (use_huffman) {
            out->insert(out->end(), huffman_value.begin(), huffman_value.end());
        } else {
            out->insert(out->end(), field.value.begin(), field.value.end());
        }
        m_table.Add(field.name, field.value);
    }
    (void)error;
    return HpackStatus::kOk;
}

}  // namespace go2cpp::net::http2
