#include "go2cpp/fiber.hpp"
#include "go2cpp/fiber_local.hpp"
#include "go2cpp/scheduler.hpp"
#include "test_support.hpp"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <condition_variable>
#include <chrono>
#include <limits>
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

/// @brief 读取 /proc/self/status 的 VmSize（KiB）。
/// @note 栈 arena 的保留/回收是进程级行为，只能通过 /proc 观测；Linux
///       测试环境可用，读不到时返回 0（相关断言会失败并暴露问题）。
std::size_t ReadVmSizeKiB() {
    FILE* status = std::fopen("/proc/self/status", "r");
    if (status == nullptr) {
        return 0;
    }
    char line[256];
    std::size_t kib = 0;
    while (std::fgets(line, sizeof(line), status) != nullptr) {
        if (std::strncmp(line, "VmSize:", 7) == 0) {
            (void)std::sscanf(line + 7, "%zu", &kib);
            break;
        }
    }
    std::fclose(status);
    return kib;
}

/// @brief 每层约 256 字节 volatile 局部的深递归（验证几何增长的栈增长）。
__attribute__((noinline)) std::uint64_t DeepWalk(std::size_t depth) {
    volatile char pad[256];
    pad[0] = static_cast<char>(depth & 0x7F);
    pad[255] = static_cast<char>((depth >> 7) & 0x7F);
    if (depth == 0) {
        return static_cast<std::uint64_t>(
            static_cast<unsigned char>(pad[0]) +
            static_cast<unsigned char>(pad[255]));
    }
    const std::uint64_t below = DeepWalk(depth - 1);
    return below + static_cast<std::uint64_t>(
                       static_cast<unsigned char>(pad[0]) +
                       static_cast<unsigned char>(pad[255]));
}

std::uint64_t ExpectedDeepWalk(std::size_t depth) {
    std::uint64_t sum = 0;
    for (std::size_t d = 0; d <= depth; ++d) {
        sum += static_cast<unsigned char>(d & 0x7F);
        sum += static_cast<unsigned char>((d >> 7) & 0x7F);
    }
    return sum;
}

}  // namespace

/**
 * @brief 验证 Fiber 的挂起恢复、迁移、嵌套栈、取消和本地缓存。
 * @return 无；测试失败由统一断言统计。
 */
