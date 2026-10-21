# 并发原语调试监控开销基线（2026-10-21）

> 结论先行：**调试监控的主要成本不在「写一条事件」，而在「每条事件都抓栈/Fiber 链」。**
> 全开（`all` + `stacks/chains` 全采样）让 Mutex 慢 **192×**、RWMutex 慢 **66×**、
> 混合场景慢 **65×**；把栈/链捕获关掉（其余全量记录）后分别只慢 **5.9× / 7.8× / 4.2×**。
> 单次事件编码实测 26 ns，单次全采样 `CaptureStackSample` 实测 **660 ns**（25×），
> 多线程争用下还会放大到微秒级。

## 1. 测量范围与方法

### 1.1 五种监控粒度

同一份 `tests/bench/concurrency_baseline.cpp` 分别链接 OFF / ON 两个 Release 构建，
ON 二进制用环境变量切换粒度：

| 配置 | 环境变量 | 记录什么 |
|---|---|---|
| `OFF` | 无（OFF 构建） | 宏完全消除，零事件 |
| `SAMPLED` | `GO2CPP_DEBUG_CATEGORIES=park,sched`、`SAMPLE_SHIFT=5` | 只看 park/sched，1/32 采样 |
| `SAMPLED_ALL` | `CATEGORIES=all`、`SAMPLE_SHIFT=5` | 全类别，1/32 采样 |
| `FULL_NO_CAPTURE` | 全类别但排除 `stacks,chains`、`SHIFT=0` | 全量事件编码 + 资源路径，不抓栈/链 |
| `FULL` | `CATEGORIES=all`、`SHIFT=0`、`STACK_SAMPLE=1`、`FIBER_CHAIN_SAMPLE=1` | 全量事件 + 每次 backtrace/Fiber 链 |

`FULL_NO_CAPTURE` 与 `FULL` 的差就是栈/链捕获成本；`SAMPLED_ALL` 用于检验
「采样位移」是否真的省成本（结论：**不省资源事件**，见 §4.2）。

### 1.2 场景与工作量

9 个业务场景 + 5 个微基准，每场景 `N=50000`（微基准同 N），延迟另跑一轮
`LAT_N=5000` 采样。每配置 3 轮，每轮独立进程，取中位数。

| 场景 | 操作定义 | 线程/协程 |
|---|---|---|
| `mutex4` / `mutex8` | N 次 `sync::Mutex` Lock/Unlock | 4 / 8 OS 线程 |
| `rwmix` | 6 读 2 写混合访问 `RWMutex`，N 次 RLock/RUnlock 或 Lock/Unlock | 8 OS 线程 |
| `mpmc` | 4 生产者 + 4 消费者，cap=64 channel 传 N 条消息 | 8 Fiber（Scheduler 8 worker） |
| `pc_rtt` | 两个 G 用 `ParkingCondition` + `std::mutex` ping-pong，N 次往返 | 2 Fiber |
| `wg` | 4 线程各 `WaitGroup::Done`，主线程一次 `Wait`，共 N 次 Done | 4 OS 线程 |
| `select2` / `select4` | 1 发 1 收，在 2/4 路 channel 上 `Select` N 次 | 2 Fiber |
| `mixed` | 2 个 worker 每轮 Mutex + Channel Send + PC notify，另有 consumer/helper | 4 Fiber |
| `record` | 直接调 `EventRing::Record` N 次 | 1 线程 |
| `record_sampled32` | `RecordSampled(every=32)` N 次 | 1 线程 |
| `lookup` | `FindResourceByAddress` N 次 | 1 线程 |
| `stack` | `CaptureStackSample(0,false)` N 次 | 1 线程 |
| `config` | `DebugConfig::Enabled(kPrimitives)` N 次 | 1 线程 |

### 1.3 口径与机器

- 吞吐 = ops / 墙钟；延迟 = 逐操作 `steady_clock`，含约 20–30 ns 计时成本。
- 事件量 = `EventRing::TotalCountAll()` 累计增量；OFF 构建恒 0。
- 「窗口覆盖倍数」= 事件数 /（活跃 ring 数 × 320 槽）。事件环**没有** dropped
  计数器；>1 表示环形窗口至少被覆盖一遍，历史事件必有丢失。这是下界，不是精确条数。
