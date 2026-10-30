# scheduler 模块审查报告（压测 / 随机 / 5 场景 / 解耦 / 测试复查）

- 日期：2026-10-30
- 审查对象：
  - `include/go2cpp/concurrency/scheduler/scheduler.hpp`（真身，967 行）
  - `include/go2cpp/concurrency/scheduler.hpp`、`include/go2cpp/scheduler/scheduler.hpp`（转发头）
  - `src/concurrency/scheduler.cpp`（4667 行）
  - 测试：`tests/concurrency/test_scheduler.cpp`、`test_scheduler_stress.cpp`、
    `test_terminal_reclaim.cpp`、`test_dynamic_scheduler.cpp`、`scheduler_smoke.cpp`、
    `test_scheduler_queue_latency.cpp`
- 压测工具：`/tmp/audit_scheduler/{bench,random,scenes,diag,scaling}.cpp`。独立程序，
  **不注册 CTest、不修改仓库**，只用 `-Iinclude` 与 `.build-bento-wsl` 里的 `.so` 链接。
- 构建：`.build-bento-wsl`（Ninja，`CMAKE_BUILD_TYPE=Release`，GCC 13.3.0）。该目录
  `GO2CPP_BUILD_TESTS=OFF`，因此测试用同一批 `.so` 手工编译链接运行（命令见 §7）。
- 环境：WSL2 Ubuntu 24.04，12 vCPU，20 GiB RAM，`vm.max_map_count=1048576`。
- 边界：只读 + 跑测试 + 写报告；未改实现、未 commit。
- 结论：**无 P0**（无崩溃、越界、UB、死锁、内存泄漏）。发现 **P1 两项**：
  ① draining 阶段的关停成本是 **O(n²)**（10 万 parked G 的 `shutdown()` 需 **56.7 s**，
  40 万积压时超过 6 分钟未完成）；② 每个 G 预留 **8 MiB** 虚拟地址，10 万并发 G 需要
  ~820 GB VA 和 ~2 VMA/G，默认 `vm.max_map_count=65530` 时容量上限约 3 万 G。
  另有 6 项 P2（测试质量为主）。正确性、随机、嵌套、取消/超时、shutdown 竞态均通过。

## 1. 压测

### 1.1 10 万 G 创建/调度/退出

单进程、P=4、`min_workers=max_workers=4`、空任务；同机重复 2–3 次，取范围
（宿主有其他负载，绝对值有 ±10% 抖动）。

| 模式 | 提交 | 全部完成 | `shutdown()` | 端到端吞吐 | RSS 峰值 | VmSize | fd |
|---|---|---|---|---|---|---|---|
| `batch`：一次性 spawn 10 万 | 725–875 ms | ≤1.2 ms | 1.4–1.7 ms | **11.4–13.8 万 G/s** | 8.3–8.6 MB | 438 MB | 5 |
| `pipeline`：分批 1000 × 100 | 793–1043 ms | 含在提交内 | 1.2–1.4 ms | 9.6–12.6 万 G/s | 8.3–8.6 MB | 438 MB | 5 |
| `scene2`：spawn 10 万并保留全部句柄 | 1299 ms | 0 ms | 1 ms | 7.7 万 G/s | 131 MB（句柄+Task 对象） | 495 MB | 5 |

- `batch` 的 drain≈0 是正常的：spawn 的 725–875 ms 里 worker 已并行消费完，
  所以端到端时间就是提交时间。**瓶颈是单线程 `spawn()`**（`Task` 构造 + admission
  锁 + registry 条带），而不是 worker 执行。
- `metrics()` 在三种模式下均为 `runs=100000 / completions=100000`，说明没有 G 被
  漏跑或重复跑；`runnable_count()=0`、`worker_count()=0`（shutdown 后）。
- fd 全程为 5，线程峰值 7（4 worker + sysmon + 采样线程 + main），无 fd/线程泄漏。

### 1.2 1000 并发 G（全部 parked，再统一唤醒）

