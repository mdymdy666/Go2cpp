# 抖动缓冲与降级策略（JitterBuffer + Degradation）

> 模块：`go2cpp::media`
> 文件：`include/go2cpp/media/jitter_buffer.hpp`、`include/go2cpp/media/degradation.hpp`
> 实现：`src/media/jitter_buffer.cpp`、`src/media/degradation.cpp`
> 测试：`tests/media/test_jitter_buffer.cpp`、`tests/media/test_degradation.cpp`
> 构建：`cmake -S . -B .build-bento-wsl -DGO2CPP_WITH_BENTO=ON -DGO2CPP_WITH_MEDIA=ON`
> 定位：补齐 `docs/modules/webrtc-media.md` §10 中列出的最后两块缺口——**抖动缓冲**与**弱网降级**。
> 环境：WSL2 / Ubuntu 24.04 / AMD Ryzen 9 8945HX (12 vCPU, Zen4) / 19GB

---

## 1. 范围与结论

| 项 | 状态 | 数据 |
|---|---|---|
| 按序号重排 | ✅ | 2048 帧块内乱序 → 100% 按序交付，0 违序 |
| 去重 | ✅ | 500 帧 ×3 副本 → 交付 500、重复丢弃 1000 |
| 丢包检测 | ✅ | 5% 丢包 → `lost_packets`=99/100（末包不可检测，见 §2.6） |
| 自适应深度 | ✅ | ±50ms 抖动 → J=35.26ms、target=90.53ms（固定 20ms 时 24.5k 次乱序） |
| 迟到帧 | ✅ | 默认丢弃；`deliver_late` 时 8 槽补发队列，逐字节正确 |
| 低分辨率 | ✅ | 320x240 → 160x120，2:1 盒式，逐像素精确，帧字节 1/4 |
| 低帧率 | ✅ | 30fps 输入 → 30/15/10fps 实际放行 300/150/99 |
| 断线重连 | ✅ | 1s 可疑 / 3s 断线 / 8 次退避 100→5000ms / 失败态 / 收包即恢复 |
| 码率自适应 | ✅ | 与 `bento::BandwidthEstimator` 联动：2M→600k→250k 触发 3 级降 |
| Push 性能 | ✅ | **21.66 ns**（1e6 次中位数，含 1200B 拷贝），1.44× 实测 memcpy |
| Pop 性能 | ✅ | **3.13 ns**（零拷贝视图），离 L1 下限约 2.1× |
| 内存有界 | ✅ | 常量 798,720 B；1e6 帧后堆增长 1,216 B（`printf` 噪声） |

**一句话**：两块缺口都已落地且可逐帧/逐包验证；热路径零堆分配、零锁、零原子，
反汇编证据见 §6。

---

## 2. JitterBuffer

### 2.1 为什么需要它

`transport.hpp` 把一帧切成 `<=1200B` 的分片走 UDP。`VideoChannel` 的重组层
**缺一片就整帧丢弃**；而 `kDeltaI420` 依赖参考帧，丢一帧会让后续差分帧全部
无法解码。所以必须在重组之前对**包级序号**做重排与等待，把抖动吸收在有界窗口内。

### 2.2 排序键：为什么用 seq 而非 timestamp

RTP 的同一帧拆成多个包时共享同一个 `timestamp`，用它排序会产生大量并列。
本实现用**单调递增的包序号 `seq`** 作为排序键，`timestamp` 只用于 RFC 3550 的
抖动估计：

```
D = (到达间隔) - (媒体时间戳间隔)
J = J + (|D| - J) / 16            // EWMA，整数除法，无浮点
target = clamp(base + J * 2.0, min, max)
```

默认 `base=20ms`、`min=10ms`、`max=400ms`、`factor=2.0x`。

### 2.3 容器：环形数组

| 候选 | 许可证 | Push | Pop | 本项目的取舍 |
|---|---|---|---|---|
| libwebrtc `video_coding/jitter_buffer.cc` | BSD-3 | O(log n) | O(log n) | 依赖 `RtpFrameObject`/`Clock`/`VCMEncodedFrame` 整套 RTC 生态，移植成本不可接受 |
| pion/interceptor `pkg/jitterbuffer` | MIT | 最坏 O(n)（`memmove` 插入） | O(1) | Go 实现，算法简单但接口无法直接复用 |
| GStreamer `rtpjitterbuffer` | LGPL-2.1 | O(log n) | O(1) | **许可证与本项目 MIT 不兼容** |

