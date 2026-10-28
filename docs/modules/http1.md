# 模块文档：http1（HTTP/1.x）

> **状态：现行**。基线 `HEAD 2ba2f4e`（2026-10-26 核对；本文所有 `file:line` 均指该工作树）。
> 本文是 `go2cpp::http`（HTTP/1.x 消息、增量解析、客户端、轻量服务端、chunked 流式响应）
> 的**唯一现行文档**：概述、API、设计、边界、错误码、测试都在这里。
> 旧文档 `docs/network.md` 的 `## HTTP` 一节已并入本文，该文件顶部已加合并标注；
> `network.md` 的 Socket / WebSocket / SSE / Hook 章节仍是现行说明，不在本文范围。
> 相关文档：[io.md](io.md)（Socket 等待与 Hook）、[../websocket.md](../websocket.md)、
> [../sse.md](../sse.md)、[http2.md](http2.md)。
> 引用约定：代码位置用 `file:line`；测试数据来自 `tests/network/` 的断言，未实测的数字不写。
> 为节省表格宽度：`http.hpp` = `include/go2cpp/network/http.hpp`，`http.cpp` = `src/network/http.cpp`，
> `test_network.cpp` = `tests/network/test_network.cpp`，`test_http2_state_matrix.cpp` = `tests/network/test_http2_state_matrix.cpp`。

---

## 1. 概述与职责

`go2cpp::http` 是 HTTP/1.x 的**应用层**实现：消息模型、字节流增量解析器、最小客户端、
可注入监听 socket 的轻量服务端，以及 chunked 流式响应写入器。它只依赖 `net::Socket`，
不依赖调度器——普通线程和 `IOManager` 的 Fiber 里都能用（`include/go2cpp/network/http.hpp:12-13`）。

| 做 | 不做 |
|---|---|
| 请求/响应消息模型与序列化（`Request`/`Response`/`Headers`） | 不做 HTTP/2/HTTP/3、h2c 升级（`http.hpp:7-8`） |
| 增量解析：半包、粘包、`Content-Length`、严格单一 `chunked`、trailer | 不做 `Expect: 100-continue` 的**服务端**处理 |
| 客户端：解析 URL、建连、发送、收响应、全局 deadline | 不做重定向跟随、连接池、TLS、代理 |
| 服务端：单连接 keep-alive 循环、method+path 精确路由、fallback | 不做 accept 循环、Fiber 调度、TLS、连接级超时 |
| `ResponseWriter`：chunked 流式输出（SSE 复用） | 不做 gzip/deflate，不做 multipart 自动解析 |
| 查询串编解码、HTTP 版本/时间解析等工具 | body 始终是**有限内存缓冲**，不是流 |

---

## 2. 文件布局与规模

| 路径 | 内容 | 规模 |
|---|---|---|
| `include/go2cpp/network/http.hpp` | 协议常量、`Headers`/`Request`/`Response`、`Parser`、`Client`、`ResponseWriter`、`Server` | 412 行 |
| `src/network/http.cpp` | 解析器、URL 拆分、客户端、chunked 写入、服务端循环 | 717 行 |
| `tests/network/test_network.cpp` | HTTP/1.x 解析/客户端/Writer 断言（与 Socket 测试同一二进制） | 690 行（HTTP 部分约 140-325 行） |
| `tests/network/test_http2_state_matrix.cpp` | `ResponseWriter` 4 状态 × 5 事件矩阵 | 20 格（`543-633`） |
| `example/network/http_demo.cpp`、`http_full.cpp` | 客户端/服务端用法示例（在 `GO2CPP_EXAMPLES` 注册，但只在 `GO2CPP_BUILD_TESTS=ON` 时构建） | — |
| `include/go2cpp/network/http.h` | 兼容旧根路径的转发头 | — |

`go2cpp_network` 库只把 `src/network/http.cpp` 编进去（`cmake/Go2CppModules.cmake:46-54`），
HTTP/1 不需要单独 target。

---

## 3. API 表

### 3.1 协议常量与名字转换（`http.hpp:49-58`）

