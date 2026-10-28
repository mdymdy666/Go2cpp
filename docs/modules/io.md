# 模块文档：io（IOManager / epoll）

> **状态：现行**。基线 `HEAD 1b46de8`（2026-10-26 核对，`src/network/` 与 `include/go2cpp/network/` 相对 HEAD 无改动）。
> 本文是 `io`（fd readiness / epoll）模块的**唯一现行文档**：接口、设计、边界、测试都在这里。
> 旧文档 `docs/gmp-vs-thread-epoll-2026-10-18.md` 的 IOManager 结论已并入本文并在其顶部加了合并标注；
> `docs/network.md` 的“线程、Fiber 和等待”一节只保留 Socket 层用法，IOManager 细节以本文为准。
> 引用约定：`file:line` 均指当前工作树；代码注释中的历史实测数字标注了出处行号与前提。

---

## 1. 概述与职责

`io::IOManager` 是**面向 Fiber 的 Linux fd 就绪管理器**：把 fd 的读/写兴趣注册进 epoll，
等待时把当前 G park 掉、把 worker 让出来，就绪后经调度器唤醒 G。它**只等就绪，不搬运数据**：
真正的 `read/write` 在 Socket/Hook 层，`IOManager` 不拥有调用方的 fd
（`include/go2cpp/network/io.hpp:165-190`）。

| 做 | 不做 |
|---|---|
| 单 fd / 多 fd 的 readiness、超时、取消、关闭通知 | 不做 `read/write`；不缓存数据、不解析协议 |
| fd 代际校验，防 `close`+复用后旧等待误唤醒 | 不拥有 fd 生命周期（`FdManager`/`Hook` 拥有） |
| epoll 事件 → claim → `Scheduler::wake_io` | 不在 `Cancel()` 里强杀已运行的 C++ 栈 |
| 后端抽象（`IOBackend`）以便换 kqueue/io_uring | 不跨 `IOManager` 共享后端实例 |

依赖四个外部件（`io.hpp:165-190` 的类注释）：`Scheduler`（park/wake）、`Context`
（取消与 deadline）、`DescriptorRegistryState`（fd 代际）、Linux epoll（readiness）。

---

## 2. 文件布局与路径澄清

| 路径 | 内容 | 规模 |
|---|---|---|
| `include/go2cpp/network/io.hpp` | `DescriptorToken/Guard`、`IOEvent/WaitStatus`、`Wait*` 结果、`IOManager`、调试快照 | 360 行 |
| `src/network/io.cpp` | `IOManager::State`（条带/队列/deadline/后端）、poller、事件处理、`wait*` | 2949 行 |
| `include/go2cpp/network/io_backend.hpp` | `IOBackend` 抽象、`BackendEvent`、后端注册表 | 87 行 |
| `src/network/io_backend.cpp` | Linux `EpollBackend`（epoll + eventfd）、命名后端工厂 | 225 行 |
| `include/go2cpp/network/fd_manager.hpp` | `FdContext`/`FdManager`：用户/系统非阻塞与收发超时元数据 | 69 行 |
| `include/go2cpp/network/hook.hpp` / `src/network/hook.cpp` | libc 拦截：`EAGAIN` 时转 `IOManager::wait_for` | 355 / 2832 行 |
| `include/go2cpp/io.hpp`、`io_backend.hpp`、`fd_manager.hpp`、`hook.hpp` | 兼容旧根路径的转发头（4~23 行） | — |

> **路径澄清**：任务书写的 `include/go2cpp/io/**`、`src/io/**` 目录**不存在**；现行布局把
> IO 放在 `network/` 下，根路径 `go2cpp/io.hpp` 只是转发头。旧文档
> `timer_design_review.md` 提到的 `src/io.cpp` 同样是已迁移的旧路径。

---

## 3. API 表

### 3.1 生命周期与调度（`io.hpp:210-341`）

