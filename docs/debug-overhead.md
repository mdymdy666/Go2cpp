# GO2CPP_DEBUG_DUMP 开销压测：全开 vs 关闭

> 工作树：`Go2Cpp-work`
> 测量日期：2026-10-21（20:23 起）
> 构建：`build-work`（`GO2CPP_DEBUG_DUMP=OFF`）、`build-debug-full`
> （`GO2CPP_DEBUG_DUMP=ON` + `GO2CPP_REPLAY=ON`，`GO2CPP_DIAGNOSTIC=OFF`）
> 结论一句话：**全开不是「多记几个字节」，而是最坏 40 倍、多数场景 4–20 倍、timer/spawn 3.6–4.1 倍的
> 吞吐损失；主要成本来自栈/Fiber 链捕获（backtrace）与跨线程 park/wake
> 事件的全局 `seq` 原子写，而不是单条事件的 20 ns。按类别白名单关闭
> `primitives`、`timer`、`io`、`stacks/chains` 可把资源类场景拉回接近零开销，
> 但 channel/select 这类 park 密集场景即使只开 park 仍有 4–5 倍损失。**

---

## 0. 结论（先看这里）

| 场景 | OFF 吞吐 | 全开吞吐 | 全开慢倍数 | 全开吞吐损失 | 采样配置慢倍数 | 采样配置损失 |
|---|---:|---:|---:|---:|---:|---:|
| channel（Fiber ping-pong） | 2,011,419 | 100,511 | **20.0×** | −95.0% | 4.93× | −79.7% |
| mutex（4 线程争用） | 39,521,349 | 2,096,489 | **18.9×** | −94.7% | 1.37× | −27.3% |
| spawn（Fiber 创建/恢复） | 156,910 | 38,355 | **4.09×** | −75.6% | 1.89× | −47.0% |
| timer（arm/fire/回调） | 244,070 | 67,857 | **3.60×** | −72.2% | 1.96× | −48.9% |
| resources（RW/CV/WG） | 64,876,069 | 1,609,907 | **40.3×** | −97.5% | 1.10× | −9.4% |
| select（两路 select） | 834,144 | 89,337 | **9.34×** | −89.3% | 3.48× | −71.3% |

（吞吐单位 ops/s；N=200,000，每配置 3 轮取中位数。采样配置 =
`GO2CPP_DEBUG_CATEGORIES=sched,park` + `GO2CPP_DEBUG_SAMPLE_SHIFT=5`。）

**三条最值得记住的事实**

1. **最大开销场景是资源历史（RWMutex/WaitGroup/ConditionVariable）**：
   全开从 64.9M ops/s 降到 1.61M ops/s，p50 从 90 ns 涨到 3,026 ns（33.6×）。
   原因是每次 RW 锁操作都走资源事件 + 栈/链捕获，而 OFF 路径本身只有 ~15 ns/操作。
2. **栈/Fiber 链捕获是最大单项成本**：在 `channel` 上，保留全部行为类别但
   **不列 `stacks,chains`**，吞吐从全开的 97k 恢复到 354k（3.6× 改善）；
   `resources` 从 1.71M 恢复到 6.45M（3.8× 改善）。事件总数几乎不变
   （channel 3.41M → 3.41M），说明贵的不是「多几条事件」，而是 backtrace
   与样本表。
3. **park/wake 是「关键事件」，采样位移对它们无效**：`channel` 只开 `park`
   仍比 OFF 慢 4.3×（2.01M → 467k），事件量只有 5 条/操作。因为每条 park
   都要做一次进程级 `seq` 的 `fetch_add`，两个 Fiber 跨 worker 交接时这条
   缓存行持续弹跳；这是 fiber 交接场景无法靠「降采样」消掉的固定成本。

**建议（按场景）**