| API | 说明 | 实现 |
|---|---|---|
| `enum class Method` | DELETE/GET/HEAD/POST/PUT/CONNECT/OPTIONS/TRACE/PATCH/Unknown | `http.hpp:49` |
| `enum class Status` | 100/101/200/201/204/302/400/401/403/404/405/408/409/413/500/501/502/503/504 | `http.hpp:50-55` |
| `method_name()` / `parse_method()` | 方法名互转；大小写敏感，未知返回 `Unknown`/空串 | `http.cpp:205-222` |
| `status_text()` | 状态码 → reason phrase；未知码返回空串 | `http.cpp:223-231` |

### 3.2 消息模型（`http.hpp:73-156`）

| 类型 | 关键成员/方法 | 语义 |
|---|---|---|
| `Headers` | `add/set/erase/has/get/values/entries/clear` | 键大小写不敏感、**保留重复值**、保持写入顺序（`http.hpp:60-72`）；`get()` 只取第一个 |
| `Request` | `method/target/version/header/trailer/body/path_values/close`；`path()/query()/add_cookie()/path_value()/to_string()` | `target` 为 origin-form；`body` 是已完整读取的有限缓冲；`close` 表示本次消息后关闭（`http.hpp:91-126`） |
| `Response` | `status/version/header/trailer/body/close`；`set_cookie()/redirect()/to_string(bool head_request)` | `to_string()` 自动补 `Content-Length`/`Connection: close`，并过滤 1xx/204/304 的 framing 头（`http.hpp:128-156`、`http.cpp:266-286`） |

`Request::to_string()` 对非法 method/target/version 返回**空串**（`http.cpp:255-263`）；
`Response::to_string()` 对非法 version 同样返回空串（`http.cpp:266-268`）。
`Headers::add()` 对非法字段名/值（CR/LF 等）**静默丢弃**（`http.cpp:239-244`），这是出站
CRLF 注入的第一道闸。

### 3.3 解析器（`http.hpp:158-257`）

| API | 说明 | 实现 |
|---|---|---|
| `ParserLimits` | `max_header_bytes=64KiB`、`max_body_bytes=64MiB`、`max_line_bytes=16KiB`、`max_trailer_bytes=16KiB` | `http.hpp:158-164` |
| `ParseState` | `NeedMore` / `Complete` / `Error` | `http.hpp:166` |
| `ParseResult` | `state/consumed/error`；`operator bool()` 只排除 `Error`（`NeedMore` 仍为 true） | `http.hpp:178-183` |
| `feed(bytes, Request&)` / `feed(bytes, Response&)` | 追加字节并尝试解析一条完整消息 | `http.cpp:290-291` |
| `finish(Response&)` | 通知输入 EOF，完成**仅以连接关闭定界**的响应 | `http.cpp:292-308` |
| `set_response_to_head(bool)` | 告诉解析器当前响应对应 HEAD，头后不读 body | `http.hpp:225-226` |
| `take_remaining()` | 取走缓冲区中下一条消息的剩余字节，并 `reset()` | `http.cpp:309` |
| `finished()` / `reset()` / `limits()` | 完成标志、复位、只读上限 | `http.hpp:228-229`、`http.cpp:288-289` |

> **`consumed` 的实际语义**：`feed()` 的 Complete/NeedMore 分支返回的 `consumed` 是**本次
> 喂入的字节数**，不是“已消费偏移”（`http.cpp:441`、`http.cpp:330`）；真正的剩余字节在
> 内部缓冲里，必须用 `take_remaining()` 取（`http.cpp:309`）。`http.hpp:200` 的
> “consumed 之前的字节已交付”是**过期注释**，不要据此切流。只有 `finish()` 返回
> `consumed == m_buffer.size()`（`http.cpp:304-307`）。

### 3.4 查询串与工具（`http.hpp:259-270`）

| API | 说明 |
|---|---|
| `query_escape/query_unescape` | RFC 3986 风格；空格 ↔ `+`，非保留字符 `alnum - _ . ~`；非法 `%` 返回 false（`http.cpp:444-445`） |
| `encode_query/decode_query` | `map<string, vector<string>>` 保留重复键与空值（`http.cpp:449-450`） |
| `canonical_header_key` | 首字母大写、其余小写（`http.cpp:446`） |
| `parse_http_version` | 解析 `HTTP/<major>.<minor>`（`http.cpp:447`） |
| `parse_http_time` | 实际只按 IMF-fixdate `%a, %d %b %Y %H:%M:%S GMT` 解析；**注释宣称的 RFC 850/ctime 未实现**（对比 `http.hpp:266` 与 `http.cpp:448`） |

