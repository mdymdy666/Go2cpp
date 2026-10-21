# Go2Cpp 性能数据汇总（2026-10-21）

> 所有数据来自本仓库的可复现基准与测试，环境：WSL2（6.6.87.2-microsoft-standard）/ x86_64 /
> 12 CPU / 页 4096B / `build-work`（Release）。测量时 loadavg < 2（否则数据不采信）。

## 1. 并发原语

| 指标 | 优化前 | 优化后 | 来源 |
|---|---:|---:|---|
| Mutex latch（8 线程） | 24–55 µs | **18.3–19 ns** | `docs/module-audit-2026-10-20.md` §6 |
| Mutex 两态唤醒（正常/饥饿） | — | 正常态原子快速路径；饥饿态 FIFO 交接 | `src/concurrency/sync.cpp` |
| ParkingCondition pc-rtt | 32.1 µs | **3.4 µs** | `docs/module-audit-2026-10-20.md` §6 |
| ParkingCondition notify（futex_waitv 双字） | — | 单次唤醒两字 | `src/concurrency/parking_condition.cpp` |
| Channel cap0 rendezvous | 30.3 µs | **1.19 µs**（25.5x） | 同上 |
| Channel ping-pong | 59 µs | **2.65 µs** | 同上 |
| RWMutex alloc/op | 3.00 | **0.02** | 同上 |
| RWMutex RLock | 64–70 µs | **43–45 ns** | 同上 |
| RecursiveMutex owner 快速路径 | — | `m_owner==0` 直接返回 | `recursive_mutex.hpp` |

## 2. Fiber vs 线程

### 切换开销（`docs/switch-benchmark.md`）

| 指标 | Fiber | 线程 | 倍数 |
|---|---:|---:|---:|
| 每次切换（ping-pong，含调度/唤醒） | **~500 ns** | **~27 µs** | **~55x** |
| 纯上下文（无调度器，估计） | ~100–200 ns | 2–5 µs（裸机） | 10–20x |

### 内存（`docs/memory-benchmark.md`，64KB 栈，n=100/1000）

| 指标 | Fiber | 线程 | 说明 |
|---|---:|---:|---|
| VmRSS / 单位 | 60.6 KB | 65.8 KB | 物理只省 8% |
| VmSize / 单位 | 68.5 KB | 6294 KB | 虚拟省 **92x** |
| 固定开销 VmRSS | 0.6–0.5 KB/个 | 13.5–9.8 KB/个 | 记录 vs TLS/guard |
| 固定开销 VmSize | 0–0.5 KB/个 | 62 MB/个（arena） | glibc 64MB arena |

## 3. 调度器与内存稳定性

| 指标 | 数据 | 来源 |
|---|---:|---|
| 终态 G 回收（D2） | RSS 2/10/30/50s：**228.9 → 20.3 MB** | `tests/enhanced/defects.md` D2 |
| 130s 长跑 | **恒 20.8 MB**（严格平台化） | 同上 |
| Release 长跑（channel 2.25M 次） | 11.6 MB 稳定 | 同上 |
| dump 体积（48s） | 2.2 KB、tasks=2、kDead=0 | 同上 |
| enqueue 吞吐（2 worker） | 966k → **1,209k/s**（+25%） | 同上 |

## 4. 网络与真实场景

| 指标 | 数据 | 来源 |
|---|---:|---|
| 死锁场景（122 clients × 4 路由） | **3006.9 QPS**、p50 0.38ms / p95 0.64ms / p99 8.98ms | `tests/scenarios/deadlock_routes` |
| 死锁检测 | `verdict=deadlock`，no_progress 644ms | 同上 |
| 受控解锁 | stuck **10 → 0**，10 请求全 200 | 同上 |
| s3 内存（8000 连接，历史） | 190MB（Fiber）vs 54MB（epoll） | `docs/module-audit` |
| 阻塞场景吞吐（历史） | 1.94x（2000 连接）/ 6.09x（8000 连接）领先 epoll 线程池 | 同上 |

## 5. 回放与诊断

| 指标 | 数据 | 来源 |
|---|---:|---|
| 96 rendezvous 重放 | 10 轮 MATCH（checksum 逐位） | `tools/replay` |
| select 重放 | 5 轮 MATCH（96 次/轮） | 同上 |
| 虚拟时钟（context 超时） | 录制 wall 102ms → 重放 **1.2ms**（virtual 101ms） | 同上 |
| 资源事件 v5（RW/CV/WG/IO） | 13 种事件，录制/回放 MATCH | `docs/replay-service.md` |
| 事件成本 | 单事件 **19.65–22.47 ns** | `docs/debug-reliability.md` |
| dump 端到端 | `--view gmp` p50 **6.76ms** / p95 8.29ms | 同上 |

## 6. 测试与质量

| 指标 | 数据 |
|---|---:|
| ctest | **130/130**（默认）、build-enhanced 61/61 |
| enhanced 五能力 | 183 用例（182 PASS / 1 SKIP） |
| 场景 | ctx-cancel 65、deadlock 17、full-trace |
| tools | migration 53、CLI 213、replay 单测+6 场景 |
| 随机/压测 | 迁移 1000 位翻转、回放 500 交错、Debug 10000 帧、CLI 1000 参数 |
| 资源历史 | 10 类结构压力（4 线程×250 随机，事件守恒） |

## 复现命令

```bash
# 切换开销
bash tests/bench/run_switch_bench.sh

# 内存基准
QUICK=1 bash tests/bench/run_memory_bench.sh

# 全量测试
bash tests/run_all_tests.sh

# ctest
ctest --test-dir build-work
```
