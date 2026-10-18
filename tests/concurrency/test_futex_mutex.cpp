/**
 * @file test_futex_mutex.cpp
 * @brief 验证内部唤醒门 `core::FutexMutex`（X-35 后取代 HybridGate）。
 *
 * 覆盖点：
 * 1. BasicLockable：可与 `std::lock_guard` / `std::unique_lock` 组合；
 * 2. 高竞争互斥：8 线程 × 20000 次读改写，丢失更新即失败；
 * 3. `try_lock` 语义：仅在完全空闲时成功；
 * 4. **跨线程解锁**：这是它取代 `std::mutex` 的唯一理由（hook 会把
 *    `lock/unlock` 分派到不同线程）；
 * 5. **唤醒对象已退出**（X-36）：确定性构造“排队后退出、被 FUTEX_WAKE
 *    选中但不接手锁”的等待者，验证真正的等待者仍能被唤醒；
 * 6. `FUTEX_WAKE` 返回 0 的分支：状态残留“有等待者位”但队列为空时，
 *    `unlock()` 必须把状态清回空闲；
 * 7. 幽灵等待者数量递增（0~10）的重复场景，含两个真实等待者。
 *
 * 不测自旋策略（该实现没有自旋）：竞争时直接 futex 睡眠。
 */

#include "go2cpp/concurrency/core/futex_mutex.hpp"

#include "test_support.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <fstream>
#include <string>
#endif

namespace {

using namespace std::chrono_literals;

#if defined(__linux__)

using RawWord = std::atomic<std::uint32_t>;

/// 与 `futex_mutex.hpp` 的位编码保持一致；测试直接读写底层字来注入
/// “已退出等待者”，这是**唯一**能确定性构造该场景的手段（真实 API
/// 产生的等待者不会在被唤醒后拒绝接手）。
constexpr std::uint32_t kLockedBit = 1U;
constexpr std::uint32_t kWaitersBit = 2U;
constexpr std::uint32_t kLockedWithWaiters = kLockedBit | kWaitersBit;

/// `m_state` 是 FutexMutex 唯一（且 alignas(64)）的非静态数据成员，
/// 类本身是标准布局，因此首成员地址等于对象地址。
static_assert(sizeof(go2cpp::core::FutexMutex) == 64,
              "测试依赖 m_state 独占一条缓存行");
static_assert(alignof(go2cpp::core::FutexMutex) == 64,
              "测试依赖 m_state 的 64 字节对齐");

RawWord* RawState(go2cpp::core::FutexMutex& gate) {
    return reinterpret_cast<RawWord*>(&gate);
}

/// 直接对门的状态字做一次原始 FUTEX_WAIT，模拟“排队后退出”的等待者。
int RawWait(RawWord* word, std::uint32_t expected) {
    return static_cast<int>(
        ::syscall(SYS_futex, reinterpret_cast<std::uint32_t*>(word),
                  FUTEX_WAIT_PRIVATE, expected, nullptr, nullptr, 0));
}

/// 读 `/proc/self/task/<tid>/wchan` 判断线程是否真的睡在 futex 上。
bool ThreadParkedInFutex(pid_t tid) {
    std::ifstream file("/proc/self/task/" + std::to_string(tid) + "/wchan");
    std::string wchan;
    std::getline(file, wchan);
    return wchan.find("futex") != std::string::npos;
}

/**
 * @brief 等到线程真的进入 futex 睡眠。
 * @details 幽灵场景依赖 FIFO 顺序：先入队的幽灵会先被 `FUTEX_WAKE` 选中。
 *          用 wchan 确认而非固定 sleep，避免高负载机器上顺序翻转。
 *          `/proc` 不可用时退回固定等待，不阻塞正确性断言（修复后的
 *          实现无论顺序如何都必须唤醒真实等待者）。
 */
void WaitUntilParked(pid_t tid) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (ThreadParkedInFutex(tid)) {
            return;
        }
        std::this_thread::sleep_for(1ms);
    }
    std::this_thread::sleep_for(20ms);
}

struct GhostScenarioResult {
    int ghosts{0};
    std::size_t ghosts_woken{0};
    bool real_woken{false};
    bool state_clean{false};
};

