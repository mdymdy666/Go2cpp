/**
 * @file test_resilience.cpp
 * @brief 韧性三组件（Retry / CircuitBreaker / RateLimiter）的独立测试。
 *
 * ## 覆盖（30 个用例，含边界与并发）
 *
 * - Retry（10）：首次成功、超时后成功、重试耗尽、4xx 不重试、退避序列、
 *   抖动区间、max_attempts=0/1、负配置规范化、延迟上限、并发共享。
 * - CircuitBreaker（9）：初始闭合、阈值打开、成功重置连败、冷却转半开、
 *   半开成功闭合、半开失败重开、Open 忽略记录、零阈值规范化、8 线程并发。
 * - RateLimiter（11）：满桶耗尽、按时间补充、补充封顶、部分令牌边界、
 *   时钟回退、零速率/零容量、非法参数、请求超过容量、并发恰好 burst、Reset。
 *
 * ## 构建运行
 *
 * ```sh
 * tests/resilience/build_and_run.sh
 * ```
 *
 * 组件是 header-only，本测试**不链接** go2cpp 运行时，也不改主 CMake。
 */
#include "go2cpp/resilience/resilience.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <thread>
#include <vector>

namespace {

using go2cpp::resilience::CircuitBreaker;
using go2cpp::resilience::CircuitBreakerConfig;
using go2cpp::resilience::CircuitState;
using go2cpp::resilience::Clock;
using go2cpp::resilience::ErrorClass;
using go2cpp::resilience::RateLimiter;
using go2cpp::resilience::Retry;
using go2cpp::resilience::RetryConfig;
using go2cpp::resilience::TimePoint;
using std::chrono::milliseconds;
using std::chrono::seconds;

int g_failures = 0;
int g_cases = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("  [断言失败] %s:%d  %s\n", __FILE__, __LINE__, #cond);  \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

#define CHECK_NEAR(actual, expected, eps)                                        \
    do {                                                                         \
        const double a_ = static_cast<double>(actual);                           \
        const double e_ = static_cast<double>(expected);                         \
        if (std::fabs(a_ - e_) > (eps)) {                                        \
            std::printf("  [断言失败] %s:%d  %s=%.9f 期望 %.9f\n", __FILE__,    \
                        __LINE__, #actual, a_, e_);                              \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

/// 假时钟：零耗时模拟冷却与令牌补充；并发测试中只读，不推进。
class FakeClock {
public:
    FakeClock() : now_(TimePoint{}) {}

    TimePoint Now() const { return now_; }

    Clock AsClock() { return [this] { return now_; }; }

    void Advance(milliseconds delta) { now_ += delta; }

private:
    TimePoint now_;
};

void RunCase(const char* name, void (*fn)()) {
    ++g_cases;
    const int before = g_failures;
    fn();
    if (g_failures == before) {
        std::printf("[PASS] %s\n", name);
    } else {
        std::printf("[FAIL] %s（%d 处断言失败）\n", name, g_failures - before);
    }
}

// ============================ Retry ============================

void TestRetryFirstSuccess() {
    std::vector<milliseconds> delays;
    Retry retry(RetryConfig{3, milliseconds{10}, milliseconds{100}, 0.0},
                [&](milliseconds d) { delays.push_back(d); }, 7);
    int calls = 0;
    const auto result = retry.Execute([&] {
        ++calls;
        return ErrorClass::kOk;
    });
    CHECK(result.ok);
    CHECK(result.attempts == 1u);
    CHECK(calls == 1);
    CHECK(delays.empty());
    CHECK(result.total_delay == milliseconds{0});
    CHECK(result.last_error == ErrorClass::kOk);
}

void TestRetryTimeoutThenSuccess() {
    std::vector<milliseconds> delays;
    Retry retry(RetryConfig{4, milliseconds{10}, milliseconds{100}, 0.0},
                [&](milliseconds d) { delays.push_back(d); }, 7);
    int calls = 0;
    const auto result = retry.Execute([&] {
        ++calls;
        if (calls < 3) {
            return ErrorClass::kTimeout;
        }
        return ErrorClass::kOk;
    });
    CHECK(result.ok);
    CHECK(result.attempts == 3u);
    CHECK(calls == 3);
    CHECK(delays.size() == 2u);
    CHECK(delays[0] == milliseconds{10});
    CHECK(delays[1] == milliseconds{20});
    CHECK(result.total_delay == milliseconds{30});
}

void TestRetryExhausted() {
    std::vector<milliseconds> delays;
    Retry retry(RetryConfig{4, milliseconds{5}, milliseconds{10}, 0.0},
                [&](milliseconds d) { delays.push_back(d); }, 7);
    int calls = 0;
    const auto result = retry.Execute([&] {
        ++calls;
        return ErrorClass::kTimeout;
    });
    CHECK(!result.ok);
    CHECK(result.attempts == 4u);
    CHECK(calls == 4);
    CHECK(delays.size() == 3u);
    CHECK(delays[0] == milliseconds{5});
    CHECK(delays[1] == milliseconds{10});
    CHECK(delays[2] == milliseconds{10});
    CHECK(result.total_delay == milliseconds{25});
    CHECK(result.last_error == ErrorClass::kTimeout);
}

void TestRetryNoRetryOnClientError() {
    std::vector<milliseconds> delays;
    Retry retry(RetryConfig{5, milliseconds{10}, milliseconds{100}, 0.0},
                [&](milliseconds d) { delays.push_back(d); }, 7);
    int calls = 0;
    const auto result = retry.Execute([&] {
        ++calls;
        return ErrorClass::kClientError;
    });
    CHECK(!result.ok);
    CHECK(result.attempts == 1u);
    CHECK(calls == 1);
    CHECK(delays.empty());
    CHECK(result.last_error == ErrorClass::kClientError);
}

void TestRetryBackoffSequenceNoJitter() {
    std::vector<milliseconds> delays;
    Retry retry(RetryConfig{5, milliseconds{10}, milliseconds{40}, 0.0},
                [&](milliseconds d) { delays.push_back(d); }, 7);
    const auto result = retry.Execute([] { return ErrorClass::kUnavailable; });
    CHECK(!result.ok);
    CHECK(delays.size() == 4u);
    CHECK(delays[0] == milliseconds{10});
    CHECK(delays[1] == milliseconds{20});
    CHECK(delays[2] == milliseconds{40});
    CHECK(delays[3] == milliseconds{40});
    CHECK(result.total_delay == milliseconds{110});
}

void TestRetryJitterBounds() {
    std::vector<milliseconds> delays;
    Retry retry(RetryConfig{6, milliseconds{100}, milliseconds{1000}, 0.5},
                [&](milliseconds d) { delays.push_back(d); }, 42);
    const auto result = retry.Execute([] { return ErrorClass::kClosed; });
    CHECK(!result.ok);
    CHECK(delays.size() == 5u);
    // 第 1..4 次：raw = 100/200/400/800，±50%；第 5 次 raw=1600 先截到 1000。
    CHECK(delays[0] >= milliseconds{50} && delays[0] <= milliseconds{150});
    CHECK(delays[1] >= milliseconds{100} && delays[1] <= milliseconds{300});
    CHECK(delays[2] >= milliseconds{200} && delays[2] <= milliseconds{600});
    CHECK(delays[3] >= milliseconds{400} && delays[3] <= milliseconds{1000});
    CHECK(delays[4] >= milliseconds{500} && delays[4] <= milliseconds{1000});
    for (const auto& d : delays) {
        CHECK(d >= milliseconds{0} && d <= milliseconds{1000});
    }
    // 固定种子下 5 次抖动不应全部相同（证明随机确实生效）。
    bool varied = false;
    for (std::size_t i = 1; i < delays.size(); ++i) {
        if (delays[i] != delays[i - 1]) {
            varied = true;
        }
    }
    CHECK(varied);
}

void TestRetryZeroAndOneAttempts() {
    int calls = 0;
    Retry zero(RetryConfig{0, milliseconds{1}, milliseconds{1}, 0.0},
               [](milliseconds) {}, 7);
    const auto zero_result = zero.Execute([&] {
        ++calls;
        return ErrorClass::kOk;
    });
    CHECK(!zero_result.ok);
    CHECK(zero_result.attempts == 0u);
    CHECK(calls == 0);

    std::vector<milliseconds> delays;
    Retry one(RetryConfig{1, milliseconds{1}, milliseconds{1}, 0.0},
              [&](milliseconds d) { delays.push_back(d); }, 7);
    const auto one_result = one.Execute([&] {
        ++calls;
        return ErrorClass::kTimeout;
    });
    CHECK(!one_result.ok);
    CHECK(one_result.attempts == 1u);
    CHECK(calls == 1);
    CHECK(delays.empty());
}

void TestRetryNegativeConfigNormalized() {
    Retry retry(RetryConfig{2, milliseconds{-5}, milliseconds{-1}, 2.0},
                [](milliseconds) {}, 7);
    CHECK(retry.config().base_delay == milliseconds{0});
    CHECK(retry.config().max_delay == milliseconds{0});
    CHECK(retry.config().jitter == 1.0);
    CHECK(retry.DelayForRetry(1) == milliseconds{0});
    CHECK(retry.DelayForRetry(0) == milliseconds{0});
}

void TestRetryDelayClamp() {
    Retry retry(RetryConfig{8, milliseconds{1000}, milliseconds{5}, 0.0},
                [](milliseconds) {}, 7);
    CHECK(retry.DelayForRetry(1) == milliseconds{5});
    CHECK(retry.DelayForRetry(2) == milliseconds{5});
    CHECK(retry.DelayForRetry(100) == milliseconds{5});
}

void TestRetryConcurrentShared() {
    Retry retry(RetryConfig{3, milliseconds{0}, milliseconds{0}, 0.2},
                [](milliseconds) {}, 12345);
    std::atomic<int> ok{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < 200; ++i) {
                const auto r = retry.Execute([] { return ErrorClass::kOk; });
                if (r.ok && r.attempts == 1u) {
                    ++ok;
                }
            }
        });
    }
    for (auto& th : threads) {
        th.join();
    }
    CHECK(ok.load() == 1600);
}

// ========================= CircuitBreaker =========================

void TestBreakerStartsClosed() {
    CircuitBreaker breaker(CircuitBreakerConfig{3, 2, milliseconds{1000}});
    CHECK(breaker.state() == CircuitState::kClosed);
    CHECK(breaker.Allow());
    CHECK(breaker.consecutive_failures() == 0u);
    CHECK(breaker.retry_after() == milliseconds{0});
}

void TestBreakerOpensAtThreshold() {
    FakeClock clock;
    CircuitBreaker breaker(CircuitBreakerConfig{3, 2, milliseconds{1000}}, clock.AsClock());
    breaker.RecordFailure();
    breaker.RecordFailure();
    CHECK(breaker.state() == CircuitState::kClosed);
    CHECK(breaker.consecutive_failures() == 2u);
    breaker.RecordFailure();
    CHECK(breaker.state() == CircuitState::kOpen);
    CHECK(!breaker.Allow());
}

void TestBreakerSuccessResetsFailureStreak() {
    FakeClock clock;
    CircuitBreaker breaker(CircuitBreakerConfig{3, 2, milliseconds{1000}}, clock.AsClock());
    breaker.RecordFailure();
    breaker.RecordFailure();
    breaker.RecordSuccess();
    CHECK(breaker.consecutive_failures() == 0u);
    breaker.RecordFailure();
    breaker.RecordFailure();
    CHECK(breaker.state() == CircuitState::kClosed);
    breaker.RecordFailure();
    CHECK(breaker.state() == CircuitState::kOpen);
}

void TestBreakerCooldownHalfOpen() {
    FakeClock clock;
    CircuitBreaker breaker(CircuitBreakerConfig{2, 2, milliseconds{1000}}, clock.AsClock());
    breaker.RecordFailure();
    breaker.RecordFailure();
    CHECK(breaker.state() == CircuitState::kOpen);
    clock.Advance(milliseconds{999});
    CHECK(!breaker.Allow());
    CHECK(breaker.state() == CircuitState::kOpen);
    clock.Advance(milliseconds{1});
    CHECK(breaker.Allow());
    CHECK(breaker.state() == CircuitState::kHalfOpen);
}

void TestBreakerHalfOpenSuccessCloses() {
    FakeClock clock;
    CircuitBreaker breaker(CircuitBreakerConfig{1, 3, milliseconds{100}}, clock.AsClock());
    breaker.RecordFailure();
    clock.Advance(milliseconds{100});
    CHECK(breaker.Allow());
    CHECK(breaker.state() == CircuitState::kHalfOpen);
    breaker.RecordSuccess();
    CHECK(breaker.state() == CircuitState::kHalfOpen);
    breaker.RecordSuccess();
    CHECK(breaker.state() == CircuitState::kHalfOpen);
    breaker.RecordSuccess();
    CHECK(breaker.state() == CircuitState::kClosed);
    CHECK(breaker.consecutive_failures() == 0u);
}

void TestBreakerHalfOpenFailureReopens() {
    FakeClock clock;
    CircuitBreaker breaker(CircuitBreakerConfig{1, 2, milliseconds{1000}}, clock.AsClock());
    breaker.RecordFailure();
    clock.Advance(milliseconds{1000});
    CHECK(breaker.Allow());
    CHECK(breaker.state() == CircuitState::kHalfOpen);
    breaker.RecordFailure();
    CHECK(breaker.state() == CircuitState::kOpen);
    clock.Advance(milliseconds{500});
    CHECK(!breaker.Allow());
    clock.Advance(milliseconds{500});
    CHECK(breaker.Allow());
    CHECK(breaker.state() == CircuitState::kHalfOpen);
}

void TestBreakerOpenIgnoresRecords() {
    FakeClock clock;
    CircuitBreaker breaker(CircuitBreakerConfig{1, 2, milliseconds{1000}}, clock.AsClock());
    breaker.RecordFailure();
    CHECK(breaker.state() == CircuitState::kOpen);
    clock.Advance(milliseconds{400});
    breaker.RecordFailure(); // Open 期间的重复失败：不推迟冷却起点。
    breaker.RecordSuccess(); // Open 期间的在途成功：不能关闸。
    CHECK(breaker.state() == CircuitState::kOpen);
    CHECK(breaker.retry_after() == milliseconds{600});
    clock.Advance(milliseconds{600});
    CHECK(breaker.retry_after() == milliseconds{0});
    CHECK(breaker.Allow());
}

void TestBreakerZeroThresholdsNormalized() {
    FakeClock clock;
    CircuitBreaker breaker(CircuitBreakerConfig{0, 0, milliseconds{500}}, clock.AsClock());
    CHECK(breaker.config().failure_threshold == 1u);
    CHECK(breaker.config().success_threshold == 1u);
    breaker.RecordFailure();
    CHECK(breaker.state() == CircuitState::kOpen);
    // 冷却 500ms 未到不放行；cooldown 本身不会被规范化成负值。
    CHECK(!breaker.Allow());
    clock.Advance(milliseconds{500});
    CHECK(breaker.Allow());
    breaker.RecordSuccess();
    CHECK(breaker.state() == CircuitState::kClosed);
}

void TestBreakerConcurrent() {
    FakeClock clock;
    CircuitBreaker breaker(CircuitBreakerConfig{10, 2, milliseconds{5000}}, clock.AsClock());
    std::atomic<int> rejected{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < 1000; ++i) {
                if (breaker.Allow()) {
                    breaker.RecordFailure();
                } else {
                    ++rejected;
                }
            }
        });
    }
    for (auto& th : threads) {
        th.join();
    }
    CHECK(breaker.state() == CircuitState::kOpen);
    CHECK(rejected.load() > 0);
    CHECK(breaker.consecutive_failures() == 0u);
}

