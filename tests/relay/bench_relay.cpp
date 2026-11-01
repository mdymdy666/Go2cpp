/**
 * @file bench_relay.cpp
 * @brief relay 组件压测：Hub 吞吐/延迟、Lease 续约成本与定时精度、Heartbeat 抖动。
 *
 * 不注册 ctest（只构建），运行：
 *   .build-bento-wsl/go2cpp_relay_bench
 *
 * 指标口径：
 * - Hub 单线程/多线程：一次迭代 = Register + Lookup + Unregister；
 * - Hub 只读：一次 = Lookup 一个已存在的 key；
 * - 定时精度：`实际触发时刻 - 预期触发时刻`，正数表示迟到；
 * - 心跳抖动：相邻两次发送的单调时间差相对 interval 的偏差。
 */
#include "go2cpp/relay/hub.hpp"
#include "go2cpp/relay/lease.hpp"

#include "go2cpp/concurrency/context.hpp"

#include "relay_test_util.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using go2cpp::relay::Heartbeat;
using go2cpp::relay::Hub;
using go2cpp::relay::Lease;
using go2cpp::relay::test::FakeStream;
using go2cpp::relay::test::Percentile;
using go2cpp::relay::test::SleepMs;
using go2cpp::relay::test::SteadyNowMs;

/// 极简连接：压测里只是 shared_ptr 的载体，Close 空实现即可。
class NullStream final : public Hub::Stream {
public:
    ::go2cpp::ErrorPtr Close() override { return {}; }
};

double ElapsedNs(std::chrono::steady_clock::time_point start) {
    return static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now() - start)
                                   .count());
}

void BenchHubSingleThread() {
    constexpr int kIterations = 1000000;
    Hub hub;
    const auto start = std::chrono::steady_clock::now();
    for (int index = 0; index < kIterations; ++index) {
        const std::string key = "room:" + std::to_string(index % 1024);
        auto conn = std::make_shared<NullStream>();
        (void)hub.Register(key, conn);
        const auto snapshot = hub.Lookup(key);
        (void)snapshot;
        hub.Unregister(key, conn);
    }
    const double ns = ElapsedNs(start);
    std::printf("[hub ] 单线程 Register+Lookup+Unregister: %d 次迭代, %.1f ns/迭代, %.2f Mops/s\n",
                kIterations, ns / kIterations,
                static_cast<double>(kIterations) / (ns / 1e9) / 1e6);
}

void BenchHubMultiThread(int thread_count, int iterations_per_thread, bool shared_key) {
    Hub hub;
    // 预置 key 与连接，避免 lookup 全部 miss：
    // - shared_key=true：所有线程争同一个 key（最坏锁竞争）；
    // - shared_key=false：每个线程用自己的 key（常见房间模型）。
    std::vector<std::shared_ptr<NullStream>> anchors;
    for (int index = 0; index < thread_count; ++index) {
        auto conn = std::make_shared<NullStream>();
        anchors.push_back(conn);
        (void)hub.Register(shared_key ? "hot" : ("room:" + std::to_string(index)), conn);
    }

    std::atomic<bool> start_flag{false};
    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(thread_count));
    for (int thread_index = 0; thread_index < thread_count; ++thread_index) {
        threads.emplace_back([&hub, &start_flag, thread_index, iterations_per_thread,
                              shared_key] {
            const std::string key =
                shared_key ? std::string("hot") : ("room:" + std::to_string(thread_index));
            while (!start_flag.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (int iteration = 0; iteration < iterations_per_thread; ++iteration) {
                auto conn = std::make_shared<NullStream>();
                (void)hub.Register(key, conn);
                (void)hub.Lookup(key);
                hub.Unregister(key, conn);
            }
        });
    }
    const auto start = std::chrono::steady_clock::now();
    start_flag.store(true, std::memory_order_release);
    for (auto &thread : threads) {
        thread.join();
    }
    const double ns = ElapsedNs(start);
    const double operations = static_cast<double>(thread_count) * iterations_per_thread;
    std::printf("[hub ] %d 线程 %s Register+Lookup+Unregister: %.0f 次迭代, %.1f ns/迭代, %.2f Mops/s\n",
                thread_count, shared_key ? "共享 1 key" : "每线程独立 key",
                operations, ns / operations, operations / (ns / 1e9) / 1e6);
    (void)anchors;
}

