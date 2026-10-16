# design · 09 Debug 类命名与继承约定（第 2 轮开发，使用者补充要求）

> **使用者要求**：所有 Debug 模块的类**尽量继承原来的类**，加 **`Debug` 前缀**，**避免重复实现**。
> 本文件记录该约定的**可行性核实结果**与**分策略落地方式**。

## 1. 核实结果：原类的可继承性（实测）

命令：`grep -rn "class [A-Za-z_]* final" include/`

### 1.1 可以继承（无 `final`，是调试最核心的四个类）

| 原类 | 声明位置 | 拟定的 Debug 类 |
|---|---|---|
| `Fiber` | `concurrency/fiber.hpp:95` | ✅ `debug::DebugFiber : public Fiber` |
| `Task` | `scheduler/scheduler.hpp:258` | ✅ `debug::DebugTask : public Task` |
| `Scheduler` | `scheduler/scheduler.hpp:407` | ✅ `debug::DebugScheduler : public Scheduler` |
| `SchedulerObserver` | `scheduler/scheduler.hpp:140` | ✅ 本就是插件接口，直接实现 |

### 1.2 **不可继承**（被显式标记 `final`，含调试最关心的几个）

| 类 | 声明位置 |
|---|---|
| `sync::Mutex` | `sync.hpp:78` |
| `sync::ConditionVariable` | `sync.hpp:129` |
| `sync::WaitGroup` | `sync.hpp:177` |
| `core::HybridGate`（`HybridMutex`） | `core/hybrid_mutex.hpp:33` |
| `core::ParkingCondition` | `core/parking_condition.hpp:22` |
| `Channel<T>` | `concurrency/channel.hpp:700` |
| `Context` | `concurrency/context.hpp:146` |
| `core::Timer` / `core::TimerService` | `core/timer.hpp:163/208` |
| `Scheduler::BlockingRegion` | `scheduler/scheduler.hpp:413` |
| `Future` / `Promise` | `concurrency/future.hpp:790/693` |
| `FiberLocalCache` | `concurrency/fiber_local.hpp:50` |
| 其余（`logging/`、`network/http.hpp`、`address.hpp` 等） | 同文件 |

> **为什么不能去掉 `final`**：使用者明确要求"**不要影响原来的框架**"。
> 修改既有类的限定符属于改动原框架，且会改变其 ABI/继承语义。因此**不动**。

### 1.3 一个额外的硬约束：`Fiber` 系列**没有虚析构**

实测（编译探针 `is_polymorphic<Fiber>` = **0**）：

```
Fiber_virtual_dtor = 0     ← 非多态类型
Fiber_is_final     = 0     ← 可继承
Fiber_default_ctor = 0
Fiber_copyable = 0  movable = 0
```

→ **规则：`Debug*` 对象绝不可通过基类指针/`unique_ptr<Base>` 销毁**。
   具体到本项目：**`DebugFiber` 不能放进 `Task::m_fiber`**（那是 `std::unique_ptr<Fiber>`，
   `scheduler.hpp:364`），否则 `~Fiber()` 非虚 → 未定义行为。

## 2. 分策略落地（三档）

| 档 | 适用 | 做法 | 命名 |
|---|---|---|---|
| **A. 继承** | 无 `final` 的类（`Fiber`/`Task`/`Scheduler`） | `Debug* : public 原类`，只**增加**调试字段与方法，**不覆盖**已有行为 | `DebugFiber` / `DebugTask` / `DebugScheduler` |
| **B. 组合** | `final` 但需要**行为包装**（如统计每次等待耗时） | `Debug*` **内含**原类引用/共享指针，转发调用 | `DebugWaitBackend`（已存在的 `WaitBackend` 正是这样）、`DebugSink` |
| **C. 只读快照** | `final` 且**只需观测**（`Mutex`/`ConditionVariable`/`Channel`/`Timer`…） | **不继承也不包装**，用纯数据结构承载快照 | `DebugMutexSnapshot` / `DebugCondVarSnapshot` / `DebugTimerSnapshot` |

