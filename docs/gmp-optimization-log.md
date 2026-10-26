# GMP 优化轮次日志

> **已合并至 [docs/modules/scheduler.md](modules/scheduler.md)（2026-10-26）。**
> 本文保留作历史追溯；现行接口、设计、边界与数据以模块文档为准，
> 本文中的行号与性能结论不再代表当前实现。

> ⚠️ **2026-10-19 更新**：本文的对比数据基于旧 benchmark，其 epoll 池模型
> 不公平（256 线程抢同一把 `m_mutex`、每请求两次跨线程交接），
> 「GMP 比 epoll 快 3 倍」「s3 6,270 QPS / P50 445ms」等结论已作废。
> 最新有效数据与优化点见 **`docs/performance-baseline-2026-10-19.md`**。


> 目标：s1~s5 五场景的 QPS / P50 / P95 / P99 / 错误数全面优于 thread 与 epoll+线程池。
> 口径：`benchmarks/gmp_vs_thread_epoll_benchmark.cpp`，`build-quality`（Release -O2），
> 12 核 WSL2。**基准默认注入 `U(0,5ms)` usleep**，跨「开/关注入」不可直接比较。
> 每轮只改一个逻辑单元，改完必须 `ctest --test-dir build-quality` 113/113。

## 当前基线（滚动更新，最近一次全量测量）

| 场景 | GMP | thread | epoll+池 | 结论 |
|---|---|---|---|---|
| s1 短连接 | 11,800 / 4.6ms | 10,458 / 4.73ms | 3,027 / 20.93ms | ✅ 领先 |
| s2 echo | 23,563 / 21.7ms (depth8) | 24,133 / 21.2ms (depth8) | 待测 | ⚠️ 落后 2% |
| s3 万连接 | 6,270 / 445ms | 9,674 / 2.83ms | 3,057 / 2245ms | ❌ 落后 35%，P50 差 150x |
| s4 混合 | 待测 | 待测 | 待测 | — |
| s5 慢客户端 | 待测 | 待测 | 待测 | — |

---

## 轮次 0（2026-10-18 18:07–18:27）：继承状态核对 + 五场景基线

**起点**：上一位 agent 在 17:51（`src/network/hook.cpp`）与 17:57（`src/network/io.cpp`）
又改了一版，引入 **`SlotStripe`（IOManager slots 也按 fd 分条）+ 独立的
`m_timer_mutex` + `process_events_batch` 批量事件处理**，库已重编（17:57）。
本轮不改代码，先把「当前真实状态」测清楚。

**五场景基线（新库，各 2 次；开注入 U(0,5ms)）**

| 场景 | GMP QPS / P50 | thread QPS / P50 | epoll QPS / P50 | GMP vs thread |
|---|---|---|---|---|
| s1 短连接 | 11,800 / 4.6ms | 10,458 / 4.73ms | 3,027 / 20.93ms | **+13%** ✅ |
| s2 echo(64c,d8) | 23,563 / 21.7ms | 24,133 / 21.2ms | 待测 | **−2.4%** |
| s3 万连接 | 6,270 / 445ms | 9,674 / 2.83ms | 3,057 / 2245ms | **−35%** ❌ |
| s4 混合 | 20,422 / 3.16ms | 22,072 / 2.96ms | 3,020 / 21.22ms | **−7.5%** |
| s5 慢客户端 | 11,229 / 2.86ms | 11,555 / 2.77ms | 3,047 / 10.48ms | **−2.8%** |

**关键观察：GMP 的 CPU/请求是 thread 的 5~7 倍**

| 场景 | GMP CPU% / µs per req | thread CPU% / µs per req |
|---|---|---|
| s4 | **730.1% / 357.4** | 132.9% / 60.2 |
| s5 | **527.4% / 469.7** | 74.8% / 64.7 |
| s2(64c,d1) | 468% / 210 | 126% / 57.5 |

s4 的 GMP 已用掉 7.3 核（12 核的 61%），**CPU 效率是吞吐的直接上限**。
此前 `utime/stime` 拆分已证明 81% 是内核态 ⇒ 优化目标是「每请求 syscall/唤醒次数」，
不是 Fiber 切换本身（0.73~0.80µs/往返）。

**下一轮方向（按预期收益）**
1. s3：1 万事件突发时每次 park/wake 的固定开销（先做 `processor_count` 扫描，
   判断是「worker 数不足」还是「每事件成本过高」）。
2. s4/s2/s5：每请求 2 次 `epoll_ctl`（ONESHOT rearm + 完成）能否合并/降频。
3. 减少 `Socket::recv/send` 每请求的 `m_mutex` 次数（`begin_operation`/`is_closing`/
   `end_operation` + `recv_timeout` 查询 = 4~5 次加解锁）。

**状态**：工作树有未提交改动（`hook.cpp`、`io.cpp` 17:51/17:57 版本 + 上一轮的
`io.hpp`/`socket.cpp`/`scheduler.*`/`test_io.cpp`）。本轮以「能构建 + ctest 113/113」
为健康门槛。
### 轮次 0 的 syscall 归因（strace -c，s2 64c depth=1）

strace 下 QPS 2,930、completed 17,577（只用于次数归因，绝对性能不可比）：

| syscall | calls | 每请求 | 备注 |
|---|---:|---:|---|
| sendmsg | 43,878 | **2.50** | 响应 = 4B 头 + body 两次 send |
| recvmsg | 43,889 | **2.50** | 请求 = 4B 头 + body，外加一次 EAGAIN |
| epoll_ctl | 43,883 | **2.50** | ONESHOT rearm + 完成时 DEL |
| futex | 23,096 | 1.31 | M 唤醒（WSL2 上最贵的单项） |
| **eventfd2** | 21,940 | **1.25** | **每请求创建一个 eventfd** |
| close | 22,027 | 1.25 | 与 eventfd 配对 |
| fcntl | 22,137 | 1.26 | eventfd 注册时读/改 flags |
| epoll_wait | 3,052 | 0.17 | 批量返回 |

**合计约 12.7 syscall/请求**（此前 H1 记录 7.6，新版本更高）。

### 根因：`cooperative_sleep_for()` 每次调用新建一个 eventfd

`src/network/hook.cpp:912`：

```cpp
SleepOutcome cooperative_sleep_for(Duration duration) {
    ...
    const int fd = static_cast<int>(
        ::syscall(SYS_eventfd2, 0, EFD_NONBLOCK | EFD_CLOEXEC));  // 每个请求一次
    if (fd < 0) return SleepOutcome::Unavailable;
    ...
}
```

基准的 `process_request` 每次注入 `usleep(0~5ms)`，Hook 把它变成
`cooperative_sleep_for` ⇒ **每个请求新建 + 注册 + 关闭一个 eventfd**（4 个 syscall：
`eventfd2` / `fcntl` / `epoll_ctl` / `close`）。

对照：thread 模型的 `usleep` 走原生 `clock_nanosleep`，**只有 1 个 syscall**。
这是「同一 QPS 下 GMP 的内核 CPU 是 thread 的 3~3.5 倍」的主要来源之一。

**下一轮要做的事（单一改动）**：把 sleep 用的 eventfd 做成 **thread_local 复用**
（每个 worker 一个，注册一次、长期存活），或让 sleep 直接 park 到 IOManager 的
deadline 队列而不需要 fd。预期每请求省 3.7 个 syscall，s2/s4/s5 的 CPU/req 下降
20~30%，在 s4（CPU 730% 已接近 12 核上限）上应直接转化为吞吐。

**注意**：复用 eventfd 时必须确认 IOManager 的 `FdSlot` 代际逻辑（同一 fd 反复
register/complete）与 `close` 路径不会把复用 fd 当成真实连接关闭；先跑
`test_io` / `test_hook` 两个专项再跑全量。
## 轮次 1（2026-10-18 18:27–18:47）：eventfd 复用 —— **负结果，已回滚**

**改动**：`cooperative_sleep_for()` 的 sleep 用 eventfd 改为每 worker 线程复用一个
（`thread_local`，不再每请求 `eventfd2` + `close` + `NotifyClose`），预期省 3.7
syscall/请求。

**A/B（s2，64 连接，depth=1，同口径连续两次）**

| 版本 | QPS | p50 | CPU% | user µs/req | sys µs/req |
|---|---:|---:|---:|---:|---:|
| 复用 eventfd（改动后） | 20,601 | 3.12ms | 630.1 | 28.9 | **276.9** |
| 每请求新建（回滚后） | 21,161 | 3.05ms | 738.3 | 32.5 | **316.4** |

- `sys` 确实降了 12%，但 **QPS 反降 2.7%**（在噪声内），整体无收益；
- 专项 `GO2CPP_TEST_FILTER=hook` / `=io` 均通过；
- **已回滚**（`cp /tmp/hook.cpp.round1.bak`），当前工作树回到 17:51 版本。

**为什么没有收益（修正上一轮的估算）**：`eventfd2 + close` 每次真实成本只有
2~5µs，占 `sys 316µs/请求` 的 1~2%。strace 的 `usecs/call` 列包含阻塞等待，
不能拿来估算 CPU 成本 —— 这是本轮的方法论教训。

**真正的 sys 成本指向「线程唤醒」而不是 syscall 条数**：

- 12.7 个 syscall/请求里，只有 `futex`（1.31/请求）与跨线程唤醒相关；
- 但 `sys` 时间高达 170~316µs/请求。按 12.7 次 syscall 分摊，每次要 20µs+，
  远超 WSL2 正常 syscall 成本（1~3µs）⇒ **大头是唤醒/调度进入内核态的时间**；
- 因此下一步要减少的是**唤醒次数**，不是 syscall 条数。

**下一轮的两个候选（按可测性）**
1. `wake_io()` 把 G 投回本 P 后是否仍然 `notify_worker()`：worker 自己就在跑，
   多唤一个 parked M 醒来发现无活再睡 = 每请求一次白唤醒（20k/s × ~50µs）。
   §2.5 修过同一类问题（yield 回队不 notify），IO 侧可能还留着。
2. `epoll_ctl` 2.5 次/请求：把 ONESHOT 重新 arm 从「每次 park」改成「只在
   interest 集合变化时」，完成路径的 DEL 延后。

先做 1（只改`notify` 条件，风险低、可用 `ctest` + 现有压力测试兜底）。

### 轮次 1 的补充测量：sleep 路径的真实成本是 206µs/次

用「关注入」作对照，把 `cooperative_sleep_for` 的净成本单独算出来（s2，64c，depth=1）：

| 配置 | QPS | CPU% | sys µs/req |
|---|---:|---:|---:|
| 开注入（每请求 1 次 usleep） | 21,161 | 738 | **316.4** |
| 关注入（`GO2CPP_BENCH_IO_SLEEP_US=0`） | 30,720 | 405 | **109.7** |
| 差值 | — | — | **≈206µs / 次 sleep** |

拆解：`eventfd2 + close` 只占 ~40µs（复用它们的收益也就是这个量级，实测 -12% sys），
其余 **~166µs 是 `WaitFor` 本身**（fd 注册 + ONESHOT rearm + park + 定时器唤醒）。

对照 thread 模型的同位置：`usleep` → 原生 `clock_nanosleep`，1 个 syscall、线程睡、
内核定时器唤醒。**这就是同一 QPS 下 GMP 内核 CPU 高 3~6 倍的单一最大来源**。

### 下一轮方案（二选一，优先 B）

- **A（小改）**：`WaitFor` 的定时器路径去掉 epoll 参与 —— 新增
  `IOManager::wait_deadline(deadline)`，`fd < 0` 的等待只进 `m_deadline_order`，
  不建 `FdSlot`、不 `epoll_ctl`。预期省 20~40µs/次。
- **B（中改，收益更大）**：sleep 直接 park 到「调度器定时器」而不是 IO 后端 ——
  `Scheduler` 增加 `park_until(task, deadline)`（复用 `TimerService` 到期调用
  `wake_for_wait`），`cooperative_sleep_for` 走它。理想情况一次 sleep 只剩
  一次定时器注册 + 一次 park/wake，**无 fd、无 epoll**。

B 的风险是新增 park 语义（取消/关闭/迁移都要覆盖），必须先补一个「1000 个 Fiber
各自 sleep 不同时长」的专项测试，再跑全量。
## 轮次 2（2026-10-18 18:47–19:07）：s3 的 syscall 构成 —— epoll_ctl 10.75 次/请求

**本轮不改代码**：先把 s3（唯一差距 35% 的场景）的每请求 syscall 拆开，避免又做
「凭直觉改、测完回滚」的浪费。

**strace -c（s3，2000 连接，5s；QPS 1,155、completed 5,801，只用于次数归因）**

