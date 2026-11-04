# 并发（concurrency）

> **一句话定位**：调度器 / Fiber / Channel / Context / Sync。
> 本文是「模块 → 能力 → 异常」手册；[并发深入文档](../../deep/01-concurrency.md)


> 这个目录是「并发运行时」的人手示例：一台用 C++17 实现的 GMP 调度器，
> 以及长在它上面的 Channel、Select、Context、同步原语、Future、定时器。

## 能力清单

Go 的并发能力由三件东西组成：**goroutine（轻量执行体）+ channel（通信）
+ context（取消传播）**。本模块把这套语义在 C++ 里重建了一遍：

- 一个 `Scheduler` 负责「下一个跑谁」（GMP 三层：G 任务 / P 逻辑处理器 / M OS 线程）；
- `Fiber` 负责「怎么把 CPU 交给它、再拿回来」（独立栈 + 汇编换栈）；
- `Channel<T>` / `Select` 提供与 Go `hchan` 对齐的通信语义；
- `Context` 提供取消树，`sync` 提供 Fiber 与普通线程都能用的锁/条件变量。

它解决的问题是：**在 C++ 里写出「几万个任务、阻塞式代码风格、不占线程」的服务端逻辑**。

## 底层架构

### 三层调度（GMP）

```
   go(fn) / scheduler.spawn(fn)
            │
            ▼
   ┌────────────────┐   投递    ┌──────────────┐   窃取   ┌──────────────┐
   │   Scheduler    │ ────────► │  P 的本地队列 │ ◄────── │ 其他 P 的队列 │
   │ (src/concurrency│          └──────┬───────┘          └──────────────┘
   │  /scheduler.cpp)│                 │ 取 G
   └────────────────┘                 ▼
                              ┌──────────────┐
                              │ M（OS 线程）  │ ──► 执行 G = Fiber
                              └──────┬───────┘
                                     │ 阻塞（Channel/Select/Sync/Timer/IO）
                                     ▼
                              ┌──────────────────────┐
                              │ core::ParkingCondition│  Fiber park，让出 M
                              └──────────┬───────────┘
                                         │ 条件满足 / 超时 / 取消
                                         ▼
                                   wake ──► 重新入队
```

- **P 数量** = 真正的并行度（`SchedulerConfig` 的 processor 数）；
- **M 可增可减**：有任务挤压时扩，空闲超过 `idle_worker_timeout` 后回落到 `min_workers`；
- **普通线程也能参与**：`ScopedThreadParticipation` 在作用域内把当前线程标记为可参与 GMP；
  长阻塞用 `BlockingRegion` 声明，调度器会补一个 M 来服务队列，避免 P 停摆。

### Fiber（执行载体）

- 每个 Fiber 一段**独立栈**（默认 128 KiB，**固定不增长**）+ 一份保存的寄存器上下文；
- 换栈在 `src/concurrency/context_backend.cpp` 与 `src/concurrency/context_x64.S`；
- 栈**池化复用**（`FiberBin`），摊掉重复建栈开销；这与 coost 的共享栈是相反的取舍；
- `FiberLocal<T>` 提供「Fiber 维度的 thread_local」，park/resume 后仍存活。

### Channel 与 Select

- `Channel<T>` 的缓冲是**预分配环形数组 + sendx/recvx**（对齐 Go 的 `hchan`），
  刻意不用 `std::deque`：少一层间接，关闭/排空顺序与 Go 一致；
- 阻塞走 `core::ParkingCondition`：受管 G 在此 park 并让出 M，普通线程走原生条件变量；
- `Select` 的执行顺序是 **先轮询 → 再登记等待节点 → 最后 park**，
  所以「多个 case 同时就绪」与「超时/取消」都能在一轮里定下来；
- 结果类型是 `SelectResult`（`selected` + `status`），不靠异常表达超时。

### 取消与同步

- `Context` 是不可变的取消树节点：`WithCancel`/`WithTimeout`/`WithDeadline` 派生，
  父取消级联到全部子节点；
- `DoneSignal::AddCallback` 是 Fiber 等待取消的挂载点；**回调在通知线程上同步执行，
  不能在里面做阻塞操作**；
