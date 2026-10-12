# Go2Cpp `fiber` 模块讲解（场景驱动）

> **范围**：`fiber`（栈式协程本体）+ `fiber_local`（Fiber 局部存储）+ `thread_policy`（线程策略作用域）。
> **方法**：先看全仓库怎么用它，再讲为什么这样设计；每场景配真实代码、实现细节与测试证据。
> **引用约定**：以符号/代码片段为主定位，行号仅作参考。
>
> 源码：`include/go2cpp/concurrency/fiber.hpp`(228) + `src/concurrency/fiber.cpp`(1502)
> 　　　`include/go2cpp/concurrency/fiber_local.hpp`(140) + `include/go2cpp/concurrency/thread_policy.hpp`(153)
> 测试：`test_fiber.cpp`(439) + `test_fiber_local.cpp`(124) + `test_thread_policy.cpp`(94)
> 示例：`example/concurrency/fiber_sync_demo.cpp`(100)

---

## 0. 一句话定位

`Fiber` 就是 GMP 模型里的 **G**——一个**有自己栈**、可以在受控边界挂起与恢复的执行单元。
它与 `scheduler` 的关系是**单向**的：`Fiber` 不依赖 `Scheduler` 对象，
而是 `Scheduler` 把 `Fiber` 当作 G 来排队。

头文件的定位声明很克制（`fiber.hpp:86-94`）：

> 依赖：内部 Context 后端提供寄存器/栈切换，Scheduler 负责把 Fiber 作为 G 在 M/P 上排队；
> **Fiber 不依赖调度器对象的生命周期，也不拥有 Scheduler**。
> ……**一个 Fiber 的 resume 调用必须串行**；迁移到其他 OS 线程由调度器保证。

**三句话概括**：① 它只管"挂起/恢复"这一件事；② 它不知道调度器的存在；
③ **`resume` 的串行性是调用方的责任**。

---

## 1. 用量地图

| 层 | 使用情况 |
|---|---|
| `src/` 内部 | `Fiber` 只被 `scheduler.cpp` 与 `context_backend` 使用；**业务模块不直接用 Fiber** |
| `FiberLocal` / `FiberLocalCache` | 被 `fiber_local.cpp` 承载，供需要"G 局部变量"的代码使用 |
| `thread_policy` | 被 `scheduler.cpp`（worker 进入/退出）与 `hook.cpp`（Hook 开关）使用 |

**要点**：`Fiber` 是**调度器的私有执行单元**。普通代码不该直接 `resume` 一个 Fiber——
它应该 `scheduler.spawn()`。测试里直接操作 `Fiber` 是为了**验证 Fiber 本身**，
而非展示推荐用法。

---

## 2. 场景一：挂起 / 恢复 —— 以及一个容易被忽略的细节：**errno**

### 2.1 基本形态

```cpp
    Fiber local_state([&] {
        LifetimeProbe probe(destroyed);
        int local = 40;
        errno = EAGAIN;
        checkpoints.push_back(++local);
        GO2CPP_CHECK(Fiber::Suspend(SuspendReason::Yield));   // ← 挂起，回到 resume 方
        GO2CPP_CHECK(errno == EAGAIN);                        // ← 恢复后 errno 还在
        checkpoints.push_back(++local);
        ...
    });

    errno = EDOM;
    GO2CPP_CHECK(local_state.resume());
    GO2CPP_CHECK(errno == EDOM);       // ← resume 也保存/恢复了调用方的 errno！
```

**这是个很容易漏的设计点**：`errno` 是**线程局部**的（glibc 里是 TLS），
而 Go2Cpp 的 Fiber 在同线程上多路复用 —— 如果上下文切换不保存/恢复 `errno`，
**Fiber A 设的 `errno` 会泄漏给 Fiber B**，而 B 可能正好在判断 `errno`。

测试把**双向**都验证了：Fiber 内部挂起前后 `errno` 保持（`EAGAIN`），
Fiber 外部 `resume` 前后 `errno` 保持（`EDOM` / `ERANGE`）。

**同类需要保存的还有**：栈指针、被调用者保存寄存器（`context_x64.S` 的 ABI 部分）。

### 2.2 挂起原因被记录下来

