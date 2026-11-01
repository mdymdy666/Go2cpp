# startup-cost：池化资源的启动开销

> 状态：现行（2026-11-01）。
> 基准程序：`tools/startup_cost/`；生成报告：`.build-startup-cost/startup_cost --out /tmp/startup_cost.md`。
> 本文是模块入口：被测池清单、测量方法论、当前实测、结论与维护方式。

## 1. 定位

运行时里有一批**池化资源**：任务块、Fiber 栈、等待节点、缓存节点、
LSM 块、媒体抖动缓冲、流表、进程池。它们平时被当作「性能优化」讨论，
但每一个都要在**启动期或第一次使用时**付一笔时间与内存。这笔钱到底多少、
花在哪个池上、是否值得，之前没有一份能复核的实测。

本模块回答三个问题：

1. **每个池初始化要多久、占多少内存**（含槽数与单槽大小，便于换算）；
2. **全部池一起初始化要多久、占多少内存**，其中多少是池、多少是运行时本身；
3. **首次使用比稳态贵多少**（惰性池的一次性成本落在第一个请求上）。

模块**不做**的事：不改任何池的实现、不做优化决策、不替代压测。
它只产出证据；是否改代码由后续任务决定。

## 2. 被测池清单

| id | 池 | 时机 | 槽结构 | 实现位置 |
|---|---|---|---|---|
| `scheduler_core` | 调度器核心（**对照，非池**） | 首次使用 | 4 个 P/M、64 片 runnable 计数（4 KiB）、全局队列 | `src/concurrency/scheduler.cpp` |
| `task_block_pool` | Task 共享块池 | 首次 spawn | per-thread 96 槽（768 B TLS）+ 进程 free-list 48 + 64 块/chunk（44 KiB） | `src/concurrency/scheduler.cpp:667` |
| `fiber_bin` | FiberBin + 栈 arena | 首次 Fiber 创建/恢复 | TLS 128 槽（1 KiB）+ arena 首建 8 槽 × 8 MiB reserve（仅 VA） | `src/concurrency/fiber.cpp:276` |
| `wait_node_pool` | sync WaitNode 池 | 首次竞争 | TLS vector，上限 32 个 `shared_ptr<WaitNode>` | `src/concurrency/sync.cpp:813` |
| `channel_wait_node` | channel WaitNode 池 | 首次阻塞收发 | TLS vector，上限 128 个裸指针节点 | `include/go2cpp/concurrency/channel.hpp:949` |
| `cache_node_pool` | LRU-K 时间轮 + 节点池 | 构造即分配 | 256 桶时间轮 + 512 槽锦标赛树 + NodePool 64 节点/块 | `include/go2cpp/cache/lru_k.hpp` |
| `lsm_arena` | LSM Arena | 首次 `Allocate` | 按需 64 KiB 块；构造零分配 | `src/storage/arena.cpp:23` |
| `jitter_buffer` | JitterBuffer | 构造即分配 | 512 槽 × 1536 B 负载 + 512 × 24 B `SlotMeta` = 798,720 B | `src/media/jitter_buffer.cpp:50` |
| `stream_table` | 流表 | 构造即分配 | 空表零分配；每流一条 `Stream` | `src/app/stream_table.cpp:108` |
| `sfu_router` | SFU 房间表 | 构造即分配 | 空路由零分配；按房间/peer 增长 | `src/media/sfu.cpp:226` |
| `process_pool` | 进程池 | 构造即分配 | 构造零 fork；`Start()` 才创建 worker | `src/os/pool.cpp:39` |

清单是**代码里找出来的全量**，不是抽样：`grep -rn 'Pool|Arena|allocator' src include`
的命中项都被覆盖，唯一未单独列出的是 `src/concurrency/context.cpp` 的
`BlockPoolAllocator`（首次 `Context` 创建时触发，被 `task_block_pool` 的
spawn 路径覆盖）。

## 3. 怎么跑

```bash
cd <repo>
tools/startup_cost/build.sh                      # 链接 .build-bento-wsl 的 .so
.build-startup-cost/startup_cost                 # 全量，写 /tmp/startup_cost.md
.build-startup-cost/startup_cost --quick         # 少量样本，约 1 s（调试用）
.build-startup-cost/startup_cost --list          # 列出全部池 id
```

单独调一个池（不跑父模式）：

