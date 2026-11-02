# sync 基础检查报告（Mutex / CV / WaitGroup / RWMutex / SpinMutex）

- 日期：2026-10-30
- 工作树：`/UserData/CodexWorkSpace/Go2cpp`（WSL2 Ubuntu 24.04）
- 审查对象：
  - `include/go2cpp/sync.hpp`（360 行公共 API）
  - `src/concurrency/sync.cpp`（2274 行）
  - `include/go2cpp/concurrency/spin_mutex.hpp`（300 行）
  - `include/go2cpp/concurrency/rwmutex.hpp`（637 行）
  - 测试：`tests/concurrency/test_sync.cpp`、`test_locks.cpp`、
    `test_spin_mutex.cpp`、`test_primitives_stress.cpp`、
    `test_primitives_cancel.cpp`、`test_sync_cancel_stress.cpp`
- 构建：`.build-bento-wsl`（Ninja，Release，`-O3 -DNDEBUG`；GCC 13.3.0）
- 环境：AMD Ryzen 9 8945HX（12 vCPU），19 GiB；**宿主同时有其它审计 agent
  在跑**，压测绝对值抖动明显，正确性类结果（计数、失败数、退出码）稳定。
- 边界：只读源码 + 运行测试 + 写报告；**未修改实现、未 commit**。
- 审计程序：本次临时创建于 `.dsh_tmp/audit_base_sync/`（独立可执行，不注册
  CTest）；任务结束已按约定清理，P1 最小复现代码见 §8，测试驱动见 §7

## 0. 结论

- **无 P0 级内存安全问题**；发现 **1 个 P1 正确性缺陷，已于 2026-10-30 修复**
  （修复详情与验证见 §6）：
  `sync::Mutex` 的等待者超时/取消清理路径用非原子 `load+store` 改写
  `m_state`，与并发的 `TryLock`/快速路径 `CAS` 竞争时会**抹掉刚获得的
  `kLocked` 位**。修复前实测：
  1. 持有者 `Unlock()` 抛 `logic_error: unlock of unlocked mutex`（未捕获即
     `std::terminate`）；
  2. **互斥性被破坏**：`LockFor` 成功返回时另一线程仍持有同一把锁（3 秒内
     629–1683 次重叠）。
  修复后（状态写入统一为 CAS 循环）10 轮 × 3 秒：0 次异常 / 0 次双持。
- 其余：CV 契约（返回时重新持有 mutex）经受控实验确认正常；随机 5000 +
  6400 次操作、5 个真实场景、指定规模的压测全部通过。
- 解耦：`sync.hpp` 只依赖 L0（`context`/`error`），符合「只依赖 base」；
  `spin_mutex.hpp` 与 `rwmutex.hpp` 头级引入了 scheduler / debug 头。
- 测试复查：`test_sync` 0 失败、`test_locks` 0 失败、`test_spin_mutex`
  3 失败（`GO2CPP_BUILD_HOOK=OFF` 构建前提缺失，非实现问题），
  无 SKIP 宏逃逸；存在 1 处文档漂移与若干覆盖缺口。

## 1. 压测（指定规模：Mutex 1e7、CV 1e6、WaitGroup、RWMutex 混合）

条件：同一 Release 构建，审计程序 3 次完整运行，取**中位数**（括号为 3 次
范围）。每次运行前无预热之外的清理；ns/op 由 `steady_clock` 计算。

| 项目 | 规模 | 中位数 | 3 次范围 |
|---|---|---:|---:|
| `Mutex` 单线程 lock+unlock | **1e7 次** | **7.53 ns/pair** | 7.50 – 12.27 |
| `Mutex` 4 线程竞争 | 4 × 50 万 = 200 万 ops | 60.55 ns/op（1651 万 ops/s） | 13.50 – 62.85 |
| `ConditionVariable` ping-pong | 4 对，100 万 wait + 100 万 notify | **2328 ns/wait** | 2195 – 4984 |
| CV 等待数/通知数（实测） | — | wait 983,551–988,838；notify 1,000,000 | 缺口来自首轮通知先于 park |
| `WaitGroup` Add(1)/Done（无等待者） | **1e6 对** | 121.53 ns/pair | 66.94 – 135.42 |
| `WaitGroup` 8 worker 波次 | 12,500 轮 × 8 Done = 10 万 Done | 12.0 µs/Done | 10.1 – 12.6 µs |
| `RWMutex` 单线程读 / 写 | 各 1e6 | 38.70 / 44.16 ns/op | 14.11–43.22 / 17.30–45.51 |
| `RWMutex` 8 线程混合（90% 读） | 200 万 ops（读 1,800,005 / 写 199,995） | **231.66 ns/op**（432 万 ops/s） | 173.98 – 357.36 |
| `SpinMutex` 单线程 | 1e6 | 21.93 ns/pair | 11.90 – 27.19 |
| `SpinMutex` 4 线程 | 40 万 ops | 42.12 ns/op（2374 万 ops/s） | 36.79 – 93.66 |

