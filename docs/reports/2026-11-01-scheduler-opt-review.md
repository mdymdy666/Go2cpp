# 调度器 557ns → 300ns 只读优化审查

- 审查对象：`/UserData/CodexWorkSpace/Go2cpp`（WSL），构建目录 `.build-bento-wsl`（Release）
- 审查性质：**只读**。未修改工作树内任何实现/测试/文档文件；本报告与临时基准二进制只落在 `/tmp`、`/var/tmp`
- 基准：`tests/concurrency/bench_scheduler_task_opt.cpp`（4P/4M、单生产者、空任务体、N 轮）
- 关联：#task 池化后 557ns、Go 135ns；待消除 gap 分项由上层给出

---

## 0. 先校正测量口径（否则会优化错半边）

本次用两种方式复核了「557ns 到底花在哪」：

1. 在 `/var/tmp/bench_ab.cpp`（复制自现有基准，加 `rdtsc` 包住 `scheduler.spawn()`）测「提交侧」耗时；
2. 用 `callgrind` 对 1P/3000 任务做指令级剖面。

结论（4P/4M，TSC≈2.5GHz，宿主有噪声，取多轮）：

| 配置 | wall ns/task | 提交侧 rdtsc（cycles/task，≈ns） |
|---|---|---|
| 4P，默认（metrics=0，collect=256） | 508 / 523 / 534 / 565 / 633 / 640 / 648 | 1191–1434（≈476–574） |
| 4P，metrics=1 | 809 / 835 / 836 | 1924–1991（≈770–796） |
| 4P，terminal_collect_interval=0 | 911 / 986 / 1258 | 2169–2987 |
| 2P，默认 | 302 / 318 / 396 / 423 / 470 / 605 | 673–1370 |
| 1P，默认 | 559 / 690 / 699 | 823–1314 |

**关键事实：wall 与「提交侧」高度重合（±10%），说明基准的墙钟由单生产者线程的 `spawn()` 路径决定，worker 侧执行（Fiber 切换/回收）已被并行隐藏。**

推论：

- 表里的 `registry 50–70ns`、`runnable 30–50ns`、`Task 构造 30–40ns` 确在生产者路径上，属于瓶颈；
- `FiberBin 80–120ns`、`metrics 双 clock_gettime 40–50ns` 主要发生在 **worker** 侧。它们对「进程 CPU ns/task」影响很大（实测 worker 数越多 CPU 时间越夸张：1P≈1.0–1.3µs、2P≈0.9–1.7µs、4P≈2.2–2.6µs，全是空闲自旋放大的），但对当前 wall 的直接贡献要打折；
- 若目标指标是 wall 557→300，**必须优先杀提交侧的共享原子与 registry/分配的固定成本**；只改 FiberBin/metrics 无法达成。

另外两个反向证据：
- 打开 `collect_metrics` → **+280~300ns/task**（不是 40–50ns）：它每任务在 4 个 worker 上做 6–8 次跨核共享行 RMW，再加两次 `clock_gettime`。这条实测同时证明「跨核共享计数器在 4P 下每次约 30–40ns」，为下文 P1–P5 的收益估算提供标尺。
- 关掉 `terminal_collect_interval`（registry 永不回收）→ **+400ns/task**：`vector<shared_ptr<Task>>` 无界增长的扩容拷贝/缓存恶化代价极高，反证 registry 结构（P6/P7）的价值。

---

## 1. 逐点审查（18 点）

> 收益估算统一为「4P/4M 单生产者 wall ns/task」，区间按宿主噪声与复用率给。
> 难度：低=局部改逻辑；中=改数据结构或所有权；高=改公开 API/内存模型。

