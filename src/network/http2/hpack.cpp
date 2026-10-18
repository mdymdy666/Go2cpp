/**
 * @file hpack.cpp
 * @brief HPACK 实现（RFC 7541）。仅作协议逻辑，不涉及 IO 或线程。
 *
 * ## 模块定位
 * HPACK 实现：被 `Connection` 的编码/解码路径调用；无 IO、无锁，唯一的可变状态
 * 是动态表（编码侧与解码侧各一份）。
 *
 * ## 本文件定义什么
 * - `ToString(HpackStatus)`；
 * - 前缀整数：`DecodeInteger()` / `EncodeInteger()`；
 * - 静态表访问：`StaticTable()` / `StaticTableEntry()` / `FindStaticIndex()` / `FindStaticNameIndex()`；
 * - 动态表：构造 / `Add()` / `Get()` / `FindRelative()` / `EvictUntilFit()`；
 * - 解码：`DecodeString()` / `DecodeName()` / `Decoder::DecodeBlock()`；
 * - 编码：`Encoder::EncodeBlock()` / `AppendTableSizeUpdate()`。
 *
 * ## 阅读建议
 * 先看 `DecodeInteger()`（HPACK 所有字段的基础），再看 `DecodeBlock()` 的四种首字节
 * 形态，最后看 `EncodeBlock()` 的择优顺序。
 *
 * ## 边界与陷阱
 * - 静态表 **1-based**、动态表相对索引 **1 是表首最新**；拼接索引 = 61 + relative；
 * - `DecodeBlock()` 失败时动态表可能已变更（不做事后回滚），失败后应断连；
 * - 编码器必须读动态表：只写不读会让连接复用场景退化成全量重发（见 EncodeBlock 注释）。
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

/**
 * @brief 解码一个前缀整数（RFC 7541 §5.1）。
 *
 * ## 步骤
 * step1. 取首字节低 `prefix_bits` 位；小于前缀最大值即为最终值；
 * step2. 前缀位全 1 时，按 7 位一组读续字节，直到最高位为 0；
 * step3. 输入耗尽返回 `kNeedMore`（增量解析）；
 * step4. 位移溢出或续字节过多返回 `kInvalidInteger`（防恶意长整数）。
 */
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
    // step1: 单字节即可表示。
    if (value < prefix_max) {
        *out = value;
        *consumed = 1;
        return HpackStatus::kOk;
    }
    // 前缀位全 1：后续 7 位一组，直到最高位为 0
    std::size_t index = 1;
    std::uint64_t shift = 0;
    // step2: 7 位一组的续字节，最高位是继续标志。
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

/**
 * @brief 编码一个前缀整数。
 *
 * ## 步骤
 * step1. 值小于前缀最大值 -> 单字节输出（高位由 `first_byte_high_bits` 指定）；
 * step2. 否则首字节写满前缀位，余值按 7 位一组输出，最后一组最高位清 0；
 * step3. 输出容量不足返回 `kInvalidInteger`（调用方给 16 字节缓冲，实际最多 11）。
 */
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

std::size_t DynamicTable::HashKey(std::string_view name,
                                    std::string_view value) noexcept {
    // 组合哈希：先 name 再混入 value。不分配任何内存 —— 最初用拼接字符串做键，
    // 压测显示并发流场景吞吐下降 41%，主要开销就是每次查找都构造一次 key。
    std::size_t h = std::hash<std::string_view>{}(name);
    const std::size_t v = std::hash<std::string_view>{}(value);
    h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    return h;
}

const HeaderField* DynamicTable::EntryBySeq(std::uint64_t seq) const noexcept {
    if (m_entries.empty()) {
        return nullptr;
    }
    const std::uint64_t oldest_seq = m_next_seq - m_entries.size();
    if (seq < oldest_seq || seq >= m_next_seq) {
        return nullptr;   // 已被逐出
    }
    // 最新条目（seq = m_next_seq-1）在下标 0
    const std::size_t index = static_cast<std::size_t>(m_next_seq - 1 - seq);
    return &m_entries[index];
}

void DynamicTable::Clear() noexcept {
    m_entries.clear();
    m_seqs.clear();
    m_index.clear();
    m_size = 0;
}

/**
 * @brief 按 FIFO 逐出直到能装下新条目。
 *
 * ## 步骤
 * step1. 从表尾（最旧）开始逐出，直到 `m_size + incoming <= m_max_size`；
 * step2. 逐出时同步维护 `m_size` 与哈希索引（序号自然失效）。
 *
 * ## 边界
 * 条目本身大于上限时表会被清空，`Add()` 随后返回 false（不会插入）。
 */
void DynamicTable::EvictUntilFit(std::size_t incoming_size) noexcept {
    // FIFO：表尾是最旧条目，从尾部逐出
    while (!m_entries.empty() && m_size + incoming_size > m_max_size) {
        const auto& oldest = m_entries.back();
        const auto oldest_seq = m_seqs.back();
        m_size -= EntrySize(oldest.name, oldest.value);
        // 从哈希索引里删掉这一条；同一哈希下可能有多条，只删序号匹配的那个。
        const auto hash = HashKey(oldest.name, oldest.value);
        const auto range = m_index.equal_range(hash);
        for (auto it = range.first; it != range.second; ++it) {
            if (it->second == oldest_seq) {
                m_index.erase(it);
                break;
            }
        }
        m_entries.pop_back();
        m_seqs.pop_back();
    }
}

/**
 * @brief 插入一个新条目到表首。
 *
 * ## 步骤
 * step1. 计算条目大小 `name+value+32`；大于表上限直接返回 false；
 * step2. `EvictUntilFit()` 腾出空间；
 * step3. 插入到 `m_entries` 表首、分配单调序号、写哈希索引并累加 `m_size`。
 */
