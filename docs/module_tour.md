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
