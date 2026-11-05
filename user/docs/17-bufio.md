# 带缓冲 IO（交接文档）

> ⚠️ **本文性能数字均为历史快照（2026-11-04 前，未按现行口径复跑）**：
> 文中的吞吐（MiB/s、GB/s）、倍率（×）、延迟（ns/µs/ms）与分位（p50/p99）数字，来自当时的交接记录，
> **不代表当前实现**。引用前先查 `docs/metrics-ledger.md` 对应 M 编号（M17–M22 为 2026-11-04 报告补录：
> tunnel 2385.3 MiB/s、TLS AEAD 1.02–1.04×、bufio 17695 MiB/s、Parser 1.95 ms、kv 1.09×、WS 0.96×/0.93×）；
> 台账无对应条目的一律按「历史数据，未复跑」处理，**禁止当作现状结论**。

> **定位**：Go `bufio` 的 C++ 复刻 —— `Reader`/`Writer`/`Scanner` 三个模板装饰器把 N 次小 `Read`/`Write` 合并成 1 次大调用；`Writer` 额外提供 Go 没有的**可配置批量 flush**（4 种策略 + iovec 段），`Peek`/`Scanner::Bytes` 交付零拷贝 `ByteSpan`。
> **源码**：`include/go2cpp/bufio/`（9 个头文件，约 105 KB；核心 `reader.hpp` 693 行、`writer.hpp` 668 行、`scanner.hpp` 419 行）；实现 `src/bufio/bufio.cpp`（211 行）；Socket 适配层 `include/go2cpp/network/bufio_socket.hpp`（498 行，不在本目录）
> **构建接入**：库目标 `go2cpp_bufio`（单源文件，`PUBLIC go2cpp_error`，别名 `go2cpp::bufio`，属于 `GO2CPP_MODULE_TARGETS`）
> **测试目标**：`go2cpp_bufio_test`（252,696 checks）、`go2cpp_bufio_stress`、`go2cpp_bufio_batch_test`（67,481 checks）；`go2cpp_bufio_batch_bench` 只构建、不注册 ctest
> **设计/实测全文**：[2026-10-31-http-perf.md](../../docs/reports/2026-10-31-http-perf.md)（写路径三档对比）、[2026-11-01-network-app-opt-review.md](../../docs/reports/2026-11-01-network-app-opt-review.md)（批量基准）、[2026-11-01-asm-network-review.md](../../docs/reports/2026-11-01-asm-network-review.md)（P11/P12 指令审计）、状态机 [Reader.md](../../docs/state-machines/Reader.md)、[Scanner.md](../../docs/state-machines/Scanner.md)

## 1. 这个模块是什么

`bufio` 只有一件事：**把「每次都要系统调用/网络往返」的小 IO，变成「一次读/写一大块，之后只动内存」**。三个类型各管一段：

| 类型 | 对应 Go | 解决什么 | 不解决什么 |
|---|---|---|---|
| `Reader<ReaderT>` | `bufio.Reader` | 逐字节/逐行解析时不再每次 `read(2)` | 不做分帧、不做协议解析、不支持 `Seek` |
| `Writer<WriterT>` | `bufio.Writer` + 本项目 `batch` 扩展 | 把小写攒成大写；批量路径把 N 条消息压成 1 次 `writev`/`sendmmsg` | 不做重传/合包语义（字节流顺序不变） |
| `Scanner<ReaderT>` | `bufio.Scanner` | 决定「一条记录到哪里结束」，token 零拷贝 | 不做 Unicode 词法（`ScanWords` 只按 ASCII 空白） |

两个设计约束决定了它长这样：

1. **只做装饰，不做替代**：`ReaderT`/`WriterT` 只需满足 `streams::ReadResult Read(char*, size_t)` / `streams::WriteResult Write(const char*, size_t)` 两个极小契约，`streams::Reader` 家族、`os::File`、`net::Socket` 适配器、另一个 `bufio::Reader`、测试桩都能套；
2. **错误不抛异常**：全部走 `Result<T>`（`ok()`/`Err()`/`Value()`），哨兵错误用 `go2cpp::Is(err, bufio::ErrXxx())` 做**身份**比较，错误在首次出现后**粘性**。