### 3.5 客户端（`http.hpp:272-308`、`http.hpp:355-361`）

| API | 说明 | 实现 |
|---|---|---|
| `Result` | `Error{None,InvalidUrl,Resolve,Connect,Timeout,Send,Receive,Parse,Closed,Cancelled}`、`message`、`shared_ptr<Response>`、`ok()` | `http.hpp:272-280` |
| `Client(timeout=30s)` | 每次请求自行建连，**不复用连接** | `http.hpp:294-308` |
| `do_request(request, host, port=80)` | 完整生命周期：DNS → connect → 发送 → 接收解析 | `http.cpp:499-575` |
| `get/head/post(url)` | URL 只接受 `http://`；解析失败返回 `InvalidUrl` | `http.cpp:576-582` |
| `Get/Head/Post` 自由函数 | 用默认/指定超时构造临时 `Client` | `http.cpp:584-586` |
| `NewRequest(method, target, body)` | 构造请求的便捷函数 | `http.cpp:587` |

### 3.6 chunked 流式响应（`http.hpp:310-353`）

| API | 说明 | 实现 |
|---|---|---|
| `StreamState` | `Idle` / `HeadersCommitted` / `Closed` / `Failed` | `http.hpp:315-321` |
| `ResponseWriter(socket, timeout)` | 拥有 socket；`timeout` 用于设置 send 超时 | `http.cpp:589-590` |
| `start(Response)` | 删除 `Content-Length`、强制 `Transfer-Encoding: chunked`、清空 body 后写响应头 | `http.cpp:591-608` |
| `write(chunk)` | 写 `<hex>\r\n<chunk>\r\n`；仅 `HeadersCommitted` 允许 | `http.cpp:609-630` |
| `flush()` | **只检查当前是否可写，不写任何字节**（历史命名） | `http.cpp:631-634` |
| `close()` | 写终止块 `0\r\n\r\n`；幂等；`Idle/Failed/Closed` 返回 false | `http.cpp:635-646` |

### 3.7 服务端（`http.hpp:363-400`）

| API | 说明 | 实现 |
|---|---|---|
| `Handler = std::function<Response(const Request&)>` | 路由回调 | `http.hpp:363` |
| `Server(listener)` | 注入已监听的 socket；**不启动 accept 循环** | `http.cpp:648` |
| `route(method, path, handler)` | method + **精确 path** 追加路由 | `http.cpp:649` |
| `set_fallback(handler)` | 未命中路由时的兜底 | `http.cpp:650` |
| `serve_connection(connection, max_requests=100)` | 单连接串行请求循环 | `http.cpp:687-715` |
| `stop()` | 原子停止标志，循环下一轮退出 | `http.hpp:389` |

兼容别名 `HttpRequest/HttpResponse/HttpHeaders/HttpParser/HttpClient/HttpServer/HttpResponseWriter`
在 `http.hpp:404-412`。

---

## 4. 设计

### 4.1 解析器：有状态字节流，不是“一次 recv 一条消息”

`Parser` 内部只有一个只追加的 `std::string m_buffer`（`http.hpp:236-237`），每次 `feed()`
把新字节 append 进去，再从头尝试解析（`http.cpp:316-321`）。TCP 半包/粘包因此天然被覆盖：
测试把一条请求的前 11 字节与其余字节分两次喂入，第一次 `NeedMore`、第二次 `Complete`
（`tests/network/test_network.cpp:141-148`）。

`feed_impl()` 的顺序是硬约束（`http.cpp:310-442`）：

1. 总输入上限预检：头部区之外的字节不得超过 `max_body_bytes`（`http.cpp:312-315`）；
2. 找 `\r\n\r\n`；找不到且超 `max_header_bytes` → `header too large`（`http.cpp:327-331`）；
3. 解析起始行；请求行多一个 token、状态码非 3 位数字都报错（`http.cpp:333-350`）；
4. 逐行解析头部，同时抽取 `Content-Length`/`Transfer-Encoding`（`http.cpp:351-368`）；
5. 冲突检查：CL 重复且不一致、CL+chunked、未知 TE 都直接 Error（`http.cpp:359-369`）；
6. 按四种定界方式收 body（见 4.2），完成后 `erase` 已消费区并置 `m_finished`（`http.cpp:438-441`）。

