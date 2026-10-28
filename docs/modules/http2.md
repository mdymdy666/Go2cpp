# 模块文档：http2（HTTP/2 全栈）

> **状态：现行**。基线 `HEAD 2ba2f4e`（2026-10-26 核对；本文所有 `file:line` 均指该工作树）。
> 本文是 `go2cpp::net::http2`（HPACK、Huffman、帧层、流状态机、连接层、Transport 实现）
> 的**唯一现行文档**：概述、API、设计、边界、测试数据都在这里。
> 旧文档 `docs/network.md` 的 `## HTTP/2` 一节已并入本文，该文件顶部已加合并标注。
> 流/连接状态转化表仍以 [../state-machines/Http2StreamState.md](../state-machines/Http2StreamState.md)
> 与 [../state-machines/Http2Connection.md](../state-machines/Http2Connection.md) 为详细附录；
> 本文给设计动机与边界，不重复 52 行转化表。
> 引用约定：`connection.hpp`/`connection.cpp` 等简写分别指
> `include/go2cpp/network/http2/` 与 `src/network/http2/` 下的同名文件；其余写全路径。

---

## 1. 概述与分层

HTTP/2 是**独立子模块**，不复用 `http::Parser`：二进制帧、HPACK 压缩与 HTTP/1.x 文本解析
没有可复用部分（`include/go2cpp/network/http2/connection.hpp:1-59`）。它的核心设计是
**把并发从协议逻辑里拿掉**：连接层只认 `Transport` 的字节流，所有阻塞都收敛到 Transport
实现（`transport.hpp:5-17`）。

| 层 | 头文件 / 实现 | 职责 | 规模 |
|---|---|---|---|
| Huffman | `huffman.hpp` / `huffman.cpp` + `huffman_tables.inc` | RFC 7541 附录 B 的 257 项码表；头值压缩 | 92 / 162 行 |
| HPACK | `hpack.hpp` / `hpack.cpp` | 静态表 61 项、动态表 FIFO、前缀整数、三种字面量、表大小更新 | 324 / 786 行 |
| 帧层 | `frame.hpp` / `frame.cpp` | 10 种帧的解析/构造；帧长、stream id、payload 形状校验 | 375 / 651 行 |
| 流状态机 | `stream_state.hpp` / `stream_state.cpp` | RFC 9113 §5.1 的 7 状态；纯逻辑、无 IO、无锁 | 220 / 267 行 |
| 连接层 | `connection.hpp` / `connection.cpp` | 握手、流表、CONTINUATION、SETTINGS、GOAWAY、流控 | 502 / 1444 行 |
| IO 抽象 | `transport.hpp` | `Read` / `Write` / `Closed`，协议逻辑唯一的缝 | 215 行 |
| Fiber IO | `fiber_transport.hpp` | `Socket::recv/send` + Hook/IOManager（协程挂起） | 151 行 |
| TLS IO | `tls_transport.hpp` / `tls_transport.cpp` | OpenSSL、非阻塞 fd + poll、SNI、ALPN=h2 | 233 / 505 行 |

> 旧文档 `network.md` 写“共 3637 行”，与当前树的口径不同（当时未计 `tls_transport.cpp` 等）。
> 以上按 `wc -l` 实测；只统计协议栈本体，不含测试。

**范围**：客户端/服务端 h2c（明文）与客户端 h2 over TLS；单连接同步阻塞 API。
**明确不做**：QUIC/HTTP3、服务器推送、优先级调度、TLS 服务端、会话复用/0-RTT
（`connection.hpp:27-41`、`tls_transport.hpp:20-49`）。

---

## 2. API 表

### 2.1 连接层（`connection.hpp:73-500`）

| API / 类型 | 说明 | 位置 |
|---|---|---|
| `Role{kClient,kServer}` | 角色决定 preface、流号奇偶、GOAWAY 的 last_stream_id | `connection.hpp:75-76` |
| `ConnectionConfig` | 帧上限、header 双上限、HPACK 表、初始窗口、并发流、retired 流、scheme 兼容开关 | `connection.hpp:79-162` |
| `ConnStatus` | `kOk/kClosed/kProtocolError/kUnsupported/kFlowControl/kHeaderListTooLarge/kTooManyStreams` | `connection.hpp:165-179` |
| `ConnState` | `kOpen / kDraining / kClosed` | `connection.hpp:188-192` |
| `IncomingEvent` | `Kind{kHeaders,kData,kSettings,kPing,kGoAway,kRstStream,kWindowUpdate,kUnknown}` + 字段 | `connection.hpp:195-219` |
| `Connection(Transport*, Role, config)` | 单向依赖注入；**非线程安全**，单驱动者 | `connection.hpp:244-248` |
| `Handshake(error)` | preface + 本地 SETTINGS + 等对端 SETTINGS + ACK | `connection.hpp:274`、`connection.cpp:345-469` |
| `StartRequest(headers, end_stream, &sid, error)` | 仅客户端；分配奇数流号并写 HEADERS | `connection.hpp:295-296`、`connection.cpp:480-554` |
| `WriteData(sid, data, end_stream, error)` | 预检 → 提交；窗口不足返回 `kFlowControl` | `connection.hpp:308-309`、`connection.cpp:568-628` |
| `SendResponseHeaders(sid, headers, end_stream, error)` | 仅服务端方向使用；发 HEADERS | `connection.hpp:312-314`、`connection.cpp:630-668` |
| `ResetStream(sid, code, error)` | 发送 RST_STREAM；idle 流拒绝 | `connection.hpp:317-318`、`connection.cpp:670-692` |
| `SendGoAway(code, error)` | 写 GOAWAY 成功后迁移 `kDraining` | `connection.hpp:329`、`connection.cpp:1044-1065` |
| `ReadEvent(out, error)` | 接收方向唯一分发点；阻塞到下一个事件 | `connection.hpp:347`、`connection.cpp:1080-1442` |
| `Streams()` / `state()` / `peer_last_stream_id()` | 只读诊断 | `connection.hpp:250-258`、`350-352` |

