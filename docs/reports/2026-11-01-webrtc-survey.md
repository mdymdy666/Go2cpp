# WebRTC 技术栈 + Linux（WSL2）可行性调研

> 调研日期：2026-10-31 · 方式：**只读**（未改工作区任何文件，本报告写 `/tmp`）
> 环境：WSL2 / Ubuntu 24.04 / 内核 6.6.87.2-microsoft-standard-WSL2 / 12 vCPU / 19 GiB
> 工作树：`/UserData/CodexWorkSpace/Go2cpp`（= `/mnt/e/CodexWorkspace/Go2Cpp`），构建 `.build-bento-wsl`
> 标注约定：**【实测】**= 本次命令跑出来的结果；**【推断】**= 基于已知事实的判断，未实测。

---

## 1. 结论摘要（TL;DR）

1. **WebRTC 的难点不在编码，在传输与自适应**。编码（VP8/VP9/H.264/AV1/Opus）在 Linux 上是现成的库；真正要自研的是 SRTP/DTLS、ICE、GCC、NACK/FEC、jitter buffer 这一整条实时链路。
2. **WSL2 比预期强**：WSLg 提供了完整的 X11（含 DAMAGE/SHM/XTEST/GLX）+ Wayland + PulseAudio；X11 抓屏、OpenGL/SDL 渲染、CUDA/Vulkan、**NVENC H.264/AV1** 全部实测可用；软件编码器（libvpx/x264/aom/svt-av1/libopus）由 ffmpeg 6.1.1 全量提供。
3. **WSL2 的硬缺口只有三个**：① 无摄像头（`/dev/video*` 不存在）；② 无 `/dev/dri` → **VAAPI 不可用**（NVENC 是 WSL 注入的私有通路，不能当作 Linux 通用方案）；③ 音频只有 RDP 转发的单声道 RDPSource（能用，但延迟与音质受限）。
4. **工作区现状与任务书的两处前提不符**（必须纠正，否则决策会错）：
   - **没有 PSK/X25519/DTLS/SRTP**：全仓库 `grep -i "PSK|x25519|ChaCha|AES-GCM|srtp|dtls"` 在 `*.hpp` 中 **0 命中**；
   - **没有 ICE/STUN/TURN**：`docs/research/net-scenarios.md:163` 明确写「无 NAT 穿透（STUN/打洞）能力」；Bento 只有 UDP 直连 + `token_pairing.hpp`（relay 的 1:N 配对表，不是穿透）。
   所谓「已有 P2P」应准确表述为：**已有 UDP 直连的帧协议与信道层**，不是 NAT 穿透意义上的 P2P。
5. **可做「Linux 版远程桌面」**：采集(WSLg/X11 实测可用) → 编码(NVENC 实测可用) → Bento 传输(已有信道层) → 渲染(GL/SDL 实测可用) 四段在 WSL2 内都能跑通；缺的是加密、输入回传、tile/脏矩形与码率自适应。
6. **与 JinShan 的关系**：JinShan 的 `IRDPSRAPI` 是 Windows 专属 COM，封装了 RDP 的采集/编码/传输，Linux **没有等价 API**；能复用的是产品层语义（会话、输入映射、脏矩形思路），协议层不可移植。

---

## 2. WebRTC 技术栈全貌（分层）

WebRTC 不是一个库，而是 **7 层、约 20 个 RFC** 的组合。下表按「职责 → 标准/协议 → Linux 常用实现 → 与我们的关系」组织。

### 2.1 采集层（Capture）

| 来源 | Linux 接口 | 关键细节 | 我们的依赖 |
|---|---|---|---|
| 摄像头 | **V4L2**（`/dev/video*`） | `VIDIOC_*` ioctl、MMAP/DMABUF、YUYV/NV12 格式协商；多摄像头枚举 | 无（WSL2 无设备） |
| 桌面（X11） | XCB/Xlib | `XShmGetImage`（共享内存整屏）+ **XDamage**（脏矩形）+ `XComposite`（窗口重定向） | GL/ffmpeg 有，自研需 `libx11-xcb-dev` |
| 桌面（Wayland） | `xdg-desktop-portal` ScreenCast + **PipeWire**；wlroots 的 `wlr-screencopy` | portal 是标准路径（需用户授权），wlr 协议是 wlroots 专有 | 无（WSLg 的 weston 不提供 wlr 协议） |
| 桌面（DRM/KMS） | `kmsgrab`（ffmpeg）/ DRM lease | 需 `CAP_SYS_ADMIN`，直接抓 scanout buffer，性能最好 | 无（WSL2 无 `/dev/dri`） |
| 音频 | **PulseAudio** / PipeWire / ALSA | Pulse 是桌面标准；ALSA 直连延迟低但混音差 | Pulse 可用（WSLg 转发） |

