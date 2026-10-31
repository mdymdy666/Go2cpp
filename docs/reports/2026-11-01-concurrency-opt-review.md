# 并发模块「还能再优化」审查（只读，2026-11-01）

> 审查方式：**只读代码**，不修改任何实现/测试文件。构建目录 `.build-bento-wsl`（Release，12 核 WSL2 / Ubuntu 24.04）。
> 环境限制：本机**没有 `perf`**（`which perf` 为空），因此定量结论来自：① 可直接运行的 `go2cpp_fast_channel_bench`；② 仓库既有基线（`docs/reports/2026-10-30-performance.md`、`docs/research/go-channel-opt.md`）；③ 对原子操作/系统调用数量与缓存行布局的静态计数。凡属估算一律标注「估算」。

## 0. 结论摘要

- 该模块**已经过多轮热路径打磨**：initial-exec TLS、打包状态字（state+token 单 CAS）、HotSpinLock 取代 `std::mutex`、栈 arena、futex_waitv 批量广播、WaitNode 线程本地池、RWMutex 单 64 位字、ChannelSpinLock 外层自旋。本次清单**不重复这些已落地的优化**。
- 剩余可优化空间集中在四类：**(A) 生命周期用 `shared_ptr` 表达导致的原子计数**、**(B) 冗余/过强的原子操作与内存序**、**(C) 跨 TU 边界与非内联调用**、**(D) 少数结构性布局与自旋策略**。
- 22 个点中，6 个属「低难度 + 中高收益」，可在不改语义的前提下先做；3 个属结构性改造（队列元素裸指针化、`runnable` 计数去全局化），收益最大但风险最高，建议单独立项。

现网实测（`./go2cpp_fast_channel_bench`，1P1C，200k 次）：

| 场景 | median | min |
|---|---:|---:|
| fast-cap0（Fiber rendezvous） | 264.17 ns/op | 254.32 |
| fast-cap0（线程 rendezvous） | 874.19 ns/op | 788.44 |
| fast-cap64（Fiber） | 14.33 ns/op | 14.10 |
| fast-cap1024（Fiber） | 7.79 ns/op | 7.15 |
| fast-try-1T（单线程非阻塞） | 5.06 ns/op | 5.05 |

对照既有基线：f2f park/wake 49.7 ns、t2t 195–237 ns、t2f 20–44 µs、f2t 75–100 µs。**无缓冲 rendezvous 与跨执行体唤醒是两个数量级的差距所在**，因此清单把「每次 park/wake 的固定开销」排在最前。

---

## 1. 优化清单（按优先级 = 收益/难度 排序）

评级口径：收益 = 高/中/低（对热路径单次操作的影响）；难度 = 低/中/高（改动面 + 回归风险）。
「优先级」= 收益÷难度的定性排序，P0 最高。

### P0-1　`ChannelParkOnce`/`ChannelWakeManaged` 用 `shared_ptr<void>` 类型擦除，每次阻塞/唤醒各付一次原子引用计数

- **位置**：`include/go2cpp/concurrency/channel.hpp:808`（`std::shared_ptr<void> task;`）、`channel.cpp:225`（`node->task = task;`）、`src/concurrency/channel.cpp:229-265`（`ChannelParkOnce` / `ChannelWakeManaged` 里的 `std::static_pointer_cast<Task>(node->task)`）、`include/go2cpp/concurrency/fast_channel.hpp:142`（同一路径）。
- **现状**：`WaitNodeBase` 用 `shared_ptr<void>` 存 Task 以避免头文件依赖 `scheduler.hpp`。每次 park（`ChannelParkOnce`）和每次唤醒（`ChannelWakeManaged`）都要 `static_pointer_cast` → **构造一个临时 `shared_ptr<Task>`（refcount +1）再析构（−1）**，即每次 2 次跨核 RMW。cap0 rendezvous 一来一回共 4 次纯属类型擦除的开销。节点本身已是**线程本地池 + 裸指针**模型（`WaitNodePool`），只有这一个字段还在用所有权语义。
- **建议**：改为 `Task* task_raw`（裸指针）+ 保留 `void* scheduler`。生命周期依据与现有 `scheduler` 指针一致：受管等待期间，G 必然被 Scheduler 注册表强引用（`task_registries`），且等待者栈帧在 park 期间不可能返回；`WaitNodeBase::ResetBase()` 里置空即可。若担心 debug 构建的可观测性，可在 `#if GO2CPP_DEBUG_RESOURCES` 下额外保留 `weak_ptr`。
- **收益**：高。每次 park/wake 省 2 次原子 RMW；按 f2f 基线 49.7 ns 估算，可省约 4–8 ns（估算 8–16%）。
- **风险**：中低。需覆盖「节点被池复用后指针陈旧」的既有审计点（`ResetBase` 已清零），以及 shutdown 期 Task 先于节点销毁的窗口——但该窗口今天同样存在（`scheduler` 裸指针已是同一契约），不新增风险类别。
- **难度**：低-中。

### P0-2　`Scheduler::current_task()` 拷贝 `shared_ptr` 只为取 `.get()`