### P1 runnable 全局原子：改 per-P 分片 + 延迟聚合
- **位置**：`src/concurrency/scheduler.cpp:2846`（定义）、`:3573/:3594/:3658`（enqueue 增）、`:5241/:5330`（worker 减）、`:6087-6090`（空闲判定读）
- **现状**：每次入队/出队各一次 `fetch_add/fetch_sub`，`relaxed` 但仍是对**同一个全局缓存行**的 RMW。生产者与 4 个 worker 同时打这一行，4P 下每次 20–40ns；每任务 2 次。
- **建议**：
  1. 计数下沉到 `Processor`：`std::atomic<std::size_t> local_runnable`（生产者投递时加目标 P 的行；worker 出队减自己 P 的行）。
  2. 全局值改为**延迟聚合**：`maybe_grow/notify/shutdown/draining` 时按 P 求和（P≤12，O(P) 仅在低频路径）。
  3. 读侧只需「是否非空」时，用 per-P 的 `bool` 提示位（relaxed 读本地行），不区分精确值。
- **预期**：40–80ns（两次 RMW + 缓存行争用）
- **风险**：中。聚合值有延迟 → `maybe_grow` 可能晚一拍；用「本地计数为 0 且聚合为 0」双条件兜底；`shutdown` 的 draining 不能用近似值，必须走精确聚合。
- **难度**：中

### P2 next_processor 全局轮转：改 thread_local 轮转
- **位置**：`scheduler.cpp:2850`（定义）、`:3568`（外部直投取 P）、`:3652/:3957/:4039/:4319`（其它取 P）
- **现状**：每个外部任务一次 `fetch_add`，所有生产者共享一行；它只用于「平均分摊到 P」，精确性零要求。
- **建议**：`thread_local std::size_t t_rr` 本地自增（并混入线程 id 做起始偏移），彻底去掉共享 RMW；P 数变化时取模即可。
- **预期**：15–30ns
- **风险**：低。仅影响投递公平性；单生产者下就是「永远投同一个 P」，会更差——可加 `t_rr += 1` 后与「P 的队列长度」比较，或每 64 次才同步一次全局。
- **难度**：低

### P3 enqueue_maintenance 全局 fetch_add：改 TLS 采样
- **位置**：`scheduler.cpp:2851`（定义）、`:3701`（每任务取票）、`:3702-3715`（四次 `active_workers` 读 + 分支）
- **现状**：每任务一次全局 RMW，只为拿一个「每 64/1024 次做一次维护」的票。票价远高于它买到的维护。
- **建议**：`thread_local uint32_t t_maintenance_ticket`，本地自增；每 64 次才做一次 `maybe_grow` 判定（或由 worker 出队侧低频触发）。`active_workers` 的 3 次读合并为 1 次。
- **预期**：15–30ns
- **风险**：低。worker 补充延迟最多 64 个任务；`initial_workers_missing` 这种急事可在 `start()` 与 `spawn` 首次调用时强制检查一次。
- **难度**：低

### P4 s_next_g_id 全局 fetch_add：改 TLS 分段 ID
- **位置**：`scheduler.cpp:210`（定义）、`:760`（`Task` 构造函数初始化列表里）
- **现状**：**每个 Task 构造**一次全局 RMW，且构造发生在生产者线程，与 worker 争同一行。
- **建议**：`thread_local { GId base; uint32_t used; }`，每次预留 256 个 ID（一次 RMW/256 任务）；`GId` 仍单调，`registry_stripe = id & 15` 的分布不变（分段不破坏低位均匀性）。
- **预期**：10–20ns
- **风险**：低。ID 不再全局严格连续（调试点/断言若依赖连续性需改）；`IsTaskAlive` 按 id 查条带的假设仍成立。
- **难度**：低

### P5 live_tasks 计数：per-P 分片
- **位置**：`scheduler.cpp:2844`（定义）、`Task::prepare_enqueue` → `bind_live_counter_locked`（`:1085`）、`release_live_counter_locked`（`:1105`）、`:5815`（draining 判零）
- **现状**：首次入队 `fetch_add`、终态 `fetch_sub`（还带 `release/acq_rel`），每任务 2 次全局 RMW。
- **建议**：按「首次入队时的目标 P」选分片计数（生产者本地行），draining 时 O(P) 求和判零；或直接复用 P1 的 per-P 计数（把「未终态数」并入同一结构）。
- **预期**：20–40ns
- **风险**：中。分片选择必须与终态释放时的 P 解耦（Task 记录分片下标），避免跨 P 递减；draining 判零必须精确（先聚合再进入 collect）。
- **难度**：中