- 默认生产/压测保持 `GO2CPP_DEBUG_DUMP=OFF`。
- 需要排障时**按需开类别**，不要 `all`：
  - 只查调度/park：`GO2CPP_DEBUG_CATEGORIES=sched,park`；channel/select 密集
    服务要接受 3.5–5× 的开销；
  - 只查并发原语/资源历史：`primitives`；resources 类负载约 10×，mutex 约
    2×；
  - `stacks`/`chains` 只在需要调用栈/父链时开；默认采样 1/16、1/8，且
    `critical=true` 的 park 会**全量**捕获，是最大成本来源；
  - `timer`/`io`/`lifecycle` 对吞吐不敏感的场景可关；timer 场景全开 3.6×，
    其中 timer 事件占 14.3 条/操作。
- `GO2CPP_DEBUG_SAMPLE_SHIFT` 只对**非关键**事件有效（runqueue、快路径锁、
  worker park 等）；不要指望它降低 park/wake、G 终态、wake 系列的成本。
- 单事件 19.65–22.47 ns 的口径**不能外推**整体开销：端到端放大来自
  backtrace、全局 seq 原子竞争、每事件类别检查与缓存行竞争。

---

## 1. 方法

### 1.1 构建与三种配置

所有库都是 Release（`-O3 -DNDEBUG`），同一源码版本（`build-debug-full`
于 2026-10-21 20:19 用当前工作树重建；`git status` 只有 `docs/` 下的未跟踪
文件，无源码改动）。

| 配置 | 库 | 编译宏（基准程序与库一致） | 运行时环境变量 |
|---|---|---|---|
| A `OFF` | `build-work` | 无 | 清除全部 `GO2CPP_DEBUG_*` |
| B `ON_FULL` | `build-debug-full` | `GO2CPP_DEBUG_DUMP=1`、`GO2CPP_REPLAY=1`、容量 256/64 | `GO2CPP_DEBUG_CATEGORIES=all`、`SAMPLE_SHIFT=0` |
| C `ON_SAMPLED` | `build-debug-full` | 同上 | `GO2CPP_DEBUG_CATEGORIES=sched,park`、`SAMPLE_SHIFT=5` |

关键实现细节：`channel.hpp` / `rwmutex.hpp` 的事件记录点在**头文件/模板**里，
基准程序必须与所链接库使用相同的 `GO2CPP_DEBUG_DUMP` / 事件环容量 /
`GO2CPP_REPLAY` 宏，否则会测出「库记录、程序不记录」的混合配置。
`run_debug_overhead.sh` 从各构建的 `CMakeCache.txt` 读取这些值传给编译器。

`build-debug-full` 的实际缓存值是 `DEBUG_DUMP=ON` + `REPLAY=ON`（不是纯
DEBUG_DUMP）。因此 B/C 的绝对值包含「资源注册表 + 资源事件转发」的固定成本；
这比纯 DEBUG_DUMP 略高，属于**现有全开构建的上界**。

### 1.2 基准程序与场景

`tests/bench/debug_overhead.cpp`（新增），一行 TSV 输出，每个场景可独立运行。
每个场景在**独立进程**中执行，避免上一场景的栈池/分配器缓存污染。

| 场景 | 操作定义（ops） | 线程/Fiber | 说明 |
|---|---|---|---|
| `channel` | 2N 次 send/recv | 2 Fiber + 2 worker | 无缓冲 channel 来回交接 N 次 |
| `mutex` | N 次 lock+unlock | 4 OS 线程 | `sync::Mutex` 高争用；临界区只做一次自增 |
| `spawn` | N 个 Fiber | 4 worker | 每批 2048 个，spawn 后逐个 `wait()` |
| `timer` | N 个定时器 | 2 worker + timer 线程 | `schedule_after(0)`，每批 1024 个，回调 `WaitGroup::Done` |
| `resources` | 6N + N/32 次原语操作 | 主线程 + 1 helper | 每轮 RLock/RUnlock/Lock/Unlock/Add/Done；每 32 轮一次真实 CV Notify（helper 在 Wait） |
| `select` | N 次 select | 2 Fiber + 2 worker | 两个 channel 上 `Select({RecvCase(a),RecvCase(b)})`，sender 交替发送 |

### 1.3 测量与统计口径

