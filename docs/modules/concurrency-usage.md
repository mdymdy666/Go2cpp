# 并发使用指南：从 Thread 迁到 Fiber

> **状态：现行**（2026-10-31）。基线 `/UserData/CodexWorkSpace/Go2cpp`，
> 构建 `.build-bento-wsl`（Release，`GO2CPP_WITH_BENTO=ON`）。
> 本文是**迁移手册**，只回答「怎么写、怎么不踩坑」；内部设计与逐接口说明见
> [`fiber.md`](fiber.md)、[`scheduler.md`](scheduler.md)、[`sync.md`](sync.md)、
> [`channel.md`](channel.md)。与本文冲突时以模块文档为准，并回来修本文。

---

## 0. 30 秒模型

| 概念 | 本项目里的对象 | 一句话 |
|---|---|---|
| **G** | `Fiber` / `scheduler::Task` | 用户逻辑的执行体，可跨线程迁移 |
| **M** | OS 线程（`std::thread`） | 真正跑指令的载体，**不能被业务阻塞** |
| **P** | 逻辑处理器（`processor_count`） | 并行度与本地队列，默认等于硬件并发 |

一句话：**业务代码只应该看见 G。** 所有「等」都必须让 G 挂起（park）并把 M 让出去；
线程只保留给「无法协作化的边界」。这与 Go 的 goroutine 模型一致，区别是 Go 没有
「原生线程执行体」这一层，而 C++ 生态里到处都是——所以迁移的重点是**识别边界**。

---

## 1. 四条铁律

### 铁律 1：只用 Fiber-aware 原语

下列原语在「受管 G」上会 park、在普通线程上退化为 futex/条件变量，**同一份代码两种
上下文都能等**（`sync.md` §1）：

| 需求 | 用这个 | 不要用 |
|---|---|---|
| 互斥 | `go2cpp::sync::Mutex` / `RecursiveMutex` / `TimedMutex` | `std::mutex` / `std::recursive_mutex` |
| 读写锁 | `go2cpp::sync::RWMutex` | `std::shared_mutex` |
| 条件等待 | `go2cpp::sync::ConditionVariable` | `std::condition_variable` |
| 计数等待 | `go2cpp::sync::WaitGroup` | 手写 `atomic + cv` |
| 消息传递 | `go2cpp::Channel<T>` / `FastChannel` / `FastSpscChannel` | `std::queue + mutex + cv` |
| 一次性结果 | `go2cpp::Future<T>` / `Promise<T>` | `std::future` / `std::promise` |
| 超时与取消 | `go2cpp::Context`（`WithCancel/WithTimeout`） | 自造 `atomic<bool>` 标志 |
| 睡眠 | `go2cpp::time::Sleep(d)` | `std::this_thread::sleep_for` |
| 每执行体状态 | `go2cpp::FiberLocal<T>` | `thread_local` |
| 提交任务 | `go2cpp::go(...)` / `Scheduler::spawn` | `std::thread` / `std::async` |
| 网络 IO | `go2cpp::net::Socket`（Hook 接管） | 裸 syscall / `*_f` 系列 |

判据不是「这个类型能不能编译」，而是：**在受管 G 上阻塞时，M 会不会被占住**。
`std::mutex` 在 G 上竞争时会真阻塞 carrier 线程，整个 P 上的其他 G 一起停摆；
`sync::Mutex` 则把 G 挂起、唤醒方通过调度器把它放回队列（`sync.md` §3.1）。

### 铁律 2：绝不用 `std::thread` 跑业务

`std::thread` 创建的线程不参与 GMP：调度器不知道它在等什么，sysmon 也拿不到
`BlockingRegion` 的信号；它与 Fiber 之间每次唤醒都要走内核 futex，单次成本
**50–100 µs**（见 §3）。更糟的是，它会让「线程数 = 并发请求数」，在压测里直接把
RSS 和上下文切换打爆。

允许 `std::thread` 的只有三类**边界**：

