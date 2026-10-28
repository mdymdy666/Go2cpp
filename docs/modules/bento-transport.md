# bento-transport 模块组：字节传输、分帧、心跳、退避与转发

> **文档状态**：现行（active）
> **日期**：2026-10-28
> **适用基线**：`afc135a`（`git rev-parse --short HEAD` 实测）
> **源码位置**：`include/go2cpp/bento/net/` 下 7 个头文件（1265 行）+ `src/bento/net/` 下 7 个实现（1679 行），合计 2944 行
> **引用约定**：`transport.hpp:146` 指 `include/go2cpp/bento/net/transport.hpp` 第 146 行；`relay.cpp:44` 指 `src/bento/net/relay.cpp` 第 44 行
> **测试目录**：`tests/bento/`；构建目录 `.build-bento-wsl`（CMake 选项 `GO2CPP_WITH_BENTO=ON`）
> **范围**：只覆盖 transport / tcp_transport / memory_transport / frame / heartbeat / backoff / relay 七个模块；`Link`、`ReliableChannel`、`PeerHub`、`crypto` 不属于本文，见第 8 节链接

## 1. 模块清单

| 模块 | 命名空间 | 头文件 | 实现 | 测试文件 | 一句话职责 |
|---|---|---|---|---|---|
| transport | `go2cpp::bento::net` | `transport.hpp` | `transport.cpp` | `test_transport.cpp` | `ITransport` 字节流抽象：连接、带 deadline 读写、半关闭、幂等关闭 |
| tcp_transport | `go2cpp::bento::net` | `tcp_transport.hpp` | `tcp_transport.cpp` | `test_transport.cpp` | 非阻塞 socket + poll 超时的 TCP 实现（监听/accept、IPv4/IPv6） |
| memory_transport | `go2cpp::bento::net` | `memory_transport.hpp` | `memory_transport.cpp` | `test_transport.cpp` | 进程内配对内存管道，**仅测试用**的确定性传输 |
| frame | `go2cpp::bento::net` | `frame.hpp` | `frame.cpp` | `test_frame.cpp`、`test_transport.cpp` | 20B 固定帧头 + CRC32 的编码/解码与流式 `FrameDecoder` |
| heartbeat | `go2cpp::bento`（注意不是 `net`） | `heartbeat.hpp` | `heartbeat.cpp` | `test_heartbeat.cpp` | 心跳节拍 + 单调时钟存活判定 + 一次性超时回调，不碰 socket |
| backoff | `go2cpp::bento`（注意不是 `net`） | `backoff.hpp` | `backoff.cpp` | `test_backoff.cpp` | 指数退避 + 对称抖动，可复现的 splitmix64 随机源 |
| relay | `go2cpp::bento::net` | `relay.hpp` | `relay.cpp` | `test_relay.cpp` | 两条 `ITransport` 间的双向搬运：有界缓冲背压、半关闭、慢客户端与超时 |

## 2. 分层与调用关系

```
上层：Link / ReliableChannel / app::Session      （bento-session.md，不在本文）
                    │ 依赖
                    ▼
        frame（20B 头 + CRC32 + FrameDecoder）    ← 只依赖 hash::Crc32
                    ▼
        relay（双向搬运 + 64KB 有界 pending）     ← 只依赖 ITransport
                    ▼
   ITransport（Connect/Read/Write/ShutdownWrite/Close）
        ▲                                      ▲
  tcp_transport（socket/poll）          memory_transport（deque+condvar，测试）
```

- 依赖方向单向向下：`tcp_transport`/`memory_transport` → `ITransport`；`relay` → `ITransport`；上层 → `frame`/`relay`/`ITransport`。
- **禁止反向依赖**：`ITransport` 及其实现不引用 `frame.hpp`（传输层不解析帧、不做消息边界）；`frame.hpp` 不引用任何传输实现（同一份编解码要能跑在 TCP、内存管道或未来的 TLS 之上）。
- `heartbeat` 与 `backoff` 是**旁路定时组件**：不依赖 `ITransport`、不持有 socket。前者只读时钟并报告「该发心跳了」，后者只算延迟；是否发送/重连由上层事件循环决定（`heartbeat.hpp:27-31`）。

## 3. API 参考

### 3.1 transport：字节流抽象（`transport.hpp`）

