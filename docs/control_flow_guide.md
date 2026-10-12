# Go2Cpp `control_flow` 模块讲解（场景驱动）

> **范围**：只讲 `control_flow`（`defer` / `panic` / `recover`）一个模块。
> **方法**：先看全仓库怎么用它，再讲它为什么这样设计；每个场景配真实代码、
> 实现细节与测试证据。
> **引用约定**：以符号/代码片段为主定位，行号仅作参考。
>
> 源码：`include/go2cpp/tools/control_flow.hpp`(241) + `src/tools/control_flow.cpp`(346)
> 测试：`tests/tools/test_control_flow.cpp`(177)
> 示例：`example/tools/control_flow_demo.cpp`(94)

---

## 0. 一句话定位

`control_flow` 提供 Go 的 `defer` / `panic` / `recover` 三件套，
但做了一个**根本性的取舍**：

> **`panic` 不做栈跳转，只发布状态。**
> （`control_flow.hpp`：「call() 只记录状态，**不使用 longjmp 或破坏栈的跳转**；
> 用户应在 defer 回调中通过 recover 读取和处理状态」）

这是整个模块的设计原点：**放弃"自动跳转"，换取"C++ 析构永远正确"**。

---

## 1. 用量地图：一个"零运行时使用"的模块

对全仓库做包含关系与符号扫描：

| 检查项 | 结果 |
|---|---|
| 包含 `control_flow.hpp` 的文件 | `example/tools/control_flow_demo.cpp`、`include/go2cpp/runtime.hpp`、`src/tools/control_flow.cpp`、`tests/tools/test_control_flow.cpp` |
| **`src/` 中 `defer` 的使用** | **0**（除 `control_flow.cpp` 自身） |
| **`src/` 中 `panic` / `recover` 的使用** | **0** |
| `tests/` + `example/` 中的使用 | `defer` 27 次、`panic` 13 次、`recover` 14 次 |

**结论**：

> **运行时自身完全不用 `defer`/`panic`/`recover`。**
> 它是**为使用者准备的 API**——即被转译的 Go 代码、以及业务代码。
> `runtime.hpp` 把它纳入聚合头，只是为了给外部一个"全功能入口"。

这与 `Error` 模块的形态**正好互补**：
`Error` 是"运行时重度使用、但只用了它一小部分能力"；
`control_flow` 是"运行时完全不用、专供使用者"。

**理解这一点很重要**：讲这个模块时，"它在运行时如何被使用"这个问题
答案是"不如何"——所以下面讲的是**它被设计成怎么用、以及使用者会遇到什么**。

---

## 2. 场景一：`defer` 作为作用域清理（RAII 的 Go 语法糖）

### 2.1 用法

```cpp
{
    defer cleanup([&] { order.push_back(1); });
    defer flush([&] { order.push_back(2); });
    defer captured([&](int value) { order.push_back(value); }, evaluated);
    evaluated = 9;
}   // ← 这里按注册的逆序执行：captured(3)、flush(2)、cleanup(1)
```

### 2.2 实现细节：它是**类型擦除的 RAII 对象**

`defer` 的析构函数就是执行点：

```cpp
defer::~defer() noexcept { run_now(); }
```

回调被擦除进一个 `Callable` 接口：

```cpp
    struct Callable {
        virtual ~Callable() = default;
        virtual void run() noexcept = 0;
    };

    template <typename F>
    struct CallableModel final : Callable {
        void run() noexcept override {
            try {
                std::invoke(m_function);
            } catch (...) {
                detail::RecordDeferException(std::current_exception());
            }
        }
        F m_function;
    };
```

**三个设计点**：

1. **`run()` 是 `noexcept` 且内部 try/catch** —— 因为它要在**析构函数**里被调用。
   如果异常从析构抛出，配合栈展开就是 `std::terminate`。
   所以用户回调的异常被**记录**而不是传播（见场景五）。
2. **删除复制与移动**（`defer(const defer&) = delete; defer(defer&&) = delete;`）——
   一个 defer 对象只对应一次执行，且必须死在它注册的作用域里。
3. **`dismiss()` 与 `run_now()`**：前者取消执行，后者立即执行并置为非活动。

### 2.3 与 C++ RAII 的关系

