# 重要函数的函数级注释进度

> 目标：为每个模块挑 **1~3 个最复杂的函数实现**补函数级注释（`@brief` + `## 步骤` + `## 并发/失败语义` + 体内 `// stepN:`）。
> 其中 **scheduler 与 ParkingCondition 各至少 3 个**。
> 只加注释，不改逻辑；每轮保持 `ctest` 113/113。

## 一、进度表

### scheduler（要求 ≥3 个）

| # | 函数 | 行数 | 状态 | 说明 |
|---|---|---:|---|---|
| 1 | `Scheduler::worker_loop` | ~420 | ✅ **已完成** | 1 函数头 + 5 处 step（快/中/慢三路径 + 执行 + 维护） |
| 2 | `Task::park_for_scheduler` | 26 | ✅ **已完成** | 1 函数头（5 步 + 4 条并发语义）+ 4 处 step |
| 3 | `Scheduler::wake_io_batch` | ~150 | ✅ **已完成** | 原已有函数头（3 步），本轮补齐体内 3 处 step 标注 |
| 4 | `Scheduler::requeue_from_worker` | 待查 | ⬜ 可选 | — |
| 5 | `Scheduler::shutdown_for` | 待查 | ⬜ 可选 | — |

**scheduler 的 3 个必做函数已全部完成。**

### ParkingCondition（要求 ≥3 个）

| # | 函数 | 状态 | 说明 |
|---|---|---|---|
| 1 | `ParkingCondition::notify_one` | ✅ **已完成** | 抢占式唤醒 + 跳过僵尸节点 + 锁外 Wake |
| 2 | `ParkingCondition::notify_all` | ✅ **已完成** | swap 出队 + 只唤醒真正抢占成功的节点 |
| 3 | `ParkingCondition::WaitOnce` | ✅ **已完成** | 7 步（判定→提前返回→入队重检→cleanup→超时→等待→映射）+ 5 条并发语义 |
| 4 | `WaitNode::Wake`（或等价唤醒路径） | ⬜ 可选 | 内部实现，已被 `WaitOnce` 的注释覆盖调用关系 |
| 5 | `ParkingCondition::wait` / `wait_for` / `wait_until` | ⬜ 可选 | 都是 `WaitOnce` 的薄封装 |

**ParkingCondition 的 3 个必做函数已全部完成。**

### 其余模块（各 1~3 个）

| 模块 | 函数 | 状态 |
|---|---|---|
| io | `poll_once` ✅ / `process_events_batch` ✅ / `register_wait` | **2/3**（register_wait 未补） |
| hook | `cooperative_io` ✅（补体内 4 处 step）/ `cooperative_connect` | 1/2 |
| fiber | `Fiber::resume_locked` ✅（6 步 + 4 处 step） | 1/1 |
| sync | `Mutex::LockImpl` ✅（6 步）/ `Unlock` ✅（4 步） | **2/2** |
| channel | `SendUntil` ✅（6 步 + 7 条语义）/ `RecvUntil` | 1/2 |
| context | `DrainCancellation` ✅（5 步 + 6 条语义） | 1/1 |
| socket | `wait_ready` ✅（5 步 + 6 条语义） | 1/1 |
| compress | `Deflater::Run` ✅（5 步 + 5 条语义） | 1/1 |
| http | `Client::do_request` ✅（6 步）/ `Server::serve_connection` ✅（5 步） | **2/2** |
| http3 | `Client::Impl::Callback` ✅（4 步 + 7 条语义） | 1/1 |
| thread_policy | `ScopedThreadParticipation` ✅ / `HookAllowed` ✅ | 1/1 |
| timer | `TimerService::Run` | ✅ **已有**（`@brief` + step1-4） |
| http2 | `Connection::ReadEvent` | ✅ **已有**（`@brief` + step1-4 + 体内标注） |
| websocket | `read_frame` | ✅ **已有**（`@brief` + step1-4 + 体内标注） |
| bufio | `Reader::Read` | ✅ **已有** |
| os | `File::ReadFile` | ✅ **已有** |
| path | `WalkRecursive` | ✅ **已有**（step1-5 + `@note`） |
| config | `LoadFromIni` | ✅ **已有** |
| log | `BufferedSink::Flush` | ✅ **已有**（20 `@brief` / 13 步骤） |
| metrics | `Histogram::Quantile` | ✅ **已有**（16 / 12） |
| json | `ParseJson` | ✅ **已有**（11 / 2） |
| encoding | 编解码循环 | ✅ **已有**（各 3 / 2） |
| byte_array | — | ✅ **无复杂函数**（23 函数均 8 行级） |
| container | — | ✅ **无复杂函数**（39 行） |

## 二、格式规范（`docs/annotation-style.md` §4）

```cpp
/**
 * @brief 一句话说明这个函数做什么。
 * @param ...
 *
 * ## 定位
 * 它在整个调用链里的位置（谁调它、它调谁）。
 *
 * ## 步骤
 *
 * step1. ...
 * step2. ...
 *
 * ## 并发/失败语义
 *
 * - 哪些状态受什么保护；
 * - 失败/异常时保证什么不变量。
 */
```

函数体内在**步骤边界**加 `// stepN: 一句话`（不要每行都标）。

## 三、第 1 轮记录：`Scheduler::worker_loop`

补了 1 个函数头（含 `## 定位`/`## 步骤` 6 步/`## 并发/失败语义` 4 条）+ 体内 5 处 `// stepN:`：

| 标注 | 对应代码 |
|---|---|
| `step2` | 本地队列快路径（无总锁） |
| `step3` | `incoming` 分条队列扫描（按 class 亲和） |
| `step4` | 全局 admission 锁（emergency → 本地 → 全局 → 窃取） |
| `step5` | `task->run()` 执行点 |
| `step6` | `maybe_grow()` 维护路径 |

**验证**：`ctest` **113/113**（67.28s）；`git diff` 只有 2 行原有注释被改写，**无代码行变动**。