`ConnectionConfig` 的关键默认值（`connection.hpp:80-161`）：`max_frame_size=16384`、
`max_header_block_size=65536`、`max_header_list_size=65536`、`header_table_size=4096`、
`initial_window_size=65535`、`max_concurrent_streams=100`、`max_retired_streams=256`、
`require_scheme=true`、`strict_scheme=false`、`auto_fill_scheme=true`、
`enforce_peer_header_list_on_requests=false`、`enforce_max_concurrent_streams=true`。

### 2.2 HPACK（`hpack.hpp:55-322`）

| API / 类型 | 说明 |
|---|---|
| `HeaderField{name,value}` | 名字必须小写（校验在 `connection.cpp:107-113`，不在 HPACK 层） |
| `HpackStatus` | `kOk/kNeedMore/kIndexOutOfRange/kInvalidInteger/kTruncatedString/kHuffmanUnsupported/kHuffmanInvalid/kInvalidTableSize`（`hpack.hpp:74-91`） |
| `DecodeInteger/EncodeInteger` | RFC 7541 §5.1 前缀整数；前缀位宽 1~8（`hpack.hpp:96-123`） |
| `StaticTableEntry/FindStaticIndex/FindStaticNameIndex` | 61 项静态表，**1-based**（`hpack.hpp:125-143`） |
| `DynamicTable` | FIFO、`name+value+32` 计费、O(1) `FindRelative`、逐出（`hpack.hpp:160-213`） |
| `Decoder::DecodeBlock` | 四种表示 + 表大小更新；块首最多两条更新（`hpack.hpp:229-256`、`hpack.cpp:517-654`） |
| `Encoder::EncodeBlock` | 静态命中 → 动态命中 → 名字静态 + 值字面量；仅 Huffman 更短才用（`hpack.hpp:272-322`、`hpack.cpp:707-784`） |
| `Encoder::SetTableSize/AppendTableSizeUpdate` | 收缩立即生效；扩容必须显式发 `001xxxxx`（`hpack.hpp:286-306`） |

### 2.3 帧层（`frame.hpp:57-373`）

| API / 类型 | 说明 |
|---|---|
| `FrameType` | 10 种：DATA/HEADERS/PRIORITY/RST_STREAM/SETTINGS/PUSH_PROMISE/PING/GOAWAY/WINDOW_UPDATE/CONTINUATION |
| `FrameFlags` | `kEndStream/kAck/kEndHeaders/kPadded/kPriority` |
| `ErrorCode` | RFC 9113 §7 的 14 个错误码 |
| `FrameStatus` | `kOk/kNeedMore/kProtocolError/kFrameSizeError/kUnknownType` |
| `FrameHeader` / `Frame` | 9 字节头 + 原始 payload；`payload.size()==header.length` |
| `DecodeFrameHeader/EncodeFrameHeader` | 24 位长度、31 位 stream id |
| `DecodeFrame` | 增量解析；先长度/stream id，再等 payload（`frame.cpp:191-222`） |
| `ParseSettings/MakeSettings/MakeSettingsAck` | SETTINGS 语义视图 |
| `ParsePing/MakePing` | 8 字节不透明数据 |
| `ParseData/MakeData` | 剥 padding |
| `ParseHeaders/MakeHeaders`、`ParseContinuation/MakeContinuation` | HEADERS 剥 padding/priority；CONTINUATION 无 END_STREAM |
| `ParseRstStream/ParseGoAway/ParseWindowUpdate/ParsePriority` | 各帧形状校验 |

### 2.4 流状态机（`stream_state.hpp:74-218`）

| API | 说明 | 实现 |
|---|---|---|
| `StreamState` | `kIdle/kReservedLocal/kReservedRemote/kOpen/kHalfClosedLocal/kHalfClosedRemote/kClosed` | `stream_state.hpp:74-82` |
| `StreamEventResult` | `kOk/kInvalidTransition/kStreamClosed` | `stream_state.hpp:87-93` |
| `SendHeaders(bool end_stream)` | idle/reserved(local)/open/half-closed(remote) 分支 | `stream_state.cpp:71-114` |
| `CanSendData()` | 只读预检：`open` 或 `half-closed(remote)` | `stream_state.cpp:116-119` |
| `SendData(bool)` | 预检 → 置 END_STREAM → 重算 | `stream_state.cpp:142-155` |
| `SendPushPromise/ReceivePushPromise` | idle → reserved(local/remote) | `stream_state.cpp:157-166`、`244-253` |
| `ReceiveHeaders/ReceiveData` | 对端方向；非法转移不修改状态 | `stream_state.cpp:191-242` |
| `SendRstStream/ReceiveRstStream` | 非 idle 的任意状态 → closed | `stream_state.cpp:168-179`、`255-265` |
| `ForceClose()` | 本地强制 closed，不发 RST_STREAM | `stream_state.cpp:128-132` |

