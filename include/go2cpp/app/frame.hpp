#pragma once

/**
 * @file frame.hpp
 * @brief 通用应用层帧（HTTP/2 风格 9 字节头）+ 流式编解码。
 *
 * @details 本文件是 `go2cpp::app` 应用层框架的**线格式层**：把「一条字节流」
 *          切分成带流标识的帧。设计直接对照 RFC 7540 §4 的帧头：
 *
 *          | 偏移 | 字段      | 位宽 | 语义                                  |
 *          |------|-----------|------|---------------------------------------|
 *          | 0    | length    | 24   | 载荷字节数（不含 9B 帧头）            |
 *          | 3    | type      | 8    | 帧类型（DATA/HEADERS/...）            |
 *          | 4    | flags     | 8    | 帧标志（END_STREAM/ACK/...）          |
 *          | 5    | R + id    | 1+31 | R 保留位必须为 0，其余为流 id         |
 *          | 9    | payload   | ...  | `length` 字节                         |
 *
 *          HTTP/2 的帧头是 9 字节、24 位长度、31 位流 id，本实现保持同样的
 *          布局，只把语义面裁剪到资源分发/KV 流需要的部分。**没有 magic 和
 *          version**：应用层帧总是运行在一条已经由传输层识别过的通道里
 *          （内存管道、Bento STREAM 通道、HTTP body），不需要再用魔数自证；
 *          需要跨进程识别协议时用 `bento_bridge` 的 20B Bento 帧封装。
 *
 * ## 帧类型
 *
 *          - `DATA`：流载荷字节；
 *          - `HEADERS`：元数据（`KvTable` 二进制编码），也用于资源元数据；
 *          - `PRIORITY`：依赖树 + 权重（RFC 7540 §5.3）；
 *          - `RST_STREAM`：取消/重置单条流；
 *          - `SETTINGS`：连接级参数协商（初始窗口、并发上限、帧大小等）；
 *          - `PING`：连接保活/时延探测（8B opaque）；
 *          - `GOAWAY`：优雅关闭，携带最后处理的流 id 与错误码；
 *          - `WINDOW_UPDATE`：连接级（stream 0）或流级窗口增量；
 *          - `RESOURCE_INDEX`：应用层扩展，资源索引（多个资源的 k=v 元数据）。
 *
 *          资源数据本身**不新增帧类型**：复用 HEADERS（元数据）+ DATA
 *          （块数据）。这是「通用化」的关键——流表只理解 HEADERS/DATA，
 *          上层用 `app.kind` 区分 kv/resource/bytes。
 *
 * ## 长度上限
 *
 *          默认单帧载荷上限 1 MiB。与 HTTP/2 的 16 KiB 默认值不同：资源
 *          分块需要更大的载荷才能跑满带宽；上限在解码前按 24 位长度字段
 *          判定，因此声明超大长度的攻击帧不会触发大块内存分配。
 *
 * ## 错误与边界
 *
 *          - `DecodeFrame` 是**无状态**的：失败不消费任何字节；
 *          - `FrameDecoder` 是**有状态**的流式解码器：处理粘包/半包，遇到
 *            非法帧时清空缓冲（帧边界已不可信，继续猜测只会放大错误）；
 *          - 所有整数一律网络字节序（大端）；
 *          - 流 id 的 R 位（最高位）必须为 0，否则判为 `kInvalidStreamId`。
 *
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace go2cpp::app {

/// 固定帧头字节数：length(3) + type(1) + flags(1) + R+stream_id(4) = 9。
inline constexpr std::size_t kFrameHeaderBytes = 9;

/// 默认单帧载荷上限：1 MiB。
inline constexpr std::uint32_t kDefaultMaxFramePayload = 1024u * 1024u;

/// 协议允许的最大单帧载荷（24 位长度字段）。
inline constexpr std::uint32_t kMaxFramePayloadLimit = (1u << 24) - 1u;

/// 最大合法流 id（31 位，R 位必须为 0）。
inline constexpr std::uint32_t kMaxStreamId = 0x7fffffffu;

/**
 * @brief 帧类型。
 * @details 数值对照 RFC 7540 §6 的类型编号（0x0..0x8），0x0d 是本框架扩展的
 *          资源索引帧。0x05（PUSH_PROMISE）与 0x09（CONTINUATION）未使用：
 *          本框架不支持服务端推送，HEADERS 也要求单帧装下（超限由上层分区）。
 */