void BenchHubLookup(int thread_count, int iterations_per_thread) {
    Hub hub;
    for (int index = 0; index < 64; ++index) {
        (void)hub.Register("room:" + std::to_string(index), std::make_shared<NullStream>());
    }
    std::atomic<bool> start_flag{false};
    std::vector<std::thread> threads;
    for (int thread_index = 0; thread_index < thread_count; ++thread_index) {
        threads.emplace_back([&hub, &start_flag, thread_index, iterations_per_thread] {
            while (!start_flag.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            std::uint64_t sink = 0;
            for (int iteration = 0; iteration < iterations_per_thread; ++iteration) {
                const auto snapshot =
                    hub.Lookup("room:" + std::to_string((thread_index + iteration) % 64));
                sink += snapshot.size();
            }
            if (sink == 0xdeadbeefULL) {
                std::printf("sink=%llu\n", static_cast<unsigned long long>(sink));
            }
        });
    }
    const auto start = std::chrono::steady_clock::now();
    start_flag.store(true, std::memory_order_release);
    for (auto &thread : threads) {
        thread.join();
    }
    const double ns = ElapsedNs(start);
    const double operations = static_cast<double>(thread_count) * iterations_per_thread;
    std::printf("[hub ] %d 线程只读 Lookup: %.0f 次, %.1f ns/次, %.2f Mops/s\n",
                thread_count, operations, ns / operations, operations / (ns / 1e9) / 1e6);
}

void BenchHubCloseKey() {
    constexpr int kKeys = 100000;
    constexpr int kConnsPerKey = 4;
    Hub hub;
    std::atomic<std::uint64_t> closed{0};
    for (int key_index = 0; key_index < kKeys; ++key_index) {
        for (int conn_index = 0; conn_index < kConnsPerKey; ++conn_index) {
            (void)hub.Register("room:" + std::to_string(key_index),
                               std::make_shared<FakeStream>(&closed));
        }
    }
    const auto start = std::chrono::steady_clock::now();
    for (int key_index = 0; key_index < kKeys; ++key_index) {
        (void)hub.CloseKey("room:" + std::to_string(key_index));
    }
    const double ns = ElapsedNs(start);
    const double connections = static_cast<double>(kKeys) * kConnsPerKey;
    std::printf("[hub ] CloseKey 批量: %d key / %.0f 连接, %.1f ns/连接, 全部关闭=%s\n",
                kKeys, connections, ns / connections,
                closed.load() == static_cast<std::uint64_t>(connections) ? "是" : "否");
}

void BenchLeaseRenew() {
    constexpr int kRenews = 2000000;
    Lease lease(10000ms, nullptr);
    const auto start = std::chrono::steady_clock::now();
    for (int index = 0; index < kRenews; ++index) {
        lease.Renew();
    }
    const double ns = ElapsedNs(start);
    std::printf("[lea ] Renew 热路径: %d 次, %.1f ns/次, %.2f Mops/s\n", kRenews,
                ns / kRenews, static_cast<double>(kRenews) / (ns / 1e9) / 1e6);
    lease.Stop();
}

void BenchLeaseAccuracy() {
    constexpr int kLeases = 200;
    constexpr std::int64_t kTimeoutMs = 50;
    std::mutex mutex;
    std::vector<double> lateness;
    lateness.reserve(kLeases);
    std::vector<std::unique_ptr<Lease>> leases;
    leases.reserve(kLeases);
    const std::int64_t start = SteadyNowMs();
    for (int index = 0; index < kLeases; ++index) {
        leases.push_back(std::make_unique<Lease>(
            std::chrono::milliseconds(kTimeoutMs), [&mutex, &lateness, start] {
                const double late =
                    static_cast<double>(SteadyNowMs() - start - kTimeoutMs);
                const std::lock_guard<std::mutex> lock(mutex);
                lateness.push_back(late);
            }));
    }
    while (true) {
        {
            const std::lock_guard<std::mutex> lock(mutex);
            if (lateness.size() == kLeases) {
                break;
            }
        }
        SleepMs(1);
    }
    std::printf("[lea ] %d 个 %lld ms 租约触发迟到: P50=%.1f P95=%.1f P99=%.1f max=%.1f ms\n",
                kLeases, static_cast<long long>(kTimeoutMs), Percentile(lateness, 50),
                Percentile(lateness, 95), Percentile(lateness, 99),
                lateness.empty() ? 0.0 : *std::max_element(lateness.begin(), lateness.end()));
}

void BenchHeartbeatJitter() {
    constexpr std::int64_t kIntervalMs = 10;
    constexpr int kTicks = 200;
    std::mutex mutex;
    std::vector<std::int64_t> timestamps;
    timestamps.reserve(kTicks + 1);
    Heartbeat heartbeat{std::chrono::milliseconds(kIntervalMs)};
    heartbeat.Start(
        nullptr,
        [&mutex, &timestamps] {
            const std::lock_guard<std::mutex> lock(mutex);
            timestamps.push_back(SteadyNowMs());
            return true;
        },
        nullptr);
    while (true) {
        {
            const std::lock_guard<std::mutex> lock(mutex);
            if (timestamps.size() >= kTicks) {
                break;
            }
        }
        SleepMs(1);
    }
    heartbeat.Stop();

    std::vector<double> jitter;
    {
        const std::lock_guard<std::mutex> lock(mutex);
        for (std::size_t index = 1; index < timestamps.size(); ++index) {
            jitter.push_back(static_cast<double>(timestamps[index] - timestamps[index - 1] -
                                                kIntervalMs));
        }
    }
    std::printf("[hb  ] %lld ms 心跳 %d 拍：间隔抖动 P50=%.1f P95=%.1f P99=%.1f max=%.1f ms\n",
                static_cast<long long>(kIntervalMs), kTicks, Percentile(jitter, 50),
                Percentile(jitter, 95), Percentile(jitter, 99),
                jitter.empty() ? 0.0 : *std::max_element(jitter.begin(), jitter.end()));
}

} // namespace

int main() {
    std::printf("relay bench（%u 硬件线程，Release 构建）\n",
                std::thread::hardware_concurrency());
    BenchHubSingleThread();
    BenchHubMultiThread(8, 100000, false);
    BenchHubMultiThread(8, 100000, true);
    BenchHubLookup(8, 200000);
    BenchHubCloseKey();
    BenchLeaseRenew();
    BenchLeaseAccuracy();
    BenchHeartbeatJitter();
    return 0;
}
