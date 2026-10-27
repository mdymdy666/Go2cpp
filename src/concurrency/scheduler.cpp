/**
 * @file scheduler.cpp
 * @brief GMP 调度器的完整实现（P/M/G 管理、窃取、park/wake、sysmon）。
 *
 * ## 这个文件实现什么
 *
 * 它是整个并发运行时最大也最核心的实现文件，把 `scheduler.hpp` 里的接口
 * 落到具体的数据结构与算法上：
 *
 * - **P（Processor）**：逻辑处理器，每个 P 一条本地队列，`processor_count`
 *   决定真正的并行度；
 * - **M（Machine）**：OS 线程，从 P 队列取任务执行；阻塞时可以把 P 交出去
 *   （handoff），由空闲 M 接管，避免一个阻塞调用拖死整个 P；
 * - **G（Goroutine/Task）**：用户提交的工作单元，状态机是 `GState`。
 *
 * ## 关键机制
 *
 * 1. **本地队列 + work stealing**：线程优先取自己 P 的队列，空闲时从别的 P
 *    "偷"一半（`take_victim`）。本地队列有长度上限，超了溢出到全局队列；
 * 2. **park / wake**：Fiber 因等待而挂起时通过 `ParkingCondition` park，
 *    唤醒方把它重新入队。这条路径的正确性直接决定死锁与否；
 * 3. **sysmon**：Go 风格的阻塞监控，观察长时间阻塞的 M 并做记录/handoff；
 * 4. **yield**：主动让出，走 `Task::defer_enqueue` → 本地队列**尾部**，
 *    不经过全局锁。
 *
 * ## 性能相关的历史结论（避免重复踩坑）
 *
 * - 全局队列锁的临界区里含 `find` + 队列操作，**加自旋预算会让整体慢 36%** ——
 *   自旋只适合极短临界区；内部唤醒门已改用无自旋的 `core::FutexMutex`；
 * - 把 `runnable` 预检查提到 park 之前会让忙等在窗口里空转，慢 37%；
 * - 把窃取移到锁外会慢 34%。
 *
 * 这些结论都来自 `build-bench`（Release）实测，改动这块之前请先看 codex.md。
 */
#include "go2cpp/scheduler/scheduler.hpp"

#include "go2cpp/debug/debug_event_ring.hpp"
#include "go2cpp/fiber.hpp"
#include "go2cpp/log.hpp"
#include "go2cpp/thread_policy.hpp"
#include "go2cpp/core/parking_condition.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <exception>
#include <iterator>
#include <limits>
#include <mutex>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <execinfo.h>
#include <pthread.h>
#include <sched.h>
#endif

namespace go2cpp::scheduler {


#ifdef GO2CPP_COUNT_GLOBAL_LOCK
namespace profiling {
std::atomic<std::uint64_t> g_lock_acquires{0};
std::atomic<std::uint64_t> g_lock_wait_ns{0};
std::atomic<std::uint64_t> g_lock_hold_ns{0};
}  // namespace profiling
#endif

namespace {

// ---------------------------------------------------------------------------
// 持锁 park 契约检查
// ---------------------------------------------------------------------------
//
// 两类标记，语义不同，处理方式也不同：
//
// | 条件 | 含义 | debug | release |
// |---|---|---|---|
// | no_park_depth > 0 | SpinMutex 临界区，禁止 park | abort | 降级 + 计数 |
// | held_mutexes > 0  | 普通 Mutex 临界区，允许但危险 | 计数 + 日志 | 计数 |
//
// 检测点放在 Task::park_for_scheduler() 的最前面：所有 park 路径
// （park_wait / park_io / park_with_reason / cleanup）都汇聚到那里，
// 且必须在状态被改成 kWaiting 之前检查，否则当前 G 已经让出执行流。

std::atomic<std::uint64_t> g_spin_mutex_park_degrade_total{0};
std::atomic<std::uint64_t> g_mutex_held_park_total{0};
std::atomic<bool> g_spin_mutex_park_warned{false};
std::atomic<bool> g_held_park_warned{false};

/// 打印一次诊断，之后的同类事件只计数，避免日志风暴。
void WarnOnce(std::atomic<bool>& flag, const char* message) noexcept {
    bool expected = false;
    if (!flag.compare_exchange_strong(expected, true,
                                      std::memory_order_relaxed)) {
        return;
    }
    std::fprintf(stderr, "%s\n", message);
    std::fflush(stderr);
}

#if defined(__linux__)
/// 打印当前调用栈；只在 abort 前的诊断路径使用。
[[maybe_unused]] void DumpParkCallStack() noexcept {
    void* frames[32];
    const int count = ::backtrace(frames, 32);
    if (count > 0) {
        std::fprintf(stderr, "[go2cpp] park call stack:\n");
        ::backtrace_symbols_fd(frames, count, ::fileno(stderr));
    }
}
#else
void DumpParkCallStack() noexcept {}
#endif

/**
 * @brief 处理「SpinMutex 临界区内发生 park」。
 *
 * @details debug 构建（定义 `GO2CPP_DEBUG_LOCKS` 或未定义 `NDEBUG`）：
 *          打印 G id、嵌套深度、原因提示与调用栈后 `std::abort()`。这是
 *          契约违反，必须让开发期立刻发现。
 *
 *          release 构建（`NDEBUG`）：**绝不 abort**。降级为普通 `Mutex`
 *          语义（正确但慢），只递增降级计数并在第一次打印警告。
 */
void ReportParkInNoParkSection([[maybe_unused]] const Task& task,
                               [[maybe_unused]] std::uint32_t depth) noexcept {
#if defined(GO2CPP_DEBUG_LOCKS) || !defined(NDEBUG)
    std::fprintf(stderr,
                 "[FATAL] G%llu parked inside a SpinMutex critical section "
                 "(no_park_depth=%u)\n",
                 static_cast<unsigned long long>(task.id()),
                 static_cast<unsigned>(depth));
    std::fprintf(stderr,
                 "[FATAL] 临界区内出现了 park 点（read/write/connect/accept/"
                 "sleep 或竞争中的 Mutex::Lock）。\n"
                 "[FATAL] 修复：可能阻塞的临界区改用 sync::Mutex，或把 park "
                 "移出临界区。\n");
    std::fflush(stderr);
    DumpParkCallStack();
    std::abort();
#else
    g_spin_mutex_park_degrade_total.fetch_add(1, std::memory_order_relaxed);
    WarnOnce(g_spin_mutex_park_warned,
             "[go2cpp] WARN: parked inside a SpinMutex critical section; "
             "degrading to Mutex semantics (release build never aborts). "
             "Counter: scheduler::spin_mutex_park_degrade_total()");
#endif
}

/**
 * @brief 处理「普通 Mutex 临界区内发生 park」。
 *
 * @details 普通 `Mutex` 允许 park，因此这里**不 abort**；只计数（debug 下
 *          额外打印一次）。这类 park 是持锁 IO 的直接信号：如果等待的 fd
 *          永不就绪，等待同一把锁的 G 会永久死锁。
 */
void ReportParkWhileHoldingMutex(const Task& task) noexcept {
    g_mutex_held_park_total.fetch_add(1, std::memory_order_relaxed);
    bool expected = false;
    if (!g_held_park_warned.compare_exchange_strong(
            expected, true, std::memory_order_relaxed)) {
        return;
    }
    std::fprintf(stderr,
                 "[go2cpp] held-lock-park: G%llu 在持有 %u 把 sync::Mutex "
                 "时 park（允许，但持锁 IO 可能永久死锁；不自动解锁）\n",
                 static_cast<unsigned long long>(task.id()),
                 static_cast<unsigned>(task.held_mutex_count()));
    std::fflush(stderr);
}

#ifdef GO2CPP_COUNT_GLOBAL_LOCK
// 诊断插桩（默认关闭）：统计调度器全局锁的获取次数与持有时间。
// 计数器定义在上面的 profiling 命名空间里（可外部链接）。

struct LockStatGuard {
    std::chrono::steady_clock::time_point begin;
    explicit LockStatGuard(std::mutex& m) {
        const auto t0 = std::chrono::steady_clock::now();
        m.lock();
        const auto t1 = std::chrono::steady_clock::now();
        profiling::g_lock_wait_ns.fetch_add(
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()),
            std::memory_order_relaxed);
        profiling::g_lock_acquires.fetch_add(1, std::memory_order_relaxed);
        begin = t1;
    }
    ~LockStatGuard() {
        const auto t2 = std::chrono::steady_clock::now();
        profiling::g_lock_hold_ns.fetch_add(
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t2 - begin).count()),
            std::memory_order_relaxed);
    }
    LockStatGuard(const LockStatGuard&) = delete;
    LockStatGuard& operator=(const LockStatGuard&) = delete;
};
#endif


std::atomic<GId> s_next_g_id{1};
std::atomic<MId> s_next_m_id{1};

thread_local Scheduler* t_scheduler = nullptr;
thread_local std::shared_ptr<Task> t_task;
thread_local MId t_machine_id = 0;
thread_local PId t_processor_id = 0;
// BlockingRegion is deliberately non-nestable: one M has one state
// transition, and rejecting nested scopes avoids double accounting. The
// machine id is retained so a misuse that lets a Fiber migrate before its
// region is destroyed can still clear the original M's state.
thread_local MId t_blocking_machine_id = 0;

/// @brief 当前线程是否正处在 worker 的 IO 空闲轮询里。
/// @details poll 内部会处理就绪事件并把 G 投回本 P；投递者随后立刻 `continue`
///          回主循环取任务，因此不需要再唤醒别的 worker。只在
///          `requeue_from_worker`（本 P 投递）里短路，跨 P 投递照常通知。
thread_local bool t_in_idle_poll = false;

/// @brief 在作用域内置位 `t_in_idle_poll` 的 RAII 守卫。
struct IdlePollScope final {
    IdlePollScope() noexcept { t_in_idle_poll = true; }
    ~IdlePollScope() noexcept { t_in_idle_poll = false; }
    IdlePollScope(const IdlePollScope&) = delete;
    IdlePollScope& operator=(const IdlePollScope&) = delete;
};

// ---------------------------------------------------------------------------
// 锁序守卫（SCHED-01 / SCHED-02）
// ---------------------------------------------------------------------------
//
// 统一锁序约定：admission(m_impl->mutex) -> 条带锁(registry/incoming)。
// `enqueue()` 会获取 admission，因此任何持有 incoming 条带锁的路径都不能
// 调用它：那会形成 incoming -> admission 的反向嵌套，与 shutdown 的
// admission -> incoming 顺序相撞（SCHED-02）。慢路径曾经在持 admission 时
// 调用 enqueue()，同一把非递归 mutex 二次加锁直接自锁（SCHED-01）。
//
// 这里用线程局部深度把 incoming 登记为叶子锁；`enqueue()` 入口在深度非零时
// fail-fast。正常路径的代价只是一次 TLS 读，而触发窗口极窄的死锁会变成
// 可定位的显式失败。
thread_local std::uint32_t t_incoming_lock_depth = 0;

/// @brief 持 incoming 条带锁并登记持锁深度；析构时先解锁再清深度。
class IncomingStripeLock final {
public:
    explicit IncomingStripeLock(std::mutex& mutex) : m_mutex(mutex) {
        m_mutex.lock();
        ++t_incoming_lock_depth;
    }
    ~IncomingStripeLock() {
        m_mutex.unlock();
        --t_incoming_lock_depth;
    }
    IncomingStripeLock(const IncomingStripeLock&) = delete;
    IncomingStripeLock& operator=(const IncomingStripeLock&) = delete;

private:
    std::mutex& m_mutex;
};

// 默认 P 数量 = 硬件并发度。`hardware_concurrency()` 在无法确定时返回 0，
// 这里兜底为 1 —— 返回 0 会让调度器算出 0 个 P，worker 一个都不启。
std::size_t default_processor_count() noexcept {
    const auto count = std::thread::hardware_concurrency();
    return count == 0 ? 1U : static_cast<std::size_t>(count);
}

std::int64_t steady_now_ns() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// 毫秒转纳秒，**饱和**而非回绕：超过可表示范围时返回 int64 最大值。
// 这样"超长超时"会被当成"几乎无限"；若回绕成负数，等待会立即返回，
// 语义就从"等很久"变成"不等"，是最危险的那种错。
std::int64_t milliseconds_to_ns(
    std::chrono::milliseconds duration) noexcept {
    constexpr auto max_value = std::numeric_limits<std::int64_t>::max();
    const auto milliseconds = duration.count();
    if (milliseconds <= 0) {
        return 0;
    }
    constexpr auto scale = static_cast<std::int64_t>(1000000);
    if (milliseconds > max_value / scale) {
        return max_value;
    }
    return milliseconds * scale;
}

}  // namespace

// Tasks retain only a weak handle. This short gate excludes Scheduler
// destruction from an in-flight external Task::Cancel without retaining the
// Scheduler itself or holding an admission lock while entering the gate.
struct TaskCancellationGate {
    explicit TaskCancellationGate(Scheduler* scheduler)
        : m_scheduler(scheduler) {}

    std::mutex m_mutex;
    Scheduler* m_scheduler;
};

Task::Task(Function function, TaskOptions options)
    : m_id(s_next_g_id.fetch_add(1, std::memory_order_relaxed)),
      m_function(std::move(function)),
      m_options(options),
      m_state(GState::kNew) {}

Task::~Task() = default;

Task::Function Task::release_callable() noexcept {
    std::lock_guard<std::mutex> lock(m_transition_mutex);
    return std::move(m_function);
}

GId Task::id() const noexcept {
    return m_id;
}

GState Task::state() const noexcept {
    return m_state.load(std::memory_order_acquire);
}

bool Task::queued() const noexcept {
    return m_queued.load(std::memory_order_acquire);
}

bool Task::started() const noexcept {
    return m_started.load(std::memory_order_acquire);
}

bool Task::cancellation_requested() const noexcept {
    return m_cancel_requested.load(std::memory_order_acquire);
}

TaskClassId Task::task_class() const noexcept {
    return m_options.task_class;
}

bool Task::failed() const noexcept {
    return state() == GState::kFailed;
}

std::exception_ptr Task::failure() const {
    std::lock_guard<std::mutex> lock(m_failure_mutex);
    return m_failure;
}

void Task::rethrow_failure() const {
    const auto error = failure();
    if (error) {
        std::rethrow_exception(error);
    }
}

bool Task::terminal() const noexcept {
    const auto current = state();
    return current == GState::kDead || current == GState::kCancelled ||
           current == GState::kFailed;
}

bool Task::wait() const {
    if (Scheduler::current_task().get() == this) {
        return false;
    }
    std::unique_lock<std::mutex> lock(m_completion_mutex);
    return m_completion_condition.wait(lock, [this] { return terminal(); });
}

bool Task::wait_for(std::chrono::steady_clock::duration timeout) const {
    if (Scheduler::current_task().get() == this) {
        return false;
    }
    std::unique_lock<std::mutex> lock(m_completion_mutex);
    return m_completion_condition.wait_for(
        lock, timeout, [this] { return terminal(); });
}

