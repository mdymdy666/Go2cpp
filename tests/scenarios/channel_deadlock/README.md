# channel_deadlock：Channel 互等死锁与 select 语义恢复

本场景对比**同一个「双方互等」协议**的四写法：

| 模式 | 写法 | 是否永久死锁 | 恢复方式 |
|---|---|---|---|
| `baseline` | `A: chX->Recv()`、`B: chY->Recv()` 纯阻塞 | **是**（stuck=2） | 无，只能外部 SIGTERM |
| `timeout` | `Select({RecvCase(ch), AfterCase(200ms)})` | 否 | 200ms 定时器分支退出 |
| `multi` | `Select({RecvCase(自己的入站), SendCase(对端的入站, token)})` | 否 | 两路 select 的 send/recv 配对 |
| `default` | `Select({RecvCase(ch), DefaultCase()})` 循环重试 | 否 | 无就绪 → 计数 → 降级退出 |

**不依赖 Doctor**：没有 `deadlock-report`、`ctx.cancel`、`force-wake`；
恢复完全靠 select 语义。`stuck` 定义为两个 channel 的 `recv_waiters` 之和。

## 场景图

```text
  Fiber A: 等 chX  ──┐         ┌── 等 chY  :Fiber B
                     │         │
                     └─ 互等 ──┘
  协议约定：A 收到 chX 后向 chY 发送，B 收到 chY 后向 chX 发送；
  但双方都在等对方的第一条消息，两个发送点都到不了。
```

两个 channel 都是 `MakeChannel<int>(0)`（无缓冲）。每轮新建 channel，
结束后检查 `recv_waiters==0`、`send_waiters==0`、`IsTaskAlive(gid)==false`、
`refs_x==refs_y==1`（只剩场景自身持有）。

## 关键设计说明：为什么 multi 用「收 + 发」

父任务给的示意是：

```cpp
select { case <-chX: ...; case <-chY: ...; }
```

但如果**双方都不发送**，纯 recv 的两路 select 仍然是死锁：两个 channel 的
接收队列都为空，没有任何 case 会就绪；把同一个「无人发送」的等待登记到
两个 channel 上，并不会产生新的可完成路径。真正能破互等的是**可完成的
替代分支**，因此本场景的 multi 写成：

```cpp
// A
select { case <-chX: ...; case chY <- token: ...; }
// B
select { case <-chY: ...; case chX <- token: ...; }
```

A 的 `SendCase(chY)` 与 B 的 `RecvCase(chY)`（或 B 的 `SendCase(chX)` 与
A 的 `RecvCase(chX)`）由 `Channel` 的 `SelectWaitState::TrySelectPair`
原子配对，两个 select 同时 selected；实测每轮都是一 send 一 recv、双方完成。
这是「select 多路」能解互等的关键，而不是多注册一个永远无消息的接收分支。

## 构建与运行

```sh
cd /UserData/CodexWorkSpace/Go2Cpp-work

flock /tmp/go2cpp_build.lock cmake -S . -B build-doctor-full
flock /tmp/go2cpp_build.lock cmake --build build-doctor-full \
      --target go2cpp_channel_deadlock -j 8

# 完整验收：baseline + timeout + multi + default + --repro 10
bash tests/scenarios/channel_deadlock/run_channel_deadlock.sh \
     build-doctor-full --repro 10
```

单独跑某一模式：

```sh
BIN=build-doctor-full/go2cpp_channel_deadlock
$BIN --mode=baseline --hold-ms=60000     # 需要外部 SIGTERM
$BIN --mode=timeout  --repro=10 --timeout-ms=200
$BIN --mode=multi    --repro=10
$BIN --mode=default  --repro=10 --default-retries=3 --default-interval-ms=20
```

脚本支持 `--quick`（repro=1）、`--ctest`（由 ctest 以构建目录为 CWD 调用）。
ctest 模式下缺 `go2cpp_channel_deadlock` 时脚本按**失败**上报（exit 1），
不再退化成 SKIP；只有手工运行且未构建时才 `exit 77`。

## 实测输出

### 1) baseline：纯阻塞互等（对照组）

```text
DEADLOCK_READY mode=baseline stuck=2 gid_a=1 gid_b=2
  chX.recv_waiters=1 chX.select_receivers=0
  chY.recv_waiters=1 chY.select_receivers=0 hold_ms=60000
STILL_STUCK elapsed_ms=250  stuck=2 chX.recv_waiters=1 chY.recv_waiters=1
STILL_STUCK elapsed_ms=501  stuck=2 chX.recv_waiters=1 chY.recv_waiters=1
...
STILL_STUCK elapsed_ms=1754 stuck=2 chX.recv_waiters=1 chY.recv_waiters=1
BASELINE_RESULT mode=baseline deadlock=1 stuck=2 recovered=0
  held_ms=1754 heartbeats=7 stopped=1 a_alive=1 b_alive=1 pass=1
```

脚本在 1.5s 后确认进程仍存活、`STILL_STUCK` 心跳持续、没有 `RESULT`，
再发 SIGTERM；`held_ms=1754` 是观察窗口，不是恢复时间。baseline 本身
可无限期卡住（`--hold-ms=60000`）。

### 2) timeout：`AfterCase(200ms)`

```text
STUCK mode=timeout round=1 stuck=2 chX.recv_waiters=1 chY.recv_waiters=1
RESULT mode=timeout round=1 pass=1 finished=1 tasks_done=1
  stuck_observed=1 stuck_before=2 stuck_after=0
  a_timed_out=1 a_index=1 a_status=ready a_kind=2 a_elapsed_ms=200
  b_timed_out=1 b_index=1 a_status=ready b_kind=2 b_elapsed_ms=200
  recv_waiters_x=0 recv_waiters_y=0 alive_a=0 alive_b=0
```

