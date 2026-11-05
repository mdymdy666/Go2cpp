# 网络栈（交接文档）

> **定位**：与 Go `net` 包对等的网络栈——地址/监听/连接、epoll IO 管理器、syscall Hook（阻塞转 park）、HTTP/1.1、HTTP/2、TLS 封装、WebSocket、SSE、QUIC/HTTP3 薄封装。
> **源码**：`include/go2cpp/network/**`、`src/network/**`（约 1.8 万行实现）　**构建**：`cmake/Go2CppModules.cmake`，目标 `go2cpp_network`（Hook 另建 `go2cpp_hook` 共享库）
> **测试**：`tests/network/`（30 个文件：`test_network` / `test_hook*` / `test_http2*` / `test_http_e2e` / `test_sse` …）
> **设计全文**：[network.md](../../docs/modules/network.md)（总览）、[http1.md](../../docs/modules/http1.md)、[http2.md](../../docs/modules/http2.md)、[tls.md](../../docs/modules/tls.md)、[ws.md](../../docs/modules/ws.md)、[sse.md](../../docs/modules/sse.md)
> **配套**：可运行示例 [user/network/README.md](../network/README.md)

## 1. 这个模块是什么

一句话：**用同步阻塞的写法写出 Go 风格的网络服务**。`Socket::accept/recv/connect`
看起来是阻塞调用，实际在受管 Fiber 上会 park 并把 OS 线程让出去；在普通线程上则退化为
原生阻塞。同一份代码，两种执行体都能跑。

它覆盖的协议栈（自下而上）：

| 层 | 内容 | 是否在 `go2cpp_network` |
|---|---|---|
| 地址 / 套接字 | `IPv4Address` / `IPv6Address` / `UnixAddress`、`net::Socket`（TCP/UDP/Unix，批量收发、`sendfile`） | 是 |
| 事件 / Hook | `io::IOManager`（epoll ET）；22 个 `HookOp` 把 libc 阻塞调用转成 park | 是（Hook 为 `go2cpp_hook`） |
| HTTP/1.1 | `http::Request/Response/Parser/Router/Server/Client/ResponseWriter` | 是 |
| HTTP/2 | `http2::Connection` + HPACK + 帧 + 流状态机 | **否**，见 §7.2 |
| TLS | `http2::TlsTransport`（OpenSSL 封装，仅客户端） | 部分是 |
| WebSocket | `net::WebSocket`（RFC 6455） | 是 |
| SSE | `go2cpp::sse::Stream` / `Parser` | 是 |
| QUIC/HTTP3 | `quic::PicoquicConnection`、`http3::Client` | 否，`GO2CPP_WITH_PICOQUIC=ON` 才编 |

**不在这里**：MQTT 3.1.1、CoAP、SOCKS5 在 `bento::net`（`src/bento/net/{mqtt,coap,socks5}.cpp`），
需要 `GO2CPP_WITH_BENTO=ON`，见 [03-bento.md](03-bento.md)。常见误判是「网络模块=全部协议」，
实际这里只管 `go2cpp::network` 命名空间。

## 2. 架构 / 数据流

```
  业务代码（同一份，Fiber 或普通线程都能跑）
      │  Socket::accept / recv / send / connect / http::Server::serve_connection
      ▼
  ┌──────────────────────────────────────────────────────────────┐
  │ go2cpp::net::Socket（fd 生命周期 + in-flight 保护）           │
  └──────────────┬───────────────────────────────────────────────┘
                 │ 真阻塞？先问 hook 是否接管（仅 Linux，BUILD_HOOK）
                 ▼
  ┌──────────────────────────────────────────────────────────────┐
  │ hook（22 个 HookOp）→ io::IOManager（epoll EPOLLET，重 arm）  │
  │   可关闭 14 / 硬拒绝 5 / 语义空 3；shutdown 从未被 hook        │
  │   EAGAIN → 注册等待；就绪 → poller 线程 wake_io()/wake_io_batch│
  │   或空闲 worker 自己在 M 上 poll（set_io_idle_poller）        │
  └──────────────┬───────────────────────────────────────────────┘
                 │ 调度器把 G 放回 P 队列（见 01-concurrency.md）
                 ▼
  ┌──────────────────────────────────────────────────────────────┐
  │ Scheduler → Fiber 从 park 点继续                              │
  └──────────────────────────────────────────────────────────────┘
```

