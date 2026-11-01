/**
 * @file bench_distributed.cpp
 * @brief 分布式原语基准：单线程延迟、多线程锁竞争、查找规模、清扫开销、TCP 往返。
 *
 * 只构建、不注册 ctest（`cmake/Go2CppDistributed.cmake` 的注释说明了原因：
 * 计时噪声不应进入回归门禁）。用：
 *   ./go2cpp_distributed_bench
 * 输出每项 ns/op 与 p50/p95/p99，机器/负载条件写在报告里。
 */

#include "go2cpp/distributed/coordinator.hpp"
#include "go2cpp/distributed/election.hpp"
#include "go2cpp/distributed/lock.hpp"
#include "go2cpp/distributed/registry.hpp"
#include "go2cpp/distributed/transport.hpp"

#include "test_support.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace go2cpp::distributed;
using Clock = std::chrono::steady_clock;

struct Summary {
    double mean_ns{0};
    double p50_ns{0};
    double p95_ns{0};
    double p99_ns{0};
    double ops_per_second{0};
};

Summary Summarize(std::vector<double> samples, double wall_seconds) {
    Summary summary;
    if (samples.empty()) {
        return summary;
    }
    std::sort(samples.begin(), samples.end());
    double total = 0;
    for (const double sample : samples) {
        total += sample;
    }
    summary.mean_ns = total / static_cast<double>(samples.size());
    const auto percentile = [&samples](double fraction) {
        const std::size_t index = static_cast<std::size_t>(
            fraction * static_cast<double>(samples.size() - 1));
        return samples[index];
    };
    summary.p50_ns = percentile(0.50);
    summary.p95_ns = percentile(0.95);
    summary.p99_ns = percentile(0.99);
    summary.ops_per_second = wall_seconds > 0
                                 ? static_cast<double>(samples.size()) / wall_seconds
                                 : 0;
    return summary;
}

template <typename Fn>
Summary Measure(int iterations, Fn &&fn) {
    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(iterations));
    const auto started = Clock::now();
    for (int index = 0; index < iterations; ++index) {
        const auto begin = Clock::now();
        fn(index);
        const auto end = Clock::now();
        samples.push_back(std::chrono::duration<double, std::nano>(end - begin).count());
    }
    const double wall =
        std::chrono::duration<double>(Clock::now() - started).count();
    return Summarize(std::move(samples), wall);
}

void Print(const char *name, const Summary &summary) {
    std::printf("%-34s mean %9.1f ns  p50 %9.1f  p95 %9.1f  p99 %9.1f  %10.0f ops/s\n",
                name, summary.mean_ns, summary.p50_ns, summary.p95_ns, summary.p99_ns,
                summary.ops_per_second);
}

