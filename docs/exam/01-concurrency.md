# Go2Cpp 理解力考试 · 并发与调度卷（15 题）

> **本卷数字均需自测，文档数字仅作历史对照。**
> 项目迭代极快，一小时可能优化多轮；任何 `docs/**` 里的 ns/吞吐都不许直接当验收依据。
> 本卷每题都给了「怎么自证」：源码/测试/工具入口，答题者必须现场跑一次并报出
> 核数、`taskset`、轮数与 min/median；与历史文档不一致时，**以当场实测为准**。
>
> 覆盖：Fiber 栈与切换、Scheduler（runnext / 窃取 / findrunnable / netpoll / sysmon /
> shutdown / 双 claim / per-P 分片 / 口径）、Channel（缓冲、等待队列、FastChannel）、
> Mutex、WaitNode、Select、Context 传播与回滚。
> 全部为 L3 陷阱 / L4 推演，不考 API 名称与查文档就能得到的记忆题。
> 出题依据：`include/go2cpp/concurrency/**`、`src/concurrency/**`、`tests/concurrency/**`、
> `tools/startup_cost/**`，以及 `docs/audit/**`、`AI_COLLAB.md` 中记录的真实故障（仅作历史对照）。
> 配套：`docs/exam/README.md`。

## 0. 已在本机验证过的自证入口（2026-11-03，WSL2 12 vCPU，未绑核）

| 入口 | 命令 | 本机结果 |
|---|---|---|
| 调度器 task churn | 编译 `tests/concurrency/bench_scheduler_task_opt.cpp`（命令见 Q01）→ `/tmp/bench_scheduler_task_opt 4 100000 3` | wall **323 / 253 / 155 ns/task**，cpu **1526 / 1222 / 754 ns/task**（3 轮，逐轮下降） |
| park→wake 长尾 | `STARTUP_COST_OUT_DIR=/tmp/exam-build tools/startup_cost/build.sh` → `/tmp/exam-build/startup_cost_park_wake --mode runtime --iters 200 --calibrate` | p50 TOTAL **13.98 µs**、p99 55.76 µs、`>1ms=0`（4 worker、idle_wait 10 ms） |
| 功能回归 | `cd .build-bento-wsl && ctest -R 'go2cpp_scheduler_smoke\|go2cpp_select_lockorder_test' --output-on-failure` | 2/2 通过 |

> 注意：本机 wall 155–323 ns 与历史文档的 122.0 / 326.7 都不一致。这不是「谁错了」，
> 而是**测量条件不同 + 机器状态不同**。凡是拿旧数字当结论的答案，本卷一律扣分。

---

## 一、测量与调度器结构

### Q01 【L4 推演】历史文档给过 wall 122.0 ns 与 submit 326.7 ns（差 2.7 倍）。现在还能复现吗？复现不出来说明什么？对外该怎么表述调度器性能？

- **痛点**：背数字、取更小的、取更新的，或者把两个口径放进同一张对比表——三种都会被项目文档骗。
- **追查位置**：
  - `tests/concurrency/bench_scheduler_task_opt.cpp:75-105`（`main`：wall 用 `steady_clock` 包住 spawn→全部完成；cpu 用 `CLOCK_PROCESS_CPUTIME_ID`）
  - `src/concurrency/scheduler.cpp:6041-6043`（worker 侧 `run_started` 只在 `collect_metrics` 打开时取钟）
  - `src/concurrency/scheduler.cpp:6077-6084`（`metric_fiber_resume_ns` 的累加点）
  - `tools/startup_cost/park_wake.cpp:80`（`__rdtsc()`：工具侧的单段计时方式）
  - 历史对照（**只作对照，不作判据**）：`docs/reports/2026-11-01-scheduler-opt-review.md:19`、`:272`（要求 wall 与提交侧 rdtsc 分开报）
- **验收标准**：
  1. 说清两个口径测的是不同区间：wall 是「提交 → 全部完成」的端到端；submit 是生产者在 `spawn()` 路径上的单段；两者不能相减、不能互换；
  2. **必须现场跑一次**并报出实测值、核数、是否 `taskset`、轮数、min/median；本机实测 wall 155–323 ns（逐轮下降），与历史 122.0 不一致——要能解释「预热/负载/核数/轮次」至少两类原因；
  3. 指出历史 122.0 是「8–11 核、4P/4M、单生产者、空任务体、`collect_metrics=false`、`terminal_collect_interval=256`」的特定条件；换环境必须重测；
  4. 指出 0.98× 与 122.0/135.49≈0.90 的换算不一致，引用倍数前必须自己算并说明分母；
  5. 给出正确对外表述：口径 + 条件 + 当场实测 + min/median，禁止只报一个历史数字；
  6. 指出 park→wake 长尾（另一个工具测的延迟分布）不是吞吐，不能混进 ns/task。