void Task::notify_terminal() noexcept {
    if (m_completion_notified.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    // Synchronize with a waiter's predicate -> cv.wait transition. Merely
    // notifying an atomic terminal flag can lose the notification between
    // those two steps while the waiter still owns completion_mutex.
    {
        std::lock_guard<std::mutex> lock(m_completion_mutex);
    }
    m_completion_condition.notify_all();
}

bool Task::try_mark_queued() noexcept {
    std::lock_guard<std::mutex> lock(m_transition_mutex);
    if (m_state.load(std::memory_order_relaxed) != GState::kRunnable ||
        m_execution_claim.load(std::memory_order_relaxed) ||
        m_queued.load(std::memory_order_relaxed)) {
        return false;
    }
    m_queued.store(true, std::memory_order_release);
    return true;
}

bool Task::try_mark_queued_from_worker() noexcept {
    // 与 try_mark_queued() 共用 transition_mutex，让 m_queued 的
    // 「检查 + 置位」只有一个线性化点。原实现用无锁 CAS，而
    // try_mark_queued() 是「持锁检查再 store」；两者并发时都能成功，
    // 于是同一个 G 被发布两个队列节点。残留节点被 worker 取出后
    // try_mark_running() 失败，其 clear_queued() 又可能误清另一个节点
    // 的 reservation，最终 G 停在 Runnable 且没有任何队列节点，表现为
    // 「fiber 唤醒丢失」：worker 全部空闲、队列全空、被唤醒的 G 永不运行。
    // 该函数只在本地回队/IO 批量唤醒路径调用，未竞争时加锁开销可忽略。
    std::lock_guard<std::mutex> lock(m_transition_mutex);
    if (m_state.load(std::memory_order_relaxed) != GState::kRunnable ||
        m_execution_claim.load(std::memory_order_relaxed) ||
        m_queued.load(std::memory_order_relaxed)) {
        return false;
    }
    m_queued.store(true, std::memory_order_release);
    return true;
}

void Task::clear_queued() noexcept {
    std::lock_guard<std::mutex> lock(m_transition_mutex);
    m_queued.store(false, std::memory_order_release);
}

void Task::defer_enqueue() noexcept {
    m_deferred_enqueue.store(true, std::memory_order_release);
}

bool Task::consume_deferred_enqueue() noexcept {
    return m_deferred_enqueue.exchange(false, std::memory_order_acq_rel);
}

void Task::request_wake() noexcept {
    std::lock_guard<std::mutex> lock(m_transition_mutex);
    m_wake_pending.store(true, std::memory_order_release);
}

bool Task::consume_wake() noexcept {
    std::lock_guard<std::mutex> lock(m_transition_mutex);
    return m_wake_pending.exchange(false, std::memory_order_acq_rel);
}

bool Task::try_register() noexcept {
    bool expected = false;
    return m_registered.compare_exchange_strong(expected, true,
                                                std::memory_order_acq_rel,
                                                std::memory_order_acquire);
}

bool Task::registered() const noexcept {
    return m_registered.load(std::memory_order_acquire);
}

bool Task::bind_owner(const std::shared_ptr<const void>& owner) noexcept {
    if (!owner) {
        return false;
    }
    std::lock_guard<std::mutex> lock(m_transition_mutex);
    if (!m_owner_anchor) {
        m_owner_anchor = owner;
        return true;
    }
    return m_owner_anchor.get() == owner.get();
}

bool Task::owned_by(const std::shared_ptr<const void>& owner) const noexcept {
    if (!owner) {
        return false;
    }
    std::lock_guard<std::mutex> lock(m_transition_mutex);
    return m_owner_anchor && m_owner_anchor.get() == owner.get();
}

void Task::bind_cancellation_gate(
    const std::shared_ptr<TaskCancellationGate>& gate) noexcept {
    std::lock_guard<std::mutex> lock(m_transition_mutex);
    m_cancellation_gate = gate;
}

bool Task::prepare_enqueue(
    const std::shared_ptr<const void>& owner,
    const std::shared_ptr<TaskCancellationGate>& gate) noexcept {
    if (!owner || !gate) {
        return false;
    }
    std::lock_guard<std::mutex> lock(m_transition_mutex);
    if (!m_owner_anchor) {
        m_owner_anchor = owner;
    } else if (m_owner_anchor.get() != owner.get()) {
        return false;
    }
    m_cancellation_gate = gate;
    const auto state = m_state.load(std::memory_order_relaxed);
    if (state == GState::kNew) {
        m_state.store(GState::kRunnable, std::memory_order_release);
    } else if (state != GState::kRunnable) {
        return false;
    }
    if (m_execution_claim.load(std::memory_order_relaxed) ||
        m_queued.load(std::memory_order_relaxed)) {
        return false;
    }
    m_queued.store(true, std::memory_order_release);
    return true;
}

bool Task::promote_new() noexcept {
    std::lock_guard<std::mutex> lock(m_transition_mutex);
    if (m_state.load(std::memory_order_relaxed) != GState::kNew) {
        return false;
    }
    m_state.store(GState::kRunnable, std::memory_order_release);
    return true;
}

WakeAction Task::wake_for_scheduler() noexcept {
    std::lock_guard<std::mutex> lock(m_transition_mutex);
    const auto state = m_state.load(std::memory_order_relaxed);
    if (state == GState::kWaiting) {
        m_wake_pending.store(false, std::memory_order_relaxed);
        m_state.store(GState::kRunnable, std::memory_order_release);
        if (m_execution_claim.load(std::memory_order_relaxed)) {
            // park() changed the logical state before the cooperative callable
            // returned. Let the owning worker publish the next run only after
            // it releases the execution claim.
            m_deferred_enqueue.store(true, std::memory_order_release);
            return WakeAction::kPending;
        }
        return WakeAction::kEnqueue;
    }
    if (state == GState::kRunning) {
        m_wake_pending.store(true, std::memory_order_release);
        return WakeAction::kPending;
    }
    return WakeAction::kRejected;
}

WakeAction Task::wake_for_io() noexcept {
    return wake_for_wait();
}

WakeAction Task::wake_for_wait() noexcept {
    // 与 park_for_scheduler() 共用转换锁。仅依赖原子状态不足以保护
    // wake_pending：park 可能先读到 Running 和 pending=false，随后 wake
    // 发布 pending=true，而 park 再把状态写成 Waiting，最终留下无人入队
    // 的 G。锁内完成状态转换和 permit 发布，确保 wake 要么被 park 消耗，
    // 要么观察到 Waiting 并负责将 G 转回 Runnable。
    std::lock_guard<std::mutex> lock(m_transition_mutex);
    const GState state = m_state.load(std::memory_order_relaxed);
    if (state == GState::kWaiting) {
        m_wake_pending.store(false, std::memory_order_release);
        m_state.store(GState::kRunnable, std::memory_order_release);
        if (m_execution_claim.load(std::memory_order_acquire)) {
            m_deferred_enqueue.store(true, std::memory_order_release);
            return WakeAction::kPending;
        }
        return WakeAction::kEnqueue;
    }
    if (state == GState::kRunning) {
        m_wake_pending.store(true, std::memory_order_release);
        return WakeAction::kPending;
    }
    return WakeAction::kRejected;
}

/**
 * @brief 把当前 G 从 `kRunning` 提交为 `kWaiting`，并告诉调用方是否真的让出。
 * @return
 * - `kParked`：挂起已提交，调用方**必须**切走 Fiber（当前 G 已不是 running）；
 * - `kRequeued`：Fiber 提交挂起前已收到唤醒，**不要切走**，继续在当前执行流跑；
 * - `kRejected`：当前 G 不是 `kRunning`（例如已被取消），调用方按普通返回处理。
 *
 * ## 定位
 *
 * 这是**所有 park 路径的唯一提交点**：`sync.cpp` 的等待、`channel` 的阻塞收发、
 * `ParkingCondition::wait` 最终都收敛到这里。它的返回值直接决定上层是否调用
 * `Fiber::Yield`，所以「返回值 → 切不切」这个契约一旦错就是死锁或丢唤醒。
 *
 * ## 步骤
 *
 * step1. **契约检查**（必须在状态转换之前）：若调用者确实是当前 G，检查
 *        它是否在 `SpinMutex` 临界区（`no_park_depth`，禁止）或持有普通
 *        `Mutex`（`held_mutexes`，允许但记日志）；
 * step2. 取 `m_transition_mutex`——状态转换与 `wake_pending` 的消费必须在
 *        同一临界区内，否则「先通知后挂起」会丢唤醒；
 * step3. 校验状态：不是 `kRunning` 就返回 `kRejected`（已被取消/已死）；
 * step4. **消费唤醒令牌**：`m_wake_pending.exchange(false)` 为真说明挂起前
 *        已经有人唤醒过，此时返回 `kRequeued`，让调用方留在当前执行流；
 * step5. 提交 `kWaiting` 并返回 `kParked`。
 *
 * ## 并发/失败语义
 *
 * - **契约检查必须在 step2 之前**：一旦返回 `kParked`，当前 G 就离开了执行流，
 *   再也无法可靠地定位「谁在持锁状态下 park」；
 * - 契约检查只在该 G 确实是当前 G 时执行——`park_for_scheduler` 是公开接口，
 *   外部线程误调时不应把别的 G 的计数算到这次调用头上；
 * - `noexcept`：本函数不分配、不加可能抛出的锁，失败只能通过返回值表达；
 * - **不变量**：返回 `kParked` 后 `m_state == kWaiting` 且 `m_wake_pending == false`；
 *   返回 `kRequeued` 后 `m_state` 仍为 `kRunning` 且令牌已被消费。
 */
ParkAction Task::park_for_scheduler() noexcept {
    // step1: 契约检查——必须在状态转换之前（返回 kParked 后就离开执行流了）。
    //
    // 只在 park 调用者确实是当前 G 时检查：park_for_scheduler() 是公开
    // 接口，外部线程误调时不应把别的 G 的计数算到这次调用头上。
    if (Scheduler::current_task_ptr() == this) {
        const std::uint32_t no_park_depth =
            m_no_park_depth.load(std::memory_order_relaxed);
        if (no_park_depth != 0U) {
            // 内层优先：SpinMutex 的「禁止」契约高于普通 Mutex 的「告警」。
            ReportParkInNoParkSection(*this, no_park_depth);
        } else if (m_held_mutexes.load(std::memory_order_relaxed) != 0U) {
            ReportParkWhileHoldingMutex(*this);
        }
    }
    // step2: 状态转换与令牌消费必须在同一临界区内，否则会丢唤醒。
    std::lock_guard<std::mutex> lock(m_transition_mutex);
    // step3: 只有 kRunning 才能 park；否则说明已被取消或已终态。
    if (m_state.load(std::memory_order_relaxed) != GState::kRunning) {
        return ParkAction::kRejected;
    }
    // step4: 挂起前已收到唤醒 → 消费令牌并留在当前执行流（kRequeued）。
    if (m_wake_pending.exchange(false, std::memory_order_acq_rel)) {
        // Fiber 提交挂起前已经收到唤醒。消费该令牌并留在当前执行流，
        // 避免先通知后挂起造成丢失唤醒。
        return ParkAction::kRequeued;
    }
    // step5: 提交挂起。
    m_state.store(GState::kWaiting, std::memory_order_release);
    return ParkAction::kParked;
}


bool Task::try_mark_running() {
    std::lock_guard<std::mutex> lock(m_transition_mutex);
    if (!m_queued.load(std::memory_order_relaxed) ||
        m_execution_claim.load(std::memory_order_relaxed) ||
        m_state.load(std::memory_order_relaxed) != GState::kRunnable) {
        return false;
    }
    // Dequeue reservation and the runnable -> running transition are one
    // indivisible Task operation. An external enqueue cannot republish the G
    // between these updates.
    m_queued.store(false, std::memory_order_relaxed);
    m_execution_claim.store(true, std::memory_order_relaxed);
    m_state.store(GState::kRunning, std::memory_order_release);
    return true;
}


bool Task::mark_runnable() {
    std::lock_guard<std::mutex> lock(m_transition_mutex);
    if (m_state.load(std::memory_order_relaxed) != GState::kWaiting) {
        return false;
    }
    m_state.store(GState::kRunnable, std::memory_order_release);
    m_wake_pending.store(false, std::memory_order_relaxed);
    return true;
}

bool Task::mark_yielded() {
    std::lock_guard<std::mutex> lock(m_transition_mutex);
    if (m_state.load(std::memory_order_relaxed) != GState::kRunning) {
        return false;
    }
    // A wake received while the G was running has been superseded by the
    // explicit yield: the G is already being requeued and must not consume
    // that old token on a later park.
    m_state.store(GState::kRunnable, std::memory_order_release);
    m_wake_pending.store(false, std::memory_order_release);
    return true;
}

bool Task::mark_yielded_fast() noexcept {
    GState expected = GState::kRunning;
    if (!m_state.compare_exchange_strong(expected, GState::kRunnable,
                                         std::memory_order_acq_rel,
                                         std::memory_order_acquire)) {
        return false;
    }
    m_wake_pending.store(false, std::memory_order_release);
    return true;
}

bool Task::mark_waiting() {
    std::lock_guard<std::mutex> lock(m_transition_mutex);
    if (m_state.load(std::memory_order_relaxed) != GState::kRunning) {
        return false;
    }
    m_state.store(GState::kWaiting, std::memory_order_release);
    return true;
}

bool Task::cancel() {
    std::shared_ptr<TaskCancellationGate> gate;
    {
        std::lock_guard<std::mutex> lock(m_transition_mutex);
        gate = m_cancellation_gate.lock();
    }
    const auto self = weak_from_this().lock();
    if (gate && self) {
        std::lock_guard<std::mutex> lock(gate->m_mutex);
        if (gate->m_scheduler) {
            return gate->m_scheduler->cancel(self);
        }
    }
    return request_cancel();
}

bool Task::request_cancel(bool notify) noexcept {
    bool became_terminal = false;
    {
        std::lock_guard<std::mutex> lock(m_transition_mutex);
        const auto state = m_state.load(std::memory_order_relaxed);
        if (state == GState::kDead || state == GState::kCancelled ||
            state == GState::kFailed) {
            return false;
        }
        m_cancel_requested.store(true, std::memory_order_release);
        if (m_fiber) {
            m_fiber->RequestCancellation();
        }
        // A started Fiber must resume to its trampoline so stack locals and
        // stack cleanup finishes normally. Only an unstarted G is terminal immediately.
        if (!m_started.load(std::memory_order_relaxed) &&
            !m_execution_claim.load(std::memory_order_relaxed)) {
            m_state.store(GState::kCancelled, std::memory_order_release);
            m_queued.store(false, std::memory_order_release);
            became_terminal = true;
        }
        m_wake_pending.store(false, std::memory_order_release);
        if (became_terminal) {
            m_deferred_enqueue.store(false, std::memory_order_release);
        }
    }
    if (became_terminal && notify) {
        notify_terminal();
    }
    return true;
}

bool Task::resume_after_failed_park() noexcept {
    std::lock_guard<std::mutex> lock(m_transition_mutex);
    const GState state = m_state.load(std::memory_order_relaxed);
    // 只有"执行权确实还握在当前执行流里"时才能声明回 Running。
    if (!m_execution_claim.load(std::memory_order_relaxed)) {
        return false;
    }
    // kWaiting：park_for_scheduler() 已经提交了挂起，但 Fiber 拒绝切换；
    // kRunnable：挂起提交的同时被唤醒方改成了可运行，但执行流并未交回
    // 调度器（deferred handoff），实际仍在运行。
    // 两种情况下 G 都还在本线程上继续执行，若不回滚成 kRunning，它要么
    // 以"等待者"身份永远留在 registry（无人唤醒），要么让后续
    // park_for_scheduler() 一律被拒绝而陷入空转。
    if (state != GState::kWaiting && state != GState::kRunnable) {
        return false;
    }
    m_state.store(GState::kRunning, std::memory_order_release);
    m_deferred_enqueue.store(false, std::memory_order_relaxed);
    m_wake_pending.store(false, std::memory_order_relaxed);
    return true;
}

bool Task::recover_orphan_running() noexcept {
    std::lock_guard<std::mutex> lock(m_transition_mutex);
    if (m_state.load(std::memory_order_relaxed) != GState::kRunning) {
        return false;
    }
    if (m_execution_claim.load(std::memory_order_relaxed)) {
        // 仍有执行者持有执行权：这不是孤儿，不能抢。
        return false;
    }
    m_state.store(GState::kRunnable, std::memory_order_release);
    m_deferred_enqueue.store(true, std::memory_order_release);
    return true;
}

bool Task::cancel_if_runnable_unqueued() noexcept {
    {
        std::lock_guard<std::mutex> lock(m_transition_mutex);
        if (m_state.load(std::memory_order_relaxed) != GState::kRunnable ||
            m_queued.load(std::memory_order_relaxed) ||
            m_execution_claim.load(std::memory_order_relaxed)) {
            return false;
        }
        if (m_started.load(std::memory_order_relaxed)) {
            // This helper is only an admission-failure rescue for an
            // unstarted G. A started Fiber must remain runnable and be
            // requeued/cancelled through Scheduler::cancel instead.
            return false;
        }
        m_cancel_requested.store(true, std::memory_order_release);
        m_state.store(GState::kCancelled, std::memory_order_release);
        m_wake_pending.store(false, std::memory_order_relaxed);
        m_deferred_enqueue.store(false, std::memory_order_relaxed);
    }
    notify_terminal();
    return true;
}

/**
 * @brief 在 worker 上执行这个 G（首次运行，或从 park 处恢复）。
 *
 * ## 前置条件
 *
 * 调用方必须已取得 `m_execution_claim`（出队时与 `m_queued` 在同一转换中取得），
 * 且只应在 worker 线程上调用。
 *
 * ## 步骤
 *
 * step1. 校验 `m_execution_claim`；没有就返回（执行权已丢，可能是因为 cancel）；
 * step2. `m_run_claim` CAS —— 防止两个执行者同时进入同一个 Fiber 栈帧。
 *        **CAS 失败的分支是本函数最容易写错的地方**：前一个执行者已释放
 *        execution_claim、但它的 `run()` 栈帧还在退栈（run_claim 未释放）。
 *        此时必须把 G 交回 `kRunnable` + 置 `m_deferred_enqueue` + 清
 *        `m_execution_claim`，否则它会永久停在 `kRunning` 且无人认领，
 *        `shutdown()` 永远等不到终态（真实事故：12 并发下 1/36 挂死）；
 * step3. 在 `m_transition_mutex` 内取 Fiber 指针（避免与析构竞争）；
 * step4. 首次运行则 `initialize` 栈与上下文，否则直接 resume 挂起点；
 * step5. 恢复执行；`run()` 返回时按 state 决定入队/终态，并在**同一临界区**释放
 *        两个 claim。
 *
 * ## 两个 claim 的关系（必须一起理解）
 *
 * | 位 | 含义 |
 * |---|---|
 * | `m_execution_claim` | 我拥有这个 G 的执行权（队列已出队） |
 * | `m_run_claim` | 我正在它的 `run()` 栈帧内（防两个执行者进同一栈） |
 *
 * 两者必须在**同一个临界区**翻转（见函数末尾注释）。晚一步释放 run_claim，
 * 新执行者就会先拿到 execution_claim、再在入口被 run_claim 拒绝并静默返回。
 */
void Task::run() {
    if (!m_execution_claim.load(std::memory_order_acquire)) {
        return;
    }
    bool expected = false;
    if (!m_run_claim.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        // 另一个执行者已经释放 execution_claim，但它的 run() 栈帧仍在退出
        // 过程中（run_claim 尚未释放）。本次执行权作废，必须把 G 交回可
        // 运行状态：否则它会永久停在 kRunning 且无人认领，
        // Scheduler::shutdown() 将永远等不到它的终态。
        std::lock_guard<std::mutex> lock(m_transition_mutex);
        if (m_state.load(std::memory_order_relaxed) == GState::kRunning) {
            m_state.store(GState::kRunnable, std::memory_order_release);
            m_deferred_enqueue.store(true, std::memory_order_release);
        }
        m_execution_claim.store(false, std::memory_order_release);
        return;
    }

    Fiber* fiber = nullptr;
    // 本次 run() 是否刚刚创建了执行栈：用于区分「首次启动」与「再次恢复」。
    bool created_fiber_now = false;
    {
        std::lock_guard<std::mutex> lock(m_transition_mutex);
        fiber = m_fiber.get();
    }
    if (fiber == nullptr) {
        try {
            const auto stack_size = m_options.stack_size == 0
                                        ? Fiber::DefaultStackSize()
                                        : m_options.stack_size;
            const auto stack_reserve_size = m_options.stack_reserve_size;
            auto created_fiber = Fiber::AcquireForScheduler(
                [this] {
                    // Move the callable onto the G stack. Its captures are
                    // released without any scheduler lock held when the body
                    // returns or the Fiber is deliberately unwound.
                    auto body = std::move(m_function);
                    if (body) {
                        // Fiber::entry catches ordinary C++ exceptions and
                        // exposes them through Fiber::failure(). User code must
                        // handle ordinary C++ exceptions explicitly.
                        body();
                    }
                },
                stack_size, m_options.fiber_bin_capacity,
                stack_reserve_size, m_options.stack_growable);
            {
                std::lock_guard<std::mutex> lock(m_transition_mutex);
                if (!m_fiber) {
                    m_fiber = std::move(created_fiber);
                    m_started.store(true, std::memory_order_release);
                    created_fiber_now = true;
                }
                fiber = m_fiber.get();
            }
        } catch (...) {
            const auto error = std::current_exception();
            {
                std::lock_guard<std::mutex> lock(m_failure_mutex);
                m_failure = error;
            }
            {
                std::lock_guard<std::mutex> lock(m_transition_mutex);
                m_state.store(GState::kFailed, std::memory_order_release);
                m_execution_claim.store(false, std::memory_order_release);
                m_run_claim.store(false, std::memory_order_release);
            }
            auto abandoned_function = release_callable();
            notify_terminal();
            return;
        }
    }

    // Cancellation may have raced with Fiber construction before the new
    // pointer was published. The task flag is authoritative; mirror it into
    // the Fiber before entering its trampoline.
    if (m_cancel_requested.load(std::memory_order_acquire)) {
        fiber->RequestCancellation();
    }
    // 发布 G/M/P 绑定。Fiber 首次进入时会继承该绑定，嵌套 Fiber 之后
    // 只能沿同一逻辑 G 的父链恢复，即使 G 迁移到另一个 M 也不会丢失
    // 调度器归属。
    if (Scheduler* const scheduler = Scheduler::current_scheduler()) {
        // 同一个 M 上的连续 yield 不需要重复获取 Fiber 元数据锁。发生
        // 迁移时才更新调试绑定，保留跨 M 运行的可观测性。
        if (m_last_machine_id != t_machine_id ||
            m_last_processor_id.load(std::memory_order_relaxed) !=
                t_processor_id ||
            !m_binding_published) {
            fiber->bind_execution(FiberExecutionBinding{
                reinterpret_cast<std::uintptr_t>(scheduler), id(),
                t_machine_id, t_processor_id, true});
            m_last_machine_id = t_machine_id;
            m_last_processor_id.store(t_processor_id,
                                      std::memory_order_relaxed);
            m_binding_published = true;
        }
    }
    // Only this run claim can move/destroy m_fiber, and it does so after
    // resume() returns while holding m_transition_mutex. A concurrent cancel
    // can therefore safely request cancellation on this stable pointer.
    // G 生命周期：首次 resume 是 start，之后是 resumed（arg2=M）。
    if (Scheduler::current_scheduler() != nullptr) {
        if (created_fiber_now) {
            GO2CPP_DEBUG_EVENT(kGStarted, id(),
                               static_cast<std::uint64_t>(t_machine_id));
        } else {
            GO2CPP_DEBUG_EVENT(kGResumed, id(),
                               static_cast<std::uint64_t>(t_machine_id));
        }
    }
    (void)created_fiber_now;
    const bool resumed = Scheduler::current_scheduler() != nullptr
                             ? fiber->resume_from_scheduler()
                             : fiber->resume();
    const FiberState fiber_state = fiber->state();
    const auto fiber_failure = fiber_state == FiberState::Failed
                                   ? fiber->failure()
                                   : std::exception_ptr{};
    bool became_terminal = false;
    Function completed_function;
    std::unique_ptr<Fiber> completed_fiber;
    {
        std::lock_guard<std::mutex> lock(m_transition_mutex);
        if (!resumed || fiber_state == FiberState::Completed ||
            fiber_state == FiberState::Failed) {
            m_deferred_enqueue.store(false, std::memory_order_relaxed);
            m_wake_pending.store(false, std::memory_order_relaxed);
            if (fiber_failure) {
                std::lock_guard<std::mutex> failure_lock(m_failure_mutex);
                m_failure = fiber_failure;
            }
            const auto terminal_state = fiber_state == FiberState::Failed
                                            ? GState::kFailed
                                            : (m_cancel_requested.load(
                                                   std::memory_order_relaxed)
                                                   ? GState::kCancelled
                                                   : GState::kDead);
            m_state.store(terminal_state, std::memory_order_release);
            // A cancelled-before-first-resume Fiber can still retain the
            // callable. Move it out while updating state, then destroy it
            // after the transition lock is released.
            completed_function = std::move(m_function);
            became_terminal = true;
            // The trampoline has returned, so no C++ stack is still active on
            // this Fiber. Release its protected stack even when a caller
            // retains the public Task handle after completion; retaining a
            // Task must not retain an already-finished G stack.
            completed_fiber = std::move(m_fiber);
        } else if (fiber_state == FiberState::Suspended &&
                   m_state.load(std::memory_order_relaxed) ==
                       GState::kRunning) {
            // Direct Fiber::Suspend calls are supported for advanced users.
            // Scheduler helpers set the state before switching, while this
            // fallback maps a raw suspension to the matching queue state.
            // A cancellation/wake can race with the context switch before
            // the Fiber has published Suspended. In that window the notifier
            // records a pending permit while the state is still Running;
            // never overwrite that permit with Waiting or shutdown can strand
            // the G forever.
            const bool pending_wake =
                m_wake_pending.load(std::memory_order_relaxed);
            const bool cancellation_requested =
                m_cancel_requested.load(std::memory_order_relaxed);
            if (fiber->suspend_reason() == SuspendReason::Yield ||
                pending_wake || cancellation_requested) {
                m_state.store(GState::kRunnable, std::memory_order_release);
                m_deferred_enqueue.store(true, std::memory_order_release);
                m_wake_pending.store(false, std::memory_order_release);
            } else {
                m_state.store(GState::kWaiting, std::memory_order_release);
            }
        }
        m_execution_claim.store(false, std::memory_order_release);
        // run_claim 必须与 execution_claim 在同一个临界区内释放。若晚一步
        // （例如交给 RAII 在函数返回时释放），新执行者会先拿到
        // execution_claim，再在 run() 入口被 run_claim 拒绝并静默返回，
        // 把 G 永久留在 kRunning。这两个位共同描述"谁在执行这个 G"，
        // 必须原子地一起翻转。
        m_run_claim.store(false, std::memory_order_release);
    }
    if (became_terminal) {
        const GState final_state = m_state.load(std::memory_order_relaxed);
        if (final_state == GState::kFailed) {
            GO2CPP_DEBUG_EVENT(
                kGFailed, id(),
                static_cast<std::uint64_t>(final_state));
        } else if (final_state == GState::kCancelled) {
            GO2CPP_DEBUG_EVENT(
                kGCancelled, id(),
                static_cast<std::uint64_t>(final_state));
        } else {
            GO2CPP_DEBUG_EVENT(
                kGCompleted, id(),
                static_cast<std::uint64_t>(final_state));
        }
        if (fiber_failure) {
            auto logger = GO2CPP_LOG_NAME("Scheduler");
            GO2CPP_LOG_ERROR(logger) << "Fiber task " << m_id
                                     << " failed with an uncaught exception";
        }
        notify_terminal();
        Fiber::RecycleForScheduler(std::move(completed_fiber),
                                   m_options.fiber_bin_capacity);
    }
}

/**
 * @brief `Scheduler` 的完整实现（PIMPL；`Scheduler` 本身只是门面）。
 *
 * ## 组成（GMP 三层 + 两个条带表）
 *
 * | 成员 | 角色 |
 * |---|---|
 * | `m_processors` | P 数组（`processor_count` 个），每个带本地 runqueue |
 * | `m_machines` | M 数组（基础 M + 按需补充的 replacement M） |
 * | `m_registry`（16 条带） | 所有**未终态** G 的强引用；shutdown 靠它等终态 |
 * | `m_incoming`（16 条带） | 外部生产者投递的新 G，worker 轮询认领 |
 * | `m_tasks`/`m_state` | admission 计数与运行状态 |
 *
 * ## 锁层次（严格顺序，违反就会死锁）
 *
 * ```text
 * m_lifecycle_mutex -> m_mutex(admission) -> 条带锁（registry/incoming）
 *                  -> P 的 mutex -> m_task_mutex（单 G 状态）
 * ```
 *
 * 任何路径都**不能反向**。`expire_timers` 特意分两阶段（定时器锁内摘节点、
 * 释放后逐条带处理）就是为了不出现「条带锁 → 定时器锁」的反向嵌套。
 *
 * **incoming 条带锁是叶子锁**：持锁期间只允许操作该条带的队列/计数与
 * Task 状态机，**禁止调用 `enqueue()`** —— 它会获取 admission 锁。
 * 旧实现的 B1 补发布正是在 `take_incoming()` 持 incoming 锁时调用
 * `enqueue()`，与 shutdown 的 `admission -> incoming` 顺序构成反向嵌套
 * （SCHED-02）；慢路径持 admission 调 `enqueue()` 则直接自锁（SCHED-01）。
 * 两条路径现在统一为「锁内只判定，释放锁后补发布」。`IncomingStripeLock`
 * 用线程局部计数登记 incoming 持锁深度，`enqueue()` 入口据此 fail-fast，
 * 防止以后再把跨锁调用引回来。
 *
 * ## 关键不变式
 *
 * - 注册表条目 = 一个尚未终态的 G，或**持锁退出**的终态 G（Doctor
 *   force-unlock 判定 owner-gone 的证据）；其余终态 G 由 worker 循环按
 *   节拍轮转回收，不再随历史完成任务数增长；
 * - `incoming` 里的 G 一定**没有 P**（`processor == kInvalidPId`）；
 * - `blocking_workers` 归零前 sysmon 不会再申请补充 M；
 * - `shutdown` 期间 admission 关闭，`enqueue` 失败后调用方必须 `cancel()`。
 */
class Scheduler::Impl {
public:
    // 任务注册表按固定条带拆分，避免大量外部生产者在提交短任务时
    // 争用同一把互斥量。每个条带仍然是普通 vector + mutex，关闭和
    // 回收阶段按条带逐一加锁，因此不会改变注册表的强所有权语义。
    static constexpr std::size_t kRegistryStripeCount = 16;
    // 外部生产者入队也按固定条带拆分。它们只负责发布尚未绑定 P
    // 的新 G，worker 会轮询这些条带并按原有的状态机认领任务。
    static constexpr std::size_t kIncomingStripeCount = 16;

    /**
     * @brief 逻辑处理器 P：一个本地 runqueue + 活动 M 计数。
     *
     * ## 定位
     *
     * P 的数量（`processor_count`）决定真正的并行度。M 必须附着到一个 P 才能取任务；
     * G 在 P 的**本地队列**里排队，队列空时从别的 P 窃取（work stealing）。
     *
     * ## 为什么有本地队列
     *
     * 这是 GMP 的核心优化：worker 把 G 放回自己的 P 只需要一把 P 队列锁
     * （`requeue_from_worker`），只有跨 P 才走更重的路径。§1.7 记录过「把窃取
     * 移出全局锁」的尝试反而 -34%，所以本地队列边界不要动。
     *
     * ## 不变量
     *
     * - `queue` 由 `mutex` 保护；`state`/`active_machines`/`running_machines` 是原子
     *   （快照可在锁外读，但**增减在 `Impl::mutex` 内**）；
     * - `running_machines == 0` ⇒ 该 P 当前没有 M 在执行 G（唤醒判定用它）；
     * - `active_machines` 含被 blocking/detached 的 M。
     */
    struct Processor {
        explicit Processor(PId processor_id) : id(processor_id) {}

        /// @brief P 编号（0 起，0 合法；不要用 `!=0` 判断存在）。
        PId id;
        /// @brief 保护 queue；本地入队/出队/窃取都拿它。
        mutable std::mutex mutex;
        /// @brief 本地 runqueue（FIFO）；worker 优先从这里取 G。
        std::deque<std::shared_ptr<Task>> queue;
        /// @brief 快照用状态（idle/running/dead）。
        std::atomic<PState> state{PState::kIdle};
        /// @brief 附着到本 P 的 M 总数（含 blocking/detached 的 M）。
        std::atomic<std::size_t> active_machines{0};
        /// @brief 正在执行 G 的 M 数；唤醒判定用它区分「有人干活」与「都在等」。
        std::atomic<std::size_t> running_machines{0};
    };

    /**
     * @brief OS 线程 M：一个 `std::thread` + 首选 P + 阻塞记账。
     *
     * ## 定位
     *
     * M 是唯一真正执行代码的实体，由 `worker_loop` 驱动。两类 M 的生命周期不同：
     *
     * | 类型 | `replacement` | 创建时机 | 退休条件 |
     * |---|---|---|---|
     * | **基础 M** | false | 启动时创建 `min_workers` 个 | **永不因空闲退出**，常驻到 shutdown |
     * | **补充 M** | true | `blocking_workers > 0` 时 | `blocking_workers` 归零后**立即退休** |
     *
     * ## 阻塞记账（sysmon 用）
     *
     * `processor_detached` / `blocking_since_ns` / `blocking_task` 由 `BlockingRegion`
     * 与 sysmon 共同读写：detach 后该 M 不再计入 P 的 attached 计数，sysmon 可据
     * `long_syscall_threshold` 为它申请一个替代 M。
     *
     * ## 不变量
     *
     * - `replacement` 在线程启动前写一次、之后只读；
     * - 所有字段要么由 worker 原子更新、要么在 `Impl::mutex` 下修改 ——
     *   **没有「worker 不持锁却做非原子写」的例外**（`machines()` 快照会在别的线程读）；
     * - `machine id` 全局递增、不从 0 开始，不要断言 `machine.0`。
     */
    struct Machine {
        explicit Machine(MId machine_id, PId processor_id,
                         bool is_replacement)
            : id(machine_id),
              processor(processor_id),
              replacement(is_replacement) {}

        MId id;
        PId processor;
        // 补充 M 标记：spawn 时在线程启动前写入一次，之后只读。基础 M 为
        // false，常驻到 shutdown；补充 M 在阻塞区结束后立即退休。
        bool replacement{false};
        std::thread thread;
        std::atomic<MState> state{MState::kIdle};
        // 这些字段由对应 worker 更新，同时允许 machines() 在另一个线程
        // 读取；即使 worker 不持有调度器总锁，也必须使用原子访问。
        std::atomic<TaskClassId> last_task_class{0};
        std::atomic<std::size_t> affinity_budget{0};
        std::atomic<std::size_t> affinity_hits{0};
        std::atomic<std::size_t> affinity_misses{0};
        // 这些原子字段由 worker、BlockingRegion 和 sysmon 共同观察；
        // P 计数的增减仍在 Impl::mutex 下完成。
        std::atomic<bool> processor_detached{false};
        std::atomic<std::int64_t> blocking_since_ns{0};
        std::atomic<GId> blocking_task{0};
        std::atomic<std::uint64_t> long_syscall_count{0};
    };

    explicit Impl(SchedulerConfig requested, Scheduler* scheduler)
        : config(normalize(requested)),
          owner_token(std::make_shared<const std::uint8_t>(0)),
          cancellation_gate(std::make_shared<TaskCancellationGate>(scheduler)),
          observer(std::move(config.observer)) {
        // 观察者也可能是**通过 config 装入**的（测试与生产代码都这么做），
        // 因此这里必须同样置位快路径标志；只覆盖 set_observer() 会让
        // config 路径装进来的观察者被 emit_event 的快路径静默跳过。
        has_observer.store(observer != nullptr,
                           std::memory_order_release);
        for (std::size_t i = 0; i < config.processor_count; ++i) {
            processors.emplace_back(static_cast<PId>(i));
        }
        // 高并发提交时注册表按条带分别扩容，避免 producer burst 反复搬迁
        // shared_ptr；超出后仍按 vector 原有策略增长，不改变上限语义。
        for (auto& registry : task_registries) {
            registry.reserve(4096 / kRegistryStripeCount);
        }
    }

    /**
     * @brief IO 空闲轮询回调（IOManager 注入，见 Scheduler::set_io_idle_poller）。
     * @details poll 在 worker 线程里直接做 epoll_wait；wake 写 eventfd 把正
     *          睡在 epoll_wait 上的 worker 踢醒。二者都只在安装了回调后使用。
     */
    struct IoIdlePoller final {
        std::function<int(int)> poll;
        std::function<void()> wake;
    };

    /**
     * @brief 通知一个空闲 worker 可能有新工作。
     * @details 条件变量通知只对睡在调度器条件变量上的 worker 有效；当有
     *          worker 正睡在 IO 后端的 epoll_wait 上时，还必须写一次 eventfd，
     *          否则它会一直等到自己的超时。两者都只是提示：worker 被唤醒后
     *          必然重新检查队列，因此不会丢工作。
     */
    void notify_worker() noexcept {
        condition.notify_one();
        const IoIdlePoller* poller = io_poller.load(std::memory_order_acquire);
        if (poller != nullptr && poller->wake &&
            io_waiting_workers.load(std::memory_order_acquire) != 0) {
            poller->wake();
        }
    }

    /// @brief notify_worker 的广播版本；语义相同，供 shutdown/draining 使用。
    void notify_all_workers() noexcept {
        condition.notify_all();
        const IoIdlePoller* poller = io_poller.load(std::memory_order_acquire);
        if (poller != nullptr && poller->wake &&
            io_waiting_workers.load(std::memory_order_acquire) != 0) {
            poller->wake();
        }
    }

    static std::size_t registry_stripe(const Task& task) noexcept {
        // GId 单调递增，低位分片即可；条带数为 2 的幂，编译器会将
        // 模运算优化为按位与。
        return static_cast<std::size_t>(task.id()) &
               (kRegistryStripeCount - 1);
    }

    ~Impl() { stop_sysmon(true); }

    /**
     * @brief 向当前观察者发布调度器事件。
     * @param event 已填充的事件快照。
     * @note 复制观察者后再释放互斥锁，绝不在调度器内部状态锁上执行
     *       外部代码；观察者异常被隔离，不能影响 worker 生命周期。
     */
    void emit_event(SchedulerEvent event) noexcept {
        // 快路径：没有观察者就直接返回（不取时钟、不加锁、不拷贝 shared_ptr）。
        // 安装观察者之后走原有完整路径，语义不变。
        if (!has_observer.load(std::memory_order_acquire)) {
            return;
        }
        if (event.timestamp == std::chrono::steady_clock::time_point{}) {
            event.timestamp = std::chrono::steady_clock::now();
        }
        SchedulerObserverPtr current;
        {
            std::lock_guard<std::mutex> lock(observer_mutex);
            current = observer;
        }
        if (!current) {
            return;
        }
        try {
            current->OnEvent(event);
        } catch (...) {
            // 观察者属于可选插件，异常不能跨越调度器边界。
        }
    }

    static SchedulerConfig normalize(SchedulerConfig requested) {
        if (requested.processor_count == 0) {
            // 默认 = 硬件并发度（与 Go 的 GOMAXPROCS 语义对齐）。
            //
            // 区分实验（docs/parameter-tuning.md 轮次 6）证明 **P 数量本身超配无效**：
            //   D(P=48, M=12) 27,809 qps ≈ A(P=12, M=12) 27,786 qps
            // 即把 P 从 12 加到 48、但 M 保持 12，QPS 完全不变。P 只是逻辑队列，
            // 超配不增加并行度。真正的收益来自常驻 M 数量（见 min_workers）。
            requested.processor_count = default_processor_count();
        }
        requested.processor_count = std::max<std::size_t>(1,
                                                           requested.processor_count);
        if (requested.max_workers == 0) {
            // Keep ordinary runnable demand P-bounded, but reserve a bounded
            // replacement ceiling for explicitly declared native blocking M's.
            // This lets the default scheduler make progress without requiring
            // every caller to guess a max-worker value; callers can still set
            // an exact max or disable overcommit below.
            requested.max_workers =
                requested.allow_worker_oversubscription &&
                        requested.processor_count <=
                            std::numeric_limits<std::size_t>::max() / 2
                    ? requested.processor_count * 2
                    : std::numeric_limits<std::size_t>::max();
        }
        requested.max_workers = std::max<std::size_t>(1, requested.max_workers);
        // A strict one-M-per-P ceiling is still available for deployments
        // that set allow_worker_oversubscription=false.  The default permits
        // declared native blocking regions to grow replacement M workers;
        // this is an intentional overcommit, not a claim of asynchronous
        // preemption for arbitrary native calls.
        if (!requested.allow_worker_oversubscription) {
            requested.max_workers =
                std::min(requested.max_workers, requested.processor_count);
        }
        if (requested.min_workers == 0) {
            // 默认 = P（Go 风格：常驻 M 数等于 P，保证每个 P 都有承载者）。
            //
            // 曾试过改成 2 × P：性能确有 +11.7%（s4 计算注入 27,786 → 31,032），
            // 但 **破坏语义** —— tests/network/test_hook.cpp 的 
            // read_f 用例要求
            // 「阻塞一个 M 后整个 P 停摆、心跳近乎平直」，常驻 M 翻倍后备用 M 继续
            // 跑心跳，测试失败。性能收益不能以改变阻塞语义为代价，故回退。
            //
            // 需要更多常驻 M 的部署可以显式设 min_workers（配置项），
            // 但要自行承担上述语义变化。
            requested.min_workers = requested.processor_count;
        }
        requested.min_workers = std::max<std::size_t>(1, requested.min_workers);
        requested.min_workers = std::min(requested.min_workers,
                                         requested.max_workers);
        // FiberBin 只用于短期突发复用；限制单个 M 的保留对象数，避免
        // 配置误写为极大值后让每个 worker 长期占用大量堆内存。
        requested.fiber_bin_capacity = std::min<std::size_t>(
            requested.fiber_bin_capacity, 4096U);
        if (requested.idle_wait <= std::chrono::milliseconds::zero()) {
            requested.idle_wait = std::chrono::milliseconds(1);
        }
        if (requested.idle_worker_timeout <= std::chrono::milliseconds::zero()) {
            requested.idle_worker_timeout = std::chrono::milliseconds(1);
        }
        if (requested.sysmon_interval <= std::chrono::milliseconds::zero()) {
            requested.sysmon_interval = std::chrono::milliseconds(1);
        }
        if (requested.long_syscall_threshold <=
            std::chrono::milliseconds::zero()) {
            requested.long_syscall_threshold = requested.sysmon_interval;
        }
        return requested;
    }

    // Create one worker while mutex is held. The thread starts only after its
    // shared Machine object has been published, so snapshots and shutdown can
    // safely retain the same object even when later workers are appended.
    // is_replacement=true 表示这是为阻塞区补位的动态 M：阻塞结束后可退休。
    bool spawn_worker_locked(Scheduler* scheduler, bool is_replacement) {
        if (!scheduler || stopping.load(std::memory_order_acquire) ||
            machines.size() >= std::numeric_limits<std::size_t>::max() ||
            active_workers.load(std::memory_order_relaxed) >=
                config.max_workers) {
            return false;
        }

        PId processor_id = 0;
        std::size_t least = std::numeric_limits<std::size_t>::max();
        for (const auto& processor : processors) {
            const auto active =
                processor.active_machines.load(std::memory_order_relaxed);
            if (active < least) {
                least = active;
                processor_id = processor.id;
            }
        }

        auto machine = std::make_shared<Machine>(
            s_next_m_id.fetch_add(1, std::memory_order_relaxed), processor_id,
            is_replacement);
        machines.emplace_back(machine);
        active_workers.fetch_add(1, std::memory_order_relaxed);
        processors[processor_id].active_machines.fetch_add(
            1, std::memory_order_relaxed);
        try {
            machine->thread = std::thread([scheduler, machine] {
                scheduler->worker_loop(std::static_pointer_cast<void>(machine));
            });
        } catch (...) {
            processors[processor_id].active_machines.fetch_sub(
                1, std::memory_order_relaxed);
            active_workers.fetch_sub(1, std::memory_order_relaxed);
            machines.pop_back();
            throw;
        }
        return true;
    }

    void reap_dead_workers() {
        std::vector<std::thread> retired;
        const auto self = std::this_thread::get_id();
        // shutdown() owns join_mutex while draining Gs. A worker must not
        // block here during requeue, or shutdown would be waiting for a G
        // whose M is waiting for shutdown's join lock.
        std::unique_lock<std::mutex> join_lock(join_mutex, std::try_to_lock);
        if (!join_lock.owns_lock()) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            // Reserve before moving any joinable std::thread. If allocation
            // fails, maybe_grow() can leave the dead records for a later
            // retry; after this point thread moves are noexcept and cannot
            // make vector destruction terminate the process.
            retired.reserve(machines.size());
            for (auto it = machines.begin(); it != machines.end();) {
                const auto& machine = *it;
                if (!machine) {
                    it = machines.erase(it);
                } else if (machine->state.load(std::memory_order_acquire) ==
                               MState::kDead &&
                           machine->thread.get_id() != self) {
                    if (machine->thread.joinable()) {
                        retired.emplace_back(std::move(machine->thread));
                    }
                    it = machines.erase(it);
                } else {
                    ++it;
                }
            }
        }
        for (auto& thread : retired) {
            if (thread.joinable()) {
                thread.join();
            }
        }
        // Keep shutdown/reaper serialization through the actual join. The
        // retired worker records are already erased, but releasing the gate
        // earlier would let an owner observe a half-finished maintenance pass.
        join_lock.unlock();
    }

    // 统计当前仍处于 sysmon 逻辑解绑状态的 M。调用方必须持有 mutex；
    // 这样 sysmon 标记后、原 G 返回前的短竞态不会使用过期计数扩容。
    std::size_t detached_machine_count_locked() const noexcept {
        std::size_t count = 0;
        for (const auto& machine : machines) {
            if (machine &&
                machine->processor_detached.load(std::memory_order_acquire)) {
                ++count;
            }
        }
        return count;
    }

    // 普通入队只按 runnable demand 扩容；每个仍 detached 的长阻塞 M
    // 预留一个替代槽，即便当前队列暂时为空，也不会让 P 长时间失去
    // 可运行的 M。
    void maybe_grow(Scheduler* scheduler) {
        if (!scheduler) {
            return;
        }
        // 诊断旁路（默认不生效）：GO2CPP_NO_GROW=1 时直接跳过维护，
        // 用于判断 maybe_grow/reap_dead_workers 是否是 worker 劣化的来源。
        static const bool no_grow = [] {
            const char* v = std::getenv("GO2CPP_NO_GROW");
            return v != nullptr && v[0] == '1';
        }();
        if (no_grow) {
            return;
        }
        if (!started.load(std::memory_order_acquire) ||
            draining.load(std::memory_order_acquire) ||
            stopping.load(std::memory_order_acquire)) {
            return;
        }
        try {
            reap_dead_workers();
        } catch (...) {
            // Failure to allocate a temporary join list cannot invalidate
            // accepted Gs. The dead records remain safe for shutdown/retry.
        }
        std::unique_lock<std::mutex> lock(mutex);
        if (!started.load(std::memory_order_acquire) ||
            draining.load(std::memory_order_acquire) ||
            stopping.load(std::memory_order_acquire)) {
            return;
        }
        const auto queued = runnable.load(std::memory_order_acquire);
        const auto busy = running_workers.load(std::memory_order_acquire);
        const auto active = active_workers.load(std::memory_order_relaxed);
        const auto blocking = blocking_workers.load(std::memory_order_acquire);
        const auto detached = detached_machine_count_locked();
        const auto effective_blocking = std::max(blocking, detached);
        // Normal runnable bursts stay P-bounded.  Each explicitly declared
        // native blocking M contributes one replacement slot, up to the
        // configured max; this prevents a large queue from creating dozens
        // of threads merely because max_workers is generous.
        const auto extra_ceiling =
            effective_blocking > std::numeric_limits<std::size_t>::max() -
                              config.processor_count
                ? std::numeric_limits<std::size_t>::max()
                : config.processor_count + effective_blocking;
        const auto worker_ceiling = std::min(config.max_workers, extra_ceiling);
        const auto demand =
            queued > std::numeric_limits<std::size_t>::max() - busy
                ? std::numeric_limits<std::size_t>::max()
                : queued + busy;
        auto desired = std::min(worker_ceiling,
                                std::max(config.min_workers, demand));
        if (detached != 0) {
            // 替代槽只需要补足当前的最小 worker 底线；普通 runnable
            // 需求仍由上面的 P 有界 ceiling 控制。不能因为机器有很多
            // 个 P，就为一个长 syscall 无条件启动 P+1 个 M。
            const auto handoff_target = std::min(
                worker_ceiling,
                detached > std::numeric_limits<std::size_t>::max() -
                                  config.min_workers
                    ? std::numeric_limits<std::size_t>::max()
                    : config.min_workers + detached);
            desired = std::max(desired, handoff_target);
        }
        if (desired <= active) {
            return;
        }
        const auto to_add = desired - active;
        for (std::size_t i = 0; i < to_add; ++i) {
            // 基础槽位（active < min_workers）创建的是基础 M，常驻；超过
            // 常驻目标的都是为阻塞区补位的补充 M，随阻塞结束退休。
            const bool replacement =
                active_workers.load(std::memory_order_relaxed) >=
                config.min_workers;
            try {
                if (!spawn_worker_locked(scheduler, replacement)) {
                    break;
                }
            } catch (...) {
                // Thread-resource exhaustion degrades parallelism, not the
                // accepted Task lifetime. Existing M's retain all queue work.
                break;
            }
        }
        lock.unlock();
        notify_all_workers();
    }

    // sysmon 只做可逆的资源记账：它不会从别的线程跳转或终止正在
    // syscall 中的 C++ 栈。达到阈值后，M 继续执行原生调用，但从 P 的
    // attached 计数中移除，并按阻塞数申请替代 M。
    void sysmon_pass(Scheduler* scheduler) noexcept {
        if (!scheduler || !config.enable_sysmon) {
            return;
        }
        // 心跳先于调度器锁更新。高负载时即使本轮无法取得锁，
        // monitor 线程也不会永久卡在业务队列上，调用者仍能观察到它在线。
        const auto now = steady_now_ns();
        [[maybe_unused]] const auto pass_number =
            sysmon_pass_count.fetch_add(1, std::memory_order_relaxed) + 1;
        GO2CPP_DEBUG_EVENT_SAMPLED(kSysmonPass, 0, pass_number, 32);
        const auto threshold = milliseconds_to_ns(config.long_syscall_threshold);
        std::size_t detached_count = 0;
        {
            std::unique_lock<std::mutex> lock(mutex, std::try_to_lock);
            if (!lock.owns_lock()) {
                // 不要把一次锁竞争当成“本轮扫描完成”。worker 在高负载
                // 下可能连续持有主锁；保留重试标记让 monitor 缩短下一
                // 个周期，而不是静默丢失长 syscall 的解绑机会。
                sysmon_retry_pending.store(true, std::memory_order_release);
                return;
            }
            sysmon_retry_pending.store(false, std::memory_order_release);
            for (const auto& machine : machines) {
                if (!machine ||
                    machine->state.load(std::memory_order_acquire) !=
                        MState::kBlocking) {
                    continue;
                }
                const auto since = machine->blocking_since_ns.load(
                    std::memory_order_acquire);
                if (since <= 0 || now < since || now - since < threshold ||
                    machine->processor_detached.load(std::memory_order_acquire)) {
                    continue;
                }
                machine->processor_detached.store(true,
                                                   std::memory_order_release);
                machine->long_syscall_count.fetch_add(
                    1, std::memory_order_relaxed);
                const auto processor_id = machine->processor % processors.size();
                auto& processor = processors[processor_id];
                if (processor.active_machines.load(
                        std::memory_order_relaxed) != 0) {
                    processor.active_machines.fetch_sub(
                        1, std::memory_order_acq_rel);
                }
                if (processor.active_machines.load(
                        std::memory_order_relaxed) == 0 &&
                    !stopping.load(std::memory_order_acquire)) {
                    processor.state.store(PState::kIdle,
                                          std::memory_order_release);
                }
                ++detached_count;
            }
        }
        if (detached_count != 0) {
            maybe_grow(scheduler);
            notify_all_workers();
        }
    }

    void sysmon_loop(Scheduler* scheduler) noexcept {
        while (!sysmon_stop.load(std::memory_order_acquire)) {
            // sysmon 的节拍等待不能争用调度器主锁。worker 在高负载下可能
            // 持有主锁进行队列/生命周期处理；监控线程只应等待自己的停止
            // 条件，然后在扫描阶段用 try_to_lock 做一次有界尝试。
            const bool retry =
                sysmon_retry_pending.exchange(false, std::memory_order_acq_rel);
            // 锁竞争后的重试保持有界退避，避免 monitor 在主锁长期繁忙
            // 时忙等，同时确保一次失败扫描不会被完整周期掩盖。
            const auto wait_interval = retry
                                           ? std::min(
                                                 config.sysmon_interval,
                                                 std::chrono::milliseconds(1))
                                           : config.sysmon_interval;
            std::unique_lock<std::mutex> lock(sysmon_wait_mutex);
            sysmon_wait_condition.wait_for(lock, wait_interval, [this] {
                return sysmon_stop.load(std::memory_order_acquire) ||
                       stopping.load(std::memory_order_acquire) ||
                       sysmon_retry_pending.load(std::memory_order_acquire);
            });
            const bool stop = sysmon_stop.load(std::memory_order_acquire) ||
                              stopping.load(std::memory_order_acquire);
            lock.unlock();
            if (stop) {
                break;
            }
            sysmon_pass(scheduler);
        }
        sysmon_active.store(false, std::memory_order_release);
    }

    void stop_sysmon(bool join) noexcept {
        sysmon_stop.store(true, std::memory_order_release);
        sysmon_wait_condition.notify_all();
        if (join && sysmon_thread.joinable() &&
            sysmon_thread.get_id() != std::this_thread::get_id()) {
            sysmon_thread.join();
        }
    }

    // 终态 G 是否可以从注册表摘除。持锁退出的终态 G 是 Doctor
    // force-unlock 判定 owner-gone 的唯一证据（TaskTerminal 按 gid 查注册
    // 表），必须保留；这类 G 的数量等于泄漏的 sync::Mutex 数量，不随历史
    // 完成任务数增长，因此不会重新引入 D2 的线性内存增长。
    static bool reclaimable_terminal(
        const std::shared_ptr<Task>& task) noexcept {
        if (!task) {
            return true;
        }
        return task->terminal() && task->held_mutex_count() == 0U;
    }

    // 把一条终态 G 追加到调用方存储；容量不足时先扩容，扩容失败返回
    // false，调用方保留该条目（回收失败是安全的，下个节拍再试）。
    static bool append_reclaimed(
        std::vector<std::shared_ptr<Task>>& deferred_destruction,
        std::shared_ptr<Task>& task) noexcept {
        if (deferred_destruction.size() == deferred_destruction.capacity()) {
            const std::size_t target = deferred_destruction.capacity() == 0
                                           ? std::size_t{32}
                                           : deferred_destruction.capacity() * 2;
            try {
                deferred_destruction.reserve(target);
            } catch (...) {
                return false;
            }
        }
        // 容量已保证：shared_ptr 的移动构造不会抛异常。
        deferred_destruction.emplace_back(std::move(task));
        return true;
    }

    // 回收单个注册表条带。常规 worker 循环按节拍轮转调用它，单次扫描量
    // 与「存活 G 总数 / 16」成正比；shutdown/cancel 路径用下方全表版本。
    // 调用方不要求持有 Impl::mutex：条带内容只由 registry_mutexes 保护，
    // 锁层次（Impl::mutex -> 条带锁）保持不变。
    void collect_terminal_stripe_locked(
        std::size_t stripe,
        std::vector<std::shared_ptr<Task>>& deferred_destruction) noexcept {
        auto& task_registry = task_registries[stripe];
        std::lock_guard<std::mutex> registry_lock(registry_mutexes[stripe]);
        // 单次稳定压缩是 O(n)。逐项 vector::erase 会把大批任务完成时的
        // 回收变成 O(n²)。
        auto output = task_registry.begin();
        auto it = task_registry.begin();
        for (; it != task_registry.end(); ++it) {
            if (!reclaimable_terminal(*it)) {
                if (output != it) {
                    *output = std::move(*it);
                }
                ++output;
                continue;
            }
            if (*it && !append_reclaimed(deferred_destruction, *it)) {
                // 分配失败：保留当前及剩余条目，下个节拍再试。
                break;
            }
        }
        if (it != task_registry.end()) {
            // 中途退出时把尚未处理的条目原样前移，避免在注册表里留下
            // 已回收位置的空洞。
            for (; it != task_registry.end(); ++it) {
                if (output != it) {
                    *output = std::move(*it);
                }
                ++output;
            }
        }
        task_registry.erase(output, task_registry.end());
    }

    void collect_terminal_locked(
        std::vector<std::shared_ptr<Task>>& deferred_destruction) noexcept {
        // The registry strongly owns every non-terminal G, including a Fiber
        // parked outside a run queue. This prevents destruction of a suspended
        // stack. Move terminal entries to caller-owned storage so arbitrary
        // user captures are never destroyed while the scheduler lock is held.
        // 本函数只依赖条带锁；shutdown 路径由调用方额外持有 Impl::mutex。
        for (std::size_t stripe = 0; stripe < kRegistryStripeCount;
             ++stripe) {
            collect_terminal_stripe_locked(stripe, deferred_destruction);
        }
    }

// 把 G 放进**紧急队列**，专供 cannot-fail 的收尾路径（取消、shutdown、
// 信号处理）。不走普通 enqueue 的原因：那条路要做队列分配、可能抛
// bad_alloc、还要抢 P 队列锁；而在收尾路径里失败就意味着 G 永久悬挂。
// 这里只改三个裸指针，O(1)、不分配、不抛异常。
    void emergency_enqueue_locked(const std::shared_ptr<Task>& task) noexcept {
        if (!task) {
            return;
        }
        task->m_emergency_next.reset();
        if (emergency_tail) {
            emergency_tail->m_emergency_next = task;
        } else {
            emergency_head = task;
        }
        emergency_tail = task;
    }

// 从紧急队列头部摘一个 G，并在摘空时把 tail 一起清掉（否则 tail
// 会悬空指向已弹出的节点）。与 emergency_enqueue_locked 配对，
// 两者都假定调用方已持有 m_impl->mutex。
    std::shared_ptr<Task> emergency_pop_locked() noexcept {
        if (!emergency_head) {
            return {};
        }
        auto task = std::move(emergency_head);
        emergency_head = std::move(task->m_emergency_next);
        if (!emergency_head) {
            emergency_tail.reset();
        }
        return task;
    }

    void begin_shutdown_locked(
        std::vector<std::shared_ptr<Task>>& deferred_destruction,
        std::vector<std::shared_ptr<Task>>& completion_notifications) {
        // Unstarted Gs have no suspended stack and can be cancelled in place.
        // Started Gs must remain runnable so their Fiber can return through its
        // trampoline and execute stack-local destructors.
        for (auto& processor : processors) {
            std::lock_guard<std::mutex> queue_lock(processor.mutex);
            std::deque<std::shared_ptr<Task>> resumable;
            while (!processor.queue.empty()) {
                auto task = std::move(processor.queue.front());
                processor.queue.pop_front();
                if (task->started()) {
                    task->request_cancel(false);
                    resumable.emplace_back(std::move(task));
                } else {
                    task->clear_queued();
                    task->request_cancel(false);
                    completion_notifications.emplace_back(task);
                    runnable.fetch_sub(1, std::memory_order_relaxed);
                    deferred_destruction.emplace_back(std::move(task));
                }
            }
            processor.queue.swap(resumable);
        }

        std::deque<std::shared_ptr<Task>> resumable_global;
        while (!global_queue.empty()) {
            auto task = std::move(global_queue.front());
            global_queue.pop_front();
            if (task->started()) {
                task->request_cancel(false);
                resumable_global.emplace_back(std::move(task));
            } else {
                task->clear_queued();
                task->request_cancel(false);
                completion_notifications.emplace_back(task);
                runnable.fetch_sub(1, std::memory_order_relaxed);
                deferred_destruction.emplace_back(std::move(task));
            }
        }
        global_queue.swap(resumable_global);

        // external producers do not hold scheduler mutex. Each incoming
        // stripe is drained independently so shutdown never needs one global
        // producer lock while a burst is being accepted.
        for (std::size_t stripe = 0; stripe < kIncomingStripeCount;
             ++stripe) {
            std::deque<std::shared_ptr<Task>> incoming;
            bool incoming_empty = false;
            {
                IncomingStripeLock incoming_lock(incoming_mutexes[stripe]);
                incoming.swap(incoming_queues[stripe]);
                incoming_empty = incoming_queues[stripe].empty();
            }
            if (incoming_empty) {
                incoming_nonempty_mask.fetch_and(
                    ~(std::uint32_t{1} << stripe), std::memory_order_release);
            }
            incoming_count.fetch_sub(incoming.size(),
                                     std::memory_order_relaxed);
            for (auto& task : incoming) {
                if (!task) continue;
                if (task->started()) {
                    task->request_cancel(false);
                    global_queue.emplace_back(std::move(task));
                } else {
                    task->clear_queued();
                    task->request_cancel(false);
                    completion_notifications.emplace_back(task);
                    runnable.fetch_sub(1, std::memory_order_relaxed);
                    deferred_destruction.emplace_back(std::move(task));
                }
            }
        }

        // The rescue queue contains only previously started Gs when a normal
        // queue allocation failed. Keep them runnable and request cooperative
        // cancellation; no new node allocation is required here.
        for (auto task = emergency_head; task; task = task->m_emergency_next) {
            task->request_cancel(false);
        }

        // Waiting Gs are not in a queue. Request cancellation and perform the
        // same pending-wake handoff used by I/O readiness so a park racing
        // this scan cannot be lost.
        for (std::size_t stripe = 0; stripe < kRegistryStripeCount;
             ++stripe) {
            std::lock_guard<std::mutex> registry_lock(
                registry_mutexes[stripe]);
            for (const auto& task : task_registries[stripe]) {
                if (!task) {
                    continue;
                }
                const auto state = task->state();
                if (state == GState::kDead || state == GState::kCancelled ||
                    state == GState::kFailed) {
                    continue;
                }
                task->request_cancel(false);
                if (task->terminal()) {
                    completion_notifications.emplace_back(task);
                }
                if (!task->started()) {
                    continue;
                }
                const auto action = task->wake_for_scheduler();
                if (action == WakeAction::kEnqueue && task->try_mark_queued()) {
                    runnable.fetch_add(1, std::memory_order_relaxed);
                    try {
                        global_queue.emplace_back(task);
                    } catch (...) {
                        emergency_enqueue_locked(task);
                    }
                } else if (task->state() == GState::kRunnable &&
                           !task->queued() && task->try_mark_queued()) {
                    // A notifier may have changed Waiting -> Runnable before it
                    // acquired this admission mutex. Shutdown must claim that G
                    // itself rather than leave a suspended Runnable unqueued.
                    runnable.fetch_add(1, std::memory_order_relaxed);
                    emergency_enqueue_locked(task);
                }
            }
        }
        collect_terminal_locked(deferred_destruction);
    }

    bool all_tasks_terminal_locked() const {
        for (std::size_t stripe = 0; stripe < kRegistryStripeCount;
             ++stripe) {
            std::lock_guard<std::mutex> registry_lock(
                registry_mutexes[stripe]);
            if (!std::all_of(task_registries[stripe].begin(),
                             task_registries[stripe].end(),
                             [](const auto& task) {
                                 return !task || task->terminal();
                             })) {
                return false;
            }
        }
        return true;
    }

    bool finish_draining_locked() noexcept {
        if (!draining.load(std::memory_order_acquire) ||
            !all_tasks_terminal_locked()) {
            return false;
        }
        draining.store(false, std::memory_order_release);
        stopping.store(true, std::memory_order_release);
        return true;
    }

    SchedulerConfig config;
    std::thread sysmon_thread;
    std::atomic<bool> sysmon_stop{false};
    std::atomic<bool> sysmon_active{false};
    std::atomic<bool> sysmon_retry_pending{false};
    std::atomic<std::uint64_t> sysmon_pass_count{0};
    std::mutex sysmon_wait_mutex;
    std::condition_variable sysmon_wait_condition;
    std::shared_ptr<const void> owner_token;
    std::shared_ptr<TaskCancellationGate> cancellation_gate;
    // 观察者属于可选扩展；通过独立互斥量替换，避免配置/插件管理线程
    // 与 worker 共享 Scheduler::mutex。事件回调永远在该锁释放后执行。
    mutable std::mutex observer_mutex;
    SchedulerObserverPtr observer;
    // 零开销快路径标志：绝大多数程序从不安装观察者，而 emit_event 在 worker_loop 里
    // **每个任务被调用两次**（开始 / 结束）。没有这个标志时，即使 observer 为空，
    // 每次也要取一次 steady_clock::now()（~25ns）、加一把全局 mutex、拷一次 shared_ptr，
    // 多 worker 时所有线程串行化在同一把锁上 —— 这是 3+ worker 数量级劣化的强嫌疑。
    std::atomic<bool> has_observer{false};
    mutable std::mutex mutex;
    mutable std::array<std::mutex, kRegistryStripeCount> registry_mutexes;
    std::array<std::mutex, kIncomingStripeCount> incoming_mutexes;
    std::array<std::deque<std::shared_ptr<Task>>, kIncomingStripeCount>
        incoming_queues;
    std::atomic<std::size_t> incoming_count{0};
    std::atomic<std::size_t> next_incoming_stripe{0};
    std::atomic<std::size_t> incoming_probe{0};
    std::atomic<std::uint32_t> incoming_nonempty_mask{0};
    // 用已 futex 化的 ParkingCondition 替代 std::condition_variable：
    // worker 的 park/unpark 全部汇聚到这里，原实现每次都要穿 glibc 的 cv 层。
    // 接口（wait / wait_for / wait_until / notify_*）与 cv 完全一致，
    // 因此调度器其余 27 处调用点无需改动。
    core::ParkingCondition condition;
    // M 自己轮询 IO 后端的回调；空表示沿用「poller 线程 + 条件变量唤醒」
    // 的传统路径（纯计算调度器、以及未使用 IOManager 的测试）。
    std::atomic<IoIdlePoller*> io_poller{nullptr};
    // 回调所有权单独持有：io_poller 只发布裸指针，避免 worker 热路径反复
    // 复制 std::function。
    std::unique_ptr<IoIdlePoller> io_poller_owner;
    // 当前正停在 IO 后端 epoll_wait 里的 worker 数。只有它非零时 notify
    // 才需要写 eventfd。
    std::atomic<std::size_t> io_waiting_workers{0};
    std::deque<std::shared_ptr<Task>> global_queue;
    std::shared_ptr<Task> emergency_head;
    std::shared_ptr<Task> emergency_tail;
    std::array<std::vector<std::shared_ptr<Task>>, kRegistryStripeCount>
        task_registries;
    std::deque<Processor> processors;
    // Machine records outlive their worker thread. Keeping shared slots makes
    // machine indices/references stable while dynamic growth appends workers,
    // and lets snapshots safely observe a worker that is being reaped.
    // A deque keeps published machine slots stable while dynamic M growth
    // appends records; snapshots and BlockingRegion lookup never observe a
    // vector relocation of an existing shared_ptr slot.
    std::deque<std::shared_ptr<Machine>> machines;
    std::atomic<bool> started{false};
    std::atomic<bool> draining{false};
    std::atomic<bool> stopping{false};
    std::atomic<bool> accepting{true};
    std::atomic<std::size_t> runnable{0};
    std::atomic<std::size_t> active_workers{0};
    std::atomic<std::size_t> running_workers{0};
    std::atomic<std::size_t> blocking_workers{0};
    std::atomic<std::size_t> next_processor{0};
    std::atomic<std::uint64_t> enqueue_maintenance{0};
    // 常规回收的轮转游标：每次触发只压缩一个注册表条带。
    std::atomic<std::size_t> next_terminal_collect_stripe{0};
    std::atomic<std::uint64_t> metric_task_runs{0};
    std::atomic<std::uint64_t> metric_task_completions{0};
    std::atomic<std::uint64_t> metric_fiber_resume_ns{0};
    std::atomic<std::uint64_t> metric_local_queue_pops{0};
    std::atomic<std::uint64_t> metric_steal_pops{0};
    std::mutex join_mutex;
};