// ========================= RateLimiter =========================

void TestLimiterBurstThenEmpty() {
    FakeClock clock;
    RateLimiter limiter(10.0, 3.0, clock.AsClock());
    CHECK(limiter.TryAcquire());
    CHECK(limiter.TryAcquire());
    CHECK(limiter.TryAcquire());
    CHECK(!limiter.TryAcquire());
    CHECK_NEAR(limiter.AvailableTokens(), 0.0, 1e-9);
}

void TestLimiterRefillAfterTime() {
    FakeClock clock;
    RateLimiter limiter(10.0, 5.0, clock.AsClock());
    for (int i = 0; i < 5; ++i) {
        CHECK(limiter.TryAcquire());
    }
    CHECK(!limiter.TryAcquire());
    clock.Advance(milliseconds{100});
    CHECK_NEAR(limiter.AvailableTokens(), 1.0, 1e-9);
    CHECK(limiter.TryAcquire());
    CHECK(!limiter.TryAcquire());
}

void TestLimiterRefillCappedAtBurst() {
    FakeClock clock;
    RateLimiter limiter(10.0, 5.0, clock.AsClock());
    for (int i = 0; i < 5; ++i) {
        CHECK(limiter.TryAcquire());
    }
    clock.Advance(seconds{10});
    CHECK_NEAR(limiter.AvailableTokens(), 5.0, 1e-9);
    for (int i = 0; i < 5; ++i) {
        CHECK(limiter.TryAcquire());
    }
    CHECK(!limiter.TryAcquire());
}