- `sync::Mutex`/`ConditionVariable`/`WaitGroup` 同时服务 Fiber 与 OS 线程，
  采用 Go 风格所有权：**解锁不要求同线程**（这正是 `std::mutex` 不能替代它的原因）。

## 最小示例

```cpp
#include "go2cpp/runtime.hpp"

#include <chrono>
#include <iostream>

int main() {
    using namespace go2cpp;
    using namespace std::chrono_literals;

    Scheduler scheduler(2);          // 2 个 P
    scheduler.start();

    auto ch = MakeChannel<int>(0);   // 无缓冲：发送方等接收方取走

    auto sender = go(scheduler, [ch] { (void)ch->Send(2026); });

    SelectLoop loop;
    loop.bind(RecvCase(ch), [](const SelectResult& r) {
        if (const auto v = r.Value<int>()) std::cout << "收到 " << *v << '\n';
    });
    (void)loop.work({}, 1s);         // 等待事件，最多 1 秒

    sender->wait();
    scheduler.shutdown();
    return 0;
}
```

不想显式管理调度器时用进程级默认调度器：`go([]{ ... })`，
结束前调用 `shutdown_default_scheduler()`。
完整可运行版本见 `beginner_demo.cpp`。

## 示例清单

全部注册进 ctest，目标名 = `go2cpp_<文件名>`：

```sh
cmake --build /tmp/build-user --target go2cpp_beginner_demo -j
/tmp/build-user/go2cpp_beginner_demo
```

| 示例（`demo`=简单版 / `full`=全面版） | 演示什么 |
| --- | --- |
| `beginner_demo` | 最短入门：`go()`、Channel、SelectLoop、超时、nil Channel |
| `runtime_basic_demo` | 只包含 `runtime.hpp` 聚合头即可用多个子系统 |
| `runtime_demo` | 端到端：Context + Channel + Timer + Select + 错误包装 |
| `runtime_full` | 聚合头的 7 个跨模块协同场景 |
| `scheduler_demo` / `scheduler_full` | 启动/提交/等待/规模查询/关闭；6 个调度器场景 |
| `dynamic_gmp_demo` / `dynamic_gmp_demo_new` / `dynamic_gmp_full` | Worker 按负载扩缩、空闲回收、任务亲和、取消竞态 |
| `fiber_sync_demo` | Fiber 与普通线程共用 Mutex / ConditionVariable / WaitGroup |
| `mixed_runtime_demo` | 普通线程与 Fiber 混合使用同步原语、FiberLocal、BlockingRegion |
| `managed_pipeline_demo` | 带超时 Context 的无缓冲生产者/消费者管线 |
| `channel_demo` / `channel_full` | 无缓冲交接、有缓冲队列、Close、select 多路复用 |
| `context_demo` / `context_full` | WithCancel 级联、WithTimeout、WithValue、Deadline |
| `context_rollback_demo` | 局部回滚 + savepoint + 补偿动作（六个场景） |
| `sync_demo` / `sync_full` | Mutex 互斥、跨执行流解锁、LockFor 超时、WaitGroup |
| `future_demo` / `future_full` | Promise/Future 的成功、超时、错误、异常、broken promise |
| `timer_task_demo` / `timer_full` | 一次性/周期/动态重排/取消/weak_ptr/异常隔离 |
| `parking_condition_demo` / `parking_condition_full` | 等待-通知适配器：受管 G 等待不阻塞 M、notify_all、超时 |
| `thread_policy_demo` / `thread_policy_full` | 线程策略查询、ScopedThreadParticipation、Hook 模式切换 |
| `fiber_local_demo` / `fiber_local_full` | FiberLocal 按需构造、两 Fiber 隔离、park 后存活 |
| `fiber_stack_limit_demo` | Fiber 栈固定 128 KiB：量安全深度、递归改迭代、限深拒绝 |
| `fiber_stack_limit_full` | fork 二分实测临界深度、换 Fiber 续跑、20 万层迭代解析 |
| `futex_mutex_demo` | 内部唤醒门 `core::FutexMutex`：CAS 快路径 + futex 慢路径 |

带 `full` 的是同一组件的多场景版（通常 6–8 个场景，含错误/边界路径）；
`demo` 版只有 3 个左右的主干场景，适合第一次读。

