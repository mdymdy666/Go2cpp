# 性能整改与 Coost 对比

> **阅读须知（2026-10-10 追加）**：本文是跨多轮的**累积记录**。各节数字只在其
> 标题日期对应的源码、构建类型、基准参数和 WSL 宿主条件下成立，**不得跨节横向
> 比较，也不得单独取其中最好的一节对外宣称**。同一负载参数（2,000 Fiber /
> 32 普通线程 / 206,400 次临界区）在不同轮次分别记录过约 `99～107 ms`、
> `8.5～8.8 s`、`6.7～6.9 s`，以及 30 轮的 `9/19/31/51 ms`（最小/中位数/P95/最大），
> 差异来自轮次间的调度配置、编译选项与宿主抖动；本文未在统一条件下重测，因此
> **不存在单一“当前值”**。需要当前结论时以日期最新的一节为准（现为
> 「2026-10-08：混合 Mutex 快速等待路径」）。更早各节保留原文，用以反映演进
> 过程与失败尝试。





## 调度器 worker 扩展性（condition 换 futex 后）

> **条件**：`build-bench`（Release + -O2）、AMD Ryzen 9 8945HX / WSL2、
> `debug_observer_benchmark` 档 0（不装观察者）、20,000 任务、3 轮取 median、
> **测量时负载 1.44**。

| workers | BEFORE | AFTER | 改善 |
|---:|---:|---:|---:|
| 1 | 1,553.0 | 1,357.3 | 1.14x |
| 2 | 2,031.8 | **949.0** | **2.14x** |
| 3 | 12,196.9 | **2,165.7** | **5.63x** |
| 4 | 24,376.7 | **3,817.5** | **6.39x** |

**倍率**：BEFORE 1.00 / 1.31 / 7.85 / 15.70x → **AFTER 1.00 / 0.70 / 1.60 / 2.81x**。

**改动**：调度器的 `std::condition_variable condition` 换成 futex 化的
`core::ParkingCondition`（接口签名完全一致，27 处调用点未改）。

**判读**：这确认了 worker 数量级劣化的根因是**调度器 park/unpark 走 glibc cv**。
4 worker 仍有 2.81x，剩余共享点是 `m_impl->mutex`。

---
## sync::Mutex 的高竞争（专项探针）

> **条件**：临时探针（`/tmp`，未入库）、`build-quality` 库、N 线程反复 lock/unlock 同一把
> `sync::Mutex`、rounds=20000、**测量时负载 0.80**。**与其它章节不可横向比较。**

| 线程数 | 每次加解锁 |
|---:|---:|
| 2 | **117.2 ns/op** |
| **4** | **29,847.3 ns/op** |

**判读**：把 `sync::Mutex` 的普通线程等待路径换成 futex 之后，4 线程争用**仍然**是万 ns 级
（改造前记录约 18,000 ns/op，同量级）。

**这说明瓶颈不在等待原语**，而在 `Unlock` 的交接逻辑：每次 unlock 都在持队列锁的情况下
逐个 `TryFinish` 认领候选者，4 线程时这个 O(等待者数) 循环 + 队列锁争用成为主要成本。
它与 worker 扩展性劣化很可能是同一个结构性问题。

---
## ParkingCondition futex 化（第 2 天）

> **条件**：`build-bench`（Release + -O2）、AMD Ryzen 9 8945HX / WSL2、
> `benchmarks/parking_condition_benchmark.cpp`、rounds=20000、**测量时负载 1.43**。
> 走**普通线程路径**（不绑定 Scheduler）。**与其它章节不可横向比较。**

| 场景 | BEFORE（`condition_variable_any`）| AFTER（futex 无状态）| 变化 |
|---|---:|---:|---:|
| A notify 空转（无等待者）| 15.30 ns/次 | 16.26 ns/次 | 0.94x（噪声）|
| C 争用（2 消费者）| 90.76 ns/次 | **71.74 ns/次** | **1.27x（快 21%）** |
| C 争用（4 消费者）| 4,351.02 ns/次 | **1,727.95 ns/次** | **2.52x（快 152%）** |

**判读**：收益随等待者数量增长。原 `condition_variable_any` 在 4 个等待者时退化到 4.3 µs，
**加入 128 次自旋预算后**降到 1.73 µs（省 2.62 µs，2.5 倍），且 2 个等待者也从 90.76 → 71.74 ns（快 21%）—— **三个场景全部不劣于原实现**。自旋让「唤醒已在路上」的窄窗口完全避开系统调用。

后续优化方向见 `DebugTooling/futex-refactor-attempt.md` 第 4 节
（`FUTEX_WAIT` 前短暂自旋、把 `waiters` 与 `generation` 编码进同一个字）。

---
## 调度器 worker 扩展性（第 2 天修复后）

> **条件**：`build-bench`（Release + -O2）、AMD Ryzen 9 8945HX / WSL2、
> `debug_observer_benchmark` 档 0（不装任何观察者）、20,000 任务、每次 3 轮取 median、
> **测量时负载 1.19 → 1.48**。**与其它章节不可横向比较。**

| workers | 修复前（ns/任务）| 修复后（ns/任务）| 修复前倍率 | 修复后倍率 |
|---:|---:|---:|---:|---:|
| 1 | 1,363.2 | 1,884.4 | 1.00x | 1.00x |
| 2 | 1,759.2 | 1,193.5 | 1.29x | 0.63x |
| 3 | 10,027.8 | **5,119.0** | 7.36x | **2.72x** |
| 4 | 22,013.9 | **13,173.8** | 16.15x | **6.99x** |

**改动**：① `emit_event` 零开销快路径（未装观察者时不取时钟、不加锁、不拷 `shared_ptr`）；
② `worker_loop` 的本地队列检查移出全局 `admission_lock`。

**判读**：劣化从 7.36x/16.15x 降到 2.72x/6.99x（改善约 2 倍），但**未消除**。
逐项排除的假设与微基准数据见 `DebugTooling/diag-worker-scaling-2.md`。

---
## 2026-10-17 事件采集线程局部批量提交 vs 逐条提交

> **本节条件**：`build-bench`（Release + `-O2`）；宿主 AMD Ryzen 9 8945HX（12 线程）/ WSL2；
> 每档事件数 200000，先热身一轮再做 **5 轮**取 min/median/max；
> 基准 `benchmarks/debug_event_collector_benchmark.cpp`（本轮新增）。
> **与其它章节不可横向比较**。

| 场景 | flush_batch=1（等价逐条） | flush_batch=16 | flush_batch=32（默认） |
|---|---:|---:|---:|
| A 单线程 median | 22.47 ns/事件 | **19.65** | 19.70 |
| B 4 线程 median | 11.69 ns/事件 | **7.65** | 11.54 |
| B 4 线程 min/max | 11.02 / 13.31 | 7.16 / 11.33 | 7.49 / 11.81 |

