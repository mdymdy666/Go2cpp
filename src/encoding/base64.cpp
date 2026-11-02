/**
 * @file base64.cpp
 * @brief `go2cpp/encoding/base64.hpp` 的实现：四种 base64 编码的编解码与严格校验。
 *
 * @author DeepSeek Harness
 * @date 2026-10-18
 * @copyright BSD-3-Clause
 *
 * @details 解码状态机刻意逐 quantum 处理，而不是先过滤 `=` 再统一解码：
 *          「填充只能出现在最后一个 quantum」「填充后不能再有数据」必须在
 *          同一次扫描里判断，否则 `"Zg==Zm9v"` 这类输入会被错误接受。
 *
 * ## 汇编级优化说明（2026-11-02）
 *
 * 旧实现的热路径有三处白给的开销，已全部消除：
 *
 *  1. **编码每字节一次 `push_back`**：每次 push_back 都要做容量检查 + 更新
 *     长度。现在先 `resize` 到最终长度，再用 4 字节 `memcpy` 一次写满一个
 *     quantum（编译器生成单条 32-bit store）。
 *  2. **解码每次调用重建 256 项反查表**：`fill(-1)` + 64 次写，小输入时比
 *     解码本身还贵。反查表移到 `Base64Encoding` 构造期（`constexpr`，四个
 *     预定义实例在编译期就是常量表），Decode 只做一次 `m_decode` 取址。
 *  3. **解码两遍扫描 + 逐字节 `push_back` 到临时串**：正常路径改为单遍
 *     quantum 解码 + 4 字节 store（写完 3 字节后指针 +3，末尾多写的 1 字节
 *     落在 scratch 的预留区），并用 `thread_local` scratch 复用缓冲区，
 *     避免每次解码一次堆分配。长度错误的输入仍走「先找第一个非法字符」的
 *     慢路径，保证 `"Zm9v!"` 依旧报 `input byte 4` 而不是长度错误。
 *
 * 反查表布局：`m_decode[c]` 为 0..63 表示数据字符，-1 表示非法；填充字符
 * 单独比较（它不在字母表里）。
 *
 * ## 模块定位
 *
 * `base64.hpp` 的实现层：四个预定义实例共用这里的编码/解码算法，差异只由
 * `m_alphabet`/`m_pad`/`m_strict` 决定。被所有包含 `base64.hpp` 的协议/序列化
 * 模块间接调用。
 *
 * ## 边界与陷阱
 *
 * 错误位置必须指向**第一个**非法字节；解码先写 `thread_local` scratch，全部
 * 成功后才追加到 `dst`，失败时 `dst` 不变。`Encode` 用 `resize` 一次分配，
 * 尾部分支不要漏掉 Raw 的填充差异。
 */

#include "go2cpp/encoding/base64.hpp"

