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

> 口径：单客户端 200 连接（`--clients 1`）。§3 的三框架 A 场景是 2 clients（400 连接），两组绝对值不可横比。

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

> 延迟分布：depth8 下 GMP p50 0.58ms、p99 154~185ms；epoll p50 15.7ms、p99 26~29ms。
> GMP 吞吐略低但小请求更快，长尾显著更差（Fiber 调度/公平性问题，待查）。

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

## 6. 独立复核（2026-10-19 19:35~19:52，第三方 agent）

同一公平驱动，交替多轮；A 为 200x2 depth1、B 为 200x2 depth8、C 为 8000 连接 rate1、D 为 1000 连接 rate100。

| 场景 | 配置 | GMP QPS 中位 | GMP cpu µs | epoll QPS 中位 | epoll cpu µs | 结论 |
|---|---|---:|---:|---:|---:|---|
| A | 200x2 depth1 | 127,357（124.0k~131.7k） | 72.9 | 137,638（132.1k~152.9k） | 68.3 | epoll +8.1% |
| B | 200x2 depth8 | 163,519（156.9k~165.7k） | 64.1 | 181,191（171.4k~182.2k） | 63.1 | epoll +10.8% |
| C | 8000 rate1, P=4 vs epoll 12w | 7,926 | 127.2 | 7,860 | 129.4 | 持平（-1.7%） |
| D | 1000 rate100 | 120,608（114.5k~126.8k） | 82.0 | 124,082（116.9k~131.3k） | 73.2 | epoll +2.9% |

- 所有轮次 `errors=0`；`completed` 与 `[window] qps` 自洽；独立探针用已知 payload 校验
  echo/read/write/ping 四种 opcode 与 200 帧流式请求，GMP 与 epoll 两个 server 均逐字节匹配，
  证明协议解析、业务入口、读写路径真实工作。
- `ctest` 115/115（61.43s）。
- 波动：A 场景 epoll ±8%、GMP ±3%；C 场景被客户端 8000/s 限速，QPS 恒为 ~7.9k，应以 cpu/sys 为准。
- 本轮复核修正：§2.3 与 §3 的 A 场景口径不同（已补标注）；B 场景 GMP 长尾 p99 154~185ms（已补说明）。

## 7. Go 1.27 对照（2026-10-19 20:05）

### 7.1 方法与公平性

`benchmarks/go_echo_server.go` 与 C++ 公平 benchmark 逐项对齐：

- 同一帧协议 `[u32 LE length][u8 opcode][body]`、同一 `process_body`
  （xorshift seed 递推，`count = seed % 4097`，生成并排序 `count` 个 int32）；
- 同一读取方式：64KB 块读（与 C++ GMP 的 `char buffer[64*1024]` 对齐），
  累积 `in` 与输出 `out` 按需增长（与 `std::string` 对齐，不预分配 64KB）；
- 同一写出方式：每响应 1 次 Write；
- C++ 侧未设 `TCP_NODELAY`（Nagle 开启），Go 侧 `SetNoDelay(false)` 对齐；
- `GOMAXPROCS=12`；GOGC 默认 100；未用 sync.Pool、未做零拷贝优化；
- 客户端为同一份 C++ benchmark client，warmup 由客户端标记排除。

### 7.2 结果（同窗口交替，各 2 轮中位数）

A 高吞吐（200x2, depth1, 10s）：

| 框架 | QPS | cpu µs/req | user | sys | p50 | p99 | RSS |
|---|---:|---:|---:|---:|---:|---:|---:|
| epoll | **149,270** | **65.7** | 55.5 | 10.2 | 1.41ms | 7.03ms | 9.5MB |
| GMP | 139,530 | 72.4 | 59.0 | 13.4 | 2.15ms | 5.59ms | 29MB |
| Go | 117,476 | 88.4 | 76.3 | 12.1 | 1.38ms | 10.70ms | 90MB |

B pipeline depth8（200x2, 10s）：

