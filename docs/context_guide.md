# Go2Cpp `Context` 模块讲解（场景驱动）

> **范围**：只讲 `context`（取消树 + 截止时间 + 值传递 + 项目特有的 `ContextRollback`）。
> **方法**：先看全仓库怎么用它，再讲为什么这样设计；每场景配真实代码、实现细节与测试证据。
> **引用约定**：以符号/代码片段为主定位，行号仅作参考。
>
> 源码：`include/go2cpp/concurrency/context.hpp`(408) + `src/concurrency/context.cpp`(1895)
> 测试：`tests/concurrency/test_context.cpp`(462)
> 示例：`example/concurrency/context_rollback_demo.cpp`(232)
> 文档：`docs/context_rollback.md`(121)

---

## 0. 一句话定位

`Context` 是 Go `context` 的 C++ 等价物，但它承担了**两件不同的事**：

1. **一个贯穿全运行时的取消/超时信号**——这是它的**运行时职责**；
2. **一个项目特有的两阶段事务封套 `ContextRollback`**——这是它的**使用者 API**。

**它在架构中的位置**：`Context` 是**并发层的地基**。
`fiber`、`scheduler`、`channel`、`future`、`timer`、`io`、`hook`、`sync` 全都引用它。
前面讲 `Error` 时那条"区分超时与取消"的主线，**其数据来源正是这里**
（`CanceledError()` / `DeadlineExceededError()` 两个单例就定义在 `context.cpp`）。

---

## 1. 用量地图：Runtime 只用它的一小半

对 `src/` 全量统计各 Context API 的出现次数：

| API | `src/` 次数 | 说明 |
|---|---:|---|
| **`ContextRollback`** | **36** | **看似最高，但全部在 `context.cpp` 自身** |
| `->Cancel()` | 21 | |
| `->Done()` | 17 | |
| `->IsDone()` | 13 | |
| `DeadlineExceededError()` | 9 | 与 Error 讲解里的判别配对 |
| `->Err()` | 7 | |
| `CanceledError()` | 5 | |
| `Background()` | 5 | |
| `WithCancel` / `WithDeadline` / `WithTimeout` | 各 4 | 创建上下文 |
| `WithCancelCause` / `WithValue` / `TODO()` | 各 3 | |
| **`->Deadline()`** | **1** | 几乎不用——运行时用 `IsDone/Err` 而非查询截止时间 |

**按文件**（除 `context.cpp` 自身 85 次外）：

| 文件 | 次数 | 主要用的是什么 |
|---|---:|---|
| `channel.cpp` | 19 | `->Cancel()` 7、`->IsDone()` 3、`CanceledError` 3、`->Err()` 2 |
| `io.cpp` | 14 | `->Err()` 4、`->Done()` 4、`DeadlineExceededError` 4 |
| `sync.cpp` | 11 | `->IsDone()` 7、`->Done()` 2 |
| `sse.cpp` | 5 | `->Done()` 2、`->Err()`、`DeadlineExceededError` |
| `timer.cpp` | 1 | `->Cancel()` |

**结论（与 `Error` 讲解相互印证）**：

> **运行时对 `Context` 的依赖可以概括成一句话：
> "每个阻塞点在等待时持有一个可被取消、可被超时的信号，
> 醒来后用 `Err()` 区分是取消还是超时。"**
> 而 `ContextRollback` 这个占据了最高出现次数的组件，**运行时一处都不用** ——
> 它和 `control_flow` 一样是**为使用者准备的**（两阶段事务）。

---

## 2. 场景一：取消树 —— 父子传播的三条硬规则

### 2.1 规则一：**先提交整棵子树的取消，再执行回调**

这是本模块最容易做错、也最值得称道的一处设计。测试里的注释写得极清楚
（`test_context.cpp`）：

> Descendant cancellation is committed before parent callbacks run.
> A callback may therefore synchronously inspect/wait on a child without
> deadlocking the cancellation caller.

对应代码：

```cpp
    const auto callback_parent = WithCancel(root);
    const auto callback_child = WithCancel(callback_parent.first);
    std::atomic<bool> callback_waited{false};
    callback_parent.first->Done().AddCallback([&] {
        callback_waited.store(callback_child.first->Done().WaitFor(100ms), ...);
    });
    callback_parent.second();
    GO2CPP_CHECK(callback_waited.load(...));   // ← 回调里等子节点，不死锁
```

