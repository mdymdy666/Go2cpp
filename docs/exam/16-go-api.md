# Go2Cpp 理解力考试 · Go API 兼容层卷（16 go-api / 5 题）

> 覆盖：`error` 接口的 C++ 表达与 `Is` 的文本扩展、`context` 取消回调的执行线程、`signal` 与
> Fiber 的 SIGSEGV handler 冲突、`chan` 的 nil/closed 语义替换、`net.Conn.Write` 的部分写契约。
> 出题依据：`user/docs/16-go-api.md`、`docs/modules/go-api-compat.md`、`docs/modules/go-stdlib-gaps.md`、
> `docs/audit/`、`AI_COLLAB.md`，以及**当前工作树源码**。
> 难度分布：L1 = 0、L2 = 0、L3 = 3、L4 = 2。
> **本卷数字均需自测，文档数字仅作历史对照。** 本卷多数题是语义题，不涉及性能数字；
> 凡需要跑的，一律给 ctest 目标或探针条件。

---

## 一、错误与取消

### Q01 【L3 陷阱】`go::Is(go::New("EOF"), go::ErrEOF())` 返回 true 吗？文件头说「不做文本比较」，为什么哨兵又做？

- **痛点**：把 `ErrEOF()` 当成「文本等于 EOF 就算匹配」，于是用 `New("EOF")` 或第三方库的
  `"EOF"` 去 `Is`；或者反过来，以为所有错误都按文本比较，写出误匹配。
- **追查位置**：
  - `include/go2cpp/go/error.go.hpp:26-37`（文件头「语义边界」：`go::error` 是
    `ErrorPtr`（`shared_ptr<const Error>`），零值 `nullptr` 等价 `nil`；`errors.Is` 按错误图遍历，
    **不做文本比较**）
  - `include/go2cpp/go/error.go.hpp:103-122`（`SentinelError`：`Is(target)` 先比地址，再
    `dynamic_cast` 到 `SentinelError` 且 `m_message` 相同 → true；注释说明这是为了兼容
    streams/network 重复构造的 `"EOF"`）
  - `include/go2cpp/go/error.go.hpp:135-139`（`ErrEOF()` 返回静态 `SentinelError("EOF")`）
  - `include/go2cpp/tools/error.hpp:91-112`（`Error::Is` 默认按对象地址；自定义错误可覆写）
- **验收标准**：
  1. 说出 `go::Is(err, target)` 的匹配方向：遍历 `err` 的错误图，调用**源错误**的
     `Is(target)`；不是把两边文本拿出来比。
  2. 推导 `New("EOF")` vs `ErrEOF()`：`New` 返回普通 `Error`，默认 `Is` 只比地址 → **false**；
     两个不同实例的 `SentinelError("EOF")` 互相 `Is` → **true**（源是 Sentinel，且文本相同）。
  3. 指出文件头与实现的表面矛盾，以及为什么不冲突：文本扩展只发生在 `SentinelError` 这一层，
     普通错误仍按身份；所以「不同语义但同文本」的错误不会互相匹配（除非都用了哨兵）。
  4. 说清 `As<T>` 的约束：`T` 必须继承 `go2cpp::Error`，返回 `shared_ptr<const T>`；
     与 Go 的 `errors.As(err, &target)` 不同，这里不是写回指针。
  5. 说出 nil 语义：`ErrorString(nil)` 返回空串（Go 的 `nil.Error()` 会 panic）；
     `HasError(err)` 就是 `bool(err)`；`Result<T>` 默认「零值 + nil error」。
- **怎么自证**：`cmake --build .build-bento-wsl -j"$(nproc)" --target go_api_concurrency && ./.build-bento-wsl/go_api_concurrency`
  （errors 段覆盖 New/Wrap/Join/Is/As）。再写 10 行探针打印四个布尔：
  `Is(New("EOF"), ErrEOF())`、`Is(ErrEOF(), New("EOF"))`、`Is(ErrEOF(), ErrEOF())`、
  `Is(Wrap(ErrEOF(),"ctx"), ErrEOF())`；期望 `false/false/true/true`。把结果贴进报告，
  并解释第三、四个为什么不同。
- **常见错误答案**：「文本一样就 Is 为 true」「`Is` 是对称的」「`ErrEOF()` 是单例，所以任何
  `"EOF"` 都能匹配」——零分。