补充：宿主空载时另一次完整运行的对照值为 `Mutex` 5.81 ns/pair、4 线程
24.11 ns/op、CV 505 ns/wait、`RWMutex` mixed 299.27 ns/op。可见当前宿主
并行负载会把绝对值放大 2–10 倍；**趋势与正确性不受影响**，但本表不应当
作性能基线使用。

解读：
- 无竞争 `Mutex` 7.5 ns/pair 与 `std::mutex` 同量级；4 线程 1651 万 ops/s。
- CV 每次等待必然发生一次 futex 睡眠/唤醒，2.3 µs/wait 属预期量级。
- `WaitGroup` 波次的 12 µs/Done 主要是 8 个 OS 线程每轮的唤醒成本，不是
  计数路径成本（计数路径 121 ns/pair）。
- RWMutex 8 线程混合 432 万 ops/s；读路径（单线程 38.7 ns）明显慢于
  `Mutex`，因为它要额外获取/释放内部 `m_state`。

## 2. 随机测试（1000+ 操作）

| 用例 | 规模 | 结果 |
|---|---|---|
| 单线程随机状态机 | **5,000 ops**：TryLock/LockFor(1–200 µs)/Unlock/CV WaitFor(1–100 µs)/CV Wait+取消（立即或 1 ms 后）/NotifyOne/NotifyAll/WaitGroup Add/Done/WaitFor/Count | 3 次运行 0 失败；结束 `waiters==0`、锁可获取 |
| 多线程随机 | **8 线程 × 800 = 6,400 ops**，共享 Mutex + CV，全部操作带超时/取消 | 3 次运行 0 失败、0 errors |
| 延迟取消 | 单线程随机中约半数 CV 等待由 1 ms 后的 cancel 线程唤醒 | 全部返回 false 且重新持锁 |

过程中曾用「高频 TryLock helper」版本，在 5000 ops 的第 1000–1500 步出现
`terminate called after throwing std::logic_error: unlock of unlocked mutex`。
该现象已被单独隔离并复现（见 §6 P1）。去掉 helper 后 3 次完整运行均通过，
说明随机状态机本身无死锁/崩溃。

## 3. 五个真实场景

| # | 场景 | 输入 | 期望 | 实际 |
|---|---|---|---|---|
| 1 | 高竞争 | 16 线程 × 20 万次 Lock/counter++/Unlock（320 万 ops） | 计数精确、无丢失 | 3 次均 `counter=3,200,000`；94–117 ns/op；总 302–373 ms；单次最大等锁 6.0–19.9 ms（宿主调度噪声） |
| 2 | 饥饿模式 | 主线程持锁 3 ms，8 个等待者按入队顺序排队，另有 1 个 TryLock 插队者 | 等待者可完成；可观测 `kStarving` | `starving` 位在三次运行的 8 轮中分别观测到 5/5/8 轮；8 个等待者全部完成（无死锁）；释放到全部完成 4.08–16.03 ms；严格 FIFO 仅 1–4/8 轮（正常模式允许插队，符合 Go 语义，非缺陷） |
| 3 | 跨线程解锁 | A 线程 Lock，B 线程 Unlock，C 线程等待；200 轮 | C 成功获得；锁可再次获取 | 200/200；最坏交接 0.297–14.4 ms（三次运行）；每轮结束 `waiters==0` |
| 4 | 取消 / 超时 | 200 轮 × {LockFor(1 ms) 超时、Context 取消 Lock、CV WaitFor(1 ms)、WaitGroup WaitFor(1 ms)} | 全部返回 false，且锁状态可恢复 | Lock 超时 200/200、取消 200/200、CV 超时 200/200；每次后 `TryLock` 成功、`waiters==0` |
| 5 | Fiber + 线程混合 | Scheduler(4P) + 8 个生产者 Fiber × 2,500 项 + 4 个消费者 OS 线程；有界队列（容量 64，Mutex+CV），`WaitGroup` 收敛后关闭 | 20,000 项恰好消费一次、sum 正确 | `consumed=20,000`，`sum=199,990,000`（精确），重复 0，`shutdown_for(5s)` 成功 |