| 签名 | 语义与返回值 | 备注 |
|---|---|---|
| `struct Endpoint { std::string host; std::uint16_t port = 0; };` | 主机名或 IP 字面量 + 主机字节序端口 | `Connect` 端口 0 非法；`Listen` 端口 0 由内核分配 |
| `enum class Code : std::uint8_t { kOk = 0, kTimeout = 1, kClosed = 2, kError = 3 };` | 粗粒度结果分类 | 具体原因在 `Result::system_error` |
| `struct Result { Code code = Code::kOk; int system_error = 0; std::size_t bytes = 0; bool ok() const noexcept; explicit operator bool() const noexcept; };` | 返回值而不是异常；`bytes` 只对 Read/Write 有意义 | `ok()` **不代表**写完全部输入 |
| `const char* CodeName(Code code) noexcept;` | 返回 `"Ok"`/`"Timeout"`/`"Closed"`/`"Error"`，未知值 `"Unknown"` | 日志与测试失败信息 |
| `virtual Result Connect(const Endpoint& endpoint, std::chrono::milliseconds timeout) = 0;` | `kOk` 已建连；`kTimeout` 超时；`kError` 解析失败/被拒绝；`kClosed` 实现已关闭 | DNS 解析不占 deadline（`transport.hpp:162`） |
| `virtual Result Read(std::uint8_t* buffer, std::size_t capacity, std::chrono::milliseconds timeout) = 0;` | 读至多 `capacity`；`kOk` 时 `bytes >= 1`；EOF 返回 `kClosed` | `capacity == 0` 是合法空操作 |
| `virtual Result Write(const std::uint8_t* data, std::size_t size, std::chrono::milliseconds timeout) = 0;` | 写至多 `size`；`kOk` 允许部分写 | 写满用 `WriteFull` |
| `virtual Result ShutdownWrite() = 0;` | 半关闭写方向（FIN）；本端 `Write` 之后返回 `kClosed`，`Read` 仍可用 | 重复调用返回 `kOk` |
| `virtual Result Close() = 0;` | 释放连接，始终返回 `kOk` | 幂等；派生类析构必须等价于它 |
| `Result ReadFull(std::uint8_t* buffer, std::size_t size, std::chrono::milliseconds timeout);` | 循环读满；成功 `bytes == size` | 整个循环共享同一 deadline（`transport.cpp:64`） |
| `Result WriteFull(const std::uint8_t* data, std::size_t size, std::chrono::milliseconds timeout);` | 循环写满；部分写入后失败返回对应 `code` 与累计 `bytes` | 非虚便利函数 |

### 3.2 tcp_transport：TCP 实现（`tcp_transport.hpp`）

| 签名 | 语义与返回值 | 备注 |
|---|---|---|
| `Result Listen(const Endpoint& bind_endpoint, int backlog = 64);` | 创建非阻塞监听 socket；`host` 为空表示通配 | `backlog <= 0` 按 64；已打开返回 `kError` + `EALREADY` |
| `Result Accept(TcpTransport* accepted, std::chrono::milliseconds timeout);` | 接受一条连接；`kTimeout` 无连接、`kClosed` 监听已关闭 | 输出对象必须未打开，否则 `kError` + `EALREADY` |
| `Result LocalEndpoint(Endpoint* endpoint) const;` | `getsockname` 数字形式；IPv6 返回 `::1` 这类不带方括号的字面量 | 未打开返回 `kClosed` |
| `bool is_open() const noexcept;` / `bool is_listening() const noexcept;` | fd 是否打开 / 是否由 `Listen` 创建 | 不涉及状态机 |
| `Result Connect/Read/Write/ShutdownWrite/Close` | 覆写 `ITransport`；全非阻塞 + `poll` 超时，`send` 用 `MSG_NOSIGNAL` 规避 SIGPIPE | EINTR 重算剩余时间后重试；EAGAIN 不算错误 |

### 3.3 memory_transport：进程内配对管道（`memory_transport.hpp`）

