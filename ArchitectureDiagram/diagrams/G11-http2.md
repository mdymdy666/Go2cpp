# G11 · HTTP/2 全栈（HPACK / 帧层 / 流状态机 / 连接层 / Transport）—— 一图看懂

> **本组与 G7 的 HTTP/1.1 并列在 L5（应用协议层）**，但内部自己又是一套小分层。
> 代码位置：`include/go2cpp/network/http2/` 与 `src/network/http2/`，共 **8 个头 + 6 个实现 + 1 个码表**，**3637 行**。
>
> **状态说明**：HTTP/2 是**已实现并验证**的（双向互通：curl/nghttp2 当客户端、Python 手写服务端当对端）。
> **QUIC / HTTP3 没有实现代码**，只有选型与阅读指南，见 §⑤ 与 `docs/quic-go-reading-guide.md`。

## ① 类架构：每个类用的底层能力 + 主要职责

### 1.1 协议分层与 `Transport` 解耦（本组最重要的设计）

```mermaid
classDiagram
    direction TB

    class Transport {
        <<抽象：只借字节流，不懂协议>>
        +Read(buf, cap) size_t
        +Write(data, len) size_t
        +Closed() bool
    }
    class MemoryTransport {
        <<测试用：双向内存缓冲>>
        +PushInbound(bytes)
        +TakeOutbound() bytes
        +MarkEof()
    }
    class FiberTransport {
        <<生产用：真实 socket>>
        +FiberTransport(socket, timeout)
        ← recv/send 经 hook 挂起 Fiber
    }
    class TlsTransport {
        <<生产用：TLS + ALPN>>
        +CreateClient(socket, sni, verify)
        +Handshake(error) bool
        +NegotiatedProtocol() string
        ← 非阻塞 SSL + poll 等待
    }

    class Connection {
        <<连接层：握手/流表/流控>>
        +Handshake() ConnStatus
        +StartRequest(h, es, out_id) ConnStatus
        +WriteData(id, data, es) ConnStatus
        +SendResponseHeaders(id, h, es) ConnStatus
        +ReadEvent(out) ConnStatus
        +ResetStream(id, code) ConnStatus
        +SendGoAway(code) ConnStatus
    }
    class StreamStateMachine {
        <<流状态机：7 状态>>
        +SendHeaders(es) Result
        +ReceiveData(es) Result
        +SendRstStream() Result
    }
    class Frame {
        <<帧：帧头 + 原始 payload>>
        +DecodeFrame(...) FrameStatus
        +MakeData / MakeHeaders / MakeSettings
    }
    class Encoder {
        <<HPACK 编码>>
        +EncodeBlock(fields, out) HpackStatus
    }
    class Decoder {
        <<HPACK 解码>>
        +DecodeBlock(in, out) HpackStatus
    }
    class Huffman {
        <<静态 Huffman 编解码>>
        +HuffmanEncode / HuffmanDecode
    }

    Transport <|-- MemoryTransport
    Transport <|-- FiberTransport
    Transport <|-- TlsTransport

    Connection --> Transport : 只依赖抽象
    Connection *-- StreamStateMachine : 每流一个
    Connection --> Frame : 收发帧
    Connection --> Encoder : 发 header
    Connection --> Decoder : 收 header
    Encoder ..> Huffman : 值压缩
    Decoder ..> Huffman : 值解压
```

**这张图要说明的一件事**：`Connection` 只认识 `Transport` 的三个方法，**完全不知道字节从哪来**。
所以协议逻辑可以在单线程内存缓冲上测完（`MemoryTransport`），再一行不改地接真实 socket
（`FiberTransport`）和 TLS（`TlsTransport`）。**并发只体现在 Transport 的实现里。**

### 1.2 连接层内部：流表 + 流控窗口

```mermaid
classDiagram
    direction TB

    class Connection {
        -m_role : Role
        -m_handshaked : bool
        -m_next_stream_id : uint32
        -m_peer_max_frame_size : uint32
        -m_read_buffer : vector~uint8~
        -m_decoder : Decoder
        -m_encoder : Encoder
    }
    class StreamTable {
        <<流表：stream id 到状态机>>
        -m_streams : map
        +ResetStreamWindows(id)
    }
    class FlowControl {
        <<RFC 9113 6.9 节的双层窗口>>
        -m_local_window : int64
        -m_remote_window : int64
        -m_stream_local_window : map
        -m_stream_remote_window : map
        +ConsumeSendWindow(id, n) bool
    }
    class IncomingEvent {
        <<ReadEvent 的产出>>
        Kind : kHeaders/kData/kSettings
        Kind : kPing/kGoAway/kRstStream
        Kind : kWindowUpdate/kUnknown
    }
    class ConnStatus {
        <<结果码>>
        kOk / kClosed
        kProtocolError / kUnsupported
        kFlowControl
    }

    Connection *-- StreamTable
    Connection *-- FlowControl
    Connection ..> IncomingEvent : 产出
    Connection ..> ConnStatus : 返回
```