| API | 语义 | 实现 |
|---|---|---|
| `IOManager(SchedulerConfig, IOBackendPtr)` | 构造时**立即**把 `poll_once/tickle` 装进内部 Scheduler，并把 `this` 登记进全局表 | `io.cpp:2155-2190` |
| `start()` | 启动调度器与 poller 线程；可重复调用（幂等） | `io.cpp:2209-2227` |
| `shutdown()` | 停止接收、唤醒并回收线程；可重复 | `io.cpp:2228-2237` |
| `is_running()` | 生命周期标志 | `io.hpp:238` |
| `go(fn)` | 提交一个由内部 Scheduler 跑的 Fiber | `io.cpp:2239-2241` |
| `scheduler()` / `current()` | 内部调度器；当前线程绑定的 manager（经 Scheduler 上的原子指针，无全局锁） | `io.cpp:2940-2948` |
| `DebugSnapshot()` | 逐条带加锁的只读快照（成本高于普通计数） | `io.cpp:2902-2937` |

### 3.2 等待（`io.hpp:253-305`）

| API | 返回 | 要点 |
|---|---|---|
| `wait(fd, event, deadline, ctx, token)` | `WaitResult{kReady,kTimeout,kCancelled,kClosed,kError}` | 只允许当前 manager 的 managed Fiber；普通线程返回 `kError/EPERM`（`io.cpp:2251-2254`） |
| `wait_for(fd, event, timeout, ctx)` | 同上 | `timeout<=0` ⇒ deadline=now（立即探测）；溢出饱和到 `max()`（`io.cpp:2391-2400`） |
| `wait_any(requests, deadline, ctx)` | `WaitAnyResult` | 实际调用 `wait_many` 并取 `ready_indices.front()`（`io.cpp:2815-2830`） |
| `wait_many(requests, deadline, ctx)` | `WaitManyResult.ready_indices` | **不是**“全部完成”的屏障；按请求顺序返回全部已就绪队头 |
| `cancel(fd, event)` / `cancel_all(fd)` | bool | 唤醒等待者 `kCancelled/ECANCELED`，**不关闭 fd**（`io.cpp:1168-1176`） |
| `notify_close(fd)` | bool | Hook 在真实 `close` 前调用：失效注册、唤醒 `kClosed/EBADF`、删槽（`io.cpp:2866-2887`） |
| `NotifyCloseAll(fd)` | void | 跨 manager 路由；回调只持 `State` 弱引用（`io.cpp:2872-2900`） |

### 3.3 代际与后端

| 类型 | 说明 |
|---|---|
| `DescriptorToken`（`io.hpp:30-47`） | 不拥有 fd；`fd()`/`generation()`/`valid()`；析构不影响 fd |
| `DescriptorGuard`（`io.hpp:69-87`） | 按 fd 分条带的**递归锁** + `Capture(fd)` / `Invalidate(fd)`；绝不能跨阻塞系统调用或 park 持有 |
| `IOBackend`（`io_backend.hpp:26-84`） | `initialize/add_or_modify/remove/wait/wake/drain_wake/shutdown` + 能力查询 `supports_zero_interest/supports_one_shot` |
| `RegisterBackend/UnregisterBackend/CreateBackend/DefaultBackendName` | 进程级命名工厂；**默认后端不可注销**（`io_backend.cpp:186-215`） |

---

## 4. 设计

### 4.1 触发模式：**EPOLLET（边沿触发），不是 ONESHOT**

这是本次核对中最容易搞错的一点，源码里有**互相矛盾的注释**，以可执行行为为准：

- `EpollBackend::add_or_modify` 注册的事件掩码是 `EPOLLERR|EPOLLHUP|EPOLLET`，读再加
  `EPOLLIN|EPOLLRDHUP`，写再加 `EPOLLOUT`（`src/network/io_backend.cpp:85-100`）——
  **没有 `EPOLLONESHOT`**；
- `supports_one_shot()` **返回 false**（`io_backend.cpp:72`），尽管其注释写着“支持 EPOLLONESHOT”；
- 因此 `IOManager` 的 `one_shot` 分支（`io.cpp:1324-1346`、`1484-1493`）在 Linux 默认后端上
  **不生效**，走的是“ET + 显式重新 arm”路径；