**为什么重要**：如果实现是"边走边回调"（取消一个节点 → 立刻跑它的回调 → 再取消子节点），
那么父节点的回调里若同步等待子节点的 `Done()`，**就会死锁**——
因为子节点此刻还没被取消。Go2CPP 选择**两阶段**：
先遍历整棵树把状态全部置为已取消（线性化），**再**逐个执行回调。

`docs/compatibility.md` 也用一句话概括了这个行为：
「取消遍历**先标记整棵子树，再执行回调**」。

### 2.2 规则二：取消是**幂等**的

```cpp
    child_pair.second();
    child_pair.second();          // ← 调两次
    ...
    GO2CPP_CHECK(Is(child->Err(), CanceledError()));
```

`Docs` 也声明「取消是幂等的」。测试还覆盖了**并发竞态**：

```cpp
    // Concurrent cancellation is first-wins and always wakes all observers.
    const auto race_pair = WithCancel(root);
    std::vector<std::thread> cancellers;
    for (int i = 0; i < 8; ++i) {
        cancellers.emplace_back([cancel = race_pair.second] { cancel(); });
    }
```

—— 8 个线程同时调同一个 `CancelFunc`，要求"**first-wins 且所有观察者都被唤醒**"。

### 2.3 规则三：传播是**迭代**的，深树不爆栈

```cpp
    // Cancellation propagation is iterative, so a deeply nested context tree
    // cannot exhaust the native C++ call stack.
    auto deep_pair = WithCancel(root);
    auto deep_leaf = deep_pair.first;
    for (int i = 0; i < 20000; ++i) {
        deep_leaf = WithCancel(deep_leaf).first;
    }
    deep_pair.second();
    GO2CPP_REQUIRE(deep_leaf->Done().WaitFor(2s));
```

**20000 层**。（这与 `Error` 模块的 10 万层深链是同一个设计关切：**凡是遍历用户可控的图，
一律不用递归**。）

---

## 3. 场景二：根 Context —— 与 Go 的第一处**有意差别**

```cpp
    const auto root = Background();
    root->Cancel();
    GO2CPP_CHECK(!root->IsDone());        // ← 根无视直接取消
```

**Go 里 `context.Background()` 是不可取消的**（它没有 `cancel` 方法暴露出来）。
Go2CPP 的根**暴露了 `Cancel()`，但调用无效**——这看起来更"宽容"，实际是
**为了 API 统一**（所有 Context 都是同一个具体类型 `Context`，不像 Go 有接口层次）。

`docs/compatibility.md:13` 明确记录了这条偏差：

> 根 context **忽略直接取消**，`Done()` 是非空、永久 pending 的 `DoneSignal`，
> **不同于 Go nil channel**。

**第二条差别也在这里**：Go 的 `Background().Done()` 返回 `nil` channel；
Go2CPP 返回一个**永久 pending 的 `DoneSignal`**。
好处是调用方可以直接 `WaitFor` 而不必判空；代价是它与 Go 的 nil 语义不同。

**还有一处**：Go 里传空 parent 会 **panic**；Go2CPP 把空 parent **映射为 `Background()`**。

---

## 4. 场景三：截止时间 —— 三个边界值都被认真处理了

### 4.1 正常路径

```cpp
    const auto deadline_pair = WithTimeout(root, 20ms);
    GO2CPP_REQUIRE(deadline_pair.first->Done().WaitFor(500ms));
    GO2CPP_CHECK(Is(deadline_pair.first->Err(), DeadlineExceededError()));
```

### 4.2 已经过期的绝对时间 → 立即完成

```cpp
    const auto already_expired = WithDeadline(root, Context::Clock::now() - 1s);
    GO2CPP_CHECK(already_expired.first->IsDone());
```

### 4.3 **饱和**：极大超时不能绕回过去

```cpp
    // Extreme relative deadlines must saturate instead of wrapping into the
    // past and cancelling a context that was intended to live indefinitely.
    const auto enormous = WithTimeout(root, ContextDuration::max());
    GO2CPP_CHECK(enormous.first->Deadline() == ContextTimePoint::max());
    GO2CPP_CHECK(!enormous.first->IsDone());
    const auto immediate = WithTimeout(root, ContextDuration::min());
    GO2CPP_CHECK(immediate.first->IsDone());
```

