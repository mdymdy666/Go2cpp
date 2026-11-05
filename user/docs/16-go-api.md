# Go API 兼容层（交接文档）

> ⚠️ **本文性能数字均为历史快照（2026-11-04 前，未按现行口径复跑）**：
> 文中的吞吐（MiB/s、GB/s）、倍率（×）、延迟（ns/µs/ms）与分位（p50/p99）数字，来自当时的交接记录，
> **不代表当前实现**。引用前先查 `docs/metrics-ledger.md` 对应 M 编号（M17–M22 为 2026-11-04 报告补录：
> tunnel 2385.3 MiB/s、TLS AEAD 1.02–1.04×、bufio 17695 MiB/s、Parser 1.95 ms、kv 1.09×、WS 0.96×/0.93×）；
> 台账无对应条目的一律按「历史数据，未复跑」处理，**禁止当作现状结论**。

> **定位**：把 Go 标准库的常用接口（net/sync/chan/context/error/time/io/os-signal/crc32/rand/flag）映射成 C++ 薄封装，让 Go 代码逐函数替换，不引入第二套运行时。
> **源码**：`include/go2cpp/go/`（12 个头文件，4,871 行，全部 header-only）；测试 `tests/go_api/`
> **构建接入**：`-DGO2CPP_WITH_GO_API=ON`（默认 OFF）；测试目标 `go_api_net`、`go_api_concurrency`、`go_api_stress`、`go_api_crc32`、`go_api_crypto_rand`、`go_api_flag`、`go_api_signal`（Unix only）
> **链接依赖**：`go2cpp_hash`（crc32 的查表与硬件路径）+ 既有的 Channel/Context/Socket/TimerService 运行时
> **设计全文**：[go-api-compat.md](../../docs/modules/go-api-compat.md)（映射表、JinShan 覆盖率、差异总表）、[go-stdlib-gaps.md](../../docs/modules/go-stdlib-gaps.md)（os/signal、crc32、crypto/rand、flag 四个缺口）

## 1. 这个模块是什么

不是「用 C++ 重写 Go 运行时」，而是**在已有运行时之上加一层 Go 形状的门面**：

```
Go 源码                     C++ 调用                        底层
net.Listen("tcp", addr) →  go::net::Listen(...)         →  Socket::CreateTCP + bind/listen
ch := make(chan T, 128) →  go::Chan<T>::Make(128)       →  go2cpp::Channel<T>
time.After(30*Second)   →  go::AfterChan(30*go::Second) →  core::TimerService
signal.Notify(c, SIGINT)→  go::signal::Notify(c, {...}) →  sigaction + 自建 pipe
```

两个设计约束决定了它长这样：

1. **薄**：每个函数 3~10 行直接转发，不复制状态机、不缓存、不做二次排队。因此「Go API 有 bug」
   与「底层有 bug」是两件可分开定位的事；
2. **错误不退出进程、不抛异常**：Go 的 `(T, error)` 在 C++ 里落成 `Result` 结构体（`IOResult`、
   `ListenResult`、`BigResult`），而不是 `std::expected`（C++17 没有）或异常（Go 不用异常）。
   Go 的 `log.Fatal`/`flag.ExitOnError` 在这里一律返回错误码（§7 第 1 条）。

| 能力 | Go 包 | 状态 |
|---|---|---|
| 监听/拨号/Conn/deadline/半关闭 | `net` | 完整（UDP 是最小语义） |
| Mutex/RWMutex/WaitGroup/Once/Cond | `sync` | 完整 |
| 有界 channel / 非阻塞收发 / select / default | 语言级 | 完整 |
| Cancel/Timeout/Deadline/Value/Cause | `context` | 完整（`NotifyContext` 除外） |
| `New`/`Wrap`/`Is`/`As`/`Join`/哨兵 | `errors` | 完整 + `ErrEOF`/`ErrClosed`/`ErrTimeout` |
| Now/Since/Sleep/After/Timer/Ticker | `time` | 完整（无 `Parse`/`Format`） |
| Copy/CopyBuffer/ReadFull/LimitReader/Discard | `io` | 完整 |
| Notify/Stop/Ignore/Reset | `os/signal` | 完整（无 `NotifyContext`） |
| ChecksumIEEE/Checksum/Update/MakeTable | `hash/crc32` | 符号映射，零新增算法 |
| Read/Int/Prime/BigInt | `crypto/rand` | 完整（`BigInt` 只做无符号） |
| String/Int/Bool/Duration + Parse/Visit | `flag` | 完整（不退出进程） |

