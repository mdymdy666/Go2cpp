# 2026-10-30 并发原语性能优化技术文档

- 工作树：`/UserData/CodexWorkSpace/Go2cpp`（WSL2 Ubuntu 24.04）
- 基线 commit：`134c7a3`（今日最后一个 perf 提交）
- 日期：2026-10-30
- 范围：并发原语（Fiber / Scheduler / Channel / sync / Context / Future）的优化前后数据、根因、手段与源码落点
- 声明：本文只整理当日已提交的优化成果，**不包含任何代码改动**，不 commit

---

## 0. 结论速览

今日共 63 个提交，其中 13 个 `perf(concurrency)`。按原语汇总的最终数字：

| 原语 / 场景 | 优化前 | 优化后 | 变化 | vs Go |
|---|---:|---:|---:|---:|
| Fiber 切换（一次完整往返） | 72.9 ns | **34.4 ns** | −52.9% | 无同口径对照 |
| Fiber `resume_from_scheduler` | 55.2 ns | **31.3 ns** | −43.3% | — |
| Fiber 批量完成（10 万并发） | 13.01 µs/f | **4.45 µs/f** | −66% | — |
| Fiber 完成后 VmSize | 4.31 GiB | **0.56 GiB** | −87% | — |
| 深栈 `mprotect` 次数（8 MiB） | 133 | **21** | −84% | — |
| 调度链 pingpong | 166.4 ns | **111.2 ns** | −33.2% | Go 1.3 同场景 ~55–75 ns（估） |
| 调度链 channel cap0 | 314.4 ns | **215.8 ns** | −31.4% | Go 103.8 ns / 7.43×（旧口径） |
| 调度链 CV 1P1C | 503.4 ns | **397.7 ns** | −21.0% | Go 100.2 ns |
| WaitNode park+wake 往返 | 16.4 µs | **0.83 µs** | 19.7× | 无同口径对照 |
| WaitNode p95 | 55.4 µs | **1.15 µs** | 48× | — |
| Channel cap0 | 861.0 ns | **209.2 ns** | −76% | Go 103.8 ns |
| Channel cap64 | 205.0 ns | **135.7 ns** | −34% | Go 27.6 ns |
| Channel cap1024 | 133.0 ns | **96.0 ns** | −28% | Go 25.5 ns |
| FastSpscChannel cap1024 | — | **20.61 ns** | 新增 | **0.81×（快于 Go）** |
| Mutex 1t | 5.55 ns | **3.50 ns** | −36.9% | Go 3.24–3.36 → **1.08×** |
| WaitGroup 1t / 4t / 16t | 5.55 / 25.41 / 63.10 | **3.77 / 9.49 / 19.64** | −32% ~ −69% | **0.71× / 0.77× / 1.01×** |
| RWMutex 混合 4t | 304.13 ns | **32.87 ns** | 9.3× | Go 46.24 → **0.71×** |
| Context `IsDone` | 5.68 ns | **0.42 ns** | −92.6% | Go 6.24 → **0.07×** |
| Future 全生命周期 | 120.5 ns | **79.1 ns** | 1.52× | Go 82 → **0.96×** |
| SpinMutex 4t / 16t | 31.63 / 32.09 | **18.48 / 14.26** | −40% / −57% | 快于同环境 `sync::Mutex`（24.63） |

> 口径提示：表中「调度链」行来自 `bench_scheduler_park`（pingpong 往返），
> 「Channel」行来自 `bench_channel_opt`（1P1C 200k 消息），两者单位都是 ns/次
> 但基准不同，**不可跨行相减或直接比较**；每行只看自己的前后变化。

正确性门禁：Fiber/Thread 混合亿级压测 **7 模型 × 10 轮 × 1e8 = 7×10⁹ 次操作、70 轮、0 失败**；
ASan/UBSan 随机序列 0 报告；TSan 在本 WSL 环境不可用（见 §1.5），以 ASan + 随机压测替代。

---

## 1. 方法论

### 1.1 环境

| 项 | 值 |
|---|---|
| CPU | AMD Ryzen 9 8945HX（6 核 12 线程 / 12 vCPU，SMT） |
| 内存 | 19 GiB |
| 内核 | 6.6.87.2-microsoft-standard-WSL2（ext4 根分区） |
| 编译器 | g++ 13.3.0，`-std=c++17/-std=c++20 -O3 -DNDEBUG` |
| Go | go1.27.1 linux/amd64，GOMAXPROCS=12 |
| 构建 | `.build-bento-wsl` / `.build-p0-wsl` / `.build-p1-test-wsl`（CMake Release） |

所有对比数字都来自同一台机器、同一天、同一批构建目录，不跨机器拼接。

### 1.2 基准口径（六条硬规则）

1. **绑核**：跨线程原语一律 `taskset -c 2,3`（同一物理核的两个 SMT 线程），
   把「跨核缓存行迁移」固定成常量，避免调度器把两个线程扔到 NUMA 远端核。
   单线程微基准不绑核，但要求连续 3 轮 min 稳定在 5% 内。
2. **负载记录**：每个报告都记录测量时的 `loadavg` 与 `procs_running`。
   今日机器被多个 agent 共享，`loadavg` 从 1.5 波动到 246，因此：
   - 绝对数字只在「同轮次、同负载档」内比较；
   - 关键结论用**同轮交替 A/B**（同一二进制、`LD_LIBRARY_PATH` 切两套库）复核，
     消除负载漂移；
   - Fiber 栈类长测优先用 `CLOCK_THREAD_CPUTIME_ID` / `CLOCK_PROCESS_CPUTIME_ID`，
     用 CPU 时间替代墙钟，宿主抢占不会放大分子。
3. **多轮取 min**：微基准每档 3–20 轮，取 min 作为「无干扰下界」，同时给出
   中位数与 p95/p99。取 min 而不是均值，是因为 WSL2 上任何一次宿主抢占都会
   把均值抬到不可比；min 对「实现变快」敏感、对「宿主变吵」不敏感。
4. **Go 同场对照**：Go 侧不用 `go test -bench`（框架自身有 20–50 ns 开销），
   而是独立 `main` 程序、进程内 3 轮取 min；C++ 侧独立可执行、5 轮取 min；
   两者**串行交替执行**（不并行跑两个基准），并用同环境的 Go min 作分母，
   避免「报告里的 Go 数字」与「今天的 Go 数字」跨环境相除。
5. **语义校验**：Channel 基准逐条校验序号，要求乱序/丢失/重复为 0；
   WaitGroup/CV 校验计数守恒；Fiber 基准校验返回值与栈内容。
   性能数字只有在语义校验通过的那一轮才录入。
6. **防退化门槛**：任一场景 P99 退化 >5% 即回滚该步；P0-3 首版（两次 CAS）
   就因 CV 407→466 ns（−14.5%）被回滚重做为单 CAS。

### 1.3 数据可信度分级

| 级别 | 判定条件 | 本文用法 |
|---|---|---|
| A | 同轮交替 A/B、≥3 轮 min、语义校验通过、负载已记录 | 作为「优化前后」的正式结论 |
| B | 同机同日、单轮或跨轮 min，负载有记录 | 作为趋势与量级引用，标注「±10%」 |
| C | 跨天、跨机器、跨 harness，或宿主负载不可控 | 只作参考，不进入达标判定 |

本文表格默认 A 级；`/tmp/conc.native[*]` 类超订场景（256 线程 / 12 核）明确标 C 级并
在 §1.5 列为不可用数据。

### 1.4 为什么这些数字可信

- **同一二进制 A/B**：调度链 P0、Channel P0、Fiber 栈 arena 都是「改前源码编一份库、
  改后源码编一份库」，同一 bench 二进制通过 `LD_LIBRARY_PATH` 切换，排除编译器与
  链接布局差异。
