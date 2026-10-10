#include "go2cpp/fiber_local.hpp"

#include "go2cpp/channel.hpp"
#include "go2cpp/scheduler.hpp"

#include "test_support.hpp"

#include <atomic>
#include <chrono>
#include <string>

namespace {

using namespace std::chrono_literals;

/** @brief 普通线程走 TLS fallback：创建、复用与 Reset。 */
void check_thread_fallback() {
    go2cpp::FiberLocal<int> slot;
    GO2CPP_REQUIRE(slot.TryGet() == nullptr);

    int& created = slot.GetOrCreate(7);
    GO2CPP_REQUIRE(created == 7);
    GO2CPP_REQUIRE(slot.TryGet() != nullptr);
    GO2CPP_CHECK(*slot.TryGet() == 7);

    *slot.TryGet() = 11;
    GO2CPP_CHECK(slot.GetOrCreate(99) == 11);

    slot.Reset();
    GO2CPP_CHECK(slot.TryGet() == nullptr);
}

/** @brief 每个 FiberLocalCache 实例分配独立键，互不覆盖。 */
void check_key_uniqueness() {
    go2cpp::FiberLocal<int> first;
    go2cpp::FiberLocal<int> second;
    go2cpp::FiberLocal<std::string> third;
    GO2CPP_CHECK(first.key_id() != second.key_id());
    GO2CPP_CHECK(second.key_id() != third.key_id());
    GO2CPP_CHECK(first.key_id() != third.key_id());
}

/** @brief 两个 Fiber 各自持有独立值，不互相覆盖，也不继承线程值。 */
void check_fiber_isolation() {
    go2cpp::FiberLocal<int> slot;
    go2cpp::Scheduler scheduler(2);
    scheduler.start();

    std::atomic<int> seen_a{-1};
    std::atomic<int> seen_b{-1};
    auto task_a = scheduler.spawn([&slot, &seen_a] {
        slot.GetOrCreate(0) = 100;
        seen_a.store(slot.GetOrCreate(-1), std::memory_order_release);
    });
    auto task_b = scheduler.spawn([&slot, &seen_b] {
        slot.GetOrCreate(0) = 200;
        seen_b.store(slot.GetOrCreate(-1), std::memory_order_release);
    });

    GO2CPP_REQUIRE(task_a->wait_for(5s));
    GO2CPP_REQUIRE(task_b->wait_for(5s));
    GO2CPP_CHECK(seen_a.load(std::memory_order_acquire) == 100);
    GO2CPP_CHECK(seen_b.load(std::memory_order_acquire) == 200);
    scheduler.shutdown();
}

/** @brief Fiber 因阻塞等待被 park 后，局部值仍然保持。 */
void check_value_survives_park() {
    go2cpp::FiberLocal<int> slot;
    go2cpp::Scheduler scheduler(1);
    scheduler.start();

    auto gate = go2cpp::MakeChannel<int>(0);
    std::atomic<int> before{-1};
    std::atomic<int> after{-1};

    auto worker = scheduler.spawn([&slot, &gate, &before, &after] {
        slot.GetOrCreate(0) = 55;
        before.store(slot.GetOrCreate(-1), std::memory_order_release);
        (void)go2cpp::Select({go2cpp::RecvCase(gate)}, {}, 5s);
        after.store(slot.GetOrCreate(-1), std::memory_order_release);
    });
    auto sender = scheduler.spawn([&gate] { (void)gate->Send(1); });

    GO2CPP_REQUIRE(worker->wait_for(5s));
    GO2CPP_REQUIRE(sender->wait_for(5s));
    GO2CPP_CHECK(before.load(std::memory_order_acquire) == 55);
    GO2CPP_CHECK(after.load(std::memory_order_acquire) == 55);
    scheduler.shutdown();
}

/** @brief Fiber 结束后其局部值被回收，新 Fiber 看不到旧值。 */
void check_cleanup_after_finish() {
    go2cpp::FiberLocal<int> slot;
    go2cpp::Scheduler scheduler(1);
    scheduler.start();

    auto first = scheduler.spawn([&slot] { slot.GetOrCreate(0) = 33; });
    GO2CPP_REQUIRE(first->wait_for(5s));

    std::atomic<bool> absent{false};
    auto second = scheduler.spawn([&slot, &absent] {
        absent.store(slot.TryGet() == nullptr, std::memory_order_release);
    });
    GO2CPP_REQUIRE(second->wait_for(5s));
    GO2CPP_CHECK(absent.load(std::memory_order_acquire));
    scheduler.shutdown();
}

}  // namespace

/**
 * @brief 验证 FiberLocalCache / FiberLocal 的生命周期与隔离语义。
 * @details 该设施此前只被 test_fiber 顺带覆盖（台账 T-07）。
 * @return 无；失败项由测试支持代码累计。
 */
void run_fiber_local_tests() {
    go2cpp_tests::announce("fiber local cache lifecycle");
    check_thread_fallback();
    check_key_uniqueness();
    check_fiber_isolation();
    check_value_survives_park();
    check_cleanup_after_finish();
}
