/**
 * @file sync.cpp
 * @brief sync 命名空间下的同步原语实现（Mutex / WaitGroup / Once 等）。
 *
 * ## 这个文件实现什么
 *
 * 面向**用户代码**的同步原语。与内部唤醒门 `core::FutexMutex`（只做
 * CAS + futex、零点几条赋值）不同，这里的原语要求完整语义（支持超时、
 * 跨线程、可被 Fiber 使用），所以实现更重。
 *
 * ## 已实现的机制
 *
 * - `sync::Mutex`：混合锁 —— 无竞争时走原子 CAS 快路径，有等待者时才 park。
 *   关键参数是"何时从自旋转入 park"：对极短临界区（计数器）加自旋预算能快
 *   21 倍，但这个结论**不能推广**到长临界区（见 `futex_mutex.hpp` 的限制）；
 * - 等待/唤醒走 `ParkingCondition`，因此同一份代码在 Fiber 里会 park 协程、
 *   在普通线程里退化为原生等待；
 * - 其余原语（WaitGroup / Once 等）建立在上述两者之上。
 *
 * ## 踩过的坑（改动前必读）
 *
 * `Mutex` 的 futex 化重构失败过两次，原因分别是：
 * 1. 没有先检查终态、把 `EAGAIN` 与 `ETIMEDOUT` 混为一谈、以及重新读取代际值
 *    三个问题叠加，导致 `io_hook` 里崩溃；
 * 2. 在 `WaitOnce` 外层多包了一层 `lock()`，违反"WaitOnce 负责重新加锁"的
 *    契约，抛 `EDEADLK`。
 *
 * 因此这块的注释会特别强调**锁契约**，不要随意简化。
 */
#include "go2cpp/sync.hpp"

#include "go2cpp/concurrency/core/futex_mutex.hpp"

#include "go2cpp/debug/debug_event_ring.hpp"
#include "go2cpp/debug/debug_resources.hpp"
#include "go2cpp/fiber.hpp"
#include "go2cpp/scheduler.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif


#if defined(__linux__)
#include <cerrno>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace {

/// FUTEX_WAIT 前的"无进展"自旋预算：唤醒已在路上时完全避开系统调用。
///
/// 128（≈1.1µs）只够覆盖「通知已经发出」的窄窗口，却覆盖不了
/// **一次完整的 FIFO 交接**：4 线程争用时每个等待者都会先睡一次，
/// 实测 25~30µs/op（见 docs/performance.md 的 250 倍退化）。
/// 2048 次 PAUSE（本机实测每次 8.7ns，约 18µs）足以让「持锁者解锁 →
/// TryFinish 选中队首 → 队首看到终态」整个过程在用户态完成。
/// 只要观察到交接进展（代际推进）就重置该计数，因此不会因为排队人数多
/// 而误睡。预算仍然**有界**：连续无进展超限就 FUTEX_WAIT。
constexpr int kWaitSpinBudget = 2048;

/// 单次连续自旋的绝对上限（≈2.3ms）。防止"代际持续被别人推进、自己却
/// 永远排不到"的异常情况把线程变成无界忙等（X-35 的红线）。睡醒后重置。
constexpr long long kWaitSpinTotalBudget = 262144;

// 裸 `FUTEX_WAIT_PRIVATE`：原子地比较 *word == expected 后睡眠，
// 值已变则立即返回 EAGAIN（"我不该睡"）。
// **PRIVATE 后缀**表示只在本进程内等待 —— 不经过跨进程的 futex 哈希表，
// 更快；这个条件变量从不跨进程共享，所以 PRIVATE 正确。
// 调用方负责在 EAGAIN 时重新检查谓词，不能把它当成错误。
int WaitFutex(std::atomic<std::uint32_t>* word, std::uint32_t expected) {
    return static_cast<int>(::syscall(SYS_futex,
                                      reinterpret_cast<std::uint32_t*>(word),
                                      FUTEX_WAIT_PRIVATE, expected, nullptr,
                                      nullptr, 0));
}

// 唤醒最多 n 个在 *word 上等待的线程（PRIVATE 版，配合 WaitFutex）。
// 返回值是**实际唤醒数**，可能小于 n —— 调用方不能据此推断等待者数量，
// 因为唤醒与等待之间存在竞态（等待者可能刚决定睡下还没进内核）。
int WakeFutex(std::atomic<std::uint32_t>* word, int n) {
    return static_cast<int>(::syscall(SYS_futex,
                                      reinterpret_cast<std::uint32_t*>(word),
                                      FUTEX_WAKE_PRIVATE, n, nullptr, nullptr, 0));
}

}  // namespace
#endif  // __linux__

namespace go2cpp::sync {
namespace {

// ---- 持锁 park 检测（最小实现） ----
// 计数只在检测开启时维护；默认值见 DetectHeldParkEnabled()。
std::atomic<int> g_detect_held_park{-1};  // -1=未解析，0/1=显式

bool DetectHeldParkEnabled() noexcept {
    int mode = g_detect_held_park.load(std::memory_order_relaxed);
    if (mode < 0) {
        static const bool default_enabled = [] {
            if (const char* value = std::getenv("GO2CPP_DETECT_HELD_PARK")) {
                return std::atoi(value) != 0;
            }
#if !defined(NDEBUG)
            return true;
#else
            return false;
#endif
        }();
        mode = default_enabled ? 1 : 0;
        g_detect_held_park.store(mode, std::memory_order_relaxed);
    }
    return mode != 0;
}

/**
 * @brief Go2Cpp Scheduler 的默认同步等待适配器。
 * @details Scheduler 类型只在 .cpp 内出现，公共 sync 头因此保持与调度器
 *          解耦；其他 Fiber 后端只需实现 WaitBackend 协议即可接入。
 */
class SchedulerWaitBackend final : public WaitBackend {
public:
    bool Park(void* scheduler_ptr, const std::shared_ptr<void>& task_value,
              std::optional<ContextTimePoint>,
              const std::function<bool()>& predicate) override {
        auto* scheduler = static_cast<Scheduler*>(scheduler_ptr);
        auto task = std::static_pointer_cast<Task>(task_value);
        if (scheduler == nullptr || !task) {
            return false;
        }
        // 持锁 park 的计数/告警统一在 `Task::park_for_scheduler()` 入口做：
        // 那里覆盖 park_wait / park_io / park_with_reason / cleanup 全部路径，
        // 而本后端只能覆盖"等 sync 原语"这一类（用户场景是持锁 read → park_io）。
        // 这里只负责维护 `Task::held_mutex_count()` 这个输入。
        for (;;) {
            if (predicate()) {
                return true;
            }
            const bool suspended = scheduler->park_wait(task);
            if (predicate()) {
                return true;
            }
            if (task->cancellation_requested() ||
                Scheduler::current_scheduler() != scheduler ||
                Scheduler::current_task().get() != task.get() ||
                (!suspended && !scheduler->is_running())) {
                return false;
            }
        }
    }

    void Wake(void* scheduler_ptr,
              const std::shared_ptr<void>& task_value) noexcept override {
        try {
            auto* scheduler = static_cast<Scheduler*>(scheduler_ptr);
            auto task = std::static_pointer_cast<Task>(task_value);
            if (scheduler != nullptr && task) {
                const bool published = scheduler->wake_registered(task);
                // `wake_registered` may report false for the benign
                // Running/Waiting -> Runnable pending-wake window.  Once the
                // execution claim has been released, a second admission
                // attempt closes the queue-publication race; if the claim is
                // still held, enqueue() simply leaves the deferred handoff
                // for the owning worker's post-Fiber cleanup path.
                if (!published && task->state() == GState::kRunnable &&
                    !task->queued()) {
                    (void)scheduler->enqueue(task);
                }
            }
        } catch (...) {
            // 唤醒回调位于取消/通知路径，异常必须被隔离。
        }
    }

    bool ParkForCleanup(
        void* scheduler_ptr, const std::shared_ptr<void>& task_value,
        const std::function<bool()>& predicate) override {
        auto* scheduler = static_cast<Scheduler*>(scheduler_ptr);
        auto task = std::static_pointer_cast<Task>(task_value);
        if (scheduler == nullptr || !task) {
            return false;
        }
        for (;;) {
            if (predicate()) {
                return true;
            }
            if (!scheduler->park_wait_for_cleanup(task)) {
                return false;
            }
        }
    }
};

std::mutex s_wait_backend_mutex;
std::shared_ptr<const WaitBackendFactory> s_wait_backend_factory;

enum class WaitResult : std::uint8_t {
    kWaiting,
    kNotified,
    kCancelled,
};

// 自旋等待时的 CPU 提示。x86 用 `_mm_pause`：让流水线知道这是自旋，
// 减少分支预测失败导致的流水线冲刷，也降低功耗与超线程争用。
// **刻意不用** `sched_yield()`：后者会把 CPU 让给别的线程，
// 在几十次迭代的自旋预算里，让出/换回的开销远大于收益。
inline void RelaxCpu() noexcept {
#if defined(__x86_64__) || defined(__i386__)
    _mm_pause();
#else
    std::this_thread::yield();
#endif
}

/// 当前单调时钟纳秒数（只在竞争慢路径读取；无竞争热路径不付这个开销）。
inline std::uint64_t SteadyNowNs() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

/// 饥饿模式阈值：与 Go `sync.Mutex` 的 `starvationThresholdNs` 对齐（1ms）。
/// 一个等待者从真正入队起累计等待超过该值，就在再次入队时把锁切到饥饿模式，
/// 由 `Unlock` 直接按 FIFO 交接所有权。
constexpr std::uint64_t kStarvationThresholdNs = 1'000'000ULL;

/**
 * @brief 普通线程在正常模式下的有界自旋预算。
 *
 * 2026-10-20 在本机（Ryzen 9 8945HX，6 物理核 / 12 逻辑核，8 OS 线程）
 * 重新标定，500ms 窗口的稳态数据：
 *
 * | 自旋预算 | sync::Mutex | std::mutex（同进程对照）|
 * |---|---|---|
 * | 0 | 28–38 ns/op | 31–33 ns/op |
 * | 8 | 28–34 ns/op | 31–34 ns/op |
 * | 16 | 27–30 ns/op | 29–33 ns/op |
 * | 32 | 25–30 ns/op | 27–28 ns/op |
 * | 64 | 55 ns/op | 30 ns/op |
 * | 128 | 82–95 ns/op | 28–29 ns/op |
 * | 2048（旧值） | 175–280 ns/op | — |
 *
 * 结论与旧设计相反：**长时间自旋反而更慢**。原因是 SMT 下多个线程持续
 * `CAS` 同一缓存行会让持锁者和其他 CPU 的流水线互相拖慢；短自旋后让等待者
 * 进入队列、在**自己的 WaitNode 缓存行**上自旋（WaitNative 的有界自旋），
 * 解锁者通过 FIFO 令牌交接，反而消除了共享缓存行的风暴。预算取 8
 * （约 100ns）既能吸收「刚释放就要重新获取」的极短竞争，又不会形成 herd。
 * 交替 A/B 实测（同一时段，3 轮）：8 → 19.4/20.5/20.1 ns，16 → 20.1/20.8/20.8 ns，
 * 32 → 25.4/22.3/25.3 ns；4/8/16 线程的无竞争临界区基准也以 8 最优。
 * 该值只影响性能，不影响语义；其他平台可用编译期宏覆盖后重新标定。
 */
#ifndef GO2CPP_MUTEX_SPIN_ATTEMPTS
#define GO2CPP_MUTEX_SPIN_ATTEMPTS 8
#endif
constexpr int kNativeSpinAttempts = GO2CPP_MUTEX_SPIN_ATTEMPTS;

/**
 * @brief `sync` 层的等待节点：一次阻塞（等锁 / 等通知 / 等 WaitGroup 归零）对应一个。
 *
 * ## 定位
 *
 * 由 `Mutex::LockImpl`、`ConditionVariable::Wait`、`WaitGroup::Wait` 创建，挂到各自的
 * `m_waiters` 队列（存 `shared_ptr`，理由见 `Mutex::Impl` 的说明）。
 *
 * ⚠️ 它与 `io::IOManager::State::WaitNode` 是**两套不同的节点**：
 *
 * | | 本节点 | `io::IOManager::State::WaitNode` |
 * |---|---|---|
 * | 等什么 | 锁 / 通知 / 计数归零 | fd 就绪 |
 * | 谁唤醒 | `Unlock` / `Notify` / `Done` | epoll 事件 / 定时器 / close |
 * | 队列 | `Mutex::Impl::m_waiters` 等 | `FdSlot::readers/writers` |
 *
 * ## 生命周期
 *
 * `kWaiting` → `Arm()`（标记活跃）→ 被 `TryFinish(...)` 认领（CAS，只有第一个源成功）
 * → 析构 `Disarm()`。
 *
 * ## 为什么 Arm/Disarm 要加锁
 *
 * `DoneSignal` 的回调可能已经离开注册表、但它的 `Wake()` 仍在执行。`Disarm` 持
 * `m_wake_mutex` 等它完成，再把 `m_scheduler` 置空 —— 防止回调生命周期超过
 * Scheduler 的关闭过程（**UAF**）。这与 `io` 层那个节点的 disarm 门是同一手法。
 */
class WaitNode final {
public:
    WaitNode(Scheduler* scheduler, std::shared_ptr<Task> task)
        : m_scheduler(scheduler), m_task(std::move(task)),
          m_backend(CreateWaitBackend()) {}

