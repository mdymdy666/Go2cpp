# Go 接口兼容层（JinShan relay 替换映射）

> 模块：`include/go2cpp/go/**` + `tests/go_api/**`
> 状态：新增（不修改既有模块）
> 构建开关：`-DGO2CPP_WITH_GO_API=ON`（默认 OFF）

---

## 0. 结论先行

1. **JinShan relay 的 Go 依赖是「小而集中」的**：`net`（TCP 监听/拨号/Conn）、
   `sync`（RWMutex/Mutex/WaitGroup）、`channel`（有界 128）、`time`（30 s / 90 s / 10 s
   超时与单调时钟）、`io`（`io.CopyBuffer` 字节泵）。这五类占全部调用量的 90% 以上。
2. **这五类已全部有 Go 风格封装**，可直接把 relay 的 Go 代码逐函数替换；
   封装层是**薄包装**，底层仍是 Go2Cpp 已有的 Channel/Context/Socket/TimerService，
   不引入第二套运行时。
3. **需要补的只有外围能力**：`os/signal`（SIGINT/SIGTERM）、`hash/crc32`、
   `crypto/rand`、`subtle.ConstantTimeCompare`、`flag`。这些不影响主链路
   （RDRL/RDCH 帧编解码、配对、字节转发），但影响「可独立运行的 relay 进程」。
4. **验证已落地**：3 个测试目标（网络/并发/压力），覆盖 TCP/UDP 回环、
   deadline、EOF、`io.Copy`、Mutex/RWMutex/WaitGroup/Once/Cond、Channel 生产者消费者、
   select、Context、Timer/Ticker/After、errors 图遍历；压力测试含 8 客户端 × 30 轮
   随机 TCP、4×20000 Channel、500 个 Context deadline、200 个 After + 10 个 Ticker。

---

## 1. JinShan relay 的 Go 接口盘点

### 1.1 依据

| 文档 | 引用内容 |
|---|---|
| `JinShanProject/docs/05-detailed/protocol.md` | §3.1 帧格式（小端 + CRC32 IEEE）、§3.3 拆包/粘包与半帧 30 s（单调时钟）、§4.6 双向 `copy`、§7.2 `crypto/rand` + 常量时间比较、§9.3 `io.Writer`、§10.1 `io.CopyBuffer` 32 KiB + 256 KiB 队列 + 30 s 写 deadline、§10.2 有界 channel 128、§11.1 `CloseWrite` 半关闭、§11.2 SIGINT/SIGTERM |
| `JinShanProject/docs/05-detailed/concurrency.md` | §1.2 relay G0–G5（accept 循环、每连接读/写 goroutine、DATA pump、过期扫描、信号处理）、§5.1 连接生命周期、§5.2 `sync.RWMutex`（sessionTable）/`sync.Mutex`（dataHub）/channel 128、§5.3 关闭与 `WaitGroup.Wait` |
| `JinShanProject/docs/05-detailed/error-codes.md` | §2 wire 状态码 0–14、§3–§9 统一错误码与重试策略（R0/R1/R2）、日志三要素（code/err_name/session_id）、禁止吞异常 |
| `JinShanProject/docs/05-detailed/sequences.md` | 四阶段配对时序、关闭时序 |

> 说明：JinShan 仓库是**设计文档仓库**，relay 的 `.go` 源码不在当前 checkout 中
> （`find . -name '*.go'` 为 0）。因此本章的 API 清单来自上述文档对实现的约束，
> 以及 Go 标准库在同类 relay 中的固定写法；每一条都标注了文档依据，
> 没有「凭印象」补出来的 API。

### 1.2 标准库使用清单（按使用频率）

