# Context 取消回调的约束（禁止 park/阻塞）

> 日期：2026-10-29。背景：Context 的取消回调（`DoneSignal::AddCallback` / 内部回调）在取消
> 传播时同步执行。本文记录「回调里 park/阻塞」的隐患与必须遵守的约定。
> 合并 JinShanProject 代码时作为检查项。

## 1. 执行机制（现状，`src/concurrency/context.cpp`）

- `DoneSignal` 用 mutex 保护 `done` 与两张回调表（`internal_callbacks` 先、`callbacks` 后）；
- 取消传播（`DrainCancellation`）：**锁内标记子树 + 摘出回调/定时器 → 锁外执行回调**；
- 即：回调不是持锁执行的（避免持锁回调再 lock/park 的死锁）。

## 2. 隐患

| # | 隐患 | 后果 |
|---|---|---|
| 1 | **回调里 park/阻塞**（mutex 慢路径、channel、IO 等待） | 在触发取消的线程上同步执行 → 取消方被挂起，取消传播卡住 |
| 2 | **回调里持锁 park**（如持 Mutex 再 park） | 自死锁 |
| 3 | **回调里重入 Cancel/注册回调** | 递归取消；需实现支持重入（当前用「搬出回调表」缓解） |
| 4 | **回调抛异常** | 取消路径崩溃（必须 try/catch） |
| 5 | **回调生命周期**（捕获引用/裸指针） | 回调可能晚于对象销毁执行 → UAF（用 shared_ptr/weak_ptr 捕获） |

## 3. 约定（强制）

1. **取消回调禁止 park/阻塞/长操作**（Go 的 context 同款约定）；
2. 回调内需要唤醒等待者 → 只用非阻塞操作（`TryFinish` + `Wake`，如 `WaitNode::TryCancelAndWake`）；
3. 回调内不要拿用户锁（只允许极短内部锁，如 FutexMutex 几条赋值）；
4. 回调必须 `noexcept` 或内部 try/catch（异常不得逃逸）；
5. 回调捕获对象用 `shared_ptr`/`weak_ptr`，不捕获裸引用/指针。

## 4. 项目内现状（安全）

`WaitNode::TryCancelAndWake(generation)`（Context 取消回调）：

```
lock(m_wake_mutex)            // FutexMutex，几条赋值
if (gen != m_generation) return false
if (!TryFinish(kCancelled)) return false
Wake()                        // backend->Wake（wake_registered，不 park）或 FUTEX_WAKE
```

- 只用极短内部锁 + 非阻塞唤醒——**不 park、不阻塞**，满足约定。

## 5. 合并 JinShanProject 时检查

1. JinShan 的取消回调是否调用阻塞操作（mutex 慢路径/IO/等待）；
2. 回调是否捕获裸指针（析构后回调触发 → UAF）；
3. 回调是否可能抛异常；
4. 合并后跑 TSan/ASan，重点看「回调路径 park/持锁」。

## 6. 待办

- [ ] 合并 JinShanProject 时执行第 5 节检查；
- [ ] 与本目录 `waitnode-lock-design.md` 一并维护。
