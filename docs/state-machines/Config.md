# Config 状态机（配置热加载事务）

- 源文件：`src/config/config.cpp`（类定义在 `include/go2cpp/config/config.hpp`）
- 关键函数：`Config::LoadFromIni()`、`Config::StartWatcher()`、`ConfigVar::ApplyString()`、
  `ConfigVar::NotifyPending()`、`ConfigVar::RollbackPending()`

## 一、单变量事务（`ConfigVar<T>`）

| 当前状态 | 触发（函数） | 下一状态 | 说明 |
|---|---|---|---|
| Idle | `ApplyString()` | Pending | 值已原子写入 `m_value`，监听器尚未调用 |
| Idle | `ApplyString()`（值未变化） | Idle | 不产生 pending，直接成功返回 |
| Idle | `SetValue()` | Idle | 非事务路径：校验后立即发布并调用监听器 |
| Idle | `FromString()` | Idle | 解码 + 校验后转调 `SetValue()` |
| Pending | `ApplyString()` | Pending（拒绝） | 返回 false：上一事务尚未通知，禁止交错覆盖 |
| Pending | `SetValue()` | Pending（拒绝） | 报「配置变量正在等待上一事务通知」 |
| Pending | `NotifyPending()` | Idle | 先取监听器快照并清 pending，再在锁外逐个回调 |
| Pending | `RollbackPending()` | Idle | 原子恢复旧快照，不调用监听器 |

## 二、整批文件事务（`Config`）

| 当前状态 | 触发（函数） | 下一状态 | 说明 |
|---|---|---|---|
| Idle | `LoadFromIni()` | Validating | 取得 `m_transaction_mutex`，构造完整候选快照 |
| Validating | `ValidateString()` / 事务校验器失败 | Idle | 尚未写入任何变量，直接返回失败 |
| Validating | `ApplyString()` | Applying | 逐变量写入值并登记 pending |
| Applying | `ApplyString()` 失败 | RolledBack | 逆序对已写变量调用 `RollbackPending()` |
| Applying | 全部变量成功 | Pending | 所有变量已写入，等待统一广播 |
| Pending | `NotifyPending()` | Committed | 释放事务锁后广播；监听器看到完整新配置 |
| Pending | 提交阶段抛异常 | RolledBack | `catch (...)` 路径逆序回滚后重新抛出 |
| RolledBack | `RollbackPending()` | Idle | 每个变量恢复旧值，监听器从未被调用 |
| Committed | `LoadFromIni()` | Validating | 下一次文件事务 |
| Idle | `StartWatcher()` | Watching | 初始 `LoadFromFile()` 成功后才创建线程 |
| Watching | 文件变化后 `LoadFromFile()` 成功 | Watching | 更新基准时间戳并清除 retry_pending |
| Watching | 文件变化后 `LoadFromFile()` 失败 | Watching | 打印错误、置 retry_pending，继续轮询 |
| Watching | `StopWatcher()` / `stop_watcher_locked()` | Idle | 置 `m_watching=false`、唤醒并 join |

## 不变量

- 同一时刻最多一个线程持有 `m_transaction_mutex`；预检与写入不会被另一个事务交错。
- Applying 阶段任何失败都会回滚**全部**已写变量：不存在「只更新一半」的可见状态。
- `NotifyPending()` 之前，本次涉及的所有 `ConfigVar` 已经完整写入；监听器读不到中间值。
- 监听器与提交回调都在配置锁外执行，允许回调再次读/提交配置。
- 单个监听器/回调抛出异常只影响它自己，不撤销已提交的值。

## 非法转化

- Pending -> Applying：第二次 `ApplyString()` 被拒绝（返回 false），不会覆盖 pending。
- Applying -> NotifyPending：所有变量写入成功后才能进入 Pending，失败必须先回滚。
- RolledBack -> Committed：回滚后只能重新发起 `LoadFromIni()`，不能直接广播。
- 在监听线程内部调用 `StopWatcher()`：`stop_watcher_locked()` 检测到当前线程即 `detach()`，
  状态仍回到 Idle，但不会等待自身退出。