`reset()` 清空全部状态（`http.cpp:289`）。**收到 Error 后不要复用同一个 Parser**：实现虽然
允许继续 `feed()` 重解析缓冲，但缓冲区内容已被判非法，继续调用没有意义（`http.hpp:187-189`）。

### 4.2 body 定界：四种路径互斥

| 情形 | 行为 | 代码 |
|---|---|---|
| 无 body（HEAD / 1xx / 204 / 304） | 头后立即 Complete；若仍声明 chunked 或非零 CL 则报 `body framing forbidden` | `http.cpp:371-378` |
| 响应且无 CL/TE | 进入 **EOF 定界**：先 `NeedMore`，等 `finish()` 后一次性交付 body，`close=true` | `http.cpp:379-387`、`292-308` |
| `Transfer-Encoding: chunked` | 逐块解析，支持 chunk 扩展与 trailer；遇到 0 块结束 | `http.cpp:393-430` |
| `Content-Length: N` | 凑满 N 字节后交付；N 超过 `max_body_bytes` 直接 Error | `http.cpp:431-437` |

请求**没有** EOF 定界路径：没有 CL/TE 的请求 body 视为 0（`http.cpp:379` 的 `response &&`）。
这是符合 RFC 的（请求不能用连接关闭定界），但也意味着客户端发 body 必须带 CL 或 chunked。

`finish()` 对非 EOF 定界响应返回 `response is not EOF-delimited`（`http.cpp:294-298`）；
对 CL 未读满的响应同样报错（测试 `truncated_parser.finish()` → Error，
`tests/network/test_network.cpp:161-164`）。

### 4.3 头部解析与安全边界

- **只认 CRLF**：`find_header_end()` 只查 `"\r\n\r\n"`（`http.cpp:202`），裸 LF 不会推进，
  最终撞 `max_header_bytes` 报 `header too large`；
- 字段名必须是 RFC token（`valid_header_name`，`http.cpp:31-40`），字段值拒绝除 TAB 外的
  所有 `<0x20` 控制字符（`http.cpp:41-44`、`http.cpp:357-358`）；
- 出站同样校验：`Headers::add()` 用同一套规则，非法值静默丢弃（`http.cpp:239-244`）；
  `Request::to_string()` 还校验 method/target/version，防请求行注入（`http.cpp:255-258`）；
- 重复 `Content-Length` 必须完全一致，否则 `invalid content length`（`http.cpp:359`）；
- `Transfer-Encoding` 只接受**单个** `chunked`（大小写不敏感），`notchunked`、重复 TE、
  其他编码一律 `unsupported transfer encoding`（`http.cpp:107-126`、`360-364`）；
- chunk-size 是十六进制，带溢出检查；`;` 后缀扩展允许，但控制字符拒绝
  （`http.cpp:127-149`）；trailer 按头部规则解析，并受 `max_trailer_bytes` 限制
  （`http.cpp:401-425`）。

### 4.4 keep-alive：服务端循环，客户端不复用

服务端 `serve_connection()` 是 keep-alive 主循环（`http.cpp:687-715`）：

- 最多处理 `max_requests` 条请求（默认 100），防止单连接长期占用 worker（`http.cpp:692`）；
- 每轮 `do { feed/recv } while (NeedMore)` 读出一条完整请求（`http.cpp:695-699`）；
- 同一 `recv` 里可能已有下一条请求，`take_remaining()` 保存为 `pending`，下一轮优先消费
  （`http.cpp:701`、`696`）；
- 路由按 `(method, exact path)` 线性匹配；未命中走 fallback，仍无则 404（`http.cpp:703-708`）；
- handler 抛异常 → 500 + **强制 close**（`http.cpp:706-707`）；
- 写完响应后 `if (request.close || response.close) return;`（`http.cpp:713`）；
- `request.close` 在解析时由 `Connection: close` 或 **HTTP/1.0** 置位（`http.cpp:439-441`）。

客户端相反：`do_request()` 在调用方未显式设置 `Connection` 时补 `Connection: close`
（`http.cpp:520`，显式设置则不覆盖），每个请求新建 socket、用完即关，不跨请求复用（`http.hpp:31`、`http.hpp:288`）。因此本模块的 keep-alive 只在“外部 accept 循环
+ `Server::serve_connection`”这一侧存在；客户端没有连接池。