- **怎么自证**：
  ```bash
  g++ -O2 -DNDEBUG -std=c++17 -Iinclude tests/concurrency/bench_scheduler_task_opt.cpp \
      -L.build-bento-wsl -Wl,-rpath,/UserData/CodexWorkSpace/Go2cpp/.build-bento-wsl \
      -lgo2cpp_scheduler -lgo2cpp_fiber -lgo2cpp_error -lgo2cpp_debug \
      -lgo2cpp_debug_event -lpthread -o /tmp/bench_scheduler_task_opt
  taskset -c 2,3 /tmp/bench_scheduler_task_opt 4 100000 5
  ```
  报出每一轮 wall/cpu 与中位数，并注明核数、是否绑核、`collect_metrics` 是否打开。
- **常见错误答案**：「文档写 122 所以达标」「取更小的那个」「122/135≈0.98」——零分（最后一个是算错）。

### Q02 【L3 陷阱】Scheduler 的 `runnext` 为什么是单槽，而不是一个小队列？

- **痛点**：答不出「亲和 + 不给窃取者留窗口 + yield 不能占槽」三层，说明只看了名字。
- **追查位置**：
  - `src/concurrency/scheduler.cpp:577-598`（为什么单槽：LIFO、最近唤醒最可能马上可运行、不经 256 槽环）
  - `src/concurrency/scheduler.cpp:5558-5577`（worker 每轮先取本 M 的 inbox、再取本 P 的 runnext）
  - `src/concurrency/scheduler.cpp:4005-4035`（槽满把旧任务挤出到环尾，不减少环容量）
  - `src/concurrency/scheduler.cpp:6128-6132`（yield 必须放环尾；若进 runnext，1P/1M Join 会饿死）
- **验收标准**：
  1. 单槽 LIFO 的语义：最新被唤醒的 G 最可能马上可运行，放最前面命中率最高；不经过 256 槽环，也不会在队列中间被抢走；
  2. 槽满时被挤出的旧任务落回同一 P 的环尾，容量不减少，只改变「最近一次唤醒」的优先级；
  3. 说出 yield 不能进 runnext 的反例：持续 yield 的 G 会一直占住快速槽，把 park 后被唤醒的 G 挤进队列而永远轮不到；
  4. 说出实现细节：C++17 没有 `atomic<shared_ptr>`，用 TTAS 自旋锁；无争用时是一次 exchange + 两次指针搬运，比 256 槽环的 CAS 更短。
- **怎么自证**：`cd .build-bento-wsl && ctest -R 'go2cpp_scheduler_smoke|go2cpp_scheduler_queue_latency' --output-on-failure`；再写一个 P=1/M=1 的循环（A 持续 yield，B park 后被 wake + Join，看 B 是否长期拿不到执行权）。
- **常见错误答案**：「因为它叫 next 所以只放一个」——零分；只说「为了快」不解释为什么单槽比小队列快，半分。

### Q03 【L3 陷阱】默认 2 KB 初始栈的 Fiber，为什么能撑 10 万连接？栈溢出时到底是谁在救场、什么时候救不了？

- **痛点**：把 `fiber_stack_size=2048` 当实际栈大小，或把 8 MiB 预留当物理内存；不知道退化路径爆栈会直接杀进程。
- **追查位置**：
  - `include/go2cpp/concurrency/scheduler/scheduler.hpp:256-265`（2 KB 请求 → 页对齐 4 KB、8 MiB `PROT_NONE` 预留、按 64 KiB 增长）
  - `include/go2cpp/concurrency/fiber.hpp:284-293`（直接构造 Fiber 默认 128 KiB；`DefaultStackReserveSize` 8 MiB）
  - `src/concurrency/fiber.cpp:539-570`（初始提交量归一化与固定栈回退检查）
  - `src/concurrency/fiber.cpp:885-893`（growable 布局：只在顶部提交，未提交区天然当 guard）
  - `src/concurrency/fiber.cpp:1257-1346`（SIGSEGV handler：fault 必须落在 `[base, committed)`、rsp 在预留区、距离 ≤256 KiB；按 64 KiB→1 MiB 几何增长后直接 return）
  - `src/concurrency/fiber.cpp:1053-1056`（固定栈路径**不安装** SIGSEGV handler，也不分配 sigaltstack）
- **验收标准**：
  1. 归一化：`initial_usable = max(page_align_up(requested), 1 page)`，所以 2048 实际是 4096；RSS 只付一页；
  2. 8 MiB 是 VA 预留（`MAP_NORESERVE`），不是物理内存；10 万 G 的 VA ≈819 GB、RSS 只按已提交页算；
  3. 说出真正的容量上限：默认 `vm.max_map_count=65530`，每 G 约 2 个 VMA，约 3 万 G 触顶（不是内存不够）；
  4. 说出 handler 的四个拒绝条件：取指 fault、fault 不在未提交区、rsp 不在预留区、距离 >256 KiB；任一不满足就恢复默认动作保留 core；
  5. 说出退化路径：`stack_growable=false`、reserve ≤ 初始提交、非 Linux/x86_64、非原生上下文、`GO2CPP_GROWABLE_STACK=0` → 固定栈 + 1 页 guard，**没有 handler**，爆栈 = 进程被内核杀掉。