| 框架 | QPS | cpu µs/req | p50 | p99 | RSS |
|---|---:|---:|---:|---:|---:|
| epoll | **180,499** | **60.4** | 15.66ms | 32.4ms | 22MB |
| GMP | 168,548 | 62.6 | 0.69ms | 122.6ms | 34MB |
| Go | 125,730 | 83.2 | 24.29ms | 96.2ms | 112MB |

C 8000 连接 rate1（10s，QPS 被客户端限速 ~7.9k）：

| 框架 | QPS | cpu µs/req | user | sys | p50 | p99 | RSS |
|---|---:|---:|---:|---:|---:|---:|---:|
| GMP P=4 | 7,400 | **127.8** | 88.4 | 39.4 | 0.218ms | 15.63ms | 488MB |
| epoll 12 | 7,828 | **130.0** | 85.9 | 44.1 | 0.212ms | 0.83ms | 55MB |
| Go 12 | 7,891 | 198.5 | 121.3 | 77.2 | 0.289ms | 0.93ms | **1.08GB** |

### 7.3 C++ 相对 Go 的优势（同工作负载）

| 维度 | A 高吞吐 | B pipeline | C 8000 连接 |
|---|---:|---:|---:|
| C++ epoll QPS 相对 Go | +27.1% | +43.6% | 限速持平 |
| C++ GMP QPS 相对 Go | +18.8% | +34.0% | 限速持平 |
| C++ epoll cpu µs 相对 Go | -25.7% | -27.4% | -34.5% |
| C++ GMP cpu µs 相对 Go | -18.2% | -24.7% | -35.6% |
| Go 的 sys µs（C 场景） | - | - | 77.2 vs 39.4/44.1 |
| C++ RSS 相对 Go | 1/9.5 ~ 1/3.1 | 1/5.1 ~ 1/3.3 | 1/19.6 ~ 1/2.2 |

**C++ 的天然优势**：无 GC、无 runtime 调度器、每请求分配可控，因此高吞吐场景
QPS 高 19~44%、CPU 低 18~36%；8000 连接时 sys 低 43%~49%、RSS 低 2.2~19.6 倍。
**Go 的优势**：A 场景 p50 1.38ms 与 epoll 1.41ms 相当；C 场景 p99 0.93ms 优于
GMP 15.63ms、略逊 epoll 0.83ms；并发模型简单（一连接一 goroutine）。

> RSS 说明：Go 的 1.08GB ≈ 8000x64KB 读缓冲（512MB）+ GOGC=100 的堆余量（~500MB）
> + goroutine 栈/runtime（~64MB）。初版实现每连接预分配 3x64KB（buf/in/out）导致
> 1.94GB，已改为 in/out 按需增长（与 std::string 对齐），内存降 44%、吞吐提升 11%。
> C++ GMP 的 488MB 主要是 8000 个 Fiber 栈与每连接 64KB 读缓冲。

### 7.4 复现

```bash
cd /UserData/CodexWorkSpace/Go2Cpp-work/benchmarks
GOCACHE=/tmp/gocache GO111MODULE=off go build -o /tmp/go_echo_server go_echo_server.go

cd /UserData/CodexWorkSpace/Go2Cpp-work
python3 benchmarks/fair_bench.py --model go --workers 12 \
  --server-cmd "/tmp/go_echo_server --port 0 --workers 12" \
  --conns 200 --clients 2 --depth 1 --rate 0 --mixed \
  --body-min 64 --body-max 4096 --warmup-ms 2000 --duration-ms 10000
```

## 8. 阻塞下游场景：GMP 的 M:N 优势（2026-10-19 20:28）

### 8.1 为什么单列这一节

第 3 节的公平 benchmark 里业务是**纯计算**（`process_body` 排序），epoll 可以完全内联，
没有任何跨线程交接；这是 epoll 模型的最优形态。真实业务通常含一次下游等待
（DB / RPC / 缓存），此时 epoll 线程池必须让 worker 阻塞或把任务交给另一组线程，
而 GMP 的 goroutine 可以在原线程 park、让 M 去处理其他 G。

