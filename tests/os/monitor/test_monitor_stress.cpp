// 监控开销与稳定性压测。
//
// 目标（对齐验收要求「监控开销 <5% CPU」）：
//   1. 200 次完整采集（4 个 /proc 采集器 + 可选 eBPF map 读取）的平均耗时；
//   2. 采样期间的进程 CPU 时间占比，验证 1Hz 配置下监控自身开销远低于 5%；
//   3. 多线程并发采集不崩溃、不产生错误；
//   4. Start/Stop 反复切换 20 轮后仍能正常聚合。

#include <sys/resource.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

#include "go2cpp/os/monitor/aggregator.hpp"
#include "test_support.hpp"

namespace {

using go2cpp::os::monitor::Aggregator;
using go2cpp::os::monitor::AggregatorConfig;
using go2cpp::os::monitor::Snapshot;

double ProcessCpuMs() {
    struct rusage usage {};
    if (::getrusage(RUSAGE_SELF, &usage) != 0) {
        return 0.0;
    }
    return static_cast<double>(usage.ru_utime.tv_sec * 1000 +
                               usage.ru_utime.tv_usec / 1000) +
           static_cast<double>(usage.ru_stime.tv_sec * 1000 +
                               usage.ru_stime.tv_usec / 1000);
}

void TestCollectCost() {
    go2cpp_tests::announce("collect_cost");
    AggregatorConfig config;
    config.enable_ebpf = false;
    Aggregator aggregator(config);

    // 预热一次，排除首次打开 /proc 的冷启动成本。
    (void)aggregator.CollectNow();

    const int iterations = 200;
    const double cpu_before = ProcessCpuMs();
    const auto wall_start = std::chrono::steady_clock::now();
    for (int i = 0; i < iterations; ++i) {
        Snapshot snapshot = aggregator.CollectNow();
        if (snapshot.cpus.empty() || snapshot.mem.total_kb == 0) {
            GO2CPP_CHECK(false);
            break;
        }
    }
    const auto wall_end = std::chrono::steady_clock::now();
    const double cpu_after = ProcessCpuMs();

    const double wall_ms =
        std::chrono::duration<double, std::milli>(wall_end - wall_start).count();
    const double per_call_us = wall_ms * 1000.0 / iterations;
    const double per_call_cpu_ms = (cpu_after - cpu_before) / iterations;
    // 验收口径是「1Hz 采样下的 CPU 占比」：单次采集的 CPU 成本 / 采样间隔。
    // 不能用「连续采集期间的 CPU 占比」——连续跑没有空闲时间，该值必然
    // 接近 100%，衡量不了监控本身的开销。
    const double ratio_at_1hz = per_call_cpu_ms / 1000.0 * 100.0;
    std::printf(
        "[stress] %d collections: wall=%.2fms avg=%.1fus cpu_per_call=%.3fms "
        "cpu_share@1Hz=%.4f%%\n",
        iterations, wall_ms, per_call_us, per_call_cpu_ms, ratio_at_1hz);

    // 单次采集应在 5ms 以内；实测通常是几十微秒。
    GO2CPP_CHECK(per_call_us < 5000.0);
    // 1Hz 配置下监控自身 CPU 开销必须低于 5%。
    GO2CPP_CHECK(ratio_at_1hz < 5.0);
}

void TestConcurrentCollect() {
    go2cpp_tests::announce("concurrent_collect");
    // 每个线程独立 Aggregator（采集器内部有上次采样状态，不能共享）。
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&failures]() {
            AggregatorConfig config;
            config.enable_ebpf = false;
            Aggregator aggregator(config);
            for (int i = 0; i < 50; ++i) {
                Snapshot snapshot = aggregator.CollectNow();
                if (snapshot.cpus.empty() || snapshot.mem.total_kb == 0 ||
                    snapshot.nets.empty()) {
                    failures.fetch_add(1);
                }
            }
        });
    }
    for (auto& thread : threads) {
        GO2CPP_JOIN_WITH_WATCHDOG(thread, std::chrono::seconds(60));
    }
    GO2CPP_CHECK(failures.load() == 0);
}

void TestStartStopChurn() {
    go2cpp_tests::announce("start_stop_churn");
    AggregatorConfig config;
    config.interval = std::chrono::milliseconds(10);
    config.enable_ebpf = false;
    config.workers = 3;
    Aggregator aggregator(config);

    for (int round = 0; round < 20; ++round) {
        GO2CPP_CHECK(aggregator.Start());
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        aggregator.Stop();
        GO2CPP_CHECK(!aggregator.running());
    }
    // 反复启停后仍能正常聚合。
    GO2CPP_CHECK(aggregator.Start());
    GO2CPP_REQUIRE_EVENTUALLY(aggregator.Stats().snapshots >= 1,
                              std::chrono::seconds(10));
    aggregator.Stop();
}

void TestSustainedAggregation() {
    go2cpp_tests::announce("sustained_aggregation");
    // 模拟 10Hz 持续采集 3 秒，验证 Fiber/Timer 在长时间运行下不退化。
    AggregatorConfig config;
    config.interval = std::chrono::milliseconds(100);
    config.enable_ebpf = false;
    config.workers = 4;
    Aggregator aggregator(config);
    GO2CPP_CHECK(aggregator.Start());
    GO2CPP_REQUIRE_EVENTUALLY(aggregator.Stats().snapshots >= 20,
                              std::chrono::seconds(30));
    const auto stats = aggregator.Stats();
    std::printf("[stress] sustained snapshots=%llu errors=%llu\n",
                static_cast<unsigned long long>(stats.snapshots),
                static_cast<unsigned long long>(stats.errors));
    GO2CPP_CHECK(stats.errors == 0);
    aggregator.Stop();
}

}  // namespace

int main() {
    TestCollectCost();
    TestConcurrentCollect();
    TestStartStopChurn();
    TestSustainedAggregation();

    const int failures = go2cpp_tests::g_failures.load(std::memory_order_relaxed);
    if (failures != 0) {
        std::fprintf(stderr, "[test] failures=%d\n", failures);
        return 1;
    }
    std::printf("[test] all monitor stress checks passed\n");
    return 0;
}
