/**
 * @file pixel_kernels.hpp
 * @brief 像素转换内核的内部声明（标量基线 + SIMD 变体）。
 * @details 供 `pixel.cpp` 与测试使用：测试可以直接调用 `*Scalar` 与 `*Simd`
 *          两个实现，逐字节比对，证明 SIMD 化没有引入任何像素差异。
 *          本文件不是公共 API（位于 src/media/），测试通过相对路径包含。
 * @author go2cpp contributors
 * @date 2026-11-02
 * @copyright MIT License
 */

#pragma once

#include <cstdint>

namespace go2cpp::media::detail {

/// @brief 转换内核层级。
enum class PixelKernelLevel : int {
    kScalar = 0,  ///< 纯标量基线（可移植，任何平台可用）。
    kSimd = 1,    ///< SSE2/SSSE3 内核（x86-64；其余平台自动回退标量）。
};

/// @brief 当前进程实际选中的内核（启动时探测一次，之后只读）。
PixelKernelLevel SelectedPixelKernel() noexcept;

/// @brief 把内核层级转成可读名字（"scalar" / "sse2"）。
const char* PixelKernelName(PixelKernelLevel level) noexcept;

/// @brief BGRA → I420 的标量基线实现（与 SIMD 逐字节一致）。
void BgraToI420Scalar(const std::uint8_t* bgra, int width, int height, int bgra_stride,
                      std::uint8_t* i420) noexcept;

/// @brief BGRA → I420 的 SIMD 实现（SSE2 Y 平面 + SSSE3 色度）。
void BgraToI420Simd(const std::uint8_t* bgra, int width, int height, int bgra_stride,
                    std::uint8_t* i420) noexcept;

/// @brief I420 → BGRA 的标量基线实现。
void I420ToBgraScalar(const std::uint8_t* i420, int width, int height, std::uint8_t* bgra,
                      int bgra_stride) noexcept;

/// @brief I420 → BGRA 的 SIMD 实现（SSSE3 每 4 像素）。
void I420ToBgraSimd(const std::uint8_t* i420, int width, int height, std::uint8_t* bgra,
                    int bgra_stride) noexcept;

}  // namespace go2cpp::media::detail
