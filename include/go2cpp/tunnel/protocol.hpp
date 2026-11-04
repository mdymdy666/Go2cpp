#pragma once

/**
 * @file protocol.hpp
 * @brief 公网穿透隧道协议：帧类型 + 载荷编解码 + 复用的通用帧格式。
 *
 * @details 本模块把「公网 VPS 上的 `TunnelServer`」和「NAT 后的本地
 *          `TunnelClient`」之间的**控制连接**协议定死。控制连接是**一条** TCP，
 *          上面同时承载注册、多条被穿透流（浏览器连接）的数据与心跳——因此
 *          帧里必须带流 id 才能区分。
 *
 * ## 1. 帧格式（复用 `go2cpp::frame::FrameCodec`）
 *
 * 不重新发明半包/粘包/长度校验，直接复用 `include/go2cpp/frame/frame_codec.hpp`
 * 的 12 字节帧头（magic/version/type/flags/length，全部小端）：
 *
 * ```text
 *   0   4  magic    "TUNL"
 *   4   1  version  1
 *   5   1  type     见 FrameType
 *   6   2  flags    保留（本版本恒 0）
 *   8   4  length   载荷字节数（小端）
 *   12  N  payload  见第 2 节
 * ```
 *
 * **不启用帧尾 CRC**（`FrameSpec::checksum` 为空）：TCP 自身已有 16 位校验和与
 * 重传，隧道再算一遍 CRC-32 只是纯开销。需要时可用
 * `MakeTunnelFrameSpec(true)` 打开，两端必须一致。
 *
 * ## 2. 载荷布局
 *
 * 所有多字节整数小端；字符串 = `len` + 原始字节（**不是** NUL 结尾）。
 *
 * | type | 名称 | 方向 | 载荷 |
 * |---|---|---|---|
 * | 1 | `HELLO` | C→S | `ver(2) flags(2) sub_len(1) sub local_port(2) token_len(2) token` |
 * | 2 | `HELLO_ACK` | S→C | `status(1) sub_len(1) sub url_len(2) url msg_len(2) msg` |
 * | 3 | `OPEN` | S→C | `stream_id(4) ip_len(1) ip port(2)` |
 * | 4 | `DATA` | 双向 | `stream_id(4) body` |
 * | 5 | `CLOSE` | 双向 | `stream_id(4) code(1)` |
 * | 6 | `PING` | 双向 | `nonce(8)` |
 * | 7 | `PONG` | 双向 | `nonce(8)` |
 *
 * `stream_id` 由发起 `OPEN` 的一侧分配：本版本只有服务端（收到浏览器连接时）
 * 发起，客户端从不主动开流；id 从 1 递增，回收后不复用。
 *
 * `HELLO` 的 `local_port` 只用于服务端目录页展示（`shop → 127.0.0.1:8080`），
 * 不参与路由。`HELLO_ACK.status` 见 `Status`。
 *
 * ## 3. 错误处理
 *
 * 本文件的错误是 `go2cpp::ErrorPtr`（与 RelayPipe / 项目其它模块一致），
 * 帧层畸形（magic/version 不符）由 `FrameCodec` 报出，调用方**必须断开**
 * 整条控制连接——帧边界已不可信。
 *
 * @author go2cpp contributors
 * @date 2026-11-02
 * @copyright MIT License
 */

