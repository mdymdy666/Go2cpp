# 媒体（media）· 深入学习

> 本文按「总—分」写：先回答这套链路怎么跑起来，再逐块拆。所有 `文件:行` 都在当前工作树
> 用 `sed -n` 复核过；凡涉及性能，只写「本机实测（命令 + 环境 + 轮数）」，没有实测就写「未实测」。
> 不引用交接文档里的历史数字当结论。

---

## 第一部分 · 总：这个模块怎么跑起来

### 1.1 入口点清单

媒体模块没有全局单例、没有内部线程（音频采集/播放子进程除外），所有推进都由调用方
`SendNext()` / `Pump()` / `Tick()` 驱动。入口分四层：

| 入口（类/函数） | 文件:行 | 谁调用它 | 触发什么 | 返回什么 |
|---|---|---|---|---|
| `CreateX11Capture(config, err)` | `include/go2cpp/media/capture.hpp:104` | 应用 / `CreateMediaSender` | 连接 X11、尝试 XShm、初始化 `X11CaptureImpl` | `unique_ptr<VideoSource>`，失败为 null + error |
| `X11CaptureImpl::Capture(out)` | `src/media/capture_x11.cpp:112` | `MediaSender::SendNext` | `XShmGetImage` → 借用或拷贝；失败重试一次 → `XGetImage` 回退 | bool；帧写入 `out` |
| `SyntheticCapture::Capture(out)` | `src/media/capture_synthetic.cpp`（类声明 `include/go2cpp/media/capture.hpp:119`） | 测试 / 无 DISPLAY 场景 | 按四种确定性图案生成 BGRA | bool |
| `CreateEncoder(kind, w, h)` | `include/go2cpp/media/codec.hpp:164` | `MediaSender` 构造 / 降级重建 | 选择 raw-bgra / raw-i420 / delta-i420 / h264 子进程 | `unique_ptr<VideoEncoder>` |
| `VideoEncoder::Encode(frame, out)` | `include/go2cpp/media/codec.hpp:108` | `MediaSender::SendNext` | 产出 `MediaHeader + payload` | bool；超限/非法返回 false |
| `CreateDecoder(kind, w, h)` | `include/go2cpp/media/codec.hpp:167` | `MediaReceiver` 构造 | 选解码器；delta 解码器持有参考帧 | `unique_ptr<VideoDecoder>` |
| `UdpVideoSender::SendFrame(...)` | `include/go2cpp/media/transport.hpp:102` / `src/media/transport.cpp:153` | `MediaSender::SendNext` | 交给 `bento::VideoChannel` 切 1200B 片并 `sendto` | bool |
| `UdpVideoReceiver::Pump(timeout, max)` | `include/go2cpp/media/transport.hpp:132` | `MediaReceiver::Pump` | `poll` + 非阻塞 `recv`，重组整帧后回调 | 本轮处理的包数 |
| `MediaReceiver::OnEncodedFrame(...)` | `src/media/pipeline.cpp:357` | `UdpVideoReceiver` 的重组完成回调 | （可选）jitter 入队 → 解码 → 渲染 | void |
| `MediaSender::SendNext(&captured, &bytes)` | `include/go2cpp/media/pipeline.hpp:87` / `src/media/pipeline.cpp:145` | 应用循环 | 采集一帧 → 降级门控 → 编码 → 发送 | bool；`captured` 是浅拷贝 |
| `MediaReceiver::Pump(timeout_ms)` | `include/go2cpp/media/pipeline.hpp:175` / `src/media/pipeline.cpp:315` | 应用循环 | 收包 + 排空 jitter + `OnTimer` | 本轮交付/处理数 |
| `CreateSdlRenderer(w, h, headless, err)` | `include/go2cpp/media/render.hpp:81` | `MediaReceiver` 构造 | 创建窗口/渲染器/纹理；失败退回 headless | `unique_ptr<FrameRenderer>` |
| `FrameRenderer::Present(frame)` | `include/go2cpp/media/render.hpp:33` | `MediaReceiver::DecodeAndPresent` | I420→BGRA 或直传 BGRA 到 `SDL_UpdateTexture` | bool |
| `SfuRouter::CreateRoom/Join/Publish/Subscribe` | `src/media/sfu.cpp:230/287/372/468` | 应用 / 信令层 | 建房间、加 peer、发布流、挂订阅 | `SfuStatus` |
| `SfuRouter::ForwardFragment(...)` | `src/media/sfu.cpp:577` | `SfuPublisher` 的 `VideoChannel` 回调 | 校验帧头 → 按订阅同步回调 sink | 转发份数 |
| `SfuPublisher::SendFrame(...)` | `src/media/sfu.cpp:750` | 应用 | 交给内部 `VideoChannel` 分片 → `ForwardFragment` | bool |
| `SignalingSession::CreateOffer/SetRemoteAnswer` | `src/media/signaling.cpp:1456/1520` | 应用 / 信令通道 | SDP 子集序列化/解析 + 状态机 | `optional<SessionDescription>` / `SignalingStatus` |
| `SessionDescription::ParseSdp(text, err)` | `src/media/signaling.cpp:998` | 对端消息到达 | 解析 SDP 子集，校验方向/候选 | `optional<SessionDescription>` |
| `CreateAudioSource(config, err)` | `include/go2cpp/media/audio.hpp:271` | `AudioSession` / 应用 | parec/arecord 子进程或合成源 | `unique_ptr<AudioSource>` |
| `CreateAudioEncoder(config, err)` | `include/go2cpp/media/audio.hpp:353` | 音频发送链 | opus / g711 / pcm 编码 | `unique_ptr<AudioEncoder>` |
| `AudioRtpPacketizer::Packetize(...)` | `include/go2cpp/media/audio.hpp:426` | 发送链 | 12B RTP 头 + 4B 分片头 + payload | 包数 |
| `AudioJitterBuffer::Push/Pop` | `include/go2cpp/media/audio.hpp:573` | 接收链 | 按包序号重排、等空洞、丢迟到 | `bool` / `JitterPopResult` |
| `MjpegAviWriter::WriteFrame(...)` | `include/go2cpp/media/avi.hpp:151` | 录制 | 写 `00dc` chunk + `idx1` 索引，回填长度 | bool |

