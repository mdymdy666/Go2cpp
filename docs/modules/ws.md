# 模块文档：ws（WebSocket / RFC 6455）

> **状态：现行**。基线 `HEAD 2ba2f4e`（2026-10-26 核对；`src/`、`include/` 相对 HEAD 无改动，本次只改文档）。
> 本文是 WebSocket 协议层的**唯一现行文档**：概述、API、握手/帧/分片/关闭设计、边界与测试集中在这里。
> 旧文档 `docs/websocket.md` 已并入本文并在其顶部加了合并标注；`docs/state-machines/WebSocket.md`
> 保留为状态机明细（已标注为本文配套），两者不再单独维护，API、默认值与行号以本文和当前代码为准。
> 引用约定：`file:line` 指当前工作树。

---

## 1. 概述与职责

`go2cpp::net::WebSocket` 是 **RFC 6455 应用层协议对象**：在**已经建立**的
`net::Socket` 上完成握手、帧编解码、分片消息重组、Ping/Pong 与 Close。
它不做监听、连接、路由、鉴权或连接池，也不拥有调度器；所有阻塞都落在
`Socket::recv/send` 上，因此普通线程与 `IOManager` 管理的 Fiber 都能用
（`include/go2cpp/network/websocket.hpp:3-29`、`src/network/websocket.cpp:5-7`）。

| 做 | 不做 |
|---|---|
| 服务端 `Accept`/`AcceptWithPending`、客户端 `Connect` 握手 | 不负责 TCP connect/listen；不解析 Upgrade 之前的 HTTP 报文 |
| 帧编码/解码、掩码方向校验、长度校验 | 不实现 permessage-deflate、扩展协商、子协议协商 |
| 分片消息重组；发送方向按帧上限自动分片 | 不提供 TLS、HTTP/2 CONNECT、代理或重定向 |
| Ping/Pong 自动响应、Close 握手、UTF-8 校验 | 不做路由、鉴权、限流、审计、会话恢复 |

依赖：`net::Socket`、`http::Request/Response`、`http::Parser`（仅客户端响应解析）、
`SocketStream`（握手写入）、`sync::Mutex`（Fiber/线程混合锁）。

---

## 2. 文件布局

| 路径 | 内容 | 规模 |
|---|---|---|
| `include/go2cpp/network/websocket.hpp` | 角色/opcode/错误/配置/消息/`WebSocket` 类 | 264 行 |
| `src/network/websocket.cpp` | 握手、base64/SHA-1、帧层、分片、关闭实现 | 834 行 |
| `include/go2cpp/websocket.hpp`、`websocket.h`、`network/websocket.h` | 兼容转发头（只 include 真身） | 各 3 行 |
| `tests/network/test_network.cpp` | WebSocket 握手与 socketpair 端到端用例 | 690 行（WS 在 `:560-689`） |
| `example/network/websocket_demo.cpp` | 最小示例 | 58 行 |
| `example/network/websocket_full.cpp` | 7 场景全面示例 | 368 行 |
| `docs/state-machines/WebSocket.md` | 握手/关闭/帧读取/分片四张状态表 | 74 行 |

构建：`src/network/websocket.cpp` 编入 `go2cpp_network`（`cmake/Go2CppModules.cmake:52`），
不引入额外第三方依赖；根路径 `include/go2cpp/websocket.hpp` 在 `go2cpp` 命名空间导出别名
（`websocket.hpp:257-264`）。

---

## 3. API

### 3.1 类型与配置