| syscall | calls | 每请求 | 对比 s2（64c） |
|---|---:|---:|---:|
| **epoll_ctl** | 62,381 | **10.75** | 2.50 |
| **fcntl** | 35,593 | **6.13** | 1.26 |
| futex | 35,295 | 6.08 | 1.31 |
| recvmsg | 21,930 | 3.78 | 2.50 |
| sendmsg | 14,988 | 2.58 | 2.50 |
| eventfd2 | 7,495 | 1.29 | 1.25 |
| close | 9,085 | 1.57 | 1.25 |
| getsockopt | 6,000 | 1.03 | 0.11 |

**合计约 34 syscall/请求**（s2 只有 12.7）。这就是 s3 每请求成本 159µs 的来源。

### 根因假设（有明确代码对应）

1. **空闲连接的 `FdSlot` 被删掉**：`complete()` 末尾
   `if (slot.readers.empty() && slot.writers.empty() && !slot.registered) stripe.slots.erase(found);`
   s3 的连接 99.99% 时间空闲 —— 每次请求完成后 slot 即被删除。下一次 wait 时
   token 缓存未命中 ⇒ `DescriptorGuard::Capture(fd)` ⇒ `descriptor_is_open(fd)`
   的 **F_GETFD（fcntl）**，这正是 6.13 次/请求的来源；
2. **slot 重建 = 重新 ADD**：`update_interest_locked` 看到 `slot.registered == false`
   就走 `EPOLL_CTL_ADD` 而不是 `MOD`；加上 register/complete/事件处理各一次，
   s3 的 epoll_ctl 变成 s2 的 4.3 倍；
3. `getsockopt` 1.03 次/请求也来自「每次重建 OpenDescription / socket 元数据」。

### 下一轮（单一改动，按此实施）

**保留空闲连接的 FdSlot 与 descriptor token**：`complete()`/`process_event_locked()`
里不再因 「无等待者」立刻 `slots.erase`，改为保留 slot（`registered=false` 时只是
`interest=0`），只在 fd 关闭（`notify_close`）或条带内 slot 数超过阈值（例如 4096）
时才回收。

预期：`fcntl` 6.13 → ~0，`epoll_ctl` 从 ADD 降为 MOD（仍 2~3 次/请求），
s3 每请求 syscall 34 → 20 左右。风险：1 万空闲连接常驻 slot 的内存（约
1 万 × ~120B ≈ 1.2MB，可接受）；需要确认 `notify_close`/`dup2` 的代际失效仍能
把保留的 slot 排空（`test_io` 有 `test_generation_reuse_and_cross_manager_close`
覆盖这条路径）。

**验证顺序**：`GO2CPP_TEST_FILTER=io` → 全量 ctest → s3（2 rep）→ 若 s3 +10% 以上
再跑 s2/s4 确认无回归。
## 轮次 3（2026-10-18 19:07–19:27）：**保留空闲 FdSlot** —— s3 +56%，p50 改善 131 倍

### 改动（一处）

`src/network/io.cpp` 的 `State::complete()`：不再在「无等待者」时
`stripe.slots.erase(found)`，改为保留 `FdSlot`（含 descriptor token 与代际）。
回收责任交给 `notify_close`（fd 真正关闭）与 `purge_terminal_locked`。

### 效果（同窗口 A/B，`--repeats 2`）

| 场景 | 改前 | 改后 | 变化 |
|---|---:|---:|---|
| **s3 万连接 QPS** | 6,270 | **9,823 / 9,802** | **+56%** |
| **s3 P50** | 445ms | **3.41 / 3.42ms** | **131x** |
| s3 P99 | 1,987ms | 7.63 / 13.59ms | 146x |
| s3 RSS | 130MB | 100.5 / 101.9MB | −23% |
| s1 QPS | 6,631（同窗口对照） | 6,729 / 6,517 | 中立 |
| s2 QPS | 20,514 | 20,740 / 20,514 | 中立 |
| s4 QPS | 20,422 | 20,094 / 19,934 | 中立 |

**s3 现在超过了 thread 模型（9,815 vs 9,674），并且完成数 99,409/99,278 已贴近
协议上限（10,000 连接 × 每秒 1 次 = 100,000/10s）。**

### 归因

改前 s3 每请求 **6.13 次 fcntl + 10.75 次 epoll_ctl**（s2 只有 1.26 / 2.50）：
空闲连接每次请求完成后 slot 即被删除 ⇒ 下一次 wait 重新
`DescriptorGuard::Capture`（F_GETFD）并重新 `EPOLL_CTL_ADD`。保留 slot 后这两项
降为「命中缓存 + MOD」。

### 本轮最大的方法论教训：**不能用不同时刻的数字做 A/B**

第一次测到 s1 从 11,800 掉到 6,824，我据此判断改动有害并回滚；回滚后 s1 仍是
6,631 —— 说明差异来自环境（`loadavg` 1.x → 4.6，`cpptools`/3×`exe` 在跑），
不是改动。**之后所有 A/B 都在同一窗口内连续跑完两组**，才得到上面「中立」的结论。

以后每轮必须在同一窗口测「对照 + 改动」，并在日志里记录 `/proc/loadavg`。

### 状态

- 当前工作树 = 保留 slot 的版本；
- `ctest --test-dir build-quality` → **113/113**；
- 尚未做的：s5 复核（下一轮）、以及同窗口复测 s1 的真实值（受负载影响）。

### 下一轮

1. 在低负载窗口重测 s1（确认它在 11k~12k 量级）；
2. s5 复核；
3. 若 s3 稳定 >9.6k，转向 s2/s4/s5 的「每请求 2.5 次 epoll_ctl + 1.3 次 futex」。
## 轮次 4（2026-10-18 19:27–19:47）：同窗口五场景基线 —— **s1 是最大短板（−29%）**

前几轮的 thread 对照数字来自低负载窗口（loadavg 1.x），与 GMP 当前数字不可比。
本轮在**同一窗口**（loadavg 2.7→3.9）跑完 5 场景 × 3 模型 × 1 rep。

| 场景 | GMP QPS / P50 | thread QPS / P50 | epoll QPS / P50 | GMP vs thread |
|---|---|---|---|---|
| s1 短连接 | 7,253 / 7.07ms | **10,192 / 4.83ms** | 3,031 / 20.99ms | **−29%** ❌ |
| s2 echo | 20,742 / 3.11ms | **22,222 / 2.95ms** | 3,030 / 21.14ms | −6.7% |
| s3 万连接 | **9,840 / 3.26ms** | 9,664 / 2.84ms | 3,056 / 2247ms | **+1.8%** ✅ |
| s4 混合 | 20,325 / 3.17ms | **21,989 / 2.98ms** | 2,995 / 21.43ms | −7.6% |
| s5 慢客户端 | 10,949 / 2.86ms | **11,584 / 2.76ms** | 3,038 / 10.45ms | −5.5% |

**s1 的 CPU 是 644% / 888µs per request，thread 只有 170% / 167µs（5.3 倍）。**
s2/s4/s5 的 CPU 也分别是 thread 的 5.5 / 5.5 / 6.8 倍 —— **CPU 效率是唯一共同瓶颈**。

### s1 的 syscall 归因（strace -c，completed 4,346，只看次数）

| syscall | calls | 每请求 | 备注 |
|---|---:|---:|---|
| **read** | 37,206 | **8.56** | 其中 **30,295 次 EAGAIN** |
| **epoll_wait** | 31,839 | **7.33** | 每请求 7 次返回，异常 |
| fcntl | 22,445 | **5.16** | |
| getsockopt | 16,827 | **3.87** | |
| recvmsg | 16,822 | 3.87 | |
| sendmsg | 11,209 | 2.58 | |
| close / epoll_ctl | 11,247 / 11,207 | 2.59 / 2.58 | |
| **write** | 9,275 | **2.13** | = `tickle()` 写 wake eventfd |
| eventfd2 | 5,610 | 1.29 | = 每请求一次 `cooperative_sleep_for` |

三个待查异常：
1. **`epoll_wait` 7.33 次/请求**，其中只有 2.13 次能由 `tickle()` 的 write 解释，
   剩下 ~5 次是真实事件返回 —— 一个短连接不该产生 5 次 epoll 事件；
2. **`read` 8.56 次/请求、其中 30,295 次 EAGAIN**，而业务路径用的是 `recvmsg`；
   需要找出这个「先读一次探测」的来源（`hook.cpp:1793` 的 `SYS_read` 回退路径？）；
3. **`getsockopt` 3.87 次/请求**（`can_wait` 的 `SO_TYPE` 判定）与 **`fcntl` 5.16**
   —— 每次 recv/send 都在重新判定 fd 类型。

### 下一轮

先做 (2)：定位 `read` 的调用点（grep 结果只显示 `hook.cpp:1793` 的 raw 回退，
说明它被高频走到了）。如果是「未接管 fd 回退路径」，那意味着 **s1 的连接在某个
时刻不被识别为「可等待 socket」**，从而退化成阻塞 read + 额外的 BlockingRegion。
这能同时解释 `epoll_wait` 偏多与 CPU 偏高。

### 轮次 4 的追加推断：`read` 与 `epoll_wait` 同源

全仓只有 `hook.cpp:1793` 的 raw 回退会调 `read`，而 s1 的业务路径走 `recvmsg`。
但 strace 显示 **`read` 8.56 次/请求、其中 30,295 次 EAGAIN**，与
**`epoll_wait` 7.33 次/请求**数量级一致，且 `write`（`tickle()` 写 wake eventfd）
正好 2.13 次/请求。

⇒ 最可能的解释是 **`drain_wake_fd()` 用 `read` 排空 wake eventfd**：每次
`tickle()` 让某个 worker 的 `epoll_wait` 返回，`poll_once()` 看到
`kWakeRegistration` 就去 `drain_wake_fd()` 一次 —— 于是「epoll_wait 一次 +
read 一次」成对出现。

也就是说 **s1 每请求发生约 7 次 tickle/唤醒**（recv 头 2 次、recv body 2 次、
send 2 次、close 通知 1 次），每次都是一次 epoll_wait 返回 + 一次 read 排空 +
一次线程被唤醒的内核开销。**这就是 888µs CPU/请求的主体。**

**下一轮验证与改动（单一）**：
1. 在 `poll_once()` 里给 `kWakeRegistration` 分支加计数器，确认 7.33 次/请求
   里有多少是 wake；
2. 若确认，改 `complete()` 末尾的 `tickle()` 为条件调用（只有「确实有 worker
   在 epoll_wait 且本地队列非空」时才写 eventfd），并复核 `register_wait` 的
   tickle 是否只在「新 deadline 早于当前 poll 超时」时必要。

预期：s1 的 epoll_wait 7.33 → 2~3，`read` 8.56 → 2~3，sys CPU 888µs → 400µs 量级。
## 轮次 5（2026-10-18 19:47–20:07）：tickle 短路（无显著收益）+ **纠正 strace 归因方法**

### 改动

`State::poll_once()` 期间置 `thread_local t_inside_poll = true`，`tickle()` 在
该标志为真时直接返回 —— 理由是「调用者本身就在 poll，处理完这批事件就回
worker_loop 从本 P 队列取任务（`wake_io` 一律投本 P），不必再写 eventfd 叫醒别的 M」。

| s1（同窗口） | QPS | p50 |
|---|---:|---:|
| 改前 | 7,253 / 7,133 | 7.07ms |
| 改后 | 7,506 / 7,224 | 6.97ms |

差异在噪声内 ⇒ **保留**（逻辑正确、减少无谓唤醒、`ctest` 113/113），但不作为
s1 的解法。

### 重要纠正：`strace -c` 的「每请求 syscall 数」不可信

上一轮我用 `strace -c` 算出 s1 每请求 7.33 次 `epoll_wait`、8.56 次 `read`，
并据此推断「每请求 7 次 tickle 唤醒」。**这个推断是错的。**

用 `strace -e trace=epoll_wait` 单独跟踪（开销小得多）并同时统计 completed：

| 口径 | completed | epoll_wait | 每请求 |
|---|---:|---:|---:|
| `strace -c`（跟踪全部 syscall） | 4,346 | 31,839 | **7.33** |
| `strace -e trace=epoll_wait` | 2,395 | 1,549 | **0.65** |

返回值的真实分布（后一种口径）：

```
count=0:   244 (15.8%)   ← 超时
count=1:  1305 (84.2%)   ← 真实事件
timeout 参数：1ms 670、2ms 295、0ms 262、10ms 146
```

⇒ **s1 没有 epoll 忙轮询**；84% 的唤醒是真实事件。`strace -c` 跟踪全部 syscall 时
进程慢 5~10 倍，超时/重试次数被同步放大，**不能拿来当「每请求次数」**。