| 签名 | 语义与返回值 | 备注 |
|---|---|---|
| `using Pair = std::pair<std::unique_ptr<MemoryTransport>, std::unique_ptr<MemoryTransport>>;` | 一对互相连接的端点 | `first` 为 A 侧，`second` 为 B 侧 |
| `static Pair CreatePair();` | 创建已连接的两端 | 唯一构造入口，不能造出「半条连接」 |
| `Result Connect(const Endpoint& endpoint, std::chrono::milliseconds timeout) override;` | 空操作式连接；未关闭返回 `kOk`，已关闭 `kClosed` | 忽略端点内容 |
| `Result Read(std::uint8_t* buffer, std::size_t capacity, std::chrono::milliseconds timeout) override;` | 从本端收件箱读；基于 `condition_variable` 的 deadline 等待 | 数据先于 EOF：对端关闭前写入的字节仍可读 |
| `Result Write(const std::uint8_t* data, std::size_t size, std::chrono::milliseconds timeout) override;` | 追加到对端收件箱；正常总是全写（`bytes == size`） | 队列无上限；半关闭/对端关闭返回 `kClosed`（`EPIPE`/`ENOTCONN`） |
| `Result ShutdownWrite() override;` / `Result Close() override;` | 半关闭 / 幂等关闭；关闭后对端读到 EOF | `Close` 只丢弃本端收件箱，对端仍能读完整字节流（`memory_transport.cpp:217-219`） |

### 3.4 frame：20B 帧编解码（`frame.hpp`）

| 签名 | 语义与返回值 | 备注 |
|---|---|---|
| `inline constexpr std::uint32_t kFrameMagic = 0x42454e54u;` / `inline constexpr std::uint16_t kFrameVersion = 1;` | ASCII `"BENT"` 魔数与帧格式版本 | 版本不符直接拒绝，不做向后兼容 |
| `struct FrameFormat { std::uint32_t magic = kFrameMagic; std::uint16_t version = kFrameVersion; };` | 线上协议标识 | 上层可用自有 magic/version 与 Link 帧区分 |
| `inline constexpr std::size_t kGenericFrameHeaderBytes = 20;` / `inline constexpr std::uint32_t kFrameMaxPayload = 1024u * 1024u;` | 固定帧头 20B；单帧载荷上限 1 MiB | 半包阶段就按声明 length 拒绝 |
| `enum class FrameStatus : std::uint8_t { kOk, kNeedMoreData, kBadMagic, kBadVersion, kBadReserved, kLengthExceeded, kChecksumMismatch, kInvalidArgument };` | 解码结果 | `kOk`/`kNeedMoreData` 是正常路径，其余表示流不可信 |
| `struct Frame { std::uint32_t magic; std::uint16_t version; std::uint16_t type; std::uint16_t flags; std::uint16_t reserved; std::uint32_t checksum; std::vector<std::uint8_t> payload; std::uint32_t Length() const noexcept; };` | 头字段 + 载荷副本；`Length()` 等于线上 length | 成员默认值从略 |
| `std::uint32_t ComputeFrameChecksum(const std::uint8_t* header_prefix, const std::uint8_t* payload, std::size_t payload_bytes) noexcept;` | CRC-32/IEEE，覆盖帧头**前 16 字节** + 载荷 | 不含 checksum 自身，含 length |
| `std::vector<std::uint8_t> Encode(const Frame& frame);` | 完整字节（20B 头 + 载荷）；失败返回空 vector | `reserved != 0` 或载荷超 1 MiB 返回空 |
| `std::vector<std::uint8_t> Encode(const Frame& frame, const FrameFormat& format);` | 指定 magic/version 编码 | `magic == 0` 视为非法返回空 |
| `FrameStatus Decode(const std::uint8_t* data, std::size_t size, Frame* out, std::size_t* consumed = nullptr) noexcept;` | 无状态解析缓冲开头一帧；`consumed` 只在 `kOk` 时写入 | 不修改输入、不吞字节 |
| `FrameStatus Decode(const FrameFormat& format, const std::uint8_t* data, std::size_t size, Frame* out, std::size_t* consumed = nullptr) noexcept;` | 同上，按给定协议标识校验 | magic/version 不符分别 `kBadMagic`/`kBadVersion` |
| `explicit FrameDecoder(std::uint32_t max_payload = kFrameMaxPayload, FrameFormat format = kDefaultFrameFormat) noexcept;` | 流式解码器 | `max_payload == 0` 表示只接受空载荷帧 |
| `void Append(const std::uint8_t* data, std::size_t size);` | 追加任意切片（粘包/半包都由它喂） | 空指针或 0 字节是空操作 |
| `FrameStatus Next(Frame* out);` | 取下一帧；`kNeedMoreData` 表示继续等数据 | 出错时**清空缓冲**（帧边界已不可信） |
| `void Clear() noexcept;` / `std::size_t PendingBytes() const noexcept;` | 丢弃未消费字节并复位 / 返回未消费字节数（含半包） | 错误恢复或重连时用 |
| `std::uint32_t MaxPayload() const noexcept;` / `const FrameFormat& format() const noexcept;` | 读取配置 | |

