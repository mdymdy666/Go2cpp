# 01 concurrency 答卷（15 题）

> 环境：WSL2 / AMD Ryzen 9 8945HX / 12 vCPU / GCC 13.3；除注明外均未绑核，loadavg 0.35。
> 所有行号均用 `sed -n` 现场核对；题面第 Q15 处行号有误，已在 Q15 标注。

## Q01

**答**：两个口径量的不是同一区间。`wall` 是「main 提交 N 个任务 → 全部完成」的端到端（含生产者入队、调度延迟、worker 执行），`cpu` 是进程 CPU 时间，4 个 worker 并行时自然数倍于 wall；history 的 122.0 是 wall、326.7 是 submit 侧单段（rdtsc），两者**不能相减、不能互换**。本机能复现出 wall 225–292 ns，复现不出 122：① 机器/核数不同（122 是 8–11 核、4P/4M、单生产者、空任务体、`collect_metrics=false`、`terminal_collect_interval=256` 的特定条件）；② 预热与宿主负载（本机 5 轮逐轮下降 287→225，说明首轮含页错误/频率爬升）。对外只能报「口径 + 条件 + 核数/绑核 + 轮数 + min/median」，禁止单报历史数字。

**代码证据**：
```text
src/concurrency/scheduler.cpp:6041-6043
  6041  const bool collect_metrics = hot_stats;
  6042  const auto run_started = collect_metrics ? steady_now_ns() : 0;
tests/concurrency/bench_scheduler_task_opt.cpp:102-105
  102  std::printf("[round %d] cpu %.0f ns/task  wall %.0f ns/task ...
```

**自证**：`g++ -O2 -DNDEBUG -std=c++17 -Iinclude tests/concurrency/bench_scheduler_task_opt.cpp -L.build-bento-wsl -Wl,-rpath,$PWD/.build-bento-wsl -lgo2cpp_scheduler -lgo2cpp_fiber -lgo2cpp_error -lgo2cpp_debug -lgo2cpp_debug_event -lpthread -o /tmp/bench_scheduler_task_opt`
- `taskset -c 2,3 /tmp/bench_scheduler_task_opt 4 100000 5` → wall 287/239/267/236/225 ns，cpu 619/519/582/514/478 ns（中位 wall 239、cpu 519）。
- 不绑核同参数 → wall 292/375/355/404/426，cpu 1526/1987/1883/2168/2217（CPU 争用使 cpu 口径恶化 3–4×）。
- `taskset -c 2,3 ... 1 100000 5` → wall 396/530/294/266/267，cpu 799/1110/632/545/560。

**我没答出的**：submit 单段口径（历史 326.7 ns）本轮没有单独测，无法验证「122 与 326.7 差 2.7 倍」；历史报告里 0.98× 的换算没有复核。

## Q02

**答**：单槽 `runnext` 是「最近一次唤醒/刚 ready 的 G」的 LIFO 快速槽：最新被唤醒的 G 最可能马上可运行，放最前面命中率最高；它不经过 256 槽环，也不会在环中间被别的 M 偷走。槽满时把旧任务挤到**同一 P 的环尾**（容量不变，只改优先级）。`yield` 必须放环尾：若持续 yield 的 G 占住快速槽，park 后被唤醒的 G 会被挤进 256 槽环而长期轮不到（1P/1M Join 饿死）。实现上 C++17 没有 `atomic<shared_ptr>`，用 TTAS 自旋锁：无争用时一次 exchange + 两次指针搬运，比环的 CAS 短。

**代码证据**：
```text
src/concurrency/scheduler.cpp:577-584
  577  * @brief 单槽快速队列（Go `runnext` / 定向邮箱的等价物）。
  581  * Go 1.3 `runqput(..., next=true)` 把刚唤醒/刚 yield 的 G 放进 `p.runnext`
  582  * 单槽：下一次 `runqget` 直接取它，不经过 256 槽环，也不会被别的 P 在
  583  * 队列中间抢走。
src/concurrency/scheduler.cpp:6128-6132
  6129  // 若把 yield 的 G 放进 runnext，持续 yield 的 G 会一直占住快速槽...
```

