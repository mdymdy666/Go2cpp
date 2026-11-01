# Go2Cpp 媒体 / WebRTC 实现 vs libwebrtc 官方对比

> 报告日期：2026-11-01
> 对比对象：`go2cpp::media`（X11 采集 → delta-i420 / H.264 → VideoChannel 1200B 分片 → UDP → 重组 / JitterBuffer → 解码 → 渲染）+ `SfuRouter` + `signaling`
> 参照系：libwebrtc（<https://webrtc.googlesource.com/src>）及其生态 SFU（mediasoup / Janus）
> 工作树：`/UserData/CodexWorkSpace/Go2cpp`（WSL2 / Ubuntu 24.04 / 12 vCPU / Ryzen 9 8945HX）
> 数据来源：我们的一侧全部来自仓内模块文档与本次同机实测；libwebrtc 一侧来自公开资料 + 本机可复现的「同源代码」实测（见 §6、§8）
> 立场：**本报告的首要目的是防止误读**。两台机器、两种算法、两种网络条件下的数字放在一张表里，很容易得出「快 100 倍」的结论；这个结论在大多数口径下是错的。

---

## 0. 结论速览（TL;DR）

1. **我们的 0.16–0.72 ms 与 libwebrtc 的 50–200 ms 不是同一把尺子**。
   前者是**本机回环、无相机、无公网、headless 渲染**的媒体处理开销；
   后者是**公网 + 真实相机（占 ~100 ms）+ 显示器 vsync** 的 glass-to-glass 延迟。
2. **同口径换算后**（都只算「采集后 → 渲染前」的机器内处理，不含相机与网络）：
   我们 0.16–0.72 ms，libwebrtc 约 **27–50 ms**（编码 10 + 抖动缓冲 7 + 解码 10 + 采集 2–5 + 渲染 0–17），
   差距约 **40–300×**，不是一个量级，但也不是「三个数量级」。
3. **真正领先的是三处「结构上的」优势**，而不是「实现更优」：
   - SFU 转发：**16.5 ns/帧** vs mediasoup 官方「单核 ~500 consumers」折算的 ~12.8 µs/包（约 **775×**）；
   - JitterBuffer：**Push 21.66 ns / Pop 3.13 ns**，零堆分配、零锁、零原子；
   - 本机媒体链路：**p50 < 1 ms**，因为整条链路里根本没有 DCT/熵编码。
4. **真正落后的是「能力面」而不是「性能面」**：
   编码器（无生产级 VP8/VP9/H.264）、公网适应（无 GCC/NACK/RTCP/PLI）、互通（无 RTP/SRTP/DTLS/ICE）、
   平台（仅 Linux WSL2）、分辨率（1080p 受 `rmem_max` 卡死）、音频（完全没有）。
5. **delta-i420 的 514× 不能与 VP8 的 185× 直接比**：一个是**无损差分**对静止桌面，
   一个是**有损变换编码**对运动画面；我们的噪声场景压缩比是 **1.004×**（等于不压缩），
   VP8 对同一画面仍有 185×。**内容与算法两个变量同时不同，任何单一倍数都无意义**。
6. 本次新增了 8 组同机实测（libvpx/x264/nvenc 的 1080p30 码率与 CPU、UDP 回环 RTT），
   使「编码 CPU」「压缩率」「网络下限」三个维度第一次有了**同一台机器上的可比数字**（§6）。

---

## 1. 口径声明（必读，先看这一节再看表）

### 1.1 三把不同的尺子

| 尺子 | 起点 | 终点 | 含什么 | 谁在用 |
|---|---|---|---|---|
| **A. 媒体链路** | `capture_ns`（采集完成） | 解码完成并交付渲染 | 编码 + 分片 + UDP 回环 + 重组 + 解码 | **我们**的 0.16–0.72 ms |
| **B. 传输增量** | 编码器输入 | 解码器输出 | 编码 + 打包 + 网络 + 抖动缓冲 + 解码 | libwebrtc 的「WebRTC 只加 ~10 ms」 |
| **C. glass-to-glass** | 镜头进光 | 显示器出光 | 相机曝光 + USB + 编码 + 网络 + 解码 + vsync | libwebrtc 的 50–200 ms |

我们的数字在 **A**，libwebrtc 常被引用的数字在 **C**。A 与 C 之间隔着相机（~100 ms）与显示（0–17 ms），
**这一段与软件实现无关**，却贡献了 C 的 60–80%。

### 1.2 我们的数字究竟是什么

`docs/modules/webrtc-media.md` §7 的口径原文：

> `steady_clock(解码完成并交付渲染) - capture_ns`，**单进程**，含内核 UDP 往返。

也就是说：

- **含**：X11 XShm 采集、delta-i420 编码、1200B 分片、`sendto`/`recvfrom` 回环、重组、delta 解码、headless 校验和；
- **不含**：公网/局域网 RTT、真实相机、显示器 vsync、SDL present；
- 渲染端默认是 `HeadlessRenderer`（只算校验和），**不是** `SDL_RenderPresent` + 60 Hz vsync。

后两点是这份对比里最容易被忽略、也最影响结论的两个「免费项」。

### 1.3 libwebrtc 的数字究竟是什么

目前能找到的最干净的公开实测是 Transitive Robotics 用**无限镜**做的 glass-to-glass 分解
（2026-05，Ubuntu 24.04，USB3 相机 30 fps，60 Hz 屏）：

| 配置 | 总延迟 | 相对「direct」的增量 |
|---|---:|---:|
| direct（相机直显，无编码无 WebRTC） | 110 ms | — |
| direct + H.264 软件编解码 | 130 ms | **+20 ms**（编码 10 + 解码 10） |
| **local WebRTC**（同机 WebRTC，H.264 软件） | **130 ms** | **+20 ms**（与上一行几乎相同） |
| remote WebRTC（经 TURN，ping RTT 30 ms） | 170 ms | +40 ms（其中 30 ms 是静态网络延迟） |

同一篇文章用 `chrome://webrtc-internals` 交叉验证：**本地 jitter buffer ~7 ms，远端 ~10 ms**。

这组数字的价值在于它把「WebRTC 本身」与「相机」剥离开了：
**WebRTC 的传输 + 抖动缓冲增量在完美网络下几乎为零（<1 ms 量级），
总增量 ~20 ms 全部来自软件编解码。**
来源：<https://transitiverobotics.com/blog/webrtc-latency-breakdown/>（本次已抓取全文，§8 逐条核对）。

### 1.4 为什么不能直接比（三条硬理由）

