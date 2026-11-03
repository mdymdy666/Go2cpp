# 并发原语 vs 工业库对比矩阵（Go / coost / libco / folly / Boost / TBB）

- 工作树：`/UserData/CodexWorkSpace/Go2cpp`（WSL2 ext4）
- 对标规范：`docs/engineering-standards.md` §一「实现后必须对比」、§四「工业库参考清单」
- 日期：2026-11-01（数据采集时间见各行出处）
- 数据来源：仓内实测报告 + 工业库官方公开数据（标注 URL）
- 状态：**只写本文档，不改代码、不 commit**

---

## 0. 结论速览

| 原语 | 我们 | 最强对手（同机实测） | 判定 |
|---|---:|---:|---|
| 裸上下文切换（ns/往返） | 6.43 | coost 2.79 / libco 24.49 / Boost 8.46 | 第二，输 coost 2.30× |
| Fiber 全量切换（ns/往返） | 33.80（后续快照 23.5） | libco 24.49 / libfiber 53.38 | 第二，输 libco 1.38× |
| Channel 1P1C cap1024（ns/条） | 10.73 | Go 23.36 / muduo 32.40 / TBB 79.13 | **第一**，赢 Go 2.18× |
| Channel 单线程对（ns/对） | 1.43（SPSC）/ 4.63（MPMC） | Go 18.56 | **第一**，赢 Go 13.0× |
| Mutex 无竞争（ns/对） | 3.75 | Go 3.24 / std 4.54 | 持平（1.16× 慢） |
| Mutex 4 线程（ns/对） | 12.18 | Go 11.42 | 持平（1.07×） |
| ConditionVariable 热路径（ns/往返） | 287.20 | Go 174.85 / std 57 882 | 输 Go 1.64×，赢 std 201× |
| ConditionVariable 冷唤醒（ns） | 620 | std 832 | 持平（噪声内） |
| WaitGroup Add/Done 1t/4t/16t（ns） | 3.77 / 9.49 / 19.64 | Go 5.31 / 12.28 / 19.42 | **1t、4t 赢**，16t 持平 |
| Context `IsDone`（ns） | 0.42 | Go 6.24 | **赢 14.9×** |
| Context `WithCancel` 创建（ns） | 265.6 | Go 76.0 | **输 3.5×** |
| 调度器任务（ns/任务） | 685.03（后续快照 326.7） | Go 135.49 | **输 5.06×（最大短板）** |

> ⚠️ **2026-11-03 复核**：Go 135.49 ns 是 **min** 统计（median 185），`bench_go.go` 源码已丢失，口径不可考；5.06× 仅作历史对照。本机重测见 `docs/metrics-ledger.md` M16（Go@4 151–163 ns）。

**一句话**：数据面（Channel / 裸切换 / Fiber 切换 / Mutex 快路径 / WG / Context 读路径）已达或超过工业库；
控制面（调度器任务生命周期、CV 热路径、Context 创建）落后 Go 一档到一档半。

---

## 1. 口径与不可比清单（先摆前提，再谈倍数）

| # | 前提 | 影响 |
|---|---|---|
| 1 | 机器：AMD Ryzen 9 8945HX，WSL2（12 vCPU / SMT），`taskset -c 2,3` 绑同一物理核的两个 SMT 兄弟 | 跨核缓存行迁移固定成常量；绝对值不可外推到裸机 |
| 2 | 轮次：每个二进制内部 5 轮 × 整组 2 次（channel 4 次），取 **min**；同时给 median | min 代表「无宿主干扰下界」，不是平均值 |
| 3 | Go 数字全部来自**同机同时段**独立 `main` 程序（不用 `go test -bench`，框架自带 20–50 ns 开销） | 不用文献典型值替代测量 |
| 4 | Go 的 channel 基准两个 goroutine 在 `GOMAXPROCS=2` 下常驻同一 P，`gopark/goready` 是**用户态状态切换**；我们是两个 OS 线程 | Go 的 23 ns 与我们的 futex 唤醒不能直接相减 |
| 5 | Fiber 33.80 ns 含调度器语义（claim CAS、状态原子、TLS errno 保存），`JumpContext` 6.43 ns 是裸换栈 | 两者不可相减，本文不这么做 |
| 6 | `std::condition_variable` 57.9 µs 是 **WSL2 的 futex 停泊成本**（单次 wait→wake ≈29 µs），裸机会低一个量级 | 该行只用于「同机对照」，不作能力结论 |
| 7 | TBB `spin_mutex` 是纯自旋、无阻塞等待者 | 4 线程超订时靠烧 CPU 换延迟，不代表「更好」 |
| 8 | 并发数据有两/三个快照（见 §9），**调度器数字口径不一致** | 已单列「待补」，不合并成一条曲线 |

