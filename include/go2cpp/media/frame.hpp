#pragma once

/**
 * @file frame.hpp
 * @brief 媒体管线的帧容器：像素格式、内存布局与内容校验。
 * @details 本模块（`go2cpp::media`）是 WebRTC 媒体端到端闭环 M0/M1 的最小实现：
 *          采集（X11）→ 编码（I420 原始 / 块差分 / 可选 H.264）→ VideoChannel
 *          1200B 分片 → UDP → 重组 → 解码 → 渲染（SDL/GL）。
 *
 *          帧内存约定（两种格式都是单 buffer）：
 *          - `kBGRA`：每像素 4 字节，通道顺序 B,G,R,A，`stride = width * 4`；
 *          - `kI420`：先 Y 平面（`stride = width`），再 U、V 平面
 *            （`stride = (width + 1) / 2`，高 `(height + 1) / 2`）。
 *
 *          奇数宽高在 I420 下用 ceil 计算色度平面，保证 `height` 为奇数时
 *          不越界；编码器的宏块遍历按块裁剪到平面边界。
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include <cstddef>
#include <cstdint>
#include <vector>

namespace go2cpp::media {

/// @brief 支持的像素格式。
enum class PixelFormat : std::uint8_t {
    kBGRA = 0,  ///< 4 字节/像素，B,G,R,A（X11 depth-24 的 ZPixmap 小端布局）。
    kI420 = 1,  ///< YUV420 平面格式，见文件头的内存约定。
};

/// @brief 编码/差分使用的宏块边长（16 像素）。
inline constexpr int kMacroblockSize = 16;

/// @brief I420 单帧字节数（宽高都按 ceil 计算色度）。
inline std::size_t I420FrameBytes(int width, int height) noexcept {
    if (width <= 0 || height <= 0) {
        return 0;
    }
    const std::size_t luma = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    const std::size_t chroma_w = (static_cast<std::size_t>(width) + 1) / 2;
    const std::size_t chroma_h = (static_cast<std::size_t>(height) + 1) / 2;
    return luma + chroma_w * chroma_h * 2;
}

/// @brief BGRA 单帧字节数。
inline std::size_t BgraFrameBytes(int width, int height) noexcept {
    if (width <= 0 || height <= 0) {
        return 0;
    }
    return static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4;
}

/// @brief 一帧原始视频数据 + 采集元数据。
struct VideoFrame {
    int width = 0;                            ///< 宽（像素），> 0。
    int height = 0;                           ///< 高（像素），> 0。
    PixelFormat format = PixelFormat::kBGRA;  ///< 像素格式。
    int stride = 0;                           ///< 行字节数；0 表示按格式推导。
    std::vector<std::uint8_t> data;           ///< 像素数据，长度见 PlaneBytes()。
    std::uint32_t frame_id = 0;               ///< 单调递增帧号，源端赋值。
    std::int64_t capture_ns = 0;              ///< 采集时刻（steady_clock 纳秒）。
    bool keyframe = false;                    ///< 是否是关键帧（编码器置位）。

    /// @brief 推导行字节数：BGRA = width*4；I420 = width（Y 平面）。
    int ComputeStride() const noexcept {
        return format == PixelFormat::kBGRA ? width * 4 : width;
    }

    /// @brief 实际生效的行字节数（`stride` 为 0 时取推导值）。
    int EffectiveStride() const noexcept {
        return stride > 0 ? stride : ComputeStride();
    }

    /// @brief 本帧应有的字节数。
    std::size_t PlaneBytes() const noexcept {
        if (format == PixelFormat::kBGRA) {
            if (width <= 0 || height <= 0) {
                return 0;
            }
            return static_cast<std::size_t>(EffectiveStride()) * static_cast<std::size_t>(height);
        }
        return I420FrameBytes(width, height);
    }

    /// @brief 帧是否可参与编解码：尺寸合法、数据长度足够。
    bool Valid() const noexcept {
        if (width <= 0 || height <= 0) {
            return false;
        }
        if (format == PixelFormat::kBGRA && stride != 0 && stride < width * 4) {
            return false;
        }
        return data.size() >= PlaneBytes();
    }

    /// @brief 重新分配帧存储并清零。
    void Reset(int new_width, int new_height, PixelFormat new_format = PixelFormat::kBGRA) {
        width = new_width;
        height = new_height;
        format = new_format;
        stride = 0;
        keyframe = false;
        data.assign(PlaneBytes(), 0);
    }

    /// @brief 内容校验（FNV-1a 64），用于端到端逐帧比对。
    std::uint64_t Checksum() const noexcept;

    /// @brief 非零字节占比（0..1），用于判断画面是否真的变化。
    double NonZeroRatio() const noexcept;
};

}  // namespace go2cpp::media
