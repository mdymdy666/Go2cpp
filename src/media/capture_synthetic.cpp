/**
 * @file capture_synthetic.cpp
 * @brief 合成采集源：确定性画面图案，用于无头 CI 与差分编码的可重复输入。
 * @details 四种图案对应四类真实场景：
 *          - `kStatic`：静止桌面（差分编码应收敛到 4 字节/帧）；
 *          - `kScroll`：滚动条/时钟刷新（少量脏块）；
 *          - `kPatch`：窗口拖动（一个矩形块移动，脏块集中在两条边）；
 *          - `kNoise`：全画面噪声（不可压缩，最坏情况，用来量上限）。
 *
 *          同一 `frame_id` 永远产生同一画面（`kNoise` 用帧号播种），因此
 *          「丢帧后重发」也能得到逐字节相同的参考帧，便于测试断言。
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include <cstring>

#include "go2cpp/media/capture.hpp"

namespace go2cpp::media {
namespace {

/// @brief 写一个像素（BGRA）。
inline void SetPixel(std::uint8_t* row, int x, std::uint8_t b, std::uint8_t g, std::uint8_t r) {
    row[x * 4 + 0] = b;
    row[x * 4 + 1] = g;
    row[x * 4 + 2] = r;
    row[x * 4 + 3] = 255;
}

/// @brief 背景底座：B 随 x 渐变、G 随 y 渐变、R 随 (x^y) 抖动的棋盘格。
void PaintBase(std::uint8_t* data, int width, int height, int stride) {
    for (int y = 0; y < height; ++y) {
        std::uint8_t* row = data + static_cast<std::size_t>(y) * static_cast<std::size_t>(stride);
        for (int x = 0; x < width; ++x) {
            const std::uint8_t b = static_cast<std::uint8_t>(x * 255 / (width > 1 ? width - 1 : 1));
            const std::uint8_t g = static_cast<std::uint8_t>(y * 255 / (height > 1 ? height - 1 : 1));
            const std::uint8_t r = ((x ^ y) & 8) != 0 ? 0xE0 : 0x20;
            SetPixel(row, x, b, g, r);
        }
    }
}

/// @brief 画一个实心矩形（裁剪到画面内）。
void FillRect(std::uint8_t* data, int width, int height, int stride, int x0, int y0, int w, int h,
              std::uint8_t b, std::uint8_t g, std::uint8_t r) {
    for (int y = y0; y < y0 + h && y < height; ++y) {
        if (y < 0) {
            continue;
        }
        std::uint8_t* row = data + static_cast<std::size_t>(y) * static_cast<std::size_t>(stride);
        for (int x = x0; x < x0 + w && x < width; ++x) {
            if (x < 0) {
                continue;
            }
            SetPixel(row, x, b, g, r);
        }
    }
}

/// @brief 把帧号编码成 8x8 的 32 位图案（大端 4 字节 = 32 个格子）。
void PaintFrameIdBlocks(std::uint8_t* data, int width, int height, int stride, int x0, int y0,
                        std::uint32_t frame_id) {
    for (int bit = 0; bit < 32; ++bit) {
        const bool on = ((frame_id >> (31 - bit)) & 1U) != 0;
        const int gx = x0 + (bit % 8) * 3;
        const int gy = y0 + (bit / 8) * 3;
        if (gx >= width || gy >= height) {
            continue;
        }
        FillRect(data, width, height, stride, gx, gy, 2, 2, on ? 0xFF : 0x00, on ? 0xFF : 0x00,
                 on ? 0x00 : 0xFF);
    }
}

/// @brief xorshift32：确定性伪随机。
inline std::uint32_t NextRandom(std::uint32_t* state) {
    std::uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

}  // namespace

SyntheticCapture::SyntheticCapture(int width, int height, SyntheticPattern pattern)
    : m_width(width > 0 ? width : 320),
      m_height(height > 0 ? height : 240),
      m_pattern(pattern) {}

bool SyntheticCapture::Capture(VideoFrame* out) {
    if (out == nullptr) {
        return false;
    }
    out->width = m_width;
    out->height = m_height;
    out->format = PixelFormat::kBGRA;
    out->stride = m_width * 4;
    out->frame_id = m_frame_id;
    out->keyframe = m_frame_id == 0;
    out->data.resize(static_cast<std::size_t>(m_width) * static_cast<std::size_t>(m_height) * 4);
    const int stride = m_width * 4;
    std::uint8_t* data = out->data.data();

    if (m_pattern == SyntheticPattern::kNoise) {
        std::uint32_t state = 0x9E3779B9U ^ (m_frame_id * 2654435761U);
        for (std::size_t i = 0; i < out->data.size(); i += 4) {
            const std::uint32_t value = NextRandom(&state);
            data[i + 0] = static_cast<std::uint8_t>(value);
            data[i + 1] = static_cast<std::uint8_t>(value >> 8);
            data[i + 2] = static_cast<std::uint8_t>(value >> 16);
            data[i + 3] = 255;
        }
    } else {
        PaintBase(data, m_width, m_height, stride);
        if (m_pattern == SyntheticPattern::kScroll) {
            // 水平滚动条：随帧号右移，超出后从左侧回卷。
            const int bar_y = m_height / 2;
            const int bar_w = m_width / 3;
            const int offset = static_cast<int>((m_frame_id * 8U) % static_cast<std::uint32_t>(m_width));
            FillRect(data, m_width, m_height, stride, offset, bar_y, bar_w, 16, 0xFF, 0x80, 0x00);
            if (offset + bar_w > m_width) {
                FillRect(data, m_width, m_height, stride, offset - m_width, bar_y, bar_w, 16, 0xFF,
                         0x80, 0x00);
            }
            PaintFrameIdBlocks(data, m_width, m_height, stride, m_width - 26, 2, m_frame_id);
        } else if (m_pattern == SyntheticPattern::kPatch) {
            // 方块沿对角线移动：每帧只脏两条边。
            const int size = 24;
            const int span_x = (m_width > size) ? (m_width - size) : 1;
            const int span_y = (m_height > size) ? (m_height - size) : 1;
            const int x = static_cast<int>(m_frame_id * 6U % static_cast<std::uint32_t>(span_x + 1));
            const int y = static_cast<int>(m_frame_id * 4U % static_cast<std::uint32_t>(span_y + 1));
            FillRect(data, m_width, m_height, stride, x, y, size, size, 0x00, 0xFF, 0xFF);
        }
        if (m_pattern != SyntheticPattern::kStatic) {
            // kStatic 保持完全静止（差分编码收敛用例依赖零变化）。
            PaintFrameIdBlocks(data, m_width, m_height, stride, 2, 2, m_frame_id & 0xFFU);
        }
    }

    ++m_frame_id;
    return true;
}

}  // namespace go2cpp::media