struct Scheduler::Lifetime final {
    explicit Lifetime(Scheduler* value) : owner(value) {}
    mutable std::mutex mutex;
    Scheduler* owner{nullptr};
};

class Scheduler::TimerExecutorAdapter final : public core::TimerExecutor {
public:
    explicit TimerExecutorAdapter(const std::shared_ptr<Lifetime>& lifetime)
        : m_lifetime(lifetime) {}

    bool Submit(std::function<void()> callback) override {
        const auto lifetime = m_lifetime.lock();
        if (!lifetime) {
            return false;
        }
        std::lock_guard<std::mutex> lock(lifetime->mutex);
        if (!lifetime->owner || !lifetime->owner->is_running()) {
            return false;
        }
        auto task = lifetime->owner->spawn(std::move(callback));
        if (task) {
            GO2CPP_DEBUG_EVENT(kTimerEnqueue, task->id(), 0);
        }
        return static_cast<bool>(task);
    }

private:
    std::weak_ptr<Lifetime> m_lifetime;
};

Scheduler::Scheduler(SchedulerConfig config)
    : m_lifetime(std::make_shared<Lifetime>(this)),
      m_impl(std::make_unique<Impl>(config, this)) {}

Scheduler::Scheduler(std::size_t processor_count)
    : Scheduler([processor_count] {
          SchedulerConfig config;
          config.processor_count = processor_count;
          return config;
      }()) {}