为公平复现这个差异，给 `gmp_vs_epoll_fair_benchmark` 加 `--downstream-us`：

- GMP 侧：fiber 内 `::usleep(N)`，被 hook 成 `cooperative_sleep_for`（park，让出 M）；
- epoll 侧：worker 线程内 `std::this_thread::sleep_for(N)`（真实阻塞该 worker）；
- 两侧仍是同一协议、同一 `process_body`、同一客户端、同一 worker 数。

`--downstream-us 0` 时与原 benchmark 完全一致（见 8.2 回归）。

### 8.2 回归：downstream 0（确认基线未被破坏）

200x2, depth1, 10s：

| 模型 | QPS | cpu µs/req | p50 | p99 |
|---|---:|---:|---:|---:|
| epoll 12w | **160,035** | **64.2** | 1.44ms | 7.23ms |
| GMP 12P | 134,509 | 70.5 | 2.02ms | 5.46ms |

epoll 仍领先 +19.0%，与第 3 节结论一致。

### 8.3 阻塞 1ms，2000 连接 rate10（目标 20k QPS），12 worker（3 rep 中位）

| 模型 | QPS | p50 | p99 | cpu µs/req | 线程 | RSS |
|---|---:|---:|---:|---:|---:|---:|
| **GMP 12P** | **19,741** | **1.29ms** | **2.44ms** | 254.5 | 18 | 58MB |
| epoll 12w | 10,152 | 173.94ms | 334.0ms | **121.3** | 13 | 22MB |

**GMP 吞吐 1.94 倍、p50 低 134 倍**。epoll 的 12 个 worker 全部被 1ms 睡眠占满，
新请求只能排队（p50 173ms）；GMP 的 12 个 P 通过 park 承载约 20 个并发等待，
每个请求的阻塞不占用线程。代价是 GMP 的 cpu µs/req 高 2.1 倍（park/wake 的内核路径）。

### 8.4 公平性对照：epoll 用同样的 24 线程（2 rep 中位）

| 模型 | QPS | p50 | p99 | cpu µs/req | 线程 |
|---|---:|---:|---:|---:|---:|
| GMP 12P（上表） | 19,741 | **1.29ms** | **2.44ms** | 254.5 | 18~25 |
| epoll 24w | 18,930 | 10.40ms | 43.96ms | **116.3** | 25 |

epoll 把 worker 翻倍到 24 后吞吐基本追平（-4.3%），但 **p50 仍差 8.0 倍、p99 差 18 倍**，
且稳态占用 25 个线程（GMP 在 18~25 之间按负载伸缩）。也就是说：epoll 可以用
「线程数 = 并发等待数」换吞吐，GMP 用「park」把线程数压到远低于并发等待数。

### 8.5 大数据量：8000 连接 rate10（目标 80k QPS），12 worker（2 rep 中位）

| 模型 | QPS | p50 | p99 | cpu µs/req | 线程 | RSS |
|---|---:|---:|---:|---:|---:|---:|
| **GMP 12P** | **59,169** | **66.0ms** | **126.5ms** | 146.4 | 18 | 413MB |
| epoll 12w | 9,723 | 783.1ms | 805.8ms | **120.8** | 13 | 60MB |

**GMP 吞吐 6.09 倍、p50 低 11.9 倍**。8000 连接 x 10 次/秒 = 80k 目标，epoll 的
12 个 worker 仍然只能处理约 12k/s，队列延迟接近 0.8s；GMP 的 P 数不变，但并发
park 数从 20 涨到 80，仍能吸收，QPS 受 12 核 CPU 上限约束（约 59k）。

### 8.6 结论与边界

