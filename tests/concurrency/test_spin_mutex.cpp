/**
 * @file test_spin_mutex.cpp
 * @brief 分层锁 `sync::SpinMutex` 与持锁 park 契约检查的测试。
 *
 * @date 2026-10-18
 * @copyright MIT License（见项目根目录 LICENSE）
 *
 * ## 覆盖范围
 *
 * 1. 普通线程语义：Lock / TryLock / Unlock / IsLocked / Guard / 异常安全；
 * 2. 随机测试：随机 G 序列（Lock/TryLock/Unlock/yield），验证
 *    `no_park_depth` 与嵌套层数一致、每个 G 结束归零；
 * 3. 真实场景 ≥5：
 *    - SpinMutex 临界区内 `read`（debug：子进程 abort；release：降级计数）；
 *    - SpinMutex 临界区内纯计算（正例，正常完成）；
 *    - 普通 Mutex 临界区内 `read`（允许，但 held 计数 +1）；
 *    - `Mutex` 套 `SpinMutex` 后 `read`（内层优先：只计 SpinMutex 降级）；
 *    - 跨 G 解锁（Go 风格所有权）后 owner 的深度归零；
 *    - 嵌套两层 SpinMutex 的深度 2→1→0。
 *
 * ## 为什么要 fork 子进程
 *
 * debug 语义是 `abort()`，发生在被测进程内。父进程必须观察子进程的退出
 * 信号（SIGABRT），因此场景 1/5 用 `fork` + `exec /proc/self/exe` 并设置
 * `GO2CPP_SPIN_MUTEX_CHILD` 选择子场景。fork 后立即 exec，避免复制
 * 多线程调度器状态。
 */
#include "go2cpp/concurrency/spin_mutex.hpp"

#include "go2cpp/io.hpp"
#include "go2cpp/scheduler.hpp"
#include "go2cpp/sync.hpp"
#include "test_support.hpp"

#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

using go2cpp::IOManager;
using go2cpp::Scheduler;
using go2cpp::SchedulerConfig;
using go2cpp::Task;
using go2cpp::sync::Mutex;
using go2cpp::sync::SpinMutex;

/// debug 语义由构建类型决定，与 scheduler.cpp 的检测桩保持一致。
#if defined(GO2CPP_DEBUG_LOCKS) || !defined(NDEBUG)
constexpr bool kDebugLockContract = true;
#else
constexpr bool kDebugLockContract = false;
#endif

SchedulerConfig MakeConfig(std::size_t processors, std::size_t workers = 0) {
    SchedulerConfig config;
    config.processor_count = processors;
    config.min_workers = workers != 0 ? workers : processors;
    config.max_workers = config.min_workers;
    config.idle_wait = 1ms;
    return config;
}

void ShutdownOrReport(Scheduler& scheduler) {
    if (!scheduler.shutdown_for(5s)) {
        go2cpp_tests::watchdog_abort(
            "scheduler shutdown timed out after spin-mutex scenario", __FILE__,
            __LINE__);
    }
}

/**
 * @brief 等待任务 Depth 达到期望值。
 * @note 只用于观测：`no_park_depth` 是原子计数，读取本身不会影响锁语义。
 */
bool WaitDepth(const std::shared_ptr<Task>& task, std::uint32_t expected,
               std::chrono::milliseconds timeout = 5s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (task && task->no_park_depth() == expected) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return task && task->no_park_depth() == expected;
}

std::uint64_t SpinDegradeTotal() {
    return go2cpp::scheduler::spin_mutex_park_degrade_total();
}

std::uint64_t MutexHeldParkTotal() {
    return go2cpp::scheduler::mutex_held_park_total();
}

/// 创建一对阻塞本地 socket。
/// @note 必须用**阻塞** fd：Hook 对用户显式设成 O_NONBLOCK 的 fd 会保持
///       EAGAIN 语义、不 park（见 test_hook 的 nonblocking 用例）。只有
///       阻塞 fd 才会被 Hook 接管并 park Fiber。
bool MakePair(int (&fds)[2]) {
    return ::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) == 0;
}

/// 直接关闭 fd，绕过 Hook 的元数据（测试结束路径，避免 hook 依赖）。
void RawClose(int fd) {
    if (fd >= 0) {
        (void)::close(fd);
    }
}

// ---------------------------------------------------------------------------
// ① 普通线程语义（没有受管 G，计数不参与）
// ---------------------------------------------------------------------------

