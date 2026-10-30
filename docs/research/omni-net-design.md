# OmniNetServer 架构设计

> 文档性质：**设计稿**。只写架构，不含实现；本轮不改任何源码、不 commit。
> 日期：2026-10-30 · 工作树：`/mnt/e/CodexWorkspace/Go2Cpp`（WSL）· 分支：`feature/gmp-debug-module`
> 范围：`go2cpp::bento::omni`（新命名空间）如何把 Bento 与 `go2cpp::net` 组合成
> 一个可配置、可嗅探、三种并发模型的服务端门面。
> 配套：[net-scenarios.md](net-scenarios.md)（22 个网络场景的广度调研与 Bento 边界）、
> [../modules/network.md](../modules/network.md)（标准协议栈现状）、
> [../../tests/bento/PROTOCOL.md](../../tests/bento/PROTOCOL.md)（Bento v0.1 线格式）。

---

## 0. 结论先行

**一句话**：OmniNetServer 不是"一个支持所有协议的服务器"，而是**把「传输 / 协议 / 并发」三件事拆成可替换部件，再用 `Channel<T>` 把业务与网络解耦的组合式门面**。

五条关键决策：

1. **组合，不继承**。`NetServer` 是工厂 + 生命周期 + 统计的持有者；`AsHttpServer()` 等五个方法是**类型化视图**，返回五个互相独立的 `final` 类，不共享 `INetServer` 基类，也不继承 `NetServer`。
2. **`Channel<T>` 是唯一收发面**。服务器不回调业务、不内建路由表；每个连接一对类型化通道，连接建立/关闭由通道生命周期表达。统一的是**机制**，不是消息类型（不做 `variant<所有协议>` 的巨通道）。
3. **`mode` 与 `concurrency` 正交**。`mode` 决定 API 语义（同步阻塞 vs 非阻塞 + 轮询），`concurrency` 决定谁来驱动（Fiber / 线程 / 调用方循环）。二者有合法组合矩阵，`Blocking + Poll` 在 `Create()` 阶段就被拒绝。
4. **嗅探只在连接首包发生一次**，分类后协议终身固定；不做 in-band 协议切换（避免降级与解析器状态混用）。单协议配置走快速路径。
5. **分阶段落地**：LinkServer（复用 Bento，最短闭环）→ HTTP/SSE（复用 base）→ WS/Raw（复用握手与分帧）→ TLS 服务端与硬化。每阶段都有退出条件，不追求一次做完。

三个**明确不做**：

- 不做"单一巨虚类"式的万能 Server（`virtual HandleHttp/HandleWs/...` 那种）；
- 不在运行时切换 fd 的阻塞模式（理由见 §6.1，这是硬约束而非偷懒）；
- 不把 relay / 集群 / 服务发现塞进 OmniNetServer（属于后续独立层）。

---

## 1. 现状盘点

### 1.1 Bento：已有什么，没有什么

Bento 当前只有 4 个头文件 + 4 个实现（约 2.4k 行），全部在 `include/go2cpp/bento/net/` 与 `src/bento/net/`：

| 组件 | 文件 | 能力 | 与 OmniNetServer 的关系 |
|---|---|---|---|
| 线格式 | `bento/net/protocol.hpp` | 12B 帧头、7 种帧类型、可选 8B ACK 段、1300B 载荷上限、`BuildFrame/ParseFrame` | LinkServer 的编解码底座；嗅探时用帧头 + `BENT` magic 识别 |
| 三类通道 | `bento/net/channel.hpp` | `ReliableChannel`（窗口 256、PTO 200ms→2s、累积 ACK + 32 位 SACK）、`UnreliableChannel`（DropStale）、`VideoChannel`（1200B 分片、4 路重组、1s 超时） | 直接复用；OmniNetServer 只做"消息 ↔ 通道"的映射与反压 |
| 因果原语 | `bento/net/relation.hpp` | `dep_seq` 挂起、`BARRIER` 作废、pending 上限 128 | 原样透传，不改变语义 |
| UDP 链路 | `bento/net/link.hpp` | 单 peer 状态机、HELLO 握手、心跳、超时、`RunOnce/RunFor` 事件循环、单线程对象 | LinkServer 的核心；**单 Link 单 peer** 是 M1 的硬边界 |

**需要纠正一个常见前提**：`transport / frame / relay / session` 常被当作 Bento 的现有能力。按当前工作树的事实，`frame` 对应 `bento/net/protocol.hpp` 的帧编解码，`channel/link` 已存在；而 **`transport`（可替换传输抽象）、`relay`（中转/配对）、`session`（跨连接会话）三个组件尚未出现**。`relay` 的雏形只存在于路线图的 `TokenPairing`（`(key, token)` 1:N 管道配对）计划中，尚未落代码。本文把这三层当作**待建层**处理，不假设它们已经可用，也不在本期设计里硬塞。

另有一条架构事实必须写进设计前提：**`Link` 是单线程对象**（`bento/net/link.hpp:5-9` 明确"所有公开方法与回调都必须在同一个线程调用，不做内部加锁"），且监听方在 `HandleHello` 里**锁定第一个 HELLO 来源**，其余来源计入 `unknown_peer_frames`（`src/bento/net/link.cpp:336-355`）。因此"一个 LinkServer 服务 N 个客户端"不是配置问题，而是要先改 Link 的 socket 归属与 peer 路由——这属于 M4 之后的独立设计，不能靠"多开几个 Channel"绕过去。

### 1.2 base：可复用的标准栈

`go2cpp::net` 约 18.7k 行，本次直接复用其中这些组件：

| 能力 | 入口 | 现状要点（与设计相关） |
|---|---|---|
| Socket | `net::Socket`（`network/socket.hpp:48`） | TCP/UDP/Unix 的 RAII；**创建即非阻塞**（`src/network/socket.cpp:156-158`），`accept4` 带 `SOCK_CLOEXEC \| SOCK_NONBLOCK`（`src/network/socket.cpp:222-223`）；`close` 等待在途操作、失败时延迟关闭 |
| 就绪等待 | `io::IOManager`（`network/io.hpp:210`） | 内置 Scheduler + epoll；`go()` 提交 Fiber、`wait/wait_for` park Fiber；普通线程退化为 100ms 分片 poll |
| 可替换后端 | `io::IOBackend`（`network/io_backend.hpp:26`） | epoll/kqueue/IOCP/io_uring 的注册表；默认 Linux=epoll |
| fd 元数据 | `net::FdManager`（`network/fd_manager.hpp:49`） | 只记录 `user_nonblock/system_nonblock` 与超时，**不拥有 fd**；读侧无锁 |
| 分帧 | `net::Framer`（`network/framing.hpp:22`） | 分隔符 / 定长 / 大端 32 位长度前缀，失败即进失败态 |
| HTTP/1 | `http::Parser` / `ResponseWriter` / `Server`（`network/http.hpp:106/189/215`） | Parser 支持半包、chunked、trailer；Writer 只做 chunked 流式；`Server` 是**单连接串行核心**，不设 recv/write 超时 |
| HTTP/2 | `net::http2::Connection`（`network/http2/connection.hpp:36`）+ `Transport`（`network/http2/transport.hpp:28`） | 协议逻辑完整，但**协议核心尚未进库目标**（`docs/modules/network.md:57-61`）；本期不纳入 protocols |
| TLS | `net::http2::TlsTransport`（`network/http2/tls_transport.hpp:86`） | **只有客户端**，`network/http2/tls_transport.hpp:49` 明确"无服务端 TLS 与会话复用" |
| WebSocket | `net::WebSocket`（`network/websocket.hpp:81`） | RFC 6455 握手/帧/分片/Ping-Pong/Close；`AcceptWithPending` 可接管 HTTP Parser 已读入的剩余字节（`network/websocket.hpp:94-97`） |
| SSE | `sse::Stream` / `sse::Parser`（`network/sse.hpp:94/143`） | 复用 `ResponseWriter`；心跳需显式 `tick()`；不建无界队列 |
| QUIC/HTTP3 | `quic::PicoquicConnection` / `http3::Client` | 默认不编译，不在本期范围 |

同时要记住 base 的三个"没有"（`docs/modules/network.md:107` 的 Listener 说明、`:285` 的边界第 5 条）：

- **没有独立 Listener 类**：监听就是 `bind + listen`，accept 循环由调用方负责；
- **没有连接池 / 路由 / 限流抽象**：`http::Server` 只是单连接串行核心；
- **没有"阻塞 fd"路径**：`Socket` 恒非阻塞，所谓"阻塞"只是调用方看到等待语义。

### 1.3 缺口清单（本设计要补的七件事）

| # | 缺口 | 现状 | OmniNetServer 的补法 |
|---|---|---|---|
| G1 | 统一的服务端生命周期 | 每处自己 `ListenTCP` + `accept` + 线程/Fiber | `NetServer::Start/Shutdown` + 连接表 |
| G2 | 协议与非连接的并发解耦 | `http::Server::serve_connection` 把解析、路由、写响应绑死 | 协议解析器只做"字节 ↔ 消息"，业务只碰 `Channel<T>` |
| G3 | 同端口多协议 | 无 | 首包嗅探 + 协议路由（§3） |
| G4 | 并发模型抽象 | 只有"Fiber 或普通线程"的隐式选择 | `ConcurrencyKind` + 三个 Executor |
| G5 | 背压与上限 | 散落在各协议的 timeout / 无队列 | `Limits` + 有界 Channel + 每连接预算 |
| G6 | 统一观测 | 各模块自己的 `Stats` | `NetStats` 聚合 + `DebugSnapshot` |
| G7 | 关闭语义 | 各协议各自为政（半关闭/Close 帧/终止 chunk） | `CloseMode` + 连接状态机（§6.3） |

### 1.4 可复用资产矩阵

