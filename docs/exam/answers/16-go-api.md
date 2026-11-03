# 16 go-api 答卷（5 题）

> 环境：WSL2 / 12 vCPU。`go_api_concurrency`、`go_api_net` 本轮均 `all scenarios passed`。

## Q01

**答**：`go::Is(err, target)` 遍历 `err` 的错误图，调用**源错误**的 `Is(target)`；不是两边取文本比较。`New("EOF")` 返回普通 `Error`，默认 `Is` 只比地址 → **false**；两个不同实例的 `SentinelError("EOF")` 互相 `Is` → **true**（源是 Sentinel 且文本相同）。文件头「不做文本比较」与实现不冲突：文本扩展只发生在 `SentinelError` 这一层，普通错误仍按身份；不同语义但同文本的错误不会互相匹配（除非都用了哨兵）。`As<T>` 要求 `T` 继承 `go2cpp::Error`，返回 `shared_ptr<const T>`（不是写回指针）。nil 语义：`ErrorString(nil)` 返回空串，`HasError(err)` 就是 `bool(err)`。

**代码证据**：
```text
include/go2cpp/go/error.go.hpp:26-29  ## 语义边界（必须知道）... errors.Is 按错误图遍历做**身份/自定义 Is** 匹配，不做文本比较；与 Go 一致。
include/go2cpp/go/error.go.hpp:103-106  @brief 带「同类即相等」语义的哨兵错误。... streams/network 层会重复构造 "EOF" 之类文本错误，因此哨兵额外实现
include/go2cpp/go/error.go.hpp:135-137  inline const error& ErrEOF() { static const error kValue = detail::MakeSentinel("EOF"); return kValue;
```

**自证**：`.build-bento-wsl/go_api_concurrency` → `[scenario] errors: New / Wrap / Is / As / Join`、`[go_api] all scenarios passed`。

**我没答出的**：没有单独打印 `Is(New("EOF"), ErrEOF())` 等四个布尔（题面建议的 10 行探针）。

## Q02

**答**：回调在**调用 `cancel()` 的线程**（或 timer 到期线程、父级取消线程）上同步执行；没有专门调度器线程，也不会自动切 Fiber。回调里睡 2 s → `cancel()` 2 s 后才返回，同一 `DoneSignal` 上排在后面的回调、以及父→子的后续取消传播都被推迟；但等待 `Done()` 的线程在 `notify_all()` 时已被唤醒，所以「等待者」比「回调」先恢复。顺序是 `done=true` → `notify_all` → `internal_callbacks` → 用户 `callbacks`；internal 先于用户是 `ContextRollback` 的契约（要在用户观察取消前冻结 undo）。每个回调单独 try/catch 吞异常，一个观察者抛异常不影响其余，也不改变「已完成」事实。正确做法：回调里只 `Notify()`/置原子标志，把重活交给被唤醒方；不要在回调里拿可能被取消路径持有的锁。

**代码证据**：
```text
include/go2cpp/concurrency/context.hpp:25-26  * `DoneSignal` 的回调在**通知线程**上同步执行，所以回调里不能做阻塞操作
src/concurrency/context.cpp:654-657  // 2. 置 done=true，并把 internal_callbacks 与 callbacks 整体搬进局部 vector...3. 释放锁后先 notify_all() 唤醒所有等待者。必须在回调之前
src/concurrency/context.cpp:1266-1269  void Cancel(...) { // 快路径：已完成时直接返回... }
src/concurrency/context.cpp:368-369  * internal_callbacks 先于 callbacks：ContextRollback 依赖它在用户观察到取消之前先冻结 undo
```

**自证**：`go_api_concurrency` 的 context 场景（WithCancel/Timeout/Value/DoneCase）通过；未写 sleep(2s) 回调探针。

**我没答出的**：没有实测「cancel 阻塞 2s、IsDone 立即 true」的时序。

## Q03

**答**：`Notify(c, SIGSEGV)` 会 `sigaction` **覆盖** Fiber 安装的 growable-stack handler；之后栈增长 fault 不再进 `growable_stack_segv_handler`，深栈直接真 SIGSEGV（或进入错误处置）。第二个差异：`go/signal` 的 handler 没有 `SA_ONSTACK`，即使没覆盖，栈溢出时也拿不到备用栈；Fiber 的 handler 明确用 `SA_SIGINFO | SA_ONSTACK` + `sigaltstack`。`signalfd` 不可行：它只对「调用线程已屏蔽」的信号生效，Go2Cpp 有调度/timer/IO 多条线程，逐线程插 `pthread_sigmask` 的改动面大于收益；`sigaction` 是进程级，任何线程收到都进同一 handler。handler 的 async-signal-safe 边界：只允许原子读、`write`、`sigaction`、`raise`；不能 malloc/加锁/写 `std::string`/调 `TrySend`（`TrySend` 在分发线程做）。`SIGKILL/SIGSTOP` 不可捕获，编号 1..63，channel 满即丢。

