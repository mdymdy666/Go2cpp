# G10 · 调试模块（go2cpp::debug）—— 一图看懂

> 覆盖本次新增的调试模块：`DebugFiber`/`DebugRegistry`、`DebugScheduler`/`DebugTask` 视图、
> `dump CLI`（含 `--follow`）、`DebugMutex`/`DebugConditionVariable`/`DebugWaitGroup`、
> `EventBuffer`/`DebugEventCollector`、`DebugBreakpoints`/`Checkpoint`、
> `DebugSignalDumper`（跨进程 dump）。另含 `sync::WaitSelector`（D3 锁争用选择钩子）。
> 源码：`include/go2cpp/debug/*.hpp`（7 个头文件）、`tests/debug/*.cpp`（7 个测试）、
> `benchmarks/debug_*_benchmark.cpp`（2 个基准）、`example/debug/debug_module_demo.cpp`（示例）。

**设计主线**：**能继承的继承，不能继承的用代理** —— 并且**绝不在调度器热路径上加钩子**。

## ① 类架构：每个类用的底层能力 + 主要职责

> 本模块共 19 个类型，一次性画在一张图上会被 Mermaid 横向铺开、文字小到不可读。
> 因此按职责拆成 **3 张**（每张 7~8 类，一屏可读）。

### ①-A 核心链路（原 Fiber/Task/Scheduler → 调试侧）

```mermaid
classDiagram
    direction TB

    class Fiber {
        <<G1>>
    }
    class Task {
        <<G2>>
    }
    class Scheduler {
        <<G2>>
    }
    class SchedulerObserver {
        <<G2 只读插件边界>>
    }

    class DebugFiber {
        <<继承 Fiber·唯一允许继承的点>>
        -FiberDebugInfo m_info
        +description() / name()
        +set_source_location(file, line)
        ← 只加字段，不改 Fiber 行为
        ← 无虚析构，绝不能放进 Task::m_fiber
    }
    class DebugRegistry {
        <<元数据注册表>>
        +Register(id, info) / Lookup(id)
        +Snapshot() / Size()
        ← 独立 std::mutex
    }
    class DebugScheduler {
        <<代理 Scheduler>>
        +Snapshot() / SnapshotWith(options)
        +DumpText / DumpIni / DumpJson
        ← 只用 public 只读接口
    }
    class TaskSnapshot {
        <<一个 G 的只读快照>>
        id / state / queued / started
        last_processor_id / task_class
        failed / cancellation_requested
    }

    DebugFiber --|> Fiber : 继承（仅此处）
    DebugFiber ..> DebugRegistry : 元数据登记
    DebugScheduler ..> Scheduler : 代理（只读）
    DebugScheduler ..> TaskSnapshot
    DebugScheduler ..> Task : 读状态
```

### ①-B 锁观测代理（原 sync → 调试侧）

```mermaid
classDiagram
    direction TB

    class sync_Mutex {
        <<G4 锁>>
    }
    class sync_CondVar {
        <<G4 条件变量>>
    }
    class sync_WaitGroup {
        <<G4 计数等待>>
    }
    class DebugMutex {
        <<代理 Mutex>>
        +Snapshot() MutexDebugSnapshot
    }
    class DebugConditionVariable {
        <<代理 CondVar>>
    }
    class DebugWaitGroup {
        <<代理 WaitGroup>>
    }

    DebugMutex ..> sync_Mutex : 代理 + public Debug 方法
    DebugConditionVariable ..> sync_CondVar : 代理
    DebugWaitGroup ..> sync_WaitGroup : 代理
```

### ①-B′ D3：锁争用选择钩子

```mermaid
classDiagram
    direction TB

    class sync_Mutex {
        <<G4 锁>>
        +Unlock()
    }
    class WaitSelector {
        <<D3 争用选择钩子>>
        +Select(candidates) optional~size_t~
        ← 只重排队列，不改认领语义
    }
    class WaiterCandidate {
        <<选择器的输入视图>>
        +index
        +is_fiber
        ← 不暴露内部 WaitNode
    }

    WaitSelector ..> WaiterCandidate : 读取候选
    WaitSelector ..> sync_Mutex : Unlock 时被调用（只重排）
```

### ①-C 事件采集 / CLI / 断点 / 信号 dump