## 4. 解耦检查

### 4.1 头文件自包含（单独 TU，`-Wall -Wextra -Werror -fsyntax-only`）

| 头文件 | 结果 |
|---|---|
| `go2cpp/sync.hpp` | **OK，0 警告** |
| `go2cpp/concurrency/sync.hpp`（转发头） | **OK，0 警告** |
| `go2cpp/concurrency/spin_mutex.hpp` | **OK，0 警告** |
| `go2cpp/concurrency/rwmutex.hpp` | **FAIL（`-Werror`）**：`rwmutex.hpp:278 unused variable 'woke'`（`GO2CPP_DEBUG_RESOURCES=0` 时该变量只用于 debug 事件宏） |

### 4.2 include 闭包（`g++ -M`）

| 头文件 | `go2cpp/` 依赖数 | 关键内容 |
|---|---:|---|
| `sync.hpp` | **5** | `concurrency/context.hpp`、`context.hpp`、`error.hpp`、`tools/error.hpp`、自身 —— **无 scheduler/fiber/io/debug** |
| `concurrency/sync.hpp` | 6 | 同上 + 转发头自身 |
| `concurrency/spin_mutex.hpp` | 13 | **含 `concurrency/scheduler/scheduler.hpp`** → `core/parking_condition.hpp`、`core/timer.hpp`、`thread_policy.hpp` |
| `concurrency/rwmutex.hpp` | 9 | **含 `debug/debug_resources.hpp`** → `debug/debug_event_ring.hpp`、`debug/debug_config.hpp` |

判定：
- `sync.hpp` **符合「只依赖 base（L0：error/context）」**；`sync.cpp` 的
  scheduler/fiber/debug 依赖只出现在实现文件，未泄漏到公共头。
- `spin_mutex.hpp` 为了内联使用 `Scheduler::current_task_ptr()`、
  `Task::shared_from_this()/enter_no_park_section()`，必须包含调度器头；
  这是设计耦合（头内联实现），不是意外。
- `rwmutex.hpp` 在 `GO2CPP_DEBUG_DUMP=OFF`（默认 Release）下仍包含
  `debug/debug_resources.hpp`；运行时代价为 0（宏展开为空），但头依赖
  不满足「只依赖 base」。
- 构建级：CMake `target_link_libraries(go2cpp_sync PUBLIC go2cpp_context
  go2cpp_scheduler)`，而 `go2cpp_context PUBLIC go2cpp_scheduler`、
  `go2cpp_scheduler PUBLIC go2cpp_fiber go2cpp_log`，因此链接 `go2cpp_sync`
  会传递带入 fiber/log —— **构建级不是只依赖 base**，与头级结论不同，
  引用方需知悉。

## 5. 测试复查

### 5.1 现有测试实际运行

用独立 driver（`test_driver.cpp`，不依赖全量 `go2cpp_tests` 目标）链接
`.build-bento-wsl` 运行：

| 模块 | 结果 | 说明 |
|---|---|---|
| `run_sync_tests()`（13 个用例） | **0 failures** | 输出 1 条持锁 park 告警（预期行为） |
| `run_locks_tests()`（RWMutex/Recursive/Timed，含基准） | **0 failures** | 基准：RWMutex 8 线程读重 `go=393 ns` vs `std::shared_mutex=96.5 ns`（4.07×）；写重 21.91×；TimedMutex 无竞争 4.7 ns（优于 std 25.7 ns） |
| `run_spin_mutex_tests()` | **3 failures** | 见 P2-2 |