**空 body 响应的陷阱**：`Response::to_string()` 只在 `!body.empty()` 时自动补
`Content-Length`（`http.cpp:281-282`）。若 handler 返回 200、空 body、`close=false` 且没有
手工设置 `Content-Length: 0`，线上响应没有长度也没有 chunked；客户端按 EOF 定界等待，
服务端却继续等下一个请求——双方互等。**空 body 的 keep-alive 响应必须显式
`header.set("Content-Length", "0")`，或 `response.close = true`，或改用 `ResponseWriter`。**
该组合没有被现有测试覆盖，属于使用方必须遵守的约定。

### 4.5 chunked 与流式响应

`ResponseWriter` 的写侧状态机是严格四态（`http.hpp:328-334`）：

```text
Idle --start--> HeadersCommitted --write--> HeadersCommitted --close--> Closed
                        \-- 任一写失败 --> Failed（终态，不可复用）
```

- `start()` 强制 chunked 并删除用户设置的 `Content-Length`（`http.cpp:593-595`）；
- `write()` 每次直接落到 `SocketStream::write_all`，**没有用户态缓冲**；`flush()` 因此
  只做状态查询（`http.cpp:631-634`）；
- 所有写入经 `write_all` 处理 partial write（`http.cpp:601-605`、`615-628`）；
- `close()` 幂等，未 start / 已 close / Failed 时返回 false（`http.cpp:635-646`）。

**不要对 1xx/204/304 用 `ResponseWriter`**：这些状态在 `to_string()` 里被禁止携带
`Transfer-Encoding`（`http.cpp:272-277`），而 writer 仍会继续写 chunk，线上会出现没有
chunked 标记却带 chunk 的非法响应。

### 4.6 超时：客户端是全局绝对 deadline，服务端没有超时

客户端：

- `Client` 构造时固定 `m_timeout`，默认 30s（`http.hpp:296`）；
- `make_deadline()` 把超时换算成 `steady_clock` 绝对时刻，`max()` 表示无限，溢出饱和到
  `max()`（`http.cpp:60-68`）；
- **每次** DNS/connect/send/recv 前都用 `remaining_time(deadline)` 重算剩余时间
  （`http.cpp:506`、`510`、`526`、`547`），避免“每阶段各拿一份完整超时”；
- 剩余时间向下取整到 0 时返回 1ms，避免 0 被解释成“立即失败”（`http.cpp:73-75`）；
- `ETIMEDOUT` → `Timeout`，`ECANCELED` → `Cancelled`，其余按阶段归到
  `Connect/Send/Receive`（`http.cpp:501-505`）。

**DNS 是同步 `getaddrinfo`**（`http.cpp:508`），在 Fiber 中会占住当前 M；deadline 无法打断
内核解析。这是已知边界，需要可取消 DNS 时应预解析或注入异步 resolver。

服务端 `serve_connection()` 的 `recv()` **不传超时**（`http.cpp:697`），即无限等待；连接级
超时只能由调用方在 handler/accept 层负责（`http.hpp:383-386`）。`ResponseWriter` 则会给
socket 设 send timeout（`http.cpp:597-600`）。

### 4.7 序列化：自动补头与 no-body 过滤

`Response::to_string(head_request)`（`http.cpp:266-286`）：

1. 校验 version；
2. 对 1xx/204/304 过滤用户手写的 `Content-Length`/`Transfer-Encoding`（`http.cpp:269-277`）；
3. `close && !header.has("Connection")` 时补 `Connection: close`（`http.cpp:280`）；
4. 有 body 且无 CL 时补 `Content-Length`（`http.cpp:281-282`）；
5. `head_request` 为真时仍计算 CL，但不输出 body（`http.cpp:284`）。

`Request::to_string()` 同理：`close` 时补 `Connection: close`，有 body 且无 CL 时补
`Content-Length`（`http.cpp:260-262`）。

---

## 5. 边界与错误码

### 5.1 客户端 `Result::Error`（`http.hpp:274-275`）

| 错误 | 触发点 | 代码 |
|---|---|---|
| `InvalidUrl` | 非 `http://`、端口 0/超 65535、裸 IPv6、authority 为空 | `http.cpp:150-201`、`576-581` |
| `Resolve` | `Address::LookupAny` 失败 | `http.cpp:508-509` |
| `Connect` | socket 创建/connect 失败 | `http.cpp:514-519` |
| `Timeout` | deadline 到期，或底层 `ETIMEDOUT` | `http.cpp:510-512`、`526-528`、`547-549`、`501-505` |
| `Send` | 发送返回 0/错误，且不是超时/取消 | `http.cpp:529-533` |
| `Receive` | recv 返回错误 | `http.cpp:550-551` |
| `Parse` | `Parser` 返回 Error，或请求行非法导致 wire 为空 | `http.cpp:522`、`572` |
| `Closed` | 对端关闭且 `finish()` 仍无法完成响应 | `http.cpp:552-556` |
| `Cancelled` | 底层返回 `ECANCELED` | `http.cpp:503` |

