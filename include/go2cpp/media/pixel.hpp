#pragma once

/**
 * @file pixel.hpp
 * @brief 像素格式转换与质量度量（BGRA ↔ I420、PSNR、逐帧差异）。
 * @details 颜色转换使用整数近似的 BT.601 limited-range 系数（与 x264/NVENC 的
 *          `-color_range tv` 默认一致），避免浮点带来的跨平台差异：
 *
 *          Y  = ( 66*R + 129*G +  25*B + 128) >> 8 + 16
 *          U  = (-38*R -  74*G + 112*B + 128) >> 8 + 128
 *          V  = (112*R -  94*G -  18*B + 128) >> 8 + 128
 *
 *          逆变换用对应的定点系数。色度按 2x2 平均下采样（不引入 siting 偏移，
 *          对往返 PSNR 已足够，且不依赖 libswscale）。
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include <cstdint>

#include "go2cpp/media/frame.hpp"

namespace go2cpp::media {

/// @brief BGRA → I420。`bgra_stride` 是源行字节数；`i420` 至少 I420FrameBytes 字节。
void BgraToI420(const std::uint8_t* bgra, int width, int height, int bgra_stride,
                std::uint8_t* i420) noexcept;

/// @brief I420 → BGRA。`bgra` 至少 `bgra_stride * height` 字节，Alpha 置 255。
void I420ToBgra(const std::uint8_t* i420, int width, int height, std::uint8_t* bgra,
                int bgra_stride) noexcept;

/// @brief 帧级 PSNR（dB，只比较两者共有的最小尺寸）。
/// @return 完全相同时返回 99.0；尺寸为 0 时返回 0.0。
double FramePsnr(const VideoFrame& reference, const VideoFrame& test) noexcept;

/// @brief 逐字节平均绝对误差（0..255）。
double FrameMae(const VideoFrame& reference, const VideoFrame& test) noexcept;

/// @brief 两帧是否逐字节相同（尺寸、格式、有效数据都必须一致）。
bool FrameEqual(const VideoFrame& a, const VideoFrame& b) noexcept;

}  // namespace go2cpp::media
