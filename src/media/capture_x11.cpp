/**
 * @file capture_x11.cpp
 * @brief X11 采集：XShm 共享内存优先，回退 XGetImage。
 * @details WSL2 的 WSLg 提供完整 X11（`$DISPLAY=:0`），`MIT-SHM` 扩展实测存在，
 *          因此 `XShmGetImage` 是默认路径：图像数据落在 `shmget` 分配的共享段里，
 *          X 服务器直接写这段内存，客户端只需一次 `memcpy` 转 BGRA。
 *
 *          回退链：`XShmQueryExtension` 失败 / `shmget` 失败 / `XShmAttach` 失败
 *          → 使用 `XGetImage`（走 X11 请求，慢但通用）。
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
#include <cstring>
#include <string>

#include "go2cpp/media/capture.hpp"

namespace go2cpp::media {
namespace {

/// @brief 忽略 X11 异步错误，避免默认处理器直接 exit(1)。
/// @details `XGetImage` 在区域越界/服务器不支持时会产生 BadMatch 异步错误；
///          默认处理器会打印并退出进程。采集失败应由 `Capture()` 的返回值表达，
///          因此这里吞掉错误码（调用方仍能通过返回值判断失败）。
int IgnoreXError(Display*, XErrorEvent*) noexcept {
    return 0;
}

/// @brief 把 XImage 的一行按 32bpp/24bpp 小端快速转换成 BGRA。
void CopyRow32(const std::uint8_t* src, std::uint8_t* dst, int width) noexcept {
    for (int x = 0; x < width; ++x) {
        dst[x * 4 + 0] = src[x * 4 + 0];
        dst[x * 4 + 1] = src[x * 4 + 1];
        dst[x * 4 + 2] = src[x * 4 + 2];
        dst[x * 4 + 3] = 255;
    }
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
        m_root = RootWindow(m_display, m_screen);
        const int screen_width = DisplayWidth(m_display, m_screen);
        const int screen_height = DisplayHeight(m_display, m_screen);
        // 抓取区域裁剪到屏幕内，避免 XShmGetImage 越界返回 BadMatch。
        m_x = std::max(0, std::min(config.x, screen_width - 1));
        m_y = std::max(0, std::min(config.y, screen_height - 1));
        m_width = config.width > 0 ? config.width : (screen_width - m_x);
        m_height = config.height > 0 ? config.height : (screen_height - m_y);
        m_width = std::max(1, std::min(m_width, screen_width - m_x));
        m_height = std::max(1, std::min(m_height, screen_height - m_y));

        if (config.use_shm && InitShm(error)) {
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
        if (m_image != nullptr) {
            if (!XShmGetImage(m_display, m_root, m_image, m_x, m_y, AllPlanes)) {
                // 共享段失效（例如服务器重启）→ 关闭后改走 XGetImage。
                CloseShm();
            } else if (CopyImage(m_image, out)) {
                return true;
            }
        }
        XImage* image = XGetImage(m_display, m_root, m_x, m_y, static_cast<unsigned int>(m_width),
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
    }

    void Close() {
        CloseShm();
        if (m_display != nullptr) {
            XCloseDisplay(m_display);
            m_display = nullptr;
        }
    }

    /// @brief XImage → VideoFrame（BGRA）。
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
        const bool fast = image->bits_per_pixel == 32 && image->byte_order == LSBFirst &&
                          image->red_mask == 0x00FF0000U && image->green_mask == 0x0000FF00U &&
                          image->blue_mask == 0x000000FFU;
        if (fast) {
            for (int y = 0; y < m_height; ++y) {
                const std::uint8_t* src = reinterpret_cast<const std::uint8_t*>(image->data) +
                                          static_cast<std::size_t>(y) *
                                              static_cast<std::size_t>(image->bytes_per_line);
                CopyRow32(src, dst + static_cast<std::size_t>(y) * static_cast<std::size_t>(out->stride),
                          m_width);
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
    Window m_root = 0;
    int m_x = 0;
    int m_y = 0;
    int m_width = 0;
    int m_height = 0;
    bool m_shm = false;
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