结论：**借鉴算法语义（RFC 3550 抖动估计 + 目标深度 + 空洞超时跳过），不移植代码**。
理由是三者要么许可证不兼容、要么强绑定外部生态；而本项目的包序号在窗口内连续、
窗口上限固定，环形数组 `slot = seq & (capacity-1)` 就是最优解：

- Push / Pop 均摊 **O(1)**：2 次数组寻址 + 1 次 `memcpy`；
- 构造期一次性分配元数据与 payload 池，**热路径零堆分配**；
- 元数据 512 × 24B = 12KB，常驻 L1。

> 检索环境限制（如实记录）：WSL 内 `getent hosts github.com` 返回回环，
> `web_fetch` 对 `raw.githubusercontent.com` 报「域名解析到内网/本机地址」，
> 因此上述实现是依据检索到的公开资料与已知算法完成的，**未逐行比对上游源码**。

### 2.4 空洞与跳过

`Pop(out, now)` 的行为：

1. 迟到补发队列非空 → 先吐补发（仅 `deliver_late`）；
2. 缓冲空 → `kEmpty`；
3. 启动缓冲（`startup_buffering`）且尚未交付过首包 → 攒够 `target` 再开始；
4. 循环：
   - `next_seq` 槽命中 → 零拷贝视图 + `kOk`；
   - `next_seq` 槽空且 `highest <= next` → 只是还没到，`kEmpty`；
   - 有更晚的包（说明是真空洞）→ 首次设定 `deadline = now + target`，
     `now < deadline` 返回 `kEmpty`；
   - 超时（或 `PopFlush`）→ `lost_packets++` 并 `next_seq++`；
     **一旦本次调用判定过超时，后续空洞连续跳过**（否则连丢 10 个包要花 10 个
     渲染 tick 才追得上）。

### 2.5 迟到帧

`seq < next_seq` 即已错过交付点：

- 默认**丢弃**（视频解码器依赖参考帧顺序，乱序补发只会让它重置）；
- `deliver_late = true` 时进入 8 槽 FIFO 补发队列，`Pop` 以 `kLateDelivered`
  优先吐出，`view.late = true`；
- 超出 `late_tolerance`（默认 64 个序号）的迟到一律丢弃；
- 例外：**首帧尚未交付时允许向前重锚**，用于吸收「第一个包本身就乱序」的启动期问题。

### 2.6 断流重锚与 32 位回绕

- `seq` 跳变 `>= capacity` → 判定流重启：清空窗口、重锚到新序号，返回
  `kDiscontinuity`（`discontinuities++`）。不这么做会永久追赶一个不存在的序列。
- 新旧判定统一用 `int32_t(seq - next_seq)` 差值，天然处理 32 位回绕；
  时间戳同理。测试从 `0xFFFFFF00` 起连发 600 个包跨越 `0xFFFFFFFF → 0`，
  零误判。
- 5% 丢包测试中 `lost_packets = 99` 而非 100：**最后一个被丢的包后面没有更晚的
  包到达**，按定义无法判定为空洞。这是算法固有边界，不是缺陷。

### 2.7 性能

条件：`.build-bento-wsl` Release（`-O3`），`taskset -c 2`，负载 `load average 9.87`，
1,056,000 次 Push + 1,056,000 次 Pop；数据为 6 次运行的中位数。

| 操作 | 实测 | 实测基线 | 理论下限 | 倍数 |
|---|---|---|---|---|
| `Push`（1200B） | **21.66 ns** | `memcpy(1200B)` = 15.00 ns | ~10.7 ns（L1 写带宽，见下） | **1.44× memcpy / 2.0× 理论** |
| `Pop`（零拷贝） | **3.13 ns** | L1 延迟 1.09 ns | ~1.5 ns | **2.1×** |
| 低负载复测 | 18.7 ns / 2.7 ns | memcpy 13.2 ns | — | 1.42× |

下限推导：
- Zen4 每周期 32B store，1200B 需 37.5 cycle；3.5GHz → **10.7 ns**（读与写并行，
  写端口是瓶颈）。glibc `memcpy` 实测 13.2~15.0 ns，本身已在 1.23~1.4× 下限。
- `Push` 索引开销 = 21.66 - 15.00 = **6.66 ns**；其内容为 1 次 meta 读 + 1 次池
  地址计算 + 1 次 meta 写 + 抖动 EWMA + 分支，理论约 3.5 ns → **1.9×**。
- `Pop` 的 3.13 ns ≈ 11 cycle，含 meta 读、占位清零、序号推进、held 计算 —— 2.1×。