**GMP 的模型优势在「业务含阻塞等待」时成立且显著**：同 worker 数 1.94~6.09 倍吞吐、
p50 低 134~11.9 倍；同线程数下 epoll 只能追平吞吐，延迟仍差 8 倍。

边界（必须说明，避免过度解读）：

1. 本文对比的是**同步业务写法**（fiber 内阻塞 / worker 内阻塞）。epoll 也可以用
   「下游 fd 加入 epoll + 状态机」实现零阻塞，理论吞吐可再上一个台阶，但那是另一套
   复杂度与代码形态；本轮没有实现该变体。
2. epoll 的 cpu µs/req 始终更低（120~121 vs 146~255），因为「阻塞一个线程」比
   「park 一个 goroutine」的内核路径更便宜；GMP 赢在吞吐与延迟，不在单请求 CPU。
3. epoll 靠增加 worker 可以追平 20k 这一档（8.4），但 8000 连接要追平 80k 需要
   ~80 个 worker，线程内存与调度成本会进一步恶化。
4. 所有轮次 `errors=0`；GMP 的 RSS 随并发 park 增长（58MB→413MB），epoll 稳定在
   22~60MB，这是两种模型的另一项权衡。

## 9. 全链路内存优化（2026-10-19 20:55）

### 9.1 测量方法

用 `/proc/<pid>/smaps_rollup` 与 `/proc/<pid>/maps` 采样 8000 连接稳态：

- base（0 连接）RSS 5MB；
- 每连接增量 =（8000 连接 RSS − base）/ 8000；
- 映射分布用来定位大头（Fiber 栈是 128KB 级匿名映射）。

原始测量（8000 连接、GMP P=4、`rate=1`、12s）：

| 配置 | 峰值 RSS | 每连接 | mid 映射数（32~512KB） | 虚拟内存 |
|---|---:|---:|---:|---:|
| 64KB 栈上读缓冲 + 128KB 栈 | 657MB | 81.5KB | 8,022 | 1,357MB |
| 8KB 读缓冲 + 128KB 栈 | 207MB | 25.9KB | 8,022 | 1,357MB |
| 8KB 读缓冲 + 32KB 栈 | 206MB | 25.7KB | 8,020 | **607MB** |
| 8KB 读缓冲 + 16KB 栈 | 208MB | 26.0KB | 25（<32KB 归入 small） | **482MB** |
| 4KB 读缓冲 + 32KB 栈 | 195MB | 24.3KB | 8,021 | 679MB |

**结论**：

1. RSS 的绝对大头是**每连接 64KB 的栈上读缓冲**（`serve_gmp_connection` 的
   `char buffer[64*1024]`）。降到 8KB 后 RSS 从 657MB → 207MB（**−68%**），
   每连接 81.5KB → 25.9KB。
2. Fiber 栈大小（128KB → 32KB）**不影响 RSS**（未触碰页不计入），但把虚拟内存
   从 1,357MB 降到 607MB（**−55%**）；16KB 栈进一步降到 482MB。
3. 4KB 读缓冲只再省 12MB（−6%），却让 4,100 字节的请求需要两次 `recv`，
   不划算，**8KB 是最佳平衡点**。

### 9.2 稳态 RSS（公平 benchmark `[window] rss`，同口径）

| 场景 | 优化前（64KB 缓冲） | 优化后（8KB 缓冲） | 变化 |
|---|---:|---:|---:|
| s3 8000 连接 / GMP P=4 | 488MB（2 rep） | **161MB** | **−67%** |
| A 高吞吐 200x2 | 29MB | **17.2MB** | −41% |
| B pipeline 200x2 | 34MB | **31.5MB** | −7% |

### 9.3 性能回归（同场景单次，窗口内无回归）

| 场景 | 指标 | 优化前 | 优化后 |
|---|---|---:|---:|
| s3 8000 rate1 | QPS | 7,920 | **7,983 / 7,985**（stack 128/32） |
| s3 8000 rate1 | cpu µs/req | 128.5 | **125.2 / 124.9** |
| A 高吞吐 | QPS / cpu | 139,530 / 72.4 | **142,448 / 71.2** |
| B pipeline | QPS / cpu | 168,548 / 62.6 | **172,132 / 62.3** |

