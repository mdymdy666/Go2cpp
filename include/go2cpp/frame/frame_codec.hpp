#pragma once

/**
 * @file frame_codec.hpp
 * @brief 通用帧编解码：magic/version 定界 + 变长载荷 + 可插拔校验和。
 *
 * @details
 * 本模块只解决「字节流 → 一条条完整帧」这一层问题，**不解释任何载荷语义**：
 * `type`/`flags` 是原样搬运的字节，帧里放什么由上层协议决定。任何自定义二进制
 * 协议（RDRL/RDCH 控制面、游戏封包、IM 帧、RPC 请求）都可以用同一套机制，
 * 差别只在 `FrameSpec`。
 *
 * ## 1. 线上布局（默认 12 字节帧头）
 *
 * ```text
 *  偏移  长度          字段      说明
 *  0     4             magic     4 字节魔数（"RDRL"/"RDCH" 等），必须逐字节相等
 *  4     1             version   版本，必须与 FrameSpec::version 相等，不做协商降级
 *  5     1             type      上层消息类型（本层不解释）
 *  6     2             flags     上层标志位，小端（本层不解释）
 *  8     4             length    载荷字节数，小端；上限 FrameSpec::max_payload
 *  12    header_len-12 reserved  扩展区（仅当 header_len > 12）：编码写 0，解码跳过，
 *                                但参与校验和计算
 *  header_len          length    payload
 *  ...                4         checksum（仅当 FrameSpec::checksum 非空，小端）
 * ```
 *
 * 校验和覆盖 **帧头前 header_len 字节 + payload**（不含校验和自身）。把 length
 * 纳入校验是刻意的：篡改长度字段会被校验和发现，而不是悄悄改变后续帧的边界。
 * 校验和放在**帧尾**而不是帧头，是为了让 12 字节帧头与 RDRL/RDCH 现有布局逐字节
 * 一致（见 §3）；关闭校验时整帧就是 `header_len + length`。
 *
 * 所有多字节整数一律**小端**。
 *
 * ## 2. 半包 / 粘包 / 越界 / 校验
 *
 * - **半包**：`Feed()` 可以任意切分喂入（1 字节一次也行）。未成帧的尾部留在
 *   内部缓冲里，下次 `Feed()` 自动续接；成功时 `*consumed == len`，调用方丢弃
 *   已喂入的字节即可。
 * - **粘包**：一次 `Feed()` 里完整帧全部解出并**追加**到 `*frames`，顺序不变。
 * - **越界**：帧头里的 `length > FrameSpec::max_payload` 在**帧头刚凑齐时**
 *   立即报 `kLengthExceeded`，不会按声明长度预分配、也不会把攻击者声明的
 *   超大载荷攒进内存（缓冲上限 = 一帧最大字节数，见 `FrameSpec::MaxFrameBytes`）。
 * - **校验失败**：报 `kChecksumMismatch`。`FrameSpec::checksum` 为空表示不校验，
 *   此时帧尾少 4 字节，载荷被篡改也照样解出（由调用方自己承担）。
 *
 * ## 3. 与 RDRL / RDCH 的关系
 *
 * RDRL 协议（`JinShanProject` 的 `docs/05-detailed/protocol.md` §3.1）的帧格式是
 * `magic "RDRL" | version 1 | type 1 | flags 2 | length 4 | payload | crc32 4`，
 * 全部小端，CRC32 覆盖帧头 12 字节 + payload。这正是本模块
 * `FrameSpec{magic="RDRL", version=1, header_len=12, max_payload=65536, checksum=CRC32 IEEE}`
 * 的默认取值：
 *
 * ```cpp
 * FrameCodec rdrl(MakeFrameSpec(MakeMagic("RDRL"), 1, 65536));
 * FrameCodec rdch(MakeFrameSpec(MakeMagic("RDCH"), 1, 4 * 1024 * 1024));
 * ```
 *
 * 两个 codec 各自只认自己的 magic：把 RDRL 帧喂给 RDCH 的 codec 会得到
 * `kBadMagic`，不会把对方的字节流解释成「未知 type」后继续解析。
 *
 * ## 4. 状态与线程模型
 *
 * `FrameCodec` **有状态**（内部保存半包），一个实例同一时刻只能被一个线程
 * `Feed()`；需要每连接一线程时，每条连接各建一个实例。`Encode()` 只读 spec，
 * 同一实例可被多线程并发调用（自定义 `checksum` 必须无共享状态时才能并发）。
 * `Reset()` 丢弃半包并释放缓冲（重连 / 换协议时调用）。
 *
 * ## 5. 错误表示
 *
 * 这里是 `go2cpp::frame::Error`（**值类型**：错误码 + 文本），不是
 * `go2cpp::Error`（抽象接口，无法作为出参写入）。需要并入项目错误体系时：
 * `const auto e = go2cpp::NewError(err.message);`。错误码见 `ErrorCode`，
 * 四类畸形（magic / version / 长度 / 校验和）各有独立码，调用方可区分处理。
 *
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace go2cpp::frame {

/// 帧头最小字节数：magic(4) + version(1) + type(1) + flags(2) + length(4)。
inline constexpr int kMinHeaderLen = 12;
/// 帧头默认字节数（RDRL/RDCH 均为 12）。
inline constexpr int kDefaultHeaderLen = kMinHeaderLen;
/// 单帧载荷默认上限（字节），对齐 RDRL 的 64 KiB。
inline constexpr int kDefaultMaxPayload = 65536;
/// 帧头允许的最大字节数（扩展区最多 4084 字节）：只为拦住明显写错的配置。
inline constexpr int kMaxHeaderLen = 4096;
/// 魔数字节数。
inline constexpr std::size_t kMagicBytes = 4;
/// 校验和字段字节数（放在帧尾，仅当启用校验时存在）。
inline constexpr std::size_t kChecksumBytes = 4;

/**
 * @brief 默认校验和：CRC-32/IEEE。
 *
 * @details 反射多项式 `0xEDB88320`、初值 `0xFFFFFFFF`、结果异或 `0xFFFFFFFF`，
 *          与 zlib `crc32()`、Go `crc32.ChecksumIEEE` 完全一致。实现直接复用
 *          `go2cpp::hash::Crc32::ChecksumIEEE`（slicing-by-8），本函数只是把
 *          `(data, len)` 适配成 `ByteSpan`，不重复造 CRC。
 *
 * @param data 待校验字节（帧头 + payload），`len == 0` 时可为 `nullptr`。
 * @param len 字节数。
 * @return CRC-32/IEEE 值。
 */
