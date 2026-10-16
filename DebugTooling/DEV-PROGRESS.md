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