### 3.5 heartbeat：心跳与超时（`heartbeat.hpp`，命名空间 `go2cpp::bento`）

| 签名 | 语义与返回值 | 备注 |
|---|---|---|
| `static constexpr std::uint32_t kDefaultIntervalMs = 30000;` / `static constexpr std::uint32_t kDefaultTimeoutMs = 90000;` | 默认 30s / 90s（连续丢 3 个心跳） | |
| `std::uint64_t MonotonicNowMs() noexcept;` | `steady_clock` 毫秒 | 不受系统时间回拨影响 |
| `explicit Heartbeat(std::uint32_t interval_ms = kDefaultIntervalMs, std::uint32_t timeout_ms = kDefaultTimeoutMs, NowFn now = nullptr, TimeoutHandler on_timeout = nullptr);` | 0 表示用默认值；`NowFn` 用于注入假时钟 | 构造时立即采样最后活动时间 |
| `void Start(std::uint32_t interval_ms);` / `void Start();` | 启动或重启节拍；最后活动时间、下次心跳、计数与超时状态全部重置 | 参数 0 表示沿用当前间隔 |
| `void Stop() noexcept;` | 停止；之后 `Poll()` 恒返回 false | 不再触发超时回调 |
| `bool Poll() noexcept;` | 推进一次定时器；`true` 表示该发心跳（已推进下次到期） | 超时判定优先；同一时刻只发一次，不补发 |
| `bool IsAlive(std::uint32_t timeout_ms) const noexcept;` / `bool IsAlive() const noexcept;` | 距最后一次活动 `< timeout_ms` | `timeout_ms == 0` 表示立即判死 |
| `void OnReceive() noexcept;` / `void OnReceiveAt(std::uint64_t now_ms) noexcept;` | 记录对端活动并清除「已超时」状态 | 任意消息都算活动，不限于心跳 |
| `void SetOnTimeout(TimeoutHandler handler);` | 覆盖超时回调；在 `Poll()` 内同步调用 | 恢复活动前只触发一次 |
| `void SetInterval(std::uint32_t interval_ms) noexcept;` / `void SetTimeout(std::uint32_t timeout_ms) noexcept;` | 运行期调整；0 恢复默认 | 运行中改间隔会立即重排下次心跳 |
| `std::uint64_t NextBeatMs() const noexcept;` / `LastActivityMs() const noexcept;` / `Beats() const noexcept;` / `TimeoutsFired() const noexcept;` / `std::uint32_t IntervalMs() const noexcept;` / `TimeoutMs() const noexcept;` / `bool IsRunning() const noexcept;` / `bool IsTimedOut() const noexcept;` | 观测：下次心跳、最后活动、心跳/超时次数、间隔、阈值与运行/超时状态 | 计数自最近一次 `Start` 起算 |

### 3.6 backoff：指数退避 + 抖动（`backoff.hpp`，命名空间 `go2cpp::bento`）

| 签名 | 语义与返回值 | 备注 |
|---|---|---|
| `static constexpr std::uint32_t kDefaultBaseMs = 1000;` / `kDefaultMaxMs = 30000;` / `static constexpr double kDefaultJitterRatio = 0.2;` | 默认 base 1s、上限 30s、抖动 ±20% | |
| `struct Config { std::uint32_t base_ms = kDefaultBaseMs; std::uint32_t max_ms = kDefaultMaxMs; double jitter_ratio = kDefaultJitterRatio; std::uint64_t seed = 0x9e3779b97f4a7c15ull; };` | 退避参数 | 构造时规范化：base 0→1；max<base 按 base；jitter 钳到 `[0,1]` |
| `Backoff();` / `explicit Backoff(Config config);` | 默认配置 / 自定义配置构造 | |
| `std::uint32_t Next() noexcept;` | 返回本次延迟并推进尝试计数，范围 `[1, max_ms]` | 第 n 次名义延迟 `base * 2^n`，封顶 max，再叠对称抖动 |
| `void Reset() noexcept;` | 尝试计数清零，随机源复位到构造时的种子 | 相同 seed → 相同序列 |
| `std::uint32_t Attempts() const noexcept;` | 已调用 `Next()` 的次数 | 不限制次数，是否放弃由调用方决定 |
| `std::uint32_t PeekNominalMs() const noexcept;` | 下一次的名义延迟（不含抖动、已封顶） | 日志/观测用 |
| `std::uint32_t BaseMs() const noexcept;` / `MaxMs() const noexcept;` / `double JitterRatio() const noexcept;` | 规范化后的配置读取 | |