```cpp
    GO2CPP_CHECK(local_state.suspend_reason() == SuspendReason::Yield);
    ...
    GO2CPP_CHECK(local_state.suspend_reason() == SuspendReason::Park);
```

`SuspendReason` 有 `None/Yield/Park/Io/Timer/Synchronization` —— 这是**观测用途**，
不影响调度决策（`scheduler` 用它来区分"协作让出"与"等待某事"）。

### 2.3 栈大小会被**归一化**，且溢出在**构造期**就被拒绝

```cpp
#if defined(__linux__) && defined(__x86_64__)
        Fiber tiny_stack([] {}, 1U);                       // ← 要 1 字节
        GO2CPP_CHECK(tiny_stack.stack_size() >= page_size * 2U);
        GO2CPP_CHECK(tiny_stack.stack_size() % page_size == 0);

        try {
            Fiber impossible_stack([] {}, std::numeric_limits<std::size_t>::max());
        } catch (const std::length_error&) {
            rejected_overflow = true;                      // ← 溢出在构造期拒绝
        }
#endif
```

头文件的承诺是「**非法或过小值会由实现归一化为可用容量；构造失败会抛出异常**」，
测试把两半都固化了：过小 → 归一化到至少 2 页且页对齐；极大 → `length_error`。

**为什么强调"构造期拒绝"**：注释写得明白——「guard page 的映射加法必须在构造阶段
拒绝溢出，**不能等到首次 resume 才破坏上下文**」。栈映射要做 `size + guard` 的加法，
`SIZE_MAX` 加一下就回绕成小值，"成功"映射一块很小的栈，然后在第一次 resume 时
把栈写穿。**这是一个必须提前拒绝的输入。**

---

## 3. 场景二：跨线程迁移 —— Fiber 的**核心能力**

```cpp
    Fiber migrating([&] {
        int preserved_local = 7;
        int& logical_local = migrated_slot.GetOrCreate(11);
        first_fiber_tid = ::syscall(SYS_gettid);
        GO2CPP_CHECK(Fiber::Suspend(SuspendReason::Yield));
        //   ← 此刻 Fiber 已经在另一个 OS 线程上被恢复
        GO2CPP_CHECK(preserved_local == 7);      // 栈上的局部变量还在
        GO2CPP_CHECK(migrated_local == 1);       // 跨挂起点写入的变量还在
        GO2CPP_CHECK(migrated_slot.TryGet() != nullptr);
        migrated_slot_preserved = *migrated_slot.TryGet() == 11;   // FiberLocal 还在
        second_fiber_tid = ::syscall(SYS_gettid);
    });
```

测试用**两个真实 `std::thread`** 完成交接：
线程一 `resume` 到挂起点 → 线程二 `resume` 跑完 → 断言
`first_thread != second_thread` **且** `first_fiber_tid != second_fiber_tid`（用 `gettid` 双重确认）。

**这验证了"Fiber 的栈是可搬移的"** —— 在现代 OS 上这完全可行，
因为栈就是一块普通内存（Go2Cpp 用的是自己分配的栈，不是 OS 线程栈）。
`context_x64.S` 的上下文结构里保存的是**栈指针**而非"某线程的栈"，
所以换个线程继续跑只是换了个"当前栈指针"的值。

**这也是"Fiber 不依赖调度器生命周期"的物理基础**：谁能拿到 `Fiber` 对象并串行 `resume`，
谁就能推进它。

---

## 4. 场景三：嵌套 —— 父 Fiber 的栈上可以再起子 Fiber

```cpp
    Fiber nested_parent([&] {
        Fiber nested_child([&] {
            nested_continuation = 1;
            Fiber::Suspend(SuspendReason::Yield);
            nested_continuation = 2;
        });
        GO2CPP_CHECK(nested_child.resume());
        GO2CPP_CHECK(nested_continuation == 1);
        Fiber::Suspend(SuspendReason::Yield);        // ← 父也挂起
        GO2CPP_CHECK(nested_child.resume());
        GO2CPP_CHECK(nested_child.state() == FiberState::Completed);
    });
```

**注意**：`nested_child` 是父 Fiber **栈上的局部对象**。这意味着：
- `resume` 子 Fiber 时，**真正的调用链**是 `外层线程 → nested_parent 的栈 → nested_child 的栈`；
- 若父在子还活着时结束，子的"父栈"就没了 —— 这正是下面场景四要处理的事。