### 2.1 两种 IO 驱动模型

| 模型 | 谁发现就绪 | 代价 | 何时用 |
|---|---|---|---|
| 独立 poller 线程 + `wake_io` | `IOManager` 的 poller | 跨线程唤醒一次往返 **54–56 µs**（WSL2 实测） | 默认 |
| worker 自己 poll（`set_io_idle_poller`） | 空闲 worker | 无跨线程唤醒 | 高并发小包（Go `netpoll` 同构） |

### 2.2 HTTP/1.1 的单连接生命周期（`Server::serve_connection`）

```
accept → 循环 max_requests(默认 100) 次：
    Parser::feed(bytes) → NeedMore（继续读） / Complete（拿到 Request）
      → Router 匹配（每方法精确哈希 + 参数/通配 trie）→ Handler(request) → Response
      → Response::to_string() 自动补 Content-Length / Connection: close → 写回
      → 若 request.close 或 stop() 则退出
```

pipelining 天然支持：`Parser` 内部缓冲可能已有多条消息，`take_remaining()` 取走剩余字节
交给下一条。单连接 pipeline=16 实测 **72.9k req/s**（串行 8.9k 的 **8.2×**）。

### 2.3 线程模型（交接重点）

- 服务端默认只用 **2 个 OS 线程**（与 Go `GOMAXPROCS=2` 同口径）；每连接一个 Fiber。
- `Socket::close()` 会等在途操作退出（`begin_operation`/`end_operation` + 延迟关闭），所以
  「另一个 Fiber 正在 recv、这个 Fiber 调 close」是安全的。`http::Server` **不启动 accept 循环**：
  构造需要注入已监听的 socket，由调用方 accept 后调 `serve_connection()`（见 §7.4）。

## 3. 对外 API（关键接口）

### 3.1 Socket 与地址

```cpp
// include/go2cpp/network/socket.hpp
class Socket final : public std::enable_shared_from_this<Socket> {
public:
    using Duration = std::chrono::milliseconds;
    static constexpr std::size_t kMaxBatch = 1024;          // 与 UIO_MAXIOV 对齐
    static Ptr CreateTCP(int family = AF_INET);
    static Ptr CreateUDP(int family = AF_INET);
    static bool SetTcpNoDelay(int fd) noexcept;             // accept() 默认已调用
    bool bind(const Address& address);
    bool listen(int backlog = SOMAXCONN);
    Ptr accept(Duration timeout = Duration::max());
    bool connect(const Address& address, Duration timeout = Duration::max());
    bool close() noexcept;
    SocketResult send(const void* data, std::size_t length, int flags = 0,
                      Duration timeout = Duration::max());   // 单次 write，可短写
    SocketResult SendAll(const void* data, std::size_t length, int flags = 0,
                         Duration timeout = Duration::max()); // 2026-11-05 补：循环到全量
    SocketResult recv(void* data, std::size_t length, int flags = 0,
                      Duration timeout = Duration::max());
    SocketResult sendv(const iovec* buffers, std::size_t count, int flags = 0, Duration timeout = Duration::max());
    SocketResult send_to(const void* data, std::size_t length, const Address& to,
                         int flags = 0, Duration timeout = Duration::max());
    SocketResult recv_from(void* data, std::size_t length, AddressPtr& from,
                           int flags = 0, Duration timeout = Duration::max());
    SocketResult recv_batch(RecvBatchEntry* entries, std::size_t count, int flags = 0,
                            Duration timeout = Duration::max());   // recvmmsg(2)
    SocketResult send_batch(const SendBatchEntry* entries, std::size_t count, int flags = 0,
                            Duration timeout = Duration::max());   // sendmmsg(2)
    SocketResult send_file(int in_fd, off_t* offset, std::size_t count,
                           Duration timeout = Duration::max());    // sendfile(2) 零拷贝
    AddressPtr local_address() const;
    AddressPtr remote_address() const;
    bool set_recv_timeout(Duration timeout);  bool set_send_timeout(Duration timeout);
    bool cancel_read() noexcept;  bool cancel_write() noexcept;   // 跨线程取消
};

struct SocketResult { ssize_t transferred{-1}; int error{0}; bool ok() const noexcept; };
```

`SocketResult::ok()` 要求 `transferred >= 0 && error == 0`。**部分成功不重试**：`send_batch` 的
`transferred` 可能小于 count，调用方把 `[transferred, count)` 作为下一批继续（重试会重复发送）。

