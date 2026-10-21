# 并发原语：随机 + 大数据量 + 多场景 5 轮测试

- 日期：2026-10-21
- 测试源：`tests/concurrency/test_random_5rounds.cpp`
- 驱动脚本：`tests/concurrency/run_random_5rounds.sh`
- 原始数据：`tests/bench/results/random_5rounds_20261021-220629.tsv`
- 每轮日志：`tests/bench/results/random5_20261021-220629/`
- 报告副本：`/tmp/random_5rounds.md`

## 1. 结论

| 项 | 结果 |
|---|---|
| 5 轮完成 | 5/5 |
| OFF 轮总操作数 | 7,838,342（每轮 1.42M~1.96M，均 ≥100 万） |
| ON（debug）轮总操作数 | 7,802,299 |
| 合计原语操作 | 15,640,641 |
| 死锁 | 0（watchdog 未触发；最慢单轮 3.63s，上限 120s） |
| 崩溃 / 未处理异常 | 0（所有场景 `bad_ops=0`，退出码 0） |
| 计数守恒断言 | 全部通过（每轮 25~28 项，共 132 项） |
| debug 事件关键事件 | 5/5 轮 `snapshot == total`（send/recv/wake 逐项相等，零丢失） |
| 场景覆盖 | 6/6：`pc`、`rwcache`、`workpool`、`pipeline`、`mixedlocks`、`storm` |
| 峰值 RSS | 6.0~6.2 MB（5 轮稳定，无增长趋势） |
| 1 分钟 loadavg | 0.57~0.67（全程 <2；5 分钟 1.96~2.10 为历史残留） |

## 2. 环境与命令

- 环境：WSL2 Ubuntu（12 vCPU，19 GiB），2026-10-21 22:06 起跑；共享机器，非独占。
  运行前 1 分钟 loadavg 0.58，轮内 0.57~0.67（均 <2，满足性能数据条件）。
- 构建：
  - OFF：`build-work`（`-O3 -DNDEBUG`，`GO2CPP_DEBUG_DUMP=OFF`）；
  - ON：`build-r5-on`（`-O3 -DNDEBUG`，`GO2CPP_DEBUG_DUMP=ON`，`GO2CPP_REPLAY=OFF`，
    事件容量 256/关键区 64）。
- 复现命令（本次固定种子；不指定 `SEEDS` 时脚本每次随机生成并打印）：

```bash
env ROUNDS=5 MIN_OPS=1000000 SCALE_OFF=100 SCALE_ON=100 \
  SEEDS="2227925036 3957249165 3800327366 3523277772 1705200321" \
  bash tests/concurrency/run_random_5rounds.sh
```

- 超时：脚本外层 `timeout -k 5 120`；程序内部 watchdog 总限 110s、无进展 25s，
  触发时打印现场并把事件 dump 写到 `/tmp/r5_watchdog_round<r>_seed<s>.log`，然后 `_exit(42)`。

## 3. 5 轮数据

| 轮 | 种子 | 场景 | 操作数 | 内部耗时(s) | 外部 wall(s) | 峰值 RSS(KB) | CPU%(计算) | user(s) | sys(s) | 结果 |
|---|---|---|---|---|---|---|---|---|---|---|
| 1 | 2227925036 | pc, rwcache, pipeline, mixedlocks | 1,955,750 | 1.719 | 1.720 | 6216 | 192% | 0.74 | 2.56 | PASS |
| 2 | 3957249165 | rwcache, pipeline, mixedlocks, storm | 1,512,918 | 3.631 | 3.640 | 6016 | 61% | 1.04 | 1.19 | PASS |
| 3 | 3800327366 | rwcache, workpool, pipeline, mixedlocks | 1,527,542 | 0.616 | 0.630 | 6016 | 175% | 0.44 | 0.64 | PASS |
| 4 | 3523277772 | pc, workpool, pipeline, mixedlocks | 1,421,663 | 1.819 | 1.830 | 6156 | 176% | 0.73 | 2.47 | PASS |
| 5 | 1705200321 | pc, workpool, pipeline, mixedlocks | 1,420,469 | 1.749 | 1.750 | 6228 | 173% | 0.68 | 2.36 | PASS |