- 机器：WSL2 Ubuntu，12 vCPU，Release（`-O3 -DNDEBUG`），loadavg 1.59（< 2）。
- ON 构建：`build-debug-conc`（`DEBUG_DUMP=ON`、`REPLAY=OFF`、容量 256+64）。
  用纯净构建而不是 `build-debug-full`，避免 REPLAY 钩子混入对比。

## 2. 总表（关键行）

完整表见 `tests/bench/results/concurrency_baseline_20261021-213553.md` 与
`..._summary.tsv`；原始轮次见 `..._213553.tsv`。

### 2.1 吞吐（ops/s，3 轮中位数）

| 场景 | OFF | SAMPLED | SAMPLED_ALL | FULL_NO_CAPTURE | FULL | FULL/OFF |
|---|---:|---:|---:|---:|---:|---:|
| mutex4 | 22,479,669 | 21,807,958 | 3,698,898 | 3,812,419 | 117,004 | **192.1×** |
| mutex8 | 18,583,181 | 21,601,967 | 3,023,987 | 3,549,810 | 121,853 | **152.5×** |
| rwmix | 3,680,455 | 2,825,315 | 109,485 | 472,795 | 56,100 | **65.6×** |
| mpmc | 7,756,971 | 2,381,347 | 693,154 | 1,650,495 | 673,718 | 11.5× |
| pc_rtt | 2,752,882 | 1,054,390 | 206,987 | 904,406 | 171,444 | 16.1× |
| wg | 28,541,942 | 17,688,521 | 9,796,406 | 11,773,997 | 2,902,524 | 9.8× |
| select2 | 1,011,322 | 229,088 | 89,065 | 210,090 | 64,054 | 15.8× |
| select4 | 699,409 | 163,811 | 75,453 | 152,936 | 51,768 | 13.5× |
| mixed | 75,689,856 | 36,313,783 | 6,658,651 | 17,998,246 | 1,163,185 | **65.1×** |

### 2.2 p50 / p99（ns）

| 场景 | OFF p50 | FULL p50 | p50 倍数 | OFF p99 | FULL p99 | p99 倍数 |
|---|---:|---:|---:|---:|---:|---:|
| mutex4 | 32 | 3,115 | 97× | 1,693 | 607,069 | 359× |
| mutex8 | 32 | 2,235 | 70× | 825 | 1,112,512 | **1,348×** |
| rwmix | 43 | 18,050 | 420× | 14,924 | 914,502 | 61× |
| mpmc | 30 | 1,253 | 42× | 39,676 | 287,454 | 7.2× |
| pc_rtt | 681 | 10,887 | 16× | 841 | 26,159 | 31× |
| wg | 85 | 1,043 | 12× | 1,735 | 4,098 | 2.4× |
| select2 | 1,191 | 18,512 | 15.5× | 2,786 | 53,544 | 19× |
| select4 | 1,516 | 21,647 | 14.3× | 1,839 | 59,893 | 33× |
| mixed | 43 | 3,344 | 78× | 4,822 | 318,927 | 66× |

### 2.3 事件量与窗口

| 场景 | FULL 事件/操作 | 窗口覆盖倍数（FULL） | 主要事件构成 |
|---|---:|---:|---|
| mutex4/8 | 2.35 | 46–92× | `mutex`（fast+slow） |
| rwmix | 9.22 | 180× | `mutex` 31 万、`rw` 11 万、`cv` 3.8 万 |
| mpmc | 1.25 | 43× | `ch` 10.6 万为主 |
| pc_rtt | 13.0 | **1,016–1,354×** | `park` 30 万、`wake` 20 万、`sched` 40 万、`pc` 30 万、`life` 10 万 |
| wg | 1.00 | 31× | `wg` 5 万 |
| select2/4 | 18.8 / 20.3 | 490–633× | `ch` 29–37 万、`sched` 20 万、`park` 15 万 |
| mixed | 0.84 | 157× | `mutex` 10.2 万、`ch` 10.7 万、`sched` 1.3 万 |