### Q02 【L4 陷阱】`context` 的取消回调在哪个线程执行？在回调里阻塞 2 秒会发生什么？

- **痛点**：把取消当成「异步通知」；在 `AddCallback` 里加锁/写日志/等 IO，结果 `cancel()` 本身
  被拖住，整棵子树的取消传播一起卡住。
- **追查位置**：
  - `include/go2cpp/concurrency/context.hpp:26-27`（明确写：`DoneSignal` 的回调在**通知线程**上
    同步执行，回调里不能做阻塞操作）
  - `src/concurrency/context.cpp:654-718`（`DoneSignal::Signal()`：锁内只搬运回调；
    锁外先 `notify_all()`，再执行 `internal_callbacks`，最后执行用户 `callbacks`；每个回调单独
    `try/catch` 吞异常）
  - `src/concurrency/context.cpp:1266-1288`（`Cancel()`：已完成直接返回；否则构造
    `CancellationWork` 并 `DrainCancellation`）
  - `src/concurrency/context.cpp:368-378`（`internal_callbacks` 先于 `callbacks` 的原因：
    `ContextRollback` 要在用户观察到取消前冻结 undo 记录）
- **验收标准**：
  1. 说出执行线程：谁调用 `cancel()`（或 timer 到期、父级取消），回调就在谁的线程上同步跑；
     没有专门的调度器线程，也不会自动切到 Fiber。
  2. 推导阻塞后果：一个用户回调睡 2 s，`cancel()` 就 2 s 后才返回；同一 `DoneSignal` 上排在后面的
     回调、以及父→子的后续取消传播都被推迟；等待 `Done()` 的线程在 `notify_all()` 时已被唤醒，
     所以「等待者」比「回调」先恢复。
  3. 说清顺序：`done=true` → `notify_all` → internal 回调 → 用户回调；internal 先于用户是
     `ContextRollback` 的契约，不能调换。
  4. 说清异常策略：单个回调抛异常被吞掉，不影响其余回调，也不改变「已完成」事实；所以回调里
     不能靠抛异常来中断取消。
  5. 说出正确做法：回调里只做 `Notify()`/置原子标志，把重活交给被唤醒方；不要在回调里
     拿可能被取消路径持有的锁（会死锁）。
- **怎么自证**：`./.build-bento-wsl/go_api_concurrency` 先过 context 取消/超时/Value 基线。
  再写探针：`auto [ctx, cancel] = WithCancel(Background()); ctx->Done().AddCallback([]{ sleep(2s); });`
  在 t0 调 `cancel()`，在另一个线程每 10 ms 打印 `ctx->Done().IsDone()`；期望 `IsDone()` 在
  `notify_all` 后立刻为 true，而 `cancel()` 到 2 s 才返回。把 `AddCallback` 换成
  `AddBeforeDoneCallback` 再跑，观察它是否在 `done=true` 之前执行（用 `IsDone()` 打印）。
- **常见错误答案**：「回调在后台线程跑，cancel 立即返回」「回调抛异常会中断取消」
  「`notify_all` 在回调之后，所以等待者要等回调」——零分。

---

## 二、信号与通道

### Q03 【L4 陷阱】`go::signal::Notify(c, SIGSEGV)` 会发生什么？为什么这里不能用 `signalfd`？

- **痛点**：把 `os/signal` 当成独立的信号工具，不知道 Fiber 的可增长栈依赖 `SIGSEGV` handler；
  或者反过来，觉得 `signalfd` 更现代、应该替换 `sigaction`。
- **追查位置**：
  - `include/go2cpp/go/os_signal.hpp:39-57`（为什么选 `sigaction`：`signalfd` 要求每个线程
    `pthread_sigmask(SIG_BLOCK)`，而进程里还有调度/timer/IO 线程，漏一个就会被默认处置杀进程）
  - `include/go2cpp/go/os_signal.hpp:390-420`（handler 只做三件事：读原子掩码、`write` 一字节到
    pipe、无监听者且未 Ignore 时 `RestoreDefault` + `raise`）
  - `include/go2cpp/go/os_signal.hpp:430-440`（分发线程 `read` pipe，锁内拷贝目标列表，锁外
    `TrySend`；满即丢）
  - `src/concurrency/fiber.cpp:448-459`（每线程 `sigaltstack`，给 growable handler 准备备用栈）
  - `src/concurrency/fiber.cpp:463-491`（安装 `SIGSEGV` handler：
    `SA_SIGINFO | SA_ONSTACK`，不加 `SA_NODEFER`，配合 TLS 重入标志）
  - `include/go2cpp/go/os_signal.hpp:219-220`（信号名表里包含 `SIGSEGV`，`Notify` 不会拒绝它）
