# 模块文档：音频链路（audio）

> **状态：现行（2026-11-01）。** 对应 WebRTC 媒体链路中「音频」这一段的
> 采集、编码、RTP 风格分片、传输、抖动缓冲、解码与播放。本文解释设计取舍、
> API 契约、验收数据与已知限制；代码与注释是事实来源，本文与代码冲突时以代码为准。
>
> 工作树：`/UserData/CodexWorkSpace/Go2cpp`；构建目录：`.build-bento-wsl`
> （Release，Ninja，g++ 13.3.0，WSL2 / Ubuntu 24.04，Ryzen 9 8945HX 12 vCPU）。

---

## 1. 概述与边界

既有 `go2cpp::media` 已经有「X11 采集 → 编码 → VideoChannel 1200B 分片 → UDP →
重组 → 解码 → 渲染」的视频闭环（`docs/modules/webrtc-media.md`）。本模块补上
音频，**不修改任何现有文件**，只新增：

| 路径 | 内容 | 规模 |
|---|---|---|
| `include/go2cpp/media/audio.hpp` | 格式/帧、采集、编码、RTP、抖动缓冲、UDP、播放、会话 | 约 1000 行 |
| `src/media/audio.cpp` | 上述全部实现 + G.711 基线/查表 A/B + Opus dlopen | 约 1900 行 |
| `tests/media/test_audio.cpp` | 12 组测试（含 1e5 帧一致、1e7 压测） | 约 900 行 |
| `examples/audio_e2e/` | 5 场景端到端演示（会话统计 + WAV/JSON 产物） | 4 文件 |
| `cmake/Go2CppMedia.cmake` | 追加 `audio.cpp` 与 `media_test_audio` 目标 | 追加 7 行 |

**做**：

- 采集：PulseAudio（`parec` 子进程）、ALSA（`arecord` 子进程）、确定性合成源；
- 编码：Opus（`dlopen("libopus.so.0")`，无 dev 头也能用）、G.711 µ-law/A-law、
  PCM 直通；统一 `AudioEncoder`/`AudioDecoder` 接口，解码器支持 PLC；
- 传输：RTP 固定头（seq/timestamp/SSRC/PT/M）+ 4B 分片头 + MTU 1200 分片；
  UDP 回环收发；进程内链路（可注入抖动/丢包）；
- 抖动缓冲：按扩展序号重排、去重、丢包判定、自适应深度（EWMA 抖动估计）；
- 播放：PulseAudio（`pacat` 子进程）、ALSA（`aplay`）、空后端；
- 端到端会话：`AudioSession`（单线程 pump），统计采集/编码/分片/重组/解码/播放/
  PLC/丢包与延迟分位数。

**不做（明确留给上层或后续）**：

- RTCP / NACK / 自适应码率（只提供丢包计数与 PLC）；
- 与标准 RTP 的**双向**互通：本实现的分片头是私有扩展，标准解码器需要网关剥离
  4 字节（见 §6）；
- 回声消除（AEC）、噪声抑制（NS）、自动增益（AGC）；
- 多线程/锁：与既有媒体模块一致，全部单线程 pump 模型；
- 音频设备枚举/热插拔：设备名由调用方传入（空 = Pulse 默认源/宿）。

---

## 2. 数据流

```
AudioSource ──s16le PCM──► AudioEncoder ──payload──►
AudioRtpPacketizer（12B RTP 头 + 4B 分片头 + 数据，MTU 1200）──►
UdpAudioSender ──UDP 回环──► UdpAudioReceiver ──►
AudioJitterBuffer（seq 重排/去重/丢包）──►
AudioRtpReassembler（timestamp 拼分片）──► AudioDecoder（PLC）──► AudioSink
```

`AudioSession` 把上述各段组装成单线程会话：`SendNext()` 采集并发送一帧，
`Pump()` 消费已到达的包并写播放后端，`Run(n)` 循环两者。

### 2.1 线格式（全部大端）