### 父链快照与"墓碑帧"

```cpp
    const auto tombstone_snapshot = tombstone_child->context_snapshot();
    GO2CPP_REQUIRE(tombstone_snapshot.size() >= 3);
    GO2CPP_CHECK(!tombstone_snapshot[1].alive);      // ← 父帧已死，但记录还在
```

头文件解释了墓碑的用途：

> Fiber 被迁移或挂起后仍保留首次进入时的父级关系；父对象结束后由共享记录保留
> `alive=false` 的墓碑帧。**墓碑只用于观测、取消和诊断，不能恢复已销毁的父栈。**

**即**：诊断链（`context_snapshot()`）在父死后仍然可用，且**明确不承诺可恢复**。
实现用 `FiberRecord`（共享记录）保存帧信息，因此不依赖父对象存活。

---

## 5. 场景四：析构语义 —— "取消 + 跑到自然结束"

```cpp
    {
        Fiber abandoned([&] {
            LifetimeProbe probe(abandoned_destroyed);
            Fiber::Suspend(SuspendReason::Park);
            abandoned_continued = true;
            abandoned_cancelled = Fiber::CancellationRequested();
        });
        GO2CPP_CHECK(abandoned.resume());
        GO2CPP_CHECK(abandoned.state() == FiberState::Suspended);
    }   // ← 析构
    GO2CPP_CHECK(abandoned_destroyed.load() == 1);   // 局部对象已析构
    GO2CPP_CHECK(abandoned_continued);               // 主体继续执行到结束
    GO2CPP_CHECK(abandoned_cancelled);               // 且能看到取消标记
```

**析构一个挂起中的 Fiber 会做三件事**：
1. 置**取消标记**；
2. **恢复它**，让它从挂起点继续跑；
3. 等它自然结束（这样它栈上的局部对象才能正确析构）。

**这解释了一个反直觉的现象**：`Fiber` 的析构**可能阻塞**。
头文件的警告是明确的：

> **忽略取消的主体可能阻塞析构。**

### 与 X-13 的关系

我在 include 审计阶段登记了 **X-13**：这条"阻塞"在实现层是
**无界 yield 自旋**（`src/concurrency/fiber.cpp` 的析构等待循环）。
测试里 `cancellation_loop` 与 `finite_suspensions` 两个用例说明：
**只要主体响应取消标记（或挂起次数有限），析构就能正常返回**；
而一个"永不检查取消、无限挂起"的主体，会让析构**永远自旋**。

**这是 X-13 的实测面**：文档承认会阻塞，但没说"忙等且无上限"。

### 未启动的 Fiber 则完全不同

```cpp
        Fiber unstarted([&, captured] { unstarted_ran = true; });
        captured.reset();
    }
    GO2CPP_CHECK(!unstarted_ran);                    // ← 主体不执行
    GO2CPP_CHECK(unstarted_destroyed.load() == 1);   // ← 但捕获的对象被释放
```

**Ready 状态的 Fiber 析构时跳过主体**（头文件：「Ready Fiber 会跳过主体」）——
因为它还没开始，没有需要展开的栈；但闭包持有的资源会随之释放。

---

## 6. 场景五：**错误父级下的销毁** —— 一个"宁可失败也不要崩"的路径

这是全文件最有意思的一段（测试注释）：

> 子 Fiber 仍挂在父栈上时，如果用户在错误的调用方销毁它，
> 运行时**不能跳入已经不在当前执行链的父栈**。
> 此路径应**放弃子上下文并标记失败，但不能 terminate**；随后父 Fiber 仍应能正常收尾。

```cpp
    Fiber detached_parent([&] {
        auto child = std::make_unique<Fiber>([] {
            GO2CPP_CHECK(Fiber::Suspend(SuspendReason::Park));
        });
        GO2CPP_CHECK(child->resume());
        detached_child.reset(child.release());      // ← 子 Fiber 逃出父栈
        GO2CPP_CHECK(Fiber::Suspend(SuspendReason::Yield));
    });
    ...
    detached_child.reset();                          // ← 在外部（非父栈）销毁
    GO2CPP_CHECK(detached_parent.resume());          // ← 父仍能正常收尾
    GO2CPP_CHECK(detached_parent.state() == FiberState::Completed);
```