Scheduler::~Scheduler() {
    if (m_lifetime) {
        std::lock_guard<std::mutex> lock(m_lifetime->mutex);
        m_lifetime->owner = nullptr;
    }
    // The worker loop stores this object as its scheduler identity. Destroying
    // the owner from one of its own Gs would otherwise free m_impl while that
    // loop is still using it. There is no safe implicit "delete later" point
    // for a stack-allocated Scheduler, so fail fast instead of permitting a
    // use-after-free. The documented ownership contract is that the Scheduler
    // (and IOManager) outlives all worker threads.
    if (current_scheduler() == this) {
        std::terminate();
    }
    shutdown();
    if (m_impl && m_impl->cancellation_gate) {
        std::lock_guard<std::mutex> lock(m_impl->cancellation_gate->m_mutex);
        m_impl->cancellation_gate->m_scheduler = nullptr;
    }
}

void Scheduler::start() {
    std::unique_lock<std::mutex> lock(m_impl->mutex);
    if (m_impl->started.load(std::memory_order_acquire) ||
        m_impl->stopping.load(std::memory_order_acquire)) {
        return;
    }

    m_impl->started.store(true, std::memory_order_release);
    m_impl->accepting.store(true, std::memory_order_release);
    m_impl->sysmon_stop.store(false, std::memory_order_release);
    try {
        for (std::size_t i = 0; i < m_impl->config.min_workers; ++i) {
            (void)m_impl->spawn_worker_locked(this, false);
        }
        if (m_impl->config.enable_sysmon) {
            m_impl->sysmon_active.store(true, std::memory_order_release);
            m_impl->sysmon_thread = std::thread([impl = m_impl.get(), this] {
                impl->sysmon_loop(this);
            });
        }
    } catch (...) {
        m_impl->sysmon_active.store(false, std::memory_order_release);
        m_impl->accepting.store(false, std::memory_order_release);
        lock.unlock();
        // Drain and join the workers that were successfully created before
        // propagating the thread-creation failure. Leaving a partially
        // started scheduler in draining state would strand accepted Gs.
        shutdown();
        throw;
    }
    lock.unlock();
    m_impl->notify_all_workers();
    m_impl->emit_event(SchedulerEvent{SchedulerEventType::kStarted});
}

/**
 * @brief 合作式关停：关闭 admission、取消未启动 G、给运行中 G 设取消、等全部终态。
 *
 * ## 步骤
 *
 * step1. 关闭 admission（新的 `go()` 被拒绝）并置 draining；
 * step2. 取消所有**尚未启动**的 G（它们不会再被调度，直接进终态）；
 * step3. 对已启动的 G 设置**合作取消** —— 只置标志，由 G 自己在下一个 park 点检查；
 * step4. 唤醒所有 parked G，让它们看到取消标志并走到终态；
 * step5. 等待注册表里全部 G 进入终态；有界等待，超时返回 false。
 *
 * ## 为什么不能强制
 *
 * 协作式模型下**没有安全点可以切断 C++ 栈**；强行终结会跳过 RAII（锁不释放、
 * 临时对象不析构）。所以只能「请求取消 + 等自然完成」——永不返回的 body 会让
 * shutdown 一直等到超时，这是有意的取舍。
 *
 * @return true 表示全部进入终态；false 表示超时（仍有 G 未结束，调用方负责 watchdog）。
 */
bool Scheduler::shutdown_for(
    std::chrono::steady_clock::duration timeout) {
    if (!m_impl) {
        return true;
    }

    const auto clock_now = std::chrono::steady_clock::now();
    const bool bounded =
        timeout != std::chrono::steady_clock::duration::max();
    const auto nonnegative_timeout =
        timeout < std::chrono::steady_clock::duration::zero()
            ? std::chrono::steady_clock::duration::zero()
            : timeout;
    const auto max_time = std::chrono::steady_clock::time_point::max();
    const auto deadline =
        bounded && nonnegative_timeout < max_time - clock_now
            ? clock_now + nonnegative_timeout
            : max_time;

    // A worker may initiate shutdown, but it must never join another M: two
    // workers doing so concurrently would each wait for the other to return
    // from shutdown. Worker callers only publish the stop request; a later
    // external caller (normally the owner/destructor) performs all joins.
    const bool called_from_worker = current_scheduler() == this;
    std::unique_lock<std::mutex> join_lock(m_impl->join_mutex,
                                           std::defer_lock);
    if (!called_from_worker) {
        join_lock.lock();
    }

    // Keep cancelled queue-owned tasks alive until all scheduler locks have
    // been released. A user capture's destructor is arbitrary C++ code and
    // must not run while it can re-enter this Scheduler.
    std::vector<std::shared_ptr<Task>> deferred_destruction;
    std::vector<std::shared_ptr<Task>> completion_notifications;
    bool was_started = false;

    {
        std::unique_lock<std::mutex> lock(m_impl->mutex);
        was_started = m_impl->started.load(std::memory_order_acquire);
        if (!m_impl->draining.load(std::memory_order_acquire) &&
            !m_impl->stopping.load(std::memory_order_acquire)) {
            m_impl->accepting.store(false, std::memory_order_release);
            m_impl->draining.store(true, std::memory_order_release);
            m_impl->begin_shutdown_locked(deferred_destruction,
                                           completion_notifications);
        }
    }

    if (was_started) {
        m_impl->emit_event(SchedulerEvent{SchedulerEventType::kStopping});
    }

    // sysmon 在 draining 阶段仍保持运行，直到所有已启动 G 完成自然
    // 取消/唤醒。这样长系统调用返回前，监控不会提前丢失替代 M。
    for (const auto& task : completion_notifications) {
        if (task) {
            task->notify_terminal();
        }
    }

    if (!was_started) {
        {
            std::lock_guard<std::mutex> lock(m_impl->mutex);
            m_impl->stopping.store(true, std::memory_order_release);
            m_impl->draining.store(false, std::memory_order_release);
            m_impl->collect_terminal_locked(deferred_destruction);
            for (auto& processor : m_impl->processors) {
                processor.state.store(PState::kDead,
                                      std::memory_order_release);
            }
        }
        m_impl->stop_sysmon(true);
        if (join_lock.owns_lock()) {
            join_lock.unlock();
        }
        for (auto& task : deferred_destruction) {
            if (task) {
                auto abandoned_function = task->release_callable();
            }
        }
        deferred_destruction.clear();
        completion_notifications.clear();
        return true;
    }
    m_impl->notify_all_workers();

    if (called_from_worker) {
        for (auto& task : deferred_destruction) {
            if (task) {
                auto abandoned_function = task->release_callable();
            }
        }
        deferred_destruction.clear();
        return false;
    }

    // Keep workers alive while every started Fiber observes cancellation and
    // returns through its trampoline. This can wait indefinitely for a G that
    // never reaches a cooperative boundary, just as joining an uncooperative
    // std::thread can; discarding its suspended stack would skip RAII.
    {
        std::unique_lock<std::mutex> lock(m_impl->mutex);
        const auto terminal = [this] {
            return m_impl->all_tasks_terminal_locked();
        };
        const bool completed =
            bounded ? m_impl->condition.wait_until(lock, deadline, terminal)
                    : (m_impl->condition.wait(lock, terminal), true);
        m_impl->collect_terminal_locked(deferred_destruction);
        if (!completed) {
            // 保持 draining 状态和已发布的取消请求；Fiber 栈仍由 worker
            // 拥有，不能因为超时而强制释放。后续 shutdown_for()/shutdown()
            // 会从这里继续收尾。
            lock.unlock();
            for (auto& task : deferred_destruction) {
                if (task) {
                    auto abandoned_function = task->release_callable();
                }
            }
            deferred_destruction.clear();
            completion_notifications.clear();
            return false;
        }
        m_impl->stopping.store(true, std::memory_order_release);
        m_impl->draining.store(false, std::memory_order_release);
    }
    m_impl->stop_sysmon(true);
    m_impl->notify_all_workers();

    // 先等待 worker 主动退出；这样有界调用不会在 timeout 后把仍运行的
    // std::thread 留给析构路径。若本轮超时，stopping 保持为 true，后续
    // 调用可以继续等待并完成 join。
    if (bounded) {
        std::unique_lock<std::mutex> lock(m_impl->mutex);
        const bool workers_stopped = m_impl->condition.wait_until(
            lock, deadline, [this] {
                return m_impl->active_workers.load(std::memory_order_acquire) ==
                       0;
            });
        if (!workers_stopped) {
            lock.unlock();
            for (auto& task : deferred_destruction) {
                if (task) {
                    auto abandoned_function = task->release_callable();
                }
            }
            deferred_destruction.clear();
            completion_notifications.clear();
            return false;
        }
    }

    // The external caller owns join_mutex, which prevents the worker-side
    // reaper from moving or erasing thread objects while they are joined.
    // Extract one std::thread at a time while holding Impl::mutex, then join
    // it after releasing that mutex because worker epilogues take the mutex
    // before exit. Moving a std::thread is noexcept, so shutdown does not
    // need a temporary vector allocation that could fail during OOM cleanup.
    for (;;) {
        std::thread worker;
        {
            std::lock_guard<std::mutex> lock(m_impl->mutex);
            for (const auto& machine : m_impl->machines) {
                if (machine && machine->thread.joinable()) {
                    worker = std::move(machine->thread);
                    break;
                }
            }
        }
        if (!worker.joinable()) {
            break;
        }
        // Worker-initiated shutdown returned above, so this path is always an
        // external owner. Join every remaining thread; comparing reusable
        // std::thread::id values here could otherwise leave a joinable record.
        worker.join();
    }

    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        for (auto& processor : m_impl->processors) {
            if (processor.active_machines.load(std::memory_order_acquire) ==
                0) {
                processor.state.store(PState::kDead,
                                      std::memory_order_release);
            }
        }

        bool any_joinable = false;
        for (const auto& machine : m_impl->machines) {
            any_joinable = any_joinable ||
                           (machine && machine->thread.joinable());
        }
        if (!any_joinable) {
            m_impl->started.store(false, std::memory_order_release);
        }
    }
    if (join_lock.owns_lock()) {
        join_lock.unlock();
    }
    for (auto& task : deferred_destruction) {
        if (task) {
            auto abandoned_function = task->release_callable();
        }
    }
    deferred_destruction.clear();
    completion_notifications.clear();
    return true;
}

void Scheduler::shutdown() {
    (void)shutdown_for(std::chrono::steady_clock::duration::max());
}

bool Scheduler::is_running() const noexcept {
    return m_impl && m_impl->started.load(std::memory_order_acquire) &&
           !m_impl->draining.load(std::memory_order_acquire) &&
           !m_impl->stopping.load(std::memory_order_acquire);
}


core::Timer Scheduler::schedule_at(
    std::chrono::steady_clock::time_point deadline,
    Task::Function function) {
    if (!function || !m_impl) {
        return {};
    }
    if (!is_running()) {
        start();
    }
    auto executor = std::make_shared<TimerExecutorAdapter>(m_lifetime);
    return core::TimerService::Default().ScheduleOn(
        deadline, std::move(function), std::move(executor));
}

core::Timer Scheduler::schedule_after(
    std::chrono::steady_clock::duration delay,
    Task::Function function) {
    return schedule_at(std::chrono::steady_clock::now() +
                           (delay < std::chrono::steady_clock::duration::zero()
                                ? std::chrono::steady_clock::duration::zero()
                                : delay),
                       std::move(function));
}