void TestLimiterPartialTokenBoundary() {
    FakeClock clock;
    RateLimiter limiter(1.0, 2.0, clock.AsClock());
    CHECK(limiter.TryAcquire(2.0));
    clock.Advance(milliseconds{500});
    CHECK_NEAR(limiter.AvailableTokens(), 0.5, 1e-9);
    CHECK(!limiter.TryAcquire(1.0));
    clock.Advance(milliseconds{500});
    CHECK_NEAR(limiter.AvailableTokens(), 1.0, 1e-9);
    CHECK(limiter.TryAcquire(1.0));
}

void TestLimiterClockBackwards() {
    FakeClock clock;
    RateLimiter limiter(10.0, 2.0, clock.AsClock());
    CHECK(limiter.TryAcquire(2.0));
    clock.Advance(milliseconds{100});
    CHECK_NEAR(limiter.AvailableTokens(), 1.0, 1e-9);
    clock.Advance(milliseconds{-50});
    CHECK_NEAR(limiter.AvailableTokens(), 1.0, 1e-9);
    CHECK(limiter.TryAcquire(1.0));
    CHECK(!limiter.TryAcquire(1.0));
}

void TestLimiterZeroRateAndBurst() {
    FakeClock clock;
    RateLimiter zero_rate(0.0, 2.0, clock.AsClock());
    CHECK(zero_rate.TryAcquire());
    CHECK(zero_rate.TryAcquire());
    clock.Advance(seconds{10});
    CHECK(!zero_rate.TryAcquire());
    CHECK_NEAR(zero_rate.AvailableTokens(), 0.0, 1e-9);

    FakeClock clock2;
    RateLimiter zero_burst(10.0, 0.0, clock2.AsClock());
    CHECK(!zero_burst.TryAcquire(0.5));
    CHECK(!zero_burst.TryAcquire(1.0));
    CHECK_NEAR(zero_burst.AvailableTokens(), 0.0, 1e-9);
    CHECK(zero_burst.TryAcquire(0.0));
}