| 排名 | 标准库 | 典型调用 | 文档依据 | 频率 |
|---|---|---|---|---|
| 1 | `net` | `Listen` / `Accept` / `Dial` / `Conn.Read` / `Write` / `Close` / `CloseWrite` / `SetDeadline` / `SetReadDeadline` / `SetWriteDeadline` / `LocalAddr` / `RemoteAddr` | protocol §3.3/§10.1/§11.1；concurrency §1.2 G0–G3、§5.1 | 极高 |
| 2 | `time` | `Now`/`Since`/`After`/`NewTicker`/`Sleep`/`Duration` 常量；单调时钟 | protocol §3.3（MonotonicNow）、§5.4（30 s 心跳）、§6.5 超时总表；concurrency §1.2 G4 | 极高 |
| 3 | `sync` | `RWMutex`（sessionTable）、`Mutex`（dataHub）、`WaitGroup`（关闭） | concurrency §5.2、§5.3、§2 MOD-14/16 | 高 |
| 4 | `io` | `CopyBuffer`（DATA pump）、`Copy`（CONTROL 双向）、`ReadFull`（帧读取）、`Writer`/`Reader` | protocol §4.6、§10.1；concurrency §1.2 G3 | 高 |
| 5 | 语言级 `chan`/`select` | 有界 channel（容量 128）、`select` 超时/取消、非阻塞 `default` | protocol §10.2；concurrency §5.2、§5.3 | 高 |
| 6 | `context` | `WithCancel`/`WithTimeout`/`Done`/`Err`；连接关闭与请求取消传播 | 文档未显式写 `context.`（Go 侧实现惯例，§11 关闭语义、§6.5 超时）；封装层按 Go 惯用补齐 | 中 |
| 7 | `errors` | `New`、`fmt.Errorf("%w")`、`Is`/`As`、哨兵错误；错误不吞、分类重试 | error-codes §0/§2/§3–§9、§13；protocol §12 | 中 |
| 8 | `os`/`os/signal` | `signal.Notify(SIGINT, SIGTERM)`、`os.Exit` | protocol §11.2；concurrency §1.2 G5 | 低（每进程 1 次） |
| 9 | `encoding/binary` | `binary.LittleEndian.PutUint32/Uint16` | protocol §3.1（小端） | 中（帧编解码） |
| 10 | `hash/crc32` | `crc32.IEEE` / `crc32.ChecksumIEEE` | protocol §3.1/§5.2 | 中（每帧） |
| 11 | `crypto/rand` + `crypto/subtle` | 8 字节 token、`ConstantTimeCompare` | protocol §7.2 | 低（每 token） |
| 12 | `bufio` | `bufio.NewReader/Writer`（帧读取、粘包缓冲） | protocol §3.3 | 中 |
| 13 | `flag` | `-max-clients`、`-buffer`、`-write-timeout`、`-data-timeout` | protocol §10.1、§6.5 | 低（启动 1 次） |
| 14 | `log`/`fmt` | 结构化日志、`key_id` 脱敏 | protocol §13、error-codes §0 | 高（但非接口替换重点） |

### 1.3 「必须支持」API 清单（按频率）

```text
P0（缺一不可，主链路）
  net.Listen / net.Listener.Accept / net.Listener.Close
  net.Conn.Read / Write / Close / CloseWrite
  net.Conn.SetReadDeadline / SetWriteDeadline
  net.Dial（tcp/udp）
  io.CopyBuffer / io.Copy / io.ReadFull
  time.Now / time.Since / time.After / time.NewTicker / time.Sleep
  sync.RWMutex / sync.Mutex / sync.WaitGroup
  make(chan T, 128) / ch <- / <-ch / close / select（含 default）
  errors.New / fmt.Errorf("%w") / errors.Is / errors.As

P1（进程级，必须补）
  os/signal.Notify（SIGINT/SIGTERM）
  encoding/binary.LittleEndian
  hash/crc32.IEEE
  crypto/rand.Read / subtle.ConstantTimeCompare

P2（可选，relay 可不用）
  flag / bufio / log / context.WithTimeout
  net/http、io/fs、sync.Map、sync.Pool
```

---

## 2. 映射表

