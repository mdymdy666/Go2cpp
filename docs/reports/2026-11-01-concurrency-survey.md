# 2026-11-01 并发原语横向对比（Go2Cpp vs 工业库，实测）

- 工作树：`/UserData/CodexWorkSpace/Go2cpp`（WSL2 Ubuntu 24.04）
- 构建目录：`.build-bento-wsl`（CMake `Release`，`-O3 -DNDEBUG`）
- 被测快照：`6c08210`（`fix(concurrency): D1/D2/D3`）+ 未提交的 net 层改动；
  库与头文件哈希见 §1.3。对比库：Boost 1.83（系统包）、oneTBB 2021.11（系统包）、
  muduo（`chenshuo/muduo` HEAD，本地编译 base）、libco（`Tencent/libco` HEAD）、
  libfiber（`iqiyi/libfiber` HEAD）、coost（`third_party/coost` 4.0.0）、Go 1.27.1。
- 边界：**只新增 `/tmp` 临时基准与本文档；不改实现；不 commit**。
- 日期说明：任务指定文件名 `2026-11-01`，实际测量时间为 2026-10-31 18:03–18:12。

---

## 0. 结论速览

**一句话**：裸上下文我们只输 coost；带调度语义的 Fiber 切换我们赢 libfiber/Boost.Asio/Go
goroutine，略输 libco；队列（1P1C）全面领先 Go/muduo/TBB；Mutex 与 Go 同级；条件变量热路径
输 Go 一档但远快于 `std::condition_variable`（WSL2 上后者每次停泊 ~29 µs）；**调度器任务
生命周期是最大短板，约为 Go 的 5 倍**。

| 场景 | Go2Cpp | 最强对手（同口径） | 差距 |
|---|---:|---:|---:|
| 裸上下文切换（ns/往返） | **6.43**（`JumpContext`） | coost 2.79 / Boost 8.46 / libco 24.49 | 仅输 coost（2.3×），赢 Boost 1.3× |
| Fiber 全量切换（ns/往返） | **33.80** | libco 24.49 / libfiber 53.38 | 输 libco 1.38×，赢 libfiber 1.58× |
| Channel 1P1C（ns/条，cap1024） | **10.73** | Go 23.36 / muduo 32.40 / TBB 79.13 | 赢 Go 2.2×，赢 muduo 3.0× |
| Mutex 1t（ns/对） | **3.75** | Go 3.24 / std 4.54 | 与 Go 同级（1.16×） |
| Mutex 4t（ns/对） | **12.18** | Go 11.42 / TBB 8.78（纯自旋） | 与 Go 同级（1.07×） |
| 条件变量 1P1C 热路径（ns/往返） | **287.20** | Go 174.85 / std 57 882 | 输 Go 1.64×，赢 std ~200× |
| 调度器任务（ns/任务） | **685.03** | Go goroutine 135.49 | **输 Go 5.06×（最大短板）** |

> 口径提示：表中每一行都来自同一台机器、同一天、`taskset -c 2,3`、5 轮 × 2 次运行取 min；
> 「Fiber 全量」与「裸切换」不可相减（前者含调度器簿记）；Go 的数字是用户态 goroutine
> 调度，不等价于 OS 线程切换。详见 §1.4。

---

## 1. 方法

### 1.1 环境

| 项 | 值 |
|---|---|
| CPU | AMD Ryzen 9 8945HX（12 vCPU / WSL2，`taskset -c 2,3` = 同一物理核的两个 SMT 兄弟） |
| 内核 | 6.6.87.2-microsoft-standard-WSL2 |
| 编译器 | g++ 13.3.0，`-std=c++17 -O3 -DNDEBUG`；Go 1.27.1 `GOMAXPROCS=2` |
| 构建 | `.build-bento-wsl`（Release），被测动态库在测量前**复制到 `/tmp/conc_survey/lib` 固定**，避免并行构建改变被测二进制 |
| 负载 | 测量期间同机还有其它 agent 的构建/压测（load ≈2.8），用多轮取 min 抑制；中位数一并给出 |