| 指标 | 数值 |
|---|---|
| 1000 个 G 全部进入 park（spawn + park） | **41.1–51.0 ms** |
| 统一 wake 入队 | 0.9–1.2 ms |
| 唤醒后全部完成 | 18.9–21.8 ms |
| `shutdown()` | 3.9–4.0 ms |
| 峰值 RSS | 9.7–9.9 MB |
| 峰值 VmSize | **8.08 GB**（1000 × 8 MiB fiber 预留） |
| fd | 5（全程不变） |
| `metrics` | `runs=2000`（每个 G park 前跑一次 + 唤醒后跑一次）、`completions=1000` |

- 唤醒吞吐约 5.3 万 G/s（1000 / 18.9 ms）；1000 个 G 的 park 状态下 RSS 只有 ~10 MB
  （每个 fiber 初始只提交 1 页），但**虚拟地址是 8.08 GB**。
- `wake accepted=1000/1000`、`runnable=0`，无丢唤醒。

### 1.3 长稳 60 秒（§3 场景 1 的量化）

4 个生产者线程持续 spawn（150–300 µs 间隔，混合空任务/yield/park/异常/短忙等），
1 个 cancel 线程、1 个低频 waker；运行 60 s 后 `shutdown()`。

| t | RSS | spawned | completed | parked | woken | cancelled |
|---|---|---|---|---|---|---|
| 10 s | 108.5 MB | 114,361 | 74,705 | 22,872 | 4,269 | 1,823 |
| 30 s | 305.7 MB | 341,698 | 223,295 | 68,339 | 12,730 | 5,555 |
| 60 s | **607.6 MB** | 688,677 | 449,878 | 137,735 | 25,527 | 11,158 |

- RSS 以约 **10 MB/s** 线性增长；`shutdown()` 耗时 **41,223 ms**；结束后 RSS 208.5 MB，
  `worker=0`、`runnable=0`、句柄池 4096 个任务全部终态。
- **RSS 增长不是泄漏**，而是积压的 parked G 的固定成本。对照实验：
  - 纯短任务 15 s、21.5 万 G：RSS 7.8 → **8.7 MB 稳定**，`shutdown=2 ms`，结束后
    `DebugSnapshotTasks()=0`，证明终态回收正常；
  - N 个 G 全部 parked 时 RSS 与 N 严格线性：N=1 万 62.1 MB、5 万 296.2 MB、
    10 万 589.4 MB，斜率 **≈5.86 KB/G**（Task 对象 + 1 页 fiber 栈），与长稳中
    11 万存活 parked G 的估算一致。
  - 因此长稳的 RSS 曲线是「waker 唤醒速率（约 425/s）低于 park 产生速率」造成的
    **测试负载积压**；换成纯短任务负载时内存完全稳定。
- 但 41 s 的 `shutdown()` 是真实问题，见 §1.4 与 P1-1。

### 1.4 `shutdown()` 随 parked G 数量的扩展性

N 个 G 全部 parked 就位后调用 `shutdown()`（每次独立进程，P=4）：

| N | setup（spawn+park） | `shutdown()` | µs/G | 结束后 `DebugSnapshotTasks()` |
|---|---|---|---|---|
| 2,000 | 91 ms | 58 ms | 29.0 | 0 |
| 10,000 | 486 ms | 584 ms | 58.4 | 0 |
| 50,000 | 2,387 ms | 9,340 ms | 186.8 | 0 |
| 100,000 | 4,880 ms | **56,676 ms** | 566.8 | 0 |

- 单 G 成本从 29 µs 涨到 567 µs，整体约 **O(n^1.6)**，明显超线性。
- 更极端的 40 万 parked 积压（15 s backlog 场景）时，`shutdown()` **超过 6 分钟
  仍未返回**，进程被手动终止（CPU 95.7%，RSS 814 MB）——不是死锁（有 CPU 进度），
  而是关停吞吐塌陷。