**关键认知**：桌面采集的性能分水岭是「是否零拷贝」。X11 的 `XShmGetImage` 是「一次共享内存拷贝」，DMABUF（Wayland/DRM）才是零拷贝。远程桌面场景 1080p30 下前者可接受，4K 就必须上 DMABUF/硬件。

### 2.2 编码层（Codec）

| 编码 | 标准 | 软件实现 | 硬件实现 | WSL2 状态【实测】 |
|---|---|---|---|---|
| VP8 | RFC 6386 | libvpx | VAAPI/NVENC | ✅ `libvpx` 可用 |
| VP9 | — | libvpx-vp9 | VAAPI/NVENC | ✅ 可用 |
| H.264 | RFC 6184 | x264 / OpenH264 | NVENC/QSV/VAAPI | ✅ `libx264`、`h264_nvenc` 可用；VAAPI ❌ |
| H.265 | RFC 7798 | x265 | NVENC/QSV/VAAPI | ✅ `libx265`、`hevc_nvenc` |
| AV1 | — | libaom / SVT-AV1 | NVENC(40系+) / QSV / VAAPI | ✅ `libaom-av1`、`libsvtav1`、**`av1_nvenc` 实测 OK** |
| Opus | RFC 6716 | libopus | — | ✅ `libopus` 可用 |

**实测记录**（ffmpeg 6.1.1）：
- `h264_nvenc` → **OK**；`av1_nvenc` → **OK**（RTX 5060 Laptop，驱动 573.22）
- `libvpx-vp9` → OK；`libx264` → OK；`libopus` → OK
- `h264_vaapi -vaapi_device /dev/dri/renderD128` → **FAIL**（`No VA display found`，无 render node）

**工程注意**：ffmpeg 自带这些编码器 ≠ 我们能链接它们。`pkg-config` 实测 **缺** `vpx/opus/x264/srtp2` 的 `.pc`（只有运行时库 `libvpx9/libopus0/libx264-164`，没有 `-dev`）。要自研调用 libvpx/libopus API，必须先 `apt install libvpx-dev libopus-dev libx264-dev libsrtp2-dev`（候选包齐全，但 **sudo 需要密码**，agent 装不了，需用户执行）。

### 2.3 传输层（Transport）

| 组件 | 标准 | 作用 | Linux 实现 | 我们的现状 |
|---|---|---|---|---|
| RTP/RTCP | RFC 3550/4585 | 媒体分包、时序、NACK/PLI/REMB 反馈 | libwebrtc、pion/rtcp、ffmpeg | ❌ 只有自研 Bento 帧，**不兼容 RTP** |
| SRTP | RFC 3711 | 媒体加密（AES-CM/AES-GCM）+ 抗重放 | libsrtp | ❌ 无 |
| DTLS | RFC 6347/9147 | 密钥协商（DTLS-SRTP，RFC 5764） | OpenSSL/BoringSSL/mbedTLS | ❌ 无（连 PSK 都没有） |
| ICE | RFC 8445 | NAT 穿透：候选收集/连通性检查/提名 | libnice、libwebrtc | ❌ 无（只有 UDP 直连） |
| STUN/TURN | RFC 8489/8656 | 反射地址发现 / 中继 | coturn、libnice | ❌ 无（`token_pairing` 只是 relay 配对表） |
| SCTP/DataChannel | RFC 8831 | 可靠/部分可靠消息通道 | usrsctp | ⚠️ Bento 的三类通道**语义相近**，但线上格式不同 |

**结论**：Bento 的 `Link` 是「UDP + 自研 12B 帧头 + 三类通道」，与 WebRTC 的「RTP/RTCP + SRTP + DTLS + ICE」是**两套不兼容的栈**。要么自研对齐 WebRTC（工作量大），要么走「自研栈 + 只借鉴 WebRTC 的算法」，二选一必须先定。

### 2.4 抗弱网（Congestion / Loss Recovery）