| 部件 | M0 | M1 | M2 | M3 | M4 |
|---|---|---|---|---|---|
|---|---|---|---|---|---|
| `bento::Link` | — | ● 核心 | — | — | ○ 多 peer |
| `bento::Reliable/Unreliable/VideoChannel` | — | ● 透传 | — | — | — |
| `net::Socket` + `IOManager` | ○ 骨架 | ● UDP | ● TCP | ● TCP | ● TLS |
| `http::Parser/ResponseWriter` | — | — | ● | ● 升级 | ● |
| `sse::Stream` | — | — | ● | — | ● |
| `net::WebSocket` | — | — | — | ● | ● |
| `net::Framer` | ○ Raw | — | — | ● Raw | ● |
| OpenSSL | — | — | — | — | ● 服务端 TLS |

（● 主要使用；○ 次要使用；— 不使用。）

---

## 2. OmniNetServer 设计

### 2.1 设计原则

- **P1 组合优于继承**：`NetServer` 只持有配置、传输端点、执行者、连接表；协议行为放在五个独立视图与内部 `ProtocolTraits` 里。允许窄接口（`IExecutor`、`IEndpoint`），禁止宽接口（`INetServer` 巨类）。
- **P2 Channel 是边界**：解析器/连接循环与业务之间只有 `Channel<T>`。业务代码不接触 `Socket`、不接触 `epoll`、不接触回调线程。
- **P3 配置即契约**：`OmniNetConfig` 在 `Create()` 一次性校验；运行期不可变（除显式 `Reconfigure` 不提供）。非法组合在启动前失败，不留运行期"半支持"状态。
- **P4 只承诺已实现的协议**：`protocols` 里列了但当前阶段未实现的协议，`Create()` 返回 `kUnsupported`，不做"先跑起来再说"。
- **P5 单驱动者**：一个连接在任一时刻只有一个读驱动者与一个写驱动者；多驱动者（读写分离到两个 Fiber）作为 M4 的可选优化，不进入首版不变量。

### 2.2 配置：`OmniNetConfig`

```cpp
namespace go2cpp::bento::omni {

enum class IoMode : std::uint8_t {
    kBlocking = 0,     ///< 视图提供同步阻塞语义的 Recv/Send（Fiber 中 park，线程中 poll）
    kNonBlocking = 1,  ///< 只提供非阻塞 Channel 与 PollOnce，不存在"等到有数据"的调用
};

enum class TransportKind : std::uint8_t {
    kTcp = 0,   ///< 流式，支持 HTTP/SSE/WS/Raw
    kUdp = 1,   ///< 数据报，只支持 Link/Raw(datagram)
    kTls = 2,   ///< 流式 + TLS；服务端 TLS 在 M4 前返回 kUnsupported
    kMemory = 3 ///< 无 fd，成对内存端点，用于单测/回放
};

enum class ProtocolKind : std::uint8_t {
    kHttp = 0,  ///< HTTP/1.1（不含 HTTP/2）
    kSse = 1,   ///< text/event-stream，建立在 HTTP/1.1 chunked 上
    kWs = 2,    ///< RFC 6455，建立在 HTTP Upgrade 上
    kLink = 3,  ///< Bento 帧协议（UDP）
    kRaw = 4,   ///< 不做协议解释，按 Framer 规则分帧后原样交付
};

enum class ConcurrencyKind : std::uint8_t {
    kGmp = 0,   ///< io::IOManager 的 Fiber
    kThread = 1,///< 固定大小线程池
    kPoll = 2,  ///< 调用方线程，NetServer 不建任何线程
};

struct Limits {
    std::size_t max_connections{1024};          ///< 总连接上限，accept 超限直接关闭并计数
    std::size_t max_connections_per_peer{64};   ///< 单远端 IP 上限，防单机打满
    std::size_t inbound_capacity{256};          ///< 每连接收件通道容量（条数）
    std::size_t outbound_capacity{256};         ///< 每连接发件通道容量（条数）
    std::size_t max_message_bytes{1U << 20};    ///< 单条业务消息上限（1 MiB）
    std::size_t read_buffer_bytes{64U << 10};   ///< 每连接读缓冲（64 KiB）
    std::size_t per_connection_budget_bytes{2U << 20}; ///< 每连接内存预算，超限先断开
    std::size_t max_accept_per_tick{64};        ///< 单轮 accept 上限，防饿死读事件
    std::size_t max_requests_per_connection{100}; ///< HTTP keep-alive 单连接请求上限
    std::size_t http_max_header_bytes{64U << 10}; ///< 覆盖 http::ParserLimits 的默认头上限
    std::uint32_t worker_threads{0};            ///< 0 = hardware_concurrency
    std::chrono::milliseconds accept_timeout{1000};
    std::chrono::milliseconds idle_timeout{60000};   ///< 连接空闲上限（0 = 不超时）
    std::chrono::milliseconds write_timeout{30000};  ///< 单次写上限，慢消费者靠它兜底
    std::chrono::milliseconds close_timeout{5000};   ///< 优雅关闭排空上限
};

struct SniffConfig {
    bool enabled{true};                    ///< protocols.size()==1 时可关（快速路径）
    std::size_t max_bytes{4096};           ///< 首包最多攒多少字节用于识别
    std::chrono::milliseconds timeout{2000}; ///< 攒不满时的等待上限
    bool fallback_to_raw{false};           ///< 识别失败是否降级为 Raw（默认关闭）
};

struct TlsConfig {
    std::string cert_file;   ///< PEM 证书链（M4）
    std::string key_file;    ///< PEM 私钥（M4）
    std::string ca_file;     ///< 客户端证书校验用（可选）
    bool verify_client{false};
    std::vector<std::string> alpn{"http/1.1"};
};

struct OmniNetConfig {
    IoMode mode{ IoMode::kBlocking };
    TransportKind transport{ TransportKind::kTcp };
    std::vector<ProtocolKind> protocols{ ProtocolKind::kHttp };
    ConcurrencyKind concurrency{ ConcurrencyKind::kGmp };

    std::string bind_address{ "127.0.0.1" };
    std::uint16_t bind_port{ 0 };      ///< 0 = 内核分配，测试常用

    Limits limits;
    SniffConfig sniff;
    TlsConfig tls;
    std::string sse_path{ "/events" }; ///< SSE 订阅路径；Accept 头优先，空串表示不按路径匹配
    std::string name{ "omni" };        ///< 日志/统计前缀，便于多实例区分
};

}  // namespace go2cpp::bento::omni
```

**字段语义的三条纪律**：

1. `mode` 只描述**调用方看到的 API 语义**，不描述 fd。base 的 `Socket` 永远是非阻塞的，"Blocking" 指的是视图的 `Recv/Send` 会等到有结果才返回（在 Fiber 里 park、在线程里 poll），不是 `fcntl` 去掉 `O_NONBLOCK`。
2. `protocols` 是**允许集合**，不是按顺序尝试的优先级列表。同端口多协议时由嗅探决定；只有一个协议时跳过**分类**（WS/SSE 例外，它们仍要先解析 HTTP 头，见 §3.4）。
3. `limits` 全部是硬上限：超限的动作必须是"可观测的拒绝"（关闭连接 / 返回 `kBackpressure` / 计数），不允许静默截断。

**Transport × Protocol 兼容矩阵**（`Create()` 校验，非法返回 `kUnsupportedCombination`）：

| | HTTP | SSE | WS | Link | Raw |
|---|---|---|---|---|---|
| TCP | ✅ | ✅ | ✅ | ❌（Link 依赖数据报语义） | ✅ |
| UDP | ❌ | ❌ | ❌ | ✅ | ✅（每数据报一帧） |
| TLS | ✅（M4） | ✅（M4） | ✅（M4） | ❌ | ✅（M4） |
| Memory | ✅ | ✅ | ✅ | ✅（单测） | ✅ |

**Mode × Concurrency 合法矩阵**：

| | Gmp | Thread | Poll |
|---|---|---|---|
| Blocking | ✅ 默认 | ✅ | ❌ `kUnsupportedCombination` |
| NonBlocking | ✅ | ✅ | ✅ |

`Blocking + Poll` 被拒的理由：Poll 模式下所有工作都在调用方的 `PollOnce` 里推进，一个"等到有数据"的同步调用会把用户事件循环卡死，且没有第二个执行者能推进网络——这是设计层面的死锁，不是性能问题。

**`Create()` 的校验顺序**（固定顺序，保证错误信息稳定、测试可断言）：

| # | 校验 | 失败返回 |
|---|---|---|
| V1 | 枚举值在已知范围内（`default:` 分支，防新增枚举未处理） | `kInvalidConfig` |
| V2 | `protocols` 非空、无重复 | `kInvalidConfig` |
| V3 | `transport × protocols` 命中兼容矩阵 | `kUnsupportedCombination` |
| V4 | `mode × concurrency` 命中合法矩阵 | `kUnsupportedCombination` |
| V5 | `kTls` 的证书/私钥非空（M4 前 `kTls` 一律 `kUnsupported`） | `kUnsupported` / `kInvalidConfig` |
| V6 | `limits` 关系：容量 > 0、`per_connection_budget_bytes ≥ max_message_bytes`、`read_buffer_bytes ≤ budget`、`close_timeout ≤ idle_timeout`（若 idle 非 0） | `kInvalidConfig` |
| V7 | `sniff` 关系：多协议时 `enabled=true`；`max_bytes ≥ 16` 且 `≤ http_max_header_bytes` | `kInvalidConfig` |
| V8 | `sse_path` 为空或以 `/` 开头 | `kInvalidConfig` |
| V9 | `bind_address` 可解析（IPv4/IPv6 字面量；域名解析失败留给 `Start()` 报 `kBindFailed`） | `kInvalidConfig` |

校验函数 `ValidateConfig(const OmniNetConfig&)` 是纯函数，不碰 fd、不建线程，可直接单测全部非法组合。

### 2.3 核心类型

不做 `variant<所有协议>`。**每种协议有自己的收/发类型**，统一的是"每条消息带元信息 + 走 `Channel`"这一机制（下文 `ChannelPtr<T>` 是 `std::shared_ptr<Channel<T>>` 的简写，与 `Channel<T>::Ptr` 等价）：