优化实验（负结果，如实记录）：把 `UpdateJitter`/`UpdateDepth`（共 54 条指令）
强制 `always_inline` 到 `Push`，`Push` 从 189 条膨胀到 225 条，实测 21.2 ns
（非内联同负载 19.2 ns）——**没有改善，反而在噪声内变慢**，i-cache 占用抵消了
call/ret 的成本（现代 CPU 的 RSB 预测 call/ret 几乎零开销）。已回退，保持简单。

> memset 只在 `Reset`/`ResetTo`（冷路径）出现；`operator new[]` 只在构造函数出现，
> 见 §6 的重定位表。

---

## 3. Degradation（降级策略）

### 3.1 四档阶梯

| 档位 | 分辨率 | 帧率 | 标称码率 | 最低可用带宽 |
|---|---|---|---|---|
| `kFull` | 320x240 | 30 | 800 kbps | 900 kbps |
| `kReduced` | 320x240 | 15 | 450 kbps | 520 kbps |
| `kLow` | 160x120 | 15 | 200 kbps | 240 kbps |
| `kMinimal` | 160x120 | 10 | 120 kbps | 140 kbps |

**先降帧率再降分辨率**：帧率对「实时性观感」影响最小，且不改变编码器的尺寸假设；
分辨率一降，单帧字节数直接落到 1/4（`BgraFrameBytes` 307200 → 76800）。

### 3.2 决策与滞回

`Update(now, sample)` 的判定顺序（前者优先）：

1. `loss >= loss_severe(12%)` → 期望保底档；
2. `loss >= loss_downgrade(5%)` 且不在冷却期 → 期望降一级；
3. `loss < loss_recover(2%)` 且带宽足够上一级的 `1.15×` → 期望升一级；
4. 否则若带宽不足以维持当前档 → 降到「能维持的最高档」。

时序（滞回）：

- **降级**：条件持续 `downgrade_hold_ms = 200ms` 后生效，一次最多
  `max_downgrade_step = 2` 级（避免一刀切到底）；
- **升级**：条件持续 `upgrade_hold_ms = 3000ms` 后生效，一次只升一级；
- **丢包冷却**：中等丢包降级后 `loss_cooldown_ms = 1000ms` 内不再因丢包降级
  （否则持续 6% 丢包会在 1 秒内连降到保底档）；严重丢包不受冷却限制；
- **重连静默**：`OnReconnected()` 后 `reconnect_guard_ms = 500ms` 内不升级。

带宽用 EWMA 平滑（`alpha = 0.5`，移位 1）。默认取 0.5 而不是 0.25 的原因：
BBR 的 `delivery_rate` 本身已是 1s 滑窗值，再重平滑会让降级迟钝
（实测 0.25 时从 1.5Mbps 降到 640kbps 需要 ~1s，期间已丢帧）。

### 3.3 与 BBR 的联动

```cpp
bento::BandwidthEstimator estimator;      // Bento 的 RFC 9002 delivery-rate 简化
auto stats = estimator.Snapshot(now_ms);  // delivery_rate_bps / loss_rate
NetworkSample sample = NetworkSample::FromBbr(stats);
controller.Update(now_ms, sample);
```

本模块**只消费** BBR 的估计，不反向修改拥塞控制——职责边界清晰，便于单独回归。
测试用真实的 `BandwidthEstimator` 产生 ACK 序列（2Mbps → 600kbps → 250kbps），
驱动出 `full → reduced → low → minimal`。

码率有两层：

- `target_bitrate_bps()`：档位标称值（离散）；
- `adaptive_bitrate_bps()`：`clamp(平滑带宽 × 0.9, 标称/4, 标称)`（档内连续），
  用于跟上带宽的缓慢变化。

### 3.4 帧率控制

`ShouldDeliverFrame(now)` 用**相位累积**而不是简单间隔判断：

```
if (!paced) { paced = true; next_due = now + interval; return true; }
if (now >= next_due) {
    next_due += interval;
    if (next_due <= now) next_due = now + interval;  // 落后太多就重对齐，不补帧
    return true;
}
return false;
```

切档时重置相位（`ApplyLevel`），否则旧档的相位会带进新档、造成第一秒抖动。
实测 30fps 输入 → 30fps 档 300/300、15fps 档 150/300、10fps 档 99/300
（99 而非 100 是 `1000/10 = 100ms` 与 33ms tick 的相位边界，误差 1%）。

### 3.5 断线重连

```
kConnected --(1s 无包)--> kSuspect --(3s 无包)--> kDisconnected
     ^                                                   |
     |                                          (退避到期) v
     +------------------ OnPacket / 成功 <-------- kReconnecting --(8 次用尽)--> kFailed
```