void TestBasicSemanticsOnNativeThread() {
    SpinMutex mutex;
    GO2CPP_CHECK(!mutex.IsLocked());
    GO2CPP_CHECK(mutex.TryLock());
    GO2CPP_CHECK(mutex.IsLocked());
    // 普通线程没有受管 G，不应记录 owner（也就没有深度）。
    GO2CPP_CHECK(mutex.OwnerNoParkDepth() == 0U);
    mutex.Unlock();
    GO2CPP_CHECK(!mutex.IsLocked());

    {
        SpinMutex::Guard guard(mutex);
        GO2CPP_CHECK(mutex.IsLocked());
    }
    GO2CPP_CHECK(!mutex.IsLocked());

    // 非阻塞 TryLock 在已持有时必须失败，且不能改变计时。
    SpinMutex::Guard guard(mutex);
    GO2CPP_CHECK(!mutex.TryLock());
    GO2CPP_CHECK(mutex.IsLocked());
}

/// 场景 9：`LockFor` 超时返回 false、不改变锁状态；释放后可成功获取。
void TestLockForTimeout() {
    Scheduler scheduler(MakeConfig(2));
    scheduler.start();
    SpinMutex mutex;

    // 主线程先持有锁（普通线程，没有受管 G 计数）。
    mutex.Lock();
    std::atomic<bool> timed_out{false};
    std::atomic<bool> retry_ok{false};
    auto waiter = scheduler.spawn([&] {
        const bool first = mutex.LockFor(50ms);
        timed_out.store(!first, std::memory_order_release);
        if (!first) {
            GO2CPP_CHECK(Scheduler::current_task_ptr()->no_park_depth() == 0U);
            const bool second = mutex.LockFor(1s);
            retry_ok.store(second, std::memory_order_release);
            if (second) {
                mutex.Unlock();
            }
        }
    });
    GO2CPP_REQUIRE_EVENTUALLY(timed_out.load(std::memory_order_acquire), 2s);
    // 超时返回 false 时锁必须仍被主线程持有。
    GO2CPP_CHECK(mutex.IsLocked());
    mutex.Unlock();
    GO2CPP_CHECK(waiter->wait_for(5s));
    GO2CPP_CHECK(retry_ok.load(std::memory_order_acquire));
    ShutdownOrReport(scheduler);
}

// ---------------------------------------------------------------------------
// ② 随机测试
// ---------------------------------------------------------------------------

void TestRandomizedNesting() {
    Scheduler scheduler(MakeConfig(4));
    scheduler.start();

    constexpr int kTasks = 8;
    constexpr int kLocks = 4;
    SpinMutex locks[kLocks];
    std::atomic<int> completed{0};
    std::vector<std::shared_ptr<Task>> tasks;
    tasks.reserve(kTasks);

    for (int index = 0; index < kTasks; ++index) {
        tasks.push_back(scheduler.spawn([&, index] {
            std::mt19937 rng(static_cast<unsigned>(index) * 7919U + 17U);
            std::vector<int> held;
            for (int step = 0; step < 400; ++step) {
                const int action = static_cast<int>(rng() % 5U);
                if (action <= 1 && held.size() < 3U) {
                    const int slot = static_cast<int>(rng() % kLocks);
                    if (locks[slot].TryLock()) {
                        Task* const self = Scheduler::current_task_ptr();
                        GO2CPP_CHECK(self != nullptr);
                        // 深度必须严格等于嵌套层数（含刚获得的这一层）。
                        GO2CPP_CHECK(self->no_park_depth() ==
                                     static_cast<std::uint32_t>(held.size() + 1));
                        held.push_back(slot);
                    }
                } else if (!held.empty()) {
                    locks[held.back()].Unlock();
                    held.pop_back();
                    Task* const self = Scheduler::current_task_ptr();
                    GO2CPP_CHECK(self->no_park_depth() ==
                                 static_cast<std::uint32_t>(held.size()));
                } else {
                    scheduler.yield_current();
                }
            }
            while (!held.empty()) {
                locks[held.back()].Unlock();
                held.pop_back();
            }
            GO2CPP_CHECK(Scheduler::current_task_ptr()->no_park_depth() == 0U);
            completed.fetch_add(1, std::memory_order_release);
        }));
    }

    GO2CPP_REQUIRE_EVENTUALLY(completed.load(std::memory_order_acquire) ==
                                  kTasks,
                              10s);
    for (auto& task : tasks) {
        GO2CPP_CHECK(task->wait_for(5s));
    }
    ShutdownOrReport(scheduler);
}