```cpp
/// 连接代号：进程内单调递增，永不复用（fd 会复用，conn id 不会）。
struct ConnectionId final {
    std::uint64_t value{0};
    bool valid() const noexcept { return value != 0; }
    friend bool operator==(ConnectionId, ConnectionId) noexcept;
};

/// 所有入站消息的公共元信息；payload 由各协议类型自带。
struct MessageMeta final {
    ConnectionId connection;
    ProtocolKind protocol{ProtocolKind::kRaw};
    std::uint64_t sequence{0};                     ///< 每连接单调递增，用于排序/丢包观测
    std::chrono::steady_clock::time_point received_at;
};

/// 关闭原因。连接关闭后仍可从 Connection 读取，用于区分正常/异常/背压。
enum class CloseReason : std::uint8_t {
    kNone = 0,          ///< 仍打开
    kPeerClosed,        ///< 对端 FIN/Close 帧/RST
    kLocalClose,        ///< 应用调用 Close
    kIdleTimeout,
    kWriteTimeout,
    kBackpressure,      ///< 发件通道或内部队列持续满，按策略断开
    kProtocolError,
    kSniffRejected,
    kLimitExceeded,
    kShutdown,
};

/// Bento 消息的统一表示（LinkServer 的收发类型）。
struct BentoMessage final {
    enum class Kind : std::uint8_t { kStream = 0, kDatagram = 1, kVideo = 2 };
    Kind kind{Kind::kStream};
    std::uint8_t channel{0};
    std::uint32_t seq{0};        ///< stream/datagram
    std::uint32_t frame_id{0};   ///< video
    bool key{false};             ///< video
    std::vector<std::uint8_t> payload;
};

/// Raw 连接的一帧（已按 Framer 规则切好，或整块数据报）。
struct RawFrame final {
    std::vector<std::uint8_t> bytes;
};

/// 服务端操作错误码（连接级错误不走这里，见 §2.7 与 CloseReason）。
enum class OmniError : std::uint8_t {
    kOk = 0,
    kInvalidConfig,            ///< 字段为空/越界/重复协议
    kUnsupportedCombination,   ///< transport×protocol 或 mode×concurrency 非法
    kUnsupported,              ///< 阶段未实现（服务端 TLS、HTTP/2）
    kBindFailed,               ///< 地址占用/权限
    kAlreadyStarted,
    kNotRunning,
    kNotPollMode,              ///< 非 Poll 模式调用 PollOnce
    kReentrant,                ///< PollOnce 重入
    kProtocolMismatch,         ///< 快速路径下首包不符合唯一协议
    kSniffTimeout,
    kSniffTooLarge,
    kBackpressure,
    kLimitExceeded,
    kTimeout,
    kClosed,
    kInternal,
};

/// HTTP 入站消息：请求 + 该请求专用的响应通道（容量 1）。
struct HttpRequestMessage final {
    MessageMeta meta;
    http::Request request;
    ChannelPtr<http::Response> response;  ///< 应用把响应写回这里；连接驱动者负责编码发送
};

/// 关闭策略：优雅排空 vs 立即中止。连接级与服务器级共用同一枚举。
enum class CloseMode : std::uint8_t { kGraceful = 0, kAbort = 1 };
using ShutdownPolicy = CloseMode;

/// Poll 模式的单轮结果；timeout=true 且 events=0 表示本轮无事发生（不是错误）。
struct PollResult final {
    OmniError error{OmniError::kOk};  ///< 失败时可能是 kNotPollMode/kReentrant/kNotRunning/kInternal
    std::size_t events{0};
    bool timeout{false};
};
```

**入站/出站类型表**（视图层暴露的 `Channel<T>` 元素类型）：

| 协议 | Inbound（服务器 → 应用） | Outbound（应用 → 服务器） |
|---|---|---|
| HTTP | `HttpRequestMessage{meta, http::Request, ResponseChannel}` | `http::Response`（经 `ResponseChannel` 回写） |
| SSE | `SseSubscribe{meta, last_event_id}` | `sse::Event` |
| WS | `WsInbound{meta, net::WebSocketMessage}` | `WsOutbound{opcode, payload}` |
| Link | `BentoMessage` | `BentoMessage` |
| Raw | `RawFrame` | `RawFrame` |

HTTP 的 `ResponseChannel` 是 `ChannelPtr<http::Response>`，随请求一起交付。这样"请求-响应关联"由通道本身表达，不需要在响应里再带一个请求 id，也不会出现"响应发错连接"。这是本设计唯一引入的**每请求短生命周期通道**，容量固定为 1（无缓冲即可，因为每个请求只会有一个响应）。

### 2.4 对象图与工厂

```cpp
class NetServer final : public std::enable_shared_from_this<NetServer> {
 public:
    /// 校验配置并构造；不绑定端口。失败返回 nullptr 并写 error。
    static std::shared_ptr<NetServer> Create(const OmniNetConfig& config,
                                             std::string* error = nullptr);

    /// 绑定/监听并启动执行者。重复调用返回 false。
    bool Start(std::string* error = nullptr);

    /// 幂等。先停止 accept，再按策略排空或中止连接，最后停执行者。
    void Shutdown(ShutdownPolicy policy = ShutdownPolicy::kGraceful,
                  std::chrono::milliseconds timeout = std::chrono::seconds(5));

    bool running() const noexcept;
    std::uint16_t local_port() const noexcept;   ///< bind_port=0 时返回实际端口

    // ---- 五个类型化视图（协议未启用时返回 valid()==false）----
    HttpServer AsHttpServer();
    SseServer  AsSseServer();
    WsServer   AsWsServer();
    LinkServer AsLinkServer();
    RawServer  AsRawServer();

    // ---- Poll 模式驱动；非 Poll 模式返回 kNotPollMode ----
    PollResult PollOnce(std::chrono::milliseconds timeout);

    const OmniNetConfig& config() const noexcept;
    NetStats stats() const;

 private:
    struct Impl;
    explicit NetServer(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> m_impl;
};
```

内部对象图（`Impl` 的成员，均不对外暴露）：

```
NetServer::Impl
 ├── OmniNetConfig                     （不可变）
 ├── std::unique_ptr<IEndpoint>        （TcpEndpoint / UdpEndpoint / MemoryEndpoint / TlsEndpoint*）
 ├── std::unique_ptr<IExecutor>        （GmpExecutor / ThreadExecutor / PollExecutor）
 ├── ConnectionTable                   （ConnectionId → shared_ptr<ConnectionState>）
 ├── Sniffer                           （首包分类，§3）
 ├── std::atomic<bool> running
 └── NetStats
```

`IEndpoint` / `IExecutor` 是**窄接口**：前者只有 `Open/Bind/Accept/Close/LocalPort`，后者只有 `Start/Stop/Post/PollOnce`。它们存在是为了让三种并发模型与四种传输能独立替换，不是为了给业务扩展。

`ConnectionTable` 负责三件事：分配/回收 `ConnectionId`、执行 `max_connections` 与 per-peer 上限、在 `Shutdown` 时按创建顺序快照（避免遍历中删除）。

### 2.5 五个视图：`AsXxx()` 为什么不是继承

五个视图是**五个独立的 final 类**，只持有一个 `std::shared_ptr<NetServer>`（保证 server 存活）与一个 `bool valid_`。它们之间没有共同基类，也不继承 `NetServer`。

```cpp
class HttpServer final {
 public:
    bool valid() const noexcept;                 ///< 协议未启用/阶段未实现时为 false
    ProtocolKind protocol() const noexcept;      ///< 恒为 kHttp

    /// 新连接通道。每建立一个 HTTP 连接推送一个 HttpConnection。
    /// server 关闭时 close；单个连接结束不会关闭本通道。
    ChannelPtr<HttpConnection> Accept();

    /// 可选便捷层：设置单一 handler 后，内部起一个分发 Fiber，
    /// 从每个连接的 Inbound 取请求、调用 handler、把响应写回 ResponseChannel。
    /// 不设置时完全走 Channel，服务器不做任何业务调度。
    void SetHandler(http::Handler handler);

    HttpServerStats stats() const;

 private:
    friend class NetServer;
    HttpServer(std::shared_ptr<NetServer> server, bool valid);
    std::shared_ptr<NetServer> m_server;
    bool m_valid{false};
};

class HttpConnection final {
 public:
    ConnectionId id() const noexcept;
    const net::Address& peer() const noexcept;
    ChannelPtr<HttpRequestMessage> Inbound();    ///< 单生产者(连接驱动者)/单消费者(应用)
    void Close(CloseMode mode = CloseMode::kGraceful) noexcept;
    CloseReason close_reason() const noexcept;
    ConnectionStats stats() const;
};
```

其余四个视图形状相同，差异只在类型与少数便捷方法：

| 视图 | 连接对象 | 额外接口 | 说明 |
|---|---|---|---|
| `SseServer` | `SseConnection` | `Broadcast(const sse::Event&)` | 内部维护订阅者表；`Broadcast` 对每个订阅者 `TrySend`，满则按 `kBackpressure` 断开并计数 |
| `WsServer` | `WsConnection` | `BroadcastText/BroadcastBinary` | 复用 `net::WebSocketMessage`；Close 码透传 |
| `LinkServer` | `LinkSession`（单例） | `Session()`、`Stats()` | M1 只有单 peer；不提供 `Accept()`，因为 Bento 的"接受"就是 HELLO 握手完成 |
| `RawServer` | `RawConnection` | `SetFraming(FramingSpec)` | `FramingSpec` = 分隔符/定长/大端长度/不分帧（每读一块一帧） |

`SetHandler`/`Broadcast` 这类便捷层**不是高并发路径**：单 handler 由一个分发 Fiber 串行调用所有请求（慢 handler 会串行化整个 server），`Broadcast` 在同一驱动节拍里遍历订阅者。它们的定位是"10 行代码跑起来"，压测与生产应改为应用自己消费 `Channel` 并自行分片并发。这一点写进视图注释，避免被误当成框架级路由。