- **怎么自证**：`cd .build-bento-wsl && ctest -R 'go2cpp_fiber_growable_stack|go2cpp_fiber_stack_limit' --output-on-failure`；再用 `ulimit -v` / `sysctl vm.max_map_count` 各跑一次 10 万 Fiber 的创建脚本，观察在哪一步失败（OOM 还是 VMA 上限）。
- **常见错误答案**：「2 KB 就是栈大小」「8 MiB 是内存，10 万连接不可能」「爆栈会抛异常」——零分。

### Q04 【L3 陷阱】`Channel<T>` 为什么不全用 `FastChannel` 的 Vyukov 无锁 ring？既然 10.73 ns 更快，为什么它没进主线？

- **痛点**：看到 10.73 ns 就以为通用 Channel 也是无锁；或者用「ABA 问题」解释——那不是原因。
- **追查位置**：
  - `include/go2cpp/concurrency/channel.hpp:1188-1196`（`RingBuffer`：所有操作由 `Channel::m_mutex` 串行化；曾评估无锁索引但放弃）
  - `include/go2cpp/concurrency/channel.hpp:32-40`（为什么不用 `deque`；为什么否决「原子标志 + Try* 无锁快路径」）
  - `include/go2cpp/concurrency/fast_channel.hpp:1-20`（文件头：实验版；Vyukov 的 `enqueue_pos`/`dequeue_pos`/每槽 `seq`；满/空才回冷路径）
  - `include/go2cpp/concurrency/fast_channel.hpp:1537-1574`（`Cap==1` 协议退化，必须特化回锁版；实测 2000 个未读值活锁）
- **验收标准**：
  1. 说出无锁 ring 换来的：稳态 1P1C 没有同地址 RMW；以及失去/难做的：close 排空、select 多路、context 取消、等待者登记；
  2. 指出文件头的真实地位：**实验件**（注释写「实验版（/tmp 专用，不入库）」），不是被淘汰的失败品；
  3. 说清通用 Channel 的单锁需求：select 的「探测-回滚」、等待者直接交接、Close 必须与缓冲共享同一线性化点，漏一处就是偶发丢值；
  4. 说出 `Cap==1` 的退化：Vyukov 协议要求 `Cap>=2`，一槽时生产者绕回同一槽前消费者没有独立 seq 状态，会覆盖未读值并活锁；
  5. 不用「ABA」当理由（Vyukov 用 seq 解决 ABA）。
- **怎么自证**：`cd .build-bento-wsl && ctest -R 'go2cpp_channel_concurrency|scenario-channel-deadlock|go2cpp_select_lockorder_test' --output-on-failure`；再读 `tests/concurrency/bench_fast_channel.cpp`，用同一份负载分别测 `FastChannel<1024>` 与 `Channel<int>(1024)`，报两者 ns/条与语义用例通过情况。
- **常见错误答案**：「因为无锁 ring 有 ABA 问题」——零分；「因为 10.73 ns 是假的」——零分（数字来自特定 1P1C 场景，但真实）。

### Q05 【L4 推演】park→wake 的 10 ms 长尾：为什么 Unlock 返回后要等一个 `idle_wait`？根因链是什么、修在哪？

- **痛点**：把长尾归因于「futex 慢」；实际上是 runnext 的消费方与 `notify_one` 的唤醒对象错配。
- **追查位置**：
  - `src/concurrency/scheduler.cpp:4162-4185`（空转 P 投 runnext 的稳定丢唤醒窗口；实测中位 9.67 ms ≈ `idle_wait` 10 ms）
  - `src/concurrency/scheduler.cpp:4036-4060`（该路径改用 `notify_all_workers()`，同一场景降到 14.5 µs）
  - `src/concurrency/scheduler.cpp:5866-5869`（不偷别的 P 的 runnext：只有 owner M 会消费）
  - `src/concurrency/scheduler.cpp:6277-6330`（idle poller：先登记 `io_waiting_workers` 再复查可运行计数，堵住「刚检查完队列、还没进 epoll_wait」的窗口）
  - 历史对照（**只作对照**）：commit `49223be`（p50 9.72 ms→18.66 µs，521×）
