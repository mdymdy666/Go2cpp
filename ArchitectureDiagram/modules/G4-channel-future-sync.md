# G4 · 并发原语：`channel` / `future` / `sync`

> 依赖：三者都建立在 G1（`ParkingCondition`、`Fiber`）与 G2/G3（`Scheduler`、`Context`）之上。
> 规模（实测）：`concurrency/channel.hpp` 2007 行 + `src/concurrency/channel.cpp` 402 行；
> `concurrency/future.hpp` 952 行 + `src/concurrency/future.cpp` 175 行；
> `include/go2cpp/sync.hpp` 219 行（**真身**）+ `src/concurrency/sync.cpp` 1324 行。
> ⚠️ 路径反例：`include/go2cpp/sync.hpp` 本身是真身，而 `include/go2cpp/concurrency/sync.hpp`
> 才是 5 行转发头 —— 与其余 26 个模块「根目录转发 + 子目录真身」的方向**相反**（台账 X-26 已修）。

---

## 0. 一句话定位

`channel` 是 Go 的 channel + select；`future` 是 Go 没有的「一次性结果容器」（**没有 then 链**）；
`sync` 是 Go 的 `sync.Mutex`/`sync.Cond`/`sync.WaitGroup`，并且**同时服务 Fiber 与原生线程**。
三者的等待全部走 `core::ParkingCondition`：受管 G 挂起让出 M，普通线程阻塞原生 cv。

---

## 1. `channel`

### 1.1 类与编译期约束

```cpp
template <typename T>
class Channel final : public std::enable_shared_from_this<Channel<T>> {
  static_assert(std::is_nothrow_move_constructible<T>::value, ...);
  static_assert(std::is_nothrow_destructible<T>::value, ...);
  static ChannelPtr<T> Create(std::size_t capacity = 0);
  explicit Channel(std::size_t capacity);
  ~Channel();                                   // 析构会调 Close()
  std::size_t Capacity() const;  std::size_t Len() const;  bool IsClosed() const;  bool closed() const;
  ChannelSendResult Send(T value, const ContextPtr& = {});
  ChannelSendResult SendFor(T value, ContextDuration, const ContextPtr& = {});
  ChannelSendResult TrySend(T value);
  ChannelRecvResult<T> Recv(const ContextPtr& = {});
  ChannelRecvResult<T> RecvFor(ContextDuration, const ContextPtr& = {});
  ChannelRecvResult<T> TryRecv();
  ChannelStatus Close() noexcept;               // 幂等；重复关闭返回 kAlreadyClosed
  std::uint64_t Generation() const;  bool WaitForChange(std::uint64_t gen, ContextDuration timeout);
  bool ArmSelectRecv(...);  bool ArmSelectSend(...);  void DisarmSelect(...);
};
using ChannelPtr<T> = std::shared_ptr<Channel<T>>;
ChannelPtr<T> MakeChannel<T>(std::size_t capacity);
ChannelSendResult SendChecked(...);  ChannelRecvResult<T> RecvChecked(...);   // nil 通道 → kNil
SendOnlyChannel<T> AsSendOnly(...);  RecvOnlyChannel<T> AsRecvOnly(...);
```

`static_assert` 的意义：为保证接收与 select 的**强异常安全**，`T` 必须是不抛的 move 构造且析构不抛。

### 1.2 结果类型（不用异常做控制流）

```cpp
enum class ChannelStatus { kReady, kWouldBlock, kClosed, kAlreadyClosed, kCancelled,
                           kDeadlineExceeded, kTimedOut, kNil, kInvalid };
struct ChannelSendResult { ChannelStatus status; ErrorPtr error; };
template <typename T> struct ChannelRecvResult {
  T value{}; bool ok{false}; bool ready{false}; ChannelStatus status; ErrorPtr error;
  T ValueOrDefault() const;
};
```

### 1.3 缓冲与交接语义