| 类型 | 语义 | 位置 |
|---|---|---|
| `enum class WebSocketRole { Client, Server }` | 角色决定掩码方向与握手形态 | `websocket.hpp:45` |
| `enum class WebSocketOpcode` | `Continuation=0x0`、`Text=0x1`、`Binary=0x2`、`Close=0x8`、`Ping=0x9`、`Pong=0xA` | `:48-55` |
| `enum class WebSocketError` | `Closed/Timeout/Io/Protocol/InvalidHandshake/MessageTooLarge/InvalidState` | `:58-67` |
| `struct WebSocketResult` | `error`、`system_error`、`message`、`ok()`、`success()` | `:70-76` |
| `struct WebSocketMessage` | `opcode` + 已组装 `payload`；`text()` 便捷判断 | `:79-83` |
| `struct WebSocketConfig` | 帧/消息上限、IO 超时、auto_pong、UTF-8 校验、发送分片开关 | `:86-98` |

`WebSocketConfig` 默认值（`websocket.hpp:87-97`）：

| 字段 | 默认 | 说明 |
|---|---|---|
| `max_frame_payload` | 16 MiB | 单帧 payload 上限；读方向超限发 1009 |
| `max_message_payload` | 64 MiB | 整条消息上限；收发两个方向都生效 |
| `io_timeout` | `Socket::Duration::max()` | 默认无限等待；覆盖握手写和每次读写 |
| `auto_pong` | `true` | 读到 Ping 自动回 Pong |
| `validate_utf8` | `true` | 文本消息与 Close reason 的 UTF-8 校验 |
| `fragment_outgoing` | `true` | 超过 `max_frame_payload` 的消息自动拆 continuation 帧 |

### 3.2 握手与收发

| API | 语义 | 位置 |
|---|---|---|
| `static bool ValidateUpgradeRequest(const http::Request&, std::string& key, std::string* reason = nullptr)` | 校验 Upgrade 请求并提取 16 字节 key | 声明 `:133`；实现 `websocket.cpp:208-238` |
| `static http::Response BuildUpgradeResponse(const http::Request&)` | 合法请求返回 101 + `Sec-WebSocket-Accept`；非法返回 400 且 `close=true` | 声明 `:137`；实现 `:240-253` |
| `static Ptr Accept(Socket::Ptr, const http::Request&, WebSocketConfig = {}, WebSocketResult* = nullptr)` | 服务端写 101 并创建对象 | 声明 `:139`；实现 `:259-262` |
| `static Ptr AcceptWithPending(Socket::Ptr, const http::Request&, std::string pending, WebSocketConfig = {}, WebSocketResult* = nullptr)` | 接管 HTTP Parser 多读的首段 WS 字节 | 声明 `:142`；实现 `:264-292` |
| `static Ptr Connect(Socket::Ptr, std::string host, std::string target = "/", WebSocketConfig = {}, WebSocketResult* = nullptr)` | 客户端发 Upgrade、校验 101 与 Accept | 声明 `:146`；实现 `:306-392` |
| `WebSocketResult read_message(WebSocketMessage&)` | 读并组装一条完整业务消息；Ping/Pong 内联处理 | 声明 `:167`；实现 `:673-749` |
| `WebSocketResult send_text(std::string_view)` | 发送文本；本地 UTF-8 与消息上限校验 | 声明 `:169`；实现 `:751-758` |
| `WebSocketResult send_binary(std::string_view)` | 发送二进制；只校验消息上限 | 声明 `:171`；实现 `:759-765` |
| `WebSocketResult ping(std::string_view = {})` | 发送 Ping；payload ≤125 | 声明 `:173`；实现 `:766` |
| `WebSocketResult pong(std::string_view = {})` | 发送 Pong；payload ≤125 | 声明 `:175`；实现 `:767` |
| `WebSocketResult close(std::uint16_t code = 1000, std::string_view reason = {})` | 发 Close 并进入关闭态；重复调用幂等 | 声明 `:177`；实现 `:769-787` |
| `WebSocketRole role()` / `bool closed()` / `Socket::Ptr socket()` | 只读访问器 | `:179-181` |

**生命周期**：`std::shared_ptr` 持有；读方向单线程（`m_read_mutex`），写方向可多线程
（`m_write_mutex`）；析构调用 `abort_transport()` 唤醒其他 M/Fiber 上的阻塞读写，
但**不 close fd**，因为 Socket 所有权可能与他人共享（`websocket.hpp:113-116`、
`websocket.cpp:399-410`）。

