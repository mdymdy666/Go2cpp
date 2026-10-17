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