上表中 `SseSubscribe`、`WsInbound`、`WsOutbound`、`FramingSpec` 与三个 `*Stats` 在对应视图头中定义，形状极薄：`SseSubscribe` = `MessageMeta` + `last_event_id`；`WsInbound/WsOutbound` = `MessageMeta` + `net::WebSocketMessage` / `{opcode, payload}`；`FramingSpec` 是 `net::Framer` 三种模式（分隔符/定长/大端长度）再加"不分帧"的声明式封装；`*Stats` 是 `ConnectionStats` 加上该协议特有的计数（如 HTTP `keepalive_reuse`、SSE `slow_subscribers`、WS `pings`）。放在各自头里而不是公共头，避免"没用到 WS 的程序也要认识 WS 类型"。

**`valid()` 而不是异常**：`AsHttpServer()` 在 `protocols` 未含 HTTP 时返回 `valid()==false` 的空视图，所有方法安全空转（`Accept()` 返回一个已关闭的通道）。理由是视图通常在启动时无条件构造，把"配置错误"推迟到第一次使用时抛异常会让初始化代码很难写；配置错误已在 `Create()` 报过一次，视图层不必重复惩罚。

**`LinkServer` 为什么没有 `Accept()`**：Bento 的"连接"由 HELLO 握手建立，握手前不存在会话对象；M1 又限定单 peer。硬造一个只推一次的 `Accept()` 只会让调用方误以为支持多客户端。等 Link 支持多 peer 后，再补 `Accept()` 才是诚实的演进路径。

### 2.6 统一收发：Channel 契约

**每个连接一对通道**，这是唯一的数据面：

```cpp
ChannelPtr<HttpRequestMessage> in  = conn.Inbound();   // 服务器 → 应用
ChannelPtr<http::Response>      out = req.response;    // 应用 → 服务器（每请求一条）
```

对 WS/Raw/Link，发件方向是**连接级**通道（不是每请求）：

```cpp
ChannelPtr<WsOutbound> out = ws_conn.Outbound();       // 应用 → 服务器，多生产者
ChannelPtr<BentoMessage> out = link_session.Outbound();
```

契约（写进头文件注释，测试逐条覆盖）：

| # | 契约 | 违反后果 |
|---|---|---|
| C1 | `Inbound()` 在连接生命周期内返回**同一个** `shared_ptr`，重复调用不新建 | 若每次新建，应用会拿到错误的消费端，消息被两个消费者分走 |
| C2 | 服务器是 Inbound 的唯一生产者；连接结束后**关闭**该通道（应用 `Recv` 得到 `kClosed`） | 应用无法感知连接结束 |
| C3 | 应用是 Outbound 的唯一/多生产者；服务器关闭 Outbound 表示"不再接受发送" | 关闭后 `Send` 返回 `kAlreadyClosed` |
| C4 | 通道有界（`limits.*_capacity`），满时**不阻塞服务器驱动者**：Outbound 一律 `TrySend`；Inbound 在 Gmp/Thread 下允许带超时 `Send`（形成反压，见 §4.8），Poll 下用 `TrySend` + `paused` | 慢应用把整个服务器拖死 |
| C5 | 消息按连接内 `sequence` 单调交付；跨连接不保证顺序 | 应用误以为全局有序 |
| C6 | 通道关闭与 `CloseReason` 的可见顺序固定：先关闭 Inbound，再让 `close_reason()` 返回终值 | 应用读到 `kNone` 却已 EOF |

**满时策略按协议区分**（不能一刀切）：

| 方向/协议 | 满时行为 | 理由 |
|---|---|---|
| Inbound（所有协议） | 驱动者**暂停读**该连接（Gmp/Thread），Poll 模式标记 `paused` 并跳过该 fd | 业务消息不能无声丢；暂停读让 TCP 窗口把压力传给对端 |
| HTTP 响应 | 关闭连接（`kBackpressure`） | HTTP/1.1 没有应用层流控，只能断开 |
| SSE 事件 | 断开该订阅者并计数 | 与 SSE 规范一致：慢消费者由客户端重连 + `Last-Event-ID` 恢复 |
| WS 消息 | 断开（Close 1011/1008 由应用决定） | RFC 6455 无窗口；不静默丢 |
| Bento STREAM | `Send` 返回 `kWouldBlock` 给应用，**不丢** | 可靠通道语义要求 |
| Bento DATAGRAM/VIDEO | 允许丢，计入 `dropped` | DropStale/旧帧丢弃是协议本身语义 |
| Raw | 断开 | 无协议语义可依 |

**为什么不在 `NetServer` 上开一个 `Channel<Inbound>` 巨通道**：`variant` 会把五种协议的生命周期、背压策略、错误语义压进同一个类型，任何新协议都要改所有消费者；而五种独立通道让"没用到的协议零成本"成立。统一的是**模式**（有界、可关闭、带元信息、`TrySend` 不阻塞驱动者），不是类型。

**控制面**单独一条通道，避免和数据面混在一起：

```cpp
enum class ControlCommand : std::uint8_t {
    kCloseConnection,
    kPauseConnection,   ///< 暂停读（应用侧消费不过来时主动刹车）
    kResumeConnection,
    kShutdown,          ///< 等价于 Shutdown(kGraceful)
};

struct ControlMessage final {
    ControlCommand command{ControlCommand::kPauseConnection};
    ConnectionId target;                          ///< kShutdown 时忽略
    CloseMode mode{CloseMode::kGraceful};
};

ChannelPtr<ControlMessage> Control();   // 应用 → 服务器，容量小（默认 64）
```

### 2.7 错误模型

不抛异常跨越 Fiber/线程边界（与 base 的 `SocketResult`/`WebSocketResult`/`sse::Result` 风格一致）。`OmniError` 已在 §2.3 定义，这里只定义返回载体与使用约定：

```cpp
struct OmniResult final {
    OmniError error{OmniError::kOk};
    std::string message;       ///< 中文可读诊断，只用于日志/展示
    bool ok() const noexcept { return error == OmniError::kOk; }
};
```

约定：`message` 不参与逻辑判断；调用方按 `error` 分支。连接级错误不通过 `OmniResult` 返回，而是写进 `CloseReason` + `ConnectionStats.errors`。

### 2.8 可观测性

`NetStats` 聚合到 server 级，`ConnectionStats` 到连接级；字段与 base/Bento 现有计数器**同名同义**，不重新发明：

```cpp
struct ConnectionStats final {
    std::uint64_t bytes_in{0};
    std::uint64_t bytes_out{0};
    std::uint64_t messages_in{0};
    std::uint64_t messages_out{0};
    std::uint64_t dropped{0};
    std::uint64_t backpressure_events{0};
    std::uint64_t errors{0};
    int last_system_error{0};   ///< errno 风格，0 表示成功
};

struct NetStats final {
    // 连接
    std::uint64_t accepted{0};
    std::uint64_t active{0};             ///< 当前存活
    std::uint64_t closed_graceful{0};
    std::uint64_t closed_abort{0};
    std::uint64_t rejected_limit{0};
    std::uint64_t rejected_per_peer{0};
    std::uint64_t sniff_rejected{0};
    // 流量
    std::uint64_t bytes_in{0};
    std::uint64_t bytes_out{0};
    std::uint64_t messages_in{0};
    std::uint64_t messages_out{0};
    std::uint64_t dropped{0};
    std::uint64_t backpressure_events{0};
    // 协议
    std::uint64_t http_requests{0};
    std::uint64_t http_keepalive_reuse{0};
    std::uint64_t sse_events{0};
    std::uint64_t ws_messages{0};
    std::uint64_t raw_frames{0};
    // 时间
    std::uint64_t accept_wait_ms_total{0};
    std::uint64_t sniff_timeout_total{0};
};
```

`DebugSnapshot()` 返回与 `io::IOManagerDebugSnapshot` 同构的只读快照（连接表大小、各状态连接数、通道深度、暂停连接数），后续接入项目现有 debug 视图（`include/go2cpp/debug/`）。本设计不新建观测框架，只做字段聚合。

### 2.9 头文件布局与构建目标

```
include/go2cpp/bento/omni/
  config.hpp        # OmniNetConfig / Limits / 枚举 / 校验函数声明
  message.hpp       # ConnectionId / MessageMeta / BentoMessage / RawFrame / CloseReason
  stats.hpp         # NetStats / ConnectionStats / HttpServerStats ...
  net_server.hpp    # NetServer + 五个 AsXxx 声明（视图头在此集中包含）
  http_server.hpp   # HttpServer / HttpConnection / HttpRequestMessage
  sse_server.hpp    # SseServer / SseConnection / SseSubscribe
  ws_server.hpp     # WsServer / WsConnection / WsInbound / WsOutbound
  link_server.hpp   # LinkServer / LinkSession
  raw_server.hpp    # RawServer / RawConnection / FramingSpec
src/bento/omni/
  config.cpp        # 校验（纯函数，可单测）
  net_server.cpp    # Impl / 工厂 / 生命周期
  sniffer.cpp       # 分类表与状态机
  executor_*.cpp    # gmp / thread / poll
  endpoint_*.cpp    # tcp / udp / memory / tls
  http_server.cpp / sse_server.cpp / ws_server.cpp / link_server.cpp / raw_server.cpp
```

构建目标：新增 `go2cpp_bento_omni`（静态库），**默认 OFF**，开关 `GO2CPP_WITH_BENTO_OMNI`，与现有 `GO2CPP_WITH_BENTO` 的"不影响主线"策略一致：

```
go2cpp_bento_omni
 ├─ go2cpp_bento        （帧/通道/Link）
 ├─ go2cpp_network      （Socket/HTTP/WS/SSE/TLS客户端）
 ├─ go2cpp_io           （IOManager/IOBackend）
 └─ OpenSSL             （仅 M4，PRIVATE 链接）
```

依赖方向单向：`omni → bento/network/io`，base 与 bento 绝不反向依赖 omni。测试目标 `bento_omni_test_*` 与实现同目录规则（`tests/bento/omni/`）。