stderr：

```text
[A] TIMEOUT gid=1 timeout_ms=200 elapsed_ms=200
[B] TIMEOUT gid=2 timeout_ms=200 elapsed_ms=200
```

先用 `STUCK stuck=2` 证明确实先互等，再由定时器分支退出；最终
`stuck_after=0`。200ms 是 select 的 `AfterCase`（Go `time.After` 的等价物），
不是 `context.WithTimeout`；本场景不需要 ctx。

### 3) multi：两路 select（收 + 发）

```text
RESULT mode=multi round=1 pass=1
  a_completed=1 a_index=1 a_kind=3   (send)
  b_completed=1 b_index=0 b_kind=1   (recv)
  stuck_after=0 recv_waiters_x=0 recv_waiters_y=0
  alive_a=0 alive_b=0 refs_x=1 refs_y=1
```

stderr：

```text
[B] MULTI gid=2 selected=1 index=0 role=recv status=ready elapsed_ms=0
[A] MULTI gid=1 selected=1 index=1 role=send status=ready elapsed_ms=0
```

每轮都是一 send 一 recv，双方同时 selected；耗时在亚毫秒级
（RESULT 的 `a_elapsed_us=62`、`b_elapsed_us=48`；日志的 ms 字段取整为 0）。

### 4) default：非阻塞降级

```text
RESULT mode=default round=1 pass=1
  a_fallback=1 a_no_ready=3 a_kind=5 a_elapsed_ms=62
  b_fallback=1 b_no_ready=3 b_kind=5 a_elapsed_ms=61
  stuck_after=0 recv_waiters_x=0 recv_waiters_y=0
```

stderr 每轮 3 次 `NO_READY`（共 6 行），之后 `FALLBACK`；没有 park，
因此 `stuck_after=0`。

## `--repro 10` 实测

```text
REPRO mode=timeout rounds=10 passed=10 failed=0
  avg_a_ms=200.10 avg_b_ms=200.10 min_ms=200 max_ms=201
REPRO mode=multi   rounds=10 passed=10 failed=0
  avg_a_ms=0.00 avg_b_ms=0.00 min_ms=0 max_ms=0
REPRO mode=default rounds=10 passed=10 failed=0
  avg_a_ms=60.00 avg_b_ms=60.00 min_ms=60 max_ms=60
```

- timeout：10/10 超时退出，耗时 200–201ms（均值 200.1ms；另一次运行
  max=204ms，属毫秒级抖动）；
- multi：10/10 双方完成，全部一 send 一 recv，亚毫秒级；
- default：10/10 走降级，每方 3 次无就绪，约 60ms。

脚本整体结果：`PASS=42 FAIL=0`。

## 对比表

| 模式 | 是否死锁 | 恢复方式 | 实测耗时 | 残留 waiters | 任务终态 |
|---|---|---|---|---|---|
| baseline | 是（stuck=2） | 无；外部 SIGTERM | 观察窗 1.75s（可无限） | 2 | 仍 kWaiting |
| timeout | 否 | `AfterCase(200ms)` 超时分支 | 200–201ms | 0 | kDead |
| multi | 否 | select send/recv 配对 | <1ms（62/48us） | 0 | kDead |
| default | 否 | `DefaultCase` 降级 | 60–64ms（3×20ms） | 0 | kDead |

## 限制（如实声明）

1. **baseline 没有恢复路径**：它是纯阻塞 `Recv()`，只能由外部 SIGTERM
   结束。这正是对照组要展示的「永久 stuck」，不是可用的生产写法。
2. **timeout 不是取消阻塞的 `Recv`**：`AfterCase` 只是 select 的一个
   定时器分支。若业务直接调用 `ch->Recv()`，加定时器不会生效；必须把
   等待改写成 select。
3. **multi 必须有可完成的替代分支**：纯 recv 的两路 select 在双方都
   不发送时仍然是死锁。本场景用 `SendCase` 提供替代分支；如果协议本身
   不允许先发，select 也救不了，需要改协议或加超时/降级。
4. **default 是轮询降级**：`--default-interval-ms=0` 会忙等；本场景默认
   3 次 ×20ms 后退出，不保证业务一定完成，只保证不永久 park。
5. **计时是 WSL2/Linux 实测**（2 P、2–4 M、200ms 定时器）；不同负载下
   有毫秒级抖动。multi 的亚毫秒值来自 `elapsed_us`，日志的 ms 字段取整为 0。
6. **无 Doctor 依赖**：本场景不注册 DiagnosticAgent，也不调用
   ctx.cancel / force-wake；因此不覆盖「外部取消」路径，那条路径在
   `ctx_cancel_rollback` 场景里验证。

## ctest 注册

- 仅在 `GO2CPP_BUILD_ENHANCED=ON` 时注册 `scenario-channel-deadlock`；
  默认构建 ctest 用例数保持 **130** 不变（实测 `build-work`：
  `100% tests passed, 0 tests failed out of 130`，27.52s）。
- 增强构建实测：注册后 62 条用例（原 61 + 本场景），
  `ctest -R '^scenario-channel-deadlock$'` 通过（4.87s）。
- 缺 demo 可执行文件时脚本 `exit 77`，ctest 记为 SKIP。
- `tests/run_all_tests.sh` 的场景步骤也加入了本场景（ctest 已覆盖时记 SKIP）。

## 实测环境

WSL2 Ubuntu、Linux x86_64、Release 构建；Scheduler 2 个 P、2–4 个 M；
计时来自 `std::chrono::steady_clock`，日期 2026-10-21。

