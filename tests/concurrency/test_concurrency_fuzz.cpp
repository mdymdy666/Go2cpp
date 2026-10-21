/**
 * @file test_concurrency_fuzz.cpp
 * @brief 10 类并发原语的随机操作序列测试（可复现、带死锁 watchdog）。
 *
 * 每轮：
 *   - 随机线程数 2~8（可用 --threads 固定）；
 *   - 每轮共 1 万次随机操作（默认 --ops=10000），操作覆盖
 *     lock/unlock/rdlock/wrlock/wait/notify/send/recv/select/timeout；
 *   - 默认 1000 轮（--rounds）。
 *
 * 断言：
 *   - 无死锁：每个线程 join 都带 30s watchdog，超时以 `[watchdog]` 失败；
 *   - 计数守恒：lock/unlock、rdlock/rdunlock、wrlock/wrunlock、
 *     send/recv（含通道剩余元素）逐一核对；
 *   - 无崩溃：所有原语调用在 try/catch 内，异常计入 bad_ops。
 *
 * 可复现：固定种子 + 打印种子；每轮线程数与每线程操作序列都由种子决定。
 * 用法：test_concurrency_fuzz [--rounds N] [--ops N] [--seed N] [--threads N]
 */

#include "go2cpp/channel.hpp"
#include "go2cpp/concurrency/core/futex_mutex.hpp"
#include "go2cpp/concurrency/core/parking_condition.hpp"
#include "go2cpp/concurrency/locks.hpp"
#include "go2cpp/concurrency/spin_mutex.hpp"
#include "go2cpp/context.hpp"
#include "go2cpp/sync.hpp"
#include "test_support.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

using go2cpp::Background;
using go2cpp::ChannelPtr;
using go2cpp::MakeChannel;
using go2cpp::core::FutexMutex;
using go2cpp::core::ParkingCondition;
using go2cpp::sync::ConditionVariable;
using go2cpp::sync::Mutex;
using go2cpp::sync::RecursiveMutex;
using go2cpp::sync::RWMutex;
using go2cpp::sync::SpinMutex;
using go2cpp::sync::TimedMutex;
using go2cpp::sync::WaitGroup;

struct Options {
    int rounds{1000};
    int ops{10000};
    std::uint64_t seed{0x5EEDF00DCAFEBABEULL};
    int fixed_threads{0};
};

bool ParseOptions(int argc, char** argv, Options* options) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", name);
                return nullptr;
            }
            return argv[++i];
        };
        if (arg == "--rounds") {
            const char* text = value("--rounds");
            if (text == nullptr) return false;
            options->rounds = std::atoi(text);
        } else if (arg == "--ops") {
            const char* text = value("--ops");
            if (text == nullptr) return false;
            options->ops = std::atoi(text);
        } else if (arg == "--seed") {
            const char* text = value("--seed");
            if (text == nullptr) return false;
            options->seed = std::strtoull(text, nullptr, 10);
        } else if (arg == "--threads") {
            const char* text = value("--threads");
            if (text == nullptr) return false;
            options->fixed_threads = std::atoi(text);
        } else if (arg == "--help" || arg == "-h") {
            std::printf(
                "usage: %s [--rounds N] [--ops N] [--seed N] [--threads N]\n",
                argv[0]);
            return false;
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
            return false;
        }
    }
    if (options->rounds <= 0 || options->ops <= 0) {
        std::fprintf(stderr, "rounds/ops must be positive\n");
        return false;
    }
    if (options->fixed_threads < 0 || options->fixed_threads > 8) {
        std::fprintf(stderr, "threads must be 0 (random) or 1..8\n");
        return false;
    }
    return true;
}

/// 一轮内的共享状态（全部栈上，轮间隔离）。
struct RoundState {
    Mutex mutex;
    RWMutex rw;
    RecursiveMutex recursive;
    TimedMutex timed;
    SpinMutex spin;
    FutexMutex futex;
    ConditionVariable cv;
    ParkingCondition pc;
    std::mutex pc_mutex;
    ChannelPtr<int> channel;

