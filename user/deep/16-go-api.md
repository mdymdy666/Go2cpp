# Go API 兼容层（go-api）· 深入学习

> 本文按「总—分」写。所有 `文件:行` 都用 `sed -n` 复核。性能只写「本机实测（命令+环境+轮数）」，
> 没测过就写「未实测」。这一层是**门面**，不是运行时；判断问题时先分「映射错」还是「底层错」。

---

## 第一部分 · 总：这个模块怎么跑起来

### 1.1 入口点清单

| 包 | 入口（类/函数） | 文件:行 | 谁调用它 | 触发什么 | 返回什么 |
|---|---|---|---|---|---|
| error | `go::error`（=`ErrorPtr`） | `include/go2cpp/go/error.go.hpp:49` | 所有包 | 错误接口值；`nullptr` = nil | `shared_ptr<const Error>` |
| error | `New/Wrap/Join/Unwrap` | `include/go2cpp/go/error.go.hpp:52/57/62/72` | 业务代码 | 建错误/包一层/聚合/解一层 | `error` |
| error | `Is/As<T>/ErrorString` | `include/go2cpp/go/error.go.hpp:77/83/88` | 业务代码 | 错误图匹配/类型提取/文本 | bool / `shared_ptr<const T>` / string |
| error | `ErrEOF/ErrClosed/ErrTimeout` | `include/go2cpp/go/error.go.hpp:135+` | io/net | 哨兵错误 | `const error&` |
| context | `Background/TODO` | `include/go2cpp/go/context.go.hpp:58/63` | 所有包 | 根上下文 | `ContextPtr` |
| context | `WithCancel/WithTimeout/WithDeadline` | `include/go2cpp/go/context.go.hpp:82/94/101` | 业务代码 | 派生子上下文 + 取消函数 | `CancelContext{context,cancel}` |
| context | `Done/Err/Cause/Deadline/Value` | include/go2cpp/go/context.go.hpp:121/127/... | 业务代码 | 读取消信号/原因/截止/值 | `DoneSignal&` / `error` / optional |
| context | `WaitDone(ctx[,timeout])` | `include/go2cpp/go/context.go.hpp:143-151` | 业务代码 | 阻塞等取消 | void / bool |
| chan | `Chan<T>::Make(cap)` | `include/go2cpp/go/chan.go.hpp:73-89` | 业务代码 | 创建有/无缓冲通道 | `Chan<T>` |
| chan | `Send/SendCtx/TrySend` | `include/go2cpp/go/chan.go.hpp:95/103/111` | 业务代码 | 发送（阻塞/可取消/非阻塞） | bool |
| chan | `Recv/RecvCtx/TryRecv` | `include/go2cpp/go/chan.go.hpp:117/131/159` | 业务代码 | 接收 | `pair<T,bool>` / optional / bool |
| chan | `Close/IsClosed/Len/Cap` | `include/go2cpp/go/chan.go.hpp:172/188/185/180` | 业务代码 | 关闭/查询 | bool / size_t |
| chan | `Select(cases)` | `include/go2cpp/go/chan.go.hpp:235/243` | 业务代码 | 多路等待 | `SelectResult{index,value,...}` |
| sync | `Mutex::Lock/Unlock/TryLock/LockCtx` | `include/go2cpp/go/sync.go.hpp:57-87` | 业务代码 | Fiber 感知互斥 | void/bool |
| sync | `RWMutex::RLock/...` | `include/go2cpp/go/sync.go.hpp:93-112` | 业务代码 | 读写锁（写者优先） | void/bool |
| sync | `WaitGroup::Add/Done/Wait/WaitCtx` | `include/go2cpp/go/sync.go.hpp:114-145` | 业务代码 | 计数等待 | void/bool |
| sync | `Once::Do/DoNoThrow` | `include/go2cpp/go/sync.go.hpp:147-170` | 业务代码 | 只执行一次 | bool |
| sync | `Cond` | `include/go2cpp/go/sync.go.hpp:191+` | 业务代码 | 条件变量 | — |
| time | `Now/Since/Until/Sleep` | `include/go2cpp/go/time.go.hpp:69/74/79/84` | 业务代码 | 时钟/睡眠 | Time/Duration |
| time | `NewTimer/After/AfterChan/NewTicker` | `time.go.hpp`（`After` 等） | 业务代码 | 定时器 | `Timer::Ptr` / `Chan<Time>` |
| io | `ReadFull/CopyBuffer/WriteString` | `include/go2cpp/go/io.go.hpp:83/105/117` | 业务代码 | 读满/拷贝/写串 | `ReadResult`/`WriteResult` |
| io | `ConnIO` | `include/go2cpp/go/io.go.hpp:186` | 业务代码 | net::Conn → io 适配 | 对象 |
| net | `Listen/Dial` | `net.go.hpp`（`Listen`/`Dial` 自由函数） | 业务代码 | TCP/UDP 监听/连接 | `ListenResult`/`DialResult` |
| net | `Conn::Read/Write/Close` | `include/go2cpp/go/net.go.hpp:102-123` | 业务代码 | 字节流 IO | `IOResult{n,err}` / error |
| net | `SetDeadline/SetRead/WriteDeadline` | `include/go2cpp/go/net.go.hpp:116-123` | 业务代码 | 设置超时 | error |
| net | `Deadline`（内部） | `include/go2cpp/go/net.go.hpp:176-213` | `TCPConn/UDPConn` | 线程安全 deadline 槽 | `SocketDuration` |
| signal | `Notify/Stop/Ignore/Reset` | `include/go2cpp/go/os_signal.hpp:493/560/...` | 业务代码 | 注册/撤销/忽略/恢复 | void |
| signal | `NotifyList` / `InstallHandler` | `include/go2cpp/go/os_signal.hpp:493/381` | 内部 | 装 sigaction + 起分发线程 | void/bool |
| crc32 | `ChecksumIEEE/Checksum/Update/New` | `include/go2cpp/go/crc32.hpp:112/121/132/138` | 业务代码 | 标准 CRC-32 | uint32/`Hash` |
| rand | `Read/Int/Prime` | `crypto_rand.hpp`（`Source` 141、`BigInt` 185、`BigResult` 272） | 业务代码 | CSPRNG/大整数/素数 | `BigResult` |
| flag | `FlagSet::Parse/Set/PrintDefaults` | `include/go2cpp/go/flag.hpp:495/546/581` | 业务代码 | 命令行解析 | `ParseResult` |
| flag | 全局 `Parse/Set/PrintDefaults` | `include/go2cpp/go/flag.hpp:806/836/851` | 业务代码 | 包级 FlagSet | 同左 |