**为什么窗口用有符号 `int64`**：RFC 允许窗口在收到 `WINDOW_UPDATE` 之前**暂时为负**
（对端可能一次发来超过当前额度的 DATA）。用无符号会把它回绕成天文数字。

## ② 重要函数：功能与实现要点

### 2.1 `Connection::ReadEvent` —— 一帧进来后怎么走

```mermaid
flowchart TD
    A["ReadEvent 被调用"] --> B["TakeFrame：从 m_read_buffer 取一个完整帧"]
    B --> C{"帧头够 9 字节？"}
    C -- 否 --> C1["FillBuffer 继续读<br/>对端关闭则返回 kClosed"]
    C -- 是 --> D{"声明长度 > max_frame_size？"}
    D -- 是 --> D1["kFrameSizeError<br/>不等 payload 收全就判"]
    D -- 否 --> E{"stream id 规则满足？"}
    E -- 否 --> E1["kProtocolError<br/>SETTINGS/PING/GOAWAY 必须 0"]
    E -- 是 --> F["按 FrameType 分发"]

    F --> G["HEADERS"]
    F --> H["DATA"]
    F --> I["SETTINGS / PING / RST / GOAWAY / WINDOW_UPDATE"]

    G --> G1{"带 END_HEADERS？"}
    G1 -- 否 --> G2["AssembleHeaderBlock<br/>循环读 CONTINUATION 直到 END_HEADERS"]
    G1 -- 是 --> G3["直接用本帧 block"]
    G2 --> G4["Decoder 解码 → vector~HeaderField~"]
    G3 --> G4
    G4 --> G5["流状态机 ReceiveHeaders"]

    H --> H1["流状态机 ReceiveData"]
    H1 --> H2["扣减本地窗口（连接级 + 流级）"]
    H2 --> H3{"消耗超过一半？"}
    H3 -- 是 --> H4["发 WINDOW_UPDATE 补回"]
    H3 -- 否 --> H5["不补 —— 每帧都补等于不限流"]

    I --> I1["PING 非 ack → 自动回 ack<br/>SETTINGS 非 ack → 自动回 ack"]
```

### 2.2 `WriteData` —— 发送前的两道闸门

```mermaid
flowchart TB
    A["WriteData(id, data, end_stream)"] --> B{"流存在？"}
    B -- 否 --> B1["kProtocolError"]
    B -- 是 --> C{"流状态机允许 SendData？"}
    C -- 否 --> C1["kProtocolError<br/>如 half-closed(local)"]
    C -- 是 --> D{"data + 9 > max_frame_size？"}
    D -- 是 --> D1["kUnsupported<br/>（未做 DATA 分片）"]
    D -- 否 --> E{"连接级 且 流级窗口都够？"}
    E -- 否 --> E1["kFlowControl<br/>调用方等 WINDOW_UPDATE 后重试"]
    E -- 是 --> F["扣两级窗口 → 写 DATA 帧"]
```

**注意点**：窗口不足时**立即返回错误**，不挂起等待。挂起需要 Fiber 条件变量，
是已知的后续工作（见 §⑤）。

## ③ 向上层提供了什么

```mermaid
graph LR
    subgraph HTTP2["HTTP/2 全栈"]
        C["Connection"]
        T["Transport"]
    end

    subgraph 使用方["谁在用"]
        S["example/network/http2_h2c_server.cpp<br/>明文 h2c 服务端"]
        CL["example/network/http2_h2c_client.cpp<br/>h2c 客户端"]
        TST["tests/network/test_*.cpp<br/>MemoryTransport / socketpair"]
    end

    subgraph 对端["互通验证过的对端"]
        CURL["curl 8.5.0<br/>内置 nghttp2 1.59.0"]
        PY["scripts/minimal_h2c_server.py<br/>socket + struct 手拼帧"]
        SS["openssl s_server -alpn h2<br/>仅验 TLS 层"]
    end

    S --> C
    CL --> C
    TST --> C
    C --> T

    CURL -. 客户端 → 我的服务端 .-> S
    CL -. 我的客户端 → 独立服务端 .-> PY
    CL -. TLS + ALPN=h2 .-> SS
```

**向上层提供了什么**（一句话）：一个**同步阻塞写法**的 HTTP/2 连接对象 ——
`ReadEvent()` 阻塞到下一件事发生，`WriteData()` 阻塞到写完，内部由 Fiber 挂起。