A/B 的 QPS 还略高（窗口更空），至少没有回归；Fiber 栈 128KB → 32KB 对性能无影响。

### 9.4 每连接剩余 24~26KB 的构成（估算）

| 项 | 大小 | 说明 |
|---|---:|---|
| 栈上读缓冲 | 8KB | 已从 64KB 降下来，一帧上限 4,100B，8KB 可一次读完 |
| `std::string in` / `out` | ~8KB | 按帧增长到 4~8KB，可按需 reserve 或池化 |
| Fiber 栈触碰页 | ~8~10KB | 栈帧 + 局部变量，128KB 栈只用触碰页 |
| Socket / FdSlot / WaitNode / Token | ~1~2KB | 每连接一个 FdSlot、每次等待一个 shared_ptr 节点 |

进一步可做的（本次未实施，收益递减）：

1. `in`/`out` 用固定小缓冲 + 按需扩容，或从池里借还（约省 4~8KB/连接）；
2. `WaitNode` / `FdSlot` 池化（每请求一次 shared_ptr 分配）；
3. Fiber 栈默认值按场景配置（32KB 可把 8000 连接的虚拟内存从 1.3GB 压到 0.6GB）；
4. client 退出后 RSS 从 657MB 只回落到 97~130MB —— `FiberBin` 栈池与 malloc 缓存
   保留，可加 `malloc_trim(0)` 或缩小 `fiber_bin_capacity`。

## 11. 优化轮次 1：Fiber 缓存扩容（2026-10-19 23:05）

### 11.1 改动

| 项 | 改动前 | 改动后 |
|---|---:|---:|
| thread-local 栈池容量（`fiber.cpp:718/803`） | 8 | **32** |
| 全局栈池容量（`fiber.cpp:726/809`） | 128 | **512** |
| `SchedulerConfig::fiber_bin_capacity`（`scheduler.hpp:240`） | 32 | **128** |

目的：提高 Fiber/栈复用命中率，减少短连接场景的 mmap/mprotect 次数。

### 11.2 验证

- `ctest` **116/116** 通过。
- A 高吞吐（200x2 depth1，16KB 初始栈）：QPS **137,943** / cpu **71.1µs** / RSS 17.4MB（改动前 142,491 / 70.3 / 17.0MB）。
- s3（8000 rate1, P=4）：QPS **7,984** / cpu **121.1µs** / RSS **169.9MB**（改动前 7,895 / 121.9 / 161.9MB）。

### 11.3 短连接专项（s1，旧 benchmark，2 clients，2 rep 中位）

| 指标 | 池 8/128/32 | 池 32/512/128 | 变化 |
|---|---:|---:|---:|
| QPS | 22,606 | **33,058** | **+46%** |
| cpu µs/req | 248.7 | **216.9** | −13% |
| p99 | 40.8ms | **5.6ms** | −86% |
| VmSize | 84GB | **8.2GB** | −90% |
| RSS | 486MB | 565MB | +16% |
| 错误率 | 1.2% | 2.1% | +0.9pp（established 更多） |

根因：池容量过小 → 短连接场景同时存活大量 growable Fiber（每个 8MB 预留），
VmSize 冲到 84GB，缺页/回收压力把 p99 推到 40ms；扩池后复用率上来，QPS/CPU/延迟/VmSize 全部改善。

### 11.4 结论

- **fiber cache 扩容对短连接是显著优化，保留**：QPS +46%、cpu −13%、p99 −86%、VmSize −90%；
- 长连接场景（A/s3）无回退：A 在 ±3% 窗口波动内，s3 cpu 基本持平；
- 代价：s1 的 RSS +16%（565MB）、s3 RSS +8MB，来自缓存更多已触碰栈；
- s1 错误率从 1.2% 升到 2.1%，但 established 从 29 万升到 40 万，绝对错误数增加与吞吐提升同向；该错误率是已知的 s1 问题（EPOLLHUP 竞争），非本轮引入。

