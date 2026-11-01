#pragma once

/**
 * @file render.hpp
 * @brief 渲染层：SDL2 窗口（WSLg X11 后端）+ 无头校验渲染器。
 * @details WSL2 的 WSLg 提供 X11 与 OpenGL（GLX），SDL2 的 x11 视频后端实测可用；
 *          但采集与渲染同时开窗口会让「采集源」和「渲染目标」互相干扰（抓到自己），
 *          因此 M1 的默认验证路径是 **无头渲染器**（逐帧校验），窗口渲染作为可选项
 *          由 `--render` 打开。
 *
 *          `SdlRenderer` 用 `SDL_CreateTexture(SDL_PIXELFORMAT_ARGB8888)` 上传 BGRA
 *          （小端下 ARGB8888 的内存布局就是 B,G,R,A），一次 `RenderCopy` + `Present`。
 *          `SDL_VIDEODRIVER=dummy` 时窗口不可见但接口仍可用，便于 CI 覆盖。
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include <cstdint>
#include <memory>
#include <string>

#include "go2cpp/media/frame.hpp"

namespace go2cpp::media {

/// @brief 帧渲染器抽象。
class FrameRenderer {
   public:
    virtual ~FrameRenderer() = default;

    FrameRenderer(const FrameRenderer&) = delete;
    FrameRenderer& operator=(const FrameRenderer&) = delete;
    FrameRenderer(FrameRenderer&&) = delete;
    FrameRenderer& operator=(FrameRenderer&&) = delete;

    /// @brief 呈现一帧（BGRA 或 I420）。
    /// @return 成功返回 true；窗口关闭/尺寸不支持返回 false。
    virtual bool Present(const VideoFrame& frame) = 0;

    /// @brief 可读名字。
    virtual const char* name() const noexcept = 0;

    /// @brief 是否真的创建了可视窗口。
    virtual bool visible() const noexcept = 0;

    /// @brief 已呈现帧数。
    virtual std::uint64_t presented() const noexcept = 0;

    /// @brief 已呈现帧的内容校验和（用于断言「渲染器收到了正确的帧」）。
    virtual std::uint64_t last_checksum() const noexcept = 0;

   protected:
    FrameRenderer() = default;
};

/// @brief 创建 SDL2 渲染器。
/// @param headless true 时不创建窗口，只统计（等价 SDL_VIDEODRIVER=dummy 但不依赖 SDL）。
/// @param error 失败原因（可为 nullptr）。
std::unique_ptr<FrameRenderer> CreateSdlRenderer(int width, int height, bool headless,
                                                 std::string* error);

/// @brief 创建无头渲染器（不依赖任何图形库，只做校验与计数）。
std::unique_ptr<FrameRenderer> CreateHeadlessRenderer();

/// @brief SDL2 是否可用（编译期可用且能初始化 video 子系统）。
bool SdlAvailable();

}  // namespace go2cpp::media
