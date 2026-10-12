# Go2Cpp 模块讲解（按依赖顺序）

> 本文由源码回溯审计过程产出。结论一律以 `路径:行号` 或符号名可追溯；
> 行号仅作参考（用户在途重排会移动行号），**以符号/代码片段为主定位**。
>
> 阅读约定：**源码已读** = 该文件 100% 逐行读过；**测试已读** = 测试文件逐行读过。

---

## 模块 1 / 基础工具层：`error`

**源码**：`include/go2cpp/tools/error.hpp`(250) + `src/tools/error.cpp`
**状态**：源码已读、测试已读
**位置**：无依赖，被几乎所有模块引用（`#include "go2cpp/error.hpp"` 走根目录转发头）

### 1.1 它解决什么问题

Go 的 `error` 是一个**接口值**：可以为 nil、可以被包装、可以用 `errors.Is`/`errors.As`
**沿链查找**。C++ 的 `std::error_code`/异常都不直接对应这套语义。本模块用
`std::shared_ptr<const Error>` 作为「可为空的 Go error 接口等价物」
（`error.hpp:17-20`），并把「不可变」定为硬契约：

> ErrorPtr 是可为空的 Go error 接口等价物。**错误实现构造后必须保持不可变**，
> 因此共享 ErrorPtr 可以安全地在线程和 Fiber 之间传递。

这条不可变性是后面一切的前提 —— 它使 `ErrorPtr` 可以**无锁跨线程/Fiber 传递**，
不需要任何同步。

### 1.2 类层次（4 个实体类 + 1 个遍历工具）

| 类型 | 角色 | 关键点 |
|---|---|---|
| `Error` | 抽象接口 | `Message()` 纯虚；`ErrorString()` 是 Go 风格别名（`:29`）；`Is()` 默认**按对象身份**比较（`:36-38`） |
| `StringError` | 叶子错误 | 只保存一条不可变文本 |
| `WrappedError` | 单 cause 包装 | `Message()` 返回当前层，`Unwrap()` 返回原因 |
| `JoinError` | 多 cause 聚合 | `UnwrapAll()` 返回全部原因 |
| `WalkErrors` | 遍历出口 | 面向日志/指标/错误转换插件 |

**设计要点**：`Is()` 的默认实现是 `this == &target` —— **按地址而非按消息**。
`test_error.cpp:36` 专门断言了这一点：`!Is(wrapped, NewError("disk unavailable"))`，
两条消息相同的错误互不匹配。这不是疏漏，而是包装链有意义的前提
（否则 `Wrap` 之后就无法区分「同一个原因」和「碰巧同文案的原因」）。

### 1.3 三个必须讲清的细节

**① 遍历是迭代的，且按地址去重。**
`As<T>` 的实现（`error.hpp:140-167`）用显式 `std::vector<ErrorPtr> pending`
栈 + `std::unordered_set<const Error*> visited` 做 BFS/DFS，**不使用递归**。
后果有两条，都被测试固化了：

- **10 万层深链不爆栈**（`test_error.cpp:96-109` 构造 100,000 层 `Wrap` 并断言
  `Is`/`As`/`ErrorMessage` 都正确）；
- **自定义错误图里的环不会无限循环**（`test_error.cpp:82-92` 的 `CycleError`
  自指，断言 `!Is` 且 `!As`）。

`WalkErrors` 的注释（`error.hpp:43-47`）把这条写成了对外承诺：
「遍历使用地址去重，因此自定义错误中的环不会导致无限循环」。

**② 所有查询型 API 都是 `noexcept`，且真的兜住了异常。**
`As<T>`（`:172-181`）是**正面范本**：

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

注释把理由写透了。测试 `test_error.cpp:68-79` 用 `ThrowingUnwrapError`
（`Unwrap`/`UnwrapAll` 都抛）验证 `Is`/`As`/`Unwrap` 三条路径都返回安全值。

**③ `Is(error, nullptr)` 退化为「error 是否为空」。**
实现里是 `if (!target) return !error;`（`src/tools/error.cpp` 的 `Is()` 内），
头文件 `error.hpp:131-134` 已声明：

> 遍历单原因或多原因错误链。允许并忽略环；**空 target 只匹配空错误，
> 与 errors.Is(err, nil) 相同**。

> 这条注释曾在台账里被登记为缺失（D-14）—— 后续 `git blame` 证明它由
> `d0fdf70`（2026-10-10 12:52）加入，**D-14 已结案**。

### 1.4 测试覆盖情况（`tests/tools/test_error.cpp` 125 行 + `test_errors_ns.cpp` 94 行）

**覆盖得相当扎实**，尤其下面这些通常会被漏掉的地方都有覆盖：