- `capacity == 0` → **rendezvous**：只通过等待者队列直接交接，不缓冲。
- `capacity > 0` → 有界 FIFO `std::deque<T> m_buffer`。
- `SendUntil`：**优先交付最早的等待接收者**，没有等待者才入缓冲。
- `RecvUntil`：先取缓冲，再 `RefillBufferLocked()` 从 `m_senders` 补位（把已在等待的发送者搬进缓冲）。
- 等待节点 `PendingSend` / `PendingRecv` **内嵌 `core::ParkingCondition cv`**，
  因此「受管 Fiber park / 普通线程原生 cv / 手动 Fiber 直接失败」三条路与 G1 完全一致。
- Context 取消通过 `CallbackGuard` 注册 `Done().AddCallback([pending]{ pending->cv.notify_one(); })`，
  并在析构时 `RemoveCallback`；轮询循环另外检查 `ParkingCondition::CancellationRequested()`。
- `FiberWaitUnsupported()` 为真（手动 Fiber）时直接返回 `kInvalid`，**不会**把 carrier 线程卡住。

### 1.4 `Close()` 的顺序契约

1. 幂等：已关闭 → `kAlreadyClosed`，不抛异常；
2. 把 `m_senders`/`m_receivers` **整体 swap 出来**，逐个置 `closed/ready`；
3. 对 select 节点额外 `TrySelect(kClosed)`；**若提交选择结果抛异常则改走 `Cancel()`**
   （否则会留下一个永远等待的 Fiber）；
4. 全部唤醒移到**锁外**逐个 `notify_one()`；
5. 关闭后：`Send` → `kClosed`；`Recv` 先排空缓冲，之后 `ok=false, status=kClosed`。

### 1.5 select

```cpp
struct SelectCase { /* probe, is_default, arm, disarm, unsupported, arm_checked */ };
struct SelectProbe { bool ready; bool ok; std::any value; SelectValue typed_value;
                     ChannelStatus status; ErrorPtr error; };
struct SelectResult { std::size_t index; bool selected, ok; std::any value; SelectValue typed_value;
                      ChannelStatus status; ErrorPtr error;
                      template <typename T> T Value() const;  ValueObject();  TypedValue<T>();
                      template <typename T> T TakeValue();    template <typename T> T Cast(const Caster<T>&); };
SelectResult Select(std::vector<SelectCase> cases, const ContextPtr& = {},
                    std::optional<ContextDuration> timeout = std::nullopt);
SelectCase RecvCase(...);  SelectCase SendCase(...);  SelectCase DefaultCase();
```

- **公平性**：`static std::atomic<std::size_t> cursor` + `fetch_add(1, relaxed)` 轮转探测起点 ——
  是**轮转**公平而非 Go 的伪随机。
- **单赢家**：`detail::SelectWaitState`（`TrySelect` / `TrySelectPair` / `Take` / `Cancel` / `Wait` / `WaitFor`）；
  `TrySelectPair` 用 `std::scoped_lock` 双锁保证发送方与接收方**同时认领**；`Take` 只允许成功一次。
- **arm 失败与异常**：`arm_checked` 返回 false 且尚未被选中时抛 `std::runtime_error`，
  随后 `Cancel() + disarm` 清理，不留悬挂节点。
- **不可复制 `T`**：`RecvCase`/`SendCase` 会置 `unsupported = true`，
  内置 `Select` 见到就直接返回 `kInvalid` —— 不会去登记一个无法回滚的等待者。
  move-only 值需要走独立 `SelectValue`（类型擦除，`Take` 要求 `use_count()==1`）。
- **default 分支**：`is_default` 在轮询里被跳过，只有当本轮确认没有任何 channel case 就绪且未取消时，
  才返回 `{index, selected=true, ok=true}`。
- 存在纯 probe 的自定义 case 时（`has_poll_case`）用 **2ms 轮询**等待，而不是无限 park。