**问题本质**：子 Fiber 挂在父 Fiber 的栈上。如果父已经不在当前执行链上，
"恢复子 Fiber 让它跑完"就等于**跳进一个不可达的调用链**——
这在上下文切换层面是**未定义行为**。头文件对这条路径的承诺是：

> Suspended Fiber 只有在固定父级/调用方仍可恢复时才会继续执行。
> ……**错误父级下安全放弃并标记 `Failed`**（无法展开挂起栈上的局部 RAII）。

**即**：宁可**泄漏**子 Fiber 栈上的 RAII 对象、把状态标为 `Failed`，
也不做一次可能崩溃的栈跳转。**这是一个明确的"安全优先"取舍。**

### 相关的第二处：错误父级析构时**也要摘除 FiberLocal**

```cpp
    // 错误父级析构也必须摘除 FiberLocal 值，不能因地址复用把旧 G 的值带给后续 Fiber。
    ...
    orphan_child.reset();
    GO2CPP_CHECK(orphan_local_destroyed.load(std::memory_order_acquire) == 1);
```

**原因**：`FiberLocal` 的存储是按 **G 的逻辑身份**索引的。
如果 G 被销毁却不清理它的槽位，**下一个复用到同一身份的 G 会读到前任的值**——
**这是与 `Context` 的 `ContextKey` anchor 同一类问题**（复用标识导致串味）。

---

## 7. 场景六：异常边界 —— 子层的异常不穿过父层

```cpp
    Fiber child_failure([&] { throw std::runtime_error("nested failure"); });
    bool parent_observed_failure = false;
    Fiber parent_failure([&] {
        const auto result = child_failure.resume_result();
        parent_observed_failure = result.accepted && result.failed() &&
                                  result.failure != nullptr &&
                                  result.context_snapshot.size() >= 3;
    });
```

**注意两点**：
1. 用的是 **`resume_result()`** 而不是 `resume()` —— 前者返回结构化的
   `FiberResumeResult{accepted, state, failure, context_snapshot}`；
2. 头文件对此的设计声明是：「**Fiber 不把 C++ 异常抛出到调度器**；
   调用者可以通过 `failure` 和 `context_snapshot` 显式取得失败原因及完整父链」。

**为什么不让异常穿过**：`scheduler` 在 Fiber 入口有 `catch (...)`，
异常一旦逃出 Fiber 入口就会变成 `GState::kFailed`（整个任务失败）。
而**父 Fiber 想要处理子的失败**时，需要的是一个**可检查的返回值**，不是栈展开。

**这正是 `control_flow` 讲解里"为何不宜用异常实现 panic"的同一个理由**
（见 `docs/control_flow_guide.md` 与台账 X-30 的附注）。

---

## 8. 场景七：`FiberLocal` —— G 局部存储的三种回退

`FiberLocalCache<T>` 的语义在测试里被完整枚举：

| 场景 | 行为 |
|---|---|
| **普通线程** | **TLS fallback** —— `GetOrCreate(7)` 建了就复用（`GetOrCreate(99)` 仍返回 11） |
| **跨 Fiber 隔离** | 两个 Fiber 各持独立值，**不互相覆盖，也不继承线程值** |
| **park 后保持** | Fiber 因等 channel 被 park，醒来后值仍是 55 |
| **Fiber 结束后回收** | 新 Fiber `TryGet() == nullptr` |
| **key 唯一** | 三个 `FiberLocal` 实例的 `key_id()` 互不相同 |
| **跨线程迁移保持** | Fiber 换线程后值仍是 11（场景二） |
| **key 对象先死** | 销毁 wrapper **不能**使 Fiber 中的值失效 |

**最后一条最值得讲**（测试注释）：

> The key object may have a shorter scope than its logical G.
> Destroying the wrapper must not invalidate a value that is still live in a Fiber;
> **the trampoline remains the sole owner of that value's cleanup.**

即：`FiberLocalCache` 对象只是**句柄**，值的所有权在**Fiber 的 trampoline**
（G 的入口包装）手里。所以：