**这是整数溢出的正确处理**：`now + max_duration` 会回绕成过去的时间点，
于是"本该永久存活的 Context"会被**立刻判定为已超时**——一个极其隐蔽的 bug。
实现用 `SaturatingDeadline(Now(), timeout)` 做饱和算术，测试把两个极端都固化了。

### 4.4 可注入时钟

```cpp
    const auto fake_now = Context::Clock::now() + 10s;
    Context::SetNowFunctionForTesting([fake_now] { return fake_now; });
    const auto fake_expired = WithDeadline(root, Context::Clock::now() - 1s);
    Context::ResetNowFunctionForTesting();
    GO2CPP_CHECK(fake_expired.first->IsDone());
```

**注意：这里埋着 X-32（见第 11 节）** —— 为了让测试能注入时钟，
`Context::Now()` 每次都要加锁读全局函数，而它在 **future 的 5 处热路径**上被调用。

---

## 5. 场景四：类型化值 —— 以及那个 `ContextKey` 的 anchor

### 5.1 用法

```cpp
    ContextKey<int> key("request-id");
    const auto valued = WithValue(root, key, 42);
    const auto child = WithCancel(valued).first;
    GO2CPP_CHECK(*child->Value(key) == 42);     // ← 沿父链查到
```

值沿父链查找，用 `std::any` 擦除。还支持字符串键（给动态转译代码用）：

```cpp
    const auto empty_key_context = WithValue(root, "", 7);
    GO2CPP_CHECK(std::any_cast<int>(empty_key_context->Value("")) == 7);
```

### 5.2 关键设计：为什么 `ContextKey` 要持有一个"无用的" `shared_ptr`

头文件里 `ContextKey` 长这样：

```cpp
    const void *Identity() const noexcept { return m_token.get(); }
    // Context 节点在类型化值存在期间保留此锚点，避免分配器复用地址让
    // 后续键发生碰撞。
    std::shared_ptr<const std::uint64_t> Anchor() const noexcept { return m_token; }
```

**身份用的是 token 的地址**（`m_token.get()`）。那么问题来了：
`ContextKey<int> k;` 出了作用域后，这个 `token` 被释放，
**下一次 `ContextKey<int>` 很可能被分配到同一个地址** —— 于是新键会**冒充旧键**，
`Value(new_key)` 就能读到 `old_key` 的值。**这是一个 ABA 问题。**

**解法**：`WithValue` 把 `key.Anchor()` 一起存进 Context 节点。
只要那个值还在 Context 里，token 就**不会被释放**，地址也就不会被复用。

测试专门验证了这一点：

```cpp
    // The key's identity allocation is retained by the context.  A later key
    // must not alias it even if the allocator would otherwise reuse an address.
    const auto retained_key_context = WithValue(root, ContextKey<int>{}, 7);
    for (int i = 0; i < 64; ++i) {
        ContextKey<int> unrelated_key;
        GO2CPP_CHECK(!retained_key_context->Value(unrelated_key).has_value());
    }
```

—— 循环 64 次创建临时键，**每次都要求查不到**（如果地址被复用，某一次就会查到 7）。

**这是一处把"分配器行为"纳入正确性论证的设计**，很值得学。

---

## 6. 场景五：`Done().WaitFor()` **必须 park** —— 一个只有 P=1 才暴露的验证

这是我认为整个测试文件里**最有价值的两个断言之一**：

```cpp
    // Done().Wait/WaitFor must park a managed G. With one P the cancelling
    // sibling can only run if the waiter releases its M instead of entering
    // a native condition_variable wait.
    Scheduler managed(1);
    managed.start();
    auto managed_cancel = WithCancel(Background());
    auto managed_waiter = managed.spawn([&] {
        managed_result.store(managed_cancel.first->Done().WaitFor(1s), ...);
    });
    ... // 等 waiter 真的开始等待
    auto managed_canceller = managed.spawn([&] { managed_cancel.second(); });
    GO2CPP_REQUIRE(managed_waiter->wait_for(2s));
    GO2CPP_CHECK(managed_result.load(...));
```

**推理链**：只给 **1 个 P**。等待者在等 `Done()`；
取消者是一个**兄弟 G**，只有在等待者让出 M 之后才能运行。

- **如果** `WaitFor` 用的是原生 `std::condition_variable` → 它**占住唯一的 M** →
  取消者永远排不上 → **测试超时失败**；
