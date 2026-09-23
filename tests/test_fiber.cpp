#include "go2cpp/fiber.hpp"
#include "go2cpp/fiber_local.hpp"
#include "go2cpp/scheduler.hpp"
#include "test_support.hpp"

#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <chrono>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <sys/syscall.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

struct LifetimeProbe {
    explicit LifetimeProbe(std::atomic<int>& destroyed) : m_destroyed(destroyed) {}
    ~LifetimeProbe() { m_destroyed.fetch_add(1, std::memory_order_relaxed); }

    std::atomic<int>& m_destroyed;
};

}  // namespace

void run_fiber_tests() {
    go2cpp_tests::announce("stackful fiber and execution context migration");
    using go2cpp::Fiber;
    using go2cpp::FiberState;
    using go2cpp::SuspendReason;
    using namespace std::chrono_literals;

    GO2CPP_CHECK(!Fiber::Suspend());
    GO2CPP_CHECK(!Fiber::CancellationRequested());

    std::atomic<int> unstarted_destroyed{0};
    bool unstarted_ran = false;
    {
        auto captured = std::make_shared<LifetimeProbe>(unstarted_destroyed);
        Fiber unstarted([&, captured] { unstarted_ran = true; });
        captured.reset();
    }
    GO2CPP_CHECK(!unstarted_ran);
    GO2CPP_CHECK(unstarted_destroyed.load(std::memory_order_relaxed) == 1);

    std::atomic<int> destroyed{0};
    std::vector<int> checkpoints;
    Fiber local_state([&] {
        LifetimeProbe probe(destroyed);
        int local = 40;
        errno = EAGAIN;
        checkpoints.push_back(++local);
        GO2CPP_CHECK(Fiber::Current() != nullptr);
        GO2CPP_CHECK(Fiber::Suspend(SuspendReason::Yield));
        GO2CPP_CHECK(errno == EAGAIN);
        checkpoints.push_back(++local);
        errno = ENOSPC;
        GO2CPP_CHECK(Fiber::Suspend(SuspendReason::Park));
        GO2CPP_CHECK(errno == ENOSPC);
        checkpoints.push_back(++local);
    });

    errno = EDOM;
    GO2CPP_CHECK(local_state.resume());
    GO2CPP_CHECK(errno == EDOM);
    GO2CPP_CHECK(local_state.state() == FiberState::Suspended);
    GO2CPP_CHECK(local_state.suspend_reason() == SuspendReason::Yield);
    GO2CPP_CHECK(destroyed.load(std::memory_order_relaxed) == 0);

    errno = ERANGE;
    GO2CPP_CHECK(local_state.resume());
    GO2CPP_CHECK(errno == ERANGE);
    GO2CPP_CHECK(local_state.state() == FiberState::Suspended);
    GO2CPP_CHECK(local_state.suspend_reason() == SuspendReason::Park);
    GO2CPP_CHECK(local_state.resume());
    GO2CPP_CHECK(local_state.state() == FiberState::Completed);
    GO2CPP_CHECK(!local_state.resume());
    GO2CPP_CHECK((checkpoints == std::vector<int>{41, 42, 43}));
    GO2CPP_CHECK(destroyed.load(std::memory_order_relaxed) == 1);

    std::mutex migration_mutex;
    std::condition_variable migration_cv;
    bool first_suspended = false;
    bool allow_first_exit = false;
    std::thread::id first_thread;
    std::thread::id second_thread;
    long first_fiber_tid = 0;
    long second_fiber_tid = 0;
    int migrated_local = 0;
    go2cpp::FiberLocalCache<int> migrated_slot;
    bool migrated_slot_preserved = false;
    Fiber migrating([&] {
        int preserved_local = 7;
        int& logical_local = migrated_slot.GetOrCreate(11);
        GO2CPP_CHECK(logical_local == 11);
        migrated_local = 1;
        first_fiber_tid = ::syscall(SYS_gettid);
        GO2CPP_CHECK(Fiber::Suspend(SuspendReason::Yield));
        GO2CPP_CHECK(preserved_local == 7);
        GO2CPP_CHECK(migrated_local == 1);
        GO2CPP_CHECK(migrated_slot.TryGet() != nullptr);
        migrated_slot_preserved = *migrated_slot.TryGet() == 11;
        migrated_local = 2;
        second_fiber_tid = ::syscall(SYS_gettid);
    });
    std::thread first([&] {
        first_thread = std::this_thread::get_id();
        GO2CPP_CHECK(migrating.resume());
        std::unique_lock<std::mutex> lock(migration_mutex);
        first_suspended = true;
        migration_cv.notify_all();
        GO2CPP_REQUIRE(migration_cv.wait_for(
            lock, 3s, [&] { return allow_first_exit; }));
    });
    {
        std::unique_lock<std::mutex> lock(migration_mutex);
        GO2CPP_REQUIRE(migration_cv.wait_for(
            lock, 3s, [&] { return first_suspended; }));
    }
    GO2CPP_CHECK(migrating.state() == FiberState::Suspended);
    std::thread second([&] {
        second_thread = std::this_thread::get_id();
        GO2CPP_CHECK(migrating.resume());
    });
    GO2CPP_JOIN_WITH_WATCHDOG(second, 3s);
    {
        std::lock_guard<std::mutex> lock(migration_mutex);
        allow_first_exit = true;
    }
    migration_cv.notify_all();
    GO2CPP_JOIN_WITH_WATCHDOG(first, 3s);
    GO2CPP_CHECK(migrating.state() == FiberState::Completed);
    GO2CPP_CHECK(migrated_local == 2);
    GO2CPP_CHECK(migrated_slot_preserved);
    GO2CPP_CHECK(migrated_slot.TryGet() == nullptr);
    GO2CPP_CHECK(first_thread != second_thread);
    GO2CPP_CHECK(first_fiber_tid != second_fiber_tid);

    Fiber failing([] { throw std::runtime_error("fiber failure"); });
    GO2CPP_CHECK(failing.resume());
    GO2CPP_CHECK(failing.state() == FiberState::Failed);
    GO2CPP_CHECK(failing.failure() != nullptr);

    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
    Fiber exclusively_resumed([&] {
        entered.store(true, std::memory_order_release);
        const auto release_deadline = std::chrono::steady_clock::now() + 5s;
        while (!release.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < release_deadline) {
            go2cpp_tests::yield_for_watchdog();
        }
        if (!release.load(std::memory_order_acquire)) {
            go2cpp_tests::watchdog_abort("fiber release", __FILE__, __LINE__);
        }
    });
    std::thread running([&] { GO2CPP_CHECK(exclusively_resumed.resume()); });
    GO2CPP_REQUIRE_EVENTUALLY(
        entered.load(std::memory_order_acquire), 3s);
    GO2CPP_CHECK(!exclusively_resumed.resume());
    release.store(true, std::memory_order_release);
    GO2CPP_JOIN_WITH_WATCHDOG(running, 3s);
    GO2CPP_CHECK(exclusively_resumed.state() == FiberState::Completed);

    std::atomic<int> abandoned_destroyed{0};
    bool abandoned_continued = false;
    bool abandoned_cancelled = false;
    {
        Fiber abandoned([&] {
            LifetimeProbe probe(abandoned_destroyed);
            Fiber::Suspend(SuspendReason::Park);
            abandoned_continued = true;
            abandoned_cancelled = Fiber::CancellationRequested();
        });
        GO2CPP_CHECK(abandoned.resume());
        GO2CPP_CHECK(abandoned.state() == FiberState::Suspended);
    }
    GO2CPP_CHECK(abandoned_destroyed.load(std::memory_order_relaxed) == 1);
    GO2CPP_CHECK(abandoned_continued);
    GO2CPP_CHECK(abandoned_cancelled);

    int cancellation_loop_iterations = 0;
    bool cancellation_loop_finished = false;
    {
        Fiber cancellation_loop([&] {
            while (!Fiber::CancellationRequested()) {
                ++cancellation_loop_iterations;
                Fiber::Suspend(SuspendReason::Park);
            }
            cancellation_loop_finished = true;
        });
        GO2CPP_CHECK(cancellation_loop.resume());
    }
    GO2CPP_CHECK(cancellation_loop_iterations == 1);
    GO2CPP_CHECK(cancellation_loop_finished);

    int finite_suspensions = 0;
    {
        Fiber finite_body([&] {
            for (int index = 0; index != 3; ++index) {
                ++finite_suspensions;
                Fiber::Suspend(SuspendReason::Yield);
            }
        });
        GO2CPP_CHECK(finite_body.resume());
    }
    GO2CPP_CHECK(finite_suspensions == 3);

    int nested_continuation = 0;
    Fiber nested_parent([&] {
        Fiber nested_child([&] {
            nested_continuation = 1;
            Fiber::Suspend(SuspendReason::Yield);
            nested_continuation = 2;
        });
        GO2CPP_CHECK(nested_child.resume());
        GO2CPP_CHECK(nested_continuation == 1);
        Fiber::Suspend(SuspendReason::Yield);
        GO2CPP_CHECK(nested_child.resume());
        GO2CPP_CHECK(nested_child.state() == FiberState::Completed);
    });
    GO2CPP_CHECK(nested_parent.resume());
    GO2CPP_CHECK(nested_parent.resume());
    GO2CPP_CHECK(nested_parent.state() == FiberState::Completed);
    GO2CPP_CHECK(nested_continuation == 2);

    // 子 Fiber 仍挂在父栈上时，如果用户在错误的调用方销毁它，
    // 运行时不能跳入已经不在当前执行链的父栈。此路径应放弃子上下文
    // 并标记失败，但不能 terminate；随后父 Fiber 仍应能正常收尾。
    std::unique_ptr<Fiber> detached_child;
    std::atomic<bool> detached_parent_suspended{false};
    Fiber detached_parent([&] {
        auto child = std::make_unique<Fiber>([] {
            GO2CPP_CHECK(Fiber::Suspend(SuspendReason::Park));
        });
        GO2CPP_CHECK(child->resume());
        GO2CPP_CHECK(child->state() == FiberState::Suspended);
        detached_child.reset(child.release());
        detached_parent_suspended.store(true, std::memory_order_release);
        GO2CPP_CHECK(Fiber::Suspend(SuspendReason::Yield));
    });
    GO2CPP_CHECK(detached_parent.resume());
    GO2CPP_REQUIRE_EVENTUALLY(
        detached_parent_suspended.load(std::memory_order_acquire), 1s);
    GO2CPP_REQUIRE(detached_child != nullptr);
    detached_child.reset();
    GO2CPP_CHECK(detached_parent.resume());
    GO2CPP_CHECK(detached_parent.state() == FiberState::Completed);

    // 错误父级析构也必须摘除 FiberLocal 值，不能因地址复用把旧 G 的
    // 值带给后续 Fiber。该 child 在挂起状态下从父栈逃逸，再由外部销毁。
    std::atomic<int> orphan_local_destroyed{0};
    go2cpp::FiberLocalCache<LifetimeProbe> orphan_local;
    std::unique_ptr<Fiber> orphan_child;
    std::atomic<bool> orphan_parent_suspended{false};
    Fiber orphan_parent([&] {
        auto child = std::make_unique<Fiber>([&] {
            orphan_local.GetOrCreate(orphan_local_destroyed);
            GO2CPP_CHECK(Fiber::Suspend(SuspendReason::Park));
        });
        GO2CPP_CHECK(child->resume());
        GO2CPP_CHECK(child->state() == FiberState::Suspended);
        orphan_child.reset(child.release());
        orphan_parent_suspended.store(true, std::memory_order_release);
        GO2CPP_CHECK(Fiber::Suspend(SuspendReason::Yield));
    });
    GO2CPP_CHECK(orphan_parent.resume());
    GO2CPP_REQUIRE_EVENTUALLY(
        orphan_parent_suspended.load(std::memory_order_acquire), 1s);
    GO2CPP_REQUIRE(orphan_child != nullptr);
    orphan_child.reset();
    GO2CPP_CHECK(orphan_local_destroyed.load(std::memory_order_acquire) == 1);
    GO2CPP_CHECK(orphan_parent.resume());
    GO2CPP_CHECK(orphan_parent.state() == FiberState::Completed);

    // 父对象结束后，子 Fiber 的诊断链仍由共享记录保留墓碑，不能
    // 因为访问快照而解引用已经销毁的父对象。子本身先完成，避免
    // 试图从不存在的父栈恢复。
    std::unique_ptr<Fiber> tombstone_child;
    {
        Fiber tombstone_parent([&] {
            tombstone_child = std::make_unique<Fiber>([] {});
            GO2CPP_CHECK(tombstone_child->resume());
            GO2CPP_CHECK(tombstone_child->state() == FiberState::Completed);
        });
        GO2CPP_CHECK(tombstone_parent.resume());
        GO2CPP_CHECK(tombstone_parent.state() == FiberState::Completed);
    }
    const auto tombstone_snapshot = tombstone_child->context_snapshot();
    GO2CPP_REQUIRE(tombstone_snapshot.size() >= 3);
    GO2CPP_CHECK(!tombstone_snapshot[1].alive);
    tombstone_child.reset();

    // 调度器挂起必须穿过完整的嵌套链。inner park 时，outer 和根 G
    // 都只保存 continuation；唤醒后应先回到 inner，再回到 outer，
    // 不能直接把 outer 的后续代码提前执行。
    go2cpp::SchedulerConfig nested_config;
    nested_config.processor_count = 1;
    nested_config.min_workers = 1;
    nested_config.max_workers = 1;
    nested_config.idle_wait = 1ms;
    go2cpp::Scheduler nested_scheduler(nested_config);
    nested_scheduler.start();
    std::atomic<int> nested_stage{0};
    std::atomic<bool> inner_snapshot_ok{false};
    std::shared_ptr<go2cpp::Task> nested_task;
    nested_task = nested_scheduler.spawn([&] {
        Fiber outer([&] {
            Fiber inner([&] {
                nested_stage.store(1, std::memory_order_release);
                const auto snapshot = Fiber::CurrentContextSnapshot();
                if (snapshot.size() >= 4 && snapshot.front().main_fiber &&
                    !snapshot.front().active && snapshot.back().active &&
                    snapshot.back().depth >= 3) {
                    inner_snapshot_ok.store(true, std::memory_order_release);
                }
                GO2CPP_CHECK(nested_scheduler.park_current());
                nested_stage.store(3, std::memory_order_release);
            });
            GO2CPP_CHECK(inner.resume());
            // 该断言在 inner park 期间不能执行；它只会在 inner 完成后
            // 继续，验证 scheduler 唤醒没有跳过父 Fiber continuation。
            GO2CPP_CHECK(nested_stage.load(std::memory_order_acquire) == 3);
            nested_stage.store(4, std::memory_order_release);
        });
        GO2CPP_CHECK(outer.resume());
        GO2CPP_CHECK(nested_stage.load(std::memory_order_acquire) == 4);
    });
    GO2CPP_REQUIRE_EVENTUALLY(
        nested_stage.load(std::memory_order_acquire) == 1, 2s);
    GO2CPP_REQUIRE_EVENTUALLY(
        nested_task->state() == go2cpp::GState::kWaiting, 2s);
    GO2CPP_CHECK(inner_snapshot_ok.load(std::memory_order_acquire));
    GO2CPP_CHECK(nested_scheduler.wake(nested_task));
    GO2CPP_CHECK(nested_task->wait_for(2s));
    GO2CPP_CHECK(nested_stage.load(std::memory_order_acquire) == 4);
    GO2CPP_CHECK(nested_task->state() == go2cpp::GState::kDead);
    nested_scheduler.shutdown();

    // 子 Fiber 的异常停在 Fiber 边界内，父 Fiber 用显式结果处理，
    // 不依赖 C++ 异常穿过调度器，也能保留父链快照。
    Fiber child_failure([&] { throw std::runtime_error("nested failure"); });
    bool parent_observed_failure = false;
    Fiber parent_failure([&] {
        const auto result = child_failure.resume_result();
        parent_observed_failure = result.accepted && result.failed() &&
                                  result.failure != nullptr &&
                                  result.context_snapshot.size() >= 3;
    });
    GO2CPP_CHECK(parent_failure.resume());
    GO2CPP_CHECK(parent_observed_failure);
    GO2CPP_CHECK(parent_failure.state() == FiberState::Completed);

    // FiberLocalCache follows the logical G across a migration, while an
    // ordinary thread gets an independent TLS fallback.
    go2cpp::FiberLocalCache<int> thread_local_value;
    thread_local_value.GetOrCreate() = 7;
    std::atomic<int> other_thread_value{0};
    std::thread fallback_thread([&] {
        GO2CPP_CHECK(thread_local_value.TryGet() == nullptr);
        other_thread_value.store(thread_local_value.GetOrCreate(9),
                                 std::memory_order_release);
    });
    GO2CPP_JOIN_WITH_WATCHDOG(fallback_thread, 3s);
    GO2CPP_CHECK(other_thread_value.load(std::memory_order_acquire) == 9);
    GO2CPP_CHECK(thread_local_value.TryGet() != nullptr);
    GO2CPP_CHECK(*thread_local_value.TryGet() == 7);
    thread_local_value.Reset();
    GO2CPP_CHECK(thread_local_value.TryGet() == nullptr);

    struct LocalLifetime {
        explicit LocalLifetime(std::atomic<int>* count) : m_count(count) {}
        ~LocalLifetime() { m_count->fetch_add(1, std::memory_order_release); }
        std::atomic<int>* m_count;
        int value{0};
    };
    go2cpp::FiberLocalCache<LocalLifetime> fiber_value;
    std::atomic<int> local_destroyed{0};
    go2cpp::Fiber local_cache_fiber([&] {
        auto& local = fiber_value.GetOrCreate(&local_destroyed);
        local.value = 42;
        GO2CPP_CHECK(fiber_value.TryGet() != nullptr);
        GO2CPP_CHECK(go2cpp::Fiber::Suspend(SuspendReason::Yield));
        GO2CPP_CHECK(fiber_value.TryGet()->value == 42);
    });
    GO2CPP_CHECK(local_cache_fiber.resume());
    GO2CPP_CHECK(local_destroyed.load(std::memory_order_acquire) == 0);
    GO2CPP_CHECK(local_cache_fiber.resume());
    GO2CPP_CHECK(local_cache_fiber.state() == FiberState::Completed);
    GO2CPP_CHECK(local_destroyed.load(std::memory_order_acquire) == 1);

    // The key object may have a shorter scope than its logical G.  Destroying
    // the wrapper must not invalidate a value that is still live in a Fiber;
    // the trampoline remains the sole owner of that value's cleanup.
    std::atomic<int> short_lived_destroyed{0};
    std::unique_ptr<go2cpp::FiberLocalCache<LocalLifetime>> short_lived_cache(
        new go2cpp::FiberLocalCache<LocalLifetime>());
    auto* const short_lived_key = short_lived_cache.get();
    Fiber short_lived_fiber([short_lived_key, &short_lived_destroyed] {
        short_lived_key->GetOrCreate(&short_lived_destroyed);
        GO2CPP_CHECK(Fiber::Suspend(SuspendReason::Yield));
    });
    GO2CPP_CHECK(short_lived_fiber.resume());
    short_lived_cache.reset();
    GO2CPP_CHECK(short_lived_destroyed.load(std::memory_order_acquire) == 0);
    GO2CPP_CHECK(short_lived_fiber.resume());
    GO2CPP_CHECK(short_lived_fiber.state() == FiberState::Completed);
    GO2CPP_CHECK(short_lived_destroyed.load(std::memory_order_acquire) == 1);
}
