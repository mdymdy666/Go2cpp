# G7 · 应用协议（HTTP / WebSocket / SSE）—— 一图看懂

> 覆盖：`http`（`Headers`/`Request`/`Response`/`Parser`/`Client`/`ResponseWriter`/`Server`）、
> `websocket`（`WebSocket`/`WebSocketMessage`/`WebSocketOpcode`/`WebSocketConfig`/`WebSocketRole`/
> `WebSocketError`/`WebSocketResult`）、`sse`（`Event`/`Parser`/`Stream`/`StreamState`/`StreamOptions`/
> `ParserLimits`/`Result`/`Error`）。
> 源码：`include/go2cpp/network/http.hpp`、`websocket.hpp`、`sse.hpp`。

## ① 类架构：每个类用的底层能力 + 主要职责

```mermaid
classDiagram
    direction TB

    class Socket {
        <<G6 网络基础>>
    }
    class SocketStream {
        <<G6 带缓冲读写>>
    }
    class Framer {
        <<G6 分帧>>
    }

    class Headers {
        <<大小写不敏感的头部表>>
        +Get / Set / Del
        +Contains
    }
    class Request {
        <<请求>>
        +Method / Path / Version
        +Headers
        +Body
    }
    class Response {
        <<响应>>
        +StatusCode
        +Headers
        +Body
    }
    class Parser {
        <<HTTP 报文解析>>
        +Feed(bytes)
        +NextRequest(out) bool
        +SetLimits(...)
        ← 头大小/体大小/行长度都有限制
    }
    class ResponseWriter {
        <<服务端写响应>>
        +WriteHeader(status)
        +Write(bytes)
        +Flush()
        ← 提供流式输出（SSE 依赖它）
    }
    class Server {
        <<HTTP 服务端>>
        +Bind / Serve
        +SetHandler(fn)
        +Shutdown
    }
    class Client {
        <<HTTP 客户端>>
        +Do(request) Response
        +Get / Post
    }

    class WebSocketOpcode {
        <<帧类型>>
        Text / Binary / Close / Ping / Pong
    }
    class WebSocketMessage {
        <<一条消息>>
        +opcode
        +payload
    }
    class WebSocketConfig {
        <<握手与限制>>
        +max_message_size
        +handshake_timeout
    }
    class WebSocket {
        <<连接>>
        +Handshake() 
        +Send(message)
        +Recv(out) 
        +Close(code, reason)
    }

    class SseEvent {
        <<SSE 事件>>
        +id / event / data
    }
    class SseParser {
        <<文本协议解析>>
        +Feed(text)
        +Next(out) bool
    }
    class StreamState {
        <<流状态>>
        Idle / Open / Closed / Failed
    }
    class SseStream {
        <<SSE 流>>
        +Send(event)
        +State()
    }

    Request *-- Headers
    Response *-- Headers
    Parser ..> Request : 产出
    Server ..> Parser : 每连接一个
    Server ..> ResponseWriter
    Client ..> Parser : 解析响应
    Client ..> Request
    Server ..> SocketStream : 用 G6
    Client ..> SocketStream
    WebSocket ..> SocketStream : 握手走 HTTP，之后走帧
    WebSocket ..> WebSocketConfig
    WebSocket ..> WebSocketMessage
    WebSocketMessage ..> WebSocketOpcode
    SseStream ..> ResponseWriter : 服务端流式推送
    SseStream ..> SseEvent
    SseParser ..> SseEvent
    SseStream ..> StreamState
```

| 类 | 主要职责 | 用的底层能力 |
|---|---|---|
| `Headers` | HTTP 头表（**大小写不敏感**） | 哈希表 + 规范化 |
| `Parser` | 把字节流解析成 `Request`/`Response`；**带各处大小限制** | `ByteArray`、`Framer` 式增量解析 |
| `Server`/`Client` | 监听/发起；把连接包装成可复用的读写 | **G6 的 `Socket`/`SocketStream`**、hook |
| `ResponseWriter` | 写状态行/头/体，支持 `Flush` 做**流式输出** | `SocketStream` |
| `WebSocket` | 握手（HTTP Upgrade）+ 帧编解码 + ping/pong/close | `SocketStream`、`WebSocketConfig` 限制 |
| `SseParser`/`SseStream` | 解析/生成 `text/event-stream` 文本协议 | `ResponseWriter`（服务端）、`SocketStream`（客户端） |

## ② 重要函数：功能与实现要点