- **位置**：`src/concurrency/channel.cpp:248`：`Scheduler::current_task().get() != task.get()`；实现见 `src/concurrency/scheduler.cpp:4727-4729`（`return t_task;` 返回拷贝）。
- **现状**：`t_task` 是 `thread_local std::shared_ptr<Task>`。该比较**只需要裸指针**，却触发一次 shared_ptr 拷贝构造 + 析构（2 次原子 RMW + 一次 TLS 访问）。同文件 `current_task_ptr()`（`scheduler.cpp:4731`）已经是零成本的裸指针版本。
- **建议**：改用 `Scheduler::current_task_ptr()`。全仓 grep 一遍 `current_task().get()`，统一替换。
- **收益**：中。每次 park 的固定开销里直接砍掉 2 次 RMW。
- **风险**：极低（语义完全等价，`current_task_ptr()` 已是公开 API 且文档写明用于热路径）。
- **难度**：极低（一行 + 同类清理）。

### P0-3　`WakeNodeLocked` 对受管 Fiber 节点做了一次无人消费的 `seq.fetch_add`

- **位置**：`include/go2cpp/concurrency/channel.hpp:2359-2370`；同构代码 `include/go2cpp/concurrency/fast_channel.hpp:137-148`（`FastPublishNodeLocked`）。
- **现状**：

  ```cpp
  static void WakeNodeLocked(detail::WaitNodeBase* node) noexcept {
    node->seq.fetch_add(1, std::memory_order_release);   // ← managed 分支根本不用它
    if (node->managed) { detail::ChannelWakeManaged(node); return; }
    if (node->parked.load(relaxed) != 0) { detail::ChannelFutexWake(&node->seq); }
  }
  ```

  受管路径走 Scheduler，`seq` 只有 native futex 语义，这里的 RMW 完全无用；而它发生在**持 Channel 锁内**，是唤醒延迟上的直接成本。
- **建议**：把 `seq.fetch_add` 移到 `!node->managed` 分支内（`parked` 判断之前或之后均可）。
- **收益**：中。每次 Fiber 唤醒省一次跨核 RMW；唤醒方常常不是节点所在核，这一行的 miss 成本高于指令本身。
- **风险**：极低。native 路径保持原顺序；managed 路径不读 `seq`（`ChannelParkOnce` 只看 `signal`）。
- **难度**：极低。

### P0-4　`SpinMutex::MarkOwner` 每次加锁都 `shared_from_this()`

- **位置**：`include/go2cpp/concurrency/spin_mutex.hpp:463-475`（`MarkOwner`）、`spin_mutex.hpp:486`（`std::shared_ptr<Task> m_owner;`）；解锁侧 `spin_mutex.hpp:271-274`。
- **现状**：受管 G 加锁成功时执行 `self->shared_from_this()`（一次 `weak_ptr::lock`：原子 CAS 循环 + refcount 写入）并存入 `m_owner`；解锁时 `m_owner.reset()`（再一次 RMW）。**SpinMutex 的语义只在持锁期间用 owner 递减 `no_park_depth`**，而持锁者必然是**当前正在运行的 G**——它不可能在临界区内被销毁（Scheduler 注册表持有强引用，Fiber 栈在脚下）。
- **建议**：`m_owner` 改成 `Task* m_owner_raw`，`MarkOwner` 直接 `self->enter_no_park_section()` 并记裸指针；`Unlock` 用裸指针 `leave_no_park_section()`（已按 owner 记账，跨 G 解锁语义不变）。若 debug 构建需要保活，用 `#if` 单独保留 shared_ptr 分支。
- **收益**：中高。SpinMutex 的卖点就是「一次 CAS 进门」，当前实际是「CAS + weak_ptr::lock + refcount RMW」，热路径被抬高一截；去掉后接近设计目标。
- **风险**：低-中。需要确认「跨 G 解锁」时 owner 已退出的场景——现有实现用 `shared_ptr` 保证 `leave_no_park_section()` 不会 UAF；改裸指针后，跨 G 解锁必须保证 Task 仍被注册表持有（当前 `Task` 在终态后会从注册表回收，理论上存在窗口）。**这一点必须在改造时用注释+断言固定契约**，或保留 shared_ptr 仅用于跨 G 解锁的降级路径。
- **难度**：低-中（风险点集中在跨 G 解锁）。

### P0-5　外部生产者入队走 `std::mutex`（pthread，无竞争也要完整状态机）

- **位置**：`src/concurrency/scheduler.cpp:262-274`（`IncomingStripeLock` 用 `std::mutex` + 每次 `++/--t_incoming_lock_depth`）、使用点 `scheduler.cpp:3255-3257`、`scheduler.cpp:4009` 附近。
- **现状**：`Scheduler::enqueue()` 的**外部生产者热路径**（channel 唤醒、IO 唤醒、定时器回调、`go()`）每次都要：`pthread_mutex_lock/unlock`（项目自己测过 15–25 ns）+ 两次 TLS 读写（锁序守卫用的深度计数）。而同仓库的 `detail::HotSpinLock`（`scheduler/scheduler.hpp:462-508`）已经证明在这类「几条指令的临界区」上更快（8–12 ns，见 `docs/research/go-channel-opt.md:296`）。
- **建议**：`IncomingStripeLock` 换 `detail::HotSpinLock`（或 `core::FutexMutex`，若担心长临界区）；`t_incoming_lock_depth` 的加减合并到同一 RAII 里但只在 debug 构建维护（release 下锁序错误不可能发生）。
- **收益**：中。每次跨线程唤醒/提交省 5–15 ns，且去掉 pthread mutex 的 robust/type 检查分支。
- **风险**：低。`HotSpinLock` 不可重入——需确认 `incoming_queues[stripe]` 内不会有重入 `enqueue()`（现有 `t_incoming_lock_depth != 0` 检查已显式禁止并 `std::terminate`，等于已有契约）。
- **难度**：低。

