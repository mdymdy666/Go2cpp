// Go 对照基准：1P1C `chan uint64` 的单条 ns/op。
//
// 口径与 C++ 侧 bench_fast_channel.cpp 对齐：
//   - 一个生产者 goroutine + 一个消费者 goroutine，逐条收发，逐条校验 FIFO；
//   - GOMAXPROCS=2，配合 taskset -c 2,3 绑定同一物理核的两个 SMT 线程；
//   - 每轮重新建 channel，N 轮取中位数。
//
// 运行（不需要 go.mod）：
//   cd tests/concurrency
//   GO111MODULE=off go build -o /tmp/go_channel_bench go_channel_bench.go
//   taskset -c 2,3 /tmp/go_channel_bench -cap 0 -n 200000 -reps 5
//   taskset -c 2,3 /tmp/go_channel_bench -cap 64 -n 200000 -reps 5
//   taskset -c 2,3 /tmp/go_channel_bench -cap 1024 -n 200000 -reps 5
//
// 说明：本文件不属于 CMake 构建，只用于生成报告中的 Go 对照数据。
package main

import (
	"flag"
	"fmt"
	"runtime"
	"sort"
	"sync"
	"time"
)

// runOnce 跑一轮完整的 1P1C，返回单条消息的纳秒数。
func runOnce(capacity, total int) float64 {
	ch := make(chan uint64, capacity)
	var wg sync.WaitGroup
	wg.Add(2)
	start := make(chan struct{})
	go func() {
		defer wg.Done()
		<-start
		for i := 0; i < total; i++ {
			ch <- uint64(i)
		}
	}()
	go func() {
		defer wg.Done()
		<-start
		var sum uint64
		for i := 0; i < total; i++ {
			v := <-ch
			if v != uint64(i) {
				panic("fifo violation")
			}
			sum += v
		}
		if sum == 0 {
			panic("unreachable")
		}
	}()
	t0 := time.Now()
	close(start)
	wg.Wait()
	return float64(time.Since(t0).Nanoseconds()) / float64(total)
}

// median 返回中位数；空切片返回 0。
func median(values []float64) float64 {
	if len(values) == 0 {
		return 0
	}
	sort.Float64s(values)
	return values[len(values)/2]
}

func main() {
	capacity := flag.Int("cap", 64, "channel capacity")
	total := flag.Int("n", 200000, "messages per round")
	reps := flag.Int("reps", 5, "rounds (median is reported)")
	flag.Parse()

	runtime.GOMAXPROCS(2)
	values := make([]float64, 0, *reps)
	for i := 0; i < *reps; i++ {
		values = append(values, runOnce(*capacity, *total))
	}
	fmt.Printf("go-cap%-6d median %8.2f ns/op\n", *capacity,
		median(values))
}