说明：

- 「内部耗时」是测试进程内 `steady_clock` 计时（主流程结束后由条件变量唤醒
  watchdog，不再空等 tick）；「外部 wall」来自 `/proc/uptime`（单调时钟），两者一致。
- `/usr/bin/time -v` 的 wall 与 CPU%（CLOCK_REALTIME 基线）在 WSL2 下会偶发漂移
  （本轮 5 轮恰好一致，但测试过程中曾出现 4.0s 报 2.0s，见 §5.2）；
  本表以内部计时为准，CPU% 由 `(user+sys)/内部耗时` 重算。
- 第 2 轮 CPU 61% 是因为 storm 场景包含大量 `ConditionVariable::WaitFor(200us)`
  超时睡眠（wall 3.63s，CPU 2.2s）。

### 3.1 场景级明细（OFF 轮）

格式：`操作数 / 场景耗时`；`—` 表示该轮未抽到该场景。

| 轮 | pc | rwcache | workpool | pipeline | mixedlocks | storm |
|---|---|---|---|---|---|---|
| 1 | 480,009 / 1.158s | 596,243 / 0.155s | — | 240,000 / 0.294s | 639,498 / 0.113s | — |
| 2 | — | 590,961 / 0.188s | — | 240,000 / 0.306s | 640,120 / 0.142s | 41,837 / 2.994s |
| 3 | — | 587,288 / 0.144s | 61,108 / 0.046s | 240,000 / 0.289s | 639,146 / 0.136s | — |
| 4 | 480,009 / 1.319s | — | 61,248 / 0.052s | 240,000 / 0.316s | 640,406 / 0.132s | — |
| 5 | 480,009 / 1.283s | — | 60,820 / 0.034s | 240,000 / 0.297s | 639,640 / 0.134s | — |

### 3.2 debug 事件轮（ON，同一 seed/round，100% 规模）

| 轮 | 事件总数 | 定向校验（snapshot/total） | 结果 |
|---|---|---|---|
| 1 | 1,577,981 | send 10/10，recv 10/10，wake 10/10 | PASS |
| 2 | 1,398,880 | send 12/12，recv 8/8，wake 10/10 | PASS |
| 3 | 1,347,555 | send 9/9，recv 11/11，wake 14/14 | PASS |
| 4 | 1,079,273 | send 10/10，recv 10/10，wake 8/8 | PASS |
| 5 | 1,098,326 | send 10/10，recv 10/10，wake 8/8 | PASS |

`wake <= send+recv` 与 `cv_notify <= cv_wait` 两不变量在 5 轮均成立（例：
round 2 的 wake 25,538 ≤ send 50,201 + recv 39,988；cv_notify 23,097 ≤ cv_wait 128,152）。

## 4. 每轮断言

每轮固定断言（`checks` 为各场景断言数之和）：

1. **无死锁**：watchdog 线程监视进度心跳与总时长；5 轮均未触发，进程退出码 0。
   测试内部曾用 SIGUSR1 栈 dump 定位过一次真实卡死（见 §5.1），确认机制有效。
2. **计数守恒**：
   - `pc`：`produced == 240000`；`received + drained == 240000`；唯一性位图无重复/越界；
     WaitGroup 计数归零；4 个消费者全部经 Context 取消退出。
   - `rwcache`：`torn == 0`；`version == 60000`；`payload == version*2`；
     通知 `recv == sent`（关闭后 drain 补齐）；RWMutex 无写锁残留。
   - `workpool`：`processed == 20000`；任务和校验；队列空；`waits == waits_done`。
   - `pipeline`：`in == mid == out == 80000`；数值和校验；超时次数记录。
   - `mixedlocks`：`counter_mu == mu_locks`；`counter_rec == recursive_locks`；
     `version == rw_writes`；`torn == 0`；无写锁残留。
   - `storm`：`sent == recv_ok`（关闭后 drain 补齐）；`waits == waits_done`；`bad == 0`；
     Send/TrySend/Recv/TryRecv/RecvFor/Select/Wait/Notify 全部覆盖。