失败明细（Release，`GO2CPP_BUILD_HOOK=OFF`）：
```
test_spin_mutex.cpp:357: check failed: MutexHeldParkTotal() >= held_before + 1U
test_spin_mutex.cpp:377: check failed: SpinDegradeTotal() >= spin_before + 1U
test_spin_mutex.cpp:391: check failed: SpinDegradeTotal() >= spin_before + 1U
```
原因：这三个用例依赖 `read()` 被 hook 成 park 点才能触发「持锁 park /
降级」计数；本构建 `GO2CPP_BUILD_HOOK=OFF`，`read` 是原生阻塞，计数不增长。
测试没有 `#if GO2CPP_TEST_HOOK` 或跳过逻辑，因此在该配置下必然失败。

额外：`test_sync_cancel_stress`（12 份并发 × 3000 轮 = 36,000 轮）全部
`OK`，未复现其注释声称的「cancel 后 cleanup 重锁丢唤醒」。

### 5.2 断言有效性

- `GO2CPP_CHECK` 走 `go2cpp_tests::check()`，**不依赖 `NDEBUG`**，Release
  下仍计数（`tests/test_support.hpp:33-42`）；`GO2CPP_REQUIRE` 走
  `watchdog_abort`（进程 abort），不会静默通过。断言宏本身有效。
- 断言规模：`test_sync` 132 CHECK + 8 REQUIRE；`test_locks` 197 + 28；
  `test_spin_mutex` 46 + 7；`test_primitives_stress` 43 + 3；
  `test_primitives_cancel` 60 + 4。`test_sync_cancel_stress` 无断言宏，
  以超时/退出码判定（该模式对「挂起」有效，但对数值正确性无校验）。
- 未发现 `if (!cond) return;` 形式的静默提前返回，也未发现 `catch (...)`
  吞异常后继续的用例（三个主测试文件中均无）。

### 5.3 覆盖缺口（按重要性）

1. **`Mutex::LockFor`/`Lock(ctx)` 超时清理与并发 `TryLock` 的竞态**：现有
   测试完全没有；本次 P1 正是在这里命中。
2. **`Mutex` 饥饿模式的直接用例**：`test_sync` 无；`test_locks` 的
   `TestScenarioWriterStarvation` 验证的是 `RWMutex` 写者饥饿。本次场景 2
   补了观测，但只能证明 `kStarving` 会出现，无法断言「一旦进入饥饿模式
   就严格 FIFO」（正常模式阶段仍会乱序）。
3. **native 线程间跨线程解锁**（A lock → B unlock）：`test_sync` 只覆盖
   Fiber/线程混合方向；本次场景 3 补 200 轮。
4. **`WaitGroup` 计数上溢**（`INT64_MAX`）：只测了负下溢（`Done()` 到 -1）。
5. **`ConditionVariable::NotifyAll` 广播**：`test_sync` 只用 `NotifyOne`；
   仅 `test_primitives_stress.cpp:465` 有一处 `NotifyAll`。
6. **CV 重锁失败抛 `std::runtime_error` 的 D3 路径**：需要 Scheduler 关闭
   与另一等待者持锁的精确交错，现有测试只能部分覆盖，未稳定触发。
7. **`SpinMutex` 在无 hook 构建下的降级契约**：无跳过/守卫（见 P2-2）。

### 5.4 SKIP / 逃逸

- 全仓库测试中**没有** `GTEST_SKIP` / `SKIP` / `#if 0` / `DISABLED_` 形式的
  跳过。
- `test_spin_mutex.cpp` 的 `if (kDebugLockContract)` 在 Release 下跳过
  2 个 fork+abort 子进程场景、改跑降级用例 —— 属**构建条件分支**，不是
  无条件逃逸，但意味着 **Release 审计不验证 debug abort 契约**。
- `test_sync_cancel_stress.cpp` 头注释写「状态：暂未注册进 ctest」，而
  `cmake/Go2CppTests.cmake:811-820` 已 `add_executable/add_test` 注册；
  注释同时声称「会命中已知缺陷」——本次 36,000 轮未复现。属文档漂移
  （P3-2）。
- `test_sync.cpp:355-359` 注释与断言不一致：注释写「Either the expected
  relock or a regression that returned unlocked leaves exactly one logical
  lock to release」，但断言 `GO2CPP_CHECK(!native_probe_after_cancel)`
  只允许「仍持锁」一种结果；若回归为未持锁，`TryLock` 成功使断言失败，
  随后 `mutex.Unlock()` 还会抛 `logic_error`，测试以异常终止而非清晰失败
  （P3-1，断言本身有效，注释误导）。

