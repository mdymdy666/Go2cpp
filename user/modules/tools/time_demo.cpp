/**
 * @file time_demo.cpp
 * @brief `go2cpp/time.hpp` 简单版：三个最常见的使用场景。
 *
 * @details 场景：
 *          ① 超时预算与截止判断（`Until` + `Before`）
 *          ② 重试退避序列（`Duration` 运算 + `String`）
 *          ③ 耗时统计（`Since` + `Sleep`，体现协作式睡眠）
 * @return 全部校验通过返回 0，否则返回 1。
 */
#include "go2cpp/runtime.hpp"
#include "go2cpp/strings.hpp"
#include "go2cpp/time.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace {

using namespace go2cpp;
using namespace go2cpp::time;

/// 场景 1：超时预算与截止判断。
bool scenario_timeout_budget() {
    std::clog << "[time][场景1] 超时预算与截止判断\n";
    const Time deadline = Time::Now().Add(500 * Millisecond);
    const Duration budget = Until(deadline);
    const bool pending = budget.Nanoseconds() > 0 && budget <= 500 * Millisecond;
    const Time expired = Time::Now().Add(-Second);
    const bool is_expired = Until(expired).Nanoseconds() < 0 && expired.Before(Time::Now());
    const bool ok = pending && is_expired;
    std::cout << "  剩余预算=" << budget.String() << "（≤500ms）未过期=" << pending
              << " 已过期判定=" << is_expired << "（预期 1 1）\n";
    return ok;
}

/// 场景 2：重试退避序列。
bool scenario_backoff() {
    std::clog << "[time][场景2] 重试退避序列（指数 + 封顶）\n";
    std::vector<std::string> delays;
    Duration delay = 100 * Millisecond;
    const Duration cap = 2 * Second;
    for (int attempt = 0; attempt < 6; ++attempt) {
        delays.push_back(delay.String());
        delay = delay * 2;
        if (delay > cap) delay = cap;
    }
    const std::string sequence = Join(delays, ",");
    const bool ok = delays.size() == 6 && delays[0] == "100ms" && delays[4] == "1.6s" &&
                    delays[5] == "2s";
    std::cout << "  序列=" << sequence << "（预期 100ms,200ms,400ms,800ms,1.6s,2s）\n";
    return ok;
}

/// 场景 3：耗时统计（Sleep 在 Fiber/Hook 下是协作式等待）。
bool scenario_elapsed() {
    std::clog << "[time][场景3] 耗时统计与协作式睡眠\n";
    const Time start = Time::Now();
    Sleep(2 * Millisecond);
    const Duration elapsed = Since(start);
    const bool ok = elapsed.Nanoseconds() >= 1000000LL && elapsed.Nanoseconds() < 1000000000LL;
    std::cout << "  经过=" << elapsed.String() << "（预期 ≥1ms 且 <1s，实际受调度影响）\n";
    return ok;
}

}  // namespace

/**
 * @brief time 简单版：三个场景覆盖截止、退避与耗时。
 * @return 全部通过返回 0，否则返回 1。
 */
int main() {
    bool ok = true;
    ok = scenario_timeout_budget() && ok;
    ok = scenario_backoff() && ok;
    ok = scenario_elapsed() && ok;

    std::clog << (ok ? "[time] 全部通过\n" : "[time] 有失败\n");
    return ok ? 0 : 1;
}