```bash
.build-startup-cost/startup_cost --child jitter_buffer --mode mem
.build-startup-cost/startup_cost --child cache_node_pool --mode time --repeat 10000
STARTUP_COST_TRACE=1 .build-startup-cost/startup_cost --child __all__   # 场景 C 分段
```

构建参数：

| 变量 | 默认 | 说明 |
|---|---|---|
| `STARTUP_COST_BUILD_DIR` | `.build-bento-wsl` | 依赖库所在目录 |
| `STARTUP_COST_OUT_DIR` | `.build-startup-cost` | 产物目录 |
| `CXX` | `g++` | 编译器 |

`build.sh` **刻意不改顶层 CMakeLists**：基准只是树内的一次性测量程序，
链接已构建好的 `.so` 即可。它额外加 `-Isrc/storage` 是因为 LSM `Arena`
目前是私有实现头（`src/storage/arena.hpp`），不为了让基准能测而去改核心的
头文件布局。

## 4. 测量方法论

### 4.1 一次进程只测一个池

池要么是进程级单例（`TaskBlockPool::Instance()`），要么是线程本地
（`FiberBin`、`WaitNode` 缓存）：同一进程内第二次就不会再付初始化费用。
想测「首次」只能一次一个进程，父进程 `fork + exec` 自身并发起。

代价是每次测量都含 exec 与动态链接。这部分由**场景 A** 单独量出：

- `startup_cost_min`：只有 `main` 与 `return 0`，不链接项目任何库；
- `startup_cost --child __none__`：完整二进制（链全部 `.so`）但不触发任何池。

两者之差 = 动态链接与静态初始化的开销（本机约 2 ms / 4.7 MiB），
它不属于池，报告里分开列。

### 4.2 时间：rdtsc + 校准，失败退回墙钟

池初始化的量级从 **27 ns**（Arena 稳态分配）到 **数百 µs**（Scheduler）不等。
`clock_gettime` 在 WSL2 上一次约 20~25 ns，对 ns 级样本是可观噪声，所以用
`rdtsc`（一次约 7 ns）。启动时用 `CLOCK_MONOTONIC` 做三轮 5 ms 校准：

- 三轮离散度 ≤ 2% → 用 TSC；
- 否则退回 `clock_gettime`（报告里会写「校准失败，退回墙钟」）。

父进程校准一次，把 `ns/tick` 通过环境变量传给子进程，避免 51 次 × 5 个惰性池
重复校准（那会多花近 4 秒）。

**例外：场景 C 的分段统一用 `CLOCK_MONOTONIC`。** 分段跨毫秒、线程与协程
可能被调度到别的 CPU 核，而 TSC 在虚拟化环境里跨核不一定同步 —— 毫秒级
测量一旦读到核间偏移，就会出现「19 ms 的唤醒」这类假数（实测踩到过）。
20 ns 的读开销对毫秒级分段可以忽略。`STARTUP_COST_WALL_CLOCK=1` 可以把
全局计时器强制切回墙钟，用于交叉验证。

### 4.3 内存：VmRSS + mallinfo2 双读

| 指标 | 来源 | 精度 | 用途 |
|---|---|---|---|
| `ΔVmRSS` | `/proc/self/status` 的 `VmRSS` | 4 KiB 页 | 真实占用的物理内存 |
| `ΔVmSize` | `VmSize` | 4 KiB 页 | 栈 arena 的 VA 预留（大头都在这里） |
| `Δheap` | `mallinfo2().uordblks + hblkhd` | 字节 | 池向分配器要了多少 |

三者缺一不可：

- 只看 RSS 会漏掉**未触碰的页**：`jitter_buffer` 分配 798,720 B，但 `new[]`
  不写内存，RSS 只涨一页；栈 arena 的 64 MiB reserve 更是完全不计入 RSS；
- 只看 heap 会漏掉 **mmap**：`Fiber` 的栈走 `mmap`，不进 glibc 堆；
- 只看 VmSize 会把 VA 当成内存用掉。

因此报告同时给三列，并在结论里明确「判断池体积看槽数 × 单槽，判断物理占用看
RSS，判断堆压力看 heap」。

### 4.4 惰性池无法 1e4 次重复

「1e4 次取中位」只适用于**可重复构造**的急切池。惰性池（进程级单例 / TLS）
在同一进程里只能用一次，所以：