平台/编译：
- `signal` 只有 POSIX 实现；`Supported()` 在 Windows 返回 false（`include/go2cpp/go/os_signal.hpp:290-296`）。
- `net` 依赖 `go2cpp::net::Socket`（Fiber 感知的非阻塞 + IOManager 等待）。
- `crc32` / `crypto_rand` 是项目 `hash` / 自带实现的别名/包装，不是重新实现。
- 没有独立的 `go::os` 包；`os/signal` 在 `os_signal.hpp`，文件系统能力走 `go2cpp::os`。

### 1.2 总架构图

```
业务代码（原本 Go）
   │
   ├─ go::chan   ──► go2cpp::Channel<T>（Fiber/线程混合等待）
   ├─ go::context──► go2cpp::Context / DoneSignal（取消树）
   ├─ go::error  ──► go2cpp::Error / ErrorPtr（错误图）
   ├─ go::sync   ──► go2cpp::sync::Mutex/WaitGroup/Once/Cond
   ├─ go::time   ──► go2cpp::time::Timer/Ticker
   ├─ go::io     ──► go2cpp::streams::{Reader,Writer,Copy,ReadFull}
   ├─ go::net    ──► go2cpp::net::Socket（非阻塞 + hook + IOManager）
   ├─ go::signal ──► sigaction + pipe + 分发线程 + Channel<TrySend>
   ├─ go::crc32  ──► go2cpp::hash::Crc32 / Table
   ├─ go::rand   ──► getrandom/OS Source + BigInt（Miller-Rabin）
   └─ go::flag   ──► 自己的 FlagSet/Value/ParseDuration

阻塞点路径（最重要）：
  Fiber 调 go::net::Conn::Read
    → Socket::recv → EAGAIN → hook 拦截 → 挂起当前 Fiber + 注册 epoll
    → 切到别的 G；数据到达 → 唤醒 → recv 返回
  在裸 std::thread 里调同一函数 = 线程阻塞（合法，但线程数会涨）

信号路径：
  kernel → sigaction handler（任意线程）
    → 读 watched_mask → write(pipe)
    → 分发线程 read(pipe) → watchers[signo] 快照 → Channel::TrySend
```

