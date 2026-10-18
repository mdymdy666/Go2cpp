# channel / future / io 并发设施审计报告（2026-10-18）

范围：`include/go2cpp/concurrency/channel.hpp`、`src/concurrency/channel.cpp`、
`include/go2cpp/concurrency/future.hpp`、`src/concurrency/future.cpp`、
`src/network/io.cpp`、`include/go2cpp/network/io.hpp`。

边界：本审计**未修改** `scheduler.cpp` / `fiber.cpp` / `parking_condition.cpp` /
`sync.cpp`（另一条流负责）。仓库：WSL ext4 主树 `/UserData/CodexWorkSpace/Go2Cpp`。

---

## 0. 方法与环境

- 构建：`build-quality`，`CMAKE_BUILD_TYPE=Release`（`-O3 -DNDEBUG`）。
- 硬件：WSL2，12 vCPU，19 GiB 内存。
- 压测：`timeout 120`，`taskset` 绑核，`procs_running` 记录在每次运行前后；
  **loadavg 只作背景**（WSL2 失真，审计期间 0.99～36 波动，15 分钟值一度
  4000+，为历史 fork storm 残留）。
- 每个性能指标：1 万次操作、5 次采样取中位数、整套基准重复 3 轮取中位数。
- 新增测试/基准不注册进 ctest（保持全量 ctest 计数 113），入口：
  `scripts/run_concurrency_audit.sh [tests|bench|all]`。

新增文件：

| 文件 | 作用 |
|---|---|
| `tests/concurrency/test_channel_concurrency.cpp` | 1 万值精确计数、随机 close、close 唤醒、FIFO、Fiber 路径 |
| `tests/concurrency/test_future_audit.cpp` | 单次终态、多次 Get、broken promise、超时、异常、多等待者 |
| `tests/network/test_io_audit.cpp` | HUP、shutdown、cancel_all/notify_close 排空、代际、close 竞态压力 |
| `benchmarks/audit_concurrency_benchmark.cpp` | channel/future/io 性能对比 |
| `scripts/run_concurrency_audit.sh` | 上述文件的编译与运行入口 |

---

## 1. channel 审计结论

### 1.1 有缓冲 / 无缓冲语义

- `capacity == 0` 走 rendezvous：发送优先与 `m_receivers` 队首会合，其次
  尝试缓冲区（容量 0 必然失败），最后入 `m_senders` 阻塞。
- `capacity > 0` 使用 `detail::RingBuffer`（原始存储 + 就地构造，等价 Go
  hchan 的 `buf/dataqsiz`），`Len()` 只统计缓冲区，不含交接中的等待者。
- 关闭后 `Recv` 先排空缓冲区再返回零值；`Send` 在入口检查 `m_closed`，
  返回 `kClosed` + `ChannelClosedError()`，**不会静默丢弃**。

### 1.2 close 语义

| 行为 | 实际 | 结论 |
|---|---|---|
| 关闭后 recv | `(zero, false)`，`status=kClosed` | ✅ |
| 关闭后 send | `status=kClosed` + 非空 `error` | ✅ 返回错误（非 panic） |
| 重复 close | `status=kAlreadyClosed` + `ChannelAlreadyClosedError()` | ✅ 显式错误（非 panic） |
| 关闭时排空缓冲 | 已缓冲值仍可读出 | ✅ |
| 关闭时阻塞的 send | 唤醒并返回 `kClosed`，值丢弃 | ✅ |
| 关闭时阻塞的 recv | 唤醒并返回 `kClosed` | ✅ |
| 关闭时阻塞的 select | 每个 select 被 `TrySelect(kClosed)` 选中 | ✅ |

`Close()` 在持有 `m_mutex` 时交换 `m_senders/m_receivers` 并逐个发布终态，
锁外 `notify_one`；等待者的谓词读取的是 pending 节点字段（不是 channel
成员），因此不会与 Close 的临界区竞争。**没有发现丢唤醒。**

### 1.3 select 多路等待

- `detail::SelectWaitState` 用 `TrySelect` / `TrySelectPair` 保证单赢家；
  step4/step6 的两次 `Take` 把“已选”与“超时/取消”线性化，Cancel 后仍会
  取回已发布的交接结果。
- 有 `arm_checked` 时注册失败会回滚（`Cancel` + `disarm`），不会留下指向
  已返回 Select 的悬空节点。