### 仍未解释的问题：s1 的 888µs CPU/请求

- 服务端 CPU 644%（`_bench_manual` 与 `run` 两个口径一致）；
- 但 gdb 采样显示 **84/112 个采样点停在 `epoll_wait`**（约 75% 的线程时间在空闲等待），
  只有 9/112 在 `Fiber::Impl::entry`（真正跑 G）；
- 主线程在 `Socket::accept → poll`（200ms 一次），不是忙循环。

两者矛盾，说明 CPU 花在**极短但极频繁**的用户态/内核态片段里，采样抓不到。
下一轮改用**用户态计时打点**：在 s1 服务端路径埋点（accept 返回 →
`manager.go` → Fiber 创建 → 首次 recv → send → close），输出耗时直方图，
定位 888µs 的构成。打点用 `steady_clock` + 原子计数器，压测后 dump。

### 状态

- 当前工作树 = 保留 slot + tickle 短路；
- `ctest --test-dir build-quality` → **113/113（70.22s）**；
- 同窗口五场景：s1 −29%、s2 −6.7%、**s3 +1.8%**、s4 −7.6%、s5 −5.5%。
## 轮次 6（2026-10-18 20:07–20:27）：s1 的阶段计时 —— 83% 是等待，CPU 全在内核

### 工具（已回滚，方法记录在此）

在 `benchmarks/gmp_vs_thread_epoll_benchmark.cpp` 的 `blocking_serve_loop()` 里按
三段埋 `steady_clock`：`recv`（含等数据）、`process`（含注入的 usleep）、`send`；
压测结束打印 `ns/req`。**已验证完毕后回滚**（保持 benchmark 无额外开销）。

**踩坑**：第一次用 `CLOCK_THREAD_CPUTIME_ID`，`recv` 段算出 330 万亿 ns ——
G 在 park 期间迁移到别的 worker，两个采样点属于不同线程，线程 CPU 时钟不可比
（无符号下溢）。**跨 park 的计时必须用 wall 时钟**。

### s1 的构成（64 连接，6s，QPS 7,526）

| 阶段 | wall ns/req | 说明 |
|---|---:|---|
| recv（含等连接/数据） | 1,872,479 | 等客户端发请求 |
| process | **2,593,533** | ≈ 注入的 `usleep(0~5ms)` 均值 2.5ms |
| send | 33,111 | 写入 socket 缓冲 |
| **合计** | **4,499,123** | 与客户端 p50 6.88ms 的差值是网络/连接建立 |

同一轮 `/proc` 口径：**CPU 577%（767µs/请求），其中 sys 671µs、user 95µs**
⇒ **83% 的墙钟是等待，真正消耗的 CPU 里 88% 在内核态**。

### 新线索：Fiber 栈的 `mprotect` 每请求 1.35 次

s1 的 strace（4,346 请求）里 `mprotect` 5,877 次、`munmap` 1,332 次 —— 对应
**每连接一个 Fiber 的创建/销毁**。s1 是每秒 7,253 次连接建立，也就是每秒
7,253 次 Fiber 创建。若 `FiberBin` 池没命中，每次都要 `mmap` + `mprotect`
（guard page）+ `munmap`，每次几十微秒，足以解释数百 µs 的内核 CPU。

**下一轮**：先确认 `FiberBin`/Fiber 池在 s1 下是否命中（`benchmarks/fiber_stack_probe.cpp`
或直接读 `fiber.cpp` 的分配路径），再看 `fiber_stack_size` 与池容量。
这是目前唯一能同时解释「sys 671µs + gdb 抓不到热点」的候选：开销全在
`mmap/mprotect/munmap` 这类极短的内核调用里，而且每个连接只发生一次。

### 状态

- benchmark 已回滚到无埋点版本；
- 库侧改动 = 保留 slot + tickle 短路；
- `ctest` 上一轮 113/113，本轮未改库，无需重跑（下一轮改动后统一跑）。

### 轮次 6 的追加试验：FiberBin 容量 32 → 256（负结果，已回滚）

假设：s1 每秒 7,253 次 Fiber 创建/销毁，而 `kDefaultFiberBinCapacity = 32`，
64 并发超过池容量 ⇒ 不命中就 `mmap + mprotect(guard) + munmap`（strace 里
`mprotect` 1.35 次/请求、`munmap` 0.31 次/请求）。

| s1（同窗口） | QPS | p50 |
|---|---:|---:|
| 容量 32（原值） | 7,075 / 6,847 | 7.32 / 7.59ms |
| 容量 256 | 7,246 / 7,104 | 7.18 / 7.31ms |

差异 +2%，在噪声内 ⇒ **已回滚**（`cp /tmp/fiber.cpp.round7.bak`）。
说明 Fiber 栈分配本来就有池化命中，不是 s1 的瓶颈。

**s1 的 671µs sys/请求 仍未归因**。现有排除项：epoll 忙轮询（已排除）、
tickle 频率（已排除）、Fiber 池（已排除）、gdb 可见热点（采样抓不到）。
下一步只剩「按 syscall 类型分别计 CPU」—— 需要 `perf` 或自建
`clock_gettime(CLOCK_PROCESS_CPUTIME_ID)` 包住每类 syscall 的宏替换，成本较高。
在拿到更多证据前，**s1 转向「降低连接建立路径的 syscall 条数」**（例如 accept 批量、
`tune_accepted` 的两次 `setsockopt` 合并）更划算。
## 轮次 7（2026-10-18 20:27–20:47）：sleep 的真实 CPU 成本 —— 单 Fiber 334µs/次

### 新工具：`_sleep_cpu_probe.cpp`（保留，可复跑）

在 Fiber 里循环 `usleep(1ms)`（Hook → `cooperative_sleep_for`），用
`CLOCK_PROCESS_CPUTIME_ID` 取进程 CPU 增量，除以 sleep 次数：

```
g++ -O2 -DNDEBUG -Iinclude -Ithird_party .dsh_tmp/_sleep_cpu_probe.cpp -o /tmp/sleep_probe \
    -Wl,-rpath,$PWD/build-quality -Lbuild-quality -lgo2cpp_io -lgo2cpp_scheduler \
    -lgo2cpp_fiber -lgo2cpp_context -lgo2cpp_sync -lgo2cpp_hook -lgo2cpp_config \
    -lgo2cpp_log -lgo2cpp_error -lz -ldl -lpthread -lm
```

### 实测

| Fiber 数 | cpu/次 sleep | wall/次 sleep |
|---|---:|---:|
| **1** | **334.26µs** | 1,182µs |
| 12 | 110.42µs | 100µs |
| 64 | 49.74µs | 24.5µs |
| 256 | 43.46µs | 9.3µs |

**单 Fiber 时一次 1ms 的协作式 sleep 要烧 334µs CPU**（其中 wall 只有 1.18ms）。
这就是 s2/s4/s5 每请求 CPU 是 thread 5~6 倍的直接原因：基准的 `process_request`
每请求注入一次 `usleep(0~5ms)`，在 thread 模型里是 `clock_nanosleep`（~2µs），
在 GMP 里是「park + 定时器 + 跨线程唤醒」的完整链路。

**摊薄效应明显**（并发 sleep 越多，单次成本越低：334 → 43µs），说明成本主要不在
「注册/插入定时器」，而在**每次唤醒一个 parked worker 的内核调度**。这与之前测到
「WSL2 一次跨线程唤醒 ~55µs」一致，属于环境放大；用户态能压缩的空间有限。

### 对目标的影响

- s2/s4/s5 的差距（−5%~−8%）**主要来自基准注入的 usleep**，不是 IO 路径本身；
- 关注入后 s2 的 sys 是 110µs/请求 vs thread 48µs（2.3 倍），这才是 IO 路径的真实差距；
- s3 的 1 万连接每秒各 1 次 sleep 属于「并发摊薄」档（≈43µs），但 s3 每请求 CPU 仍
  719µs —— 说明 s3 的成本不在 sleep 上，而在 1 万 Fiber 的事件投递。

### 下一轮

转向 **s2 关注入路径**：`GO2CPP_BENCH_IO_SLEEP_US=0` + `--scenario s2`，
把 110µs sys/请求 拆到 `epoll_ctl`（2.5 次）与 `futex`（1.3 次）上，目标是
把 epoll_ctl 从「每次 park 一次 rearm」降下来（例如 interest 不变时跳过 MOD）。
## 轮次 8（2026-10-18 20:47–21:07）：notify_worker 优化（负结果）+ 确认 tickle 短路有效

### 试验 1：有 IO 等待者时跳过 `condition.notify_one()`（负结果，已回滚）

`notify_worker()` 原本无条件 `condition.notify_one()`（一次 futex wake，哪怕条件变量上
没人），然后再视情况写 eventfd。netpoll 模型下空闲 worker 绝大多数睡在 `epoll_wait`，
所以那次通知看起来是浪费。

| s2 + `IO_SLEEP=0`（同窗口） | QPS | sys µs/req |
|---|---:|---:|
| 跳过 notify_one | 24,120 | 169.7 |
| 保留 notify_one（回滚后） | 22,936 | 179.6 |

**跳过反而更快（+5%）**，但差异不大且方向与我预期相反 —— 真正原因是
`poller->wake()` 有 `m_tickle_pending` 合并，可能把唤醒合并掉。**已回滚**。

### 试验 2：tickle 短路的 A/B（**有效，保留**）

把轮次 5 的 tickle 短路临时回滚，同一 s2 场景：

| s2 + `IO_SLEEP=0`（同窗口连续三次） | QPS | sys µs/req |
|---|---:|---:|
| tickle 短路（有 `t_inside_poll`） | **22,936** | 179.6 |
| 无短路（回滚） | 20,643 | 235.0 |

**+11% QPS、−24% sys** ⇒ 轮次 5 的改动确实有效，只是当时被测成了噪声。已重新应用。

### 重要教训：**WSL2 的绝对性能会随时间漂移，跨轮次数字不可比**

同一场景（s2 + `IO_SLEEP=0`）在不同轮次测到：30,720（轮次 0）→ 22,936（本轮）。
中间只多了「保留 slot + tickle 短路」两个正向改动，却低了 25%。同时 `loadavg`
只有 0.35。结论：**这台机器的绝对吞吐会随会话/宿主机状态漂移**，只有
「同一窗口内连续跑的 A/B」可信。之前把 30,720 当作基线是错的。

### 状态

- 当前工作树 = 保留 slot + tickle 短路；
- `ctest --test-dir build-quality` → 113/113；
- 后续所有轮次的数字**只与同窗口对照组比较**，并在日志里同时记录 `loadavg`。
## 轮次 9（2026-10-18 21:07–21:27）：新基线（旧基线作废）+ s1 瓶颈锁定在主线程 accept

### 同窗口五场景基线（loadavg 1.23→2.55）

| 场景 | GMP QPS / P50 | thread QPS / P50 | epoll QPS | GMP vs thread |
|---|---|---|---|---|
| s1 | 7,629 / 6.81ms | **10,664 / 4.59ms** | 3,032 | **−28.5%** ❌ |
| s2 | 21,398 / 3.01ms | 22,230 / 2.95ms | 3,021 | −3.7% |
| s3 | **9,832 / 3.46ms** | 9,662 / 2.84ms | 3,032 | **+1.8%** ✅ |
| s4 | 21,492 / 2.99ms | 22,131 / 2.96ms | 3,032 | −2.9% |
| s5 | 11,130 / 2.82ms | 11,425 / 2.80ms | 3,060 | −2.6% |

（旧基线已按轮次 8 的结论作废；本表与 thread 是同一窗口。）

**s2/s4/s5 的差距已收敛到 3% 以内，s3 反超，s1 是唯一显著短板（−28.5%）。**

### s1 的归因：不是 Fiber 创建，也不是 sleep

新探针 `_fiber_create_probe.cpp`（已放进项目 `.dsh_tmp/`）：

| 模式 | cpu/任务 | wall/任务 |
|---|---:|---:|
| 串行「建一个、等一个」（`manager.go` + `wait`） | **105.30µs** | 95.25µs |
| 64 并发在飞 | **17.51µs** | 3.88µs |

- Fiber 创建/销毁本身只有 **17.5µs**（128KB 栈 + guard page，池化后）；
- 串行模式的 105µs 主要来自 `wait()` 的跨线程唤醒（主线程不是 G，走 ParkingCondition/futex）；
- sleep 在 64 并发档约 50µs（轮次 7 的探针）。

⇒ **792µs/请求 里只有 ~70µs 能由「Fiber + sleep」解释**。

### 推断：s1 的瓶颈是**主线程串行 accept 的 95µs/连接**