编译开关：`-DGO2CPP_WITH_MEDIA=ON`（默认 OFF）、`-DGO2CPP_MEDIA_TESTS=ON`；X11/SDL/ffmpeg/parec
都是**运行时探测**，不是编译期强制。X11 不可用时 `CreateX11Capture` 返回 null；SDL 不可用时
`CreateSdlRenderer` 失败，`CreateMediaReceiver` 退回 headless；ffmpeg 缺失时 h264 用例 SKIP。

### 1.2 总架构图

```
应用循环（调用方线程，全链路单线程）
  │
  ├─ MediaSender::SendNext ─────────────────────────────────────────────┐
  │    │ 1 Capture()                                                    │
  │    ▼                                                                │
  │  VideoSource ──BGRA──► VideoFrame（owned 或 borrowed）               │
  │    │ 2 degradation->ShouldDeliverFrame? 否 → 跳过编码/发送           │
  │    │ 3 keyframe_interval / ConsumeKeyframeRequest → ForceKeyframe    │
  │    ▼                                                                │
  │  VideoEncoder::Encode ──► MediaHeader(32B)+payload                  │
  │    │ 4 transport->SendFrame(frame_id,key,...)                       │
  │    ▼                                                                │
  │  bento::VideoChannel ──≤1200B/片，≤256 片/帧──► UDP sendto           │
  └─────────────────────────────────────────────────────────────────────┘
                              │  UDP（回环或真实网络）
                              ▼
  MediaReceiver::Pump ──► UdpVideoReceiver::Pump
                              │ poll + recv，按 frame_id 重组
                              │ 缺片超时 → 丢整帧；旧 frame_id → 丢
                              ▼
                    OnEncodedFrame(frame_id,key,data,bytes)
                              │
                 ┌────────────┴─────────────┐
                 │ jitter 关闭               │ jitter 打开
                 ▼                           ▼
          DecodeAndPresent            PushToJitter（整帧切 8KB 块）
                 │                           │ JitterBuffer::Push
                 │                           ▼ Pop（按块序拼帧）
                 │                     DrainJitter → DecodeAndPresent
                 ▼
          VideoDecoder::Decode（delta 需要参考帧）
                 │
                 ▼
          FrameRenderer::Present（SDL 直传 / headless 校验）
                 │
                 └─ capture_ns 计时 → LatencyRecorder p50/p99

旁路（不进 pipeline）：
  SfuRouter ── 订阅索引 ── ForwardFragment（指针转发，同步 sink）
  SignalingSession ── SDP/候选状态机 ── Tick 驱动超时
  Audio：Source → Encoder → RtpPacketizer → UdpAudioSender
         UdpAudioReceiver → Reassembler → AudioJitterBuffer → Decoder(PLC) → Sink
  录制：MjpegAviWriter（只封装已编码 JPEG，不含 JPEG 编码器）
```

边界与锁：
- `MediaSender` / `MediaReceiver` **零内部线程、零锁**；跨线程使用必须调用方加锁。
- `SfuRouter` 的容器不是线程安全的；`ForwardFragment` 与订阅变更必须在同一线程。
  `forward_depth` 是「延迟回收」的深度计数，不是锁。
- `UdpVideoReceiver` 单线程 `poll`；`bento::VideoChannel` 内部有重组状态与超时表。
- 音频采集/播放是子进程 + 管道；读路径 `poll` + 总超时，不占调度器线程。
- 所有「延迟」以 `MediaHeader.capture_ns` 为唯一口径，渲染端用 `steady_clock` 减它。