与 Go 对齐的语义（同名同边界）：`Read` 最多触发一次底层读、`Peek` 零拷贝且不消费、`ReadLine` 去 `\r\n`、`ReadString` 含分隔符且可无限长、`UnreadByte`/`UnreadRune` 受「最近一次操作」约束、`Flush` 循环处理部分写、`Reset` 丢弃未刷数据、`Scanner` 的 `ErrFinalToken` 终止语义。

本项目的扩展（Go 没有）：

- `batch.hpp`：`FlushPolicy`（`kOnFull`/`kInterval`/`kOnExplicit`/`kHybrid`）、`BatchConfig`、`BatchEntry`；底层实现 `WriteBatch` 时走 `writev`/`sendmmsg`；
- `ByteSpan`：C++17 没有 `std::span`，这里给 `(uint8_t*, size_t)` 的可变非拥有视图；
- `ScanBytes`、`ErrFinalToken`、`Scanner::Buffer(max)` 的上限语义。

## 2. 架构 / 数据流

### 2.1 分层与契约

```
   业务 / 协议（HTTP、WS、MQTT、日志、配置解析）
     │ 按行 / 按词 / 按 token              ▲ 逐条小写
     ▼                                     │
  bufio::Scanner<ReaderT>            bufio::Writer<WriterT>
     │ Read(char*, size_t)                │ Write(ByteSpan)
     ▼                                    ▲
  bufio::Reader<ReaderT>                 │
     │ Read(char*, size_t)                │
     ▼                                    ▲
  streams::Reader / os::File / net::Socket 适配器（bufio_socket.hpp）
     │ read(2)                            ▲ write(2) / writev(2) / sendmmsg(2) / sendfile(2)
     ▼                                    │
            内核 / 真实文件
```

三个类型都**只引用**底层对象（构造函数收 `T&`），不拥有它；底层必须活得比装饰器久。全部方法假定同一线程串行调用，**内部无锁**。

### 2.2 一次「读一行」的数据流

```
  底层 read(2) 返回 4096 字节
        ▼
  m_buffer: [ ........ 已读 4096 ........ ]
             ^m_r=0      ^m_w=4096
  ReadLine()
    ├─ memchr('\n') 命中第 index 字节
    ├─ 交付 [m_r, m_r+index)（去行尾 '\r'），m_r += index + 1
    ▼
  m_buffer: [####已交付####| 剩余字节 ]
                        ^m_r     ^m_w
  fill()（缓冲空时）
    ├─ memmove 把 [m_r, m_w) 搬到头部 → m_w=剩余、m_r=0
    ├─ 一次底层 Read 填满剩余空间
    └─ 读到 0 字节且无错误 → 累计 100 次判 streams::IoErrNoProgress()
```

EOF 是**粘性**的：底层一旦返回 EOF，`m_err` 记住，后续读方法直接以它收尾；`Err()` 可查。

### 2.3 缓冲策略（三者不同）

| 类型 | 初始容量 | 增长 | 压缩方式 | 失败形态 |
|---|---|---|---|---|
| `Reader` | 4096（可传，<16 提升到 16） | **不增长** | `fill()` 里 `memmove` 掉已消费前缀 | `ReadLine` 超容量 → `ErrTooLong`；`ReadString` 无界 |
| `Writer` | 4096（同上） | **不增长** | `ConsumeFront()` 前移缓冲并同步段表 | 缓冲满自动批量 flush；大块且缓冲空时直写 |
| `Scanner` | 4096（可传） | 放不下 token 时**翻倍**，夹到 `max_token_size`（默认 64 KiB） | `ReadMore()` 里 `memmove` 未切分窗口 | 到上限仍切不出 → `ErrTooLong` |

### 2.4 批量写路径（本项目扩展）

```
  Write("A") ─┐
  Write("B") ─┼─► m_buffer: [A][B][C]     m_segments = [{0,1},{1,1},{2,1}]
  Write("C") ─┘
        │ 触发：水位 threshold / 段数 max_batch / 间隔 interval / 显式 FlushBatch
        ▼
  FlushBatch()
    ├─ 底层有 WriteBatch(const BatchEntry*, size_t) → 一次调用（writev / sendmmsg）
    └─ 没有 → 退化为一次 Write（数据本来就在连续缓冲里，系统调用同样是 1 次）
        ▼
  部分写：result.count < 总量 → ConsumeFront(count) 后继续循环，直到写完或出错
```

