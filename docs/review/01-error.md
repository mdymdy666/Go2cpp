# 01 error 模块问题文档

> 基线：`bd99603`｜关键文件：`include/go2cpp/tools/error.hpp`（353 行）、`src/tools/error.cpp`（415 行）
> 阅读状态：已答（首轮）

## 1. 模块速览

- 职责：不可变错误链/错误图；`Is`/`As`/`Unwrap`/`WalkErrors` 查询；日志、配置、网络模块共用。
- 公共入口：`include/go2cpp/error.hpp`（4 行，转发到 `tools/error.hpp`）。
- 关键类型：
  - `Error` 接口：`tools/error.hpp:91-110`
  - `StringError`（叶子）：`:152-161`
  - `WrappedError`（单原因）：`:175-190`
  - `JoinError`（多原因）：`:203-216`
- 关键函数：`NewError/Wrap/Join`（`:219-225`）、`Unwrap(All)`/`ErrorMessage`（`:228-232`）、`Is`（`:239`）、`As<T>`（`:274-284`）、`WalkErrors`（`:141`）。

## 2. 问题记录

### Q-001 error 如何嵌套使用？
- 提问：error 模块如何嵌套使用？
- 结论：两种形态——单链 `Wrap` 套 `Wrap`（Message 用 `": "` 展平）；多原因 `Join({...})`（Message 用换行连接，嵌套 Join 用显式栈展开）。两者可混合，查询统一走 `Is`/`As` 的环安全遍历。
- 证据：
  - `WrappedError::Message()` 迭代同类型链：`src/tools/error.cpp:188-230`
  - `JoinError::Message()` 显式工作表展开嵌套 Join：`src/tools/error.cpp:244-300`
  - `Wrap` 工厂：`error.cpp:307-314`；`Join` 工厂（过滤空）：`error.cpp:316-332`
  - `Is` 遍历 `UnwrapAll()` + `visited` 防环：`error.cpp:363-413`
  - `As<T>` 同类遍历：`include/go2cpp/tools/error.hpp:243-284`
- 示例：
  ```cpp
  auto base = NewError("connection refused");
  auto db   = Wrap(base, "query user");
  auto api  = Wrap(db,   "GET /users");
  // api->Message() == "GET /users: query user: connection refused"
  auto batch = Join({api, Wrap(NewError("i/o timeout"), "redis GET")});
  // batch->Message() == "GET /users: ...\nredis GET: i/o timeout"
  // Is(batch, base) == true
  ```
- 风险/建议：`Wrap(cause, message)` 参数顺序与 Go `fmt.Errorf("msg: %w", cause)` 相反；`Wrap(nullptr,msg)` 返回空，会静默丢上下文。
- 状态：已答

### Q-002 为什么整个项目“没有 JoinError 和 WrapError”？
- 提问：为什么没有 `JoinError` 和 `WrapError`？
- 结论：**这两个名字是类型，不是 API**。`JoinError`/`WrappedError` 是 `Join()`/`Wrap()` 返回对象的实现类（`tools/error.hpp:175/203`）；对外工厂只有 `Wrap`/`Join`，与 Go 的 `errors.Join`、`fmt.Errorf("%w")` 对齐——Go 同样没有 `JoinError`/`WrapError` 函数。命名上刻意「类名=结构、函数名=动作」，避免同名歧义。
- 证据：
  - `ErrorPtr Wrap(ErrorPtr cause, std::string message);` — `tools/error.hpp:221`
  - `ErrorPtr Join(std::vector<ErrorPtr> causes);` — `tools/error.hpp:223`
  - `class WrappedError final : public Error` — `tools/error.hpp:175`
  - `class JoinError final : public Error` — `tools/error.hpp:203`
  - `errors::` Go 风格别名只有 `New/Wrap/Join/Is/As`，无 `*Error` 工厂 — `tools/error.hpp:313-349`
- 风险/建议：`JoinError`/`WrappedError` 构造函数是 public，可直接 `make_shared` 绕过工厂；不推荐（会跳过空指针过滤/统一 `ErrorPtr` 约束）。代码审查看到 `JoinError(` 时先分清是类型构造还是函数调用。
- 状态：已答

### Q-003 嵌套/查询还有哪些坑？
- 结论：
  1. `Message()` 没有环保护（`WrappedError::Message` 会调自定义 cause 的 `Message()`，`error.cpp:220`）；`Is`/`As` 有 `visited` 防环。自定义 `Error` 不要构造环。
  2. `Is(err, nil)` 返回 `!err`（`error.cpp:400-403`），与 Go 一致；两次 `NewError("x")` 身份不同，不相等。
  3. `As<T>` 只按动态类型，不能按基类/接口；`AsMutable`（`error.hpp:286-296`）是 const_cast 逃生口，共享时改对象不安全。
  4. 深链析构用 thread-local 工作表迭代释放（`error.cpp:137-186`），避免递归爆栈；这也意味着错误链可以很深，但 `Message()` 拼字符串仍有 O(n) 内存。
- 状态：已答

## 3. 已确认缺陷

