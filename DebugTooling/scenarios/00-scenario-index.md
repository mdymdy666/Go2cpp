# scenarios · 00 场景索引（第 3 轮，骨架）

> 本文件是**场景骨架**：先固定分类与编号，逐轮把每个场景展开为
> 「前置条件 / 触发方式 / 期望信息 / 现有能力 / 缺口 / 证据」。
> 目标：**≥30 个真实场景**；当前骨架已列 **34 个**。

## 分类总览

| 类 | 名称 | 数量 | 核心问题 |
|---|---|---:|---|
| **A** | 只读观测（看得见） | 8 | 如何在不干扰运行的前提下取得一致快照 |
| **B** | 运行时变更（改得动） | 8 | 如何安全地改变运行中的 Fiber/调度器 |
| **C** | 可视化与输出 | 7 | 如何把快照变成可用的人类/机器可读形式 |
| **D** | 锁与同步 | 5 | 如何观测并影响锁的等待与归属 |
| **E** | 远程、多机与工具链 | 6 | 如何跨进程/跨机器/与 gdb 协同 |
| | **合计** | **34** | |

## A. 只读观测（8 个）

| 编号 | 场景 | 现有能力（证据） |
|---|---|---|
| A1 | 查询单个 Fiber 快照：编号、状态、最近挂起原因、嵌套深度、最后所在线程 | ✅ `Fiber::id()`/`state()`/`suspend_reason()`/`nesting_depth()`/`debug_info()`（`fiber.hpp:186-221`） |
| A2 | 导出 Fiber 的完整调用链（含逻辑父级与已结束父级的墓碑帧） | ✅ `context_snapshot()`（`fiber.hpp:206-211`） |
| A3 | 查询 Fiber 当前/最近的 G→M→P 绑定 | ✅ `execution_binding()`（`fiber.hpp:215`）、`Scheduler::current_*()`（`scheduler.hpp:607-610`） |
| A4 | 调度器全局概览：Fiber 总数、worker 数、P 数 | ✅ `runnable_count()`/`worker_count()`/`processor_count()`（`scheduler.hpp:582-585`） |
| A5 | 每个 P 的本地队列长度与状态 | ✅ `processors()` → `ProcessorSnapshot{id,state,queued}`（`scheduler.hpp:210-214,592`） |
| A6 | 每个 M 的状态与阻塞诊断（已脱离 P、正在阻塞的任务、长系统调用计数） | ✅ `machines()` → `MachineSnapshot`（`scheduler.hpp:217-233,593`） |
| A7 | 订阅调度事件流（10 类）并还原时间线 | ✅ `SchedulerObserver::OnEvent`（`scheduler.hpp:140-183`） |
| A8 | 采集/差分性能指标（resume 次数、偷取次数、本地队列命中） | ✅ `metrics()`（`scheduler.hpp:236-243,599`） |

**A 类的共同缺口**：数据齐备，但**没有统一的序列化出口**，也没有"取一份一致快照"的组合接口
（现有接口是逐个调用的，多次调用之间状态会变）。

## B. 运行时变更（8 个）

| 编号 | 场景 | 现状 |
|---|---|---|
| B1 | 查询某个 Fiber 的任务元数据：名字、描述、函数名、文件与行号 | ❌ **完全没有**：`Task` 只有私有 `std::function<void()> m_function`（`scheduler.hpp:362`），`FiberContextFrame` 无任何名字/行号字段 |
| B2 | 运行中替换某个 Fiber 要执行的函数体 | ❌ `m_function` 私有且无 setter（`scheduler.hpp:356,362`） |
| B3 | 把某个 Fiber 跨 M 迁移 | ❌ `FiberExecutionBinding` 是"不拥有的标识"（`fiber.hpp:32-34`），非控制接口 |
| B4 | 把某个 Fiber 插队到指定 Fiber 之前执行 | ❌ 有 `enqueue`（`scheduler.hpp:492`）与后端 `try_mark_queued*`（`301-308`），但无按位置插入 |
| B5 | 外部暂停/恢复指定 Fiber（断点式） | ❌ `Fiber::Suspend()` 是**协作式**（Fiber 自己调用，`fiber.hpp:156`）；注释明确"不会从另一个线程强行切断任意 C++ 调用栈"（`scheduler.hpp:399-400`、`411`） |
| B6 | 取消指定 Fiber 并观测收敛过程 | ✅ 部分：`Scheduler::cancel(task)`（`scheduler.hpp:545`）+ 协作式取消标记 |
| B7 | 运行期修改调度参数（`idle_wait`/`local_queue_limit`/`collect_metrics`…） | ❌ `SchedulerConfig` 在构造时传入（`scheduler.hpp:161-197`），未见运行期 setter |
| B8 | 运行期替换同步等待后端（影响锁的等待行为） | ✅ **已有官方接入点**：`SetWaitBackendFactory()`（`sync.hpp:46-66`、`sync.cpp:612`），但**只影响之后创建的等待节点** |

**B 类的核心结论**：唯一已开放的运行期替换点是 **`WaitBackend` 工厂**；其余全部需要新建，
且必须解决"不强行切断 C++ 调用栈"这条既有约束。

## C. 可视化与输出（7 个）