`dropped_threads=0`、`stack_samples dropped=0`（栈表未满 4096）；但窗口覆盖
31–1354×，意味着默认 320 槽/线程在并发原语下只能保留最后 **0.07%–3%** 的事件。

## 3. 成本分解

### 3.1 栈/链捕获是最大头（FULL vs FULL_NO_CAPTURE）

| 场景 | FULL_NO_CAPTURE | FULL | 栈/链带来的额外慢倍数 |
|---|---:|---:|---:|
| mutex4 | 3,812,419 | 117,004 | **32.6×** |
| mixed | 17,998,246 | 1,163,185 | **15.5×** |
| rwmix | 472,795 | 56,100 | **8.4×** |
| pc_rtt | 904,406 | 171,444 | 5.3× |
| select2 | 210,090 | 64,054 | 3.3× |
| mpmc | 1,650,495 | 673,718 | 2.4× |

单线程微基准直接量化了这一层：

| 微基准 | OFF | ON（全量事件） | FULL（栈全采样） |
|---|---:|---:|---:|
| `record`（事件编码+环写+计数） | 1.61 ns | 26.4 ns | 26.1 ns |
| `record_sampled32`（1/32 命中） | 1.89 ns | 4.7 ns | 4.7 ns |
| `lookup`（资源注册表） | 1.85 ns | 8.7 ns | 8.5 ns |
| `stack`（backtrace+栈表锁） | 1.84 ns | 2.4 ns（类别关） | **660 ns** |
| `config`（类别掩码读） | 0.48 ns | 1.1 ns | 1.1 ns |

`stack` 的 660 ns 是单线程、无争用下的下限；8 线程全开时 backtrace 与
栈表 `std::mutex` 互相争用，实测每条事件摊销到微秒级（Mutex 8 线程 p50
从 2.2 µs 起、p99 到 1.1 ms）。

### 3.2 事件编码+资源路径（FULL_NO_CAPTURE vs OFF）

| 场景 | 慢倍数 | 事件/操作 | 估算额外成本/事件 |
|---|---:|---:|---:|
| mutex4/8 | 5.2–5.9× | 2.1–2.2 | ~100 ns |
| rwmix | 7.8× | 8.2 | ~226 ns |
| mpmc | 4.7× | 1.4 | ~336 ns |
| pc_rtt | 3.0× | 13.0 | ~57 ns |
| select2/4 | 4.6–4.8× | 18.8–20.3 | ~200–250 ns |
| mixed | 4.2× | 0.81 | ~52 ns（6 ops/轮摊薄） |

单线程 `record` 只有 26 ns/条，业务场景却到 57–336 ns/条，差额来自：
资源事件结构体构造、`CaptureFiberChain()` 调用（即使类别关闭也要走宏分支）、
`GetTotals().codes[..].fetch_add`（同类事件集中的 counter 争用）、以及
事件写环导致的 cache 污染。

### 3.3 采样位移没有省下资源事件（SAMPLED_ALL vs FULL_NO_CAPTURE）

| 场景 | SAMPLED_ALL | FULL_NO_CAPTURE | 事件/操作（两者） |
|---|---:|---:|---:|
| mutex4 | 3,698,898 | 3,812,419 | 2.26 / 2.15 |
| rwmix | 109,485 | 472,795 | 8.10 / 8.16 |
| pc_rtt | 206,987 | 904,406 | 13.00 / 13.00 |
| select2 | 89,065 | 210,090 | 18.76 / 18.80 |
| mixed | 6,658,651 | 17,998,246 | 0.80 / 0.81 |

两者事件数几乎相同，但 `SAMPLED_ALL` 在 rwmix 上还慢 4.3×、pc 慢 4.4×。
原因是 `GO2CPP_DEBUG_SAMPLE_SHIFT` 只作用于 `RecordSampled` 的快路径事件与
**非 critical** 的栈捕获；阻塞/唤醒类资源事件每次都记，`critical=true` 的
栈/链捕获也无视采样（`CaptureStackSample` 中 `!critical` 才按周期跳过）。
所以「采样配置」要么把 primitives 类别整个关掉（`SAMPLED`，Mutex/RWMutex
回到接近 OFF），要么几乎不省（`SAMPLED_ALL`），中间没有可用的旋钮。