### P0-6　外部入队一次操作 3 次全局 RMW（`runnable` / `incoming_count` / `incoming_nonempty_mask`）

- **位置**：`src/concurrency/scheduler.cpp:3258`（`runnable.fetch_add`）、`3263-3264`（`incoming_count.fetch_add`）、`3265-3267`（`incoming_nonempty_mask.fetch_or`）；出队侧 `scheduler.cpp:4899`、`4932`（`runnable.fetch_sub`）。
- **现状**：每投递一个 G 就写 3 个全局共享原子；其中 `incoming_nonempty_mask` 是**幂等置位**（同一 stripe 位反复 `fetch_or` 同一个值），完全可以先 `load(relaxed)` 判断该位是否已置位，未置位才 `fetch_or`。
- **建议**：
  1. `incoming_nonempty_mask`：`if (!(mask & bit)) mask.fetch_or(bit)`（工作线程清位时用 `fetch_and`，语义不变）；
  2. `incoming_count`：改成 per-stripe 计数数组，`wakeup` 判定用「任一分条非空」的 mask（已存在），彻底去掉每次入队的全局 RMW；
  3. `runnable`：见 P2-1（需要更大改造）。
- **收益**：中。每次外部唤醒少 1–2 次跨核 RMW；高并发提交时这 3 个字段会互相 ping-pong。
- **风险**：低-中。`incoming_count` 目前被 `shutdown` 与 worker 的「有没有活」判定读；改 per-stripe 后需保证 shutdown 逐条带扫描（已有该逻辑，`begin_shutdown_locked` 就是逐 stripe 的）。
- **难度**：低-中。

### P0-7　`Fiber::resume_from_scheduler` 重复了 `Task::execution_claim` 已经提供的串行化

- **位置**：`src/concurrency/fiber.cpp:1848-1868`（`m_resume_claim.compare_exchange_strong(acq_rel)` + `store(release)`）、`fiber.cpp:2278`（`std::atomic<bool> m_resume_claim`）。
- **现状**：注释已写明「调度器已经通过 Task 的 execution claim 串行化恢复」。既然如此，调度器路径上的 `m_resume_claim` **CAS+store 是双保险**，每次 resume 多付 1 次 acq_rel CAS（包含完整内存栅栏）。f2f 基线 49.7 ns 中这是可测量的一块。
- **建议**：新增一个「调用方已持有执行权」的入口（如 `resume_from_scheduler_claimed()`），跳过 claim；`Fiber::resume()` / `resume_from_scheduler()` 的对外语义保持不变（默认仍校验）。或用 `m_resume_claim.store(true, relaxed)` + 仅 debug 构建保留 CAS。
- **收益**：中。每次 Fiber 切回省一次 acq_rel CAS（估算 3–5 ns，约 6–10%）。
- **风险**：中。属于「把不变量交给调用方」的典型改造，必须用注释 + 断言（`m_run_claim == true` 或 `current_task_ptr()` 匹配）把契约钉死，否则并发 resume 会双栈执行——**这是最危险的一类回归**。
- **难度**：低（代码量）／中（评审强度）。

### P0-8　编译期：缺 LTO / `-fno-semantic-interposition` / `-fno-plt`，热路径跨 TU 调用无法内联

- **位置**：`CMakeLists.txt`（全文无 LTO 相关选项）、`cmake/Go2CppModules.cmake:160`（`go2cpp_runtime` 是 INTERFACE 库）、`cmake/Go2CppOptions.cmake`（无 `-fno-semantic-interposition`）。
- **现状**：多个**每次 park/wake/channel 操作都要走**的函数体在 `.cpp` 里，头文件只留声明：
  - `detail::ChannelParkOnce` / `ChannelWakeManaged` / `ChannelFutexWake`（`channel.cpp`，但被 `channel.hpp`（模板）和 `fast_channel.hpp` 调用）；
  - `Scheduler::current_task_ptr()` / `current_scheduler()` / `current_processor_id()`（`scheduler.cpp:4727-4760`）；
  - `Task::park_for_scheduler()` / `wake_for_wait_impl()`（`scheduler.cpp`，从 `channel`/`sync` 侧调用）。

  共享库默认语义允许符号插替（interposition），GCC 因此**不会跨 TU 内联**这些函数，调用点都得走 PLT。项目已经为「懒绑定 xsave 崩溃」加了 `-Wl,-z,now`（`CMakeLists.txt:15-17`），说明链接期行为是被认真对待的——但没有做 `-fno-plt` + `-fno-semantic-interposition`。
- **建议**：
  1. 给库目标加 `-fno-semantic-interposition`（GCC 下允许跨 TU 内联同库内符号，不改变 ELF 语义）；
  2. 加 `-fno-plt`（省掉每次调用的 PLT 跳板与 GOT 间接）；
  3. 试 `-flto=thin`（或 `-flto=auto`）并做完整回归（含 ASan/TSan 构建，`--gc-sections` 与 sanitizer 组合需要验证）；
  4. 对 `current_task_ptr()` 这类 3 行函数，考虑把 TLS 变量以 accessor 形式暴露到头文件（或用 `GO2CPP_ALWAYS_INLINE` + `__attribute__((visibility("hidden")))`）。
