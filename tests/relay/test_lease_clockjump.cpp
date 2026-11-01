/**
 * @file test_lease_clockjump.cpp
 * @brief 用 LD_PRELOAD 注入墙钟跳变，验证 Lease 只依赖单调时钟（REQ-005）。
 *
 * 断言方式：
 * - 正向跳变（+1h）发生在租约计时中间时，若实现用墙钟算截止时间，租约会在
 *   跳变瞬间立刻到期；正确实现应在 monotonic 的 timeout 处到期；
 * - 反向跳变（-1h）时，墙钟实现会以为还剩约 1 小时，5 秒内不会触发；
 *   正确实现仍在 monotonic 的 timeout 处到期；
 * - 跳变之后 `Stop()` 仍然立即生效。
 *
 * 本用例必须由 ctest 以 `LD_PRELOAD=libgo2cpp_relay_fake_clock.so` 启动；
 * 直接运行时会打印 skipped 并返回 0（不是失败）。
 */
#include "go2cpp/relay/lease.hpp"

#include "relay_test_util.hpp"
#include "test_support.hpp"

#include <dlfcn.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>

namespace {

using namespace std::chrono_literals;
using go2cpp::relay::Lease;
using go2cpp::relay::test::SleepMs;
using go2cpp::relay::test::SteadyNowMs;

using TriggerFn = void (*)();

/** @brief 通过 dlsym 找到垫片导出的触发函数。 */
struct FakeClock {
    TriggerFn forward{nullptr};
    TriggerFn backward{nullptr};
    TriggerFn reset{nullptr};

    bool Available() const {
        return forward != nullptr && backward != nullptr && reset != nullptr;
    }
};

FakeClock LoadFakeClock() {
    FakeClock clock;
    clock.forward = reinterpret_cast<TriggerFn>(
        dlsym(RTLD_DEFAULT, "go2cpp_fake_clock_jump_forward"));
    clock.backward = reinterpret_cast<TriggerFn>(
        dlsym(RTLD_DEFAULT, "go2cpp_fake_clock_jump_backward"));
    clock.reset = reinterpret_cast<TriggerFn>(
        dlsym(RTLD_DEFAULT, "go2cpp_fake_clock_reset"));
    return clock;
}

/** @brief 记录到期次数与首次触发时刻。 */
struct ExpiryCounter {
    std::atomic<int> count{0};
    std::atomic<std::int64_t> first_fire_ms{0};

    void OnExpire() {
        if (count.fetch_add(1) == 0) {
            first_fire_ms.store(SteadyNowMs());
        }
    }
};

/** @brief 场景 1：正向前跳发生在租约窗口中间。 */
void TestForwardJumpDuringLease(const FakeClock &fake) {
    ExpiryCounter counter;
    const auto wall_before = std::chrono::system_clock::now();
    const std::int64_t start = SteadyNowMs();
    const Lease lease(800ms, [&counter] { counter.OnExpire(); });

    SleepMs(250);
    GO2CPP_CHECK(counter.count.load() == 0); // 250ms 时不应到期
    fake.forward();
    const auto wall_after = std::chrono::system_clock::now();
    const auto wall_delta =
        std::chrono::duration_cast<std::chrono::seconds>(wall_after - wall_before).count();
    // 先确认垫片确实生效，否则本用例证明不了任何事。
    GO2CPP_CHECK(wall_delta >= 3000);

    GO2CPP_REQUIRE_EVENTUALLY(counter.count.load() == 1, 5s);
    const std::int64_t elapsed = counter.first_fire_ms.load() - start;
    // 墙钟实现会在跳变瞬间（约 250ms）就触发；正确实现约 800ms。
    GO2CPP_CHECK(elapsed >= 750);
    GO2CPP_CHECK(elapsed <= 3000);
    SleepMs(200);
    GO2CPP_CHECK(counter.count.load() == 1);
    std::printf("[clockjump] +1h：租约 %lld ms 到期（配置 800ms，墙钟跳变量 %lld s）\n",
                static_cast<long long>(elapsed), static_cast<long long>(wall_delta));
    fake.reset();
}

/** @brief 场景 2：反向前跳发生在租约窗口中间（墙钟实现会晚约 1 小时）。 */
void TestBackwardJumpDuringLease(const FakeClock &fake) {
    ExpiryCounter counter;
    const std::int64_t start = SteadyNowMs();
    const Lease lease(700ms, [&counter] { counter.OnExpire(); });

    SleepMs(200);
    fake.backward();
    const auto wall_after = std::chrono::system_clock::now();
    (void)wall_after;

    // 若实现用墙钟计算截止时间，这里要等约 1 小时，5 秒超时即失败。
    GO2CPP_REQUIRE_EVENTUALLY(counter.count.load() == 1, 5s);
    const std::int64_t elapsed = counter.first_fire_ms.load() - start;
    GO2CPP_CHECK(elapsed >= 650);
    GO2CPP_CHECK(elapsed <= 3000);
    std::printf("[clockjump] -1h：租约 %lld ms 到期（配置 700ms）\n",
                static_cast<long long>(elapsed));
    fake.reset();
}

/** @brief 场景 3：跳变之后 Stop 仍然立即生效。 */
void TestStopAfterJump(const FakeClock &fake) {
    ExpiryCounter counter;
    {
        Lease lease(500ms, [&counter] { counter.OnExpire(); });
        SleepMs(100);
        fake.forward();
        SleepMs(100);
        lease.Stop();
    }
    SleepMs(700);
    GO2CPP_CHECK(counter.count.load() == 0);
    fake.reset();
}

/** @brief 场景 4：跳变之后 Renew 仍然按单调时钟重置窗口。 */
void TestRenewAfterJump(const FakeClock &fake) {
    ExpiryCounter counter;
    const std::int64_t start = SteadyNowMs();
    Lease lease(600ms, [&counter] { counter.OnExpire(); });

    SleepMs(150);
    fake.forward();
    SleepMs(150);
    GO2CPP_CHECK(counter.count.load() == 0);
    const std::int64_t renew_at = SteadyNowMs();
    lease.Renew();

    GO2CPP_REQUIRE_EVENTUALLY(counter.count.load() == 1, 5s);
    const std::int64_t since_renew = counter.first_fire_ms.load() - renew_at;
    GO2CPP_CHECK(since_renew >= 550);
    GO2CPP_CHECK(counter.first_fire_ms.load() - start <= 3000);
    fake.reset();
}

} // namespace

int main() {
    go2cpp_tests::announce("relay.lease.clockjump");

    const FakeClock fake = LoadFakeClock();
    if (!fake.Available()) {
        std::printf(
            "relay_lease_clockjump_test: skipped（需通过 ctest 以 "
            "LD_PRELOAD=libgo2cpp_relay_fake_clock.so 运行）\n");
        return 0;
    }

    TestForwardJumpDuringLease(fake);
    TestBackwardJumpDuringLease(fake);
    TestStopAfterJump(fake);
    TestRenewAfterJump(fake);

    const int failures = go2cpp_tests::g_failures.load();
    if (failures != 0) {
        std::fprintf(stderr, "relay_lease_clockjump_test: %d checks failed\n", failures);
        return 1;
    }
    std::printf("relay_lease_clockjump_test: all checks passed\n");
    return 0;
}