`defer` 本质上**没有发明新机制**——它就是"把 lambda 装进一个析构时会调用的对象"。
它的价值在于**语法层面接近 Go**，让转译代码的形态与源码一致。

**执行顺序不需要额外实现**：C++ 局部对象的析构顺序天然是**逆序**，
所以 LIFO 语义是"免费"的。测试断言 `order == [3, 2, 1]`。

---

## 3. 场景二：参数在**注册时**求值 —— 一个容易踩的语义

### 3.1 行为

```cpp
    int evaluated = 3;
    defer captured([&](int value) { order.push_back(value); }, evaluated);
    evaluated = 9;          // 之后改掉
    // 回调收到的仍是 3
```

测试断言：`order[0] == 3`。

### 3.2 实现细节：参数被搬进 `std::tuple` 并 **move** 进 lambda

```cpp
    template <typename F, typename... Args>
    static std::unique_ptr<Callable> MakeCallableWithArgs(F&& function,
                                                          Args&&... args) {
        using StoredFunction = std::decay_t<F>;
        using StoredArgs = std::tuple<std::decay_t<Args>...>;
        auto stored_function = StoredFunction(std::forward<F>(function));
        auto stored_args = StoredArgs(std::forward<Args>(args)...);
        return MakeCallable(
            [function = std::move(stored_function),
             args = std::move(stored_args)]() mutable {
                std::apply(
                    [&function](auto&&... values) {
                        std::invoke(function,
                                    std::forward<decltype(values)>(values)...);
                    },
                    std::move(args));
            });
    }
```

**注意 `std::move(args)`**：参数在**首次执行时被移动出去**。
由于 `run_now()` 会置 `m_active = false`（且 `dismiss()` 也能取消），
一个 `defer` 最多执行一次，所以这个 move 是安全的。

### 3.3 为什么这个语义值得专门讲

Go 的 `defer f(x)` 也是**注册时求值**参数（而 `defer func(){ f(x) }()` 才是延迟求值）。
所以这里**与 Go 一致**。示例的注释还特意点明：

> 这个例子用值参数而不是引用参数，便于新手观察"注册时求值"的语义。

**换句话说**：想延迟求值就捕获引用或写闭包——这是使用者的选择，
模块只提供"按值保存"这一种默认行为。

---

## 4. 场景三：`panic`/`recover` 的显式边界

### 4.1 三个对象与所有权

| 对象 | 持有 | 生命周期语义 |
|---|---|---|
| `panic` | `shared_ptr<PanicState>` | **拥有**状态；析构则状态消失 |
| `recover` | **`weak_ptr<PanicState>`** | **不延长**状态寿命；`panic` 先死则安静失效 |
| `PanicState` | `mutex` + `PanicInfo` + `active` + `recovered` | 被上面两者共享 |

**`recover` 用 `weak_ptr` 是刻意的**：

```cpp
    bool bind(panic& source) noexcept {
        m_state = source.m_state;
        return !m_state.expired();
    }
    void reset() noexcept { m_state.reset(); }
    bool bound() const noexcept { return !m_state.expired(); }
```

于是 `panic` 先析构后，`recover` 的 `active()` / `take()` / `peek()` 都安全返回空，
**不会把状态对象钉在内存里**。

### 4.2 触发与消费

```cpp
    panic pa;
    recover rec(pa);
    {
        defer boundary([&] {
            const auto value = rec.take();      // ← 只有在这里才能成功
            recovered_info = *value;
        });
        pa.call("explicit failure", 17);
        GO2CPP_CHECK(pa.active());
        GO2CPP_CHECK(!rec.take().has_value());  // ← 边界外：失败
    }
    GO2CPP_CHECK(!pa.active());
    GO2CPP_CHECK(pa.recovered());
```

**这段测试把"边界"讲得很清楚**：
同一时刻、同一线程，**在 defer 回调外调 `take()` 会失败**，
进入回调后**成功**，且消费后 `panic` 变为非活动 + `recovered`。

### 4.3 与 Go 的差别（必须说清）

| | Go | Go2Cpp |
|---|---|---|
| `panic` 触发后 | **立即展开栈**，执行 defer | **只记录状态，继续往下执行** |
| `recover` 成功后 | 从 panic 点之后继续（调用方的栈已展开） | **没有"继续"这回事**——调用方必须自己 return/分支 |
| 未 recover 的 panic | 程序崩溃并打印栈 | **沉默**（若无观察者，只是状态留在对象里） |