### 1.2 测量口径

- **线程绑定**：所有 C++/Go 进程 `taskset -c 2,3`；1P1C 的生产者/消费者分别是这两个 SMT 线程。
- **轮数与统计**：每个二进制内部跑 5 轮（每轮重新构造对象），整组跑 2 次（channel 为 4 次），
  取全部轮次的 **min**；表内 median 为最优一轮的中位数。
- **计时**：`steady_clock` 批量计时（1e6 次量级），不逐次读时钟；1P1C 用自旋起跑门，
  计时不含线程创建；逐条校验 FIFO/序号，任何丢失或乱序直接非零退出。
- **上下文切换**：同一 OS 线程内 ping-pong，一次「进入 + 返回」记 1 次往返（=2 次换栈）。
- **条件变量**：热路径 = 两线程同一把锁 + 同一 CV 交棒；冷路径 = 等待者先睡 1 ms 再 notify，
  只测 notify→唤醒返回的延迟（200 次取 min）。

### 1.3 被测快照（可复现标识）

| 对象 | 标识 |
|---|---|
| 工作树 commit | `6c08210`（测量开始时的 commit 为 `51696dc`，channel 头文件在 18:07 更新后已按新头重测） |
| `include/go2cpp/concurrency/fast_channel.hpp` | `fc692176948be614b7184b2d6c1c60cc` |
| `include/go2cpp/concurrency/channel.hpp` | `d8e5e4a10493d0feb744803c3b59b10c` |
| `include/go2cpp/sync.hpp` | `19de1478e9814402c6d9bc27a628dde2` |
| `libgo2cpp_channel.so.0.1.0` | `67edb1ee4e0ccdc9b919894de314c9a6` |
| `libgo2cpp_fiber.so.0.1.0` | `e78166733326b59e2af4ec079cf719b9` |
| `libgo2cpp_sync.so.0.1.0` | `28dfa618bcd737780022fc8185b14298` |

第三方源码：muduo `9a360b6…`（base 手工编译为 `libmuduo_base.a`）、libco `dc6aafc…`
（`make` 生成 `libcolib.a`）、libfiber（`c/` 下 `make` 生成 `libfiber.a`）、coost 4.0.0
（CMake 生成 `libco.a`）、Boost 1.83 系统 `libboost_context/coroutine`、oneTBB 2021.11 系统包。

### 1.4 不可比清单（结论必须带这些前提）

1. **Go 的 chan 基准不是「OS 线程唤醒」**：两个 goroutine 在 `GOMAXPROCS=2` 下由 Go 运行时
   在 P 上调度，`gopark/goready` 是用户态状态切换；C++ 侧是两个 OS 线程绑 SMT 兄弟核，
   每次交接都付缓存行所有权迁移。Go 的 23 ns 不能与 C++ 的 futex 唤醒直接相减。
2. **Fiber 34/35 ns 含调度器语义**（claim CAS、状态原子、TLS errno 保存），与
   `fcontext`/`tb_context_jump` 的裸换栈不是同一层；本文另列 `JumpContext` 6.43 ns 作同口径对照，
   两者不能相减。
3. **muduo `BlockingQueue` 无界 vs `BoundedBlockingQueue` 有界**：无界时生产者永不阻塞，
   测的是「锁竞争 + deque」；有界 1024 才与 Go/FastChannel 的 cap1024 对齐。
4. **`std::condition_variable` 的 58 µs 是 WSL2 特有的停泊成本**（每次 futex wait→wake ≈29 µs，
   见 §5 冷路径 1.5 µs 的反差）；裸机 Linux 会低一个量级，绝对数不可外推。
5. **TBB `spin_mutex` 是纯自旋、无阻塞等待者**：4 线程 / 2 CPU 超订时它靠烧 CPU 换低延迟，
   不代表「更好」；`std::mutex` 4t 的 min 8.53 / median 13.56 抖动大，只能看量级。