**选择依据**：
- 观测**不需要**多态或行为扩展 → 快照结构最简单、零侵入（档 C 最常用）；
- 只有需要"藏字段/加方法"（`DebugFiber`）或"换实现"（`DebugSink`）时才用 A/B。

## 3. 与"避免重复实现"的关系

- 档 A/B **天然复用**原类行为（不重写）；
- 档 C 的快照结构**不含逻辑**（只是字段搬运），因此也不构成重复实现；
- **不复制**任何原类的状态机、锁策略、调度逻辑 —— 这些只在原类里存在一份。

## 4. 对既有设计文档的影响

`design/06-interface-signatures.md` 的 R 组接口中，凡是"给 `final` 类加方法"的写法
（例如 `Mutex::DebugSnapshot()`）**应改为档 C 的独立快照函数**，避免触碰原类定义：

```cpp
// 原设计（需给 final 类加成员方法）
MutexDebugSnapshot Mutex::DebugSnapshot() const;

// 调整后（不修改原类；用自由函数 + 友元或公开只读接口）
// 若原类无可读接口，则通过 Debug 侧的注册表/探针收集，或在原类上仅加 const 只读方法
```

> **尚未定案**：档 C 若发现原类**完全无法从外部读取**所需字段（`Mutex::Impl` 全私有），
> 有两条路：① 在原类上加**只读** `const` 方法（极小侵入，但确实改了原类）；
> ② 让 `Debug` 侧成为 `friend`（同样改原类）。**这一取舍留待 P3 阶段按实际情况定**，
> 本轮不擅自决定。

## 2.4 使用者确认的方案：**代理 + 原类私有 `Debug*` 方法**（第 4 轮）

使用者在看到"复制 `Mutex` 需连带 350–1000 行、且上游一改就漂移"的数据后，确认采用：

1. **`final` 类不继承、不复制实现**；
2. 需要观测或改行为的，建 `Debug*` **代理**（组合持有原类的引用或共享指针）；
3. **在原类内部新增私有 `Debug*` 方法**（由 `Debug*` 代理通过 `friend` 访问）；
4. 侵入极小（每个类几行），**不复制实现、无上游漂移**。

> 与 §2 三档策略的关系：**§2.4 取代了档 C 的"只读快照"**（快照只能读、无法表达
> "原类内部提供的调试动作"）；档 A（继承）仍适用于无 `final` 的 `Fiber`/`Task`/`Scheduler`；
> 档 B（组合）即本节的代理形式的特例。
>
> 也就是说最终策略是两档：**能继承的继承（`Debug* : public 原类`）；
> 不能继承的用代理（`Debug*` 组合原类 + 原类内私有 `Debug*` 方法 + `friend`）**。

### 命名与接口约定

| 项 | 约定 |
|---|---|
| 代理类名 | `Debug` + 原类名（`DebugMutex`、`DebugConditionVariable`、`DebugChannel`…） |
| 代理位置 | `include/go2cpp/debug/` 下，按模块分文件 |
| 原类内的方法名 | `Debug*`，**私有**（例如 `Mutex::DebugWaiters()`），并在原类内 `friend class DebugMutex;` |
| 默认开销 | 私有方法只在被代理调用时执行；代理不存在则**零开销** |
| 命名空间 | `go2cpp::debug` |

### 侵入清单（每个 `final` 类需要改的行）

```cpp
// 在 sync.hpp 的 Mutex 类内
class Mutex final {
public:
    // ... 原有内容不变 ...
private:
    friend class go2cpp::debug::DebugMutex;   // ← 新增 1 行
    DebugMutexSnapshot DebugSnapshotInternal() const;  // ← 新增 1 行声明
};
```
（实现在 `sync.cpp` 内新增约 5–10 行。）

→ **每个类净增 2 行声明 + 5–10 行实现**，原行为与语义**完全不变**。
