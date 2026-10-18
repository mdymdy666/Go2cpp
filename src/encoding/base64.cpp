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
 */

#include "go2cpp/encoding/base64.hpp"

#include "go2cpp/encoding/errors.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace go2cpp::encoding {

namespace {

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

std::size_t Base64Encoding::Encode(std::string& dst,
                                   std::string_view src) const {
    const std::size_t start = dst.size();
    dst.reserve(start + EncodedLen(src.size()));
    const char* table = m_alphabet.data();

    std::size_t i = 0;
    for (; i + 3 <= src.size(); i += 3) {
        const std::uint32_t chunk = (static_cast<std::uint32_t>(ByteAt(src, i)) << 16) |
                                    (static_cast<std::uint32_t>(ByteAt(src, i + 1)) << 8) |
                                    static_cast<std::uint32_t>(ByteAt(src, i + 2));
        dst.push_back(table[(chunk >> 18) & 0x3f]);
        dst.push_back(table[(chunk >> 12) & 0x3f]);
        dst.push_back(table[(chunk >> 6) & 0x3f]);
        dst.push_back(table[chunk & 0x3f]);
    }

    const std::size_t rest = src.size() - i;
    if (rest == 1) {
        const std::uint32_t chunk = static_cast<std::uint32_t>(ByteAt(src, i)) << 16;
        dst.push_back(table[(chunk >> 18) & 0x3f]);
        dst.push_back(table[(chunk >> 12) & 0x3f]);
        if (!IsRaw()) {
            dst.push_back(m_pad);
            dst.push_back(m_pad);
        }
    } else if (rest == 2) {
        const std::uint32_t chunk =
            (static_cast<std::uint32_t>(ByteAt(src, i)) << 16) |
            (static_cast<std::uint32_t>(ByteAt(src, i + 1)) << 8);
        dst.push_back(table[(chunk >> 18) & 0x3f]);
        dst.push_back(table[(chunk >> 12) & 0x3f]);
        dst.push_back(table[(chunk >> 6) & 0x3f]);
        if (!IsRaw()) {
            dst.push_back(m_pad);
        }
    }
    return dst.size() - start;
}

std::string Base64Encoding::EncodeToString(std::string_view src) const {
    std::string out;
    Encode(out, src);
    return out;
}

ErrorPtr Base64Encoding::Decode(std::string& dst, std::string_view src) const {
    // 反查表：-1 表示非法字符。四个预定义实例共用同一张表布局，按字母表重建。
    std::array<std::int8_t, 256> decode_map{};
    decode_map.fill(-1);
    for (std::size_t i = 0; i < 64; ++i) {
        decode_map[static_cast<unsigned char>(m_alphabet[i])] =
            static_cast<std::int8_t>(i);
    }
    const auto value_of = [&decode_map](unsigned char c) -> int {
        return static_cast<int>(decode_map[c]);
    };

    const std::size_t len = src.size();
    // 先扫描非法字符：错误位置必须指向第一个非法字节，而不是笼统地报长度。
    for (std::size_t k = 0; k < len; ++k) {
        const unsigned char c = ByteAt(src, k);
        if (value_of(c) < 0 &&
            !(!IsRaw() && c == static_cast<unsigned char>(m_pad))) {
            return CorruptAt(k);
        }
    }
    if (IsRaw()) {
        if (len % 4 == 1) {
            return CorruptAt(len - 1);
        }
    } else if (len % 4 != 0) {
        // 带填充编码必须以完整 quantum 结束：缺填充 / 多字符都非法。
        return CorruptAt(len);
    }

    std::string out;
    out.reserve(DecodedLen(len));

    std::size_t i = 0;
    while (i < len) {
        const std::size_t remaining = len - i;

        if (remaining >= 4 && !(!IsRaw() && i + 4 == len)) {
            // 非末尾的完整 quantum：四位都必须是数据字符，出现 '=' 即非法。
            const int v0 = value_of(ByteAt(src, i));
            const int v1 = value_of(ByteAt(src, i + 1));
            const int v2 = value_of(ByteAt(src, i + 2));
            const int v3 = value_of(ByteAt(src, i + 3));
            if (v0 < 0) return CorruptAt(i);
            if (v1 < 0) return CorruptAt(i + 1);
            if (v2 < 0) return CorruptAt(i + 2);
            if (v3 < 0) return CorruptAt(i + 3);
            const std::uint32_t chunk = (static_cast<std::uint32_t>(v0) << 18) |
                                        (static_cast<std::uint32_t>(v1) << 12) |
                                        (static_cast<std::uint32_t>(v2) << 6) |
                                        static_cast<std::uint32_t>(v3);
            out.push_back(static_cast<char>((chunk >> 16) & 0xff));
            out.push_back(static_cast<char>((chunk >> 8) & 0xff));
            out.push_back(static_cast<char>(chunk & 0xff));
            i += 4;
            continue;
        }

        if (!IsRaw() && remaining == 4) {
            // 末尾 quantum：允许 `xx==` 与 `xxx=` 两种填充形态。
            const int v0 = value_of(ByteAt(src, i));
            const int v1 = value_of(ByteAt(src, i + 1));
            if (v0 < 0) return CorruptAt(i);
            if (v1 < 0) return CorruptAt(i + 1);

            const char c2 = src[i + 2];
            const char c3 = src[i + 3];
            if (c2 == m_pad) {
                if (c3 != m_pad) {
                    return CorruptAt(i + 3);
                }
                // Strict：`xx==` 只用 v1 的高 4 位，低 4 位必须为 0。
                if (m_strict && (v1 & 0x0f) != 0) {
                    return CorruptAt(i + 1);
                }
                out.push_back(static_cast<char>((v0 << 2) | (v1 >> 4)));
                i += 4;
                continue;
            }
            const int v2 = value_of(static_cast<unsigned char>(c2));
            if (v2 < 0) return CorruptAt(i + 2);
            if (c3 == m_pad) {
                // Strict：`xxx=` 只用 v2 的高 6 位，低 2 位必须为 0。
                if (m_strict && (v2 & 0x03) != 0) {
                    return CorruptAt(i + 2);
                }
                out.push_back(static_cast<char>((v0 << 2) | (v1 >> 4)));
                out.push_back(
                    static_cast<char>(((v1 & 0x0f) << 4) | (v2 >> 2)));
                i += 4;
                continue;
            }
            const int v3 = value_of(static_cast<unsigned char>(c3));
            if (v3 < 0) return CorruptAt(i + 3);
            const std::uint32_t chunk = (static_cast<std::uint32_t>(v0) << 18) |
                                        (static_cast<std::uint32_t>(v1) << 12) |
                                        (static_cast<std::uint32_t>(v2) << 6) |
                                        static_cast<std::uint32_t>(v3);
            out.push_back(static_cast<char>((chunk >> 16) & 0xff));
            out.push_back(static_cast<char>((chunk >> 8) & 0xff));
            out.push_back(static_cast<char>(chunk & 0xff));
            i += 4;
            continue;
        }

        // 无填充编码的末尾 quantum：只剩 2 或 3 个字符（%4==1 已提前拒绝）。
        const int v0 = value_of(ByteAt(src, i));
        const int v1 = value_of(ByteAt(src, i + 1));
        if (v0 < 0) return CorruptAt(i);
        if (v1 < 0) return CorruptAt(i + 1);
        if (m_strict && (v1 & 0x0f) != 0) {
            return CorruptAt(i + 1);
        }
        out.push_back(static_cast<char>((v0 << 2) | (v1 >> 4)));
        if (remaining == 3) {
            const int v2 = value_of(ByteAt(src, i + 2));
            if (v2 < 0) return CorruptAt(i + 2);
            if (m_strict && (v2 & 0x03) != 0) {
                return CorruptAt(i + 2);
            }
            out.push_back(static_cast<char>(((v1 & 0x0f) << 4) | (v2 >> 2)));
        }
        i = len;
    }

    // 强异常安全：全部成功后才追加，失败时 dst 保持原样。
    dst += out;
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
