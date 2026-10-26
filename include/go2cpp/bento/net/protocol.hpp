#pragma once

/**
 * @file protocol.hpp
 * @brief Bento Transport Protocol v0.1 的帧格式、常量与编解码。
 * @details 帧头固定 12 字节，网络字节序（大端）：
 *
 *          | 偏移 | 字段    | 类型 | 语义                              |
 *          |------|---------|------|-----------------------------------|
 *          | 0    | ver     | u8   | 协议版本，当前 1                  |
 *          | 1    | type    | u8   | FrameType                         |
 *          | 2    | chan    | u8   | 通道 id（move=0/click=1/video=2） |
 *          | 3    | flags   | u8   | kFlagAck / kFlagKey / kFlagRetransmit |
 *          | 4    | seq     | u32  | 通道内序列号                      |
 *          | 8    | dep_seq | u32  | 依赖的不可靠 seq，0 表示无依赖    |
 *
 *          若 flags 含 kFlagAck，帧头后紧跟 8 字节 ACK 段
 *          （ack_seq u32 + ack_bits u32），再之后是载荷。
 *
 *          设计取舍：把 ACK 段做成可选前缀而不是独立固定字段，是为了让
 *          "数据帧捎带 ACK" 与 "纯 ACK 帧" 共用同一套编解码；ack_bits 的
 *          bit i 表示 ack_seq+i 已收到（SACK 位图，窗口 32），ack_seq 本身
 *          表示「下一个期望的 seq」（累积确认）。
 *
 * @author go2cpp contributors
 * @date 2026-10-26
 * @copyright MIT License
 */

#include <cstddef>
#include <cstdint>
#include <vector>