### 1.3 复杂场景一：一帧从采集到上屏（含 jitter、丢片、关键帧）

1. 应用调用 `MediaSender::SendNext(&captured, &bytes)`（`src/media/pipeline.cpp:145`）。
2. `m_source->Capture(m_capture.get())`（`src/media/pipeline.cpp:149`）。X11 路径先 `TryRebuildShm()`，
   再用 `XShmGetImage`；成功走 `CaptureFromShm`（`src/media/capture_x11.cpp:112-121`）。
3. 若 `zero_copy=true` 且格式可直读，`BorrowShmImage` 把 XShm 段借给帧并原地填 alpha
   （`src/media/capture_x11.cpp:282-300`）；否则 `CopyImage` 做 SSE2 行拷贝（`src/media/capture_x11.cpp:305+`）。
   失败路径：`XShmGetImage` 连续失败 → `CloseShm()` + 1 秒冷却重建（`src/media/capture_x11.cpp:126-133`）。
4. `m_capture->frame_id = ++m_frame_id`，`capture_ns = NowNs()`（`src/media/pipeline.cpp:154-155`）。
   **frame_id 从 1 开始**，因为 `VideoChannel` 把 `<= 已交付帧号` 视为 stale。
5. 降级门控：`ShouldDeliverFrame(now_ms)` 为 false → `++m_frames_skipped`，仍然把 `captured`
   浅拷贝出去并返回 true，但**不编码、不发送**（`src/media/pipeline.cpp:157-165`）。失败路径：这一步
   让 `frame_id` 出现空洞，收端按 frame_id 判新旧时不会等这些帧。
6. 关键帧判定：`keyframe_interval > 0 && frame_id % interval == 0` → `ForceKeyframe()`
   （`src/media/pipeline.cpp:167-169`）；降级/重连的 `ConsumeKeyframeRequest()` 也会强制
   （`src/media/pipeline.cpp:171-174`）。
7. `m_encoder->Encode(*m_capture, &m_encoded)`（`src/media/pipeline.cpp:175`）。delta 编码器在这里
   更新 `m_ref`；超限时返回 false（见 Q02 的坑）。
8. `m_encoded[6] != 0` 取 keyframe 位（`src/media/pipeline.cpp:181`），`m_transport->SendFrame(...)`
   （`src/media/pipeline.cpp:182`）。`UdpVideoSender` 转交 `bento::VideoChannel::SendFrame`
   （`src/media/transport.cpp:153-155`），后者切 1200B 片并逐片 `sendto`。
9. 收端 `MediaReceiver::Pump(timeout_ms)` → `m_transport->Pump(timeout_ms, 128)`
   （`src/media/pipeline.cpp:315-318`）。`UdpVideoReceiver` 把片按 `frame_id` 聚合；缺片超过 1s 或收到
   更旧 frame_id → 丢整帧；重组完成调用 `OnEncodedFrame`。
10. `OnEncodedFrame`（`src/media/pipeline.cpp:357`）：jitter 关闭时直接 `DecodeAndPresent`；
    打开时 `PushToJitter` + `DrainJitter`（`src/media/pipeline.cpp:359-363`）。
11. `PushToJitter` 把整帧按 `jitter_chunk_bytes`（默认 8192）切块，带 12B 块头
    （`src/media/pipeline.cpp:393-431`）；每块一个单调 `m_jitter_seq`，同一帧的块连续入队。
12. `JitterBuffer::Push`（`src/media/jitter_buffer.cpp:88`）按 `seq & (capacity-1)` 定位，
    判重/判迟到；首次到达记录 `m_first_arrival_ns`。`Pop`（`src/media/jitter_buffer.cpp:175`）在
    `m_next_seq` 有空洞时等 `m_hole_deadline_ns`（`src/media/jitter_buffer.cpp:248-252`），到期跳过整段。
13. `DrainJitter` 按块头把块拼回整帧（`src/media/pipeline.cpp:433+`）；拼不齐的帧计入
    `jitter_frames_dropped`，不进入解码。
14. `DecodeAndPresent`：`m_decoder->Decode`（delta 必须有参考帧，否则 `m_ref` 空/尺寸不符 → 丢帧，
    `src/media/codec_delta.cpp:368-371`）；`ShouldDeliverFrame` 再做一次渲染门控；`m_latency.Add(now-capture_ns)`；
    `m_renderer->Present(frame)`。
15. `SdlRenderer::Present`（`src/media/render.cpp:129`）：BGRA 直传 `SDL_UpdateTexture`
    （`src/media/render.cpp:163-172`），I420 先转 `m_bgra` 再传；`SDL_RenderCopy` + `SDL_RenderPresent`；
    `SDL_PollEvent` 只 peek，不阻塞（`src/media/render.cpp:173-180`）。