**自证**：`ctest -R 'go2cpp_scheduler_smoke|go2cpp_scheduler_queue_latency'` → 2/2 passed；`go2cpp_scheduler_queue_latency` 实测空队列新任务 p50=35.2 µs，单 P 积压 2000 时 p50=311.1 µs（≈8.8×），说明「插队快速槽」对尾延迟有实际价值。

**我没答出的**：没有单独写「A 持续 yield、B park→wake」的饿死复现程序（只跑了项目自带 queue_latency）。

## Q03

**答**：`fiber_stack_size=2048` 是**请求量**，页对齐后 `initial_usable = max(page_align_up(2048), 4096) = 4096`，实际只提交 1 页；其余是 8 MiB `PROT_NONE` + `MAP_NORESERVE` 的 **VA 预留**，不是物理内存。10 万 G 的 VA 预留理论上 800 GiB（本机实测 VmSize 只显示 98 GiB，因为相邻 PROT_NONE 映射被内核合并；RSS 103 MiB ≈ 1 KiB/G）。真正容量上限不是内存，而是 `vm.max_map_count`（本机 1048576，足够；默认 65530 的机器约 3 万 G 触顶）。栈溢出由 SIGSEGV handler 救场，四个拒绝条件：取指 fault、fault 不在未提交区、rsp 不在预留区、距离 >256 KiB；退化路径（`stack_growable=false`/reserve≤初始提交/非 Linux-x86_64/`GO2CPP_GROWABLE_STACK=0`）**不安装 handler**，爆栈直接杀进程。

**代码证据**：
```text
include/go2cpp/concurrency/scheduler/scheduler.hpp:256-264
  256  // Fiber 栈的初始提交量（字节）。默认 2048（Go 的 2KB 初始栈）；
  258  //   initial_usable = max(page_align_up(requested), 1 * page)
  259  // 请求 2048 时实际初始可用 4096 字节（1 页）...
  262  // 每个 Fiber 预留的虚拟地址空间（字节），默认 8MiB...
src/concurrency/fiber.cpp:1054-1056
  1054  // reserve == initial_usable（或只多出不足一个增长块）按固定栈处理。
  1055  // 固定栈路径不安装 SIGSEGV handler，也不分配 sigaltstack。
```

**自证**：自写 `/tmp/fiber_lifecycle N`（4 worker，N 个 Fiber 阻塞在无缓冲 channel）：
- N=10000：spawn 6.92 ms，parked VmSize 77.3 GB / RSS 53.0 MB，shutdown 49.32 ms
- N=50000：spawn 28.79 ms，VmSize 83.1 GB / RSS 73.5 MB，shutdown 59.50 ms
- N=100000：spawn 55.54 ms，VmSize 98.3 GB / RSS 103.3 MB，shutdown 74.08 ms
- `sysctl vm.max_map_count` = 1048576；`ulimit -v` = unlimited（本机未触顶）。

**我没答出的**：没有在 `vm.max_map_count=65530` 的机器上复现「约 3 万 G 失败」，也没有做 `stack_growable=false` 的爆栈崩溃实验。

## Q04

**答**：`FastChannel` 的 Vyukov ring 只在「1P1C、有缓冲、无 close/select/cancel」的稳态里省掉同地址 RMW；一旦要支持 close 排空、select 多路、context 取消、等待者登记，就必须让缓冲与等待者共享同一线性化点。通用 `Channel<T>` 单锁的设计正是为此：select 的「探测-回滚」、等待者直接交接、Close 与缓冲必须同一临界区；任何一处拆开都会出现「不崩溃、只丢值」的偶发 bug。`fast_channel.hpp` 文件头写明它是**实验版（/tmp 专用，不入库）**；`Cap==1` 时 Vyukov 协议退化（生产者绕回同一槽前消费者没有独立 seq 状态），必须特化回锁版。ABA 不是理由——Vyukov 用每槽 seq 解决了 ABA。

