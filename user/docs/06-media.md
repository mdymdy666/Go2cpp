# 媒体（交接文档）

> **定位**：`go2cpp::media` 实时媒体链路——采集、编码、1200B 分片传输、抖动缓冲、弱网降级、
> SFU 转发、SDP 信令、渲染、音频、MJPEG/AVI 录制。是「能跑通且可逐帧量化」的自研链路，
> **不是 WebRTC 实现**（无 RTP/RTCP、DTLS/SRTP、ICE）。
> **源码**：`include/go2cpp/media/`、`src/media/`　**构建**：`cmake/Go2CppMedia.cmake`（`-DGO2CPP_WITH_MEDIA=ON`，默认 OFF）
> **测试**：`tests/media/`（17 个文件，ctest 名字前缀 `media_`）
> **设计全文**：[webrtc-media.md](../../docs/modules/webrtc-media.md)、[media-audio.md](../../docs/modules/media-audio.md)、
> [jitter-degradation.md](../../docs/modules/jitter-degradation.md)、[sfu-signaling.md](../../docs/modules/sfu-signaling.md)

## 1. 这个模块是什么

一句话：**把一块屏幕和一个麦克风，变成一串带 32 字节头的字节块，经 UDP 分片送到对端并还原成
画面和声音**，中间可选做包级重排、弱网降档、一对多转发与 SDP 协商。

链路四段（视频）：采集 → 编码 → 传输 → 渲染。在此之上叠了三层可选能力：

| 能力 | 头文件 | 解决的问题 | 默认 |
|---|---|---|---|
| 抖动缓冲 | `jitter_buffer.hpp` | UDP 乱序/丢包直接打断差分参考帧，需按包序号重排并在有界窗口内等空洞 | 关（`MediaConfig::jitter`） |
| 弱网降级 | `degradation.hpp` | 带宽掉下去时先降帧率再降分辨率，回来分档回升 | 关（`MediaConfig::degradation`） |
| SFU / 信令 | `sfu.hpp`、`signaling.hpp` | 一对多转发（不解码、不重编码）与 offer/answer + 候选交换 | 独立类，不进管线 |

音频是并行的另一条链（`audio.hpp`）：采集 → Opus/G.711/PCM → RTP 风格分片 → 抖动缓冲 →
解码（含 PLC）→ 播放；录制侧 `avi.hpp` 只做封装，把**已编码的 JPEG 帧**写成 MJPEG AVI（不含 JPEG
编码器）。全部**单线程 pump**，模块内部不起线程、不加锁——跨线程使用必须调用方加锁。

## 2. 架构 / 数据流

### 2.1 视频链

```
  采集                          编码                        传输
┌──────────────────┐      ┌──────────────────┐      ┌────────────────────────┐
│ X11Capture       │      │ VideoEncoder     │      │ UdpVideoSender         │
│  XShm（零拷贝可选）│─BGRA→│  delta-i420(默认) │─字节→│  VideoChannel          │
│  SyntheticCapture│      │  raw-i420/raw-bgra│      │  ≤1200B/片，≤256 片/帧 │
└──────────────────┘      │  h264（子进程）   │      └───────────┬────────────┘
                          └──────────────────┘                  │ UDP 127.0.0.1
                                                                 ▼
  渲染                         解码                        接收/重排
┌──────────────────┐      ┌──────────────────┐      ┌────────────────────────┐
│ FrameRenderer    │←BGRA/│ VideoDecoder     │←整帧─│ UdpVideoReceiver       │
│  SDL2 / headless │ I420 │  差分需参考帧     │      │  乱序重组、丢旧帧、1s  │
└──────────────────┘      └──────────────────┘      │  缺片超时              │
                                                     │ JitterBuffer（可选）   │
                                                     │  512 槽环形、包级 seq  │
                                                     └────────────────────────┘
```

- 每帧前固定 32B `MediaHeader`（`G2MF` + codec + w/h + frame_id + payload_len + `capture_ns`）；
  `capture_ns` 是端到端延迟的唯一口径（采集完成 → 解码交付渲染）。
- `delta-i420` 是 16×16 宏块 + 行内 RLE 的**无损**差分：无脏块时整帧只有 36B；丢一帧会让后续
  差分帧内容错，所以每 `keyframe_interval`（默认 30）帧强制关键帧自愈。
