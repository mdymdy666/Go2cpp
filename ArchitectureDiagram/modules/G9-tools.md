# G9 · 工具与新手层：`control_flow` / `go` / `event` / `thread_policy` / `fiber_local` / `hybrid_mutex` / `status` / `runtime`

> 依赖：本组是**跨层**的便捷与策略设施 —— 它们把 G1~G8 的能力包装成「转译代码直接可用」的形态。
> 规模（实测）：`tools/control_flow.hpp` 241 + `.cpp` 346；`tools/go.hpp` 220（header-only）；
> `tools/event.hpp` 424（header-only）；`concurrency/thread_policy.hpp` 153 + `.cpp` 202；
> `concurrency/fiber_local.hpp` 140 + `.cpp` 183；`core/hybrid_mutex.hpp` 114（header-only）；
> `core/status.hpp` 20；`runtime.hpp` 36（聚合头）。

---

## 0. 一句话定位

这一层决定「**转译后的 Go 代码写起来像不像 Go**」：
`go()` 是 goroutine 的入口，`defer`/`panic`/`recover` 是显式状态协议，
`EventBatch` 是多事件循环，`fiber` 是轻量任务句柄，`FiberLocal` 是 G 局部存储，
`thread_policy` 控制外部线程如何参与，`StatusCode` 是跨模块统一状态码，`runtime.hpp` 是总入口。

---

## 1. `core/status.hpp`：跨模块统一状态码

```cpp
enum class StatusCode {
  kOk = 0, kCancelled, kDeadlineExceeded, kClosed, kAlreadyClosed, kInvalidState, kShutdown
};
```

用途：取消/截止/关闭/调度器生命周期的**统一枚举**；同时各 API 仍会返回更详细的 `ErrorPtr`。
（注意与 `io::WaitStatus`、`ChannelStatus`、`FutureStatus` 等**各自模块的状态枚举**并存，
它们是不同的类型，不要混用。）

---

## 2. `tools/control_flow.hpp`：显式 defer / panic / recover

### 2.1 `defer`

```cpp
class defer final {                       // 别名 Defer
  template <typename F> explicit defer(F&& function);
  template <typename F, typename... Args> defer(F&& function, Args&&... args);   // ★ 构造时按值保存参数
  ~defer() noexcept;                      // 执行一次（若未 dismiss）
  defer(const defer&) = delete;  defer(defer&&) = delete;      // 不可拷贝不可移动
  void dismiss() noexcept;                // 取消
  void run_now() noexcept;                // 立即执行并置非活动
  bool active() const noexcept;
};
```

- LIFO 顺序**由 C++ 作用域规则天然提供**（不是 Go 的函数级 defer 栈）。
- 覆盖正常返回、提前 `return` 与异常展开三条路径。
- 回调**按值保存**函数与参数（`MakeCallableWithArgs` 用 `std::tuple<std::decay_t<Args>...>`）。
- 回调抛出的异常被 `detail::RecordDeferException` 吞掉（`noexcept` 析构不能抛），
  之后用 `LastDeferException()` / `ClearDeferException()` 取回。

### 2.2 `panic` 与 `recover`（显式状态协议，不是栈展开）

```cpp
struct PanicInfo { bool raised{false}; std::string message; int code{0}; std::any payload; bool valid(); };

class panic final {                       // 别名 Panic
  void call(std::string message, int code = 0);
  void call(const char* message, int code = 0);
  template <typename T> void call(T payload, int code = 0);      // 任意可移动负载
  bool active() const noexcept;  bool recovered() const noexcept;
  PanicInfo info() const;        void clear() noexcept;
};
class recover final {                     // 别名 Recover
  bool bind(panic& source) noexcept;   void reset() noexcept;   bool bound() const noexcept;
  bool active() const noexcept;
  std::optional<PanicInfo> take() const;   // ★ 只有在 defer 回调期间才成功
  bool operator()() const noexcept;
  PanicInfo peek() const;                  // 只读，不改 recovered 标记
};
std::exception_ptr LastDeferException() noexcept;   void ClearDeferException() noexcept;
```

