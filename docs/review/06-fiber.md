# 06 fiber 状态机与成员变量跟踪（专项审查待办）

> 基线：`Go2Cpp-work` @ `fc287c6`（2026-10-23）。
> 用户判定（2026-10-23）：**「这个 Fiber 的状态转化工程太过复杂，到时候后面再专门审查，你先记录在文档，对每个 Fiber 成员变量的跟踪的全面审查」**。
> 状态：**暂停，待专项审查**（本轮只登记范围与清单，不做逐项结论）。
> 关键文件：`include/go2cpp/concurrency/fiber.hpp`（493 行）、`src/concurrency/fiber.cpp`（3270 行，`Fiber::Impl` 定义在 `:1115`，成员区 `:1742-1812`）。

## 1. 为什么说「状态转换过于复杂」——复杂度来源（已定位）

1. **状态不在 Fiber 自身，而在共享的 `FiberRecord`**：`m_record` 是 `shared_ptr`，父链共享、可留墓碑（`alive=false`）。因此「Fiber 的状态」有两条生命周期：对象生命周期 vs record 生命周期，二者不同步（`fiber.cpp:952-976`）。
2. **一套状态、8 个写入点，分散在 4 个函数里**（见 §2），其中 `entry()`、`resume_locked()`、`suspend()`、迁移导入各自写，没有单一状态机入口。
3. **两套状态机并存**：Fiber 侧 `FiberState{Ready,Running,Suspended,Completed,Failed}`，调度器侧 `GState{kRunning,kWaiting,kRunnable,...}`；`scheduler.cpp:960-985` 在 `run()` 收尾时按 `FiberState` **反推** `GState`，且 `suspend_reason()==Yield` 会改变映射结果（见 05 号文档关于 reason 的结论）。
4. **状态与 5 个旁路标志交叉**：`m_resume_claim`（恢复权）、`m_parent_bound`（父链已绑）、`m_scheduler_parent_bound`、`m_scheduler_propagate`、`m_record->cancel_requested`。判断「能否 resume」要同时看它们，散落在 `resume_locked:1491-1540`、`suspend:1638-1690`。
5. **迁移导入直接写状态**：`fiber.cpp:2953` 把导入的 Fiber 置 `Suspended`，绕过正常状态机入口。

## 2. 状态转换全景（全部 `m_record->state.store` 写入点）

| # | 行号 | 写入值 | 所在函数 | 触发条件 |
|---|---|---|---|---|
| S1 | 1224 | `Failed` | `Impl::` 某收尾路径 | 失败收尾 |
| S2 | 1263 | `Completed` | `Impl::` 收尾 | 主体正常返回 |
| S3 | 1341 | `terminal_state`（Completed/Failed） | `entry()` | Fiber 主体结束并发布终态 |
| S4 | 1528 | `Failed` | `resume_locked()` | 父链校验失败/状态非法 |
| S5 | 1534 | `Running` | `resume_locked()` | 切入执行前 |
| S6 | 1646 | `Suspended` | `suspend()` | 主动挂起，切回 caller 前 |
| S7 | 1687 | `Running` | `suspend()` 返回侧 | 从 suspend 返回（被再次 resume） |
| S8 | 1737 | `Failed` | `initialize_context()` | 上下文初始化失败 |
| S9 | 2953 | `Suspended` | `ImportMigrationImage()` | 迁移导入，**绕过状态机** |

读取点（`state.load`）共 14 处：`1139, 1258, 1269, 1292, 1511, 1585, 1604, 1612, 1640, 1950, 1978, 2003, 2046, 2442`。

**已可见的风险形状**（待专项审查确认，不在本轮下结论）：
- Ready→Running 只有 S5 一个入口，但 Failed 有 S1/S4/S8 三个入口、Completed 有 S2/S3 两个，**终态写入路径不唯一**，需确认是否存在双写或竞态（例如 S3 与 S1/S2 是否可能同时发生）。
- S9 让「导入即 Suspended」成为第 6 种合法迁移，需确认它与 S6 的语义等价性。

## 3. 成员变量逐个跟踪清单（专项审查的检查表）

审查方法：对每个成员 grep 全部写点/读点 → 判断「哪一状态下可写」→ 确认同步机制（原子/互斥/单线程）→ 确认生命周期边界（何时失效）。

### 3.1 `Fiber` 本类（1 项）

| 成员 | 类型 | 说明 | 审查要点 |
|---|---|---|---|
| `m_impl` | `unique_ptr<Impl>` | 唯一成员 | 析构期间的并发读取（`Current()` 持有裸指针） |

### 3.2 `Fiber::Impl`（22 项核心 + 条件编译 11 项）