## 2. 架构 / 数据流

### 2.1 分层

```
   业务代码（原本是 Go）
        ▼
   go2cpp::go  ── chan / context / error / sync / time / io / net / signal / crc32 / rand / flag
        │                                   （header-only，无状态、无全局单例）
        ▼
   go2cpp 运行时 ── Channel<T> / Context / Socket / TimerService / Scheduler
        ▼
   OS ── epoll / io_uring / getrandom / sigaction / pthread
```

**封装层本身没有线程**。`go_api_signal` 是唯一例外：第一次 `Notify` 时起一条分发线程
（信号必须有人 `read(2)` 自建 pipe）。

### 2.2 阻塞点必须进调度器（最重要的不变量）

```
 业务 Fiber 调 conn.Read()
   ▼  go::net::TCPConn::Read（把 deadline 换算成 Socket 超时）
 Socket::recv ── EAGAIN ──► hook 拦截 ──► 挂起当前 Fiber，注册 epoll 事件 ──► 切到别的 G
   ▼
 数据到达 → 唤醒 Fiber → recv 返回 → IOResult{n, err}
```

因此：**在 Fiber 里调 `go::net::Conn::Read` 是协程阻塞（不占 M），在裸 `std::thread` 里调是线程阻塞**。
两种都合法，混用时不要指望线程数不涨。

### 2.3 `select` 的 case 集合

```
go::Select({ ch.RecvCase(),  ch.SendCase(v),  go::DoneCase(ctx),
             go::AfterCase(go::Second),  go::DefaultCase() })
   ▼
 SelectResult{ selected, index, value }   // index 对应 cases 的下标
```

## 3. 对外 API（关键接口）

### 3.1 chan / select / context / error

```cpp
// include/go2cpp/go/chan.go.hpp
template <typename T> class Chan final {
    static Chan Make(std::size_t capacity = 0);      // make(chan T, n)
    bool Send(T value);                              // ch <- v；关闭/失败返回 false（Go panic）
    bool SendCtx(const ContextPtr& context, T value);
    bool Recv(T& out);                               // <-ch；关闭后返回 false
    bool TrySend(T value);  bool TryRecv(T& out);  void Close();
    std::size_t Len() const;  std::size_t Cap() const noexcept;
    SelectCase RecvCase() const;  SelectCase SendCase(T value) const;
};
inline SelectCase DoneCase(const ContextPtr& context);
inline SelectResult Select(std::initializer_list<SelectCase> cases, ...);

// include/go2cpp/go/context.go.hpp
struct CancelContext { ContextPtr context; CancelFunc cancel; };   // 可结构化绑定
inline ContextPtr Background();  inline ContextPtr TODO();
inline CancelContext WithCancel(const ContextPtr& parent);
inline CancelContext WithTimeout(const ContextPtr& parent, Duration timeout);
inline CancelContext WithDeadline(const ContextPtr& parent, ContextTimePoint deadline);
inline const DoneSignal& Done(const ContextPtr& context);
inline error Err(const ContextPtr& context);  inline error Cause(const ContextPtr& context);

// include/go2cpp/go/error.go.hpp
inline error New(std::string message);                  // errors.New
inline error Wrap(error cause, std::string message);    // fmt.Errorf("%w")
inline bool Is(const error& err, const error& target) noexcept;
template <typename T> std::shared_ptr<T> As(const error& err);   // errors.As
inline std::string ErrorString(const error& err);
inline const error& ErrEOF();  inline const error& ErrClosed();  inline const error& ErrTimeout();
```

### 3.2 sync / time / io

