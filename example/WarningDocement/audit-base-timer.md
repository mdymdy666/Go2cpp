# timer 模块审计报告（只读）

- 审计日期：2026-10-30
- 工作树：`/UserData/CodexWorkSpace/Go2cpp`（WSL2 Ubuntu，12 核 / 20 GB）
- 构建：`.build-bento-wsl`（Release，`-O2`，`GO2CPP_BUILD_TESTS=OFF`）
- 边界：**只读 + 跑测试 + 写报告**，未修改任何实现文件，未 commit

## 一、结论

| 维度 | 结论 | 证据 |
|---|---|---|
| 压测（10 万） | **通过**：注册 82.8 万/s、取消 415 万/s、10 万全部触发无丢失 | §三 |
| 精度 / 漂移（ms 级） | **优秀**：1/10/100 ms 三档 P99 ≤ 0.43 ms，尾延迟 ≤ 0.43 ms | §四 |
| 随机（1000+） | **通过**：32000 次随机操作，统计不变量成立，ASan/TSan/valgrind 全干净 | §五 |
| 五场景 | **全部通过**（精度、大队列、取消竞态、60 s 长稳、时钟回退） | §六 |
| 解耦 / 自包含 | **通过**：`timer.hpp` 零项目内依赖（只用 9 个标准库头），`timer.cpp` 只依赖 log/debug 基础层 | §七 |
| 测试复查 | **测试全部通过，但存在 2 个 P1 级测试缺陷**（恒真等待、随机取消 100% 取消） | §八、§九 |

本次审计**未发现实现功能缺陷**：10 万级压测、取消竞态、时钟跳变、内存检查均无异常。发现的问题集中在**测试有效性**与**文档漂移**。

## 二、检查对象与路径勘误

任务书写的 `include/go2cpp/{timer,timer_full}.hpp` **在当前工作树不存在**。实际路径：

| 任务书 | 实际路径 | 说明 |
|---|---|---|
| `include/go2cpp/timer.hpp` | `include/go2cpp/core/timer.hpp` | 4 行转发头（兼容旧路径） |
| `include/go2cpp/timer_full.hpp` | `include/go2cpp/concurrency/core/timer.hpp` | 505 行，唯一实现头 |
| `src/concurrency/timer*.cpp` | `src/concurrency/timer.cpp` | 1343 行，单文件 |

`timer_full` 目前只是 `example/concurrency/timer_full.cpp`（示例），不是头文件。本报告按实际路径审计。

补充：`git log` 显示 `dce497a` 曾引入 TimerBin 对象池，随后 `b8fc1c0` 回退（"实测仅 0.7%，噪声内"），当前实现**没有对象池**——这直接影响测试复查结论（见 §八.3）。

## 三、压测：10 万定时器 创建 / 触发 / 取消

条件：独立 `TimerService` 实例，单线程注册，回调体为一次 `atomic fetch_add`，10 万个定时器共用同一 deadline（注册完成后 400 ms）。

| 指标 | 实测 |
|---|---|
| 注册 10 万个 | **120.739 ms** → **828,230 次/秒** |
| 注册+全部触发总耗时 | 420.179 ms（含 400 ms 计划延迟） |
| 全部触发窗口 | 299.440 ms |
| 尾部延迟（最后回调 − deadline） | **19.902 ms** |
| 创建 10 万个 1h 定时器 | 65.760 ms → 1,520,689 次/秒 |
| 取消 10 万个 | **24.079 ms** → **4,152,917 次/秒** |
| 注册后 RSS | 44,700 KB（基线 3,840 KB，**+39.9 MB**，≈ 418 B/定时器） |
| 触发后 RSS | 44,700 KB（不增长） |
| 统计不变量 | `armed=100000, completed=100000, cancelled=0, failed=0, active=0` |

同一 deadline 的 10 万个定时器（FIFO 场景）：

| 指标 | 实测 |
|---|---|
| 注册 | 98.674 ms |
| 全部触发 | 317.815 ms |
| 尾部延迟 | 17.815 ms |
| 触发吞吐 | 456,327 次/秒 |
| **FIFO 违规** | **0**（10 万条按入队顺序执行） |
| RSS | 48,444 KB |

**解读**：尾部 17.9~19.9 ms 是 10 万个回调在 timer 单线程上串行执行的成本（约 180 ns/个），不是调度抖动。`Run()` 每轮只从 `multimap` 摘一个到期项（timer.cpp:961-963），10 万同刻到期时需 10 万次加解锁 + 10 万次回调，这解释了该数字。属于设计取舍（单线程执行模型），不是缺陷；若需要并行应走 `TimerExecutor`。

