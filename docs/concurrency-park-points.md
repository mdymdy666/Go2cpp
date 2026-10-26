> **已合并至 [docs/modules/sync.md](modules/sync.md)**：park 点清单、临界区禁用与 `SpinMutex` 契约见该文 §4.3；channel 的 park 语义见 [docs/modules/channel.md](modules/channel.md)。本文保留为历史快照。

# 并发 park 点清单（临界区禁用清单）

> 适用版本：2026-10-18。结论来自 `include/go2cpp/network/hook.hpp`、
> `src/network/hook.cpp`、`src/concurrency/scheduler.cpp` 与
> `tests/network/test_hook*.cpp` 的实际实现，不是凭印象罗列。
>
> 一句话：**`SpinMutex` 临界区内只允许纯计算；任何可能 park 或阻塞 M 的调用
> 都必须移到临界区外，或改用 `sync::Mutex`。**

---

## 1. 为什么需要这份清单

Go2Cpp 是**协作式**调度，park 点是显式的，而且很多看起来"同步"的函数
实际上会把当前 G 挂起：

```cpp
G1: Lock(A) -> read(fd) -> park     // 持着 A 走了
G2: Lock(B) -> read(fd) -> park     // 持着 B 走了
G3: Lock(A) -> 等 G1 释放 A
    A 在 G1 手里，G1 在等可能永远不来的 fd -> 永久死锁
```

Go 是抢占式调度，没有这个问题；Go2Cpp 把 `read` / `write` / `connect` /
`accept` / `sleep` 全 hook 了，所以**必须**在代码层面区分两类临界区：

| 锁 | 临界区内允许 | 用在哪 |
|---|---|---|
| `sync::Mutex` | park、IO、等待 | 可能阻塞的临界区 |
| `sync::SpinMutex` | **只有纯计算** | 极短的数据结构更新 |

---

## 2. 会 park（临界区内禁止）

这些调用在受管 G 中会走 `Scheduler::park_*`，让出 M，直到 fd 就绪或超时。

### 2.1 fd 就绪类（epoll 唤醒）

| 函数 | 阻塞原因 | 就绪事件 |
|---|---|---|
| `read` / `readv` / `recv` / `recvfrom` / `recvmsg` | 等数据 | fd 可读 |
| `write` / `writev` / `send` / `sendto` / `sendmsg` | 等缓冲空间 | fd 可写 |
| `connect` | 等连接建立 | fd 可写 |
| `accept` / `accept4` | 等新连接 | fd 可读 |
| FIFO/pipe 上的 `read` / `write` | 等数据/缓冲 | fd 可读/可写 |

**注意**：如果调用方把 fd 显式设成 `O_NONBLOCK`，Hook 会保持 `EAGAIN`
语义、**不 park**（见 `tests/network/test_hook.cpp` 的 nonblocking 用例）。
但这不是"安全"——你在临界区里拿到 `EAGAIN` 之后通常还得重试/等待。

### 2.2 时间类（定时器唤醒）

| 函数 | 实现 |
|---|---|
| `sleep` / `usleep` / `nanosleep` / `clock_nanosleep` | `cooperative_sleep_for`（eventfd + `WaitFor` 带超时）→ park |

### 2.3 同步原语（`park_wait` 路径）

这些不是 libc 函数，但同样走 `Task::park_for_scheduler()`：

| 函数 | 何时 park |
|---|---|
| `sync::Mutex::Lock` / `lock` / `LockFor` | 锁被占用时 |
| `sync::ConditionVariable::Wait` / `WaitFor` | 等通知/超时 |
| `sync::WaitGroup::Wait` / `WaitFor` | 计数未归零 |
| `channel` 的发送/接收 | 对端未就绪/缓冲满 |
| `future` / `promise` 的 `get` / `wait` | 结果未就绪 |
| `Scheduler::park_current` / `park_wait` / `park_io` | 显式 park |
| `Fiber::SuspendForScheduler` | 底层挂起 |

---

## 3. 不 park，但会**阻塞整个 M**（临界区内同样禁止）

### 3.1 `*_f` 原始版本

| 函数 | 行为 |
|---|---|
| `read_f` / `readv_f` / `recv_f` / `recvfrom_f` | 直调 libc，阻塞 M |
| `write_f` / `writev_f` / `send_f` / `sendto_f` | 直调 libc，阻塞 M |
| `connect_f` / `accept_f` | 直调 libc，阻塞 M |
| `sleep_f` / `usleep_f` / `nanosleep_f` / `clock_nanosleep_f` | 直调 libc，阻塞 M |
| `poll_f` / `select_f` / `epoll_wait_f` | 直调 libc，阻塞 M |

**为什么 `*_f` 在临界区里也不好**：它不会 park，所以不会触发 `SpinMutex`
的契约检查——但它把**整个 worker 线程（M）**钉住，该 M 上所有其他 G 全部
停摆。持有锁的 G 仍然持有锁，只是没人能运行；一旦别的 G 等这把锁，就是
"持锁阻塞"的另一种形态，deadline 可能无限长。