#include "go2cpp/encoding/errors.hpp"

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace go2cpp::encoding {

namespace {

#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
constexpr bool kLittleEndian = true;
#else
constexpr bool kLittleEndian = false;
#endif

/// 构造 `illegal base64 data at input byte N` 错误（对齐 Go 的错误文本）。
ErrorPtr CorruptAt(std::size_t index) {
    return detail::Tagged(
        "illegal base64 data at input byte " + std::to_string(index),
        Base64CorruptInput());
}

/// 把 `char` 按无符号字节读取。
inline unsigned char ByteAt(std::string_view src, std::size_t index) noexcept {
    return static_cast<unsigned char>(src[index]);
}

/// 把一个 24-bit chunk 按大端写成 4 字节（第 4 字节是垃圾，由调用方丢弃）。
inline void Store3BigEndian(char* out, std::uint32_t chunk) noexcept {
    if constexpr (kLittleEndian) {
        const std::uint32_t big = __builtin_bswap32(chunk << 8U);
        std::memcpy(out, &big, sizeof(big));
    } else {
        out[0] = static_cast<char>((chunk >> 16U) & 0xffU);
        out[1] = static_cast<char>((chunk >> 8U) & 0xffU);
        out[2] = static_cast<char>(chunk & 0xffU);
    }
}

/// 把 4 个 ASCII 字节按给定顺序写成 4 字节。
inline void Store4(char* out, std::uint32_t packed) noexcept {
    if constexpr (kLittleEndian) {
        std::memcpy(out, &packed, sizeof(packed));
    } else {
        out[0] = static_cast<char>(packed & 0xffU);
        out[1] = static_cast<char>((packed >> 8U) & 0xffU);
        out[2] = static_cast<char>((packed >> 16U) & 0xffU);
        out[3] = static_cast<char>((packed >> 24U) & 0xffU);
    }
}

}  // namespace

std::size_t Base64Encoding::EncodedLen(std::size_t src_len) const noexcept {
    const std::size_t full = src_len / 3 * 4;
    const std::size_t rest = src_len % 3;
    if (rest == 0) {
        return full;
    }
    // 剩余 1 字节 -> 2 个字符；剩余 2 字节 -> 3 个字符。
    return full + (IsRaw() ? rest + 1 : 4);
}

std::size_t Base64Encoding::DecodedLen(std::size_t src_len) const noexcept {
    if (IsRaw()) {
        // 对齐 Go：n/4*3 + (n%4)*6/8；不校验输入是否合法。
        return src_len / 4 * 3 + src_len % 4 * 6 / 8;
    }
    return src_len / 4 * 3;
}

/**
 * @brief 把 `src` 编码后追加到 `dst`。
 *
 * ## 步骤
 * step1. 记录追加起点，按 `EncodedLen` 一次性 `resize`；
 * step2. 每 3 字节拼成 24-bit chunk，查字母表后用一次 32-bit store 写出 4 字符；
 * step3. 尾部剩 1/2 字节：输出 2/3 个字符，非 Raw 时补 `=`；
 * step4. 返回本次追加的字节数。
 */
std::size_t Base64Encoding::Encode(std::string& dst,
                                   std::string_view src) const {
    // step1: 记录追加起点并一次性扩展到最终长度
    const std::size_t start = dst.size();
    const std::size_t encoded = EncodedLen(src.size());
    if (encoded == 0) {
        return 0;
    }
    dst.resize(start + encoded);
    char* out = dst.data() + start;
    const char* table = m_alphabet.data();
    const auto* in = reinterpret_cast<const unsigned char*>(src.data());
    const std::size_t size = src.size();

    // step2: 完整的 3 字节 -> 4 字符（单次 32-bit store）
    std::size_t i = 0;
    for (; i + 3 <= size; i += 3) {
        const std::uint32_t chunk =
            (static_cast<std::uint32_t>(in[i]) << 16) |
            (static_cast<std::uint32_t>(in[i + 1]) << 8) |
            static_cast<std::uint32_t>(in[i + 2]);
        const std::uint32_t packed =
            static_cast<std::uint32_t>(
                static_cast<unsigned char>(table[(chunk >> 18) & 0x3f])) |
            (static_cast<std::uint32_t>(
                 static_cast<unsigned char>(table[(chunk >> 12) & 0x3f]))
             << 8) |
            (static_cast<std::uint32_t>(
                 static_cast<unsigned char>(table[(chunk >> 6) & 0x3f]))
             << 16) |
            (static_cast<std::uint32_t>(
                 static_cast<unsigned char>(table[chunk & 0x3f]))
             << 24);
        Store4(out, packed);
        out += 4;    }

    // step3: 尾部 1/2 字节，按是否 Raw 决定补几个 '='
    const std::size_t rest = size - i;
    if (rest == 1) {
        const std::uint32_t chunk = static_cast<std::uint32_t>(in[i]) << 16;
        out[0] = table[(chunk >> 18) & 0x3f];
        out[1] = table[(chunk >> 12) & 0x3f];
        if (!IsRaw()) {
            out[2] = m_pad;
            out[3] = m_pad;
        }
    } else if (rest == 2) {
        const std::uint32_t chunk =
            (static_cast<std::uint32_t>(in[i]) << 16) |
            (static_cast<std::uint32_t>(in[i + 1]) << 8);
        out[0] = table[(chunk >> 18) & 0x3f];
        out[1] = table[(chunk >> 12) & 0x3f];
        out[2] = table[(chunk >> 6) & 0x3f];
        if (!IsRaw()) {
            out[3] = m_pad;
        }
    }
    // step4: 返回追加长度（含填充）
    return encoded;
}

std::string Base64Encoding::EncodeToString(std::string_view src) const {
    std::string out;
    Encode(out, src);
    return out;
}

/**
 * @brief 严格解码 `src`，成功后才把结果追加到 `dst`。
 *
 * ## 步骤
 * step1. 长度错误时走慢路径：先找第一个非法字符（保证错误位置与旧实现一致），
 *        找不到才报长度错误；
 * step2. 正常路径单遍解码：每 quantum 查 `m_decode`，4 字节 store 写 3 字节；
 * step3. 末尾 quantum 处理 `xx==`/`xxx=`/Raw 的 2~3 字符尾巴，
 *        strict 时校验未使用的低位必须为 0；
 * step4. 全部成功后才追加到 `dst`（失败时 `dst` 保持不变）。
 */
ErrorPtr Base64Encoding::Decode(std::string& dst, std::string_view src) const {
    const std::size_t len = src.size();
    if (len == 0) {
        return nullptr;
    }
    const bool raw = IsRaw();
    const std::int8_t* decode = m_decode.data();
    const char* data = src.data();

    // step1: 长度不合法时才扫描非法字符，保证错误位置语义不变。
    if (!raw) {
        if (len % 4 != 0) {
            for (std::size_t k = 0; k < len; ++k) {
                const unsigned char c = static_cast<unsigned char>(data[k]);
                if (decode[c] < 0 && c != static_cast<unsigned char>(m_pad)) {
                    return CorruptAt(k);
                }
            }
            return CorruptAt(len);
        }
    } else if (len % 4 == 1) {
        for (std::size_t k = 0; k < len; ++k) {
            if (decode[static_cast<unsigned char>(data[k])] < 0) {
                return CorruptAt(k);
            }
        }
        return CorruptAt(len - 1);
    }

    // step2: 解码到线程本地 scratch（复用容量，避免每次分配）。
    thread_local std::string scratch;
    const std::size_t decoded_len = DecodedLen(len);
    scratch.resize(decoded_len + 4);  // +4：允许最后一个 quantum 的 4 字节 store
    char* out = scratch.data();

    std::size_t i = 0;
    if (!raw) {
        // 除最后一个 quantum 外的完整 quantum：四位都必须是数据字符。
        while (i + 4 < len) {
            const int v0 = decode[static_cast<unsigned char>(data[i])];
            const int v1 = decode[static_cast<unsigned char>(data[i + 1])];
            const int v2 = decode[static_cast<unsigned char>(data[i + 2])];
            const int v3 = decode[static_cast<unsigned char>(data[i + 3])];
            if (v0 < 0) return CorruptAt(i);
            if (v1 < 0) return CorruptAt(i + 1);
            if (v2 < 0) return CorruptAt(i + 2);
            if (v3 < 0) return CorruptAt(i + 3);
            const std::uint32_t chunk = (static_cast<std::uint32_t>(v0) << 18) |
                                        (static_cast<std::uint32_t>(v1) << 12) |
                                        (static_cast<std::uint32_t>(v2) << 6) |
                                        static_cast<std::uint32_t>(v3);
            Store3BigEndian(out, chunk);
            out += 3;
            i += 4;
        }
        // step3a: 末尾 quantum：允许 `xx==` 与 `xxx=` 两种填充形态。
        const int v0 = decode[static_cast<unsigned char>(data[i])];
        const int v1 = decode[static_cast<unsigned char>(data[i + 1])];
        if (v0 < 0) return CorruptAt(i);
        if (v1 < 0) return CorruptAt(i + 1);
        const char c2 = data[i + 2];
        const char c3 = data[i + 3];
        if (c2 == m_pad) {
            if (c3 != m_pad) {
                return CorruptAt(i + 3);
            }
            // Strict：`xx==` 只用 v1 的高 4 位，低 4 位必须为 0。
            if (m_strict && (v1 & 0x0f) != 0) {
                return CorruptAt(i + 1);
            }
            out[0] = static_cast<char>((v0 << 2) | (v1 >> 4));
            out += 1;
        } else {
            const int v2 = decode[static_cast<unsigned char>(c2)];
            if (v2 < 0) return CorruptAt(i + 2);
            if (c3 == m_pad) {
                // Strict：`xxx=` 只用 v2 的高 6 位，低 2 位必须为 0。
                if (m_strict && (v2 & 0x03) != 0) {
                    return CorruptAt(i + 2);
                }
                out[0] = static_cast<char>((v0 << 2) | (v1 >> 4));
                out[1] = static_cast<char>(((v1 & 0x0f) << 4) | (v2 >> 2));
                out += 2;
            } else {
                const int v3 = decode[static_cast<unsigned char>(c3)];
                if (v3 < 0) return CorruptAt(i + 3);
                const std::uint32_t chunk =
                    (static_cast<std::uint32_t>(v0) << 18) |
                    (static_cast<std::uint32_t>(v1) << 12) |
                    (static_cast<std::uint32_t>(v2) << 6) |
                    static_cast<std::uint32_t>(v3);
                Store3BigEndian(out, chunk);
                out += 3;
            }
        }
        i = len;
    } else {
        // 无填充编码：完整 quantum。
        while (i + 4 <= len) {
            const int v0 = decode[static_cast<unsigned char>(data[i])];
            const int v1 = decode[static_cast<unsigned char>(data[i + 1])];
            const int v2 = decode[static_cast<unsigned char>(data[i + 2])];
            const int v3 = decode[static_cast<unsigned char>(data[i + 3])];
            if (v0 < 0) return CorruptAt(i);
            if (v1 < 0) return CorruptAt(i + 1);
            if (v2 < 0) return CorruptAt(i + 2);
            if (v3 < 0) return CorruptAt(i + 3);
            const std::uint32_t chunk = (static_cast<std::uint32_t>(v0) << 18) |
                                        (static_cast<std::uint32_t>(v1) << 12) |
                                        (static_cast<std::uint32_t>(v2) << 6) |
                                        static_cast<std::uint32_t>(v3);
            Store3BigEndian(out, chunk);
            out += 3;
            i += 4;
        }
        // step3b: 无填充编码的末尾 quantum：只剩 2 或 3 个字符。
        if (i < len) {
            const std::size_t remaining = len - i;
            const int v0 = decode[static_cast<unsigned char>(data[i])];
            const int v1 = decode[static_cast<unsigned char>(data[i + 1])];
            if (v0 < 0) return CorruptAt(i);
            if (v1 < 0) return CorruptAt(i + 1);
            if (m_strict && (v1 & 0x0f) != 0) {
                return CorruptAt(i + 1);
            }
            out[0] = static_cast<char>((v0 << 2) | (v1 >> 4));
            out += 1;
            if (remaining == 3) {
                const int v2 = decode[static_cast<unsigned char>(data[i + 2])];
                if (v2 < 0) return CorruptAt(i + 2);
                if (m_strict && (v2 & 0x03) != 0) {
                    return CorruptAt(i + 2);
                }
                out[0] = static_cast<char>(((v1 & 0x0f) << 4) | (v2 >> 2));
                out += 1;
            }
        }
    }

    // step4: 强异常安全：全部成功后才追加，失败时 dst 保持原样。
    scratch.resize(static_cast<std::size_t>(out - scratch.data()));
    dst.append(scratch);
    return nullptr;
}

Base64DecodeResult Base64Encoding::DecodeString(std::string_view src) const {
    Base64DecodeResult result;
    result.error = Decode(result.data, src);
    if (result.error) {
        result.data.clear();
    }
    return result;
}

}  // namespace go2cpp::encoding