段边界**不改变字节流**：TCP 上接收端看到的仍是按写入顺序拼接的字节，段只决定一次 `writev` 的 iovec 数量。UDP 例外（`SocketBatchWriter` 的 datagram 模式下一条段 = 一条数据报）。

## 3. 对外 API

### 3.1 公共底座：`ByteSpan` / `Result` / 哨兵错误

```cpp
class ByteSpan {                       // byte_span.hpp：非拥有、可变、浅 const
    constexpr ByteSpan(std::uint8_t* data, std::size_t size) noexcept;
    std::uint8_t* data() const noexcept;
    std::size_t size() const noexcept;
    ByteSpan first(std::size_t count) const noexcept;
    ByteSpan subspan(std::size_t offset, std::size_t count = SIZE_MAX) const noexcept;  // 越界静默截断
};
ByteSpan AsBytes(char*, std::size_t);          // 另有 string& / vector<uint8_t>& 重载
std::string_view AsStringView(ByteSpan) noexcept;
std::string ToString(ByteSpan);                // 唯一会分配的入口

template <typename T> class Result {   // result.hpp：值 + 错误
    bool ok() const noexcept;  explicit operator bool() const noexcept;
    const ErrorPtr& Err() const noexcept;
    T& Value() & noexcept;     T ValueOr(T fallback) const&;
};                                     // Result<void> 只有 ok()/Err()

ErrorPtr ErrBufferFull();         ErrorPtr ErrTooLong();
ErrorPtr ErrNegativeCount();      ErrorPtr ErrInvalidAdvance();
ErrorPtr ErrInvalidUnreadByte();  ErrorPtr ErrInvalidUnreadRune();
ErrorPtr ErrFinalToken();         // EOF 复用 streams::IoEof()，不在这里
```

生命周期契约：`Peek`/`ReadSlice`/`Scanner::Bytes` 返回的 span 指向内部缓冲，**下一次读/扫描之后失效**；拷贝 span 不延长数据寿命。

### 3.2 `Reader<ReaderT>`

```cpp
explicit Reader(ReaderT& source);                       // 4096
explicit Reader(ReaderT& source, std::size_t size);     // <16 提升到 16

std::size_t Read(ByteSpan p);                           // 最多一次底层读，n 可小于 p.size()
streams::ReadResult Read(char* buffer, std::size_t size);  // streams 契约；有数据时错误留到下一次
Result<std::uint8_t> ReadByte();
ReadRuneResult ReadRune();                              // {rune, size, error}；非法序列给 U+FFFD + size=1
Result<void> UnreadByte();  Result<void> UnreadRune();
Result<std::string> ReadLine();                         // 去 '\n'、去行尾 '\r'；超容量 → ErrTooLong
Result<std::string> ReadString(char delim);             // 含分隔符，长度无上限
Result<std::string> ReadBytes(char delim);              // 同 ReadString
Result<ByteSpan> ReadSlice(char delim);                 // 含分隔符的零拷贝切片；缓冲满 → ErrBufferFull + 已有数据
Result<ByteSpan> Peek(std::size_t n);                   // 零拷贝不消费；n > Size() → ErrBufferFull（不预读）
std::size_t Discard(std::size_t n);
std::size_t Buffered() const noexcept;  std::size_t Size() const noexcept;
const ErrorPtr& Err() const noexcept;                   // 粘性；正常 EOF 时是 streams::IoEof()
```

`ReadLine` 的 `ErrTooLong` 路径**不回滚**已消费字节，Reader 停在溢出点附近，通常应丢弃；需要任意长行用无界的 `ReadString('\n')`。

### 3.3 `Writer<WriterT>` + `BatchConfig`

```cpp
explicit Writer(WriterT& destination);                  // 4096
explicit Writer(WriterT& destination, std::size_t size);

std::size_t Write(ByteSpan p);                          // 返回「已接受」字节数，不代表已落地
streams::WriteResult Write(const char* data, std::size_t size);  // streams 契约
Result<void> WriteByte(std::uint8_t value);
Result<void> WriteString(std::string_view text);
Result<void> Flush();                                   // 单次 Write，循环处理部分写
Result<void> FlushBatch();                              // 有 WriteBatch 时一次批量写，否则退化为 Flush
void SetBatchConfig(BatchConfig config);                // 立即生效；threshold 按容量钳制
const BatchConfig& GetBatchConfig() const noexcept;
std::size_t PendingBatches() const noexcept;            // 待发段数
template <typename SourceT> Result<std::size_t> ReadFrom(SourceT& source);  // 读到 EOF
void Reset(WriterT& destination) noexcept;              // 丢弃未刷数据、清错误
std::size_t Buffered() const noexcept;  std::size_t Available() const noexcept;
std::size_t Size() const noexcept;      const ErrorPtr& Err() const noexcept;
```