- **收益**：中高。估算热路径 3–8%（省 PLT + 允许内联 `current_task_ptr`/`ChannelParkOnce` 这类小函数）。
- **风险**：中。LTO 会改变内联与栈布局，**必须重跑 fiber 栈/信号（SIGSEGV 增长）与 ASan/TSan 套件**；`-fno-semantic-interposition` 会破坏「运行时替换同库符号」的用法（项目未使用，但需确认）。
- **难度**：低（改构建）／中（验证）。

### P1-1　`AddCancelCallback` 的 `std::function` 必然堆分配（每次带 context 的阻塞收发一次 malloc）

- **位置**：`include/go2cpp/concurrency/channel.hpp:2413-2429`（捕获 `weak_ptr<CancelHub>` 16B + `address` 8B + `generation` 4B）、`src/concurrency/context.cpp:371-420`（`DoneSignal::State::CallbackEntry{id, std::function}` 存入 `std::vector`）。
- **现状**：lambda 捕获区约 28–32 字节，**超过 libstdc++ `std::function` 的 16 字节 SBO**，必然 `operator new`。加上 `callbacks.emplace_back` 触发的 vector 扩容，单次「带 Context 的阻塞 Recv/Send」= 1 次 malloc + 1 次可能的 realloc + 1 次 free（在高并发下是 arena/锁竞争）。`docs/research/go-channel-opt.md:216` 也指出「一次带超时的阻塞 Recv ≈ 4 次堆分配」。
- **建议**：
  1. 给 `DoneSignal` 增加 C 风格注册：`CallbackId AddCallbackRaw(void (*fn)(void*), void* ctx)`，把 `{fn, ctx}` 存进 `CallbackEntry`（16 字节，零分配）；
  2. 或把取消登记做成**侵入式**：`WaitNodeBase` 内置 `CancelHook{prev,next,id}`，由 `Channel` 自己的 `CancelHub` 维护双链表（现有 `CancelWaiter` 已经按 (address, generation) 定位，改成 O(1) 摘链更顺）；
  3. vector 改为小容量 inline 存储（`static_vector` 风格）以消除首个回调的扩容。
- **收益**：高。一次 malloc/free 在 4–16 线程下约 40–120 ns，而 cap0 rendezvous 的 Fiber 路径总共 264 ns——这是占比最大的一块可摘除成本。
- **风险**：中。`DoneSignal` 是公共 API（`context.hpp`），新增接口需保持 ABI/源码兼容；回调执行必须在锁外（现有实现已保证），改造时不能破坏。
- **难度**：中。

### P1-2　`FutexMutex` 的快慢路径都可以再省一次 RMW

- **位置**：`include/go2cpp/concurrency/core/futex_mutex.hpp:109-116`（慢路径 `exchange(kLockedBit|kWaitersBit)` 循环）、`futex_mutex.hpp:129-130`（`unlock` 无条件 `fetch_and`）。
- **现状**：
  - `lock()` 慢路径每轮都做一次 `exchange`（写操作，抢独占），即使 futex_wait 因 EAGAIN 立刻返回（值刚变）；标准做法是 EAGAIN 后先 `load(acquire)` 判断锁位，未持有时再 CAS；
  - `unlock()` 用 `fetch_and(~kLockedBit)`，**无竞争时也是一次 RMW**（`lock` 前缀，约 20 cycles + 抢占缓存行）。当状态恰为 `kLockedBit`（无等待者、无令牌）时，用 `store(0, release)` 即可；只有状态含 `kWaitersBit` 时才需要 RMW 或 CAS。
- **建议**：
  - `unlock()`：先 `load(relaxed)`，若 `== kLockedBit` 则 `compare_exchange_strong(kLockedBit→0)`（CAS 在无竞争下比 `lock and` 便宜，且失败时退回原 `fetch_and`）；
  - `lock()` 慢路径：EAGAIN 分支加 `if (m_state.load(acquire) == kUnlocked) { CAS 0→locked }` 短路。
- **收益**：中。`FutexMutex` 被 `ParkingCondition` 队列锁、`ChannelSpinLock`、`WaitNode` 门、incoming 锁共用，是仅次于 Task 状态字的第二热原子。
- **风险**：低。状态机不变量（`kWaitersBit` 必须保持到队列真的为空）不能动；建议保持 `kMaxWakeAttempts` 唤醒链逻辑不变，只优化判定入口。
- **难度**：低。

### P1-3　`ChannelSpinLock` 固定 64 次 PAUSE 自旋，与「临界区只有几十条指令」不匹配

- **位置**：`include/go2cpp/concurrency/channel.hpp:963-996`（`kSpinIterations = 64`）。
- **现状**：每次抢不到锁都先跑 64 次 PAUSE（约 0.5 µs）再进 `FutexMutex::lock`。对一个「几条到几十条指令」的临界区，64 次自旋意味着**持锁者被抢占时，等待者会白烧 0.5 µs 才睡**；而 MPMC 4 线程下锁交接频繁，这段自旋会叠加到每次交接上。对比：`HotSpinLock` 用 128 次但只在「已知有竞争者」时进入读自旋；`SpinMutex` 用 64 次后就跟一次 `yield_current()`。
- **建议**：两级策略——首次冲突只 8–16 次 PAUSE（覆盖「持锁者正在同一核上跑完最后几条指令」），之后改为 `sched_yield`/`park` 前的退避；或用「上次是否自旋成功」的历史做自适应（`SpinBudget` 已有该模式，见 `channel.hpp:565-578`）。
- **收益**：中。锁交接密集场景（MPMC 压测）每次交接省 0.3–0.5 µs 的无效自旋。
- **风险**：低-中。自旋缩短会让「刚释放就被抢」的命中率下降，需要 A/B（项目已有 `bench_channel_opt` 口径可直接复用）。
- **难度**：低。