```cpp
// include/go2cpp/go/sync.go.hpp
class Mutex final { void Lock(); void Unlock(); bool TryLock(); bool LockCtx(const ContextPtr&); };
class RWMutex final { void RLock(); void RUnlock(); void Lock(); void Unlock(); };
class WaitGroup final {
    void Add(std::int64_t delta);  void Done();  void Wait();    // 无调度器时抛 std::logic_error
    bool WaitCtx(const ContextPtr& context);  bool WaitFor(Duration timeout);
};
class Once final { bool Do(fn);  bool DoNoThrow(fn); };          // DoNoThrow 才是 Go 语义

// include/go2cpp/go/time.go.hpp
using Duration = ::go2cpp::time::Duration;
inline Time Now();  inline Duration Since(const Time& start);  inline void Sleep(Duration);
inline Timer::Ptr NewTimer(Duration delay);  inline Timer::Ptr After(Duration delay);
inline Chan<Time> AfterChan(Duration delay); inline Ticker::Ptr NewTicker(Duration period);
using ::go2cpp::time::Second;      // 单位常量：Second / Millisecond / Microsecond / Nanosecond

// include/go2cpp/go/io.go.hpp
inline ReadResult  ReadFull(Reader&, void* buffer, std::size_t size);
inline ReadResult  CopyBuffer(Writer& destination, Reader& source, void* buffer, std::size_t size);
inline WriteResult WriteString(Writer& writer, std::string_view text);
class ConnIO final : public Reader, public Writer, public Closer;   // net::Conn → io 适配器
```

### 3.3 net

```cpp
// include/go2cpp/go/net.go.hpp
struct IOResult { std::size_t n; error err; bool Ok() const noexcept; };
class Conn {
    virtual IOResult Read(void* buffer, std::size_t size) = 0;        // EOF 时 err == ErrEOF()
    virtual IOResult Write(const void* buffer, std::size_t size) = 0; // 允许部分写
    virtual error Close() = 0;
    virtual Addr LocalAddr() const = 0;  virtual Addr RemoteAddr() const = 0;
    virtual error SetReadDeadline(const Time& deadline) = 0;
    virtual error SetWriteDeadline(const Time& deadline) = 0;
};
class Listener { virtual std::pair<ConnPtr, error> Accept() = 0; virtual error Close() = 0; };
class TCPConn final : public Conn { IOResult CloseWrite();  IOResult CloseRead(); };
struct DialResult   { ConnPtr conn;        error err;  bool Ok() const noexcept; };
struct ListenResult { ListenerPtr listener; error err; bool Ok() const noexcept; };
inline ListenResult Listen(const std::string& network, const std::string& address);
inline DialResult   DialTimeout(const std::string& network, const std::string& address, Duration);
```

`network` 接受 `"tcp"/"tcp4"/"tcp6"/"udp"/"udp4"/"udp6"`；`address` 是 `"host:port"`（IPv6 用 `"[::1]:port"`）。

### 3.4 os/signal、crc32、crypto/rand、flag

```cpp
// include/go2cpp/go/os_signal.hpp —— namespace go2cpp::go::signal
inline bool Supported() noexcept;                        // Windows 上 false
// Notify(c, {sigs...}) / Notify(c, sig)：空列表 = 全部可捕获信号（1..63）；通道满即丢
// Stop(c) / Ignore({sigs...}) / Reset({sigs...})；无监听者时恢复 SIG_DFL
// 用 sigaction 而非 signalfd（理由见 §9 与 os_signal.hpp 文件头）

// include/go2cpp/go/crc32.hpp —— namespace go2cpp::go::crc32（符号映射，零新增算法）
inline std::uint32_t ChecksumIEEE(ByteSpan data) noexcept;
inline std::uint32_t Checksum(ByteSpan data, const Table& table) noexcept;
inline std::uint32_t Update(std::uint32_t crc, const Table& table, ByteSpan data) noexcept;

// include/go2cpp/go/crypto_rand.hpp —— namespace go2cpp::go::crypto::rand
// Source：getrandom(2) → /dev/urandom 回退；Windows 用 std::random_device
struct BigResult { BigInt value; error err; };
inline std::size_t Read(void* buffer, std::size_t size);
inline BigResult Int(Source& source, const BigInt& max);   // [0, max)，拒绝采样
inline BigResult Prime(Source& source, int bits);          // 最高两位置 1

// include/go2cpp/go/flag.hpp —— namespace go2cpp::go::flag
inline std::shared_ptr<std::string> String(const std::string& name, std::string value, ...);
inline std::shared_ptr<bool> Bool(const std::string& name, bool value, ...);
inline ParseResult Parse(int argc, char** argv);           // 失败不退出进程
```