状态说明：**已有** = Go2Cpp 现有实现，封装层直接转发；**新增** = 本任务新增的
Go 风格封装；**缺失** = 当前没有对应能力（需要后续补，或明确不用）。

### 2.1 net

| Go API | Go2Cpp 封装（`go2cpp::go::net`） | 底层 | 状态 |
|---|---|---|---|
| `net.Listen("tcp", addr)` | `Listen(network, address)` → `ListenResult` | `Socket::CreateTCP` + `bind` + `listen` | 新增 |
| `net.Listen("udp", addr)` | `Listen(network, address)` → `UDPListener` | `Socket::CreateUDP` + `bind` | 新增 |
| `net.Dial` / `DialTimeout` | `Dial` / `DialTimeout` | `Socket::CreateTCP/UDP` + `connect` | 新增 |
| `(*net.TCPListener).Accept` | `Listener::Accept()` | `Socket::accept` | 新增 |
| `(*net.TCPListener).Close` | `Listener::Close()` | `Socket::close` | 新增 |
| `(*net.TCPListener).Addr` | `Listener::Addr()` | `Socket::local_address` | 新增 |
| `net.Addr` | `Addr`（`Network`/`String`） | `go2cpp::net::Address` | 新增 |
| `net.Conn.Read` | `Conn::Read` → `IOResult` | `Socket::recv` | 新增 |
| `net.Conn.Write` | `Conn::Write` → `IOResult` | `Socket::send` | 新增 |
| `net.Conn.Close` | `Conn::Close` | `Socket::close` | 新增 |
| `net.Conn.CloseWrite` | `TCPConn::CloseWrite` | `Socket::shutdown(SHUT_WR)` | 新增 |
| `net.Conn.CloseRead` | `TCPConn::CloseRead` | `Socket::shutdown(SHUT_RD)` | 新增 |
| `net.Conn.LocalAddr/RemoteAddr` | `Conn::LocalAddr/RemoteAddr` | `Socket::local_address/remote_address` | 新增 |
| `net.Conn.SetDeadline` | `Conn::SetDeadline(Time)` | 保存 deadline，读/写时换算成 `Socket` 超时 | 新增 |
| `net.Conn.SetReadDeadline` | `Conn::SetReadDeadline` | 同上（读方向） | 新增 |
| `net.Conn.SetWriteDeadline` | `Conn::SetWriteDeadline` | 同上（写方向） | 新增 |
| `(*net.UDPConn).ReadFrom/WriteTo` | `UDPConn`（Accept 后按 peer 收发） | `Socket::recv_from/send_to` | 新增（最小语义） |
| `net.ResolveTCPAddr` | 未单独封装 | `go2cpp::net::ResolveTCPAddr` | 已有（底层） |
| `net.SplitHostPort` | `detail::SplitHostPort`（内部） | 自实现（含 `[::1]:port`） | 新增（内部） |
| `net.LookupHost` / DNS 解析 | 无独立封装 | `getaddrinfo`（Resolve* 内部） | 缺失 |
| `net/http` | — | — | 不适用（relay 不做 HTTP） |

### 2.2 sync

| Go API | Go2Cpp 封装（`go2cpp::go`） | 底层 | 状态 |
|---|---|---|---|
| `sync.Mutex` | `Mutex`（`Lock`/`Unlock`/`TryLock`/`LockCtx`） | `go2cpp::sync::Mutex` | 新增 |
| `sync.RWMutex` | `RWMutex`（`RLock`/`RUnlock`/`Lock`/`Unlock`/`Try*`） | `go2cpp::sync::RWMutex` | 新增 |
| `sync.WaitGroup` | `WaitGroup`（`Add`/`Done`/`Wait`/`WaitCtx`） | `go2cpp::sync::WaitGroup` | 新增 |
| `sync.Once` | `Once`（`Do`/`DoNoThrow`） | `std::call_once` | 新增 |
| `sync.Cond` | `Cond`（`Wait`/`Signal`/`Broadcast`） | `go2cpp::sync::ConditionVariable` | 新增 |
| `sync/atomic` | 未封装（直接用 `std::atomic`） | C++ 标准库 | 已有 |
| `sync.Map` | — | — | 缺失（relay 用 RWMutex + map） |
| `sync.Pool` | — | — | 缺失（非必需） |

