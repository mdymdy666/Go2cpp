# 调试模块开发 · 进度表（DEV）

> 分支：`feature/gmp-debug-module`；窗口：2026-10-16 13:00 – 18:00（每 15 分钟一轮）。
> 设计依据：本目录 `design/` 与 `scenarios/`；本项目规范：`PROJECT_RULES.md`。
> **使用者定案**：① 元数据用 `DebugFiber` 继承 `Fiber`（调试信息放 `DebugFiber`，不影响原框架）；
> ② 多机时钟用双时间戳 + 明确声明不做绝对对齐；③ 运行期可改参数只开放观测/节律类。

## 总览

| 阶段 | 目标 | 状态 |
|---|---|---|
| P0 | 只读快照接口 + 元数据 + `emit_event` 微基准 | 🚧 进行中 |
| P1 | 序列化（text/ini/json）+ stdio/文件 sink → `dump scheduler` 可用 | ⏳ |
| P2 | 事件采集 + 环形缓冲 + `--follow` | ⏳ |
| P3 | 锁观测（D1/D2）+ 锁争用选择钩子（D3） | ⏳ |
| P4 | 控制面（迁移/插队/延迟替换），默认关闭 | ⏳ |
| P5 | 协作式断点（5 个安全点） | ⏳ |

## 逐轮记录

