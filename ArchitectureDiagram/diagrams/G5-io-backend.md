# G5 · IO 后端与文件描述符管理 —— 一图看懂

> 覆盖：`IOBackend`（可替换的事件循环后端）、`BackendEvent` / `BackendEventFlag`、
> `FdManager` / `FdContext`（fd 的元信息与超时）、`hook`（把阻塞系统调用转成协程挂起）。
> 源码：`include/go2cpp/network/io_backend.hpp`、`fd_manager.hpp`、`hook.hpp`。

**这一层解决的问题**：让"看起来阻塞"的 socket 读写**不阻塞工作线程**，
办法是**把 fd 设为非阻塞 + 交给事件循环 + 让当前 G 挂起**。

## ① 类架构：每个类用的底层能力 + 主要职责

```mermaid
classDiagram
    direction TB

    class BackendEventFlag {
        <<事件标志位>>
        kReadable
        kWritable
        kError
        kHangup
    }
    class BackendEvent {
        <<一个就绪事件>>
        +token uint64
        +events uint32
    }
    class IOBackend {
        <<可替换的事件循环后端>>
        +Add(fd, token, events) bool
        +Mod(fd, events) bool
        +Del(fd) bool
        +Wait(timeout, out) int
        ← 工厂可替换（epoll / poll / 测试用假后端）
    }
    class IOBackendFactory {
        <<工厂>>
    }

    class FdContext {
        <<单个 fd 的元信息>>
        +fd() int
        +initialized() bool
        +closed() bool
        +user_nonblock() bool
        +system_nonblock() bool
        ← 区分"用户设了非阻塞"与"运行时为了挂起设了非阻塞"
    }
    class FdManager {
        <<fd 表>>
        +Acquire(fd) FdContext
        +erase(fd) void
        +set_nonblock(fd, user, system) bool
        +set_timeout(fd, receive, timeout) bool
        ← 每个 fd 的收发超时都记在这里
    }

    class ThreadHookMode {
        <<线程是否参与 hook>>
    }
    class ScopedEnable {
        <<RAII 开关 hook>>
    }
    class ScopedIOManagerBinding {
        <<RAII 绑定当前线程到 IO 管理器>>
    }
    class ParkingCondition {
        <<G1 等待原语>>
    }
    class Scheduler {
        <<G2 调度器>>
        +park_wait(node)
    }

    IOBackendFactory ..> IOBackend : 创建
    IOBackend ..> BackendEvent : 输出就绪事件
    BackendEvent ..> BackendEventFlag : events 位
    FdManager *-- FdContext : 一 fd 一上下文
    ScopedEnable ..> ThreadHookMode : RAII 切换
    ScopedIOManagerBinding ..> FdManager
    ParkingCondition ..> Scheduler : 挂起受管 G
    FdManager ..> IOBackend : 非阻塞 + 注册到事件循环
```

| 类 | 主要职责 | 用的底层能力 |
|---|---|---|
| `IOBackend` | **可替换**的事件循环：注册/修改/删除 fd 监听，`Wait` 返回就绪事件 | `epoll`/`poll`（当前实现）、fd |
| `BackendEvent` / `BackendEventFlag` | 就绪事件与标志位（可读/可写/错误/挂断） | 无（纯数据） |
| `FdManager` | fd 的元信息表：是否非阻塞（**用户设置的**与**运行时设置的**分开记）、收发超时 | `FdContext` |
| `FdContext` | 单 fd 上下文；区分 `user_nonblock` 与 `system_nonblock` 是关键 | 无 |
| `hook`（`ScopedEnable` 等） | 把线程标记为"参与 hook"，从而让阻塞调用走"挂起"路径 | `ThreadHookMode`（线程局部） |

## ② 重要函数：功能与实现要点