| 机制 | 作用 | WebRTC 做法 | 我们现状 |
|---|---|---|---|
| **GCC** | 基于到达时间梯度的带宽估计 | trendline + 到达时间滤波器 + REMB/transport-cc | ⚠️ 有 **BBRv1 子集**（`congestion.hpp`）+ NewReno 对照，**不是 GCC**；BBR 对「排队延迟」不敏感，弱网尾延迟不如 GCC |
| **NACK/RTX** | 丢包重传 | RTCP NACK + 独立 RTX 流 | ✅ 有 SACK + 快速重传 + PTO（Bento 内） |
| **FEC** | 前向纠错，抗随机丢包 | ULPFEC RFC 5109 / FlexFEC | ✅ 有 XOR + **GF(256) Reed-Solomon**（比典型 XOR 更强） |
| **jitter buffer** | 乱序/抖动平滑 | 自适应深度（按网络抖动动态调整） | ❌ 无（`VideoChannel` 只有 1s 重组超时，不是 jitter buffer） |
| **pacing** | 平滑发送，避免突发 | 令牌桶 + 拥塞窗口 | ✅ 有（`pacing_enabled`，5ms burst） |
| **adaptive bitrate** | 按带宽调分辨率/码率 | 编码器码率控制 + 层选择（simulcast/SVC） | ❌ 无 |

### 2.5 信令（Signaling）

- **SDP offer/answer**（RFC 3264/8866）：交换编解码、ICE 候选、DTLS 指纹、方向。
- **ICE 候选交换**（trickle ICE）：增量交换候选，缩短建连时间。
- **我们的现状**：❌ 完全没有。Bento 的建连是自定义 `HELLO` 握手（单 peer、无协商）。
- **最小实现**：不需要完整 SDP 解析器；自研场景只要交换「编解码 + 分辨率 + 候选地址 + 密钥材料」4 项，用一个 JSON 信令即可，但**要互操作 WebRTC 就必须实现 SDP**。

### 2.6 渲染（Render）与音频处理（Audio Processing）

| 能力 | Linux 方案 | WSL2 状态【实测】 |
|---|---|---|
| 视频渲染 | SDL2/SDL3、OpenGL、Vulkan、GTK/Qt | ✅ `-f sdl` OK、`-f opengl` OK；GLX 扩展存在；Vulkan 设备 OK |
| 音频采集/播放 | PulseAudio/PipeWire/ALSA | ✅ `parec --device=RDPSource` 成功录到 244 KB；`RDPSink.monitor` 可录系统声 |
| Opus 编解码 | libopus | ✅ |
| **AEC**（回声消除） | WebRTC AEC3 / speexdsp | ❌ 无（需 `webrtc-audio-processing` 或自研） |
| **NS**（降噪） | WebRTC NS / RNNoise | ❌ 无 |
| **AGC**（自动增益） | WebRTC AGC | ❌ 无 |

**AEC 是隐藏的大坑**：远程桌面若做双向语音，AEC 不好会导致啸叫；WebRTC 的 AEC3 有 10 年工程积累，自研不现实，必须用 `webrtc-audio-processing` 库（LGPL/BSD 混合，需确认许可）。

---

## 3. 环境检查：方法与原始证据

全部为**只读命令**（未安装、未改配置），在 WSL2 内执行。

### 3.1 设备与显示

| 检查 | 命令 | 结果 | 判定 |
|---|---|---|---|
| 摄像头 | `ls /dev/video*` | `No such file or directory` | ❌ 无设备 |
| GPU 设备 | `ls /dev/dri` | 不存在 | ❌ 无 DRM render node |
| WSL GPU 通路 | `ls /dev/dxg` | 存在（`crw-rw-rw- 10,125`） | ✅ DirectX/CUDA 通路 |
| 声卡 | `ls /dev/snd` | 仅 `timer` | ❌ 无 ALSA 硬件 |
| X11 | `env \| grep DISPLAY` | `DISPLAY=:0` | ✅ |
| Wayland | 同上 | `WAYLAND_DISPLAY=wayland-0` | ✅ |
| 音频服务 | `env \| grep PULSE_SERVER` | `unix:/mnt/wslg/PulseServer` | ✅ |
| WSLg 版本 | `cat /mnt/wslg/versions.txt` | `WSLg 1.0.66`，weston + PulseAudio + FreeRDP | — |
| 屏幕 | `xrandr` | `XWAYLAND0 2560x1600`，多档分辨率 | ✅ |
| GPU | `nvidia-smi` | **RTX 5060 Laptop GPU，573.22，8151 MiB** | ✅ |