| 断言 | 位置 | 价值 |
|---|---|---|
| 消息相同但对象不同 → `Is` 不匹配 | `test_error.cpp:36` | 锁定「按身份」语义 |
| `Join` 单独一个元素仍返回 `JoinError` 而非退化 | `:46-49` | 锁定类型稳定性 |
| `Join({})` 返回空 | `:51` | 边界 |
| `WalkErrors` 图去重后恰好访问 4 个对象 | `:61` | 锁定去重行为 |
| `WalkErrors` 遇 `kStop` 返回 false | `:62-64` | 锁定返回值语义 |
| `WalkErrors` 传**空 visitor** 返回 false | `:65` | 锁定空 `std::function` 路径 |
| 自定义 `Unwrap` 抛异常时 `Is`/`As`/`Unwrap` 都安全 | `:76-79` | noexcept 边界 |
| 自指错误图不死循环 | `:88-92` | 环安全 |
| 10 万层深链的**精确格式化长度** | `:104-108` | 迭代实现 + 精度 |
| 10 万层嵌套 `Join` 的长度与尾部内容 | `:120-124` | 同上 |

**测试盲点（`test_error.cpp` 未覆盖）**：

- **`AsMutable<T>()` 零覆盖** —— 它用 `std::const_pointer_cast` 返回可变的
  `shared_ptr<T>`（`error.hpp:183-193`），与全局「错误不可变」契约存在张力，
  注释自己都说「普通调用方应使用 `As<T>()`」。**高危接口无测试**。
- **`errors_is()` / `errors_as<T>()`**（`error.hpp:195-202`）这两个 `inline`
  小写别名无覆盖；`test_errors_ns.cpp` 只测了 `errors::` **命名空间内**的同名函数。
- **`WalkErrors` 的 visitor 抛异常**（只测了 `kStop` 与空 visitor）——
  实现是 noexcept，值得固化。
- `Error::ErrorString()` 别名、`JoinError::Message()` 对空/单元素 cause 的处理。

### 1.5 本模块发现的问题

| 编号 | 严重度 | 摘要 |
|---|---|---|
| **T-10** | **高** | **HEAD 无法构建**：`test_errors_ns.cpp` 与 `errors_demo.cpp` 已提交，但它们依赖的 `errors::` 命名空间只在用户在途改动里 |
| **T-09** | 低 | `test_errors_ns.cpp:55-60` 的注释称「头文件只有裸声明、没有注释」，与事实相反；且引用了已过时行号 |
| **T-11** | 低 | `AsMutable`、`errors_is`、`errors_as` 三个公开接口零测试覆盖 |
| **D-25** | 低 | `WalkErrors` 注释未声明**空 visitor** 的行为（行为本身已被 `test_error.cpp:65` 固化，仅文档缺失） |

**T-10 的成因值得记一笔**：B-03 批次按台账 U-01 的默认建议「保留该 API 并补最小
测试与示例」行动，提交了测试与示例，**却没有提交它所依赖的头文件**——
因为该头文件是用户在途文件，AI 不得代为提交。于是仓库进入了一个
「测试已提交、被测代码未提交」的中间态。**这类跨文件依赖的提交必须成组考虑。**

### 1.6 示例（`example/tools/errors_demo.cpp` 54 行）

5 个场景，覆盖构造/包装、身份语义、聚合（含空输入）、类型提取（命中与未命中）、
边界（`Unwrap` 到底返回空）。**场景划分合理**，且 `example/README.md:60` 有对应登记行。

唯一问题是与 T-10 相同的依赖：它同样调用 `errors::`，因此 HEAD 版本编译不过。

---

## 模块 2 / 基础工具层：`control_flow`

**源码**：`include/go2cpp/tools/control_flow.hpp`(241) + `src/tools/control_flow.cpp`(346)
**状态**：源码已读、测试已读（`tests/tools/test_control_flow.cpp` 177 行 + `example/tools/control_flow_demo.cpp` 94 行）
**位置**：仅依赖 `error` 的转发头与标准库；被 `src/tools/` 与示例使用

### 2.1 它解决什么问题

Go 的 `defer`/`panic`/`recover` 是一套**基于栈展开**的控制流。C++ 里 `defer` 可以用 RAII
天然表达（析构即执行），但 `panic`/`recover` 不能直接映射 —— 用 `longjmp` 会破坏
C++ 析构，用异常则要求用户代码到处写 try/catch，且异常穿过 Fiber 边界很危险。

Go2Cpp 的选择是：**把 panic 做成显式状态，不做栈跳转**。头文件说得非常直白：

> call() 只记录状态，不使用 longjmp 或破坏栈的跳转；用户应在 defer 回调中通过
> recover 读取和处理状态。（`control_flow.hpp:143-145`）

所以这里的 "recover" **不是**捕获并继续执行，而是「在 defer 回调里读走那个状态对象」。
调用方必须自己在 `panic` 之后 return / 分支 —— 这一点示例里也点明了
（`control_flow_demo.cpp:63-64`）。

### 2.2 三个组件

| 组件 | 角色 | 关键实现 |
|---|---|---|
| `defer` | RAII 作用域回调 | 类型擦除的 `Callable`/`CallableModel`（`control_flow.hpp:88-106`），`run() noexcept` 内部 try/catch |
| `panic` | 显式状态容器 | `shared_ptr<PanicState>`；`call()` 只是写状态 |
| `recover` | 状态读取句柄 | 持 **`weak_ptr`**（`control_flow.hpp:231`），所以**不延长 panic 生命周期** |