**结论**

1. **批量化收益真实但不大**：单线程 **约 12.5%**（22.47 → 19.65 ns/事件）。
   这与预期一致 —— `EventBuffer` 本身已是**无锁**环形缓冲，批量提交省的是
   「读 head / 判满 / 写 head」的**原子操作次数**，而不是锁争用。
2. **多线程下表面收益更大（约 35%），但方差显著**（min 7.16 / max 11.81），
   因此不宜把"35%"当作稳定值对外表述。
3. **`flush_batch` 从 16 增到 32 没有可测差异**（19.65 vs 19.70、7.65 vs 11.54 后者落在噪声内）
   —— 说明 **16 已接近饱和**；默认值 32 保留无碍，但也没有必要上调。
4. **一项容易误读的观察**：B 场景的 `ns/事件` 反而低于 A 场景（11.69 vs 22.47）。
   这**不是**"多线程下单次更快"，而是分母被 4 个线程**并行摊薄**了 ——
   本表所有数字都是"总耗时 / 总事件数"，不能跨场景比较单次成本。

**未做的事**：没有对 `PushBatch` 内部做进一步拆分（例如一次预留 N 条时是否值得用 CAS 而非
"读-判-写"三段），也没有测更大 `flush_batch`（64/128）或更高并发（8/16 线程）。

---

## 2026-10-16 D3 锁争用选择钩子（WaitSelector）开销

> **本节条件**：`build-bench`（Release + `-O2`）；宿主 AMD Ryzen 9 8945HX（12 线程）/ WSL2；
> 每档**先热身 1 轮（不计入）**再做 3 轮取 min/median/max；
> 场景 A 无竞争 = 单线程 `TryLock`/`Unlock` 20 万次/轮；
> 场景 B 有竞争 = 4 线程各 2 万次 `Lock`/`Unlock`/轮；
> 基准 `benchmarks/debug_lock_selector_overhead_benchmark.cpp`。**与其它章节不可横向比较**。

| 档 | 场景 A 无竞争 median | 场景 B 有竞争 min/median/max |
|---|---:|---|
| 0 未注册选择器（框架默认） | 3.71 ns/op | 15,785 / **18,416** / 19,842 ns/op |
| 1 注册"不干预"选择器 | 3.78 ns/op | 23,907 / （输出被截断）|
| 2 注册"队尾优先"选择器 | 3.76 ns/op | 16,776 ns/op（独立单轮测量）|

**结论**：

1. **无竞争路径三档完全一致**（3.68 ~ 4.03 ns/op，差异在噪声量级）——
   证实选择器在 `Mutex::Unlock` 的 `compare_exchange` 快路径上**零开销**：
   该路径根本不会进入 `ReorderWaitersForSelector`。
2. **有竞争路径的 13~24 µs/op 不是选择器引入的**：
   档 0 **完全没有注册选择器**，其竞争成本本身就是 15.8 ~ 19.8 µs/op，
   与档 2 的 16.8 µs/op 同一量级。因此**选择器不增加可测量的开销**。
3. **附带发现（独立于调试模块，仅供排查线索，本节不作结论）**：
   `Mutex` 在 4 线程高竞争下约 **18 µs/op**。作为对照，
   同一宿主上单线程无竞争仅 3.7 ns/op —— 两者相差约 5000 倍。
   这属于 `sync::Mutex` 自身的竞争行为，**不是本模块引入的**，
   本模块仅在文档中记录该观察，不声称原因。

**未完成项（如实记录）**：有一次使用默认参数（`5` 轮 × 无竞争 200 万次迭代）的运行
**超过 15 分钟未结束**，且因输出未 flush 而丢失。改用分档单跑 + 逐轮 `fflush(stdout)`
后每次都能在数秒内完成，因此**本次结论不依赖那次运行**；
但那次超时的**根因尚未定位**，留作未决项，不作为任何结论的依据。

---

## 2026-10-16 调度器观察者（调试插件）开销三档对照

> **本节条件**：`build-bench`（Release，基准目标另加 `-O2`）；宿主 AMD Ryzen 9 8945HX（12 线程）/ WSL2；
> 任务数 20000，worker 数 1/2/4；每档**先跑 1 轮热身（不计入）**再做 3 轮取 median；
> 基准 `benchmarks/debug_observer_overhead_benchmark.cpp`。**与其它章节不可横向比较**。

| worker 数 | 档0 observer=nullptr | 档1 原子计数 | 档2 环形缓冲+消费线程 |
|---:|---:|---:|---:|
| 1 | 1,601.6 ns/任务 | 1,656.7（+3.4%） | 1,698.9（+6.1%） |
| 2 | 1,345.2 | 1,295.4（噪声） | 1,600.9（+19.0%） |
| 4 | **19,914** | 24,444（+22.7%） | 23,165（+16.3%） |

结论：① 单 worker 下观察者净开销 **+3~6%**（约 27 ns/事件），机制本身很轻；
② worker 数增加时开销比例上升，其中包含 `emit_event` 内 `observer_mutex` 的多 worker 争用；
③ **4 worker 的基线本身异常**（1,602 → 19,914 ns，约 12 倍），属**调度器自身的扩展性问题**，
与观察者无关；因此 4 worker 下的 +22.7% 建立在异常基线上，**不宜作为典型开销对外表述**。

另记一条方法教训：初版基准**没有热身轮**，导致先测的档显著偏慢（档 0 曾比档 1 慢约 20 倍），
属测量错误；加热身轮后数据才可用。

---

## 2026-10-15 Channel 缓冲改为环形结构（对齐 Go hchan）

> **本节条件**：`build-bench`（Release，基准目标另加 `-O2`）；宿主 AMD Ryzen 9 8945HX / WSL2；
> 每项 **3 次运行 × 3 轮 = 9 个 median 样本**，下表给「最好 / 中位 / 最差」；
> 参数与上一节相同（SPSC 50,000；MPMC 4×12,500；ping-pong 10,000；空探测 100,000）。
> **与本文其它章节不可横向比较**。

### 改动内容

`Channel<T>` 的缓冲由 `std::deque<T>` 改为环形结构 `detail::RingBuffer<T>`，对齐 Go `hchan` 的
`buf` / `dataqsiz` / `sendx` / `recvx` / `qcount` 模型：

1. **预分配原始存储 + 就地构造**：`capacity` 个槽位一次分配（不构造元素），`capacity == 0` 时完全不分配
   —— 对应 Go 的 `dataqsiz == 0`（无缓冲通道）；
