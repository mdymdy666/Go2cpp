# 并发运行时（交接文档）

> **定位**：Go 风格协作式并发运行时——GMP 调度器 + 可挂起 Fiber + Channel/Select + 同步原语 + Context/Future/Timer。
> **源码**：`include/go2cpp/concurrency/`、`src/concurrency/`（约 2.0 万行实现）　**构建**：`cmake/Go2CppModules.cmake`，目标 `go2cpp_scheduler` / `go2cpp_fiber` / `go2cpp_channel` / `go2cpp_sync` / `go2cpp_future` / `go2cpp_context`
> **测试**：`tests/concurrency/`（88 个文件：`test_*` / `bench_*` / `stress_*`）
> **设计全文**：[scheduler.md](../../docs/modules/scheduler.md)、[fiber.md](../../docs/modules/fiber.md)、[channel.md](../../docs/modules/channel.md)、[sync.md](../../docs/modules/sync.md)、[select-semantics.md](../../docs/modules/select-semantics.md)、[timer.md](../../docs/modules/timer.md)
> **配套**：迁移手册 [concurrency-usage.md](../../docs/modules/concurrency-usage.md)，可运行示例 [user/concurrency/README.md](../concurrency/README.md)

## 1. 这个模块是什么

一句话：**把 Go 的 goroutine 模型搬进 C++**。业务代码只看见「G」（用户态执行体），
G 阻塞时挂起自己的栈并把 OS 线程（M）让出去，因此可以用同步写法写高并发程序，
既不需要 callback 状态机，也不需要「一连接一线程」。

它解决的三件事：

1. **并发度与线程数解耦**：10 万个 Fiber 共享 P 个 OS 线程，栈按页提交（默认 1 页 + 8 MiB 预留）。
2. **阻塞语义统一**：`sync::Mutex` / `Channel` / `Context` / `Future` / 网络 IO 在 G 上 park，
   在普通线程上退化为 futex，同一份代码两种执行体都能等。
3. **可观测**：G/M/P 三套状态机 + 快照 + 指标，卡死时能回答「谁在等谁」。

它**不**解决：不替换 `std::mutex`；不做异步抢占（没有安全点插入）；不接管未声明的原生阻塞调用。
所以迁移时第一件事是识别边界，而不是把 `std::thread` 换成 `go()` 就完事（见 §9）。

| 层 | 本项目对象 | 拥有什么 | 数量 |
|---|---|---|---|
| G | `scheduler::Task`（内含一个 `Fiber`） | 入口函数、栈、状态机、取消标记 | 用户提交量 |
| M | `Scheduler::Impl::Machine` + `std::thread` | 真正跑指令的线程 | `min_workers` 起，阻塞区补位，`max_workers` 封顶 |
| P | `Scheduler::Impl::Processor` | 本地 runqueue + 活动 M 计数 | `SchedulerConfig::processor_count`，默认硬件并发 |

## 2. 架构 / 数据流

```
                      go(fn) / Scheduler::spawn(fn)
                                  │
                    ┌─────────────▼──────────────┐
                    │  Task（G）：Fiber + 状态机  │
                    │  m_state_word: GState|wake  │
                    └─────────────┬──────────────┘
                      入队（worker 内）│ 入队（外部线程）
                                  ▼
        ┌──────────────────────┐   ┌──────────────────────────┐
        │ P 本地队列 deque（P 锁）│   │ incoming_queues[16] 条带 │
        └──────────┬───────────┘   └───────────┬──────────────┘
                   │ 本地快路径                 │ incoming 中路径
        ┌──────────▼───────────────────────────▼──────────────┐
        │ admission 慢路径（持 Impl::mutex）                    │
        │ 紧急救援链 → 本地 FIFO → 全局 FIFO → work stealing    │
        └──────────┬──────────────────────────────────────────┘
                   │ 出队 + Runnable→Running claim
                   ▼
        ┌──────────────────────────────────────────────────┐
        │ M（OS 线程 worker_loop）                          │
        │   run() → Fiber::resume() → 用户函数              │
        │   park 挂起栈 / yield 回本 P 队首（不 notify）     │
        └──────────┬───────────────────────────────────────┘
                   │ 空闲
                   ▼
      IO idle poller（在本 M 上 poll） 或 ParkingCondition 睡到 idle_wait
```

