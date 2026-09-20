#include "go2cpp/fiber.hpp"
#include "go2cpp/panic_defer.hpp"
#include "test_support.hpp"

#include <atomic>
#include <cerrno>
#include <condition_variable>
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
    Fiber migrating([&] {
        int preserved_local = 7;
        migrated_local = 1;
        first_fiber_tid = ::syscall(SYS_gettid);
        GO2CPP_CHECK(Fiber::Suspend(SuspendReason::Yield));
        GO2CPP_CHECK(preserved_local == 7);
        GO2CPP_CHECK(migrated_local == 1);
        migrated_local = 2;
        second_fiber_tid = ::syscall(SYS_gettid);
    });
    std::thread first([&] {
        first_thread = std::this_thread::get_id();
        GO2CPP_CHECK(migrating.resume());
        std::unique_lock<std::mutex> lock(migration_mutex);
        first_suspended = true;
        migration_cv.notify_all();
        migration_cv.wait(lock, [&] { return allow_first_exit; });
    });
    {
        std::unique_lock<std::mutex> lock(migration_mutex);
        migration_cv.wait(lock, [&] { return first_suspended; });
    }
    GO2CPP_CHECK(migrating.state() == FiberState::Suspended);
    std::thread second([&] {
        second_thread = std::this_thread::get_id();
        GO2CPP_CHECK(migrating.resume());
    });
    second.join();
    {
        std::lock_guard<std::mutex> lock(migration_mutex);
        allow_first_exit = true;
    }
    migration_cv.notify_all();
    first.join();
    GO2CPP_CHECK(migrating.state() == FiberState::Completed);
    GO2CPP_CHECK(migrated_local == 2);
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
        while (!release.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    });
    std::thread running([&] { GO2CPP_CHECK(exclusively_resumed.resume()); });
    while (!entered.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    GO2CPP_CHECK(!exclusively_resumed.resume());
    release.store(true, std::memory_order_release);
    running.join();
    GO2CPP_CHECK(exclusively_resumed.state() == FiberState::Completed);

    std::atomic<int> abandoned_destroyed{0};
    bool abandoned_continued = false;
    bool abandoned_cancelled = false;
    bool abandoned_deferred = false;
    {
        Fiber abandoned([&] {
            go2cpp::panic_defer::run([&] {
                go2cpp::panic_defer::Frame frame;
                frame.defer_call([&] { abandoned_deferred = true; });
                LifetimeProbe probe(abandoned_destroyed);
                Fiber::Suspend(SuspendReason::Park);
                abandoned_continued = true;
                abandoned_cancelled = Fiber::CancellationRequested();
            });
        });
        GO2CPP_CHECK(abandoned.resume());
        GO2CPP_CHECK(abandoned.state() == FiberState::Suspended);
    }
    GO2CPP_CHECK(abandoned_destroyed.load(std::memory_order_relaxed) == 1);
    GO2CPP_CHECK(abandoned_continued);
    GO2CPP_CHECK(abandoned_cancelled);
    GO2CPP_CHECK(abandoned_deferred);

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

    using namespace go2cpp::panic_defer;
    std::string recovered;
    bool panic_run_completed = false;
    Fiber panic_fiber([&] {
        panic_run_completed = run([&] {
            Frame frame;
            frame.defer_call([&] {
                const PanicValue value = recover();
                if (const auto* text = value.as_text()) {
                    recovered = *text;
                }
            });
            panic(PanicValue::text("fiber-local panic"));
            Fiber::Suspend(SuspendReason::Yield);
        });
    });
    GO2CPP_CHECK(panic_fiber.resume());
    GO2CPP_CHECK(!panicking());
    GO2CPP_CHECK(panic_fiber.resume());
    GO2CPP_CHECK(panic_fiber.state() == FiberState::Completed);
    GO2CPP_CHECK(panic_run_completed);
    GO2CPP_CHECK(recovered == "fiber-local panic");
    GO2CPP_CHECK(!panicking());

    ExecutionContext execution_context;
    std::thread bind_first([&] {
        Binding binding(execution_context);
        panic(PanicValue::text("migrated panic"));
        GO2CPP_CHECK(panicking());
    });
    bind_first.join();
    GO2CPP_CHECK(!panicking());

    std::thread bind_second([&] {
        GO2CPP_CHECK(!panicking());
        Binding binding(execution_context);
        const PanicValue value = current_panic();
        GO2CPP_CHECK(value.valid());
        GO2CPP_CHECK(value.as_text() != nullptr);
        GO2CPP_CHECK(*value.as_text() == "migrated panic");
    });
    bind_second.join();
    GO2CPP_CHECK(!panicking());
}
