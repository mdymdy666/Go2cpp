/**
 * @file capture_x11.cpp
 * @brief X11 采集：XShm 共享内存优先，回退 XGetImage。
 * @details WSL2 的 WSLg 提供完整 X11（`$DISPLAY=:0`），`MIT-SHM` 扩展实测存在，
 *          因此 `XShmGetImage` 是默认路径：图像数据落在 `shmget` 分配的共享段里，
 *          X 服务器直接写这段内存。默认把共享段拷成 BGRA 自有帧（历史语义）；
 *          打开 `X11CaptureConfig::zero_copy` 后，帧直接借用 XShm 段（零整帧拷贝），
 *          只原地把 alpha 字节填 0xFF。借用视图有效期到下一次 `Capture()`。
 *
 *          回退链：`XShmQueryExtension` 失败 / `shmget` 失败 / `XShmAttach` 失败
 *          → 使用 `XGetImage`（走 X11 请求，慢但通用）。`XGetImage` 的缓冲随
 *          `XDestroyImage` 释放，这条路径必须拷贝，零拷贝对它不生效。
 *
 *          格式假设：depth-24 的 ZPixmap、`bits_per_pixel == 32`、小端，内存布局是
 *          B,G,R,X。X 通道填 0xFF 得到 BGRA。若遇到其他 bpp，用 `XGetPixel` 逐点
 *          读取（慢，但保证正确）。
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <sys/ipc.h>
#include <sys/shm.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>

#include "capture_kernels.hpp"
#include "go2cpp/media/capture.hpp"

namespace go2cpp::media {
namespace {

/// @brief XImage 是否是「32bpp / 小端 / RGB 顺序」的快速格式。
/// @details 该格式下内存布局就是 B,G,R,X，可以整行向量化处理；否则要逐点
///          `XGetPixel`（任意 bpp/字节序都能正确解出颜色）。
bool IsFastBgraFormat(const XImage* image) noexcept {
    return image != nullptr && image->bits_per_pixel == 32 && image->byte_order == LSBFirst &&
           image->red_mask == 0x00FF0000U && image->green_mask == 0x0000FF00U &&
           image->blue_mask == 0x000000FFU;
}

/// @brief 忽略 X11 异步错误，避免默认处理器直接 exit(1)。
/// @details `XGetImage` 在区域越界/服务器不支持时会产生 BadMatch 异步错误；
///          默认处理器会打印并退出进程。采集失败应由 `Capture()` 的返回值表达，
///          因此这里吞掉错误码（调用方仍能通过返回值判断失败）。
int IgnoreXError(Display*, XErrorEvent*) noexcept {
    return 0;
}

/// @brief X11 采集实现（PIMPL 隐藏 Xlib 类型）。
class X11CaptureImpl final : public VideoSource {
   public:
    ~X11CaptureImpl() override {
        Close();
    }

    bool Init(const X11CaptureConfig& config, std::string* error) {
        m_display = XOpenDisplay(nullptr);
        if (m_display == nullptr) {
            if (error != nullptr) {
                *error = "XOpenDisplay failed (DISPLAY unset or X server unreachable)";
            }
            return false;
        }
        ::XSetErrorHandler(IgnoreXError);
        m_shm_info.shmid = -1;
        m_screen = DefaultScreen(m_display);
        m_target = config.target_window != 0 ? static_cast<Window>(config.target_window)
                                             : RootWindow(m_display, m_screen);
        m_zero_copy = config.zero_copy;
        m_fill_alpha = config.fill_alpha_in_place;

        int target_width = DisplayWidth(m_display, m_screen);
        int target_height = DisplayHeight(m_display, m_screen);
        if (config.target_window != 0) {
            XWindowAttributes attributes{};
            if (XGetWindowAttributes(m_display, m_target, &attributes) == 0) {
                if (error != nullptr) {
                    *error = "XGetWindowAttributes(target_window) failed";
                }
                Close();
                return false;
            }
            target_width = attributes.width;
            target_height = attributes.height;
        }
        // 抓取区域裁剪到目标 drawable 内，避免 XShmGetImage 越界返回 BadMatch。
        m_x = std::max(0, std::min(config.x, target_width - 1));
        m_y = std::max(0, std::min(config.y, target_height - 1));
        m_width = config.width > 0 ? config.width : (target_width - m_x);
        m_height = config.height > 0 ? config.height : (target_height - m_y);
        m_width = std::max(1, std::min(m_width, target_width - m_x));
        m_height = std::max(1, std::min(m_height, target_height - m_y));

        m_use_shm = config.use_shm;
        if (m_use_shm && InitShm(error)) {
            return true;
        }
        if (error != nullptr && !error->empty()) {
            error->clear();  // 回退到 XGetImage 时不算失败
        }
        return true;
    }

    bool Capture(VideoFrame* out) override {
        if (out == nullptr || m_display == nullptr) {
            return false;
        }
        m_last_borrowed = false;
        TryRebuildShm();
        if (m_image != nullptr) {
            if (XShmGetImage(m_display, m_target, m_image, m_x, m_y, AllPlanes)) {
                return CaptureFromShm(out);
            }
            // WSLg/XWayland 在窗口重绘/合成期间偶发 BadMatch：先 XSync 清掉
            // 待处理错误再重试一次，避免一次瞬态失败就永久退化到 XGetImage。
            XSync(m_display, False);
            if (XShmGetImage(m_display, m_target, m_image, m_x, m_y, AllPlanes)) {
                return CaptureFromShm(out);
            }
            // 连续失败：关闭共享段，1 秒冷却后尝试重建（服务器重启/段失效也能自愈）。
            CloseShm();
            ScheduleShmRebuild();
        }
        XImage* image = XGetImage(m_display, m_target, m_x, m_y,
                                  static_cast<unsigned int>(m_width),
                                  static_cast<unsigned int>(m_height), AllPlanes, ZPixmap);
        if (image == nullptr) {
            return false;
        }
        const bool ok = CopyImage(image, out);
        XDestroyImage(image);
        return ok;
    }

    int width() const noexcept override {
        return m_width;
    }
    int height() const noexcept override {
        return m_height;
    }
    const char* name() const noexcept override {
        return m_shm ? "x11-shm" : "x11-getimage";
    }
    bool available() const noexcept override {
        return m_display != nullptr;
    }

    bool last_capture_borrowed() const noexcept override {
        return m_last_borrowed;
    }

    /// @brief 使用的采集方式是否 XShm。
    bool using_shm() const noexcept {
        return m_shm;
    }

   private:
    bool InitShm(std::string* error) {
        if (!XShmQueryExtension(m_display)) {
            if (error != nullptr) {
                *error = "MIT-SHM extension missing";
            }
            return false;
        }
        Visual* visual = DefaultVisual(m_display, m_screen);
        const int depth = DefaultDepth(m_display, m_screen);
        m_image = XShmCreateImage(m_display, visual, static_cast<unsigned int>(depth), ZPixmap,
                                  nullptr, &m_shm_info, static_cast<unsigned int>(m_width),
                                  static_cast<unsigned int>(m_height));
        if (m_image == nullptr) {
            if (error != nullptr) {
                *error = "XShmCreateImage failed";
            }
            return false;
        }
        const std::size_t bytes = static_cast<std::size_t>(m_image->bytes_per_line) *
                                  static_cast<std::size_t>(m_image->height);
        m_shm_info.shmid = shmget(IPC_PRIVATE, bytes, IPC_CREAT | 0600);
        if (m_shm_info.shmid < 0) {
            if (error != nullptr) {
                *error = "shmget failed";
            }
            XDestroyImage(m_image);
            m_image = nullptr;
            return false;
        }
        void* address = shmat(m_shm_info.shmid, nullptr, 0);
        if (address == reinterpret_cast<void*>(-1)) {
            if (error != nullptr) {
                *error = "shmat failed";
            }
            shmctl(m_shm_info.shmid, IPC_RMID, nullptr);
            XDestroyImage(m_image);
            m_image = nullptr;
            return false;
        }
        m_shm_info.shmaddr = static_cast<char*>(address);
        m_image->data = static_cast<char*>(address);
        m_shm_info.readOnly = False;
        if (!XShmAttach(m_display, &m_shm_info)) {
            if (error != nullptr) {
                *error = "XShmAttach failed";
            }
            shmdt(m_shm_info.shmaddr);
            shmctl(m_shm_info.shmid, IPC_RMID, nullptr);
            XDestroyImage(m_image);
            m_image = nullptr;
            return false;
        }
        XSync(m_display, False);
        m_shm = true;
        return true;
    }

    void CloseShm() {
        if (m_image != nullptr) {
            if (m_shm) {
                XShmDetach(m_display, &m_shm_info);
            }
            XDestroyImage(m_image);
            m_image = nullptr;
        }
        if (m_shm_info.shmaddr != nullptr) {
            shmdt(m_shm_info.shmaddr);
            m_shm_info.shmaddr = nullptr;
        }
        if (m_shm_info.shmid >= 0) {
            shmctl(m_shm_info.shmid, IPC_RMID, nullptr);
            m_shm_info.shmid = -1;
        }
        m_shm = false;
        m_last_borrowed = false;
    }

    void Close() {
        CloseShm();
        if (m_display != nullptr) {
            XCloseDisplay(m_display);
            m_display = nullptr;
        }
    }

    /// @brief 用当前 XShm 图像产出一帧（零拷贝借用或 owned 拷贝）。
    bool CaptureFromShm(VideoFrame* out) {
        if (m_zero_copy && IsFastBgraFormat(m_image) && BorrowShmImage(out)) {
            m_last_borrowed = true;
            return true;
        }
        return CopyImage(m_image, out);
    }

    /// @brief 共享段失效后的重建调度（1 秒冷却，避免每帧都尝试 attach）。
    void ScheduleShmRebuild() noexcept {
        m_shm_rebuild_pending = true;
        m_shm_retry_at = std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
    }

    /// @brief 冷却到期后尝试重建 XShm；失败则再等 1 秒。
    void TryRebuildShm() {
        if (m_image != nullptr || !m_use_shm || !m_shm_rebuild_pending) {
            return;
        }
        if (std::chrono::steady_clock::now() < m_shm_retry_at) {
            return;
        }
        std::string ignored;
        if (InitShm(&ignored)) {
            m_shm_rebuild_pending = false;
        } else {
            m_shm_retry_at = std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
        }
    }

    /// @brief 零拷贝：XShm 段直接借给帧，只原地填 alpha（不搬整帧）。
    /// @return 成功返回 true；共享段不可用时返回 false（由调用方回退拷贝路径）。
    bool BorrowShmImage(VideoFrame* out) noexcept {
        if (m_image == nullptr || m_image->data == nullptr) {
            return false;
        }
        const auto* base = reinterpret_cast<const std::uint8_t*>(m_image->data);
        out->width = m_width;
        out->height = m_height;
        out->format = PixelFormat::kBGRA;
        out->Borrow(base, m_image->bytes_per_line);
        if (m_fill_alpha) {
            // 原地改共享段的第 4 字节：与 owned 路径的 BGRA 逐字节一致，
            // 但只写 1/4 字节且不产生第二块缓冲（仍是零拷贝视图）。
            auto* row = const_cast<std::uint8_t*>(out->Bytes());
            const std::size_t stride = static_cast<std::size_t>(out->stride);
            for (int y = 0; y < m_height; ++y) {
                detail::FillAlphaRowSimd(row + static_cast<std::size_t>(y) * stride, m_width);
            }
        }
        return true;
    }

    /// @brief XImage → VideoFrame（BGRA），SSE2 行拷贝 + alpha 填充。
    bool CopyImage(XImage* image, VideoFrame* out) {
        if (image == nullptr) {
            return false;
        }
        out->width = m_width;
        out->height = m_height;
        out->format = PixelFormat::kBGRA;
        out->stride = m_width * 4;
        out->data.resize(static_cast<std::size_t>(m_width) * static_cast<std::size_t>(m_height) * 4);
        std::uint8_t* dst = out->data.data();
        const auto* base = reinterpret_cast<const std::uint8_t*>(image->data);
        if (IsFastBgraFormat(image)) {
            const std::size_t src_stride = static_cast<std::size_t>(image->bytes_per_line);
            const std::size_t dst_stride = static_cast<std::size_t>(out->stride);
            for (int y = 0; y < m_height; ++y) {
                detail::CopyBgraRowSimd(dst + static_cast<std::size_t>(y) * dst_stride,
                                        base + static_cast<std::size_t>(y) * src_stride, m_width);
            }
            return true;
        }
        for (int y = 0; y < m_height; ++y) {
            std::uint8_t* row = dst + static_cast<std::size_t>(y) * static_cast<std::size_t>(out->stride);
            for (int x = 0; x < m_width; ++x) {
                const unsigned long pixel = XGetPixel(image, x, y);
                row[x * 4 + 0] = static_cast<std::uint8_t>((pixel & image->blue_mask) >> 0);
                row[x * 4 + 1] = static_cast<std::uint8_t>((pixel & image->green_mask) >> 8);
                row[x * 4 + 2] = static_cast<std::uint8_t>((pixel & image->red_mask) >> 16);
                row[x * 4 + 3] = 255;
            }
        }
        return true;
    }

    Display* m_display = nullptr;
    int m_screen = 0;
    Window m_target = 0;
    int m_x = 0;
    int m_y = 0;
    int m_width = 0;
    int m_height = 0;
    bool m_shm = false;
    bool m_zero_copy = false;
    bool m_fill_alpha = true;
    bool m_last_borrowed = false;
    bool m_use_shm = true;
    bool m_shm_rebuild_pending = false;
    std::chrono::steady_clock::time_point m_shm_retry_at{};
    XImage* m_image = nullptr;
    XShmSegmentInfo m_shm_info{};
};

}  // namespace

bool X11Available() {
    Display* display = XOpenDisplay(nullptr);
    if (display == nullptr) {
        return false;
    }
    XCloseDisplay(display);
    return true;
}

std::unique_ptr<VideoSource> CreateX11Capture(const X11CaptureConfig& config, std::string* error) {
    auto capture = std::make_unique<X11CaptureImpl>();
    if (!capture->Init(config, error)) {
        return nullptr;
    }
    return capture;
}

}  // namespace go2cpp::media