void run_fiber_tests() {
    go2cpp_tests::announce("stackful fiber and execution context migration");
    using go2cpp::Fiber;
    using go2cpp::FiberState;
    using go2cpp::SuspendReason;
    using namespace std::chrono_literals;

    GO2CPP_CHECK(!Fiber::Suspend());
    GO2CPP_CHECK(!Fiber::CancellationRequested());

#if defined(__linux__) && defined(__x86_64__)
    // 可变长栈的初始提交量下限是 1 页（2048 请求归一化为 4096）；
    // 固定栈回退仍会在映射末尾额外保留 1 页 guard。映射加法必须在构造
    // 阶段拒绝溢出，不能等到首次 resume 才破坏上下文。
    const long raw_page_size = ::sysconf(_SC_PAGESIZE);
    if (raw_page_size > 0) {
        const auto page_size = static_cast<std::size_t>(raw_page_size);
        Fiber tiny_stack([] {}, 1U);
        // 1 字节请求归一化为恰好 1 页；这是比原来的 >= 2 页更精确的新语义。
        GO2CPP_CHECK(tiny_stack.stack_size() == page_size);
        GO2CPP_CHECK(tiny_stack.stack_size() % page_size == 0);

        bool rejected_overflow = false;
        try {
            Fiber impossible_stack([] {},
                                   std::numeric_limits<std::size_t>::max());
        } catch (const std::length_error&) {
            rejected_overflow = true;
        }
        GO2CPP_CHECK(rejected_overflow);
    }
#endif

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
#if defined(__SANITIZE_ADDRESS__)
    // ASan 下 fiber 实现按设计禁用增长栈（fiber.cpp 的 resolve_reserve），
    // Scheduler 默认 2KiB 初始提交归一化后只有 1 页固定栈，容不下插桩后的
    // scheduler/测试栈帧（实测 stack-overflow 位于 initialize_context）。
    // 这里只放大本测试调度器 G 的初始栈，不改变被测语义；非 ASan 构建
    // 仍走默认的 2KiB 初始提交 + 增长栈。
    nested_config.fiber_stack_size = 512U * 1024U;
#endif
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

    // ------------------------------------------------------------------
    // FIBER-01/02/03 回归：诊断接口与生命周期并发
    // ------------------------------------------------------------------

    // FIBER-01：诊断线程并发读 debug_stack，主线程让 Fiber 完成（完成路径
    // 会 reset m_stack）。修复后非当前 Fiber 只复制 FiberRecord 元数据，
    // 必须返回 has_stack=false 且绝不崩溃。
    {
        std::vector<std::unique_ptr<Fiber>> race_fibers;
        race_fibers.reserve(16);
        for (int index = 0; index < 16; ++index) {
            race_fibers.push_back(std::make_unique<Fiber>(
                [] { GO2CPP_CHECK(Fiber::Suspend(SuspendReason::Yield)); }));
            GO2CPP_CHECK(race_fibers.back()->resume());
        }
        std::atomic<bool> reader_started{false};
        std::atomic<bool> reader_stop{false};
        std::atomic<bool> saw_stack_fields{false};
        std::atomic<std::uint64_t> snapshot_count{0};
        std::thread reader([&] {
            reader_started.store(true, std::memory_order_release);
            while (!reader_stop.load(std::memory_order_acquire)) {
                for (auto& fiber : race_fibers) {
                    const auto snapshot = fiber->debug_stack();
                    if (snapshot.has_stack || snapshot.mapping_base != 0 ||
                        snapshot.mapping_top != 0 ||
                        snapshot.committed_begin != 0 ||
                        snapshot.current_sp != 0) {
                        saw_stack_fields.store(true,
                                               std::memory_order_release);
                    }
                    snapshot_count.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
        GO2CPP_REQUIRE_EVENTUALLY(
            reader_started.load(std::memory_order_acquire), 2s);
        for (auto& fiber : race_fibers) {
            GO2CPP_CHECK(fiber->resume());
            GO2CPP_CHECK(fiber->state() == FiberState::Completed);
        }
        std::this_thread::sleep_for(10ms);
        reader_stop.store(true, std::memory_order_release);
        GO2CPP_JOIN_WITH_WATCHDOG(reader, 5s);
        GO2CPP_CHECK(!saw_stack_fields.load(std::memory_order_acquire));
        GO2CPP_CHECK(snapshot_count.load(std::memory_order_relaxed) > 0);
    }

    // FIBER-02：另一个线程仍在 Fiber 内执行时析构。修复后析构等待有界
    // （约 200ms 预算），超时后放弃所有权而不是无限 yield() 烧核；计数
    // 必须递增，被隔离的 Impl 让 resume 线程随后安全返回。
    {
        const std::uint64_t timeouts_before = Fiber::DestroyTimeoutCount();
        std::atomic<bool> entered{false};
        std::atomic<bool> release_spinner{false};
        std::atomic<bool> resume_returned{false};
        Fiber* raw_fiber = new Fiber([&] {
            entered.store(true, std::memory_order_release);
            while (!release_spinner.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
        });
        std::thread runner([&raw_fiber, &resume_returned] {
            raw_fiber->resume();
            resume_returned.store(true, std::memory_order_release);
        });
        GO2CPP_REQUIRE_EVENTUALLY(entered.load(std::memory_order_acquire), 2s);
        const auto destroy_started = std::chrono::steady_clock::now();
        delete raw_fiber;
        const auto destroy_elapsed =
            std::chrono::steady_clock::now() - destroy_started;
        GO2CPP_CHECK(Fiber::DestroyTimeoutCount() > timeouts_before);
        GO2CPP_CHECK(destroy_elapsed < 3s);
        release_spinner.store(true, std::memory_order_release);
        GO2CPP_JOIN_WITH_WATCHDOG(runner, 5s);
        GO2CPP_CHECK(resume_returned.load(std::memory_order_acquire));
        GO2CPP_CHECK(Fiber::DestroyTimeoutCount() == timeouts_before + 1U);
    }

    // FIBER-03：外部线程并发读状态/请求取消，同时 FiberBin 路径反复复用
    // 同一个 Fiber（reset record）。修复后 record 原地重置，不再替换
    // shared_ptr，读到的始终是有效元数据。
    //
    // 生命周期协议（测试侧修复 P1-1）：Fiber 的唯一 owner 是
    // unique_ptr，observer 不能跨越 RecycleForScheduler 持有裸指针。
    // 回收契约规定 record 的 use_count()!=1 时静默丢弃该 Fiber（见
    // fiber.cpp 的 RecycleForScheduler），unique_ptr 随即析构对象；若
    // observer 手里还留着裸指针，下一次解引用就是 heap-use-after-free
    // （ASan 实测）。因此这里用 publish_mutex 串行化「发布/清空指针」与
    // 「一轮完整读取」：主线程清空指针并重新拿到锁之后，observer 不可能
    // 仍在使用旧指针，回收时 use_count 必然为 1，FiberBin 复用路径依然
    // 被覆盖。指针只在锁内可见，不参与锁外的生命周期判断。
    {
        std::atomic<bool> observer_stop{false};
        std::atomic<bool> invalid_state{false};
        // P2-3：此前 200 轮可能早于 observer 的首次调度，observer_reads
        // 保持 0 导致假失败（valgrind 下已复现）。计数用 release/acquire
        // 发布，并在首轮显式等待一次读取完成。
        std::atomic<std::uint64_t> observer_reads{0};
        std::mutex publish_mutex;
        Fiber* published = nullptr;  // 受 publish_mutex 保护，不跨越回收点。
        std::thread observer([&] {
            while (!observer_stop.load(std::memory_order_acquire)) {
                bool read_this_round = false;
                {
                    std::lock_guard<std::mutex> lock(publish_mutex);
                    Fiber* const fiber = published;
                    if (fiber != nullptr) {
                        const FiberState state = fiber->state();
                        if (state != FiberState::Ready &&
                            state != FiberState::Running &&
                            state != FiberState::Suspended &&
                            state != FiberState::Completed &&
                            state != FiberState::Failed) {
                            invalid_state.store(true, std::memory_order_release);
                        }
                        (void)fiber->suspend_reason();
                        (void)fiber->debug_info();
                        (void)fiber->debug_stack();
                        (void)fiber->execution_binding();
                        (void)fiber->nesting_depth();
                        (void)fiber->failure();
                        fiber->RequestCancellation();
                        observer_reads.fetch_add(1,
                                                std::memory_order_release);
                        read_this_round = true;
                    }
                }
                if (!read_this_round) {
                    std::this_thread::yield();
                }
            }
        });
        for (int round = 0; round < 200; ++round) {
            auto fiber = Fiber::AcquireForScheduler(
                [] {}, Fiber::DefaultStackSize(), 8U);
            GO2CPP_REQUIRE(fiber != nullptr);
            {
                std::lock_guard<std::mutex> lock(publish_mutex);
                published = fiber.get();
            }
            GO2CPP_CHECK(fiber->resume());
            GO2CPP_CHECK(fiber->state() == FiberState::Completed);
            if (round == 0) {
                // 首轮等 observer 完整读一次再回收：这一轮之后每轮都有
                // 真实并发读取，observer_reads>0 的断言也不再赌调度时序。
                GO2CPP_REQUIRE_EVENTUALLY(
                    observer_reads.load(std::memory_order_acquire) > 0, 5s);
            }
            {
                // 拿到锁即代表 observer 已完成（或尚未开始）对旧指针的
                // 访问；此后回收不会与任何 observer 读取重叠。
                std::lock_guard<std::mutex> lock(publish_mutex);
                published = nullptr;
            }
            Fiber::RecycleForScheduler(std::move(fiber), 8U);
        }
        observer_stop.store(true, std::memory_order_release);
        GO2CPP_JOIN_WITH_WATCHDOG(observer, 10s);
        GO2CPP_CHECK(!invalid_state.load(std::memory_order_acquire));
        GO2CPP_CHECK(observer_reads.load(std::memory_order_acquire) > 0);
    }

    // ---- 栈 arena：批量创建/完成不泄漏虚拟地址，重复批次复用槽 ----------
    {
        constexpr std::size_t kWaveCount = 256;
        const std::size_t before_kib = ReadVmSizeKiB();
        std::size_t first_peak_kib = 0;
        for (int wave = 0; wave < 3; ++wave) {
            std::vector<std::unique_ptr<Fiber>> fibers;
            fibers.reserve(kWaveCount);
            for (std::size_t i = 0; i < kWaveCount; ++i) {
                fibers.push_back(std::make_unique<Fiber>(
                    [] { Fiber::Suspend(SuspendReason::Park); }, 4096));
                GO2CPP_REQUIRE(fibers.back() != nullptr);
                GO2CPP_CHECK(fibers.back()->resume());
            }
            const std::size_t peak_kib = ReadVmSizeKiB();
            // 256 个 8MiB 预留 ≈ 2GiB 虚拟地址；arena 不应少给。
            GO2CPP_CHECK(peak_kib >= before_kib + 1024U * 1024U);
            if (wave == 0) {
                first_peak_kib = peak_kib;
            } else {
                // 后续批次应复用已保留的槽，峰值不随批次线性增长。
                GO2CPP_CHECK(peak_kib <= first_peak_kib + 512U * 1024U);
            }
            for (auto& fiber : fibers) {
                GO2CPP_CHECK(fiber->resume());
                GO2CPP_CHECK(fiber->state() == FiberState::Completed);
            }
            fibers.clear();
            const std::size_t after_kib = ReadVmSizeKiB();
            GO2CPP_CHECK(after_kib <= peak_kib);
            // 完成后只保留有界的空闲 arena（默认 8MiB 槽 ≤ 64 个 ≈ 512MiB），
            // 其余整体 munmap；旧实现会留下约 544×8MiB 的逐栈缓存。
            GO2CPP_CHECK(after_kib <= before_kib + 1024U * 1024U);
        }
    }

    // ---- 几何增长：深层递归（约 3.5MiB）在 16MiB 预留内完成 ----------------
    {
        constexpr std::size_t kDepth = 12000;
        std::atomic<bool> checksum_ok{false};
        Fiber deep(
            [&checksum_ok] {
                const std::uint64_t got = DeepWalk(kDepth);
                checksum_ok.store(got == ExpectedDeepWalk(kDepth),
                                  std::memory_order_release);
            },
            64U * 1024U, 16U * 1024U * 1024U, true);
        GO2CPP_CHECK(deep.resume());
        while (deep.state() == FiberState::Suspended) {
            GO2CPP_CHECK(deep.resume());
        }
        GO2CPP_CHECK(deep.state() == FiberState::Completed);
        GO2CPP_CHECK(checksum_ok.load(std::memory_order_acquire));
    }

    // ---- 多大小类：不同 reserve 各自成类，互不串用 ------------------------
    {
        std::atomic<int> completed{0};
        std::vector<std::unique_ptr<Fiber>> fibers;
        for (std::size_t mib = 1; mib <= 10; ++mib) {
            fibers.push_back(std::make_unique<Fiber>(
                [&completed] {
                    completed.fetch_add(1, std::memory_order_relaxed);
                    Fiber::Suspend(SuspendReason::Yield);
                    completed.fetch_add(1, std::memory_order_relaxed);
                },
                4096, mib * 1024U * 1024U, true));
        }
        for (auto& fiber : fibers) {
            GO2CPP_CHECK(fiber->resume());
        }
        for (auto& fiber : fibers) {
            GO2CPP_CHECK(fiber->resume());
            GO2CPP_CHECK(fiber->state() == FiberState::Completed);
        }
        GO2CPP_CHECK(completed.load(std::memory_order_relaxed) == 20);
    }

    // ---- 同一 reserve、不同初始窗口：arena 类必须按 (reserve, usable) 匹配 ----
    {
        std::atomic<int> mixed{0};
        {
            Fiber small([&mixed] { mixed.fetch_add(1, std::memory_order_relaxed); },
                        4096, 8U * 1024U * 1024U, true);
            GO2CPP_CHECK(small.resume());
            GO2CPP_CHECK(small.state() == FiberState::Completed);
        }
        {
            // 若复用上面那个 4KiB 窗口的槽，初始上下文会落在 PROT_NONE 区域
            // 并触发不可增长的 SIGSEGV；这里一旦串用就是硬崩。
            Fiber large([&mixed] { mixed.fetch_add(1, std::memory_order_relaxed); },
                        128U * 1024U, 8U * 1024U * 1024U, true);
            GO2CPP_CHECK(large.resume());
            GO2CPP_CHECK(large.state() == FiberState::Completed);
        }
        GO2CPP_CHECK(mixed.load(std::memory_order_relaxed) == 2);
    }

    // ---- 增长过的槽复用：committed 必须复位到初始窗口 ----------------------
    {
        std::atomic<std::size_t> usable_before{0};
        std::atomic<std::size_t> usable_after{0};
        {
            Fiber deep(
                [&usable_before] {
                    usable_before.store(
                        Fiber::Current()->debug_stack().usable_bytes,
                        std::memory_order_relaxed);
                    volatile std::uint64_t sink = DeepWalk(6000);
                    (void)sink;
                },
                64U * 1024U, 16U * 1024U * 1024U, true);
            GO2CPP_CHECK(deep.resume());
            GO2CPP_CHECK(deep.state() == FiberState::Completed);
        }
        {
            Fiber next(
                [&usable_after] {
                    usable_after.store(
                        Fiber::Current()->debug_stack().usable_bytes,
                        std::memory_order_relaxed);
                },
                64U * 1024U, 16U * 1024U * 1024U, true);
            GO2CPP_CHECK(next.resume());
            GO2CPP_CHECK(next.state() == FiberState::Completed);
        }
        GO2CPP_CHECK(usable_before.load(std::memory_order_relaxed) ==
                     64U * 1024U);
        // 深度递归已让第一个 Fiber 的 committed 下移；槽回收时必须重新
        // mprotect(PROT_NONE) 并复位，第二个 Fiber 只能拿到初始 64KiB。
        GO2CPP_CHECK(usable_after.load(std::memory_order_relaxed) ==
                     64U * 1024U);
    }

    // ---- 调度器栈复用：FiberBin 回收的 Fiber 保留栈，任务不重复建栈 ------
    {
        constexpr std::size_t kTasks = 512;
        const std::size_t before_kib = ReadVmSizeKiB();
        std::atomic<std::size_t> runs{0};
        for (std::size_t i = 0; i < kTasks; ++i) {
            auto fiber = Fiber::AcquireForScheduler(
                [&runs] { runs.fetch_add(1, std::memory_order_relaxed); },
                4096, 64U);
            GO2CPP_REQUIRE(fiber != nullptr);
            GO2CPP_CHECK(fiber->resume_from_scheduler());
            GO2CPP_CHECK(fiber->state() == FiberState::Completed);
            Fiber::RecycleForScheduler(std::move(fiber), 64U);
        }
        GO2CPP_CHECK(runs.load(std::memory_order_relaxed) == kTasks);
        const std::size_t after_kib = ReadVmSizeKiB();
        // bin 最多保留 64 个栈（64 × 8MiB = 512MiB）；不随 512 个任务增长。
        GO2CPP_CHECK(after_kib <= before_kib + 1024U * 1024U);
    }
}