2. **索引推进 + 回绕，不做取模**：`if (++tail == capacity) tail = 0;`
   —— 与 `chan.go` 的 `c.sendx++ / if c.sendx == c.dataqsiz { c.sendx = 0 }` 一致；
   初版用了 `% capacity` 求槽位，锁内除法在 MPMC 场景下明显拖慢，已按 Go 的写法去掉；
3. **接口保持 `std::deque` 的子集**（`size`/`empty`/`front`/`emplace_back`/`pop_front`/`pop_back`），
   因此通道内部的 15 个调用点无需改动，语义风险被限制在缓冲本身。

### 结果对比（ns/op）

| 项 | 优化前（中位） | 优化后 最好 / 中位 / 最差 | 判定 |
|---|---:|---|---|
| SPSC 缓冲 64 | 735 | 588 / 600 / 629 | **改善 14–20%** |
| SPSC 缓冲 1024 | 268 | 226 / 238 / 239 | **改善 11–16%** |
| MPMC 缓冲 1024 | 738 | 652 / 656 / 694 | **改善 6–12%** |
| MPMC 缓冲 64 | 4,892 | 5,598 / 5,618 / 5,898 | **退化 14–21%（未解决）** |
| ping-pong 无缓冲 | 64,937 | 49,400 / 50,555 / 58,757 | 改善 9–24% |
| ping-pong 缓冲 1 | 59,976 | 49,295 / 49,377 / 57,666 | 改善 4–18% |
| 空探测 `TryRecv` | 53.2 | 51.2 / 51.5 / 52.1 | 略好（未针对该路径优化） |
| SPSC 无缓冲 | 29,704 | 24,340 / 25,945 / 29,661 | 持平（该路径不用缓冲） |

### 结论（含未解决项）

1. **有缓冲路径确实受益**：SPSC 64/1024 分别改善 14–20% 与 11–16%，MPMC 1024 改善 6–12%，
   与「`deque` 的块边界维护比环形索引贵」的预期一致；ping-pong 也一致改善。
2. **MPMC 缓冲 64 退化 14–21%，原因未查明，已如实记录**。已排除「锁内取模」这一因素
   （去掉取模后仍然退化）。该场景容量小、阻塞与唤醒频繁，主要成本可能落在等待/唤醒路径上，
   而缓冲结构的差异被放大或淹没；也可能与模板内联后的代码布局有关。**需要单独剖析才能定位**，
   当前**不建议**仅凭这一点回退整个改动（3 项改善 + 1 项退化）。
3. **空探测（`TryRecv` 空路径 51–53 ns）未变动**：该路径不进缓冲，成本来自完整状态机。
   曾评估「原子标志 + 无锁快速路径」，最终**放弃**：Go 的 `hchan` 没有这种机制，
   且需要插桩十余处、漏一处即为偶发丢值，收益又只覆盖「操作必然失败」的场景。

### 测试

`ctest` **79/79 通过**（29.55 s），构建 **0 warning**。另在 `tests/concurrency/test_channel.cpp`
补了 4 个环形缓冲专项场景（这是替换 `deque` 后最易出错的地方）：

- 容量 2 反复回绕 200 轮，验证 FIFO 与索引回绕；
- 容量 1 的满/空交替 1,000 轮（最易暴露 head 前进与计数错误）；
- 容量 7（非 2 的幂，回绕不整齐）跨线程传 20,000 个值并校验总和；
- 20,000 步随机操作与 `std::deque` 参考模型对拍，输出序列必须逐项一致。

---

## 2026-10-15 Channel 与「线程 + 阻塞队列」对比

> **本节条件**：`build-bench`（`CMAKE_BUILD_TYPE=Release`，基准目标另加 `-O2`）；
> 宿主 AMD Ryzen 9 8945HX（12 线程）/ WSL2；每项跑 **3 轮**，下表给 **median**（区间见括号）；
> 数据量：SPSC 50,000 个 int；MPMC 4 生产 × 12,500（合计 50,000）；ping-pong 往返 10,000；空探测 100,000。
> 与本文其它章节**不可横向比较**。

基准程序：`benchmarks/channel_vs_queue_benchmark.cpp`（由 CMake 在 `GO2CPP_BUILD_BENCHMARKS` 下登记）。

对比三种实现（都是 FIFO）：

| 代号 | 实现 |
|---|---|
| **A** | `go2cpp::Channel<int>`（本节被考察对象） |
| **B** | `std::mutex` + **两个** `condition_variable`（`not_empty`/`not_full`）+ `deque`，`notify_one`（教科书式有界队列） |
| **C** | `std::mutex` + **一个** `condition_variable` + `notify_all`（常见粗糙写法，用于量化惊群代价） |

### 结果（ns/op，越低越好）

**① SPSC 吞吐（1 生产 + 1 消费）**

| 实现 | 容量 | median | min–max |
|---|---:|---:|---|
| A Channel | 0 | 29,704 | 29,668–30,136 |
| A Channel | 64 | 735 | 700–759 |
| A Channel | 1024 | 268 | 258–270 |
| B 双 cv 队列 | 1 | 54,724 | 54,037–59,905 |
| B 双 cv 队列 | 64 | 848 | 802–860 |
| B 双 cv 队列 | 1024 | **84** | 73–101 |
| C 单 cv 队列 | 64 | 764 | 730–770 |
| C 单 cv 队列 | 1024 | 79 | 76–81 |

**② MPMC 吞吐（4 生产 + 4 消费）**

| 实现 | 容量 | median | min–max |
|---|---:|---:|---|
| A Channel | 0 | 35,225 | 35,162–35,655 |
| A Channel | 64 | **4,892** | 4,537–4,972 |
| B 双 cv 队列 | 64 | **9,253** | 6,590–12,242 |
| A Channel | 1024 | 738 | 634–913 |
| B 双 cv 队列 | 1024 | 502 | 435–891 |
| C 单 cv 队列 | 1024 | 206 | 177–224 |

**③ ping-pong 往返延迟（无缓冲会合，双向）**

| 实现 | median | min–max |
|---|---:|---|
| A Channel 无缓冲(0) | 64,937 | 59,740–65,545 |
| A Channel 有缓冲(1) | 59,976 | 56,244–63,302 |
| B 双 cv 队列(1) | 52,682 | 52,170–56,279 |
| C 单 cv 队列(1) | 53,974 | 53,934–54,129 |

**④ 空容器探测成本（`TryRecv` / `TryPop`）**

| 实现 | median | min–max |
|---|---:|---|
| A `Channel::TryRecv()` | 53.2 | 53.1–53.7 |
| B `BoundedQueue::TryPop()` | 4.1 | 4.1–4.3 |
| C `NaiveQueue::TryPop()` | 4.1 | 4.1–4.1 |

### 结论

1. **Channel 的抽象确实有代价，且可量化**：空探测慢约 **13 倍**（53 ns vs 4 ns，因为要过完整状态机：加锁、查 context、构造结果结构体）；
   高吞吐有缓冲场景慢 **3–4 倍**（SPSC 1024：268 vs 84 ns）。
