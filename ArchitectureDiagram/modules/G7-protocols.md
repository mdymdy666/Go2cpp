# G7 · 应用协议：`http` / `websocket` / `sse`

> 依赖：三者都建立在 G5/G6 之上（`Socket`/`SocketStream`/`Framer` 风格 + `sync` 混合同步），
> 且都**不拥有调度器**：阻塞/唤醒策略由 `Socket` 自己决定，因此普通线程与 IOManager 管理的 Fiber 都能用。
> 规模（实测）：`network/http.hpp` 246 + `src/network/http.cpp` 620；
> `network/websocket.hpp` 176 + `src/network/websocket.cpp` 715；
> `network/sse.hpp` 205 + `src/network/sse.cpp` 613。
> 三者共同的协议边界：**HTTP/2、HTTP/3、TLS 均不在范围内**（`http.hpp` 头注释明确声明）。

---

## 0. 一句话定位

`http` 提供「消息模型 + 增量解析器 + 最小客户端 + 轻量服务端」；
`websocket` 在已连接的 `net::Socket` 上做 RFC 6455 握手/帧/分片/关闭；
`sse` 做 Server-Sent Events 的事件编码、增量解析与流式写入（复用 `http::ResponseWriter`）。
三者都是**协议边界组件**，明确不承担路由、鉴权、连接池、服务发现、重连调度。

---

## 1. `http`

### 1.1 消息模型

```cpp
enum class Method { Delete, Get, Head, Post, Put, Connect, Options, Trace, Patch, Unknown };
enum class Status : int { Continue=100, SwitchingProtocols=101, OK=200, Created=201, NoContent=204, Found=302,
                          BadRequest=400, Unauthorized=401, Forbidden=403, NotFound=404, MethodNotAllowed=405,
                          RequestTimeout=408, Conflict=409, PayloadTooLarge=413, InternalError=500,
                          NotImplemented=501, BadGateway=502, ServiceUnavailable=503, GatewayTimeout=504 };
std::string method_name(Method);  Method parse_method(string_view);  std::string status_text(Status);

class Headers {                    // 键大小写不敏感；**保留重复值**
  void add(name, value);  void set(name, value);  bool erase(name);  bool has(name);
  std::string get(name, fallback = {}) const;      // 返回首个
  std::vector<std::string> values(name) const;     // 全部重复值
  const std::vector<Entry>& entries() const noexcept;   // Entry = pair<string,string>，保序
};
class Request  { Method method; std::string target, version; Headers header, trailer;
                 std::string body; std::map<std::string,std::string> path_values; bool close;
                 path(); query(); add_cookie(); path_value(); to_string(); };
class Response { Status status; std::string version; Headers header, trailer; std::string body; bool close;
                 set_cookie(name, value, attributes); redirect(location, code); to_string(bool head_request); };
```

### 1.2 增量解析器（本模块的核心）

```cpp
struct ParserLimits { std::size_t max_header_bytes{64*1024}; max_body_bytes{64*1024*1024};
                      max_line_bytes{16*1024}; max_trailer_bytes{16*1024}; };
enum class ParseState { NeedMore, Complete, Error };
struct ParseResult { ParseState state; std::size_t consumed; std::string error; };

class Parser {
  ParseResult feed(std::string_view bytes, Request&);      // 请求
  ParseResult feed(std::string_view bytes, Response&);     // 响应
  ParseResult finish(Response&);                           // 告知 EOF：完成"以连接关闭定界"的响应
  void set_response_to_head(bool) noexcept;                // HEAD 响应头后不读 body
  std::string take_remaining();                            // 一条消息之后的多余字节
  bool finished() const noexcept;
};
```

要点：

- **半包/粘包**都由内部 `m_buffer` 处理，`NeedMore` 只表示"还差字节"；`consumed` 告知调用方本次消费了多少。
- 支持 **Content-Length、chunked、trailer**；`m_in_chunk_trailer`、`m_chunk_remaining` 是显式状态。
- 响应没有长度也没有 chunked 时，body **由连接 EOF 定界**（`m_response_close_delimited` +
  `m_body_start`），必须调 `finish()` 才会一次性交付 body。
- 超过 `ParserLimits` 或出现冲突的 `CL`/`TE` → 明确返回 `Error`。
- `take_remaining()` 把「同一次 read 中后续消息的字节」交回调用方（粘包场景必需）。
- ★ 一处已登记的事实（`codex.md` B-01）：`feed_impl` 入口只检查 `m_finished`，**不检查 `m_error`**，
  因此解析器没有"粘性错误态"——出错后重复喂入仍报错的原因是出错时缓冲区未被丢弃。