- 已关闭通道上的 `RecvCase` 立即可选并返回 `kClosed`。

### 1.4 FIFO 公平性

- `m_senders` / `m_receivers` 是 `std::deque`，普通 send/recv 与
  `ArmSelectRecv/Send` 都只服务队首；被取消的队首会弹掉后继续。
- 专项测试（8 个等待者、5ms 间隔、5 轮）接收/发送顺序 0 误差。
- `RefillBufferLocked` 遇到无法转移的 select sender 会 `break`（队头阻塞），
  但该分支被 `static_assert(nothrow_move)` 前置条件排除，实际不可达。

### 1.5 精确计数（1 万值）

| 拓扑 | 值数 | missing | duplicate | phantom |
|---|---|---|---|---|
| 4P × 4C，cap=7 | 10000 | 0 | 0 | — |
| 2P × 2C，cap=0 | 10000 | 0 | 0 | — |
| 3P × 3C，cap=4，随机 close | 6000（2439 成功发送） | 0 | 0 | 0 |
| 2P × 2C，std::string | 2000 | — | — | 内容 0 损坏 |

随机 close 场景同时验证：`Send` 返回 `Ok` 的每个值恰好被接收一次；
返回 `Closed` 的值一次也不出现；所有 send/recv 都无 `TimedOut`。

### 1.6 close 唤醒时延

16 个阻塞 recv、8 个阻塞 send、16 个阻塞 select：`Close()` 后分别在
1ms / 1ms / 1ms 内全部返回（门限 1000ms）。

---

## 2. future 审计结论

| 检查项 | 结论 |
|---|---|
| `SetValue`/`SetError`/`SetException`/`Cancel` 只成功一次 | ✅ 4 线程 ×1000 轮竞争，`winners` 恰为 1000，无双重终态 |
| 第二次 Set 必须报错 | ✅ 返回 `false`，已发布结果不变 |
| `Get` 阻塞直到就绪 | ✅ |
| `Get` 可多次 | ✅ 16 线程 ×100 次，值不被消费 |
| Promise 未完成即析构 | ✅ 发布 `BrokenPromiseError()`，阻塞者收到 `kError`；不泄漏、不永久阻塞 |
| `GetFor(duration)` 真超时 | ✅ 40ms 请求实测 40ms 返回 `kTimedOut`；超时不改变 Future 状态，稍后可再次成功读取 |
| `SetError` 后 Get | ✅ `status=kError` 且 `Is(error)` 成立；`GetOrThrow` 抛 `FutureError` |
| `SetException` 后 Get | ✅ `GetResult().exception` 非空；`GetOrThrow` 原样 `rethrow` |
| 多等待者 | ✅ 16 个 Fiber/线程同时等待，SetValue 后全部就绪 |
| Fiber 取消 | ✅ 8 个 G 的 Context 取消后全部返回 `kCancelled`，Promise 仍可完成 |
| Promise 移动 | ✅ 旧对象析构不发布 broken promise |
| `Status()` 快照 | ⚠️ pending 映射为 `kTimedOut`（枚举无 pending 态，见 C-2） |

**未发现 A 级缺陷。** 默认后端的 `wait*` 不会虚假唤醒；插件后端契约与
`StatusLocked` 的“兜底返回 kTimedOut”存在语义张力（C-3，未改）。

---

## 3. io 审计结论

### 3.1 fd 注册/注销竞态

- 所有 `FdSlot`、等待队列、deadline 索引都由 `State::m_mutex` 保护；
  epoll `ADD/MOD/DEL` 在同一临界区内完成，`DescriptorGuard` 另有一把进程级
  递归锁保护代际令牌的捕获/失效。
- `register_wait` 在发布节点前、持有 `m_mutex` 时二次校验 token 的
  `valid()/fd()`，关闭路径先失效 token 再排空，构成双线性化点。
- `notify_close` / `finish_fd` / `shutdown` / 错误路径都走同一排空函数。

### 3.2 epoll 事件与 Fiber 唤醒

- 每个 `registration_id` 编码 fd + 注册序号，迟到事件在
  `process_event_locked` 中被拒绝；ONESHOT 后端消费后显式 rearm。
- 每次事件对同一 fd/方向只 `take_ready_locked` 一个队首，水平触发保证
  其余等待者随后被唤醒；批量唤醒用 `wake_io_batch` 按 P 分桶。
