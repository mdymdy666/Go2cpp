# Reader 状态机

- 源文件：`include/go2cpp/bufio/reader.hpp`（模板实现，无 .cpp）
- 关键函数：`Read`、`ReadByte`、`ReadRune`、`ReadLine`、`ReadString`、`ReadSlice`、`Peek`、`Discard`、`fill`、`UnreadByte`、`UnreadRune`

## 缓冲状态

| 当前状态 | 触发（函数） | 下一状态 | 说明 |
|---|---|---|---|
| Empty | `Read`/`ReadByte`/`ReadRune`/`ReadLine`/`ReadString`/`ReadSlice`/`Peek`/`Discard` | Buffered | 经 `fill()` 取到至少 1 字节 |
| Empty | `Read`/`ReadByte`/`ReadRune`/`ReadLine`/`ReadString`/`ReadSlice`/`Peek`/`Discard` | Terminated | 底层 EOF/错误且没有数据可交付 |
| Buffered | `Read`/`ReadByte`/`ReadRune`/`ReadLine`/`ReadString`/`ReadSlice`/`Peek`/`Discard` | Buffered | 只消费了一部分 |
| Buffered | 同上 | Empty | 恰好消费完缓冲 |
| Buffered | `fill` | Draining | 本次读到数据且底层同时带错误（数据先入缓冲，错误延后） |
| Draining | 任意读操作 | Draining | 仍在交付缓冲里剩余的数据 |
| Draining | 任意读操作 | Terminated | 数据耗尽，粘性错误浮出 |
| Terminated | 任意读操作 | Terminated | 直接返回 `m_err`，不再调用底层 |
| Empty | `Read`（快路径：`p.size() >= Size()`） | Empty | 直读调用方缓冲，不改变内部游标 |
| Empty | `Read`（快路径） | Terminated | 直读即遇错误，置 `m_err` |

## Unread 子状态

| 当前状态 | 触发（函数） | 下一状态 | 说明 |
|---|---|---|---|
| None | `ReadByte` | Byte | `m_can_unread_byte = true` |
| Byte | `UnreadByte` | None | 回退 `m_r` 并清标记 |
| None | `ReadRune` | Rune | 记录 `m_last_rune_size` |
| Byte | `ReadRune` | Rune | 先 `InvalidateUnread()`，再记录新长度 |
| Rune | `UnreadRune` | None | 回退 `m_last_rune_size` 字节 |
| Byte/Rune | `Read`/`ReadLine`/`ReadString`/`ReadSlice`/`Peek`/`Discard` | None | `InvalidateUnread()` 让上一次操作不可撤销 |
| None/Byte/Rune | `UnreadByte`（非 Byte 态） | 不变 | 返回 `ErrInvalidUnreadByte()` |
| None/Byte | `UnreadRune`（非 Rune 态） | 不变 | 返回 `ErrInvalidUnreadRune()` |

## 不变量

- `0 <= m_r <= m_w <= m_buffer.size()`；
- `Buffered() == m_w - m_r`；
- `m_err` 非空后 `fill()` 不再调用底层，所有读操作最终以该错误收尾；
- `m_can_unread_byte` 为真时 `m_r > 0`；
- `m_last_rune_size > 0` 时 `m_r >= m_last_rune_size`。

## 非法转化

- Terminated -> Buffered：没有 `Reset()`，只能重新构造 Reader；任何读操作都直接返回粘性错误。
- Rune -> Byte：不存在该转化；`UnreadRune()` 之后回到 None，再 `ReadByte` 才进入 Byte。
- 同一个 Reader 跨线程并发调用：未定义行为（内部无锁，`m_r`/`m_w` 会撕裂）。
