# 专项：Fiber 链级元信息集中化（防止元信息不实时）

> 来源：2026-11-05 用户审查提出（原话保留）：
>
> 「这个其实我认为是一个错误的实现，万一存在一个执行流，记录了中间的 Fiber，但是这个 Fiber 的
> main fiber 中间改变了状态，恰好这时候中间 Fiber 状态没改过来，被使用了，非常危险。所以这里
> 需要一个 Fiber 元信息的专项，**所有的 Fiber 链上面的共享的东西，直接记录在一个东西里面**，
> 防止同链多 Fiber 并发引用导致的元信息不实时导致的问题。」

---

## 一、当前实现（实测，`src/concurrency/fiber.cpp:1401`）

```cpp
struct FiberRecord {
    std::shared_ptr<FiberRecord> parent;      // 链：子上 → 父
    std::atomic<bool> alive;
    std::atomic<FiberState> state;            // 每个 Fiber 独立
    std::atomic<SuspendReason> reason;        // 每个 Fiber 独立
    std::atomic<bool> cancellation_requested;// 每个 Fiber 独立
    mutable std::mutex mutex;                 // 每个 Fiber 一把锁
    std::uint64_t active_parent_id;
    std::size_t depth;
    ...
};
```

**读取取消状态**（`CancellationRequested()`，实测）：

```cpp
std::shared_ptr<FiberRecord> record = current->m_impl->m_record;
while (record != nullptr) {
    if (record->cancellation_requested.load(acquire)) return true;
    std::lock_guard<std::mutex> lock(record->mutex);   // ← 每层一次锁
    record = record->parent;
}
```

## 二、用户指出的危险（风险场景）

### 场景 A：中间 Fiber 被记录，父链状态变了但中间没同步

```
main fiber (M0)  ──创建──► mid fiber (M1) ──创建──► leaf fiber (M2)

某个执行流持有 M1 的 shared_ptr<FiberRecord>（如 Context 回调、debug 注册、future）
   ↓
main fiber 改变状态（取消 / 结束 / 迁移）
   ↓
M1 的 cancellation_requested / state 仍是旧值
   ↓
执行流读 M1 的元信息 → **读到过时的值** → 误判「还能用」→ 危险
```

### 场景 B：链上状态不一致

- M0 被取消（`cancellation_requested = true`）；
- M1 自己的 `cancellation_requested` 仍是 `false`；
- 执行流持有 M1 的 `shared_ptr`，直接读 M1 的字段 → **读到 false**（**元信息不实时**）；
- 只有调 `Fiber::CancellationRequested()`（从当前 Fiber 沿链向上走）才正确——**但持有 M1 引用的执行流不是「从 M1 往上走」的上下文**。

### 场景 C：同步延迟

- 取消传播需要在每个 Fiber 上逐个 `store`（或遍历）；
- 中间任何一步被跳过/延迟，链上就出现「半取消」状态；
- 持有中间 Fiber 引用的代码无法判断「我拿到的 M1 到底是否已随链取消」。

## 三、根因

> **「链级共享」的元信息被拆散到每个 `FiberRecord` 上**，靠「沿链遍历」来重建一致性。
> 一旦有人**持有中间节点**的引用，就绕过了遍历，直接读到过时值。

## 四、整改方案：链级元信息集中到一个对象

### 4.1 结构

```cpp
/// 一条 Fiber 链（main + 所有后代）共享的元信息。所有「随链传播」的字段都在这里，
/// 只此一份 —— 任何持有链上任意 Fiber 引用的执行流，读到的都是同一个值。
struct FiberChainState {
    std::atomic<bool> cancellation_requested{false};  // 一次 store 全链生效
    std::atomic<std::uint64_t> deadline_ns{0};        // 未来可扩展
    std::atomic<std::uint64_t> trace_id{0};           // 未来可扩展
    // 注意：不放「每个 Fiber 独立」的东西（state/reason/affinity/depth）
};

struct FiberRecord {
    std::shared_ptr<FiberChainState> chain;   // ← 链上所有 Fiber 共享这一个
    std::atomic<FiberState> state;            // 每个 Fiber 独立（Running/Waiting 不同）
    std::atomic<SuspendReason> reason;        // 每个 Fiber 独立
    ...
};
```

### 4.2 字段归属（哪些共享、哪些独立）

| 字段 | 归属 | 理由 |
|---|---|---|
| `cancellation_requested` | **链级（共享）** | 「父取消 → 子取消」是链语义；共享后 **1 次 store**、读取 **O(1) 无锁** |
| `alive` | **链级（共享）** | 整条链的生命周期一致（父死子必死） |
| `trace_id` / `deadline`（未来） | **链级（共享）** | 跨链追踪/超时 |
| `state`（Running/Waiting/Ready） | **每 Fiber 独立** | 父 Running、子 Waiting 必须能同时表达 |
| `reason`（SuspendReason） | **每 Fiber 独立** | 挂起原因不同 |
| `affinity_machine` / `locked_machine` | **每 Fiber 独立** | M 亲和不同 |
| `depth` / `active_parent_id` | **每 Fiber 独立** | 位置不同 |
| `mutex` | **每 Fiber 独立** | 保护各自字段 |

### 4.3 改造点

| 位置 | 改动 |
|---|---|
| `FiberRecord` 构造 | 创建时 `chain = make_shared<FiberChainState>()` |
| 创建子 Fiber | `child->chain = parent->chain`（**继承同一个**，不新建） |
| `CancellationRequested()` | `return current->m_impl->m_record->chain->cancellation_requested.load(acquire);`（**O(1) 无锁**，不再遍历） |
| 取消传播 | `chain->cancellation_requested.store(true, release)`（**一次 store**，不再逐个 Fiber） |
| 任何「持有中间 Fiber 引用」的代码 | 读 `record->chain->cancellation_requested`（**与父链一致**） |

### 4.4 不变量（必须写清）

1. **一条链只有一个 `FiberChainState`**（根创建，所有后代继承同一个 `shared_ptr`）；
2. **链级字段只能通过 `chain` 读写**，禁止再在 `FiberRecord` 上留同名副本；
3. **`state`/`reason` 仍然每 Fiber 独立**——不要为了「一个值」把执行状态也合并（会导致父 Running、子无法 Waiting）；
4. **`shared_ptr` 生命周期**：链上任意 Fiber 持有就保活 chain state（最后一个退出时释放）。

## 五、验收

1. `Fiber::CancellationRequested()` 从 O(depth) 遍历 + N 次锁 → **O(1) 无锁**；
2. 取消传播从「逐个 Fiber store」→ **一次 store**；
3. **场景 A/B 的回归测试**：
   - 持有中间 Fiber 引用 → 父取消 → **中间 Fiber 读到的链级取消立即为 true**；
   - 深链（depth 1000）→ `CancellationRequested()` 耗时恒定；
4. 现有 `test_fiber*` / `test_scheduler*` / `ctest -R fiber` 全过；
5. TSan 0 报告（共享 `shared_ptr` 的引用计数）。

## 六、与其它专项的关系

- **丢唤醒专项**（`docs/plans/lost-wakeup-plan.md`）：本次改造不涉及 condvar，但 `FiberRecord::mutex` 的遍历加锁会减少——**顺带降低锁竞争**；
- **WaitNode 三套统一**：本次改造**不重叠**（WaitNode 是「等待节点」，FiberChainState 是「链级元信息」），但都属于「元信息集中化」的同一类整改。

## 七、状态

**待开工**（用户确认后按「4.3 改造点」逐项做）。