### 1.6 坑点

1. 对已关闭通道 `Send` 返回 `kClosed` 状态对象，**不是异常也不是 Go 的 panic**。
2. 内建 select **不支持不可复制 T**（返回 `kInvalid`），这不是"永久等待"而是显式失败。
3. `Channel` 必须活得比所有等待者长（等待节点不持有通道所有权）。
4. select 的 probe 与 arm 之间存在窗口，实现靠「持通道锁再查就绪 + `TrySelectPair`」修补；
   已挂起的 channel case **不允许再轮询**（`TryRecv`/`TrySend` 可能消耗一次传输却没发布选择状态）。

---

## 2. `future`（Go 没有对应物）

### 2.1 类型与 API

```cpp
template <typename T> class Promise {          // **不可复制，仅可移动**
  Future<T> GetFuture();  Future<T> future();
  void SetValue(...);      // void 与非 void 两套 SFINAE 重载
  void SetError(ErrorPtr);  void SetException(std::exception_ptr);  void Cancel();
  // 析构时若仍 pending → SetError(BrokenPromiseError())；移动赋值覆盖未完成者同样
};
template <typename T> class Future {           // **可复制**
  bool valid() const;  bool Wait();  bool WaitFor(duration);  bool WaitUntil(time_point);
  FutureResult<T> GetResult();  GetResultFor(...);  GetResultUntil(...);
  std::optional<FutureResult<T>> TryGet();  FutureStatus Status();
  T GetOrThrow();        // 显式 opt-in 抛异常；要求可复制（static_assert）
};
enum class FutureStatus { kReady, kError, kException, kCancelled, kTimedOut, kDeadlineExceeded, kInvalid };
template <typename T> struct FutureResult;   // T 与 void 两个特化
MakePromise<T>() → std::pair<Promise<T>, Future<T>>;
```

**没有 `then`/`thenOn`/链式回调**（头文件中无该符号）：续接只能另起 Fiber 等待。

### 2.2 等待后端与超时

- `FutureWaitBackend { virtual bool Wait(lock, deadline, predicate); virtual void NotifyAll(); }`
  + `Set/ResetFutureWaitBackendFactory` + `CreateFutureWaitBackend`。
  默认实现 `DefaultFutureWaitBackend` 只是 `core::ParkingCondition` 的包装
  → **受管 Fiber 会 park 而不是阻塞 M**。
- 超时用 `Context::Now()` / `DeadlineAfter` / `MergeDeadline`（Context deadline 优先）。
- 每个等待者有独立的 `FutureWaitRegistration{ atomic<bool> cancelled }`，
  避免「一个等待者超时」污染其他等待者。
- 未发现与 `Timer` 的直接耦合（deadline 通过 Context 间接作用）。

### 2.3 坑点

1. 默认「不抛异常」：只有 `GetOrThrow` / `FutureError` 是显式异常边界。
2. `WaitFor` 超时**不改变 Future 状态**，可以再次等待；
   而 `Status()` 对「仍在 pending」的情况会返回 `kTimedOut` —— 容易误读为「已完成但超时」。
3. `BrokenPromiseError()` 是**进程级共享单例**：多个不同 broken promise 之间 `Is(a,b)==true`，无法区分来源。
4. `GetOrThrow` 要求 `T` 可复制（`static_assert`）；move-only 值只能走 `FutureResult::Value()` 指针。

---

## 3. `sync`

### 3.1 只有三个原语 + 一个可替换后端协议