### 3.7 relay：双向字节中继（`relay.hpp`）

| 签名 | 语义与返回值 | 备注 |
|---|---|---|
| `Relay() = default;` | 构造 | **没有 `Start`/`Stop`**：生命周期由阻塞调用与调用线程决定 |
| `RelayResult Run(ITransport& src, ITransport& dst, const RelayOptions& options = RelayOptions{});` | 在两条已连接传输间双向搬运，直到正常结束或触发断开条件 | 返回前会 `Close()` 两条传输 |
| `RelayResult ConnectAndRun(ITransport& src, ITransport& dst, const Endpoint& destination, const RelayOptions& options = RelayOptions{});` | 先拨号 `dst` 再执行 `Run` | 拨号失败返回 `kConnectTimeout`/`kConnectFailed` 并关闭两端 |
| `RelayStats Stats() const noexcept;` | 本实例历次运行的累计统计（拷贝返回） | 峰值字段取最大值，不能相加 |
| `struct RelayOptions { std::size_t buffer_bytes = 64 * 1024; std::size_t read_chunk_bytes = 16 * 1024; std::uint32_t read_slice_ms = 0; std::uint32_t write_slice_ms = 5; std::uint32_t poll_interval_ms = 1; std::uint32_t write_timeout_ms = 2000; std::uint32_t idle_timeout_ms = 30000; std::uint32_t connect_timeout_ms = 3000; };` | 每方向 pending 容量、单次读写切片、慢客户端/空闲/拨号超时 | 超时字段 0 = 关闭该超时；切片字段 0 = 非阻塞调用一次 |
| `struct RelayStats { std::uint64_t bytes_src_to_dst; std::uint64_t bytes_dst_to_src; std::uint64_t read_src_to_dst; std::uint64_t read_dst_to_src; std::uint64_t peak_pending_src_to_dst; std::uint64_t peak_pending_dst_to_src; std::uint64_t peak_buffered_bytes; std::uint64_t half_closes; std::uint64_t slow_consumers; std::uint64_t idle_timeouts; std::uint64_t connect_timeouts; std::uint64_t connect_failures; std::uint64_t read_errors; std::uint64_t write_errors; };` | 双向字节数、pending 峰值、半关闭与各类断开计数 | `bytes_*` 是成功写出量，`read_*` 是读入量 |
| `RelayStats& operator+=(const RelayStats& other) noexcept;` | 累加；峰值取 max、其余相加 | |
| `enum class RelayCloseReason : std::uint8_t { kCompleted, kSlowConsumer, kIdleTimeout, kConnectTimeout, kConnectFailed, kReadError, kWriteError };` | 中继结束原因 | 只有 `kCompleted` 是正常结束 |
| `struct RelayResult { RelayCloseReason reason; RelayStats stats; std::uint64_t elapsed_ms; int system_error; bool completed() const noexcept; };` | 一次运行的结果；`elapsed_ms` 在 `ConnectAndRun` 中含拨号耗时 | |
| `const char* RelayCloseReasonName(RelayCloseReason reason) noexcept;` | 返回 `"Completed"`/`"SlowConsumer"` 等 | 未知值 `"Unknown"` |

## 4. 设计要点

