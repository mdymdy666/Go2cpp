# Fiber vs 线程：切换开销基准（2026-10-21）

> 目的：量化「协程切换比线程切换便宜多少」。结论：**Fiber ~500ns vs 线程 ~28µs（WSL2），约 57 倍**。

## 方法

| 对比 | 方法 | 每次切换包含 |
|---|---|---|
| **Fiber** | 两个协程通过**无缓冲 channel 来回交接** | park + wake + 调度器 queue 操作 + 用户态上下文切换 |
| **线程** | 两个 pthread 通过 **mutex + condition_variable 来回交接** | futex wait + wake（系统调用）+ 内核调度器切换 |

- 基准：`tests/bench/fiber_vs_thread_switch.cpp`；
- 计时：`steady_clock` 包住 N 轮 ping-pong（每次 ping-pong = 2 次切换）；
- 环境：WSL2（6.6.87.2-microsoft-standard）/ x86_64 / 12 CPU / `build-work`。

## 数据（n=100000，200000 次切换，3 轮）

| 轮次 | Fiber | 线程 | 倍数 |
|---|---:|---:|---:|
| 1 | 466 ns | 27,657 ns | 59x |
| 2 | 514 ns | 29,015 ns | 56x |
| 3 | 523 ns | 29,253 ns | 56x |
| **平均** | **~500 ns** | **~28.6 µs** | **~57x** |

## 为什么差这么多

| 成本项 | Fiber | 线程 |
|---|---|---|
| 上下文保存/恢复 | 用户态寄存器（~56B，`context_x64.S`） | 内核 `task_struct` + FPU 状态 + 地址空间检查 |
| 调度 | 用户态 runqueue（原子操作） | 内核 CFS + 负载均衡 |
| 唤醒 | 用户态 futex / 条件变量（可选） | **系统调用** `futex(FUTEX_WAKE)` + 可能 IPI |
| 缓存 / TLB | 同进程，TLB 命中 | 可能跨核（cache/TLB 失效） |
| 虚拟化 | 无 | **WSL2 放大**（裸机通常 2–5µs） |

## 边界（必须一起读）

1. **Fiber 的 ~500ns 包含调度器开销**（park/wake + runqueue），纯 `resume/suspend`（无调度器）通常 **~100–200ns**；
2. **线程 28µs 是 WSL2 环境**——裸机 Linux 通常 **2–5µs**（仍比 Fiber 慢 10–20 倍）；
3. 这是「ping-pong 交接」场景（每次都要唤醒对方），不是「同线程内顺序切换」；
4. 线程切换受 CPU 频率/核数/负载影响，方差大。

## 复现

```bash
bash tests/bench/run_switch_bench.sh
NS="10000 100000" bash tests/bench/run_switch_bench.sh
```