### P1-4　`Fiber::resume_locked` 每轮 3 次状态 load + 5 次 `FiberRecord` RMW

- **位置**：`src/concurrency/fiber.cpp:2016-2018`（state/reason/propagate 三次 store）、`2024-2040`（`affinity_machine.exchange` + `affinity_processor.store` + `affinity_hits.fetch_add`）、`2080`/`2106`/`2115-2116`（三次 `m_record->state.load(acquire)`）。
- **现状**：一次 resume 对同一个 `FiberRecord` 做 6 次以上原子写 + 3 次同字段读；其中 `2080` 与 `2106` 读的是**同一个字段**（中间无用户代码），`2115` 的 `after_jump` 也紧随其后。亲和统计（`affinity_hits/migrations`）只是诊断用途，却每次切换都写一条共享 cache line——而 Scheduler 侧 `collect_metrics=false` 的部署并不需要它。
- **建议**：
  1. 三次 `state` load 合并为一次（`jump` 返回后立刻读一次，后续分支复用）；
  2. `affinity_machine`/`affinity_processor` 合并为**一个 64 位打包字**（machine 在高 32 位），一次 store 代替 exchange+store；`affinity_hits` 只在数值变化或 `collect_metrics` 打开时递增（`Task` 已有 `collect_metrics` 配置，可透传到 Fiber）；
  3. state/reason/propagate 合并为一次打包 CAS（`FiberState` 8 位 + `SuspendReason` 8 位 + propagate 1 位，与 `Task::m_state_word` 同款做法）。
- **收益**：中高。每次切换省 4–5 次原子操作；f2f 49.7 ns 里估计能砍 15–25%。
- **风险**：中。`FiberRecord` 被 `debug_stack`/`debug_info`/`context_snapshot` 跨线程读，打包后必须保持「单字 linearizable」，否则诊断会读到撕裂状态。
- **难度**：中。

### P1-5　`FiberRecord` 冷热字段混排，诊断/父链字段与切换热字段同行

- **位置**：`src/concurrency/fiber.cpp:1295-1345`（`FiberRecord` 字段区）、`src/concurrency/fiber.cpp:1477`（`Fiber::Impl`）。
- **现状**：每轮切换要写的 `state`/`reason`/`affinity_*` 与只在诊断时读的 `parent`/`active_parent_id`/`last_thread`/`execution`/`stack_low`/`stack_high`/`function_name` 混在同一结构里；`m_record` 还被 `m_metadata_mutex` 保护（`bind_caller`、`reset_for_scheduler`）。诊断线程（Doctor/`debug_stack`）遍历时会反复触摸这些行，把正在 resume 的热行打出缓存。
- **建议**：把 `FiberRecord` 显式分两段并各自 `alignas(64)`：**热段**（`state`、`reason`、`cancellation_requested`、`affinity_*`）与**冷段**（父链、线程 id、执行绑定、栈范围、函数名）。热段只由持 `resume_claim` 的线程写；冷段只由持 `m_metadata_mutex` 的线程写——**这同时消除了「诊断读与热写共享互斥量」的需要**。
- **收益**：中。热路径少一次 cache line 争用/失效；诊断开销与运行时抖动解耦。
- **风险**：低（纯布局）。但 `FiberRecord` 被 `shared_ptr` 共享且部分字段在锁外读（`alive`、`state`），拆分后要复查每个读点的同步方式。
- **难度**：中。

### P1-6　`LocalRunQueue::Cell` 用 `shared_ptr<Task>` 作元素，每槽 32 字节、两槽共享一条 cache line

- **位置**：`src/concurrency/scheduler.cpp:429-437`（`struct Cell { atomic<size_t> sequence; shared_ptr<Task> task; }`、`alignas(64) array<Cell,256> m_cells`）。
- **现状**：`sizeof(Cell)` = 8 + 16 = 24 → 对齐到 32 字节（`shared_ptr` 8 字节对齐），**一条 cache line 装 2 个槽**；生产者写槽 N 时会使消费者正在读的槽 N+1 失效。并且 push/pop 各要移动一次 `shared_ptr`（refcount 原子），队列本身成了「原子计数放大器」——队列里每个元素生命周期都带 1 次 +1 和 1 次 −1。
- **建议**：
  1. 元素改 `Task*` 裸指针，强所有权由注册表（`task_registries` 已持强引用直到终态回收）与 `incoming` 队列节点共同保证；`push/pop` 变成纯指针搬运；
  2. 槽内布局 `alignas(64)` 或至少保证 `sequence` 与 `task` 在**同一 32 字节半行**，减少跨槽拖拽（当前已满足，但每槽 32B 意味着相邻生产者/消费者互扰）；
  3. 若保留 `shared_ptr`，至少把 `Cell` 显式 `alignas(32)`（去掉隐式 padding 的不确定性）。