## 4. 最小用法（可编译示例）

```cpp
// go_echo.cpp —— 监听一个端口，接受一条连接，读一次、回写、关闭
#include <cstdio>
#include <vector>

#include "go2cpp/go/net.go.hpp"
#include "go2cpp/go/time.go.hpp"

int main() {
    auto ln = go2cpp::go::net::Listen("tcp", "127.0.0.1:9000");
    if (!ln.Ok()) { std::fprintf(stderr, "listen: %s\n", go2cpp::go::ErrorString(ln.err).c_str()); return 1; }

    auto accepted = ln.listener->Accept();          // pair<ConnPtr, error>
    if (!accepted.first) {
        std::fprintf(stderr, "accept: %s\n", go2cpp::go::ErrorString(accepted.second).c_str());
        return 1;
    }
    auto conn = accepted.first;

    // 30 s 读超时；deadline 是绝对时间点，不是相对时长
    (void)conn->SetReadDeadline(go2cpp::go::Now().Add(30 * go2cpp::go::Second));

    std::vector<char> buffer(65536);
    const auto read = conn->Read(buffer.data(), buffer.size());
    if (read.err) {
        std::fprintf(stderr, "read: %s\n", go2cpp::go::ErrorString(read.err).c_str());
        (void)conn->Close();
        return 1;
    }
    const auto written = conn->Write(buffer.data(), read.n);   // 允许部分写，真实代码要循环
    (void)conn->Close();
    std::printf("echoed %zu -> %zu bytes\n", read.n, written.n);
    return written.err ? 1 : 0;
}
```

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DGO2CPP_WITH_GO_API=ON
cmake --build build --target go2cpp_runtime -j
g++ -std=c++17 -O2 -Iinclude go_echo.cpp -Lbuild -lgo2cpp_runtime -lpthread -o go_echo
./go_echo &                          # 终端 1
printf 'hello' | nc 127.0.0.1 9000    # 终端 2，应打印 hello
```

要做双向泵就是 `go::io::ConnIO dst(*a), src(*b); go::io::CopyBuffer(dst, src, buf, sizeof(buf));`
再接 `static_cast<go::net::TCPConn&>(*b).CloseWrite()`。

## 5. 源码位置

| 文件 | 行数 | 职责 |
|---|---:|---|
| `go/chan.go.hpp` | 250 | `Chan<T>`、`RecvCase`/`SendCase`/`DoneCase`、`Select` |
| `go/context.go.hpp` | 181 | `WithCancel/Timeout/Deadline/Value`、`Done/Err/Cause`、`ErrCanceled` |
| `go/error.go.hpp` | 234 | `New/Wrap/Is/As/Join/Unwrap`、`ErrEOF/ErrClosed/ErrTimeout` 哨兵 |
| `go/net.go.hpp` | 716 | `Listen/Dial/DialTimeout`、`Conn`/`Listener`/`TCPConn`/`UDPConn`、`HostPort` |
| `go/sync.go.hpp` | 221 | `Mutex`/`RWMutex`/`WaitGroup`/`Once`/`Cond` |
| `go/time.go.hpp` | 301 | `Now/Since/Sleep`、`Timer`/`Ticker`/`After`/`AfterFunc`、单位常量 |
| `go/io.go.hpp` | 223 | `Copy/CopyBuffer/CopyN/ReadFull/ReadAll/WriteString`、`ConnIO` 适配器 |
| `go/os_signal.hpp` | 657 | `signal` 命名空间：`Notify/Stop/Ignore/Reset`、`Signal`、`Supported` |
| `go/crc32.hpp` | 170 | `IEEE`/`Castagnoli`/`Koopman` 表与三个入口的符号映射 |
| `go/crypto_rand.hpp` | 1,031 | `Source`/`OsSource`、`Read`/`Int`/`Prime`、无符号 `BigInt` + Miller-Rabin |
| `go/flag.hpp` | 867 | `FlagSet`、`String/Int/Bool/Int64/Float64/Duration`、`Parse`/`Visit`/`PrintDefaults` |

`src/` 下**没有对应实现**（全部 header-only，`go/go.hpp` 只是汇总头），所以任何性能问题都落在
它调用的底层模块上。

## 6. 性能数据

**本层没有 asm 级实测**：`docs/reports/` 下没有 `asm-go-api*.md`。这一层是 3~10 行的转发
（`Listen` 内部就是 `Socket::CreateTCP` + `bind` + `listen`），成本全在底层 `Socket`/`Channel`/`Scheduler`。

唯一有实测数字的能力是 **crc32**（它直接落到 `hash::Crc32` 的加速路径上，
来源 [go-stdlib-gaps.md](../../docs/modules/go-stdlib-gaps.md) §2.3，64 × 1 MiB、单核）：

| 多项式 | 吞吐 | 实现 |
|---|---:|---|
| IEEE（`ChecksumIEEE` 默认） | **18.38 GB/s** | PCLMULQDQ 折叠 + Barrett 归约 |
| Castagnoli | **12.18 GB/s** | SSE4.2 `crc32q` |
| Koopman | **3.20 GB/s** | slicing-by-8 回退 |

`crypto/rand` 的实测是**正确性**而非吞吐（1e6 字节 → 125000 个 8 字节块零重复、256 桶卡方
`χ²(255)=297.3`）；`go_api_signal` 没有吞吐指标（每进程一次注册）。要量其它接口的延迟，
自己写基准并绑核，别引用本文之外的数字。

## 7. 边界与已知限制

1. **`flag` 解析失败不退出进程、不自动打印用法**（`-h` 除外）。这是与 Go 最关键的行为差异：Go 的
   `flag.Parse` 会 `ExitOnError`，这里返回 `ParseResult`，由调用方决定。
2. **`WaitGroup::Wait()` 在没有调度器时抛 `std::logic_error`**：它委托 `sync::WaitGroup`，后者需要
   scheduler 才能挂起等待。裸 `std::thread` 环境请用 `WaitFor` 或自己用 `mutex`+CV。
3. **nil channel 不死锁**：Go 的 nil channel 永久阻塞；`go::Chan` 默认构造后 `Send`/`Recv` 返回
   false。这是**安全差异**——不会让 relay 卡死，但错误处理必须显式。
4. **向已关闭 channel 发送/重复 Close**：Go panic，这里返回 false。
5. **`sync.Once` 的 panic 语义不同**：Go 即使 panic 也标记完成；`std::call_once` 不标记，要 Go
   语义请用 `DoNoThrow`。
6. **`Timer.Stop` 竞态**：已进入回调的调用栈不会被打断（底层 `core::Timer` 语义）。
7. **`context` 返回值形态**：Go 返回 `(Context, CancelFunc)`，这里返回 `CancelContext`（可结构化绑定）。
8. **`errors.Is` 的文本匹配**：Go 只按身份/自定义 `Is`；本封装的哨兵额外支持「同类 + 文本相同」，
   是为了兼容 `streams` 层重复构造的 `"EOF"`。
9. **UDP 是最小语义**：`UDPListener::Accept` 等到首个数据报并缓存，返回的 `UDPConn` 按 peer 收发；
   多 peer 广播需要 `ListenPacket`（未提供）。
10. **signal 的边界**：通道满即丢；向已关闭通道发送静默丢弃（Go panic）；信号编号上限 63（64 位
    原子掩码）；`SIGKILL`/`SIGSTOP` 不可捕获；Windows 上 `Supported() == false`；`Notify` 会覆盖
    宿主已有的 `sigaction`（Go 亦然）。
11. **`crypto/rand` 的 `Prime` 始终使用传入的 `Source`**：Go 1.26+ 会忽略传入 reader 改用全局 DRBG，
    本实现保留可测试性，语义与 Go 1.25 及更早一致。`BigInt` 只做无符号、无完整 `Div`。
12. **`time.After` 的返回类型**：Go 返回 `<-chan Time`；`go::After` 返回 `shared_ptr<Timer>`，
    要纯通道请用 `go::AfterChan`。

## 8. 测试与验证方法

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DGO2CPP_WITH_GO_API=ON
cmake --build build -j12
ctest --test-dir build -R 'go_api_' --output-on-failure
```