2. **但在「多生产者 + 小缓冲」下 Channel 反而快约 1.9 倍**（MPMC 64：4,892 vs 9,253 ns）：
   传统队列的 `not_full.notify_one()` 可能叫醒**另一个**生产者（而非刚阻塞的那个），产生「被唤醒→发现无空位→重新等待」的循环；
   Channel 每个等待节点自带 `ParkingCondition`，可**精确唤醒**目标等待者。这正是多出来的那部分结构换来的收益。
3. **无缓冲会合（rendezvous）的成本与实现基本无关**：三者都在 52–65 µs 量级（Channel 略慢约 20%），
   因为该路径由**内核唤醒/调度**主导，而不是由用户态代码路径主导。
4. **朴素单 cv + `notify_all` 在单生产者场景并不吃亏**（C 与 B 的 64/1024 档基本持平），
   说明「惊群」的代价主要体现在多等待者竞争时；本轮 MPMC 只跑了 64/1024 两档，未单独量化 C 的 MPMC 小缓冲表现。

### 可优化点（未实施）

- `Channel::TryRecv()` / `TrySend()` 的空容器快速路径：用一个原子标志（或 `m_generation` + 缓冲/队列长度的原子快照）
  在**不进锁**的情况下判断「必然无值」，可把 53 ns 降到个位数纳秒。当前实现每次都要进锁并构造结果结构体。
- 缓冲容量的选择对性能影响远大于实现差异（A 从 64 → 1024 提升约 2.7 倍），
  实际使用中**优先加大容量**比换实现更有效。

### 复现方式

```sh
cmake -S . -B build-bench -DGO2CPP_BUILD_BENCHMARKS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-bench --target go2cpp_channel_vs_queue_benchmark --parallel
./build-bench/go2cpp_channel_vs_queue_benchmark 3
```

---

## 2026-09-30 调度器与混合锁专项收尾

本节是本轮代码变更后的最新结果，优先级高于本文早期采样。测试使用
`build-release-current`，WSL2/Linux，Release 编译；宿主调度会造成明显抖动，
因此只报告重复运行区间，不把单次最好成绩当作保证。

### 调度器热路径

本轮的实际执行链路为：

1. 外部生产者先完成 G 的 owner、取消门、注册表 admission 和 queued 状态；
   已注册的 IO/定时器唤醒跳过 registry 分片锁。
2. 新任务进入 16 条 incoming 条带，生产者通过 TLS 固定条带；非空位图让
   worker 不必为明显为空的条带逐一加锁。
3. worker 先取当前 P 本地队列，再按亲和性预算处理 incoming，最后才进入全局
   admission、窃取和救援队列。当前 worker 唤醒本地 G 时直接回本地 P，避免
   再次走外部 admission。
4. Fiber 返回后只在仍有 runnable G 时通知其他 M；维护周期中的 M 扩容、空闲
   M 回收和 sysmon 观察不进入每次恢复的全局锁路径。

公平调度基准为 50,000 个 G、每个 G 4 次 yield、8 个生产线程、8 个 P。当前
Go2Cpp 的 `spawn_ms + elapsed_ms` 为 `84～103 ms`（5 次：84、92、101、103、
95 ms）；同一份本机 coost 对照为 `18～36 ms`。中位数约为 coost 的 `3.0x`，
尚未达到 1.25x 目标。关闭 `collect_metrics` 只改变几个百分点，说明主要成本
不是计时器，而是每个 G 的 `shared_ptr`/注册表生命周期、受保护 Fiber 栈
（Linux x86_64 为自有上下文后端）以及 G 状态转换和跨 M admission。

继续把这些状态改成无锁裸指针会破坏 shutdown、取消和跨线程最后一个引用的
所有权；把受保护栈替换为 coost 的快照栈也会失去 guard page，并且要求禁止栈
地址逃逸。当前没有在未证明安全前做这两类危险优化。

### 混合 Mutex

`go2cpp::sync::Mutex` 采用一个原子锁位加一条受 `m_mutex` 保护的 FIFO 等待队列。
Fiber 和普通线程共用队列；Fiber 等待节点在调用栈上，带 Context 的节点使用
线程本地小缓存。Unlock 始终在队列锁下完成线性化，并把锁位直接交给队头，避免
“先清锁、后来者抢到、旧 owner 再误清锁”的竞态。已有 waiter 时禁止新的
自旋者越过队头。

专项混合基准（2,000 Fibers、32 普通线程、206,400 次临界区）当前为
`8.5～8.8 s`；coost 为 `1.6～5.7 s`（一次宿主抖动离群），线程
`std::mutex` 基准为 `39～40 ms`。因此该场景仍不适合高争用共享锁，不能宣称
达到线程池或 coost 性能。原因是 Fiber 阻塞后必须经过 park、状态发布、跨 M
唤醒和安全栈恢复；一次 CAS 优化无法消除这条链路。

规范使用方式：

- 短临界区、低到中等争用时可直接使用 `sync::Mutex`，普通线程和 managed Fiber
  可以同时访问同一实例。
- 不要在持锁期间执行 socket、文件、定时器等待或主动 park；先复制所需状态，
  解锁后再做阻塞操作。
- 高争用计数器优先使用原子变量或按 P/任务类别分片；普通线程独占的热点使用
  `std::mutex`，不要为了统一接口把所有锁都换成混合锁。
- `TryLock` 在存在 FIFO waiter 时返回 false，这是公平性约束，不是异常。

### 栈后端结论

本机 coost HEAD `c1cc11b32d5208912675a98fe091686848e60bc3` 使用固定约 1 MiB
执行栈槽，挂起时把栈内容复制到可增长的 Buffer；Buffer 增长不等于执行栈自动
扩容，深栈仍可能越过固定槽。它没有 Go 式 `morestack`/`copystack` 的保护页、
完整 C++ RAII 展开和 ASan/TSan Fiber 标注。Boost 的 `segmented_stack` 又依赖
`-fsplit-stack`/libgcc，工具链、异常和 sanitizer 兼容性受限。因此本项目继续
使用受保护固定栈（Linux x86_64 为 Go2Cpp 自有上下文 ABI，其他平台为
Boost.Context），自动动态栈列为未完成能力；不能以 coost 的快照机制冒充已解决。

### 其他高负载结果

同一轮 `go2cpp_high_load_stress` 三次：计算 Fiber `1415～1522 ms`、线程池
`1372～1414 ms`；IO 多路等待 Fiber `646～786 ms`、线程 poll
`1785～1919 ms`；Channel Fiber `21～27 ms`、线程 `67～88 ms`。这些结果说明
IO、Channel 和低争用计算场景仍有优势，但不能推导混合锁和公平调度已经全面
超过 coost。