- **验收标准**：
  1. 说出冲突：`go::signal::Notify(c, Signal(SIGSEGV))` 会 `sigaction` 覆盖 Fiber 安装的
     growable-stack handler；之后栈增长 fault 不再进 `growable_stack_segv_handler`，深栈直接
     真 SIGSEGV（或进入错误处置）。
  2. 说出第二个差异：`go/signal` 的 handler 没有 `SA_ONSTACK`，即使没覆盖，栈溢出时也拿不到
     备用栈；Fiber 的 handler 明确用了 `SA_ONSTACK` + `sigaltstack`。
  3. 说出 `signalfd` 方案为什么不成立：它只对「调用线程已屏蔽」的信号生效；Go2Cpp 有调度、
     timer、IO 多条线程，逐线程插 `pthread_sigmask` 的改动面大于收益；`sigaction` 是进程级，
     任何线程收到都进同一 handler。
  4. 说清 handler 的 async-signal-safe 边界：只允许原子读、`write`、`sigaction`、`raise`；
     不能 `malloc`/加锁/写 `std::string`/调 `TrySend`（`TrySend` 在分发线程做，不在 handler）。
  5. 说出 `SIGKILL`/`SIGSTOP` 不可捕获（`InstallHandler` 失败被静默跳过）、编号范围 1..63、
     channel 满即丢、向已关闭 channel 发送静默丢弃（Go 会 panic）。
- **怎么自证**：`cmake --build .build-bento-wsl -j"$(nproc)" --target go_api_signal && ./.build-bento-wsl/go_api_signal`
  （SIGINT 端到端、Stop、Ignore、多通道、Stop 恢复默认处置）。
  再写探针：启动一个需要深栈的 Fiber（例如递归到超过初始 4 KiB 提交窗口），先 `Notify` 监听
  `SIGSEGV`，再触发深栈；对比「未 Notify」时能正常增长、「Notify 后」是否直接崩溃。
  **这个探针会让进程挂掉，放在子进程里跑**；报告里贴退出码与 dmesg/`WTERMSIG`。
- **常见错误答案**：「`signalfd` 更安全，应该替换 sigaction」「`Notify(SIGSEGV)` 只是多一个
  监听者，不会影响 Fiber」「handler 里可以调 `TrySend`」——零分。

### Q04 【L3 陷阱】关闭的 channel 上 `Send`/`Close` 返回 false 而不是 panic——哪些 Go 语义被替换成了静默失败？

- **痛点**：把「不崩」当成「语义一致」；移植 Go 代码时依赖 panic 做断言/兜底，结果错误被吞，
  数据悄悄丢失。
- **追查位置**：
  - `include/go2cpp/go/chan.go.hpp:36-42`（语义边界：nil channel 在 Go 永久阻塞，这里
    `Send`/`Recv` 返回 false；关闭后 `Send` 返回 false，Go 会 panic）
  - `include/go2cpp/go/chan.go.hpp:94-100`（`Send`：`!m_channel` 直接 false；否则
    `m_channel->Send(...).Ok()`）
  - `include/go2cpp/go/chan.go.hpp:117-127`（`Recv`：无值返回 `{T{}, false}`；关闭后 ok=false）
  - `include/go2cpp/go/chan.go.hpp:171-177`（`Close`：幂等，重复关闭返回 false）
  - `include/go2cpp/go/chan.go.hpp:149-166`（`Recv(T&)` / `TryRecv` 的 false 同时表示
    「关闭」「无数据」「nil channel」，调用方无法区分）
- **验收标准**：
  1. 列出四处替换：nil channel 的 Send/Recv（Go 永久阻塞→false）、send on closed（panic→false）、
     close twice（panic→false）、receive on closed（返回零值+false，与 Go 一致）。
  2. 说出最危险的后果：`Send` 返回 false 既可能是「通道已关闭」也可能是「nil channel」，
     调用方若只 `if (!ch.Send(v)) continue;` 会把逻辑错误当背压/满队列处理，消息静默丢失。
  3. 说清 `Recv` 的缓冲语义：关闭后先把缓冲里的值按序取完（ok=true），取空才 ok=false；
     这一点与 Go 一致。
  4. 说出 `select` 的表达差异：C++ 用 `SelectCase` 列表 + `Select()`，`index` 对应下标；
     `DefaultCase()` 必须显式放；nil channel 的 case 在这里不会永久阻塞，而是不可选。
  5. 说出安全建议：需要 Go 的崩溃语义时，在门面外自己断言（`ch.Valid()`、`!ch.IsClosed()`），
     不要把 false 当唯一错误信号。