边界：
- 门面**没有状态**（`signal` 的进程级单例是唯一例外）；不要在门面加缓存/重试/队列。
- 阻塞点必须进调度器；否则 Fiber 会占住 M。
- 大多数「与 Go 不一致」是**刻意的安全差异**（nil channel 不挂死、closed 不 panic），
  移植时要显式处理。

### 1.3 复杂场景一：一次 context 取消如何传播到 Fiber/IO

1. 业务：`auto [ctx, cancel] = go::WithTimeout(go::Background(), 100ms);`
   （`include/go2cpp/go/context.go.hpp:94`）→ `Context::WithTimeout`（`src/concurrency/context.cpp:238` 附近）
   创建子 `State`，把父 `DoneSignal` 的内部回调注册到子（`src/concurrency/context.cpp:1854-1877`）。
2. 子 `State` 同时起一个 timer，到期调 `CancelDeadline()`；timer 句柄存在 `State` 里，
   `cancel` 或析构时可以撤销。
3. Fiber 调 `go::net::Conn::Read` → `TCPConn::Read`（`include/go2cpp/go/net.go.hpp:326`）→
   `m_socket->recv(buffer, size, 0, timeout)`。
4. `Socket::recv` 在非阻塞 fd 上 `recvmsg`；EAGAIN → `wait_ready(fd, kRead, left, cancel_gen)`
   → hook 把当前 Fiber 挂起并注册 epoll（`src/network/socket.cpp` 的 `recv_impl`）。
5. 100ms 到期（或显式 `cancel()`）：timer 线程/调用线程进入 `DoneSignal::Signal()`
   （`src/concurrency/context.cpp:667`）。
6. `Signal()` 锁内把 `internal_callbacks` / `callbacks` 搬到局部，置 `done=true`，锁外
   `notify_all()`（`src/concurrency/context.cpp:690-698`）。
7. 先跑 internal 回调：`ContextRollback` 冻结 undo 记录；再跑用户回调。
   典型用户回调只做 `chan.TrySend` / `Future.Notify`（`include/go2cpp/concurrency/context.hpp:25-27` 的警告）。
8. 等待 `DoneSignal` 的 Fiber 被 `notify_all` 唤醒（`DoneSignal::Wait` 的 cv）；
   或 `Select` 的 `DoneCase` 被选中（`include/go2cpp/go/chan.go.hpp:221`）。
9. 如果 IO 等待注册了取消回调，Socket 的 `wait_ready` 被唤醒后返回 `ECANCELED`；
   `recv` 返回 `{-1, ECANCELED}`；`MapSocketError` 把它映射成什么？
   看 `include/go2cpp/go/net.go.hpp:216-232`：`ECANCELED` 不在超时/关闭分支，会落到
   `New("read: " + strerror)`，**不是** `ErrCanceled`（这是一个语义缺口，移植时要自己判）。
10. `TCPConn::Read` 返回 `{0, err}`；业务看到错误，检查 `go::Err(ctx)` 或
    `go::Is(err, go::ErrCanceled())`。
11. `cancel()` 返回后，子 `State` 的 timer 被撤销；`ContextPtr` 释放时若还有子节点，
    父链用线程本地释放队列迭代析构，避免深树爆栈（`context.cpp` 的
    `t_state_release_queue`）。
12. 失败/边界：如果回调里阻塞 2s，`cancel()` 就 2s 才返回（同步回调，见 Q02）；
    如果父上下文先取消，子通过 `CancelFromParent` 级联，timer 被撤销。

### 1.4 复杂场景二：error 包装 + 提前返回时的清理（没有 defer）

1. Go 写法：
   ```go
   f, err := os.Open(name)
   if err != nil { return fmt.Errorf("open %s: %w", name, err) }
   defer f.Close()
   ...
   ```
   C++ 没有 `defer`；对应物是 RAII/scope guard + `go::Wrap`。
2. 打开：`go::net::Dial` / 文件 API 返回 `Result<T>`（`include/go2cpp/go/error.go.hpp:174-205`）；
   失败时 `err != nullptr`，成功时 `value` 有效。