- `EPOLLERR|EPOLLHUP` 同时置 read/write ready 并 claim 队首 → **会唤醒**
  （专项测试：对端直接 close，`Wait` 返回 `kReady`）。
- `interest==0` 仍收到 HUP/ERR 时显式失效注册，避免 poller 忙循环。

### 3.3 FD 复用

- `DescriptorToken`（fd + generation）+ `FdSlot::generation` +
  `registration_id` 三层隔离；`notify_close` 删除 slot，旧事件 token 找不到
  slot 即被忽略。
- 专项测试：`NotifyClose` 后同一数字 fd 仍打开，新等待先超时、写入后
  ready，说明代际已重建且无旧状态残留。
- 绕过 Hook 直接 `close(2)` 不会通知 IOManager；这是 Hook 层契约（C-6）。

### 3.4 停止唤醒

- `State::shutdown` 在 `m_mutex` 内失效所有注册、排空所有队列
  （`kCancelled/ECANCELED`），锁外批量唤醒；16 个无限等待 Fiber 全部返回。

### 3.5 **A 级缺陷：终止性排空被队头阻塞**

`drain_queue_locked` 原实现从队头逐个 `pop_front`，遇到
`begin_operation()` 失败的队头就 `break`。该队头正被另一条完成路径
（`Context` 取消回调的 `complete()`：先在锁外取操作令牌 + claim，再取
`m_mutex`）处理。此时 `notify_close()` 会无条件 `m_slots.erase(fd)`，队列中
其余等待者失去 epoll 注册、也不在 deadline 索引（无 deadline 时），
**永久 park**。

复现器 `tests/network/test_io_audit.cpp::TestCloseWhileHeadCancelRace`：
8 对 fd × 每对 2 个等待者（A 带可取消 Context，B 只有 100ms 兜底 deadline），
并发取消 A 与 `NotifyClose`，200 轮。

| 版本 | 丢失唤醒（B 收到 kTimeout 而非 kClosed） |
|---|---|
| 修复前 | **138 / 1600**（99/200 轮命中） |
| 修复后 | **0 / 1600**（3 次重复运行均为 0） |

---

## 4. 问题清单（按严重程度）

### A — 正确性

- **A-1（已修）** io `drain_queue_locked` 在队头忙时提前 break +
  `notify_close` 无条件删 slot → 队列其余等待者永久 park。见 3.5。
- **A-2（非本流，已修复）** `go2cpp_state_matrix` 确定性失败：
  `Runnable(!queued) × park_for_scheduler`。`src/concurrency/scheduler.cpp`
  （14:11，另一条流）新增 `runnable_in_place` 分支，使 Runnable 且
  `!queued` 也返回 `ParkAction::kParked`；而
  `tests/concurrency/test_state_machine_matrix.cpp:373-376` 仍期望只有
  Running → `kParked`。属于另一条流的边界；**14:13 对方已修复（矩阵重跑 failures=0），重建后全量 ctest 113/113**。

- **A-3（非本流，构建 all 失败）** 另一条流新增的
  `benchmarks/lock_overhead_matrix_benchmark.cpp:144` 使用
  `std::mutex` 但没有 `#include <mutex>`，导致
  `cmake --build build-quality`（all）在 96% 处失败。该目标未注册
  ctest，不影响 113/113；本审计未修改该文件。

### B — 性能

- **B-1（已优化）** channel 每次 send/recv 都调用 `m_change_cv.notify_all()`，
  即使没有任何 `WaitForChange` 观察者。已改为观察者计数 + 空队列快速返回。
- **B-2（已优化）** future 的 `Wait` 在终态/零时长场景仍分配
  `FutureWaitRegistration` 并走完整等待状态机。已加两条快速路径。
- **B-3（记录，未改）** 无缓冲 channel 单次会合约 3.6～4.2µs：每次阻塞
  需要 `PendingSend/Recv` + `ParkingCondition::WaitNode` 两次堆分配，
  再加 fiber park/wake。TLS 节点缓存可降低，但需要动
  `parking_condition.cpp`（另一条流边界），本轮未做。