- `io.cpp:273-291`（“epoll 实例（`EPOLLONESHOT`）”“没有独立 poller 线程”“`armed` 一次性后端”）
  与 `io.cpp:1828-1845`（“内核的 EPOLLONESHOT 保证”）都是**过期注释**，不要据此设计；
  4.2/4.3 的成员与线程描述以代码和本文为准。

ET 下的三个必要动作：

1. **同一 fd 还有等待者时必须 MOD 重 arm**：一次边沿只唤醒队头一个节点，若队列非空，
   置 `force_rearm=true`（`io.cpp:1531-1535`），下一次 `update_interest_locked` 强制
   `EPOLL_CTL_MOD` 让内核重新产生边沿；
2. **无等待者时的边沿不能丢**：`process_event_locked` 在没有等待者时把方向记入
   `pending_read/pending_write`（`io.cpp:1501-1526`），下一次 `register_wait` 直接消费，
   稳态下省掉 DEL/ADD；
3. **interest 必须精确投影**：读|写收缩为写时必须 MOD，否则 ET 下会持续收到无消费者的
   读事件（`io.cpp:1357-1363` 的注释记录了旧实现的忙循环）。

### 4.2 数据结构

`IOManager::State`（`src/network/io.cpp:264-292`）：

| 成员 | 角色 | 位置 |
|---|---|---|
| `m_stripes[64]` | fd → `FdSlot` 的分片表，`fd & 63` 定位，`alignas(64)` 独占 cacheline | `io.cpp:2043-2064` |
| `m_deadline_order` | `multimap<time_point, node_id>`，IO 等待超时 | `io.cpp:2106` |
| `m_deadline_index` | `unordered_map<node_id, TimerRecord{order, weak_ptr<WaitNode>}>`，O(1) 摘除 | `io.cpp:645-664` |
| `m_backend` | `IOBackend`（默认 epoll，含 eventfd 唤醒） | `io.cpp:2101` |
| `m_registrations`（token 编码） | epoll token = `(fd+1)<<32 | seq`，迟到事件靠序号拒绝 | `io.cpp:302-315` |
| `WaitNode` | 一次等待：`id/fd/generation/event/task/descriptor/outcome(原子 CAS)/wake_mutex` | `io.cpp:317-520` |
| `FdSlot` | 每 fd：`readers/writers` FIFO、`registered/armed/interest/registration_id/force_rearm/pending_*`、`generation` | `io.cpp:578-639` |

> `io.cpp:264-292` 的成员表本身已**过期**：它写的 `m_deadline`/`m_registrations`/`m_eventfd`
> 与现行成员名不对应（现行是 `m_deadline_order`/`m_deadline_index`，eventfd 在后端内部）。
> 上表按现行代码整理，引用时以上表为准。

锁顺序是硬约束：**条带锁 → `m_timer_mutex`**（`io.cpp:2104-2112`）。`expire_timers` 因此分两阶段：
先在定时器锁内摘节点，释放后再逐条带更新（`io.cpp:1621-1683`），绝不反向嵌套。

### 4.3 事件循环

**两条驱动路径共享同一个后端/同一个 epoll fd**：

1. **独立 poller 线程**：`State::start` 在 `!m_worker_polling` 时创建 `m_poller`
   （`io.cpp:721-731`），跑 `poll_loop()`（`io.cpp:1725-1790`）；
2. **worker 空闲自 poll**：构造时 `set_worker_polling(true)` 并注入
   `Scheduler::set_io_idle_poller(poll_once, tickle)`（`io.cpp:2178-2190`），worker 无 G 可跑时
   自己 `epoll_wait`，就绪事件**在本线程**处理，G 直接投回本 P（`io.cpp:1828-1845`、
   `src/concurrency/scheduler.cpp:4387` 的调度器侧注释）。

`poll_once`（`io.cpp:1878-1935`）的步骤：

1. 后端不可用/已停止 ⇒ 返回 -1，让调用方退回条件变量等待，避免忙循环；
2. 有效超时 = `min(timeout_ms, poll_timeout_ms())`；有 deadline 已到期直接取 0；
3. `m_backend->wait(events, 256, effective)`，`EINTR` 只重试 `wait`；
4. 含 `kWakeRegistration`（eventfd，token=0）则排空并清 `m_tickle_pending`；
5. 有事件先 `process_events_batch`（批量 256，逐 fd 取条带锁），**无论有无事件**都
   `expire_timers()` —— 超时不能依赖 IO 事件到达。