**第三条最需要强调**：Go2Cpp 的 `panic` 若没人 `recover`，
**程序不会崩溃**——它只是留下一个状态对象。示例注释也点明了：

> panic 是显式状态发布；**C++ 不会自动跳转**，业务代码应在这里 return、分支
> 或把状态交给上层。

**这是"用显式状态换取不破坏 C++ 语义"的必然代价**：安全性提高（不会越栈展开），
但使用者必须自己承担"记得检查"的责任。

---

## 5. 场景四：`panic` 状态**可跨 Fiber 迁移**（设计意图）

头文件对此有明确声明（`control_flow.hpp:140`、`:143`）：

> 一个显式的、**可跨 Fiber 迁移**的 panic 状态。
> **可随 Fiber 迁移**的显式 panic 状态。

**实现确实支持**：`PanicState` 被 `shared_ptr` 持有，`panic` 与 `recover`
都可以随对象一起被搬到别的 Fiber 甚至别的线程；`PanicState` 内部有 `mutex`
保护，所以跨线程访问字段也是安全的。

**测试验证了跨线程可见性**：

```cpp
    panic separate;
    recover separate_recover(separate);
    std::atomic<bool> other_thread_saw_active{true};
    std::thread worker([&] {
        other_thread_saw_active.store(separate_recover.active(),
                                       std::memory_order_release);
        GO2CPP_CHECK(!separate_recover());
    });
    worker.join();
    GO2CPP_CHECK(!other_thread_saw_active.load(std::memory_order_acquire));
```

**另一个线程调用 `active()` 是安全的**（走 `mutex`），
而 `operator()()` 在那个线程返回 false——因为它不在 defer 回调里。

---

## 6. 场景五：恢复边界的守门条件

> **更正（2026-10-12，经用户质疑后复核）**：本节原先称这是「本模块唯一的确定性缺陷」并把 X-30 定为「契约违反」，**该判断过重**。复核项目自身文档后确认：`docs/boundary_audit.md:32-33` 与 `docs/compatibility.md:14` **早已明确声明**本模块的边界是「回调作用域，不是编译器级 Go G/栈帧」，且「**跨执行流消费不会被库自动阻止，属于调用方必须遵守的限制**」——即**实现行为是有意为之且已对外声明**。真正的问题只是 `control_flow.hpp:199-200` 的措辞与这两处文档矛盾。X-30 因此下调为「低（文档口径冲突）」。下文保留原始分析，但请以本更正为准。

### 6.0 原始分析（保留）


### 6.1 实现

`take()` 与 `operator()()` 的**第一件事**：

```cpp
bool recover::take() const {
    if (!detail::InDeferCallback()) {
        return std::nullopt;
    }
    ...
}

bool recover::operator()() const noexcept {
    if (!detail::InDeferCallback()) {
        return false;
    }
    ...
}
```

而 `InDeferCallback()` 的定义是：

```cpp
thread_local unsigned int s_defer_depth = 0;

class DeferCallbackScope final {
public:
    DeferCallbackScope() noexcept : m_previous(s_defer_depth) {
        ++s_defer_depth;
    }
    ~DeferCallbackScope() noexcept { s_defer_depth = m_previous; }
};

bool InDeferCallback() noexcept { return s_defer_depth != 0; }
```

**即："是否在 defer 回调内"= "本线程的 defer 深度是否非零"。**

### 6.2 缺陷（X-30）：边界是**线程**级，不是 **Fiber** 级

头文件承诺的是 Fiber 级（`control_flow.hpp:199-200`）：

> take() 只有在活动的 defer 回调期间才会返回状态；
> **其他线程或 Fiber 不能越过恢复边界捕获 panic**。

**但 Go2Cpp 的 Fiber 在同一 OS 线程上多路复用**，所以：

```
1. Fiber A 进入 defer 回调          → s_defer_depth = 1
2. A 在回调中 park（等 channel/锁/IO）→ M 让给 Fiber B
3. B 调用 recover::take()           → 看到 s_defer_depth == 1
                                     → 误判「在 defer 回调内」
                                     → 可以消费掉不属于它的 panic 状态
```

**头文件承诺"其他 Fiber 不能越过"，实现允许它越过**——这是**契约违反**，
不是"实现有待改进"。

