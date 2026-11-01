#pragma once

/**
 * @file include/go2cpp/media/avi.hpp
 * @brief 最小 MJPEG AVI 封装（RIFF / hdrl / movi / idx1），用于 JinShan MOD-32 录屏落盘。
 *
 * ## 定位
 *
 * `media` 模块输出的是裸 JPEG 帧（`codec.hpp` 的 MJPEG 编码器产出一个
 * `std::vector<uint8_t>`），要变成「能用播放器/ffmpeg 打开的视频文件」还差一个
 * 容器。本文件提供**只写一次、顺序追加**的 MJPEG AVI 写入器：
 *
 * ```
 * RIFF "AVI "
 *   LIST "hdrl"          avih（主头） + LIST "strl"（strh + strf）
 *   LIST "movi"          "00dc" <jpeg> ...（每帧一个 chunk，奇数长度补 0）
 *   "idx1"               N × {ckid, flags, offset, size}
 * ```
 *
 * 设计取舍：
 *
 * - **只支持 MJPEG**：不做 B 帧/索引重排（MJPEG 每帧都是关键帧），
 *   `idx1` 的 flags 固定 `AVIIF_KEYFRAME`；
 * - **单视频流**：音频/字幕不在这个版本范围内（JinShan 录屏只需要画面）；
 * - **先占位、后回填**：`hdrl`/`movi` 的长度与 `avih.dwTotalFrames` 在
 *   `Finish()` 时用 `pwrite` 回填，避免把整段视频缓存在内存里；
 * - **不做 CRC/校验**：AVI/RIFF 本身没有校验字段，损坏检测靠上层
 *   （帧级 SHA-256 或时长/帧数比对）。
 *
 * ## 写文件流程
 *
 * ```cpp
 * MjpegAviWriter writer;
 * writer.Open("/tmp/screen.avi", {1920, 1080, 30}, &error);
 * for (frame : jpeg_frames) writer.WriteFrame(frame.data(), frame.size(), &error);
 * writer.Finish(&error);   // 回填长度 + 写 idx1；之后才能被 ffprobe 正确解析
 * ```
 *
 * **必须调用 `Finish()`**：不调用时文件头里的长度字段是 0，播放器会认为视频
 * 为空（析构只关闭 fd，不会隐式 Finish——录制中途崩溃的场景应由上层决定是
 * 丢弃还是尝试修复）。
 *
 * ## 边界与陷阱
 *
 * - `WriteFrame` 只接受**完整 JPEG**（SOI `FF D8` 开头、EOI `FF D9` 结尾）；
 *   本类不做转码/校验，传错数据会得到「能解析但解不出画面」的文件；
 * - 帧尺寸必须与 `Open()` 时一致（MJPEG 的 `strf` 固定宽高，不支持中途变分辨率）；
 * - `Finish()` 之后再 `WriteFrame` 会失败；
 * - 文件超过 4 GiB 时 RIFF 的 32 位长度字段会溢出（RIFF 规范限制）。示例
 *   量级的录屏（1080p30 MJPEG ≈ 5~15 MiB/s）约 5~13 分钟达到上限，长时间
 *   录制请分段。
 *
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <unistd.h>
#include <vector>

namespace go2cpp::media {

/// MJPEG AVI 的静态参数（打开后不可变）。
struct MjpegAviConfig {
    std::uint32_t width = 640;   ///< 帧宽（像素）。
    std::uint32_t height = 480;  ///< 帧高（像素）。
    std::uint32_t fps = 30;      ///< 帧率（`dwRate/dwScale` 的分子）。
};

namespace detail {

/// AVI 索引项：ckid(4) + flags(4) + offset(4) + size(4)。
struct AviIndexEntry {
    std::uint32_t offset = 0;  ///< 相对 movi data 起点的 chunk 头偏移。
    std::uint32_t size = 0;    ///< 帧数据字节数（不含 chunk 头）。
};

inline void PutLe16(std::string* out, std::uint16_t value) {
    out->push_back(static_cast<char>(value & 0xffu));
    out->push_back(static_cast<char>((value >> 8) & 0xffu));
}

inline void PutLe32(std::string* out, std::uint32_t value) {
    out->push_back(static_cast<char>(value & 0xffu));
    out->push_back(static_cast<char>((value >> 8) & 0xffu));
    out->push_back(static_cast<char>((value >> 16) & 0xffu));
    out->push_back(static_cast<char>((value >> 24) & 0xffu));
}

inline void PutFourCC(std::string* out, const char fourcc[5]) {
    out->append(fourcc, 4);
}

/// 在 `pos` 处回填 32 位小端整数。
inline bool PatchLe32(int fd, off_t pos, std::uint32_t value) {
    const std::uint8_t bytes[4] = {
        static_cast<std::uint8_t>(value & 0xffu),
        static_cast<std::uint8_t>((value >> 8) & 0xffu),
        static_cast<std::uint8_t>((value >> 16) & 0xffu),
        static_cast<std::uint8_t>((value >> 24) & 0xffu),
    };
    std::size_t written = 0;
    while (written < sizeof(bytes)) {
        const ssize_t n = ::pwrite(fd, bytes + written, sizeof(bytes) - written,
                                   pos + static_cast<off_t>(written));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        written += static_cast<std::size_t>(n);
    }
    return true;
}

/// 顺序写全部字节。
inline bool WriteAll(int fd, const void* data, std::size_t size) {
    const std::uint8_t* bytes = static_cast<const std::uint8_t*>(data);
    std::size_t written = 0;
    while (written < size) {
        const ssize_t n = ::write(fd, bytes + written, size - written);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        written += static_cast<std::size_t>(n);
    }
    return true;
}

}  // namespace detail

/**
 * @brief MJPEG AVI 写入器（顺序追加 + Finish 回填索引）。
 *
 * ## 不变量
 * - `Open()` 成功后 `m_fd >= 0`，直到 `Close()`/析构；
 * - `m_movi_data_start` 是第一个 `00dc` chunk 头的文件偏移，`idx1` 的 offset
 *   相对它计算；
 * - `Finish()` 只执行一次；重复调用返回 true（幂等）。
 */