## ④ 状态流转

### 4.1 流的 7 个状态（RFC 9113 §5.1）

```mermaid
stateDiagram-v2
    [*] --> idle
    idle --> open : 发/收 HEADERS
    idle --> reservedLocal : 发 PUSH_PROMISE
    idle --> reservedRemote : 收 PUSH_PROMISE
    reservedLocal --> halfClosedRemote : 发 HEADERS
    reservedRemote --> halfClosedLocal : 收 HEADERS
    open --> halfClosedLocal : 发 END_STREAM
    open --> halfClosedRemote : 收 END_STREAM
    halfClosedLocal --> closed : 收 END_STREAM
    halfClosedRemote --> closed : 发 END_STREAM
    open --> closed : 双向 END_STREAM
    halfClosedLocal --> closed : 发/收 RST_STREAM
    halfClosedRemote --> closed : 发/收 RST_STREAM
    reservedLocal --> closed : RST_STREAM
    reservedRemote --> closed : RST_STREAM
    closed --> [*]
```

**两个容易搞反的方向**（本实现在 `test_stream_state.cpp` 用 84 组合穷举矩阵覆盖）：

- `half-closed(local)` —— **本地**关了，所以**不能发** DATA/HEADERS，但**还能收**；
- `half-closed(remote)` —— **对端**关了，所以**不能收**，但**还能发**。

**`idle` 上的 RST_STREAM 是协议错误**（RFC 规定 idle 只接受 HEADERS/PRIORITY），
不能在 idle 上直接关流。

### 4.2 一次请求穿过四层（含 CONTINUATION 与流控）

```mermaid
sequenceDiagram
    participant App as 应用
    participant Conn as Connection
    participant HP as Encoder
    participant St as StreamStateMachine
    participant Tr as Transport

    App->>Conn: StartRequest(headers, end_stream)
    Conn->>HP: EncodeBlock(headers)
    HP-->>Conn: header block（含 Huffman）
    Conn->>St: SendHeaders(end_stream)
    St-->>Conn: kOk（idle → open 或 half-closed(local)）
    Conn->>Conn: ResetStreamWindows(stream_id)

    alt block 能装进一帧
        Conn->>Tr: HEADERS(END_HEADERS)
    else block 超过 max_frame_size
        Conn->>Tr: HEADERS(无 END_HEADERS)
        loop 直到写完
            Conn->>Tr: CONTINUATION(+END_HEADERS)
        end
    end

    Note over Conn,Tr: 只有最后一片带 END_HEADERS<br/>end_stream 只加在 HEADERS 上

    App->>Conn: WriteData(id, data, true)
    Conn->>Conn: ConsumeSendWindow(id, size)
    alt 窗口不足
        Conn-->>App: kFlowControl
    else 窗口够
        Conn->>Tr: DATA(END_STREAM)
    end
```

### 4.3 流控窗口的三个区（发送侧视角）

```mermaid
stateDiagram-v2
    state "充足（> 一半）" as Ok
    state "紧张（≤ 一半，> 0）" as Tight
    state "耗尽（≤ 0）" as Empty

    [*] --> Ok : 握手后窗口 = initial_window_size
    Ok --> Tight : 连续发 DATA
    Tight --> Ok : 收到 WINDOW_UPDATE
    Tight --> Empty : 继续发 DATA
    Empty --> Tight : 收到 WINDOW_UPDATE
    Empty --> Empty : WriteData 返回 kFlowControl
```

**接收侧是对称的**：收到 DATA 扣减本地窗口，**消耗超过一半**才发 `WINDOW_UPDATE` 补回。
每帧都补会让流控形同虚设，还会白花带宽（实测测试里 64+16 字节只触发 2 个 `WINDOW_UPDATE`，
流级与连接级各一）。

### 4.4 HPACK 动态表：插入 → 超限 → FIFO 逐出

```mermaid
flowchart TD
    A["Add(name, value)"] --> B["条目开销 = name.size + value.size + 32"]
    B --> C{"单条 > 表上限？"}
    C -- 是 --> C1["清空整表，不插入<br/>（RFC 7541 §4.4）"]
    C -- 否 --> D{"总大小 + 新条目 > 上限？"}
    D -- 否 --> E["插到表首<br/>相对索引 1"]
    D -- 是 --> F["从表尾逐出最旧条目"]
    F --> D
    E --> G["HPACK 索引 62 指的是表首"]
```