### P6 registry 强引用（shared_ptr + vector）：改侵入式链表/裸指针
- **位置**：`scheduler.cpp:2760`（`array<vector<shared_ptr<Task>>,16>`）、`:3494-3501`（首次入队 emplace_back）、`:2432-2465`（collect 压缩/erase）、`:4957+`（按 id 查条带）
- **现状**：每个 G 首次入队要 `emplace_back(shared_ptr)`（+1 控制块 RMW，可能触发扩容拷贝），回收时 `erase`（-1 并可能 dispose）；条带 vector 扩容会整体搬迁 shared_ptr（每个元素 2 次原子）。
- **建议（两级）**：
  1. **短期（不改语义）**：Task 内嵌 `prev/next` 裸指针，registry 改成侵入式双向链表（O(1) 插入/摘除），彻底去掉 vector 扩容与节点分配；链表只持**裸指针**，所有权仍由「队列节点/外部句柄」的 shared_ptr 承担，且用「仅在终态摘除」保证悬挂安全。
  2. **中期**：registry 持 `Task*` 并成为「非终态 G 的唯一所有者」；外部 `TaskPtr` 改为侵入式引用计数（`Task::m_refs`），不再需要 `enable_shared_from_this`。
- **预期**：40–80ns（含控制块 RMW 与扩容摊销）
- **风险**：中高。裸指针方案必须保证「摘除前不会有最后一个强引用消失」；链表节点与并发 collect 的线性化点要严格（条带锁内摘除）。
- **难度**：中

### P7 registry 条带锁与「无条件注册」：延迟注册/延迟摘除
- **位置**：`scheduler.cpp:2727-2728`（16 把 HotSpinLock）、`:3495`（首次入队抢锁）、`:3501`（发布）
- **现状**：每个首次入队的 G 都要抢一次条带锁（`exchange` 跨核 RMW），只为了把 shared_ptr 塞进 vector；绝大多数空任务根本不会 park，registry 对它们纯属额外成本。
- **建议**：
  1. 用「生产者线程固定条带 + 无锁 MPSC 段」替代锁：每线程一个待注册小批次，worker/collect 时合并（类似 Go 的 `allgs` 批量追加）。
  2. 更激进：**延迟注册**——只有 G 第一次 park/等待时才注册（此时才真正需要「挂起不被释放」的强所有权）；纯 run-to-completion 的 G 不进 registry。
  3. 摘除也可延迟：终态后由 collect 批量摘，减少锁内工作。
- **预期**：30–60ns
- **风险**：中高。延迟注册会改变 `IsTaskAlive/TaskTerminal/DebugSnapshotTasks` 的可见性语义（未注册的存活 G 查不到），需要在这几个查询里补「队列中可达」的判定，或接受「未注册=不可见」的文档化语义。
- **难度**：高

### P8 Task 构造：字段初始化与 `enable_shared_from_this`
- **位置**：`scheduler.cpp:759-765`（构造函数）、`scheduler.hpp:515`（基类）、`scheduler.hpp:700-810`（成员）
- **现状**：`Task` 有约 25 个原子/实例字段，构造时逐个 store；`enable_shared_from_this` 带来一个 `weak_ptr`（16B + 控制块指针），并把 Task 大小推到 704B（64B 对齐，池按 704B 取块）；`m_options` 按值拷贝。
- **建议**：
  1. 去掉 `enable_shared_from_this`（代码里 `shared_from_this()` 用得极少，改用显式 `shared_ptr` 传递或 registry 查找）；省 weak_ptr 与 `_M_weak_this` 的初始化。
  2. 热字段与冷字段拆成两个结构：热字段（state/queued/claim/fiber_ptr/queued 位）保证在同一缓存行，冷字段（failure/condition/options/emergency_next）另放，缩小构造与首次触碰面积。
  3. `m_options` 只存调度真正需要的 3 个字段（stack_size/task_class/fiber_bin_capacity），避免整个 `TaskOptions` 拷贝。
  4. 构造函数标 `noexcept` 并把能省的初始化器挪到 `static constexpr` 默认成员（编译期常量，不产生运行期 store）。