**代码证据**：
```text
include/go2cpp/concurrency/channel.hpp:1192-1196
  1192  // 注：所有操作由 Channel::m_mutex 串行化...
  1193  // 「分离的生产/消费锁」与「无锁索引」：单线程路径确实更快，但 select 的
  1194  // 探测-回滚、等待者直接交接都要与缓冲共享同一临界区...
include/go2cpp/concurrency/fast_channel.hpp:2
  2  * @file concurrency/fast_channel.hpp  —— 实验版（/tmp 专用，不入库）
include/go2cpp/concurrency/fast_channel.hpp:1537
  1537  // FastChannel<T, 1>：单槽特化（Vyukov ring 在 Cap == 1 时协议退化）
```

**自证**：`.build-bento-wsl/go2cpp_fast_channel_bench`（1P1C，taskset 2,3，5 轮，total=200000）：
- 线程：fast-cap64 10.23 ns（min 8.95）、fast-cap1024 16.07（min 13.15）、spsc-cap1024 17.92、fast-cap0（阻塞路径）643.30
- Fiber：fast-cap1024 6.23（min 5.82）、fast-cap64 9.51、fast-cap0 137.70
- `ctest -R 'go2cpp_channel_concurrency|go2cpp_select_lockorder_test|go2cpp_fiber_select_test'` → 3/3 passed。
- 通用 `Channel<int>` 的同口径 bench（`bench_channel_opt`）运行失败：`undefined symbol: _ZNK6go2cpp7Context6IsDoneEv`（旧二进制与新 `.so` ABI 不匹配），**没有拿到通用 Channel 的 ns/条**，不引用文档 10.73。

**我没答出的**：通用 `Channel<int>(1024)` 与 `FastChannel<1024>` 的同负载直接对比（旧 bench 二进制 ABI 失效，未重编）。

## Q05

**答**：长尾根因是「投递方与唤醒方错配」：G 被投进 P 的 `runnext` → `notify_one()` 唤醒的是 CV 队首（不一定属于该 P）→ 被唤醒的 M 查自己环、全局队列、再偷其他 P 的**环**（都不看 runnext）→ 没活干回去睡 → owner M 睡到 `idle_wait`（10 ms）超时。所以中位长尾≈`idle_wait`。修复：本 P 有运行中的 M 才投 runnext，否则投本地环（任意 M 可窃取）；空 P 路径改 `notify_all_workers()`。另一条同类窗口是 idle poller：必须先登记 `io_waiting_workers` 再复查可运行计数，否则 notify 落在「检查完、还没进 epoll_wait」之间就丢。实测已无 >1 ms 长尾，且 p50 不随 `idle_wait` 变化（10 ms 与 2 ms 都是 ~16 µs），说明修复生效。

**代码证据**：
```text
src/concurrency/scheduler.cpp:4162-4169
  4162  // ② Go 式（默认）：投到目标 P 的 runnext...
  4167  // runnext 是只有本 P 的 owner M 会消费的槽：worker 的窃取路径明确
  4168  // 跳过别的 P 的 runnext...
src/concurrency/scheduler.cpp:4043-4045
  4043  if (running == 0) {
  4044  // 本 P 没有运行中的 M ⇒ owner M 正在睡，而 runnext 只有 owner 会消费...
```

**自证**：`STARTUP_COST_OUT_DIR=/tmp/exam-build tools/startup_cost/build.sh` → `/tmp/exam-build/startup_cost_park_wake --mode runtime --iters 200 --calibrate`：
- 200 样本：TOTAL p50 16.16 µs / p99 104.24 µs / max 117.28 µs，`>1ms=0`；B 段（wake→worker-run）p50 15.93 µs。
- 1000 样本：TOTAL p50 17.88 µs / p99 70.45 µs / max 471.67 µs，`>1ms=0`。
- `--idle-wait-ms 2`：TOTAL p50 16.18 µs / p99 63.19 µs；`--idle-wait-ms 10`：p50 16.47 / p99 80.45 —— **p50 不随 idle_wait 变化**。
- ctest `go2cpp_scheduler_smoke` passed。

