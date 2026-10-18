# Scanner 状态机

- 源文件：`include/go2cpp/bufio/scanner.hpp`（模板实现，无 .cpp；预置 `SplitFunc` 在 `src/bufio/bufio.cpp`）
- 关键函数：`Scan`、`ReadMore`、`Shutdown`、`SetErr`

## 状态

| 当前状态 | 触发（函数） | 下一状态 | 说明 |
|---|---|---|---|
| Idle | `Scan` | Token | 首轮即切出一个 token |
| Idle | `Scan` | Finished | 空输入：EOF 且窗口切不出 token |
| Idle | `Scan` | Failed | 首轮即遇 `SplitFunc` 错误或底层错误 |
| Idle | `Scan` | Token | `ErrFinalToken` 分支：交付最后一个 token 并 `Shutdown` |
| Token | `Scan` | Token | 后续 token，窗口继续推进 |
| Token | `Scan` | Finished | 流结束且无更多 token（`m_err` 清空） |
| Token | `Scan` | Failed | `ErrTooLong`/`ErrInvalidAdvance`/IO 错误 |
| Token | `Text`/`Bytes` | Token | 只读，不改变状态 |
| Scanning | `Scan` | Token | 内部循环中 `SplitFunc` 切出 token |
| Scanning | `Scan` | Finished | `atEOF` 且窗口再也切不出 token |
| Scanning | `Scan` | Failed | `ReadMore` 到达上限或底层错误 |
| Finished | `Scan` | Finished | `m_done == true`，直接返回 false |
| Failed | `Scan` | Failed | 直接返回 false，`Err()` 保留首个真实错误 |

> `Scanning` 是 `Scan()` 内部循环的中间状态，不可从外部观察；`Finished`/`Failed`
> 都表现为 `m_done == true`，区别只在 `m_err` 是否为空。

## 不变量

- `0 <= m_start <= m_end <= m_buffer.size()`；
- `m_max_token_size >= m_buffer.size() >= m_initial_buffer_size`；
- `m_done` 为真后 `m_token` 为空、`m_start == m_end == 0`；
- `m_done && !m_err` 表示正常 EOF，`m_done && m_err` 表示失败终止；
- `m_token` 非空时必定完全落在 `m_buffer` 内。

## 非法转化

- Finished/Failed -> Token：没有 `Reset()`，终止后只能重新构造 `Scanner`。
- Token -> Idle：不存在；`m_token` 只会被下一次 `Scan()` 清空并进入新一轮。
- 把 `SplitFunc` 的 `token` 指向窗口外：`Scan()` 判 `ErrInvalidAdvance` 并终止，不会越界读。
