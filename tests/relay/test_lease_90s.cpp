/**
 * @file test_lease_90s.cpp
 * @brief 验收用例：90s 不 Renew 恰好触发一次（真实等待，不缩放）。
 *
 * 慢用例（约 90 秒），ctest 标签为 `relay;long`，按需运行：
 * `ctest --test-dir .build-bento-wsl -R relay_lease_90s -L long`
 *
 * 断言：
 * - 89s 前不触发（留 1s 调度抖动余量）；
 * - 96s 内触发；
 * - 只触发一次（再等 1s 计数不变）。
 */
#include "go2cpp/relay/lease.hpp"

#include "relay_test_util.hpp"
#include "test_support.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>

namespace {

using namespace std::chrono_literals;
using go2cpp::relay::Lease;
using go2cpp::relay::test::SleepMs;
using go2cpp::relay::test::SteadyNowMs;

} // namespace

int main() {
    go2cpp_tests::announce("relay.lease.90s");

    std::atomic<int> fired{0};
    std::atomic<std::int64_t> fire_ms{0};
    const std::int64_t start = SteadyNowMs();
    const Lease lease(90000ms, [&fired, &fire_ms] {
        if (fired.fetch_add(1) == 0) {
            fire_ms.store(SteadyNowMs());
        }
    });

    // 每 250ms 采样一次，最多等 100s；期间若触发就退出等待。
    while (fired.load() == 0 && SteadyNowMs() - start < 100000) {
        SleepMs(250);
    }
    const std::int64_t elapsed = fire_ms.load() - start;

    GO2CPP_CHECK(fired.load() == 1);
    GO2CPP_CHECK(elapsed >= 89000); // 不允许明显提前到期
    GO2CPP_CHECK(elapsed <= 96000); // 不允许明显迟到
    SleepMs(1000);
    GO2CPP_CHECK(fired.load() == 1); // 只触发一次

    std::printf("relay_lease_90s_test: fired=%d elapsed=%lld ms（配置 90000 ms）\n",
                fired.load(), static_cast<long long>(elapsed));

    const int failures = go2cpp_tests::g_failures.load();
    if (failures != 0) {
        std::fprintf(stderr, "relay_lease_90s_test: %d checks failed\n", failures);
        return 1;
    }
    std::printf("relay_lease_90s_test: all checks passed\n");
    return 0;
}
