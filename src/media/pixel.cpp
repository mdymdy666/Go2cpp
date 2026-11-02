/**
 * @file pixel.cpp
 * @brief 像素转换与质量度量实现（标量基线 + SSE2/SSSE3 内核）。
 * @details 定点 BT.601 limited-range；色度按 2x2 平均下采样。逆变换的系数
 *          与正向互为近似（误差 <= 1 LSB），因此「BGRA→I420→BGRA」的往返
 *          误差平均 8 左右，PSNR 约 30 dB —— 这是色度下采样的固有损失，
 *          不是实现缺陷；逐帧校验走 `FrameEqual`（同格式）而不是往返比较。
 *
 *          SIMD 化原则：**逐位等价**。整数定点运算在 SSE2/SSSE3 里用 16 位
 *          乘法 + 32 位累加精确复现标量表达式（同样的 +128 偏置、同样的算术
 *          右移、同样的饱和到 [0,255]），因此 `*Simd` 与 `*Scalar` 的输出
 *          逐字节相同（tests/media/test_codec.cpp 的 SIMD 一致性用例验证）。
 *
 *          运行时分派：进程内首次调用时探测一次 CPU（`__builtin_cpu_supports`），
 *          不支持 SSSE3 的机器自动走标量基线，不改变行为。
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include "go2cpp/media/pixel.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "pixel_kernels.hpp"

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#define GO2CPP_MEDIA_X86 1
#include <immintrin.h>
#else
#define GO2CPP_MEDIA_X86 0
#endif

namespace go2cpp::media {
namespace {

/// @brief 整数除法向下取整（用于负系数平移）。
inline int FloorDiv(int value, int divisor) noexcept {
    return value >= 0 ? value / divisor : -((-value + divisor - 1) / divisor);
}

/// @brief 限幅到 [0, 255]。
inline std::uint8_t Clamp8(int value) noexcept {
    if (value < 0) {
        return 0;
    }
    if (value > 255) {
        return 255;
    }
    return static_cast<std::uint8_t>(value);
}

/// @brief 色度平面（2x2 平均下采样 + BT.601 定点），标量与 SIMD 共用。
/// @details 色度只占整帧 1/4 样本，原实现每样本做一次 sum / samples
///          （samples 是运行时变量 → 真实 idiv，约 20~40 周期/样本，实测
///          占整帧转换时间的一半以上）。这里按「完整 2x2 / 右边界 / 下边界 /
///          角点」四种情况拆开，主循环全是 2 的幂平均（>> 2），无分支、
///          无除法，编译器可以把内层循环向量化。
void BgraToI420Chroma(const std::uint8_t* bgra, int width, int height, int bgra_stride,
                      std::uint8_t* plane_u, std::uint8_t* plane_v) noexcept {
    const int chroma_w = (width + 1) / 2;
    const int chroma_h = (height + 1) / 2;
    const int full_w = width / 2;    // 完整 2x2 块的列数
    const int full_h = height / 2;   // 完整 2x2 块的行数

    // 1) 完整 2x2 区域：samples == 4，平均即 >> 2。
    for (int cy = 0; cy < full_h; ++cy) {
        const std::uint8_t* r0 = bgra + static_cast<std::size_t>(2 * cy) *
                                            static_cast<std::size_t>(bgra_stride);
        const std::uint8_t* r1 = r0 + bgra_stride;
        std::uint8_t* out_u = plane_u + static_cast<std::size_t>(cy) * static_cast<std::size_t>(chroma_w);
        std::uint8_t* out_v = plane_v + static_cast<std::size_t>(cy) * static_cast<std::size_t>(chroma_w);
        for (int cx = 0; cx < full_w; ++cx) {
            const std::uint8_t* p = r0 + static_cast<std::size_t>(cx) * 8;
            const std::uint8_t* q = r1 + static_cast<std::size_t>(cx) * 8;
            const int b = (p[0] + p[4] + q[0] + q[4]) >> 2;
            const int g = (p[1] + p[5] + q[1] + q[5]) >> 2;
            const int r = (p[2] + p[6] + q[2] + q[6]) >> 2;
            out_u[cx] = Clamp8(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
            out_v[cx] = Clamp8(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
        }
        // 右边界：宽度为奇数时最后一列只有 2 个样本（上下各 1），平均即 >> 1。
        if (full_w < chroma_w) {
            const std::uint8_t* p = r0 + static_cast<std::size_t>(width - 1) * 4;
            const std::uint8_t* q = r1 + static_cast<std::size_t>(width - 1) * 4;
            const int b = (p[0] + q[0]) >> 1;
            const int g = (p[1] + q[1]) >> 1;
            const int r = (p[2] + q[2]) >> 1;
            out_u[full_w] = Clamp8(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
            out_v[full_w] = Clamp8(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
        }
    }

    // 2) 下边界：高度为奇数时最后一行只有 2 个样本（左右各 1），平均即 >> 1。
    if (full_h < chroma_h) {
        const int cy = full_h;
        const std::uint8_t* r0 = bgra + static_cast<std::size_t>(height - 1) *
                                            static_cast<std::size_t>(bgra_stride);
        std::uint8_t* out_u = plane_u + static_cast<std::size_t>(cy) * static_cast<std::size_t>(chroma_w);
        std::uint8_t* out_v = plane_v + static_cast<std::size_t>(cy) * static_cast<std::size_t>(chroma_w);
        for (int cx = 0; cx < full_w; ++cx) {
            const std::uint8_t* p = r0 + static_cast<std::size_t>(cx) * 8;
            const int b = (p[0] + p[4]) >> 1;
            const int g = (p[1] + p[5]) >> 1;
            const int r = (p[2] + p[6]) >> 1;
            out_u[cx] = Clamp8(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
            out_v[cx] = Clamp8(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
        }
        // 角点：宽度与高度都是奇数时只有 1 个样本。
        if (full_w < chroma_w) {
            const std::uint8_t* p = r0 + static_cast<std::size_t>(width - 1) * 4;
            const int b = p[0];
            const int g = p[1];
            const int r = p[2];
            out_u[full_w] = Clamp8(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
            out_v[full_w] = Clamp8(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
        }
    }
}

/// @brief 单个像素的 Y（标量路径，尾部与基线共用）。
inline std::uint8_t LumaOfPixel(const std::uint8_t* px) noexcept {
    return Clamp8(((66 * static_cast<int>(px[2]) + 129 * static_cast<int>(px[1]) +
                    25 * static_cast<int>(px[0]) + 128) >>
                   8) +
                  16);
}

}  // namespace

namespace detail {

PixelKernelLevel SelectedPixelKernel() noexcept {
    static const PixelKernelLevel kLevel = []() noexcept {
#if GO2CPP_MEDIA_X86
        __builtin_cpu_init();
        if (__builtin_cpu_supports("ssse3")) {
            return PixelKernelLevel::kSimd;
        }
#endif
        return PixelKernelLevel::kScalar;
    }();
    return kLevel;
}

const char* PixelKernelName(PixelKernelLevel level) noexcept {
    switch (level) {
        case PixelKernelLevel::kScalar:
            return "scalar";
        case PixelKernelLevel::kSimd:
            return "sse2";
    }
    return "unknown";
}

void BgraToI420Scalar(const std::uint8_t* bgra, int width, int height, int bgra_stride,
                      std::uint8_t* i420) noexcept {
    if (bgra == nullptr || i420 == nullptr || width <= 0 || height <= 0) {
        return;
    }
    if (bgra_stride <= 0) {
        bgra_stride = width * 4;
    }
    const int chroma_w = (width + 1) / 2;
    const int chroma_h = (height + 1) / 2;
    std::uint8_t* plane_y = i420;
    std::uint8_t* plane_u = i420 + static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    std::uint8_t* plane_v = plane_u + static_cast<std::size_t>(chroma_w) * static_cast<std::size_t>(chroma_h);

    for (int y = 0; y < height; ++y) {
        const std::uint8_t* row = bgra + static_cast<std::size_t>(y) * static_cast<std::size_t>(bgra_stride);
        std::uint8_t* out_y = plane_y + static_cast<std::size_t>(y) * static_cast<std::size_t>(width);
        for (int x = 0; x < width; ++x) {
            out_y[x] = LumaOfPixel(row + static_cast<std::size_t>(x) * 4);
        }
    }
    BgraToI420Chroma(bgra, width, height, bgra_stride, plane_u, plane_v);
}

#if GO2CPP_MEDIA_X86

/// @brief SSE2 Y 平面：每批 4 像素，`_mm_madd_epi16` 精确复现定点表达式。
__attribute__((target("sse2"))) void BgraToI420Y_SSE2(const std::uint8_t* bgra, int width, int height,
                                                       int bgra_stride,
                                                       std::uint8_t* plane_y) noexcept {
    // 标量公式是 66*R + 129*G + 25*B；lane 顺序是 B,G,R,A，因此系数按 (25,129,66,0) 排列。
    const __m128i coef = _mm_setr_epi16(25, 129, 66, 0, 25, 129, 66, 0);
    const __m128i zero = _mm_setzero_si128();
    const __m128i bias = _mm_set1_epi32(128);
    const __m128i add16 = _mm_set1_epi32(16);
    const int x_end4 = width & ~3;
    for (int y = 0; y < height; ++y) {
        const std::uint8_t* row = bgra + static_cast<std::size_t>(y) * static_cast<std::size_t>(bgra_stride);
        std::uint8_t* out_y = plane_y + static_cast<std::size_t>(y) * static_cast<std::size_t>(width);
        int x = 0;
        for (; x < x_end4; x += 4) {
            const __m128i px =
                _mm_loadu_si128(reinterpret_cast<const __m128i*>(row + static_cast<std::size_t>(x) * 4));
            const __m128i lo = _mm_unpacklo_epi8(px, zero);
            const __m128i hi = _mm_unpackhi_epi8(px, zero);
            const __m128i mlo = _mm_madd_epi16(lo, coef);
            const __m128i mhi = _mm_madd_epi16(hi, coef);
            // mlo = [25B0+66G0, 129R0, 25B1+66G1, 129R1]；mhi = 像素 2/3 的同构结果。
            const __m128i a = _mm_unpacklo_epi64(mlo, mhi);  // 像素 0、2 的部分和
            const __m128i b = _mm_unpackhi_epi64(mlo, mhi);  // 像素 1、3 的部分和
            const __m128i s = _mm_add_epi32(a, _mm_shuffle_epi32(a, _MM_SHUFFLE(2, 3, 0, 1)));
            const __m128i t = _mm_add_epi32(b, _mm_shuffle_epi32(b, _MM_SHUFFLE(2, 3, 0, 1)));
            // s = [Y0,Y0,Y2,Y2]、t = [Y1,Y1,Y3,Y3]；两次 unpack 拼成 [Y0,Y1,Y2,Y3]。
            const __m128i low = _mm_unpacklo_epi32(s, t);   // [Y0,Y1,Y0,Y1]
            const __m128i high = _mm_unpackhi_epi32(s, t);  // [Y2,Y3,Y2,Y3]
            __m128i yv = _mm_unpacklo_epi64(low, high);
            yv = _mm_add_epi32(yv, bias);
            yv = _mm_srai_epi32(yv, 8);
            yv = _mm_add_epi32(yv, add16);
            const __m128i p16 = _mm_packs_epi32(yv, yv);
            const __m128i p8 = _mm_packus_epi16(p16, p16);
            const std::uint32_t packed = static_cast<std::uint32_t>(_mm_cvtsi128_si32(p8));
            std::memcpy(out_y + x, &packed, 4);
        }
        for (; x < width; ++x) {
            out_y[x] = LumaOfPixel(row + static_cast<std::size_t>(x) * 4);
        }
    }
}

void BgraToI420Simd(const std::uint8_t* bgra, int width, int height, int bgra_stride,
                    std::uint8_t* i420) noexcept {
    if (bgra == nullptr || i420 == nullptr || width <= 0 || height <= 0) {
        return;
    }
    if (bgra_stride <= 0) {
        bgra_stride = width * 4;
    }
    const int chroma_w = (width + 1) / 2;
    const int chroma_h = (height + 1) / 2;
    std::uint8_t* plane_y = i420;
    std::uint8_t* plane_u = i420 + static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    std::uint8_t* plane_v = plane_u + static_cast<std::size_t>(chroma_w) * static_cast<std::size_t>(chroma_h);
    BgraToI420Y_SSE2(bgra, width, height, bgra_stride, plane_y);
    BgraToI420Chroma(bgra, width, height, bgra_stride, plane_u, plane_v);
}

/// @brief SSSE3 逆向：每批 4 像素，`_mm_madd_epi16` + `_mm_shuffle_epi8` 交错成 BGRA。
__attribute__((target("ssse3"))) void I420ToBgraRow_SSSE3(const std::uint8_t* row_y,
                                                          const std::uint8_t* row_u,
                                                          const std::uint8_t* row_v, int width,
                                                          std::uint8_t* out) noexcept {
    const __m128i zero = _mm_setzero_si128();
    const __m128i sub16 = _mm_set1_epi16(16);
    const __m128i sub128 = _mm_set1_epi16(128);
    const __m128i kBCoef = _mm_set1_epi32((516 << 16) | 298);
    const __m128i kGcdCoef = _mm_set1_epi32(static_cast<int>((0xFF9Cu << 16) | 298u));  // [-100, 298]
    const __m128i kGceCoef = _mm_set1_epi32(static_cast<int>(0xFF30u << 16));           // [-208, 0]
    const __m128i kRCoef = _mm_set1_epi32((409 << 16) | 298);
    const __m128i bias = _mm_set1_epi32(128);
    const __m128i alpha = _mm_set1_epi32(255);
    const __m128i shuffle = _mm_setr_epi8(0, 4, 8, 12, 1, 5, 9, 13, 2, 6, 10, 14, 3, 7, 11, 15);
    const int x_end4 = width & ~3;
    int x = 0;
    for (; x < x_end4; x += 4) {
        std::uint32_t y4 = 0;
        std::memcpy(&y4, row_y + x, 4);
        __m128i c16 = _mm_unpacklo_epi8(_mm_cvtsi32_si128(static_cast<int>(y4)), zero);
        c16 = _mm_sub_epi16(c16, sub16);

        std::uint16_t u2 = 0;
        std::uint16_t v2 = 0;
        std::memcpy(&u2, row_u + x / 2, 2);
        std::memcpy(&v2, row_v + x / 2, 2);
        // [U0,U1] → [U0,U0,U1,U1]：两个像素共享一个色度样本。
        __m128i d4 = _mm_shufflelo_epi16(
            _mm_unpacklo_epi8(_mm_cvtsi32_si128(static_cast<int>(u2)), zero),
            _MM_SHUFFLE(1, 1, 0, 0));
        __m128i e4 = _mm_shufflelo_epi16(
            _mm_unpacklo_epi8(_mm_cvtsi32_si128(static_cast<int>(v2)), zero),
            _MM_SHUFFLE(1, 1, 0, 0));
        d4 = _mm_sub_epi16(d4, sub128);
        e4 = _mm_sub_epi16(e4, sub128);

        const __m128i cd = _mm_unpacklo_epi16(c16, d4);  // [c0,d0,c1,d0,c2,d1,c3,d1]
        const __m128i ce = _mm_unpacklo_epi16(c16, e4);  // [c0,e0,c1,e0,c2,e1,c3,e1]

        __m128i bv = _mm_madd_epi16(cd, kBCoef);
        __m128i gv = _mm_add_epi32(_mm_madd_epi16(cd, kGcdCoef), _mm_madd_epi16(ce, kGceCoef));
        __m128i rv = _mm_madd_epi16(ce, kRCoef);
        bv = _mm_srai_epi32(_mm_add_epi32(bv, bias), 8);
        gv = _mm_srai_epi32(_mm_add_epi32(gv, bias), 8);
        rv = _mm_srai_epi32(_mm_add_epi32(rv, bias), 8);

        const __m128i bg = _mm_packs_epi32(bv, gv);      // [B0..B3, G0..G3]
        const __m128i ra = _mm_packs_epi32(rv, alpha);   // [R0..R3, 255 x4]
        const __m128i packed = _mm_packus_epi16(bg, ra); // [B,G,R,A] 分段
        const __m128i bgra = _mm_shuffle_epi8(packed, shuffle);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(out + static_cast<std::size_t>(x) * 4), bgra);
    }
    for (; x < width; ++x) {
        const int c = static_cast<int>(row_y[x]) - 16;
        const int d = static_cast<int>(row_u[x / 2]) - 128;
        const int e = static_cast<int>(row_v[x / 2]) - 128;
        const int c1 = 298 * c;
        out[static_cast<std::size_t>(x) * 4 + 0] = Clamp8(FloorDiv(c1 + 516 * d + 128, 256));
        out[static_cast<std::size_t>(x) * 4 + 1] = Clamp8(FloorDiv(c1 - 100 * d - 208 * e + 128, 256));
        out[static_cast<std::size_t>(x) * 4 + 2] = Clamp8(FloorDiv(c1 + 409 * e + 128, 256));
        out[static_cast<std::size_t>(x) * 4 + 3] = 255;
    }
}

void I420ToBgraSimd(const std::uint8_t* i420, int width, int height, std::uint8_t* bgra,
                    int bgra_stride) noexcept {
    if (i420 == nullptr || bgra == nullptr || width <= 0 || height <= 0) {
        return;
    }
    if (bgra_stride <= 0) {
        bgra_stride = width * 4;
    }
    const int chroma_w = (width + 1) / 2;
    const int chroma_h = (height + 1) / 2;
    const std::uint8_t* plane_y = i420;
    const std::uint8_t* plane_u = i420 + static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    const std::uint8_t* plane_v = plane_u + static_cast<std::size_t>(chroma_w) * static_cast<std::size_t>(chroma_h);
    for (int y = 0; y < height; ++y) {
        const std::uint8_t* row_y = plane_y + static_cast<std::size_t>(y) * static_cast<std::size_t>(width);
        const std::uint8_t* row_u =
            plane_u + static_cast<std::size_t>(y / 2) * static_cast<std::size_t>(chroma_w);
        const std::uint8_t* row_v =
            plane_v + static_cast<std::size_t>(y / 2) * static_cast<std::size_t>(chroma_w);
        std::uint8_t* out = bgra + static_cast<std::size_t>(y) * static_cast<std::size_t>(bgra_stride);
        I420ToBgraRow_SSSE3(row_y, row_u, row_v, width, out);
    }
}

#else  // !GO2CPP_MEDIA_X86

void BgraToI420Simd(const std::uint8_t* bgra, int width, int height, int bgra_stride,
                    std::uint8_t* i420) noexcept {
    BgraToI420Scalar(bgra, width, height, bgra_stride, i420);
}

void I420ToBgraSimd(const std::uint8_t* i420, int width, int height, std::uint8_t* bgra,
                    int bgra_stride) noexcept {
    I420ToBgraScalar(i420, width, height, bgra, bgra_stride);
}

#endif  // GO2CPP_MEDIA_X86

void I420ToBgraScalar(const std::uint8_t* i420, int width, int height, std::uint8_t* bgra,
                      int bgra_stride) noexcept {
    if (i420 == nullptr || bgra == nullptr || width <= 0 || height <= 0) {
        return;
    }
    if (bgra_stride <= 0) {
        bgra_stride = width * 4;
    }
    const int chroma_w = (width + 1) / 2;
    const int chroma_h = (height + 1) / 2;
    const std::uint8_t* plane_y = i420;
    const std::uint8_t* plane_u = i420 + static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    const std::uint8_t* plane_v = plane_u + static_cast<std::size_t>(chroma_w) * static_cast<std::size_t>(chroma_h);

    for (int y = 0; y < height; ++y) {
        const std::uint8_t* row_y = plane_y + static_cast<std::size_t>(y) * static_cast<std::size_t>(width);
        std::uint8_t* out = bgra + static_cast<std::size_t>(y) * static_cast<std::size_t>(bgra_stride);
        const std::uint8_t* row_u =
            plane_u + static_cast<std::size_t>(y / 2) * static_cast<std::size_t>(chroma_w);
        const std::uint8_t* row_v =
            plane_v + static_cast<std::size_t>(y / 2) * static_cast<std::size_t>(chroma_w);
        for (int x = 0; x < width; ++x) {
            const int c = static_cast<int>(row_y[x]) - 16;
            const int d = static_cast<int>(row_u[x / 2]) - 128;
            const int e = static_cast<int>(row_v[x / 2]) - 128;
            const int c1 = 298 * c;
            const int b = FloorDiv(c1 + 516 * d + 128, 256);
            const int g = FloorDiv(c1 - 100 * d - 208 * e + 128, 256);
            const int r = FloorDiv(c1 + 409 * e + 128, 256);
            out[static_cast<std::size_t>(x) * 4 + 0] = Clamp8(b);
            out[static_cast<std::size_t>(x) * 4 + 1] = Clamp8(g);
            out[static_cast<std::size_t>(x) * 4 + 2] = Clamp8(r);
            out[static_cast<std::size_t>(x) * 4 + 3] = 255;
        }
    }
}

}  // namespace detail

void BgraToI420(const std::uint8_t* bgra, int width, int height, int bgra_stride,
                std::uint8_t* i420) noexcept {
    if (detail::SelectedPixelKernel() == detail::PixelKernelLevel::kSimd) {
        detail::BgraToI420Simd(bgra, width, height, bgra_stride, i420);
        return;
    }
    detail::BgraToI420Scalar(bgra, width, height, bgra_stride, i420);
}

void I420ToBgra(const std::uint8_t* i420, int width, int height, std::uint8_t* bgra,
                int bgra_stride) noexcept {
    if (detail::SelectedPixelKernel() == detail::PixelKernelLevel::kSimd) {
        detail::I420ToBgraSimd(i420, width, height, bgra, bgra_stride);
        return;
    }
    detail::I420ToBgraScalar(i420, width, height, bgra, bgra_stride);
}

double FramePsnr(const VideoFrame& reference, const VideoFrame& test) noexcept {
    if (reference.format != test.format) {
        return 0.0;
    }
    const int width = std::min(reference.width, test.width);
    const int height = std::min(reference.height, test.height);
    if (width <= 0 || height <= 0) {
        return 0.0;
    }
    const std::size_t ref_stride = static_cast<std::size_t>(reference.EffectiveStride());
    const std::size_t test_stride = static_cast<std::size_t>(test.EffectiveStride());
    const std::size_t row_bytes =
        reference.format == PixelFormat::kBGRA ? static_cast<std::size_t>(width) * 4
                                               : static_cast<std::size_t>(width);
    const std::size_t need = row_bytes * static_cast<std::size_t>(height);
    if ((reference.OwnsStorage() && reference.data.size() < need) ||
        (test.OwnsStorage() && test.data.size() < need)) {
        return 0.0;
    }
    const std::uint8_t* ref_bytes = reference.Bytes();
    const std::uint8_t* test_bytes = test.Bytes();
    if (ref_bytes == nullptr || test_bytes == nullptr) {
        return 0.0;
    }
    double squared = 0.0;
    std::size_t samples = 0;
    for (int y = 0; y < height; ++y) {
        const std::uint8_t* a = ref_bytes + static_cast<std::size_t>(y) * ref_stride;
        const std::uint8_t* b = test_bytes + static_cast<std::size_t>(y) * test_stride;
        for (std::size_t i = 0; i < row_bytes; ++i) {
            const double diff = static_cast<double>(a[i]) - static_cast<double>(b[i]);
            squared += diff * diff;
            ++samples;
        }
    }
    if (samples == 0) {
        return 0.0;
    }
    const double mse = squared / static_cast<double>(samples);
    if (mse <= 0.0) {
        return 99.0;
    }
    return 10.0 * std::log10((255.0 * 255.0) / mse);
}

double FrameMae(const VideoFrame& reference, const VideoFrame& test) noexcept {
    if (reference.format != test.format) {
        return 255.0;
    }
    const int width = std::min(reference.width, test.width);
    const int height = std::min(reference.height, test.height);
    if (width <= 0 || height <= 0) {
        return 255.0;
    }
    const std::size_t ref_stride = static_cast<std::size_t>(reference.EffectiveStride());
    const std::size_t test_stride = static_cast<std::size_t>(test.EffectiveStride());
    const std::size_t row_bytes =
        reference.format == PixelFormat::kBGRA ? static_cast<std::size_t>(width) * 4
                                               : static_cast<std::size_t>(width);
    const std::size_t need = row_bytes * static_cast<std::size_t>(height);
    if ((reference.OwnsStorage() && reference.data.size() < need) ||
        (test.OwnsStorage() && test.data.size() < need)) {
        return 255.0;
    }
    const std::uint8_t* ref_bytes = reference.Bytes();
    const std::uint8_t* test_bytes = test.Bytes();
    if (ref_bytes == nullptr || test_bytes == nullptr) {
        return 255.0;
    }
    double total = 0.0;
    std::size_t samples = 0;
    for (int y = 0; y < height; ++y) {
        const std::uint8_t* a = ref_bytes + static_cast<std::size_t>(y) * ref_stride;
        const std::uint8_t* b = test_bytes + static_cast<std::size_t>(y) * test_stride;
        for (std::size_t i = 0; i < row_bytes; ++i) {
            const int diff = static_cast<int>(a[i]) - static_cast<int>(b[i]);
            total += diff < 0 ? -diff : diff;
            ++samples;
        }
    }
    if (samples == 0) {
        return 255.0;
    }
    return total / static_cast<double>(samples);
}

bool FrameEqual(const VideoFrame& a, const VideoFrame& b) noexcept {
    if (a.width != b.width || a.height != b.height || a.format != b.format) {
        return false;
    }
    if (!a.Valid() || !b.Valid()) {
        return false;
    }
    const std::uint8_t* a_bytes = a.Bytes();
    const std::uint8_t* b_bytes = b.Bytes();
    if (a_bytes == nullptr || b_bytes == nullptr) {
        return false;
    }
    // 按行比较有效像素，跳过行尾 padding（紧凑帧等价于整体 memcmp）。
    const std::size_t row_bytes = a.format == PixelFormat::kBGRA
                                      ? static_cast<std::size_t>(a.width) * 4
                                      : static_cast<std::size_t>(a.width);
    const std::size_t a_stride = static_cast<std::size_t>(a.EffectiveStride());
    const std::size_t b_stride = static_cast<std::size_t>(b.EffectiveStride());
    for (int y = 0; y < a.height; ++y) {
        if (std::memcmp(a_bytes + static_cast<std::size_t>(y) * a_stride,
                        b_bytes + static_cast<std::size_t>(y) * b_stride, row_bytes) != 0) {
            return false;
        }
    }
    return true;
}

}  // namespace go2cpp::media
