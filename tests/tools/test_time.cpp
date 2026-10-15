/**
 * @file test_time.cpp
 * @brief `go2cpp/time.hpp` 的正式测试：Go time 语义边界 + 大数据量 + 随机往返 + 真实场景。
 *
 * 测试设计（按项目规范：大数据量 / 随机 / ≥5 真实场景）：
 *   场景 1 超时预算计算（deadline - now，负数即超时）   场景 4 日志时间戳格式化
 *   场景 2 重试退避（指数退避序列）                     场景 5 周期任务节拍（Ticker 间隔累加）
 *   场景 3 指标窗口（滑动窗口的时间边界）               场景 6 耗时统计（Since 单调性）
 *   大数据量：10 万次 Duration 格式化 + 10 万次纳秒↔Duration 往返
 *   随机：随机纳秒 → String() → 语义回算校验、随机 Add/Sub 往返、随机比较与 Truncate/Round
 *
 * ⚠️ 注释中不得出现连续的斜杠加星号（GCC -Wcomment）。
 */
#include "go2cpp/time.hpp"
#include "test_support.hpp"

#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

namespace {

using go2cpp::time::Duration;
using go2cpp::time::Time;

/// 独立参考实现：把纳秒数格式化成 Go 的紧凑形式（与实现完全不同的写法，用于对拍）。
std::string ReferenceFormat(std::int64_t nanoseconds) {
    constexpr std::int64_t kSecond = 1000000000LL;
    const bool negative = nanoseconds < 0;
    std::uint64_t value = static_cast<std::uint64_t>(negative ? -nanoseconds : nanoseconds);
    if (value == 0) return "0s";
    std::string out;
    if (value < static_cast<std::uint64_t>(kSecond)) {
        // 小于 1 秒：按 ns / µs / ms 选单位，保留有效小数
        const char* unit = "ns";
        std::uint64_t scale = 1;
        if (value >= 1000000ULL) {
            unit = "ms";
            scale = 1000000ULL;
        } else if (value >= 1000ULL) {
            unit = "\xC2\xB5s";
            scale = 1000ULL;
        }
        const std::uint64_t whole = value / scale;
        const std::uint64_t remainder = value % scale;
        out = std::to_string(whole);
        if (remainder != 0) {
            // 小数部分按 scale 的位数补零后去尾零
            std::string fraction = std::to_string(remainder);
            while (fraction.size() < std::to_string(scale).size() - 1) fraction.insert(fraction.begin(), '0');
            while (!fraction.empty() && fraction.back() == '0') fraction.pop_back();
            if (!fraction.empty()) out += "." + fraction;
        }
        out += unit;
    } else {
        const std::uint64_t total_seconds = value / static_cast<std::uint64_t>(kSecond);
        const std::uint64_t fraction = value % static_cast<std::uint64_t>(kSecond);
        const std::uint64_t hours = total_seconds / 3600;
        const std::uint64_t minutes = (total_seconds % 3600) / 60;
        const std::uint64_t seconds = total_seconds % 60;
        if (hours > 0) out += std::to_string(hours) + "h";
        if (hours > 0 || minutes > 0) out += std::to_string(minutes) + "m";
        out += std::to_string(seconds);
        if (fraction != 0) {
            std::string digits = std::to_string(fraction);
            while (digits.size() < 9) digits.insert(digits.begin(), '0');
            while (!digits.empty() && digits.back() == '0') digits.pop_back();
            if (!digits.empty()) out += "." + digits;
        }
        out += "s";
    }
    return negative ? "-" + out : out;
}

}  // namespace

/**
 * @brief 运行 time 模块的全部测试。
 */