## 四、精度与漂移（ms 级）

空载、独立服务、逐个性触发（`condition_variable` 等待），延迟 = 实际触发 − 计划 deadline。

| 目标 | P50 | P95 | P99 | max | 均值 | 样本 |
|---|---|---|---|---|---|---|
| 1 ms | 0.114 | 0.165 | **0.208** | 0.226 | 0.121 | 200 |
| 10 ms | 0.153 | 0.226 | **0.242** | 0.266 | 0.157 | 200 |
| 100 ms | 0.253 | 0.395 | **0.424** | 0.427 | 0.255 | 50 |

单位 ms。固定速率周期任务跑 2 s 的漂移（本次触发 − 计划时间）：

| 周期 | ticks / 期望 | 漂移 P99 | 漂移 max |
|---|---|---|---|
| 1 ms | 1999 / 2000 | 0.203 | 0.398 |
| 10 ms | 199 / 200 | 0.268 | 0.387 |
| 100 ms | 20 / 20 | 0.250 | 0.312 |

结论：**亚毫秒级精度**，1 ms 档 P99 0.21 ms、max 0.23 ms，没有累积漂移（kFixedRate 下 2 s 内 1 ms 周期只少 1 拍）。精度不受队列规模影响的部分见 §六.2（大队列下尾部延迟由回调执行决定）。

## 五、随机测试（1000+）

4 线程 × 8000 操作 = **32000 次**随机操作，固定种子（`0xA11CE + tid*7919`）。操作分布：一次性创建、`ScheduleEvery`（max_runs 1~3）、`ScheduleRepeat`（Continue / ContinueAfter / ContinueAt / Stop）、`Cancel`、`Reset`、`Refresh`、状态查询、句柄析构、`ScheduleOn`（拒绝型执行器）、`ScheduleWeakAfter`（owner 存活/失效各半）、`ScheduleEveryOn`（计数执行器）。

| 指标 | 实测 |
|---|---|
| armed / completed / cancelled / failed / active | 18724 / 5248 / 13113 / 363 / 0 |
| 不变量 `armed = completed+cancelled+failed+active` | **成立** |
| 实际执行的用户回调 | 6909 次 |
| `ScheduleOn` 创建数 | 889（其中 363 个按预期 failed） |
| 进程 RSS | 4,992 KB |
| 崩溃 / 挂起 | 无 |

内存与竞态检查：

| 工具 | 场景 | 结果 |
|---|---|---|
| valgrind 3.22 | leak_probe（触发/取消/周期/执行器/弱引用/异常） | `6045 allocs, 6045 frees, 0 bytes in use at exit`，**0 errors** |
| ASan (`-fsanitize=address`) | `race`（16000 定时器 + 200 周期） | 无报告、无泄漏 |
| ASan | `random`（32000 操作） | 无报告、无泄漏 |
| TSan (`-fsanitize=thread`) | `race` | 无 data race 报告 |
| TSan | `random` | 无 data race 报告 |

多轮泄漏趋势（5 轮 × 2 万次创建/取消）：

| 轮次 | 0 | 1 | 2 | 3 | 4 |
|---|---|---|---|---|---|
| RSS (KB) | 9,212 | 8,896 | 9,016 | 8,896 | 8,896 |

首轮后完全平稳，增长 4.9 MB 属分配器首次缓存，**无泄漏迹象**。

## 六、五场景

### 6.1 精度：1 ms / 10 ms / 100 ms

见 §四。三档均达标，1 ms 档在 WSL2（HZ=1000 宿主）下 P99 0.208 ms。

### 6.2 大量定时器

10 万个同一 deadline，验证不丢、不重、FIFO：

- 触发数 100000 / 100000，`sequence` 槽位恰好写满
- 顺序违规 **0**
- 尾部延迟 17.815 ms（回调串行成本，见 §三）
- 统计：`completed=100000, cancelled=0`

额外观察（`DebugSnapshot` 在超大队列上的持锁成本）：

| 接口（10 万排队） | 单次耗时 |
|---|---|
| `DebugSnapshot()` | **8.851 ms** |
| `PendingCount()` | ~0.005 µs（1000 次合计 5 µs） |
| `NextDeadline()` | 0.014 µs（1000 次合计 14 µs） |

