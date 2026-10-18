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

struct Mutex::Impl {
    std::mutex m_mutex;
    // 同一个原子字节同时发布“已持有”和“已有等待者/等待者正在入队”
    // 两个状态。Unlock 的 compare_exchange 因此不会读取到旧的等待者
    // 标记后错误地释放锁，避免丢失唤醒。
    static constexpr std::uint8_t kLocked = 1U;
    static constexpr std::uint8_t kWaiter = 2U;
    std::atomic<std::uint8_t> m_state{0U};
    // 只统计已经进入等待队列的普通线程。managed Fiber 在没有 native
    // waiter 时可以采用有界协作重试，避免每次短临界区都 park。
    std::atomic<std::size_t> m_native_waiters{0U};
    // 队列必须持有 shared_ptr。Unlock 会先从队列取出等待者，再在释放
    // 队列锁后调用 Wake；如果队列只保存裸指针，等待方可能在这段窗口
    // 内返回并销毁栈上的 WaitNode，或把缓存节点重置给下一次等待，
    // 从而形成悬空访问。队列持有的引用把节点生命周期延长到 Wake
    // 完成，同时不改变锁外唤醒以避免锁反转的约束。
    std::deque<std::shared_ptr<WaitNode>> m_waiters;
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

    std::uint8_t expected_state = 0U;
    if (m_impl->m_state.compare_exchange_strong(
            expected_state, Impl::kLocked, std::memory_order_acquire,
            std::memory_order_relaxed)) {
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

    // 混合并发下，短临界区通常只需要等待当前 Fiber 完成一小段工作。
    // 没有 native waiter 时，managed G 走有界协作重试，避免每次短竞争
    // 都进入 WaitNode + park。managed waiter 之间允许有限度地越过队列，
    // 以换取吞吐；一旦有普通线程 waiter，最多再重试 4 次后回到 FIFO
    // handoff，保证线程不会被 Fiber 长时间压住。预算耗尽后仍会进入
    // WaitNode + park，不会无界占用 M。
    if (target && target.scheduler->processor_count() > 1 &&
        m_impl->m_native_waiters.load(std::memory_order_acquire) == 0U &&
        context == nullptr) {
        // 竞争刚出现时，临界区通常仍在另一个 P 上运行。协作让出可以
        // 避免把短临界区升级为 Fiber 挂起/恢复和跨 M 唤醒；单 P 不走
        // 这条路径，因此不会把唯一 M 忙等住。预算耗尽后再进入 waiter。
        // 只给当前 G 一个极小的协作窗口，避免短临界区立即创建等待
        // 节点；窗口耗尽后一次性进入 FIFO 等待队列。不能使用长自旋，
        // 否则一个 P 上的持锁 G 可能被同一 P 的等待 G 阻塞。
        // 受管 Fiber 不在这里反复做完整 yield。等待节点的 park/wake
        // 本身已经是无丢失唤醒的轻路径；重复 yield 会把同一批 G 重新
        // 放回 P 队列，制造秒级长尾。
        constexpr int kCooperativeAttempts = 256;
        for (int attempt = 0; attempt < kCooperativeAttempts; ++attempt) {
            // native waiter 出现后，重试预算被限制为 4 次，避免 Fiber
            // 无限越过普通线程；这次检查放在每轮 CAS 前。
            if (m_impl->m_native_waiters.load(std::memory_order_acquire) !=
                    0U &&
                attempt >= 4) {
                break;
            }
            expected_state = 0U;
            if (m_impl->m_state.compare_exchange_weak(
                    expected_state, Impl::kLocked,
                    std::memory_order_acquire,
                    std::memory_order_relaxed)) {
                return true;
            }
            if (!ignore_task_cancellation &&
                target.task->cancellation_requested()) {
                break;
            }
            if (!target.scheduler->yield_current()) {
                break;
            }
        }
    }
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

struct ConditionVariable::Impl {
    std::mutex m_mutex;
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

struct WaitGroup::Impl {
    mutable std::mutex m_mutex;
    std::int64_t m_count{0};
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

}  // namespace go2cpp::sync