`send()`/`recv()` 的 `timeout` 是**三态**（2026-11-05 修正，`socket.hpp` 的 H-02/H-03）：
`Duration::max()`（默认）**不是无限**，而是回落到 `recv_timeout()`/`send_timeout()` 配置，
只有配置也为 max 时才真正无限；有限正数 = 本次等待上限；`Duration(0)` = 合法的「不等待」，
不可读/不可写时立即返回 `{-1, ETIMEDOUT}`（不是清除超时）；负数 = `EINVAL`。
大包必须用 `SendAll()`：`send()` 部分成功仍 `ok()==true`，直接重试会重发前缀。

### 3.2 HTTP/1.1

```cpp
// include/go2cpp/network/http.hpp（namespace go2cpp::http）
class Client final {
    explicit Client(std::chrono::milliseconds timeout = std::chrono::seconds(30));
    Result do_request(const Request& request, const std::string& host, std::uint16_t port = 80);
    Result get(const std::string& url);
    Result post(const std::string& url, std::string body, std::string content_type = "application/octet-stream");
};

class Server final {
    explicit Server(net::Socket::Ptr listener);           // 注入 listener，不启动 accept 循环
    void route(Method method, std::string path, Handler handler);   // "/users/:id"、"/*" 都支持
    void set_fallback(Handler handler);
    void serve_connection(net::Socket::Ptr connection, std::size_t max_requests = 100);
    void stop() noexcept;
};

class ResponseWriter final {   // chunked 流式响应（SSE 复用它）
    bool start(Response response = {});   // 强制 chunked，丢弃 Content-Length
    bool write(std::string_view chunk);
    bool flush() noexcept;                // 只检查可写，不写字节（历史命名）
    bool close();
};
```

`ParserLimits` 默认：`max_header_bytes=64KiB`、`max_body_bytes=64MiB`、`max_line_bytes=16KiB`。
`Headers` 键大小写不敏感、**保留重复值**、保持写入顺序，`get()` 只取第一个。

### 3.3 WebSocket / SSE / TLS

```cpp
// include/go2cpp/network/websocket.hpp（namespace go2cpp::net）
static Ptr WebSocket::Accept(Socket::Ptr, const http::Request&, WebSocketConfig = {}, WebSocketResult* = nullptr);
static Ptr WebSocket::Connect(Socket::Ptr, std::string host, std::string target = "/", WebSocketConfig = {}, WebSocketResult* = nullptr);
WebSocketResult read_message(WebSocketMessage&);   // 组装完整消息，Ping/Pong 内联处理
WebSocketResult send_text(std::string_view);       // 本地 UTF-8 + 消息上限校验

// include/go2cpp/network/sse.hpp（namespace go2cpp::sse）
Stream(net::Socket::Ptr, ContextPtr = {}, StreamOptions = {});   // 内部用 ResponseWriter chunked
Result start();  Result send(const Event&);  Result tick();  Result close();
Parser(std::string_view bytes, std::vector<Event>& out);         // 有状态单消费者

// include/go2cpp/network/http2/tls_transport.hpp
static std::unique_ptr<TlsTransport> CreateClient(Socket::Ptr, const std::string& server_name,
                                                  bool verify_peer = true,
                                                  Duration timeout = 10s,
                                                  const std::string& ca_file = {}, const std::string& ca_path = {});
bool Handshake(std::string* error = nullptr);
std::size_t Read(std::uint8_t*, std::size_t);   std::size_t Write(const std::uint8_t*, std::size_t);
const std::string& NegotiatedProtocol() const noexcept;   // ALPN，如 "h2"
```

`WebSocketConfig` 默认：`max_frame_payload=16 MiB`、`max_message_payload=64 MiB`、
`auto_pong=true`、`validate_utf8=true`、`fragment_outgoing=true`。
`TlsTransport::verify_peer` 默认 **true**（fail-closed，审计后从 false 改过来的）。

## 4. 最小用法（可编译示例）

