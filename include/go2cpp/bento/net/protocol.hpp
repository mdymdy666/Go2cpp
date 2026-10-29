#pragma once

/**
 * @file protocol.hpp
 * @brief Bento Transport Protocol v0.2 的帧格式、常量与编解码。
 * @details 帧头固定 16 字节，网络字节序（大端）：
 *
 *          | 偏移 | 字段    | 类型 | 语义                              |
 *          |------|---------|------|-----------------------------------|
 *          | 0    | ver     | u8   | 协议版本，当前 2                  |
 *          | 1    | type    | u8   | FrameType                         |
 *          | 2    | chan    | u8   | 通道 id（move=0/click=1/video=2） |
 *          | 3    | flags   | u8   | kFlagAck / kFlagKey / kFlagRetransmit / kFlagFragmented /
 *          |      |         |      | kFlagEncrypted / kFlagFec / kFlagResume |
 *          | 4    | conn_id | u32  | 连接标识（HELLO 协商，迁移/0-RTT 复用） |
 *          | 8    | seq     | u32  | 通道内序列号                      |
 *          | 12   | dep_seq | u32  | 依赖的不可靠 seq，0 表示无依赖    |
 *
 *          conn_id 是 P2 新增字段：接收端按它路由，而不是按源 IP:port；
 *          因此对端更换本地地址（NAT rebinding / 移动网络切换）后，
 *          只要 conn_id 相同且（加密模式下）AEAD 校验通过，会话即可延续。
 *
 *          若 flags 含 kFlagAck，帧头后紧跟 8 字节 ACK 段
 *          （ack_seq u32 + ack_bits u32），再之后是载荷。
 *
 *          若 STREAM 帧的 flags 含 kFlagFragmented，载荷开头是 8 字节
 *          分片子头（frame_id u32 + frag_idx u16 + frag_cnt u16），
 *          其后是该分片的数据。单帧载荷 <= 1300B 的消息不分片。
 *
 *          设计取舍：把 ACK 段做成可选前缀而不是独立固定字段，是为了让
 *          "数据帧捎带 ACK" 与 "纯 ACK 帧" 共用同一套编解码；ack_bits 的
 *          bit i 表示 ack_seq+i 已收到（SACK 位图，窗口 32），ack_seq 本身
 *          表示「下一个期望的 seq」（累积确认）。
 *
 *          **SACK 多块扩展（P3）**：32 位位图只覆盖 ack_seq..ack_seq+31，
 *          发送窗口 256/接收窗口 512 下窗口外的丢失不可判定，只能等 PTO
 *          整批兜底重传。为此新增纯 ACK 帧的可选尾部（8B 基础之后）：
 *
 *          | 偏移 | 字段     | 类型 | 语义                                |
 *          |------|----------|------|-------------------------------------|
 *          | 0    | ack_seq  | u32  | 与旧格式相同                        |
 *          | 4    | ack_bits | u32  | 与旧格式相同                        |
 *          | 8    | nblocks  | u8   | SACK 块数，1..kMaxSackBlocks        |
 *          | 9    | blocks[] | 4n   | { start_offset u16, len u16 }       |
 *
 *          start_offset 是相对 ack_seq 的偏移（u16 足以覆盖 512 接收窗口），
 *          len 是连续已收到的 seq 个数。载荷总长 8（旧格式）或 9+4n；
 *          旧端只认 payload_bytes == 8，因此**扩展只在双方通过 HELLO/
 *          HELLO_ACK 的 caps 位协商成功后发送**（见 kCapSackExt），未协商
 *          或对端为旧端时由发送方降级为 8B 位图。数据帧捎带的 ACK 段保持
 *          固定 8B（帧头 flags 已无空闲位，无法标示变长），扩展只走纯 ACK
 *          帧；纯 ACK 每 10~20ms 一次，足以覆盖弱网下的 SACK 需求。
 *
 * @author go2cpp contributors
 * @date 2026-10-26
 * @copyright MIT License
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace go2cpp::bento {

/// 协议版本。任何 ver != kProtocolVersion 的帧在解析阶段被拒绝。
inline constexpr std::uint8_t kProtocolVersion = 2;

/// 固定帧头字节数（v0.2 起含 4B conn_id）。
inline constexpr std::size_t kFrameHeaderBytes = 16;

/// 可选 ACK 段字节数（ack_seq + ack_bits）。
inline constexpr std::size_t kAckBytes = 8;

/// SACK 多块扩展：最大块数。16 块 = 64B 扩展，纯 ACK 帧总载荷 73B，
/// 远低于 kMaxFramePayload；块数上限同时约束接收端的解析工作量。
/// 8 块在多丢包链路（40% 丢包、窗口 96）下只能覆盖约 12 个 seq，剩余
/// 已收项仍会被 PTO 整批重传；16 块把单次可确认范围翻倍，实测 1Mbit
/// 限速场景重传从 205 降到 150 以下。块按需使用（run 少时载荷仍很小）。
inline constexpr std::size_t kMaxSackBlocks = 16;

/// 单个 SACK 块线上字节数：start_offset u16 + len u16（大端）。
inline constexpr std::size_t kSackBlockBytes = 4;

/// 扩展 ACK 载荷头：8B 基础 + 1B 块数。
inline constexpr std::size_t kAckExtHeaderBytes = kAckBytes + 1;

/// 扩展 ACK 载荷上限（块数取满）。
inline constexpr std::size_t kAckPayloadMaxBytes =
    kAckExtHeaderBytes + kMaxSackBlocks * kSackBlockBytes;

/// HELLO / HELLO_ACK 基础载荷字节数（magic u32 + session u32 + conn_id u32）。
inline constexpr std::size_t kHelloBaseBytes = 12;

/// HELLO / HELLO_ACK 带能力位的载荷字节数（基础 + caps u16）。
inline constexpr std::size_t kHelloCapsBytes = kHelloBaseBytes + 2;

/// HELLO/HELLO_ACK 能力位。
/// bit0 = 支持 SACK 多块扩展（纯 ACK 帧可变长尾部）。旧端不读该字段，
/// 因而收到 14B HELLO/HELLO_ACK 时按 >=12B 解析、忽略尾部，不会崩。
inline constexpr std::uint16_t kCapSackExt = 0x0001;

/// 单帧载荷上限。留出 16B 头 + 8B ACK 后仍低于 1500B 的常见以太网 MTU，
/// 默认路径不依赖 IP 分片。
inline constexpr std::size_t kMaxFramePayload = 1300;

/// 单帧（头 + ACK + 载荷）上限。
inline constexpr std::size_t kMaxFrameBytes = kFrameHeaderBytes + kAckBytes + kMaxFramePayload;

/// PMTUD 探测帧允许的完整帧上限（包含 16B 头，不含加密封装开销）。
/// 常规数据帧仍受 kMaxFrameBytes 限制；只有 kPmtudProbe 例外。
inline constexpr std::size_t kMaxProbeFrameBytes = 1500;

/// PMTUD 探测/确认帧载荷下限（token u32 + target u32）。
inline constexpr std::size_t kPmtudPayloadBytes = 8;

/// 通道 id。协议不强制语义，这三个是项目约定值。
inline constexpr std::uint8_t kChannelMove = 0;
inline constexpr std::uint8_t kChannelClick = 1;
inline constexpr std::uint8_t kChannelVideo = 2;

/// 帧类型。
enum class FrameType : std::uint8_t {
    kHello = 0x01,     ///< 握手请求：载荷 4B magic + 4B session + 4B conn_id。
    kHelloAck = 0x02,  ///< 握手响应：回显 session 与协商后的 conn_id。
    kStream = 0x03,    ///< 可靠有序消息（seq + ACK + 重传 + 去重）。
    kDatagram = 0x04,  ///< 不可靠消息（seq 单调，接收端 DropStale）。
    kVideo = 0x05,     ///< 视频帧分片：载荷 8B 子头 + 分片数据。
    kAck = 0x06,       ///< 纯 ACK / 心跳：载荷 8B，无数据。
    kBarrier = 0x07,   ///< 因果屏障：header.seq 之前的不可靠 seq 作废。
    /// 流控窗口更新（MAX_DATA）：载荷 8B 大端绝对上限（接收端承诺可接收
    /// 的通道累计字节偏移）。通道 id 在 header.channel。语义与 RFC 9000
    /// 的 MAX_DATA 一致：单调递增，收到较小值忽略。
    kMaxData = 0x08,
    /// 路径 MTU 探测：载荷 >= 8B（token u32 + target u32），其余为填充；
    /// 对端原样回 kPmtudAck。这是 P2 的 PMTUD 探针，不参与应用交付。
    kPmtudProbe = 0x09,
    /// 路径 MTU 探测确认：载荷 >= 8B，回显 token 与 target。
    kPmtudAck = 0x0A,
};

/// MAX_DATA 帧载荷字节数（u64 绝对上限）。
inline constexpr std::size_t kMaxDataPayloadBytes = 8;

/// 帧标志位。
inline constexpr std::uint8_t kFlagAck = 0x01;         ///< 帧头后有 8B ACK 段。
inline constexpr std::uint8_t kFlagKey = 0x02;         ///< VIDEO 关键帧。
inline constexpr std::uint8_t kFlagRetransmit = 0x04;  ///< 重传帧（仅用于观测）。
/// STREAM 载荷前有 8B 分片子头；仅当消息大于 kMaxFramePayload 时置位。
inline constexpr std::uint8_t kFlagFragmented = 0x08;
/// 加密帧：外层头之后是内层完整帧的 AEAD 密文 + 16B tag（见 crypto.hpp）。
/// 未启用 PSK 时该标志永远不出现。
inline constexpr std::uint8_t kFlagEncrypted = 0x10;
/// VIDEO 的 XOR FEC 校验片：载荷为 8B 视频子头 + FEC 扩展 + 校验数据。
/// 仅当发送端配置了 video_fec_group_size > 0 时出现。
inline constexpr std::uint8_t kFlagFec = 0x20;
/// 0-RTT 会话恢复后的首批帧：接收端见到该标志先清空旧应用的通道状态，
/// 再处理本帧；发送端在收到对端 kFlagResumeAck 后停止置位。
inline constexpr std::uint8_t kFlagResume = 0x40;
/// 监听方确认已接受 0-RTT 恢复首帧：客户端见到后停止在后续帧上置
/// kFlagResume。避免“服务端旧心跳先到”导致客户端过早清除恢复标志。
inline constexpr std::uint8_t kFlagResumeAck = 0x80;

/// @brief 线上帧头（主机内存表示，便于测试与调试）。
struct FrameHeader {
    std::uint8_t version = kProtocolVersion;
    FrameType type = FrameType::kAck;
    std::uint8_t channel = 0;
    std::uint8_t flags = 0;
    std::uint32_t conn_id = 0;  ///< P2：连接标识；握手期客户端提议、监听方确认。
    std::uint32_t seq = 0;
    std::uint32_t dep_seq = 0;
};

/// @brief SACK 扩展块：相对 ack_seq 的连续已收区间。
struct SackBlock {
    std::uint16_t start_offset = 0;  ///< 相对 ack_seq 的起始偏移，>= 1。
    std::uint16_t length = 0;        ///< 连续已收到的 seq 个数，>= 1。
};

/// @brief 累积确认 + 32 位 SACK 位图 + 可选多块扩展。
/// @details 线上格式见文件头：前 8B 与旧协议逐字节一致；sack_blocks 只在
///          纯 ACK 帧的扩展尾部出现（由 EncodeAckPayload 编码）。数据帧
///          捎带的 ACK 段永远只用前 8B，因此 block 字段在此场景恒为 0。
struct AckState {
    std::uint32_t ack_seq = 0;  ///< 下一个期望的 seq（即所有 < ack_seq 的消息已收到）。
    std::uint32_t ack_bits = 0;  ///< bit i 置位表示 ack_seq+i 已收到。
    /// 扩展块数：0 = 只有 32 位位图（旧格式/降级）。
    std::uint8_t sack_block_count = 0;
    /// 扩展块：相对 ack_seq 的连续已收区间。块内不重叠，按 start_offset 升序。
    std::array<SackBlock, kMaxSackBlocks> sack_blocks{};
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

/// @brief 编码 16 字节帧头到大端缓冲区。
/// @param out 至少 kFrameHeaderBytes 字节的可写缓冲。
void EncodeHeader(const FrameHeader& header, std::uint8_t* out) noexcept;

/// @brief 解码 16 字节帧头并校验版本/类型。
/// @return 成功返回 true；长度不足、版本不符或类型未知返回 false。
bool DecodeHeader(const std::uint8_t* data, std::size_t size, FrameHeader* out) noexcept;

/// @brief 编码 8 字节 ACK 段。
void EncodeAck(const AckState& ack, std::uint8_t* out) noexcept;

/// @brief 解码 8 字节 ACK 段。
bool DecodeAck(const std::uint8_t* data, std::size_t size, AckState* out) noexcept;

/// @brief 纯 ACK 帧载荷长度是否合法：8B 旧格式，或 9+4n（1<=n<=kMaxSackBlocks）。
bool IsValidAckPayloadSize(std::size_t bytes) noexcept;

/// @brief 按当前块数计算纯 ACK 载荷字节数（8 或 9+4n）。
/// @note 块数越界（>kMaxSackBlocks）时返回 kAckBytes，表示会降级为位图。
std::size_t AckPayloadBytes(const AckState& ack) noexcept;

/// @brief 编码纯 ACK 帧载荷（基础 8B + 可选扩展尾部）。
/// @param capacity out 的可写字节数；< 8 返回 0。
/// @return 实际写入字节数。块数无效或 capacity 不足时只写 8B 并返回 8，
///         保证调用方拿到的是旧端可解析的最小合法载荷。
std::size_t EncodeAckPayload(const AckState& ack, std::uint8_t* out,
                             std::size_t capacity) noexcept;

/// @brief 解码纯 ACK 帧载荷，同时接受旧 8B 与扩展 9+4n 两种长度。
/// @return 长度/块数/块边界任一非法返回 false（调用方计入 invalid_frames）。
bool DecodeAckPayload(const std::uint8_t* data, std::size_t size, AckState* out) noexcept;

/// @brief 构造完整帧（头 + 可选 ACK 段 + 载荷）。
/// @param ack 非空时在帧头后写入 ACK 段并置 kFlagAck；调用方不要自行置位。
/// @param payload 载荷指针，可为 nullptr（bytes 必须为 0）。
/// @return 完整帧字节；参数非法时返回空 vector。
std::vector<std::uint8_t> BuildFrame(const FrameHeader& header, const AckState* ack,
                                     const std::uint8_t* payload, std::size_t payload_bytes);

/// @brief 解析任意类型的完整帧，并做长度与类型相关的边界检查。
/// @return 格式非法返回 false（调用方应计入 invalid_frames）。
bool ParseFrame(const std::uint8_t* data, std::size_t size, FrameView* out) noexcept;

/// @brief STREAM 分片子头字节数：frame_id u32 + frag_idx u16 + frag_cnt u16。
inline constexpr std::size_t kStreamFragmentHeaderBytes = 8;

/// @brief STREAM 每个分片可携带的应用数据上限（帧载荷上限减去子头）。
inline constexpr std::size_t kStreamFragmentBytes =
    kMaxFramePayload - kStreamFragmentHeaderBytes;

/// @brief STREAM 分片子头（仅 kFlagFragmented 帧存在）。
struct StreamFragmentHeader {
    std::uint32_t frame_id = 0;   ///< 逻辑消息 id；同一消息的所有分片相同。
    std::uint16_t frag_index = 0; ///< 分片下标，0 起。
    std::uint16_t frag_count = 1; ///< 分片总数，至少 1。
};

/// @brief 编码 STREAM 分片子头。
void EncodeStreamFragmentHeader(const StreamFragmentHeader& header,
                                std::uint8_t* out) noexcept;

/// @brief 解码 STREAM 分片子头（不校验 index/count 关系，交给通道层）。
bool DecodeStreamFragmentHeader(const std::uint8_t* data, std::size_t size,
                                StreamFragmentHeader* out) noexcept;

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