---

## 3. 协议嗅探与路由

### 3.1 嗅探发生在哪一层

- **流式传输（TCP/TLS/Memory-stream）**：嗅探发生在 `accept` 之后、任何协议解析器创建之前。驱动者先读第一段字节进 `sniff_buffer`（上限 `sniff.max_bytes`），分类成功后再把缓冲**原样交给**该协议的解析器（不能让解析器重读，否则首包丢失）。
- **数据报（UDP）**：没有"首包流"概念，每个数据报独立。M1 只允许 `protocols == {Link}` 或 `{Raw}`；若两者同时启用，按首 16 字节判断 `BENT` magic（`ver=1` + `type∈[1,7]` + 载荷前 4 字节 `'B','E','N','T'`），失败则按 Raw。
- **Memory**：与流式相同，测试可以直接 `PushInbound` 一段精心构造的首包来驱动嗅探。

### 3.2 识别表

| 判定 | 特征 | 最少字节 | 依据 |
|---|---|---|---|
| HTTP/1.x | 首 token 是 `GET/POST/PUT/DELETE/HEAD/OPTIONS/PATCH/CONNECT/TRACE` 之一，后跟 SP | 方法 token + 1（最短 4） | `http::parse_method`（`network/http.hpp:34`）已覆盖全部方法 |
| HTTP/2 prior knowledge | 精确匹配 `PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n` | 24 | RFC 9113 §3.4；本期识别但**返回 `kUnsupported`**（HTTP/2 核心未进库） |
| WebSocket | 先按 HTTP 解析出完整请求头，再校验 `Upgrade: websocket`、`Connection` 含 `Upgrade`、存在 `Sec-WebSocket-Key` | 完整头（受 `limits.http_max_header_bytes` 限制） | `net::WebSocket::ValidateUpgradeRequest`（`network/websocket.hpp:86`） |
| Bento Link | 12B 帧头可解析（`ver=1`、`type∈[1,7]`、`payload_bytes` 合法）且 HELLO/Ack 载荷前 4 字节为 `BENT` | 16（头 12 + magic 4） | `bento/net/protocol.hpp:58-66`；magic 见 `src/bento/net/link.cpp:24-29` |
| Raw | 以上都不匹配，且 `kRaw` 已启用 | — | 兜底 |

**识别顺序**固定为 HTTP → h2 preface → Bento → Raw；WS 是 HTTP 的子判定（必须在 HTTP 头解析完成后）。顺序固定是为了结果可复现，不做"按概率猜"。

### 3.3 嗅探状态机

```
accept
  │
  ▼
[kNeedMore] ── 字节不足 ──► 继续累积（受 sniff.max_bytes / sniff.timeout 限制）
  │
  ├─ 分类成功 ──► [kClassified] ──► 把 sniff_buffer 交给对应解析器；协议此后固定
  │
  └─ 超限 / 超时 ──► [kRejected] ──► 关闭(kSniffRejected) 或 fallback_to_raw
```

状态机只有三个状态、四条边，实现上就是一个 `switch` + 一个计时器。**不做增量再嗅探**：分类后即使后续字节看起来像别的协议，也不再改判——否则协议解析器可以互相污染状态，且给了攻击者"先伪装 HTTP 再发 Bento"的降级面。

### 3.4 与 `protocols` 配置的交互

| 配置 | 行为 |
|---|---|
| `protocols.size() == 1` 且不是 WS/SSE（两者仍需先解析 HTTP 头） | 跳过**分类**（不是跳过 HTTP 头解析），直接创建解析器；仍做一次廉价校验（如 HTTP 方法首 token），失败计 `kProtocolMismatch` 并关闭，避免把任意字节喂给解析器 |
| `protocols.size() == 1` 且是 WS | 不能跳过：必须先解析 HTTP Upgrade 请求，再转为 WS |
| `protocols.size() >= 2` | 走完整嗅探；`sniff.enabled=false` 视为配置冲突（`kInvalidConfig`） |
| `kRaw` 与其它协议共存 | Raw 只作兜底；`fallback_to_raw=true` 时未识别流量进 Raw，否则关闭 |
| `transport/protocols` 含本期未实现的组合（如 M4 之前的 `kTls`） | `Create()` 返回 `kUnsupported`，不进入运行期 |

### 3.5 升级与剩余字节

WS 是本设计里唯一的"协议内升级"：

```
HTTP 请求头解析完成
  ├─ 普通请求 → 交给 HTTP 处理路径
  └─ Upgrade: websocket → 校验 → 写 101 → WebSocket::AcceptWithPending(socket, request, pending)
```

`pending` 来自 `http::Parser::take_remaining()`（`network/http.hpp:116`）。这是必须走的一步：客户端可能把第一个 WS 帧和 Upgrade 请求放在同一个 TCP 段里，丢掉 `pending` 会表现为"握手成功但首帧永远收不到"。base 已提供 `AcceptWithPending`（`network/websocket.hpp:94-97`），本设计只需保证顺序正确：先把整个 `sniff_buffer` 喂给 `http::Parser`，再用 `take_remaining()` 取出 Parser 尚未消费的字节，连同 request 一起交给 `AcceptWithPending`。

SSE 不是升级，而是普通 HTTP 响应的一种：请求的 `Accept` 含 `text/event-stream`（或命中 `sse_path`，默认 `/events`）时，用 `sse::Stream` 接管 socket 写 `text/event-stream`。因此 SSE 与 HTTP 共用同一个解析器与连接对象，只是 outbound 类型从 `http::Response` 变成 `sse::Event`。

### 3.6 失败与降级

- **超时**：`sniff.timeout` 内攒不到可判定字节 → `kSniffTimeout`，关闭连接并计数。默认 2s，足以覆盖高延迟链路的第一个 RTT。
- **超限**：`sniff.max_bytes` 是硬上限（默认 4096，远小于 HTTP 头上限 64 KiB）。超过仍无法判定，说明不是本服务器支持的协议，关闭比继续攒更安全（防慢速攻击）。
- **h2 preface**：识别出来但本期不支持 → 关闭并计 `kUnsupported`。**不能**退回 HTTP/1 解析：`PRI * HTTP/2.0` 会被 `http::parse_method` 判为未知方法，错误信息会误导排查方向。
- **降级**：`fallback_to_raw` 默认 false。开启后所有未识别流量按 Raw 交付，相当于把服务器变成"任意字节中继"，只建议在受控内网或调试时使用。

### 3.7 "路由"的边界

本设计的"路由"只有两层：

1. **协议路由**：首包 → 协议解析器（§3.2）；
2. **连接路由**：`ConnectionId` → 连接对象（`ConnectionTable`）。

**不内建 HTTP 路径路由表**。理由：`http::Server::route` 只服务于它自己的 `serve_connection` 同步循环，无法直接嫁接到"每连接一对 Channel"的模型；强行复用会把业务回调重新塞回网络驱动者线程，违背 P2。单 handler 便捷层（`HttpServer::SetHandler`）只覆盖"一个函数处理所有请求"的简单场景，多路由由应用在自己的分发 Fiber 里做——那本来就是业务逻辑，不该由网络层拥有。

---

## 4. 并发模型

### 4.1 三个执行者

| 执行者 | 线程归属 | fd 等待方式 | 适用 |
|---|---|---|---|
| `GmpExecutor` | `io::IOManager` 的 worker（默认 = CPU 核数） | `Socket` EAGAIN → `IOManager::wait` → park Fiber | 高并发、同步写法、默认 |
| `ThreadExecutor` | 固定大小线程池（`limits.worker_threads`） | `Socket` 内部 100ms 分片 poll | 低并发、需要真阻塞语义、调试 |
| `PollExecutor` | **无**（调用方线程） | `PollOnce` 内 epoll_wait + 非阻塞读写 | 嵌入已有事件循环、单线程程序、测试 |

三者都实现同一个窄接口：

```cpp
class IExecutor {
 public:
    virtual ~IExecutor() = default;
    virtual bool Start(std::string* error) = 0;
    virtual void Stop() = 0;
    /// 投递一个任务；Poll 模式下入队但不执行（由 PollOnce 驱动）。
    virtual void Post(std::function<void()> task) = 0;
    /// 仅 Poll 模式有效；其它模式返回 kNotPollMode。
    virtual PollResult PollOnce(std::chrono::milliseconds timeout) = 0;
};
```

### 4.2 Blocking 模式

**语义**：视图提供"等到结果"的调用。

```cpp
// HttpConnection 的阻塞便捷方法（内部就是 Inbound()->Recv() + 超时）
ChannelRecvResult<HttpRequestMessage> Recv(std::chrono::milliseconds timeout);
```

- `Blocking + Gmp`：`Recv` 在 Fiber 中 park，不占 worker；`Send` 走 `Outbound()->Send`，满时 park 直到有空间或超时。
- `Blocking + Thread`：每个连接一个线程任务，`Recv` 真正睡眠；线程数必须 ≥ 并发连接数，否则 accept 后排队（`Limits::worker_threads` 默认 0，实际取 `hardware_concurrency()` 且下限 4；`Create()` 时对 `max_connections > worker_threads` 给出警告级诊断）。
- 两种组合下 fd **仍然是非阻塞的**，"阻塞"只发生在 Channel 与 `IOManager::wait` 层。这一点必须在文档与注释里反复强调，否则后来者会去 `fcntl` 改 fd 模式（§6.1 的坑）。

### 4.3 NonBlocking 模式

**语义**：不存在"等到有数据"的调用；应用要么轮询 `TryRecv`，要么自己在别的执行者上阻塞（Channel 本身支持 `Recv`，所以"非阻塞服务器"不妨碍业务用阻塞消费者）。

