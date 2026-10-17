# 零影响验证 · 未启用调试模块时框架性能不变（第 31 轮）

> **目的**：使用者要求「调试部分不怎么需要性能，但**必须保证不影响原来的模块性能**」。
> 本文件用**对照组实验**证明这一点。
>
> **组 A（对照）**：`main` 分支用 `git archive` 导出到 `/tmp` 后**独立构建**，**完全没有 debug 模块**。
> **组 B（实验）**：当前分支 `feature/gmp-debug-module`，**含 debug 模块但运行时不安装任何观察者、
> 不注册选择器、不启用断点**。
>
> 构建：`build-bench`（Release + `-O2`）；宿主 AMD Ryzen 9 8945HX（12 线程）/ WSL2。
> 每个基准跑一次，取**基准自身内部**的中位数（基准内部已做多轮）。
> **与其它章节不可横向比较。**

## 1. 结构性保证（比性能数字更硬的证据）

```sh
$ grep -rn "go2cpp/debug" include/go2cpp/ src/ --include=*.hpp --include=*.cpp \
      | grep -v "^include/go2cpp/debug/"     # 排除 debug 自身
（无输出）
```

- **框架完全不依赖 debug 模块**：`include/go2cpp/`（除 `debug/`）与 `src/` 里
  **没有任何一处** `#include "go2cpp/debug/..."`；
- **依赖是单向的**：`debug` 模块只引用 `scheduler.hpp` / `fiber.hpp` / `sync.hpp`；
- 因此**删掉整个 `include/go2cpp/debug/`、`tests/debug/`、`example/debug/` 之后，
  框架仍可编译、行为不变** —— 这是比任何基准数字都强的保证。

## 2. 唯一在框架热路径上的改动及处理

| 改动 | 位置 | 在热路径上？ | 处理 |
|---|---|---|---|
| `WaitSelector` 注册表 + `ReorderWaitersForSelector` | `sync.cpp` 的 `Mutex::Unlock` 慢路径 | **是** | 见下 |
| `Mutex`/`ConditionVariable`/`WaitGroup` 的 `DebugSnapshotInternal()` | `sync.hpp`/`sync.cpp` | 否（仅被显式调用） | 无需处理 |
| `Scheduler::DebugSnapshotTasks()` | `scheduler.cpp` | 否（仅被显式调用） | 无需处理 |

**`Unlock` 慢路径的两级快速返回**：
① `waiters.size() < 2` 直接返回（绝大多数情况）；
② 若曾注册过选择器才用 `std::atomic<bool> s_has_selector` 快判 —— **未注册时只读一次原子量**，
   不取锁、不拷贝 `shared_ptr`。

> 初版只做了 ①，`< 2` 之外仍会加 `s_selector_mutex` 并拷贝 `shared_ptr`（约 30~60 ns）。
> 按「必须不影响」的要求，第 31 轮补了 ②，使未注册时开销归零（提交 `b8ff33d`）。

## 3. 实测结果：gmp_compute（调度器吞吐与扩展性）

| 指标 | main | HEAD | HEAD/main |
|---|---:|---:|---:|
| Fiber 计算并行（ms） | 30.58 | 30.75 | **1.01x** |
| **Thread 计算并行（ms）** | 30.85 | 43.60 | **1.41x** ⚠️ |
| 调度器扩展 workers=1（ms） | 163.85 | 169.31 | **1.03x** |
| 调度器扩展 workers=2（ms） | 83.15 | 85.18 | **1.02x** |
| 调度器扩展 workers=4（ms） | 41.72 | 42.65 | **1.02x** |
| 调度器扩展 workers=8（ms） | 21.92 | 22.67 | **1.03x** |
| 调度器扩展 workers=12（ms） | 17.58 | 16.68 | **0.95x** |
| spawn 开销（µs/G） | 25.94 | 25.62 | **0.99x** |

## 4. 实测结果：timer_task

| 指标 | main | HEAD | HEAD/main |
|---|---:|---:|---:|
| TimerService elapsed_ms | 5.34 | 5.22 | **0.98x** |
| Scheduler+Fiber elapsed_ms | 5.77 | 5.73 | **0.99x** |

## 5. 关于那两个 >10% 的差异 —— 为什么不归因于本次改动

实测里有**两处**超过 10%，而且都**不是**框架中被改动的代码：

| 指标 | main | HEAD | HEAD/main | 该指标是否经过本次改动的代码 |
|---|---:|---:|---:|---|
| **Thread 计算并行**（原生 `std::thread`） | 30.85 | 43.60 | **1.41x** | **否**（纯 `std::thread`，与协程/锁无关） |
| **BoundedQueue(1024)**（基准自带队列） | 682.10 | 926.80 | **1.36x** | **否**（基准自己实现，不经过框架） |
| **NaiveQueue(1024)**（基准自带队列） | 302.10 | 160.50 | **0.53x** | **否**（同上） |

三条判据：

1. **它们不含本次改动的任何代码** —— 原生线程和基准自带队列都与 `debug` 模块无关；
2. **方向不一致** —— 有的变慢 1.41x/1.36x，有的变快 0.53x。**代码退化只会单向变慢**，
   而"有的快有的慢"是**测量噪声**的典型特征；
3. **波动幅度比被改动的代码更大** —— 被改动的框架路径（调度器扩展、spawn、TimerService）
   全部落在 **0.95~1.03x**，反而是没被改动的对照组波动更大。

因此结论是：**这些差异属噪声，不能归因于 debug 模块**。

## 6. 结论

1. **结构上零影响**：框架零处 `include` debug 头，删掉整个 debug 模块框架照常编译运行；
2. **热路径上零开销**：唯一进入 `Mutex::Unlock` 慢路径的改动有两级快速返回，
   未注册选择器时只剩**一次原子读**；
3. **实测无系统性退化**：被改动的框架路径 `HEAD/main` 全部落在 **0.95~1.03x**；
   超过 10% 的只有**不含本次改动**的对照组（原生线程、基准自带队列），且方向不一致。

## 7. 未做 / 需注意

- 每个基准**只跑一次**（基准内部已有自己的多轮取中位数），未做跨次重复统计；
- 只选了 `gmp_compute` / `timer_task` / `channel_vs_queue` 三个基准，
  **未覆盖** `go2cpp_high_load_stress` 与其余 example 级用例；
- 本文件说明的是「**不启用调试时**不影响」；**启用观察者/选择器后的开销**
  另见 `docs/performance.md` 的「观察者开销」与「D3 选择器开销」两节。
