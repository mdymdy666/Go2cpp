/**
 * @file pixel.cpp
 * @brief 像素转换与质量度量实现。
 * @details 定点 BT.601 limited-range；色度按 2x2 平均下采样。逆变换的系数
 *          与正向互为近似（误差 <= 1 LSB），因此「BGRA→I420→BGRA」的往返
 *          误差平均 8 左右，PSNR 约 30 dB —— 这是色度下采样的固有损失，
 *          不是实现缺陷；逐帧校验走 `FrameEqual`（同格式）而不是往返比较。
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include "go2cpp/media/pixel.hpp"

#include <algorithm>
#include <cmath>

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

}  // namespace

void BgraToI420(const std::uint8_t* bgra, int width, int height, int bgra_stride,
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
            const int b = row[static_cast<std::size_t>(x) * 4 + 0];
            const int g = row[static_cast<std::size_t>(x) * 4 + 1];
            const int r = row[static_cast<std::size_t>(x) * 4 + 2];
            out_y[x] = Clamp8(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
        }
    }

    for (int cy = 0; cy < chroma_h; ++cy) {
        std::uint8_t* out_u = plane_u + static_cast<std::size_t>(cy) * static_cast<std::size_t>(chroma_w);
        std::uint8_t* out_v = plane_v + static_cast<std::size_t>(cy) * static_cast<std::size_t>(chroma_w);
        for (int cx = 0; cx < chroma_w; ++cx) {
            int sum_b = 0;
            int sum_g = 0;
            int sum_r = 0;
            int samples = 0;
            for (int dy = 0; dy < 2; ++dy) {
                const int sy = cy * 2 + dy;
                if (sy >= height) {
                    break;
                }
                const std::uint8_t* row =
                    bgra + static_cast<std::size_t>(sy) * static_cast<std::size_t>(bgra_stride);
                for (int dx = 0; dx < 2; ++dx) {
                    const int sx = cx * 2 + dx;
                    if (sx >= width) {
                        break;
                    }
                    sum_b += row[static_cast<std::size_t>(sx) * 4 + 0];
                    sum_g += row[static_cast<std::size_t>(sx) * 4 + 1];
                    sum_r += row[static_cast<std::size_t>(sx) * 4 + 2];
                    ++samples;
                }
            }
            if (samples == 0) {
                out_u[cx] = 128;
                out_v[cx] = 128;
                continue;
            }
            const int b = sum_b / samples;
            const int g = sum_g / samples;
            const int r = sum_r / samples;
            out_u[cx] = Clamp8(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
            out_v[cx] = Clamp8(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
        }
    }
}

void I420ToBgra(const std::uint8_t* i420, int width, int height, std::uint8_t* bgra,
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
    if (reference.data.size() < ref_stride * static_cast<std::size_t>(height) ||
        test.data.size() < test_stride * static_cast<std::size_t>(height)) {
        return 0.0;
    }
    double squared = 0.0;
    std::size_t samples = 0;
    for (int y = 0; y < height; ++y) {
        const std::uint8_t* a = reference.data.data() + static_cast<std::size_t>(y) * ref_stride;
        const std::uint8_t* b = test.data.data() + static_cast<std::size_t>(y) * test_stride;
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
    if (reference.data.size() < ref_stride * static_cast<std::size_t>(height) ||
        test.data.size() < test_stride * static_cast<std::size_t>(height)) {
        return 255.0;
    }
    double total = 0.0;
    std::size_t samples = 0;
    for (int y = 0; y < height; ++y) {
        const std::uint8_t* a = reference.data.data() + static_cast<std::size_t>(y) * ref_stride;
        const std::uint8_t* b = test.data.data() + static_cast<std::size_t>(y) * test_stride;
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
    const std::size_t bytes = a.PlaneBytes();
    return std::equal(a.data.begin(), a.data.begin() + static_cast<std::ptrdiff_t>(bytes),
                      b.data.begin());
}

}  // namespace go2cpp::media
