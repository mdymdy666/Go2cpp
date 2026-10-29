/**
 * @file bench_heartbeat_backoff.cpp
 * @brief MOD-11/12 心跳与退避：吞吐压测 + 随机不变量 + 多场景 + 长稳。
 *
 * @details 独立可执行程序，**不注册 CTest**：长稳场景按真实秒计时，
 *          放进 CTest 会拖慢整套回归。测试代码只依赖两个被测头文件，
 *          故意不引 test_util.hpp（它间接包含 link.hpp），以此证明
 *          heartbeat/backoff 与其它 bento 模块解耦。
 *
 * 构建（需先构建 .build-bento-wsl）：
 *   g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror \
 *       -Iinclude tests/bento/bench_heartbeat_backoff.cpp \
 *       .build-bento-wsl/libgo2cpp_bento.a -pthread \
 *       -o .build-bento-wsl/bench_heartbeat_backoff
 *
 * 运行：
 *   .build-bento-wsl/bench_heartbeat_backoff          # 6s 长稳
 *   .build-bento-wsl/bench_heartbeat_backoff --long   # 60s 长稳
 *
 * @author go2cpp contributors
 * @date 2026-10-29
 * @copyright MIT License
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include "go2cpp/bento/net/backoff.hpp"
#include "go2cpp/bento/net/heartbeat.hpp"

namespace {

using namespace go2cpp::bento;

using SteadyClock = std::chrono::steady_clock;

int g_checks = 0;
int g_failures = 0;

#define BENCH_CHECK(condition)                                                       \
    do {                                                                             \
        ++g_checks;                                                                  \
        if (!(condition)) {                                                          \
            std::fprintf(stderr, "[FAIL] %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            ++g_failures;                                                            \
        }                                                                            \
    } while (0)

#define BENCH_CHECK_EQ(actual, expected)                                                 \
    do {                                                                                 \
        ++g_checks;                                                                      \
        const unsigned long long bench_actual =                                          \
            static_cast<unsigned long long>(actual);                                     \
        const unsigned long long bench_expected =                                        \
            static_cast<unsigned long long>(expected);                                   \
        if (bench_actual != bench_expected) {                                            \
            std::fprintf(stderr, "[FAIL] %s:%d: %s == %s (%llu != %llu)\n", __FILE__,    \
                         __LINE__, #actual, #expected, bench_actual, bench_expected);    \
            ++g_failures;                                                                \
        }                                                                                \
    } while (0)

/// 当前常驻内存（KiB）；读不到 /proc 时返回 0。
std::uint64_t CurrentRssKb() {
    std::FILE* file = std::fopen("/proc/self/statm", "r");
    if (file == nullptr) {
        return 0;
    }
    unsigned long total_pages = 0;
    unsigned long resident_pages = 0;
    const int fields = std::fscanf(file, "%lu %lu", &total_pages, &resident_pages);
    std::fclose(file);
    if (fields != 2) {
        return 0;
    }
    return static_cast<std::uint64_t>(resident_pages) * 4;
}

/// 可复现的伪随机源：xorshift64*，足够随机且零分配。
class Rng {
 public:
    explicit Rng(std::uint64_t seed) : m_state(seed == 0 ? 0x9e3779b97f4a7c15ull : seed) {}

    std::uint64_t Next() {
        std::uint64_t x = m_state;
        x ^= x >> 12;
        x ^= x << 25;
        x ^= x >> 27;
        m_state = x;
        return x * 0x2545f4914f6cdd1dull;
    }

    /// [low, high] 闭区间；high <= low 时原样返回 low。
    std::uint32_t Range(std::uint32_t low, std::uint32_t high) {
        if (high <= low) {
            return low;
        }
        const std::uint64_t span = static_cast<std::uint64_t>(high) - low + 1;
        return low + static_cast<std::uint32_t>(Next() % span);
    }

    /// [0, 1)
    double Unit() {
        return static_cast<double>(Next() >> 11) * (1.0 / 9007199254740992.0);
    }

 private:
    std::uint64_t m_state;
};

struct BenchStat {
    std::uint64_t total_ops = 0;
    double ns_per_op = 0.0;
    double million_ops_per_sec = 0.0;
    double median_round_ms = 0.0;
};

/// 每轮 ops 固定、跑 rounds 轮，取中位数，避免单次计时受调度噪声影响。
BenchStat Measure(std::uint64_t ops_per_round, int rounds,
                  const std::function<void()>& body) {
    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(rounds));
    for (int round = 0; round < rounds; ++round) {
        const SteadyClock::time_point start = SteadyClock::now();
        body();
        const SteadyClock::time_point end = SteadyClock::now();
        samples.push_back(
            std::chrono::duration<double, std::milli>(end - start).count());
    }
    BenchStat stat;
    stat.total_ops = ops_per_round * static_cast<std::uint64_t>(rounds);
    std::sort(samples.begin(), samples.end());
    const double median_ms = samples[samples.size() / 2];
    stat.median_round_ms = median_ms;
    stat.ns_per_op = (median_ms * 1'000'000.0) / static_cast<double>(ops_per_round);
    stat.million_ops_per_sec =
        static_cast<double>(ops_per_round) / (median_ms * 1000.0);
    return stat;
}

void PrintBench(const char* name, const BenchStat& stat) {
    std::printf("[BENCH] %-34s %9llu ops  %7.2f ns/op  %8.2f Mops/s  (中位 %7.1f ms/轮)\n",
                name, static_cast<unsigned long long>(stat.total_ops), stat.ns_per_op,
                stat.million_ops_per_sec, stat.median_round_ms);
}

// ---------------------------------------------------------------------------
// 吞吐压测
// ---------------------------------------------------------------------------

constexpr std::uint64_t kOpsPerRound = 1'000'000;  // 每项 >= 100 万次
constexpr int kRounds = 5;
/// 足够大的超时，保证长循环内不会误触发（uint32 上限，约 49.7 天）。
constexpr std::uint32_t kHugeTimeoutMs = 0xFFFFFFFFu;

void BenchHeartbeatPollHit() {
    std::uint64_t hits = 0;
    const BenchStat stat = Measure(kOpsPerRound, kRounds, [&hits] {
        std::uint64_t now = 0;
        Heartbeat heartbeat(1000, kHugeTimeoutMs, [&now] { return now; });
        heartbeat.Start(1000);
        std::uint64_t local = 0;
        for (std::uint64_t i = 0; i < kOpsPerRound; ++i) {
            now += 1000;
            if (heartbeat.Poll()) {
                ++local;
            }
        }
        hits += local;
    });
    BENCH_CHECK_EQ(hits, kOpsPerRound * static_cast<std::uint64_t>(kRounds));
    PrintBench("heartbeat.poll_hit(全命中)", stat);
}

void BenchHeartbeatPollMiss() {
    std::uint64_t hits = 0;
    const BenchStat stat = Measure(kOpsPerRound, kRounds, [&hits] {
        std::uint64_t now = 0;
        Heartbeat heartbeat(1000, kHugeTimeoutMs, [&now] { return now; });
        heartbeat.Start(1000);
        std::uint64_t local = 0;
        for (std::uint64_t i = 0; i < kOpsPerRound; ++i) {
            if (heartbeat.Poll()) {
                ++local;
            }
        }
        hits += local;
    });
    BENCH_CHECK_EQ(hits, 0U);
    PrintBench("heartbeat.poll_miss(未到期)", stat);
}

void BenchHeartbeatPollTimedOut() {
    std::uint64_t hits = 0;
    const BenchStat stat = Measure(kOpsPerRound, kRounds, [&hits] {
        std::uint64_t now = 0;
        Heartbeat heartbeat(1000, 3000, [&now] { return now; });
        heartbeat.Start(1000);
        now = 5000;
        (void)heartbeat.Poll();  // 触发一次超时，进入短路分支
        if (!heartbeat.IsTimedOut()) {
            ++hits;
        }
        std::uint64_t local = 0;
        for (std::uint64_t i = 0; i < kOpsPerRound; ++i) {
            if (heartbeat.Poll()) {
                ++local;
            }
        }
        hits += local;
    });
    BENCH_CHECK_EQ(hits, 0U);
    PrintBench("heartbeat.poll_timedout(短路)", stat);
}

void BenchHeartbeatOnReceive() {
    std::uint64_t last = 0;
    const BenchStat stat = Measure(kOpsPerRound, kRounds, [&last] {
        std::uint64_t now = 0;
        Heartbeat heartbeat(1000, kHugeTimeoutMs, [&now] { return now; });
        heartbeat.Start(1000);
        for (std::uint64_t i = 0; i < kOpsPerRound; ++i) {
            ++now;
            heartbeat.OnReceiveAt(now);
        }
        last = heartbeat.LastActivityMs();
    });
    BENCH_CHECK_EQ(last, kOpsPerRound);
    PrintBench("heartbeat.on_receive_at", stat);
}

void BenchHeartbeatIsAlive() {
    std::uint64_t alive = 0;
    const BenchStat stat = Measure(kOpsPerRound, kRounds, [&alive] {
        std::uint64_t now = 1000;
        Heartbeat heartbeat(1000, kHugeTimeoutMs, [&now] { return now; });
        std::uint64_t local = 0;
        for (std::uint64_t i = 0; i < kOpsPerRound; ++i) {
            if (heartbeat.IsAlive(5000)) {
                ++local;
            }
        }
        alive += local;
    });
    BENCH_CHECK_EQ(alive, kOpsPerRound * static_cast<std::uint64_t>(kRounds));
    PrintBench("heartbeat.is_alive", stat);
}

void BenchBackoffNextDefault() {
    std::uint64_t sum = 0;
    const BenchStat stat = Measure(kOpsPerRound, kRounds, [&sum] {
        Backoff backoff;
        std::uint64_t local = 0;
        for (std::uint64_t i = 0; i < kOpsPerRound; ++i) {
            local += backoff.Next();
        }
        sum += local;
    });
    BENCH_CHECK(sum > 0);
    PrintBench("backoff.next(默认 1s/30s/0.2)", stat);
}

void BenchBackoffNextNoJitter() {
    std::uint64_t sum = 0;
    const BenchStat stat = Measure(kOpsPerRound, kRounds, [&sum] {
        Backoff::Config config;
        config.jitter_ratio = 0.0;
        Backoff backoff(config);
        std::uint64_t local = 0;
        for (std::uint64_t i = 0; i < kOpsPerRound; ++i) {
            local += backoff.Next();
        }
        sum += local;
    });
    BENCH_CHECK(sum > 0);
    PrintBench("backoff.next(无抖动快路径)", stat);
}

void BenchBackoffNextFullJitter() {
    std::uint64_t sum = 0;
    const BenchStat stat = Measure(kOpsPerRound, kRounds, [&sum] {
        Backoff::Config config;
        config.jitter_ratio = 1.0;
        config.max_ms = 30000;
        Backoff backoff(config);
        std::uint64_t local = 0;
        for (std::uint64_t i = 0; i < kOpsPerRound; ++i) {
            local += backoff.Next();
        }
        sum += local;
    });
    BENCH_CHECK(sum > 0);
    PrintBench("backoff.next(抖动 1.0)", stat);
}

void BenchBackoffNextCapped() {
    std::uint64_t sum = 0;
    const BenchStat stat = Measure(kOpsPerRound, kRounds, [&sum] {
        Backoff backoff;
        for (int i = 0; i < 8; ++i) {
            (void)backoff.Next();  // 先推过 30s 上限
        }
        std::uint64_t local = 0;
        for (std::uint64_t i = 0; i < kOpsPerRound; ++i) {
            local += backoff.Next();
        }
        sum += local;
    });
    BENCH_CHECK(sum > 0);
    PrintBench("backoff.next(已封顶 30s)", stat);
}

void BenchBackoffU32Max() {
    std::uint64_t highest = 0;
    const BenchStat stat = Measure(kOpsPerRound, kRounds, [&highest] {
        Backoff::Config config;
        config.base_ms = 1;
        config.max_ms = 0xFFFFFFFFu;
        config.jitter_ratio = 0.0;
        Backoff backoff(config);
        std::uint64_t local_highest = 0;
        for (std::uint64_t i = 0; i < kOpsPerRound; ++i) {
            const std::uint32_t delay = backoff.Next();
            if (delay > local_highest) {
                local_highest = delay;
            }
        }
        if (local_highest > highest) {
            highest = local_highest;
        }
    });
    // 32 次之后 nominal 必须能到 uint32 上限；旧实现左移上限 31 位只能到 2^31。
    BENCH_CHECK_EQ(highest, 0xFFFFFFFFu);
    PrintBench("backoff.next(uint32 上限)", stat);
}

void BenchBackoffResetNext() {
    std::uint64_t sum = 0;
    const BenchStat stat = Measure(kOpsPerRound, kRounds, [&sum] {
        Backoff backoff;
        const std::uint32_t first = backoff.Next();
        backoff.Reset();
        std::uint64_t local = 0;
        for (std::uint64_t i = 0; i < kOpsPerRound; ++i) {
            backoff.Reset();
            local += backoff.Next();
        }
        BENCH_CHECK_EQ(local, static_cast<std::uint64_t>(first) * kOpsPerRound);
        sum += local;
    });
    BENCH_CHECK(sum > 0);
    PrintBench("backoff.reset+next", stat);
}

void BenchBackoffPeek() {
    std::uint64_t sum = 0;
    const BenchStat stat = Measure(kOpsPerRound, kRounds, [&sum] {
        Backoff backoff;
        std::uint64_t local = 0;
        for (std::uint64_t i = 0; i < kOpsPerRound; ++i) {
            (void)backoff.Next();
            local += backoff.PeekNominalMs();
        }
        sum += local;
    });
    BENCH_CHECK(sum > 0);
    PrintBench("backoff.peek_nominal", stat);
}

// ---------------------------------------------------------------------------
// 随机不变量（1000+ 组随机参数）
// ---------------------------------------------------------------------------

void RandomHeartbeat() {
    Rng rng(0xC0FFEEull);
    std::uint64_t total_polls = 0;
    std::uint64_t total_beats = 0;
    std::uint64_t total_timeouts = 0;

    for (int config = 0; config < 1200; ++config) {
        const std::uint32_t interval = rng.Range(1, 60000);
        const std::uint32_t timeout = rng.Range(1, 300000);
        std::uint64_t now = rng.Range(0, 1'000'000);
        std::uint64_t fired = 0;
        Heartbeat heartbeat(interval, timeout, [&now] { return now; },
                            [&fired] { ++fired; });
        heartbeat.Start(interval);

        std::uint64_t previous_beats = 0;
        std::uint64_t previous_timeouts = 0;
        for (int step = 0; step < 400; ++step) {
            const std::uint32_t action = rng.Range(0, 9);
            if (action == 0) {
                heartbeat.OnReceiveAt(now);
            } else if (action == 1) {
                now += rng.Range(1, 5000);
            } else if (action == 2) {
                heartbeat.SetInterval(rng.Range(1, 60000));
            } else if (action == 3) {
                heartbeat.SetTimeout(rng.Range(1, 300000));
            } else {
                const std::uint64_t before_ms = now;
                const std::uint64_t next_before = heartbeat.NextBeatMs();
                const bool was_timed_out = heartbeat.IsTimedOut();
                const std::uint64_t elapsed_before =
                    before_ms >= heartbeat.LastActivityMs()
                        ? before_ms - heartbeat.LastActivityMs()
                        : 0;
                const std::uint32_t timeout_now = heartbeat.TimeoutMs();
                const bool beat = heartbeat.Poll();
                ++total_polls;

                // 不变量 1：返回 true 必须恰好对应一次心跳，且时刻不早于旧到期点；
                //            下次到期点严格等于「本次时刻 + 当前间隔」。
                if (beat) {
                    BENCH_CHECK(!was_timed_out);
                    BENCH_CHECK(before_ms >= next_before);
                    BENCH_CHECK_EQ(heartbeat.Beats(), previous_beats + 1);
                    BENCH_CHECK_EQ(heartbeat.NextBeatMs(),
                                   before_ms + heartbeat.IntervalMs());
                }
                // 不变量 2：跨过超时阈值的那一次 Poll 必须恰好触发一次回调。
                if (!was_timed_out && elapsed_before >= timeout_now) {
                    BENCH_CHECK(heartbeat.IsTimedOut());
                    BENCH_CHECK_EQ(heartbeat.TimeoutsFired(), previous_timeouts + 1);
                    BENCH_CHECK(!beat);
                }
                // 不变量 3：已判超时与仍然存活不可能同时为真。
                BENCH_CHECK(!heartbeat.IsTimedOut() || !heartbeat.IsAlive());
            }
            BENCH_CHECK(heartbeat.Beats() >= previous_beats);
            BENCH_CHECK(heartbeat.TimeoutsFired() >= previous_timeouts);
            BENCH_CHECK_EQ(heartbeat.TimeoutsFired(), fired);
            previous_beats = heartbeat.Beats();
            previous_timeouts = heartbeat.TimeoutsFired();
        }
        total_beats += previous_beats;
        total_timeouts += previous_timeouts;
    }
    std::printf("[RANDOM] heartbeat: 1200 组配置, %llu 次 Poll, %llu 次心跳, %llu 次超时\n",
                static_cast<unsigned long long>(total_polls),
                static_cast<unsigned long long>(total_beats),
                static_cast<unsigned long long>(total_timeouts));
    BENCH_CHECK(total_polls <= 1200ull * 400ull);
    BENCH_CHECK(total_polls >= 1200ull * 400ull / 3);
}

void RandomBackoff() {
    Rng rng(0xBADF00Dull);
    std::uint64_t samples = 0;

    for (int config_index = 0; config_index < 1200; ++config_index) {
        Backoff::Config config;
        config.base_ms = rng.Range(1, 1'000'000);
        config.max_ms = rng.Range(config.base_ms, 0xFFFFFFFFu);
        config.jitter_ratio = rng.Unit();
        config.seed = rng.Next();
        Backoff backoff(config);

        for (int step = 0; step < 64; ++step) {
            const std::uint32_t delay = backoff.Next();
            ++samples;
            // 不变量 1：永远落在 [1, max]，绝不返回 0（否则调用方忙循环）。
            BENCH_CHECK(delay >= 1);
            BENCH_CHECK(delay <= config.max_ms);
            // 不变量 2：无抖动时严格等于名义值。
            if (config.jitter_ratio <= 0.0) {
                BENCH_CHECK_EQ(delay, backoff.PeekNominalMs());
            }
            BENCH_CHECK(backoff.Attempts() == static_cast<std::uint32_t>(step + 1));
        }

        // 不变量 3：Reset 后序列完全复现。
        backoff.Reset();
        BENCH_CHECK_EQ(backoff.Attempts(), 0U);
        const std::uint32_t first = backoff.Next();
        backoff.Reset();
        BENCH_CHECK_EQ(backoff.Next(), first);

        // 不变量 4：名义延迟单调不减且被 max 截断。
        backoff.Reset();
        std::uint32_t previous = 0;
        for (int step = 0; step < 40; ++step) {
            const std::uint32_t nominal = backoff.PeekNominalMs();
            BENCH_CHECK(nominal >= previous);
            BENCH_CHECK(nominal <= config.max_ms);
            (void)backoff.Next();
            previous = nominal;
        }
    }
    std::printf("[RANDOM] backoff: 1200 组配置, %llu 个延迟采样\n",
                static_cast<unsigned long long>(samples));
    BENCH_CHECK(samples == 1200ull * 64ull);
}

// ---------------------------------------------------------------------------
// 场景
// ---------------------------------------------------------------------------

/// 场景 1：假时钟边界前后 1ms（心跳间隔与超时阈值各验证两侧）。
void ScenarioBoundaries() {
    // 心跳节拍边界：interval=1000，检查 999/1000/1999/2000。
    {
        std::uint64_t now = 0;
        Heartbeat cadence(1000, 60000, [&now] { return now; });
        cadence.Start(1000);
        BENCH_CHECK_EQ(cadence.NextBeatMs(), 1000U);
        now = 999;
        BENCH_CHECK(!cadence.Poll());
        now = 1000;
        BENCH_CHECK(cadence.Poll());
        BENCH_CHECK_EQ(cadence.Beats(), 1U);
        BENCH_CHECK_EQ(cadence.NextBeatMs(), 2000U);
        now = 1999;
        BENCH_CHECK(!cadence.Poll());
        now = 2000;
        BENCH_CHECK(cadence.Poll());
        BENCH_CHECK_EQ(cadence.Beats(), 2U);
        now = 3001;  // 大步跨过到期点仍只补一次
        BENCH_CHECK(cadence.Poll());
        BENCH_CHECK_EQ(cadence.Beats(), 3U);
        BENCH_CHECK_EQ(cadence.NextBeatMs(), 4001U);
    }

    // 超时边界：interval=60000（不会心跳），timeout=5000，检查 4999/5000。
    {
        std::uint64_t now = 0;
        std::uint64_t fired = 0;
        Heartbeat watchdog(60000, 5000, [&now] { return now; },
                           [&fired] { ++fired; });
        watchdog.Start();
        now = 4999;
        BENCH_CHECK(!watchdog.Poll());
        BENCH_CHECK(!watchdog.IsTimedOut());
        BENCH_CHECK(watchdog.IsAlive());
        BENCH_CHECK_EQ(fired, 0U);

        now = 5000;  // 恰好等于阈值即判死（>= 语义）
        BENCH_CHECK(!watchdog.Poll());
        BENCH_CHECK(watchdog.IsTimedOut());
        BENCH_CHECK_EQ(fired, 1U);
        BENCH_CHECK(!watchdog.IsAlive());

        watchdog.OnReceiveAt(now);  // 恢复后重新获得完整窗口
        BENCH_CHECK(!watchdog.IsTimedOut());
        now = 9999;
        BENCH_CHECK(watchdog.IsAlive());
        now = 10000;
        BENCH_CHECK(!watchdog.IsAlive());
        BENCH_CHECK(!watchdog.Poll());
        BENCH_CHECK_EQ(fired, 2U);
    }
}

/// 场景 2：连续 1000 轮「超时 -> 恢复」，验证只触发一次 / 恢复可再触发。
void ScenarioTimeoutRecovery() {
    std::uint64_t fired = 0;
    std::uint64_t now = 0;
    Heartbeat heartbeat(1000, 3000, [&now] { return now; },
                        [&fired] { ++fired; });
    heartbeat.Start(1000);
    constexpr int kRounds = 1000;
    for (int round = 0; round < kRounds; ++round) {
        now += 3000;
        BENCH_CHECK(!heartbeat.Poll());
        BENCH_CHECK(heartbeat.IsTimedOut());
        BENCH_CHECK_EQ(heartbeat.TimeoutsFired(), static_cast<std::uint64_t>(round + 1));
        // 未恢复前再 Poll 多次都不应重复触发。
        for (int extra = 0; extra < 3; ++extra) {
            now += 10;
            BENCH_CHECK(!heartbeat.Poll());
        }
        BENCH_CHECK_EQ(heartbeat.TimeoutsFired(), static_cast<std::uint64_t>(round + 1));
        heartbeat.OnReceiveAt(now);
        BENCH_CHECK(!heartbeat.IsTimedOut());
    }
    BENCH_CHECK_EQ(fired, static_cast<std::uint64_t>(kRounds));
    BENCH_CHECK_EQ(heartbeat.TimeoutsFired(), static_cast<std::uint64_t>(kRounds));
}

/// 场景 3：30s 上限 + ±20% 抖动，统计 10 万采样落在 [24s, 30s] 且分布均匀。
void ScenarioBackoffCapJitter() {
    constexpr int kSeeds = 1000;
    constexpr int kSamplesPerSeed = 100;
    Rng rng(0x5EEDull);
    std::uint64_t total = 0;
    std::uint64_t low = 0;
    std::uint64_t high = 0;
    std::uint64_t histogram[10] = {};
    for (int seed_index = 0; seed_index < kSeeds; ++seed_index) {
        Backoff::Config config;
        config.base_ms = 1000;
        config.max_ms = 30000;
        config.jitter_ratio = 0.2;
        config.seed = rng.Next();
        Backoff backoff(config);
        for (int warm = 0; warm < 8; ++warm) {
            (void)backoff.Next();  // 推到上限
        }
        for (int i = 0; i < kSamplesPerSeed; ++i) {
            const std::uint32_t delay = backoff.Next();
            ++total;
            BENCH_CHECK(delay >= 24000);
            BENCH_CHECK(delay <= 30000);
            if (delay < 24000) {
                ++low;
            }
            if (delay > 30000) {
                ++high;
            }
            std::size_t bucket = static_cast<std::size_t>(delay - 24000) / 600;
            if (bucket > 9) {
                bucket = 9;
            }
            ++histogram[bucket];
        }
    }
    BENCH_CHECK_EQ(low, 0U);
    BENCH_CHECK_EQ(high, 0U);
    BENCH_CHECK_EQ(total, 100000U);
    std::printf("[SCENARIO] 30s 上限 + ±20%% 抖动: %llu 采样, 直方图(24s->30s, 每桶 600ms):",
                static_cast<unsigned long long>(total));
    for (std::size_t i = 0; i < 10; ++i) {
        std::printf(" %llu", static_cast<unsigned long long>(histogram[i]));
    }
    std::printf("\n");
    // 均匀抖动下每个桶期望 10%±2%；离散度检查防止「伪随机退化成常数」。
    for (std::size_t i = 0; i < 10; ++i) {
        const double ratio = static_cast<double>(histogram[i]) / 100000.0;
        BENCH_CHECK(ratio > 0.06 && ratio < 0.14);
    }
}

/// 场景 4：多线程。a) 每线程独立实例；b) 共享实例 + 外部锁；c) 退避并发。
void ScenarioMultiThread() {
    constexpr int kThreads = 4;
    constexpr std::uint64_t kOpsPerThread = 250'000;  // 4 线程合计 100 万

    // a) 独立实例：无共享状态，验证 100 万次并发 Poll 全部命中。
    {
        std::vector<std::uint64_t> hits(kThreads, 0);
        std::vector<std::thread> threads;
        threads.reserve(kThreads);
        for (int index = 0; index < kThreads; ++index) {
            threads.emplace_back([&hits, index] {
                std::uint64_t now = 0;
                Heartbeat heartbeat(1000, kHugeTimeoutMs, [&now] { return now; });
                heartbeat.Start(1000);
                std::uint64_t local = 0;
                for (std::uint64_t i = 0; i < kOpsPerThread; ++i) {
                    now += 1000;
                    if (heartbeat.Poll()) {
                        ++local;
                    }
                }
                hits[static_cast<std::size_t>(index)] = local;
            });
        }
        for (std::thread& thread : threads) {
            thread.join();
        }
        for (int index = 0; index < kThreads; ++index) {
            BENCH_CHECK_EQ(hits[static_cast<std::size_t>(index)], kOpsPerThread);
        }
    }

    // b) 共享实例 + std::mutex：验证文档承诺的「跨线程需外部加锁」确实可用。
    {
        std::uint64_t now = 0;  // 锁保护
        std::uint64_t fired = 0;
        Heartbeat shared(1000, kHugeTimeoutMs, [&now] { return now; },
                         [&fired] { ++fired; });
        shared.Start(1000);
        std::mutex mutex;
        std::uint64_t polls = 0;
        std::vector<std::thread> threads;
        threads.reserve(kThreads);
        for (int index = 0; index < kThreads; ++index) {
            threads.emplace_back([&] {
                for (std::uint64_t i = 0; i < 50'000; ++i) {
                    std::lock_guard<std::mutex> lock(mutex);
                    shared.OnReceiveAt(++now);  // 锁内保持严格单调
                    (void)shared.Poll();
                    ++polls;
                }
            });
        }
        for (std::thread& thread : threads) {
            thread.join();
        }
        BENCH_CHECK_EQ(polls, 4U * 50'000U);
        BENCH_CHECK_EQ(fired, 0U);
        BENCH_CHECK_EQ(shared.LastActivityMs(), now);
        BENCH_CHECK(shared.Beats() <= polls);
    }

    // c) 独立 Backoff 并发 100 万次：验证内核态无共享、结果始终在范围内。
    {
        std::vector<std::uint64_t> sums(kThreads, 0);
        std::vector<std::thread> threads;
        threads.reserve(kThreads);
        for (int index = 0; index < kThreads; ++index) {
            threads.emplace_back([&sums, index] {
                Backoff backoff;
                std::uint64_t sum = 0;
                for (std::uint64_t i = 0; i < kOpsPerThread; ++i) {
                    const std::uint32_t delay = backoff.Next();
                    if (delay == 0 || delay > 30000) {
                        sums[static_cast<std::size_t>(index)] = 0;  // 触发下方断言
                        return;
                    }
                    sum += delay;
                }
                sums[static_cast<std::size_t>(index)] = sum;
            });
        }
        for (std::thread& thread : threads) {
            thread.join();
        }
        for (int index = 0; index < kThreads; ++index) {
            BENCH_CHECK(sums[static_cast<std::size_t>(index)] > 0);
        }
    }
}

/// 场景 5：回调内重入（Poll/OnReceive/Start），验证不会二次触发或死循环。
void ScenarioReentrancy() {
    std::uint64_t now = 0;
    int reentry_polls = 0;
    bool saw_timeout = false;
    Heartbeat heartbeat(1000, 3000, [&now] { return now; });
    heartbeat.SetOnTimeout([&] {
        saw_timeout = true;
        ++reentry_polls;
        (void)heartbeat.Poll();          // 回调里重入 Poll：必须返回 false
        heartbeat.OnReceiveAt(now);      // 回调里恢复：清除超时态
    });
    heartbeat.Start(1000);
    now = 3000;
    BENCH_CHECK(!heartbeat.Poll());
    BENCH_CHECK(saw_timeout);
    BENCH_CHECK_EQ(reentry_polls, 1);
    BENCH_CHECK(!heartbeat.IsTimedOut());  // 回调内 OnReceiveAt 已恢复
    BENCH_CHECK_EQ(heartbeat.TimeoutsFired(), 1U);

    // 回调内 Start 重启：计数清零、后续节拍按新间隔。
    std::uint64_t restart_now = 0;
    int starts = 0;
    Heartbeat restarted(1000, 3000, [&restart_now] { return restart_now; });
    restarted.SetOnTimeout([&] {
        ++starts;
        restarted.Start(2000);
    });
    restarted.Start(1000);
    restart_now = 4000;
    (void)restarted.Poll();
    BENCH_CHECK_EQ(starts, 1);
    BENCH_CHECK_EQ(restarted.IntervalMs(), 2000U);
    BENCH_CHECK(restarted.IsRunning());
    BENCH_CHECK_EQ(restarted.NextBeatMs(), 6000U);
}

/// 场景 6：真实时钟长稳（默认 6s / --long 60s），验证无漂移累积与内存稳定。
void ScenarioLongRun(int seconds) {
    constexpr std::uint32_t kIntervalMs = 25;
    const std::uint64_t rss_before = CurrentRssKb();
    Heartbeat heartbeat(kIntervalMs, 90000);
    std::uint64_t fired = 0;
    heartbeat.SetOnTimeout([&fired] { ++fired; });
    heartbeat.Start();

    const SteadyClock::time_point deadline =
        SteadyClock::now() + std::chrono::seconds(seconds);
    std::uint64_t polls = 0;
    while (SteadyClock::now() < deadline) {
        (void)heartbeat.Poll();
        ++polls;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const std::uint64_t rss_after = CurrentRssKb();
    const std::uint64_t expected = static_cast<std::uint64_t>(seconds) * 1000 / kIntervalMs;

    std::printf(
        "[SCENARIO] %ds 长稳: %llu 次 Poll, %llu 次心跳(期望约 %llu), 超时 %llu, "
        "RSS %llu->%llu KiB\n",
        seconds, static_cast<unsigned long long>(polls),
        static_cast<unsigned long long>(heartbeat.Beats()),
        static_cast<unsigned long long>(expected), static_cast<unsigned long long>(fired),
        static_cast<unsigned long long>(rss_before),
        static_cast<unsigned long long>(rss_after));

    BENCH_CHECK_EQ(fired, 0U);
    BENCH_CHECK(heartbeat.Beats() >= expected * 9 / 10);
    BENCH_CHECK(heartbeat.Beats() <= expected * 11 / 10);
    if (rss_before != 0 && rss_after != 0) {
        BENCH_CHECK(rss_after < rss_before + 2048);  // 增长 < 2 MiB
    }
    BENCH_CHECK(heartbeat.IsAlive());
}

/// 结构体尺寸/复制语义的静态事实，写进报告用。
void PrintLayout() {
    std::printf("[INFO] sizeof(Heartbeat)=%zu, sizeof(Backoff)=%zu, sizeof(Backoff::Config)=%zu\n",
                sizeof(Heartbeat), sizeof(Backoff), sizeof(Backoff::Config));
    static_assert(std::is_copy_constructible_v<Heartbeat>, "Heartbeat 应可拷贝");
    static_assert(std::is_copy_constructible_v<Backoff>, "Backoff 应可拷贝");
    static_assert(std::is_nothrow_destructible_v<Heartbeat>, "析构不应抛");
    static_assert(std::is_nothrow_destructible_v<Backoff>, "析构不应抛");
}

}  // namespace

int main(int argc, char** argv) {
    int long_run_seconds = 6;
    for (int index = 1; index < argc; ++index) {
        if (std::string(argv[index]) == "--long") {
            long_run_seconds = 60;
        }
    }

    std::printf("=== heartbeat / backoff 压测审查 ===\n");
    PrintLayout();
    const std::uint64_t rss_start = CurrentRssKb();

    BenchHeartbeatPollHit();
    BenchHeartbeatPollMiss();
    BenchHeartbeatPollTimedOut();
    BenchHeartbeatOnReceive();
    BenchHeartbeatIsAlive();
    BenchBackoffNextDefault();
    BenchBackoffNextNoJitter();
    BenchBackoffNextFullJitter();
    BenchBackoffNextCapped();
    BenchBackoffU32Max();
    BenchBackoffResetNext();
    BenchBackoffPeek();

    RandomHeartbeat();
    RandomBackoff();

    ScenarioBoundaries();
    ScenarioTimeoutRecovery();
    ScenarioBackoffCapJitter();
    ScenarioMultiThread();
    ScenarioReentrancy();
    ScenarioLongRun(long_run_seconds);

    const std::uint64_t rss_end = CurrentRssKb();
    std::printf("[INFO] 全程 RSS %llu -> %llu KiB\n",
                static_cast<unsigned long long>(rss_start),
                static_cast<unsigned long long>(rss_end));

    if (g_failures != 0) {
        std::fprintf(stderr, "[FAIL] bench_heartbeat_backoff: %d/%d 断言失败\n",
                     g_failures, g_checks);
        return 1;
    }
    std::printf("[PASS] bench_heartbeat_backoff (%d 断言)\n", g_checks);
    return 0;
}