```cpp
class WaitBackend {                            // 最小可替换等待后端
  virtual bool Park(void* scheduler, const std::shared_ptr<void>& task,
                    std::optional<ContextTimePoint> deadline,
                    const std::function<bool()>& predicate) = 0;
  virtual bool ParkForCleanup(void* scheduler, const std::shared_ptr<void>& task,
                              const std::function<bool()>& predicate);   // 默认转发 Park
  virtual void Wake(void* scheduler, const std::shared_ptr<void>& task) noexcept = 0;
};
bool SetWaitBackendFactory(WaitBackendFactory);  void ResetWaitBackendFactory() noexcept;
std::shared_ptr<WaitBackend> CreateWaitBackend() noexcept;

class Mutex final {                            // FIFO 混合互斥锁（Go 风格所有权）
  bool Lock(const ContextPtr& = {});           bool LockFor(ContextDuration, const ContextPtr& = {});
  bool TryLock() noexcept;                     void Unlock();
  void lock();  bool try_lock() noexcept;      void unlock();      // BasicLockable 兼容拼写
};
class ConditionVariable final {                // 与 Mutex 配套的调度器条件变量
  bool Wait(Mutex&, const ContextPtr& = {});   bool WaitFor(Mutex&, ContextDuration, const ContextPtr& = {});
  void NotifyOne() noexcept;                   void NotifyAll() noexcept;
};
class WaitGroup final {                        // 可复用等待组
  void Add(std::int64_t delta);   void Done();
  bool Wait(const ContextPtr& = {});  bool WaitFor(ContextDuration, const ContextPtr& = {});
  std::int64_t Count() const noexcept;
};
```

**没有** `Once` / 信号量 / 原子包装（全仓库的 `Once` 只指 `TimerRepeatMode::kOnce`）。

### 3.2 语义要点

- `Mutex`：Fiber 与普通线程**共用一个 FIFO 等待队列**；
  `m_state` 把 `locked`/`waiter` 两个状态压进**同一个原子字节**，
  消除「已设等待者标志但未入队 → Unlock 丢唤醒」的窗口；
  `Unlock` 不要求由同一 G/线程调用（Go 风格所有权），因此 **Fiber 加锁后可由别的线程解锁**。
- `ConditionVariable`：通知**不累积**（无等待者时通知丢失，与 Go `Cond.Signal` 一致）；
  取消后**仍强制重锁**再返回（返回值表示是否正常完成重锁，调用方必须检查）。
- `WaitGroup`：计数归零唤醒当前波次，之后 `Add` 可开启新波次；
  下溢或有符号溢出抛 `std::logic_error` 且**计数保持不变**。
- 复用 node 的并发安全：等待节点走 `thread_local` 缓存（上限 32），
  因此必须用 `generation` 在同一把 `m_wake_mutex` 内完成「检查 + 终态 + 唤醒」，
  否则旧回调会污染下一代的 `m_result`（源码注释明确记录了这次修复）。
- 队列必须持 `shared_ptr<WaitNode>`：`Unlock` 在释放队列锁后才异步 `Wake`，
  裸指针会在窗口内被回收/复用 → UAF（源码注释记录）。
- 公平性：`kCooperativeAttempts = 256` 的协作重试 + `kNativeSpinAttempts = 256` 的原生自旋
  ——**不是严格 FIFO**，不要把它当公平锁依赖。

### 3.3 `core::HybridMutex`（不是"线程/Fiber 混合锁"）

- 真身是 `core::detail::HybridGate`：`atomic_flag m_fast` 自旋（默认 64 次，`_mm_pause`/`yield`）
  失败后进 `std::mutex m_slow` 慢路径。
- **它没有任何 park 逻辑**：若在受管 G 上用它保护长临界区，G 会占满 carrier 线程自旋。
- 实际使用点只有 4 处内部「唤醒门」：`parking_condition.cpp` 的 `WaitNode::m_wake_mutex`、
  `sync.cpp` 的等待节点门、`io.cpp` 的 `wake_mutex`。
- ★ **用户决策（台账 X-35）**：框架**不允许** `HybridGate` 的实现与使用，需替换为 `sync::Mutex`
  （关键约束：不能用 `std::mutex`，因为存在 lock/unlock 跨线程的场景）。**本轮范围外，尚未执行。**

### 3.4 坑点

