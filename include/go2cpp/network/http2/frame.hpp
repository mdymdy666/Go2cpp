/**
 * @file frame.hpp
 * @brief HTTP/2 帧层（RFC 9113 §4 / §6）。
 *
 * ## 分层设计
 *
 * 帧层刻意分成两层，职责不混：
 *
 * 1. **通用帧**：9 字节帧头（length/type/flags/stream id）+ 原始 payload。
 *    这一层只负责"把字节流切成帧"，不解释 payload 语义；
 * 2. **语义解析/构造**：每种帧一个 `ParseXxx` / `MakeXxx`，把 payload 解释成
 *    结构体，或把结构体序列化成帧。
 *
 * 这样新增帧类型只需加一对函数，不用改通用路径；也便于独立测试。
 *
 * ## 已实现的帧
 *
 * DATA / HEADERS / PRIORITY / RST_STREAM / SETTINGS / PUSH_PROMISE / PING /
 * GOAWAY / WINDOW_UPDATE / CONTINUATION / 未知类型（原样透传 payload）。
 *
 * ## 关键校验（都在这一层做，不留给上层）
 *
 * - 帧长不得超过 `SETTINGS_MAX_FRAME_SIZE`（默认 16384）；
 * - 要求 stream id 为 0 的帧（SETTINGS/PING/GOAWAY）若带了非 0 值 -> 协议错误；
 * - 要求 stream id 非 0 的帧若带 0 -> 协议错误；
 * - SETTINGS 的 ack 帧必须空 payload、非 ack 帧长度必须是 6 的倍数；
 * - PING payload 必须恰好 8 字节；
 * - RST_STREAM / WINDOW_UPDATE 的 payload 长度必须分别恰好 4 字节；
 * - **增量解析**：数据不完整时返回 `kNeedMore` 而非报错（这是流式解析的必需）。
 *
 * ## 本文件定义什么
 * - `FrameType` / `FrameFlags` / `ErrorCode` / `FrameStatus`：线上常量与结果码；
 * - `FrameHeader` / `Frame`：通用帧（9 字节头 + 原始 payload），是 IO 与语义层的分界；
 * - 各帧的语义视图（`SettingsFrame`、`PingFrame`、`DataFrame`、`HeadersFrame`、
 *   `RstStreamFrame`、`GoAwayFrame`、`WindowUpdateFrame`、`PriorityFrame`）；
 * - 每对 `Decode*`/`Parse*` 与 `Make*`：字节 <-> 结构体的双向转换。
 *
 * ## 阅读建议
 * 先看 `FrameHeader` 与 `DecodeFrame()`（增量解析的核心），再看任一帧的
 * `ParseXxx` —— 它们的形状完全一致：校验类型/流 id/payload 长度，再取字段。
 *
 * ## 边界与陷阱
 * - `DecodeFrame()` 的 `kNeedMore` 必须**先于**其它错误判断：帧可能只是没收全；
 * - payload 长度上限来自对端 SETTINGS（`Connection::m_peer_max_frame_size`），不是常量；
 * - `ParseXxx` 不做状态机判断（例如 DATA 落在 closed 流），那是 `Connection` 的职责。
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace go2cpp::net::http2 {

/** @brief 帧类型（RFC 9113 §6）。 */
enum class FrameType : std::uint8_t {
    kData = 0x0,
    kHeaders = 0x1,
    kPriority = 0x2,
    kRstStream = 0x3,
    kSettings = 0x4,
    kPushPromise = 0x5,
    kPing = 0x6,
    kGoAway = 0x7,
    kWindowUpdate = 0x8,
    kContinuation = 0x9,
};

/** @brief 帧头标志位（按帧类型解释）。 */
namespace FrameFlags {
inline constexpr std::uint8_t kEndStream = 0x1;
inline constexpr std::uint8_t kAck = 0x1;
inline constexpr std::uint8_t kEndHeaders = 0x4;
inline constexpr std::uint8_t kPadded = 0x8;
inline constexpr std::uint8_t kPriority = 0x20;
}  // namespace FrameFlags