1. 进程启动/收尾（main、信号处理前的引导）；
2. 固定大小的**边界线程池**，专门跑第三方阻塞库（见 §5）；
3. 必须绑定 OS 线程的第三方回调（例如某些 SDK 要求回调固定在创建线程）。

即便如此，边界线程也必须：有界、可 join、不持有 Fiber 侧锁等待、不反向依赖
调度器推进。`thread_policy::ScopedThreadParticipation` 只写 TLS 元数据，
**不会**把线程变成 M，也不能替代 `go()`。

### 铁律 3：网络全 Fiber

`read/write/recv/send/connect/accept`（socket 与 FIFO/pipe）以及
`sleep/usleep/nanosleep` 已被 Hook 接管：受管 G 上先试一次非阻塞系统调用，
`EAGAIN` 后注册 epoll 并 park，M 立即空出（`hook-blocking-policy.md`）。
因此**网络代码不需要包线程池**，直接写同步风格即可：

```cpp
#include "go2cpp/socket.hpp"

void EchoOnce(go2cpp::net::Socket& sock) {
  char buffer[4096];
  const auto got = sock.recv(buffer, sizeof(buffer));   // G park，M 留给别人
  if (got.ok() && got.bytes > 0) {
    (void)sock.send(buffer, static_cast<std::size_t>(got.bytes));  // 同样 park
  }
}
```

边界要记牢（`docs/compatibility.md` 的「有界 syscall 表面」）：

- **`*_f` 系列永远直调 libc、真阻塞**，只能出现在边界线程上；在 G 上调用等于自废武功。
- Hook **不接管**：`poll/select/epoll_wait`（事件循环入口）、`getaddrinfo`（DNS）、
  `fsync/wait4`、普通文件 IO、`eventfd/timerfd` 等非 socket 非 FIFO 的 fd。
  这些要么走 `Scheduler::BlockingRegion`（sysmon 会补 M），要么挪到边界线程池。
- `MSG_WAITALL`、`MSG_OOB` 在受管 G 上返回 `ENOTSUP`；`recvmmsg/sendmmsg`、
  `sendfile/splice` 属于显式边界，不要假设它们会 park。

### 铁律 4：线程只做边界

「边界」的四种典型形态：

| 形态 | 例子 | 处理方式 |
|---|---|---|
| 第三方同步库 | 旧 HTTP 客户端、数据库驱动、硬件 SDK | 固定池 + `Promise/Future`（§5） |
| 不可协作 syscall | `getaddrinfo`、`fsync`、`wait4`、普通文件读写 | 池 + Future，或 `BlockingRegion` 短临界区 |
| CPU 密集计算 | 压缩、加解密、编解码 | 池 + Future；或拆成小片在 G 上 `Yield` |
| 事件循环入口 | `poll/select/epoll_wait` | `BlockingRegion`，绝不在里面再跑调度器 |

线程只做边界的结果是：**线程数由部署决定（P + 池大小），而不是由请求数决定**。
这也是 Go 程序「看起来只有几根线程却扛几万连接」的原因。

---

## 2. Thread → Fiber 对照表

