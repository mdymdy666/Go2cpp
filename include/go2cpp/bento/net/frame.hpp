#pragma once

/**
 * @file frame.hpp
 * @brief 通用帧编解码（MOD-10）：固定 20B 帧头 + 变长载荷 + CRC32 校验。
 *
 * @details 帧布局（网络字节序，大端）：
 *
 *          | 偏移 | 字段     | 类型 | 语义                                |
 *          |------|----------|------|-------------------------------------|
 *          | 0    | magic    | u32  | 固定 0x42454E54（"BENT"）           |
 *          | 4    | version  | u16  | 帧格式版本，当前 1                  |
 *          | 6    | type     | u16  | 上层消息类型（本层不解释）          |
 *          | 8    | flags    | u16  | 上层标志位（本层不解释）            |
 *          | 10   | reserved | u16  | 保留，必须为 0                      |
 *          | 12   | length   | u32  | 载荷字节数（不含 20B 帧头）         |
 *          | 16   | checksum | u32  | CRC-32/IEEE，范围见下               |
 *          | 20   | payload  | ...  | `length` 字节                       |
 *
 *          `checksum` 覆盖 **帧头前 16 字节 + 载荷**。刻意不覆盖 checksum 自身，
 *          也刻意把 length 纳入校验：这样篡改长度字段会被校验和发现，而不是
 *          悄悄改变后续帧的边界。
 *
 * ## 相对 yojimbo 的取舍
 *
 *          yojimbo 的 reliable 包头是**变长**的（1B prefix_byte 用位掩码标记
 *          哪些字段存在，省略全 1 的 ack_bits 字节）；本帧格式用于「先定边界、
 *          再交给上层协议」，宁可固定 20B 头换取解析器的常数时间与可测试性，
 *          不引入变长位打包。yojimbo 的 ack_bits「全 1 即可省略」思想只作为
 *          对照参考，未复制其编码。
 *
 * ## 拒绝策略
 *
 *          非法 magic / 版本不符 / reserved 非 0 / 长度超限 / 校验失败一律
 *          返回错误并**清空流式解码器缓冲**：此时帧边界已不可信，继续在同一条
 *          字节流上猜测边界只会把错误放大，调用方应关闭连接后重连。半包
 *          （数据还不够一整帧）不算错误，保留缓冲等待后续数据。
 *
 *          长度上限默认 1 MiB，且在半包阶段就按帧头声明的 length 判定，因此
 *          攻击者用一个声明超大 length 的 20B 头无法让我们分配对应内存。
 *
 * @author go2cpp contributors
 * @date 2026-10-28
 * @copyright MIT License
 */

#include <cstddef>
#include <cstdint>
#include <vector>