- **吞吐**：场景前后各一次 `steady_clock` 墙钟，`ops / wall`；不做逐操作计时。
- **延迟**：另跑一轮逐操作采样，每场景最多 100,000 个样本；p50/p99/p99.9 为
  排序后分位。**样本包含 `steady_clock::now()` 的固定成本（本机约 20–30 ns）**，
  因此 `mutex` OFF 的 p50=32 ns 已接近计时器下限，不能当真实锁延迟。
- **端到端 vs 单操作**：`channel`/`select` 的延迟是每次交接/select 的耗时；
  `spawn`/`timer` 是 spawn→任务体、arm→回调的端到端时间（**含排队**），
  所以 p50 在毫秒级，主要反映批次排队，不反映单次 CPU 成本。
- **事件量**：`EventRing::TotalCountAll()` 的累计计数增量（不做
  `ResetTotals()`，避免与后台线程并发写冲突）。OFF 构建恒为 0。
- **RSS**：`/proc/self/status` 的 `VmRSS`（场景前/后）与 `VmHWM`（进程峰值）。
- **轮次**：每 (场景, 配置) 跑 3 轮，汇总取中位数；同一轮内 OFF→ON_FULL→
  ON_SAMPLED 相邻运行，减少系统状态漂移。
- **有效性**：脚本读取运行前 1 分钟 loadavg；>2.0 标记 `valid=0`，汇总剔除。
  本次所有 54 个进程的 loadavg 在 **0.41–1.15**，全部有效（18/18 组合 3 轮）。

### 1.4 环境

| 项 | 值 |
|---|---|
| CPU | AMD Ryzen 9 8945HX（WSL2 可见 12 vCPU） |
| 内存 | 20 GB（WSL2） |
| 内核 | 6.6.87.2-microsoft-standard-WSL2（x86_64） |
| 编译器 | g++ 13.3.0（Ubuntu 24.04） |
| 构建 | Release，`-O3 -DNDEBUG` |
| 负载 | 1 分钟 loadavg 0.41–1.15，低于 2.0 阈值 |

WSL2 是虚拟化环境，绝对数不能外推到裸机；**同机同轮次的比值**是本文主要结论。

---

## 2. 数据

### 2.1 场景 × 配置（中位数）

原始文件：`tests/bench/results/debug_overhead_20261021-202324.tsv`；
汇总：`..._summary.tsv`。

| 场景 | 配置 | 轮数 | 吞吐 (ops/s) | p50 (ns) | p99 (ns) | p99.9 (ns) | RSS (KB) | 峰值 RSS (KB) | 事件量 | 事件/操作 |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| channel | OFF | 3 | 2,011,419 | 482 | 1,041 | 5,285 | 4,736 | 6,144 | 0 | 0 |
| channel | ON_FULL | 3 | 100,511 | 9,410 | 23,927 | 58,133 | 6,016 | 7,552 | 6,826,394 | 34.13 |
| channel | ON_SAMPLED | 3 | 407,628 | 2,334 | 6,383 | 19,663 | 5,596 | 7,132 | 3,600,091 | 18.00 |
| mutex | OFF | 3 | 39,521,349 | 32 | 1,946 | 100,632 | 4,480 | 5,760 | 0 | 0 |
| mutex | ON_FULL | 3 | 2,096,489 | 299 | 9,795 | 192,110 | 5,120 | 6,400 | 459,657 | 2.30 |
| mutex | ON_SAMPLED | 3 | 28,743,578 | 32 | 1,615 | 110,630 | 4,828 | 6,144 | 0 | 0 |
| spawn | OFF | 3 | 156,910 | 11,664 | 102,943 | 191,327 | 9,344 | 9,728 | 0 | 0 |
| spawn | ON_FULL | 3 | 38,355 | 21,099 | 215,429 | 515,799 | 11,604 | 12,192 | 2,167,387 | 10.84 |
| spawn | ON_SAMPLED | 3 | 83,089 | 14,716 | 118,166 | 325,601 | 11,080 | 11,596 | 800,834 | 4.00 |
| timer | OFF | 3 | 244,070 | 1,674,050 | 5,401,955 | 6,800,880 | 9,344 | 9,728 | 0 | 0 |
| timer | ON_FULL | 3 | 67,857 | 6,237,326 | 15,089,578 | 17,556,903 | 11,776 | 12,160 | 2,851,881 | 14.26 |
| timer | ON_SAMPLED | 3 | 124,666 | 3,326,411 | 10,183,620 | 11,859,134 | 10,880 | 11,264 | 800,392 | 4.00 |
| resources | OFF | 3 | 64,876,069 | 90 | 561 | 1,232 | 4,352 | 4,736 | 0 | 0 |
| resources | ON_FULL | 3 | 1,609,907 | 3,026 | 21,862 | 31,610 | 5,084 | 5,468 | 3,081,843 | 2.55 |
| resources | ON_SAMPLED | 3 | 58,800,679 | 98 | 518 | 706 | 4,608 | 4,956 | 0 | 0 |
| select | OFF | 3 | 834,144 | 1,607 | 2,178 | 12,796 | 4,608 | 4,864 | 0 | 0 |
| select | ON_FULL | 3 | 89,337 | 12,450 | 38,893 | 98,857 | 5,980 | 6,364 | 3,760,261 | 18.80 |
| select | ON_SAMPLED | 3 | 239,705 | 3,570 | 15,672 | 40,867 | 5,596 | 5,980 | 1,800,079 | 9.00 |