### 2.3 chan / select（语言级）

| Go | Go2Cpp 封装（`go2cpp::go`） | 底层 | 状态 |
|---|---|---|---|
| `make(chan T, n)` | `Chan<T>::Make(n)` | `go2cpp::Channel<T>` | 新增 |
| `ch <- v` / `<-ch` | `Chan::Send` / `Recv` / `SendCtx` / `RecvCtx` | `Channel::Send/Recv` | 新增 |
| `close(ch)` | `Chan::Close` | `Channel::Close` | 新增 |
| `len(ch)` / `cap(ch)` | `Len()` / `Cap()` | `Channel::Len/Capacity` | 新增 |
| 非阻塞发送/接收 | `TrySend` / `TryRecv` | `Channel::TrySend/TryRecv` | 新增 |
| `select { ... }` | `Select({...})` | `go2cpp::Select` | 新增 |
| `default:` | `DefaultCase()` | `go2cpp::DefaultCase` | 新增 |
| `case <-ctx.Done()` | `DoneCase(ctx)` | `Context::Done` | 新增 |
| `case <-time.After(d)` | `AfterCase(d)` | `core::TimerService` | 新增 |
| `for v := range ch` | `while (ch.Recv(v))` | — | 语法差异 |

### 2.4 context

| Go API | Go2Cpp 封装（`go2cpp::go`） | 底层 | 状态 |
|---|---|---|---|
| `context.Background()` | `Background()` | `go2cpp::Context::Background` | 已有（转发） |
| `context.TODO()` | `TODO()` | `go2cpp::Context::TODO` | 已有（转发） |
| `context.WithCancel` | `WithCancel` → `CancelContext` | `Context::WithCancel` | 新增（返回形态） |
| `context.WithTimeout` | `WithTimeout` | `Context::WithTimeout` | 新增 |
| `context.WithDeadline` | `WithDeadline` | `Context::WithDeadline` | 新增 |
| `context.WithValue` | `WithValue`（字符串键 / `ContextKey<T>`） | `Context::WithValue` | 新增 |
| `ctx.Done()` | `Done(ctx)` / `WaitDone(ctx)` | `DoneSignal` | 新增 |
| `ctx.Err()` | `Err(ctx)` | `Context::Err` | 新增 |
| `ctx.Cause()` | `Cause(ctx)` | `Context::Cause` | 新增 |
| `ctx.Deadline()` | `Deadline(ctx)` | `Context::Deadline` | 新增 |
| `ctx.Value(key)` | `Value(ctx, key)` | `Context::Value` | 新增 |
| `context.Canceled` | `ErrCanceled()` | `CanceledError()` | 新增（哨兵） |

### 2.5 errors

| Go API | Go2Cpp 封装（`go2cpp::go`） | 底层 | 状态 |
|---|---|---|---|
| `errors.New` | `New` | `NewError` | 已有（底层）+ 新增别名 |
| `fmt.Errorf("%w", err)` | `Wrap(err, msg)` | `WrappedError` | 已有 |
| `errors.Is` | `Is` | 错误图遍历（身份/自定义 `Is`） | 已有 |
| `errors.As` | `As<T>` | `dynamic_pointer_cast` 遍历 | 已有 |
| `errors.Join` | `Join` | `JoinError` | 已有 |
| `errors.Unwrap` | `Unwrap` | `Error::Unwrap` | 已有 |
| `err.Error()` | `ErrorString` | `ErrorMessage` | 已有 |
| `io.EOF` / `io.ErrUnexpectedEOF` | `ErrEOF()` / `ErrUnexpectedEOF()` | 自定义 `SentinelError` | 新增 |
| `net.ErrClosed` / `os.ErrClosed` | `ErrClosed()` | 同上 | 新增 |
| `os.ErrDeadlineExceeded` | `ErrDeadlineExceeded()` / `ErrTimeout()` | 同上 | 新增 |
| `(T, error)` 返回值 | `Result<T>` / `Status` | — | 新增 |

