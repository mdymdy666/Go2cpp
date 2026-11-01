# WebRTC 媒体端到端（X11 采集 → 编码 → 传输 → 渲染）

> 模块：`go2cpp::media`（`include/go2cpp/media/**` + `src/media/**` + `tests/media/**`）
> 定位：WebRTC 媒体链路的 **最小闭环（M0 + M1）**，不是完整的 WebRTC 实现。
> 构建：`cmake -S . -B .build-bento-wsl -DGO2CPP_WITH_BENTO=ON -DGO2CPP_WITH_MEDIA=ON`
> 环境：WSL2 / Ubuntu 24.04 / WSLg 1.0.66 / 12 vCPU / RTX 5060 Laptop / 2560x1600

---

## 1. 范围与结论

| 项 | 状态 | 说明 |
|---|---|---|
| M0 采集 + 编码 | ✅ | X11 XShm 采到 BGRA；I420 无损差分 + 原始帧 + H.264（ffmpeg/NVENC）三条编码路径 |
| M1 端到端闭环 | ✅ | 采集 → 编码 → VideoChannel 1200B 分片 → UDP 回环 → 重组 → 解码 → 渲染，100 帧逐帧校验通过 |
| 延迟（本机回环） | ✅ | p50 **0.16–0.71 ms**，p99 **0.36–1.19 ms**（含采集/编码/分片/UDP/重组/解码/渲染） |
| 帧率（本机回环） | ✅ | 1027–2938 fps（320x240 ～ 256x192，单线程 pump） |
| 丢包恢复 | ✅ | 丢 1 个分片 → 该帧丢弃 → 差分参考断裂 → 下一个关键帧逐字节恢复 |
| 与标准 WebRTC 互通 | ❌ | 无 RTP/RTCP、SRTP/DTLS、ICE；`VideoChannel` 是自研帧格式（见 §9） |
| 1080p | ❌（受限） | WSL2 `rmem_max = 208KB` 导致单帧突发 > ~300KB 丢包，见 §8 |

**一句话**：媒体四段（采集/编码/传输/渲染）在 WSL2 内**已经闭环且可逐帧验证**；
缺的是 WebRTC 的**安全（DTLS/SRTP）、NAT 穿透（ICE）、自适应（GCC/码率）**——
这些不属于 M0/M1 的范围，见 §10。

---

## 2. 架构

```
┌──────────────┐   BGRA   ┌──────────────┐  MediaHeader+payload  ┌────────────────────┐
│ VideoSource  │ ───────► │ VideoEncoder │ ────────────────────► │  UdpVideoSender    │
│ X11/XShm     │          │ raw/delta    │                       │  VideoChannel      │
│ Synthetic    │          │ (可选 H.264) │                       │  1200B 分片        │
└──────────────┘          └──────────────┘                       └─────────┬──────────┘
                                                                          │ UDP 127.0.0.1
┌──────────────┐   BGRA   ┌──────────────┐   I420/BGRA   ┌─────────────────▼──────────┐
│ FrameRenderer│ ◄─────── │ VideoDecoder │ ◄──────────── │  UdpVideoReceiver          │
│ SDL2 window  │          │ raw/delta    │               │  VideoChannel 4 路重组     │
│ Headless     │          │ (可选 H.264) │               │  1s 缺片超时               │
└──────────────┘          └──────────────┘               └────────────────────────────┘
```

| 层 | 头文件 | 实现 | 依赖 |
|---|---|---|---|
| 帧容器 | `frame.hpp` | `frame.cpp` | 标准库 |
| 像素转换 | `pixel.hpp` | `pixel.cpp` | 标准库（定点 BT.601） |
| 采集 | `capture.hpp` | `capture_x11.cpp` / `capture_synthetic.cpp` | X11 + Xext |
| 编码/解码 | `codec.hpp` | `codec.cpp` / `codec_delta.cpp` / `ffmpeg_codec.cpp` | 标准库 |
| 传输 | `transport.hpp` | `transport.cpp` | Bento `VideoChannel` + POSIX socket |
| 渲染 | `render.hpp` | `render.cpp` | SDL2（可选） |
| 统计 | `stats.hpp` | `stats.cpp` | 标准库 |
| 管线 | `pipeline.hpp` | `pipeline.cpp` | 以上全部 |

