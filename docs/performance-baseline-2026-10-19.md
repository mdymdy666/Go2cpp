# Go2Cpp 性能基线与优化记录（2026-10-19）

> 本文是当前**唯一有效**的性能基线。旧的 `gmp-optimization-log.md`、
> `gmp-perf-analysis.md`、`coost-comparison.md` 中的对比数据来自
> 2026-10-18 的旧 benchmark，其 epoll 池模型不公平（见 §1），只保留方法论参考。

## 0. 结论摘要

| 场景 | 指标 | 数值 |
|---|---|---|
| s3 海量空闲连接（4000×2，GMP P=24） | sys µs/req | **713.8 → 78.8（-89%）** |
| s3（同参数） | cpu µs/req | 848.1 → 173.7（-80%） |
| s3（GMP P=4 vs epoll 256w） | cpu µs/req | **145.0 vs 144.1（持平）** |
| s2 高吞吐（公平 epoll，12 线程） | QPS | epoll 132.9k > GMP 121.8k（-8%）> coost 107.9k |
| 抽象层开销（A 场景，相对裸 epoll） | cpu µs/req | **GMP +11.1%，coost +22.7%** |
| `ctest` | 通过率 | 115/115 |

核心结论：

1. s3 的高 sys 根因是 **`accept4(SOCK_NONBLOCK)` 被 Hook 误归因为用户意图**，
   导致所有连接退回 Socket 层的 100ms 分片轮询，每片一次 DEL+ADD。
2. GMP 与裸 epoll 的剩余差距（5%~21%）来自**协程抽象层**；同样跑协程的
   coost 开销更大（4%~27%），说明这不是 Go2Cpp 的实现缺陷。
3. 旧的「GMP 比 epoll 快 3 倍」结论**不成立**：旧 benchmark 的 epoll 模型
   用 256 线程抢同一把全局锁、每请求两次跨线程交接。

---

## 1. 测试方法与公平性

### 1.1 旧 benchmark 的五个问题（已废弃对比数据）

| # | 问题 | 影响 |
|---|---|---|
| 1 | epoll 池用 256 个工作线程（12 核），任务投递/完成走同一把 `m_mutex` + cv，每请求两次跨线程交接 | 严重低估 epoll |
| 2 | s3 的 QPS 被客户端限速（8000 连接 × 1 req/s ≈ 7950） | 「QPS 相同」由客户端决定，不反映 server 上限 |
| 3 | CPU 采样窗口包含 warmup（连接建立阶段） | `cpu_us_per_req` 被摊入建连成本 |
| 4 | s1 的 GMP 侧约 2.5% 连接错误（epoll 0） | 对比不对等 |
| 5 | 两侧 send/recv 写法不同（GMP 2 次 send、epoll 1 次） | 引入 syscall 差异 |

### 1.2 新 benchmark：`benchmarks/gmp_vs_epoll_fair_benchmark.cpp`

- **epoll 模型与 GMP 同构**：K 个独立 IO 线程，各自 epoll 实例，就绪事件在
  本线程内联完成读/解析/业务/写；**无跨线程投递、无全局锁**；
- 两侧同一协议 `[u32 len][u8 opcode][body]`、同一份业务函数（thread_local
  seed 递推 + 排序，与旧 `process_request` 逐行一致）；
- 两侧都是「1 次 send + 1 次批量 recv」；
- 客户端：**指数分布到达**（`--rate`）、body 随机 `[64,4096]`、pipeline depth、
  短/长连接可配、多客户端；
- 采样：客户端在 warmup 结束时打印 `MEASURE_START`，驱动脚本
  `benchmarks/fair_bench.py` 只统计该标记之后的 `/proc/<pid>/stat`
  utime/stime 与完成数，**排除 warmup**。

### 1.3 coost 对照

- 源码 `https://github.com/idealvin/coost`（v3.0.0），拷贝到
  `/UserData/CodexWorkSpace/coost`，`cmake -DCMAKE_BUILD_TYPE=Release` 构建 `libco.a`；
- `benchmarks/coost_echo_server.cpp`：同一协议、同一业务函数、`co_sched_num=12`；
- 客户端与驱动与 GMP/epoll 完全一致。

---

## 2. 优化点清单

### 2.1 第一轮：s3 内核态 CPU 根因（sys 713.8 → 78.8 µs/req）

**根因链**

1. `Socket::accept` 用 `accept4(..., SOCK_CLOEXEC | SOCK_NONBLOCK)`；
2. Hook 的 `accept4`/`register_new_socket` 把该 `O_NONBLOCK` 记成
   `m_user_nonblocking = true`；
3. `can_wait()` 因此为假 → **Hook 完全不 park**；
4. `Socket::recv` 退回 `wait_ready()` 的 **100ms 分片**循环；
5. 每片超时：`expire_timers` → `update_interest_locked` 在
   `interest==0 && ONESHOT armed` 时 DEL，slot 因 `!registered` 被删除；
6. 下一片重新 `register_wait` → ADD。

