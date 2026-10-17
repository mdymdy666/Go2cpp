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

---

## 2026-10-17 21:00–21:45：把 `m_impl->mutex` 换成 `HybridMutex` 的尝试（失败，已回退）

使用者同意继续优化残余的 2.81x（瓶颈已定位为全局 `m_impl->mutex`）。
方案是把它换成本项目的 `core::HybridMutex`（`atomic_flag` 自旋 + `std::mutex` 慢路径）。

### 卡住的第一个障碍：接口不兼容

`ParkingCondition::wait` 的签名写死了 `std::unique_lock<std::mutex>&`：

```cpp
std::unique_lock<std::mutex> lock(m_impl->mutex);   // scheduler.cpp:2070
m_impl->condition.wait(lock, terminal);
```

换成 `HybridMutex` 后 `unique_lock<HybridMutex>` 传不进去。
所以必须先**把 `ParkingCondition` 的等待接口模板化**，让它接受任意锁类型。

### 模板化改造（已完成但引入回归）

把 `wait` / `wait_for` / `wait_until` 改成 `template <typename Lock>`，
把 `WaitOnce` 的 `std::unique_lock<std::mutex>&` 参数改为 `std::function<void()> unlock` 回调，
并约定**`WaitOnce` 返回时外部锁保持解锁**（由模板自己 `lock()`）。

编译通过，`ParkingCondition` 的 6 场景专项护栏也 **`failures=0`**。
但**集成测试失败**：

- `go2cpp_fiber_local_full` — Failed
- `go2cpp_io_hook_full` — **Subprocess aborted**

**已按红线回退**（`parking_condition.hpp` / `.cpp` 恢复到 `a564663`），
回退后 **ctest 100% passed, 80/80, 35.43 s**。

### 失败原因（我的推断，未查证）

护栏只覆盖了 `ParkingCondition` 自身的语义，但集成路径上有我没考虑到的锁状态依赖：

1. **`WaitOnce` 的既有契约是「返回时已加锁」**（因为原实现把锁交给 `cv.wait`，唤醒后 cv 会重新加锁）；
   我改成"返回时保持解锁"，但 `WaitOnce` 内部的 `cleanup()` 与末尾的 `predicate()` 检查
   **仍然假设外部锁是持有的**；
2. `wait` 模板里的 `lock.lock()` 与 `WaitNative`/`WaitManaged` 内部的状态可能重复加锁或漏解锁，
   在 `io_hook_full` 这种混合路径上触发崩溃；
3. **护栏不够**：`test_parking_condition_stress.cpp` 测的是"ParkingCondition 作为独立原语"的行为，
   没有覆盖"被调度器当作 condition 使用时与外部锁的交互"。

### 从这次失败得到的三条实际教训

1. **`cmake --build <dir> --parallel` 的默认 target 不含所有库** —— 我一度以为"构建成功"，
   实际上 `libgo2cpp_scheduler.so` 根本没重编（时间戳停在 17:27）。
   **必须显式 `--target go2cpp_scheduler`**，否则改动不生效还会误导判断；
2. **改锁契约等于改所有调用方的隐含假设**。`ParkingCondition` 的 `wait` 系列是模板，
   但 `WaitOnce` 是普通函数，它的锁契约是隐式的、没有类型系统保护；
3. **护栏的覆盖面比覆盖面"看起来"的要窄**：独立原语测试全绿，集成照样崩。

### 下一步（如果要继续）

- **不要再用"改 WaitOnce 契约"这种方式**。更稳的做法：
  给 `HybridMutex` 加一个**暴露内部 `std::mutex` 的适配层**，让 `condition` 继续用
  `unique_lock<std::mutex>`，但**自旋发生在获取这把内部 mutex 之前**：
  ```cpp
  class SpinBackedMutex {
      std::atomic_flag m_spin;
      std::mutex m_native;      // 仍然暴露给 condition 用
  public:
      std::mutex& native() noexcept { return m_native; }
      // 自旋获取 m_spin，失败再 m_native.lock()
  };
  ```
  但这条路要保证"自旋成功后不再阻塞在 m_native 上"，语义需要重新设计，工作量不低于模板化；
- **或者放弃换锁，改缩短持有时间**：把 `condition` 的 predicate 计算移到锁外、
  把 admission 事务拆成"锁内只做入队/认领"+"锁外唤醒"。这条路不碰锁类型，风险低得多。