- **可复现命令**：每份 `/tmp/*.md` 报告末尾都有构建目录 + bench 可执行名 + 参数；
  仓库内 bench 源码在 `tests/concurrency/bench_*.cpp`，可独立重跑。
- **失败如实记录**：未达标项（Cond、Channel MPMC、RW 写、Context 创建、WG 唤醒）
  在 §4 单列，不与达标项混排；被否决的方案（自研两态 spinlock 丢唤醒、
  Mutex handoff 挂起、自旋预算 0/32/64）也留在报告里。

### 1.5 已知口径陷阱（不可比清单）

1. **Go 的 1P1C channel 基准不是跨核基准**：Go 的两个 goroutine 跑在同一个 P
   （同一 OS 线程）上，`gopark`/`goready` 只是运行时内的状态切换；C++ 基准是
   两个 OS 线程绑两个 SMT 线程，每次交接都付缓存行所有权迁移（40–60 ns）。
   因此 `Channel cap1024 96 ns vs Go 25.5 ns` 里，至少 40–60 ns 是执行模型差异，
   不是实现浪费。
2. **`Fiber 切换 34.4 ns` 与「裸切换 15.75 ns」不同口径**：前者含调度器
   claim/状态字/TLS 恢复的一次 resume，后者是 `JumpContext` 往返/2；不能相减。
3. **`WG Done→Wait 唤醒 35.7 µs` 的主体是内核**：其中 `FUTEX_WAKE` 一次就
   19–20 µs，任何用户态优化都动不了它。
4. **`conc.native[N]`（N=64/256 等待线程）为 C 级**：12 核上超订，p50 波动
   45–50 ms，`notify` 调用耗时跨轮差 10 倍，已明确不作为结论。
5. **TSan 在本 WSL 不可用**：任何 TSan 程序（含空 `main`）报
   `unexpected memory mapping`，`vm.mmap_rnd_bits` 无权限改、`setarch -R` 仍段错误。
   TSan 结论以「其他机器/其他时段」的报告为准，本文不引用 TSan 数字。
6. **ASan + 默认 4 KB Fiber 栈必然 stack-overflow**：ASan 给每帧加红区，
   4 KB 栈放不下。因此调度链的 ASan 验证统一用 64 KB 栈 smoke（20k 轮 park/wake），
   这属于工具限制，不是被测代码缺陷。

---

## 2. 原语逐项

### 2.1 Fiber 切换

**结论**

| 指标 | 优化前 | 优化后 | 变化 |
|---|---:|---:|---:|
| resume→Suspend→resume 往返（1e6 次） | 72.9 ns | **34.4 ns** | −52.9% |
| 调度器 `resume_from_scheduler` | 55.2 ns | **31.3 ns** | −43.3% |
| 逐次采样 P50 | 80 ns | **50 ns** | −37.5% |
| 逐次采样 P99 | 126 ns | **51 ns** | −59.5% |
| `JumpContext` 裸往返（对照） | 6.2–6.7 ns | 6.2–6.7 ns | 未变 |

**根因**：换栈汇编只值 6.2–6.7 ns（17 条指令：6 个 callee-saved push、1 个返回地址、
`mov rsp` 换栈）。72.9 ns 里约 66 ns 是 Fiber 层簿记，三块：

1. 7 个热路径 `thread_local` 是 general-dynamic 模型，每次访问生成
   `lea TLS_index; call __tls_get_addr@plt`，实测每次 5–8 ns，一次往返访问 6–10 次；
2. `bind_caller()` 每次 resume 都加锁刷新父链，即使父级从没变过（2 次 mutex +
   `shared_ptr` 引用计数 + 父记录拷贝）；
3. errno 的 `noinline` 存取函数与冗余状态写。

**手段**（源码位置按当前工作树）

- 7 个热路径 TLS 加 `tls_model("initial-exec")`：`src/concurrency/fiber.cpp:127`
  （宏）及其使用点；`t_errno_slot` 每线程缓存 `&errno`：`fiber.cpp:286`；
  `swap_errno` 合并「读旧值 + 写新值」，往返 noinline 调用 5→3 次：`fiber.cpp:299`。
- `bind_caller()` 快路径：父级已绑定且等于本次调用者时只做 2 次 atomic load，
  `m_parent`/`m_parent_bound` 改 atomic、写侧仍持锁：`fiber.cpp:2009`、
  `fiber.cpp:2452-2457`。
- `resume_from_scheduler()`：`fiber.cpp:1983`；`suspend()` 返回路径删 4 次冗余
  原子/TLS 写与一次 errno 恢复。
- growable 支持结果每线程缓存，`m_stack_growable_active` 在 `initialize_context` 固化。

**代价/风险**：initial-exec 假定「库在进程启动时加载」；若未来需要 `dlopen` 本库，
必须回退 general-dynamic（代码注释已写）。剩余 34.4 ns 中约 28 ns 是 resume claim CAS、
状态原子读写与 TLS 保存/恢复，继续压缩需改 record 状态布局，收益 <5 ns。

### 2.2 Fiber 栈 arena

**结论**

| 指标（10 万并发） | 优化前 | 优化后 | 变化 |
|---|---:|---:|---:|
| 首切（建栈 + 首次 resume） | 15.48 µs/f | **11.37 µs/f** | −27% |
| 批量完成（resume 到终态） | 13.01 µs/f | **4.45 µs/f** | **−66%** |
| 完成后 VmSize（栈缓存驻留） | 4.31 GiB | **0.56 GiB** | **−87%** |
| 8 MiB 深栈（3 万帧） | 133 次 mprotect | **21 次** | −84% |
| FiberBin 任务复用 | 130.9 ns/task | **106.0 ns/task** | −19% |
| 并发峰值 VmSize | 781.3 GiB | 781.5 GiB | 不变（8 MiB VA/个的预留策略） |

**根因**：WSL2 上每次建栈的真实系统调用成本——`mmap(8 MiB)+mprotect(4 KiB)` 4.4–7.0 µs、
`munmap(8 MiB)` 2.7–8 µs、首次触碰栈页缺页 2.2 µs。10 万并发时每个 Fiber 都要走
「mmap → mprotect → munmap」，且 `go()` 单次 ~14.5 µs 主要落在 mmap/mprotect/munmap
与首次触碰缺页上。
另外栈增长固定 64 KiB 一步，8 MiB 深栈要 133 次 `mprotect`。

**手段**

- 进程级 `StackArena` / `StackArenaClass` / `StackArenaRegistry`：按 `(reserve, usable)`
  分大小类，每类首次 `mmap(slots × slot_size, PROT_NONE|MAP_NORESERVE)`，
  槽数 8→16→32→64 自适应；创建从空闲表取「已提交过初始窗口」的槽（零系统调用），
  销毁回空闲表不 munmap；整块 arena 全空且超出 `kStackArenaRetainedSlots=64`
  才整体 munmap 一次。位置：`src/concurrency/fiber.cpp:568`（注释块）、
  `fiber.cpp:606-609`（常量）、`fiber.cpp:619`（StackArena）、`fiber.cpp:652`（registry）、
  `fiber.cpp:672`（StackArenaCreateLocked）。
- 锁外 munmap：一次 512 MiB arena 的 munmap 实测可达 166 µs，持 registry 锁会卡住
  其它线程建栈，因此 munmap 移到锁外。
- 栈增长几何步长：64 KiB → 128 KiB → … → 1 MiB 封顶，复位/复用重置为 64 KiB；
  多提交的只是 VA，RSS 不变；guard 检测粒度从 64 KiB 放宽到最多 1 MiB
  （仍在原 256 KiB `rsp/fault` 判定窗口内，代码注释已写明）。
- FiberBin 复用：调度器路径完成时把栈保留在 Fiber 上（`RecycleForScheduler`），
  下个任务 `reset_for_scheduler` 只校验终态 + `prepare_reuse()` 复位增长区，
  正常路径零系统调用；布局不一致（stack_size/reserve/growable 变了）则丢弃重取。