### 5.2 解析错误字符串（`ParseResult.error`）

| 字符串 | 条件 | 代码 |
|---|---|---|
| `input too large` | 输入总量超过头/体预算 | `http.cpp:312-315` |
| `header too large` | 头未结束或头块超过 64KiB | `http.cpp:329`、`332` |
| `invalid start line` / `invalid request` | 起始行 token 数/方法/target/version 非法 | `http.cpp:335-348` |
| `invalid response` / `invalid status` | 状态行非法或状态码非 3 位数字 | `http.cpp:341-345` |
| `invalid header` / `invalid header name` / `invalid header value` | 冒号缺失、字段名非 token、值含控制字符 | `http.cpp:355-358` |
| `invalid content length` | 非数字或重复 CL 不一致 | `http.cpp:359` |
| `unsupported transfer encoding` | 非单一 chunked、重复 TE | `http.cpp:360-364`、`107-126` |
| `content length with chunked` | CL 与 TE 同时出现 | `http.cpp:369` |
| `body framing forbidden for response` | 1xx/204/304/HEAD 仍声明 body | `http.cpp:374-378` |
| `body too large` / `body length overflow` | 超过 `max_body_bytes` 或长度加法溢出 | `http.cpp:400`、`432`、`392`、`433` |
| `invalid chunk` / `chunk line too large` / `chunk terminator missing` | chunk 行非法/超行上限/缺 CRLF | `http.cpp:398-399`、`429` |
| `trailers too large` / `invalid trailer` | trailer 块超限或字段非法 | `http.cpp:407`、`415` |
| `response is not EOF-delimited` / `invalid EOF body offset` | `finish()` 用错定界方式 | `http.cpp:297-299` |

### 5.3 其他边界

- `Status` 是枚举：解析时可 `static_cast` 任意数字（`http.cpp:346`），但序列化未知码时
  `status_text()` 返回空串（`http.cpp:230`），状态行会以空格结尾；业务应只使用枚举值。
- `split_url()` 不支持 `https://`（`http.cpp:152-153`）；TLS 不在本模块。
- 客户端不跟随 3xx；`redirect()` 只写 Location（`http.cpp:265`），`do_request()` 原样返回
  3xx（`http.cpp:560-570`）。
- 客户端不处理 `Expect: 100-continue`：它只把 1xx 当 informational 跳过（`http.cpp:560-569`）；
  服务端不发送 100。
- `Parser` 的 body 全量驻留内存，不能用于不受限大文件；大文件/SSE 用 `ResponseWriter`
  （`http.hpp:30`、`http.cpp:631-634`）。
- 出站头没有大小上限（只有字符校验）；`ParserLimits` 只约束入站解析。
- `serve_connection()` 解析失败直接 return，**不发送 400/431**：对端只会看到连接关闭。
- `stop()` 只在轮次边界生效，不会打断正在阻塞的 `recv()`（`http.cpp:692`、`697`）。

---

## 6. 测试

### 6.1 `go2cpp_tests`（包含 `tests/network/test_network.cpp`）

CMake 把 `test_network.cpp` 编进统一测试二进制 `go2cpp_tests`
（`cmake/Go2CppTests.cmake:35-75`、`216`），HTTP/1 断言集中在
`tests/network/test_network.cpp:140-325`（下表未写文件名的行号均属该文件）：