enum class FrameType : std::uint8_t {
    kData = 0x00,           ///< 流数据。
    kHeaders = 0x01,        ///< 元数据（KvTable 二进制）。
    kPriority = 0x02,       ///< 优先级（parent + weight）。
    kRstStream = 0x03,      ///< 重置/取消单条流。
    kSettings = 0x04,       ///< 连接设置。
    kPing = 0x06,           ///< 保活/探测。
    kGoAway = 0x07,         ///< 优雅关闭。
    kWindowUpdate = 0x08,   ///< 窗口增量。
    kResourceIndex = 0x0d,  ///< 应用层扩展：资源索引。
};

/// @brief 帧类型名（日志/测试失败信息用）。
const char* FrameTypeName(FrameType type) noexcept;

/**
 * @brief 错误码。
 * @details 0..7 对照 HTTP/2 错误码，8.. 是本框架的资源/校验扩展。
 */
enum class ErrorCode : std::uint32_t {
    kNoError = 0,           ///< 无错误。
    kProtocolError = 1,     ///< 协议错误（非法状态转换/非法帧）。
    kInternalError = 2,     ///< 本端内部错误。
    kFlowControlError = 3,  ///< 流控违规（超额发送/窗口溢出）。
    kStreamClosed = 4,      ///< 流已关闭。
    kFrameSizeError = 5,    ///< 帧尺寸违规。
    kRefusedStream = 6,     ///< 并发流超限/流被拒绝。
    kCancel = 7,            ///< 取消。
    kChecksumError = 8,     ///< 资源块/整体校验失败。
    kResourceNotFound = 9,  ///< 资源不存在。
    kResumeOutOfRange = 10, ///< 断点续传起点越界。
    kInvalidChunk = 11,     ///< 块索引/偏移不连续。
};

/// @brief 错误码名（日志/测试失败信息用）。
const char* ErrorCodeName(ErrorCode code) noexcept;

/// @brief 帧标志位（flags 是 8 位字段，低 5 位已定义）。
enum FrameFlag : std::uint8_t {
    kFlagNone = 0x00,        ///< 无标志。
    kFlagEndStream = 0x01,   ///< 本帧是该方向最后一帧。
    kFlagAck = 0x02,         ///< SETTINGS/PING 的确认。
    kFlagEndHeaders = 0x04,  ///< 元数据结束（为兼容 HTTP/2 语义保留）。
    kFlagResume = 0x08,      ///< GOAWAY/HEADERS：携带断点续传起点。
    kFlagPriority = 0x10,    ///< HEADERS 附带优先级字段（未使用，保留）。
};

/**
 * @brief SETTINGS 参数 id。
 * @details 前 4 项对照 HTTP/2；本框架额外定义小 KV 阈值与资源块大小，
 *          让两端可以在握手时协商分块策略。
 */
enum class SettingId : std::uint16_t {
    kMaxConcurrentStreams = 1,  ///< 入站并发流上限。
    kInitialWindowSize = 2,     ///< 初始流窗口（字节）。
    kMaxFrameSize = 3,          ///< 单帧载荷上限。
    kMaxHeaderListSize = 4,     ///< HEADERS 载荷上限（字节）。
    kSmallKvThreshold = 5,      ///< 小 KV 阈值（字节）。
    kResourceChunkSize = 6,     ///< 资源块建议大小（字节）。
};

/// @brief 一条 SETTINGS 参数。
struct SettingsEntry {
    SettingId id = SettingId::kMaxConcurrentStreams;  ///< 参数 id。
    std::uint32_t value = 0;                          ///< 参数值。
};

/// @brief 一帧（头部字段 + 载荷副本）。
struct Frame {
    std::uint32_t stream_id = 0;          ///< 流 id（0 = 连接级）。
    FrameType type = FrameType::kData;    ///< 帧类型。
    std::uint8_t flags = kFlagNone;       ///< 标志位。
    std::vector<std::uint8_t> payload;    ///< 载荷副本。

    /// 载荷字节数。
    std::uint32_t Length() const noexcept {
        return static_cast<std::uint32_t>(payload.size());
    }

    /// @brief 是否置位给定标志。
    bool HasFlag(std::uint8_t flag) const noexcept { return (flags & flag) != 0; }