1. **网络项缺失**：我们的 UDP 只走 `127.0.0.1`，RTT 实测 p50 **12.08 µs**（§6.3）；
   公网 RTT 是 10–100 ms 量级，**差 3–4 个数量级**。
2. **相机项缺失**：WSL2 无 `/dev/video*`（调研已确认），我们从未接过真实相机。
   而公开的 glass-to-glass 数据里，相机 + USB 就占 ~100 ms。
3. **算法项不同**：delta-i420 是**无损差分**（块比较 + RLE），VP8/H.264 是**有损变换编码**。
   前者在静止桌面上几乎不要钱，在噪声画面上退化为 1:1；后者在两类内容上都有稳定压缩比。
   **同一维度上，「压缩率」这个指标对两者定义都不同。**

---

## 2. 对比总表

### 2.1 主表（任务要求的 8 个维度）

| 维度 | Go2Cpp | libwebrtc | 口径说明 |
|---|---|---|---|
| **延迟（本机回环）** | p50 **0.16–0.72 ms**、p99 **0.36–1.19 ms**（7 个场景，含 X11 真实采集） | **同口径推算 27–50 ms**；其中可实测的编解码增量 **~20 ms**、本地抖动缓冲 **~7 ms** | **必须分开**。我们在口径 A（不含相机/公网/vsync），libwebrtc 的 110–170 ms 在口径 C。换算见 §4 |
| **延迟（公网）** | **未测**（无公网路径、无第二台机器、无 `CAP_NET_ADMIN`） | 典型 **50–200 ms**；实测远端 170 ms（TURN 代理、RTT 30 ms） | 我们没有公网数据，**这一格不可比**，只能标「未测」 |
| **压缩率** | delta-i420 无损差分：静止桌面 18512B→**36B/帧（514×）**；X11 桌面 ~306B/帧；噪声 **1.004×**（不膨胀） | VP8 有损：1080p30 4 Mbps 目标 → 本机实测 **4053 kbps**（运动）/ **333 kbps**（静止）＝ 185× / 2260×（相对原始 I420 746 Mbps） | **算法不同**：无损差分 vs 有损变换。**内容也不同**：我们 320×240 桌面，VP8 实测 1080p 合成图。倍数不可直接比 |
| **编码 CPU** | delta 编码 = 16×16 块比较 + RLE，**未单独测 ns**（管线整体 0.16–0.72 ms/帧） | 软件 VP8 1080p30 **本机实测 0.60–0.82 核**（公开说法 1–2 核）；x264 veryfast 0.80 核；NVENC **0.14 核** | 同一台 Ryzen 9 8945HX、同一 libvpx 源码（§6.1）。我们的 delta 无 1080p 数据 |
| **SFU 转发** | **16.5 ns/帧**（2 订阅者、924B 分片、**60.5 M frames/s**）；真实 VideoChannel 路径 6.15–6.48 M fps | mediasoup 官方：单 worker（**单核**）约 **500 consumers**；按 1200B 包、1.5 Mbps/路折算 ≈ **12.8 µs/包** | 我们**不含** SRTP 加解密、RTCP、NACK 重传缓存、多线程锁；libwebrtc 本身不是 SFU |
| **JitterBuffer** | Push **21.66 ns**（1.44× `memcpy` 下限）、Pop **3.13 ns**；1e5 帧 ±50 ms 抖动 **100% 交付、0 违序**；内存常量 798,720 B | 帧级 jitter buffer（`RtpFrameObject` + 排序 map + 参考帧管理 + 解码顺序），**无公开 ns 基准**；本地缓冲延迟实测 **~7 ms** | 我们按**包序号**重排，libwebrtc 按**帧 + 参考帧完整性**重排。功能集合不同，ns 数不可直接相减 |
| **FEC** | RS **GF(256)** k≤64 / m≤16 + XOR(m=1)；5% 丢包 +FEC：p50 **131→38 ms（-71%）**，重传 **-27%** | **ULPFEC（RFC 5109）/ FlexFEC（RFC 8627）**，XOR 类；典型冗余 **10–30%** 带宽 | 我们的 RS 可纠 m 个**任意**丢失，ULPFEC 主要纠 1–2 个；但 ULPFEC 是标准、可互通。**开销未同口径测** |
| **信令** | SDP **子集**解析 **6.66 µs/次**（~20 行、严格行状态机、库内零分配） | 完整 SDP（RFC 4566 + 3264 + 8839 + trickle + simulcast/rid/extmap/rtcp-fb），**无公开基准** | 工作量不同：我们只认自己发的子集，libwebrtc 要处理任意对端 |
| **平台** | **Linux WSL2 only**（X11/WSLg、SDL2、无 `/dev/dri`、无相机、无音频） | **全平台**：Windows / macOS / Linux / Android / iOS，浏览器 + 原生 | — |

### 2.2 细分表：协议与能力覆盖

| 能力 | Go2Cpp | libwebrtc | 差距定性 |
|---|---|---|---|
| RTP / RTCP | ❌ 自研 Bento 帧（16B 头 + 8B 视频子头） | ✅ RFC 3550 全套 | **不互通** |
| SRTP / DTLS | ❌ 明文 UDP（仅回环测试） | ✅ DTLS-SRTP（BoringSSL + libsrtp） | 安全缺口 |
| ICE / STUN / TURN | ⚠️ 有 `p2p` 候选与优先级（RFC 8445 §5.1.2.1），无完整连通性检查 | ✅ 完整 | 半套 |
| NACK / PLI / FIR | ❌（SFU 明确不转发 ACK/FEC 片） | ✅ | 缺口 |
| 拥塞控制 | ⚠️ BBRv1 子集（`congestion.hpp`），媒体层**未接入** | ✅ GCC（trendline + transport-cc） | 算法不同且未闭环 |
| simulcast / SVC | ❌ | ✅ | 缺口 |
| 音频（Opus/AEC/NS/AGC） | ❌ 完全没有 | ✅ | 缺口 |
| 视频编码 | ⚠️ 无损 delta（生产可用）+ H.264（仅 ffmpeg 子进程探测） | ✅ VP8/VP9/H.264/AV1/H.265 | 能力缺口 |
| 抖动缓冲 | ✅ 包级、自适应、环形数组 | ✅ 帧级、参考帧感知 | 各有所长 |
| FEC | ✅ GF(256) RS + XOR | ✅ ULPFEC / FlexFEC | 算法更强，标准性更弱 |
| SDP 信令 | ✅ 子集（offer/answer/candidate/bye） | ✅ 完整 | 子集 |
| 加密 | ❌ | ✅ | 缺口 |

### 2.3 细分表：工程指标