namespace go2cpp::bento {

/// 协议版本。任何 ver != 1 的帧在解析阶段被拒绝。
inline constexpr std::uint8_t kProtocolVersion = 1;

/// 固定帧头字节数。
inline constexpr std::size_t kFrameHeaderBytes = 12;

/// 可选 ACK 段字节数（ack_seq + ack_bits）。
inline constexpr std::size_t kAckBytes = 8;

/// 单帧载荷上限。留出 12B 头 + 8B ACK 后仍在 1280B 的安全 UDP 载荷内，
/// 避免在真实链路上触发 IP 分片（本协议不依赖 IP 分片）。
inline constexpr std::size_t kMaxFramePayload = 1300;

/// 单帧（头 + ACK + 载荷）上限。
inline constexpr std::size_t kMaxFrameBytes = kFrameHeaderBytes + kAckBytes + kMaxFramePayload;

/// 通道 id。协议不强制语义，这三个是项目约定值。
inline constexpr std::uint8_t kChannelMove = 0;
inline constexpr std::uint8_t kChannelClick = 1;
inline constexpr std::uint8_t kChannelVideo = 2;

/// 帧类型。
enum class FrameType : std::uint8_t {
    kHello = 0x01,     ///< 握手请求：载荷 4B magic + 4B session。
    kHelloAck = 0x02,  ///< 握手响应：回显 session。
    kStream = 0x03,    ///< 可靠有序消息（seq + ACK + 重传 + 去重）。
    kDatagram = 0x04,  ///< 不可靠消息（seq 单调，接收端 DropStale）。
    kVideo = 0x05,     ///< 视频帧分片：载荷 8B 子头 + 分片数据。
    kAck = 0x06,       ///< 纯 ACK / 心跳：载荷 8B，无数据。
    kBarrier = 0x07,   ///< 因果屏障：header.seq 之前的不可靠 seq 作废。
};

/// 帧标志位。
inline constexpr std::uint8_t kFlagAck = 0x01;         ///< 帧头后有 8B ACK 段。
inline constexpr std::uint8_t kFlagKey = 0x02;         ///< VIDEO 关键帧。
inline constexpr std::uint8_t kFlagRetransmit = 0x04;  ///< 重传帧（仅用于观测）。

/// @brief 线上帧头（主机内存表示，便于测试与调试）。
struct FrameHeader {
    std::uint8_t version = kProtocolVersion;
    FrameType type = FrameType::kAck;
    std::uint8_t channel = 0;
    std::uint8_t flags = 0;
    std::uint32_t seq = 0;
    std::uint32_t dep_seq = 0;
};

/// @brief 累积确认 + 32 位 SACK 位图。
struct AckState {
    std::uint32_t ack_seq = 0;  ///< 下一个期望的 seq（即所有 < ack_seq 的消息已收到）。
    std::uint32_t ack_bits = 0;  ///< bit i 置位表示 ack_seq+i 已收到。
};

/// @brief 解析后的帧视图，payload 指向调用方缓冲区，不拥有内存。
struct FrameView {
    FrameHeader header;
    AckState ack;  ///< 仅当 has_piggyback_ack 为 true 时有效。
    bool has_piggyback_ack = false;
    const std::uint8_t* payload = nullptr;
    std::size_t payload_bytes = 0;
};

/// @brief 判断序列号 a 是否在 b 之后（回绕安全）。
/// @note 使用有符号差值，只适用于窗口远小于 2^31 的场景；本协议窗口最大 512。
inline bool SeqGreater(std::uint32_t a, std::uint32_t b) noexcept {
    return static_cast<std::int32_t>(a - b) > 0;
}

/// @brief 判断 a 是否 >= b（回绕安全）。
inline bool SeqGreaterOrEqual(std::uint32_t a, std::uint32_t b) noexcept {
    return !SeqGreater(b, a);
}

/// @brief 当前单调时钟毫秒数（steady_clock，不受系统时间调整影响）。
std::uint64_t NowMs() noexcept;

/// @brief 帧类型是否是已定义类型。
bool IsKnownFrameType(std::uint8_t raw) noexcept;

/// @brief 帧类型名（日志/测试用）。
const char* FrameTypeName(FrameType type) noexcept;

/// @brief 编码 12 字节帧头到大端缓冲区。
/// @param out 至少 kFrameHeaderBytes 字节的可写缓冲。
void EncodeHeader(const FrameHeader& header, std::uint8_t* out) noexcept;

/// @brief 解码 12 字节帧头并校验版本/类型。
/// @return 成功返回 true；长度不足、版本不符或类型未知返回 false。
bool DecodeHeader(const std::uint8_t* data, std::size_t size, FrameHeader* out) noexcept;

/// @brief 编码 8 字节 ACK 段。
void EncodeAck(const AckState& ack, std::uint8_t* out) noexcept;

/// @brief 解码 8 字节 ACK 段。
bool DecodeAck(const std::uint8_t* data, std::size_t size, AckState* out) noexcept;

/// @brief 构造完整帧（头 + 可选 ACK 段 + 载荷）。
/// @param ack 非空时在帧头后写入 ACK 段并置 kFlagAck；调用方不要自行置位。
/// @param payload 载荷指针，可为 nullptr（bytes 必须为 0）。
/// @return 完整帧字节；参数非法时返回空 vector。
std::vector<std::uint8_t> BuildFrame(const FrameHeader& header, const AckState* ack,
                                     const std::uint8_t* payload, std::size_t payload_bytes);

/// @brief 解析任意类型的完整帧，并做长度与类型相关的边界检查。
/// @return 格式非法返回 false（调用方应计入 invalid_frames）。
bool ParseFrame(const std::uint8_t* data, std::size_t size, FrameView* out) noexcept;

/// @brief 视频分片子头（8B）：frame_id u32 + frag_idx u16 + frag_cnt u16。
inline constexpr std::size_t kVideoHeaderBytes = 8;

/// @brief 视频分片子头。
struct VideoFragmentHeader {
    std::uint32_t frame_id = 0;
    std::uint16_t frag_index = 0;
    std::uint16_t frag_count = 0;
};

/// @brief 编码视频分片子头。
void EncodeVideoHeader(const VideoFragmentHeader& header, std::uint8_t* out) noexcept;

/// @brief 解码视频分片子头（不校验 frag_index/frag_count 关系）。
bool DecodeVideoHeader(const std::uint8_t* data, std::size_t size,
                       VideoFragmentHeader* out) noexcept;

}  // namespace go2cpp::bento