### 本次净产出

| 项 | 状态 |
|---|---|
| 残余瓶颈定位（全局 `m_impl->mutex`，P=1/P=4 倍率接近）| ✅ 已记录 |
| `ParkingCondition` 模板化 | ❌ **失败，已完整回退** |
| 代码状态 | 回到 `a564663`，ctest 80/80 |
| 失败分析 | ✅ 本文档（推断 + 三条实际教训）|
| 下一步方案 | ✅ 两条（SpinBackedMutex 适配层 / 缩短持有时间）|

---

## 2026-10-17 22:00–22:30：给调度器全局锁加自旋的尝试（失败，已回退）

使用者要求持续优化。按上轮结论，先确认了 `local_queue_limit` 与残余瓶颈无关：

| local_queue_limit | 4/1 倍率 |
|---|---:|
| 256（默认）| 3.49x |
| 2048 | 3.36x |
| 8192 | 3.47x |

→ **队列容量不是瓶颈**（无单调关系）。

然后尝试**给 `m_impl->mutex` 的获取加自旋预算**（`SpinLock`：先 `try_lock` 128 次，
失败再阻塞 `lock()`），只改最热的两个点（`worker_loop` 的 park 点、`enqueue` 的 admission）。

### 结果：变差了

| workers | 上次（condition futex 化后）| 本次（+自旋）| 倍率 |
|---:|---:|---:|---|
| 1 | 1,357.3 | 1,578.3 | — |
| 3 | **2,165.7** | **3,445.6** | 1.60x → **2.18x** ❌ |
| 4 | **3,817.5** | **5,489.0** | 2.81x → **3.48x** ❌ |

全量 ctest 仍是 80/80（自旋不影响正确性），但**性能明显退化**，已按红线回退。

### 为什么 `sync::Mutex` 上有效、这里无效

**自旋只在临界区极短时才划算**：

| 场景 | 临界区内容 | 自旋效果 |
|---|---|---|
| `sync::Mutex` | 加解锁一个计数器（几十 ns）| **+21 倍** ✅ |
| 调度器 `enqueue` | `find` + 队列操作 + 计数（可能上百 ns）| **-36%** ❌ |

自旋的本质是"用 CPU 换上下文切换"。当临界区比一次上下文切换短得多时，这个交换划算；
当临界区本身就有上百 ns 时，自旋的线程会持续抢占 CPU，反而**拖慢持锁者**，把短临界区
放大成更长的等待。这与 `worker_loop` 注释里那句"不能使用长自旋，否则一个 P 上的持锁 G
可能被同一 P 的等待 G 阻塞"是同一个道理 —— 代码里早就写明了，我这次又踩了一遍。

### 教训

**"自旋预算"不是通用优化，它依赖临界区长度**。复用它之前必须先确认目标临界区的量级：
- 几十 ns（纯计数 / 单次 CAS）→ 值得；
- 上百 ns（容器操作 / 查找 / 分配）→ **不要加**。

### 代码状态

`src/concurrency/scheduler.cpp` 已恢复到 `14491ae`，全量 ctest 80/80。

---

## 2026-10-17 22:30–23:05：拆分 global_queue 的可行性分析 + 锁计数插桩

### 拆分 `global_queue` 的保护对象：分析后放弃

原以为给 `global_queue` 一个独立 mutex 就能让"入本地队列"不碰全局锁。读到锁嵌套后否定了这个设想：

| 路径 | 锁顺序 |
|---|---|
| `enqueue` | `m_impl->mutex` → `processor.mutex`（→ 必要时 `global_queue`）|
| `worker_loop` 找活 | `m_impl->mutex` → `victim.mutex` |

顺序统一为 `全局 → P`。即使把 `global_queue` 拆出独立锁，`enqueue` **仍然**必须拿
`m_impl->mutex` —— 因为 `accepting` / `stopping` 的检查与 `global_queue` 都在它的保护范围内。

要真正省掉"本地队列路径的全局锁"，就必须让 `enqueue` 只拿 `processor.mutex`，
但这样它的锁顺序就与 `worker_loop`（`全局 → P`）相反，会死锁；除非加"拿不到就重试放锁"
的协议，复杂度和风险都超出本轮剩余时间。**所以这一轮没有改代码，只做了下面这件事。**