```mermaid
flowchart TB
    subgraph READ["一次「看起来阻塞」的 socket 读"]
        R1["业务代码调用 read()/recv()"] --> R2{"当前线程参与 hook?<br/>（ThreadHookMode）"}
        R2 -- 否 --> R3["直接走系统调用（真阻塞）<br/>—— 普通线程不受影响"]
        R2 -- 是 --> R4{"当前是否受管 G?"}
        R4 -- 否 --> R3
        R4 -- 是 --> R5["把 fd 设为**非阻塞**（记录 system_nonblock）"]
        R5 --> R6["尝试真正读一次"]
        R6 --> R7{"EAGAIN?"}
        R7 -- 否 --> R8["直接返回数据"]
        R7 -- 是 --> R9["注册到 IOBackend（关心可读）<br/>并 park 当前 G"]
        R9 --> R10["事件循环发现可读 -> Wake 该 G"]
        R10 --> R11["G 恢复后**重试读**<br/>（循环直到成功或真错）"]
        R11 --> R8
    end

    subgraph TIMEOUT["超时"]
        T1["FdManager.set_timeout(fd, receive, timeout)"] --> T2["记在 FdContext 里"]
        T2 --> T3["等待时把 timeout 传给 park 路径"]
        T3 --> T4["超时到 -> G 被唤醒并返回超时错误"]
    end

    subgraph NB["非阻塞语义的保持"]
        N1["若用户自己设了非阻塞（user_nonblock）"] --> N2["hook 不把它变成「会等待」<br/>而是直接返回 EAGAIN"]
        N2 --> N3["因此用户的非阻塞语义**不被 hook 破坏**"]
    end

    READ --> TIMEOUT
    READ --> NB
```

**要点**

- **两套非阻塞标记**（`user_nonblock` / `system_nonblock`）：运行时为了挂起会把 fd 设成非阻塞，但**用户自己设的非阻塞不能因此变成"会等待"** —— 这个区分是正确性的关键；
- hook 只在**受管 G** 上生效（源码里 `BlockingRegion` 也只对 managed G 生效），普通线程仍走真阻塞；
- 挂起后**必须重试**读/写（因为事件就绪不等于一次能读够）；
- 超时记在 `FdContext`，不是每次调用传参。

## ③ 向上层提供了什么

```mermaid
graph LR
    subgraph G5API["IO 能力"]
        A1["IOBackend：可替换事件循环 + 工厂"]
        A2["FdManager：fd 元信息 / 非阻塞 / 收发超时"]
        A3["hook：阻塞调用 -> 协程挂起"]
        A4["ScopedEnable / ScopedIOManagerBinding：RAII 开关"]
    end
    subgraph USE["谁在用"]
        U1["socket：连接/收发走 hook"]
        U2["socket_stream：带缓冲的读写"]
        U3["network/http、websocket、sse：全部建立在 hook 之上"]
        U4["调试模块：不直接用它，但 block 生命周期事件由 scheduler 观测"]
    end
    A1 --> U1
    A2 --> U1
    A3 --> U2
    A3 --> U3
    A1 --> U3
```

## ④ 状态流转

### 一个 fd 在 hook 下的状态

```mermaid
stateDiagram-v2
    [*] --> kFresh : 创建（可能在 FdManager 里还没有记录）
    kFresh --> kRegistered : 首次参与挂起，登记到 FdManager + IOBackend
    kRegistered --> kWaitingRead : 读 EAGAIN -> 注册可读并 park
    kRegistered --> kWaitingWrite : 写 EAGAIN -> 注册可写并 park
    kWaitingRead --> kRegistered : 事件就绪 -> Wake -> 重试
    kWaitingWrite --> kRegistered : 事件就绪 -> Wake -> 重试
    kRegistered --> kClosed : close() -> 从 FdManager 与 IOBackend 摘除
    kWaitingRead --> kClosed : 等待中被关闭（唤醒并报错）
    kWaitingWrite --> kClosed : 同上
    kClosed --> [*]
```

### 线程的 hook 模式

```mermaid
stateDiagram-v2
    [*] --> kDisabled : 新建线程（默认不参与 hook）
    kDisabled --> kEnabled : ScopedEnable 构造 / 设置 ThreadHookMode
    kEnabled --> kDisabled : ScopedEnable 析构（RAII 恢复）
    note right of kEnabled
        只在 enabled 且当前是受管 G 时，
        阻塞调用才会转成挂起
    end note
```

## 与 Go 的差异

| 项 | Go | Go2Cpp |
|---|---|---|
| 事件循环 | `netpoll`（内部集成） | **可替换的 `IOBackend`**（工厂注入） |
| 阻塞转挂起 | 运行时自动接管所有 fd | 需线程**显式参与 hook**（`ThreadHookMode`） |
| 非阻塞语义 | 用户不可见 | **区分 user/system 两套标记**，用户语义不被破坏 |
| 超时 | `SetDeadline` 系列 | `FdManager::set_timeout`（按 fd 记录） |