## 4. 瓶颈排序（按预期收益）

### P0：栈/Fiber 链捕获路径（收益 2.4–32×）

- 占比：FULL 的额外成本中 70–95% 来自这一层（§3.1）。
- 关键代码：`src/debug/debug_event_ring.cpp:1715` `CaptureStackSample`、
  `src/concurrency/fiber.cpp` 的 `CaptureFiberChain`；默认每 16/8 次采样，
  但 critical 全量，且 8 线程共享一个栈表 `std::mutex`。
- 建议：默认关闭 `stacks/chains`；critical 也按 1/16–1/64 采样；backtrace
  移到低优先级线程/批量 flush；栈表按线程分片或改成无锁读。
- 预期：FULL 回落到接近 FULL_NO_CAPTURE——Mutex 192×→~6×（**32×** 提升）、
  混合 65×→~4×（**15×**）、RWMutex 66×→~8×（**8×**）、Mutex8 p99
  1.1 ms→~3 µs（**300×+**）。

### P1：资源事件条数与事件编码（收益 1.5–5×）

- 占比：FULL_NO_CAPTURE 相对 OFF 的 4.2–7.8×，以及全开下每事件 57–200 ns 的底噪。
- 证据：select 18.8 条/op、pc 13 条/op、rwmix 9.2 条/op；`record` 26 ns/条
  是最低下限。一次等待被 park enter/return、wake request/enqueued、pc wait/notify、
  sched enqueue/push/pop/claim、G lifecycle 重复描述。
- 建议：合并成对的 park/wake 为一条「交接」事件；sched 的
  runqueue push/pop/claim 默认关闭或 1/64；`TotalCount` 改成 per-thread
  计数、批量 flush；资源事件按「每 K 次操作记录 1 次」采样。
- 预期：事件量降 3–5×，FULL_NO_CAPTURE 从 4–8× 降到 2–3×；对 select/pc
  收益最大（事件最肥）。

### P2：采样语义（收益 3–8×，在保留原语事件的前提下）

- 现状：shift 对资源事件无效，critical 栈捕获也无效；`SAMPLED_ALL` 仍
  9.3–33.6×，而 `SAMPLED`（关闭 primitives）只有 1.3–4.4×。
- 建议：给资源事件单独的采样周期（按原语/操作类型），并让 critical 栈
  捕获受 shift 约束；提供「原语事件 1/N + 栈 1/M」两个独立旋钮。
- 预期：`SAMPLED_ALL` 从 9–34× 降到 2–5×；无需牺牲全部原语事件。

### P3：环形窗口容量与保留率（可观测性，不直接提速）

- 现状：窗口覆盖 31–1354×，dump 只保留最后 0.07%–3%；`Snapshot()` 里
  正常并发下还会因撕裂校验丢条。
- 建议：先靠 P1/P2 把事件量降下来；关键事件用独立大环；容量按线程分片
  或动态扩展；dump 增加「覆盖倍数/截断」标记（现在只有 `seq_at_start/end`）。
- 预期：保留率从 <3% 到可用；吞吐无直接收益，容量盲目增大会增加 cache 压力。

### P4：资源注册表查找（收益 <10%）

- 单次 `FindResourceByAddress` 8.5 ns（shared_mutex + hash）；热路径多数
  已缓存 `resource_id`，不是主成本。若要优化：无锁读或每线程缓存。

### 原语本身（非 debug 层，供下一轮参考）

- `RWMutex` 混合：OFF p99 14.9 µs / p50 43 ns（347×），写者优先下读者排队，
  是原语层尾延迟的主要来源；全开后 p99 914 µs。
- `Channel` MPMC：OFF 吞吐 7.76 M ops/s（cap=64 命中），但 p99 39.7 µs
  （p50 30 ns 的 1300×），瓶颈在阻塞唤醒/调度而非事件（仅 1.25 条/op）。