| 指标 | Go2Cpp | libwebrtc | 说明 |
|---|---|---|---|
| 第三方依赖 | **0**（媒体层仅标准库 + X11 + 可选 SDL2） | 数十个（BoringSSL、libsrtp、libvpx、opus、abseil、protobuf…） | 构建体量差一个量级以上 |
| 许可证 | MIT | BSD-3（含第三方各自许可） | 兼容 |
| 构建 | CMake，单目录 `.build-bento-wsl` | GN + 平台工具链，Google 内部风格 | — |
| 热路径堆分配 | **0**（SFU/JitterBuffer 均有 objdump 证据） | 有（`RtpFrameObject` 引用计数 + 容器） | 见 `asm-sfu.md`、`jitter-degradation.md` §6 |
| 热路径锁/原子 | **0**（单线程模型） | 有（多线程 + 锁 + 原子） | 单线程是 ns 级的前提 |
| 代码规模 | 媒体 ~1 万行（含测试） | 数百万行 | — |
| 内存 | JitterBuffer 常量 **798,720 B**，1e6 帧后堆增长 1,216 B | 未测 | — |

---

## 3. 逐维度分析

### 3.1 延迟（本机 / 公网必须分开）

**我们的（口径 A，7 个场景，100/60/40/30/15 帧）**

| 场景 | p50 | p99 | max | fps |
|---|---:|---:|---:|---:|
| static 320×240 | 0.237 ms | 0.360 ms | 1.374 ms | 2038 |
| patch 320×240 | 0.254 ms | 0.984 ms | 1.245 ms | 1947 |
| scroll 256×192 | 0.162 ms | 0.698 ms | 0.698 ms | 2938 |
| noise 192×144 | 0.302 ms | 0.628 ms | 0.628 ms | 2464 |
| raw-i420 320×240 | 0.712 ms | 1.194 ms | 1.194 ms | 1028 |
| raw-bgra 160×120 | 0.411 ms | 0.643 ms | 0.643 ms | 1717 |
| **X11 真实采集 320×240** | **0.305 ms** | **1.003 ms** | 1.003 ms | 1163 |

区间：**p50 0.16–0.72 ms，p99 0.36–1.19 ms**。
（任务书摘要里出现的 `p50 0.31 ms`、`p99 1.58 ms` 与模块文档的逐场景表不完全一致，
本报告以 `docs/modules/webrtc-media.md` §7 的逐场景原始表为准；`0.31 ms` 接近 X11 真实采集的 p50，
`1.58 ms` 可能是另一轮含冷启动/调度抖动的 max 值，未在文档中找到对应行，**不采用**。）

**拆解（我们能确定的部分）**

| 段 | 量级 | 依据 |
|---|---:|---|
| UDP 回环 RTT（1200B） | **12.08 µs**（p50）/ 27.86 µs（p99） | 本次实测，含 Python 开销，是**上界**（§6.3） |
| 其余（采集 + delta 编解码 + 分片/重组 + 校验） | 剩余 ~0.15–0.70 ms | 由总延迟减去网络项反推 |

也就是说：**我们的端到端延迟里，网络只占 ~2–7%，其余全是媒体处理。**
这恰好说明「我们的低延迟来自『没做什么』（没有 DCT/熵编码、没有加密、没有 vsync），
而不是『把什么做得特别快』」——这是必须写清楚的诚实结论。

**libwebrtc 的（口径 C，公开实测）**

| 段 | 量级 | 来源 |
|---|---:|---|
| 相机 + USB | **~100 ms** | Transitive Robotics 无限镜实测 |
| H.264 软件编码 | ~10 ms | 同上（direct→direct+h264 的增量） |
| H.264 软件解码 | ~10 ms | 同上 |
| 本地 WebRTC 传输（含打包/抖动缓冲） | **~0 ms**（总增量与「仅编解码」几乎相同） | 同上 |
| 本地 jitter buffer 延迟 | **~7 ms**（`webrtc-internals`） | 同上 |
| 公网静态延迟 | 10–100 ms（实测案例 30 ms） | 同上 |
| 显示（60 Hz vsync） | 0–17 ms | 同上 |

于是：

- **口径 C（glass-to-glass，公网）**：110（直显）+ 20（编解码）+ 网络 → **130–170 ms**，与「50–200 ms」区间一致；
- **口径 B（传输 + 编解码增量，本机）**：**~20 ms**；
- **口径 A（我们的尺子，把相机换成抓屏、把 vsync 换成 headless）**：**~27–50 ms**（见 §4）。

### 3.2 压缩率（delta vs VP8，不同算法）

**我们的**

| 场景 | 输入 | 输出 | 倍数 | 性质 |
|---|---:|---:|---:|---|
| 静止桌面（合成） | 18512 B（128×96 I420 关键帧） | **36 B** | **514×** | 无损 |
| 静止桌面（X11 WSLg） | 9168 B / 30 帧 | ~306 B/帧 | — | 无损 |
| 噪声 192×144 | 41472 B/帧 | ≈ 1.004× | **1.004×** | 无损，**最坏情况不膨胀** |
| patch 320×240 | — | 2–5 KB/帧 | — | 无损 |

关键性质：**delta 是逐字节无损的**。解码结果与编码输入**完全相同**，可以逐帧
`memcmp` 断言（测试里就是这么做的，491 checks）。这对「远程桌面/远程操作」类场景是硬优势：
文字边缘、UI 控件、色块不会糊。

关键限制：**压缩率强烈依赖内容**。静止/低变化 → 514×；噪声 → 1.004×（等于没压缩）。
且**依赖参考帧**：丢一帧 → 差分链断裂 → 必须等下一个关键帧（默认 30 帧）自愈。

**libwebrtc 的（VP8，本机实测）**

| 内容 | 目标码率 | 实测码率 | 相对原始 I420（746 Mbps @1080p30） |
|---|---:|---:|---:|
| 运动合成图（testsrc2） | 4 Mbps | **4053 kbps** | **185×** |
| 运动合成图 | 2 Mbps | 2039 kbps | 366× |
| 静止彩条（smptebars） | 4 Mbps | **333 kbps** | **2260×** |
| 静止彩条 | 2 Mbps | 657 kbps（VBR 波动） | 1145× |

（libvpx `-deadline realtime -cpu-used 8`，300 帧 1080p30，§6.1）

**为什么不能直接比**

| 变量 | 我们 | VP8 |
|---|---|---|
| 算法 | 无损块差分 + RLE | 有损 DCT + 量化 + 熵编码 |
| 分辨率 | 320×240 / 256×192 | 1920×1080 |
| 内容 | WSLg 桌面（几乎静止） | 合成运动图 / 彩条 |
| 指标定义 | 「关键帧 → 差分帧」的比值 | 「原始像素 → 码流」的比值 |
| 失真 | **0** | PSNR 受量化影响 |