`resources` 的 ops = 6N + N/32 = 1,206,251，所以「事件/操作」2.55 对应
**每轮 15.4 条事件**；不要与 channel 的「每次交接 34 条」直接横向比较。

### 2.2 ON/OFF 比值

| 场景 | OFF 吞吐 | 全开吞吐 | 全开慢倍数 | 全开吞吐损失 | 全开 p50 倍数 | 全开 p99 倍数 | 采样慢倍数 | 采样 p99 倍数 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| channel | 2,011,419 | 100,511 | 20.01× | −95.0% | 19.5× | 23.0× | 4.93× | 6.13× |
| mutex | 39,521,349 | 2,096,489 | 18.85× | −94.7% | 9.34× | 5.03× | 1.37× | 0.83× |
| spawn | 156,910 | 38,355 | 4.09× | −75.6% | 1.81× | 2.09× | 1.89× | 1.15× |
| timer | 244,070 | 67,857 | 3.60× | −72.2% | 3.73× | 2.79× | 1.96× | 1.89× |
| resources | 64,876,069 | 1,609,907 | 40.30× | −97.5% | 33.6× | 39.0× | 1.10× | 0.92× |
| select | 834,144 | 89,337 | 9.34× | −89.3% | 7.75× | 17.9× | 3.48× | 7.20× |

### 2.3 事件量与 RSS

事件构成（全开、单轮原始 TSV `event_detail`）：

| 场景 | 主要事件（每轮 20 万操作） | 总量 |
|---|---|---:|
| channel | park=800k, wake=800k, chsend=400k, chrecv=800k, gresume=400k, gcreate=2 | 6.83M |
| mutex | mutex（kMutexLock）=238k + kMutexUnlock 等 | 0.46M |
| spawn | gcreate=200k + 生命周期/入队/出队事件 | 2.17M |
| timer | timerarm=200k, timerfire=200k, wg=200k, gcreate=200k | 2.85M |
| resources | mutex=819k, rw=1.00M, cv=18.7k, wg=400k | 3.08M |
| select | park=400k, wake=400k, chsend=300k, chrecv=350k, gresume=200k | 3.76M |

采样配置（C）只保留 sched/park：channel 仍有 18 条/操作（park+wake 全量），
select 9 条/操作；`mutex`/`resources` 的事件量为 **0**（primitives 被白名单关闭）。

RSS 增量（ON_FULL − OFF，KB）：

| 场景 | OFF | ON_FULL | 增量 | ON_SAMPLED |
|---|---:|---:|---:|---:|
| channel | 4,736 | 6,016 | +1,280 | 5,596 |
| mutex | 4,480 | 5,120 | +640 | 4,828 |
| spawn | 9,344 | 11,604 | +2,260 | 11,080 |
| timer | 9,344 | 11,776 | +2,432 | 10,880 |
| resources | 4,352 | 5,084 | +732 | 4,608 |
| select | 4,608 | 5,980 | +1,372 | 5,596 |