**我没答出的**：没有在修复前代码上复现 9.67 ms 长尾（历史 commit `49223be` 未回退验证）。

## Q06

**答**：顺序是「锁成本从低到高」：① `inbox`（M 定向槽，无锁）→ ② 本 P `runnext`（owner M 无锁取）→ ③ 本地 256 槽环（只碰 P 队列锁，不碰调度器总锁）→ ④ incoming 16 条带（按条带锁 + 轮转起点 + 空条带掩码，避免固定顺序饥饿）→ ⑤ admission 总锁（emergency → 本地 FIFO → 全局 FIFO → 窃取）→ ⑥ 都没有才进 netpoll/idle poller。netpoll 只在「无 G 可跑 + 常驻 M + 未开 allow_idle_shrink + 存在阻塞区」时启用；先登记再复查堵住丢唤醒窗口。窃取不能提前：源码记录过「把窃取移出全局锁 −34%」，本地队列边界不要动。

**代码证据**：
```text
src/concurrency/scheduler.cpp:5558  // step0: 快速槽——本 M 的定向槽...与本 P 的 runnext
src/concurrency/scheduler.cpp:5602  // step2: 快路径——只碰本 P 的本地队列，不加调度器总锁
src/concurrency/scheduler.cpp:5641-5644  // step3: 中路径——扫描 incoming 分条队列...轮转扫全部 stripe
src/concurrency/scheduler.cpp:5767-5769  // step4: 慢路径...emergency → 本地 FIFO → 全局 FIFO → 窃取队尾
src/concurrency/scheduler.cpp:2009  // 「把窃取移出全局锁」的尝试反而 -34%
```

**自证**：`ctest -R 'go2cpp_scheduler_smoke|go2cpp_scheduler_queue_latency'` → 2/2 passed；queue_latency 空队列 p50 35.2 µs vs 积压 2000 时 311.1 µs（本地队列/快速槽确实影响排队延迟）。park_wake 的 B 段 p50 15.93–17.44 µs（200/1000 样本）。

**我没答出的**：没有逐级关闭（例如禁用 incoming 分条）做 A/B 归因。

## Q07

**答**：sysmon 是**观测器**不是抢占器：默认 10 ms 一拍、`long_syscall_threshold=50 ms`，只处理已声明的 `MState::kBlocking`（BlockingRegion/Hook 边界）；发现超时只「把 P 从该 M detach + 申请补充 M」，原 M 的栈与 G 都不动，返回时恢复计数、让补充 M 退休。不能抢占的原因：协作式调度没有安全点，强行切断任意 C++ 调用栈会跳过 RAII、泄漏锁；未声明阻塞对 sysmon 完全不可见。`try_to_lock` 失败不算「本轮扫描完成」，会置 `sysmon_retry_pending` 并把下一周期缩到 1 ms。BlockingRegion 三条契约：只能在有 G 的 M 上进入、对象不可移动（移动会让析构发生在别的线程）、不能跨 Fiber yield/park 或任务边界（否则 `std::terminate()`）。

**代码证据**：
```text
src/concurrency/scheduler.cpp:2498  void sysmon_pass(Scheduler* scheduler) noexcept {
src/concurrency/scheduler.cpp:2560-2564  // sysmon 的节拍等待不能争用调度器主锁...try_to_lock...
src/concurrency/scheduler.cpp:5045-5051  BlockingRegion::BlockingRegion(...) { if (m_scheduler == nullptr || ... ) return; }
include/go2cpp/concurrency/scheduler/scheduler.hpp:279-281
  279  // Go sysmon 风格的阻塞监控。它只观察已声明的 BlockingRegion/Hook
  280  // 边界，不会从另一个线程强行切断任意 C++ 调用栈。
src/concurrency/scheduler.cpp:6101-6106  // 仍有活动标记...宁可在边界处 fail-fast...std::terminate()
```

**自证**：`ctest -R go2cpp_scheduler_smoke` → passed（0.03 s，含 BlockingRegion 用例）。未单独写 `sleep(1)` + BlockingRegion 的 worker 增减观测程序。