---

## 2. Fiber / 上下文切换

同机实测（`bench_ctx`，1e6 次往返，5 轮 min，来源 `docs/reports/2026-11-01-concurrency-survey.md` §2）：

| 实现 | 机制 | ns/往返 | 折算单次 | vs 我们 |
|---|---|---:|---:|---:|
| coost `tb_context_jump` | 裸 asm，无调度 | **2.79** | 1.4 | 我们慢 2.30× |
| **Go2Cpp `JumpContext`** | 裸 asm，17 条指令 | **6.43** | 3.2 | 基准 |
| Boost.Context `fcontext` | 裸 asm + 栈检查 | 8.46 | 4.2 | 我们快 1.32× |
| Boost.Coroutine2 | fcontext + 协程对象 | 8.68 | 4.3 | 我们快 1.35× |
| libco `co_resume/co_yield` | asm + 协程簿记 | 24.49 | 12.2 | 我们快 3.81×（裸层） |
| **Go2Cpp `Fiber::resume`** | asm + 调度器簿记 | **33.80** | 16.9 | 基准（带语义） |
| libfiber `ready/yield` | 调度器 + epoll | 53.38 | 26.7 | 我们快 1.58× |
| Boost.Asio `spawn+post` | 协程 + io_context 队列 | 166.60 | 83.3 | 我们快 4.93× |
| Go goroutine 经 chan 往返 | runtime park/unpark | 171.57 | 85.8 | 我们快 5.08× |
| `std::condition_variable` 线程交棒 | futex（WSL2） | 59 082 | 29 543 | 我们快 1748× |
| `std::thread` 创建 + join | clone/join | 84 439 | — | 不同层 |

**结论**：裸换栈我们是第二（只输 coost 的极薄 asm，2.30×）；带调度语义的切换我们同样是第二（输 libco 1.38×，
但 libco 是单线程非抢占协程、无跨线程调度/取消，语义更薄）。对 libfiber / Asio / Go 的对应路径我们是 1.58–5.08× 领先。

**工业库公开数据对照（folly::fibers / libco / coost）**：

| 库 | 官方公开数据 | 可核验性 | 结论 |
|---|---|---|---|
| libco（Tencent） | 官方 README 只给定性：「微信服务数万台机器自 2013 年运行」「copy-stack 模式支撑数千万 TCP 连接」，**未给 ns 级切换数字** | 已取到（`raw.githubusercontent.com/Tencent/libco/master/README.md`） | 公开无 ns 数据；以本文同机实测 24.49 ns 为准 |
| coost（idealvin） | 官方仓库文档本轮抓取失败（404，仓库路径迁移），**未取到 ns 级公开数字** | 未取到 | 以本文同机实测 2.79 ns（裸跳）为准 |
| folly::fibers（Meta） | 官方 README 只有设计说明（FiberManager 非线程安全、每线程一个），**无切换延迟公开数字** | 已取到（`raw.githubusercontent.com/facebook/folly/main/folly/fibers/README.md`） | **未测/待补**：需本机编译 folly 后同口径测量 |
| Boost.Context | 无官方 ns 数字 | — | 以本文同机实测 8.46 ns 为准 |

> 结论：**folly::fibers 缺我方同机实测，标「待补」**；libco / coost 用同机实测值，不用二手转载值。

---

## 3. Channel 1P1C（cap1024，跨线程）