## 6. 缺陷与观察

### P1（高，**已修复 2026-10-30**）：Mutex 状态字被非原子清理覆盖，互斥性可被破坏

**状态**：已修复（`src/concurrency/sync.cpp`，未 commit）。修复前 3 秒可稳定
复现 9–34 次 `unlock of unlocked mutex`、629–1683 次双持；修复后 10 轮 × 3 秒
0 次异常 / 0 次双持，随机 8×800 十轮 0 失败，`sync.cpp` 构建 0 警告。

**现象（修复前）**
1. 线程 A 反复 `TryLock(); Unlock();`，线程 B 反复 `LockFor(1–100 µs)`：
   3 秒内 A 的 `Unlock()` 抛 `go2cpp::sync::Mutex unlock of unlocked mutex`
   9–34 次（A 约完成 0.8–1.4 亿次锁操作）。
2. 让 A 在持锁后 `sleep 200 µs`，B 的 `LockFor` 若在 A 持锁期间返回成功，
   即 **B 在 A 持锁时也获得了同一把锁**：3 秒运行中
   `acquires=8,693–10,861` 次里有 **629–1,683 次**与 A 的持锁窗口重叠。

**隔离结论**
- 纯 `TryLock/Unlock` 8 线程 2 秒（1.08–1.74 亿次）：0 错误。
- 受控的「`WaitFor` 期间才抢锁，等 helper 退出后主线程再 Unlock」：
  5 次 × 约 4.2 亿次 helper 锁操作（合计约 21 亿次）：**0 错误**。
- 因此 **CV 契约正常**，缺陷在 `Mutex` 的超时/取消清理路径与快路径 CAS
  的竞争。

**机制（修复前代码定位）**
等待者超时/取消后，在 `m_mutex` 内做非原子的读-改-写：

- `src/concurrency/sync.cpp:1558-1568`（`LockImpl` 超时/取消清理）：
  ```cpp
  if (m_impl->m_waiters.empty()) {
      std::uint8_t state = m_impl->m_state.load(std::memory_order_acquire);
      state = static_cast<std::uint8_t>(state & ~Impl::kWaiter);
      if ((state & Impl::kLocked) == 0U && (state & Impl::kWoken) == 0U) {
          state = 0U;
      }
      m_impl->m_state.store(state, std::memory_order_release);
  }
  ```
- 同类写法：`Arm()` 失败清理（`sync.cpp:1465-1473`，清 `kWaiter|kStarving`）、
  `Impl::DropWakeToken()`（`sync.cpp:1086-1131`，含 `store(0U)`）、
  `Unlock` 慢路径（`sync.cpp:1704-1751`，5 处 `store`）。
- 关键：`TryLock`/`Lock` 快路径的 CAS **不持** `m_mutex`，因此「在 `m_mutex`
  内」并不构成与快路径的互斥；load 与 store 之间快路径可写入 `kLocked`，
  store 用旧快照覆盖即抹掉它。

竞态窗口：
1. 清理方 `load` 到 `kLocked == 0`（锁空闲）的旧值；
2. 另一线程 `TryLock`/`Lock` 快路径 `CAS` 成功，写入 `kLocked`；
3. 清理方 `store` 自己基于旧值算出的状态（`0` 或缺少 `kLocked`），
   **覆盖**刚写入的 `kLocked`。

后果：
- 锁被误判为空闲，第二个执行流可通过 `TryLock`/`Lock` 进入临界区 →
  互斥保护失效（数据竞争/数据损坏）；
- 原持有者解锁时看到 `kLocked==0`，抛 `logic_error`；若未捕获即
  `std::terminate`。

**修复（2026-10-30，`src/concurrency/sync.cpp`）**
- 新增 `Mutex::Impl::UpdateStateAtomic`（`sync.cpp:1084-1106`）：CAS 循环，
  `updater` 每次以最新状态重算目标值；成为本文件唯一的状态写协议。
- `DropWakeToken`（`sync.cpp:1121-1191`）：4 处 `store` → 1 次 CAS 循环；
  `catch` 的 `fetch_and` 内存序 `release` → `acq_rel`。