- `NonBlocking + Gmp`：连接驱动者是一个 Fiber，循环 `wait → 读 → 解析 → TrySend`；业务可以在另一个 Fiber 里 `Recv`。这是推荐的默认组合（把 `Blocking` 的 park 从连接驱动者挪到业务侧，连接驱动者永不因应用慢而停摆）。
- `NonBlocking + Thread`：线程池里跑同样的循环，`wait` 退化为 poll。适合不想引入 Fiber 调度器的宿主程序。
- `NonBlocking + Poll`：`PollOnce` 是唯一推进点，服务器不创建任何线程，也不启动 Scheduler。

**公平性**：`PollOnce` 每轮对每个就绪 fd 最多处理 `kMaxFramesPerTick`（默认 32）条消息 / 64 KiB，然后让出，避免单个高流量连接饿死其它连接。这个上限与 Bento `Link::RunOnce` 的 `kMaxFramesPerRun=256`（`bento/net/link.hpp:210`）同源，但更小，因为 OmniNetServer 一层要服务多个连接。

### 4.4 Poll 模式

```cpp
PollResult NetServer::PollOnce(std::chrono::milliseconds timeout);
```

- `timeout` 是本次 `epoll_wait` 的上限；有事件立即返回；无事件到点返回 `events=0, timeout=true, error=kOk`（空闲不是错误）。
- `PollOnce` 不可重入：内部用一个 `std::atomic_flag` 检测嵌套调用，重入直接返回 `kReentrant`。
- 定时器（idle/write/sniff/accept 退避）在 `PollOnce` 内按"当前时间 - 上次 tick"推进；调用方必须周期性调用（建议 ≤100ms），否则超时精度等于调用间隔。
- `PollExecutor` 拥有自己的 epoll fd（不复用 `IOManager`），因此 **Poll 模式与 Gmp 模式不能共享连接对象**。配置在 `Create()` 固定，运行期不改。

### 4.5 线程与调用约束

| 操作 | 允许的调用者 | 备注 |
|---|---|---|
| `NetServer::Create/Start` | 任意线程，一次 | `Start` 后配置不可变 |
| `Shutdown` | 任意线程；幂等 | 不能从连接驱动者回调里同步等待自己退出（见 §6.4） |
| `PollOnce` | 仅启动它的那个线程 | 与 epoll fd 的线程归属一致 |
| `XConnection::Inbound()->Recv` | 任意线程/Fiber | `Channel` 本身支持跨线程 park |
| `XConnection::Outbound()->TrySend` | 任意线程/Fiber | 应用侧推荐 `TrySend`；服务器内部对 Outbound 也只 `TrySend` |
| `Close` | 任意线程 | 只置状态 + 唤醒，不做阻塞清理 |
| `stats()` | 任意线程 | 原子计数，允许弱一致 |

**绝不能做的事**：在持有 `std::mutex`/`sync::Mutex` 时调用可能 park 的 `Recv`/`Send`。base 的 `docs/modules/network.md` 已把这条列为硬约束（`docs/modules/network.md:89-90`），OmniNetServer 的视图注释要再写一遍。

### 4.6 定时器

| 定时器 | 作用 | 触发动作 |
|---|---|---|
| accept 退避 | `accept` 返回 EMFILE/ENFILE 时退避 10ms→1s | 暂停 accept，恢复后继续 |
| 首包/嗅探 | `sniff.timeout` | 关闭 `kSniffTimeout` |
| 空闲 | `idle_timeout`（0=关） | 关闭 `kIdleTimeout`；SSE/WS 有心跳时按"最后一次成功写"计时 |
| 写超时 | `limits.write_timeout` | 关闭 `kWriteTimeout`；慢消费者兜底 |
| 优雅关闭 | `limits.close_timeout` | 强制 abort 剩余连接 |
| 心跳（协议自带） | Bento `heartbeat_interval_ms`、SSE `tick()`、WS Ping | 由协议层管理，OmniNetServer 只提供驱动节拍 |

Gmp/Thread 模式下用一个 100ms 周期的 timer Fiber/线程驱动上表；Poll 模式下全部折算进 `PollOnce` 的 `epoll_wait` 超时。**不引入新的定时器框架**，直接复用 `io::IOManager` 的 deadline 机制或 `core::Timer`（M0 决定，倾向后者以避免 Poll 模式被 IOManager 绑定）。

### 4.7 单驱动者模型

每个连接在任一时刻只有：

- 一个**读驱动者**：负责 `wait(readable) → recv → 解析 → TrySend(inbound)`；
- 一个**写驱动者**：负责 `TryRecv(outbound) → 编码 → send`。

首版让**同一个执行者串行做读和写**（一个循环里先读后写），理由是：

1. 避免 `Socket` 读写并发时的额外同步（base 的 `Socket` 允许读写并发，但协议解析器不一定）；
2. 保证 `sequence` 单调与关闭顺序可推理；
3. 代价是"写阻塞会拖慢读"，但写有 `write_timeout` 兜底，且多数 HTTP/WS 响应能在超时内写完。

M4 若压测证明写是瓶颈，再把写拆成独立 Fiber/线程，并引入"每连接一个写队列 + 单写者"的不变量。**拆分是优化，不是首版承诺**。

### 4.8 反压与公平性

反压链路（以 HTTP 为例）：

```
业务慢 → Outbound 满 / Inbound Send 超时
       → 驱动者停止取件或暂停读该连接
       → TCP 接收窗口收缩 → 对端发送变慢
```

这条链路成立的前提是**驱动者绝不阻塞在业务通道上**。因此：

- 服务器侧对 Inbound 用 `Send`（可 park）还是 `TrySend`？**分模式**：Gmp/Thread 下用带超时的 `Send`（park 驱动者，天然反压）；Poll 下只能用 `TrySend`，失败即 `paused=true`，下一轮再试。
- `paused` 连接不参与本轮读事件分发，但仍参与写与定时器，避免"暂停读"把写也饿死。
- 每连接预算 `per_connection_budget_bytes` 把"通道容量 × 消息大小"封顶；超预算先断开，防止 256 条 1 MiB 消息 × 1024 连接的理论爆炸。

---

## 5. 分阶段实现路径

### 5.0 依赖图

```
M0 骨架（config/message/NetServer/Memory/Poll）
 ├──► M1 LinkServer（bento，UDP，单 peer）
 └──► M2 HTTP + SSE（base，TCP，accept/keep-alive/背压）
        ├──► M3 WS（HTTP Upgrade）+ Raw（Framer）+ 多协议嗅探
        └──► M4 TLS 服务端 + 硬化（指标/关闭/长稳）
M1 ─────────────────────────────► M4（Bento 侧硬化）
```

M1 与 M2 可并行，因为一个走 UDP/Bento、一个走 TCP/base，共用 M0 的骨架与 Channel 契约。M3 依赖 M2 的 HTTP 解析与连接对象（WS 升级要用），M4 依赖 M2/M3 的协议路径来验证 TLS。

### 5.1 M0：骨架与 Memory 传输

**范围**

- `config.hpp` 全部枚举与 `Limits`、`ValidateConfig()` 纯函数；
- `message.hpp`、`stats.hpp`；
- `NetServer::Create/Start/Shutdown/PollOnce` 骨架；
- `MemoryEndpoint`（成对内存缓冲，参考 `http2::MemoryTransport` 的 inbound/outbound 设计，`network/http2/transport.hpp:69-126`）；
- `RawServer` 最小实现（无嗅探，固定 Raw），作为骨架的"第一个协议"。

**退出条件**

- 配置矩阵的全部非法组合都能在 `Create()` 返回对应错误（单测覆盖 4×5 矩阵与 2×3 矩阵）；
- Memory 端点能完成"写入 → PollOnce → RawFrame 出站"的闭环；
- `Shutdown` 在无连接、有连接、重复调用三种情况下都幂等；
- 0 警告（`-Wall -Wextra -Wpedantic -Werror`，与 Bento 现有目标一致）。

**不做**：真实 socket、Fiber、线程池、任何嗅探。

### 5.2 M1：LinkServer

**范围**

- `UdpEndpoint`（bind + 非阻塞收发的薄封装，复用 `Socket::CreateUDP`）；
- `LinkServer`：一个 `bento::Link` + 三个回调（`on_stream/on_datagram/on_video`）→ `BentoMessage` → Inbound；
- Outbound：`BentoMessage` → `Link::SendStream/SendDatagram/SendVideoFrame`；
- `LinkSession` 的统计合并 `LinkStats` + 三类通道 `Stats`；
- 驱动方式：Poll 模式下 UDP fd 也注册进 `PollExecutor` 的 epoll，收到可读事件后调 `Link::RunOnce(0)` 抽干数据报并推进 Link 定时器；Gmp/Thread 下把 `Link` 的全部调用**固定在一个专用 OS 线程**（Link 的单线程不变量是线程级的，普通 Fiber 会在 worker 间迁移），该线程循环 `RunOnce(100)`。

  > 这是 M1 最容易写错的一处：把 `RunOnce` 放进一个可迁移的 Fiber，单机测试通常也能过，但在多 worker 下会随机破坏 `m_reliable`/`m_relation` 的无锁前提。

**为什么先做它**：Bento 是仓库里唯一已经跑过弱网验证的完整收发闭环（`tests/bento/PROTOCOL.md` 记录 20% 丢包 + 200ms 延迟下 STREAM 100% 按序），把它接进 OmniNetServer 能最早暴露"通道契约 / 关闭语义 / 统计口径"的问题，而且不依赖 TCP accept、HTTP 解析这些还没写的部件。

**退出条件**

- `tests/bento/test_link_loopback.cpp` 的 STREAM/DATAGRAM 场景与 `tests/bento/test_video_fragment.cpp` 的视频分片场景，能在 OmniNetServer 之上原样通过；
- 单 peer 边界有明确断言：第二个来源的 HELLO 被拒且计入 `unknown_peer_frames`，OmniNetServer 不静默接受；
- 弱网代理（`tests/bento/udp_proxy.hpp` + `netem_probe`）下 100% 按序；
- 关闭/重连：`Link::Close` 后 `LinkSession` 的 Inbound 关闭，`close_reason()==kLocalClose`；对端超时则 `kPeerClosed`。