/** @brief HTTP/2 错误码（RFC 9113 §7）。 */
enum class ErrorCode : std::uint32_t {
    kNoError = 0x0,
    kProtocolError = 0x1,
    kInternalError = 0x2,
    kFlowControlError = 0x3,
    kSettingsTimeout = 0x4,
    kStreamClosed = 0x5,
    kFrameSizeError = 0x6,
    kRefusedStream = 0x7,
    kCancel = 0x8,
    kCompressionError = 0x9,
    kConnectError = 0xa,
    kEnhanceYourCalm = 0xb,
    kInadequateSecurity = 0xc,
    kHttp11Required = 0xd,
};

/** @brief 帧解析状态。 */
enum class FrameStatus : std::uint8_t {
    kOk,
    /// 数据不完整，需要更多字节（增量解析）。
    kNeedMore,
    /// 帧头合法但语义非法（长度、stream id、payload 形状等）。
    kProtocolError,
    /// 帧长超过 SETTINGS_MAX_FRAME_SIZE。
    kFrameSizeError,
    /// 不支持的帧类型（可透传，但调用方需知道）。
    kUnknownType,
};

const char* ToString(FrameStatus status) noexcept;
const char* ToString(FrameType type) noexcept;

/** @brief 帧头（9 字节线上格式）。 */
/**
 * @brief 帧头（9 字节线上格式）。
 *
 * ## 定位
 * 被 `DecodeFrameHeader()` 填充、`EncodeFrameHeader()` 序列化；
 * `Connection::TakeFrame()` 先解析它，再据此决定还要读多少字节。
 */
struct FrameHeader final {
    /// 24 位 payload 长度（不含 9 字节帧头）。
    std::uint32_t length{0};
    FrameType type{FrameType::kData};
    std::uint8_t flags{0};
    /// 31 位流标识；保留最高位为 0。
    std::uint32_t stream_id{0};
};

/** @brief 一个完整帧：帧头 + 原始 payload。 */
/**
 * @brief 一个完整帧：帧头 + 原始 payload。
 *
 * ## 定位
 * `Connection` 与 `Transport` 之间的通用单位：`TakeFrame()` 产出，
 * `WriteFrame()` 消费；语义层只通过 `ParseXxx`/`MakeXxx` 触碰它。
 *
 * ## 不变量
 * `payload.size() == header.length`（`MakeXxx` 维护，`DecodeFrame()` 保证）。
 */
struct Frame final {
    FrameHeader header;
    std::vector<std::uint8_t> payload;
};

/** @brief 帧头固定长度。 */
inline constexpr std::size_t kFrameHeaderSize = 9;

/** @brief 默认 SETTINGS_MAX_FRAME_SIZE（RFC 9113 §6.5.2）。 */
inline constexpr std::uint32_t kDefaultMaxFrameSize = 16384;

/** @brief 协议允许的 SETTINGS_MAX_FRAME_SIZE 区间。 */
inline constexpr std::uint32_t kMinMaxFrameSize = 16384;
inline constexpr std::uint32_t kMaxMaxFrameSize = 16777215;

/**
 * @brief 解析 9 字节帧头。
 * @param in 输入缓冲。
 * @param length 可用字节数；小于 9 返回 `kNeedMore`。
 * @param out 成功时写入帧头。
 */
FrameStatus DecodeFrameHeader(const std::uint8_t* in, std::size_t length,
                              FrameHeader* out) noexcept;

/** @brief 序列化帧头到 9 字节。 */
void EncodeFrameHeader(const FrameHeader& header, std::uint8_t out[kFrameHeaderSize]) noexcept;

/**
 * @brief 从字节流中解析出一个完整帧（帧头 + payload）。
 * @param in 输入缓冲。
 * @param length 可用字节数。
 * @param out 成功时写入帧（payload 已拷贝）。
 * @param consumed 成功时写入消耗的字节数（9 + payload 长度）。
 * @param max_frame_size 帧长上限（来自 SETTINGS）。
 * @return 状态。`kNeedMore` 表示帧还不完整；`kFrameSizeError` 表示声明长度超限。
 * @note 调用方应**先看 `kNeedMore` 再判断其他错误**：帧可能只是还没收全。
 */