RTP 固定头 12 字节（RFC 3550 最小子集：V=2、P=0、X=0、CC=0）：

| 偏移 | 字段 | 类型 | 语义 |
|---|---|---|---|
| 0 | flags | u8 | `0x80`（V=2） |
| 1 | pt/m | u8 | bit7 = marker（本实现=「帧最后一片」），bit0..6 = PT |
| 2 | seq | u16 | 包序号（回绕） |
| 4 | ts | u32 | 时间戳（**源采样率时钟**，见 §2.2） |
| 8 | ssrc | u32 | 同步源 |
| 12 | frag | u16 | 分片序号（0 起，本项目扩展） |
| 14 | total | u16 | 分片总数（1 = 不分片） |
| 16 | data | ... | 编码负载 |

`frag/total` **总是存在**：音频帧可能超过 1188B（例如 20ms@48kHz 双声道 s16le =
3840B），接收端按 timestamp 无歧义重组。缺这 4 字节、或字段非法的包一律记
`malformed` 丢弃——不做「裸包启发式识别」，避免把标准 Opus 负载的前 4 字节误判
成合法分片头。

### 2.2 时间戳口径

`timestamp` 用源采样率时钟（44.1kHz → 每 10ms 帧 +441），而不是 Opus 规定的
48kHz 时钟。接收端只用它做「同一帧的分片分组」与乱序排序，不用于解码时钟；
用源时钟可以让「采集帧号 ↔ timestamp」保持整数关系，便于逐帧校验。
与标准 RTP 互通时需要按 48k 重标（`ts48 = ts44.1k * 48000 / 44100`）。

---

## 3. 采集

| 后端 | 实现 | 可用性（WSL2 实测） |
|---|---|---|
| PulseAudio | `parec --format=s16le --rate=R --channels=C --raw --latency-msec=L [--device=D]` 子进程 + 管道 | **可用**：`/mnt/wslg/PulseServer`，默认源 `RDPSource`（s16le 1ch 44.1kHz） |
| ALSA | `arecord -q -t raw -f S16_LE -r R -c C` 子进程 | **不可用**：`libasound.so.2` 在，但 `arecord` 未安装（alsa-utils 缺失） |
| 合成 | 正弦/扫频/噪声/DTMF，4096 项正弦表 + 32 位定点相位 | 恒可用（测试与 CI 的默认源） |

为什么走子进程：与 `ffmpeg_codec.cpp` 同样的取舍——WSL2 没有 `libpulse-dev`，
但 Pulse CLI 齐全；零链接依赖，代价是每 10ms 一次 882B 管道拷贝（可忽略）。

子进程封装用 `posix_spawnp` + `posix_spawn_file_actions`（不是 `fork`，避免多线程
环境下 fork 的锁问题）。读路径 `poll + read` 循环、总超时 2s；写路径
`poll + write` 循环、总超时 1s。

**SIGPIPE**：`pacat` 提前退出时写管道会收到 `SIGPIPE` 杀掉进程。`PipeProcess` 在
第一次创建写端时用 `std::call_once` 把 `SIGPIPE` 设为 `SIG_IGN`（进程级副作用，
文档标注；写失败以 `EPIPE` 返回 false）。这是服务端程序常见做法，但调用方若
依赖默认 SIGPIPE 行为需要自行恢复。

合成源是**确定性**的（同一帧号产生同一 PCM），因此可以：
- 1e5 帧逐字节比对；
- 固定种子的噪声做最坏情况带宽测试；
- 扫频（非周期）用于 Opus 延迟对齐。

---

## 4. 编码

| 编码 | 负载 | 说明 |
|---|---|---|
| `kOpus` | 40B/10ms @32kbps | `dlopen("libopus.so.0")` + 手写 8 个符号原型；48kHz 内部时钟 |
| `kPcmu` | 1B/样本 | G.711 µ-law，**查表**实现（`kUlawEncode`/`kUlawDecode` constexpr 表） |
| `kPcma` | 1B/样本 | G.711 A-law，公式实现（不是性能重点） |
| `kPcmS16LE` | 2B/样本 | 直通，逐字节可校验 |

