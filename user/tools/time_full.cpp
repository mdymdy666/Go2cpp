/**
 * @file time_full.cpp
 * @brief `go2cpp/time.hpp` 全面版：六个场景覆盖截止、退避、窗口、时间戳、节拍与格式化。
 *
 * @details 场景：
 *   1 超时预算与截止判断            4 日志时间戳格式化（含纳秒与去尾零）
 *   2 重试退避序列（指数 + 封顶）    5 周期任务节拍（时间点累加与零漂移校验）
 *   3 指标窗口边界（滑动窗口）       6 Duration 格式化对照表 + 10 万次规模（计时仅供参考）
 *
 * @note 计时数字只用于确认没有爆炸性劣化；性能结论必须用 Release 基准（build-bench）测量。
 * @return 全部场景通过返回 0，否则返回 1。
 */
#include "go2cpp/runtime.hpp"
#include "go2cpp/strings.hpp"
#include "go2cpp/time.hpp"

#include <chrono>
#include <iostream>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using namespace go2cpp;
using namespace go2cpp::time;

bool scenario_timeout_budget() {
    std::clog << "[full][场景1] 超时预算与截止判断\n";
    const Time deadline = Time::Now().Add(500 * Millisecond);
    const Duration budget = Until(deadline);
    const Time expired = Time::Now().Add(-Second);
    const bool ok = budget.Nanoseconds() > 0 && budget <= 500 * Millisecond &&
                    Until(expired).Nanoseconds() < 0 && expired.Before(Time::Now());
    std::cout << "  剩余预算=" << budget.String() << " 已过期=" << (Until(expired).Nanoseconds() < 0)
              << "（预期 ≤500ms 且 1）\n";
    return ok;
}

bool scenario_backoff() {
    std::clog << "[full][场景2] 重试退避序列（指数 + 封顶）\n";
    std::vector<std::string> delays;
    Duration delay = 100 * Millisecond;
    const Duration cap = 2 * Second;
    for (int attempt = 0; attempt < 6; ++attempt) {
        delays.push_back(delay.String());
        delay = delay * 2;
        if (delay > cap) delay = cap;
    }
    const bool ok = delays.size() == 6 && delays[0] == "100ms" && delays[1] == "200ms" &&
                    delays[2] == "400ms" && delays[3] == "800ms" && delays[4] == "1.6s" &&
                    delays[5] == "2s";
    std::cout << "  序列=" << Join(delays, ",")
              << "（预期 100ms,200ms,400ms,800ms,1.6s,2s）\n";
    return ok;
}

bool scenario_metric_window() {
    std::clog << "[full][场景3] 指标窗口边界（滑动窗口）\n";
    const Time window_start = Time::FromUnix(1700000000);
    const Time window_end = window_start.Add(2 * Minute);
    const Time inside = Time::FromUnix(1700000030);
    const Time outside = Time::FromUnix(1700000130);          // 2 分 10 秒后
    const bool ok = window_end.Sub(window_start).Nanoseconds() == (2 * Minute).Nanoseconds() &&
                    inside.After(window_start) && inside.Before(window_end) &&
                    outside.After(window_end) &&
                    window_start.Add(Minute).Before(window_end) &&
                    !window_end.Add(-Second).Equal(window_end);
    std::cout << "  窗口长度=" << window_end.Sub(window_start).String() << " 窗口内="
              << (inside.After(window_start) && inside.Before(window_end)) << " 窗口外="
              << (outside.After(window_end)) << "（预期 2m0s 1 1）\n";
    return ok;
}

bool scenario_log_timestamp() {
    std::clog << "[full][场景4] 日志时间戳格式化\n";
    const bool ok = Time::FromUnix(1700000000, 123000000).String() ==
                        "2023-11-14 22:13:20.123 +0000" &&
                    Time::FromUnix(0).String() == "1970-01-01 00:00:00 +0000" &&
                    Time::FromUnix(1700000000).String() == "2023-11-14 22:13:20 +0000" &&
                    Time::FromUnix(1, 1000).String() == "1970-01-01 00:00:01.000001 +0000" &&
                    Time{}.IsZero() && !Time::FromUnix(0).IsZero();
    std::cout << "  样例=" << Time::FromUnix(1700000000, 123000000).String()
              << " 零值判定=" << Time{}.IsZero() << " Unix0 非零=" << !Time::FromUnix(0).IsZero()
              << "（预期 2023-11-14 22:13:20.123 +0000 / 1 / 1）\n";
    return ok;
}

bool scenario_ticker_ticks() {
    std::clog << "[full][场景5] 周期任务节拍（零漂移累加）\n";
    const Time base = Time::FromUnix(1700000000);
    int drift_free = 0;
    for (int tick = 1; tick <= 1000; ++tick) {
        const Time at = base.Add(tick * 250 * Millisecond);
        if (at.Sub(base).Nanoseconds() == static_cast<std::int64_t>(tick) * 250000000LL) {
            ++drift_free;
        }
    }
    const bool ok = drift_free == 1000;
    std::cout << "  1000 拍无漂移=" << drift_free
              << " 第 1000 拍=" << base.Add(1000 * 250 * Millisecond).Sub(base).String()
              << "（预期 1000 / 4m10s）\n";
    return ok;
}

bool scenario_format_and_scale() {
    std::clog << "[full][场景6] Duration 格式化对照 + 10 万次规模\n";
    struct Sample {
        std::int64_t nanoseconds;
        const char* expected;
    };
    const Sample samples[] = {
        {0, "0s"},
        {1, "1ns"},
        {999, "999ns"},
        {1000, "1\xC2\xB5s"},
        {1500, "1.5\xC2\xB5s"},
        {1000000, "1ms"},
        {1500000, "1.5ms"},
        {1000000000, "1s"},
        {1500000000, "1.5s"},
        {1000000500, "1.0000005s"},
        {60000000000LL, "1m0s"},
        {90000000000LL, "1m30s"},
        {3600000000000LL, "1h0m0s"},
        {3661500000000LL, "1h1m1.5s"},
        {-1500000000, "-1.5s"},
    };
    bool table_ok = true;
    for (const Sample& sample : samples) {
        if (Duration::FromNanoseconds(sample.nanoseconds).String() != sample.expected) {
            table_ok = false;
        }
    }

    constexpr std::size_t kIterations = 100000;
    const auto started = Clock::now();
    std::size_t total = 0;
    for (std::size_t i = 0; i < kIterations; ++i) {
        const std::int64_t nanos = static_cast<std::int64_t>(i) * 12347 - 500000000LL;
        total += Duration::FromNanoseconds(nanos).String().size();
    }
    const auto elapsed_us =
        std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - started).count();

    const bool ok = table_ok && total > kIterations;
    std::cout << "  15 条对照全对=" << table_ok << " " << kIterations << " 次格式化总字符=" << total
              << " 耗时=" << elapsed_us << " µs（Debug 计时仅供参考）\n";
    return ok;
}

}  // namespace

/**
 * @brief time 全面版：六个场景。
 * @return 全部通过返回 0，否则返回 1。
 */
int main() {
    bool ok = true;
    ok = scenario_timeout_budget() && ok;
    ok = scenario_backoff() && ok;
    ok = scenario_metric_window() && ok;
    ok = scenario_log_timestamp() && ok;
    ok = scenario_ticker_ticks() && ok;
    ok = scenario_format_and_scale() && ok;

    std::clog << (ok ? "[full] 6 个场景全部通过\n" : "[full] 存在失败场景\n");
    return ok ? 0 : 1;
}