---

## 3. 采集层（X11 / XShm）

### 3.1 路径

1. `XOpenDisplay(nullptr)` → WSLg 的 `DISPLAY=:0`；
2. `XShmQueryExtension` 存在（WSLg 实测有 `MIT-SHM`）→
   `XShmCreateImage` + `shmget(IPC_PRIVATE)` + `shmat` + `XShmAttach`；
3. 每帧 `XShmGetImage(root, x, y, w, h)`：X 服务器把像素直接写进共享段，客户端只做一次
   BGRA 转换拷贝；
4. 失败回退 `XGetImage`（见 §8 的 WSL2 限制：本机 **BadMatch**）。

### 3.2 格式与裁剪

- depth-24 / 32bpp / LSBFirst 的 ZPixmap，内存布局 `B,G,R,X`，把 X 通道填 `0xFF` 得 BGRA；
- 非 32bpp 时走 `XGetPixel` 逐点（慢但正确）；
- 抓取区域裁剪到屏幕内：`x∈[0, W-1]`、`width∈[1, W-x]`；负坐标归 0，超大尺寸截断
  （实测请求 100000x100000 被裁剪为 2560x1600 并采集成功）。

### 3.3 合成采集源

`SyntheticCapture` 提供四种**确定性**图案（同一 `frame_id` 永远同一画面）：

| 图案 | 语义 | 差分编码表现 |
|---|---|---|
| `kStatic` | 静止桌面 | 收敛到 **36B/帧**（头 32B + bitmap 0） |
| `kScroll` | 滚动条 + 帧号刷新 | 每帧几 KB（少量脏块） |
| `kPatch` | 窗口拖动（24x24 方块移动） | 每帧 2–5 KB |
| `kNoise` | 全画面伪随机 | 不可压缩，≈ I420 原大小 |

---

## 4. 编码层

### 4.1 四种 CodecKind

| 枚举 | 名字 | 压缩 | 无损 | 用途 |
|---|---|---|---|---|
| `kRawBgra` | `raw-bgra` | 无（4B/px） | ✅ | M0 兜底：任何机器都能跑 |
| `kRawI420` | `raw-i420` | 1.5B/px | ✅（BGRA→I420 有转换损失） | 兜底 + 基线对比 |
| `kDeltaI420` | `delta-i420` | 块级差分 + RLE | ✅（I420 域内逐字节无损） | **M1 默认** |
| `kH264` | `h264` | 有损（NVENC/libx264） | ❌ | 探测/验证（见 §4.3） |

### 4.2 delta-i420（自研，默认）

设计目标：**不引入任何第三方编码器**也能把桌面画面压到极小，且能逐帧严格断言。

- 16x16 宏块网格（色度 8x8），每帧比较当前 I420 与参考帧；
- 无脏块 → payload 只有 4 字节（`bitmap_bytes = 0`），**整帧 36 字节**；
  实测 128x96 关键帧 18512B → 差分帧 36B，**514 倍**；
- 脏块按行优先，每块 1B mode（0=raw 384B / 1=RLE），块内 `(run u8, value u8)`；
- 边缘宏块按平面边界裁剪，奇数宽高（1x1、3x5、17x33、33x17、213x157）全部覆盖；
- **无反向通道**：差分依赖参考帧，丢帧会让后续帧内容错，因此管线每 `keyframe_interval`
  帧强制关键帧（默认 30）自愈。

线格式见 `include/go2cpp/media/codec.hpp` 的文件头注释。

### 4.3 H.264 / NVENC 的真实结论

- **NVENC 本身可用**：`ffmpeg -c:v h264_nvenc` 实测成功；`av1_nvenc` 也可用；
- **但 `go2cpp_media` 不链接 libavcodec**：WSL2 只有运行时库（`libavcodec.so.60`），
  **没有 `libavcodec-dev` / `libx264-dev` / `libvpx-dev` 头文件**，`pkg-config` 全部缺失，
  agent 无 sudo 密码装不了（见调研报告 R1）；
