# net::WebSocket 状态机

> **配套标注（2026-10-26）**：本文是 [docs/modules/ws.md](../modules/ws.md) 的配套状态机明细；API 与边界以模块文档为准，状态表仍按当前 `src/network/websocket.cpp` 维护。

- 源文件：`src/network/websocket.cpp`（类定义在 `include/go2cpp/network/websocket.hpp`）
- 关键函数：`AcceptWithPending()`、`Connect()`、`read_message()`、`read_frame()`、
  `close()`、`protocol_error()`、`abort_transport()`
- 本 md 覆盖三块：**握手**（连接对象创建）、**关闭流程**（`m_close_sent` /
  `m_closed`）、**帧读取阶段**与**分片消息重组**（`m_fragmented` /
  `m_fragment_opcode`）。

## 握手转化表

| 当前状态 | 触发（函数） | 下一状态 | 说明 |
|---|---|---|---|
| 不存在对象 | `AcceptWithPending()` | Open（服务端角色） | 校验 Upgrade 请求、写 101 响应、接管 pending 字节 |
| 不存在对象 | `Accept()` | Open（服务端角色） | 等价于 `AcceptWithPending(pending={})` |
| 不存在对象 | `Connect()` | Open（客户端角色） | 写 Upgrade 请求、逐字节读到 `\r\n\r\n`、校验 101 与 `Sec-WebSocket-Accept` |
| 不存在对象 | `AcceptWithPending()` / `Connect()`（校验失败） | 不创建对象 | 返回空 `Ptr`，**不**关闭调用方持有的 Socket |
| 不存在对象 | `BuildUpgradeResponse()`（请求非法） | 不创建对象 | 返回 400 响应并置 `close = true` |

## 关闭流程转化表

| 当前状态 | 触发（函数） | 下一状态 | 说明 |
|---|---|---|---|
| Open | `close()` | Closing（`m_close_sent = true`） | 发 Close 帧、置 `m_closed`、`abort_transport()` 唤醒阻塞读写 |
| Open | `read_message()`（收到 Close） | Closing | 若未发过 Close 则回一个同样的 Close，再置 `m_closed` |
| Open | `protocol_error()` | Closing | 先 `close(code)` 再返回 `WebSocketError::Protocol` |
| Open | 写帧失败（`write_frame_unlocked()`） | Closed | `abort_transport()` + `m_closed = true` |
| Open | `read_frame()` 读失败 | Open（不变） | 只返回错误；`read_message()` 的调用方决定是否 `close()` |
| Closing | `close()` | Closing（幂等成功） | `m_close_sent` 已置位，直接返回 `WebSocketResult::success()` |
| Closing | `send_text()` / `send_binary()` / `ping()` / `pong()` | Closed（拒绝） | `write_frame_unlocked()` 检查 `m_closed` 后返回 `Closed` |
| 任意 | `~WebSocket()` / `abort_transport()` | Closed | 取消底层读写等待，不直接 close fd（Socket 所有权可能与他人共享） |

## 帧读取阶段表（`read_frame()` 内部）

| 当前阶段 | 触发（函数） | 下一阶段 | 说明 |
|---|---|---|---|
| 期望基础头（2 字节） | `read_frame()` | 期望扩展长度 | 解析 FIN/RSV/opcode/MASK/长度；RSV 非 0 即 `protocol_error()` |
| 期望扩展长度 | `read_frame()` | 期望掩码键 | len==126 读 2 字节、len==127 读 8 字节；非最小编码即协议错误 |
| 期望扩展长度 | `read_frame()` | 错误 | 控制帧 `!fin` 或 `>125`、payload 超过 `max_frame_payload` -> `protocol_error()` |
| 期望掩码键 | `read_frame()` | 期望载荷 | `masked` 时读 4 字节掩码键 |
| 期望载荷 | `read_frame()` | 帧就绪 | 读满 `length` 字节并按掩码逐字节 XOR |
| 期望载荷 | `read_frame()` | 错误 | 读失败返回 Io/Timeout/Closed；分配失败置 `m_closed` 并返回 `MessageTooLarge` |

## 分片消息状态表（`read_message()`）

| 当前状态 | 触发（函数） | 下一状态 | 说明 |
|---|---|---|---|
| Idle（未分片） | `read_message()` | Fragmenting | 收到非 FIN 的 Text/Binary：记录 `m_fragment_opcode` 与首片 |
| Fragmenting | `read_message()` | Fragmenting | 收到 Continuation 且 `fin == false`，继续追加 |
| Fragmenting | `read_message()` | Idle | 收到 Continuation 且 `fin == true`，拼装完成并清空分片缓冲 |
| Fragmenting | `read_message()` | 错误 | 分片期间再收到 Text/Binary -> `protocol_error()` |
| Idle | `read_message()` | Idle | 收到 Ping -> 自动 Pong 后继续读；收到 Pong -> 直接继续读 |
| 任意 | `read_message()` | 错误 | 累计长度超过 `max_message_payload` -> `protocol_error(1009)` |
| 任意 | `read_message()` | 错误 | 文本消息 UTF-8 非法（且 `validate_utf8`）-> `protocol_error(1007)` |

## 不变量

- `m_closed == true` 后 `write_frame_unlocked()` 一律返回 `WebSocketError::Closed`，
  且不会再有字节写到线上；
- `m_close_sent` 为真时 `close()` 幂等成功，不会重复发 Close 帧；
- 客户端发出的帧必须带掩码、服务端发出的帧必须不带掩码；`read_frame()` 对
  反向掩码直接判协议错误（RFC 6455 §5.1）；
- 同一时刻最多一条分片消息在组装：`m_fragmented` 为真时拒绝新的 Text/Binary；
- 读锁 `m_read_mutex` 串行化 `read_message()`，写锁 `m_write_mutex` 串行化
  所有发送；两把锁都是 `sync::Mutex`（Fiber/线程混合），因为持锁期间可能 park。

## 非法转化

- `Closing -> Open`：不存在。WebSocket 协议不允许"关闭后继续复用"；
- Fragmenting 状态下收到 Text/Binary：直接 `protocol_error()` 并关闭连接；
- 收到 `payload.size() == 1` 的 Close 帧：`protocol_error()`（关闭码必须 2 字节）；
- 收到未知 opcode 或 RSV 位非 0：`protocol_error()`。
