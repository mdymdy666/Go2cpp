/**
 * @file render.cpp
 * @brief 渲染实现：SDL2 窗口（WSLg/X11）+ 无头校验渲染器。
 * @details 无头渲染器不依赖任何图形库，CI 与「只验证像素正确性」的端到端测试
 *          走这条路径；SDL 渲染器在 WSLg 下创建真实窗口，用 `SDL_UpdateTexture`
 *          上传 BGRA，`SDL_RENDERER_ACCELERATED` 失败时自动回退 software。
 *          BGRA 帧**直传**纹理（`frame.Bytes()` + `EffectiveStride()`），不复制到
 *          中间帧、不写 PPM；I420 输入才做一次格式转换（目标缓冲复用）。
 *          `Readback()` 用 `SDL_RenderReadPixels` 回读窗口内容，供像素级验证。
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include "go2cpp/media/render.hpp"

#include <SDL.h>

#include <cstring>
#include <string>

#include "go2cpp/media/pixel.hpp"

namespace go2cpp::media {
namespace {

/// @brief SDL 视频子系统的引用计数（多渲染器共享一次 Init）。
int g_sdl_refs = 0;

bool AcquireSdl(std::string* error) {
    if (g_sdl_refs == 0) {
        if (SDL_Init(SDL_INIT_VIDEO) != 0) {
            if (error != nullptr) {
                *error = std::string("SDL_Init(SDL_INIT_VIDEO) failed: ") + SDL_GetError();
            }
            return false;
        }
    }
    ++g_sdl_refs;
    return true;
}

void ReleaseSdl() {
    if (g_sdl_refs > 0) {
        --g_sdl_refs;
        if (g_sdl_refs == 0) {
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
        }
    }
}

/// @brief 无头渲染器：只做计数与校验和。
class HeadlessRenderer final : public FrameRenderer {
   public:
    bool Present(const VideoFrame& frame) override {
        if (!frame.Valid()) {
            return false;
        }
        m_last_checksum = frame.Checksum();
        ++m_presented;
        return true;
    }

    const char* name() const noexcept override {
        return "headless";
    }
    bool visible() const noexcept override {
        return false;
    }
    std::uint64_t presented() const noexcept override {
        return m_presented;
    }
    std::uint64_t last_checksum() const noexcept override {
        return m_last_checksum;
    }

   private:
    std::uint64_t m_presented = 0;
    std::uint64_t m_last_checksum = 0;
};

/// @brief SDL2 渲染器。
class SdlRenderer final : public FrameRenderer {
   public:
    ~SdlRenderer() override {
        if (m_texture != nullptr) {
            SDL_DestroyTexture(m_texture);
        }
        if (m_renderer != nullptr) {
            SDL_DestroyRenderer(m_renderer);
        }
        if (m_window != nullptr) {
            SDL_DestroyWindow(m_window);
        }
        if (m_sdl_owned) {
            ReleaseSdl();
        }
    }

    bool Init(int width, int height, bool headless, std::string* error) {
        if (!headless) {
            if (!AcquireSdl(error)) {
                return false;
            }
            m_sdl_owned = true;
            m_window = SDL_CreateWindow("go2cpp media (M1 loopback)", SDL_WINDOWPOS_CENTERED,
                                        SDL_WINDOWPOS_CENTERED, width, height, SDL_WINDOW_SHOWN);
            if (m_window == nullptr) {
                if (error != nullptr) {
                    *error = std::string("SDL_CreateWindow failed: ") + SDL_GetError();
                }
                return false;
            }
            m_renderer = SDL_CreateRenderer(m_window, -1, SDL_RENDERER_ACCELERATED);
            if (m_renderer == nullptr) {
                m_renderer = SDL_CreateRenderer(m_window, -1, SDL_RENDERER_SOFTWARE);
            }
            if (m_renderer == nullptr) {
                if (error != nullptr) {
                    *error = std::string("SDL_CreateRenderer failed: ") + SDL_GetError();
                }
                return false;
            }
            m_visible = true;
        }
        return EnsureTexture(width, height);
    }

    bool Present(const VideoFrame& frame) override {
        if (!frame.Valid()) {
            return false;
        }
        m_last_checksum = frame.Checksum();
        m_zero_copy = false;
        if (!m_visible) {
            ++m_presented;
            return true;
        }
        if (!EnsureTexture(frame.width, frame.height)) {
            return false;
        }
        const std::uint8_t* pixels = nullptr;
        int stride = 0;
        if (frame.format == PixelFormat::kI420) {
            // I420 → BGRA 是格式转换，不可避免；转换缓冲复用成员，不重新分配。
            if (m_bgra.width != frame.width || m_bgra.height != frame.height ||
                m_bgra.format != PixelFormat::kBGRA ||
                m_bgra.data.size() < BgraFrameBytes(frame.width, frame.height)) {
                m_bgra.width = frame.width;
                m_bgra.height = frame.height;
                m_bgra.format = PixelFormat::kBGRA;
                m_bgra.stride = frame.width * 4;
                m_bgra.data.resize(BgraFrameBytes(frame.width, frame.height));
            }
            I420ToBgra(frame.Bytes(), frame.width, frame.height, m_bgra.data.data(), m_bgra.stride);
            pixels = m_bgra.data.data();
            stride = m_bgra.stride;
        } else {
            // 直传：BGRA 帧的像素指针/stride 直接交给 SDL，不做中间拷贝。
            pixels = frame.Bytes();
            stride = frame.EffectiveStride();
            m_zero_copy = true;
        }
        if (SDL_UpdateTexture(m_texture, nullptr, pixels, stride) != 0) {
            return false;
        }
        SDL_RenderClear(m_renderer);
        SDL_RenderCopy(m_renderer, m_texture, nullptr, nullptr);
        SDL_RenderPresent(m_renderer);
        // 处理窗口关闭：不阻塞，只 peek。
        SDL_Event event;
        while (SDL_PollEvent(&event) != 0) {
            if (event.type == SDL_QUIT) {
                m_visible = false;
            }
        }
        ++m_presented;
        return true;
    }

    bool Readback(VideoFrame* out) override {
        if (out == nullptr || !m_visible || m_renderer == nullptr || m_texture == nullptr) {
            return false;
        }
        const int width = m_texture_width;
        const int height = m_texture_height;
        if (width <= 0 || height <= 0) {
            return false;
        }
        out->width = width;
        out->height = height;
        out->format = PixelFormat::kBGRA;
        out->stride = width * 4;
        out->data.resize(BgraFrameBytes(width, height));
        // ARGB8888 在小端的内存布局就是 B,G,R,A，与我们的 BGRA 帧逐字节对应。
        return SDL_RenderReadPixels(m_renderer, nullptr, SDL_PIXELFORMAT_ARGB8888,
                                    out->data.data(), out->stride) == 0;
    }

    const char* name() const noexcept override {
        return m_visible ? "sdl-window" : "sdl-headless";
    }
    bool visible() const noexcept override {
        return m_visible;
    }
    std::uint64_t presented() const noexcept override {
        return m_presented;
    }
    std::uint64_t last_checksum() const noexcept override {
        return m_last_checksum;
    }
    bool last_present_zero_copy() const noexcept override {
        return m_zero_copy;
    }

   private:
    bool EnsureTexture(int width, int height) {
        if (m_texture != nullptr && m_texture_width == width && m_texture_height == height) {
            return true;
        }
        if (m_texture != nullptr) {
            SDL_DestroyTexture(m_texture);
            m_texture = nullptr;
        }
        if (!m_visible) {
            m_texture_width = width;
            m_texture_height = height;
            return true;
        }
        m_texture = SDL_CreateTexture(m_renderer, SDL_PIXELFORMAT_ARGB8888,
                                      SDL_TEXTUREACCESS_STREAMING, width, height);
        if (m_texture == nullptr) {
            return false;
        }
        m_texture_width = width;
        m_texture_height = height;
        return true;
    }

    SDL_Window* m_window = nullptr;
    SDL_Renderer* m_renderer = nullptr;
    SDL_Texture* m_texture = nullptr;
    int m_texture_width = 0;
    int m_texture_height = 0;
    bool m_visible = false;
    bool m_sdl_owned = false;
    bool m_zero_copy = false;
    VideoFrame m_bgra;
    std::uint64_t m_presented = 0;
    std::uint64_t m_last_checksum = 0;
};

}  // namespace

bool SdlAvailable() {
    std::string error;
    if (!AcquireSdl(&error)) {
        return false;
    }
    ReleaseSdl();
    return true;
}

std::unique_ptr<FrameRenderer> CreateSdlRenderer(int width, int height, bool headless,
                                                 std::string* error) {
    auto renderer = std::make_unique<SdlRenderer>();
    if (!renderer->Init(width, height, headless, error)) {
        return nullptr;
    }
    return renderer;
}

std::unique_ptr<FrameRenderer> CreateHeadlessRenderer() {
    return std::make_unique<HeadlessRenderer>();
}

}  // namespace go2cpp::media