3. 包装：`err = go::Wrap(err, "open " + name)`（`include/go2cpp/go/error.go.hpp:57`）→ `go2cpp::Wrap`
   建 `WrappedError`，`Unwrap()` 指向原因。
4. 判错：`go::Is(err, go::ErrClosed())` 沿错误图遍历，调用每层的 `Is`；
   `SentinelError` 的文本扩展只在哨兵层生效（Q01）。
5. 清理：用 `std::unique_ptr` / 自定义 `ScopeGuard`（析构里 `Close()`）；
   不能依赖 `defer` 的「函数返回时执行」，要保证所有提前 `return` 路径都触发析构。
6. 错误文本：`go::ErrorString(err)` 返回最外层 message；要完整链需要自己 `Unwrap` 循环，
   或 `Join` 多条。
7. 多错误：`go::Join({e1,e2})` 建 `JoinError`，`UnwrapAll()` 返回全部原因；
   `Is` 会遍历所有分支（带地址去重，防环）。
8. 异常边界：RPC/反射层可能抛 `RpcError`；门面函数本身不抛（除 `WaitGroup::Wait` /
   `Mutex::Lock` 在无调度器时抛 `std::logic_error`）。
9. 清理顺序：C++ 析构逆序；如果 `Close()` 可能抛，scope guard 的析构必须 `noexcept`
   并吞异常，否则 `std::terminate`。
10. 失败路径：包装后忘记保留原因 → `Is` 断链；把 `error` 存成 `shared_ptr<Derived>` 再转
    `error`，空指针仍是 nil（C++ 没有 Go 的 typed-nil 陷阱）。
11. 测试：`go_api_concurrency` 的 errors 段覆盖 New/Wrap/Join/Is/As；`go_api_stress`
    有 1000 层 errors。
12. 与 Go 的差异：`ErrorString(nil)` 返回空串（Go panic）；`As<T>` 返回
    `shared_ptr<const T>` 而不是写回指针；`Join` 的文本格式由实现决定。

### 1.5 关键不变量清单

| # | 不变量 | 证据 | 破法后果 |
|---|---|---|---|
| 1 | 门面不持有状态/不加缓存/不重试 | `user/docs/16-go-api.md:327-333`、各头文件 | 语义分裂成两套 |
| 2 | 阻塞点必须进调度器 | `include/go2cpp/go/net.go.hpp:326-338`、`Socket::recv` hook | Fiber 占住 M，吞吐塌 |
| 3 | `go::error` 是 `shared_ptr<const Error>`，空 = nil | `include/go2cpp/go/error.go.hpp:26-30`、49 | 把 `New("")` 当 nil |
| 4 | 哨兵文本匹配只在 `SentinelError` 层 | `include/go2cpp/go/error.go.hpp:110-122` | 误以为所有 "EOF" 都匹配 |
| 5 | context 回调在通知线程同步执行 | `include/go2cpp/concurrency/context.hpp:25-27`、`src/concurrency/context.cpp:698-718` | 回调阻塞拖住整棵取消树 |
| 6 | 取消顺序：done → notify → internal → user | `src/concurrency/context.cpp:667-718` | 调换会破坏 rollback/等待者 |
| 7 | nil channel 不阻塞、closed 不 panic | `include/go2cpp/go/chan.go.hpp:38-42`、95-100、172-177 | 移植代码静默丢数据 |
| 8 | `WaitGroup::Wait`/`Mutex::Lock` 无调度器会抛 | `include/go2cpp/go/sync.go.hpp:68-70`、124-128 | 裸线程里未捕获 → terminate |
| 9 | signal handler 只做 async-signal-safe 操作 | `include/go2cpp/go/os_signal.hpp:381-415` | 死锁/崩溃 |
| 10 | `net.Conn::Write` 允许部分写 | `include/go2cpp/go/net.go.hpp:108`、326-338 | 半帧发送，协议错位 |

---

## 第二部分 · 分：逐块细讲

### 2.1 error

**职责**：用 C++ 值语义表达 Go 的 `error` 接口，保留 `Is/As/Unwrap/Join`。

