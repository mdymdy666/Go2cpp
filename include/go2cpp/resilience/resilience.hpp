#pragma once

/**
 * @file resilience.hpp
 * @brief 韧性组件聚合头：重试 / 熔断 / 限流。
 *
 * 三者组合顺序（调用方自上而下）：
 *
 * ```
 *   入口限流 RateLimiter.TryAcquire
 *     └─ 熔断 CircuitBreaker.Allow
 *          └─ 重试 Retry.Execute  → 上游调用
 * ```
 *
 * 限流在最外层保护自己，熔断在中间保护上游，重试在最内层处理抖动。
 * 反向组合会放大流量（重试绕过限流）或制造惊群（限流在重试内部）。
 */
#include "go2cpp/resilience/circuit_breaker.hpp"
#include "go2cpp/resilience/clock.hpp"
#include "go2cpp/resilience/rate_limiter.hpp"
#include "go2cpp/resilience/retry.hpp"