- `panic::call()` **只发布 `PanicInfo` 到共享 `PanicState`**：不 `longjmp`、不抛内部异常、不结束函数。
- panic 状态是 `shared_ptr<detail::PanicState>` → **可随 Fiber 迁移**。
- ★ **恢复边界是「是否在 defer 回调内」**：`detail::InDeferCallback()` 用 **`thread_local` 深度**
  实现（`s_defer_depth`），而语言层面 Fiber 会在同一 OS 线程上多路复用 ——
  因此「Fiber A 在 defer 回调里 park，Fiber B 同线程 recover」会被判为「都在 defer 回调内」。
  项目已在 `docs/compatibility.md` 声明「共享对象不绑定 G/Fiber，跨执行流消费不会被库自动阻止，
  属于调用方必须遵守的限制」（台账 X-30 的最终结论：**文档口径问题，不是实现缺陷**）。
- `recover::take()` 不会捕获普通 C++ 异常（那是 Fiber 边界 `failure()` 的职责）。

### 2.3 与 Go 的差异

| Go | 本项目 | 差异 |
|---|---|---|
| `defer` 是语言级（函数级 LIFO 栈） | `defer` 对象（作用域级 LIFO） | 需要显式声明对象；无编译器插入 |
| `panic` 触发栈展开 + 执行全部 defer | `panic::call` 只记录状态 | **不做栈展开**；必须手工在 defer 回调里 recover |
| `recover()` 只能在 defer 内调用 | `recover::take()` 只在 defer 回调内成功 | 语义一致，但边界实现是 thread_local 深度 |
| panic 会终止 goroutine | panic 状态需显式消费 | 不消费不会自动终止；由调用方决定 |

---

## 3. `tools/go.hpp`：goroutine 入口与默认调度器

```cpp
namespace detail {
  struct DefaultSchedulerState { std::mutex mutex; std::shared_ptr<Scheduler> scheduler; };
  DefaultSchedulerState& default_scheduler_state();     // static auto* state = new ...（★ 故意不析构）
  std::shared_ptr<Scheduler> ensure_default_scheduler(); // 句柄为空或已停 → 新建并 start
}
std::shared_ptr<Scheduler> default_scheduler_handle();
Scheduler& default_scheduler();                        // 别名 DefaultScheduler()
void shutdown_default_scheduler() noexcept;             // 别名 ShutdownDefault()

std::shared_ptr<Task> go(Scheduler& scheduler, Task::Function);   // 未运行则自动 start
std::shared_ptr<Task> go(Task::Function);                         // 默认调度器
template <typename Function> std::shared_ptr<Task> go(Function&&);  // 任意可调用对象；别名 Go()
core::Timer after(std::chrono::steady_clock::duration, Task::Function);   // 默认调度器延时提交
core::Timer every(std::chrono::steady_clock::duration, Task::Function, core::TimerOptions = {});

class fiber final {                       // 轻量 Fiber 描述器（不持有线程、不创建隐式调度器）
  explicit fiber(Function, TaskOptions options = {});
  fiber(const fiber&) = delete;  fiber(fiber&&) = delete;      // ★ 避免重复提交
  std::shared_ptr<Task> task() const noexcept;
  std::shared_ptr<Task> bind(Scheduler&);                      // 已绑其他调度器则返回空
  bool wait() const;   bool wait_for(duration) const;   bool cancel();
  GState state() const noexcept;   bool started() const noexcept;
};
```

关键设计：

- **默认调度器是进程级堆对象且永不析构**（`new DefaultSchedulerState()`）——
  避免静态析构顺序导致工作线程访问已销毁对象（这是项目里正确处理该风险的样板，
  与 8 个错误单例的写法相反，见 G3）。
- `Scheduler::shutdown()` 是**终态**：`shutdown_default_scheduler()` 先把句柄 **move 出去**再关闭，
  因此后续 `go()` 会**新建**一个 Scheduler，且不会把任务提交到已 stopping 的旧实例。
- `fiber` 是裸指针 `Scheduler* m_scheduler` 但只做相等比较、**从不解引用**，所以 Scheduler 先析构也安全。
- `after`/`every` 只是注册定时器，**不在定时器线程直接执行用户函数**（到点后提交为 Fiber）。

---

## 4. `tools/event.hpp`：`EventBatch`（多事件循环）