### 1.3 客户端 / 服务端 / 流式写出

```cpp
struct Result { enum class Error { None, InvalidUrl, Resolve, Connect, Timeout, Send, Receive, Parse, Closed, Cancelled };
                Error error; std::string message; std::shared_ptr<Response> response; bool ok(); };

class Client {                     // 最小 HTTP/1.1 客户端；连接不跨协程共享
  explicit Client(std::chrono::milliseconds timeout = 30s);
  Result do_request(const Request&, const std::string& host, std::uint16_t port = 80);
  Result get(url);  Result head(url);  Result post(url, body, content_type);
};
Result Get/Head/Post(...);  Request NewRequest(Method, target, body = {});

enum class StreamState { Idle, HeadersCommitted, Closed, Failed };
class ResponseWriter {             // 分块流式响应写入器（单线程/单 Fiber 持有）
  bool start(Response = {});  bool write(std::string_view chunk);  bool flush() noexcept;  bool close();
  StreamState state() const noexcept;
};

using Handler = std::function<Response(const Request&)>;
class Server {                     // 可替换的线程/Fiber 服务端核心
  explicit Server(net::Socket::Ptr listener);
  void route(Method, path, Handler);  void set_fallback(Handler);
  void serve_connection(net::Socket::Ptr connection, std::size_t max_requests = 100);
  void stop() noexcept;
};
```

辅助函数：`query_escape`/`query_unescape`、`canonical_header_key`、`parse_http_version`、
`parse_http_time`（RFC 1123/RFC 850/ctime）、`encode_query`/`decode_query`（`QueryValues = map<string, vector<string>>`，
保留重复键与空值）。

### 1.4 坑点

1. `split_url` **强制 `http://` 前缀**（无 TLS）；`Client` 每次新建 `Socket` 且默认
   `Connection: close` —— **无连接池、无 keep-alive**。
2. `Server::serve_connection` **不设置 recv/write 超时**（无限等待），
   连接级超时需由调用方在 handler 内或上层负责（`Client` 则有覆盖 connect/send/recv 的绝对 deadline）。
3. `ResponseWriter::write` 的所有底层写入都处理 partial write，
   且**不在用户锁内执行 Socket 阻塞操作**。
4. `Headers::get` 只返回**首个**值；需要全部时用 `values()` 或 `entries()`。
5. `Parser` 是**有状态单消费者**对象；多 Fiber/线程同时 `feed` 必须外部加锁或集中到一个 Fiber。

---

## 2. `websocket`（RFC 6455）

### 2.1 类型与握手

```cpp
enum class WebSocketRole { Client, Server };       // 角色决定收发帧是否必须掩码
enum class WebSocketOpcode : uint8_t { Continuation=0x0, Text=0x1, Binary=0x2, Close=0x8, Ping=0x9, Pong=0xA };
enum class WebSocketError { None, Closed, Timeout, Io, Protocol, InvalidHandshake, MessageTooLarge, InvalidState };
struct WebSocketResult { WebSocketError error; int system_error; std::string message; bool ok(); };
struct WebSocketMessage { WebSocketOpcode opcode; std::string payload; bool text(); };
struct WebSocketConfig { std::size_t max_frame_payload{16MiB}; max_message_payload{64MiB};
                         Socket::Duration io_timeout{max}; bool auto_pong{true}; bool validate_utf8{true}; };

class WebSocket final : public std::enable_shared_from_this<WebSocket> {
  static bool ValidateUpgradeRequest(const http::Request&, std::string& key, std::string* reason = nullptr);
  static http::Response BuildUpgradeResponse(const http::Request&);
  static Ptr Accept(Socket::Ptr, const http::Request&, WebSocketConfig = {}, WebSocketResult* = nullptr);
  static Ptr AcceptWithPending(Socket::Ptr, const http::Request&, std::string pending,
                               WebSocketConfig = {}, WebSocketResult* = nullptr);
  static Ptr Connect(Socket::Ptr, std::string host, std::string target = "/",
                     WebSocketConfig = {}, WebSocketResult* = nullptr);
  WebSocketResult read_message(WebSocketMessage&);       // 阻塞读取并重组；Ping/Pong 内部处理
  WebSocketResult send_text(std::string_view);  send_binary(std::string_view);
  WebSocketResult ping(std::string_view = {});  pong(std::string_view = {});
  WebSocketResult close(std::uint16_t code = 1000, std::string_view reason = {});   // 幂等
  WebSocketRole role() const noexcept;  bool closed() const noexcept;  Socket::Ptr socket() const noexcept;
};
```