## 3 个常见异常

### 1. 向已关闭的 Channel 发送
- **现象**：数据静默丢失，或调用方以为发送成功。
- **原因**：`Send` 返回 `kClosed`，**不 panic、不抛异常**；返回值带 `[[nodiscard]]`，忽略后逻辑会继续。
- **处理**：必须检查返回值；需要「失败即异常」时用 `SendOrThrow`；关闭后先 drain 再退出。

### 2. 普通线程里直接用 Channel / Mutex
- **现象**：整个线程卡住；单线程「先 Send 后 Recv」必定自死锁。
- **原因**：非 Fiber 上下文 park 走 native futex 路径，阻塞整个 OS 线程，而不是让出 M。
- **处理**：把逻辑放进 `go()`；或用有缓冲 channel + `TrySend`；或用 `BlockingRegion` 标记；
  必须同步等待时用 `SendFor`/`RecvFor` 超时。

### 3. `WithCancel` 后忘记 `cancel()`
- **现象**：等待 `Done()` 的 goroutine 永久 park，内存/句柄不释放。
- **原因**：Context **不会**随作用域自动取消，取消是显式动作。
- **处理**：用 `CancelGuard`（RAII）或 defer cancel；有界场景改用 `WithTimeout`；
  等待方同时检查 `Done()` 与 `Err()`。

## 异常处理示例（默认行为 + 可选策略）

这三个示例回答同一个问题：**真实场景里出错时，模块默认怎么处理，调用方
还能怎么选**。每个文件头都写明「场景 / 默认行为（file:line）/ 可选策略 /
如何选择」，代码可独立编译运行。

| 示例 | 场景 | 默认行为 | 可选策略 |
| --- | --- | --- | --- |
| `01-send-on-closed.cpp` | 向已关闭的 Channel 发送 | `Send` 返回 `kClosed`，不 panic、不抛异常（`channel.hpp:2894`）；返回值 `[[nodiscard]]`（`channel.hpp:521`） | 检查返回值并记录 / `SendOrThrow`（`channel.hpp:1679`）/ 关闭后 drain / `SendFor` 超时 |
| `02-blocking-in-thread.cpp` | 普通线程（非 Fiber）里用 Channel/Mutex | park 走 native futex 路径，阻塞整个 OS 线程（`parking_condition.cpp:407`）；单线程「先 Send 后 Recv」必自死锁 | 放进 `go()` / 带缓冲 + `TrySend` / `BlockingRegion` / `SendFor`·`RecvFor` 超时 |
| `03-context-cancel-leak.cpp` | `WithCancel` 后忘记 `cancel()` | Context 不会自动取消，等待 `Done()` 的 goroutine 永久 park（`context.hpp:230`、`:304`） | `CancelGuard`（defer cancel）/ `WithTimeout` / 主动检查 `Done()`·`Err()` / 第二个退出信号 |

编译与运行（这三个示例尚未注册进 ctest，用已构建好的库直接编）：

```sh
ROOT=/UserData/CodexWorkSpace/Go2cpp
BUILD=${BUILD:-$ROOT/.build-bento-wsl}   # 任一已编译好的构建目录
g++ -std=c++17 -Wall -Wextra -Wpedantic -I"$ROOT/include" \
  "$ROOT/user/modules/concurrency/01-send-on-closed.cpp" \
  -L"$BUILD" -Wl,-rpath,"$BUILD" \
  -lgo2cpp_channel -lgo2cpp_context -lgo2cpp_scheduler -lgo2cpp_fiber \
  -lgo2cpp_sync -lgo2cpp_error -lgo2cpp_debug -lgo2cpp_debug_event \
  -lgo2cpp_io -lgo2cpp_hook -lgo2cpp_config -lgo2cpp_network \
  -lgo2cpp_log -lgo2cpp_future -lgo2cpp_control_flow -lgo2cpp_os \
  -lgo2cpp_math -lgo2cpp_rand -lgo2cpp_path -lgo2cpp_container \
  -lgo2cpp_compress -lgo2cpp_encoding -lgo2cpp_metrics \
  -lgo2cpp_relay_pipe -lgo2cpp_os_monitor -lgo2cpp_hash \
  -lpthread -ldl -o /tmp/01-send-on-closed
/tmp/01-send-on-closed
```