1. 手动 resume 的 Fiber（无 Scheduler/Task）**不受支持**：`LockImpl` 返回 false，
   `Mutex::lock()` 抛 `std::logic_error`，`ConditionVariable::Wait` 走 `abort_wait(true)` 保留锁并返回 false。
2. `Unlock()` 在未持锁时抛 `std::logic_error`；`WaitGroup::Add` 负值下溢/正溢出同样抛。
3. 锁与等待对象必须活得比所有等待者长；禁止把线程所有的 `std::mutex` 跨 Fiber yield/Hook IO。
4. `Mutex` 非递归，成功 `Lock` 必须恰好对应一次 `Unlock`。

---

## 4. 与 Go 的对应与差异

| Go | 本项目 | 差异 |
|---|---|---|
| `chan T` | `Channel<T>` | nil 通道返回 `kNil`（Go 永久阻塞）；关闭后发送返回状态（Go panic） |
| `select`（伪随机公平） | `Select(cases, ctx, timeout)` | **轮转**公平；内建 case 拒绝 move-only 类型；支持 Context 取消 |
| `close(ch)` | `Close() → ChannelStatus` | 幂等且返回状态码，不 panic |
| `sync.Mutex` | `sync::Mutex` | 无 goroutine 所有权、可跨执行流 Unlock、支持 Context 与超时、非严格 FIFO |
| `sync.Cond` | `ConditionVariable` | 通知不累积（一致）；取消后强制重锁并返回 bool |
| `sync.WaitGroup` | `WaitGroup` | 支持 Context/超时；溢出抛 `logic_error` |
| `sync.Once` / `sync/atomic` / `errgroup` | **无**（atomic 直接用 `std::atomic`） | 需要时用 C++ 原生设施 |
| 无 | `Future` / `Promise` | 一次性结果容器，无 then 链 |

---

## 5. 对架构图的贡献

**节点**

- `Channel<T>`（缓冲 deque + 发送/接收等待队列 + generation）
- `Select`（`SelectCase` 轮转探测 + `SelectWaitState` 单赢家）
- `SelectValue` / `Caster`（类型擦除与转换出口）
- `Future/Promise`（一次性结果 + `FutureWaitBackend`）
- `sync::Mutex` / `ConditionVariable` / `WaitGroup`（FIFO 混合队列 + `WaitBackend`）
- `core::HybridGate`（内部唤醒门；★用户已决定要移除）

**连线**

- `Channel → ParkingCondition`：「PendingSend/PendingRecv 内嵌 cv；受管 G park / 原生线程 cv」
- `Channel → Context`：「Done().AddCallback 唤醒等待者；FiberWaitUnsupported → kInvalid」
- `Select ⇄ Channel`：「ArmSelectRecv/ArmSelectSend + TrySelectPair 双锁单赢家」
- `Future → FutureWaitBackend → ParkingCondition`：「默认后端就是 ParkingCondition 包装」
- `sync::Mutex/CV/WaitGroup → WaitBackend → Scheduler`：「Park/ParkForCleanup/Wake 三入口」
- `Channel/sync → Error`：「状态码 + `ErrorPtr`（kCancelled/kDeadlineExceeded 等）」

**图内文字**

> 并发原语统一把「等待」委托给 `ParkingCondition`：受管 G 挂起让出 M，普通线程阻塞原生条件变量，
> 手动 Fiber 直接失败（绝不卡住 carrier）。`Channel` 支持容量 0（rendezvous）与有界 FIFO；
> `select` 用轮转 cursor + 单赢家 `SelectWaitState` 实现，**内建 case 只接受可复制值**（否则立即 `kInvalid`）。
> `sync` 的锁是 Go 风格所有权：**可以在 A Fiber 加锁、B 线程解锁**，但非严格 FIFO。
> `Future/Promise` 是 Go 没有的设施，且**没有 then 链**，续接靠另起 Fiber。