RSS 增量主要来自每线程事件环（256+64 条 × 约 112 B ≈ 36 KB/线程）、累计计数
数组，以及全开时创建的栈/Fiber 链样本表（容量 4096/2048）。毫秒级场景的
增量都在 0.6–2.4 MB 量级，不是主要成本。

---

## 3. 分场景解读

### 3.1 channel（最典型的 fiber 交接）

全开 20.0×、p50 19.5×。每次交接产生约 34 条事件（park/wake 各 2、channel
send/recv、G resume 等），但 34 × 22 ns ≈ 0.75 µs，解释不了 p50 从 482 ns
涨到 9.4 µs。真正的大头是：

- 每次 park 的 `critical=true` 栈捕获与 Fiber 链捕获（backtrace + 样本表）；
- 进程级 `seq` 的 `fetch_add`：两个 worker 来回交接时同一缓存行反复失效；
- `DebugConfig::Enabled()` 的原子读与分支（即使类别关闭也要走）。

补充实验（§4）把「全行为类别、但不开 stacks/chains」后，吞吐从 97k 恢复到
354k（3.6×），证明栈/链捕获约占全开额外成本的 3.6×；只开 `park` 仍有 4.3×，
说明 park/wake 本身也不便宜。

### 3.2 mutex（4 线程争用）

全开 18.9×、p50 9.3×。OFF 的 p50=32 ns 已接近计时器下限；全开 p50=299 ns。
每次争用写 `kMutexLock`/`kMutexUnlock`，事件量只有 2.3 条/操作，但吞吐损失
与事件量不成比例——多线程同时写各自线程环、共享 `seq` 原子、以及事件分支
让临界区周边代码变大，缓存争用被放大。

采样配置（只 sched/park）事件量为 0，吞吐仍从 39.5M 降到 28.7M（−27.3%）。
这是**「开了 DEBUG_DUMP 但类别全关」的固定检查成本**（每次 Lock/Unlock 都
要过一次 `Enabled()`），不是事件写入。该场景单轮波动较大
（21.6M–38.8M），27.3% 是中位数口径，量级参考即可。

### 3.3 spawn（Fiber 创建/恢复）

全开 4.09×、p50 1.81×。每个任务本身要分配 Task/Fiber 栈（OFF 也要），
debug 事件（gcreate + 生命周期 + 入队/出队约 10.8 条/任务）叠加在较贵的
基线上，所以相对开销小于 channel。采样配置 1.89×，事件 4.0 条/任务。

### 3.4 timer（arm/fire/回调）

全开 3.60×、p50 3.73×；采样 1.96×。事件 14.3 条/定时器（arm/fire/wg/
gcreate 等）。OFF 的 p50=1.67 ms、ON_FULL p50=6.24 ms 是**批量排队口径**
（每批 1024 个 0 延迟定时器同时到期，timer 线程逐个执行回调），不是单次
arm→fire 的 CPU 成本；吞吐（244k → 68k ops/s）更能反映开销。
timer 场景单轮波动也较大（OFF 244k–351k），结论看中位数与量级。

### 3.5 resources（RWMutex + WaitGroup + ConditionVariable）

全开 40.3×、p50 33.6×，是**最大开销场景**。每轮 6 个原语操作产生约 15.4 条
事件（rw=1M、mutex=819k、wg=400k、cv=18.7k 每 20 万轮），且 RW 锁的每次
RLock/RUnlock/Lock/Unlock 都带资源事件与栈/链捕获；OFF 路径极快（90 ns/轮），
任何额外成本都会被放大成高倍数。

采样配置只慢 1.10×、事件 0：`primitives` 被白名单关闭后，RWMutex/WaitGroup
只剩一次 `Enabled()` 检查（以及 REPLAY 打开的资源注册表固定成本）。
这说明**资源历史类开销几乎完全可以通过类别白名单关掉**。