    /// @brief 置位/清除标志。
    void SetFlag(std::uint8_t flag, bool on = true) noexcept {
        if (on) {
            flags = static_cast<std::uint8_t>(flags | flag);
        } else {
            flags = static_cast<std::uint8_t>(flags & ~flag);
        }
    }

    /// @brief 该帧是否携带 END_STREAM。
    bool EndStream() const noexcept { return HasFlag(kFlagEndStream); }
};

/// @brief 帧解码结果。
enum class FrameStatus : std::uint8_t {
    kOk = 0,              ///< 成功解出一帧。
    kNeedMoreData = 1,    ///< 半包，等待更多字节。
    kLengthExceeded = 2,  ///< 声明的载荷长度超过上限。
    kInvalidType = 3,     ///< 未知帧类型。
    kInvalidStreamId = 4, ///< R 位非 0。
    kInvalidArgument = 5, ///< 空输出指针/空输入。
};

/// @brief 帧解码结果名。
const char* FrameStatusName(FrameStatus status) noexcept;

/**
 * @brief 一次性解析缓冲区开头的一帧（默认 1 MiB 上限）。
 * @param data 输入字节；`size == 0` 时可为 nullptr。
 * @param size 可用字节数。
 * @param out 输出帧；不可为 nullptr。
 * @param consumed 非空时写入本帧消耗的字节数（仅 `kOk`）。
 * @return 解析结果；`kNeedMoreData` 表示数据不足，可等待更多数据后重试。
 * @note 无状态：失败时不修改输入，也不消费字节。
 */
FrameStatus DecodeFrame(const std::uint8_t* data, std::size_t size, Frame* out,
                        std::size_t* consumed = nullptr) noexcept;

/// @brief 同 `DecodeFrame`，但由调用方指定载荷上限。
FrameStatus DecodeFrame(const std::uint8_t* data, std::size_t size,
                        std::uint32_t max_payload, Frame* out,
                        std::size_t* consumed = nullptr) noexcept;

/**
 * @brief 编码一帧（自动填充 24 位长度字段）。
 * @return 完整字节（9B 头 + 载荷）；载荷超过 `kMaxFramePayloadLimit`、R 位
 *         非 0 或类型未知时返回空 vector（合法帧最短 9 字节，不会混淆）。
 */
std::vector<std::uint8_t> EncodeFrame(const Frame& frame);

/// @brief 编码追加到 `out`（便于批量发送时复用缓冲）。
bool AppendEncodedFrame(const Frame& frame, std::vector<std::uint8_t>* out);

/**
 * @brief 流式帧解码器：处理粘包与半包。
 *
 * @details 用法：
 * ```cpp
 * FrameDecoder decoder;
 * decoder.Append(buf, n);
 * Frame frame;
 * while (decoder.Next(&frame) == FrameStatus::kOk) { ... }
 * ```
 * 遇到非法帧时 `Next` 返回错误并清空缓冲：此时帧边界已不可信，调用方
 * 应关闭连接后重建，而不是在同一条字节流上继续猜测。
 */
class FrameDecoder {
   public:
    /// @param max_payload 允许的单帧载荷上限；默认 1 MiB。
    explicit FrameDecoder(std::uint32_t max_payload = kDefaultMaxFramePayload) noexcept;

    /// @brief 追加收到的字节。
    void Append(const std::uint8_t* data, std::size_t size);

    /// @brief 取出下一帧；`kOk` 表示 `out` 已填充。
    FrameStatus Next(Frame* out);

    /// @brief 尚未消费的字节数（含半包）。
    std::size_t PendingBytes() const noexcept { return m_buffer.size() - m_offset; }

    /// @brief 丢弃所有未消费字节并复位。
    void Clear() noexcept;

    /// @brief 配置的载荷上限。
    std::uint32_t MaxPayload() const noexcept { return m_max_payload; }

   private:
    std::vector<std::uint8_t> m_buffer;  ///< 待解析字节。
    std::size_t m_offset = 0;            ///< 已消费前缀。
    std::uint32_t m_max_payload = kDefaultMaxFramePayload;  ///< 载荷上限。
};

// ---------------------------------------------------------------------------
// 整数/字符串编解码小工具（大端；供载荷格式复用）
// ---------------------------------------------------------------------------