- 其它：`native_page_size()` 结果缓存（原来每个 Fiber 构造都调 `sysconf`）。

**与 Go 1.3 对照**：Go 用每 M 的 8 KiB 栈缓存 + 可搬栈；C++ 栈上有裸指针不能搬栈，
只能「预留大 VA + 原地 mprotect 增长」，所以 8 MiB/个的峰值 VmSize 是设计取舍，
不是泄漏。本次优化把它的**残留**从 4.31 GiB 压到 0.56 GiB。

### 2.3 调度链 P0（runq / 状态字 / dequeue 即 claim）

**结论**

| 场景 | 优化前（父口径） | P0 后 | 变化 |
|---|---:|---:|---:|
| pingpong（1P1M 往返） | 166.4 ns | **111.2 ns** | −33.2% |
| Channel cap0（1P2M） | 314.4 ns | **215.8 ns** | −31.4% |
| CV 1P1C | 503.4 ns | **397.7 ns** | −21.0% |
| yield（同 M 换出 + 本地队列） | — | 117.0 ns | — |
| ext_wake（外部线程唤醒 park G） | — | 461 ns | — |
| conc 批量唤醒（200k G，4P） | — | 164.6 ns/G | — |

同源 A/B（8 轮中位，基线 = 当日更早的 HotSpinLock + initial-exec TLS 版）：
pingpong 119.6→117.4（−1.8%）、cap0 239→230（−3.8%）、CV 407→403（−1.0%）。
说明：**大头在上午那轮（TLS + 自旋锁）已经被吃掉**，P0 的净收益来自队列与 dequeue 路径；
相对「全 P0 前」的 166.4/314.4/503.4 才是用户可见收益。
口径注意：本表由 `bench_scheduler_park` 产出，与 §2.5 `bench_channel_opt` 的
1P1C 200k 消息基准不是同一套程序，两处 cap0/channel 数字不可互比。

**根因**：一次 park→wake→resume 的加锁账是 **4 次 `m_transition_mutex` + 2 次
`processor.mutex` + 2 次 `m_resume_claim` CAS**（此前文档笼统写「6 次 mutex」，
逐点核对后不准确）。Go 同场景是 **0 次锁 + 1 次 CAS（runqget）+ 1 次 release store
（runqput）**。差距不在「调度逻辑」，在同步原语：`shared_ptr` 队列元素的引用计数、
`std::deque` + mutex、以及每次 enqueue 的状态锁。

**手段**

1. **P0-2 先做：256 槽无锁 MPMC 环形队列**（`LocalRunQueue`，
   `src/concurrency/scheduler.cpp:336`）。结构照抄 Go 1.3 `runtime/proc.c:2912-3027`：
   head 消费者 CAS、tail 生产者发布、槽位 `sequence` 标记可见性、满时
   `push()` 返回 false 溢出到全局队列（对应 `runqputslow`）、窃取一次取一半
   （对应 `runqgrab`）。**与 Go 的差异**：Bento 的 P 没有唯一 owner（replacement M
   可与基础 M 共享同一 P、外部唤醒会精确投递到目标 P），所以 tail 也用 CAS，
   得到 Vyukov 有界 MPMC；生产端/消费端仍各只付一次 CAS。
2. **P0-1 状态机去 `transition_mutex`**：`m_state` 与 `m_wake_pending` 合并为单个
   32 位原子字 `m_state_word`（低 8 位状态、bit8 唤醒令牌）：
   `include/go2cpp/concurrency/scheduler/scheduler.hpp:685`（kStateMask）、`:732`（成员）。
   `park_for_scheduler()`（`scheduler.cpp:933`）、`wake_for_wait_impl()`
   （`scheduler.cpp:862`）、`wake_for_scheduler()`、`mark_yielded_fast()`
   （`scheduler.cpp:1047`）全部改单字 CAS 循环。`m_transition_mutex` 只留在冷路径：
   owner/gate 绑定、Fiber 创建/销毁、cancel、`resume_after_failed_park`、
   `recover_orphan_running`、`try_mark_queued*`、`clear_queued`。
3. **P0-3 dequeue 即 claim**：`Task::claim_from_queue()`（`scheduler.cpp:998`）
   单 CAS `Runnable → Running` 即执行权仲裁；CAS 前 `m_queued == true` 阻止外部重复
   入队，CAS 后 `state == Running` 阻止补发布；成功后 `queued=false`、
   `execution_claim=true`。worker_loop 三个取任务点统一改用它，失败走
   `recover_unclaimed()`；`try_mark_running()` 保留为公开的加锁版本。

**配套修复：`-Wl,-z,now`**。改动前 `scheduler` 测试 3/3 段错误，gdb 栈固定在
`begin_shutdown_locked → wake_for_scheduler` 的 PLT 懒绑定（`do_lookup_x`/`_dl_fixup`）。
根因不是调度器：`_dl_runtime_resolve_xsavec` 把完整 xsave 状态压到**当前栈**，
而 Fiber 初始栈只有 1 页（4 KB），较深关停链首次解析符号时越过栈边界写进 guard page。
`LD_BIND_NOW=1` 下全绿证实；已在顶层 `CMakeLists.txt` 加 `add_link_options(-Wl,-z,now)`。

**回滚记录**：P0-3 首版「先抢 claim 再改状态、2 次 CAS」使 CV 407→466 ns（−14.5%），
超过 5% 门槛，回滚重做为单 CAS，复测 403 ns 保留。

### 2.4 WaitNode / ParkingCondition

**结论**

| 指标（n=20000，3 轮） | 基线 | 新版 | 变化 |
|---|---:|---:|---:|
| park+wake 往返 mean | 16.4 µs | **0.83 µs** | **19.7×** |
| p95 | 55.4 µs | **1.15 µs** | **48×** |
| p50（单轮） | 1.9–3.8 µs | 291–782 ns | — |

**根因**：等待队列是 `std::mutex + std::deque<std::shared_ptr<WaitNode>>`，
每次 park 有堆分配 + 引用计数；取消/超时清理是 `std::find + deque::erase`（O(n)），
N 个等待者逐个被唤醒时退化为 O(N²)；`notify_all` 在锁内逐个 `FUTEX_WAKE`。
更隐蔽的是 p95：基线通知方常被队列锁/分配/引用计数拖过等待者的 18 µs 自旋窗口，
等待者已睡进 futex，于是 p95 达到 55 µs。

**手段**

- 队列改**侵入式双向链表 + thread_local 节点池**：`include/go2cpp/concurrency/core/parking_condition.hpp:34`
  （设计注释）、`:196-197`（m_head/m_tail）。节点复用，`wait` 热路径堆分配与
  `shared_ptr` 引用计数全部消失。
- 队列锁改 `core::FutexMutex`（locked+waiters 双位状态、唤醒链确认接手）；
  取消/超时清理从 O(n) 摘链变 O(1)。
- `notify_all` 改「锁内一次认领 + 锁外批量唤醒」：native 走一次
  `FUTEX_WAKE(INT_MAX)` 广播 + `futex_waitv` 双字等待，不再逐个唤醒。
- 节点内 `result` 与 `futex_seq` 合并，「写终态 + 推进代际」只付一次缓存行迁移；
  `Wake()` 增加 native 无锁快路径。

**遗留（C 级数据）**：`conc.native[N]`（64/256 等待线程）被 OS 调度主导，
p50 45–50 ms、跨轮差 10 倍，不作为结论；队列结构 O(N²)→O(N) 的收益需要在
空载机器、线程数 ≤ 核数时重测。

### 2.5 Channel P0

**结论**（1P1C，200k×3，`taskset -c 2,3`，ns/消息，三轮中位数）