- **验收标准**：
  1. 完整复述链路：G 被投进 P 的 runnext → `notify_one()` 唤醒的是 CV 队首（属于哪个 P 不确定）→ 被唤醒的 M 查自己的环、全局队列、再偷其他 P 的**环**（都跳过 runnext）→ 没活可干回去睡 → owner M 仍睡到 `idle_wait` 超时；
  2. 说出为什么只有多 P 才出现：单 worker 时 `notify_one` 必然命中 owner，同段只有十几 µs；
  3. 说出修复的两条分支：本 P 有运行中的 M 才投 runnext，否则投本地环（任意 M 可窃取）；空 P 路径改用广播；
  4. 区分「延迟分布」与「吞吐」：`49223be` 修的是 p50 长尾，不是 ns/task；两个指标不能混；
  5. 指出另一条同类窗口：idle poller 必须先登记再复查，否则 notify 落在「检查完、还没进 epoll_wait」之间就丢。
- **怎么自证**：
  ```bash
  STARTUP_COST_OUT_DIR=/tmp/exam-build tools/startup_cost/build.sh
  /tmp/exam-build/startup_cost_park_wake --mode runtime --iters 1000 --calibrate
  ```
  报 B 段（wake→worker-run）与 TOTAL 的 p50/p99、`>1ms` 计数；再用 `--idle-wait-ms 2` 重跑，观察 p50 是否随 `idle_wait` 变化（本机 200 样本实测 p50 TOTAL 13.98 µs、`>1ms=0`）。
- **常见错误答案**：「futex 唤醒就是慢」「加线程就能解决」——零分。

### Q06 【L3 陷阱】`findrunnable` 的查找顺序为什么是「inbox → runnext → 本地环 → incoming → admission → netpoll」？每一级在省什么？

- **痛点**：把顺序当实现细节；实际上每一级对应一种锁成本，顺序错了就是热路径争用或丢唤醒。
- **追查位置**：
  - `src/concurrency/scheduler.cpp:5558-5601`（step0：M 定向槽 inbox → 本 P runnext）
  - `src/concurrency/scheduler.cpp:5602-5637`（step2：本地 256 槽环，不碰调度器总锁）
  - `src/concurrency/scheduler.cpp:5641-5763`（step3：incoming 16 条带，按条带锁 + 轮转起点 + 空条带掩码）
  - `src/concurrency/scheduler.cpp:5767-5838`（step4：admission 锁内 emergency → 本地 FIFO → 全局 FIFO → 窃取）
  - `src/concurrency/scheduler.cpp:6277-6330`（无 G 可跑时才进 IO idle poller；先登记再复查）
- **验收标准**：
  1. 完整复述顺序与每级的锁：无锁快路径 → 分条锁中路径 → 总锁慢路径 → 等待；
  2. 解释 incoming 的轮转起点与空条带掩码：避免固定顺序饥饿、跳过空条带；
  3. 说出 netpoll 的启用条件：没有 G 可跑、常驻 M、未开 `allow_idle_shrink`、补充 M 还需存在阻塞区；
  4. 说出 idle poller 的丢唤醒窗口与「先登记再复查」的修复；
  5. 说出为什么不能把窃取提前：`src/concurrency/scheduler.cpp:2009-2010` 记录过「把窃取移出全局锁 −34%」，本地队列边界不能动。
- **怎么自证**：`cd .build-bento-wsl && ctest -R 'go2cpp_scheduler_smoke|go2cpp_scheduler_queue_latency' --output-on-failure`；再用 Q05 的 park_wake 工具观察 B 段是否随 P 数变化。
- **常见错误答案**：「顺序无所谓，最后都能找到」——零分；只背顺序不解释锁成本，半分。

### Q07 【L4 推演】sysmon 为什么只能「detach + 补 M」，不能抢占或迁移长阻塞的 G？BlockingRegion 在其中承担什么契约？

- **痛点**：把 sysmon 当 Go 的异步抢占；实际上它只做可逆记账，未声明的阻塞完全看不见。
- **追查位置**：
  - `src/concurrency/scheduler.cpp:2498-2556`（只观察 `MState::kBlocking`；超过 `long_syscall_threshold` 才置 `processor_detached` 并申请替代 M）
  - `src/concurrency/scheduler.cpp:2558-2587`（周期等待；`try_to_lock` 失败置 `sysmon_retry_pending`，下一周期缩到 1 ms）
  - `src/concurrency/scheduler.cpp:5045-5070`（`BlockingRegion` 构造/析构：进入时发布 `MState::kBlocking`、计数 +1；析构按进入时记录的 machine id 恢复）
  - `include/go2cpp/concurrency/scheduler/scheduler.hpp:279-283`（只观察已声明的 BlockingRegion/Hook 边界）
  - `src/concurrency/scheduler.cpp:6101-6107`（任务边界仍有活动 region 就 `std::terminate()`，宁可 fail-fast）