FrameStatus DecodeFrame(const std::uint8_t* in, std::size_t length, Frame* out,
                        std::size_t* consumed,
                        std::uint32_t max_frame_size = kDefaultMaxFrameSize) noexcept;

// ---------------------------------------------------------------------------
// SETTINGS（§6.5）
// ---------------------------------------------------------------------------

/** @brief SETTINGS 参数标识。 */
enum class SettingsId : std::uint16_t {
    kHeaderTableSize = 0x1,
    kEnablePush = 0x2,
    kMaxConcurrentStreams = 0x3,
    kInitialWindowSize = 0x4,
    kMaxFrameSize = 0x5,
    kMaxHeaderListSize = 0x6,
};

/** @brief 一个 SETTINGS 参数。 */
struct SettingsEntry final {
    std::uint16_t id{0};
    std::uint32_t value{0};
};

/** @brief SETTINGS 帧的语义视图。 */
/**
 * @brief SETTINGS 帧的语义视图。
 *
 * ## 定位
 * `Connection::FinishSettingsHandshake()` 与 `ReadEvent()` 的 SETTINGS 分支使用。
 */
struct SettingsFrame final {
    bool ack{false};
    std::vector<SettingsEntry> entries;
};

/** @brief 解析 SETTINGS payload（不含帧头）。 */
FrameStatus ParseSettings(const Frame& frame, SettingsFrame* out, std::string* error);

/** @brief 构造 SETTINGS 帧。 */
Frame MakeSettings(const std::vector<SettingsEntry>& entries);

/** @brief 构造 SETTINGS ACK 帧（空 payload）。 */
Frame MakeSettingsAck();

// ---------------------------------------------------------------------------
// PING（§6.7）
// ---------------------------------------------------------------------------

/** @brief PING 帧语义视图。 */
/**
 * @brief PING 帧语义视图。
 *
 * ## 定位
 * `Connection` 在两处自动回 ACK：握手期 `FinishSettingsHandshake()` 与
 * `ReadEvent()` 的 PING 分支。
 */
struct PingFrame final {
    bool ack{false};
    /// 恰好 8 字节不透明数据。
    std::uint64_t opaque_data{0};
};

FrameStatus ParsePing(const Frame& frame, PingFrame* out, std::string* error);
Frame MakePing(std::uint64_t opaque_data, bool ack);

// ---------------------------------------------------------------------------
// DATA（§6.1）
// ---------------------------------------------------------------------------

/**
 * @brief DATA 帧的语义视图。
 *
 * ## 定位
 * `Connection::WriteData()` 构造；`ReadEvent()` 的 DATA 分支解析并做接收流控。
 *
 * ## 不变量
 * `data` 已剥掉 padding（`ParseData()` 负责），长度即实际业务字节数。
 */
struct DataFrame final {
    std::uint32_t stream_id{0};
    bool end_stream{false};
    std::vector<std::uint8_t> data;
};

FrameStatus ParseData(const Frame& frame, DataFrame* out, std::string* error);
Frame MakeData(std::uint32_t stream_id, bool end_stream,
               const std::vector<std::uint8_t>& data);

// ---------------------------------------------------------------------------
// HEADERS / CONTINUATION（§6.2 / §6.10）
// ---------------------------------------------------------------------------

/**
 * @brief HEADERS / CONTINUATION 的语义视图。
 *
 * ## 定位
 * 由 `ParseHeaders()` / `ParseContinuation()` 产出；`Connection::AssembleHeaderBlock()`
 * 把多片 `header_block` 拼成一整块，再交给 HPACK 解码。
 *
 * ## 不变量
 * `end_headers == false` 表示后面还有 CONTINUATION；`end_stream` 只可能来自 HEADERS。
 */