16. 计时与统计：`m_delivered` / `m_dropped` / `m_frames_skipped` / `jitter_stats()` /
    `transport_stats()` 在 `MediaReceiver` 上暴露；延迟只认 `capture_ns`。

失败/降级路径汇总：
- XShm 不可用 → `XGetImage`（必须拷贝，`XDestroyImage` 释放）；WSLg 下抓 root 会 BadMatch。
- 分片丢失 → 重组器丢整帧；delta 参考链断，等下一个关键帧（默认 30 帧）。
- jitter 容量满 → 丢最旧/标记 dropped；不会反压发送端。
- 渲染失败 → `++m_dropped`，不重试；headless 路径只做 checksum。

### 1.4 复杂场景二：弱网触发降级与恢复（发送端）

1. 应用周期性把 `NetworkSample`（带宽估计、丢包率、RTT）喂给 `MediaSender::OnNetworkSample`
   （`src/media/pipeline.cpp` 的 `OnNetworkSample`，转发到 `DegradationController::Update`，
   `src/media/degradation.cpp:159`）。
2. `DegradationController::Update` 维护四档 `QualityLevel`（`include/go2cpp/media/degradation.hpp:60`，
   `kQualityLevelCount=4`）与滞回：降档快（约 200ms 窗口）、升档慢（约 3s 窗口）。
   采样不足或处于冷却期时只更新统计，不切档。
3. 判定需要降档 → 返回 `DegradationEvent`（`include/go2cpp/media/degradation.hpp:152`），带原因
   `DegradationReason`（`include/go2cpp/media/degradation.hpp:139`）。
4. `MediaSender::SendNext` 在发送前看到 `event.level_changed` 为真 → `RebuildForLevel()`
   （`src/media/pipeline.cpp:201-204`）。
5. `RebuildForLevel()`（`src/media/pipeline.cpp:208`）：按新档位算宽高；若与当前采集源尺寸相同，只重建
   编码器；否则重建采集源（X11 重新裁剪/合成源改尺寸）。
6. 新编码器 `ForceKeyframe()`（`src/media/pipeline.cpp:234`），保证收端从关键帧重建参考链；旧编码器/
   采集源的引用在 `m_source`/`m_encoder` 赋值后失效（`src/media/pipeline.cpp:235-236`）。
7. 如果档位只改帧率不改分辨率：`ShouldDeliverFrame` 用相位控制跳帧（`src/media/pipeline.cpp:157`），
   采集照常推进，`capture_ns` 连续；被跳过的帧不编码，但 `frame_id` 仍自增。
8. 降档后 `adaptive_bitrate_bps` 在档内继续微调；`ReconnectSupervisor`
   （`include/go2cpp/media/degradation.hpp:304`）在连续失败时按指数退避（100→5000ms，±20% 抖动）重连。
9. 升档条件满足 → 同样走 `RebuildForLevel()`，分辨率变大时收端会看到几何变化；
   delta 解码器在 `m_ref_w/h` 不匹配时丢帧（`src/media/codec_delta.cpp:368`），等新关键帧。
10. 恢复后 `ConsumeKeyframeRequest()` 被 `SendNext` 消费（`src/media/pipeline.cpp:171-174`），
    避免重复强制关键帧。
11. 收端 `MediaReceiver::OnNetworkSample` 只做渲染帧率门控（`pipeline.cpp` 同名函数），
    不重建解码器；解码照常以保持参考链。
12. 失败路径：新采集源创建失败 → `RebuildForLevel` 保留旧源/编码器并返回，档位事件被丢弃；
    下一次采样再试。

关键不变量：
- 切档必须强制关键帧，否则收端参考链断。
- `RebuildForLevel` 之后，任何外部持有的 `encoder()` / `source()` 引用都失效。
- 降帧率不能停采集，否则 `capture_ns` 与媒体时间轴断裂。
- 降档/升档的滞回窗口不可省，否则会在阈值附近震荡。

### 1.5 关键不变量清单