### 2.1 park / wake 的四个不变量

1. **契约检查先于状态转换**：在 `SpinMutex` 临界区（`no_park_depth > 0`）里 debug abort、
   release 降级计数；持普通 `Mutex` 时计数 + 一次性告警。**先消费 pending wake 再挂起**：
   `m_wake_pending.exchange(false)` 为真则返回 `kRequeued`，留在当前执行流——这是
   「先通知后挂起」不丢唤醒的唯一手段。
3. **wake 不碰目标 Fiber 栈**：`Waiting→Runnable` 只改状态并入队，`Running` 只置令牌位；
   真正的 `resume` 由 `m_execution_claim` 串行化。**状态与令牌在同一次 CAS 里**
   （`m_state_word` 低 8 位是 `GState`、bit 8 是 wake 令牌）——拆成两个原子会留下
   「令牌已发布、挂起已提交、却没人消费」的窗口。

### 2.2 线程模型（交接重点）

- **worker 数不是固定的**：`min_workers`（默认 = P 数）常驻；`BlockingRegion` 或 sysmon 判定
  长阻塞后拉起补充 M，受 `max_workers`（默认 `2*P`）封顶；`allow_idle_shrink` 默认 **false**。
- **一个 G 同一时刻最多一个执行者**，但可以跨 M 迁移（M 亲和只影响投递顺序）。**sysmon**
  是独立线程，默认 10 ms 一拍，只观测不抢占。
- **IO 有两条路径**：独立 poller 线程 + `wake_io()` 跨线程唤醒（WSL2 实测一次往返
  54–56 µs）；或 `set_io_idle_poller()` 让空闲 worker 自己在 M 上 poll（Go `netpoll` 模型）。

## 3. 对外 API（关键接口）

### 3.1 提交任务

```cpp
// include/go2cpp/tools/go.hpp —— 新手入口，隐式默认调度器
std::shared_ptr<Task> go(Task::Function function);
template <typename F> std::shared_ptr<Task> go(F&& function);      // lambda 直接传
std::shared_ptr<Task> go(Scheduler& scheduler, Task::Function function);
core::Timer after(std::chrono::steady_clock::duration delay, Task::Function function);
core::Timer every(std::chrono::steady_clock::duration period, Task::Function function,
                  core::TimerOptions options = {});
void shutdown_default_scheduler() noexcept;
```

`fiber` 是可先构造、后 `bind()` 的句柄：`fiber f(fn); f.bind(scheduler); f.wait();`。
**默认调度器是进程级的**，`shutdown_default_scheduler()` 是终态，之后再 `go()` 会新建实例。

### 3.2 调度器

```cpp
// include/go2cpp/concurrency/scheduler/scheduler.hpp
explicit Scheduler(SchedulerConfig config = {});
void start();
bool run_main(Task::Function function);   // 当前线程当 m0 跑 main G，返回即停机
void shutdown();
bool shutdown_for(std::chrono::steady_clock::duration timeout);
std::shared_ptr<Task> spawn(Task::Function function);
std::shared_ptr<Task> spawn(Task::Function function, TaskOptions options);
bool yield_current();  bool park_current();
bool park_wait(const std::shared_ptr<Task>& task);           // 同步原语等待
bool wake(const std::shared_ptr<Task>& task);
bool wake_io(const std::shared_ptr<Task>& task);             // IO 轮询器专用
bool cancel(const std::shared_ptr<Task>& task);
std::vector<TaskSnapshot> DebugSnapshotTasks() const;
TaskTerminalState TaskTerminal(GId id) const;
static Scheduler* current_scheduler() noexcept;
static std::shared_ptr<Task> current_task() noexcept;
```

`SchedulerConfig` 关键默认值：`processor_count=0`（→ 硬件并发）、`max_workers=0`（→ `2*P`）、
`min_workers=0`（→ P）、`local_queue_limit=256`、`idle_wait=10ms`、`fiber_stack_size=2048`
（页对齐后 1 页）、`fiber_stack_reserve_size=8 MiB`、`fiber_stack_growable=true`。

### 3.3 Fiber

