# G0 · 总览：10 组模块如何拼成一次 `go()` —— 一图看懂

> 这是「先看整体」的那一张。想看细节请进入对应组的图：G1~G10。
> 本图不复述任何组内部细节，只画**层次、依赖方向**，以及**一次协程生命周期穿过哪些层**。

## ① 分层与依赖方向（谁可以依赖谁）

```mermaid
graph TB
    subgraph L5["L5 · 应用协议  ·  G7"]
        P1["http / websocket / sse"]
    end
    subgraph L4["L4 · 网络基础  ·  G6"]
        N1["socket / address / framing / bytearray / endian"]
    end
    subgraph L3["L3 · IO 后端  ·  G5"]
        I1["io_backend / fd_manager / hook"]
    end
    subgraph L2["L2 · 并发原语  ·  G4 + 取消与错误 G3"]
        C1["channel / future / sync(Mutex/CondVar/WaitGroup)"]
        C2["context / error"]
    end
    subgraph L1["L1 · GMP 调度  ·  G2"]
        S1["Task(G) / Scheduler / P / M / worker_loop / sysmon"]
    end
    subgraph L0["L0 · 地基层  ·  G1"]
        F1["Fiber / FiberBin / ParkingCondition / Timer"]
    end
    subgraph LX["横切（被所有层使用）· G8 + G9"]
        X1["log / config"]
        X2["go() / defer / EventBatch / HybridMutex / FiberLocal / thread_policy"]
    end
    subgraph LD["调试（只读观察整个栈）· G10"]
        D1["DebugScheduler / DebugEventCollector / DebugBreakpoints / DebugSignalDumper"]
    end

    L5 --> L4
    L4 --> L3
    L3 --> L2
    L2 --> L1
    L1 --> L0
    LX -.-> L0
    LX -.-> L1
    LX -.-> L2
    LX -.-> L3
    LX -.-> L4
    LX -.-> L5
    LD -. 只读 .-> L1
    LD -. 只读 .-> L2
    LD -. 只读 .-> L0
```

**依赖规则**：箭头**只允许向下**（L5→L4→…→L0）。`LX` 与 `LD` 是横向的：
前者可被任意层使用，后者**只读**地观察、绝不反向控制。

## ② 一次 `go(fn)` 到完成，穿过了哪些层

```mermaid
sequenceDiagram
    participant U as 用户代码
    participant T as G9 go()/fiber
    participant S as G2 Scheduler
    participant W as G2 worker_loop(M)
    participant F as G1 Fiber
    participant G as 用户的 fn

    U->>T: go(fn)
    T->>S: Scheduler::spawn(fn)
    S->>S: 构造 Task + Fiber，登记 TaskRegistry（强持有）
    S->>S: enqueue：本地队列（上限 256）否则全局队列
    S-->>T: shared_ptr<Task>
    T-->>U: fiber（可 wait/cancel）

    W->>S: pop_front（本地 FIFO）或 work stealing（从别队队尾）
    W->>F: resume（首次进入时移出 m_function）
    F->>G: 执行用户函数

    Note over G,F: 若遇到阻塞（锁/channel/IO），走 L2 的等待原语
    G->>F: Suspend(Park) —— **只在 5 个安全点**
    F-->>W: 切回 worker_loop（Fiber 栈与局部变量原样保留）

    W->>S: 该 G 状态 kWaiting，队列里换下一个
    Note over S: 被 Wake 后重新入队（pending-wake 防丢唤醒）
    W->>F: 再次 resume
    F->>G: 从 Suspend 之后继续

    G-->>F: 正常返回 / 抛出
    F-->>W: FiberResumeResult{Completed|Failed}
    W->>S: state=kDead/kFailed，完成 Promise
    S-->>U: wait() 返回；失败可 rethrow_failure()
```

## ③ 阻塞一次，会跨过哪几层