**我没答出的**：没有实测「detach 后补充 M 数量变化、返回后退休」的时序。

## Q08

**答**：不能直接释放栈，因为已启动的 G 的栈帧还活着，释放会跳过栈上 RAII。顺序：置 draining / `set_accepting(false)` → `begin_shutdown_locked`（未启动 G 原地取消；已启动 G 保持可运行，让 Fiber 从 trampoline 返回、跑栈上析构）→ 等 `live_tasks_total()==0`（O(1) 原子计数）→ 外部 owner 逐个 join worker（worker 自己不 join 自己）→ 归零后才 `finish_draining_locked()` 做一次全表确认。sysmon 在 draining 阶段保持运行，终态归零后才停。旧 O(n²) 根因：每完成一个 G 就持 admission 锁做 `collect_terminal` + `finish_draining` 两次全表扫描，G 完成数 × 注册表大小；修复是 `live_tasks` 原子计数（首次入队 +1、四个终态转移点 −1，与状态同临界区），归零后才全表确认。超时不能强释放栈：worker 仍拥有栈，强释放跳过 RAII；有界 shutdown 超时只返回 false 并保持 draining。

**代码证据**：
```text
src/concurrency/scheduler.cpp:2756-2758
  2756  // Unstarted Gs have no suspended stack and can be cancelled in place.
  2757  // Started Gs must remain runnable so their Fiber can return through its
  2758  // trampoline and execute stack-local destructors.
src/concurrency/scheduler.cpp:3444-3446  set_accepting(false); set_draining(true); begin_shutdown_locked(...)
src/concurrency/scheduler.cpp:3505-3509  // O(1) 快路径...计数归零后再由 finish_draining_locked() 做一次全表确认
src/concurrency/scheduler.cpp:6170-6174  // 旧实现在这里每完成一个 G 就扫两遍注册表...10 万 G 的 shutdown 因此退化成 O(n²)。
```

**自证**：自写 `/tmp/fiber_lifecycle N`（N 个 parked G 后 shutdown）：
- N=10000 → 49.32 ms；N=50000 → 59.50 ms；N=100000 → 74.08 ms。
- 从 1 万到 10 万增量 24.76 ms / 9 万 G ≈ 0.275 µs/G，近似线性（旧 O(n²) 在 10 万应到数十秒量级）。

**我没答出的**：没有把 N 拉到 100 万验证线性外推，也没有对比修复前代码。

## Q09

**答**：`execution_claim` 是执行权（谁在跑这个 G），`run_claim` 表示「`run()` 栈帧尚未退出」；两者必须在同一临界区一起翻转。只放 `execution_claim`：新执行者进入 `run()` 时会被 `run_claim` 拒绝并静默返回，G 永久停在 `kRunning` 无人认领，shutdown 等不到终态。`run_claim` 失败要回滚为 Runnable 并 `deferred_enqueue`。状态字低 8 位是 `GState`、bit8 是 wake 令牌：park 要求整个字「Running 且无令牌」才 CAS 成 Waiting，wake 对 Waiting 改状态、对 Running 只置令牌；同一个 32 位字用 CAS 竞争，不存在「令牌已发布、挂起已提交、却没人消费」的窗口。`claim_from_queue` 单 CAS：Runnable→Running 本身就是执行权唯一仲裁点，先抢 claim 再改状态会多一次 RMW。

**代码证据**：
```text
src/concurrency/scheduler.cpp:1916-1918
  1916  // execution_claim，再在 run() 入口被 run_claim 拒绝并静默返回，
  1917  // 把 G 永久留在 kRunning。这两个位共同描述"谁在执行这个 G"，
  1918  // 必须原子地一起翻转。
src/concurrency/scheduler.cpp:1489-1494  // 只做一次 CAS：Runnable -> Running 本身就是执行权的唯一仲裁点。
src/concurrency/scheduler.cpp:1354-1355  // 状态与令牌在同一个 32 位字里，wake 与 park 各自用 CAS 竞争同一个字...
```

