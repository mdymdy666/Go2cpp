// go_sched_bench.go —— 台账 M16（Go scheduler 基线）的可复现基准。
//
// 用途：测量 goroutine 起停的单位成本（每次 `go f()` + wg.Wait 摊到单个 task 的
// 纳秒数），作为 Go 调度器基线；与 C++ 侧同口径基准
// tests/concurrency/bench_scheduler_task_opt.cpp（产物 /tmp/bench_sched）对照。
// 对应台账条目见 docs/metrics-ledger.md 的 M16，使用说明见同目录 README.md。
//
// 编译（临时模块，不把 go.mod/go.sum 带进仓库）：
//   cd tools/bench && go mod init gobench 2>/dev/null; go build -o /tmp/go_sched_bench go_sched_bench.go
//
// 运行（参数依次为：任务数、轮数、GOMAXPROCS）：
//   taskset -c 2,3 /tmp/go_sched_bench 100000 5 4
//
// 期望量级（Go 1.27.1 / 12 vCPU / taskset -c 2,3 / 100k×5 → @4）：约 150–165 ns/task。
// 该区间只作量级参考，不是验收判据；机器、核数或负载变化后需重测。
package main

import (
	"fmt"
	"os"
	"runtime"
	"sort"
	"strconv"
	"sync"
	"time"
)

func main() {
	n := 100000
	reps := 5
	procs := 4
	if len(os.Args) > 1 {
		n, _ = strconv.Atoi(os.Args[1])
	}
	if len(os.Args) > 2 {
		reps, _ = strconv.Atoi(os.Args[2])
	}
	if len(os.Args) > 3 {
		procs, _ = strconv.Atoi(os.Args[3])
	}
	prev := runtime.GOMAXPROCS(procs)
	gogc := os.Getenv("GOGC")
	if gogc == "" {
		gogc = "(default 100)"
	}
	fmt.Printf("go=%s GOMAXPROCS=%d(prev=%d) N=%d reps=%d GOGC=%s\n",
		runtime.Version(), runtime.GOMAXPROCS(0), prev, n, reps, gogc)
	var ms runtime.MemStats
	runtime.ReadMemStats(&ms)
	fmt.Printf("NumGoroutine(start)=%d NumGC(start)=%d\n", runtime.NumGoroutine(), ms.NumGC)

	vals := make([]float64, 0, reps)
	for r := 0; r < reps; r++ {
		var wg sync.WaitGroup
		wg.Add(n)
		t0 := time.Now()
		for i := 0; i < n; i++ {
			go func() { wg.Done() }()
		}
		wg.Wait()
		el := time.Since(t0)
		ns := float64(el.Nanoseconds()) / float64(n)
		vals = append(vals, ns)
		var m2 runtime.MemStats
		runtime.ReadMemStats(&m2)
		fmt.Printf("[round %d] wall_ns_per_task=%.2f total_ms=%.3f gc_delta=%d alloc_MiB=%.1f\n",
			r, ns, float64(el.Nanoseconds())/1e6, m2.NumGC-ms.NumGC,
			float64(m2.TotalAlloc-ms.TotalAlloc)/(1024*1024))
		ms = m2
	}
	sort.Float64s(vals)
	med := vals[len(vals)/2]
	fmt.Printf("median_ns_per_task=%.2f min=%.2f max=%.2f NumGoroutine(end)=%d\n",
		med, vals[0], vals[len(vals)-1], runtime.NumGoroutine())
}
