/**
 * @file deflate_reader.hpp
 * @brief 自研 DEFLATE（RFC 1951）解码器：stored / fixed / dynamic Huffman。
 *
 * @details 为什么解码器必须完整支持三种块类型，而编码器只做两种：
 *
 *   - 本模块输出的流由自己控制，固定 Huffman 已够用；
 *   - 但输入可能来自任何标准实现（Python `zlib`、GNU `gzip`、nginx），它们
 *     普遍使用动态 Huffman（BTYPE=10）。只支持 stored/fixed 会让
 *     `test_interop.cpp` 的「系统 gzip → 本模块」方向直接失败。
 *
 * 设计要点：
 *
 *   1. **完全增量**：输入 32 KiB 一块、输出任意大小；位读取器把「未消费的
 *      整字节」留在输入缓冲里，半字节放进 `m_bit_buffer`，因此任意位置都能
 *      暂停/恢复（ReaderEngine 的拉取模型要求如此）。
 *   2. **输出窗口 32 KiB 环形缓冲**：匹配复制可能跨越多次 `Inflate` 调用，
 *      窗口保证 `distance` 始终能取到已输出字节。
 *   3. **错误分类**：头错误 → `ErrHeader`，校验和 → `ErrChecksum`，
 *      预置字典 → `ErrDictionary`，其余格式错误 → `ErrCorrupt`；截断由
 *      上层 ReaderEngine 在源 EOF 时统一报 `ErrUnexpectedEof`。
 *   4. **动态 Huffman 表**：用 `1 << max_bits` 的查表数组解码，码长上限 15；
 *      建表时检查过订阅（over-subscribed）与空表。固定表在构造时建一次。
 *
 * 未实现项与原因：
 *   - 预置字典：公开 API 没有字典入口，遇到 FDICT 直接 `ErrDictionary`；
 *   - gzip 的 FHCRC 只跳过不校验：RFC 1952 允许，且没有调用方依赖它；
 *   - 多成员由上层 `ReaderEngine` 调 `ResetForNextMember()` 驱动。
 *
 * @note 该头文件只被 `inflater.cpp` 包含，不属于公开 API。
 */
#pragma once

#include "go2cpp/compress/errors.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

namespace go2cpp::compress::detail {

/// 一次解码调用的三分类，由 `inflater.cpp` 映射成公开的 `InflateStatus`。
enum class DecodeStatus : std::uint8_t {
    kOk,       ///< 继续；可能需要更多输入或输出空间。
    kMemberEnd,///< 当前 deflate 流 / gzip 成员完整结束。
    kError,    ///< 数据错误，具体原因见 `ErrorPtr` 输出参数。
};

/**
 * @brief 自研 DEFLATE 增量解码器。
 *
 * 生命周期：由 `detail::Inflater::Impl` 持有。`CommitInput` 只在
 * `input_available() == 0` 时调用（与 zlib 的 `avail_in == 0` 约定一致）。
 */
class InflateDecoder {
public:
    explicit InflateDecoder(int window_bits) {
        if (window_bits == -15) {
            m_format = Format::kRaw;
            m_ok = true;
        } else if (window_bits == 15) {
            m_format = Format::kZlib;
            m_ok = true;
        } else if (window_bits == 31) {
            m_format = Format::kGzip;
            m_ok = true;
        }
        BuildFixedTables();
        Reset();
    }

    bool ok() const noexcept { return m_ok && m_fixed_lit.max_bits > 0; }

    /// 复位到「等待新流头部」，清空输入与所有流状态。
    void Reset() {
        m_input = nullptr;
        m_input_size = 0;
        m_input_pos = 0;
        m_bit_buffer = 0;
        m_bits = 0;
        m_phase = Phase::kHeader;
        m_error.reset();
        ResetMemberState();
    }

    /// 结束当前成员、准备解析下一个 gzip 成员；保留未消费输入与位缓存。
    void ResetForNextMember() {
        m_phase = Phase::kHeader;
        m_error.reset();
        ResetMemberState();
    }

    std::size_t input_capacity() const noexcept { return m_input_capacity; }

    std::size_t input_available() const noexcept {
        return m_input_size - m_input_pos;
    }

    void SetInputCapacity(std::size_t capacity) noexcept { m_input_capacity = capacity; }

    /**
     * @brief 挂上新的输入块。
     * @note 前提：`input_available() == 0`；否则忽略，避免把新数据接在缓冲中部。
     */
    void CommitInput(const std::uint8_t* data, std::size_t size) {
        if (m_input_pos < m_input_size) {
            return;
        }
        m_input = data;
        m_input_size = size;
        m_input_pos = 0;
    }