| 轮 | 内容 | 证据 |
|---:|---|---|
| 1 | 建分支 `feature/gmp-debug-module`；删除旧定时任务；**编译探针验证 `DebugFiber` 继承可行性** | `git branch --show-current`；探针输出 `Fiber_virtual_dtor=0 / is_final=0 / default_ctor=0 / copyable=0 / movable=0 / DebugFiber_ok=1` |
| 2 | 核实**原类可继承性**并写成约定：仅 `Fiber`/`Task`/`Scheduler`/`SchedulerObserver` 可继承；`Mutex`/`ConditionVariable`/`Channel`/`Context`/`Timer`/`HybridGate`/`ParkingCondition` 等**被标记 `final`**，不可继承也不改原框架 | `grep -rn "class [A-Za-z_]* final" include/`；新增 `design/09-debug-class-conventions.md` |
| 3 | 实现 **`DebugFiber`（继承 `Fiber`）+ `DebugRegistry`**（独立注册表，原框架零改动）；补测试（5 场景 + 压测 + 随机）；登记 CMake 与 `test_main`；构建通过 | `include/go2cpp/debug/debug_fiber.hpp`（183 行）、`tests/debug/test_debug_fiber.cpp`（232 行）、`cmake/Go2CppTests.cmake`、`tests/test_main.cpp` |
| 4 | 生成 **final 类清单**（81 个，含各自是否含虚函数）：**自身含 `virtual` 的只有 2 个**（`SelectValue`、`tools::defer`），其余 79 个为非虚 → 去掉 `final` 对它们零影响（已有实测支持）；核心相关类 16 个；确认**代理方案**并写入 `design/09` §2.4 | `design/10-final-class-inventory.md`（132 行）；`design/09` §2.4；脚本按花括号切类体逐个判定 |
| 5 | 观察者开销三档对照基准：新建 benchmarks/debug_observer_overhead_benchmark.cpp（222 行）并登记 CMake；**发现并修正一处测量错误**（缺热身轮导致先测档偏慢约 20 倍）；实测 1/2/4 worker 三档；**关键结论**：单 worker 下观察者净开销仅 +3~6%（约 27 ns/事件），而 **4 worker 基线本身异常**（1,602 → 19,914 ns，约 12 倍）属调度器自身扩展性问题、与观察者无关；建议插件用线程局部缓冲规避 observer_mutex 争用 | `DebugTooling/perf-observer-overhead.md`；`docs/performance.md` 新节；`./build-bench/go2cpp_debug_observer_benchmark 3 20000 {1,2,4}` |
| 6 | **`DebugScheduler` 代理 + text/ini/json 序列化**（P0 核心 + P1 起步）：新建 `include/go2cpp/debug/debug_scheduler.hpp`（311 行，**只用 Scheduler 现有 public 接口，零侵入**）；`tests/debug/test_debug_scheduler.cpp`（5 场景 + 压测 + 随机）；登记 CMake 与 test_main；**全量 ctest 79/79 通过**；压测：Snapshot 1.26 us/次、三种 Dump 约 11.6 us/次 | `debug_scheduler.hpp`；三处编译纠错（命名空间别名、`const char*+const char*`、`go2cpp::Json`）与一处**测试假设纠错**（machine id 不从 0 开始，实测为 5） |
| 7 | **dump CLI + 原子文件 sink**（P1 收尾）：新建 `include/go2cpp/debug/debug_cli.hpp`（219 行）——`ParseDumpCommand`（--format/--output/-o/--append/--summary/--processors/--machines/--metrics/位置参数 name）、`WriteDumpFile`（临时文件 + rename 原子写；append 退化为直接追加并如实说明）、`RunDump`/`RunDumpCli`；`tests/debug/test_debug_cli.cpp`（214 行，5 场景 + 压测 + 随机）；**诚实标注**：本 CLI 是**进程内**形态，跨进程 dump 需信号/注入机制，属后续阶段 | `debug_cli.hpp`；`test_debug_cli.cpp`；`ctest 79/79` |
| 8 | **P3 锁观测**（Mutex + ConditionVariable）：按代理方案在 `sync.hpp`/`sync.cpp` 内加`DebugSnapshotInternal()`（每个类净增约 8 行，**语义与开销不变**），新增 `include/go2cpp/debug/debug_sync.hpp`（70 行）与 `tests/debug/test_debug_sync.cpp`（189 行，5 场景 + 压测 + 随机）；**偏离记录**：原计划的 `friend + private` 在本项目包含顺序下**未对内联代理生效**（编译报 private 访问），改为 **public + Debug 前缀**并注明原因；**全量 ctest 79/79 通过**；压测 Snapshot ×20000 = 1811 us（≈90 ns/次） | `sync.hpp`、`sync.cpp`、`debug_sync.hpp`、`test_debug_sync.cpp`；ctest 全绿 |
| 9 | **D3 锁争用选择钩子**（使用者最初点名的核心能力）：在 `sync.hpp` 新增 `WaiterCandidate` 与 `WaitSelector`（`Select(candidates) -> optional<index>`）以及 `SetWaitSelectorFactory`/`ResetWaitSelectorFactory`；`sync.cpp` 里在 `Mutex::Unlock` 取得队列锁后、FIFO 认领循环**之前**调用 `ReorderWaitersForSelector`；**关键设计**：选择器**只把选中者换到队首**，不改动“逐个 TryFinish 认领”的裁决方式，因此「所有权无缝转移、TryLock 不能插队」完全不受影响；接口刻意不暴露内部 `WaitNode`，也不虚构 `wait_ns`（节点未记录等待起点）；测试 `tests/debug/test_debug_lock_selector.cpp`（302 行）：5 场景（默认 FIFO / 指定者优先 / 不干预与越界退化 / 抛异常被隔离 / Reset 后恢复且 TryLock 仍不能插队）+ 压测（无竞争 10 万次 ≈29 ns/次、4 线程竞争 2 万次 3 ms）+ 随机（随机指派下标 8 轮不放死） | `sync.hpp`、`sync.cpp`、`test_debug_lock_selector.cpp`；全量 ctest 结果见下一轮记录 |
| 10 | **WaitGroup 观测**（P3 收尾）+ **两处自查纠错**：`sync.hpp`/`sync.cpp` 新增 `WaitGroupDebugSnapshot{count, waiters}` 与 `WaitGroup::DebugSnapshotInternal()`，`debug_sync.hpp` 新增 `DebugWaitGroup` 代理，`test_debug_sync.cpp` 补 1 个场景（含空指针代理）；**纠错一**：`WaitSelector::Select` 原带 `noexcept` 而测试里 `throw`，编译报 `-Wterminate` —— 说明"异常被隔离"**根本到不了 catch**，属实现与文档不符，已去掉接口的 `noexcept` 并让场景 4 真正进入选择器（补足 2 个等待者）；**纠错二**：`go2cpp_tests` 在 ctest 下曾 Timeout，定位为 `TIMEOUT 30` 余量不足（整体 18s）且与 build-bench 并行时被 CPU 争用打爆，已放宽到 60；新增 D3 选择器开销基准 `benchmarks/debug_lock_selector_overhead_benchmark.cpp`（189 行） | `sync.hpp`、`sync.cpp`、`debug_sync.hpp`、`test_debug_sync.cpp`、`test_debug_lock_selector.cpp`、`cmake/Go2CppTests.cmake`、基准文件；构建 0 warning，**全量 ctest 79/79** |
| 11 | **P2 事件采集**：新建 `include/go2cpp/debug/debug_events.hpp`（270 行）——`EventBuffer`（有界环形、Push/Pop 均非阻塞不抛异常、满则丢并计数）与 `DebugEventCollector : SchedulerObserver`（回调内只做一次非阻塞入队，不加锁不分配不做 I/O），带 text/ini/json 三种序列化；`tests/debug/test_debug_events.cpp`（265 行）：6 个场景（空/满/回绕、满时丢弃守恒、**接真实调度器采集到任务级与 worker 级事件**、小容量必然丢弃证明有界性、Drain/Peek 语义、三种序列化含 json 解析回来）+ 压测（Push/Pop 100 万次 = 43 ms）+ 随机（200 组随机容量/数量，校验 captured+dropped 守恒）；**全量 ctest 79/79**；**D3 选择器基准初步结果**：无竞争场景三档一致（5.25/4.36/4.52 ns）证实在 CAS 快路径上零开销；有竞争的**大参数（80,000 次/档）未在 15 分钟内完成**，小参数可完成，**原因尚未定位，已如实标注为未决项** | `debug_events.hpp`、`test_debug_events.cpp`、`debug_selector_overhead_benchmark` 部分结果 |
| 12 | **D3 选择器开销定位（澄清第 11 轮遗留未决项）**：给基准加第 4 个参数支持**分档单跑**并在场景 B 逐轮 `fflush`，避免再出现"输出丢失只剩 exit code"；关键数据：**无竞争三档一致（3.71/3.78/3.76 ns/op）**证明选择器在 CAS 快路径零开销；**有竞争时档 0（未注册选择器＝框架默认）本身就是 15,785/18,416/19,842 ns/op**，与档 2 的 16,776 同量级 → **13~24 µs/op 是 Mutex 自身竞争成本，与选择器无关**；据此**作废**上一轮"LastSelector 可能饥饿"的推测（未测基线档就归因，是方法错误）；**附带发现**：Mutex 4 线程竞争约 18 µs/op，vs 单线程无竞争 3.7 ns/op，相差约 5000 倍，属 sync::Mutex 自身行为，本模块不声称原因 | `docs/performance.md` 新节、`DebugTooling/perf-observer-overhead.md`、`benchmarks/debug_lock_selector_overhead_benchmark.cpp`（分档改造）；仍未定位：默认参数那次 15 分钟未结束的根因（不视为已解决） |
| 13 | **`dump events` 接入 CLI（打通 P2 与 P1）**：`debug_cli.hpp` 加 `DumpTarget{kScheduler, kEvents}`（**缺省仍为 scheduler，向后兼容**，且非法目标名必须报 `unknown target` 而**不能静默当成 scheduler**）、`RunEventsDump`（用只读 `Peek`，**不消耗**采集器事件）与统一入口 `RunDump(command, scheduler, collector, ...)` 按 target 分派；`debug_scheduler.hpp` 的 `DumpOptions` 加 `event_limit`；`test_debug_cli.cpp` 补场景（target 解析与默认值、非法目标报错、端到端写文件且内容与返回值一致、dump 后 `Pending()` 不变、json 可解析）；**全量 ctest 79/79**，实机可见 7 条 `kTaskStarted` 被正确 dump | `debug_cli.hpp`、`debug_scheduler.hpp`、`test_debug_cli.cpp`；构建 0 warning，ctest 全绿 |
| 14 | **P5 协作式断点**（零侵入）：`debug_breakpoints.hpp`（293 行）——`DebugBreakpoints` 注册表（Add/Remove/Clear/SetEnabled/ShouldStop/Resume/ResumeAll/Waiting/Hits/TotalHits/Snapshot/DumpText）、自由函数 `Checkpoint(name, task_id)`（**不阻塞**，只返回命中的断点 id）与 RAII `ScopedCheckpoint`；**关键设计决定：不在调度器热路径加钩子** —— 理由是本项目永不强制 detach 任意 C++ 调用栈（cooperative-only），且 `Scheduler` 无安全点回调、`SchedulerObserver` 是只读边界（其头文件明写不能控制 G 执行权），要挂钩子就得改热路径；改用**使用者在自己代码里调用 Checkpoint** 的协作式做法，因此**零侵入、不改变任何热路径开销**，也完全符合"G 只在它自己声明的位置停"的项目原则；测试 `test_debug_breakpoints.cpp`（274 行）：8 个场景（未注册恒为 0 / 名称匹配 / skip / once 与重新启用 / task_id 过滤与 0=任意 / Resume 与 ScopedCheckpoint 自动恢复 / **受管 G 内 40 个任务并发经同一检查点全部命中** / Clear 与 ResetStats 语义分离）+ 压测（100 万次：无断点 53 ns/次、1 个不匹配断点 72 ns/次）+ 随机（3000 轮随机注册/移除/命中，校验 id 唯一与 Size 一致）；**全量 ctest 79/79** | `debug_breakpoints.hpp`、`test_debug_breakpoints.cpp`；构建 0 warning |
| 15 | **收尾：最终交付报告** `DebugTooling/DEV-FINAL-REPORT.md`（167 行）——含一句话结论、交付概览（12 提交 / 26 文件 / +4832−1 行）、各阶段成果与提交号、测试覆盖明细（每模块场景数与压测内容）、性能实测汇总、**对原框架的精确改动清单**（唯一改动 `sync.hpp` +104 与 `sync.cpp` +104，**纯新增 0 删除**）、**13 条能力边界总表**、**3 项未决项**（4-worker 基线 12 倍劣化 / Mutex 竞争约 18 µs / 一次基准超时根因）、与设计文档的对应关系、后续建议、复现方式 | `DEV-FINAL-REPORT.md`；核心改动量由 `git diff --numstat` 自动写入，非手写 |
| 16 | **诊断未决项 1（只测不改）**：`DebugTooling/diag-worker-scaling.md` —— 实验 1 扫 1/2/3/4/6/8 worker：**1→2 正常并行（0.69x，更快）**，**2→3 跳变约 9.8 倍**，4 worker 约 13~16x，6/8 趋于平稳（23~26 µs）；两轮独立测量均出现跳变 → **可复现**；实验 2 固定 4 worker 把任务数从 2k 增到 80k（40 倍）：每任务 19.2k→23.1k ns（**1.20x，未下降**）→ **排除固定启动开销**，支持持续并发开销；4 worker 下离差可达 3.7 倍（**不稳定本身是信号**）；**未做 profiling，故不指定具体机制**；全部测量在 observer=nullptr 下进行，与调试模块无关 | `diag-worker-scaling.md`；最终报告未决项 1 已由「根因未定位」更新为「已诊断、机制未确认」 |
| 18 | **可运行示例** `example/debug/debug_module_demo.cpp`（237 行，登记 `GO2CPP_EXAMPLES`，因项目的示例机制会**自动注册为 ctest 用例**）：7 节演示 —— 启动调度器并挂采集器 / 跑 20 任务同时验证协作式断点命中 20 次 / `dump scheduler` 三种格式 / `dump events`（含「dump 不消耗事件」断言）/ `Mutex`+`ConditionVariable`+`WaitGroup` 快照 / `RunDumpCli` 原子写文件 / 断点观测与清理；**顺带两处修正**：① 示例自身 `argc` 写成 6 而数组仅 5 个元素，越界读到 `nullptr` 后 抛 `basic_string: construction from null`（gdb backtrace 指向 `RunDumpCli`），已改用 `sizeof` 推导；② **产品级改进**：给 `RunDumpCli` 加 `nullptr` 参数防御，遇到空指针返回参数错误而不是让进程崩溃；构建 0 warning，示例自报「所有演示步骤通过」 | `example/debug/debug_module_demo.cpp`、`debug_cli.hpp`；ctest 全绿 |
| 19 | **任务2 第二轮：G1（地基层）一图** `diagrams/G1-foundation.md`（308 行，5 个 Mermaid）—— 类架构含 Fiber / FiberBin / FiberContextFrame / FiberExecutionBinding / FiberResumeResult / ParkingCondition / Timer / TimerService / TimerDecision / TimerExecutor，每类标注用了哪些底层能力与职责；重要函数画出 AcquireForScheduler 的栈池复用、resume_from_scheduler 的三分支返回（Completed/Failed/Suspended）、Suspend 的协作式让出、ParkingCondition 对受管 G 与普通线程的双路径；向上层列出 Scheduler/sync/channel/future/timer/context 各自取用的能力；状态流转给出 **Fiber 状态机**（并注明与 G 的 kWaiting 对应）与 **Timer 状态机**（含 TimerDecision 续期）；另附「挂起→唤醒」的栈变化 ASCII 图 + 与 Go 的差异表 | `ArchitectureDiagram/diagrams/G1-foundation.md`、diagrams/README.md 索引；校验 5 个 mermaid 且四要点齐全 |
| 20 | **任务1 收尾-A：枚举全部 G 接口** —— 使用者要求先完成调试模块收尾，故暂停任务2。`scheduler.hpp` 加 `TaskSnapshot`（8 个只读字段）与 `Scheduler::DebugSnapshotTasks()`，`scheduler.cpp` 实现**按 16 分片逐个持锁遍历**（用 `task_registries.size()` 而非硬编码，避免与常量漂移）；`debug_scheduler.hpp` 加 `SnapshotWith(options)`，`DumpText/Ini/Json` 在 `include_fibers=true` 时输出 `fibers` 段（text 表格 / ini 的 `[g.N]` 节 / json 数组），**不要求时 json 仍为 null 保持向后兼容**；补场景 6 测试（不要求时不采集、G 结束后被回收故为空、长命阻塞 G 能被枚举到且 state/started/failed 合理、三种格式均含该 G、默认仍为 null）；构建 0 warning、ctest 全绿 | `scheduler.hpp`、`scheduler.cpp`、`debug_scheduler.hpp`、`test_debug_scheduler.cpp` |
| 21 | **任务1 收尾-B：`dump events --follow`**：`debug_events.hpp` 加 `EventFollowSink` 与 `FollowEvents(collector, sink, interval, max_batches, overall_timeout)`；`debug_cli.hpp` 加 `--follow[=N]`、`--follow-interval=ms`、`--follow-timeout=ms`（默认 1000ms），跟随分支**逐事件输出一行 JSON 并每批 fflush**；**修掉一个我自己引入的严重缺陷**：首版在没有总超时时，若采集器一直为空则 `batches` 永远到不了 `max_batches`，`FollowEvents` **无限循环**，导致 `debug_events` 卡住、进而让**整条 ctest 超时**；已加总超时兜底并在注释写明「强烈建议非 0」；测试：有事件取走全部且消耗后 Pending 归零、空采集器返回 0 不产生空批、sink 返回 false 提前停止、压测 1000 批（205 µs）、随机 50 组批数不超上限；CLI 侧覆盖裸 `--follow`、`--follow=N`、双间隔参数、`--follow=0`/`=abc`/负间隔/负超时四类非法值报错、不带 follow 行为不变、端到端报告 followed N batches | `debug_events.hpp`、`debug_cli.hpp`、两个测试文件；**全量 ctest 80/80** |
| 22 | **任务1 收尾-C：事件采集线程局部批量发布 + 修 1 处 flaky + 修 1 处测试超时**：`EventBuffer::PushBatch(data,count)` 一次预留后拷贝，把「读 head/判满/写 head」的原子操作**从 count 次降到 1 次**（缓冲本已无锁，故省的是原子操作而非锁争用）；`OnEvent` 改为**线程局部暂存 + 满 32 条一次提交**并处理同线程切换采集器的旧暂存排空；`Pending/Captured/Dropped/Peek/Drain` 读取前先 `FlushStaging()`（首版未 Flush 导致 3 处既有断言失败）；**如实记录固有代价**：其它线程暂存只能等各自下次调用或进程退出；**修 flaky**：`debug_lock_selector` 原断言「选择器指定队尾 → 线程 2 先拿锁」不成立（线程入队顺序不确定），改为统计 selector 被调用次数，连跑 5 次 0 失败；**修测试超时**：`debug_events` 随机测试原为 50 组 × 500ms 超时（最坏 25s），且事件数可能为 0 导致每组等满超时 → 曾把 ctest 拖成 Timeout；改为事件数 1..40 且超时压到 20ms | `debug_events.hpp`、`test_debug_events.cpp`、`test_debug_lock_selector.cpp`；构建 0 warning，**全量 ctest 80/80**（30.79s）|
| 23 | **任务1 收尾-D（最后一项）：跨进程 dump（信号驱动）**：新建 `debug_signal_dump.hpp`（230 行）——`DebugSignalDumper::Install/Uninstall/Pending/PollOnce/DumpCount/StartWatcher/StopWatcher`，用 `SIGUSR1` 触发；**关键设计**：信号处理器里**只做一次 `atomic<bool>` 置位**（满足 async-signal-safe，绝不做 I/O/分配/加锁），真正的 dump 由 `PollOnce`（主循环）或 watcher 线程完成；dump 内容含 `include_fibers` 的全 G 列表，若绑定了采集器还额外写 `<path>.events.ini`；**如实说明**：未 Install 时 SIGUSR1 走内核默认动作（终止进程）、本模块不接管 SIGUSR2/SIGQUIT；测试 `test_debug_signal_dump.cpp`（228 行，5 场景 + 压测 + 随机）：未安装时 PollOnce 报错、`raise(SIGUSR1)` 后 Pending 为真且产生含 `[scheduler]` 与 fibers 段的文件、连续两次信号产生两个文件、watcher 线程模式、Uninstall 后不再接管；压测 `PollOnce`（无请求）**7 ns/次**；随机 8 组校验 dump 计数一致 | `debug_signal_dump.hpp`、`test_debug_signal_dump.cpp`；构建 0 warning，**全量 ctest 80/80**。**至此任务1 的 4 项未做项全部完成** |
| 24 | **任务1 的 example 完善**：`example/debug/debug_module_demo.cpp` 从 7 节扩到 **10 节**，新增 ⑧ 枚举全部 G（造一个长命阻塞 G，用 `SnapshotWith({include_fibers=true})` 列出存活 G 的 id/state/queued/started/failed，并断言 text 含 fibers 段、默认 json 仍为 null）；⑨ `dump events --follow=2` 解析与端到端（真实打印逐事件 JSON，报告 followed N batches）；⑩ 信号驱动跨进程 dump（`Install` → `raise(SIGUSR1)` → `Pending` → `PollOnce` 写出快照并清理），并在示例里说明真实场景应使用 `kill -USR1 <pid>`；新增 `GStateLabel` 辅助函数；示例自报「所有演示步骤通过」，构建 0 warning | `example/debug/debug_module_demo.cpp`；ctest 80/80 |
| 25 | **任务2：G3（取消与错误）+ G4（并发原语）两图** —— `diagrams/G3-context-error.md`（218 行、5 个 Mermaid）覆盖 Context / DoneSignal / ContextRollback / Error 家族，画出取消向下游传播、回调在**锁外**执行、undo **逆序**回滚、Context 与 ContextRollback 各自状态机；`diagrams/G4-concurrency.md`（247 行、5 个 Mermaid）覆盖 Mutex / ConditionVariable / WaitGroup / WaitSelector / RingBuffer / SelectWaitState / Channel / FutureState / Promise / Future，画出 Mutex 的 CAS 快路径 + 受管 G 有界协作重试 + **所有权无缝转移**（并注明 D3 只重排队列故不影响该语义）、Channel 的有缓冲/无缓冲两路与 select 的 CAS 单赢家 + generation 守卫、Future 的 variant 三选一；状态流转给出 **Channel 9 状态**与 **WaitNode 交接状态**；两图均含与 Go 的差异表 | `ArchitectureDiagram/diagrams/G3-context-error.md`、`G4-concurrency.md`、README 索引 |
| 26 | **任务2：G5（IO 后端）+ G6（网络基础）两图** —— `diagrams/G5-io-backend.md`（195 行、5 个 Mermaid）覆盖 IOBackend/BackendEvent/BackendEventFlag/FdManager/FdContext/ThreadHookMode/ScopedEnable，画出「看起来阻塞」的读如何变成 试一次→EAGAIN→注册事件→park→就绪重试 的循环，并强调 **user_nonblock 与 system_nonblock 两套标记**（运行时为了挂起设非阻塞，但用户语义不被破坏）、hook 只对受管 G 生效、超时记在 FdContext；状态流转给出 fd 在 hook 下的状态与线程 hook 模式；`diagrams/G6-network-base.md`（239 行、5 个 Mermaid）覆盖 Address 继承体系 / Socket / SocketWaiter / SocketStream / Framer / ByteArray / EndianCodec，画出 Accept 的 park-重试循环、**Framer 的拆包与 max_frame_size 超长防护**、ReadFull 的短读处理、SetDeadline 只影响等待不关连接；状态流转给出 Socket 生命周期与 Framer 缓冲状态（含粘包）；两图均含与 Go 的差异表 | `ArchitectureDiagram/diagrams/G5-io-backend.md`、`G6-network-base.md`、README 索引 |
| 27 | **任务2：G7（应用协议）+ G8（横切设施）两图** —— `diagrams/G7-protocols.md`（269 行、**6 个 Mermaid**）覆盖 http（Headers/Request/Response/Parser/ResponseWriter/Server/Client）、websocket（WebSocket/WebSocketMessage/WebSocketOpcode/WebSocketConfig/WebSocketRole/WebSocketError/Result）、sse（Event/Parser/Stream/StreamState/StreamOptions/ParserLimits），画出一次请求的 accept→Feed→解析→handler→写响应→keep-alive 循环、WebSocket 的握手校验与 Ping **自动回 Pong**、**SSE 必须 Flush**（不 flush 客户端收不到，最容易踩的坑）、Framer 式的 `ParserLimits` 与 `max_message_size` 防内存耗尽；状态流转给出 HTTP 连接、WebSocket 连接、SSE 流三个状态机；`diagrams/G8-log-config.md`（246 行、5 个 Mermaid）覆盖 LogSink/FileSink/RotatingFileSink/Stdout/Stderr/Memory/Callback sink 与 LogFilter/LogFormatter 基类 + MinimumLevelFilter/PatternFormatter + Logger/LoggerManager/LogRecord/LogLine，以及 IniFile/ValueCodec 特化/ConfigVarBase/ConfigVar/Config/RuntimeConfig；画出 **过滤在格式化之前**（被丢的日志不做字符串格式化）、每个 FileSink 用自己的锁、RotatingFileSink 写前检查大小、配置解码失败**保留旧值并报错不静默降级**；状态流转给出 Logger 配置状态与 ConfigVar 生命周期；两图均含与 Go 的差异表 | `ArchitectureDiagram/diagrams/G7-protocols.md`、`G8-log-config.md`、README 索引 |
| 28 | **任务2：G9（工具与新手层）+ G10（调试模块）两图 —— 10 张模块图全部完成** —— `diagrams/G9-tools.md`（249 行、5 个 Mermaid）覆盖 go()/fiber/DefaultSchedulerState/defer/EventBatch(SelectLoop)/HybridGate/FiberLocalCache/thread_policy 系列，画出 go 的 detach 语义、defer 作为 RAII 的**异常被吞**差异、EventBatch::Wait 的 Arm→CAS 单赢家→Disarm 回滚、HybridGate 的 G 与线程双路径共享同一门状态；状态流转给出 EventBatch 一次 Wait 与 HybridMutex 双路径；`diagrams/G10-debug.md`（315 行、**6 个 Mermaid**）覆盖 DebugFiber/DebugRegistry/DebugScheduler/TaskSnapshot/DumpCommand/DebugFileSink/DebugMutex 等三代理/WaitSelector(D3)/EventBuffer/DebugEventCollector/FollowEvents/DebugBreakpoints/Checkpoint/DebugSignalDumper，画出 dump 三条路径（含 **include_fibers 按 16 分片遍历**、**原子写 tmp+rename**、**FollowEvents 必须带超时否则无限循环**）、D3 的只重排队列、线程局部批量 PushBatch、信号 handler 只置原子标志；并**用表格标出每个类的侵入性**（DebugFiber 继承、其余代理、原类仅 sync 加约 8 行 / D3 约 20 行）；状态流转给出 dump 请求、协作断点、D3 决策三个状态机 | `ArchitectureDiagram/diagrams/G9-tools.md`、`G10-debug.md`、README 索引；**G1~G10 全部有图** |
| 29 | **任务2 收尾：最终核对 + 更新 ArchitectureDiagram/README** —— 核对结果：10 个图文件齐全、**52 个 Mermaid 块**经静态检查无问题（图型合法、flowchart 的 `[]`/`{}`/`()` 括号配对、stateDiagram 有转移、无全角引号）、每张图结构统一（classDiagram×1 + flowchart×1 + graph×1 + stateDiagram×2~3）、索引 10/10 已完成，**全量 ctest 80/80**；并把 `ArchitectureDiagram/README.md` 从旧的「modules 文字报告为主体」改写为**以 diagrams/ 为交付主体**：给出「想看什么去哪里」导航表、四章节与图型对应表、10 张图的文件/行数/Mermaid 数总表（合计 2611 行 52 图）、渲染方式、以及 5 条维护约定 | `ArchitectureDiagram/README.md`；README 里明确说明旧 modules/ 保留为注解来源而非交付形态 |
| 30 | **补做承诺的量化：事件采集批量化收益（build-bench）** —— 给 `DebugEventCollector` 加 `SetFlushBatch(n)`/`FlushBatch()`（线程局部生效，可设 1 退化为逐条以便对比），新建 `benchmarks/debug_event_collector_benchmark.cpp`（155 行，登记 GO2CPP_BUILD_BENCHMARKS），测 flush_batch = 1/16/32 × 单线程/4 线程；结果：**单线程 22.47 → 19.65 ns/事件（约 12.5%）**，4 线程 11.69 → 7.65（约 35%）但**方差显著**（min 7.16 / max 11.81），**16 与 32 无可测差异（16 已接近饱和）**；并如实说明「4 线程绝对值反而更低」是分母被并行摊薄所致、**不能跨场景比较单次成本**；数据写入 docs/performance.md 新节 | `debug_events.hpp`、`benchmarks/debug_event_collector_benchmark.cpp`、`cmake/Go2CppTests.cmake`、`docs/performance.md` |
| 31 | **按使用者要求：调试模块不影响原框架性能 —— 修掉 D3 在热路径上的残留开销** —— 复查发现 `Mutex::Unlock` 慢路径里，`ReorderWaitersForSelector` 虽有 `waiters.size()<2` 快速返回，但**≥2 个等待者**时仍会 `CurrentWaitSelector()` → 加 `s_selector_mutex` + 拷贝 `shared_ptr`（约 30~60 ns）；按「必须不影响原模块性能」的要求，新增 `std::atomic<bool> s_has_selector` 做**二级快判**，未注册选择器时**只读一次原子量、不取锁、不拷 shared_ptr**，做到真正零开销；`SetWaitSelectorFactory` 用 release 先发布对象再置标志、`ResetWaitSelectorFactory` 先清标志再清对象，保证读到 true 的线程一定能看到非空选择器；**另做结构性验证**：`grep` 确认 `include/go2cpp/`（除 `debug/`）与 `src/` **零处 include debug 头**，依赖是单向的 → **删掉整个 debug 模块框架仍可编译、行为不变** | `src/concurrency/sync.cpp`；构建 0 warning、全量 ctest 80/80、选择器测试连跑 3 次 0 失败 |
| 32 | **零影响验证 + 报告附录** —— 按使用者要求「调试不影响原框架」：① 修掉 D3 在 `Mutex::Unlock` 慢路径的残留加锁（`std::atomic<bool>` 二级快判，未注册时零锁零拷贝，`b8ff33d`）；② 结构性保证并用实证确认：把源码树复制到 `/tmp`、剔除全部 debug 文件与 CMake 条目后，**configure 与构建均成功**（即删掉整个 debug 模块框架照常编译）；③ 对照组实测（`main` 无 debug vs HEAD 含 debug 不启用）：被改动路径全部 **0.95~1.03x**，超过 10% 的只有不含本次改动的对照组（原生 `std::thread` 1.41x、基准自带 `BoundedQueue` 1.36x）且**方向不一致** → 判为噪声；新增 `zero-overhead-verification.md`；另给 `DEV-FINAL-REPORT.md` 追加**附录 A**，修正它只反映 16 个提交的过期状态 | `sync.cpp`、`zero-overhead-verification.md`、`DEV-FINAL-REPORT.md`；ctest 80/80 |
| 33 | **实机渲染验证（重要）** —— 用 `@mermaid-js/mermaid-cli` 渲染 11 个文件：首轮仅 **30/56 成功**，**26 张失败**，而在此之前我的静态检查（图型合法/括号配对/有转移箭头/无全角引号）**全部通过** —— 证明静态检查不足。修复 **9 处**：6 处节点标签内嵌半角引号（Mermaid 把内层引号当标签结束符）、1 处非法关系箭头（G5 的 `<-..` 改 `..>`）、1 处 stateDiagram 标签里的 `::`（G10 改 `.`）、1 处偶发 404（重试即过）；最终 **56/56 全部渲染成功**；新增 `mermaid-render-verification.md` 记录方法与教训（含 mermaid-cli 的产物命名坑：只生成 `<base>-N.svg` 而非 `<base>.svg`） | `diagrams/G4,G5,G8,G9,G10`、`mermaid-render-verification.md`；56/56 渲染成功 |
| 34 | **可读性修复（实机视觉检查发现的）** —— 把 G10 的 classDiagram 从 **19 类 / 132 行** 拆成 **3 张各 7~8 类**（①-A 核心链路 / ①-B 锁与 D3 争用 / ①-C 事件·CLI·断点·信号）。原因：19 类一次性画会被 Mermaid **横向铺开**（实测渲染成 784×242 的极扁条，文字小到不可读），违反使用者「一图一屏可读」的要求；拆分后 784×428，文字清晰。另修 G2 状态机的 **4 处裸换行**（Python 的 `\n` 变真换行 → Mermaid 把续行当成独立状态，渲染后顶部出现一排孤立节点），改用 `<br/>`；**记录一次我造成的破坏**：曾用批量脚本「合并续行」，把 `subgraph` 后的节点定义也合并，**导致 11 个文件全部解析失败**，已用 `git show 82f8c91:...` 恢复（避免 checkout/restore），再只精确修 G2 的 4 处 | `diagrams/G10-debug.md`、`G2-scheduler.md`；渲染 56/56 全绿 |
| 35 | **可读性修复（第二轮·自动检测）** —— 写脚本用 **PNG 宽高比**自动找出被横向铺开的类图（宽/高 > 2.2 判为扁条）：实测 **G8 为 3.10（21 类，784×253）**、**G9 为 3.41（18 类，784×230）**，其余 7 个（G1 1.04 / G3 1.26 / G4 1.73 / G5 0.87 / G6 1.06 / G7 1.34）均正常；把 G8 拆成 3 张（Sink 家族 / 过滤·格式化·写入 / 配置）、G9 拆成 3 张（go 与 defer / select 与混合互斥 / FiberLocal 与线程策略），拆分后 6 张宽高比全部落在 **1.12~2.00**；并补回被我的替换误删的 G9 类职责表格 | `diagrams/G8-log-config.md`、`G9-tools.md`；渲染全绿 |
| 36 | **全量宽高比扫描 + 消除最后一张扁条** —— 把检测从「只看类图」扩展到**全部 62 张图**（Pillow 读 PNG 尺寸，宽/高 > 2.2 判扁）：结果**只有 G10 第 2 张偏扁（784×312，2.51）**；先尝试精简箭头（无效，反而 3.05），最终**把它拆成 ①-B 锁观测代理（6 类）与 ①-B′ D3 争用选择（2 类，新增 `WaiterCandidate` 输入视图类）**，拆分后 G10 共 9 张、**扁条 0**、全部宽高比落在 **0.32~2.09** | `diagrams/G10-debug.md`；63 张图全部渲染通过 |
| 37 | **文档与实测数字对齐** —— 发现 `diagrams/README.md` 与 `mermaid-render-verification.md` 仍写着「56 张图 / 2774 行」，而 G8/G9/G10 拆图后实际已是 **63 张 / 2862 行**，属交付文档自相矛盾；已按实测逐文件数字重建 README 的文件清单表（含每文件行数与 Mermaid 数），把问题清单从「9 处」更新为「14 处」（新增 4 处裸换行 + 3 处扁条），并给渲染验证文档补第 8 节说明后续拆图与方法学收获（PNG 宽高比自动检测可读性）；README 的维护约定也从 9 条扩到 10 条（新增「单张类图类数控制在 12 个以内」与「检查宽高比」）| `diagrams/README.md`、`mermaid-render-verification.md`；数字与实测一致 |
| 38 | **futex 重构任务启动 + ParkingCondition 基线基准** —— 使用者指定新方向：把基础并发组件从「原子锁 + std::mutex/condition_variable」重构为**直接基于 futex 的无状态锁**，不改变上层接口、功能相同。已查明 `parking_condition.cpp`（337 行）当前用 `HybridMutex` + **`std::condition_variable_any`** + `std::mutex` + `atomic<Result>`，每次都穿过 glibc 的 cv 层；全项目 std::mutex 约 500 处（scheduler 95 / context 56 / io 38 / log 36 / **sync 31** / timer 30 / fiber 25 / future 22 / channel 17 / **parking_condition 7**），故聚焦使用者点名的两个：ParkingCondition 与 sync::Mutex。新增 `benchmarks/parking_condition_benchmark.cpp`（已登记 GO2CPP_BUILD_BENCHMARKS）测三档，**基线（负载 0.08）：notify 空转 15.30 ns/次、2 消费者争用 90.76 ns/次、4 消费者争用 4351.02 ns/次（是 2 消费者的 48 倍）** —— 直接量化了 cv 在多样等待者下的退化 | `benchmarks/parking_condition_benchmark.cpp`、`cmake/Go2CppTests.cmake`；构建通过 |
| 39 | **sync::Mutex 的普通线程等待路径迁移到 futex** —— 与 ParkingCondition 同一模式，但更简单：每个 `WaitNode` 自带一个 `alignas(64) atomic<uint32_t> m_futex_seq`，`notify_one` 只推进该 node（无需共享代际与等待者计数）；`WaitNative` 走 **自旋 128 次 → `FUTEX_WAIT(代际)` → 重判终态**。已确认 `m_native_mutex`/`m_native_condition` 只有 2 处使用，可用 `#if defined(__linux__)` 完整隔离，非 Linux 保留原 cv 实现。**全量 ctest 100% passed, 80/80, 34.13s**。但本次基准（`debug_lock_selector_benchmark`，负载 1.42）**未测出可信改善**：档1 median 22,488.77 ns/op，但 5 轮区间 3,812.82~26,655.01（方差近 7 倍），与改造前记录的约 18,000 ns/op 无法做有效对比 —— 如实记录，不下结论 | `src/concurrency/sync.cpp`；ctest 80/80 |
| 40 | **发现并修掉 sync::Mutex 3+ 线程跳变的直接原因（自旋额度被浪费）** —— 用专项探针隔离出「必进等待路径」的成本：单线程 45.68 ns、**2 线程 84.39 ns**、**3 线程 6,097.45 ns**、**4 线程 36,014.44 ns** —— 与 worker 扩展性劣化**完全同构**（1→2 正常、3 起数量级跳变）。读码定位到 `LockImpl` 的普通线程自旋条件是 `(m_state & kWaiter) == 0`：**一旦有等待者就立即退出自旋**，而 3+ 线程时 `kWaiter` 常驻，于是普通线程**完全得不到自旋机会**，每次 unlock 都要唤醒挂起的线程。但该函数注释本就承诺「一旦有普通线程 waiter，最多再重试 4 次后回到 FIFO」—— 实现把这个额度浪费了。改为**两段预算**（无等待者 256 次 / 有等待者 4 次），越过额度后严格回 FIFO，`TryLock` 走另一条路径故「不能插队」语义不变。实测：B2 84.39→**74.09**（快 12%）、**B4 36,014→1,709.83（快 21 倍）**、B3 无改善（6,097→6,902，噪声内，4 次额度对 3 线程不够）| `src/concurrency/sync.cpp`；全量 ctest 80/80 |