内存在这里的取舍很实在：动态表**省的是重复 header 的字节数**，代价是要维护 FIFO
与索引语义；静态表 61 项是**编译期常量**，零成本，`FindStaticIndex` 一次线性扫描
就够（61 项、且高频命中的都在前面）。

## ⑤ 已实现 vs 只设计（必须分清）

| 能力 | 状态 | 证据 |
|---|---|---|
HPACK（静态表 61 + 动态表 + 整数前缀 + 三种字面量）| **已实现** | RFC 7541 §C.1/C.2/C.3 官方示例逐字节比对 |
Huffman 编解码（257 项码表）| **已实现** | `www.example.com` 编码逐字节匹配 §C.4.1 |
帧层（DATA/HEADERS/PRIORITY/RST_STREAM/SETTINGS/PUSH_PROMISE/PING/GOAWAY/WINDOW_UPDATE/CONTINUATION）| **已实现** | 10 场景 + 随机 3000 轮 |
流状态机（7 状态 + 84 组合矩阵）| **已实现** | 穷举矩阵 + 随机 5000 轮 |
连接层（握手 / 流表 / CONTINUATION / 流控）| **已实现** | 10 场景闭环 |
`MemoryTransport` / `FiberTransport` | **已实现** | socketpair 真实 IO 闭环 |
`TlsTransport`（TLS 1.2+ / SNI / ALPN=h2）| **已实现** | openssl s_server 实测 ALPN='h2' |
h2c 服务端 / 客户端示例 | **已实现** | 与 curl、Python 手写服务端双向互通 |

| 未做 | 原因 |
|---|---|
**QUIC / HTTP3** | **零实现代码**。只有选型与阅读指南（`docs/quic-go-reading-guide.md`）。quic-go 是 Go 库，C++ 无法直接链接 |
**HTTP/2 over TLS 完整链路** | 只验证到 TLS 层 + ALPN 协商；缺真正的 h2 TLS 服务器（nginx/nghttpd） |
**服务端 TLS** | 只做了客户端 |
**流控的阻塞等待** | 窗口不足立即返回 `kFlowControl`，不挂起等 `WINDOW_UPDATE` |
**DATA 分片** | 单帧超过 `max_frame_size` 返回 `kUnsupported`（HEADERS 有 CONTINUATION，DATA 没有对应机制） |
**优先级** | PRIORITY 能解析但不参与调度（RFC 9113 已弃用） |
**服务器推送 API** | PUSH_PROMISE 能解析，无推送接口 |
**会话复用 / 0-RTT** | 未做 |
**证书校验默认值** | `verify_peer=false` 是测试便利，**不是安全默认**，生产必须打开 |

## 与 Go 的差异

| 维度 | Go 的 `x/net/http2` | 本项目 |
|---|---|---|
IO 模型 | `net.Conn` 接口 + goroutine 阻塞 | `Transport` 接口 + Fiber 挂起。**抽象点相同**，实现不同 |
连接驱动 | 多个 goroutine（读/写/调度各一） | **单驱动者**：一个 Fiber 跑 `ReadEvent` 循环，写是同步的 |
流控等待 | 窗口不足时 `Write` 阻塞 | 立即返回 `kFlowControl`，由调用方重试 |
CONTINUATION | 由 header writer 自动处理 | `WriteHeaderBlock` 自动分片；接收侧 `AssembleHeaderBlock` 拼接 |
HPACK Huffman | 支持 | 支持（码表从 Go 的实现提取并校验） |
DATA 分片 | 自动按 `max_frame_size` 切 | **未做** |
优先级 | 已弃用（RFC 9113）| 能解析不调度 |
服务器推送 | 有 `Push` API | 只能解析 PUSH_PROMISE |

## 怎么读这一组图

1. **先看 1.1** —— 理解 `Transport` 为什么存在，这是整个 HTTP/2 模块能脱离网络测试的原因；
2. **再看 4.1** —— 流的 7 个状态，`half-closed` 两个方向最容易搞反；
3. **然后 2.1** —— 一帧进来后的完整分发路径，包含 CONTINUATION 与流控扣减；
4. **最后 ⑤** —— 哪些是真的、哪些只是设计，别把 QUIC 当成已实现。

## 维护约定（与 G1~G10 相同，另有两条本组特有）

1. 本组**一图至少包含**：协议分层、`ReadEvent` 分发、向上层提供、流状态机；
2. **`Transport` 是抽象边界**，画任何新 IO 实现都要画在 `Transport` 之下，不要画进 `Connection`；
3. **已实现 vs 只设计**这一节必须随代码更新 —— 这个项目最容易出现的问题是把"设计过"当成"实现过"；
4. 新增帧类型时，同时更新 2.1 的分发图与 ⑤ 的帧层清单。