| 场景 | 基线（P0 前） | P0 后 | 变化 | Go 同场景 |
|---|---:|---:|---:|---:|
| cap0 | 861.0 | **209.2** | −76% | 103.8 |
| cap64 | 205.0 | **135.7** | −34% | 27.6 |
| cap1024 | 133.0 | **96.0** | −28% | 25.5 |
| 空 TryRecv（cap0/cap1024） | 14.8 | 20.1 | +36%（变慢） | — |
| 单线程 Send+Recv（cap1024） | 15.2 | 17.0 | +12%（变慢） | — |

**根因**（对照 `docs/research/go-channel-opt.md` 的三条结构性差距）：

1. **锁的数量**：Go `hchan` 只有一把锁（`chan.go:54`），且注释明确「锁保护 hchan
   全部字段以及阻塞其上的 sudog 的部分字段」；Bento 一次阻塞收发要过 channel 锁 +
   `ParkingCondition` 队列锁 + 每节点唤醒锁。
2. **等待节点分配**：Go 的 sudog 从 per-P 池取（`p.sudogbuf[128]`，稳态零分配）；
   Bento 每次 `make_shared<PendingSend/Recv>` + `deque<shared_ptr>` 节点。
3. **热路径共享写**：Bento 每次 send/recv 都 `++m_generation`（跨核 RFO）；
   Go 热路径不写任何共享计数器。

**手段**（`include/go2cpp/concurrency/channel.hpp`）

1. **单锁化**：等待队列搬进 `Channel`（侵入式双向链表），notify 不再经过
   `ParkingCondition`；一次阻塞收发只竞争 Channel 自身一把锁。
2. **节点池化**：`detail::WaitNodePool`（`channel.hpp:916`），thread_local 空闲链，
   上限 128（对应 Go `p.sudogbuf[128]`），稳态零堆分配。
3. **去 generation 原子**：热路径交接/关闭改用 `PublishChangeLocked()`（仅
   watchers>0 才写 `m_generation`）；`WaitForChange` 用独立
   `m_change_mutex + ParkingCondition`（`channel.hpp:1678`），与自旋锁解耦。
4. **自旋锁替 `std::mutex`**：`detail::ChannelSpinLock`（`channel.hpp:963`，
   成员 `:3274`）：64 次 PAUSE 自旋 + `core::FutexMutex` 慢路径，无竞争单次 CAS，
   绝不持锁 park。
5. **取消路径改造**：回调只持 `weak_ptr<CancelHub>` + `(节点地址, generation)`，
   在 hub 锁 + Channel 锁内按身份定位节点，CAS `signal` 后 `futex_wake(node->seq)`，
   不再持 `shared_ptr<Pending*>`。

**过程中修掉的三个正确性缺陷**（都真实踩到并复现）：

- 自研两态 spinlock 的 `exchange(0) + FUTEX_WAKE(1)` 假设「被唤醒者一定接手锁并
  继续唤醒」，多等待者 + 2 核绑定时不成立，出现「锁空闲、消费者睡在 LockSlow、
  生产者睡在 WaitNative」的永久挂起（gdb 抓栈确认）；最终复用 `FutexMutex`
  的唤醒链协议，外层加 64 次 PAUSE 自旋。
- 入队窗口：注册取消回调不能在锁内（`DoneSignal::AddCallback` 在已取消时会同步
  执行回调，回调要拿 Channel 锁，自旋锁不可重入）；入队前后各做一次「窗口闭合」检查。
- 取消回调注册从「持锁注册」改「入队后解锁注册 → 重新加锁」，注册失败清理节点。

**代价**：空 `TryRecv` 与单线程收发变慢 4–5 ns——`std::mutex` 无竞争解锁是单次
store，`FutexMutex` 解锁是 `fetch_and` 原子 RMW（约 7 ns）。这是为「多等待者唤醒链
正确性」付的代价，自研两态 spinlock 虽快但实测丢唤醒，已否决。

### 2.6 FastChannel / FastSpscChannel

**结论**（空载，`taskset -c 2,3`，200k×5，median）

| 项 | 结果 | Go | 倍数 |
|---|---:|---:|---:|
| `FastSpscChannel` cap1024 | **20.61 ns** | 25.53 | **0.81×（快于 Go）** |
| `FastSpscChannel` cap64 | 25.59 ns（min 17.43） | 24.83 | 1.03×（min 0.70×） |
| `FastChannel` cap64 | 118.62 ns | 24.83 | 4.78× |
| `FastChannel` cap1024 | 95.28 ns | 25.53 | 3.73× |
| `FastChannel` cap0 | 814.23 ns | 89.01 | 9.15× |
| 单线程 `TryRecv+TrySend` | **1.12 ns/op** | — | — |

**根因/定位**：数据面不需要 Context/超时/取消/select/调试，通用 `Channel` 为这些
语义付的成本在数据面是纯浪费。SPSC 场景下生产者与消费者各自独占 head/tail，
可以完全无锁。

**手段**（`include/go2cpp/concurrency/fast_channel.hpp`，新增）

- `FastChannel<T, Cap>`：编译期容量、`std::array` 零堆分配、单锁 MPMC
  （`ChannelSpinLock`），无 Context/超时/取消/select/调试。
- `FastSpscChannel<T, Cap>`：单生产者单消费者无锁 ring（原子 head/tail，
  acquire/release），阻塞时才用一把 mutex 登记等待者；唤醒所需的 `FUTEX_WAKE`
  通过 `FastWakeHint` 带到锁外执行。
- 阻塞语义复用 Channel P0 的 per-node futex / Scheduler park 原语，支持
  Fiber↔Fiber / Fiber↔Thread / Thread↔Fiber / Thread↔Thread 四种组合。

**结论边界**：SPSC cap1024 已快于 Go；`FastChannel`（通用 MPMC、单锁）在 1P1C
单条延迟上达不到 Go 的 24.8 ns——差距不在 Channel 逻辑，而在执行模型（Go 是同 P
goroutine 直接 handoff，无系统调用；C++ 是两 OS 线程经共享 cacheline + per-node
futex 交接）。

### 2.7 Mutex

**结论**（1e7 次 lock/unlock pair；C++ 5 轮 min，Go 3 轮 min，串行交替）

| 场景 | Go（同环境） | Bento 基线 | Bento 本次 | 倍数（min/min） |
|---|---:|---:|---:|---:|
| 1t | 3.24–3.36 | 5.55 | **3.50** | **1.08×** |
| 4t | 17.59–21.66 | 25.43 | **24.63** | 1.40× |
| 16t | 21.16–22.26 | 26.48 | **27.19** | 1.29× |

1t 明确改善（跨 10+ 轮稳定 3.4–3.9）；4t/16t 受宿主编译负载干扰大
（同一二进制 21.4–28.1 ns/op），不下强结论。

**根因**：`Lock` 是 PImpl 间接（`mov (%rbx),%rax; movzbl 0x40(%rax)` 两次依赖 load）
+ 跨 TU 调用（`DetectHeldParkEnabled()` 带 function-local static guard，无法内联）
+ 大函数帧（`push×6 + sub $0x108` + 栈保护，快路径付完整 prologue/epilogue）
+ 调用方每次构造空 `shared_ptr<Context>` 并走 PLT + 慢路径前移
（CAS 失败先做 `CurrentTarget()`，三次 PLT 且 `current_task` 还拷贝 `shared_ptr`）。

**手段**（`include/go2cpp/sync.hpp`、`src/concurrency/sync.cpp`）

- 状态字内联到对象：`alignas(64) std::atomic<std::uint8_t> m_state` 放对象起始
  （CAS 地址就是 `this`，少一条 `lea`），`m_impl` 只服务慢路径。
- 快路径内联到头文件：一次 relaxed load + 一次 CAS，没有函数调用、没有栈帧；
  慢路径落 `LockImpl`（`sync.hpp:266`）/`UnlockSlow`，语义不变；保留带 context 的
  重载与 `GO2CPP_DEBUG_RESOURCES` 构建的完整路径。