- `using error = ErrorPtr`（`include/go2cpp/go/error.go.hpp:49`）：`shared_ptr<const Error>`；零值 = nil。
- `New`（52）→ `NewError`；`Wrap`（57）→ `go2cpp::Wrap`；`Join`（62/67）；`Unwrap`（72）。
- `Is`（77）→ `go2cpp::Is`：遍历错误图，调用源错误的 `Is(target)`。
- `As<T>`（83）：`T` 必须继承 `go2cpp::Error`，返回 `shared_ptr<const T>`。
- `SentinelError`（110-126）：`Is` 先比地址，再 `dynamic_cast` + 文本相等；用于
  `ErrEOF/ErrClosed/ErrTimeout`（135+）。
- `Result<T>` / `Status`（174-240）：`(T, error)` / `error` 的 C++ 表达。

**匹配规则**：
```
Is(err, target):
  err == nullptr ? target == nullptr
  : err->Is(*target) || Is(err->Unwrap(), target)   // Join 遍历所有原因
```
**失败路径**：`As<T>` 类型不匹配返回 null；`Wrap(nil, msg)` 是否允许？看 `Wrap` 实现，
一般允许（产生无原因错误）；`Join({})` 返回 nil 或空 JoinError，需确认。
**误用**：把 `New("EOF")` 当 `ErrEOF()`；用文本比较替代 `Is`；持有 `BoundMethod*`
跨 Server 生命周期（那是 RPC，不是 error）。

### 2.2 context

**职责**：取消树 + `DoneSignal` + 截止时间 + 值查找。

- `Context`（`include/go2cpp/concurrency/context.hpp:208+`）：`Background/TODO`、
  `WithCancel/WithCancelCause/WithDeadline/WithTimeout`、`Done/Err/Cause/Deadline/Value`。
- `DoneSignal`（71-162）：`Wait/WaitFor/IsDone/AddCallback/AddBeforeDoneCallback`；
  拷贝共享同一状态。
- `CancelFunc` = `std::function<void()>`；`CancelCauseFunc` = `std::function<void(ErrorPtr)>`。
- 取消实现（`src/concurrency/context.cpp:1266-1288`）：`Cancel` 构造 `CancellationWork`，`DrainCancellation`
  迭代传播；已完成直接返回（幂等）。
- `Signal()`（`src/concurrency/context.cpp:667-718`）：顺序 done → notify_all → internal → user；
  每个回调 try/catch。
- 值查找（`src/concurrency/context.cpp:1300+`）：`Lookup` 沿 parent 链，先 typed_values 再 named_values。
- 深树释放：线程本地 `t_state_release_queue` 迭代析构，避免递归爆栈。

**状态机**：
```
Active ──cancel()/deadline/父取消──► Canceled(Err=Canceled/DeadlineExceeded, Cause=...)
   │                                          │
   └──────────── Done().Wait() ◄──────────────┘
```
**失败路径**：父已取消 → 子立即取消；timer 已触发 → cancel 幂等；回调抛异常被吞。
**误用**：不调用 `cancel` 导致 timer 存活到 deadline；在回调里阻塞；把 `Done()` 当 channel
做 `select` 之外的忙轮询。

### 2.3 chan / select

- `Chan<T>`（`include/go2cpp/go/chan.go.hpp:73-204`）：包装 `go2cpp::Channel<T>`；`Make(cap)`、`Send`、
  `SendCtx`、`TrySend`、`Recv`、`RecvCtx`、`TryRecv`、`Close`、`Len/Cap`、`RecvCase/SendCase`。
- `SelectCase` / `SelectResult`：底层 `go2cpp::Select`；`DefaultCase`（216）、`DoneCase`（221）、
  `AfterCase`（time.go.hpp）。
- 语义替换（38-42）：nil channel 不阻塞；closed Send 返回 false；重复 Close 返回 false；
  `Recv` 关闭后 `{T{},false}`，缓冲先吐完。
- `select` 公平性：底层 `Select` 负责；C++ 侧只保证 `index` 对应传入顺序。

**失败路径**：nil/closed/TrySend 满/RecvCtx 取消。**误用**：用 false 同时表示「关闭」和
「无数据」；在 select 里漏 `DefaultCase` 导致阻塞。

### 2.4 sync

- `Mutex`（`include/go2cpp/go/sync.go.hpp:57-91`）：`Lock/Unlock/TryLock/LockCtx`；`Lock` 失败抛
  `std::logic_error`（无调度器）。