| 现在（Thread 世界） | 迁到（Fiber 世界） | 头文件 | 语义差异 / 注意 |
|---|---|---|---|
| `std::thread t(fn)` | `go2cpp::go(fn)` 或 `sched.spawn(fn)` | `go2cpp/go.hpp` | 返回 `Task` 句柄；`Task::wait()` 在 G 上 park |
| `pthread_create` | 同上 | 同上 | 不要直接建线程 |
| `std::mutex` | `sync::Mutex` | `go2cpp/sync.hpp` | 不记录 owner（A 锁 B 解合法）；未持有 `Unlock` 抛 `logic_error` |
| `std::recursive_mutex` | `sync::RecursiveMutex` | `go2cpp/concurrency/recursive_mutex.hpp` | owner + 深度；跨线程解锁是错误 |
| `std::timed_mutex` | `sync::TimedMutex` | `go2cpp/concurrency/timed_mutex.hpp` | 超时返回 `false`，不抛异常 |
| `std::shared_mutex` | `sync::RWMutex` | `go2cpp/concurrency/rwmutex.hpp` | 写者优先；读侧计数是原子操作 |
| `std::condition_variable` | `sync::ConditionVariable` | `go2cpp/sync.hpp` | `Wait(Mutex&)` 返回时**必重新持有**锁；`NotifyOne/NotifyAll` |
| `std::this_thread::sleep_for` | `go2cpp::time::Sleep(d)` | `go2cpp/tools/time.hpp` | 口语称 **fiber::Sleep**；Hook 开启时协作式，否则退化为真睡 |
| `thread_local T` | `FiberLocal<T>`（别名 `FiberLocalCache<T>`） | `go2cpp/fiber_local.hpp` | 随 G 迁移；Fiber 结束时统一析构；线程上有 TLS fallback |
| `std::queue + mutex + cv` | `Channel<T>` / `FastChannel<T,Cap>` | `go2cpp/channel.hpp` | 有界、FIFO、可 `Select`；`Close` 后 `Recv` 排空再 `kClosed` |
| 自建线程池 | `Scheduler`（P/M）+ `go()` | `go2cpp/scheduler.hpp` | 任务由运行时调度；不要自己管理 worker 数 |
| `std::future/promise` | `Future<T>` / `Promise<T>` | `go2cpp/future.hpp` | 只完成一次；Promise 析构发 broken-promise；等待走 park |
| `std::atomic<T>` | **保持不变** | `<atomic>` | 原子量没有执行体语义，不需要迁移 |
| 信号量 / 令牌桶 | 有缓冲 `Channel<T>`（cap=N） | `go2cpp/channel.hpp` | 本库没有独立 `Semaphore`，用通道表达 |

`Channel` 的容量就是背压：cap=0 是 rendezvous，cap=N 是 N 个在途许可。
**不要**用无界 `std::queue` 模拟通道——内存会先于逻辑爆炸。

---

## 3. 跨执行体性能回退：50–100 µs 是物理，不是 bug

### 3.1 一条数据线

| 拓扑 | 唤醒路径 | 本机实测（负载 2.5–3.0） |
|---|---|---|
| Fiber → Fiber | 调度器 park/wake，同进程不陷内核 | **49.7 ns/op**（`f2f`，cap=64） |
| Thread → Thread | futex（无竞争先自旋 8 次 PAUSE） | 195–237 ns/op（`t2t`） |
| Fiber ↔ Thread | futex `WAIT/WAKE` + 两次上下文切换 + 调度延迟 | **50–100 µs/op**（`f2t` 75–100、`t2f` 20–44，随负载放大） |
| 原生线程等 WaitGroup | futex 唤醒 + 线程重新被调度 | 65–83 µs/op |

参考：`JumpContext` 裸往返只有 6.2–6.7 ns，Fiber 切换 p50 约 76 ns；
差距全部来自**跨执行体的内核路径**，不是原语实现。

### 3.2 什么时候触发

只要「等待方」和「唤醒方」不在同一执行体，就会命中：

- 一个原生线程等 Fiber 生产的消息（`t2f`）；
- 一个 Fiber 等原生线程池的结果（`f2t`）；
- `WaitGroup` 的 `Done` 来自线程、`Wait` 在 G 上；
- 混合锁：native 线程持有 `sync::Mutex`，G 在等这把锁；
- 任何「逐条 ping-pong」的跨执行体握手。

反过来，**同执行体不触发**：f2f 用调度器 park/wake，t2t 用 futex 但通常自旋成功。

### 3.3 为什么是物理

跨执行体唤醒必须让内核参与：`FUTEX_WAIT` 把线程挂起、`FUTEX_WAKE` 把它重新排队，
中间是两次上下文切换 + 调度器选核 + 缓存/分支预测失效。单次 5–50 µs 是内核的
正常量级；本机在共享负载下测到 50–100 µs，属于「物理 + 环境」，**不是并发原语的
bug**，也无法在保持「两个真实执行体」语义的前提下消除。

### 3.4 怎么避免（按收益排序）