core::Timer Scheduler::schedule_every(
    std::chrono::steady_clock::duration period,
    Task::Function function,
    core::TimerOptions options) {
    if (!function) {
        return {};
    }
    if (!is_running()) {
        start();
    }
    auto executor = std::make_shared<TimerExecutorAdapter>(m_lifetime);
    return core::TimerService::Default().ScheduleEveryOn(
        period, std::move(function), std::move(executor), options);
}


/// 安装运行期调度器观察者；只替换插件指针，不改变任何 G/M/P 状态。
/// @param observer 新观察者，空指针表示关闭事件通知。
/// @return 无；观察者快照由 Impl 的独立锁保护。
void Scheduler::set_observer(SchedulerObserverPtr observer) noexcept {
    if (!m_impl) {
        return;
    }
    try {
        SchedulerObserverPtr replaced;
        std::lock_guard<std::mutex> lock(m_impl->observer_mutex);
        replaced = std::move(m_impl->observer);
        m_impl->observer = std::move(observer);
        // 先发布对象、再置标志，保证读到 true 的线程一定能看到非空观察者。
        m_impl->has_observer.store(m_impl->observer != nullptr,
                                   std::memory_order_release);
    } catch (...) {
        // shared_ptr 交换不会主动分配；此处保持 noexcept 契约，异常时
        // 保留原观察者，避免插件安装影响调度器运行。
    }
}

/// 读取当前观察者快照；调用方可在返回后安全持有该 shared_ptr。
/// @return 当前观察者；未安装时返回空指针。
SchedulerObserverPtr Scheduler::observer() const noexcept {
    if (!m_impl) {
        return {};
    }
    try {
        std::lock_guard<std::mutex> lock(m_impl->observer_mutex);
        return m_impl->observer;
    } catch (...) {
        return {};
    }
}

bool Scheduler::set_io_idle_poller(std::function<int(int)> poll,
                                  std::function<void()> wake) {
    if (!m_impl || !poll) {
        return false;
    }
    if (m_impl->io_poller.load(std::memory_order_acquire) != nullptr ||
        m_impl->started.load(std::memory_order_acquire)) {
        // worker 已经在跑；运行期替换回调和 std::function 的并发读写无法
        // 用一次发布保证，因此只允许 start() 之前安装一次。
        return false;
    }
    std::unique_ptr<Impl::IoIdlePoller> owner;
    try {
        owner = std::make_unique<Impl::IoIdlePoller>();
        owner->poll = std::move(poll);
        owner->wake = std::move(wake);
    } catch (...) {
        return false;
    }
    m_impl->io_poller_owner = std::move(owner);
    m_impl->io_poller.store(m_impl->io_poller_owner.get(),
                            std::memory_order_release);
    return true;
}

bool Scheduler::has_io_idle_poller() const noexcept {
    return m_impl && m_impl->io_poller.load(std::memory_order_acquire) != nullptr;
}

std::shared_ptr<Task> Scheduler::spawn(Task::Function function) {
    return spawn(std::move(function), TaskOptions{});
}

/**
 * @brief 提交一个可调用对象，返回 `Task` 句柄（GMP 里的「创建 G」）。
 *
 * ## 步骤
 *
 * step1. 用配置补全 `stack_size` / `fiber_bin_capacity`（0 表示取 Scheduler 默认值）；
 * step2. 构造 `Task`（同时持有 callable，不在此启动 Fiber —— 首次被 worker 取到才建栈）；
 * step3. `enqueue()` 入队；失败（admission 已关闭/admission 竞态）时**立即 `cancel()`**，
 *        让调用方通过 `task->state()` 看到终态，而不是拿到一个永不运行的句柄。
 *
 * ## 线程模型
 *
 * 可以从任意线程调用；`enqueue` 走外部 admission 路径（全局锁 + registry 条带），
 * 与 worker 内部的 `requeue_from_worker` 是两条不同的路径。
 */
std::shared_ptr<Task> Scheduler::spawn(Task::Function function,
                                      TaskOptions options) {
    if (options.stack_size == 0) {
        options.stack_size = m_impl->config.fiber_stack_size;
    }
    if (options.stack_reserve_size == 0) {
        options.stack_reserve_size = m_impl->config.fiber_stack_reserve_size;
    }
    // 配置是总开关：即使调用方没有显式关闭，配置关闭时也必须退化。
    if (!m_impl->config.fiber_stack_growable) {
        options.stack_growable = false;
    }
    if (options.fiber_bin_capacity == 0) {
        options.fiber_bin_capacity = m_impl->config.fiber_bin_capacity;
    }
    auto task = std::make_shared<Task>(std::move(function), options);
    GO2CPP_DEBUG_EVENT(kGCreated, task ? task->id() : 0, 0);
    if (!enqueue(task)) {
        task->cancel();
    }
    return task;
}

bool Scheduler::enqueue(const std::shared_ptr<Task>& task) {
    GO2CPP_DEBUG_EVENT(kEnqueue, task ? task->id() : 0, 0);
    if (!task || !m_impl) {
        GO2CPP_DEBUG_EVENT(kEnqueueRejected, task ? task->id() : 0, 1);
        return false;
    }
    // 锁序守卫：enqueue() 会获取 admission 锁；若调用方仍持有 incoming
    // 条带锁，就与 shutdown 的 admission→incoming 顺序构成反向嵌套
    // （SCHED-02）。正常路径只读一次线程局部变量。
    if (t_incoming_lock_depth != 0) {
        auto logger = GO2CPP_LOG_NAME("Scheduler");
        GO2CPP_LOG_ERROR(logger)
            << "lock-order violation: enqueue() called while holding an "
               "incoming stripe lock";
        std::terminate();
    }
    const auto initial_state = task->state();
    if (initial_state == GState::kDead || initial_state == GState::kCancelled ||
        initial_state == GState::kFailed) {
        GO2CPP_DEBUG_EVENT(kEnqueueRejected, task->id(), 2);
        return false;
    }

    const bool from_worker =
        current_scheduler() == this && t_processor_id < m_impl->processors.size();
    const bool drain_resume =
        m_impl->draining.load(std::memory_order_acquire) &&
        task->started() && task->cancellation_requested();
    if ((!m_impl->accepting.load(std::memory_order_acquire) && !drain_resume) ||
        m_impl->stopping.load(std::memory_order_acquire)) {
        bool notify = false;
        if (task->owned_by(m_impl->owner_token)) {
            task->request_cancel(false);
            notify = task->terminal();
        }
        if (notify) {
            task->notify_terminal();
        }
        GO2CPP_DEBUG_EVENT(kEnqueueRejected, task->id(), 3);
        return false;
    }

    const auto admission_state = task->state();
    if (!drain_resume && admission_state != GState::kNew &&
        admission_state != GState::kRunnable) {
        GO2CPP_DEBUG_EVENT(kEnqueueRejected, task->id(), 4);
        return false;
    }
    if (!task->prepare_enqueue(m_impl->owner_token,
                               m_impl->cancellation_gate)) {
        GO2CPP_DEBUG_EVENT(kEnqueueRejected, task->id(), 5);
        return false;
    }

    bool registered_here = false;
    bool registry_admission_failed = false;
    // IO/定时器唤醒的 G 通常已经在注册表中。已注册任务只需重新发布
    // 队列节点，不再为每次 wake 争用对应条带的 registry mutex；首次
    // 入队仍在锁内完成 try_register + vector 发布的原子边界。
    if (!task->registered()) {
        const auto registry_stripe = Scheduler::Impl::registry_stripe(*task);
        std::lock_guard<std::mutex> registry_lock(
            m_impl->registry_mutexes[registry_stripe]);
        if (!task->registered()) {
            registered_here = task->try_register();
            if (registered_here) {
                try {
                    m_impl->task_registries[registry_stripe].emplace_back(task);
                } catch (...) {
                    task->m_registered.store(false, std::memory_order_release);
                    registered_here = false;
                    registry_admission_failed = true;
                }
            }
        }
    }
    // 注册表是挂起 G 的最后一个强所有权。首次注册分配失败时绝不能
    // 继续发布队列节点，否则任务第一次 park 后可能只剩调用方的裸句柄，
    // Fiber 栈会在无人持有时被销毁。将这次入队报告为失败，让 spawn()
    // 按普通取消路径处理。
    if (registry_admission_failed) {
        task->clear_queued();
        task->request_cancel(false);
        task->notify_terminal();
        GO2CPP_DEBUG_EVENT(kEnqueueRejected, task->id(), 6);
        return false;
    }

    if (task->state() != GState::kRunnable || !task->queued()) {
        // B2：prepare_enqueue 在本函数内已把 m_queued 置 true。若随后任务
        // 被取消 / 被别的 worker 抢走，这里直接 return 会留下一个没有队列
        // 节点的 reservation；之后 try_mark_queued 永远失败，G 再也无法
        // 入队（唤醒丢失）。清掉它：此时任务已不在 kRunnable（或 queued
        // 本就是 false），不会误清另一个有效节点。
        task->clear_queued();
        if (registered_here) {
            std::shared_ptr<Task> abandoned;
            {
                const auto registry_stripe =
                    Scheduler::Impl::registry_stripe(*task);
                std::lock_guard<std::mutex> registry_lock(
                    m_impl->registry_mutexes[registry_stripe]);
                auto& task_registry =
                    m_impl->task_registries[registry_stripe];
                auto found = std::find(task_registry.begin(),
                                       task_registry.end(), task);
                if (found != task_registry.end()) {
                    if (task->terminal()) {
                        abandoned = std::move(*found);
                    }
                    task_registry.erase(found);
                }
            }
            task->m_registered.store(false, std::memory_order_release);
            if (abandoned) {
                auto abandoned_function = abandoned->release_callable();
            }
        }
        return false;
    }

    // Worker-issued shutdown resumes must use the admission-locked queues so
    // shutdown's draining scan and the resume are ordered. Normal producers
    // use the short incoming queue and never contend with worker dequeue.
    if (!from_worker && !drain_resume) {
        bool published = false;
        // 同一生产者固定到同一条带，保留单生产者提交的 FIFO；第一次
        // 进入时才从全局分配条带，后续任务不再为选择条带做原子递增。
        static thread_local std::size_t t_incoming_stripe =
            std::numeric_limits<std::size_t>::max();
        if (t_incoming_stripe == std::numeric_limits<std::size_t>::max()) {
            t_incoming_stripe =
                m_impl->next_incoming_stripe.fetch_add(
                    1, std::memory_order_relaxed) &
                (Scheduler::Impl::kIncomingStripeCount - 1);
        }
        const auto incoming_stripe = t_incoming_stripe;
        {
            IncomingStripeLock incoming_lock(
                m_impl->incoming_mutexes[incoming_stripe]);
            if (m_impl->accepting.load(std::memory_order_acquire) &&
                !m_impl->stopping.load(std::memory_order_acquire)) {
                m_impl->runnable.fetch_add(1, std::memory_order_relaxed);
                try {
                    m_impl->incoming_queues[incoming_stripe].emplace_back(task);
                    GO2CPP_DEBUG_EVENT(kRunQueuePush, task->id(),
                                       ::go2cpp::debug::kGlobalQueueTag);
                    m_impl->incoming_count.fetch_add(1,
                                                     std::memory_order_release);
                    m_impl->incoming_nonempty_mask.fetch_or(
                        std::uint32_t{1} << incoming_stripe,
                        std::memory_order_release);
                    published = true;
                } catch (...) {
                    m_impl->runnable.fetch_sub(1, std::memory_order_relaxed);
                }
            }
        }
        if (!published) {
            if (task->started()) {
                // incoming_queue 的计数在 emplace 失败时已经回滚；救援链
                // 仍需要重新计入 runnable，worker 取出它时才会配对递减。
                std::lock_guard<std::mutex> admission_lock(m_impl->mutex);
                m_impl->runnable.fetch_add(1, std::memory_order_relaxed);
                m_impl->emergency_enqueue_locked(task);
                m_impl->notify_worker();
                return true;
            }
            task->clear_queued();
            task->request_cancel(false);
            if (registered_here) {
                const auto registry_stripe =
                    Scheduler::Impl::registry_stripe(*task);
                std::lock_guard<std::mutex> registry_lock(
                    m_impl->registry_mutexes[registry_stripe]);
                auto& task_registry =
                    m_impl->task_registries[registry_stripe];
                auto found = std::find(task_registry.begin(),
                                       task_registry.end(), task);
                if (found != task_registry.end()) {
                    task_registry.erase(found);
                }
                task->m_registered.store(false, std::memory_order_release);
            }
            task->notify_terminal();
            return false;
        }
    } else {
        std::unique_lock<std::mutex> admission_lock(m_impl->mutex);
        if ((!m_impl->accepting.load(std::memory_order_acquire) && !drain_resume) ||
            m_impl->stopping.load(std::memory_order_acquire)) {
            task->clear_queued();
            task->request_cancel(false);
            admission_lock.unlock();
            task->notify_terminal();
            return false;
        }
        const auto preferred =
            from_worker ? static_cast<std::size_t>(t_processor_id)
                        : m_impl->next_processor.fetch_add(
                              1, std::memory_order_relaxed);
        auto& processor =
            m_impl->processors[preferred % m_impl->processors.size()];
        std::lock_guard<std::mutex> queue_lock(processor.mutex);
        const bool use_global =
            processor.queue.size() >= m_impl->config.local_queue_limit;
        m_impl->runnable.fetch_add(1, std::memory_order_relaxed);
        try {
            if (!use_global) {
                processor.queue.push_back(task);
                GO2CPP_DEBUG_EVENT(kRunQueuePush, task->id(), processor.id);
            } else {
                m_impl->global_queue.push_back(task);
                GO2CPP_DEBUG_EVENT(kRunQueuePush, task->id(),
                                   ::go2cpp::debug::kGlobalQueueTag);
            }
        } catch (...) {
            if (task->started()) {
                // 已启动 Fiber 不能在分配失败时丢失。救援队列复用
                // Task 内嵌链，不需要再次分配节点，仍保留 runnable/queued
                // 计数，后续 worker 会从这里恢复并完成取消收尾。
                m_impl->emergency_enqueue_locked(task);
            } else {
                m_impl->runnable.fetch_sub(1, std::memory_order_relaxed);
                task->clear_queued();
                task->request_cancel(false);
                admission_lock.unlock();
                if (registered_here) {
                    const auto registry_stripe =
                        Scheduler::Impl::registry_stripe(*task);
                    std::lock_guard<std::mutex> registry_lock(
                        m_impl->registry_mutexes[registry_stripe]);
                    auto& task_registry =
                        m_impl->task_registries[registry_stripe];
                    const auto found = std::find(task_registry.begin(),
                                                 task_registry.end(), task);
                    if (found != task_registry.end()) {
                        task_registry.erase(found);
                    }
                    task->m_registered.store(false, std::memory_order_release);
                }
                task->notify_terminal();
                return false;
            }
        }
    }

    const auto maintenance_ticket =
        m_impl->enqueue_maintenance.fetch_add(1, std::memory_order_relaxed);
    const bool initial_workers_missing =
        m_impl->active_workers.load(std::memory_order_relaxed) <
        m_impl->config.min_workers;
    const bool replacement_may_be_needed =
        m_impl->active_workers.load(std::memory_order_relaxed) <=
            m_impl->config.min_workers &&
        m_impl->runnable.load(std::memory_order_relaxed) != 0;
    const bool processor_workers_missing =
        m_impl->active_workers.load(std::memory_order_relaxed) <
        m_impl->processors.size();
    if (initial_workers_missing || replacement_may_be_needed ||
        (processor_workers_missing && (maintenance_ticket & 63U) == 0U) ||
        (!processor_workers_missing && (maintenance_ticket & 1023U) == 0U)) {
        m_impl->maybe_grow(this);
    }
    m_impl->notify_worker();
    return true;
}

bool Scheduler::requeue_to_processor(const std::shared_ptr<Task>& task,
                                     PId processor_id) {
    GO2CPP_DEBUG_EVENT(kRequeueProcessor, task ? task->id() : 0, processor_id);
    if (!task || processor_id >= m_impl->processors.size()) {
        return false;
    }
    if (!m_impl->accepting.load(std::memory_order_acquire) ||
        m_impl->draining.load(std::memory_order_acquire) ||
        m_impl->stopping.load(std::memory_order_acquire)) {
        return false;
    }
    if (!task->try_mark_queued()) {
        return task->queued();
    }

    auto& processor = m_impl->processors[processor_id];
    bool retry_regular_enqueue = false;
    bool notify_worker = false;
    {
        std::lock_guard<std::mutex> queue_lock(processor.mutex);
        if (!m_impl->accepting.load(std::memory_order_acquire) ||
            m_impl->draining.load(std::memory_order_acquire) ||
            m_impl->stopping.load(std::memory_order_acquire)) {
            retry_regular_enqueue = true;
        } else {
            m_impl->runnable.fetch_add(1, std::memory_order_relaxed);
            try {
                processor.queue.push_back(task);
                GO2CPP_DEBUG_EVENT(kRunQueuePush, task->id(), processor_id);
                // 该 P 已有运行中的 M 时，它会在当前 G 返回后消费本地
                // 队列；此时唤醒额外 M 只会制造 futex 抖动。
                const auto running = processor.running_machines.load(
                    std::memory_order_relaxed);
                const auto active = processor.active_machines.load(
                    std::memory_order_relaxed);
                notify_worker = running == 0 ||
                                (processor.queue.size() >= 16U &&
                                 running < active);
            } catch (...) {
                m_impl->runnable.fetch_sub(1, std::memory_order_relaxed);
                task->clear_queued();
                retry_regular_enqueue = true;
            }
        }
    }
    if (retry_regular_enqueue) {
        task->clear_queued();
        return false;
    }
    if (notify_worker) {
        m_impl->notify_worker();
    }
    return true;
}

bool Scheduler::requeue_from_worker(const std::shared_ptr<Task>& task,
                                    bool allow_notify) {
    GO2CPP_DEBUG_EVENT(kRequeueWorker, task ? task->id() : 0, 1);
    if (!task || current_scheduler() != this ||
        t_processor_id >= m_impl->processors.size()) {
        return enqueue(task);
    }

    // Worker 自己把刚刚 yield 的 G 放回原 P 时，不需要重新做 owner、
    // registry 和全局 admission。只使用 P 队列锁，避免每次 Fiber 切换
    // 都争用调度器总锁。shutdown 仍通过 draining/stopping 的二次检查与
    // P 队列锁建立一致的排空顺序。
    if (!m_impl->accepting.load(std::memory_order_acquire) ||
        m_impl->draining.load(std::memory_order_acquire) ||
        m_impl->stopping.load(std::memory_order_acquire)) {
        return enqueue(task);
    }

    auto& processor = m_impl->processors[t_processor_id];
    bool retry_regular_enqueue = false;
    bool notify_worker = false;
    {
        std::lock_guard<std::mutex> queue_lock(processor.mutex);
        if (!m_impl->accepting.load(std::memory_order_acquire) ||
            m_impl->draining.load(std::memory_order_acquire) ||
            m_impl->stopping.load(std::memory_order_acquire)) {
            retry_regular_enqueue = true;
        } else if (task->try_mark_queued_from_worker()) {
            // 计数必须先于节点发布，否则另一个 worker 可能先 pop 节点
            // 再递减尚未增加的 runnable，造成下溢。
            m_impl->runnable.fetch_add(1, std::memory_order_relaxed);
            try {
                processor.queue.push_back(task);
                GO2CPP_DEBUG_EVENT(kRunQueuePush, task->id(),
                                   t_processor_id);
                // 当前 M 正在执行这个 G；本地回队无需再次唤醒另一个
                // M，减少每次 Fiber handoff 的条件变量切换。
                const auto running = processor.running_machines.load(
                    std::memory_order_relaxed);
                const auto active = processor.active_machines.load(
                    std::memory_order_relaxed);
                notify_worker =
                    allow_notify && !t_in_idle_poll &&
                    (running == 0 ||
                     (processor.queue.size() >= 16U && running < active));
            } catch (...) {
                m_impl->runnable.fetch_sub(1, std::memory_order_relaxed);
                task->clear_queued();
                retry_regular_enqueue = true;
            }
        } else {
            return false;
        }
    }
    if (retry_regular_enqueue) {
        return enqueue(task);
    }
    if (notify_worker) {
        m_impl->notify_worker();
    }
    return true;
}

bool Scheduler::yield(const std::shared_ptr<Task>& task) {
    if (!task || current_scheduler() != this ||
        current_task().get() != task.get() || task->cancellation_requested() ||
        !task->mark_yielded_fast()) {
        return false;
    }
    task->defer_enqueue();
    GO2CPP_DEBUG_PARK_HINT(kYield);
    GO2CPP_DEBUG_EVENT(kParkEnter, task->id(),
                       static_cast<std::uint64_t>(SuspendReason::Yield));
    if (!Fiber::SuspendForScheduler(SuspendReason::Yield)) {
        GO2CPP_DEBUG_EVENT(kParkRejected, task->id(), 4);
        // mark_yielded_fast() 已把状态改成 kRunnable，但这次 yield 并没有
        // 真正交回调度器。必须把它恢复成 kRunning，否则当前执行流会在
        // "runnable 但未入队"的状态下继续跑，下一次 park 被拒后空转。
        (void)task->resume_after_failed_park();
        return false;
    }
    GO2CPP_DEBUG_EVENT(kParkReturn, task->id(), 1);
    return !task->cancellation_requested();
}

bool Scheduler::yield_current() {
    return yield(current_task());
}

bool Scheduler::park(const std::shared_ptr<Task>& task) {
    return park_with_reason(task, SuspendReason::Park);
}

bool Scheduler::park_wait(const std::shared_ptr<Task>& task) {
    GO2CPP_DEBUG_EVENT(kParkEnter, task ? task->id() : 0,
                       static_cast<std::uint64_t>(SuspendReason::Park));
    if (!task || current_scheduler() != this ||
        current_task().get() != task.get()) {
        GO2CPP_DEBUG_EVENT(kParkRejected, task ? task->id() : 0, 1);
        return false;
    }
    if (m_impl->draining.load(std::memory_order_acquire) ||
        m_impl->stopping.load(std::memory_order_acquire) ||
        task->cancellation_requested()) {
        GO2CPP_DEBUG_EVENT(kParkRejected, task->id(), 2);
        return false;
    }

    // WaitNode 已经在线性化队列中发布，Task 的 Waiting 状态可以直接
    // 原子提交。第二次检查覆盖 shutdown 与 park 的交错；若 shutdown
    // 已开始，回滚 Waiting 状态并让调用方走取消路径，不能遗留悬挂 G。
    const ParkAction action = task->park_for_scheduler();
    if (action != ParkAction::kParked) {
        GO2CPP_DEBUG_EVENT(kParkRejected, task->id(), 3);
        return false;
    }
    if (m_impl->draining.load(std::memory_order_acquire) ||
        m_impl->stopping.load(std::memory_order_acquire) ||
        task->cancellation_requested()) {
        (void)task->wake_for_wait();
        GO2CPP_DEBUG_EVENT(kParkRejected, task->id(), 2);
        return false;
    }
    if (!Fiber::SuspendForScheduler(SuspendReason::Park)) {
        (void)task->resume_after_failed_park();
        GO2CPP_DEBUG_EVENT(kParkRejected, task->id(), 5);
        return false;
    }
    GO2CPP_DEBUG_EVENT(kParkReturn, task->id(),
                       task->cancellation_requested() ? 0 : 1);
    return !task->cancellation_requested();
}

/// 函数功能：完成 park_wait_for_cleanup 调用，供条件变量重锁阶段使用。
/// 执行流程：校验当前 G 归属，提交 Waiting 状态并挂起当前 Fiber；该路径刻意
/// 不因 G 的取消标记或 draining 状态拒绝 park，因为等待目标是释放用户 mutex。
/// @param[in] task 当前受管任务。
/// @return Fiber 恢复后返回 true；错误上下文或状态转换失败返回 false。
/// @note 调用方必须已经在线性化等待队列中注册了唤醒节点。
bool Scheduler::park_wait_for_cleanup(const std::shared_ptr<Task>& task) {
    if (!task || current_scheduler() != this ||
        current_task().get() != task.get()) {
        GO2CPP_DEBUG_EVENT(kParkRejected, task ? task->id() : 0, 1);
        return false;
    }
    GO2CPP_DEBUG_EVENT(kParkEnter, task->id(),
                       static_cast<std::uint64_t>(SuspendReason::Park));
    const ParkAction action = task->park_for_scheduler();
    if (action != ParkAction::kParked) {
        GO2CPP_DEBUG_EVENT(kParkRejected, task->id(), 3);
        return false;
    }
    if (!Fiber::SuspendForScheduler(SuspendReason::Park)) {
        (void)task->resume_after_failed_park();
        GO2CPP_DEBUG_EVENT(kParkRejected, task->id(), 5);
        return false;
    }
    GO2CPP_DEBUG_EVENT(kParkReturn, task->id(), 1);
    return true;
}