三个示例都在当前树实测通过（退出码 0、`-Wall -Wextra -Wpedantic` 零告警）。
`01` 里的 `[[nodiscard]]` 可以这样自查：把某处写成 `ch->Send(v);`，
`-Wall` 会给出 `-Wunused-result`，并指向 `channel.hpp:1626` 与 `:521`。

## 源码位置

| 能力 | 头文件 | 实现 |
| --- | --- | --- |
| Scheduler | `include/go2cpp/concurrency/scheduler/scheduler.hpp` | `src/concurrency/scheduler.cpp` |
| Fiber | `include/go2cpp/concurrency/fiber.hpp` | `src/concurrency/fiber.cpp` |
| 换栈 | `include/go2cpp/concurrency/fiber.hpp`（`context_backend`） | `src/concurrency/context_backend.cpp`、`context_x64.S` |
| FiberLocal | `include/go2cpp/concurrency/fiber_local.hpp` | `src/concurrency/fiber_local.cpp` |
| Channel / Select | `include/go2cpp/concurrency/channel.hpp`（header-only） | — |
| Context | `include/go2cpp/concurrency/context.hpp` | `src/concurrency/context.cpp` |
| 同步原语 | `include/go2cpp/sync.hpp` | `src/concurrency/sync.cpp` |
| Future / Promise | `include/go2cpp/concurrency/future.hpp` | `src/concurrency/future.cpp` |
| 定时器 | `include/go2cpp/core/timer.hpp` | `src/concurrency/timer.cpp` |
| park/wake | `include/go2cpp/core/parking_condition.hpp` | `src/concurrency/parking_condition.cpp` |
| 线程策略 | `include/go2cpp/thread_policy.hpp` | `src/concurrency/thread_policy.cpp` |
| 聚合头 | `include/go2cpp/runtime.hpp` | —（只 include，不含 API） |

设计取舍与演进历史见 `docs/PROGRESS-ROUND2.md`、`docs/modules/fiber.md`、`docs/fiber_guide.md`。

## 注意事项

- **必须 `start()` 之后提交任务，结束前 `shutdown()`**；用默认调度器则结尾调
  `shutdown_default_scheduler()`，否则进程退出时仍有 Fiber 在跑。
- **Fiber 栈固定、不增长**：深递归、超大局部数组会直接踩爆栈（没有 SIGSEGV handler
  兜底）。先读 `fiber_stack_limit_demo.cpp` 再写递归代码；`FiberOptions` 可以调大栈。
- **不要在 Fiber 里做未被 Hook 覆盖的阻塞调用**（文件 IO、第三方库内部阻塞等），
  它会连同 M 一起卡住；用 `BlockingRegion` 显式声明，或把工作扔到普通线程。
- **Channel 的值语义**：`Select` 内建的 `SendCase`/`RecvCase` 要求 `T` 可复制，
  move-only 类型请用普通 `Send/Recv` 或独立的 `SelectValue`；
  否则 select 会返回 `kInvalid` 而不是永久等待。
- **错误用返回值而不是异常**：`ChannelStatus`/`SelectResult`/`FutureStatus` 都是显式状态，
  超时是 `kTimedOut`，取消是 `kCancelled`，不要把两者混为一谈。
- **示例日志里的「失败状态」是教学内容**，不代表进程失败；每个示例以退出码 0/1 表示
  自身断言是否全过。
- **异常路径默认不 panic**：关闭后 `Send` 返回 `kClosed`、普通线程的阻塞会占住
  OS 线程、`Context` 不会自动取消。三者的完整默认行为与替代策略见上面的
  「异常处理示例」，改代码前先确认自己要的是默认语义还是显式策略。

## 深入阅读

- [并发模块 · 深入学习](../../deep/01-concurrency.md)
- [交接摘要](../../docs/01-concurrency.md)
- 源码位置：`include/go2cpp/concurrency/`、`include/go2cpp/sync.hpp`、`src/concurrency/`
- 示例目标名 = `go2cpp_<文件名>`；新增示例需登记 `cmake/Go2CppTests.cmake` 与本文清单。