- SFU 把「编码字节」换成「VideoChannel 分片」：`SfuPublisher` 出片，`ForwardFragment` 按订阅把
  **指针**转给 `SfuSubscriber`，全程不解码、不拷贝 payload。

### 2.2 音频链

```
AudioSource ─s16le─► AudioEncoder ─► AudioRtpPacketizer ─► UdpAudioSender
 (parec/arecord/合成)  (opus/g711/pcm)  12B RTP + 4B 分片    │ UDP
                                                            ▼
AudioSink ◄─ AudioDecoder(PLC) ◄─ AudioRtpReassembler ◄─ AudioJitterBuffer ◄─ UdpAudioReceiver
```

`AudioJitterBuffer`（`audio.hpp`，按包数配置）与 `JitterBuffer`（`jitter_buffer.hpp`，按纳秒配置）
是**两套独立实现**，别混用。

### 2.3 线程模型（交接重点）

| 组件 | 线程 | 说明 |
|---|---|---|
| `MediaSender` / `MediaReceiver` | 调用方线程 | `SendNext()` / `Pump(timeout_ms)` 各推进一步，内部零线程 |
| `UdpVideoReceiver` / `UdpAudioReceiver` | 调用方线程 | `poll` + 非阻塞 `recv`，单次处理 N 个包 |
| `SfuRouter` / `SfuPublisher` / `SfuSubscriber` | 调用方线程 | 转发是同步回调；回调里可退订（延迟回收） |
| `SignalingSession` | 调用方线程 | 纯状态机，`Tick()` 驱动超时 |
| X11 / `ffmpeg` / `parec` / `pacat` | 子进程 + 同步管道 | 读路径 `poll` + 总超时 |

「单线程」是 ns 级性能与无锁统计的前提，不是可选的风格。

## 3. 对外 API（关键接口）

### 3.1 帧与采集

```cpp
struct VideoFrame {
    int width, height; PixelFormat format; int stride;
    std::vector<std::uint8_t> data;  bool keyframe;
    const std::uint8_t* Bytes() const noexcept;              // 借用视图时指向外部缓冲
    void Borrow(const std::uint8_t* bytes, int row_stride = 0) noexcept;
    std::uint64_t Checksum() const noexcept;
};
struct X11CaptureConfig {
    int width = 640, height = 480, x = 0, y = 0;
    bool use_shm = true, zero_copy = false, fill_alpha_in_place = true;
    unsigned long target_window = 0;   // 0 = root
};
std::unique_ptr<VideoSource> CreateX11Capture(const X11CaptureConfig&, std::string* error);
```

`zero_copy=true` 时帧的 `data` 为空，像素只在**下一次 `Capture()` 之前**有效；默认关闭是因为
`codec.cpp` 仍直接读 `frame.data`（见 §7）。

### 3.2 编解码

```cpp
enum class CodecKind : std::uint8_t { kRawBgra, kRawI420, kDeltaI420, kH264 };
std::unique_ptr<VideoEncoder> CreateEncoder(CodecKind kind, int width, int height);
std::unique_ptr<VideoDecoder> CreateDecoder(CodecKind kind, int width, int height);
bool EncodeMediaHeader(const MediaHeader&, std::uint8_t* out) noexcept;
bool DecodeMediaHeader(const std::uint8_t* data, std::size_t bytes, MediaHeader*) noexcept;
CodecAvailability ProbeAvailability();   // 探测 h264/nvenc/x11/sdl，进程内只探测一次
```

`kDeltaI420` 用于桌面/文档类画面；`kRawI420` 是对照基线；`kH264` 仅在 ffmpeg 子进程可用时成立，
且没有逐帧封装（§7）。

### 3.3 传输

```cpp
bool Open(std::uint16_t peer_port, std::string* error);   // UdpVideoSender
bool SendFrame(std::uint32_t frame_id, bool key, const std::uint8_t* data, std::size_t bytes);
int  Pump(int timeout_ms, int max_packets);              // UdpVideoReceiver，返回交付帧数
void OnTimer(std::uint64_t now_ms);                     // 驱动 1s 缺片清理

static std::unique_ptr<MediaSender> Create(const MediaConfig&, std::uint16_t peer_port, std::string*);
bool SendNext(VideoFrame* captured, std::size_t* encoded_bytes);   // 采集+编码+分片
static std::unique_ptr<MediaReceiver> Create(const MediaConfig&, std::string*);
int  Pump(int timeout_ms);                              // 收包→重组→解码→渲染
std::uint16_t local_port() const noexcept;
```

