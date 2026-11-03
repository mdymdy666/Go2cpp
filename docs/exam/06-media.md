# Go2Cpp 理解力考试 · 媒体卷（06 media / 5 题）

> 覆盖：X11Shm 零拷贝采集与借用生命周期、delta-i420 参考帧、收端 jitter 的真实接线位置、
> SFU 指针转发的生命周期与延迟回收、音频重采样的 SIMD 分派边界。
> 出题依据：`user/docs/06-media.md`、`docs/modules/{webrtc-media,media-audio,jitter-degradation,sfu-signaling}.md`、
> `docs/audit/`、`AI_COLLAB.md`，以及**当前工作树源码**。
> 难度分布：L1 = 0、L2 = 0、L3 = 2、L4 = 3。
> **本卷数字均需自测，文档数字仅作历史对照。** 文中若出现历史数字，一律视为「可能已过时」；
> 验收以你跑出来的数为准，并说明测量条件（构建目录、机器、轮数、负载）。

---

## 一、采集与帧所有权

### Q01 【L4 陷阱】`SendNext(&out)` 拿到的帧，为什么在下一帧之后就可能是脏数据甚至野指针？

- **痛点**：把 `*captured = *m_capture` 当成深拷贝/安全快照；开了 `zero_copy` 后跨帧持有
  `VideoFrame`，或在另一个线程读 `Bytes()`。静止画面测试不会暴露，一动就花屏/崩溃。
- **追查位置**：
  - `include/go2cpp/media/capture.hpp:14-19`（借用视图只活到下一次 `Capture()`，不能跨线程读）
  - `include/go2cpp/media/capture.hpp:85-89`（`zero_copy` 默认 false；只有 XShm 成功且格式可直读才生效，
    `XGetImage` 回退必须拷贝，因为 `XDestroyImage` 会释放图像）
  - `src/media/capture_x11.cpp:251-260`（`CaptureFromShm`：借用失败就回退 `CopyImage`）
  - `src/media/capture_x11.cpp:282-300`（`BorrowShmImage` 调 `out->Borrow(base, bytes_per_line)`，
    并原地改共享段的 alpha 字节）
  - `include/go2cpp/media/frame.hpp:75`（`borrowed` 裸指针）
  - `include/go2cpp/media/frame.hpp:125-131`（`Borrow` 清空并 `shrink_to_fit` 自有 `data`）
  - `src/media/pipeline.cpp:160`、`src/media/pipeline.cpp:188`（`*captured = *m_capture` 是
    `VideoFrame` 的成员级拷贝：`borrowed` 指针被复制，`data` 为空）
- **验收标准**（答到这些算过）：
  1. 说清 `VideoFrame` 的两种所有权：`borrowed == nullptr` 时 `data` 拥有像素；`borrowed != nullptr`
     时 `data` 为空、像素只读，`OwnsStorage()` 为 false。
  2. 说出 `SendNext(&out)` 的 `out` 不是快照：它复制的是「借用指针 + 空 data」，下一次
     `m_source->Capture(m_capture.get())` 会覆盖同一块 XShm 段；若中途 `InitShm` 重建/`CloseShm`，
     旧指针还可能指向已 detach 的段。
  3. 指出零拷贝生效的三个前提：`zero_copy=true`、`use_shm=true` 且 XShm 初始化成功、当前 XImage 是
     可直读的 BGRA 布局；任一不满足就走 owned 拷贝，此时跨帧持有反而是安全的。
  4. 给出至少两种正确用法：在本次 `SendNext` 返回后、下一次调用前消费 `Bytes()`；或显式把像素
     `memcpy` 进自有 `VideoFrame`（并 `Unborrow()`），或直接关闭 `zero_copy`。
  5. 指出原地填 alpha 改的是 XShm 共享段（X server 也映射着它），只动第 4 字节，像素三通道不变。
- **怎么自证**：先 `cmake --build .build-bento-wsl -j"$(nproc)" --target media_test_capture`，再跑
  `./.build-bento-wsl/media_test_capture`。它会逐字节比对 zero-copy 与 owned 两条路径——**全绿不代表
  跨帧安全**，因为用例是静止/同帧比对。再写一个 20 行探针：同一 `X11Capture` 连续 `Capture` 两次，
  第二次前修改画面，比较第一次 `Bytes()` 的内容 → 会看到第一次的视图已被第二次覆盖；开
  `zero_copy=false` 再跑同一探针 → 内容保持不变。报告里写清「借用视图的生命周期边界」。