    ~WaitNode() { Disarm(); }

    bool Arm() noexcept {
        std::lock_guard<core::FutexMutex> lock(m_wake_mutex);
        if (result() != WaitResult::kWaiting) {
            return false;
        }
        m_active = true;
        return true;
    }

    void Disarm() noexcept {
        // 回调可能已经离开 DoneSignal 的注册表。这里等待其 Wake 调用完成，
        // 防止回调生命周期超过 Scheduler 的关闭过程。
        std::lock_guard<core::FutexMutex> lock(m_wake_mutex);
        m_active = false;
        m_scheduler = nullptr;
    }

    WaitResult result() const noexcept {
        return m_result.load(std::memory_order_acquire);
    }

    // CAS 把等待结果从 kWaiting 推到终态，返回是否**本次**完成了转移。
    // 非阻塞且幂等：多个通知者并发调用时只有一个返回 true，
    // 其余看到已终态返回 false —— 这保证了"先到先赢、不会覆盖对方的结论"。
    bool TryFinish(WaitResult result) noexcept {
        WaitResult expected = WaitResult::kWaiting;
        return m_result.compare_exchange_strong(
            expected, result, std::memory_order_acq_rel,
            std::memory_order_acquire);
    }

    /**
     * @brief 饥饿模式的「所有权直接交接」：认领节点并标记为已授予锁。
     *
     * 先写 `m_granted` 再 CAS `m_result`：等待者在自旋中一旦看到
     * `kNotified` 就会读 `m_granted`，若顺序反过来会出现「看到通知但还没
     * 看到授予」的窗口，在饥饿模式下会误以为自己需要重新竞争。
     * 反之，先发布授予、后发布通知，等待者读 `m_result`（acquire）时
     * 一定能看到之前的 `m_granted` 写（release）。
     *
     * @return true 表示本节点赢得了交接，`Wake()` 后调用者直接持有锁。
     */
    bool TryFinishAndGrant() noexcept {
        m_granted.store(true, std::memory_order_release);
        if (!TryFinish(WaitResult::kNotified)) {
            // 已被取消/超时认领；撤销标记，节点随后会被回收复用。
            m_granted.store(false, std::memory_order_relaxed);
            return false;
        }
        return true;
    }

    /// @brief 本节点是否通过饥饿模式直接获得了锁（必须持有锁返回）。
    bool granted() const noexcept {
        return m_granted.load(std::memory_order_acquire);
    }

    void Wake() noexcept {
        std::lock_guard<core::FutexMutex> lock(m_wake_mutex);
        WakeLocked();
    }

    // Context 的取消回调可能已经通过 weak_ptr 取得了本节点，随后
    // RemoveCallback 又与节点回收并发。取消、active 检查和唤醒必须在
    // 同一把门锁内完成；否则旧回调可能在节点放回 thread-local cache
    // 后，把下一次等待的 m_result 错误地改成 Cancelled。generation 用来
    // 区分同一个缓存节点的不同等待代次。
    bool TryCancelAndWake(std::uint64_t generation) noexcept {
        std::lock_guard<core::FutexMutex> lock(m_wake_mutex);
        if (generation != m_generation ||
            !TryFinish(WaitResult::kCancelled)) {
            return false;
        }
        // 取消可能与 Arm() 交错。未 Arm 时只发布终态，发布方会在
        // 入队锁内观察到 Cancelled 并放弃挂起；已 Arm 时才需要实际唤醒。
        if (m_active) {
            WakeLocked();
        }
        return true;
    }

    // 读取唤醒代际。每次 Wake 递增；等待者用它判断"我记下的代际是否已过期"
    // （过期说明通知已发生，无需再 park）。读时持锁，与 WakeLocked 的写互斥。
    std::uint64_t generation() noexcept {
        std::lock_guard<core::FutexMutex> lock(m_wake_mutex);
        return m_generation;
    }

private:
    void WakeLocked() noexcept {
        if (!m_active) {
            return;
        }
        try {
            if (m_scheduler != nullptr && m_task) {
                // managed waiter 通过所属 Scheduler 恢复。
                // 已注册 G 走 wake_registered()；该入口仍保留 started G，
                // 即使 shutdown 或队列分配与这个回调并发。
                if (m_backend) {
                    m_backend->Wake(m_scheduler,
                                    std::static_pointer_cast<void>(m_task));
                }
            } else {
                // 普通线程调用者在节点私有条件变量上等待。结果字段就是谓词，
                // 因此 wait() 之前到达的通知不会丢失。
#if defined(__linux__)
                // 代际推进必须无条件执行：它既是等待者的"通知已发生"信号，
                // 也是 FUTEX_WAIT 的唤醒字。只有确实有人睡在 futex 上时才付
                // 一次系统调用；正在自旋的等待者会通过代际/结果自行发现。
                m_futex_seq.fetch_add(1, std::memory_order_release);
                if (m_futex_waiters.load(std::memory_order_acquire) != 0U) {
                    (void)WakeFutex(&m_futex_seq, 1);
                }
#else
                m_native_condition.notify_one();
#endif
            }
        } catch (...) {
            // Scheduler 的可靠唤醒交接会保留所有已经启动的 G。
        }
    }

public:

    bool managed() const noexcept {
        return m_scheduler != nullptr && static_cast<bool>(m_task);
    }

    /// @brief 受管等待者的 G 编号；native/未知为 0（调试事件用）。
    std::uint64_t gid() const noexcept {
        return m_task ? static_cast<std::uint64_t>(m_task->id()) : 0U;
    }

    WaitResult WaitNative() noexcept {
#if defined(__linux__)
        // futex 无状态等待：有界自适应自旋 -> FUTEX_WAIT(代际) -> 重判终态。
        //
        // ## 为什么自旋必须覆盖"一次完整交接"
        //
        // 这里的自旋不是可有可无的优化，而是**避免整个 Mutex 退化成
        // "每个操作睡一次"** 的关键：解锁方在这段时间内把所有权直接交给
        // 队首（TryFinish + Wake），正在自旋的等待者一看到终态就返回，
        // 完全不需要 FUTEX_WAIT/WAKE 两次系统调用。
        //
        // ## 为什么用"无进展"而不是"固定次数"
        //
        // 固定预算（历史值 128 ≈ 1.1µs）覆盖不了一次完整交接：4 线程争用
        // 时每个操作都恰好睡一次，实测 25~30µs/op（`docs/performance.md`
        // 记录的 250 倍退化）。只把固定值调大又会在真正需要睡眠时白烧 CPU。
        //
        // 因此这里统计的是**连续没有观察到交接进展**的次数：只要队首还在
        // 被唤醒（m_futex_seq 推进），说明持有者仍在推进，继续自旋；
        // 连续 kWaitSpinBudget 次（约 18µs，PAUSE 实测 8.7ns）没有任何进展，
        // 才判定"持锁者可能被换出"，转入 FUTEX_WAIT。
        //
        // 仍然严格有界：总自旋次数另有 kWaitSpinTotalBudget 上限，
        // 不会出现 X-35 禁止的无界自旋；睡醒后重新计数。
        std::uint32_t expected = m_futex_seq.load(std::memory_order_acquire);
        for (;;) {
            if (result() != WaitResult::kWaiting) {
                return result();
            }
            int idle_spins = 0;
            long long total_spins = 0;
            while (idle_spins < kWaitSpinBudget &&
                   total_spins < kWaitSpinTotalBudget) {
                if (result() != WaitResult::kWaiting) {
                    return result();
                }
                const std::uint32_t observed =
                    m_futex_seq.load(std::memory_order_acquire);
                if (observed != expected) {
                    // 有别的等待者被交接：这是进展，重置"无进展"计数。
                    expected = observed;
                    idle_spins = 0;
                } else {
                    ++idle_spins;
                }
                ++total_spins;
                RelaxCpu();
            }
            if (result() != WaitResult::kWaiting) {
                return result();
            }
            // 发布"我将睡在 futex 上"，再复核一次终态。顺序很重要：
            // 唤醒方先 TryFinish 再读 m_futex_waiters；等待方先自增再读结果。
            // 二者至少有一方能看到对方，因此不会丢失唤醒。
            m_futex_waiters.fetch_add(1U, std::memory_order_acq_rel);
            if (result() != WaitResult::kWaiting) {
                m_futex_waiters.fetch_sub(1U, std::memory_order_acq_rel);
                return result();
            }
            const std::uint32_t latest =
                m_futex_seq.load(std::memory_order_acquire);
            if (latest != expected) {
                expected = latest;
                m_futex_waiters.fetch_sub(1U, std::memory_order_acq_rel);
                continue;
            }
            (void)WaitFutex(&m_futex_seq, expected);
            m_futex_waiters.fetch_sub(1U, std::memory_order_acq_rel);
            const std::uint32_t after_wake =
                m_futex_seq.load(std::memory_order_acquire);
            if (after_wake != expected) {
                expected = after_wake;
            }
        }
#else
        try {
            std::unique_lock<std::mutex> lock(m_native_mutex);
            m_native_condition.wait(lock, [this] {
                return result() != WaitResult::kWaiting;
            });
        } catch (...) {
            // condition_variable 可能报告实现或系统错误。不要让错误越过运行时
            // 等待边界（该边界因调度器和取消回调只是通知而声明为 noexcept），
            // 而应将其转换为与取消相同的终态路径。
            (void)TryFinish(WaitResult::kCancelled);
        }
        return result();
#endif
    }

    const std::shared_ptr<Task>& task() const noexcept { return m_task; }
    Scheduler* scheduler() const noexcept { return m_scheduler; }
    const std::shared_ptr<WaitBackend>& backend() const noexcept {
        return m_backend;
    }