| 类型 | 首次样本 | 稳态样本 |
|---|---|---|
| 惰性池 | 51 次独立进程 | 同一进程内第二次（探针内部） |
| 急切池 | 21 次独立进程（每次只构造一次） | 进程内 10000 次取中位 |

这个口径差异会放大「首次 vs 稳态」的倍数（首次来自冷进程），报告在场景 D
的表下明确标注。

### 4.5 WaitNode 池只报内存

`wait_node_pool` 与 `channel_wait_node` 的一次「初始化」就是一次约 200 B 的
`make_shared`，而触发它必须先 `park`。park 期间等待持锁方的时间远大于分配本身，
从外部无法把两者剥离；硬报一个「耗时」只会得到被等待污染的数。
所以这两项**只报内存**，时间列显示 `-`。节点是否真的分配了，用 `Δheap` 证明。

「是否已经 park」用 `Task::state() == GState::kWaiting` 精确判定，而不是 sleep
一个固定窗口：sleep 太短会在 waiter 还没进队列时就 `Unlock`（那时它根本不会
分配 WaitNode，测到的是假样本），太长则把观察时间算进测量。

### 4.6 场景 C 的分段归因

场景 C（全部池）的端到端时间里含一次真实的 `sync::Mutex` 竞争，用于触发
WaitNode 池。这段**一定**包含一次 park→wake 往返，其延迟由调度器与宿主决定，
与池无关。为了不把它算成池成本，程序在场景 C 里逐段计时：

```
scheduler → first-spawn → fiber → sync-* → channel-waitnode → eager-pools
```

并单独给出「与池无关的等待」（park→wake 往返）与「扣除等待后的有效成本」。

场景 C 的时间取 21 次独立进程的**最小值**：调度干扰、缺页、宿主负载只会让
一次运行更慢，不会更快，最小值最接近无干扰的真实成本。同时统计「慢于最小值
3 倍」的次数，用来暴露间歇性的长尾（本次 6/21）。

## 5. 实测数据（2026-11-01，WSL2，Ryzen 9 8945HX）

> 完整、可再生的报告由程序写到 `/tmp/startup_cost.md`；下面是同一次运行的摘要。
> 环境：Linux 6.6.87.2-microsoft-standard-WSL2 / glibc 2.39 / GCC 13.3.0 /
> Release（`-O2 -DNDEBUG`）/ 调度器固定 4 个 worker。

### 5.1 基线（场景 A）

| 程序 | 最小 wall | 中位 wall | 峰值 RSS |
|---|---|---|---|
| 只 main + return | 0.73~0.96 ms | 0.98~1.25 ms | 1.25 MiB |
| 完整二进制（不触发池） | 2.3~2.9 ms | 2.9~3.5 ms | 6.00 MiB |

### 5.2 每个池（场景 B）

下表是**两次独立运行**的区间（本机宿主负载会让单池数字有 ±40% 波动，
绝对值以最新 `/tmp/startup_cost.md` 为准）：

| 池 | 首次 | 稳态/二次 | ΔVmRSS | Δheap | ΔVmSize | 槽数 × 单槽 |
|---|---|---|---|---|---|---|
| `scheduler_core`（对照） | 370~540 µs | - | +256~384 KiB | +53.4 KiB | +40.15 MiB | 4 P |
| `task_block_pool` | 70~110 µs | 47~81 µs | +128 KiB | +36.4 KiB | +64~128 MiB | 96 × 704 B |
| `fiber_bin` | 22~41 µs | 0.4~0.5 µs | +0~128 KiB | +12.7 KiB | +64.25 MiB | 128 × 4 KiB |
| `wait_node_pool` | - | - | +0~256 KiB | +36.7 KiB | +64.39 MiB | 32 × ~200 B |
| `channel_wait_node` | - | - | +128 KiB | +36.4 KiB | +64.39 MiB | 128 × ~200 B |
| `cache_node_pool` | 22~37 µs | 2.2~2.7 µs | +128 KiB | +37.6 KiB | - | 64 节点/块 |
| `lsm_arena` | 4.3~5.3 µs | 29~33 ns | +0 B | +64.5 KiB | +144 KiB | 按需 × 64 KiB |
| `jitter_buffer` | 17~26 µs | 0.12~0.18 µs | +128 KiB | **+784.0 KiB** | +772 KiB | 512 × 1.5 KiB |
| `stream_table` | 12~21 µs | 77~97 ns | +128 KiB | +880 B | - | 256 × ~1 KiB |
| `sfu_router` | 8.8~10.5 µs | 0.12~0.14 µs | +128 KiB | +1.3 KiB | - | 按需 |
| `process_pool` | 4.3~6.9 µs | 48~87 ns | +0 B | +1.1 KiB | - | 4 worker |