- 根因（代码定位，未修改）：worker 循环在 `draining` 状态下，**每完成一个 G** 就持
  admission 锁做两次全表扫描：
  - `src/concurrency/scheduler.cpp:4375-4385`：
    `collect_terminal_locked()`（16 个条带全扫，`1727-1738`）+ `finish_draining_locked()`
    （内部再调 `all_tasks_terminal_locked()` 全扫，`1899-1913`）；
  - 10 万 G 时每完成 1 个 G 扫描 ~10 万条目 ×2，总计约 10¹⁰ 次原子读 + 条带锁，
    与实测 56.7 s 吻合。
- 影响：进程关停时间可能超过部署侧的 termination grace period（常见 30 s），
  导致被 SIGKILL，未完成清理。

## 2. 随机测试（spawn / park / wake / cancel / shutdown）

- 5 个种子 × 6000 步 = **30,000 次随机操作**，**0 失败**。
- 每步操作分布：spawn 38%、cancel 20%、wake 24%、`wait_for` 5%、随机 shutdown 2%
  （仅在 1/3 步之后触发）、shutdown 后安全 API 测试。
- 每次运行实际覆盖：spawn 745–803 次、park 85–113 次、wake 21–34 次、cancel 22–32 次、
  异常任务 80–102 次（`kFailed` + `failure()`）、随机 shutdown 在 2001–2049 步
  （耗时 0–2 ms，返回 true），之后 4000 步继续调用 spawn/cancel/wake 验证安全性。
- 结束校验全部通过：`non_terminal=0`、`running=0`、`runnable=0`、`worker=0`；
  shutdown 后 `spawn()` 返回的句柄状态为 `kCancelled`；RSS 3.97 MB → 5.2–6.0 MB
  （句柄池 + fiber 缓存），无持续增长。
- **valgrind**（seed=7，1200 步，含 shutdown 竞态）：
  `definitely lost: 0 bytes / indirectly lost: 0 bytes / ERROR SUMMARY: 0 errors`。

## 3. 五个真实场景

| # | 场景 | 模拟的真实用法 | 结果 |
|---|---|---|---|
| 1 | 长稳 60 s | 持续短任务 + 周期 park/wake + 随机取消 | **通过**（全部终态、无崩溃），但 RSS 607 MB、`shutdown` 41.2 s（P1-1 证据） |
| 2 | 高 G 数 10 万 | 一次性提交 10 万 G 并保留全部句柄 | **通过**：10 万全部 `kDead`，`done=100000/100000`，非终态 0，worker/runnable 归零，fd 5 |
| 3 | 取消/超时 | 200 个 parked G 半取消半唤醒；`shutdown_for(20ms)` 超时；`wait_for(10ms)` 超时 | **通过**：cancel 100/100 返回 false、wake 100/100 返回 true、被取消的 G 终态 `kCancelled`；有界 shutdown 20.1 ms 返回 false，二次 `shutdown_for(5s)` 在 299 ms 内完成；`wait_for` 10.5 ms 超时，取消后 join 成功 |
| 4 | 嵌套 | 父 spawn 子后 park、子唤醒父；三层链式 park/wake；父 spawn 100 子并 yield 等待 | **通过**：4a/4b/4c 全部完成，depth_done=3，child_done=100，无死锁（超时 10 s 内） |
| 5 | shutdown 竞态 | 10 轮：4 生产者 + cancel + wake 持续运行中 `shutdown_for(5s)`；另测 worker 内调用 `shutdown()` | **通过**：每轮 1.0–1.9 万 G，关停 35–79 ms，全部终态、worker/runnable 归零；worker 内 shutdown 快速返回且不 join 自己，外部再次 shutdown 完成回收 |

场景 5 的 10 轮关停耗时（35–79 ms）与 §1.4 的 10 万 parked 关停（56.7 s）并不矛盾：
前者每轮只有 1–2 万 G 且在关停时多数已完成，后者是 10 万 G 全部 parked 的极端形态。

## 4. 解耦检查

### 4.1 头文件自包含

三条编译检查全部通过（`g++ -std=c++17 -fsyntax-only -Wall -Wextra -Iinclude -Ithird_party`）：

- 把 `include/go2cpp/concurrency/scheduler/scheduler.hpp` 直接当主 TU 编译：通过
  （仅 `#pragma once in main file` 警告，属预期）；