### 3.2 X11 扩展（决定采集与输入注入能力）

`xdpyinfo` 实测 22 个扩展，**关键扩展全在**：

```
BIG-REQUESTS  Composite  DAMAGE  DOUBLE-BUFFER  GLX  MIT-SHM  Present
RANDR  RECORD  RENDER  SHAPE  SYNC  XFIXES  XInputExtension  XTEST  XVideo
```

对应能力：
- **DAMAGE** → 脏矩形通知（远程桌面的核心，不用整屏轮询）
- **MIT-SHM** → `XShmGetImage` 零额外拷贝抓屏
- **XTEST** → 键鼠注入（远程桌面的输入回传）
- **GLX/Present** → 渲染与 vsync
- **XComposite** → 按窗口采集

### 3.3 编码/采集/渲染实测

| 测试 | 命令（节选） | 结果 |
|---|---|---|
| X11 抓屏 | `ffmpeg -f x11grab -video_size 1280x720 -i :0.0 -frames:v 1` | ✅ 生成 PNG（空桌面 3,011 B） |
| X11 抓窗口 | 启动 `xeyes` 后同命令 2560x1600 | ✅ **12,290 B**（画面有内容，证明能采 WSLg 内 Linux 窗口） |
| KMS 抓屏 | `ffmpeg -f kmsgrab -i -` | ❌ `Failed to open DRM device` |
| NVENC H.264 | `ffmpeg ... -c:v h264_nvenc` | ✅ OK |
| NVENC AV1 | `ffmpeg ... -c:v av1_nvenc` | ✅ OK |
| VP9 | `ffmpeg ... -c:v libvpx-vp9` | ✅ OK |
| x264 | `ffmpeg ... -c:v libx264` | ✅ OK |
| Opus | `ffmpeg ... -c:a libopus` | ✅ OK |
| VAAPI | `-vaapi_device /dev/dri/renderD128` | ❌ `No VA display found` |
| CUDA | `ffmpeg -init_hw_device cuda` | ✅ OK |
| Vulkan | `ffmpeg -init_hw_device vulkan` | ✅ OK |
| OpenGL 渲染 | `ffmpeg -f opengl -window_size 320x240` | ✅ OK |
| SDL 渲染 | `ffmpeg -f sdl` | ✅ OK |
| 音频录制 | `parec --device=RDPSource --raw` | ✅ 244,314 B（3 秒 s16le） |
| 音频源 | `pactl list short sources` | `RDPSource`（1ch 44.1k）、`RDPSink.monitor`（2ch） |

### 3.4 工具链与依赖

```
gcc 13.3.0 · cmake 3.28.3 · ninja 1.11.1 · ffmpeg 6.1.1 · pulseaudio 16.1
```

`pkg-config` 探测（**关键差异**）：

| 库 | pkg-config | 说明 |
|---|---|---|
| aom / sdl2 / x11 / wayland-client / libdrm / egl / gl | ✅ 有 | 开发包已装 |
| **vpx / opus / x264 / srtp2** | ❌ **无** | 只有运行时库，**链接不了** |
| libva | 未探测到 | `libva2` 在，但 `libva-dev`/`vainfo` 没有 |

`apt-cache policy` 显示候选包齐全：`libvpx-dev 1.14.0`、`libopus-dev 1.4`、`libsrtp2-dev 2.5.0`、`libx264-dev`、`libavcodec-dev`。
`archive.ubuntu.com` 可达（HTTP 200），但 **`sudo` 需要密码**（`sudo -n true` 失败）→ **agent 无法自行安装，必须由用户执行一条 apt 命令**。

### 3.5 网络事实（影响传输/ICE 测试）

- WSL2 地址：`172.17.211.143/20`，默认路由 `172.17.208.1`，**MTU 1280**；
- 这是 Windows NAT 后的内网地址 → **外部无法直连**，入站需 `netsh portproxy` 或 WSL2 mirrored 模式；
- 无 `CAP_NET_ADMIN`（`sudo` 需密码）→ `tc netem` 弱网测试仍只能 SKIP（与 `tests/bento/PROTOCOL.md` 记录一致）；
- `sysctl net.core.rmem_max = 212992`（208 KB）→ 高码率视频接收需调大，否则丢包。

---

## 4. WSL2 能力结论表（逐项）

图例：✅ 能做 · ⚠️ 受限（能做但有明确条件）· ❌ 不能做

