# 模块文档：timer（Timer / TimerService）

> **状态：现行**。基线 `HEAD 1b46de8`（2026-10-26 核对，工作树源码相对 HEAD 无改动）。
> 本文是 `timer` 模块的**唯一现行文档**：接口、设计、边界、测试都在这里。
> 旧文档 `docs/timer_design_review.md`、`docs/timer_usage.md`、`docs/timer_benchmark.md`
> 已并入本文并在各自顶部加了合并标注；原文保留作追溯，**其中的路径、行号与性能数字以本文为准**。
> 引用约定：`file:line` 均指当前工作树；凡引用旧文档或代码注释中的历史实测数字，均注明出处与前提。

---

## 1. 概述与职责

Timer 是运行时里**唯一自带后台线程**的并发设施，且**刻意不依赖 Scheduler**
（`include/go2cpp/concurrency/core/timer.hpp:1-25`，`src/concurrency/timer.cpp:1-30`）。
原因有两条：调度器自己需要超时能力（`wait_for`），反向依赖会成环；Timer 又必须在
Scheduler 尚未启动的初始化阶段可用。

| 做 | 不做 |
|---|---|
| 绝对/相对截止时间登记，按时间有序排队 | 不继承、不拥有、不伪造成 GMP 的 M |
| 取消、重排（Reset / Refresh）、周期与最大次数 | 不在计时线程恢复别的线程的 Fiber 栈 |
| 回调异常隔离、时长分级告警、只读快照 | 不保证"回调开始后还能强杀"——C++ 调用栈不可异步切断 |
| 通过 `TimerExecutor` 把回调交给外部执行环境 | 不实现 fd readiness/epoll（那是 `io::IOManager`） |

`TimerService::Default()` 是**进程级**懒初始化单例（`src/concurrency/timer.cpp:1099-1102`），
只在进程结束时回收；需要独立生命周期、独立异常观察器或隔离阻塞回调时，应显式构造服务。

---

## 2. 文件布局

| 路径 | 内容 | 规模 |
|---|---|---|
| `include/go2cpp/concurrency/core/timer.hpp` | 全部公开类型：状态/选项/决策/执行器/来源/快照/`Timer`/`TimerService` | 505 行 |
| `src/concurrency/timer.cpp` | `Timer::State`、`TimerService::Impl`、后台线程 `Run()`、调试打点 | 1343 行 |
| `include/go2cpp/core/timer.hpp` | 兼容转发头（只 `#include` 真身，4 行） | 4 行 |
| `tests/concurrency/test_timer.cpp` | Fiber/Channel 交互、取消、`WaitForChange` | 363 行 |
| `tests/concurrency/test_timer_stress.cpp` | 六个压力场景（独立 ctest 目标 `go2cpp_timer_stress`） | 315 行 |
| `tests/debug/test_timer_debug.cpp` | dump/snapshot/来源统计/事件环/并发 dump | 579 行 |
| `tests/replay/test_timer_replay.cpp` | 录制-回放：同 deadline/周期/取消/随机 1000/select/10k 压测 | 858 行 |

> **路径澄清（重要）**：任务书中写的 `include/go2cpp/concurrency/timer.hpp` **不存在**；
> 真身是 `include/go2cpp/concurrency/core/timer.hpp`，而 `include/go2cpp/core/timer.hpp`
> 是转发头。旧文档 `timer_design_review.md` 写的 `include/go2cpp/core/timer.hpp` 与
> `src/timer.cpp` 也都是旧路径，现行实现位于 `concurrency/` 子目录。

---

## 3. API 表

### 3.1 句柄 `core::Timer`（`timer.hpp:317-355`）

| API | 语义 | 关键约束 |
|---|---|---|
| `~Timer()` | **等价于 `Cancel()`**（`timer.cpp:1006-1008`） | 临时对象立即取消 ⇒ 回调永不触发；调用方必须持有句柄 |
| `Cancel()` | 取消尚未领取的回调，重复调用安全（`timer.cpp:1020-1037`） | 已进入 `kRunning` 的回调不会被强行打断 |
| `Active()` | 是否仍可能产生下一次回调 | 读 `State::m_active`，加状态锁 |
| `StateValue()` | `kInvalid/kPending/kRunning/kCompleted/kCancelled/kFailed` | 生命周期状态机，见 `timer.hpp:50-58` |
| `RunCount()` | 已领取的回调次数 | 从 1 开始递增，在 `Execute` 内加锁更新 |
| `Reset(deadline)` | 把**仍 Pending** 的任务改到绝对时间 | 不是删除再插入：原地重排、保留 `run_count`（`timer.cpp:496-537`） |
| `Refresh()` | 按原 `period` 从当前时刻重新开始 | 仅周期任务有意义；无正 period 返回 false（`timer.cpp:1075-1090`） |