`LastDeferException()` / `ClearDeferException()` 同理
（它们读写的 `s_defer_exception` 也是 `thread_local`）：
A 记录的 defer 异常会被 B 读到，B 的清空操作会抹掉 A 的记录。

### 6.3 项目自己有正确答案

`include/go2cpp/concurrency/fiber_local.hpp` 提供 `FiberLocal<T>`：
**在 Fiber 内返回 Fiber 局部值、在普通线程内回退为线程局部值**——
正是本场景所需。`control_flow` 却用了裸 `thread_local`。

### 6.4 为什么只登记不修

改它属于**行为性改动**（要换掉 TLS 载体），不满足"零行为变化"的即时修复例外；
且触发需构造特定交错，本环境 **TSan 不可用**（WSL `unexpected memory mapping`），
给不出运行期现场。

---

## 7. 场景六：`defer` 回调的异常被**记录**而非传播

### 7.1 为什么不能传播

`~defer()` 是 `noexcept`。若回调抛异常且正在栈展开中，直接 `std::terminate`。
所以 `CallableModel::run()` 把它抓下来交给 `RecordDeferException()`。

### 7.2 怎么取出来

```cpp
    ClearDeferException();
    {
        defer catches_throw([] { throw std::runtime_error("defer callback"); });
    }
    GO2CPP_CHECK(LastDeferException() != nullptr);
```

**语义上这是一个"最近一次 defer 异常"的槽位**（线程局部），
由使用者负责检查与清理。测试还验证了 `ClearDeferException()` 之后取回空。

### 7.3 测试里的一处噪声（T-13）

同一段测试接着写了：

```cpp
    bool ordinary_exception_seen = false;
    try {
        throw std::logic_error("ordinary exception");
    } catch (const std::logic_error&) {
        ordinary_exception_seen = true;
    }
    GO2CPP_CHECK(ordinary_exception_seen);
```

**这个断言恒为 true**——它只验证了 C++ 语言本身的 try/catch，
无论被测模块对不对都会通过，**无法证伪任何实现**。
作者意图应是"验证 defer 记录的异常不污染普通异常处理"，但写法没做到。

---

## 8. 实现细节：`PanicState` 与并发

```cpp
struct PanicState {
    mutable std::mutex mutex;
    PanicInfo info;
    bool active{false};
    bool recovered{false};
};
```

**所有字段访问都在 `mutex` 下**：`active()` / `take()` / `operator()()` /
`peek()` / `clear()` / `call_info()` 无一例外。
这使 `panic`/`recover` **可以安全地跨线程使用**（与场景四一致）。

**各方法对边界条件的态度不同**，这一点设计得很有分寸：

| 方法 | 检查 `InDeferCallback()` | 理由 |
|---|---|---|
| `take()` | **是** | 消费型操作，必须守住恢复边界 |
| `operator()()` | **是** | 同上 |
| `peek()` | **否** | **只读不改**，用于诊断 |
| `active()` | **否** | 只读，且要支持跨线程查询 |
| `clear()` | **否** | 由持有者主动重置 |

**`take()` 的消费是 move 语义**：

```cpp
    PanicInfo result = std::move(state->info);
    state->info = {};
    state->active = false;
```

—— 状态被**搬走**后原位清空，保证不会被消费两次。

---

## 9. 测试证据与盲点

### 9.1 已覆盖

| 断言 | 锁定 |
|---|---|
| `order == [3, 2, 1]` | `defer` 的 **LIFO** 顺序 |
| `order[0] == 3` | 参数**注册时求值** |
| `run_count == 1` + `!once.active()` | `run_now()` **幂等** |
| `!dismissed_ran` | `dismiss()` **取消**执行 |
| `unwound_ran`（异常展开路径） | `defer` 在**栈展开**时也执行 |
| 边界外 `!take()` / 边界内 `take()` 成功 | **恢复边界** |
| `!pa.active()` + `pa.recovered()` | 消费后状态迁移 |
| `recovered_info.message/code` | `PanicInfo` 字段完整 |
| `callable_panic.recovered()` + `!info().valid()` | `operator()()` 的清理 |
| `LastDeferException() != nullptr` → `Clear` 后为空 | defer 异常槽位 |
| 跨线程 `active()` 安全 | 跨线程可见性 |

