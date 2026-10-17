# Hook 阻塞函数判定准则

> 本文记录「哪些会阻塞的系统调用应该被 hook（park Fiber）、哪些不该」的判定标准，
> 以及每条结论的依据。目的是避免为了"覆盖更多函数"而 hook 那些**醒不来**的调用。

## 一、判定准则（唯一一条）

**本项目的唤醒机制是 `IOManager` 的 epoll —— 也就是「某个 fd 变得可读/可写」。**

因此：

> **只有当调用可以归约为「在某个 fd 上等待它就绪」时，它才应该 park。**
> 否则 park 之后**没有任何东西能唤醒它** —— 那会把"worker 被阻塞"变成
> "**Fiber 永久挂起**"，后者更难查：没有崩溃、没有超时，就是不动。

### 推论：三类

| 类别 | 特征 | 处置 |
|---|---|---|
**A. 可 park** | 阻塞原因就是「某个 fd 就绪」，epoll 能报 | hook，park Fiber |
**B. 需桥接** | 阻塞事件不是 fd 就绪，但**可以**用 eventfd/timerfd/signalfd 造一个 | 先桥接，再 hook |
**C. 不该 hook** | 既不是 fd 就绪，也无法桥接；或本身就是事件循环入口 | 保持 `BlockingRegion` + sysmon 接管 |

---

## 二、判断表

### A. 可 park（已有 hook）

| 函数 | 阻塞原因 | 就绪事件 | 状态 |
|---|---|---|---|
`read` / `readv` / `recv` / `recvfrom` | 等数据 | fd 可读 | ✅ 已 hook（socket）|
`write` / `writev` / `send` / `sendto` | 等缓冲空间 | fd 可写 | ✅ 已 hook（socket）|
`connect` | 等连接建立 | fd 可写 | ✅ 已 hook |
`accept` | 等新连接 | fd 可读 | ✅ 已 hook |

### A'. 可 park 但**当前未 hook**（FIFO/pipe）

| 函数 | 阻塞原因 | 就绪事件 | 状态 |
|---|---|---|---|
**FIFO/pipe 上的 `read`** | 等数据 | **fd 可读** | ⚠️ 未 hook |
**FIFO/pipe 上的 `write`** | 等缓冲空间 | **fd 可写** | ⚠️ 未 hook |

**现状**：hook 用 `getsockopt(fd, SOL_SOCKET, SO_TYPE)` 判定是否接管，
对 FIFO/pipe 必然返回 `ENOTSOCK` → 落到 libc → **真阻塞 worker**。

**为什么没改**：见第四节「与既有设计的冲突」。

### B. 需桥接（**未实现**，只给方案）

| 函数 | 阻塞原因 | 桥接方案 | 备注 |
|---|---|---|---|
`fsync` / `fdatasync` | 等磁盘完成 | **`io_uring` + eventfd**，或 `AIO` + eventfd | 没有 fd 就绪可等 |
`wait4` / `waitpid` | 等子进程退出 | **`signalfd(SIGCHLD)`** | 把信号变成可读事件 |
`nanosleep` / `usleep` / `clock_nanosleep` | 等时间 | **`timerfd`** | 时间变成可读事件 |

**注意**：`sleep()`（秒级）已经通过 `cooperative_sleep_for` 实现了 park，
但它**不是**通过 epoll 唤醒的 —— 见下面对 `sleep` 的说明。

`sleep` 为什么能 park：它把「等到某个时间点」注册进调度器的**定时器**，
由 `Timer` 队列在到期时唤醒。这是与 epoll 并列的第二条唤醒路径，
所以 `nanosleep` 理论上也能走同一套定时器。**这一条需要进一步确认**
（`sleep` 的实现是否复用 `Timer`，还是另有机制）。

### C. 不该 hook

| 函数 | 理由 |
|---|---|
`poll` / `select` / `epoll_wait` | 它们**本身就是事件循环的入口**，不是"在某个 fd 上阻塞"。hook 它们会与 `IOManager` 内部的 `epoll_wait` 递归。且用户调用它们时**本来就知道自己在做事件循环**。当前 `invoke_native_blocking`（标记 `BlockingRegion` + 调原生）是正确行为。|
`close` / `shutdown` / `fcntl` / `dup` / `dup2` | 不阻塞在 fd 上；且关掉会导致 fd 元数据与内核状态脱节 |
`open`（对 FIFO） | 阻塞在"等对端打开"，**此时还没有 fd**，没有可等的就绪事件。正确解法是 `O_NONBLOCK` + 打开后由 `read` park —— `os` 模块已经这样做了。|
`getaddrinfo` | DNS 解析，不是 fd 操作（要走异步 resolver 才能不阻塞）|