### 11.4 下轮候选

1. `wake_batch`：A 场景 epoll_wait 0.47 次/请求（epoll 侧 0.10），检查 poller 批量策略；
2. `recvmsg 1.2 次/请求`：定位多出的 0.2 次来源（EAGAIN 重试/partial）；
3. Fiber 控制块 cacheline hot/cold + runqueue 伪共享；
4. s1 短连接专项：验证 fiber cache 扩容收益。

### 11.5 第二轮：池容量再加倍（2026-10-19 23:12）

把 32/512/128 继续加倍到 64/1024/256，s1 短连接（2 rep）：

| 指标 | 32/512/128 | 64/1024/256 |
|---|---:|---:|
| QPS | **33,058** | 32,118（−2.8%） |
| cpu µs/req | **216.9** | 221.0 |
| p99 | 5.6ms | 5.7ms |
| RSS | 565MB | 556~566MB |

结论：**继续加倍无收益（略降），32/512/128 是拐点**，已回退到该配置并重编。
说明缓存容量存在边际递减：超出后只是保留更多未触碰/已触碰栈，RSS 不再下降，反而挤占 cache。

## 12. 优化轮次 2：批量发送（2026-10-19 23:35）

### 12.1 问题

公平 benchmark 的 GMP 服务端每解析一帧就 `send_all` 一次；pipeline（depth8）下
一次 recv 解析出 8 帧就要 8 次 `sendmsg`，而 epoll 侧是「append 到 conn.out 后一次 flush」。
A 场景 strace 实测 GMP `sendmsg` 1.2 次/请求（理论 1.0），B 场景差距更大。

### 12.2 改动

`benchmarks/gmp_vs_epoll_fair_benchmark.cpp` 的 `serve_gmp_connection`：

- 每轮 recv 开始 `out.clear()`，所有解析帧 append 到同一个 `out`；
- 解析循环结束后一次 `send_all`（批量发送）；
- `out.reserve(16KB)`（depth8 批量约 32KB，一次 realloc；s3 每连接只触碰约 4KB）。

### 12.3 结果（同机，各 1-2 rep）

| 场景 | 指标 | 批量前 | 批量后 |
|---|---|---:|---:|
| B pipeline depth8 | QPS | 168,548 | **191,958（+13.9%）** |
| B pipeline depth8 | cpu µs/req | 62.6 | **57.8（−7.7%）** |
| B pipeline depth8 | sys µs/req | 8.5 | **4.5（−47%）** |
| B pipeline depth8 | sendmsg/请求 | 1.2 | **0.33（−72%）** |
| A depth1 | QPS / cpu | 142,491 / 70.3 | 142,934 / 70.2（持平） |
| s3 8000 rate1 | QPS / cpu | 7,983 / 121.9 | 7,991 / 123.3（噪声内） |
| s3 8000 rate1 | RSS | 161.9MB | 190.9MB（+29MB，out 缓冲触碰页） |

### 12.4 结论

- **pipeline 场景显著收益**：QPS +13.9%、sys −47%、sendmsg −72%；这是本轮最大的单点优化；
- depth1/s3 无回退（批量退化为 1 帧 1 send，与原行为一致）；
- 代价是每连接 out 缓冲的触碰页（s3 +29MB）；`reserve(64KB)` 时 s3 会 +45MB，因此定为 16KB；
- 该改动在 benchmark 应用层，未改库；库侧 Socket 若要支持「写合并」需要发送缓冲与 flush 策略，风险更高，本轮未做。

## 13. 微优化实验记录（2026-10-19 23:45～00:15）

前三轮大项（fiber cache、send 批量）之后，继续尝试单个 syscall/布局级微优化，逐项记录：