1. **deadline 是整次调用的预算，不是每次等待**：`Read`/`Write` 内部循环（EAGAIN/EINTR）必须扣除已花时间，上层才能给一次完整操作一个上界（`transport.hpp:22-27`）。
2. **传输层只保证有序字节，不保留消息边界**：TCP 与内存管道刻意语义一致，分帧交给 `frame.hpp`，传输层不做任何解析（`transport.hpp:16-20`）。
3. **半关闭与完全关闭分离**：`ShutdownWrite()` 只发 FIN，本端仍可 `Read`；请求-响应协议若读到 EOF 就 `Close`，会截断响应（`transport.hpp:42-47`、`relay.hpp:54-65`）。`Close()` 幂等且始终 `kOk`。
4. **允许部分写**：流式传输的 `Write` 不保证写完，调用方要么自己循环，要么用 `WriteFull`；这也是实现背压的基础（`transport.hpp:38-40`）。
5. **CRC 覆盖「头前 16B + 载荷」，且把 length 纳入校验**：篡改长度会被校验和发现，而不是悄悄改变后续帧边界；不覆盖 checksum 自身（`frame.hpp:20-22`）。
6. **心跳用单调时钟 + 可注入假时钟，且没有后台线程**：`Poll()` 是唯一驱动点，测试用假时钟推进 30s/90s 而不用真等待（`heartbeat.hpp:22-31`）。
7. **指数退避必须带抖动且封顶**：抖动打散多客户端同时断线后的重连风暴，上限保证不会「越退越久」直到永不重连；抖动后仍钳制在 `max_ms` 内（`backoff.hpp:7-20`）。
8. **relay 的有界队列就是背压**：pending 满时停止从源读，慢客户端最多多占 `buffer_bytes`；慢客户端判据是「连续 `write_timeout_ms` 无写进展」，而不是某一次 `Write` 超时，慢但仍在动的客户端不会被误杀（`relay.hpp:33-52`）。

## 5. 边界与已知限制

- **帧上限**：载荷 ≤ `kFrameMaxPayload`（1 MiB），头固定 20B，`reserved` 必须为 0；两个自由函数 `Decode` 固定用 1 MiB 上限，只有 `FrameDecoder` 能自定义 `max_payload`（传 0 表示只收空载荷帧）。
- **relay 默认常量**：每方向 `64 KB` pending、单次读 `16 KB`、写切片 `5 ms`、空转休眠 `1 ms`、慢客户端 `2000 ms`、空闲 `30000 ms`、拨号 `3000 ms`；`buffer_bytes == 0` 会被规范化为 64 KB，`read_chunk_bytes` 超过容量时按容量处理（`relay.cpp:44-53`）。
- **错误码映射**：`ECONNRESET`/`EPIPE`/`ENOTCONN`/`ECONNABORTED`/`ESHUTDOWN` → `kClosed`；`ETIMEDOUT` → `kTimeout`；其余归 `kError` 并保留原始 errno（`transport.hpp:61-66`）。
- **线程模型**：接口本身不串行化。`MemoryTransport` 一对端点可跨线程读写；`TcpTransport` 的 `Read`/`Write` 通常可并发，但 `Close` 与其它操作并发需调用方协调；`FrameDecoder`、`Heartbeat`、`Backoff`、`Relay` 均**非线程安全**，一条连接一个实例。
- **DNS 不占 deadline**：`getaddrinfo` 没有超时参数，解析阶段不受 `timeout` 约束；需要硬上界时传 IP 字面量。
- **未实现/不提供**：除 TCP 与内存管道外的传输（WebSocket/TLS 只是接口预留，未实现）；`Relay` 没有 `Start`/`Stop`；`Heartbeat` 不创建后台线程，不调用 `Poll()` 就不会有任何超时判定。
- **仅测试用**：`MemoryTransport` 队列无上限，写入方快于读取方时会持续占用内存，不可在生产路径替代真实传输（`memory_transport.hpp:17-22`）。

## 6. 测试数据