/// @brief 向缓冲追加 16 位大端整数。
void PutU16(std::vector<std::uint8_t>& out, std::uint16_t value);
/// @brief 向缓冲追加 32 位大端整数。
void PutU32(std::vector<std::uint8_t>& out, std::uint32_t value);
/// @brief 向缓冲追加 64 位大端整数。
void PutU64(std::vector<std::uint8_t>& out, std::uint64_t value);
/// @brief 向缓冲追加 varint（LEB128）。
void PutVarint(std::vector<std::uint8_t>& out, std::uint64_t value);
/// @brief 向缓冲追加长度前缀字符串（varint 长度 + 字节）。
void PutString(std::vector<std::uint8_t>& out, std::string_view value);

/// @brief 读取 16 位大端整数（调用方保证至少 2 字节）。
std::uint16_t GetU16(const std::uint8_t* in) noexcept;
/// @brief 读取 32 位大端整数（调用方保证至少 4 字节）。
std::uint32_t GetU32(const std::uint8_t* in) noexcept;
/// @brief 读取 64 位大端整数（调用方保证至少 8 字节）。
std::uint64_t GetU64(const std::uint8_t* in) noexcept;

/**
 * @brief 读取 varint。
 * @param in 输入。
 * @param size 可用字节数。
 * @param value 输出值。
 * @param consumed 输出消耗字节数（仅成功时写）。
 * @return 成功返回 true；截断/溢出返回 false。
 */
bool GetVarint(const std::uint8_t* in, std::size_t size, std::uint64_t* value,
               std::size_t* consumed) noexcept;

/**
 * @brief 读取长度前缀字符串。
 * @return 成功返回 true；截断/长度超限返回 false。
 */
bool GetString(const std::uint8_t* in, std::size_t size, std::string* value,
               std::size_t* consumed, std::size_t max_bytes = 16u * 1024u * 1024u) noexcept;

// ---------------------------------------------------------------------------
// 标准帧载荷编解码
// ---------------------------------------------------------------------------

/// @brief SETTINGS 载荷：`entry_count(2) + N * (id(2) + value(4))`。
std::vector<std::uint8_t> EncodeSettings(const std::vector<SettingsEntry>& entries);
/// @brief 解码 SETTINGS 载荷。
bool DecodeSettings(const std::uint8_t* data, std::size_t size,
                    std::vector<SettingsEntry>* entries);

/// @brief GOAWAY 载荷：`last_stream_id(4) + error(4) + debug(varint + bytes)`。
std::vector<std::uint8_t> EncodeGoAway(std::uint32_t last_stream_id, ErrorCode error,
                                       std::string_view debug = {});
/// @brief 解码 GOAWAY 载荷。
bool DecodeGoAway(const std::uint8_t* data, std::size_t size,
                  std::uint32_t* last_stream_id, ErrorCode* error, std::string* debug);

/// @brief RST_STREAM 载荷：`error(4)`。
std::vector<std::uint8_t> EncodeRstStream(ErrorCode error);
/// @brief 解码 RST_STREAM 载荷。
bool DecodeRstStream(const std::uint8_t* data, std::size_t size, ErrorCode* error);

/// @brief WINDOW_UPDATE 载荷：`increment(4，31 位有效)`。
std::vector<std::uint8_t> EncodeWindowUpdate(std::uint32_t increment);
/// @brief 解码 WINDOW_UPDATE 载荷；增量为 0 视为非法。
bool DecodeWindowUpdate(const std::uint8_t* data, std::size_t size,
                        std::uint32_t* increment);

/// @brief PRIORITY 载荷：`parent_id(4) + weight(2)`。
std::vector<std::uint8_t> EncodePriority(std::uint32_t parent_id, std::uint16_t weight);
/// @brief 解码 PRIORITY 载荷。
bool DecodePriority(const std::uint8_t* data, std::size_t size, std::uint32_t* parent_id,
                    std::uint16_t* weight);

/// @brief PING 载荷：8 字节 opaque（不足补 0，多余截断）。
std::vector<std::uint8_t> EncodePing(const std::array<std::uint8_t, 8>& opaque);
/// @brief 解码 PING 载荷（要求恰好 8 字节）。
bool DecodePing(const std::uint8_t* data, std::size_t size,
                std::array<std::uint8_t, 8>* opaque);

}  // namespace go2cpp::app
