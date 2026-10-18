# BufferedSink 状态机（日志批量缓冲）

- 源文件：`src/logging/log.cpp`（类定义在 `include/go2cpp/logging/log.hpp`）
- 关键函数：`BufferedSink::Write()`、`BufferedSink::ShouldFlushLocked()`、
  `BufferedSink::FlushBatch()`、`BufferedSink::Flush()`、`BufferedSink::Reopen()`

## 状态

| 状态 | 含义 |
|---|---|
| Empty | `m_buffer` 为空，底层 Sink 没有待写批次 |
| Buffering | 缓冲区非空但未达到刷新条件 |
| Flushing | 已把缓冲区 `swap` 到局部变量，正在（或即将）在锁外写底层 |
| Bypassing | 单条记录达到 `bypass_threshold`，跳过缓冲直接写底层 |
| Immediate | 策略为 `kImmediate`，每条都直接写底层 |

## 转化表

| 当前状态 | 触发（函数） | 下一状态 | 说明 |
|---|---|---|---|
| Empty | `Write()`（普通记录） | Buffering | 追加文本并保证以 `\n` 结尾 |
| Buffering | `Write()`（未达阈值） | Buffering | 继续累积；`kByTime` 只在 Write/Flush 时惰性检查 |
| Buffering | `Write()`（达到容量/时间） | Flushing | 锁内 `swap` 出批次并更新 `m_last_flush` |
| Flushing | `FlushBatch()` | Empty | 锁外串行写底层、`Flush`、`m_flush_count += 1` |
| Buffering | `Flush()` | Flushing | 显式刷新，空缓冲也会转发底层 `Flush()` |
| Empty | `Flush()` | Empty | 无数据可写，只调用 `m_inner->Flush()` |
| Empty / Buffering | `Write()`（`>= bypass_threshold`） | Empty / Buffering | 绕过缓冲：直接写并 Flush，不碰 `m_buffer` |
| Empty / Buffering | `Write()`（`kImmediate`） | Empty / Buffering | 不缓冲，直接写并 Flush |
| Empty / Buffering / Flushing | `Reopen()` | Empty | 先 `Flush()`，再在 `m_io_mutex` 内转发 `Reopen()` |

## 不变量

- 持 `m_mutex` 期间**绝不**调用底层 Sink；只做 `append`/`swap`/时间比较。
- `m_mutex` 与 `m_io_mutex` 不嵌套：先释放前者，再获取后者，因此不存在锁序死锁。
- 缓冲区内容总是以 `\n` 结尾，批量文本可按行解析。
- 一条记录要么完整落在某一批里，要么被单独写出，不会被拆成两半。
- `m_flush_count` 只增不减；`kByTime` 不会在没有 Write/Flush 时自动触发。

## 非法转化

- Buffering -> Flushing 时仍持有 `m_mutex`：代码通过 `swap` + 作用域确保锁已释放，禁止在锁内调 `m_inner`。
- `WriteRaw()` 返回 false：不改变状态，退化为 `m_inner->Write(last_record, batch)`。
- 底层 `m_inner` 为空：`Write`/`Flush`/`Reopen` 直接返回，状态保持不变（不会崩溃）。