- **验收标准**：
  1. 说出阈值：`sysmon_interval=10 ms`、`long_syscall_threshold=50 ms`；只处理 `MState::kBlocking`；
  2. 说出锁竞争处理：`try_to_lock` 失败不算「本轮扫描完成」，保留重试标记并缩短周期；
  3. 说出 detach 的可逆性：只减 P 的 attached 计数 + `maybe_grow()`，原 M 的栈和 G 都不动；返回时恢复计数、让补充 M 退休；
  4. 解释为什么不能抢占：协作式调度没有安全点，强行切断任意 C++ 调用栈会跳过 RAII、泄漏锁；未 Hook/未声明阻塞的调用对 sysmon 不可见；
  5. 说出 BlockingRegion 的三条契约：只能在有 G 的 M 上进入、不可移动（移动会让析构发生在别的线程）、不能跨 Fiber yield/park 或任务边界（否则 terminate）。
- **怎么自证**：`cd .build-bento-wsl && ctest -R 'go2cpp_scheduler_smoke' --output-on-failure`（含 BlockingRegion 用例）；再写一个 `sleep(1)` 外包 `BlockingRegion` 的 G，观察 worker 数是否临时增加、返回后补充 M 是否退休。
- **常见错误答案**：「sysmon 会打断/迁移长任务，和 Go 的抢占一样」——零分。

### Q08 【L4 推演】`shutdown()` 为什么不能直接释放所有 Fiber 栈？它的顺序是什么？O(n²) 长尾从哪来、怎么修的？

- **痛点**：背「先关 admission 再 join」；不知道「已启动 G 必须保持可运行」的原因，也不知道 10 万 parked G 关停 56.7 s 的根因。
- **追查位置**：
  - `src/concurrency/scheduler.cpp:2756-2777`（未启动 G 原地取消；已启动 G 保持可运行，让 Fiber 从 trampoline 返回执行栈上 RAII）
  - `src/concurrency/scheduler.cpp:3440-3449`（置 draining、关 admission、`begin_shutdown_locked`）
  - `src/concurrency/scheduler.cpp:3503-3513`（等 `live_tasks_total()==0` 的 O(1) 谓词）
  - `src/concurrency/scheduler.cpp:3564-3582`（外部 owner 逐个 join；worker 自己调用时不 join 自己）
  - `src/concurrency/scheduler.cpp:6170-6186`（draining 只在计数归零时做一次全表确认）
  - 历史对照（**只作对照**）：`docs/audit/audit-base-scheduler.md:245-273`（旧实现 10 万 parked G 关停 56.7 s→3.3 s）
- **验收标准**：
  1. 说出「未启动 vs 已启动」的区别：未启动没有挂起栈，可原地取消；已启动必须让它自然返回来跑栈上析构；
  2. 复述完整顺序，并指出 **sysmon 在 draining 阶段保持运行**，终态归零后才停；
  3. 解释 O(n²)：旧实现每完成一个 G 就持 admission 锁做 `collect_terminal` + `finish_draining` 两次全表扫描，G 完成数 × 注册表大小；
  4. 说出修复：`live_tasks` 原子计数（首次入队 +1、四个终态转移点 −1，与状态同临界区），归零后才全表确认；
  5. 说出为什么不能「超时强制释放栈」：worker 仍拥有栈，强释放会跳过 RAII；有界 shutdown 超时只返回 false，保持 draining 以便下次继续。
- **怎么自证**：先跑 `ctest -R go2cpp_scheduler_smoke`；再写一个 N=10 万 parked G 的小程序，分别测 `shutdown()` 在 N=1 万/5 万/10 万的耗时并画比例，确认是否线性。
- **常见错误答案**：「shutdown 就是把所有 Fiber 析构掉」「超时就强杀」——零分。

### Q09 【L3 陷阱】`execute/gogo` 路径为什么需要 `execution_claim` 和 `run_claim` 两个位？少一个会怎样？状态字为什么必须带 wake 令牌？

- **痛点**：把两个 claim 当冗余；或把「状态」和「唤醒令牌」拆成两个原子——这正是「Runnable 但无人调度」的来源。
- **追查位置**：
  - `src/concurrency/scheduler.cpp:1916-1919`（两个 claim 必须在同一临界区一起翻转，否则新执行者会在 `run()` 入口被静默拒绝，G 永久 `kRunning`）
  - `src/concurrency/scheduler.cpp:1712-1731`（`run_claim` 失败时把 G 回滚为 Runnable 并 `deferred_enqueue`）
  - `src/concurrency/scheduler.cpp:1489-1513`（`claim_from_queue`：一次 CAS 完成 Runnable→Running 与执行权仲裁）
  - `src/concurrency/scheduler.cpp:1424-1468`（park：要求整个字「Running 且无令牌」才 CAS 成 Waiting）
  - `src/concurrency/scheduler.cpp:1353-1387`（wake：Waiting→Runnable，或 Running 时只置 bit8 令牌）