```cpp
#include "go2cpp/runtime.hpp"
#include <chrono>
#include <iostream>

int main() {
    using namespace go2cpp;
    auto& sched = default_scheduler();   // 2026-11-05 修正：默认调度器是进程级单例

    auto listener = net::Socket::CreateTCP(AF_INET);
    if (!listener->bind(*net::IPv4Address::Create("127.0.0.1", 0)) || !listener->listen(8)) return 1;
    // 2026-11-05 修正：port() 在 IPAddress 子类，不在 Address 基类，需向下转型
    const auto local = std::dynamic_pointer_cast<net::IPAddress>(listener->local_address());
    if (!local) return 1;
    const auto port = local->port();   // 0 → 内核分配

    auto server = go(sched, [listener] {                    // Fiber 上 accept 会 park
        auto conn = listener->accept(std::chrono::milliseconds(5000));
        if (!conn) return;
        char buf[64]{};
        auto got = conn->recv(buf, sizeof(buf), 0, std::chrono::milliseconds(5000));
        if (got.ok()) {
            (void)conn->send(buf, static_cast<std::size_t>(got.transferred), 0,
                             std::chrono::milliseconds(5000));
        }
    });

    auto client = net::Socket::CreateTCP(AF_INET);
    if (!client->connect(*net::IPv4Address::Create("127.0.0.1", port), std::chrono::milliseconds(5000))) return 1;
    const std::string msg = "ping";
    (void)client->send(msg.data(), msg.size());
    char reply[64]{};
    auto got = client->recv(reply, sizeof(reply));
    std::cout << "echo=" << std::string(reply, got.ok() ? got.transferred : 0) << '\n';
    if (!server) return 1;              // 空句柄 = 未提交，必须判空（2026-11-05 修正）
    server->wait();
    shutdown_default_scheduler();
    return 0;
}
```

编译：`g++ -std=c++17 -O2 -I include demo.cpp build/libgo2cpp_network.a
build/libgo2cpp_scheduler.a build/libgo2cpp_io.a -lpthread`（Hook 需 `go2cpp_hook` 共享库，
只影响 libc 直调，本示例走 `Socket` 不需要）。HTTP 纯解析示例见
[user/network/http_demo.cpp](../network/http_demo.cpp)（不建连接，`Parser::feed` 往返）。

## 5. 源码位置

| 文件 | 行数 | 职责 |
|---|---:|---|
| `include/go2cpp/network/socket.hpp` / `src/network/socket.cpp` | 346 / 1188 | `Socket` 全 API、fd in-flight 保护、批量/零拷贝收发 |
| `include/go2cpp/network/address.hpp` / `src/network/address.cpp` | 240 / 344 | 三种地址族、`Create` 长度校验、`Lookup` |
| `include/go2cpp/network/io.hpp` / `src/network/io.cpp` | 360 / 2949 | `IOManager`（epoll ET、重 arm、批量唤醒） |
| `include/go2cpp/network/hook.hpp` / `src/network/hook.cpp` | 405 / 2979 | 22 个 `HookOp`、符号拦截、开关与降级 |
| `include/go2cpp/network/http.hpp` / `src/network/http.cpp` | 585 / 1498 | 消息模型、`Parser`、`Router`、`Client`、`Server`、`ResponseWriter` |
| `include/go2cpp/network/websocket.hpp` / `src/network/websocket.cpp` | 278 / 1018 | 握手、帧编解码、分片、UTF-8 校验 |
| `include/go2cpp/network/sse.hpp` / `src/network/sse.cpp` | 341 / 808 | `Stream` 写侧、`Parser` 读侧、心跳 |
| `include/go2cpp/network/http2/connection.hpp` / `src/network/http2/connection.cpp` | 502 / 1444 | 连接层：preface、SETTINGS、流状态机、流控 |
| `include/go2cpp/network/http2/hpack.hpp` / `src/network/http2/hpack.cpp` | 324 / 786 | HPACK 静态/动态表、整数与字符串编码 |
| `include/go2cpp/network/http2/frame.hpp` / `src/network/http2/frame.cpp` | 375 / 651 | 9 种帧的编解码 |
| `include/go2cpp/network/http2/tls_transport.hpp` / `src/network/http2/tls_transport.cpp` | 244 / 753 | OpenSSL 封装（仅客户端）、ALPN、SNI |
| `include/go2cpp/network/http3/client.hpp` / `src/network/http3/client.cpp` | 466 / 1348 | HTTP/3 客户端（picoquic，默认不编） |

## 6. 性能数据

### 6.1 vs Go（6 协议 × 14 场景，2026-10-31）