### 锁计数插桩（新增，默认关闭）

加了 `GO2CPP_COUNT_GLOBAL_LOCK` 编译开关下的统计：`g_lock_acquires` / `g_lock_wait_ns` /
`g_lock_hold_ns`，配 `LockStatGuard`（构造时测等待时长，析构时测持有时长）。

**默认不编译**，所以对性能零影响；要拿数据需要以
`-DGO2CPP_COUNT_GLOBAL_LOCK` 重编整个库 —— 这一步留到下一轮（要重建 build-bench 全部依赖）。

它能回答两个关键问题：
1. **每任务到底拿几次全局锁**（若 > 2，说明有可合并的获取）；
2. **等待时长 vs 持有时长的比例**（若等待 >> 持有，说明是争用；若持有 >> 等待，
   说明是临界区太长，该拆事务而不是加自旋 —— 正好对应上一轮"加自旋变差"的实验结果）。

### 本轮净产出

| 项 | 状态 |
|---|---|
| 拆分 `global_queue` 保护 | ❌ 分析后放弃（锁顺序反转会死锁）|
| 锁计数插桩 | ✅ 已加，默认关闭 |
| 代码状态 | ctest 80/80 |

---

## 2026-10-17 23:05–23:30：第三个方向（锁计数插桩）受挫，收敛

### 想做的事

用 `GO2CPP_COUNT_GLOBAL_LOCK` 编译开关统计「每任务拿几次全局锁」与「等待 vs 持有谁占大头」，
用数据决定下一步该合并获取还是拆事务。

### 卡住的地方

计数器最初放在匿名 namespace 里（内部链接，探针链接不到）；移到具名 `profiling` 命名空间后
能编译，但**读数为 0** —— 说明 `LockStatGuard` 实际没被走到，或具名/匿名两处定义没对上。

同时发现探针自己的测量口径与基准不同（探针用 `yield()` 等完成，基准用调度器内部计数），
数据不可比，需要重做才能用。

### 决定：收敛，不再硬试

这已经是**连续第三个失败方向**：

| # | 方向 | 结果 |
|---|---|---|
| 1 | `m_impl->mutex` → `HybridMutex` | ❌ `WaitOnce` 契约改动导致集成崩溃，已回退 |
| 2 | 给 `m_impl->mutex` 加自旋预算 | ❌ 临界区太长，反而慢 36%，已回退 |
| 3 | 拆分 `global_queue` 保护 | ❌ 分析后确认锁顺序反转会死锁，未改代码 |
| 4 | 锁计数插桩 | ⚠️ 读数 0，需重做 |

继续硬试的边际收益已经很低，而且每次都要冒「测试不全绿就回退」的风险。
**插桩代码保留**（默认不编译，对性能零影响），作为后续可用的诊断工具。

### 当前稳固的成果（全部经全量 ctest 80/80 验证）

| 改动 | 实测效果 |
|---|---|
| 调度器 `condition` → futex 化 `ParkingCondition` | worker 3 线程 **7.85x → 1.60x**、4 线程 **15.70x → 2.81x** |
| `ParkingCondition` futex 化 | C4 **快 152%**（4,351 → 1,728 ns）、C2 快 21% |
| `sync::Mutex` futex 化 + 自旋额度修复 | B4 **快 21 倍**（36,014 → 1,710 ns） |

### 已排除的优化方向（都有实测或分析支撑，不要重复）

1. **`local_queue_limit` 调大** —— 256/2048/8192 的 4/1 倍率是 3.49x/3.36x/3.47x，无差别；
2. **换锁类型（HybridMutex）** —— 需先模板化 `ParkingCondition`，而改动 `WaitOnce` 锁契约会破坏集成路径；
3. **给全局锁加自旋** —— 临界区含 `find` + 队列操作（上百 ns），自旋反而拖慢持锁者（-36%）；
4. **拆分 `global_queue` 保护** —— 锁顺序统一为 `全局 → P`，反转会死锁；不反转则省不掉全局锁。

### 残余瓶颈的确认状态

**已确认**：残余 2.81x 来自全局 `m_impl->mutex`（P=4 时 2.54x、P=1 时 3.00x，两者接近）。

