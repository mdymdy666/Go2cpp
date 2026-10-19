# Go2Cpp 参数优化

> 目标：对影响性能的默认参数做实测扫描，综合评定后改**源码默认值**（不是只在 benchmark 传参）。
> 压测统一用**计算注入**（`GO2CPP_BENCH_COMPUTE_ITEMS=4096`，随机 0~4096 个 int 排序），
> 不用 `usleep`（它会让 GMP 的协作睡眠成为被测对象，口径不公平）。

## 一、参数清单（完整）

### SchedulerConfig（`include/go2cpp/concurrency/scheduler/scheduler.hpp:190`）

| 参数 | 当前默认 | 含义 | 待扫 |
|---|---|---|---|
| `processor_count` | `0`（= 硬件核数） | P 数量，决定并行度 | ⬜ |
| `max_workers` | `0`（= 2×P，且受 `allow_worker_oversubscription` 影响） | M 上限 | ⬜ |
| `idle_wait` | `10ms` | worker 空闲时 `epoll_wait` 的超时 | ⬜ |
| `local_queue_limit` | `256` | 本地队列上限，超出溢出到全局队列 | ⬜ |
| `min_workers` | `0`（= P） | 常驻基础 M 数量 | ⬜ |
| `idle_worker_timeout` | `250ms` | 补充 M 的阻塞结束检查周期 | ⬜ |
| `fiber_stack_size` | `0`（= 128KiB） | 每 Fiber 栈大小 | ⬜ |
| `task_affinity_budget` | `4` | 任务亲和预算 | ⬜ |
| `allow_worker_oversubscription` | `true` | 允许 M 数超过 P | ⬜ |
| `allow_idle_shrink` | `false` | 基础 M 空闲是否退出 | ⬜ |
| `enable_sysmon` | `true` | 阻塞监控开关 | ⬜ |
| `sysmon_interval` | `10ms` | sysmon 周期 | ⬜ |
| `long_syscall_threshold` | `50ms` | 长系统调用判定阈值 | ⬜ |
| `pin_workers_to_cpu` | `false` | M 绑核 | ⬜ |
| `fiber_bin_capacity` | `32` | 每 M 的 FiberBin 容量 | ⬜ |
| `collect_metrics` | `true` | 累计指标开关 | ⬜ |

### IOManager（`src/network/io.cpp`）

| 参数 | 当前值 | 含义 | 待扫 |
|---|---|---|---|
| `kSlotStripeCount` | `64` | fd 槽位条带数 | ⬜ |
| `epoll_wait` 的 maxevents | 待查（`poll_once`） | 一次拿多少事件 | ⬜ |
| `kRegistryStripeCount` | `16` | 注册表条带数 | ⬜ |
| `kIncomingStripeCount` | `16` | 外部入队条带数 | ⬜ |

### 其它

| 参数 | 当前位置 | 含义 | 待扫 |
|---|---|---|---|
| `kMaxEvents`（epoll） | 待查 | epoll_wait 的 maxevents | ⬜ |
| `kSpinBudget`（ParkingCondition） | `128` | futex 前的自旋预算 | ⬜ |
| `kDefaultFiberBinCapacity` | `32` | Fiber 运行时默认池容量 | ⬜ |

## 二、进度表

| 轮次 | 参数族 | 状态 | 推荐值 | 结论 |
|---|---|---|---|---|
| 1 | （参数梳理） | ✅ | — | 清单见上 |
| 2 | `processor_count` | ⬜ | — | — |
| 3 | `min_workers` / `max_workers` | ⬜ | — | — |
| 4 | `epoll_wait` 批次 | ⬜ | — | — |
| 5 | `fiber_bin_capacity` / `fiber_stack_size` | ⬜ | — | — |
| 6 | `local_queue_limit` | ⬜ | — | — |
| 7 | `sysmon_interval` / `long_syscall_threshold` | ⬜ | — | — |
| 8 | 条带数（64/16/16） | ⬜ | — | — |
| 9 | `idle_wait` / `idle_worker_timeout` | ⬜ | — | — |
| 10 | `pin_workers_to_cpu` / `allow_worker_oversubscription` | ⬜ | — | — |

## 三、已知的基线（计算注入，s4，64 连接，12 核）

| 配置 | QPS | P50 | P99.9 | µs/req | user | sys |
|---|---:|---:|---:|---:|---:|---:|
| GMP P=24 | 32,188 | 1.91ms | 4.98ms | 235.4 | 80.1 | 155.3 |
| epoll 256w | 35,854 | 1.56ms | 4.07ms | 106.2 | 73.3 | 33.0 |

**核心差距**：`sys` 155.3 vs 33.0（4.7 倍）——参数优化的主要目标就是降这个。

## 轮次 2：`processor_count` 扫描（计算注入，s4，64 连接）

