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

#include "go2cpp/fiber.hpp"
#include "go2cpp/scheduler.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
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

struct WaitTarget {
    Scheduler* scheduler{nullptr};
    std::shared_ptr<Task> task;
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
 * ## 状态字（`m_state` 两个位，必须一起理解）
 *
 * | 位 | 含义 |
 * |---|---|
 * | `kLocked` | 锁已被持有 |
 * | `kWaiter` | 可能有线程睡在 futex 上 / 正在入队 |
 *
 * **不变量**：只要可能有线程睡在 futex 上，状态就**永远不会是 0/1** —— 这样新来的
 * `lock()` 无法用 `CAS(0→1)` 快路径绕过仍在睡眠的等待者。这条不变量是 250 倍退化
 * 修复的核心（详见 `docs/gmp-optimization-log.md` 的「sync::Mutex 的 250 倍退化」）。
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
    // 同一个原子字节同时发布“已持有”和“已有等待者/等待者正在入队”
    // 两个状态。Unlock 的 compare_exchange 因此不会读取到旧的等待者
    // 标记后错误地释放锁，避免丢失唤醒。
    static constexpr std::uint8_t kLocked = 1U;
    static constexpr std::uint8_t kWaiter = 2U;
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
};

Mutex::Mutex() : m_impl(std::make_unique<Impl>()) {}