## 下一步（第 4 轮）

1. **跑通全量验证**：`GO2CPP_TEST_FILTER=debug_fiber ./build-quality/go2cpp_tests` 与 `ctest`；
2. **P0 继续**：`emit_event` 微基准（三档：不调用 / observer 为空 / 空回调），
   这是设计文档中"零开销证明"的第一步，成本最低；
3. 之后开始 R1/R2 只读快照接口的**可行性设计**（注意：`Task`/`Scheduler` 无虚析构，
   按 `design/09` 的档 A/档 C 策略处理）。

## 约束提醒（每轮自查）

- 不改动原框架文件（`Fiber`/`Task`/`Scheduler`/`sync` 等）：本模块只**新增**文件；
- 测试必须含压测 + 随机 + ≥5 真实场景，新增测试必须登记 CMake；
- 性能结论只用 `build-bench`（Release）；每个逻辑单元跑 `build-quality` + `ctest`；
- git 只 add 明确路径；不提交使用者此前在途的 4 个文件。

---

## 收尾状态（2026-10-16 17:50，按使用者要求停止定时任务）

**本段为任务状态的快速记录。**

### 交付状态

| 项 | 值 |
|---|---|
| 分支 | `feature/gmp-debug-module` |
| 提交数 | **16**（`b7f172a` → `39fa255`） |
| 阶段完成度 | **P0 / P1 / P2 / P3（含 D3）/ P5 全部完成**；**P4 按使用者定案不做**（属结构类） |
| 全量测试 | **ctest 80/80 通过**（含新增的可运行示例用例） |
| 主交付报告 | `DebugTooling/DEV-FINAL-REPORT.md` |
| 对原框架改动 | 仅 `sync.hpp` **+104** 与 `sync.cpp` **+104**（**纯新增、0 删除**），其余全部为新增文件 |