bool Scheduler::park_io(const std::shared_ptr<Task>& task) {
    GO2CPP_DEBUG_EVENT(kParkEnter, task ? task->id() : 0,
                       static_cast<std::uint64_t>(SuspendReason::Io));
    if (!task || current_scheduler() != this ||
        current_task().get() != task.get()) {
        GO2CPP_DEBUG_EVENT(kParkRejected, task ? task->id() : 0, 1);
        return false;
    }
    if (m_impl->draining.load(std::memory_order_acquire) ||
        m_impl->stopping.load(std::memory_order_acquire) ||
        task->cancellation_requested()) {
        GO2CPP_DEBUG_EVENT(kParkRejected, task->id(), 2);
        return false;
    }
    // The IO poller changes Waiting -> Runnable with wake_io(). Publish the
    // state before the second shutdown check; shutdown either observes it in
    // its registry scan or is observed here without taking the scheduler
    // admission mutex on every socket wait.
    const auto action = task->park_for_scheduler();
    if (action != ParkAction::kParked) {
        GO2CPP_DEBUG_EVENT(kParkRejected, task->id(), 3);
        return false;
    }
    if (m_impl->draining.load(std::memory_order_acquire) ||
        m_impl->stopping.load(std::memory_order_acquire) ||
        task->cancellation_requested()) {
        (void)task->wake_for_io();
        GO2CPP_DEBUG_EVENT(kParkRejected, task->id(), 2);
        return false;
    }
    if (!Fiber::SuspendForScheduler(SuspendReason::Io)) {
        (void)task->resume_after_failed_park();
        GO2CPP_DEBUG_EVENT(kParkRejected, task->id(), 5);
        return false;
    }
    GO2CPP_DEBUG_EVENT(kParkReturn, task->id(),
                       task->cancellation_requested() ? 0 : 1);
    return !task->cancellation_requested();
}

bool Scheduler::park_with_reason(const std::shared_ptr<Task>& task,
                                  SuspendReason reason) {
    GO2CPP_DEBUG_EVENT(kParkEnter, task ? task->id() : 0,
                       static_cast<std::uint64_t>(reason));
    if (!task || current_scheduler() != this ||
        current_task().get() != task.get()) {
        GO2CPP_DEBUG_EVENT(kParkRejected, task ? task->id() : 0, 1);
        return false;
    }
    // Shutdown scans waiting Gs under this same mutex. Do not enter waiting
    // after the scan has begun.
    ParkAction action = ParkAction::kRejected;
    {
        std::lock_guard<std::mutex> admission_lock(m_impl->mutex);
        if (m_impl->draining.load(std::memory_order_acquire) ||
            m_impl->stopping.load(std::memory_order_acquire) ||
            task->cancellation_requested()) {
            GO2CPP_DEBUG_EVENT(kParkRejected, task->id(), 2);
            return false;
        }
        action = task->park_for_scheduler();
    }
    if (action != ParkAction::kParked) {
        GO2CPP_DEBUG_EVENT(kParkRejected, task->id(), 3);
        return false;
    }
    if (!Fiber::SuspendForScheduler(reason)) {
        (void)task->resume_after_failed_park();
        GO2CPP_DEBUG_EVENT(kParkRejected, task->id(), 5);
        return false;
    }
    GO2CPP_DEBUG_EVENT(kParkReturn, task->id(),
                       task->cancellation_requested() ? 0 : 1);
    return !task->cancellation_requested();
}

bool Scheduler::park_current() {
    return park(current_task());
}

bool Scheduler::wake(const std::shared_ptr<Task>& task) {
    return wake_or_cancel(task);
}

bool Scheduler::wake_registered(const std::shared_ptr<Task>& task) {
    GO2CPP_DEBUG_EVENT(kWakeRequest, task ? task->id() : 0, 0);
    if (!task || !m_impl || !task->owned_by(m_impl->owner_token)) {
        return false;
    }
    const auto action = task->wake_for_wait();
    if (action == WakeAction::kPending) {
        GO2CPP_DEBUG_EVENT(kWakePending, task->id(), 0);
        return false;
    }
    if (action != WakeAction::kEnqueue) {
        GO2CPP_DEBUG_EVENT(kWakeRejected, task->id(), 2);
        return false;
    }
    if (!task->registered()) {
        // A task can leave the registry while its final wake callback is
        // racing with shutdown.  Fall back to the fully checked path rather
        // than publishing a node without a lifetime anchor.
        if (enqueue(task)) {
            GO2CPP_DEBUG_EVENT(kWakeEnqueued, task->id(),
                               m_impl->runnable.load(std::memory_order_relaxed));
            return true;
        }
        GO2CPP_DEBUG_EVENT(kWakeRejected, task->id(), 3);
        return false;
    }
    if (current_scheduler() == this &&
        t_processor_id < m_impl->processors.size() &&
        requeue_from_worker(task)) {
        GO2CPP_DEBUG_EVENT(kWakeEnqueued, task->id(),
                           m_impl->runnable.load(std::memory_order_relaxed));
        return true;
    }
    // 混合 Mutex 的 Unlock 通常来自普通线程。G 最近运行的 P 是稳定的
    // 亲和性提示，把唤醒直接放入该 P 的本地队列，绕过 incoming stripe
    // 和 scheduler admission 锁；P 失效或正在 shutdown 时仍走原回退路径。
    const PId preferred_processor = task->last_processor_id();
    if (preferred_processor < m_impl->processors.size() &&
        requeue_to_processor(task, preferred_processor)) {
        GO2CPP_DEBUG_EVENT(kWakeEnqueued, task->id(),
                           m_impl->runnable.load(std::memory_order_relaxed));
        return true;
    }
    if (!task->try_mark_queued()) {
        // Another notifier may have won the queue publication.  It is safe to
        // report success when the task is already queued; otherwise let the
        // regular admission path repair the state.
        if (task->queued()) {
            GO2CPP_DEBUG_EVENT(kWakeEnqueued, task->id(),
                               m_impl->runnable.load(std::memory_order_relaxed));
            return true;
        }
        if (enqueue(task)) {
            GO2CPP_DEBUG_EVENT(kWakeEnqueued, task->id(),
                               m_impl->runnable.load(std::memory_order_relaxed));
            return true;
        }
        GO2CPP_DEBUG_EVENT(kWakeRejected, task->id(), 3);
        return false;
    }

    static thread_local std::size_t t_incoming_stripe =
        std::numeric_limits<std::size_t>::max();
    if (t_incoming_stripe == std::numeric_limits<std::size_t>::max()) {
        t_incoming_stripe =
            m_impl->next_incoming_stripe.fetch_add(1, std::memory_order_relaxed) &
            (Scheduler::Impl::kIncomingStripeCount - 1);
    }
    const auto stripe = t_incoming_stripe;
    bool published = false;
    {
        IncomingStripeLock lock(m_impl->incoming_mutexes[stripe]);
        if (m_impl->accepting.load(std::memory_order_acquire) &&
            !m_impl->stopping.load(std::memory_order_acquire)) {
            m_impl->runnable.fetch_add(1, std::memory_order_relaxed);
            try {
                m_impl->incoming_queues[stripe].emplace_back(task);
                GO2CPP_DEBUG_EVENT(kRunQueuePush, task->id(),
                                   ::go2cpp::debug::kGlobalQueueTag);
                m_impl->incoming_count.fetch_add(1, std::memory_order_release);
                m_impl->incoming_nonempty_mask.fetch_or(
                    std::uint32_t{1} << stripe, std::memory_order_release);
                published = true;
            } catch (...) {
                m_impl->runnable.fetch_sub(1, std::memory_order_relaxed);
            }
        }
    }
    if (!published) {
        task->clear_queued();
        if (enqueue(task)) {
            GO2CPP_DEBUG_EVENT(kWakeEnqueued, task->id(),
                               m_impl->runnable.load(std::memory_order_relaxed));
            return true;
        }
        GO2CPP_DEBUG_EVENT(kWakeRejected, task->id(), 3);
        return false;
    }
    m_impl->notify_worker();
    GO2CPP_DEBUG_EVENT(kWakeEnqueued, task->id(),
                       m_impl->runnable.load(std::memory_order_relaxed));
    return true;
}

bool Scheduler::wake_io(const std::shared_ptr<Task>& task) {
    GO2CPP_DEBUG_EVENT(kIoWake, task ? task->id() : 0, 0);
    GO2CPP_DEBUG_WAKE_HINT(kIo);
    if (!task || !task->owned_by(m_impl->owner_token)) {
        return false;
    }
    const auto action = task->wake_for_io();
    if (action == WakeAction::kPending) {
        GO2CPP_DEBUG_EVENT(kWakePending, task->id(), 0);
        return true;
    }
    if (action != WakeAction::kEnqueue) {
        GO2CPP_DEBUG_EVENT(kWakeRejected, task->id(), 2);
        return false;
    }
    // A worker waking another G can publish directly to its current P.  The
    // task is already registered and owned by this scheduler, so routing the
    // handoff through external admission would reacquire the registry stripe
    // and incoming-queue locks on every IO completion.
    if (current_scheduler() == this &&
        t_processor_id < m_impl->processors.size()) {
        if (requeue_from_worker(task)) {
            GO2CPP_DEBUG_EVENT(kWakeEnqueued, task->id(),
                               m_impl->runnable.load(std::memory_order_relaxed));
            return true;
        }
    }
    // IO 轮询线程优先投递到任务最近运行的 P，减少跨队列调度开销。
    const PId preferred_processor = task->last_processor_id();
    if (preferred_processor < m_impl->processors.size() &&
        requeue_to_processor(task, preferred_processor)) {
        GO2CPP_DEBUG_EVENT(kWakeEnqueued, task->id(),
                           m_impl->runnable.load(std::memory_order_relaxed));
        return true;
    }
    if (enqueue(task)) {
        GO2CPP_DEBUG_EVENT(kWakeEnqueued, task->id(),
                           m_impl->runnable.load(std::memory_order_relaxed));
        return true;
    }
    GO2CPP_DEBUG_EVENT(kWakeRejected, task->id(), 3);
    return false;
}

/**
 * @brief 批量唤醒一批 IO 就绪的 G（`process_events_batch` 的事件出口）。
 *
 * ## 步骤
 *
 * step1. 对每个 task 调 `wake_for_io()`：返回 `kPending`（挂起前已收到唤醒令牌）或
 *        `kRejected`（终态/拒绝）都跳过 —— 它们不需要再入队；
 * step2. 按 `last_processor_id()` 排序，让同一 P 的 G 连续入队（减少 P 队列锁的
 *        反复获取与跨 P 唤醒）；
 * step3. 逐 P 分组入队；分组失败时走 `fallback`（全局 incoming 条带）。
 *
 * ## 为什么这里不做 `owned_by` 校验
 *
 * 调用方是 `IOManager::State` 的 `WaitNode`，它**只有在本 Scheduler 注册成功后**
 * 才会进入 wake list。公开的 `wake_io()` 仍保留完整校验；批量入口只接受内部已登记
 * 节点，从而省掉高频 IO 上每事件一次的无效锁竞争（s2/s4 的实测优化）。
 */
bool Scheduler::wake_io_batch(
    const std::vector<std::shared_ptr<Task>>& tasks) {
    GO2CPP_DEBUG_EVENT(kWakeBatch, 0, tasks.size());
    if (tasks.empty()) {
        return true;
    }
    struct Pending final {
        std::shared_ptr<Task> task;
        PId processor{0};
    };
    std::vector<Pending> pending;
    std::vector<std::shared_ptr<Task>> fallback;
    try {
        pending.reserve(tasks.size());
        fallback.reserve(tasks.size());
        // step1: 先对每个 task 尝试 wake_for_io；kPending/kRejected 都跳过。
        for (const auto& task : tasks) {
            // 调用方是 IOManager::State 的 WaitNode；该节点只有在本
            // Scheduler 注册成功后才会进入 wake list，因此这里不再为
            // 每个 readiness 事件重复获取 Task 的 owner 互斥量。公开
            // wake_io() 仍保留完整 owned_by 校验，批量入口只接受内部
            // 已登记节点，减少高频 IO 的无效锁竞争。
            if (!task) {
                continue;
            }
            const auto action = task->wake_for_io();
            if (action == WakeAction::kPending) {
                GO2CPP_DEBUG_EVENT(kWakePending, task->id(), 0);
                continue;
            }
            if (action == WakeAction::kRejected) {
                GO2CPP_DEBUG_EVENT(kWakeRejected, task->id(), 2);
                continue;
            }
            pending.push_back(Pending{task, task->last_processor_id()});
        }
        // step2: 按 last_processor_id 排序，让同一 P 的 G 连续入队。
        std::sort(pending.begin(), pending.end(),
                  [](const Pending& left, const Pending& right) {
                      return left.processor < right.processor;
                  });
    } catch (...) {
        for (const auto& task : tasks) {
            if (task) {
                if (task->state() == GState::kRunnable) {
                    (void)enqueue(task);
                } else {
                    (void)wake_io(task);
                }
            }
        }
        return false;
    }

    // step3: 逐 P 分组入队；分组不可用（P 越界/正在 drain/stopping）时走 fallback。
    std::size_t index = 0;
    while (index < pending.size()) {
        const PId processor_id = pending[index].processor;
        std::size_t end = index;
        while (end < pending.size() &&
               pending[end].processor == processor_id) {
            ++end;
        }
        if (processor_id >= m_impl->processors.size() ||
            !m_impl->accepting.load(std::memory_order_acquire) ||
            m_impl->draining.load(std::memory_order_acquire) ||
            m_impl->stopping.load(std::memory_order_acquire)) {
            for (std::size_t item = index; item < end; ++item) {
                fallback.push_back(pending[item].task);
            }
            index = end;
            continue;
        }

        auto& processor = m_impl->processors[processor_id];
        bool notify_worker = false;
        std::size_t published = 0;
        {
            std::lock_guard<std::mutex> queue_lock(processor.mutex);
            if (!m_impl->accepting.load(std::memory_order_acquire) ||
                m_impl->draining.load(std::memory_order_acquire) ||
                m_impl->stopping.load(std::memory_order_acquire)) {
                for (std::size_t item = index; item < end; ++item) {
                    fallback.push_back(pending[item].task);
                }
            } else {
                for (std::size_t item = index; item < end; ++item) {
                    const auto& task = pending[item].task;
                    // wake_for_io 已经完成 Waiting -> Runnable；此处没有
                    // execution claim，直接复用原子入队门，避免批量 IO
                    // 每个事件再获取 Task transition_mutex。
                    if (!task->try_mark_queued_from_worker()) {
                        continue;
                    }
                    try {
                        processor.queue.push_back(task);
                        GO2CPP_DEBUG_EVENT(kRunQueuePush, task->id(),
                                           processor_id);
                        GO2CPP_DEBUG_EVENT(
                            kWakeEnqueued, task->id(),
                            m_impl->runnable.load(std::memory_order_relaxed));
                        ++published;
                        m_impl->runnable.fetch_add(1,
                                                   std::memory_order_relaxed);
                    } catch (...) {
                        task->clear_queued();
                        fallback.push_back(task);
                    }
                }
                const auto running = processor.running_machines.load(
                    std::memory_order_relaxed);
                const auto active = processor.active_machines.load(
                    std::memory_order_relaxed);
                notify_worker = published != 0 &&
                                (running == 0 || running < active);
            }
        }
        if (notify_worker) {
            m_impl->notify_worker();
        }
        index = end;
    }

    for (const auto& task : fallback) {
        if (task) {
            (void)enqueue(task);
        }
    }
    return fallback.empty();
}

bool Scheduler::wake_or_cancel(const std::shared_ptr<Task>& task) {
    GO2CPP_DEBUG_EVENT(kWakeRequest, task ? task->id() : 0, 0);
    if (!task || !task->owned_by(m_impl->owner_token)) {
        return false;
    }
    const auto action = task->wake_for_wait();
    if (action == WakeAction::kPending) {
        // The running G will consume the permit if it parks. It is not
        // immediately runnable because its C++ callable still owns the M.
        GO2CPP_DEBUG_EVENT(kWakePending, task->id(), 0);
        return false;
    }
    if (action == WakeAction::kRejected) {
        GO2CPP_DEBUG_EVENT(kWakeRejected, task->id(), 2);
        return false;
    }
    if (current_scheduler() == this &&
        t_processor_id < m_impl->processors.size()) {
        if (requeue_from_worker(task)) {
            GO2CPP_DEBUG_EVENT(kWakeEnqueued, task->id(),
                               m_impl->runnable.load(std::memory_order_relaxed));
            return true;
        }
    }
    if (!enqueue(task)) {
        // An accepted concurrent/shutdown handoff may already have queued the
        // G. Cancel only unstarted work; started Gs must be retained for drain.
        if (!task->started()) {
            (void)task->cancel_if_runnable_unqueued();
        } else if (task->state() == GState::kRunnable && !task->queued()) {
            bool notify = false;
            {
                std::lock_guard<std::mutex> lock(m_impl->mutex);
                if (task->try_mark_queued()) {
                    task->request_cancel(false);
                    m_impl->runnable.fetch_add(1, std::memory_order_relaxed);
                    m_impl->emergency_enqueue_locked(task);
                    notify = task->terminal();
                }
            }
            if (notify) {
                task->notify_terminal();
            }
        }
        GO2CPP_DEBUG_EVENT(kWakeRejected, task->id(), 3);
        m_impl->notify_all_workers();
        return false;
    }
    GO2CPP_DEBUG_EVENT(kWakeEnqueued, task->id(),
                       m_impl->runnable.load(std::memory_order_relaxed));
    return true;
}

bool Scheduler::cancel(const std::shared_ptr<Task>& task) {
    if (!task || !m_impl || !task->owned_by(m_impl->owner_token)) {
        return false;
    }
    const bool changed = task->request_cancel();
    if (!changed || !task->started()) {
        if (changed) {
            // Unstarted cancellation changes the shutdown predicate without
            // necessarily enqueueing or waking a worker.
            m_impl->notify_all_workers();
        }
        return changed;
    }

    const auto action = task->wake_for_wait();
    if (action == WakeAction::kEnqueue) {
        if (!enqueue(task)) {
            (void)task->cancel_if_runnable_unqueued();
        }
    } else if (action == WakeAction::kRejected &&
               task->state() == GState::kRunnable && !task->queued()) {
        // wake_for_wait 对 kRunnable 返回 kRejected。但 "runnable" 并不
        // 蕴含 "已入队"：deferred handoff 的中间态正是 kRunnable +
        // queued=false。取消必须补一次发布，否则 shutdown 永远等不到它。
        if (!enqueue(task)) {
            (void)task->cancel_if_runnable_unqueued();
        }
    }
    // A running G consumes the pending token at its next park. A waiting G is
    // now queued, while an already-runnable G needs no additional node.
    return true;
}

Scheduler::BlockingRegion::BlockingRegion(Scheduler* scheduler) noexcept
    : m_scheduler(scheduler != nullptr ? scheduler
                                       : Scheduler::current_scheduler()) {
    if (m_scheduler == nullptr ||
        m_scheduler != Scheduler::current_scheduler()) {
        return;
    }
    m_active = Scheduler::enter_blocking();
    if (m_active) {
        m_machine_id = Scheduler::current_machine_id();
    }
}

Scheduler::BlockingRegion::~BlockingRegion() noexcept {
    if (m_active && m_scheduler != nullptr) {
        Scheduler::leave_blocking_for(m_scheduler, m_machine_id);
        if (t_blocking_machine_id == m_machine_id) {
            t_blocking_machine_id = 0;
        }
        m_active = false;
    }
}

// 进入 BlockingRegion：把当前 M 从 kRunning 标到 kBlocking，记录起始时间
// 与所属 G，并让 blocking_workers +1。
// 这一步是 **handoff 的触发点** —— sysmon 看到某台 M 在 kBlocking 停留过久，
// 就会把它的 P 转交给别的 M，保证即使这台 M 卡在原生调用（阻塞 read/
// 第三方库）里，队列中的 G 仍有人执行。
// 只有当前 M 处于 kRunning 才成功；其它状态说明已有别的路径在管它。
bool Scheduler::enter_blocking() noexcept {
    Scheduler* const scheduler = current_scheduler();
    if (scheduler == nullptr || !scheduler->m_impl || !current_task()) {
        return false;
    }
    bool entered = false;
    {
        std::lock_guard<std::mutex> lock(scheduler->m_impl->mutex);
        for (const auto& machine : scheduler->m_impl->machines) {
            if (!machine || machine->id != t_machine_id) {
                continue;
            }
            const auto state = machine->state.load(std::memory_order_relaxed);
            if (state == MState::kRunning) {
                machine->state.store(MState::kBlocking,
                                     std::memory_order_release);
                machine->blocking_since_ns.store(steady_now_ns(),
                                                std::memory_order_release);
                const auto task = current_task();
                machine->blocking_task.store(
                    task ? task->id() : 0, std::memory_order_release);
                machine->processor_detached.store(false,
                                                   std::memory_order_release);
                scheduler->m_impl->blocking_workers.fetch_add(
                    1, std::memory_order_relaxed);
                entered = true;
            }
            break;
        }
    }
    if (entered) {
        t_blocking_machine_id = t_machine_id;
        // Growth is deliberately outside the admission lock.  A queued G can
        // now obtain a replacement M even when this M enters a native call.
        scheduler->m_impl->maybe_grow(scheduler);
    }
    return entered;
}

// 离开 BlockingRegion：按 thread_local 记下的 machine id 恢复 M 状态。
// 用 t_blocking_machine_id 而不是重新查 current，是因为进入与离开之间
// 可能发生过 Fiber 迁移 —— 必须恢复**进入时那台 M**，否则会留下
// 一台永远停在 kBlocking 的 M，sysmon 会反复尝试 handoff 它。
void Scheduler::leave_blocking() noexcept {
    Scheduler* const scheduler = current_scheduler();
    if (scheduler == nullptr || !scheduler->m_impl ||
        t_blocking_machine_id == 0) {
        return;
    }
    leave_blocking_for(scheduler, t_blocking_machine_id);
    t_blocking_machine_id = 0;
}

void Scheduler::leave_blocking_for(Scheduler* scheduler,
                                   MId machine_id) noexcept {
    if (scheduler == nullptr || !scheduler->m_impl || machine_id == 0) {
        return;
    }
    bool left_blocking = false;
    {
        std::lock_guard<std::mutex> lock(scheduler->m_impl->mutex);
        for (const auto& machine : scheduler->m_impl->machines) {
            if (!machine || machine->id != machine_id) {
                continue;
            }
            if (machine->state.load(std::memory_order_relaxed) ==
                MState::kBlocking) {
                const bool detached = machine->processor_detached.exchange(
                    false, std::memory_order_acq_rel);
                machine->blocking_since_ns.store(0, std::memory_order_release);
                machine->blocking_task.store(0, std::memory_order_release);
                machine->state.store(MState::kRunning,
                                     std::memory_order_release);
                scheduler->m_impl->blocking_workers.fetch_sub(
                    1, std::memory_order_relaxed);
                if (detached) {
                    auto& processor = scheduler->m_impl->processors[
                        machine->processor %
                        scheduler->m_impl->processors.size()];
                    processor.active_machines.fetch_add(
                        1, std::memory_order_acq_rel);
                    processor.state.store(PState::kRunning,
                                          std::memory_order_release);
                }
                left_blocking = true;
            }
            break;
        }
    }
    if (left_blocking) {
        // 阻塞区结束意味着补位 M 的使命已经完成：唤醒挂起的补充 M，让它
        // 当场退休，而不是再等一个 idle_worker_timeout。通知放在锁外，
        // 避免被唤醒的 worker 立刻阻塞在 mutex 上。
        scheduler->m_impl->notify_all_workers();
    }
}