- 只 `#include "go2cpp/concurrency/scheduler/scheduler.hpp"` 的最小 TU：通过；
- 只 `#include "go2cpp/scheduler.hpp"`（旧路径转发头）：通过。

### 4.2 依赖边界

`-MM` 展开的项目头文件（去掉系统头与转发头）只有：

```
include/go2cpp/concurrency/core/parking_condition.hpp
include/go2cpp/concurrency/core/timer.hpp
include/go2cpp/concurrency/thread_policy.hpp
```

`scheduler.cpp` 额外直接包含：`concurrency/fiber.hpp`、`logging/log.hpp`、
`debug/debug_config.hpp`、`debug/debug_event_ring.hpp`。

动态链接（`readelf -d`）：

- `libgo2cpp_scheduler.so` → `libgo2cpp_fiber.so`、`libgo2cpp_log.so`；
- `libgo2cpp_fiber.so` → 无 go2cpp 依赖（无环）；
- 上层 `libgo2cpp_context.so` → scheduler/error，方向正确。

结论：**只依赖 concurrency base**（core 原语、thread_policy、fiber），未出现对
channel/context/sync/future/io/network/bento/app 的依赖；头文件自包含、无循环链接。
唯一跨模块耦合是 `debug_event_ring`（调试事件环）与 `logging/log`，属可观测性依赖，
记为 P2 提示而非缺陷。

## 5. 测试复查

### 5.1 实跑结果

单独编译 5 个调度器测试模块（`test_scheduler`、`test_scheduler_stress`、
`test_terminal_reclaim`、`test_dynamic_scheduler`、`scheduler_smoke`）并运行：

```
[sched-suite] failures=0
WALL=1.08s CPU=120% MAXRSS=22628KB
```

`test_scheduler_queue_latency` 单独运行也 `failures=0`：
空队列启动延迟 p50=54.2 µs；单 P 积压 2000 任务时新任务排队 p50=1284.3 µs（约 24×）。

### 5.2 覆盖的正面结论

- `test_scheduler.cpp`（542 行）：任务生命周期、yield、外部 park 拒绝、observer
  事件、cancelled 队列节点的捕获释放、raw `Fiber::Suspend(Park)` 取消竞态、跨
  Scheduler 所有权拒绝、pre-cancel 拒绝、并发 shutdown（两个 worker 内同时调）、
  managed `Join` 在 P=1 下 park 等待、取消 join、thread policy、`BlockingRegion`
  的 enter/leave 可重入、异常终态与 `rethrow_failure`、有界 shutdown 超时后二次收尾。
- `test_scheduler_stress.cpp`：7 个场景覆盖短任务突发、spawn/shutdown 循环、
  yield 混合、长短任务、shutdown 期间提交、runnable 计数、enqueue/wake/shutdown
  混合（SCHED-01/02 回归），实跑 1 s 内完成。
- `test_terminal_reclaim.cpp`：终态 G 的按节拍回收、关闭回收后保留、默认节拍上限、
  持锁退出 G 的保留、`DebugSnapshotTasks()` 只返回非终态。
- `test_dynamic_scheduler.cpp`：动态 M 增减、P 上限、`BlockingRegion` 触发替代 M、
  sysmon 检测长阻塞、affinity 命中/未命中、wake drain。
- `scheduler_smoke.cpp`：12 个 smoke 用例，覆盖 park 返回 false、wake-before-park
  令牌、wake 后延后到 callable 返回、shutdown 取消 waiting、异常隔离、预启动
  shutdown、队列任务在运行时锁外析构。
- 未发现 SKIP 逃逸：调度器相关测试没有 `SKIP`/`#if 0`/条件跳过；`tests/test_main.cpp`
  对未知过滤名返回 2（T-08 已修），不会把空跑伪装成通过。

### 5.3 测试质量问题（P2 详单）

