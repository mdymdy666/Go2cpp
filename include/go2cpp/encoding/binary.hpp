/**
 * @file binary.hpp
 * @brief Go `encoding/binary` 的 C++ 对应实现（定长大端/小端 + LEB128 变长整数）。
 *
 * @author DeepSeek Harness
 * @date 2026-10-18
 * @copyright BSD-3-Clause（语义对齐 Go 标准库；本文件为独立实现）
 *
 * @details 复用优先的结论（检索日期 2026-10-18）：
 *   | 候选 | 结论 |
 *   |---|---|
 *   | Go 标准库 `encoding/binary`（Go 1.23.0，BSD-3-Clause） | **语义参照**：
 *     `ByteOrder` 的定长读写、`PutUvarint`/`Uvarint`/`PutVarint`/`Varint` 的
 *     LEB128 与 zig-zag 规则、溢出/截断判定都按它对齐 |
 *   | Boost.Endian / `boost::endian` | **不采用**：项目已有 Boost 依赖（仅 context），
 *     但 `boost::endian` 只管定长字节序，不提供 LEB128；且其 `endian_buffer`
 *     面向内存缓冲，与「从 `string_view` 读、往 `string` 追加」的接口不匹配 |
 *   | protobuf 的 `CodedInputStream` / `Varint` 内部实现 | **不采用**：依赖整个
 *     protobuf 运行时，且溢出语义是「返回 false」而非项目需要的错误锚点 |
 *   | 自研 | **采用**：定长读写只是位移与 `memcpy`，LEB128 只有几十行；关键收益是
 *     **短输入不越界**（Go 会 panic，本模块返回 `ErrShortBuffer()` 错误） |
 *
 * ## 关键语义
 *
 * - **只做「从字节串读」与「往字节串追加」**，不实现 `io.Reader`/`io.Writer` 版本
 *   （`binary.Read`/`binary.Write` 依赖反射，C++ 侧没有对应物，且需求未要求）。
 * - 读取函数在输入长度不足时返回包装了 `ErrShortBuffer()` 的错误，**绝不越界**；
 *   `value` 为 0。Go 对应接口在切片过短时会 panic，这是本模块的刻意差异。
 * - 写入函数把字节**追加**到 `dst` 末尾并返回追加长度（2/4/8），不要求调用方
 *   预分配；这与 Go `PutUint16(b []byte, v)` 写入 `b[0:2]` 的形态不同，理由同 base64。
 * - 字节序用 `Binary::Order` 显式传入，默认 `BigEndian`；`Binary::BigEndian` /
 *   `Binary::LittleEndian` 两个常量让调用点保持 Go 风格。
 * - 有符号数按**补码**解释；浮点数按 IEEE-754 位模式原样搬运（`memcpy`），
 *   NaN 的 payload 不会丢失。
 * - **LEB128**：`Uvarint` 最多 10 字节；第 10 字节必须 `<= 0x01`，否则
 *   `ErrUvarintOverflow()`；输入在终止字节前耗尽返回 `ErrUvarintTruncated()`。
 *   注意 Go 的细节：**恰好 10 个字节且全部带续位**时返回「缓冲区太小」而不是
 *   「溢出」，只有出现第 11 个字节才判溢出；本实现保持该判定顺序。
 * - `Varint` 用 zig-zag：`ux = x<<1`，负数再按位取反；解码
 *   `x = (ux >> 1) ^ -(ux & 1)`，与 Go `PutVarint`/`Varint` 完全一致。
 *
 * ## 模块定位
 *
 * `encoding` 的二进制整数层：定长整数的字节序读写，以及 LEB128/zig-zag
 * 变长整数。上游是协议帧解析、序列化与消息编解码，下游只有
 * `go2cpp::tools::error`。实现全部在 `src/encoding/binary.cpp`。
 *
 * ## 本文件定义什么
 *
 * - `ErrShortBuffer` / `ErrUvarintOverflow` / `ErrUvarintTruncated`：错误锚点；
 * - `BinaryValue<T>`：定长读取的 `(value, error)` 返回；
 * - `Binary`：`Order` 枚举、定长读写、`Uvarint`/`Varint` 及其 Result。
 *
 * ## 阅读建议
 *
 * 先读「关键语义」中「短输入返回错误而不是 panic」「写入是追加」两条；
 * 再看 `Binary` 的不变量，最后看 .cpp 里 `Uvarint` 的 10 字节边界。
 *
 * ## 边界与陷阱
 *
 * - 读取函数永不越界，短输入返回 `ErrShortBuffer()` 且 `value == 0`；
 * - 写入函数把字节追加到 `dst`，返回本次追加长度，不要求预分配；
 * - 恰好 10 个续位字节返回「截断」而不是「溢出」，第 11 个字节才判溢出。
 */

