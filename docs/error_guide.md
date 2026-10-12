# Go2Cpp `Error` 模块深度讲解（场景驱动）

> **范围**：本文只讲 `error` 一个模块。
> **方法**：以「它在什么场景下被谁用、怎么用、为什么这样设计」为主线，
> 配合真实调用点代码、实现细节与测试证据。
> **引用约定**：以符号/代码片段为主定位，行号仅作参考（用户在途重排会移动行号）。
>
> 源码：`include/go2cpp/tools/error.hpp`(250) + `src/tools/error.cpp`
> 测试：`tests/tools/test_error.cpp`(125)、`tests/tools/test_errors_ns.cpp`(94)
> 示例：`example/tools/errors_demo.cpp`(54)

---

## 0. 一句话定位

`Error` 是 **Go `error` 接口值的 C++ 等价物**：一个**可为空**、**不可变**、
**可沿链查找**的错误值。它的全部设计都围绕一句话展开
（`error.hpp:17-20`）：

> ErrorPtr 是可为空的 Go error 接口等价物。**错误实现构造后必须保持不可变**，
> 因此共享 ErrorPtr 可以安全地在线程和 Fiber 之间传递。

**"不可变"这一个词，撑起了后面所有场景**：因为错误值不会变，
`ErrorPtr` 可以被任意线程、任意 Fiber 无锁地复制、入队、跨边界投递。
整个运行时里到处把 `ErrorPtr` 塞进队列和回调，靠的就是这一条。

---

## 1. 场景地图：它在运行时到底被用来干什么

这是**先看用量再讲设计**。对 `src/` 全量扫描各 API 的出现次数：

| API | 出现次数 | 说明 |
|---|---:|---|
| `ErrorPtr` | 75 | 绝大多数是**类型标注**——它是贯穿全运行时的"错误载体" |
| `Is(` | 9 | **真实主用途**：`Is(err, DeadlineExceededError())` |
| `DeadlineExceededError()` | 9 | 与上一行配对 |
| `NewError(` | 8 | 只在 4 个文件里创建错误 |
| `As<` | **0** | **运行时从不使用**（仅测试/示例） |
| `Wrap(` | **1** | 运行时几乎不用 |
| `Join(` | 4 | 少量 |
| `WalkErrors(` | **0** | 运行时从不使用（仅测试） |

**这张表本身就是结论**：

> `Error` 对外提供的是一套完整的 Go 风格错误树工具（包装、聚合、类型提取、遍历），
> 但**运行时真正依赖的只有一个能力：区分「超时」和「取消」**。
> 其余 API 是**为使用者（转译代码、业务代码、插件）准备的**，不是运行时自身的需要。

下面逐个场景展开。

---

## 2. 场景一（最重要）：区分「超时」与「取消」

### 2.1 问题

Go2Cpp 里，几乎每个可阻塞的 API 都接受一个 `ContextPtr`：
`channel` 的收发、`io` 的 `wait`、`future` 的等待、`sse` 的写入……

这些 API 在阻塞期间可能因为**两种不同原因**被唤醒：

- **取消**：有人主动 `cancel()`（用户放弃、上游关闭、`shutdown()`）
- **超时**：`WithTimeout`/`WithDeadline` 的期限到了

**这两者必须给调用方不同的反馈**——因为调用方的处置完全不同：
超时通常意味着"重试/降级"，取消意味着"别等了，收工"。
在 POSIX 层面它们是 `ETIMEDOUT` 与 `ECANCELED`；在 `Go` 里它们是
`context.DeadlineExceeded` 与 `context.Canceled`。

### 2.2 它怎么做的

`Context` 把原因记录为一个 `ErrorPtr`（`Context::Err()`），
两个标准原因是**进程级单例**（`context.cpp`）：

```cpp
ErrorPtr CanceledError() {
    static const ErrorPtr error = NewError("context canceled");
    return error;
}
ErrorPtr DeadlineExceededError() {
    static const ErrorPtr error = NewError("context deadline exceeded");
    return error;
}
```

调用方则用 **`Is`** 判别：

```cpp
Is(context->Err(), DeadlineExceededError())
```

### 2.3 三个真实调用现场（配合使用的代码）

**① `channel` —— 把 Context 错误映射为 Channel 状态码**
（`src/concurrency/channel.cpp`，`Select()` 内）

