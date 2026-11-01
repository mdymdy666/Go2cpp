/**
 * @file codec_delta.cpp
 * @brief 16x16 块级差分编码器（kDeltaI420）：无损、纯整数、零第三方依赖。
 * @details 算法见 `codec.hpp` 里 `CodecKind::kDeltaI420` 的线格式说明。核心取舍：
 *
 *          - **块级差分**：桌面画面的变化高度局部化（光标、时钟、滚动条），
 *            整屏只有少数 16x16 宏块变化。静止帧的负载退化为 4 字节
 *            （bitmap_bytes = 0），1280x720 从 1.3MB 降到 4 字节。
 *          - **无损**：不做量化，解码结果与编码输入逐字节相等，端到端可以
 *            按 `FrameEqual` 严格断言，不需要 PSNR 容差。
 *          - **块内 RLE**：单个块内大面积为同色（窗口底色），`(run,value)` 对比
 *            原始 384 字节更短；编码器逐块选更短的一种，解码器按 mode 读。
 *          - **无反向通道**：差分依赖参考帧，丢一个分片就丢整帧、后续帧也会因
 *            参考不一致而错；因此由上层（pipeline）每 N 帧强制关键帧自愈。
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include <algorithm>
#include <cstring>
#include <vector>

#include "codec_internal.hpp"
#include "go2cpp/media/codec.hpp"
#include "go2cpp/media/pixel.hpp"

namespace go2cpp::media {
namespace {

/// @brief 单个宏块最多包含的字节数（Y 16x16 + U 8x8 + V 8x8）。
constexpr std::size_t kMaxBlockBytes = 16 * 16 + 8 * 8 + 8 * 8;

/// @brief 平面坐标下的宏块几何。
struct BlockGeometry {
    int bx = 0;       ///< 宏块列号。
    int by = 0;       ///< 宏块行号。
    int y_w = 0;      ///< Y 块宽（<= 16）。
    int y_h = 0;      ///< Y 块高（<= 16）。
    int c_w = 0;      ///< 色度块宽（<= 8）。
    int c_h = 0;      ///< 色度块高（<= 8）。
};

/// @brief 计算宏块几何（边缘宏块裁剪）。
BlockGeometry ComputeGeometry(int width, int height, int chroma_w, int chroma_h, int bx,
                              int by) noexcept {
    BlockGeometry geo;
    geo.bx = bx;
    geo.by = by;
    geo.y_w = std::min(kMacroblockSize, width - bx * kMacroblockSize);
    geo.y_h = std::min(kMacroblockSize, height - by * kMacroblockSize);
    geo.c_w = std::min(kMacroblockSize / 2, chroma_w - bx * (kMacroblockSize / 2));
    geo.c_h = std::min(kMacroblockSize / 2, chroma_h - by * (kMacroblockSize / 2));
    return geo;
}

/// @brief 宏块字节数。
std::size_t BlockBytes(const BlockGeometry& geo) noexcept {
    return static_cast<std::size_t>(geo.y_w) * static_cast<std::size_t>(geo.y_h) +
           static_cast<std::size_t>(geo.c_w) * static_cast<std::size_t>(geo.c_h) * 2;
}

/// @brief 把宏块的 Y/U/V 数据按顺序拷出（返回字节数）。
std::size_t GatherBlock(const std::uint8_t* plane_y, const std::uint8_t* plane_u,
                        const std::uint8_t* plane_v, int width, int chroma_w,
                        const BlockGeometry& geo, std::uint8_t* dst) noexcept {
    std::size_t offset = 0;
    const int x0 = geo.bx * kMacroblockSize;
    const int y0 = geo.by * kMacroblockSize;
    for (int row = 0; row < geo.y_h; ++row) {
        std::memcpy(dst + offset, plane_y + static_cast<std::size_t>(y0 + row) * width + x0,
                    static_cast<std::size_t>(geo.y_w));
        offset += static_cast<std::size_t>(geo.y_w);
    }
    const int cx0 = geo.bx * (kMacroblockSize / 2);
    const int cy0 = geo.by * (kMacroblockSize / 2);
    for (int row = 0; row < geo.c_h; ++row) {
        std::memcpy(dst + offset, plane_u + static_cast<std::size_t>(cy0 + row) * chroma_w + cx0,
                    static_cast<std::size_t>(geo.c_w));
        offset += static_cast<std::size_t>(geo.c_w);
    }
    for (int row = 0; row < geo.c_h; ++row) {
        std::memcpy(dst + offset, plane_v + static_cast<std::size_t>(cy0 + row) * chroma_w + cx0,
                    static_cast<std::size_t>(geo.c_w));
        offset += static_cast<std::size_t>(geo.c_w);
    }
    return offset;
}

/// @brief 把宏块数据写回三个平面。
void ScatterBlock(std::uint8_t* plane_y, std::uint8_t* plane_u, std::uint8_t* plane_v, int width,
                  int chroma_w, const BlockGeometry& geo, const std::uint8_t* src) noexcept {
    std::size_t offset = 0;
    const int x0 = geo.bx * kMacroblockSize;
    const int y0 = geo.by * kMacroblockSize;
    for (int row = 0; row < geo.y_h; ++row) {
        std::memcpy(plane_y + static_cast<std::size_t>(y0 + row) * width + x0, src + offset,
                    static_cast<std::size_t>(geo.y_w));
        offset += static_cast<std::size_t>(geo.y_w);
    }
    const int cx0 = geo.bx * (kMacroblockSize / 2);
    const int cy0 = geo.by * (kMacroblockSize / 2);
    for (int row = 0; row < geo.c_h; ++row) {
        std::memcpy(plane_u + static_cast<std::size_t>(cy0 + row) * chroma_w + cx0, src + offset,
                    static_cast<std::size_t>(geo.c_w));
        offset += static_cast<std::size_t>(geo.c_w);
    }
    for (int row = 0; row < geo.c_h; ++row) {
        std::memcpy(plane_v + static_cast<std::size_t>(cy0 + row) * chroma_w + cx0, src + offset,
                    static_cast<std::size_t>(geo.c_w));
        offset += static_cast<std::size_t>(geo.c_w);
    }
}

/// @brief 逐像素比较当前帧与参考帧的宏块。
bool MacroblockDiffers(const std::uint8_t* cur, const std::uint8_t* ref, int width, int height,
                       int chroma_w, int chroma_h, const BlockGeometry& geo) noexcept {
    const int x0 = geo.bx * kMacroblockSize;
    const int y0 = geo.by * kMacroblockSize;
    for (int row = 0; row < geo.y_h; ++row) {
        const std::uint8_t* a = cur + static_cast<std::size_t>(y0 + row) * width + x0;
        const std::uint8_t* b = ref + static_cast<std::size_t>(y0 + row) * width + x0;
        if (std::memcmp(a, b, static_cast<std::size_t>(geo.y_w)) != 0) {
            return true;
        }
    }
    const std::size_t luma = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    const int cx0 = geo.bx * (kMacroblockSize / 2);
    const int cy0 = geo.by * (kMacroblockSize / 2);
    for (int plane = 0; plane < 2; ++plane) {
        const std::size_t base = luma + static_cast<std::size_t>(plane) * chroma_w * chroma_h;
        for (int row = 0; row < geo.c_h; ++row) {
            const std::uint8_t* a =
                cur + base + static_cast<std::size_t>(cy0 + row) * chroma_w + cx0;
            const std::uint8_t* b =
                ref + base + static_cast<std::size_t>(cy0 + row) * chroma_w + cx0;
            if (std::memcmp(a, b, static_cast<std::size_t>(geo.c_w)) != 0) {
                return true;
            }
        }
    }
    return false;
}

/// @brief RLE 编码：(run u8, value u8) 对；返回写入字节数（偶数，最多 2*n）。
std::size_t RleEncode(const std::uint8_t* src, std::size_t bytes, std::uint8_t* dst) noexcept {
    std::size_t offset = 0;
    std::size_t i = 0;
    while (i < bytes) {
        const std::uint8_t value = src[i];
        std::size_t run = 1;
        while (i + run < bytes && src[i + run] == value && run < 255) {
            ++run;
        }
        dst[offset++] = static_cast<std::uint8_t>(run);
        dst[offset++] = value;
        i += run;
    }
    return offset;
}

/// @brief RLE 解码到恰好 `bytes` 字节。
bool RleDecode(const std::uint8_t* src, std::size_t src_bytes, std::uint8_t* dst,
               std::size_t bytes) noexcept {
    std::size_t offset = 0;
    std::size_t i = 0;
    while (i + 1 < src_bytes) {
        const std::size_t run = src[i];
        if (run == 0) {
            return false;
        }
        if (offset + run > bytes) {
            return false;
        }
        std::memset(dst + offset, src[i + 1], run);
        offset += run;
        i += 2;
    }
    return offset == bytes;
}

/// @brief 差分编码器。
class DeltaEncoder final : public VideoEncoder {
   public:
    DeltaEncoder(int width, int height) : m_width(width), m_height(height) {}

    bool Encode(const VideoFrame& frame, std::vector<std::uint8_t>* out) override {
        std::vector<std::uint8_t> i420;
        int width = 0;
        int height = 0;
        if (!detail::ToCompactI420(frame, &i420, &width, &height)) {
            return false;
        }
        const bool geometry_changed = (m_ref_w != width || m_ref_h != height);
        const bool key = m_force_keyframe || geometry_changed || m_ref.empty();
        if (key) {
            m_ref.assign(i420.begin(), i420.end());
            m_ref_w = width;
            m_ref_h = height;
            m_force_keyframe = false;
        }

        const int chroma_w = (width + 1) / 2;
        const int chroma_h = (height + 1) / 2;
        const int mb_cols = (width + kMacroblockSize - 1) / kMacroblockSize;
        const int mb_rows = (height + kMacroblockSize - 1) / kMacroblockSize;
        const std::size_t mb_count = static_cast<std::size_t>(mb_cols) * static_cast<std::size_t>(mb_rows);

        std::vector<std::uint8_t> payload;
        payload.reserve(static_cast<std::size_t>(width) * 3U);
        std::uint8_t block[kMaxBlockBytes];
        std::uint8_t rle[kMaxBlockBytes * 2];
        static const std::uint8_t kZeroBitmap[4] = {0, 0, 0, 0};

        if (!key) {
            const std::size_t bitmap_bytes = (mb_count + 7) / 8;
            std::vector<std::uint8_t> bitmap(bitmap_bytes, 0);
            std::vector<std::uint16_t> dirty;
            dirty.reserve(mb_count / 4 + 1);
            for (int by = 0; by < mb_rows; ++by) {
                for (int bx = 0; bx < mb_cols; ++bx) {
                    const BlockGeometry geo = ComputeGeometry(width, height, chroma_w, chroma_h, bx, by);
                    const std::size_t index = static_cast<std::size_t>(by) * static_cast<std::size_t>(mb_cols) +
                                              static_cast<std::size_t>(bx);
                    if (MacroblockDiffers(i420.data(), m_ref.data(), width, height, chroma_w, chroma_h,
                                          geo)) {
                        bitmap[index / 8] |= static_cast<std::uint8_t>(1U << (index % 8));
                        dirty.push_back(static_cast<std::uint16_t>(index));
                    }
                }
            }
            if (dirty.empty()) {
                payload.assign(kZeroBitmap, kZeroBitmap + 4);
            } else {
                const std::size_t header_bytes = 4 + bitmap.size();
                payload.resize(header_bytes);
                payload[0] = static_cast<std::uint8_t>(bitmap_bytes >> 24);
                payload[1] = static_cast<std::uint8_t>(bitmap_bytes >> 16);
                payload[2] = static_cast<std::uint8_t>(bitmap_bytes >> 8);
                payload[3] = static_cast<std::uint8_t>(bitmap_bytes);
                std::memcpy(payload.data() + 4, bitmap.data(), bitmap.size());
                AppendBlocks(i420.data(), width, height, chroma_w, chroma_h, mb_cols, dirty, block,
                             rle, &payload);
            }
        } else {
            std::vector<std::uint16_t> all;
            all.reserve(mb_count);
            for (std::size_t index = 0; index < mb_count; ++index) {
                all.push_back(static_cast<std::uint16_t>(index));
            }
            AppendBlocks(i420.data(), width, height, chroma_w, chroma_h, mb_cols, all, block, rle,
                         &payload);
        }

        out->resize(MediaHeader::kBytes + payload.size());
        MediaHeader header;
        header.codec = CodecKind::kDeltaI420;
        header.keyframe = key;
        header.width = static_cast<std::uint16_t>(width);
        header.height = static_cast<std::uint16_t>(height);
        header.frame_id = frame.frame_id;
        header.payload_bytes = static_cast<std::uint32_t>(payload.size());
        header.capture_ns = static_cast<std::uint64_t>(frame.capture_ns);
        EncodeMediaHeader(header, out->data());
        std::memcpy(out->data() + MediaHeader::kBytes, payload.data(), payload.size());

        ++m_frames;
        m_bytes += out->size();
        if (key) {
            ++m_keyframes;
        }
        m_ref.assign(i420.begin(), i420.end());
        if (out->size() > kMaxEncodedFrameBytes) {
            ++m_oversize;
            return false;
        }
        return true;
    }

    CodecKind kind() const noexcept override {
        return CodecKind::kDeltaI420;
    }
    const char* Name() const noexcept override {
        return m_oversize > 0 ? "delta-i420(oversize!)" : "delta-i420";
    }
    void ForceKeyframe() noexcept override {
        m_force_keyframe = true;
    }
    std::uint64_t encoded_frames() const noexcept override {
        return m_frames;
    }
    std::uint64_t encoded_bytes() const noexcept override {
        return m_bytes;
    }
    std::uint64_t keyframes() const noexcept override {
        return m_keyframes;
    }

    /// @brief 超过 VideoChannel 单帧上限而被拒绝的帧数。
    std::uint64_t oversize_frames() const noexcept {
        return m_oversize;
    }

   private:
    /// @brief 追加一批宏块（每个块 1B mode + 数据）。
    void AppendBlocks(const std::uint8_t* i420, int width, int height, int chroma_w, int chroma_h,
                      int mb_cols, const std::vector<std::uint16_t>& blocks, std::uint8_t* block,
                      std::uint8_t* rle, std::vector<std::uint8_t>* payload) {
        const std::size_t luma = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
        const std::uint8_t* plane_y = i420;
        const std::uint8_t* plane_u = i420 + luma;
        const std::uint8_t* plane_v = i420 + luma + static_cast<std::size_t>(chroma_w) * chroma_h;
        for (std::uint16_t index : blocks) {
            const int bx = static_cast<int>(index) % mb_cols;
            const int by = static_cast<int>(index) / mb_cols;
            const BlockGeometry geo = ComputeGeometry(width, height, chroma_w, chroma_h, bx, by);
            const std::size_t bytes =
                GatherBlock(plane_y, plane_u, plane_v, width, chroma_w, geo, block);
            const std::size_t rle_bytes = RleEncode(block, bytes, rle);
            if (rle_bytes + 2 < bytes) {
                payload->push_back(1);  // mode = rle
                payload->push_back(static_cast<std::uint8_t>(rle_bytes >> 8));
                payload->push_back(static_cast<std::uint8_t>(rle_bytes));
                payload->insert(payload->end(), rle, rle + rle_bytes);
            } else {
                payload->push_back(0);  // mode = raw
                payload->insert(payload->end(), block, block + bytes);
            }
        }
    }

    int m_width;
    int m_height;
    int m_ref_w = 0;
    int m_ref_h = 0;
    bool m_force_keyframe = true;
    std::vector<std::uint8_t> m_ref;
    std::uint64_t m_frames = 0;
    std::uint64_t m_bytes = 0;
    std::uint64_t m_keyframes = 0;
    std::uint64_t m_oversize = 0;
};

/// @brief 差分解码器。
class DeltaDecoder final : public VideoDecoder {
   public:
    DeltaDecoder(int width, int height) : m_width(width), m_height(height) {}

    bool Decode(const std::uint8_t* data, std::size_t bytes, VideoFrame* out) override {
        MediaHeader header;
        if (!DecodeMediaHeader(data, bytes, &header) || header.codec != CodecKind::kDeltaI420) {
            ++m_dropped;
            return false;
        }
        const int width = header.width;
        const int height = header.height;
        const int chroma_w = (width + 1) / 2;
        const int chroma_h = (height + 1) / 2;
        const int mb_cols = (width + kMacroblockSize - 1) / kMacroblockSize;
        const int mb_rows = (height + kMacroblockSize - 1) / kMacroblockSize;
        const std::size_t mb_count =
            static_cast<std::size_t>(mb_cols) * static_cast<std::size_t>(mb_rows);

        if (header.keyframe) {
            m_ref.assign(I420FrameBytes(width, height), 0);
            m_ref_w = width;
            m_ref_h = height;
        } else if (m_ref.empty() || m_ref_w != width || m_ref_h != height) {
            // 缺参考帧：差分帧无法还原，整帧丢弃，等下一个关键帧。
            ++m_dropped;
            return false;
        }

        const std::size_t luma = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
        std::uint8_t* plane_y = m_ref.data();
        std::uint8_t* plane_u = m_ref.data() + luma;
        std::uint8_t* plane_v = m_ref.data() + luma + static_cast<std::size_t>(chroma_w) * chroma_h;
        const std::uint8_t* cursor = data + MediaHeader::kBytes;
        const std::uint8_t* end = cursor + header.payload_bytes;

        std::vector<std::uint16_t> blocks;
        if (!header.keyframe) {
            if (static_cast<std::size_t>(end - cursor) < 4) {
                ++m_dropped;
                return false;
            }
            const std::size_t bitmap_bytes = (static_cast<std::size_t>(cursor[0]) << 24) |
                                             (static_cast<std::size_t>(cursor[1]) << 16) |
                                             (static_cast<std::size_t>(cursor[2]) << 8) |
                                             static_cast<std::size_t>(cursor[3]);
            cursor += 4;
            if (bitmap_bytes > 0) {
                if (static_cast<std::size_t>(end - cursor) < bitmap_bytes) {
                    ++m_dropped;
                    return false;
                }
                for (std::size_t index = 0; index < mb_count; ++index) {
                    if ((cursor[index / 8] & (1U << (index % 8))) != 0) {
                        blocks.push_back(static_cast<std::uint16_t>(index));
                    }
                }
                cursor += bitmap_bytes;
            }
        } else {
            blocks.reserve(mb_count);
            for (std::size_t index = 0; index < mb_count; ++index) {
                blocks.push_back(static_cast<std::uint16_t>(index));
            }
        }

        std::uint8_t block[kMaxBlockBytes];
        for (std::uint16_t index : blocks) {
            const int bx = static_cast<int>(index) % mb_cols;
            const int by = static_cast<int>(index) / mb_cols;
            const BlockGeometry geo = ComputeGeometry(width, height, chroma_w, chroma_h, bx, by);
            const std::size_t block_bytes = BlockBytes(geo);
            if (cursor >= end) {
                ++m_dropped;
                return false;
            }
            const std::uint8_t mode = *cursor++;
            if (mode == 0) {
                if (static_cast<std::size_t>(end - cursor) < block_bytes) {
                    ++m_dropped;
                    return false;
                }
                std::memcpy(block, cursor, block_bytes);
                cursor += block_bytes;
            } else if (mode == 1) {
                if (static_cast<std::size_t>(end - cursor) < 2) {
                    ++m_dropped;
                    return false;
                }
                const std::size_t rle_bytes =
                    (static_cast<std::size_t>(cursor[0]) << 8) | static_cast<std::size_t>(cursor[1]);
                cursor += 2;
                if (static_cast<std::size_t>(end - cursor) < rle_bytes ||
                    !RleDecode(cursor, rle_bytes, block, block_bytes)) {
                    ++m_dropped;
                    return false;
                }
                cursor += rle_bytes;
            } else {
                ++m_dropped;
                return false;
            }
            ScatterBlock(plane_y, plane_u, plane_v, width, chroma_w, geo, block);
        }

        if (cursor != end) {
            ++m_dropped;
            return false;
        }

        out->width = width;
        out->height = height;
        out->format = PixelFormat::kI420;
        out->stride = 0;
        out->frame_id = header.frame_id;
        out->capture_ns = static_cast<std::int64_t>(header.capture_ns);
        out->keyframe = header.keyframe;
        out->data = m_ref;
        ++m_decoded;
        return true;
    }

    CodecKind kind() const noexcept override {
        return CodecKind::kDeltaI420;
    }
    std::uint64_t decoded_frames() const noexcept override {
        return m_decoded;
    }
    std::uint64_t dropped_frames() const noexcept override {
        return m_dropped;
    }

   private:
    int m_width;
    int m_height;
    int m_ref_w = 0;
    int m_ref_h = 0;
    std::vector<std::uint8_t> m_ref;
    std::uint64_t m_decoded = 0;
    std::uint64_t m_dropped = 0;
};

}  // namespace

namespace detail {

std::unique_ptr<VideoEncoder> CreateDeltaEncoder(int width, int height) {
    return std::make_unique<DeltaEncoder>(width, height);
}

std::unique_ptr<VideoDecoder> CreateDeltaDecoder(int width, int height) {
    return std::make_unique<DeltaDecoder>(width, height);
}

}  // namespace detail

}  // namespace go2cpp::media