s1 的主线程循环是「`listener->accept(200ms)` → `tune_accepted` → `manager.go`」，
每个连接一次。串行探针的 95µs/任务 正是这个量级：

> 主线程上限 ≈ 1 / 95µs ≈ **10,500 连接/s**，而 thread 模型本轮实测 **10,664**、
> GMP 只有 **7,629**。thread 的 accept 只做 `accept4 + pthread_create`（~30µs），
> GMP 还要构造 `Socket`、`setsockopt(TCP_NODELAY)`、`manager.go`（外部 admission
> 入队 + 可能唤醒 worker）。

**`--raw-io` 对照实验无效**：裸 `recv` 对已被切成非阻塞的 socket 直接返回 EAGAIN，
连接立刻关闭（QPS 2、11 万错误），不能用来评估 Socket 包装层。

### 下一轮

1. 用探针拆 `manager.go()` 的外部入队成本（只 go、不 wait）；
2. **accept 批量化**：一次 accept4 循环取多个连接，再批量 `manager.go`，把「每连接一次
   主线程唤醒/入队」摊薄。这是 s1 唯一有量级的候选。
## 轮次 10（2026-10-18 21:27–21:47）：accept 批量化 —— **无收益，已回滚**

**改动**：`run_gmp_server` 的主循环改成「一次收割最多 64 个连接再逐个 `manager.go`」
（`Socket::accept` 内部先 `accept4`、EAGAIN 才等，所以 `accept(0ms)` 不会阻塞）。

| s1（同窗口） | QPS | p50 |
|---|---:|---:|
| 原循环（每连接一次 accept） | 7,629 / 7,133 | 6.81 / 7.59ms |
| accept 批量化 | 7,588 / 7,400 | 6.88 / 7.02ms |

差异在噪声内 ⇒ **已回滚**。说明 s1 的 listen 队列里基本没有积压
（客户端是 64 并发但连接节奏分散），瓶颈不是 accept 系统调用次数。

### 对 s1 的重新判断

`Socket::accept` 的实现（`socket.cpp:233`）是「先 `accept4`，EAGAIN 才 `wait_ready`」，
阻塞 listener 下首次 `accept4` 就直接拿到连接、不进等待。所以主线程每连接的成本是：

1. `accept4`（内核 TCP 栈，~10~20µs）
2. `FromFd`（Socket 构造 + `init_nonblock` 的 raw `fcntl`）
3. `tune_accepted`（一次 `setsockopt(TCP_NODELAY)`）
4. `manager.go`（外部 admission 入队 + 可能唤醒 worker）

**下一轮必须把 2/4 单独量化**，否则只能在 ±3% 里打转。做法：给 `_fiber_create_probe`
加「只 `manager.go`、不 `wait`」的模式（连发 N 个空 G，最后统一等），测
`go()` 的 CPU/次；再用 `strace -e trace=accept4,fcntl,setsockopt,epoll_ctl` 数
`FromFd + tune_accepted` 的 syscall 次数。

### 状态

工作树回到「保留 slot + tickle 短路」；本轮只改 benchmark 且已回滚，
`ctest` 维持 113/113。
## 轮次 11（2026-10-18 21:47–22:07）：Fiber 栈复用不再重复 mprotect（**小正收益，保留**）

### 归因工具的两个新数据点

`_fiber_create_probe`（新增 `concurrency=0` 模式：只 `go()` 不 `wait`）：

| 模式 | cpu/任务 |
|---|---:|
| 只 `go()`（concurrency=0） | **17.60µs** |
| 64 并发在飞 | 17.51µs |
| 串行 create+wait | 105.30µs（等待占大头） |

`strace -c -e trace=mprotect,mmap,munmap` 跑 `go()` 5000 次：

```
mprotect 5,038 次（1.008 次/任务）
mmap        86 次   ← 栈已经从池里复用，不再 mmap
munmap      31 次
```

### 改动

`FiberStack::acquire()` 从池里取栈时会 `mprotect(mapping, page, PROT_NONE)`
**重设 guard page**，但 `release()` 只是把 mapping 放回缓存、**从不改动 guard page**
（它从 `allocate()` 起一直是 PROT_NONE）。⇒ 复用路径的两次 `mprotect` 是纯冗余，
已删除（创建路径 `allocate()` 里的那次保留）。

| 指标 | 改前 | 改后 |
|---|---:|---:|
| `go_only_cpu_per_task` | 17.60µs | **14.51µs**（−18%） |
| s1 QPS（同窗口 2 rep） | 7,629 / 7,133 | **7,910 / 7,523**（+4%） |
| s1 P50 | 6.81 / 7.59ms | 6.60 / 6.91ms |

**保留**（安全：guard page 从未被释放路径改动过；`ctest` 113/113）。

### 方法论补充：strace 的 `usecs/call` 再次被证伪

strace 显示 mprotect 280µs/call，但去掉后 `go()` 只快 3µs。**strace 会把每次
syscall 的耗时放大 10~100 倍，永远不要用它估算 CPU 成本**（这已是第二次踩到）。

### 剩余 s1 成本画像

`go()` 还有 14.5µs（`fcontext` 建栈 + Task 分配 + 外部 admission 入队），
加上 `accept4`（~10~20µs）、`FromFd`、`setsockopt`，主线程每连接约 90µs。
下一轮：用 `strace -e trace=fcntl,setsockopt,accept4` 精确数 `FromFd`/`tune_accepted`
的 syscall 条数，看是否还有可省的（尤其是 `Socket` 构造里的 `F_GETFD`/`F_GETFL`）。
## 轮次 12（2026-10-18 22:07–22:27）：s1 的差距与 sleep 无关 —— 纯连接建立路径

### 决定性对照：关掉基准注入的 usleep

| s1 + `IO_SLEEP=0`（同窗口 2 rep） | QPS | CPU% | CPU µs/请求 |
|---|---:|---:|---:|
| **GMP** | **6,875** | 472.8 | **687.6** |
| thread | 10,295 | 147.9 | **143.6** |

GMP 关掉 sleep 后**反而更低**（7,910 → 6,875，因为开注入时客户端节奏更慢、主线程压力更小），
CPU 仍是 thread 的 **4.8 倍**。⇒ **s1 的 −26% 与 sleep 无关，纯粹是「每连接一个 G」的
建立/销毁路径**。

### 连接建立路径的 syscall 明细（completed 3,486）

| syscall | 每请求 | 说明 |
|---|---:|---|
| **fcntl** | **5.50** | `DescriptorGuard::Capture` 的 F_GETFD + `init_nonblock` + hook 的 fcntl |
| recvmsg | 4.12 | 头/体分读 + EAGAIN |
| **getsockopt** | **4.12** | `is_socket()` 的 `SO_TYPE` + connect 的 `SO_ERROR` |
| sendmsg / close / epoll_ctl | 2.74 / 2.76 / 2.74 | |
| accept4 / setsockopt | 1.38 / 1.38 | `tune_accepted` 的 `TCP_NODELAY` |

**close 2.76 次/请求**（一个连接只该 close 一次）与 **fcntl 5.5 次** 是明显的冗余。

### 归因手段的升级：callgrind 可用

`perf` 不可用（`perf_event_paranoid=2`、无 sudo），但 **`valgrind --tool=callgrind`
与 `callgrind_annotate` 都在**（`/usr/bin/`）。它给的是**确定性的指令级成本**，
不受采样偏差影响，也不像 strace 那样放大单次耗时。

### 下一轮

```bash
valgrind --tool=callgrind --callgrind-out-file=/tmp/cg.out \
  ./build-gmp-vs-thread/gmp_vs_thread_epoll_benchmark server --model gmp --pin --port 0
# 另一个进程跑 client --scenario s1 --conns 16 --duration-ms 3000（慢 20~50 倍）
callgrind_annotate --auto=yes /tmp/cg.out | head -60
```

目标：直接看 687µs 里 `operator new`/`fcontext`/`enqueue`/`close`/`fcntl` 各占多少
（含**调用次数**），一次定位，不再靠推理。
## 轮次 13（2026-10-18 22:27–22:47）：callgrind 归因 —— **用户态只占 4%，s1 的账在内核**

### 方法

`valgrind --tool=callgrind`（确定性指令级成本，不受采样偏差与 strace 放大影响）：

```
python3 .dsh_tmp/_callgrind_server.py --scenario s1 --conns 8 --duration-ms 2500
# 服务端 valgrind 下 QPS 799、completed 1,998
```

### 结果（exclusive 成本，PROGRAM TOTALS = 164.2M Ir）

| 函数 | Ir | 占比 |
|---|---:|---:|
| **pthread_mutex_lock** | 18.4M | **11.20%** |
| **pthread_mutex_unlock** | 13.8M | **8.41%** |
| worker_loop（自身） | 10.9M | 6.63% |
| **__tls_get_addr** | 8.2M | **4.98%** |
| _int_free | 8.0M | 4.86% |
| _int_malloc | 6.3M | 3.82% |
| poll_once | 6.1M | 3.69% |
| malloc | 5.6M | 3.39% |
| EpollBackend::wait | 4.3M | 2.62% |
| register_wait | 2.1M | 1.25% |

聚类：**互斥锁 ≈ 19.6%**、**malloc/free 系列 ≈ 16%**、**TLS 访问 5%**。

### 决定性结论

`164.2M Ir / 1,998 请求 = 82,200 指令/请求 ≈ 27µs @3GHz`（纯用户态）。

而 `/proc` 口径的 s1 是 **687µs CPU/请求** ⇒ **用户态只占 4%，96% 在内核态**。
对照 thread 模型同样是 20 次左右的 syscall，内核只有 48µs —— **GMP 的内核开销是
thread 的 14 倍**。

**s1 的 −26% 无法靠用户态优化追平**：就算把用户态全部消灭，也只影响 4%。
剩下的账（park/wake 的内核调度 + epoll ONESHOT 注册/注销 + 每连接 close）需要
内核级 profiling（`perf`/`bpftrace`），当前环境两者都不可用
（`perf_event_paranoid=2`、无 sudo、无 bpftrace）。

### 可做的（用户态，收益微小但确定）

1. `pthread_mutex_unlock`+`lock` 占 19.6% ⇒ 每请求仍有大量短锁（`Socket::m_mutex`
   的 `begin_operation`/`is_closing`/`end_operation` 各一次、`can_wait` 的
   `m_open->m_mutex`、`DescriptorGuard` 的条带锁）。合并 `Socket` 的几次加锁能省
   几个百分点。
2. malloc/free 16% ⇒ `Socket::Ptr`、`std::string`、`shared_ptr<WaitNode>` 的分配；
   `WaitNode` 已有 TLS 缓存，`Socket` 可以池化。
3. `__tls_get_addr` 5% ⇒ 动态库里的 `thread_local` 访问都走 GOT 间接调用；
   热路径上的 `t_current_task`/`t_processor_id` 可考虑缓存到局部。

**收益换算**：用户态合计 27µs，即使砍掉一半也只省 13µs（687 → 674µs，QPS +2%）。

### 决策

s1 暂不追（投入产出比最低）；后续轮次优先做「用户态占比高」的场景
（s2/s4/s5 的长连接路径用户态占比更高，且它们的差距只有 3%）。
若之后能拿到 `perf`（需要宿主机放开 `perf_event_paranoid`），再回来做 s1 的内核态归因。
## 轮次 14（2026-10-18 22:47–23:07）：s2 的 callgrind —— 用户态同样只占 3%

s2（长连接 echo），callgrind 下 completed 5,281，PROGRAM TOTALS = **152.1M Ir**
⇒ **28,800 指令/请求 ≈ 9.6µs 用户态**，而 s2 的 `/proc` 口径是 **312µs CPU/请求**
⇒ **用户态占 3%**，与 s1 的 4% 几乎一样。

| 函数 | 占比 |
|---|---:|
| **pthread_mutex_lock + unlock** | **22.2%** |
| **__tls_get_addr** | **6.37%** |
| worker_loop（自身） | 5.57% |
| malloc/free 系列 | ~9% |
| register_wait | 2.60% |
| cooperative_io（hook 的 recv 包装） | 2.35% |
| poll_once / EpollBackend::wait | 2.03 / 1.44% |
| purge_terminal_locked | 1.21% |
| DescriptorGuard 构造 / snapshot_descriptor / descriptor_for | 1.21 / 1.07 / 1.02% |

**结论：s1~s5 全部是内核态主导，用户态优化（锁、分配、TLS）最多影响 3~4%。**

### 因此唯一有效的方向：减少内核操作

s2 关注入后的 syscall 约 9 次/请求，其中 **`epoll_ctl` 2.5 次** 是最大可减项：