| 编号 | 场景 | 现状 |
|---|---|---|
| C1 | 文本 dump（人类可读，可直接看） | ❌ 无 CLI |
| C2 | ini 格式 dump | ❌ 无序列化器 |
| C3 | json 格式 dump | ❌ 无序列化器（项目有 `json.hpp` 可用） |
| C4 | 重定向到文件（含原子写入/轮转考虑） | ❌ 无统一出口 |
| C5 | 按条件过滤（某 P / 某状态 / 深度阈值 / 只显示阻塞者） | ❌ 无查询层 |
| C6 | 时间线可视化（事件流 → 甘特图 / SVG） | ❌ 无 |
| C7 | 实时监视模式（周期刷新，人可观察） | ❌ 无 |

## D. 锁与同步（5 个）

| 编号 | 场景 | 现状 |
|---|---|---|
| D1 | 查询某把锁的持有者与等待队列 | ❌ 无对外查询接口（`Mutex::Impl` 全私有，`sync.cpp:665-686`） |
| D2 | 区分锁的等待者类型（Fiber / OS 线程）与各自数量 | ⚠️ 内部有 `m_native_waiters` 计数（`sync.cpp:676-678`），但**不对外暴露** |
| D3 | **锁争用决策**：在争用时决定哪个 Fiber 获得锁 | ❌ **不可替换**：现有 FIFO 出队策略硬编码在 `Unlock` 路径；`WaitBackend` 只替换"怎么 park/怎么 wake"，不替换"选谁" |
| D4 | 死锁/长等待检测（等待链、环） | ❌ 无 |
| D5 | 条件变量等待者快照与通知统计 | ❌ 无（`ConditionVariable` 有配套 FIFO 队列，`sync.hpp:129+`） |

**D 类的现实边界（重要）**：`Mutex` 已有明确的竞争策略且写得很克制 ——
`LockImpl` 里 managed G 在没有 native waiter 时做**有界协作重试（256 次）**，
一旦出现普通线程等待者就把重试**限制在 4 次**并回到 FIFO handoff；单 P 不走该路径
（`sync.cpp:711-760` 注释原文：「managed waiter 之间允许有限度地越过队列，以换取吞吐」）。
**任何"决定谁获得锁"的新机制都必须与这套既有策略共存**，不能简单替换成别的公平性模型。

## E. 远程、多机与工具链（6 个）

| 编号 | 场景 | 现状 |
|---|---|---|
| E1 | 基础通道：stdio / 文件 / 环形缓冲 | ❌ 无（项目规范要求"调试输出与日志要有统一入口，不散落裸 printf"） |
| E2 | 插件式 topic 传输（Stratum 式，把调试事件发布到 topic） | ❌ 无 |
| E3 | 多机同步调试（跨机器同一时刻快照/联合时间线） | ❌ 无，且**时钟问题是前提**：`SchedulerEvent::timestamp` 是 `steady_clock`（`scheduler.hpp:135-137`），跨机不可比 |
| E4 | 与 gdb 混合：OS 线程 ↔ Fiber 的映射导出 | ❌ 无（但 `FiberContextFrame::last_thread` 是 thread::id，`fiber.hpp:65`，可作为映射起点） |
| E5 | 复现：事件序列录制与回放 | ❌ 无 |
| E6 | 向上提供工具形态（库 / CLI / HTTP 端点） | ❌ 无 |

## dump CLI 的选项场景（目标 ≥15，骨架已列 20）

命令形态（使用者给定）：`Go2Cpp dump scheduler [--options] ... scheduler_name`

| # | 选项 | 含义 |
|---:|---|---|
| 1 | `--format=text\|ini\|json` | 输出格式 |
| 2 | `--output=<file>` | 重定向到文件 |
| 3 | `--append` / `--truncate` | 文件写入语义 |
| 4 | `--summary` | 只输出概要（Fiber 数、M/P 数、指标） |
| 5 | `--fibers` | 列出全部 Fiber |
| 6 | `--fiber=<id>` | 只看一个 Fiber（可重复） |
| 7 | `--stacks` | 附带调用栈 |
| 8 | `--stack-depth=N` | 调用栈深度上限 |
| 9 | `--state=<list>` | 按 G 状态过滤（阻塞/挂起/异常…） |
| 10 | `--min-depth=N` | 按嵌套深度过滤 |
| 11 | `--processors` | 输出 P 列表 |
| 12 | `--machines` | 输出 M 列表 |
| 13 | `--metrics` | 输出指标 |
| 14 | `--events[=N]` | 输出最近 N 条事件 |
| 15 | `--locks` | 输出锁与等待者 |
| 16 | `--waiters` | 只输出等待关系 |
| 17 | `--watch[=interval]` | 周期刷新（实时监视） |
| 18 | `--follow` | 跟踪事件（类似 `tail -f`） |
| 19 | `--include=<fields>` | 字段选择（控制输出宽度） |
| 20 | `--color=auto\|always\|never` | 终端着色 |

## 尚未展开的部分（后续轮次的活）

1. 每个场景补「前置条件 / 触发方式 / 期望信息 / 验证方式」；
2. 明确哪些场景**只读安全**、哪些**必须带前置条件校验**；
3. 给出 `dump` 的**输出 schema**（ini 与 json 的字段定义）；
4. 明确 **gdb 混合**与**多机时钟**两个难点的可行边界。