6. **调度器对比语义不同**：Go 的 135 ns 是「创建 + 退出」，Go2Cpp 的 685 ns 是
   「`spawn` 出 Task + Fiber + 入调度队列 + 跑完 + `wait_for` 完成」，后者包含更多工作。

---

## 2. 上下文切换

| 实现 | 机制 | min (ns/往返) | median | 折算单次 |
|---|---|---:|---:|---:|
| coost `tb_context_jump` | 裸 asm（无调度） | **2.79** | 2.80 | 1.4 ns |
| Go2Cpp `detail::JumpContext` | 裸 asm（17 条指令） | **6.43** | 6.47 | 3.2 ns |
| Boost.Context `fcontext` | 裸 asm + 栈检查 | **8.46** | 8.58 | 4.2 ns |
| Boost.Coroutine2 `pull/push` | fcontext + 协程对象 | **8.68** | 8.72 | 4.3 ns |
| libco `co_resume/co_yield` | asm + 协程簿记 | **24.49** | 24.53 | 12.2 ns |
| **Go2Cpp `Fiber::resume`** | asm + 调度器簿记 | **33.80** | 35.04 | 16.9 ns |
| libfiber `ready/yield` | 调度器 + epoll | **53.38** | 53.48 | 26.7 ns |
| Boost.Asio `spawn+post` | 协程 + io_context 队列 | **166.60** | 166.65 | 83.3 ns |
| Go `chan` ping-pong（goroutine 切换） | runtime park/unpark | **171.57** | 178.83 | 85.8 ns |
| `std::condition_variable` 线程交棒 | futex（WSL2） | **59 082** | 61 641 | 29.5 µs |
| `std::thread` 创建 + join | clone/join | **84 439** | 87 258 | — |

**分析**

- **裸换栈**：Go2Cpp 6.43 ns/往返（3.2 ns/次）只输 coost（2.79）。coost 的 asm 更薄（不保存
  errno/TLS、无栈边界检查），Go2Cpp 的 `context_x64.S` 有 17 条指令（6 个 callee-saved push +
  返回地址 + `mov rsp`），两者都在「十几条指令」量级；Boost.Context 多一次栈检查，8.46 ns 合理。
- **带调度语义的切换**：`Fiber::resume` 33.8 ns，比裸切换多 ~27 ns。这 27 ns 来自
  resume claim CAS、Fiber 状态原子读写、TLS errno 保存/恢复与 `bind_caller` 快路径（见
  `src/concurrency/fiber.cpp` 的 TLS `initial-exec`、`resume_from_scheduler`、`bind_caller`），
  与 2026-10-30 优化报告的 34.4 ns 一致。
- **对手定位**：libco 24.49 ns 比我们快 1.38×，但 libco 是单线程非抢占协程、没有跨线程调度与
  取消语义；libfiber 53.38 ns（调度器 + epoll）比我们慢 1.58×；Boost.Asio 166.6 ns 是
  「协程 + io_context 队列」的完整代价；Go 的 goroutine 切换经 chan 往返 171.6 ns，
  是我们的 5.1×。
- **结论**：裸层我们不是最快但属第一梯队；**带调度语义的切换我们是第一梯队**（仅次于更薄的
  libco），比 libfiber/Asio/Go 的对应路径快。

---

## 3. Channel / 队列（1P1C，cap1024）

| 实现 | 语义 | min (ns/条) | median |
|---|---|---:|---:|
| **Go2Cpp `FastChannel<1024>`** | MPMC 无锁 ring（Vyukov） | **10.73** | 12.83 |
| **Go2Cpp `FastSpscChannel<1024>`** | SPSC 无锁 ring | **14.54** | 18.04 |
| Go `chan uint64` cap1024 | runtime hchan | **23.36** | 28.68 |
| Go `chan uint64` cap64 | runtime hchan | 21.64 | 25.37 |
| Go `chan uint64` cap0 | 无缓冲，直接交接 | 86.80 | 94.12 |
| muduo `BlockingQueue`（无界） | mutex + deque + CV | 32.40 | 38.45 |
| muduo `BoundedBlockingQueue(1024)` | mutex + 环形 + CV | 71.16 | 75.61 |
| TBB `concurrent_bounded_queue(1024)` | 有界阻塞队列 | 79.13 | 86.88 |

