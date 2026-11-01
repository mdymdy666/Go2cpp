/**
 * @file test_concurrent.cpp
 * @brief 多线程并发：锁互斥、服务注册/发现竞态、选主唯一性与混合负载。
 *
 * 与单线程功能测试的区别：这里所有断言都在多个线程同时操作同一个
 * `Coordinator` 时成立，依赖的是协调者内部的单锁与 fencing 语义，而不是
 * 调用方自己的串行化。
 */

#include "go2cpp/distributed/coordinator.hpp"
#include "go2cpp/distributed/election.hpp"
#include "go2cpp/distributed/lock.hpp"
#include "go2cpp/distributed/registry.hpp"

#include "distributed_test_support.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace go2cpp::distributed;
using namespace go2cpp::distributed::test;

void TestConcurrentLockMutualExclusion() {
    go2cpp_tests::announce("concurrent.lock_mutex");
    Coordinator coordinator(MakeOptions());
    DistributedLock locks(coordinator);
    constexpr int kThreads = 64;
    constexpr int kIterations = 300;
    LockOptions manual;
    manual.auto_renew = false;

    std::atomic<int> inside{0};
    std::atomic<int> max_inside{0};
    std::atomic<std::int64_t> successes{0};
    std::atomic<int> errors{0};
    std::atomic<bool> start{false};

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int worker = 0; worker < kThreads; ++worker) {
        threads.emplace_back([&, worker] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (int iteration = 0; iteration < kIterations; ++iteration) {
                auto acquired = locks.Acquire("hot-key", "worker-" + std::to_string(worker),
                                              2000ms, manual);
                if (!acquired.ok()) {
                    if (acquired.status != Status::kConflict) {
                        errors.fetch_add(1, std::memory_order_relaxed);
                        return;
                    }
                    --iteration;  // 冲突不是失败：重试同一轮。
                    std::this_thread::yield();
                    continue;
                }
                LockHandle handle = std::move(acquired.value);
                const int now_inside = inside.fetch_add(1, std::memory_order_acq_rel) + 1;
                int observed = max_inside.load(std::memory_order_relaxed);
                while (now_inside > observed &&
                       !max_inside.compare_exchange_weak(observed, now_inside)) {
                }
                if (now_inside != 1) {
                    errors.fetch_add(1, std::memory_order_relaxed);
                }
                if (!handle.valid()) {
                    errors.fetch_add(1, std::memory_order_relaxed);
                }
                inside.fetch_sub(1, std::memory_order_acq_rel);
                handle.Release();
                successes.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    start.store(true, std::memory_order_release);
    for (std::thread &thread : threads) {
        thread.join();
    }

    GO2CPP_CHECK(errors.load() == 0);
    GO2CPP_CHECK(max_inside.load() == 1);
    GO2CPP_CHECK(successes.load() == static_cast<std::int64_t>(kThreads) * kIterations);
    GO2CPP_CHECK(!locks.Query("hot-key").has_value());
}

void TestConcurrentServiceChurn() {
    go2cpp_tests::announce("concurrent.service_churn");
    Coordinator coordinator(MakeOptions());
    constexpr int kWriters = 8;
    constexpr int kReaders = 4;
    constexpr int kIterations = 80;
    std::atomic<bool> stop{false};
    std::atomic<int> errors{0};

    std::vector<std::thread> readers;
    readers.reserve(kReaders);
    for (int reader = 0; reader < kReaders; ++reader) {
        readers.emplace_back([&] {
            while (!stop.load(std::memory_order_acquire)) {
                const auto instances = coordinator.LookupService("churn");
                std::set<std::string> seen;
                for (const ServiceInstance &instance : instances) {
                    if (instance.remaining <= Duration::zero() ||
                        !seen.insert(instance.instance).second) {
                        errors.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
        });
    }

    std::vector<std::thread> writers;
    writers.reserve(kWriters);
    for (int writer = 0; writer < kWriters; ++writer) {
        writers.emplace_back([&, writer] {
            for (int iteration = 0; iteration < kIterations; ++iteration) {
                ServiceRegistration registration;
                registration.service = "churn";
                registration.instance = "w" + std::to_string(writer) + "-" + std::to_string(iteration);
                registration.endpoint = "10.10.0." + std::to_string(writer);
                registration.owner = registration.instance;
                registration.ttl = 500ms;
                const auto registered = coordinator.RegisterService(registration);
                if (!registered.ok()) {
                    errors.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                if (iteration % 2 == 0) {
                    // 一半主动注销，一半留给 TTL 过期/后续覆盖。
                    const auto released =
                        coordinator.ReleaseLease(registered.value.id, registration.owner);
                    if (!released.ok() && released.status != Status::kNotFound) {
                        errors.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
        });
    }
    for (std::thread &thread : writers) {
        thread.join();
    }
    stop.store(true, std::memory_order_release);
    for (std::thread &thread : readers) {
        thread.join();
    }
    GO2CPP_CHECK(errors.load() == 0);
}

void TestConcurrentElectionSingleLeader() {
    go2cpp_tests::announce("concurrent.election_unique");
    Coordinator coordinator(MakeOptions());
    LeaderElection election(coordinator);
    auto node1 = election.Join("race", "node-1", 250ms);
    auto node2 = election.Join("race", "node-2", 250ms);
    auto node3 = election.Join("race", "node-3", 250ms);
    GO2CPP_REQUIRE(node1.ok() && node2.ok() && node3.ok());

    std::atomic<bool> stop{false};
    std::atomic<int> errors{0};
    std::atomic<int> observations{0};
    std::thread observer([&] {
        while (!stop.load(std::memory_order_acquire)) {
            const ElectionState state = election.State("race");
            observations.fetch_add(1, std::memory_order_relaxed);
            int leaders = 0;
            for (const CandidateInfo &candidate : state.candidates) {
                if (candidate.leader) {
                    ++leaders;
                }
            }
            if (leaders > 1) {
                errors.fetch_add(1, std::memory_order_relaxed);
            }
            if (!state.leader.empty()) {
                if (leaders != 1) {
                    errors.fetch_add(1, std::memory_order_relaxed);
                }
                if (state.candidates.empty() ||
                    state.candidates.front().node != state.leader) {
                    errors.fetch_add(1, std::memory_order_relaxed);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    SleepMs(120);
    GO2CPP_CHECK(election.Leader("race")->node == "node-1");
    node1.value.Leave();
    SleepMs(250);
    const auto next = election.Leader("race");
    GO2CPP_REQUIRE(next.has_value());
    GO2CPP_CHECK(next->node == "node-2");
    GO2CPP_CHECK(next->epoch >= 2);

    stop.store(true, std::memory_order_release);
    observer.join();
    GO2CPP_CHECK(errors.load() == 0);
    GO2CPP_CHECK(observations.load() > 0);
}

void TestConcurrentMixedClients() {
    go2cpp_tests::announce("concurrent.mixed");
    Coordinator coordinator(MakeOptions());
    constexpr int kThreads = 8;
    std::atomic<bool> stop{false};
    std::atomic<int> errors{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int worker = 0; worker < kThreads; ++worker) {
        threads.emplace_back([&, worker] {
            ServiceRegistry registry(coordinator);
            DistributedLock locks(coordinator);
            LeaderElection election(coordinator);
            const std::string name = "worker-" + std::to_string(worker);
            while (!stop.load(std::memory_order_acquire)) {
                auto registration = registry.Register("mixed", name, "10.11.0." + std::to_string(worker),
                                                      200ms);
                if (!registration.ok()) {
                    errors.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                if (registry.Lookup("mixed").empty()) {
                    errors.fetch_add(1, std::memory_order_relaxed);
                }
                auto candidate = election.Join("mixed-election", name, 200ms);
                if (!candidate.ok()) {
                    errors.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                if (!candidate.value.is_leader()) {
                    // 非主也可以正常续约与查询。
                    if (!candidate.value.Refresh().ok()) {
                        errors.fetch_add(1, std::memory_order_relaxed);
                    }
                }
                auto acquired = locks.Acquire("mixed-lock", name, 200ms);
                if (acquired.ok()) {
                    acquired.value.Release();
                } else if (acquired.status != Status::kConflict) {
                    errors.fetch_add(1, std::memory_order_relaxed);
                }
                registration.value.Stop();
                candidate.value.Leave();
            }
        });
    }
    SleepMs(700);
    stop.store(true, std::memory_order_release);
    for (std::thread &thread : threads) {
        thread.join();
    }
    GO2CPP_CHECK(errors.load() == 0);
    // 所有句柄都在循环末尾释放；清扫后不应有残留的锁/实例。
    SleepMs(250);
    coordinator.SweepExpired();
    GO2CPP_CHECK(coordinator.Stats().active_leases == 0);
    GO2CPP_CHECK(coordinator.LookupService("mixed").empty());
}

} // namespace

int main() {
    go2cpp_tests::announce("distributed.concurrent");
    TestConcurrentLockMutualExclusion();
    TestConcurrentServiceChurn();
    TestConcurrentElectionSingleLeader();
    TestConcurrentMixedClients();

    const int failures = go2cpp_tests::g_failures.load();
    if (failures != 0) {
        std::fprintf(stderr, "distributed_concurrent_test: %d checks failed\n", failures);
        return 1;
    }
    std::printf("distributed_concurrent_test: all cases passed\n");
    return 0;
}