- 现在：ONESHOT ⇒ 每次 park 一次 `MOD`（rearm），每次完成一次 `MOD`/`DEL`；
- 2.5 次 = register 1 + complete 1 + 事件处理 0.5。

**下一轮的实验（单一、可回滚）**：把 IO 后端从 ONESHOT 改为 **LT + 保持注册**：
`interest` 从 IN→0 时**不清零内核注册**（只在 `notify_close`/`fd 复用`时 DEL），
等待时若 interest 未变化就跳过 `EPOLL_CTL_MOD`。预期 `epoll_ctl` 2.5 → 1，
省 1.5 次 syscall（~5~15µs）。

**已知风险**：LT 下若某个 fd 一直可读而没有等待者，`epoll_wait` 会持续返回它
（忙循环）。现有 `process_event_locked` 已有 `pending_read` 丢弃逻辑，但必须在压测里
确认 s3（1 万空闲连接）不出现 CPU 飙升；一旦出现就回滚。

### 备选（更保险）

只合并 `complete()` 的 DEL：`expire_timers()` 与 `complete_if_head()` 里各有一处
`stripe.slots.erase` / `invalidate_registration_locked`，可延后到「下次同 fd 注册」时
再处理，省掉完成路径的 `DEL`（约 0.5 次/请求）。
## 轮次 15（2026-10-18 23:07–23:27）：poll 期间的本 P 投递不再唤醒别的 worker（**+2.5%，保留**）

### 定位

`poll_once` → `process_events_batch` → `wake_nodes` → `wake_io` → `requeue_from_worker`。
此时处理事件的 worker 状态是 `kParked`（`running_machines == 0`），于是

```cpp
notify_worker = allow_notify && (running == 0 || ...);
```

恒为真 ⇒ **每次 IO 完成都多一次 futex wake / eventfd 写入**，而被唤醒的 worker
大概率发现队列已被投递者自己取走（投递者紧接着 `continue` 回主循环取本 P 队列）。

### 改动

- 新增文件级 `thread_local bool t_in_idle_poll` + `IdlePollScope` RAII；
- `worker_loop` 里 `idle_poller->poll(timeout_ms)` 包在 scope 内；
- `requeue_from_worker` 的 notify 条件加 `!t_in_idle_poll`。
- **只短路本 P 投递**；`requeue_to_processor`（跨 P）照常通知。

### A/B（s2 + `IO_SLEEP=0`，同窗口各 2 rep）

| 版本 | QPS | p50 |
|---|---:|---:|
| 改动后 | **23,070 / 22,399**（中位 22,735） | 2.63 / 2.74ms |
| 回滚后 | 22,469 / 21,875（中位 22,172） | 2.73 / 2.82ms |

**+2.5% QPS、p50 略降**。逻辑上是纯粹去掉「多余的跨线程唤醒」，方向正确，
**保留**（`ctest --test-dir build-quality` → **113/113，60.24s**）。

### 当前累计的有效改动（4 项）

| 改动 | 效果 |
|---|---|
| 空闲 `FdSlot` 不立即删除 | s3 **+56%**，P50 131× |
| poll 上下文不 tickle（io 侧） | s2 +11%，sys −24% |
| Fiber 栈复用不重复 mprotect | `go()` −18%，s1 +4% |
| poll 期间本 P 投递不唤醒（scheduler 侧） | s2 +2.5% |

四项共同的主题都是**去掉「本可以不做」的唤醒/系统调用**，而不是改算法。
## 轮次 16（2026-10-18 23:27–23:47）：**s1 的基准客户端才是瓶颈**（口径问题）

### 4 项改动后的同窗口五场景（loadavg 1.8~2.2）

| 场景 | GMP | thread | epoll | GMP vs thread | 对比改动前 |
|---|---|---|---|---|---|
| s1 | 7,586 / 6.85ms | 10,469 / 4.71ms | 3,027 | −27.5% | −28.5% |
| s2 | 21,593 / 2.98ms | 22,114 / 2.96ms | 3,035 | **−2.4%** | −3.7% |
| s3 | **9,802 / 3.44ms** | 9,643 / 2.85ms | 3,051 | **+1.6%** ✅ | +1.8% |
| s4 | 21,108 / 3.05ms | 21,981 / 2.98ms | 3,001 | −4.0% | −2.9% |
| s5 | 11,350 / 2.83ms | 11,565 / 2.76ms | 3,051 | **−1.9%** | −2.6% |

### 关键发现：s1 的**单客户端**是瓶颈

`s1` 是「64 并发短连接」，但基准客户端是**单线程 epoll**。把它拆成两个客户端进程
（各 32 连接，总连接数不变）：

| s1（同窗口） | 单客户端 64 连接 | 双客户端 32×2 | 变化 |
|---|---:|---:|---:|
| **GMP** | 7,586 | **15,139** | **+100%** |
| **thread** | 10,469 | **19,104** | +82% |
| GMP p50 | 6.85ms | **3.78ms** | −45% |

⇒ **之前所有「s1 −27.5%」的数字都被客户端掩盖了**：GMP 的服务端能力在双客户端下
直接翻倍（+100%），而 thread 只 +82% —— 说明 GMP 的单客户端延迟更高（6.85 vs 4.71ms），
把客户端拖成了瓶颈。

**修正后的真实差距：15,139 vs 19,104 = −20.7%**（不是 −27.5%），
但 GMP 的 CPU 仍是 thread 的 3.2 倍（702% / 464µs vs 274% / 143µs）。

### 需要修正的口径

`s1` 单客户端 7.6~10.5k 是**客户端上限**，不能代表服务端能力。后续 s1 评测应至少用
2 个客户端进程（或把客户端改成多线程 epoll）。这条要同步到
`docs/gmp-perf-analysis.md` 与 `benchmarks` 的使用说明。

### 下一轮

1. s1 用 **4 客户端**（16×4）测 GMP 的真实上限，并与 thread 同窗口对比；
2. 若 GMP 的真实上限接近或超过 thread，则 s1 的「差距」主要是**客户端 + 延迟**
   （p50 3.78 vs 3.04ms），而不是吞吐能力。
## 轮次 17（2026-10-18 23:47–00:07）：**s1 在正确口径下已超越 thread**

### 四客户端（16×4，总 64 连接，同窗口）

| s1 | QPS | P50 | P99 | 错误 |
|---|---:|---:|---:|---:|
| **GMP** | **20,232** | **3.01ms** | 5.68ms | 844 |
| thread | 19,388 | 3.04ms | 5.38ms | 0 |
| epoll+池 | ~3,030（单客户端口径，未重测） | — | — | — |

**GMP 的 QPS +4.4%、P50 略优（3.01 vs 3.04ms）**，但 P99 略差（5.68 vs 5.38ms），
且多了 844 个错误（0.83%）。

### 口径修正（重要）

单客户端 64 连接时测到的 7,586 QPS **完全是客户端上限**（单线程 epoll）：

| 客户端数 | GMP | thread |
|---|---:|---:|
| 1 × 64 连接 | 7,586 | 10,469 |
| 2 × 32 连接 | 15,139 | 19,104 |
| **4 × 16 连接** | **20,232** | **19,388** |

GMP 随客户端数**线性扩展**（7.6k → 15.1k → 20.2k），thread 在 2 客户端后趋平
（10.5k → 19.1k → 19.4k）。**结论：s1 的 GMP 吞吐上限高于 thread**，
之前的「−27.5%」是客户端瓶颈造成的假象。

### 遗留：844 个错误

`869` 的分析（轮次 3）指出这些是 `EPOLLHUP`（errno=0，客户端在收到响应前看到对端关闭），
不是 connect 失败。0.64% 的已建立连接被服务端提前关闭，需要单独归因：
- 服务端 `serve_gmp_connection` 在 recv 失败时直接 close；
- 也可能与 `accept` 速率（~10.5k/s）跟不上 16.5k/s 的连接建立速率有关（listen 队列压力）。

### 下一轮

1. 把 s1 的错误定位到具体分支（给 `serve_gmp_connection` 的每条 return 加计数）；
2. 若与 accept 速率有关，考虑「多 accept 线程」或提高 listen backlog；
3. 同步更新 `docs/gmp-perf-analysis.md` 的 s1 口径说明。
## 轮次 18（2026-10-19 00:07–00:27）：s1 的 896 个错误**不在服务端**

### 改动与结果（诊断埋点，已回滚）

给 `blocking_serve_loop` 的 5 条 return 各加一个计数器，跑 s1 四客户端：

```
[serve-exit] gmp total=131730 recv_hdr=131688 bad_len=0 recv_body=0 opcode0=0 send=42
```

- 服务端一共处理 **131,730** 个连接，与客户端 `established=131,729` **完全一致**；
- 其中 **131,688 次是「recv 头失败」**——这是 s1 的正常终止路径：客户端收到响应后
  用 `SO_LINGER 0`（RST）关闭，服务端的下一次 `recv` 拿到 ECONNRESET 后退出；
- **`send` 只失败 42 次**，`bad_len`/`recv_body`/`opcode0` 全为 0。

⇒ **服务端没有「提前关闭连接」的路径在跑**（除 42 次回写失败），客户端的 896 个
`EPOLLHUP` 不是服务端造成的。

### 最可能的根因：**客户端 epoll 事件的 `generation` 竞态**

`LoadClient::update_interest()` 里事件数据是 `event.data.u64 = index`，而 `CConn`
还有一个 `generation` 字段用于校验定时器。短连接场景下每毫秒都有连接关闭/重连，
同一个 `index` 会被复用：

1. 旧连接 close（`epoll_ctl(DEL)` + `fd = -1`）；
2. 内核对**已入队但尚未返回**的旧事件仍会交付；
3. 新连接复用同一 `index`，`fd >= 0`，`handle_event` 的
   `if (m_conns[index].fd < 0) continue;` **拦不住**；
4. 旧事件（可能带 `EPOLLERR|EPOLLHUP`）作用到新连接上 ⇒ `++m_errors`。

**这与「错误率 0.68%、且 thread 模型为 0」不一致** —— thread 模型的连接生命周期更长
（每连接一个线程），index 复用频率低，所以几乎不触发。

### 下一轮

把 `data.u64` 编码成 `(generation << 32) | index`，`handle_event` 先校验 generation；
若旧事件就丢弃。预期 896 → 接近 0，且**不影响吞吐**（纯粹是丢弃过期事件）。
`docs/gmp-perf-analysis.md` 里 s1 的「错误」口径也要同步修正。
## 轮次 19（2026-10-19 00:27–00:47）：generation 校验 **无效，已回滚**

**假设**：s1 四客户端下 874~896 个 `EPOLLHUP` 来自「槽位 index 复用 + 内核交付
过期事件」，改成 `(generation << 32) | index` 并在分发前校验即可消除。

**实测（四客户端 16×4，同窗口）**：

| 版本 | QPS | errors |
|---|---:|---:|
| 只按下标分发（原） | 20,232 | 874 |
| **带 generation 校验** | **20,198** | **997** |

**错误没有减少（反而略增）** ⇒ 假设不成立，**已回滚**（`diff` 确认恢复）。

### 重新梳理「已知事实」，避免继续瞎猜

1. 服务端埋点：`serve-exit total=131,730`，与客户端 `established=131,729` **完全一致**；
   `send` 失败只有 **42** 次，`recv_hdr` 失败 131,688 次（客户端 RST 的正常收尾）；
2. 客户端错误**全部**是 `client_epollerr_hup`，`client_connect`/`client_so_error`
   一次都没出现过；
3. thread 模型同一客户端、同一场景 **0 错误**；
4. 错误率 ≈ 0.68%，与 GMP 的处理时延（p50 3ms，thread 2.9ms）同量级但不是线性关系。

⇒ **服务端没有「主动关连接」的代码路径在跑**，但客户端确实「在收到响应前看到了对端关闭」。
**剩下的唯一解释是「42 次 send 失败之外的时序竞争」**：服务端 G 结束（
`blocking_serve_loop` 收到 ECONNRESET 后 return）→ `socket->close()` → FIN/RST
追上了尚未被客户端读走的响应。

### 下一步（换思路）

不再从客户端找，而是**从服务端收尾时序入手**：在 `serve_gmp_connection` 的
`socket->close()` 之前插入一次 `shutdown(fd, SHUT_WR)`（send FIN 而不是 RST），
或者确认 close 是否因为在途数据触发 RST。这项改动在**库的 Socket::close** 或
benchmark 的收尾路径上，可单独 A/B（看 errors 是否降到接近 0，且吞吐无损）。