```cpp
    short_lived_cache.reset();                                       // key 先死
    GO2CPP_CHECK(short_lived_destroyed.load() == 0);                 // 值还活着
    GO2CPP_CHECK(short_lived_fiber.resume());
    GO2CPP_CHECK(short_lived_destroyed.load() == 1);                 // Fiber 结束时才销毁
```

**这与 `Context` 的 `ContextKey` anchor 是同一思路的两个方向**：
`ContextKey` 是"**值**钉住**键**"（防地址复用），
`FiberLocalCache` 是"**G** 拥有**值**"（防止句柄先死导致值消失）。
两者都在处理**句柄与数据生命周期不一致**的问题。

---

## 9. `thread_policy`：两个只写 TLS 元数据的作用域

`test_thread_policy.cpp`(94) 覆盖四个检查：

| 检查 | 内容 |
|---|---|
| `check_default_state` | 默认：`scheduler == nullptr`、`kUnmanaged`、`!participates_in_gmp()` |
| `check_participation_scope` | `ScopedThreadParticipation` 析构后**恢复进入前的 TLS** |
| `check_hook_mode_scope` | `ScopedThreadHookMode` 保存/恢复旧值；`SetCurrentThreadHookMode` 往返 |
| `check_runtime_worker_flag` | 调度器 worker 上的快照标 `runtime_worker = true` |

**这个模块的设计克制值得记**。头文件反复强调它的作用域**只写元数据**：

> 它**不会创建 M、绑定 P、安装 `Scheduler::current_scheduler()` 或执行队列任务**。

**即**：`ScopedThreadParticipation` 只是"声明我是谁"，不是"把我变成 worker"。
这个区分很重要——否则外部线程会以为自己可以跑调度任务。

**另一处值得记**：`thread_policy::detail` 里的 `EnterRuntimeWorker`/`LeaveRuntimeWorker`
被明确标注为「**这是源码级 Hook，不是 ABI 承诺**」（见源码回溯轮的记录）。
**主动声明"什么不是承诺"**，比默认沉默要好得多。

---

## 10. 实现细节速览

| 关注点 | 做法 |
|---|---|
| **上下文切换** | Linux x86_64 用项目自带的 `context_x64.S`；其他平台回退 Boost.Context |
| **栈** | 自己分配（非 OS 线程栈），页对齐、至少 2 页、含 guard page |
| **溢出防护** | `size + guard` 的加法在**构造期**校验，拒绝 `length_error` |
| **errno** | 切换时保存/恢复（双向） |
| **诊断链** | `FiberRecord` 共享记录 + 墓碑帧（`alive=false`），父死后仍可查询 |
| **执行绑定** | `FiberExecutionBinding` 只存**不拥有的标识**（`scheduler_token` 是地址整数形式），避免 Fiber 依赖 scheduler 模块、避免调度器生命周期被调试信息反向延长 |
| **resume 串行性** | 由调用方保证；重复 `resume` 被拒绝（测试 `:190`） |
| **FiberLocal** | 按 G 逻辑身份索引；值由 trampoline 拥有 |
| **回收** | `FiberBin`：**只有已完成/失败且上下文已释放**的 G 才能回到创建它的 M 的线程本地池 |

---

## 11. 测试评述：`test_fiber.cpp` 是本项目**边界覆盖最全**的测试

它枚举了 **18 类**边界，其中多类是我在别处没见过有人测的：

| # | 验证点 |
|---|---|
| 1 | 栈归一化 + **溢出构造期拒绝** |
| 2 | 未启动 Fiber 析构：**跳过主体但释放捕获** |
| 3 | **errno 双向保存/恢复** |
| 4 | **跨线程迁移**（栈局部 + FiberLocal 保持，双 `gettid` 确认） |
| 5 | 异常 → `FiberState::Failed` + `failure()` |
| 6 | **排他 resume**（第二次被拒绝） |
| 7 | 遗弃 Fiber 析构：取消 + 跑到结束 |
| 8 | 取消循环 / 有限挂起 |
| 9 | 嵌套 Fiber（父栈上的子） |
| 10 | **错误父级下销毁**：放弃 + `Failed`，不 terminate |
| 11 | **错误父级析构也要摘除 FiberLocal** |
| 12 | **墓碑帧**（父死后快照仍可查） |
| 13 | **调度器 park 穿过完整嵌套链**（inner park → 唤醒先回 inner 再回 outer） |
| 14 | 子异常停在 Fiber 边界内（`resume_result`） |
| 15 | FiberLocal 跨线程 TLS fallback |
| 16 | FiberLocal 值生命周期（Fiber 结束才销毁） |
| 17 | key 对象先死不影响 Fiber 中的值 |
| 18 | `FiberLocal` 三实例 key 唯一 |

