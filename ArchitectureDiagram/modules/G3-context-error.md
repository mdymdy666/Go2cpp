# G3 · 取消与错误：`context` / `error`

> 依赖：`context` → `error`（`ErrorPtr`）+ G1/G2（`DoneSignal` 用 `core::ParkingCondition` 等待，
> deadline 用 `core::TimerService`）。`error` 是全项目横切的最底层之一。
> 规模（实测）：`concurrency/context.hpp` 505 行 + `src/concurrency/context.cpp` 2412 行；
> `tools/error.hpp` 251 行 + `src/tools/error.cpp` 478 行。
> ⚠️ `context.cpp` 有约 71 处模板化敷衍注释（「函数功能：…执行流程：1. 校验传入参数…」），
> **与实际代码不符**，本报告一律以代码为准。`error.hpp`/`error.cpp` 中也有少量同类模板句。

---

## 0. 一句话定位

`context` 提供**可取消的父子树**（含 deadline 与类型化值）以及一套**局部补偿事务**（`ContextRollback`）；
`error` 提供**不可变、可共享、可用身份判定的错误图**（`Wrap`/`Join`/`Is`/`As`）。
两者共同定义了「失败与取消」在全项目的传播协议。

---

## 1. `context`

### 1.1 类型体系（只有两个类）

```cpp
class Context final {                      // 构造函数私有，只能经工厂创建
  using Clock = std::chrono::steady_clock;
  using NowFunction = std::function<ContextTimePoint()>;
  static ContextPtr Background();          // 与 TODO() 返回**同一个** root 单例（state->root = true）
  static ContextPtr TODO();
  static std::pair<ContextPtr, CancelFunc>      WithCancel(const ContextPtr&);
  static std::pair<ContextPtr, CancelCauseFunc> WithCancelCause(const ContextPtr&);
  static std::pair<ContextPtr, CancelFunc>      WithDeadline(const ContextPtr&, ContextTimePoint);
  static std::pair<ContextPtr, CancelFunc>      WithTimeout(const ContextPtr&, ContextDuration);
  template <typename T> static ContextPtr WithValue(const ContextPtr&, const ContextKey<T>&, T);
  static ContextPtr WithValue(const ContextPtr&, std::string key, std::any value);   // 字符串键重载
  const DoneSignal& Done() const noexcept;   DoneSignal& Done() noexcept;
  bool IsDone() const noexcept;              ErrorPtr Err() const;   ErrorPtr Cause() const;
  std::optional<ContextTimePoint> Deadline() const;   bool HasDeadline() const;
  std::any Value(const std::string&) const;
  template <typename T> std::optional<T> Value(const ContextKey<T>&) const;
  void Cancel(ErrorPtr cause = {});   void CancelDeadline();
  static void SetNowFunctionForTesting(NowFunction);  static void ResetNowFunctionForTesting();
  static ContextTimePoint Now();
};
class ContextRollback final { /* 见 §1.5 */ };
using ContextPtr     = std::shared_ptr<Context>;
using CancelFunc     = std::function<void()>;
using CancelCauseFunc= std::function<void(ErrorPtr)>;
using RollbackScope  = ContextRollback;
```

类外还有一组同名自由函数（`Background()`/`WithCancel(...)` 等），只是薄转发。

### 1.2 `DoneSignal`：只关闭一次、可复制的完成通知

- 内部 `State` = `mutable std::mutex` + `core::ParkingCondition cv` + `bool done` + 两组回调表
  （`internal_callbacks` / 用户 `callbacks`）。
- `DoneSignal` 自身只有一个 `shared_ptr<State>`，**复制即共享同一信号**；`Context::m_done`
  就是 `m_state->done` 的副本（头文件里的"镜像成员"，让 `Done()` 能内联返回引用）。
- API：`Wait()` / `WaitFor(duration)` / `WaitUntil(time_point)` / `IsDone()` /
  `AddCallback(cb) → CallbackId` / `RemoveCallback(id)`。