来源：[docs/reports/2026-11-01-net-vs-go.md](../../docs/reports/2026-11-01-net-vs-go.md) 与
[docs/reports/vs-network.md](../../docs/reports/vs-network.md)。口径：WSL2 回环、64B 消息、
串行 ping-pong、服务端 `taskset -c 2,3`、Go `GOMAXPROCS=2`、我们 2 个 OS 线程。

| 场景 | Go（16 连接） | 我们（16 连接） | 比值 |
|---|---:|---:|---:|
| TCP echo 64B | 90,638 | **110,501** | 1.22× |
| UDP echo 64B（批量） | 60,748 | **176,202** | 2.90× |
| HTTP GET /hello | 68,943 | **92,875** | 1.35× |

**结论原样引用**：「6 协议 × 14 场景，**14/14 达标（要求 ≤1.1× Go 耗时），且 14/14 全部快于
Go**，领先 **1.07–2.90×**」；最差项是 WebSocket 16 连接 1.07×，最大项是 UDP 16 连接 2.90×
（靠 `recvmmsg`/`sendmmsg` 批量 syscall）。**vs muduo/libevent/asio/Seastar 的网络吞吐未测**，
只有队列层实测（muduo `BlockingQueue` 32.40 ns vs 我们 `FastChannel` 10.73 ns）。
### 6.2 HTTP 端到端（2026-11-02）

来源：[docs/reports/asm-http-e2e.md](../../docs/reports/asm-http-e2e.md)（`-O3 -DNDEBUG`，GCC 13.3）。

| 项 | 结果 |
|---|---|
| 路由 | 线性扫描 → 每方法精确哈希 + 参数/通配 trie；64 条静态路由 O(1) |
| 静态文件 | `Response::file` + `sendfile(2)` 零拷贝；200/206/304/400/403/413/416、单区间 Range、ETag |
| pipelining | pipeline=16 单连接 **72.9k req/s**（串行 8.9k 的 **8.2×**）；新增 **260 条**真实 socket 断言 0 失败 |
| 对标 nginx（2 worker） | 多连接 hello **1.03–1.89×**、静态 **0.96–1.79×**（最差慢 4%） |
| 对标 Go net/http | hello **1.28–3.21×**、路由 **1.20–1.28×**、静态 **1.07–1.40×** |

### 6.3 TLS（2026-11-02）

来源：[docs/reports/asm-tls.md](../../docs/reports/asm-tls.md)。`TlsTransport` 对裸 OpenSSL：
握手延迟（共享 `SSL_CTX`）mean **1.070×**、p50 1.091×、p99 1.120×；32KB 写吞吐
0.933–1.008×（均值 0.975×）；64B 小包写延迟 0.946–1.064×（优化前 1.29–1.35×），全部 <1.1×。
同一报告的另一结论要一并记住：**仓库自研 AEAD（`src/bento/net/crypto.cpp`，不在 TLS 路径上）
仅在 ≤64B 领先（0.52×），≥256B 起被 OpenSSL AVX2/AVX512 拉开，1MB 时 13.41×**。

> ⚠️ **上句是 2026-11-02 的历史快照，已被推翻**：2026-11-04 的 16 路 AVX-512 IFMA 复测
> 把 1 MiB 从 13.41× 降到 **1.03–1.04×**（64 KiB 1.02–1.03×），见
> `docs/reports/asm-tls.md` §「冲 1.1×（2026-11-04）」。本文是交接摘要，保留旧值仅作对照。

## 7. 边界与已知限制

1. **Hook 只在 Linux 生效**：`GO2CPP_BUILD_HOOK=ON` 且 Linux 才构建 `go2cpp_hook`；
   `shutdown` **从未被 hook**（旧文档误记为已覆盖），对端关闭检测依赖 `read` 返回 0。
2. **HTTP/2 核心源文件不在 `go2cpp_network` 里**：`connection.cpp`/`frame.cpp`/`hpack.cpp`/
   `huffman.cpp`/`stream_state.cpp` 只在测试 target 中编译，**没有 `go2cpp_http2` 库目标**，
   生产要用必须自己把这些 `.cpp` 加进 target。
3. **TLS 只有客户端**：没有 `SSL_accept` 路径、没有会话复用/0-RTT/ticket；`verify_peer=false`
   会同时关链校验和主机名校验；空 `server_name` 不设 SNI 也不做主机名匹配。
4. **`http::Server` 不 accept**：构造注入 listener，调用方自己 `accept()` 再
   `serve_connection()`；后者**不设 recv/write 超时**（无限等待），连接级超时要上层负责。