### 4.1 Opus 为什么用 dlopen

本机 `pkg-config --exists opus` 为假（无 `opus/opus.h`），但 `libopus.so.0` 存在
（libopus 1.4）。因此不写 `#include <opus.h>`，而是：

```cpp
void* lib = dlopen("libopus.so.0", RTLD_NOW | RTLD_LOCAL);
// 8 个符号：encoder_create/encode/encoder_ctl/encoder_destroy
//           decoder_create/decode/decoder_destroy/get_version_string
```

好处：编译期零依赖、运行期有就用；`ProbeAudioCodecs()` 在运行期给出
`opus=false` + 明确失败原因，上层 `PreferredAudioCodec()` 自动降级到 G.711。
坏处：符号表变化只能在运行期发现（测试覆盖 `opus != nullptr` 分支）。

### 4.2 重采样

Opus 只接受 8/12/16/24/48kHz。44.1kHz 输入需要 441→480 的转换，采用
**16.16 定点线性插值**（每样本约 3 条算术指令，`ResampleLinear` 反汇编 309
条指令 / 18 分支，含双声道循环）。不做带限滤波：对语音/测试音足够，对高频
音乐会引入轻微镜像——这是已知限制。

解码方向对称：`opus_decode` 输出 48k 的 480 样本，再线性插值回 44.1k 的 441。

### 4.3 PLC（丢包隐藏）

`AudioDecoder::Decode(nullptr, 0, out)` 约定为 PLC：
- Opus：`opus_decode(st, nullptr, 0, pcm, frame_size, 0)`（libopus 内置 PLC）；
- G.711：重复上一帧并衰减一半（首次 PLC 输出静音）；
- PCM：输出静音。

PLC 帧计入 `concealed_frames()`，不计入 `decoded_frames()`。

### 4.4 G.711 的「基线 vs 查表」

`src/media/audio.cpp` 同时保留两份 µ-law 编码：

- `UlawEncodeBaselineImpl`：教科书式「移位循环找 exponent」，14 条分支；
- `UlawEncodeTableImpl`：`constexpr` 生成的 256 项 `kUlawEncode.exponent` 查表，0 分支。

两份实现**逐字节等价**（测试穷举 65536 个输入断言），性能差 4.2–4.8 倍（§9.2）。
基线保留的意义是：性能回归时可复现、可审计。

---

## 5. 抖动缓冲与重组

### 5.1 `AudioJitterBuffer`

- 队列按**扩展序号**（16→64 位展开）有序插入（`std::upper_bound` + `vector`，
  0 次堆分配）；`max_packets` 上限（默认 512）防无界；
- 入队过滤：`ext < next` 记 `late`、重复序号记 `duplicates`、跳变超过
  `max_reorder_distance` 记 `late`；
- 出队：队首缺失时**等 `target_delay_packets × packet_ms` 再判丢**（抖动窗口内
  先等，避免把晚到包误判为丢包）；队首到达时若队列已够深则立即交付；
- 自适应深度：`desired = 1 + ceil(jitter_ms / packet_ms)`，clamp 到
  `[min, max]`，每次 ±1 慢变；抖动估计用到达间隔偏差的 EWMA（有符号差，
  乱序到达时不会下溢）；
- 统计：`queue_delay()`（`LatencyRecorder`，p50/p99）、`jitter_ms()`、`depth()`。

### 5.2 `AudioRtpReassembler`

按 `(ssrc, timestamp)` 挂起分片组，`max_pending` 组上限，组内 `present` 位图去重；
收齐后按分片序拼接并交付；`OnTimer` 清理超时组（`incomplete`）。

乱序（倒序喂入）与丢片（缺 1 片）都有测试覆盖。

### 5.3 传输

- `UdpAudioSender`：复用视频侧 `UdpEndpoint`（127.0.0.1:0 绑定、`sendto`），
  每帧分片后逐包发送，统计 `datagrams/failures/bytes`；