- **子进程后端只做到探测**：`ffmpeg_codec.cpp` 保留了 fork/exec + 双向管道的骨架，
  但「一帧进一帧出」的边界对齐需要解析 Annex-B NAL 或改用 mpegts PES，
  M1 不做（逐帧校验路径用无损 delta 更合适）；
- **独立验证已做**：`media_test_h264` 直接调 ffmpeg CLI 跑「30 帧 BGRA → H.264 → BGRA」，
  实测：

  | 编码器 | 帧数 | PSNR 均值 | PSNR 最小 | 码流 |
  |---|---|---|---|---|
  | `h264_nvenc` | 30 | **37.09 dB** | 33.35 dB | 400 KB |
  | `libx264` | 30 | 37.08 dB | 33.34 dB | 402 KB |

  且码流包含 Annex-B 起始码 `00 00 00 01`。

---

## 5. 传输层

### 5.1 复用 Bento VideoChannel（未修改）

- 单帧拆成 `<= 1200B` 的 VIDEO 分片，每片是完整 Bento 帧（16B 头 + 8B 视频子头 + 数据），
  可直接 `sendto`；
- 收端 `OnFrame` 做乱序重组、旧帧丢弃（DropStale）、1s 缺片超时；
- 单帧上限 256 片 ≈ 300KB；4 路并发重组。

### 5.2 MediaHeader（本模块新增，32B 大端）

`magic "G2MF" | version | codec | flags | w | h | frame_id | payload_len | capture_ns`

`capture_ns` 是**端到端延迟的口径**：采集完成时刻 → 解码完成并交付渲染的时刻。

### 5.3 UDP 端点

- `AF_INET/SOCK_DGRAM`，绑定 `127.0.0.1:0`（内核分配端口）；
- `SO_RCVBUF/SO_SNDBUF` 请求 4MB（实际被 `rmem_max` 截断，见 §8）；
- 单线程 pump：`poll` + 非阻塞 `recv`，`Pump(timeout_ms, max_packets)`；
- **不启线程**：测试可逐包精确控制（丢包/乱序注入）。

### 5.4 进程内直连

`InProcessVideoLink` 用同一套 VideoChannel 分片/重组，但不过 socket，
用于隔离「编解码正确性」与「UDP 丢包」两类问题。

---

## 6. 渲染层

| 渲染器 | 说明 |
|---|---|
| `HeadlessRenderer` | 零依赖，只统计帧数 + 内容校验和（M1 逐帧校验默认走这条） |
| `SdlRenderer(headless=true)` | 初始化 SDL 但不建窗口（`SDL_VIDEODRIVER=dummy` 可跑） |
| `SdlRenderer(headless=false)` | 真实窗口：`SDL_UpdateTexture(ARGB8888)` + `RenderCopy` + `Present` |

SDL2 在 WSLg 下实测可用（`SdlAvailable() == true`）；`SDL_SetHint` 未使用，
加速失败自动回退 software renderer。窗口尺寸随帧变化自动重建纹理。

---

## 7. 延迟与帧率（本机回环实测）

口径：`steady_clock(解码完成并交付渲染) - capture_ns`，**单进程**，含内核 UDP 往返。

| 场景 | 帧数 | 交付/校验 | 字节 | p50 | p99 | max | fps |
|---|---|---|---|---|---|---|---|
| static 320x240 | 100 | 100/100 | 75,880 | **0.237 ms** | 0.360 ms | 1.374 ms | 2038 |
| patch 320x240 | 100 | 100/100 | 479,128 | 0.254 ms | **0.984 ms** | 1.245 ms | 1947 |
| scroll 256x192 | 60 | 60/60 | 280,847 | 0.162 ms | 0.698 ms | 0.698 ms | 2938 |
| noise 192x144 | 40 | 40/40 | 1,665,110 | 0.302 ms | 0.628 ms | 0.628 ms | 2464 |
| raw-i420 320x240 | 30 | 30/30 | 3,456,960 | 0.712 ms | **1.194 ms** | 1.194 ms | 1028 |
| raw-bgra 160x120 | 15 | 15/15 | 1,152,480 | 0.411 ms | 0.643 ms | 0.643 ms | 1717 |
| **X11 真实采集 320x240** | 30 | 30/30 | 9,168 | 0.305 ms | **1.003 ms** | 1.003 ms | 1163 |

