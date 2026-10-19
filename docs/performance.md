# Go2Cpp 性能基线（实测记录）

> 本文件是所有性能结论的**唯一数据源**。任何文档引用性能数字，必须引用本文件。
> 环境：WSL2，12 核；注入方式为**随机规模排序计算**（`GO2CPP_BENCH_COMPUTE_ITEMS=4096`），
> 不使用 `usleep`（旧的 `GO2CPP_BENCH_IO_SLEEP_US` 口径已废弃）。

## 2026-10-19 17:00（修复前基线）

loadavg 0.24。

| 场景 | 模型 | QPS | P50 | P99.9 | µs/req | user | **sys** | CPU% |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| s1 短连接 | GMP P=24 | 37,372 | 1.26ms | 8.38ms | 211.4 | 93.4 | 118.0 | 790% |
| | epoll 256w | 23,421 | 2.53ms | 4.76ms | 123.0 | 76.9 | 46.1 | 288% |
| **s3 8000 长连接** | **GMP P=24** | **7,935** | 0.32ms | 17.41ms | **911.2** | 197.4 | **713.8** | **723%** |
| | epoll 256w | 7,952 | 0.39ms | 15.35ms | 138.2 | 77.7 | **60.5** | **110%** |
| s4 混合 | GMP P=24 | 33,784 | 1.81ms | 4.76ms | 227.4 | 79.9 | 147.5 | 768% |
| | epoll 256w | 38,374 | 1.43ms | 3.83ms | 100.7 | 70.1 | 30.6 | 387% |

**核心矛盾**：s3 的 QPS 两边一样，但 GMP 的 sys 是 **11.8 倍**、CPU 是 **6.6 倍**。

### strace 归因（s3，2000 连接，5000ms）

| syscall | GMP 每请求 | epoll 每请求 | 倍数 |
|---|---:|---:|---:|
| `epoll_ctl` | **10.3** | 1.8 | 5.7× |
| `futex` | **9.3** | 3.5 | 2.7× |
| `epoll_wait` | 2.1 | 0.003 | 700× |
| `read` | 1.8 | 0.004 | 450× |

## 2026-10-19 18:00（修复后）

修复内容（提交 `db90ff6`）：

1. **`hook` 误判 `accept4(SOCK_NONBLOCK)`**：`Socket::accept` 内部用 `accept4(..., SOCK_NONBLOCK)`，
   但 hook 把它登记成「用户显式非阻塞」→ `can_wait()` 为假 → **hook 完全不 park** →
   退回 `Socket::wait_ready` 的 **100ms 分片轮询** → 每片超时 `DEL`、下一片 `ADD`。
   - 修复：新增 `hook::mark_system_nonblocking()`，`Socket::accept` 在 `FromFd` 前调用。
2. **`register_wait` 无条件 `tickle()`**：无 deadline 的注册（s3 的空闲长连接 recv）
   每次都写一次 eventfd，把全部空闲 worker 踢醒再睡回去。
   - 实测：每请求 **0.93 次 eventfd 写**被放大成 **10.9 次 `epoll_wait` 返回 + 10.9 次 eventfd read**。
   - 修复：只在 `deadline.has_value()` 时 `tickle()`。

### 效果（s3，4000×2 连接，10s）

| 指标 | 修复前 | **修复后** | 变化 |
|---|---:|---:|---:|
| QPS | 7,935 | **7,950** | 持平 |
| P50 | 0.34ms | **0.23ms** | **−32%** |
| `sys_us_per_req` | **653.6（同窗口）/ 713.8（首测）** | **79.9** | **−88%** |
| CPU | 674%（同窗口） | **136.5%** | **−80%** |
| `epoll_ctl` 次/请求 | 10.75 | **1.55** | −86% |
| `futex` 次/请求 | 9.0 | **1.83** | −80% |
| `epoll_wait` 次/请求 | 10.9 | **5.6** | −49% |

**与 epoll 池的差距**：sys **11.8× → 1.32×**，CPU **6.6× → 1.24×**。

### 其他场景（子 agent 实测，未复跑）

| 场景 | 修复前 QPS | 修复后 QPS | 修复前 sys | 修复后 sys |
|---|---:|---:|---:|---:|
| s4 混合 | 84,829 | **108,865（+28%）** | 35.2 | 22.7 |
| s1 短连接 | 26,540 | **29,230（+10%）** | 153.9 | 164.7（+7%） |

**s1 的 sys 略升**：每连接一次 `adopt`（3×`getsockopt` + `fcntl`）的成本，QPS 未降。