### 2.6 time

| Go API | Go2Cpp 封装（`go2cpp::go`） | 底层 | 状态 |
|---|---|---|---|
| `time.Duration` / `Time` / 单位常量 | `Duration` / `Time` / `Second` … | `go2cpp::time` | 已有（转发） |
| `time.Now` / `Since` / `Until` / `Sleep` | `Now` / `Since` / `Until` / `Sleep` | `go2cpp::time` | 已有（转发） |
| `time.After(d)` | `After(d)`（`shared_ptr<Timer>`）/ `AfterChan(d)` | `core::TimerService::ScheduleAfter` | 新增 |
| `time.NewTimer` | `NewTimer(d)` / `Timer` | `core::Timer` | 新增 |
| `Timer.Stop/Reset/C` | `Timer::Stop/Reset/C` | `core::Timer::Cancel/Reset` | 新增 |
| `time.AfterFunc` | `AfterFunc` | `TimerService::ScheduleAfter` | 新增 |
| `time.NewTicker` | `NewTicker` / `Ticker` | `ScheduleEvery` | 新增 |
| `Ticker.Stop/Reset/C` | `Ticker::Stop/Reset/C` | `core::Timer::Cancel/Refresh` | 新增 |
| `time.Tick` | `Tick` | `ScheduleEvery` | 新增 |
| `time.Parse/Format/LoadLocation` | — | — | 缺失（relay 不用） |

### 2.7 io

| Go API | Go2Cpp 封装（`go2cpp::go::io`） | 底层 | 状态 |
|---|---|---|---|
| `io.Reader` / `Writer` / `Closer` | `Reader` / `Writer` / `Closer` | `go2cpp::streams` | 已有（转发） |
| `io.Copy` | `Copy` | `streams::Copy` | 已有 |
| `io.CopyBuffer` | `CopyBuffer` | `streams::CopyBuffer` | 已有 |
| `io.CopyN` | `CopyN` | `streams::CopyN` | 已有 |
| `io.ReadFull` / `ReadAtLeast` / `ReadAll` | `ReadFull` / `ReadAtLeast` / `ReadAll` | `streams` | 已有 |
| `io.WriteString` | `WriteString` | `streams::WriteString` | 已有 |
| `io.LimitReader` / `MultiReader` / `TeeReader` / `Discard` | 同名 | `streams` | 已有 |
| `net.Conn` 当 `io.Reader`/`Writer` | `ConnReader` / `ConnWriter` / `ConnIO` | `net::Conn` 适配器 | 新增 |
| `io.Pipe` | — | 可用 `Chan<Bytes>` 模拟 | 缺失（非必需） |
| `bufio.Reader/Writer` | — | `go2cpp::bufio` | 已有（底层） |

### 2.8 其他

| Go API | Go2Cpp 对应 | 状态 |
|---|---|---|
| `os/signal.Notify(SIGINT/SIGTERM)` | 无封装（需 `std::signal` 或 signalfd） | **缺失**（建议补 `go/os.go.hpp`） |
| `encoding/binary.LittleEndian` | `go2cpp::endian` / `myendian.hpp` | 已有（命名不同） |
| `hash/crc32.IEEE` | 无公共封装（`app/resource` 内部有 CRC） | **缺失** |
| `crypto/rand.Read` | 无公共封装（可用 `getrandom`） | **缺失** |
| `crypto/subtle.ConstantTimeCompare` | 无 | **缺失** |
| `flag` | `go2cpp::config`（INI） | 已有但非 Go 风格 |
| `log` | `go2cpp::logging` | 已有 |
| `net/http` | `go2cpp::net::HttpServer` | 已有（relay 不需要） |

---