#pragma once

#include "go2cpp/tools/error.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace go2cpp::encoding {

/// 对应 Go `encoding/binary` 读取时「缓冲区太小」的错误锚点。
inline ErrorPtr ErrShortBuffer() {
    static const ErrorPtr* const kError =
        new ErrorPtr(NewError("encoding/binary: buffer too small"));
    return *kError;
}

/// LEB128 无符号变长整数超过 64 位时的错误锚点。
inline ErrorPtr ErrUvarintOverflow() {
    static const ErrorPtr* const kError =
        new ErrorPtr(NewError("encoding/binary: varint overflows a 64-bit integer"));
    return *kError;
}

/// LEB128 输入在终止字节前耗尽（截断）时的错误锚点。
inline ErrorPtr ErrUvarintTruncated() {
    static const ErrorPtr* const kError =
        new ErrorPtr(NewError("encoding/binary: unexpected end of varint"));
    return *kError;
}

/**
 * @brief 定长读取的返回值：`value` + 错误。
 * @tparam T 读取的类型（`std::uint16_t` 等）。
 *
 * ## 定位
 * `Binary::UintXX`/`IntXX`/`FloatXX` 的统一返回类型；调用方用 `operator bool`
 * 判成功，失败时 `value` 为 `T{}`、`error` 为 `ErrShortBuffer()` 的包装。
 */
template <typename T>
struct BinaryValue {
    /// 读取结果；失败时为 `T{}`。
    T value{};
    /// 成功为 `nullptr`。
    ErrorPtr error;

    /// 便于 `if (auto r = Binary::Uint32(...))` 直接判断成功。
    explicit operator bool() const noexcept { return error == nullptr; }
};

/**
 * @brief Go `encoding/binary` 的静态函数集合。
 *
 * @details 与 `Hex` 相同，使用 `struct Binary` 让调用点保持
 *          `encoding::Binary::Uint32(...)` 的形态。
 *
 * ## 定位
 * `encoding` 的二进制整数工具集，无状态、纯静态；被协议帧解析与序列化模块
 * 调用。字节序由调用方显式传入，避免依赖宿主端序。
 *
 * ## 不变量
 * - 所有读取都先检查长度，绝不越界；不足时返回 `ErrShortBuffer()`；
 * - 所有写入只追加到 `dst` 末尾，返回本次追加的字节数；
 * - `Uvarint` 最多消耗 10 字节；`Varint` 是 `Uvarint` 的 zig-zag 映射；
 * - 有符号数按补码、浮点数按 IEEE-754 位模式原样搬运。
 */
struct Binary {
    /// 字节序。对应 Go 的 `binary.BigEndian` / `binary.LittleEndian`。
    enum class Order : std::uint8_t {
        kBigEndian,   ///< 大端：高位字节在前（网络字节序）。
        kLittleEndian ///< 小端：低位字节在前（x86/ARM 原生序）。
    };

    /// Go 风格常量：`Binary::BigEndian`。
    static constexpr Order BigEndian = Order::kBigEndian;
    /// Go 风格常量：`Binary::LittleEndian`。
    static constexpr Order LittleEndian = Order::kLittleEndian;

    /// 定长读取结果别名。
    using Uint16Result = BinaryValue<std::uint16_t>;
    using Uint32Result = BinaryValue<std::uint32_t>;
    using Uint64Result = BinaryValue<std::uint64_t>;
    using Int16Result = BinaryValue<std::int16_t>;
    using Int32Result = BinaryValue<std::int32_t>;
    using Int64Result = BinaryValue<std::int64_t>;
    using Float32Result = BinaryValue<float>;
    using Float64Result = BinaryValue<double>;