- `UdpAudioReceiver`：`Recv(timeout)` → `DecodeRtpHeader` → 回调（包 + 到达毫秒）；
- `InProcessAudioLink`：同一套分片，`due_ms = now + rand(0..jitter)`，按 due 排序
  交付；`SetDropEvery(n)` 模拟周期性丢包。测试与示例用它在不碰 socket 的情况下
  复现弱网。

---

## 6. 播放与端到端会话

- `ProcessAudioSink`（Pulse/ALSA 共用）：`pacat --playback --format=s16le --rate=R
  --channels=C --raw --latency-msec=L [--device=D]` 子进程 + stdin 管道；
- `NullAudioSink`：只统计，不碰设备（CI/压测默认）；
- `AudioSession`：`Create` 按配置组装采集/编码/分片/传输/抖动/重组/解码/播放，
  `SendNext`/`Pump`/`Run` 三个入口；`stats()` 给出 `AudioSessionStats`
  （帧数、字节数、丢包、PLC、延迟分位数、输出 RMS）。

**标准 RTP 互通**：本实现的 `frag/total` 扩展必须由网关剥离（接收方向）或补齐
（发送方向）；`marker` 位语义也从「talkspurt 开始」改为「帧最后一片」。这是
有意的取舍——项目内没有标准 RTP 对端，私有扩展换来了无歧义的重组。

---

## 7. 验收数据（2026-11-01，WSL2 / Ryzen 9 8945HX）

### 7.1 功能与一致性

| 用例 | 结果 |
|---|---|
| PCM 1e5 帧（441 样本/帧）编码→分片→重组→解码 | **逐字节 0 处不一致**，88,200,000 B，checksum 链 `0xc471e9c06a63e512`，耗时 163.6 ms |
| G.711 穷举 65536 个输入：基线 vs 查表 | 0 处不一致 |
| G.711 µ-law / A-law 往返 SNR（440Hz，-6dBFS） | **36.70 dB / 38.97 dB** |
| Opus 48k mono / 48k stereo / 44.1k(重采样) | **26.44 / 19.04 / 25.86 dB**，包均 41.1 B，lookahead 对齐 312 / 624 / 287 样本 |
| Opus PLC | 空负载输出 480 样本，`concealed_frames=1` |
| RTP 头 20 万次随机往返 + 边界（seq=0xFFFF、ts/ssrc=全 1） | 全部一致；非法版本/短包/CC/X 位被拒 |
| 分片/重组：1/2/7 片、倒序、缺片超时、重复、坏分片 | 全部符合预期 |
| 抖动缓冲：顺序 / ±20ms 乱序 / 每 10 丢 1 / 重复 / 迟到 / 自适应 / 序号回绕 | 顺序 100/100 交付、±20ms **200/200 零丢失**、丢包 90 包 + 10 kLost、回绕 4/4 |
| UDP 回环 1000 包 | **1000/1000** 收到，序号连续，checksum 匹配 |
| 端到端会话（进程内 PCM 100 帧 / UDP 80 帧 / Opus 80 帧） | 播放 100/80/80，重组与解码全等 |
| PulseAudio 真实采集 / 播放 | `parec` 5 包、`pacat` 20 帧写入成功 |

### 7.2 延迟

| 口径 | p50 | p99 | max |
|---|---:|---:|---:|
| 纯处理链路（采集→编码→分片→重组→解码→写 sink，1e5 帧） | 0.0002 ms | 0.0003 ms | 0.027 ms |
| 会话（进程内，10ms 帧 + 抖动缓冲 3 包） | 14.6 ms | 19.9 ms | — |
| 会话（UDP 回环，10ms 帧 + 抖动缓冲 3 包） | — | 19.6–20.6 ms | — |
| 会话（Opus，进程内） | — | 23.4–23.8 ms | — |
| 抖动缓冲排队延迟（顺序到达，target=3 包） | 30.0 ms | 30.0 ms | — |
| 抖动缓冲排队延迟（±20ms 乱序） | 20.0 ms | 30.0 ms | — |