- `RWMutex`（93-112）：写者优先；`RLock/RUnlock/Lock/Unlock/TryRLock/TryLock`。
- `WaitGroup`（114-145）：`Add/Done/Wait/WaitCtx/WaitFor`；`Wait` 无调度器抛。
- `Once`（147-170）：`Do` 用 `std::call_once`（panic 不标记）；`DoNoThrow` 才是 Go 语义。
- `Cond`（191+）：包装条件变量，支持 Fiber。

**失败路径**：计数为负、未 Add 就 Done、无调度器 Wait、Once 内抛异常。
**误用**：在裸 `std::thread` 里用 `Wait()`；把 `Once::Do` 当 Go 的 `Once`（异常语义不同）。

### 2.5 time

- `Duration/Time` 是 `go2cpp::time` 的别名（57-58）；单位常量 61-66。
- `Now/Since/Until/Sleep`（69-89）：单调时钟；`Sleep` 在 Fiber 里 park，不占线程。
- `NewTimer/After/AfterChan/NewTicker`：`Timer::Ptr` / `Chan<Time>`；`After` 返回 Timer 而非
  channel（Go 返回 `<-chan Time`）。
- `ToSteady/FromSteady`（91-100）：与 `steady_clock` 互转。

**失败路径**：`Timer.Stop` 与已触发回调竞态（不打断已在执行的栈）；ticker 未 Stop 泄漏。
**误用**：把 `After` 当 channel；用 wall clock 做超时。

### 2.6 io

- `ReadFull`（`include/go2cpp/go/io.go.hpp:83`）、`CopyBuffer`（105）、`WriteString`（117）。
- `ConnIO`（186）：把 `net::Conn` 适配成 `Reader/Writer/Closer`。
- 语义：`ReadFull` 读满或返回 `ErrUnexpectedEOF`；`Copy` 直到 EOF；`WriteString` 处理短写？
  看 `streams::WriteString` 实现（可能循环）。
- `ErrShortWrite` / `ErrNoProgress` 在 `error.go.hpp` 有哨兵。

**失败路径**：短读/短写/EOF/底层错误。**误用**：把 `Read` 的 `n>0 && err!=nil` 当失败丢弃数据
（Go 允许同时返回 n 和 err）。

### 2.7 net

- `Addr`（`include/go2cpp/go/net.go.hpp:66-88`）、`Conn`（102-123）、`Listener`（138-150）。
- `TCPConn::Read`（326-338）：deadline 过期立刻 `ErrDeadlineExceeded`；`recv` 返回 0 →
  `ErrEOF`；其它 errno 走 `MapSocketError`（216-232）。
- `TCPConn::Write`（326-338）：`send` 返回 `transferred >= 0` 就成功，**允许部分写**（Q05）。
- `Deadline`（176-213）：mutex 保护；`Remaining()` 返回 0 表示过期，`NoTimeout()` 是 max。
- `Listen/Dial`：TCP/UDP；UDP 的 `Accept` 等首个数据报并缓存（最小语义）。
- `SetDeadline(零值 Time)` 清除 deadline（`Deadline::Set`）。

**失败路径**：连接拒绝/重置/超时/关闭；`MapSocketError` 把 EAGAIN/ETIMEDOUT 统一成
`ErrDeadlineExceeded`，EBADF/ENOTCONN/EPIPE → `ErrClosed`。
**误用**：`Write` 不循环；`Read` 忽略 `n>0` 的 err；deadline 设过去时间以为会重置。

### 2.8 signal

- `Signal`（`include/go2cpp/go/os_signal.hpp:145-187`）：编号 + `String()`；`Interrupt/Kill` 常量。
- `SignalHub`（320-336）：watchers map、watched/ignored/armed 原子掩码、pipe fd、分发线程；
  `g_hub` 是进程级单例，**永不销毁**（避免静态析构期 UAF）。
- `InstallHandler`（381-415）：`sigaction` + `SA_RESTART`；handler 只做原子读 + `write` +
  `RestoreDefault` + `raise`。
- `DispatchLoop`（417-448）：阻塞 `read(pipe)`，锁内拷贝 watchers，锁外 `TrySend`。
- `NotifyList`（493-530）：空列表 = 所有可捕获信号（跳过 SIGKILL/SIGSTOP）；注册时
  `Notify` 覆盖 `Ignore`。
- `StopChannel`（532-558）：移除 channel；无监听者时 `DisarmLocked` 恢复默认。
- `IgnoreList`（560-585）：无监听者时吞掉。
- `Supported()`（290）：Windows false。

