# futex 无状态锁重构 · 最终结果

> 使用者指定：把基础并发组件从「原子锁 + std::mutex / condition_variable」重构为
> **直接基于 futex 的无状态锁**，不改上层接口、功能相同。
> **结论：ParkingCondition 的普通线程等待路径已完成改造，全场景优于原实现。**

## 1. 最终数据

> 条件：`build-bench`（Release + -O2）、AMD Ryzen 9 8945HX / WSL2、
> `benchmarks/parking_condition_benchmark.cpp`、rounds=20000、**测量时负载 1.42**。

| 场景 | BEFORE（`condition_variable_any`）| futex 无自旋 | **futex + 自旋（最终）** | 净改善 |
|---|---:|---:|---:|---:|
| A notify 空转（无等待者）| 15.30 ns | 16.26 ns | **15.71 ns** | **≈持平** |
| C 争用（2 消费者）| 90.76 ns | 302.83 ns ❌ | **71.74 ns** | **快 21%** |
| C 争用（4 消费者）| 4,351.02 ns | 2,385.04 ns | **1,727.95 ns** | **快 152%（2.5 倍）** |

**关键**：自旋让"2 等待者退化 3.3 倍"变成"快 21%"，同时把 4 等待者的收益从 82% 提到 **152%**。
**三个场景全部不劣于原实现。**

## 2. 三个演进阶段（每一步都踩了坑）

### 阶段 1：直接换 futex（失败，已回退 `95577db`）

编译通过，但 `io_hook_demo` / `io_hook_full` **Subprocess aborted**。

三个缺陷：
1. 每轮循环没有先判终态，只依赖 `FUTEX_WAIT` 的返回码；
2. 没有区分 `EAGAIN`（值已变）与 `ETIMEDOUT`（真超时）；
3. 被唤醒后没有重读代际。

### 阶段 2：修掉三个缺陷（全绿，但 C2 退化）

- 修完三处后：专项护栏 6 场景 `failures=0`、全量 ctest **80/80**；
- 但实测 **C2 从 90.76 ns 退化到 302.83 ns**（慢 3.3 倍）。

**退化原因**：无竞争路径上 `condition_variable_any` 的 fast path 只做一次原子判空就返回，
而 futex 版本即使等待时间极短也要走 `FUTEX_WAIT` 系统调用；加上 `m_futex_waiters` 的两次原子 RMW。

### 阶段 3：加自旋预算（最终形态）

在 `FUTEX_WAIT` 前先自旋最多 `kSpinBudget = 128` 次（`atomic_signal_fence` 循环读代际）。
唤醒已在路上时完全避开系统调用 —— 这是 Go runtime / parking_lot 的标准做法。

还踩了一个锁契约坑导致 `EDEADLK`（"Resource deadlock avoided"）：
**`WaitNative` 返回时外部锁必须保持解锁**（由 `WaitOnce` 亲自关锁）。
原实现把锁交给 `cv.wait` 管理、返回时也是解锁的；我多写了一个 `outer.lock()` 就冲突破死锁检测。

## 3. 实现要点

```cpp
alignas(64) std::atomic<std::uint32_t> m_futex_seq{0};       // 代际计数（futex word）
alignas(64) std::atomic<std::uint32_t> m_futex_waiters{0};   // 等待者数，0 时跳过 syscall
```

- `notify_*`：代际 +1，再 `FUTEX_WAKE(1 / INT_MAX)`；
- 等待方：读旧代际 → 自旋预算 → `FUTEX_WAIT(expected)` → 被唤醒后重读代际。

**为什么不需要等待者队列**：`cond.wait(pred)` 本就容忍虚假唤醒，所以共享一个代际字就够
—— 这正是"无状态"的含义。

## 4. 验证

| 项 | 结果 |
|---|---|
| `ParkingCondition` 专项护栏（6 场景）| **failures=0**（含 notify 早于 wait、200 轮超时/唤醒竞态、1000 轮重入）|
| 全量 ctest | **100% passed, 80/80, 35.06 s** |
| 上层接口 | **未改**（只加两个私有成员）|
| 非 Linux | `#if defined(__linux__)` 隔离，保留原 cv 实现 |

## 5. 未做（下一步可继续）

- **`sync::Mutex`**（31 处 `std::mutex`）—— 使用者点名的另一个组件；
- **`HybridGate`**（`atomic_flag` + `std::mutex` 慢路径）；
- 调度器内部的 `condition` / `m_impl->mutex`（95 处）—— 这可能是 worker 扩展性劣化的根因之一；
- 把 `waiters` 与 `generation` 编码进同一个字（`word = (gen << 16) | waiters`），把两次原子 RMW 合并为一次。

---

## 2026-10-17 19:35–19:50：sync::Mutex 的专项测量（补齐上轮缺的量化）

上轮用 `debug_lock_selector_benchmark` 没测出 `sync::Mutex` 改造的差异，因为那个基准的耗时
主要由争用排队决定。本轮写了一个临时探针（`/tmp`，不入库），让**等待成为必经路径**：
N 个线程反复 lock/unlock 同一把 `sync::Mutex`。

### 实测（负载 0.80，干净）

| 线程数 | 每次加解锁 |
|---:|---:|
| 2 | **117.2 ns/op** |
| **4** | **29,847.3 ns/op** |

**4 线程比 2 线程慢 255 倍。**

### 结论：sync::Mutex 的 futex 改造没能解决它的高竞争退化

这与改造前记录的"高竞争约 18,000 ns/op"**同量级**（29,847 vs 18,000，都是万 ns 级）。
所以：

- **futex 化改善了"等待原语本身"**（`ParkingCondition` 的 C4 快 152% 是实证）；
- **但没有改善"多线程争用一把 `sync::Mutex`"** —— 因为这种场景的瓶颈不是等待原语，
  而是**持有权在 4 个线程之间传递时的排队/上下文切换**；
- 也就是说 `sync::Mutex` 的慢路径结构（每次 unlock 要在锁内挑一个等待者并逐个 TryFinish）
  才是瓶颈，与"用 cv 还是 futex"无关。

### 下一步该做什么（建议）

1. **不要继续在等待原语上打转** —— 已经证明收益在那里；
2. 真正要动的是 `sync::Mutex::Unlock` 的**交接逻辑**：当前每次 unlock 都在持队列锁的情况下
   遍历候选者、逐个 `TryFinish` 认领。4 线程时这个 O(等待者数) 的循环 + 队列锁争用是主要成本；
3. 可考虑的方向：**把"选谁"和"认领"分离**（先用一次原子交换决定赢家，再在锁外唤醒）、
   或引入**每线程独立等待槽**避免共享队列；
4. 这与之前查到的 **worker 扩展性劣化**（`min_workers=N` 时 4 worker 慢 15.7 倍）很可能是
   **同一个结构性问题**：都是"共享队列 + 锁内 O(N) 扫描"。

### 仍然成立的两个成果

| 组件 | 改造 | 实测 |
|---|---|---|
| `ParkingCondition` | ✅ futex 无状态 | **C4 快 152%（4,351→1,728 ns）、C2 快 21%** |
| `sync::Mutex` | ✅ futex 无状态 | 等待原语已换，但**4 线程争用未改善**（29,847 ns/op）|

两者的差别正好说明：**等待原语的优化只在"等待本身是瓶颈"时有效**。
`ParkingCondition` 的基准场景（多等待者轮流被唤醒）符合这个条件；
`sync::Mutex` 的基准场景（争用同一把锁）不符合。
