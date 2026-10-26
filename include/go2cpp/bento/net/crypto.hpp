#pragma once

/**
 * @file crypto.hpp
 * @brief Bento 的 PSK 认证加密层：ChaCha20-Poly1305（RFC 8439）、
 *        HChaCha20 密钥派生、64 位滑动重放窗口与加密帧封装。
 * @details 设计目标：
 *
 *          - **无第三方库**：ChaCha20 块函数、Poly1305 MAC、HChaCha20 KDF
 *            全部在本模块内实现，便于审计与交叉编译。
 *          - **PSK 模式**：双方预共享 32B 密钥。HELLO/HELLO_ACK 携带
 *            16B 随机 nonce，用 PSK 派生的握手密钥做 AEAD；双方 nonce
 *            共同派生会话方向密钥，后续每个数据帧都用会话密钥加密。
 *          - **防重放**：每方向独立的 64 位滑动窗口（IPsec 风格）。
 *            帧头保留明文的 wire seq 作为 AEAD nonce 与窗口索引；
 *            只有通过 Poly1305 验证的包才会推进窗口，伪造高序号包
 *            无法把窗口推高（避免 DoS）。
 *          - **帧头认证**：加密帧外层 12B 头作为 AEAD 的 AAD；密文内是
 *            完整的 Bento 内层帧（12B 头 + 可选 ACK 段 + 载荷）。接收端
 *            解密后再走原有 ParseFrame/通道逻辑，协议语义零改动。
 *
 *          线上加密帧格式（外层头 12B + 密文 + 16B tag）：
 *
 *          | 偏移 | 字段    | 语义                                      |
 *          |------|---------|-------------------------------------------|
 *          | 0    | ver=1   | 协议版本                                  |
 *          | 1    | type    | 内层帧类型（HELLO/STREAM/...）             |
 *          | 2    | chan    | 内层通道                                  |
 *          | 3    | flags   | 内层 flags | kFlagEncrypted (0x10)         |
 *          | 4    | seq     | 每方向单调递增的 wire seq（从 1 开始）     |
 *          | 8    | dep_seq | 外层保留 0（内层 dep_seq 在密文里）        |
 *          | 12   | ct      | 内层完整帧的 ChaCha20 密文                 |
 *          | ...  | tag     | Poly1305 16B                              |
 *
 *          nonce = 方向标签 u32 BE || wire seq u64 BE（12B）。方向密钥
 *          本身已由 KDF 分离，这里的标签进一步保证两端 nonce 空间不重叠。
 *
 * @author go2cpp contributors
 * @date 2026-10-26
 * @copyright MIT License
 */

#include <cstddef>
#include <cstdint>
#include <vector>

#include "go2cpp/bento/net/protocol.hpp"