本文记录本轮调度器和混合锁优化的实际结果。所有数据来自同一台
WSL2/Linux 主机、同一份源码和同一条 `go2cpp_high_load_stress` 命令；
时间单位为毫秒。

## 本轮实现的优化

- 调度器在本地 P 队列成功取到 G 后不再获取调度器全局 admission 锁。
- G/M/P 运行计数和任务亲和性记录统一放到两条取任务路径之后，避免本地
  快路径遗漏 M/P 计数。
- `maybe_grow()` 从每次 Fiber 恢复改成有界维护周期；当活跃 M 少于 P 且
  仍有 runnable G 时立即补齐初始 worker，保留 P=1/并发 shutdown 的语义。
- Scheduler 恢复 Fiber 时首次绑定父级，后续同一 G 的恢复跳过重复父链元数据
  锁；发生 M/P 迁移时仍更新调试绑定。
- 混合锁在短临界区竞争时先进行最多 16 次 Fiber 协作让出，只有持续竞争才
  创建等待节点并进入 FIFO handoff。已发布等待者存在时禁止无序插入，避免
  新的 TryLock/Lock 越过 FIFO 队列。

这些改动没有把普通线程锁伪装成 Fiber 锁，也没有用异步强行切断 C++ 栈；
Fiber 等待仍通过 Scheduler park，普通线程仍通过条件变量等待。

## 高负载结果

测试参数固定为：调度器 50,000 个任务、每个任务 4 次 yield；计算负载
8,000 个任务、每个任务 100,000 次循环；混合锁为 2,000 个 Fiber 和
32 个普通线程；Channel 为 32,000 条消息；IO 为 4,096 个 reader、25
轮 readiness。代表性运行结果如下：

| 场景 | Fiber/Go2Cpp | 线程基准 | 比例 |
|---|---:|---:|---:|
| 计算任务 | 1454 ms | 1415 ms | 1.03x |
| 调度与 yield | 474 ms | 65 ms | 7.29x |
| 混合 Mutex | 103 ms | 37 ms | 2.78x |
| Channel | 58 ms | 80 ms | 0.73x（Fiber 更快） |
| IO 等待 | 1748 ms | 1812 ms | 0.96x（Fiber 更快） |

旧实现同一基准的调度约 6.2 秒、混合 Mutex 约 13.9 秒；本轮分别降到
约 0.47 秒和 0.10 秒。重复运行存在宿主调度抖动：调度通常为
0.45～0.57 秒，混合 Mutex 在稳定运行时约 0.10～0.20 秒。高负载测试
连续完成全部校验，没有发现重复执行、丢任务、锁失败或消息丢失。

调度基准中的线程版本调用 `std::this_thread::yield()`，它不保存用户栈，
所以不能把 7.29x 直接解释为 Fiber 上下文切换的纯成本。当前指标中
`fiber_resume_ns` 可用于测量真实 Fiber 恢复累计时间；本轮代表性运行是
约 0.79 秒/250,000 次恢复，即约 3.2 微秒/次，其中包含安全状态、errno、
调度边界和统计开销。

## 与 Coost 的可比性