优先级：同一轮里 **readiness 先于 timeout**（`io.cpp:1782-1783`），与 `poll/select` 一致；
`wait_many` 收集结果时也显式保持该规则（`io.cpp:2774-2790`）。

### 4.4 注册 → park → 唤醒（与 Fiber park 的集成）

`wait()` 的完整链路（`src/network/io.cpp:2243-2390`）：

1. 前置：必须是本 manager 的当前 G，否则 `EPERM`；`TimePoint::max()` 归一化为“无超时”，
   避免每条空闲连接都挂一个定时器节点把 `epoll_wait` 变成忙轮询（`io.cpp:2256-2261`）；
2. Context 已完成 ⇒ 直接返回 `kTimeout/ETIMEDOUT` 或 `kCancelled/ECANCELED`；
3. deadline 已过 ⇒ `probe_immediate` 用**原生 `poll(0)`** 做一次零超时探测，
   不进入 Hook、也不再注册（`io.cpp:84-130`）；
4. `register_wait`：条带锁内二次校验 fd 代际 → 复用/新建 `WaitNode` → 入 FIFO →
   需要超时才在**同一临界区**登记 deadline（锁序：条带 → timer）→ `update_interest_locked`
   发布 epoll 兴趣；只有**新增了更早的 deadline** 才 `tickle()`（`io.cpp:786-1036`）；
5. Context 回调：`Done().AddCallback` 里 `claim(kTimeout|kCancelled)`，
   与 epoll/超时路径竞争同一个 `outcome`；
6. `park_io(node->task)` 循环：唤醒后先查 `outcome`，未完成则处理伪唤醒；
   若任务被取消、scheduler 变化或 scheduler 已停止，则 `claim(kCancelled/ECANCELED)` 兜底
   （`io.cpp:2343-2370`）；
7. 结果解码；`RemoveCallback`；`recycle_node` 进 TLS 缓存（声明在 `io.cpp:544-547`，
   回收在 `io.cpp:1079`）。

**单赢家**：`claim()` 用 CAS 把 `outcome` 从 0 改成 `(status|errno)`，epoll 事件、
`expire_timers`、`notify_close`、Context 取消四方只有第一个成功（`io.cpp:1211-1227`）。
唤醒侧：单个节点走 `WaitNode::wake()` → `scheduler->wake_io(task)`（`io.cpp:381-390`）；
批量则收集任务后 `wake_io_batch(tasks)`（`io.cpp:1940-1990`），减少锁竞争。

### 4.5 与 Hook 的集成

- `hook::bind_io_manager(manager)` 把 manager 绑到当前线程（`hook.hpp:52-56`、
  `hook.cpp:1523-1530`，RAII 版 `ScopedIOManagerBinding` 在 `hook.hpp:227-237`）；
- Hook 的 `read/write/recv/send/connect/accept` 统一走 `cooperative_io`：
  先做一次**真实 syscall**，`EAGAIN` 才注册 epoll 并 park，唤醒后**重试**（数据可能被别的读者取走）
  （`src/network/hook.cpp:1054-1120`，步骤注释同段）；
- `close` 路径先 `IOManager::NotifyCloseAll(fd)` 再真正关闭（`hook.cpp:807-811`），
  保证“close 后数字 fd 复用”不会把旧事件交给新连接；
- `DescriptorGuard` 在 syscall 期间持有 fd 条带锁，**绝不跨 park 持有**（`io.hpp:60-68`）。

---

## 5. 边界与陷阱

### 5.1 fd 数量与容量

- epoll 本身没有 fd 数量上限，实际上限是进程 `RLIMIT_NOFILE`；`64` 只是**锁分片数**，
  不是 fd 容量（`io.cpp:2046`）。单个条带内的 `unordered_map` 无硬上限。
