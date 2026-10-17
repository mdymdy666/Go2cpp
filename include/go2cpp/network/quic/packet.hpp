/**
 * @file packet.hpp
 * @brief QUIC 包头的解析与构造（RFC 9000 §17）。
 *
 * ## 能力边界（**先读这一段**）
 *
 * 本模块只解析**明文可见**的包头字段：
 *
 * - Header Form（long / short）
 * - Version（含 Version Negotiation 的 version==0 特殊包）
 * - Destination / Source Connection ID
 * - Initial 包的 Token 与 Length
 * - 0-RTT / Handshake 的 Length
 * - Retry 包的 Retry Token（长度隐含）+ 尾部 16 字节完整性标签
 * - Short header 的 DCID（**长度需由连接状态提供**，包里没有）
 *
 * **不解析、也无法解析**的：
 * - **Packet Number 的真实值与长度** —— RFC 9001 §5.4 用 header protection
 *   对首字节的低 4 位与 packet number 本身做了加密。没有 header protection
 *   密钥，那几位是伪随机的。本模块只给出 `packet_number_offset`（PN 起始位置）
 *   和 `protected_packet_number_byte`（受保护的首字节），**明确不假装知道长度**。
 * - 任何 payload / 帧内容。
 *
 * ## 为什么不连 header protection 一起做
 *
 * header protection 需要 TLS 1.3 的密钥派生（HKDF-Expand-Label + AEAD），
 * 那属于 TLS 集成，不是包头解析。混在一起会让"包结构"这一层失去可独立验证性。
 * 当前实现刻意停在"结构"这一层，保证每个字段都能对照 RFC 逐字节验证。
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace go2cpp::net::quic {

/** @brief QUIC 版本号（RFC 9000 定义的是 v1）。 */
inline constexpr std::uint32_t kVersion1 = 0x00000001;
/** @brief Version Negotiation 包用的版本号（0）。 */
inline constexpr std::uint32_t kVersionNegotiation = 0x00000000;
/** @brief QUIC v1 的 Retry 完整性标签长度。 */
inline constexpr std::size_t kRetryIntegrityTagLength = 16;
/** @brief Connection ID 的最大长度（RFC 9000 §17.2：0..20 字节）。 */
inline constexpr std::size_t kMaxConnectionIdLength = 20;

/** @brief 包类型。 */
enum class PacketType : std::uint8_t {
    kInitial,
    kZeroRtt,
    kHandshake,
    kRetry,
    kVersionNegotiation,
    /// 1-RTT（short header）
    kOneRtt,
};

const char* ToString(PacketType type) noexcept;

/** @brief 解析状态。 */
enum class HeaderStatus : std::uint8_t {
    kOk,
    /// 数据不完整（增量解析必须区分这种情况）。
    kNeedMore,
    /// 结构非法：DCID/SCID 超长、Initial 缺 token、Retry 太短、fixed bit 为 0 等。
    kInvalid,
    /// 版本不受支持（当前只认 v1；未知版本应回 Version Negotiation）。
    kUnsupportedVersion,
};

const char* ToString(HeaderStatus status) noexcept;

/** @brief 解析出的包头（只含明文可见字段）。 */
struct PacketHeader final {
    bool is_long{false};
    PacketType type{PacketType::kInitial};
    std::uint32_t version{0};

    /// 原始首字节（long/short 共用的第一字节）。
    std::uint8_t first_byte{0};
    std::vector<std::uint8_t> dcid;
    std::vector<std::uint8_t> scid;

    /// Initial 包的 token（其他类型为空）。
    std::vector<std::uint8_t> token;
    /// long header 的 Length 字段（= packet number + payload 的字节数）。
    /// short header 无此字段，保持 0。
    std::uint64_t payload_length{0};
    /// Retry 包的完整性标签（16 字节；其他类型为空）。
    std::vector<std::uint8_t> retry_integrity_tag;
    /// Version Negotiation 包里列出的可用版本。
    std::vector<std::uint32_t> supported_versions;

    /// packet number 字段在**整个包缓冲**中的起始偏移。
    /// @note PN 内容被 header protection 加密，本结构不提供其值。
    std::size_t packet_number_offset{0};
};

/** @brief 解析结果。 */
struct HeaderDecoded final {
    HeaderStatus status{HeaderStatus::kNeedMore};
    PacketHeader header;
    /// 成功时消耗的字节数（= packet_number_offset，PN 之前的部分）。
    std::size_t consumed{0};
};

/**
 * @brief 解析一个 QUIC 包头。
 * @param in 输入缓冲。
 * @param length 可用字节数。
 * @param short_dcid_length **仅 short header 需要**：本端连接的 DCID 长度
 *        （RFC 9000 §17.3：1-RTT 包里不含 DCID 长度，接收方必须从连接状态得知）。
 *        传 0 或超过 20 视为参数错误。
 * @return 状态与解析结果。
 * @note `kNeedMore` 时 `consumed == 0`，调用方可等更多数据后从同一位置重试。
 */
HeaderDecoded DecodePacketHeader(const std::uint8_t* in, std::size_t length,
                                 std::size_t short_dcid_length = 8) noexcept;

// ---------------------------------------------------------------------------
// 构造（仅用于测试与抓包回放；生产发送需要 header protection 密钥）
// ---------------------------------------------------------------------------

/** @brief 构造一个 long header 包（不含 payload）。 */
struct LongHeaderSpec final {
    PacketType type{PacketType::kInitial};
    std::uint32_t version{kVersion1};
    std::vector<std::uint8_t> dcid;
    std::vector<std::uint8_t> scid;
    /// 仅 Initial 使用。
    std::vector<std::uint8_t> token;
    /// 仅 Initial / 0-RTT / Handshake 使用。
    std::uint64_t payload_length{0};
    /// 仅 Retry 使用（必须 16 字节）。
    std::vector<std::uint8_t> retry_integrity_tag;
};

/** @brief 序列化 long header；失败返回空 vector。 */
std::vector<std::uint8_t> EncodeLongHeader(const LongHeaderSpec& spec);

/** @brief 构造 Version Negotiation 包（version 字段为 0）。 */
std::vector<std::uint8_t> EncodeVersionNegotiation(
    const std::vector<std::uint8_t>& dcid, const std::vector<std::uint8_t>& scid,
    const std::vector<std::uint32_t>& supported_versions);

/** @brief 构造 short header 包（1-RTT）；`packet_number` 只用于占位，不加密。 */
std::vector<std::uint8_t> EncodeShortHeader(const std::vector<std::uint8_t>& dcid,
                                            std::uint64_t packet_number,
                                            std::uint8_t pn_length = 4);

}  // namespace go2cpp::net::quic