**单线程快路径（同一线程 TrySend/TryRecv 一对）**

| 实现 | min (ns/对) | median |
|---|---:|---:|
| Go2Cpp `FastSpscChannel` | **1.43** | 1.44 |
| Go2Cpp `FastChannel` | **4.63** | 4.83 |
| Go `chan` cap1024（send+recv） | 18.56 | 19.22 |

**分析**

- **1P1C 我们领先**：`FastChannel` 10.73 ns 是 Go 的 0.46×、muduo 无界的 0.33×、TBB 的 0.14×。
  原因是稳态下生产者只推进 `enqueue_pos`、消费者只推进 `dequeue_pos`，跨核通过每个槽的 `seq`
  交接，没有锁、没有系统调用、没有内存分配；Go 的 hchan 要走 runtime 锁 + `gopark/goready`，
  muduo 每对 put/take 两次 mutex + 条件变量，TBB 有界队列带阻塞等待协议。
- **MPMC 版反而比 SPSC 版快 26%**：`FastSpscChannel::Push/Pop` 每步都要读对端的
  `m_head`/`m_tail`（跨核缓存行），且每次操作无条件查一次等待槽；Vyukov ring 的生产者/消费者
  各自只碰自己那一侧的序号 + 槽位 `seq`，跨核流量更省。单线程时相反（SPSC 1.43 vs MPMC 4.63），
  因为 SPSC 无 `seq` 的 CAS/写。
- **Go 的无缓冲 chan 86.8 ns**：每次交接都要 park/unpark 一个 goroutine；我们的有缓冲 ring
  不需要唤醒（未满/未空），所以差距最大。
- **与任务给定参考值的差异**：参考值 FastChannel 6.95 ns 更接近我们的**单线程对**（4.63 ns）
  而非 1P1C（10.73 ns）；Go 28.59 → 本次 23.36；muduo 38.5 → 32.40。差异来自机器状态、
  轮次与实现版本，量级与排序不变。

---

## 4. Mutex（1t / 4t）

| 实现 | 1t min | 1t median | 4t min | 4t median |
|---|---:|---:|---:|---:|
| TBB `spin_mutex` | **1.66** | 1.73 | **8.78** | 9.39 |
| Go `sync.Mutex` | 3.24 | 3.36 | 11.42 | 12.40 |
| **Go2Cpp `Mutex`** | **3.75** | 3.86 | **12.18** | 12.77 |
| Go2Cpp `SpinMutex` | 3.58 | 3.62 | 8.32 | 17.79 |
| `std::mutex` | 4.54 | 4.72 | 8.53 | 13.56 |

单位：ns / 一次 lock+unlock。4t = 4 线程总操作数 5e6，在 2 个 CPU 上超订。

**分析**

- **1t**：Go2Cpp 3.75 ns 与 Go 3.24 ns 同级（1.16×），优于 `std::mutex` 4.54 ns（0.83×）。
  我们的快路径是 relaxed load + 一次 CAS（`include/go2cpp/sync.hpp` 的 `Mutex::Lock`），
  std 的 futex 版即使无竞争也要走 `lock cmpxchg` + 少量簿记；TBB `spin_mutex` 只有一条
  atomic exchange，1.66 ns 是「裸自旋」的下限，但它没有等待者队列/公平性。
- **4t**：Go2Cpp 12.18、Go 11.42、TBB 8.78、std 8.53(min)/13.56(median)。4 线程抢 2 个 CPU
  时方差极大（std 的 min/median 差 59%），只能看量级：**我们与 Go 在噪声内同级**；
  TBB 更快是因为它纯自旋、不挂起，代价是空转烧核。我们的 `SpinMutex` 4t 8.32–16.57 波动大，
  不作为结论。