**自证**：`ctest -R 'go2cpp_scheduler_smoke|go2cpp_scheduler_queue_latency'` → 2/2 passed。TSAN 构建本轮未跑。

**我没答出的**：没有用 TSAN 复跑 park/wake 往返来验证「同一 G 两个执行者」不出现。

## Q10

**答**：分片至少三类：任务注册表 16 条带（`kRegistryStripeCount`）、外部生产者 incoming 16 条带（`kIncomingStripeCount`）、runnable 按 P 分片（每个 P 的 256 槽本地环 + `active_machines/running_machines` 原子提示位）。目的：把全局原子 RMW 改成生产者本地 cacheline，减少跨核迁移与单锁争用。代价：精确总量要 O(P) 跨核聚合，热路径不能每任务读全局；所以维护频率固定 64 拍。draining 判零必须精确：少算一个存活 G 会提前停 worker、丢未完成 Fiber，因此用与状态同临界区的 `live_tasks`，不是分片求和。

**代码证据**：
```text
src/concurrency/scheduler.cpp:1993-1996  kRegistryStripeCount = 16; kIncomingStripeCount = 16;
src/concurrency/scheduler.cpp:6008-6011  // 分片后精确聚合要 O(P) 次跨核 load...因此固定 64 拍
src/concurrency/scheduler.cpp:6204-6206  // P1 后这里读本 P 分片（本核缓存行）而不是全局聚合
src/concurrency/scheduler.cpp:3508-3509  return m_impl->live_tasks_total() == 0;
```

**自证**：`taskset -c 2,3 /tmp/bench_scheduler_task_opt 4 100000 5` → wall 287/239/267/236/225；`... 1 100000 5` → wall 396/530/294/266/267。P=4 的尾轮（225）明显低于 P=1（267），但首轮更差（287 vs 396 的抖动说明首轮受预热影响）。未用 `perf stat -e cache-misses`。

**我没答出的**：没有做「分片前后」的 A/B（需要改代码），也没有跨核 cacheline 计数。

## Q11

**答**：等待队列用侵入式双链表 + 每等待者节点自带 32 位 futex 字：节点由等待者栈帧持有，Channel 队列只持裸指针，入队/出队/摘链都在 Channel 自旋锁内；`signal` 用 CAS 产生单一赢家（kReady/kClosed/kCancelled），交接方即使遇到已取消节点也只需 pop 跳过、不需要解引用已销毁对象。为什么 per-node futex：共享唤醒字会唤醒错节点（审计 D2），每节点自己的字 + `FUTEX_WAKE(1)` 才能精确唤醒。`NotifyChange()` 必须锁外，否则锁序反转；值只在交接成功后移动。节点池化上限 128，稳态每次阻塞收发 0 次堆分配。

**代码证据**：
```text
include/go2cpp/concurrency/channel.hpp:772-775
  772  // 1. **等待队列归 Channel 所有**：节点是侵入式双链表，队列只持裸指针；
  773  //    入队/出队/摘链全部在 Channel 自旋锁内完成...
  775  // 2. **节点池化**：节点从线程本地空闲链取还（上限 128...）
include/go2cpp/concurrency/channel.hpp:2757-2763
  2757  // step3: 优先交接给等待中的接收者。等待队列是侵入式链表，摘链 O(1)；
  2763  // 已被取消/超时：取消回调不摘链，这里清理。
include/go2cpp/concurrency/channel.hpp:2727-2728  // NotifyChange() 必须在锁外...
```

**自证**：`ctest -R 'go2cpp_channel_concurrency|go2cpp_select_lockorder_test|go2cpp_fiber_select_test'` → 3/3 passed；`go2cpp_sync_cancel_stress` → `[sync-cancel-stress] OK iterations=5000`。

**我没答出的**：没有测量「稳态 0 次堆分配」的分配计数（需要 malloc hook / perf）。

## Q12