**状态机**：
```
未注册 ──Notify──► armed(watched bit) ──信号──► handler → pipe → TrySend
   │                     │
   │                     └─Stop/无 watcher──► RestoreDefault
   └─Ignore──► ignored bit（handler 吞掉）
```
**失败路径**：pipe 满丢字节；channel 满 `TrySend` 失败；SIGKILL/STOP 装不上；编号 >63 忽略。
**误用**：`Notify(SIGSEGV)` 覆盖 Fiber handler（Q03）；向已关闭 channel 发送以为会 panic；
以为 Notify 是并集就重复调用（Go 1.16+ 是并集，这里也是）。

### 2.9 crc32 / crypto/rand / flag

- `crc32`（`include/go2cpp/go/crc32.hpp:84-138`）：`Table/Hash/ByteSpan` 是 `go2cpp::hash` 的别名；
  `IEEE/Castagnoli/Koopman` 常量；`ChecksumIEEE`、`Checksum`、`Update`、`New`。
  任何「优化 crc32」都应落在 `include/go2cpp/hash/crc32.hpp`，不要在门面复制实现。
- `crypto/rand`（`include/go2cpp/go/crypto_rand.hpp:141-...`）：`Source`（141）、`OsSource`（160）、
  `BigInt`（185）、`BigResult`（272）；`Read/Int/Prime`；`ProbablyPrime` 是小素数试除 +
  Miller-Rabin（没有 Baillie-PSW）；`Prime` 最高两位置 1。
  **差异**：Go 1.26+ 的 `Prime` 忽略传入 reader，这里始终用传入 `Source`（保留可测试性）。
- `flag`（`include/go2cpp/go/flag.hpp:142-581`）：`ParseResult`、`Value`、`FlagSet::Parse/Set/PrintDefaults`；
  失败**不退出进程、不自动打印用法**（`-h` 除外）；`ParseDuration` 内联了一份，
  `tools/time.hpp` 的版本待统一。

**失败路径**：flag 非法值返回 `ParseResult` 由调用方处理；rand 的 `Source` 短读返回 error；
crc32 表不匹配导致校验失败。**误用**：把 flag 当 Go 的 `ExitOnError`；把 `Prime` 的
reader 参数当摆设。

### 2.10 与 Go 的不可对齐点（速查）

| Go 语义 | 本实现 | 影响 |
|---|---|---|
| nil channel 永久阻塞 | 返回 false | 不会挂死，但错误被吞 |
| send on closed panic | 返回 false | 静默丢数据 |
| close twice panic | 返回 false | 同上 |
| `net.Conn.Write` 全写或 error | 允许部分写 + nil error | 半帧 |
| `flag.Parse` 退出进程 | 返回 `ParseResult` | 调用方必须处理 |
| `WaitGroup.Wait` 任意 goroutine | 无调度器抛 `logic_error` | 裸线程崩 |
| context 回调异步？ | 同步（与 Go 相同） | 回调阻塞拖住取消 |
| `errors.Is` 纯身份 | 哨兵文本扩展 | 同文本哨兵可匹配 |
| `signal.Notify` 可捕获同步信号 | 会覆盖 Fiber SIGSEGV handler | 深栈崩 |
| `Prime(reader,...)` Go1.26 忽略 reader | 始终用 reader | 可测试性优先 |

---

## 附：本机自测入口（不引用历史数字）

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DGO2CPP_WITH_GO_API=ON
cmake --build build -j"$(nproc)"
ctest --test-dir build -R 'go_api_' --output-on-failure
```

单测：`go_api_net`（TCP/UDP/deadline/EOF/io）、`go_api_concurrency`（Mutex/WaitGroup/Once/
Chan/Select/Context/Timer/errors）、`go_api_stress`（并发/深错误链）、`go_api_crc32`（标准向量/
随机切分）、`go_api_crypto_rand`（随机性/Int 对拍/素性）、`go_api_flag`（四种赋值/非法值）、
`go_api_signal`（SIGINT/Stop/Ignore/多通道，仅 Unix）。

语义探针建议：`New("EOF")` vs `ErrEOF()`、`Write` 8MiB 的部分写、`context` 回调阻塞 2s、
`Notify(SIGSEGV)` 后深栈——都能在几分钟内复现，报告里写清编译选项与运行环境。