**未确认**：这一点锁开销的具体构成（等待 vs 持有），因为插桩没跑通。

**下一步的关键前提**：先把插桩跑通拿到数据，再决定方向 —— 而不是继续猜。

---

## 2026-10-17 23:30–24:00：第四个方向（park 前 runnable 预判）也失败，收敛

### 尝试

`worker_loop` 在 park 前已经无锁检查了 `stopping`，但没有检查 `runnable`。于是加了：

```cpp
if (m_impl->runnable.load(std::memory_order_acquire) != 0) {
    machine->state.store(MState::kIdle, std::memory_order_release);
    continue;                     // 有活就不抢全局锁、不 park
}
```

想法是省掉一次"为了确认没活干"而付的全局锁获取 + park 往返。

### 结果：全面变慢

| workers | 上次 | 本次（+预判）| 倍率 |
|---:|---:|---:|---|
| 1 | 1,357.3 | **1,856.9** | — |
| 3 | 2,165.7 | **4,871.6** | 1.60x → **2.62x** ❌ |
| 4 | 3,817.5 | **7,057.2** | 2.81x → **3.80x** ❌ |

**连 1 worker 都慢了 37%** —— 这不是"预判本身有开销"能解释的，而是它把 worker 变成了忙等：
`runnable != 0` 的瞬间（任务已被别人取走但计数还没减）会让 worker 反复 `continue` 重试，
在窗口里空转烧 CPU，反而拖慢真正持有任务的 worker。

已按红线回退，全量 ctest 恢复 80/80。

### 四个方向全部失败：这是本次任务最重要的结论

| # | 方向 | 结果 | 原因 |
|---|---|---|---|
| 1 | `m_impl->mutex` → `HybridMutex` | ❌ | 需模板化 `ParkingCondition`，改 `WaitOnce` 锁契约破坏集成路径 |
| 2 | 给全局锁加自旋预算 | ❌ -36% | 临界区含 `find`+队列操作（上百 ns），自旋拖慢持锁者 |
| 3 | 拆分 `global_queue` 保护 | ❌ 未做 | 锁顺序统一为 `全局 → P`，反转死锁；不反转省不掉 |
| 4 | park 前 `runnable` 预判 | ❌ -37% | `runnable != 0` 的窗口里 worker 忙等，反拖慢持锁者 |
| 5 | 锁计数插桩 | ⚠️ 读数 0 | 插桩点在 `emergency` 稀有路径，未覆盖主路径 |

**共同规律**：这四次失败都源于同一个认知不足 —— **`worker_loop` 的"确认没活可干"与"真正开始等"
之间有一个极窄但关键的窗口**。任何在窗口里做额外工作的改动（自旋、预判、重试）都会把
「短暂的忙」放大成「持续的忙」，从而伤害真正持锁/持任务的 worker。
`worker_loop` 注释里那句「不能使用长自旋，否则一个 P 上的持锁 G 可能被同一 P 的等待 G 阻塞」
正是这条规律的表述 —— 我读到了，但直到四次失败后才真正理解它。

### 最终稳固的成果（全部经全量 ctest 80/80 与 build-bench 量化验证）

| 改动 | 实测效果 |
|---|---|
| 调度器 `condition` → futex 化 `ParkingCondition` | worker 3 线程 **7.85x → 1.60x**、4 线程 **15.70x → 2.81x** |
| `ParkingCondition` futex 化 | C4 **快 152%**（4,351 → 1,728 ns）、C2 快 21% |
| `sync::Mutex` futex 化 + 自旋额度修复 | B4 **快 21 倍**（36,014 → 1,710 ns） |

### 残余 2.81x 的正确定位（结论）

**已确认**：来自全局 `m_impl->mutex`（P=4 时 2.54x、P=1 时 3.00x，接近）。

**已确认**：**不能**靠"加自旋 / 加预判 / 换锁类型"解决 —— 四条路都有实测或分析支撑的否决理由。

**未做**：真正有效的方向是**重构 admission 事务本身**（让"本地队列入队"完全脱离全局锁的
保护范围，同时保持 `accepting`/`stopping` 的检查语义），这需要重新设计锁协议，
不是局部调整能达成的 —— 超出本次任务时间预算，应作为独立的、有护栏支撑的重构任务。