| 能力 | 判定 | 原因 / 证据 |
|---|---|---|
| X11 桌面采集（WSLg 内 Linux 应用） | ⚠️ | **实测可抓**（xeyes 12 KB vs 空屏 3 KB）；但只能采 **WSLg 虚拟屏**，**采不到 Windows 桌面** |
| Wayland 采集 | ❌ | WSLg 的 weston 不实现 `wlr-screencopy`；portal 后端是 gtk，不支持 ScreenCast；无 PipeWire daemon |
| DRM/KMS 采集 | ❌ | 无 `/dev/dri`，`kmsgrab` 直接失败 |
| 摄像头 | ❌（默认）/ ⚠️（直通） | 无 `/dev/video*`；内核有 `uvcvideo.ko`，理论上可用 usbipd 直通 + `modprobe`，但需 Windows 管理员 |
| 软件编码 VP8/VP9/H.264/AV1/Opus | ✅ | ffmpeg 实测全通过 |
| 硬件编码 NVENC | ✅ | 实测 OK（WSL 注入 `libnvidia-encode.so`）；但这是 WSL 专有通路，**不能代表原生 Linux** |
| 硬件编码 VAAPI/QSV | ❌ | 无 `/dev/dri/renderD128`；QSV 无 Intel GPU |
| OpenGL / SDL 渲染 | ✅ | GLX 扩展在，`-f opengl`/`-f sdl` 实测 OK；渲染性能未测（可能走 d3d12/llvmpipe） |
| Vulkan | ✅ | `-init_hw_device vulkan` 实测 OK |
| PulseAudio 采集/播放 | ⚠️ | 实测可录；但只有 1ch 44.1k 的 RDP 转发，**延迟与音质受限** |
| ALSA 直连 | ❌ | `/dev/snd` 只有 timer |
| AEC/NS/AGC | ❌ | 无 `webrtc-audio-processing`，需引入或自研 |
| UDP 传输 / 拥塞控制 / FEC 实验 | ✅ | 12 vCPU、回环、Bento 已构建 |
| 真实 NAT 穿透（ICE）测试 | ⚠️ | WSL2 自身在 NAT 后，可作「被穿透端」；完整测试需外部 STUN/TURN 与另一台机器 |
| `tc netem` 弱网 | ❌ | 无 root（`CAP_NET_ADMIN`） |
| 摄像头/桌面端到端**产品级**验证 | ❌ | 必须原生 Linux 或 Windows |

---

## 5. 可行性矩阵（修正版）

> 说明：任务书给的矩阵有 4 处前提需修正，已在「修正」列标注。修正依据见 §3、§7。

| 技术 | 依赖 | 我们的现状（核实后） | WSL2 可行 | 原生 Linux 可行 | 修正 |
|---|---|---|---|---|---|
| 桌面采集 | X11/Wayland | ❌ 无采集代码 | ⚠️ X11 可（仅 WSLg 内窗口） | ✅ | 补充：Windows 桌面采不到 |
| 摄像头 | V4L2 | ❌ | ❌（无设备） | ✅ | — |
| VP8/VP9 编码 | libvpx | ❌ 无调用代码 | ✅（ffmpeg 可用；自研需 `-dev`） | ✅ | 补充 `-dev` 缺失 |
| H.264 | OpenH264/x264 | ❌ | ✅ | ✅ | — |
| AV1 | aom/svt-av1 | ❌ | ✅ | ✅ | 新增 |
| RTP/RTCP | 自研 | ❌ **无 RTP**，只有 Bento 帧 | ✅（自研） | ✅ | **纠正**：Bento 帧 ≠ RTP，不互通 |
| SRTP/DTLS | 自研/OpenSSL | ❌ **无 PSK/DTLS/SRTP**（全仓库 0 命中） | ✅（可做） | ✅ | **纠正**：没有「部分」，是从零 |
| ICE/STUN/TURN | 已有 P2P | ❌ **无 STUN/ICE/TURN**，只有 UDP 直连 + token 配对表 | ✅（可编译，穿透测试受限） | ✅ | **纠正**：不能记 ✅ |
| GCC 拥塞控制 | 自研 | ⚠️ **BBRv1 子集**，非 GCC | ✅ | ✅ | **纠正**：算法不同 |
| NACK/RTX | 自研 | ✅ SACK + 快速重传 + PTO | ✅ | ✅ | 实际比「部分」强 |
| FEC | 自研 | ✅ XOR + GF(256) RS | ✅ | ✅ | 实际比「XOR」强 |
| jitter buffer | 自研 | ❌ 无 | ✅ | ✅ | 新增 |
| adaptive bitrate | 自研 | ❌ 无 | ✅ | ✅ | 新增 |
| SDP 信令 | 自研 | ❌ 无 | ✅ | ✅ | 新增 |
| 渲染 | OpenGL/SDL | ❌ | ✅ **实测 OK** | ✅ | **纠正**：不是 ⚠️ |
| Opus 音频 | libopus | ❌ | ✅（编码）；采集 ⚠️ | ✅ | 补采集限制 |
| AEC/NS/AGC | webrtc-audio-processing | ❌ | ⚠️（可装库） | ✅ | 新增 |
| DataChannel/SCTP | usrsctp | ⚠️ 语义相近（Bento 通道） | ✅ | ✅ | — |

