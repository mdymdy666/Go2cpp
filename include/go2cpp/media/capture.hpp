#pragma once

/**
 * @file capture.hpp
 * @brief 采集层：X11（XShm / XGetImage）+ 合成画面源。
 * @details WSL2 的 WSLg 提供完整 X11（MIT-SHM 实测存在），因此采集走：
 *
 *          1. `XOpenDisplay(nullptr)` 打开 `$DISPLAY`（WSLg 里是 `:0`）；
 *          2. `XShmQueryExtension` 判定共享内存扩展；可用则 `XShmCreateImage`
 *             + `shmget/shmat` + `XShmAttach`，每帧 `XShmGetImage`（服务器直接
 *             写共享段，无 socket 往返）；
 *          3. 扩展不可用时回退 `XGetImage`（走 X11 协议，慢但总能跑）。
 *
 *          **零拷贝**：XShm 路径默认仍把共享段拷进 `VideoFrame::data`（历史行为，
 *          兼容 `codec.cpp` 等直接读 `frame.data` 的调用方）。打开
 *          `X11CaptureConfig::zero_copy` 后，采集帧改为 `VideoFrame::Borrow()`
 *          的**借用视图**，直接指向 XShm 段，只把 alpha 字节原地填 0xFF；整帧
 *          memcpy 消失。借用视图的像素**只在下一次 `Capture()` 之前有效**，调用方
 *          必须在这之前用 `Bytes()` 消费（不能长期持有，也不能跨线程读）。
 *
 *          采集结果统一转成 **BGRA**（X11 depth-24 小端 ZPixmap 的内存布局是
 *          B,G,R,X，把 X 通道填 0xFF 得到 BGRA，后续转换按 B/G/R 取）。
 *
 *          `SyntheticCapture` 不依赖图形环境，用于无头 CI 与差分编码的可重复
 *          输入（静止画面/滚动条/局部矩形/噪声 四种图案）。
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include <cstdint>
#include <memory>
#include <string>

#include "go2cpp/media/frame.hpp"

namespace go2cpp::media {

/// @brief 视频帧源抽象。
class VideoSource {
   public:
    virtual ~VideoSource() = default;

    VideoSource(const VideoSource&) = delete;
    VideoSource& operator=(const VideoSource&) = delete;
    VideoSource(VideoSource&&) = delete;
    VideoSource& operator=(VideoSource&&) = delete;

    /// @brief 采集一帧到 `out`（覆盖，BGRA）。
    /// @return 成功返回 true；X11 断开/窗口越界返回 false。
    virtual bool Capture(VideoFrame* out) = 0;

    /// @brief 源宽度（像素）。
    virtual int width() const noexcept = 0;

    /// @brief 源高度（像素）。
    virtual int height() const noexcept = 0;

    /// @brief 可读名字。
    virtual const char* name() const noexcept = 0;

    /// @brief 是否真的连上了图形后端（合成源恒 true）。
    virtual bool available() const noexcept = 0;

    /// @brief 最近一次 `Capture()` 是否返回零拷贝视图（借用外部缓冲）。
    /// @details 默认 false（自有内存）。仅用于验证/统计：true 时帧的 `data` 为空，
    ///          像素由 `Bytes()` 指向源缓冲，生命周期到下一次 `Capture()`。
    virtual bool last_capture_borrowed() const noexcept {
        return false;
    }

   protected:
    VideoSource() = default;
};

/// @brief X11 采集配置。
struct X11CaptureConfig {
    int width = 640;   ///< 抓取宽度；<=0 表示用屏幕全宽（窗口模式下用窗口宽度）。
    int height = 480;  ///< 抓取高度；<=0 表示用屏幕全高（窗口模式下用窗口高度）。
    int x = 0;         ///< 抓取起点 X（相对抓取目标）。
    int y = 0;         ///< 抓取起点 Y（相对抓取目标）。
    bool use_shm = true;  ///< 优先 XShm；测试里可关掉以覆盖 XGetImage 路径。
    /// @brief 零拷贝：XShm 段直接借给 `VideoFrame`，不 memcpy 到自有缓冲。
    /// @details 默认 false 保持历史 API 语义（帧拥有像素、可长期持有）。开启后：
    ///          - `VideoFrame::data` 为空、`borrowed` 指向 XShm 段；
    ///          - 帧只在下一次 `Capture()` 前有效，调用方必须用 `Bytes()` 读；
    ///          - `use_shm=true` 且 XShm 初始化成功时才生效；回退 `XGetImage`
    ///            的路径必须拷贝（`XDestroyImage` 会释放图像），此时仍是自有帧。
    bool zero_copy = false;
    /// @brief 零拷贝模式下是否原地把 alpha 填 0xFF（默认 true）。
    /// @details XShm 段里第 4 字节是 X11 的填充字节，不是 alpha。填 0xFF 得到
    ///          与 owned 路径逐字节一致的 BGRA；关掉可省掉这一遍原地写（下游若
    ///          只用 B/G/R，例如 BGRA→I420 编码，可以关）。
    bool fill_alpha_in_place = true;
    /// @brief 抓取目标窗口（0 = root window）。
    /// @details 用于抓自建窗口（测试/合成画面），避免依赖 WSLg root 画面；
    ///          XShmGetImage 对 InputOutput 窗口是合法 drawable。
    unsigned long target_window = 0;
};

/// @brief 创建 X11 采集源。
/// @param error 失败原因（可为 nullptr）。
/// @return 失败返回 nullptr（调用方应回退到合成源或直接 SKIP）。
std::unique_ptr<VideoSource> CreateX11Capture(const X11CaptureConfig& config,
                                             std::string* error);

/// @brief 无头环境下 X11 是否可用（只探测 `XOpenDisplay` + 扩展）。
bool X11Available();

/// @brief 合成画面图案。
enum class SyntheticPattern : std::uint8_t {
    kStatic = 0,   ///< 静止画面（首帧之后完全不变，差分编码应接近 0 字节）。
    kScroll = 1,   ///< 水平滚动条 + 计数文本块，模拟光标/时间戳刷新。
    kPatch = 2,    ///< 单块矩形在画面内移动，模拟窗口拖动。
    kNoise = 3,    ///< 全画面伪随机噪声（不可压缩，最坏情况）。
};

/// @brief 合成采集源（确定性：同一帧号产生同一画面，便于逐帧校验）。
class SyntheticCapture final : public VideoSource {
   public:
    SyntheticCapture(int width, int height, SyntheticPattern pattern);

    bool Capture(VideoFrame* out) override;
    int width() const noexcept override {
        return m_width;
    }
    int height() const noexcept override {
        return m_height;
    }
    const char* name() const noexcept override {
        return "synthetic";
    }
    bool available() const noexcept override {
        return true;
    }

    /// @brief 当前帧号（下一次 Capture 使用后自增）。
    std::uint32_t frame_id() const noexcept {
        return m_frame_id;
    }

   private:
    int m_width;
    int m_height;
    SyntheticPattern m_pattern;
    std::uint32_t m_frame_id = 0;
    std::uint32_t m_seed = 0x12345678U;
};

}  // namespace go2cpp::media