句柄不可复制、可移动；移动赋值先 `Cancel()` 旧状态（`timer.cpp:1010-1018`）。

### 3.2 服务 `core::TimerService`（`timer.hpp:362-501`）

| API | 说明 | 实现位置 |
|---|---|---|
| `Default()` | 进程级单例 | `timer.cpp:1099` |
| `Schedule(deadline, cb)` | 绝对时间一次性任务；空回调返回无效句柄；过去时间尽快执行 | `timer.cpp:1104` |
| `ScheduleAfter(delay, cb)` | 相对延时；负值按 0 处理（`nonnegative`，`timer.cpp:59-65`） | `timer.cpp:1124` |
| `ScheduleEvery(period, cb, opts)` | 固定延迟/固定速率周期；`period<=0` 或空回调返回无效句柄 | `timer.cpp:1130` |
| `ScheduleRepeat(initial_delay, cb, opts)` | 回调返回 `TimerDecision` 动态决定下一次 | `timer.cpp:1148` |
| `ScheduleOn(deadline, cb, executor)` | 到期后提交给 `TimerExecutor`；**来源自动标 `kScheduler`** | `timer.cpp:1177` |
| `ScheduleEveryOn(period, cb, executor, opts)` | 每次到期都提交给执行器 | `timer.cpp:1200` |
| `ScheduleWeakAfter(delay, weak_ptr<void>, cb)` | owner 已消失则到期后自动完成、不回调 | `timer.cpp:1226` |
| `ScheduleWeakEvery(period, owner, cb, opts)` | owner 消失后回调返回 `Stop` | `timer.cpp:1240` |
| `SetExceptionHandler(h)` | 替换异常观察器（锁外执行） | `timer.cpp:1261` |
| `SetDurationLimits(limits)` | 动态替换 WARN/ERROR 阈值；负值或 `warn>error` 返回 false | `timer.cpp:1267` |
| `IsRunning()` / `PendingCount()` / `NextDeadline()` | 只读观测；Running 不计入 Pending | `timer.cpp:1275-1286` |
| `DebugSnapshot()` | 无分配只读快照（`timer.hpp:252-271`） | `timer.cpp:1288` |

### 3.3 决策、执行器与来源

| 类型 | 要点 |
|---|---|
| `TimerDecision`（`timer.hpp:123-164`） | `Stop/Continue/ContinueAfter/ContinueAt`；只影响当前任务 |
| `TimerOptions`（`timer.hpp:83-97`） | `repeat`、`period`、`wait_mode`、`spin_window`（默认 50µs）、`max_runs`（0=不限）、`skip_missed`（默认 true） |
| `TimerExecutor`（`timer.hpp:172-181`） | 单方法 `Submit(std::function<void()>)`；必须快速返回，返回 false 表示拒收 |
| `TimerSource`（`timer.hpp:192-202`） | 8 个调试分类；`kTimerSourceCount=8`（`timer.hpp:204`） |
| `SetThreadTimerSource` / `TimerSourceScope` | 线程本地标签；RAII 析构恢复旧值（`timer.cpp:1319-1342`） |
| `TimerDurationLimits`（`timer.hpp:99-109`） | 默认 WARN=30 天、ERROR=365 天；只告警不拒绝；0 关闭该级别 |

---

## 4. 设计

### 4.1 数据结构：`multimap` + 反向索引，**不是最小堆、也不是时间轮**

这是本次核对中最需要纠正的一条（旧文档与任务描述均提到"最小堆/定时轮"）：

- `Entries = std::multimap<time_point, shared_ptr<State>>`，`begin()` 即最早截止时间
  （`src/concurrency/timer.cpp:288-289`、`266-270`）；
- `m_index` 是 `unordered_map<State*, Entries::iterator>`，让 `Remove`/`Reset`/
  `Cancel` 直接定位，删除为 O(log n) 而非线性扫描（`timer.cpp:270-274`、`428-448`）；
- 每实例**一个**后台线程，无时间轮分层、无哈希桶（`timer.cpp:286-290`、`906-990`）。