### 3.4 抖动缓冲与降级

```cpp
PushResult Push(std::uint32_t seq, std::uint32_t timestamp, const std::uint8_t* payload,
                std::size_t bytes, std::int64_t arrival_ns) noexcept;   // 热路径零分配
PopResult  Pop(JitterPacketView* out, std::int64_t now_ns) noexcept;     // 零拷贝视图
PopResult  PopFlush(JitterPacketView* out, std::int64_t now_ns) noexcept;

DegradationEvent Update(std::int64_t now_ms, const NetworkSample& sample) noexcept;
bool ShouldDeliverFrame(std::int64_t now_ms) noexcept;   // 帧率门控
bool ConsumeKeyframeRequest() noexcept;                  // 切档/重连后强制关键帧

enum class QualityLevel : std::uint8_t { kFull, kReduced, kLow, kMinimal };
// kFull 320x240@30 → kReduced 320x240@15 → kLow 160x120@15 → kMinimal 160x120@10
```

`NetworkSample::FromBbr(const bento::BandwidthStats&)` 是带宽估计器接进媒体层的唯一入口；
`rtt_ms` 留 0（`BandwidthStats` 里没有平滑 RTT）。

### 3.5 SFU 与信令

```cpp
SfuStatus CreateRoom(const SfuRoomId& room);
SfuStatus Join(const SfuRoomId& room, SfuPeerId peer);
SfuStatus Publish(const SfuRoomId& room, SfuPeerId publisher, SfuStreamId stream);
SfuStatus Subscribe(const SfuRoomId& room, SfuPeerId subscriber, SfuPeerId publisher,
                    SfuStreamId stream, SfuFragmentSink sink);
std::size_t ForwardFragment(const SfuRoomId& room, SfuPeerId publisher, SfuStreamId stream,
                            const std::uint8_t* data, std::size_t bytes, std::uint8_t hops = 0);

std::optional<SessionDescription> CreateOffer(std::string* error = nullptr);
SignalingStatus SetRemoteAnswer(const SessionDescription& answer, std::string* error = nullptr);
SignalingStatus AddRemoteCandidate(const IceCandidate& candidate, std::string* error = nullptr);
SignalingStatus OnIceConnected(const IceCandidate& local, const IceCandidate& remote, std::string*);
SignalingState state() const noexcept;   // kIdle/kOffering/kAnswering/kConnected/kFailed/kClosed
```

`ForwardFragment` 的 `hops` 是级联跳数（普通发布传 0），超过 `max_cascade_hops` 直接丢弃并计入
`fragments_hop_limit`——这是唯一的防环手段。

### 3.6 音频、会话与录制

```cpp
static std::unique_ptr<AudioSession> Create(const AudioSessionConfig&, std::string* error);
bool SendNext(AudioFrame* captured_pcm);   // 采集+编码+分片+发送
int  Pump();                               // 收包→抖动→重组→解码→播放
int  Run(int frames);                      // 单线程跑 N 帧

bool Open(std::string path, const MjpegAviConfig& config, std::string* error);    // MjpegAviWriter
bool WriteFrame(const std::uint8_t* jpeg, std::size_t size, std::string* error);  // 只接受完整 JPEG
bool Finish(std::string* error);                                                  // 回填长度 + idx1
```

## 4. 最小用法（可编译示例）

发送端与接收端在同一进程跑 60 帧闭环（合成画面 + 差分编码 + UDP 回环）：