- **收益**：高。每次 push/pop 省 2 次原子 RMW + 一次 refcount 控制块随机访问；256 槽数组从 8 KB 降到 4 KB（更友好于 L2）。
- **风险**：高。这是队列所有权的核心不变量（注释 `scheduler.cpp:333-334` 明确写了「元素是 shared_ptr：队列节点本身是注册表之外的第二份强引用，已取消的残留节点在出队前不会析构」）。**改成裸指针必须先在 `begin_shutdown_locked` / 取消路径上重建等价保证**，否则会 UAF。
- **难度**：高。建议单独立项，配套 ASan + 随机压测。

### P1-7　`RunNextSlot` 的单槽每次 push/take 都走 `HotSpinLock`

- **位置**：`src/concurrency/scheduler.cpp:463-496`。
- **现状**：`runnext` 是**每次 yield/唤醒都会碰**的快速槽（`requeue_from_worker`、`publish_runnext`、worker `step0`），实现是 `HotSpinLock`（一次 `exchange` acquire + 一次 `store` release）。单槽完全可以用三态 CAS 做到无锁：`Empty / Full(task) / Contended`，push 用 `CAS(Empty→task)`、take 用 `exchange(Empty)`。
- **建议**：改成 `std::atomic<std::uintptr_t>` 存 tagged 指针（低位 2 位做状态），或用 128 位 CAS（x86 `cmpxchg16b`，C++ 里需 `-mcx16` 或内联汇编）。最简单可行版：`atomic<Task*> slot` + 独立 `atomic<bool> busy` 的乐观双字协议（读-校验-重试）。
- **收益**：中。省一次 `exchange`（每次 yield/唤醒的核心路径）。
- **风险**：中。单槽无锁 push 的「挤出 displaced」语义需要仔细处理（当前 `displaced` 由调用方回退到本地队列）；ABBA 风险低但要 TSan 验证。
- **难度**：中。

### P2-1　`runnable` 全局计数在 worker 取任务快路径上做 `fetch_sub`

- **位置**：`src/concurrency/scheduler.cpp:4899`、`4932`（每次 claim 后 `runnable.fetch_sub(relaxed)`）、判定 `4916-4919`（`runnable.load(acquire) > processors.size()`）。
- **现状**：所有 worker 每取一个 G 都对**同一个全局原子**做 RMW；高并发下这是调度器里唯一的全局串行点（且与 `incoming_count`、`live_tasks` 同行/邻近行）。
- **建议**：改成 per-P 计数（`Processor::runnable`）+ 全局只做「求和判定」（或干脆用 per-P 的 `queue.size()` + mask 判定）；`shutdown` 的「是否还有人可跑」用「所有 P 计数为 0 且 incoming mask 为 0」的 O(P) 判定（P 通常是核数，几十个，可接受）。
- **收益**：中高。去掉每次取任务的全局 RMW，多核扩展性直接受益。
- **风险**：中高。`runnable` 参与 shutdown 收敛判定（`draining` 用 O(1) 计数代替 O(n) 扫描，见 `scheduler.hpp:716-719`），改成分片后必须保证「计数增减与状态转移同一线性化点」这条不变量仍然成立。
- **难度**：中高。建议与 P1-6 一起立项。

### P2-2　`m_generation_watchers` 的 acq_rel RMW 可 relax，且可与 `m_generation` 合并布局

- **位置**：`include/go2cpp/concurrency/channel.hpp:2789-2792`、`3159-3162`（`fetch_add/fetch_sub(acq_rel)`）、`2623-2629`（`PublishChangeLocked` 每次 send/recv 先 load 它）、`3494`（字段声明，与 `m_generation`/`m_buffer`/`m_senders` 相邻）。
- **现状**：这两处 acq_rel 只用于**计数**（决定是否要推进 generation），不需要任何同步语义；且该计数与热字段同行，观察者线程的 RMW 会把 `m_closed`/`m_senders`/`m_receivers` 所在行拉走。
- **建议**：① 两处改 `relaxed`（配对使用 `fetch_add/sub` 已保证不丢）；② 把 `m_generation_watchers` + `m_generation` 移到独立 cache line（`alignas(64)`），与 `m_lock` 之后的冷字段分开；③ 更进一步：取消 `watchers` 门控，`PublishChangeLocked` 无条件 `m_generation.fetch_add(1, relaxed)`（在锁内，行已是独占，代价低于跨线程读 watchers 的 miss）。
- **收益**：中低。省 2 次 RMW 的栅栏成本 + 减少一次跨行触碰。
- **风险**：低。计数放松不影响正确性（只影响 `WaitForChange` 是否被唤醒，谓词会重检）。
- **难度**：极低。

### P2-3　`ParkingCondition::WaitNative` 的 `futex_waiters` 计数用 acq_rel

- **位置**：`src/concurrency/parking_condition.cpp:419`、`505`、`507`。
- **现状**：`m_hot.futex_waiters.fetch_add(1, acq_rel)` / `fetch_sub(1, acq_rel)`，外加 `m_broadcast.waiters` 同样 acq_rel。这些计数只用于「notify_all 时是否可以跳过系统调用」，纯提示。
- **建议**：全部改 `relaxed`（`notify_all` 侧读它做决策，读到陈旧值最多多一次 FUTEX_WAKE，不影响正确性）。
- **收益**：低-中。每次 park 省 2 条带栅栏的 RMW。
- **风险**：极低。
- **难度**：极低。

