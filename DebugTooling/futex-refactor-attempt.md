# futex 无状态锁重构 · 结果

> 使用者指定：把基础并发组件从「原子锁 + std::mutex / condition_variable」重构为
> **直接基于 futex 的无状态锁**，不改上层接口、功能相同。
> 本文记录 **ParkingCondition 普通线程等待路径的成功改造**及其真实代价。

## 1. 结论

| 项 | 结果 |
|---|---|
| 上层接口 | **完全未变**（`wait` / `wait_for` / `wait_until` / `notify_one` / `notify_all` 签名一致）|
| 功能语义 | **一致**（6 个专项护栏场景 + 全量 80 个测试全绿）|
| **4 等待者争用** | **4,351 → 2,385 ns，快 82%** ✅ |
| 2 等待者争用 | 90.76 → 302.83 ns，**慢 3.3 倍** ❌ |
| notify 空转（无等待者）| 15.30 → 16.26 ns（噪声内）|

**净收益取决于等待者数量**：等待者多时大幅受益（这是原 cv 的痛点），等待者少时因
futex 系统调用比 cv 的优化路径贵而退化。

## 2. 实现

### 2.1 用户态状态：一个 32 位代际字

```cpp
alignas(64) std::atomic<std::uint32_t> m_futex_seq{0};       // 代际计数（futex word）
alignas(64) std::atomic<std::uint32_t> m_futex_waiters{0};   // 等待者数，0 时跳过 syscall
```

- `notify_*`：`m_futex_seq.fetch_add(1)` 后 `FUTEX_WAKE(1 或 INT_MAX)`；
- 等待方：读旧代际 `v`，`FUTEX_WAIT(&m_futex_seq, v)`。

**关键洞察**：`cond.wait(pred)` 本来就容忍虚假唤醒，所以**不需要等待者队列/节点分配** ——
一个共享代际字就够，这正是"无状态"。

### 2.2 三个必须写对的细节（第一版曾因此崩溃）

第一版编译通过但 `io_hook_demo` / `io_hook_full` **Subprocess aborted**，已回退（`95577db`）。
第二版修掉三处后才全绿：

1. **每轮循环先判终态**（`result() != kWaiting`），不能只依赖 `FUTEX_WAIT` 的返回码；
2. **区分 `EAGAIN`（值已变，继续等）与 `ETIMEDOUT`（真超时）**，不能都当"继续等"；
3. **被唤醒后重读代际**，作为下一轮 `FUTEX_WAIT` 的 expected。

还有一个锁契约问题导致 `EDEADLK`（"Resource deadlock avoided"）：
**`WaitNative` 返回时外部锁必须保持解锁**（由 `WaitOnce` 亲自关锁）。
原实现把锁交给 `cv.wait` 管理、返回时也是解锁的；我多写了一个 `outer.lock()` 就冲突破死锁检测。

### 2.3 非 Linux 回退

`#if defined(__linux__)` 包住 futex 路径，其它平台保留原 `condition_variable_any` 实现，
语义不变。

## 3. 验证

| 项 | 结果 |
|---|---|
| `ParkingCondition` 专项护栏（6 场景）| **failures=0**，含 notify 早于 wait、200 轮超时/唤醒竞态、1000 轮重入 |
| 全量 ctest | **100% passed, 80/80, 34.25 s** |
| 上层接口 | 未改（只加私有成员）|

## 4. 已知代价与后续优化

**C2（2 消费者）从 90.76 ns 退化到 302.83 ns**，原因推断：
- `m_futex_waiters` 的进出是两次原子 RMW（`fetch_add` / `fetch_sub`）；
- 无竞争时 `condition_variable_any` 的 fast path 只有一次原子判空 + 直接返回，
  而我的实现仍要走 `FUTEX_WAIT` 系统调用（即使等待时间极短）。

**建议的优化方向**：
1. **`FUTEX_WAIT` 前先短暂自旋**（读 `m_futex_seq` 若干次），无竞争时完全避免系统调用 ——
   这是 Go runtime / Rust parking_lot 都用的做法；
2. 把 `m_futex_waiters` 的进出降为 relaxed 语义，或与 `m_futex_seq` 编码进**同一个字**的上/下半部
   （`word = (generation << 16) | waiters`），把两次原子操作合并为一次。

## 5. 未做

- 未改 `sync::Mutex`（31 处 `std::mutex`）与 `HybridGate`（`atomic_flag` + `std::mutex`）；
- 未改调度器内部的 `condition` / `m_impl->mutex`（95 处）；
- 未验证多机/跨架构（futex 是 Linux 专有，已用 `#if defined(__linux__)` 隔离）。