```cpp
// include/go2cpp/concurrency/fiber.hpp
explicit Fiber(Function function, std::size_t stack_size = DefaultStackSize(),
               std::size_t stack_reserve_size = 0, bool stack_growable = true);
bool resume() noexcept;                       // 串行；并发 resume 第二个返回 false
FiberResumeResult resume_result() noexcept;   // 带失败原因与父链快照
static bool Suspend(SuspendReason reason = SuspendReason::Yield) noexcept;
static Fiber* Current() noexcept;
static std::uint64_t LockToCurrentM() noexcept;   // 等价 runtime.LockOSThread
static constexpr std::size_t DefaultStackSize() noexcept;        // 128 KiB
static constexpr std::size_t DefaultStackReserveSize() noexcept; // 8 MiB
```

### 3.4 Channel / Select

```cpp
// include/go2cpp/concurrency/channel.hpp
template <typename T> ChannelPtr<T> MakeChannel(std::size_t capacity = 0);
ChannelSendResult Send(T value, const ContextPtr& context = {});
ChannelSendResult SendFor(T value, Duration timeout, const ContextPtr& context = {});
ChannelRecvResult<T> Recv(const ContextPtr& context = {});
ChannelRecvResult<T> RecvFor(Duration timeout, const ContextPtr& context = {});
ChannelSendResult TrySend(T value);   ChannelRecvResult<T> TryRecv();
ChannelSendResult Close() noexcept;

SelectResult Select(const std::vector<SelectCase>& cases,
                    const ContextPtr& context = {},
                    std::optional<ContextDuration> timeout = std::nullopt);
SelectCase RecvCase(const ChannelPtr<T>& channel);
SelectCase SendCase(const ChannelPtr<T>& channel, T value);
SelectCase DefaultCase();  SelectCase DoneCase(const ContextPtr& ctx);
SelectCase AfterCase(ContextDuration timeout);
```

**语义要点**（细节见 select-semantics.md）：`Send/Recv` 成功返回 `kReady`；读取已关闭的
Channel 先读完缓冲再 `kClosed`；`Recv` 返回值有 `operator bool`（= `Ok()`），配合
`while (auto got = ch->Recv())` 即可模拟 `range ch`。排序规则：**有值/可发送 > closed/nil >
default > 取消/超时**——closed 不会遮蔽同一次 select 里另一个通道的缓冲值（有意差异，修复 D1）。

### 3.5 同步原语 / Context / Future

```cpp
// include/go2cpp/sync.hpp（namespace go2cpp::sync）
class Mutex { bool Lock(); bool Lock(const ContextPtr&); bool LockFor(ContextDuration, const ContextPtr& = {});
              bool TryLock() noexcept; void Unlock(); };
class ConditionVariable { bool Wait(Mutex& mutex, const ContextPtr& context = {});
                          void NotifyOne() noexcept; void NotifyAll() noexcept; };
class WaitGroup { void Add(std::int64_t delta); void Done(); bool Wait(const ContextPtr& context = {}); };

// include/go2cpp/concurrency/spin_mutex.hpp
class alignas(64) SpinMutex { void Lock(); bool TryLock() noexcept; void Unlock(); };  // 临界区内禁止 park

// include/go2cpp/concurrency/context.hpp（namespace go2cpp）
ContextPtr Background();
std::pair<ContextPtr, CancelFunc> WithCancel(const ContextPtr& parent);
std::pair<ContextPtr, CancelFunc> WithTimeout(const ContextPtr& parent, ContextDuration timeout);
void Cancel(ErrorPtr cause = {});   // 成员；幂等，向子树传播
bool IsDone() const noexcept;  ErrorPtr Err() const;  ErrorPtr Cause() const;// include/go2cpp/concurrency/future.hpp（namespace go2cpp）
template <typename T> std::pair<Promise<T>, Future<T>> MakePromise();
bool Promise<T>::SetValue(U value);  bool SetError(ErrorPtr);  bool SetException(std::exception_ptr);
FutureResult<T> Future<T>::GetResult(const ContextPtr& context = {}) const;
FutureResult<T> Future<T>::GetResultFor(ContextDuration timeout, const ContextPtr& = {}) const;
```

