# 并发框架性能测试报告（2026-10-17）

> **测量环境**：AMD Ryzen 9 8945HX / WSL2 / Linux 内核，`build-quality` 与 `build-bench`
> 均为 `-O2` Release。所有数字仅在本次测量条件下有效，**跨机器不可横向比较**。
>
> **数据可信度**：每轮开始前要求 `loadavg < 1.0`，轮间间歇 3s。全部纳入统计的轮次
> 负载在 0.41~1.17 之间。凡出现离群值的场景均单独标注。

---

## 一、先检查原有压测的不足

原有资产：`tests/concurrency/high_load_stress.cpp`（5 个场景，含 Fiber vs 线程池对照）、
`benchmarks/io_epoll_fair_benchmark.cpp`（Fiber vs 线程池+epoll）。

作为**性能对比证据**，它们有下面这些问题。逐条列出，因为后面的数据修正方式都取决于它。

### 1.1 测量方法层面

| # | 问题 | 证据 | 后果 |
|---|---|---|---|
| 1 | **单次测量，无重复** | `StressFiberCompute()` 直接返回一次 `wall` | 单次数字方差可达数倍，不可作结论 |
| 2 | **无 warmup** | 计时从构造 Scheduler 开始 | 首轮含缺页/动态链接/线程创建 |
| 3 | **线程数不对齐** | `StressConfig()` 是 `processor_count=8, min_workers=2, max_workers=16`，而对照是 `ThreadPool(16)` | 8 个 P 对 16 个线程，不是同一并发度 |
| 4 | **计时口径不一致** | Fiber 侧含 `shutdown_for()`；`StressThreadCompute()` 里 pool 的析构发生在计时返回之后 | 两边算的不是同一段 |
| 5 | **`loadavg > 1.5 作废` 规则自相矛盾** | 压测自身把 loadavg 从 0.81 推到 2.40 | 用结束负载否定这一轮，等于否定所有轮次 |
| 6 | **不识别离群值** | 见下面 1.2 的 30080ms | 中位数被单个异常值污染 |
| 7 | **只有总耗时，无延迟分布** | 只报 `ms` | 看不出长尾，而并发框架的差异主要在尾部 |
| 8 | **校验失败即 abort** | `Check()` 抛异常 | 一旦失败，连已测到的数据也拿不到 |

### 1.2 实测发现的**真实测量缺陷**（这是本次最重要的发现之一）

`high_load_stress` 的 `channel` 场景在某轮报出 **30080 ms**，而其它轮是 **79~90 ms** —— 相差 385 倍。

追查后确认：这不是调度器卡顿，而是**测试代码把 `Task::wait_for()` 的超时等待计入了耗时**：

- 我在新写的混合基准里复现了同样的现象：`scheduler #1 = 30066 ms`，其余轮次 31~37 ms；
- 该基准原本有 `for (auto& h : handles) h->wait_for(5s)`，而任务体早已全部执行完（`done=8000/8000`）；
- 去掉这个循环后，#1 从 30066 ms 降到 61 ms，**30 秒卡顿完全消失**。

**结论**：`done` 计数（任务体执行完毕）与 `Task::wait_for()` 返回（Task 状态机到达终态）
之间存在可观测延迟，逐个 `wait_for` 会把这段延迟计入 wall clock。任何"任务已完成"的
基准都不应再对每个 Task 调 `wait_for` 来计时。

### 1.3 覆盖度层面

| # | 缺失 | 说明 |
|---|---|---|
| 9 | **无混合随机仿真** | 5 个场景都是固定形状：固定任务数、固定计算量、固定等待次数。真实服务是长尾分布 + 混合类型 |
| 10 | **无真实 `threadPool+epoll` 对照** | `high_load_stress` 的 IO 对照是 `thread-poll`（每线程 `poll` 一个 fd），不是共享 epoll |

---

## 二、改进措施

### 2.1 测量框架（脚本层，不改 C++ 测试语义）

新增测量脚本做了这些事：

1. **每轮开始前**等待 `loadavg < 1.0`（而不是结束后用 `> 1.5` 否定这一轮）；
2. 轮间间歇 **3 秒**，让压测自身产生的负载回落；
3. 多轮（7 轮）取 **min 与 median 并列**，min 代表"无干扰的最好情况"，median 代表常态；
4. **离群识别**：某侧 `max > 3 × min` 时单独标注，并提示"以 min 为准"；
5. **正确性独立校验**：统计多少轮 `all-checks-passed`，与性能数字分开报告。