- `Mutex`：OFF p50 32 ns（fast path），8 线程 p99 825 ns（26×）；全开后
  p99 1.11 ms（1348×），说明 debug 层把争用尾延迟放大得最厉害。

## 5. 覆盖矩阵与未覆盖项

| 原语 | 本次场景 | 结论适用性 |
|---|---|---|
| `sync::Mutex` | mutex4 / mutex8 / mixed | 实测 |
| `RWMutex` | rwmix / mixed | 实测 |
| `Channel<T>` | mpmc / select2 / select4 / mixed | 实测 |
| `ParkingCondition` | pc_rtt / mixed | 实测（Fiber 路径；native 路径 RTT 约 78 µs，被 futex 唤醒主导） |
| `WaitGroup` | wg | 实测 |
| `ConditionVariable` | rwmix（内部 CV） | 间接实测（未单列） |
| `RecursiveMutex` | 无 | 未实测；事件码 `kRecursiveLock/Unlock` 与 Mutex 同路径，结论可外推但无数据 |
| `TimedMutex` | 无 | 未实测；复用 `Mutex::LockFor` |
| `SpinMutex` | 无 | 未实测；无等待路径，事件量应接近 Mutex fast path |
| `FutexMutex` | 无 | 未实测；native 等待路径，事件应接近 PC native |

未覆盖的原因：方向修正后以「debug 开销对比」为主线，优先覆盖有代表性的
6 类原语与 5 种监控粒度；剩余 4 类的事件记录点与已测原语共用
`src/concurrency/sync.cpp` 的同一套宏，成本结构相同，但**没有实测数据**。

## 6. 复现

```bash
cd /UserData/CodexWorkSpace/Go2Cpp-work

# 1) 纯净 ON 构建（DEBUG_DUMP=ON、REPLAY=OFF）
cmake -S . -B build-debug-conc \
  -DCMAKE_BUILD_TYPE=Release \
  -DGO2CPP_DEBUG_DUMP=ON \
  -DGO2CPP_REPLAY=OFF \
  -DGO2CPP_BUILD_TESTS=OFF \
  -DGO2CPP_BUILD_EXAMPLES=OFF
cmake --build build-debug-conc -j12

# 2) 全量基线（9 业务场景 + 5 微基准 × 5 配置 × 3 轮）
bash tests/bench/run_concurrency_baseline.sh

# 3) 单场景 / 单配置
/tmp/go2cpp-concurrency-baseline/bin/conc_ON \
  --scenario=mutex8 --n=50000 --lat-n=5000 --config=FULL
```

脚本产出：
`tests/bench/results/concurrency_baseline_<ts>.tsv`（原始轮次）、
`..._summary.tsv`（中位数+倍数）、`...md`（数据表），并将同一份 Markdown
写到 `/tmp/concurrency_baseline.md`。

## 7. 边界与已知限制

- 数据来自 WSL2 单机，绝对 ns 不代表裸机/生产；比较的是同机相对开销。
- 延迟含逐操作 `steady_clock` 成本；全开场景 p99 受调度抖动影响，3 轮中位数
  只降低而不消除噪声。
- `FULL` 的 `STACK_SAMPLE=1`/`FIBER_CHAIN_SAMPLE=1` 是「每次捕获」的极端配置；
  默认（1/16、1/8）介于 `FULL_NO_CAPTURE` 与 `FULL` 之间。
- 事件环无 dropped 计数器；「窗口覆盖倍数」是历史丢失的下界，不是精确条数。
- 未改 `src/**`，未 commit；`build-debug-conc` 被 `.gitignore` 的 `build-*/` 覆盖。
- 线程版 Channel（普通 OS 线程 Send/Recv）在极高频率混合负载下出现过一次挂起，
  本次 `mixed` 改用 Fiber 路径；该现象未复现定位，另有风险记录，不影响本文
  的 Fiber/线程原语对比数据。


## 8. 优化后（v7，2026-10-21 22:15）