| 用例 | 数据 | 断言 |
|---|---|---|
| 半包请求 | `POST /x?a=1`、`Content-Length: 5`，先喂前 11 字节再喂其余 | 第一次 `NeedMore`，第二次 `Complete`，body=`hello`、path=`/x`（`141-148`） |
| EOF 定界响应 | `HTTP/1.1 200 OK\r\nConnection: close\r\n\r\npartial` | 头后 `NeedMore` 且 body 空；`finish()` 后 body=`partial`、`close=true`（`149-160`） |
| 截断 CL | `Content-Length: 4` 只给 `xy` | `finish()` → Error（`161-164`） |
| chunked | `4\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n` | body=`Wikipedia`（`165-169`） |
| 重复 CL | `Content-Length: 1` + `Content-Length: 2` | Error（`170-173`） |
| chunk 扩展 + trailer | `3;tag=x`、`X-Checksum: ok` | body=`abc`，trailer 可取到（`176-184`） |
| 非法 TE | `notchunked` | Error（`185-189`） |
| 204 no-body | `HTTP/1.1 204` + `Content-Length: 0` | 立即 Complete（`191-194`） |
| no-body 序列化 | 204 + body + 手写 CL:99 | 输出不含 CL，也不含 body（`195-201`） |
| CRLF 注入 | `X-Test: ok\r\nInjected: yes` | 头被丢弃（`202-204`） |
| Connection token | `disclose, keep-alive` / `keep-alive, CLOSE` | 前者不 close，后者 close（`206-214`） |
| 请求行/版本注入 | target/version 带 CRLF | `to_string()` 为空（`215-220`） |
| 非法 target | `GET /bad target HTTP/1.1` | Error（`221-224`） |
| URL 端口 | `http://127.0.0.1:65537/` | `InvalidUrl`（`225-226`） |
| HEAD | `Content-Length: 100` 但无 body | `set_response_to_head(true)` 后立即 Complete（`236-241`） |
| Writer 状态机 | socketpair，1s 超时 | 未 start 的 write 失败；start 后 write/close 成功；重复 write/close 失败（`242-254`） |
| 客户端 EOF | 服务端发 `Connection: close` + `hello` | `ok()` 且 body=`hello`（`264-290`） |
| 全局 deadline | 服务端每 130ms 发 1 字节，客户端超时 230ms | `Timeout`（`293-325`） |

### 6.2 `go2cpp_http2_state_matrix`（Writer 矩阵）

`tests/network/test_http2_state_matrix.cpp:543-633` 对 `ResponseWriter` 的
`4 状态 × 5 事件 = 20 格`逐格断言返回值与迁移后状态：`start` 只在 `Idle` 成功；
`write`/`flush`/`close` 只在 `HeadersCommitted` 成功；`Failed` 是终态。
测试用 socketpair 构造合法流，并用已关闭 fd 构造 `Failed` 夹具（`567-582`）。

### 6.3 场景测试（间接覆盖 `Server`）

`tests/scenarios/deadlock_routes/deadlock_routes_server.cpp` 用
`http::Server` + `Scheduler` 起真实服务端（`1117`），每连接一个 Fiber，并调用
`serve_connection(std::move(connection), 1)`（`1261`）；该用例由 `scenario-deadlock-routes`
执行（`cmake/Go2CppTests.cmake:869`），主线是死锁检测，但会真实走 HTTP/1 请求-响应路径。

### 6.4 构建与运行状态（如实标注）

- 指定构建目录 `/UserData/build/go2cpp-mcp-integrated` 配置为
  `GO2CPP_BUILD_TESTS:BOOL=OFF`（该目录 `CMakeCache.txt` 第 300 行），因此**没有**
  `go2cpp_tests` 可执行文件，本次核对未在该目录运行 HTTP/1 测试。
- `example/network/http_demo.cpp`、`http_full.cpp` 已注册在 `GO2CPP_EXAMPLES`
  （`cmake/Go2CppTests.cmake:1031-1032`），但示例构建段整体位于 `GO2CPP_BUILD_TESTS` 内；
  指定构建目录关闭测试，因此不产出这两个示例。
- 本文的 HTTP/1 结论来自源码与上述测试断言逐条核对；没有单独 HTTP/1 ctest 用例。

---

## 7. 相关文档与维护

- Socket 等待、Hook、FdManager：[io.md](io.md)；`network.md` 的对应章节仍现行。
- WebSocket 握手复用 `Parser` + `take_remaining()`：[../websocket.md](../websocket.md)。
- SSE 复用 `ResponseWriter`：[../sse.md](../sse.md)。
- HTTP/2 独立子模块：[http2.md](http2.md)，**不复用**本 Parser。
- 修改本模块时同步检查：`ParserLimits` 默认值、`Result::Error` 枚举、`ResponseWriter`
  状态机、`Response::to_string()` 的 no-body 过滤；这些都被测试或调用方依赖。