```cpp
enum class FlushPolicy : std::uint8_t { kOnFull, kInterval, kOnExplicit, kHybrid };
struct BatchConfig {                                    // 默认即推荐值
    FlushPolicy policy = FlushPolicy::kHybrid;          // 「满」或「超时」任一满足
    std::size_t max_batch = 64;                         // 上限 1024（对齐 IOV_MAX）
    std::size_t threshold = 4096;                       // 按缓冲容量钳制
    std::chrono::microseconds interval{100};            // 惰性检查，无后台线程
};
```

四项默认值：`kHybrid` + 64 段 + 4096 B + 100 µs。`kInterval`/`kHybrid` 的定时检查发生在 `Write`/`WriteByte` 返回前，**写完就停必须显式 `Flush()`/`FlushBatch()`**。

`Writer` 的析构**不 flush**（与 Go 一致）；忘记 `Flush()` 等于丢数据，`Reset()` 会丢弃未刷缓冲。

### 3.4 `Scanner<ReaderT>`

```cpp
explicit Scanner(ReaderT& source);                      // 4096 / 64 KiB 上限
explicit Scanner(ReaderT& source, std::size_t buffer_size,
                 std::size_t max_token_size = kMaxScanTokenSize);

bool Scan();                                            // false：Err() 为空 = 正常结束，否则是终止原因
std::string Text() const;                               // 拷贝
ByteSpan Bytes() const noexcept;                        // 零拷贝；下一次 Scan 后失效
const ErrorPtr& Err() const noexcept;
void Buffer(std::size_t max_token_size) noexcept;       // 只保留 max 语义
std::size_t MaxTokenSize() const noexcept;  std::size_t BufferSize() const noexcept;
void SetSplit(SplitFunc split);  const SplitFunc& Split() const noexcept;

struct SplitResult { std::size_t advance{0}; ByteSpan token{}; ErrorPtr error; };
using SplitFunc = std::function<SplitResult(ByteSpan data, bool at_eof)>;
SplitResult ScanLines(ByteSpan, bool);   // 去 '\n' 与行尾 '\r'
SplitResult ScanWords(ByteSpan, bool);   // 只按 ASCII 空白
SplitResult ScanBytes(ByteSpan, bool);   // 每字节一个 token
```

`SplitFunc` 契约：`advance` 是消费字节数，`token` 必须指向窗口内部，`advance == 0 && token 为空` 表示「继续读」；用 `token.data() != nullptr` 判断「有 token」（空行是合法 token）。自定义函数越界报 `ErrInvalidAdvance`，不 panic。

### 3.5 选型建议

| 场景 | 用什么 | 理由 |
|---|---|---|
| 逐行读日志/配置 | `Scanner` 或 `Reader::ReadLine` | Scanner 自动扩容到 64 KiB；ReadLine 有上限、失败更明确 |
| 逐字节状态机 | `Reader::ReadByte` | 纯内存操作，1.1 ns/次量级 |
| 先看后决定 | `Reader::Peek` | 零拷贝，不消费 |
| 大块转发 | `Reader::Read(ByteSpan)` + `Writer::Write` | 大块且缓冲空时两边都有直写快路径 |
| 高频小消息（< 4 KiB） | `Writer` + `kHybrid` 或 `FlushBatch` | 把 N 次系统调用压成 1 次 |
| 大包（≥ 缓冲容量） | **绕过 bufio 直写** | 见 §6：4096 B 时 bufio 反而慢 19% |

## 4. 最小用法

