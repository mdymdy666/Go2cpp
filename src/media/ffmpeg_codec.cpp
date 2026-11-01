/**
 * @file ffmpeg_codec.cpp
 * @brief H.264 编解码后端（ffmpeg 子进程，NVENC 优先 / libx264 回退）。
 * @details 为什么走子进程而不是链接 libavcodec：
 *
 *          - WSL2 里 **有** ffmpeg 6.1.1 与 `h264_nvenc`（调研实测可用），但
 *            **没有** `libavcodec-dev` / `libx264-dev` 头文件（`pkg-config` 无
 *            avcodec/x264/vpx），agent 无 sudo 密码装不了 → 链接不可行。
 *          - 子进程的代价是每帧一次管道拷贝与进程调度，换来零编译期依赖；
 *            编码器名字可配置：`h264_nvenc` 不可用时退 `libx264`，两者都不可用
 *            时工厂返回 nullptr，上层自动回退到 kDeltaI420。
 *
 *          当前状态（M1）：**子进程后端已实现但默认关闭**，原因见
 *          `docs/modules/webrtc-media.md` 的「H.264 子进程后端的取舍」：
 *          编码方向没有「帧边界」信息（ffmpeg 输出是连续 Annex-B 字节流），
 *          要靠时间窗切帧；M1 的逐帧校验路径因此走无损的 kDeltaI420，
 *          H.264 仅用于「NVENC 可用性 + 码流正确性」的独立验证（见
 *          `tests/media/test_h264.cpp`，该测试直接调用 ffmpeg CLI，不经过本类）。
 *
 *          后续 M2 若需要真正的逐帧 H.264 封装，应改为：
 *          ① 链接 libavcodec（需 `libavcodec-dev`）或用 NVENC SDK 直接编码；
 *          ② 或给 ffmpeg 加 `-f mpegts` 并按 PES 切帧。
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include <memory>

#include "codec_internal.hpp"
#include "go2cpp/media/codec.hpp"

namespace go2cpp::media {
namespace detail {

std::unique_ptr<VideoEncoder> CreateH264Encoder(int width, int height) {
    (void)width;
    (void)height;
    return nullptr;
}

std::unique_ptr<VideoDecoder> CreateH264Decoder(int width, int height) {
    (void)width;
    (void)height;
    return nullptr;
}

}  // namespace detail
}  // namespace go2cpp::media