```mermaid
flowchart TB
    subgraph SRV["Server::Serve 的一次请求"]
        S1["accept 一个连接（G6）"] --> S2["循环 Feed 字节到 Parser"]
        S2 --> S3{"解析出一个完整请求?"}
        S3 -- 否 --> S4["继续读（可能 park）"]
        S4 --> S2
        S3 -- 是 --> S5["调用用户 handler(request, writer)"]
        S5 --> S6{"handler 是否调用了 WriteHeader?"}
        S6 -- 否 --> S7["默认 200"]
        S6 -- 是 --> S8["用指定状态码"]
        S7 --> S9["写响应并 Flush"]
        S8 --> S9
        S9 --> S10{"keep-alive?"}
        S10 -- 是 --> S2
        S10 -- 否 --> S11["关闭连接"]
    end

    subgraph WS["WebSocket 握手与收发"]
        W1["收到带 Upgrade: websocket 的 HTTP 请求"] --> W2["校验 Sec-WebSocket-Key 与版本"]
        W2 --> W3{"校验通过?"}
        W3 -- 否 --> W4["返回 400 并关闭"]
        W3 -- 是 --> W5["计算 Sec-WebSocket-Accept，返回 101"]
        W5 --> W6["之后按帧读写：mask/opcode/len/payload"]
        W6 --> W7{"opcode?"}
        W7 -- Ping --> W8["自动回 Pong"]
        W7 -- Close --> W9["回 Close 并进入关闭"]
        W7 -- Text/Binary --> W10["拼成 WebSocketMessage 交给上层"]
        W10 --> W11{"超过 max_message_size?"}
        W11 -- 是 --> W12["报错并关闭（防内存耗尽）"]
    end

    subgraph SSE["SSE 服务端推送"]
        E1["handler 调用 WriteHeader(200)<br/>并设 Content-Type: text/event-stream"] --> E2["每次 Send(event) 写一段：<br/>id:/event:/data:/空行"]
        E2 --> E3["Flush（关键：不 Flush 客户端收不到）"]
        E3 --> E4{"客户端断开?"}
        E4 -- 是 --> E5["Stream 状态 -> Closed"]
        E4 -- 否 --> E2
    end

    SRV --> WS
    SRV --> SSE
```

**要点**

- `Parser` **必须有各处上限**（头大小、体大小、行长）：否则一个恶意超长头就能耗尽内存 ——
  这与 G6 里 `Framer::max_frame_size` 是同一类防护；
- HTTP 的 `Headers` 查找**大小写不敏感**（协议要求）；
- `WebSocket` 的 `Ping` **自动回 `Pong`**，用户不用处理；
- `WebSocketConfig::max_message_size` 同样防内存耗尽（帧可以声明很大的长度）；
- **SSE 必须 `Flush`**：不 flush 的话数据留在用户态缓冲里，客户端根本收不到 ——
  这是 SSE 最容易踩的坑，图上专门标出。

## ③ 向上层提供了什么

```mermaid
graph LR
    subgraph G7API["协议能力"]
        A1["HTTP：Server/Client/Parser/ResponseWriter/Headers"]
        A2["WebSocket：握手 + 帧收发 + ping/pong + close"]
        A3["SSE：服务端流式推送 + 客户端解析"]
    end
    subgraph USE["谁在用"]
        U1["业务代码：实现 HTTP 服务或调用外部 API"]
        U2["实时推送场景：WebSocket / SSE"]
        U3["调试模块：不直接用；<br/>但请求处理中的阻塞会体现为 G 的 kWaiting"]
    end
    A1 --> U1
    A2 --> U2
    A3 --> U2
```

## ④ 状态流转

### 一次 HTTP 连接

```mermaid
stateDiagram-v2
    [*] --> kAccepted : accept
    kAccepted --> kReadingHeader : 读字节 -> Parser
    kReadingHeader --> kReadingBody : 头完整
    kReadingBody --> kDispatched : 体完整 -> 交给 handler
    kDispatched --> kWriting : handler 写响应
    kWriting --> kReadingHeader : keep-alive 继续
    kWriting --> kClosing : Connection: close
    kReadingHeader --> kError : 报文非法（超限/格式错）
    kReadingBody --> kError : 体超限/超时
    kDispatched --> kError : handler 抛异常（转 500）
    kClosing --> [*]
    kError --> [*]
```

### WebSocket 连接

```mermaid
stateDiagram-v2
    [*] --> kHttpHandshake : 收到 Upgrade 请求
    kHttpHandshake --> kOpen : 101 Switching Protocols
    kHttpHandshake --> kRejected : 校验失败（400）
    kOpen --> kOpen : Text/Binary 收发
    kOpen --> kClosing : 收到 Close 或本地 Close()
    kClosing --> kClosed : 双方 Close 完成
    kOpen --> kFailed : 帧非法 / 超 max_message_size
    kClosed --> [*]
    kFailed --> [*]
    kRejected --> [*]
```

### SSE 流

```mermaid
stateDiagram-v2
    [*] --> kIdle : 构造
    kIdle --> kOpen : 设好头并开始推送
    kOpen --> kOpen : Send(event) + Flush
    kOpen --> kClosed : 客户端断开 / 主动结束
    kOpen --> kFailed : 写失败（连接错误）
    kClosed --> [*]
    kFailed --> [*]
```

## 与 Go 的差异

| 项 | Go | Go2Cpp |
|---|---|---|
| HTTP 服务端 | `net/http`（`http.Handler` 接口 + `ServeMux`） | `http::Server` + 函数式 handler + `ResponseWriter` |
| 头部大小写 | `textproto.CanonicalMIMEHeaderKey` | `Headers` 内部规范化，查找不敏感 |
| WebSocket | 第三方库（`gorilla/websocket` 等） | **内置** `WebSocket`（含握手、ping/pong、close） |
| SSE | 手写 `text/event-stream` | **内置** `sse::Stream` + `Parser` |
| 限制防护 | 由库或用户设置 | `ParserLimits`、`WebSocketConfig::max_message_size` **默认存在** |