```cpp
    // 已就绪的 channel 操作与取消之间存在竞争，语义与 Go 的 select 一致。
    // 本轮没有 channel case 获胜时，在 default 或休眠前检查取消状态。
    if (context && context->IsDone()) {
      const auto err = context->Err();
      const bool deadline_error = err && Is(err, DeadlineExceededError());
      return {SelectResult::kNoSelection, false, false, {},
              deadline_error ? ChannelStatus::kDeadlineExceeded
                             : ChannelStatus::kCancelled,
              err ? err : CanceledError()};
    }
```

**看点**：这里写成 `err && Is(err, ...)`，即**先判空再问**。
这不是多余的——它同时给出了"没有错误时"的兜底：`err ? err : CanceledError()`。

**② `io` —— 把 Context 错误映射为 errno**
（`src/network/io.cpp`，`IOManager::wait` 内）

```cpp
    if (context && context->IsDone()) {
        const bool timed_out = Is(context->Err(), DeadlineExceededError());
        return {timed_out ? WaitStatus::kTimeout : WaitStatus::kCancelled,
                timed_out ? ETIMEDOUT : ECANCELED};
    }
```

**看点**：这里**没有**先判空。这是安全的，因为 `Is(nullptr, X)` 会返回 `false`
（走的是"空 target 才匹配空错误"的另一条分支，见场景三）——
即**空 err 自然落进"取消"分支**，符合直觉。

**③ `sse` —— 把 Context 错误映射为模块自己的枚举**
（`src/network/sse.cpp`，`Stream::cancellation_result`）

```cpp
Result Stream::cancellation_result() const {
    if (!m_state || m_state->cancelled.load(std::memory_order_acquire) ||
        (m_context && m_context->IsDone())) {
        if (m_context) {
            const ErrorPtr error = m_context->Err();
            if (error && Is(error, DeadlineExceededError())) {
                return invalid_result(Error::DeadlineExceeded, "SSE Context 已到期");
            }
        }
        return invalid_result(Error::Cancelled, "SSE Context 已取消");
    }
```

**三个现场的模式完全一致**：

```
Context::Err()  →  Is(err, DeadlineExceededError())  →  模块自己的状态码 / errno
```

**这就是 `Error` 在运行时的核心职责**：一个**类型安全的错误判别信号**，
让每个模块都能把"为什么被唤醒"翻译成自己的词汇表。

### 2.4 为什么不用枚举？

一个自然的疑问：既然只是区分两种原因，为什么不用 `enum class CancellationReason`？

因为 `Context` 的错误还要**向外传播**：调用方可能把它一路返回给自己的调用者，
可能加上自己的上下文（`Wrap`），可能和别的失败聚合（`Join`），
最终展示给用户。用一个**富错误值**承载，比用一个枚举位更有表达力，
且能在不改变签名的情况下扩展（例如将来加"因父 Context 取消"）。

**这是"可扩展性优先于最小实现"的取舍**——代价是运行时多了一次 shared_ptr 拷贝
（单例使这个代价降到极低）。

---

## 3. 场景二：单例 + 身份比较 —— 整个机制的枢纽

### 3.1 实现细节

`Is` 能判断"这个错误是不是超时错误"，靠的是 **`Error::Is` 的默认实现是身份比较**：

```cpp
  virtual bool Is(const Error& target) const noexcept {
    return this == &target;      // error.hpp —— Error 的默认实现
  }
```

**搭配** `DeadlineExceededError()` 返回**同一个对象**，于是
`Is(err, DeadlineExceededError())` 实际是在问"**err 是不是就是那个超时单例**"。

**如果 `DeadlineExceededError()` 每次返回新对象，`Is` 将永远返回 false** ——
整套超时/取消判别会静默失效，且**不会报错**。这是本模块最脆弱的接缝。

### 3.2 全仓库的单例清单（8 个）

| 单例 | 位置 | 文本 |
|---|---|---|
| `CanceledError` | `context.cpp` | "context canceled" |
| `DeadlineExceededError` | `context.cpp` | "context deadline exceeded" |
| `ChannelClosedError` | `channel.cpp` | "channel closed" |
| `ChannelAlreadyClosedError` | `channel.cpp` | "channel already closed" |
| `ChannelWouldBlockError` | `channel.cpp` | "channel operation would block" |
| `ChannelNilError` | `channel.cpp` | "nil channel" |
| `ChannelTimeoutError` | `channel.cpp` | "channel operation timed out" |
| `BrokenPromiseError` | `future.cpp` | "go2cpp future promise was abandoned" |