读数注意：

- `ΔVmRSS` 含探针内新建的 worker 线程栈（约 128 KiB），`Δheap` 含首次路径上的
  Fiber 对象与状态记录。所以 `wait_node_pool` / `channel_wait_node` 的约 36 KiB
  **不能**当作「WaitNode 大小」读，单个节点约 200 B；
- `ΔVmSize` 的大头是**栈 arena 的虚拟地址预留**（8 槽 × 8 MiB = 64 MiB），
  不是物理内存；`VmRSS` 才是物理占用。
- `Δheap` 与 `ΔVmSize` 跨运行很稳定，**时间列才有波动** —— 这本身就说明
  池的内存结构是确定的、时间受宿主影响。

### 5.3 全部池（场景 C）

| 组合 | 时间 | ΔVmRSS | Δheap |
|---|---|---|---|
| 只启动 Scheduler（对照） | 0.26~0.39 ms | +256 KiB | +53.4 KiB |
| 全部急切池（不含 Scheduler） | 48~59 µs | +384 KiB | +888.5 KiB |
| **全部池 + 跑完第一个任务** | **0.94~0.95 ms** | **+640 KiB** | **+996.3 KiB** |
| 同上，扣掉 park/wake 等待 | **0.89~0.93 ms** | — | — |
| 端到端（含 exec、运行、scheduler 关停） | 5.4~7.2 ms | — | — |

池净增量：**VmRSS +384 KiB**（全部池 640 KiB − Scheduler 256 KiB）；
按分配器口径 **heap +943 KiB**，其中 JitterBuffer 一项占 784 KiB。

场景 C 的分段（同一进程内，取 21 次中最干净的一次；下同）：

| 阶段 | 耗时 | 占比 |
|---|---|---|
| `scheduler`（4 个 worker 启动） | 654~660 µs | 69.6% |
| `first-spawn`（首次 spawn 到任务完成） | 39~72 µs | 4~8% |
| `fiber`（首次创建 + resume） | 7 µs | 0.7% |
| `sync-*`（一次 Mutex 竞争，含 park/wake） | 30~104 µs | 3~11% |
| `channel-waitnode`（一次阻塞收发） | 27~44 µs | 3~5% |
| `eager-pools`（6 个急切池构造） | 67~74 µs | 7~8% |
| **与池无关的等待** | **15~53 µs** | **1.6~5.7%** |

时间取 21 次独立进程的**最小值**：调度干扰只会让一次运行更慢，不会更快，
最小值最接近无干扰的真实成本。21 次里有 **4~6 次**慢于最小值的 3 倍 ——
慢的那些多了一次约 10 ms 的 park→wake 唤醒（见第 9 节）。

## 6. 结论

**启动开销可接受。** 全部池的净内存增量是 **VmRSS +384 KiB / heap +943 KiB**
（全部池 640 KiB 减去只启动 Scheduler 的 256 KiB），其中 JitterBuffer 一项
占 784 KiB（媒体路径独占，纯 HTTP/RPC 服务不会构造它）。时间上：

- 池本身（不含 Scheduler）**48~59 µs**；
- 加上 Scheduler 启动与第一个任务 **0.89~0.93 ms**（已扣除 park/wake 等待）；
- 端到端（exec + 运行 + scheduler 关停）**5.4~7.2 ms**。

对比典型 Web 服务：Nginx 与 Go 静态二进制约 5~15 ms，Node/Express
100~300 ms，Python/gunicorn 300~800 ms，Java/Spring Boot 1~3 s。
本运行时落在 **Go 静态二进制的同一量级甚至更快**，比动态语言框架低一个数量级。

需要强调的是：**启动时间的大头是 Scheduler（约 660 µs，占 70%）与进程本身的
动态链接（约 1.9~2.2 ms），不是池。** 11 个池加起来只有 50 µs 量级，继续压池的
收益上限就是这几十微秒。

## 7. 优化建议

按证据排序，每条都能追到上表的数字：