/**
 * @brief 确定性构造“唤醒对象已退出”的场景。
 *
 * 队列（FIFO）：
 *   ghost_1 … ghost_N   —— 原始 FUTEX_WAIT 等待者，被唤醒后**直接返回**，
 *                          不拿锁、不碰状态（模拟被取消/已退出的对象）；
 *   real                —— 真实 `FutexMutex::lock()` 等待者。
 *
 * `unlock()` 第一次 `FUTEX_WAKE(1)` 命中的必然是 ghost_1，内核返回 1。
 * 旧实现到此为止：real 永久睡死。修复后的实现必须继续唤醒链，直到
 * 全部 ghost 退出并让 real 拿到锁。
 *
 * gate 在失败路径上故意泄漏：被 detach 的线程会永远睡在它上面，析构即 UB。
 */
GhostScenarioResult RunGhostScenario(int ghosts) {
    GhostScenarioResult result;
    result.ghosts = ghosts;
    auto gate = std::make_unique<go2cpp::core::FutexMutex>();
    go2cpp::core::FutexMutex* const raw_gate = gate.get();
    RawWord* const state = RawState(*gate);

    raw_gate->lock();
    state->store(kLockedWithWaiters, std::memory_order_release);

    std::vector<std::thread> ghost_threads;
    std::vector<std::shared_ptr<std::atomic<pid_t>>> ghost_tids;
    // 失败路径会 detach 线程；用 shared_ptr 保证被 detach 的线程不会
    // 引用已销毁的栈对象。
    auto ghosts_done = std::make_shared<std::atomic<int>>(0);
    for (int index = 0; index < ghosts; ++index) {
        auto tid = std::make_shared<std::atomic<pid_t>>(0);
        ghost_tids.push_back(tid);
        ghost_threads.emplace_back([tid, state, ghosts_done] {
            tid->store(static_cast<pid_t>(::syscall(SYS_gettid)));
            (void)RawWait(state, kLockedWithWaiters);
            // 已退出的等待者：醒来后什么都不做。
            ghosts_done->fetch_add(1, std::memory_order_release);
        });
        while (tid->load() == 0) {
            std::this_thread::yield();
        }
        WaitUntilParked(tid->load());
    }

    auto real_tid = std::make_shared<std::atomic<pid_t>>(0);
    auto real_got = std::make_shared<std::atomic<bool>>(false);
    std::thread real([real_tid, raw_gate, real_got] {
        real_tid->store(static_cast<pid_t>(::syscall(SYS_gettid)));
        raw_gate->lock();
        real_got->store(true, std::memory_order_release);
        raw_gate->unlock();
    });
    while (real_tid->load() == 0) {
        std::this_thread::yield();
    }
    WaitUntilParked(real_tid->load());
    GO2CPP_CHECK(state->load(std::memory_order_acquire) == kLockedWithWaiters);

    raw_gate->unlock();  // 触发唤醒链

    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline &&
           (ghosts_done->load(std::memory_order_acquire) < ghosts ||
            !real_got->load(std::memory_order_acquire))) {
        std::this_thread::sleep_for(1ms);
    }

    result.ghosts_woken = static_cast<std::size_t>(
        ghosts_done->load(std::memory_order_acquire));
    result.real_woken = real_got->load(std::memory_order_acquire);
    result.state_clean = state->load(std::memory_order_acquire) == 0U;

    if (result.real_woken) {
        real.join();
    } else {
        real.detach();
    }
    for (auto& thread : ghost_threads) {
        if (result.ghosts_woken == static_cast<std::size_t>(ghosts)) {
            thread.join();
        } else {
            thread.detach();
        }
    }
    if (result.real_woken && result.ghosts_woken == static_cast<std::size_t>(ghosts)) {
        gate.reset();
    } else {
        (void)gate.release();  // 有线程仍睡在里面：故意泄漏
    }
    return result;
}

/**
 * @brief 覆盖 `FUTEX_WAKE` 返回 0 的分支。
 * @details 状态字被注入“空闲 + 有等待者位”，但 futex 队列实际为空
 *          （模拟等待者取消/退出后残留的标记）。`unlock()` 的唤醒链
 *          第一轮就会拿到 0，必须用 CAS(2 -> 0) 把门恢复成纯净空闲，
 *          并且不能破坏后续加锁。
 */