- **预期**：10–25ns
- **风险**：低（1/3/4）；中（2，涉及字段重排与并发可见性）。
- **难度**：低–中

### P9 FiberBin 复用路径：去 pthread mutex、per-P 槽
- **位置**：`src/concurrency/fiber.cpp:2491`（`AcquireForScheduler`）、`:2521`（`RecycleForScheduler`）、`:1570`（`reset_for_scheduler`）、`:1602`（`m_record->mutex`）、`:1646`（`m_failure_mutex`）、`:2359`（`initialize_context` 复用分支）
- **现状**：每任务复用要走 `reset_for_scheduler`：**两把 pthread mutex**（`m_record->mutex` 重置 FiberRecord、`m_failure_mutex` 清 failure），callgrind 里 `pthread_mutex_lock+unlock` 合计 12.4% 指令、调用数 5998 次 ≈ 2×3000，正是这两把锁；`AcquireForScheduler` 每次 177 指令、`initialize_context` 105 指令、`RecycleForScheduler` 58 指令。
- **建议**：
  1. `reset_for_scheduler` 用一次「整块重置」：把 FiberRecord 的可重置字段集中成 64/128B 的 POD 子结构，`memset` + 必要字段单独 store，**不加锁**（复用前提已经是 `use_count()==1` 且状态为 Completed/Failed，无并发观察者；把该前提写成 assert）。
  2. `m_failure` 只在非空时才加锁清理（或换 `HotSpinLock`）；空异常指针是绝大多数情况。
  3. FiberBin 从 TLS `vector` 改成「P 结构上的定长槽 + 计数」（`Processor::fiber_bin[k]`），避免 TLS 访问与 vector 边界检查；`Acquire/Recycle` 直接按 `t_processor_id` 索引。
  4. `initialize_context` 的复用判定（`usable_size()==m_stack_size && reserve_size()==expected && prepare_reuse()`）可缓存为「上次成功复用」的布尔位，正常路径只读一位。
- **预期**：40–80ns（worker 侧，主要降进程 CPU；wall 收益取决于 worker 是否成为瓶颈，4P 下约 10–30ns）
- **风险**：中。去锁依赖「无并发观察者」这一前提；`FiberRecord` 可能被外部观察者（debug dump、嵌套 Fiber 父链）读过，必须证明 `use_count()==1` 时无读者，或保留一把只在 DEBUG 构建启用的校验锁。
- **难度**：中

### P10 metrics 双 `clock_gettime`：默认关 + 采样 + rdtsc
- **位置**：`scheduler.cpp:5157`（`hot_stats`）、`:5688-5700`（两次 `steady_now_ns`）、`:289`（`steady_now_ns` 实现）、`:3701` 附近维护计数
- **现状**：`SchedulerConfig::collect_metrics` 默认 `false`（`scheduler.hpp:293`），基准默认不吃这笔；但打开后实测 **+280~300ns/task**（远超原估 40–50ns），因为除了两次 `clock_gettime`，还有 6–8 次跨核 metric RMW。
- **建议**：
  1. 保持默认关；把「每任务计时」改成**采样**（每 64 或 256 个任务测一次，用任务计数低位判断），计时源换 `rdtsc`（x86 约 20–30 cycles，比 `clock_gettime` 的 vDSO 60–100 cycles 便宜）。
  2. 所有 `metric_*` 累加改 per-worker 本地计数（worker 结构体里），快照时聚合，彻底去掉跨核 RMW。
  3. `emit_event` 的两次 `has_observer` acquire load 可合并为「每任务读一次到局部变量」。