## 3. JinShan 覆盖情况

### 3.1 可以直接替换（封装已覆盖）

| relay 代码 | 替换为 |
|---|---|
| `ln, err := net.Listen("tcp", addr)` | `auto ln = go::net::Listen("tcp", addr);` |
| `conn, err := ln.Accept()` | `auto accepted = ln.listener->Accept();` |
| `conn.SetReadDeadline(...)` | `conn->SetReadDeadline(go::Now().Add(...));` |
| `n, err := conn.Read(buf)` | `auto r = conn->Read(buf, sizeof(buf));` |
| `_, err = conn.Write(buf[:n])` | `(void)conn->Write(buf, r.n);` |
| `conn.CloseWrite()` | `static_cast<go::net::TCPConn&>(*conn).CloseWrite();`（或直接持有 `TCPConn`） |
| `io.CopyBuffer(dst, src, buf)` | `go::io::CopyBuffer(dst_io, src_io, buf, sizeof(buf));` |
| `io.Copy(dst, src)` | `go::io::Copy(dst_io, src_io);` |
| `io.ReadFull(r, frame)` | `go::io::ReadFull(reader, frame, len);` |
| `var mu sync.RWMutex` | `go::RWMutex mu;` |
| `var wg sync.WaitGroup` | `go::WaitGroup wg;` |
| `wg.Wait()` | `wg.Wait();` |
| `ch := make(chan []byte, 128)` | `auto ch = go::Chan<Bytes>::Make(128);` |
| `select { case ch <- v: case <-ctx.Done(): }` | `go::Select({ch.SendCase(v), go::DoneCase(ctx)});` |
| `time.After(30 * time.Second)` | `go::AfterChan(30 * go::Second)` 或 `go::AfterCase(30 * go::Second)` |
| `ticker := time.NewTicker(time.Second)` | `auto ticker = go::NewTicker(go::Second);` |
| `errors.Is(err, io.EOF)` | `go::Is(err, go::ErrEOF())` |
| `fmt.Errorf("read frame: %w", err)` | `go::Wrap(err, "read frame")` |

### 3.2 需要补（当前封装未覆盖）

| 能力 | 影响 | 建议 |
|---|---|---|
| `os/signal` | relay 无法优雅处理 SIGINT/SIGTERM（§11.2 关闭顺序第 1 步） | 新增 `include/go2cpp/go/os.go.hpp`：`signal::Notify`（signalfd/自管道）+ `NotifyContext` |
| `hash/crc32` | RDRL/RDCH 帧校验无法直接调用 | 新增 `go/crc32.hpp` 或复用现有实现并暴露 `ChecksumIEEE` |
| `crypto/rand` + `subtle` | token 生成与常量时间比较 | 新增 `go/crypto.go.hpp`（`getrandom` + `ConstantTimeCompare`） |
| `flag` | 命令行参数（`-max-clients` 等） | 可用现有 `config` 替代，或后续封装 |
| UDP `PacketConn` 完整语义 | 当前 `UDPListener::Accept` 是「首包 + 固定 peer」，不适合多 peer 广播场景 | relay 不需要；如需多 peer，补 `ListenPacket` + `ReadFrom/WriteTo` |

### 3.3 明确不需要

`net/http`（relay 不下载）、`io/fs`、`sync.Map`/`sync.Pool`、`time.Parse/Format`、
`database/sql`、`encoding/json`（RDCH 是二进制帧）。

---

## 4. 使用示例

### 4.1 监听 + accept + 每连接读写（Go → C++）

```go
ln, err := net.Listen("tcp", ":9000")
if err != nil { log.Fatal(err) }
for {
    conn, err := ln.Accept()
    if err != nil { return }
    go handle(conn)
}

func handle(conn net.Conn) {
    defer conn.Close()
    conn.SetReadDeadline(time.Now().Add(10 * time.Second))
    buf := make([]byte, 65536)
    n, err := conn.Read(buf)
    if err != nil { return }
    conn.Write(buf[:n])
}
```

