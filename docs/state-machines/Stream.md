# Stream（SSE 服务端流）状态机

> **配套标注（2026-10-26）**：本文是 [docs/modules/sse.md](../modules/sse.md) 的配套状态机明细；API 与边界以模块文档为准。
> 注意：表中“Context 取消回调 -> Failed”是简化说法；仅取消、尚未写失败时 `state()` 返回 `Closed`，详见模块文档 §4.4。

- 源文件：`src/network/sse.cpp`（类定义在 `include/go2cpp/network/sse.hpp`）
- 关键函数：`start`、`send`、`comment`、`tick`、`close`、`state`
- 状态载体：`http::ResponseWriter::state()`、`m_failed`、Context 取消标记

## 状态

| 当前状态 | 触发（函数） | 下一状态 | 说明 |
|---|---|---|---|
| Idle | `start` | Open | 写 200 + `text/event-stream` 响应头成功 |
| Idle | `start` | Failed | 响应头写入失败（socket 无效时不置 Failed，可重试） |
| Idle | `send`/`comment`/`tick`/`close` | Idle | 返回 `Error::Closed`，状态不变 |
| Open | `send` | Open | 事件编码并写成功 |
| Open | `comment` | Open | 注释/心跳写成功 |
| Open | `tick` | Open | 心跳未到期时不写字节；到期则写注释 |
| Open | `send`/`comment` | Failed | 写超时/短写/IO 错误 |
| Open | `close` | Closed | 终止块写成功 |
| Open | `close` | Failed | 终止块写失败 |
| Open | Context 取消回调 | Failed | 取消标记置真并 `cancel_write` 唤醒写等待 |
| Closed | `close` | Closed | 幂等成功 |
| Closed | `send`/`comment`/`tick` | Closed | 返回 `Error::Closed` |
| Failed | 任意 | Failed | 粘性失败，不再写字节 |

> `state()` 由 `m_failed`、Context 取消标记与 `m_writer.state()` 共同决定；
> `active()` 等价于 `state() == Open`。

## 不变量

- 状态只向前迁移：Idle -> Open -> Closed/Failed，失败后不复用连接；
- 只有 Open 允许 `send()`/`comment()`/`tick()`，其余状态返回 `Error::Closed`；
- 所有写路径持 `sync::Mutex`，`state()` 也加锁读；
- 心跳时间点 `m_next_heartbeat` 只在成功写入后刷新；
- `close()` 幂等；注销 Context 回调必须在锁外完成。

## 非法转化

- Closed/Failed -> Open：不存在，只能重建 `Stream`。
- Idle -> Closed：`close()` 在未 start 时返回 `Error::Closed`，但不改变状态。
- 多生产者并发 `send()`：字节不会交错，但事件顺序由调用方负责。