- **预期**：默认配置下 0ns；开启时回收 250–300ns（对诊断性能回归很有价值）
- **风险**：低。采样会改变指标语义（由精确累计变近似），需在 `SchedulerMetrics` 文档标注。
- **难度**：低

### P11 worker 单槽（inbox / runnext）：TTAS 锁改原子 exchange
- **位置**：`scheduler.cpp:465-505`（`RunNextSlot`，`HotSpinLock` + `shared_ptr`）、`:5223`（`machine->inbox.take()`）、`:5232`（`runnext.take()`）
- **现状**：worker 每轮找活先 `inbox.take()` 再 `runnext.take()`，两次都是 `exchange` + `store` 的锁；空槽时仍然要写一次（跨核 RMW）。这两个槽在稳态下**绝大多数是空的**，纯属固定税。
- **建议**：单槽用 `std::atomic<Task*>` + `exchange(nullptr)`（取）/ `exchange(task)`（放，返回被挤出的），无锁、空槽只读不写；`shared_ptr` 的引用计数由「槽只持裸指针 + 池所有权」或「槽持 intrusive 引用」承担。
- **预期**：10–20ns（worker 侧；4P 下空槽 RMW 争用会明显）
- **风险**：低–中（裸指针需要所有权保证；先用 `atomic<shared_ptr>` 不存在，C++17 只能 intrusive 或锁）。
- **难度**：中

### P12 队列 push 的 by-value `shared_ptr`：改引用/移动
- **位置**：`scheduler.cpp:352`（`LocalRunQueue::push(std::shared_ptr<Task> task)`）、`:472`（`RunNextSlot::push` 同）
- **现状**：`enqueue` 里 `direct_processor.queue.push(task)` 会**拷贝**一次 shared_ptr（+1 控制块 RMW），函数返回时形参析构再 -1。每任务白付 2 次原子。
- **建议**：签名改 `push(const std::shared_ptr<Task>&)` 或 `push(std::shared_ptr<Task>&&)`；调用方已持有左值时用前者，move 语义处用后者。Cell 内部赋值仍是 move。
- **预期**：15–30ns
- **风险**：极低（纯签名/重载）。
- **难度**：低

### P13 P 环 Cell 的 `shared_ptr` 与 tail CAS：裸指针 + 批量发布
- **位置**：`scheduler.cpp:340-380`（`Cell{sequence, shared_ptr<Task>}`、`push` 的 tail CAS）、`:352-372`
- **现状**：每次 push 一次 tail CAS + 一次 `cell.task` 赋值（shared_ptr 拷贝/move 的引用计数）+ 一次 `sequence` release store；pop 同理。队列节点本身就是「registry 之外的第二份强引用」。
- **建议**：
  1. Cell 改裸 `Task*`，所有权由池 + registry/句柄保证（配合 P6/P16）；每次进出省 2 次控制块 RMW。
  2. 生产者侧批量发布 tail（本地攒 8/16 个再 CAS 一次），把 CAS 摊销；消费端保持逐条。
  3. pop 的 `sequence.store` 用 relaxed（已有 release 语义由 tail/head 承担时需论证）。
- **预期**：30–60ns
- **风险**：中高。裸指针要求「出队到执行期间的所有权」有唯一归属；批量发布改变可见性延迟（worker 可能看不到已入队但未发布的项），需要在 `notify` 前 flush。
- **难度**：中

