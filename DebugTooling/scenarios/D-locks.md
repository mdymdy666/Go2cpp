# scenarios · D 锁与同步（5 个场景详细设计）

> 依据第 3、4 轮对 `sync.hpp`（219 行）、`sync.cpp:665-1005`、`core/hybrid_mutex.hpp`（114 行）的阅读。
> **共同前提**：所有锁的内部状态都在 `Mutex::Impl` 里且**全私有**（`sync.cpp:665-686`），
> 因此 D 类的每一个场景都需要**新增只读接口**。

## D0 · 先把现有锁的形态说清（本节是所有 D 场景的基础）

```cpp
// src/concurrency/sync.cpp:665-686
struct Mutex::Impl {
    std::mutex m_mutex;                       // 队列锁
    static constexpr std::uint8_t kLocked = 1U;
    static constexpr std::uint8_t kWaiter = 2U;
    std::atomic<std::uint8_t> m_state{0U};    // 同一字节同时发布「已持有」与「有等待者」
    std::atomic<std::size_t> m_native_waiters{0U};   // 仅统计普通线程等待者
    std::deque<std::shared_ptr<WaitNode>> m_waiters;  // FIFO 队列
};
```

**关键事实**：
1. `m_state` 只记录「**已锁定**」与「**有等待者**」两个位，**不记录持有者身份** ——
   这是刻意的：`sync.hpp:70-77` 注释明确「Unlock **不要求**由同一 G/线程调用，采用 Go 风格所有权」。
2. `m_native_waiters` 只统计**普通线程**等待者；Fiber 等待者数 = `m_waiters.size() - m_native_waiters`。
3. 队列元素是 `shared_ptr<WaitNode>`，原因写在注释里：Unlock 会**先从队列取出、再在锁外 Wake**，
   裸指针会在这段窗口里悬空。

## D1 · 查询某把锁的持有状态与等待队列

- **目的**：回答「这把锁现在被占着吗？有多少人在等？都是谁（Fiber / 线程）？」
- **现状**：❌ 无任何对外查询接口。
- **能查到什么（基于现有字段）**：
  | 信息 | 可得到？ | 依据 |
  |---|---|---|
  | 是否被持有 | ✅ | `m_state & kLocked` |
  | 是否有等待者 | ✅ | `m_state & kWaiter` |
  | 等待者总数 | ✅ | `m_waiters.size()` |
  | 等待者类型分布 | ✅ | `m_native_waiters`（线程数）→ 其余为 Fiber |
  | **持有者是谁** | ❌ **做不到** | `m_state` 不记录 owner |
- **⚠️ 诚实结论（重要）**：**「谁持有这把锁」在当前实现下无法得知**，
  因为 Go 风格所有权允许「A 加锁、B 解锁」，记录 owner 会改变语义与开销。
  若调试确实需要，只能提供**可选的、默认关闭的 owner 追踪**（仅在调试构建或显式开启时写入），
  且必须写明"开启后与生产行为不同"。
- **新增接口（设计）**：
  ```cpp
  struct MutexDebugSnapshot {
      bool held{false};
      std::size_t waiters{0};
      std::size_t native_waiters{0};
      std::size_t fiber_waiters{0};
      // 可选（需 owner 追踪开启）：GId owner_task; std::thread::id owner_thread;
  };
  MutexDebugSnapshot Mutex::DebugSnapshot() const;
  ```
- **持锁边界**：读 `m_waiters` **必须持 `m_mutex`**（队列锁）；
  `m_state`/`m_native_waiters` 是原子量可无锁读。
- **风险**：低（只读）；但要注意**不得在持队列锁时做序列化或 IO**。

## D2 · 区分等待者类型与数量

- **目的**：判断"是 Fiber 在等，还是 OS 线程在等" —— 这直接决定是否会拖慢 M。
- **现状**：⚠️ 数据已有（`m_native_waiters`，`sync.cpp:676-678`），但**不对外暴露**。
- **语义补充（来自 `LockImpl` 的注释，`sync.cpp:735-745`）**：
  「managed waiter 之间允许有限度地越过队列，以换取吞吐；**一旦有普通线程 waiter**，
  最多再重试 4 次后回到 FIFO handoff，保证线程不会被 Fiber 长时间压住。」
  → 所以 `native_waiters` 不只是"统计量"，它**会影响 Fiber 的重试预算**，是调试时的关键指标。
- **新增**：并入 D1 的快照结构（`native_waiters` / `fiber_waiters`）。
- **风险**：低。

## D3 · 锁争用决策（**本任务最需要新增的能力**）

- **目的**：使用者要求「在锁争抢的时候**决定哪个 Fiber 能获取权限**」。
- **现状**：❌ **不可替换**。FIFO 出队硬编码在 `Mutex::Unlock`（`sync.cpp:955-1005`）：
  从队首取 → `TryFinish(kNotified)` 认领成功者即被选定 → **所有权直接交给队首**
  （注释：「因而 `TryLock` 调用者**不能插队**」）。