### 2.5 IO 抽象与实现（`transport.hpp`、`fiber_transport.hpp`、`tls_transport.hpp`）

| API | 说明 | 位置 |
|---|---|---|
| `Transport::Read/Write/Closed` | 三条契约：Read 阻塞到至少 1 字节、Write 必须写满、Closed 表示读侧 EOF | `transport.hpp:58-105` |
| `Transport::ReadWithTimeout` | 带等待上限；默认实现退化为 `Read`（可能超 deadline） | `transport.hpp:92-95` |
| `MemoryTransport` | 测试替身；双向独立缓冲、`PushInbound/MarkEof/TakeOutbound` | `transport.hpp:107-213` |
| `FiberTransport` | Socket 生产实现；短写循环；超时/EOF/错误统一返回 0，细节在 `last_error()` | `fiber_transport.hpp:59-149` |
| `TlsTransport::CreateClient` | 建 SSL_CTX/SSL、非阻塞 fd、SNI、ALPN（h2 优先） | `tls_transport.hpp:112-115` |
| `TlsTransport::Handshake/NegotiatedProtocol` | TLS 握手并读 ALPN 结果 | `tls_transport.hpp:132-142` |
| `TlsTransport::Cancel/Close/LastError` | 取消等待、发 close_notify、错误诊断 | `tls_transport.hpp:139-169` |

---

## 3. 设计

### 3.1 HPACK：静态表 + FIFO 动态表 + O(1) 索引

**静态表 61 项**（RFC 7541 附录 A）以 1-based 数组存放（`hpack.cpp:172-242`），索引 1..61
直接命中；HPACK 动态表的相对索引 1 表示**最新**条目，因此 `full_index = 61 + relative`
（`hpack.cpp:682-691`）。这个“差 61”是 HPACK 最常见的 off-by-one 来源，代码在
`FindDynamicIndex` 与 `Decoder::DecodeBlock` 各做一次显式换算（`hpack.cpp:572-584`、682-691）。

**动态表**（`hpack.hpp:160-213`）的条目大小按 RFC 7541 §4.1 为 `name.size()+value.size()+32`
（`hpack.cpp:277-282`）；插入超限时从表尾（最旧）逐出（`hpack.cpp:328-354`）。查找不是线性
扫描：`m_index` 是 `name\0value -> 单调序号` 的哈希索引，相对索引由“当前序号 - 条目序号”
直接算出（`hpack.cpp:386-408`）。`hpack.hpp:187-191` 记录了动机：线性扫描版本在连接复用
场景下吞吐下降 36%。

**解码**（`hpack.cpp:517-654`）按首字节分派：

| 首字节模式 | 表示 | 是否入动态表 |
|---|---|---|
| `001xxxxx` | 动态表大小更新（5 位前缀） | 改表上限，不插入 |
| `1xxxxxxx` | 索引字段（7 位前缀） | 否 |
| `01xxxxxx` | 含增量索引的字面量（6 位前缀） | **是** |
| `0000xxxx` | 不含索引的字面量（4 位前缀） | 否 |
| `0001xxxx` | 绝不索引的字面量（4 位前缀） | 否 |

表大小更新只能位于 header block 开头且最多两条（先最小、再最终值），块中部出现或第三条
都返回 `kInvalidTableSize`（`hpack.cpp:524-539`）；新值超过本端 SETTINGS 上限同样拒绝
（`hpack.cpp:548-553`）。

**编码**（`hpack.cpp:707-784`）的择优顺序：

1. 完全命中静态表 → 7 位索引；
2. 完全命中动态表 → 7 位索引（连接复用的关键路径；`hpack.cpp:727-741` 记录了“编码器
   只写不读”的旧缺陷）；
3. 名字命中静态表 → 6 位“含增量索引字面量”，只发值；
4. 值只在 Huffman 结果**更短**时才用 Huffman，避免长随机串膨胀（`hpack.cpp:762-779`）；
5. 每个字面量都 `Add()` 到动态表，保证两端表同步。

**Huffman**（`huffman.cpp:46-161`）解码用二叉树逐位下降，而不是多级查表；257 个符号约 512
个节点，构造一次。填充必须是全 1 且不足 8 位，输入中出现 EOS 直接 `kEosInInput`
（`huffman.hpp:5-12`）。编码末尾用 EOS 的最高位补齐（`huffman.cpp:82-113`）。

### 3.2 帧层：两段式校验与增量解析

`DecodeFrame()` 的校验顺序是刻意的（`frame.cpp:179-222`）：

1. 帧头不足 9 字节 → `kNeedMore`；
2. 声明长度 > `max_frame_size` → `kFrameSizeError`（不必等 payload）；
3. stream id 规则（SETTINGS/PING/GOAWAY 必须 0，其余必须非 0）→ 立即 `kProtocolError`；
4. payload 未收全 → `kNeedMore`；收全才拷贝并写 `consumed`。

`Connection::TakeFrame()` 在此之上再做一次**帧长预检**（`connection.cpp:249-303`）：仅凭 9
字节头判超限，先尽力发 GOAWAY(FRAME_SIZE_ERROR) 再置 `kClosed`，**不读取 payload**，避免
单连接被 16MB 帧撑爆（`connection.cpp:260-275` 的 D14 注释）。这是边界测试与压力测试都
依赖的行为。