- epoll token 把 fd 编码进高 32 位（`uint32(fd)+1`，`io.cpp:303-315`），隐含 fd 必须非负且
  不超过 `INT_MAX`；负 fd 在入口即 `EBADF`。
- 单次 `epoll_wait` 最多处理 256 个事件（`io_backend.cpp:112-135` 的 `kMaxEvents`、
  `io.cpp:1885` 的 `kEventBatch`）；事件更多时靠下一轮继续。

### 5.2 ET 的四个坑

1. **多等待者**：只唤醒队头，剩余必须 MOD 重 arm；漏掉就会“后面的 Fiber 永远等”（`io.cpp:1531-1535`）；
2. **无等待者**：边沿只能记 pending，不能丢；`pending_*` 与 `interest` 必须同步（`io.cpp:1501-1526`）；
3. **interest 收缩**：必须精确 MOD，否则残留方向会持续报告（`io.cpp:1357-1363`）；
4. **关闭后的 HUP/ERR**：`interest==0` 的保留注册仍可能收到 `EPOLLERR/EPOLLHUP`，
   必须直接失效该代际并删槽，否则 poller 空转（`io.cpp:1494-1508`）。

### 5.3 调用约束

- `wait*` **只能**在 managed Fiber 中调用；普通线程返回 `kError/EPERM`，请用系统 `poll/select`
  （`io.cpp:2251-2254`、`2419-2424`）。这是为了避免“偷偷阻塞调用方线程”。
- `wait_many` 同一集合内**禁止重复 fd/方向**（返回 `EINVAL`），否则一次 readiness 可能消费多个
  请求、破坏 FIFO 语义（`io.cpp:2433-2444`）；空集合也是 `EINVAL`。
- `wait_many` 不是屏障：它返回**本轮已经就绪**的全部队头索引；需要“全部完成”请组合
  多个等待或上层 `WaitGroup`（`io.hpp:150-160`）。
- `wait_any` 只是 `wait_many` 的第一个就绪项，**不是**随机/最快，也不保证跨轮公平
  （`io.cpp:2815-2830`）。
- `Cancel()` 不会关闭 fd，也不会打断已进入用户态执行的代码；`notify_close` 才是关闭协议。
- `shutdown()` 把全部等待者以 `kCancelled/ECANCELED` 唤醒（`io.cpp:2228-2237`、`736-780`），
  之后新注册返回 `ESHUTDOWN`（映射为 `kClosed`，`io.cpp:2274-2281`）。
- 后端实例与 manager **一对一**：重复绑定同一后端返回 `EBUSY`（`io.cpp:670-690`）。

### 5.4 性能相关的实现事实（代码注释中的实测，非本次复测）

| 结论 | 出处 |
|---|---|
| 保留空 `FdSlot`（不每次删/建）：s3 每请求 `fcntl` 6.13→命中缓存、`epoll_ctl` 10.75→MOD；QPS 6,270→9,823（+56%），P50 445ms→3.4ms | `io.cpp:594-605` |
| 64 条带 + `alignas(64)` 前，整批 256 事件同一把锁（锁内还 `epoll_ctl`），实测并行度约 1.7 核 | `io.cpp:2049-2060` |
| 无 deadline 的注册不再 `tickle`：0.93 次/请求 eventfd 写放大成 10.9 次 `epoll_wait` 返回与 read | `io.cpp:1025-1036` |
| `epoll_ctl` 用 `try_lock` 自适应：低并发串行（每请求 CPU 404µs→900µs 的内核自旋），高并发直连（序列化会掉 24%） | `io.cpp:2092-2100` |

---

## 6. 测试

### 6.1 ctest 入口

| 用例 | 内容 | 注册位置 |
|---|---|---|
| `go2cpp_io_audit` | 7 个 HUP/shutdown/cancel/close 审计场景，`TIMEOUT 300` | `cmake/Go2CppTests.cmake:817-825` |
| `go2cpp_tests` + `GO2CPP_TEST_FILTER=io` | `run_io_tests()`：13 个场景（见下） | `cmake/Go2CppTests.cmake:216`、`tests/test_main.cpp:96` |
| `go2cpp_hook_*`（hook 套件） | Hook 侧 park/close 行为，跨模块 | `cmake/Go2CppTests.cmake:455-495` |