- **B-4（记录，未改）** `IOManager::WaitRead` 约 23.8µs vs 裸 epoll_wait
  7.3µs（3.3x）。主要成本：每次等待一次 `epoll_ctl(MOD)` rearm（ONESHOT）、
  `State::m_mutex` 全局锁、Scheduler park/wake 与 eventfd tickle。属于设计
  成本；进一步优化需要 per-fd 锁或批量 rearm，不在本轮范围。
- **B-5（记录）** 新 drain 对超长队列使用 `deque::erase(middle)`，n 个等待者
  最坏 O(n²)。当前每 fd 队列通常 1～3，未优化；若未来出现大量同 fd 等待者，
  应改为重建队列（O(n)）。

### C — 文档 / 语义

- **C-1** 重复 `close` 返回 `kAlreadyClosed`、关闭后 `send` 返回 `kClosed`，
  都是显式错误码而非 Go 的 panic。`Close()` 标注 `noexcept`，头文件已声明；
  send 不是静默丢弃，满足“不能静默丢弃”的底线。若要 panic 语义需要改 API
  并同步更新既有测试（`test_channel.cpp:88` 明确断言 `kAlreadyClosed`），
  本轮按 C++ 错误码风格保留。
- **C-2** `Future::Status()` 对 pending 返回 `kTimedOut`（`FutureStatus`
  没有 pending 枚举），易被误读为“已超时”。建议后续新增 `kPending` 或
  提供 `IsReady()`。
- **C-3** `FutureWaitBackend` 的契约要求容忍虚假唤醒，但
  `StatusLocked` 在没有取消/deadline 时把“仍未就绪”兜底成 `kTimedOut`。
  默认 `ParkingCondition` 后端不会虚假唤醒，自定义后端可能误报超时。
- **C-4** `Channel` 析构会 `Close()` 唤醒等待者，但等待者醒来仍需
  `m_mutex`；调用方必须保证 Channel 的生命周期长于所有等待者（头文件已
  声明，Go 由 GC 保证）。
- **C-5** 绕过 Hook 的 `close(2)` 不会通知 IOManager；fd 复用防护依赖
  Hook 在真实 close 前调用 `NotifyClose`（文档已声明）。
- **C-6** `BrokenPromiseError()` 是进程级单例，两个不同 broken promise 的
  错误 `Is()` 比较为 true（头文件已声明为有意设计）。

---

## 5. 修复内容

### 5.1 `src/network/io.cpp` — `drain_queue_locked`

- 由“队头阻塞即 break”改为**遍历整个队列**：忙节点保留原位（它自己的完成
  路径会 claim/唤醒），其余节点逐个 `begin_operation + claim`、移出并放入
  WakeList；成功 claim 的节点仍由 WakeList 统一 `end_operation()`。
- 影响 `notify_close` / `finish_fd(cancel/cancel_all)` / `fail_slot_locked` /
  `shutdown` 四条路径，终止性状态从此保证覆盖所有等待者。
- `take_ready_locked` / `complete_if_head` 保持原有“不越过队头”的 FIFO
  语义不变。

### 5.2 `include/go2cpp/concurrency/channel.hpp`

- 新增 `m_change_waiters` 原子计数与 `NotifyChange()`：仅在确有
  `WaitForChange` 观察者时调用 `m_change_cv.notify_all()`。
- `WaitForChange` 持锁先 `fetch_add`，等待返回（仍持锁）后 `fetch_sub`；
  与通知方“持锁递增 generation → 解锁 → 读计数”由同一把 `m_mutex` 排序，
  不会漏掉已注册观察者（等待者随后检查 generation 时必然看到变化）。
- 24 处锁外 `m_change_cv.notify_all()` 改为 `NotifyChange()`。

### 5.3 `include/go2cpp/concurrency/future.hpp`

- `FutureState<T>::Wait`（非 void 与 void 两份）增加：
  1. 持锁检查终态，已终态直接 `StatusLocked` 返回（省 `make_shared` +
     `weak_from_this`）；
  2. 无 Context 且 deadline 已过时直接返回 `kTimedOut`（TryGet/GetFor(0)
     不再分配等待票据）。
- 语义不变：终态优先于取消/超时的顺序仍由 `StatusLocked` 保证。

---

## 6. 性能数据