来源：`docs/reports/2026-11-01-concurrency-survey.md` §3（`bench_channel`，200k 条，4 组取 min）。

| 实现 | 语义 | ns/条 | median | vs 我们 |
|---|---|---:|---:|---:|
| **Go2Cpp `FastChannel<1024>`** | MPMC 无锁 ring（Vyukov） | **10.73** | 12.83 | 基准 |
| **Go2Cpp `FastSpscChannel<1024>`** | SPSC 无锁 ring | **14.54** | 18.04 | 慢于 MPMC 版（见下） |
| Go `chan uint64` cap1024 | runtime hchan | 23.36 | 28.68 | 我们快 2.18× |
| Go `chan uint64` cap64 | runtime hchan | 21.64 | 25.37 | 我们快 2.02× |
| Go `chan uint64` cap0 | 直接交接 | 86.80 | 94.12 | 我们（cap1024）快 8.09× |
| muduo `BlockingQueue`（无界） | mutex + deque + CV | 32.40 | 38.45 | 我们快 3.02× |
| muduo `BoundedBlockingQueue(1024)` | mutex + 环形 + CV | 71.16 | 75.61 | 我们快 6.63× |
| TBB `concurrent_bounded_queue(1024)` | 有界阻塞队列 | 79.13 | 86.88 | 我们快 7.38× |

单线程快路径（同一线程 TrySend/TryRecv 一对）：

| 实现 | ns/对 | vs 我们 |
|---|---:|---:|
| Go2Cpp `FastSpscChannel` | **1.43** | — |
| Go2Cpp `FastChannel` | **4.63** | — |
| Go `chan` cap1024（send+recv） | 18.56 | 我们快 4.01×（对 MPMC 版） |

**后续内部快照（不同口径，勿与上表混用）**：`go2cpp_fast_channel_bench` 实测
fast-cap1024（Fiber 模式）median **7.79 ns/op**、min 7.15；fast-cap64（Fiber）14.33；fast-try-1T 5.06
（来源 `docs/reports/2026-11-01-concurrency-opt-review.md` §0）。这是同线程 Fiber 交接口径，
**不是** §3 的跨线程 1P1C，两者不可相减。

**结论**：1P1C 队列我们**第一**：赢 Go 2.18×、muduo 3.02×、TBB 7.38×。
原因：稳态下生产者只推进 `enqueue_pos`、消费者只推进 `dequeue_pos`，每槽 `seq` 交接，
零锁、零 syscall、零分配；Go 走 runtime 锁 + `gopark/goready`，muduo 每对 put/take 两次 mutex + CV。

**MPMC 反而比 SPSC 快 26%**（10.73 vs 14.54）：SPSC 每步要读对端 `m_head`/`m_tail`（跨核缓存行），
且每次无条件查等待槽；Vyukov ring 各自只碰自己那侧序号 + 槽 `seq`，跨核流量更省。

---

## 4. Mutex / SpinMutex

来源：survey §4（`bench_mutex`，1e7 次 lock+unlock，5 轮 min）。

| 实现 | 1t ns/对 | 1t median | 4t ns/对 | 4t median |
|---|---:|---:|---:|---:|
| TBB `spin_mutex` | **1.66** | 1.73 | **8.78** | 9.39 |
| Go `sync.Mutex` | 3.24 | 3.36 | 11.42 | 12.40 |
| **Go2Cpp `Mutex`** | **3.75** | 3.86 | **12.18** | 12.77 |
| Go2Cpp `SpinMutex` | 3.58 | 3.62 | 8.32 | 17.79 |
| `std::mutex` | 4.54 | 4.72 | 8.53 | 13.56 |

**结论**：无竞争 3.75 ns 与 Go 3.24 ns 同级（1.16× 慢），优于 `std::mutex` 4.54（0.83×）；
4 线程与 Go 11.42 在噪声内同级（1.07×）。TBB 1.66/8.78 是纯自旋下限，没有等待者队列与公平性，
不构成「更好」。

---

## 5. ConditionVariable（1P1C）