- **验收标准**：
  1. 区分两个位：`execution_claim` 是执行权，`run_claim` 表示「`run()` 栈帧尚未退出」；
  2. 说出反例：只放 `execution_claim`，新执行者会在入口被 `run_claim` 拒绝并静默返回，G 停在 `kRunning` 无人认领，shutdown 永远等不到终态；
  3. 解释状态字：低 8 位 GState、bit8 是 wake 令牌；park/wake 用 CAS 竞争同一个 32 位字，不存在「令牌已发布、挂起已提交、却没人消费」的窗口；
  4. 说出 `claim_from_queue` 为什么单 CAS：Runnable→Running 本身就是执行权唯一仲裁点；先抢 claim 再改状态要多一次 RMW（源码注释实测 ~8 ns）。
- **怎么自证**：`ctest -R 'go2cpp_scheduler_smoke|go2cpp_scheduler_queue_latency' --output-on-failure`；再用 TSAN 构建跑 park/wake 往返（源码注释里把它列为验证项），确认没有「同一 G 两个执行者」的报错。
- **常见错误答案**：「两个标志是冗余的，留一个就行」——零分。

### Q10 【L3 陷阱】per-P 分片：哪些结构分了片？分片后精确聚合变贵，draining 判零为什么不能读分片近似？

- **痛点**：把分片当万能优化；不知道分片后「精确总量」要 O(P) 聚合，而 shutdown 判零必须精确。
- **追查位置**：
  - `src/concurrency/scheduler.cpp:1990-1996`（registry 16 条带、incoming 16 条带；理由：避免生产者集中争用一把锁）
  - `src/concurrency/scheduler.cpp:2019-2040`（P 本地结构；`active_machines`/`running_machines` 原子）
  - `src/concurrency/scheduler.cpp:6008-6013`（分片后精确聚合要 O(P) 次跨核 load，热路径只固定 64 拍做维护）
  - `src/concurrency/scheduler.cpp:6201-6217`（完成一个 G 后只读**本 P**分片决定是否 notify）
  - `src/concurrency/scheduler.cpp:6170-6186`（draining 判零用全局 `live_tasks`，不是分片近似）
- **验收标准**：
  1. 列出至少三类分片：registry 16 条带、incoming 16 条带、runnable 按 P 分片（外加 P 自己的本地队列）；
  2. 说出目的：把全局原子 RMW 变成生产者本地行，减少跨核 cacheline 迁移；
  3. 说出代价：精确总量要 O(P) 聚合，热路径只能读本地分片/提示位；
  4. 解释 draining 判零为什么必须精确：少算一个存活 G 就会提前停 worker、丢掉未完成的 Fiber；所以用与状态同临界区的 `live_tasks`，而不是分片求和；
  5. 引用收益时必须现场测（本机 wall 155–323 ns 与历史 122 不一致），不能直接搬历史 −34.7%。
- **怎么自证**：用 Q01 的 bench 分别跑 `4 100000 5` 与 `1 100000 5`，报两种 P 数下 wall 与 cpu；再用 `perf stat -e cache-misses` 观察分片前后跨核流量（可选）。
- **常见错误答案**：「分片是为了省内存」「分片一定更快」——零分。

---

## 二、Channel / Mutex / WaitNode / Select / Context

### Q11 【L3 陷阱】Channel 的等待队列为什么用侵入式链表 + 每节点 futex？节点生命周期归谁？共享唤醒字会坏在哪？

- **痛点**：以为等待队列是 `std::condition_variable`；不知道共享 32 位唤醒字会唤醒错节点（审计 D2）。
- **追查位置**：
  - `include/go2cpp/concurrency/channel.hpp:768-782`（设计要点：队列归 Channel、节点池化上限 128、per-node futex、取消不持节点、不能持锁 park）
  - `include/go2cpp/concurrency/channel.hpp:2757-2769`（发送方摘 recvq：侵入式链表 O(1)、跳过已取消节点）
  - `include/go2cpp/concurrency/channel.hpp:2303-2330`（等待节点协议：`signal` CAS 单一赢家；`accepted`/`closed` 互斥）
  - `include/go2cpp/concurrency/channel.hpp:2727-2738`（`NotifyChange()` 必须锁外；值只在交接成功后移动）
- **验收标准**：
  1. 说出队列归属：节点由等待者栈帧持有，Channel 队列只持裸指针；入队/出队/摘链都在 Channel 锁内；
  2. 说出 per-node futex 的理由：共享唤醒字会唤醒错节点（D2），每节点自己的 32 位字 + `FUTEX_WAKE(1)` 才能精确唤醒；
  3. 说出「锁外 notify」的理由：唤醒可能跨线程，持 Channel 锁调用会形成锁序反转/惊群；
  4. 说出单一赢家：交接方用 CAS 把 `signal` 从 0 推到 `kReady/kClosed/kCancelled`，只有一个能提交；取消回调不摘链，由交接方清理；
  5. 说出节点池化：稳态每次阻塞收发 0 次堆分配；池上限 128。
- **怎么自证**：`cd .build-bento-wsl && ctest -R 'go2cpp_channel_concurrency|go2cpp_channel_thread_stress' --output-on-failure`；再跑 `scenario-channel-deadlock`，确认多生产者/多消费者 + 取消下不挂死。
- **常见错误答案**：「阻塞就是 `condition_variable.wait`，唤醒就是 `notify_all`」——零分。

