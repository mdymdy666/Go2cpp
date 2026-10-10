#include "go2cpp/thread_policy.hpp"

#include "go2cpp/scheduler.hpp"

#include "test_support.hpp"

#include <atomic>
#include <chrono>

namespace {

using namespace std::chrono_literals;

/** @brief 默认状态下当前线程不参与 GMP，也未绑定调度器。 */
void check_default_state() {
    const auto policy = go2cpp::CurrentThreadPolicy();
    GO2CPP_CHECK(policy.scheduler == nullptr);
    GO2CPP_CHECK(policy.participation ==
                 go2cpp::ThreadParticipationMode::kUnmanaged);
    GO2CPP_CHECK(!policy.runtime_worker);
    GO2CPP_CHECK(!policy.participates_in_gmp());
    GO2CPP_CHECK(!go2cpp::ThreadParticipatesInGMP());
}

/** @brief 参与作用域在析构后恢复进入前的 TLS 状态。 */
void check_participation_scope() {
    go2cpp::Scheduler scheduler(2);
    scheduler.start();
    {
        go2cpp::ScopedThreadParticipation scope(scheduler);
        const auto inside = go2cpp::CurrentThreadPolicy();
        GO2CPP_CHECK(inside.scheduler == &scheduler);
        GO2CPP_CHECK(inside.participation ==
                     go2cpp::ThreadParticipationMode::kGmpEligible);
        GO2CPP_CHECK(inside.participates_in_gmp());
        GO2CPP_CHECK(go2cpp::ThreadParticipatesInGMP());
    }
    const auto after = go2cpp::CurrentThreadPolicy();
    GO2CPP_CHECK(after.scheduler == nullptr);
    GO2CPP_CHECK(after.participation ==
                 go2cpp::ThreadParticipationMode::kUnmanaged);
    GO2CPP_CHECK(!go2cpp::ThreadParticipatesInGMP());
    scheduler.shutdown();
}

/** @brief Hook 模式作用域保存并恢复旧值。 */
void check_hook_mode_scope() {
    const auto baseline = go2cpp::CurrentThreadHookMode();
    {
        go2cpp::ScopedThreadHookMode disabled(go2cpp::ThreadHookMode::kDisabled);
        GO2CPP_CHECK(go2cpp::CurrentThreadHookMode() ==
                     go2cpp::ThreadHookMode::kDisabled);
    }
    GO2CPP_CHECK(go2cpp::CurrentThreadHookMode() == baseline);

    go2cpp::SetCurrentThreadHookMode(go2cpp::ThreadHookMode::kEnabled);
    GO2CPP_CHECK(go2cpp::CurrentThreadHookMode() ==
                 go2cpp::ThreadHookMode::kEnabled);
    go2cpp::SetCurrentThreadHookMode(baseline);
    GO2CPP_CHECK(go2cpp::CurrentThreadHookMode() == baseline);
}

/** @brief 调度器 worker 线程上的快照应标记 runtime_worker。 */
void check_runtime_worker_flag() {
    go2cpp::Scheduler scheduler(1);
    scheduler.start();
    std::atomic<bool> worker_flag{false};
    std::atomic<bool> ran{false};
    auto task = scheduler.spawn([&worker_flag, &ran] {
        worker_flag.store(go2cpp::CurrentThreadPolicy().runtime_worker,
                          std::memory_order_release);
        ran.store(true, std::memory_order_release);
    });
    GO2CPP_REQUIRE(task->wait_for(5s));
    GO2CPP_REQUIRE(ran.load(std::memory_order_acquire));
    GO2CPP_CHECK(worker_flag.load(std::memory_order_acquire));
    scheduler.shutdown();
}

}  // namespace

/**
 * @brief 验证线程参与与 Hook 策略作用域的行为与恢复语义。
 * @details 这两个作用域此前仅有间接覆盖（台账 T-07）；它们只写 TLS 元数据，
 *          不会把外部线程附加成 worker。
 * @return 无；失败项由测试支持代码累计。
 */
void run_thread_policy_tests() {
    go2cpp_tests::announce("thread participation and hook policy");
    check_default_state();
    check_participation_scope();
    check_hook_mode_scope();
    check_runtime_worker_flag();
}
