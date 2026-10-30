// Future 优化的 Go 侧对照基准（一次性完成语义用 channel / sync.Once /
// errgroup 风格等价物表达）。
//
// 运行：
//   cd tests/concurrency/bench_future_go && GO111MODULE=off go run main.go
//
// 输出与 tests/concurrency/bench_future_opt.cpp 的表格逐项对应：
//   创建 / 完成（一次性发布）/ 已就绪读取 / 跨 goroutine 唤醒 / 取消 / 广播。
package main

import (
	"context"
	"flag"
	"fmt"
	"sync"
	"sync/atomic"
	"testing"
	"time"
)

var sink int64

func run(name string, f func(*testing.B)) {
	result := testing.Benchmark(f)
	fmt.Printf("%-54s %10.2f ns/op\n", name, float64(result.NsPerOp()))
}

// 创建：make(chan int, 1) + close，等价于“建一个一次性结果槽”。
func BenchmarkChanCreate(b *testing.B) {
	for i := 0; i < b.N; i++ {
		ch := make(chan int, 1)
		close(ch)
		sink += int64(cap(ch))
	}
}

// 创建 + 发布 + 读取（Future 全生命周期）。
func BenchmarkChanSetGet(b *testing.B) {
	for i := 0; i < b.N; i++ {
		ch := make(chan int, 1)
		ch <- i
		sink += int64(<-ch)
	}
}

// 已就绪读取：buffered channel 中已有值，连续发送+接收。
func BenchmarkChanReadyRecv(b *testing.B) {
	ch := make(chan int, 1)
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		ch <- 42
		sink += int64(<-ch)
	}
}

// 已关闭 channel 的读取快路径（不再有值，直接返回零值）。
func BenchmarkChanClosedRecv(b *testing.B) {
	ch := make(chan int, 1)
	ch <- 42
	close(ch)
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		sink += int64(<-ch)
	}
}

// sync.Once 已完成后的快路径（Go 1.27 once.go 的 atomic 快路径）。
func BenchmarkSyncOnceDone(b *testing.B) {
	var once sync.Once
	once.Do(func() { sink++ })
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		once.Do(func() { sink++ })
	}
}

// sync.WaitGroup 的单次 Add/Done/Wait（errgroup 汇合点的基础成本）。
func BenchmarkWaitGroup(b *testing.B) {
	var wg sync.WaitGroup
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		wg.Add(1)
		wg.Done()
		wg.Wait()
	}
}

// errgroup 风格：每个任务一个 goroutine + 结果 channel + Wait。
func BenchmarkErrgroupStyle(b *testing.B) {
	results := make(chan error, 1)
	var wg sync.WaitGroup
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			results <- nil
		}()
		wg.Wait()
		<-results
	}
}

// 跨 goroutine 的 set → wake 端到端（channel 交接）。
func BenchmarkChanGoroutineWakeup(b *testing.B) {
	request := make(chan int)
	response := make(chan int)
	go func() {
		for value := range request {
			response <- value
		}
	}()
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		request <- i
		sink += int64(<-response)
	}
	close(request)
}

// context 已取消时 select 的快路径（对应 C++ Wait(已取消 Context)）。
func BenchmarkContextDoneFastPath(b *testing.B) {
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		select {
		case <-ctx.Done():
			sink++
		default:
			sink--
		}
	}
}

// context 取消唤醒一个等待方（端到端，每轮新建 context + goroutine）。
func BenchmarkContextCancelWakeup(b *testing.B) {
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		ctx, cancel := context.WithCancel(context.Background())
		done := make(chan struct{})
		go func() {
			<-ctx.Done()
			close(done)
		}()
		cancel()
		<-done
	}
}

// 一次 close 广播唤醒 8 个等待者。
func BenchmarkCloseBroadcast(b *testing.B) {
	const waiters = 8
	gates := make(chan chan struct{}, waiters)
	acked := make(chan struct{}, waiters)
	var wg sync.WaitGroup
	wg.Add(waiters)
	for i := 0; i < waiters; i++ {
		go func() {
			defer wg.Done()
			for gate := range gates {
				<-gate
				acked <- struct{}{}
			}
		}()
	}
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		gate := make(chan struct{})
		for j := 0; j < waiters; j++ {
			gates <- gate
		}
		close(gate)
		for j := 0; j < waiters; j++ {
			<-acked
		}
	}
	b.StopTimer()
	close(gates)
	wg.Wait()
}

func main() {
	flag.Set("test.benchtime", "300ms")
	flag.Parse()
	fmt.Printf("== Future 对照基准（Go %s）==\n", "channel/once/errgroup")
	run("创建 make(chan,1)+close", BenchmarkChanCreate)
	run("创建+SetValue+Get 全生命周期", BenchmarkChanSetGet)
	run("已就绪读取（buffered recv）", BenchmarkChanReadyRecv)
	run("已关闭 channel 读取", BenchmarkChanClosedRecv)
	run("sync.Once 已完成快路径", BenchmarkSyncOnceDone)
	run("sync.WaitGroup Add/Done/Wait", BenchmarkWaitGroup)
	run("errgroup 风格（goroutine+chan+Wait）", BenchmarkErrgroupStyle)
	run("set→wake 跨 goroutine e2e", BenchmarkChanGoroutineWakeup)
	run("Context Done 已取消快路径", BenchmarkContextDoneFastPath)
	run("Context Cancel 唤醒 e2e", BenchmarkContextCancelWakeup)
	run("close 广播 8 个等待者", BenchmarkCloseBroadcast)
	_ = atomic.LoadInt64(&sink)
	_ = time.Now
}