### 9.2 盲点（T-14）

四个公开接口零覆盖：

- **`panic::clear()`** —— 会重置 `payload`/`message`/`code`/`raised`/`active`/`recovered` 六个字段；
- **`recover::bind()`** —— 返回值语义未验证（**绑定时 `panic` 已析构会返回 false**）；
- **`recover::reset()` / `bound()`**；
- **`recover::peek()`** —— 它与 `take()` 的关键差异是**不检查边界**，
  即"可在 defer 边界外读取状态"，这一有意设计没有被固化。

### 9.3 文件职责问题（T-12）

`test_control_flow.cpp` 还 `#include "go2cpp/channel.hpp"`，
其**后 60 行（`:118-177`）全部在测 `Channel`/`Select`/`SelectValue`/`SelectCaster`**
（`MakeChannel`、`RecvCase`/`SendCase`、`MakeCaster`、`SelectProbe::SetAny`、
`SelectResult::Cast`、move-only 的 `Take`/`TakeValue`、内建 Select 拒绝 move-only）。

而 `announce` 名是 `"defer/panic/recover"`，过滤器名也只有 `control_flow`/`defer`——
**于是 `GO2CPP_TEST_FILTER=control_flow` 会连带跑 Select 测试，
而 `filter=channel` 跑不到它们。**

---

## 10. 示例评述（`example/tools/control_flow_demo.cpp`，94 行）

三个场景：

| 场景 | 内容 |
|---|---|
| 一 | `defer` 参数求值与 LIFO（注释点明"用值参数便于新手观察注册时求值"） |
| 二 | defer 边界内 recover，边界外拒绝 |
| 三 | move-only 值放入内建 Channel Select 被拒绝（`kInvalid`） |

**优点**：注释主动解释了设计意图，对新手友好——
"panic 是显式状态发布；C++ 不会自动跳转，业务代码应在这里 return、分支"
（`:63-64`）；"不会伪造 longjmp"（`:72-73`）。

**一个运行副作用**：它写日志到 `log/control_flow_demo.log`（`:37-38`），
**运行示例会改动 `log/` 目录**——在质量审计流程中这与"`log/` 基线 md5 必须保持一致"
的约束冲突，因此本流程**只做静态审读、不运行 example**。

---

## 11. 问题清单

| 编号 | 严重度 | 摘要 |
|---|---|---|
| **X-30** | **中（契约违反）** | 恢复边界用 `thread_local`，同线程多 Fiber 下会跨 Fiber 泄漏；而头文件 `:199-200` **明确承诺"其他线程或 Fiber 不能越过"** |
| T-12 | 低 | `test_control_flow.cpp` 后 60 行实际在测 `Channel`/`Select`，与文件归属和 announce 名不符 |
| T-13 | 低 | `:97-103` 的 `ordinary_exception_seen` 断言恒为 true，无信息量 |
| T-14 | 低 | `panic::clear()`、`recover::bind()`/`reset()`/`bound()`/`peek()` 零覆盖 |

---

## 12. 一页总结

| 维度 | 结论 |
|---|---|
| **设计原点** | `panic` **不做栈跳转**，只发布状态——换取"C++ 析构永远正确" |
| **运行时用途** | **零使用**。它是**为使用者（转译代码/业务代码）准备的 API** |
| **`defer` 本质** | 类型擦除的 RAII 对象；LIFO 顺序由 C++ 析构顺序**免费提供** |
| **`defer` 参数** | **注册时按值保存**（与 Go 一致）；想延迟求值请捕获引用 |
| **`defer` 异常** | 回调异常被 `RecordDeferException` **记录**而非传播（析构不能抛） |
| **所有权** | `panic` 持 `shared_ptr`（拥有）；`recover` 持 `weak_ptr`（不延长寿命） |
| **恢复边界** | 由 `InDeferCallback()` 守门；`peek()`/`active()`/`clear()` 有意不检查 |
| **跨 Fiber 迁移** | `panic` 状态**可迁移**（`shared_ptr` + `mutex`），头文件明确承诺 |
| **最严重缺陷** | **边界是线程级而非 Fiber 级**，与头文件承诺直接矛盾（X-30） |
| **与 Go 的最大差别** | panic 无人 recover 时**程序不崩溃**，只是留下一个状态对象 |