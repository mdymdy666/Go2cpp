/**
 * @file test_heartbeat.cpp
 * @brief MOD-11 心跳与超时：正常节拍/超时触发/停止/假时钟边界/默认配置。
 * @author go2cpp contributors
 * @date 2026-10-28
 * @copyright MIT License
 */

#include <cstdint>
#include <cstdio>

#include "go2cpp/bento/net/heartbeat.hpp"
#include "test_util.hpp"

namespace {

using namespace go2cpp::bento;

/// 可手工推进的假时钟，毫秒。
struct FakeClock {
    std::uint64_t now = 0;
};

void TestDefaultsAndMonotonicClock() {
    Heartbeat heartbeat;
    BENTO_CHECK_EQ(heartbeat.IntervalMs(), Heartbeat::kDefaultIntervalMs);
    BENTO_CHECK_EQ(heartbeat.TimeoutMs(), Heartbeat::kDefaultTimeoutMs);
    BENTO_CHECK(!heartbeat.IsRunning());
    BENTO_CHECK(heartbeat.IsAlive(Heartbeat::kDefaultTimeoutMs));

    const std::uint64_t first = MonotonicNowMs();
    const std::uint64_t second = MonotonicNowMs();
    BENTO_CHECK(second >= first);
}

void TestHeartbeatCadence() {
    FakeClock clock{1000};
    Heartbeat heartbeat(1000, 90000, [&clock]() { return clock.now; });
    heartbeat.Start(1000);
    BENTO_CHECK(heartbeat.IsRunning());
    BENTO_CHECK_EQ(heartbeat.NextBeatMs(), 2000U);

    BENTO_CHECK(!heartbeat.Poll());
    clock.now = 1999;
    BENTO_CHECK(!heartbeat.Poll());
    clock.now = 2000;
    BENTO_CHECK(heartbeat.Poll());
    BENTO_CHECK_EQ(heartbeat.Beats(), 1U);
    BENTO_CHECK_EQ(heartbeat.NextBeatMs(), 3000U);

    clock.now = 2999;
    BENTO_CHECK(!heartbeat.Poll());
    clock.now = 3000;
    BENTO_CHECK(heartbeat.Poll());
    BENTO_CHECK_EQ(heartbeat.Beats(), 2U);

    // 时钟大步跳跃：只报一次，不补发（心跳只为刷新存活）。
    clock.now = 10000;
    BENTO_CHECK(heartbeat.Poll());
    BENTO_CHECK_EQ(heartbeat.Beats(), 3U);
    BENTO_CHECK_EQ(heartbeat.NextBeatMs(), 11000U);
    BENTO_CHECK(!heartbeat.Poll());
}

void TestTimeoutFiresOnce() {
    FakeClock clock{0};
    int fired = 0;
    Heartbeat heartbeat(1000, 3000, [&clock]() { return clock.now; },
                        [&fired]() { ++fired; });
    heartbeat.Start(1000);

    clock.now = 2999;
    heartbeat.Poll();  // 有心跳到期，但未到超时
    BENTO_CHECK_EQ(fired, 0);
    BENTO_CHECK(heartbeat.IsAlive());
    BENTO_CHECK(!heartbeat.IsTimedOut());

    clock.now = 3000;  // 恰好等于阈值 -> 判超时，且本 tick 不再发心跳
    BENTO_CHECK(!heartbeat.Poll());
    BENTO_CHECK_EQ(fired, 1);
    BENTO_CHECK(heartbeat.IsTimedOut());
    BENTO_CHECK(!heartbeat.IsAlive());
    BENTO_CHECK_EQ(heartbeat.TimeoutsFired(), 1U);

    clock.now = 5000;
    BENTO_CHECK(!heartbeat.Poll());
    BENTO_CHECK_EQ(fired, 1);  // 未恢复活动前不重复触发
    BENTO_CHECK_EQ(heartbeat.TimeoutsFired(), 1U);

    heartbeat.OnReceiveAt(clock.now);  // 对端恢复
    BENTO_CHECK(!heartbeat.IsTimedOut());
    BENTO_CHECK(heartbeat.IsAlive());

    clock.now = 8000;  // 距恢复恰好 3000ms -> 再次超时
    BENTO_CHECK(!heartbeat.Poll());
    BENTO_CHECK_EQ(fired, 2);
    BENTO_CHECK_EQ(heartbeat.TimeoutsFired(), 2U);
}

void TestStop() {
    FakeClock clock{0};
    int fired = 0;
    Heartbeat heartbeat(1000, 3000, [&clock]() { return clock.now; },
                        [&fired]() { ++fired; });
    heartbeat.Start(1000);
    BENTO_CHECK(heartbeat.IsRunning());
    heartbeat.Stop();
    BENTO_CHECK(!heartbeat.IsRunning());

    clock.now = 100000;
    BENTO_CHECK(!heartbeat.Poll());
    BENTO_CHECK_EQ(fired, 0);
    BENTO_CHECK_EQ(heartbeat.Beats(), 0U);

    // IsAlive 只看时间窗，与运行状态无关（便于状态机先判活再决定是否 Stop）。
    BENTO_CHECK(!heartbeat.IsAlive());

    // Stop 后再次 Start 可复用同一实例。
    heartbeat.Start(500);
    BENTO_CHECK(heartbeat.IsRunning());
    clock.now = 100500;
    BENTO_CHECK(heartbeat.Poll());
    BENTO_CHECK_EQ(heartbeat.Beats(), 1U);
}

void TestIsAliveBoundaries() {
    FakeClock clock{10000};
    Heartbeat heartbeat(1000, 90000, [&clock]() { return clock.now; });
    heartbeat.OnReceiveAt(10000);

    BENTO_CHECK(heartbeat.IsAlive(1));
    BENTO_CHECK(!heartbeat.IsAlive(0));  // 0 表示立即判死，避免漏配当成永活
    BENTO_CHECK(heartbeat.IsAlive(11));

    clock.now = 10089;
    BENTO_CHECK(heartbeat.IsAlive(90));
    clock.now = 10090;
    BENTO_CHECK(!heartbeat.IsAlive(90));

    // 刷新活动后重新获得完整窗口。
    heartbeat.OnReceiveAt(clock.now);
    BENTO_CHECK(heartbeat.IsAlive(90));
    BENTO_CHECK_EQ(heartbeat.LastActivityMs(), 10090U);
}

void TestRestartAndRuntimeConfig() {
    FakeClock clock{0};
    Heartbeat heartbeat(1000, 5000, [&clock]() { return clock.now; });
    heartbeat.Start(1000);
    clock.now = 1000;
    BENTO_CHECK(heartbeat.Poll());
    BENTO_CHECK_EQ(heartbeat.Beats(), 1U);

    // 重启：计数清零，下一次心跳按新间隔从当前时刻重排。
    heartbeat.Start(2000);
    BENTO_CHECK_EQ(heartbeat.Beats(), 0U);
    BENTO_CHECK_EQ(heartbeat.IntervalMs(), 2000U);
    BENTO_CHECK_EQ(heartbeat.NextBeatMs(), 3000U);
    clock.now = 2999;
    BENTO_CHECK(!heartbeat.Poll());
    clock.now = 3000;
    BENTO_CHECK(heartbeat.Poll());

    // 运行中改间隔：从当前时刻重新排期，不沿用旧到期点。
    clock.now = 3500;
    heartbeat.SetInterval(5000);
    BENTO_CHECK_EQ(heartbeat.NextBeatMs(), 8500U);
    BENTO_CHECK(!heartbeat.Poll());

    // 0 表示恢复默认值，而不是关闭。
    heartbeat.SetInterval(0);
    BENTO_CHECK_EQ(heartbeat.IntervalMs(), Heartbeat::kDefaultIntervalMs);
    heartbeat.SetTimeout(0);
    BENTO_CHECK_EQ(heartbeat.TimeoutMs(), Heartbeat::kDefaultTimeoutMs);
    heartbeat.SetTimeout(1500);
    BENTO_CHECK_EQ(heartbeat.TimeoutMs(), 1500U);
}

void TestRealClockSmoke() {
    Heartbeat heartbeat;  // 默认 30s/90s + steady_clock
    heartbeat.Start();
    BENTO_CHECK(!heartbeat.Poll());  // 30s 未到
    BENTO_CHECK(heartbeat.IsAlive());
    heartbeat.OnReceive();
    BENTO_CHECK(heartbeat.IsAlive(Heartbeat::kDefaultTimeoutMs));
    heartbeat.Stop();
    BENTO_CHECK(!heartbeat.IsRunning());
}

}  // namespace

int main() {
    TestDefaultsAndMonotonicClock();
    TestHeartbeatCadence();
    TestTimeoutFiresOnce();
    TestStop();
    TestIsAliveBoundaries();
    TestRestartAndRuntimeConfig();
    TestRealClockSmoke();
    return bento_test::Finish("test_heartbeat");
}
