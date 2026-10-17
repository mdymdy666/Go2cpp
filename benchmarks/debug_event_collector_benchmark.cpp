/**
 * @file debug_event_collector_benchmark.cpp
 * @brief 事件采集「线程局部批量提交」相对「逐条提交」的收益实测（build-bench / Release）。
 *
 * 背景：`DebugEventCollector::OnEvent` 现在是"攒够 flush_batch 条再一次性 PushBatch"。
 *       `EventBuffer` 本身是**无锁**环形缓冲，所以这个优化省的是**原子操作次数**而不是锁争用。
 *       本基准就是量化"省下来到底值多少"。
 *
 * 三个档：
 *   档 1：flush_batch = 1   —— 退化为逐条 Push（等价于优化前的行为）
 *   档 2：flush_batch = 16
 *   档 3：flush_batch = 32  （默认值）
 *
 * 两个场景：
 *   A 单线程：主线程直接调 OnEvent N 次（测纯 Push 路径的原子开销）
 *   B 4 线程并发：4 个线程各调 OnEvent N 次（测多线程下原子争用的差异）
 *
 * 每个测量点结束后调用 Flush()/Drain() 把剩余不足一批的事件提交并清空，避免影响下一轮。
 *
 * 用法：./go2cpp_debug_event_collector_benchmark [轮数] [每线程事件数]
 */

#include "go2cpp/debug.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using go2cpp::SchedulerEvent;
using go2cpp::SchedulerEventType;
using go2cpp::debug::DebugEventCollector;

SchedulerEvent MakeEvent(std::uint64_t id) {
    SchedulerEvent event;
    event.type = SchedulerEventType::kTaskStarted;
    event.task_id = id;
    event.timestamp = Clock::now();
    return event;
}

/// 单线程：调 OnEvent count 次，再 Flush。
double SingleThreadPerEvent(DebugEventCollector& collector, std::size_t count) {
    const auto started = Clock::now();
    for (std::size_t i = 0; i < count; ++i) {
        collector.OnEvent(MakeEvent(i));
    }
    (void)collector.Flush();
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started);
    return static_cast<double>(elapsed.count()) / static_cast<double>(count);
}

/// 多线程：threads 个线程各调 per_thread 次，再各自 Flush。
struct MultiResult {
    double ns_per_event{0};
    std::uint64_t captured{0};
    std::uint64_t dropped{0};
};

MultiResult MultiThread(DebugEventCollector& collector, int threads, std::size_t per_thread) {
    std::vector<std::thread> workers;
    workers.reserve(static_cast<std::size_t>(threads));
    const auto started = Clock::now();
    for (int t = 0; t < threads; ++t) {
        workers.emplace_back([&collector, per_thread, t] {
            for (std::size_t i = 0; i < per_thread; ++i) {
                collector.OnEvent(MakeEvent(static_cast<std::uint64_t>(t) * 1000000U + i));
            }
            (void)collector.Flush();   // 提交本线程剩余不足一批的事件
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started);
    const auto total = static_cast<std::size_t>(threads) * per_thread;
    MultiResult result;
    result.ns_per_event = total > 0 ? static_cast<double>(elapsed.count()) / static_cast<double>(total) : 0.0;
    result.captured = collector.Captured();
    result.dropped = collector.Dropped();
    collector.Drain();   // 清空，避免影响下一轮
    return result;
}

template <typename Fn>
void Report(const char* label, int repeats, Fn&& run) {
    std::vector<double> samples;
    for (int i = 0; i < repeats; ++i) {
        samples.push_back(run());
    }
    std::sort(samples.begin(), samples.end());
    std::printf("  %-34s min %8.2f  median %8.2f  max %8.2f  ns/事件\n",
                label, samples.front(), samples[samples.size() / 2], samples.back());
    std::fflush(stdout);
}

}  // namespace

int main(int argc, char** argv) {
    int repeats = 5;
    std::size_t per_thread = 200000;
    if (argc > 1) { const int v = std::atoi(argv[1]); if (v > 0) repeats = v; }
    if (argc > 2) { const long v = std::atol(argv[2]); if (v > 0) per_thread = static_cast<std::size_t>(v); }

    std::printf("=== 事件采集：线程局部批量提交 vs 逐条提交（build-bench / Release）===\n");
    std::printf("参数：轮数=%d 每档事件数=%zu\n", repeats, per_thread);

    const std::vector<std::size_t> batches = {1, 16, 32};

    std::printf("\n[A] 单线程直接调 OnEvent（纯 Push 路径的原子开销）\n");
    for (std::size_t batch : batches) {
        DebugEventCollector collector(1U << 20);   // 足够大，避免因满而丢弃
        collector.SetFlushBatch(batch);
        (void)SingleThreadPerEvent(collector, per_thread / 10);   // 热身
        collector.Drain();
        char label[64];
        std::snprintf(label, sizeof(label), "flush_batch=%zu", batch);
        Report(label, repeats, [&collector, per_thread] {
            return SingleThreadPerEvent(collector, per_thread);
        });
        collector.Drain();
    }

    std::printf("\n[B] 4 线程并发调 OnEvent（多线程下的原子争用）\n");
    for (std::size_t batch : batches) {
        DebugEventCollector collector(4U << 20);
        collector.SetFlushBatch(batch);
        (void)MultiThread(collector, 4, per_thread / 10);   // 热身
        char label[64];
        std::snprintf(label, sizeof(label), "flush_batch=%zu", batch);
        std::vector<double> samples;
        MultiResult last;
        for (int i = 0; i < repeats; ++i) {
            last = MultiThread(collector, 4, per_thread);
            samples.push_back(last.ns_per_event);
        }
        std::sort(samples.begin(), samples.end());
        std::printf("  %-34s min %8.2f  median %8.2f  max %8.2f  ns/事件  [captured=%llu dropped=%llu]\n",
                    label, samples.front(), samples[samples.size() / 2], samples.back(),
                    static_cast<unsigned long long>(last.captured),
                    static_cast<unsigned long long>(last.dropped));
        std::fflush(stdout);
    }

    std::printf("\n说明：ns/事件 来自 build-bench（Release, -O2）；每档先热身一轮再做 %d 轮取区间。\n", repeats);
    std::printf("      档 flush_batch=1 等价于优化前的逐条 Push，可与 16/32 直接对比。\n");
    std::printf("      EventBuffer 本身无锁，故这里量的是原子操作次数减少带来的收益。\n");
    return 0;
}