- 持锁 park 检测开关内联：`g_held_park_detect` 原子读 + 可预测分支，去掉跨函数调用。
- 普通线程不再付 `CurrentTarget()` 的 `shared_ptr` 拷贝（scheduler 为空时直接返回）。
- 先 load 再 CAS 而不是先 CAS(0)：4t/16t 下「锁空闲但状态带 kWaiter/kWoken」是常态，
  先 CAS(0) 必失败，等于每次多争用一次缓存行（4t 27.5→24.6，代价 1t 3.45→3.67）。

**否决方案**：队列锁换 `FutexMutex`（收益 <1 ns）、正常模式 Unlock 直接交接
（4t 挂起 >10 min，回滚）、自旋预算 0/32/64（均劣于 8）。

### 2.8 WaitGroup

**结论**

| 场景 | Go | 改前 | 改后 | 提速 | vs Go |
|---|---:|---:|---:|---:|---:|
| Add/Done 1t | 5.31 | 5.55 | **3.77** | 1.47× | **0.71×** |
| Add/Done 4t | 12.28 | 25.41 | **9.49** | 2.68× | **0.77×** |
| Add/Done 16t | 19.42 | 63.10 | **19.64** | 3.21× | **1.01×** |
| Done→Wait 唤醒延迟 | 10 925 | 35 464 | 35 700 | 0.99× | 3.27× |

前两项与 16t 达标；唤醒延迟未达标，且**主体是内核**：其中 `FUTEX_WAKE` 一次
19–20 µs（`wg.done.cost` 实测），任何用户态改动都动不了。

**根因**：Add/Done 在无等待者时仍然取互斥量 + 构造容器遍历。
**手段**（`src/concurrency/sync.cpp`）：计数改原子；无等待者时整条锁路径消失
（`Add` `sync.cpp:2462`、`Done` `:2515`、`Wait` `:2549`）；`Done` 到 0 才走
唤醒路径；`Wait` 的等待节点复用 WaitNode 池。

### 2.9 RWMutex

**结论**（ns/op，同 harness 同日）

| 场景 | 1t | 4t | 8t | 16t |
|---|---:|---:|---:|---:|
| 纯读 | 3.70 | 19.15 | 23.39 | 20.65 |
| 混合 90% 读 / 10% 写 | **5.88** | **32.87** | 46.91 | 87.31 |
| 纯写 | 28.08 | 565.35 | 2663.88 | 42655.23 |

对比 HEAD 基线：混合 4t 304.13→32.87（9.3×）、8t 61.02→46.91（1.30×）、
16t 569.32→87.31（6.5×）；纯写 4t 3753→565（6.6×）、8t 165218→2664（62×）、
16t 380928→42655（8.9×）。

vs Go（`go_vs_bento` 的 4R1W 4 线程 46.24 ns）：**混合 4t 32.87 → 0.71×（反超）**。
若按 1t 无争用混合 5.88 ns 与 Go 的 46.24 ns 粗算，得到 0.13×；**两者口径不同
（1t vs 4 线程），0.13× 只作量级参考，达标判定用 0.71×**。

**根因**：写者用 ticket + `ConditionVariable` park 排队，每次交接都要 futex 唤醒
（源码注释自述一次唤醒往返约 2.3 µs），4 线程写争用 386 ns/op。

**手段**（`include/go2cpp/concurrency/rwmutex.hpp`）：读者门原子计数
（`m_reader_count` 整体偏移 `-kReaderGate` 表示门关，`rwmutex.hpp:151` 起 `RLock`）、
写者交接令牌（`Unlock` 直接把手里的写权交给队首写者，避免「释放→唤醒→重新竞争」）、
精确 `NotifyOne`（只唤醒一个等待者，不广播）。半成品接手时修了 4 个问题
（门计数还原、写者优先死锁窗口、取消清理、交接令牌丢失）。

**未达标**：纯写 4/8/16 线程仍 565/2664/42655 ns（Go 19.39 ns @4t，约 29×），
见 §4.3。

### 2.10 ConditionVariable

**结论**：`sync.Cond` wait/notify 1P1C **448.55 → 231.39 ns（1.94×）**，
Go 100.23 ns → **2.31×，未达标**。空 notify（无等待者）5.29 → 2.26 ns（−57%，
该行来自同日 sync 二轮，与 1P1C 不是同一轮测量）。

**根因（关键判断）**：不是实现浪费，而是**线程模型差异**。Go 的 1P1C 里两个
goroutine 跑在同一个 P（同一 CPU 核），锁交接是 L1 命中；本基准是两个 OS 线程
跨核交替，每次交接付一次缓存行所有权迁移（40–60 ns），且唤醒路径下限是
WSL2 的 futex 唤醒（20–30 µs，历史上测到 23.7 µs）。

**手段**（`src/concurrency/sync.cpp`）：`Wait` 去掉返回后注定失败的查找
（`sync.cpp:2109`）；`NotifyOne`（`:2261`）/`NotifyAll`（`:2304`）无等待者时
不取队列锁，只有真的有等待者才走交接；等待节点复用 WaitNode 池；
锁内只认领、锁外唤醒。

### 2.11 Context

**结论**

| 指标（median，ns/op） | 优化前 | 优化后 | 变化 | vs Go |
|---|---:|---:|---:|---:|
| `IsDone()` 未取消 | 5.68 | **0.42** | −92.6%（13.5×） | Go 6.24 → **0.07×** |
| `Done().IsDone()` | 5.15 | **0.42** | −91.8% | — |
| `Deadline()` 有 deadline | 5.14 | **0.84** | −83.7% | Go 1.89 → 2.25× 快 |
| `HasDeadline()` | 6.07 | **0.41** | −93.2% | — |
| `Done().Wait()` 已取消 | 15.06 | **2.07** | −86.3% | — |
| `AddCallback+RemoveCallback` | 26.10 | **16.65** | −36.2% | — |
| 取消 + 64 个 Done 回调 | 2427 | **1314** | −45.9% | — |
| `WithCancel+cancel+destroy` | 284.5 | 265.6 | −6.6% | Go 76.0 → 3.5× 慢 |
| Channel cap8 带 live ctx 的额外成本 | +27.0 ns/对 | **−1.1 ns（噪声）** | 归零 | — |

**根因**：`IsDone` 走互斥锁；`Deadline` 每次取锁；取消回调逐个 `make_shared` +
deque 遍历；Context 链每层 5 次堆分配。

**手段**（`include/go2cpp/concurrency/context.hpp`）：`DoneSignal::IsDone` 改
一次 acquire 原子读（`context.hpp:97`，`m_done_flag` 指向 `State::done`，
生命周期由 `m_state` 保证）；Deadline 不可变缓存（构造时算好）；取消回调改
vector 批量搬运；深链取消不再每节点 malloc；`Now()` 缓存。

**未达标**：`WithCancel` 创建 265.6 ns vs Go 76 ns（3.5×），根因是分配模型
（State+控制块、DoneSignal::State、Context+控制块共 5 次分配）与
`WithTimeout` 的专用 timer 线程，属 §4.4。

### 2.12 Future

**结论**

| 指标 | 优化前 | 优化后 | 变化 | vs Go |
|---|---:|---:|---:|---:|
| `GetResult`/`get`（已就绪） | 23.6 ns | **5.7 ns** | 4.1× | — |
| `Status` | 5.2 ns | **0.77 ns** | 6.8× | — |
| `TryGet` / `GetFor(0)` | 58 ns | **2.1 ns** | 28× | — |
| 创建+SetValue | 98 ns | 75.7 ns | 1.30× | — |
| 完整生命周期（创建+Set+Get） | 120.5 ns | **79.1 ns** | 1.52× | Go 82 → **0.96×** |
| 跨线程唤醒 e2e | 41.6 µs | 40.0 µs | 1.04× | 被 OS 调度主导 |

**根因**：终态由互斥锁保护，`Set` 每次都加锁/广播；`Get` 已就绪也要取锁；
Context 以 `shared_ptr` 参与，每次拷贝一次原子引用计数；零超时探测也计算 deadline。

