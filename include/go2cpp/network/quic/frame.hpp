/**
 * @file frame.hpp
 * @brief QUIC 帧层（RFC 9000 §19）。
 *
 * @details
 * ============================================================================
 * ## 一、这一层做什么，不做什么
 *
 * **做**：把「一个包里的载荷字节」解释成一组帧，以及把帧序列化成字节。
 * 纯逻辑，**无密钥、无连接状态**，因此可以对照 RFC 逐字段写测试。
 *
 * **不做**：包头的加解密（Header Protection，RFC 9001 §5.4）、ACK 的生成与
 * 丢包检测、流状态机与流控。这些都需要密钥或连接状态，见 `docs/quic.md`
 * 的「未实现」清单。
 *
 * ## 二、一个关键前提：`length` 必须是「包的剩余字节数」
 *
 * `DecodeFrame` 的 `length` 参数**不是**"你手里缓冲区的大小"，而是
 * **"当前包还剩多少字节"**。原因是 RFC 9000 §19.8 规定：
 *
 * > If the LEN bit is not set, then the Stream Data field extends to the end
 * > of the packet.
 *
 * 也就是说 LEN=0 的 STREAM 帧**天然延伸到包尾**，没有长度字段可依。调用方
 * 必须保证 `length` 恰好是包尾，否则解析会跨度数与真实边界不符。
 *
 * 同理，PADDING 帧也是靠"剩余全是零"来消费的。
 *
 * ## 三、未知帧类型：**RFC 要求当成连接错误，不是静默跳过**
 *
 * 这一点容易搞反，所以特别写明：
 *
 * > RFC 9000 §12.4：**An endpoint MUST treat the receipt of a frame of unknown
 * > type as a connection error of type FRAME_ENCODING_ERROR.**
 *
 * QUIC 的扩展帧（如 RFC 9221 的 DATAGRAM）之所以能加，是因为**双方先通过
 * 其他机制协商**，而不是靠"接收方忽略不认识的东西"。因此本模块对未知类型
 * 返回 `FrameStatus::kUnknownType`，**调用方应当把它当作协议错误并关闭连接**，
 * 而不是跳过 —— 后者会造成两端对包边界的理解分歧，进而完全错位。
 *
 * 返回一个独立的枚举值（而不是直接并入 `kFrameEncodingError`）只是为了
 * **诊断时能区分"对端用了新扩展"和"对端发的字节是垃圾"**，不是给它豁免权。
 *
 * ## 四、PADDING 的合并消费
 *
 * 每个 `0x00` 字节逻辑上是一个独立的 PADDING 帧，但连续 N 个零等价于
 * "N 个 PADDING"。本实现把它们**合并消费**成一个 `padding_count = N` 的帧，
 * 语义等价且避免上层为几百个填充帧做几百次循环。
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace go2cpp::net::quic {

// ============================================================================
// 帧类型码（RFC 9000 §19）
// ============================================================================

/** @brief 帧类型码。 */
enum class FrameType : std::uint8_t {
    kPadding = 0x00,
    kPing = 0x01,
    kAck = 0x02,
    /// ACK + ECN 计数（RFC 9000 §19.3.2）。
    kAckEcn = 0x03,
    kResetStream = 0x04,
    kStopSending = 0x05,
    kCrypto = 0x06,
    kNewToken = 0x07,
    /// STREAM 的 8 个变体占 0x08..0x0f，用 `IsStreamType()` 判断。
    kStreamBase = 0x08,
    kMaxData = 0x10,
    kMaxStreamData = 0x11,
    kMaxStreamsBidi = 0x12,
    kMaxStreamsUni = 0x13,
    kDataBlocked = 0x14,
    kStreamDataBlocked = 0x15,
    kStreamsBlockedBidi = 0x16,
    kStreamsBlockedUni = 0x17,
    kNewConnectionId = 0x18,
    kRetireConnectionId = 0x19,
    kPathChallenge = 0x1a,
    kPathResponse = 0x1b,
    kConnectionCloseTransport = 0x1c,
    kConnectionCloseApplication = 0x1d,
    kHandshakeDone = 0x1e,
};

/** @brief STREAM 帧类型码的低 3 位标志（RFC 9000 §19.8）。 */
namespace StreamBits {
/// 带 Offset 字段（**最高**的那一位，0x04）。
inline constexpr std::uint8_t kOffset = 0x04;
/// 带 Length 字段（0x02）。若不设，数据延伸到包尾。
inline constexpr std::uint8_t kLength = 0x02;
/// FIN：发送方向到此结束（**最低**位，0x01）。
inline constexpr std::uint8_t kFin = 0x01;
}  // namespace StreamBits