5. **`Parser::feed` 的 `consumed` 语义**（2026-11-05 修正）：`consumed` 是**本条消息在
   内部缓冲中占用的字节数**（跨 feed 累积；`ParseResult` 不变量要求 Complete 时 `consumed>0`），
   不是「本次喂入的字节数」，也不是「已消费偏移」。多读的剩余字节必须用 `take_remaining()` 取；
   后者保留 `set_response_to_head()` 的连接级标志（流水线下一条仍服务同一 HEAD 请求），
   只有 `reset()` 清零。
   **`ResponseWriter::flush()` 不写字节**（只检查可写，历史命名）；**`http::Client` 不复用连接**。
6. **QUIC/HTTP3 默认 OFF**：`GO2CPP_WITH_PICOQUIC=ON` 才编，**无 ctest**，只做过编译验证。
7. **WebSocket 所有权**：`Accept/Connect` 拿 `Socket::Ptr`，析构只 `abort_transport()`
   **不 close fd**；读方向单线程（`m_read_mutex`），写方向可多线程。**SSE `Parser` 是有状态单
   消费者**，并发 `feed()` 必须调用方加 `sync::Mutex`。
8. **MQTT/CoAP/SOCKS5 不在本模块**：见 §1 与 [03-bento.md](03-bento.md)。

## 8. 测试与验证方法

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DGO2CPP_BUILD_TESTS=ON -DGO2CPP_BUILD_HOOK=ON
cmake --build build -j && ctest --test-dir build -R "network|hook|http|sse|http2|tls" --output-on-failure
./build/test_network          # TCP/UDP/Unix、超时、close 竞态
```

| 主题 | 代表文件 |
|---|---|
| Socket/地址/IO | `test_network.cpp`、`test_connection.cpp`、`test_io.cpp`、`test_fd_manager.cpp` |
| Hook | `test_hook.cpp`、`test_hook_p0.cpp`、`test_hook_fifo.cpp`、`test_hook_correctness.cpp` |
| HTTP/1.1 | `test_http_e2e.cpp`（260 条断言，自包含 main）、`net_perf_server.cpp`（基准服务端） |
| HTTP/2 / WS / SSE / TLS | `test_http2_boundary.cpp`、`test_http2_stress.cpp`、`test_hpack.cpp`、`test_sse.cpp`、`test_tls_alpn.cpp` |

压测：`tests/network/bench_net_batch.cpp`（批量 syscall）、`http_loadgen.go`（Go 客户端，
与服务端同机跑）、`net_perf_server.cpp`（`hello`/`route`/`static` 三个变体）。
⚠️ 测性能必须**绑核并记录 loadavg**，否则 WSL2 上数字飘 ±30%。

## 9. 交接注意

- **先分清「本模块」与「bento」**：MQTT/CoAP/SOCKS5/HTTP 服务端在 `bento::net`，改协议实现前
  先 `grep -rn "namespace go2cpp::net" include/`，别在错误的目录里找。
- **Hook 是全局状态**：`SetOpEnabled(op, false)` 之后**必须恢复**，否则后续所有 Fiber 静默变慢；
  测试里用 RAII 包装（`hook.hpp` 的注释已警告「容易忘恢复」）。
- **HTTP/2 的构建坑**：新目标用到 `http2::Connection` 时链接错误是「未定义符号」，因为库目标里
  根本没有这些 `.o`——先把 §7.2 的五个 `.cpp` 加进 target。
- **`serve_connection` 的 max_requests=100**：长连接超过 100 个请求会被主动断开；压测 keep-alive
  时如果 100 个请求后掉线，不是 bug。
- **`accept()` 默认开 `TCP_NODELAY`**：关掉会让小包往返被延迟 ACK 压到约 40 ms
  （`docs/reports/2026-10-31-http-perf.md` §6.1）。`SetTcpNoDelayDefault(false)` 只用于对比实验。
- **fd 是瞬时值**：`Socket::fd()` 不进入 in-flight 保护，取到后可能被 close/复用（ABA），
  **不要跨操作持有**。
- **未完成事项**：HTTP/2 服务端 + h2 over TLS 全链路未验证（缺 nginx/nghttpd 对端）、TLS 服务端/
  会话复用、QUIC ctest 仍缺；`hpack.cpp` 的 unused-function 告警已修复（当前源码无 unused 告警点）。
