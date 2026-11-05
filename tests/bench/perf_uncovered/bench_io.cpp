// io 模块微基准：IOManager 事件循环吞吐（提交→执行→完成）ops/s。
//
// 口径：IOManager 启动后，主线程批量 mgr.go(空任务)，自旋等待全部完成；
// 一次 op = 一个空任务从提交到执行完毕（含 Scheduler 入队、worker 唤醒、
// Fiber 执行、Task 回收）。这是 IOManager 事件循环的极限吞吐，不含真实
// fd 等待 —— 真实 IO 的吞吐由 fd 就绪频率决定，不在微基准能覆盖的范围。

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <thread>

#include "bench_common.hpp"
#include "go2cpp/io.hpp"

int main() {
    go2cpp::io::IOManager manager;
    if (!manager.start()) {
        std::printf("io_start_failed=1\n");
        return 1;
    }

    std::atomic<std::size_t> done{0};

    bench::RunAdaptive("io_manager_task", 2000, 150.0, [&](std::size_t n) {
        done.store(0, std::memory_order_relaxed);
        for (std::size_t i = 0; i < n; ++i) {
            manager.go([&done]() { done.fetch_add(1, std::memory_order_relaxed); });
        }
        const auto deadline = bench::Clock::now() + std::chrono::seconds(20);
        while (done.load(std::memory_order_relaxed) < n) {
            if (bench::Clock::now() > deadline) {
                std::printf("io_wait_timeout done=%zu n=%zu\n",
                            done.load(std::memory_order_relaxed), n);
                return;
            }
            std::this_thread::yield();
        }
        bench::g_sink = done.load(std::memory_order_relaxed);
    });

    manager.shutdown();
    return 0;
}