| # | 不变量 | 证据 | 破法后果 |
|---|---|---|---|
| 1 | 一帧的 `frame_id` 从 1 开始且发送端单调递增 | `src/media/pipeline.cpp:152-155` | 首帧 0 被 `VideoChannel` 判 stale，永远不交付 |
| 2 | 零拷贝借用视图只活到下一次 `Capture()` | `include/go2cpp/media/capture.hpp:14-19`、`include/go2cpp/media/frame.hpp:125-131` | 跨帧持有读到新画面/已 detach 的段 |
| 3 | delta 解码必须有参考帧；几何变化视为无参考 | `src/media/codec_delta.cpp:368-371` | 花屏/整帧丢弃，直到关键帧 |
| 4 | 关键帧周期默认 30 帧，切档/重连必须强制 | `src/media/pipeline.cpp:167-174` | 丢包后错误持续整个 GOP 甚至永久 |
| 5 | `MediaHeader` 固定 32B，`kMaxEncodedFrameBytes` 按分片上限留余量 | `include/go2cpp/media/codec.hpp:76-90`、`include/go2cpp/media/codec.hpp:182-184` | 头解析错位/超限帧发不出去 |
| 6 | 单帧 ≤ 256×1200B；分片上限是硬约束 | `include/go2cpp/media/codec.hpp:182-184`、`include/go2cpp/media/transport.hpp:8-15` | 大分辨率关键帧被拒或 UDP 丢包 |
| 7 | jitter 在「整帧重组之后」，块序连续 | `src/media/pipeline.cpp:357-363`、`src/media/pipeline.cpp:393-431` | 误以为能恢复丢片；统计恒 0 |
| 8 | SFU 转发零拷贝，`SfuFragment` 只在 sink 调用期间有效 | `src/media/sfu.cpp:655-687`、`include/go2cpp/media/sfu.hpp:8-13` | 异步持有指针读到下一帧 |
| 9 | SFU 失活立即扣计数、删除延迟到 `forward_depth==0` | `src/media/sfu.cpp:149-160`、`src/media/sfu.cpp:186-193` | 回调内退订导致迭代器失效/UAF |
| 10 | 全链路单线程；跨线程必须外部加锁 | `pipeline.hpp` 文件头、`include/go2cpp/media/sfu.hpp:8-13` | 数据竞争/统计错乱 |

---

## 第二部分 · 分：逐块细讲

### 2.1 帧容器与采集

**职责**：把「一块屏幕」变成 `VideoFrame`，并明确像素所有权。

**关键数据结构**（`include/go2cpp/media/frame.hpp:66-146`）：
- `width/height/format/stride`：尺寸与布局；`stride==0` 时按格式推导（`EffectiveStride()`，`include/go2cpp/media/frame.hpp:91-93`）。
- `std::vector<uint8_t> data`：自有像素；借用模式下为空。
- `const uint8_t* borrowed`：零拷贝视图（`include/go2cpp/media/frame.hpp:75`）。
- `frame_id`、`capture_ns`、`keyframe`：线格式与延迟口径。
- `Bytes()`（`include/go2cpp/media/frame.hpp:100`）统一读入口；`OwnsStorage()`（`include/go2cpp/media/frame.hpp:105`）判所有权；
  `Borrow()`（`include/go2cpp/media/frame.hpp:125-131`）清空自有 data；`Unborrow()`（`include/go2cpp/media/frame.hpp:134`）只清指针。

**X11 采集状态机**（`src/media/capture_x11.cpp`）：
```
Init: XOpenDisplay → XShmQueryExtension? → XShmCreateImage → shmget/shmat → XShmAttach
  │ 失败                                                  │ 成功
  ▼                                                       ▼
XGetImage 模式（每帧拷贝）                          XShm 模式（每帧 XShmGetImage）
                                                          │ zero_copy && fast BGRA?
                                                          ├─ 是 → BorrowShmImage（借用）
                                                          └─ 否 → CopyImage（owned）
XShmGetImage 连续失败 → CloseShm → 1s 冷却 → TryRebuildShm → InitShm
```
关键函数：
- `Capture()`（`src/media/capture_x11.cpp:112`）：`TryRebuildShm` → `XShmGetImage` → 失败 `XSync` 重试一次 →
  连续失败 `CloseShm` + `ScheduleShmRebuild` → `XGetImage` 回退。
- `CaptureFromShm()`（`src/media/capture_x11.cpp:252`）：`zero_copy && IsFastBgraFormat && BorrowShmImage`
  才借用，否则 `CopyImage`。
- `BorrowShmImage()`（`src/media/capture_x11.cpp:284`）：`out->Borrow(base, bytes_per_line)`，可选原地
  填 alpha（`FillAlphaRowSimd`，只写第 4 字节）。
- `InitShm()`（`src/media/capture_x11.cpp:166`）：建共享段、attach；失败释放并返回 false。

**失败路径与限制**：
- WSLg / XWayland 偶发 `BadMatch`：先 `XSync` 清错误再重试一次（`src/media/capture_x11.cpp:118-124`）。
- `XGetImage` 抓 root 在 WSLg 必 `BadMatch`（环境限制，测试 SKIP 而非 FAIL）。
- 区域裁剪到 drawable 内，避免越界（`src/media/capture_x11.cpp:94` 附近）。
- 共享段失效后 1 秒冷却，避免每帧 attach 打爆 X server。

**典型误用**：跨帧持有借用帧；在多线程读 `Bytes()`；把 `XGetImage` 路径也当零拷贝。

### 2.2 编码与线格式

**职责**：把 `VideoFrame` 编成 `MediaHeader + payload`，并保证分片层能承载。

**关键数据结构**：
- `MediaHeader`（`include/go2cpp/media/codec.hpp:76-90`）：`version`、`codec`、`keyframe`、`width/height`、
  `frame_id`、`payload_bytes`、`capture_ns`；固定 32B，magic `G2MF`。