### 2.2 并发模型（★ 重要）

- 一个对象**允许一个读方向 + 多个并发写调用**：`sync::Mutex m_read_mutex` 与 `m_write_mutex`
  分别串行化读/写；**读操作之间、写操作之间串行化**，同一条连接的业务消息顺序由写锁保证。
  → 注释明确说明用项目的混合 Mutex 是为了「避免受管 Fiber 在同一个 M 上阻塞 OS 线程」。
- `m_fragmented` / `m_fragment_opcode` / `m_fragment_payload`：分片消息重组状态。
- `m_prefetched` / `m_prefetched_offset`：`AcceptWithPending` 接管 HTTP Parser 已经读入但未消费的字节
  （否则握手后的首帧会丢）。
- `abort_transport()`：用 `Socket::cancel_read/cancel_write` 唤醒跨线程/跨 Fiber 的阻塞调用。
- `protocol_error(message, code = 1002)`：发对应 close code 并把连接置为不可复用。

### 2.3 协议规则（实测/注释确认）

1. **角色错向的掩码即协议错**：客户端发帧必须掩码、服务端必须不掩码，违反 → `protocol_error(1002)`。
2. **控制帧必须 ≤ 125 字节且不可分片**；违反即协议错。
3. `Ping`/`Pong` 由库内部处理（`auto_pong=true` 时自动回 Pong），
   **不会交给 `read_message` 的调用方**——这是 2026-10-13 记录过的一条示例断言错误来源。
4. `validate_utf8` 默认 true：文本帧需要合法 UTF-8；`valid_close_code` 校验关闭码合法性。
5. 客户端 `make_client_key()` 用 `std::random_device` 生成 Sec-WebSocket-Key；
   `make_accept_key()` 做 `SHA1(key + GUID)` 的 Base64（握手校验）。
6. `connect` 侧用**逐字节 `recv`** 读握手响应头（最多 64 KiB）——
   台账 X-22：功能正确但系统调用次数高三个数量级（一次握手可接受，未修）。

### 2.4 与 Go 的差异

| Go | 本项目 | 差异 |
|---|---|---|
| `gorilla/websocket` / `nhooyr.io/websocket` | `net::WebSocket` | 只做单连接协议对象；无拨号器、连接池、压缩扩展（permessage-deflate） |
| `conn.ReadMessage()` | `read_message(WebSocketMessage&)` | Ping/Pong 内部消化；分片在库内重组 |
| 无内置角色掩码检查（库各自处理） | 明确 role 决定掩码并校验 | — |
| `conn.SetReadDeadline` | `WebSocketConfig::io_timeout` | 单一超时字段，不是双向 deadline |

---

## 3. `sse`（Server-Sent Events）

### 3.1 事件模型

```cpp
enum class Error { None, InvalidEvent, MessageTooLarge, Cancelled, DeadlineExceeded,
                   Closed, Io, Backpressure, Parse, ResourceExhausted };
struct Result { Error error; int system_error; std::string message; bool ok(); static Result success(); };

struct Event {
  std::string id;        // 单行字段，不得含 CR/LF/NUL
  std::string event;     // 同上
  std::string data;      // 可多行；编码时按规范拆成多个 data 字段
  std::optional<std::uint64_t> retry_ms;
  bool valid(std::string* reason = nullptr) const;
  Result encode(std::string& output, std::size_t max_bytes = 0) const;   // 以空行结尾
};
struct StreamOptions { std::size_t max_event_bytes{1MiB};
                       std::chrono::milliseconds heartbeat_interval{15s};
                       net::Socket::Duration write_timeout{max};
                       bool add_accel_buffering_header{true}; };
struct ParserLimits  { std::size_t max_line_bytes{64KiB}; max_event_bytes{1MiB}; max_buffer_bytes{2MiB};
                       std::uint64_t max_retry_ms{24h}; };
```

### 3.2 增量解析器 `Parser`

- 支持**任意字节分片**、**LF / CRLF / CR 三种换行**、UTF-8 BOM、注释行、重复 `data` 字段、`Last-Event-ID`。
- **EOF 不会把没有空行结束的半个事件强行派发** —— 避免把截断数据误报为完整业务消息
  （这是 2026-10-13 记录的另一条示例断言错误来源：`"data: cr\r\r"` 的末尾 `\r`
  需要后续字节确认才能判定为行结束）。