复杂度：插入 O(log n)（multimap 红黑树），删除/重排 O(log n) + O(1) 查索引；
取堆顶 O(1)。**没有**"百万定时器 O(1)"的时间轮能力，这一点在容量规划时必须按
`n log n` 与单线程串行执行来估算。

### 4.2 后台线程主循环 `Run()`

四步（`src/concurrency/timer.cpp:893-990`）：

1. 锁内取 `m_generation` 与 `m_entries.begin()->first`；空表时在 `m_cv` 上无限等待；
2. 未到期时 `wait_until(deadline)`，谓词为 `m_stopping || m_generation != generation`
   —— 用**代际号**识别"等待期间插入/删除/重排"，避免漏唤醒与无谓重算；
3. 到期则**成批摘出**容器（同步维护 `m_index`），释放锁后逐个 `Execute`；
   摘出后执行保证回调里可以安全地再次注册/取消而不自锁；
4. `m_stopping` 置位则退出，否则回到 1。

等待策略由**队头任务**的选项决定：`kConditionVariable`（默认）或 `kBusyWait`
（只在 `remaining <= spin_window` 时自旋 + `yield`，`timer.cpp:940-959`）。
等待期间每次唤醒都累加 `m_background_wakeups`（调试快照字段）。

### 4.3 arm / fire / cancel 三条路径

| 路径 | 行为 | 代码 |
|---|---|---|
| arm | 锁内校验 `m_stopping`、分配 `timer_id`、`m_entries.emplace`、同步 `m_index`、`++m_generation`、统计+事件，解锁后 `notify_all` | `timer.cpp:394-426` |
| fire | 状态锁内校验 `active && kPending` → 置 `kRunning`、`++run_count`、**拷贝**回调/执行器/选项后立即解锁 → 锁外执行（周期回调 > 执行器 > 普通回调）→ 结果交 `Finish` | `timer.cpp:539-615` |
| cancel | 先从容器摘除（若存在），再在状态锁内拒绝重复取消，清空全部回调，统计+事件，`notify_all` | `timer.cpp:451-494` |

`Finish`（`timer.cpp:617-774`）是唯一的收尾决策点：

- 有异常 ⇒ `kFailed`，**不重排**（失败的周期不会静默重试），异常已先交给观察器；
- `kStop`、非周期、或 `run_count >= max_runs` ⇒ `kCompleted`；
- `kAfter/kAt/kContinue` ⇒ 算下次时间重新入队；
  `kFixedRate + skip_missed` 时会跳过已错过的周期并把下次推到 `now + period`，
  避免"追帧"忙循环（`timer.cpp:706-712`）；
- 服务正在停止（`m_stopping`）⇒ 一律收敛为 `kCancelled`，**不再插回队列**。

### 4.4 与 Scheduler 的集成（park/wake 的真实边界）

Timer **不认识 Fiber**，只认 `TimerExecutor`。桥接在调度器一侧：

- `Scheduler::TimerExecutorAdapter::Submit` 在 `Lifetime` 锁内校验调度器仍存活，
  然后 `spawn(std::move(callback))` 建新 G（`src/concurrency/scheduler.cpp:1964-1990`）；
- `Scheduler::schedule_at/schedule_after/schedule_every` 创建适配器并调用
  `TimerService::Default().ScheduleOn / ScheduleEveryOn`（`scheduler.cpp:2296-2340`）；
- 因此到期回调**只会把 G 发布到调度器队列**，Fiber 栈由某个 M 正常恢复，
  绝不会在计时线程上恢复别的线程的 Fiber 栈（`docs/timer_design_review.md` 的同名约束仍成立）；
- 调度器销毁前把 `Lifetime::owner` 置空，之后 `Submit` 返回 false，避免访问悬空对象
  （`scheduler.cpp:2028-2042`、`1968-1978`）。

**与 IOManager deadline 队列的分工**：`io::IOManager` 自己维护
`m_deadline_order`（`src/network/io.cpp:648-664`），超时与 `epoll_wait` 同一次返回，
延迟更低；Timer 适合业务定时器。**不要把每请求 IO 超时塞进 TimerService**，
否则计时线程会成为瓶颈（`timer.cpp:283-286` 明确写了这条）。

### 4.5 调试与观测

- `TimerDebugSnapshot` 在服务锁内采集 `active/next_deadline/sources[*].active`，
  统计计数为原子量，**可与容器状态差一拍**（`timer.hpp:236-245`）；