1. **拓扑对齐**：生产者、消费者放同一执行体。f2f 比 f2t 快约 **3 个数量级**
   （50 ns vs 50–100 µs）；同一份逻辑能用 Fiber 表达就不要落线程。
2. **批量搬运**：一次跨执行体搬 N 条，把 50–100 µs 摊薄。`Channel` cap≥64、
   批量提交、`send_batch`/`writev` 都是这个思路。
3. **一次性结果用 Future**：一个请求一个 `Future`，不要让每个元素都跨一次执行体。
4. **分片去共享**：16 个参与者共享一把锁实测 294 µs/op；按 8 分片后回到 ~40 ns/op。
   争用会把 ns 级路径放大 4 个数量级，比跨执行体更致命。
5. **固定边界池**：池大小按外部库并发上限设，不在请求路径上创建/销毁线程。
6. **按预算设计容量**：必须跨执行体时，按 **50–100 µs/次** 估时延；
   1e6 次逐条 f2t = 50–100 s，批量化 1024 条后只剩约 1000 次跨执行体。

---

## 4. 三个必须改的习惯

### 4.1 `std::mutex` → `sync::Mutex`

```cpp
// 迁移前：G 竞争时阻塞 carrier 线程，P 上的其他 G 一起停摆
std::mutex mu;
{
  std::lock_guard<std::mutex> lock(mu);
  queue.push(item);
}

// 迁移后：受管 G park、普通线程 futex，同一 FIFO 队列互相唤醒
#include "go2cpp/sync.hpp"
go2cpp::sync::Mutex mu;
if (mu.Lock(ctx)) {              // ctx 可取消；失败返回 false
  queue.push(item);
  mu.Unlock();
}
```

注意：

- `sync::Mutex` **不记录 owner**，A 加锁 B 解锁是合法的（Go 风格）；未持有时
  `Unlock` 抛 `std::logic_error`。
- 临界区里**允许 park**，但不要持锁做长 IO/`Sleep`：G 挂着锁睡觉会把等待者一起拖住。
  打开 `GO2CPP_DETECT_HELD_PARK=1` 可让「持锁 park」计数并被压测断言抓到。
- 需要超时用 `LockFor(timeout)`；需要 RAII 用 `std::lock_guard<sync::Mutex>`（提供
  `lock/try_lock/unlock`），但 guard 的析构不能在另一个线程执行。

### 4.2 `sleep_for` → `time::Sleep`（fiber::Sleep）

```cpp
// 迁移前：真睡当前 M，P 上其他 G 全部停摆（sysmon 只能事后补 M）
std::this_thread::sleep_for(10ms);

// 迁移后：Hook 拦截 nanosleep → eventfd + IOManager::WaitFor，G park、M 让出
#include "go2cpp/time.hpp"
go2cpp::time::Sleep(10ms);       // 语义同 Go time.Sleep；d<=0 立即返回
```

前提与边界：

- 必须 **Hook 开启**（进程默认开）且当前执行流绑定在 `IOManager` 上；否则它会退化为
  真实的 `nanosleep`，这是 `time.hpp` 文件头已声明的差异。
- 在**边界线程**上调用 `time::Sleep` 就是真睡——这正是边界线程该有的语义，
  不要指望它协作。
- 持锁时不要 `Sleep`（同上，`GO2CPP_DETECT_HELD_PARK` 会抓）。
- 需要「可取消的等待」用 `Channel::RecvFor` / `Context`，不要用 `Sleep` 轮询。

### 4.3 `thread_local` → `FiberLocal`

```cpp
// 迁移前：值跟着 M 走；G 迁移到另一个 M 后丢失或串味
thread_local RequestContext tls_ctx;

// 迁移后：值按逻辑 Fiber（FiberRecord）索引，跨 M 迁移仍在
#include "go2cpp/fiber_local.hpp"
static go2cpp::FiberLocal<RequestContext> g_ctx;   // 进程级槽对象

RequestContext& ctx = g_ctx.GetOrCreate();         // 首次构造并保存
if (auto* existing = g_ctx.TryGet()) { /* ... */ }
g_ctx.Reset();                                     // 显式清理
```