**矩阵一句话**：**WSL2 能验证「编码 + 传输 + 抗弱网 + 渲染」，不能验证「摄像头 + Windows 桌面采集 + 真实 NAT 穿透 + 低延迟音频」**。

---

## 6. 与 JinShan 的关系

### 6.1 JinShan 是什么

JinShan 是 Windows 远程桌面产品，基于 **`IRDPSRAPI`**（Remote Desktop Session API，COM）。它把「屏幕采集 → 编码 → 传输 → 输入回传」全部交给 Windows 的 RDP 服务：
- 应用只负责提供窗口/桌面句柄、接收输入事件；
- 编码（H.264/RemoteFX）、传输（RDP 的 TCP/UDP 双通道）、加密（CredSSP/TLS）都是系统实现；
- **这套 API 只在 Windows 存在**，Linux 没有对应物（xrdp/FreeRDP 是服务端实现，不是可调用的采集 API）。

### 6.2 Linux 对应方案

| JinShan（Windows） | Linux 对应 | 成熟度 |
|---|---|---|
| `IRDPSRAPI` 采集 | X11（XShm + XDamage）/ Wayland（portal+PipeWire） | X11 成熟；Wayland 碎片化 |
| RDP 内置 H.264 | NVENC / VAAPI / x264 | 成熟（需自己接） |
| RDP 传输 + 加密 | 自研（Bento）或 libwebrtc | 我们只有半套 |
| RDP 输入回传 | XTEST / uinput / libinput | 成熟 |
| RDP 音频重定向 | PulseAudio + Opus | 成熟 |

### 6.3 我们能否做「Linux 版远程桌面」？

**能，但不是「移植 JinShan」，而是「重写一条 Linux 管线」**：

```
X11 采集(XDamage+XShmGetImage)
   → 编码(NVENC H.264 / x264)
   → Bento VideoChannel 分片(1200B/帧, 4 路重组, 可选 FEC)
   → UDP 传输(Bento Link, 需补加密)
   → 解码(ffmpeg/libavcodec)
   → SDL/GL 渲染
   ↖ 输入回传(XTEST) + 音频回传(Pulse+Opus)
```

**与 JinShan 可复用的部分**：产品层——会话管理、多显示器映射、输入坐标变换、剪贴板、断线重连策略、脏矩形调度思路。
**不可复用**：`IRDPSRAPI` 的 COM 接口、RDP 的协议栈、CredSSP 认证。

**Bento 的 S12 判定**（`docs/research/net-scenarios.md:174-179`）：架构贴合（STREAM 输入 + VIDEO 画面 + DATAGRAM 光标），但缺口明确——
① 无 tile/脏矩形（VIDEO 是整帧，远程桌面必须按脏矩形发）；
② 无通道优先级（输入被画面队头阻塞 = 卡顿）；
③ 无码率自适应（RDP 会按带宽动态降分辨率）；
④ 无加密；
⑤ 无多 peer（一个 Link 锁首个 HELLO）。

---

## 7. 我们已有能力的真实盘点（Bento）

> 核实方式：读 `include/go2cpp/bento/net/*.hpp` + `.build-bento-wsl` 产物 + 文档交叉验证。