**`defer` 的参数语义**：参数在**构造时按值保存**（`MakeCallableWithArgs` 把函数与
`std::tuple` 一起捕获进 lambda，`control_flow.hpp:115-132`）。测试 `:25-31` 专门验证：
`captured` 注册时 `evaluated == 3`，之后改成 9，回调仍收到 **3**。
执行顺序是 C++ 作用域的**逆序**（LIFO），测试断言 `order == [3, 2, 1]`。

**`defer` 的异常处理**：回调体被包在 `CallableModel::run() noexcept` 里
（`control_flow.hpp:97-103`），异常被 `RecordDeferException()` 记录而**不是传播** ——
否则会从析构函数里抛出。用户要取这次异常得主动调 `LastDeferException()`。

**`recover` 用 `weak_ptr` 是刻意的**：`bound()` 用 `!m_state.expired()` 判断
（`control_flow.hpp:219`），`take()`/`active()`/`peek()` 都先 `m_state.lock()`。
这样 `panic` 先析构时，`recover` 会安静地失效，而不是把状态对象钉在内存里。

### 2.3 恢复边界的守门条件（本模块的核心设计）

`recover::take()` 与 `recover::operator()()` 的**第一件事**是：

```cpp
if (!detail::InDeferCallback()) {
    return std::nullopt;      // take()
    return false;             // operator()
}
```

`InDeferCallback()` 就是 `s_defer_depth != 0`（`control_flow.cpp:38`），
而 `s_defer_depth` 由 `DeferCallbackScope` 在回调执行期间递增、结束递减
（`:28-32`）。也就是说：**panic 只能在 defer 回调执行期间被消费** ——
这正是「恢复边界」的实现。测试 `:63-74` 验证了边界内 `take()` 成功、
**边界外 `take()` 返回 `nullopt`**。

**`peek()` 是唯一的例外**：它**不检查** `InDeferCallback()`
（对比 `:258` 与 `:313-320`），因为它只读不改，用作诊断。

### 2.4 ★ 本模块发现的问题

| 编号 | 严重度 | 摘要 |
|---|---|---|
| **X-30** | **中** | **恢复边界用 `thread_local` 而非 Fiber 局部，同线程多 Fiber 下会跨 Fiber 泄漏** |
| T-12 | 低 | `test_control_flow.cpp` 后 60 行测的是 `Channel`/`Select`，与文件归属和 announce 名不符 |
| T-13 | 低 | `:97-103` 的 `ordinary_exception_seen` 断言恒为 true，无信息量 |
| T-14 | 低 | `panic::clear()`、`recover::bind()`/`reset()`/`bound()`/`peek()` 无测试覆盖 |

#### X-30 详述

`control_flow.cpp:15-16` 定义了两个 **`thread_local`**：

```cpp
thread_local unsigned int s_defer_depth = 0;
thread_local std::exception_ptr s_defer_exception;
```

而 **Go2Cpp 的 Fiber 在同一 OS 线程上多路复用**（这是整个运行时的基础）。
`thread_local` 只能区分线程，**不能区分同线程的不同 Fiber**。于是：

1. Fiber A 进入某个 `defer` 回调 → `s_defer_depth` 变成 1；
2. A 在回调中 **park**（例如回调里等 channel、等锁、等 IO）→ M 被让给 Fiber B；
3. B 调用 `recover::take()` 或 `recover()` → 看到 `s_defer_depth == 1`
   → **被判为「在 defer 回调内」**，可以消费掉不属于它的 panic 状态。

`s_defer_exception` 同理：A 在回调中抛出的异常会被 B 通过 `LastDeferException()`
读到，B 的 `ClearDeferException()` 也会抹掉 A 的记录。

**最有说服力的地方在于：项目自己有正确的工具。**
`include/go2cpp/concurrency/fiber_local.hpp` 提供了 `FiberLocal<T>` ——
它在 Fiber 内返回 Fiber 局部值、在普通线程内回退为线程局部值，**正是本场景所需**。
`control_flow` 却用了 `thread_local`。

**触发条件**是「同线程 Fiber A 在 defer 回调内 park」，在 Fiber 运行时里并非罕见路径；
但需要构造特定交错，本环境 TSan 不可用（WSL `unexpected memory mapping`），
因此**作为登记项而非即时修复** —— 修改 `control_flow.cpp` 属行为性改动，
不满足「零行为变化」的即时修复例外。

### 2.5 示例评述

`example/tools/control_flow_demo.cpp` 三个场景：defer 的参数求值与 LIFO、
defer 边界内 recover（边界外拒绝）、move-only Select 的错误路径。
场景划分合理，且**注释里主动解释了「panic 是显式状态发布；C++ 不会自动跳转」**
（`:63-64`）与「不会伪造 longjmp」（`:72-73`），对新手友好。

一个小瑕疵：示例用 `LoggerManager` 写日志到 `log/control_flow_demo.log`
（`:37-38`），运行示例会改动 `log/` 目录 —— 在质量审计流程中这属于**运行副作用**，
需要与「`log/` 基线 md5 必须保持一致」的约束协调（本流程因此**不运行** example，
只做静态审读）。

---