    void Reset(Scheduler* scheduler, std::shared_ptr<Task> task) noexcept {
        std::lock_guard<core::FutexMutex> lock(m_wake_mutex);
        m_active = false;
        m_scheduler = scheduler;
        m_task = std::move(task);
        m_backend = CreateWaitBackend();
        m_result.store(WaitResult::kWaiting, std::memory_order_release);
        m_granted.store(false, std::memory_order_relaxed);
#if defined(__linux__)
        // 复用的节点必须回到「没有线程睡在 futex 上」的状态；正常路径在
        // WaitNative 返回前已归零，这里再显式清一次，避免上一代异常退出
        // 让唤醒方误判仍需 FUTEX_WAKE。
        m_futex_waiters.store(0U, std::memory_order_relaxed);
#endif
        ++m_generation;
        if (m_generation == 0U) {
            m_generation = 1U;
        }
    }


private:
    Scheduler* m_scheduler{nullptr};
    std::shared_ptr<Task> m_task;
    std::atomic<WaitResult> m_result{WaitResult::kWaiting};
    /// @brief 饥饿模式所有权交接标记；普通唤醒保持 false（等待者需重新竞争）。
    std::atomic<bool> m_granted{false};
    core::FutexMutex m_wake_mutex;
#if defined(__linux__)
    // 每个等待者自己的 futex 代际字：notify_one 只推进这一个 node，
    // 所以不需要共享代际/等待者计数（比 ParkingCondition 的形态更简单）。
    alignas(64) std::atomic<std::uint32_t> m_futex_seq{0};
    // 当前真正睡在 m_futex_seq 上的线程数。唤醒方先发布终态、再读本计数：
    // 计数为 0 说明等待者仍在自旋（会自行看到终态），可以省掉一次
    // FUTEX_WAKE 系统调用；计数非 0 才必须唤醒。
    std::atomic<std::uint32_t> m_futex_waiters{0};
#endif
    std::mutex m_native_mutex;
    std::condition_variable m_native_condition;
    std::shared_ptr<WaitBackend> m_backend;
    bool m_active{false};
    std::uint64_t m_generation{1U};
};

// WaitNode 只在等待期间被队列或取消回调引用。完成一次等待后将对象放回
// 当前 M 的小缓存，避免混合 Mutex 在短临界区竞争时反复分配控制块和
// condition_variable。缓存节点已清空 scheduler/task，不延长用户对象生命期。
thread_local std::vector<std::shared_ptr<WaitNode>> t_wait_node_cache;

std::shared_ptr<WaitNode> AcquireWaitNode(Scheduler* scheduler,
                                          std::shared_ptr<Task> task) {
    if (!t_wait_node_cache.empty()) {
        auto waiter = std::move(t_wait_node_cache.back());
        t_wait_node_cache.pop_back();
        waiter->Reset(scheduler, std::move(task));
        return waiter;
    }
    return std::make_shared<WaitNode>(scheduler, std::move(task));
}

void ReleaseWaitNode(std::shared_ptr<WaitNode> waiter) noexcept {
    if (!waiter) {
        return;
    }
    waiter->Reset(nullptr, {});
    if (t_wait_node_cache.size() < 32U) {
        t_wait_node_cache.push_back(std::move(waiter));
    }
}

/**
 * @brief 一次等待的「执行者」三元组：调度器 + Task + 是否是手动 Fiber。
 *
 * ## 为什么需要 `unsupported_manual_fiber`
 *
 * 用手动 `Fiber`（`Fiber::resume()`，未归属 Scheduler）的代码也会走到阻塞原语，
 * 但那时**没有 M 可以 park**。必须返回「不支持」而不是硬等或崩溃 —— 这个标志让
 * `wait_*` 能区分「普通线程」（可以走原生条件变量）与「手动 Fiber」（只能报错）。
 */
struct WaitTarget {
    /// @brief 当前调度器；nullptr 表示不在受管上下文。
    Scheduler* scheduler{nullptr};
    /// @brief 当前 G；nullptr 表示普通线程或手动 Fiber。
    std::shared_ptr<Task> task;
    /// @brief 是否在「手动 Fiber 且无调度器」上下文（此时不能 park）。
    bool unsupported_manual_fiber{false};

    explicit operator bool() const noexcept {
        return scheduler != nullptr && static_cast<bool>(task);
    }
};

WaitTarget CurrentTarget() noexcept {
    Scheduler* const scheduler = Scheduler::current_scheduler();
    auto task = Scheduler::current_task();
    const bool manual_fiber = Fiber::Current() != nullptr &&
                              (scheduler == nullptr || !task);
    return {scheduler, std::move(task), manual_fiber};
}

/**
 * @brief 把等待节点注册到 Context 的取消信号上，析构时自动注销（RAII）。
 *
 * ## 定位
 *
 * `Mutex::LockImpl`、`ConditionVariable::Wait`、`WaitGroup::Wait` 在 park 前构造它：
 * Context 被取消时，回调会 `TryCancelAndWake` 认领等待节点，让等待方从 park 返回
 * 并看到 `kCancelled`。没有它，取消一个正在等锁的 G 只能等锁自己释放。
 *
 * ## 为什么回调捕获 `weak_ptr` + `generation`
 *
 * - `weak_ptr`：等待节点可能已被回收并复用到下一次等待，强引用会延长它的生命周期；
 * - `generation`：节点被复用时 `m_generation` 会 +1，回调据此拒绝「上一轮」的取消 ——
 *   否则一个迟到的取消会把下一次等待误杀。
 *
 * ## RAII
 *
 * 析构调 `Reset()` 注销回调。不注销虽然靠 `weak_ptr` + generation 也不会出错，
 * 但会让 Context 一直持有回调（浪费内存），且每次取消都要多跑一次空回调。
 */
class ContextSubscription final {
public:
    ContextSubscription(const ContextPtr& context,
                        const std::shared_ptr<WaitNode>& waiter)
        : m_context(context) {
        if (!m_context || !waiter) {
            return;
        }
        const std::weak_ptr<WaitNode> weak_waiter(waiter);
        const std::uint64_t generation = waiter->generation();
        m_id = m_context->Done().AddCallback([weak_waiter, generation] {
            const auto current = weak_waiter.lock();
            if (current) {
                (void)current->TryCancelAndWake(generation);
            }
        });
    }

    ContextSubscription(const ContextSubscription&) = delete;
    ContextSubscription& operator=(const ContextSubscription&) = delete;

    ~ContextSubscription() { Reset(); }

    void Reset() noexcept {
        if (m_context && m_id != 0) {
            m_context->Done().RemoveCallback(m_id);
        }
        m_id = 0;
        m_context.reset();
    }

private:
    ContextPtr m_context;
    DoneSignal::CallbackId m_id{0};
};

/**
 * @brief RAII 取消守卫：析构时调用 `CancelFunc`（句柄为空则什么都不做）。
 *
 * ## 定位
 *
 * `ConditionVariable::WaitFor`、`WaitGroup::WaitFor`、`Timer` 的超时路径用它：
 * 先 `TimeoutContext(...)` 拿到一个带取消的 Context，再用本守卫保证**函数任何
 * 退出路径**（正常返回、抛异常、提前 `return`）都会取消那个临时 Context ——
 * 否则它会一直活着、其定时器不释放，且 `Context` 的取消树多一个孤儿节点。
 *
 * ## 为什么 `CancelFunc` 可以为空
 *
 * `TimeoutContext` 在「没有超时」的分支返回空 cancel，此时守卫退化为无操作；
 * 调用方因此不需要在每条路径上写 `if (cancel) cancel();`。
 */
class CancelGuard final {
public:
    explicit CancelGuard(CancelFunc cancel) : m_cancel(std::move(cancel)) {}
    ~CancelGuard() {
        if (m_cancel) {
            m_cancel();
        }
    }