```cpp
#include "go2cpp/media/pipeline.hpp"
#include <iostream>

int main() {
    go2cpp::media::MediaConfig cfg;
    cfg.width = 320;  cfg.height = 240;
    cfg.codec = go2cpp::media::CodecKind::kDeltaI420;
    cfg.pattern = go2cpp::media::SyntheticPattern::kPatch;
    cfg.use_x11 = false;   // true 则走 X11Capture

    std::string err;
    auto receiver = go2cpp::media::MediaReceiver::Create(cfg, &err);   // 先建，拿端口
    if (!receiver) { std::cerr << "receiver: " << err << "\n"; return 1; }
    auto sender = go2cpp::media::MediaSender::Create(cfg, receiver->local_port(), &err);
    if (!sender) { std::cerr << "sender: " << err << "\n"; return 1; }

    int delivered = 0;
    for (int i = 0; i < 60; ++i) {
        if (!sender->SendNext(nullptr, nullptr)) break;   // captured/bytes 可空
        delivered += receiver->Pump(20);                  // 20ms 预算内收包→解码→渲染
    }
    std::cout << "sent=" << sender->frames_sent()
              << " delivered=" << receiver->frames_delivered() << "\n";
    return delivered > 0 ? 0 : 1;
}
```

链接：`target_link_libraries(app PRIVATE go2cpp::media)`。`peer_port` 取自 `local_port()`
（内核分配），所以必须先建接收端；真实窗口渲染把 `cfg.render = true`（需 `SdlAvailable()`）。

## 5. 源码位置

| 文件 | 行数 | 职责 |
|---|---:|---|
| `include/go2cpp/media/frame.hpp` / `src/media/frame.cpp` | 155 / 66 | `VideoFrame`、色彩格式、stride、校验和、借用视图 |
| `include/go2cpp/media/pixel.hpp` / `src/media/pixel.cpp` | 44 / 518 | BGRA↔I420（定点 BT.601）、PSNR/MAE、2:1 下采样 |
| `include/go2cpp/media/capture.hpp` / `capture_x11.cpp`、`capture_synthetic.cpp` | 150 / 376、149 | XShm/XGetImage 采集、SSE2 alpha 填充、四种确定性合成图案 |
| `include/go2cpp/media/codec.hpp` / `codec.cpp`、`codec_delta.cpp`、`ffmpeg_codec.cpp` | 186 / 525、501、49 | `MediaHeader` 线格式、raw/delta 编解码、ffmpeg 子进程骨架 |
| `include/go2cpp/media/transport.hpp` / `src/media/transport.cpp` | 199 / 303 | UDP 端点、`VideoChannel` 分片封装、进程内直连链路 |
| `include/go2cpp/media/render.hpp` / `src/media/render.cpp` | 90 / 277 | headless / SDL2 渲染器、纹理直传、`Readback()` |
| `include/go2cpp/media/stats.hpp` / `src/media/stats.cpp` | 106 / 83 | `LatencyRecorder` 分位数、`RateMeter` |
| `include/go2cpp/media/jitter_buffer.hpp` / `jitter_buffer.cpp` | 302 / 386 | 512 槽环形包缓冲、EWMA 抖动、空洞跳过、迟到补发 |
| `include/go2cpp/media/degradation.hpp` / `degradation.cpp` | 373 / 547 | 四档阶梯与滞回、帧率门控、`ReconnectSupervisor` 退避 |
| `include/go2cpp/media/pipeline.hpp` / `pipeline.cpp` | 293 / 517 | `MediaSender`/`MediaReceiver` 组装四段 + 可选 jitter/降级 |
| `include/go2cpp/media/sfu.hpp` / `src/media/sfu.cpp` | 336 / 943 | 房间/流/订阅索引、选择性转发、级联、延迟回收 |
| `include/go2cpp/media/signaling.hpp` / `signaling.cpp` | 454 / 1767 | SDP 子集序列化/解析、候选编解码、协商状态机 |
| `include/go2cpp/media/audio.hpp` / `src/media/audio.cpp` | 1026 / 2850 | 音频采集/编解码/PLC/RTP 分片/抖动/播放/会话 |
| `include/go2cpp/media/avi.hpp`（全 inline，无 .cpp） | 454 | MJPEG AVI 写入：RIFF/hdrl/movi/idx1、长度回填 |
| `cmake/Go2CppMedia.cmake` | — | 目标 `go2cpp_media` + 各 `media_test_*`（`GO2CPP_MEDIA_TESTS`） |

## 6. 性能数据

数字来自 `docs/reports/asm-*.md`，本机（WSL2 / Ryzen 9 8945HX / g++ 13.3 / `-O3`）实测，原样引用。

**采集与渲染**（[asm-media-capture-render.md](../../docs/reports/asm-media-capture-render.md)）：