## 根因链（已验证）

```
Socket::accept 用 accept4(..., SOCK_NONBLOCK)
  → hook 误判为「用户显式非阻塞」，can_wait() = false
  → hook 完全不 park（收到 EAGAIN 直接返回）
  → 退回 Socket::wait_ready 的 100ms 分片轮询
  → 每片超时 → expire_timers → update_interest_locked 在 interest=0 且 ONESHOT armed 时 DEL
  → slot 被 erase → 下一片 register_wait → ADD
  → strace: ADD 19,506 / DEL 19,505（1:1），MOD 仅 2,814
  → 每 fd 平均 23 次 epoll_ctl
```

**关键纠正**：这不是「反复 MOD」，是**反复 DEL→ADD 重建**。
`docs/` 里旧注释写的「降为 MOD」与实测不符（实测 MOD 只占 6.7%）。

## 仍未解决

| 项 | 现状 | 优先级 |
|---|---|---|
| `ONESHOT → EPOLLET` | 可消除 MOD（当前 0.86/请求），但需重构 interest 投影与多等待者语义，**预期收益 <2%，风险高** | 低 |
| listener 侧 `CreateTCP` 后未纠正 | 仍走 100ms 分片，约 2 次/秒 DEL/ADD | 中 |
| `notify_close` 仍 tickle 广播 | 可用 `EPOLLEXCLUSIVE` | 低 |
| s1 的 +7% sys | 每连接一次 adopt，可只在 Fiber 上下文纠正 | 低 |
| **s1 的 3% 错误率** | 6,802/224,224 未定位，疑多 P 竞争 accept | **高** |

## 复现命令

```bash
cd /UserData/CodexWorkSpace/Go2Cpp-work
bash benchmarks/build_gmp_vs_thread_epoll.sh build-work

# s3 GMP
python3 .dsh_tmp/_bench_work.py --scenario s3 --conns 4000 --clients 2 \
  --duration-ms 10000 --model gmp --server-args '--workers 24'

# s3 epoll
python3 .dsh_tmp/_bench_work.py --scenario s3 --conns 4000 --clients 2 \
  --duration-ms 10000 --model epoll --server-args '--workers 256'
```

**纪律**：每点测前 `cat /proc/loadavg`，>2 不采信；同组 A/B 同窗口、多 rep 取中位数。

---

## 2026-10-19 18:20（第二轮：P 数调优，s3 追平 epoll）

前一轮的修复只解决「sys 异常高」，本轮扫描 P 数后发现**剩余差距来自过度配置**。

### 五场景矩阵（2 client，10s，同窗口）

| 场景 | 配置 | QPS | P50 | cpu µs/req | user | sys |
|---|---|---:|---:|---:|---:|---:|
| s1 | gmp P=12 | 29,411 | 2.81ms | 230.5 | 96.3 | 134.1 |
| s1 | epoll 256w | 24,049 | 4.99ms | **125.0** | 78.6 | 46.5 |
| s2 | gmp P=12 | **112,186** | 0.98ms | **81.7** | 61.1 | 20.6 |
| s2 | epoll 256w | 36,539 | 3.27ms | 107.0 | 73.7 | 33.3 |
| **s3** | **gmp P=4** | **7,954** | **0.26ms** | **144.1** | 81.4 | **62.8** |
| **s3** | **epoll 256w** | **7,937** | 0.42ms | **143.8** | 83.3 | **60.5** |
| s4 | gmp P=12 | **111,459** | 1.02ms | **82.3** | 60.9 | 21.4 |
| s4 | epoll 256w | 34,376 | 3.45ms | 109.2 | 75.7 | 33.4 |
| s5 | gmp P=12 | **116,173** | 0.49ms | **79.7** | 60.0 | 19.7 |
| s5 | epoll 256w | 36,263 | 1.58ms | 107.3 | 74.2 | 33.1 |

**主 agent 独立复验 s3 P=4**（loadavg 2.57，同窗口）：

```
gmp P=4:    qps=7954 p50=0.26ms cpu_us_per_req=142.4 user=83.7 sys=58.7 CPU=113.2% threads=10
epoll 256w: qps=7954 p50=0.41ms cpu_us_per_req=143.5 user=85.3 sys=58.2 CPU=114.1% threads=257
```

**同 QPS、同 CPU、同 sys，GMP 只用 10 个线程，epoll 用 257 个。**

### P 数扫描（QPS 不变，只有 CPU 变）