3. **无崩溃/未处理异常**：所有线程循环体在 `try/catch` 内，`bad_ops` 恒 0；`failures=0`。
4. **debug 事件**（仅 ON 轮）：
   - 累计不变量：`wake <= send+recv`、`cv_notify <= cv_wait`、`TotalCountAll() > 0`；
   - 关键事件零丢失：Reset 后做 16 次无缓冲 channel 往返（发送线程保持存活），
     `Snapshot` 逐条统计与 `TotalCount` 累计逐项相等（见 §3.2）。

## 5. 发现的问题与风险

### 5.1 Go 风格 Mutex 不可重入：随机操作序列的自死锁陷阱（测试代码已规避）

- **现象**：最初版 `storm` 场景把 `ConditionVariable` 的等待/通知与随机 `lock/unlock`
  共用同一把 `sync::Mutex`。某个线程在仍持有该锁时又执行 `cv.WaitFor(mutex)` 内部的
  `Lock()`，触发自死锁；随后其他线程全部阻塞在 `Mutex::LockImpl`，watchdog 判定 stuck。
- **定位手段**：watchdog 触发后向各线程发 `SIGUSR1` 打印栈，栈显示所有业务线程停在
  `Mutex::LockImpl` / `LockForConditionWait`，无任何线程在临界区内执行。
- **判定**：与 Go `sync.Mutex` 语义一致（不可重入，`ConditionVariable::Wait` 要求调用方
  已持锁），**不是库缺陷**；但它是随机测试最容易踩的坑：操作序列必须显式跟踪
  「本线程是否已持锁」。测试中已把 CV 改为独立 `cv_mutex` 并加注释说明。
- **建议**：`ConditionVariable` 文档可补一句「同一线程不得在持锁状态下再次 Lock 同一
  Mutex（含经由 Wait 重入）」的显式警告；随机/属性测试生成器应维护持锁状态机。

### 5.2 WSL2 下 `/usr/bin/time -v` 的 wall/CPU% 会漂移（环境风险）

- **现象**：同一份 round 2 代码，`/usr/bin/time -v` 报 `Elapsed 0:02.00`（实际内部
  计时 4.009s、`/proc/uptime` 4.02s），CPU% 随之从 55% 翻倍到 108%；重跑后又恢复正常。
- **原因**：GNU time 用 `CLOCK_REALTIME` 测 wall；WSL2 的 realtime 时钟在负载下会被
  虚拟化平台校正/漂移，而测试内部用的 `steady_clock` 不受影响。
- **规避**：脚本以内部 `steady_clock` 为耗时主口径，`/proc/uptime` 交叉验证，
  CPU% 用 `(user+sys)/内部耗时` 重算；`time -v` 原始值保留为参考列（TSV 中有两列）。
- **建议**：CI 性能门禁用 `CLOCK_MONOTONIC`/`steady_clock`，不要直接信 `time -v` 的 wall。

### 5.3 EventRing::Snapshot 只覆盖存活线程（debug 窗口语义，非缺陷）

- **现象**：场景线程 `join()` 后再 `Snapshot()`，返回 0 条；而 `TotalCount` 仍累计。
- **原因**：线程退出时 `Registration::~Registration` 调用 `RemoveRing`，其 ring 从全局
  表移除（`src/debug/debug_event_ring.cpp`）；这是有意的生命周期管理，不是丢事件。
- **影响**：若在 join 后做「关键事件不丢」校验会误判。定向校验已改为让发送线程
  存活到快照读完，再断言 `snapshot == TotalCount`。
- **建议**：debug 工具/文档注明「Snapshot 是存活线程窗口」；离线回放应以 dump 文件
  或 `TotalCount` 为准。

### 5.4 原语事件按路径记录，不能当等量调用计数（语义说明）

- `kMutexLock` 只在慢路径（竞争/阻塞）记录，`kMutexFastLock` 按 32 次采样；
  `kCvNotify` 只在真正唤醒等待者时记录；`kCvWait` 在进入等待、取消、返回多个路径记录。