    CancelGuard(const CancelGuard&) = delete;
    CancelGuard& operator=(const CancelGuard&) = delete;

private:
    CancelFunc m_cancel;
};

// cleanup 重锁路径没有任何取消源可以中断它，因此必须给出明确的
// 退出预算：park 连续被状态机拒绝说明执行边界已经异常，继续循环只会
// 把"永久挂起"换成"永久自旋"。预算取较大值，正常路径永远是 0 次。
constexpr std::size_t kMaxCleanupParkRetries = 4096;

WaitResult Await(WaitNode* waiter, bool cleanup_relock = false) noexcept {
    if (!waiter) {
        return WaitResult::kCancelled;
    }

    const auto finish = [waiter](WaitResult result) {
        waiter->Disarm();
        return result;
    };
    if (!waiter->managed()) {
        // 普通线程调用者读取相同的结果状态，但在 condition_variable 上休眠；
        // 这样阻塞的 OS 线程不会占用 GMP worker，同时保持先通知后等待的语义。
        return finish(waiter->WaitNative());
    }

    // managed G 从不等待原生 condition_variable：park 会释放其 M，Scheduler
    // 的唤醒路径会重新排入同一个逻辑 G。
    Scheduler* const scheduler = waiter->scheduler();
    const auto& task = waiter->task();
    std::size_t cleanup_retries = 0;
    for (;;) {
        // Unlock() 可以在 waiter 刚发布、但尚未真正调用 park() 的窗口内
        // 完成交接。此时 Wake 已把结果设为 Notified；若仍然进入
        // park_with_reason()，会无谓获取 Scheduler admission 锁，短临界区
        // 的每次交接都会被放大成一次调度慢路径。先读取终态还能保留
        // notify-before-park 语义：结果已经线性化时无需再次挂起。
        const WaitResult published = waiter->result();
        if (published != WaitResult::kWaiting) {
            return finish(published);
        }
        if (!cleanup_relock && task->cancellation_requested()) {
            if (waiter->result() == WaitResult::kWaiting) {
                (void)waiter->TryFinish(WaitResult::kCancelled);
            }
            return finish(waiter->result());
        }

        // 发布后始终尝试 park，即使通知者可能已经先一步完成。先通知后 park
        // 的唤醒会保存为 pending token；park 消耗该 token 后直接返回，不会挂起。
        const bool suspended = cleanup_relock
            ? waiter->backend()->ParkForCleanup(
                  scheduler, std::static_pointer_cast<void>(task),
                  [waiter] { return waiter->result() != WaitResult::kWaiting; })
            : waiter->backend()->Park(
                  scheduler, std::static_pointer_cast<void>(task), std::nullopt,
            [waiter, task] {
                return waiter->result() != WaitResult::kWaiting ||
                       task->cancellation_requested();
            });
        const WaitResult result = waiter->result();
        if (result != WaitResult::kWaiting) {
            return finish(result);
        }
        if (!cleanup_relock && (task->cancellation_requested() ||
            Scheduler::current_scheduler() != scheduler ||
            Scheduler::current_task().get() != task.get() ||
            (!suspended && !scheduler->is_running()))) {
            (void)waiter->TryFinish(WaitResult::kCancelled);
            return finish(waiter->result());
        }
        if (cleanup_relock) {
            // 到这里 result 仍是 kWaiting、park 也没能挂起当前 G。本路径
            // 没有任何取消源可以打断它，必须自己给出退出条件，否则就是
            // 一个不烧 CPU 也永不返回（或纯自旋）的死循环。
            if (Scheduler::current_scheduler() != scheduler ||
                Scheduler::current_task().get() != task.get()) {
                // 执行上下文已经丢失，继续 park 没有意义。
                (void)waiter->TryFinish(WaitResult::kCancelled);
                return finish(waiter->result());
            }
            if (!suspended) {
                if (++cleanup_retries > kMaxCleanupParkRetries) {
                    // 状态机持续拒绝挂起：快速失败，而不是把整个
                    // Scheduler::shutdown() 拖成无限等待。
                    (void)waiter->TryFinish(WaitResult::kCancelled);
                    return finish(waiter->result());
                }
                // 被拒绝时让出一次执行权，避免用纯自旋烧满一个 M。
                (void)scheduler->yield_current();
            } else {
                cleanup_retries = 0;
            }
        }
        // false park 可能消耗无关的 pending permit。该 permit 和无关的 unpark
        // 都属于伪唤醒，不代表取消。
    }
}

template <typename Queue, typename Waiter>
void RemoveWaiter(Queue& waiters, const Waiter& waiter) noexcept {
    const auto position = std::find(waiters.begin(), waiters.end(), waiter);
    if (position != waiters.end()) {
        waiters.erase(position);
    }
}

ContextPtr TimeoutContext(ContextDuration timeout, const ContextPtr& parent,
                          CancelFunc* cancel) {
    auto pair = WithTimeout(parent ? parent : Background(), timeout);
    *cancel = std::move(pair.second);
    return std::move(pair.first);
}

}  // namespace

/// 函数功能：注册同步等待后端工厂，隔离具体 Scheduler/Fiber 实现。
/// 执行流程：在短临界区内替换工厂快照；实际工厂调用始终发生在锁外。
/// @param[in] factory 新建等待节点使用的后端工厂。
/// @return 成功发布返回 true；复制 std::function 失败返回 false。
/// @note 已存在等待节点继续使用原后端，不会被强制迁移。

namespace {

/// 全局选择器工厂（与 WaitBackend 工厂同风格）。默认空 = 纯 FIFO。
std::mutex s_selector_mutex;
std::shared_ptr<WaitSelector> s_selector;
// 快速判空标志：绝大多数程序**从不**注册选择器，而 Unlock 的慢路径在被多线程争用时
// 会被频繁执行。若不先做一次原子读，每次都要加 s_selector_mutex 并拷贝 shared_ptr
// （约 30~60 ns）。有了这个标志，未注册时热路径只剩**一次原子读**，做到真正零开销。
std::atomic<bool> s_has_selector{false};

/// 取当前选择器（无锁快路径：先用原子标志判断是否可能为空）。
std::shared_ptr<WaitSelector> CurrentWaitSelector() noexcept {
    try {
        std::lock_guard<std::mutex> lock(s_selector_mutex);
        return s_selector;
    } catch (...) {
        return {};
    }
}

/// 让选择器重排等待队列：**只把被选中的节点换到队首**，不改认领语义。
/// @note 该函数在持队列锁时调用；异常与越界一律退化为不干预。
void ReorderWaitersForSelector(std::deque<std::shared_ptr<WaitNode>>& waiters) {
    if (waiters.size() < 2U) {
        return;
    }
    // 零开销快路径：没有注册过选择器时只读一次原子量（不取锁、不拷 shared_ptr）。
    if (!s_has_selector.load(std::memory_order_acquire)) {
        return;
    }
    auto selector = CurrentWaitSelector();
    if (!selector) {
        return;
    }
    std::vector<WaiterCandidate> candidates;
    candidates.reserve(waiters.size());
    for (std::size_t index = 0; index < waiters.size(); ++index) {
        candidates.push_back(WaiterCandidate{index, waiters[index] && waiters[index]->managed()});
    }
    std::optional<std::size_t> chosen;
    try {
        chosen = selector->Select(candidates);
    } catch (...) {
        return;   // 策略异常必须被隔离，退化为默认 FIFO
    }
    if (!chosen.has_value() || *chosen == 0U || *chosen >= waiters.size()) {
        return;   // 不干预或下标非法
    }
    std::swap(waiters[0], waiters[*chosen]);
}

}  // namespace

bool SetWaitSelectorFactory(WaitSelectorFactory factory) {
    try {
        std::shared_ptr<WaitSelector> selector;
        if (factory) {
            selector = factory();
        }
        std::lock_guard<std::mutex> lock(s_selector_mutex);
        s_selector = std::move(selector);
        // 先发布对象、再置标志，保证读到 true 的线程一定能看到非空选择器。
        s_has_selector.store(s_selector != nullptr, std::memory_order_release);
        return true;
    } catch (...) {
        return false;
    }
}

void ResetWaitSelectorFactory() noexcept {
    // 先清标志再清对象：之后的 Unlock 走零开销快路径，不会再去加锁。
    s_has_selector.store(false, std::memory_order_release);
    std::lock_guard<std::mutex> lock(s_selector_mutex);
    s_selector.reset();
}

bool SetWaitBackendFactory(WaitBackendFactory factory) {
    try {
        auto next = std::make_shared<const WaitBackendFactory>(
            std::move(factory));
        std::lock_guard<std::mutex> lock(s_wait_backend_mutex);
        s_wait_backend_factory = std::move(next);
        return true;
    } catch (...) {
        return false;
    }
}

/// 函数功能：恢复默认 Scheduler 同步等待适配器。
/// @return 无返回值。
/// @note 只影响后续创建的等待节点。
void ResetWaitBackendFactory() noexcept {
    try {
        std::shared_ptr<const WaitBackendFactory> old;
        {
            std::lock_guard<std::mutex> lock(s_wait_backend_mutex);
            old.swap(s_wait_backend_factory);
        }
        // 在锁外销毁用户工厂，避免析构函数重入注册接口时死锁。
        old.reset();
    } catch (...) {
    }
}

/// 函数功能：创建同步等待节点的后端实例。
/// @return 非空后端；用户工厂异常或返回空指针时回退 SchedulerWaitBackend。
/// @note 用户工厂在锁外执行，避免注册函数重入造成锁反转。
std::shared_ptr<WaitBackend> CreateWaitBackend() noexcept {
    // 默认后端无状态。此前每个等待节点都重新分配一个
    // SchedulerWaitBackend 控制块，短锁竞争中这部分分配成本会超过
    // 临界区本身；共享一个长期存活实例，仍保留自定义工厂的插件边界。
    static const std::shared_ptr<WaitBackend> s_default_backend =
        std::make_shared<SchedulerWaitBackend>();
    std::shared_ptr<const WaitBackendFactory> factory_snapshot;
    try {
        {
            std::lock_guard<std::mutex> lock(s_wait_backend_mutex);
            factory_snapshot = s_wait_backend_factory;
        }
        if (factory_snapshot && *factory_snapshot) {
            if (auto backend = (*factory_snapshot)()) {
                return backend;
            }
        }
    } catch (...) {
    }
    return s_default_backend;
}

/**
 * @brief `sync::Mutex` 的共享实现（PIMPL 隐藏；`Mutex` 本身只持 `unique_ptr<Impl>`）。
 *
 * ## 定位
 *
 * 所有 `sync::Mutex` 实例共享这套语义；`SpinMutex` 也**复用它**（只额外维护
 * `no_park_depth` 契约计数），因此这里的每条不变量同时约束两种锁。上层使用者：
 * `sync::ConditionVariable`/`WaitGroup`、`channel`、`future`、`IOManager` 的等待节点。
 *
 * ## 状态字（`m_state` 四个位，必须一起理解）
 *
 * | 位 | 含义 |
 * |---|---|
 * | `kLocked` | 锁已被持有 |
 * | `kWaiter` | 等待队列非空（仅用于诊断与「解锁必须进慢路径」的提示） |
 * | `kStarving` | 饥饿模式：解锁必须把所有权直接交给 FIFO 队首 |
 * | `kWoken` | 正常模式下已唤醒一个等待者，它尚未获取/重新入队 |
 *
 * **正常模式（Go `sync.Mutex` 的 mutexNormal）**：`Unlock` 先把锁真正释放
 * （清 `kLocked`），再唤醒一个等待者；被唤醒者必须**重新竞争**，新来者也可
 * 用 CAS 插队。`kWoken` 是「唤醒令牌」：同一时刻最多只有一个被唤醒的等待者，
 * 它获取锁或重新入队时必须消费令牌，避免每次 `Unlock` 都做一次 futex wake
 * 的唤醒风暴。
 *
 * **饥饿模式（mutexStarving）**：某个等待者从真正入队起累计等待超过
 * `kStarvationThresholdNs`（1ms）后，在（再次）入队时置 `kStarving`。此后
 * `Unlock` **不释放锁**，而是把 `kLocked` 的所有权直接交给 FIFO 队首
 * （`WaitNode::TryFinishAndGrant`），新来者不得插队、只能入队。退出条件：
 * 队列清空，或被交接者自身等待未超过阈值（说明积压已在阈值内消化）。
 *
 * **不变量**：`kWaiter` 与队列非空在 `m_mutex` 下同步维护；`kWoken` 被
 * 置位时，该等待者已从队列摘除且不在队列里。所有状态字的修改要么在
 * `m_mutex` 内，要么是只影响 `kLocked` 的 CAS（会保留其余提示位）。
 *
 * ## 队列与唤醒
 *
 * `m_waiters` 是 FIFO 且**存 `shared_ptr`**：`Unlock` 先从队列取出等待者、再在
 * 释放队列锁后 `Wake`；若存裸指针，等待方可能在这段窗口内返回并销毁栈上的
 * `WaitNode`（或被缓存节点复用），形成悬空访问。`shared_ptr` 把节点生命周期延长
 * 到 `Wake` 完成，同时保持「锁外唤醒」避免锁反转。
 *
 * ## 线程模型
 *
 * 队列操作都在 `m_mutex` 内；状态位用原子操作在锁外快路径读。受管 G 竞争时
 * **直接 park**（不做 yield 重试），普通线程走 `WaitNative` + 自旋预算。
 */
struct Mutex::Impl {
    std::mutex m_mutex;
#if GO2CPP_DEBUG_RESOURCES
    /// @brief 资源级历史编号（构造时注册；OFF 构建不存在该成员）。
    ::go2cpp::debug::ResourceId m_debug_resource{0};
#endif
    // 同一个原子字节同时发布锁状态、等待者提示、饥饿模式与唤醒令牌。
    // 关键点：正常模式下 Unlock 清 kLocked 后新来者仍能 CAS 拿锁；只有
    // kStarving 会关闭快路径（饥饿模式禁止插队）。
    static constexpr std::uint8_t kLocked = 1U;
    static constexpr std::uint8_t kWaiter = 2U;
    static constexpr std::uint8_t kStarving = 4U;
    static constexpr std::uint8_t kWoken = 8U;
    std::atomic<std::uint8_t> m_state{0U};
    // 只统计已经进入等待队列的普通线程。受管 Fiber 不参与该计数：
    // 它们竞争时直接 park（不再做协作 yield 重试，见 LockImpl 的说明）。
    std::atomic<std::size_t> m_native_waiters{0U};
    // 队列必须持有 shared_ptr。Unlock 会先从队列取出等待者，再在释放
    // 队列锁后调用 Wake；如果队列只保存裸指针，等待方可能在这段窗口
    // 内返回并销毁栈上的 WaitNode，或把缓存节点重置给下一次等待，
    // 从而形成悬空访问。队列持有的引用把节点生命周期延长到 Wake
    // 完成，同时不改变锁外唤醒以避免锁反转的约束。
    std::deque<std::shared_ptr<WaitNode>> m_waiters;
    // 最近一次成功加锁的受管 G（持锁 park 检测用；0 表示未记录）。
    std::atomic<std::uint64_t> m_owner{0};