- `Signal()` 的五步顺序**就是契约**：
  1. 加锁置 `done=true`；
  2. 把两组回调**搬出**（清空表）；
  3. **释放锁** → `notify_all()`（唤醒所有等待者）；
  4. 先跑 `internal_callbacks`（`ContextRollback` 用它抢在观察者之前冻结 undo 日志）；
  5. 再跑用户 `callbacks`，每个独立 try/catch（异常不外泄）。
- `AddCallback` 在「注册时信号已完成」的情况下会**在调用者线程同步立即执行**
  （`invoke_now` 分支）——这是个易踩的重入点。
- 已知缺陷（台账 X-25 系列）：`IsDone()`/`Signal()` 与 `Context::State::IsCanceled()`
  声明 `noexcept` 却会加锁；锁失败时 `std::system_error` 会穿过 `noexcept` → `terminate`。

### 1.3 取消树与传播顺序

`Context::State` 字段：`parent`（**强引用**）、`children`（**weak** 列表）、`error`、`cause`、
`deadline`、`timer_id`、`typed_values` / `named_values` / `typed_key_anchors`、`done`、
`before_done_callbacks`，全部由 `State::mutex` 保护。

`DrainCancellation` 是**迭代实现**（显式 `std::vector<CancellationWork>` 栈，**不是递归**；
测试覆盖 20000 层深树），顺序是硬规则：

```text
① 遍历整棵子树：写 error/cause、摘除 timer、从 parent 的 children 摘除、搬走 before_done_callbacks
② 全部标记完成之后，才跑 before-done 钩子（逆序）—— 让 ContextRollback 冻结 undo
③ 再对 to_signal **逆序** 调 DoneSignal::Signal()
```

第 ③ 步的逆序意味着**子节点先于父节点完成线性化** —— 因此「父的 Done 回调里同步等子」
不会死锁。取消是幂等的：`DrainCancellation` 内 `if (root || error) continue;`
—— 已经取消的节点直接跳过，**root 永远跳过**（即 `Background().Cancel()` 静默无效）。

`Err()` 的取值只有两个**进程级单例**：`CanceledError()` / `DeadlineExceededError()`；
判定惯例是 `Is(err, DeadlineExceededError())`（按指针身份）。`Cause()` 是首次取消原因，
缺省等于 `error`。

### 1.4 类型化值（`ContextKey<T>`）

- 身份取 `m_token.get()` —— 即**堆上 `shared_ptr<const uint64_t>` 对象的地址**；
  它保存的数值只是 `reinterpret_cast<uintptr_t>(this)` 的占位，不参与匹配。
- `Name()` 只用于诊断；`Anchor()` 把锚点交给 Context 保存（`typed_key_anchors`），
  使「键已析构但值仍存活」期间该地址不会被分配器复用（防 ABA）。
- 查找沿 `parent` 链逐级加锁：typed 只比 `token`，字符串键只比 `name`；子遮蔽父。
- 取值模板 `Value(const ContextKey<T>&) → std::optional<T>`（`any_cast` 指针版，类型不符返回 `nullopt`）；
  **没有** Go 风格的 `Value<T>(key)` 泛型接口。

### 1.5 `ContextRollback` + `Savepoint`：局部补偿事务

- 构造时 `WithCancel(parent)` 造一个**临时 child**，并注册两个回调：
  `AddBeforeDoneCallback`（取消窗口内冻结动作日志）+ `DoneSignalAccess::AddInternalCallback`（真正执行补偿）。
- 状态五态：`kActive / kRollingBack / kCommitted / kRolledBack / kFailed`。
- `RecordUndo(action)` 与 `RecordUndoWithCause(action)` 共用**同一条 LIFO 队列**；
  失败时通过 `std::string* reason` 给出可读原因（动作为空 / 事务已结束 / child 已取消而转入回滚）。
- `Savepoint` = `{weak_ptr<State> m_state, std::size_t m_depth, uint64_t m_generation}`；
  `Mark()` 取令牌；`valid()` = 状态活动 ∧ generation 相同 ∧ `m_depth <= actions.size()` ∧ 未被取消。