- 因此「事件数 == 调用数」不成立。本测试用三个可证明的不变量：
  `wake <= send+recv`、`cv_notify <= cv_wait`、容量内 `snapshot == total`。
- **风险**：若后续把事件计数接监控告警，需要按事件语义设置阈值，避免把
  「慢路径次数」误当「总调用次数」。

### 5.5 debug 构建开销 2.2~5.1 倍（性能提示）

- OFF 轮 0.62~3.63s，ON 轮 3.17~6.13s（同 seed/round/规模）。
- 主要成本来自原语慢路径事件记录（`kMutexLock`、`kCvWait`、`kChannelSend` 等）与
  高频短超时下的 park/wake 事件。
- **建议**：生产默认保持 `GO2CPP_DEBUG_DUMP=OFF`；压测 debug 开销时区分「采样」与
  「全量」两种配置（既有 `tests/bench/run_concurrency_baseline.sh` 已覆盖）。

### 5.6 高频短超时的系统开销（观察项，未触发问题）

- `storm` 场景 200µs 级 `WaitFor` 超时使 sys 时间占比升高（如 round 2：sys 1.19s /
  wall 3.63s，CPU 61%），意味着大量 futex 唤醒与定时器操作。
- 这不是缺陷，但提示：业务若大量使用「极短超时的 CV/Channel 轮询」，应评估
  定时器合并与退避策略，否则 CPU 花在 syscall 上。

## 6. 覆盖范围与未覆盖项

已覆盖（每轮随机 4 个场景、5 轮并集覆盖全部 6 个）：

- 生产者-消费者：`Channel` 有缓冲/阻塞收发、`WaitGroup`、`Context` 取消；
- 读写缓存：`RWMutex` 读写混跑 + `Channel` 失效通知（TrySend/TryRecv/Close/drain）；
- 工作池：`Mutex` + `ConditionVariable`（WaitFor 超时）+ 队列 + NotifyAll 停止；
- 管道：三级 `Channel` 串联 + `RecvFor` 超时 + Close 传播；
- 混合锁：`Mutex` / `RWMutex` / `RecursiveMutex`（1~3 层递归）随机切换；
- 原语风暴：随机 lock/unlock、TrySend/TryRecv、RecvFor 超时、Select+Default、
  CV WaitFor/NotifyOne，覆盖 `WaitGroup::WaitFor`。

未覆盖 / 限制：

- **受管 G 路径**：本测试全部使用 `std::thread`（native futex/condvar 路径），
  未启动 GMP Scheduler，因此 `Scheduler::spawn` / Fiber park-wake 路径未被本轮覆盖
  （由 `tests/concurrency` 既有 Fiber/Scheduler 测试覆盖）。
- **异常路径**：fd 耗尽、OOM、信号中断、多进程竞争未覆盖；仅覆盖超时、Context
  取消、channel close。
- **性能代表性**：共享 WSL2 环境、非独占，1 分钟 loadavg 0.57~0.67（<2），
  数据仅用于相对比较，不作为绝对性能基线。

## 7. 复现与产物

```bash
# 随机种子（脚本打印并写入报告；可用下面的固定种子复现本次结果）
env ROUNDS=5 MIN_OPS=1000000 \
  SEEDS="2227925036 3957249165 3800327366 3523277772 1705200321" \
  bash tests/concurrency/run_random_5rounds.sh

# 只跑单轮（调试用）
/path/to/test_random_5rounds_off --seed 3957249165 --round 1 --min-ops 1000000

# 只打印某轮的随机场景组合
/path/to/test_random_5rounds_off --plan --seed 3957249165 --round 1
```

产物：

- `tests/bench/results/random_5rounds_20261021-220629.tsv`（每轮原始数据）
- `tests/bench/results/random_5rounds_20261021-220629.md`（脚本生成的简版表）
- `tests/bench/results/random5_20261021-220629/`（每轮 stdout/stderr、`time -v`、meta）
- `/tmp/random_5rounds.md`（完整报告副本）

未提交（按要求不 commit）。
