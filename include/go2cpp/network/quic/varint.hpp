/**
 * @file varint.hpp
 * @brief QUIC 可变长度整数（RFC 9000 §16）。
 *
 * ## 这是 QUIC 的地基
 *
 * QUIC 里几乎所有数值字段都是 varint：包号的长度前缀、帧类型后的长度、
 * `max_udp_payload_size`、stream ID、offset……一旦这里的边界写错，
 * **上层所有字段都会错位**，而错误表现往往是"偶发解析失败"而非立即崩溃。
 *
 * ## 规格
 *
 * | 高 2 位 | 字节数 | 可用位 | 最大值 |
 * |---|---|---|---|
 * | `00` | 1 | 6 | 63 |
 * | `01` | 2 | 14 | 16383 |
 * | `10` | 4 | 30 | 1073741823 |
 * | `11` | 8 | 62 | 4611686018427387903 (2^62-1) |
 *
 * 编码器总是选**能装下的最短长度**；解码器**接受非最短编码**
 * （RFC 不要求发送方用最短，接收方必须宽容）。
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace go2cpp::net::quic {

/** @brief varint 最大可表示值（2^62 - 1）。 */
inline constexpr std::uint64_t kVarIntMax = (std::uint64_t{1} << 62) - 1;

/** @brief 编解码状态。 */
enum class VarIntStatus : std::uint8_t {
    kOk,
    /// 输入字节不足，需要更多数据（增量解析必须区分这种情况）。
    kNeedMore,
    /// 值超出 2^62-1（仅编码时可能）。
    kTooLarge,
};

const char* ToString(VarIntStatus status) noexcept;

/** @brief 编码结果。 */
struct VarIntEncoded final {
    VarIntStatus status{VarIntStatus::kOk};
    /// 实际写入字节数；失败时为 0。
    std::size_t size{0};
};

/**
 * @brief 编码一个 varint 到 `out`，用能装下的最短长度。
 * @param out 调用方保证至少 8 字节可写。
 */
VarIntEncoded EncodeVarInt(std::uint64_t value, std::uint8_t* out) noexcept;

/** @brief 解码结果。 */
struct VarIntDecoded final {
    VarIntStatus status{VarIntStatus::kNeedMore};
    std::uint64_t value{0};
    /// 成功时消耗的字节数（1/2/4/8）。
    std::size_t consumed{0};
};

/**
 * @brief 从 `in` 解码一个 varint。
 * @return 不足时返回 `kNeedMore` 且 `consumed == 0`，调用方可从同一位置重试。
 */
VarIntDecoded DecodeVarInt(const std::uint8_t* in, std::size_t length) noexcept;

/** @brief 某个值用最短编码需要多少字节（1/2/4/8）。 */
std::size_t VarIntEncodedSize(std::uint64_t value) noexcept;

}  // namespace go2cpp::net::quic