### P14 原子内存序：逐项收紧
- **位置**：`scheduler.cpp:1298-1305`（`claim_from_queue` 的 `acq_rel` CAS）、`:363-368`（tail CAS relaxed 已好）、`:5345`（`incoming_nonempty_mask` release）、`:3601`（mask fetch_or release）
- **现状**：多数已用 relaxed/acquire 合理；`claim_from_queue` 的 CAS 用 `acq_rel`（成功序），实际上状态位从 Runnable→Running 没有「发布数据」需求，可用 `release` 成功序 + `acquire` 失败序。
- **建议**：逐条做「谁发布、谁消费」的表格化审计；`m_queued.store(false)` 之后只有本线程读，可 `relaxed`；`execution_claim.store(true)` 同理（由 state CAS 的 release 覆盖可见性）。
- **预期**：5–15ns（x86 上 acquire/release 免费，收益主要在 ARM；x86 的收益来自少一次 mfence 语义的编译器屏障与指令选择，较小）
- **风险**：低（但必须逐条论证）；错误会变成极难复现的丢唤醒。
- **难度**：中

### P15 TLS 模型：fiber 库仍在走 `__tls_get_addr`
- **位置**：`src/concurrency/fiber.cpp:279`（`thread_local FiberBin s_thread_fiber_bin`）、`s_current_fiber` 等；对照 `scheduler.cpp:210-215` 的 `GO2CPP_SCHED_HOT_TLS`（已 initial-exec）
- **现状**：callgrind 里 `__tls_get_addr` 占 2.34% 指令（107 指令/任务），来自 fiber 库用 general-dynamic 模型的 TLS 访问。
- **建议**：给 fiber.cpp 的热 TLS 加 `__attribute__((tls_model("initial-exec")))`（或编译该库时加 `-ftls-model=initial-exec`）；dlopen 场景需评估，但本项目是链接期依赖。
- **预期**：10–20ns（worker 侧）
- **风险**：低。若未来有 `dlopen` 使用方，initial-exec 会占静态 TLS 槽；可先用宏按构建类型切换。
- **难度**：低

### P16 与 Go 对比：能否「Task 无引用计数」
- **位置**：`scheduler.hpp:515`（`Task : enable_shared_from_this`）、`scheduler.cpp:3390/3409`（`spawn` 返回 `shared_ptr<Task>`）、`:3426`（`allocate_shared` + `TaskBlockAllocator`）
- **现状**：`spawn` 返回 `shared_ptr<Task>`，队列 Cell、registry vector、TLS 槽、`emergency_next`、`completion_notification_next` 全是 shared_ptr；每个任务至少 4–6 次控制块 RMW。Go 的 `newproc` 之所以只要 19 条指令，是因为 `g` 由 P 的 `gfree` 池与 `allgs` 拥有，**没有引用计数**，用户拿到的是 `*g`/`goid`，生命周期由运行时兜底。
- **建议（分层，不要一步到位）**：
  1. **近期**：`spawn` 返回侵入式 `TaskHandle`（8B 指针 + `Task::m_refs` 原子），取代 `shared_ptr`；省控制块分配、`enable_shared_from_this`、每次拷贝的两次指针追逐。语义与 shared_ptr 等价，改动可控。
  2. **中期**：`spawn` 返回 `GId`（弱句柄），强所有权只在「registry + 队列」；用户 API 提供 `Wait(GId)/Cancel(GId)/State(GId)`。这是 Go 的模型，但会**破坏现有 `shared_ptr<Task>` API**（大量测试与上层代码），必须走兼容层（`TaskHandle` 内部仍可 `shared_from_this` 式查表）。
  3. **长期**：Task 内存由 per-P 池拥有，回收条件 = 终态 + 队列无节点 + 无外部强句柄；用 epoch/hazard 或「池代际 + 句柄校验」防 ABA。**不建议**在没有 hazard 机制时用裸指针裸回收——一个滞留的 `shared_ptr` 就会 UAF。
- **预期**：40–80ns（近期方案）；中期方案额外 20–40ns
- **风险**：高（公开 API 兼容性、ABI、测试面）；近期方案风险中低。
- **难度**：高