std::uint32_t Crc32IeeeFrameChecksum(const std::uint8_t* data,
                                     std::size_t len) noexcept;

/**
 * @brief 帧编解码错误码。
 *
 * @details 成功恒为 `kNone`；其余值按「调用方错误 / 配置错误 / 字节流畸形」
 *          分开，便于上层决定是断开连接还是修代码。
 */
enum class ErrorCode : std::uint8_t {
    kNone = 0,             ///< 成功。
    kInvalidArgument = 1,  ///< 出参为空，或 `data == nullptr` 但 `len != 0`。
    kInvalidSpec = 2,      ///< `FrameSpec` 非法（header_len / max_payload 越界）。
    kPayloadTooLarge = 3,  ///< `Encode()` 的载荷超过 `max_payload`（不截断）。
    kLengthExceeded = 4,   ///< 解码时帧头声明的 `length` 超过 `max_payload`。
    kBadMagic = 5,         ///< 前 4 字节不是 `FrameSpec::magic`。
    kBadVersion = 6,       ///< 版本字节与 `FrameSpec::version` 不相等。
    kChecksumMismatch = 7, ///< 校验和不匹配（帧头或载荷被破坏）。
    kOutOfMemory = 8,      ///< 缓冲分配失败（解码路径不向调用方抛异常）。
};

/// @brief 错误码短名（日志 / 测试失败信息用，不带空格）。
const char* ErrorCodeName(ErrorCode code) noexcept;

/**
 * @brief 帧编解码的错误值（`ErrorCode` + 面向人的文本）。
 *
 * @details 纯值类型，可拷贝、可比较 `code`；`message` 里带具体数值
 *          （如 `length 70000 exceeds max_payload 65536`），便于直接打日志。
 *          与 `go2cpp::ErrorPtr` 体系的关系见文件头 §5。
 */
struct Error {
    /// 错误码；`kNone` 表示成功。
    ErrorCode code = ErrorCode::kNone;
    /// 错误文本（含具体数值），成功时为空。
    std::string message;

    /// @brief 是否成功（`code == kNone`）。
    bool Ok() const noexcept { return code == ErrorCode::kNone; }
    /// @brief 复位为成功状态并清空文本。
    void Clear() noexcept {
        code = ErrorCode::kNone;
        message.clear();
    }
};

/**
 * @brief 一帧的逻辑内容：类型 + 标志 + 载荷字节。
 *
 * @details `type`/`flags` 由上层解释，本模块只搬运。`payload` 是载荷**副本**，
 *          与编解码器内部缓冲不共享生命周期。
 */
struct Frame {
    /// 上层消息类型（本层不解释）。
    std::uint8_t type = 0;
    /// 上层标志位（本层不解释）。
    std::uint16_t flags = 0;
    /// 载荷字节（本层不解释语义）。
    std::vector<std::uint8_t> payload;
};