SETTINGS 的语义校验在帧层完成，不留给上层（`frame.cpp:236-302`）：ACK 必须空 payload；
非 ACK 长度必须是 6 的倍数；`ENABLE_PUSH` 只允许 0/1；`INITIAL_WINDOW_SIZE` 不得超过
2^31-1；`MAX_FRAME_SIZE` 必须在 `[16384, 16777215]`。非法值是**连接错误**，不能静默忽略。

### 3.3 流状态机：用两个 bool 重算七状态

`StreamStateMachine` 只有三个可变字段：`m_state`、`m_local_end`、`m_remote_end`
（`stream_state.hpp:203-214`）。`open`/`half-closed(*)`/`closed` 不是直接赋值，而是由
`RefreshFromEndFlags()` 根据两个 END_STREAM 标记重算（`stream_state.cpp:49-59`）：

```text
local_end && remote_end -> closed
local_end              -> half-closed(local)
remote_end             -> half-closed(remote)
否则                    -> open
```

`reserved(*)` 不参与这个等价关系，它只由 `SendPushPromise/ReceivePushPromise` 直接赋值
（`stream_state.cpp:157-166`、`244-253`）。所有 `Send*`/`Receive*` 事件方法都是**预检语义**：
非法转移返回 `kInvalidTransition` 且**不修改任何字段**，因此 `CanSendData()` 后再
`SendData()` 不会出现“校验失败但 END_STREAM 已被吃掉”（`stream_state.hpp:112-117`）。
`kClosed` 之后所有事件返回 `kStreamClosed`（`stream_state.cpp:72-74` 等）。

### 3.4 连接层：握手、CONTINUATION、双闸、并发流

**握手**（`connection.cpp:345-469`）：客户端先写 24 字节 preface，再写本地 SETTINGS
（表大小/帧上限/初始窗口/并发流/header 列表上限；客户端额外发 `ENABLE_PUSH=0`），然后
`FinishSettingsHandshake()` 循环取帧：收到非 ACK SETTINGS → `ApplyPeerSettings()` → 回 ACK →
`m_handshaked=true`；握手期 PING 自动回 ACK；其它帧暂时忽略（宽容策略，`connection.cpp:466-468`）。
服务端先读 preface 并 `memcmp` 比对，不匹配返回 `kProtocolError`（`connection.cpp:367-379`）。

**SETTINGS 运行期生效**（`connection.cpp:1308-1327`、`815-871`）：旧实现只回 ACK 不应用，
对端调小 `MAX_FRAME_SIZE`/`INITIAL_WINDOW_SIZE` 后本端仍按旧值发送，必然被拒。
`INITIAL_WINDOW_SIZE` 只按**差值**调整所有已存在流的流级发送窗口，**不影响连接级窗口**
（`connection.cpp:833-844`）——连接窗口只能由 WINDOW_UPDATE(0) 改变。
`HEADER_TABLE_SIZE` 变化会排队 `0x20` 表大小更新，拼到下一个 header block 首部
（`connection.cpp:819-828`、`873-885`）。

**CONTINUATION**：

- 发送：`WriteHeaderBlock()` 以 `peer_max_frame_size` 为单片上限（payload，不含 9 字节头）；
  一帧装得下走快路径，否则 HEADERS(无 END_HEADERS) + 若干 CONTINUATION，最后一片带
  END_HEADERS，`end_stream` 只加在 HEADERS 上（`connection.cpp:931-967`）；
- 接收：`AssembleHeaderBlock()` 循环取帧直到 END_HEADERS，期间出现非 CONTINUATION 或
  流 id 不一致都是协议错误；**边拼边累计**上限，不能等收完再判（`connection.cpp:978-1034`）。

**两道 header 上限**（`connection.hpp:80-97`）：

| 闸门 | 默认 | 防什么 | 代码 |
|---|---|---|---|
| `max_header_block_size` | 64 KiB | CONTINUATION 泛滥：压缩块本身撑爆内存 | `connection.cpp:982-1032`、1115-1128 |
| `max_header_list_size` | 64 KiB | HPACK 解压炸弹：压缩块小、解压后极大（每项 `name+value+32`） | `connection.cpp:1135-1148` |

单片 HEADERS 不走 `AssembleHeaderBlock`，因此必须**独立**再检查一次压缩块上限，否则一个
10MB 单帧可绕过（`connection.cpp:1115-1128`）。两个上限在测试里分别用 256 字节与 100 字节
验证（`tests/network/test_connection.cpp:675-747`）。

**MAX_CONCURRENT_STREAMS**（RFC 9113 §5.1.2）：

- 客户端 `StartRequest()` 在**分配流号之前**检查对端通告的上限；超限返回
  `kTooManyStreams`，被拒绝的尝试不消耗流号（`connection.cpp:498-508`）；
- 服务端收到新流时若 `OpenStreamCount() >= max_concurrent_streams`，先完成 HPACK 解码
  （保持动态表同步），再回 `RST_STREAM(REFUSED_STREAM)` 且**不把流放进流表**
  （`connection.cpp:1195-1209`）；
- `OpenStreamCount()` 统计 `open + half-closed(local) + half-closed(remote)`，**排除 closed**
  （`connection.cpp:695-707`）；关闭的流仍可有界保留在表里用于容忍迟到帧。