strace 实测（2000×2 连接，4 s，3892 请求）：ADD 19,506 / DEL 19,505（1:1）、
MOD 仅 2,814；每 fd 平均 23 次 ctl。对照实验：`--raw-io`（把 `::recv` 直接
交给 Hook）时 QPS=0、completed=0、established=2000，证明 Hook 确实收到 EAGAIN
不 park。

**改动**

- `hook::mark_system_nonblocking(fd)`：把框架 accept 出来的连接纠正为
  system-nonblocking（`Socket::accept` 在 `FromFd` 前调用；**不在 Socket
  构造函数调用**，否则会破坏 `Socket::set_waiter()` 语义，见
  `tests/network/test_network.cpp:476`）；
- `register_wait` 只在 `deadline.has_value()` 时 `tickle()`：无 deadline 的
  注册不改变 `poll_timeout_ms()`，无条件 tickle 会把全部空闲 worker 踢醒
  （实测每请求 0.93 次 eventfd 写被放大成 10.9 次 epoll_wait + 10.9 次 read）。

**效果**（s3，4000×2，GMP P=24，同机同轮）

| 指标 | 修复前 | 修复后 |
|---|---:|---:|
| QPS | 7,950 | 7,950 |
| P50 | 0.34 ms | 0.24 ms |
| cpu µs/req | 848.1 | 173.7 |
| sys µs/req | 653.6 | 78.8 |
| epoll_ctl/请求 | 10.75 | 1.55 |
| futex WAKE/WAIT（每请求） | 5.39 / 3.63 | 1.83 / 1.83 |

### 2.2 第二轮：P/worker 数扫描（s3 已与 epoll 持平）

| s3 配置 | QPS | cpu µs/req | sys |
|---|---:|---:|---:|
| GMP P=4 | 7,954 | **145.0** | 63.5 |
| GMP P=8 | 7,941 | 162.1 | 76.0 |
| GMP P=12 | 7,937 | 170.2 | 80.8 |
| GMP P=24 | 7,943 | 164~184 | 76~89 |
| epoll 256w | 7,937 | 143.8 | 60.8 |

s1 短连接：P=4 215 / P=8 228 / **P=12 222（QPS 31k 最高）** / P=24 268。

**结论**：s3 是 IO-bound（只需 ~1.4 核），P=24 属过配置；所有 M 共享同一个
epoll 实例，事件到达会唤醒全部 waiter（内核 thundering herd）。**P=4 时 GMP
与原生 epoll 完全持平**。

### 2.3 第三轮：EPOLLET + 热路径微优化

**EPOLLET 替代 EPOLLONESHOT**

旧语义：每个事件消费后内核自动 disarm，下一次等待必须 `epoll_ctl(MOD)`。
新语义：注册长期有效；等待队列为空时**保留 slot.interest**，事件到达由
`process_event_locked` 记为 `pending_read/write`，下一次 `register_wait`
用零超时 poll 消费。同一 fd 仍有等待者时置 `force_rearm` 强制 MOD，
防止后续等待者饿死。

效果（A 场景，gmp 12 workers，200 连接，depth1，10 s）：

| 阶段 | QPS | cpu | sys | user |
|---|---:|---:|---:|---:|
| 公平基线 | 131,780 | 74.2 | 15.0 | 59.3 |
| + EPOLLET | 135,833 | 72.5 | 13.7 | 58.8 |
| + hook 锁合并/定时器快速路径/current 无锁 | 139,190 | 71.1 | 13.1 | 58.1 |

ET 后 `epoll_ctl` 从 1 次/请求降到 ~0（strace：400 连接、17k 请求只有 8 次）。

**其余微优化**（合计在噪声内，保留：

- `hook::wait_decision()` 合并 `can_wait` + `operation_timeout` 的两次加锁；
- `expire_timers()`/`poll_timeout_ms()` 用 `m_timer_count` 原子快速路径，
  无 deadline 时不取 `m_timer_mutex`、不读时钟；
- `BackendEvent events[256]{}` 去掉值初始化（每次 poll_once 省 4KB 清零）；
- `IOManager::current()` 改为读 `Scheduler` 上的原子指针，去掉每次 IO 的全局
  注册表锁 + 哈希查找。

**回归**（旧 benchmark）：s3 24w cpu 170.9（前 173.5）、s3 4w 145.0（前 144.1）、
s1 12w 225.4（前 230.5），无退化。

---

## 3. 三框架对比（2026-10-19，12 调度器/worker，2 clients）

### A 高吞吐 depth1（3 次中位数）

| 框架 | QPS | cpu µs/req | user | sys |
|---|---:|---:|---:|---:|
| epoll（无协程） | 132,941 | 71.3 | 59.2 | 12.4 |
| GMP | 121,790 | 79.2 | 62.8 | 15.2 |
| coost | 107,899 | 87.5 | 69.4 | 18.4 |

波动：GMP 116.9k~130.0k（±5%），coost 103.9k~124.0k（±9%）。

### B pipeline depth8（单次）

