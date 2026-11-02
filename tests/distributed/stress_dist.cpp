/**
 * @file stress_dist.cpp
 * @brief 1e6 次续约/抢锁/选主压力测试（本地协调者），同时校验不变量。
 *
 * 独立编译（不进仓库构建）：见 /tmp/distwork/build_stress.sh。
 */

#include "go2cpp/distributed/coordinator.hpp"
#include "go2cpp/distributed/election.hpp"
#include "go2cpp/distributed/lock.hpp"
#include "go2cpp/distributed/registry.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

using namespace go2cpp::distributed;
using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;
using std::chrono::seconds;

namespace {

int g_failures = 0;

#define REQ(cond)                                                                    \
    do {                                                                             \
        if (!(cond)) {                                                               \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);              \
            ++g_failures;                                                            \
        }                                                                            \
    } while (0)

double NsPerOp(const Clock::time_point &begin, const Clock::time_point &end, long n) {
    const double ns = std::chrono::duration<double, std::nano>(end - begin).count();
    return n > 0 ? ns / static_cast<double>(n) : 0.0;
}

void Report(const char *name, long n, double ns_per_op) {
    std::printf("%-26s %9ld ops  %10.2f ns/op  %8.2f Mops/s\n", name, n, ns_per_op,
                ns_per_op > 0 ? 1e3 / ns_per_op : 0.0);
}

} // namespace