std::size_t Scheduler::processor_count() const noexcept {
    return m_impl ? m_impl->processors.size() : 0;
}

std::size_t Scheduler::worker_count() const noexcept {
    return m_impl ? m_impl->active_workers.load(std::memory_order_acquire) : 0;
}

/**
 * @brief Doctor 的最小受控唤醒：转发既有 `notify_all_workers()`。
 *
 * 为什么不直接调 `m_impl->condition.notify_all()`：
 * `notify_all_workers()` 同时覆盖两类空闲等待——睡在调度器
 * `ParkingCondition` 上的 worker，以及睡在 IO 后端 `epoll_wait` 上的 worker
 * （后者要写 eventfd）。只 notify 条件变量会漏掉后者。
 *
 * 为什么不需要持 `Impl::mutex`：`notify_all_workers()` 的设计就是锁外通知
 * （见其定义处的注释），worker 醒来后会自己取锁并重检状态；在诊断线程持锁
 * 反而会增加与 park 路径的锁序耦合。
 *
 * 失败隔离：`IoIdlePoller::wake` 是外部注入的 `std::function`，理论上可能
 * 抛异常；本接口声明 noexcept，因此这里吞掉异常（唤醒失败等价于「没有
 * 唤醒」，不会把诊断线程打崩）。
 */
std::size_t Scheduler::ForceWakeIdle() noexcept {
    if (!m_impl) {
        return 0;
    }
    try {
        m_impl->notify_all_workers();
    } catch (...) {
        // 外部 poller 的 wake 抛出：保持诊断接口 noexcept。
    }
    return worker_count();
}

std::size_t Scheduler::runnable_count() const noexcept {
    return m_impl ? m_impl->runnable.load(std::memory_order_acquire) : 0;
}

std::size_t Scheduler::global_runnable_count() const noexcept {
    if (!m_impl) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    return m_impl->global_queue.size();
}

std::vector<ProcessorSnapshot> Scheduler::processors() const {
    std::vector<ProcessorSnapshot> result;
    if (!m_impl) {
        return result;
    }
    result.reserve(m_impl->processors.size());
    for (const auto& processor : m_impl->processors) {
        std::lock_guard<std::mutex> lock(processor.mutex);
        result.push_back(ProcessorSnapshot{
            processor.id, processor.state.load(std::memory_order_acquire),
            processor.queue.size()});
    }
    return result;
}


std::vector<TaskSnapshot> Scheduler::DebugSnapshotTasks() const {
    std::vector<TaskSnapshot> result;
    if (!m_impl) {
        return result;
    }
    // 只返回**非终态** G。终态（kDead/kCancelled/kFailed）已不再参与调度，
    // 但可能仍滞留在 registry 中等待 collect_terminal_locked() 回收；若全量
    // dump 枚举它们，长跑进程的 tasks 会随「历史完成任务数」线性膨胀
    // （实测 48s 达 23 万条 / 61MB），并可能撑爆 CLI 的 64MiB 传输上限。
    // 未回收的终态 G 属于调度器内部回收滞后，不应出现在对外观测快照里。
    // 分片遍历：每片单独持锁，不长时间阻塞其它分片的新增/回收。
    // 用 task_registries.size() 而不是硬编码 16，避免与常量漂移。
    for (std::size_t stripe = 0; stripe < m_impl->task_registries.size(); ++stripe) {
        std::lock_guard<std::mutex> lock(m_impl->registry_mutexes[stripe]);
        // 不按 registry 大小 reserve：过滤后存活 G 通常只有个位数，
        // 按全量预留会在历史任务很多时白白分配大块内存。
        for (const auto& task : m_impl->task_registries[stripe]) {
            if (!task || task->terminal()) {
                continue;
            }
            TaskSnapshot snapshot;
            snapshot.id = task->id();
            snapshot.state = task->state();
            snapshot.queued = task->queued();
            snapshot.started = task->started();
            snapshot.last_processor_id = task->last_processor_id();
            snapshot.task_class = task->task_class();
            snapshot.failed = task->failed();
            snapshot.cancellation_requested = task->cancellation_requested();
            result.push_back(snapshot);
        }
    }
    return result;
}

bool Scheduler::IsTaskAlive(GId id) const {
    if (!m_impl) {
        return false;
    }
    // 单个 gid 只可能落在它 id 对应的那一个条带里（注册时用同一个
    // id 按位与「分片数 - 1」选片），所以这里只锁一个分片、只扫这一片，
    // 不枚举全量注册表；开销与历史任务数无关。
    const std::size_t stripe =
        static_cast<std::size_t>(id) & (Impl::kRegistryStripeCount - 1);
    std::lock_guard<std::mutex> registry_lock(
        m_impl->registry_mutexes[stripe]);
    for (const auto& task : m_impl->task_registries[stripe]) {
        if (task && task->id() == id) {
            // 注册表可能仍滞留已进入终态、尚未被 collect_terminal_locked
            // 回收的 G，因此必须看 terminal()，不能「查到了就算活」。
            return !task->terminal();
        }
    }
    return false;
}

TaskTerminalState Scheduler::TaskTerminal(GId id) const {
    if (!m_impl) {
        return TaskTerminalState::kUnknown;
    }
    const std::size_t stripe =
        static_cast<std::size_t>(id) & (Impl::kRegistryStripeCount - 1);
    std::lock_guard<std::mutex> registry_lock(
        m_impl->registry_mutexes[stripe]);
    for (const auto& task : m_impl->task_registries[stripe]) {
        if (!task || task->id() != id) {
            continue;
        }
        switch (task->state()) {
            case GState::kDead:
                // kDead 是 Fiber 正常执行完毕的终态。
                return TaskTerminalState::kCompleted;
            case GState::kFailed:
                return TaskTerminalState::kFailed;
            case GState::kCancelled:
                return TaskTerminalState::kCancelled;
            default:
                // 仍是活动状态：不是终态，返回 kUnknown；需要区分存活时
                // 先调 IsTaskAlive()。
                return TaskTerminalState::kUnknown;
        }
    }
    return TaskTerminalState::kUnknown;
}

std::vector<MachineSnapshot> Scheduler::machines() const {
    std::vector<MachineSnapshot> result;
    if (!m_impl) {
        return result;
    }
    // start() appends to this deque while holding the scheduler mutex.  Take
    // the same lock for snapshots so embedders can inspect machines while a
    // scheduler is being started without racing container mutation.
    std::lock_guard<std::mutex> scheduler_lock(m_impl->mutex);
    result.reserve(m_impl->machines.size());
    for (const auto& machine : m_impl->machines) {
        if (!machine) {
            continue;
        }
        result.push_back(MachineSnapshot{
            machine->id, machine->state.load(std::memory_order_acquire),
            machine->processor,
            machine->last_task_class.load(std::memory_order_relaxed),
            machine->affinity_hits.load(std::memory_order_relaxed),
            machine->affinity_misses.load(std::memory_order_relaxed),
            machine->processor_detached.load(std::memory_order_acquire),
            machine->blocking_task.load(std::memory_order_acquire),
            machine->long_syscall_count.load(std::memory_order_acquire),
            machine->replacement});
    }
    return result;
}

bool Scheduler::sysmon_running() const noexcept {
    return m_impl && m_impl->sysmon_active.load(std::memory_order_acquire) &&
           !m_impl->sysmon_stop.load(std::memory_order_acquire);
}

std::uint64_t Scheduler::sysmon_pass_count() const noexcept {
    return m_impl ? m_impl->sysmon_pass_count.load(std::memory_order_acquire)
                  : 0;
}

SchedulerMetrics Scheduler::metrics() const noexcept {
    if (!m_impl) {
        return {};
    }
    return {m_impl->metric_task_runs.load(std::memory_order_acquire),
            m_impl->metric_task_completions.load(std::memory_order_acquire),
            m_impl->metric_fiber_resume_ns.load(std::memory_order_acquire),
            m_impl->metric_local_queue_pops.load(std::memory_order_acquire),
            m_impl->metric_steal_pops.load(std::memory_order_acquire)};
}

std::shared_ptr<Task> Scheduler::current_task() noexcept {
    return t_task;
}

Task* Scheduler::current_task_ptr() noexcept {
    return t_task.get();
}

Scheduler* Scheduler::current_scheduler() noexcept {
    return t_scheduler;
}

MId Scheduler::current_machine_id() noexcept {
    return t_machine_id;
}

PId Scheduler::current_processor_id() noexcept {
    return t_processor_id;
}

/**
 * @brief M（OS 线程）的主循环：取 G、执行、回收，空转时做维护。
 * @param opaque_machine 该 M 的 `Impl::Machine`；为空时直接返回（防御性检查）。
 *
 * ## 定位
 *
 * 这是整个调度器的**唯一执行驱动**：所有 G 都在这一个 `for (;;)` 里被
 * 「取出 → 标记 running → 执行 → 回收」。P 只是队列容器，真正的并行度来自
 * 同时跑在多少个 M 上。
 *
 * ## 步骤
 *
 * step1. 绑定线程本地身份（`t_scheduler` / `t_machine_id` / `t_processor_id`），
 *        可选地把当前线程 pin 到与本 P 对应的 CPU；
 * step2. **快路径**：直接取本 P 的本地队列（`own_processor.queue`）队首。
 *        本 P 的队列由本 P 的 worker 独占消费，因此不需要调度器总锁；
 * step3. **中路径**：扫描 `incoming` 分条队列（跨线程投递的入口）。
 *        若本 M 有任务类亲和且预算未耗尽，先按 class 匹配，再退化为 FIFO；
 * step4. **慢路径**：本地与 incoming 都空时，才在 `m_impl->mutex` 这把
 *        admission 锁下依次尝试 `emergency_pop` → 本地 FIFO → 全局 FIFO →
 *        从其它 P 的**队尾**窃取（窃取用队尾，减少与本地消费者的争用）；
 * step5. 执行前做 M/P 计数与亲和记账，然后 `task->run()`；返回后按
 *        终态发事件、记指标，并把「yield 回来且已 unqueue」的 G 重新入队；
 * step6. 空转或到达摊销周期时调用 `maybe_grow()`（补充 M 属于维护路径，
 *        不放在 Fiber 切换热路径上）。
 *
 * ## 并发/失败语义
 *
 * - **取任务的三条路径互斥**：只有一条会成功给 `task` 赋值，且每条路径都
 *   必须完成 `runnable` 递减与 `try_mark_running()` 认领；
 * - `try_mark_running()` 失败（任务已被取消或已死）时必须 `clear_queued()`，
 *   否则队列节点的 reservation 会泄漏；
 * - `task->run()` 返回后**必须**把 `running_workers` / `running_machines` /
 *   `MState` / `PState` 全部还原，否则后续唤醒判定会把空闲 P 误判为忙；
 * - 若 `t_blocking_machine_id != 0`（BlockingRegion 跨越了 G 边界），
 *   这里 `std::terminate()`：这是不可恢复的状态损坏，静默清理会传播不一致。
 */