**答**：「两态」指**正常模式**与**饥饿模式**两个状态机模式；`kLocked/kWaiter/kWoken/kStarving` 是状态字里的 4 个标志位，不是 4 个互斥状态。正常模式：Unlock 先清 `kLocked` 再唤醒一个等待者，被唤醒者回顶部重新 CAS，新来者可插队；`kWoken` 保证同时最多一个被唤醒者。饥饿模式：等待超过 1 ms 置 `kStarving`；Unlock **不释放** `kLocked`，把所有权直接交给 FIFO 队首，新来者只能入队。为什么必须原子 RMW：`TryLock` 快路径 CAS 不取 `m_mutex`，锁内用旧快照 `load+store` 会覆盖刚写入的 `kLocked`，导致两个执行流同时持锁（项目真实发生过：3 秒 9–34 次解锁异常、629–1683 次双持）。退出条件：被交接者自身未超阈值会清 `kStarving`；队列清空也退出。

**代码证据**：
```text
src/concurrency/sync.cpp:1276-1279
  1276  * 原子 RMW**：只影响 `kLocked` 的 CAS（保留其余提示位）...
  1278  * `load` + `store`——超时/取消清理曾因此抹掉并发 `TryLock` 写入的
  1279  * `kLocked`，令两个执行流同时持锁（P1，2026-10-30 修复）。
src/concurrency/sync.cpp:1539-1546  // ## 两态状态机（正常模式 / 饥饿模式）...饥饿模式：Unlock 不释放锁...
src/concurrency/sync.cpp:2027-2029  if (kStarving) { // 饥饿模式：kLocked 不释放，所有权直接交给 FIFO 队首
```

**自证**：`go2cpp_sync_cancel_stress` → `OK iterations=5000`（取消/超时路径不崩、不报 logic_error）。未写「A Lock/Unlock + B TryLock」的 3 秒双持压测。

**我没答出的**：没有统计 `mutex_held_park_total` / 双持计数来量化修复前问题。

## Q13

**答**：取消/超时节点**不从队列中间摘链**：节点结果用 CAS 单赢家，队列没有反向索引，取消回调不能安全解引用队列外节点；交接方只能从头逐个 pop、跳过僵尸节点。O(n²) 构成：单次交接最坏 O(n)（跳过所有僵尸）× 每次 Unlock 都做 `ReorderWaitersForSelector` 全队列重排 × n 次取消/超时。后果是大量超时等待者时 Unlock 变慢、尾延迟放大；持队列锁做长扫描还会推迟所有等待者。修复方向：反向索引/句柄（取消时 O(1) 摘链）、惰性批量清理、把「跳过僵尸」移出交接热路径。节点内部锁不能用 `std::mutex`：Wake 可由任意线程调用，跨线程解锁是 UB，必须用无 owner 的 FutexMutex。

**代码证据**：
```text
src/concurrency/sync.cpp:1372-1373  void DropWakeToken() noexcept { std::shared_ptr<WaitNode> selected;
src/concurrency/sync.cpp:1386-1390
  1386  while (!m_waiters.empty()) { ... if (candidate && !candidate->managed()) { ... } }
src/concurrency/sync.cpp:1120-1122
  1120  /// 让选择器重排等待队列：**只把被选中的节点换到队首**，不改认领语义。
  1122  void ReorderWaitersForSelector(...)
src/concurrency/sync.cpp:2088-2091  // 正常模式：先摘除一个等待者并置唤醒令牌...
src/concurrency/sync.cpp:2133  // step4: 锁外唤醒（与 LockImpl 的 park 路径配对；持队列锁唤醒会死锁）。
```

**自证**：`go2cpp_sync_cancel_stress` → OK iterations=5000。题面要求的 N=1k/5k/10k Unlock 超线性压测**未跑**。

**我没答出的**：没有实测 O(n²) 曲线（需要自己写等待者+超时+Unlock 的压测）。

## Q14