### 13.1 epoll 事件批大小 256 → 1024（回退）

- 目的：一次 `epoll_wait` 拿更多就绪事件，减少 wait 次数。
- 结果：B 场景 188,893 / 189,649（基线 189,159，噪声内）；strace 显示 B 的 `epoll_wait` 已经只有 **0.09 次/请求**（pipeline 批量已足够），256 并不制约。
- 代价：`BackendEvent events[1024]` 占 16KB 栈，触发 growable 栈增长。
- 结论：**无收益，回退到 256**。

### 13.2 接收缓冲预分配 `in.reserve(16KB)`（保留）

- 目的：`std::string in` 靠 SSO 增长会多次 realloc；与 `out` 一样预分配。
- 结果：B 场景 190,220 / 190,322（基线 189,159，+0.6%，噪声内）；RSS 32MB vs 36MB。
- 结论：收益在噪声内但 RSS 略降，**保留**。

### 13.3 Fiber 控制块 `alignas(64)`（回退）

- 目的：避免相邻 `Fiber::Impl` 共享 cacheline（`m_resume_claim`/`m_record` 等）。
- 结果：A 141,774（基线 141,968）、B 187,945（基线 189,159）、s3 8,007（基线 7,983），全部噪声内；s3 RSS 190 → **216MB**（对齐 padding 与池缓存）。
- 结论：**无收益且增内存，回退**。

### 13.4 本轮小结

- syscall 已压到约 3 次/请求（recvmsg 1.2 + sendmsg 0.33 + epoll_wait 0.47 + futex 0.12，A/B 混合口径），纯 IO 场景的剩余空间在 1% 量级；
- 可测量的收益仍集中在「短连接 fiber 复用」（+46%）与「pipeline 批量发送」（+12~14%）；
- 下一批候选必须换维度：库侧 Socket 写合并（影响所有用户、风险高）、s1 错误率（2.1%，非性能但影响有效吞吐）、recv 动态缓冲（s3 RSS 代价大）。

## 14. 批处理正确性：合并唤醒的「吞唤醒」修复（2026-10-19 00:33）

### 14.1 问题

`tickle()` 用布尔 `m_tickle_pending.exchange(true)` 合并 eventfd 写，poll 侧
`drain_wake_fd()` 后 `store(false)`。这是 check-set 与 drain 的经典竞态：

```
tickle: exchange(true) 读到旧 true → 跳过写
poll:   drain_wake_fd(); store(false) → 回 epoll_wait
```

在「drain 之后、store 之前」到达的 tickle 会被吞掉。IO 就绪由内核 epoll 驱动不会丢，
但新增定时器/新注册 fd 的及时唤醒可能被延迟，属于「批处理中间无新元素导致后续卡住」的变体。

### 14.2 修复

`src/network/io.cpp`：

- `m_tickle_pending` 由 `atomic<bool>` 改为 `atomic<uint32_t>` 计数；
- `tickle()`：`fetch_add(1) == 0` 时才 `m_backend->wake()`（0→1 的那次写）；
- 两个 drain 点：`store(false)` 改为 `exchange(0, acq_rel)`。

这样 drain 之后到达的 tickle 必然看到计数 0 并重新写 eventfd，不会吞唤醒；
drain 之前被合并的 tickle 也已由「poller 正在处理」满足（处理完会回 worker_loop 检查队列）。

### 14.3 验证

- `ctest` **116/116** 通过；
- A：142,722 QPS / 70.2µs；B：192,126 / 58.0；s3：7,963 / 121.8（均无回退，A/B 略好于修复前）。

### 14.4 批处理必须满足的四条规则（本轮总结）

