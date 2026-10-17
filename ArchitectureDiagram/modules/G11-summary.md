# G11 · HTTP/2 全栈：`hpack` / `huffman` / `frame` / `stream_state` / `connection` / `transport`

> 依赖：只依赖 **G6** 的 `Socket`（通过 `FiberTransport`）与 **G0** 的 Fiber 调度（通过 hook 挂起）。
> 协议逻辑本身**不依赖任何 IO** —— 这是本模块能被完整测试的原因。
>
> 规模（实测 3637 行）：
> `http2/hpack.hpp` 234 + `src/network/http2/hpack.cpp` 575；
> `huffman.hpp` 63 + `huffman.cpp` 162 + `huffman_tables.inc` 58；
> `frame.hpp` 275 + `frame.cpp` 538；
> `stream_state.hpp` 132 + `stream_state.cpp` 192；
> `connection.hpp` 232 + `connection.cpp` 610；
> `transport.hpp` 128；`fiber_transport.hpp` 101；
> `tls_transport.hpp` 103 + `tls_transport.cpp` 234。
>
> **边界**：**QUIC / HTTP3 不在范围内，且没有任何实现代码**。只有选型与阅读指南
> `docs/quic-go-reading-guide.md`（quic-go 是 Go 库，C++ 无法直接链接，本项目未引入任何 Go 依赖）。

---

## 0. 一句话定位

`http2` 提供「HPACK 头部压缩 + 二进制帧 + 流状态机 + 连接管理」的完整 HTTP/2 实现，
通过 `Transport` 接口与 IO 解耦，因此同一套协议逻辑可以跑在内存缓冲（测试）、
真实 socket（生产）和 TLS（生产）三种通道上。

---

## 1. `Transport` 抽象（★ 本模块最重要的设计）

### 1.1 为什么有它

连接层里有 90% 是协议逻辑（握手、帧循环、流表、状态推进），只有最后一步
「从哪读字节、往哪写字节」与环境相关。把这一步抽成三个纯虚方法：

```cpp
virtual std::size_t Read(std::uint8_t* buffer, std::size_t capacity) = 0;
virtual std::size_t Write(const std::uint8_t* data, std::size_t length) = 0;
virtual bool Closed() const noexcept;
```

`Connection` **只认识这三个方法**，完全不知道字节从哪来。

### 1.2 三个实现

| 实现 | 用途 | 关键点 |
|---|---|---|
`MemoryTransport` | 测试 / 协议回放 | 双向内存缓冲；`PushInbound` 模拟对端发来的字节，`TakeOutbound` 取走本端写出的字节 |
`FiberTransport` | 生产（明文 h2c） | `Read/Write` 映射到 `Socket::recv/send`；hook 让 Fiber 挂起 |
`TlsTransport` | 生产（TLS） | OpenSSL，非阻塞 SSL + `poll` 等待，ALPN 协商 `h2` |

### 1.3 这带来了什么

- **协议逻辑可以在单线程内存缓冲上完整验证**，不需要 socket、不需要 Fiber。
  本项目的 HPACK、帧层、流状态机、连接层全部用 `MemoryTransport` 测；
- **接真实网络时连接层代码一行不改** —— 换成 `FiberTransport` 即可；
- **HTTP/2 的全部测试都是确定性的**，没有 flaky 网络等待。

**这是"抄协议、改并发"能成立的前提**：并发的差异被压缩到 `Transport` 的实现里。

---

## 2. HPACK（RFC 7541）

### 2.1 静态表与动态表

- **静态表 61 项**（附录 A）是编译期常量，零成本。`FindStaticIndex` 一次线性扫描就够
  —— 61 项、高频命中的都在前面（`:method`/`:path`/`:scheme`/`:status`）；
- **动态表**是 FIFO，条目开销 = `name.size() + value.size() + 32`（§4.1）；
  插入导致超限时从**表尾**逐出最旧条目；
- **条目本身大于表上限时清空整表且不插入**（§4.4）—— 这条容易漏，结果是索引错位。

HPACK 索引语义：`1..61` 是静态表，`62..` 是动态表，**62 指最新插入的**（表首）。

### 2.2 整数前缀编码（§5.1）

值小于 `2^N - 1` 时一字节；否则首字节低 N 位全 1，后续每字节 7 位、最高位作续位标志。

**编码时要保留首字节高位** —— 那是「增量索引」等表示类型的标记。所以
`EncodeInteger(value, prefix_bits, first_byte_high_bits, ...)` 有三个参数，
不是两个。

### 2.3 Huffman（§5.2）

- 码表 257 项（256 字节 + EOS），从 Go 的 `x/net/http2/hpack/tables.go` **自动提取**
  （`huffman_tables.inc`），避免手工转录 257 项出错；
- 编码末尾用 **EOS 的最高位（全 1）** 填充到字节边界，填充长度 **< 8 位**；
- 解码用**二叉树逐位下降**而非 8 位查表：树的语义与 RFC 的"前缀码"定义一一对应，
  便于对照标准审查；257 个符号的树只有约 512 节点；