    /**
     * @brief 解压到 `out`。
     * @param[out] produced 本次产出的字节数。
     * @param[out] error 失败时的错误；成功/结束时置空。
     */
    DecodeStatus Inflate(std::uint8_t* out, std::size_t out_size, std::size_t* produced,
                         ErrorPtr* error) {
        if (produced) {
            *produced = 0;
        }
        if (error) {
            *error = {};
        }
        if (!ok()) {
            if (error) {
                *error = ErrInternal();
            }
            return DecodeStatus::kError;
        }
        if (m_phase == Phase::kFailed) {
            if (error) {
                *error = m_error ? m_error : ErrCorrupt();
            }
            return DecodeStatus::kError;
        }
        if (m_phase == Phase::kDone) {
            return DecodeStatus::kMemberEnd;
        }
        std::size_t out_pos = 0;
        for (;;) {
            const Step step = StepOnce(out, out_size, &out_pos);
            if (step == Step::kContinue) {
                continue;
            }
            if (step == Step::kError) {
                if (error) {
                    *error = m_error ? m_error : ErrCorrupt();
                }
                if (produced) {
                    *produced = out_pos;
                }
                return DecodeStatus::kError;
            }
            if (step == Step::kMemberEnd) {
                m_phase = Phase::kDone;
                if (produced) {
                    *produced = out_pos;
                }
                return DecodeStatus::kMemberEnd;
            }
            // kNeedInput / kNeedOutput 对上层都是「先返回已产出数据」。
            if (produced) {
                *produced = out_pos;
            }
            return DecodeStatus::kOk;
        }
    }

private:
    enum class Format : std::uint8_t { kRaw, kZlib, kGzip };
    enum class Phase : std::uint8_t {
        kHeader,
        kBlockHeader,
        kStored,
        kHuffman,
        kDynamicHeader,
        kTrailer,
        kDone,
        kFailed,
    };
    enum class BlockKind : std::uint8_t { kStored, kFixed, kDynamic };
    enum class Step : std::uint8_t {
        kContinue,
        kNeedInput,
        kNeedOutput,
        kMemberEnd,
        kError,
    };
    enum class DecodeResult : std::uint8_t { kOk, kNeedInput, kCorrupt };
    enum class GzState : std::uint8_t {
        kFixed,
        kExtraLength,
        kExtraData,
        kName,
        kComment,
        kHcrc,
        kDone,
    };
    enum class DynState : std::uint8_t { kCounts, kClen, kLengths, kRepeat };
    enum class HuffStage : std::uint8_t {
        kSymbol,
        kLengthExtra,
        kDistanceSymbol,
        kDistanceExtra,
    };

    struct HuffmanEntry {
        std::uint16_t symbol{0};
        std::uint8_t bits{0};
    };

    struct HuffmanTable {
        std::vector<HuffmanEntry> entries;
        int max_bits{0};
    };

    static constexpr std::size_t kWindowSize = 32U * 1024U;
    static constexpr std::size_t kWindowMask = kWindowSize - 1U;
    static constexpr std::size_t kMaxHeaderString = 1U << 20;

