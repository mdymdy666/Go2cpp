#pragma once

/**
 * @file codec.hpp
 * @brief 媒体帧的线格式（MediaHeader）与编解码器抽象。
 * @details 解决的问题：Bento `VideoChannel` 只负责「把一段字节拆成 1200B 分片」，
 *          它不知道这段字节是 BGRA、I420 还是 H.264。本文件定义分片之前的
 *          **应用层帧封装**，让收端能无歧义地还原像素。
 *
 *          ## 线格式（全部大端）
 *
 *          | 偏移 | 字段        | 类型 | 语义                                   |
 *          |------|-------------|------|----------------------------------------|
 *          | 0    | magic       | u32  | 0x47324D46（"G2MF"）                    |
 *          | 4    | version     | u8   | 当前 1                                  |
 *          | 5    | codec       | u8   | CodecKind                               |
 *          | 6    | flags       | u8   | bit0 = 关键帧                           |
 *          | 7    | reserved    | u8   | 0                                       |
 *          | 8    | width       | u16  | 像素宽                                  |
 *          | 10   | height      | u16  | 像素高                                  |
 *          | 12   | reserved    | u16  | 0                                       |
 *          | 14   | reserved    | u16  | 0                                       |
 *          | 16   | frame_id    | u32  | 源端单调帧号                            |
 *          | 20   | payload_len | u32  | 紧随其后的负载字节数                     |
 *          | 24   | capture_ns  | u64  | 采集时刻（steady_clock 纳秒，端到端延迟） |
 *          | 32   | payload     | ...  | 编解码器负载                            |
 *
 *          ## CodecKind::kDeltaI420 负载格式
 *
 *          `kDeltaI420` 是本项目自研的「块级差分 + 行内 RLE」编码，用于在**不引入
 *          第三方编码器**的前提下把静止/局部变化的桌面画面压到极小（静止帧只剩
 *          bitmap），同时保证逐帧可无损还原（纯整数、无量化）。
 *
 *          ```
 *          delta_payload :=
 *              u32 bitmap_bytes        // 0 = 本帧无脏块（画面静止）
 *              u8  bitmap[bitmap_bytes]  // bit i = 第 i 个宏块是否变化（LSB first）
 *              for each dirty block:     // 按宏块行优先顺序
 *                  u8  mode              // 0 = raw(384B), 1 = rle
 *                  ...按宏块内三个平面块顺序（Y 16x16, U 8x8, V 8x8）
 *                  mode==0: 384 字节原始平面数据
 *                  mode==1: 每个平面块 u16 长度 + RLE 对（run u8, value u8）
 *          ```
 *
 *          宏块在平面边界处裁剪（奇数宽高与边缘宏块都安全）；`mode` 的选择由
 *          编码器按「RLE 后更短」决定，解码器按相同规则读取。
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "go2cpp/media/frame.hpp"

namespace go2cpp::media {

/// @brief 编码器种类。
enum class CodecKind : std::uint8_t {
    kRawBgra = 0,   ///< 不压缩，BGRA 原样传输（M0 兜底，保证无编码器也能闭环）。
    kRawI420 = 1,   ///< BGRA→I420 后原样传输（1/2 带宽，无压缩）。
    kDeltaI420 = 2, ///< I420 + 16x16 块级差分 + RLE（默认，无损）。
    kH264 = 3,      ///< ffmpeg 子进程（h264_nvenc 优先，回退 libx264）产出的 Annex-B。
};

/// @brief CodecKind 转可读名字。
const char* CodecKindName(CodecKind kind) noexcept;

/// @brief 从字符串解析 CodecKind（"delta"/"raw"/"raw-i420"/"h264"）。
bool ParseCodecKind(const char* text, CodecKind* out) noexcept;

/// @brief 媒体帧封装头。
struct MediaHeader {
    /// 固定字节数。
    static constexpr std::size_t kBytes = 32;
    /// magic 常量。
    static constexpr std::uint32_t kMagic = 0x47324D46U;  // "G2MF"

    std::uint8_t version = 1;
    CodecKind codec = CodecKind::kDeltaI420;
    bool keyframe = false;
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::uint32_t frame_id = 0;
    std::uint32_t payload_bytes = 0;
    std::uint64_t capture_ns = 0;
};

/// @brief 编码头到 32 字节大端缓冲区。
void EncodeMediaHeader(const MediaHeader& header, std::uint8_t* out) noexcept;

/// @brief 解码头；magic/version/长度任一不符返回 false。
bool DecodeMediaHeader(const std::uint8_t* data, std::size_t bytes, MediaHeader* out) noexcept;

/// @brief 视频编码器接口。
class VideoEncoder {
   public:
    virtual ~VideoEncoder() = default;

    VideoEncoder(const VideoEncoder&) = delete;
    VideoEncoder& operator=(const VideoEncoder&) = delete;
    VideoEncoder(VideoEncoder&&) = delete;
    VideoEncoder& operator=(VideoEncoder&&) = delete;

    /// @brief 编码一帧，输出「MediaHeader + 负载」的完整字节块。
    /// @param out 输出缓冲，内容被覆盖。
    /// @return 成功返回 true；帧非法或后端不可用返回 false。
    virtual bool Encode(const VideoFrame& frame, std::vector<std::uint8_t>* out) = 0;

    /// @brief 编码器种类。
    virtual CodecKind kind() const noexcept = 0;

    /// @brief 可读名字（用于日志/报告）。
    virtual const char* Name() const noexcept = 0;

    /// @brief 强制下一帧为关键帧。
    virtual void ForceKeyframe() noexcept = 0;

    /// @brief 已编码帧数。
    virtual std::uint64_t encoded_frames() const noexcept = 0;

    /// @brief 已产出的负载总字节数（含头）。
    virtual std::uint64_t encoded_bytes() const noexcept = 0;

    /// @brief 已产出的关键帧数。
    virtual std::uint64_t keyframes() const noexcept = 0;

   protected:
    VideoEncoder() = default;
};

/// @brief 视频解码器接口。
class VideoDecoder {
   public:
    virtual ~VideoDecoder() = default;

    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;
    VideoDecoder(VideoDecoder&&) = delete;
    VideoDecoder& operator=(VideoDecoder&&) = delete;

    /// @brief 解码一个「MediaHeader + 负载」字节块。
    /// @param out 解码结果（I420 或 BGRA，取决于 codec）；帧号/采集时间从头部复制。
    /// @return 成功返回 true；数据非法或依赖缺失（差分帧缺参考帧）返回 false。
    virtual bool Decode(const std::uint8_t* data, std::size_t bytes, VideoFrame* out) = 0;

    /// @brief 期望的 codec（用于校验收到的帧）。
    virtual CodecKind kind() const noexcept = 0;

    /// @brief 成功解码帧数。
    virtual std::uint64_t decoded_frames() const noexcept = 0;

    /// @brief 丢弃帧数（缺参考帧/头部非法/后端失败）。
    virtual std::uint64_t dropped_frames() const noexcept = 0;

   protected:
    VideoDecoder() = default;
};

/// @brief 创建编码器；像素尺寸在首帧时自适应（`width/height` 仅作提示）。
std::unique_ptr<VideoEncoder> CreateEncoder(CodecKind kind, int width, int height);

/// @brief 创建解码器。
std::unique_ptr<VideoDecoder> CreateDecoder(CodecKind kind, int width, int height);

/// @brief 探测可用编码后端（不改变任何状态）。
struct CodecAvailability {
    bool delta = true;      ///< 自研差分编码器恒可用。
    bool raw = true;        ///< 原始帧恒可用。
    bool h264 = false;      ///< ffmpeg 子进程可用。
    bool h264_nvenc = false;///< ffmpeg 报告存在 h264_nvenc 编码器且 nvidia-smi 有 GPU。
    bool x11_capture = false;///< DISPLAY 可用且能打开 X11。
    bool sdl_render = false; ///< SDL2 渲染可用。
};

/// @brief 探测编码/采集/渲染后端；结果缓存于进程内（只探测一次）。
CodecAvailability ProbeAvailability();

/// @brief 一个「MediaHeader + 负载」块的最大字节数（受 VideoChannel 分片上限约束）。
/// @details VideoChannel 单帧最多 256 片 × 1200B = 307200B；去掉 32B 头留余量。
inline constexpr std::size_t kMaxEncodedFrameBytes = 256 * 1200 - 64;

}  // namespace go2cpp::media