void CheckWakeWithEmptyQueue() {
    go2cpp::core::FutexMutex gate;
    gate.lock();
    RawState(gate)->store(kWaitersBit, std::memory_order_release);
    gate.unlock();
    GO2CPP_CHECK(RawState(gate)->load(std::memory_order_acquire) == 0U);

    GO2CPP_CHECK(gate.try_lock());
    gate.unlock();
    GO2CPP_CHECK(RawState(gate)->load(std::memory_order_acquire) == 0U);
}

/// 幽灵等待者数量递增（0~10），每个数量都要求真实等待者被唤醒。
void CheckGhostScenarios() {
    for (const int ghosts : {1, 2, 3, 10}) {
        const GhostScenarioResult result = RunGhostScenario(ghosts);
        GO2CPP_CHECK(result.ghosts_woken == static_cast<std::size_t>(ghosts));
        GO2CPP_CHECK(result.real_woken);
        GO2CPP_CHECK(result.state_clean);
    }
}

/**
 * @brief 两个真实等待者排在 N 个幽灵之后。
 * @details 验证唤醒链不会“唤醒一个幽灵就停下”，而是推进到两个真实
 *          等待者都被服务。
 */
void CheckTwoRealWaitersBehindGhosts() {
    auto gate = std::make_unique<go2cpp::core::FutexMutex>();
    go2cpp::core::FutexMutex* const raw_gate = gate.get();
    RawWord* const state = RawState(*gate);

    raw_gate->lock();
    state->store(kLockedWithWaiters, std::memory_order_release);

    constexpr int kGhosts = 5;
    std::vector<std::thread> ghost_threads;
    std::vector<std::shared_ptr<std::atomic<pid_t>>> tids;
    auto ghosts_done = std::make_shared<std::atomic<int>>(0);
    for (int index = 0; index < kGhosts; ++index) {
        auto tid = std::make_shared<std::atomic<pid_t>>(0);
        tids.push_back(tid);
        ghost_threads.emplace_back([tid, state, ghosts_done] {
            tid->store(static_cast<pid_t>(::syscall(SYS_gettid)));
            (void)RawWait(state, kLockedWithWaiters);
            ghosts_done->fetch_add(1, std::memory_order_release);
        });
        while (tid->load() == 0) {
            std::this_thread::yield();
        }
        WaitUntilParked(tid->load());
    }

    auto real_done = std::make_shared<std::atomic<int>>(0);
    std::vector<std::thread> real_threads;
    std::vector<std::shared_ptr<std::atomic<pid_t>>> real_tids;
    for (int index = 0; index < 2; ++index) {
        auto tid = std::make_shared<std::atomic<pid_t>>(0);
        real_tids.push_back(tid);
        real_threads.emplace_back([tid, raw_gate, real_done] {
            tid->store(static_cast<pid_t>(::syscall(SYS_gettid)));
            raw_gate->lock();
            real_done->fetch_add(1, std::memory_order_release);
            raw_gate->unlock();
        });
        while (tid->load() == 0) {
            std::this_thread::yield();
        }
        WaitUntilParked(tid->load());
    }

    raw_gate->unlock();

    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline &&
           (ghosts_done->load(std::memory_order_acquire) < kGhosts ||
            real_done->load(std::memory_order_acquire) < 2)) {
        std::this_thread::sleep_for(1ms);
    }
    const bool ok = ghosts_done->load() == kGhosts && real_done->load() == 2;
    GO2CPP_CHECK(ghosts_done->load(std::memory_order_acquire) == kGhosts);
    GO2CPP_CHECK(real_done->load(std::memory_order_acquire) == 2);
    GO2CPP_CHECK(state->load(std::memory_order_acquire) == 0U);

    for (auto& thread : ghost_threads) {
        if (ok) {
            thread.join();
        } else {
            thread.detach();
        }
    }
    for (auto& thread : real_threads) {
        if (ok) {
            thread.join();
        } else {
            thread.detach();
        }
    }
    if (ok) {
        gate.reset();
    } else {
        (void)gate.release();
    }
}

/**
 * @brief 真实等待者正常竞争时，唤醒链不得引入死锁或丢失唤醒。
 * @details 4 个线程反复加解锁；期间不注入幽灵。它同时也验证
 *          状态字在反复争用后能回到 0（不会残留等待者位）。
 */