    static constexpr int kLengthBase[29] = {
        3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27,
        31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static constexpr int kLengthExtra[29] = {
        0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
        2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static constexpr int kDistanceBase[30] = {
        1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
        193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
        6145, 8193, 12289, 16385, 24577};
    static constexpr int kDistanceExtra[30] = {
        0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
        6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
    static constexpr std::uint8_t kCodeLengthOrder[19] = {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

    // ---------------------------------------------------------------------
    // 位读取
    // ---------------------------------------------------------------------

    bool EnsureBits(int count) {
        while (m_bits < count) {
            if (m_input_pos >= m_input_size) {
                return false;
            }
            m_bit_buffer |= static_cast<std::uint64_t>(m_input[m_input_pos++]) << m_bits;
            m_bits += 8;
        }
        return true;
    }

    bool ReadBits(int count, std::uint32_t* value) {
        if (!EnsureBits(count)) {
            return false;
        }
        const std::uint32_t mask = (std::uint32_t{1} << count) - 1U;
        *value = static_cast<std::uint32_t>(m_bit_buffer) & mask;
        m_bit_buffer >>= count;
        m_bits -= count;
        return true;
    }

    void DropBits(int count) {
        if (count <= 0) {
            return;
        }
        m_bit_buffer >>= count;
        m_bits -= count;
    }

    void AlignToByte() {
        const int drop = m_bits & 7;
        if (drop != 0) {
            DropBits(drop);
        }
    }

    bool ReadByte(std::uint8_t* byte) {
        std::uint32_t value = 0;
        if (!ReadBits(8, &value)) {
            return false;
        }
        *byte = static_cast<std::uint8_t>(value);
        return true;
    }

    // ---------------------------------------------------------------------
    // Huffman 建表 / 解码
    // ---------------------------------------------------------------------

    static std::uint32_t ReverseBits(std::uint32_t code, int bits) noexcept {
        std::uint32_t reversed = 0;
        for (int index = 0; index < bits; ++index) {
            reversed = (reversed << 1U) | (code & 1U);
            code >>= 1U;
        }
        return reversed;
    }

    static bool BuildHuffman(const std::uint8_t* lengths, std::size_t count,
                             bool allow_empty, HuffmanTable* table) {
        int counts[16] = {};
        int max_bits = 0;
        for (std::size_t index = 0; index < count; ++index) {
            const int length = lengths[index];
            if (length > 15) {
                return false;
            }
            if (length > 0) {
                ++counts[length];
                max_bits = std::max(max_bits, length);
            }
        }
        if (max_bits == 0) {
            table->entries.clear();
            table->max_bits = 0;
            return allow_empty;
        }
        // 过订阅检查：每层剩余码字不能为负。
        int left = 1;
        for (int bits = 1; bits <= max_bits; ++bits) {
            left <<= 1;
            left -= counts[bits];
            if (left < 0) {
                return false;
            }
        }
        int next_code[16] = {};
        int code = 0;
        for (int bits = 1; bits <= max_bits; ++bits) {
            code = (code + counts[bits - 1]) << 1;
            next_code[bits] = code;
        }
        const std::size_t table_size = std::size_t{1} << max_bits;
        table->entries.assign(table_size, HuffmanEntry{});
        for (std::size_t symbol = 0; symbol < count; ++symbol) {
            const int length = lengths[symbol];
            if (length == 0) {
                continue;
            }
            const int canonical = next_code[length]++;
            if (canonical >= (1 << length)) {
                return false;
            }
            const std::uint32_t reversed =
                ReverseBits(static_cast<std::uint32_t>(canonical), length);
            for (std::size_t index = reversed; index < table_size;
                 index += std::size_t{1} << length) {
                table->entries[index] = HuffmanEntry{static_cast<std::uint16_t>(symbol),
                                                     static_cast<std::uint8_t>(length)};
            }
        }
        table->max_bits = max_bits;
        return true;
    }

    DecodeResult DecodeSymbol(const HuffmanTable& table, std::uint16_t* symbol) {
        if (table.max_bits == 0 || table.entries.empty()) {
            return DecodeResult::kCorrupt;
        }
        const std::uint32_t mask = (std::uint32_t{1} << table.max_bits) - 1U;
        if (!EnsureBits(table.max_bits)) {
            // 输入暂时不足：用高位补零查表；只有「码长 <= 已有位数」才可判定。
            const std::uint32_t index =
                static_cast<std::uint32_t>(m_bit_buffer) & mask;
            const HuffmanEntry& entry = table.entries[index];
            if (entry.bits == 0) {
                return DecodeResult::kNeedInput;
            }
            if (entry.bits > m_bits) {
                return DecodeResult::kNeedInput;
            }
            m_bit_buffer >>= entry.bits;
            m_bits -= entry.bits;
            *symbol = entry.symbol;
            return DecodeResult::kOk;
        }
        const std::uint32_t index = static_cast<std::uint32_t>(m_bit_buffer) & mask;
        const HuffmanEntry& entry = table.entries[index];
        if (entry.bits == 0) {
            return DecodeResult::kCorrupt;
        }
        m_bit_buffer >>= entry.bits;
        m_bits -= entry.bits;
        *symbol = entry.symbol;
        return DecodeResult::kOk;
    }

    void BuildFixedTables() {
        std::uint8_t lit_lengths[288] = {};
        for (int symbol = 0; symbol <= 143; ++symbol) {
            lit_lengths[symbol] = 8;
        }
        for (int symbol = 144; symbol <= 255; ++symbol) {
            lit_lengths[symbol] = 9;
        }
        for (int symbol = 256; symbol <= 279; ++symbol) {
            lit_lengths[symbol] = 7;
        }
        for (int symbol = 280; symbol <= 287; ++symbol) {
            lit_lengths[symbol] = 8;
        }
        BuildHuffman(lit_lengths, 288, false, &m_fixed_lit);
        std::uint8_t dist_lengths[32];
        std::memset(dist_lengths, 5, sizeof(dist_lengths));
        BuildHuffman(dist_lengths, 32, false, &m_fixed_dist);
    }

    // ---------------------------------------------------------------------
    // 状态机
    // ---------------------------------------------------------------------

    Step StepOnce(std::uint8_t* out, std::size_t out_size, std::size_t* out_pos) {
        switch (m_phase) {
            case Phase::kHeader:
                return StepHeader();
            case Phase::kBlockHeader:
                return StepBlockHeader();
            case Phase::kStored:
                return StepStored(out, out_size, out_pos);
            case Phase::kHuffman:
                return StepHuffman(out, out_size, out_pos);
            case Phase::kDynamicHeader:
                return StepDynamicHeader();
            case Phase::kTrailer:
                return StepTrailer();
            case Phase::kDone:
                return Step::kMemberEnd;
            case Phase::kFailed:
                return Step::kError;
        }
        return Step::kError;
    }

    Step Fail(ErrorPtr error) {
        m_error = std::move(error);
        m_phase = Phase::kFailed;
        return Step::kError;
    }

    Step StepHeader() {
        if (m_format == Format::kRaw) {
            m_phase = Phase::kBlockHeader;
            return Step::kContinue;
        }
        if (m_format == Format::kZlib) {
            while (m_header_fill < 2) {
                if (!ReadByte(&m_header_bytes[m_header_fill])) {
                    return Step::kNeedInput;
                }
                ++m_header_fill;
            }
            const std::uint32_t cmf = m_header_bytes[0];
            const std::uint32_t flg = m_header_bytes[1];
            if ((cmf & 0x0FU) != 8U || (cmf >> 4U) > 7U ||
                ((cmf << 8U) | flg) % 31U != 0U) {
                return Fail(ErrHeader());
            }
            if ((flg & 0x20U) != 0U) {
                return Fail(ErrDictionary());
            }
            m_phase = Phase::kBlockHeader;
            return Step::kContinue;
        }
        return StepGzipHeader();
    }

    Step StepGzipHeader() {
        for (;;) {
            switch (m_gz_state) {
                case GzState::kFixed:
                    while (m_header_fill < 10) {
                        if (!ReadByte(&m_header_bytes[m_header_fill])) {
                            return Step::kNeedInput;
                        }
                        ++m_header_fill;
                    }
                    if (m_header_bytes[0] != 0x1FU || m_header_bytes[1] != 0x8BU ||
                        m_header_bytes[2] != 8U) {
                        return Fail(ErrHeader());
                    }
                    m_gz_flags = m_header_bytes[3];
                    if ((m_gz_flags & 0xE0U) != 0U) {
                        return Fail(ErrHeader());
                    }
                    // FLG 的可选字段顺序：FEXTRA → FNAME → FCOMMENT → FHCRC。
                    m_gz_need_extra = (m_gz_flags & 0x04U) != 0U;
                    m_gz_need_name = (m_gz_flags & 0x08U) != 0U;
                    m_gz_need_comment = (m_gz_flags & 0x10U) != 0U;
                    m_gz_need_hcrc = (m_gz_flags & 0x02U) != 0U;
                    m_gz_state = GzState::kDone;
                    break;
                case GzState::kExtraLength: {
                    std::uint32_t value = 0;
                    if (!ReadBits(16, &value)) {
                        return Step::kNeedInput;
                    }
                    m_gz_skip = value;
                    m_gz_state = (m_gz_skip == 0) ? GzState::kDone : GzState::kExtraData;
                    break;
                }
                case GzState::kExtraData: {
                    std::uint8_t ignored = 0;
                    while (m_gz_skip > 0) {
                        if (!ReadByte(&ignored)) {
                            return Step::kNeedInput;
                        }
                        --m_gz_skip;
                    }
                    m_gz_state = GzState::kDone;
                    break;
                }
                case GzState::kName:
                case GzState::kComment: {
                    std::uint8_t byte = 0;
                    for (;;) {
                        if (!ReadByte(&byte)) {
                            return Step::kNeedInput;
                        }
                        if (byte == 0) {
                            break;
                        }
                        if (++m_gz_string_length > kMaxHeaderString) {
                            return Fail(ErrHeader());
                        }
                    }
                    m_gz_string_length = 0;
                    m_gz_state = GzState::kDone;
                    break;
                }
                case GzState::kHcrc: {
                    std::uint8_t byte = 0;
                    while (m_gz_hcrc_fill < 2) {
                        if (!ReadByte(&byte)) {
                            return Step::kNeedInput;
                        }
                        ++m_gz_hcrc_fill;
                    }
                    m_gz_state = GzState::kDone;
                    break;
                }
                case GzState::kDone:
                    AdvanceGzipOptional();
                    if (m_gz_state == GzState::kDone) {
                        m_phase = Phase::kBlockHeader;
                        return Step::kContinue;
                    }
                    break;
            }
        }
    }

    /// 按 FLG 选择下一个可选字段；没有可选字段时留在 kDone。
    void AdvanceGzipOptional() {
        if (m_gz_need_extra) {
            m_gz_need_extra = false;
            m_gz_state = GzState::kExtraLength;
            return;
        }
        if (m_gz_need_name) {
            m_gz_need_name = false;
            m_gz_state = GzState::kName;
            return;
        }
        if (m_gz_need_comment) {
            m_gz_need_comment = false;
            m_gz_state = GzState::kComment;
            return;
        }
        if (m_gz_need_hcrc) {
            m_gz_need_hcrc = false;
            m_gz_state = GzState::kHcrc;
            return;
        }
        m_gz_state = GzState::kDone;
    }

    Step StepBlockHeader() {
        if (m_block_stage == 0) {
            // BFINAL(1) + BTYPE(2) 一次读 3 位；不足时位读取器会把已取到的
            // 字节留在内部位缓存里，不会消耗半个字段。
            std::uint32_t header = 0;
            if (!ReadBits(3, &header)) {
                return Step::kNeedInput;
            }
            m_final_block = (header & 1U) != 0U;
            m_block_type = (header >> 1U) & 0x3U;
            m_block_stage = 1;
        }
        switch (m_block_type) {
            case 0: {
                if (m_stored_stage == 0) {
                    AlignToByte();
                    std::uint32_t length = 0;
                    if (!ReadBits(16, &length)) {
                        return Step::kNeedInput;
                    }
                    m_stored_len = length;
                    m_stored_stage = 1;
                }
                std::uint32_t complement = 0;
                if (!ReadBits(16, &complement)) {
                    return Step::kNeedInput;
                }
                if ((m_stored_len ^ complement) != 0xFFFFU) {
                    return Fail(ErrCorrupt());
                }
                m_stored_remaining = m_stored_len;
                m_stored_stage = 0;
                m_block_stage = 0;
                m_huff_stage = HuffStage::kSymbol;
                m_phase = Phase::kStored;
                return Step::kContinue;
            }
            case 1:
                m_lit_table = m_fixed_lit;
                m_dist_table = m_fixed_dist;
                m_huff_stage = HuffStage::kSymbol;
                m_block_stage = 0;
                m_phase = Phase::kHuffman;
                return Step::kContinue;
            case 2:
                ResetDynamicHeader();
                m_block_stage = 0;
                m_phase = Phase::kDynamicHeader;
                return Step::kContinue;
            default:
                return Fail(ErrCorrupt());
        }
    }

    Step StepStored(std::uint8_t* out, std::size_t out_size, std::size_t* out_pos) {
        while (m_stored_remaining > 0) {
            if (*out_pos >= out_size) {
                return Step::kNeedOutput;
            }
            std::uint32_t byte = 0;
            if (!ReadBits(8, &byte)) {
                return Step::kNeedInput;
            }
            EmitByte(static_cast<std::uint8_t>(byte), out, out_pos);
            --m_stored_remaining;
        }
        return EndBlock();
    }

    /// 只看下一个 Huffman 符号、不消费；输出已满时用它判断是否恰好是 EOB。
    DecodeResult PeekSymbol(const HuffmanTable& table, std::uint16_t* symbol,
                            int* bits) {
        if (table.max_bits == 0 || table.entries.empty()) {
            return DecodeResult::kCorrupt;
        }
        const std::uint32_t mask = (std::uint32_t{1} << table.max_bits) - 1U;
        const bool enough = EnsureBits(table.max_bits);
        const std::uint32_t index = static_cast<std::uint32_t>(m_bit_buffer) & mask;
        const HuffmanEntry& entry = table.entries[index];
        if (entry.bits == 0) {
            return enough ? DecodeResult::kCorrupt : DecodeResult::kNeedInput;
        }
        if (entry.bits > m_bits) {
            return DecodeResult::kNeedInput;
        }
        *symbol = entry.symbol;
        *bits = entry.bits;
        return DecodeResult::kOk;
    }

    Step StepHuffman(std::uint8_t* out, std::size_t out_size, std::size_t* out_pos) {
        for (;;) {
            if (m_match_remaining > 0) {
                if (*out_pos >= out_size) {
                    return Step::kNeedOutput;
                }
                EmitByte(ByteAtDistance(m_match_distance), out, out_pos);
                --m_match_remaining;
                continue;
            }
            if (*out_pos >= out_size) {
                // 输出恰好填满时仍要处理「块结束」：EOB 不产生输出，若不在这里
                // 消费它，ReaderEngine 会误以为还需要输入而报截断。
                std::uint16_t symbol = 0;
                int bits = 0;
                const DecodeResult result = PeekSymbol(m_lit_table, &symbol, &bits);
                if (result == DecodeResult::kOk && symbol == 256) {
                    DropBits(bits);
                    // 必须返回给 StepOnce 重新按 phase 分派；留在本循环里会
                    // 继续用旧 Huffman 表解下一个块头。
                    return EndBlock();
                }
                if (result == DecodeResult::kCorrupt) {
                    return Fail(ErrCorrupt());
                }
                return Step::kNeedOutput;
            }
            switch (m_huff_stage) {
                case HuffStage::kSymbol: {
                    std::uint16_t symbol = 0;
                    const DecodeResult result = DecodeSymbol(m_lit_table, &symbol);
                    if (result == DecodeResult::kNeedInput) {
                        return Step::kNeedInput;
                    }
                    if (result == DecodeResult::kCorrupt) {
                        return Fail(ErrCorrupt());
                    }
                    if (symbol < 256) {
                        EmitByte(static_cast<std::uint8_t>(symbol), out, out_pos);
                        continue;
                    }
                    if (symbol == 256) {
                        // 同上：块结束必须交回 StepOnce，才能切到「下一块头 / 尾部」。
                        return EndBlock();
                    }
                    if (symbol > 285) {
                        return Fail(ErrCorrupt());
                    }
                    m_pending_length_symbol = symbol;
                    m_huff_stage = HuffStage::kLengthExtra;
                    [[fallthrough]];
                }
                case HuffStage::kLengthExtra: {
                    const int length_index =
                        static_cast<int>(m_pending_length_symbol) - 257;
                    if (length_index < 0 || length_index >= 29) {
                        return Fail(ErrCorrupt());
                    }
                    const int length_extra = kLengthExtra[length_index];
                    std::uint32_t length_value = 0;
                    if (length_extra > 0 &&
                        !ReadBits(length_extra, &length_value)) {
                        return Step::kNeedInput;
                    }
                    m_pending_length =
                        static_cast<std::size_t>(kLengthBase[length_index]) +
                        length_value;
                    if (m_pending_length < 3 || m_pending_length > 258) {
                        return Fail(ErrCorrupt());
                    }
                    m_huff_stage = HuffStage::kDistanceSymbol;
                    [[fallthrough]];
                }
                case HuffStage::kDistanceSymbol: {
                    std::uint16_t distance_symbol = 0;
                    const DecodeResult result =
                        DecodeSymbol(m_dist_table, &distance_symbol);
                    if (result == DecodeResult::kNeedInput) {
                        return Step::kNeedInput;
                    }
                    if (result == DecodeResult::kCorrupt || distance_symbol > 29) {
                        return Fail(ErrCorrupt());
                    }
                    m_pending_distance_symbol = distance_symbol;
                    m_huff_stage = HuffStage::kDistanceExtra;
                    [[fallthrough]];
                }
                case HuffStage::kDistanceExtra: {
                    const int distance_index =
                        static_cast<int>(m_pending_distance_symbol);
                    const int distance_extra = kDistanceExtra[distance_index];
                    std::uint32_t distance_value = 0;
                    if (distance_extra > 0 &&
                        !ReadBits(distance_extra, &distance_value)) {
                        return Step::kNeedInput;
                    }
                    const std::size_t distance =
                        static_cast<std::size_t>(kDistanceBase[distance_index]) +
                        distance_value;
                    if (distance == 0 || distance > kWindowSize ||
                        distance > m_member_out) {
                        return Fail(ErrCorrupt());
                    }
                    m_match_remaining = m_pending_length;
                    m_match_distance = distance;
                    m_huff_stage = HuffStage::kSymbol;
                    continue;
                }
            }
        }
    }

    Step StepDynamicHeader() {
        for (;;) {
            switch (m_dyn_state) {
                case DynState::kCounts: {
                    // HLIT(5) + HDIST(5) + HCLEN(4) 一次读 14 位；不足时位读取器
                    // 会把已取到的字节留在内部缓存里，不会消费半个字段。
                    std::uint32_t header = 0;
                    if (!ReadBits(14, &header)) {
                        return Step::kNeedInput;
                    }
                    m_hlit = static_cast<int>(header & 31U) + 257;
                    m_hdist = static_cast<int>((header >> 5U) & 31U) + 1;
                    m_hclen = static_cast<int>((header >> 10U) & 15U) + 4;
                    m_clen_lengths.fill(0);
                    m_clen_read = 0;
                    m_all_lengths.assign(static_cast<std::size_t>(m_hlit + m_hdist), 0);
                    m_length_index = 0;
                    m_prev_length = 0;
                    m_dyn_state = DynState::kClen;
                    break;
                }
                case DynState::kClen: {
                    while (m_clen_read < m_hclen) {
                        std::uint32_t length = 0;
                        if (!ReadBits(3, &length)) {
                            return Step::kNeedInput;
                        }
                        m_clen_lengths[kCodeLengthOrder[m_clen_read]] =
                            static_cast<std::uint8_t>(length);
                        ++m_clen_read;
                    }
                    if (!BuildHuffman(m_clen_lengths.data(), m_clen_lengths.size(), false,
                                      &m_clen_table)) {
                        return Fail(ErrCorrupt());
                    }
                    if (m_clen_table.max_bits == 0) {
                        return Fail(ErrCorrupt());
                    }
                    m_dyn_state = DynState::kLengths;
                    break;
                }
                case DynState::kLengths: {
                    if (m_length_index >= m_all_lengths.size()) {
                        return FinishDynamicTables();
                    }
                    std::uint16_t symbol = 0;
                    const DecodeResult result = DecodeSymbol(m_clen_table, &symbol);
                    if (result == DecodeResult::kNeedInput) {
                        return Step::kNeedInput;
                    }
                    if (result == DecodeResult::kCorrupt || symbol > 18) {
                        return Fail(ErrCorrupt());
                    }
                    if (symbol <= 15) {
                        m_all_lengths[m_length_index++] =
                            static_cast<std::uint8_t>(symbol);
                        m_prev_length = static_cast<std::uint8_t>(symbol);
                        continue;
                    }
                    // 16/17/18 是重复码：符号本身先存下来，额外位不足时下次
                    // 从 kRepeat 继续，避免回滚已消费的位。
                    m_repeat_symbol = symbol;
                    m_dyn_state = DynState::kRepeat;
                    [[fallthrough]];
                }
                case DynState::kRepeat: {
                    const std::uint16_t symbol = m_repeat_symbol;
                    std::uint32_t extra = 0;
                    int extra_bits = 0;
                    std::size_t repeat = 0;
                    std::uint8_t value = 0;
                    if (symbol == 16) {
                        extra_bits = 2;
                        if (m_length_index == 0) {
                            return Fail(ErrCorrupt());
                        }
                        value = m_prev_length;
                    } else if (symbol == 17) {
                        extra_bits = 3;
                        value = 0;
                    } else {
                        extra_bits = 7;
                        value = 0;
                    }
                    if (!ReadBits(extra_bits, &extra)) {
                        return Step::kNeedInput;
                    }
                    if (symbol == 16) {
                        repeat = 3 + extra;
                    } else if (symbol == 17) {
                        repeat = 3 + extra;
                    } else {
                        repeat = 11 + extra;
                    }
                    if (m_length_index + repeat > m_all_lengths.size()) {
                        return Fail(ErrCorrupt());
                    }
                    for (std::size_t index = 0; index < repeat; ++index) {
                        m_all_lengths[m_length_index++] = value;
                    }
                    m_prev_length = value;
                    m_dyn_state = DynState::kLengths;
                    continue;
                }
            }
        }
    }

    Step FinishDynamicTables() {
        if (!BuildHuffman(m_all_lengths.data(), static_cast<std::size_t>(m_hlit),
                          false, &m_lit_table)) {
            return Fail(ErrCorrupt());
        }
        if (m_lit_table.max_bits == 0 || m_all_lengths[256] == 0) {
            return Fail(ErrCorrupt());
        }
        if (!BuildHuffman(m_all_lengths.data() + m_hlit,
                          static_cast<std::size_t>(m_hdist), true,
                          &m_dist_table)) {
            return Fail(ErrCorrupt());
        }
        m_huff_stage = HuffStage::kSymbol;
        m_phase = Phase::kHuffman;
        return Step::kContinue;
    }

    Step EndBlock() {
        if (!m_final_block) {
            m_phase = Phase::kBlockHeader;
            return Step::kContinue;
        }
        if (m_format == Format::kRaw) {
            m_phase = Phase::kDone;
            return Step::kMemberEnd;
        }
        AlignToByte();
        m_trailer_fill = 0;
        m_phase = Phase::kTrailer;
        return Step::kContinue;
    }

    Step StepTrailer() {
        const std::size_t need = (m_format == Format::kZlib) ? 4U : 8U;
        while (m_trailer_fill < need) {
            std::uint8_t byte = 0;
            if (!ReadByte(&byte)) {
                return Step::kNeedInput;
            }
            m_trailer[m_trailer_fill++] = byte;
        }
        if (m_format == Format::kZlib) {
            FinishAdler();
            const std::uint32_t expected =
                (static_cast<std::uint32_t>(m_trailer[0]) << 24U) |
                (static_cast<std::uint32_t>(m_trailer[1]) << 16U) |
                (static_cast<std::uint32_t>(m_trailer[2]) << 8U) |
                static_cast<std::uint32_t>(m_trailer[3]);
            const std::uint32_t actual = (m_adler_b << 16U) | m_adler_a;
            if (expected != actual) {
                return Fail(ErrChecksum());
            }
        } else {
            const std::uint32_t expected_crc =
                static_cast<std::uint32_t>(m_trailer[0]) |
                (static_cast<std::uint32_t>(m_trailer[1]) << 8U) |
                (static_cast<std::uint32_t>(m_trailer[2]) << 16U) |
                (static_cast<std::uint32_t>(m_trailer[3]) << 24U);
            const std::uint32_t expected_size =
                static_cast<std::uint32_t>(m_trailer[4]) |
                (static_cast<std::uint32_t>(m_trailer[5]) << 8U) |
                (static_cast<std::uint32_t>(m_trailer[6]) << 16U) |
                (static_cast<std::uint32_t>(m_trailer[7]) << 24U);
            const std::uint32_t actual_crc = m_crc ^ 0xFFFFFFFFU;
            const std::uint32_t actual_size =
                static_cast<std::uint32_t>(m_member_out & 0xFFFFFFFFU);
            if (expected_crc != actual_crc || expected_size != actual_size) {
                return Fail(ErrChecksum());
            }
        }
        m_phase = Phase::kDone;
        return Step::kMemberEnd;
    }

    // ---------------------------------------------------------------------
    // 输出 / 校验和
    // ---------------------------------------------------------------------

    void EmitByte(std::uint8_t byte, std::uint8_t* out, std::size_t* out_pos) {
        out[(*out_pos)++] = byte;
        m_window[m_window_pos] = byte;
        m_window_pos = (m_window_pos + 1U) & kWindowMask;
        ++m_member_out;
        UpdateChecksum(byte);
    }

    std::uint8_t ByteAtDistance(std::size_t distance) const noexcept {
        return m_window[(m_window_pos - distance) & kWindowMask];
    }

    void UpdateChecksum(std::uint8_t byte) noexcept {
        m_adler_a += byte;
        m_adler_b += m_adler_a;
        if (++m_adler_count >= 5552U) {
            FinishAdler();
        }
        m_crc = CrcTable()[(m_crc ^ byte) & 0xFFU] ^ (m_crc >> 8U);
    }

    void FinishAdler() noexcept {
        m_adler_a %= 65521U;
        m_adler_b %= 65521U;
        m_adler_count = 0;
    }

    static const std::array<std::uint32_t, 256>& CrcTable() {
        static const std::array<std::uint32_t, 256> table = [] {
            std::array<std::uint32_t, 256> result{};
            for (std::uint32_t index = 0; index < 256; ++index) {
                std::uint32_t value = index;
                for (int bit = 0; bit < 8; ++bit) {
                    value = (value & 1U) ? (0xEDB88320U ^ (value >> 1U)) : (value >> 1U);
                }
                result[index] = value;
            }
            return result;
        }();
        return table;
    }

    void ResetDynamicHeader() {
        m_dyn_state = DynState::kCounts;
        m_hlit = 0;
        m_hdist = 0;
        m_hclen = 0;
        m_clen_read = 0;
        m_clen_lengths.fill(0);
        m_all_lengths.clear();
        m_length_index = 0;
        m_prev_length = 0;
        m_clen_table = HuffmanTable{};
        m_repeat_symbol = 0;
    }

    void ResetMemberState() {
        m_header_fill = 0;
        m_gz_state = GzState::kFixed;
        m_gz_flags = 0;
        m_gz_skip = 0;
        m_gz_string_length = 0;
        m_gz_hcrc_fill = 0;
        m_gz_need_extra = false;
        m_gz_need_name = false;
        m_gz_need_comment = false;
        m_gz_need_hcrc = false;
        m_dyn_state = DynState::kCounts;
        m_hlit = 0;
        m_hdist = 0;
        m_hclen = 0;
        m_clen_read = 0;
        m_clen_lengths.fill(0);
        m_all_lengths.clear();
        m_length_index = 0;
        m_prev_length = 0;
        m_repeat_symbol = 0;
        m_clen_table = HuffmanTable{};
        m_final_block = false;
        m_block_stage = 0;
        m_block_type = 0;
        m_stored_stage = 0;
        m_stored_len = 0;
        m_stored_remaining = 0;
        m_huff_stage = HuffStage::kSymbol;
        m_pending_length_symbol = 0;
        m_pending_distance_symbol = 0;
        m_pending_length = 0;
        m_match_remaining = 0;
        m_match_distance = 0;
        m_trailer_fill = 0;
        m_adler_a = 1;
        m_adler_b = 0;
        m_adler_count = 0;
        m_crc = 0xFFFFFFFFU;
        m_member_out = 0;
        m_window_pos = 0;
    }

    // 格式与输入
    Format m_format{Format::kRaw};
    bool m_ok{false};
    const std::uint8_t* m_input{nullptr};
    std::size_t m_input_size{0};
    std::size_t m_input_pos{0};
    std::size_t m_input_capacity{0};
    std::uint64_t m_bit_buffer{0};
    int m_bits{0};

    // 头部
    std::array<std::uint8_t, 10> m_header_bytes{};
    int m_header_fill{0};
    GzState m_gz_state{GzState::kFixed};
    std::uint8_t m_gz_flags{0};
    std::size_t m_gz_skip{0};
    std::size_t m_gz_string_length{0};
    int m_gz_hcrc_fill{0};
    bool m_gz_need_extra{false};
    bool m_gz_need_name{false};
    bool m_gz_need_comment{false};
    bool m_gz_need_hcrc{false};

    // 动态 Huffman 头部
    DynState m_dyn_state{DynState::kCounts};
    int m_hlit{0};
    int m_hdist{0};
    int m_hclen{0};
    int m_clen_read{0};
    std::array<std::uint8_t, 19> m_clen_lengths{};
    std::vector<std::uint8_t> m_all_lengths;
    std::size_t m_length_index{0};
    std::uint8_t m_prev_length{0};
    std::uint16_t m_repeat_symbol{0};
    HuffmanTable m_clen_table;

    // 块与表
    Phase m_phase{Phase::kHeader};
    bool m_final_block{false};
    int m_block_stage{0};
    std::uint32_t m_block_type{0};
    int m_stored_stage{0};
    std::uint32_t m_stored_len{0};
    std::size_t m_stored_remaining{0};
    HuffStage m_huff_stage{HuffStage::kSymbol};
    std::uint16_t m_pending_length_symbol{0};
    std::uint16_t m_pending_distance_symbol{0};
    std::size_t m_pending_length{0};
    std::size_t m_match_remaining{0};
    std::size_t m_match_distance{0};
    HuffmanTable m_fixed_lit;
    HuffmanTable m_fixed_dist;
    HuffmanTable m_lit_table;
    HuffmanTable m_dist_table;

    // 尾部与校验和
    std::array<std::uint8_t, 8> m_trailer{};
    std::size_t m_trailer_fill{0};
    std::uint32_t m_adler_a{1};
    std::uint32_t m_adler_b{0};
    std::uint32_t m_adler_count{0};
    std::uint32_t m_crc{0xFFFFFFFFU};
    std::uint64_t m_member_out{0};

    // 输出窗口
    std::array<std::uint8_t, kWindowSize> m_window{};
    std::size_t m_window_pos{0};

    ErrorPtr m_error;
};

} // namespace go2cpp::compress::detail
