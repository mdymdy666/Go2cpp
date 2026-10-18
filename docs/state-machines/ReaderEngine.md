# ReaderEngine 状态机

- 源文件：`include/go2cpp/compress/detail/reader_engine.hpp`（模板，实现细节在 `src/compress/inflater.cpp`）
- 关键函数：`Read`、`Refill`、`FinishOnSourceEof`、`Reset`
- 状态载体：`m_stream_end`、`m_error`、`m_source_eof`、`m_deferred_error`、`Inflater::input_available()`

## 状态

| 当前状态 | 触发（函数） | 下一状态 | 说明 |
|---|---|---|---|
| NeedInput | `Read` | Decoding | `Refill` 从源取到至少 1 字节并 `CommitInput` |
| NeedInput | `Read` | Eof | `Refill` 在成员结束边界遇到源 EOF |
| NeedInput | `Read` | Failed | `Refill` 遇到截断/IO 错误，或连续空读超限 |
| Decoding | `Read` | Decoding | `Inflater::Inflate` 产出数据且未结束 |
| Decoding | `Read` | NeedInput | 输入被消费完，循环回到 `Refill` |
| Decoding | `Read` | Eof | `Inflate` 返回 `kMemberEnd` 且非多成员模式 |
| Decoding | `Read` | Decoding | 多成员模式且缓冲里还有下一个成员，`ResetForNextMember` 后继续 |
| Decoding | `Read` | Failed | `kError`，或 `kOk` 但既不消费输入也不产出（zlib 卡死） |
| Eof | `Read` | Eof | 直接返回 0 并把 `streams::IoEof()` 留给本次调用 |
| Failed | `Read` | Failed | 粘性错误，直接返回 0，不再调用源或 zlib |
| Eof/Failed | `Reset` | NeedInput | 清空结束标记/错误/延迟错误并复位 inflater |

> `Decoding` 是 `Read` 内部循环的瞬时状态；对外可观察的是「本次返回了多少
> 字节 + `Err()` 的值」。`m_deferred_error` 非空表示「数据已交付，错误下次报」，
> 此时仍在 `Decoding`，直到缓冲耗尽才进入 `Failed`。

## 不变量

- `m_error` 非空即粘性：`Read` 返回 0，`Refill` 返回 false；
- `m_stream_end == true` 后不再向源读数据；
- `m_inflater.input_available() > 0` 时不会调用 `Refill`；
- 多成员模式只在 `kMemberEnd` 时 `ResetForNextMember`，不会跨成员保留 zlib 状态。

## 非法转化

- Eof -> Decoding：没有 `Reset` 时不会发生；`Read` 始终返回 0 + `IoEof`。
- Failed -> NeedInput：只能通过 `Reset(ReaderT&)` 重新绑定源。
- 单成员模式在 `kMemberEnd` 之后继续 `Read`：直接 Eof，不会尝试解析尾部垃圾。
