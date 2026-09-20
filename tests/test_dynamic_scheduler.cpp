#include "go2cpp/scheduler.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

class Watchdog {
public:
    explicit Watchdog(std::chrono::seconds timeout)
        : m_thread([this, timeout] {
              const auto deadline = std::chrono::steady_clock::now() + timeout;
              while (!m_done.load(std::memory_order_acquire)) {
                  if (std::chrono::steady_clock::now() >= deadline) {
                      std::abort();
                  }
                  std::this_thread::sleep_for(10ms);
              }
          }) {}

    ~Watchdog() {
        m_done.store(true, std::memory_order_release);
        GO2CPP_JOIN_WITH_WATCHDOG(m_thread, 3s);
    }

private:
    std::atomic<bool> m_done{false};
    std::thread m_thread;
};

bool WaitUntil(const std::function<bool()>& predicate,
               std::chrono::milliseconds timeout = 3s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    return predicate();
}

void Burst(go2cpp::Scheduler& scheduler, std::size_t worker_cap,
           std::atomic<int>& maximum_running) {
    constexpr int count = 96;
    std::atomic<bool> release{false};
    std::atomic<int> running{0};
    std::atomic<int> completed{0};
    std::vector<std::atomic<int>> seen(count);
    for (auto& item : seen) {
        item.store(0, std::memory_order_relaxed);
    }
    std::vector<std::shared_ptr<go2cpp::Task>> tasks;
    tasks.reserve(count);
    for (int index = 0; index < count; ++index) {
        tasks.emplace_back(scheduler.spawn([&, index] {
            seen[index].fetch_add(1, std::memory_order_relaxed);
            const auto active = running.fetch_add(1, std::memory_order_acq_rel) + 1;
            auto maximum = maximum_running.load(std::memory_order_acquire);
            while (maximum < active &&
                   !maximum_running.compare_exchange_weak(
                       maximum, active, std::memory_order_acq_rel)) {}
            const auto release_deadline =
                std::chrono::steady_clock::now() + 10s;
            while (!release.load(std::memory_order_acquire) &&
                   std::chrono::steady_clock::now() < release_deadline) {
                go2cpp_tests::yield_for_watchdog();
            }
            if (!release.load(std::memory_order_acquire)) {
                go2cpp_tests::watchdog_abort("burst release", __FILE__,
                                             __LINE__);
            }
            running.fetch_sub(1, std::memory_order_acq_rel);
            completed.fetch_add(1, std::memory_order_release);
        }, go2cpp::TaskOptions{0, static_cast<go2cpp::TaskClassId>(index % 3 + 1)}));
    }
    GO2CPP_CHECK(WaitUntil([&] {
        return scheduler.worker_count() == worker_cap;
    }));
    GO2CPP_CHECK(scheduler.worker_count() <= scheduler.processor_count());
    release.store(true, std::memory_order_release);
    GO2CPP_CHECK(WaitUntil([&] {
        return completed.load(std::memory_order_acquire) == count;
    }));
    for (int index = 0; index < count; ++index) {
        GO2CPP_CHECK(seen[index].load(std::memory_order_acquire) == 1);
        GO2CPP_CHECK(tasks[index]->wait_for(1s));
        GO2CPP_CHECK(tasks[index]->state() == go2cpp::GState::Dead);
    }
    GO2CPP_CHECK(scheduler.runnable_count() == 0);
}