`sync::Mutex` **不可重入且不记录 owner**（Go 风格：A 加锁、B 可解锁）；`Context::IsDone`
是一次原子读（0.42 ns）；`Future` 内部永远返回状态，只有 `GetOrThrow` 才转异常。

## 4. 最小用法（可编译示例）

```cpp
#include "go2cpp/runtime.hpp"
#include <chrono>
#include <iostream>

int main() {
    using namespace go2cpp;
    using namespace std::chrono_literals;

    Scheduler scheduler(2);            // P = 2
    scheduler.start();
    auto jobs = MakeChannel<int>(4);   // 有缓冲：前 4 次 Send 不阻塞
    auto done = MakeChannel<int>(1);
    sync::WaitGroup wg;
    wg.Add(1);

    go(scheduler, [&] {                // 在 Fiber 上执行，可 park
        for (int i = 1; i <= 3; ++i) { (void)jobs->Send(i * 10); }
        (void)jobs->Close();           // 消费者按 range 语义退出
        (void)done->Send(1);
        wg.Done();
    });

    int sum = 0;
    while (auto got = jobs->Recv()) { sum += *got.value; }
    const auto sel = Select({RecvCase(done), DefaultCase()}, {}, 100ms);
    wg.Wait();                         // 必须等 Fiber 结束再 shutdown
    scheduler.shutdown();
    std::cout << "sum=" << sum << " selected=" << sel.selected << '\n';
    return sum == 60 ? 0 : 1;
}
```

编译（完整示例工程见 [user/app/CMakeLists.txt](../app/CMakeLists.txt)）：

```bash
g++ -std=c++17 -O2 -I include demo.cpp \
    build/libgo2cpp_scheduler.a build/libgo2cpp_fiber.a build/libgo2cpp_channel.a \
    build/libgo2cpp_sync.a build/libgo2cpp_context.a -lpthread
```

## 5. 源码位置

| 文件 | 行数 | 职责 |
|---|---:|---|
| `include/go2cpp/concurrency/scheduler/scheduler.hpp` | 1309 | 公开接口真身：状态枚举、`SchedulerConfig`、`Task`、`Scheduler`、快照与指标 |
| `src/concurrency/scheduler.cpp` | 6528 | GMP 实现：任务状态机、P/M/队列/窃取、worker_loop、sysmon、park/wake、shutdown |
| `include/go2cpp/concurrency/fiber.hpp` / `src/concurrency/fiber.cpp` | 552 / 4074 | 用户态栈、上下文切换、取消标记、嵌套父链、FiberBin 复用 |
| `src/concurrency/context_x64.S` | 83 | x86_64 汇编上下文切换（17 条指令的 `JumpContext`） |
| `include/go2cpp/concurrency/channel.hpp` / `src/concurrency/channel.cpp` | 4305 / 829 | `Channel<T>`、环形缓冲、等待节点、`Select` 状态机、方向视图 |
| `include/go2cpp/concurrency/sync.hpp` / `src/concurrency/sync.cpp` | 545 / 3068 | `Mutex` / `ConditionVariable` / `WaitGroup` 的 Fiber/线程双模实现 |
| `include/go2cpp/concurrency/context.hpp` / `src/concurrency/context.cpp` | 592 / 2732 | 取消树、`DoneSignal`、值继承、截止时间 |
| `include/go2cpp/concurrency/future.hpp` / `src/concurrency/future.cpp` | 1237 / 162 | `Promise`/`Future` 共享状态与等待后端 |
| `include/go2cpp/concurrency/spin_mutex.hpp` | 489 | 禁止 park 的自旋锁，持锁 park 检测的数据源 |
| `include/go2cpp/concurrency/core/parking_condition.hpp` / `src/concurrency/parking_condition.cpp` | 214 / 1027 | futex 化条件变量（worker 空转等待） |
| `include/go2cpp/concurrency/core/timer.hpp` / `src/concurrency/timer.cpp` | 505 / 1343 | 定时任务服务、`Timer` 句柄、固定延迟/固定频率 |
| `include/go2cpp/tools/go.hpp` | 310 | `go()` / `after()` / `every()` / `fiber` 句柄与默认调度器管理 |

## 6. 性能数据