1. **恒真断言**：`tests/concurrency/test_scheduler_stress.cpp:101`
   `check(late != nullptr, "shutdown 后 spawn 返回非空")` —— `spawn()` 在拒绝时也返回
   非空句柄（内部 `cancel()`），该断言恒真；同理 `:347` 的
   `check(rejected.load() == 0, "spawn 未返回空句柄")` 也恒真。真正有效的是紧随其后的
   `late->state() == GState::kCancelled`。
2. **observer 终态口径错误**：`test_scheduler.cpp:53-54` 把 `kTaskSuspended`
   也累加到 `tasks_terminal`，而 `:183` 又用 `tasks_terminal >= task_count` 断言。
   一旦 park 事件变多，`kTaskSuspended` 会虚增「终态」计数，掩盖真正的
   `kTaskCompleted/kTaskFailed/kTaskCancelled` 丢失。
3. **场景名不副实**：`test_scheduler_stress.cpp` 的 `scenario_park_resume`
   （“park/resume 混合”）实际只调用 `yield_current()`（`:132` 附近），
   **没有一次 park/wake**。真正的 park/wake 压测只在 `scheduler_smoke` 的少量用例里，
   与场景名承诺的覆盖不符。
4. **API 契约误用**：`test_scheduler_stress.cpp:283` 在场景 7 里直接调用
   `scheduler.park_wait(self)`，但头文件注释明确要求「Task 已由 WaitNode 注册」
   （`scheduler.hpp` 中 `park_wait` 说明）。测试没有构造 WaitNode，属于对
   park_wait 前置条件的误用；能跑通是因为 `wake()` 走的是通用状态机，但这会让
   该用例无法发现「park_wait 缺少 WaitNode 注册」类缺陷。
5. **覆盖缺口**（按 API 统计测试文件数）：
   - `ForceWakeIdle`：**0** 个测试；
   - `wake_io_batch`、`set_io_idle_poller`：0 个直接单元测试（仅 `src/network/io.cpp`
     间接调用；io 测试可能覆盖 IOManager 层，但不直接验证批量唤醒的 owner 不变量）；
   - `IsTaskAlive`：仅 `tests/scenarios/channel_deadlock/channel_deadlock_demo.cpp`
     使用（场景 demo，不在 CTest 单元测试里）；`TaskTerminal` 仅 1 个文件。
6. **测试入口粒度**：`go2cpp_tests` 是单体可执行（`cmake/Go2CppTests.cmake:217`），
   没有 `set_tests_properties(... TIMEOUT)`；`scheduler_smoke` 用 `SMOKE_CHECK`
   直接 `std::abort()`，一个 smoke 失败会终止整个 smoke 进程且不留下其他用例结果
   （作为独立 CTest 尚可，但与 `GO2CPP_CHECK` 的计数语义不一致）。
7. **观测型测试无阈值断言**：`test_scheduler_queue_latency.cpp` 只断言“有样本/
   未超时”（5 处 `Check`），不断言延迟上限；作为基准保留合理，但无法防止性能回退。

## 6. 问题清单

### P0

无。未发现崩溃、越界、UB、死锁或内存泄漏：30,000 步随机 0 失败；valgrind 0 错误、
0 泄漏；10 万 G 高负载、嵌套、取消/超时、shutdown 竞态均干净收尾。

### P1-1（已修复 2026-10-30）draining 阶段每完成一个 G 做两次全表扫描，关停 O(n²)

- 证据（修复前）：
  - `shutdown()` 扩展性：2 千 58 ms → 1 万 584 ms → 5 万 9.34 s → **10 万 56.7 s**
    （29 → 567 µs/G，超线性）；
  - 长稳 60 s 结束时累计 13.7 万次 park（约 11 万存活），`shutdown()` **41.2 s**；
  - 40 万 parked 积压时 `shutdown()` **>6 分钟未返回**（CPU 95.7%，非死锁）。
- 定位（修复前）：`src/concurrency/scheduler.cpp:4375-4385`（draining 分支），每次 G
  结束调用 `collect_terminal_locked()`（16 条带全扫）和
  `finish_draining_locked()`（→ `all_tasks_terminal_locked()` 再全扫）；`shutdown_for()`
  的等待谓词同样每次唤醒全扫。G 完成次数 × 注册表大小 = O(n²)。
