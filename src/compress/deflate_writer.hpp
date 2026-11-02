/**
 * @file deflate_writer.hpp
 * @brief 自研 DEFLATE（RFC 1951）编码器：固定 Huffman + LZ77 滑动窗口。
 *
 * @details 为什么自研而不是继续依赖系统 zlib：
 *
 *   1. **零外部依赖**：`go2cpp_compress` 不再需要 `find_package(ZLIB)`，
 *      交叉编译/嵌入式部署不必先准备 `libz.so` 或 `libz.a`；
 *   2. **行为可控**：存储块（BTYPE=00）与固定 Huffman 块（BTYPE=01）的
 *      选择规则、级别到搜索深度的映射都写在本仓库里，便于审计与测试；
 *   3. **格式兼容**：输出仍是标准 DEFLATE，Python `zlib`、GNU `gzip` 可直接解。
 *
 * 本实现只做「最小可用子集」，明确不做的部分与原因：
 *
 *   | 未实现 | 原因 |
 *   |---|---|
 *   | 动态 Huffman（BTYPE=10）编码 | 需要码长限制与树构造，收益主要在超短块；固定 Huffman + LZ77 已能满足本项目压缩率要求（日志文本 ~8:1）。解码侧仍完整支持动态 Huffman，见 `deflate_reader.hpp`。 |
 *   | 预置字典 | 当前没有调用场景，公开 API 也没有字典入口。 |
 *   | 多线程/硬件加速 | 每个 Writer 独占状态，无共享数据。 |
 *
 * 压缩策略（按级别）：
 *   - level 0：只发存储块，不压缩；
 *   - level 1..9：LZ77 贪心匹配 + 固定 Huffman。级别越高，哈希链搜索越深、
 *     `nice_length` 越大，压缩率更好但更慢；
 *   - 每个块编码前比较「固定 Huffman 实际位数」与「存储块位数」，后者更小
 *     时回退为存储块。这是高熵数据不膨胀的关键（随机字节输出约 1.0003x，
 *     而不是固定 Huffman 的约 1.055x）。
 *
 * 位序约定：DEFLATE 的 Huffman 码按 MSB-first 定义、按 LSB-first 传输，
 * 因此 `BitWriter::PutCode` 先反转码字；额外位（长度/距离）直接按 LSB-first
 * 写入。
 *
 * @note 该头文件只被 `deflater.cpp` 包含，不属于公开 API。
 */
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace go2cpp::compress::detail {

/**
 * @brief 按位写入的字节流。
 * @details 只在内存里维护一个字节向量；调用方通过 `ConsumeBytes()` 在排空后
 *          释放已写出的整字节，未满一字节的 `m_buffer` 会保留，保证跨块/跨
 *          冲刷的位连续性。
 */
class BitWriter {
public:
    /// 回滚点：输出字节数、未满字节的位缓存与绝对位位置。
    struct Checkpoint {
        std::size_t size;
        std::uint64_t buffer;
        int bits;
        std::uint64_t position;
    };

    /// 写 `count` 位（`value` 的低位在前，即 DEFLATE 的 LSB-first）。
    /// @note 位累加器按 32 位整字落盘（一条 32-bit store），不再逐字节
    ///       `push_back`：后者每个字节一次容量检查 + 长度更新，是编码热路径
    ///       上最贵的一段。
    void PutBits(std::uint32_t value, int count) {
        if (count <= 0) {
            return;
        }
        const std::uint64_t mask = (std::uint64_t{1} << count) - 1U;
        m_buffer |= (static_cast<std::uint64_t>(value) & mask) << m_bits;
        m_bits += count;
        m_position += static_cast<std::uint64_t>(count);
        while (m_bits >= 32) {
            FlushWord();
        }
    }

    /// 写 Huffman 码：码字按 MSB-first 定义，传输时反转成 LSB-first。
    void PutCode(std::uint32_t code, int bits) {
        PutBits(Reverse(code, bits), bits);
    }

    /// 补齐到下一个字节边界（补 0 位）。
    /// @note 位累加器可能持有 8~31 位（32 位批量落盘的尾部），因此必须按
    ///       `m_bits & 7` 补位，不能写死 `8 - m_bits`（后者只有 m_bits<8 时成立）。
    void Align() {
        const int drop = m_bits & 7;
        if (drop != 0) {
            PutBits(0, 8 - drop);
        }
        FlushCompleteBytes();
    }

    /// 当前绝对位位置（含未满字节的位）。
    std::uint64_t position() const noexcept { return m_position; }

    Checkpoint Save() const noexcept {
        return Checkpoint{m_size, m_buffer, m_bits, m_position};
    }

    void Restore(const Checkpoint& checkpoint) {
        m_size = checkpoint.size;
        m_buffer = checkpoint.buffer;
        m_bits = checkpoint.bits;
        m_position = checkpoint.position;
    }

    /// 把累加器里已凑满的整字节落到输出缓冲（不改变位流顺序）。
    /// @details 32 位批量落盘会留下 8~31 位的尾部，其中 >=8 位的部分是
    ///          已确定的整字节；`data()/size()` 的语义要求把它们算进去。
    void FlushCompleteBytes() {
        while (m_bits >= 8) {
            EnsureCapacity(1);
            m_bytes[m_size++] = static_cast<std::uint8_t>(m_buffer & 0xFFU);
            m_buffer >>= 8U;
            m_bits -= 8;
        }
    }

    /// 写对齐后的原始字节（调用方必须先 `Align()`）。
    void PutBytes(const std::uint8_t* data, std::size_t size) {
        if (size == 0) {
            return;
        }
        FlushCompleteBytes();
        EnsureCapacity(size);
        std::memcpy(m_bytes.data() + m_size, data, size);
        m_size += size;
        m_position += static_cast<std::uint64_t>(size) * 8U;
    }

    /// 已写出的完整字节（只读视图）。
    const std::uint8_t* data() const noexcept { return m_bytes.data(); }

    /// 已写出的完整字节数。
    std::size_t size() const noexcept { return m_size; }

    /// 排空后释放整字节缓冲；保留未满字节的位缓存与已分配容量。
    void ConsumeBytes() noexcept { m_size = 0; }

    /// 是否有尚未交给调用方的完整字节。
    bool HasBytes() const noexcept { return m_size != 0; }

private:
    static std::uint32_t Reverse(std::uint32_t code, int bits) noexcept {
        std::uint32_t reversed = 0;
        for (int index = 0; index < bits; ++index) {
            reversed = (reversed << 1U) | (code & 1U);
            code >>= 1U;
        }
        return reversed;
    }

    /// 把累加器低 32 位落到输出缓冲（小端序，LSB-first 位流）。
    void FlushWord() {
        EnsureCapacity(4);
#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
        const std::uint32_t word = static_cast<std::uint32_t>(m_buffer & 0xFFFFFFFFU);
        std::memcpy(m_bytes.data() + m_size, &word, sizeof(word));
#else
        m_bytes[m_size + 0] = static_cast<std::uint8_t>(m_buffer & 0xFFU);
        m_bytes[m_size + 1] = static_cast<std::uint8_t>((m_buffer >> 8U) & 0xFFU);
        m_bytes[m_size + 2] = static_cast<std::uint8_t>((m_buffer >> 16U) & 0xFFU);
        m_bytes[m_size + 3] = static_cast<std::uint8_t>((m_buffer >> 24U) & 0xFFU);
#endif
        m_size += 4;
        m_buffer >>= 32U;
        m_bits -= 32;
    }

    void EnsureCapacity(std::size_t extra) {
        const std::size_t need = m_size + extra;
        if (m_bytes.size() >= need) {
            return;
        }
        std::size_t next = m_bytes.size() * 2U;
        if (next < need) {
            next = need;
        }
        if (next < 4096U) {
            next = 4096U;
        }
        m_bytes.resize(next);
    }

    std::vector<std::uint8_t> m_bytes;  ///< 输出字节（容量复用）
    std::size_t m_size{0};              ///< 已写出的完整字节数
    std::uint64_t m_buffer{0};          ///< 位累加器
    int m_bits{0};                      ///< 累加器中未落盘的位数
    std::uint64_t m_position{0};        ///< 绝对位位置
};

/**
 * @brief 自研 DEFLATE 编码器。
 *
 * 生命周期：由 `detail::Deflater::Impl` 持有；`Feed` 接收未压缩字节，
 * `Pump` 在缓冲达到一个块时产出压缩块，`Sync`/`Finish` 负责冲刷与收尾。
 * 非线程安全：每个 Writer 一个实例。
 */
class DeflateEncoder {
public:
    /// 格式由 windowBits 决定，与 zlib 的 `deflateInit2` 约定一致。
    enum class Format : std::uint8_t {
        kRaw,  ///< -15：raw deflate。
        kZlib, ///< 15：RFC 1950（2 字节头 + adler32 尾）。
        kGzip, ///< 31：RFC 1952（10 字节头 + crc32/isize 尾）。
    };

    DeflateEncoder(int level, int window_bits) {
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
        // Go 的 HuffmanOnly(-2)：只做 Huffman 编码，不用 LZ77 匹配。
        m_huffman_only = (level == -2);
        m_level = (level == -2) ? 6 : level;
        if (m_level < 0) {
            m_level = 6;
        }
        Reset();
    }

    bool ok() const noexcept { return m_ok; }

    void Reset() {
        m_writer = BitWriter{};
        m_input.clear();
        m_history.clear();
        m_combined.clear();
        m_head.assign(kHashSize, kInvalidPosition);
        m_previous.assign(kWindowSize, kInvalidPosition);
        m_consumed = 0;
        m_block_start = 0;
        m_header_done = false;
        m_sync_done = false;
        m_finished = false;
        m_drain_offset = 0;
        m_total_in = 0;
        m_adler_a = 1;
        m_adler_b = 0;
        m_crc = 0xFFFFFFFFU;
        m_error.clear();
    }

    /// 追加未压缩字节；同时更新 adler32/crc32 与总长度。
    void Feed(const std::uint8_t* data, std::size_t size) {
        if (!m_ok || data == nullptr || size == 0) {
            return;
        }
        // 只有对应格式才需要校验和：zlib 用 adler32、gzip 用 crc32，raw 都不用。
        // 旧实现无条件两样都算，等于白烧一半的校验和带宽。
        if (m_format == Format::kZlib) {
            UpdateAdler(data, size);
        } else if (m_format == Format::kGzip) {
            UpdateCrc(data, size);
        }
        m_total_in += size;
        m_input.insert(m_input.end(), data, data + size);
        m_sync_done = false;
    }

    /// 把已满一个块的数据编码出去，控制内部缓冲不超过一个块。
    void Pump() {
        while (m_input.size() >= kBlockSize) {
            m_block_start = m_consumed;
            EncodeBlock(m_input.data(), kBlockSize, false);
            m_consumed += kBlockSize;
            AppendHistory(m_input.data(), kBlockSize);
            m_input.erase(m_input.begin(),
                          m_input.begin() + static_cast<std::ptrdiff_t>(kBlockSize));
        }
    }

    /// 冲刷：把当前数据写成非结束块，再补一个空存储块对齐到字节边界。
    void Sync() {
        if (!m_ok || m_finished || m_sync_done) {
            return;
        }
        EnsureHeader();
        Pump();
        if (!m_input.empty()) {
            m_block_start = m_consumed;
            EncodeBlock(m_input.data(), m_input.size(), false);
            m_consumed += m_input.size();
            AppendHistory(m_input.data(), m_input.size());
            m_input.clear();
        }
        // zlib 的 Z_SYNC_FLUSH 语义：空存储块是「解码端可安全解到此处」的标记。
        EmitStoredBlock(nullptr, 0, false);
        m_sync_done = true;
    }

    /// 结束流：写最终块、补字节边界、写尾部校验和。
    void Finish() {
        if (!m_ok || m_finished) {
            return;
        }
        EnsureHeader();
        Pump();
        if (!m_input.empty()) {
            m_block_start = m_consumed;
            EncodeBlock(m_input.data(), m_input.size(), true);
            m_consumed += m_input.size();
            AppendHistory(m_input.data(), m_input.size());
            m_input.clear();
        } else {
            EmitFixedEmptyFinal();
        }
        m_writer.Align();
        EmitTrailer();
        m_finished = true;
    }

    bool finished() const noexcept { return m_finished; }

    /// 把已产出的完整字节复制到 `out`；返回本次复制字节数。
    std::size_t Drain(std::uint8_t* out, std::size_t capacity) {
        m_writer.FlushCompleteBytes();
        const std::uint8_t* bytes = m_writer.data();
        const std::size_t total = m_writer.size();
        if (m_drain_offset >= total) {
            m_writer.ConsumeBytes();
            m_drain_offset = 0;
            return 0;
        }
        const std::size_t available = total - m_drain_offset;
        const std::size_t take = std::min(available, capacity);
        std::memcpy(out, bytes + m_drain_offset, take);
        m_drain_offset += take;
        if (m_drain_offset == total) {
            m_writer.ConsumeBytes();
            m_drain_offset = 0;
        }
        return take;
    }

    bool HasPending() {
        m_writer.FlushCompleteBytes();
        return m_drain_offset < m_writer.size();
    }

    const std::string& message() const noexcept { return m_error; }

private:
    static constexpr std::size_t kBlockSize = 32U * 1024U;
    static constexpr std::size_t kWindowSize = 32U * 1024U;
    static constexpr std::size_t kWindowMask = kWindowSize - 1U;
    static constexpr int kHashBits = 15;
    static constexpr std::size_t kHashSize = std::size_t{1} << kHashBits;
    static constexpr std::uint32_t kInvalidPosition = 0xFFFFFFFFU;

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

    struct Match {
        int length{0};
        std::uint32_t position{kInvalidPosition};
    };

    int MaxChain() const noexcept {
        switch (m_level) {
            case 0: return 0;
            case 1: return 4;
            case 2: return 8;
            case 3: return 16;
            case 4: return 32;
            case 5: return 48;
            case 6: return 64;
            case 7: return 96;
            case 8: return 128;
            default: return 256;
        }
    }

    int NiceLength() const noexcept {
        if (m_level <= 1) {
            return 16;
        }
        if (m_level <= 3) {
            return 32;
        }
        if (m_level <= 5) {
            return 64;
        }
        if (m_level <= 7) {
            return 128;
        }
        return 258;
    }

    void EnsureHeader() {
        if (m_header_done) {
            return;
        }
        switch (m_format) {
            case Format::kZlib:
                m_writer.PutBits(0x78U, 8); // CMF：CM=8(deflate)、CINFO=7(32 KiB)
                m_writer.PutBits(ZlibFlags(), 8);
                break;
            case Format::kGzip: {
                const std::uint8_t header[10] = {0x1F, 0x8B, 8, 0, 0, 0, 0, 0, 0, 3};
                for (const std::uint8_t byte : header) {
                    m_writer.PutBits(byte, 8);
                }
                break;
            }
            case Format::kRaw:
                break;
        }
        m_header_done = true;
    }

    /// zlib FLG：FLEVEL 反映级别，FCHECK 保证 (CMF<<8|FLG) % 31 == 0。
    std::uint32_t ZlibFlags() const noexcept {
        if (m_level <= 1) {
            return 0x01; // FLEVEL=0
        }
        if (m_level <= 5) {
            return 0x5E; // FLEVEL=1
        }
        if (m_level == 6) {
            return 0x9C; // FLEVEL=2
        }
        return 0xDA; // FLEVEL=3
    }

    void EmitTrailer() {
        if (m_format == Format::kZlib) {
            const std::uint32_t adler = (m_adler_b << 16U) | m_adler_a;
            m_writer.PutBits((adler >> 24U) & 0xFFU, 8);
            m_writer.PutBits((adler >> 16U) & 0xFFU, 8);
            m_writer.PutBits((adler >> 8U) & 0xFFU, 8);
            m_writer.PutBits(adler & 0xFFU, 8);
        } else if (m_format == Format::kGzip) {
            const std::uint32_t crc = m_crc ^ 0xFFFFFFFFU;
            m_writer.PutBits(crc & 0xFFU, 8);
            m_writer.PutBits((crc >> 8U) & 0xFFU, 8);
            m_writer.PutBits((crc >> 16U) & 0xFFU, 8);
            m_writer.PutBits((crc >> 24U) & 0xFFU, 8);
            const std::uint32_t isize = static_cast<std::uint32_t>(m_total_in & 0xFFFFFFFFU);
            m_writer.PutBits(isize & 0xFFU, 8);
            m_writer.PutBits((isize >> 8U) & 0xFFU, 8);
            m_writer.PutBits((isize >> 16U) & 0xFFU, 8);
            m_writer.PutBits((isize >> 24U) & 0xFFU, 8);
        }
    }

    void EncodeBlock(const std::uint8_t* data, std::size_t size, bool final) {
        // 任何块（包括 Write 期间由 Pump 产出的非结束块）都必须先写封装头。
        EnsureHeader();
        if (size == 0) {
            EmitFixedEmptyFinal();
            return;
        }
        if (m_level == 0) {
            EmitStoredBlock(data, size, final);
            return;
        }
        if (m_huffman_only) {
            // Huffman-only 不参与「固定 Huffman vs 存储块」的择优：它的语义
            // 就是只做熵编码，若回退成存储块就名不副实了。
            EmitFixedBlock(data, size, final);
            return;
        }
        const std::uint64_t start = m_writer.position();
        const BitWriter::Checkpoint checkpoint = m_writer.Save();
        EmitFixedBlock(data, size, final);
        const std::uint64_t fixed_bits = m_writer.position() - start;
        const std::uint64_t padding = (8U - ((start + 3U) & 7U)) & 7U;
        const std::uint64_t stored_bits = 3U + padding + 32U + 8U * size;
        if (fixed_bits >= stored_bits) {
            m_writer.Restore(checkpoint);
            EmitStoredBlock(data, size, final);
        }
    }

    void EmitStoredBlock(const std::uint8_t* data, std::size_t size, bool final) {
        m_writer.PutBits(final ? 1U : 0U, 1);
        m_writer.PutBits(0U, 2); // BTYPE=00
        m_writer.Align();
        m_writer.PutBits(static_cast<std::uint32_t>(size & 0xFFFFU), 16);
        m_writer.PutBits(static_cast<std::uint32_t>((~size) & 0xFFFFU), 16);
        m_writer.PutBytes(data, size);
    }

    void EmitFixedEmptyFinal() {
        m_writer.PutBits(1U, 1);
        m_writer.PutBits(1U, 2); // BTYPE=01
        EmitSymbol(256);
    }

    void EmitFixedBlock(const std::uint8_t* data, std::size_t size, bool final) {
        m_writer.PutBits(final ? 1U : 0U, 1);
        m_writer.PutBits(1U, 2); // BTYPE=01
        if (size == 0) {
            EmitSymbol(256);
            return;
        }
        if (m_huffman_only) {
            for (std::size_t index = 0; index < size; ++index) {
                EmitSymbol(data[index]);
            }
            EmitSymbol(256);
            return;
        }
        m_combined.clear();
        m_combined.reserve(m_history.size() + size);
        m_combined.insert(m_combined.end(), m_history.begin(), m_history.end());
        m_combined.insert(m_combined.end(), data, data + size);
        const std::size_t history_size = m_history.size();
        // 哈希链**跨块保留**：位置用流内绝对偏移，前驱表按 32 KiB 环形索引。
        // 旧实现每块重建 head（128 KiB 清零）并把 history 逐位置重插一遍，
        // 32 KiB 一块的额外代价约等于数据本身，且丢掉了跨块匹配。
        const std::uint32_t base =
            static_cast<std::uint32_t>(m_block_start - history_size);
        const int max_chain = MaxChain();
        const int nice_length = NiceLength();
        std::size_t position = history_size;
        while (position < m_combined.size()) {
            const Match match =
                FindMatch(m_combined, position, base, max_chain, nice_length);
            if (match.length >= 3) {
                const std::uint32_t absolute =
                    base + static_cast<std::uint32_t>(position);
                EmitLengthDistance(
                    static_cast<std::size_t>(match.length),
                    static_cast<std::size_t>(absolute - match.position));
                for (int offset = 0; offset < match.length; ++offset) {
                    InsertHash(m_combined, position + static_cast<std::size_t>(offset), base);
                }
                position += static_cast<std::size_t>(match.length);
            } else {
                EmitSymbol(m_combined[position]);
                InsertHash(m_combined, position, base);
                ++position;
            }
        }
        EmitSymbol(256);
    }

    static std::uint32_t Hash3(const std::uint8_t* data) noexcept {
        const std::uint32_t value = (static_cast<std::uint32_t>(data[0]) << 16U) |
                                    (static_cast<std::uint32_t>(data[1]) << 8U) |
                                    static_cast<std::uint32_t>(data[2]);
        return (value * 2654435761U) >> (32 - kHashBits);
    }

    /// 从两个位置开始比较，返回公共前缀长度（8 字节一跳，按 ctz 定位差异）。
    static std::size_t MatchLength(const std::uint8_t* lhs,
                                   const std::uint8_t* rhs,
                                   std::size_t max_length) noexcept {
        std::size_t length = 0;
        while (length + 8 <= max_length) {
            std::uint64_t a = 0;
            std::uint64_t b = 0;
            std::memcpy(&a, lhs + length, sizeof(a));
            std::memcpy(&b, rhs + length, sizeof(b));
            const std::uint64_t diff = a ^ b;
            if (diff != 0) {
#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
                return length + static_cast<std::size_t>(__builtin_ctzll(diff) >> 3);
#else
                return length + static_cast<std::size_t>(__builtin_clzll(diff) >> 3);
#endif
            }
            length += 8;
        }
        while (length < max_length && lhs[length] == rhs[length]) {
            ++length;
        }
        return length;
    }

    /// 插入当前位置的哈希链（绝对位置，前驱表环形）。
    void InsertHash(const std::vector<std::uint8_t>& data, std::size_t position,
                    std::uint32_t base) {
        if (position + 2U >= data.size()) {
            return;
        }
        const std::uint32_t absolute = base + static_cast<std::uint32_t>(position);
        const std::uint32_t hash = Hash3(data.data() + position);
        m_previous[absolute & kWindowMask] = m_head[hash];
        m_head[hash] = absolute;
    }

    /// 在 32 KiB 窗口内找最长匹配；`base` 是 `data[0]` 的流内绝对偏移。
    Match FindMatch(const std::vector<std::uint8_t>& data, std::size_t position,
                    std::uint32_t base, int max_chain, int nice_length) const {
        Match best;
        if (position + 2U >= data.size()) {
            return best;
        }
        const std::uint32_t absolute = base + static_cast<std::uint32_t>(position);
        const std::size_t max_length = std::min<std::size_t>(258U, data.size() - position);
        std::uint32_t candidate = m_head[Hash3(data.data() + position)];
        int chain = max_chain;
        while (candidate != kInvalidPosition && chain-- > 0) {
            const std::uint32_t distance = absolute - candidate;
            if (distance == 0 || distance > kWindowSize) {
                break;
            }
            const std::size_t candidate_position = candidate - base;
            if (best.length == 0 ||
                data[candidate_position + static_cast<std::size_t>(best.length)] ==
                    data[position + static_cast<std::size_t>(best.length)]) {
                const std::size_t length =
                    MatchLength(data.data() + candidate_position,
                                data.data() + position, max_length);
                if (length > static_cast<std::size_t>(best.length)) {
                    best.length = static_cast<int>(length);
                    best.position = candidate;
                    if (length >= static_cast<std::size_t>(nice_length)) {
                        break;
                    }
                }
            }
            candidate = m_previous[candidate & kWindowMask];
        }
        return best;
    }

    void EmitLengthDistance(std::size_t length, std::size_t distance) {
        int length_index = 28;
        while (length_index > 0 &&
               length < static_cast<std::size_t>(kLengthBase[length_index])) {
            --length_index;
        }
        EmitFixedCode(257 + length_index);
        const int extra = kLengthExtra[length_index];
        if (extra > 0) {
            m_writer.PutBits(static_cast<std::uint32_t>(
                                 length - static_cast<std::size_t>(kLengthBase[length_index])),
                             extra);
        }
        int distance_index = 29;
        while (distance_index > 0 &&
               distance < static_cast<std::size_t>(kDistanceBase[distance_index])) {
            --distance_index;
        }
        // 固定 Huffman 的距离码只有 5 位，反转结果可预计算。
        m_writer.PutBits(ReversedDistCodes()[distance_index], 5);
        const int distance_extra = kDistanceExtra[distance_index];
        if (distance_extra > 0) {
            m_writer.PutBits(static_cast<std::uint32_t>(
                                 distance -
                                 static_cast<std::size_t>(kDistanceBase[distance_index])),
                             distance_extra);
        }
    }

    void EmitSymbol(int symbol) { EmitFixedCode(symbol); }

    /// 固定 Huffman 码字表（已按位流顺序反转），避免每符号做位反转循环。
    static const std::array<std::uint32_t, 288>& ReversedLitCodes() {
        static const std::array<std::uint32_t, 288> table = [] {
            std::array<std::uint32_t, 288> result{};
            for (int symbol = 0; symbol < 288; ++symbol) {
                std::uint32_t code = 0;
                int bits = 0;
                FixedLitCode(symbol, &code, &bits);
                result[static_cast<std::size_t>(symbol)] =
                    BitWriterReverse(code, bits);
            }
            return result;
        }();
        return table;
    }

    static const std::array<std::uint8_t, 288>& LitCodeBits() {
        static const std::array<std::uint8_t, 288> table = [] {
            std::array<std::uint8_t, 288> result{};
            for (int symbol = 0; symbol < 288; ++symbol) {
                std::uint32_t code = 0;
                int bits = 0;
                FixedLitCode(symbol, &code, &bits);
                result[static_cast<std::size_t>(symbol)] =
                    static_cast<std::uint8_t>(bits);
            }
            return result;
        }();
        return table;
    }

    static const std::array<std::uint32_t, 32>& ReversedDistCodes() {
        static const std::array<std::uint32_t, 32> table = [] {
            std::array<std::uint32_t, 32> result{};
            for (int index = 0; index < 32; ++index) {
                result[static_cast<std::size_t>(index)] =
                    BitWriterReverse(static_cast<std::uint32_t>(index), 5);
            }
            return result;
        }();
        return table;
    }

    static std::uint32_t BitWriterReverse(std::uint32_t code, int bits) noexcept {
        std::uint32_t reversed = 0;
        for (int index = 0; index < bits; ++index) {
            reversed = (reversed << 1U) | (code & 1U);
            code >>= 1U;
        }
        return reversed;
    }

    void EmitFixedCode(int symbol) {
        m_writer.PutBits(ReversedLitCodes()[static_cast<std::size_t>(symbol)],
                         LitCodeBits()[static_cast<std::size_t>(symbol)]);
    }

    static void FixedLitCode(int symbol, std::uint32_t* code, int* bits) noexcept {
        if (symbol <= 143) {
            *code = 0x30U + static_cast<std::uint32_t>(symbol);
            *bits = 8;
        } else if (symbol <= 255) {
            *code = 0x190U + static_cast<std::uint32_t>(symbol - 144);
            *bits = 9;
        } else if (symbol <= 279) {
            *code = static_cast<std::uint32_t>(symbol - 256);
            *bits = 7;
        } else {
            *code = 0xC0U + static_cast<std::uint32_t>(symbol - 280);
            *bits = 8;
        }
    }

    void AppendHistory(const std::uint8_t* data, std::size_t size) {
        m_history.insert(m_history.end(), data, data + size);
        if (m_history.size() > kWindowSize) {
            const std::size_t drop = m_history.size() - kWindowSize;
            m_history.erase(m_history.begin(),
                            m_history.begin() + static_cast<std::ptrdiff_t>(drop));
        }
    }

    void UpdateAdler(const std::uint8_t* data, std::size_t size) {
        constexpr std::uint32_t kMod = 65521U;
        constexpr std::size_t kBatch = 5552U;
        std::uint32_t a = m_adler_a;
        std::uint32_t b = m_adler_b;
        std::size_t offset = 0;
        while (offset < size) {
            const std::size_t take = std::min(kBatch, size - offset);
            for (std::size_t index = 0; index < take; ++index) {
                a += data[offset + index];
                b += a;
            }
            a %= kMod;
            b %= kMod;
            offset += take;
        }
        m_adler_a = a;
        m_adler_b = b;
    }

    void UpdateCrc(const std::uint8_t* data, std::size_t size) {
        const auto& tables = CrcTables();
        std::uint32_t crc = m_crc;
        std::size_t index = 0;
#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
        // slice-by-8：每 8 字节 8 次查表，比逐字节查表快数倍。
        for (; index + 8 <= size; index += 8) {
            std::uint64_t block = 0;
            std::memcpy(&block, data + index, sizeof(block));
            crc ^= static_cast<std::uint32_t>(block & 0xFFFFFFFFU);
            const std::uint32_t high = static_cast<std::uint32_t>(block >> 32U);
            crc = tables[7][crc & 0xFFU] ^ tables[6][(crc >> 8U) & 0xFFU] ^
                  tables[5][(crc >> 16U) & 0xFFU] ^ tables[4][(crc >> 24U) & 0xFFU] ^
                  tables[3][high & 0xFFU] ^ tables[2][(high >> 8U) & 0xFFU] ^
                  tables[1][(high >> 16U) & 0xFFU] ^ tables[0][(high >> 24U) & 0xFFU];
        }
#endif
        for (; index < size; ++index) {
            crc = tables[0][(crc ^ data[index]) & 0xFFU] ^ (crc >> 8U);
        }
        m_crc = crc;
    }

    static const std::array<std::array<std::uint32_t, 256>, 8>& CrcTables() {
        static const std::array<std::array<std::uint32_t, 256>, 8> tables = [] {
            std::array<std::array<std::uint32_t, 256>, 8> result{};
            for (std::uint32_t index = 0; index < 256; ++index) {
                std::uint32_t value = index;
                for (int bit = 0; bit < 8; ++bit) {
                    value = (value & 1U) ? (0xEDB88320U ^ (value >> 1U)) : (value >> 1U);
                }
                result[0][index] = value;
            }
            for (std::size_t slice = 1; slice < 8; ++slice) {
                for (std::uint32_t index = 0; index < 256; ++index) {
                    const std::uint32_t previous = result[slice - 1][index];
                    result[slice][index] =
                        (previous >> 8U) ^ result[0][previous & 0xFFU];
                }
            }
            return result;
        }();
        return tables;
    }

    Format m_format{Format::kRaw};
    bool m_ok{false};
    bool m_huffman_only{false};
    int m_level{6};
    BitWriter m_writer;
    std::vector<std::uint8_t> m_input;
    std::vector<std::uint8_t> m_history;
    /// 块内拼接缓冲（history + 当前块），跨块复用避免每次分配。
    std::vector<std::uint8_t> m_combined;
    /// 哈希头表与前驱表，跨块持久（位置为流内绝对偏移）。
    std::vector<std::uint32_t> m_head;
    std::vector<std::uint32_t> m_previous;
    /// 已编码的流内字节数（块起点的绝对偏移基准）。
    std::size_t m_consumed{0};
    /// 当前块在流内的绝对起点。
    std::size_t m_block_start{0};
    bool m_header_done{false};
    bool m_sync_done{false};
    bool m_finished{false};
    std::size_t m_drain_offset{0};
    std::uint64_t m_total_in{0};
    std::uint32_t m_adler_a{1};
    std::uint32_t m_adler_b{0};
    std::uint32_t m_crc{0xFFFFFFFFU};
    std::string m_error;
};

} // namespace go2cpp::compress::detail