纯处理链路是「内存内音频路径」的下限（微秒级）；会话延迟主要由**抖动缓冲的
目标深度**（3 包 × 10ms = 30ms）与 `Run` 的实时节拍决定，不是 CPU 处理能力。

### 7.3 吞吐（≥1e7，`taskset -c 2`，3 次取中位）

| 指标 | 样本数 | ns/单位 | 吞吐 | 备注 |
|---|---:|---:|---:|---|
| µ-law 编码（内联批量） | 1e7 | **1.33 ns/样本** | 752 M 样本/s | 生产路径 |
| µ-law 编码（逐样本函数调用） | 1e7 | 1.53 ns/样本 | 655 M 样本/s | 含 call/ret |
| µ-law 编码（教科书基线） | 1e7 | **6.38 ns/样本** | 157 M 样本/s | 14 条分支 |
| µ-law 解码（内联批量） | 1e7 | **0.28 ns/样本** | 3.55 G 样本/s | 5 条指令 |
| RTP 头编码 | 1e7 包 | **1.99 ns/包** | 502 M 包/s | 18 指令、0 分支 |
| 整帧分片（882B → 1 片） | 1e6 帧 | **28.9 ns/帧** | 34.6 M 帧/s | 复用输出缓冲 |
| 同上，每帧新建 vector | 2e5 帧 | 55.9 ns/帧 | 17.9 M 帧/s | 复用收益 **1.95×** |

---

## 8. 性能分析（objdump + 下限）

### 8.1 热函数反汇编

```bash
g++ -std=c++17 -Iinclude -O3 -DNDEBUG -c src/media/audio.cpp -o /tmp/audio_o3.o
objdump -d --no-show-raw-insn --disassemble=<mangled> /tmp/audio_o3.o
```

| 函数 | 静态指令 | call | 分支 | 分配 | 原子 |
|---|---:|---:|---:|---:|---:|
| `detail::UlawEncodeBaseline` | 54 | 0 | **14** | 0 | 0 |
| `detail::UlawEncodeTable` | 26 | 0 | **0** | 0 | 0 |
| `detail::UlawDecodeTable` | 5 | 0 | 0 | 0 | 0 |
| `EncodeRtpHeader` | 18 | 0 | 0 | 0 | 0 |
| `AudioRtpPacketizer::Packetize` | 168 | 3 | 17 | **0** | 0 |
| `AudioJitterBuffer::Push` | 230 | 2 | 35 | **0** | 0 |
| `ResampleLinear` | 309 | 0 | 18 | 0 | 0 |
| `AudioSession::Pump` | 418 | 21 | 58 | 0 | 0 |

`Packetize` 的 3 个 `call` 都是 `std::vector` 的容量检查/构造辅助（内联失败的
`operator new` 路径），正常路径不触发；`JitterPush` 的 2 个 `call` 同理。

### 8.2 离物理下限几倍

按 `asm-sfu.md` 的反推频率 4.5 GHz：

| 路径 | 实测 | 折算周期 | 理论下限 | 差距 |
|---|---:|---:|---:|---:|
| µ-law 编码（批量） | 1.33 ns/样本 | 6.0 cycles | ~4 cycles（L1 查表 load-to-use + 依赖链） | **1.5×** |
| µ-law 解码（批量） | 0.28 ns/样本 | 1.27 cycles | 1 cycle（1 load + 1 store/样本，端口限制） | **1.27×** |
| RTP 头 | 1.99 ns/包 | 9.0 cycles | ~4 cycles（3 次 4B store + bswap，含 call） | **2.2×** |
| 整帧分片 | 28.9 ns/帧 | 130 cycles | ~33 cycles（886B L1 拷贝 14 + 1202B 结构 19） | **3.9×** |

结论：