> 同一 `tests/bench/run_concurrency_baseline.sh`、同一机器。改前数据见 §2/§3
> （`concurrency_baseline_20261021-213553`）；本轮
> `concurrency_baseline_20261021-221537`（3 轮中位数）。
> 采集时环境有并行任务（`cpptools` 占 1 核，loadavg 2.2–2.8），因此用
> `LOAD_MAX=4` 纳入有效样本；同一轮内 OFF 与各配置相邻运行，相对倍数仍可比。
> 另有 loadavg<2 的交叉验证轮 `..._220832`，关键倍数差异 <20%。

### 8.1 结论（目标 vs 实测）

| 指标 | 改前 | 改后 | 验收目标 | 结果 |
|---|---:|---:|---:|---|
| FULL `mutex4` 慢倍数 | 192.1× | **4.8×** | ≤30× | 达标（40×↓） |
| FULL `mutex8` 慢倍数 | 152.5× | **4.9×** | — | 31×↓ |
| FULL `mutex8` p99 | 1,112,512 ns | **3,846 ns** | — | 289×↓ |
| SAMPLED_ALL `rwmix` 慢倍数 | 33.6× | **6.2×** | ≤8× | 达标（5.4×↓） |
| 事件/操作 `mutex4` | 2.35 | **0.40** | 降 2×+ | 5.9×↓ |
| 事件/操作 `rwmix` | 9.22 | **2.20** | 降 2×+ | 4.2×↓ |
| 事件/操作 `mpmc` / `mixed` | 1.25 / 0.84 | **0.40 / 0.20** | — | 3.1× / 4.2×↓ |
| 窗口覆盖 `mutex4` | 46–92× | **14.8×** | — | 保留率 ×3–6 |
| 窗口覆盖 `rwmix` | 180× | **42.3×** | — | 保留率 ×4.3 |
| 微基准 `Record` | 26.1 ns | 25.8 ns | 不退化 | 持平 |
| 微基准 `CaptureStackSample` | 660 ns | 667 ns | 不退化 | 持平（显式全量语义保留） |
| 微基准 `FindResourceByAddress` | 8.5 ns | 7.8 ns | — | 持平 |
| 微基准 `Enabled` | 1.1 ns | 1.1 ns | — | 持平 |

### 8.2 改了什么

| # | 改动 | 位置 | 作用 |
|---|---|---|---|
| 1 | 资源事件栈/链**独立采样周期**（默认 1/16，再乘全局 `SAMPLE_SHIFT`），critical 同样受约束 | `debug_event_ring.cpp` `CaptureStackSampleForResource` / `fiber.cpp` `CaptureFiberChainForResource` | 消除「FULL 下每条 Mutex ok 事件都 backtrace+Fiber 链」这一 70–95% 成本 |
| 2 | 资源事件**记录**采样（默认 1/8），只稀释非关键结果；blocked/wake/timeout/cancelled/closed 全量 | `ResourceEventWanted` + 宏内提前判定 | 事件量降 4–6×；关键配对不丢 |
| 3 | sched 高频事件默认 1/8（enqueue/requeue/push/pop/claim/GStarted/GResumed） | `EventRing::Record` | pc/select 的事件噪声降 8×（可用 `GO2CPP_DEBUG_SCHED_SAMPLE=1` 恢复） |
| 4 | 栈样本表 **16 分片**（按帧哈希选分片，跨线程去重语义不变） | `StackShard` / `StackTable` | 多线程 backtrace 后的全局 mutex 争用消失 |
| 5 | 累计计数 **16 分片**（线程 id 取模），读取时求和仍精确 | `EventTotals` | 去掉同类事件在多核上的 cacheline 乒乓 |
| 6 | 采样判定前移到**宏内、结构体构造之前** | `debug_resources.hpp` | 被稀释的 7/8 调用零构造、零跨模块调用 |
| 7 | `WriteSlot` 除 seq 外**整块 memcpy** | `debug_event_ring.cpp` | 每条事件 19 次 store → 2 条 cacheline 批量写 |
| 8 | **资源事件改写入关键区**（P1 正确性修复） | `RecordResourceEvent` | 普通区 300 条噪声不再冲掉 lock(blocked)/unlock(wake)；代价是关键区容量 64 成为新的保留上限 |
| 9 | `WaitNode` 的 `gid()/managed()/scheduler()` 改读**原子快照**（TSAN 修复） | `sync.cpp` | 消除 `Reset()` 写 `shared_ptr<Task>` 与无锁读的数据竞争 |