- 每个来源有 7 个计数：`active/armed/fired/cancelled/rescheduled/completed/failed`
  （`timer.hpp:226-234`）；
- 线程状态 `kStarting/kWaiting/kExecuting/kStopping/kStopped`（`timer.hpp:213-223`）；
- 事件环打点 `kTimerArm/kTimerFire/kTimerCancel/kTimerReschedule`（`timer.cpp:151-180`），
  OFF 构建由宏完全消除；
- `TimerSource` 目前**只有 `kScheduler` 会被自动打点**：`ScheduleOn/ScheduleEveryOn`
  硬编码（`timer.cpp:1189-1191`、`1222`）。其余来源（`kContext/kParkingCondition/kChannel/
  kTimedMutex/kSleep/kUser`）需要调用方自己用 `TimerSourceScope` 标注；**运行时其它模块
  并未内置打点**（全仓 `grep TimerSource::` 只命中 timer 自身）。未标注一律 `kUnknown`。

---

## 5. 边界与陷阱

### 5.1 时钟与数值

- 只用 `std::chrono::steady_clock`（`timer.hpp:363`），**不受墙上时钟回拨/校时影响**；
  但 `steady_clock` 在系统挂起（suspend）期间是否计时由平台决定，Linux 上通常**不包含**
  休眠时间——需要"墙钟语义"的超时必须由业务自己换算，Timer 不提供。
- `saturating_add` 显式处理加法溢出，把"很久以后"饱和到 `time_point::max()`，
  避免回绕成"已经过期"（`timer.cpp:74-90`）。
- 所有对外剩余时长统一钳到 0，`nonnegative` 保证调用方看不到负值（`timer.cpp:59-65`）。
- 超长任务仍可注册，只是按 `TimerDurationLimits` 打 WARN/ERROR 日志，不拒绝
  （`timer.cpp:98-119`）。

### 5.2 精度

- 默认 `kConditionVariable`：精度受内核唤醒与调度延迟影响，典型为几十 µs 量级；
- `kBusyWait` 仅在截止前 `spin_window`（默认 50µs）内自旋，会**占满一个核**，
  只适合极低延迟场景，不能作为全局默认（`timer.hpp:60-66`、`timer.cpp:940-959`）；
- 周期任务的实际间隔 = 回调耗时 + 周期（`kFixedDelay`）或计划时间线（`kFixedRate`），
  不是硬实时保证。

### 5.3 大量定时器与阻塞

- 单服务单线程：**一个长期阻塞的回调会推迟该服务所有后续任务**（`timer.cpp:283-286`、
  `timer_usage.md` 同名段落）。重活应交给 `TimerExecutor` 或独立服务实例。
- `n` 个定时器的注册成本是 `O(n log n)`；`multimap` 每个节点一次分配，
  `m_index` 再一次哈希节点分配，内存约为 `2n` 个节点。
- 周期任务不会并发重入：`Finish` 之后才重新入队，慢回调只会延后自身。

### 5.4 生命周期与异常

- **不能在本服务的回调里销毁该服务**：析构检测到 `m_thread.get_id() == this_thread`
  直接 `std::terminate()`（`timer.cpp:352-356`），因为线程 join 自身是未定义行为。
- 服务析构会：置 `m_stopping`、唤醒、join，然后把遗留条目逐个 `CancelState` 并计数
  （`timer.cpp:357-389`）——调用方不会看到永远 pending 的句柄。
- 回调异常被捕获后交给观察器；观察器自身抛出的异常被吞掉，不影响计时线程
  （`timer.cpp:776-792`）。
- `Timer::Cancel` 在服务已销毁后仍安全：走 `m_owner.lock()` 失败分支，直接改状态
  （`timer.cpp:1020-1037`）。
- 析构即取消是**最容易踩的坑**：`service.ScheduleAfter(1ms, cb);` 作为语句丢弃返回值，
  句柄当场析构，回调永不执行（`timer.hpp:26-33`）。

---

## 6. 测试

### 6.1 ctest 入口

| 用例 | 内容 | 注册位置 |
|---|---|---|
| `go2cpp_tests` + `GO2CPP_TEST_FILTER=timer` | `run_timer_tests()`：Timer 唤醒 Fiber、`WaitForChange` 代际等待、取消 | `cmake/Go2CppTests.cmake:216`、`tests/test_main.cpp:97` |
| `go2cpp_tests` + `GO2CPP_TEST_FILTER=timer_debug` | snapshot/来源统计/dump 格式/事件环/并发 dump | `tests/test_main.cpp:126` |
| `go2cpp_tests` + `GO2CPP_TEST_FILTER=timer_replay` | 录制-回放六个场景 | `tests/test_main.cpp:127` |
| `go2cpp_timer_stress` | 六场景压测，`TIMEOUT 300` | `cmake/Go2CppTests.cmake:329-335` |