结论：**「514× vs 185×」这句话没有信息量**。有意义的两句话是：

1. 对**静止桌面**，delta 的 514× **优于** VP8 的 2260×？——不，2260× > 514×。
   但 514× 是**无损**的，2260× 是**有损**的，且 VP8 的 2260× 是 1080p 对 320×240 不可比。
2. 对**噪声画面**，delta 是 1.004×（不压缩），VP8 仍有 ~185×。**这才是 delta 的真实短板**：
   它把「压缩」这件事外包给了「画面本身变化小」这个前提。

### 3.3 编码 CPU

**公开说法**：WebRTC 软件编码 1080p30 约 **1–2 核**。
**本次同机实测**（Ryzen 9 8945HX，12 vCPU，300 帧 1080p30 = 10 s 视频，`/usr/bin/time`）：

| 编码器 | 内容 | 目标 | wall | user+sys | **折算核数** | 实测码率 |
|---|---|---:|---:|---:|---:|---:|
| libvpx VP8（4 线程） | 运动 | 4 Mbps | 3.80 s | 8.23 s | **0.82 核** | 4053 kbps |
| libvpx VP8（**单线程**） | 运动 | 4 Mbps | 5.39 s | 6.04 s | **0.60 核** | 4018 kbps |
| libvpx VP8（4 线程） | 静止 | 4 Mbps | 3.04 s | 5.89 s | 0.59 核 | 333 kbps |
| libx264 veryfast（4 线程） | 运动 | 4 Mbps | 2.82 s | 8.02 s | 0.80 核 | 4008 kbps |
| h264_nvenc p4 | 运动 | 4 Mbps | 1.56 s | 1.44 s | **0.14 核** | 4599 kbps |

三条结论：

1. **公开的「1–2 核」偏保守**：在 Zen4 上，libvpx realtime 编码 1080p30 只需 **0.6–0.8 核**。
   这修正了我们的参照系——比较编码成本时不能用一个高估的基线。
2. **单线程比四线程总 CPU 更低**（6.04 vs 8.23 CPU·s）：多线程有并行开销，
   1080p30 用单线程 5.39 s/10 s = **1.85× 实时**，足够。
3. **NVENC 是数量级差异**（0.14 核）。但 WSL2 的 NVENC 是私有注入通路，
   不能代表「服务器无 GPU」的生产场景（`webrtc-media.md` §8 已记录）。

**我们的**：delta 编码 = 16×16 宏块逐块 `memcmp`（编译器向量化）+ 脏块 RLE。
没有单独测 ns/帧，但从管线总延迟可反推**上限**：
static 320×240 全链路 0.237 ms/帧（2038 fps），其中 delta 编码只是其中一个环节，
**远低于毫秒级**。它没有「码率控制」概念，因此也没有「CPU 随码率/复杂度上升」的问题。

诚实差距：**我们的 delta 没有 1080p 数据**。I420 1080p 单帧 3.11 MB，
16×16 宏块数 8160 个，每帧要比较 8160 次 384B（亮度）+ 2×96B（色度）——
理论上是内存带宽受限（~3.1 MB/帧读 + 3.1 MB/帧参考读），
30 fps 下需 ~187 MB/s，单核可承受，但**未实测**（见 §7）。

### 3.4 SFU 转发

**我们的**（`docs/reports/asm-sfu.md`、`sfu-signaling.md` §8.4）

| 路径 | 样本 | 结果 | 折算 |
|---|---:|---|---:|
| `SfuRouter::ForwardFragment` 裸转发 | 1e6 帧 × 5 次 | 0.0165–0.0167 s | **16.5 ns/帧**（2 订阅者）、**60.5 M frames/s** |
| 优化前（通用 `ParseFrame`） | 同上 | 0.0278–0.0288 s | 35.6 M frames/s（优化后 **1.70×**） |
| `SfuPublisher → SFU → SfuSubscriber`（真实 VideoChannel） | 2 万帧 × 5 次 | 0.003 s | 6.15–6.48 M frames/s |

- 热函数静态指令 **347**，每帧动态指令 ≈ **263**，`lock`/原子 **0**，堆分配 **0**；
- 估算下限 42–62 cycles/帧，实测约 **1.2–1.8×** 下限（4.5 GHz 估算）；
- 零拷贝：924B 分片只读 24B 头，转发的是指针。按「拷贝给 2 个订阅者」折算，
  60.5 M fps ≈ **111.8 GB/s**，**超过单核 memcpy 物理上限**——这正是「只转指针」的意义。

**libwebrtc 生态的 SFU**

- libwebrtc **本身不是 SFU**，它提供的是「单 PeerConnection 的收发」。SFU 是 mediasoup / Janus / LiveKit 等。
- mediasoup 官方 <https://mediasoup.org/documentation/v3/scalability/>：
  > "a mediasoup C++ subprocess can typically handle **over ~500 consumers** in total"
  > 一个 Worker = **一个 CPU 核**；200–300 viewers（400–600 consumers）时单 router 可能到上限。
- **折算**：500 consumers × 假设 1.5 Mbps/路 = 750 Mbps ≈ 94 MB/s；
  按 1200B/包 → ~78,000 包/s → **≈ 12.8 µs/包·核**。
  这与「10–100 µs/帧级」的公开印象一致，且**这次是可倒推的、有出处的**。
- mediasoup 社区（官方论坛）明确说 500 只是「特定机器上的近似值」，不是保证值：
  <https://mediasoup.discourse.group/t/mediasoup-instance-deployement-in-aws/6022/4>

**差距的定性**

| 项 | 我们 | mediasoup | 倍数 |
|---|---:|---:|---:|
| 单包/帧转发 | **16.5 ns** | ~12.8 µs（倒推） | **~775×** |
| SRTP 加解密 | ❌ 无 | ✅ AES-GCM | 我们**省掉了最贵的一段** |
| RTCP / NACK 缓存 | ❌ 无 | ✅ | 同上 |
| 线程/锁 | 单线程无线程 | 多线程 + 锁 | 同上是 ns 级前提 |
| 互通 | ❌ | ✅ | — |

**所以 775× 里有相当大一部分是「我们没做那些事」，不是「我们做得更好」。**
公平的说法是：**在「只转发分片指针」这个被简化过的问题上，我们把常数压到了接近内存下限（1.2–1.8×）**；
而 mediasoup 在「完整 RTP + SRTP + NACK + 多线程」这个问题上，成本主要花在功能上。