    /// 读取大端/小端 `uint16`；输入不足 2 字节返回 `ErrShortBuffer()`。
    static Uint16Result Uint16(std::string_view src,
                               Order order = Order::kBigEndian) noexcept;
    /// 读取大端/小端 `uint32`；输入不足 4 字节返回 `ErrShortBuffer()`。
    static Uint32Result Uint32(std::string_view src,
                               Order order = Order::kBigEndian) noexcept;
    /// 读取大端/小端 `uint64`；输入不足 8 字节返回 `ErrShortBuffer()`。
    static Uint64Result Uint64(std::string_view src,
                               Order order = Order::kBigEndian) noexcept;
    /// 读取 `int16`（补码）；输入不足 2 字节返回 `ErrShortBuffer()`。
    static Int16Result Int16(std::string_view src,
                             Order order = Order::kBigEndian) noexcept;
    /// 读取 `int32`（补码）；输入不足 4 字节返回 `ErrShortBuffer()`。
    static Int32Result Int32(std::string_view src,
                             Order order = Order::kBigEndian) noexcept;
    /// 读取 `int64`（补码）；输入不足 8 字节返回 `ErrShortBuffer()`。
    static Int64Result Int64(std::string_view src,
                             Order order = Order::kBigEndian) noexcept;
    /// 读取 IEEE-754 `float`；输入不足 4 字节返回 `ErrShortBuffer()`。
    static Float32Result Float32(std::string_view src,
                                 Order order = Order::kBigEndian) noexcept;
    /// 读取 IEEE-754 `double`；输入不足 8 字节返回 `ErrShortBuffer()`。
    static Float64Result Float64(std::string_view src,
                                 Order order = Order::kBigEndian) noexcept;

    /// 追加 `uint16`，返回追加字节数（2）。
    static std::size_t PutUint16(std::string& dst, std::uint16_t value,
                                 Order order = Order::kBigEndian);
    /// 追加 `uint32`，返回追加字节数（4）。
    static std::size_t PutUint32(std::string& dst, std::uint32_t value,
                                 Order order = Order::kBigEndian);
    /// 追加 `uint64`，返回追加字节数（8）。
    static std::size_t PutUint64(std::string& dst, std::uint64_t value,
                                 Order order = Order::kBigEndian);
    /// 追加 `int16`，返回追加字节数（2）。
    static std::size_t PutInt16(std::string& dst, std::int16_t value,
                                Order order = Order::kBigEndian);
    /// 追加 `int32`，返回追加字节数（4）。
    static std::size_t PutInt32(std::string& dst, std::int32_t value,
                                Order order = Order::kBigEndian);
    /// 追加 `int64`，返回追加字节数（8）。
    static std::size_t PutInt64(std::string& dst, std::int64_t value,
                                Order order = Order::kBigEndian);
    /// 追加 IEEE-754 `float`，返回追加字节数（4）。
    static std::size_t PutFloat32(std::string& dst, float value,
                                  Order order = Order::kBigEndian);
    /// 追加 IEEE-754 `double`，返回追加字节数（8）。
    static std::size_t PutFloat64(std::string& dst, double value,
                                  Order order = Order::kBigEndian);

    /// `Uvarint` 的返回值：对应 Go 的 `(uint64, int)`。
    /// ## 定位：`Uvarint()` 的返回类型；`bytes_read` 供调用方推进输入偏移。
    struct UvarintResult {
        /// 解出的值；失败时为 0。
        std::uint64_t value = 0;
        /// 成功时消耗的字节数（1..10）；失败时为 0。
        std::size_t bytes_read = 0;
        /// 成功为 `nullptr`。
        ErrorPtr error;

        /// 便于直接判断成功。
        explicit operator bool() const noexcept { return error == nullptr; }
    };

    /// `Varint` 的返回值：对应 Go 的 `(int64, int)`。
    /// ## 定位：`Varint()` 的返回类型；`bytes_read` 供调用方推进输入偏移。
    struct VarintResult {
        /// 解出的值；失败时为 0。
        std::int64_t value = 0;
        /// 成功时消耗的字节数（1..10）；失败时为 0。
        std::size_t bytes_read = 0;
        /// 成功为 `nullptr`。
        ErrorPtr error;

        /// 便于直接判断成功。
        explicit operator bool() const noexcept { return error == nullptr; }
    };

    /// 解码 LEB128 无符号变长整数。
    static UvarintResult Uvarint(std::string_view src) noexcept;

    /// 解码 zig-zag 编码的有符号变长整数。
    static VarintResult Varint(std::string_view src) noexcept;

    /// 追加 LEB128 编码，返回追加字节数（1..10）。
    static std::size_t PutUvarint(std::string& dst, std::uint64_t value);

    /// 追加 zig-zag + LEB128 编码，返回追加字节数（1..10）。
    static std::size_t PutVarint(std::string& dst, std::int64_t value);
};

/// 便捷别名：`encoding::ByteOrder`。
using ByteOrder = Binary::Order;

}  // namespace go2cpp::encoding