---

## 4. 设计

### 4.1 服务端握手

`ValidateUpgradeRequest()`（`websocket.cpp:208-238`）按顺序检查：

1. 方法必须是 `GET`，版本必须是 `HTTP/1.1`（`:219-220`）；
2. 不能有 HTTP body：`body` 为空、`Content-Length` 缺失或为 `0`、无
   `Transfer-Encoding`（`:221-224`）；
3. `Host` 非空（`:225`）；
4. `Upgrade` 的逗号列表含 `websocket`，`Connection` 含 `upgrade`，大小写不敏感
   （`:186-197`、`:226-228`）；
5. `Sec-WebSocket-Version` 必须是 `13`（`:229-230`）；
6. `Sec-WebSocket-Key` 去空白后无控制字符，合法 base64 且解码恰好 16 字节
   （`:232-237`；base64 解码在 `:60-99`）。

`BuildUpgradeResponse()`（`:240-253`）返回 `101 Switching Protocols`，设
`Upgrade: websocket`、`Connection: Upgrade`、`Sec-WebSocket-Accept`；Accept 是
`base64(SHA1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"))`（GUID 在 `:41`，
SHA-1 在 `:117-164`，拼接在 `:818-821`）。请求非法时返回 400 并置 `close=true`
（`:243-247`）。

`AcceptWithPending()`（`:264-292`）先校验请求，再按 `io_timeout` 设置 socket 发送
超时（若有限，`:276-280`），用 `SocketStream::write_all` 写 101 响应，最后创建
`WebSocketRole::Server` 对象并把 `pending` 交给 `m_prefetched`。`Accept()` 等价于
`pending={}`（`:259-262`）。

**为什么需要 `AcceptWithPending`**：HTTP Parser 可能和 Upgrade 请求在同一次 `recv`
中读到首个 WS 帧；服务端必须把 `parser.take_remaining()` 传入，否则这段字节被丢弃，
首个帧永远读不到（`docs/websocket.md` 的原始说明，现并入本文）。`read_exact()` 会优先
消费 `m_prefetched`（`:412-424`）。

**失败不关闭 Socket**：`Accept*` 校验/写响应失败时只填 `result` 并返回空 `Ptr`，
调用方仍拥有 Socket（`websocket.hpp:28`、`websocket.cpp:269-287`）。

### 4.2 客户端握手

`Connect()`（`websocket.cpp:306-392`）：

1. 校验 `target` 非空且以 `/` 开头、`host` 非空、两者无控制字符（`:308-317`）；
2. `make_client_key()` 生成 16 字节随机数并 base64（`:823-832`）；失败返回
   `InvalidHandshake`；
3. 拼 `GET target HTTP/1.1`，带 `Host`、`Upgrade`、`Connection`、
   `Sec-WebSocket-Key`、`Sec-WebSocket-Version: 13`（`:325-332`）；
4. 按 `io_timeout` 设置发送超时并写出请求（`:333-344`）；
5. **逐字节**读到 `\r\n\r\n`，每轮 recv 的剩余预算按绝对 deadline 计算；响应头上限
   64 KiB，超限返回 `InvalidHandshake`（`:345-376`）；
6. 用 `http::Parser` 解析响应，要求 `ParseState::Complete`、状态 101、
   `Upgrade`/`Connection` token 正确、`Sec-WebSocket-Accept` 精确匹配（`:378-388`）；
7. 创建 `WebSocketRole::Client` 对象（`:389-391`）。

任何一步失败都返回空 `Ptr` 并填 `result`，**不关闭**传入的 Socket（`:304`）。
客户端不校验 `Host` 与 `Origin` 的对应关系，也不处理重定向/代理。

### 4.3 帧格式与校验

RFC 6455 基础头 2 字节（`websocket.cpp:480-533`）：