void BenchSingleThread() {
    CoordinatorOptions options;
    options.sweep_interval = std::chrono::milliseconds(10);
    Coordinator coordinator(options);
    DistributedLock locks(coordinator);
    ServiceRegistry registry(coordinator);
    LeaderElection election(coordinator);

    // 注册压测：每次注册后立即释放，避免 2 万条记录把后续 Lookup 打爆。
    ServiceRegistration registration;
    registration.service = "bench-register";
    registration.owner = "bench-node";
    registration.ttl = std::chrono::seconds(10);
    const Summary register_summary = Measure(20000, [&](int index) {
        registration.instance = "inst-" + std::to_string(index);
        const auto result = coordinator.RegisterService(registration);
        if (result.ok()) {
            (void)coordinator.ReleaseLease(result.value.id, registration.owner);
        }
    });
    Print("register_release_service", register_summary);

    registration.service = "bench-renew";
    registration.instance = "renew";
    const auto seeded = coordinator.RegisterService(registration);
    GO2CPP_REQUIRE(seeded.ok());
    const LeaseId lease = seeded.value.id;
    const Summary renew_summary =
        Measure(20000, [&](int) { (void)coordinator.RenewLease(lease, registration.owner); });
    Print("renew_lease", renew_summary);

    LockOptions manual;
    manual.auto_renew = false;
    const Summary lock_summary = Measure(10000, [&](int) {
        auto acquired = locks.Acquire("bench-lock", "bench", std::chrono::seconds(10), manual);
        GO2CPP_CHECK(acquired.ok());
        if (acquired.ok()) {
            acquired.value.Release();
        }
    });
    Print("acquire_release_lock", lock_summary);

    // 独立的查找规模用例：10/100/1000 个实例，互不污染。
    for (const int instances : {10, 100, 1000}) {
        Coordinator lookup_coordinator(options);
        ServiceRegistry lookup_registry(lookup_coordinator);
        std::vector<Registration> handles;
        handles.reserve(static_cast<std::size_t>(instances));
        for (int index = 0; index < instances; ++index) {
            auto result = lookup_registry.Register("scale", "inst-" + std::to_string(index),
                                                   "10.0.0.1:9000",
                                                   std::chrono::seconds(30));
            GO2CPP_REQUIRE(result.ok());
            handles.push_back(std::move(result.value));
        }
        const Summary summary =
            Measure(2000, [&](int) { (void)lookup_registry.Lookup("scale"); });
        char name[64];
        std::snprintf(name, sizeof(name), "lookup_%d_instances", instances);
        Print(name, summary);
    }

    auto first_candidate = election.Join("bench-election", "node-a", std::chrono::seconds(10));
    auto second_candidate = election.Join("bench-election", "node-b", std::chrono::seconds(10));
    GO2CPP_REQUIRE(first_candidate.ok() && second_candidate.ok());
    const Summary election_summary =
        Measure(10000, [&](int) { (void)election.State("bench-election"); });
    Print("query_election_2_nodes", election_summary);
}