- **所以**测试通过就**证明了** `WaitFor` 走的是 park 路径。

**这是一种"用资源约束反证实现路径"的测试手法**——比直接检查"它有没有 park"更可靠，
因为它验证的是**可观察的并发行为**，而不是内部实现细节。

**同一手法在 `ContextRollback::rollback_done()` 上又用了一次**（见 7.3）。

---

## 7. 场景六：`ContextRollback` —— 项目特有的两阶段事务

### 7.1 它是什么

`docs/context_rollback.md` 开篇就把边界划清了：

> `ContextRollback` 是 Go2Cpp 对"临时子 Context + 局部补偿动作"的受限封装。
> **它不是数据库事务，也不是 `longjmp`，不会恢复已经提交的父 Context 状态。**

用法：

```cpp
    ContextRollback scope(parent);           // 建立一个临时 child
    scope.record_undo([resource] { ... });   // 登记补偿动作
    auto mark = scope.savepoint();           // 局部回滚点
    scope.record_undo([resource] { ... });
    scope.rollback_to(mark);                 // 只撤销 mark 之后的
    scope.commit();                          // 或 scope.rollback();
```

内部它其实就干两件事：**持有一个临时 child Context**（用于感知取消/超时），
**持有一个 undo 动作栈**（用于补偿）。

### 7.2 undo 的**线性化顺序**：先 Done，后补偿

测试里有一处非常细的验证：

```cpp
        GO2CPP_CHECK(scope.record_undo([&] {
            undo_observed_done.store(child_scope->Done().WaitFor(100ms), ...);
            ...
        }));
```

—— **undo 执行时，child 的 `Done()` 已经完成了**。
即顺序是：`child.Done()` 线性化 → 然后执行 undo。
头文件对此有明确声明：

> rollback_on_cancel 为 true 时，child Context 被父级取消或 deadline 取消后，
> 尚未提交的 undo 会在 **child Done 已线性化后**由触发取消的线程执行。

**这条保证的意义**：undo 里可以安全地"等待下游观察到取消"，而不会和取消本身互相等待。

### 7.3 `RollbackDone` 与 `child.Done` 是**两个不同的信号**

头文件写得很清楚：

> 与 child Context 的 Done 分离：该信号表示 **commit 或完整 rollback 的补偿动作已经执行完毕**。

| 信号 | 含义 |
|---|---|
| `child.Done()` | 取消已线性化（下游可以开始反应） |
| `rollback_done()` | **补偿动作已经跑完**（资源真的释放了） |

文档还专门警告了这个误区（`context_rollback.md` 的"常见错误"列）：

> 只等 `Done()` 就访问已注销资源，和 undo 并发产生 **UAF**。

**而 `rollback_done()` 也必须能 park**（测试里用同样的 P=1 手法验证）：

```cpp
    // RollbackDone 也必须让 managed G 让出 M；否则 P=1 时执行 rollback
    // 的兄弟任务无法运行，等待者会把整个调度器卡住。
```

### 7.4 状态机与几条不变量

- **`commit` 与 `rollback` 竞争 → 只有一个终态获胜，undo 至多执行一次**
  （测试用两个真线程竞争，再断言 `undo_count <= 1`）；
- **undo 抛异常不会中断其余补偿**，最终状态标 `kFailed` 且 `had_failure()` 为真
  （示例场景五还专门检查"**即使 undo 抛异常，动作已经执行，资源不能泄漏**"）；
- **`Done` 观察者运行时，undo 日志已经冻结**——观察者不能在取消窗口里"追加一个会逃过回滚的新动作"；
- **Savepoint 绑定创建它的事务**（跨事务使用被拒绝），且**局部回滚后旧 savepoint 立即失效**，
  即使动作数量恢复旧深度也不会因 **ABA** 重新生效。

### 7.5 为什么要这么多竞争测试

`test_context.cpp` 里有两处 **32 轮循环**，分别验证：

```
父取消 vs 显式 rollback     （32 轮，断言 undo_count <= 1 且 RollbackDone 必定完成）
commit   vs 父取消          （32 轮，断言线性化顺序决定结果，两条路径都要完成通知）
```

**这是"把并发窗口用重复执行逼出来"的做法**——因为无法用 TSan（本环境不可用），
只能用真实的多次竞争来提高覆盖概率。**这是我在整个项目里见过最细致的并发测试。**

---