| P | QPS | P50 | P99.9 | µs/req | **sys** | CPU | RSS | vsz |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 12（=核数） | 27,959 | 2.20ms | 4.92ms | 248.3 | 168.2 | 694% | 6.0MB | 888MB |
| 18 | 29,558 | 2.08ms | 4.81ms | 247.5 | 167.3 | 731% | 6.2MB | 1,320MB |
| 24（2×） | 32,328 | 1.91ms | 4.70ms | 237.6 | 158.8 | 768% | 6.5MB | 1,752MB |
| 36 | 33,593 | 1.84ms | 4.70ms | 236.4 | 154.7 | 794% | 7.3MB | 2,616MB |
| 48 | 35,419 | 1.75ms | 4.67ms | 232.0 | 149.1 | 822% | 7.9MB | 3,480MB |
| **96（8×）** | **52,555** | **1.17ms** | 4.73ms | **165.6** | **79.9** | 870% | 11.5MB | 6,873MB |

### 结论

1. **QPS 随 P 单调上升**（+88%），**sys 随 P 单调下降**（−52%）——P 越多，跨 M 协调越少；
2. **P99.9 基本不变**（4.67~4.92ms），**vsz 线性增长**（888MB → 6,873MB）；
3. **P=96 时对比 epoll 256w**：QPS **+47%**、P50 **−25%**、vsz 更小（6,873 vs 8,138MB）；
   但 P99.9 仍差（4.73 vs 4.07ms）、sys 仍差（79.9 vs 33.0，但从 4.7× 缩到 2.4×）。

### 推荐值（待验证其它场景后再改默认）

| P | 场景 | 代价 |
|---|---|---|
| 核数（当前默认） | 省内存 | QPS 只有 P=96 的 53% |
| **2×核数（推荐默认）** | 均衡 | QPS +16%、sys −6%，vsz 2.9× |
| **8×核数** | 吞吐优先（内存充足） | QPS +88%、sys −52%，vsz 8× |

**下一轮必须验证**：`2×核数` 在 s1（短连接）与 s3（8,000 长连接）下是否同样更好——
否则改默认会顾此失彼。

## 轮次 3：`processor_count` 默认值已改（核数 → 2×核数）

### 改动

`src/concurrency/scheduler.cpp` 的 `normalize()`：

```cpp
if (requested.processor_count == 0) {
    // 默认 2 × 核数（不是 1 × 核数）
    requested.processor_count = default_processor_count() * 2;
}
```

### 验证：三个场景 P=12 vs P=24

| 场景 | P=12 | **P=24** | 变化 |
|---|---|---|---|
| **s1 短连接** | 19,121 QPS / 3.13ms / err 0.48% | **39,141 / 1.20ms / err 3.1%** | **QPS +105%、P50 −62%**，但错误率 6.5× |
| **s3 8,000 长连接** | 7,919 / 3.56ms / P99.9 14.98ms | 7,941 / **0.31ms / 3.95ms** | **P50 −91%、P99.9 −74%**，CPU +23% |
| **s4 计算+IO** | 27,959 | 32,328 | **QPS +16%、sys −6%** |

**三个场景全部改善**（s1 的 QPS、s3 的延迟、s4 的吞吐与 sys）。

### 代价

| 项 | P=12 | P=24 |
|---|---|---|
| 虚拟内存 | 888MB | **1,752MB**（2×） |
| s1 错误率 | 0.48% | **3.1%**（P 越多 accept 竞争越激烈） |

**s1 错误率上升是唯一需要关注的副作用**——非阻塞 accept 在多 P 同时 accept 时 RST 概率增加。

### ⚠️ 踩到的坑：benchmark 的 `--workers` 会覆盖源码默认

`benchmarks/gmp_vs_thread_epoll_benchmark.cpp:1731`：

```cpp
const auto workers = static_cast<std::size_t>(arg_int(argc, argv, "--workers", 8));
```

**默认 8**，会覆盖 `config.processor_count`。所以：

- 要测**源码默认** → 必须传 `--workers 0`（`0` 会让 `normalize()` 走默认）；
- 要测**指定 P** → 传 `--workers N`。

**验证方式**：看 `peak_threads`。`--workers 0` 时是 **26**（P=24 + sysmon + timer），
`--workers 8` 时是 10。

### 验证

- `ctest` **113/113**（60.24s）
- 默认生效确认：`--workers 0` → `peak_threads=26`（P=24）

### 下一轮

`s1` 的错误率从 0.48% 涨到 3.1% 是 P 超配的副作用，需要确认：
1. 这是否与 `accept` 的竞争有关（多个 P 同时 `accept`）；
2. 如果确认，是否需要在 `processor_count > 核数` 时**限制 accept 到单一 P**（或加 `EPOLLEXCLUSIVE`）。