本轮直接使用本机已有的 Coost checkout：
`/mnt/e/CodexWorkspace/Go2Cpp/coost`，构建时的 HEAD 为 `c1cc11b`。
该 checkout 在开始前已经是 dirty 状态，因此下面的数字代表这份本地快照，
不代表某个干净发布版；没有修改 Coost 源码。参见
[Coost 官方仓库](https://github.com/idealvin/coost)。

两个项目都在同一台 WSL2/Linux 主机上以 Release 构建。调度测试使用 50,000
个任务、每个任务 4 次让出、8 个生产线程；Coost 使用 `co::sleep(0)` 让任务
被定时器重新放回 runnable 队列，因而只比较“自动重新调度”这条路径，不能把
Coost 的 `co::yield()`（需要调用者显式再次 resume）当成同一个 API。混合锁
测试使用 2,000 个协程、32 个普通线程，共 206,400 次加解锁；IO 测试使用
4,096 个 socketpair reader、25 轮 readiness。

| 场景 | Coost | Go2Cpp（本轮） | 结果 |
|---|---:|---:|---|
| 自动调度与 yield | 13～52 ms | 436～497 ms | Go2Cpp 慢约 8～38 倍 |
| 混合 Fiber/线程 Mutex | 1,097～1,766 ms | 99～107 ms | Go2Cpp 快约 10～18 倍 |
| 多 fd IO 等待 | 486～569 ms | 1,666～2,098 ms | Go2Cpp 慢约 3.0～4.3 倍 |

调度差距主要来自实现模型：Coost 使用少量共享栈，切换时保存/恢复栈内容；
Go2Cpp 当前使用受保护固定栈（Linux x86_64 为自有上下文后端），并在安全性优先的路径中维护
G/M/P、Fiber 父链和任务状态。共享栈可以显著减少栈分配和切换成本，但必须
补齐 C++ 栈对象生命周期、栈地址失效和异常边界验证，不能直接替换成未验证
的 memcpy 方案。

混合锁结果已经反超 Coost。本轮将无 Context 的竞争等待节点改为调用栈上的
节点，队列只保存裸指针并保持 FIFO handoff；带 Context 的等待仍由缓存的
拥有节点保证取消期间的生命周期。这样去除了短等待的 shared_ptr 控制块和
分配开销，同时没有改变普通线程阻塞语义。

IO 仍是明确的性能缺口。Go2Cpp 的 DescriptorToken、fd generation 校验、
取消/超时竞争和 epoll interest 更新增加了锁与生命周期管理成本；Coost 的
实现使用更轻量的嵌入式等待节点。下一步可以在保持 generation 校验的前提下
增加 Fiber 私有 IO 等待节点和批量 interest 更新，但当前不宣称已经解决。

上述 Go2Cpp 调度对照临时将 Fiber 栈设为 32 KiB；生产默认值仍由
`SchedulerConfig` 控制，没有为了单个基准改变公开默认值。Coost 的调度和锁
数据来自三次连续运行，IO 数据也来自三次连续运行；宿主调度抖动会影响区间。

## 复现实验

```sh
cd /mnt/e/CodexWorkspace/NewGo2Cpp
cmake --build build-engineering-wsl --target go2cpp_high_load_stress -j2
timeout 90s ./build-engineering-wsl/go2cpp_high_load_stress
```

完整功能测试仍需单独运行 `go2cpp_tests`；性能测试不是内存安全或竞态
证明，ASan、UBSan、TSan 和 Valgrind 仍按 `docs/testing.md` 的独立命令执行。

## 2026-09-30 当前工作区复测

以下数字来自当前工作区最后一次 Release 构建，均在同一台 WSL2 主机上重复运行，
用于判断本轮改动是否引入回归，不代表所有机器的固定性能承诺。

| 场景 | Go2Cpp | 对照 | 结论 |
|---|---:|---:|---|
| 公平调度（50,000 个任务） | 90～114 ms | Coost 14～48 ms | 中位数约 4.0 倍，仍未达到 1.25 倍目标 |
| 高负载计算 | 1.45～1.50 s | 线程池 1.41～1.50 s | 约 1.0～1.06 倍 |
| 混合 Fiber/线程 Mutex | 约 2.0 s | 线程基准约 47 ms | 仍是明显缺口；不能宣称已解决 |
| IO 多路等待 | 869～909 ms | Coost 1,409～1,529 ms | 约为 Coost 的 0.59 倍，本场景已超过 Coost |

工程化热路径的独立测量为：8 个线程并发写入启用的 CallbackSink 约
2.67～4.71M 条记录/秒；8 个线程并发读取配置快照约 17.9～19.2M 次/秒。
这两个测试没有与 Coost 同 API 的直接对照，因此不能推导“达到 Coost 1.25 倍”。

> **时效说明**：以下记录反映当时状态，已被本节之后的「2026-09-30 Context 与
> 跨 M 唤醒专项」取代——自研上下文后端其后重新引入，并成为 Linux x86_64 的默认
> 后端。此处保留原文以反映演进过程，当前事实以该专项一节为准。

本轮再次评估了自研 Fiber Context。之前的自定义汇编上下文在嵌套 Fiber 父链恢复
场景出现栈上下文破坏并已撤回；生产实现仍是 Boost.Context 的受保护固定栈。
因此当前没有 Go 式 `morestack`/`copystack` 自动扩容，也没有把固定栈替换成未经
验证的共享栈。该限制是已知兼容性边界，不能按“自动扩栈已完成”交付。

随后针对 IO 取消竞态增加了 WaitNode 的回调和完成操作屏障：移除 Context 回调或
回收节点前，会等待正在执行的取消/完成回调退出；epoll pending readiness 在下一次
消费时用零超时原生 `poll` 再验证，避免 FD 已被读空后返回伪 ready。该修复后的高负载
样例为：计算 1,544 ms（线程 1,490 ms）、调度 123 ms（线程 88 ms）、混合 Mutex
8,931 ms（线程 39 ms）、Channel 22 ms（线程 72 ms）、IO 955 ms（线程 2,035 ms）。
这说明 IO 场景仍明显受益，但混合锁和调度提交开销仍未达到 Coost 的 1.25 倍目标。

## 2026-09-30 Context 与跨 M 唤醒专项

Linux x86_64 已增加 Go2Cpp 自有上下文后端：汇编只保存 callee-saved 寄存器、
RSP/RIP 和 caller transfer，C++ `FiberStack` 使用带 `PROT_NONE` guard page 的
mmap，并按栈大小使用 TLS/全局有界缓存。这样不再链接 Boost.Context；ASan/TSan
切换标记、父链校验、`m_resume_claim` 和跨 M 的 transfer 更新仍由 Fiber 外层
负责。非 Linux x86_64 回退 Boost.Context。

coost 的 `tb_context_make/jump` 已作为布局参考，但没有直接复制其共享栈、8 槽
固定调度器或 `_exit` 终止路径。coost 的 Buffer 不是自动动态栈；本后端同样不
提供 Go 式 `morestack`，栈容量仍需通过配置显式设置。

native 后端高负载三次采样：调度 `113～133 ms`，计算 `1327～1526 ms`，混合
Mutex `6680～6895 ms`，IO `616～665 ms`；同一轮线程基准分别约 `40～46 ms`、
`1283～1439 ms`、`39～42 ms`、`1614～1704 ms`。上下文替换没有改变调度语义，
但显著降低了栈映射重复分配；调度仍慢于线程和 coost，混合锁仍是主要瓶颈。

IO WaitNode、ParkingCondition 和 sync WaitNode 的跨 M wake/disarm/arm 门使用
`HybridGate`：先对 atomic flag 做 64 次 `_mm_pause`/yield，持续竞争才进入
`std::mutex` 慢路径；门内仍保护 Scheduler 指针、Task shared ownership、
callback_active 和 operation 生命周期。它没有修改 Fiber 的 `fcontext` 并发规则：
同一 Fiber 仍必须由 Task execution claim 串行恢复，wake 只发布状态和队列节点。

与前一版同负载约 8.5～8.8 s 的混合锁相比，本轮 native Context+HybridGate 为
约 6.7～6.9 s（该基准宿主抖动较大），线程 `std::mutex` 仍约 40 ms。HybridGate
只能降低唤醒门开销，不能把 Fiber park/wake、FIFO handoff 和跨 M 调度变成一次
原子操作；高争用共享锁仍应按本文件的分片/原子规约使用。

## 2026-10-01 FiberBin 与 MLocalTaskQueue 复核

FiberBin 只复用已完成 Fiber 的对象和 `Impl`，不会复用挂起栈或跨 M 共享对象；
Fiber 栈映射仍由已有 TLS/全局受保护栈缓存处理。默认每个 M 保留 32 个对象，
上限 4096，可通过 `SchedulerConfig`、INI 和动态配置绑定调整。Release 高负载功能
测试和 CTest 通过；ASan/UBSan 单测可通过，但重复压力运行仍可能触发既有
`test_readiness_timeout_race` 的时序失败，不能据此宣称 sanitizer 全部稳定通过。
该失败在临时禁用 FiberBin 后仍可复现，未证明由本轮缓存引入。

本轮尝试的 M 私有任务队列没有保留：在 50,000 个任务、每个任务 4 次 yield 的
测试中出现任务完成超时。该方案需要重新设计 runnable 计数、局部配额、P/global
批量转移、sysmon 脱离和 shutdown 排空的统一状态机，当前不能把未验证实现当成
性能收益。现有高负载基线仍以 P 本地队列、incoming 分片和全局窃取为准。

## 2026-10-01：混合 Mutex 与同步等待快路径

本轮修复了混合 Mutex 的三个实际问题。`m_fast_locked`/`m_has_waiters` 拆分为
一个 `m_state` 原子字节；等待者在队列锁内发布 `kWaiter` 后再入队，避免
Unlock 覆盖尚未入队的等待者标志而丢唤醒。带 Context 的 WaitNode 增加 generation，
Context 旧回调不能污染 thread-local 缓存复用的下一代节点；取消与 `Arm()` 交错时
先发布终态，避免 Fiber 永久挂起。

性能路径增加了两项约束。已注册 G 被外部 M 唤醒时，优先投递到 G 最近运行的 P，
避免每次 handoff 经过 incoming stripe；P 本地回队只有在没有运行 M 或队列积压时
通知其他 worker，减少无效 futex 唤醒。同步等待新增 `Scheduler::park_wait()`，
沿用 IO 的原子 Waiting→Runnable 交接和 shutdown 二次检查，跳过普通 Mutex 等待
每次获取 scheduler admission 锁的开销。Fiber 在没有 native waiter 时最多协作重试
64 次；出现普通线程 waiter 后最多再试 4 次即进入 FIFO 队列，保证普通线程不会
被无限越过。

当前 Release 高负载（2,000 Fiber、32 个普通线程、每个 Fiber 100 次、每个普通线程
200 次）连续样本为 15、27、29、35 ms；完整高负载两次混合 Mutex 为 26/50 ms，
对应线程 `std::mutex` 为 45/40 ms。同期完整样本中计算为 1,506/1,522 ms（线程
1,443/1,411 ms），调度为 121/124 ms（线程 73/45 ms），Channel 为 24/26 ms
（线程 67/78 ms），IO 为 662/748 ms（线程 poll 1,895/1,911 ms）。这些数字是
本机重复运行结果，不是跨机器的性能承诺；调度器和 Fiber 栈切换仍可能受宿主调度
抖动影响。

验证命令：

```sh
cd /mnt/e/CodexWorkspace/NewGo2Cpp
cmake --build build-native-context -j$(nproc)
ctest --test-dir build-native-context --output-on-failure
ctest --test-dir build-engineering-werror --output-on-failure
ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 ./build-native-asan/go2cpp_tests
valgrind --leak-check=full --show-leak-kinds=definite,indirect,possible \
  --error-exitcode=99 ./build-native-context/go2cpp_fiber_sync_demo
```

本轮 Debug/Release 14 项 CTest、Werror 3 项 CTest 和 ASan 单测均通过；Valgrind
ERROR SUMMARY 为 0，definite/indirect/possible leak 均为 0，仅有 288 bytes
still reachable 的进程级缓存。TSan 仍受 WSL `unexpected memory mapping` 环境错误
限制，不能宣称通过。

## 2026-10-02：性能复测与混合 Mutex 生命周期修复

本轮重新构建 Release，并使用 `taskset -c 0-7` 固定到同一组 CPU。高负载参数仍为
50,000 个调度任务、8,000 个计算任务、2,000 个 Fiber + 32 个普通线程、
32,000 条 Channel 消息和 4,096 个 socketpair reader。修复后的三轮代表性结果如下，
单位为毫秒：

| 场景 | Go2Cpp | 线程基准 | Go2Cpp/线程 |
|---|---:|---:|---:|
| 计算 | 1595 / 1614 / 1560 | 1616 / 1632 / 1555 | 中位数约 0.99x |
| 调度与 yield | 93 / 150 / 107 | 55 / 55 / 50 | 中位数约 1.95x |
| 混合 Mutex | 18 / 1784 / 1562 | 39 / 38 / 37 | 双峰，不能用单一稳定倍率表示 |
| Channel | 17 / 18 / 20 | 53 / 69 / 56 | 中位数约 0.32x |
| 多路 IO | 2896 / 2614 / 2725 | 2034 / 2016 / 2091 | 中位数约 1.34x |

同机 Coost 对照使用 `build-coost/compare_*.cpp`，并以 `taskset -c 0-7`
运行：调度 50,000×4 的中位数约 8 ms，IO 中位数约 1,686 ms，混合 Mutex
中位数约 1,721 ms。两边的计时边界、线程数和编译选项仍不完全相同：Coost
对照是本地 checkout 的 `-O2` 构建，Go2Cpp 为 `-O3 -DNDEBUG`；调度和
混合锁结果还包含不同的 scheduler 初始化/关闭路径。因此这些数据用于工程
定位，不是严格的同 ABI 基准。

复测期间发现并修复了一个确定的混合 Mutex UAF。原实现把栈上 `WaitNode*`
放进队列，`Unlock` 弹出后释放队列锁，再调用 `Wake()`；等待方可能已经
返回并销毁栈节点，或把拥有节点重置后放回线程本地缓存，导致随机 SIGSEGV、
永久等待和 20 ms/2 s 两种性能模式。现在队列持有 `shared_ptr<WaitNode>`，
`Unlock` 在锁外唤醒期间继续持有该引用，唤醒完成后才允许节点复用。

修复后 Release 高负载连续 5 轮全部完成，ASan/UBSan mutex-only 连续 5 轮无
报告，Werror CTest 3/3、Release CTest 14/14 通过。混合 Mutex 仍有明显长尾，
需要后续继续优化等待/调度路径；不能把偶发的 18 ms 样本当成稳定性能。

## 2026-10-08：混合 Mutex 快速等待路径

针对上面的长尾，本轮没有放宽正确性检查，而是收窄了无取消受管 Fiber 的热路径：

- `src/concurrency/sync.cpp` 中无 Context、受 Scheduler 管理的 Fiber 仍然使用
  `shared_ptr<WaitNode>` 和 `Scheduler::park_wait()`；通用 `WaitNode` 的
  `Arm/Disarm/Wake` 状态保护没有被删除，带 Context、普通线程、ConditionVariable
  和手动 Fiber 继续走完整保护路径。本轮没有把通用节点替换为未经验证的裸节点。
- 默认无状态 `SchedulerWaitBackend` 改为共享实例，避免每次短等待重复分配后端控制块；
  自定义 `WaitBackendFactory` 的插件语义不变。
- Fiber 竞争只保留有界的 256 次协作重试；重试失败才挂起到 FIFO 队列，避免旧版
  64 次重试与等待节点慢路径之间反复切换。该预算不是公开语义，后续可通过策略对象
  调整，但不能改为无界忙等。
- `Unlock` 仍在队列锁外唤醒，队列引用在唤醒完成前不会释放；快速节点的 `Wake`、
  `Disarm` 和缓存 `Reset` 仍共享同一 `HybridGate`，没有用性能优化破坏 UAF 防护。

原始动态调度配置（2～16 worker、8 P）下，`go2cpp_high_load_stress --mutex-only`
连续 30 轮得到 Fiber 混合锁耗时最小/中位数/P95/最大值为
`9/19/31/51 ms`，同一完整高负载中的线程池 `std::mutex` 基线约 `36～40 ms`；
30 轮均成功，无此前的 1～2 秒长尾。
本轮完整 CTest 为 `17/17` 通过，ASan/UBSan 的同步过滤测试通过，Valgrind
混合锁专项 `ERROR SUMMARY: 0`，definite/indirect/possible leak 均为 0，仍有
96 bytes 的进程级缓存可达块。Valgrind 下耗时约 715 ms，不用于性能比较。

验证命令：

```sh
cmake --build build-network -j2
ctest --test-dir build-network --output-on-failure
for i in $(seq 1 8); do
  timeout 45s env LD_LIBRARY_PATH=build-network \
    ./build-network/go2cpp_high_load_stress --mutex-only
done
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1 GO2CPP_TEST_FILTER=sync \
  ./build-network-asan/go2cpp_tests
valgrind --tool=memcheck --leak-check=full \
  --show-leak-kinds=definite,indirect,possible --error-exitcode=99 \
  ./build-network/go2cpp_high_load_stress --mutex-only
```

这组数据说明混合锁已经跨过线程基线 75% 的要求，但不是“无条件快于
`std::mutex`”：Fiber 首次批量竞争仍包含上下文切换和调度成本，长临界区或带取消
的等待会主动进入完整慢路径。调用方仍应把临界区保持短小，不在锁内执行阻塞 IO、日志
回调或用户插件代码。

## 2026-10-18：sync::Mutex 250 倍退化的根因与修复；HybridGate 删除

> **条件**：`build-bench`（Release + `-O3 -DNDEBUG`）、AMD Ryzen 9 8945HX / WSL2、
> 新增 `benchmarks/sync_mutex_contention_benchmark.cpp`、N 线程反复 lock/unlock
> 同一把 `sync::Mutex`、rounds/thread=20000、`taskset -c 0-7`、A/B 交替各 5 轮、
> 取中位数。**测量时 loadavg 1.37～2.34**（>1.5 的轮次已在原始数据中标注）。

### 根因：不是 `Unlock` 的 O(n) 队列遍历，而是「每个操作睡一次」

用线程局部探针（临时加、已删除）在 4 线程 × 20000 ops 下观测到：

| 计数（每线程） | 值 |
|---|---:|
| `lock` 调用 | 20000 |
| 进入等待队列 | 19793 |
| `FUTEX_WAIT` 次数 | **19793** |
| `FUTEX_WAKE`（`Unlock` 侧） | 19793 |

即**除开头几个操作外，每次加锁都真实睡在 futex 上**。配合 `strace -c` 的
`FUTEX_WAKE` 调用数可以看到：8 线程时如果所有线程都留在自旋路径，futex 调用为 0，
单次加解锁是 100～300 ns；一旦有人转入等待队列，`kWaiter` 常驻，后续竞争者
「见到 kWaiter 只自旋 4 次」→ 立即入队 → `WaitNative` 的自旋预算 128（≈1.1 µs）
覆盖不了一次完整交接 → 每次操作一次睡眠。睡眠/唤醒在本机空闲时约 0.4 µs，
在 loadavg 3～4 时实测被放大到 ~96 µs，这就是 25～30 µs/op 的来源。

`Unlock` 的队列遍历是 O(队列长度)，但队列长度只有 3（4 线程）或 7（8 线程），
**不是** 250 倍退化的主因。

### 修复（`src/concurrency/sync.cpp`）

1. 普通线程入队前的自旋预算 `kNativeSpinAttempts`：256 → **2048**（≈18 µs）。
   它必须覆盖「N 个线程轮流抢同一把锁」的整个窗口；128/256 会让 8 线程中有人
   256 次全失败而转入睡眠。
2. `WaitNode::WaitNative` 的自旋预算：128 → **2048**，并改为「连续无进展才睡」；
   另设绝对上限 262144（≈2.3 ms）保证**严格有界**（X-35 红线）。
3. 新增 `m_futex_waiters` 计数：只有确实有线程睡在 futex 上时才调用
   `FUTEX_WAKE`，纯自旋交接完全在用户态完成。
4. 标定数据（同一轮 A/B，8 线程）：256→~23 µs、1024→321 ns、
   **2048→225 ns**、4096→349 ns（多出的空转无收益）。最终取 2048。

### 修复前后（中位数，taskset 0-7，A/B 交替 × 5）

| 线程 | BEFORE | AFTER | 加速 |
|---:|---:|---:|---:|
| 2 | 25.62 ns | 25.46 ns | 1.0x |
| 4 | **15,954.74 ns** | **60.33 ns** | **264x** |
| 8 | **20,777.13 ns** | **166.45 ns** | **125x** |

4 线程达到任务要求的「≤1 µs/op」。8 线程有两个高负载轮次（loadavg 2.29）
回退到 5.3～5.7 µs，中位数仍为 166 ns；这不是稳定态，报告保留原始轮次。

### HybridGate：按用户决策删除（X-35）

- 删除 `include/go2cpp/core/hybrid_mutex.hpp` 与
  `include/go2cpp/concurrency/core/hybrid_mutex.hpp`；
- 3 个生产文件、12 个加锁点、3 个成员改为 `core::FutexMutex`
  （`include/go2cpp/concurrency/core/futex_mutex.hpp`）：
  `parking_condition.cpp` 3+1、`sync.cpp` 6+1、`io.cpp` 3+1；
- 新实现：状态字 `0/1/2`（空闲/已锁/有等待者）+ Linux futex。
  **无自旋、无 condition_variable**；`exchange(2)` 抢锁保证每次
  `unlock()` 看到 2 时必定 `FUTEX_WAKE`，不会丢唤醒链；保留跨线程解锁。
- 测试/示例/基准同步替换：`test_futex_mutex.cpp`、`example/concurrency/futex_mutex_demo.cpp`、
  `benchmarks/futex_mutex_contention_benchmark.cpp`。

同一口径对比（`taskset -c 0-7`、loadavg 2.83；旧实现为删除前的
`HybridGate` 头文件独立编译）：

| 场景 | 旧 HybridGate | 新 FutexMutex |
|---|---:|---:|
| 2 线程争用 | 16.90 ns/op | 12.01 ns/op |
| 4 线程争用 | 82.41 ns/op | 13.63 ns/op |
| 8 线程争用 | 169.09 ns/op | 17.10 ns/op |
| 2 ms 长临界区 + 2 等待者的进程 CPU | 1.355 ms | 0.044 ms |
| 2 ms 长临界区 + 8 等待者的进程 CPU | 0.398 ms | 0.139 ms |

长临界区一列证明旧实现的慢路径会在持有者不推进时烧 CPU；新实现等待者直接睡眠。

### 死锁与回归

- `2×100`、`2×20000`、`4×20000`、`8×20000` 各 5 轮，共 **20/20 全部完成**，
  无挂死；`GO2CPP_TEST_FILTER=sync` 连续 5 轮通过。
- **全量 ctest：113/113 通过**（`/UserData/CodexWorkSpace/Go2Cpp/build-quality`，
  负载 4.8 下运行，Release）。性能对比的 A/B 数据在另一份 checkout
  （`/mnt/e/CodexWorkspace/Go2Cpp`，同一基线提交）上测量；两棵树的
  `sync.cpp` 基线一致，改动逐行 diff 核对过。