struct HeadersFrame final {
    std::uint32_t stream_id{0};
    bool end_stream{false};
    bool end_headers{false};
    /// 原始头部块片段（HPACK 编码，可能跨 CONTINUATION）。
    std::vector<std::uint8_t> header_block;
};

FrameStatus ParseHeaders(const Frame& frame, HeadersFrame* out, std::string* error);
Frame MakeHeaders(std::uint32_t stream_id, bool end_stream, bool end_headers,
                  const std::vector<std::uint8_t>& header_block);

FrameStatus ParseContinuation(const Frame& frame, HeadersFrame* out, std::string* error);
Frame MakeContinuation(std::uint32_t stream_id, bool end_headers,
                       const std::vector<std::uint8_t>& header_block);

// ---------------------------------------------------------------------------
// RST_STREAM / GOAWAY / WINDOW_UPDATE / PRIORITY
// ---------------------------------------------------------------------------

/**
 * @brief RST_STREAM 帧的语义视图。
 *
 * ## 定位
 * `Connection::ResetStream()` 发送；`ReadEvent()` 收到后把对应流置 closed。
 *
 * ## 不变量
 * `stream_id` 非 0（协议要求），`error_code` 是 RFC 9113 §7 的错误码。
 */
struct RstStreamFrame final {
    std::uint32_t stream_id{0};
    ErrorCode error_code{ErrorCode::kNoError};
};

FrameStatus ParseRstStream(const Frame& frame, RstStreamFrame* out, std::string* error);
Frame MakeRstStream(std::uint32_t stream_id, ErrorCode error_code);

/**
 * @brief GOAWAY 帧的语义视图。
 *
 * ## 定位
 * `Connection::SendGoAway()` 构造；`ReadEvent()` 收到后记录 `last_stream_id`
 * 并把连接迁到 `kDraining`。
 *
 * ## 不变量
 * `last_stream_id` 是「对端已处理的最大流 id」，`debug_data` 可为空。
 */
struct GoAwayFrame final {
    std::uint32_t last_stream_id{0};
    ErrorCode error_code{ErrorCode::kNoError};
    std::vector<std::uint8_t> debug_data;
};

FrameStatus ParseGoAway(const Frame& frame, GoAwayFrame* out, std::string* error);
Frame MakeGoAway(std::uint32_t last_stream_id, ErrorCode error_code,
                 const std::vector<std::uint8_t>& debug_data = {});

/**
 * @brief WINDOW_UPDATE 帧的语义视图。
 *
 * ## 定位
 * `ReadEvent()` 用它补回发送窗口；`stream_id == 0` 表示连接级窗口。
 *
 * ## 不变量
 * `increment` 是 31 位非零值（为 0 属于协议错误，`ParseWindowUpdate()` 已拒绝）。
 */
struct WindowUpdateFrame final {
    std::uint32_t stream_id{0};
    /// 31 位增量；为 0 是协议错误。
    std::uint32_t increment{0};
};

FrameStatus ParseWindowUpdate(const Frame& frame, WindowUpdateFrame* out, std::string* error);
Frame MakeWindowUpdate(std::uint32_t stream_id, std::uint32_t increment);

/**
 * @brief PRIORITY 帧的语义视图（解析保留但不用）。
 *
 * ## 定位
 * 能解析但**不参与调度**（RFC 9113 已弃用优先级树）；保留是为了不把 PRIORITY
 * 帧当未知帧丢弃。
 */
struct PriorityFrame final {
    std::uint32_t stream_id{0};
    /// 依赖的流 id（最高位是独占标志）。
    std::uint32_t depends_on{0};
    bool exclusive{false};
    std::uint8_t weight{0};
};

FrameStatus ParsePriority(const Frame& frame, PriorityFrame* out, std::string* error);
Frame MakePriority(std::uint32_t stream_id, std::uint32_t depends_on, bool exclusive,
                   std::uint8_t weight);

}  // namespace go2cpp::net::http2