注意（`fiber.md` §8）：

- 值在 Fiber trampoline 结束时由运行时统一析构；**析构必须 noexcept 且不能挂起 Fiber**。
- `TryGet()` 返回裸 `T*`，在 `Reset()` 或 Fiber 结束后失效，不要保存到别的执行体。
- 普通线程上自动退化为线程局部备份，线程退出时销毁。
- 槽对象（`FiberLocal<T>` 实例）要活过使用期：静态/成员持有，不要临时构造。
- 真正「每 OS 线程一份」的元数据（例如池 worker 的统计）才继续用 `thread_local`；
  与 G 绑定的业务状态一律 `FiberLocal`。

---

## 5. 外部库衔接：线程池 + Future（不同步等）

第三方同步库无法改造时，标准做法是**边界线程池 + `Promise/Future`**，
让阻塞发生在边界线程、等待发生在 Fiber：

```cpp
#include "go2cpp/future.hpp"
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

// 固定大小的边界池：只跑第三方阻塞调用，不跑业务逻辑。
class LegacyPool {
 public:
  explicit LegacyPool(std::size_t workers) {
    for (std::size_t i = 0; i < workers; ++i) {
      workers_.emplace_back([this] { Loop(); });
    }
  }
  ~LegacyPool() {
    {
      std::lock_guard<std::mutex> lock(mu_);
      stopping_ = true;
    }
    cv_.notify_all();
    for (auto& worker : workers_) worker.join();
  }

  // Fiber 侧调用：立即返回 Future；阻塞只发生在边界线程。
  go2cpp::Future<Reply> Query(Query query) {
    auto promise = std::make_shared<go2cpp::Promise<Reply>>();
    auto future = promise->GetFuture();
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (stopping_ || tasks_.size() >= kMaxPending) {
        promise->SetError(go2cpp::NewError("legacy pool full"));
        return future;
      }
      tasks_.push_back([promise, query = std::move(query)] {
        try {
          promise->SetValue(LegacyBlockingQuery(query));   // 这里才是真阻塞
        } catch (...) {
          promise->SetException(std::current_exception());
        }
      });
    }
    cv_.notify_one();
    return future;
  }

 private:
  static constexpr std::size_t kMaxPending = 1024;
  void Loop();
  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<std::function<void()>> tasks_;
  bool stopping_{false};
  std::vector<std::thread> workers_;
};

// 调用方（受管 G）：GetResult 内部 park，M 立刻去跑别的 G。
void Handle(go2cpp::ContextPtr ctx, LegacyPool& pool) {
  auto result = pool.Query(MakeQuery()).GetResult(ctx);
  if (result.Ok()) {
    Use(*result.Value());
  } else if (result.Cancelled()) {
    // ctx 被取消：任务结果被丢弃，消费方不会永久等待
  }
}
```

要点：

1. **不同步等**：Fiber 侧只调 `GetResult(ctx)`（park），绝不 `std::this_thread::join`
   或忙等；池线程侧只做阻塞调用 + 完成 Promise，绝不调 `Future::Get`。
2. **有界**：池队列必须有上限（上面的 `kMaxPending`），满了立刻返回错误 Future；
   无界队列 = 迟到的 OOM。
3. **取消**：`GetResult(ctx)` 在 ctx 取消时返回 `kCancelled`；外部库不支持取消时，
   至少让结果被丢弃，不要让任务无限重试。
4. **broken-promise**：`Promise` 在未完成时析构会自动发布错误，消费方**不会永久等待**；
   但仍要保证每个 Promise 都有执行者，否则就是逻辑错误。
5. **收尾顺序**：先停调度器（不再提交新任务）→ 停池并 join → 再析构 Future 状态。
6. **池线程不是 Fiber 上下文**：不要在池线程里用 `time::Sleep` 期待协作，也不要
   在池线程里依赖 `FiberLocal` 的迁移语义（它只会落到 TLS fallback）。