    // 守恒计数（全部操作成对）。
    std::atomic<long long> mutex_lock{0};
    std::atomic<long long> mutex_unlock{0};
    std::atomic<long long> rw_read{0};
    std::atomic<long long> rw_read_unlock{0};
    std::atomic<long long> rw_write{0};
    std::atomic<long long> rw_write_unlock{0};
    std::atomic<long long> recursive_lock{0};
    std::atomic<long long> recursive_unlock{0};
    std::atomic<long long> timed_lock{0};
    std::atomic<long long> timed_unlock{0};
    std::atomic<long long> spin_lock{0};
    std::atomic<long long> spin_unlock{0};
    std::atomic<long long> futex_lock{0};
    std::atomic<long long> futex_unlock{0};
    std::atomic<long long> channel_send{0};
    std::atomic<long long> channel_recv{0};
    std::atomic<long long> select_recv{0};
    std::atomic<long long> cv_wait{0};
    std::atomic<long long> cv_notify{0};
    std::atomic<long long> pc_wait{0};
    std::atomic<long long> pc_notify{0};
    std::atomic<long long> timeouts{0};
    std::atomic<long long> bad_ops{0};
    std::atomic<long long> done_ops{0};
};

std::uint64_t MixSeed(std::uint64_t seed, int round, int thread) {
    std::uint64_t value = seed ^ (static_cast<std::uint64_t>(round) *
                                 0x9E3779B97F4A7C15ULL);
    value ^= static_cast<std::uint64_t>(thread + 1) * 0xBF58476D1CE4E5B9ULL;
    value ^= value >> 30;
    value *= 0xBF58476D1CE4E5B9ULL;
    value ^= value >> 27;
    value *= 0x94D049BB133111EBULL;
    value ^= value >> 31;
    return value;
}