暂无（error 模块本轮未发现 HEAD 缺陷；审计文档也未列 error 项）。

## 4. 待读 / 待确认

- [ ] `WalkErrors` 的 visitor 抛异常语义（`error.cpp:130-134` 返回 false）是否与调用方预期一致
- [ ] `JoinError::Message()` 对「Join→Wrapped→Join」混合嵌套的换行层级（当前逐叶子换行）
- [ ] 与 `context`/`config` 模块的 `Wrap` 链实际用法（下一模块）

## 5. 关联

- 导读：`docs/code-review-guide.md` §2.1（error target）、§5 路线 F
- 相关：`docs/error_guide.md`


## 6. 与 Go error 生态的差距（功能缺失清单）

> 口径：按用户确认，**当前实现无问题**；本节只记录「Go 有、项目未覆盖或形态不同」的能力，
> 不作为缺陷。Go 基线取 1.20+（`errors.Join`、`fmt.Errorf` 多 `%w`）。

先澄清包划分：Go 的 error 能力跨两个包——`errors` 只有 `New/Is/As/Unwrap/Join`，
`Errorf` 属于 **`fmt`**。项目同样如此：`Errorf` 在 `include/go2cpp/tools/fmt.hpp:674`，
不在 `tools/error.hpp`。

| # | 能力 | Go 位置/语义 | 项目现状 | 状态 |
|---|---|---|---|---|
| G-01 | 多 `%w` 的包装类型 | Go 1.20+ `fmt.Errorf("%w %w", a, b)` 返回 `*fmt.wrapErrors`：`Message` 是完整格式化文本，`Unwrap() []error` 返回多个 cause | `Errorf` 用 `Join({Wrap(a,msg), Join({a,b})})` 近似（`fmt.hpp:721-723`），没有「message + 多 cause」类型 | **缺失**（近似实现） |
| G-02 | 单个 `%w` 的文本语义 | `wrapError.Error()` 返回格式化后的 `msg`（`msg` 已含 cause 文本，不再追加） | 复用 `Wrap`，`WrappedError::Message()` 会再追加 cause（`error.cpp:188-230`），所以 `Errorf("x: %w", e)` 的文本是 `"x: e: e"`（`tests/tools/test_fmt.cpp:73` 固化了该输出） | **缺失**（无 `Message` 不追加 cause 的包装类型） |
| G-03 | `%w` 的 flags/width | `fmt` 支持 `%5w`、`%+w` 等带 flags 的 `%w` | `ParseSpec` 能识别 `verb=='w'`（`fmt.hpp:691`），但归一化只做字面 `find("%w")`（`:698`），`%5w` 落到 `%!w(error=...)` | **缺失** |
| G-04 | `%w` 参数不是 error | Go 输出 `%!w(...)`，并把该参数按普通值处理 | `IsErrorPtr` 不通过时直接忽略该 cause（`fmt.hpp:710-714`），文本仍按 `%v` 渲染，链里没有它 | 差异（容错 vs 显式报错） |
| G-05 | `Unwrap() []error` 接口 | Go 1.20 用接口断言 `interface{ Unwrap() []error }` | 虚函数 `UnwrapAll()`（`error.hpp:104`、`JoinError::UnwrapAll` `:212`）；C++ 无 Go 接口，形态不同 | 形态差异（无法直接兼容） |
| G-06 | `errors.As` 到接口/指针目标 | `errors.As(&target)` 可匹配任意接口/具体类型 | `As<T>` 只按动态类型 `dynamic_pointer_cast`（`error.hpp:274-284`），不支持基类/接口断言 | 形态差异 |
| G-07 | `%+v`/`%#v` 对 error 的格式 | `fmt` 对 error 调用 `Error()`，`%+v` 等有细节 | `FormatValue` 对 ErrorPtr 只处理 `%v/%s/%q/%T`（`fmt.hpp:313-325`），其他 verb 输出 `%!verb(error=...)` | **缺失**（若需要） |
| G-08 | 无 `%w` 的 `Errorf` | 返回 `*errors.errorString`（等价 `errors.New`） | `NewError(message)`（`fmt.hpp:721`），一致 | 已对齐 |
| G-09 | `errors.Is(err, nil)` | `target == nil` 时返回 `err == nil` | 已对齐（`error.cpp:400-403`） | 已对齐 |

### 小结

- 核心 `errors` 子集完整：`New / Is / As / Unwrap / Join`，额外提供 `WalkErrors`、
  `ErrorMessage`、`AsMutable`、`errors::` 别名。
- 缺口集中在 **`fmt.Errorf` 的包装类型与格式细节**：G-01/G-02/G-03 是真实的功能缺失，
  统一修法是新增一个 `FormattedError`（保存 `message` + `vector<ErrorPtr> causes`，
  `Message()` 原样返回、`UnwrapAll()` 返回 causes），`Errorf` 改用它；随后补 `%w` 的
  flags 归一化、修 `test_fmt.cpp:73` 的断言并补多 `%w` 的 Message/UnwrapAll 用例。
- G-05/G-06 是 C++ 与 Go 的类型系统差异，不建议强行模拟；文档里明确即可。
