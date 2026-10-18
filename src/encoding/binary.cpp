/**
 * @file binary.cpp
 * @brief `go2cpp/encoding/binary.hpp` 的实现：定长字节序读写 + LEB128 变长整数。
 *
 * @author DeepSeek Harness
 * @date 2026-10-18
 * @copyright BSD-3-Clause
 *
 * @details 所有读取路径都先检查长度再访问字节，因此短输入只会得到
 *          `ErrShortBuffer()`，不会越界；这是相对 Go（切片过短会 panic）的
 *          刻意差异，也是 C++ 侧必须自己承担的边界责任。
 *
 * ## 模块定位
 *
 * `binary.hpp` 的实现层：定长整数的字节序读写与 LEB128/zig-zag 变长整数。
 * 上游是协议帧解析与序列化模块，下游只有 `go2cpp::tools::error`。
 *
 * ## 本文件定义什么
 *
 * - 内部工具：`Load16/32/64`、`Store16/32/64`、`ByteAt`、`LowByte`；
 * - `ShortBuffer()`：带函数名/期望长度/实际长度的 `ErrShortBuffer()` 包装；
 * - `Binary` 的 8 组定长读写、`Uvarint`/`Varint` 及其写入函数。
 *
 * ## 阅读建议
 *
 * 定长部分只需确认「先查长度、再按 Order 组装」；重点看 `Uvarint` 的
 * 10 字节边界与 `Varint` 的 zig-zag 还原。
 *
 * ## 边界与陷阱
 *
 * 定长读取返回的 `value` 在失败时为 0；`Uvarint` 恰好 10 个续位字节返回
 * 「截断」而不是「溢出」，只有出现第 11 个字节才判溢出。
 */

#include "go2cpp/encoding/binary.hpp"