何时不用 Future：一对一 fire-and-forget 用 `Channel` 或直接 `go()` 更轻；
只有「一个结果 + 错误 + 可能取消」才值得 `Future`。

---

## 6. 迁移检查清单

复制到 PR 描述里逐条勾：

- [ ] `grep -rn 'std::thread\|pthread_create\|std::async'` —— 只允许启动、边界池、第三方回调
- [ ] `grep -rn 'std::mutex\|std::recursive_mutex\|std::shared_mutex\|std::condition_variable'`
      —— 全部换成 `sync::*`，或在边界线程内说明理由
- [ ] `grep -rn 'sleep_for\|usleep\|nanosleep\|::sleep('` —— G 上一律 `time::Sleep`
- [ ] `grep -rn 'thread_local'` —— 与 G 绑定的状态改 `FiberLocal`
- [ ] `grep -rn 'getaddrinfo\|fsync\|wait4\|open('` —— 阻塞 syscall 是否在边界线程/`BlockingRegion`
- [ ] 每个 socket 调用确认走 `net::Socket`，没有 `*_f` 混入业务路径
- [ ] 每个锁临界区里没有 IO / `Sleep` / 阻塞 channel 操作
      （`GO2CPP_DETECT_HELD_PARK=1` 跑一轮压测，计数应为 0）
- [ ] 线程池有界、可 join、不会反向等待调度器
- [ ] 跨执行体路径已用 §3 的 50–100 µs 预算估算，且尽量批量化
- [ ] FiberLocal 跨 M 迁移有测试（参考 `tests/concurrency/test_fiber_local.cpp`）
- [ ] 取消路径：`Context` 取消后 `Future/Channel` 等待方被唤醒且不泄漏
- [ ] 收尾顺序：调度器 → 池 → Future 状态，无静态析构顺序问题
- [ ] `ctest --test-dir .build-bento-wsl --output-on-failure` 全绿（当前 85 项，
      `bento_netem` 无权限时 SKIP）

---

## 7. 反模式速查

| 写法 | 后果 | 改法 |
|---|---|---|
| `std::mutex` 保护热路径队列 | G 阻塞占住 M，P 停摆 | `sync::Mutex` 或 `Channel` |
| `sleep_for(1ms)` 轮询标志 | 睡死 M，延迟抖动 | `time::Sleep` + `Channel`/`ConditionVariable` |
| `thread_local` 存请求上下文 | G 迁移后丢失/串味 | `FiberLocal<T>` |
| 每请求 `std::thread` | 线程爆炸 + 50–100 µs/次 | 固定池 + `Future` |
| 在 G 上 `getaddrinfo` | Hook 不接管，真阻塞 M | 边界池 + `Future` |
| 持锁做 `recv`/`Sleep` | 等待者一起挂，易死锁 | 锁内只做内存操作 |
| 池线程里 `future.get()` | 池饥饿/互等死锁 | 回调里 `go()` 转回 Fiber |
| 无界 `std::queue` 当通道 | 内存无上限 | 有界 `Channel`（cap=N） |
| 用容量 1 的 channel 代替 Future | 丢 broken-promise 语义 | `Promise/Future` |
| `Select` 里用 `std::mutex` 保护多通道 | 锁序不确定，可死锁 | `Select` 自带 lockorder（地址排序） |

---

## 8. 参考

- 执行体与栈：[`fiber.md`](fiber.md)、[`../fiber_guide.md`](../fiber_guide.md)
- 调度器与 `BlockingRegion`：[`scheduler.md`](scheduler.md)
- 锁与等待底座：[`sync.md`](sync.md)、[`../hook-blocking-policy.md`](../hook-blocking-policy.md)
- 通道与 `Select`：[`channel.md`](channel.md)
- 性能数据与口径：[`../reports/2026-10-30-performance.md`](../reports/2026-10-30-performance.md)、
  [`../reports/2026-10-31-verify-final.md`](../reports/2026-10-31-verify-final.md)
- 遗留项：[`../reports/2026-10-31-base-backlog.md`](../reports/2026-10-31-base-backlog.md)