**第 13 项特别值得说**：它验证了**嵌套 park 的唤醒顺序**——
`inner` 在调度器里 park 时，`outer` 和根 G 都只保存 continuation；
唤醒后必须**先回到 inner 再回到 outer**，不能把 outer 的后续代码提前执行：

```cpp
            GO2CPP_CHECK(nested_scheduler.park_current());
            ...
            GO2CPP_CHECK(inner.resume());
            // 该断言在 inner park 期间不能执行；它只会在 inner 完成后继续，
            // 验证 scheduler 唤醒没有跳过父 Fiber continuation。
            GO2CPP_CHECK(nested_stage.load(std::memory_order_acquire) == 3);
```

**这是"用手写断言检测执行顺序错误"**——如果唤醒顺序错了，
`scheduler.park_current()` 会让 inner 直接返回给 scheduler，
而 outer 的后续代码会**在 inner 还没跑完时**被执行，断言就会失败。

---

## 12. 本模块发现的问题

| 编号 | 严重度 | 摘要 |
|---|---|---|
| **T-17** | **中** | **`FiberBin` 的两个调度器专用接口零覆盖**——而它们正是 `scheduler` 的依赖面 |
| T-18 | 低 | `thread_policy` 两处未覆盖：引用版构造、`kInherit` 的继承语义 |
| X-13 | 中 | （既有）析构等待"可能阻塞"实为**无界 yield 自旋**，文档未说明忙等 |

### T-17 详述

`Fiber::AcquireForScheduler(..., bin_capacity = 32)` 与 `Fiber::RecycleForScheduler(...)`
在 `tests/` 与 `example/` 中**零匹配**。而头文件对它们的约束相当具体：

> 只有**已经完成或失败、且不再有可恢复上下文**的内部 G Fiber 才允许回收到创建它的 M
> 的线程本地池。……它们**不会跨线程转移仍挂起的 Fiber**，也不会改变 Fiber 的公开所有权规则。

这三条约束——**回收条件**、**`bin_capacity` 上限**、**跨线程拒绝**——
全都没有测试固化。`test_fiber.cpp` 覆盖了 18 类边界，
**唯独在"调度器侧回收路径"上是空白**，而它恰是 `scheduler` 依赖的接口。

**已登记，建议补测**。

---

## 13. 一页总结

| 维度 | 结论 |
|---|---|
| **它是什么** | GMP 里的 **G**：有自己栈、可挂起/恢复的执行单元 |
| **与 scheduler 的关系** | **单向**——Fiber 不依赖 Scheduler 对象，Scheduler 把 Fiber 当 G 排队 |
| **`resume` 串行性** | **调用方的责任**（重复 resume 被拒绝） |
| **核心能力** | **跨线程迁移**（栈可搬移，因为用的是自己分配的栈而非 OS 线程栈） |
| **易漏的正确性点** | **errno 的双向保存/恢复**（Fiber 复用一个线程，errno 是线程局部的） |
| **必须提前拒绝的输入** | 极大栈尺寸——`size + guard` 会回绕，**构造期**就要 `length_error` |
| **析构语义** | 未启动 → 跳过主体；挂起中 → **取消 + 恢复 + 等它结束**（故**可能阻塞**，见 X-13） |
| **错误父级** | **安全放弃 + 标 `Failed`**，宁可泄漏 RAII 也不做可能崩的栈跳转 |
| **异常边界** | 不穿过 Fiber 边界；父用 `resume_result()` 显式检查 |
| **诊断** | 共享记录 + **墓碑帧**（父死后仍可查，但明确不可恢复） |
| **FiberLocal** | 按 G 逻辑身份；值由 **trampoline** 拥有，故 key 先死不影响值 |
| **待办** | **T-17**（FiberBin 零覆盖）、T-18、X-13 |