- 退避 `initial << n`，封顶 `max_backoff_ms = 5000ms`，叠加 ±20% 确定性抖动
  （xorshift32，种子可配）→ 测试可复现、生产避免同步风暴；
- 实测序列：第 n 次 Probe 之后安排的间隔 = 200/400/800/1600/3200/5000/5000/5000 ms；
- 恢复到连接态时置位 `ConsumeKeyframeRequest()`——**重连后画面能否恢复取决于
  编码器是否立刻发关键帧**，缺了这一步解码器会一直缺参考帧；
- `kFailed` 后仍按 `max_backoff_ms` 周期性返回 `kExhausted`，交给上层决定
  是否降档或重建会话。

### 3.6 2:1 下采样

`Downscale2xBgra` 做 4 像素整数平均（`(sum + 2) >> 2` 四舍五入），
`320x240 → 160x120` 逐像素与手工计算一致（0 mismatch）。
GCC 把内层 4 通道循环向量化成 320 条指令的 SSE 版本（见 §6）。
奇数尺寸 / 空指针直接返回，不写越界。

---

## 4. 测试

全部用例确定性（固定种子），不 sleep，用模拟纳秒/毫秒时钟。

### 4.1 `media_test_jitter_buffer`（17,269 断言，0.18s）

| # | 场景 | 断言要点 |
|---|---|---|
| 1 | 顺序流 2000 帧 | 全交付、零重复/迟到/丢包 |
| 2 | 负载完整性 | 16 种长度 93..1488B，逆序到达，零拷贝内容逐字节一致 |
| 3 | 块内乱序 2048 帧 | 100% 按序；`reordered=1718`、`max_buffered=32` |
| 4 | 重复包 ×3 | 交付 500、`duplicates=1000` |
| 5 | 5% 随机丢包 | 交付 1900；`lost_packets=99`（末包不可检测） |
| 6 | 连续丢 10 个 | 一次 `Pop` 跳过整段（`lost_packets=10`） |
| 7 | 迟到 | 丢弃策略 2 例；补发策略 1 例逐字节正确 |
| 8 | 断流重锚 | `9 → 5000` 触发 `kDiscontinuity` 并重锚 |
| 9 | 32 位回绕 | 600 帧跨 `0xFFFFFFFF→0`，零误判 |
| 10 | 非法输入/配置规范化 | `nullptr`/0B/70KB 拒绝；`capacity=0→8`；时间倒退安全 |
| 11 | **抖动 ±50ms，1e5 帧** | **100.000% 交付、0 违序**；target 90.53ms、J 35.26ms；held p50 21.08 / p99 77.03 ms |
| 12 | **压测 1e6 帧** | push 21.66 ns / pop 3.13 ns；固定内存；堆增长 1,216 B |

### 4.2 `media_test_degradation`（184 断言，0.00s）

| # | 场景 | 断言要点 |
|---|---|---|
| 1 | 带宽阶梯 1.5M→700k→500k→200k→100k | full→reduced→low→minimal，3 次降级 |
| 2 | 丢包触发 6% / 15% | 各降一级 / 两步降到保底（`max_downgrade_step=2`） |
| 3 | 恢复 3Mbps | 2.9s 内不升、3s 后逐级升回，`upgrades=3` |
| 4 | 帧率落地 | 30fps 输入 → 300/150/99 |
| 5 | 断线重连 | suspect 1s / disconnect 3s / 8 次退避 / failed / 收包恢复 |
| 6 | 退避抖动 | 8 次均落在 ±20% 区间内 |
| 7 | **BBR 联动** | 真实 `BandwidthEstimator` → `FromBbr` → 三级降档 |
| 8 | 滞回 | 150ms 低 / 150ms 高 ×20 周期 → 0 次切档 |
| 9 | 重连重建 | 关键帧请求只消费一次、静默期不升档 |
| 10 | 下采样 | 320x240→160x120 逐像素 0 mismatch、帧字节 1/4、奇数尺寸/空指针安全 |
| 11 | 随机带宽 1000 次 | 状态自洽（事件 from/to、fps 与档位一致） |

### 4.3 复现

```bash
cmake -S . -B .build-bento-wsl -DGO2CPP_WITH_BENTO=ON -DGO2CPP_WITH_MEDIA=ON
ninja -C .build-bento-wsl media_test_jitter_buffer media_test_degradation
taskset -c 2 .build-bento-wsl/media_test_jitter_buffer
taskset -c 2 .build-bento-wsl/media_test_degradation
cd .build-bento-wsl && ctest -R media_ --output-on-failure     # 11/11 PASS
```

---

## 5. 如何接进现有管线