### 3.2 事件循环入口

| 函数 | 原因 |
|---|---|
| `poll` / `ppoll` / `select` / `pselect` / `epoll_wait` / `epoll_pwait` | 它们**本身就是事件循环入口**，不是"在某个 fd 上等待"；Hook 刻意不 park，只走 `invoke_native_blocking` 标记 `BlockingRegion` + sysmon |

### 3.3 不阻塞、但会阻塞 M 的操作

| 函数 | 原因 |
|---|---|
| `close` / `shutdown` / `fcntl` / `dup` / `dup2` | 不阻塞在 fd 上；Hook 不让关闭元数据管理 |
| `fsync` / `fdatasync` / `wait4` / `getaddrinfo` | 当前**未 hook**（需要 io_uring/signalfd/异步 resolver 桥接），原生阻塞 M |

---

## 4. 纯计算（可以放进 `SpinMutex`）

- 算术、位运算、字符串/容器操作（不触发分配器系统调用）；
- 对已有内存的读写、`memcpy`、`std::vector` 的**已预留容量**内的 push；
- 使用 `std::atomic` 的短操作；
- 已经就绪的 fd 上的 `*_f`（例如普通文件）——**但前提是你能证明它不会
  阻塞**，不能证明就不要放。

> 经验法则：临界区里不要出现"等"这个词。等待 = park 或阻塞 M。

---

## 5. 正确做法

1. **可能 IO / 可能等待的临界区用 `sync::Mutex`**，并在持锁期间接受 park；
2. **纯计算的小临界区用 `sync::SpinMutex`**，让"持锁 park"在开发期直接
   abort；
3. 如果临界区里必须 IO，把数据拷贝出来、**先解锁**、再做 IO：
   ```cpp
   std::string payload;
   {
       sync::SpinMutex::Guard guard(mutex);
       payload = std::move(pending_);   // 纯计算
   }
   (void)::write(fd, payload.data(), payload.size());  // 解锁后 IO
   ```
4. 需要超时保护时优先 `Mutex::LockFor(duration)`，避免无限期等锁；
5. 不要用 `*_f` 规避检查：它只是把协程死锁换成线程阻塞。

---

## 6. 运行时检查（怎么发现违反）

两套标记，互斥判定、内层优先：

| 条件 | 含义 | debug（`GO2CPP_DEBUG_LOCKS` 或未定义 `NDEBUG`） | release（`NDEBUG`） |
|---|---|---|---|
| `no_park_depth > 0` | `SpinMutex` 临界区 | 打印 G id、深度、调用栈 → `abort()` | 降级为 `Mutex` 语义 + `scheduler::spin_mutex_park_degrade_total()` +1 + 一次性警告 |
| `held_mutex_count > 0` 且 `no_park_depth == 0` | 普通 `Mutex` 临界区 | 计数 + 一次性日志 | `scheduler::mutex_held_park_total()` +1 |

- 检测点在 `Task::park_for_scheduler()` 入口（`src/concurrency/scheduler.cpp`），
  所有 park 路径（`park_wait` / `park_io` / `park_with_reason` /
  `park_wait_for_cleanup`）都汇聚到这里；
- 普通 `Mutex` 的 `held_mutex_count` 由 `sync.cpp` 在 `Lock`/`Unlock` 维护，
  由 `sync::SetHeldLockParkDetection()` 控制（默认 Debug 开、Release 关，
  环境变量 `GO2CPP_DETECT_HELD_PARK=1/0` 可覆盖）；
- `SpinMutex` 的深度由 `Task::no_park_depth()` 暴露，锁对象上也可通过
  `SpinMutex::OwnerNoParkDepth()` 观测。

---

## 7. 代码审查清单

- [ ] `SpinMutex::Lock()` 与 `Unlock()` 之间是否出现本文件第 2/3 节的任何函数？
- [ ] 是否在持锁期间调用虚函数/回调（可能间接 IO）？
- [ ] 是否在持锁期间等待另一个 `Mutex` / channel / future？
- [ ] 锁内是否有日志输出（`fprintf` / 日志模块可能加锁/刷盘）？
- [ ] 临界区是否足够短（经验值：几十条指令，不含循环等待）？
- [ ] 是否用 `*_f` "绕过"检查（应改为解锁后调用）？

---

## 8. 相关文件

- `include/go2cpp/concurrency/spin_mutex.hpp`：`SpinMutex` 与语义说明；
- `src/concurrency/scheduler.cpp`：`Task::park_for_scheduler()` 检测桩；
- `include/go2cpp/network/hook.hpp`：Hook 开关与 `*_f` API；
- `docs/hook-blocking-policy.md`：Hook 判定标准（可 park / 需桥接 / 不该 hook）；
- `tests/concurrency/test_spin_mutex.cpp`：debug abort / release 降级的真实场景；
- `benchmarks/spin_mutex_benchmark.cpp`：`SpinMutex` vs `Mutex` vs `std::mutex`。
