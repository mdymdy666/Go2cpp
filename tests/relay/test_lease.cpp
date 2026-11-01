/**
 * @file test_lease.cpp
 * @brief Lease 的功能、幂等、并发续约与随机化测试（REQ-005）。
 *
 * 覆盖：
 * - 到期只触发一次；触发时间不早于配置的 timeout；
 * - 中途 Renew 重置计时（原截止点不触发）；
 * - Stop / 析构取消；到期后 Stop、Renew 均为空操作；
 * - timeout <= 0 立即到期；
 * - 在 on_expire 里调用 Stop()（自停止）不死锁；
 * - 多线程并发 Renew 期间不过期，停止续约后只触发一次；
 * - 300 个租约各自只触发一次；100 个租约的随机续约序列。
 *
 * 90 秒真实用例在 test_lease_90s.cpp（慢用例，ctest 标签 long）；
 * 墙钟跳变用例在 test_lease_clockjump.cpp（LD_PRELOAD 注入）。
 */
#include "go2cpp/relay/lease.hpp"

#include "relay_test_util.hpp"
#include "test_support.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <random>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using go2cpp::relay::Lease;
using go2cpp::relay::test::SleepMs;
using go2cpp::relay::test::SteadyNowMs;

/** @brief 记录到期次数与首次触发时刻的计数器。 */
struct ExpiryCounter {
    std::atomic<int> count{0};
    std::atomic<std::int64_t> first_fire_ms{0};

    void OnExpire() {
        const int previous = count.fetch_add(1);
        if (previous == 0) {
            first_fire_ms.store(SteadyNowMs());
        }
    }
};

void test_expire_once() {
    ExpiryCounter counter;
    const std::int64_t start = SteadyNowMs();
    const Lease lease(150ms, [&counter] { counter.OnExpire(); });

    GO2CPP_REQUIRE_EVENTUALLY(counter.count.load() == 1, 5s);
    const std::int64_t elapsed = counter.first_fire_ms.load() - start;
    // 允许 20ms 的调度抖动，但不允许明显提前（提前说明用了错误的时钟/期限）。
    GO2CPP_CHECK(elapsed >= 130);
    GO2CPP_CHECK(elapsed < 3000);

    // 只触发一次：再等几个周期，计数不变。
    SleepMs(300);
    GO2CPP_CHECK(counter.count.load() == 1);
}

void test_renew_resets_deadline() {
    ExpiryCounter counter;
    Lease lease(600ms, [&counter] { counter.OnExpire(); });

    SleepMs(200);
    GO2CPP_CHECK(counter.count.load() == 0);
    const std::int64_t renew_at = SteadyNowMs();
    lease.Renew();
    // 距构造已经约 450ms（< 600ms），续约后应重新计时，不得提前触发。
    SleepMs(250);
    GO2CPP_CHECK(counter.count.load() == 0);

    GO2CPP_REQUIRE_EVENTUALLY(counter.count.load() == 1, 5s);
    const std::int64_t elapsed_since_renew = counter.first_fire_ms.load() - renew_at;
    GO2CPP_CHECK(elapsed_since_renew >= 550);
    GO2CPP_CHECK(elapsed_since_renew < 3000);
    SleepMs(200);
    GO2CPP_CHECK(counter.count.load() == 1);
}

void test_stop_before_expiry() {
    ExpiryCounter counter;
    Lease lease(100ms, [&counter] { counter.OnExpire(); });
    lease.Stop();
    SleepMs(300);
    GO2CPP_CHECK(counter.count.load() == 0);
    // Stop 幂等：再次调用不崩溃、不触发。
    lease.Stop();
    SleepMs(100);
    GO2CPP_CHECK(counter.count.load() == 0);
}

void test_stop_after_expiry() {
    ExpiryCounter counter;
    Lease lease(60ms, [&counter] { counter.OnExpire(); });
    GO2CPP_REQUIRE_EVENTUALLY(counter.count.load() == 1, 5s);
    lease.Stop();
    SleepMs(200);
    GO2CPP_CHECK(counter.count.load() == 1);
}

void test_renew_after_expiry_is_noop() {
    ExpiryCounter counter;
    Lease lease(60ms, [&counter] { counter.OnExpire(); });
    GO2CPP_REQUIRE_EVENTUALLY(counter.count.load() == 1, 5s);
    // 到期后 Renew 不能复活租约（否则「已经宣布死亡」的状态会被撤回）。
    lease.Renew();
    SleepMs(250);
    GO2CPP_CHECK(counter.count.load() == 1);
}

void test_destructor_cancels() {
    ExpiryCounter counter;
    {
        const Lease lease(100ms, [&counter] { counter.OnExpire(); });
    }
    SleepMs(300);
    GO2CPP_CHECK(counter.count.load() == 0);
}

void test_destructor_after_expiry_does_not_duplicate() {
    ExpiryCounter counter;
    {
        const Lease lease(50ms, [&counter] { counter.OnExpire(); });
        GO2CPP_REQUIRE_EVENTUALLY(counter.count.load() == 1, 5s);
    }
    SleepMs(100);
    GO2CPP_CHECK(counter.count.load() == 1);
}

void test_non_positive_timeout_expires_immediately() {
    ExpiryCounter zero_counter;
    const Lease zero(0ms, [&zero_counter] { zero_counter.OnExpire(); });
    GO2CPP_REQUIRE_EVENTUALLY(zero_counter.count.load() == 1, 5s);

    ExpiryCounter negative_counter;
    const Lease negative(-100ms, [&negative_counter] { negative_counter.OnExpire(); });
    GO2CPP_REQUIRE_EVENTUALLY(negative_counter.count.load() == 1, 5s);
}