```cpp
// bufio_demo.cpp —— 缓冲读 → 按行扫描 → 缓冲写，一趟跑通
#include <algorithm>
#include <cstring>
#include <iostream>
#include <string>

#include "go2cpp/bufio/bufio.hpp"

namespace streams = go2cpp::streams;
using namespace go2cpp::bufio;

struct Source {  // 底层 Read 契约
    std::string text;
    std::size_t pos = 0;
    streams::ReadResult Read(char* p, std::size_t n) {
        const std::size_t take = std::min(n, text.size() - pos);
        std::memcpy(p, text.data() + pos, take);
        pos += take;
        return take ? streams::ReadResult{take, {}} : streams::ReadResult{0, streams::IoEof()};
    }
};

struct Sink {  // 底层 Write 契约
    std::string bytes;
    streams::WriteResult Write(const char* p, std::size_t n) {
        bytes.append(p, n);
        return streams::WriteResult{n, {}};
    }
};

int main() {
    Source source{"alpha\nbeta\r\ngamma\n"};
    Reader reader(source);    // 默认 4096 字节缓冲；CTAD 推出 Reader<Source>
    Scanner scanner(reader);  // 默认 ScanLines，Scanner<Reader<Source>>
    while (scanner.Scan()) std::cout << '[' << scanner.Text() << ']';
    std::cout << '\n';

    Sink sink;
    Writer writer(sink);
    writer.WriteString("x\n");  // 只进缓冲
    writer.Flush();             // 不 Flush 就丢数据
    std::cout << "flushed=" << sink.bytes.size() << '\n';
}
```

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --target go2cpp_bufio -j
g++ -std=c++17 -O2 -Iinclude bufio_demo.cpp -Lbuild -lgo2cpp_bufio -lgo2cpp_error -o bufio_demo
./bufio_demo     # 输出：[alpha][beta][gamma] / flushed=2
```

上面代码在 2026-11-02 用 `g++ 13.3.0 -std=c++17 -Wall -Wextra -Wpedantic` 编译并运行通过。`beta\r\n` 的 `\r` 被 `ScanLines` 去掉；`WriteString` 后不 `Flush()` 则 `sink.bytes` 为空。

## 5. 源码位置

| 文件 | 行数 | 职责 |
|---|---:|---|
| `include/go2cpp/bufio/bufio.hpp` | 94 | 模块入口：只聚合 7 个头文件，不定义类型 |
| `include/go2cpp/bufio/byte_span.hpp` | 152 | `ByteSpan`、`AsBytes`/`AsStringView`/`ToString` |
| `include/go2cpp/bufio/result.hpp` | 132 | `Result<T>`/`Result<void>` |
| `include/go2cpp/bufio/errors.hpp` | 73 | 7 个哨兵错误工厂声明（定义在 .cpp） |
| `include/go2cpp/bufio/limits.hpp` | 65 | 4096/16/100/64 KiB 四个常量 + `detail::ClampBufferSize` |
| `include/go2cpp/bufio/batch.hpp` | 108 | `FlushPolicy`/`BatchConfig`/`BatchEntry` 与默认值 |
| `include/go2cpp/bufio/reader.hpp` | 693 | `Reader<ReaderT>`、`Rune`/`RuneError`、`ReadRuneResult`、`detail::DecodeUtf8` |
| `include/go2cpp/bufio/writer.hpp` | 668 | `Writer<WriterT>`、`detail::HasWriteBatch`、段表、`ReadFrom` |
| `include/go2cpp/bufio/scanner.hpp` | 419 | `SplitResult`/`SplitFunc`、`Scanner<ReaderT>` |
| `src/bufio/bufio.cpp` | 211 | 7 个哨兵错误 + 3 个预置 `SplitFunc` + `DropCarriageReturn`/`IsAsciiSpace` |
| `include/go2cpp/network/bufio_socket.hpp` | 498 | **不在本目录**：`SocketReader`/`SocketBatchWriter`、`BufferedSocketReader/Writer` 别名 |

本目录合计 2,615 行（9 个头 2,404 + 实现 211）。`src/bufio/bufio.cpp` 为什么存在：哨兵错误靠**指针身份**比较，必须全进程一份；`SplitFunc` 是 `std::function` 目标，需要唯一定义。

## 6. 性能数据

**没有 `asm-bufio*.md` 这类专项报告**，`docs/reports/` 下也没有对 `Reader`/`Scanner` 单函数的 objdump 审计。下面三类数字来自既有报告或本次交接复跑，口径逐个注明，**不要混用**。

### 6.1 端到端写路径三档（引自 http-perf，间接）

来源 [2026-10-31-http-perf.md](../../docs/reports/2026-10-31-http-perf.md) §4：一条 TCP 回环连接，客户端连续写协议真实报文，对端独立线程排空，每档 3 轮取最佳。这里的「bufio」是 `net::BufferedSocketWriter`（4 KiB、`kHybrid`），**不是** `Writer` 单函数基准：

| 报文 | 单写 send | bufio(4KB, kHybrid) | bufio+batch(64, writev) | syscall/msg（单写 / bufio / batch） |
|---|---:|---:|---:|---|
| HTTP 响应 120B | 144,433 ops/s | 3,335,649（23.1×） | 3,925,869（27.2×） | 1.000 / 0.029 / 0.031 |
| WS 帧 64B | 186,480 | 5,072,555（27.2×） | 6,181,102（33.1×） | 1.000 / 0.016 / 0.016 |
| MQTT PUBLISH 96B | 181,607 | 3,906,118（21.5×） | 3,886,612（21.4×） | 1.000 / 0.023 / 0.031 |
| CoAP 响应 64B | 183,892 | 5,099,062（27.7×） | 4,988,958（27.1×） | 1.000 / 0.016 / 0.016 |
| 大消息 4096B | **151,121** | 122,721（**0.81×**） | 146,702（0.97×） | 1.000 / 1.000 / 1.000 |

**分界线**：小于缓冲容量的消息收益 21~33 倍；等于/大于容量的消息因「直写 + 一次内存拷贝」退化为慢 19%，应绕过 bufio。

### 6.2 `go2cpp_bufio_batch_bench`（引自 network-app-opt-review）

来源 [2026-11-01-network-app-opt-review.md](../../docs/reports/2026-11-01-network-app-opt-review.md)：16 B 消息、单连接 TCP 回环：

| 场景 | 吞吐 | syscalls/msg |
|---|---:|---:|
| raw-send（逐条） | 0.70 Mmsg/s | 1.20 |
| batch-64 | 12.32 Mmsg/s | 0.019 |
| batch-256 | 15.86 Mmsg/s | 0.005 |

该基准 `go2cpp_bufio_batch_bench` **只构建、不注册 ctest**（见 §8）。

### 6.3 本次交接复跑的 `go2cpp_bufio_stress`（直接，2026-11-02）

WSL 内、未绑核、真实文件 + `read(2)`、页缓存热、3 轮取中位数；1 MiB 数据：

| 模式 | 缓冲 | 底层 read(2) 次数 | 中位耗时 | 吞吐 |
|---|---:|---:|---:|---:|
| bare-read（每次 1B） | 无 | 1,048,576 | 493.332 ms | 2.0 MiB/s |
| `ReadByte` | 4096 | 256 | 1.145 ms | 873.4 MiB/s |
| `Read(128B)` | 4096 | 256 | 0.552 ms | 1,811.5 MiB/s |
| `Read(128B)` | 512 | 2,048 | 1.375 ms | 727.4 MiB/s |
| `Read(128B)` | 65,536 | 16 | 0.421 ms | 2,372.6 MiB/s |

系统调用减少 **99.9756%（4096×）**；相对逐字节裸读的吞吐加速 **430.9×**。注意这是极端对比（1 B/次），真实协议不会这么读；它证明的是「调用次数主导」这个事实。

`go2cpp_bufio_test` 内置场景计时的同批数据：Scanner 扫 10 万行（1,088,890 B）**627.7 MiB/s**、底层调用 268；1 MiB 按 512 B 分块读 **1,390.6 MiB/s**（`Read` 2,048 次、底层 1,415 次）；1 万行日志写入只产生 **157 次**底层 write。

### 6.4 静态指令审计（引自 asm-network-review，非吞吐）

[2026-11-01-asm-network-review.md](../../docs/reports/2026-11-01-asm-network-review.md) 的反汇编统计（没有重新测量，只抄表）：

| 函数 | 指令 | call | jcc | jmp | 原子 |
|---|---:|---:|---:|---:|---:|
| `bufio::Writer::Write` | 345 | 26 | 47 | 22 | 3 |
| `bufio::Writer::FlushBatchImpl` | 784 | 37 | 105 | 33 | 4 |

该报告的 P11/P12 指出两处可省成本：每次 `Write` 在定时策略下取一次 `steady_clock::now()`（18.4 ns）并做若干 `shared_ptr` 原子操作（估计 20~30 条指令/次）；`FlushBatchImpl` 每次 flush 重建 `BatchEntry` 数组（估计 200~400 条指令）。**两项都未实施**，属于待办而非已获得的收益。

**其余能力无数据**：`ReadLine`/`Peek`、`Scanner` 扩容、UDP `sendmmsg` 路径都没有单函数实测；需要数字请按 §8 建基准，不要引用本文件以外的数字。

## 7. 边界与已知限制

1. **`Peek(n > Size())` 直接报错、不预读**。Go 会先填满缓冲再报 `ErrBufferFull` 并返回已有数据；这里返回纯错误（`Result<ByteSpan>` 失败态不带值）。要「多读一点再判断」请自行扩容或先 `Read`。
2. **`ReadLine` 没有 `isPrefix` 两段式**。超长行直接 `ErrTooLong`，且**已消费字节不回滚**，Reader 语义变成「从溢出点继续」，通常应放弃它。任意长行请用无界的 `ReadString('\n')`。
3. **`ReadLine` 末行无 `\n` 时保留 `\r`**。这是 Go 的原始行为（只有 `\r\n` 成对才丢 `\r`）；`Scanner` 的 `ScanLines` 在 EOF 时也丢 `\r`，两者故意不一致。
4. **`Reader` 不支持 Seek**。缓冲会把文件偏移推到「已读」位置，回退只能靠 `UnreadByte`/`UnreadRune`（各一次）。要随机访问直接用 `os::File`。
5. **`Writer` 析构不 flush**。与 Go 一致，理由是不能在析构里交付错误；忘记 `Flush()` 就是丢数据（测试 `TestWriterDestructor` 专门验证了这一点）。
6. **`Reset()` 丢弃未刷数据并清错误**：它不会先把旧目标写空。
7. **`Scanner` 默认单 token 上限 64 KiB**。超过 → `ErrTooLong`；`Buffer(max)` 只能调上限（不能复用外部缓冲，Go 的 `Buffer(buf, max)` 只保留了 `max` 语义）。`ReadLine`/`ReadString` 不共享这个上限。
8. **`ScanWords` 只按 ASCII 空白切分**（空格、`\t`、`\n`、`\r`、`\v`、`\f`）。Go 用 `unicode.IsSpace`，中文/全角空格不在此列。
9. **错误传播是「粘性 + 身份比较」**：第一次失败后 `m_err` 不再清（除非 `Reset()`），所有后续操作直接返回同一个 `ErrorPtr`；判断一律 `go2cpp::Is(err, ...)`，不要比较文本。哨兵错误对象故意泄漏（进程内唯一）。
10. **`Flush()` 与 Go 的关键差异**：Go 只写一次、短写报 `io.ErrShortWrite`；这里**循环写完**（底层每次只吐 3 字节也能正确工作）。底层返回 0 字节且无错误时，连续 100 次才报 `IoErrShortWrite()`，给「暂时不可写」留余地。
11. **`Writer::AvailableBuffer` 未实现**（Go 1.18+）：把内部缓冲直接暴露给调用方需要额外的生命周期契约，本模块没做。
12. **批量策略的定时检查是惰性的**：没有后台线程/定时器；`kInterval`/`kHybrid` 只在 `Write`/`WriteByte` 返回前看表。空闲期不会自动 flush。
13. **`Scanner::Bytes`/`Peek` 的返回值都是「借用」**：下一次读/扫描就失效；跨调用保存请 `ToString`/`Text()` 拷贝。
14. **底层契约必须遵守**：底层 `Read`/`Write` 返回的 `count` 大于请求长度会被判违约（`ReadFrom` 报 `IoErrShortWrite`，`FlushLinear` 报短写），不会信任越界计数。

## 8. 测试与验证

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DGO2CPP_BUILD_TESTS=ON
cmake --build build -j12 --target go2cpp_bufio_test go2cpp_bufio_stress go2cpp_bufio_batch_test

ctest --test-dir build -R 'go2cpp_bufio_test' --output-on-failure        # 252,696 checks
ctest --test-dir build -R 'go2cpp_bufio_stress' --output-on-failure      # 系统调用/吞吐对比
ctest --test-dir build -R 'go2cpp_bufio_batch_test' --output-on-failure  # 67,481 checks
```