/** @brief 判断类型码是否落在 STREAM 的 8 个变体内。 */
inline constexpr bool IsStreamType(std::uint8_t type) noexcept {
    return type >= 0x08 && type <= 0x0f;
}

/** @brief 帧类型转字符串（便于日志与测试诊断）。 */
const char* ToString(FrameType type) noexcept;

// ============================================================================
// 解析状态
// ============================================================================

/** @brief 帧解析/序列化状态。 */
enum class FrameStatus : std::uint8_t {
    /// 成功。
    kOk,
    /// 输入不足，需要更多字节。**此时 `consumed == 0`**，调用方可以原位置重试。
    kNeedMore,
    /// 帧编码非法（长度字段与实际不符、保留位非零、字段超范围等）。
    /// @note RFC 9000 §12.4 要求对这类情况发 FRAME_ENCODING_ERROR。
    kFrameEncodingError,
    /// 未知帧类型 —— **调用方应当当协议错误处理**，见文件头 §三。
    kUnknownType,
    /// 序列化时给出的字段对该帧类型不合法（内部一致性检查，不是对端问题）。
    kInvalidArgument,
};

const char* ToString(FrameStatus status) noexcept;

// ============================================================================
// 帧结构
// ============================================================================

/**
 * @brief ACK 帧的一个额外区间。
 * @details 编码顺序是 `[Gap, ACK Range Length]`（RFC 9000 §19.3.1）。
 */
struct AckRange final {
    /// 与前一个区间的**最小包号**之间隔了多少个包（再减 1）。
    std::uint64_t gap{0};
    /// 本区间包含的包数减 1（即"长度"字段的值）。
    std::uint64_t length{0};
};

/**
 * @brief 一个 QUIC 帧。
 * @details 所有类型共用这一个结构体；每个字段在哪类帧里有效见各字段注释。
 *          无效字段保持默认值。**不用 union**：会引入平凡性与生命周期问题，
 *          而这里不是热点路径。
 */
struct Frame final {
    /// 帧类型码的原始值（含 STREAM 的低 3 位标志）。
    std::uint8_t type_code{0};
    /** @brief 分类后的帧类型；STREAM 的 8 个变体都归为 `kStreamBase`。 */
    FrameType type{FrameType::kPadding};

    // ---- PADDING ----
    /// 连续 PADDING 的个数（合并消费，见文件头 §四）。
    std::uint64_t padding_count{0};

    // ---- ACK / ACK_ECN ----
    /// 最大的已确认包号。
    std::uint64_t largest_acked{0};
    /// ACK 延迟（单位由 ack_delay_exponent 决定，本层不解码）。
    std::uint64_t ack_delay{0};
    /// 第一个区间的长度（包数减 1）。
    std::uint64_t first_ack_range{0};
    /// 额外的 ACK 区间（可能有多个）。
    std::vector<AckRange> ack_ranges;
    /// 仅 `kAckEcn` 有效：ECT(0) / ECT(1) / CE 计数。
    std::uint64_t ect0_count{0};
    std::uint64_t ect1_count{0};
    std::uint64_t ce_count{0};

    // ---- STREAM ----
    /// 流 id（STREAM / RESET_STREAM / STOP_SENDING / MAX_STREAM_DATA / STREAM_DATA_BLOCKED）。
    std::uint64_t stream_id{0};
    /// 是否带 Offset 字段（STREAM）。
    bool has_offset{false};
    /// 是否带 Length 字段（STREAM）。
    bool has_length{false};
    /// 是否置 FIN（STREAM）。
    bool fin{false};
    /// STREAM 的数据偏移（仅 `has_offset` 时有效，否则为 0）。
    std::uint64_t offset{0};
    /// 流数据（STREAM / CRYPTO）。
    std::vector<std::uint8_t> data;

    // ---- RESET_STREAM ----
    /// 应用层错误码。
    std::uint64_t app_error_code{0};
    /// RESET_STREAM 的最终大小。
    std::uint64_t final_size{0};

    // ---- NEW_TOKEN ----
    /// NEW_TOKEN 的不透明令牌。
    std::vector<std::uint8_t> token;

    // ---- MAX_DATA / MAX_STREAM_DATA / DATA_BLOCKED / STREAM_DATA_BLOCKED ----
    std::uint64_t max_data{0};
    std::uint64_t max_stream_data{0};

    // ---- MAX_STREAMS / STREAMS_BLOCKED ----
    std::uint64_t max_streams{0};