namespace go2cpp::bento::net {

/// 帧魔数：ASCII "BENT"（0x42 0x45 0x4e 0x54）。
inline constexpr std::uint32_t kFrameMagic = 0x42454e54u;

/// 帧格式版本。版本不符直接拒绝，不做向后兼容。
inline constexpr std::uint16_t kFrameVersion = 1;

/**
 * @brief 线上协议标识：magic + version。
 * @details 帧布局（20B 头 + CRC32）是通用的，但**协议归属**由这两个字段
 *          决定。Link 使用默认的 "BENT"/1；任何在帧层之上定义自有协议的
 *          模块（例如 `app::Session` 的会话层）都应当传入独立的
 *          magic/version，这样同一个 `FrameDecoder` 只会接受自己那套协议，
 *          两种协议混接到同一端口时会在 magic/version 阶段明确失败，而不是
 *          把对方的帧解释成「未知类型」后继续解析。
 */
struct FrameFormat {
    std::uint32_t magic = kFrameMagic;      ///< 帧魔数。
    std::uint16_t version = kFrameVersion;  ///< 帧格式版本。
};

/// Link 协议的线上标识（默认值）。
inline constexpr FrameFormat kDefaultFrameFormat{kFrameMagic, kFrameVersion};

/// 固定帧头字节数：magic(4) + version(2) + type(2) + flags(2) + reserved(2) +
/// length(4) + checksum(4) = 20。
inline constexpr std::size_t kGenericFrameHeaderBytes = 20;

/// 单帧载荷上限（1 MiB）。超过该值的帧头在解析阶段即被拒绝。
inline constexpr std::uint32_t kFrameMaxPayload = 1024u * 1024u;

/**
 * @brief 帧编解码结果。
 *
 * @details `kOk` / `kNeedMoreData` 是正常路径；其余值表示这条字节流已经不可信。
 */
enum class FrameStatus : std::uint8_t {
    kOk = 0,                ///< 成功解出一帧。
    kNeedMoreData = 1,      ///< 半包：现有字节数不足一整帧，继续 Append。
    kBadMagic = 2,          ///< 前 4 字节不是 kFrameMagic。
    kBadVersion = 3,        ///< 版本不是 kFrameVersion。
    kBadReserved = 4,       ///< reserved 字段非 0（当前版本要求必须为 0）。
    kLengthExceeded = 5,    ///< length 超过上限（默认 1 MiB）。
    kChecksumMismatch = 6,  ///< CRC32 校验失败（载荷或帧头被破坏）。
    kInvalidArgument = 7,   ///< 调用方传了空输出指针。
    kOutOfMemory = 8,       ///< 载荷缓冲分配失败（解码路径不向外抛异常）。
};

/// @brief 状态名（日志/测试失败信息用）。
const char* FrameStatusName(FrameStatus status) noexcept;

/// @brief 解码后的帧（头部字段 + 载荷副本）。
struct Frame {
    std::uint32_t magic = kFrameMagic;      ///< 解码时填实际值；编码时由 FrameFormat 决定。
    std::uint16_t version = kFrameVersion;  ///< 解码时填实际值；编码时由 FrameFormat 决定。
    std::uint16_t type = 0;                 ///< 上层消息类型。
    std::uint16_t flags = 0;                ///< 上层标志位。
    std::uint16_t reserved = 0;             ///< 必须为 0。
    std::uint32_t checksum = 0;             ///< 解码时填线上值；编码时由 Encode 计算后填入。
    std::vector<std::uint8_t> payload;      ///< 载荷副本。