```mermaid
classDiagram
    direction TB

    class SchedulerObserver {
        <<G2 只读边界>>
    }
    class DebugScheduler {
        <<代理 Scheduler>>
    }

    class EventBuffer {
        <<无锁有界环形缓冲>>
        +Push / PushBatch / Pop / PeekAt
        +Pushed / Dropped
    }
    class DebugEventCollector {
        <<实现 SchedulerObserver>>
        -线程局部暂存（满 32 条提交）
        +Drain / Peek / Pending
        +DumpText/Ini/Json
    }
    class FollowEvents {
        <<持续跟随>>
        +sink(events) -> bool
        ← 必须带 overall_timeout
    }
    class DumpCommand {
        <<CLI 解析结果>>
        target / format / output
        follow / follow_batches / follow_timeout
    }
    class DebugFileSink {
        <<原子写：临时文件 + rename>>
    }
    class DebugBreakpoints {
        <<协作式断点注册表>>
        +Add(spec) / Remove(id)
        +ShouldStop(name, task_id)
        +Resume / Waiting / TotalHits / ResetStats
    }
    class Checkpoint {
        <<使用者代码里手动插入>>
        +Checkpoint(name, task_id) -> id
        +ScopedCheckpoint（RAII 自动 Resume）
    }
    class DebugSignalDumper {
        <<SIGUSR1 驱动·跨进程>>
        +Install / Uninstall
        +PollOnce()
        +StartWatcher / StopWatcher
        ← handler 只置 atomic 标志
    }

    DebugEventCollector --|> SchedulerObserver : 实现只读边界
    DebugEventCollector *-- EventBuffer
    DebugEventCollector ..> FollowEvents
    DumpCommand ..> DebugScheduler : 分派
    DumpCommand ..> DebugFileSink
    DebugBreakpoints ..> Checkpoint
    DebugSignalDumper ..> DebugScheduler : 生成快照
    DebugSignalDumper ..> DebugEventCollector : 可选导出事件
```

## ② 重要函数：功能与实现要点

```mermaid
flowchart TB
    subgraph DUMP["dump 的三条路径"]
        D1["ParseDumpCommand(args)"] --> D2{"target?"}
        D2 -- scheduler --> D3["DebugScheduler::SnapshotWith(options)"]
        D2 -- events --> D4{"follow?"}
        D4 -- 否 --> D5["DebugEventCollector::Dump*(limit)"]
        D4 -- 是 --> D6["FollowEvents(sink, interval, batches, **timeout**)"]
        D3 --> D7{"include_fibers?"}
        D7 -- 是 --> D8["Scheduler::DebugSnapshotTasks()<br/>**按 16 分片逐个持锁遍历**"]
        D7 -- 否 --> D9["不采集（json 输出 fibers: null）"]
        D8 --> D10["格式化为 text/ini/json"]
        D9 --> D10
        D5 --> D10
        D6 --> D11["逐事件输出一行 JSON + 每批 fflush"]
        D10 --> D12{"output?"}
        D12 -- stdout --> D13["fwrite"]
        D12 -- 文件 --> D14["WriteDumpFile：**tmp+rename 原子写**"]
    end

    subgraph D3["D3 选择器接入点"]
        E1["Mutex::Unlock 持队列锁"] --> E2["ReorderWaitersForSelector()"]
        E2 --> E3{"有注册的选择器?"}
        E3 -- 否 --> E4["跳过（仅一次原子判空）"]
        E3 -- 是 --> E5["构造 WaiterCandidate 列表<br/>（index + is_fiber，**不暴露 WaitNode**）"]
        E5 --> E6["selector->Select(...)<br/>异常被捕获 -> 退化为默认 FIFO"]
        E6 --> E7["**只把选中者与队首交换**"]
        E7 --> E8["之后仍由原循环逐个 TryFinish 认领<br/>→ 所有权无缝转移与 TryLock 不插队**不受影响**"]
    end

    subgraph EV["事件采集的批量化"]
        F1["OnEvent 在 worker 线程被调用"] --> F2["写入**线程局部**暂存（满 32 条）"]
        F2 --> F3{"满一批?"}
        F3 -- 否 --> F4["返回（不碰共享缓冲）"]
        F3 -- 是 --> F5["EventBuffer::PushBatch(data, count)<br/>**一次预留，原子操作从 count 次降到 1 次**"]
        F5 --> F6["Pending/Captured/Peek/Drain 读取前先 FlushStaging<br/>否则「刚写未攒够一批」不可见"]
    end

    subgraph SIG["跨进程 dump"]
        G1["外部 kill -USR1 pid"] --> G2["信号处理器：**只** atomic 置位<br/>（async-signal-safe）"]
        G2 --> G3["PollOnce 在主循环 / watcher 线程检查"]
        G3 --> G4["DebugScheduler::DumpText(include_fibers=true)"]
        G4 --> G5["原子写到 go2cpp-signal-dump-<pid>-<ns>.txt"]
        G5 --> G6["若绑定了采集器，另写 .events.ini"]
    end

    DUMP --> D3
    D3 --> EV
    EV --> SIG
```

**要点（含设计上的硬约束）**

- **`DebugFiber` 是唯一用继承的地方**，因为它是"用户自己创建并真实执行"的对象；其余全是**代理**，因为它们包装的是**框架已创建的对象**；
- **`DebugFiber` 绝不能放进 `Task::m_fiber`**：`Fiber` **没有虚析构**（实测 `is_polymorphic == 0`），通过 `unique_ptr<Fiber>` 销毁 `DebugFiber` 是 UB；
- **D3 只重排队列顺序**，不改"逐个 `TryFinish` 认领"的裁决方式，所以"所有权无缝转移、`TryLock` 不能插队"完全不受影响；
- **`FollowEvents` 必须带总超时**：否则缓冲一直为空时 `batches` 永远到不了上限 → 无限循环（**首版就踩了这个坑，导致整条 ctest 超时**）；
- **信号处理器只置原子标志**，绝不做 I/O/分配/加锁（那不是 async-signal-safe）；
- **`include_fibers` 默认关闭**：枚举全部 G 要遍历 16 个分片，成本高于其它快照，所以按需开启。