`future.cpp` 里写明了采用单例的理由：

```cpp
ErrorPtr BrokenPromiseError() {
    // 共享不可变错误对象，避免每个未完成 Promise 析构时重复分配。
    static const ErrorPtr error =
        NewError("go2cpp future promise was abandoned");
    return error;
}
```

**"避免重复分配"** —— 因为 `BrokenPromiseError()` 会在**每个未完成的 Promise 析构时**
被调用，是热路径。

### 3.3 自定义错误如何参与

用户可以派生 `Error` 并**重写 `Is`** 来定义自己的匹配语义
（`error.hpp` 的注释：「自定义错误可以重写此函数，实现 Go 的 Is 判断」）。
`test_error.cpp` 里的 `TaggedError` 就是这一用法的演示——它用 `As<TaggedError>`
做类型提取，而非重写 `Is`。

---

## 4. 场景三：nil 语义 —— `Is(err, nullptr)`

### 4.1 语义

`Is(error, target)` 在 `target` 为空时**退化为"error 是否为空"**：

```cpp
// src/tools/error.cpp 的 Is() 内
if (!target) {
    return !error;
}
```

头文件已声明（`error.hpp`）：

> 遍历单原因或多原因错误链。允许并忽略环；**空 target 只匹配空错误，
> 与 errors.Is(err, nil) 相同**。

### 4.2 与 Go 的对应

Go 里 `errors.Is(err, nil)` 返回 `err == nil`。这是**有意对齐**的，
所以 Go2Cpp 的 `Is` 有三个语义区域：

| 调用 | 语义 |
|---|---|
| `Is(err, target)`，两者都非空 | 沿链做**身份匹配** |
| `Is(err, nullptr)` | 等价于 `!err` |
| `Is(nullptr, nullptr)` | `true` |

`test_errors_ns.cpp` 的 `check_null_target_semantics()` 把这三条都固化了。

### 4.3 一次"登记后又被推翻"的记录

这一条曾被登记为文档缺失（台账 **D-14**），理由是
「头文件只有裸声明、无任何注释」。后来 `git blame` 证明
`error.hpp:131-134` 的这段注释由提交 `d0fdf70`（"fix: complete full module
boundary audit"，2026-10-10 12:52）加入，**D-14 的前提不成立，已结案**。

**教训**：`tests/tools/test_errors_ns.cpp` 里那段声称"头文件无注释"的测试注释
**本身就是当时的判断依据**，而它是错的。以测试注释为证据去登记文档缺陷，
等于把二手信息当一手证据——这条已登记为 **T-09**。

---

## 5. 场景四：包装链 —— 给错误加上下文（`Wrap`）

### 5.1 意图

Go 的 `fmt.Errorf("read config: %w", err)` 在保留原因的同时追加一层描述。
`Wrap` 对应它：

```cpp
const auto base = NewError("disk unavailable");
const auto wrapped = Wrap(base, "read config");
ErrorMessage(wrapped);   // "read config: disk unavailable"
Is(wrapped, base);       // true —— 仍能沿链找到原因
```

关键在于 **`Is` 会沿链下降**：包装之后，原来的身份匹配依然成立。
这正是"按身份而非按消息比较"的价值——`test_error.cpp` 断言
`!Is(wrapped, NewError("disk unavailable"))`，即**消息相同也不匹配**。

### 5.2 实现细节：`Message()` 是拼接的

`WrappedError::Message()` 返回 `"当前层: 原因"`。
`ErrorMessage()`（自由函数）负责空安全地取文本。

### 5.3 现实：运行时只用了一次

全量扫描显示 `Wrap(` 在 `src/` 中**只出现 1 次**。
也就是说：**这套包装能力主要是给使用者（转译后的 Go 代码、业务代码）准备的**，
运行时自身倾向于用**单例错误 + 状态码**表达问题，
而不是构造包装链——因为热路径上少一次分配更重要。

**这是"接口面向使用者、实现面向性能"的典型分工。**

---

## 6. 场景五：聚合多个失败（`Join`）

### 6.1 意图与陷阱

`Join` 把多个错误合成一个（类似 Go 1.20 的 `errors.Join`）：