| 测试文件 | 用例/断言 | 覆盖点 |
|---|---|---|
| `test_transport.cpp` | 19 个 `[CASE]`、162 处 `CHECK(`（实测）；`bento-modules.md` 记为 19 用例/348 断言 | 内存管道双向 100 KiB、半关闭、超时、幂等关闭、部分读；TCP 回环 100 KiB、accept/读超时、FIN 不杀响应、多连接隔离、连接拒绝、IPv6 回环（不支持时跳过）；帧往返/粘包/半包/非法头/超长/校验失败 |
| `test_frame.cpp` | 10 个测试函数、27 处 `CHECK(`（实测）；`bento-modules.md` 记为 12 用例/5511 断言（含 200 轮 fuzz） | 往返、粘包、逐字节半包、坏 magic、版本/reserved、长度超限、CRC 失败、空载荷、截断头/参数、随机流 fuzz |
| `test_heartbeat.cpp` | 7 个测试函数、40 处 `CHECK(`（实测）；文档记 7/67 | 默认值与单调时钟、正常节拍、超时只触发一次、`Stop`、`IsAlive` 边界、重启与运行期改配置、真实时钟冒烟 |
| `test_backoff.cpp` | 7 个测试函数、9 处 `CHECK(`（实测）；文档记 7/565 | 倍增序列、上限+抖动、抖动区间、确定性种子、`Reset`、参数规范化、`PeekNominalMs` |
| `test_relay.cpp` | 13 个测试函数、56 处 `CHECK(`（实测）；文档记 13/80 | TCP 双向 100KB、半关闭传播、慢客户端、有界缓冲背压（pending 峰值 ≤ buffer_bytes）、空流、空闲/拨号超时、拨号失败、随机分块、写错误、极小缓冲、吞吐 |
| `test_stream_fragment.cpp` | 44 处 `CHECK(` | **按相关度取舍**：测的是 Link 的 STREAM >1300B 分片（`channel.hpp`/`link.hpp`/`protocol.hpp`），不是 `frame.hpp` 的直接测试 |
| `test_ipv6.cpp` | 16 处 `CHECK(` | 同样走 `link.hpp`（UDP/IPv6）；`TcpTransport` 的 IPv6 覆盖在 `test_transport.cpp` 的 `tcp: IPv6 loopback` 用例 |
| `test_conn_migration.cpp` | 23 处 `CHECK(` | Link 层的 conn_id 迁移与会话续接，不属于本模块组 |

运行命令（在构建目录 `.build-bento-wsl` 内，ctest 名与 `CTestTestfile.cmake` 实测一致）：

```bash
cd /UserData/CodexWorkSpace/Go2cpp/.build-bento-wsl
ctest -R '^bento_' --output-on-failure
ctest -R '^bento_(transport|frame|heartbeat|backoff|relay|stream_fragment|ipv6|conn_migration)$' --output-on-failure
```

> 本文编写期间未重新执行 ctest。「已验证」的结论只来自仓库文档 `bento-modules.md` 的既有记录：`ctest -R bento` 48 项（47 通过 + `bento_netem` SKIP，2026-10-28 实测，见 `codex.md` §28；app 层构建另有 59/59 记录，§27）。表中「实测」数字是本次用 `grep -c 'CHECK('` 统计的**源码断言行数**，与运行期断言计数不同（循环与 fuzz 会把一行断言执行多次）。

## 7. 示例

### 示例 1：TCP 传输 + 帧编解码 + 心跳（单线程，已实测编译运行）

```cpp
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "go2cpp/bento/net/frame.hpp"
#include "go2cpp/bento/net/heartbeat.hpp"
#include "go2cpp/bento/net/tcp_transport.hpp"

int main() {
    using namespace go2cpp::bento::net;
    using ms = std::chrono::milliseconds;

    TcpTransport listener;
    if (!listener.Listen({"127.0.0.1", 0}).ok()) return 1;
    Endpoint local;
    if (!listener.LocalEndpoint(&local).ok()) return 1;

    // 同一线程即可：connect 先进入 listen backlog，随后 Accept 取回。
    TcpTransport client;
    if (!client.Connect(local, ms(2000)).ok()) return 1;
    TcpTransport server;
    if (!listener.Accept(&server, ms(2000)).ok()) return 1;

    Frame request;
    request.type = 7;
    request.payload = {0x10, 0x20, 0x30};
    const std::vector<std::uint8_t> wire = Encode(request);
    if (wire.empty() || !client.WriteFull(wire.data(), wire.size(), ms(2000)).ok()) return 1;

    FrameDecoder decoder;
    Frame received;
    FrameStatus status = FrameStatus::kNeedMoreData;
    std::uint8_t buffer[32];
    while (status == FrameStatus::kNeedMoreData) {
        const Result read = server.Read(buffer, sizeof(buffer), ms(2000));
        if (read.code != Code::kOk) break;
        decoder.Append(buffer, read.bytes);
        status = decoder.Next(&received);
    }
    if (status != FrameStatus::kOk) return 1;

    go2cpp::bento::Heartbeat heartbeat(30000, 90000);
    heartbeat.SetOnTimeout([] { std::printf("peer timed out\n"); });
    heartbeat.Start();
    heartbeat.OnReceive();
    std::printf("frame type=%u payload=%u alive=%d\n", received.type, received.Length(),
                heartbeat.IsAlive() ? 1 : 0);

    (void)client.ShutdownWrite();
    (void)client.Close();
    (void)server.Close();
    (void)listener.Close();
    return 0;
}
```