namespace go2cpp::bento {

// 加密帧标志 kFlagEncrypted 定义在 protocol.hpp（帧标志属于协议层）。

/// ChaCha20 密钥字节数。
inline constexpr std::size_t kChaCha20KeyBytes = 32;

/// ChaCha20 nonce 字节数。
inline constexpr std::size_t kChaCha20NonceBytes = 12;

/// Poly1305 / AEAD 认证标签字节数。
inline constexpr std::size_t kAeadTagBytes = 16;

/// PSK 字节数（固定 32B，便于直接用 HChaCha20 派生）。
inline constexpr std::size_t kPskBytes = 32;

/// 握手 nonce 字节数。
inline constexpr std::size_t kHandshakeNonceBytes = 16;

/// 加密外层帧除密文外的固定开销：16B 头 + 16B tag。
inline constexpr std::size_t kSecureOverheadBytes = kFrameHeaderBytes + kAeadTagBytes;

/// 方向标签（也写进 AEAD nonce 的前 4 字节）。
enum class SecureDirection : std::uint32_t {
    kClientToServer = 1,  ///< 客户端发送、监听方接收。
    kServerToClient = 2,  ///< 监听方发送、客户端接收。
};

/// 握手消息版本。
inline constexpr std::uint8_t kSecureHandshakeVersion = 1;

/// HELLO / HELLO_ACK 统一 magic：'B' 'E' 'N' 'T'。
inline constexpr std::uint8_t kSecureMagic[4] = {'B', 'E', 'N', 'T'};

/// 握手消息明文长度（加密前的帧载荷）。
inline constexpr std::size_t kHelloBodyBytes = 4 + 1 + 1 + 2 + 4 + kHandshakeNonceBytes + 8;
inline constexpr std::size_t kHelloAckBodyBytes =
    4 + 1 + 1 + 2 + 4 + kHandshakeNonceBytes + kHandshakeNonceBytes + 8;

/// @brief 握手 HELLO 明文体（36B）。
struct HelloBody {
    std::uint32_t session = 0;                      ///< 发起方 session id。
    std::uint8_t nonce[kHandshakeNonceBytes] = {};  ///< 16B 随机 nonce。
    std::uint64_t timestamp_ms = 0;                 ///< 发送时刻，防旧包重放。
};

/// @brief 握手 HELLO_ACK 明文体（52B）。
struct HelloAckBody {
    std::uint32_t session = 0;
    std::uint8_t client_nonce[kHandshakeNonceBytes] = {};  ///< 回显，绑定请求。
    std::uint8_t server_nonce[kHandshakeNonceBytes] = {};  ///< 监听方随机 nonce。
    std::uint64_t timestamp_ms = 0;
};

/// @brief 加密帧打开结果。
enum class OpenStatus : std::uint8_t {
    kOk = 0,
    kMalformed = 1,     ///< 长度/标志/版本非法。
    kReplay = 2,        ///< wire seq 落在窗口外或已接收。
    kAuthFailed = 3,    ///< Poly1305 校验失败（篡改/错误密钥）。
    kNotEncrypted = 4,  ///< 帧未置 kFlagEncrypted。
};

/// @brief 生成密码学随机字节（std::random_device，无第三方依赖）。
/// @note 仅在握手/测试等低频路径调用；不要用于每包 nonce 生成。
void SecureRandomBytes(std::uint8_t* out, std::size_t bytes);

/// @brief ChaCha20 块函数（RFC 8439 §2.3）。
/// @param key 32B 密钥。
/// @param counter 32 位块计数器。
/// @param nonce 12B nonce。
/// @param out 64B 输出。
void ChaCha20Block(const std::uint8_t key[kChaCha20KeyBytes], std::uint32_t counter,
                   const std::uint8_t nonce[kChaCha20NonceBytes], std::uint8_t out[64]) noexcept;

/// @brief HChaCha20（RFC 7539 §2.3）：从主密钥和 16B 输入派生 32B 子密钥。
/// @details 与 ChaCha20 相同的 20 轮排列，但输出 state[0..3] + state[12..15]
///          且不做初始状态加法；适合做无第三方库的 KDF。
void HChaCha20(const std::uint8_t key[kChaCha20KeyBytes], const std::uint8_t input[16],
               std::uint8_t out[kChaCha20KeyBytes]) noexcept;

/// @brief ChaCha20 流加密/解密（对称）。
/// @param counter 起始块计数器；RFC 8439 AEAD 从 1 开始。
void ChaCha20Xor(const std::uint8_t key[kChaCha20KeyBytes], std::uint32_t counter,
                 const std::uint8_t nonce[kChaCha20NonceBytes], const std::uint8_t* input,
                 std::uint8_t* output, std::size_t bytes) noexcept;

/// @brief 一次性 Poly1305（RFC 8439 §2.5）。
void Poly1305(const std::uint8_t key[32], const std::uint8_t* message, std::size_t bytes,
              std::uint8_t tag[16]) noexcept;

/// @brief AEAD 加密（RFC 8439 §2.8）。
/// @param ciphertext 与 plaintext 等长；允许 input/output 相同（原地）。
void AeadSeal(const std::uint8_t key[kChaCha20KeyBytes],
              const std::uint8_t nonce[kChaCha20NonceBytes], const std::uint8_t* aad,
              std::size_t aad_bytes, const std::uint8_t* plaintext, std::size_t bytes,
              std::uint8_t* ciphertext, std::uint8_t tag[kAeadTagBytes]) noexcept;

/// @brief AEAD 解密并校验 tag（常量时间比较）。
/// @return tag 不匹配返回 false，此时不保证 plaintext 内容。
bool AeadOpen(const std::uint8_t key[kChaCha20KeyBytes],
              const std::uint8_t nonce[kChaCha20NonceBytes], const std::uint8_t* aad,
              std::size_t aad_bytes, const std::uint8_t* ciphertext, std::size_t bytes,
              const std::uint8_t tag[kAeadTagBytes], std::uint8_t* plaintext) noexcept;

/// @brief 常量时间比较两个 16B tag。
bool ConstantTimeEqual16(const std::uint8_t* a, const std::uint8_t* b) noexcept;

/// @brief PSK -> 握手方向密钥。
void DeriveHandshakeKey(const std::uint8_t psk[kPskBytes], SecureDirection direction,
                        std::uint8_t out[kChaCha20KeyBytes]) noexcept;

/// @brief 双方 nonce -> 会话方向密钥。
void DeriveSessionKey(const std::uint8_t psk[kPskBytes],
                      const std::uint8_t client_nonce[kHandshakeNonceBytes],
                      const std::uint8_t server_nonce[kHandshakeNonceBytes],
                      SecureDirection direction, std::uint8_t out[kChaCha20KeyBytes]) noexcept;

/// @brief 构造 12B AEAD nonce：方向 u32 BE || seq u64 BE。
void MakeSecureNonce(SecureDirection direction, std::uint32_t seq,
                     std::uint8_t nonce[kChaCha20NonceBytes]) noexcept;

/// @brief PSK -> 8B 指纹（用于把会话票据/缓存绑定到同一 PSK）。
void DerivePskFingerprint(const std::uint8_t psk[kPskBytes],
                          std::uint8_t out[8]) noexcept;

/// @brief 64 位滑动重放窗口（IPsec 风格）。
/// @details highest 是收到过的最大 wire seq；bitmap 的 bit i 表示
///          highest-i 已收到。窗口外的旧包一律拒绝。所有比较按
///          有符号 32 位回绕差值，兼容 seq 溢出的连接。
class ReplayWindow final {
   public:
    static constexpr std::uint32_t kWindow = 64;