| 能力 | 文件 | 事实 |
|---|---|---|
| 帧协议 | `protocol.hpp` | 12B 帧头、7 种帧类型、1300B 载荷上限、可选 8B ACK、FEC 子头 |
| 可靠通道 | `channel.hpp` | 窗口 256、累积 ACK + 32 位 SACK、PTO 200ms→2s、快速重传阈值 2、可选 FEC 组 |
| 不可靠通道 | 同上 | `UnreliableChannel`：DropStale（旧帧直接丢） |
| 视频通道 | 同上 | 1200B 分片、4 路重组、1s 超时、每帧 k+m 校验分片、FEC 恢复 |
| 拥塞控制 | `congestion.hpp` | **BBRv1 最小子集**（Startup/Drain/ProbeBW/ProbeRTT）+ NewReno 对照 + pacing |
| FEC | `fec.hpp` | XOR（m=1）与 **GF(256) Reed-Solomon**（k≤64, m≤16） |
| 因果原语 | `relation.hpp` | dep_seq 挂起、BARRIER 作废 |
| 链路 | `link.hpp` | 单线程、单 peer、HELLO 握手、心跳、`RunOnce` 事件循环 |
| relay 配对 | `token_pairing.hpp` | (key, token) 1:N 配对表，8 字节 bearer token，非穿透 |
| 构建产物 | `.build-bento-wsl` | `libgo2cpp_bento.a` 存在；测试二进制含 `bento_test_fec`、`bento_test_congestion`、`bento_test_video_fragment` 等 |

**结论**：Bento 在「抗弱网 + 视频分片 + 拥塞控制」上**已经超过「部分」**；但它在「安全（DTLS/SRTP）、NAT 穿透（ICE）、多 peer、tile 语义、码率自适应」上是**零**。这五项恰好是远程桌面与 WebRTC 互通的硬门槛。

---

## 8. 最小可行路径（建议）

### 阶段 M0：采集 + 编码打通（WSL2，1–2 天）

- 用 `ffmpeg -f x11grab -i :0.0 -c:v h264_nvenc` 抓 WSLg 桌面并编码为文件；
- 验收：1080p30、编码延迟 < 10ms、CPU 占用 < 30%；
- 风险：WSLg 的 XWayland 抓屏拷贝开销（未测，需 profile）。

### 阶段 M1：端到端闭环（WSL2，3–5 天）

- **采集** X11 → **编码**（NVENC 或 x264）→ **Bento VideoChannel** 分片 → UDP 回环 → **解码**（ffmpeg CLI 或 libavcodec）→ **SDL 渲染**；
- 复用现有 `VideoChannel` 的 1200B 分片与 FEC，不改帧格式；
- 验收：1080p30 端到端 < 100ms（局域网）、5% 丢包下可恢复、`xeyes`/自绘窗口画面正确；
- 这是**最短闭环**，也是判断「Bento 是否适合实时视频」的关键实验。

### 阶段 M2：远程桌面必需件（WSL2 + 原生 Linux）

1. **加密**：DTLS-SRTP（用 OpenSSL/mbedTLS + libsrtp）或 Noise IK（更轻，但非 WebRTC 标准）；
2. **输入回传**：XTEST 注入（实测 XTEST 扩展在）；
3. **音频**：Pulse 采集 → Opus → 传输 → 播放（AEC 用 `webrtc-audio-processing`）；
4. **tile/脏矩形**：XDamage 事件驱动，只发变化区域；
5. **码率自适应**：按 Bento 的带宽估计调编码器目标码率。

### 阶段 M3：NAT 穿透（需真实环境）

- ICE-lite + STUN（自研或 libnice）+ TURN（coturn）；
- WSL2 自身在 NAT 后（`172.17.x`，MTU 1280），可作「被穿透端」测试；
- 需要第二台机器或公网 STUN 服务器，**WSL2 单机无法完整验证**。

### 阶段 M4：原生 Linux / 摄像头

- VAAPI（原生 Linux 有 `/dev/dri`）、V4L2 摄像头；
- WSL2 只做协议/算法验证，产品级采集与摄像头必须在原生 Linux/Windows。

### 取舍建议

- **不要**为了「兼容 WebRTC」而重写 Bento 成 RTP/RTCP：成本极高，且 Bento 的通道语义（SACK+BBR+FEC）已经够用。
- **要**补的是「安全 + 穿透 + tile + 码率自适应」四项，它们与是否兼容 WebRTC 无关，是远程桌面的共性需求。
- 若未来必须与浏览器互通，再单独做一层「WebRTC 网关」（libwebrtc 或 pion），把 Bento 当内部传输。

---

## 9. 风险与前置条件