条件：Release（`-O3 -DNDEBUG`）、`taskset -c 0`、1 万次/样本、每项 5 样本
取中位数、整套重复 3 轮（三轮的 `procs_running` 分别为 5 / 1 / 1）；
运行窗口内 loadavg 3.9～4.3（WSL2 值不可信，仅作背景）。基线（“优化前”）
在同一宿主、`procs_running=2`、loadavg 0.99 时采集。

| 指标 | 优化前 ns/op | 优化后中位数 ns/op | 变化 |
|---|---:|---:|---:|
| channel 有缓冲 Send+Recv 往返 | 129.2 | **58.2** | -55% |
| channel 有缓冲 TrySend+TryRecv 快路径 | 125.9 | **57.8** | -54% |
| channel 无缓冲 1P1C 会合 | 4612.6 | **4177.9** | -9%（未改此路径，宿主噪声） |
| future 已就绪 Get | 38.6 | **29.2** | -24% |
| future SetValue→Get 完整生命周期 | 163.9 | **138.9** | -15% |
| future pending TryGet（0ms 探测） | 118.4 | **81.0** | -32% |
| 裸 epoll_wait 唤醒（1P） | 8058.0 | **7418.5** | 未改（宿主噪声） |
| IOManager WaitRead 唤醒（1P） | 25362.9 | **23316.7** | 未改（宿主噪声） |

- 优化后 3 轮（每轮 5 样本中位数）：channel 往返 58.8 / 58.2 / 56.4；
  unbuffered 4343.1 / 4177.9 / 3840.0；future hot 29.2 / 29.7 / 28.4；
  lifecycle 135.3 / 139.1 / 138.9；pending TryGet 80.8 / 83.1 / 81.0；
  WaitRead 24017.6 / 23316.7 / 23298.7。同一指标跨轮离散度 < 10%
  （unbuffered 约 12%，受调度影响）。
- 另一次更安静的窗口（`procs_running=2`）实测：channel 往返 50.1～53.2、
  future hot 20.3～21.3、pending TryGet 73.8～76.9。报告取较保守的三轮
  中位数，不取最好成绩。
- **io vs epoll：约 3.1x**（23.3µs vs 7.4µs）。差距来源见 B-4；这不是
  本次修改引入的退化（修改前后稳态逻辑未动）。
- 说明：channel/future 的收益来自去除空等待队列的互斥量与热路径分配；
  io 两侧都没有优化，差异属于宿主噪声，仅作同次采样的基线对照。

## 7. 全量 ctest

- 命令：`flock /tmp/go2cpp_build.lock ctest --test-dir build-quality -j4`。
- 结果：**113/113 全绿**（总耗时 18.31s），包括 `go2cpp_tests`
  （channel/future/io/timer/hook 等全部模块）、`go2cpp_state_matrix`、
  `go2cpp_io_hook_full`。
- 说明：审计过程中曾出现一次 `go2cpp_state_matrix` 确定性失败
  （`Runnable(!queued) × park_for_scheduler`），根因为另一条流 14:11 的
  `scheduler.cpp` 改动；该流 14:13 修复后，重建矩阵测试 failures=0，
  全量 ctest 恢复 113/113（见 A-2）。
- 本审计的三个新测试/基准**未注册 ctest**，因此不改变 113 的计数。
- `cmake --build build-quality`（all 目标）目前会因另一条流新增的
  `benchmarks/lock_overhead_matrix_benchmark.cpp` 缺少 `#include <mutex>`
  而失败；该文件不属于本审计范围，且未注册 ctest，不影响测试结果
  （见 A-3）。

## 8. 仍未解决

1. **A-3**：`benchmarks/lock_overhead_matrix_benchmark.cpp`（另一条流新增）
   缺少 `#include <mutex>`，导致 `cmake --build build-quality`（all）失败。
   未注册 ctest，不影响 113/113；需要对方补 include。
2. **B-3**：无缓冲 channel 的两次堆分配（TLS 节点缓存）未做，触及
   `parking_condition.cpp`。
3. **B-4**：IOManager 约 3.1x epoll 成本（ONESHOT rearm + 全局 State 锁 +
   park/wake）未优化。
4. **B-5**：超长等待队列下 drain 的 O(n²) 未优化（当前每 fd 1～3 个等待者，
   无实际影响）。
5. **C-1/C-2/C-3**：Go panic 语义、`FutureStatus` 缺 pending 态、
   自定义等待后端的虚假唤醒契约，均属 API/文档层面，未改。