编译与运行（本次在 WSL 内实测通过，输出 `frame type=7 payload=3 alive=1`）：

```bash
g++ -std=c++20 -Wall -Wextra -I include -o /tmp/demo1 demo_tcp_frame.cpp \
    src/bento/net/frame.cpp src/bento/net/tcp_transport.cpp src/bento/net/transport.cpp \
    src/bento/net/heartbeat.cpp src/hash/crc32.cpp -pthread
/tmp/demo1
```

### 示例 2：Relay 半关闭转发 + 退避（两条内存管道，已实测）

```cpp
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

#include "go2cpp/bento/net/backoff.hpp"
#include "go2cpp/bento/net/memory_transport.hpp"
#include "go2cpp/bento/net/relay.hpp"

int main() {
    using namespace go2cpp::bento::net;
    using ms = std::chrono::milliseconds;
    go2cpp::bento::Backoff backoff;

    MemoryTransport::Pair inbound = MemoryTransport::CreatePair();   // 客户端 ↔ 中继入站
    MemoryTransport::Pair outbound = MemoryTransport::CreatePair();  // 中继出站 ↔ 上游

    std::thread client([&] {
        const std::vector<std::uint8_t> request{1, 2, 3, 4};
        (void)inbound.first->WriteFull(request.data(), request.size(), ms(2000));
        (void)inbound.first->ShutdownWrite();  // 只关写方向，仍能读响应
        std::uint8_t response[8] = {};
        const Result read = inbound.first->ReadFull(response, 3, ms(2000));
        std::printf("client got %zu bytes code=%s\n", read.bytes, CodeName(read.code));
    });

    std::thread upstream([&] {
        std::uint8_t request[8] = {};
        (void)outbound.second->ReadFull(request, 4, ms(2000));
        const std::vector<std::uint8_t> response{9, 8, 7};
        (void)outbound.second->WriteFull(response.data(), response.size(), ms(2000));
        (void)outbound.second->ShutdownWrite();
    });

    Relay relay;
    RelayOptions options;
    options.idle_timeout_ms = 5000;
    const RelayResult result = relay.Run(*inbound.second, *outbound.first, options);
    client.join();
    upstream.join();

    std::printf("relay reason=%s s2d=%llu d2s=%llu\n", RelayCloseReasonName(result.reason),
                static_cast<unsigned long long>(result.stats.bytes_src_to_dst),
                static_cast<unsigned long long>(result.stats.bytes_dst_to_src));
    const unsigned first = backoff.Next();
    const unsigned second = backoff.Next();
    backoff.Reset();
    std::printf("backoff %u then %u ms; reset attempts=%u nominal=%u\n", first, second,
                backoff.Attempts(), backoff.PeekNominalMs());
    return 0;
}
```

编译（实测运行输出 `relay reason=Completed s2d=4 d2s=3` 与两行退避结果，默认种子可复现）：

```bash
g++ -std=c++20 -Wall -Wextra -I include -o /tmp/demo2 demo_relay.cpp \
    src/bento/net/relay.cpp src/bento/net/memory_transport.cpp src/bento/net/transport.cpp \
    src/bento/net/backoff.cpp -pthread
```

## 8. 相关文档

- [bento-session.md](bento-session.md)：`Link`、`ReliableChannel`、`PeerHub` 与会话层——本组模块的上层消费者
- [bento-security.md](bento-security.md)：`crypto`（ChaCha20-Poly1305）、`x25519`、重放窗口与安全传输
- [bento-foundation.md](bento-foundation.md)：`hash::Crc32`、错误/反射等基础组件（`frame` 的 CRC 依赖）
- [bento-scenarios.md](bento-scenarios.md)：端到端场景与验证口径
- [PROTOCOL.md](../../tests/bento/PROTOCOL.md)：Bento 线上协议与帧格式的测试侧约定
