# tools/bench —— 台账 M16 的可复现基准

本目录存放 `docs/metrics-ledger.md` 里 **M16（Go scheduler 基线）** 的基准源码。
之前这份基准只存在于 `/tmp/go_sched_bench.go`，重启即失、台账不可复现；现在固化进仓库。

## 编译

`go.mod`/`go.sum` 属于一次性编译产物，**不进仓库**，按需临时生成：

```bash
cd tools/bench && go mod init gobench 2>/dev/null; go build -o /tmp/go_sched_bench go_sched_bench.go
```

## 运行

```bash
taskset -c 2,3 /tmp/go_sched_bench 100000 5 4
```

参数依次为：**任务数**（每次起的 goroutine 数）、**轮数**、**GOMAXPROCS**。
输出每轮 `wall_ns_per_task` / `total_ms` / `gc_delta`，末行为中位数与最小/最大值。

## 与台账的对应

| 台账条目 | 本基准 | C++ 对照 |
|---|---|---|
| M16 Go scheduler 基线 | `tools/bench/go_sched_bench.go` | `tests/concurrency/bench_scheduler_task_opt.cpp` → `/tmp/bench_sched` |

M16 的结论按「同轮、同机、同核位」的 Go/C++ 比值给出，不跨轮拼接数字。
本基准产出的数值只用于量级对照，**不是验收判据**；环境变了就重测。

## 注意

- **基准必须独占**：跑前确认没有其它 bench 进程
  （`pgrep -a -f "bench_|net_perf" | grep -v grep`），且 `loadavg < 2`。
- 用 `taskset` 绑核，并让 Go 与 C++ 两侧绑同样的核位，否则比值没有意义。
- 建议保留完整输出（含 `go=` 版本行与 `GOGC`）随台账一起记录。