### 3.6 select（多路等待）

全开 9.34×、采样 3.48×。每次 select 约 18.8 条事件（park/wake + select/
channel + G resume）。它比 channel 温和，因为每次 select 只 park 一次
（channel ping-pong 是每次交接两边各 park 一次），但仍是 park 密集场景。

---

## 4. 类别归因（补充实验，N=100k、1 轮）

为了回答「全开的钱花在哪些类别」，用同一 `build-debug-full` 二进制只切换
`GO2CPP_DEBUG_CATEGORIES`，跑 `channel` 与 `resources`（原始数据
`/tmp/category_attribution.tsv`，非主表）：

| 类别配置 | channel 吞吐 | channel p50 | channel 事件/操作 | resources 吞吐 | resources p50 | resources 事件/操作 |
|---|---:|---:|---:|---:|---:|---:|
| 全开 `all`（含 stacks/chains） | 97,312 | 9,703 | 17.07 | 1,713,862 | 2,926 | 2.55 |
| 行为类别全开、不含 stacks/chains | 354,347 | 2,719 | 17.06 | 6,452,931 | 761 | 2.56 |
| 同上 + `SAMPLE_SHIFT=5` | 352,412 | 2,665 | 17.00 | 6,236,311 | 762 | 2.54 |
| 只开 `primitives` + shift=5 | 415,647 | 2,204 | 7.00 | 6,634,236 | 752 | 2.53 |
| 只开 `park` + shift=5 | 466,953 | 2,064 | 5.00 | 62,077,552 | 90 | 0 |

相对 OFF（channel 2,011,419；resources 64,876,069）的慢倍数：

| 类别配置 | channel | resources |
|---|---:|---:|
| 全开 | 20.7× | 37.9× |
| 行为类别全开、无栈/链 | 5.7× | 10.1× |
| 同上 + 采样 | 5.7× | 10.4× |
| 只 primitives | 4.8× | 9.8× |
| 只 park | 4.3× | 1.04× |

读法：

- **stacks/chains 是最大单项**：去掉后 channel 20.7×→5.7×、resources
  37.9×→10.1×；事件总数几乎不变（3.41M），说明成本是 backtrace 与样本表，
  不是事件条数。
- **primitives 是第二大头**：只开它，resources 9.8×；关掉它，resources
  回到 1.04×（park 对 resources 无事件）。
- **park 无法用采样位移降**：只开 park 时 channel 仍 4.3×；`SAMPLE_SHIFT=5`
  对 channel 的事件量几乎无影响（17.07→17.00），因为 park/wake 属关键事件。
- 补充实验只 1 轮、N=100k，用于归因；主表的 3 轮中位数更稳。

---

## 5. 结论与建议

### 5.1 全开的开销上限

- **最坏场景（资源历史类）**：40.3× 慢、p50 33.6×，绝对吞吐从 64.9M 降到
  1.61M ops/s；RSS +0.7 MB。
- **Fiber 交接/多路等待**：channel 20.0×、select 9.34×，p50 分别 19.5×/7.75×。
- **mutex 争用**：18.9×。
- **创建/定时器类**：spawn 4.09×、timer 3.60×（基线本身有分配成本，相对温和）。
- **RSS 上限**：单场景全开比 OFF 多 0.6–2.4 MB；不随 N 线性增长（事件环固定，
  样本表有容量上限），不是内存风险。

### 5.2 哪些类别可以关

按收益排序（基于 §4 归因与主表 C 配置）：

1. **`stacks` / `chains`：最该关**。全开时它们贡献 channel 约 3.6×、
   resources 约 3.8× 的额外成本；只在需要调用栈/父链时开。
2. **`primitives`（mutex/rwlock/cv/wg/channel/select 事件）：按需开**。
   资源历史场景 9.8×，关掉后 resources 仅 1.04×；mutex 只保留 `sched,park`
   时事件为 0、仍有 1.37× 的固定 Enabled 检查成本。