### P17 Task 块池的跨线程归还：生产者/消费者 TLS 不对称
- **位置**：`scheduler.cpp:523`（`kThreadCacheCapacity=96`）、`:556`（`Acquire`）、`:572`（`Release`）、`:611`（`Refill` 的全局锁）
- **现状**：Task 内存在生产者线程分配、在 worker 线程的 collect 里释放 → 释放进入 **worker 的 TLS 缓存**，生产者下次分配要 `Refill`（全局 `m_mutex` + 批量搬 48 块）。即「分配热路径周期性吃一把全局锁 + 内存跨核弹跳」。
- **建议**：池改成 per-P/全局无锁栈（tagged pointer 或按 P 分片），或让 worker 把块批量还给「生产者所属池」。至少把 `Refill` 的批量从 48 提到 256，降低锁频率。
- **预期**：5–15ns（摊销）
- **风险**：低。
- **难度**：低–中

### P18 `maybe_grow` 每任务检查：事件驱动 + 降频
- **位置**：`scheduler.cpp:2194`（`maybe_grow`）、`:3701-3716`（每任务判定）、callgrind 里 `maybe_grow` 约 41 指令/任务
- **现状**：即使 worker 数已满足，每任务仍要读 `active_workers` ×3、`runnable` ×1、算 4 个布尔再判分支。
- **建议**：把「需要补 worker」变成事件：`active_workers` 下降（worker 退出）时置一个 `needs_grow` 原子位，`enqueue` 只读这一位（一次 relaxed load）；或沿用 P3 的 TLS 采样。
- **预期**：10–20ns
- **风险**：低。
- **难度**：低

### 附：enqueue 789 条指令的瘦身清单（跨 P3/P12/P18/P7）
- `prepare_enqueue`（`:1051`）目前是「加锁 + owner/gate 检查 + state 检查 + queued 检查 + live 计数」，其中 owner/gate 绑定只在首次需要：稳态可先做无锁快路径（`state==Runnable && !queued && !claim` 的 CAS），失败再走慢路径。
- `registered()` 的 `acquire` load（`:1017`）+ 首次注册锁可以合并进同一次条带锁（一次锁做「注册 + 发布」）。
- `accepting()/stopping()/draining()` 三次 `lifecycle_flags` acquire load（`:2790-2801`）在 enqueue 里出现 2 次，可合并为一次读 + 位测试。
- 目标是 enqueue 独占指令从 ~319（callgrind 实测）降到 <200，且跨核 RMW 从 ~6 次降到 2 次。

---

## 2. 「557 → 300」组合方案

> 前提：**先确认指标口径**。若目标是 wall（当前 508–648，中位 ~570），按下面三阶段推进；若目标是进程 CPU ns/task，则 P9/P10/P15/P11 的权重上升，wall 阶段可后置。

### 阶段 1：零语义风险（预计 −90~170ns → 400–480）
1. P2 `next_processor` → TLS 轮转
2. P3 `enqueue_maintenance` → TLS 采样（64 次）
3. P4 `s_next_g_id` → TLS 分段 ID（256/段）
4. P5 `live_tasks` → per-P 分片（draining 精确聚合）
5. P12 队列 push 签名去拷贝
6. P18 `maybe_grow` 改事件位
7. P10 metrics 保持默认关（顺手把开启成本降到采样 + rdtsc）

验证：`bench_sched_opt` ABBA 交替 5 次取中位；`ctest -R scheduler|concurrency` 全绿；TSAN 跑 `stress_task_churn`。

### 阶段 2：局部结构改动（预计 −80~150ns → 300–400）
1. P1 `runnable` → per-P 分片 + 延迟聚合
2. P9 FiberBin reset 去两把 pthread mutex + per-P 槽
3. P11 单槽 TTAS → 原子 exchange
4. P13 队列 Cell 裸指针（或先只做批量 tail 发布）
5. P15 fiber 库 TLS initial-exec
6. P17 Task 池归还路径/批量大小
7. P8 Task 字段冷热拆分 + 去 `enable_shared_from_this`

### 阶段 3：所有权重构（预计 −50~100ns → 250–330）
1. P6 registry 侵入式链表（先做，无 API 变更）
2. P7 registry 延迟注册 / 无锁条带段
3. P16 `TaskHandle` 侵入式引用（可选，取决于 API 兼容预算）