- 静止桌面 100 帧共 75,880B ≈ **759B/帧**（含 1 个关键帧），差分编码把带宽压到极低；
- 噪声场景压缩比 **1.004**，说明 delta 对不可压缩输入不膨胀（最坏情况有界）；
- X11 真实采集的 9,168B/30 帧 ≈ **306B/帧**（WSLg 桌面几乎静止）。
- 本机回环延迟远低于局域网 WebRTC 的 30–100ms 量级：这里的数字**不含网络 RTT**，
  只反映「媒体处理 + 本机 UDP」的开销；跨机部署要再加上 RTT 与 jitter buffer。

---

## 8. WSL2 限制（实测，影响设计）

| # | 限制 | 实测/证据 | 影响与对策 |
|---|---|---|---|
| 1 | `XGetImage` 抓 root window **BadMatch** | `XGetImage(root,0,0,320,240)` 返回 NULL，XError code=8；WSLg 的 XWayland root 被 composite 重定向 | 回退路径在 WSL2 **不可用**；必须走 XShm。测试对该路径 SKIP 而非 FAIL |
| 2 | `net.core.rmem_max = 212992`（208KB） | `sysctl net.core.rmem_max` | 单帧突发 > ~300KB（含 skb ~768B/包开销）会丢包。默认 320x240（关键帧 115KB / 97 片）在预算内；512x384（295KB / 247 片）实测丢包。对策：降分辨率 或 root 调大 rmem_max |
| 3 | `VideoChannel` 单帧上限 256 片 = 307200B | `kMaxFragments * kFragmentBytes` | 即使 rmem 够，单帧也发不到 640x480 的 I420（460800B）。大分辨率必须走 delta（脏块）或真编码器 |
| 4 | NVENC 是 WSL 注入的私有通路 | `/usr/lib/wsl/lib/libnvidia-encode.so.1`；`h264_nvenc` OK | 不能代表原生 Linux 部署（服务器多无 GPU）；编码层已抽象，可换 VAAPI/x264 |
| 5 | 无 `libavcodec-dev`/`libx264-dev`/`libvpx-dev` | `pkg-config` 全部缺失，只有运行时 `.so` | 无法链接 libavcodec/libvpx；H.264 只能走 ffmpeg 子进程或 NVENC SDK |
| 6 | 无 `/dev/dri` | `ls /dev/dri` 不存在 | VAAPI 不可用 |
| 7 | 采不到 Windows 桌面 | WSLg 只提供 Linux 侧 X11 | 本闭环验证的是「WSLg 内的 Linux 画面」 |
| 8 | 无 `CAP_NET_ADMIN` | 无 sudo | `tc netem` 弱网不可用；用 `InProcessVideoLink`/手工分片注入做丢包测试 |

---

## 9. 与「标准 WebRTC」的差距

本模块**不是** WebRTC 实现，是一条自研的实时媒体链路：

| WebRTC 组件 | 本模块 | 差距 |
|---|---|---|
| RTP/RTCP | Bento `VideoChannel`（自研帧 + 分片） | 不兼容 RTP，不能与浏览器互通 |
| SRTP/DTLS | ❌ | 明文 UDP（仅回环测试用） |
| ICE/STUN/TURN | ❌ | 只有 `127.0.0.1` 直连 |
| GCC/REMB | ❌ | 无带宽估计；`congestion.hpp` 的 BBR 子集未被媒体层使用 |
| NACK/FEC | ⚠️ | VideoChannel 支持 XOR FEC（`fec_group_size`），本模块默认关闭 |
| jitter buffer | ❌ | 只有 1s 缺片超时，不是按抖动自适应的 jitter buffer |
| adaptive bitrate | ❌ | 编码参数固定 |
| SDP 信令 | ❌ | 无（管线在进程内直接构造） |