**GOAWAY 与 draining**：

- `SendGoAway()` 的 `last_stream_id` 取“对端发起、本端可能已处理的最大流 id”：客户端恒为 0
  （本实现不支持推送），服务端用 `m_max_seen_peer_stream_id`（`connection.cpp:1049-1058`）。
  旧实现把本端发起的流号也算进去，客户端会发出 `last_stream_id=1`，属于协议错误；
- 写 GOAWAY 成功后才迁移 `kDraining`（`connection.cpp:1060-1063`）；
- 收到 GOAWAY：记录 `m_peer_last_stream_id`，`kOpen → kDraining`，之后 `StartRequest()`
  返回 `kProtocolError`（`connection.cpp:1371-1388`、`486-493`）；已有流仍可收尾
  （`../state-machines/Http2Connection.md:31-44`）。

**流表回收**：`max_retired_streams` 默认 256；`ReapClosedStreams()` 只在 closed 流超过阈值时
按流 id 从小到大淘汰最老的，同时删除两条窗口记录（`connection.cpp:719-746`）。
`IsEvictedStream()` 让“已关闭但被淘汰”的流上的迟到 RST_STREAM/WINDOW_UPDATE 可被忽略，
并让迟到 DATA 的错误信息区分为 STREAM_CLOSED，而不是误判成 idle 流
（`connection.cpp:748-763`、1234-1241、1348-1357、1398-1405`）。

**PUSH_PROMISE**：客户端在 SETTINGS 通告 `ENABLE_PUSH=0`；收到 PUSH_PROMISE 一律置
`kClosed` 并返回 `kProtocolError`，**不能**当未知帧跳过——它的 header block 会更新对端
HPACK 编码器动态表，而本端不更新，跳过必然导致后续 header 失步（`connection.cpp:1420-1429`）。

### 3.5 流控：预检/提交两段式，窗口不阻塞

**窗口模型**（`connection.hpp:470-480`）：

- 连接级：`m_local_window`（允许对端发多少）、`m_remote_window`（对端允许我发多少），
  初值恒为 65535，**不随** `SETTINGS_INITIAL_WINDOW_SIZE` 改变；
- 流级：`m_stream_local_window` / `m_stream_remote_window`，流初值分别取本端/对端
  `INITIAL_WINDOW_SIZE`（`connection.cpp:887-891`）；
- 窗口用 `int64_t`：对端调小 `SETTINGS_INITIAL_WINDOW_SIZE` 时，流级发送窗口按**负差值**
  调整，可能变成负数；因此不能用无符号类型，`HasSendWindow()` 会一直拒绝到补回额度
  （`connection.cpp:833-844`、893-901）。

**发送路径**（`connection.cpp:568-628`）严格两段式：

1. 只读预检：`CanSendData()` → 帧尺寸（`data.size() > peer_max_frame_size` → `kUnsupported`）
   → `HasSendWindow()`（不足 → `kFlowControl`）；
2. 提交：`SendData()` 推进状态机 → `ConsumeSendWindow()` 扣连接级+流级 → `WriteFrame()`；
   扣减失败或写帧失败都 `ForceClose()` 该流，绝不留“状态已变、帧没发全”的中间态。

因此 `kFlowControl` 不是错误：被拒绝的调用**不消耗** END_STREAM，等 `WINDOW_UPDATE`
（再读一次事件）后重试即可（`connection.hpp:33-36`、`connection.cpp:586-605`）。

**接收路径**（`connection.cpp:1246-1305`）：先校验 `DATA.size()` 不超过连接级与流级可用
窗口，**再**推进状态机、扣窗口；否则窗口会被推成负数，既无法回报 `FLOW_CONTROL_ERROR`
也让补窗增量失真。扣减后当流级**剩余**窗口降到 `initial_window_size/2` 及以下时回 `WINDOW_UPDATE`；连接窗口
同理。增量取 `initial_window_size - 当前值` 并饱和到 `2^31-1`（`WINDOW_UPDATE` 的 increment
超过 2^31-1 是协议错误）。

**未实现**：窗口不足时不挂起等待，也不按读取速度自适应配平；由调用方重试
（`connection.hpp:33-36`）。DATA 不做分片：单帧超过 `SETTINGS_MAX_FRAME_SIZE` 返回
`kUnsupported`（`connection.cpp:594-600`），HEADERS 有 CONTINUATION，DATA 没有对应机制。

### 3.6 Transport：单驱动者与三种字节源

所有协议逻辑只调用 `Transport::Read/Write/Closed`（`transport.hpp:58-105`），因此：

- `MemoryTransport` 可在**单线程**里跑完整闭环，不需要 socket/Fiber；它的 `Read` 是非阻塞的，
  没数据返回 0，与“阻塞到至少 1 字节”的契约冲突，只适合“数据已预置”的测试
  （`transport.hpp:29-33`、144-157）；
- `FiberTransport` 把 `Read/Write` 映射到 `Socket::recv/send`，Hook + IOManager 在 Fiber 中
  挂起；`Write` 循环处理短写，失败置 `m_failed`（`fiber_transport.hpp:81-133`）；
- `TlsTransport` 把 socket 设为非阻塞，`SSL_connect/SSL_read/SSL_write` 遇到
  `WANT_READ/WANT_WRITE` 时用 `poll` 等待，分片 100ms 检查取消与总 deadline
  （`tls_transport.hpp:11-18`、`126-132`）。

**并发约束**：`Connection`、`StreamStateMachine`、`FiberTransport`、`TlsTransport` 全部
**非线程安全**，一个连接只能由一个 Fiber/线程驱动（`connection.hpp:221-225`、
`stream_state.hpp:97-103`、`fiber_transport.hpp:46-48`）。要并发处理多条连接，就每条连接
一个 Fiber；共享 `Connection` 需外部加锁。

---

## 4. 边界与已知限制

### 4.1 协议能力边界

| 未做 / 边界 | 具体行为 | 代码 |
|---|---|---|
| 服务器推送 | 客户端通告 `ENABLE_PUSH=0`，收到 PUSH_PROMISE 即关闭连接 | `connection.cpp:394-398`、1420-1429 |
| 优先级调度 | PRIORITY 帧能解析，不参与调度（RFC 9113 已弃用） | `frame.cpp:614-649`、`connection.hpp:37` |
| DATA 分片 | 单帧 > `peer_max_frame_size` 返回 `kUnsupported` | `connection.cpp:594-600` |
| 流控阻塞等待 | 窗口不足返回 `kFlowControl`，不 park | `connection.cpp:601-605` |
| TLS 服务端 | 只有 `CreateClient`，没有 server 侧 | `tls_transport.hpp:30-31` |
| 会话复用 / 0-RTT | 未实现 | `tls_transport.hpp:31` |
| HPACK 解码失败回滚 | 失败后动态表可能已部分变更，应断连而非重试 | `hpack.hpp:217-228` |
| GOAWAY 后自动重放 | 只记录 `last_stream_id`，不重放未处理流 | `connection.cpp:1376-1378` |
| QUIC / HTTP3 | 零实现代码；仅有选型文档 | `docs/quic.md` |
| 连接迁移 / 多路复用调度 | 无事件循环；`ReadEvent()` 单线程同步阻塞 | `connection.hpp:11-16` |

### 4.2 实现细节边界（容易踩）

- **HPACK 表大小扩容**：`Encoder::SetTableSize()` 只收缩不扩张；扩容必须由连接层排队
  `AppendTableSizeUpdate()`，否则对端解码器仍按旧上限解释索引，会以 COMPRESSION_ERROR
  断连（`hpack.hpp:291-306`、`connection.cpp:819-828`）。
- **连接级窗口不等于 INITIAL_WINDOW_SIZE**：对端 SETTINGS 只改流级窗口；连接级窗口只能靠
  `WINDOW_UPDATE(0)`（`connection.cpp:833-844`）。
- **GOAWAY 的 last_stream_id**：客户端发 0，服务端发已见最大客户端流 id；不要用本端流号
  （`connection.cpp:1049-1058`）。
- **孤立 CONTINUATION**：分发器收到它直接协议错误（正常序列由 `AssembleHeaderBlock`
  内联消费，`connection.cpp:1430-1437`）。
- **header 名必须小写**：大写是连接错误；伪头必须在普通字段之前、不能重复；trailers 不允许
  伪头；CONNECT 必须有 `:authority`（`connection.cpp:92-204`）。
- **`:scheme` 兼容开关**：默认只对“带 `:authority` 的请求”强制 `:scheme`，`strict_scheme=false`
  是为兼容既有测试；生产建议开启 `strict_scheme`（`connection.hpp:116-139`、
  `connection.cpp:87-95`）。
- **`enforce_peer_header_list_on_requests=false`**：客户端默认不提前检查对端
  `MAX_HEADER_LIST_SIZE`，请求超限由服务端拒绝；服务端响应方向始终检查
  （`connection.hpp:140-149`、`connection.cpp:510-520`、`646-651`）。
- **构建未链接核心源文件**：`go2cpp_network` 只编 `tls_transport.cpp`；`connection/frame/hpack/
  huffman/stream_state` 只在各测试 target 里编译（`cmake/Go2CppModules.cmake:46-54`、
  `cmake/Go2CppTests.cmake:354-580`）。生产使用 HTTP/2 需要把这些 `.cpp` 加入自己的 target
  或修改 CMake；当前没有 `go2cpp_http2` 库目标。
- **示例未注册**：`example/network/http2_h2c_client.cpp`、`http2_h2c_server.cpp` 不在
  `GO2CPP_EXAMPLES` 列表（`cmake/Go2CppTests.cmake:990-1045`），当前构建不产出这两个可执行
  文件；源码仍可作为用法参考。
- **编译告警**：`hpack.cpp:465` 的 `DecodeName` 在 `-Wall -Wextra` 下报 unused-function；
  不影响行为，但阻塞“零警告”目标。

---

## 5. 测试

### 5.1 ctest 用例清单（`cmake/Go2CppTests.cmake`）

| ctest 名称 | 源文件 | 覆盖 | 超时 |
|---|---|---|---|
| `go2cpp_hpack_test` | `tests/network/test_hpack.cpp` | 9 场景 + RFC 7541 官方向量 + 2000 随机 | 180s（`354-363`） |
| `go2cpp_frame_test` | `tests/network/test_frame.cpp` | 10 场景 + 3000 随机 | 180s（`365-373`） |
| `go2cpp_stream_state_test` | `tests/network/test_stream_state.cpp` | 5 场景 + 84 组合 + 5000 随机 | 180s（`375-383`） |
| `go2cpp_connection_test` | `tests/network/test_connection.cpp` | 11 场景（握手/闭环/CONTINUATION/流控/header 上限） | 180s（`386-405`） |
| `go2cpp_fiber_transport_test` | `tests/network/test_fiber_transport.cpp` | 2 场景（socketpair 读写、真实 socket 闭环） | 180s（`497-513`） |
| `go2cpp_http2_stress` | `tests/network/test_http2_stress.cpp` | 6 类场景 + 1b 拒绝路径，main 共 11 次调用 | 300s（`515-527`） |
| `go2cpp_http2_boundary` | `tests/network/test_http2_boundary.cpp` | 21 固定用例 + 200000 fuzz | 180s（`529-540`） |
| `go2cpp_http2_state_matrix` | `tests/network/test_http2_state_matrix.cpp` | 84 + 12 + 20 + 24 格 | 300s（`552-566`） |
| `go2cpp_http2_state_desync_repro` | `tests/network/test_http2_state_desync_repro.cpp` | 6 组回归（R1/R2/R13） | 120s（`568-582`） |

`go2cpp_tls_alpn_test` **不注册 ctest**：它需要 `openssl s_server -alpn h2` 提供对端，
属非 hermetic 测试，只构建独立二进制，由 `scripts/verify_tls_alpn.sh` 驱动
（`cmake/Go2CppTests.cmake:385-394`、`tests/network/test_tls_alpn.cpp:80-99`）。

### 5.2 各测试的数据与断言

**HPACK（`test_hpack.cpp`）**：RFC 7541 §C.1 整数向量（10→`0a`、1337→`1f 9a 0a`、42→`2a`，
`61-79`）；静态表 61 项；动态表 FIFO/逐出；§C.2/C.3 官方请求块逐字节比对（`ScenarioRfcExamples`）；
编码→解码往返；错误处理（索引越界、截断、Huffman 非法、表大小超限）；表大小更新；2000 轮
随机往返（`503`）；Huffman 的 `www.example.com` 编码必须等于
`f1 e3 c2 e5 f2 3a 6b a0 ab 90 f4 ff`（`442-458`）。

**帧层（`test_frame.cpp`）**：10 场景——帧头往返/增量、帧长上限、stream id 规则、SETTINGS、
PING、DATA（含 padding）、HEADERS/CONTINUATION、RST/GOAWAY/WINDOW_UPDATE/PRIORITY、增量解析
与粘包、3000 轮随机往返（`tests/network/test_frame.cpp:44-54`、`454`）。

**流状态机（`test_stream_state.cpp`）**：完整生命周期、84 组合穷举矩阵（7 状态 × 12 事件，
`80-198`）、PUSH_PROMISE 路径、RST_STREAM 关闭语义、5000 轮随机事件序列（`261-303`）。

**连接层（`test_connection.cpp`）**：11 场景。关键数据：

- 场景 9：50000 个 `x` 的 header value（Huffman 对 `x` 是 5 位，编码后约 31250 字节
  > 16384），要求恰好 1 个 HEADERS + ≥1 个 CONTINUATION，服务端拼回等长原值
  （`453-535`）；
- 场景 10：`initial_window_size=64`，发 64 字节成功、再发 1 字节必须 `kFlowControl`；
  补流级 + 连接级两个 WINDOW_UPDATE 后才能再发 16 字节；接收侧 64 字节耗尽窗口后必须回
  WINDOW_UPDATE（`539-648`）；
- 场景 11：压缩块上限 256 + 4000 字节 value → `kHeaderListTooLarge`；解压列表上限 100 +
  200 字节 value → `kHeaderListTooLarge`；默认 65536 + 200 字节 value 成功且 header 数=3
  （`652-781`）。

**FiberTransport（`test_fiber_transport.cpp`）**：socketpair + 项目真实 `Socket`，场景 1 读写
往返、场景 2 完整 HTTP/2 请求/响应闭环（`62-231`）。

**TLS/ALPN（`test_tls_alpn.cpp`）**：连接 `openssl s_server -alpn h2`，断言 ALPN 结果非空且
等于 `"h2"`（`62-64`）；默认端口 18443（`81`）。

**压力（`test_http2_stress.cpp`）**：全部用 `MemoryTransport`，排除网络抖动；场景 1
并发流 100/1000/5000、场景 1b 服务端拒绝路径、场景 2 串行 1000 次延迟、场景 3 大 body
1MB/10MB、场景 4 连接复用 1000 次、场景 5 HPACK 唯一 header 5000 次、场景 6 CONTINUATION
1MB/10MB（`638-657`）。每条场景都断言**接受数不超过通告的 100**、每个被接受请求都被收到、
大 body 必须走到 WINDOW_UPDATE、复用后平均字节数 < 首帧一半、CONTINUATION 解码值等长。

**边界（`test_http2_boundary.cpp`）**：每个“必须不崩溃”的用例在 `fork()` 子进程里跑，父进程
3s 看门狗；覆盖 9 个畸形帧、7 个 HPACK 攻击、2 个 CONTINUATION 攻击、3 个流控攻击，最后
200000 次随机位翻转/截断 fuzz（固定种子 `20261017`、9 个语料、30s 看门狗，`484-601`）。

**状态矩阵（`test_http2_state_matrix.cpp`）**：HTTP/2 流 7×12=84 格逐格断言返回值与下一状态
（`164-304`）；HTTP/2 连接握手态 2×6=12 格；`ResponseWriter` 4×5=20 格；SSE 4×6=24 格；
另加“非法事件不污染后续合法事件”与两个线程同时驱动同一状态机的探针（`1-26`）。

**回归（`test_http2_state_desync_repro.cpp`）**：R1a/R1b（`WriteData` 尺寸/流控校验早于状态
推进）、R2a/R2b（发/收 GOAWAY 后 draining）、R13（接收窗口越界必须
`FLOW_CONTROL_ERROR`）、R2c（写失败后 `kClosed`），共 35 个 `Check`（`295-317`）。

### 5.3 本次实测（2026-10-26，`loadavg=0.34`）

指定构建目录 `/UserData/build/go2cpp-mcp-integrated` 的 `GO2CPP_BUILD_TESTS=OFF`，没有这些
测试二进制。为核对文档数据，本次用 `g++ 13.3.0 -std=c++20 -O2 -Wall -Wextra -Wpedantic`
直接编译并运行了 7 个纯协议测试（不依赖 runtime；`state_matrix`/`fiber_transport`/`tls_alpn`
需要 runtime 或外部服务，未运行）：

| 二进制 | 结果 | 关键输出 |
|---|---|---|
| `hpack` | `failures=0` | 随机往返 2000 轮完成 |
| `frame` | `failures=0` | 随机往返 3000 轮完成 |
| `stream_state` | `failures=0` | 84 组合；随机 5000 轮：ok=18125 invalid=14675 closed=67200 |
| `connection` | `failures=0` | HEADERS=1 CONTINUATION=2；流控补窗 2；两个 header 上限 case 均 `header-list-too-large` |
| `boundary` | `failures=0` | 9+7+2+3 用例；200000 次 fuzz 无崩溃 |
| `desync` | `failures=0` | `checks=35` |
| `stress` | `failures=0` | 见下表 |

`stress` 单次采样（非基准；用于确认路径被真实走到）：

| 场景 | 数据 |
|---|---|
| 并发流 100 / 1000 / 5000 | 接受 100，拒绝 0 / 900 / 4900；收到 100 |
| 服务端拒绝路径 | 发送 120，接受 100，拒绝 20，服务端流表 100 |
| 串行 1000 次 | p50=1.23µs，p90=1.54µs，p99=1.92µs，max=5.23µs |
| 大 body 1MB / 10MB | 373.7 / 421.3 MB/s；WINDOW_UPDATE=42 / 426 |
| 连接复用 1000 次 | 完成 100，拒绝 900；首帧 48B，平均 14.3B |
| HPACK 唯一 header 5000 次 | 完成 100，拒绝 900；总字节 5323B；内存增长 0KB |
| CONTINUATION 1MB / 10MB | 编码后 918039 / 9180112B；帧 57 / 561；解码值 1048576 / 10485760B |

> **测试设计局限（如实标注）**：`stress` 的“连接复用”和“HPACK 动态表”场景只发请求、不关闭
> 流，因此 100 条后触发 `MAX_CONCURRENT_STREAMS`，后面的 900 次全部被拒；它们实际只测了
> 100 次，不足以证明长连接下动态表长期稳定。`test_http2_stress.cpp:512-516` 的断言
> （`done <= 100`、`avg < first*0.5`）能过，但覆盖面被高估。另：场景 3 用
> `kChunk=16375`（16384-9）是历史保守值，实现的上限是 payload 16384
> （`connection.cpp:594-600`）。

### 5.4 复现命令（无需 CMake 测试配置）

```sh
cd /UserData/CodexWorkSpace/Go2cpp
g++ -std=c++20 -O2 -Wall -Wextra -Wpedantic -I include \
  tests/network/test_hpack.cpp src/network/http2/hpack.cpp src/network/http2/huffman.cpp \
  -o /tmp/hpack && /tmp/hpack