    // ---- NEW_CONNECTION_ID / RETIRE_CONNECTION_ID ----
    std::uint64_t sequence_number{0};
    std::uint64_t retire_prior_to{0};
    /// 连接 ID（1~20 字节）。
    std::vector<std::uint8_t> connection_id;
    /// 无状态重置令牌（固定 16 字节）。
    std::vector<std::uint8_t> stateless_reset_token;

    // ---- PATH_CHALLENGE / PATH_RESPONSE ----
    /// 固定 8 字节的不透明数据。
    std::array<std::uint8_t, 8> path_data{};

    // ---- CONNECTION_CLOSE ----
    /// 连接错误码（传输层或应用层）。
    std::uint64_t error_code{0};
    /// 仅 `kConnectionCloseTransport` 有效：触发错误的帧类型。
    std::uint64_t closing_frame_type{0};
    /// 原因短语（UTF-8 字节序列，可能为空）。
    std::string reason_phrase;
};

/** @brief 帧解析结果。 */
struct FrameDecoded final {
    FrameStatus status{FrameStatus::kNeedMore};
    Frame frame;
    /// 成功时消耗的字节数；失败时为 0（`kNeedMore` 时调用方原位置重试）。
    std::size_t consumed{0};
};

/** @brief 帧序列化结果。 */
struct FrameEncoded final {
    FrameStatus status{FrameStatus::kOk};
    std::vector<std::uint8_t> bytes;
};

// ============================================================================
// 解析与序列化
// ============================================================================

/**
 * @brief 从 `in` 解析一个帧。
 * @param in 载荷起点。
 * @param length **当前包的剩余字节数**（不是缓冲区容量，见文件头 §二）。
 * @return 状态、帧、消耗字节数。
 * @note 失败时 `consumed == 0`，便于上层原位置重试或报错。
 */
FrameDecoded DecodeFrame(const std::uint8_t* in, std::size_t length) noexcept;

/**
 * @brief 把一个帧序列化成字节。
 * @return `kInvalidArgument` 表示字段组合不合法（例如 NEW_CONNECTION_ID 的
 *         `connection_id` 长度不在 1~20）。
 */
FrameEncoded EncodeFrame(const Frame& frame);

// ============================================================================
// 便捷构造器（避免调用方手填 type_code 与各个 bool）
// ============================================================================

Frame MakePadding(std::uint64_t count = 1);
Frame MakePing();
Frame MakeAck(std::uint64_t largest_acked, std::uint64_t ack_delay,
              std::uint64_t first_ack_range,
              const std::vector<AckRange>& ranges = {});
Frame MakeAckEcn(std::uint64_t largest_acked, std::uint64_t ack_delay,
                 std::uint64_t first_ack_range, const std::vector<AckRange>& ranges,
                 std::uint64_t ect0, std::uint64_t ect1, std::uint64_t ce);
Frame MakeResetStream(std::uint64_t stream_id, std::uint64_t app_error_code,
                      std::uint64_t final_size);
Frame MakeStopSending(std::uint64_t stream_id, std::uint64_t app_error_code);
Frame MakeCrypto(std::uint64_t offset, std::vector<std::uint8_t> data);
Frame MakeNewToken(std::vector<std::uint8_t> token);
/// @param has_offset 是否编码 Offset 字段；@param has_length 是否编码 Length 字段。
Frame MakeStream(std::uint64_t stream_id, std::uint64_t offset, bool has_offset,
                 bool has_length, bool fin, std::vector<std::uint8_t> data);
Frame MakeMaxData(std::uint64_t max_data);
Frame MakeMaxStreamData(std::uint64_t stream_id, std::uint64_t max_stream_data);
Frame MakeMaxStreams(std::uint64_t max_streams, bool bidirectional);
Frame MakeDataBlocked(std::uint64_t max_data);
Frame MakeStreamDataBlocked(std::uint64_t stream_id, std::uint64_t max_stream_data);
Frame MakeStreamsBlocked(std::uint64_t max_streams, bool bidirectional);
Frame MakeNewConnectionId(std::uint64_t sequence_number, std::uint64_t retire_prior_to,
                          std::vector<std::uint8_t> connection_id,
                          std::vector<std::uint8_t> stateless_reset_token);
Frame MakeRetireConnectionId(std::uint64_t sequence_number);
Frame MakePathChallenge(const std::array<std::uint8_t, 8>& data);
Frame MakePathResponse(const std::array<std::uint8_t, 8>& data);
Frame MakeConnectionCloseTransport(std::uint64_t error_code,
                                   std::uint64_t closing_frame_type,
                                   std::string reason_phrase);
Frame MakeConnectionCloseApplication(std::uint64_t error_code,
                                     std::string reason_phrase);
Frame MakeHandshakeDone();

}  // namespace go2cpp::net::quic