/**
 * @brief 帧格式描述：同一套编解码机制靠它区分不同协议。
 *
 * @details 五个字段完整决定线上字节：`magic`/`version` 做协议识别，
 *          `header_len` 决定帧头长度（含扩展区），`max_payload` 决定上限，
 *          `checksum` 决定是否在帧尾追加 4 字节校验和。
 *
 *          默认值刻意对齐 RDRL：`magic` 必须显式设置（默认全 0，任何真实帧都会
 *          在 magic 阶段被拒，属于「快速失败」而不是静默接受错误协议）。
 */
struct FrameSpec {
    /// 4 字节魔数；用 `MakeMagic("RDRL")` 从字面量构造。
    std::array<std::uint8_t, kMagicBytes> magic{{0, 0, 0, 0}};
    /// 版本号，必须逐字节相等，不做协商降级。
    std::uint8_t version = 1;
    /// 帧头字节数（含扩展区），必须落在 `[kMinHeaderLen, kMaxHeaderLen]`。
    int header_len = kDefaultHeaderLen;
    /// 单帧载荷上限（字节），必须 `>= 0`。
    int max_payload = kDefaultMaxPayload;
    /// 校验和函数；空表示不校验。默认 CRC-32/IEEE。
    std::function<std::uint32_t(const std::uint8_t*, std::size_t)> checksum =
        &Crc32IeeeFrameChecksum;

    /// @brief 是否启用校验和（即帧尾多 4 字节）。
    bool HasChecksum() const noexcept { return static_cast<bool>(checksum); }

    /// @brief 本 spec 是否可用（`Encode`/`Feed` 都会先检查）。
    bool Valid() const noexcept {
        return header_len >= kMinHeaderLen && header_len <= kMaxHeaderLen &&
               max_payload >= 0;
    }

    /// @brief 单帧最大字节数（帧头 + 载荷 + 校验和）；spec 非法时返回 0。
    std::size_t MaxFrameBytes() const noexcept {
        if (!Valid()) {
            return 0;
        }
        return static_cast<std::size_t>(header_len) +
               static_cast<std::size_t>(max_payload) +
               (HasChecksum() ? kChecksumBytes : 0);
    }
};

/**
 * @brief 由 4 字符字面量构造魔数。
 *
 * @param text 恰好 4 个字符的字符串字面量（含结尾 `\0`，所以参数是 `char[5]`）。
 *             长度不对会在编译期失败，避免把 `"RDR"` 这种半个魔数写进线上。
 * @return 4 字节魔数。
 */
constexpr std::array<std::uint8_t, kMagicBytes> MakeMagic(
    const char (&text)[kMagicBytes + 1]) noexcept {
    return {static_cast<std::uint8_t>(text[0]),
            static_cast<std::uint8_t>(text[1]),
            static_cast<std::uint8_t>(text[2]),
            static_cast<std::uint8_t>(text[3])};
}

/**
 * @brief 以魔数为主参数构造 `FrameSpec`（其余走默认：version=1、header_len=12、
 *        CRC-32/IEEE）。
 * @param magic 4 字节魔数（`MakeMagic("RDRL")`）。
 * @param version 版本号。
 * @param max_payload 载荷上限；默认 64 KiB。
 * @return 填好的 spec；需要关校验时把返回值的 `checksum` 置空即可。
 */
inline FrameSpec MakeFrameSpec(std::array<std::uint8_t, kMagicBytes> magic,
                               std::uint8_t version = 1,
                               int max_payload = kDefaultMaxPayload) noexcept {
    FrameSpec spec;
    spec.magic = magic;
    spec.version = version;
    spec.max_payload = max_payload;
    return spec;
}

/**
 * @brief 有状态的帧编解码器：编码一帧 / 从字节流里解出完整帧。
 *
 * @details 一个实例对应一条字节流（一条连接）。内部缓冲只保存**未成帧的尾部**，
 *          上限为一帧最大字节数；`Reset()` 释放它。
 *
 * ## 用法
 * @code
 *   go2cpp::frame::FrameCodec codec(
 *       go2cpp::frame::MakeFrameSpec(go2cpp::frame::MakeMagic("RDRL"), 1, 65536));
 *
 *   // 发送
 *   go2cpp::frame::Frame out_frame;
 *   out_frame.type = 1;                       // 语义由上层定
 *   out_frame.payload = {0x03, 'k', 'e', 'y'};
 *   std::vector<std::uint8_t> wire;
 *   go2cpp::frame::Error err;
 *   if (!codec.Encode(out_frame, &wire, &err)) {  // wire 可复用，失败时被清空
 *       LOG(ERROR) << err.message;
 *   }
 *
 *   // 接收（recv 到多少喂多少，半包/粘包都不用自己处理）
 *   std::vector<go2cpp::frame::Frame> frames;
 *   std::size_t consumed = 0;
 *   if (!codec.Feed(buffer, n, &frames, &consumed, &err)) {
 *       // 字节流已不可信：frames 里是出错前已解出的帧，之后必须断开连接
 *   }
 * @endcode
 *
 * ## 不变量
 * - `Feed()` 成功返回时 `*consumed == len`，内部只留未成帧尾部；
 * - 任何错误返回时内部缓冲被清空（不含 `kInvalidArgument`），调用方应断开连接；
 * - 出错前已解出的完整帧仍会留在 `*frames` 里（它们是可信的）。
 */