- `retry_after()` / `last_event_id()` 供重连使用（`Last-Event-ID` 由调用方填入下次请求）。
- ★ 状态：`Parser` 是**有状态单消费者**对象；多线程/Fiber 同时 `feed` 必须外部加锁或集中到一个 Fiber。

### 3.3 流写入器 `Stream`

```cpp
enum class StreamState { Idle, Open, Closed, Failed };   // 状态只向前迁移，失败后不能复用连接
class Stream {
  Stream(net::Socket::Ptr, ContextPtr = {}, StreamOptions = {});
  Result start();                       // 发送 200 + text/event-stream
  Result send(const Event&);            // 失败后流进入 Failed
  Result comment(std::string_view text = "heartbeat");
  Result tick();                        // 到心跳时间才发注释，否则不写任何字节
  Result close();                       // chunked 终止块；幂等
  StreamState state() const noexcept;  bool active() const noexcept;
  bool heartbeat_due() const noexcept;  std::chrono::steady_clock::time_point next_heartbeat() const noexcept;
};
void PrepareResponse(http::Response&, bool add_accel_buffering_header = true);
std::string LastEventId(const http::Request&);
```

- 内部复用 `http::ResponseWriter`（chunked）；`sync::Mutex m_mutex` —— 注释明确：
  「写入可能在 Fiber 中 park，**必须使用 Fiber/线程混合锁，不能使用裸 `std::mutex`**」。
- **互斥只保证字节不交错，不承诺业务事件顺序**；生产者仍需控制顺序与背压，模块**不建立无界队列**。
- Context 取消时标记流并调用 `cancel_write` 唤醒阻塞写（`arm_context_callback`/`disarm_context_callback`
  + `DoneSignal::CallbackId m_callback_id`）。
- 三个查询接口（`state()`/`heartbeat_due()`/`next_heartbeat()`）都被声明为 `noexcept`，
  且**内部 try/catch 兜住加锁异常**（项目里「noexcept 查询接口要兜住加锁异常」策略的正面样本）。

### 3.4 与 Go 的差异

| Go | 本项目 | 差异 |
|---|---|---|
| 无标准库 SSE（常用 `r3labs/sse` 等） | `sse::Event/Parser/Stream` | 提供编码 + 增量解析 + 流写入三段 |
| `http.Flusher` 手动 flush | `ResponseWriter` + `StreamOptions::write_timeout` | 显式 chunked 写入器 |
| 重连/Last-Event-ID 由客户端库决定 | `Parser::last_event_id()` / `retry_after()` + `LastEventId(request)` | 只提供协议字段，不实现重连调度 |

---

## 4. 对架构图的贡献

**节点**

- `HTTP`（`Headers`（保重复、大小写不敏感）/`Request`/`Response`/`Parser`（CL+chunked+trailer+EOF 定界）/
  `Client`/`Server`/`ResponseWriter`）
- `WebSocket`（`ValidateUpgradeRequest`/`Accept`/`Connect` + 帧编解码 + 分片重组 + 读/写双锁）
- `SSE`（`Event` 编码 / `Parser` 增量解析 / `Stream` 流写入 + 心跳注释）

**连线**

- `HTTP Client → Socket`：「每次新建连接 + Connection: close（无连接池）」
- `HTTP Server ⇐ Socket`：「serve_connection 单 Fiber 串行、不设超时」
- `WebSocket → http`：「复用 Upgrade 请求校验与 101 响应构造」
- `WebSocket → Socket`：「读/写分别用 sync::Mutex 串行；abort_transport 取消等待」
- `SSE → http::ResponseWriter`：「chunked 分块写出；心跳用 SSE 注释」
- `SSE Parser/Stream → Context`：「取消 → cancel_write 唤醒阻塞写」
- `WebSocket/SSE → sync::Mutex`：「混合锁，保证 Fiber 阻塞时不占住 M」

**图内文字**

> 应用协议层是**协议边界组件**：不拥有调度器、不做路由/鉴权/连接池。
> `http::Parser` 是单消费者增量状态机（支持半包/粘包/CL/chunked/trailer；
> 无长度响应必须 `finish()` 由 EOF 定界）。
> `WebSocket` 允许「一个读 + 多个写」并发（读锁/写锁分开），
> Ping/Pong 由库内部消化、分片在库内重组，角色错向的掩码即协议错。
> `SSE` 支持 LF/CRLF/CR 三种换行与 UTF-8 BOM，EOF 不派发未以空行结束的半个事件；
> 流写入用 Fiber/线程混合锁，模块不建立无界队列（背压交给生产者）。
> 三者共同边界：**无 TLS、无 HTTP/2/3**。