- `RollbackTo(mark)`：只执行 `m_depth` 之后的动作，事务回到 `kActive`，并 `generation++`
  让**所有旧令牌失效**（防 ABA）；一次成功的局部回滚会让已有 Savepoint 全部作废。
- `Rollback(cause)`：取消 child + LIFO 跑完全部动作 + 进终态；重复调用仍返回 true，已提交则 false。
- `Commit()`：只丢弃动作，**不取消 child**（调用方仍可把 `context()` 交给下游）。
- 析构 = 自动 `Rollback()`（移动赋值会先回滚被覆盖者）。
- **两个信号不可混用**：`context()->Done()` 只表示「取消已线性化」；
  补偿是否跑完要看 `RollbackDone()`。undo 不得等待自身的 `RollbackDone()`。
- undo 异常：被 `RecordFailure` 收集，`failure()` 只给第一个（历史行为）、`failures()` 给全部；
  状态落 `kFailed`，**其余动作继续执行**。

### 1.6 Deadline 与时钟

- `WithDeadline(parent, requested)` **只在 `requested < inherited` 时**才设置并注册 timer；
  已过期则同步 `Cancel({}, true)`（与 Go 一致）；否则沿用父的 deadline（**没有自己的 timer**）。
- `WithTimeout` 用饱和算术 `SaturatingDeadline(Now(), timeout)`：
  `duration::max()` → `TimePoint::max()`（不取消）、`duration::min()` → 立即过期。
- deadline 由进程级 `TimerService` 单例触发，回调里对 weak state 调 `state->Cancel({}, true)`。
- `SetNowFunctionForTesting` 注入测试时钟；`Now()` 每次调用**加全局锁 + 拷贝一个 `std::function`**
  （台账 X-32：正常运行时该 function 恒空，却在 `Future::Wait`、`WithTimeout` 等热路径上付费）。

### 1.7 坑点

1. **`Done()` 不是 channel 也不是 future**：可复制、可无限次等待、可多线程同时等；
   「一次性」只体现在回调上（`AddCallback` 恰好调用一次；`RemoveCallback` 只能撤销**未触发**的）。
2. **两个信号分离**：只等 `child->Done()` 就访问被 undo 的资源，会拿到「取消已线性化但补偿未跑完」的中间态。
3. **root 的 `Cancel()` 静默无效**，且 `Background().Done()` 是**永久 pending 的信号**（不是 Go 的 nil channel）。
4. **空 parent 被映射为 `Background()`**（`MakeChild` 里 `parent ? parent : Background()`）——Go 会 panic。
5. `AddCallback` 在信号已完成时会**同步立即**在调用者线程执行 → 持锁/析构路径上注册需小心重入。
6. `WithDeadline` 不一定创建新定时器（见 §1.6）。
7. `WithValue` 的键比**地址**不比名字：同名的两个 `ContextKey` 互不命中；键一旦析构且 Context 不再持有该值，
   地址理论上可复用（现有测试只覆盖值仍被持有期间）。
8. 手动 Fiber（`Fiber::Current()!=nullptr` 但无 `current_task()`）上 `Wait/WaitFor` 会立刻返回
   `kCancelled`（`ParkingCondition::FiberWaitUnsupported()`），**不会**阻塞 carrier 线程。
9. `ContextRollback` 的析构/移动赋值都会触发回滚 —— 放进容器或按值返回时容易意外提前回滚。

### 1.8 与 Go `context` 的差异

| Go | 本项目 | 差异 |
|---|---|---|
| `context.Background()` 无取消能力，`Done()` 为 nil channel | 同一类型，`Cancel()` 存在但无效，`Done()` 永久 pending | 显式记录在 `docs/compatibility.md` |
| 空 parent → panic | 映射为 `Background()` | 更宽松，但错误更难发现 |
| `context.AfterFunc` | **无** | — |
| `Value<T>(key)` 泛型取值 | 只有 `Value(ContextKey<T>)` → `optional<T>` | 需要显式 optional 处理 |
| `Deadline() (time.Time, bool)` | `optional<steady_clock::time_point>` | 类型不同 |
| 无对应物 | **`ContextRollback` / `Savepoint` / `RollbackDone`** 两阶段补偿事务 | 本项目扩展 |