class FrameCodec {
   public:
    /**
     * @brief 用给定 spec 构造；spec 非法时所有操作返回 `kInvalidSpec`。
     * @param spec 帧格式（拷贝保存）。
     */
    explicit FrameCodec(FrameSpec spec);

    // 编码器持有半包状态，拷贝/移动都会让两条连接共享同一缓冲，直接禁止。
    FrameCodec(const FrameCodec&) = delete;
    FrameCodec& operator=(const FrameCodec&) = delete;
    FrameCodec(FrameCodec&&) = delete;
    FrameCodec& operator=(FrameCodec&&) = delete;

    /**
     * @brief 编码一帧（自动填 magic/version/length/校验和）。
     * @param frame 待编码帧；只读取 `type`/`flags`/`payload`。
     * @param out 输出缓冲；**会被清空后写入**，失败时保持为空（不会留下半帧）。
     * @param err 错误出参，可为 `nullptr`；进入函数时先清空。
     * @return 成功 `true`；失败 `false` 且 `err`（若非空）带错误码与文本。
     *
     * @note 载荷超过 `max_payload` 返回 `kPayloadTooLarge`，**不截断**。
     */
    bool Encode(const Frame& frame, std::vector<std::uint8_t>* out,
                Error* err) const;

    /**
     * @brief 喂入字节，解出所有已完整的帧。
     * @param data 输入字节；`len == 0` 时可为 `nullptr`。
     * @param len 输入字节数。
     * @param frames 输出：**追加**已解出的完整帧（调用方负责先清理要复用的容器）。
     * @param consumed 输出：本次从 `data` 接管的字节数；成功时恒等于 `len`，
     *                 失败时是出错前已处理的字节数（出错帧的帧头不计入）。
     * @param err 错误出参，可为 `nullptr`；进入函数时先清空。
     * @return 成功（含「只是半包，还没有完整帧」）返回 `true`；字节流畸形返回
     *         `false`，此时内部缓冲已清空，调用方必须断开连接。
     *
     * @note 半包是正常路径：返回 `true`、`*frames` 为空、`*consumed == len`，
     *       剩余字节留在内部缓冲等下次 `Feed()`。
     */
    bool Feed(const std::uint8_t* data, std::size_t len,
              std::vector<Frame>* frames, std::size_t* consumed, Error* err);

    /**
     * @brief 丢弃半包并释放内部缓冲（重连 / 换协议 / 错误后重新开始）。
     * @note 只影响重组状态，不影响 spec，也不影响调用方已拿到的 Frame。
     */
    void Reset() noexcept;

    /// @brief 当前 spec。
    const FrameSpec& Spec() const noexcept { return m_spec; }
    /// @brief spec 是否可用。
    bool SpecValid() const noexcept { return m_spec.Valid(); }
    /// @brief 内部缓冲里未成帧的字节数（诊断/测试用，正常情况下小于一帧）。
    std::size_t BufferedBytes() const noexcept { return m_buffer.size(); }

   private:
    /// 校验帧头（magic/version/length），成功时写整帧字节数。
    bool ValidateHeader(const std::uint8_t* header, std::size_t* frame_bytes,
                        Error* err) const;
    /// 校验校验和并从完整帧字节里取出 Frame 追加到 frames。
    bool ExtractFrame(const std::uint8_t* frame, std::size_t frame_bytes,
                      std::vector<Frame>* frames, Error* err) const;
    /// Feed 的实际实现（异常包装在外层）。
    bool FeedImpl(const std::uint8_t* data, std::size_t len,
                  std::vector<Frame>* frames, std::size_t* consumed, Error* err);
    /// 写错误（`err` 可为空）。
    static void SetError(Error* err, ErrorCode code, std::string message);

    FrameSpec m_spec;                 ///< 帧格式（构造后只读）
    std::vector<std::uint8_t> m_buffer;  ///< 未成帧的尾部字节
};

}  // namespace go2cpp::frame