### 3.5 JitterBuffer

**我们的**（`docs/modules/jitter-degradation.md` §2）

| 项 | 数值 | 说明 |
|---|---:|---|
| Push（1200B） | **21.66 ns** | 1.44× `memcpy(1200B)`=15.00 ns，2.0× 理论下限 |
| Pop（零拷贝视图） | **3.13 ns** | ~11 cycles，2.1× L1 下限 |
| 容器 | 环形数组 `slot = seq & (cap-1)` | Push/Pop 均摊 **O(1)** |
| 元数据 | 512 × 24B = 12 KB | 常驻 L1 |
| ±50 ms 抖动，1e5 帧 | **100.000% 交付、0 违序** | target 90.53 ms、J 35.26 ms |
| 内存 | 常量 **798,720 B**，1e6 帧后堆增长 1,216 B | 热路径零分配 |
| 指令/锁 | Push 189 条指令、4 call、**0 lock**；PopInternal **0 call** | objdump 证据 |
| 32 位回绕 | 600 帧跨 `0xFFFFFFFF→0`，零误判 | `int32_t(seq-next)` 差值 |

**libwebrtc 的**

- 帧级：`video_coding/jitter_buffer` 用 `RtpFrameObject`（引用计数）+ 按 timestamp 排序的容器 +
  帧完整性判断 + 参考帧管理 + `VCMEncodedFrame` 生命周期；
- 音频侧 NetEQ：官方/社区资料给出的默认值是 **min 0 ms / target 60 ms / max 2000 ms**
  （<https://omr.it.com/blog/jitter-buffers-webrtc-playout-delay-tuning/>，社区文章，非官方文档）；
- 实测缓冲延迟：**本地 ~7 ms、远端 ~10 ms**（`webrtc-internals`，Transitive Robotics）。

**对比的正确说法**

- 我们的 21.66 ns 是**「把一个包放进环形槽」**的成本；
  libwebrtc 的对应操作是**「把一个包挂到帧对象上，判断帧是否完整，维护参考帧链」**。
  两者不是同一操作，**不能相减**。
- 我们的算法是 RFC 3550 抖动估计（EWMA，`J += (|D|-J)/16`）+ `target = clamp(base + 2J, 10, 400ms)`；
  libwebrtc 的做法更复杂（帧级 + 解码顺序 + 丢帧策略），但**没有公开的 ns 级基准**可引。
- **我们的优势是「可测、可证、可复现」**：有 objdump、有下限推导、有 1e6 帧压测；
  libwebrtc 这一层几乎没有公开的同类证据。这本身是工程文化差异，不是性能差异。

### 3.6 FEC

**我们的**

- `fec.hpp`：XOR（m=1）与 **GF(256) Reed-Solomon（k≤64, m≤16）**；
- 效果：5% 丢包 + FEC 后 p50 **131 → 38 ms（-71%）**，重传 **-27%**；
- 机制优势：RS 可恢复 **m 个任意位置**的丢失，不要求丢失连续或对齐；
- 机制代价：编解码是 GF(256) 乘加，**本次没有单独测 ns/包**（见 §7）。

**libwebrtc 的**

- **ULPFEC（RFC 5109）**：XOR 型，通常 1–2 个包的冗余，能恢复单个丢失（或特定组合）；
- **FlexFEC（RFC 8627）**：更灵活的 XOR 交织，可按 SSRC 保护；
- 典型冗余开销 **10–30% 带宽**（经验值，非本次实测）；
- 优势：**标准、可与浏览器互通、与 NACK 协同**（先 NACK 后 FEC 的混合策略）。

**对比的正确说法**

| 项 | 我们（RS GF(256)） | libwebrtc（ULPFEC/FlexFEC） |
|---|---|---|
| 纠错能力 | m 个任意丢失 | 主要 1–2 个（交织后可更多） |
| 计算成本 | GF(256) 乘加，较高 | XOR，极低 |
| 带宽开销 | 取决于 m/k | 10–30% |
| 标准性 | 自研线格式 | RFC 5109 / 8627，可互通 |
| 与重传协同 | 有（-27% 重传） | 有（NACK + FEC） |

结论：**算法能力我们更强，工程成熟度与互通性他们更强**。
而且「-71%」这个数字是**我们的场景**（5% 随机丢包、模拟到达序列），
不能直接搬到公网（公网是突发丢包 + 重排 + 抖动混合）。

### 3.7 信令

**我们的**（`docs/reports/asm-signaling.md`、`sfu-signaling.md` §8.5）

| 指标 | 数值 |
|---|---:|
| SDP 解析 | **6.66 µs/次** |
| 热函数静态指令 | 1,609 |
| 每次动态指令（自身） | ≈ 8,050 |
| 每次动态指令（含子调用） | ≈ 134,600 |
| `lock`/原子 | 0 |
| 库内堆分配 | 0 |
| 估算下限 | 1.5–3 µs（分配主导） |
| 与下限差距 | 2.2–4.4×（控制面，可接受） |

- 只支持 `v=0`、固定顺序序列化（可逐字节断言）、严格行状态机；
- 未知 `a=` 属性忽略，已知但非法的整体失败；
- 候选优先级严格按 RFC 8445 §5.1.2.1 计算（host 2130706431 / prflx 1845493759 / srflx 1694498815 / relay 16777215）；
- 测试：500 次 SDP 字节变异（189 合法 / 311 拒绝）、1000 个随机候选往返、300 段随机字节串不崩溃。

**libwebrtc 的**

- 完整 SDP：RFC 4566（SDP）、RFC 3264（offer/answer）、RFC 8839（ICE 属性）、
  trickle ICE、`a=rid`/simulcast、`a=extmap`、`a=rtcp-fb`、BUNDLE、rtcp-mux；
- 解析器要处理任意对端（Chrome/Firefox/Safari/各种 MCU）的输出，容错面大得多；
- **没有公开的 µs 级基准**（这不是社区关注点）。

结论：**6.66 µs 与「libwebrtc 的 SDP 解析」不可比**——我们只解析自己序列化的 ~20 行子集，
对方要解析任意第三方生成的完整 SDP。这是「子集 vs 全集」的差距，不是「快 vs 慢」。
真正可比的结论只有一个：**在「控制面 6.66 µs」这个量级上，建连耗时可忽略**
（相比 ICE 打洞的 10–100 ms），所以这块的优化意义本身有限。

### 3.8 平台