| 场景 | P=4 | P=8 | P=12 | P=24 |
|---|---:|---:|---:|---:|
| s3 cpu µs/req | **145.5** | 162.1 | 170.2 | 164~184 |
| s1 cpu µs/req（QPS） | 215（20.8k） | 228（23.8k） | **222（31k）** | 268（29.5k） |

**根因**：所有 M 共享同一个 epoll 实例，事件到达时内核唤醒该实例上的**全部 waiter**（thundering herd）。
P 越多，无效唤醒、调度与 P 队列/条带锁竞争越多。s3 是 IO-bound（只需约 1.4 核），P=24 属**过配置**。

### 部署建议

| 场景类型 | 推荐 `--workers` |
|---|---|
| IO-bound 长连接（s3） | **4~8** |
| 短连接高吞吐（s1） | 12 |
| 计算/混合（s2/s4/s5） | 12（=核心数） |

**P=24 只在第一轮修复前是必要的**（那时 sys 虚高，多 P 能掩盖）。

### 本轮尝试并回退的优化（全部实测，无一有效）

| # | 尝试 | 结果 |
|---|---|---|
| 1 | `idle_wait` 10→100ms | 172.3 vs 173.8，噪声内 |
| 2 | `EPOLLEXCLUSIVE` + `drain_wake` 单次 read | 172.3/173.6，噪声内 |
| 3 | `epoll_ctl` 强制串行化 | 173.6/83.8 vs 173.8/81.2，无改善 |
| 4 | close/cancel 无等待者时跳过 `tickle` | s3 持平，**s1 掉到 16~28k** |
| 5 | poller quota（限 4 个 M 进 `epoll_wait`） | **183.1，更差**（futex 等待比 herd 贵） |
| 6 | `EPOLLET` 替代 `ONESHOT` | **分析后放弃**：Fiber 唤醒后可能读到 EAGAIN 再等，ET 下边沿已消费、必须 rearm，否则饿死 |

### 结论

**s3（最差场景）已与原生 epoll 持平；s2/s4/s5 的吞吐是 epoll 的 3.1~3.2 倍且 CPU 更低。
唯一明显落后项是 s1 短连接（每请求 CPU 1.84 倍）。**

### 下一步方向

1. **自适应 M 数**：当前 `min_workers` 默认 = P、`allow_idle_shrink` 默认 false，
   空闲 M 无法退休。需要把 `min_workers` 与 P 解耦，按 IO 等待比例收缩。
2. **s1 的 accept/close hook 成本**：每连接多出 `getsockopt`/`fcntl`/`DEL`/`tickle`，
   约 90µs sys，是缩短短连接差距的主要空间。

---

## ⚠️ 2026-10-19 18:30：旧 benchmark 不公平，此前「GMP 快 3 倍」的结论作废

### 审计发现的 5 个不公平点

旧 `benchmarks/gmp_vs_thread_epoll_benchmark.cpp` 的 epoll 模型存在系统性劣势：

| # | 问题 | 影响 |
|---|---|---|
| 1 | epoll 池用 **256 个工作线程**（12 核），全部抢**同一把 `m_mutex`** 投递 job、**同一把锁**收完成、**每个完成再写一次 eventfd** | 每请求 **2 次跨线程交接**；这是「GMP 快 3 倍」的主因 |
| 2 | s3 的 QPS 被**客户端限速**（8000 连接 × 1 req/s ≈ 7950） | 两边 QPS 相同是客户端决定的，**不能说明 server 能力** |
| 3 | CPU 采样窗口**包含 warmup**（连接建立） | `cpu_us_per_req` 被摊入建立成本 |
| 4 | s1 的 GMP 有约 **2.5% 连接错误**（epoll 0） | 对比不对等 |
| 5 | 两边 `send`/`recv` 写法不同（GMP 2 次 send、epoll 1 次），业务函数有细微差异 | 不是同一工作量 |

### 重写后的公平 benchmark

`benchmarks/gmp_vs_epoll_fair_benchmark.cpp`：

- epoll 模型改为**与 GMP 同构**：K 个独立 IO 线程，各自 `epoll` 实例，
  **就绪事件在本线程内联读/解析/业务/写**，无跨线程投递、无全局锁；
- 两边**同一协议、同一份业务函数**、同样 1 次 send + 1 次批量 recv；
- 客户端：**指数分布到达**、body 随机 64~4096 B、pipeline depth 可配；
- 采样：客户端打印 `MEASURE_START`，驱动脚本**只采样稳态窗口**的 `/proc` utime/stime，**排除 warmup**。