- **常见错误答案**：「`*captured = *m_capture` 是拷贝，所以 out 可以一直用」「`Bytes()` 每次调用都会
  返回最新帧」「zero_copy 只是省一次 memcpy，语义不变」——零分。

---

## 二、编码与参考链

### Q02 【L4 陷阱】delta 编码因超限返回 false 后，参考帧已经更新——收端接下来会看到什么？

- **痛点**：只看 `return false`，没注意失败发生在 `m_ref.assign` **之后**；以为「这帧没发出去，
  下一帧会重新基于上一帧编码」。实际是编码器与解码器的参考链从此分叉，直到下一个关键帧才自愈。
- **追查位置**：
  - `src/media/codec_delta.cpp:195-200`（是否关键帧：`m_force_keyframe || geometry_changed || m_ref.empty()`；
    关键帧时先 `m_ref.assign` 并清 `m_force_keyframe`）
  - `src/media/codec_delta.cpp:274-277`（**先** `m_ref.assign(i420...)`，**再**判断
    `out->size() > kMaxEncodedFrameBytes`，超限才 `++m_oversize; return false`）
  - `include/go2cpp/media/codec.hpp:182-184`（`kMaxEncodedFrameBytes = 256 * 1200 - 64`，受
    VideoChannel 分片上限约束）
  - `src/media/pipeline.cpp:167-174`（每 `keyframe_interval` 帧强制关键帧；降级/重连请求也会强制）
  - `src/media/codec_delta.cpp:368-371`（解码端缺参考帧时整帧丢弃，等下一个关键帧）
- **验收标准**：
  1. 说出失败顺序：编码器已经把当前帧写进 `m_ref`，但 `Encode` 返回 false，`MediaSender::SendNext`
     直接返回 false，**这一帧没有进入传输**。
  2. 推导后果：下一帧仍是 delta（`m_force_keyframe` 已消费、几何未变、`m_ref` 非空），它引用的
     是收端从未收到的那一帧；收端 `m_ref` 与发端不同，画面错到下一个关键帧。
  3. 区分两种超限：关键帧超限 → 收端连参考都没有，后续 delta 全部被丢弃；delta 超限 → 收端能解但
     内容错。两者都只能靠关键帧恢复。
  4. 指出 `m_oversize` 与 `Name()` 会变成 `delta-i420(oversize!)`，但 `keyframes()` 不会增加；
     监控只看 `frames_sent` 会漏掉这类「编码成功但发送失败」的帧。
  5. 给出修复方向（任选）：超限时不更新 `m_ref`、或更新后立即 `ForceKeyframe()`、或把
     `kMaxEncodedFrameBytes` 与分片层预算统一。
- **怎么自证**：`cmake --build .build-bento-wsl -j"$(nproc)" --target media_test_codec && ./.build-bento-wsl/media_test_codec`
  先确认常规 delta 逐帧无损。再做超限实验：用 640×480 随机噪声（I420 一帧 460800B）连续编码，
  打印每帧 `Encode` 返回值、`header.keyframe`、输出字节数、`oversize_frames()`；观察「返回 false 的
  下一帧仍是非关键帧」。把同一串字节喂给 `DeltaDecoder`，比较解码结果与源帧 → 从失败帧之后开始不一致，
  直到手动 `ForceKeyframe()`。报告写清分辨率、噪声种子、关键帧周期。
- **常见错误答案**：「返回 false 就是这帧没编码，下一帧会重新参考上一帧」「超限只影响这一帧」
  「解码端会自动请求关键帧」——零分。

---

## 三、收端弱网与转发

### Q03 【L4 陷阱】打开 jitter 后，为什么「乱序重排/丢包恢复」统计永远是 0？这套接线到底买到了什么？

- **痛点**：照着文档以为 jitter 在「UDP 分片层」重排，能救回丢失/乱序的 1200B 分片；实际它接在
  `UdpVideoReceiver` **重组完成之后**，看到的是整帧再切出来的 8KB 块。