- `CodecKind`（`include/go2cpp/media/codec.hpp:62-71`）：`kRawBgra / kRawI420 / kDeltaI420 / kH264`。
- `VideoEncoder`（`include/go2cpp/media/codec.hpp:99-133`）：`Encode`、`ForceKeyframe`、`kind`、`Name`、统计。
- `VideoDecoder`（`include/go2cpp/media/codec.hpp:136-161`）：`Decode`、`dropped_frames`。
- `kMaxEncodedFrameBytes`（`include/go2cpp/media/codec.hpp:184`）：`256*1200-64`。

**delta-i420 编码**（`src/media/codec_delta.cpp`）：
1. `ToCompactI420` 把 BGRA/I420 统一成紧凑 I420。
2. `key = m_force_keyframe || geometry_changed || m_ref.empty()`（`src/media/codec_delta.cpp:195`）。
3. 关键帧：所有 16×16 宏块进 `AppendBlocks`，`m_ref.assign(i420)`。
4. 差分帧：逐宏块 `MacroblockDiffers`，脏块写 bitmap + RLE；全静时 payload 只有 4 字节 0 bitmap。
5. 写头、拷贝 payload、更新 `m_ref`，最后检查 `kMaxEncodedFrameBytes`（**顺序是坑**，见 Q02）。
6. `m_oversize` 计数；`Name()` 变 `delta-i420(oversize!)`。

**delta-i420 解码**（`src/media/codec_delta.cpp:345+`）：
- 关键帧：`m_ref.assign(I420FrameBytes, 0)`，再按块写。
- 差分帧：`m_ref` 空/尺寸不符 → `++m_dropped; return false`；否则按 bitmap 逐块重建。
- 输出 `out->keyframe`、`frame_id`、`capture_ns`。

**状态机**：
```
m_ref.empty() ──关键帧──► 有参考
     ▲                       │ 几何变化 / ForceKeyframe
     └───────────────────────┘
差分帧 ──缺参考/越界──► drop（不更新参考）
```

**失败路径**：超限、几何变化、缺参考、payload 截断/bitmap 越界、block 越界都返回 false；
调用方必须把 false 当「本帧不可发」，不能只清统计。

**典型误用**：把 delta 当无损可丢帧；忘记 `ForceKeyframe`；超限后继续发差分（Q02）。

### 2.3 传输与重组

**职责**：把编码块切成 ≤1200B 的 VIDEO 分片，经 UDP 发出；收端按 frame_id 重组。

**关键数据结构**：
- `UdpVideoSender`（`include/go2cpp/media/transport.hpp:87-114`）：内部 `bento::VideoChannel`，`SendFrame`。
- `UdpVideoReceiver`（`include/go2cpp/media/transport.hpp:116-157`）：`poll` + `recv`，`Pump`、`OnTimer`、统计。
- `InProcessVideoLink`（`include/go2cpp/media/transport.hpp:159-197`）：进程内直连，丢包注入/测试用。
- `VideoChannel` 的 16B 帧头 + 8B 视频子头 + ≤1200B payload（`include/go2cpp/media/transport.hpp:8-15`）。

**发送路径**：`MediaSender::SendNext` → `UdpVideoSender::SendFrame`（`src/media/transport.cpp:153`）→
`VideoChannel::SendFrame` → 每片 `sendto`。失败只返回 false，不重传。

**接收路径**：`UdpVideoReceiver::Pump`（`include/go2cpp/media/transport.hpp:132`）→ `poll` → `recv` 循环 →
`VideoChannel` 重组 → 整帧回调 `OnEncodedFrame`。`OnTimer`（`include/go2cpp/media/transport.hpp:135`）清理超时帧。

**失败路径**：
- 缺片超时（约 1s）→ 丢整帧，`frames_skipped`/`dropped` 增加。
- 旧 frame_id → 丢弃（防重放/回退）。
- 畸形包 → `dropped_packets`。
- 单帧 >300KB → 发送端拒绝或 UDP 丢包；WSL2 默认 `rmem_max` 是环境限制，不是逻辑 bug。

**典型误用**：把 `VideoChannel` 当可靠传输；忽略 `keyframe` 起始片；在大分辨率下用 raw I420。

### 2.4 抖动缓冲（视频）

**职责**：在接收侧按序号重排，吸收到达抖动；有界窗口内等空洞，超时跳过。

**关键数据结构**（`include/go2cpp/media/jitter_buffer.hpp`）：
- `JitterBufferConfig`：`capacity`（2 的幂）、`max_payload_bytes`、`target_delay`、
  `min/max_delay`、`late_tolerance`、`deliver_late`、`startup_buffering`、`timestamp_hz`。
- 512 槽环形数组 + `m_next_seq` + `m_hole_deadline_ns` + `m_late_meta[8]`。
- `JitterStats`：received/reordered/duplicates/lost/late_delivered/dropped_late、target_delay、held。