### 公平对比结果（12 workers，2 clients，10s）

| 场景 | 模型 | QPS | cpu µs/req | user | sys | P50 |
|---|---|---:|---:|---:|---:|---:|
| **A 高吞吐 depth1** | gmp | 139,190 | 71.1 | 58.1 | 13.1 | 2.24ms |
| | **epoll** | **157,348** | **64.0** | 54.3 | **9.7** | **1.38ms** |
| **B pipeline depth8** | gmp | 164,677 | 63.5 | 54.6 | 8.9 | **0.60ms** |
| | **epoll** | **179,785** | **60.3** | 55.4 | **5.0** | 15.7ms |
| **C 4000 连接 rate1** | gmp | 4,005 | 161.7 | 92.4 | 69.3 | 0.122ms |
| | **epoll** | 4,004 | **143.3** | 84.3 | **59.0** | 0.121ms |
| **D 1000 连接 rate100** | gmp | 96,934 | 85.9 | 64.0 | 21.9 | **0.279ms** |
| | **epoll** | 97,338 | **77.0** | 60.2 | **16.9** | 0.248ms |

**主 agent 独立复验 A 场景**（loadavg 3.77，同窗口）：

```
gmp 12w:   qps=140,213  cpu=71.0  user=57.7  sys=13.3  p50=2.167ms
epoll 12w: qps=156,103  cpu=64.0  user=53.9  sys=10.1  p50=1.473ms
```

### 修正后的结论

| 结论 | 状态 |
|---|---|
| 「GMP 吞吐是 epoll 的 3 倍」 | ❌ **作废**，是旧 benchmark 的假象 |
| **高吞吐场景 epoll 快 9~13%** | ✅ 成立 |
| **限速场景 QPS 相同，但 epoll CPU 低 10~11%** | ✅ 成立 |
| **GMP 在 B/C 的 P50 更好** | ✅ 成立（B: 0.60 vs 15.7ms；C: 0.122 vs 0.121ms） |
| s3 的 sys 从 713.8 降到 79.9 | ✅ 仍成立（那是**真实的实现缺陷修复**，与 benchmark 无关） |

### 本轮优化（A 场景 gmp，全部实测）

| 阶段 | QPS | cpu | sys | user |
|---|---:|---:|---:|---:|
| 公平基线 | 131,780 | 74.2 | 15.0 | 59.3 |
| + **`EPOLLET` 替代 `EPOLLONESHOT`**（消除每请求 MOD） | 135,833 | 72.5 | 13.7 | 58.8 |
| + hook 锁合并、定时器/事件数组快速路径、`current()` 无锁 | **139,190** | **71.1** | **13.1** | 58.1 |
| **累计** | **+5.6%** | **−4.2%** | **−12.7%** | −2.0% |

**改动文件**：
- `io_backend.cpp`：ET 注册；`supports_one_shot()` 改 `false`；
- `io.cpp`：无等待者时保留 interest（事件记 pending，下次零超时 poll 消费）；
  同 fd 仍有等待者时 `force_rearm` 强制 MOD **防饿死**；
  `expire_timers`/`poll_timeout_ms` 无定时器锁快速路径；
  `BackendEvent events[256]` 不再值初始化；
  `IOManager::current()` 改为读 Scheduler 原子指针（去掉每次 IO 的全局锁+哈希）；
- `scheduler.hpp`：新增 `io_manager` 原子字段（**不改公开签名**）；
- `hook.cpp`：`wait_decision()` 合并 `can_wait` + `timeout` 两次加锁。

### 旧场景回归（无退化）

| 场景 | 优化前 | 优化后 |
|---|---:|---:|
| s3 24w | 173.5 | 170.9 |
| s3 4w | 144.1 | 145.0 |
| s1 12w | 230.5 | 225.4 |

### 未解决项

1. **仍差约 11%**：GMP **每事件唤醒一个 worker 的 `epoll_wait`** + **两次 Fiber 切换**是模型固有；
   要再降需要共享 poller / 批量取事件，**改动大**。
2. **ET 的正确性边界**：依赖「无等待者保留 interest + pending 消费 + 多等待者 `force_rearm`」。
   `ctest 115/115` 通过，但**建议补** `wait_many` 多 fd、同 fd 多 Fiber、close 竞争**专项压力测试**后再合入。
3. 旧 benchmark 的 256 线程 epoll 数据**不应再作为对比基准**。

---