实测来源：[docs/reports/vs-concurrency.md](../../docs/reports/vs-concurrency.md)（2026-11-01，
AMD Ryzen 9 8945HX / WSL2 12 vCPU，`taskset -c 2,3` 绑同一物理核的两个 SMT 兄弟，5 轮取 min）：

| 原语 | 我们 | 最强对手（同机） | 判定 |
|---|---:|---:|---|
| 裸上下文切换（ns/往返） | **6.43** | coost 2.79 / libco 24.49 / Boost 8.46 | 第二，输 coost 2.30× |
| Fiber 全量切换（ns/往返） | **33.80**（后续快照 23.5） | libco 24.49 / libfiber 53.38 | 第二，输 libco 1.38× |
| Channel 1P1C cap1024（ns/条） | **10.73** | Go 23.36 / muduo 32.40 | **第一**，赢 Go 2.18× |
| Mutex 无竞争（ns/对） | **3.75** | Go 3.24 / std 4.54 | 持平 |
| ConditionVariable 热路径（ns/往返） | 287.20 | Go 174.85 / std 57882 | 输 Go 1.64×，赢 std 201× |
| WaitGroup Add/Done 1t/4t/16t（ns） | 3.77 / 9.49 / 19.64 | Go 5.31 / 12.28 / 19.42 | 1t、4t 赢 |
| Context `IsDone`（ns） | **0.42** | Go 6.24 | **赢 14.9×**（`WithCancel` 创建 265.6 反而输 3.5×） |
| 调度器任务（ns/任务） | 685.03（后续快照 326.7） | Go 135.49 | **输 5.06×（最大短板）** |

> ⚠️ **2026-11-03 复核**：Go 135.49 ns 是 **min** 统计（median 185）、`bench_go.go` 已丢失，口径不可考；5.06× 仅作历史对照。本机 Go@4 基线见 `docs/metrics-ledger.md` M16（151–163 ns）。

指令级证据见 [2026-11-01-asm-concurrency-review.md](../../docs/reports/2026-11-01-asm-concurrency-review.md)
（`objdump -d` 数指令，GCC 13.3 `-O3 -DNDEBUG`）：`Scheduler::spawn` **194** 条指令、
`Scheduler::enqueue` **789** 条 / 栈帧 **712 B**、`Scheduler::wake_registered` **294** 条、
`Task::run` **441** 条 / 760 B、`Task::wait()` **170** 条、`sync::Mutex::lock()` 快路径 **26** 条；
对照 Go `runtime.newproc` 只有 **19** 条。

结论：**数据面（Channel / 裸切换 / Mutex 快路径 / Context 读）已达或超过工业库；控制面
（任务生命周期、CV 热路径、Context 创建）落后 Go 一档到一档半**。fiber.md §11 的
「Fiber 切换 ~500 ns vs 线程 ~28.6 µs（57×）」是另一套口径（`docs/switch-benchmark.md`），
**不可与上表 33.80 ns 横比**。

## 7. 边界与已知限制

1. **无异步抢占**：CPU 密集的 G 只能靠 `yield`/`park` 让出；sysmon 只处理已声明的阻塞。
   **未声明的阻塞观测不到**——`BlockingRegion` 是唯一信号，忘了写就等于调度器瞎了。
2. **临界区内禁止 park**：持 `Mutex` 时做 IO / `Sleep` / 阻塞 channel 操作会形成
   「持锁消失」型死锁（Go2Cpp 特有）；`SpinMutex` 内的 park 会被检测并降级。
3. **Fiber 栈三个硬边界**：ASan/TSan 构建强制固定栈（不装 SIGSEGV handler）；growable 用
   **进程级** SIGSEGV handler，与用户 handler/JIT 冲突（`GO2CPP_GROWABLE_STACK=0` 回退）；
   >256 KiB 的 `alloca`/VLA 直接 fatal，不增长。
4. **`Channel<T>` 要求 `T` nothrow move + nothrow destruct**；内建 `RecvCase/SendCase` 还要求
   `T` 可复制（move-only 走 `SelectValue` 自定义路径），`SelectResult::Value<T>()` 对 move-only
   返回 `nullopt`，必须用 `TakeValue<T>()`。