**关键函数**：
- 构造（`src/media/jitter_buffer.cpp:45`）：容量归一化到 2 的幂，分配槽与迟到池。
- `Push`（`src/media/jitter_buffer.cpp:88`）：算 `ahead = seq - m_next_seq`；迟到且超出容忍 → dropped_late；
  `deliver_late` 且在上限内 → `PushLate`；否则写槽、更新 EWMA。
- `Pop`（`src/media/jitter_buffer.cpp:175`）/`PopInternal`（`src/media/jitter_buffer.cpp:183`）：先迟到补发；启动缓冲；
  空洞等 deadline；到期跳过并连续丢。
- `UpdateJitter`（`src/media/jitter_buffer.cpp:302`）：RFC 3550 式 EWMA `J += (|D|-J)/16`。

**状态机**：
```
未启动 ──首包──► startup_buffering（等 target_delay）
                     │
                     ▼
                 按序交付 ──空洞──► 等 deadline ──超时──► 跳过整段
                     ▲                                   │
                     └────────── 下一包 ──────────────────┘
```

**失败路径**：容量满、payload 超上限、序号回绕、断流重锚。**注意**：pipeline 的接线在整帧
重组之后（Q03），所以这里看不到网络分片乱序。

### 2.5 弱网降级与重连

**职责**：根据带宽/丢包采样在四档之间切换，并管理重连退避。

**关键数据结构**：
- `QualityLevel`（`include/go2cpp/media/degradation.hpp:60`）、`QualityParams`（74）、`DegradationConfig`（86）。
- `NetworkSample`（128）：带宽估计、丢包率、RTT。
- `DegradationController`（175）：当前档、滞回计时、冷却、关键帧请求。
- `ReconnectSupervisor`（304）：`LinkState`（265）、`ReconnectAction`（277）、退避配置（284）。

**关键函数**：
- `DegradationController::Update(now_ms, sample)`（`src/media/degradation.cpp:159`）：算档位、滞回、
  返回事件。
- `ConsumeKeyframeRequest()`（`src/media/degradation.cpp:348`）：把「切档后需要关键帧」交给发送端。
- `RebuildForLevel()`（`src/media/pipeline.cpp:208`）：重建源/编码器，强制关键帧。
- `ReconnectSupervisor`：指数退避 + 抖动；重连成功后请求关键帧。

**失败路径**：重建失败保留旧对象；降档事件丢失；升档过快导致震荡（滞回防）。

### 2.6 SFU

**职责**：一对多转发，不解码、不重编码；按订阅选择性转发。

**关键数据结构**：
- `SfuRouter`（`include/go2cpp/media/sfu.hpp:158`）：`Impl` 持有 `rooms`（map）、每个 room 的 `peers`/`streams`。
- `Stream`：`subscriptions` 向量（`unique_ptr<Subscription>`）、`dead`、统计。
- `Subscription`：`subscriber`、`sink`、`active`、`waiting_key`。
- `SfuFragment`（`include/go2cpp/media/sfu.hpp:89`）：借用指针 + 头 + hops。
- `SfuPublisher`（232）/`SfuSubscriber`（264）/`SfuCascade`（318）。

**关键函数**：
- `ForwardFragment`（`src/media/sfu.cpp:577`）：校验 `data/bytes`、hops、16B 头 + 8B 视频子头、
  type/channel/ACK 标志、frag_count/index、payload 上限；找 room/stream；遍历订阅同步 sink。
- `Subscribe`（468）：创建 `Subscription`，`waiting_key = require_keyframe_start`。
- `Unsubscribe`（510）：`ReleaseSubscription`（149）+ `Cleanup`（186）。
- `Leave`（307）：`MarkStreamDead` + 延迟回收。

**状态机**：
```
Room ──Join──► Peer ──Publish──► Stream ──Subscribe──► Subscription(active)
                                              │ waiting_key
                                              ▼ 收到关键帧首片
                                           active 转发
Room/Peer/Stream dead ──forward_depth==0──► Cleanup 真删除
```

**失败路径**：room/stream 不存在、无订阅者、hop 超限、畸形片、非关键帧起始。热路径零分配、
零锁；sink 慢会拖慢发布者。

### 2.7 信令

**职责**：SDP 子集序列化/解析 + ICE 候选交换 + offer/answer 状态机。

**关键数据结构**：
- `IceCandidate`（`include/go2cpp/media/signaling.hpp:96`）、`IsValidIceCandidate`（124）、排序（127）。
- `SessionDescription`（229）：`type`、`media`、`candidates`、`fingerprint` 等。
- `SignalingSession`（386）：role/state、local/remote description、候选、超时。
- `SignalingState`（299）、`SignalingStatus`（311）、`SignalingStats`（329）。