`DebugSnapshot` 在 `m_mutex` 内遍历全部 `m_entries` 统计来源分布（timer.cpp:834-840），10 万队列下单次约 **8.9 ms**，期间定时器线程无法摘取到期任务。文档只说"只读、不分配内存"，未提示 O(n) 持锁。属于 P2 风险（dump 通道可能推迟到期回调），不是功能缺陷。

### 6.3 取消竞态

8 线程 × 2000 = 16000 个一次性定时器，每个随机延迟 0~3 ms，60% 概率在随机 0~3 ms 后取消；句柄统一保留到检查结束（避免析构即取消）。

| 指标 | 实测 |
|---|---|
| 完成（run_count=1） | 11,370 |
| 取消时 `RunCount()==0` 且状态 kCancelled，之后仍触发 | **0** |
| 重复触发（count>1） | **0** |
| 从未触发且非"取消成功" | 0 |

周期定时器竞态：200 个 1 ms 周期任务并发取消，取消后 50 ms 内 `RunCount` 变化数 **0**。

结论：取消语义与文档一致——**取消只阻止"尚未领取"的回调**；已进入 `kRunning` 的回调会执行完（`Impl::Cancel` 对 kRunning 也置 kCancelled 并返回 true，timer.cpp:451-487）。因此 `Cancel()` 返回后不能据状态推断"回调一定没跑"，测试也按"取消时 RunCount==0 才要求之后不触发"来断言。这是本次审计认为最需要写进文档的一条语义。

### 6.4 长稳 60 秒

5 ms 固定速率周期任务 + 后台扰动线程（每 20 ms 创建 20 个短定时器、随机取消一半）。

| 指标 | 实测 |
|---|---|
| ticks | **12,007**（按实际运行时长期望 12,003.99，比值 1.000） |
| 漂移 P50 / P99 / max | 0.120 / 0.356 / **2.253** ms |
| RSS 0 s → 60 s | 4,096 → 4,480 KB（**+0.375 MB**） |
| 结束时 `PendingCount()` | 0 |
| 统计不变量 | 成立 |

每 10 s 采样：ticks 2001 / 4002 / 6003 / 8003 / 10003 / 12003，线性无累积误差；RSS 10 s 后稳定在 4,352 KB。

### 6.5 时钟回退

真实修改系统时钟会污染宿主与其他并行任务，因此用 **LD_PRELOAD 注入**：只对 `CLOCK_REALTIME` 加偏移，`CLOCK_MONOTONIC` 原样透传，验证 timer 的 `steady_clock` 调度是否受真实时间跳变影响。

| 检查 | 结果 |
|---|---|
| 注入是否生效 | `CLOCK_REALTIME` 回退 −3600.000 s |
| 回退状态下 10 / 100 / 500 ms 定时器延迟 | 0.125 / 0.070 / 0.087 ms |
| 等待中途回退（100 ms 处 −2 h，200 ms 处恢复 −1 h）的 300 ms 定时器 | **300.080 ms** 触发，未提前/延后 |
| 前跳 +1 h 后 50 ms 定时器 | 0.104 ms |
| 回退状态下 10 ms 周期任务 300 ms 内 | 29 ticks（期望 30） |
| 恢复后 offset | 0，时间误差 0.964 s（测试本身耗时） |

结论：timer 全部使用 `steady_clock`（`TimerService::Clock`），真实时间回退/前跳**不影响**调度。代码中无 `system_clock` 参与超时计算（grep 确认）。唯一副作用是日志时间戳会随系统时钟回退（log 模块用 `system_clock`），与 timer 调度无关。

## 七、解耦与头文件自包含

### 7.1 头文件自包含

`include/go2cpp/concurrency/core/timer.hpp` 与 `include/go2cpp/core/timer.hpp` 单独编译（`-std=c++17 -Wall -Wextra -Wpedantic -fsyntax-only`）**均通过，零警告**。

直接依赖只有 9 个标准库头（timer.hpp:32-40）：
`<array> <chrono> <cstddef> <cstdint> <exception> <functional> <memory> <mutex> <optional>`

`g++ -MM` 展开的**传递依赖中没有出现任何 `go2cpp/` 路径**——即 timer 头文件不依赖项目内任何模块（比"只依赖 base"更强）。

### 7.2 实现依赖

`src/concurrency/timer.cpp` 直接 include 3 个项目头（timer.cpp:35-37）：

| 头文件 | 层次 | 是否反向依赖 scheduler |
|---|---|---|
| `go2cpp/core/timer.hpp` | 自身 | — |
| `go2cpp/logging/log.hpp` | 基础层（log） | 否（只有注释提到 fiber） |
| `go2cpp/debug/debug_event_ring.hpp` | 基础层（debug，OFF 构建下宏展开为空） | 否（只有注释） |