class MjpegAviWriter {
   public:
    MjpegAviWriter() = default;
    ~MjpegAviWriter() { Close(); }

    MjpegAviWriter(const MjpegAviWriter&) = delete;
    MjpegAviWriter& operator=(const MjpegAviWriter&) = delete;

    /**
     * @brief 创建 AVI 文件并写入 RIFF/hdrl/movi 头（长度字段先占位）。
     * @param path 目标路径（会截断已存在文件）。
     * @param config 宽/高/帧率。
     * @param error 非空时写入错误信息。
     */
    bool Open(std::string path, const MjpegAviConfig& config, std::string* error) {
        Close();
        m_finished = false;
        m_index.clear();
        m_max_frame_bytes = 0;
        if (config.width == 0 || config.height == 0 || config.fps == 0) {
            SetError(error, "宽/高/帧率必须大于 0");
            return false;
        }
        m_config = config;
        m_path = std::move(path);
        m_fd = ::open(m_path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (m_fd < 0) {
            SetError(error, "创建文件失败：" + std::string(std::strerror(errno)));
            return false;
        }

        // RIFF 头：长度在 Finish 回填。
        std::string head;
        head.reserve(16);
        detail::PutFourCC(&head, "RIFF");
        m_riff_size_pos = FilePos() + static_cast<off_t>(head.size());
        detail::PutLe32(&head, 0);
        detail::PutFourCC(&head, "AVI ");
        if (!Write(head, error)) {
            Close();
            return false;
        }

        // LIST hdrl。
        std::string hdrl;
        hdrl.reserve(256);
        detail::PutFourCC(&hdrl, "LIST");
        const off_t hdrl_size_pos = FilePos() + static_cast<off_t>(hdrl.size());
        detail::PutLe32(&hdrl, 0);
        detail::PutFourCC(&hdrl, "hdrl");

        // avih：56 字节主头。
        detail::PutFourCC(&hdrl, "avih");
        detail::PutLe32(&hdrl, 56);
        m_avih_data_pos = FilePos() + static_cast<off_t>(hdrl.size());
        detail::PutLe32(&hdrl, 1000000u / m_config.fps);  // dwMicroSecPerFrame
        detail::PutLe32(&hdrl, 0);                        // dwMaxBytesPerSec
        detail::PutLe32(&hdrl, 0);                        // dwPaddingGranularity
        detail::PutLe32(&hdrl, 0x10u);                    // dwFlags = AVIF_HASINDEX
        detail::PutLe32(&hdrl, 0);                        // dwTotalFrames（回填）
        detail::PutLe32(&hdrl, 0);                        // dwInitialFrames
        detail::PutLe32(&hdrl, 1);                        // dwStreams
        detail::PutLe32(&hdrl, 0);                        // dwSuggestedBufferSize（回填）
        detail::PutLe32(&hdrl, m_config.width);           // dwWidth
        detail::PutLe32(&hdrl, m_config.height);          // dwHeight
        for (int i = 0; i < 4; ++i) {
            detail::PutLe32(&hdrl, 0);  // dwReserved[4]
        }

        // LIST strl（长度 = "strl" 之后到 hdrl 结束）。
        detail::PutFourCC(&hdrl, "LIST");
        const off_t strl_size_pos = FilePos() + static_cast<off_t>(hdrl.size());
        detail::PutLe32(&hdrl, 0);
        detail::PutFourCC(&hdrl, "strl");
        const off_t strl_data_pos = FilePos() + static_cast<off_t>(hdrl.size());

        // strh：56 字节流头。
        detail::PutFourCC(&hdrl, "strh");
        detail::PutLe32(&hdrl, 56);
        m_strh_data_pos = FilePos() + static_cast<off_t>(hdrl.size());
        detail::PutFourCC(&hdrl, "vids");      // fccType
        detail::PutFourCC(&hdrl, "MJPG");      // fccHandler
        detail::PutLe32(&hdrl, 0);             // dwFlags
        detail::PutLe16(&hdrl, 0);             // wPriority
        detail::PutLe16(&hdrl, 0);             // wLanguage
        detail::PutLe32(&hdrl, 0);             // dwInitialFrames
        detail::PutLe32(&hdrl, 1);             // dwScale
        detail::PutLe32(&hdrl, m_config.fps);  // dwRate
        detail::PutLe32(&hdrl, 0);             // dwStart
        detail::PutLe32(&hdrl, 0);             // dwLength（回填）
        detail::PutLe32(&hdrl, 0);             // dwSuggestedBufferSize（回填）
        detail::PutLe32(&hdrl, 0xffffffffu);   // dwQuality = -1（默认）
        detail::PutLe32(&hdrl, 0);             // dwSampleSize
        detail::PutLe16(&hdrl, 0);             // rcFrame.left
        detail::PutLe16(&hdrl, 0);             // rcFrame.top
        detail::PutLe16(&hdrl, static_cast<std::uint16_t>(m_config.width));   // right
        detail::PutLe16(&hdrl, static_cast<std::uint16_t>(m_config.height));  // bottom

        // strf：40 字节 BITMAPINFOHEADER。
        detail::PutFourCC(&hdrl, "strf");
        detail::PutLe32(&hdrl, 40);
        detail::PutLe32(&hdrl, 40);                                     // biSize
        detail::PutLe32(&hdrl, m_config.width);                         // biWidth
        detail::PutLe32(&hdrl, m_config.height);                        // biHeight
        detail::PutLe16(&hdrl, 1);                                      // biPlanes
        detail::PutLe16(&hdrl, 24);                                     // biBitCount
        detail::PutFourCC(&hdrl, "MJPG");                               // biCompression
        detail::PutLe32(&hdrl, m_config.width * m_config.height * 3u);  // biSizeImage
        detail::PutLe32(&hdrl, 0);  // biXPelsPerMeter
        detail::PutLe32(&hdrl, 0);  // biYPelsPerMeter
        detail::PutLe32(&hdrl, 0);  // biClrUsed
        detail::PutLe32(&hdrl, 0);  // biClrImportant

        if (!Write(hdrl, error)) {
            Close();
            return false;
        }
        const off_t hdrl_end = FilePos();
        const bool patched =
            detail::PatchLe32(m_fd, hdrl_size_pos,
                              static_cast<std::uint32_t>(hdrl_end - hdrl_size_pos - 4)) &&
            detail::PatchLe32(m_fd, strl_size_pos,
                              static_cast<std::uint32_t>(hdrl_end - strl_data_pos));
        if (!patched) {
            SetError(error, "回填 hdrl 长度失败");
            Close();
            return false;
        }

        // LIST movi：长度在 Finish 回填。
        std::string movi;
        movi.reserve(16);
        detail::PutFourCC(&movi, "LIST");
        m_movi_size_pos = FilePos() + static_cast<off_t>(movi.size());
        detail::PutLe32(&movi, 0);
        detail::PutFourCC(&movi, "movi");
        if (!Write(movi, error)) {
            Close();
            return false;
        }
        m_movi_data_start = FilePos();
        m_open = true;
        return true;
    }

    /**
     * @brief 追加一帧 JPEG（写入 `00dc` chunk，奇数长度补 1 字节 0）。
     * @param jpeg 完整 JPEG 字节（SOI..EOI）。
     * @param size 字节数（必须 > 0 且 ≤ 4 GiB - 8）。
     */
    bool WriteFrame(const std::uint8_t* jpeg, std::size_t size, std::string* error) {
        if (!m_open || m_finished) {
            SetError(error, "写入前必须先 Open，且不能晚于 Finish");
            return false;
        }
        if (jpeg == nullptr || size == 0) {
            SetError(error, "空帧");
            return false;
        }
        if (size > 0xffffff00u) {
            SetError(error, "单帧过大（超过 RIFF chunk 上限）");
            return false;
        }
        const off_t pos = FilePos();
        if (pos < 0) {
            SetError(error, "获取文件位置失败");
            return false;
        }
        std::string chunk;
        chunk.reserve(8 + size + 1);
        detail::PutFourCC(&chunk, "00dc");
        detail::PutLe32(&chunk, static_cast<std::uint32_t>(size));
        chunk.append(reinterpret_cast<const char*>(jpeg), size);
        if ((size & 1u) != 0) {
            chunk.push_back('\0');  // RIFF chunk 按偶数字节对齐。
        }
        if (!Write(chunk, error)) {
            return false;
        }
        detail::AviIndexEntry entry;
        entry.offset = static_cast<std::uint32_t>(pos - m_movi_data_start);
        entry.size = static_cast<std::uint32_t>(size);
        m_index.push_back(entry);
        if (size > m_max_frame_bytes) {
            m_max_frame_bytes = static_cast<std::uint32_t>(size);
        }
        return true;
    }

    /**
     * @brief 写 `idx1` 索引并回填所有长度/计数，之后文件可被播放器解析。
     * @note 幂等：重复调用直接返回 true。
     */
    bool Finish(std::string* error) {
        if (!m_open) {
            SetError(error, "尚未 Open");
            return false;
        }
        if (m_finished) {
            return true;
        }
        const off_t movi_end = FilePos();
        if (movi_end < 0) {
            SetError(error, "获取文件位置失败");
            return false;
        }
        // movi LIST 长度 = "movi" 之后的 chunk 数据字节数。
        if (!detail::PatchLe32(m_fd, m_movi_size_pos,
                               static_cast<std::uint32_t>(movi_end - m_movi_data_start))) {
            SetError(error, "回填 movi 长度失败");
            return false;
        }

        // idx1：16 字节/项。
        std::string index;
        index.reserve(8 + m_index.size() * 16);
        detail::PutFourCC(&index, "idx1");
        detail::PutLe32(&index, static_cast<std::uint32_t>(m_index.size() * 16));
        for (const detail::AviIndexEntry& entry : m_index) {
            detail::PutFourCC(&index, "00dc");
            detail::PutLe32(&index, 0x10u);  // AVIIF_KEYFRAME
            detail::PutLe32(&index, entry.offset);
            detail::PutLe32(&index, entry.size);
        }
        if (!Write(index, error)) {
            return false;
        }

        const off_t file_end = FilePos();
        const bool patched =
            detail::PatchLe32(m_fd, m_riff_size_pos,
                              static_cast<std::uint32_t>(file_end - m_riff_size_pos - 4)) &&
            detail::PatchLe32(m_fd, m_avih_data_pos + 16,
                              static_cast<std::uint32_t>(m_index.size())) &&
            detail::PatchLe32(m_fd, m_avih_data_pos + 28, m_max_frame_bytes) &&
            detail::PatchLe32(m_fd, m_strh_data_pos + 32,
                              static_cast<std::uint32_t>(m_index.size())) &&
            detail::PatchLe32(m_fd, m_strh_data_pos + 36, m_max_frame_bytes);
        if (!patched) {
            SetError(error, "回填文件头失败");
            return false;
        }
        if (::fsync(m_fd) != 0) {
            SetError(error, "fsync 失败：" + std::string(std::strerror(errno)));
            return false;
        }
        m_finished = true;
        return true;
    }

    /// @brief 关闭文件（未 Finish 时文件不完整，调用方自行决定是否保留）。
    void Close() {
        if (m_fd >= 0) {
            (void)::close(m_fd);
            m_fd = -1;
        }
        m_open = false;
    }

    /// @brief 是否已打开。
    bool is_open() const noexcept { return m_open; }
    /// @brief 是否已 Finish。
    bool is_finished() const noexcept { return m_finished; }
    /// @brief 已写帧数。
    std::uint32_t frame_count() const noexcept {
        return static_cast<std::uint32_t>(m_index.size());
    }
    /// @brief 目标路径。
    const std::string& path() const noexcept { return m_path; }
    /// @brief 配置。
    const MjpegAviConfig& config() const noexcept { return m_config; }

   private:
    bool Write(const std::string& bytes, std::string* error) {
        if (!detail::WriteAll(m_fd, bytes.data(), bytes.size())) {
            SetError(error, "写文件失败：" + std::string(std::strerror(errno)));
            return false;
        }
        return true;
    }

    off_t FilePos() const noexcept { return ::lseek(m_fd, 0, SEEK_CUR); }

    static void SetError(std::string* error, const std::string& message) {
        if (error != nullptr) {
            *error = message;
        }
    }

    int m_fd = -1;                                ///< 文件描述符。
    std::string m_path;                           ///< 目标路径。
    MjpegAviConfig m_config;                      ///< 宽/高/帧率。
    std::vector<detail::AviIndexEntry> m_index;   ///< 待写入 idx1 的帧索引。
    off_t m_riff_size_pos = 0;                    ///< RIFF 长度字段位置。
    off_t m_avih_data_pos = 0;                    ///< avih 数据起点。
    off_t m_strh_data_pos = 0;                    ///< strh 数据起点。
    off_t m_movi_size_pos = 0;                    ///< movi LIST 长度字段位置。
    off_t m_movi_data_start = 0;                  ///< movi 数据起点（idx1 基准）。
    std::uint32_t m_max_frame_bytes = 0;          ///< 最大帧字节数。
    bool m_open = false;                          ///< Open 成功且未 Close。
    bool m_finished = false;                      ///< Finish 已执行。
};

}  // namespace go2cpp::media