void TestLimiterInvalidArgsClamped() {
    FakeClock clock;
    RateLimiter limiter(-5.0, -1.0, clock.AsClock());
    CHECK(limiter.rate_per_second() == 0.0);
    CHECK(limiter.burst() == 0.0);
    CHECK(limiter.TryAcquire(0.0));
    CHECK(!limiter.TryAcquire(1.0));
}

void TestLimiterCostGreaterThanBurst() {
    FakeClock clock;
    RateLimiter limiter(10.0, 2.0, clock.AsClock());
    CHECK(!limiter.TryAcquire(3.0));
    CHECK_NEAR(limiter.AvailableTokens(), 2.0, 1e-9); // 失败请求不消耗。
    CHECK(limiter.TryAcquire(2.0));
    CHECK(!limiter.TryAcquire(0.5));
}

void TestLimiterConcurrentExactlyBurst() {
    FakeClock clock;
    RateLimiter limiter(0.0, 100.0, clock.AsClock());
    std::atomic<int> acquired{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 10; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < 1000; ++i) {
                if (limiter.TryAcquire()) {
                    ++acquired;
                }
            }
        });
    }
    for (auto& th : threads) {
        th.join();
    }
    CHECK(acquired.load() == 100);
    CHECK_NEAR(limiter.AvailableTokens(), 0.0, 1e-9);
}