## 8. 场景七：与 `Error` 的集成 —— 两条主线在这里汇合

`Error` 讲解里的核心结论是「运行时只依赖 `Is(err, DeadlineExceededError())` 这一个能力」。
那个 `err` 从哪来？**就是 `Context::Err()`**；那两个单例也定义在 `context.cpp`。

```cpp
    const auto deadline_pair = WithTimeout(root, 20ms);
    GO2CPP_REQUIRE(deadline_pair.first->Done().WaitFor(500ms));
    GO2CPP_CHECK(Is(deadline_pair.first->Err(), DeadlineExceededError()));
```

**完整链路**：

```
WithTimeout / WithDeadline
    → TimerService 到期
    → Context::State 标记取消（deadline_error = true）
    → 发布 CanceledError() 或 DeadlineExceededError() 单例到 state->error
    → Done().Signal() 唤醒所有等待者
    → 各模块调 context->Err() 拿到它
    → Is(err, DeadlineExceededError()) 判别
    → 映射成自己的状态码 / errno / 枚举
```

**这条链路横跨 `context` → `timer` → `error` → `channel`/`io`/`sse` 四个模块**，
是本项目最核心的一条贯穿设计。

---

## 9. 实现细节速览

| 关注点 | 做法 |
|---|---|
| **值查找** | `LookupValue(token, name)` 沿父链；类型化键只比 `Identity()`，字符串键比名字 |
| **锚点** | `WithValue` 把 `key.Anchor()`（`shared_ptr<const uint64_t>`）存进节点，防地址复用 |
| **内部回调优先** | `AddBeforeDoneCallback` vs 普通 `AddCallback`——**内部回调（如 `ContextRollback`）必须先于用户观察回调执行**（`context.cpp:22`） |
| **取消遍历** | **迭代**（`std::vector` 显式栈），先全树标记再跑回调 |
| **回调执行** | 在锁外执行 |
| **时钟** | 全局 `s_now_function` + `s_clock_mutex`；默认走 `steady_clock`（**见 X-32**） |
| **根 Context** | 忽略 `Cancel()`；`Done()` 是永久 pending 的 `DoneSignal`（非 nil） |
| **空 parent** | 映射为 `Background()`（Go 会 panic） |

---

## 10. 测试与示例评述

### 10.1 `test_context.cpp`(462) —— 项目里最细致的并发测试

覆盖亮点（按价值排序）：

| 验证点 | 价值 |
|---|---|
| **P=1 时 `Done().WaitFor` 必须 park** | 用资源约束**反证实现路径** |
| **P=1 时 `rollback_done()` 必须 park** | 同上，第二个入口 |
| **先提交子树取消、再跑回调** | 证明回调里同步等子节点不死锁 |
| 两处 **32 轮竞争**（rollback/commit vs 父取消） | 无 TSan 时的替代手段 |
| **20000 层深树**传播 | 迭代实现 |
| **8 线程并发取消**（first-wins） | 幂等 + 公平唤醒 |
| **`ContextDuration::max()` 饱和** | 整数回绕防护 |
| **key identity 的 64 次防别名循环** | 分配器 ABA |
| **savepoint 的 ABA 防护** | 局部回滚后 token 失效 |
| **undo 抛异常后继续执行其余补偿** | 失败路径不中断 |
| **移动语义**（源对象变空、目标负责回滚） | 所有权转移 |

### 10.2 一处盲点（T-16）

**`ContextRollback::committed()` 零覆盖** —— 它的镜像方法 `rolled_back()` 被断言了
四处（`:200`、`:226`、`:237`、`:424`），而 `committed()` 在测试与示例中**一次都没出现**。
`docs/context_rollback.md` 的命名约定表还专门提到过它（"另有 `committed()`、
`had_failure()` 只提供小写形式"），`had_failure()` 已被验证，`committed()` 没有。

### 10.3 `example/concurrency/context_rollback_demo.cpp`(232) —— 六个场景

示例的注释把设计意图交代得很清楚，且**每个场景都有明确的失败判据**：

| 场景 | 内容 |
|---|---|
| 一 | 成功提交，补偿不执行；**"提交后资源仍由业务持有"** |
| 二 | savepoint 局部回滚 + 后续完整回滚；**旧 savepoint 必须失效（防 ABA）** |
| 三 | 父取消 → child Done → undo |
| 四 | deadline 到期 → 自动取消 → undo |
| 五 | **undo 抛异常**：后续动作仍执行、状态 `kFailed`；并警告"**业务必须检查 `had_failure`，不能只看 `rollback()` 返回值**" |
| 六 | 重复使用失效 savepoint 被拒绝；提示"**调用方应重新调用 `savepoint()`**" |