// ---------------------------------------------------------------------------
// ③ 真实场景：park 场景的共享搭建
// ---------------------------------------------------------------------------

/**
 * @brief 在 Fiber 中执行一个会 park 的 `read`，返回是否观察到了 park。
 *
 * @details `read` 在空 socket 上返回 EAGAIN；Hook 接管后 park 当前 G。
 *          若 Hook 未启用（无 go2cpp_hook 的构建），read 立即返回，此时
 *          本场景无法覆盖 park 路径，测试会显式报告。
 */
struct ReadParkRun {
    bool saw_read{false};
    bool saw_error{false};
    int error{0};
};

ReadParkRun RunReadScenario(IOManager& manager, bool hold_spin_mutex,
                            bool hold_plain_mutex, bool nested_contract) {
    ReadParkRun result;
    int fds[2]{-1, -1};
    if (!MakePair(fds)) {
        result.saw_error = true;
        result.error = errno;
        return result;
    }

    std::atomic<bool> reader_done{false};
    SpinMutex spin;
    Mutex plain;

    auto reader = manager.Go([&] {
        if (nested_contract) {
            plain.Lock();
        }
        if (hold_spin_mutex || nested_contract) {
            spin.Lock();
        }
        if (hold_plain_mutex) {
            plain.Lock();
        }
        char byte = 0;
        const ssize_t count = ::read(fds[0], &byte, 1);
        if (count < 0) {
            result.saw_error = true;
            result.error = errno;
        } else {
            result.saw_read = true;
        }
        if (hold_plain_mutex) {
            plain.Unlock();
        }
        if (hold_spin_mutex || nested_contract) {
            spin.Unlock();
        }
        if (nested_contract) {
            plain.Unlock();
        }
        reader_done.store(true, std::memory_order_release);
    });

    // writer 在 20ms 后写一个字节，确保 reader 已经进入 read 并 park。
    auto writer = manager.Go([&] {
        std::this_thread::sleep_for(20ms);
        const char byte = 'x';
        const ssize_t written = ::write(fds[1], &byte, 1);
        (void)written;
    });

    const bool reader_ok = reader->wait_for(5s) && writer->wait_for(5s);
    if (!reader_ok) {
        go2cpp_tests::watchdog_abort("read scenario did not finish", __FILE__,
                                     __LINE__);
    }
    if (!reader_done.load(std::memory_order_acquire)) {
        result.saw_error = true;
    }
    RawClose(fds[0]);
    RawClose(fds[1]);
    return result;
}

/// 场景 2：临界区内纯计算，正常完成。
void TestPureComputationInsideCriticalSection() {
    Scheduler scheduler(MakeConfig(2));
    scheduler.start();
    SpinMutex mutex;
    std::atomic<std::uint64_t> sum{0};
    auto task = scheduler.spawn([&] {
        for (int round = 0; round < 1000; ++round) {
            SpinMutex::Guard guard(mutex);
            std::uint64_t value = sum.load(std::memory_order_relaxed);
            for (int i = 0; i < 64; ++i) {
                value = value * 6364136223846793005ULL + 1442695040888963407ULL;
            }
            sum.store(value, std::memory_order_relaxed);
        }
        GO2CPP_CHECK(Scheduler::current_task_ptr()->no_park_depth() == 0U);
    });
    GO2CPP_CHECK(task->wait_for(5s));
    GO2CPP_CHECK(sum.load(std::memory_order_relaxed) != 0U);
    ShutdownOrReport(scheduler);
}

/// 场景 4：普通 Mutex 临界区内 read —— 允许，但 held 计数 +1。
/// 该场景在 debug/release 下都不会 abort，因此两种构建都执行。
void TestPlainMutexHeldPark() {
    const bool saved_detection = go2cpp::sync::HeldLockParkDetection();
    go2cpp::sync::SetHeldLockParkDetection(true);
    {
        IOManager manager(MakeConfig(2));
        GO2CPP_CHECK(manager.Start());
        const std::uint64_t spin_before = SpinDegradeTotal();
        const std::uint64_t held_before = MutexHeldParkTotal();
        const ReadParkRun run = RunReadScenario(manager, false, true, false);
        GO2CPP_CHECK(run.saw_read);
        GO2CPP_CHECK(MutexHeldParkTotal() >= held_before + 1U);
        GO2CPP_CHECK(SpinDegradeTotal() == spin_before);
        manager.shutdown();
    }
    go2cpp::sync::SetHeldLockParkDetection(saved_detection);
}