- `Arm()` 失败清理（`sync.cpp:1518-1527`）：`load+store` → CAS 循环。
- `LockFor` 超时/取消清理（`sync.cpp:1610-1628`）：`load+store` → CAS 循环；
  「锁空闲」按 CAS 时最新状态判断，`kLocked` 永不丢失。
- `Unlock` 慢路径（`sync.cpp:1764-1847`）：5 处 `store` → 各分支 CAS 循环。
- 全文 `m_state.store` 计数 = **0**；正常/饥饿/唤醒令牌的状态机与分支条件
  不变，CAS 仍在原 `m_mutex` 临界区内。
- 顺带：`waiters_after`/`debug_count` 包进 `#if GO2CPP_DEBUG_RESOURCES`，
  消除 `sync.cpp` 在 Release 下的 2 个 `-Wunused-but-set-variable`。
- **回归测试**：`tests/concurrency/test_sync.cpp` 新增
  `TestMutexTimeoutCleanupRace()`（1 秒，锁定超时清理 + 并发 `TryLock`，
  断言无解锁异常/无双持/结束后可再获取），已加入 `run_sync_tests()`。

**回归验证（修复后）**

| 实验 | 修复前 | 修复后 |
|---|---|---|
| `LockFor` 超时 3 s × 10 轮 | 3 轮中 2 轮命中，共 34 次解锁异常 | **0 次**（helper 约 13.7 亿次锁操作） |
| 持锁 200 µs 双持检测 3 s × 10 轮 | 629 / 970 / 1683 次双持 | **0 次** |
| 随机 8×800 × 10 次 | `unlock_errors=1`（1 次运行） | **0/0/clean** |
| `test_sync`（含新增回归用例）/ `test_locks` / `sync_cancel_stress(5000)` | — | 全过 |
| `sync.cpp` 构建 | 2 warning | **0 warning** |

详细数据与诊断程序见 `/tmp/sync_p1_fix.md`。

### P2-1：`rwmutex.hpp` 在 `-Werror` 下无法编译（Release）

`const bool woke = ...`（`rwmutex.hpp:278`）只在
`GO2CPP_DEBUG_RESOURCES` 非 0 时被事件宏使用；默认 Release 下
`-Wall -Wextra` 报 `unused variable`，加 `-Werror` 直接失败。项目自身
CMake 只开 `-Wall -Wextra -Wpedantic`（未 `-Werror`），所以 CI 不红，
但违反「零警告」目标，且任何 `-Werror` 使用方包含该头即无法编译。

### P2-2：无 hook 构建下 `test_spin_mutex` 3 个断言失败（缺前置条件）

`TestPlainMutexHeldPark`、`TestSpinMutexReleaseDegrade` 依赖 `read()` 被
hook 成 park 点；`GO2CPP_BUILD_HOOK=OFF` 时计数不增长。测试未用
`GO2CPP_TEST_HOOK` 守卫，也未跳过，因此在该配置下必然失败。建议加
`#if GO2CPP_TEST_HOOK`（或运行时能力探测）并在缺失时明确 SKIP 理由。

### P2-3：`spin_mutex.hpp` 头级依赖 scheduler

为内联 `Scheduler::current_task_ptr()` 与 `Task` 计数，包含
`concurrency/scheduler/scheduler.hpp`，闭包带入 `core/timer`、
`core/parking_condition`、`thread_policy`（13 个 `go2cpp/` 头）。与
「只依赖 base」不符，但属设计必需；若要解耦需把 `MarkOwner` 移出头文件
或改为不透明接口（本轮不重构）。

### P2-4：`rwmutex.hpp` 在 debug OFF 下仍包含 debug 头

`Go2CPP_DEBUG_RESOURCES=0` 时仍 `#include "go2cpp/debug/debug_resources.hpp"`，
闭包带入 `debug_event_ring.hpp` / `debug_config.hpp`。运行时代价为 0，但
头依赖不满足「只依赖 base」，也会拖慢编译。

### P3-1：`test_sync.cpp:355-359` 注释与断言矛盾

注释允许「回归为未持锁」，断言只允许「仍持锁」；失败路径还会因
`Unlock()` 抛异常而 terminate，定位信息不如 `GO2CPP_CHECK` 清晰。