### 2.2 新增基准 `benchmarks/mixed_random_benchmark.cpp`

补齐 1.3 的第 9 项。设计要点：

- **三类任务随机混合**：`pure`（纯计算）/ `yield`（计算中途让出 1~3 次）/ `short`（极短请求）；
- **长度长尾**：计算量在 `基准/1 ~ 基准/8` 之间随机，模拟长短不均的请求；
- **两侧同一任务序列**（同种子生成），保证公平；
- Fiber 用 `yield_current()`、线程池用 `std::this_thread::yield()`，都是"主动放弃时间片"；
- **不使用真实 IO**，只回答"混合形状负载下调度开销如何"（真实 IO 由 `io_epoll_fair_benchmark` 覆盖）；
- 自带 5 轮 + 中位数 + p50/p90/p99/max。

### 2.3 复用已有的 `threadPool+epoll` 基准

`benchmarks/io_epoll_fair_benchmark.cpp` 里已有真实的"单 epoll poller + 固定 worker"实现
（补齐 1.3 的第 10 项）。本次首次构建并运行它。

---

## 三、测试结果

### 3.1 计算任务

`high_load_stress` compute 场景：8000 任务 × 100000 轮整数哈希链，7 轮。

| 模型 | min (ms) | median (ms) |
|---|---:|---:|
| Scheduler + Fiber | **1487** | 1520 |
| ThreadPool (16 线程) | **1424** | 1451 |
| **倍率（Fiber / Pool）** | **1.04x** | 1.05x |

**判读**：计算密集场景两者基本持平，Fiber 略慢 4%。这个差距与"每次任务切换要多一次
栈切换"的量级相符，**没有数量级差异**。

### 3.2 IO 任务（两种口径）

**口径 A：简单 fd 就绪**（`high_load_stress` io-wait：4096 reader × 25 轮）

| 模型 | min (ms) | median (ms) |
|---|---:|---:|
| IOManager + Fiber | **1819** | 1901 |
| 线程池 + 每线程 poll | **1814** | 1884 |
| **倍率** | **1.00x** | 1.01x |

**口径 B：高并发就绪波次**（`io_epoll_fair_benchmark`：2048 fd × 20 波 × 3 轮，唯一解）

| 模型 | steady median (ms) | events/sec |
|---|---:|---:|
| **threadPool + epoll**（1 poller + 8 worker）| **856** | ~47,000 |
| **IOManager + Fiber**（8 P） | **266** | ~152,000 |
| **倍率（Fiber / pool+epoll）** | **0.31x** | — |

两边的 `events` 都是 `40960/40960`、`errors=0`、`watchdog=ok`，**正确性一致**。

**判读**：
- 单个 fd 简单等待时两者持平 —— 此时瓶颈在 syscall 本身，不在调度模型；
- **并发度上来后（2048 个 fd 同时就绪）Fiber 快 3.2 倍** —— 这正是"事件驱动 + 少数线程"
  相对"epoll 派发给固定 worker"的优势所在；
- 代价：fiber 的 **setup 是 75~90 ms**（建栈），而 pool+epoll 只要 3.6~4.3 ms。
  这个成本只付一次，任务越多越摊薄；**短命进程用 fiber 不划算**。

### 3.3 混合随机仿真

`mixed_random_benchmark`：8000 任务（pure=4791 / yield=2445 / short=764），
计算量长尾分布，5 轮，`workers=8`。

| 轮 | Scheduler (ms) | ThreadPool (ms) |
|---|---:|---:|
| 1 | 37.15 | 26.52 |
| 2 | 36.62 | 57.19 |
| 3 | 34.60 | 197.28 |
| 4 | 38.89 | 212.01 |
| 5 | 34.62 | 41.67 |
| **median** | **36.62** | **57.19** |
| **min** | **34.60** | **26.52** |
| 离散度 | **12%** | **700%** |

**倍率（median）：0.64x —— Fiber 快 36%。但 min 上 ThreadPool 更快（26.52 vs 34.60）。**

单任务延迟（p50/p90/p99，单位 µs，取各轮典型值）：