MutexDebugSnapshot Mutex::DebugSnapshotInternal() const {
    MutexDebugSnapshot snapshot;
    const std::uint8_t state = m_impl->m_state.load(std::memory_order_acquire);
    snapshot.held = (state & Impl::kLocked) != 0U;
    snapshot.has_waiters = (state & Impl::kWaiter) != 0U;
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

Mutex::~Mutex() = default;

bool Mutex::Lock(const ContextPtr& context) {
    return LockImpl(context, false);
}

bool Mutex::LockForConditionWait() {
    return LockImpl({}, true);
}

bool Mutex::LockImpl(const ContextPtr& context,
                     bool ignore_task_cancellation) {
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

    std::uint8_t expected_state = 0U;
    if (m_impl->m_state.compare_exchange_strong(
            expected_state, Impl::kLocked, std::memory_order_acquire,
            std::memory_order_relaxed)) {
        note_lock_held();
        return true;
    }

    const WaitTarget target = CurrentTarget();
    if (target.unsupported_manual_fiber) {
        // 手动恢复的 Fiber 没有可用于 park 的 Scheduler continuation。返回 false
        // 比阻塞 carrier 线程更安全。
        return false;
    }
    if (!ignore_task_cancellation && target &&
        target.task->cancellation_requested()) {
        return false;
    }

    // 受管 Fiber 的竞争路径：**直接进入 WaitNode + park**，不忙自旋、不 yield。
    //
    // ## 为什么删掉旧的"协作 yield 重试"（2026-10-18 探针实测）
    //
    // 旧实现对受管 G 最多做 256 次 `yield_current()`：每次都是一次完整的
    // Fiber 挂起 + 重新入队。64 Fiber / 8 M、32,000 次 lock 的探针口径：
    //   进入协作循环 29,281 次，累计 yield 7,464,941 次，其中只有 196 次
    //   在循环里真正抢到锁；其余 29,085 次烧完 256 次 yield 后仍然 park。
    // 平均每次 lock 233 次上下文切换，全是无效功 —— 这就是
    // M=8/Fiber=64 退化到 73,000~92,000 ns/op 的原因。同一负载 M=1 不走
    // 这条 yield 路径（直接 park），只要 634 ns/op，相差 115 倍。
    //
    // ## 为什么最终连"忙自旋"也不做
    //
    // 试过三种自旋预算（8 / 32 上限的自适应衰减、以及不限队列的 64/256），
    // 5 次中位数对比（场景 B，M=8）：
    //   自旋 0   ：Fiber=2 780ns、Fiber=8 893ns、Fiber=64 740ns
    //   自旋 8   ：Fiber=2 1076ns、Fiber=8 948ns、Fiber=64 788ns
    //   自旋 32  ：Fiber=2 1327ns、Fiber=8 950ns、Fiber=64 770ns
    // 自旋只会变慢。原因：受管 Fiber 上，持锁者要么正跑在**另一个 M** 上
    // （unlock→TryFinish→wake 的 park/wake 交接实测只要 390ns，比自旋更
    // 便宜），要么自己已经 park/yield（自旋纯属空烧）。自旋还要么挡住本 M
    // 上其他 G，要么越过 FIFO 队列插队。
    //
    // 因此这里的策略是：竞争时直接把当前 G 挂起（park 会释放 M 让其他 G
    // 继续跑），由 Unlock 精确唤醒 FIFO 队首。等待者的控制块走 thread-local
    // 缓存复用（32 个/线程），稳态零堆分配。

    // std::mutex 在 Linux 上也会先进行短暂自旋；混合锁的 native
    // 调用者如果立即进入条件变量慢路径，会把很短的临界区放大成一次
    // futex 唤醒。这里保留较小的有界自旋，并且一旦队列发布立即退出，
    // 因而不会越过 FIFO waiter。
    if (!target) {
        // 自旋预算分两段（2026-10-18 实测重新标定）：
        //   1. 没有等待者时给足 2048 次（≈18µs，PAUSE 实测 8.7ns）。这一档
        //      必须覆盖"N 个线程轮流抢同一把锁"的整个窗口：太小（128/256）
        //      时 8 线程争用会有人 256 次全失败、转入等待队列并睡一次，
        //      之后每个操作都要付一次 futex 唤醒延迟 —— 这就是
        //      docs/performance.md 里 4 线程 25~30µs/op 的来源。
        //      实测 2048 时 8 线程 median 225ns/op；4096 反而更慢（349ns，
        //      多出来的空转没有收益），1024 则回退到 321ns。
        //   2. 已有等待者时只给 4 次 —— 严格回到 FIFO handoff，不越过
        //      队列里的等待者，保持既有公平性承诺。
        // 越界后进入 WaitNode 等待；WaitNative 侧还有一层有界自旋。
        constexpr int kNativeSpinAttempts = 2048;
        constexpr int kNativeSpinWithWaiters = 4;
        for (int attempt = 0; attempt < kNativeSpinAttempts; ++attempt) {
            if ((m_impl->m_state.load(std::memory_order_acquire) &
                 Impl::kWaiter) != 0U &&
                attempt >= kNativeSpinWithWaiters) {
                break;
            }
            expected_state = 0U;
            if (m_impl->m_state.compare_exchange_weak(
                    expected_state, Impl::kLocked,
                    std::memory_order_acquire,
                    std::memory_order_relaxed)) {
                return true;
            }
            RelaxCpu();
        }
    }

    // Mutex::Unlock 会在释放 m_mutex 后异步执行 Wake，因此即使没有
    // Context，也必须让队列持有一个 shared_ptr，不能使用栈上节点。
    std::shared_ptr<WaitNode> owned_waiter =
        AcquireWaitNode(target.scheduler, target.task);
    WaitNode* waiter = owned_waiter.get();
    ContextSubscription subscription(context, owned_waiter);
    const auto release_waiter = [&] {
        subscription.Reset();
        if (owned_waiter) {
            ReleaseWaitNode(std::move(owned_waiter));
        }
    };
    {
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        if ((context && context->IsDone()) ||
            waiter->result() != WaitResult::kWaiting) {
            release_waiter();
            return false;
        }

        // 等待者位只在持有队列锁时发布。这样不会出现“等待者已经设置
        // 标志但尚未入队，Unlock 却覆盖标志”的窗口：Unlock 的无竞争
        // CAS 可以在本段之前把锁释放掉，当前调用随后重新观察到空闲锁
        // 并直接取得；如果 CAS 发生在发布之后，则必然看到 kWaiter 并
        // 进入同一把队列锁完成 FIFO 交接。
        for (;;) {
            expected_state = m_impl->m_state.load(std::memory_order_acquire);
            if ((expected_state & Impl::kLocked) == 0U &&
                m_impl->m_waiters.empty()) {
                if (m_impl->m_state.compare_exchange_weak(
                        expected_state, Impl::kLocked,
                        std::memory_order_acquire,
                        std::memory_order_relaxed)) {
                    note_lock_held();
                    release_waiter();
                    return true;
                }
                continue;
            }

            if ((expected_state & Impl::kWaiter) == 0U) {
                const std::uint8_t published = static_cast<std::uint8_t>(
                    expected_state | Impl::kWaiter);
                if (!m_impl->m_state.compare_exchange_weak(
                        expected_state, published,
                        std::memory_order_acq_rel,
                        std::memory_order_acquire)) {
                    continue;
                }
            }
            break;
        }

        m_impl->m_waiters.push_back(owned_waiter);
        if (!target) {
            m_impl->m_native_waiters.fetch_add(1U, std::memory_order_release);
        }
        if (!waiter->Arm()) {
            m_impl->m_waiters.pop_back();
            if (!target) {
                m_impl->m_native_waiters.fetch_sub(1U,
                                                    std::memory_order_release);
            }
            if (m_impl->m_waiters.empty()) {
                m_impl->m_state.fetch_and(
                    static_cast<std::uint8_t>(~Impl::kWaiter),
                    std::memory_order_release);
            }
            release_waiter();
            return false;
        }
    }

    const bool managed_waiter = waiter->managed();
    const WaitResult result = Await(waiter, ignore_task_cancellation);
    subscription.Reset();
    if (result != WaitResult::kNotified) {
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
            m_impl->m_state.fetch_and(
                static_cast<std::uint8_t>(~Impl::kWaiter),
                std::memory_order_release);
        }
        if (owned_waiter) {
            ReleaseWaitNode(std::move(owned_waiter));
        }
        return false;
    }
    if (owned_waiter) {
        ReleaseWaitNode(std::move(owned_waiter));
    }
    note_lock_held();
    return true;
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
    std::uint8_t expected = 0U;
    const bool acquired = m_impl->m_state.compare_exchange_strong(
        expected, Impl::kLocked, std::memory_order_acquire,
        std::memory_order_relaxed);
    return acquired;
}

