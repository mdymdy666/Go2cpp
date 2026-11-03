# 06 media 答卷（5 题）

> 环境：WSL2 / 12 vCPU。media 默认 OFF，`.build-bento-wsl` 未含 media 目标，本轮**未重跑** media 测试（见各题「没答出」）。

## Q01

**答**：`VideoFrame` 两种所有权：`borrowed == nullptr` 时 `data` 拥有像素；`borrowed != nullptr` 时 `data` 为空、像素只读、`OwnsStorage()` 为 false。`SendNext(&out)` 的 `*captured = *m_capture` 是成员级拷贝：`borrowed` 指针被复制、`data` 为空——不是快照；下一次 `m_source->Capture(m_capture.get())` 会覆盖同一块 XShm 段，若中途 `InitShm` 重建/`CloseShm`，旧指针还可能指向已 detach 的段。零拷贝生效的三个前提：`zero_copy=true`、`use_shm=true` 且 XShm 初始化成功、当前 XImage 是可直读 BGRA 布局；任一不满足走 owned 拷贝（此时跨帧持有反而安全）。正确用法：本次 `SendNext` 返回后、下一次调用前消费 `Bytes()`；或显式 `memcpy` 进自有帧并 `Unborrow()`；或关 `zero_copy`。原地填 alpha 改的是 XShm 共享段（X server 也映射着），只动第 4 字节，像素三通道不变。

**代码证据**：
```text
include/go2cpp/media/capture.hpp:14-17  // **零拷贝**：XShm 路径默认仍把共享段拷进 VideoFrame::data（历史行为...）。打开 zero_copy 后...借用视图
include/go2cpp/media/capture.hpp:85-88  // - VideoFrame::data 为空、borrowed 指向 XShm 段；- 帧只在下一次 Capture() 前有效...
include/go2cpp/media/frame.hpp:75  const std::uint8_t* borrowed = nullptr;   ///< 零拷贝视图（非空时像素只读）。
include/go2cpp/media/frame.hpp:125-128  void Borrow(...) { borrowed = bytes; stride = ...; data.clear();
src/media/pipeline.cpp:160  *captured = *m_capture;   // 成员级拷贝，borrowed 指针被复制
```

**自证**：`grep -n "Borrow\|borrowed\|zero_copy" include/go2cpp/media/capture.hpp src/media/pipeline.cpp` 验证借用与拷贝路径；未写 20 行探针（题面建议的「连续 Capture 两次比对 Bytes」）。

**我没答出的**：没有在 X11 环境跑「跨帧持有视图被覆盖」的探针（`.build-bento-wsl` 无 media 目标，需 `-DGO2CPP_WITH_MEDIA=ON` 另建）。

## Q02

**答**：失败顺序是：编码器**先** `m_ref.assign(i420)` 更新参考帧，**再**判断 `out->size() > kMaxEncodedFrameBytes`，超限才 `++m_oversize; return false`；`MediaSender::SendNext` 直接返回 false，这一帧**没进入传输**。下一帧仍是 delta（`m_force_keyframe` 已消费、几何未变、`m_ref` 非空），它引用的是收端从未收到的那一帧 → 参考链分叉，画面错到下一个关键帧（`keyframe_interval` 或降级/重连强制）。关键帧超限则收端连参考都没有，后续 delta 全被丢弃。`m_oversize` 与 `Name()` 变 `delta-i420(oversize!)`，但 `keyframes()` 不增加；只看 `frames_sent` 会漏掉「编码成功但发送失败」。修复方向：超限时不更新 `m_ref`、或更新后立即 `ForceKeyframe()`、或统一 `kMaxEncodedFrameBytes` 与分片预算。

**代码证据**：
```text
src/media/codec_delta.cpp:272-275
  272  m_ref.assign(i420.begin(), i420.end());
  273  if (out->size() > kMaxEncodedFrameBytes) {
  274      ++m_oversize;
  275      return false;
include/go2cpp/media/codec.hpp:182-184  // 一个「MediaHeader + 负载」块的最大字节数（受 VideoChannel 分片上限约束）...kMaxEncodedFrameBytes = 256 * 1200 - 64;
src/media/codec_delta.cpp:368-371  // 缺参考帧：差分帧无法还原，整帧丢弃，等下一个关键帧。
src/media/pipeline.cpp:167-169  if (keyframe_interval > 0 && frame_id % interval == 0) m_encoder->ForceKeyframe();
```

**自证**：代码路径核对；未跑 640×480 随机噪声的超限实验。

**我没答出的**：没有实测「返回 false 的下一帧仍非关键帧」与收端逐帧比对（未构建 media 目标）。

## Q03

**答**：jitter 接在 `UdpVideoReceiver` **重组完成之后**：数据流是「UDP 分片 → VideoChannel 重组 → 完整帧 → 切 8KB 块 → JitterBuffer → 按块序拼回 → 解码/渲染」。同一帧的所有块在**一次函数调用内连续入队**、序号连续且无并发插入，块与块之间不可能乱序，跨帧块也不交错，所以 `reordered`/`duplicates` 在正常路径恒为 0；网络层真正的乱序/丢片早在重组器被处理（丢片=整帧丢弃），jitter 看不到。它真实买到的是「给整帧增加可配置排队延迟（吸收帧到达抖动）+ 容量上限内帧级 FIFO」，代价是固定延迟与切块/拷贝。「丢一个分片 jitter 能救」是错的：重组器已丢弃整帧。`startup_buffering=false` 只是首帧不攒满 `target_delay`。