```cpp
auto listen = go::net::Listen("tcp", ":9000");
if (!listen.Ok()) {
    // 生产代码接 go2cpp::logging；这里只演示错误分支。
    std::fprintf(stderr, "listen: %s\n", go::ErrorString(listen.err).c_str());
    return;
}
for (;;) {
    auto accepted = listen.listener->Accept();
    if (!accepted.first) {
        return;  // listener 关闭
    }
    auto conn = accepted.first;
    std::thread([conn] {
        (void)conn->SetReadDeadline(go::Now().Add(10 * go::Second));
        std::vector<char> buffer(65536);
        const auto read = conn->Read(buffer.data(), buffer.size());
        if (read.err) {
            return;
        }
        (void)conn->Write(buffer.data(), read.n);
        (void)conn->Close();
    }).detach();
}
```

### 4.2 DATA pump：`io.CopyBuffer` + 写 deadline + `CloseWrite`

```go
func pump(dst, src net.Conn) {
    defer dst.CloseWrite()
    buf := make([]byte, 32*1024)
    dst.SetWriteDeadline(time.Now().Add(30 * time.Second))
    io.CopyBuffer(dst, src, buf)
}
```

```cpp
void Pump(go::net::Conn& dst, go::net::Conn& src) {
    go::io::ConnIO dst_io(dst), src_io(src);  // 适配器不拥有连接
    std::vector<char> buffer(32 * 1024);
    (void)dst.SetWriteDeadline(go::Now().Add(30 * go::Second));
    (void)go::io::CopyBuffer(dst_io, src_io, buffer.data(), buffer.size());
    if (auto* tcp = dynamic_cast<go::net::TCPConn*>(&dst)) {
        (void)tcp->CloseWrite();
    }
}
```

### 4.3 sessionTable：`sync.RWMutex`

```go
var sessions = struct {
    sync.RWMutex
    m map[string]*Session
}{m: map[string]*Session{}}

func lookup(key string) (*Session, bool) {
    sessions.RLock()
    defer sessions.RUnlock()
    s, ok := sessions.m[key]
    return s, ok
}
```

```cpp
class SessionTable {
public:
    std::optional<std::shared_ptr<Session>> Lookup(const std::string& key) {
        mu_.RLock();
        auto it = map_.find(key);
        std::optional<std::shared_ptr<Session>> result;
        if (it != map_.end()) {
            result = it->second;
        }
        mu_.RUnlock();  // C++ 没有 defer；用 RAII 守卫可避免漏解锁
        return result;
    }

private:
    go::RWMutex mu_;
    std::unordered_map<std::string, std::shared_ptr<Session>> map_;
};
```

### 4.4 CONTROL 写 goroutine：有界 channel + `select`

```go
func (c *controlConn) sendLoop() {
    for {
        select {
        case frame := <-c.out:      // 容量 128
            c.conn.SetWriteDeadline(time.Now().Add(30 * time.Second))
            if _, err := c.conn.Write(frame); err != nil { return }
        case <-c.done:
            return
        }
    }
}
```

```cpp
void ControlConn::SendLoop() {
    for (;;) {
        auto result = go::Select({out_.RecvCase(), go::DoneCase(done_)});
        if (!result.selected || result.index != 0) {
            return;  // done 或取消
        }
        auto frame = result.Value<Bytes>().value_or(Bytes{});
        (void)conn_->SetWriteDeadline(go::Now().Add(30 * go::Second));
        if (!conn_->Write(frame.data(), frame.size()).Ok()) {
            return;
        }
    }
}
```

### 4.5 超时与取消：`context.WithTimeout`

```go
ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
defer cancel()
select {
case <-ctx.Done():
    return ctx.Err()
case result := <-work:
    return result
}
```

```cpp
auto timed = go::WithTimeout(go::Background(), 10 * go::Second);
auto result = go::Select({work.RecvCase(), go::DoneCase(timed.context)});
if (result.index != 0) {
    return go::Err(timed.context);
}
return result.Value<Result>().value_or(Result{});
```

