# 00 并发安全政策（全局约束）

> 决策来源：2026-10-20 用户明确要求——**并发（scheduler/sync）是最底层模块**；
> config、log 等所有模块都需要回调，回调可能 park；今后所有模块**默认使用 sync 模块**
> 作为并发安全的保证。
> 本文是跨模块政策，优先级高于各模块问题文档。

## 1. 原则

- 并发原语是 L0/L1 基础设施；config、log、metrics、os、network 等建立在它之上。
- 任何**可能被 Fiber 调用**的临界区，默认使用 `sync::Mutex`（可 park、FIFO 交接、进入
  `Task::m_held_mutexes` 检测）。
- 裸 `std::mutex` 只允许在「已证明不会 park、且不跨 Fiber」的极短临界区；**新增代码禁止**。
- 回调（listener/observer/sink/formatter/worker）一律**锁外执行**；确需持锁的，必须显式声明
  「不得 park/阻塞」并加 debug 断言。

## 2. 规则

| 编号 | 规则 |
|---|---|
| R1 | 互斥默认 `sync::Mutex`；纯短临界区用 `sync::SpinMutex`（debug 下持锁 park 直接 abort） |
| R2 | 条件等待用 `sync::ConditionVariable` 或 `core::ParkingCondition`；禁止 `std::condition_variable`（native-only 模块例外） |
| R3 | 回调锁外执行；持锁回调必须标注「不得 park/阻塞」并加断言 |
| R4 | 依赖只能向下：不允许 `sync/scheduler` 反向依赖 `config/log` |
| R5 | 例外必须写 ADR：为什么不能 park、为什么不能用 sync、失效后果 |

## 3. 依赖图与硬约束（关键）

当前 CMake 依赖（`cmake/Go2CppModules.cmake`）：

```text
scheduler → fiber, log          (:247)
config    → scheduler, log      (:248)
sync      → context, scheduler  (:258)
```

如果直接让 `log` 依赖 `sync`，会形成环：

```text
log → sync → scheduler → log
```

所以「所有模块默认 sync」**在 log 上不能直接落地**。这不是原则问题，是依赖方向问题；
必须先拆依赖，或给 log 明确的例外。

## 4. 落地方案

### 方案 A：log 拆两层（用户提出，推荐）

把 log 拆成「无 sync 依赖的 core」与「依赖 sync 的 concurrent 层」，环自然消失：

```text
go2cpp_log（core）
  Level / LogRecord / Filter / Formatter / Sink / Logger / LoggerManager / 宏
  依赖：Threads（不依赖 sync/scheduler）
        ↑                         ↑
scheduler ───────────────────────┘（scheduler 只用 core 写日志）
config ────→ log（core）或 log_concurrent

go2cpp_log_concurrent（新）
  AsyncSink / Dispatcher / FiberSink / 可 park 的 Flush
  依赖：go2cpp_log + go2cpp_sync

依赖图（无环）：
scheduler → fiber, log
config    → scheduler, log [, log_concurrent]
sync      → context, scheduler
log_concurrent → log, sync
```

准确边界（避免把「无 mutex」误解成「完全无锁」）：

- core 的**读路径**已经无锁：`Logger::m_level`、`Logger::m_worker_snapshot`、
  `LogItemWorker::m_snapshot` 都是原子量/原子 `shared_ptr`；
- core 的**写路径**（`AddWorker`/`SetLevel`/`Configure`）仍需串行化：可以保留 `std::mutex`
  （短临界区、不 park），或声明「配置修改必须由单写者串行化」，由 concurrent 层负责加锁；
- `FileSink::Write` 的 `std::mutex` 无法完全去掉（多线程写同一 fd 必须串行化）；它不 park，
  但会阻塞 M，所以 core 契约要写「Sink 必须本地/非阻塞」；
- concurrent 层提供：`AsyncSink`（队列 + 单写线程 + `sync::ConditionVariable`）、
  `FiberSink`（用 `sync::Mutex` 包装任意 Sink）、`Dispatcher`（回调/热更新投递）。

落地步骤（不改代码，先定接口）：

1. 拆 CMake：`go2cpp_log` 去掉任何 sync 依赖；新增 `go2cpp_log_concurrent`
   （`src/logging/async.cpp` / `include/go2cpp/logging/async.hpp`）；
2. `LoggerManager::Configure` 的 `FileSink` 构造移到锁外，锁内只替换 worker 快照；
3. config 的 log 监听器改用 `log_concurrent` 的 `Configure`/`Dispatcher`（或 core 的
   `Configure` + 自己的串行化）；
4. 加依赖图检查（`cmake --graphviz` 或脚本）与「core 链接接口不含 sync/scheduler」断言。

### 方案 B：log 明确例外

- 规定「日志路径禁止 park，Sink 必须非阻塞（普通文件/内存/终端）」；
- log 内部继续用 `std::mutex`，但加契约与检测桩；
- 适用于短期：不用动 CMake，但风险仍在（自定义 Sink 可能阻塞/park）。

### 方案 C：反转 `scheduler → log`

- scheduler 通过接口注入日志，不直接依赖 `go2cpp_log`；
- 成本最高，收益是依赖图更干净，可后续再做。

## 5. 迁移清单

当前 `config`/`logging`/`metrics` 中裸 `std::mutex` 约 **102 处**（`grep -rn "std::mutex"`），
需要逐个分类：

| 模块 | 典型位置 | 处理 |
|---|---|---|
| config | `ConfigVar::m_mutex`（`config.hpp:480`）、`Config::m_mutex`（`:622`）、`m_transaction_mutex`（`:625`） | 改 `sync::Mutex`（config 已依赖 scheduler，加 sync 不形成环） |
| log | `Logger::m_mutex`、`LogItemWorker::m_mutex`、`FileSink::m_mutex`、`BufferedSink::m_mutex` 等 | 依赖环未解前：契约「不得 park」+ 检测；解环后改 `core::Mutex` |
| metrics | Counter/Gauge 以 atomic 为主，Histogram 少量锁 | 逐个确认是否跨 Fiber |

检测现状：`mutex_held_park_total()` / `spin_mutex_park_degrade_total()` 只覆盖 `sync` 模块；
裸 `std::mutex` 不在 `m_held_mutexes` 计数里，持锁 park 无告警。迁移前应先加 instrumentation
或静态审计。

## 6. 验收标准

- 新增代码 `grep` 不到裸 `std::mutex`（`core`/`sync` 内部实现除外）；
- 持锁 park 计数器为 0；
- config listener / log sink 的 park 场景压测（回调里 sleep/IO）不出现 worker 卡死；
- 依赖图无环（`cmake --graphviz` 或脚本检查）。

## 7. 与各模块文档的关系

- config：`02-config.md` §8「裸 std::mutex 与 park」是本政策的具体案例；
- log：`03-log.md` 需补「并发安全约束/依赖环例外」；
- 后续新增模块（scheduler/fiber/sync/io/http2…）一律按本政策审查。