### P2-4　`sync::Mutex` 每个实例一次 `make_unique<Impl>` 堆分配

- **位置**：`src/concurrency/sync.cpp:1340`（`Mutex::Mutex() : m_impl(std::make_unique<Impl>())`）、`include/go2cpp/sync.hpp:376-377`。
- **现状**：`Mutex` 把热状态字内联到了对象起始（很好的设计），但 `Impl`（等待队列 deque + `std::mutex` + debug id + owner 记录）仍是一次堆分配；`SpinMutex` 里还**内嵌**一个 `Mutex`（`spin_mutex.hpp:479`），于是每把 SpinMutex = 1 次 Mutex 构造 = 1 次 malloc。大量小锁对象（连接池、session 表）的构造/析构成本与内存碎片都在这里。
- **建议**：把 `Impl` 的关键字段直接内联进 `Mutex`（`std::deque` 可换成侵入式链表，`std::mutex` 换 `FutexMutex`），或将 `Impl` 改为**惰性分配**（首次进入慢路径才 `make_unique`，快路径永远不分配）。
- **收益**：中低。构造/析构路径（非热循环）；但对「每连接一把锁」的服务器模型是实打实的分配次数减半。
- **风险**：低-中。`Impl` 里含 `std::mutex`，内联会改变 `sizeof(Mutex)`（当前 128 字节，已因 `alignas(64)` 变大）；需检查是否有 ABI/序列化依赖。
- **难度**：中。

### P2-5　`Scheduler::park_wait` 每次 park 重复读 4–6 次同类原子 / TLS

- **位置**：`src/concurrency/scheduler.cpp:3755-3789`。
- **现状**：`current_scheduler()`、`current_task_ptr()`、`m_impl->stop_requested()` ×2、`task->cancellation_requested()` ×2、外加 `park_for_scheduler()` 内部的 `current_task_ptr()` 与两个计数 load。全部是同线程可见、park 期间不会变的判定。
- **建议**：入口处取一次快照（`scheduler`/`task` 局部变量、`stop` 与 `cancel` 各读一次），后续分支复用；`stop_requested()` 可以在进入 `park_for_scheduler` 前只判一次（第二次检查的语义是「覆盖 shutdown 与 park 的交错」，但 shutdown 侧会通过 `wake_for_wait` 收敛，保留即可但不必重复读 `m_impl`）。
- **收益**：低。每条都是一次 L1 命中，但 park 本身只有几十 ns 量级时占比可见。
- **风险**：低-中。第二次 `stop_requested` 检查是**正确性窗口**（shutdown 与 park 交错），不能简单删掉——只能合并读取时机，不能去掉检查。
- **难度**：极低。

### P2-6　debug 构建下 111 处 `GO2CPP_DEBUG_EVENT` 落在调度热路径

- **位置**：`src/concurrency/scheduler.cpp`（111 处调用）、`include/go2cpp/debug/debug_event_ring.hpp:661-673`（`DebugConfig::Enabled(...)` 运行时判定）、`720`（未启用时的空宏）。
- **现状**：Release（`GO2CPP_DEBUG_DUMP=0`）下宏展开为 `((void)0)`，零成本——**这一条只影响 debug/诊断构建**。但 debug 构建恰恰是排查并发问题的主力构建，`Enabled()` 是跨 TU 的非内联查询，`Task::run()` 内一次切换就有 4 处。
- **建议**：worker 线程启动时把「本 worker 是否采集」缓存成 `thread_local bool`，宏里只判这个 bool；或按 category 在 `worker_loop` 入口做一次位图快照。
- **收益**：低（仅 debug 构建）。让 debug 构建的调度行为更接近 release，减少「加上调试就变行为」的观察者效应。
- **风险**：极低。
- **难度**：低。

### P2-7　`context_x64.S` 不保存 MXCSR / x87 控制字（正确性风险，非性能）

- **位置**：`src/concurrency/context_x64.S:50-75`（`go2cpp_context_jump` 只保存 6 个 callee-saved 通用寄存器）。
- **现状**：与 fcontext/coost 一致（SysV 不要求保存 MXCSR），**性能上已无可优化**（约 15 条指令）。但若用户代码在 Fiber 内改舍入模式/异常掩码（`fesetround`、`_controlfp`），状态会跨 Fiber 泄漏——这是切换点唯一未覆盖的 ABI 面。
- **建议**：不改为默认；在 `Fiber` 文档中明确「Fiber 之间不隔离 FPU 控制字」，或用 `#if GO2CPP_FPU_ISOLATION` 提供可选的 `stmxcsr/fnstcw` 保存（每次切换 +2 条指令）。
- **收益**：低（性能上为负）；价值在正确性边界清晰化。
- **风险**：低。
- **难度**：低。

---

## 2. 优先级矩阵

