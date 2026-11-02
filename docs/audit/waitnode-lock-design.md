# WaitNode 内部锁设计：为什么必须是 FutexMutex（std::mutex 的问题）

> 日期：2026-10-28。背景：WaitNode（`src/concurrency/sync.cpp:300`）内部用
> `core::FutexMutex m_wake_mutex` 保护节点字段。本文记录「若改用 std::mutex」的问题，
> 并在**合并 JinShanProject 代码时**作为检查项处理。

## 1. 问题清单（std::mutex 替代 FutexMutex）

### 1.1 跨线程解锁是 UB（最严重）

- `WaitNode::Wake()` 可由**任意线程**调用（取消回调、超时 timer、通知线程、PeerHub 回调）；
- `std::mutex` 要求**同一线程**加锁/解锁（C++ 标准 [thread.mutex.requirements]：非同一线程解锁是 UB）；
- 用 `std::mutex` 会在 Wake 路径引入未定义行为（glibc 可能不报错，但 TSAN 会报；换实现可能崩）。

### 1.2 持锁 park 无法防护（自死锁）

- `m_wake_mutex` 的临界区只有几条赋值，**绝不能 park/sleep/回调**；
- `std::mutex` 不阻止持锁 park：若未来有人在临界区内调 Scheduler park（例如回调里），持锁 park → 自死锁；
- FutexMutex 的语义就是「极短、不阻塞」，代码审查可据此约束；std::mutex 没有这层保护。

### 1.3 热路径开销

- `Arm()`/`Disarm()`/`Wake()` 在每次 park/wake 都执行（高频）；
- FutexMutex 无竞争 = 1 次 CAS（~ns）；`std::mutex`（pthread_mutex_lock）有额外封装/原子操作/可能的 futex 路径；
- 实测参考：sync::Mutex 优化后 18–19ns（快路径），内部锁若用 std::mutex 会拖慢。

### 1.4 语义不匹配

- WaitNode 的内部锁**不需要**公平性、超时、所有权（锁只护几条字段）；
- `std::mutex` 的完整语义（公平/超时/递归检测）是额外负担，且 glibc 的 owner 检查在跨线程解锁场景下反而有害。

### 1.5 与 Fiber 调度的交互

- 若临界区内触发 Fiber 调度（park），`std::mutex` 无「禁止 park」检查；
- FutexMutex 的临界区在编译器/审查层面可保证不含 park 调用（几条赋值）。

## 2. 结论

> WaitNode 的内部锁**必须**是 FutexMutex（无 owner、可跨线程解锁、极短、禁止阻塞），
> 不能用 std::mutex。

## 3. 合并 JinShanProject 代码时的处理

JinShanProject（`E:\CodexWorkspace\JinShanProject`）是 Windows/C++ 项目，其同步原语可能用
`std::mutex`。合并/参考其代码时：

1. **检查**：JinShan 代码中是否有「跨线程 Wake/解锁」的同步原语（等待节点、事件、CV）；
2. **禁止**：把 `std::mutex` 直接搬进 WaitNode 类实现（跨线程解锁 UB）；
3. **替换**：若 JinShan 的等待节点需要跨线程唤醒，改用 FutexMutex（Linux）或明确的
   `std::mutex + condition_variable`（同线程模式）并文档化线程模型；
4. **审查**：合并后的同步原语必须通过 TSAN（跨线程解锁/持锁 park 检测）。

## 4. 待办

- [ ] 合并 JinShanProject 代码时执行上述检查（第 3 节）；
- [ ] 在 `docs/review/` 保留本记录，合并完成后更新状态。