| 目标 | 覆盖 |
|---|---|
| `go_api_net` | TCP 回显、deadline 超时、EOF、Close 幂等、UDP 回环、`io.Copy`/`ReadFull`/`WriteString` |
| `go_api_concurrency` | Mutex/WaitGroup/Once/Cond/RWMutex、Chan 生产者消费者/close/select、Context 取消/超时/Value、Timer/Ticker/After、errors |
| `go_api_stress` | 8 客户端 × 30 轮随机 TCP、4×20000 Channel、8×20000 Mutex、500 Context deadline、200 After + 10 Ticker、1000 层 errors |
| `go_api_crc32` | 标准向量（`"123456789"` → IEEE `0xCBF43926`）、Go 1.27 oracle 1e6 字节、逐位参考、2000 次随机切分 `Update`、流式 Hash、边界长度 |
| `go_api_crypto_rand` / `go_api_flag` | Read 随机性（1e6 字节零重复 + 卡方）、75 个 `Int` 值逐值对拍、400 个素性候选；flag 的四种赋值形式、bool 三形态、Duration 11 合法 + 8 非法 |
| `go_api_signal` | SIGINT 端到端（fork 子进程 `kill(2)`）、Stop、Ignore、多通道、String 表、Stop 恢复默认处置、无缓冲通道 100 次 `raise` |