**代码证据**：
```text
src/media/pipeline.cpp:355-360  void MediaReceiver::OnEncodedFrame(frame_id, key, data, bytes) { ... if (m_jitter != nullptr) { PushToJitter(frame_id, data, bytes);
src/media/pipeline.cpp:393-395  void MediaReceiver::PushToJitter(...) { if (m_jitter == nullptr || data == nullptr || bytes == 0) {
src/media/pipeline.cpp:272-277  if (config.jitter) { const int chunk = NormalizeChunkBytes(config.jitter_chunk_bytes); ... max_payload_bytes = max(...)
src/media/jitter_buffer.cpp:88-93  PushResult JitterBuffer::Push(seq, timestamp, payload, bytes, arrival_ns)
```

**自证**：代码路径核对；未跑 `media_test_e2e_jitter`（无 media 构建）。

**我没答出的**：没有注入分片乱序对比 `reordered` 统计。

## Q04

**答**：`SfuFragment` 在栈上构造，`fragment.data = data`、`fragment.payload = payload` 都是**借用指针、不拷贝**；它只在本次 `sink` 调用期间有效，sink 返回后发布方可复用缓冲。订阅者只存指针异步用 → 读到下一帧或已释放内存。正确做法：sink 内同步消费，或把 `bytes` 拷进自己的队列/引用计数块。回调里退订/离开安全：只置 `active=false` 并扣 peer 计数、不 erase；`forward_depth` 阻止 `Cleanup` 重入，索引遍历的 `total` 循环前取定；转发深度归零后才真正 `Cleanup()`。`require_keyframe_start`：新订阅必须从关键帧**首片**开始，否则重组器永远缺片，被门控的片计入 `fragments_keyframe_gated`。热路径为了 0 分配/0 锁不做深拷贝、不做背压，订阅者慢会拖慢发布者（同步回调）。

**代码证据**：
```text
src/media/sfu.cpp:655-657  fragment.data = data; fragment.bytes = bytes; fragment.header = header;
src/media/sfu.cpp:666-672  if (!subscription->active) continue; if (subscription->waiting_key) { // 必须从关键帧首片开始
src/media/sfu.cpp:149-152  void ReleaseSubscription(...) { if (!subscription.active) return;
src/media/sfu.cpp:186-189  void Cleanup() { if (forward_depth > 0) { cleanup_pending = true; return; }
src/media/sfu.cpp:695-696  if (forward_depth == 0 && cleanup_pending) Cleanup();
```

**自证**：代码路径核对；未跑 `media_test_sfu` 的 3000 步随机拓扑。

**我没答出的**：没有写「sink 只存指针 → 读到下一帧」的探针。

## Q05

**答**：分派条件是 `channels == 1 && CpuHasAvx2()` 才走 `ResampleLinearAvx2Mono`，否则 `ResampleLinearScalarRange`；双声道必然回退标量。`SelectedAudioKernel()` 是**进程级 CPU 探测**，不是「本帧实际内核」，用它做性能归因会把标量路径误记成 AVX2。SIMD 只做单声道的原因：`vpgatherdd` 一次 gather 两个相邻样本，双声道交错时 `idx+1` 是另一个声道，插值语义不成立；要做双声道得按声道分离或 8 样本跨步 gather。文档里的 44.1→48 AVX2 数字是单声道口径；立体声要用标量数据或先 deinterleave。正确性约束：SIMD 与标量必须逐样本一致（同一 16.16 定点 step、同样 clamp 与舍入）。

**代码证据**：
```text
src/media/audio.cpp:745-747  /// @brief 16.16 定点线性重采样（单/双声道交错处理；自动选 SIMD 内核）。 void ResampleLinear(..., int channels)
src/media/audio.cpp:696-699  /// @brief 单声道重采样的 AVX2 内核...用一条 vpgatherdd 同时取出 (in[idx], in[idx+1])
src/media/audio.cpp:654-656  void ResampleLinearScalarRange(...)  // 标量通用实现（单/双声道）
src/media/audio.cpp:1272  AudioKernelLevel SelectedAudioKernel() noexcept {   // 题面写 1269，实际函数定义在 1272（本卷埋点，偏移 3 行）
src/media/audio.cpp:1596  ResampleLinear(input, in_frames, frame.format.sample_rate, m_pcm.data(), ...)
```
> **题面错误**：Q05 的 `src/media/audio.cpp:1269` 是 µ-law 编码循环，`SelectedAudioKernel()` 定义在 **1272**（本卷埋点）。

**自证**：`grep -n "SelectedAudioKernel" src/media/audio.cpp` → 1272；`sed -n '745,750p'` 见 channels 参数。未做单/双声道计时探针。

**我没答出的**：没有实测单声道 vs 双声道重采样耗时比与 `perf stat` 指令数。