void TestLimiterReset() {
    FakeClock clock;
    RateLimiter limiter(1.0, 3.0, clock.AsClock());
    CHECK(limiter.TryAcquire(3.0));
    CHECK(!limiter.TryAcquire(1.0));
    limiter.Reset();
    CHECK_NEAR(limiter.AvailableTokens(), 3.0, 1e-9);
    CHECK(limiter.TryAcquire(3.0));
}

void TestLimiterFractionalTokens() {
    FakeClock clock;
    RateLimiter limiter(10.0, 10.0, clock.AsClock());
    CHECK(limiter.TryAcquire(2.5));
    CHECK(limiter.TryAcquire(2.5));
    CHECK_NEAR(limiter.AvailableTokens(), 5.0, 1e-9);
    CHECK(limiter.TryAcquire(5.0));
    CHECK_NEAR(limiter.AvailableTokens(), 0.0, 1e-9);
}

}  // namespace

int main() {
    std::printf("=== 韧性组件测试（Retry / CircuitBreaker / RateLimiter）===\n");

    RunCase("Retry/首次成功", TestRetryFirstSuccess);
    RunCase("Retry/超时后成功", TestRetryTimeoutThenSuccess);
    RunCase("Retry/重试耗尽", TestRetryExhausted);
    RunCase("Retry/4xx 不重试", TestRetryNoRetryOnClientError);
    RunCase("Retry/无抖动退避序列", TestRetryBackoffSequenceNoJitter);
    RunCase("Retry/抖动区间", TestRetryJitterBounds);
    RunCase("Retry/max_attempts=0 与 1", TestRetryZeroAndOneAttempts);
    RunCase("Retry/负配置规范化", TestRetryNegativeConfigNormalized);
    RunCase("Retry/延迟上限截断", TestRetryDelayClamp);
    RunCase("Retry/8 线程共享实例", TestRetryConcurrentShared);

    RunCase("Breaker/初始闭合", TestBreakerStartsClosed);
    RunCase("Breaker/阈值打开", TestBreakerOpensAtThreshold);
    RunCase("Breaker/成功重置连败", TestBreakerSuccessResetsFailureStreak);
    RunCase("Breaker/冷却转半开", TestBreakerCooldownHalfOpen);
    RunCase("Breaker/半开成功闭合", TestBreakerHalfOpenSuccessCloses);
    RunCase("Breaker/半开失败重开", TestBreakerHalfOpenFailureReopens);
    RunCase("Breaker/Open 忽略记录", TestBreakerOpenIgnoresRecords);
    RunCase("Breaker/零阈值规范化", TestBreakerZeroThresholdsNormalized);
    RunCase("Breaker/8 线程并发", TestBreakerConcurrent);

    RunCase("Limiter/满桶耗尽", TestLimiterBurstThenEmpty);
    RunCase("Limiter/按时间补充", TestLimiterRefillAfterTime);
    RunCase("Limiter/补充封顶", TestLimiterRefillCappedAtBurst);
    RunCase("Limiter/部分令牌边界", TestLimiterPartialTokenBoundary);
    RunCase("Limiter/时钟回退", TestLimiterClockBackwards);
    RunCase("Limiter/零速率与零容量", TestLimiterZeroRateAndBurst);
    RunCase("Limiter/非法参数", TestLimiterInvalidArgsClamped);
    RunCase("Limiter/请求超过容量", TestLimiterCostGreaterThanBurst);
    RunCase("Limiter/并发恰好 burst", TestLimiterConcurrentExactlyBurst);
    RunCase("Limiter/Reset", TestLimiterReset);
    RunCase("Limiter/小数令牌", TestLimiterFractionalTokens);

    std::printf("=== 用例 %d 个，失败 %d 处 ===\n", g_cases, g_failures);
    return g_failures == 0 ? 0 : 1;
}