**收敛判断**：阶段 1+2 的关键项全部落地（尤其 P1/P5/P9/P12/P13）即可进入 350–400；要达到 300 必须叠加 P6/P7（registry 是提交侧剩下的最大固定成本），并且 `Task` 构造与队列节点不再各付一次控制块 RMW（P13/P16）。**只做 worker 侧的 FiberBin 无法达成 wall 目标**——这是本次审查最重要的校正。

---

## 3. 与 Go 的对照结论

| 维度 | Go `newproc` | 当前实现 | 可借鉴动作 |
|---|---|---|---|
| G 所有权 | P 的 `gfree` 池 + `allgs`，无引用计数 | `shared_ptr` + registry 强引用 | 池所有权 + 侵入式引用（P16） |
| 入队 | `runqput` 写本地环/runnext，无全局计数 | 5 个全局共享 RMW | P1–P5 |
| 注册 | 全量 `allgs` 只在 GC/race 用，不逐 G 加锁 | 每 G 抢一次条带锁 | P7 延迟注册 |
| 唤醒 | runnext 单槽原子 CAS | TTAS 锁 + shared_ptr | P11 |
| 栈 | `stackalloc` 从 P 的 stackcache | TLS FiberBin + 两把 mutex | P9/P15 |

结论：**方向一致（per-P 化 + 去引用计数），但不能照抄「无引用计数」**——C++ 的公开 API 返回可长期持有的句柄，必须有某种保命机制；正确路径是「侵入式引用（近期）→ 弱句柄 + 池所有权（长期）」，而不是裸指针裸回收。

---

## 4. 验证清单（给实施方）

1. **口径**：wall 与 CPU ns/task 分开报；固定 `taskset`、固定轮数、5 次中位；记录 `submit` 分段（rdtsc）与 `runnable/registry` 计数。
2. **正确性**：`ctest -R 'scheduler|concurrency|fiber'`；`stress_task_churn_1e8` 长跑；`shutdown` 超时用例；TSAN 下 park/wake 往返。
3. **不变量断言**（建议在 debug 构建加）：
   - 任一 G 在「Runnable + 有队列节点」与「无节点」之间不存在第三种状态；
   - registry 里的每个裸指针在摘除前仍被至少一个强引用持有；
   - `runnable` 聚合值在 `notify_worker` 前不为 0 时必然能找到节点。
4. **回滚**：P1/P5 的 per-P 计数保留「编译期开关」，出问题可切回全局原子。

---

## 5. 附：本次实测命令

```bash
# 现有基准（与仓库一致）
g++ -O2 -DNDEBUG -std=c++17 -Iinclude tests/concurrency/bench_scheduler_task_opt.cpp \
  -L.build-bento-wsl -Wl,-rpath,$PWD/.build-bento-wsl \
  -lgo2cpp_scheduler -lgo2cpp_fiber -lgo2cpp_error -lgo2cpp_debug -lgo2cpp_debug_event \
  -lpthread -o /var/tmp/bench_sched_opt
taskset -c 2,3 /var/tmp/bench_sched_opt 4 100000 3

# A/B（metrics / registry 回收 / 提交侧 rdtsc）：/var/tmp/bench_ab.cpp
taskset -c 2,3 /var/tmp/bench_ab 4 100000 3 0 256   # 默认
taskset -c 2,3 /var/tmp/bench_ab 4 100000 3 1 256   # metrics=1
taskset -c 2,3 /var/tmp/bench_ab 4 100000 3 0 0     # 关闭 registry 周期回收

# 指令级剖面
valgrind --tool=callgrind --callgrind-out-file=/tmp/cg_sched.out \
  /var/tmp/bench_sched_opt 1 3000 1
callgrind_annotate --threshold=80 /tmp/cg_sched.out
```

> 本次审查未修改工作树任何文件；临时源码/二进制在 `/var/tmp`，报告在 `/tmp`。