```mermaid
flowchart TB
    A["用户：lock() / ch.recv() / read()"] --> B{"哪一类阻塞?"}
    B -- 锁或 channel --> C["L2：sync / channel<br/>构造 WaitNode + 注册等待队列"]
    B -- socket 读写 --> D["L3：hook 截获<br/>fd 设非阻塞 + 注册到 IOBackend"]
    B -- 定时 --> E["L1：TimerService"]
    C --> F["L1：park_wait 安全点"]
    D --> F
    E --> F
    F --> G{"能否立刻完成?"}
    G -- 能 --> H["ParkAction::kRequeued<br/>不进 park，省一次切换"]
    G -- 不能 --> I["ParkAction::kParked<br/>Fiber::Suspend 切回 worker"]
    I --> J["L0：Fiber 保存寄存器上下文<br/>栈与局部变量原样保留"]
    J --> K["worker 去跑别的 G"]
    K --> L["事件就绪 / 被 Wake"]
    L --> M["WakeAction::kPending 先挂标志<br/>（防丢唤醒）"]
    M --> N["重新入队 -> 再次 resume"]
    N --> O["重试原操作（EAGAIN 循环）"]
```

## ④ 调试模块如何只读地观察这一切

```mermaid
graph LR
    subgraph OBS["可观测点（只读）"]
        O1["SchedulerObserver 的 9 类事件<br/>（emit_event 唯一出口）"]
        O2["Scheduler 的只读快照<br/>processors / machines / metrics / DebugSnapshotTasks"]
        O3["sync 的 DebugSnapshotInternal<br/>（Mutex / CondVar / WaitGroup）"]
        O4["使用者手动插入的 Checkpoint"]
    end
    subgraph COLLECT["G10 调试模块"]
        C1["DebugEventCollector（非阻塞有界）"]
        C2["DebugScheduler 代理（三格式 dump）"]
        C3["DebugMutex 等代理"]
        C4["DebugBreakpoints"]
        C5["DebugSignalDumper（SIGUSR1）"]
    end
    O1 --> C1
    O2 --> C2
    O3 --> C3
    O4 --> C4
    C2 --> C5
    C1 --> C5
```

**方向性很重要**：`G10` 只有**指向框架的读箭头**，**没有**任何从框架指向 `G10` 的调用
（唯一的例外是 `SchedulerObserver::OnEvent`，那是框架本来就有的**只读**插件边界）。
因此**删掉整个 `debug` 模块，框架行为不变**。

## ⑤ 与 Go 的层次对照

| Go 的层次 | Go2Cpp 的对应 | 差异要点 |
|---|---|---|
| `runtime`（GMP + 抢占 + 栈增长） | **G1 + G2** | 纯协作、**固定栈**、无抢占 |
| `runtime` 的 netpoll | **G5**（可替换 `IOBackend`） | 需线程显式参与 hook |
| `sync` / `chan` | **G4** | 无自旋；受管 G 有**有界协作重试** |
| `context` / `errors` | **G3** | 具体类而非接口；`WrappedError` 可解包 |
| `net` / `net/http` / 第三方 ws | **G6 + G7** | ws 与 SSE **内置** |
| 标准库 log / 第三方 config | **G8** | 三抽象 + 类型安全 `ConfigVar<T>` |
| 无（Go 无 `defer` 之外的语法糖层） | **G9** | `go()`/`defer`/`EventBatch` 让 Go 代码近乎原样编译 |
| `runtime/trace` + `pprof` + `dlv` | **G10** | 覆盖更窄：**只读 + 协作式**，但可跨进程信号 dump |

## 怎么用这套图

1. **先看本图**（G0）建立层次与方向感；
2. **遇到具体问题再进对应的组**：卡死 → G2/G4；IO 慢 → G5/G6；协议问题 → G7；
   要排障 → G10；
3. 每组图内固定四段：类架构 → 重要函数实现 → 向上层提供什么 → 状态流转；
4. 每张图末尾都有「与 Go 的差异」表 —— **这个项目最容易出错的地方就是按 Go 语义误用**。