/// 场景 3/5：release 下降级；debug 由子进程验证 abort（本函数不执行）。
void TestSpinMutexReleaseDegrade() {
    const bool saved_detection = go2cpp::sync::HeldLockParkDetection();
    go2cpp::sync::SetHeldLockParkDetection(true);

    {
        IOManager manager(MakeConfig(2));
        GO2CPP_CHECK(manager.Start());
        const std::uint64_t spin_before = SpinDegradeTotal();
        const std::uint64_t held_before = MutexHeldParkTotal();
        const ReadParkRun run = RunReadScenario(manager, true, false, false);
        GO2CPP_CHECK(run.saw_read);
        // release：必须降级、计数增长，且绝不能 abort。
        GO2CPP_CHECK(SpinDegradeTotal() >= spin_before + 1U);
        // 内层 SpinMutex 优先，普通 Mutex 的告警计数不应增长。
        GO2CPP_CHECK(MutexHeldParkTotal() == held_before);
        manager.shutdown();
    }

    {
        // 场景 5：Mutex 套 SpinMutex，内层优先。
        IOManager manager(MakeConfig(2));
        GO2CPP_CHECK(manager.Start());
        const std::uint64_t spin_before = SpinDegradeTotal();
        const std::uint64_t held_before = MutexHeldParkTotal();
        const ReadParkRun run = RunReadScenario(manager, false, false, true);
        GO2CPP_CHECK(run.saw_read);
        GO2CPP_CHECK(SpinDegradeTotal() >= spin_before + 1U);
        GO2CPP_CHECK(MutexHeldParkTotal() == held_before);
        manager.shutdown();
    }

    go2cpp::sync::SetHeldLockParkDetection(saved_detection);
}

/// 场景 6：跨 G 解锁后 owner 的深度必须归零。
void TestCrossTaskUnlock() {
    Scheduler scheduler(MakeConfig(2));
    scheduler.start();
    SpinMutex mutex;
    std::atomic<bool> locked{false};
    std::atomic<bool> released{false};

    auto owner = scheduler.spawn([&] {
        mutex.Lock();
        locked.store(true, std::memory_order_release);
        while (!released.load(std::memory_order_acquire)) {
            scheduler.yield_current();
        }
    });
    GO2CPP_REQUIRE_EVENTUALLY(locked.load(std::memory_order_acquire), 5s);
    GO2CPP_CHECK(WaitDepth(owner, 1U));

    auto releaser = scheduler.spawn([&] {
        mutex.Unlock();  // Go 风格：由另一个 G 解锁。
        released.store(true, std::memory_order_release);
    });
    GO2CPP_CHECK(releaser->wait_for(5s));
    GO2CPP_CHECK(WaitDepth(owner, 0U));
    GO2CPP_CHECK(owner->wait_for(5s));
    ShutdownOrReport(scheduler);
}

/// 场景 7：嵌套两层 SpinMutex 的深度 2→1→0。
void TestNestedSpinMutexDepth() {
    Scheduler scheduler(MakeConfig(2));
    scheduler.start();
    SpinMutex outer;
    SpinMutex inner;
    std::atomic<bool> layer_one{false};
    std::atomic<bool> layer_two{false};
    std::atomic<bool> gate_one{false};
    std::atomic<bool> gate_two{false};

    auto task = scheduler.spawn([&] {
        outer.Lock();
        layer_one.store(true, std::memory_order_release);
        while (!gate_one.load(std::memory_order_acquire)) {
            scheduler.yield_current();
        }
        inner.Lock();
        layer_two.store(true, std::memory_order_release);
        while (!gate_two.load(std::memory_order_acquire)) {
            scheduler.yield_current();
        }
        inner.Unlock();
        outer.Unlock();
    });
    GO2CPP_REQUIRE_EVENTUALLY(layer_one.load(std::memory_order_acquire), 5s);
    GO2CPP_CHECK(WaitDepth(task, 1U));
    gate_one.store(true, std::memory_order_release);
    GO2CPP_REQUIRE_EVENTUALLY(layer_two.load(std::memory_order_acquire), 5s);
    GO2CPP_CHECK(WaitDepth(task, 2U));
    gate_two.store(true, std::memory_order_release);
    GO2CPP_CHECK(task->wait_for(5s));
    GO2CPP_CHECK(WaitDepth(task, 0U));
    ShutdownOrReport(scheduler);
}