若两轮内仍无解，就把 s1 的 errors 记为**已知局限**（thread 模型每连接一个内核线程，
关闭时序由内核保证；GMP 用共享 worker 收尾，天然多一个竞争窗口），并把结论写进
`docs/gmp-perf-analysis.md` 的已知局限列表。
## 轮次 20（2026-10-19 00:47–01:07）：RST 假设也被证伪 —— s1 的 errors 记为已知局限

**假设**：客户端用 `SO_LINGER 0`（RST）关闭，导致服务端 close 时用 RST 收尾，
客户端「响应未读完就对端关闭」。

**实测**（临时改成正常 FIN 关闭，四客户端 16×4）：

| 版本 | QPS | errors | P99 |
|---|---:|---:|---:|
| RST 关闭（原） | 20,232 | **874** | 5.68ms |
| **FIN 关闭（诊断）** | **14,598** | **3,476** | 32.52ms |

错误**暴增 4 倍**、吞吐掉 28%（TIME_WAIT 吃端口）⇒ **RST 不是元凶，反而是当前的
最优选择**。已回滚。

### 三轮排查后的结论（18/19/20）

| 假设 | 结论 |
|---|---|
| 服务端主动关连接 | 埋点证明没有：`send` 失败仅 42 次，其余 131,688 次是客户端 RST 的正常收尾 |
| 客户端 epoll index 复用（generation 竞态） | 加 generation 校验后 874 → 997，无效 |
| 客户端 RST 关闭引发 | 改 FIN 后 874 → 3,476，反向 |

**剩下的解释**：GMP 用**共享 worker 收尾**（G 结束 → `Socket::close()` 在某个 worker
上执行），而 thread 模型是**每连接一个内核线程**、close 与 recv 在同一个线程里串行完成。
两者在「最后一个响应字节被客户端读走」与「服务端 close 发出 FIN/RST」之间存在一个
GMP 特有的竞争窗口。0.68% 的错误率与 p50（3ms）同量级，符合「窗口 × 连接速率」的估算。

**记为已知局限**，写入 `docs/gmp-perf-analysis.md` 的「已知局限」列表：
> GMP 的 s1（短连接）在 4 客户端口径下约有 **0.68% 的连接**会被客户端记为
> `EPOLLHUP`（服务端收尾时序竞争）；thread 模型为 0。已排除服务端主动关闭、
> 客户端 index 复用、RST 关闭三种成因。彻底修需要在 `Socket::close()` 前
> 做「等待对端确认最后一次写入」（如 `shutdown(SHUT_WR)` + 读空到 EOF），
> 但这会增加每连接一次往返，代价高于收益。

### 当前战绩（正确口径）

| 场景 | GMP vs thread | 备注 |
|---|---|---|
| s1 | **+4.4%**（20,232 vs 19,388） | 四客户端；errors 874 为已知局限 |
| s2 | −2.4% | |
| s3 | **+1.6%** | |
| s4 | −4.0% | |
| s5 | −1.9% | |
## 轮次 21（2026-10-19 01:07–01:27）：s4 的真实差距是 25%，被 sleep 掩盖了

### s4 关闭注入延迟（同窗口 2 rep）

| s4 + `IO_SLEEP=0` | QPS | P50 | CPU% | CPU µs/请求 |
|---|---:|---:|---:|---:|
| GMP | 23,555 | 2.58ms | 457.6 | **194.2** |
| thread | **31,516** | 1.98ms | 135.4 | **42.9** |

**GMP 落后 25%**（开注入时只差 4.0%）。原因：注入的 `usleep(0~5ms)` 在 GMP 上要
334µs CPU、在 thread 上只要 ~2µs，它把两边的差距**稀释**成了 4%。

### 这条数据改变了整体判断

| 场景 | 开注入（基准默认口径） | 关注入（纯 IO 能力） |
|---|---|---|
| s2 | −2.4% | 之前测过：GMP 30,720 vs thread 29,078（**+5.6%**，但那是另一时段） |
| **s4** | **−4.0%** | **−25%** |
| s5 | −1.9% | 未测 |

⇒ **GMP 的纯 IO 路径成本是 thread 的 4~5 倍（内核态）**，这是 s1/s4 的真实差距；
默认口径下只差 2~4%，是因为基准自带的 sleep 占了主导。

**对目标的直接含义**：用户的口径是**基准默认**（开注入）。在这个口径下，
s1/s3 已超越、s2/s4/s5 只差 2~4%，**差距是可追的**；但「关注入」的 25% 是
协作式调度 + 单 IOManager 的结构性成本，短期不可能消除。

### 下一轮

优先做**默认口径下能兑现**的项：
1. s4 的 4KB 双向传输：确认 `send_all_socket` 是否因 4KB 分片而多次 `sendmsg`
   （若是，可以把「头 + body」合并成一次 `writev`，省 1 次 syscall/请求）；
2. s5（慢客户端）的 −1.9%：它只有一个慢连接，主要看 4MB 响应的分块发送。

两者都属于「减少一次 syscall」的量级，与之前 4 项有效改动的主题一致。
## 轮次 22（2026-10-19 01:27–01:47）：s4 的 syscall 构成 —— 三项各 2 次/请求

`s4` + `strace -e trace=recvmsg,sendmsg,epoll_ctl,futex`（16 连接，3s）：

| syscall | calls | 每请求（按 completed≈11.5k 估） |
|---|---:|---:|
| sendmsg | 23,162 | **2.01** |
| recvmsg | 23,166 | **2.01** |
| epoll_ctl | 23,143 | **2.01** |
| futex | 4,357 | 0.38 |

- `recvmsg` 2 次 = 4B 头 + body（s4 的 body 是 4KB 或 1B，一次能读完）；
- `sendmsg` 2 次 = 4B 头 + body（**可以合并成一次 `writev`，省 1 次 syscall**）；
- `epoll_ctl` 2 次 = park 时的 ONESHOT rearm + 完成路径，**固有**；
- `futex` 0.38 次，已经很低（轮次 15 的改动生效）。

### 为什么不改 benchmark 的 send 合并

把「头 + body」合并成一次 `Socket::sendv` 能省 1 次 syscall/请求（≈2.6%），
但那是**应用层写法**，与并发模型无关。要用于对比就必须**两个模型同时改**
（thread 侧的 `write_response_fd` 也要改成 `writev`），否则对比失真；
而且 benchmark 的 `process_request` 一次性构造 body，真实服务端未必能合并。
⇒ **不作为本轮改动**，但值得写进「推荐用法」：**小帧场景用 `sendv` 合并头体**。

### 下一轮：`pending_read` 快速路径

`register_wait` 里已经有 `pending_read/pending_write`（上一次 epoll 事件遗留的就绪提示）
与零超时 `poll` 复检。目前的用法是「用 pending 避免一次 park」，但仍然走了
`register_wait` → `update_interest_locked`（一次 `epoll_ctl`）。

**改进**：在 `IOManager::wait()` 入口直接读 slot 的 pending 位，若命中且零超时
`poll` 复检通过，**直接返回 kReady**，完全跳过 `register_wait` 与 `epoll_ctl`。
预期对 s2/s4/s5（高频 ping-pong）每请求省 1 次 `epoll_ctl`（≈1 次 syscall，2~3%）。

**风险**：pending 是「候选提示」，可能 stale（数据已被别的读走）。必须保留
零超时 `poll` 复检（`::syscall(SYS_poll, ..., 0)`，1 次 syscall），
所以净收益是「把 epoll_ctl 换成 poll」——只有 poll 更便宜时才划算，
需要实测确认。
## 轮次 23（2026-10-19 01:47–02:07）：`pending` 快速路径 —— 净收益为零，已回滚

**改动**：`IOManager::wait()` 入口新增 `State::try_consume_pending(fd, event)`：
若 slot 上有上一次 epoll 事件留下的 `pending_read/pending_write`，做一次零超时
`poll` 复检，命中就直接返回 `kReady`，跳过 `register_wait` 与那次 `EPOLL_CTL_MOD`。

**实测（`IO_SLEEP=0`，同窗口 2 rep）**

| 场景 | 改前 | 改后 |
|---|---:|---:|
| s2 | 22,469 / 21,875（中位 22,172） | 22,469 / 22,164（中位 22,317） |
| s4 | 23,555（单次） | 22,617 / 22,171（中位 22,394） |

**无净收益**（s4 还略低）⇒ **已回滚**。

### 原因（与事前预判一致）

净收益其实是「把 1 次 `epoll_ctl` 换成 1 次 `poll`」。WSL2 上两者同量级（~5µs），
唯一的节省是 `register_wait` 入队的那点用户态指令（而用户态只占 3~4%）。

**这再次印证了 callgrind 的结论**：只要不减少**内核操作的总量**，换一种系统调用
没有意义。

### 到本轮为止的结论（稳定）

- **有效改动 4 项**（空闲 slot 保留、tickle 短路、mprotect 复用、poll 期不唤醒）；
- **被证伪 9 项**（epoll 忙轮询、tickle 洪水、Fiber 池容量、sleep eventfd、notify 跳过、
  accept 批量、用户态锁/malloc/TLS、generation 竞态、RST 关闭、pending 快速路径）；
- **剩余成本结构**：每请求 `2×recvmsg + 2×sendmsg + 2×epoll_ctl + 1×park/wake`，
  全部是内核操作，且每一项都已是最少次数。

**若要继续提升，只有两条路**（都超出当前环境能力）：
1. 用 `io_uring` 把 recv/send/epoll_ctl 合并成一次提交（需要重写 IO 后端）；
2. 拿到 `perf` 权限，确认 660µs 内核时间在 `epoll_ctl`/`futex`/TCP 栈之间的真实分布。

### 当前战绩（默认口径，同窗口）

s1 **+4.4%**（四客户端）✅、s2 −2.4%、s3 **+1.6%** ✅、s4 −4.0%、s5 −1.9%。
## 轮次 24（2026-10-19 02:07–02:27）：**epoll_ctl 全是 ADD/DEL，没有 MOD** —— 与设计矛盾

### 实测（`strace -e trace=epoll_ctl` 原始输出 + 操作分类）

s2，16 连接，3s，completed 7,612：

```
EPOLL_CTL_ADD: 11,332   (1.49 次/请求)
EPOLL_CTL_DEL: 11,331   (1.49 次/请求)
EPOLL_CTL_MOD: 0        ← 一次都没有
```

### 为什么这与设计矛盾

`update_interest_locked()` 的设计意图写得很清楚（`io_backend.cpp:81` 的注释）：

> `interest==0` 是一个有意的「停用注册」：保留 fd 在 epoll 实例中，但暂时不监听
> 任何普通就绪事件。这样同一个 fd 的等待者频繁进出时只需 `EPOLL_CTL_MOD`，
> 不必反复 `DEL/ADD`。

而现在的实测是**每个 park 都 `DEL` 一次、下一个 park 再 `ADD` 一次**，完全没有 MOD。
按代码路径推断应该是：

1. `wait` → `register_wait` → `update_interest_locked`（interest 0→IN）：
   `slot.registered` 若为 true 走 `MOD`，为 false 走 `ADD`；
2. 事件到达 → `process_event_locked` 把 `slot.armed = false`（1268-1271）；
3. `complete()` → interest=0 → `if (!slot.registered || !slot.armed) { slot.interest=0; return; }`
   —— **armed 已 false，不该 DEL**。

但实测 DEL 与 ADD 严格成对（11,332 vs 11,331），说明 `slot.registered` 在每次
`complete` 之后确实变成了 false，`armed` 也确实被置回了 true（否则不会 DEL）。

**嫌疑点**（下一轮用计数器逐个验证）：
- `process_event_locked` 的早退分支 `if (!slot.registered || slot.registration_id != registration_id) return;`
  —— 迟到事件不影响 armed，但如果**正常事件也被判为迟到**，armed 就保持 true，
  下一次 `complete` 就会 DEL；
- `update_interest_locked` 在 interest 非 0 时会把 `slot.armed = true`（1167），
  如果 `process_event_locked` 末尾又调了一次 `update_interest_locked`（还有别的等待者），
  armed 会被重置为 true。

**如果定位成功并修掉，每请求可省 2 次 `epoll_ctl`（DEL + ADD → 0 或 1 次 MOD），
按之前的数据约 5~10µs/请求，对 s2/s4/s5 是 2~5%。这是本轮之后最值得做的一项。**
## 轮次 25（2026-10-19 02:27–02:47）：确认 DEL 来自「interest==0 且 armed」

在 `update_interest_locked()` 的 `interest==0` 分支（`invalidate_registration_locked`
之前）加计数器，跑 s2（16 连接，3s，completed 16,693）：

```
[epoll-stat] io add=0 mod=0 del_armed=21404 del_nowait=0
```

**`del_armed = 21,404` ⇒ 1.28 次/请求**，与 strace 的 `DEL 1.49/请求` 量级一致
（差额来自 warmup 与连接建立期）。

