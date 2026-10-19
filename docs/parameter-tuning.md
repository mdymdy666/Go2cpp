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