**不做**：多 peer、TCP、TLS、协议嗅探（`protocols` 只允许 `{kLink}`）。

### 5.3 M2：HTTP 与 SSE

**范围**

- `TcpEndpoint`：`ListenTCP` + accept 循环（Fiber 或线程或 Poll 分支），`SO_REUSEADDR` 由 base 已设；
- 每连接一个驱动者：`http::Parser` 增量解析 → `HttpRequestMessage` → Inbound；
- Outbound：`http::Response` → `Response::to_string()` → `SocketStream::write_all`；`Connection: close` 与 `max_requests` 语义沿用 base；
- keep-alive：同一连接串行处理多个请求，受 `limits.max_requests_per_connection`（默认 100，与 `http::Server::serve_connection` 的默认一致）约束；
- SSE：请求的 `Accept` 含 `text/event-stream` 或路径匹配 `sse_path` 时，把连接交给 `sse::Stream`，Inbound 变 `SseSubscribe`，Outbound 变 `sse::Event`，`tick()` 由定时器驱动；
- 慢消费者：`write_timeout` + `kBackpressure` 关闭；`SseServer::Broadcast` 对满订阅者断开。

**退出条件**

- `curl` 与仓库自带客户端能完成 GET/POST/HEAD、chunked 响应、`Connection: close`、keep-alive 复用；
- SSE 客户端（`docs/sse.md` 的用法）能收到事件、心跳注释、`Last-Event-ID` 重连；
- 半包/粘包随机测试：把同一请求切成 1..N 字节的随机片段喂入，解析结果与一次性喂入一致（随机 1000 轮）；
- 慢消费者：订阅者停止读取后，`Broadcast` 在预算内断开且 server 内存不增长（RSS 观测）；
- 1000 并发连接（`NonBlocking+Gmp`）下无 fd 泄漏、无 Fiber 泄漏。

**不做**：HTTP/2、TLS、WS、多协议同端口、路由表。

### 5.4 M3：WebSocket 与 Raw，多协议嗅探

**范围**

- 嗅探器完整实现（§3 的识别表与状态机）；
- WS 升级：HTTP 解析 → `ValidateUpgradeRequest` → 101 → `WebSocket::AcceptWithPending`；之后 WS 帧走 `read_message/send_text/send_binary`；
- `WsConnection` 的 Inbound/Outbound 与 Ping/Pong/Close 透传；
- `RawServer` 的 `FramingSpec`（分隔符/定长/大端长度/不分帧）+ 与其它协议同端口嗅探；
- 协议路由：同一 TCP 端口上 HTTP/WS/Raw 共存。

**退出条件**

- 浏览器/`websocat` 能完成握手、文本/二进制、分片消息、Ping/Pong、Close 1000；
- 首帧与 Upgrade 请求同段到达时，`AcceptWithPending` 拿到完整首帧（构造该分段的单测）；
- 同端口三种流量（HTTP GET、WS Upgrade、自定义 Raw）各 100 连接并发，分类准确率 100%，无串协议；
- h2 preface 被正确识别并拒绝，不落入 HTTP/1 解析器。

**不做**：permessage-deflate、WS 扩展协商、HTTP/2 CONNECT、TLS。

### 5.5 M4：TLS 服务端与硬化

**范围**

- `TlsEndpoint`：`SSL_CTX` 服务端上下文、证书/私钥加载、`SSL_accept` 非阻塞循环（`WANT_READ/WANT_WRITE` → `IOManager::wait`）、SNI、ALPN；
- TLS 上的 HTTP/SSE/WS/Raw（协议层完全复用，不感知 TLS）；
- 指标与 debug 快照接入；
- 长稳与资源上限：24h 运行、fd/内存/线程曲线、优雅关闭排空；
- 可选：每连接双驱动者（写 Fiber）的压测对比，决定是否进入默认。

**退出条件**

- `openssl s_client` 能完成 TLS1.2/1.3 握手、ALPN 协商 `http/1.1`；
- 自签证书 + 显式 CA 的客户端能校验通过；错误证书被拒（fail-closed）；
- TLS 上的 HTTP/WS 与明文路径的功能测试全部通过；
- 24h 长稳：无 fd 增长、无内存单调增长、关闭后无残留线程；
- `Shutdown` 在 1000 连接、每连接 1 MiB 待发数据时能在 `close_timeout` 内完成（或按策略 abort 并计数）。

**不做**：TLS 会话复用/0-RTT、客户端证书轮换、mTLS 的 SPIFFE 集成、HTTP/2 over TLS。

### 5.6 每阶段的测试门槛

按项目规范（压测 + 随机 + ≥5 真实场景），每阶段至少：

| 类型 | M0 | M1 | M2 | M3 | M4 |
|---|---|---|---|---|---|
| 单元 | 配置矩阵、枚举校验、Memory 端点 | BentoMessage 映射、Link 状态机 | Parser 半包、keep-alive 状态机 | 嗅探表/状态机、FramingSpec | TLS 参数校验 |
| 随机 | 随机配置组合 | 随机通道/随机丢包 | 随机切片/随机请求 | 随机首包/随机帧 | 随机 TLS 记录分片 |
| 压测 | 无（骨架无吞吐语义） | 10k 消息吞吐、p99 | 1k 连接、HTTP QPS | 1k 连接、WS 消息速率 | TLS 握手速率、长稳 |
| 场景 5+ | Memory 回环、非法配置 | loopback/弱网/多消息/关闭/重连 | curl/SSE/慢消费者/keep-alive/大响应 | 浏览器 WS/Raw/同端口/h2 拒绝/首帧同段 | 自签/CA/明文回退拒绝/长稳/优雅关闭 |

**禁止**把"能跑通"当验收；压测必须给数字（QPS、p50/p95/p99、RSS、fd 数）与条件（连接数、消息大小、时长、机器）。

---

## 6. 风险与约束

### 6.1 fd 模式切换（最高风险，结论：不做）

**事实**：

- base 的 `Socket` 在构造时**无条件**调用 `init_nonblock()`（`src/network/socket.cpp:47-60`、`:156-158`），失败则关闭 fd 并返回空；`accept4` 直接用 `SOCK_NONBLOCK`（`src/network/socket.cpp:222-223`）。因此 `FromFd` 不是"保留调用方 fd 模式"，而是把它**静默改成非阻塞**——`O_NONBLOCK` 属于 open file description，`dup` 出来的副本会一起改变。
- `FdManager` 同时记录 `user_nonblock` 与 `system_nonblock`（`network/fd_manager.hpp:26-45`），但读侧无锁、且没有"切换模式"的 API。
- Linux 上 `accept` 的新 fd **不继承** `O_NONBLOCK`，所以 base 用 `accept4(SOCK_NONBLOCK)` 显式设置；`dup` 则共享同一个 open file description 的标志。

**如果允许运行时切换，会发生什么**：

1. 从非阻塞切到阻塞：必须先从 epoll 摘除（`IOBackend::remove`）、等待该 fd 的在途操作归零、再 `fcntl` 清标志。任何一步顺序错，`epoll_wait` 仍会报告就绪，但一次 `recv` 可能阻塞整个 poller/worker 线程——Fiber 调度器停摆。
2. 从阻塞切到非阻塞：已经 park 的 Fiber/线程不会被唤醒（它们等的是旧语义），表现为"切换后卡住直到下一次数据到达"。
3. `Socket` 的 `close`/`cancel_read` 依赖 fd 代际与在途计数；外部 `fcntl` 绕过了这套保护，close 与读可能竞争同一个 fd 号（ABA）。
4. OpenSSL 的非阻塞重试模式要求 fd 非阻塞：把阻塞 fd 交给 `SSL_accept`/`SSL_read` 会卡住整个 worker。客户端 `TlsTransport` 的注释已用同一理由解释为什么必须非阻塞（`network/http2/tls_transport.hpp:43`）；服务端 TLS（M4）沿用这条约束。

**结论**：`mode` 在 `Create()` 固定，连接生命周期内**不切换 fd 模式**。Blocking/NonBlocking 只影响视图 API 与驱动的等待策略，不影响 `O_NONBLOCK`。若未来确实需要"把已有 fd 交给第三方阻塞库"，做法是：`release()` 出 fd、由调用方完全接管（不再经过 `Socket`/`FdManager`/Hook），而不是在 `Socket` 内部切换。

### 6.2 背压

- **单点原则**：服务器侧永不无限阻塞在业务通道上；`TrySend` 失败或 `Send` 超时必须落到 §2.6 的策略表。
- **HTTP/1.1 没有应用层流控**：慢客户端只能靠 `write_timeout` 与断开；不要试图用"读它的请求"来反压一个只写不读的响应流。
- **SSE 的慢消费者**是常态而非异常：`Broadcast` 必须能承受"10% 订阅者卡住"，策略是断开 + 计数，不是无限缓冲。
- **Bento STREAM 的窗口是协议层背压**：`SendStream` 返回 false 表示窗口满或载荷超长，必须映射成 `kWouldBlock` 并让应用重试，**不能**在 OmniNetServer 内部无界排队。
- **预算优先于容量**：`inbound_capacity × max_message_bytes` 可能远超 `per_connection_budget_bytes`，以后者为准，超预算先断开。

### 6.3 关闭语义

连接状态机：

```
kConnecting ──► kOpen ──► kDraining ──► kClosed
     │            │            │
     │            │            └─ 排空 Outbound / 发 Close 帧 / 终止 chunk，超时 abort
     │            └─ 应用 Close / 对端 FIN / 错误
     └─ 握手失败/超时 ──► kClosed
```

`kConnecting` 在 TCP 上表示"accept 后到首包分类完成"，在 Bento 上是"HELLO 握手"，Memory 与 TCP 同构。这样状态机可以同时描述四种 transport，不必为 UDP 单开一套。

`CloseMode`：