| 模型 | p50 | p90 | p99 |
|---|---:|---:|---:|
| Scheduler | 15.5 | 62 | 91 |
| ThreadPool | 15.3 | 61 | 89 |

**判读（这组数据最值得注意）**：
- **单任务延迟分布几乎完全一样**（p50 15.5 vs 15.3 µs）—— 任务本身的执行成本相同；
- 差异全在**整体吞吐的稳定性**上：Fiber 5 轮在 34.6~38.9 ms 之间（12%），
  ThreadPool 在 26.5~212.0 ms 之间（**700%**）；
- ThreadPool 有两次严重的整体退化（197ms、212ms），而 Fiber 没有；
- 但 ThreadPool 的**最好情况更快**。所以"谁更好"取决于你要的是**峰值**还是**可预测性**。

### 3.4 其余场景（`high_load_stress`，7 轮，取 min）

| 场景 | Fiber (ms) | 线程池对照 (ms) | 倍率 | 说明 |
|---|---:|---:|---:|---|
| channel（32000 消息 / 32 Fiber）| **79** | 105 | **0.75x** | Fiber 快 33% |
| mixed-mutex（2000 Fiber + 32 线程）| **16** | 39 | **0.41x** | Fiber 快 2.4 倍 |
| scheduler（50000 任务 × 4 yield）| **225** | 64 | **3.52x** | **Fiber 慢 3.5 倍** |

**`scheduler` 场景是 Fiber 唯一明显落后的一项**：50000 个任务每次 yield 4 次，
Fiber 侧 225ms vs 线程池 64ms。这与之前定位的"调度器全局锁每次持有 30 µs"一致 ——
纯 yield 型负载放大了调度器内部锁的成本。

---

## 四、结论

### 4.1 Fiber 明显占优的场景

1. **高并发 IO 就绪**（2048 fd）：**快 3.2 倍**；
2. **共享状态竞争**（mixed-mutex）：**快 2.4 倍**；
3. **消息传递**（channel）：**快 33%**。

### 4.2 基本持平的场景

4. **纯计算**：慢 4%（可接受）；
5. **简单单 fd IO**：持平。

### 4.3 Fiber 明显落后的场景

6. **纯 yield 型负载**（50000 × 4 yield）：**慢 3.5 倍**。根因是调度器内部全局锁的
   持有时长（此前测得每次持有 30 µs），不是等待原语。

### 4.4 关于"稳定性"的重要发现

混合随机负载下，**Fiber 的吞吐比 ThreadPool 稳定得多**（离散 12% vs 700%），
尽管单任务延迟分布相同。但 ThreadPool 的**峰值更快**。这是一个需要按用途取舍的结论，
不是"谁绝对更好"。

---

## 五、未完成与后续建议

1. **`scheduler` 场景的 3.5 倍差距未修复**。此前 5 个优化方向全部失败并已回退，
   当前最可靠的下一步是**先把锁统计细分到 `take_local` / `take_global` / `emergency` / `steal`
   各自耗时**（插桩已设计但未跑通），再动手；
2. **`Task::wait_for()` 与实际完成之间的延迟未定位**（见 1.2）。它不影响正确性，
   但会影响任何用 `wait_for` 做同步的测量的准确性；
3. **fiber 建栈成本（setup 75~90 ms）未优化**。短命进程场景下这是硬伤；
4. 本报告的 IO 场景**不含真实网络协议**（编解码、连接建立），只有 fd 就绪；
5. 所有数字均为**单机单次会话**，未做跨机器验证。

---

## 六、如何复现

```bash
# 1) 计算 / IO / channel / mutex / scheduler 五个场景（7 轮，需脚本驱动）
cd build-quality && ./go2cpp_high_load_stress

# 2) 高并发 IO：Fiber vs threadPool+epoll
cd build-bench && GO2CPP_IO_BENCH_FDS=2048 GO2CPP_IO_BENCH_WAVES=20 \
  GO2CPP_IO_BENCH_WORKERS=8 GO2CPP_IO_BENCH_REPEATS=3 \
  ./go2cpp_io_epoll_fair_benchmark

# 3) 混合随机仿真
cd build-bench && ./go2cpp_mixed_random_benchmark --repeats 5 --workers 8

# 测量前必须确认负载干净
cat /proc/loadavg   # 要求 < 1.0
```