来源：survey §5（`bench_cond`）。

| 指标 | Go2Cpp `ConditionVariable` | Go `sync.Cond` | `std::condition_variable` |
|---|---:|---:|---:|
| 热路径 ping-pong（ns/往返） | **287.20** | **174.85** | 57 882 |
| 冷路径 notify→唤醒（ns） | 620 | — | 832（min）/1 524 |
| 无等待者 notify（ns） | 2.26 | — | — |

**结论**：热路径**输 Go 1.64×**（唯一明显落后于 Go 的原语），但赢 `std::condition_variable` 201×。
优势全部来自「先自旋、避免停泊」（原子槽 + 2048 次 pause 预算），不是 futex 更快：
冷唤醒我们 620 ns 与 std 832 ns 在噪声内同级。长等待（超自旋预算）场景不应期待 200× 优势。

---

## 6. WaitGroup

来源：`docs/reports/2026-10-30-performance.md` §2.8（同 harness 同日，C++ 5 轮 min / Go 3 轮 min 串行交替）。

| 场景 | Go | 我们（优化后） | vs Go | 判定 |
|---|---:|---:|---:|---|
| Add/Done 1t | 5.31 | **3.77** | 0.71× | **赢 1.41×** |
| Add/Done 4t | 12.28 | **9.49** | 0.77× | **赢 1.29×** |
| Add/Done 16t | 19.42 | 19.64 | 1.01× | 持平 |
| Done→Wait 唤醒延迟 | 10 925 | 35 700 | 3.27× | **输 3.27×（主因内核）** |

**结论**：计数路径（Add/Done）1t、4t 反超 Go，16t 持平。唤醒延迟 3.27× 的**主体是内核**：
单次 `FUTEX_WAKE` 在 WSL2 实测 19–20 µs，任何用户态改动都动不了它——这一项不改架构就无法达标。

---

## 7. Context

来源：`docs/reports/2026-10-30-performance.md` §2.11。

| 指标（median，ns/op） | 优化前 | 优化后 | vs Go | 判定 |
|---|---:|---:|---:|---|
| `IsDone()` 未取消 | 5.68 | **0.42** | Go 6.24 | **赢 14.9×** |
| `Done().IsDone()` | 5.15 | **0.42** | — | — |
| `Deadline()` 有 deadline | 5.14 | **0.84** | Go 1.89 | **赢 2.25×** |
| `HasDeadline()` | 6.07 | **0.41** | — | — |
| `Done().Wait()` 已取消 | 15.06 | **2.07** | — | — |
| `AddCallback+RemoveCallback` | 26.10 | **16.65** | — | — |
| 取消 + 64 个 Done 回调 | 2427 | **1314** | — | — |
| `WithCancel+cancel+destroy` | 284.5 | 265.6 | Go 76.0 | **输 3.5×（待补）** |
| Channel cap8 带 live ctx 的额外成本 | +27.0 | **−1.1（噪声）** | — | 归零 |

**结论**：读路径（IsDone/Deadline）碾压 Go（14.9× / 2.25×），因为改成一次 acquire 原子读 + 不可变缓存；
**创建路径输 3.5×**，根因是分配模型（State + DoneSignal::State + Context 控制块共 5 次堆分配）
与 WithTimeout 的专用 timer 线程。

**工业库对照**：coost / libco / folly **无 Context 等价物**（无取消传播语义），
因此本行只有 Go 一个对手，不存在「三库横评」——这一点在 §11 待补里注明。

---

## 8. 调度器任务（控制面短板）

| 指标 | Go2Cpp | Go | 差距 | 出处 |
|---|---:|---:|---:|---|
| `Scheduler.spawn+run` 全生命周期（ns/任务） | 685.03（median 874） | 135.49（median 185） | **5.06×** | survey §6 |
| 后续优化快照（ns/任务） | 326.7（−32.9%） | 135 | 2.41× | `2026-11-01-plan.md` §四 |
| 汇编审查口径（ns/任务） | 656–764 | 135 | 4.2–5.7× | `2026-11-01-asm-concurrency-review.md` §0 |
| Task 尺寸 | 456 B（阶段 2+3 后） | goroutine ≈2 KB 初始栈 | — | `AI_COLLAB.md` / commit `24c3504` |