```
 0                   1
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5
+-+-+-+-+-------+-+-------------+
|F|R|R|R| opcode|M| Payload len |
|I|S|S|S|  (4)  |A|     (7)     |
|N|V|V|V|       |S|             |
| |1|2|3|       |K|             |
+-+-+-+-+-------+-+-------------+
```

- FIN=`0x80`，RSV=`0x70`（必须为 0，否则 1002，`:486`）；
- opcode 只接受 0/1/2/8/9/10，其它立即协议错误（`:487-491`）；
- MASK=`0x80`；长度 7 位：`<126` 直接用，`126` 再读 2 字节，`127` 再读 8 字节
  （`:497-510`）；
- 拒绝非最小编码：126 后面的值 `<126`、127 后面的值 `<65536` 都是协议错误
  （`:502`、`:509`）；
- 127 形态的最高位必须为 0，且长度不能超过 `SIZE_MAX`（`:506`、`:518-519`）；
- 控制帧（opcode≥8）必须 `FIN=1` 且长度 ≤125，否则协议错误（`:511-514`）；
- 数据帧长度超过 `max_frame_payload` 时按 1009 关闭（`:512-514`）；
- 读掩码键 4 字节，读满 payload 后逐字节 XOR 解掩码（`:516-531`）；
- `std::bad_alloc` 时 `abort_transport()` + `m_closed=true`，返回
  `MessageTooLarge`/`ENOMEM`（`:520-526`）。

写帧（`websocket.cpp:552-622`）按角色决定是否掩码：

- 客户端必须掩码，服务端必须不掩码（`:563`）；4 字节掩码键用 `std::random_device`
  生成（`:582-592`），生成失败返回 `Io`；
- 长度编码选择最短合法形态：`<126` 单字节、`<=0xFFFF` 用 126、其它用 127
  （`:569-581`）；
- 先写头（含掩码键），再写 payload；客户端会先把 payload 拷贝并 XOR 掩码
  （`:593-620`），因此发送大消息有额外一份内存拷贝；
- 任一次写失败都 `abort_transport()` + `m_closed=true`（`:594-598`、`:602-607`、
  `:616-620`）。

### 4.4 掩码方向

RFC 6455 §5.1 要求客户端发出的帧必须掩码、服务端发出的帧必须不掩码。
`read_frame()` 按本端角色校验对端帧（`websocket.cpp:492-495`）：

- 服务端读到**未掩码**帧 → 协议错误；
- 客户端读到**带掩码**帧 → 协议错误。

`write_frame_unlocked()` 用 `m_role == WebSocketRole::Client` 决定是否加掩码
（`:563`）。测试用 socketpair 覆盖客户端发送掩码帧、服务端按角色解掩码的路径
（`tests/network/test_network.cpp:580-629`）；反向掩码被拒的负例目前没有自动化测试。

### 4.5 分片与消息重组

**读方向**（`websocket.cpp:673-749`）：

- 第一条 Text/Binary 且 `FIN=0`：记录 `m_fragmented=true`、`m_fragment_opcode`、
  首片 payload（`:745-747`）；
- Continuation：`m_fragmented` 必须为真，否则协议错误（`:719-720`）；累计长度超过
  `max_message_payload` 发 1009（`:721-723`）；`FIN=1` 时交付整条消息（`:726-731`）；
- 分片期间再收到 Text/Binary：协议错误（`:733`）；
- 文本消息在**整条消息**上做 UTF-8 校验，失败发 1007（`:729-730`、`:741-742`）；
- 单帧 Text/Binary 且 `FIN=1` 直接交付；超过 `max_message_payload` 发 1009
  （`:736-743`）。

**写方向**（`websocket.cpp:624-648`、`:751-765`）：

- `fragment_outgoing=true` 且消息长度 > `max_frame_payload` 时，`send_message()`
  持**整条消息**的写锁拆成若干 continuation 帧，首帧用原 opcode，末帧 `FIN=1`
  （`:634-646`）；帧之间不会插入其它消息的帧；