```cpp
class EventBatch final {
  using Handler = std::function<void(const SelectResult&)>;
  static std::shared_ptr<StopControl> make_stop_control();
  void bind(SelectCase event, Handler handler = {});
  void add_event(SelectCase event, Handler handler = {});
  void load(SelectCase event, Handler handler = {});
  void clear();
  void make_loop_time(Duration timeout, bool stop_on_timeout = true);
  void make_stop_config(std::size_t max_rounds, Duration max_duration);
  void make_stop_config(std::size_t max_rounds, /* 另一重载 */);
  void stop() noexcept;   void reset();
  bool runnable() const;  bool runable() const;         // 拼写兼容别名
  bool happened() const;  bool happend() const;
  static SelectResult stopped_result(const char* message);
  SelectResult work(const ContextPtr& context = {}, /* 见签名 */);
  std::optional<SelectResult> handle() const;
  SelectResult last_result_copy() const;   SelectResult last_result() const;
  std::size_t size() const;   std::vector<SelectCase> cases() const;
  SelectResult work_stopped();
};
```

- 它只包装 `SelectCase`/`SelectResult`/`Channel`，**不替你启动阻塞回调**（需自行 `go()`）。
- `work()` 由 `m_work_mutex`（`sync::Mutex`）保证**单消费者**；事件列表修改采用锁保护 +
  快照语义（`cases()` 返回快照）。
- `stop()` 通过内部 `StopControl`（`DoneSignal`/Context）做**协作式**停止，唤醒等待。
- ★ 台账 X-27：**所有写入方法在 `sync::Mutex::Lock()` 返回 false 时静默返回**
  （`add_event`/`bind`/`clear`/`reset`/`make_loop_time`/`make_stop_config`），调用方无法感知失败；
  现实触发路径是「在手动 Fiber（无 Scheduler 归属）中竞争锁」。
- 类注释只要求「对象必须一直存活到所有 work/bind/add_event 调用返回」。

---

## 5. `concurrency/thread_policy.hpp`：外部线程参与策略

- 类型/入口：`ThreadHookMode`、`ScopedThreadHookMode`、`ScopedThreadParticipation`、
  `CurrentThreadHookMode()`、`SetCurrentThreadHookMode(mode)`（`go2cpp::hook` 命名空间提供同名别名）。
- `ScopedThreadParticipation` 只记录「本线程是否愿意参与 Scheduler」，**不会**把普通线程变成 M、
  不给它运行队列、不提供 `run_one`。
- `ScopedThreadHookMode` 只覆盖**当前线程**的 socket Hook 模式；runtime worker 会自动标记为 managed worker。
- 语义边界（`docs/design.md`）：Hook 关闭时，managed 代码应使用非阻塞 API 或显式 `BlockingRegion`。

---

## 6. `concurrency/fiber_local.hpp`：G 局部存储

```cpp
template <typename T> class FiberLocalCache;     // 别名 FiberLocal<T>
// 关键接口（示例与头文件一致）：GetOrCreate(default)、TryGet()、Reset()、key_id()
```

- 值存**全局 `Registry`**：`mutex` + `unordered_map<Fiber*, unordered_map<KeyId, shared_ptr<void>>>`
  —— **按 Fiber 对象指针隔离**，因此 G 迁移到别的 M 后值仍属于同一个 G。
- 在 Fiber 之外调用 → 退化为 `thread_local` 后备值（按线程隔离）。
- 键 `KeyId` 由 `AllocateKey()` 原子单调分配（0 为哨兵；回绕即 `std::terminate()`）。
- 值的清理时机：trampoline 结束、`abandon_suspended()`；**析构不在锁内执行**。
- 它**不是**栈池或 Fiber 对象池（那是 `FiberStack` 缓存与 `FiberBin`）。

---

## 7. `core/hybrid_mutex.hpp`：`HybridGate`（定位与用户决策）

- 真身：`core::detail::HybridGate`（别名 `core::HybridMutex`）—— `atomic_flag` 自旋
  （默认 64 次，可用 `SetDefaultOptions` 调）+ `std::mutex` 慢路径。
- **「混合」指的是「自旋 + 阻塞」两种策略，不是「线程 / Fiber」两种执行体**：它内部**没有任何 park 逻辑**。
- 实际使用点：4 处内部「唤醒门」（`parking_condition.cpp` 的 `WaitNode::m_wake_mutex`、
  `sync.cpp` 的等待节点门、`io.cpp` 的 `wake_mutex`）。
- ★ **用户决策（台账 X-35）**：框架**不允许** `HybridGate` 的实现与使用，需替换为 `sync::Mutex`；
  约束是**不能用 `std::mutex`**（存在 lock/unlock 跨线程场景）。**本轮范围外，未执行。**
- 台账 X-33 补充：它没有 Context、没有 park、没有公平性，只适用于极短临界区。

---

## 8. `runtime.hpp`：聚合头

