#pragma once

/**
 * @file stun_message.hpp
 * @brief RFC 5389 STUN 消息的编解码（只覆盖本模块用到的子集）。
 * @details 支持范围：
 *          - Binding 方法（Request / Success Response / Error Response）；
 *          - XOR-MAPPED-ADDRESS、MAPPED-ADDRESS、OTHER-ADDRESS、
 *            RESPONSE-ORIGIN、CHANGE-REQUEST、ERROR-CODE、SOFTWARE、
 *            USERNAME、DATA、LIFETIME 等属性的 TLV 编解码；
 *          - 未知属性原样保留（`Find()` 可以取到），不会因为多一个属性而
 *            拒绝整条消息。
 *
 *          **刻意不实现**：
 *          - MESSAGE-INTEGRITY / FINGERPRINT 的校验与生成（需要 HMAC-SHA1
 *            与 CRC32；本模块的 STUN 只用于地址发现与连通性检查，不做鉴权）；
 *          - TURN 的 long-term credential（本模块的 TURN 是自定义中继协议，
 *            不是 RFC 5766 的完整实现）。
 *
 *          这两点在 `docs/modules/p2p-nat-traversal.md` 的「限制」一节有说明：
 *          部署在公网时应由反向代理/防火墙限制 STUN 服务只被可信网段访问。
 *
 *          类型字段的编码沿 RFC 5389 §6：method 与 class 的比特交错排列，
 *          见 `EncodeStunType()` 的注释。
 *
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "go2cpp/p2p/endpoint.hpp"

namespace go2cpp::p2p {

/// STUN magic cookie（RFC 5389 §6）：`0x2112A442`。
inline constexpr std::uint32_t kStunMagicCookie = 0x2112A442U;
/// 消息头长度：type(2) + length(2) + cookie(4) + transaction id(12)。
inline constexpr std::size_t kStunHeaderSize = 20;
/// 事务 ID 长度。
inline constexpr std::size_t kStunTransactionIdSize = 12;
/// 单个属性值上限（长度字段是 16 位）。
inline constexpr std::size_t kStunMaxAttributeValue = 65535;
/// 整条消息上限（UDP 载荷上限为 65507；这里留出余量）。
inline constexpr std::size_t kStunMaxMessageSize = 65535;
/// XOR-MAPPED-ADDRESS 端口掩码（RFC 5389 §15.2）。
inline constexpr std::uint16_t kStunXorPortMask = 0x2112U;

/// Binding 方法号。
inline constexpr std::uint16_t kStunMethodBinding = 0x0001U;

/// MAPPED-ADDRESS（RFC 3489 旧属性）。
inline constexpr std::uint16_t kStunAttributeMappedAddress = 0x0001U;
/// CHANGE-REQUEST（RFC 3489，仅 32 位低两位有效）。
inline constexpr std::uint16_t kStunAttributeChangeRequest = 0x0003U;
/// USERNAME。
inline constexpr std::uint16_t kStunAttributeUsername = 0x0006U;
/// MESSAGE-INTEGRITY（本模块只保留，不校验）。
inline constexpr std::uint16_t kStunAttributeMessageIntegrity = 0x0008U;
/// ERROR-CODE。
inline constexpr std::uint16_t kStunAttributeErrorCode = 0x0009U;
/// UNKNOWN-ATTRIBUTES。
inline constexpr std::uint16_t kStunAttributeUnknownAttributes = 0x000AU;
/// CHANNEL-NUMBER（TURN 预留）。
inline constexpr std::uint16_t kStunAttributeChannelNumber = 0x000CU;
/// LIFETIME（TURN 预留）。
inline constexpr std::uint16_t kStunAttributeLifetime = 0x000DU;
/// XOR-PEER-ADDRESS（TURN 预留）。
inline constexpr std::uint16_t kStunAttributeXorPeerAddress = 0x0012U;
/// DATA（TURN 预留）。
inline constexpr std::uint16_t kStunAttributeData = 0x0013U;
/// REALM（TURN 预留）。
inline constexpr std::uint16_t kStunAttributeRealm = 0x0014U;
/// NONCE（TURN 预留）。
inline constexpr std::uint16_t kStunAttributeNonce = 0x0015U;
/// XOR-RELAYED-ADDRESS（TURN 预留）。
inline constexpr std::uint16_t kStunAttributeXorRelayedAddress = 0x0016U;
/// REQUESTED-TRANSPORT（TURN 预留）。
inline constexpr std::uint16_t kStunAttributeRequestedTransport = 0x0019U;
/// XOR-MAPPED-ADDRESS（RFC 5389）。
inline constexpr std::uint16_t kStunAttributeXorMappedAddress = 0x0020U;
/// SOFTWARE。
inline constexpr std::uint16_t kStunAttributeSoftware = 0x8022U;
/// FINGERPRINT（本模块只保留，不校验）。
inline constexpr std::uint16_t kStunAttributeFingerprint = 0x8028U;
/// RESPONSE-ORIGIN（RFC 5780：响应实际使用的源地址）。
inline constexpr std::uint16_t kStunAttributeResponseOrigin = 0x802BU;
/// OTHER-ADDRESS（RFC 5780：服务器的备用地址）。
inline constexpr std::uint16_t kStunAttributeOtherAddress = 0x802CU;

/// CHANGE-REQUEST 的 change-IP 位。
inline constexpr std::uint32_t kStunChangeIp = 0x04U;
/// CHANGE-REQUEST 的 change-port 位。
inline constexpr std::uint32_t kStunChangePort = 0x02U;

/// STUN 消息类（RFC 5389 §6）。
enum class StunClass : std::uint8_t {
    kRequest = 0,
    kIndication = 1,
    kSuccessResponse = 2,
    kErrorResponse = 3,
};

/// 一个 STUN TLV 属性；`value` 已去掉 4 字节对齐填充。
struct StunAttribute {
    std::uint16_t type = 0;
    std::vector<std::uint8_t> value;
};

/**
 * @brief 一条 STUN 消息的内存表示。
 *
 * @details `transaction_id` 在构造时由 `MakeBindingRequest()` 随机生成；
 *          解析得到的消息保留原始事务 ID。`XOR-MAPPED-ADDRESS` 的 IPv6
 *          编码依赖事务 ID，因此 `AddAddress(..., xored=true)` 必须在
 *          事务 ID 确定之后调用。
 */