| 项 | Go2Cpp | libwebrtc |
|---|---|---|
| 操作系统 | **仅 Linux（WSL2 验证）** | Windows / macOS / Linux / Android / iOS |
| 采集 | X11 XShm（WSLg）；无相机（`/dev/video*` 不存在）；无 Wayland/DRM | V4L2 / AVFoundation / DirectShow / ScreenCaptureKit / X11 / PipeWire |
| 硬件编码 | NVENC（WSL 私有注入）；**无 VAAPI**（无 `/dev/dri`） | VAAPI / QSV / NVENC / VideoToolbox / MediaCodec |
| 渲染 | SDL2（可选）+ Headless | 各平台原生 + OpenGL/Vulkan/Metal |
| 音频 | **无**（仅 PulseAudio 可用，未接入） | Opus + AEC3/NS/AGC |
| 网络 | UDP 直连（`127.0.0.1`）；无 NAT 穿透、无公网验证 | ICE/STUN/TURN 全套 |

这一维度我们**全面落后**，且不是「实现问题」，是「范围问题」：
本模块的定位是 WSL2 内的最小闭环（`webrtc-media.md` §1 明确定位为「不是完整的 WebRTC 实现」）。

---

## 4. 同口径换算（本报告的核心）

### 4.1 问题：本机回环下 libwebrtc 到底是多少？

公开资料里几乎没有「libwebrtc 在本机回环、不含相机」的单一数字，但有可拆解的实测：

- 无限镜实测：direct 110 ms，local WebRTC 130 ms → **WebRTC 增量 20 ms**（= 编码 10 + 解码 10）；
- `webrtc-internals`：本地 jitter buffer **~7 ms**；
- 相机 + USB ~100 ms（可整体扣除）；
- 显示器 vsync 0–17 ms（headless 时可扣除）。

据此组装「口径 A」（采集后 → 渲染前，本机，无相机，无 vsync）：

| 段 | 保守 | 激进 | 依据 |
|---|---:|---:|---|
| 采集（X11 抓屏 + 格式转换） | 5 ms | 2 ms | 与我们的 XShm 同量级；无实测，标**推算** |
| 编码（VP8 软件，1080p30） | 10 ms | 10 ms | 无限镜实测 |
| 打包 + SRTP + UDP 回环 | 0.1 ms | 0.01 ms | 与我们的 12 µs 同量级 |
| 抖动缓冲 | 7 ms | 3 ms | `webrtc-internals` 本地 7 ms；理想网络可更低 |
| 解码（VP8 软件） | 10 ms | 5 ms | 无限镜实测 10 ms |
| 渲染（headless，不含 vsync） | 2 ms | 1 ms | 推算（纹理上传/拷贝） |
| **合计** | **~34 ms** | **~21 ms** | |

取 **27–50 ms** 作为区间（下界放宽到 21 ms，上界含 1080p 编码抖动与内存拷贝）。

### 4.2 换算表（同等口径）

| 口径 | Go2Cpp | libwebrtc（换算） | 倍数 | 可信度 |
|---|---:|---:|---:|---|
| **A. 采集后→渲染前（本机，headless）** | **0.16–0.72 ms** | **27–50 ms** | **~40–300×** | 我们：实测；对方：**推算**（分段有实测支撑） |
| B. 仅「编码 + 解码」 | delta ≈ µs 级（未单测） | **~20 ms** | **>1000×** | 我们未单测；对方实测 |
| C. 传输 + 抖动缓冲（本机） | **12 µs**（UDP RTT） | **~7 ms**（JB）+ ~0（网络） | **~580×** | 均为实测/换算 |
| D. 公网 glass-to-glass | **未测** | **130–170 ms**（实测）；行业 50–200 ms | **不可比** | 我们无数据 |
| E. 1080p30 单帧编码 CPU | 未测（无 1080p delta 数据） | **0.60–0.82 核**（本机实测） | 不可比 | 对方实测 |

### 4.3 换算的不确定性（必须写在结论旁边）

1. **我们的 0.16–0.72 ms 不含 vsync**。若换成真实 `SDL_RenderPresent` + 60 Hz vsync，
   平均要再加 **0–16.7 ms**（取决于相位）。这一项就能把「40–300×」压到「1–3×」。
   **这是整份报告里对结论影响最大的单一因素。**
2. **我们的渲染是 headless 校验和**，不是纹理上传 + GPU 合成 + 扫描输出。
   真实渲染的成本我们没有测过。
3. **libwebrtc 的 27–50 ms 是推算**，不是任何人跑出来的一次测量。
   分段依据（编码 10、JB 7、解码 10）有实测，采集与渲染两段是估计。
4. **分辨率不同**：我们 320×240 / 256×192 / 192×144，对方 1080p。
   编码/解码成本对分辨率近似线性，若我们也做 1080p，delta 的块比较成本会上升
   （见 §3.3 的带宽估算），但**不会**上升到 20 ms 量级。
5. **算法不同**：delta 是无损差分，VP8 是有损变换。这是「快」的根本原因，
   也是「压缩率/画质不可比」的根本原因。**同一个差异同时解释了两件事。**

---

## 5. 结论：领先、落后、不可比

### 5.1 我们领先的（且经得起追问）

| # | 维度 | 证据 | 领先的**真实原因** |
|---|---|---|---|
| 1 | **SFU 转发** | 16.5 ns/帧、60.5 M fps、1.2–1.8× 下限、零拷贝零锁零分配 | 只转分片指针，不做 SRTP/RTCP/NACK；单线程 |
| 2 | **JitterBuffer** | Push 21.66 ns（1.44× memcpy）、Pop 3.13 ns、1e5 帧 100% 交付 | 包级环形数组、按 seq 定位 O(1)、常量内存 |
| 3 | **本机媒体链路延迟** | p50 0.16–0.72 ms、p99 0.36–1.19 ms | **无损差分替代 DCT**，链路里没有编码器 |
| 4 | **最坏情况有界** | 噪声场景 1.004×，不膨胀 | 差分编码的天然性质 |
| 5 | **可验证性** | 逐帧 `memcmp` 断言（1102 checks）、objdump 下限推导、1e6 帧压测 | 无损 + 零分配 + 单线程 ⇒ 结果确定 |
| 6 | **依赖与体量** | 媒体层 0 第三方依赖、MIT、单目录 CMake | 只做自己需要的子集 |

### 5.2 我们落后的（能力面，不是性能面）

