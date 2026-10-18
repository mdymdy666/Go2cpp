# http2::Connection 连接层状态机

- 源文件：`src/network/http2/connection.cpp`（类定义在 `include/go2cpp/network/http2/connection.hpp`）
- 关键函数：`Handshake()`、`FinishSettingsHandshake()`、`ReadEvent()`、`WriteFrame()`、
  `SendGoAway()`、`StartRequest()`、`WriteData()`
- 本 md 覆盖两层状态：**握手进度**（`m_handshaked` + `m_peer_settings_received` /
  `m_local_settings_sent`）与**连接生命周期**（`ConnState`）。流级状态见
  `Http2StreamState.md`。

## 状态

| 状态 | 含义 |
|---|---|
| 未握手 | 对象已构造，preface / SETTINGS 尚未交换完 |
| 已握手 | `m_handshaked == true`，可收发请求/响应 |
| `kOpen` | 连接可正常使用，**唯一**允许新建流的局面 |
| `kDraining` | 已发或已收 GOAWAY：不再接受新流，已有流可继续收尾 |
| `kClosed` | 传输失败：连接已不可用，任何操作快速失败 |

## 握手转化表

| 当前状态 | 触发（函数） | 下一状态 | 说明 |
|---|---|---|---|
| 未握手 | `Handshake()`（客户端） | 未握手（已发 preface） | 先写 24 字节 preface，再写本地 SETTINGS |
| 未握手 | `Handshake()`（服务端） | 未握手（已校验 preface） | 读 preface 并比对，失败返回 `kProtocolError` |
| 未握手（已发 SETTINGS） | `FinishSettingsHandshake()` | 已握手 | 收到对端非 ACK SETTINGS：应用参数并回 ACK |
| 未握手（已发 SETTINGS） | `FinishSettingsHandshake()` | 未握手 | 收到 SETTINGS ACK 或 PING：继续循环等待 |
| 已握手 | `Handshake()` | 已握手 | 直接返回 `kOk`（幂等） |
| `kClosed` | `Handshake()` | `kClosed` | 返回 `kClosed`，错误信息"连接已关闭，无法握手" |

## 连接生命周期转化表

| 当前状态 | 触发（函数） | 下一状态 | 说明 |
|---|---|---|---|
| `kOpen` | `WriteFrame()` | `kClosed` | 任何帧写失败（含 preface 写失败、Transport 为空） |
| `kOpen` | `SendGoAway()` | `kDraining` | GOAWAY 写成功后进入 draining |
| `kOpen` | `ReadEvent()`（收到 GOAWAY 帧） | `kDraining` | 记录 `m_peer_last_stream_id`，不再接受新流 |
| `kOpen` | `ReadEvent()`（传输 EOF/读失败） | `kOpen`（不变） | 返回 `kClosed` 但**不迁移状态**；上层应自行停止使用该连接 |
| `kDraining` | `StartRequest()` | `kDraining` | 拒绝新流，返回 `kProtocolError` |
| `kDraining` | `SendGoAway()` | `kDraining` | 重复发 GOAWAY 仍可写帧，状态不变 |
| `kDraining` | `WriteData()` / `SendResponseHeaders()` / `ResetStream()` | `kDraining` | 已有流继续收尾 |
| `kDraining` | `ReadEvent()`（再次收到 GOAWAY） | `kDraining` | 更新 `m_peer_last_stream_id`，幂等 |
| `kDraining` | `WriteFrame()` | `kClosed` | 传输失败仍是终态 |
| `kClosed` | 任一公开方法 | `kClosed` | 快速失败，返回 `kClosed`（`StartRequest` 返回 `kProtocolError`） |

## 不变量

- 只有 `state() == kOpen` 且 `handshaked()` 为真时 `StartRequest()` 才可能成功；
- `kDraining` 只禁止**新建**流，不禁止在已有流上读写；这一点让"发 GOAWAY 后
  把进行中的响应写完"成为合法操作；
- `kClosed` 是终态：没有任何路径把它改回 `kOpen` 或 `kDraining`；
- 握手期间 `m_read_buffer` 中已消费的字节必须被 erase 掉，否则 `TakeFrame()`
  会把同一个帧解析两次（`kNeedMore` 与 `consumed` 的语义依赖于此）；
- 单个驱动者：所有状态迁移都发生在调用者线程/Fiber 上，没有锁，也没有原子量。

## 非法转化

- `kClosed -> kOpen/kDraining`：不存在这样的路径。想要重连只能新建 `Connection`
  对象（HPACK 动态表、流表都不复用）；
- `kDraining -> kOpen`：不存在。收到对端 GOAWAY 后即使对端"反悔"也不会恢复；
- `kOpen` 下调用 `StartRequest()` 但握手未完成：返回 `kProtocolError` 且**不**
  消耗流号（`m_next_stream_id` 只在真正分配时才加 2）。