两个模块都是新增、独立的，**不修改任何既有文件**（只往 `cmake/Go2CppMedia.cmake`
追加了源文件与测试目标）：

```
UdpVideoReceiver::Pump
   └─ VideoChannel 重组（每帧 1200B 分片）
        └─ [新增] JitterBuffer.Push(seq, ts, payload, bytes, now)
             └─ [新增] JitterBuffer.Pop(&view, now) → VideoDecoder::Decode(view.data, view.bytes)

BBR BandwidthEstimator.Snapshot
   └─ [新增] NetworkSample::FromBbr → DegradationController.Update
        ├─ target_width/height  → 采集/编码尺寸
        ├─ ShouldDeliverFrame   → 每帧是否投递
        ├─ adaptive_bitrate_bps → 编码器目标码率
        └─ [新增] ReconnectSupervisor.Tick → 发起重连 / kExhausted
             └─ OnPacket 恢复 → ConsumeKeyframeRequest → 编码器强制关键帧
```

`seq` 建议直接用 `VideoChannel` 的 `frame_id`（或分片序号），`timestamp` 用
`capture_ns` 换算到 90kHz。

---

## 6. 反汇编证据

命令（`.o` 阶段，`-O3`）：

```bash
objdump -d --no-show-raw-insn .build-bento-wsl/CMakeFiles/go2cpp_media.dir/src/media/jitter_buffer.cpp.o
readelf -r .build-bento-wsl/CMakeFiles/go2cpp_media.dir/src/media/jitter_buffer.cpp.o
```

| 函数 | 指令 | call | 条件跳转 | lock | 分配 | PLT/GOT |
|---|---|---|---|---|---|---|
| `JitterBuffer::Push` | 189 | 4（UpdateJitter、memcpy、ResetTo、PushLate） | 20 | 0 | **0** | 0 |
| `JitterBuffer::PopInternal` | 142 | **0** | 17 | 0 | **0** | 0 |
| `JitterBuffer::UpdateJitter` | 37 | 0 | 1 | 0 | 0 | 0 |
| `JitterBuffer::UpdateDepth` | 17 | 0 | 1 | 0 | 0 | 0 |
| `DegradationController::ShouldDeliverFrame` | 37 | 0 | 3 | 0 | 0 | 0 |
| `DegradationController::Update`（冷） | 224 | 4 | 29 | 0 | 0 | 0 |
| `ReconnectSupervisor::OnPacket` | 14 | 0 | 1 | 0 | 0 | 0 |
| `ReconnectSupervisor::Tick`（冷） | 80 | 2 | 10 | 0 | 0 | 0 |
| `Downscale2xBgra`（SSE 向量化） | 320 | 0 | 17 | 0 | 0 | 0 |

重定位表结论：

- `operator new[]`（`_Znam`）只出现在**构造函数**（3 处），`operator delete[]`
  只在析构（5 处）——热路径零分配被字节码级证实；
- `memcpy` 两处：`Push`（0x8f7）与 `PushLate`（0x7cb）；`memset` 三处：
  构造函数、`Reset`、`ResetTo`（全是冷路径）；
- **无 `lock` 前缀、无 `xchg`、无原子指令**——单线程设计，热路径无同步开销；
- `PopInternal` **零 call**：交付路径全部内联，没有函数调用开销。

---

## 7. 已知边界与未覆盖

| 项 | 说明 |
|---|---|
| 单线程 | `Push`/`Pop` 不得并发；跨线程由调用方加锁（这是 ns 级的前提） |
| 补发队列固定 8 槽 | 极端突发的补发会丢弃多出的迟到帧（计入 `dropped_late`） |
| 末包丢包不可检测 | 算法固有：没有更晚的包就无法区分「丢失」与「还没到」 |
| 未接进 `pipeline.cpp` | 本次按边界要求只新增模块，未改动既有管线代码 |
| 分辨率切换需要采集端配合 | `Downscale2xBgra` 提供像素操作，但 `X11Capture` 未接动态尺寸 |
| 未与真实 UDP 弱网联合压测 | 用模拟到达序列覆盖抖动/丢包；联合压测属于下一步 |
| 升级策略是逐级的 | 一次只升一级，从 `kMinimal` 回到 `kFull` 至少 9s |
| BBR 只取两个字段 | `rtt_ms` 留 0（`BandwidthStats` 里没有平滑 RTT） |

---

## 8. 相关文档

- `docs/modules/webrtc-media.md`：采集/编码/传输/渲染闭环（本模块的上游）
- `docs/modules/bento-transport.md`：`VideoChannel` 分片与重组
- 压测与反汇编原始数据：`/tmp/jitter_degradation.md`