void Scheduler::worker_loop(std::shared_ptr<void> opaque_machine) {
    auto machine = std::static_pointer_cast<Impl::Machine>(opaque_machine);
    if (!machine) {
        return;
    }
    t_scheduler = this;
    thread_policy::detail::EnterRuntimeWorker(this);
    t_machine_id = machine->id;
    t_processor_id = machine->processor;
    // 把当前线程的 P 发布给事件环：之后所有事件都带 pid。
    GO2CPP_DEBUG_BIND_PROCESSOR(t_processor_id);
    m_impl->emit_event(SchedulerEvent{SchedulerEventType::kWorkerStarted,
                                      0, machine->id, machine->processor});
    GO2CPP_DEBUG_EVENT(kThreadStart, machine->id, machine->processor);
#if defined(__linux__)
    if (m_impl->config.pin_workers_to_cpu &&
        machine->processor < static_cast<PId>(CPU_SETSIZE)) {
        cpu_set_t cpu_set;
        CPU_ZERO(&cpu_set);
        CPU_SET(static_cast<int>(machine->processor), &cpu_set);
        // 亲和性是优化提示；容器或 cpuset 拒绝时继续运行，不改变语义。
        (void)::pthread_setaffinity_np(pthread_self(), sizeof(cpu_set),
                                       &cpu_set);
    }
#endif
    auto& own_processor =
        m_impl->processors[machine->processor % m_impl->processors.size()];
    bool retired = false;
    std::size_t maintenance_budget = 0;
    // 距上次终态回收又完成了多少个终态 G。只有进入终态的 G 才会增加
    // 注册表回收压力，yield/park 不计数。
    std::size_t terminal_since_collect = 0;

    // B1：dequeue 的 claim 失败收尾。旧实现无条件 clear_queued()，会把
    // 「pop 之后、clear 之前」新发布的 reservation 一起清掉，制造
    // 「kRunnable 但没有任何队列节点」的唤醒丢失。这里按任务当前状态
    // 条件清理：确实不再需要队列节点才清，仍可运行且无执行者则需要补发布。
    //
    // 注意：本函数只做**判定**，不调用 enqueue()。三个调用点分别在
    // admission 锁内（慢路径）、incoming 条带锁内（中路径）和无锁
    // （本地快路径）执行；锁内 enqueue 会自锁或形成反向锁序
    // （SCHED-01/02）。调用方把判定结果带到所有调度器锁之外再补发布。
    const auto recover_unclaimed = [](const std::shared_ptr<Task>& task) {
        std::lock_guard<std::mutex> lock(task->m_transition_mutex);
        const auto state = task->m_state.load(std::memory_order_relaxed);
        const bool executing =
            task->m_execution_claim.load(std::memory_order_relaxed);
        if (state == GState::kRunnable && !executing) {
            // 仍可运行且没有执行者：m_queued 为 true 说明已有更新的
            // 队列节点（保留）；为 false 说明这个 G 已经没有节点，
            // 必须补发布，否则它将永久停在 Runnable。
            return !task->m_queued.load(std::memory_order_relaxed);
        }
        // 已不可运行 / 已被别的 worker claim：刚 pop 的节点是残留，
        // 清除 queued 不会影响任何有效 reservation。
        task->m_queued.store(false, std::memory_order_release);
        return false;
    };

    for (;;) {
        std::shared_ptr<Task> task;
        bool claimed = false;
        bool local_queue_pop = false;
        // B1 的补发布是延迟动作：三个找活路径都可能持有 admission /
        // incoming 条带锁，必须等本轮所有调度器锁释放后再 enqueue。
        std::shared_ptr<Task> unclaimed_republish;
        // step2: 快路径——只碰本 P 的本地队列，不加调度器总锁。
        // 绝大多数 yield 回来的 G 都会回到原 P。本地队列由同一 P 的
        // worker 独占消费；只有本地为空时才进入需要全局 admission 锁的
        // 全局队列/窃取路径。
        if (!m_impl->stopping.load(std::memory_order_acquire) &&
            m_impl->running_workers.load(std::memory_order_acquire) != 0 &&
            m_impl->runnable.load(std::memory_order_acquire) >
                m_impl->processors.size()) {
            {
                std::lock_guard<std::mutex> queue_lock(own_processor.mutex);
                if (!own_processor.queue.empty()) {
                    task = std::move(own_processor.queue.front());
                    own_processor.queue.pop_front();
                    local_queue_pop = true;
                    GO2CPP_DEBUG_EVENT(kRunQueuePop, task->id(),
                                       own_processor.id);
                }
            }
            if (task) {
                m_impl->runnable.fetch_sub(1, std::memory_order_relaxed);
                claimed = task->try_mark_running();
                if (claimed) {
                    GO2CPP_DEBUG_EVENT(kRunQueueClaimOk, task->id(), 0);
                } else {
                    GO2CPP_DEBUG_EVENT(kRunQueueClaimFail, task->id(), 0);
                    if (recover_unclaimed(task)) {
                        unclaimed_republish = task;
                    }
                }
            }
        }
        if (task && m_impl->stopping.load(std::memory_order_acquire)) {
            task->request_cancel(false);
        }
        if (!task && !m_impl->stopping.load(std::memory_order_acquire)) {
            // step3: 中路径——扫描 incoming 分条队列（跨线程投递入口）。
            // 每个 stripe 有自己的锁；先按任务类亲和匹配，再从 start 开始
            // 轮转扫全部 stripe，避免固定顺序造成饥饿。
            if (m_impl->incoming_count.load(std::memory_order_acquire) != 0) {
                const auto start = m_impl->incoming_probe.fetch_add(
                    1, std::memory_order_relaxed);
                const auto preferred_class = machine->last_task_class.load(
                    std::memory_order_relaxed);
                const bool prefer_class =
                    preferred_class != 0 &&
                    machine->affinity_budget.load(std::memory_order_relaxed) !=
                        0 &&
                    m_impl->config.task_affinity_budget != 0;
                const auto take_incoming = [&](std::size_t stripe,
                                               bool require_class) {
                    IncomingStripeLock incoming_lock(
                        m_impl->incoming_mutexes[stripe]);
                    auto& incoming_queue = m_impl->incoming_queues[stripe];
                    if (incoming_queue.empty()) {
                        return false;
                    }
                    auto selected = incoming_queue.begin();
                    if (require_class) {
                        std::size_t scanned = 0;
                        for (auto it = selected;
                             it != incoming_queue.end() &&
                             scanned < m_impl->config.task_affinity_budget;
                             ++it, ++scanned) {
                            if (*it && (*it)->task_class() == preferred_class) {
                                selected = it;
                                break;
                            }
                        }
                        if (!*selected ||
                            (*selected)->task_class() != preferred_class) {
                            return false;
                        }
                    }
                    task = std::move(*selected);
                    incoming_queue.erase(selected);
                    GO2CPP_DEBUG_EVENT(kRunQueuePop, task->id(),
                                       ::go2cpp::debug::kGlobalQueueTag);
                    if (incoming_queue.empty()) {
                        m_impl->incoming_nonempty_mask.fetch_and(
                            ~(std::uint32_t{1} << stripe),
                            std::memory_order_release);
                    }
                    m_impl->incoming_count.fetch_sub(
                        1, std::memory_order_release);
                    m_impl->runnable.fetch_sub(1, std::memory_order_relaxed);
                    claimed = task->try_mark_running();
                    if (claimed) {
                        GO2CPP_DEBUG_EVENT(kRunQueueClaimOk, task->id(), 0);
                    } else {
                        GO2CPP_DEBUG_EVENT(kRunQueueClaimFail, task->id(), 0);
                        // 锁内只判定；enqueue 留到条带锁释放后，否则会与
                        // shutdown 的 admission→incoming 反向（SCHED-02）。
                        if (recover_unclaimed(task)) {
                            unclaimed_republish = task;
                        }
                    }
                    return true;
                };
                // Preserve task-class locality despite sharded producers: try
                // all stripes for a matching class before accepting another
                // class. The fallback keeps FIFO-like progress if no match
                // is currently available.
                const auto scan_mask = [&] {
                    return m_impl->incoming_nonempty_mask.load(
                        std::memory_order_acquire);
                };
                auto mask = scan_mask();
                if (prefer_class) {
                    for (std::size_t offset = 0;
                         offset < Scheduler::Impl::kIncomingStripeCount &&
                         !task;
                         ++offset) {
                        const auto stripe =
                            (start + offset) &
                            (Scheduler::Impl::kIncomingStripeCount - 1);
                        if ((mask & (std::uint32_t{1} << stripe)) == 0) {
                            continue;
                        }
                        (void)take_incoming(stripe, true);
                        mask = scan_mask();
                    }
                }
                if (!task) {
                    for (std::size_t offset = 0;
                         offset < Scheduler::Impl::kIncomingStripeCount &&
                         !task;
                         ++offset) {
                        const auto stripe =
                            (start + offset) &
                            (Scheduler::Impl::kIncomingStripeCount - 1);
                        if (mask != 0 &&
                            (mask & (std::uint32_t{1} << stripe)) == 0) {
                            continue;
                        }
                        (void)take_incoming(stripe, false);
                        mask = scan_mask();
                    }
                }
            }
        }
        if (task && m_impl->stopping.load(std::memory_order_acquire)) {
            task->request_cancel(false);
        }
        if (!task) {
            // step4: 慢路径——本地与 incoming 都空，进入全局 admission 锁。
            // 顺序固定为：emergency → 本地 FIFO → 全局 FIFO → 窃取队尾。
            // Queue removal, the shutdown check, and the runnable->running
            // claim form one admission transaction.  A task already claimed
            // from this worker's local queue skips this global mutex entirely:
            // the worker owns the execution claim and the local queue lock has
            // already made the node invisible to stealers.
#ifdef GO2CPP_COUNT_GLOBAL_LOCK
            LockStatGuard admission_stat(m_impl->mutex);
            std::unique_lock<std::mutex> admission_lock(m_impl->mutex,
                                                        std::adopt_lock);
            const auto stat_lock = &admission_lock;
            (void)stat_lock;
#else
            std::lock_guard<std::mutex> admission_lock(m_impl->mutex);
#endif
            if (!m_impl->stopping.load(std::memory_order_acquire)) {
                const bool prefer_class =
                    machine->last_task_class.load(std::memory_order_relaxed) != 0 &&
                    machine->affinity_budget.load(std::memory_order_relaxed) != 0 &&
                    m_impl->config.task_affinity_budget != 0;
                const auto preferred_class =
                    machine->last_task_class.load(std::memory_order_relaxed);
                const auto scan_limit = m_impl->config.task_affinity_budget;
                const auto take_local = [&](auto& queue) {
                    if (prefer_class) {
                        auto it = queue.begin();
                        std::size_t scanned = 0;
                        for (; it != queue.end() && scanned < scan_limit;
                             ++it, ++scanned) {
                            if (*it && (*it)->task_class() == preferred_class) {
                                auto selected = std::move(*it);
                                queue.erase(it);
                                GO2CPP_DEBUG_EVENT(kRunQueuePop,
                                                   selected->id(),
                                                   own_processor.id);
                                return selected;
                            }
                        }
                    }
                    if (queue.empty()) {
                        return std::shared_ptr<Task>{};
                    }
                    auto selected = std::move(queue.front());
                    queue.pop_front();
                    GO2CPP_DEBUG_EVENT(kRunQueuePop, selected->id(),
                                       own_processor.id);
                    return selected;
                };
                const auto take_global = [&](auto& queue) {
                    if (prefer_class) {
                        auto it = queue.begin();
                        std::size_t scanned = 0;
                        for (; it != queue.end() && scanned < scan_limit;
                             ++it, ++scanned) {
                            if (*it && (*it)->task_class() == preferred_class) {
                                auto selected = std::move(*it);
                                queue.erase(it);
                                GO2CPP_DEBUG_EVENT(kRunQueuePop,
                                                   selected->id(),
                                                   ::go2cpp::debug::kGlobalQueueTag);
                                return selected;
                            }
                        }
                    }
                    if (queue.empty()) {
                        return std::shared_ptr<Task>{};
                    }
                    auto selected = std::move(queue.front());
                    queue.pop_front();
                    GO2CPP_DEBUG_EVENT(kRunQueuePop, selected->id(),
                                       ::go2cpp::debug::kGlobalQueueTag);
                    return selected;
                };
                const auto take_victim = [&](auto& queue,
                                             [[maybe_unused]] PId victim_id) {
                    if (prefer_class) {
                        auto it = queue.rbegin();
                        std::size_t scanned = 0;
                        for (; it != queue.rend() && scanned < scan_limit;
                             ++it, ++scanned) {
                            if (*it && (*it)->task_class() == preferred_class) {
                                auto base = std::next(it).base();
                                auto selected = std::move(*base);
                                queue.erase(base);
                                GO2CPP_DEBUG_EVENT(kRunQueuePop,
                                                   selected->id(), victim_id);
                                return selected;
                            }
                        }
                    }
                    if (queue.empty()) {
                        return std::shared_ptr<Task>{};
                    }
                    auto selected = std::move(queue.back());
                    queue.pop_back();
                    GO2CPP_DEBUG_EVENT(kRunQueuePop, selected->id(),
                                       victim_id);
                    return selected;
                };

                // Rescue resumes are serviced first: they exist only after
                // queue allocation failure or a shutdown wake handoff.
                task = m_impl->emergency_pop_locked();

                // Prefer the local FIFO queue.
                if (!task) {
                    std::lock_guard<std::mutex> queue_lock(own_processor.mutex);
                    if (!own_processor.queue.empty()) {
                        task = take_local(own_processor.queue);
                    }
                }

                // Then inspect the global FIFO queue.
                if (!task && !m_impl->global_queue.empty()) {
                    task = take_global(m_impl->global_queue);
                }

                // Finally steal from the back of another P. A bounded scan
                // avoids monopolizing the scheduler when all processors are
                // busy.
                if (!task && m_impl->processors.size() > 1) {
                    const auto start = m_impl->next_processor.fetch_add(
                        1, std::memory_order_relaxed);
                    [[maybe_unused]] std::uint64_t victim_id = 0;
                    for (std::size_t offset = 0;
                         offset < m_impl->processors.size(); ++offset) {
                        auto& victim = m_impl->processors[
                            (start + offset) % m_impl->processors.size()];
                        if (&victim == &own_processor) {
                            continue;
                        }
                        std::lock_guard<std::mutex> queue_lock(victim.mutex);
                        if (!victim.queue.empty()) {
                            task = take_victim(victim.queue, victim.id);
                            if (task) {
                                m_impl->metric_steal_pops.fetch_add(
                                    1, std::memory_order_relaxed);
                                victim_id =
                                    static_cast<std::uint64_t>(victim.id);
                            }
                            break;
                        }
                    }
                    if (task) {
                        GO2CPP_DEBUG_EVENT(kStealOk, task->id(), victim_id);
                    } else {
                        GO2CPP_DEBUG_EVENT_SAMPLED(
                            kStealFail, 0,
                            static_cast<std::uint64_t>(
                                m_impl->processors.size() - 1),
                            16);
                    }
                }

                if (task) {
                    m_impl->runnable.fetch_sub(1, std::memory_order_relaxed);
                    claimed = task->try_mark_running();
                    if (claimed) {
                        GO2CPP_DEBUG_EVENT(kRunQueueClaimOk, task->id(), 0);
                    } else {
                        GO2CPP_DEBUG_EVENT(kRunQueueClaimFail, task->id(), 0);
                        // 节点已经从队列取出：按任务当前状态条件清理
                        // reservation。补发布必须等 admission 锁释放，
                        // 否则 worker 分支会二次锁同一把 mutex（SCHED-01）。
                        if (recover_unclaimed(task)) {
                            unclaimed_republish = task;
                        }
                    }
                }
            }
        }

        // B1 补发布统一放在所有调度器锁释放之后：enqueue() 的 worker 分支
        // 会获取 admission 锁，而慢路径曾持有 admission（SCHED-01），
        // 中路径曾持有 incoming 条带锁（SCHED-02）。到这里两种锁都已释放。
        if (unclaimed_republish) {
            (void)enqueue(unclaimed_republish);
        }

        // Both the local fast path and the admission path arrive here with a
        // claimed G. Keep the M/P accounting in one place so a local dequeue
        // cannot skip running_workers/affinity bookkeeping.
        if (task && claimed) {
            m_impl->running_workers.fetch_add(1, std::memory_order_relaxed);
            machine->state.store(MState::kRunning, std::memory_order_release);
            const auto task_class = task->task_class();
            const auto last_task_class =
                machine->last_task_class.load(std::memory_order_relaxed);
            if (task_class != 0 && task_class == last_task_class &&
                machine->affinity_budget.load(std::memory_order_relaxed) != 0) {
                machine->affinity_hits.fetch_add(1, std::memory_order_relaxed);
                machine->affinity_budget.fetch_sub(1, std::memory_order_relaxed);
            } else {
                if (last_task_class != 0 && task_class != last_task_class) {
                    machine->affinity_misses.fetch_add(1,
                                                       std::memory_order_relaxed);
                }
                machine->last_task_class.store(task_class,
                                               std::memory_order_relaxed);
                machine->affinity_budget.store(
                    task_class == 0 ? 0 : m_impl->config.task_affinity_budget,
                    std::memory_order_relaxed);
            }
        }

        if (task && claimed) {
            // step6: 维护路径——按「有积压就勤查、否则摊销」的策略决定
            // 是否补充 M。它不在 Fiber 切换热路径上。
            // Growth is maintenance, not part of the Fiber switch hot path.
            // A previous implementation scanned/reaped under scheduler locks
            // for every resume, which dominated short yield workloads. Check
            // quickly on a backlog and otherwise amortize the scan.
            ++maintenance_budget;
            const auto growth_period =
                m_impl->runnable.load(std::memory_order_relaxed) >
                        m_impl->processors.size()
                    ? std::size_t{8}
                    : std::size_t{64};
            const bool needs_initial_worker =
                m_impl->active_workers.load(std::memory_order_relaxed) <
                    m_impl->processors.size() &&
                m_impl->runnable.load(std::memory_order_relaxed) != 0;
            if (needs_initial_worker || maintenance_budget >= growth_period) {
                m_impl->maybe_grow(this);
                maintenance_budget = 0;
            }
            own_processor.running_machines.fetch_add(1,
                                                     std::memory_order_relaxed);
            own_processor.state.store(PState::kRunning,
                                      std::memory_order_release);
            t_task = task;
            GO2CPP_DEBUG_BIND_TASK(task->id());
            Fiber::BindCurrentExecution(FiberExecutionBinding{
                reinterpret_cast<std::uintptr_t>(this), 0, t_machine_id,
                t_processor_id, true});
            m_impl->emit_event(SchedulerEvent{
                SchedulerEventType::kTaskStarted, task->id(), t_machine_id,
                t_processor_id, GState::kRunning});
            const bool collect_metrics = m_impl->config.collect_metrics;
            const auto run_started = collect_metrics ? steady_now_ns() : 0;
            // step5: 执行 G。返回值通过 task 的终态体现（Dead/Failed/
            // Cancelled/Yielded/Parked），这里只负责记账与后续回收。
            task->run();
            const auto task_state_after_run = task->state();
            if (task_state_after_run == GState::kDead ||
                task_state_after_run == GState::kCancelled ||
                task_state_after_run == GState::kFailed) {
                ++terminal_since_collect;
            }
            const auto task_event_type =
                task_state_after_run == GState::kDead
                    ? SchedulerEventType::kTaskCompleted
                : task_state_after_run == GState::kFailed
                    ? SchedulerEventType::kTaskFailed
                : task_state_after_run == GState::kCancelled
                    ? SchedulerEventType::kTaskCancelled
                    : SchedulerEventType::kTaskSuspended;
            m_impl->emit_event(SchedulerEvent{
                task_event_type, task->id(), t_machine_id, t_processor_id,
                task_state_after_run});
            if (collect_metrics) {
                const auto resume_elapsed =
                    static_cast<std::uint64_t>(std::max<std::int64_t>(
                        0, steady_now_ns() - run_started));
                m_impl->metric_task_runs.fetch_add(
                    1, std::memory_order_relaxed);
                m_impl->metric_fiber_resume_ns.fetch_add(
                    resume_elapsed, std::memory_order_relaxed);
                if (task->terminal()) {
                    m_impl->metric_task_completions.fetch_add(
                        1, std::memory_order_relaxed);
                }
                if (local_queue_pop) {
                    m_impl->metric_local_queue_pops.fetch_add(
                        1, std::memory_order_relaxed);
                }
            }
            t_task.reset();
            GO2CPP_DEBUG_UNBIND_TASK();
            Fiber::BindCurrentExecution(FiberExecutionBinding{
                reinterpret_cast<std::uintptr_t>(this), 0, t_machine_id,
                t_processor_id, true});
            // BlockingRegion 不能跨 Fiber yield/park 或 G 的任务边界。若
            // 仍有活动标记，继续调度会让 blocking_workers 和 P 的 attached
            // 计数永久失真；宁可在边界处 fail-fast，也不能静默清掉 TLS 后
            // 把不一致状态传给后续 G。正常 Hook/RAII 路径在这里必为 0。
            if (t_blocking_machine_id != 0) {
                std::terminate();
            }
            if (own_processor.running_machines.fetch_sub(
                    1, std::memory_order_acq_rel) == 1) {
                own_processor.state.store(PState::kIdle,
                                          std::memory_order_release);
            }
            machine->state.store(MState::kIdle, std::memory_order_release);
            m_impl->running_workers.fetch_sub(1, std::memory_order_acq_rel);

            // A yield issued by the running G is committed only after its
            // execution claim is released, preventing a second M from
            // entering the same callable concurrently.
            bool requeued_self = false;
            if (task->state() == GState::kRunnable &&
                !task->queued()) {
                (void)task->consume_deferred_enqueue();
                // 刚 yield 的 G 放回**本** P 的队列，当前 M 马上就会从
                // 队首取它。此时再 notify_one 会唤醒一个睡着的 M，让它
                // 从队尾偷走这个 G —— 结果是一次跨 M 迁移 + 一次
                // wake-from-sleep（实测 4~5µs），而本地继续执行只要
                // 几百纳秒。allow_notify=false 关掉这个虚假唤醒。
                requeued_self = requeue_from_worker(task, false);
                if (!requeued_self && !task->cancel_if_runnable_unqueued()) {
                    // 入队失败且 cancel_if_runnable_unqueued() 也收敛不了，
                    // 说明这是一个**已启动**的 G（它只处理未启动的）。
                    // draining/stopping 时 enqueue() 的 worker 分支会直接
                    // 丢弃已启动 G；若不在这里补一条不可失败的发布路径，
                    // G 就停在 Runnable 且无人调度，shutdown 永远等不到
                    // 它的终态。紧急队列不分配、不抛异常，是唯一安全出口。
                    std::lock_guard<std::mutex> orphan_lock(m_impl->mutex);
                    if (task->try_mark_queued()) {
                        m_impl->runnable.fetch_add(1,
                                                   std::memory_order_relaxed);
                        m_impl->emergency_enqueue_locked(task);
                        m_impl->notify_worker();
                    }
                }
            } else {
                (void)task->consume_deferred_enqueue();
            }
            // 不变式：run() 已返回，G 就不可能还处于 kRunning 且无人执行。
            // 一旦出现（执行权交接窗口的残留），必须立刻自愈重新发布，
            // 绝不能把"永远不会被调度的 Running"留给 shutdown 无限等待。
            if (task->recover_orphan_running()) {
                if (!requeue_from_worker(task)) {
                    task->clear_queued();
                    std::lock_guard<std::mutex> admission_lock(m_impl->mutex);
                    if (task->try_mark_queued()) {
                        m_impl->runnable.fetch_add(1,
                                                   std::memory_order_relaxed);
                        m_impl->emergency_enqueue_locked(task);
                        m_impl->notify_worker();
                    }
                }
            }
            std::vector<std::shared_ptr<Task>> deferred_destruction;
            const bool draining =
                m_impl->draining.load(std::memory_order_acquire);
            const std::size_t collect_interval =
                m_impl->config.terminal_collect_interval;
            if (draining) {
                // 收尾路径必须全表回收，并配合 finish_draining_locked()
                // 判断注册表是否已全部终态。
                std::lock_guard<std::mutex> admission_lock(m_impl->mutex);
                m_impl->collect_terminal_locked(deferred_destruction);
                (void)m_impl->finish_draining_locked();
                terminal_since_collect = 0;
            } else if (collect_interval != 0 &&
                       terminal_since_collect >= collect_interval) {
                // 常规路径按节拍轮转压缩一个条带：单次扫描量是存活 G 总数
                // 的 1/16，约 16 个节拍覆盖全表；终态 G 在表内的滞留上限
                // 是 collect_interval * 16，不随历史完成任务数增长。
                // 回收只拿条带锁，不争用 admission 锁。
                const std::size_t stripe =
                    m_impl->next_terminal_collect_stripe.fetch_add(
                        1, std::memory_order_relaxed) &
                    (Scheduler::Impl::kRegistryStripeCount - 1);
                m_impl->collect_terminal_stripe_locked(stripe,
                                                       deferred_destruction);
                terminal_since_collect = 0;
            }
            if (draining) {
                m_impl->notify_all_workers();
            } else if (!requeued_self &&
                       task->state() != GState::kWaiting &&
                       m_impl->runnable.load(std::memory_order_relaxed) != 0) {
                // 只有"本 M 真正结束了一个 G、且仍有可运行任务"时才踢一个
                // 空闲 M。
                //
                // 两种常见返回不满足这个条件，都**不能**在这里 notify：
                // - yield 回队（requeued_self）：G 刚放回本 P 队首，本 M
                //   下一轮就会取它；
                // - park 返回（state == kWaiting）：唤醒方（Unlock）已经把
                //   下一个 G 放进了本 P 队列，本 M 下一轮就会取它。
                // 这两种情况下去 notify_one，会唤醒一个睡着的 M，让它从
                // 队尾把该 G 偷走，把几百纳秒的本地续跑放大成 4~5µs 的
                // 跨 M 唤醒（64 Fiber 实测整档慢 10 倍以上）。其他队列的
                // 可运行工作由发布方（enqueue / wake_registered）负责唤醒。
                m_impl->notify_worker();
            }
            for (auto& retained_task : deferred_destruction) {
                if (retained_task) {
                    auto abandoned_function =
                        retained_task->release_callable();
                }
            }
            deferred_destruction.clear();
            continue;
        }

        if (task && !claimed) {
            // A cancelled/unstarted queue node can lose its execution claim
            // after dequeue. It still has a registry entry; prune it here so
            // a quiet scheduler does not retain terminal callables forever.
            std::vector<std::shared_ptr<Task>> deferred_destruction;
            {
                std::lock_guard<std::mutex> admission_lock(m_impl->mutex);
                m_impl->collect_terminal_locked(deferred_destruction);
            }
            terminal_since_collect = 0;
            for (auto& retained_task : deferred_destruction) {
                if (retained_task) {
                    auto abandoned_function =
                        retained_task->release_callable();
                }
            }
            deferred_destruction.clear();
            // A concurrent external cancellation can make the registry
            // terminal without touching the scheduler condition variable.
            // Wake an owner waiting for the final terminal transition after
            // this stale queue node has been consumed.
            m_impl->notify_all_workers();
        }

        if (m_impl->stopping.load(std::memory_order_acquire)) {
            machine->state.store(MState::kStopping, std::memory_order_release);
            break;
        }

        // ------------------------------------------------------------------
        // Go runtime.netpoll 模型：没有 G 可跑时，M 自己进入 IO 后端等待。
        //
        // 就绪事件由本线程就地处理，wake_io() 走 requeue_from_worker 把 G
        // 投回本 P，于是 park -> epoll_wait -> wake 在同一个 M 上闭环，
        // 不再有「poller 线程 -> 条件变量 -> worker」的跨线程唤醒。
        //
        // 只在常驻 M 上启用：
        // - allow_idle_shrink 打开时保留条件变量路径，否则空闲退休判定永不
        //   执行（历史语义不能悄悄改变）；
        // - 补充 M 的职责是接管被阻塞区占住的 P，只有在确实存在阻塞区时
        //   才轮询；阻塞区结束后它仍需走条件变量路径退休。
        // ------------------------------------------------------------------
        const Impl::IoIdlePoller* idle_poller =
            m_impl->io_poller.load(std::memory_order_acquire);
        if (idle_poller != nullptr && !m_impl->draining.load(std::memory_order_acquire) &&
            !m_impl->config.allow_idle_shrink &&
            (!machine->replacement ||
             m_impl->blocking_workers.load(std::memory_order_acquire) != 0)) {
            machine->state.store(MState::kParked, std::memory_order_release);
            if (own_processor.running_machines.load(std::memory_order_acquire) ==
                0) {
                own_processor.state.store(PState::kIdle,
                                          std::memory_order_release);
            }
            // 先登记「本 M 即将进入后端等待」，再复查可运行计数：
            // 这样登记之后到达的 notify_worker()/enqueue() 一定会看到非零
            // 计数并写 eventfd，不会落进「刚检查完队列、还没进 epoll_wait」
            // 的丢唤醒窗口。
            m_impl->io_waiting_workers.fetch_add(1, std::memory_order_release);
            const bool work_pending =
                m_impl->runnable.load(std::memory_order_acquire) != 0 ||
                m_impl->stopping.load(std::memory_order_acquire);
            int polled = 0;
            if (!work_pending) {
                auto timeout_ms =
                    static_cast<int>(m_impl->config.idle_wait.count());
                if (timeout_ms < 1) {
                    timeout_ms = 1;
                }
                const IdlePollScope poll_scope;
                polled = idle_poller->poll(timeout_ms);
            }
            m_impl->io_waiting_workers.fetch_sub(1, std::memory_order_release);
            machine->state.store(MState::kIdle, std::memory_order_release);
            if (work_pending || polled >= 0) {
                // 轮到事件（polled > 0）和正常超时（polled == 0）都直接回到
                // 主循环：队列检查很便宜，且能避免条件变量与 epoll 等待
                // 重叠时 notify 落到空窗里。
                continue;
            }
            // polled < 0：IO 后端已停用（IOManager shutdown 中），退回下面
            // 的条件变量等待，避免忙循环。
        }

        machine->state.store(MState::kParked, std::memory_order_release);
        GO2CPP_DEBUG_EVENT_SAMPLED(
            kWorkerPark, machine->id,
            static_cast<std::uint64_t>(own_processor.id), 8);
        // 锁归属说明（R10）：worker 对自己 M 的状态写入不需要 Impl::mutex；
        // 但 P 的 attached 记账（active_machines）由 worker 创建/退休与
        // sysmon/leave_blocking 在 Impl::mutex 内共同维护。这里的
        // PState::kIdle 只是"本 P 当前没有运行中的 M"的快速观测值，
        // 不参与资源记账决策；不要把它当作授权的依据。
        if (own_processor.running_machines.load(std::memory_order_acquire) ==
            0) {
            own_processor.state.store(PState::kIdle,
                                      std::memory_order_release);
        }
#ifdef GO2CPP_COUNT_GLOBAL_LOCK
        LockStatGuard park_stat(m_impl->mutex);
        std::unique_lock<std::mutex> lock(m_impl->mutex, std::adopt_lock);
#else
        std::unique_lock<std::mutex> lock(m_impl->mutex);
#endif
        if (m_impl->finish_draining_locked()) {
            lock.unlock();
            m_impl->notify_all_workers();
            machine->state.store(MState::kStopping,
                                 std::memory_order_release);
            break;
        }
        // 长 syscall 的原 M 仍计入 active_workers，但已经从 P 的
        // attached 计数中移除。替代 M 在原 M 返回前必须保留，不能被
        // 普通 idle timeout 提前回收。
        const auto required_workers = [this] {
            const auto detached = m_impl->detached_machine_count_locked();
            if (detached == 0) {
                return m_impl->config.min_workers;
            }
            const auto handoff_floor =
                detached > std::numeric_limits<std::size_t>::max() -
                              m_impl->config.min_workers
                    ? std::numeric_limits<std::size_t>::max()
                    : m_impl->config.min_workers + detached;
            return std::max(
                m_impl->config.min_workers,
                std::min(m_impl->config.max_workers, handoff_floor));
        };
        // 基础 M 与补充 M 的回收策略不同：
        // - 补充 M 的使命是接替被原生调用占住的 P。blocking_workers 归零
        //   （阻塞区结束）后它已经无事可做，立即退休 —— 这是 Go 的 M 回收
        //   语义。退休前提是「阻塞已结束」，不会因为新 G 入队而失效。
        // - 基础 M 常驻到 shutdown。只有显式打开 allow_idle_shrink 时才
        //   保留历史的空闲缩容行为（下限 min_workers）。
        const bool replacement = machine->replacement;
        const bool idle_shrink = m_impl->config.allow_idle_shrink;
        const auto worker_floor = required_workers();
        const bool can_shrink =
            idle_shrink &&
            m_impl->active_workers.load(std::memory_order_relaxed) >
                worker_floor;
        // 补充 M 用 idle_worker_timeout 复查「阻塞是否结束」；基础 M 不
        // 用它触发退出，只沿用 idle_wait 的空闲等待节奏。
        const auto idle_timeout = (replacement || can_shrink)
                                      ? m_impl->config.idle_worker_timeout
                                      : m_impl->config.idle_wait;
        const bool woke = m_impl->condition.wait_for(
            lock, idle_timeout, [this, replacement, idle_shrink] {
                if (m_impl->stopping.load(std::memory_order_acquire) ||
                    m_impl->runnable.load(std::memory_order_acquire) != 0) {
                    return true;
                }
                // 阻塞区结束时立刻唤醒补充 M 做退休判定，避免等满一个
                // idle_worker_timeout 才回收。
                return replacement && !idle_shrink &&
                       m_impl->blocking_workers.load(
                           std::memory_order_acquire) == 0;
            });
        // 退休判定与计数修改仍在同一个持锁临界区内：wait_for 返回后锁没有
        // 释放，因此这里的 runnable/blocking 观测即最终状态，不会出现
        // 「判定空闲后、置 kStopping 前」被新 G 插队的窗口。
        const auto current_floor = required_workers();
        const auto runnable_now =
            m_impl->runnable.load(std::memory_order_acquire);
        const auto blocking_now =
            m_impl->blocking_workers.load(std::memory_order_acquire);
        const bool stopping_now =
            m_impl->stopping.load(std::memory_order_acquire);
        bool retire = false;
        if (!stopping_now && runnable_now == 0) {
            if (replacement && !idle_shrink) {
                // 补充 M：阻塞声明归零且 sysmon 解绑计数归零，才说明它不再
                // 有「替代被占住的 P」的职责。
                retire = blocking_now == 0 &&
                         m_impl->detached_machine_count_locked() == 0;
            } else if (can_shrink && !woke) {
                retire = m_impl->active_workers.load(
                             std::memory_order_relaxed) > current_floor;
            }
        }
        if (retire) {
            machine->state.store(MState::kStopping, std::memory_order_release);
            // 持锁减计数：多个补充 M 同时退休时不会一起跌破常驻底线。
            m_impl->active_workers.fetch_sub(1, std::memory_order_relaxed);
            own_processor.active_machines.fetch_sub(1,
                                                    std::memory_order_relaxed);
            retired = true;
            break;
        }
        GO2CPP_DEBUG_EVENT_SAMPLED(
            kWorkerUnpark, machine->id,
            static_cast<std::uint64_t>(woke ? 1 : 3), 8);
        machine->state.store(MState::kIdle, std::memory_order_release);
    }

    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        if (!retired) {
            // sysmon 可能已经从 P 的 attached 计数中移除了这个 M。
            // 退出时只撤销仍然 attached 的计数，避免 size_t 下溢。
            if (!machine->processor_detached.load(std::memory_order_acquire)) {
                own_processor.active_machines.fetch_sub(
                    1, std::memory_order_relaxed);
            }
            m_impl->active_workers.fetch_sub(1, std::memory_order_relaxed);
        }
        if (own_processor.active_machines.load(std::memory_order_relaxed) == 0) {
            own_processor.state.store(
                m_impl->stopping.load(std::memory_order_acquire)
                    ? PState::kDead
                    : PState::kIdle,
                std::memory_order_release);
        }
        machine->state.store(MState::kDead, std::memory_order_release);
    }
    m_impl->emit_event(SchedulerEvent{SchedulerEventType::kWorkerStopped,
                                      0, machine->id, machine->processor});
    GO2CPP_DEBUG_EVENT(kThreadStop, machine->id, machine->processor);
    m_impl->notify_all_workers();
    t_task.reset();
    GO2CPP_DEBUG_UNBIND_TASK();
    Fiber::BindCurrentExecution(FiberExecutionBinding{});
    thread_policy::detail::LeaveRuntimeWorker();
    t_scheduler = nullptr;
    t_machine_id = 0;
    GO2CPP_DEBUG_UNBIND_PROCESSOR();
    t_processor_id = 0;
    t_blocking_machine_id = 0;
}

// ---------------------------------------------------------------------------
// 持锁 park 契约计数器
// ---------------------------------------------------------------------------

std::uint64_t spin_mutex_park_degrade_total() noexcept {
    return g_spin_mutex_park_degrade_total.load(std::memory_order_relaxed);
}

std::uint64_t mutex_held_park_total() noexcept {
    return g_mutex_held_park_total.load(std::memory_order_relaxed);
}

}  // namespace go2cpp::scheduler