### 4.6 错误分类：`errors.Is` / `errors.As`

```go
if errors.Is(err, io.EOF) { return }
var opErr *net.OpError
if errors.As(err, &opErr) { log.Warn(opErr.Error()) }
```

```cpp
if (go::Is(err, go::ErrEOF())) {
    return;
}
if (auto op = go::As<NetOpError>(err)) {
    log::Warn(go::ErrorString(err));
}
```

---

## 5. 构建与测试

```bash
# 配置（Bento standalone 构建，复用 socket_bufio runtime）
cmake -S . -B .build-bento-wsl -DGO2CPP_WITH_GO_API=ON

# 编译三个测试目标
ninja -C .build-bento-wsl go_api_net go_api_concurrency go_api_stress

# 运行
ctest --test-dir .build-bento-wsl -R 'go_api_' --output-on-failure
```

| 目标 | 覆盖 |
|---|---|
| `go_api_net` | TCP 回显、deadline 超时、EOF、Close 幂等、UDP 回环、`io.Copy`/`ReadFull`/`WriteString` |
| `go_api_concurrency` | Mutex/WaitGroup/Once/Cond/RWMutex、Chan 生产者消费者/close/select、Context 取消/超时/Value、Timer/Ticker/After、errors |
| `go_api_stress` | 8 客户端 × 30 轮随机 TCP、4×20000 Channel、8×20000 Mutex、500 Context deadline、200 After + 10 Ticker、1000 层 errors |

---

## 6. 边界与差异（必须知道）

| 差异 | 说明 | 影响 |
|---|---|---|
| nil channel | Go 的 nil channel 永久阻塞；`go::Chan` 默认构造返回 false，不死锁 | 安全差异；不会让 relay 卡死，但错误处理要显式 |
| `send on closed channel` | Go panic；`Chan::Send` 返回 false | 调用方必须检查返回值 |
| `Close` 重复调用 | Go panic；`Chan::Close` 返回 false | 同上 |
| `time.After` 返回类型 | Go 返回 `<-chan Time`；`go::After` 返回 `shared_ptr<Timer>`，`go::AfterChan` 返回 `Chan<Time>` | 用 `AfterChan` 得到纯通道 |
| `Timer.Stop` 竞态 | 已进入回调的调用栈不会被打断（底层 `core::Timer` 语义） | 与 Go 的 `Stop` 竞态细则不完全一致 |
| `sync.Once` panic | Go 即使 panic 也标记完成；`std::call_once` 不标记 | 用 `DoNoThrow` 获得 Go 语义 |
| `errors.Is` 文本 | Go 只按身份/自定义 `Is`；本封装哨兵额外支持「同类 + 文本相同」 | 兼容 streams 层重复构造的 `"EOF"` |
| `context` 返回值 | Go 返回 `(Context, CancelFunc)`；C++ 返回 `CancelContext` | 可结构化绑定 `auto [ctx, cancel]` |
| `Listener.Addr` 命名 | C++ 不允许成员函数与类型同名，`Addr()` 返回类型别名 `AddressType` | 调用方无需关心 |
| UDP Listener | `Accept` 等到首个数据报并缓存；返回的 `UDPConn` 按 peer 收发 | relay 不需要多 peer UDP；如需请补 `ListenPacket` |
| `conn.Read` 的 EOF | 返回 `go::ErrEOF()`；`go::io` 适配器会转成 `streams::IoEof()` | `io.Copy` 正确结束 |
| 时间精度 | deadline 换算成毫秒（`Socket::Duration`） | 亚毫秒 deadline 会被提升到 1 ms |

---

## 7. 变更记录

| 日期 | 变更 |
|---|---|
| 2026-10-31 | 新增 `include/go2cpp/go/**` 八个头文件、`tests/go_api/**` 三个测试目标、`cmake/Go2CppGoApi.cmake`、本文档 |