### 8.3 吞吐（ops/s，3 轮中位数）与相对 OFF 倍数

| 场景 | OFF | SAMPLED | SAMPLED_ALL | FULL_NO_CAPTURE | FULL | FULL 慢倍数（改前→改后） |
|---|---:|---:|---:|---:|---:|---:|
| mutex4 | 27,698,345 | 17,961,067 | 8,023,847 | 9,881,401 | 5,744,463 | 192.1× → **4.8×** |
| mutex8 | 23,362,499 | 17,713,418 | 7,970,860 | 7,684,119 | 4,724,406 | 152.5× → **4.9×** |
| rwmix | 3,298,420 | 3,143,632 | 532,188 | 634,502 | 372,777 | 65.6× → **8.8×** |
| mpmc | 6,459,614 | 1,771,939 | 1,562,066 | 1,572,903 | 1,283,229 | 11.5× → 5.0× |
| pc_rtt | 2,803,244 | 1,612,497 | 1,271,082 | 1,323,132 | 867,289 | 16.1× → 3.2× |
| wg | 31,948,445 | 17,359,208 | 14,905,214 | 25,830,802 | 15,480,476 | 9.8× → 2.1× |
| select2 | 882,542 | 116,911 | 112,545 | 118,713 | 108,263 | 15.8× → 8.2× |
| select4 | 714,503 | 82,358 | 78,884 | 80,669 | 76,962 | 13.5× → 9.3× |
| mixed | 76,375,368 | 27,603,000 | 21,583,310 | 21,693,778 | 17,683,244 | 65.1× → 4.3× |

### 8.4 p50 / p99（ns）

| 场景 | OFF p99 | FULL p99 | p99 倍数（改前→改后） |
|---|---:|---:|---:|
| mutex4 | 991 | 6,467 | 359× → **6.5×** |
| mutex8 | 597 | 3,846 | 1,348× → **6.4×** |
| rwmix | 7,479 | 562,658 | 61× → 75×（原语层尾延迟主导，见 §8.7） |
| mpmc | 48,731 | 213,774 | 7.2× → 4.4× |
| pc_rtt | 1,005 | 4,238 | 31× → 4.2× |
| wg | 1,373 | 1,287 | 2.4× → 0.9× |
| select2 | 1,525 | 37,600 | 19× → 24.7× |
| select4 | 2,206 | 42,620 | 33× → 19.3× |
| mixed | 5,450 | 20,729 | 66× → 3.8× |

### 8.5 事件量与窗口覆盖

| 场景 | FULL 事件/操作（改前→改后） | 窗口覆盖（改前→改后） | 主要构成（改后） |
|---|---:|---:|---|
| mutex4/8 | 2.35 → **0.40** | 46–92× → **14.8× / 6.0×** | `mutex` |
| rwmix | 9.22 → **2.20** | 180× → **42.3×** | `mutex`/`rw`/`cv` |
| mpmc | 1.25 → **0.40** | 43× → **15.4×** | `ch` |
| pc_rtt | 13.0 → **8.60** | 1,354× → **898×** | `park`/`wake`/`pc`/`sched` |
| wg | 1.00 → **0.10** | 31× → **3.9×** | `wg` |
| select2/4 | 18.8 / 20.3 → **12.0 / 12.2** | 490–633× → **313–318×** | `ch`/`sched`/`park` |
| mixed | 0.84 → **0.20** | 157× → **35×** | `mutex`/`ch` |

select/pc 的剩余事件以 park/wake/资源交接为主（关键集合，不允许采样），
这也是它们在 FULL 下仍保留 8–9× 的原因。

### 8.6 微基准（单次调用）