### 6.2 压测规模（源码常量，非估算）

`tests/concurrency/test_timer_stress.cpp`：

| 场景 | 规模 | 断言 |
|---|---|---|
| 1 高频一次性 | 2000 个 1ms 定时器，全部持有句柄 | 计数精确等于 2000，30s 上限 |
| 2 状态不残留 | 跨代复用槽位 | B 不继承 A 的回调 |
| 3 并发 | 8 线程 × 300 定时器 = 2400 | 总数精确，多/少都报 FAILED |
| 4 随机取消 | 500 轮，`mt19937(20261017)`，0–800µs | 取消后计数稳定不再增长 |
| 5 周期 + 取消 | 1ms 周期至少 3 tick；再建/取消 100 次 | 取消后不再 tick |
| 6 混合压力 | 1200 轮，`mt19937(777)`，四种形态 | 静置窗口内计数稳定 |

### 6.3 本次复核运行（2026-10-26）

命令与结果（`/tmp/docs_timer_io_tests.log`）：

| 命令 | 结果 | 耗时 |
|---|---|---|
| `GO2CPP_TEST_FILTER=timer ./go2cpp_tests` | `all checks passed` | 303 ms |
| `GO2CPP_TEST_FILTER=timer_debug ./go2cpp_tests` | 通过 | 51 ms |
| `GO2CPP_TEST_FILTER=timer_replay ./go2cpp_tests` | 通过 | 646 ms |
| `./go2cpp_timer_stress` | 六场景全部 `failures=0` | 738 ms |

`timer_debug` 输出的注册/取消微基准（10k 次，Release、`GO2CPP_DEBUG_DUMP=OFF`）：

| 操作 | p50 | p95 | p99 | max |
|---|---:|---:|---:|---:|
| `ScheduleAfter` | 166 ns | 3,683 ns | 18,176 ns | 214,109 ns |
| `Cancel` | 166 ns | 1,124 ns | 8,012 ns | 189,338 ns |

同次运行：10k 次 `DebugSnapshot` 共 123,696 ns；并发 dump 84,681 次后 `active_after=0`。

> **验证口径与限制**：测试二进制来自 `build-work`（2026-10-21 22:17 构建的 Release）。
> 该构建对应的提交为 `6c0a9d1`，`git diff 6c0a9d1 HEAD -- <timer/scheduler/测试全部相关文件>`
> 为**空**，即二进制对应的 timer 源码与当前工作树一致。当前 HEAD 整体无法重编：
> `src/os/dir.cpp` 引用了未定义的 `detail::ContainsNul`（`git grep` 只命中 7 处调用、
> 无定义），与本模块无关，故无法从 HEAD 现场产出新二进制。上文数字是该提交上的真实运行结果，
> **不是**引用旧文档。

### 6.4 历史基准（引用，非本次复测）

`docs/timer_benchmark.md` 记录：200 个 5ms 任务、Release、WSL 主机下
每任务一线程 27.91ms / P50 8911µs；TimerService 单线程 5.50ms / P50 444µs；
Scheduler+Fiber 6.41ms / P50 943µs；coost `co::sleep` 对照 7.11ms / P50 238µs。
该组数字的编译环境与 run 与本机不同，且 coost 用的是相对睡眠，**只能作量级参考**。

---

## 7. 已知缺口（如实标注）

- 没有"最小堆/时间轮"实现；也没有 `wait_finished()`（`Cancel` 不阻塞等待回调结束），
  旧 `timer_design_review.md` 第五节把它列为"建议接口"，**尚未实现**。
- 没有热替换时钟接口；`TimerService` 不可 `join_for(timeout)`，析构只能无界等待。
- 来源分类只在 timer 内部自动标注 `kScheduler`；Context/Channel/ParkingCondition/
  timed_mutex/`time.Sleep` 的调用点都未打点（`time.Sleep` 走 Hook 的 `nanosleep`，
  与 Timer 无关）。
- 本文不覆盖 `Scheduler::schedule_*` 的 G 生命周期细节，见 [scheduler.md](scheduler.md)。