#include "go2cpp/error.hpp"
#include "go2cpp/frame/frame_codec.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace go2cpp {
namespace tunnel {

/// 协议版本；`HELLO.version` 与本值不等时服务端拒绝。
inline constexpr std::uint16_t kProtocolVersion = 1;
/// 4 字节魔数 `"TUNL"`。
inline constexpr char kFrameMagic[] = "TUNL";
/// 单帧载荷上限（字节），与 `FrameCodec` 的 `max_payload` 一致。
/// 2026-11-04 合并写实测：256 KiB 帧在 4 并发下反而比 64 KiB 低 15%
/// （单帧重组缓冲 + 队列元素跨核迁移压垮 L2），故保持 64 KiB，靠 writev
/// 一次提交 4-5 帧来减少 syscall。
inline constexpr std::size_t kMaxPayload = 65536;
/// `DATA` 帧里单个 body 的上限（`stream_id` 占 4 字节）。
inline constexpr std::size_t kMaxDataChunk = kMaxPayload - 4;
/// 子域名最大长度（DNS label 上限）。
inline constexpr std::size_t kMaxSubdomainLen = 63;
/// token 最大长度（防止 HELLO 帧过大）。
inline constexpr std::size_t kMaxTokenLen = 256;

/// 单次 `SendData` 合并写最多几帧（16 x 65532 B ≈ 1 MiB，iovec 段数 32）。
inline constexpr std::size_t kMaxSendBatchFrames = 16;
/// 隧道帧头字节数（12 字节，与 `frame::FrameCodec` 的 header_len 一致）。
inline constexpr std::size_t kFrameHeaderBytes = 12;

/// 帧类型（对应 `frame::Frame::type`）。
enum class FrameType : std::uint8_t {
    kHello = 1,     ///< 客户端注册：子域名 / 本地端口 / token。
    kHelloAck = 2,  ///< 服务端应答：状态 / 最终子域名 / 公网 URL。
    kOpen = 3,      ///< 服务端通知客户端：有一条新公网连接。
    kData = 4,      ///< 流数据（双向）。
    kClose = 5,     ///< 流关闭（双向）。
    kPing = 6,      ///< 心跳请求。
    kPong = 7,      ///< 心跳应答。
    kChallenge = 8, ///< 服务端 → 客户端：鉴权挑战（16B 随机数）。
    kAuth = 9,      ///< 客户端 → 服务端：挑战应答（16B AEAD tag）。
    kGoAway = 10,   ///< 双向：优雅关闭（不再接受新流，等待在途流结束）。
    kOpenRaw = 11,  ///< 服务端 → 客户端：per-stream 裸 TCP 数据通道（载荷同 OPEN）。
    kRawBind = 12,  ///< 客户端 → 服务端：数据连接绑定到某个 stream_id（载荷 4B id）。
};

/// `HELLO.flags` 的 bit0：本连接是「数据连接」（对应 frp 的 work conn /
/// rathole 的 data channel）。控制连接负责注册与心跳，数据连接只承载流。
inline constexpr std::uint32_t kHelloFlagDataConnection = 1U << 0U;
/// HELLO.flags 的 bit1：本数据连接是 **per-stream 裸 TCP 通道**——握手完成后
/// 只发一个 kRawBind 帧，随后该 TCP 直接承载一个流的原始字节（两端 socket
/// 之间走 splice(2)，不再有任何帧头）。控制连接只传 kOpenRaw / kClose。
inline constexpr std::uint32_t kHelloFlagRawStream = 1U << 1U;

/**
 * @brief 读环境变量 GO2CPP_TUNNEL_RAW_STREAM（1/true/on/yes，大小写不敏感）。
 *
 * @return 开关状态；默认 true（per-stream raw TCP，2026-11-04 起默认化）。
 *         显式设 0/false/off/no 回退旧 mux 帧化路径。两端必须都支持：
 *         服务端决定是否发 kOpenRaw，客户端决定是否响应；老客户端不认
 *         kOpenRaw 会断控制连接，升级时先升级客户端，或临时设 0。
 */
bool RawStreamEnabledFromEnv();

/**
 * @brief 读 GO2CPP_TUNNEL_RAW_THRESHOLD：多大的请求 body 才走 per-stream raw。
 *
 * @return 阈值字节数；默认 1048576（1 MiB）。0 = 全部请求走 raw（含未知长度）；
 *         -1 = 全走 mux；非法值/空值按默认处理。Content-Length 缺失（未知长度）
 *         一律走 mux——raw 数据通道要在写请求头前决定，无法等 body 到齐。
 */
std::int64_t RawStreamThresholdBytesFromEnv();

/**
 * @brief 按请求 body 长度决定走 raw 还是 mux。
 *
 * @param content_length [in] 请求 Content-Length；负数表示未知/缺失。
 * @return true = raw 数据通道（零拷贝 splice）；false = mux 帧化路径。
 *
 * ## 定位
 * 服务端 HandlePublic 的唯一路径选择点。选错只会损失性能或增加一次握手，
 * 不影响正确性；但小请求误走 raw 会多 1 RTT + 4 帧握手。
 */
bool ShouldUseRawStream(std::int64_t content_length);

/// PSK 字节数，与 Bento 的 32B PSK 约定一致。
inline constexpr std::size_t kPskBytes = 32;
/// 鉴权挑战字节数。
inline constexpr std::size_t kChallengeBytes = 16;
/// 鉴权应答 tag 字节数（ChaCha20-Poly1305 tag）。
inline constexpr std::size_t kAuthTagBytes = 16;

/// 32 字节预共享密钥。
using Psk = std::array<std::uint8_t, kPskBytes>;

/// `GOAWAY` 载荷。
struct GoAway {
    std::uint8_t reason = 0;          ///< 0=正常关闭，1=维护，2=被替换。
    std::uint32_t retry_after_ms = 0; ///< 建议重连延迟；0 表示不要重连。
};

/// 帧类型短名（日志/测试用）。
const char* FrameTypeName(FrameType type) noexcept;

/**
 * @brief `HELLO_ACK.status` 的取值。
 * @details 只有 `kOk` 表示注册成功；其余状态客户端应停止重连并打印原因。
 */
enum class Status : std::uint8_t {
    kOk = 0,             ///< 注册成功。
    kRejected = 1,       ///< 通用拒绝（子域名非法 / 已被占用 / 并发上限）。
    kBadVersion = 2,     ///< 协议版本不匹配。
    kUnauthorized = 3,   ///< token 不匹配。
    kServerShutdown = 4, ///< 服务端正在退出（客户端可重连）。
};

/// `Status` 短名。
const char* StatusName(Status status) noexcept;

/**
 * @brief 构造隧道使用的帧格式描述。
 *
 * @param with_checksum 是否启用帧尾 CRC-32；默认 `false`（TCP 已保证完整性）。
 * @return `magic="TUNL"`、`version=1`、`header_len=12`、`max_payload=64 KiB` 的 spec。
 * @note 两端必须使用同一取值；不一致时 version 字节相同但帧尾长度不同，
 *       表现为解析错位，因此协议版本变更时应同时提升 `kProtocolVersion`。
 */
frame::FrameSpec MakeTunnelFrameSpec(bool with_checksum = false);

// ============================================================================
// 载荷结构体
// ============================================================================

/// `HELLO` 载荷。
struct Hello {
    std::uint16_t version = kProtocolVersion;
    std::uint32_t flags = 0;
    std::string subdomain;   ///< 期望子域名；空表示由服务端分配。
    std::uint16_t local_port = 0;  ///< 本地端口（仅展示用）。
    std::string token;       ///< 可选鉴权 token。
};

/// `HELLO_ACK` 载荷。
struct HelloAck {
    Status status = Status::kOk;
    std::string subdomain;   ///< 最终生效的子域名（自动分配时与请求不同）。
    std::string public_url;  ///< 完整公网 URL，如 `http://203.0.113.7:8443/shop/`。
    std::string message;     ///< 失败原因或提示。
};

/// `OPEN` 载荷。
struct Open {
    std::uint32_t stream_id = 0;
    std::string peer_ip;        ///< 公网侧对端 IP（展示 / 日志）。
    std::uint16_t peer_port = 0;
};

/// `DATA` 载荷（拆开后）。
struct DataFrame {
    std::uint32_t stream_id = 0;
    const std::uint8_t* body = nullptr;
    std::size_t length = 0;
};

/// `CLOSE` 载荷。
struct Close {
    std::uint32_t stream_id = 0;
    std::uint8_t code = 0;
};

// ============================================================================
// 编解码（每个函数都做边界检查，绝不按声明长度预分配）
// ============================================================================

/// 编码 `HELLO` 载荷；字段超长时返回 false 并写 `*err`。
bool EncodeHello(const Hello& hello, std::vector<std::uint8_t>* out,
                 ErrorPtr* err);
/// 解码 `HELLO` 载荷；畸形/超长返回 false。
bool DecodeHello(const std::uint8_t* data, std::size_t length, Hello* out,
                 ErrorPtr* err);

/// 编码 `HELLO_ACK` 载荷。
bool EncodeHelloAck(const HelloAck& ack, std::vector<std::uint8_t>* out,
                    ErrorPtr* err);
/// 解码 `HELLO_ACK` 载荷。
bool DecodeHelloAck(const std::uint8_t* data, std::size_t length, HelloAck* out,
                    ErrorPtr* err);

/// 编码 `OPEN` 载荷。
bool EncodeOpen(const Open& open, std::vector<std::uint8_t>* out, ErrorPtr* err);
/// 解码 `OPEN` 载荷。
bool DecodeOpen(const std::uint8_t* data, std::size_t length, Open* out,
                ErrorPtr* err);

/**
 * @brief 把 `stream_id` 与一段 body 拼成 `DATA` 载荷。
 * @param stream_id 流 id。
 * @param body 数据首地址；`length == 0` 时可为 `nullptr`。
 * @param length 数据字节数，必须 ≤ `kMaxDataChunk`。
 * @param out 输出载荷（被清空后写入）。
 * @param err 错误出参，可为 `nullptr`。
 * @return 成功 true；`length` 过大或 `body` 为空且 `length > 0` 返回 false。
 */
bool EncodeData(std::uint32_t stream_id, const std::uint8_t* body,
                std::size_t length, std::vector<std::uint8_t>* out,
                ErrorPtr* err);
/// 解析 `DATA` 载荷；`body` 指向 `data` 内部，生命周期随入参。
bool DecodeData(const std::uint8_t* data, std::size_t length, DataFrame* out,
                ErrorPtr* err);

/// 编码 `CLOSE` 载荷。
bool EncodeClose(const Close& close, std::vector<std::uint8_t>* out,
                 ErrorPtr* err);
/// 解码 `CLOSE` 载荷。
bool DecodeClose(const std::uint8_t* data, std::size_t length, Close* out,
                 ErrorPtr* err);

/// 编码 `PING`/`PONG` 的 8 字节 nonce 载荷。
std::vector<std::uint8_t> EncodeNonce(std::uint64_t nonce);
/// 解码 8 字节 nonce；长度不为 8 返回 false。
bool DecodeNonce(const std::uint8_t* data, std::size_t length,
                 std::uint64_t* out);

// ============================================================================
// 鉴权（复用 Bento 的 PSK + ChaCha20-Poly1305，不自己发明 MAC）
// ============================================================================

/**
 * @brief 由人类可读的 token 派生 32B PSK：`sha256(token)`。
 * @param secret 服务端与客户端共享的字符串（任意长度）。
 * @return 32B PSK。
 * @note 用摘要而不是直接截断，保证任意长度 token 都映射到固定 32B，
 *       且不把原始 token 发上线（挑战-应答只传 tag）。
 */
Psk DerivePsk(const std::string& secret);

/**
 * @brief 生成 16B 密码学随机挑战（`getrandom(2)` / `/dev/urandom`）。
 * @return 16B 挑战；系统 CSPRNG 不可用时返回空 vector。
 */
std::vector<std::uint8_t> MakeChallenge();

/**
 * @brief 计算挑战应答：`AEAD-Seal(key=KDF(psk), nonce=challenge[0..12),
 *        aad=challenge, plaintext=空)` 的 16B tag。
 * @param psk 预共享密钥。
 * @param challenge 16B 挑战。
 * @param tag 输出 16B tag。
 * @return 成功 true。
 */
bool ComputeAuthTag(const Psk& psk, const std::uint8_t* challenge,
                    std::uint8_t* tag);

/**
 * @brief 校验挑战应答（常量时间比较由 AEAD 内部保证）。
 * @return tag 正确返回 true。
 */
bool VerifyAuthTag(const Psk& psk, const std::uint8_t* challenge,
                   const std::uint8_t* tag);

/// 编码/解码 16B 挑战载荷（长度必须恰好 16）。
std::vector<std::uint8_t> EncodeChallenge(const std::uint8_t* challenge);
bool DecodeChallenge(const std::uint8_t* data, std::size_t length,
                     std::uint8_t* out);

/// 编码/解码 16B 应答载荷。
std::vector<std::uint8_t> EncodeAuth(const std::uint8_t* tag);
bool DecodeAuth(const std::uint8_t* data, std::size_t length,
                std::uint8_t* out);

/// 编码/解码 `GOAWAY` 载荷。
bool EncodeGoAway(const GoAway& goaway, std::vector<std::uint8_t>* out,
                  ErrorPtr* err);
bool DecodeGoAway(const std::uint8_t* data, std::size_t length, GoAway* out,
                  ErrorPtr* err);

/**
 * @brief 校验子域名合法性：只允许 `[a-z0-9-]`，长度 1..63，不以 `-` 开头/结尾。
 * @param subdomain 待校验子域名（应为小写）。
 * @return 合法返回 true。
 */
bool IsValidSubdomain(const std::string& subdomain) noexcept;

/**
 * @brief 生成随机子域名（6 位小写字母 + 数字）。
 * @param length 位数，取值 4..32（越界自动夹取）。
 * @return 随机子域名；随机源不可用时退化为时间戳后缀。
 */
std::string MakeRandomSubdomain(std::size_t length = 6);

}  // namespace tunnel
}  // namespace go2cpp
