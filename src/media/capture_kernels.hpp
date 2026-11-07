/**
 * @file capture_kernels.hpp
 * @brief X11 采集的像素搬运内核（标量基线 + SSE2 变体）。
 * @details 采集热路径只有两种工作：
 *
 *          1. **owned 路径**：把 XImage 的一行拷进 `VideoFrame::data`，同时把
 *             X11 depth-24 的填充字节（BGRX 的 X）改写成 0xFF，得到严格 BGRA；
 *          2. **零拷贝路径**：XShm 映射的像素直接借给 `VideoFrame`，只把 alpha
 *             字节原地填 0xFF（不搬整行，只写每 4 字节的 1 个字节）。
 *
 *          两个变体都提供 `Scalar`（可移植基线）与 `Simd`（SSE2，x86-64 基线
 *          ISA），测试逐字节比对两者，保证 SIMD 化没有引入像素差异。与
 *          `pixel_kernels.hpp` 的分层方式一致：声明在 `src/media/`，不进入公共
 *          头文件；实现为 inline，避免为一个 helper 增加 TU 与构建入口。
 * @author go2cpp contributors
 * @date 2026-11-02
 * @copyright MIT License
 */

#pragma once

#include <cstdint>
#include <cstring>

#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <emmintrin.h>
#define GO2CPP_MEDIA_CAPTURE_SSE2 1
#else
#define GO2CPP_MEDIA_CAPTURE_SSE2 0
#endif

namespace go2cpp::media::detail {

/// @brief 每像素 4 字节的 BGRA 掩码常量（小端：[B,G,R,A]）。
inline constexpr std::uint32_t kCaptureBgrMask = 0x00FFFFFFU;
/// @brief alpha 通道全 1 掩码（小端第 4 字节）。
inline constexpr std::uint32_t kCaptureAlphaMask = 0xFF000000U;

/// @brief 标量基线：拷一行 BGRX → BGRA（X 字节写 0xFF）。
inline void CopyBgraRowScalar(std::uint8_t* dst, const std::uint8_t* src, int width) noexcept {
    for (int x = 0; x < width; ++x) {
        const std::size_t offset = static_cast<std::size_t>(x) * 4U;
        dst[offset + 0] = src[offset + 0];
        dst[offset + 1] = src[offset + 1];
        dst[offset + 2] = src[offset + 2];
        dst[offset + 3] = 255;
    }
}

/// @brief SSE2：每轮 4 像素（16 字节）一条 `pand` + `por`，尾像素走标量。
inline void CopyBgraRowSimd(std::uint8_t* dst, const std::uint8_t* src, int width) noexcept {
#if GO2CPP_MEDIA_CAPTURE_SSE2
    const __m128i bgr = _mm_set1_epi32(static_cast<int>(kCaptureBgrMask));
    const __m128i alpha = _mm_set1_epi32(static_cast<int>(kCaptureAlphaMask));
    int x = 0;
    for (; x + 4 <= width; x += 4) {
        const std::size_t offset = static_cast<std::size_t>(x) * 4U;
        __m128i pixels = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + offset));
        pixels = _mm_or_si128(_mm_and_si128(pixels, bgr), alpha);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + offset), pixels);
    }
    for (; x < width; ++x) {
        const std::size_t offset = static_cast<std::size_t>(x) * 4U;
        dst[offset + 0] = src[offset + 0];
        dst[offset + 1] = src[offset + 1];
        dst[offset + 2] = src[offset + 2];
        dst[offset + 3] = 255;
    }
#else
    CopyBgraRowScalar(dst, src, width);
#endif
}

/// @brief 标量基线：原地把一行每像素的第 4 字节写成 0xFF（RGB 不动）。
inline void FillAlphaRowScalar(std::uint8_t* row, int width) noexcept {
    for (int x = 0; x < width; ++x) {
        row[static_cast<std::size_t>(x) * 4U + 3U] = 255;
    }
}

/// @brief SSE2：原地 16 字节/轮 `por` 填 alpha，尾像素走标量。
/// @details 原地填充仍要读一次缓存行（alpha 与 RGB 共享），但只写回同一块
///          内存，不产生第二块目标缓冲；相比 owned 路径少一整块写带宽。
inline void FillAlphaRowSimd(std::uint8_t* row, int width) noexcept {
#if GO2CPP_MEDIA_CAPTURE_SSE2
    const __m128i alpha = _mm_set1_epi32(static_cast<int>(kCaptureAlphaMask));
    int x = 0;
    for (; x + 4 <= width; x += 4) {
        const std::size_t offset = static_cast<std::size_t>(x) * 4U;
        __m128i pixels = _mm_loadu_si128(reinterpret_cast<const __m128i*>(row + offset));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(row + offset), _mm_or_si128(pixels, alpha));
    }
    for (; x < width; ++x) {
        row[static_cast<std::size_t>(x) * 4U + 3U] = 255;
    }
#else
    FillAlphaRowScalar(row, width);
#endif
}

/// @brief 当前构建是否启用了 SSE2 采集内核。
inline constexpr bool CaptureHasSimd() noexcept {
    return GO2CPP_MEDIA_CAPTURE_SSE2 != 0;
}

}  // namespace go2cpp::media::detail