---

## 2. `error`

### 2.1 类型与接口

```cpp
class Error {                                   // 不可变错误值（共享安全）
  virtual ~Error() = default;
  virtual std::string Message() const = 0;
  virtual std::string ErrorString() const;      // Go 风格别名
  virtual std::shared_ptr<const Error> Unwrap() const;                    // 单原因
  virtual std::vector<std::shared_ptr<const Error>> UnwrapAll() const;    // 多原因（默认返回 Unwrap()）
  virtual bool Is(const Error& target) const noexcept;                    // 默认：this == &target
};
using ErrorPtr = std::shared_ptr<const Error>;

class StringError final : public Error;   // 叶子：一条不可变文本
class WrappedError final : public Error;  // message + 单一 cause；Message() 返回**整条链**
class JoinError   final : public Error;   // 多个非空 cause；Message() 用换行连接
```

自由函数：`NewError`、`Wrap(cause, message)`、`Join(vector/initializer_list)`、`Unwrap`、
`UnwrapAll`、`ErrorMessage`、`Is(error, target)`、`As<T>(error)`、`AsMutable<T>(error)`、
`WalkErrors(error, visitor)`；外加 `errors::` 命名空间的一组别名（`errors::New/Wrap/Join/Unwrap/Is/As`）。

### 2.2 关键实现细节（决定行为与性能）

1. **`WrappedError::Message()` 返回整条链**：形如 `"outer: inner: root"`。
   实现用 `for(;;)` 循环 + 裸指针推进 + `current_owner = std::move(cause)`（**先推进指针再释放前一个所有者**），
   因此长链不会递归爆栈。→ 这是全项目最著名的坑：**不能用文本比较判断错误层级**，要用 `Is` 的身份比较。
2. **`JoinError::Message()`** 同样用显式工作表展开嵌套 `Join`（避免 `Join({previous, next})` 深链递归）。
3. **析构也是迭代的**：`WrappedError::~WrappedError` / `JoinError::~JoinError` 用
   `thread_local std::vector<ErrorPtr>* t_error_release_work` 工作表，把「释放 shared_ptr 成员」
   变成最外层一次迭代排空 —— 否则深错误链析构会递归耗尽原生栈。
4. **`Is` 是身份语义**：`IsImpl` 迭代遍历 + `unordered_set<const Error*> visited` 去重（环安全）；
   命中条件 `next.get() == target.get() || next->Is(*target)`。
   `Is(err, nullptr)` 的语义是「err 也为空」——即 `errors.Is(err, nil)`。
   整个函数 `noexcept` 且内部 try/catch（自定义 `UnwrapAll` 抛异常只报「不匹配」）。
5. **`As<T>`** 同样是迭代 + visited + `dynamic_pointer_cast`，异常转 `nullptr`（`noexcept` 边界）；
   `AsMutable<T>` 是显式出口（`const_pointer_cast`），普通调用方应用 `As<T>` 并保持不可变。
6. **`WalkErrors`** 提供环安全的图遍历插件点：visitor 返回 `kStop` 或抛异常 → 返回 false；
   每个错误对象最多访问一次（按地址去重）。日志/指标/转换器可复用它而不依赖 `WrappedError`/`JoinError`。
7. **`Join` 会过滤空原因**，全空时返回空 `ErrorPtr`；`Wrap(nullptr, msg)` 也返回空
   （即「包装一个 nil 错误」等于 nil）。

### 2.3 坑点

1. `Wrap` 的 `Message()` 是链，**层级判断必须用指针身份**（`Is`），文本比较会错。
2. `Error` 实现构造后**必须不可变**：`ErrorPtr` 是 `shared_ptr<const Error>`，共享是安全的；
   `AsMutable` 仅供明确的适配场景。