> **口径警告**：三个数字来自三个不同快照与三种「任务」定义（是否含入队、是否含 `wait_for` 完成通知），
> **不可画成一条下降曲线**。这一点列入 §11「待补/需统一口径」。
>
> ⚠️ 另（2026-11-03 复核）：Go 135.49 ns 为 **min** 统计（median 185），且 `bench_go.go` 源码已丢失 → 口径不可考；本机 Go@4 基线见 `docs/metrics-ledger.md` M16（151–163 ns），C++ 同轮中位见 M03（239 ns）。

**结论**：调度器是唯一「明显没到极速」的项——数据面已到位（Channel 赢 Go 2.18×、Fiber 23.5 ns 近物理下限 21 ns），
但任务创建仍 2.4–5× Go。汇编审查给出的根因不是换栈汇编（只占 6.4 ns），而是
「每个任务付一次堆分配、5 次引用计数、2 套同步原语、148 个函数的栈保护」。

---

## 9. 差距分析（为什么输、为什么赢）

| 项 | 我们赢/输的机制根因 |
|---|---|
| 裸换栈 6.43 vs coost 2.79 | coost asm 更薄：不保存 errno/TLS、无栈边界检查；我们 17 条指令含 6 个 callee-saved push + `mov rsp` |
| Fiber 33.80 vs libco 24.49 | 我们多了 resume claim CAS、Fiber 状态原子、TLS errno 保存/恢复、`bind_caller` 快路径；libco 无跨线程调度/取消 |
| Channel 10.73 vs Go 23.36 | 我们零锁零 syscall 零分配（Vyukov ring + 每槽 seq）；Go 走 runtime 锁 + gopark/goready |
| Mutex 3.75 vs Go 3.24 | Go 快路径只有一条 CAS + 少量簿记；我们多一次「持锁 park 检测」的原子读（可预测分支） |
| CV 287 vs Go 175 | Go 用用户态 park/unpark（同 P 内状态切换）；我们是原子槽 + 自旋观察，跨 OS 线程付缓存行迁移 |
| WG 唤醒 35.7 µs vs Go 10.9 µs | 我们走 `FUTEX_WAKE`（单次 19–20 µs，WSL2 内核成本）；Go 在用户态链上直接 goready |
| ctx 创建 265.6 vs 76 | 我们 5 次堆分配 + WithTimeout 专用 timer 线程；Go 一个结构体 + 定时器堆 |
| 调度器 685 vs 135（历史 min，见 ledger M16） | `shared_ptr<Task>` + 独立 Fiber + 入队/出队 + `wait_for` 完成通知；Go 2 KB 栈 + P 本地缓存 + freelist |

**共性结论**：
1. **数据面赢在「零同步 / 零分配」**——凡是稳态可以纯原子推进的路径，我们都到或超过工业库；
2. **控制面输在「对象生命周期」**——凡是每次操作要构造/析构带引用计数的对象（Task、Context State、CV 等待节点），
   我们就落后一档到一档半；
3. **跨 OS 线程的阻塞唤醒是物理墙**——WSL2 的 futex 往返 19–29 µs，这不是实现问题，是内核 + 虚拟化成本。

---

## 10. 改进方向（按 收益 ÷ 难度 排序）

