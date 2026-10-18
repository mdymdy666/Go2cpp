# panic 状态机

- 源文件：`src/tools/control_flow.cpp`（类定义在 `include/go2cpp/tools/control_flow.hpp`）
- 关键函数：`panic::call`、`panic::call_info`、`panic::clear`、`recover::take`、`recover::operator()`、`recover::peek`

## 状态

| 当前状态 | 触发（函数） | 下一状态 | 说明 |
|---|---|---|---|
| Idle | `panic::call` | Active | 发布 info，`active=true`、`recovered=false` |
| Idle | `panic::call_info` | Active | 同上，所有 `call` 重载的汇合点 |
| Active | `panic::call` | Active | 覆盖之前的 info，重新进入 Active |
| Active | `recover::take` | Recovered | 取走 info，`active=false`、`recovered=true` |
| Active | `recover::operator()` | Recovered | 丢弃 info/payload，`active=false`、`recovered=true` |
| Active | `recover::peek` | Active | 只读快照，不改变状态 |
| Active | `panic::clear` | Idle | 清空 info/payload，`active=false`、`recovered=false` |
| Recovered | `panic::clear` | Idle | 复位到未触发 |
| Recovered | `panic::call` | Active | 重新触发 |
| Recovered | `recover::take` | Recovered | 返回 `nullopt`（`active=false`） |
| Idle | `recover::take` | Idle | 返回 `nullopt`（没有活动 panic） |
| Idle | `recover::operator()` | Idle | 返回 false |
| 任意 | `recover::peek`/`panic::info` | 不变 | 持锁读取快照 |

> 状态存储在 `detail::PanicState` 中：`active` 与 `recovered` 两个布尔量的
> 组合决定状态。`panic` 拷贝共享同一状态，`recover` 只持有 `weak_ptr`。

## 不变量

- `active == info.raised`；
- `recovered` 为真时 `active` 必为假；
- 所有对 `info`/`active`/`recovered` 的读写都持有 `PanicState::mutex`；
- `take()` 成功时一定同时完成「取走 info + 置 recovered」。

## 非法转化

- Recovered -> Active 只能通过 `panic::call`；直接改标志不被允许（成员私有）。
- 在 defer 回调之外调用 `recover::take`/`operator()`：直接失败，不改变状态。
- `recover` 绑定的 `panic` 已销毁：`weak_ptr` 升级失败，所有操作按「无状态」处理。