传递依赖全集（`-MM`）：`core/timer.hpp → concurrency/core/timer.hpp → logging/log.hpp → debug/debug_event_ring.hpp → debug/debug_config.hpp`。**无 scheduler / fiber / network / io 依赖**。

### 7.3 依赖方向

反向依赖（谁 include timer.hpp）：`concurrency/channel.hpp`、`concurrency/scheduler/scheduler.hpp`、`debug/debug_dump.hpp`、`runtime.hpp`、`src/concurrency/parking_condition.cpp`。方向正确：上层（channel/scheduler）依赖 timer，timer 不反过来依赖它们。

### 7.4 与 Scheduler 的解耦

`TimerExecutor` 是唯一扩展点，`TimerService` 不继承 `Scheduler`，`Scheduler` 通过执行器适配（`ScheduleOn/ScheduleEveryOn` 自动标记 `kScheduler` 来源）。`timer.cpp` 中不存在对 Scheduler 类型的引用（grep 只命中注释）。

## 八、测试复查

### 8.1 现有测试实际运行结果

| 测试 | 编译 | 运行 | 断言数 |
|---|---|---|---|
| `tests/concurrency/test_timer.cpp`（`run_timer_tests`） | 零警告 | **failures=0** | 33 CHECK + 3 REQUIRE + 19 EVENTUALLY |
| `tests/concurrency/test_timer_stress.cpp` | 零警告 | **failures=0** | 17 个 `Check` |

两者在 `.build-bento-wsl` 的库上独立编译运行通过（构建目录 `GO2CPP_BUILD_TESTS=OFF`，未走 ctest，故手工编译；ctest 注册见 `cmake/Go2CppTests.cmake:43` 与 `:330-336`）。

### 8.2 【P1】压测测试的"等待"是空操作，多条断言恒真

`test_timer_stress.cpp:47-57` 定义了 `WaitUntil(pred, limit)`，语义是"循环到 pred 为真"。但代码中 **7 处**用恒真谓词当 sleep 用：

| 行号 | 调用 | 实际耗时 | 受影响的断言 |
|---|---|---|---|
| 106 | `WaitUntil([] { return true; }, 1ms)` | 0 ms | 第 120 行 `generation_a` 稳定性 |
| 117 | 同上 | 0 ms | 第 122 行 `generation_b == 1`（部分） |
| 195 | `..., 50ms` | 0 ms | 第 199 行 `fired == settled` |
| 198 | `..., 100ms` | 0 ms | 同上 |
| 218 | `..., 30ms` | 0 ms | 第 219 行 `ticks == after_cancel` |
| 277 | `..., 300ms` | 0 ms | 第 280 行 `one_shot == settled` |
| 279 | `..., 200ms` | 0 ms | 同上 |

实测（`probe_tests`，与测试同实现）：

```
probe_waituntil_100ms_actual_ms = 0.000
probe_waituntil_1ms_actual_ms   = 0.000
probe_residue_wait_gap_us       = 0
```

**后果**：这些断言两侧的采样间隔不足 1 微秒，"取消后 50/100/300 ms 内不得再触发"的语义**完全没有被验证**。取消路径如果出现延迟触发（例如 1 ms 后才 fire），这组测试不会失败。

**修复建议**（本次不改代码）：把恒真 `WaitUntil` 换成真实 `std::this_thread::sleep_for`，或改成"等待计数稳定"的谓词（连续两次采样相同才返回）。

### 8.3 【P1】ScenarioRandomCancel 实际 100% 取消，从未验证"保留的会触发"

`ScenarioRandomCancel`（`test_timer_stress.cpp:177-201`）每轮创建 `auto timer = ...` 局部句柄，**迭代结束即析构 = 取消**。复现探针结果：

```
probe_random_cancel_rounds   = 500
probe_random_cancel_explicit = 249   ← 显式 Cancel
probe_random_cancel_implicit = 251   ← 析构隐式 Cancel
probe_random_cancel_fired    = 0     ← 实际触发数
```

500 个定时器**全部被取消，0 个触发**。该场景名义上覆盖"随机取消 + 未取消触发"，实际只覆盖了"取消后不触发"的一半，而且（见 8.2）这一半的稳定性断言是恒真的。

**建议**：把句柄存入跨迭代的容器，只对其中约一半 Cancel，最后断言 `fired == 未取消数`。