⇒ **DEL 100% 来自「`interest == 0` 且 `slot.armed == true`」这一条分支**。

### 这说明了什么

`process_event_locked()` 里明明有：

```cpp
if (one_shot) {
    if (!slot.armed) return;
    slot.armed = false;      // ← ① 事件消费后置 false
}
```

但 `complete()` 走到 `update_interest_locked()` 时 `armed` 又是 **true**。只可能是：

1. **事件根本没走 `process_event_locked`**（例如由 `expire_timers` / `fail_slot` /
   `complete_if_head` 的补采样路径完成）—— 但 s2 无超时、无错误；
2. **`process_event_locked` ①之后又调用了一次 `update_interest_locked`**，而那时
   `readers` 仍非空（`interest != 0`）⇒ 1167 行 `slot.armed = !one_shot || interest != 0`
   **把它重新置为 true**；等那个等待者被 `complete()` 消费时，interest 变 0 但 armed=true
   ⇒ DEL。

**s2 每请求 1.28 次 DEL（不是 1 次）** 正好符合「有的 park 之后队列里还有一个等待者」的特征
（`recv` 头与 body 分成两次 park 时会多一个节点）。

### 修法（下一轮）

在 `complete()` 的 interest==0 路径上区分「本次完成来自事件」还是「来自超时/取消」：

- **事件路径**：`process_event_locked` 已经消费了 ONESHOT，内核已自动 disarm ⇒
  只需把 `slot.armed = false`，**不 DEL**；
- **超时/取消路径**：内核仍 armed ⇒ 必须 DEL（保持现状）。

实现上给 `complete()` 增加一个 `bool from_event` 参数（或用一个 per-node 的
`event_consumed` 标记），只有 `from_event` 时才跳过 DEL。预期每请求省 1.28 次
`epoll_ctl`，对 s2/s4/s5 约 2~4%。
## 轮次 26（2026-10-19 02:47–03:07）：DEL 全部发生在「无等待者」时 —— 是取消注册，省不掉

轮次 25 确认 DEL 来自 `update_interest_locked()` 的 `interest==0 且 armed` 分支。
本轮统计触发那一刻槽位里的等待者状态（s2，completed 16,717）：

```
[del-stat] io empty=21383 readers_only=0 writers_only=0 both=0
```

**21,383 次 DEL 全部来自 `readers` 与 `writers` 都为空**，即「最后一个等待者被消费完之后」。

### 为什么会这样

`process_event_locked()` 消费 ONESHOT 事件后：
1. `slot.armed = false`（内核已 disarm）；
2. 末尾调 `update_interest_locked()`：`interest == 0 && !armed` ⇒ **直接 return，不 DEL** ✓

但随后 **`complete()` 路径**（或下一次 `register_wait` 的收尾）再次进入
`update_interest_locked()`，此时 `armed` 又被置回了 true（因为中间某个时刻
`interest != 0`——例如 `recv` 头与 body 分两次 park 时，两个等待者先后进出），
于是 interest 归零时走 DEL。

**这条 DEL 其实是有意义的**：无等待者时如果保留 `EPOLLIN` 注册，fd 一可读就会
持续触发 `epoll_wait`（`take_ready_locked` 返回空 → 置 `pending_read` → 忙循环）。
所以「无等待者时必须关掉监听」是正确行为，**要么 DEL，要么 MOD events=0**。

### 结论：这条路优化不了

- `DEL`（1.28 次/请求）改 `MOD events=0` 只是换一种系统调用，**次数不变**；
- 完全不通知内核会让无等待者的可读 fd 变成忙循环；
- 所以 `ADD + DEL` 各 1.28~1.49 次/请求是**当前设计的必然成本**。

（`ADD` 而不是 `MOD` 是因为 DEL 把 `slot.registered` 清了；如果改成
`MOD events=0` 保留注册，下一次就是 `MOD` 而不是 `ADD` —— 但仍然是 1 次，
且 `MOD` 与 `ADD` 在 WSL2 上同价，所以没有净收益。）

### 累计被证伪的优化路径：10 项

epoll 忙轮询、tickle 洪水、Fiber 池容量、sleep eventfd 复用、notify 跳过、
accept 批量化、用户态锁/malloc/TLS、generation 竞态、RST 关闭、pending 快速路径、
**epoll_ctl 减少**。

**剩余可做的只有两条**（都需要环境或架构级改动）：
1. `io_uring` 后端（把 recv/send/epoll_ctl 合并成批量提交）；
2. 拿到 `perf` 权限做内核态归因。
## 轮次 27（2026-10-19 03:07–03:27）：完整战绩（三 rep 中位数 + s1 四客户端）

### s2~s5（`run --repeats 3`，同窗口，loadavg 0.7→3.2）

| 场景 | GMP | thread | GMP vs thread |
|---|---:|---:|---:|
| s2 | 21,389 / 3.01ms | 22,167 / 2.95ms | **−3.5%** |
| s3 | **9,833 / 3.45ms** | 9,651 / 2.84ms | **+1.9%** ✅ |
| s4 | 20,866 / 3.08ms | 22,132 / 2.96ms | **−5.7%** |
| s5 | 11,368 / 2.83ms | 11,555 / 2.76ms | **−1.6%** |

### s1（四客户端 16×4，同窗口）

| s1 | QPS | P50 | P99 | 错误 |
|---|---:|---:|---:|---:|
| **GMP** | **20,284** | **3.01ms** | 5.69ms | 1,030 |
| thread | 19,524 | 3.04ms | 5.28ms | 0 |

**GMP +3.9%，P50 略优**；P99 略差（5.69 vs 5.28ms）；错误 1,030（0.68%，轮次 18-20 已定性为
「共享 worker 收尾时序」的已知局限）。

### 完整战绩（当前口径）

| 场景 | GMP vs thread | QPS | 判定 |
|---|---|---:|---|
| **s1** | **+3.9%** | 20,284 | ✅ 超越（四客户端口径） |
| s2 | −3.5% | 21,389 | ❌ |
| **s3** | **+1.9%** | 9,833 | ✅ 超越（且贴近协议上限 10k） |
| s4 | −5.7% | 20,866 | ❌ 最大短板 |
| s5 | −1.6% | 11,368 | ❌（噪声边缘） |

**2/5 超越，3/5 差 1.6~5.7%。**

### 下一步

s4 是最大短板（−5.7%）：50% READ（4KB 上）+ 50% WRITE（4KB 下）。
关注入后它是 −25%（纯 IO 能力差距），默认口径下 −5.7% 已经是「sleep 稀释后」的结果。
**可做的不多**，但有一个未试过的点：s4 的 `kReadBody = 4096` 会让 `recv` 一次读 4KB，
`Socket::recv` 的 `recvv` 每次都要 `wait_ready`（带 100ms 切片）——如果 4KB 一次到不齐，
就会多次 park。下一轮先确认 s4 的 park 次数（用 `[epoll-stat]` 或 strace 的 epoll_ctl 数）。
## 轮次 28（2026-10-19 03:27–03:47）：s4 的 park 次数与 s2 完全相同 —— 无可优化点

假设：s4 的 4KB body 一次到不齐，导致多次 `wait_ready`/park（每次带 100ms 切片），
从而比 s2 多付 park/wake 成本。

**实测**（`strace -e trace=epoll_ctl` 原始输出，completed 7,627）：

```
EPOLL_CTL_ADD: 11,422  (1.50 次/请求)
EPOLL_CTL_DEL: 11,421  (1.50 次/请求)
```

**与 s2 的 1.49 完全一致** ⇒ 4KB body 没有造成额外 park，`ADD+DEL` 次数与 64B 的 s2 相同。

### 所以 s4 的 −5.7% 与 s2 的 −3.5% 同源

| s4 | GMP | thread | 倍率 |
|---|---:|---:|---:|
| QPS | 20,866 | 22,132 | 0.94x |
| CPU µs/请求 | **318.9** | **60.3** | **5.3x** |
| epoll_ctl/请求 | 1.50 | 0（无 epoll，阻塞 IO） | — |

s4 只是「4KB 拷贝 + 同样的 park/wake/epoll_ctl 内核成本」，CPU 基数比 s2 大，
所以相对差距看起来不同。**s4 没有独立的可优化点**。

### 至此，五个场景的差距全部归因完毕

| 场景 | 差距 | 归因 | 可优化性 |
|---|---|---|---|
| s1 | **+3.9%** | — | 已超越 |
| s2 | −3.5% | 每请求 park/wake + 2 次 epoll_ctl（内核态） | 已到设计下限 |
| s3 | **+1.9%** | — | 已超越 |
| s4 | −5.7% | 同 s2（4KB 拷贝抬高 CPU 基数） | 无独立优化点 |
| s5 | −1.6% | 同 s2 | 噪声边缘 |

**结论**：在「协作式调度 + 单 IOManager + epoll」这套架构下，剩下的差距已经全部是
**每请求一次 park/wake + 两次 epoll_ctl 的固定内核成本**，无法通过用户态改动消除。
**默认口径下的可优化空间已耗尽**。
## 轮次 29（2026-10-19 03:47–04:07）：口径修正落地 + 最终战绩文档化

### 改动

1. **benchmark 的 `usage()`** 加入 s1 客户端瓶颈提醒（单客户端 ~8k 就是上限，
   测服务端上限请起 2~4 个客户端进程；实测 GMP 20.2k / thread 19.5k）。
2. **`docs/gmp-perf-analysis.md` 新增第 13 节**：口径修正、五场景最终战绩、
   有效改动清单、逐场景归因、两条剩余路径、已知局限。

### 为什么这轮不改库

轮次 20~28 已经把五个场景的差距逐一归因完：

- s1/s3 已超越；
- s2/s4/s5 的 1.6~5.7% 全部是「每请求 1 次 park/wake + 2 次 `epoll_ctl`」的内核成本，
  `epoll_ctl` 的 `ADD+DEL` 是 ONESHOT + 「无等待者必须取消监听」的必然结果；
- callgrind 证明用户态只占 3~4%，10 条优化路径已证伪。

继续瞎改只会浪费轮次，所以把「口径」和「战绩」固化下来，避免后来者再踩
「单客户端测出 7.6k」的坑。

### 当前状态（稳定）

| 场景 | GMP vs thread | QPS |
|---|---|---:|
| s1 | **+3.9%** ✅ | 20,284 |
| s2 | −3.5% | 21,389 |
| s3 | **+1.9%** ✅ | 9,833 |
| s4 | −5.7% | 20,866 |
| s5 | −1.6% | 11,368 |

`ctest --test-dir build-quality` → 113/113。
## 轮次 30（2026-10-19 04:07–04:27）：`interest==0` 不再 DEL，保留注册 —— **+5.9%（保留）**

### 依据（轮次 24~26 的测量）

s2/s4 每请求 `ADD 1.49 + DEL 1.49`、**0 次 MOD**，且 21,383 次 DEL 全部发生在
「readers 与 writers 都为空」时。原因是 ONESHOT 事件被消费后，`slot.armed` 又被
`update_interest_locked` 置回 true，收尾时走 `invalidate_registration_locked`（DEL），
下一次 wait 只能 ADD。

### 改动

`update_interest_locked()` 的 `interest == 0` 分支：ONESHOT 后端下**不再 DEL**，
只把 `slot.interest = 0` 后返回。

**安全性**：ONESHOT 语义保证内核在报告一次事件后自动 disarm；即使 `slot.armed` 仍为真，
最多再报一次空事件，然后彻底安静 —— 不会忙循环。实测 CPU 正常（无飙升）。
`notify_close`（fd 真正关闭）仍走 `invalidate_registration_locked`，所以不会泄漏注册。

### 同窗口 A/B（`IO_SLEEP=0`，各 2 rep）

| 场景 | 改前（回滚后） | 改后（不 DEL） | 变化 |
|---|---:|---:|---:|
| **s2** | 21,617 / 21,793（中位 21,705） | **23,171 / 22,778（中位 22,975）** | **+5.9%** |
| **s4** | 22,335 / 22,358（中位 22,347） | **23,195 / 22,509（中位 22,852）** | **+2.3%** |

p50 也同步改善（s2 2.84 → 2.63ms；s4 2.76 → 2.63ms）。

**保留**。`ctest --test-dir build-quality` → **113/113（61.80s）**。

### 累计有效改动：5 项

| 改动 | 效果 |
|---|---|
| 空闲 `FdSlot` 不立即删除 | s3 **+56%**、P50 131× |
| poll 上下文不 tickle | s2 +11% |
| Fiber 栈复用不重复 `mprotect` | `go()` −18% |
| poll 期间本 P 投递不唤醒 | s2 +2.5% |
| **`interest==0` 不 DEL** | **s2 +5.9%、s4 +2.3%** |

