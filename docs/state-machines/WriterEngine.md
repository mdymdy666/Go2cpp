# WriterEngine 状态机

- 源文件：`include/go2cpp/compress/detail/writer_engine.hpp`（模板，实现细节在 `src/compress/deflater.cpp`）
- 关键函数：`Write`、`Flush`、`Close`、`Drain`、`Reset`
- 状态载体：`m_error`（粘性错误）、`m_closed`（是否已结束）、`m_deflater`

## 状态

| 当前状态 | 触发（函数） | 下一状态 | 说明 |
|---|---|---|---|
| Open | `Write` | Writing | 进入压缩泵，按 `kMaxChunkSize` 切片 |
| Writing | `Write` | Open | 本片输入被 zlib 消费完且 `Drain` 成功 |
| Writing | `Write` | Failed | sink 写失败，或 zlib 无进展（`ErrInternal`） |
| Open | `Flush` | Flushing | 进入 `Z_SYNC_FLUSH` 泵 |
| Flushing | `Flush` | Open | 产出全部交给 sink |
| Flushing | `Flush` | Failed | sink 写失败 |
| Open/Writing | `Close` | Finishing | 进入 `Z_FINISH` 循环 |
| Finishing | `Close` | Closed | 收到 `kStreamEnd` 且最后一批产出已 drain |
| Finishing | `Close` | Failed | sink 写失败或 `kError` |
| Closed | `Write`/`Flush` | Failed | 置 `ErrClosed()` 为粘性错误并返回 0 |
| Closed | `Close` | Closed | 幂等成功 |
| Failed | 任意 | Failed | 直接返回粘性错误，不再碰 zlib |
| Failed/Closed | `Reset` | Open | 清错误/closed 并复位或重建 Deflater |
| Open | `Reset` | Open | 复用 Deflater 并清空状态 |

> `Writing` / `Flushing` / `Finishing` 是三个泵函数的瞬时状态，不可从外部
> 直接观察；对外看到的是 `Err()` 与 `Closed()`。`Close` 失败时同时置
> `m_error` 与 `m_closed`，因此 `Failed` 优先于 `Closed`。

## 不变量

- `m_error` 非空后所有入口先返回它，不再推进 zlib；
- `m_closed == true` 后 `Write`/`Flush` 返回 `ErrClosed()`；
- `m_deflater` 只在构造失败时为 null（此时 `m_error` 非空）；
- `Drain()` 成功返回后，`output_size()` 那批字节已全部交给 sink；
- 已写出的字节在失败时不回退。

## 非法转化

- Closed -> Writing：`Reset` 之外不存在；`Write` 会置 `ErrClosed`。
- Failed -> Open：只能通过 `Reset`；没有隐式恢复。
- 构造参数非法（level 越界）：直接进入 Failed，不会创建 Deflater。