### 8.4 【P2】文件头注释与实现不符（对象池已回退）

`test_timer_stress.cpp:1-20` 的说明仍描述"对象池最大的风险是复用未清空的旧状态""thread_local 池"等；实现侧 TimerBin 已在 `b8fc1c0` 回退，当前 `Timer::State` 每次 `make_shared` 新建。场景 2 的注释"等这一代的 State 完全释放（引用计数归零）后，它可能被复用"同样失效。

### 8.5 【P2】`test_timer.cpp` 覆盖缺口

已覆盖：一次性/周期/动态决策、FIFO 同 deadline、Reset、busy wait、弱引用、执行器拒绝、异常隔离、时长告警、饱和 deadline、并发注册、Scheduler 集成、停止竞态、析构取消。

未覆盖（本次审计逐项验证，代码支持但无断言）：

| 缺口 | 风险 |
|---|---|
| `ScheduleEveryOn`（执行器周期路径） | 中：唯一未被测试的执行器入口 |
| Timer 移动语义（move 后源失效、移动赋值取消旧值） | 中：`Timer& operator=` 先 Cancel 旧值，无测试 |
| `Reset` 对已取消/已完成 timer 返回 false | 低 |
| `Refresh()` 正常路径 | 低 |
| `Schedule` 空回调、`ScheduleAfter` 负延迟（按 0 处理） | 低 |
| `SetDurationLimits` 非法参数（负值、warn > error）返回 false 且保持旧值 | 低 |
| `SetExceptionHandler` 运行期替换 | 低 |
| `DebugSnapshot` 的 `thread_state` 迁移（kStarting→kWaiting→kExecuting） | 低（debug 测试覆盖了快照字段，未覆盖状态迁移） |
| `PendingCount`/`NextDeadline` 并发快照一致性 | 低 |

### 8.6 【P2】TSan 回归不含 timer

`tests/concurrency/run_tsan.sh` 使用 `tests/concurrency/test_tsan_smoke.cpp`，覆盖锁/通道/CV/ParkingCondition，**不含 timer**（grep 无 timer）。本次审计手工用 TSan 跑了 `race`/`random` 场景（无报告），但该覆盖不在仓库回归入口内。

### 8.7 【P2】`go2cpp_timer_stress` 未启用 `-Werror`

`cmake/Go2CppTests.cmake:333-334` 只给 `-O2 -Wall -Wextra -Wpedantic`，没有 `-Werror`；项目规范要求严格模式零警告。本次实测无警告，但回归中新增警告不会被拦住。

### 8.8 断言有效性与 SKIP 逃逸

- **断言机制有效**：`GO2CPP_CHECK` 累计 `g_failures`，`tests/test_main.cpp` 在 `failures != 0` 时返回 1；`GO2CPP_REQUIRE` 超时/失败直接 `abort()`（watchdog），不会被吞掉；`stress` 测试返回 `g_failures == 0 ? 0 : 1`。
- **过滤器防漏跑**：`test_main.cpp` 在 `GO2CPP_TEST_FILTER` 未匹配任何模块时返回 2，不会把"0 个用例"当成通过。
- **无 SKIP 逃逸**：timer 测试内部没有 SKIP/DISABLED 宏；`tests/run_all_tests.sh` 的 SKIP 只用于"构建目录缺失"，并且最终"全部 SKIP"会 `exit 1`，不构成静默通过。
- **CI 覆盖**：`.github/workflows/ci.yml:45-62` 默认构建 `-DGO2CPP_BUILD_TESTS=ON` 并跑全量 ctest，包含 `go2cpp_tests`（timer 模块）与 `go2cpp_timer_stress`。

## 九、问题清单