| # | 成员 | 类型 | 语义 | 审查要点 |
|---|---|---|---|---|
| 1 | `m_owner` | `Fiber*` | 反向指针，trampoline 用 | 构造后是否恒定；`reset_for_scheduler` 复用对象时会不会指向旧 owner |
| 2 | `m_id` | `uint64` | 全局单调 id | 池化复用时换新 id，`FiberRecord::id` 是 `const`，需确认同步替换 |
| 3 | `m_record` | `shared_ptr<FiberRecord>` | 与外部共享的状态载体 | **核心**：record 生存期可超过 Fiber（墓碑）；谁还会持有 |
| 4 | `m_debug_gid` | `atomic<uint64>` | 调试用 G 编号 | 无锁快照是否可能读到上一个任务的值 |
| 5 | `m_function` | `Function` | 主体回调 | `reset_for_scheduler` 替换时机；析构时释放捕获对象是否在无锁上下文 |
| 6 | `m_stack_size` | `size_t` | 归一化后的初始提交量 | 只读？`reset_for_scheduler` 是否改 |
| 7 | `m_stack_reserve_size` | `size_t` | 预留区大小（0=默认 8MiB） | 同上 |
| 8 | `m_stack_growable` | `bool` | 是否可增长 | 与 `GO2CPP_GROWABLE_STACK` 环境变量的优先级 |
| 9 | `m_stack` | `unique_ptr<FiberStack>` | 活动栈 | 非空表示「仍在活动」——`resume_locked` 清空、`reset` 拒复用；需核对清空点与 entry 返回的顺序 |
| 10 | `m_context` | `Context` | 保存的寄存器 | 无效窗口（entry 返回后到清空之间）谁可能读 |
| 11 | `m_caller` | `Context` | 调用方上下文 | `suspend` 写入、`resume_locked` 读取；跨线程迁移时是否失效 |
| 12 | `m_scheduler_propagate` | `atomic<bool>` | 是否把 reason 传播给父 | 与取消传播的交互 |
| 13 | `m_scheduler_parent_bound` | `bool` | 调度器父链已绑 | 谁写、是否可解绑 |
| 14 | `m_resume_claim` | `atomic<bool>` | 恢复权门 | **核心并发点**：与 `m_run_claim`(scheduler) 的关系、失败时是否必清 |
| 15 | `m_metadata_mutex` | `mutex` | 保护元数据拷贝 | 与 `m_failure_mutex` 的锁序，是否存在嵌套加锁 |
| 16 | `m_last_resume_thread_hash` | `atomic<size_t>` | 上次 resume 线程 | 用途是诊断还是断言；误报风险（线程 id 复用） |
| 17 | `m_parent` | `Fiber*` | 手工父链 | 父对象先析构后的悬垂；墓碑是否覆盖 |
| 18 | `m_parent_bound` | `bool` | 父链已绑 | 未绑定时挂起→Failed 的判定点 |
| 19 | `m_failure_mutex` | `mutex` | 保护 `m_failure` | 与 `m_metadata_mutex` 的锁序 |
| 20 | `m_failure` | `exception_ptr` | 捕获的异常 | 多写入点（entry/initialize_context）是否可能覆盖 |
| 21 | `m_saved_errno` | `int` | 进入时保存的 errno | 切回时恢复；嵌套 Fiber 时是否正确成对 |
| 22 | `migration_*`（6 项） | `atomic<bool>` / 裸指针 / `uint32` | 迁移等待状态 | 只在 `GO2CPP_MIGRATION_API=ON`；裸指针在挂起期间的有效性 |
| — | `m_asan_*`（3 项） | 指针/`size_t` | ASAN 假栈 | 条件编译，仅在 sanitizer 构建 |
| — | `m_tsan_*`（2 项） | `void*` | TSAN fiber 句柄 | 同上；`__tsan_switch_to_fiber` 的成对性 |

### 3.3 `FiberRecord`（15 项，跨父链共享）

| 成员 | 类型 | 同步 | 审查要点 |
|---|---|---|---|
| `id` | `const uint64` | 不可变 | 复用 Fiber 时如何换 id（const 字段） |
| `parent` | `shared_ptr<FiberRecord>` | 无锁 | 父链构建时机；循环引用风险 |
| `alive` | `atomic<bool>` | 原子 | 墓碑发布点；读侧如何处理 false |
| `state` | `atomic<FiberState>` | 原子（release/acquire） | 见 §2，8 个写点 |
| `reason` | `atomic<SuspendReason>` | 原子 | 与 state 的写入顺序（先 state 后 reason？） |
| `cancellation_requested` | `atomic<bool>` | 原子 | 沿父链传播的方向与终止条件 |
| `mutex` | `mutable mutex` | 互斥 | 保护 `active_parent_id/depth/last_thread/execution` |
| `active_parent_id` | `uint64` | 由 mutex | 最近一次 resume 的实际调用者 |
| `depth` | `size_t` | 由 mutex | 嵌套深度；迁移后是否重算 |
| `last_thread` | `thread::id` | 由 mutex | 跨线程恢复的观测 |
| `execution` | `FiberExecutionBinding` | 由 mutex | 绑定元数据，不拥有 Scheduler |
| `function_name` | `string` | 由 mutex | 调试专用，`GO2CPP_DEBUG_DUMP` |
| `stack_low/high/known` | `uintptr_t`/`bool` | 由 mutex | 调试栈范围，best-effort |

## 4. 建议的专项审查顺序（下一轮从这里开始）

1. **`state` 的 8 个写点**：画完整状态图（含 S9 绕过），确认「同一 Fiber 能否出现两个终态写」。
2. **`m_resume_claim` + `m_parent_bound` + `m_record->state` 三者的关系**：这是 `resume_locked:1491-1540` 的判定核心，也最容易出并发 bug。
3. **`m_stack` / `m_context` / `m_caller` 的失效窗口**：entry 返回 → 清空 → reset 复用，三者的顺序约束。
4. **`FiberRecord` 的墓碑语义**：`alive=false` 后哪些观察路径仍会读 `state`/`reason`（`debug_info:1966`、`context_snapshot:1985`）。
5. **条件编译成员**（ASAN/TSAN/迁移）与主路径的交互：`__tsan_switch_to_fiber` 是否在所有切换点配对。

## 5. 待办

| # | 事项 | 状态 |
|---|---|---|
| F1 | 状态转换专项审查（§2 的 9 个写点 + 14 个读点） | 待做 |
| F2 | `Impl` 成员逐项跟踪（§3.2 的 22 项） | 待做 |
| F3 | `FiberRecord` 15 项 + 墓碑语义 | 待做 |
| F4 | 两套状态机（FiberState ↔ GState）的映射一致性 | 待做 |
| F5 | 把结论回填本文件，并更新 `docs/review/README.md` 索引 | 待做 |