- **怎么自证**：`./.build-bento-wsl/go_api_concurrency`（Chan 生产者/消费者/close/select）。
  再写探针：`Chan<int> nil;` 调 `Send(1)`/`Recv()` 各打印返回值；`auto ch=Make(2); ch.Close();
  ch.Close(); ch.Send(1);` 打印三次结果；`Make(1); Send(7); Close(); Recv(); Recv();` 打印
  `(7,true)` 与 `(0,false)`。把输出与 Go 的同段程序（panic/阻塞）逐条对照。
- **常见错误答案**：「返回 false 就是 Go 的 panic，只是换成返回值」「关闭后 Recv 立刻返回
  false，不会先吐缓冲」「nil channel 的 select case 会永久阻塞」——零分。

---

## 三、网络语义

### Q05 【L3 陷阱】`net.Conn::Write` 在这里允许部分写，Go 的契约是什么？为什么这是最危险的不对齐点？

- **痛点**：直接照搬 Go 的 `conn.Write(buf)`，假设「返回 nil 就等于全写完」；在大包/对端慢/
  非阻塞 socket 上被截断，协议帧只发了一半，对端解析错位。
- **追查位置**：
  - `include/go2cpp/go/net.go.hpp:106-110`（`Conn::Write` 注释直接写「允许部分写」）
  - `include/go2cpp/go/net.go.hpp:326-338`（`TCPConn::Write`：`m_socket->send(...)` 返回
    `transferred >= 0` 就 `{n, {}}`，**n 可以小于 size 且 err 为空**）
  - `src/network/socket.cpp:756-760`（`send_impl`：一次 `::sendmsg`，`count >= 0` 就返回
    `{count, 0}`，不循环写满）
  - `include/go2cpp/go/net.go.hpp:90-96`（`IOResult{n, err}`，`Ok()` 只看 err）
  - `include/go2cpp/network/socket.hpp:34-38`（`SocketResult`：`transferred` 是实际字节数，可能小于请求）
- **验收标准**：
  1. 说出 Go 的契约：`io.Writer.Write` 若返回 `n < len(p)` 必须返回非 nil error；
     `net.Conn.Write` 的常见实现会循环到写完或出错，调用方通常可以「err == nil ⇒ 全写」。
  2. 说出这里的实现：一次 `sendmsg`，非阻塞 socket 上可能只写一部分；`transferred >= 0`
     就返回成功，err 为空。
  3. 推导后果：上层按「整帧写入」假设，半帧留在内核缓冲、剩余字节被丢弃/下次调用才写，
     对端 `FrameCodec` 看到长度字段与实际不符 → 协议错位/断连；小包回环测试几乎不会触发。
  4. 说出正确用法：循环直到 `n == size` 或 `err != nil`（注意 EAGAIN/ETIMEDOUT 的处理），
     或使用项目的 `WriteAll` 类工具；不能只看 `Ok()`。
  5. 说出 deadline 交互：`Write` 先检查 `m_write_deadline.Remaining()==0` → 立刻
     `ErrDeadlineExceeded`；部分写后剩余数据没有自动重试，deadline 也不会延长。
- **怎么自证**：`cmake --build .build-bento-wsl -j"$(nproc)" --target go_api_net && ./.build-bento-wsl/go_api_net`
  （小包回环，通常全写）。再写探针：服务端 accept 后不读、`SO_RCVBUF` 调小；客户端
  `SetWriteDeadline(now+1s)` 后一次 `Write` 8 MiB，打印 `r.n/r.err` 与 `r.n == size`：
  期望看到 `n < size && err == nil`（或 EAGAIN 映射的超时），证明部分写真实存在。
  把调用改成循环写，统计循环次数。
- **常见错误答案**：「`Write` 返回 Ok 就是全写完了」「部分写只可能发生在 UDP」
  「`SetWriteDeadline` 会自动把剩余数据写完」——零分。
