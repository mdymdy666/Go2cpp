// resilience 模块微基准：熔断判定 / 限流判定 ns/op。
//
// 口径：
//   - circuit_breaker_allow：Closed 状态下 CircuitBreaker::Allow() 一次
//     （1 次互斥量 + 1 次 steady_clock::now()，是真实放行路径）；
//   - ratelimiter_acquire：RateLimiter::TryAcquire(1.0) 一次（懒补充 + 扣减）。
// 两者都是「判定即可丢弃」的纯函数型调用，不存在队列/阻塞。

#include <cstddef>

#include "bench_common.hpp"
#include "go2cpp/resilience/circuit_breaker.hpp"
#include "go2cpp/resilience/rate_limiter.hpp"

int main() {
    using go2cpp::resilience::CircuitBreaker;
    using go2cpp::resilience::CircuitBreakerConfig;
    using go2cpp::resilience::RateLimiter;
    using go2cpp::resilience::RateLimiterConfig;

    CircuitBreaker breaker{CircuitBreakerConfig{}};
    if (!breaker.Allow()) {
        std::printf("circuit_probe_failed=1\n");
        return 1;
    }
    breaker.RecordSuccess();

    bench::RunAdaptive("circuit_breaker_allow", 20000, 150.0, [&](std::size_t n) {
        std::size_t acc = 0;
        for (std::size_t i = 0; i < n; ++i) {
            acc += breaker.Allow() ? 1u : 0u;
        }
        bench::g_sink = acc;
    });

    // 桶容量拉满、速率拉高：观测的是「判定」开销，不是限流拒绝路径。
    RateLimiter limiter{RateLimiterConfig{1e9, 1e9, -1.0}};
    bench::RunAdaptive("ratelimiter_acquire", 20000, 150.0, [&](std::size_t n) {
        std::size_t acc = 0;
        for (std::size_t i = 0; i < n; ++i) {
            acc += limiter.TryAcquire(1.0) ? 1u : 0u;
        }
        bench::g_sink = acc;
    });
    return 0;
}