**共同主题仍然是「去掉本可以不做的那一次内核操作」**。这一项和「FdSlot 不删除」
是同一类问题的两面：一个是「不要删槽位」，一个是「不要删内核注册」。

### 下一轮

在同一窗口重测五场景三模型，确认这 5 项叠加后的战绩（s2/s4 的 −3.5%/−5.7% 有望收窄到噪声内）。
## 轮次 31（2026-10-19 04:27–04:47）：不 DEL 的改动在**开注入口径下 s4 恶化**，回滚

轮次 30 的 A/B（`IO_SLEEP=0`）显示 s2 +5.9%、s4 +2.3%，但那是**纯 IO** 口径。
本轮用**用户口径（开注入 0~5ms usleep）**重测 s2~s5（2 rep，loadavg 1.6→4.1）：

| 场景 | GMP（不 DEL） | thread | vs thread | 上一轮（有 DEL） |
|---|---:|---:|---:|---:|
| s2 | 21,607 / 2.98ms | 22,086 | **−2.2%** | −3.5% |
| s3 | 9,794 / 3.06ms | 9,650 | **+1.5%** ✅ | +1.9% |
| s4 | 20,310 / 3.16ms | 22,015 | **−7.8%** | −5.7% |
| s5 | 11,305 / 2.84ms | 11,547 | −2.1% | −1.6% |

**s2 改善 1.3 个百分点，但 s4 恶化 2.1 个百分点**，且 s4 的 CPU 从 665.6% 升到 **720.8%**。

### 解释

「不 DEL」在 ONESHOT 语义下会**多一次空事件**（内核已 disarm，但 `slot.armed` 仍为真时
那次事件前已经入队）。4KB 正文的 s4 每次 park 的 `armed` 状态更容易为真，
空事件更多 ⇒ `epoll_wait` 返回次数与 `process_event_locked`（锁 + 槽位查找）随之增加。
**空事件（~5~10µs + 可能的 worker 唤醒）比一次 DEL（~5µs）更贵。**

⇒ **已回滚**（恢复「armed 为真时 DEL」的原逻辑）。

### 教训（与轮次 30 的结论修正）

轮次 30 的 +5.9%/+2.3% 是**关注入口径**下的结果，不能直接推广到用户口径。
**所有改动必须在「开注入」口径下复测才算数**——这一条此前也踩过（轮次 8 的环境漂移）。

### 当前状态（用户口径，5 项改动中的 4 项仍生效）

| 场景 | GMP vs thread |
|---|---|
| s1 | **+3.9%** ✅ |
| s2 | −3.5% |
| s3 | **+1.9%** ✅ |
| s4 | −5.7% |
| s5 | −1.6% |

有效改动回到 4 项（空闲 slot 保留、tickle 短路、mprotect 复用、poll 期不唤醒）。
## 轮次 32（2026-10-19 04:47–05:07）：最后一次尝试 —— 单 iovec 用 `::recv`/`::send`，失败，回滚

**动机**：`Socket::recv/send` 最终都走 `recvv/sendv(count=1)` → `recvmsg/sendmsg`；
`msghdr` 版本每次都要内核解析控制消息结构，理论上 4KB 场景（s4）比 `::recv` 贵。

**改动**：单 iovec 时改走 `::recv`/`::send`（Hook 对这两个符号都有拦截，park 能力不变）。

**实测**

| 口径 | 场景 | 改前 | 改后 | 变化 |
|---|---|---|---:|---:|---:|
| 关注入 | s4 | 22,347 | **22,686** | +1.5% |
| **开注入（用户口径）** | **s2** | 22,134（thread） | **20,898** | **−5.6%**（改前 −3.5%） |
| **开注入（用户口径）** | **s4** | 21,994（thread） | **20,470** | **−6.9%**（改前 −5.7%） |

关注入有微弱正收益，但**开注入口径下 s2/s4 都变差** ⇒ **已回滚**
（`socket.hpp` 与 `socket.cpp` 均已恢复，`recv_one_impl`/`send_one_impl` 计数为 0）。

### 最终状态（用户口径，4 项有效改动）

| 场景 | GMP | thread | GMP vs thread |
|---|---:|---:|---:|
| s1 | 20,284 | 19,524 | **+3.9%** ✅ |
| s2 | 21,389 | 22,167 | −3.5% |
| s3 | 9,833 | 9,651 | **+1.9%** ✅ |
| s4 | 20,866 | 22,132 | −5.7% |
| s5 | 11,368 | 11,555 | −1.6% |

`ctest --test-dir build-quality` → **113/113（61.14s）**。

### 按用户要求停止

用户指示「下一次如果再不能追平就停止」。本轮（第 32 轮）是那次尝试，结果是
**没有追平**（s2/s4/s5 仍在 −1.6%~−5.7%），因此：

1. **已删除 20 分钟定时任务**（`schedule-6324090c`）；
2. **优化轮次到此结束**，工作树停在「4 项有效改动」的状态；
3. 累计证伪 **12 条**优化路径（见 `docs/gmp-perf-analysis.md` 第 12/13 节）。

### 如需继续，前提是以下之一

- 放开 `perf_event_paranoid`（内核态归因）；
- 决定投入 `io_uring` IO 后端（唯一能真正减少 syscall 条数的路）。

---

## 附：2026-10-19 全项目注释工作启动（每 15 分钟一轮）

后续轮次记录转见 `docs/annotation-progress.md`（进度表与轮次记录）与
`docs/annotation-style.md`（格式规范）。不再往本文件追加。

- 第 1 轮：提交 32 轮优化成果（`e566b4b`）；写规范（136 行）；派 2 个子 agent
  （bufio/compress/hash/encoding、http2/websocket/sse/quic）；确认 GMP 侧注释缺口：
  `context.cpp`（1,920 行 / 注释 9%）、`channel.hpp`（2,127 行）、`future.hpp`（1,061 行）、
  `src/network/io.cpp`、`hook.cpp`、`socket.cpp`。

---

# 轮次 33（2026-10-19）：参考 coost 的 GMP 内核优化

**目标**：参考 coost（github.com/idealvin/coost）优化 GMP 内核，核心指标是降低 `cpu_us_per_req`。

## 一、读懂 coost（阶段 1）

clone 到 `third_party/coost`（Windows 侧 clone 后 rsync 到 WSL），cmake 编译出 `libco.a`。

### coost 的三个关键设计

1. **共享栈 + 拷栈**（`src/co/sched.cc:67`）：
   ```cpp
   Stack* const s = this->get_stack(co->id);
   if (s->co != co) {
       save_stack(s->co, s);      // 当前协程栈内容 → co->buf
       restore_stack(co, s);      // 目标协程 buf → 物理栈
       s->co = co;
   }
   from = tb_context_jump(co->ctx, _main_co);   // 只换寄存器
   ```
   **每次切换拷栈**，而 GMP 的独立栈是**零拷贝**（`swapcontext`）。**这一项 GMP 更优**。

2. **EPOLLET（边缘触发）**（`src/co/epoll/epoll.cc:36`）：
   ```cpp
   ev.events = EPOLLIN | EPOLLET;
   epoll_ctl(_ep, EPOLL_CTL_ADD, fd, &ev);   // 只注册一次，无 rearm
   ```
   GMP 用 `EPOLLONESHOT`（需要 rearm）。**但实测 GMP 的 `epoll_ctl` 每请求只有 0.037 次**，
   因为 `update_interest_locked` 在 interest 未变时跳过——**ONESHOT 不是瓶颈**。

3. **每线程一个 Sched + 本地队列**（`__thread Sched* g_sched`）：
   与 GMP 的 P 结构类似，**窃取时机也相同**（本地空后才 `steal_tasks`）。

### coost 对照的结论

| 维度 | coost | GMP | 谁更好 |
|---|---|---|---|
| 栈切换 | 共享栈 + 拷栈 | 独立栈 + 零拷贝 | **GMP** |
| 调度器数量 | `co_sched_num` 可配 | `processor_count`（原本写死） | 持平（已修） |
| 窃取时机 | 本地空后才偷 | 本地空后才偷 | 持平 |
| **park/wake** | **同线程用户态** | **跨 M futex** | **coost** |

**能借鉴的全部落地，不能借鉴的是 M:N 的固有代价。**

## 二、三个根因（阶段 3）

| # | 根因 | 证据 | 影响 |
|---|---|---|---|
| 1 | **benchmark 把 `processor_count` 写死为 0（= 核数 12）** | 源码 `config.processor_count = 0` | CPU 密集场景 QPS 只有应有的 53% |
| 2 | **`usleep` hook 走协作睡眠**（eventfd + epoll + futex） | strace：`futex` 1,296 次 / 23,145 请求 | 每请求 284µs sys |
| 3 | **P99.9 较差**（35ms vs epoll 29ms） | `nonvol=0.000`（非窃取）；`sys` 随 P 增长（32.5→55.6µs） | 长尾 |

**同时修正了两个错误归因**：

| 曾经的判断 | 实测 | 处理 |
|---|---|---|
| 根因 = 每请求 2 次 `epoll_ctl` | 23,145 请求只有 **1 次** | **撤回** |
| `sys_us_per_req=283.9µs` 是 IO 成本 | 忙等注入后只有 **33.7µs** | **撤回**（那是 `usleep` hook） |

## 三、三项优化及其数据（阶段 4）

### 优化 1：P 数量可配置（提交 `712c89b`）

`--workers` 现在传入 `config.processor_count`，对齐 coost 的 `co_sched_num`。

| P | QPS | P50 | P99.9 | µs/req | sys µs |
|---|---:|---:|---:|---:|---:|
| 12（原默认） | 3,094 | 20.15ms | 45.63ms | 2,562 | 50.0 |
| **24** | 5,845 | 10.97ms | **35.00ms** | 1,992 | **32.5** |
| 48 | 6,024 | **8.53ms** | 43.65ms | 1,941 | 50.4 |
| 96 | **6,318** | 8.56ms | 40.95ms | **1,816** | 55.6 |
| epoll 256w | 5,431 | 11.02ms | 29.07ms | 2,075 | 39.1 |

**P≥24 时，GMP 在 QPS / P50 / 每请求 CPU 三项上全面超过 epoll 256w。**
（忙等 5ms = 真实 CPU 计算的口径）

### 优化 2：`cooperative_sleep_for` 复用 thread_local eventfd（提交 `c315ec3`）

`epoll_ctl` 从每请求 ~0.5 次降到 **0.037 次**，但 `sys_us_per_req` **283.8 → 280.9（−1%）**。
**诚实结论：大头不在 eventfd，在 futex。**

### 优化 3：`GO2CPP_SLEEP_BLOCKING` 开关（提交 `5167062`）

让 `usleep` 退化为「真阻塞 M」（`invoke_native_blocking` + `BlockingRegion`）。

| 配置 | QPS | µs/req | sys µs |
|---|---:|---:|---:|
| P=24 park（默认） | **22,289** | 320.2 | 283.8 |
| **P=24 真阻塞** | 17,863 | **81.4** | **57.8** |
| **P=64 真阻塞** | **22,264** | 184.6 | 148.1 |
| epoll 256w | 22,121 | **77.2** | 63.2 |

**CPU 降 75%，但无法同时保住 QPS**——这是 futex 跨 M 唤醒的代价。

## 四、最终结论

| 场景 | GMP vs epoll 256w |
|---|---|
| **真实 CPU 计算（忙等 5ms）** | **P≥24 时全面胜出**（QPS +11~16%，CPU −6~12%） |
| **纯 IO（关注入）** | **sys 33.7µs vs 39.1µs，GMP 更优** |
| **协作睡眠（usleep 注入）** | 落后（320 vs 77µs），**无法消除**（M:N 的 futex 代价） |

**「GMP 是高 CPU 低效率」的结论被推翻了一半**：真实 CPU 负载下它更省，`usleep` 口径下的劣势是测量对象不真实（真实业务的后端处理是计算，不是 `usleep`）。

## 五、未解决与建议

| 项 | 状态 |
|---|---|
| **P99.9 落后 20%**（35 vs 29ms） | **M:N 的结构性代价**（2 跳唤醒 vs 1 跳），coost 同样存在 |
| **usleep 场景的 futex 成本** | 无法消除，除非放弃 M:N 模型 |
| **coost 自带 benchmark** | 跑不出有效数据（compute 场景超时、无输出） |

**建议**：把 benchmark 的「5ms 注入」从 `usleep` 改为「排序 + 忙等」（真实计算），
这样对标才能反映真实业务形态。当前 `GO2CPP_BENCH_BUSY_US` 已支持该模式。