`docs/context_rollback.md` 的六行表格还额外给了**"常见错误"**一列
（如"只等 `Done()` 就访问已注销资源，和 undo 并发产生 UAF"）——
**这种"成功判据 + 常见错误"并列的文档写法，对使用者非常友好，值得作为项目其他文档的范本。**

**运行副作用**：示例写日志到 `log/context_rollback_demo.log`，
与审计流程的"`log/` 基线 md5 保持一致"约束冲突，故本流程只做静态审读。

---

## 11. 本模块发现的问题

| 编号 | 严重度 | 摘要 |
|---|---|---|
| **X-32** | **低-中** | `Context::Now()` 在 **future 的 5 处热路径**上被调用，却每次都加**全局锁**并**复制 `std::function`**，而该函数在正常运行时恒为空 |
| T-16 | 低 | `ContextRollback::committed()` 零测试覆盖（`rolled_back()` 有四处） |

### X-32 详述

```cpp
ContextTimePoint Context::Now() {
    NowFunction now;
    {
        std::lock_guard<std::mutex> lock(s_clock_mutex);   // ← 全局锁
        now = s_now_function;                              // ← 复制 std::function
    }
    return now ? now() : Clock::now();
}
```

`s_now_function` **只在测试中被设置**，正常运行时恒为空——
即每次调用都在为**不存在的自定义时钟**付出一次全局锁 + 一次 `std::function` 拷贝。

**调用点确认为热路径**（`grep` 全量确认）：
`future.hpp:291`（`Wait` 的谓词）、`future.hpp:363`/`:558`（`StatusLocked`）、
`future.hpp:492`、`:601`（`DeadlineAfter`），以及 `context.cpp:1667`/`:1704`
（`WithDeadline`/`WithTimeout`）。**每一次 `Future::Wait`、每一次创建超时上下文都会走这里。**

**最有说服力的对照**：项目在 `log.hpp:363-364` 明确写出了正确做法 ——

> 热路径只做一次原子 shared_ptr 读取。配置变更时生成新的快照，
> 因此格式化器和 Sink **不会在每条日志上争用 Worker 配置锁**。

**同一个项目知道怎么在热路径上避免锁，这一处没做。**
修复只需把 `s_now_function` 换成 `std::atomic<std::shared_ptr<NowFunction>>`，
或先做一次廉价的 `std::atomic<bool>` 判断，测试接口不变。

---

## 12. 一页总结

| 维度 | 结论 |
|---|---|
| **架构位置** | **并发层地基**——`fiber`/`scheduler`/`channel`/`future`/`timer`/`io`/`sync` 全部引用它 |
| **运行时真实职责** | 给每个阻塞点提供**可取消 + 可超时**的等待信号，醒来用 `Err()` 区分原因 |
| **取消树三条硬规则** | ① **先全树标记、再跑回调**（否则回调里等子节点会死锁）② 幂等 ③ **迭代**传播 |
| **与 Go 的三处有意差别** | 根 Context 暴露 `Cancel()` 但无效；`Done()` 非 nil 而是永久 pending；空 parent 映射为 `Background()`（Go panic） |
| **最精巧的边界处理** | `ContextDuration::max()` 的**饱和算术**——不饱和会回绕成过去，让"永久"的 Context 立刻超时 |
| **最精巧的正确性论证** | `ContextKey` 的 **anchor**：身份用 token 地址，故必须钉住它防分配器 ABA |
| **最精巧的测试手法** | **P=1 反证 park**——等待者若不让出 M，兄弟取消者永远跑不起来 |
| **项目特有组件** | `ContextRollback`（临时 child + undo 栈），两阶段事务；**运行时零使用，纯使用者 API** |
| **两信号分离** | `child.Done()`（取消已线性化）≠ `rollback_done()`（补偿已跑完）；只等前者会 UAF |
| **与 Error 的汇合点** | `Err()` 产出 `CanceledError()`/`DeadlineExceededError()` 单例，`Is` 判别后映射成各模块状态码 |
| **待办** | X-32（热路径锁）、T-16（`committed()` 无覆盖） |