**手段**（`include/go2cpp/concurrency/future.hpp`）：32 位原子字
`m_word`（`future.hpp:588`/`:812`）替代锁保护终态；`Get`/`Status` 已就绪时完全
无锁；`Set` 只在需要唤醒等待者时才走慢路径；Context 改 `const Context*` 参与、
不拷贝 `shared_ptr`；零超时探测不计算 deadline；按终态只拷贝一个 payload
`shared_ptr`。

### 2.13 SpinMutex

**结论**

| 指标 | 优化前 | 优化后 | 变化 |
|---|---:|---:|---:|
| 1 线程 pair | 5.54 / 5.44 ns | **5.09 / 5.12 ns** | −7% |
| 4 线程争用 | 31.63 / 31.20 ns | **18.48 / 19.57 ns** | **−40%** |
| 16 线程争用 | 32.09 / 37.03 ns | **14.26 / 14.94 ns** | **−57%** |

4t 18.48 ns 已明显快于同环境 `sync::Mutex`（24.63 ns），即「4t 反超」。

**根因（报告口径）**：优化前 4 线程争用 31.2 ns/op，与同机 `sync::Mutex` 相当甚至略慢；争用路径没有把「自旋—让出—睡眠」分成三段，高争用时空转与缓存行迁移占主导。
**手段**（`include/go2cpp/concurrency/spin_mutex.hpp`）：移植 Go `lock_futex.c`
的「自旋 + 门」结构——64 次 PAUSE 自旋，拿不到进「门」（复用 `sync::Mutex` 的
FIFO park 队列，`spin_mutex.hpp:29` 设计注释、`:257` 释放只有一次 release store），
门内再自旋 16 次；只读自旋阶段用 TTAS 避免写缓存行。

### 2.14 Scheduler park/wake（20:00 那一轮，P0 之前）

**结论**：pingpong 180.4→166.4 ns（−7.8%）、yield 163.8→155.1（−5.4%）、
channel 322.8→314.4（−2.6%）、CV 532.5→503.4（−5.5%）。
**根因判断**：park+resume 166.4 ns 中 Fiber 切换 31.2 ns（18.8%）、worker 调度链
123.9 ns（74.5%）、park/wake 状态机增量只有 11.3 ns（6.8%）；无缓冲 Channel 一次
阻塞收发 157.2 ns 中 Scheduler+Fiber 约 141 ns（90%）。
**手段**：按 Go `gopark/park0/schedule/execute/gogo` 结构对齐——去掉每次 park/wake
的冗余原子读与一次 Task 锁；lifecycle 位图替代 `shared_ptr` 判定；快校验路径；
IO 批量唤醒。**结论是「剩下的空间不在 park/wake 入口，在 worker 调度链的状态锁」**，
这直接导出了 §2.3 的 P0。

---

## 3. 关键洞察（今天学到的）

### 3.1 Go 的 chan 快，因为语义外置

Go 的 `chansend`/`chanrecv` 签名里没有 context、没有超时、没有取消；
`select { case ch <- v: case <-ctx.Done(): }` 由编译器展开成 `selectgo`，
取消检查发生在 `gopark` 的 `unlockf` 回调里（`selparkcommit` 释放锁并复查），
**不在 channel 的数据结构里**。所以 `hchan` 的热路径只需要处理「值 + 等待者队列」，
不需要为每次 send/recv 注册/注销取消回调、维护 generation、判断 deadline。

对比 Bento：`Channel::Send(ctx, value)` 每次都要在入队前 `AddCancelCallback`、
出队后 `RemoveCallback`，还要在取消回调里按身份定位节点。这不是「实现多写了
几行」，而是**语义位置不同**：Go 把取消放在 select 层，Bento 放在 channel 层。
P0 的取消路径改造（弱引用 hub + 节点身份 + CAS signal）本质上是在
「语义不能外置」的前提下，把外置的那部分成本压到最小。

**推论**：如果未来允许数据面用 `FastChannel`（无 ctx/无 select），那么可以直接
拿到「语义外置」的收益——SPSC cap1024 20.61 ns 已经证明了这一点（0.81× Go）。

### 3.2 Go 的 hchan 结构：单锁 / sudog 池 / 直接交接

三条设计决定，缺一不可：

1. **单锁**：`hchan.lock` 一把锁保护 hchan 全部字段以及阻塞其上的 sudog 的部分
   字段（`chan.go:48-53` 注释）。等待节点没有自己的锁，因为它的 `elem`/`next`/`prev`
   都由 channel 锁保护。Bento 之前每个 `WaitNode` 有独立队列锁 + 唤醒锁，
   正是缺这个前提的结果。
2. **sudog 池**：`p.sudogbuf[128]` + 全局 `sudogcache`，稳态阻塞路径零分配。
   Bento 的 `WaitNodePool`（上限 128）就是照这个做的。
3. **直接交接**：有等待接收者时，`send` 直接把值 memmove 到接收者的 `elem`，
   不经过缓冲区、不 park、不唤醒——`goready` 只是把 G 放回 runq。
   这是 Go 1P1C `cap0` 只有 103.8 ns 的根本原因。

**不变量**：`sendq` 与 `recvq` 至少一个为空；有缓冲时 `qcount>0 ⇒ recvq 空`、
`qcount<dataqsiz ⇒ sendq 空`。这两条让 Go 的 send/recv 只需检查一个队列。
Bento 的 select 是「逐个 arm、逐个复查」，允许并存，所以拿不到这条优化——
这是语义差异，不是 bug。

### 3.3 Fiber 切换的 66 ns 全是簿记

`JumpContext` 裸往返 6.2–6.7 ns，17 条指令；Go 的 gosave+gogo 是 22 条、2 次 TLS。
差异不是「C++ 汇编写得差」，而是：

- **TLS general-dynamic**：每次 `thread_local` 访问 `__tls_get_addr@plt` 5–8 ns，
  一次往返 6–10 次，单项就 30–80 ns。改 initial-exec 后 callgrind 里
  `__tls_get_addr` 从 10.5% 降到 4.9%。
- **`bind_caller` 重复加锁**：每次 resume 都刷新父链，2 次 mutex + `shared_ptr`
  引用计数 + 父记录拷贝，即使父级从没变过。
- **errno noinline**：往返 5 次 noinline 调用。

**教训**：无栈协程的切换成本，主要不在「保存寄存器」，而在「切换前后顺手做的
一切簿记」。优化顺序应该是：先砍 TLS/加锁/虚调用，再谈汇编。

### 3.4 调度链 6 次锁 vs Go 0 锁 1 CAS：runq 所有权协议

Go 的 runq 是「单生产者 + CAS 多消费者」：`runqput` 由 P 的 owner 写 tail
（release store），`runqget` 消费者 CAS head；「同一个 G 只会被一个 M 取走」
由**队列节点的所有权**保证，不需要额外的执行权仲裁。`execute()` 里没有任何锁。

Bento 之前有 4 次 `m_transition_mutex` + 2 次 `processor.mutex` + 2 次
`m_resume_claim` CAS，因为它的 P 没有唯一 owner（replacement M 可共享 P、
外部唤醒会精确投递到目标 P）。P0 的解法不是「删锁」，而是**重建所有权协议**：

- 队列节点所有权 = 执行权：`claim_from_queue()` 单 CAS `Runnable→Running`，
  CAS 赢家独占节点，别人不能再入队、不能再 resume；
- 状态字与唤醒令牌合并成一个 32 位原子字，消除「先改状态再置令牌」的两步窗口；
- `transition_mutex` 只留在 cancel/shutdown/冷路径。

结果：pingpong 166.4→111.2 ns（−33.2%），但相对 Go 仍有差距——因为还剩
`shared_ptr` 引用计数与 Fiber 切换本身（§5 的 P1）。