- `max_frame_payload == 0` 时直接返回 `MessageTooLarge`（`:630-632`）；
- `fragment_outgoing=false` 时恢复严格单帧语义：超过帧上限的消息发不出去
  （`websocket.hpp:92-97`、`websocket.cpp:754-757`）；
- 发送文本前会按整条消息做 UTF-8 校验（`:752`），不是逐帧校验。

### 4.6 Ping/Pong

- 读方向：`read_message()` 收到 Ping 时，若 `auto_pong=true` 立即回 Pong，
  payload 原样回显（`websocket.cpp:680-689`）；收到 Pong 直接丢弃（`:690`）；
  因此业务层**不会**看到 Ping/Pong 消息，`read_message()` 只返回 Text/Binary/Close。
- 写方向：`ping()`/`pong()` 走 `send_control()`，payload >125 返回 `Protocol`，
  否则 `write_frame(..., fin=true)`（`:650-653`、`:766-767`）。
- 控制帧可以穿插在分片消息之间，`read_message()` 的控制帧处理在分片状态之前，
  不会破坏 `m_fragment_payload`。

### 4.7 Close 与错误关闭

`close()`（`websocket.cpp:769-787`）：

1. 校验 close code 与 reason：reason ≤123 字节（125 减 2 字节 code），且
   `validate_utf8` 时必须是合法 UTF-8（`:770-771`）；
2. 取写锁；若 `m_close_sent` 已为真，置 `m_closed` 并幂等成功（`:772-776`）；
3. 构造 payload：2 字节 code + reason；即使 reason 为空也**总是带 2 字节 code**
   （`:777-781`）；
4. 写 Close 帧，置 `m_close_sent`、`m_closed=true`，`abort_transport()` 唤醒阻塞
   读写（`:782-786`）。

合法 close code（`:789-792`）：`1000-1014` 中排除 `1004/1005/1006/1015`，以及
`3000-4999`。其它值返回 `Protocol`，不会发帧。

收到 Close（`:691-718`）：

- payload 长度为 1 → 协议错误（Close 的 code 必须 2 字节，`:692`）；
- 长度 ≥2 时解析 code/reason；code 非法或 reason 非法 UTF-8 → `protocol_error()`
  （`:693-700`）；
- 若本端还没发过 Close，按原 payload 回一个 Close（`:701-713`）；
- 置 `m_closed=true`，把 Close 作为 `WebSocketMessage` 返回**一次**（`:714-717`）；
- 回 Close 写失败时置 `m_closed` 并返回写错误，不返回 Close 消息（`:708-711`）。

`protocol_error(message, code=1002)`（`:655-659`）先 `close(code, "protocol error")`，
再返回 `WebSocketError::Protocol`。各类不可恢复错误的 close code：

| 场景 | code | 位置 |
|---|---|---|
| RSV 非 0、未知 opcode、掩码方向错、非最小长度、控制帧非法、意外 Continuation、分片期间新数据帧、非法 Close | 1002 | `:486`、`:490`、`:495`、`:502`、`:509`、`:513`、`:692`、`:699`、`:720`、`:733` |
| 文本消息 UTF-8 非法 | 1007 | `:730`、`:742` |
| 帧/消息超过上限、长度溢出 | 1009 | `:514`、`:519`、`:723`、`:737` |

**关闭语义要点**：`close()` 发出 Close 后立即把 `m_closed` 置真，**不等待对端回
Close**；`closed()` 为真后所有发送返回 `Closed`。`read_message()` 的 IO 失败只返回
错误、不自动 `close()`，是否关闭由调用方决定（`websocket.hpp:28`、`websocket.cpp:20-21`）。
析构/`abort_transport()` 只取消底层读写，不 close fd（`:399-410`）。

### 4.8 并发、超时与生命周期