### Q12 【L3 陷阱】`sync::Mutex` 的状态字为什么必须原子 RMW？`load+store` 会怎样制造「双持」？「两态」到底指哪两态？

- **痛点**：把「两态」误解成 locked/unlocked；或把状态字当普通变量改——项目真实发生过双持与解锁异常。
- **追查位置**：
  - `src/concurrency/sync.cpp:1259-1280`（状态位：`kLocked`/`kWaiter`/`kWoken`/`kStarving`；正常/饥饿两模式）
  - `src/concurrency/sync.cpp:1276-1279`（`load` + `store` 曾抹掉并发 `TryLock` 写入的 `kLocked`）
  - `src/concurrency/sync.cpp:1347`（`UpdateStateAtomic`：CAS 失败用最新值重算）
  - `src/concurrency/sync.cpp:1539-1549`（正常/饥饿两态状态机）
  - `src/concurrency/sync.cpp:2028`（饥饿模式 `Unlock` 不释放 `kLocked`，所有权直接交给 FIFO 队首）
  - 历史对照（**只作对照**）：`AI_COLLAB.md:264-267`（修复前 3 秒 9–34 次解锁异常、629–1683 次双持）
- **验收标准**：
  1. 说出两态是「正常/饥饿」两个模式；4 个位是状态字里的标志位，不是 4 个互斥状态；
  2. 正常模式：Unlock 先释放再唤醒，被唤醒者回顶部重新 CAS，新来者可插队；`kWoken` 保证同时最多一个被唤醒者；
  3. 饥饿模式：等待超过 1 ms 置 `kStarving`；Unlock 不释放 `kLocked`，把所有权直接交给 FIFO 队首，新来者不得插队；
  4. 说出原子 RMW 的必要性：`TryLock` 快路径 CAS 不持 `m_mutex`，锁内用旧快照 `load+store` 会覆盖刚写入的 `kLocked`，导致双持 + 解锁异常；
  5. 说出退出条件：被交接者自身未超阈值会清 `kStarving`；队列清空也退出。
- **怎么自证**：`cd .build-bento-wsl && ctest -R 'go2cpp_sync_cancel_stress|go2cpp_scheduler_smoke' --output-on-failure`；再写一个「线程 A 循环 `Lock/Unlock`，线程 B 高频 `TryLock`」的压测，跑 3 秒，检查 `mutex_held_park_total` 与是否出现 `logic_error`。
- **常见错误答案**：「两态就是 locked/unlocked」「状态字加锁保护就行」——零分（`TryLock` 快路径根本不取那把锁）。

### Q13 【L3 陷阱】WaitNode 的等待队列为什么会出现 O(n²)？取消/超时的节点不摘链，谁在替它买单？

- **痛点**：以为取消/超时节点会从队列中间 O(1) 摘除；实际上交接方只能逐个 pop 跳过僵尸节点，叠加每次 Unlock 的全队列重排就是 O(n²)。
- **追查位置**：
  - `src/concurrency/sync.cpp:309-320`（`WaitNode`：一次阻塞一个节点，同时服务 Mutex/CV/WaitGroup）
  - `src/concurrency/sync.cpp:1122`（`ReorderWaitersForSelector`：每次 Unlock 全队列重排）
  - `src/concurrency/sync.cpp:1386`（`DropWakeToken`：循环找活节点）
  - `src/concurrency/sync.cpp:2088`（正常模式交接循环：跳过已取消节点）
  - `src/concurrency/sync.cpp:2133`（锁外唤醒；持队列锁唤醒会死锁）
  - 历史对照（**只作对照**）：`docs/audit/waitnode-lock-design.md:12`（节点内部锁必须是 FutexMutex 的理由）
- **验收标准**：
  1. 说出节点不摘链的原因：结果用 CAS 单赢家，队列没有反向索引，取消回调不能安全解引用队列外节点；
  2. 说出 O(n²) 的构成：单次交接最坏 O(n)（逐个 pop 跳过僵尸）× 每次 Unlock 的全队列重排 × n 次取消；
  3. 说出后果：大量超时等待者时 Unlock 变慢，尾延迟放大；持队列锁做长扫描还会推迟所有等待者；
  4. 说出至少两个修复方向：反向索引/句柄（取消时 O(1) 摘链）、惰性批量清理、把「跳过僵尸」从交接热路径移走；
  5. 说出节点内部锁为什么不能用 `std::mutex`：Wake 可由任意线程调用，跨线程解锁是 UB；必须用无 owner 的 FutexMutex。
- **怎么自证**：写一个 N 个等待者、每个带 1 ms 超时、然后连续 Unlock 的压测，分别测 N=1k/5k/10k 的 Unlock 耗时，观察是否超线性；同时跑 `ctest -R go2cpp_sync_cancel_stress`。
- **常见错误答案**：「取消时 O(1) 摘掉就行」——零分（节点结果 CAS 与队列摘链不是同一个线性化点）。