struct StunMessage {
    std::uint16_t method = kStunMethodBinding;
    StunClass klass = StunClass::kRequest;
    std::array<std::uint8_t, kStunTransactionIdSize> transaction_id{};
    std::vector<StunAttribute> attributes;

    /// 构造一个随机事务 ID 的 Binding Request（带 SOFTWARE）。
    static StunMessage MakeBindingRequest();

    /// 构造 Binding Success Response（XOR-MAPPED-ADDRESS + MAPPED-ADDRESS + SOFTWARE）。
    static StunMessage MakeBindingSuccess(const std::array<std::uint8_t, kStunTransactionIdSize>& transaction_id,
                                          const Endpoint& mapped);

    /// 构造 Binding Error Response；`code` 形如 400/420，`reason` 为 UTF-8 原因短语。
    static StunMessage MakeBindingError(const std::array<std::uint8_t, kStunTransactionIdSize>& transaction_id,
                                        std::uint16_t code, std::string_view reason);

    /// 查找第一个指定类型的属性；不存在返回 nullptr。
    const StunAttribute* Find(std::uint16_t type) const noexcept;

    /// 追加一个属性；长度超过 65535 或 `length > 0 && data == nullptr` 时失败。
    bool AddAttribute(std::uint16_t type, const void* data, std::size_t length);

    /// 追加一个 32 位大端属性。
    bool AddU32(std::uint16_t type, std::uint32_t value);

    /// 追加字符串属性（不含结尾 NUL）。
    bool AddString(std::uint16_t type, std::string_view text);

    /**
     * @brief 追加地址属性。
     * @param xored true 时按 XOR-MAPPED-ADDRESS 规则编码（IPv4 用 cookie，
     *        IPv6 用 cookie + transaction id 做掩码）。
     */
    bool AddAddress(std::uint16_t type, const Endpoint& endpoint, bool xored);

    /// 读取 32 位大端属性；不存在或长度不是 4 时返回 nullopt。
    std::optional<std::uint32_t> GetU32(std::uint16_t type) const;

    /// 读取地址属性；`xored` 必须与属性类型匹配。
    std::optional<Endpoint> GetAddress(std::uint16_t type, bool xored) const;

    /// 优先返回 XOR-MAPPED-ADDRESS，退化到 MAPPED-ADDRESS。
    std::optional<Endpoint> GetMappedAddress() const;

    /// 解析 ERROR-CODE 属性；返回是否成功。
    bool GetErrorCode(std::uint16_t* code, std::string* reason) const;
};

/// 组合 method 与 class 为 16 位 type（RFC 5389 §6 的比特交错布局）。
std::uint16_t EncodeStunType(std::uint16_t method, StunClass klass) noexcept;

/// 从 16 位 type 解出 method。
std::uint16_t DecodeStunMethod(std::uint16_t type) noexcept;

/// 从 16 位 type 解出 class。
StunClass DecodeStunClass(std::uint16_t type) noexcept;

/**
 * @brief 序列化一条 STUN 消息。
 * @param error 失败时写入原因（可为空）。
 * @return 线格式字节；失败返回空 vector。
 */
std::vector<std::uint8_t> SerializeStunMessage(const StunMessage& message, std::string* error = nullptr);

/**
 * @brief 反序列化一条 STUN 消息。
 * @param data 收到的字节。
 * @param length 字节数。
 * @param error 失败时写入原因（可为空）。
 * @return 成功返回消息；头不完整、magic cookie 不符、长度字段越界、
 *         属性截断时返回 nullopt。
 * @note 严格校验「头部 length 字段 == 实际字节数 - 20」，因此带尾部垃圾的
 *       数据报会被拒绝而不是被宽容解析。
 */
std::optional<StunMessage> ParseStunMessage(const void* data, std::size_t length,
                                            std::string* error = nullptr);

/// 消息类名（用于日志/诊断）。
std::string StunClassName(StunClass klass);

}  // namespace go2cpp::p2p