- **读**：`read_message()` 全程持 `m_read_mutex`（`sync::Mutex`，Fiber/线程混合），
  同一对象只允许一个读者；持锁期间 `Socket::recv` 可能 park（`:673-675`）。
- **写**：`write_frame()`/`send_control()` 每帧取 `m_write_mutex`；`send_message()`
  为整条消息持锁，保证分片帧不被其它写者插入（`:535-538`、`:624-627`）。
- **超时**：`io_timeout` 是每次 `read_exact`/`write_all` 的**总 deadline**，
  不是每轮 recv/send 重新计时（`:412-432`、`:444-456`）；握手写也用它设置
  socket 发送超时（`:276-280`、`:333-337`）。
- **取消**：`abort_transport()` 调 `Socket::cancel_read()` 与 `cancel_write()`，
  使用独立取消代际，可跨线程/Fiber 唤醒等待；不转移 Socket 所有权（`:404-410`）。
- **对象销毁**：析构置 `m_closed` 并取消读写；调用方仍负责 Socket 的最终关闭。
- **锁选择**：两把锁都是 `sync::Mutex` 而不是 `std::mutex`，因为持锁期间可能 park；
  业务代码不应在外层再包可能 park 的 Socket 调用（`websocket.hpp:236-240`）。

---

## 5. 边界与已知限制

- **无 TLS**：`WebSocket` 只接收已连接的 `net::Socket`；需要 `wss://` 时由调用方
  先建 TLS 传输，但当前 `TlsTransport` 是 HTTP/2 的 `Transport` 接口，不是
  `Socket`，不能直接套在本模块上（`docs/websocket.md` 原限制，仍成立）。
- **无扩展/子协议**：不支持 permessage-deflate、`Sec-WebSocket-Extensions`、
  `Sec-WebSocket-Protocol` 协商；`BuildUpgradeResponse()` 不会回选子协议。
- **无 HTTP/2 CONNECT**：WebSocket over HTTP/2 未实现。
- **无会话恢复/重连**：对象关闭后不可复用；没有跨连接会话状态。
- **服务端需要先解析 HTTP**：`Accept*` 的 `request` 必须由 `http::Parser` 产出；
  Parser 多读的字节必须走 `AcceptWithPending`，否则丢帧。
- **握手只做基础校验**：不校验 `Origin`、不校验 `Host` 与目标的一致性、不处理
  认证头、不跟随重定向；这些属于上层 NetService。
- **客户端响应头 64 KiB 上限**：超过即 `InvalidHandshake`，没有可配置项
  （`websocket.cpp:370-373`）。
- **无限等待是默认值**：`io_timeout` 默认 `Socket::Duration::max()`；生产必须设置
  有限值，否则慢对端/半开连接会让 Fiber 长期挂起（`websocket.hpp:89`）。
- **`max_frame_payload=0`**：读方向任何非空帧都超限；写方向 `send_message()`
  直接返回 `MessageTooLarge`，属于配置错误而不是合法极限。
- **Close 不等对端**：`close()` 发出即 `closed()`；对端可能还没处理 Close。
- **Close payload 总带 code**：API 不提供「空 Close payload」；默认 code 1000。
- **读方向不交付 Ping/Pong**：业务层无法观察 Ping/Pong；需要统计时要在协议层加钩子。
- **UTF-8 校验范围**：只校验文本消息与 Close reason；二进制不校验；分片文本在整条
  消息上校验，因此中途分片可能先被缓存再被拒。
- **测试缺口**：没有独立的 frame 单元测试覆盖 RSV/掩码方向/非最小长度/非法 UTF-8
  等负例；现有自动化集中在握手向量与 socketpair 端到端（见 §6）。

---

## 6. 测试

### 6.1 自动化用例

WebSocket 用例在 `tests/network/test_network.cpp`，由 `run_network_tests()` 执行：