- **EOS 不允许出现在输入中**；尾部停留时剩余位必须全 1 且 < 8 位，否则填充非法。

### 2.4 坑点

1. **错误码要区分 `kNeedMore` 与 `kTruncatedString`** —— 增量解析时前者可以等，
   后者必须断开。混用会把"还没收全"当成协议错误；
2. **编码端对短串会退化到原样**：Huffman 对某些字节的码长 > 8，短串反而膨胀。
   本实现只在 `huffman.size() < raw.size()` 时才用 Huffman；
3. 已知未解：**含控制字符的值导出后再读回的形态不一致**（`DumpIni` 转义正确，
   但 `IniFile::Parse` 读回时换行被还原、制表符没有）—— 这是 config 模块的问题，
   与 HPACK 无关，但同属"转义对称性"这一类坑。

---

## 3. 帧层（RFC 9113 §4 / §6）

### 3.1 通用帧与语义帧分离

| 层 | 职责 |
|---|---|
**通用帧** | 9 字节帧头（length 24 位 + type 8 + flags 8 + stream id 31）+ 原始 payload。只负责"把字节流切成帧" |
**语义帧** | 每种帧一个 `ParseXxx` / `MakeXxx`，解释 payload |

新增帧类型只需加一对函数，不改通用路径。

### 3.2 校验都在这一层（不留给上层）

- 帧长 > `SETTINGS_MAX_FRAME_SIZE` → `kFrameSizeError`（**不等 payload 收全就判**）；
- `SETTINGS`/`PING`/`GOAWAY` 必须 stream 0；`DATA`/`HEADERS` 等不能为 0；
- `SETTINGS` 非 ack 长度须为 6 的倍数，ACK 须空 payload；
- `PING` 恰好 8 字节；`RST_STREAM`/`WINDOW_UPDATE` 恰好 4 字节；
- `WINDOW_UPDATE` 增量为 0 是协议错误；
- `PADDED` 的 pad length 越界报错。

### 3.3 增量解析的 `kNeedMore`

帧头或 payload 不足时返回 `kNeedMore` 而**不是报错**。这是流式解析的必需：
TCP 会把一个帧拆成多段到达，若把"半包"当错误，连接会被误断。

但帧长超限与 stream id 违规**可以在拿到帧头后立即判定**，不必等 payload ——
这样对端的明显违约能尽早暴露。

### 3.4 坑点

1. `PADDED` 的处理顺序：先读 pad length 字节，再跳过开头、切掉末尾 padding，
   **HEADERS 还要额外跳 5 字节优先级字段**（当 `PRIORITY` 标志置位时）；
2. `StreamFor` 用 `try_emplace` 而不是 `operator[]` —— `StreamStateMachine` 没有
   默认构造（流 id 必须确定），`operator[]` 编不过。

---

## 4. 流状态机（§5.1）

### 4.1 七个状态

`idle` / `reserved(local)` / `reserved(remote)` / `open` / `half-closed(local)` /
`half-closed(remote)` / `closed`。

### 4.2 最容易搞反的两处

- **`half-closed(local)`** = **本地**关了 → **不能发** DATA/HEADERS，**还能收**；
- **`half-closed(remote)`** = **对端**关了 → **不能收**，**还能发**。

这两个方向搞反不会崩，只会让连接在后续请求上莫名报协议错误 —— 极难定位。
所以本模块用**84 组合穷举矩阵**（7 状态 × 12 事件）覆盖，而不是靠零星用例。

### 4.3 `idle` 上的 `RST_STREAM` 是协议错误

RFC 9113 §5.1 规定 `idle` 只接受 `HEADERS`/`PRIORITY`。所以
「任何状态都能收 RST」是**错的**，`idle` 上必须拒绝。
（本实现最初就写错了这一点，是穷举矩阵抓出来的。）

---

## 5. 连接层

### 5.1 握手

- 客户端：发 preface（`PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n`，24 字节）+ 本地 SETTINGS，
  等对端 SETTINGS，回 ACK；
- 服务端：先读 preface 校验，再发 SETTINGS、收 SETTINGS、回 ACK。

**preface 不足 24 字节且对端关闭时返回 `kClosed` 而非 `kProtocolError`** ——
要区分"对端根本没发"和"发了但不匹配"。

### 5.2 CONTINUATION

header block 超过 `max_frame_size - 9` 时分片：
`HEADERS`（无 `END_HEADERS`）+ 若干 `CONTINUATION`，**只有最后一片带 `END_HEADERS`**，
`end_stream` 只加在 `HEADERS` 上。

接收侧 `AssembleHeaderBlock` 循环读到 `END_HEADERS`，校验每个 CONTINUATION 的
流 id 与 HEADERS 一致；**中间出现其它帧按 §6.10 报协议错误**。