**关键函数**：
- `ParseSdp`（`src/media/signaling.cpp:998`）：逐行解析 `m=`/`a=`/`c=`，校验方向、候选、fingerprint。
- `CreateOffer`（1456）/`SetRemoteOffer`（1476）/`CreateAnswer`（1497）/`SetRemoteAnswer`（1520）。
- `AddLocalCandidate`（1564）/`AddRemoteCandidate`（1588）/`EndOfRemoteCandidates`（1618）。
- `OnIceConnected`（1627）/`OnIceFailed`（1665）/`Tick`（1674）。

**状态机**：
```
Idle ──CreateOffer──► HaveLocalOffer ──SetRemoteAnswer──► Stable
  │                        │
  │                        └─SetRemoteOffer──► HaveRemoteOffer ──CreateAnswer──► Stable
  └─SetRemoteOffer──► ...       失败/超时 ──► Failed（Tick 驱动）
```

**失败路径**：SDP 语法错、方向不兼容、候选非法、角色冲突、超时。`a=fingerprint` 只透传，
不做 DTLS 校验。

### 2.8 音频

**职责**：采集 → 编码（Opus/G.711/PCM）→ RTP 分片 → 接收/重组 → 抖动 → 解码（PLC）→ 播放。

**关键数据结构**：
- `AudioFormat`（`include/go2cpp/media/audio.hpp:106`）：采样率、声道、样本格式。
- `AudioFrame`（153）：`data`、`format`、`timestamp`。
- `AudioEncoder`（289）/`AudioDecoder`（318）：Opus/G711/PCM。
- `AudioRtpPacketizer`（426）：12B RTP + 4B 分片；`AudioRtpReassembler`（482）。
- `AudioJitterBuffer`（573）：按包数配置，与视频 `JitterBuffer` 完全独立。
- `AudioSession`（974）：采集→编码→发送 / 接收→解码→播放的组装。

**关键函数**：
- `EncodeRtpHeader`/`DecodeRtpHeader`（398/401）。
- `UlawEncodeBlock`（894）：AVX2 分派，标量回退；`ResampleLinear`（src/media/audio.cpp:746）：
  `channels==1 && AVX2` 才走 SIMD（Q05）。
- `OpusAudioEncoder::Encode`：48k 直通；44.1k 先重采样到 48k。
- `AudioJitterBuffer::Push/Pop`：按包序号、等空洞、PLC。

**状态机**：
```
采集 ──► 编码 ──► RTP 分片 ──► UDP
                                  │
UDP ──► 重组 ──► 抖动缓冲 ──► 解码（缺包 PLC）──► 播放
```

**失败路径**：Pulse/ALSA 不可用 → 合成源/null sink；Opus 缺失 → G.711/PCM；重采样只做
定点线性（高频镜像，已知限制）；无 AEC/NS/AGC。

### 2.9 AVI 封装

**职责**：把已编码的 JPEG 帧写成 MJPEG AVI，不包含 JPEG 编码器。

**关键数据结构**：
- `MjpegAviConfig`（`include/go2cpp/media/avi.hpp:70`）：宽高、帧率、质量（仅记录）。
- `AviIndexEntry`（79）：`idx1` 的 chunk id/flags/offset/size。
- `MjpegAviWriter`（151）：`WriteFrame`、`Finish`、长度回填。

**关键函数**：`PutLe16/32`、`PutFourCC`、`PatchLe32`（101）、`WriteAll`（124）；
`WriteFrame` 写 `00dc` chunk 并记录 index；`Finish` 回填 RIFF/movi 长度与 `idx1`。

**失败路径**：奇数长度 chunk 需要 padding；`idx1` offset 相对 `movi`；超过 4GB 的 AVI 限制
（未做 OpenDML）。**典型误用**：把任意编码帧当 JPEG 写入；忘记 `Finish` 导致索引缺失。

---

## 附：本机自测入口（不引用历史数字）

```bash
cmake -S . -B .build-media -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DGO2CPP_WITH_BENTO=ON -DGO2CPP_WITH_MEDIA=ON -DGO2CPP_MEDIA_TESTS=ON
cmake --build .build-media -j"$(nproc)"
ctest --test-dir .build-media -R '^media_' --output-on-failure
```

单模块：`media_test_capture`（XShm/owned 逐字节）、`media_test_codec`（delta 无损/拒绝）、
`media_test_transport`（乱序/缺片/畸形）、`media_test_jitter_buffer`（乱序/丢包/回绕）、
`media_test_degradation`（四档/滞回/退避）、`media_test_sfu`（随机拓扑）、
`media_test_signaling`（SDP 往返/变异）、`media_test_audio`（G.711 穷举/SIMD）、
`media_test_avi_mjpeg`（结构/索引）、`media_test_e2e` / `media_test_e2e_jitter`（端到端）。

性能只认自测：例如
`taskset -c 2 ./.build-media/media_test_capture` 看它打印的 p50/p99 与帧数；
不同机器/负载不可横向比较。