- 影响：关停时间可超过部署 grace period，导致被强杀、清理不完整。
- 修复（2026-10-30，只改 `scheduler.cpp` / `scheduler.hpp`）：
  - 新增 `Scheduler::Impl::live_tasks` 原子计数（未终态 G 数）：`Task::prepare_enqueue()`
    首次入队成功时 +1，四个终态转移点（未启动取消 ×2、Fiber 创建失败、run 终态）
    在同一 `transition_mutex` 临界区内 -1，计数与状态转移同线性化点；
  - `finish_draining_locked()` 先做 O(1) 计数判定，归零后才做一次全表确认兜底；
    `shutdown_for()` 等待谓词改读计数；
  - worker 完成路径只在计数归零时做一次 `collect_terminal_locked()` + 收尾，
    draining 期间不再逐 G 全表扫描、逐 G `notify_all_workers()`；
  - stale 队列节点的全表回收推迟到计数归零。
- 修复后数据（同机同工具，3 次取最小）：
  | N | 修复前 `shutdown` | 修复后 `shutdown` | 改善 |
  |---|---|---|---|
  | 10,000 | 627 ms | **221 ms** | 2.8× |
  | 50,000 | 9,385 ms | **1,268 ms** | 7.4× |
  | 100,000 | 45,195 ms（审计原始 56,676 ms） | **3,325 ms** | 13.6×（对 56.7 s 为 17.0×） |
  | 400,000 | >6 min 未完成 | **13,161 ms** | >27× |
  单 G 成本 452 → 33 µs，标度恢复线性（10 万→40 万基本不变）。
- 未达 <2 s 的说明：scheduler 自身关停逻辑已降到几十 ms（未启动 10 万 53 ms、
  全部完成 10 万 1 ms、cancel 提交 37 ms），剩余约 3 s 是 10 万个已 parked Fiber
  的恢复与栈释放——strace 显示 `mmap`/`munmap`/`mprotect` 占 52.8%，且单 worker
  关停（1,831 ms）快于 4 worker（3,325 ms），说明并发 munmap 争用是瓶颈。该成本
  属 fiber 模块的栈分配策略，超出本次「只改 scheduler」边界；后续若改为栈 arena /
  批量串行释放，10 万有望进入 1.5–2 s。
- 验证：现有 scheduler 测试套件 failures=0；3 万步随机 shutdown 竞态 0 失败；
  20 轮 shutdown/cancel/wake 三方竞态 0 失败；完整构建 0 警告。

### P1-2（未修）每 G 8 MiB VA 预留带来的容量约束

- 证据：1000 个 parked G 峰值 VmSize **8.08 GB**；10 万 parked G 峰值
  **819.7 GB**（`VmRSS` 591 MB）；10 万并发 G 的 VMA 约 20 万个
  （8 MiB 预留 + 初始 1 页提交，按每 G 2 个 VMA 估算）。
- 影响：默认 Linux `vm.max_map_count=65530` 时，约 3 万并发 G 就会触顶（本机
  `max_map_count=1048576` 才能跑通 10 万）；小内存/容器环境还需评估页表开销。
- 建议：在 `SchedulerConfig`/文档中明确「每 G 8 MiB VA、并发 G 上限 ≈
  max_map_count/2」的部署约束；或为高并发场景提供更小的
  `fiber_stack_reserve_size` 默认值/按需策略。

### P2（记录，本次不改）

1. 恒真断言（`test_scheduler_stress.cpp:101`、`:347`）。
2. observer 把 `kTaskSuspended` 计入 `tasks_terminal`（`test_scheduler.cpp:53-54`、
   `:183`）。
3. stress 场景 3 名为 park/resume、实际只 yield（`test_scheduler_stress.cpp:132` 附近）。
4. stress 场景 7 直接 `park_wait` 而未注册 WaitNode（`test_scheduler_stress.cpp:283`），
   违反 `park_wait` 的前置条件。
5. 覆盖缺口：`ForceWakeIdle` 0 测试；`wake_io_batch`/`set_io_idle_poller` 无直接单测；
   `IsTaskAlive` 仅场景 demo。