| 项 | 数字 |
|---|---|
| XShm 零拷贝采集 1e5 帧 | p50 **0.137 ms**、2272.4 fps（owned 0.142 ms / 2244.2 fps → **0.965×**） |
| `X11CaptureImpl::Capture` | 954 ins / 栈帧 552 B → **437 ins / 88 B**（合计 735 ins，-23%） |
| SDL 直传渲染 1e5 帧 | p50 **0.291 ms**、2579.5 fps（旧拷贝 0.437 ms → **0.66×**）；`Present` 306→**227 ins** |
| 离下限 | 采集 p50 132 µs ≈ **10×** 内存下限，差距全在 `XShmGetImage` 的 X 往返 |

**音频**（[asm-media-audio.md](../../docs/reports/asm-media-audio.md)）：

| 项 | 数字 |
|---|---|
| Opus 包装 vs 裸 libopus | 48k 直通 **1.006×**（17,147.5 vs 17,050.0 ns/帧）、44.1k 含重采样 **1.037×** |
| µ-law 编码 AVX2 | **0.207 ns/样本**（4,830 M 样本/s），比查表 **5.54×** |
| 重采样 441→480 AVX2 | **272.0 ns/帧**（0.567 ns/样本），比标量 **3.15×**、比 `SRC_LINEAR` 快 **10.2×** |
| `OpusAudioEncoder::Encode` | 栈帧 1304 B → **8 B**、`rep stos` 1275 B → **0**、指令 224 → 207 |
| 正确性 | 409,205 checks；1e5 帧 checksum 与基线逐字节相同；Opus SNR 26.44/19.04/25.86 dB 不变 |

**SFU**（[asm-sfu.md](../../docs/reports/asm-sfu.md)）：`ForwardFragment` 1e6 帧裸转发 **16.5 ns/帧**
（60.47 M frames/s，2 订阅者），优化前 28.1 ns → **1.70×**；动态指令 276→263 Ir/帧，
`lock`/`malloc`/`@plt` 均为 **0**。

**信令**（[asm-signaling.md](../../docs/reports/asm-signaling.md)）：`ParseSdp` 中位 **6.66 µs**
（原 7.41 µs，**-10.2%**），自身 8,050 Ir/次、inclusive 134,600 Ir/次（分配主导），
估算下限 1.5–3 µs → 差 **2.2–4.4×**（控制面可接受）。

**抖动缓冲**（数据在 [jitter-degradation.md](../../docs/modules/jitter-degradation.md) §2.7，非 asm 报告）：
`Push`（1200B）**21.66 ns**（memcpy 15.00 ns 的 1.44×）、`Pop` 零拷贝 **3.13 ns**；1e6 帧后堆增长
1,216 B，内存常量 798,720 B。

**无 asm 级实测**：AVI/MJPEG 写入、PulseAudio/ALSA 采集与播放、`MediaSender`/`MediaReceiver` 整链。
整链只有 [webrtc-media.md](../../docs/modules/webrtc-media.md) §7 的回环端到端数字
（p50 0.16–0.71 ms / p99 0.36–1.19 ms、1027–2938 fps），那不是汇编级审计。

## 7. 边界与已知限制

1. **不是 WebRTC**：无 RTP/RTCP、DTLS/SRTP、ICE、GCC/REMB；NACK/FEC 默认关闭；无 simulcast/SVC。
   与浏览器**不能互通**，SDP 只是文本子集（`a=fingerprint` 只透传）。
2. **差分编码依赖参考帧**：丢一帧 → 后续帧内容错，靠每 30 帧强制关键帧自愈；中途订阅必须从
   关键帧首片开始（`require_keyframe_start`）。
3. **单帧 300KB 硬顶**：`VideoChannel` 256 片 × 1200B；640×480 的 I420（460800B）发不出去，
   大分辨率必须走差分或真编码器。
4. **WSL2 `rmem_max = 208KB`**：单帧突发 > ~300KB 会丢包（512×384 实测丢）；默认 320×240 是
   **按这条限制选的**，不是随手定的。
5. **零拷贝默认关闭**：`codec.cpp` 直接读 `frame.data`，开 `zero_copy` 后调用方必须改用 `Bytes()`
   且不能跨帧持有；XGetImage 回退路径无法零拷贝。
