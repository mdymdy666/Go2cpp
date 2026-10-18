# EventBatch 状态机

- 源文件：`include/go2cpp/tools/event.hpp`（header-only，无 .cpp）
- 关键函数：`bind`、`add_event`、`clear`、`work`、`stop`、`reset`
- 外部依赖：`Channel::Select`、`sync::Mutex`、`Context`

## 状态

| 当前状态 | 触发（函数） | 下一状态 | 说明 |
|---|---|---|---|
| Unbound | `bind` | Runnable | 清空旧事件并装载一个 case |
| Unbound | `add_event` | Runnable | 追加至少一个 case |
| Runnable | `add_event` | Runnable | 追加 case，不影响停止位 |
| Runnable | `clear` | Unbound | 清空事件、结果、停止位与轮数 |
| Runnable | `work` | Waiting | 取得 `m_work_mutex`，准备调用 `Select` |
| Waiting | `work` | Runnable | Select 返回且未触发停止条件 |
| Waiting | `work` | Stopped | 超时且 `stop_on_timeout=true`，或轮数/总时长到限 |
| Waiting | `work` | Stopped | Select 异常被转成 kInvalid 结果（批次继续，除非后续 stop） |
| Runnable/Unbound | `stop` | Stopped | 置 `m_stopped` 并 cancel 停止 Context，唤醒等待者 |
| Stopped | `reset` | Runnable/Unbound | 清停止位/轮数/结果，重建停止 Context |
| Stopped | `clear` | Unbound | 清空事件并复位 |
| Stopped | `work` | Stopped | 直接返回 `stopped_result`，不调用 Select |
| 任意 | `handle`/`last_result_copy`/`size`/`runnable` | 不变 | 只读快照 |

> `Waiting` 表示 `work()` 已持有 `m_work_mutex` 且正在 `Select`；它不可从
> 外部直接观察。`Stopped` 的触发原因可能是 `stop()`、每轮超时、`m_max_rounds`
> 或 `m_max_duration`，统一表现为 `m_stopped == true`。

## 不变量

- `m_entries` 与 `m_last` 的所有读写都持 `m_mutex`；
- `m_rounds` 只在成功执行一轮 `work()` 后递增；
- `m_stopped` 为真后 `work()` 不再调用 `Select`；
- `m_has_result` 为真时 `m_last` 是最近一轮结果的副本；
- `m_work_mutex` 保证同一时刻最多一个 `work()`；
- `m_max_rounds == 0` / `m_max_duration <= 0` 表示对应条件不限制。

## 非法转化

- Stopped -> Waiting：没有 `reset()`/`clear()` 时不会重新进入执行态。
- 同一批并发调用 `work()`：第二个调用会阻塞在 `m_work_mutex`，不会并行执行 Select。
- `Lock()` 失败时写方法（`add_event`/`clear`/`stop` 等）静默返回，状态不变。