| 微基准 | OFF | SAMPLED_ALL | FULL | 说明 |
|---|---:|---:|---:|---|
| `record` | 1.5 ns | 26.3 ns | 25.8 ns | 编码+环写+分片计数 |
| `record_sampled32` | 1.8 ns | 5.0 ns | 5.0 ns | 采样判定（位与替代取模） |
| `lookup` | 1.8 ns | 8.2 ns | 7.8 ns | 资源注册表 |
| `stack` | 1.8 ns | 8.6 ns | 667.2 ns | 显式 `CaptureStackSample(0,false)`，`STACK_SAMPLE=1` 时仍全量（有意保留语义） |
| `config` | 0.5 ns | 1.1 ns | 1.1 ns | 类别掩码 |

### 8.7 采样配置（新增/修正）

| 环境变量 | 默认 | 作用 |
|---|---:|---|
| `GO2CPP_DEBUG_RESOURCE_SAMPLE` | 8 | 资源事件记录周期（非关键结果；相位 1 必记） |
| `GO2CPP_DEBUG_RESOURCE_STACK_SAMPLE` | 16 | 资源事件栈捕获周期（critical 同样受约束，再乘 `SAMPLE_SHIFT`） |
| `GO2CPP_DEBUG_RESOURCE_CHAIN_SAMPLE` | 16 | 资源事件 Fiber 链周期 |
| `GO2CPP_DEBUG_SCHED_SAMPLE` | 8 | 调度器高频事件记录周期（1=恢复全量） |
| `GO2CPP_DEBUG_STACK_SAMPLE` | 16 | **显式**栈捕获周期（非 critical），语义不变 |
| `GO2CPP_DEBUG_FIBER_CHAIN_SAMPLE` | 8 | **显式**链捕获周期（非 critical） |
| `GO2CPP_DEBUG_SAMPLE_SHIFT` | 0 | 全局降采样位移，叠加到捕获周期（不再影响资源事件记录周期） |

采样相位统一为「第 1 次必记」，因此低频操作（< N 次）不会被稀释，
短测试与单次操作的可观测性不变。

### 8.8 正确性

- `ctest --test-dir build-debug-full`：**131/131 通过**（含新增的采样语义场景 9）。
- `build-work`（OFF）完整构建成功，`go2cpp_tests` 通过；`NowNs()` 已移出
  `GO2CPP_DEBUG_DUMP` 宏保护（修复 OFF 构建编译错误）。
- `tests/concurrency/run_tsan.sh`：**status: CLEAN**（0 data race，4 场景 PASS）；
  修复 `WaitNode` 的 `gid()/managed()` 与 `Reset()` 的 `shared_ptr<Task>` 竞争。
- `tests/debug/test_debug_record_correctness.cpp`：**PASS**（配对、游标、
  多线程不丢、采样语义、环形保留、类别过滤）。
- P1 验收：普通区写爆 1000 条后，Mutex 慢路径 blocked 事件 `before=1 after=1`
  （改前 `before=1 after=0`）。
- 资源事件采样语义：`SetDebugEventSampleEvery(4,1)` 下 40 次 fast-path lock
  记录 10 条（1/4），10 次 blocked 记录 10 条（关键全量）。

### 8.9 已知限制与后续

- **rwmix** 的 FULL 仍 8.8×（p99 562 µs，相对 OFF 75×）。它的瓶颈已不是事件量
  （2.2 条/op），而是「debug 事件在 RWMutex 持锁/交接路径上把等待队列放大」；
  FULL_NO_CAPTURE 也有 5.2×。要进一步降低需要把事件写环移出临界区
  （例如每线程批量缓冲 + 后台 flush），属于下一轮工作。
- **select2/4** 仍 8–9×：剩余事件是关键 park/wake/ch，按「关键事件不丢」的
  约束不能采样；它们的 p99 由原语层唤醒路径主导。
- 资源事件改写入关键区后，保留窗口从「普通区 256」变成「关键区 64」；
  高频长跑下只保留最近 64 条资源事件（P1 的明确取舍：优先保证不被普通噪声
  覆盖，而不是保留更多条）。需要更长历史时应增大
  `GO2CPP_DEBUG_EVENT_CRITICAL_CAPACITY`。
- 本轮采集环境有并行任务（loadavg 2.2–2.8，`LOAD_MAX=4`），绝对 ns 与
  loadavg<2 的基线不严格同条件；跨轮交叉验证（`..._220832`）差异 <20%。