### P3-2：`test_sync_cancel_stress.cpp` 头注释与 CMake 注册不一致

注释称「暂未注册进 ctest」「会命中已知缺陷」；实际
`cmake/Go2CppTests.cmake:811-820` 已注册，且本次 12 份并发 × 3000 轮
未复现 STUCK。注释需更新或缺陷需重新确认。

### P3-3：`sync.hpp` 顶部 `DebugMutex` / `DebugConditionVariable` 前置声明

`sync.hpp:12-15` 声明了 `go2cpp::debug::DebugMutex` /
`DebugConditionVariable`，但该头内未使用（真身定义在
`debug/debug_sync.hpp`）。属无害残留，可清理。

### P3-4：CV `Wait` 无超时/取消时的契约需更醒目

`ConditionVariable::Wait(mutex)` 在没有 Notify 且无 Context 时会永久
挂起（符合契约）。本次随机测试第一次实现就因「Wait 后无通知者」自锁，
说明该边界容易被误用；文档可在「使用边界」中显式写「无取消源时必须
配 Notify 或改用 WaitFor」。

## 7. 复现命令

> 说明：`.dsh_tmp/audit_base_sync/` 为审计临时目录、`.dsh_tmp/sync_p1_fix/`
> 为 P1 修复任务临时目录，均已按任务约定清理；P1 诊断程序可按 §7 命令重建。
> 下列命令中的 `audit_sync.cpp` / `*_diag.cpp` / `test_driver.cpp` 需按
> §8 与报告描述重建；仓库内现有测试源码（`tests/concurrency/*.cpp`）
> 不受影响，可直接用 `tests/test_main.cpp` 的等价模块入口替代驱动。

```bash
cd /UserData/CodexWorkSpace/Go2cpp
# 主审计程序（压测 + 随机 + 5 场景）
g++ -std=c++20 -O2 -DNDEBUG -Iinclude .dsh_tmp/audit_base_sync/audit_sync.cpp \
    -o .dsh_tmp/audit_base_sync/audit_sync \
    -L.build-bento-wsl -Wl,-rpath,$PWD/.build-bento-wsl \
    -lgo2cpp_sync -lgo2cpp_scheduler -lgo2cpp_fiber -lgo2cpp_context \
    -lgo2cpp_log -lgo2cpp_error -lpthread
.dsh_tmp/audit_base_sync/audit_sync          # 完整（约 8–15 s）
.dsh_tmp/audit_base_sync/audit_sync --quick  # 缩小随机/场景规模

# P1 回归（3 秒；修复前 9–34 次异常 / 629–1683 次双持，修复后 0/0）
g++ -std=c++17 -O2 -DNDEBUG -Iinclude .dsh_tmp/sync_p1_fix/mutex_p1_diag.cpp \
    -o .dsh_tmp/sync_p1_fix/mutex_p1_diag \
    -L.build-bento-wsl \
    -Wl,-rpath,/UserData/CodexWorkSpace/Go2cpp/.build-bento-wsl \
    -lgo2cpp_sync -lgo2cpp_scheduler -lgo2cpp_fiber -lgo2cpp_context \
    -lgo2cpp_log -lgo2cpp_error -lpthread
.dsh_tmp/sync_p1_fix/mutex_p1_diag timeout 3 10   # 期望 0 次解锁异常
.dsh_tmp/sync_p1_fix/mutex_p1_diag overlap 3 10   # 期望 0 次双持

# 现有测试
g++ -std=c++20 -O2 -DNDEBUG -Iinclude -Itests \
    .dsh_tmp/audit_base_sync/test_driver.cpp \
    tests/concurrency/test_sync.cpp tests/concurrency/test_locks.cpp \
    tests/concurrency/test_spin_mutex.cpp \
    -o .dsh_tmp/audit_base_sync/go2cpp_sync_tests \
    -L.build-bento-wsl -Wl,-rpath,$PWD/.build-bento-wsl \
    -lgo2cpp_channel -lgo2cpp_fiber -lgo2cpp_scheduler -lgo2cpp_sync \
    -lgo2cpp_context -lgo2cpp_io -lgo2cpp_network -lgo2cpp_log \
    -lgo2cpp_error -lgo2cpp_metrics -lgo2cpp_debug_event -lgo2cpp_debug \
    -lgo2cpp_config -lpthread
.dsh_tmp/audit_base_sync/go2cpp_sync_tests sync
.dsh_tmp/audit_base_sync/go2cpp_sync_tests locks
.dsh_tmp/audit_base_sync/go2cpp_sync_tests spin_mutex

# 解耦（自包含 + include 闭包）
bash .dsh_tmp/audit_base_sync/decouple_check.sh
```