| # | 维度 | 差距 | 性质 |
|---|---|---|---|
| 1 | **视频编码** | 无生产级 VP8/VP9/H.264；H.264 仅 ffmpeg 子进程探测（PSNR 37 dB 但未接入管线） | **能力缺口** |
| 2 | **公网适应** | 媒体层未接 BBR/GCC；无 NACK/PLI/FIR/RTCP | **能力缺口**（算法有，未闭环） |
| 3 | **互通** | 无 RTP/SRTP/DTLS/ICE，不能与浏览器或任何 WebRTC 端点通话 | **协议缺口** |
| 4 | **平台** | 仅 Linux WSL2；无 Windows/macOS/移动端 | **范围缺口** |
| 5 | **分辨率** | 1080p 受 `rmem_max=208KB` 与「单帧 256 片 = 300KB」双重限制 | **工程缺口**（可修） |
| 6 | **音频** | 完全没有（Opus/AEC/NS/AGC 均无） | **能力缺口** |
| 7 | **安全** | 明文 UDP；`a=fingerprint` 只透传，无 DTLS 握手/SRTP | **安全缺口** |
| 8 | **测试方法** | 无真实相机、无无限镜、无公网、无 `tc netem` | **方法缺口**（环境所限） |

### 5.3 不可比的（不要写进任何宣传材料）

- 我们的 0.16 ms vs libwebrtc 的 130 ms —— 口径 A vs 口径 C，**差一个相机**；
- 我们的 514× vs VP8 的 185× —— 无损 vs 有损、静止 vs 运动，**两个变量同时不同**；
- 我们的 6.66 µs SDP vs libwebrtc 的 SDP —— 子集 vs 全集；
- 我们的 16.5 ns vs mediasoup 的 12.8 µs —— 无 SRTP/RTCP/NACK vs 有。

### 5.4 一句话总结

> **在「自己定义问题、自己定义协议、自己定义渲染终点」的这条链路上，
> 我们把常数压到了接近硬件下限（1.2–1.8×），这是真实的工程成绩；
> 但这条链路比 libwebrtc 少了相机、网络、加密、反馈、互通、平台与音频——
> 它证明了「媒体四段可以在 WSL2 内闭环且可量化」，
> 不能证明「我们比 libwebrtc 快 100 倍」。**

---

## 6. 本次新增实测（可复现，2026-11-01）

### 6.1 编码器同机基准（libwebrtc 的 VP8/H.264 后端）

环境：Ryzen 9 8945HX（12 vCPU，Zen4）、WSL2 Ubuntu 24.04、ffmpeg 6.1.1、
`libvpx`（VP8 realtime，`-cpu-used 8`）、`libx264 -preset veryfast`、`h264_nvenc -preset p4`。
输入：`lavfi` 合成源 1920×1080@30，300 帧（10 s），`-pix_fmt yuv420p`。

```bash
# 运动画面
ffmpeg -f lavfi -i testsrc2=size=1920x1080:rate=30 -frames:v 300 -pix_fmt yuv420p \
  -c:v libvpx -b:v 4M -deadline realtime -cpu-used 8 -threads 4 -benchmark -y /tmp/o.ivf
# 静止画面
ffmpeg -f lavfi -i smptebars=size=1920x1080:rate=30 -frames:v 300 -pix_fmt yuv420p \
  -c:v libvpx -b:v 4M -deadline realtime -cpu-used 8 -threads 4 -benchmark -y /tmp/o.ivf
# CPU 用 /usr/bin/time -f "wall=%e user=%U sys=%S" 采集
```

结果见 §3.3。**注意**：`-benchmark` 在 `-loglevel error` 下不打印 `utime`，
本次用 `/usr/bin/time` 采集（第一次尝试用 `-benchmark` 只拿到 `maxrss`，已修正）。

### 6.2 原始帧基准

- I420 1080p 单帧 = **3,110,400 B**（2.97 MB）；
- BGRA 1080p 单帧 = **8,294,400 B**；
- 1080p30 I420 裸码率 = **746 Mbps**（这是 §3.2 里 185×/2260× 的分母）。

### 6.3 UDP 回环 RTT（网络项下限）

```python
# server: bind 127.0.0.1:0；client 发 1200B，server 原样回；N=20000
```

| 指标 | 数值 |
|---|---:|
| p50 | **12.08 µs** |
| p90 | 12.75 µs |
| p99 | 27.86 µs |
| max | 414.40 µs |

含 Python 解释器开销，是**上界**；真实内核 UDP 回环往返应更低（C 实现可到个位数 µs）。
**用途**：证明我们 0.16–0.72 ms 的端到端里，网络项只占 2–7%。

### 6.4 对既有数据的三点修正

1. **「软件编码 1–2 核/1080p30」偏保守**：本机实测 0.60–0.82 核（libvpx realtime）。
2. **「本地 WebRTC 延迟」不是 0**：编解码 ~20 ms 是真实成本，只是被相机的 100 ms 掩盖了。
3. **我们的网络项确实极低**：12 µs，比任何公网场景低 3–4 个数量级。

---

## 7. 未测清单（哪些数据没有）

| # | 缺什么 | 为什么缺 | 影响 |
|---|---|---|---|
| 1 | **公网/局域网端到端延迟** | 无第二台机器、无公网路径；`p2p` 只有回环 | §2.1「延迟（公网）」格只能写「未测」 |
| 2 | **真实相机采集延迟** | WSL2 无 `/dev/video*` | 无法与 glass-to-glass 数据对齐 |
| 3 | **SDL 真实渲染 + vsync 延迟** | 默认 headless | §4.3 最大不确定项 |
| 4 | **1080p 端到端** | `rmem_max=208KB` + 单帧 256 片限制 | 我们所有延迟数据都在 ≤320×240 |
| 5 | **delta 在 1080p 的压缩率与耗时** | 同上 | §3.2/§3.3 只能给 320×240 与理论上限 |
| 6 | **FEC 编解码 ns/包** | 本次未测 | 无法与 ULPFEC 的 XOR 成本对比 |
| 7 | **SFU >2 订阅者 / 多房间并发** | 压测只做 2 订阅者 | 16.5 ns 是 2 订阅者的数字 |
| 8 | **JitterBuffer + 真实 UDP 弱网联合压测** | 无 `CAP_NET_ADMIN`，`tc netem` 不可用；只有模拟到达序列 | 抖动数据是合成的 |
| 9 | **BBR 在媒体管线的端到端闭环** | 媒体层未接入 `congestion.hpp` | 「+FEC -71%」是独立实验，不是管线数据 |
| 10 | **完整 SDP（simulcast/rid/extmap）解析耗时** | 我们只实现子集 | 6.66 µs 只对子集成立 |
| 11 | **libwebrtc 同机 A/B** | 未安装（GN 构建 + 依赖体积） | 只能引用公开实测 + 自己换算 |
| 12 | **多线程/多核扩展性** | 单线程模型 | ns 级数字的前提是单线程 |
| 13 | **功耗 / 内存带宽 / 缓存行为** | 未测 | 1080p 时可能成为瓶颈 |
| 14 | **音频链路** | 完全没有实现 | 无法对比 Opus/AEC |
| 15 | **安全开销（DTLS/SRTP）** | 未实现 | mediasoup 的 12.8 µs 里含这段，我们不含 |