void BenchSweep() {
    CoordinatorOptions options;
    options.sweep_interval = std::chrono::milliseconds(1000);
    options.min_ttl = std::chrono::milliseconds(10);
    options.max_ttl = std::chrono::seconds(60);
    Coordinator coordinator(options);
    constexpr int kLeases = 20000;
    for (int index = 0; index < kLeases; ++index) {
        ServiceRegistration registration;
        registration.service = "sweep";
        registration.instance = "inst-" + std::to_string(index);
        registration.owner = "bench";
        registration.ttl = std::chrono::milliseconds(20);
        GO2CPP_REQUIRE(coordinator.RegisterService(registration).ok());
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    const auto begin = Clock::now();
    const std::size_t swept = coordinator.SweepExpired();
    const auto end = Clock::now();
    const double milliseconds =
        std::chrono::duration<double, std::milli>(end - begin).count();
    std::printf("%-34s swept %zu leases in %.2f ms (%.1f ns/lease)\n", "sweep_20000_expired",
                swept, milliseconds, milliseconds * 1e6 / static_cast<double>(kLeases));
}

void BenchContendedLock() {
    for (const int threads_count : {1, 2, 4, 8, 16}) {
        CoordinatorOptions options;
        options.sweep_interval = std::chrono::milliseconds(100);
        Coordinator coordinator(options);
        DistributedLock locks(coordinator);
        LockOptions manual;
        manual.auto_renew = false;
        constexpr int kPerThread = 2000;
        std::atomic<int> completed{0};
        std::atomic<int> errors{0};
        std::atomic<bool> start{false};
        std::vector<std::thread> threads;
        threads.reserve(static_cast<std::size_t>(threads_count));
        const auto wall_begin = Clock::now();
        std::vector<std::vector<double>> latencies(static_cast<std::size_t>(threads_count));
        for (int worker = 0; worker < threads_count; ++worker) {
            latencies[static_cast<std::size_t>(worker)].reserve(kPerThread);
            threads.emplace_back([&, worker] {
                while (!start.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                int done = 0;
                int attempts = 0;
                while (done < kPerThread && attempts < kPerThread * 100000) {
                    ++attempts;
                    const auto begin = Clock::now();
                    auto acquired = locks.Acquire("contended", "w" + std::to_string(worker),
                                                  std::chrono::seconds(10), manual);
                    if (!acquired.ok()) {
                        if (acquired.status != Status::kConflict) {
                            errors.fetch_add(1);
                            return;
                        }
                        std::this_thread::yield();
                        continue;
                    }
                    const auto end = Clock::now();
                    latencies[static_cast<std::size_t>(worker)].push_back(
                        std::chrono::duration<double, std::nano>(end - begin).count());
                    acquired.value.Release();
                    ++done;
                    completed.fetch_add(1, std::memory_order_relaxed);
                }
                if (done < kPerThread) {
                    errors.fetch_add(1);
                }
            });
        }
        start.store(true, std::memory_order_release);
        for (std::thread &thread : threads) {
            thread.join();
        }
        const double wall =
            std::chrono::duration<double>(Clock::now() - wall_begin).count();
        std::vector<double> all;
        for (auto &latency : latencies) {
            all.insert(all.end(), latency.begin(), latency.end());
        }
        const Summary summary = Summarize(std::move(all), wall);
        char name[64];
        std::snprintf(name, sizeof(name), "contended_lock_%d_threads", threads_count);
        Print(name, summary);
        GO2CPP_CHECK(errors.load() == 0);
        GO2CPP_CHECK(completed.load() == threads_count * kPerThread);
    }
}

void BenchTransport() {
    CoordinatorOptions options;
    options.sweep_interval = std::chrono::milliseconds(20);
    Coordinator coordinator(options);
    CoordinatorServer server(coordinator);
    GO2CPP_REQUIRE(server.Start().ok());
    CoordinatorClient client("127.0.0.1", server.port());
    GO2CPP_REQUIRE(client.Connect().ok());
    ServiceRegistry registry(client);
    DistributedLock locks(client);

    // TCP 注册用裸客户端：`ServiceRegistry::Register` 会为每个实例启动
    // 一个心跳 + 本地租约线程，测出来的是线程创建开销而不是 RPC 往返。
    std::string instance = "remote-0";
    const Summary register_summary = Measure(2000, [&](int index) {
        instance = "remote-" + std::to_string(index);
        ServiceRegistration registration;
        registration.service = "remote-reg";
        registration.instance = instance;
        registration.endpoint = "10.0.0.1:9000";
        registration.owner = "bench";
        registration.ttl = std::chrono::seconds(30);
        const auto registered = client.RegisterService(registration);
        if (registered.ok()) {
            (void)client.ReleaseLease(registered.value.id, registration.owner);
        }
    });
    Print("tcp_register_release", register_summary);

    auto registration = registry.Register("remote", "renew-me", "10.0.0.1:9000",
                                          std::chrono::seconds(30));
    GO2CPP_REQUIRE(registration.ok());
    const Summary renew_summary = Measure(2000, [&](int) { (void)registration.value.Renew(); });
    Print("tcp_renew", renew_summary);

    // 查找规模：服务端 2000 个实例，客户端每次取全量快照。
    for (int index = 0; index < 2000; ++index) {
        ServiceRegistration extra;
        extra.service = "remote";
        extra.instance = "bulk-" + std::to_string(index);
        extra.endpoint = "10.0.0.1:9000";
        extra.owner = "bench";
        extra.ttl = std::chrono::seconds(60);
        GO2CPP_REQUIRE(client.RegisterService(extra).ok());
    }
    const Summary lookup_summary = Measure(2000, [&](int) { (void)registry.Lookup("remote"); });
    Print("tcp_lookup_2001_instances", lookup_summary);

    LockOptions manual;
    manual.auto_renew = false;
    const Summary lock_summary = Measure(2000, [&](int) {
        auto acquired = locks.Acquire("remote-lock", "bench", std::chrono::seconds(30), manual);
        if (acquired.ok()) {
            acquired.value.Release();
        }
    });
    Print("tcp_acquire_release", lock_summary);

    registration.value.Stop();
    client.Close();
    server.Stop();
}

} // namespace

int main() {
    std::printf("== go2cpp distributed benchmark ==\n");
    BenchSingleThread();
    BenchSweep();
    BenchContendedLock();
    BenchTransport();
    std::printf("== done ==\n");
    return go2cpp_tests::g_failures.load() == 0 ? 0 : 1;
}