**"同一时刻只允许一条流发 header block"** 这个 RFC 约束在本实现里**天然成立**：
`Connection` 是单驱动者（一个 Fiber），且发送是同步的。

### 5.3 流控（§6.9）

两对窗口（连接级 + 流级），**有符号 64 位**（RFC 允许窗口在收到 `WINDOW_UPDATE`
之前暂时为负，无符号会回绕成天文数字）。

- **发送前**：连接级与流级都必须有额度，否则返回 `kFlowControl`（**不阻塞等待**，
  调用方等 `WINDOW_UPDATE` 后重试）；
- **接收时**：扣减两级本地窗口，**只有消耗超过一半**才发 `WINDOW_UPDATE` 补回。
  每帧都补会让流控形同虚设，还白花带宽。

### 5.4 坑点

1. **服务端被动创建的流也要初始化窗口** —— 只在客户端 `StartRequest` 里初始化的话，
   服务端回响应时 `ConsumeSendWindow` 找不到该流会直接失败；
2. 测试里只补**流级** `WINDOW_UPDATE` 不够，RFC 要求**连接级与流级都有额度**；
3. 发了两个 `WINDOW_UPDATE` 就必须读两个，只读一个的话第二个还在缓冲区，
   连接级窗口不会恢复。

---

## 6. TLS + ALPN

- OpenSSL 客户端，TLS 1.2+、SNI、ALPN 优先 `h2`；
- **底层 socket 设为非阻塞**：`SSL_connect`/`SSL_read`/`SSL_write` 返回
  `WANT_READ`/`WANT_WRITE` 时用 `poll` 等待后重试。本项目的 hook 会拦截 `poll`，
  所以在 Fiber 里等待会**挂起协程**而不是卡住 worker 线程；
- **不能用阻塞 fd** —— 那会让 SSL 内部阻塞整个线程；
- **证书校验默认关闭**（`verify_peer=false`）。这是为自签名证书本地测试的便利，
  **不是安全默认**；生产必须打开并配置 CA。

---

## 7. 与 Go 的差异

| 维度 | Go `x/net/http2` | 本项目 |
|---|---|---|
IO 模型 | `net.Conn` 接口 + goroutine 阻塞 | `Transport` 接口 + Fiber 挂起。**抽象点相同**，实现不同 |
连接驱动 | 多个 goroutine（读/写/调度各一）| **单驱动者**：一个 Fiber 跑 `ReadEvent` 循环 |
流控等待 | 窗口不足时 `Write` 阻塞 | 立即返回 `kFlowControl`，调用方重试 |
DATA 分片 | 自动按 `max_frame_size` 切 | **未做**（HEADERS 有 CONTINUATION，DATA 没有对应机制）|
优先级 | 已弃用（RFC 9113）| 能解析但不参与调度 |
服务器推送 | 有 `Push` API | 只能解析 PUSH_PROMISE |
TLS | `crypto/tls` 内置 | OpenSSL 包装，ALPN 手工设置 |

---

## 8. 验证方式（已实现 vs 只设计）

### 已实现并验证

| 能力 | 验证方式 |
|---|---|
HPACK | **RFC 7541 §C.1/C.2/C.3 官方示例逐字节比对** |
Huffman | **`www.example.com` 编码逐字节匹配 §C.4.1** |
帧层 | 10 场景 + 随机 3000 轮 |
流状态机 | **84 组合穷举矩阵** + 随机 5000 轮 |
连接层 | 10 场景（含 CONTINUATION 50000 字节 header、流控窗口耗尽/恢复）|
`FiberTransport` | socketpair 真实 IO 闭环 |
`TlsTransport` | `openssl s_server -alpn h2` 实测 `TLSv1.3` / `ALPN='h2'` |
**双向互通** | curl 8.5.0（nghttp2 1.59.0）当客户端 → 我的服务端 ✅；我的客户端 → Python 手拼帧服务端 ✅ |

### 只设计 / 未实现

| 项 | 说明 |
|---|---|
**QUIC / HTTP3** | **零实现代码**，只有 `docs/quic-go-reading-guide.md` |
**HTTP/2 over TLS 完整链路** | 只验证到 TLS 层 + ALPN；缺真正的 h2 TLS 服务器 |
**服务端 TLS** | 只做了客户端 |
**流控的阻塞等待** | 窗口不足立即返回错误 |
**DATA 分片** | 单帧超限返回 `kUnsupported` |
**优先级 / 服务器推送 / 0-RTT** | 未做 |

---

## 9. 对架构图的贡献

- 本文件是 `diagrams/G11-http2.md`（9 张图）的**注解来源**；
- 在 `diagrams/G0-overview.md` 的分层图里，HTTP/2 属于 **L5 应用协议层**，
  与 G7 的 HTTP/1.1 并列；
- `Transport` 抽象在 G0 的 L4（网络基础）上体现为「IO 的唯一出口」，是全项目
  **唯一**同时被协议层和测试层直接依赖的接口。
