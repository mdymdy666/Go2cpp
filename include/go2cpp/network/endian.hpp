#pragma once

/**
 * @file endian.hpp
 * @brief 网络字节序和主机字节序转换工具。
 * @details 该文件只包含无状态的整数转换函数，不依赖 Socket 或调度器，
 *          协议层可以替换为自己的编码策略。
 *
 * ## 模块定位
 * 无状态字节序转换：host <-> big-endian（网络字节序）。被 `address`、协议编码与
 * socket 层使用；不依赖 Socket 或调度器。
 *
 * ## 本文件定义什么
 * - `byteswap16/32/64`：纯位运算的字节交换；
 * - `host_to_be*` / `be_to_host*`：按 `__BYTE_ORDER__` 条件编译的转换。
 *
 * ## 阅读建议
 * 先看 `byteswap*`（纯位运算、可 constexpr），再看 `host_to_be*` 的条件编译分支。
 *
 * ## 边界与陷阱
 * - 依赖编译器预定义的 `__BYTE_ORDER__` / `__ORDER_LITTLE_ENDIAN__`（GCC/Clang 提供）；
 *   平台不定义时会走「保持不变」分支 —— 在大端机上正确，在其它情况下可能静默出错；
 * - `be_to_host*` 与 `host_to_be*` 是同一个交换（字节交换自反），保留两个名字只为可读性；
 * - 小端主机上 `host_to_be32(x) == byteswap32(x)`，转换路径上**不要重复交换**。
 */

#include <cstdint>

namespace go2cpp::net {

/** @brief 交换 16 位整数的高低字节。 */
constexpr std::uint16_t byteswap16(std::uint16_t value) noexcept {
    return static_cast<std::uint16_t>((value >> 8) | (value << 8));
}

/** @brief 交换 32 位整数的字节。 */
constexpr std::uint32_t byteswap32(std::uint32_t value) noexcept {
    return ((value & 0x000000ffU) << 24) |
           ((value & 0x0000ff00U) << 8) |
           ((value & 0x00ff0000U) >> 8) |
           ((value & 0xff000000U) >> 24);
}

/** @brief 交换 64 位整数的字节。 */
constexpr std::uint64_t byteswap64(std::uint64_t value) noexcept {
    return ((value & 0x00000000000000ffULL) << 56) |
           ((value & 0x000000000000ff00ULL) << 40) |
           ((value & 0x0000000000ff0000ULL) << 24) |
           ((value & 0x00000000ff000000ULL) << 8) |
           ((value & 0x000000ff00000000ULL) >> 8) |
           ((value & 0x0000ff0000000000ULL) >> 24) |
           ((value & 0x00ff000000000000ULL) >> 40) |
           ((value & 0xff00000000000000ULL) >> 56);
}

/** @brief 在小端主机上转换为大端，在大端主机上保持不变。 */
constexpr std::uint16_t host_to_be16(std::uint16_t value) noexcept {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return byteswap16(value);
#else
    return value;
#endif
}
/**
 * @brief host -> big-endian 的 32 位转换。
 *
 * ## 说明
 * 小端主机上等于 `byteswap32`，大端主机上原样返回；三档位宽各自条件编译，
 * 避免运行期分支（`constexpr` 可用于常量初始化）。
 */
constexpr std::uint32_t host_to_be32(std::uint32_t value) noexcept {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return byteswap32(value);
#else
    return value;
#endif
}
constexpr std::uint64_t host_to_be64(std::uint64_t value) noexcept {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return byteswap64(value);
#else
    return value;
#endif
}
constexpr std::uint16_t be_to_host16(std::uint16_t value) noexcept {
    return host_to_be16(value);
}
constexpr std::uint32_t be_to_host32(std::uint32_t value) noexcept {
    return host_to_be32(value);
}
constexpr std::uint64_t be_to_host64(std::uint64_t value) noexcept {
    return host_to_be64(value);
}

}  // namespace go2cpp::net

namespace go2cpp {
using net::be_to_host16;
using net::be_to_host32;
using net::be_to_host64;
using net::host_to_be16;
using net::host_to_be32;
using net::host_to_be64;
}  // namespace go2cpp