**代码证据**：
```text
include/go2cpp/go/os_signal.hpp:39-42  ## 为什么用 sigaction 而不是 signalfd ... `signalfd` 的语义要求调用线程先 `pthread_sigmask(SIG_BLOCK)`
include/go2cpp/go/os_signal.hpp:219-220  #if defined(SIGSEGV) case SIGSEGV: return "segmentation fault";
src/concurrency/fiber.cpp:463  bool install_growable_stack_handler() noexcept {
```

**自证**：`go_api_signal` 未跑（需构建 target）；代码路径核对。

**我没答出的**：没有在子进程里跑「Notify(SIGSEGV) 后深栈崩溃」的探针。

## Q04

**答**：四处替换：nil channel 的 Send/Recv（Go 永久阻塞→false）、send on closed（panic→false）、close twice（panic→false）、receive on closed（返回零值+false，与 Go 一致）。最危险：`Send` 返回 false 既可能是「通道已关闭」也可能是「nil channel」，调用方若 `if (!ch.Send(v)) continue;` 会把逻辑错误当背压/满队列，消息静默丢失。`Recv` 缓冲语义：关闭后先把缓冲里的值按序取完（ok=true），取空才 ok=false（与 Go 一致）。`select` 用 `SelectCase` 列表 + `Select()`，`index` 对应下标，`DefaultCase()` 必须显式放；nil channel 的 case 在这里不会永久阻塞而是不可选。安全建议：需要 Go 的崩溃语义时在门面外自己断言（`ch.Valid()`、`!ch.IsClosed()`），不要把 false 当唯一错误信号。

**代码证据**：
```text
include/go2cpp/go/chan.go.hpp:37-39  ## 语义边界 - Go 的 nil channel 永久阻塞；本封装的默认构造 Chan 是 nil，Send/Recv 直接返回 false，不会挂死
include/go2cpp/go/chan.go.hpp:94-97  /** @brief 对应 `ch <- v`... */ bool Send(T value) { if (!m_channel) return false;
include/go2cpp/go/chan.go.hpp:171-174  /** @brief 对应 close(ch)；幂等，重复关闭返回 false。 */ bool Close() { if (!m_channel) return false;
```

**自证**：`go_api_concurrency` 的 `chan: producer/consumer + close + select default` 通过。

**我没答出的**：没有写 nil/closed 的返回值探针并与 Go 逐条对照。

## Q05

**答**：Go 契约：`io.Writer.Write` 若返回 `n < len(p)` 必须返回非 nil error；`net.Conn.Write` 常见实现会循环到写完或出错，调用方通常可「err == nil ⇒ 全写」。这里的实现：一次 `::sendmsg`，`count >= 0` 就返回 `{count, 0}`——**n 可以小于 size 且 err 为空**；上层若按「整帧写入」假设，半帧留在内核缓冲、剩余字节丢弃/下次才写，对端 `FrameCodec` 看到长度字段与实际不符 → 协议错位/断连；小包回环几乎不触发。正确用法：循环直到 `n == size` 或 `err != nil`（处理 EAGAIN/ETIMEDOUT），或用项目 `WriteAll` 类工具，不能只看 `Ok()`。deadline：`Write` 先检查 `m_write_deadline.Remaining()==0` → 立刻 `ErrDeadlineExceeded`；部分写后剩余数据不会自动重试，deadline 也不会延长。

**代码证据**：
```text
include/go2cpp/go/net.go.hpp:106-109  /** @brief 对应 conn.Read(p)... */ virtual IOResult Read(...); /** @brief 对应 conn.Write(p)；允许部分写。 */ virtual IOResult Write(...);
include/go2cpp/go/net.go.hpp:326-329  IOResult Write(const void* buffer, std::size_t size) override { if (!m_socket || !m_socket->valid()) return {0, ErrClosed()};
src/network/socket.cpp:755-759  for (;;) { ... const ssize_t count = ::sendmsg(fd, message, flags | MSG_NOSIGNAL); if (count >= 0) return {count, 0};
```

**自证**：`go_api_net` 的小包回环场景 `all scenarios passed`（回环通常全写，不触发部分写）。

**我没答出的**：没有把 `SO_RCVBUF` 调小、8 MiB 一次 Write 复现 `n < size && err == nil`。