void run_time_tests() {
    go2cpp_tests::announce("time: Go 语义边界 + 大数据量 + 随机往返 + 真实场景");
    using namespace go2cpp;
    using namespace go2cpp::time;

    // ---------------- 场景 1：超时预算计算 ----------------
    {
        const Time deadline = Time::Now().Add(500 * Millisecond);
        const Duration budget = Until(deadline);
        GO2CPP_CHECK(budget.Nanoseconds() > 0);
        GO2CPP_CHECK(budget.Nanoseconds() <= (500 * Millisecond).Nanoseconds());
        const Time expired = Time::Now().Add(-Second);
        GO2CPP_CHECK(Until(expired).Nanoseconds() < 0);
        GO2CPP_CHECK(expired.Before(Time::Now()));
    }

    // ---------------- 场景 2：重试退避序列 ----------------
    {
        std::vector<std::string> backoff;
        Duration delay = 100 * Millisecond;
        for (int attempt = 0; attempt < 5; ++attempt) {
            backoff.push_back(delay.String());
            delay = delay * 2;                                  // 指数退避
            if (delay > 5 * Second) delay = 5 * Second;         // 上限封顶
        }
        GO2CPP_CHECK(backoff.size() == 5);
        GO2CPP_CHECK(backoff[0] == "100ms");
        GO2CPP_CHECK(backoff[1] == "200ms");
        GO2CPP_CHECK(backoff[2] == "400ms");
        GO2CPP_CHECK(backoff[3] == "800ms");
        GO2CPP_CHECK(backoff[4] == "1.6s");
        GO2CPP_CHECK((10 * Second > 5 * Second) && ((10 * Second).Truncate(Second).Nanoseconds() ==
                                                    (10 * Second).Nanoseconds()));
    }

    // ---------------- 场景 3：指标窗口（滑动窗口边界） ----------------
    {
        const Time window_start = Time::FromUnix(1700000000);
        const Duration window = 2 * Minute;
        const Time window_end = window_start.Add(window);
        GO2CPP_CHECK(window_end.Sub(window_start).Nanoseconds() == (2 * Minute).Nanoseconds());
        const Time sample = Time::FromUnix(1700000030);         // 30 秒后
        GO2CPP_CHECK(sample.After(window_start) && sample.Before(window_end));
        GO2CPP_CHECK(window_start.Add(Minute).Before(window_end));      // 窗口内第一分钟
        GO2CPP_CHECK(window_end.Add(-Second).Before(window_end));
    }

    // ---------------- 场景 4：日志时间戳格式化 ----------------
    {
        const Time stamp = Time::FromUnix(1700000000, 123000000);
        GO2CPP_CHECK(stamp.String() == "2023-11-14 22:13:20.123 +0000");
        GO2CPP_CHECK(Time::FromUnix(0).String() == "1970-01-01 00:00:00 +0000");
        GO2CPP_CHECK(Time::FromUnix(1700000000).String() == "2023-11-14 22:13:20 +0000");
        // 纳秒精度：小数末尾的零要被去掉
        GO2CPP_CHECK(Time::FromUnix(1, 1000).String() == "1970-01-01 00:00:01.000001 +0000");
    }

    // ---------------- 场景 5：周期任务节拍（累积误差应为零） ----------------
    {
        const Time base = Time::FromUnix(1700000000);
        Duration accumulated;
        for (int tick = 1; tick <= 1000; ++tick) {
            const Time at = base.Add(tick * 250 * Millisecond);
            accumulated = accumulated + (at.Sub(base) - (tick - 1) * 250 * Millisecond);
        }
        // 累积到最后一次：第 1000 拍相对第 999 拍应正好 250ms
        GO2CPP_CHECK(accumulated.Nanoseconds() == (1000 * 250 * Millisecond).Nanoseconds());
    }

    // ---------------- 场景 6：耗时统计（Since 的单调性与量级） ----------------
    {
        const Time start = Time::Now();
        Sleep(3 * Millisecond);
        const Duration elapsed = Since(start);
        GO2CPP_CHECK(elapsed.Nanoseconds() >= 2000000LL);       // 至少 2ms（放宽调度误差）
        GO2CPP_CHECK(elapsed.Nanoseconds() < 1000000000LL);     // 不该到 1 秒
        GO2CPP_CHECK(elapsed.String().size() >= 2);             // 形如 "3.1ms"
    }

    // ---------------- 语义边界 ----------------
    {
        GO2CPP_CHECK(Duration{}.String() == "0s");
        GO2CPP_CHECK(Duration::FromNanoseconds(1).String() == "1ns");
        GO2CPP_CHECK(Duration::FromNanoseconds(-1).String() == "-1ns");
        GO2CPP_CHECK(Duration::FromNanoseconds(1000).String() == "1\xC2\xB5s");
        GO2CPP_CHECK(Duration::FromNanoseconds(1000000).String() == "1ms");
        GO2CPP_CHECK(Second.String() == "1s");
        GO2CPP_CHECK((1500 * Millisecond).String() == "1.5s");
        GO2CPP_CHECK(Minute.String() == "1m0s");
        GO2CPP_CHECK(Hour.String() == "1h0m0s");
        GO2CPP_CHECK(((Hour + Minute) + 1500 * Millisecond).String() == "1h1m1.5s");
        GO2CPP_CHECK(Duration::FromNanoseconds(1000000500).String() == "1.0000005s");
        GO2CPP_CHECK((2 * Second).Nanoseconds() == 2000000000LL);
        GO2CPP_CHECK((Minute / 2).String() == "30s");        // Minute/2 是 30 秒，不是 30 分钟
        GO2CPP_CHECK(Hour.Truncate(Minute).String() == "1h0m0s");
        GO2CPP_CHECK((1500 * Millisecond).Round(Second).String() == "2s");
        GO2CPP_CHECK(Duration::FromNanoseconds(-1500).Abs().Nanoseconds() == 1500);
        // 零 Time 与 Unix 0 的区别（Go 的零值是 1 年 1 月）
        GO2CPP_CHECK(Time{}.IsZero());
        GO2CPP_CHECK(!Time::FromUnix(0).IsZero());
        // Equal 只用墙钟，Sub 优先单调读数
        const Time a = Time::FromUnix(100);
        const Time b = Time::FromUnix(100);
        GO2CPP_CHECK(a.Equal(b) && !a.Before(b) && !a.After(b));
        GO2CPP_CHECK(a.Sub(b).Nanoseconds() == 0);
        // Sleep 非正时长立即返回
        Sleep(Duration{});
        Sleep(-Second);
    }

    // ---------------- 大数据量 1：10 万次 Duration 格式化 ----------------
    {
        constexpr std::size_t kIterations = 100000;
        std::size_t total = 0;
        for (std::size_t i = 0; i < kIterations; ++i) {
            const std::int64_t nanos = static_cast<std::int64_t>(i) * 12347 - 500000000LL;
            total += Duration::FromNanoseconds(nanos).String().size();
        }
        GO2CPP_CHECK(total > kIterations);                      // 每个结果至少 2 字符
    }

    // ---------------- 大数据量 2：10 万次纳秒换算往返 ----------------
    {
        constexpr std::size_t kIterations = 100000;
        std::int64_t mismatches = 0;
        for (std::size_t i = 0; i < kIterations; ++i) {
            const std::int64_t nanos = static_cast<std::int64_t>(i) * 7919 - 400000000LL;
            const Duration duration = Duration::FromNanoseconds(nanos);
            if (duration.Nanoseconds() != nanos) ++mismatches;
            // 注意：是「同一个 Duration 的单位换算」，不是再构造一次（早先多除了一次）
            if (Duration::FromNanoseconds(nanos).Microseconds() != nanos / 1000) ++mismatches;
            if (Duration::FromNanoseconds(nanos).Milliseconds() != nanos / 1000000) {
                ++mismatches;
            }
        }
        GO2CPP_CHECK(mismatches == 0);
    }

    // ---------------- 随机 1：随机纳秒 → String() 与独立参考实现对拍 ----------------
    {
        std::mt19937_64 rng(20261015ULL);
        std::uniform_int_distribution<std::int64_t> dist(-4000000000000LL, 4000000000000LL);
        std::int64_t mismatches = 0;
        std::string first_mismatch;
        for (int iteration = 0; iteration < 100000; ++iteration) {
            const std::int64_t nanos = dist(rng);
            const std::string ours = Duration::FromNanoseconds(nanos).String();
            const std::string reference = ReferenceFormat(nanos);
            if (ours != reference) {
                ++mismatches;
                if (first_mismatch.empty()) {
                    first_mismatch = std::to_string(nanos) + " ours=" + ours + " ref=" + reference;
                }
            }
        }
        if (!first_mismatch.empty()) std::printf("  first mismatch: %s\n", first_mismatch.c_str());
        GO2CPP_CHECK(mismatches == 0);
    }

    // ---------------- 随机 2：随机 Add/Sub 往返 ----------------
    {
        std::mt19937_64 rng(4242ULL);
        std::uniform_int_distribution<std::int64_t> base_dist(0, 4000000000LL);
        std::uniform_int_distribution<std::int64_t> delta_dist(-2000000000LL, 2000000000LL);
        std::int64_t mismatches = 0;
        for (int iteration = 0; iteration < 100000; ++iteration) {
            const Time base = Time::FromUnix(base_dist(rng));
            const Duration delta = Duration::FromNanoseconds(delta_dist(rng));
            const Time moved = base.Add(delta);
            if (moved.Sub(base).Nanoseconds() != delta.Nanoseconds()) ++mismatches;
            if (moved.Add(-delta).Sub(base).Nanoseconds() != 0) ++mismatches;
            if (base.Add(-delta).Before(moved) != (delta.Nanoseconds() > 0)) ++mismatches;
        }
        GO2CPP_CHECK(mismatches == 0);
    }

    // ---------------- 随机 3：随机比较 / Truncate / Round 的性质 ----------------
    {
        std::mt19937_64 rng(31337ULL);
        std::uniform_int_distribution<std::int64_t> dist(-100000000000LL, 100000000000LL);
        std::int64_t mismatches = 0;
        for (int iteration = 0; iteration < 100000; ++iteration) {
            const std::int64_t left = dist(rng);
            const std::int64_t right = dist(rng);
            const Duration a = Duration::FromNanoseconds(left);
            const Duration b = Duration::FromNanoseconds(right);
            // 比较的三分律
            const int comparisons = (a < b ? 1 : 0) + (a == b ? 1 : 0) + (a > b ? 1 : 0);
            if (comparisons != 1) ++mismatches;
            // Truncate 结果的绝对值不超过原值，且是单位的整数倍
            const Duration unit = Millisecond;
            const Duration truncated = a.Truncate(unit);
            if (truncated.Nanoseconds() % unit.Nanoseconds() != 0) ++mismatches;
            if (truncated.Abs().Nanoseconds() > a.Abs().Nanoseconds()) ++mismatches;
            // Round 与 Truncate 的差值不超过一个单位（负数时差为负，故取绝对值）
            const Duration rounded = a.Round(unit);
            const std::int64_t gap = rounded.Nanoseconds() - truncated.Nanoseconds();
            const std::int64_t magnitude = gap < 0 ? -gap : gap;
            if (magnitude > unit.Nanoseconds()) ++mismatches;
        }
        GO2CPP_CHECK(mismatches == 0);
    }
}