结论：**M0/M1 的目标是「验证媒体四段在 WSL2 能跑通且可量化」，不追求协议互通**。

---

## 10. 后续（M2 建议）

1. **加密**：DTLS-SRTP（OpenSSL + libsrtp2）或 Noise IK（更轻，但非标准）；
2. **输入回传**：XTEST 注入（WSLg 有 XTEST 扩展）；
3. **tile/脏矩形**：把 delta 的宏块 dirty bitmap 升级成 XDamage 驱动的 tile 编码，
   只发变化区域（本模块的 bitmap 已经是这个方向的最小版本）；
4. **码率自适应**：用 delta 的实时字节数 + `congestion.hpp` 的带宽估计调关键帧间隔与分辨率；
5. **jitter buffer**：按 `capture_ns` 的到达抖动做自适应深度；
6. **真 H.264 逐帧封装**：给 ffmpeg 子进程加 mpegts/PES，或链接 libavcodec（需 `-dev` 包）；
7. **1080p**：调 `rmem_max` + 多帧流水 + 真编码器（H.264 码率比 I420 低 50–100 倍，
   1080p30 的 4Mbps 码流每帧仅 ~17KB，完全在 UDP 预算内）。

---

## 11. 测试与验收

```bash
cmake -S . -B .build-bento-wsl -DGO2CPP_WITH_BENTO=ON -DGO2CPP_WITH_MEDIA=ON -DGO2CPP_MEDIA_TESTS=ON
cmake --build .build-bento-wsl -j8
ctest --test-dir .build-bento-wsl -R "media_" --output-on-failure
```

| 测试 | 覆盖 | 实测 |
|---|---|---|
| `media_frame` | 尺寸推导 / 校验和 / BGRA↔I420 / 奇数尺寸 / 行补齐 / PSNR | 36 checks，0.01s |
| `media_codec` | MediaHeader 往返与拒绝 / raw 往返 / delta 100 帧逐帧无损 / 静止收敛 / 单块 / 噪声 / 奇数尺寸 / 关键帧恢复 / 损坏拒绝 | 491 checks，0.04s |
| `media_capture` | 合成四图案 + 确定性 / XShm 采集 / XGetImage 回退（SKIP）/ 区域裁剪 / 负坐标 | 108 checks，0.11s |
| `media_transport` | UDP 端点 / 3000B 与 150KB 往返 / 乱序 / 缺片超时 / 进程内丢包 / 畸形包 | 63 checks，0.03s |
| `media_render` | headless 与 SDL（dummy）/ 纹理重建 / I420 输入 / 非法帧 | 32 checks，0.03s |
| `media_e2e` | 8 个场景（含 X11 真实采集、丢包恢复）+ 逐帧校验 + p50/p99 | 359 checks，0.89s |
| `media_h264` | ffmpeg/NVENC 探测 + 30 帧编码解码 PSNR | 13 checks，3.11s |

**总计 7 个测试、1102 条断言、4.25 秒**，全部通过；无需 root、无外部网络依赖。

---

## 12. 文件清单

```
include/go2cpp/media/frame.hpp        帧容器与格式
include/go2cpp/media/pixel.hpp        BGRA↔I420 / PSNR / MAE
include/go2cpp/media/capture.hpp      采集抽象 + 合成源
include/go2cpp/media/codec.hpp        编解码接口 + MediaHeader + 线格式
include/go2cpp/media/transport.hpp    UDP 端点 + VideoChannel 封装
include/go2cpp/media/render.hpp       渲染抽象
include/go2cpp/media/stats.hpp        延迟分位数
include/go2cpp/media/pipeline.hpp     端到端管线

src/media/{frame,pixel,stats,codec,codec_delta,ffmpeg_codec,
           capture_x11,capture_synthetic,transport,render,pipeline}.cpp

tests/media/{test_frame,test_codec,test_capture,test_transport,
             test_render,test_e2e,test_h264}.cpp + media_test_util.hpp

cmake/Go2CppMedia.cmake               构建接入（默认 OFF）
```