```cpp
const auto joined = Join({wrapped, tagged});
ErrorMessage(joined);   // "read config: disk unavailable\ntagged"
Is(joined, tagged);     // true
```

**陷阱**：`Join({single})` **不会退化成那一个错误**，而是仍返回 `JoinError`。
`test_error.cpp` 专门断言了这一点：

```cpp
    const auto singleton_join = Join({base});
    GO2CPP_CHECK(singleton_join &&
                 dynamic_cast<const JoinError*>(singleton_join.get()) != nullptr);
```

**为什么值得固化**：如果实现"顺手优化"成单元素直接返回该元素，
依赖 `UnwrapAll()` 遍历全部原因的下游代码行为会变。
**类型稳定性本身是契约。**

**边界**：`Join({})` 返回空 `ErrorPtr`（`:51` 断言）。

### 6.2 实现细节：它是"图"不是"链"

`Join` 让错误从**单链**变成**有向图**（一个 `JoinError` 有多个 cause，
每个 cause 又可以是 `JoinError`）。这带来两个必须处理的后果：
**环**和**深度**。见下一节。

---

## 7. 场景六：类型化提取（`As<T>`）—— 运行时零使用，但设计完备

### 7.1 意图

`As<T>(err)` 沿错误图找**第一个能 `dynamic_cast` 到 `T` 的节点**：

```cpp
class TaggedError final : public go2cpp::Error { int m_tag; ... };
const auto found = As<TaggedError>(joined);
found->tag();   // 7
```

对应 Go 的 `errors.As`，用于把业务错误从包装里"捞出来"。

### 7.2 实现细节：迭代 + 地址去重 + noexcept 兜底

`AsImpl`（`error.hpp`）用**显式容器**而非递归：

```cpp
  std::vector<ErrorPtr> pending;
  pending.push_back(error);
  while (!pending.empty()) {
    ErrorPtr next = std::move(pending.back());
    pending.pop_back();
    if (!next || !visited.insert(next.get()).second) {
      continue;                        // 环：访问过就跳过
    }
    ...
  }
```

两条后果都被测试**用极端数据**固化了：

- **10 万层深链不爆栈**：`test_error.cpp` 真的构造 100,000 层 `Wrap`，
  再断言 `Is` / `As<StringError>` / `ErrorMessage` 全部正确，
  并且**精确校验格式化后的长度**；
- **自指错误图不死循环**：`CycleError` 的 `Unwrap()` 返回自己，断言 `!Is` 且 `!As`。

外层还有 `noexcept` + `try/catch` 兜底：

```cpp
template <typename T>
std::shared_ptr<const T> As(const ErrorPtr& error) noexcept {
  try {
    std::unordered_set<const Error*> visited;
    return detail::AsImpl<T>(error, visited);
  } catch (...) {
    // Is/As 是查询型 noexcept API；扩展 Error 的解包、哈希容器分配失败
    // 都必须转换为"未匹配"，不能让异常穿过边界并触发 terminate。
    return {};
  }
}
```

**这是全项目 noexcept 查询接口的正面范本**——
用户自定义的 `Error` 子类可以让 `Unwrap()`/`UnwrapAll()` 抛异常，
`As` 必须把它们变成"未匹配"而不是让进程 `terminate`。
`test_error.cpp` 用 `ThrowingUnwrapError` 验证了 `Is`/`As`/`Unwrap` 三条路径。

> 这条范本之所以重要，是因为项目里**有几处漏做了同样的兜底**
> （台账 X-25 记录了 7 处 `noexcept` 路径下可能抛出的加锁/锁操作）。

### 7.3 高危兄弟：`AsMutable<T>`

`AsMutable` 用 `const_pointer_cast` 返回**可变**的 `shared_ptr<T>`：

```cpp
template <typename T>
std::shared_ptr<T> AsMutable(const ErrorPtr& error) noexcept {
  // 显式适配出口：ErrorPtr 被共享时修改返回对象并不安全。普通调用方应
  // 使用 As<T>()，并保持错误值不可变。
  ...
  return std::const_pointer_cast<T>(As<T>(error));
}
```

**它与"错误必须不可变"的全局契约直接冲突**——注释自己都在劝阻使用。
它存在的原因应是兼容某个需要可变错误的外部接口。
**该接口零测试覆盖**（台账 T-11）。

---

## 8. 场景七：插件遍历（`WalkErrors`）—— 运行时零使用

### 8.1 意图

