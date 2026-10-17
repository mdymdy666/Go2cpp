# futex 无状态锁重构 · 尝试记录

> 使用者指定：把基础并发组件从「原子锁 + std::mutex / condition_variable」重构为
> **直接基于 futex 的无状态锁**，不改上层接口、功能相同。
> 本文记录 **ParkingCondition 的第一次尝试（未成功，已回退）**，以及踩到的坑与后续设计。

## 1. 基线（重构前，`condition_variable_any`）

`benchmarks/parking_condition_benchmark.cpp`，负载 0.08：

| 场景 | 成本 |
|---|---:|
| A notify 空转（无等待者）| **15.30 ns/次** |
| C 争用（2 消费者）| **90.76 ns/次** |
| C 争用（4 消费者）| **4,351.02 ns/次** |

**4 个等待者比 2 个慢 48 倍** —— 这是要攻的量化目标。

## 2. 尝试的实现

### 2.1 设计

`ParkingCondition` 增加两个成员（私有，不影响上层接口）：

```cpp
alignas(64) std::atomic<std::uint32_t> m_futex_seq{0};       // 代际计数（futex word）
alignas(64) std::atomic<std::uint32_t> m_futex_waiters{0};   // 等待者数，0 时跳过 syscall
```

- `notify_one`：`m_futex_seq++` 后 `FUTEX_WAKE(1)`；
- `notify_all`：`m_futex_seq++` 后 `FUTEX_WAKE(INT_MAX)`；
- 等待方：读旧代际 `v`，`FUTEX_WAIT(&m_futex_seq, v)`；被唤醒后**重新检查谓词**。

关键洞察：`cond.wait(pred)` 的契约本来就允许虚假唤醒，所以**不需要等待者队列**，
一个共享代际字即可 —— 这正是"无状态"的含义。

### 2.2 落地的改动

- `parking_condition.hpp`：加上述两个成员（`alignas(64)` 避免与其它热字段同 cache line）；
- `parking_condition.cpp`：加 `FutexWait` / `FutexWake`（`SYS_futex` + `FUTEX_WAIT_PRIVATE` /
  `FUTEX_WAKE_PRIVATE`），`WaitNode::WaitNative` 走 futex，`Wake()` 的普通线程分支改为
  `m_futex_seq.fetch_add` + `FutexWake`；非 Linux 平台保留 `condition_variable_any` 回退；
- `WaitNode` 增加 `ParkingCondition* owner`（futex 字在 owner 上）。

**编译通过，全量 ctest 失败。** `go2cpp_io_hook_demo` 与 `go2cpp_io_hook_full`
**Subprocess aborted**（是崩溃，不是超时；当时 loadavg 14.81，但 abort 不是负载能造成的）。

→ 按红线**立即回退**。回退后重新验证：**ctest 100% passed, 80/80, 33.89 s**。

## 3. 失败原因分析（未经证实，属于推断）

1. **`FUTEX_WAIT` 的 expected 与谓词检查之间存在窗口**：
   我读 `expected` 在 `outer.unlock()` **之前**，而 `notify` 可能在解锁前后任意时刻发生。
   若 `notify` 在"读 expected 之后、进入 FUTEX_WAIT 之前"完成，`FUTEX_WAIT` 会立即返回
   `EAGAIN`（值已变），这本身没问题；但我的循环在 `rc != 0` 时只判超时、**没有再判一次谓词**，
   可能把 `EAGAIN` 当成"继续等同一代际"从而错失唤醒。
2. **`m_futex_waiters` 计数与唤醒的竞态**：`notify` 先读 `m_futex_waiters` 再决定是否 `FUTEX_WAKE`，
   若等待者刚好在两者之间进入等待，唤醒会丢失。经典解法是**先递增再检查谓词**（我已做）
   但 `notify` 侧的"读计数 → 唤醒"不是原子的，仍可能漏。
3. **`io_hook` 路径可能依赖 `ParkingCondition` 的某个非显然语义**（例如同一 node 反复 wait），
   我的实现改变了重入行为。

## 4. 后续正确做法（建议）

- **不要**用"共享代际字 + 无条件重试"这种简化实现；应实现标准的
  `futex word = (generation << 1) | has_waiters` 双字段编码，或用 `FUTEX_WAIT_BITSET` + 位掩码
  区分"代际推进"与"等待者存在"；
- **或**改用已被广泛验证的形态：无锁 MCS/CLH 队列 + `FUTEX_WAKE` 精确唤醒队首，
  这样 `notify_one` 语义与计数天然一致；
- **护栏必须先行**：先在 `tests/concurrency/test_scheduler_stress.cpp`（6 场景，已入库）之外，
  补一个 `ParkingCondition` 专项的并发/超时/取消压力测试，再动实现；
- **分步验证**：先只改"无等待者时 notify 的快速返回"（已能拿到 A 档 15.30 ns 的下界），
  再逐步引入 futex 等待，每步都跑全量 ctest。

## 5. 本次尝试的净产出

| 项 | 状态 |
|---|---|
| `ParkingCondition` 基线基准 | ✅ 已入库（`4d174f2`），量化出 4 等待者 48 倍退化 |
| futex 重构 | ❌ **未成功，已完整回退**（工作树无残留，ctest 80/80） |
| `ParkingCondition` 现状 | 未改动（仍是 `condition_variable_any`） |
| 失败原因分析 | ✅ 本文档（推断，未验证） |
| 后续设计建议 | ✅ 本文档第 4 节 |

**教训**：并发原语的无锁改造不能靠"最小改动"完成 —— 唤醒丢失的窗口极窄，
但一旦命中就是偶发崩溃。这次我按红线回退是对的，但**不该在没有专项护栏的情况下直接改实现**。