#include "go2cpp/encoding/errors.hpp"

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace go2cpp::encoding {

namespace {

using Order = Binary::Order;

inline unsigned char ByteAt(std::string_view src, std::size_t index) noexcept {
    return static_cast<unsigned char>(src[index]);
}

inline char LowByte(std::uint64_t value) noexcept {
    return static_cast<char>(value & 0xff);
}

ErrorPtr ShortBuffer(const char* function, std::size_t need, std::size_t got) {
    return detail::Tagged(std::string("binary.") + function +
                              ": buffer too small: need " +
                              std::to_string(need) + " bytes, got " +
                              std::to_string(got),
                          ErrShortBuffer());
}

std::uint16_t Load16(std::string_view src, Order order) noexcept {
    const std::uint16_t b0 = ByteAt(src, 0);
    const std::uint16_t b1 = ByteAt(src, 1);
    return order == Order::kBigEndian ? static_cast<std::uint16_t>((b0 << 8) | b1)
                                      : static_cast<std::uint16_t>((b1 << 8) | b0);
}

std::uint32_t Load32(std::string_view src, Order order) noexcept {
    const std::uint32_t b0 = ByteAt(src, 0);
    const std::uint32_t b1 = ByteAt(src, 1);
    const std::uint32_t b2 = ByteAt(src, 2);
    const std::uint32_t b3 = ByteAt(src, 3);
    if (order == Order::kBigEndian) {
        return (b0 << 24) | (b1 << 16) | (b2 << 8) | b3;
    }
    return (b3 << 24) | (b2 << 16) | (b1 << 8) | b0;
}

std::uint64_t Load64(std::string_view src, Order order) noexcept {
    std::uint64_t value = 0;
    if (order == Order::kBigEndian) {
        for (std::size_t i = 0; i < 8; ++i) {
            value = (value << 8) | ByteAt(src, i);
        }
    } else {
        for (std::size_t i = 0; i < 8; ++i) {
            value |= static_cast<std::uint64_t>(ByteAt(src, i)) << (8 * i);
        }
    }
    return value;
}

void Store16(std::string& dst, std::uint16_t value, Order order) {
    if (order == Order::kBigEndian) {
        dst.push_back(LowByte(value >> 8));
        dst.push_back(LowByte(value));
    } else {
        dst.push_back(LowByte(value));
        dst.push_back(LowByte(value >> 8));
    }
}

void Store32(std::string& dst, std::uint32_t value, Order order) {
    if (order == Order::kBigEndian) {
        dst.push_back(LowByte(value >> 24));
        dst.push_back(LowByte(value >> 16));
        dst.push_back(LowByte(value >> 8));
        dst.push_back(LowByte(value));
    } else {
        dst.push_back(LowByte(value));
        dst.push_back(LowByte(value >> 8));
        dst.push_back(LowByte(value >> 16));
        dst.push_back(LowByte(value >> 24));
    }
}

void Store64(std::string& dst, std::uint64_t value, Order order) {
    if (order == Order::kBigEndian) {
        for (int shift = 56; shift >= 0; shift -= 8) {
            dst.push_back(LowByte(value >> shift));
        }
    } else {
        for (int shift = 0; shift < 64; shift += 8) {
            dst.push_back(LowByte(value >> shift));
        }
    }
}

}  // namespace

Binary::Uint16Result Binary::Uint16(std::string_view src, Order order) noexcept {
    if (src.size() < 2) {
        return {0, ShortBuffer("Uint16", 2, src.size())};
    }
    return {Load16(src, order), nullptr};
}

Binary::Uint32Result Binary::Uint32(std::string_view src, Order order) noexcept {
    if (src.size() < 4) {
        return {0, ShortBuffer("Uint32", 4, src.size())};
    }
    return {Load32(src, order), nullptr};
}

Binary::Uint64Result Binary::Uint64(std::string_view src, Order order) noexcept {
    if (src.size() < 8) {
        return {0, ShortBuffer("Uint64", 8, src.size())};
    }
    return {Load64(src, order), nullptr};
}

Binary::Int16Result Binary::Int16(std::string_view src, Order order) noexcept {
    const Uint16Result raw = Uint16(src, order);
    if (!raw) {
        return {0, raw.error};
    }
    std::int16_t value = 0;
    std::memcpy(&value, &raw.value, sizeof(value));
    return {value, nullptr};
}

Binary::Int32Result Binary::Int32(std::string_view src, Order order) noexcept {
    const Uint32Result raw = Uint32(src, order);
    if (!raw) {
        return {0, raw.error};
    }
    std::int32_t value = 0;
    std::memcpy(&value, &raw.value, sizeof(value));
    return {value, nullptr};
}

Binary::Int64Result Binary::Int64(std::string_view src, Order order) noexcept {
    const Uint64Result raw = Uint64(src, order);
    if (!raw) {
        return {0, raw.error};
    }
    std::int64_t value = 0;
    std::memcpy(&value, &raw.value, sizeof(value));
    return {value, nullptr};
}

Binary::Float32Result Binary::Float32(std::string_view src,
                                      Order order) noexcept {
    const Uint32Result raw = Uint32(src, order);
    if (!raw) {
        return {0.0f, raw.error};
    }
    float value = 0.0f;
    std::memcpy(&value, &raw.value, sizeof(value));
    return {value, nullptr};
}

Binary::Float64Result Binary::Float64(std::string_view src,
                                      Order order) noexcept {
    const Uint64Result raw = Uint64(src, order);
    if (!raw) {
        return {0.0, raw.error};
    }
    double value = 0.0;
    std::memcpy(&value, &raw.value, sizeof(value));
    return {value, nullptr};
}

std::size_t Binary::PutUint16(std::string& dst, std::uint16_t value,
                              Order order) {
    const std::size_t start = dst.size();
    Store16(dst, value, order);
    return dst.size() - start;
}

std::size_t Binary::PutUint32(std::string& dst, std::uint32_t value,
                              Order order) {
    const std::size_t start = dst.size();
    Store32(dst, value, order);
    return dst.size() - start;
}

std::size_t Binary::PutUint64(std::string& dst, std::uint64_t value,
                              Order order) {
    const std::size_t start = dst.size();
    Store64(dst, value, order);
    return dst.size() - start;
}

std::size_t Binary::PutInt16(std::string& dst, std::int16_t value,
                             Order order) {
    std::uint16_t raw = 0;
    std::memcpy(&raw, &value, sizeof(raw));
    return PutUint16(dst, raw, order);
}

std::size_t Binary::PutInt32(std::string& dst, std::int32_t value,
                             Order order) {
    std::uint32_t raw = 0;
    std::memcpy(&raw, &value, sizeof(raw));
    return PutUint32(dst, raw, order);
}

std::size_t Binary::PutInt64(std::string& dst, std::int64_t value,
                             Order order) {
    std::uint64_t raw = 0;
    std::memcpy(&raw, &value, sizeof(raw));
    return PutUint64(dst, raw, order);
}

std::size_t Binary::PutFloat32(std::string& dst, float value, Order order) {
    std::uint32_t raw = 0;
    std::memcpy(&raw, &value, sizeof(raw));
    return PutUint32(dst, raw, order);
}

std::size_t Binary::PutFloat64(std::string& dst, double value, Order order) {
    std::uint64_t raw = 0;
    std::memcpy(&raw, &value, sizeof(raw));
    return PutUint64(dst, raw, order);
}

/**
 * @brief 解码 LEB128 无符号变长整数。
 *
 * ## 步骤
 * step1. 逐字节扫描；出现第 11 个字节（`i == 10`）必定溢出；
 * step2. 遇到终止字节（`< 0x80`）：第 10 个字节必须 `<= 1`，否则溢出；
 *        否则组装低 7 位并返回 `bytes_read`；
 * step3. 续位字节：取低 7 位累加到 `value`，`shift += 7`；
 * step4. 输入在终止字节前耗尽（含恰好 10 个续位字节）→ 截断错误。
 */
Binary::UvarintResult Binary::Uvarint(std::string_view src) noexcept {
    UvarintResult result;
    std::uint64_t value = 0;
    unsigned shift = 0;
    // step1: 最多 10 字节；第 11 个字节出现即溢出
    for (std::size_t i = 0; i < src.size(); ++i) {
        if (i == 10) {
            // 第 11 个字节才出现：必然超过 64 位（对齐 Go 的 MaxVarintLen64）。
            result.error = detail::Tagged(
                "binary.Uvarint: varint overflows a 64-bit integer",
                ErrUvarintOverflow());
            return result;
        }
        const unsigned char byte = static_cast<unsigned char>(src[i]);
        // step2: 终止字节；第 10 字节只剩 1 位有效空间
        if (byte < 0x80) {
            if (i == 9 && byte > 1) {
                // 第 10 个字节只剩 1 位有效空间。
                result.error = detail::Tagged(
                    "binary.Uvarint: varint overflows a 64-bit integer",
                    ErrUvarintOverflow());
                return result;
            }
            value |= static_cast<std::uint64_t>(byte) << shift;
            result.value = value;
            result.bytes_read = i + 1;
            return result;
        }
        // step3: 续位字节，累加低 7 位
        value |= static_cast<std::uint64_t>(byte & 0x7f) << shift;
        shift += 7;
    }
    // step4: 输入在终止字节前耗尽。注意：恰好 10 个续位字节也走这里
    // （Go 返回 0，表示「缓冲区太小」而不是溢出，只有第 11 个字节才算溢出）。
    result.error =
        detail::Tagged("binary.Uvarint: unexpected end of varint",
                       ErrUvarintTruncated());
    return result;
}

/**
 * @brief 解码 zig-zag 编码的有符号变长整数。
 *
 * ## 步骤
 * step1. 复用 `Uvarint` 取出无符号位模式；
 * step2. 按 `x = (ux >> 1) ^ -(ux & 1)` 还原，用位取反避免有符号右移。
 */
Binary::VarintResult Binary::Varint(std::string_view src) noexcept {
    VarintResult result;
    // step1: 先按无符号 LEB128 解码
    const UvarintResult raw = Uvarint(src);
    if (!raw) {
        result.error = raw.error;
        return result;
    }
    // step2: zig-zag 还原：最低位是符号位
    std::uint64_t bits = raw.value >> 1;
    if ((raw.value & 1) != 0) {
        bits = ~bits;
    }
    std::int64_t value = 0;
    std::memcpy(&value, &bits, sizeof(value));
    result.value = value;
    result.bytes_read = raw.bytes_read;
    return result;
}

std::size_t Binary::PutUvarint(std::string& dst, std::uint64_t value) {
    const std::size_t start = dst.size();
    while (value >= 0x80) {
        dst.push_back(static_cast<char>((value & 0x7f) | 0x80));
        value >>= 7;
    }
    dst.push_back(static_cast<char>(value));
    return dst.size() - start;
}

std::size_t Binary::PutVarint(std::string& dst, std::int64_t value) {
    // zig-zag：负数映射到奇数。用显式的按位取反避免依赖有符号右移的实现定义行为。
    std::uint64_t encoded = static_cast<std::uint64_t>(value) << 1;
    if (value < 0) {
        encoded = ~encoded;
    }
    return PutUvarint(dst, encoded);
}

}  // namespace go2cpp::encoding