**教训**：并发数据结构的锁数量，取决于「谁拥有这个对象」能否用一句话说清。
说不清就只能加锁；说清了（哪怕要引入一个新协议），锁就能变成 CAS。

### 3.5 futex 23.7 µs 是物理下限（WSL2）

本机 `FUTEX_WAKE` 一次 19–20 µs（`wg.done.cost` 实测 19.1–20.6 µs），
等待往返历史上测到 23.7 µs。这意味着：

- 任何需要真正 park 到 futex 的路径，单次往返下限就是 20–30 µs；
- `wg.wakeup 35.7 µs`、`Conc 唤醒延迟 3.27× Go` 不是实现问题，是内核问题；
- 唯一出路是**不 park**：自旋窗口内完成交接（WaitNode 新版 p95 从 55.4 µs
  降到 1.15 µs，就是让准备与通知都落在等待者的 18 µs 自旋窗口内），
  或者用无锁 ring 把 park 移出热路径（§5 P2）。

**教训**：在 WSL2 上优化同步原语，先量出「一次 futex 往返」的本地成本，
把它当作预算上限；所有低于它的目标都必须靠「避免 park」实现，而不是
「把 park 写快一点」。

### 3.6 跨核 cacheline 40–60 ns 是物理下限

两个 OS 线程绑两个 SMT 线程时，一次锁行/队列行在两端之间往返约 40–60 ns。
这解释了：

- Channel cap1024 96 ns 里至少 40–60 ns 是缓存行迁移，Go 的 25.5 ns 基准
  根本不包含这部分（同 P goroutine 切换）；
- Mutex 1t 3.50 ns 与 4t 24.63 ns 的差距不是锁实现变差，是缓存行开始迁移；
- RWMutex 混合 4t 32.87 ns 已接近该下限，继续优化单次延迟的空间很小。

**推论**：跨线程原语的「单次延迟」在 2 核绑定下有一个 ~40 ns 的地板；
要突破只能改变执行模型（批量化、同核 handoff、无锁 ring + 批量提交），
而不是继续抠指令数。

### 3.7 PLT 懒绑定 + 4 KB Fiber 栈 = xsave 越界 SIGSEGV

`_dl_runtime_resolve_xsavec` 会把完整 xsave 状态压到**当前栈**（约 2–4 KB，
取决于 CPU 的 xsave 区域大小）。Fiber 初始栈只有 1 页（4 KB），较深的关停调用链
首次解析符号时直接越过栈边界写进 guard page → SIGSEGV。栈回溯固定在
`begin_shutdown_locked → wake_for_scheduler` 的 `do_lookup_x`/`_dl_fixup`。

修复：顶层 `CMakeLists.txt` 加 `add_link_options(-Wl,-z,now)`（UNIX），
启动期一次性绑定全部 PLT。`LD_BIND_NOW=1` 下全绿证实了根因。

**教训**：小栈 + 动态链接 = 隐式栈需求。任何「4 KB 栈」的设计都必须假设
动态链接器会在任意调用点吃掉 2–4 KB；要么立即绑定，要么把栈最小值提到 16 KB。
这类问题不会被单元测试发现，只会在深调用链的关停路径偶发。

---

## 4. 未达标项与根因

### 4.1 ConditionVariable：2.31×

- 数字：448.55→231.39 ns（1.94× 提速），Go 100.23 ns，仍 2.31×。
- 根因：**线程模型**。Go 的 1P1C 两个 goroutine 同 P，锁交接 L1 命中；
  本基准两 OS 线程跨核，每次交接 40–60 ns 缓存行迁移 + 唤醒路径受 futex 下限约束。
- 为什么不再优化：剩下的 131 ns 里，跨核迁移 ~50 ns、futex 唤醒 ~20–30 µs 的
  尾部、Go 侧根本没有的 park 入口 ~30 ns。继续抠只能改基准（同核跑），
  那不是优化。
- 可选方向：把 CV 的等待队列改成「同核自旋优先 + 跨核才 park」的自适应策略
  （类似 Go 的 `sync.Cond` 在 `notify` 时先 `goready` 再看 P 是否空闲），
  预期只能到 1.5–1.8×。

### 4.2 Channel MPMC：3.7–4.8×

- 数字：`FastChannel` cap1024 95.28 ns（Go 25.53，3.73×）、cap64 118.62（4.78×）、
  cap0 814.23（9.15×）；通用 `Channel` cap1024 96.0（3.77×）。
- 根因三条：① 跨核缓存行（40–60 ns）；② park 路径未无锁化，cap0 每条都要
  park/unpark；③ 通用 Channel 仍背 Context/select/调试语义。
- 已证明的出路：SPSC 无锁 ring 20.61 ns（0.81× Go）——**同一台机器、同一口径**，
  说明差距不是「C++ 不行」，而是「MPMC + park 语义」的成本。
- 未解决：MPMC 的无锁化需要「批量 + 赢家 CAS」或 Vyukov 队列，且要保留
  close/select/等待者语义，风险高于当日边界。

### 4.3 RWMutex 写路径：29×

- 数字：纯写 4t 565.35 ns（Go 19.39，29×）、8t 2663.88、16t 42655.23。
- 根因：写者之间是「ticket 排队 + CV park」，每次交接一次 futex 唤醒
  （~2.3 µs 往返的老路径），16t 时排队长度把延迟推到 42 µs。
- 已改善：相对基线 3753/165218/380928 已降 6.6×/62×/8.9×，1t 纯写 28.08 ns
  优于基线 38.13；写者交接令牌 + 精确 Notify 已经生效。
- 未解决：写-写交接仍是「释放 → 唤醒 → 重新竞争」，没有做成 Go 那种
  「直接把所有权交给队首」的无锁 handoff（Mutex 上试过 handoff，4t 挂起，已回滚）。
- 方向：写者队列改侵入式 + 队首 CAS 直接交接，唤醒只在「队首是 park 线程」时发生；
  需要先有 Mutex 的可靠 handoff 协议。

### 4.4 Context 创建：3.5×

- 数字：`WithCancel+cancel+destroy` 265.6 ns（Go 76.0）。
- 根因：每层 5 次堆分配（State+控制块、DoneSignal::State、Context+控制块），
  `WithTimeout` 还有专用 timer 线程 + multimap。
- 未解决原因：合并分配会改 `State`/`DoneSignal` 的所有权结构，超出「保持语义」
  的边界。
- 预期：单次分配 + 对象池可把 266→~100 ns、813→~400 ns（估算，未验证）。

### 4.5 WaitGroup Done→Wait 唤醒：3.27×

- 35.7 µs，其中 futex 唤醒 19–20 µs 是内核成本，无法优化；
- 已做：Done 到 0 才唤醒、等待节点池化；
- 结论：这是「WSL2 物理下限」类未达标，不建议继续投入。

### 4.6 调度链目标 80–100 ns 未达

- 实际 111.2 ns（目标 80–100）。剩余成本不在锁（HotSpinLock 无争用已 ~10 ns），
  而在 `shared_ptr` 引用计数（每次 push/pop 一次 `lock xadd`/`lock dec`）、
  `ParkingCondition` 的 wait/notify 路径、Fiber 切换本身（31.3 ns）。
- 对应 §5 的 P1-1（shared_ptr 消除）与 P1-3（runnext）。

---

## 5. 后续路线（P1 / P2）

### 5.1 P1：去掉堆分配与引用计数（预计 2–3 天）

1. **shared_ptr 消除**（收益最大，风险最高）
   - 现状：`std::deque<shared_ptr<Task>>` 的每次 push/pop 一次原子增/减；
     `Channel` 等待节点、`Future` payload 同样。
   - 改法：队列元素改侵入式 `Task*` 链表节点，生命周期由 registry 保活
     （任务在 registry 中时不会被析构）；`shared_ptr` 只留在 API 边界。
   - 预期：调度链 −10~15 ns；Channel 阻塞路径 −5~10 ns。
   - 风险：UAF。必须 ASan + 长稳压测 + registry 引用计数审计。