    /// 载荷字节数（与线上 length 字段一致）。
    std::uint32_t Length() const noexcept {
        return static_cast<std::uint32_t>(payload.size());
    }
};

/**
 * @brief 计算一帧的 CRC-32（覆盖帧头前 16 字节 + 载荷）。
 * @param header_prefix 指向帧头起始处，必须至少可读 16 字节。
 * @param payload 载荷；`payload_bytes == 0` 时可为 nullptr。
 * @param payload_bytes 载荷字节数。
 * @return IEEE 多项式 CRC-32。
 *
 * @details 复用 `go2cpp::hash::Crc32`（slicing-by-8），本函数只是把「头前缀 + 载荷」
 *          两段拼成一次流式计算，避免为了校验把整帧再拷贝一遍。
 */
std::uint32_t ComputeFrameChecksum(const std::uint8_t* header_prefix,
                                   const std::uint8_t* payload,
                                   std::size_t payload_bytes) noexcept;

/**
 * @brief 编码一帧（自动填充 length/checksum，magic/version 用 Link 默认值）。
 * @param frame 待编码帧；只读取 type/flags/reserved/payload。
 * @return 完整字节（20B 头 + 载荷）；载荷超过 kFrameMaxPayload 或 reserved 非 0
 *         时返回空 vector（此时线上不可能产生合法帧，调用方必须处理）。
 *
 * @note 返回空 vector 是唯一失败信号：合法编码最短 20 字节，二者不会混淆。
 */
std::vector<std::uint8_t> Encode(const Frame& frame);

/**
 * @brief 编码一帧，使用调用方指定的 protocol magic/version。
 * @details 上层自有协议（如会话层）用它把自己的帧和 Link 帧区分开：
 *          同一个帧布局、同一套 CRC，但 magic/version 不同，接收端
 *          `FrameDecoder` 只认自己那套。
 * @param format 线上协议标识；magic 为 0 视为非法（返回空 vector）。
 * @return 同 `Encode(const Frame&)`。
 */
std::vector<std::uint8_t> Encode(const Frame& frame, const FrameFormat& format);

/**
 * @brief 同 `Decode`，但按调用方给定的 magic/version 校验。
 * @param format 期望的线上协议标识。
 * @return 同 `Decode`；magic/version 不符分别返回 `kBadMagic`/`kBadVersion`。
 */
FrameStatus Decode(const FrameFormat& format, const std::uint8_t* data, std::size_t size,
                   Frame* out, std::size_t* consumed = nullptr) noexcept;

/**
 * @brief 一次性解析，按调用方给定的协议标识与载荷上限校验。
 * @param format 期望的线上协议标识；magic 为 0 返回 `kInvalidArgument`。
 * @param max_payload 允许的单帧载荷上限（语义同 `FrameDecoder` 构造参数）。
 * @param data 输入字节；`size == 0` 时可为 nullptr。
 * @param size 可用字节数。
 * @param out 输出帧；不可为 nullptr。
 * @param consumed 非空时写入本帧消耗的字节数（只有 `kOk` 时写）。
 * @return 同 `Decode`；`length > max_payload` 返回 `kLengthExceeded`。
 *
 * @details 与 `FrameDecoder` 的可配置上限保持对称：一次性解码不再被硬编码的
 *          1 MiB 束缚，同时对超限声明仍然在读取载荷之前拒绝。
 */
FrameStatus Decode(const FrameFormat& format, std::uint32_t max_payload,
                   const std::uint8_t* data, std::size_t size, Frame* out,
                   std::size_t* consumed = nullptr) noexcept;

/**
 * @brief 一次性解析缓冲区开头的一帧。
 * @param data 输入字节。
 * @param size 可用字节数。
 * @param out 输出帧；不可为 nullptr。
 * @param consumed 非空时写入本帧消耗的字节数（20 + length）；仅 kOk 时写。
 * @return 解析结果。`kNeedMoreData` 表示数据不足，可等待更多数据后重试。
 *
 * @note 本函数无状态：错误后不会修改输入，也不会「吞掉」任何字节；流式场景请用
 *       FrameDecoder，它在错误时会清空自己的缓冲。
 * @note 载荷缓冲分配失败时返回 `kOutOfMemory`：本函数是 `noexcept`，绝不向外抛
 *       异常；此时 `out->payload` 的内容未定义，调用方必须按错误路径丢弃它。
 */
FrameStatus Decode(const std::uint8_t* data, std::size_t size, Frame* out,
                   std::size_t* consumed = nullptr) noexcept;

/**
 * @brief 流式帧解码器：处理粘包与半包。
 *
 * @details 调用方把读到的任意字节 `Append` 进来，再循环 `Next` 取帧：
 *
 * ```cpp
 * decoder.Append(buf, n);
 * Frame frame;
 * while (decoder.Next(&frame) == FrameStatus::kOk) {
 *     Handle(frame);
 * }
 * ```
 *
 *          - 粘包：一次 Append 多帧，Next 依次返回，直到 kNeedMoreData；
 *          - 半包：Next 返回 kNeedMoreData，缓冲保留，下次 Append 后继续；
 *          - 非法/超长/校验失败：返回对应错误并清空缓冲（帧边界已不可信）。
 *
 * ## 线程模型
 *
 *          非线程安全。一个解码器只能由同一线程串行使用；不同连接各用一个实例。
 */
class FrameDecoder {
 public:
    /// @param max_payload 允许的单帧载荷上限；传 0 表示只接受空载荷帧。
    /// @param format 期望的线上协议标识（默认 Link "BENT"/1）。
    explicit FrameDecoder(std::uint32_t max_payload = kFrameMaxPayload,
                          FrameFormat format = kDefaultFrameFormat) noexcept;

    /// 追加收到的字节。空指针或 0 字节是无操作。
    void Append(const std::uint8_t* data, std::size_t size);

    /// 尝试取出下一帧；返回 kOk 表示 out 已填充，kNeedMoreData 表示继续等数据。
    FrameStatus Next(Frame* out);

    /// 丢弃所有未消费字节并复位（错误恢复或连接重建时用）。
    void Clear() noexcept;

    /// 尚未消费的字节数（含半包）。
    std::size_t PendingBytes() const noexcept { return m_buffer.size() - m_offset; }

    /// 配置的载荷上限。
    std::uint32_t MaxPayload() const noexcept { return m_max_payload; }

    /// 期望的线上协议标识。
    const FrameFormat& format() const noexcept { return m_format; }

 private:
    std::vector<std::uint8_t> m_buffer;  ///< 待解析字节；m_offset 之前是已消费前缀。
    std::size_t m_offset = 0;            ///< 已消费前缀长度；Append 时整体压缩。
    std::uint32_t m_max_payload;         ///< 单帧载荷上限。
    FrameFormat m_format;                ///< 期望的 magic/version。
};

}  // namespace go2cpp::bento::net