void GrowShrinkRegrow() {
    go2cpp::SchedulerConfig config;
    config.processor_count = 4;
    config.min_workers = 1;
    config.max_workers = 4;
    config.idle_worker_timeout = 25ms;
    config.local_queue_limit = 7;
    go2cpp::Scheduler scheduler(config);
    scheduler.start();
    GO2CPP_CHECK(scheduler.worker_count() == 1);

    std::atomic<bool> snapshots_done{false};
    std::thread snapshots([&] {
        while (!snapshots_done.load(std::memory_order_acquire)) {
            for (const auto& machine : scheduler.machines()) {
                GO2CPP_CHECK(machine.processor < scheduler.processor_count());
            }
            for (const auto& processor : scheduler.processors()) {
                GO2CPP_CHECK(processor.id < scheduler.processor_count());
            }
            go2cpp_tests::yield_for_watchdog();
        }
    });

    std::atomic<int> maximum_running{0};
    for (int wave = 0; wave < 6; ++wave) {
        Burst(scheduler, 4, maximum_running);
        GO2CPP_CHECK(WaitUntil([&] { return scheduler.worker_count() == 1; }));
        // A later admission performs dead-M reaping. Drive that maintenance
        // point explicitly so the bound is checked after joins, including
        // under slow Memcheck scheduling.
        if (wave + 1 < 6) {
            auto maintenance = scheduler.spawn([] {});
            GO2CPP_CHECK(maintenance->wait_for(5s));
            GO2CPP_CHECK(WaitUntil(
                [&] { return scheduler.machines().size() <= 4; }, 5s));
        }
    }
    GO2CPP_CHECK(maximum_running.load(std::memory_order_acquire) <= 4);
    auto final_maintenance = scheduler.spawn([] {});
    GO2CPP_CHECK(final_maintenance->wait_for(5s));
    GO2CPP_CHECK(WaitUntil(
        [&] { return scheduler.machines().size() <= 4; }, 5s));
    snapshots_done.store(true, std::memory_order_release);
    GO2CPP_JOIN_WITH_WATCHDOG(snapshots, 5s);
    scheduler.shutdown();
    GO2CPP_CHECK(scheduler.worker_count() == 0);
}

void ProcessorCapAndMinimumFloor() {
    go2cpp::SchedulerConfig config;
    config.processor_count = 3;
    config.min_workers = 2;
    config.max_workers = 64;  // Strict mode keeps the effective bound at P.
    config.allow_worker_oversubscription = false;
    config.idle_worker_timeout = 3ms;
    go2cpp::Scheduler scheduler(config);
    scheduler.start();
    GO2CPP_CHECK(scheduler.worker_count() == 2);
    std::atomic<int> maximum_running{0};
    for (int wave = 0; wave < 8; ++wave) {
        Burst(scheduler, 3, maximum_running);
        GO2CPP_CHECK(WaitUntil([&] { return scheduler.worker_count() == 2; }));
        std::this_thread::sleep_for(5ms);
        GO2CPP_CHECK(scheduler.worker_count() == 2);
    }
    GO2CPP_CHECK(maximum_running.load(std::memory_order_acquire) <= 3);
    scheduler.shutdown();
}

void BlockingRegionOvercommit() {
    go2cpp::SchedulerConfig config;
    config.processor_count = 1;
    config.min_workers = 1;
    config.max_workers = 0;  // Normalized default reserves one replacement M.
    config.idle_worker_timeout = 10ms;
    config.allow_worker_oversubscription = true;
    go2cpp::Scheduler scheduler(config);
    scheduler.start();

    std::mutex native_mutex;
    std::condition_variable native_condition;
    bool release_blocked = false;
    std::atomic<bool> entered{false};
    std::atomic<int> peers_done{0};
    auto blocked = scheduler.spawn([&] {
        go2cpp::BlockingRegion region;
        GO2CPP_CHECK(region.active());
        entered.store(true, std::memory_order_release);
        std::unique_lock<std::mutex> lock(native_mutex);
        GO2CPP_REQUIRE(native_condition.wait_for(
            lock, 5s, [&] { return release_blocked; }));
    });
    GO2CPP_CHECK(WaitUntil([&] {
        return entered.load(std::memory_order_acquire);
    }));

    std::vector<std::shared_ptr<go2cpp::Task>> peers;
    for (int index = 0; index < 8; ++index) {
        peers.emplace_back(scheduler.spawn([&] {
            peers_done.fetch_add(1, std::memory_order_release);
        }));
    }
    GO2CPP_CHECK(WaitUntil([&] {
        return peers_done.load(std::memory_order_acquire) == 8;
    }));
    GO2CPP_CHECK(scheduler.worker_count() >= 2);
    GO2CPP_CHECK(scheduler.worker_count() <= 2);
    const auto machine_snapshot = scheduler.machines();
    const auto blocking_count = std::count_if(
        machine_snapshot.begin(), machine_snapshot.end(), [](const auto& machine) {
            return machine.state == go2cpp::MState::Blocking;
        });
    GO2CPP_CHECK(blocking_count == 1);

    {
        std::lock_guard<std::mutex> lock(native_mutex);
        release_blocked = true;
    }
    native_condition.notify_all();
    GO2CPP_CHECK(blocked->wait_for(2s));
    for (const auto& peer : peers) {
        GO2CPP_CHECK(peer->wait_for(2s));
    }
    GO2CPP_CHECK(WaitUntil([&] { return scheduler.worker_count() == 1; }));
    scheduler.shutdown();
}