## ③ 向上层提供了什么

```mermaid
graph LR
    subgraph G10API["调试能力"]
        A1["dump scheduler [--format] [--output] [--include-fibers]"]
        A2["dump events [--follow[=N]] [--follow-interval] [--follow-timeout]"]
        A3["锁/条件变量/等待组只读快照（Debug* 代理）"]
        A4["D3：SetWaitSelectorFactory（锁争用策略）"]
        A5["协作式断点：DebugBreakpoints + Checkpoint/ScopedCheckpoint"]
        A6["跨进程 dump：DebugSignalDumper（SIGUSR1）"]
        A7["事件采集：DebugEventCollector（非阻塞有界 + 三格式序列化）"]
    end
    subgraph USE["谁在用"]
        U1["现场排障：给运行中的进程发 SIGUSR1 拿快照"]
        U2["性能/卡死分析：dump scheduler 看 P/M/队列/全 G"]
        U3["复现竞争：D3 指定谁先获锁"]
        U4["定位某条路径：在代码里插 Checkpoint"]
        U5["外部监控：实现 SchedulerObserver 接 DebugEventCollector"]
    end
    A1 --> U1
    A1 --> U2
    A2 --> U5
    A3 --> U2
    A4 --> U3
    A5 --> U4
    A6 --> U1
    A7 --> U5
```

## ④ 状态流转

### 一次 dump 请求的生命周期

```mermaid
stateDiagram-v2
    [*] --> kParsed : ParseDumpCommand 成功
    kParsed --> kInvalid : 非法参数（unknown target/format/负超时）
    kInvalid --> [*] : 返回 1 并报错（**不静默降级**）
    kParsed --> kSnapshot : 采集快照
    kSnapshot --> kSerialize : 格式化
    kSerialize --> kWriting : 写 stdout 或文件
    kWriting --> kCommitted : 原子 rename 完成
    kWriting --> kFailed : 短写 / rename 失败（**删除临时文件**）
    kCommitted --> [*]
    kFailed --> [*]
```

### 协作式断点的命中与恢复

```mermaid
stateDiagram-v2
    [*] --> kRegistered : DebugBreakpoints.Add(spec)
    kRegistered --> kSkipping : Checkpoint 命中但 skip 未用完
    kSkipping --> kRegistered : skip 递减
    kRegistered --> kWaiting : Checkpoint 命中（skip 用完）
    kWaiting --> kRegistered : Resume() / ResumeAll()
    kWaiting --> kDisabled : once=true（自动失效）
    kDisabled --> kRegistered : SetEnabled(id, true)
    note right of kWaiting
        **只在此处登记命中，不阻塞**；
        是否真的停下来等由调用方决定
    end note
```

### 锁争用选择器（D3）的决策流

```mermaid
stateDiagram-v2
    [*] --> kNoSelector : 未注册（默认 FIFO）
    kNoSelector --> kSelectorActive : SetWaitSelectorFactory(factory)
    kSelectorActive --> kDeciding : Unlock 持有队列锁时被调用
    kDeciding --> kKeepOrder : 返回 nullopt / 下标 0 / 越界
    kDeciding --> kSwapped : 返回有效下标（>0 且 <size）
    kSwapped --> kClaiming : **与队首交换**后进入原认领循环
    kKeepOrder --> kClaiming
    kClaiming --> kSelectorActive : TryFinish 认领成功并 Wake
    kSelectorActive --> kNoSelector : ResetWaitSelectorFactory()
    note right of kDeciding
        选择器抛出的异常被隔离，退化为默认 FIFO；
        不做饥饿保护（策略的责任）
    end note
```

## 与 Go 的差异

| 项 | Go | Go2Cpp |
|---|---|---|
| 观测接口 | `runtime/trace` + `pprof`（丰富） | `SchedulerObserver` 的 9 类**只读**事件 + dump 快照 |
| 控制能力 | 可用 `trace`/`debug` 包间接影响 | **只开放观测类与节律类**，结构类明确拒绝（P4 控制面不做）|
| 断点 | 用 `dlv` 任意行、任意变量 | **协作式**：只能在 `Checkpoint` 处停；**不强制 detach 任意 C++ 调用栈** |
| 跨进程 dump | `SIGQUIT` 打印全部 goroutine 栈 | `SIGUSR1` 写快照到文件（含全 G 列表与可选事件流）|
| 锁持有者 | `mutex` profile 有争用信息 | **无 owner 字段**（`m_state` 不记录持有者，Go 风格所有权允许 A 锁 B 解）|
| 时间基准 | `nanotime`（单调） | **双时间戳**（steady + wall），明确声明**不做跨机绝对对齐** |