---

## 三、`BlockingRegion` / sysmon 机制（不要破坏它）

对 **C 类**以及**未接管的 fd**，项目现有设计是：

1. `hook` 构造 `BlockingRegion`（标记"M 正在做可能很长的系统调用"）
2. `sysmon` 观察到长阻塞后，**申请替代 M**（`machine.processor_detached`）
3. 该 P 上的其他 Fiber 由替代 M 继续执行

**这与 park 是两条并行机制**：
- park：Fiber 挂起，**工作线程立即空出来**，代价是 ~1µs 调度往返
- BlockingRegion + detach：worker **真的阻塞**，但 sysmon 会补一个新 M，代价是线程创建

**`tests/network/test_hook.cpp:435-470` 明确依赖后者**：它建一个 pipe，
一个 Fiber 调 `read`（无数据），期望 `processor_detached` 被置位。

---

## 四、FIFO park：已实测可行，但与既有设计冲突（**未合入**）

### 实测数据（加 `manager.start()` 之后）

```
[hook-fifo] hook read(pipe):     400ms 内心跳增量 = 39，读到 5 字节
[hook-fifo] read_f(pipe) [对照]: 400ms 内心跳增量 = 0，读到 5 字节
```

**39 vs 0** —— 说明实现是有效的：`read` 在 pipe 上 park 了（worker 空闲，心跳照走），
`read_f` 真阻塞了 worker（心跳停）。

### 实现方式（约 90 行）

1. `OpenDescription` 加 `bool m_pollable{true}`，`can_wait` 用它替代 `m_socket`
2. 加 `inspect_fifo(fd)`：`::fstat` + `S_ISFIFO`（`getsockopt` 认不出 FIFO）
3. 加 `adopt_fifo(fd)`：照 `adopt_socket` 的结构，但不读 `SO_RCVTIMEO`，`m_socket=false`
4. `cooperative_io` 里 socket 接管失败后再试 FIFO

### 为什么没合入

**它改变了既有契约**：`tests/network/test_hook.cpp:449` 断言 pipe 上的 `read`
会触发 `machine.processor_detached`。一旦 park，worker 不再阻塞，
sysmon 就不会 detach，该断言失败。

**这是架构选择，不是 bug**：

| 方案 | 优点 | 缺点 |
|---|---|---|
park（我实现的） | worker 立即空出，无线程创建开销 | 绕过 `BlockingRegion`/sysmon 设计；需改既有测试 |
BlockingRegion（现行） | 统一机制；`sysmon` 可观测 | 真的占一个线程，靠补 M 维持 |

**如果将来要合入**，需要同时：
1. 明确 `BlockingRegion` 与 park 的适用边界（哪些 fd 走哪条）
2. 修改 `test_hook.cpp` 的断言（或让它覆盖两条路径）
3. 补 `*_f` 的对照测试

---

## 五、一个**假阳性**测试（需要修）

`tests/network/test_hook_raw.cpp` 场景 3 断言：

```cpp
Check(delta < 8, "read_f must block the worker (heartbeats should be nearly flat)");
```

**但它从未调用 `manager.start()`** —— `IOManager` 只构造不启动，
`Go()` 只是入队，心跳 Fiber **根本不会运行**，`delta` 恒为 0，
所以这个断言**永远通过，什么也没证明**。

诊断证据：

```
[diag] read 绑定到: .../libgo2cpp_hook.so.0     ← hook 确实生效
[diag] 第 1 个 100ms 后心跳 = 0
[diag] 第 2 个 100ms 后心跳 = 0
[diag] 第 3 个 100ms 后心跳 = 0                 ← 心跳完全没跑
```

**修法**：在构造 `IOManager` 之后加 `manager.start()`，
并先断言"空转 100ms 后心跳 > 3"作为探针，再断言阻塞期间心跳变平。

**同一个坑也可能影响其它用「心跳增量」做证据的测试** —— 建议排查。

---

## 六、结论汇总

| 项 | 结论 |
|---|---|
**判断准则** | 能归约为「fd 就绪」才能 park，否则会变成 Fiber 永久挂起 |
**已 hook** | socket 上的 read/write/connect/accept + sleep |
**FIFO/pipe** | 技术上可 park（已实测 39 vs 0），但与 `BlockingRegion`/sysmon 现有设计冲突，**未合入** |
**fsync/wait4/nanosleep** | 需 eventfd/signalfd/timerfd 桥接，**未实现**（方案已给）|
**poll/select/epoll_wait** | 不该 hook（事件循环入口）|
**已知缺陷** | `test_hook_raw.cpp` 场景 3 是假阳性（缺 `manager.start()`）|