void Mutex::Unlock() {
    // 持锁 park 检测：只有"解锁者就是记录里的 owner"时才递减，
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
    // 无等待者时只需一次 CAS，不进入队列互斥量。若等待者位同时存在，
    // CAS 必定失败并转入下面的 FIFO 交接路径。
    std::uint8_t expected_state = Impl::kLocked;
    if (m_impl->m_state.compare_exchange_strong(
            expected_state, 0U, std::memory_order_release,
            std::memory_order_acquire)) {
        return;
    }

    std::shared_ptr<WaitNode> selected;
    {
        // Unlock 与 waiter 发布必须共享同一把队列锁。原子快路径无法
        // 同时覆盖 waiter 入队和另一个线程重新取得锁的交错，统一在此
        // 线性化可以避免丢唤醒和“误解锁后来 owner”。
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        if ((m_impl->m_state.load(std::memory_order_acquire) &
             Impl::kLocked) == 0U) {
            throw std::logic_error("go2cpp::sync::Mutex unlock of unlocked mutex");
        }
        // D3：先让选择器重排队列（只换位置，不改认领语义）；默认无选择器时
        // 只多一次指针判空，热路径开销可忽略。
        ReorderWaitersForSelector(m_impl->m_waiters);
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
        if (!selected) {
            m_impl->m_state.store(0U, std::memory_order_release);
        } else {
            m_impl->m_state.store(
                static_cast<std::uint8_t>(Impl::kLocked |
                                           (m_impl->m_waiters.empty()
                                                ? 0U
                                                : Impl::kWaiter)),
                std::memory_order_release);
        }
        // 选定 waiter 后锁在逻辑上仍保持持有：所有权直接交给 FIFO 队首，
        // 因而 TryLock 调用者不能插队。
    }
    if (selected) {
        selected->Wake();
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
struct ConditionVariable::Impl {
    /// @brief 保护 m_waiters；Wait 入队、NotifyOne/NotifyAll 出队。
    std::mutex m_mutex;
    /// @brief 通知等待队列（FIFO）；存 shared_ptr 以便锁外 Wake 时节点仍存活。
    std::deque<std::shared_ptr<WaitNode>> m_waiters;
};

ConditionVariable::ConditionVariable() : m_impl(std::make_unique<Impl>()) {}

ConditionVariableDebugSnapshot ConditionVariable::DebugSnapshotInternal() const {
    ConditionVariableDebugSnapshot snapshot;
    std::lock_guard<std::mutex> lock(m_impl->m_mutex);
    snapshot.waiters = m_impl->m_waiters.size();
    return snapshot;
}

ConditionVariable::~ConditionVariable() = default;

bool ConditionVariable::Wait(Mutex& mutex, const ContextPtr& context) {
    // 必须先识别手动 Fiber。即使 Context 已取消，也不能走普通线程的
    // unlock/relock 路径，否则竞争中的重锁会阻塞 carrier 线程。
    const WaitTarget target = CurrentTarget();
    const auto abort_wait = [&mutex](bool preserve_lock) {
        // 所有可调度调用者统一走 cleanup 重锁。它忽略 G 的取消标记，
        // 但仍可让出当前 G 等待 owner 释放 mutex；只有手动 Fiber 没有
        // Scheduler continuation，因此必须保留进入 Wait 前的锁状态。
        if (!preserve_lock) {
            mutex.Unlock();
            (void)mutex.LockForConditionWait();
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
    const bool relocked = mutex.LockForConditionWait();
    return relocked && result == WaitResult::kNotified;
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
        selected->Wake();
    }
}

void ConditionVariable::NotifyAll() noexcept {
    std::deque<std::shared_ptr<WaitNode>> waiters;
    {
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        waiters.swap(m_impl->m_waiters);
    }
    for (const auto& waiter : waiters) {
        if (waiter && waiter->TryFinish(WaitResult::kNotified)) {
            waiter->Wake();
        }
    }
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
    /// @brief 未完成计数；`Add(n)` 加、`Done()` 减，归零时唤醒全部等待者。
    std::int64_t m_count{0};
    /// @brief 等待计数归零的队列（FIFO）；`Wait` 入队、`Done` 归零时整体 swap 出。
    std::deque<std::shared_ptr<WaitNode>> m_waiters;
};

WaitGroup::WaitGroup() : m_impl(std::make_unique<Impl>()) {}

WaitGroupDebugSnapshot WaitGroup::DebugSnapshotInternal() const {
    WaitGroupDebugSnapshot snapshot;
    std::lock_guard<std::mutex> lock(m_impl->m_mutex);
    snapshot.count = m_impl->m_count;
    snapshot.waiters = m_impl->m_waiters.size();
    return snapshot;
}

WaitGroup::~WaitGroup() = default;

void WaitGroup::Add(std::int64_t delta) {
    std::deque<std::shared_ptr<WaitNode>> released;
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
        if (m_impl->m_count == 0) {
            for (const auto& waiter : m_impl->m_waiters) {
                if (waiter) {
                    (void)waiter->TryFinish(WaitResult::kNotified);
                }
            }
            released.swap(m_impl->m_waiters);
        }
    }

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

    const WaitResult result = Await(waiter.get());
    subscription.Reset();
    if (result != WaitResult::kNotified) {
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        RemoveWaiter(m_impl->m_waiters, waiter);
        return false;
    }
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

}  // namespace go2cpp::sync