3. **8 个进程级错误单例**（`CanceledError`/`DeadlineExceededError`/`ChannelClosedError`/
   `ChannelAlreadyClosedError`/`ChannelWouldBlockError`/`ChannelNilError`/`ChannelTimeoutError`/
   `BrokenPromiseError`）都是函数局部 `static const ErrorPtr`：静态析构后在退出阶段返回空指针，
   而 `Is(err, {})` 变成「err 是否为空」→ 可能把「超时」误判成「非超时」（台账 X-31）。
   项目在 `go.hpp` 的默认调度器上已用「`new` 且不析构」规避，错误单例**尚未**统一处理。
4. `broken promise` 的单例是进程级的：多个不同来源的 broken promise 之间 `Is(a,b) == true`，无法区分来源。
5. `errors::` 命名空间是**工作区新增、尚未提交**的 API（`include/go2cpp/tools/error.hpp` 当前 diff +47 行）。
   任何新代码若依赖它，必须确认提交边界（本项目已因此触发过一次「HEAD 无法构建」的风险）。

### 2.4 与 Go `errors` 的差异

| Go | 本项目 | 差异 |
|---|---|---|
| `errors.New` / `fmt.Errorf("%w")` | `NewError` / `Wrap(cause, message)` | `Wrap` 的 Message 是整条链（Go 只给当前层文本 + `%w` 语义） |
| `errors.Is` | `Is(error, target)` | 同样支持链；这里**支持多原因图**（`JoinError` + 地址去重的环安全遍历） |
| `errors.As` | `As<T>` / `AsMutable<T>` | 返回 `shared_ptr<const T>`，不抛异常 |
| `errors.Join`（Go 1.20） | `Join` / `JoinError` | Message 用换行连接；空原因被过滤 |
| `Unwrap() error` | `Unwrap()` / `UnwrapAll()` | 增加了多原因版本（Go 1.20 也用 `Unwrap() []error`） |

---

## 3. 对架构图的贡献

**节点**

- `Context`（取消树：root 单例 / WithCancel / WithDeadline / WithTimeout / WithValue）
- `DoneSignal`（一次性关闭 + 两组回调；可复制共享）
- `ContextKey<T>`（身份 = 堆地址 + 锚点防 ABA）
- `ContextRollback`（+`Savepoint`，LIFO 补偿事务，独立 `RollbackDone`）
- `Error`（不可变错误图：String / Wrapped（链式 Message）/ Join（多原因））
- `ErrorRegistry`（8 个进程级单例错误，用身份判定）

**连线**

- `Context → DoneSignal`：「Signal() 恰好一次；回调在锁外、internal 先于 user」
- `Context → TimerService`：「deadline 到期 → state->Cancel({}, true)」
- `ContextRollback → DoneSignal`：「before-done 冻结 undo → internal 回调执行补偿 → RollbackDone」
- `DoneSignal → ParkingCondition`：「Wait/WaitFor 在受管 G 上 park，让出 M」
- `Context → Error`：「Err() ∈ {CanceledError, DeadlineExceededError}（身份比较）」
- `Error → 全模块`：「所有失败路径统一返回 ErrorPtr/Wrap 链」

**图内文字**

> 取消协议：先把整棵子树标记完（摘 timer、摘 parent 登记、搬走 before-done 回调），
> 再**逆序**（子 → 父）线性化 `DoneSignal`，所以父的 Done 回调里同步等子不会死锁；
> root 的 Cancel 是空操作。补偿与取消是**两个信号**：`Done()` 表示取消已线性化，
> `RollbackDone()` 才表示 undo 跑完。
> 错误协议：`ErrorPtr` 是不可变共享值，`Wrap` 的文本是整条链，
> 因此层级/种类判定一律用**身份比较** `Is(err, XxxError())`；`JoinError` 让错误成为图而不是链。