void test_callback_can_stop_self() {
    std::atomic<int> fired{0};
    std::weak_ptr<Lease> weak;
    auto lease = std::make_shared<Lease>(50ms, [&fired, &weak] {
        fired.fetch_add(1);
        // 在计时线程的回调里调用 Stop()：实现检测到「当前就是计时线程」，
        // 不能自 join，必须 detach。
        if (const auto locked = weak.lock()) {
            locked->Stop();
        }
    });
    weak = lease;

    GO2CPP_REQUIRE_EVENTUALLY(fired.load() == 1, 5s);
    SleepMs(150);
    GO2CPP_CHECK(fired.load() == 1);
    lease.reset(); // 计时线程已 detach，析构不应等待/崩溃
}

void test_concurrent_renew_keeps_alive() {
    std::atomic<int> fired{0};
    Lease lease(300ms, [&fired] { fired.fetch_add(1); });
    std::atomic<bool> stop_renewing{false};

    std::vector<std::thread> renewers;
    for (int thread_index = 0; thread_index < 4; ++thread_index) {
        renewers.emplace_back([&lease, &stop_renewing] {
            while (!stop_renewing.load(std::memory_order_relaxed)) {
                lease.Renew();
                SleepMs(10);
            }
        });
    }
    // 2 倍 timeout 内持续续约：不允许过期。
    SleepMs(600);
    GO2CPP_CHECK(fired.load() == 0);
    stop_renewing.store(true, std::memory_order_relaxed);
    for (auto &thread : renewers) {
        thread.join();
    }
    // 停止续约后按最后一次 Renew 计时，只触发一次。
    GO2CPP_REQUIRE_EVENTUALLY(fired.load() == 1, 5s);
    SleepMs(300);
    GO2CPP_CHECK(fired.load() == 1);
}

void test_many_leases_fire_once() {
    constexpr int kLeases = 300;
    std::atomic<int> fired{0};
    std::vector<std::unique_ptr<Lease>> leases;
    leases.reserve(kLeases);
    for (int index = 0; index < kLeases; ++index) {
        leases.push_back(std::make_unique<Lease>(
            std::chrono::milliseconds(20 + (index % 10) * 5),
            [&fired] { fired.fetch_add(1); }));
    }
    GO2CPP_REQUIRE_EVENTUALLY(fired.load() == kLeases, 10s);
    SleepMs(300);
    GO2CPP_CHECK(fired.load() == kLeases);
}

/** @brief 随机续约序列：每个租约必须恰好触发一次，不能多也不能少。 */
void test_randomized_renew_and_expiry() {
    constexpr int kLeases = 100;
    std::atomic<int> fired{0};
    std::vector<std::shared_ptr<std::atomic<int>>> per_lease;
    std::vector<std::unique_ptr<Lease>> leases;
    leases.reserve(kLeases);
    per_lease.reserve(kLeases);
    std::mt19937_64 random(20261101);

    for (int index = 0; index < kLeases; ++index) {
        auto counter = std::make_shared<std::atomic<int>>(0);
        per_lease.push_back(counter);
        const auto timeout =
            std::chrono::milliseconds(20 + static_cast<int>(random() % 60));
        leases.push_back(std::make_unique<Lease>(timeout, [counter, &fired] {
            counter->fetch_add(1);
            fired.fetch_add(1);
        }));
    }

    // 4 个线程随机续约 250ms（不保证覆盖所有租约），随后全部停止续约。
    std::atomic<bool> stop_renewing{false};
    std::vector<std::thread> renewers;
    for (int thread_index = 0; thread_index < 4; ++thread_index) {
        renewers.emplace_back([&leases, &stop_renewing, thread_index] {
            std::mt19937_64 local_random(20261101 + static_cast<std::uint64_t>(thread_index));
            while (!stop_renewing.load(std::memory_order_relaxed)) {
                const auto which = static_cast<std::size_t>(local_random() % leases.size());
                leases[which]->Renew();
                SleepMs(1);
            }
        });
    }
    SleepMs(250);
    stop_renewing.store(true, std::memory_order_relaxed);
    for (auto &thread : renewers) {
        thread.join();
    }

    GO2CPP_REQUIRE_EVENTUALLY(fired.load() == kLeases, 10s);
    SleepMs(300);
    GO2CPP_CHECK(fired.load() == kLeases);
    for (const auto &counter : per_lease) {
        GO2CPP_CHECK(counter->load() == 1);
    }
}

} // namespace

int main() {
    go2cpp_tests::announce("relay.lease");

    test_expire_once();
    test_renew_resets_deadline();
    test_stop_before_expiry();
    test_stop_after_expiry();
    test_renew_after_expiry_is_noop();
    test_destructor_cancels();
    test_destructor_after_expiry_does_not_duplicate();
    test_non_positive_timeout_expires_immediately();
    test_callback_can_stop_self();
    test_concurrent_renew_keeps_alive();
    test_many_leases_fire_once();
    test_randomized_renew_and_expiry();

    const int failures = go2cpp_tests::g_failures.load();
    if (failures != 0) {
        std::fprintf(stderr, "relay_lease_test: %d checks failed\n", failures);
        return 1;
    }
    std::printf("relay_lease_test: all checks passed\n");
    return 0;
}