    /**
     * @brief 唤醒令牌持有者退出（取消 / context done）时把令牌转交出去。
     *
     * 令牌持有者已从队列摘除；它既不获取锁也不重新入队时，必须：
     * - 清 `kWoken`，否则后续 `Unlock` 会一直认为「唤醒已在路上」而不再唤醒；
     * - 若此刻锁空闲且仍有等待者，补一次唤醒（正常模式）或直接交接
     *   （饥饿模式），否则等待者会永久睡在已经空闲的锁上。
     *
     * 必须在**不持** `m_mutex` 时调用；`Wake` 在锁外执行。
     */
    void DropWakeToken() noexcept {
        std::shared_ptr<WaitNode> selected;
        try {
            std::lock_guard<std::mutex> lock(m_mutex);
            std::uint8_t state = m_state.load(std::memory_order_acquire);
            state = static_cast<std::uint8_t>(state & ~kWoken);
            ReorderWaitersForSelector(m_waiters);
            const bool starved = (state & kStarving) != 0U;
            if (m_waiters.empty()) {
                state = static_cast<std::uint8_t>(state & ~kWaiter);
                // 没有排队者，也没有可交接对象：空闲时把状态清干净
                // （含遗留的 kStarving）。
                if ((state & kLocked) == 0U) {
                    state = 0U;
                }
                m_state.store(state, std::memory_order_release);
                return;
            }
            if ((state & kLocked) != 0U) {
                // 锁在别人手里：它释放时会看到 kWoken 已清而唤醒下一个。
                m_state.store(state, std::memory_order_release);
                return;
            }
            // 锁空闲 + 仍有等待者：令牌持有者退出了，必须由这里补交接。
            while (!m_waiters.empty()) {
                std::shared_ptr<WaitNode> candidate =
                    std::move(m_waiters.front());
                m_waiters.pop_front();
                if (candidate && !candidate->managed()) {
                    m_native_waiters.fetch_sub(1U,
                                               std::memory_order_release);
                }
                const bool claimed =
                    starved ? candidate && candidate->TryFinishAndGrant()
                            : candidate &&
                                  candidate->TryFinish(WaitResult::kNotified);
                if (claimed) {
                    selected = std::move(candidate);
                    break;
                }
            }
            if (selected) {
                if (starved) {
                    state = static_cast<std::uint8_t>(state | kLocked | kWaiter);
                } else {
                    state = static_cast<std::uint8_t>(state | kWoken | kWaiter);
                }
                if (m_waiters.empty()) {
                    state = static_cast<std::uint8_t>(state & ~kWaiter);
                }
                m_state.store(state, std::memory_order_release);
            } else {
                m_state.store(0U, std::memory_order_release);
            }
        } catch (...) {
            // 极端情况下（系统级 mutex 失败）至少别把令牌永久留在状态里。
            m_state.fetch_and(static_cast<std::uint8_t>(~kWoken),
                              std::memory_order_release);
        }
        if (selected) {
            selected->Wake();
        }
    }
};

Mutex::Mutex() : m_impl(std::make_unique<Impl>()) {
#if GO2CPP_DEBUG_RESOURCES
    m_impl->m_debug_resource = GO2CPP_DEBUG_RESOURCE_REGISTER(
        kMutex, this, nullptr);
#endif
}

MutexDebugSnapshot Mutex::DebugSnapshotInternal() const {
    MutexDebugSnapshot snapshot;
    const std::uint8_t state = m_impl->m_state.load(std::memory_order_acquire);
    snapshot.held = (state & Impl::kLocked) != 0U;
    snapshot.has_waiters = (state & Impl::kWaiter) != 0U;
    snapshot.state_bits = state;
    snapshot.locked = (state & Impl::kLocked) != 0U;
    snapshot.waiter_flag = (state & Impl::kWaiter) != 0U;
    snapshot.starving = (state & Impl::kStarving) != 0U;
    snapshot.woken = (state & Impl::kWoken) != 0U;
    snapshot.owner_gid = m_impl->m_owner.load(std::memory_order_relaxed);
    snapshot.owner_known = snapshot.owner_gid != 0U;
    snapshot.native_waiters = m_impl->m_native_waiters.load(std::memory_order_acquire);
    {
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        snapshot.waiters = m_impl->m_waiters.size();
    }
    // fiber_waiters 由总量减去普通线程数；两者都来自同一把队列锁下的观测，
    // 因此不会出现 native > total 的情形（除非并发中队列正在变化，此时 clamp）。
    snapshot.fiber_waiters = snapshot.waiters > snapshot.native_waiters
                                 ? snapshot.waiters - snapshot.native_waiters
                                 : 0U;
    return snapshot;
}

Mutex::~Mutex() {
#if GO2CPP_DEBUG_RESOURCES
    GO2CPP_DEBUG_RESOURCE_UNREGISTER(m_impl->m_debug_resource);
#endif
}

bool Mutex::Lock(const ContextPtr& context) {
    return LockImpl(context, false);
}

bool Mutex::LockForConditionWait() {
    return LockImpl({}, true);
}

/**
 * @brief Mutex 的加锁实现（`Lock` 与 `LockForConditionWait` 的共同落点）。
 * @param context 可选的取消上下文；已失效时直接返回 false。
 * @param ignore_task_cancellation 条件变量内部加锁时传 true，避免把
 *        「等待条件」误判成「任务已取消」。
 * @return true 表示已持有锁；false 表示失败/被取消（都不会持锁返回）。
 *
 * ## 定位
 *
 * `Mutex::lock` / `try_lock` / `ConditionVariable::Wait` 都收敛到这里。
 * 它是**混合锁**：无竞争走一次 CAS；受管 G 竞争时直接 park（把 M 让给
 * 别的 G）；普通线程竞争时先用很短的有界自旋吸收极短临界区，然后进入
 * FIFO 等待队列（等待节点在 `WaitNative` 里还有一层自旋，见那里）。
 *
 * ## 两态状态机（正常模式 / 饥饿模式）
 *
 * - **正常模式**：`Unlock` 先释放锁（清 `kLocked`）再唤醒一个等待者；被
 *   唤醒者必须回到本函数顶部重新 CAS 竞争，新来者也可以插队。`kWoken`
 *   令牌保证同一时刻最多一个被唤醒者，避免每次解锁一次 futex 唤醒的
 *   唤醒风暴。
 * - **饥饿模式**：某个等待者入队后累计等待超过 1ms，就在（再次）入队时
 *   置 `kStarving`。此后 `Unlock` 不释放锁，而是把所有权直接交给 FIFO
 *   队首（`WaitNode::TryFinishAndGrant`），新来者不得插队。被交接者若
 *   自身等待未超过阈值，会在返回前清掉 `kStarving`，避免一次调度抖动
 *   让整把锁永久停在 futex 交接模式；队列清空也会退出饥饿模式。
 *
 * ## 步骤
 *
 * step1. 上下文已失效 → 返回 false（先判，避免拿到锁却无法返回）；
 * step2. **快速路径**：状态没有 `kLocked/kStarving` 时一次 CAS 拿锁；
 * step3. 判定等待者类型：受管 G / 普通线程 / 手动 Fiber；手动 Fiber 无法
 *        park，直接返回 false；
 * step4. **普通线程**：短自旋（`kNativeSpinAttempts`）后进入等待队列；
 * step5. 在 `m_mutex` 下发布 `kWaiter`（必要时 `kStarving`）并 push；
 * step6. 等待结束：被普通唤醒 → 回到 step2 重新竞争；被饥饿模式交接 →
 *        直接持锁返回；超时/取消 → 从队列摘除且不持锁返回。
 *
 * ## 并发/失败语义
 *
 * - **快速路径只有一次 CAS**，不加锁、不碰队列；
 * - **受管 G 竞争时直接 park，不自旋也不 yield**——实测自旋只会更慢；
 * - **不持锁返回 false**：任何失败路径都在拿到锁之前返回；
 * - 正常模式下被唤醒者重新竞争，因此**不保证**严格的唤醒即持有；
 *   1ms 的饥饿模式阈值保证不会无限被插队；
 * - 等待者在队列里用 `shared_ptr` 持有——`Unlock` 是「先出队、锁外 Wake」，
 *   裸指针会让节点在 Wake 前被销毁。
 */
bool Mutex::LockImpl(const ContextPtr& context,
                     bool ignore_task_cancellation) {
    // step1: 上下文已失效时直接失败（调用方按取消处理）。
    if (context && context->IsDone()) {
        return false;
    }

    // 只在检测开启时才记录 owner/持锁数；默认关闭时只是一次 relaxed 读。
    const auto note_lock_held = [this] {
        if (!DetectHeldParkEnabled()) {
            return;
        }
        Task* const self = Scheduler::current_task_ptr();
        if (self == nullptr) {
            return;
        }
        self->note_mutex_locked();
        m_impl->m_owner.store(static_cast<std::uint64_t>(self->id()),
                              std::memory_order_relaxed);
    };

    // step2: 无竞争快速路径——先于任何等待者类型判定，保证「无竞争只有
    // 一次 CAS」，不为 CurrentTarget() 的调度器查询/共享指针拷贝付开销。
    std::uint8_t expected_state =
        m_impl->m_state.load(std::memory_order_acquire);
    if ((expected_state & (Impl::kLocked | Impl::kStarving)) == 0U) {
        if (m_impl->m_state.compare_exchange_weak(
                expected_state,
                static_cast<std::uint8_t>(expected_state | Impl::kLocked),
                std::memory_order_acquire, std::memory_order_relaxed)) {
            note_lock_held();
            GO2CPP_DEBUG_EVENT_SAMPLED(
                kMutexFastLock, GO2CPP_DEBUG_BOUND_TASK(), 0, 32);
            GO2CPP_DEBUG_RESOURCE_EVENT(
                kMutexLock, m_impl->m_debug_resource,
                GO2CPP_DEBUG_BOUND_TASK(), 0, 0,
                ::go2cpp::debug::ResourceResult::kOk, 0, false);
            return true;
        }
    }

    // step3（类型判定只做一次）: 受管 G 可 park；手动 Fiber 无法 park，
    // 只能失败；已请求取消的 G 也直接失败。
    const WaitTarget target = CurrentTarget();
    if (target.unsupported_manual_fiber) {
        return false;
    }
    if (!ignore_task_cancellation && target &&
        target.task->cancellation_requested()) {
        return false;
    }

    // 等待起点：从真正进入等待队列开始累计。超过阈值后在（再次）入队时
    // 把锁切到饥饿模式（kStarving），由 Unlock 直接按 FIFO 交接所有权。
    std::uint64_t wait_start_ns = 0U;
    // 正常模式下「唤醒令牌」的本地持有标记：被 Unlock 唤醒后置 true，
    // 获取锁/重新入队/放弃等待时必须消费它（见 Impl::DropWakeToken）。
    bool was_woken = false;

    const auto wait_exceeded_threshold = [&]() -> bool {
        return wait_start_ns != 0U &&
               SteadyNowNs() - wait_start_ns > kStarvationThresholdNs;
    };
    const auto abandon_wake_token = [&] {
        if (!was_woken) {
            return;
        }
        was_woken = false;
        m_impl->DropWakeToken();
    };

    // 争用事件：能走到这里说明快速 CAS 已失败，本次 Lock 必须排队/自旋/park。
    // 只在慢路径记录（无竞争加锁零开销）；pid 由事件环从调度器绑定读取。
    GO2CPP_DEBUG_PARK_HINT(kMutex);
    GO2CPP_DEBUG_RESOURCE_EVENT(
        kMutexLock, m_impl->m_debug_resource,
        target.task ? static_cast<std::uint64_t>(target.task->id()) : 0ULL, 0,
        0, ::go2cpp::debug::ResourceResult::kBlocked,
        m_impl->m_native_waiters.load(std::memory_order_relaxed) + 1U, true);

    for (;;) {
        // step2: 无竞争/正常模式快速路径——一次 CAS。kStarving 时禁止插队。
        expected_state = m_impl->m_state.load(std::memory_order_acquire);
        if ((expected_state & (Impl::kLocked | Impl::kStarving)) == 0U) {
            std::uint8_t desired = static_cast<std::uint8_t>(
                expected_state | Impl::kLocked);
            if (was_woken) {
                desired = static_cast<std::uint8_t>(desired & ~Impl::kWoken);
            }
            if (m_impl->m_state.compare_exchange_weak(
                    expected_state, desired, std::memory_order_acquire,
                    std::memory_order_relaxed)) {
                note_lock_held();
                GO2CPP_DEBUG_RESOURCE_EVENT(
                    kMutexLock, m_impl->m_debug_resource,
                    target.task ? static_cast<std::uint64_t>(target.task->id())
                                : 0ULL,
                    0, 0, ::go2cpp::debug::ResourceResult::kOk, 0, false);
                return true;
            }
        }

        // 被唤醒后必须先看取消：已经取消的调用不能持锁返回。
        if (context && context->IsDone()) {
            abandon_wake_token();
            return false;
        }
        if (!ignore_task_cancellation && target &&
            target.task->cancellation_requested()) {
            abandon_wake_token();
            return false;
        }

        // step4: 普通线程在正常模式下先做有界自旋（短预算，见
        // kNativeSpinAttempts 的标定表）。已有等待者时**不再**缩短预算：
        // 正常模式允许新来者与等待者公平竞争，锁被释放后自旋者可以直接
        // CAS 拿到，而不必每次操作都付一次 futex 唤醒。饥饿模式关闭这条
        // 路径（新来者不得插队）。
        if (!target && (expected_state & Impl::kStarving) == 0U) {
            for (int attempt = 0; attempt < kNativeSpinAttempts; ++attempt) {
                expected_state =
                    m_impl->m_state.load(std::memory_order_acquire);
                if ((expected_state & Impl::kStarving) != 0U) {
                    break;
                }
                if ((expected_state & Impl::kLocked) == 0U) {
                    std::uint8_t desired = static_cast<std::uint8_t>(
                        expected_state | Impl::kLocked);
                    if (was_woken) {
                        desired = static_cast<std::uint8_t>(
                            desired & ~Impl::kWoken);
                    }
                    if (m_impl->m_state.compare_exchange_weak(
                            expected_state, desired,
                            std::memory_order_acquire,
                            std::memory_order_relaxed)) {
                        note_lock_held();
                        GO2CPP_DEBUG_RESOURCE_EVENT(
                            kMutexLock, m_impl->m_debug_resource,
                            target.task
                                ? static_cast<std::uint64_t>(target.task->id())
                                : 0ULL,
                            0, 0, ::go2cpp::debug::ResourceResult::kOk, 0,
                            false);
                        return true;
                    }
                }
                RelaxCpu();
            }
        }

        if (wait_start_ns == 0U) {
            // 从“真正进入等待队列”开始计时：自旋阶段（≤kNativeSpinAttempts
            // 次 PAUSE）不计入。这样一次调度抢占（线程在用户态自旋时被换出）
            // 不会误触发饥饿模式，同时仍然保证入队后的等待有 1ms 上界。
            wait_start_ns = SteadyNowNs();
        }

        // step5: 走等待路径。每次 park 都需要一个新代际的节点（结果字段
        // 是终态），因此循环内从 thread-local 缓存取、结束后归还。
        std::shared_ptr<WaitNode> owned_waiter =
            AcquireWaitNode(target.scheduler, target.task);
        WaitNode* waiter = owned_waiter.get();
        ContextSubscription subscription(context, owned_waiter);
        bool acquired_under_lock = false;
        bool abort_before_enqueue = false;
        {
            std::lock_guard<std::mutex> lock(m_impl->m_mutex);
            if ((context && context->IsDone()) ||
                waiter->result() != WaitResult::kWaiting) {
                abort_before_enqueue = true;
            } else {
                for (;;) {
                    std::uint8_t state =
                        m_impl->m_state.load(std::memory_order_acquire);
                    // 正常模式：锁空闲即可抢占（即使队列还有等待者）。
                    // 饥饿模式：锁空闲说明没有可交接的持有者（队列已空，
                    // 或交接刚刚落地），此时直接取得锁是唯一能推进的选择。
                    if ((state & Impl::kLocked) == 0U) {
                        std::uint8_t desired = static_cast<std::uint8_t>(
                            state | Impl::kLocked);
                        desired = static_cast<std::uint8_t>(
                            desired & ~Impl::kStarving);
                        if (was_woken) {
                            desired = static_cast<std::uint8_t>(
                                desired & ~Impl::kWoken);
                        }
                        if (m_impl->m_state.compare_exchange_weak(
                                state, desired, std::memory_order_acquire,
                                std::memory_order_relaxed)) {
                            acquired_under_lock = true;
                            break;
                        }
                        continue;
                    }
                    // 先发布 kWaiter（必要时连 kStarving 一起发布），再
                    // push 队列：这样持锁者的快速 CAS 会在状态里看到提示，
                    // 不会在入队过程中把锁释放掉而漏掉唤醒。
                    std::uint8_t published = static_cast<std::uint8_t>(
                        state | Impl::kWaiter);
                    if (wait_exceeded_threshold()) {
                        published = static_cast<std::uint8_t>(
                            published | Impl::kStarving);
                    }
                    if (was_woken) {
                        // 重新入队必须消费唤醒令牌，否则后续 Unlock 会一直
                        // 以为「已有唤醒在路上」而不再叫醒任何人。
                        published = static_cast<std::uint8_t>(
                            published & ~Impl::kWoken);
                    }
                    if (m_impl->m_state.compare_exchange_weak(
                            state, published, std::memory_order_acq_rel,
                            std::memory_order_acquire)) {
                        was_woken = false;
                        break;
                    }
                }
                if (!acquired_under_lock) {
                    m_impl->m_waiters.push_back(owned_waiter);
                    if (!target) {
                        m_impl->m_native_waiters.fetch_add(
                            1U, std::memory_order_release);
                    }
                    if (!waiter->Arm()) {
                        m_impl->m_waiters.pop_back();
                        if (!target) {
                            m_impl->m_native_waiters.fetch_sub(
                                1U, std::memory_order_release);
                        }
                        if (m_impl->m_waiters.empty()) {
                            // 没有排队者就不该继续声称饥饿/有等待者。
                            std::uint8_t state = m_impl->m_state.load(
                                std::memory_order_acquire);
                            state = static_cast<std::uint8_t>(
                                state & ~(Impl::kWaiter | Impl::kStarving));
                            m_impl->m_state.store(
                                state, std::memory_order_release);
                        }
                        abort_before_enqueue = true;
                    }
                }
            }
        }

        if (acquired_under_lock) {
            subscription.Reset();
            ReleaseWaitNode(std::move(owned_waiter));
            note_lock_held();
            GO2CPP_DEBUG_RESOURCE_EVENT(
                kMutexLock, m_impl->m_debug_resource,
                target.task ? static_cast<std::uint64_t>(target.task->id())
                            : 0ULL,
                0, 0, ::go2cpp::debug::ResourceResult::kOk, 0, false);
            return true;
        }
        if (abort_before_enqueue) {
            subscription.Reset();
            ReleaseWaitNode(std::move(owned_waiter));
            // 令牌还没有被消费（发布 CAS 未发生）：转交给其他等待者。
            abandon_wake_token();
            return false;
        }

        // step6: 等待被唤醒 / 超时 / 取消。
        const bool managed_waiter = waiter->managed();
        const WaitResult result = Await(waiter, ignore_task_cancellation);
        subscription.Reset();

        if (result == WaitResult::kNotified) {
            // 饥饿模式：所有权已经直接交接，kLocked 一直保持在状态里，
            // 不能再去 CAS（那会失败并误以为自己还要等待）。
            const bool granted = waiter->granted();
            ReleaseWaitNode(std::move(owned_waiter));
            if (granted) {
                // 饥饿模式的退出条件之一（与「队列清空」并列）：被交接的
                // 等待者自身累计等待没有超过阈值，说明积压已经在阈值内被
                // 消化，立即恢复正常模式。否则一次偶发调度抖动（某个等待者
                // 恰好被换出 >1ms）会让整把锁永久停在「每次操作一次 futex
                // 交接」的高延迟模式，正是本缺陷要消除的稳态退化。
                if (!wait_exceeded_threshold()) {
                    std::uint8_t state =
                        m_impl->m_state.load(std::memory_order_acquire);
                    while ((state & Impl::kStarving) != 0U) {
                        if (m_impl->m_state.compare_exchange_weak(
                                state,
                                static_cast<std::uint8_t>(
                                    state & ~Impl::kStarving),
                                std::memory_order_acq_rel,
                                std::memory_order_acquire)) {
                            break;
                        }
                    }
                }
                note_lock_held();
                GO2CPP_DEBUG_RESOURCE_EVENT(
                    kMutexLock, m_impl->m_debug_resource,
                    target.task ? static_cast<std::uint64_t>(target.task->id())
                                : 0ULL,
                    0, 0, ::go2cpp::debug::ResourceResult::kOk, 0, false);
                return true;
            }
            // 正常模式：只是被叫醒，必须回到循环重新竞争。
            was_woken = true;
            continue;
        }

        // 超时/取消/调度器关闭：把自己从队列里摘掉。Unlock 也可能已经
        // 摘走并丢弃了僵尸节点，因此这里必须用 find 而不是假定仍在队首。
        {
            std::lock_guard<std::mutex> lock(m_impl->m_mutex);
            const auto position = std::find_if(
                m_impl->m_waiters.begin(), m_impl->m_waiters.end(),
                [waiter](const std::shared_ptr<WaitNode>& candidate) {
                    return candidate.get() == waiter;
                });
            if (position != m_impl->m_waiters.end()) {
                m_impl->m_waiters.erase(position);
                if (!managed_waiter) {
                    m_impl->m_native_waiters.fetch_sub(
                        1U, std::memory_order_release);
                }
            }
            if (m_impl->m_waiters.empty()) {
                std::uint8_t state =
                    m_impl->m_state.load(std::memory_order_acquire);
                state = static_cast<std::uint8_t>(state & ~Impl::kWaiter);
                // 锁空闲且没有唤醒令牌：清掉遗留的饥饿标记（饥饿模式的
                // 退出条件就是「队列清空且没有在途交接」）。
                if ((state & Impl::kLocked) == 0U &&
                    (state & Impl::kWoken) == 0U) {
                    state = 0U;
                }
                m_impl->m_state.store(state, std::memory_order_release);
            }
        }
        ReleaseWaitNode(std::move(owned_waiter));
        return false;
    }
}


bool Mutex::LockFor(ContextDuration timeout, const ContextPtr& parent) {
    if (parent && parent->IsDone()) {
        return false;
    }
    // 零或负超时的 timed lock 仍应先进行标准的立即 try-lock。如果把它转给
    // 已取消的 Context，即使 mutex 空闲也会错误失败。
    if (timeout <= ContextDuration::zero()) {
        return TryLock();
    }
    CancelFunc cancel;
    const ContextPtr context = TimeoutContext(timeout, parent, &cancel);
    CancelGuard cancel_guard(std::move(cancel));
    return Lock(context);
}

bool Mutex::TryLock() noexcept {
    // 正常模式允许插队，因此只检查「已被持有 / 饥饿模式」两位，保留
    // kWaiter/kWoken 提示位；饥饿模式下新来者不得插队（Go 语义）。
    std::uint8_t expected = m_impl->m_state.load(std::memory_order_acquire);
    for (;;) {
        if ((expected & (Impl::kLocked | Impl::kStarving)) != 0U) {
            return false;
        }
        const std::uint8_t desired =
            static_cast<std::uint8_t>(expected | Impl::kLocked);
        if (m_impl->m_state.compare_exchange_weak(
                expected, desired, std::memory_order_acquire,
                std::memory_order_relaxed)) {
            return true;
        }
    }
}


/**
 * @brief 释放 Mutex；正常模式释放并唤醒一个等待者，饥饿模式直接交接所有权。
 *
 * ## 定位
 *
 * `LockImpl` 的另一半。关键设计是「先出队、锁外 Wake」：队列操作在
 * `m_mutex` 内完成（与等待者的入队线性化），但 `Wake()` 必须在释放
 * `m_mutex` 之后调用——否则唤醒一个 Fiber 后它立刻回来抢锁，会形成
 * 「持队列锁 → 唤醒 → 等待者抢锁」的锁序反转。
 *
 * ## 两态
 *
 * - **正常模式**（状态不含 `kStarving`）：摘除 FIFO 队首并置 `kWoken`
 *   令牌，然后把 `kLocked` 真正清掉，锁外唤醒。被唤醒者必须重新竞争，
 *   新来者可以插队。若已有 `kWoken`（上一次唤醒还没被消费），只释放锁、
 *   不再唤醒第二个——这是把「每次解锁一次 futex wake」降为「每个唤醒
 *   周期一次」的关键。
 * - **饥饿模式**（状态含 `kStarving`）：不释放 `kLocked`，逐个 `TryFinish`
 *   队首直到找到一个活节点，标记 `granted` 后锁外 `Wake`；被选中者醒来
 *   即直接持有锁。队列清空时清掉 `kStarving` 并真正释放。
 *
 * ## 步骤
 *
 * step1. 持锁 park 检测：只有「解锁者就是记录里的 owner」时才递减计数；
 * step2. **快速路径**：`CAS kLocked → 0`，仅在状态恰好是 `kLocked`
 *        （无等待者/无令牌/非饥饿）时成功，不进入队列锁；
 * step3. **慢路径**（持 `m_mutex`）：确认锁确实被持有（否则抛
 *        `logic_error`），`ReorderWaitersForSelector` 重排，再按上面
 *        两态分别处理；
 * step4. 锁外 `Wake()`。
 *
 * ## 并发/失败语义
 *
 * - **解锁未持有的 mutex 抛 `logic_error`**（继续下去会让状态永久错乱）；
 * - `kWaiter` 位与 `m_waiters` 非空在 `m_mutex` 内同步维护；
 * - **唤醒在锁外**：这是与 `LockImpl` 的 park 路径配对的约定，违反它会死锁；
 * - `TryFinish` 失败的节点（已被超时/取消）会被跳过并继续找下一个，
 *   因此不会因队首是僵尸节点而丢唤醒；
 * - 正常路径不抛异常，唯一的异常是 `logic_error`（误解锁）。
 */
void Mutex::Unlock() {
    // step1: 持锁 park 检测：只有「解锁者就是记录里的 owner」时才递减，
    // 避免 Go 风格跨 G 解锁把计数减错。
    if (DetectHeldParkEnabled()) {
        Task* const self = Scheduler::current_task_ptr();
        if (self != nullptr &&
            m_impl->m_owner.load(std::memory_order_relaxed) ==
                static_cast<std::uint64_t>(self->id())) {
            self->note_mutex_unlocked();
            m_impl->m_owner.store(0U, std::memory_order_relaxed);
        }
    }
    // 无等待者/无令牌/非饥饿时只需一次 CAS 真正释放锁，不进入队列互斥量。
    // step2: 正常模式快速路径——一次 CAS，不进队列锁。
    std::uint8_t expected_state = Impl::kLocked;
    if (m_impl->m_state.compare_exchange_strong(
            expected_state, 0U, std::memory_order_release,
            std::memory_order_acquire)) {
        GO2CPP_DEBUG_RESOURCE_EVENT(
            kMutexUnlock, m_impl->m_debug_resource, GO2CPP_DEBUG_BOUND_TASK(),
            0, 0, ::go2cpp::debug::ResourceResult::kOk, 0, false);
        return;
    }

    // step3: 慢路径——队列操作在 m_mutex 内与等待者入队线性化。
    std::shared_ptr<WaitNode> selected;
    std::uint32_t waiters_after = 0;
    {
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        std::uint8_t state = m_impl->m_state.load(std::memory_order_acquire);
        if ((state & Impl::kLocked) == 0U) {
            throw std::logic_error(
                "go2cpp::sync::Mutex unlock of unlocked mutex");
        }
        // D3：先让选择器重排队列（只换位置，不改认领语义）。
        ReorderWaitersForSelector(m_impl->m_waiters);

        if ((state & Impl::kStarving) != 0U) {
            // 饥饿模式：kLocked 不释放，所有权直接交给 FIFO 队首。
            // 新来者看到 kStarving 只能入队，不能插队。
            while (!m_impl->m_waiters.empty()) {
                std::shared_ptr<WaitNode> candidate =
                    std::move(m_impl->m_waiters.front());
                m_impl->m_waiters.pop_front();
                if (candidate && !candidate->managed()) {
                    m_impl->m_native_waiters.fetch_sub(
                        1U, std::memory_order_release);
                }
                if (candidate && candidate->TryFinishAndGrant()) {
                    selected = std::move(candidate);
                    break;
                }
            }
            if (selected) {
                state = static_cast<std::uint8_t>(state & ~Impl::kWoken);
                if (m_impl->m_waiters.empty()) {
                    state = static_cast<std::uint8_t>(state & ~Impl::kWaiter);
                } else {
                    state = static_cast<std::uint8_t>(state | Impl::kWaiter);
                }
                m_impl->m_state.store(state, std::memory_order_release);
            } else {
                // 队列已空：饥饿模式结束，真正释放锁并回到正常模式。
                m_impl->m_state.store(0U, std::memory_order_release);
            }
        } else if ((state & Impl::kWoken) != 0U) {
            // 正常模式下已有唤醒令牌在途：只释放锁，不再唤醒第二个，
            // 避免「每个 Unlock 一次 futex wake」的唤醒风暴。令牌持有
            // 者会获取锁或重新入队，并在那时消费令牌。
            state = static_cast<std::uint8_t>(state & ~Impl::kLocked);
            if (m_impl->m_waiters.empty()) {
                state = static_cast<std::uint8_t>(state & ~Impl::kWaiter);
            }
            m_impl->m_state.store(state, std::memory_order_release);
        } else {
            // 正常模式：先摘除一个等待者并置唤醒令牌，再把锁真正释放。
            while (!m_impl->m_waiters.empty()) {
                std::shared_ptr<WaitNode> candidate =
                    std::move(m_impl->m_waiters.front());
                m_impl->m_waiters.pop_front();
                if (candidate && !candidate->managed()) {
                    m_impl->m_native_waiters.fetch_sub(
                        1U, std::memory_order_release);
                }
                if (candidate && candidate->TryFinish(WaitResult::kNotified)) {
                    selected = std::move(candidate);
                    break;
                }
            }
            if (selected) {
                state = static_cast<std::uint8_t>(
                    (state & ~Impl::kLocked) | Impl::kWoken);
                if (m_impl->m_waiters.empty()) {
                    state = static_cast<std::uint8_t>(state & ~Impl::kWaiter);
                } else {
                    state = static_cast<std::uint8_t>(state | Impl::kWaiter);
                }
                m_impl->m_state.store(state, std::memory_order_release);
            } else {
                m_impl->m_state.store(0U, std::memory_order_release);
            }
        }
        waiters_after = static_cast<std::uint32_t>(m_impl->m_waiters.size());
    }
    // step4: 锁外唤醒（与 LockImpl 的 park 路径配对；持队列锁唤醒会死锁）。
    if (selected) {
#if GO2CPP_DEBUG_RESOURCES
        const Task* const unlocker = Scheduler::current_task_ptr();
        GO2CPP_DEBUG_EVENT(kMutexUnlock,
                           unlocker != nullptr ? unlocker->id() : 0, 1);
        GO2CPP_DEBUG_RESOURCE_EVENT(
            kMutexUnlock, m_impl->m_debug_resource,
            unlocker != nullptr ? static_cast<std::uint64_t>(unlocker->id())
                                : 0ULL,
            selected->gid(), 0, ::go2cpp::debug::ResourceResult::kWake,
            waiters_after, true);
#endif
        GO2CPP_DEBUG_WAKE_HINT(kMutex);
        selected->Wake();
    } else {
        GO2CPP_DEBUG_RESOURCE_EVENT(
            kMutexUnlock, m_impl->m_debug_resource, GO2CPP_DEBUG_BOUND_TASK(),
            0, 0, ::go2cpp::debug::ResourceResult::kOk, waiters_after, false);
    }
}


void Mutex::lock() {
    if (!Lock()) {
        throw std::logic_error(
            "go2cpp::sync::Mutex lock was cancelled or scheduler unavailable");
    }
}

/**
 * @brief `sync::ConditionVariable` 的共享实现（CV 本身无状态，等待队列在这里）。
 *
 * ## 定位
 *
 * 语义与 `std::condition_variable` 对齐：`Wait` 必须持外部 `Mutex`，返回时
 * 外部锁**重新被持有**；`NotifyOne/NotifyAll` 不要求持锁。
 * 上层使用者：`channel`、`future`、`parking_condition` 的线程路径、`timer`。
 *
 * ## 与 `Mutex::Impl::m_waiters` 的区别（很容易混）
 *
 * - `Mutex::Impl::m_waiters` 是**等锁**的队列，由 `Unlock` 做 FIFO 交接；
 * - 本队列是**等通知**的队列，由 `NotifyOne/NotifyAll` 唤醒，与锁的交接无关。
 *
 * 两者都用 `shared_ptr<WaitNode>` 保存，理由相同：唤醒发生在锁外，
 * 裸指针会让等待方在窗口内销毁节点。
 */

namespace {

/// `ConditionVariable::Wait` 重锁失败的可观测计数（D3，2026-10-20）。
///
/// 每 +1 表示 Wait 已经释放调用方的 mutex，却**没能重新持有**它。旧实现
/// 这时直接 return false，而 Wait 的契约要求「返回 false 时 mutex 仍被
/// 持有」；上层（RWMutex）据此对内部锁 Unlock() / 修改受保护容器，
/// 在未持锁状态下就是 UB + 计数泄漏。
std::atomic<std::uint64_t> g_cv_relock_failures{0};

/// 无 park 重试预算。m_state 这类内部锁的临界区只有若干字段更新，正常
/// 几次 TryLock 就能取回；预算留足跨核缓存行迁移的延迟。
constexpr std::size_t kConditionRelockRetryLimit = 8192;

/// @brief 条件变量重锁失败后，用不进入等待队列的 TryLock 抢回外部锁。
/// @return true 表示已重新持有 mutex；false 表示预算内没抢到。
/// @note 只用于调度器关闭 / G 已取消导致无法 park 的路径。此时锁通常空闲；
///       抢不到说明持锁者本身也被挂起，继续等只会把「明确失败」变成挂死。
bool TryRelockWithoutParking(Mutex& mutex) noexcept {
    for (std::size_t attempt = 0; attempt < kConditionRelockRetryLimit;
         ++attempt) {
        if (mutex.TryLock()) {
            return true;
        }
        RelaxCpu();
        if ((attempt & 63U) == 63U) {
            std::this_thread::yield();
        }
    }
    return false;
}

/// @brief 重锁彻底失败：计数 + 前若干次打诊断 + 抛异常（不持锁）。
/// @note 抛 std::runtime_error 而不是 std::logic_error，让上层
///       （RWMutex）能把「锁已丢失」与「调用方用法错误」区分开。
[[noreturn]] void ThrowConditionRelockFailure() {
    const std::uint64_t count =
        g_cv_relock_failures.fetch_add(1, std::memory_order_relaxed) + 1U;
    if (count <= 8U) {
        std::fprintf(stderr,
                     "[go2cpp][sync][D3] ConditionVariable::Wait failed to "
                     "reacquire its mutex (count=%llu); throwing, mutex NOT "
                     "held\n",
                     static_cast<unsigned long long>(count));
        std::fflush(stderr);
    }
    throw std::runtime_error(
        "go2cpp::sync::ConditionVariable::Wait: failed to reacquire the "
        "mutex after waking (scheduler shut down while another waiter held "
        "it); the mutex is NOT held");
}

}  // namespace

struct ConditionVariable::Impl {
    /// @brief 保护 m_waiters；Wait 入队、NotifyOne/NotifyAll 出队。
    std::mutex m_mutex;
#if GO2CPP_DEBUG_RESOURCES
    ::go2cpp::debug::ResourceId m_debug_resource{0};
#endif
    /// @brief 通知等待队列（FIFO）；存 shared_ptr 以便锁外 Wake 时节点仍存活。
    std::deque<std::shared_ptr<WaitNode>> m_waiters;
};

ConditionVariable::ConditionVariable() : m_impl(std::make_unique<Impl>()) {
#if GO2CPP_DEBUG_RESOURCES
    m_impl->m_debug_resource =
        GO2CPP_DEBUG_RESOURCE_REGISTER(kConditionVariable, this, nullptr);
#endif
}

ConditionVariableDebugSnapshot ConditionVariable::DebugSnapshotInternal() const {
    ConditionVariableDebugSnapshot snapshot;
    std::lock_guard<std::mutex> lock(m_impl->m_mutex);
    snapshot.waiters = m_impl->m_waiters.size();
    return snapshot;
}

ConditionVariable::~ConditionVariable() {
#if GO2CPP_DEBUG_RESOURCES
    GO2CPP_DEBUG_RESOURCE_UNREGISTER(m_impl->m_debug_resource);
#endif
}

bool ConditionVariable::Wait(Mutex& mutex, const ContextPtr& context) {
    // 必须先识别手动 Fiber。即使 Context 已取消，也不能走普通线程的
    // unlock/relock 路径，否则竞争中的重锁会阻塞 carrier 线程。
    const WaitTarget target = CurrentTarget();
    const auto abort_wait = [this, &mutex](bool preserve_lock) {
#if GO2CPP_DEBUG_RESOURCES
        GO2CPP_DEBUG_RESOURCE_EVENT_V5(
            kCvWait, m_impl->m_debug_resource, GO2CPP_DEBUG_BOUND_TASK(), 0, 0,
            ::go2cpp::debug::ResourceResult::kCancelled, 0, true, 0, -1, 0,
            0);
#endif
        // 所有可调度调用者统一走 cleanup 重锁。它忽略 G 的取消标记，
        // 但仍可让出当前 G 等待 owner 释放 mutex；只有手动 Fiber 没有
        // Scheduler continuation，因此必须保留进入 Wait 前的锁状态。
        if (!preserve_lock) {
            mutex.Unlock();
            // D3：重锁失败 = mutex 未被持有。不能把 false 返回给
            // 「假定仍持锁」的调用方；先无 park 抢回，抢不回就抛异常。
            if (!mutex.LockForConditionWait() &&
                !TryRelockWithoutParking(mutex)) {
                ThrowConditionRelockFailure();
            }
        }
        return false;
    };
    if (target.unsupported_manual_fiber) {
        return abort_wait(true);
    }
    if (context && context->IsDone()) {
        return abort_wait(false);
    }
    if (target && target.task->cancellation_requested()) {
        // 即使进入 Wait 前 G 已取消，也必须完成可调度的 cleanup 重锁，
        // 不能调用会拒绝已取消 G 的普通 Mutex::Lock。
        return abort_wait(false);
    }

    const auto waiter =
        std::make_shared<WaitNode>(target.scheduler, target.task);
    ContextSubscription subscription(context, waiter);
    bool abort_before_publish = false;
    {
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        abort_before_publish =
            (context && context->IsDone()) ||
            waiter->result() != WaitResult::kWaiting;
        if (!abort_before_publish) {
            m_impl->m_waiters.push_back(waiter);
            if (!waiter->Arm()) {
                m_impl->m_waiters.pop_back();
                abort_before_publish = true;
            }
        }
    }
    if (abort_before_publish) {
        // 释放 CV 注册表锁后再执行。Unlock 可能唤醒调用 NotifyOne 的 waiter，
        // 它必须能够检查注册表，避免锁顺序反转。
        return abort_wait(false);
    }
#if GO2CPP_DEBUG_RESOURCES
    GO2CPP_DEBUG_RESOURCE_EVENT_V5(
        kCvWait, m_impl->m_debug_resource, GO2CPP_DEBUG_BOUND_TASK(), 0, 0,
        ::go2cpp::debug::ResourceResult::kBlocked,
        static_cast<std::uint32_t>(m_impl->m_waiters.size()), true, 0, -1, 0,
        0);
#endif

    try {
        // 先发布再解锁。并发 Notify 可能在此时运行，但 Scheduler 的 pending-wake
        // 语义保证后续 park 不会丢失唤醒。
        mutex.Unlock();
    } catch (...) {
        (void)waiter->TryFinish(WaitResult::kCancelled);
        waiter->Disarm();
        subscription.Reset();
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        RemoveWaiter(m_impl->m_waiters, waiter);
        throw;
    }

    const WaitResult result = Await(waiter.get());
    subscription.Reset();
    {
        // 已取消 Context 的节点由其 owner 移除。Notify 路径会先摘除获胜节点，
        // 因而通知后执行这里同样安全。
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        RemoveWaiter(m_impl->m_waiters, waiter);
    }

    // Context 取消不会豁免条件变量的重锁规则。Scheduler 关闭不同：Mutex::Lock
    // 拒绝再次 park 已请求取消的 G，但仍可能立即获取锁。
    //
    // D3：LockForConditionWait 返回 false 表示**未持锁**，与 Wait「返回时
    // 外部 mutex 必被持有」的契约冲突；旧实现直接把 false 返回给上层，
    // RWMutex 会在未持锁状态下 Unlock / 修改受保护容器（UB + 泄漏）。
    // 先用不 park 的 TryLock 抢回（调度器关闭时锁通常空闲），抢不回则抛
    // 异常，把「锁已丢失」变成可捕获的显式错误。
    if (!mutex.LockForConditionWait() && !TryRelockWithoutParking(mutex)) {
        ThrowConditionRelockFailure();
    }
#if GO2CPP_DEBUG_RESOURCES
    GO2CPP_DEBUG_RESOURCE_EVENT_V5(
        kCvWait, m_impl->m_debug_resource, GO2CPP_DEBUG_BOUND_TASK(), 0, 0,
        result == WaitResult::kNotified
            ? ::go2cpp::debug::ResourceResult::kOk
            : ::go2cpp::debug::ResourceResult::kCancelled,
        0, true, 0, -1, 0, 0);
#endif
    return result == WaitResult::kNotified;
}

bool ConditionVariable::WaitFor(Mutex& mutex, ContextDuration timeout,
                                const ContextPtr& parent) {
    CancelFunc cancel;
    const ContextPtr context = TimeoutContext(timeout, parent, &cancel);
    CancelGuard cancel_guard(std::move(cancel));
    return Wait(mutex, context);
}

void ConditionVariable::NotifyOne() noexcept {
    std::shared_ptr<WaitNode> selected;
    {
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        while (!m_impl->m_waiters.empty()) {
            auto candidate = std::move(m_impl->m_waiters.front());
            m_impl->m_waiters.pop_front();
            if (candidate &&
                candidate->TryFinish(WaitResult::kNotified)) {
                selected = std::move(candidate);
                break;
            }
        }
    }
    if (selected) {
#if GO2CPP_DEBUG_RESOURCES
        GO2CPP_DEBUG_RESOURCE_EVENT_V5(
            kCvNotify, m_impl->m_debug_resource, GO2CPP_DEBUG_BOUND_TASK(),
            selected->gid(), 0, ::go2cpp::debug::ResourceResult::kWake, 0,
            true, 0, -1, 0, 0);
#endif
        selected->Wake();
    }
}

void ConditionVariable::NotifyAll() noexcept {
    std::deque<std::shared_ptr<WaitNode>> waiters;
    {
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        waiters.swap(m_impl->m_waiters);
    }
    // v5：广播记录**一条** kCvNotifyAll，而不是每个等待者一条 kCvNotify。
    // 字段：waiters=候选等待者数，value=实际唤醒数，target=首个被唤醒 gid。
    // 这样回放端既能算出「广播了几次」，也能看出每次广播覆盖多少等待者，
    // 序列长度不随等待者数膨胀。
    std::uint32_t woken = 0;
    std::uint64_t first_gid = 0;
    for (const auto& waiter : waiters) {
        if (waiter && waiter->TryFinish(WaitResult::kNotified)) {
            if (first_gid == 0U) {
                first_gid = waiter->gid();
            }
            ++woken;
            waiter->Wake();
        }
    }
#if GO2CPP_DEBUG_RESOURCES
    GO2CPP_DEBUG_RESOURCE_EVENT_V5(
        kCvNotifyAll, m_impl->m_debug_resource, GO2CPP_DEBUG_BOUND_TASK(),
        first_gid, static_cast<std::uint64_t>(woken),
        ::go2cpp::debug::ResourceResult::kWake,
        static_cast<std::uint32_t>(waiters.size()), !waiters.empty(), 0, -1, 0,
        0);
#endif
}

/**
 * @brief `sync::WaitGroup` 的共享实现（`Add/Done/Wait` 的计数与等待队列）。
 *
 * ## 状态机（只有两态）
 *
 * `Pending(count > 0)` -(Done() 使 count 归零)-> `Done`：归零时唤醒全部等待者。
 * `Add()` 在归零后可以重新开始一轮（Go 的 `WaitGroup` 允许复用），但
 * **`Add` 与 `Wait` 并发是调用方的错误**（Go 里会 panic 或竞态）。
 *
 * ## 不变量
 *
 * - `m_count >= 0`；`Add` 让计数变负是未定义行为（本实现不 panic，靠调用方守约）；
 * - `m_count == 0 && !m_waiters.empty()` 只是**瞬态**：`Done` 归零时在同一把锁内
 *   把队列 swap 出来，再在锁外逐个唤醒。
 */
struct WaitGroup::Impl {
    /// @brief 保护 m_count 与 m_waiters；Add/Done 写、Wait 读。
    mutable std::mutex m_mutex;
#if GO2CPP_DEBUG_RESOURCES
    ::go2cpp::debug::ResourceId m_debug_resource{0};
#endif
    /// @brief 未完成计数；`Add(n)` 加、`Done()` 减，归零时唤醒全部等待者。
    std::int64_t m_count{0};
    /// @brief 等待计数归零的队列（FIFO）；`Wait` 入队、`Done` 归零时整体 swap 出。
    std::deque<std::shared_ptr<WaitNode>> m_waiters;
};

WaitGroup::WaitGroup() : m_impl(std::make_unique<Impl>()) {
#if GO2CPP_DEBUG_RESOURCES
    m_impl->m_debug_resource =
        GO2CPP_DEBUG_RESOURCE_REGISTER(kWaitGroup, this, nullptr);
#endif
}

WaitGroupDebugSnapshot WaitGroup::DebugSnapshotInternal() const {
    WaitGroupDebugSnapshot snapshot;
    std::lock_guard<std::mutex> lock(m_impl->m_mutex);
    snapshot.count = m_impl->m_count;
    snapshot.waiters = m_impl->m_waiters.size();
    return snapshot;
}

WaitGroup::~WaitGroup() {
#if GO2CPP_DEBUG_RESOURCES
    GO2CPP_DEBUG_RESOURCE_UNREGISTER(m_impl->m_debug_resource);
#endif
}

void WaitGroup::Add(std::int64_t delta) {
    std::deque<std::shared_ptr<WaitNode>> released;
    std::int64_t debug_count = 0;
    {
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        if (delta < 0 &&
            (delta == std::numeric_limits<std::int64_t>::min() ||
             m_impl->m_count < -delta)) {
            throw std::logic_error("go2cpp::sync::WaitGroup negative counter");
        }
        if (delta > 0 &&
            m_impl->m_count >
                std::numeric_limits<std::int64_t>::max() - delta) {
            throw std::logic_error("go2cpp::sync::WaitGroup counter overflow");
        }
        m_impl->m_count += delta;
        debug_count = m_impl->m_count;
        if (m_impl->m_count == 0) {
            for (const auto& waiter : m_impl->m_waiters) {
                if (waiter) {
                    (void)waiter->TryFinish(WaitResult::kNotified);
                }
            }
            released.swap(m_impl->m_waiters);
        }
    }

#if GO2CPP_DEBUG_RESOURCES
    // v5：Add 与 Done 分成两种事件（回放端可分别统计「加了几个」与「做完几个」）。
    // value=操作后的计数，res_aux 低 32 位=本次 delta（Done 为 0xFFFFFFFF）。
    if (delta > 0) {
        GO2CPP_DEBUG_RESOURCE_EVENT_V5(
            kWaitGroupAdd, m_impl->m_debug_resource,
            GO2CPP_DEBUG_BOUND_TASK(),
            released.empty()
                ? 0U
                : static_cast<std::uint64_t>(released.front()->gid()),
            static_cast<std::uint64_t>(debug_count),
            released.empty() ? ::go2cpp::debug::ResourceResult::kOk
                             : ::go2cpp::debug::ResourceResult::kWake,
            static_cast<std::uint32_t>(released.size()), !released.empty(), 0,
            -1, 0, static_cast<std::uint64_t>(delta));
    } else {
        GO2CPP_DEBUG_RESOURCE_EVENT_V5(
            kWaitGroupDone, m_impl->m_debug_resource,
            GO2CPP_DEBUG_BOUND_TASK(),
            released.empty()
                ? 0U
                : static_cast<std::uint64_t>(released.front()->gid()),
            static_cast<std::uint64_t>(debug_count),
            released.empty() ? ::go2cpp::debug::ResourceResult::kOk
                             : ::go2cpp::debug::ResourceResult::kWake,
            static_cast<std::uint32_t>(released.size()), !released.empty(), 0,
            -1, 0, static_cast<std::uint64_t>(delta));
    }
#endif
    // 在零值转换被锁保护时标记旧一轮结果。后续 Add 不能改变这些结果；
    // 任意 Scheduler 工作都放在锁外执行。
    for (const auto& waiter : released) {
        if (waiter && waiter->result() == WaitResult::kNotified) {
            waiter->Wake();
        }
    }
}

void WaitGroup::Done() { Add(-1); }

bool WaitGroup::Wait(const ContextPtr& context) {
    {
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        if (m_impl->m_count == 0) {
            return true;
        }
    }
    if (context && context->IsDone()) {
        return false;
    }

    const WaitTarget target = CurrentTarget();
    if (target.unsupported_manual_fiber) {
        return false;
    }
    if (target && target.task->cancellation_requested()) {
        return false;
    }
    const auto waiter =
        std::make_shared<WaitNode>(target.scheduler, target.task);
    ContextSubscription subscription(context, waiter);
    {
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        if (m_impl->m_count == 0) {
            return true;
        }
        if ((context && context->IsDone()) ||
            waiter->result() != WaitResult::kWaiting) {
            return false;
        }
        m_impl->m_waiters.push_back(waiter);
        if (!waiter->Arm()) {
            m_impl->m_waiters.pop_back();
            return false;
        }
    }
#if GO2CPP_DEBUG_RESOURCES
    GO2CPP_DEBUG_RESOURCE_EVENT_V5(
        kWaitGroupWait, m_impl->m_debug_resource, GO2CPP_DEBUG_BOUND_TASK(), 0,
        static_cast<std::uint64_t>(m_impl->m_count),
        ::go2cpp::debug::ResourceResult::kBlocked,
        static_cast<std::uint32_t>(m_impl->m_waiters.size()), true, 0, -1, 0,
        0);
#endif

    const WaitResult result = Await(waiter.get());
    subscription.Reset();
    if (result != WaitResult::kNotified) {
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        RemoveWaiter(m_impl->m_waiters, waiter);
#if GO2CPP_DEBUG_RESOURCES
        GO2CPP_DEBUG_RESOURCE_EVENT_V5(
            kWaitGroupWait, m_impl->m_debug_resource, GO2CPP_DEBUG_BOUND_TASK(),
            0, 0, ::go2cpp::debug::ResourceResult::kCancelled, 0, true, 0, -1,
            0, 0);
#endif
        return false;
    }
#if GO2CPP_DEBUG_RESOURCES
    GO2CPP_DEBUG_RESOURCE_EVENT_V5(
        kWaitGroupWait, m_impl->m_debug_resource, GO2CPP_DEBUG_BOUND_TASK(), 0,
        0, ::go2cpp::debug::ResourceResult::kOk, 0, true, 0, -1, 0, 0);
#endif
    return true;
}

bool WaitGroup::WaitFor(ContextDuration timeout, const ContextPtr& parent) {
    CancelFunc cancel;
    const ContextPtr context = TimeoutContext(timeout, parent, &cancel);
    CancelGuard cancel_guard(std::move(cancel));
    return Wait(context);
}

std::int64_t WaitGroup::Count() const noexcept {
    std::lock_guard<std::mutex> lock(m_impl->m_mutex);
    return m_impl->m_count;
}

void SetHeldLockParkDetection(bool enabled) noexcept {
    g_detect_held_park.store(enabled ? 1 : 0, std::memory_order_relaxed);
}

bool HeldLockParkDetection() noexcept { return DetectHeldParkEnabled(); }

std::uint64_t HeldLockParkCount() noexcept {
    // 单一计数源：`Task::park_for_scheduler()` 覆盖所有 park 路径
    // （含 park_io），比 sync 自己的等待后端更完整。
    return scheduler::mutex_held_park_total();
}


/// @brief D3 诊断：ConditionVariable::Wait 重锁失败次数（进程级累计）。
/// @note 用 extern "C" 导出，避免为一个诊断计数改动公共头文件/ABI。
extern "C" std::uint64_t go2cpp_debug_cv_relock_failures() noexcept {
    return g_cv_relock_failures.load(std::memory_order_relaxed);
}

}  // namespace go2cpp::sync