    void Reset() noexcept;
    /// @brief 把窗口初始化为「下一个期望 seq 为 next_seq」。
    /// @details 只用于 0-RTT 恢复：对端会从票据里的 next_seq 继续发送，
    ///          旧会话已确认过的序号必须视为已消费，不能重新接受。
    ///          next_seq <= 1 等价于 Reset()。
    void InitializeBefore(std::uint32_t next_seq) noexcept;
    bool initialized() const noexcept {
        return m_initialized;
    }
    std::uint32_t highest() const noexcept {
        return m_highest;
    }
    /// @brief 只查询不更新：该 seq 当前是否可接受。
    bool WouldAccept(std::uint32_t seq) const noexcept;
    /// @brief 接受 seq 并更新窗口。返回 false 表示重放/过旧。
    bool Accept(std::uint32_t seq) noexcept;

   private:
    std::uint32_t m_highest = 0;
    std::uint64_t m_bitmap = 0;
    bool m_initialized = false;
};

/// @brief 编码 HELLO 明文体。
bool EncodeHelloBody(const HelloBody& body, std::uint8_t* out, std::size_t bytes) noexcept;
/// @brief 解码并校验 magic/version/长度。
bool DecodeHelloBody(const std::uint8_t* data, std::size_t bytes, HelloBody* out) noexcept;

/// @brief 编码 HELLO_ACK 明文体。
bool EncodeHelloAckBody(const HelloAckBody& body, std::uint8_t* out, std::size_t bytes) noexcept;
/// @brief 解码并校验 HELLO_ACK 明文体。
bool DecodeHelloAckBody(const std::uint8_t* data, std::size_t bytes, HelloAckBody* out) noexcept;

// ---------------------------------------------------------------------------
// 0-RTT 会话票据
// ---------------------------------------------------------------------------

/// 会话票据线上长度（固定 92B，含 16B Poly1305 认证标签）。
inline constexpr std::size_t kSessionTicketBytes = 92;

/// 会话票据版本。
inline constexpr std::uint8_t kSessionTicketVersion = 1;

/// @brief 0-RTT 会话票据明文（不含 MAC）。
/// @details 票据不携带密钥，只携带双方 nonce、conn_id 与两个方向的
///          “下一个 wire seq”。持有 PSK 的一端可据此重新派生会话密钥；
///          MAC 由 PSK 派生，因此错误 PSK/被篡改/过期的票据会在
///          DecodeSessionTicket 或 Link::Resume 处被拒绝。
struct SessionTicket {
    std::uint32_t conn_id = 0;
    std::uint32_t session_id = 0;
    std::uint64_t issued_ms = 0;
    std::uint64_t expires_ms = 0;
    std::uint8_t client_nonce[kHandshakeNonceBytes] = {};
    std::uint8_t server_nonce[kHandshakeNonceBytes] = {};
    std::uint32_t client_next_wire_seq = 1;
    std::uint32_t server_next_wire_seq = 1;
};

/// @brief 编码并认证会话票据。
/// @param psk 32B 预共享密钥；MAC 密钥由它派生。
/// @return 参数非法（conn_id/session/next seq 为 0、时间倒置、长度不符）返回 false。
bool EncodeSessionTicket(const SessionTicket& ticket, const std::uint8_t psk[kPskBytes],
                         std::vector<std::uint8_t>* out);

/// @brief 解码并校验会话票据的 magic/version/长度/MAC。
/// @note 不检查过期；调用方（Link::Resume）用 expires_ms 判断。
bool DecodeSessionTicket(const std::uint8_t* data, std::size_t bytes,
                         const std::uint8_t psk[kPskBytes], SessionTicket* out);

/// @brief 把一个完整内层帧封装成加密外层帧。
/// @param inner 由 BuildFrame 产出的完整帧（其 conn_id 写入外层头并参与 AAD）。
/// @param seq 本方向递增的 wire seq（必须非 0）。
/// @param out 输出：16B 外层头 + 密文 + 16B tag。
/// @return 参数/长度非法返回 false。
bool SealFrame(const std::uint8_t key[kChaCha20KeyBytes], SecureDirection direction,
               std::uint32_t seq, const std::uint8_t* inner, std::size_t bytes,
               std::vector<std::uint8_t>* out);

/// @brief 打开加密外层帧。
/// @param key 本方向接收密钥。
/// @param direction 发送方方向（决定 nonce 标签）。
/// @param replay 接收方向的重放窗口；只有认证成功后才会推进。
/// @param inner 输出内层完整帧。
/// @param status 失败原因。
bool OpenFrame(const std::uint8_t key[kChaCha20KeyBytes], SecureDirection direction,
               ReplayWindow* replay, const std::uint8_t* packet, std::size_t bytes,
               std::vector<std::uint8_t>* inner, OpenStatus* status);

/// @brief 用握手密钥封装握手帧（HELLO / HELLO_ACK）。
/// @param inner_type 必须是 kHello 或 kHelloAck。
/// @param body 明文握手体。
/// @param session 握手 session id（写入内层 header.seq）。
/// @param conn_id 连接 id（写入内外层帧头；监听方在 HELLO_ACK 里回填确认值）。
/// @param seq 握手方向 wire seq（重传时复用同一 seq 表示幂等重传）。
bool SealHandshakeFrame(const std::uint8_t psk[kPskBytes], SecureDirection direction,
                        std::uint8_t inner_type, std::uint32_t session, std::uint32_t conn_id,
                        std::uint32_t seq, const std::uint8_t* body, std::size_t body_bytes,
                        std::vector<std::uint8_t>* out);

/// @brief 打开握手帧，得到明文握手体。
/// @param conn_id 输出内层帧的 conn_id（外层同名字段已参与 AEAD 认证）。
bool OpenHandshakeFrame(const std::uint8_t psk[kPskBytes], SecureDirection direction,
                        const std::uint8_t* packet, std::size_t bytes, std::uint8_t* inner_type,
                        std::uint32_t* session, std::uint32_t* conn_id,
                        std::vector<std::uint8_t>* body, OpenStatus* status);

}  // namespace go2cpp::bento