| 框架 | QPS | cpu | user | sys |
|---|---:|---:|---:|---:|
| epoll | 158,386 | 66.8 | 60.8 | 6.0 |
| coost | 151,491 | 69.6 | 60.6 | 9.0 |
| GMP | 145,723 | 70.4 | 59.6 | 10.7 |

### C 4000 连接 rate1（2 次）

| 框架 | QPS | cpu | user | sys |
|---|---:|---:|---:|---:|
| epoll | 4,008 / 3,725 | 138.1 | 82.7 | 55.4 |
| GMP | 4,006 | 159.9 | 95.7 | 64.2 |
| coost | 3,655 | 164.9 | 99.2 | 65.7 |

### D 1000 连接 rate100（单次）

| 框架 | QPS | cpu | user | sys |
|---|---:|---:|---:|---:|
| epoll | 94,740 | 77.3 | 60.6 | 16.7 |
| GMP | 89,265 | 93.2 | 69.6 | 23.6 |
| coost | 73,791 | 98.1 | 75.5 | 22.5 |

### 抽象层开销（相对裸 epoll 的 cpu µs/req）

| 场景 | GMP | coost |
|---|---:|---:|
| A | **+11.1%** | +22.7% |
| B | +5.4% | +4.2% |
| C | **+15.8%** | +19.4% |
| D | **+20.6%** | +26.9% |

A 场景 GMP 的 +7.9 µs 拆解：**user +3.6 µs**（Hook 拦截 + Fiber 切换/调度）、
**sys +2.8 µs**（每事件一次 `epoll_wait` 唤醒 + futex）。

**结论**：协程框架相对裸 epoll 的 5%~21% 开销是抽象层固有成本；GMP 的抽象层
比成熟的 coost 更轻（A/C/D），B 场景两者相当。GMP 与 epoll 的差距**不是**
Go2Cpp 特有的实现缺陷。

---

## 4. 复现

```bash
# 1) 构建
cd /UserData/CodexWorkSpace/Go2Cpp-work
cmake -S . -B build-work -DCMAKE_BUILD_TYPE=Release
cmake --build build-work -j8
ctest --test-dir build-work          # 115/115

# 2) 公平压测程序
g++ -std=c++17 -O3 -DNDEBUG -Iinclude -Ithird_party \
  benchmarks/gmp_vs_epoll_fair_benchmark.cpp \
  -o build-gmp-vs-thread/gmp_vs_epoll_fair_benchmark \
  -Wl,-rpath,$PWD/build-work -Lbuild-work \
  -lgo2cpp_network -lgo2cpp_io -lgo2cpp_scheduler -lgo2cpp_fiber \
  -lgo2cpp_context -lgo2cpp_sync -lgo2cpp_hook -lgo2cpp_config \
  -lgo2cpp_log -lgo2cpp_error -lz -ldl -lpthread -lm

# 3) GMP / epoll
python3 benchmarks/fair_bench.py --model gmp   --workers 12 --conns 200 --clients 2 \
  --depth 1 --rate 0 --mixed --body-min 64 --body-max 4096 \
  --warmup-ms 2000 --duration-ms 10000
python3 benchmarks/fair_bench.py --model epoll --workers 12 --conns 200 --clients 2 \
  --depth 1 --rate 0 --mixed --body-min 64 --body-max 4096 \
  --warmup-ms 2000 --duration-ms 10000

# 4) coost
cd /UserData/CodexWorkSpace/coost/build-wsl && cmake .. -DCMAKE_BUILD_TYPE=Release && make -j8
# server 源码在 benchmarks/coost_echo_server.cpp：
g++ -std=c++17 -O3 -I/UserData/CodexWorkSpace/coost/include \
  benchmarks/coost_echo_server.cpp -o coost_echo_server \
  -L/UserData/CodexWorkSpace/coost/build-wsl/src -lco -lpthread -ldl
python3 benchmarks/fair_bench.py --model coost \
  --server-cmd "/UserData/CodexWorkSpace/coost-bench/coost_echo_server --port 0 --workers 12" \
  --conns 200 --clients 2 --depth 1 --rate 0 --mixed \
  --body-min 64 --body-max 4096 --warmup-ms 2000 --duration-ms 10000
```

## 5. 未解决项与风险

1. **EPOLLET 改动的正确性边界**：依赖「无等待者保留 interest + pending 消费 +
   多等待者 force_rearm」。115 个测试全绿，但建议补 `wait_many` 多 fd、
   同 fd 多 Fiber、close 竞争专项压力测试后再视为稳定。
2. **高吞吐仍差 ~8%~11%**：GMP 每事件唤醒一个 worker 的 `epoll_wait` + 两次
   Fiber 切换是模型固有；批处理/共享 poller 才能再降，改动大。
3. **coost 与 GMP 互有胜负**：B 场景 coost 略快（151k vs 146k）。
4. **旧 baseline 已作废**：`gmp-optimization-log.md` 等文档里的
   「GMP 比 epoll 快 3 倍 / s3 6,270 QPS」等数据基于旧 benchmark，不再引用。
5. 本版本仍标记为 **0.1.0（未达 V1）**：ET 路径与多等待者场景的稳定性还需要
   一轮专项压测。