| 目标 | 文件 | 覆盖重点 |
|---|---|---|
| `go2cpp_bufio_test` | `tests/bufio/test_bufio.cpp`（2,003 行） | Reader 基础/`Peek`/`ReadLine`/`ReadString`/`Discard`/Unread/Rune；Writer 基础/部分写与故障/析构；Scanner 基础/三种 `SplitFunc`/`Buffer`/`ErrFinalToken`；6 个真实场景（日志、10 万行扫描、1 MiB 分块、短写对端等） |
| `go2cpp_bufio_stress` | `tests/bufio/test_bufio_stress.cpp`（418 行） | 真实文件 + `read(2)`：bare / `ReadByte` / `Read(128B)`，512/4096/65536 三档缓冲；逐字节校验内容；strace 可复核 |
| `go2cpp_bufio_batch_test` | `tests/bufio/test_bufio_batch.cpp`（1,117 行） | 4 种策略、段合并、部分写、粘性错误、0 字节边界、谎报计数、500 轮随机、4 线程独立/共享 Writer、TCP loopback `writev`、5 个场景 |
| `go2cpp_bufio_batch_bench` | `tests/bufio/bench_bufio_batch.cpp`（611 行） | **不是 ctest**：16/64/256 B 消息、4 种策略的吞吐与 p50/p99/p999，结果写 `/tmp/bufio_batch_bench.md` |