| 用例 | 断言 | 位置 |
|---|---|---|
| 服务端握手校验 | 合法 Upgrade 通过；`Sec-WebSocket-Accept` 等于 RFC 6455 示例值 `s3pPLMBiTxaQ9kYGzzhZRbK+xOo=`；非法 base64 key 被拒 | `:560-578` |
| socketpair 端到端 | 客户端 `Connect` + 服务端 `Accept`；文本 `hello`/`world`；126 字节二进制走扩展长度；`close(1000,"done")` 后服务端读到 Close | `:580-629` |
| Pending 字节与分片 | HTTP 请求与两个 masked 分片帧一次写入；`AcceptWithPending(parser.take_remaining())` 后重组出 `abcd` | `:631-689` |

注册：`tests/test_main.cpp:100` 在 `network` 过滤器下调用 `run_network_tests()`；
CMake 把 `test_network.cpp` 编进 `go2cpp_tests`（`cmake/Go2CppTests.cmake:41`），
`go2cpp_tests` 注册为 ctest（`:216`）。运行：

```bash
GO2CPP_TEST_FILTER=network ./build/go2cpp_tests
# 或
ctest --test-dir build -R go2cpp_tests --output-on-failure
```

### 6.2 示例

- `example/network/websocket_demo.cpp`（58 行）：最小握手 + echo。
- `example/network/websocket_full.cpp`（368 行）：7 个场景——握手校验、握手响应、
  完整 socketpair 会话、二进制、Ping/Pong、关闭语义、角色/底层 socket
  （文件头 `:15-26`）。
- 两者在 `cmake/Go2CppTests.cmake:1007-1008` 登记为示例 target。

### 6.3 历史审计与覆盖缺口

- **历史审计结论已修复**：`docs/module-audit-2026-10-20.md:45` 记录的
  「WebSocket 无发送端分片（>16MiB 发不出）」已被 `fragment_outgoing` +
  `send_message()` 修复（`websocket.hpp:92-97`、`websocket.cpp:624-648`）；
  旧结论不再代表现状。
- 帧层负例（RSV、未知 opcode、反向掩码、非最小 126/127、控制帧分片/超长、
  非法 Close code/reason、非法 UTF-8、超限 1009）没有独立自动化用例；
  代码路径存在（`websocket.cpp:486-531`、`:691-742`），但缺少回归保护。
- 未覆盖 `io_timeout` 超时、`Cancel`/析构唤醒、并发多写者、`fragment_outgoing=false`。
- 测试全部基于 `socketpair(AF_UNIX)`，没有真实 TCP 服务端/浏览器互通测试。

---

## 7. 使用示例

```cpp
#include "go2cpp/network/http.hpp"
#include "go2cpp/network/websocket.hpp"

// 服务端：request 由 http::Parser 解析得到
go2cpp::WebSocketConfig config;
config.io_timeout = std::chrono::seconds(10);
config.max_frame_payload = 1 * 1024 * 1024;
config.max_message_payload = 4 * 1024 * 1024;

go2cpp::WebSocketResult result;
auto ws = go2cpp::WebSocket::AcceptWithPending(
    socket, request, parser.take_remaining(), config, &result);
if (!ws) return;  // result.message 是握手失败原因；socket 仍归调用方

go2cpp::WebSocketMessage message;
while (ws->read_message(message).ok()) {
    if (message.opcode == go2cpp::WebSocketOpcode::Close) break;
    if (message.text()) {
        if (!ws->send_text("echo: " + message.payload).ok()) break;
    } else if (!ws->send_binary(message.payload).ok()) {
        break;
    }
}
```

---

## 8. 相关文档

- `docs/modules/io.md`：`Socket` 等待在 Fiber 中如何 park/wake。
- `docs/state-machines/WebSocket.md`：握手、关闭、帧读取、分片四张状态表。
- `docs/websocket.md`：已合并的旧使用说明（顶部有标注）。
- `docs/modules/sse.md`：另一条基于 HTTP/1.1 的单向推送协议，与本文并列。
- `docs/network.md`：网络层总览与 HTTP Parser 的 Upgrade 说明。