包含全部公开模块（channel/config/context/control_flow/core-timer/error/future/fiber/fiber_local/go/
event/hook/io/network 各头/log/scheduler/sync/thread_policy）。
**不含** `go2cpp/json.hpp`（JSON 是可选第三方薄封装，见 `docs/dependencies.md`；纳入聚合头会让
所有使用者的编译时间无条件增加 ~920 KB 单头 costs）。

---

## 9. 本组坑点汇总

1. `defer` 不可拷贝/不可移动，且**作用域级**而非函数级 —— 放在循环里会每次迭代注册一个。
2. `panic` 不消费不会终止任何东西；`recover` 必须在 defer 回调内。
3. `recover` 的边界是 `thread_local` 深度（跨 Fiber 会串），项目已在 `compatibility.md` 声明为**调用方约束**。
4. `EventBatch` 的写方法**静默失败**（X-27）；`work()` 是单消费者。
5. `fiber` 不可拷贝/移动（避免重复提交）；`bind` 到第二个调度器返回空。
6. `FiberLocal` 的值在 `abandon_suspended()` 路径上也会被清理（但挂起栈上的 RAII 不会执行）。
7. `HybridGate` 误用会让受管 G 在 carrier 线程上自旋（X-33）；用户已决定移除（X-35）。

---

## 10. 与 Go 的对应与差异

| Go | 本项目 | 差异 |
|---|---|---|
| `go f()` | `go(fn)` / `go(sched, fn)` / `Go(fn)` | 返回 `Task` 句柄（可 wait/cancel），Go 无句柄 |
| 无（goroutine 无句柄） | `fiber` 类 | 可先构造后提交；不可拷贝 |
| 无 | `after` / `every`（默认调度器版） | Go 用 `time.AfterFunc`/`Ticker` |
| `defer` / `panic` / `recover` | 显式状态协议 | 无栈展开；边界为作用域与 thread_local 深度 |
| `select` + `for` 循环 | `EventBatch`/`SelectLoop` | 面向事件循环的封装，非语言语法 |
| 无 | `ScopedThreadParticipation` / `ScopedThreadHookMode` | 外部线程参与与 Hook 模式的 RAII scope |
| goroutine-local（Go 无官方支持） | `FiberLocal<T>` | 按 Fiber 隔离，Fiber 外回退 TLS |

---

## 11. 对架构图的贡献

**节点**

- `go()` / `fiber`（任务入口与轻量句柄）
- `default_scheduler`（进程级、`new` 且不析构）
- `control_flow`（`defer` / `panic` / `recover` + `LastDeferException`）
- `EventBatch`（`SelectCase` 集合 + 单消费者 `work()` + 协作式 stop）
- `thread_policy`（`ScopedThreadParticipation` / `ScopedThreadHookMode`）
- `FiberLocal<T>`（G 局部存储：全局 Registry 按 Fiber 指针隔离）
- `HybridGate`（内部唤醒门；★ 计划移除）
- `StatusCode`（统一状态码）、`runtime.hpp`（聚合入口）

**连线**

- `go() → default_scheduler → Scheduler::spawn`：「未运行则自动 start；shutdown 后 go() 会新建实例」
- `fiber::bind → Scheduler::add`：「不可拷贝，避免重复提交」
- `control_flow → Fiber 边界`：「defer 异常记录到 LastDeferException；panic 状态在共享 PanicState」
- `EventBatch → Channel/Select`：「work() 单消费者；stop() 协作式唤醒」
- `FiberLocal → FiberRecord 生命周期`：「trampoline 结束 / abandon_suspended 时清理」
- `thread_policy → Hook`：「线程级 Hook 模式覆盖（ScopedThreadHookMode）」

**图内文字**

> 新手层决定「写起来像不像 Go」：`go()` 提交 Fiber（返回可 wait/cancel 的 `Task`），
> `defer` 是作用域级 RAII（LIFO 由 C++ 保证），`panic/recover` 是**显式状态协议**而非栈展开
> （恢复边界是「是否在 defer 回调内」，实现为 `thread_local` 深度 —— 跨 Fiber 需调用方自律）。
> `EventBatch` 把 `SelectCase` 集合包成事件循环（`work()` 单消费者，写方法在加锁失败时静默返回）。
> `FiberLocal<T>` 用全局 Registry 按 Fiber 指针隔离值，是唯一正确的「G 局部存储」手段
> （`thread_local` 不行）。