5. **nil 通道语义不同**：`SendChecked/RecvChecked` 对空 `ChannelPtr` 返回 `kNil`，**不会**像
   Go 的 nil channel 一样永久阻塞。**手动 Fiber 也不能 park**：未归属 Scheduler 的 `Fiber` 上
   `Send/Recv` 返回 `kInvalid`、`Mutex::Lock` 返回 false——没有等待后端就不假装能等。
6. **跨 M 唤醒是微秒级**（4~5 µs）：`yield` 回来的 G 刻意不 notify，否则 64 Fiber 场景整档慢
   10 倍；`ConditionVariable` 热路径 287 ns、`NotifyAll` 即使 0 等待者也要一次队列 swap。
7. **定时器单线程**：一个长期阻塞的回调会推迟该服务所有后续任务；`Timer` 句柄析构即取消
   （`service.ScheduleAfter(1ms, cb);` 丢弃返回值 = 回调永不执行）。

## 8. 测试与验证方法

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DGO2CPP_BUILD_TESTS=ON
cmake --build build -j
ctest --test-dir build -R "channel|scheduler|fiber|sync|context|future|timer" --output-on-failure
./build/test_scheduler        # 也可单跑：启停/窃取/取消/快照
./build/test_select_lockorder # Select 的锁序与回滚
```

按主题挑文件（都在 `tests/concurrency/`）：

| 主题 | 代表文件 |
|---|---|
| 调度器 / Fiber | `test_scheduler.cpp`、`test_scheduler_stress.cpp`、`test_dynamic_scheduler.cpp`、`test_fiber.cpp`、`test_fiber_growable_stack.cpp`、`fiber_tsan_migrate.cpp` |
| Channel/Select | `test_channel.cpp`、`test_channel_concurrency.cpp`、`test_fiber_select.cpp`、`test_select_value_first.cpp` |
| 同步 / Context / 压测 | `test_sync.cpp`、`test_parking_condition_stress.cpp`、`test_context.cpp`、`test_timer.cpp`、`high_load_stress.cpp`、`bench_park_wake_opt.cpp` |

TSAN/ASAN 有现成脚本：`tests/concurrency/run_tsan.sh`、`run_random_5rounds.sh`、
`build_and_run_tests.sh`。**并发改动必须至少跑一遍 TSAN**——丢唤醒在普通构建里只表现为
偶发 hang。观测用 `scheduler.processors()` / `DebugSnapshotTasks()` 打印 G/M/P 状态，
`ch->DebugSnapshot()` 给出缓冲长度、等待者数与 select 注册数。

## 9. 交接注意

- **先读 park/wake 协议再看代码**：`Task::park_for_scheduler()` / `wake_for_wait()` /
  `m_state_word` 三处是一个整体，改一处必须同步另外两处，否则会引入丢唤醒或双执行。
- **`Scheduler::shutdown()` 是终态**：同一实例不能再次 `start()`；`shutdown_for()` 超时返回
  false 时可稍后再调，但 Fiber 栈不会因超时被释放。
- **不要给 yield 回来的 G 加 `notify_one`**：这个「优化」会立刻把本地续跑变成跨 M 唤醒（4~5 µs），
  `scheduler.cpp` 里注释最多的一处就是它。
- **`enqueue` 是当前最贵的热路径**（789 条指令 / 712 B 栈帧）：可省项已列在
  `2026-11-01-asm-concurrency-review.md`（`current_task()` 换 `current_task_ptr()`、
  去掉非平凡 `thread_local` 的 guard），照单执行即可，别重复分析。
- **Fiber 析构可能长时间阻塞**：主体忽略取消就无法强制终止（为了 C++ RAII 语义）；
  `DestroyTimeoutCount() > 0` 说明有人违反了「owner 长于所有 resume()」的契约。
  **默认调度器故意进程级泄漏**（避免静态析构顺序问题），不要「顺手」改成 `static Scheduler`。
- **未完成事项**：异步抢占（安全点）、`local_queue_limit` 对 incoming 条带无效、跨进程迁移仍是
  实验特性（`GO2CPP_MIGRATION_API` 默认关）、CV 热路径与 `Context::WithCancel` 创建仍是落后项。