g++ -std=c++20 -O2 -Wall -Wextra -Wpedantic -I include \
  tests/network/test_connection.cpp src/network/http2/connection.cpp \
  src/network/http2/frame.cpp src/network/http2/hpack.cpp \
  src/network/http2/huffman.cpp src/network/http2/stream_state.cpp -o /tmp/conn && /tmp/conn
```

其余测试替换源文件与 `main` 即可；`fiber_transport`/`state_matrix` 需要链接
`go2cpp_runtime`，应按 `cmake/Go2CppTests.cmake` 的 target 配置构建。

---

## 6. 相关文档与维护

- 流状态转化表：[../state-machines/Http2StreamState.md](../state-machines/Http2StreamState.md)。
- 连接生命周期/握手转化表：[../state-machines/Http2Connection.md](../state-machines/Http2Connection.md)。
- HTTP/1 客户端/服务端：[http1.md](http1.md)；两者**不共享 Parser**。
- TLS/ALPN 验证脚本：`scripts/verify_tls_alpn.sh`；互通脚本
  `scripts/minimal_h2c_server.py`。
- QUIC/HTTP3 选型：[../quic.md](../quic.md)、[../quic-go-reading-guide.md](../quic-go-reading-guide.md)
  （**零实现**，不要与本文能力混淆）。
- 修改本模块时同步检查：`ConnectionConfig` 默认值、`ConnStatus` 枚举、两道 header 上限、
  GOAWAY 的 `last_stream_id` 口径、`OpenStreamCount()` 的计数口径；这些都被测试或对端行为依赖。