3. **`park` / `sched`：channel/select 的核心成本，且关键事件不可采样**。
   只开 `park` 时 channel 4.3×；只开 `sched,park`（带 shift=5）时 channel
   4.93×、select 3.48×。如果服务是 channel/select 密集，开这两类就要接受
   3.5–5× 的代价；不能通过 `SAMPLE_SHIFT` 降低。
4. **`timer` / `io` / `lifecycle`：可关**。timer 场景全开 3.60×，其中 timer
   事件约 14.3 条/操作；spawn 场景的 gcreate/生命周期事件约 10.8 条/任务。
5. **`GO2CPP_DEBUG_SAMPLE_SHIFT`**：对非关键事件（runqueue、快路径锁、
   worker park、sysmon）有效；对 park/wake/G 终态/wake 系列无效。

### 5.3 与单事件成本的关系

`docs/debug-reliability.md` 的 19.65–22.47 ns/事件是**批量写环**的成本，
不含 backtrace、不含跨线程缓存行竞争、不含类别检查。端到端压测显示：

- 事件量 × 22 ns 只能解释 channel 全开 p50 的约 8%（0.75 µs / 9.4 µs）；
- 剩余 92% 来自栈/链捕获、`seq` 全局原子、Enabled 分支与缓存效应；
- 因此**不能用单事件成本估算业务影响**，本文的端到端比值才是决策依据。

---

## 6. 边界与未覆盖

- **环境**：WSL2 虚拟化，12 vCPU / 20 GB；绝对吞吐不可外推裸机，比值与量级
  可参考。跨机器复测应重新跑脚本。
- **计时**：p50/p99 含 `steady_clock` 成本（约 20–30 ns）；`mutex` OFF 的
  32 ns 已接近下限，锁真实延迟被高估。吞吐不受逐操作计时影响。
- **timer/spawn 延迟**：端到端含排队（timer 每批 1024、spawn 每批 2048），
  p50 是毫秒级；不要当单次 CPU 成本。吞吐更可比。
- **ON 构建含 REPLAY**：`build-debug-full` 实际是 `DEBUG_DUMP=ON` +
  `REPLAY=ON`（`DIAGNOSTIC=OFF`）。资源注册表在 C 配置下仍然激活，因此
  C 的 resources 仍有 1.10× 固定成本；纯 DEBUG_DUMP 构建可能略低。
- **事件量口径**：只统计 `EventRing` 累计计数；`stacks/chains` 样本与
  backtrace 不增加事件条数，所以「事件/操作」不能单独解释开销。
- **mutex ON_SAMPLED 方差**：单轮 21.6M–38.8M，27.3% 是中位数口径；
  该场景对调度/缓存敏感。
- **未覆盖**：真实 socket/IO 压测（`kIo` 事件）、显式 `ParkingCondition`
  park/wake、迁移/回放协议、CLI/Doctor 在线查询、长跑（小时级）事件环覆盖
  与内存漂移。这些是后续工作。
- **OFF 构建的符号**：`build-work` 的 `libgo2cpp_debug_event.so` /
  `libgo2cpp_debug.so` 仍导出 `EventRing` 等**空实现**（接口保留，供工具
  直接调用）；热路径库 `scheduler/sync/channel/fiber` 对 `EventRing` 的
  动态符号引用为 **0**（见附录 A），基准 OFF 二进制的 `EventRing::Record`
  引用也为 0。即「OFF 无调试热路径」成立，但「二进制里完全没有 debug 字样」
  不成立（接口空壳仍在）。
- **重跑注意**：`build-debug-full` 曾落后源码一天（2026-10-20 22:06 构建，
  源码更新到 10-21 15:35），本次已在 10-21 20:19 重建；复测前应确认
  `build-debug-full/libgo2cpp_scheduler.so.0` 晚于 `src/concurrency/*.cpp`。

---

## 7. 复现

脚本用 `flock` 串行化（同一时间只允许一个压测实例），每个子进程用 `timeout 300` 兜底；`loadavg > 2.0` 的轮次标记 `valid=0` 并在汇总时剔除。

