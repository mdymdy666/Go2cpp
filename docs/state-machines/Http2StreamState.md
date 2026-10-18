# StreamStateMachine 状态机

- 源文件：`src/network/http2/stream_state.cpp`（类定义在 `include/go2cpp/network/http2/stream_state.hpp`）
- 关键函数：`SendHeaders()`、`SendData()`、`SendPushPromise()`、`SendRstStream()`、
  `ReceiveHeaders()`、`ReceiveData()`、`ReceivePushPromise()`、`ReceiveRstStream()`、
  `ForceClose()`，以及内部结算函数 `RefreshFromEndFlags()`
- 规范：RFC 9113 §5.1。本实现是**纯逻辑**组件：无 IO、无锁、单驱动者。

## 状态

| 状态 | 含义 |
|---|---|
| `idle` | 流号已分配但没有任何帧往来 |
| `reserved(local)` | 本端发过 PUSH_PROMISE，等待本端打开 |
| `reserved(remote)` | 对端发过 PUSH_PROMISE，等待对端打开 |
| `open` | 双向都可收发 |
| `half-closed(local)` | 本端已发 END_STREAM，只能收 |
| `half-closed(remote)` | 对端已发 END_STREAM，只能发 |
| `closed` | 终态，任何事件都返回 `kStreamClosed` |

> 重要实现细节：`open` / `half-closed(*)` / `closed` 不是直接赋值的，而是由
> `RefreshFromEndFlags()` 依据 `m_local_end`、`m_remote_end` 两个布尔量**重算**。
> 因此转化表里凡是从 `open`/`half-closed` 出发的行，实际落点由"哪一侧置了
> END_STREAM"决定；表中按实际结果逐行列出。

## 转化表

| 当前状态 | 触发（函数） | 下一状态 | 说明 |
|---|---|---|---|
| idle | `SendHeaders(false)` | open | 客户端发首个 HEADERS，两个方向都未结束 |
| idle | `SendHeaders(true)` | half-closed(local) | HEADERS 带 END_STREAM（无 body 的请求） |
| idle | `ReceiveHeaders(false)` | open | 服务端收到首个 HEADERS |
| idle | `ReceiveHeaders(true)` | half-closed(remote) | 对端在 HEADERS 上就结束发送方向 |
| idle | `SendPushPromise()` | reserved(local) | 只有 idle 能发 PUSH_PROMISE |
| idle | `ReceivePushPromise()` | reserved(remote) | 只有 idle 能收 PUSH_PROMISE |
| idle | `SendRstStream()` | idle（拒绝，返回 `kInvalidTransition`） | RFC 9113 §5.1：idle 上除 HEADERS/PRIORITY 外都是协议错误 |
| idle | `ReceiveRstStream()` | idle（拒绝，返回 `kInvalidTransition`） | 同上 |
| reserved(local) | `SendHeaders(false)` | half-closed(remote) | promised stream 被打开；对端只收不发 |
| reserved(local) | `SendHeaders(true)` | closed | 两侧 END_STREAM 同时成立 |
| reserved(local) | `ReceiveHeaders(bool)` | reserved(local)（拒绝） | 本端发起的 promised stream 不能收到 HEADERS |
| reserved(remote) | `ReceiveHeaders(false)` | half-closed(local) | 对端打开 promised stream；本端只收 |
| reserved(remote) | `ReceiveHeaders(true)` | closed | 对端在 HEADERS 上就结束，本端也已关闭 |
| reserved(remote) | `SendHeaders(bool)` | reserved(remote)（拒绝） | 本端不能向对端发起的 promised stream 发 HEADERS |
| open | `SendHeaders(false)` | open | trailers（不带 END_STREAM）不改变状态 |
| open | `SendHeaders(true)` | half-closed(local) | 带 END_STREAM 的 trailers 或重复 HEADERS |
| open | `SendData(false)` | open | 普通 DATA |
| open | `SendData(true)` | half-closed(local) | 最后一个 DATA |
| open | `ReceiveHeaders(false)` | open | 对端 trailers |
| open | `ReceiveHeaders(true)` | half-closed(remote) | 对端在 trailers 上结束 |
| open | `ReceiveData(false)` | open | 普通 DATA |
| open | `ReceiveData(true)` | half-closed(remote) | 对端最后一个 DATA |
| open | `SendRstStream()` | closed | 任何非 idle/closed 状态都直接关闭 |
| open | `ReceiveRstStream()` | closed | 同上 |
| half-closed(local) | `ReceiveHeaders(false)` | half-closed(local) | 本端已关，仍可收 trailers |
| half-closed(local) | `ReceiveHeaders(true)` | closed | 对端也结束 |
| half-closed(local) | `ReceiveData(false)` | half-closed(local) | 仍可收 body |
| half-closed(local) | `ReceiveData(true)` | closed | 对端结束 |
| half-closed(local) | `SendHeaders(bool)` | half-closed(local)（拒绝） | 本端发送方向已关，不能再发 |
| half-closed(local) | `SendData(bool)` | half-closed(local)（拒绝） | 同上（`CanSendData()` 为 false） |
| half-closed(remote) | `SendHeaders(false)` | half-closed(remote) | 本端仍可发 trailers |
| half-closed(remote) | `SendHeaders(true)` | closed | 本端也结束 |
| half-closed(remote) | `SendData(false)` | half-closed(remote) | 仍可发 body |
| half-closed(remote) | `SendData(true)` | closed | 本端结束 |
| half-closed(remote) | `ReceiveHeaders(bool)` | half-closed(remote)（拒绝） | 对端发送方向已关，不能再收 |
| half-closed(remote) | `ReceiveData(bool)` | half-closed(remote)（拒绝） | 同上 |
| closed | 任一 `Send*`/`Receive*` | closed（返回 `kStreamClosed`） | 终态；上层据此决定忽略还是报 STREAM_CLOSED |
| 任意非 idle 状态 | `ForceClose()` | closed | 本地强制关闭（写帧失败/额度提交失败），不发 RST_STREAM |

## 不变量

- `m_state == kClosed` 时所有事件方法都返回 `kStreamClosed`，且不再改写任何字段；
- `m_local_end && m_remote_end` 等价于 `kClosed`；只有一侧为真时必然是
  `half-closed(local)` 或 `half-closed(remote)`，两者都假时必然是 `open`
  （例外：`reserved(*)` 不参与该等价关系，它由 `SendPushPromise`/`ReceivePushPromise` 直接赋值）；
- `kReservedLocal` 只在 `SendPushPromise()` 之后出现，`kReservedRemote` 只在
  `ReceivePushPromise()` 之后出现；
- 方法不分配内存、不加锁，转换失败时**不修改**任何状态（预检语义），
  因此调用方可以先 `CanSendData()` 再 `SendData()` 而不会出现"校验吃掉了 END_STREAM"。

## 非法转化

- `idle -> closed`（经 RST_STREAM）：返回 `kInvalidTransition`，不改状态；
- `half-closed(local) -> 发送`、`half-closed(remote) -> 接收`：返回
  `kInvalidTransition`；上层 `Connection` 把它翻译成 `kProtocolError`；
- `reserved(local) -> 接收 HEADERS`、`reserved(remote) -> 发送 HEADERS`：返回
  `kInvalidTransition`；
- 所有非法转化都不抛异常、不 assert —— 设计上要求调用方检查返回值，把
  "是否断开连接"的决定留在连接层。
