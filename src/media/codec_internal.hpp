#pragma once

/**
 * @file codec_internal.hpp
 * @brief 媒体编解码器的内部工厂声明（不对外安装）。
 * @details 拆分到独立头的目的：让 `codec.cpp`（MediaHeader/raw/工厂/探针）与
 *          `codec_delta.cpp`（差分编解码）互不包含实现细节，同时共享创建入口。
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include <cstdint>
#include <memory>

#include "go2cpp/media/codec.hpp"

namespace go2cpp::media::detail {

/// @brief 创建 I420 原始帧编码器/解码器（无压缩）。
std::unique_ptr<VideoEncoder> CreateRawI420Encoder(int width, int height);
std::unique_ptr<VideoDecoder> CreateRawI420Decoder(int width, int height);

/// @brief 创建 BGRA 原始帧编码器/解码器（无压缩，M0 兜底）。
std::unique_ptr<VideoEncoder> CreateRawBgraEncoder(int width, int height);
std::unique_ptr<VideoDecoder> CreateRawBgraDecoder(int width, int height);

/// @brief 创建 16x16 块差分编码器/解码器。
std::unique_ptr<VideoEncoder> CreateDeltaEncoder(int width, int height);
std::unique_ptr<VideoDecoder> CreateDeltaDecoder(int width, int height);

/// @brief 创建 H.264（ffmpeg 子进程）编码器/解码器；不可用时返回 nullptr。
std::unique_ptr<VideoEncoder> CreateH264Encoder(int width, int height);
std::unique_ptr<VideoDecoder> CreateH264Decoder(int width, int height);

/// @brief ffmpeg 可执行文件是否可用（缓存探测结果）。
bool FfmpegAvailable();

/// @brief h264_nvenc 是否可用（缓存探测结果）。
bool NvencAvailable();

/// @brief 把任意输入帧（BGRA / 带 stride 的 I420）转成紧凑 I420。
/// @param out 输出缓冲（被 resize 到 I420FrameBytes）。
/// @param width 输出宽。
/// @param height 输出高。
/// @return 输入非法返回 false。
bool ToCompactI420(const VideoFrame& frame, std::vector<std::uint8_t>* out, int* width,
                   int* height);

}  // namespace go2cpp::media::detail