自建最小闭环：跑 §4 的 `go_echo` 并用 `nc` 打一发；把 `SetReadDeadline` 改成 `Now()`（已过期）
验证 `read.err == ErrTimeout()`；用 `go::Select` 同时等 channel 与 `DoneCase` 验证 ctx 取消能打断
等待；`go_api_signal` 只在 Unix 上注册（依赖 `fork`/`sigaction`/`waitpid`）。

## 9. 交接注意

- **这是门面不是运行时**：遇到「Go API 行为不对」，先判断是门面映射错还是底层错。门面只有 3~10 行，
  读一眼就能排除；底层问题去 `docs/modules/{network,channel,sync,timer}.md`。
- **不要在这层加状态**：一旦加了缓存/队列/重试，就不再是「薄封装」，Go 语义与底层语义会分裂成两套。
  需要重试请在业务层做（或用 `bento` 的 `Backoff`）。
- **`WaitGroup::Wait` 会抛异常**：新人最容易踩的坑。裸线程测试里用 `WaitFor`，或确认调度器已启动。
- **`flag` 不退出进程、signal 用 `sigaction`**：两条都是刻意的库边界——前者为了可嵌入，后者因为
  `signalfd` 要在每个线程创建处插 `pthread_sigmask`（见 `os_signal.hpp` 文件头）。
- **crc32 是符号映射**：任何「优化 crc32」都应落在 `include/go2cpp/hash/crc32.hpp`，改在这一层等于
  复制实现（[asm-reuse-hash.md](../../docs/reports/asm-reuse-hash.md) 记录过这个教训）。
- **未完成**：`signal::NotifyContext`、`tools/time.hpp` 的 `ParseDuration`（`flag` 现在内联了一份，
  补齐后应改为转发）、`crypto/subtle.ConstantTimeCompare`、`crypto/rand.Text()`（Go 1.24）、
  UDP 的 `ListenPacket`/多 peer 语义。