| 级别 | 编号 | 问题 | 位置 | 建议 |
|---|---|---|---|---|
| P1 | T1 | `WaitUntil` 被当 sleep 用、谓词恒真，7 处等待实为 0 ms，相关断言恒真 | `test_timer_stress.cpp:106,117,195,198,218,277,279` | 换真实 sleep 或"连续两次采样相同"的稳定谓词 |
| P1 | T2 | 随机取消场景 100% 取消，实测触发数 0，未验证"保留的会触发" | `test_timer_stress.cpp:177-201` | 句柄留存，只取消一半，断言触发数 |
| P2 | T3 | 注释仍描述已回退的对象池（TimerBin） | `test_timer_stress.cpp:1-20,105` | 更新注释 |
| P2 | T4 | `ScheduleEveryOn`、Timer 移动语义、Reset 失败路径等无测试 | `test_timer.cpp` | 补 6~8 个用例（见 8.5） |
| P2 | T5 | TSan 回归入口不含 timer | `tests/concurrency/run_tsan.sh` | 把 timer 的取消/执行路径纳入 TSan 冒烟 |
| P2 | T6 | `DebugSnapshot` 在 10 万队列上持锁 ~8.9 ms | `timer.cpp:834-840` | 文档标注 O(n) 成本，或采样/分段统计 |
| P2 | T7 | `go2cpp_timer_stress` 未开 `-Werror` | `cmake/Go2CppTests.cmake:333` | 与其他目标对齐严格模式 |
| P3 | T8 | `test_timer.cpp` 用固定 `sleep_for(150ms/20ms)` 验证"取消后不触发" | `test_timer.cpp:80,359` | 慢机/高负载下可能误判，可改轮询断言 |
| P3 | T9 | `m_background_wakeups` 每次循环自增（含正常到期），语义偏"循环次数" | `timer.cpp:909-959` | 文档写明口径，避免误读为"被 notify 次数" |

## 十、未覆盖与限制

1. **时钟回退为注入模拟**：用 LD_PRELOAD 改 `CLOCK_REALTIME`，未修改真实系统时钟（避免污染宿主）。这验证了 timer 不使用真实时间；但没有验证"系统时钟跳变对日志/监控模块"的影响（超出 timer 范围）。
2. **未跑全量 ctest**：`.build-bento-wsl` 为 `GO2CPP_BUILD_TESTS=OFF`，timer 测试用独立编译运行验证；ctest 全量（含 timer）在 CI 默认构建中执行，本次未复现整条 CI。
3. **未做 ARM/其他平台**：仅在 x86_64 WSL2 验证。
4. **未做 >10 万规模**：10 万已满足任务要求；更大规模下的 `multimap` 内存与 `DebugSnapshot` 成本未测。
5. **未深挖优化**：按任务要求不提议重构，仅记录尾部延迟与 dump 成本的成因。

## 十一、复现命令

```bash
cd /UserData/CodexWorkSpace/Go2cpp

# 1) 编译审计程序（链接现有 Release 库）
g++ -std=c++20 -O2 -g -Iinclude -Wall -Wextra -Wpedantic -pthread \
  /tmp/timer_audit/audit_timer.cpp -o /tmp/timer_audit/audit_timer \
  -Wl,--no-as-needed -Wl,-rpath,$PWD/.build-bento-wsl -L.build-bento-wsl \
  -lgo2cpp_scheduler -lgo2cpp_fiber -lgo2cpp_log -ldl -lm

# 2) 压测 / 精度 / 大队列 / 竞态 / 长稳 / 随机 / 泄漏
./audit_timer throughput
./audit_timer precision
./audit_timer bulk
./audit_timer race
./audit_timer soak --soak-seconds 60
./audit_timer random
./audit_timer leak

# 3) 时钟回退（LD_PRELOAD 注入）
gcc -shared -fPIC -O2 realtime_shift.c -o librealtime_shift.so -ldl
LD_PRELOAD=./librealtime_shift.so ./clock_probe

# 4) 现有测试
g++ -std=c++20 -O2 -Iinclude -Itests -pthread tests/concurrency/test_timer.cpp \
  main_timer.cpp -o test_timer <同样的 -l 列表> && ./test_timer
g++ -std=c++20 -O2 -Iinclude -pthread tests/concurrency/test_timer_stress.cpp \
  -o timer_stress <同样的 -l 列表> && ./timer_stress

# 5) 内存检查
valgrind --leak-check=full --errors-for-leak-kinds=definite,indirect \
  --error-exitcode=99 ./leak_probe
g++ -std=c++20 -O1 -g -fsanitize=address -fno-omit-frame-pointer -Iinclude \
  -pthread audit_timer.cpp src/concurrency/timer.cpp src/logging/log.cpp \
  -o audit_timer_asan -ldl -lm && ./audit_timer_asan race
g++ -std=c++20 -O1 -g -fsanitize=thread -Iinclude -pthread audit_timer.cpp \
  src/concurrency/timer.cpp src/logging/log.cpp -o audit_timer_tsan -ldl -lm
setarch x86_64 -R ./audit_timer_tsan race
```

> 审计过程中生成的临时文件（`/tmp/timer_audit/`、`.dsh_tmp/audit_timer/`）已清理；仓库实现文件未改动。