| 模式 | HTTP | SSE | WS | Bento | Raw |
|---|---|---|---|---|---|
| `kGraceful` | 写完当前响应 + `Connection: close` | 发终止 chunk `0\r\n\r\n` | Close 帧（1000/应用指定） | 停发新消息，等 inflight 清空或 `close_timeout` | `shutdown(SHUT_WR)`，读完剩余 |
| `kAbort` | 直接 `close` | 直接 `close` | 直接 `close`（不发 Close 帧） | `Link::Close`（不触发 `on_disconnected`） | 直接 `close` |

三条硬约束：

1. **先关 Inbound 再关 Outbound**：应用先看到 EOF（知道连接结束），才失去发送能力；反过来会让应用在"以为还活着"时发送失败。
2. **`Link::Close()` 不触发 `on_disconnected`**（`bento/net/link.hpp:152`），所以 LinkServer 必须自己把 `kLocalClose` 写进 `LinkSession`，不能等回调。
3. **`Socket::close()` 可能返回 false**（当前上下文无法 park 时延迟关闭，`network/socket.hpp:89-94`），OmniNetServer 必须把"关闭未完成"计入状态并在下一个 tick 重试，不能假设一次 `close` 就释放 fd。

### 6.4 生命周期

- `NetServer` 被视图以 `shared_ptr` 持有；`Shutdown` 只停资源，不析构对象。最后一个引用释放时才析构。
- **连接对象不得持有 `shared_ptr<NetServer>`**：`ConnectionTable` 持有连接状态，若连接再反向持有 server 就构成循环引用。连接只持 `weak_ptr<NetServer>`（`stats()` 时 `lock()`，失败返回空快照），或干脆只持连接状态与统计原子。
- `Shutdown` 禁止从连接驱动者内部同步调用（会等待自己）；提供 `PostShutdown()` 异步版本，内部 `Post` 到执行者。
- `ConnectionTable` 的遍历一律先快照 `vector<shared_ptr<ConnectionState>>`，避免回调里删除导致迭代器失效。
- `MemoryEndpoint` 没有 fd，但同样要走 `kDraining`（否则测试覆盖不到关闭顺序）。
- 析构顺序：停 accept → 停定时器 → 关闭所有连接 → 停执行者 → 释放端点 → 释放表。任何一步失败只记日志，不抛异常。

### 6.5 并发不变量

| # | 不变量 | 检查方式 |
|---|---|---|
| I1 | 一个连接只有一个读驱动者 | 连接状态里的 `driving` 标志 + 断言 |
| I2 | `Inbound` 只有一个生产者（服务器） | 类型上只暴露 `ChannelPtr`，服务器内部持有唯一写端 |
| I3 | 服务器不在业务通道上无限阻塞 | 代码审查 + `TrySend` 覆盖率测试 |
| I4 | `PollOnce` 不可重入 | `atomic_flag`，重入返回 `kReentrant` |
| I5 | `Link` 的所有调用在同一 **OS 线程**（不是同一 Fiber） | LinkServer 把驱动绑到专用线程 + TSAN 用例 |
| I6 | 不持锁 park | 关闭路径的 TSAN/死锁用例（1000 次并发关闭） |

### 6.6 服务端 TLS 与 HTTP/2 的现状约束

- **服务端 TLS 不存在**：`network/http2/tls_transport.hpp` 只实现客户端（`:49` 明确"无服务端 TLS 与会话复用"）。`transport=kTls` 在 M4 之前一律 `kUnsupported`。M4 的实现要点：`SSL_CTX_new(TLS_server_method())`、加载证书链/私钥、`SSL_accept` 非阻塞重试、ALPN 选择 `http/1.1`（本期不协商 h2）。
- **HTTP/2 核心未进库**：`go2cpp_network` 只编译 `http2/tls_transport.cpp`，`connection/frame/hpack/huffman/stream_state` 只在测试目标里（`docs/modules/network.md:57-61`）。因此 `ProtocolKind` 没有 `kHttp2`，嗅探识别到 h2 preface 只做拒绝。**不要把"代码存在"当成"可用"**。
- **QUIC/HTTP3 默认不编译**：与本期无关，但配置校验要拒绝 `protocols` 里未来可能加入的枚举值（用 `default:` 分支返回 `kUnsupported`，防新增枚举忘记处理）。

### 6.7 资源上限与默认值

默认值按"单机 1k 连接、每连接 2 MiB 预算"设计，总计约 2 GiB 上限；生产必须按机器调：

| 项 | 默认 | 上限推导 |
|---|---|---|
| `max_connections` | 1024 | 文件描述符 ≥ 1024 + 保留 |
| `read_buffer_bytes` | 64 KiB | 64 KiB × 1024 ≈ 64 MiB |
| `inbound/outbound_capacity` | 256 / 256 | 条数上限，真实占用看消息大小 |
| `max_message_bytes` | 1 MiB | 超过按协议拒绝（HTTP 413 / WS 1009 / Bento 分片） |
| `per_connection_budget_bytes` | 2 MiB | 超预算断开 `kLimitExceeded` |
| `worker_threads`（Thread 模式） | 0（实际 `max(hw, 4)`） | 低于 `max_connections` 时排队 |
| `sniff.max_bytes` | 4096 | 远小于 HTTP 头上限 64 KiB |

### 6.8 兼容性与可回退

- 新库默认 OFF，不进入 `go2cpp_network` 的导出面，不影响现有构建与安装；
- 不修改 base/Bento 的公开头与语义；发现必须改的点（如 Link 多 peer）另立设计，不在本设计里"顺手改"；
- 每个阶段独立可回退：M1 不依赖 M2，删掉 omni 目录不影响 Bento 与 base 的现有测试；
- 命名空间 `go2cpp::bento::omni` 与现有 `go2cpp::bento` 并列，避免污染 `go2cpp::net`。

---

## 7. 未决问题

1. **`mode` 是否真的需要**。既然 fd 永远非阻塞、Channel 本身可阻塞，`Blocking` 与 `NonBlocking` 的差别只在"视图是否提供同步便捷方法"。当前的保留理由是：它能显式拒绝 `Blocking+Poll` 这种死锁配置，且贴近 base 的同步写法；若后续证明它只是便捷层，可降级为视图方法而不占配置字段。
2. **HTTP 每请求通道的分配成本**。`Channel` 是 `shared_ptr` + 环形缓冲，每个请求一次分配；1k QPS 下可接受，10 万 QPS 下需要池化。M2 压测后决定是否引入 `ResponseChannel` 池。
3. **每连接双驱动者**。首版单驱动者，写阻塞会拖慢读；M4 用压测数据决定是否拆分。
4. **路由表是否永远不做**。若多数用户需要路径路由，`http::Server::route` 的语义可以移植到 `HttpServer` 的分发 Fiber 里，而不改网络层。届时再评估。
5. **`LinkServer` 多 peer 的归属**。是改 `Link` 支持共享 socket + peer 路由，还是"每 peer 一个 `Link` + `SO_REUSEPORT`"？前者要动 Bento 核心，后者要 N 个端口。这是 M4 之后独立立项的问题，本设计不预设答案。
6. **TLS 服务端是否需要客户端证书（mTLS）**。`TlsConfig.verify_client` 已留位，但证书轮换、CRL/OCSP 不在本期。

---

## 8. 附录：事实索引

| 结论 | 证据 |
|---|---|
| Bento 只有 4 个头/4 个实现 | `include/go2cpp/bento/net/{protocol,channel,relation,link}.hpp`、`src/bento/net/*.cpp` |
| 帧头 12B、载荷上限 1300B、7 种帧类型 | `include/go2cpp/bento/net/protocol.hpp:40,47,58-66` |
| 可靠窗口 256、PTO 200ms→2s | `include/go2cpp/bento/net/channel.hpp:47-53` |
| Link 单线程、单 peer、`Close` 不触发回调 | `bento/net/link.hpp:5-9,44-47,152`；`src/bento/net/link.cpp:336-355` |
| `kMaxChannels=8`、`kMaxFramesPerRun=256` | `include/go2cpp/bento/net/link.hpp:208-210` |
| Socket 恒非阻塞、`accept4` 带 `SOCK_NONBLOCK` | `src/network/socket.cpp:156-158,222-223` |
| `close` 可能延迟关闭 | `include/go2cpp/network/socket.hpp:89-94` |
| 无独立 Listener、无连接池/路由/限流 | `docs/modules/network.md:107,285` |
| `http::Server` 单连接串行、默认 100 请求 | `include/go2cpp/network/http.hpp:215-228` |
| `Parser::take_remaining` / WS `AcceptWithPending` | `include/go2cpp/network/http.hpp:116`、`include/go2cpp/network/websocket.hpp:94-97` |
| SSE 复用 `ResponseWriter`、心跳显式 tick | `include/go2cpp/network/sse.hpp:143-168` |
| TLS 只有客户端 | `include/go2cpp/network/http2/tls_transport.hpp:49` |
| HTTP/2 核心未进库目标 | `docs/modules/network.md:57-61` |
| `MemoryTransport` 的 inbound/outbound 设计可参考 | `include/go2cpp/network/http2/transport.hpp:69-126` |
| IOManager/Fiber/epoll 能力 | `include/go2cpp/network/io.hpp:210-348` |

**一句话收尾**：OmniNetServer 的价值不在"支持多少协议"，而在把**连接生命周期、协议识别、并发驱动、背压与关闭**这五件所有服务端都要做错一遍的事，收敛成一套可测试的契约；协议只是这套契约上的可替换插件。


> **勘误（2026-10-30 18:35）**：本文基线取自 E:\CodexWorkspace\Go2Cpp 旧副本（HEAD 603c51c，Bento 早期四件套），**与 WSL 工作树最新代码不符**——WSL 已有 22 个 Bento 模块（transport/tcp_transport/memory_transport/frame/heartbeat/backoff/relay/pairing/session_table/keystore/command/fetch/file_browser/zip/backup/capability/session/session_control/peer_hub/crypto/x25519/error/reflect/udp_transport），且服务端 TLS/HTTP2 核心均在库内。§1 现状盘点与 §5 阶段路径需按最新代码复核后再用。