本次交接（2026-11-02）在 `.build-bento-wsl` 里复跑了前三个：`checks=252696 failures=0`、`checks=67481 failures=0`、stress 全部通过。

自建最小闭环：

- `strace -c -e trace=read ./go2cpp_bufio_stress --size 65536 --mode bare` 与 `--mode bufio` 对比 `read` 次数（文件头已给出命令）；
- 拿一个「每次最多写 3 字节」的 Sink 套 `Writer`，`Flush()` 后必须字节完整（部分写循环的验收）；
- 自定义 `SplitFunc` 返回越界 `advance`，必须得到 `ErrInvalidAdvance` 而不是崩溃；
- 用 `kOnExplicit` 策略写 100 条消息后不 flush，底层应 0 字节；`FlushBatch()` 后一次调用交付全部。

## 9. 交接注意

- **链接是显式的**：三个类都是模板（头文件可实例化），但 7 个哨兵错误与 3 个 `SplitFunc` 的唯一定义在 `src/bufio/bufio.cpp`。**不链接 `go2cpp_bufio` 会在链接期缺符号**；它和 `os` 一样不在 `runtime` 导出表里。
- **不要按单个调用方改 `limits.hpp`**：四个常量同时决定 Reader/Writer/Scanner 的默认行为；`kMaxConsecutiveEmptyReads` 还被 `Writer::ReadFrom`/`Flush` 复用。
- **`bufio` 与 `streams` 的边界**：`bufio` 只加缓冲，不做 `ReadFull`/`Copy` 这类组合操作（那些在 `streams.hpp`）。`Writer::ReadFrom` 是唯一的内置泵。
- **下游适配层在 `network/bufio_socket.hpp`**：`BufferedSocketWriter` 是 `Writer<SocketBatchWriter>` 的别名，`SocketBatchWriter::WriteBatch` 决定走 `writev`（TCP）、`sendmmsg`（UDP datagram）还是 `sendfile`（文件）。改批量语义先看这层，不要改 `Writer`。
- **`Flush` 与 `FlushBatch` 不是同义词**：前者强制单次 `Write`（最保守），后者优先走底层批量接口；`kOnFull`/`kHybrid` 自动触发的是后者。
- **段表不变量**：`m_segments` 必须按顺序铺满 `[0, m_n)`。修改 `CopyIn`/`ConsumeFront`/`WriteByte` 的段逻辑时，`FlushBatchImpl` 会检查空段表并回退 `FlushLinear`，但空洞仍会导致漏数据。
- **未完成项**（都是有意留的）：`Peek` 的「预读 + 部分数据」形态、`ReadLine` 的 `isPrefix`、`Writer::AvailableBuffer`、`Scanner` 复用外部缓冲、`ScanWords` 的 Unicode 空白、`Reader` 无 `Seek`；性能侧 P11（每次 Write 取时钟）与 P12（每次 flush 重建 iovec）尚未实施。
- **文档地图**：状态机在 [Reader.md](../../docs/state-machines/Reader.md)（46 行）与 [Scanner.md](../../docs/state-machines/Scanner.md)（39 行）；端到端协议基准在 [2026-10-31-http-perf.md](../../docs/reports/2026-10-31-http-perf.md)；本模块**没有**独立设计文档，深挖只能读头文件注释。
