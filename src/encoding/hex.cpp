/**
 * @file hex.cpp
 * @brief `go2cpp/encoding/hex.hpp` 的实现：小写编码 + 大小写不敏感解码。
 *
 * @author DeepSeek Harness
 * @date 2026-10-18
 * @copyright BSD-3-Clause
 */

#include "go2cpp/encoding/hex.hpp"

#include "go2cpp/encoding/errors.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace go2cpp::encoding {

namespace {

constexpr char kLowerHexDigits[] = "0123456789abcdef";

inline unsigned char ByteAt(std::string_view src, std::size_t index) noexcept {
    return static_cast<unsigned char>(src[index]);
}

/// 单字符转十六进制值；非法返回 -1。大小写都接受（对齐 Go `fromHexChar`）。
int HexValue(unsigned char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/// 把非法字节格式化成 Go 风格的可读形式（`U+0067 'g'`）。
std::string DescribeByte(unsigned char c) {
    static const char kHex[] = "0123456789ABCDEF";
    std::string text = "encoding/hex: invalid byte: U+00";
    text.push_back(kHex[(c >> 4) & 0x0f]);
    text.push_back(kHex[c & 0x0f]);
    if (c >= 0x20 && c < 0x7f) {
        text += " '";
        text.push_back(static_cast<char>(c));
        text.push_back('\'');
    }
    return text;
}

}  // namespace

std::size_t Hex::Encode(std::string& dst, std::string_view src) {
    const std::size_t start = dst.size();
    dst.reserve(start + src.size() * 2);
    for (std::size_t i = 0; i < src.size(); ++i) {
        const unsigned char byte = ByteAt(src, i);
        dst.push_back(kLowerHexDigits[byte >> 4]);
        dst.push_back(kLowerHexDigits[byte & 0x0f]);
    }
    return dst.size() - start;
}

std::string Hex::EncodeToString(std::string_view src) {
    std::string out;
    Encode(out, src);
    return out;
}

ErrorPtr Hex::Decode(std::string& dst, std::string_view src) {
    if (src.size() % 2 != 0) {
        return detail::Tagged("encoding/hex: odd length hex string",
                              HexOddLength());
    }

    std::string out;
    out.reserve(src.size() / 2);
    for (std::size_t i = 0; i < src.size(); i += 2) {
        const int high = HexValue(ByteAt(src, i));
        if (high < 0) {
            return detail::Tagged(DescribeByte(ByteAt(src, i)),
                                  HexInvalidByte());
        }
        const int low = HexValue(ByteAt(src, i + 1));
        if (low < 0) {
            return detail::Tagged(DescribeByte(ByteAt(src, i + 1)),
                                  HexInvalidByte());
        }
        out.push_back(static_cast<char>((high << 4) | low));
    }

    dst += out;
    return nullptr;
}

HexDecodeResult Hex::DecodeString(std::string_view src) {
    HexDecodeResult result;
    result.error = Decode(result.data, src);
    if (result.error) {
        result.data.clear();
    }
    return result;
}

}  // namespace go2cpp::encoding