/// 场景 8：临界区外 park 不得误报 SpinMutex 契约。
void TestParkOutsideCriticalSection() {
    Scheduler scheduler(MakeConfig(2));
    scheduler.start();
    const std::uint64_t spin_before = SpinDegradeTotal();
    Mutex mutex;
    std::atomic<bool> holder_ready{false};
    std::atomic<bool> release_holder{false};

    auto holder = scheduler.spawn([&] {
        mutex.Lock();
        holder_ready.store(true, std::memory_order_release);
        while (!release_holder.load(std::memory_order_acquire)) {
            scheduler.yield_current();
        }
        mutex.Unlock();
    });
    GO2CPP_REQUIRE_EVENTUALLY(holder_ready.load(std::memory_order_acquire), 5s);

    std::atomic<bool> waiter_done{false};
    auto waiter = scheduler.spawn([&] {
        // 没有 SpinMutex：普通 Mutex 竞争会 park，但不应触发 SpinMutex 检查。
        mutex.Lock();
        mutex.Unlock();
        waiter_done.store(true, std::memory_order_release);
    });
    GO2CPP_REQUIRE_EVENTUALLY(!waiter_done.load(std::memory_order_acquire), 1s);
    release_holder.store(true, std::memory_order_release);
    GO2CPP_CHECK(waiter->wait_for(5s));
    GO2CPP_CHECK(holder->wait_for(5s));
    GO2CPP_CHECK(SpinDegradeTotal() == spin_before);
    ShutdownOrReport(scheduler);
}

// ---------------------------------------------------------------------------
// ④ debug abort 的 fork + exec 验证
// ---------------------------------------------------------------------------

/// 父进程：fork 子进程运行指定场景，期望 SIGABRT。
bool ForkChildExpectAbort(const char* scenario) {
    const pid_t pid = ::fork();
    if (pid < 0) {
        return false;
    }
    if (pid == 0) {
        ::setenv("GO2CPP_SPIN_MUTEX_CHILD", scenario, 1);
        ::execl("/proc/self/exe", "go2cpp_tests",
                static_cast<char*>(nullptr));
        ::_exit(127);
    }
    for (int attempt = 0; attempt < 100; ++attempt) {
        int status = 0;
        const pid_t done = ::waitpid(pid, &status, WNOHANG);
        if (done == pid) {
            return WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
        }
        if (done < 0) {
            return false;
        }
        std::this_thread::sleep_for(50ms);
    }
    ::kill(pid, SIGKILL);
    (void)::waitpid(pid, nullptr, 0);
    return false;
}

}  // namespace

/**
 * @brief 子进程入口：运行会触发 SpinMutex 契约违反的场景。
 *
 * @details 只由父进程通过 `GO2CPP_SPIN_MUTEX_CHILD` 环境变量启动。debug
 *          构建下预期在 park 入口 abort；release 构建下正常返回（父进程
 *          不会在 release 下调用本入口）。
 */
void run_spin_mutex_child(const char* scenario) {
    go2cpp::IOManager manager(MakeConfig(2));
    if (!manager.Start()) {
        std::fprintf(stderr, "[spin-mutex-child] manager start failed\n");
        std::exit(127);
    }
    if (std::strcmp(scenario, "nested") == 0) {
        (void)RunReadScenario(manager, false, false, true);
    } else {
        (void)RunReadScenario(manager, true, false, false);
    }
    manager.shutdown();
    // 走到这里说明没有 abort —— debug 契约检查失效。
    std::fprintf(stderr,
                 "[spin-mutex-child] scenario '%s' did not abort; debug lock "
                 "contract is not enforced\n",
                 scenario);
    std::exit(3);
}

/**
 * @brief 运行 SpinMutex / 持锁 park 契约的全部测试。
 * @return 无；失败由统一的 `g_failures` 统计。
 */
void run_spin_mutex_tests() {
    go2cpp_tests::announce("spin mutex and held-park contract");

    TestBasicSemanticsOnNativeThread();
    TestLockForTimeout();
    TestRandomizedNesting();
    TestPureComputationInsideCriticalSection();
    TestCrossTaskUnlock();
    TestNestedSpinMutexDepth();
    TestParkOutsideCriticalSection();
    TestPlainMutexHeldPark();

    if (kDebugLockContract) {
        // 场景 1：SpinMutex 临界区 read -> abort。
        GO2CPP_CHECK(ForkChildExpectAbort("read"));
        // 场景 5：Mutex 套 SpinMutex 临界区 read -> 内层优先，仍 abort。
        GO2CPP_CHECK(ForkChildExpectAbort("nested"));
    } else {
        // release 不做 abort 验证；这里断言降级计数与「不崩」。
        TestSpinMutexReleaseDegrade();
    }
}