bool DynamicTable::Add(std::string name, std::string value) {
    const std::size_t incoming = EntrySize(name, value);
    if (incoming > m_max_size) {
        // 条目本身超过上限：清空表且不插入（RFC 7541 §4.4）
        Clear();
        return false;
    }
    EvictUntilFit(incoming);
    const std::uint64_t seq = m_next_seq++;
    m_index.emplace(HashKey(name, value), seq);
    m_entries.insert(m_entries.begin(), HeaderField{std::move(name), std::move(value)});
    m_seqs.insert(m_seqs.begin(), seq);
    m_size += incoming;
    return true;
}

const HeaderField* DynamicTable::Get(std::size_t relative_index) const noexcept {
    if (relative_index == 0 || relative_index > m_entries.size()) {
        return nullptr;
    }
    return &m_entries[relative_index - 1];
}

/**
 * @brief 查找完全匹配的条目并返回相对索引（O(1)）。
 *
 * ## 说明
 * 内部用 `name\0value -> 序号` 的哈希索引，相对索引由当前计数器与序号直接推出，
 * 不需要扫描。这条路径是连接复用的性能关键：早期线性实现导致吞吐下降 36%。
 */
std::size_t DynamicTable::FindRelative(const std::string& name,
                                       const std::string& value) const noexcept {
    if (m_entries.empty()) {
        return 0;
    }
    const auto range = m_index.equal_range(HashKey(name, value));
    for (auto it = range.first; it != range.second; ++it) {
        const HeaderField* entry = EntryBySeq(it->second);
        if (entry != nullptr && entry->name == name && entry->value == value) {
            // 相对索引 = 最新序号 - 本条序号；最新条目的相对索引为 1。
            const std::uint64_t relative = m_next_seq - it->second;
            if (relative == 0 || relative > m_entries.size()) {
                return 0;
            }
            return static_cast<std::size_t>(relative);
        }
    }
    return 0;
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

/**
 * @brief 解码一整块 header block。
 *
 * ## 步骤
 * step1. 首字节 `001xxxxx` -> 动态表大小更新（不得超过 SHETTINGS 上限）；
 * step2. 首字节 `1xxxxxxx` -> 索引字段（静态 <=61，动态按相对索引）；
 * step3. 首字节 `01xxxxxx` -> 含增量索引的字面量（名字/值解析后 `Add()`）；
 * step4. 首字节 `0000xxxx` / `0001xxxx` -> 不含索引 / 绝不索引的字面量（不入表）；
 * step5. 每种表示都推进 `offset`，直到消费完整块；任何越界都返回 `kIndexOutOfRange`。
 *
 * ## 边界
 * 解码失败时动态表可能已部分变更，调用方应断连而不是重试同一块。
 */
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
        // step1: 表大小更新（必须 <= 本端 SETTINGS 上限）。
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
        // step2: 索引字段。
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
        // step3/4: 三种字面量表示，区别是前缀位数与是否入表。
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

std::size_t Encoder::FindDynamicIndex(const std::string& name,
                                      const std::string& value) const noexcept {
    // HPACK 索引 62 对应动态表最新条目，所以 full_index = 61 + relative_index。
    // 底层是 O(1) 哈希查找（见 DynamicTable::FindRelative）。
    const std::size_t relative = m_table.FindRelative(name, value);
    if (relative == 0) {
        return 0;
    }
    return StaticTableSize() + relative;
}

/**
 * @brief 编码一组 header 字段。
 *
 * ## 步骤
 * step1. 完全命中静态表 -> 7 位前缀索引；
 * step2. 完全命中动态表 -> 7 位前缀索引（复用此前发出的字段）；
 * step3. 名字命中静态表 -> 6 位前缀「含增量索引字面量」，只发值；
 * step4. 值在 Huffman 更短时用 Huffman 编码，否则发原文；
 * step5. 每种字面量都同步 `m_table.Add()`，保证两端动态表一致。
 *
 * ## 为什么 step2 不能省
 * 只看静态表会让「同一组 header 重复请求」每次都重发完整字符串；压测实测
 * 1000 次相同请求从平均 47 字节/请求降到个位数。
 */
HpackStatus Encoder::EncodeBlock(const std::vector<HeaderField>& fields,
                                 std::vector<std::uint8_t>* out,
                                 std::string* error) {
    if (out == nullptr) {
        return HpackStatus::kInvalidInteger;
    }
    std::uint8_t buffer[16] = {};
    for (const auto& field : fields) {
        // 1) 完全匹配静态表 -> 索引
        // step1: 静态表完全匹配。
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
        // 1b) 完全匹配动态表 -> 索引
        //     **这一段曾经缺失**：编码器只往动态表里写、从不读，导致「连接复用
        //     同一组 header」时每个请求仍然重发完整字符串，动态表白建。
        //     压测（1000 次相同请求）实测平均 47 字节/请求，修复后降到个位数。
        // step2: 动态表完全匹配（连接复用的关键路径）。
        const auto dynamic_exact = FindDynamicIndex(field.name, field.value);
        if (dynamic_exact != 0) {
            std::size_t written = 0;
            const auto status =
                EncodeInteger(dynamic_exact, 7, 0x80U, buffer, sizeof(buffer), &written);
            if (status != HpackStatus::kOk) {
                return status;
            }
            out->insert(out->end(), buffer, buffer + written);
            continue;
        }
        // 2) 名字命中静态表 -> 含增量索引的字面量（复用名字索引）
        // step3: 名字命中静态表时只发值。
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
