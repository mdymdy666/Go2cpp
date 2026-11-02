/**
 * @file frame.cpp
 * @brief 帧容器实现：FNV-1a 校验与非零占比。
 * @details 像素统一从 `Bytes()` 读取，因此自有缓冲与零拷贝视图（XShm 借用）
 *          走同一套校验逻辑；借用视图的长度按 `PlaneBytes()` 计。
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include "go2cpp/media/frame.hpp"

namespace go2cpp::media {

std::uint64_t VideoFrame::Checksum() const noexcept {
    // FNV-1a 64：对有效像素字节（不含 stride 补齐区）求校验，端到端逐帧比对用。
    std::uint64_t hash = 1469598103934665603ULL;
    const std::uint8_t* bytes_ptr = Bytes();
    const std::size_t bytes = PlaneBytes();
    if (bytes_ptr == nullptr || bytes == 0) {
        return 0;
    }
    if (OwnsStorage() && data.size() < bytes) {
        return 0;
    }
    if (format == PixelFormat::kBGRA && EffectiveStride() == width * 4) {
        for (std::size_t i = 0; i < bytes; ++i) {
            hash ^= bytes_ptr[i];
            hash *= 1099511628211ULL;
        }
        return hash;
    }
    // 有行补齐时只对每行有效像素求校验，避免 padding 噪声影响比对。
    const std::size_t row_bytes = format == PixelFormat::kBGRA
                                      ? static_cast<std::size_t>(width) * 4
                                      : static_cast<std::size_t>(width);
    const std::size_t stride_bytes = static_cast<std::size_t>(EffectiveStride());
    for (int y = 0; y < height; ++y) {
        const std::uint8_t* row = bytes_ptr + static_cast<std::size_t>(y) * stride_bytes;
        for (std::size_t i = 0; i < row_bytes; ++i) {
            hash ^= row[i];
            hash *= 1099511628211ULL;
        }
    }
    return hash;
}

double VideoFrame::NonZeroRatio() const noexcept {
    const std::uint8_t* bytes_ptr = Bytes();
    const std::size_t bytes = PlaneBytes();
    if (bytes_ptr == nullptr || bytes == 0) {
        return 0.0;
    }
    if (OwnsStorage() && data.size() < bytes) {
        return 0.0;
    }
    std::size_t non_zero = 0;
    for (std::size_t i = 0; i < bytes; ++i) {
        if (bytes_ptr[i] != 0) {
            ++non_zero;
        }
    }
    return static_cast<double>(non_zero) / static_cast<double>(bytes);
}

}  // namespace go2cpp::media