void BoundedClassAffinity() {
    go2cpp::SchedulerConfig config;
    config.processor_count = 1;
    config.min_workers = 1;
    config.max_workers = 1;
    config.task_affinity_budget = 3;
    config.local_queue_limit = 0;  // One global queue makes order observable.
    go2cpp::Scheduler scheduler(config);
    std::mutex order_mutex;
    std::vector<int> order;
    std::vector<std::shared_ptr<go2cpp::Task>> tasks;
    for (int index = 0; index < 80; ++index) {
        const int task_class = index % 2 + 1;
        tasks.emplace_back(scheduler.spawn([&, task_class] {
            std::lock_guard<std::mutex> lock(order_mutex);
            order.push_back(task_class);
        }, go2cpp::TaskOptions{0, static_cast<go2cpp::TaskClassId>(task_class)}));
    }
    scheduler.start();
    for (const auto& task : tasks) {
        GO2CPP_CHECK(task->wait_for(2s));
    }
    GO2CPP_CHECK(order.size() == 80);
    if (order.size() == 80) {
        GO2CPP_CHECK(std::count(order.begin(), order.end(), 1) == 40);
        GO2CPP_CHECK(std::count(order.begin(), order.end(), 2) == 40);
        const auto first_foreign = std::find(order.begin(), order.end(), 2);
        GO2CPP_CHECK(first_foreign != order.end());
        GO2CPP_CHECK(first_foreign - order.begin() <= 4);
    }
    const auto machines = scheduler.machines();
    GO2CPP_CHECK(machines.size() == 1);
    if (machines.size() == 1) {
        GO2CPP_CHECK(machines.front().affinity_hits > 0);
        GO2CPP_CHECK(machines.front().affinity_misses > 0);
    }
    scheduler.shutdown();
}

void CancelWakeShutdownRace() {
    for (int round = 0; round < 40; ++round) {
        go2cpp::SchedulerConfig config;
        config.processor_count = 2;
        config.max_workers = 2;
        config.idle_worker_timeout = 5ms;
        go2cpp::Scheduler scheduler(config);
        scheduler.start();
        std::atomic<int> destructed{0};
        std::atomic<int> entered{0};
        auto task = scheduler.spawn([&] {
            struct StackGuard {
                std::atomic<int>* m_counter;
                ~StackGuard() { m_counter->fetch_add(1, std::memory_order_release); }
            } guard{&destructed};
            entered.fetch_add(1, std::memory_order_release);
            const auto cancellation_deadline =
                std::chrono::steady_clock::now() + 10s;
            while (!go2cpp::Scheduler::current_task()->cancellation_requested() &&
                   std::chrono::steady_clock::now() < cancellation_deadline) {
                (void)scheduler.park_current();
            }
            if (!go2cpp::Scheduler::current_task()->cancellation_requested()) {
                go2cpp_tests::watchdog_abort("cancel wake", __FILE__, __LINE__);
            }
        });
        GO2CPP_CHECK(WaitUntil([&] {
            return task->state() == go2cpp::GState::Waiting;
        }));
        std::thread notifier([&] { (void)scheduler.wake_or_cancel(task); });
        scheduler.shutdown();
        GO2CPP_JOIN_WITH_WATCHDOG(notifier, 5s);
        GO2CPP_CHECK(task->state() == go2cpp::GState::Cancelled);
        GO2CPP_CHECK(entered.load(std::memory_order_acquire) == 1);
        GO2CPP_CHECK(destructed.load(std::memory_order_acquire) == 1);
        GO2CPP_CHECK(scheduler.worker_count() == 0);
    }
}

}  // namespace

void run_dynamic_scheduler_tests() {
    Watchdog watchdog(20s);
    go2cpp_tests::announce("dynamic M growth/shrink, P cap, bounded affinity and wake drain");
    GrowShrinkRegrow();
    ProcessorCapAndMinimumFloor();
    BlockingRegionOvercommit();
    BoundedClassAffinity();
    CancelWakeShutdownRace();
}

#ifdef GO2CPP_DYNAMIC_SCHEDULER_MAIN
int main() {
    run_dynamic_scheduler_tests();
    return go2cpp_tests::g_failures.load(std::memory_order_relaxed) == 0 ? 0 : 1;
}
#endif
