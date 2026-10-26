# 定时任务性能基准

> **已合并至 [docs/modules/timer.md](modules/timer.md)（2026-10-26）**。
> 本文保留作历史快照：其中的路径、行号、计数与性能数字都可能已过期，
> 现行接口、设计与测试以模块文档为准。


`benchmarks/timer_task_benchmark.cpp` 对同一批 5ms 截止时间任务测量三种模型：每任务一个普通线程、Go2Cpp `TimerService` 单计时线程、Scheduler 的 P/Fiber 执行器。输出包括总耗时和回调相对计划时间的 p50/p99 延迟。

构建和运行：

```sh
cmake -S . -B build-bench -DGO2CPP_BUILD_BENCHMARKS=ON -DGO2CPP_BUILD_TESTS=OFF -DGO2CPP_BUILD_EXAMPLES=OFF
cmake --build build-bench --target go2cpp_timer_task_benchmark --parallel 2
GO2CPP_TIMER_BENCH_TASKS=2000 ./build-bench/go2cpp_timer_task_benchmark
```

`GO2CPP_TIMER_BENCH_TASKS` 可设为 10000 等更大数量（最大 100000）。普通线程基准会创建同样数量的线程，数量过大时可能受系统线程上限影响；这属于对比项本身的成本，不是测试失败。该程序不自动链接 coost，避免把外部库混入默认构建；比较 coost 时请在 coost 自己的构建中使用相同任务数、相同 5ms 延迟和相同机器，并记录编译器与 CPU。

这是调度开销实验，不代表真实 IO 吞吐。TimerService 回调在计时线程执行；Scheduler 回调先由 TimerService 到期，再投递到 Fiber，因而延迟包含一次调度开销。

## 本次实测记录

在同一 WSL 主机、Release 优化、200 个任务、约 5 ms 延迟下，Go2Cpp 基准输出为：

| 模型 | 总耗时 | 延迟 P50/P99 |
| --- | ---: | ---: |
| 每任务一个普通线程 | 27.91 ms | 8911 / 18856 us |
| Go2Cpp TimerService | 5.50 ms | 444 / 471 us |
| Go2Cpp Scheduler + Fiber | 6.41 ms | 943 / 1357 us |

同一台主机用 /mnt/e/CodexWorkspace/Go2Cpp/coost 的 Release 静态库做了独立对照，调用 flag::parse 后启动 coost scheduler，每个协程执行 co::sleep(5)，200 个任务的结果为 7.11 ms、P50/P99=238/773 us。coost 这一项使用的是相对睡眠接口，而 Go2Cpp 使用同一个绝对 deadline，因此只能作为量级参考，不能当作严格同语义排名；编译时 coost 的汇编文件还报告了缺少 .note.GNU-stack 的链接警告。基准不把 coost 作为项目依赖，避免污染默认构建。