| # | 风险 | 影响 | 缓解 |
|---|---|---|---|
| R1 | **`sudo` 需要密码**，agent 无法 `apt install` | 自研链接 `libvpx/libopus/libsrtp2` 被阻塞 | 用户执行一条 `apt install libvpx-dev libopus-dev libx264-dev libsrtp2-dev libavcodec-dev` |
| R2 | WSLg 抓屏性能未测 | 1080p30 可能 CPU 打满 | M0 阶段先 profile；必要时用 `XShmGetImage` 直连而非 ffmpeg |
| R3 | WSL2 的 NVENC 是私有注入 | 不能代表生产 Linux（服务器多无 GPU） | 编码层做抽象，同时保留 x264/VAAPI 后端 |
| R4 | Pulse 是 RDP 转发，延迟高 | 双向语音体验差 | 音频只做「可听」，实时通话需原生 Linux/Windows |
| R5 | MTU 1280 + `rmem_max` 208KB | 高码率视频丢包 | 调 `rmem_max`（需 root）+ 控制单包 ≤ 1200B（Bento 已如此） |
| R6 | 无 `CAP_NET_ADMIN` | `tc netem` 弱网测试仍 SKIP | 用用户态 UDP 代理（已有 `tests/bento/udp_proxy.hpp`） |
| R7 | AEC 自研不现实 | 双向语音啸叫 | 引入 `webrtc-audio-processing`（确认许可证） |
| R8 | 工作区「已有 P2P/PSK」是误记 | 排期与工作量估算会偏 | 本报告 §7 已纠正，需同步给决策方 |

---

## 10. 待确认问题

1. **目标是否要求与浏览器/标准 WebRTC 互通？** 若否，Bento 自研栈足够，不必碰 SDP/ICE 标准；若是，成本翻倍。
2. **远程桌面的目标平台**：只做 Linux→Linux，还是要 Windows↔Linux 互通？后者需要 RDP 或 WebRTC 网关。
3. **音频是否必须双向**？单向（只听）可跳过 AEC。
4. **是否接受 NVENC 作为主编码**？若产品要部署在无 GPU 的服务器，必须做 VAAPI/x264 后端。
5. **摄像头是否在首版范围**？WSL2 无法验证，需原生 Linux 机器。
6. **许可证**：`webrtc-audio-processing`（BSD 部分）、`libsrtp`（BSD）、`OpenH264`（BSD，但有 Cisco 专利条款）需逐个确认。

---

## 附录 A：关键命令清单（可复现）

```bash
# 设备
ls /dev/video* ; ls /dev/dri ; ls /dev/dxg ; ls /dev/snd

# 显示
echo $DISPLAY $WAYLAND_DISPLAY $PULSE_SERVER ; xrandr ; xdpyinfo -queryExtensions

# GPU
nvidia-smi --query-gpu=name,driver_version --format=csv

# 编码器
ffmpeg -hide_banner -encoders | grep -E "vpx|x264|aom|svt|opus|nvenc|vaapi|qsv"
ffmpeg -hide_banner -devices | grep -E "x11grab|kmsgrab|pulse|alsa"

# 抓屏
ffmpeg -f x11grab -framerate 5 -video_size 1280x720 -i :0.0 -frames:v 1 out.png

# 音频
parec --device=RDPSource --format=s16le --rate=44100 --channels=1 --raw out.raw

# 依赖
pkg-config --modversion vpx opus x264 srtp2
apt-cache policy libvpx-dev libopus-dev libsrtp2-dev
```

## 附录 B：证据索引（工作区）

- `include/go2cpp/bento/net/protocol.hpp` — 12B 帧头、7 种帧类型
- `include/go2cpp/bento/net/channel.hpp` — 三通道、SACK、PTO、视频分片、FEC 组
- `include/go2cpp/bento/net/congestion.hpp` — BBRv1 子集 + NewReno
- `include/go2cpp/bento/net/fec.hpp` — XOR + GF(256) RS
- `include/go2cpp/bento/net/link.hpp` — 单线程单 peer
- `include/go2cpp/bento/net/token_pairing.hpp` — relay 1:N 配对（非穿透）
- `docs/research/net-scenarios.md:163,174-179,279` — 「无 NAT 穿透」「S12 远程桌面缺口」
- `docs/modules/bento-roadmap.md:96-106` — 22 场景判定与 E1–E5 缺口
- `tests/bento/PROTOCOL.md` — 弱网测试基线（20% 丢包 + 200ms）