`As`/`Is` 解决"找不找得到"，`WalkErrors` 解决"**全部看一遍**"：
日志、指标、错误转换器需要统一遍历整张错误图。头文件写明：

> 该函数是 Is/As 之外的插件扩展出口，日志、指标和错误转换器可以复用
> **统一的环安全遍历规则**，不需要依赖 WrappedError/JoinError。

它的返回约定是"**visitor 全部返回 `kContinue` 且未抛异常**"才为 `true`。

### 8.2 边界情况：空 visitor

`test_error.cpp` 有一条容易被忽略的断言：

```cpp
GO2CPP_CHECK(!WalkErrors(joined, ErrorVisitor{}));
```

传空 `std::function` 时，调用会抛 `std::bad_function_call`，
被函数的 `noexcept` 兜底捕获 → 返回 `false`。
**行为是安全的，返回 false 也合理**，但头文件注释只说了
"访问器返回 `kStop` 或抛出异常时返回 false"，没有明写"空 visitor"这一条
（台账 **D-25**）。**行为已被测试固化，缺的只是文档一句话。**

---

## 9. 场景八：跨线程 / 跨 Fiber 无锁传递

### 9.1 为什么可以不加锁

`ErrorPtr` 是 `shared_ptr<const Error>`。因为**错误对象构造后不可变**，
多个线程/Fiber 同时持有同一个 `ErrorPtr` 并读取它是**数据竞争的定义之外**
（没有写入）。这使得：

- 错误可以**入队**（`Scheduler` 的完成通知、`IOManager` 的等待结果）；
- 错误可以**跨 Fiber 边界投递**（`Future` 的 `SetError` → 另一 Fiber 的 `GetResult`）；
- 错误可以**被多个观察者共享**（`WalkErrors` 的 visitor 拿到的是 `const ErrorPtr&`）。

**如果错误可变，上面每一条都需要额外同步**——这是"不可变"这条契约的回报。

### 9.2 唯一的例外

`AsMutable` 打破了它（见 7.3），因此被注释标为"显式适配出口"。

---

## 10. 测试证据汇总

### 10.1 `tests/tools/test_error.cpp` 的 10 条关键断言

| # | 断言 | 锁定的是什么 |
|---|---|---|
| 1 | `!Is(wrapped, NewError("disk unavailable"))` | **按身份而非消息**比较 |
| 2 | `Is(singleton_join, base)` + `dynamic_cast<JoinError*>` | `Join` 单元素**不退化** |
| 3 | `!Join({})` | 空聚合返回空 |
| 4 | `visited.size() == 4U` | `WalkErrors` **图去重**后恰好 4 个对象 |
| 5 | `!WalkErrors(joined, kStop)` | 主动停止 → 返回 false |
| 6 | `!WalkErrors(joined, ErrorVisitor{})` | **空 visitor** → 返回 false |
| 7 | `ThrowingUnwrapError` 下 `!Is` / `!As` / `!Unwrap` | **noexcept 边界** |
| 8 | 自指 `CycleError` → `!Is` / `!As` | **环安全** |
| 9 | 10 万层 `Wrap` 的**精确长度** + 尾部内容 | 迭代实现 + 格式化精度 |
| 10 | 10 万层嵌套 `Join` 的长度 + 尾部内容 | 同上（聚合路径） |

### 10.2 `tests/tools/test_errors_ns.cpp`

覆盖 `errors::` 命名空间这层 Go 风格薄封装：`New`/`Wrap`/`Join`（两种重载）/`As`/
`Is`/`Unwrap`，外加**身份语义**与**空 target 语义**各一组。

**这层封装的来历**：它是用户在途改动新增的 API，台账 **T-05**（"新增的
`errors::*` 命名空间无测试、无示例"）按 **U-01** 的默认建议"**保留**该 API
并补最小测试与示例"处理，于是新增了本测试与 `errors_demo.cpp`。

### 10.3 覆盖盲点（已登记 T-11）

- **`AsMutable<T>()`** —— 高危接口，零覆盖；
- **`errors_is()` / `errors_as<T>()`** —— 两个 `inline` 小写别名，零覆盖；
- **`WalkErrors` 的 visitor 抛异常**（只测了 `kStop` 与空 visitor）；
- `Error::ErrorString()` 别名；`JoinError::Message()` 对空/单元素 cause 的处理。

---