**答**：旧实现 `probe()` 命中「已关闭且缓冲为空」返回 `kClosed`，调用方按习惯 break，另一个通道的缓冲值再无人取（D1 丢值）。新规则：`kReady` 立即返回（recv 已取走值 / send 已发出值，传输已发生，回滚会丢值）；`closed/nil/invalid` 无副作用，暂存到本轮扫描结束，只有确实没有可交付值/可完成发送时才返回暂存的 closed。不能给 `SelectCase` 加 `peek`：不是性能问题，而是**公共结构体尺寸/ABI**——多 agent 共享工作树、多个构建目录，「旧 .so + 新头」会在切换点读到越界指针并 SEGFAULT。随机起点用线程本地 xorshift，避免固定下标让后面的 case 饿死。

**代码证据**：
```text
src/concurrency/channel.cpp:450-455
  450  // D1 真正修复（有值/可发送优先）：probe 会真正完成一次传输...
  452  // nil / 构造失败这三种就绪结果**没有发生任何传输**，可以暂存到本轮扫描
  453  // 结束再决定。这样同一个 select 里「已关闭且空」的 case 不会遮蔽另一个
  454  // 通道的缓冲值...
src/concurrency/channel.cpp:479-480  if (probe.status == ChannelStatus::kReady) { // 传输已经发生...必须立刻返回
include/go2cpp/concurrency/channel.hpp:651  class SelectWaitState final {
```

**自证**：`ctest -R 'go2cpp_select_lockorder_test|go2cpp_fiber_select_test'` → 2/2 passed（0.25/0.17 s）。`stress_select_value_first.cpp` 未单独编译跑。

**我没答出的**：没有跑 D1 的值优先压力用例 `stress_select_value_first`。

## Q15

**答**：取消传播必须两阶段：先标记完整子树（写 error/cause、摘 timer、摘子节点），再执行 `before_done` 与 `Signal`；若先通知父回调，取消传播期间同步的 parent→child 等待会死锁。实现用显式栈迭代（深树递归会爆栈），已取消子树直接跳过保证每节点只处理一次；`before_done` 与 `Signal` 都**逆序**（后注册先回滚），回调异常被吞掉。回调在通知线程同步执行，禁止 park/阻塞（否则取消方被挂起、整棵子树卡住）。`ContextRollback` 必须在标记之后冻结 undo：先标记所有 Context 取消，再冻结；`Commit` 发现 child 已取消就转回滚，而不是「先提交再发现取消」。

**代码证据**（题面此行号有误，实测位置如下）：
```text
src/concurrency/context.cpp:1105-1106
  1105  * step1. 用**显式栈**（`work`）而不是递归遍历：深层 Context 树递归会爆栈，
  1106  *        这里每轮取一个节点、把它的后代压回栈，直到栈空；
src/concurrency/context.cpp:1214-1217
  1214  // step5: 两阶段完成——先逆序执行 before_done 回调，再逆序 Signal。
  1215  // 所有 Context 状态都已先标记取消，再让 rollback scope 冻结其
  1216  // undo...
src/concurrency/context.cpp:2111-2129   // ContextRollback::Commit 检查 m_state->ContextCanceled() 后转 kRollingBack
src/concurrency/context.cpp:655-657  // 搬走而非原地遍历...回调在锁外执行...先 notify_all() 唤醒所有等待者
include/go2cpp/concurrency/context.hpp:25  * `DoneSignal` 的回调在**通知线程**上同步执行，所以回调里不能做阻塞操作
```
> **题面错误**：Q15 写 `src/concurrency/context.cpp:1560-1630`（Begin/Commit 检查 child 取消）。`sed -n '1560,1630p'` 实际是 `ContextRollback::State::RecordImpl` 与 `Mark()`，不是 Begin/Commit；`ContextRollback::Commit` 在 **2111**，取消检查在 2124。本卷埋点即此处（偏移 3 行的说法与实际不符，实际错到另一个函数）。

**自证**：`ctest -R 'go2cpp_context_rollback_demo|go2cpp_future_audit'` 未找到同名用例；改用 `ctest -R 'go2cpp_scheduler_smoke'` 与 `go2cpp_sync_cancel_stress` 作为功能回归（passed / OK 5000）。未写「父回调里同步等子 Context」的死锁反证。

**我没答出的**：没有构造两阶段删除后的死锁反例，也没有跑 `ContextRollback` 的专项用例。