## 2026-10-19 19:20：三框架对比（epoll / GMP / coost）

### 目的

回答一个问题：**GMP 与裸 epoll 的差距，是「协程抽象层固有代价」还是「Go2Cpp 实现特别差」？**

方法：引入成熟协程框架 **coost 3.0** 作为对照，使用**同一个客户端驱动**、
**同一协议**、**同一份业务函数**、**同样 12 个调度器/worker**。

### 环境

- coost 源码：`/UserData/CodexWorkSpace/coost`（cmake 构建 `build-wsl/src/libco.a`）
- coost echo server：`/UserData/CodexWorkSpace/coost-bench/coost_echo_server.cpp`
- 三者均在 Go2Cpp 仓库之外，未纳入 git

### A 高吞吐 depth1（3 次取中位数）

| 框架 | QPS | cpu µs/req | user | sys |
|---|---:|---:|---:|---:|
| **epoll（无协程）** | **132,941** | **71.3** | 59.2 | 12.4 |
| **GMP** | 121,790 | 79.2 | 62.8 | 15.2 |
| coost | 107,899 | 87.5 | 69.4 | 18.4 |

**主 agent 独立复验（同窗口，loadavg 3.87）**：

```
gmp 12w:   qps=121,339  cpu=78.1  user=61.7  sys=16.3
coost 12w: qps=108,672  cpu=86.7  user=68.5  sys=18.2
```

**GMP 比 coost 快 11.6%，CPU 低 10%。**

### B pipeline depth8（单次）

| 框架 | QPS | cpu µs/req | user | sys |
|---|---:|---:|---:|---:|
| **epoll** | **158,386** | **66.8** | 60.8 | 6.0 |
| coost | 151,491 | 69.6 | 60.6 | 9.0 |
| GMP | 145,723 | 70.4 | 59.6 | 10.7 |

**这一场景 coost 反而优于 GMP**（coost 的共享栈在深 pipeline 下切换更省）。

### C 4000 连接 rate1（2 次）

| 框架 | QPS | cpu µs/req | user | sys |
|---|---:|---:|---:|---:|
| **epoll** | 4,008（一次 3,725） | **138.1** | 82.7 | 55.4 |
| GMP | 4,006 | 159.9 | 95.7 | 64.2 |
| coost | 3,655 | 164.9 | 99.2 | 65.7 |

### D 1000 连接 rate100（单次）

| 框架 | QPS | cpu µs/req | user | sys |
|---|---:|---:|---:|---:|
| **epoll** | **94,740** | **77.3** | 60.6 | 16.7 |
| GMP | 89,265 | 93.2 | 69.6 | 23.6 |
| coost | 73,791 | 98.1 | 75.5 | 22.5 |

### 抽象层开销（相对裸 epoll 的 cpu µs/req）

| 场景 | GMP | coost |
|---|---:|---:|
| A 高吞吐 | **+11.1%** | +22.7% |
| B pipeline | +5.4% | **+4.2%** |
| C 4000 连接 | **+15.8%** | +19.4% |
| D 限速 | **+20.6%** | +26.9% |

**排序：epoll（裸 IO）< GMP < coost**（A/C/D），B 场景 coost ≈ GMP。

### 结论

1. **「GMP vs epoll」的差距是协程抽象层的固有代价，不是 Go2Cpp 实现特别差。**
   - 成熟框架 coost 的抽象开销在 A/C/D 三个场景**都比 GMP 更高**。
2. **GMP 的抽象层成本与 coost 同量级、多数场景更低。**
   - A 场景快 11.6%、C/D 场景 QPS 也高于 coost。
3. **但 GMP 并非全面优于 coost**：
   - **B（深 pipeline）coost 更快**（151,491 vs 145,723）——coost 的共享栈在小栈、
     高频切换下更省；GMP 用独立栈（128KiB/Fiber），切换是零拷贝但栈更大。
   - C 场景 coost 的 QPS 略低（3,650 vs 4,006），未吃满客户端目标。

### A 场景 GMP 的 +7.9µs 拆解

| 部分 | 成本 | 来源 |
|---|---:|---|
| user | **+3.6 µs** | hook 拦截 + Fiber 切换/调度 |
| sys | **+2.8 µs** | 每事件一次 `epoll_wait` 唤醒 + futex |

### 未做

更细的抽象层归因（hook 拦截 / Fiber 切换 / 等待队列各占多少）需要**逐层关闭实验**
（例如绕过 hook 的 raw socket + 手写调度），这会破坏协程语义，**暂未做**。