- **已存在的两个可替换点，但都不是这个**：
  | 可替换点 | 能改什么 | 不能改什么 |
  |---|---|---|
  | `core::HybridMutex`（`hybrid_mutex.hpp:23-57`） | 自旋次数 `fast_attempts`（默认 64，可 `SetDefaultOptions` 运行期改） | 与等待者选择无关（它只用于调度器内部极短临界区） |
  | `sync::WaitBackend`（`sync.hpp:13-66`） | **怎么 park / 怎么 wake** | **选谁**（接口里没有"选择"语义） |
- **接口形态设计（关键结论）**：
  - ❌ **不能**做成「先释放锁，再让所有等待者重新竞争」—— 那会破坏
    「所有权无缝转移 + `TryLock` 不能插队」这条既有语义；
  - ✅ **必须**做成「**从等待队列中挑一个**」的策略钩子：
    ```cpp
    // 设计（形状示意，非最终 API）
    class WaitSelector {
    public:
        virtual ~WaitSelector() = default;
        // 在持队列锁的前提下调用；返回要选中的节点指针，返回 nullptr 表示不干预（走默认 FIFO）。
        virtual WaitNode* Select(std::deque<std::shared_ptr<WaitNode>>& waiters) noexcept = 0;
    };
    // 接入：Mutex::Impl 增加 std::shared_ptr<WaitSelector> selector（默认空 = 现有行为）
    // 调用点：Mutex::Unlock 的 while 循环之前；选中失败仍需回退 FIFO。
    ```
  - **必须遵守的四条约束**：
    1. 选择**在持 `m_mutex`（队列锁）时**进行，选择器**不得阻塞、不得抛异常**；
    2. 选中的节点**仍要 `TryFinish(kNotified)` 认领**（可能失败 → 回退 FIFO）；
    3. **不得**让未选中的节点重新进入竞争；
    4. 必须处理**饥饿**：建议提供"被跳过的次数上限"，超过则强制 FIFO（否则策略可能饿死某个 G）。
- **与前两个替换点的关系**：三者**正交** —— `HybridMutex` 管自旋、`WaitBackend` 管 park/wake、
  `WaitSelector` 管选谁。可以同时启用。
- **风险**：**高**。会改变单机上的锁公平性与尾延迟；必须默认关闭、显式启用，
  并且**测试覆盖**（顺序性、饥饿、与取消/超时的交互）。

## D4 · 死锁 / 长等待检测

- **目的**：发现"谁在等谁"的环，或"等太久"的锁。
- **现状**：❌ 无。
- **可行性分级（诚实）**：
  | 能力 | 可行性 | 原因 |
  |---|---|---|
  | **长等待告警**（等待超过阈值） | ✅ 可做 | 只需在等待开始时记时间戳，无需 owner 信息 |
  | **等待者列表观测** | ✅ 可做 | 同 D1 |
  | **完整死锁环检测** | ❌ **做不到** | 需要"等待图"（谁持有什么、在等什么），而 **`m_state` 不记录 owner**（D1 已证）→ 无法构建有向边 |
  - → 要做完整死锁检测，**必须先引入 owner 追踪**（可选、默认关闭），
    并且要覆盖**所有**同步原语（`Mutex`/`ConditionVariable`/`WaitGroup`），成本不小。
- **建议的最小可用形态**：只做"**长等待告警 + 等待者 dump**"，并在文档中明确
  「本工具**不做**死锁环判定」，避免使用者误以为能自动定位死锁。
- **风险**：中（时间戳与阈值需要与调度器的 `steady_clock` 一致）。

## D5 · 条件变量等待者快照与通知统计

- **现状**：❌ `ConditionVariable`（`sync.hpp:129+`）有配套 FIFO 队列与 `NotifyOne`/`NotifyAll`
  （`sync.cpp:1139`），但**没有查询接口**，也没有通知计数。
- **设计**：
  - 等待者快照：与 D1 同形（总数 + 类型分布）；
  - **通知统计**：`notify_one_calls` / `notify_all_calls` / `wakeups`（实际被唤醒数）
    —— 用于回答"是空通知多，还是等待者多"；
  - 与 `Mutex` 的**重锁语义**有关：注释明确「正常通知或取消返回前都会重新获得 mutex」
    （`sync.hpp:123-128`），且 `LockForConditionWait` 走**不可被 G 取消中断**的路径
    （`sync.hpp:110-112`、`sync.cpp:707-709`）。调试输出应能体现"正在重锁"这一状态。
- **风险**：低-中。

## D 类共同结论

1. **五个场景都需要新增只读接口**（现有内部状态全私有）；
2. **「谁持有锁」在本实现下不可知**（Go 风格所有权），完整死锁检测因此**不可行** ——
   这两条必须如实写进文档，不能承诺做不到的能力；
3. **D3 是唯一会改变运行时行为的**，接口必须是「从队列中选谁」，且默认关闭、需处理饥饿；
4. 三个可替换点（`HybridMutex` 自旋 / `WaitBackend` park-wake / 新增 `WaitSelector` 选谁）
   **彼此正交**，设计上不应合并。