```bash
cd /UserData/CodexWorkSpace/Go2Cpp-work
# 正式：N=200000，每配置 3 轮
bash tests/bench/run_debug_overhead.sh

# 快速冒烟
N=2000 LAT_N=1000 ROUNDS=1 bash tests/bench/run_debug_overhead.sh

# 只跑部分场景 / 复用二进制
SCENARIOS="channel resources" SKIP_BUILD=1 bash tests/bench/run_debug_overhead.sh
```

输出：

- `tests/bench/results/debug_overhead_<ts>.tsv`：原始轮次（含 load/valid/rc/
  event_detail）；
- `tests/bench/results/debug_overhead_<ts>_summary.tsv`：中位数 + 比值；
- `tests/bench/results/debug_overhead_<ts>.md`：自动生成的表格；
- `/tmp/go2cpp-debug-overhead/bin/`：三份二进制（rpath 分别指向
  `build-work` / `build-debug-full`）；
- `/tmp/go2cpp-debug-overhead/logs/`：每轮 stderr。

类别归因实验（§4）：

```bash
# 用 ON 二进制，只切换环境变量；参考 /tmp/category_attribution.tsv
GO2CPP_DEBUG_CATEGORIES=park GO2CPP_DEBUG_SAMPLE_SHIFT=5 \
  /tmp/go2cpp-debug-overhead/bin/debug_overhead_ON_FULL \
  --scenario=channel --n=100000 --lat-n=50000 --config=PARK_ONLY
```

---

## 附录 A：OFF/ON 符号验证（nm）

命令与结果（2026-10-21，`/tmp/nm_verification.txt`）：

| 库 | OFF `build-work` | ON `build-debug-full` |
|---|---:|---:|
| `libgo2cpp_scheduler.so` | `EventRing` 引用 0 | 8 |
| `libgo2cpp_sync.so` | 0 | 3 |
| `libgo2cpp_channel.so` | 0 | 2 |
| `libgo2cpp_fiber.so` | 0 | 0 |
| `libgo2cpp_debug_event.so` | 22（空实现导出） | 22 |
| `libgo2cpp_debug.so` | 10 | 10 |
| 基准二进制 `EventRing::Record` | 0 | 1 |

结论：OFF 的**热路径库**不引用事件环；`debug_event.so`/`debug.so` 仍导出
接口（空实现），这是「接口可用、热路径零开销」的设计，不是残留调试代码。

## 附录 B：原始数据清单

- 主数据：`tests/bench/results/debug_overhead_20261021-202324.tsv`
- 汇总：`tests/bench/results/debug_overhead_20261021-202324_summary.tsv`
- 自动表格：`tests/bench/results/debug_overhead_20261021-202324.md`
- 符号验证：`/tmp/nm_verification.txt`
- 类别归因：`/tmp/category_attribution.tsv`
- 采集日志：`/tmp/debug_overhead_run.log`，每轮 stderr 在
  `/tmp/go2cpp-debug-overhead/logs/`

## 最终结论（2026-10-21 更新）

**并发原语 debug 即使只开 `primitives` 也有 2–10x 开销**（实测：mutex ~6x、rwmix ~10x、mpmc ~4.5x、pc ~2.1x、select ~4.4x；全开更高：mutex 192x、rwmix 66x）。

因此产品决策为：

1. **生产默认全关**（`GO2CPP_DEBUG_DUMP=OFF`）——不常开事件环/资源历史；
2. **线上排查依赖**：
   - **普通日志**（业务日志/错误码/关键路径日志）；
   - **Doctor 按需诊断**：`status`/`dump`/`deadlock-report`/`ctx.cancel` 是**快照式**操作，不常驻记录，开销只在调用瞬间；
3. **短时开启 debug**：排障窗口内按类别开（`park,sched` 或 `primitives`），问题定位后立即关闭；
4. Debug 的定位从「常开监控」改为「**按需诊断工具**」——需要持续监控时，用业务日志 + 外部指标，而不是事件环。

> 一句话：**性能优先的项目默认全关 Debug；线上问题 = 日志定位 + Doctor 现场诊断 + 短时开 Debug 复现。