2. **runnext 快槽**（Go 1.11 的 `proc.go:7529`）
   - 每个 P 一个 runnext 槽：`ready` 若目标是「刚被 park 的 G」直接放 runnext，
     下次 `runqget` 优先取。
   - 预期：ping-pong 场景 −5~10 ns（正好是当前 pingpong 的主要形态）。
   - 风险低，可独立开关。
3. **栈池补全**（arena 已完成，剩固定档 free-list）
   - 现状：arena 按 `(reserve, usable)` 分 8 类，已覆盖主流尺寸；
     剩余是「>64 KiB 走 mmap + growable」的冷路径。
   - 改法：增加 8/16/32/64 KiB 固定档 per-thread free-list（纯指针数组，
     0 系统调用），仅大栈走 arena/growable。
   - 预期：`go()` 从 14.5 µs 降到 1–2 µs（部分已被 FiberBin 吃掉）。
4. **Context 单次分配 + 池化**（§4.4）
5. **RWMutex 写者队列侵入式 + 队首直接交接**（§4.3）

### 5.2 P2：多核扩展性与数据面（预计 1–2 周）

1. **无锁 ring 数据面**（`FastSpscChannel` 已验证 0.81× Go）
   - 推广到 MPSC/SPMC：单生产者单消费者用现成 SPSC；多生产者用「每生产者
     一个槽位 + 原子序号」或 Vyukov MPMC；
   - 批量 API（`SendBatch`/`RecvBatch`）把缓存行迁移摊到 N 条消息。
   - 目标：MPMC cap1024 ≤35 ns（当前 95–118 ns），前提是接受「无 select/无 ctx」。
2. **select 批量加锁 + 赢家 CAS**：Go 的 `selectgo` 一次加锁全部 channel、
   随机轮询、赢家一次 CAS；Bento 现在是逐个 arm/复查。
3. **工作窃取**（`runqsteal`）：随机选 P、偷一半、自己留一个先跑；
   多核吞吐 +10–20%（非单次延迟）。
4. **netpoll 前移**：把非阻塞 poll 挪进 worker 的查找循环，减少一次跨线程唤醒
   （−5~15 ns，IO 密集场景）。
5. **自旋配额**：`2*spinning >= nproc - nidle` 判定，省 CPU 而非延迟。

### 5.3 验收门槛（每一步都必须过）

- 微基准：`JumpContext` 裸往返不退化（6.2–6.7 ns）、resume 单次、park/wake 往返
  各 1e7 次取 P50/P99；
- 真实场景 ≥5 个：cap0 生产者-消费者、cap64 流水线、mutex 竞争、IO 密集、
  深递归（栈增长）；
- 正确性：ASan + 1e9 次随机 park/wake/cancel 交错 + 60 分钟长稳；
- 防退化：任一场景 P99 退化 >5% 即回滚该步。

---

## 6. 附录

### 6.1 今日 perf 提交（13 个）

| commit | 内容 |
|---|---|
| `1ddae4e` | Fiber 切换优化（TLS initial-exec/bind_caller 快路径）72.9→34.4 ns |
| `023eeb8` | Fiber 栈 arena/几何增长/FiberBin 复用——完成 −66%、VmSize −87%、mprotect 133→21 |
| `134c7a3` | 调度链 P0（状态字单 CAS/256 槽 MPMC 环/dequeue 即 claim）pingpong −33.2%/channel −31.4% |
| `d68c428` | WaitNode/Parking（TLS 池移交回收站/节点池/锁外唤醒）rtt 19.7×、TSan 0 race |
| `3a49b66` | Channel P0（等待队列侵入式/节点池/自旋锁/取消 CAS）cap1024 96 ns |
| `6ce03fa` | FastChannel/FastSpscChannel（SPSC cap1024 0.81× Go） |
| `cb5dafa` | Mutex 去间接（状态字内联/快路径内联头文件）1t 1.08× Go |
| `bb2c928` | sync 二轮（WaitGroup −87.7%/CV notify −57%/Mutex 16t −4.8%） |
| `3f0a1c4` | RWMutex 二期（读者门原子/写者交接令牌/精确 Notify） |
| `8933728` | Context（IsDone 0.42 ns/取消回调 vector/Deadline 缓存） |
| `888af8f` | Future（终态原子字/零锁快路径）全生命周期 79.1 ns 优于 Go 82 ns |
| `480a5b2` | SpinMutex（只读自旋/单 CAS/退避）4t 18.5 ns/16t 14.3 ns |
| `d933c0d` | Scheduler park/wake（lifecycle 位图/去 shared_ptr/快校验）channel −2.6% |

配套：`74c11a1`（`-Wl,-z,now` 修 PLT 懒绑定 SIGSEGV）、`9da1155`（混合模型亿级压测）、
`7595baa`（Worker 调度链独立回归 7 场景）、`fe6e379`（测试有效性修复）。

### 6.2 数据来源

| 主题 | 报告 | 位置 |
|---|---|---|
| Go 对照基线 | `go_vs_bento.md` | `/tmp`（临时，未入库） |
| Fiber 切换 | `fiber_switch_opt2.md` | `/tmp` |
| Fiber 栈/创建 | `fiber_opt_full.md` | `/tmp` |
| 调度链 P0 | `sched_p0.md` | `/tmp` |
| park/wake | `scheduler_park_opt2.md` | `/tmp` |
| WaitNode | `waitnode_opt2.md` | `/tmp` |
| Channel P0 | `channel_p0v2.md` | `/tmp` |
| FastChannel | `fast_channel.md` | `/tmp` |
| Mutex | `mutex_opt3.md` | `/tmp` |
| sync 二轮 | `sync_opt2.md` | `/tmp` |
| Cond/WG | `cond_wg_opt.md` | `/tmp` |
| RWMutex | `rwmutex_opt2.md` | `/tmp` |
| Context | `context_opt.md` | `/tmp` |
| Future | `future_opt.md` | `/tmp` |
| SpinMutex | `spin_mutex_opt.md` | `/tmp` |
| 亿级压测 | `mixed_billion.md` | `/tmp` |
| 设计分析 | `docs/research/go-scheduler-fiber-analysis.md` | 仓库 |
| Channel 调研 | `docs/research/go-channel-opt.md` | 仓库 |
| Worker 链设计 | `docs/research/worker-chain-design.md` | 仓库 |
| Go 运行时源码 | `docs/research/go-runtime-c-source.md` | 仓库 |

### 6.3 复现入口

- 基准源码：`tests/concurrency/bench_{sync_primitives,channel_opt,fiber_switch_opt,park_wake_opt,context_opt,future_opt,spin_mutex_opt,rwmutex_opt,fast_channel,scheduler_park}.cpp`
- Go 对照：`tests/concurrency/go_channel_bench.go`、`tests/concurrency/bench_future_go/`
- 构建：`.build-bento-wsl`（Release）；`cmake --build . -j` 后直接运行对应可执行
- 典型命令：`taskset -c 2,3 ./go2cpp_fast_channel_bench`、
  `GO2CPP_TEST_FILTER=fiber ./go2cpp_tests`
- 正确性：`run_tsan.sh`（本机 TSan 不可用）、`run_random_5rounds.sh`、
  `stress_mixed_billion`

### 6.4 术语

- **P/M/G**：处理器（逻辑调度单元）/ 工作线程 / 协程任务，沿用 Go 的 GMP 术语
- **park/unpark**：把当前执行体挂起/唤醒，对应 futex 睡眠与唤醒
- **handoff**：把执行权或数据所有权直接交给对方，不经过队列与唤醒
- **claim**：取得某个任务的执行权（CAS 成功即独占）
- **runq**：每个 P 的本地就绪队列
- **1P1C / 1P2M**：1 生产者 1 消费者 / 1 生产者 2 个 M