---

## 5. 条件变量（1P1C）

| 指标 | Go2Cpp `ConditionVariable` | Go `sync.Cond` | `std::condition_variable` |
|---|---:|---:|---:|
| 热路径 ping-pong（ns/往返） | **287.20** | **174.85** | 57 882 |
| 冷路径 notify→唤醒（ns） | 620 | — | 832（min）/1 524 |

**分析**

- 我们的 287 ns 来自 1P1C 快路径：无锁 `m_fast_waiter` 槽 + 自旋预算 2048 次 pause
  （`src/concurrency/sync.cpp:2079` 的 `kCvFastSpinBudget`，`Wait` 在 `sync.cpp:2209`），
  通知方一次 exchange 即完成交接。Go 的 174.85 ns 是 runtime 内 park/unpark，比我们的
  原子槽 + 自旋观察更薄，**这一项我们输 Go 1.64×**。
- `std::condition_variable` 57.9 µs 不是实现差，而是 **WSL2 上 futex 停泊 ~29 µs/次**：
  同一台机器上「已睡 1 ms 再 notify」的冷唤醒只要 0.8–1.5 µs，反证 58 µs 的主体是
  「wait 侧调度出去」而不是「wake 侧唤醒」。我们的冷唤醒 0.62–1.03 µs 与 std 的
  0.83–1.52 µs **在噪声内同级**——也就是说，我们的优势全部来自「先自旋、避免停泊」，
  不是 futex 更快。长等待（>自旋预算）场景不应期待 200× 优势。

---

## 6. 调度器（可选）

| 指标 | Go2Cpp `Scheduler.spawn+run` | Go goroutine 创建+退出 |
|---|---:|---:|
| ns / 任务 | **685.03**（median 874） | **135.49**（median 185） |

**分析**

- 5.06× 的差距来自每个任务的对象与生命周期：`shared_ptr<Task>` + 独立 Fiber（默认栈池）+
  入队/出队 + `wait_for` 完成通知；Go 的 goroutine 是 2 KB 初始栈、P 本地缓存批量复用、
  退出走 freelist。**这是本轮最大的短板**，也是「调度器」层面的真实差距。
- 但要注意口径：Go 的 135 ns 只含创建+退出，不含任何调度队列等待；我们的 685 ns 含
  1P/1M 调度器上的入队、执行、完成通知全过程。若要压这个数，方向是 Task/Fiber 对象池化、
  批量 spawn、完成通知批量化，而不是继续抠换栈汇编（裸切换只占 6.4 ns）。

---

## 7. 强弱项与原因

| 项 | 我们 | 最强对手 | 判定 | 原因 |
|---|---:|---:|---|---|
| 裸上下文切换 | 6.43 ns | coost 2.79 | 强（第二） | 17 条指令换栈；coost asm 更薄 |
| Fiber 全量切换 | 33.80 ns | libco 24.49 | 强（第二） | 调度语义完整；libco 更薄但无跨线程 |
| Channel 1P1C | 10.73 ns | Go 23.36 | **强（第一）** | Vyukov 无锁 ring，无 syscall/分配 |
| Channel 单线程 | 1.43 ns | Go 18.56 | **强（第一）** | 掩码取模 + 无等待协议 |
| Mutex 1t | 3.75 ns | Go 3.24 / TBB 1.66 | 同级 | 快路径 1 CAS；TBB 是裸自旋 |
| Mutex 4t | 12.18 ns | Go 11.42 | 同级 | 有等待者挂起/公平性，非纯自旋 |
| 条件变量热路径 | 287 ns | Go 175 | 弱一档 | 原子槽 + 自旋；Go 用户态 park 更薄 |
| 条件变量冷唤醒 | 0.62 µs | std 0.83 µs | 同级 | 都走 futex，差距在噪声内 |
| 调度器任务 | 685 ns | Go 135 | **弱（5×）** | Task/Fiber/队列/完成通知的重对象模型 |
| 线程创建 | — | std 84 µs | 不适用 | 我们不做线程创建对比 |