1. **批次边界由已完成的数据/事件决定**，绝不「等凑满 N 个」：send 批次 = 本次 recv 的完整帧，循环末尾无条件 flush；wake 批次 = 本次 epoll_wait 返回的 count 个事件。
2. **合并唤醒用计数而非布尔**：只有 0→1 写 eventfd，drain 用 exchange(0)，避免吞唤醒。
3. **时间窗口合并必须配 deadline**：若将来做「1ms 内的响应合并」，必须由定时器到期强制 flush，不能依赖「再来一个元素」。
4. **EOF/关闭/错误必须 flush 剩余**：连接关闭前把已解析响应的 out 发完（当前实现已满足）；批次内存要有上限（背压）。

### 14.5 待补测试

- 并发 tickle + drain 的压力测试（当前单测覆盖不到这个竞态窗口，修复靠代码审查与语义推导）；
- 「单元素批次 / 末尾半帧 / 无后续事件」的端到端回归（当前靠 fair_bench 的 depth1/depth8 覆盖了部分）。

## 15. s1 错误率口径澄清（2026-10-19 01:08）

### 15.1 做法

给旧 benchmark 客户端（`gmp_vs_thread_epoll_benchmark.cpp`）的 `EPOLLERR|EPOLLHUP`
分支加 `getsockopt(SO_ERROR)` 直方图：`err_fin`（SO_ERROR=0，正常 FIN）、
`err_reset`（ECONNRESET）、`err_pipe`（EPIPE）、`err_other`，并输出到 RESULT JSON。

### 15.2 数据

| 场景 | completed | errors | 错误率 | 主要类型 |
|---|---:|---:|---:|---|
| 直连，无 `--pin` | 76,704 | 66 | **0.086%** | err_pipe 78（直方图口径另计） |
| 直连，`--pin` | 67,339 | 25 | **0.037%** | err_pipe 25 |
| `_bench_work` 高吞吐（~34k QPS） | 137,395 | 3,685 | **2.7%** | 未细分（本轮之前） |

### 15.3 结论

- **s1 错误率不是固定 2%，而是随吞吐/并发升高**：直连 ~9.5k QPS 时仅 0.04~0.09%，
  `_bench_work` 把吞吐推到 ~34k QPS 时才到 2.7%；
- 错误类型以 **EPIPE**（服务端 send 时对端已关闭）为主，`err_reset` 只有个位数，
  `err_fin=0`——不是「正常 FIN 被误计」；
- `--pin` 反而降低错误率（0.037%），说明不是 pin 引起的；
- 历史「3% 错误率」应理解为「高并发短连接下的 close/send 竞争」，不是库的稳定缺陷；
- 修复方向（如果要做）：服务端在 close 前 `shutdown(SHUT_WR)` 并读到 EOF（多一次往返），
  或客户端忽略 EPIPE（只改统计口径，不解决重连成本）。本轮只做口径澄清，未改行为。

### 15.4 附带改动

- 旧 benchmark 客户端新增 errno 直方图输出（诊断用，不影响被测服务端）。

## 16. 接收/发送缓冲预留 16KB → 4KB（回退）

- 目的：s3 的 out/in 缓冲预留是每连接 16KB，8000 连接虚拟 128MB；降到 4KB（单帧典型值）期望降 RSS。
- 结果：s3 RSS **224.7MB**（16KB 时 190~217MB，反而升高，分配器碎片/realloc）；B 194,113（+1%，噪声内）。
- 结论：**无内存收益，回退到 16KB**。

### s3 RSS 构成估算（190MB，8000 连接）

| 项 | 估算 |
|---|---:|
| 基础（0 连接） | 5MB |
| Fiber 栈触碰（16KB 初始，实际 ~10KB/连接） | ~80MB |
| `in`/`out` 缓冲触碰（各 ~4KB/连接） | ~32MB |
| 栈池缓存（512 × ~26KB） | ~13MB |
| Socket/FdSlot/WaitNode/Fiber 控制块 | ~16MB |
| 页表/增长区/分配器碎片 | ~40MB |

进一步降 RSS 需要改「每连接 in/out 缓冲」为共享/池化（应用层）或缩小栈池（512 → 256），
都会牺牲复用率，本轮未做。