1. **不要动池，先看 Scheduler 与进程启动。** Scheduler 启动约 660 µs 占场景 C
   的 70%；完整二进制（不触发池）比最小程序多约 1.9 ms / 4.75 MiB，这是动态
   链接与静态初始化的钱。若启动时间是硬指标，收益在这两处，不在池。
2. **JitterBuffer 是唯一值得管的内存项**（+784 KiB，占池 heap 净增量的 83%）。
   它按 `capacity × max_payload_bytes` 预留，默认 512 × 1536 B。
   纯 HTTP/RPC 服务不应构造它；确需构造时按实际 codec 的包长下调
   `max_payload_bytes`（例如语音 320 B 可降到 512 × 384 B ≈ 196 KiB）。
3. **WaitNode 池不需要优化。** 单个节点约 200 B，32 槽上限合计 6.4 KiB；
   真正贵的是触发它的 park/wake 往返（**间歇性约 10 ms**，21 次里 4~6 次），
   那是调度器唤醒路径，不是池。
4. **首个请求延迟敏感的服务应预热一次。** `task_block_pool` 首次 70~110 µs
   vs 稳态 47~81 µs，`fiber_bin` 首次 22~41 µs vs 稳态 0.4 µs —— 一次性成本
   落在第一个请求上。在服务 ready 之前 spawn 一个空任务即可摊掉。
5. **不要为了「省内存」去掉栈 arena 的 VA 预留。** 64 MiB 是虚拟地址，
   RSS 只有几十 KiB；去掉它换来的是每次 fiber 创建一次 `mmap`（约 20 µs）。

## 8. 维护：新增一个池

1. 在 `pools.cpp` 的 `PoolCatalog()` 里加一条 `PoolInfo`（id、中文名、时机、
   源码位置、槽数、单槽、说明）；
2. 写一个 `ProbeXxx(ProbeMode, std::size_t)`：
   - `kMemory` 分支：`Capture()` → 构造/触发 → `Capture()` → `FillDelta()`，
     对象要活到第二次 `Capture()` 之后；
   - `kTime` 分支：循环构造/析构，`FinishTiming()`；
3. 在 `RunProbe()` 里分发；
4. 如果是惰性池，`repeatable = false`（父进程会跑 51 次独立进程）；
5. 重新 `build.sh` 并跑一遍，确认报告里新行有数据且 `ok=1`。

## 9. 限制与已知问题

- **单机、单次运行**：数据是 2026-11-01 在一台 WSL2 机器上的快照。TSC 频率、
  调度延迟、页大小不同的机器上绝对值会变（同一台机器重复跑，单池时间有约
  ±40% 波动，内存几乎不变），但**相对关系**（池 << Scheduler << 动态链接）
  是结构性的。
- **惰性池的「首次」含冷进程成本**：新进程的页表、brk、mmap 都是冷的，
  所以 `fiber_bin` 的 85 倍、`lsm_arena` 的 161 倍这类倍数偏高。
  真实的「第一次请求」介于首次与稳态之间。
- **`Δheap` 会被分配器缓存污染**：glibc 的 free 不归还，连续测多个池时
  前一个池的堆会被后一个池复用。基准用「一次进程一个池」规避，但单个池的
  `Δheap` 里仍含首次路径上的其它分配（见 5.2 读数注意）。
- **park→wake 有间歇性 ~10 ms 长尾，未定位到根因**：同一台机器上 21 次里约
  4~6 次会从几十 µs 跳到约 10 ms（唤醒后被重新调度的延迟）。它是该调度器在
  WSL2 上的真实行为（已用墙钟交叉验证，不是 TSC 假象），基准只做了归因
  （不计入池成本），没有深挖。
  这也意味着**首个 park/wake 的 p99 目前不可控**，是比池大得多的启动/首请求风险。
- **`process_pool` 只测构造**，不测 `Start()` 的 fork 成本（那取决于
  worker 数量与系统负载，不属于「启动期固定开销」）。

## 10. 相关文档

- [`docs/modules/fiber.md`](fiber.md)：FiberBin 与栈 arena 的设计
- [`docs/modules/cache.md`](cache.md)：LRU-K 时间轮与节点池
- [`docs/modules/bounded-queue.md`](bounded-queue.md)：有界队列与背压
- [`docs/engineering-standards.md`](../engineering-standards.md)：性能口径总则