| 优先级 | 动作 | 预期收益 | 依据 |
|---|---|---|---|
| **P0** | Task/Fiber 对象池 + 完成通知批量化（`shared_ptr<Task>` 改侵入式引用/池化） | 调度器 685 → 250–280 ns（阶段 2），再到 200–220 ns | asm 审查 §16、plan §四 D1 |
| **P0** | `Scheduler::current_task()` 的 `shared_ptr` 拷贝改 `current_task_ptr()` | 8–12 ns/任务（每次 join 路径） | asm 审查 #2（改法零语义变化） |
| **P1** | CV 等待节点裸指针化 + 自旋预算调优 | CV 287 → 约 200 ns（追平 Go 的 1.2–1.5× 以内） | opt-review P0-1/P0-3；asm 审查 #1 |
| **P1** | `Task::wait()` 去掉 `std::mutex + CV + seq` 三件套，直接 futex word | 15–25 ns/任务 | asm 审查 #1 |
| **P1** | 编译期：`-fno-semantic-interposition` / `-fvisibility=hidden` / `-Wl,-Bsymbolic-functions` | 省 2180 处 GOT 间接调用中的大部分，10–20 ns/任务 | asm 审查 #5/#6 |
| **P2** | Context 创建路径：State 与 DoneSignal 合并、WithTimeout 去专用线程（用 timer wheel） | 265.6 → 目标 ≤120 ns（Go 76 的 1.6× 内） | performance §2.11 / §4.4 |
| **P2** | 非平凡 `thread_local` 改 POD `t_task_raw`（IE 模型） | 10–20 ns/park 往返 | asm 审查 #3 |
| **P3** | 补齐 folly::fibers 同机实测（编译后同口径） | 补齐矩阵缺项，不产生性能收益 | §2 表 |

**不做**：继续抠换栈汇编（裸切换只占 6.4 ns，压不出量级）；把 CV 冷路径当优化目标（主体是 futex）。

---

## 11. 待补 / 未测（诚实清单）

| 项 | 状态 | 原因 |
|---|---|---|
| folly::fibers 切换延迟 | **未测/待补** | 官方无 ns 级公开数字；本机未编译 folly（依赖重） |
| coost 官方 ns 数据 | **未取到** | 仓库文档路径 404（抓取失败），只用同机实测 2.79 ns |
| libco 官方 ns 数据 | **无公开值** | 官方 README 只有定性描述（微信数万台、千万连接） |
| 调度器任务「统一口径」基准 | **待补** | 685 / 326.7 / 656–764 三个快照的「任务」定义不同，需一次统一 harness 复测 |
| Fiber 23.5 ns 的独立基准报告 | **待补** | 该数出现在 plan 与 asm 审查的「基线」里，未见独立压测报告落盘 |
| Channel MPMC 多 P 多 C 吞吐（GOMAXPROCS 对比） | **未测** | 目前只测 1P1C 与单线程对 |
| TSan 验证 | **环境不可用** | WSL2 上任何 TSan 程序报 `unexpected memory mapping`，以 ASan + 随机压测替代 |
| CV 长等待（超自旋预算）达标判定 | **未测** | 现有数据只覆盖热路径与冷唤醒两种极端 |

---

## 12. 数据来源清单

| # | 来源 | 用途 |
|---|---|---|
| 1 | `docs/reports/2026-11-01-concurrency-survey.md` | §2/§3/§4/§5/§6 同机实测（9 类对比对象全部实测） |
| 2 | `docs/reports/2026-10-30-performance.md` | §6 WG / §7 Context / 优化前后数据 |
| 3 | `docs/reports/2026-10-31-verify-final.md` | 正确性门禁（3×10⁹ 混合操作 + 2.28×10⁸ 分模型）与失败现场 |
| 4 | `docs/reports/2026-11-01-asm-concurrency-review.md` | 指令数/分配/原子计数、根因归因、改进项排序 |
| 5 | `docs/reports/2026-11-01-concurrency-opt-review.md` | 后续内部快照（fast-cap1024 7.79 ns）与 22 项清单 |
| 6 | `docs/reports/2026-11-01-plan.md` | 调度器 326.7 ns / Fiber 23.5 ns 汇总 |
| 7 | `https://raw.githubusercontent.com/Tencent/libco/master/README.md` | libco 官方公开描述（无 ns 数字） |
| 8 | `https://raw.githubusercontent.com/facebook/folly/main/folly/fibers/README.md` | folly::fibers 官方设计说明（无 ns 数字） |

**工业库公开数据缺口**：coost、folly 的 ns 级公开数字本轮均未取到，已按规范标「未测/待补」，不以二手数字填充。