int main(int argc, char **argv) {
    const long n = argc > 1 ? std::strtol(argv[1], nullptr, 10) : 1000000;
    CoordinatorOptions options;
    options.sweep_interval = milliseconds(1000);
    options.min_ttl = milliseconds(10);
    options.max_ttl = seconds(60);
    Coordinator coordinator(options);

    // ---- 1) 续约 n 次 ----
    ServiceRegistration registration;
    registration.service = "stress";
    registration.instance = "node-0";
    registration.owner = "node-0";
    registration.ttl = seconds(60);
    const auto seeded = coordinator.RegisterService(registration);
    REQ(seeded.ok());
    const LeaseId lease = seeded.value.id;
    {
        long ok = 0;
        const auto begin = Clock::now();
        for (long i = 0; i < n; ++i) {
            const auto result = coordinator.RenewLease(lease, registration.owner);
            if (!result.ok()) {
                ++g_failures;
                break;
            }
            ++ok;
        }
        const auto end = Clock::now();
        Report("renew_lease", n, NsPerOp(begin, end, n));
        REQ(ok == n);
    }

    // ---- 2) 直接 AcquireLock/ReleaseLease n 次，fencing 严格单调 ----
    {
        const auto begin = Clock::now();
        for (long i = 0; i < n; ++i) {
            const auto acquired = coordinator.AcquireLock("stress-lock", "owner", seconds(60));
            if (!acquired.ok() ||
                acquired.value.fencing != static_cast<std::uint64_t>(i) + 1) {
                ++g_failures;
                break;
            }
            const auto released = coordinator.ReleaseLease(acquired.value.id, "owner");
            if (!released.ok()) {
                ++g_failures;
                break;
            }
        }
        const auto end = Clock::now();
        Report("acquire_release_direct", n, NsPerOp(begin, end, n));
    }

    // ---- 3) facade Acquire+Release n 次（含句柄构造/析构）----
    {
        DistributedLock locks(coordinator);
        LockOptions manual;
        manual.auto_renew = false;
        const auto begin = Clock::now();
        for (long i = 0; i < n; ++i) {
            auto acquired = locks.Acquire("stress-lock-facade", "owner", seconds(60), manual);
            if (!acquired.ok() ||
                acquired.value.fencing_token() != static_cast<std::uint64_t>(i) + 1) {
                ++g_failures;
                break;
            }
            acquired.value.Release();
        }
        const auto end = Clock::now();
        Report("acquire_release_facade", n, NsPerOp(begin, end, n));
    }

    // ---- 4) 选主：n 次重复加入（原地续约），leader/epoch 不变 ----
    {
        const auto first = coordinator.JoinElection("stress-election", "node-a", seconds(60));
        REQ(first.ok());
        REQ(first.value.leader);
        const std::uint64_t epoch0 = first.value.epoch;
        long ok = 0;
        const auto begin = Clock::now();
        for (long i = 0; i < n; ++i) {
            const auto joined =
                coordinator.JoinElection("stress-election", "node-a", seconds(60));
            if (!joined.ok() || !joined.value.leader || joined.value.epoch != epoch0) {
                ++g_failures;
                break;
            }
            ++ok;
        }
        const auto end = Clock::now();
        Report("election_rejoin", n, NsPerOp(begin, end, n));
        REQ(ok == n);
    }

    // ---- 5) facade 选主 Join 10 万次（auto_renew=false，含 Leave 析构）----
    {
        LeaderElection election(coordinator);
        CandidateOptions no_renew;
        no_renew.auto_renew = false;
        const long joins = n / 10;
        long ok = 0;
        const auto begin = Clock::now();
        for (long i = 0; i < joins; ++i) {
            auto joined = election.Join("stress-facade-election", "node-a", seconds(60), no_renew);
            if (!joined.ok() || !joined.value.is_leader()) {
                ++g_failures;
                break;
            }
            ++ok;
        }
        const auto end = Clock::now();
        Report("election_join_facade", joins, NsPerOp(begin, end, joins));
        REQ(ok == joins);
    }

    // ---- 6) 4 线程争抢锁 n 次，token 全局唯一且单调 ----
    {
        DistributedLock locks(coordinator);
        LockOptions manual;
        manual.auto_renew = false;
        constexpr int kThreads = 4;
        const long per_thread = n / kThreads;
        std::vector<std::thread> threads;
        std::vector<std::vector<std::uint64_t>> tokens(static_cast<std::size_t>(kThreads));
        std::atomic<long> ok{0};
        std::atomic<long> conflicts{0};
        std::atomic<bool> start{false};
        const auto begin = Clock::now();
        for (int t = 0; t < kThreads; ++t) {
            tokens[static_cast<std::size_t>(t)].reserve(static_cast<std::size_t>(per_thread));
            threads.emplace_back([&, t] {
                while (!start.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                long done = 0;
                while (done < per_thread) {
                    auto acquired = locks.Acquire("stress-contended",
                                                  "w" + std::to_string(t), seconds(60), manual);
                    if (!acquired.ok()) {
                        conflicts.fetch_add(1, std::memory_order_relaxed);
                        std::this_thread::yield();
                        continue;
                    }
                    tokens[static_cast<std::size_t>(t)].push_back(
                        acquired.value.fencing_token());
                    acquired.value.Release();
                    ++done;
                    ok.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
        start.store(true, std::memory_order_release);
        for (auto &thread : threads) {
            thread.join();
        }
        const auto end = Clock::now();
        std::vector<std::uint64_t> all;
        all.reserve(static_cast<std::size_t>(ok.load()));
        for (auto &v : tokens) {
            all.insert(all.end(), v.begin(), v.end());
        }
        std::sort(all.begin(), all.end());
        const bool unique = std::adjacent_find(all.begin(), all.end()) == all.end();
        std::printf("%-26s %9ld ops  %10.2f ns/op  conflicts=%ld unique=%s\n",
                    "contended_lock_4x", ok.load(), NsPerOp(begin, end, ok.load()),
                    conflicts.load(), unique ? "yes" : "no");
        REQ(ok.load() == kThreads * per_thread);
        REQ(unique);
        REQ(static_cast<long>(all.size()) == ok.load());
        std::printf("  token_range: min=%llu max=%llu size=%zu ok=%ld\n",
                    static_cast<unsigned long long>(all.front()),
                    static_cast<unsigned long long>(all.back()), all.size(), ok.load());
        REQ(all.front() == 1);
        REQ(all.back() == static_cast<std::uint64_t>(ok.load()));
    }

    // ---- 收尾：统计一致性 ----
    const CoordinatorStats stats = coordinator.Stats();
    std::printf("stats: active_leases=%zu services=%zu instances=%zu locks=%zu "
                "elections=%zu candidates=%zu sweeps=%llu expired=%llu\n",
                stats.active_leases, stats.services, stats.instances, stats.locks,
                stats.elections, stats.candidates,
                static_cast<unsigned long long>(stats.sweeps),
                static_cast<unsigned long long>(stats.expired_total));
    REQ(stats.locks == 0);
    REQ(stats.candidates == 1);

    if (g_failures == 0) {
        std::printf("STRESS_OK n=%ld\n", n);
        return 0;
    }
    std::printf("STRESS_FAILED failures=%d n=%ld\n", g_failures, n);
    return 1;
}