## 11. 示例（`example/tools/errors_demo.cpp`，54 行）

5 个场景，与测试互补——它演示"怎么用"而不是"对不对"：

| 场景 | 内容 |
|---|---|
| 一 | 构造与包装（成功路径） |
| 二 | 身份语义：消息相同但对象不同 → `Is` 不匹配 |
| 三 | 聚合：initializer_list 与 vector 两种重载；空输入返回空 |
| 四 | 类型提取：`As` 命中与未命中（未命中返回空而不抛异常） |
| 五 | 边界：`Unwrap` 逐层下降到底返回空，不死循环 |

`example/README.md` 有对应登记行。

**唯一问题**：与下面的 T-10 相同——它同样调用 `errors::`，
而该命名空间**尚未提交**，因此从 HEAD 构建会失败。

---

## 12. 本模块的问题清单

| 编号 | 严重度 | 摘要 |
|---|---|---|
| **T-10** | **高** | **HEAD 无法构建**：`test_errors_ns.cpp` 与 `errors_demo.cpp` 已提交，但它们依赖的 `errors::` 命名空间**只在用户在途改动中** |
| **X-31** | 低 | 8 个错误单例用函数局部 `static`，程序退出阶段被析构后返回**空 `ErrorPtr`**，使 `Is(err, XxxError())` 退化为 `!err`，**超时会被静默误判为非超时** |
| T-09 | 低 | `test_errors_ns.cpp` 的注释称"头文件只有裸声明、没有注释"，与事实相反；且引用已过时行号 |
| T-11 | 低 | `AsMutable`、`errors_is`、`errors_as` 三个公开接口零覆盖 |
| D-25 | 低 | `WalkErrors` 注释未声明**空 visitor** 的行为（行为已被测试固化） |
| D-14 | — | **已结案**：`Is` 的空 target 语义注释由 `d0fdf70` 加入，前提不成立 |

### X-31 详述（本轮为写本文而做全仓库扫描时发现）

8 个单例都是这个形状：

```cpp
ErrorPtr DeadlineExceededError() {
    static const ErrorPtr error = NewError("context deadline exceeded");
    return error;
}
```

函数局部 `static` 会在**程序退出阶段被析构**。析构之后该函数返回**空 `ErrorPtr`**，
而 `Is(err, {})` 的语义是 **`!err`** —— 于是：

```
本意：Is(ctxErr, DeadlineExceededError())  →  判断"是不是超时"
退化：Is(ctxErr, {})                       →  变成"err 是不是空"
后果：超时被静默判为「非超时」，走取消分支
```

**项目其实认识这个风险**——`include/go2cpp/tools/go.hpp` 对默认调度器
**刻意用 `new` 且永不析构**：

> 默认调度器故意采用进程生命周期句柄，**避免静态析构顺序导致工作线程
> 访问已销毁对象**。

**但 8 个错误单例没有做同样处理。** 正常路径安全（`Scheduler::shutdown()`
一般会先停线程），但 `shutdown_for()` **超时会返回 false 且线程可能仍在运行**，
此时恰好发生 Context 超时即可命中。

**修复建议**：改用 `static const ErrorPtr* const error = new ErrorPtr(...)`
（同 `go.hpp` 的做法），或 `leaky singleton` 惯用法。

---

## 13. 一页总结

| 维度 | 结论 |
|---|---|
| **核心契约** | 错误值**不可变** → `shared_ptr<const Error>` 可无锁跨线程/Fiber 共享 |
| **运行时真正用途** | **只用一件事**：`Is(err, DeadlineExceededError())` 区分超时/取消（3 个现场：channel / io / sse） |
| **枢纽机制** | 标准错误是**进程级单例** + `Error::Is` 默认**按地址比较**；二者缺一则超时判别静默失效 |
| **完整能力** | 包装链 / 聚合图 / 类型提取 / 插件遍历——**面向使用者与插件**，运行时基本不用 |
| **算法要点** | 遍历**迭代**且**按地址去重** → 10 万层不爆栈、环不死循环 |
| **安全边界** | `Is`/`As` 是 `noexcept` 且**真的兜住**用户自定义错误的异常（全项目正面范本） |
| **最脆弱处** | 单例若返回新对象，判别失效且**不报错**；单例若在退出阶段被析构，判别**语义反转** |
| **待办** | T-10（高，需用户提交在途文件）、X-31、T-09、T-11、D-25 |