bool RunRound(const Options& options, int round, std::uint64_t seed) {
    std::mt19937_64 round_rng(MixSeed(seed, round, 0));
    const int thread_count = options.fixed_threads > 0
                                 ? options.fixed_threads
                                 : static_cast<int>(2 + (round_rng() % 7));
    RoundState state;
    state.channel = MakeChannel<int>(128);
    const auto ctx = Background();
    WaitGroup done;
    done.Add(thread_count);

    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(thread_count));
    for (int thread = 0; thread < thread_count; ++thread) {
        const int base = options.ops / thread_count;
        const int extra = thread == thread_count - 1
                              ? options.ops % thread_count
                              : 0;
        const std::uint64_t thread_seed = MixSeed(seed, round, thread + 1);
        threads.emplace_back([&, base, extra, thread_seed] {
            std::mt19937_64 rng(thread_seed);
            // select 用线程私有的 1 槽通道：避免 probe 与其它线程的
            // send/recv 竞争导致「值被 select 取走但计数无法配对」。
            auto probe_channel = MakeChannel<int>(1);
            const int count = base + extra;
            for (int i = 0; i < count; ++i) {
                try {
                    switch (static_cast<int>(rng() % 14)) {
                        case 0: {
                            if (state.mutex.TryLock()) {
                                state.mutex_lock.fetch_add(
                                    1, std::memory_order_relaxed);
                                state.mutex.Unlock();
                                state.mutex_unlock.fetch_add(
                                    1, std::memory_order_relaxed);
                            }
                            break;
                        }
                        case 1: {
                            if (state.rw.RLockFor(0ms)) {
                                state.rw_read.fetch_add(
                                    1, std::memory_order_relaxed);
                                state.rw.RUnlock();
                                state.rw_read_unlock.fetch_add(
                                    1, std::memory_order_relaxed);
                            }
                            break;
                        }
                        case 2: {
                            if (state.rw.LockFor(0ms)) {
                                state.rw_write.fetch_add(
                                    1, std::memory_order_relaxed);
                                state.rw.Unlock();
                                state.rw_write_unlock.fetch_add(
                                    1, std::memory_order_relaxed);
                            }
                            break;
                        }
                        case 3: {
                            state.recursive.Lock();
                            state.recursive.Lock();
                            state.recursive_lock.fetch_add(
                                2, std::memory_order_relaxed);
                            state.recursive.Unlock();
                            state.recursive.Unlock();
                            state.recursive_unlock.fetch_add(
                                2, std::memory_order_relaxed);
                            break;
                        }
                        case 4: {
                            if (state.timed.TryLock()) {
                                state.timed_lock.fetch_add(
                                    1, std::memory_order_relaxed);
                                state.timed.Unlock();
                                state.timed_unlock.fetch_add(
                                    1, std::memory_order_relaxed);
                            }
                            break;
                        }
                        case 5: {
                            if (state.spin.TryLock()) {
                                state.spin_lock.fetch_add(
                                    1, std::memory_order_relaxed);
                                state.spin.Unlock();
                                state.spin_unlock.fetch_add(
                                    1, std::memory_order_relaxed);
                            }
                            break;
                        }
                        case 6: {
                            if (state.futex.try_lock()) {
                                state.futex_lock.fetch_add(
                                    1, std::memory_order_relaxed);
                                state.futex.unlock();
                                state.futex_unlock.fetch_add(
                                    1, std::memory_order_relaxed);
                            }
                            break;
                        }
                        case 7: {
                            state.mutex.Lock();
                            state.mutex_lock.fetch_add(
                                1, std::memory_order_relaxed);
                            const bool notified = state.cv.WaitFor(state.mutex, 0ms);
                            (void)notified;
                            state.cv_wait.fetch_add(1, std::memory_order_relaxed);
                            state.mutex.Unlock();
                            state.mutex_unlock.fetch_add(
                                1, std::memory_order_relaxed);
                            if ((rng() & 1U) != 0U) {
                                state.cv.NotifyOne();
                                state.cv_notify.fetch_add(
                                    1, std::memory_order_relaxed);
                            }
                            break;
                        }
                        case 8: {
                            std::unique_lock<std::mutex> lock(state.pc_mutex);
                            (void)state.pc.wait_for(lock, 0ms,
                                                    [] { return false; });
                            state.pc_wait.fetch_add(1, std::memory_order_relaxed);
                            if ((rng() & 1U) != 0U) {
                                state.pc.notify_one();
                                state.pc_notify.fetch_add(
                                    1, std::memory_order_relaxed);
                            }
                            break;
                        }
                        case 9: {
                            const auto result = state.channel->TrySend(
                                static_cast<int>(rng() & 0xFFFFU));
                            if (result.Ok()) {
                                state.channel_send.fetch_add(
                                    1, std::memory_order_relaxed);
                            }
                            break;
                        }
                        case 10: {
                            const auto result = state.channel->TryRecv();
                            if (result.Ok()) {
                                state.channel_recv.fetch_add(
                                    1, std::memory_order_relaxed);
                            }
                            break;
                        }
                        case 11: {
                            (void)probe_channel->TrySend(1);
                            const auto result = go2cpp::Select(
                                {go2cpp::RecvCase(probe_channel),
                                 go2cpp::DefaultCase()},
                                ctx, std::nullopt);
                            if (result.selected && result.index == 0) {
                                state.select_recv.fetch_add(
                                    1, std::memory_order_relaxed);
                            } else {
                                (void)probe_channel->TryRecv();  // 清理
                            }
                            break;
                        }
                        case 12: {
                            if ((rng() & 1U) != 0U) {
                                const auto result =
                                    state.channel->RecvFor(0ms, ctx);
                                if (result.Ok()) {
                                    state.channel_recv.fetch_add(
                                        1, std::memory_order_relaxed);
                                } else {
                                    state.timeouts.fetch_add(
                                        1, std::memory_order_relaxed);
                                }
                            } else if (state.mutex.LockFor(0ms)) {
                                state.mutex_lock.fetch_add(
                                    1, std::memory_order_relaxed);
                                state.mutex.Unlock();
                                state.mutex_unlock.fetch_add(
                                    1, std::memory_order_relaxed);
                            } else {
                                state.timeouts.fetch_add(
                                    1, std::memory_order_relaxed);
                            }
                            break;
                        }
                        default: {
                            // 一次「短临界区」：验证忙态下其它操作不破坏计数。
                            state.mutex.Lock();
                            state.mutex_lock.fetch_add(
                                1, std::memory_order_relaxed);
                            state.mutex.Unlock();
                            state.mutex_unlock.fetch_add(
                                1, std::memory_order_relaxed);
                            break;
                        }
                    }
                } catch (...) {
                    state.bad_ops.fetch_add(1, std::memory_order_relaxed);
                }
                state.done_ops.fetch_add(1, std::memory_order_relaxed);
            }
            done.Done();
        });
    }

    for (auto& thread : threads) {
        GO2CPP_JOIN_WITH_WATCHDOG(thread, 30s);
    }
    if (!done.WaitFor(10s)) {
        GO2CPP_CHECK(false);
        return false;
    }

    const long long remaining =
        static_cast<long long>(state.channel->Len());
    if (state.bad_ops.load() != 0 ||
        state.done_ops.load() != options.ops ||
        state.mutex_lock.load() != state.mutex_unlock.load() ||
        state.rw_read.load() != state.rw_read_unlock.load() ||
        state.rw_write.load() != state.rw_write_unlock.load() ||
        state.recursive_lock.load() != state.recursive_unlock.load() ||
        state.timed_lock.load() != state.timed_unlock.load() ||
        state.spin_lock.load() != state.spin_unlock.load() ||
        state.futex_lock.load() != state.futex_unlock.load() ||
        state.channel_send.load() != state.channel_recv.load() + remaining) {
        std::fprintf(
            stderr,
            "[fuzz][debug] round=%d threads=%d bad=%lld done=%lld/%d "
            "mutex=%lld/%lld rw=%lld/%lld/%lld/%lld rec=%lld/%lld "
            "timed=%lld/%lld spin=%lld/%lld futex=%lld/%lld "
            "ch=%lld/%lld remaining=%lld timeouts=%lld\n",
            round, thread_count,
            (long long)state.bad_ops.load(),
            (long long)state.done_ops.load(), options.ops,
            (long long)state.mutex_lock.load(),
            (long long)state.mutex_unlock.load(),
            (long long)state.rw_read.load(),
            (long long)state.rw_read_unlock.load(),
            (long long)state.rw_write.load(),
            (long long)state.rw_write_unlock.load(),
            (long long)state.recursive_lock.load(),
            (long long)state.recursive_unlock.load(),
            (long long)state.timed_lock.load(),
            (long long)state.timed_unlock.load(),
            (long long)state.spin_lock.load(),
            (long long)state.spin_unlock.load(),
            (long long)state.futex_lock.load(),
            (long long)state.futex_unlock.load(),
            (long long)state.channel_send.load(),
            (long long)state.channel_recv.load(), remaining,
            (long long)state.timeouts.load());
    }
    return state.bad_ops.load() == 0 &&
           state.done_ops.load() == options.ops &&
           state.mutex_lock.load() == state.mutex_unlock.load() &&
           state.rw_read.load() == state.rw_read_unlock.load() &&
           state.rw_write.load() == state.rw_write_unlock.load() &&
           state.recursive_lock.load() == state.recursive_unlock.load() &&
           state.timed_lock.load() == state.timed_unlock.load() &&
           state.spin_lock.load() == state.spin_unlock.load() &&
           state.futex_lock.load() == state.futex_unlock.load() &&
           state.channel_send.load() ==
               state.channel_recv.load() + remaining;
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    if (!ParseOptions(argc, argv, &options)) {
        return 2;
    }
    std::printf(
        "[fuzz] seed=%llu rounds=%d ops_per_round=%d threads=%s\n",
        static_cast<unsigned long long>(options.seed), options.rounds,
        options.ops,
        options.fixed_threads > 0 ? std::to_string(options.fixed_threads).c_str()
                                  : "2-8(random)");

    const auto started = Clock::now();
    long long total_ops = 0;
    for (int round = 0; round < options.rounds; ++round) {
        if (round % 100 == 0) {
            const double seconds =
                std::chrono::duration<double>(Clock::now() - started).count();
            std::printf("[fuzz] round %d/%d elapsed=%.3fs\n", round,
                        options.rounds, seconds);
            std::fflush(stdout);
        }
        if (!RunRound(options, round, options.seed)) {
            std::fprintf(stderr,
                         "[fuzz] FAIL round=%d seed=%llu (reproduce with "
                         "--rounds=%d --ops=%d --seed=%llu --threads=%d)\n",
                         round,
                         static_cast<unsigned long long>(options.seed),
                         round + 1, options.ops,
                         static_cast<unsigned long long>(options.seed),
                         options.fixed_threads);
            return 1;
        }
        total_ops += options.ops;
    }
    const double seconds =
        std::chrono::duration<double>(Clock::now() - started).count();
    std::printf(
        "[fuzz] PASS rounds=%d ops=%lld elapsed=%.3fs seed=%llu\n",
        options.rounds, total_ops, seconds,
        static_cast<unsigned long long>(options.seed));
    return 0;
}