- **追查位置**：
  - `src/media/pipeline.cpp:357-364`（`OnEncodedFrame(frame_id, key, data, bytes)` 收到的是**整帧**；
    开 jitter 时先 `PushToJitter` 再 `DrainJitter`）
  - `src/media/pipeline.cpp:393-431`（`PushToJitter` 把整帧按 `jitter_chunk_bytes` 切块，
    每块带 12B 块头，`++m_jitter_seq` 后逐块 `Push`；同一帧的所有块在**一次函数调用内连续入队**）
  - `src/media/pipeline.cpp:274-284`（jitter 配置：容量至少 1024 块块头 + 负载，
    `startup_buffering=false`）
  - `include/go2cpp/media/pipeline.hpp:26-33`（设计注释自己写明「重组出的整帧再切块」，
    并解释为什么必须切：`JitterBuffer` 单包上限 64KB）
  - `src/media/jitter_buffer.cpp:90-160`（`Push` 按包序号定位、判重、判迟到；空洞只在
    `Pop` 的 `m_hole_deadline_ns` 到期时跳过）
- **验收标准**：
  1. 说清数据流：UDP 分片 → `VideoChannel` 重组 → **完整帧** → 切 8KB 块 → `JitterBuffer`
     → 按块序拼回 → 解码/渲染。
  2. 推导统计：同一帧的块序号连续且无并发插入，块与块之间不可能乱序；跨帧的块也不会交错，
     所以 `reordered`/`duplicates` 在正常路径恒为 0；网络层真正的乱序/丢片早在重组器就被处理
     （丢片=整帧丢弃），jitter 看不到。
  3. 说出它真实买到的东西：给整帧增加一个可配置的排队延迟（吸收帧到达抖动），以及容量上限内的
     帧级 FIFO；代价是固定延迟和额外的切块/拷贝。
  4. 指出「丢一个分片后 jitter 能救」是错的：重组器已经丢弃整帧，jitter 收到的是下一整帧；
     差分编码下这会打断参考链，仍要靠关键帧。
  5. 说出 `startup_buffering=false` 的影响：首帧不额外攒满 `target_delay`，但后续仍按目标深度排队。
- **怎么自证**：`cmake --build .build-media-e2e -j"$(nproc)" --target media_test_e2e_jitter && ./.build-media-e2e/media_test_e2e_jitter`。
  在测试里注入 UDP 分片乱序（`media_test_transport` 已有乱序用例），同时打印
  `receiver->jitter_stats().reordered/duplicates/lost` 与 `jitter_frames_dropped()`：
  期望看到乱序被重组器吃掉、jitter 的 `reordered` 仍为 0；把同一组乱序直接喂给 `JitterBuffer::Push`
  （绕过 pipeline）才会看到非 0。两者对比即可证明「jitter 不在分片层」。
- **常见错误答案**：「jitter 在 UDP 层重排 1200B 分片」「开了 jitter 就能从丢片恢复」
  「reordered=0 说明网络没乱序」——零分。

### Q04 【L3 陷阱】SFU 转发的是指针：订阅者把 `SfuFragment` 存进队列，为什么读到的是下一帧？

- **痛点**：把 `ForwardFragment` 当成「投递一份数据」，在 sink 里把 `fragment.data` 存起来异步处理；
  或以为回调里 `Unsubscribe` 会让正在遍历的 vector 失效。
- **追查位置**：
  - `include/go2cpp/media/sfu.hpp:8-13`（三条不变量：流归 publisher 所有；失活立即扣计数、
    真正删除延迟到 `forward_depth==0`；转发热路径不分配）
  - `src/media/sfu.cpp:655-662`（`SfuFragment` 在栈上构造，`fragment.data = data`、
    `fragment.payload = payload`，**都是借用指针，不拷贝**）
  - `src/media/sfu.cpp:668-687`（按索引遍历订阅，`subscription->sink(fragment)` 在**当前线程同步**调用）
  - `src/media/sfu.cpp:149-160`（`ReleaseSubscription` 只置 `active=false` 并扣 peer 计数，不 erase）
  - `src/media/sfu.cpp:186-193`（`Cleanup()` 在 `forward_depth>0` 时只置 `cleanup_pending`，不回收）
  - `src/media/sfu.cpp:694-697`（转发深度归零后才真正 `Cleanup()`）