| 点 | 收益 | 难度 | 优先级 | 一句话 |
|---|---|---|---|---|
| P0-1 task 裸指针化 | 高 | 低-中 | ★★★★★ | 每次 park/wake 省 2 次 RMW |
| P0-2 current_task().get() | 中 | 极低 | ★★★★★ | 一行替换 |
| P0-3 WakeNodeLocked 冗余 seq | 中 | 极低 | ★★★★★ | 移进 native 分支 |
| P0-4 SpinMutex shared_from_this | 中高 | 低-中 | ★★★★★ | 加锁热路径去 refcount |
| P0-5 incoming 锁自旋化 | 中 | 低 | ★★★★☆ | pthread → HotSpinLock |
| P0-6 入队 3 次全局 RMW | 中 | 低-中 | ★★★★☆ | mask 先读后写 |
| P0-7 resume_claim 冗余 CAS | 中 | 低（评审重） | ★★★★☆ | 契约交给 execution_claim |
| P0-8 LTO/fno-plt | 中高 | 低-中 | ★★★★☆ | 让跨 TU 小函数可内联 |
| P1-1 AddCallback 堆分配 | 高 | 中 | ★★★★ | 每次阻塞一次 malloc |
| P1-2 FutexMutex 双优化 | 中 | 低 | ★★★★ | 省 2 次 RMW |
| P1-3 ChannelSpinLock 自适应 | 中 | 低 | ★★★★ | 64 次 PAUSE 太长 |
| P1-4 resume 原子合并 | 中高 | 中 | ★★★☆ | 3 load→1，5 RMW→2 |
| P1-5 FiberRecord 冷热分离 | 中 | 中 | ★★★☆ | 诊断不拖热行 |
| P1-6 Cell 裸指针化 | 高 | 高 | ★★★ | 队列去 refcount，风险最高 |
| P1-7 RunNextSlot 无锁化 | 中 | 中 | ★★★ | 单槽三态 CAS |
| P2-1 runnable 去全局 | 中高 | 中高 | ★★★ | 唯一全局串行点 |
| P2-2 generation_watchers | 中低 | 极低 | ★★★ | acq_rel→relaxed + 移行 |
| P2-3 futex_waiters relaxed | 低-中 | 极低 | ★★☆ | 纯提示计数 |
| P2-4 Mutex::Impl 内联 | 中低 | 中 | ★★☆ | 构造期一次 malloc |
| P2-5 park_wait 重复读 | 低 | 极低 | ★★ | 快照复用 |
| P2-6 debug 宏缓存 | 低 | 低 | ★★ | 只 debug 构建 |
| P2-7 FPU 控制字 | 低 | 低 | ★ | 正确性边界 |

> 表中 P0/P1/P2 是「实施批次」建议，与上面章节编号对应；★ 数量是收益/难度比。

## 3. 已确认「不值得再动」的部分（避免重复劳动）

- **Cell / head / tail 的缓存行分离**：`LocalRunQueue` 已把 `m_cells`、`m_head`、`m_tail` 各自 `alignas(64)`（`scheduler.cpp:434-437`），无需再调。
- **Task 状态与唤醒令牌打包**：单 32 位字 + CAS 已实现（`scheduler.cpp:521-556`），比双原子正确且更快，不要拆回去。
- **ParkingCondition 的 native 等待**：per-node futex + futex_waitv 广播 + 2048 次自旋 + deadline 每 128 次采样（`parking_condition.cpp:407-509`）已是成熟形态；剩下的只有 P2-3 的 relaxed。
- **RWMutex 的状态字**：单 64 位 `fetch_add/CAS`（`rwmutex.hpp` 头部说明），1t 约 5 ns 已优于 Go；不要再引入每读者互斥量。
- **fiber 栈分配**：arena + 线程本地/全局缓存已把稳态建栈变成零系统调用（`fiber.cpp:568+`），不要再逐栈 `mmap`。
- **把窃取移出全局锁**：项目已实测 −34%（`scheduler.hpp:395-397`），不要重试。
- **`-Wl,-z,now`**：为规避懒绑定 xsave 冲栈而加（`CMakeLists.txt:15-17`），不要为启动时间改回 lazy。

## 4. 建议的实施与验证顺序

1. **第一批（1 天内可完成、零语义风险）**：P0-2、P0-3、P2-2、P2-3、P2-5。
   验证：`ctest -R "channel|park|scheduler"` + `go2cpp_fast_channel_bench`（已有二进制口径）+ TSan 构建跑 `go2cpp_channel_concurrency`。
2. **第二批（本周）**：P0-1、P0-4、P0-5、P0-6、P1-2、P1-3。
   验证：ASan + TSan + `go2cpp_channel_thread_stress`、`go2cpp_parking_condition_stress`，并复跑 fast-channel 三档（cap0/64/1024）。
3. **第三批（需设计评审）**：P0-7、P1-1、P1-4、P1-5、P0-8。
   验证：复用 `docs/reports` 里同类压测矩阵 + 单核/超订（`taskset`）场景，确认没有把「自旋命中」换成「park 更多」。
4. **第四批（立项）**：P1-6、P1-7、P2-1。
   这三项都触碰所有权/计数不变量，必须配套「失败注入 + shutdown 竞态」测试，单独 PR。

## 5. 方法与局限

- **未使用 `perf`**：本机无 `perf` 二进制（`which perf` 为空），`perf record/report` 不可用；因此没有指令级归因。所有百分比均为基于「原子操作条数 × 典型跨核成本（20–40 cycles）」的估算。
- **部分旧基准二进制不可用**：`.build-bento-wsl/bench_channel_opt`、`bench_channel_vs_queue` 因 ABI 漂移无法启动（`undefined symbol: go2cpp::Context::IsDone`），说明这些二进制早于当前库版本；只有 `go2cpp_fast_channel_bench` 可跑（数据见上表）。
- **只读约束**：本次未修改任何源码/测试；未新建构建目录。报告之外的结论如需实验验证，需要另开任务。