void CheckContentionStateReturnsToZero() {
    go2cpp::core::FutexMutex gate;
    std::atomic<long long> counter{0};
    constexpr int kThreads = 4;
    constexpr int kIterations = 20000;
    std::vector<std::thread> workers;
    for (int index = 0; index < kThreads; ++index) {
        workers.emplace_back([&gate, &counter] {
            for (int round = 0; round < kIterations; ++round) {
                gate.lock();
                counter.fetch_add(1, std::memory_order_relaxed);
                gate.unlock();
            }
        });
    }
    for (auto& worker : workers) {
        GO2CPP_JOIN_WITH_WATCHDOG(worker, 60s);
    }
    GO2CPP_REQUIRE(counter.load(std::memory_order_relaxed) ==
                   static_cast<long long>(kThreads) * kIterations);
    GO2CPP_CHECK(RawState(gate)->load(std::memory_order_acquire) == 0U);
}

#endif  // defined(__linux__)

/**
 * @brief 验证满足 BasicLockable，可与作用域锁组合。
 */
void check_scoped_lock() {
    go2cpp::core::FutexMutex gate;
    {
        std::lock_guard<go2cpp::core::FutexMutex> lock(gate);
        GO2CPP_CHECK(true);
    }
    {
        std::unique_lock<go2cpp::core::FutexMutex> lock(gate);
        GO2CPP_CHECK(lock.owns_lock());
    }
    gate.lock();
    gate.unlock();
}

/**
 * @brief 多线程高竞争下验证临界区串行化。
 * @details 读-改-写刻意使用非原子的两步操作，只有互斥成立时结果才正确。
 */
void check_mutual_exclusion() {
    go2cpp::core::FutexMutex gate;
    std::atomic<long long> counter{0};
    constexpr int kThreads = 8;
    constexpr int kIterations = 20000;
    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int index = 0; index < kThreads; ++index) {
        workers.emplace_back([&gate, &counter] {
            for (int round = 0; round < kIterations; ++round) {
                gate.lock();
                counter.store(counter.load(std::memory_order_relaxed) + 1,
                              std::memory_order_relaxed);
                gate.unlock();
            }
        });
    }
    for (auto& worker : workers) {
        GO2CPP_JOIN_WITH_WATCHDOG(worker, 60s);
    }
    GO2CPP_REQUIRE(counter.load(std::memory_order_relaxed) ==
                   static_cast<long long>(kThreads) * kIterations);
}

/**
 * @brief 验证 try_lock 只在门完全空闲时成功。
 */
void check_try_lock() {
    go2cpp::core::FutexMutex gate;
    GO2CPP_CHECK(gate.try_lock());
    GO2CPP_CHECK(!gate.try_lock());
    gate.unlock();
    GO2CPP_CHECK(gate.try_lock());
    gate.unlock();
}

/**
 * @brief 验证跨线程解锁：持有在 A 线程，释放在 B 线程。
 * @details 这正是 `std::mutex` 无法满足、而唤醒门必须支持的场景。
 */
void check_cross_thread_unlock() {
    go2cpp::core::FutexMutex gate;
    gate.lock();
    std::atomic<bool> other_thread_saw_lock{false};
    std::thread other([&] {
        other_thread_saw_lock.store(!gate.try_lock(),
                                    std::memory_order_release);
        gate.unlock();  // 跨线程释放
    });
    GO2CPP_JOIN_WITH_WATCHDOG(other, 60s);
    GO2CPP_REQUIRE(other_thread_saw_lock.load(std::memory_order_acquire));
    // 释放后必须能立即重新取得，证明 unlock 真正生效。
    GO2CPP_REQUIRE(gate.try_lock());
    gate.unlock();
}

}  // namespace

/**
 * @brief 运行 FutexMutex 的全部用例。
 * @return 无；失败项由测试支持代码累计。
 */
void run_futex_mutex_tests() {
    go2cpp_tests::announce("futex mutex semantics");
    check_scoped_lock();
    check_mutual_exclusion();
    check_try_lock();
    check_cross_thread_unlock();

    go2cpp_tests::announce("futex mutex exited-waiter wakeup chain");
#if defined(__linux__)
    CheckWakeWithEmptyQueue();
    CheckGhostScenarios();
    CheckTwoRealWaitersBehindGhosts();
    CheckContentionStateReturnsToZero();
#endif
}