### 主要交付物

- 源码：`include/go2cpp/debug.hpp` + `debug/{fiber,scheduler,cli,sync,events,breakpoints}.hpp`（6 个子模块）
- 测试：`tests/debug/test_debug_{fiber,scheduler,cli,sync,lock_selector,events,breakpoints}.cpp`（7 个）
- 基准：`benchmarks/debug_{observer,lock_selector}_overhead_benchmark.cpp`（2 个）
- 示例：`example/debug/debug_module_demo.cpp`（可运行，一条命令看全部能力）
- 文档：`DEV-FINAL-REPORT.md`、`DEV-PROGRESS.md`、`diag-worker-scaling.md`、
  `perf-observer-overhead.md`、`design/09-debug-class-conventions.md`、`design/10-final-class-inventory.md`

### 未决项（3 项，均**未**解决，详见最终报告 §7）

1. 调度器 **3 worker 起数量级劣化**（1→2 正常，2→3 跳变约 9.8 倍，4 worker 约 13~16 倍）；
   已诊断并**排除**固定启动开销，但**未做 profiling**，具体机制未确认；
   与调试模块无关（全程 `observer=nullptr`）。
2. `sync::Mutex` 4 线程竞争约 **18 µs/op**（单线程无竞争仅 3.7 ns/op），属其自身行为。
3. 一次默认参数基准运行超 15 分钟未结束，根因未定位；结论不依赖该次运行。

### 未做项（已在报告中标注原因）

- P2 的 `--follow`（持续跟随输出）；
- "枚举全部 G" 接口（`dump scheduler` 的 `fibers` 字段输出为 `null`）；
- 跨进程 dump（需信号/注入机制）；
- 事件采集的"每 worker 线程局部暂存 + 批量发布"（已标注为后续优化）。

### 工作树

仅有使用者此前在途的 4 个文件未提交，**本次开发没有误带入**：
`example/concurrency/future_full.cpp`、`include/go2cpp/logging/log.hpp`、
`include/go2cpp/tools/error.hpp`、`src/tools/error.cpp`。
`.dsh_tmp/` 仅含一个 2026-10-15 的旧文件 `stack_probe.csv`（非本次会话产生，未改动）。

### 定时任务

按使用者要求**已停止**（`schedule-102ff7f6-f909-4a29-958e-5bdcd6855f4c` 已删除）。
**注意**：目标（goal）保持 `active`，未标记完成 —— 因为使用者只要求停止定时任务，
未表示整体目标已达成；如需继续开发，恢复该目标即可接着做上面的未做项。