6. `go2cpp_tests` 单体无 TIMEOUT；`scheduler_smoke` 用 abort、与 `GO2CPP_CHECK`
   计数语义不一致。
7. `test_scheduler_queue_latency.cpp` 只有“有样本/未超时”断言，没有延迟阈值。
8. 长稳 RSS 随存活 parked G 线性增长（≈5.86 KB/G），属设计成本而非泄漏；建议在
   模块文档里写明「parked G 的常驻成本」，避免被误判为泄漏。

## 7. 复现命令与工具

```bash
# 构建目录（已存在，无需重新配置）
cd /UserData/CodexWorkSpace/Go2cpp
ninja -C .build-bento-wsl go2cpp_scheduler

# 压测（源码在 /tmp/audit_scheduler，临时目录，未写入仓库）
g++ -std=c++17 -O2 -g -Iinclude -Ithird_party -pthread /tmp/audit_scheduler/bench.cpp \
  -L.build-bento-wsl -Wl,-rpath,$PWD/.build-bento-wsl \
  -lgo2cpp_scheduler -lgo2cpp_fiber -lgo2cpp_log -lgo2cpp_error -o /tmp/audit_scheduler/bench
/tmp/audit_scheduler/bench batch 100000 4        # 10 万 G 创建/调度/退出
/tmp/audit_scheduler/bench parked 1000 4        # 1000 并发 parked G
/tmp/audit_scheduler/bench parked 100000 4      # 10 万并发（约 820GB VA）

# 随机（5 个种子，每个 6000 步）
/tmp/audit_scheduler/random 1 6000
/tmp/audit_scheduler/random 2 6000
/tmp/audit_scheduler/random 3 6000
/tmp/audit_scheduler/random 4 6000
/tmp/audit_scheduler/random 5 6000

# valgrind
valgrind --leak-check=full --show-leak-kinds=definite,indirect \
  --errors-for-leak-kinds=definite,indirect --error-exitcode=99 \
  /tmp/audit_scheduler/random 7 1200

# 5 场景
/tmp/audit_scheduler/scenes longrun 60
/tmp/audit_scheduler/scenes high-g 100000
/tmp/audit_scheduler/scenes cancel
/tmp/audit_scheduler/scenes nested
/tmp/audit_scheduler/scenes race 10

# shutdown 扩展性
/tmp/audit_scheduler/scaling 10000
/tmp/audit_scheduler/scaling 50000
/tmp/audit_scheduler/scaling 100000

# 现有测试（构建目录 tests=OFF，需手工编译）
g++ -std=c++17 -O2 -g -Wall -Wextra -Iinclude -Ithird_party -Itests -pthread \
  /tmp/audit_scheduler/sched_suite_main.cpp \
  tests/concurrency/test_scheduler.cpp tests/concurrency/test_scheduler_stress.cpp \
  tests/concurrency/test_terminal_reclaim.cpp tests/concurrency/test_dynamic_scheduler.cpp \
  tests/concurrency/scheduler_smoke.cpp \
  -L.build-bento-wsl -Wl,-rpath,$PWD/.build-bento-wsl \
  -lgo2cpp_context -lgo2cpp_channel -lgo2cpp_sync -lgo2cpp_future \
  -lgo2cpp_scheduler -lgo2cpp_fiber -lgo2cpp_log -lgo2cpp_error \
  -lgo2cpp_io -lgo2cpp_network -lgo2cpp_os -lgo2cpp_config \
  -o /tmp/audit_scheduler/sched_suite && /tmp/audit_scheduler/sched_suite
```

环境限制说明：`.build-bento-wsl` 当前 `GO2CPP_BUILD_TESTS=OFF`，没有 `go2cpp_tests`
可执行文件，因此现有测试未通过 `ctest` 运行，而是用同一批 `.so` 手工编译运行；
真实场景 5 的「worker 内 shutdown」也覆盖到了。所有压测程序的源码与二进制放在
`/tmp/audit_scheduler/`，未写入仓库。