### Q14 【L3 陷阱】Select 的「有值优先」：为什么 `closed` 要延后？为什么不能给 `SelectCase` 加 `peek`？

- **痛点**：把「closed 优先」当直觉；或以为加个预判字段是纯优化，不知道它会破坏 ABI。
- **追查位置**：
  - `src/concurrency/channel.cpp:446-501`（探测主循环：随机起点、`kReady` 立即返回、closed/nil 延后到扫描结束）
  - `src/concurrency/channel.cpp:450-490`（注释：`kReady` 传输已发生不可回滚；closed 无副作用可暂存）
  - `include/go2cpp/concurrency/channel.hpp:651`（`SelectWaitState`：等待路径的单赢家）
  - 历史对照（**只作对照**）：`docs/modules/select-semantics.md:21-33`（D1 根因）、`:94-104`（否决 peek 的 ABI 原因）
- **验收标准**：
  1. 说出旧实现的丢值路径：`probe()` 命中「已关闭且缓冲为空」返回 `kClosed`，调用方按习惯 break，另一个通道的缓冲值再也无人取；
  2. 说出新规则：`kReady` 立即返回（传输已发生，回滚会丢值）；closed/nil/invalid 无副作用，暂存到扫描结束；
  3. 说出「有值 vs 可发送」为什么不排序：两者都立即传输，谁先探测到谁赢；把 send 成功也延后就必须回滚一次已发生的发送；
  4. 说出否决 `peek` 的原因不是性能，而是**公共结构体尺寸/ABI**：多 agent 共享工作树、多个构建目录，「旧 .so + 新头」会在切换点读到越界指针并 SEGFAULT；
  5. 说出随机起点：线程本地 xorshift，避免固定下标让后面的 case 饿死。
- **怎么自证**：`cd .build-bento-wsl && ctest -R 'go2cpp_select_lockorder_test|go2cpp_fiber_select_test' --output-on-failure`；再跑 `tests/concurrency/stress_select_value_first.cpp`（按文件头命令编译），断言「每个已关闭通道的缓冲在返回 kClosed 前为空」。
- **常见错误答案**：「closed 优先更符合直觉」——零分；「加个 peek 预判就好了」——零分（ABI 事故）。

### Q15 【L4 推演】Context 取消传播为什么必须「先标记完整子树、再执行回调」？`ContextRollback` 又为什么必须在这之后冻结 undo？

- **痛点**：把取消写成「从根递归调用回调」；不知道父回调里同步等子会死锁，也不知道取消窗口里 `RecordUndo/Commit` 的竞态。
- **追查位置**：
  - `src/concurrency/context.cpp:1100-1132`（步骤：显式栈迭代、先标记后唤醒、锁外回调、逆序）
  - `src/concurrency/context.cpp:1214-1233`（先逆序执行 `before_done`，再逆序 `Signal`；先标记再让 rollback scope 冻结 undo）
  - `src/concurrency/context.cpp:655-670`（`DoneSignal`：锁内搬出回调，锁外先 notify 再执行，绝不在持锁状态跑用户代码）
  - `src/concurrency/context.cpp:1501-1530`（`ContextRollback::State`）
  - `src/concurrency/context.cpp:1560-1630`（`Begin/Commit` 检查 child 是否已取消；已取消转回滚）
  - `include/go2cpp/concurrency/context.hpp:25`（回调在通知线程同步执行，不能阻塞）
- **验收标准**：
  1. 说出两阶段：先标记完整子树（写 error/cause、摘 timer、摘子节点），再执行 `before_done` 与 `Signal`；若先通知父回调，取消传播期间同步的 parent→child 等待会死锁；
  2. 说出实现：显式栈迭代而非递归（深树会爆栈）；已取消子树直接跳过，保证每节点只处理一次；
  3. 说出逆序：后注册的 undo 先回滚；回调异常被吞掉，一个观察者抛异常不能阻止其他观察者被唤醒；
  4. 说出回调禁 park 的后果：回调在通知线程上同步执行，park/阻塞会把取消方挂起，整棵子树传播卡住；捕获裸指针还会 UAF；
  5. 说出 rollback 的顺序：先标记所有 Context 取消，再冻结 undo；Commit 发现 child 已取消就转回滚，而不是「先提交再发现取消」。
- **怎么自证**：`cd .build-bento-wsl && ctest -R 'go2cpp_context_rollback_demo|go2cpp_future_audit' --output-on-failure`；再写一个「父 Context 的 Done 回调里同步 `wait` 子 Context」的用例，确认不会死锁（去掉两阶段后应当死锁，可作反证）。
- **常见错误答案**：「取消就是从根递归调用回调」「回滚就是 try/catch 里调用 undo」——零分。
