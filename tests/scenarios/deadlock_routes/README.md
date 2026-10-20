# deadlock_routes：100+ client 锁顺序死锁的端到端场景

> 真实 HTTP 服务 + 122 个并发 client + 两把真实 `sync::Mutex` 的锁顺序反转，
> 用 DebugServer 抓现场、Doctor 判定/解锁。**不是 mock**：所有等待都发生在
> 服务端 Fiber 的真实 park 上。

## 场景构成

| 组件 | 说明 |
|---|---|
| `deadlock_routes_server.cpp` | 服务端 + 负载 client + 编排线程，单进程 |
| `run_deadlock_scenario.sh` | 端到端验收脚本（启动/抓 dump/判定/解锁/校验） |
| 资源 | `res-a` / `res-b` / `res-c` / `res-d`，各一把 `sync::Mutex` + 共享计数器 |
| 交叉依赖 | `/res-a` 先 A 后 B，`/res-b` 先 B 后 A；`/res-d` 先 D 后 C |
| 加载路由 | `/res-c`、`/res-d`、`/health`、`/res-a-ro`、`/res-b-ro`（单锁） |
| 并发 client | 120 个加载线程 + 2 个触发 client = **122**；压测期 1500ms |
| 死锁触发 | 两个 client 分别 GET `/res-a?hold=1`、`/res-b?hold=1`：handler 拿到第一把锁后在 `ParkingCondition` 会合，双方都持锁后再拿第二把 → A↔B 互等 |
| 追加卡死 | 死锁建立后再发 4 条 `/res-a-ro` + 4 条 `/res-b-ro`，排进 A/B 等待队列 → 卡住数 10 |

阶段行（stdout，脚本按行解析）：

```text
READY pid=... port=... clients=122 debug_socket=... doctor_socket=...
WARMUP res-a.get=200 res-a.post=200 res-b.get=200 res-b.post=200
LOAD_START clients=122 load_ms=1500
LOAD_DONE load_requests=... ok=... failed=... success_rate=... qps=... p50_ms=... p95_ms=... p99_ms=...
LOAD_ERRORS connect=... receive=... sample_connect="..." sample_receive="..."
LOAD_QUIESCENT active_load=0
[scenario] trigger gid=... first=res-a rendezvous=1 -> second lock res-b
[scenario] trigger gid=... first=res-b rendezvous=1 -> second lock res-a
DEADLOCK_READY gid_a=... gid_b=... waiters_a=... waiters_b=...
STUCK_PROBE stuck=10 ...
QUIESCENT stuck=10 active_load=0 waiters_a=5 waiters_b=5
[go2cpp][doctor][audit] mutex-unlock --force ...
[scenario] stale-unlock-skip gid=... mutex=res-a ...
RECOVERED stuck_after=0 ... stale_unlock_skips=1 ...
FINAL ...
FINAL_ERRORS ...
FINAL_ROUTES ...
FINAL_RESOURCES ...
```

## 运行

```bash
# 构建（三个开关都要开）
cmake -S . -B build-doctor-full \
  -DGO2CPP_DEBUG_DUMP=ON -DGO2CPP_DIAGNOSTIC=ON -DGO2CPP_WITH_CLI=ON
cmake --build build-doctor-full -j

# 完整验收（17 项断言；dump/report/命令输出写到 /tmp）
bash tests/scenarios/deadlock_routes/run_deadlock_scenario.sh build-doctor-full

# 触发率：新进程重复 10 次
bash tests/scenarios/deadlock_routes/run_deadlock_scenario.sh build-doctor-full --repro 10
```

产物：

- `/tmp/scenario-deadlock-dump.txt`：`scheduler,fiber,mutex,events` 四段 dump；
- `/tmp/deadlock-report.txt`：`deadlock-report` 原文（含等待图边）；
- `/tmp/deadlock-force-wake.txt`、`/tmp/deadlock-unlock-refused.txt`、
  `/tmp/deadlock-unlock-forced.txt`：Doctor 命令原文；
- `/tmp/deadlock-scenario-<tag>.log.<port>`：服务端 stdout+stderr。

## 观测要点

dump 的 `fiber` 组件由场景自己提供注解，能直接看到「谁在等哪把锁」：

```text
[component fiber]
fiber.0.gid=4794
fiber.0.role=res-a
fiber.0.state=kWaiting
fiber.0.waiting_on=res-b
fiber.0.held=res-a
fiber.1.gid=4795
fiber.1.role=res-b
fiber.1.state=kWaiting
fiber.1.waiting_on=res-a
fiber.1.held=res-b
```

`mutex` 组件给出 owner / waiters：

```text
[component mutex]
mutex.res-a.held=true
mutex.res-a.waiters=5
mutex.res-a.owner_gid=4794
mutex.res-a.owner_known=true
mutex.res-b.held=true
mutex.res-b.waiters=5
mutex.res-b.owner_gid=4795
```

`deadlock-report` 的 mutex 边：

```text
edge.2.from=res-a
edge.2.kind=mutex
edge.2.owner_gid=4794
edge.2.owner_alive=true
edge.2.waiters=5
edge.3.from=res-b
edge.3.owner_gid=4795
edge.3.owner_alive=true
edge.3.waiters=5
```

## 受控强制解锁的协作约定（重要）

`mutex-unlock --force` 直接执行 `Mutex::Unlock()`，**被夺锁的 owner 不会收到
任何通知**。若它之后仍按原逻辑 `Unlock()`，轻则抛
`unlock of unlocked mutex`，重则把锁从新持有者手里抢走。

本场景的 handler 因此遵守一条约定：

1. 拿到第一把锁后采样 `DiagnosticAgentStats::forced_unlocks`；
2. 释放第一把锁前再采样一次，并检查 Mutex 的 `held/owner_gid`；
3. 只有「计数没变」或「锁仍 held 且 owner 是自己」才 `Unlock()`，否则跳过
   （stderr 打 `stale-unlock-skip`）。

**这只是 demo 的协作式恢复，不是通用做法**：`--force` 仍然会破坏互斥语义，
只适用于进程已经卡死、数据一致性已不可保的故障处置。详见
`docs/diagnostic-channel.md` §4.5.1。

## 已知边界（如实记录）

- 加载 client 用 `http::Client`，每请求新建连接（`Connection: close`）。
  高并发短连接下会偶发 `connect` 失败（错误串 `Bad file descriptor`）与
  `receive` 失败（`Connection reset by peer`），成功率约 95%；失败分类由
  `FINAL_ERRORS` 如实输出。这与死锁场景无关，是客户端/hook 的既有行为；
- 加载期不让 client 访问 `/res-a`、`/res-b`：否则压测中随时可能出现计划外
  的自然死锁，触发率会退化成随机事件。交叉锁路由只由 2 个触发 client
  在固定时序下并发访问；
- 服务端 `deadlock_threshold=600ms`，`deadlock-report` 需要全局无进展
  超过阈值才会给出 `verdict=deadlock`（之前是 `stalled`）；
- 场景进程退出前会 `DebugServer::Stop()` / `Doctor::Stop()` 并 `_Exit`，
  避免静态析构顺序问题。