6. **h264 只到探测**：无 `libavcodec-dev`，`ffmpeg_codec.cpp` 是 fork/exec 骨架，NAL 边界对齐未做；
   验证走 `media_test_h264` 直接调 ffmpeg CLI。WSLg 下 `XGetImage` 抓 root 会 BadMatch（只能走 XShm）。
7. **音频**：无 AEC/NS/AGC、无 RTCP/NACK；4B 分片头是私有扩展，标准解码器需网关剥离；重采样是
   定点线性插值，高频有镜像（已知限制，非 bug）。
8. **单线程 + 无鉴权**：`Push`/`Pop`、`ForwardFragment`、`SignalingSession` 都不可并发调用；
   SFU 房间/发布/订阅无 ACL，候选可伪造，DTLS 未实现。

## 8. 测试与验证方法

```bash
cmake -S . -B .build-media -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DGO2CPP_WITH_BENTO=ON -DGO2CPP_WITH_MEDIA=ON -DGO2CPP_MEDIA_TESTS=ON
cmake --build .build-media -j"$(nproc)"
ctest --test-dir .build-media -R '^media_' --output-on-failure
```

| 测试 | 覆盖重点 |
|---|---|
| `media_test_frame` / `media_test_codec` | 尺寸推导、BGRA↔I420、`MediaHeader` 往返/拒绝、delta 逐帧无损、关键帧恢复 |
| `media_test_capture` | 合成四图案、XShm 零拷贝 vs owned 逐字节、标量 vs SSE2、区域裁剪 |
| `media_test_transport` | UDP 往返、乱序、缺片超时、进程内丢包注入、畸形包 |
| `media_test_jitter_buffer` / `media_test_degradation` | 2048 帧乱序、5% 丢包、±50ms 抖动 1e5 帧、1e6 压测；四档切换与滞回、重连退避 |
| `media_test_sfu` / `media_test_signaling` | 3000 步随机拓扑、1e6 帧吞吐；SDP 往返、500 次变异、6 个协商场景 |
| `media_test_audio` / `media_test_avi_mjpeg` | 1e5 帧逐字节、G.711 穷举、SIMD vs 标量、Opus PLC；AVI 结构与索引回填 |
| `media_test_e2e` / `media_test_media_e2e_jitter` | 端到端逐帧校验 + p50/p99 |
| `media_test_render` / `media_test_sdl_render` | headless/SDL 直传、采集→渲染→回读 0 差异 |

环境相关用例 SKIP 而非 FAIL（XGetImage 回退、SDL 缺失、ffmpeg 缺失）。观测入口：
`sender->frames_sent()/bytes_sent()`、`receiver->jitter_stats()`、`frames_skipped()`、
`router.stats()`、`session.snapshot()`。

## 9. 交接注意

- **先分清三条链**：视频（`pipeline`）、音频（`audio`）、SFU（`sfu`）各有自己的分片/重组实现，
  共同点只有 Bento `VideoChannel` 的 1200B 分片格式。改分片契约要三处一起改。
- **`capture_ns` 是唯一延迟口径**：新加计时统计别自造字段，直接用 `MediaHeader` 里的它。
- **改 `MediaHeader` 必须同时改**：`codec.hpp` 线格式注释、`Encode/DecodeMediaHeader`、对端解码、
  [webrtc-media.md](../../docs/modules/webrtc-media.md) §5.2。32B 是硬约定，
  `kMaxEncodedFrameBytes` 按它留了 64B 余量。
- **零拷贝生命周期最容易踩坑**：借用视图只活到下一次 `Capture()`，跨帧持有会读到被覆盖的像素
  （单测里 200 帧静止窗口不暴露这个问题）。
- **降级会重建采集源与编码器**：`RebuildForLevel()` 会强制关键帧；外部持有的 `encoder()` 引用
  在切档后失效。
- **SFU 回调里不要做重活**：`ForwardFragment` 在订阅者栈上同步调 sink，阻塞等于阻塞整个房间。
- **未完成事项**：DTLS/SRTP、真 H.264 逐帧封装、XDamage 驱动采集、DMA-BUF/EGL 直传、
  RTCP/NACK/PLI、simulcast、房间鉴权。逐条对照见各模块文档 §9/§10。