### 6.2 场景与规模（源码常量）

`tests/network/test_io.cpp`（973 行，13 个 `test_*`，入口 `run_io_tests` 在 `:958-973`）：

| 场景 | 规模/断言 |
|---|---|
| `test_readiness_timeout_race` | 10000 轮、每批 32（Valgrind 下降到 1000），就绪与超时竞争 |
| `test_wait_many_immediate_ready_race` | 1000 次迭代，立即就绪与注册竞争 |
| `test_multiple_waiters_same_fd_level_trigger` | 同一 fd 多等待者 FIFO 唤醒 |
| `test_dual_interest_shrink` | 读写兴趣收缩后的 ET 行为 |
| `test_generation_reuse_and_cross_manager_close` | close/复用代际、跨 manager 关闭 |
| `test_wait_any_and_wait_many` / `test_wait_any_cancellation_and_close` | 多 fd 语义、取消与关闭 |
| 其余 6 个 | 单 P、shutdown 唤醒无限等待、嵌套 Fiber、伪唤醒、立即探测、后端工厂 |

`tests/network/test_io_audit.cpp`（477 行）：

| 场景 | 规模 |
|---|---|
| `TestShutdownWakesAllWaiters` | 16 个等待者全部返回 |
| `TestCancelAllDrainsEveryWaiter` | 3 个等待者 |
| `TestNotifyCloseDrainsEveryWaiter` | 2 个等待者，`kClosed/EBADF` |
| `TestCloseWhileHeadCancelRace` | 8 对 fd × 200 轮 |
| 其余 3 个 | HUP 唤醒、代际复用、`wait_any` 关闭 |

### 6.3 本次复核运行（2026-10-26）

| 命令 | 结果 | 耗时 |
|---|---|---|
| `GO2CPP_TEST_FILTER=io ./go2cpp_tests` | rc=0（完整断言集） | 1,341 ms |
| `./go2cpp_io_audit` | rc=0 | 216 ms |

> **验证口径与限制**：测试二进制来自 `build-work`（2026-10-21 22:17 的 Release 构建）。
> 该构建对应提交 `6c0a9d1`；`git diff 6c0a9d1 HEAD -- src/network include/go2cpp/network
> src/concurrency/scheduler.cpp tests/network tests/concurrency` 为**空**，即二进制对应的
> io 源码与当前工作树一致。当前 HEAD 整体无法重编（`src/os/dir.cpp` 引用未定义的
> `detail::ContainsNul`，与本模块无关），故无法从 HEAD 现场产出新二进制；上表数字是该提交上
> 的真实运行结果，**不是**引用旧文档。

### 6.4 旧文档中的 IO 性能数据（引用，前提已变）

`docs/gmp-vs-thread-epoll-2026-10-18.md` 记录：单事件跨线程唤醒路径 185–225µs/往返、
批量就绪时 IOManager+Fiber 153k events/s（对比 thread-pool epoll 45k events/s）。
该文的“IOManager 只有 1 个 poller 线程串行处理事件 → 5–13k/s 上限”是**旧实现**
（无 worker 自 poll、无条带）的结论，现行实现已由 4.3 的两条驱动路径与 64 条带取代；
数字只能作历史对照，不能用于当前容量规划。

---

## 7. 已知缺口（如实标注）

- `docs/state-machines/FdSlot.md` 被 `docs/annotation-progress.md:26` 标为“待建”，
  当前没有 `FdSlot` 状态机图；本文 4.2 的字段表是唯一集中的描述。
- 后端只实现了 epoll；`IOBackend` 的 kqueue/IOCP/io_uring 仍是接口预留，未验证。
- 事件环 `kIoWait/kIoRead/kIoWrite/kIoWake` 仅在 `GO2CPP_DEBUG_RESOURCES` 下求值，
  默认构建看不到单次等待的资源事件。
- 本文不覆盖 Socket/Stream/HTTP 的读写语义，见 [network.md](../network.md)；
  与 Timer 的分工见 [timer.md](timer.md)。