---

## 8. 来源与可信度分级

### 8.1 分级定义

| 级别 | 定义 | 本报告中的例子 |
|---|---|---|
| **A. 一手/可复现** | 本次会话实测，或仓内文档中带命令与原始输出的数据 | §6 全部；`webrtc-media.md` §7；`asm-sfu.md`；`jitter-degradation.md` §6 |
| **B. 官方文档/标准** | 厂商官方文档、RFC 原文 | mediasoup Scalability；RFC 5109/8627/3550/4566/8445 |
| **C. 第三方实测/社区** | 有方法学描述的博客、论坛回答 | Transitive Robotics 无限镜；omr.it jitter buffer 文章；mediasoup 论坛 |
| **D. 经验值/未核实** | 行业口口相传、无法定位单一出处 | 「软件编码 1–2 核/1080p30」；「ULPFEC 10–30% 开销」；「公网 50–200 ms」 |

### 8.2 逐条来源

| # | 内容 | 级别 | 来源 |
|---|---|---|---|
| 1 | glass-to-glass 分解（110/130/130/170 ms；编码解码各 10 ms；相机 100 ms；JB 7/10 ms） | **C** | <https://transitiverobotics.com/blog/webrtc-latency-breakdown/>（本次抓取全文，2026-05-06） |
| 2 | mediasoup「单核 ~500 consumers」、200–300 viewers 上限 | **B** | <https://mediasoup.org/documentation/v3/scalability/>（本次抓取全文） |
| 3 | 「500 只是近似值」 | **C** | <https://mediasoup.discourse.group/t/mediasoup-instance-deployement-in-aws/6022/4>（本次抓取） |
| 4 | NetEQ 默认 min 0 / target 60 / max 2000 ms；抖动 5–15 ms(Wi-Fi)/40–80 ms(LTE) | **C** | <https://omr.it.com/blog/jitter-buffers-webrtc-playout-delay-tuning/>（本次抓取全文） |
| 5 | ULPFEC | **B** | RFC 5109 <https://www.rfc-editor.org/rfc/rfc5109>（标准文本，本次未逐字抓取） |
| 6 | FlexFEC | **B** | RFC 8627 <https://www.rfc-editor.org/rfc/rfc8627>（同上） |
| 7 | RTP/RTCP 与抖动估计 | **B** | RFC 3550 <https://www.rfc-editor.org/rfc/rfc3550>（同上） |
| 8 | SDP / offer-answer / ICE 属性 | **B** | RFC 4566 / 3264 / 8839 / 8445 / 5763 |
| 9 | SRTP | **B** | RFC 3711 <https://www.rfc-editor.org/rfc/rfc3711> |
| 10 | libwebrtc 源码 | **B** | <https://webrtc.googlesource.com/src> |
| 11 | 「公网 50–200 ms」 | **D** | 行业区间，无单一权威出处；本次实测 170 ms 落在其中 |
| 12 | 「软件编码 1–2 核/1080p30」 | **D** | 经验值；本次实测 0.60–0.82 核（§6.1），**已修正** |
| 13 | 「SFU 转发 10–100 µs/帧级」 | **D→B** | 本次由 mediasoup「500 consumers/核」倒推得 **~12.8 µs/包**，升级为有出处的推算 |
| 14 | 我们全部数据 | **A** | `docs/modules/webrtc-media.md`、`sfu-signaling.md`、`jitter-degradation.md`、`docs/reports/asm-sfu.md`、`asm-signaling.md` |

### 8.3 网络可达性说明（诚实记录）

本次会话中，`app.unpkg.com`、`transitiverobotics.com`、`omr.it.com`、`mediasoup.org`、
`mediasoup.discourse.group` 可正常抓取；但
`webrtc.googlesource.com`（超时）、`raw.githubusercontent.com`（被安全策略拦截：域名解析到内网）、
`en.wikipedia.org` / `webrtcforthecurious.com`（fetch/DNS 失败）、`ar5iv.labs.arxiv.org`（503）、
`aicassindra.com`（超时）均不可达。
因此 RFC 类来源只做「标准存在性引用」，未在本次逐字核验条款；
libwebrtc 源码级常量（如 playout delay 上下限）**本次未从源码核实**，报告中一律标注为「未核实」。

---

## 9. 复现命令

```bash
# 我们的数据（仓内）
cmake -S . -B .build-bento-wsl -DGO2CPP_WITH_BENTO=ON -DGO2CPP_WITH_MEDIA=ON -DGO2CPP_WITH_P2P=ON
cmake --build .build-bento-wsl -j8
ctest --test-dir .build-bento-wsl -R "media_" --output-on-failure

taskset -c 2 .build-bento-wsl/media_test_jitter_buffer   # Push 21.66 / Pop 3.13 ns
taskset -c 2 .build-bento-wsl/media_test_sfu             # 16.5 ns/帧
taskset -c 2 .build-bento-wsl/media_test_e2e             # p50/p99 逐场景

# 反汇编证据
objdump -d --no-show-raw-insn .build-bento-wsl/CMakeFiles/go2cpp_media.dir/src/media/jitter_buffer.cpp.o
objdump -d --no-show-raw-insn .build-bento-wsl/CMakeFiles/go2cpp_media.dir/src/media/sfu.cpp.o

# libwebrtc 后端的同机实测（§6.1）
ffmpeg -hide_banner -encoders | grep -E 'libvpx|libx264|nvenc'
/usr/bin/time -f "wall=%e user=%U sys=%S" \
  ffmpeg -f lavfi -i testsrc2=size=1920x1080:rate=30 -frames:v 300 -pix_fmt yuv420p \
  -c:v libvpx -b:v 4M -deadline realtime -cpu-used 8 -threads 4 -y /tmp/o.ivf
```

---

## 10. 一句话给决策者

> 我们的实现证明的是「**在剥离了相机、网络、加密、互通与平台之后，媒体链路的常数可以压到硬件下限附近**」；
> libwebrtc 证明的是「**在保留上述全部要素的前提下，glass-to-glass 可以做到 130–170 ms**」。
> 这两句话都是真的，**但它们不是同一句话**。任何把它们放进同一张表、
> 却不标注口径的对比，都是在制造误导。