- **验收标准**：
  1. 说清 `data` 的所有权：它指向 `SfuPublisher` 调用 `ForwardFragment` 时的线格式缓冲；
     `SfuFragment` 只在本次 sink 调用期间有效，sink 返回后发布方可以复用该缓冲。
  2. 说出正确做法：sink 内同步消费，或把 `bytes` 拷贝进自己的队列/引用计数块；不能只存指针。
  3. 说清「回调里退订/离开」为什么安全：只失活不 erase，`forward_depth` 阻止 `Cleanup` 重入，
     索引遍历的 `total` 在循环前取定，元素对象仍存活；计数在失活时立即扣减，所以回调内查询一致。
  4. 说出 `require_keyframe_start` 的门控：新订阅必须从关键帧**首片**开始，否则重组器永远缺片；
     被门控的片计入 `fragments_keyframe_gated`。
  5. 指出热路径的取舍：为了 0 分配/0 锁，SFU 不做深拷贝，也不在转发中做背压；订阅者慢会直接
     拖慢发布者（同步回调）。
- **怎么自证**：`cmake --build .build-bento-wsl -j"$(nproc)" --target media_test_sfu && ./.build-bento-wsl/media_test_sfu`
  （3000 步随机拓扑 + 1e6 帧吞吐）。再写探针：sink 里只保存 `fragment.data` 到 vector，发布方连发
  两帧不同图案，循环结束后读第一帧指针 → 内容等于第二帧（或已释放）；把 sink 改成 `memcpy` 后再跑
  → 内容稳定。ASAN 构建下可把「发布方在回调返回后释放缓冲」的版本跑出 use-after-free。
- **常见错误答案**：「`SfuFragment` 里存的是 shared_ptr，随便持有」「回调里 Unsubscribe 会
  use-after-free」「SFU 会为每个订阅者拷贝一份 payload」——零分。

### Q05 【L3 陷阱】44.1 kHz 立体声接 48 kHz Opus，为什么 AVX2 重采样没生效？

- **痛点**：看到 `SelectedAudioKernel()` 返回 `avx2`、文档又有 44.1→48 的加速数字，就以为所有
  44.1→48 路径都走了 SIMD；实际 SIMD 内核只处理**单声道**。
- **追查位置**：
  - `src/media/audio.cpp:746-772`（`ResampleLinear`：`channels == 1 && CpuHasAvx2()` 才走
    `ResampleLinearAvx2Mono`，否则 `ResampleLinearScalarRange`）
  - `src/media/audio.cpp:696-740`（`ResampleLinearAvx2Mono`：8 输出/轮，`vpgatherdd` 一次取
    `(in[idx], in[idx+1])`，只在末端 clamp 区之前走 SIMD，余下交标量）
  - `src/media/audio.cpp:654-694`（`ResampleLinearScalarRange`：单/双声道交错的通用实现）
  - `src/media/audio.cpp:1269-1287`（`SelectedAudioKernel()` 只反映 CPU 能力，不反映本次调用
    实际走的分支）
  - `src/media/audio.cpp:1596`、`src/media/audio.cpp:1701`（编码/解码侧的重采样调用点，把
    `format.channels` 原样传入）
- **验收标准**：
  1. 指出分派条件是 `channels == 1`，不是「有 AVX2 就加速」；双声道必然回退标量。
  2. 说出 `SelectedAudioKernel()` 的语义边界：它是**进程级 CPU 探测**，不是「本帧实际内核」；
     用它做性能归因会把标量路径误记成 AVX2。
  3. 说出 SIMD 内核为什么只做单声道：`vpgatherdd` 一次 gather 两个相邻样本，双声道交错时
     `idx+1` 是另一个声道，插值语义不成立；要做双声道得改成按声道分离或 8 样本跨步 gather。
  4. 指出文档里的 44.1→48 AVX2 数字是**单声道口径**；立体声场景要用标量数据，或先 deinterleave。
  5. 说出正确性约束：SIMD 与标量必须逐样本一致（同一 16.16 定点 `step`、同样的 clamp 与舍入），
     否则左右声道/不同 CPU 会产生不同音频。
- **怎么自证**：`cmake --build .build-bento-wsl -j"$(nproc)" --target media_test_audio && ./.build-bento-wsl/media_test_audio`。
  然后写计时探针：同一段 44100→48000、10 秒正弦，分别用 `channels=1` 和 `channels=2` 调
  `ResampleLinear`，各跑 1000 次取中位数；单声道应明显快于双声道（比值自己测，不引用文档）。
  再用 `perf stat -e instructions` 对比，确认双声道路径没有 AVX2 gather 指令。
- **常见错误答案**：「AVX2 探测通过就都走 SIMD」「双声道是两条单声道，应该同样快」
  「`SelectedAudioKernel()==avx2` 说明这次重采样用了 AVX2」——零分。