**总体判断**：数据面（channel、裸切换、Fiber 切换、Mutex 快路径）已达到或超过工业库水平；
控制面（调度器任务生命周期、条件变量热路径）落后 Go 一档。后续优化优先级：
① Task/Fiber 对象池与批量完成（调度器 5× 差距）；② CV 等待节点/自旋预算调优（1.6× 差距）；
③ 不是换栈汇编（只占 6.4 ns，压不出量级）。

---

## 8. 复现

基准源码全部在 `/tmp/conc_survey/src`（本机），二进制在 `/tmp/conc_survey/bin`：

```
/tmp/conc_survey/src/bench_ctx.cpp       # 上下文：Go2Cpp/Boost/coost/libco/libfiber/Asio/std
/tmp/conc_survey/src/bench_channel.cpp   # 1P1C：FastChannel/FastSpsc/muduo×2/TBB
/tmp/conc_survey/src/bench_mutex.cpp     # Mutex 1t/4t：Go2Cpp/std/TBB
/tmp/conc_survey/src/bench_cond.cpp      # CV 热路径 + 冷唤醒
/tmp/conc_survey/src/bench_sched.cpp     # Scheduler spawn
/tmp/conc_survey/src/bench_go.go         # Go：chan/Mutex/Cond/goroutine
/tmp/conc_survey/src/bench_libfiber.cpp  # libfiber ready/yield
```

运行（示例）：

```
taskset -c 2,3 /tmp/conc_survey/bin/bench_ctx 1000000 5
taskset -c 2,3 /tmp/conc_survey/bin/bench_channel 200000 5
taskset -c 2,3 /tmp/conc_survey/bin/bench_mutex 10000000 5
taskset -c 2,3 /tmp/conc_survey/bin/bench_cond 50000 5
taskset -c 2,3 /tmp/conc_survey/bin/bench_sched 20000 5
taskset -c 2,3 /tmp/conc_survey/bin/bench_go -n 200000 -reps 5
taskset -c 2,3 /tmp/conc_survey/bin/bench_libfiber 200000 5
```

被测库固定为 `/tmp/conc_survey/lib`（从 `.build-bento-wsl` 复制，`-Wl,-rpath` 指向该目录），
第三方源码在 `/tmp/conc_survey/vendor`（libco/muduo/coost/libfiber），构建产物在
`/tmp/conc_survey/build`。原始输出在 `/tmp/conc_survey/res/*.txt`。

---

## 9. 附注

- 本轮的 9 类对比对象**全部实测**（Go、muduo、Boost.Context、libco、libfiber、TBB、
  std、Boost.Asio、coost），没有用文献数字替代。
- libfiber 官方 benchmark 与本文口径不同（其自带测试测的是网络吞吐），因此只采用本文
  同口径的 53.38 ns，不与官方数字混排。
- 测量期间工作树有并行任务在改 `channel`/`net`；本文已固定库哈希并在 18:07 头文件更新后
  重测 channel（结果差异在噪声内），但**重跑时请以 §1.3 的哈希为准**。

### 9.1 任务给定参考值 vs 本次实测

| 项 | 任务参考值 | 本次实测 | 说明 |
|---|---:|---:|---|
| Fiber 切换 | 34.4 ns | 33.80 ns | 一致（同口径往返） |
| FastChannel 1P1C | 6.95 ns | 10.73 ns（cap1024）/ 4.63 ns（单线程对） | 参考值介于两者之间，口径不同 |
| Go chan 1P1C | 28.59 ns | 23.36 ns（cap1024） | 同量级，机器状态差异 |
| muduo BlockingQueue | 38.5 ns | 32.40 ns（无界） | 同量级 |
| Mutex 1t | 3.7–5.2 ns | 3.75 ns | 一致 |
| 条件变量 1P1C | 146 ns | 287.20 ns | 本次为双线程完整往返，参考值可能为单向/更热路径 |