- G.711 解码已贴近端口下限（1.27×），编码的 6 cycles 主要是**每样本串行查表**
  的 load-to-use 延迟；循环展开/软件流水可接近 4 cycles，但 30ms 一帧的音频
  路径（每帧 441 样本 × 6 cycles ≈ 2.6 µs）不是瓶颈，暂不展开；
- RTP 头 9 cycles 中约 4 个是 `call/ret`（测试从外部函数调用），生产路径
  `Packetize` 内联后约 5 cycles；
- 整帧分片的 130 cycles 主要是 `AudioRtpPacket`（1202B）的构造/移动，优化方向
  是「预分配槽位 + 写指针」而不是继续压 memcpy——但 28.9 ns/帧 对 100 帧/秒
  的音频只有 0.003% CPU，属于可接受范围。

### 8.3 架构层面的取舍（先架构后汇编）

1. **每包零分配**：`AudioRtpPacket` 用固定 `std::array<uint8_t, 1188>`，不是
   `std::vector`；`Packetize` 的 3 个 call 不在正常路径。对比「每帧新建 vector」
   的 55.9 ns/帧，复用缓冲省 1.95×；
2. **抖动缓冲用有序 `vector` 而非 `std::map`**：每次插入 O(n)（n≤12），但零节点
   分配；`Push` 230 条指令、0 分配；
3. **采集/播放复用 `AudioFrame::data` 的 capacity**：`Read` 只 `resize` 不重新
   分配（稳态 0 次 malloc）；
4. **G.711 查表而不是逐位循环**：静态指令 54→26、分支 14→0、吞吐 4.2–4.8×。

---

## 9. 已知限制（WSL2 实测）

1. **PulseAudio 经 RDP 转发**：WSLg 没有本地声卡，`RDPSink`/`RDPSource` 的数据
   要过 RDP 到 Windows。`RDPSource` 在无音频活动时是 `SUSPENDED`，`parec` 可能
   长时间读不到包；测试用 2s 超时并 SKIP（不是失败）。真实设备链路的会话 p99
   约 20–24ms（含 30ms 目标深度的抖动缓冲），远高于内存链路的 0.0003ms。
2. **没有 `libopus-dev`**：只能 `dlopen("libopus.so.0")`；`ProbeAudioCodecs()`
   在库缺失时返回 `opus=false`，`PreferredAudioCodec()` 降级到 G.711。
3. **没有 `alsa-utils`**：`arecord`/`aplay` 不存在，ALSA 后端代码保留但
   `AlsaCaptureAvailable()`/`AlsaPlaybackAvailable()` 为 false；测试 SKIP。
4. **UDP 接收队列有限**：`net.core.rmem_max` 默认 208KB，一次灌 1000 个 894B 包
   会丢包；测试改为「每 16 帧 Pump 一次」（边发边收），真实会话本身就是边发边收。
5. **`SIGPIPE` 进程级忽略**：`pacat` 提前退出时写端收到 `EPIPE` 而不是被杀；
   这是服务端常见做法，但会改变宿主进程的默认信号行为。
6. **重采样是线性插值**：44.1k↔48k 有轻微高频镜像；对语音/测试音 SNR 25.9dB
   （Opus 48k mono 26.4dB），对宽带音乐不适用。
7. **标准 RTP 互通需要网关**：`frag/total` 4B 扩展与 marker 语义是本项目私有约定。

---

## 10. 复现

```bash
cd /UserData/CodexWorkSpace/Go2cpp
cmake --build .build-bento-wsl --target media_test_audio -j 8
./.build-bento-wsl/media_test_audio          # 打印 [PERF] 与全部断言

# 性能（固定 CPU、三次取中位）
taskset -c 2 ./.build-bento-wsl/media_test_audio

# 端到端演示
./examples/audio_e2e/build_and_run.sh --real

# 反汇编
g++ -std=c++17 -Iinclude -O3 -DNDEBUG -c src/media/audio.cpp -o /tmp/audio_o3.o
objdump -d --no-show-raw-insn \
  --disassemble=_ZN6go2cpp5media6detail18UlawEncodeBaselineEs /tmp/audio_o3.o
```
