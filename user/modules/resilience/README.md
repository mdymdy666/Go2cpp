# 韧性（resilience）

> **一句话定位**：重试 / 熔断 / 限流——把「上游抖动」挡在业务代码之外。
> 实现位于 [`include/go2cpp/resilience/`](../../../include/go2cpp/resilience/)，
> header-only、无运行时依赖、不链接主库。

## 为什么需要它

真实网络里三类故障会反复出现，而业务代码不该各写一遍：

| 故障 | 症状 | 组件 | 错误做法 |
|------|------|------|----------|
| 上游超时 | 偶发 `kTimeout`，重试即恢复 | `Retry` | 裸 `while` 重试 → 重试风暴 |
| 连续失败 | 上游整体不可用，请求全超时 | `CircuitBreaker` | 继续打满 → 上游彻底起不来 |
| 突发流量 | 瞬时 QPS 超出口径 | `RateLimiter` | 无限放行 → 自己被打挂 |

## 组件与 API

```cpp
#include "go2cpp/resilience/resilience.hpp"
using namespace go2cpp::resilience;
```

### Retry：指数退避 + 抖动

```cpp
// 最多 4 次尝试；退避 20ms → 40ms → 80ms，上限 500ms，±20% 抖动。
Retry retry(RetryConfig{4, std::chrono::milliseconds{20},
                        std::chrono::milliseconds{500}, 0.2});

auto r = retry.Execute([&] {
    return DoRequest() ? ErrorClass::kOk : ErrorClass::kTimeout;
});
if (!r.ok) {
    // r.attempts / r.total_delay / r.last_error
}
```

只有 `kTimeout` / `kClosed` / `kUnavailable` 会重试；`kClientError`（4xx）
与 `kPermanent` 立即返回。**调用方必须保证操作幂等**——超时时请求可能
已经到达上游。

### CircuitBreaker：三态熔断

```cpp
CircuitBreaker breaker(CircuitBreakerConfig{
    5,                            // 连续失败 5 次 → Open
    2,                            // 半开连续成功 2 次 → Closed
    std::chrono::seconds{30}});   // Open 冷却 30s → HalfOpen

if (!breaker.Allow()) {
    return Degrade();             // 快速失败，不碰上游
}
if (DoRequest()) {
    breaker.RecordSuccess();
} else {
    breaker.RecordFailure();
}
```

`Allow()` 放行后**必须**恰好配一次 `RecordSuccess` / `RecordFailure`；
漏记会让半开探测统计失真。

### RateLimiter：令牌桶

```cpp
// 1000 QPS，允许 2000 的突发（桶容量）。
RateLimiter limiter(1000.0, 2000.0);

if (!limiter.TryAcquire()) {
    return TooManyRequests();     // 或降级 / 入队，不要忙等
}
```

`TryAcquire()` 不阻塞：桶不足立即返回 `false`，由调用方决定拒绝还是排队。

## 三个异常场景

### 场景 1：上游超时（Retry）

上游偶发 200ms 超时，单次失败不代表不可用：

```cpp
Retry retry(RetryConfig{4, std::chrono::milliseconds{20},
                        std::chrono::milliseconds{500}, 0.2});
auto result = retry.Execute([&] {
    if (CallUpstream()) return ErrorClass::kOk;
    return ClassifyUpstreamError();   // 超时→kTimeout，4xx→kClientError
});
if (!result.ok && result.last_error == ErrorClass::kClientError) {
    return BadRequest();              // 不重试，直接上报
}
```

**边界**：`max_attempts` 含首次调用；`max_attempts == 0` 时一次都不调用。
延迟序列可用 `DelayForRetry(n)` 单测，无需真睡。

### 场景 2：连续失败（CircuitBreaker）

上游整体宕机，重试只会放大故障：熔断器在阈值后直接拒绝，冷却后半开探测：

```cpp
if (!breaker.Allow()) {
    return CachedOrDefault();         // 保护上游，也保护自己的线程池
}
const bool ok = CallUpstream();
if (ok) {
    breaker.RecordSuccess();
} else {
    breaker.RecordFailure();
}
```

**边界**：`Open` 期间的在途成功**不会**关闭熔断（避免掩盖持续故障）；
半开阶段任一失败立即重新 `Open` 并重新计时。

### 场景 3：突发流量（RateLimiter + 组合）

秒杀/重连风暴时按桶拒绝，超出的请求快速失败：

```cpp
RateLimiter limiter(1000.0, 2000.0);

bool HandleRequest() {
    if (!limiter.TryAcquire()) return false;   // 429 / 降级
    if (!breaker.Allow()) return false;
    return retry.Execute([&] { /* 上游调用 */ }).ok;
}
```

**组合顺序固定为：限流 → 熔断 → 重试**。反过来会让重试绕过限流，
或在熔断外面白等重试时间。

## 测试

```sh
tests/resilience/build_and_run.sh              # 30 用例
SANITIZE=thread tests/resilience/build_and_run.sh   # 并发检查
```

覆盖：重试耗尽、4xx 不重试、抖动区间、退避截断、熔断打开/半开恢复、
冷却边界、时钟回退、零速率/零容量、并发恰好消耗 burst、8/10 线程竞争。
组件不依赖 go2cpp 运行时，测试可直接用 g++ 编译。

## 设计边界

- 时间源可注入（`Clock`），生产用 `steady_clock`，测试用假时钟；
- 三个组件均线程安全（互斥量保护状态），可在 fiber 与普通线程中调用；
- 限流器**不排队**、熔断器**不重试**、重试器**不判重**——职责单一。