## 8. 最小复现代码（P1，可直接编译；修复后输出 0/0）

> 状态：已修复（2026-10-30）。修复前实测 3 秒：`unlock_errors` = 9–34、
> `overlaps` = 629–1683；修复后 10 轮 × 3 秒均为 `unlock_errors=0 overlaps=0`。
> 完整诊断程序（timeout / overlap / random 三模式）见 `/tmp/sync_p1_fix.md` §4。

```cpp
// 编译：g++ -std=c++17 -O2 -DNDEBUG -Iinclude this.cpp -o diag \
//   -L.build-bento-wsl \
//   -Wl,-rpath,/UserData/CodexWorkSpace/Go2cpp/.build-bento-wsl \
//   -lgo2cpp_sync -lgo2cpp_scheduler -lgo2cpp_fiber -lgo2cpp_context \
//   -lgo2cpp_log -lgo2cpp_error -lpthread
#include "go2cpp/sync.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <random>
#include <thread>

int main() {
    go2cpp::sync::Mutex mutex;
    std::atomic<bool> stop{false}, holding{false};
    std::atomic<long long> unlock_errors{0}, overlaps{0};
    std::thread helper([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            if (!mutex.TryLock()) { std::this_thread::yield(); continue; }
            holding.store(true, std::memory_order_seq_cst);
            std::this_thread::sleep_for(std::chrono::microseconds(200));
            holding.store(false, std::memory_order_seq_cst);
            try { mutex.Unlock(); }
            catch (const std::logic_error&) {
                unlock_errors.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });
    std::mt19937 rng(777);
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    long long acquires = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        if (!mutex.LockFor(std::chrono::microseconds(1 + rng() % 100))) {
            continue;
        }
        ++acquires;
        if (holding.load(std::memory_order_seq_cst)) {
            overlaps.fetch_add(1, std::memory_order_relaxed);  // 双持
        }
        try { mutex.Unlock(); } catch (...) { return 2; }
    }
    stop.store(true, std::memory_order_relaxed);
    helper.join();
    std::printf("acquires=%lld unlock_errors=%lld overlaps=%lld\n",
                acquires, unlock_errors.load(), overlaps.load());
    return (unlock_errors.load() == 0 && overlaps.load() == 0) ? 0 : 1;
}
```

## 9. 未覆盖 / 限制（如实声明）
- **未跑 TSan/ASan/UBSan**：`.build-bento-wsl` 为普通 Release；带 sanitizer
  需要另外的构建目录与全量依赖，本轮未做。P1 修复前后的判定依据都是行为
  复现（异常 + 互斥重叠），不是竞态检测器报告。
- **压测绝对值受宿主并行负载影响**：同一程序三次运行可差 2–10 倍；报告已
  给范围而非单点。未做 CPU 亲和绑定或空载复测。
- **饥饿模式的严格 FIFO 未能单独验证**：只能观测 `kStarving` 出现与全部
  等待者完成；正常模式阶段的乱序（1–4/8 轮严格 FIFO）符合 Go 语义，
  但没有构造出「进入饥饿模式后仍乱序」的反例。
- **CV 重锁失败的 D3 路径**（`ThrowConditionRelockFailure`）未稳定触发。
- **Scheduler shutdown 期间**的 `Wait`/`Lock` 行为只在现有测试中间接覆盖，
  未做专项压测。
- **Fiber 场景下**的 Mutex 饥饿/P1 修复未单独压测（复现与回归均只在
  native 线程；受管 G 的 park 路径可能放大或掩盖问题）。
- **`RecursiveMutex` / `TimedMutex`** 不在本任务范围，仅通过
  `test_locks` 顺带运行（0 失败）。
- 现有 `test_sync_cancel_stress` 的「已知缺陷」注释无法证实是否已修复：
  36,000 轮未复现不足以证明；需按注释中的多进程并发条件做更大规模抽样。
