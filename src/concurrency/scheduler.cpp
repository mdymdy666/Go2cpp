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
#include <memory>
#include <mutex>
#include <new>
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

// 热路径 thread_local 统一使用 initial-exec TLS 模型。共享库默认是
// general-dynamic：每次访问都要调用 __tls_get_addr，而 park/wake/yield
// 一次往返要读十几次（callgrind 基线里 __tls_get_addr 占 10.5% 指令）。
// 调度器库在进程启动时随可执行文件加载（不是 dlopen 插件），
// initial-exec 把访问退化成一条 fs 段读。
#if defined(__GNUC__) || defined(__clang__)
#define GO2CPP_SCHED_HOT_TLS __attribute__((tls_model("initial-exec")))
#else
#define GO2CPP_SCHED_HOT_TLS
#endif

thread_local Scheduler* t_scheduler GO2CPP_SCHED_HOT_TLS = nullptr;
thread_local std::shared_ptr<Task> t_task GO2CPP_SCHED_HOT_TLS;
thread_local MId t_machine_id GO2CPP_SCHED_HOT_TLS = 0;
thread_local PId t_processor_id GO2CPP_SCHED_HOT_TLS = 0;
// ---------------------------------------------------------------------------
// 阶段 1 提交侧去共享原子（2026-11-01-scheduler-opt-review.md P2/P3/P4）
// ---------------------------------------------------------------------------
//
// 下面三组状态原本都是「每个任务一次全局原子 RMW」，且只服务启发式判定 /
// 标识分配，没有精确性要求：
//   - P2：next_processor 轮转（投递分摊）；
//   - P3：enqueue_maintenance 维护票（每 64/1024 次做一次维护）；
//   - P4：s_next_g_id（Task 构造时分配 GId）。
// 改成线程局部后，热路径不再触碰共享缓存行，只剩一次 fs 段读。

/// @brief P2：本线程的 P 轮转游标。
thread_local std::size_t t_processor_rr GO2CPP_SCHED_HOT_TLS = 0;
/// @brief P2：游标是否已经从全局种子领过起始偏移。
thread_local bool t_processor_rr_seeded GO2CPP_SCHED_HOT_TLS = false;

/// @brief P2：选择一个 P 编号（0..count-1）。
/// @details 首次调用从 `seed` 领一个起始偏移（每线程一次全局 RMW），之后
///          完全本地自增。多生产者靠不同起始偏移自然错开；单生产者下就是
///          严格轮转。轮转只影响投递分摊的公平性，精确性没有要求——P 的
///          队列满/空闲另有判定，取模的 P 数变化也不影响正确性。
std::size_t pick_processor_round_robin(std::atomic<std::size_t>& seed,
                                       std::size_t count) noexcept {
    if (count == 0) {
        return 0;
    }
    if (!t_processor_rr_seeded) {
        t_processor_rr = seed.fetch_add(1, std::memory_order_relaxed);
        t_processor_rr_seeded = true;
    }
    return t_processor_rr++ % count;
}

/// @brief P3：enqueue 维护票（本地自增，替代每任务一次全局 fetch_add）。
thread_local std::uint64_t t_maintenance_ticket GO2CPP_SCHED_HOT_TLS = 0;

/// @brief P4：GId 分段大小。每段一次全局 RMW，摊到 256 个 Task 构造上。
constexpr GId kGIdSegmentSize = 256;
thread_local GId t_g_id_next GO2CPP_SCHED_HOT_TLS = 0;
thread_local GId t_g_id_end GO2CPP_SCHED_HOT_TLS = 0;

/// @brief P4：分配一个进程内唯一的 GId。
/// @details 全局唯一性保留（段从 s_next_g_id 一次领 256 个，段基单调），
///          但不再保证全局严格连续：同一线程的 ID 段内连续，跨线程可能
///          交错出现「后分配的 ID 小于先分配的 ID」。注册表条带仍均匀：
///          段基数是 256 的倍数，低 4 位在每段内按 0..15 循环。GId 是
///          64 位，正常进程不可能耗尽。
GId allocate_g_id() noexcept {
    if (t_g_id_next == t_g_id_end) {
        const GId base =
            s_next_g_id.fetch_add(kGIdSegmentSize, std::memory_order_relaxed);
        t_g_id_next = base;
        t_g_id_end = base + kGIdSegmentSize;
    }
    return t_g_id_next++;
}

// BlockingRegion is deliberately non-nestable: one M has one state
// transition, and rejecting nested scopes avoids double accounting. The
// machine id is retained so a misuse that lets a Fiber migrate before its
// region is destroyed can still clear the original M's state.
thread_local MId t_blocking_machine_id GO2CPP_SCHED_HOT_TLS = 0;

/// @brief 当前线程是否正处在 worker 的 IO 空闲轮询里。
/// @details poll 内部会处理就绪事件并把 G 投回本 P；投递者随后立刻 `continue`
///          回主循环取任务，因此不需要再唤醒别的 worker。只在
///          `requeue_from_worker`（本 P 投递）里短路，跨 P 投递照常通知。
thread_local bool t_in_idle_poll GO2CPP_SCHED_HOT_TLS = false;

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
thread_local std::uint32_t t_incoming_lock_depth GO2CPP_SCHED_HOT_TLS = 0;

/// @brief 持 incoming 条带锁并登记持锁深度；析构时先解锁再清深度。
class IncomingStripeLock final {
public:
    explicit IncomingStripeLock(detail::HotSpinLock& mutex) : m_mutex(mutex) {
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
    detail::HotSpinLock& m_mutex;
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


/**
 * @brief 每个 P 的本地可运行队列：256 槽有界 MPMC 环形队列。
 *
 * ## 来源
 *
 * 结构照抄 Go 1.3 `runtime/proc.c:2912-3027` 的 runq（Go 1.27 逐行同构）：
 * `head` 由消费者用 CAS 抢占、`tail` 由生产者发布、每个槽用序号标记
 * 可见性；满时 `push()` 返回 false，由调用方溢出到全局队列（对应
 * `runqputslow`），窃取从 `head` 批量取（对应 `runqgrab`）。
 *
 * ## 与 Go 的差异（为什么 tail 也要 CAS）
 *
 * Go 的 runq 是「owner-only 单生产者 + CAS 多消费者」：写 tail 的只有该 P
 * 的 owner M。Bento 的 P 没有唯一 owner（replacement M 可与基础 M 共享
 * 同一个 P，外部唤醒也可能把 G 精确投递到目标 P），因此生产端同样可能
 * 并发，`tail` 用 CAS 仲裁，得到标准的 Vyukov 有界 MPMC 队列。
 *
 * ## 不变量
 *
 * - 槽位在 `sequence == position` 时才可被生产者写入，`sequence ==
 *   position + 1` 时才可被消费者读取；`sequence` 的 release/acquire 同时
 *   承担 `task` 的发布；
 * - 同一节点只会被一个消费者取走（CAS head 的唯一赢家）；
 * - 元素是 `shared_ptr<Task>`：队列节点本身就是注册表之外的第二份强引用，
 *   已取消的残留节点在出队前不会析构。
 */
class LocalRunQueue final {
public:
    static constexpr std::size_t kCapacity = 256;
    static constexpr std::size_t kMask = kCapacity - 1;

    LocalRunQueue() noexcept {
        for (std::size_t i = 0; i < kCapacity; ++i) {
            m_cells[i].sequence.store(i, std::memory_order_relaxed);
        }
    }
    LocalRunQueue(const LocalRunQueue&) = delete;
    LocalRunQueue& operator=(const LocalRunQueue&) = delete;

    /// @brief 入队（左值：队列节点做一次 shared_ptr 拷贝）。
    /// @details P12：形参从 by-value 改成 const 引用。原实现每个任务
    ///          多付「形参拷贝 + 形参析构」两次控制块 RMW；队列节点本来
    ///          就要持有一份强引用，因此这里只剩节点赋值这一次。
    bool push(const std::shared_ptr<Task>& task) noexcept {
        return push_impl(task);
    }

    /// @brief 入队（右值：直接移动，零引用计数操作）。
    bool push(std::shared_ptr<Task>&& task) noexcept {
        return push_impl(std::move(task));
    }

    /// @brief 出队；队列空返回空指针。
    std::shared_ptr<Task> pop() noexcept {
        std::size_t position = m_head.load(std::memory_order_relaxed);
        for (;;) {
            Cell& cell = m_cells[position & kMask];
            const std::size_t sequence =
                cell.sequence.load(std::memory_order_acquire);
            const auto difference =
                static_cast<std::int64_t>(sequence) -
                static_cast<std::int64_t>(position + 1);
            if (difference == 0) {
                if (m_head.compare_exchange_weak(position, position + 1,
                                                 std::memory_order_relaxed,
                                                 std::memory_order_relaxed)) {
                    auto task = std::move(cell.task);
                    cell.sequence.store(position + kMask + 1,
                                        std::memory_order_release);
                    return task;
                }
            } else if (difference < 0) {
                return {};
            } else {
                position = m_head.load(std::memory_order_relaxed);
            }
        }
    }

    /// @brief 近似长度；只在快照/超限判定里使用，不参与正确性。
    std::size_t size() const noexcept {
        const auto head = m_head.load(std::memory_order_acquire);
        const auto tail = m_tail.load(std::memory_order_acquire);
        return tail >= head ? static_cast<std::size_t>(tail - head) : 0U;
    }

    bool empty() const noexcept { return size() == 0U; }

    /// @brief 批量窃取（Go runqgrab 的一次偷一半）；返回实际取到的个数。
    std::size_t steal(std::shared_ptr<Task>* out, std::size_t max) noexcept {
        std::size_t count = 0;
        while (count < max) {
            auto task = pop();
            if (!task) {
                break;
            }
            out[count++] = std::move(task);
        }
        return count;
    }

private:
    /// @brief push 的公共实现：左值拷贝一次，右值移动。
    template <typename TaskPtr>
    bool push_impl(TaskPtr&& task) noexcept {
        if (!task) {
            return false;
        }
        std::size_t position = m_tail.load(std::memory_order_relaxed);
        for (;;) {
            Cell& cell = m_cells[position & kMask];
            const std::size_t sequence =
                cell.sequence.load(std::memory_order_acquire);
            const auto difference =
                static_cast<std::int64_t>(sequence) -
                static_cast<std::int64_t>(position);
            if (difference == 0) {
                if (m_tail.compare_exchange_weak(position, position + 1,
                                                 std::memory_order_relaxed,
                                                 std::memory_order_relaxed)) {
                    cell.task = std::forward<TaskPtr>(task);
                    cell.sequence.store(position + 1,
                                        std::memory_order_release);
                    return true;
                }
            } else if (difference < 0) {
                return false;  // 满
            } else {
                position = m_tail.load(std::memory_order_relaxed);
            }
        }
    }

    struct Cell {
        std::atomic<std::size_t> sequence{0};
        std::shared_ptr<Task> task;
    };

    // 槽位数组与两个游标分开缓存行，避免生产者/消费者互相拖拽同一行。
    alignas(64) std::array<Cell, kCapacity> m_cells{};
    alignas(64) std::atomic<std::size_t> m_head{0};
    alignas(64) std::atomic<std::size_t> m_tail{0};
};

/**
 * @brief 单槽快速队列（Go `runnext` / 定向邮箱的等价物）。
 *
 * ## 为什么是单槽
 *
 * Go 1.3 `runqput(..., next=true)` 把刚唤醒/刚 yield 的 G 放进 `p.runnext`
 * 单槽：下一次 `runqget` 直接取它，不经过 256 槽环，也不会被别的 P 在
 * 队列中间抢走。单槽 LIFO 的语义比「放队首」更简单：最新被唤醒的 G
 * 最可能马上可运行（生产者刚发布完），放最前面命中率最高。
 *
 * ## 并发
 *
 * 生产端可能是 owner M 自己（yield 回队）或外部唤醒者（跨线程投递）；
 * 消费端是本 P 的 worker 或（窃取路径上的）空闲 worker。项目面向
 * C++17，没有 `std::atomic<std::shared_ptr>`，因此用项目内的 TTAS 自旋锁
 * 保护单槽：无争用时就是一次 exchange + 两次指针搬运，比 256 槽环的
 * CAS 更短。
 *
 * ## 与 256 槽环的关系
 *
 * 槽满时把旧任务交给调用方放回环尾（对应 Go `runqputslow` 的下一层），
 * 因此 runnext 不移除任何既有队列容量，只改变「最近一次唤醒」的优先级。
 */
class RunNextSlot final {
public:
    RunNextSlot() noexcept = default;
    RunNextSlot(const RunNextSlot&) = delete;
    RunNextSlot& operator=(const RunNextSlot&) = delete;

    /// @brief 放入槽（左值：槽内做一次 shared_ptr 拷贝）。
    bool push(const std::shared_ptr<Task>& task,
              std::shared_ptr<Task>& displaced) noexcept {
        return push_impl(task, displaced);
    }

    /// @brief 放入槽（右值：直接移动，零引用计数操作）。
    bool push(std::shared_ptr<Task>&& task,
              std::shared_ptr<Task>& displaced) noexcept {
        return push_impl(std::move(task), displaced);
    }

    /// @brief 取走槽内任务；空槽返回空指针。
    std::shared_ptr<Task> take() noexcept {
        std::lock_guard<detail::HotSpinLock> lock(m_mutex);
        return std::move(m_task);
    }

    /// @brief 槽是否为空（诊断/快照用，非同步判定）。
    bool empty() const noexcept {
        std::lock_guard<detail::HotSpinLock> lock(m_mutex);
        return m_task == nullptr;
    }

private:
    template <typename TaskPtr>
    bool push_impl(TaskPtr&& task,
                   std::shared_ptr<Task>& displaced) noexcept {
        if (!task) {
            return false;
        }
        std::lock_guard<detail::HotSpinLock> lock(m_mutex);
        displaced = std::move(m_task);
        m_task = std::forward<TaskPtr>(task);
        return true;
    }

    mutable detail::HotSpinLock m_mutex;
    std::shared_ptr<Task> m_task;
};

// ---------------------------------------------------------------------------
// Task 共享块分配池（Task 对象 + shared_ptr 控制块的那一次整块分配）
// ---------------------------------------------------------------------------
//
// spawn() 用 `std::allocate_shared<Task>` 一次分配「Task 对象 + 控制块」，
// 大小 704B 且要求 64 字节对齐（Task 含 alignas(64) 的完成原语）。glibc 对
// align>16 的请求走 posix_memalign → _mid_memalign，释放走 free 的
// merge/consolidate 慢路径；callgrind 在 4P/4M 空任务 churn 上测得
// 「分配 + 释放」合计约 17% 指令，其中对齐分配约 9%。
//
// 池只复用**内存**：allocate_shared 的销毁器仍在引用归零时正常调用
// `~Task()`（用户回调、异常、Fiber 句柄都在那里释放），再把整块内存交回
// 池。因此不存在「对象被复用却没有重置」的风险，也不需要改动 Task 的
// 构造/析构语义。
//
// 结构（单 size class，因为同一进程里只有 Task 走这条路径）：
//   - 每线程固定数组缓存：无锁、无分配，容量有界（96 块 ≈ 66KiB/线程）；
//   - 缓存空/满时与进程级 free-list 批量交换（一次 48 块，只锁一次）；
//   - 新内存按 64 块一个 chunk 向系统申请，chunk 记录在池里，池析构时释放。
class TaskBlockPool final {
public:
    static constexpr std::size_t kBlockAlign = 64;
    static constexpr std::size_t kMaxPooledBlock = 4096;
    static constexpr std::size_t kThreadCacheCapacity = 96;

    static TaskBlockPool& Instance() noexcept {
        static TaskBlockPool pool;
        return pool;
    }

    TaskBlockPool(const TaskBlockPool&) = delete;
    TaskBlockPool& operator=(const TaskBlockPool&) = delete;

    /// @brief 记录块大小（由分配器在首次分配前设置）。
    /// @return true 表示尺寸与既有 size class 一致，可以走池。
    /// @note 快路径只读一个原子：这条路径在每个任务的分配/释放上都要
    ///       走一次，早期实现在这里加了 std::mutex，单次分配白付一对
    ///       futex 原子操作。只有首次确定 size class 时才需要锁。
    bool SetBlockSize(std::size_t bytes) noexcept {
        const std::size_t block = RoundUp(bytes);
        if (block == 0 || block > kMaxPooledBlock) {
            return false;
        }
        std::size_t current = m_block_size.load(std::memory_order_acquire);
        if (current == 0) {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_block_size.load(std::memory_order_relaxed) == 0) {
                m_block_size.store(block, std::memory_order_release);
            }
            current = m_block_size.load(std::memory_order_acquire);
        }
        return current == block;
    }

    /// @brief 取一块不少于 bytes 字节、64 字节对齐的内存。
    /// @return nullptr 表示该尺寸/对齐超出池能力，调用方必须走系统分配。
    void* Acquire(std::size_t bytes, std::size_t align) noexcept {
        if (align > kBlockAlign || !SetBlockSize(bytes)) {
            return nullptr;
        }
        ThreadCache& cache = t_task_block_cache;
        if (cache.count != 0) {
            return cache.blocks[--cache.count];
        }
        Refill(cache);
        if (cache.count != 0) {
            return cache.blocks[--cache.count];
        }
        return nullptr;
    }

    /// @brief 归还一块由 Acquire() 取出的内存（原地析构已完成）。
    void Release(void* block, std::size_t bytes) noexcept {
        if (block == nullptr) {
            return;
        }
        const std::size_t rounded = RoundUp(bytes);
        if (rounded == 0 || rounded > kMaxPooledBlock) {
            return;
        }
        ThreadCache& cache = t_task_block_cache;
        if (cache.count == kThreadCacheCapacity) {
            Recycle(cache);
        }
        if (cache.count < kThreadCacheCapacity) {
            cache.blocks[cache.count++] = block;
        }
    }

private:
    static constexpr std::size_t kGlobalBatch = 48;
    static constexpr std::size_t kChunkBlocks = 64;

    struct ThreadCache {
        void* blocks[kThreadCacheCapacity];
        std::size_t count;
    };

    static std::size_t RoundUp(std::size_t bytes) noexcept {
        return (bytes + kBlockAlign - 1U) & ~(kBlockAlign - 1U);
    }

    TaskBlockPool() noexcept = default;

    ~TaskBlockPool() {
        for (void* chunk : m_chunks) {
            ::operator delete(chunk, std::align_val_t(kBlockAlign));
        }
    }

    /// @brief 从全局 free-list 批量补充本地缓存；不足时申请新 chunk。
    void Refill(ThreadCache& cache) noexcept {
        const std::size_t block_size =
            m_block_size.load(std::memory_order_acquire);
        if (block_size == 0) {
            return;
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        while (cache.count < kGlobalBatch && !m_free.empty()) {
            cache.blocks[cache.count++] = m_free.back();
            m_free.pop_back();
        }
        if (cache.count >= kGlobalBatch || m_exhausted) {
            return;
        }
        void* chunk = nullptr;
        try {
            chunk = ::operator new(block_size * kChunkBlocks,
                                   std::align_val_t(kBlockAlign));
            m_chunks.push_back(chunk);
        } catch (...) {
            // 池只是分配优化；失败时让调用方回退到系统对齐分配。
            m_exhausted = true;
            if (chunk != nullptr) {
                ::operator delete(chunk, std::align_val_t(kBlockAlign));
            }
            return;
        }
        auto* base = static_cast<unsigned char*>(chunk);
        for (std::size_t i = 0; i < kChunkBlocks; ++i) {
            cache.blocks[cache.count++] = base + i * block_size;
        }
    }

    /// @brief 把本地缓存的一半批量还回全局 free-list。
    void Recycle(ThreadCache& cache) noexcept {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::size_t half = cache.count / 2U;
        while (half != 0) {
            try {
                m_free.push_back(cache.blocks[--cache.count]);
            } catch (...) {
                ++cache.count;
                return;
            }
            --half;
        }
    }

    // TLS 缓存：POD（无构造/析构），不产生 TLS guard。
    // 用 initial-exec 模型：共享库默认 general-dynamic 时每次访问都要
    // 调 __tls_get_addr（30~50 cycles）。这里的 TLS 在每条任务分配/
    // 释放路径上都要读一次，必须与调度器其它热 TLS 用同一模型。
    static thread_local ThreadCache t_task_block_cache GO2CPP_SCHED_HOT_TLS;

    std::mutex m_mutex;
    std::vector<void*> m_free;
    std::vector<void*> m_chunks;
    std::atomic<std::size_t> m_block_size{0};
    bool m_exhausted{false};
};

thread_local TaskBlockPool::ThreadCache TaskBlockPool::t_task_block_cache GO2CPP_SCHED_HOT_TLS{};

/// @brief allocate_shared 专用分配器：Task 控制块走池，其它回退系统分配。
template <typename T>
class TaskBlockAllocator {
public:
    using value_type = T;

    TaskBlockAllocator() noexcept = default;
    template <typename U>
    TaskBlockAllocator(const TaskBlockAllocator<U>&) noexcept {}

    T* allocate(std::size_t n) {
        const std::size_t bytes = n * sizeof(T);
        // allocate 与 deallocate 的判定条件必须逐字对称：若某次分配走了
        // 系统路径，释放时必须也走系统 delete，否则池会「收养」一块永远
        // 不会被回收的外来内存。
        if (n == 1U && bytes <= TaskBlockPool::kMaxPooledBlock &&
            alignof(T) <= TaskBlockPool::kBlockAlign) {
            if (void* block = TaskBlockPool::Instance().Acquire(bytes,
                                                                alignof(T))) {
                return static_cast<T*>(block);
            }
        }
        return static_cast<T*>(::operator new(bytes,
                                             std::align_val_t(alignof(T))));
    }

    void deallocate(T* p, std::size_t n) noexcept {
        if (p == nullptr) {
            return;
        }
        const std::size_t bytes = n * sizeof(T);
        if (n == 1U && bytes <= TaskBlockPool::kMaxPooledBlock &&
            alignof(T) <= TaskBlockPool::kBlockAlign) {
            TaskBlockPool::Instance().Release(p, bytes);
            return;
        }
        ::operator delete(p, bytes, std::align_val_t(alignof(T)));
    }

    template <typename U>
    bool operator==(const TaskBlockAllocator<U>&) const noexcept {
        return true;
    }
    template <typename U>
    bool operator!=(const TaskBlockAllocator<U>&) const noexcept {
        return false;
    }
};



/// @brief CPU 自旋提示（x86 `pause` / ARM `yield`）。
/// @details 空闲自旋等待新工作时用它替代裸空转：pause 让 HT 兄弟核拿到
///          执行端口，同时避免空转循环把分支预测和流水线打满。
inline void CpuRelax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    asm volatile("yield" ::: "memory");
#else
    std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
}

/// @brief 空闲 worker 在进入条件变量等待前的自旋轮数。
/// @details 每轮只读 runnable/stopping 两个原子 + 一条 pause（约 10~15ns），
///          512 轮 ≈ 5~7µs。持续空闲时每个 idle_wait 周期只烧这么多 CPU，
///          可以忽略；而短任务 churn 下它让 worker 不必为每个 G 付一次
///          futex 睡眠/唤醒往返（1~3µs）。
constexpr std::size_t kIdleSpinRounds = 512;


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
    // P4：GId 来自线程本地段（一次全局 RMW 摊到 256 个 Task 构造），
    // 不再每个任务对 s_next_g_id 做一次共享 fetch_add。
    : m_id(allocate_g_id()),
      m_function(std::move(function)),
      m_options(options),
      m_state_word(static_cast<std::uint32_t>(GState::kNew)) {}

Task::~Task() = default;


// ---------------------------------------------------------------------------
// 打包状态字（P0-1）
// ---------------------------------------------------------------------------
//
// 旧实现用 `m_transition_mutex` 把「状态迁移」和「唤醒令牌」放在同一个
// 临界区里；两个字段拆成独立原子后，park 与 wake 之间存在不可消除的
// 窗口（wake 读到 Running → park 提交 Waiting → wake 置令牌，令牌永远
// 无人消费）。这里把两者压进同一个 32 位字，所有迁移都用 CAS 竞争这个
// 字，令牌的发布/消费与状态迁移由同一条原子指令线性化。
void Task::StoreStateValue(GState state, std::memory_order order) noexcept {
    std::uint32_t word = m_state_word.load(std::memory_order_relaxed);
    for (;;) {
        const std::uint32_t desired =
            (word & ~kStateMask) | static_cast<std::uint32_t>(state);
        if (m_state_word.compare_exchange_weak(word, desired, order,
                                               std::memory_order_relaxed)) {
            return;
        }
    }
}

void Task::StoreWakePending(bool pending,
                            std::memory_order order) noexcept {
    std::uint32_t word = m_state_word.load(std::memory_order_relaxed);
    for (;;) {
        const std::uint32_t desired =
            pending ? (word | kWakePendingBit) : (word & ~kWakePendingBit);
        if (m_state_word.compare_exchange_weak(word, desired, order,
                                               std::memory_order_relaxed)) {
            return;
        }
    }
}

bool Task::ExchangeWakePending(bool pending,
                               std::memory_order order) noexcept {
    std::uint32_t word = m_state_word.load(std::memory_order_relaxed);
    for (;;) {
        const bool previous = (word & kWakePendingBit) != 0U;
        const std::uint32_t desired =
            pending ? (word | kWakePendingBit) : (word & ~kWakePendingBit);
        if (m_state_word.compare_exchange_weak(word, desired, order,
                                               std::memory_order_relaxed)) {
            return previous;
        }
    }
}

Task::Function Task::release_callable() noexcept {
    std::lock_guard<detail::HotSpinLock> lock(m_transition_mutex);
    return std::move(m_function);
}

GId Task::id() const noexcept {
    return m_id;
}

GState Task::state() const noexcept {
    return StateValue(std::memory_order_acquire);
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

bool Task::lock_to_current_m() noexcept {
    // 只允许在 G 自己的执行流里绑定：别的线程无法知道它「当前」在哪个 M。
    if (Scheduler::current_task_ptr() != this) {
        return false;
    }
    const MId machine = Scheduler::current_machine_id();
    if (machine == 0) {
        return false;
    }
    m_locked_machine.store(machine, std::memory_order_release);
    // 同步到 Fiber 记录：run() 返回时以 Fiber 侧为准同步回 Task，双写
    // 避免「Task 绑定后下一次 run 被 Fiber 的旧值覆盖」。
    if (auto* fiber = m_fiber_ptr.load(std::memory_order_acquire)) {
        (void)fiber->lock_to_machine(machine);
    }
    return true;
}

bool Task::unlock_from_m() noexcept {
    m_locked_machine.store(0, std::memory_order_release);
    if (auto* fiber = m_fiber_ptr.load(std::memory_order_acquire)) {
        (void)fiber->lock_to_machine(0);
    }
    return true;
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
    // 自等待检测只需要指针比较：current_task() 会复制一份 shared_ptr，
    // 在 join 热路径上白付两次控制块原子操作。
    if (Scheduler::current_task_ptr() == this) {
        return false;
    }
    // 完成快路径：join 绝大多数发生在 G 已进入终态之后，此时不需要
    // 触碰完成互斥量与等待原语。
    if (terminal()) {
        return true;
    }
    // 谓词只看终态：GState 的终态不可逆，序号只用于让「通知先于登记」
    // 的窗口可见，而终态本身已经承担了同一职责（notify_terminal 在
    // 状态发布之后才读等待者数，见那里的 Dekker 论证）。
    std::unique_lock<std::mutex> lock(m_completion_mutex);
    m_completion_waiters.fetch_add(1, std::memory_order_seq_cst);
    m_completion_condition.wait(lock, [this] { return terminal(); });
    m_completion_waiters.fetch_sub(1, std::memory_order_seq_cst);
    return true;
}

bool Task::wait_for(std::chrono::steady_clock::duration timeout) const {
    if (Scheduler::current_task_ptr() == this) {
        return false;
    }
    if (terminal()) {
        return true;
    }
    std::unique_lock<std::mutex> lock(m_completion_mutex);
    m_completion_waiters.fetch_add(1, std::memory_order_seq_cst);
    const bool done = m_completion_condition.wait_for(
        lock, timeout, [this] { return terminal(); });
    m_completion_waiters.fetch_sub(1, std::memory_order_seq_cst);
    return done;
}

void Task::notify_terminal() noexcept {
    if (m_completion_notified.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    // 无等待者时零开销：绝大多数 G 从来没人 join，旧实现在这里无条件做
    // 一次 seq_cst fetch_add（x86 上是 lock xadd，~20-40 cycles）。
    //
    // 正确性（Dekker）：本函数只在状态已以 release 发布为终态之后调用。
    // notifier: [S] store(terminal, release) → [L] load(waiters, seq_cst)；
    // waiter:   [P] load(terminal, acquire) → [A] lock → [W] waiters++
    //           (seq_cst) → [R] 谓词里 load(terminal, acquire)。
    // L 与 W 都是 seq_cst，构成全序：
    //  - 若 L 在 W 之前（读到 0）：W 在 S 之后，R 在 W 之后，故 R 必看到
    //    终态，waiter 不睡；
    //  - 若 W 在 L 之前：L 看到等待者，走加锁 + notify_all，而 waiter 的
    //    Arm 在持锁状态下完成，通知不会落在 Arm 之前。
    // 两条路径都不会丢唤醒，因此序号自增可以只在「确实有人等」时做。
    if (m_completion_waiters.load(std::memory_order_seq_cst) == 0) {
        return;
    }
    m_completion_seq.fetch_add(1, std::memory_order_seq_cst);
    // 有等待者时必须持锁再通知：等待方在持锁状态下完成「登记 + 谓词复查」，
    // 拿到这把锁才能保证通知发生在登记之后。
    {
        std::lock_guard<std::mutex> lock(m_completion_mutex);
    }
    m_completion_condition.notify_all();
}

bool Task::try_mark_queued() noexcept {
    std::lock_guard<detail::HotSpinLock> lock(m_transition_mutex);
    if (StateValue(std::memory_order_relaxed) != GState::kRunnable ||
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
    std::lock_guard<detail::HotSpinLock> lock(m_transition_mutex);
    if (StateValue(std::memory_order_relaxed) != GState::kRunnable ||
        m_execution_claim.load(std::memory_order_relaxed) ||
        m_queued.load(std::memory_order_relaxed)) {
        return false;
    }
    m_queued.store(true, std::memory_order_release);
    return true;
}

void Task::clear_queued() noexcept {
    std::lock_guard<detail::HotSpinLock> lock(m_transition_mutex);
    m_queued.store(false, std::memory_order_release);
}

void Task::defer_enqueue() noexcept {
    m_deferred_enqueue.store(true, std::memory_order_release);
}

bool Task::consume_deferred_enqueue() noexcept {
    return m_deferred_enqueue.exchange(false, std::memory_order_acq_rel);
}

void Task::request_wake() noexcept {
    std::lock_guard<detail::HotSpinLock> lock(m_transition_mutex);
    StoreWakePending(true, std::memory_order_release);
}

bool Task::consume_wake() noexcept {
    std::lock_guard<detail::HotSpinLock> lock(m_transition_mutex);
    return ExchangeWakePending(false, std::memory_order_acq_rel);
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
    std::lock_guard<detail::HotSpinLock> lock(m_transition_mutex);
    if (m_owner_anchor == nullptr) {
        m_owner_anchor = owner.get();
        return true;
    }
    return m_owner_anchor == owner.get();
}

bool Task::owned_by(const std::shared_ptr<const void>& owner) const noexcept {
    if (!owner) {
        return false;
    }
    std::lock_guard<detail::HotSpinLock> lock(m_transition_mutex);
    return m_owner_anchor != nullptr && m_owner_anchor == owner.get();
}

void Task::bind_cancellation_gate(
    const std::shared_ptr<TaskCancellationGate>& gate) noexcept {
    std::lock_guard<detail::HotSpinLock> lock(m_transition_mutex);
    if (!m_gate_bound) {
        m_cancellation_gate = gate;
        m_gate_bound = true;
    }
}

bool Task::prepare_enqueue(
    const std::shared_ptr<const void>& owner,
    const std::shared_ptr<TaskCancellationGate>& gate,
    std::atomic<std::size_t>* live_tasks) noexcept {
    if (!owner || !gate) {
        return false;
    }
    std::lock_guard<detail::HotSpinLock> lock(m_transition_mutex);
    if (m_owner_anchor == nullptr) {
        m_owner_anchor = owner.get();
    } else if (m_owner_anchor != owner.get()) {
        return false;
    }
    if (!m_gate_bound) {
        m_cancellation_gate = gate;
        m_gate_bound = true;
    }
    const auto state = StateValue(std::memory_order_relaxed);
    if (state == GState::kNew) {
        StoreStateValue(GState::kRunnable, std::memory_order_release);
    } else if (state != GState::kRunnable) {
        return false;
    }
    if (m_execution_claim.load(std::memory_order_relaxed) ||
        m_queued.load(std::memory_order_relaxed)) {
        return false;
    }
    m_queued.store(true, std::memory_order_release);
    // 一个 G 从首次入队成功到进入终态是一个连续的非终态区间：在这里
    // 计入存活数，终态转移时再递减。draining 只需比较这个计数是否归零，
    // 不再为每个完成的 G 扫描整张注册表（旧实现的 O(n²) 关停）。
    bind_live_counter_locked(live_tasks);
    return true;
}

void Task::bind_live_counter_locked(
    std::atomic<std::size_t>* counter) noexcept {
    if (counter == nullptr || m_live_counted) {
        return;
    }
    m_live_tasks = counter;
    m_live_counted = true;
    counter->fetch_add(1, std::memory_order_release);
}

void Task::release_live_counter_locked() noexcept {
    if (!m_live_counted) {
        return;
    }
    m_live_counted = false;
    auto* counter = m_live_tasks;
    m_live_tasks = nullptr;
    if (counter != nullptr) {
        counter->fetch_sub(1, std::memory_order_acq_rel);
    }
}

bool Task::promote_new() noexcept {
    std::lock_guard<detail::HotSpinLock> lock(m_transition_mutex);
    if (StateValue(std::memory_order_relaxed) != GState::kNew) {
        return false;
    }
    StoreStateValue(GState::kRunnable, std::memory_order_release);
    return true;
}

WakeAction Task::wake_for_scheduler() noexcept {
    std::lock_guard<detail::HotSpinLock> lock(m_transition_mutex);
    const auto state = StateValue(std::memory_order_relaxed);
    if (state == GState::kWaiting) {
        StoreWakePending(false, std::memory_order_relaxed);
        StoreStateValue(GState::kRunnable, std::memory_order_release);
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
        StoreWakePending(true, std::memory_order_release);
        return WakeAction::kPending;
    }
    return WakeAction::kRejected;
}

WakeAction Task::wake_for_io() noexcept {
    return wake_for_wait();
}

WakeAction Task::wake_for_wait() noexcept {
    return wake_for_wait_impl(false);
}

WakeAction Task::wake_for_wait_reserve() noexcept {
    return wake_for_wait_impl(true);
}

WakeAction Task::wake_for_wait_impl(bool reserve_queue) noexcept {
    // 无锁 CAS 循环：状态与令牌在同一个 32 位字里，wake 与 park 各自用
    // CAS 竞争同一个字，不会出现「令牌已发布但挂起已提交」的窗口。
    for (;;) {
        std::uint32_t word = m_state_word.load(std::memory_order_acquire);
        const GState state = StateOfWord(word);
        if (state == GState::kWaiting) {
            // 目标字不带令牌位：转 Runnable 的同时清掉旧令牌。
            if (!m_state_word.compare_exchange_strong(
                    word, static_cast<std::uint32_t>(GState::kRunnable),
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                continue;
            }
            if (m_execution_claim.load(std::memory_order_acquire)) {
                m_deferred_enqueue.store(true, std::memory_order_release);
                return WakeAction::kRegistered;
            }
            if (reserve_queue) {
                // 状态 CAS 已是唯一线性化点；第二个唤醒者读到 Runnable
                // 会返回 kRejected，不会重复占位。
                m_queued.store(true, std::memory_order_release);
            }
            return WakeAction::kEnqueue;
        }
        if (state == GState::kRunning) {
            if (!m_state_word.compare_exchange_strong(
                    word, word | kWakePendingBit,
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                continue;
            }
            return WakeAction::kPending;
        }
        return WakeAction::kRejected;
    }
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
 * step2. **无锁 CAS**：状态与唤醒令牌打包在同一个 32 位字里，不需要锁；
 * step3. 校验状态：不是 `kRunning` 就返回 `kRejected`（已被取消/已死）；
 * step4. 字的 bit8 为真说明挂起前已经有人唤醒过：清令牌并返回 `kRequeued`，
 *        让调用方留在当前执行流；
 * step5. 整个字（Running 且无令牌）匹配时 CAS 成 `kWaiting` 并返回 `kParked`；
 *        CAS 失败说明并发的 wake 改了字，重读后按新状态处理。
 *
 * ## 并发/失败语义
 *
 * - **契约检查必须在 step2 之前**：一旦返回 `kParked`，当前 G 就离开了执行流，
 *   再也无法可靠地定位「谁在持锁状态下 park」；
 * - 契约检查只在该 G 确实是当前 G 时执行——`park_for_scheduler` 是公开接口，
 *   外部线程误调时不应把别的 G 的计数算到这次调用头上；
 * - `noexcept`：本函数不分配、不加可能抛出的锁，失败只能通过返回值表达；
 * - **不变量**：返回 `kParked` 后状态字 == `kWaiting` 且令牌位为 0；
 *   返回 `kRequeued` 后状态字仍为 `kRunning` 且令牌位已被清。
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
    // step2: 无锁提交。状态与唤醒令牌打包在同一个 32 位字里：
    //        「转 Waiting」的 CAS 要求整个字（Running 且无令牌）都匹配。
    //        并发 wake 只有两种可能——先把令牌位置位（本 CAS 失败，重试后
    //        消费令牌返回 kRequeued），或在本 CAS 之后看到 Waiting 并负责
    //        把 G 转回 Runnable；不存在「令牌落地但没人消费」的窗口。
    for (;;) {
        const std::uint32_t word =
            m_state_word.load(std::memory_order_acquire);
        // step3: 只有 kRunning 才能 park；否则说明已被取消或已终态。
        if (StateOfWord(word) != GState::kRunning) {
            return ParkAction::kRejected;
        }
        // step4: 挂起前已收到唤醒 → 清令牌并留在当前执行流（kRequeued）。
        if ((word & kWakePendingBit) != 0U) {
            std::uint32_t expected = word;
            if (m_state_word.compare_exchange_strong(
                    expected, word & ~kWakePendingBit,
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                return ParkAction::kRequeued;
            }
            continue;
        }
        // step5: 提交挂起（目标字不含令牌位）。
        std::uint32_t expected = word;
        if (m_state_word.compare_exchange_strong(
                expected, static_cast<std::uint32_t>(GState::kWaiting),
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            return ParkAction::kParked;
        }
    }
}


bool Task::try_mark_running() {
    std::lock_guard<detail::HotSpinLock> lock(m_transition_mutex);
    if (!m_queued.load(std::memory_order_relaxed) ||
        m_execution_claim.load(std::memory_order_relaxed) ||
        StateValue(std::memory_order_relaxed) != GState::kRunnable) {
        return false;
    }
    // Dequeue reservation and the runnable -> running transition are one
    // indivisible Task operation. An external enqueue cannot republish the G
    // between these updates.
    m_queued.store(false, std::memory_order_relaxed);
    m_execution_claim.store(true, std::memory_order_relaxed);
    StoreStateValue(GState::kRunning, std::memory_order_release);
    return true;
}


bool Task::claim_from_queue() noexcept {
    // dequeue 即 claim：队列节点由 pop 者独占，因此不需要再取转换锁。
    // 顺序很重要——先抢 execution_claim（此后外部 try_mark_queued 会因
    // claim 为 true 而拒绝），再清 queued，最后推进状态；三步都用原子
    // 操作，任意并发方看到的都是「已有执行者」或「尚未出队」之一。
    // 只做一次 CAS：Runnable -> Running 本身就是执行权的唯一仲裁点。
    // 在 CAS 成功之前 m_queued 仍为 true，外部 try_mark_queued 会被
    // 「已排队」拒绝；CAS 成功后再发布 execution_claim 并清 queued，
    // 这个窗口里外部看到的仍是「已排队 + Running」，不会重复入队。
    // （先抢 claim 再改状态要多付一次 RMW，实测在 1P2M 的 CV 往返上
    //   每次切换多 ~8ns，因此合并成单 CAS。）
    for (;;) {
        std::uint32_t word = m_state_word.load(std::memory_order_acquire);
        if (StateOfWord(word) != GState::kRunnable) {
            return false;
        }
        if (m_state_word.compare_exchange_strong(
                word, static_cast<std::uint32_t>(GState::kRunning),
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            m_queued.store(false, std::memory_order_release);
            m_execution_claim.store(true, std::memory_order_release);
            return true;
        }
    }
}

bool Task::mark_runnable() {
    std::lock_guard<detail::HotSpinLock> lock(m_transition_mutex);
    if (StateValue(std::memory_order_relaxed) != GState::kWaiting) {
        return false;
    }
    StoreStateValue(GState::kRunnable, std::memory_order_release);
    StoreWakePending(false, std::memory_order_relaxed);
    return true;
}

bool Task::mark_yielded() {
    std::lock_guard<detail::HotSpinLock> lock(m_transition_mutex);
    if (StateValue(std::memory_order_relaxed) != GState::kRunning) {
        return false;
    }
    // A wake received while the G was running has been superseded by the
    // explicit yield: the G is already being requeued and must not consume
    // that old token on a later park.
    StoreStateValue(GState::kRunnable, std::memory_order_release);
    StoreWakePending(false, std::memory_order_release);
    return true;
}

bool Task::mark_yielded_fast() noexcept {
    for (;;) {
        std::uint32_t word = m_state_word.load(std::memory_order_acquire);
        if (StateOfWord(word) != GState::kRunning) {
            return false;
        }
        // yield 明确放弃旧令牌：G 已经在回队，不应在下次 park 消费它。
        if (m_state_word.compare_exchange_strong(
                word, static_cast<std::uint32_t>(GState::kRunnable),
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            return true;
        }
    }
}

bool Task::mark_waiting() {
    std::lock_guard<detail::HotSpinLock> lock(m_transition_mutex);
    if (StateValue(std::memory_order_relaxed) != GState::kRunning) {
        return false;
    }
    StoreStateValue(GState::kWaiting, std::memory_order_release);
    return true;
}

bool Task::cancel() {
    std::shared_ptr<TaskCancellationGate> gate;
    {
        std::lock_guard<detail::HotSpinLock> lock(m_transition_mutex);
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
        std::lock_guard<detail::HotSpinLock> lock(m_transition_mutex);
        const auto state = StateValue(std::memory_order_relaxed);
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
            StoreStateValue(GState::kCancelled, std::memory_order_release);
            m_queued.store(false, std::memory_order_release);
            release_live_counter_locked();
            became_terminal = true;
        }
        StoreWakePending(false, std::memory_order_release);
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
    std::lock_guard<detail::HotSpinLock> lock(m_transition_mutex);
    const GState state = StateValue(std::memory_order_relaxed);
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
    StoreStateValue(GState::kRunning, std::memory_order_release);
    m_deferred_enqueue.store(false, std::memory_order_relaxed);
    StoreWakePending(false, std::memory_order_relaxed);
    return true;
}

bool Task::recover_orphan_running() noexcept {
    // 热路径快检：run() 正常返回后状态是 kWaiting（park）或 kRunnable
    // （yield），既不是 kRunning 也就无需进入临界区。只有真实出现
    // 「run 已返回但仍停在 kRunning」的异常窗口才需要加锁复核。
    if (StateValue(std::memory_order_acquire) != GState::kRunning) {
        return false;
    }
    std::lock_guard<detail::HotSpinLock> lock(m_transition_mutex);
    if (StateValue(std::memory_order_relaxed) != GState::kRunning) {
        return false;
    }
    if (m_execution_claim.load(std::memory_order_relaxed)) {
        // 仍有执行者持有执行权：这不是孤儿，不能抢。
        return false;
    }
    StoreStateValue(GState::kRunnable, std::memory_order_release);
    // 孤儿 Running 可能残留一个「运行期间收到」的唤醒令牌；G 重新入队
    // 后不应在下次 park 消费它，否则会把一次普通挂起误判成已唤醒。
    StoreWakePending(false, std::memory_order_release);
    m_deferred_enqueue.store(true, std::memory_order_release);
    return true;
}

bool Task::cancel_if_runnable_unqueued() noexcept {
    {
        std::lock_guard<detail::HotSpinLock> lock(m_transition_mutex);
        if (StateValue(std::memory_order_relaxed) != GState::kRunnable ||
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
        StoreStateValue(GState::kCancelled, std::memory_order_release);
        StoreWakePending(false, std::memory_order_relaxed);
        m_deferred_enqueue.store(false, std::memory_order_relaxed);
        release_live_counter_locked();
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
        std::lock_guard<detail::HotSpinLock> lock(m_transition_mutex);
        if (StateValue(std::memory_order_relaxed) == GState::kRunning) {
            StoreStateValue(GState::kRunnable, std::memory_order_release);
            m_deferred_enqueue.store(true, std::memory_order_release);
        }
        m_execution_claim.store(false, std::memory_order_release);
        return;
    }

    // 热路径：已发布的 Fiber 指针不再取 m_transition_mutex。写侧
    // （首次创建、终态回收）始终持锁并 release 发布，执行权由
    // m_run_claim 串行化，因此这里 acquire 读到的是稳定指针。
    Fiber* fiber = m_fiber_ptr.load(std::memory_order_acquire);
    // 本次 run() 是否刚刚创建了执行栈：用于区分「首次启动」与「再次恢复」。
    bool created_fiber_now = false;
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
                std::lock_guard<detail::HotSpinLock> lock(m_transition_mutex);
                if (!m_fiber) {
                    m_fiber = std::move(created_fiber);
                    m_fiber_ptr.store(m_fiber.get(), std::memory_order_release);
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
                std::lock_guard<detail::HotSpinLock> lock(m_transition_mutex);
                StoreStateValue(GState::kFailed, std::memory_order_release);
                release_live_counter_locked();
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
    // M 亲和：把 Fiber 记录的「最近运行 M / LockToCurrentM 绑定」同步回
    // Task。唤醒方（可能是外部线程）读的是 Task 的原子字段，因此这一步
    // 必须在处理 requeue 之前完成——run() 紧接着就会把 G 放回队列，而
    // 另一个线程可能已经在 park 期间发起唤醒（pending 令牌）。
    m_affinity_machine.store(fiber->preferred_machine(),
                             std::memory_order_release);
    m_locked_machine.store(fiber->locked_machine(),
                           std::memory_order_release);
    const FiberState fiber_state = fiber->state();
    const auto fiber_failure = fiber_state == FiberState::Failed
                                   ? fiber->failure()
                                   : std::exception_ptr{};
    bool became_terminal = false;
    Function completed_function;
    std::unique_ptr<Fiber> completed_fiber;
    {
        std::lock_guard<detail::HotSpinLock> lock(m_transition_mutex);
        if (!resumed || fiber_state == FiberState::Completed ||
            fiber_state == FiberState::Failed) {
            m_deferred_enqueue.store(false, std::memory_order_relaxed);
            StoreWakePending(false, std::memory_order_relaxed);
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
            StoreStateValue(terminal_state, std::memory_order_release);
            release_live_counter_locked();
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
            m_fiber_ptr.store(nullptr, std::memory_order_release);
        } else if (fiber_state == FiberState::Suspended) {
            // Direct Fiber::Suspend calls are supported for advanced users.
            // Scheduler helpers set the state before switching；这里只处理
            // 「裸 Suspend」的回退映射。
            //
            // 状态与令牌必须来自同一次字读取：wake 现在不再取转换锁，
            // 若分两次读 state/pending，会漏掉「读到 Running 之后才置位」
            // 的令牌，把 G 留在 Waiting 且无人入队。CAS 失败说明并发方
            // 已经迁移了状态，重读后按新状态处理。
            for (;;) {
                std::uint32_t word =
                    m_state_word.load(std::memory_order_acquire);
                if (StateOfWord(word) != GState::kRunning) {
                    break;
                }
                const bool pending_wake = (word & kWakePendingBit) != 0U;
                const bool cancellation_requested =
                    m_cancel_requested.load(std::memory_order_relaxed);
                const bool requeue_now =
                    fiber->suspend_reason() == SuspendReason::Yield ||
                    pending_wake || cancellation_requested;
                if (!requeue_now) {
                    // 目标字不带令牌位：提交 Waiting 的同时清掉旧令牌。
                    if (m_state_word.compare_exchange_strong(
                            word,
                            static_cast<std::uint32_t>(GState::kWaiting),
                            std::memory_order_acq_rel,
                            std::memory_order_acquire)) {
                        break;
                    }
                    continue;
                }
                if (m_state_word.compare_exchange_strong(
                        word, static_cast<std::uint32_t>(GState::kRunnable),
                        std::memory_order_acq_rel,
                        std::memory_order_acquire)) {
                    m_deferred_enqueue.store(true, std::memory_order_release);
                    break;
                }
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
        const GState final_state = StateValue(std::memory_order_relaxed);
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
 * | `m_tasks` / `live_tasks` | admission 计数与存活 G 计数 |
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
        /// @brief 冷路径互斥（shutdown 排空与快照）；热路径不再进入。
        /// @details 本地队列本身是 256 槽无锁环形，push/pop 由 CAS 仲裁；
        ///          这把锁只在 begin_shutdown_locked() 等冷路径用于串行化
        ///          「排空 / 重新发布」的整段动作。
        mutable detail::HotSpinLock mutex;
        /// @brief 本地 runqueue（256 槽有界 MPMC 环形，FIFO）；worker 优先取它。
        LocalRunQueue queue;
        /// @brief 快速槽（单槽 LIFO）：刚 yield/刚被唤醒的 G 优先在这里，
        ///        下一轮由本 P 的 worker 直接取，不经过 256 槽环。
        RunNextSlot runnext;
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
        /// @brief 定向槽（单槽）：LockToCurrentM 绑定的 G 与「最近 M 空闲」
        ///        的定向唤醒只投到这里，由本 M 独占消费。普通 G 不走它。
        RunNextSlot inbox;
        std::atomic<std::size_t> runnext_hits{0};
        std::atomic<std::size_t> inbox_hits{0};
        std::atomic<std::size_t> same_machine_resumes{0};
        std::atomic<std::size_t> cross_machine_resumes{0};
        std::atomic<std::size_t> locked_redirects{0};
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
        wake_policy.store(config.wake_policy, std::memory_order_relaxed);
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
        // 没有等待者时跳过 notify_one：glibc 的 pthread_cond_signal 即使
        // 无人等待也要读 __wrefs/__wseq 并做 futex 记账（~20-50 cycles），
        // 而 4P/4M 空任务 churn 下这是每个 G 都走一次的路径。
        if (sleeping_workers.load(std::memory_order_relaxed) != 0) {
            condition.notify_one();
        }
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
        if (!scheduler || stopping() ||
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
            needs_grow.store(true, std::memory_order_relaxed);
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

    /// @brief 按 M id 查找仍可接收任务的 M；调用方必须持有 mutex。
    /// @details 只返回非 Dead 的记录；shutdown/reaper 只会把记录从
    ///          machines 中移除，因此持锁拿到的 shared_ptr 在锁外仍然有效。
    std::shared_ptr<Machine> find_machine_locked(MId machine_id) const {
        for (const auto& machine : machines) {
            if (machine && machine->id == machine_id &&
                machine->state.load(std::memory_order_acquire) !=
                    MState::kDead) {
                return machine;
            }
        }
        return {};
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
        // P18：进入维护即清除事件位。置位方（worker 退出）总是先递减
        // active_workers 再 store(true)，因此「先清位、后读计数」不会漏掉
        // 一次已经完成的递减：若读到的仍是满员值，那说明没有未完成的退出。
        needs_grow.store(false, std::memory_order_relaxed);
        // 满员快路径：普通可运行任务的需求以 P 数量为上限（见下方
        // extra_ceiling = processor_count + effective_blocking），所以
        // 「没有阻塞 M 且 active_workers 已达 P 数量」时扩容不可能发生。
        // 只读两个原子就返回：否则 4P/4M 空任务 churn 会每 8 个任务抢一次
        // admission 锁并遍历 machines，实测把 1e6 任务从 1.5us/task 拖到
        // 5.8us/task（GO2CPP_NO_GROW=1 对照可复现）。
        if (blocking_workers.load(std::memory_order_acquire) == 0 &&
            active_workers.load(std::memory_order_acquire) >=
                config.processor_count) {
            return;
        }
        if (!started.load(std::memory_order_acquire) ||
            draining() ||
            stopping()) {
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
            draining() ||
            stopping()) {
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
                    !stopping()) {
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
                       stopping() ||
                       sysmon_retry_pending.load(std::memory_order_acquire);
            });
            const bool stop = sysmon_stop.load(std::memory_order_acquire) ||
                              stopping();
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
        std::lock_guard<detail::HotSpinLock> registry_lock(registry_mutexes[stripe]);
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
            std::lock_guard<detail::HotSpinLock> queue_lock(processor.mutex);
            std::deque<std::shared_ptr<Task>> resumable;
            // 快速槽里的 G 与环里的语义相同，shutdown 必须一起排空；
            // 否则它会停在可运行状态却没有任何节点可被消费。
            if (auto runnext_task = processor.runnext.take()) {
                resumable.emplace_back(std::move(runnext_task));
            }
            while (auto task = processor.queue.pop()) {
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
            // 队列已换成无锁环形，不能整段 swap 回去；逐个重新发布，
            // 满时转入全局队列（本函数稍后的 global 排空会统一收尾）。
            for (auto& task : resumable) {
                if (!processor.queue.push(task)) {
                    global_queue.emplace_back(std::move(task));
                }
            }
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

        // 每个 M 的定向槽也可能持有可运行 G（LockToCurrentM / 严格同 M
        // 唤醒）；shutdown 不能漏掉它们，否则收尾会永远等不到终态。
        for (auto& machine : machines) {
            if (!machine) {
                continue;
            }
            auto inbox_task = machine->inbox.take();
            if (!inbox_task) {
                continue;
            }
            if (inbox_task->started()) {
                inbox_task->request_cancel(false);
                global_queue.emplace_back(std::move(inbox_task));
            } else {
                inbox_task->clear_queued();
                inbox_task->request_cancel(false);
                completion_notifications.emplace_back(inbox_task);
                runnable.fetch_sub(1, std::memory_order_relaxed);
                deferred_destruction.emplace_back(std::move(inbox_task));
            }
        }

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
            std::lock_guard<detail::HotSpinLock> registry_lock(
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
            std::lock_guard<detail::HotSpinLock> registry_lock(
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
        if (!draining()) {
            return false;
        }
        // 快路径：还有 G 未终态时直接返回，不做任何全表扫描。计数归零
        // 后才做一次全表确认 —— 即使计数因某条漏减路径提前归零，这里也
        // 会拒绝提前结束，正确性优先于性能。
        if (live_tasks_total() != 0) {
            return false;
        }
        if (!all_tasks_terminal_locked()) {
            return false;
        }
        set_draining(false);
        set_stopping(true);
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
    // 条带锁只在「注册表增删/回收」的短临界区里持有，且绝大多数时候没有
    // 竞争者。用 HotSpinLock 替代 std::mutex：失败路径要付 futex 状态机，
    // 在每任务一次入队的热路径上是可观的固定成本（见 detail::HotSpinLock）。
    mutable std::array<detail::HotSpinLock, kRegistryStripeCount>
        registry_mutexes;
    std::array<detail::HotSpinLock, kIncomingStripeCount> incoming_mutexes;
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
    // 正睡在 condition 上的 worker 数。notify_worker() 只在这个计数非零
    // 时才调用 condition.notify_one()：唤醒一个没有等待者的条件变量在
    // glibc 里仍要做原子读与 futex 记账，短任务 churn 下这是提交侧的
    // 固定成本。计数在 worker 进入 wait_for 之前递增、返回后递减，与
    // 谓词（runnable != 0）配合不会丢唤醒：enqueue 先加 runnable 再
    // notify，worker 无论看到哪个顺序都不会睡过新工作。
    std::atomic<std::size_t> sleeping_workers{0};
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
    // ------------------------------------------------------------------
    // 生命周期标志合并（park/wake 热路径只读一次）
    // ------------------------------------------------------------------
    //
    // 原实现把 draining / stopping / accepting 拆成三个 std::atomic<bool>，
    // 每次 park 要读两次（draining + stopping），每次 wake 还要多读一次
    // accepting。三个标志只有 shutdown 一个写者、语义又都是「单调关停」，
    // 因此合并成一个原子位图后，热路径只需一次 acquire load 就能同时拿到
    // 两个停止标志——与 Go 1.3 gopark/park0 只读 `sched.gcwaiting` 一个
    // 全局字的结构一致（proc.c:1355-1390）。
    //
    // 写者顺序：位图是唯一真值，不存在第二份状态需要同步。fetch_or /
    // fetch_and 使用 acq_rel，保证置位前的队列/注册表写入对读到 flag 的
    // park 方可见（与原先 release store 等价）。
    static constexpr std::uint8_t kDrainingFlag = 1U;
    static constexpr std::uint8_t kStoppingFlag = 2U;
    static constexpr std::uint8_t kNotAcceptingFlag = 4U;
    std::atomic<std::uint8_t> lifecycle_flags{0U};

    bool draining() const noexcept {
        return (lifecycle_flags.load(std::memory_order_acquire) &
                kDrainingFlag) != 0U;
    }
    bool stopping() const noexcept {
        return (lifecycle_flags.load(std::memory_order_acquire) &
                kStoppingFlag) != 0U;
    }
    bool accepting() const noexcept {
        return (lifecycle_flags.load(std::memory_order_acquire) &
                kNotAcceptingFlag) == 0U;
    }
    /// @brief park/wake 热路径的合并判断：一次 load 同时看两个停止标志。
    bool stop_requested() const noexcept {
        return (lifecycle_flags.load(std::memory_order_acquire) &
                (kDrainingFlag | kStoppingFlag)) != 0U;
    }
    void set_draining(bool on) noexcept {
        if (on) {
            lifecycle_flags.fetch_or(kDrainingFlag, std::memory_order_acq_rel);
        } else {
            lifecycle_flags.fetch_and(static_cast<std::uint8_t>(~kDrainingFlag),
                                      std::memory_order_acq_rel);
        }
    }
    /// @brief 新工作是否已被拒绝接纳（accepting=false 或 draining/stopping）。
    /// @details wake/requeue 的入口检查原先要读三次独立原子；现在与
    ///          park 共用同一个位图，一次 acquire load 即可判定。
    bool accept_blocked() const noexcept {
        return (lifecycle_flags.load(std::memory_order_acquire) &
                (kDrainingFlag | kStoppingFlag | kNotAcceptingFlag)) != 0U;
    }
    void set_stopping(bool on) noexcept {
        if (on) {
            lifecycle_flags.fetch_or(kStoppingFlag, std::memory_order_acq_rel);
        } else {
            lifecycle_flags.fetch_and(static_cast<std::uint8_t>(~kStoppingFlag),
                                      std::memory_order_acq_rel);
        }
    }
    void set_accepting(bool on) noexcept {
        if (on) {
            lifecycle_flags.fetch_and(
                static_cast<std::uint8_t>(~kNotAcceptingFlag),
                std::memory_order_acq_rel);
        } else {
            lifecycle_flags.fetch_or(kNotAcceptingFlag,
                                     std::memory_order_acq_rel);
        }
    }
    // 注册表中尚未进入终态的 G 数（未启动 / runnable / running / parked）。
    // 首次入队成功时 +1，终态转移时 -1；两者都在 Task 的
    // m_transition_mutex 内完成。draining 用「归零」做 O(1) 完成判定，
    // 再配合一次全表确认兜底，取代旧实现每完成一个 G 扫描全表的 O(n²)。
    // P5：存活 G 计数按「首次入队的目标 P」分片（16 个缓存行槽位，索引
    // & 15）。任务在自己的 P 上进入终态时递减的就是自己的分片行，缓存行
    // 留在本核；生产者递增写目标 P 的行。原来 2 次全局 RMW（4P 下争用
    // 一条缓存行，20-40ns/次）被摊到 4 条互不争用的行上。
    static constexpr std::size_t kLiveTaskShardCount = 16;
    struct LiveTaskShard {
        alignas(64) std::atomic<std::size_t> value{0};
    };
    std::array<LiveTaskShard, kLiveTaskShardCount> live_task_shards{};

    /// @brief 取分片计数；processor_hint 是首次入队时的目标 P 编号。
    std::atomic<std::size_t>* live_task_shard(
        std::size_t processor_hint) noexcept {
        const std::size_t index =
            processor_hint & (kLiveTaskShardCount - 1);
        return &live_task_shards[index].value;
    }

    /// @brief 存活 G 数的精确聚合（draining 判零必须用它，不能用近似值）。
    /// @details 只有 processor_count 个分片可能被使用（分片索引来自
    ///          target_hint % processor_count），所以聚合读 ≤ min(16, P) 行。
    std::size_t live_tasks_total() const noexcept {
        const std::size_t shards = std::max<std::size_t>(
            1U, std::min<std::size_t>(kLiveTaskShardCount,
                                      processors.size()));
        std::size_t total = 0;
        for (std::size_t i = 0; i < shards; ++i) {
            total += live_task_shards[i].value.load(
                std::memory_order_acquire);
        }
        return total;
    }

    // draining / stopping / accepting 的唯一位图与访问器见本类上方。
    std::atomic<std::size_t> runnable{0};
    std::atomic<std::size_t> active_workers{0};
    std::atomic<std::size_t> running_workers{0};
    std::atomic<std::size_t> blocking_workers{0};
    std::atomic<std::size_t> next_processor{0};
    // P18：扩容需求事件位。worker 退出（active_workers 递减之后）与阻塞
    // 进入会置位；enqueue 的稳态路径只读这一位（一次 relaxed load），
    // 不再逐任务读取 active_workers/runnable 做四个布尔判定。
    std::atomic<bool> needs_grow{true};
    // 常规回收的轮转游标：每次触发只压缩一个注册表条带。
    std::atomic<std::size_t> next_terminal_collect_stripe{0};
    std::atomic<std::uint64_t> metric_task_runs{0};
    std::atomic<std::uint64_t> metric_task_completions{0};
    std::atomic<std::uint64_t> metric_fiber_resume_ns{0};
    std::atomic<std::uint64_t> metric_local_queue_pops{0};
    std::atomic<std::uint64_t> metric_steal_pops{0};
    // M 亲和 / runnext 观测（只增计数器；命中率由快照差值计算）。
    std::atomic<std::uint64_t> metric_runnext_pushes{0};
    std::atomic<std::uint64_t> metric_runnext_pops{0};
    std::atomic<std::uint64_t> metric_runnext_displaced{0};
    std::atomic<std::uint64_t> metric_inbox_pushes{0};
    std::atomic<std::uint64_t> metric_inbox_pops{0};
    std::atomic<std::uint64_t> metric_affinity_wakes{0};
    std::atomic<std::uint64_t> metric_pinned_redirects{0};
    std::atomic<std::uint64_t> metric_same_machine_resumes{0};
    std::atomic<std::uint64_t> metric_cross_machine_resumes{0};
    std::atomic<std::uint64_t> metric_steal_runnext{0};
    // 运行期可切换的唤醒投递策略（A/B 对比用）。默认取 config。
    std::atomic<WakePolicy> wake_policy{WakePolicy::kRunNext};
    // run_main 的 main G 与完成标志。main_task 只在 run_main 内写（worker
    // 启动前 / 退出后），worker 侧只读比较；main_done 是原子退出信号。
    std::shared_ptr<Task> main_task;
    std::atomic<bool> main_done{false};
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
        m_impl->stopping()) {
        return;
    }

    m_impl->started.store(true, std::memory_order_release);
    m_impl->set_accepting(true);
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
        m_impl->set_accepting(false);
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

bool Scheduler::run_main(Task::Function function) {
    if (!m_impl || !function ||
        !m_impl->started.load(std::memory_order_acquire) ||
        m_impl->main_task) {
        return false;
    }
    // 主线程成为 m0：登记一台不拥有 std::thread 的 M 记录并绑定 P0。
    // 它和普通 worker 走完全相同的取任务/执行路径，区别只有两点：
    //   1) main G 绑定到本 M（定向槽投递，不会迁移）；
    //   2) main G 进入终态后置 main_done，本 M 退出循环，由这里收尾。
    auto machine = std::make_shared<Impl::Machine>(
        s_next_m_id.fetch_add(1, std::memory_order_relaxed), 0, false);
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->machines.emplace_back(machine);
        m_impl->active_workers.fetch_add(1, std::memory_order_relaxed);
        m_impl->processors[0].active_machines.fetch_add(
            1, std::memory_order_relaxed);
    }
    auto task = std::allocate_shared<Task>(TaskBlockAllocator<Task>{},
                                          std::move(function));
    // 绑定到本 M：调度器只把 main G 投进这台 M 的定向槽；别的 M 从队列
    // 误取到时，worker_loop 会把它重定向回来。
    task->m_locked_machine.store(machine->id, std::memory_order_release);
    task->m_affinity_machine.store(machine->id, std::memory_order_release);
    m_impl->main_task = task;
    if (!enqueue(task)) {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->main_task.reset();
        for (auto it = m_impl->machines.begin();
             it != m_impl->machines.end(); ++it) {
            if (*it == machine) {
                m_impl->machines.erase(it);
                break;
            }
        }
        m_impl->active_workers.fetch_sub(1, std::memory_order_relaxed);
        m_impl->needs_grow.store(true, std::memory_order_relaxed);
        m_impl->processors[0].active_machines.fetch_sub(
            1, std::memory_order_relaxed);
        return false;
    }
    // 当前线程作为 M 进入调度循环：main G 从定向槽取到后执行；它 park 时
    // 本 M 继续跑别的 G（与 Go 的 m0 参与调度一致）。
    worker_loop(std::static_pointer_cast<void>(machine));
    // worker_loop 退出时已把本 M 标记 Dead 并清空 TLS；剩余 G 由 shutdown
    // 的 draining 路径取消/回收。
    const bool completed = task->state() == GState::kDead;
    shutdown();
    m_impl->main_task.reset();
    m_impl->main_done.store(false, std::memory_order_release);
    return completed;
}

/**
 * @brief 合作式关停：关闭 admission、取消未启动 G、给运行中 G 设取消、等全部终态。
 *
 * ## 设计语义（D2，重要）
 *
 * `shutdown()`/`shutdown_for()` **不是**「排空队列」：调用时仍在队列里、
 * 尚未开始执行的已提交任务会被**取消**（进入 `kCancelled` 终态），不会
 * 被执行。需要「已提交任务全部跑完再关停」的调用方，必须先自行等待这些
 * 任务完成（`Task::state()` / `WaitGroup` / `Future`），再调用 shutdown。
 *
 * 这样设计的原因：GMP 的关停是合作式的，没有安全点可以切断正在执行的
 * C++ 栈；若 shutdown 先排空队列，一个持续提交新任务的调用方可以让它
 * 永远无法返回（活锁）。取消未开始的任务则保证关停有确定的上界：已在
 * 运行的 G 在下一个 park 点看到取消标志后退出，未开始的 G 直接进终态。
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
        if (!m_impl->draining() &&
            !m_impl->stopping()) {
            m_impl->set_accepting(false);
            m_impl->set_draining(true);
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
            m_impl->set_stopping(true);
            m_impl->set_draining(false);
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
        // O(1) 快路径：draining 每完成一个 G 都会 notify，若每次唤醒都
        // 全表扫描，10 万 G 的关停会回到 O(n²)。计数归零后再由
        // finish_draining_locked() 做一次全表确认兜底。
        const auto terminal = [this] {
            return m_impl->live_tasks_total() == 0;
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
        m_impl->set_stopping(true);
        m_impl->set_draining(false);
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
           !m_impl->draining() &&
           !m_impl->stopping();
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
    auto task = std::allocate_shared<Task>(TaskBlockAllocator<Task>{},
                                          std::move(function), options);
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
        m_impl->draining() &&
        task->started() && task->cancellation_requested();
    // P5：首次入队的目标 P 同时决定存活计数的分片——任务在该 P 上执行结束
    // 时递减的是它自己的分片行。这里只算一次，后面的直投 / 外部生产者
    // 投递复用同一个提示（轮转游标因此每任务只前进一次）。
    const std::size_t target_hint =
        from_worker ? static_cast<std::size_t>(t_processor_id)
                    : pick_processor_round_robin(m_impl->next_processor,
                                                 m_impl->processors.size());
    if ((!m_impl->accepting() && !drain_resume) ||
        m_impl->stopping()) {
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
                               m_impl->cancellation_gate,
                               m_impl->live_task_shard(target_hint))) {
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
        std::lock_guard<detail::HotSpinLock> registry_lock(
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
                std::lock_guard<detail::HotSpinLock> registry_lock(
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
        // ---- 外部生产者直投 P 队列（Go runqput 的等价路径）----------
        // 旧路径把每个外部任务都塞进 incoming 条带：一次条带锁 + 一次
        // deque 节点分配 + 三个跨核原子（runnable / incoming_count /
        // nonempty_mask）。rdtsc 分段实测占提交侧 415ns（总 1258ns）。
        // 本地环本来就是无锁 MPMC（tail 用 CAS 仲裁，多生产者也安全），
        // 直投它只需一次 CAS + 一次 runnable 记账；环满（256 槽）时才
        // 回退到 incoming 条带，因此长队列/积压语义完全保留。
        if (m_impl->accepting() && !m_impl->stopping()) {
            const auto direct = target_hint;
            auto& direct_processor =
                m_impl->processors[direct % m_impl->processors.size()];
            if (direct_processor.queue.push(task)) {
                m_impl->runnable.fetch_add(1, std::memory_order_relaxed);
                published = true;
            }
        }
        if (!published) {
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
            if (m_impl->accepting() &&
                !m_impl->stopping()) {
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
                std::lock_guard<detail::HotSpinLock> registry_lock(
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
        if ((!m_impl->accepting() && !drain_resume) ||
            m_impl->stopping()) {
            task->clear_queued();
            task->request_cancel(false);
            admission_lock.unlock();
            task->notify_terminal();
            return false;
        }
        const auto preferred = target_hint;
        auto& processor =
            m_impl->processors[preferred % m_impl->processors.size()];
        const bool use_global =
            processor.queue.size() >= m_impl->config.local_queue_limit;
        m_impl->runnable.fetch_add(1, std::memory_order_relaxed);
        try {
            // 本地环满（或 local_queue_limit=0）时溢出到全局队列，对应
            // Go 的 runqputslow；环形 push 本身不分配、不抛异常。
            if (!use_global && processor.queue.push(task)) {
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
                    std::lock_guard<detail::HotSpinLock> registry_lock(
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

    // P3：维护票改成线程局部自增——原实现每任务一次全局 fetch_add，只为
    // 拿一个「每 64/1024 次做一次维护」的票，票价远高于买到的维护。
    // P18：稳态不再逐任务读 active_workers/runnable；只有「扩容事件位被置」
    // 或本地采样命中 64 的倍数时才进入 maybe_grow（其内部自带走满员快
    // 路径，成本是两次 relaxed load）。worker 退出与阻塞进入会重新置位，
    // 因此最坏延迟是 64 个任务。
    ++t_maintenance_ticket;
    if (m_impl->needs_grow.load(std::memory_order_relaxed) ||
        (t_maintenance_ticket & 63U) == 0U) {
        m_impl->maybe_grow(this);
    }
    m_impl->notify_worker();
    return true;
}

/**
 * @brief 把已占用 queued 的任务发布到 P 的 runnext 快速槽。
 *
 * @details 被挤出的旧任务落回同一 P 的 256 槽环尾（Go `runqputslow` 的
 *          下一层）；环也满时清掉 queued 后交给全局 enqueue 收敛。
 *          发布本身不会失败——除非 task 为空。
 * @param processor 目标 P。
 * @param task 调用方已成功占用 m_queued 的任务。
 * @param allow_notify 是否允许在本 P 没有运行中的 M 时唤醒 worker。
 * @return 成功发布返回 true。
 */
bool Scheduler::publish_runnext(PId processor_id,
                                const std::shared_ptr<Task>& task,
                                bool allow_notify) {
    if (!task || processor_id >= m_impl->processors.size()) {
        return false;
    }
    auto& processor = m_impl->processors[processor_id];
    std::shared_ptr<Task> displaced;
    if (!processor.runnext.push(task, displaced)) {
        return false;
    }
    m_impl->metric_runnext_pushes.fetch_add(1, std::memory_order_relaxed);
    if (displaced) {
        m_impl->metric_runnext_displaced.fetch_add(1,
                                                   std::memory_order_relaxed);
        // 旧任务必须继续可运行：先尝试本地环尾，满时由 enqueue 收敛。
        if (!processor.queue.push(displaced)) {
            displaced->clear_queued();
            (void)enqueue(displaced);
        }
    }
    if (allow_notify) {
        // 该 P 已有运行中的 M 时，它会在当前 G 返回后优先取 runnext；
        // 此时唤醒额外 M 只会制造 futex 抖动（与本地环的判定一致）。
        const auto running = processor.running_machines.load(
            std::memory_order_relaxed);
        const auto active = processor.active_machines.load(
            std::memory_order_relaxed);
        if (running == 0 ||
            (processor.queue.size() >= 16U && running < active)) {
            m_impl->notify_worker();
        }
    }
    return true;
}

/**
 * @brief 把任务发布到指定 M 的定向槽（inbox）。
 *
 * ## 用途
 *
 * - `Fiber::LockToCurrentM()` 绑定的 G：只有绑定 M 能执行；
 * - `WakePolicy::kLastMachine` 下的严格同 M 唤醒：最近 M 空闲时直接把
 *   G 交给它，避免「唤醒一个 M、却被另一个 M 抢走」。
 *
 * ## 失败语义
 *
 * M 不存在/已死亡返回 false，调用方回退到普通队列并在需要时解除绑定。
 * 锁序：只在内部短暂持有 admission 锁查找 M，inbox 的自旋锁在锁外获取。
 */
bool Scheduler::publish_to_machine(MId machine_id,
                                   const std::shared_ptr<Task>& task,
                                   bool allow_notify) {
    if (!task || machine_id == 0) {
        return false;
    }
    std::shared_ptr<Impl::Machine> target;
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        target = m_impl->find_machine_locked(machine_id);
    }
    if (!target) {
        return false;
    }
    std::shared_ptr<Task> displaced;
    if (!target->inbox.push(task, displaced)) {
        return false;
    }
    m_impl->metric_inbox_pushes.fetch_add(1, std::memory_order_relaxed);
    if (displaced) {
        // 定向槽只有一个位置：被挤出的任务回落到该 M 所属 P 的队列。
        auto& processor = m_impl->processors[
            static_cast<std::size_t>(target->processor) %
            m_impl->processors.size()];
        if (!processor.queue.push(displaced)) {
            displaced->clear_queued();
            (void)enqueue(displaced);
        }
    }
    if (allow_notify) {
        // 定向槽只有目标 M 会消费：notify_one 可能唤醒别的 M，目标 M 继续
        // 睡到自己的 idle 超时，G 就白等一个周期。这里用广播保证目标 M
        // 一定被踢醒（低频路径，代价可接受）。
        m_impl->notify_all_workers();
    }
    return true;
}

bool Scheduler::requeue_to_processor(const std::shared_ptr<Task>& task,
                                     PId processor_id) {
    GO2CPP_DEBUG_EVENT(kRequeueProcessor, task ? task->id() : 0, processor_id);
    if (!task || processor_id >= m_impl->processors.size()) {
        return false;
    }
    if (m_impl->accept_blocked()) {
        return false;
    }
    if (!task->try_mark_queued()) {
        return task->queued();
    }

    auto& processor = m_impl->processors[processor_id];
    if (m_impl->accept_blocked()) {
        task->clear_queued();
        return false;
    }
    m_impl->runnable.fetch_add(1, std::memory_order_relaxed);
    const auto policy = m_impl->wake_policy.load(std::memory_order_relaxed);

    // LockToCurrentM 的 G 不属于「提示」而是硬约束：只能投到绑定 M 的
    // 定向槽。绑定 M 已死亡时解除绑定，否则 G 会永久搁浅。
    const MId locked = task->locked_machine_id();
    if (locked != 0 && policy != WakePolicy::kRoundRobin) {
        if (publish_to_machine(locked, task, true)) {
            return true;
        }
        task->unlock_from_m();
    }

    const MId affinity = task->last_machine_id();
    // ① 纯亲和：最近 M 还在就交给它（严格同 M，定向槽只由它消费）。
    if (affinity != 0 && policy == WakePolicy::kLastMachine &&
        affinity != 0U) {
        if (publish_to_machine(affinity, task, true)) {
            m_impl->metric_affinity_wakes.fetch_add(1,
                                                    std::memory_order_relaxed);
            return true;
        }
    }

    // ② Go 式（默认）：投到目标 P 的 runnext。同一 P 的 M 下一轮直接取，
    // 这是 Go `goready` 的快速槽语义；被挤出者自动落回本地环。
    if (policy != WakePolicy::kRoundRobin) {
        if (publish_runnext(processor_id, task, true)) {
            GO2CPP_DEBUG_EVENT(kRunQueuePush, task->id(), processor_id);
            if (affinity != 0 &&
                task->last_processor_id() == processor_id) {
                m_impl->metric_affinity_wakes.fetch_add(
                    1, std::memory_order_relaxed);
            }
            return true;
        }
    }

    // 回退（含 ③ 轮询对照）：目标 P 的本地环尾。
    if (!processor.queue.push(task)) {
        // 本地环满：清掉 reservation 后交回调用方的常规入队回退链。
        m_impl->runnable.fetch_sub(1, std::memory_order_relaxed);
        task->clear_queued();
        return false;
    }
    GO2CPP_DEBUG_EVENT(kRunQueuePush, task->id(), processor_id);
    {
        // 该 P 已有运行中的 M 时，它会在当前 G 返回后消费本地队列；
        // 此时唤醒额外 M 只会制造 futex 抖动。
        const auto running = processor.running_machines.load(
            std::memory_order_relaxed);
        const auto active = processor.active_machines.load(
            std::memory_order_relaxed);
        if (running == 0 ||
            (processor.queue.size() >= 16U && running < active)) {
            m_impl->notify_worker();
        }
    }
    return true;
}

bool Scheduler::requeue_from_worker(const std::shared_ptr<Task>& task,
                                    bool allow_notify, bool prefer_runnext) {
    GO2CPP_DEBUG_EVENT(kRequeueWorker, task ? task->id() : 0, 1);
    if (!task || current_scheduler() != this ||
        t_processor_id >= m_impl->processors.size()) {
        return enqueue(task);
    }

    // Worker 自己把刚刚 yield 的 G 放回原 P 时，不需要重新做 owner、
    // registry 和全局 admission。只使用 P 队列锁，避免每次 Fiber 切换
    // 都争用调度器总锁。shutdown 仍通过 draining/stopping 的二次检查与
    // P 队列锁建立一致的排空顺序。
    if (m_impl->accept_blocked()) {
        return enqueue(task);
    }

    auto& processor = m_impl->processors[t_processor_id];
    if (m_impl->accept_blocked()) {
        return enqueue(task);
    }
    if (!task->try_mark_queued_from_worker()) {
        return false;
    }
    // 计数必须先于节点发布：无锁环的消费者可能立刻 pop，若先发布再记账
    // 会先递减尚未增加的 runnable，造成 size_t 下溢。
    m_impl->runnable.fetch_add(1, std::memory_order_relaxed);
    const auto policy = m_impl->wake_policy.load(std::memory_order_relaxed);

    // LockToCurrentM 是硬约束而不是提示：锁定 G 只能进绑定 M 的定向槽。
    // yield 回队与同 M 同步唤醒也必须走这里，否则锁定 G 会被投进 runnext
    // 或本地环，同 P 的其它 M 取到后再做一次误取重定向（多付一次跨线程
    // 投递，也放宽了「只在绑定 M 上恢复」的语义）。
    const MId locked = task->locked_machine_id();
    if (locked != 0 && policy != WakePolicy::kRoundRobin) {
        if (publish_to_machine(locked, task, true)) {
            GO2CPP_DEBUG_EVENT(kRunQueuePush, task->id(), locked);
            return true;
        }
        // 绑定 M 已退场：解除绑定，回退到普通投递路径。
        task->unlock_from_m();
    }
    if (policy == WakePolicy::kRunNext && prefer_runnext) {
        // Go 式：被唤醒的 G 进本 P 的 runnext，当前 M 下一轮直接取它，
        // 不经过 256 槽环，也不给别的 M 从队尾偷走的机会。
        if (publish_runnext(t_processor_id, task,
                            allow_notify && !t_in_idle_poll)) {
            GO2CPP_DEBUG_EVENT(kRunQueuePush, task->id(), t_processor_id);
            return true;
        }
        m_impl->runnable.fetch_sub(1, std::memory_order_relaxed);
        task->clear_queued();
        return enqueue(task);
    }
    // ① 纯亲和 / ③ 轮询对照：放本地环尾；轮询策略把 G 交给下一个 P，
    // 用来测「不保留亲和」的代价。
    Impl::Processor* target = &processor;
    if (policy == WakePolicy::kRoundRobin && m_impl->processors.size() > 1) {
        target = &m_impl->processors[pick_processor_round_robin(
            m_impl->next_processor, m_impl->processors.size())];
    }
    if (!target->queue.push(task)) {
        m_impl->runnable.fetch_sub(1, std::memory_order_relaxed);
        task->clear_queued();
        return enqueue(task);
    }
    GO2CPP_DEBUG_EVENT(kRunQueuePush, task->id(), target->id);
    {
        // 当前 M 正在执行这个 G；本地回队无需再次唤醒另一个 M，
        // 减少每次 Fiber handoff 的条件变量切换。
        const auto running = target->running_machines.load(
            std::memory_order_relaxed);
        const auto active = target->active_machines.load(
            std::memory_order_relaxed);
        if (allow_notify && !t_in_idle_poll &&
            (running == 0 ||
             (target->queue.size() >= 16U && running < active))) {
            m_impl->notify_worker();
        }
    }
    return true;
}

bool Scheduler::requeue_from_worker_reserved(const std::shared_ptr<Task>& task,
                                             bool allow_notify) {
    // 前置条件：调用方已在 wake_for_wait_reserve() 的成功分支里占用了
    // m_queued。因此这里只做「发布队列节点」，不再取 Task 的转换锁 ——
    // 入队权与状态转换已经在一个临界区里完成。
    //
    // 失败出口必须 clear_queued()：调用方的回退链（requeue_from_worker /
    // requeue_to_processor / enqueue）都要求 m_queued 未被占用，否则
    // prepare_enqueue()/try_mark_queued() 会拒绝发布，留下无节点的
    // reservation。
    if (!task || current_scheduler() != this ||
        t_processor_id >= m_impl->processors.size()) {
        if (task) {
            task->clear_queued();
        }
        return false;
    }
    if (m_impl->accept_blocked()) {
        task->clear_queued();
        return false;
    }

    auto& processor = m_impl->processors[t_processor_id];
    if (m_impl->accept_blocked()) {
        task->clear_queued();
        return false;
    }
    // 计数必须先于节点发布，理由同 requeue_from_worker()。
    m_impl->runnable.fetch_add(1, std::memory_order_relaxed);
    const auto policy = m_impl->wake_policy.load(std::memory_order_relaxed);

    // LockToCurrentM 是硬约束而不是提示：锁定 G 只能进绑定 M 的定向槽。
    // yield 回队与同 M 同步唤醒也必须走这里，否则锁定 G 会被投进 runnext
    // 或本地环，同 P 的其它 M 取到后再做一次误取重定向（多付一次跨线程
    // 投递，也放宽了「只在绑定 M 上恢复」的语义）。
    const MId locked = task->locked_machine_id();
    if (locked != 0 && policy != WakePolicy::kRoundRobin) {
        if (publish_to_machine(locked, task, true)) {
            GO2CPP_DEBUG_EVENT(kRunQueuePush, task->id(), locked);
            return true;
        }
        // 绑定 M 已退场：解除绑定，回退到普通投递路径。
        task->unlock_from_m();
    }
    if (policy == WakePolicy::kRunNext) {
        if (publish_runnext(t_processor_id, task,
                            allow_notify && !t_in_idle_poll)) {
            GO2CPP_DEBUG_EVENT(kRunQueuePush, task->id(), t_processor_id);
            return true;
        }
        m_impl->runnable.fetch_sub(1, std::memory_order_relaxed);
        task->clear_queued();
        return false;
    }
    Impl::Processor* target = &processor;
    if (policy == WakePolicy::kRoundRobin && m_impl->processors.size() > 1) {
        target = &m_impl->processors[pick_processor_round_robin(
            m_impl->next_processor, m_impl->processors.size())];
    }
    if (!target->queue.push(task)) {
        m_impl->runnable.fetch_sub(1, std::memory_order_relaxed);
        task->clear_queued();
        return false;
    }
    GO2CPP_DEBUG_EVENT(kRunQueuePush, task->id(), target->id);
    {
        const auto running = target->running_machines.load(
            std::memory_order_relaxed);
        const auto active = target->active_machines.load(
            std::memory_order_relaxed);
        if (allow_notify && !t_in_idle_poll &&
            (running == 0 ||
             (target->queue.size() >= 16U && running < active))) {
            m_impl->notify_worker();
        }
    }
    return true;
}

bool Scheduler::yield(const std::shared_ptr<Task>& task) {
    if (!task || current_scheduler() != this ||
        current_task_ptr() != task.get() || task->cancellation_requested() ||
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
        current_task_ptr() != task.get()) {
        GO2CPP_DEBUG_EVENT(kParkRejected, task ? task->id() : 0, 1);
        return false;
    }
    if (m_impl->stop_requested() || task->cancellation_requested()) {
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
    if (m_impl->stop_requested() || task->cancellation_requested()) {
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
        current_task_ptr() != task.get()) {
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
        current_task_ptr() != task.get()) {
        GO2CPP_DEBUG_EVENT(kParkRejected, task ? task->id() : 0, 1);
        return false;
    }
    if (m_impl->stop_requested() || task->cancellation_requested()) {
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
    if (m_impl->stop_requested() || task->cancellation_requested()) {
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
        current_task_ptr() != task.get()) {
        GO2CPP_DEBUG_EVENT(kParkRejected, task ? task->id() : 0, 1);
        return false;
    }
    // Shutdown scans waiting Gs under this same mutex. Do not enter waiting
    // after the scan has begun.
    ParkAction action = ParkAction::kRejected;
    {
        std::lock_guard<std::mutex> admission_lock(m_impl->mutex);
        if (m_impl->stop_requested() ||
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
    if (!task || !m_impl) {
        return false;
    }
#if !defined(NDEBUG)
    // 调试构建保留完整的所有权复核，跨调度器误用会在测试期直接暴露。
    if (!task->owned_by(m_impl->owner_token)) {
        return false;
    }
#else
    // release 快路径：`registered()` 为真说明该 G 走过
    // `enqueue() -> prepare_enqueue()` 的 owner 绑定（`try_register()`
    // 只在其成功之后调用），因此无需为一次复核再取
    // `Task::m_transition_mutex`。未注册的 G（从未入队 / 已回收）仍走
    // 完整校验，不会绕过所有权检查。
    if (!task->registered() && !task->owned_by(m_impl->owner_token)) {
        return false;
    }
#endif
    // 本 M 上的同步唤醒（无缓冲 channel / Mutex 交接的热路径）可以在
    // 同一个临界区里顺带预留入队权：wake 的状态转换与 try_mark_queued 的
    // 「检查 + 置位」本来就共用 m_transition_mutex，分两次取锁只是多付
    // 一次无争用也要 10ns 量级的锁。外部线程唤醒仍走完整路径。
    const bool worker_local =
        current_scheduler() == this &&
        t_processor_id < m_impl->processors.size() && task->registered();
    const auto action = worker_local ? task->wake_for_wait_reserve()
                                     : task->wake_for_wait();
    if (action == WakeAction::kPending) {
        // G 正在执行：唤醒只能登记成令牌位，由它下一个 park 点消费，
        // 这里没有发布队列节点，按契约返回 false。
        GO2CPP_DEBUG_EVENT(kWakePending, task->id(), 0);
        return false;
    }
    if (action == WakeAction::kRegistered) {
        // G 已提交 Waiting、执行权尚未释放：状态已推进到 kRunnable 并置
        // deferred_enqueue，执行权释放后由拥有者的收尾路径补入队。
        // 唤醒请求已经登记成功。
        GO2CPP_DEBUG_EVENT(kWakeEnqueued, task->id(), 0);
        return true;
    }
    if (action != WakeAction::kEnqueue) {
        GO2CPP_DEBUG_EVENT(kWakeRejected, task->id(), 2);
        return false;
    }
    if (!task->registered()) {
        if (worker_local) {
            // 注册表在本 M 做出判断之后被回收：预留的入队权必须退回，
            // 否则 enqueue() 的 prepare_enqueue() 会看到 m_queued=true。
            task->clear_queued();
        }
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
    if (worker_local && requeue_from_worker_reserved(task)) {
        GO2CPP_DEBUG_EVENT(kWakeEnqueued, task->id(),
                           m_impl->runnable.load(std::memory_order_relaxed));
        return true;
    }
    if (current_scheduler() == this &&
        t_processor_id < m_impl->processors.size() &&
        requeue_from_worker(task)) {
        GO2CPP_DEBUG_EVENT(kWakeEnqueued, task->id(),
                           m_impl->runnable.load(std::memory_order_relaxed));
        return true;
    }
    // 混合 Mutex 的 Unlock 通常来自普通线程。G 最近运行的 P 是稳定的
    // 亲和性提示，把唤醒直接放入该 P 的快速槽，绕过 incoming stripe
    // 和 scheduler admission 锁；P 失效或正在 shutdown 时仍走原回退路径。
    // ③ 轮询对照策略下不保留亲和，改由 P 编号轮转选择。
    PId preferred_processor = task->last_processor_id();
    if (m_impl->wake_policy.load(std::memory_order_relaxed) ==
            WakePolicy::kRoundRobin &&
        !m_impl->processors.empty()) {
        preferred_processor = static_cast<PId>(pick_processor_round_robin(
            m_impl->next_processor, m_impl->processors.size()));
    }
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
        if (m_impl->accepting() &&
            !m_impl->stopping()) {
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
    if (action == WakeAction::kPending ||
        action == WakeAction::kRegistered) {
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
    // ③ 轮询对照策略下改由 P 编号轮转选择（A/B 基准用）。
    PId preferred_processor = task->last_processor_id();
    if (m_impl->wake_policy.load(std::memory_order_relaxed) ==
            WakePolicy::kRoundRobin &&
        !m_impl->processors.empty()) {
        preferred_processor = static_cast<PId>(pick_processor_round_robin(
            m_impl->next_processor, m_impl->processors.size()));
    }
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
            if (action == WakeAction::kPending ||
                action == WakeAction::kRegistered) {
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
            m_impl->accept_blocked()) {
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
            std::lock_guard<detail::HotSpinLock> queue_lock(processor.mutex);
            if (m_impl->accept_blocked()) {
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
                    m_impl->runnable.fetch_add(1,
                                               std::memory_order_relaxed);
                    if (processor.queue.push(task)) {
                        GO2CPP_DEBUG_EVENT(kRunQueuePush, task->id(),
                                           processor_id);
                        GO2CPP_DEBUG_EVENT(
                            kWakeEnqueued, task->id(),
                            m_impl->runnable.load(std::memory_order_relaxed));
                        ++published;
                    } else {
                        m_impl->runnable.fetch_sub(1,
                                                   std::memory_order_relaxed);
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
    if (!task) {
        return false;
    }
#if !defined(NDEBUG)
    // 调试构建保留完整的所有权复核，跨调度器误用会在测试期直接暴露。
    if (!task->owned_by(m_impl->owner_token)) {
        return false;
    }
#else
    // release 快路径：`registered()` 为真说明该 G 走过
    // `enqueue() -> prepare_enqueue()` 的 owner 绑定（`try_register()`
    // 只在其成功之后调用），因此无需为一次复核再取
    // `Task::m_transition_mutex`。未注册的 G（从未入队 / 已回收）仍走
    // 完整校验，不会绕过所有权检查。
    if (!task->registered() && !task->owned_by(m_impl->owner_token)) {
        return false;
    }
#endif
    const auto action = task->wake_for_wait();
    if (action == WakeAction::kPending) {
        // 运行中的 G 会在下次 park 时消费令牌；没有发布队列节点。
        GO2CPP_DEBUG_EVENT(kWakePending, task->id(), 0);
        return false;
    }
    if (action == WakeAction::kRegistered) {
        // suspend 已提交、执行权尚未释放：deferred 入队由拥有者收尾完成。
        GO2CPP_DEBUG_EVENT(kWakeEnqueued, task->id(), 0);
        return true;
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
    // 外部线程（Mutex Unlock / 条件变量 notify）唤醒：先按最近 M 投递到
    // 目标 P 的快速槽，命中时同 M 续跑、保留缓存；失败才落到全局 enqueue。
    // 这正是 Go `goready` 的「唤醒者 P 的 runnext → 全局队列」顺序。
    PId preferred_processor = task->last_processor_id();
    if (m_impl->wake_policy.load(std::memory_order_relaxed) ==
            WakePolicy::kRoundRobin &&
        !m_impl->processors.empty()) {
        preferred_processor = static_cast<PId>(pick_processor_round_robin(
            m_impl->next_processor, m_impl->processors.size()));
    }
    if (preferred_processor < m_impl->processors.size() &&
        requeue_to_processor(task, preferred_processor)) {
        GO2CPP_DEBUG_EVENT(kWakeEnqueued, task->id(),
                           m_impl->runnable.load(std::memory_order_relaxed));
        return true;
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

void Scheduler::set_wake_policy(WakePolicy policy) noexcept {
    if (m_impl) {
        // release：让已经读到旧策略的 worker 在下一次 load 后看到新值；
        // 只影响后续唤醒的投递目标，不搬动已入队的 G。
        m_impl->wake_policy.store(policy, std::memory_order_release);
    }
}

WakePolicy Scheduler::wake_policy() const noexcept {
    return m_impl ? m_impl->wake_policy.load(std::memory_order_acquire)
                  : WakePolicy::kRunNext;
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
        // 无锁环的 size() 本身就是 acquire 读，不再需要冷路径锁。
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
        std::lock_guard<detail::HotSpinLock> lock(
            m_impl->registry_mutexes[stripe]);
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
    std::lock_guard<detail::HotSpinLock> registry_lock(
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
    std::lock_guard<detail::HotSpinLock> registry_lock(
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
            machine->replacement,
            machine->runnext_hits.load(std::memory_order_relaxed),
            machine->inbox_hits.load(std::memory_order_relaxed),
            machine->same_machine_resumes.load(std::memory_order_relaxed),
            machine->cross_machine_resumes.load(std::memory_order_relaxed),
            machine->locked_redirects.load(std::memory_order_relaxed)});
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
            m_impl->metric_steal_pops.load(std::memory_order_acquire),
            m_impl->metric_runnext_pushes.load(std::memory_order_acquire),
            m_impl->metric_runnext_pops.load(std::memory_order_acquire),
            m_impl->metric_runnext_displaced.load(std::memory_order_acquire),
            m_impl->metric_inbox_pushes.load(std::memory_order_acquire),
            m_impl->metric_inbox_pops.load(std::memory_order_acquire),
            m_impl->metric_affinity_wakes.load(std::memory_order_acquire),
            m_impl->metric_pinned_redirects.load(std::memory_order_acquire),
            m_impl->metric_same_machine_resumes.load(
                std::memory_order_acquire),
            m_impl->metric_cross_machine_resumes.load(
                std::memory_order_acquire),
            m_impl->metric_steal_runnext.load(std::memory_order_acquire)};
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
    // 热路径统计开关。`SchedulerConfig::collect_metrics` 默认关闭，关闭时
    // worker 不再为投递/迁移计数付跨核原子 RMW，也不在每次 run 前后取钟。
    // 配置只在 start() 前设定，因此循环外缓存一次即可。
    const bool hot_stats = m_impl->config.collect_metrics;

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
        std::lock_guard<detail::HotSpinLock> lock(task->m_transition_mutex);
        const auto state = task->StateValue(std::memory_order_relaxed);
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

    // M 亲和：任务可能被 LockToCurrentM 绑定到别的 M。无锁上下文
    // （step0/step2）可以直接把它投回绑定 M 的定向槽；持锁上下文
    // （incoming 条带 / admission）只能判定，把重定向推迟到锁外 enqueue
    // （SCHED-01/02 的锁序教训）。
    const auto redirect_locked = [&](const std::shared_ptr<Task>& candidate) {
        const MId locked = candidate->locked_machine_id();
        if (locked == 0 || locked == machine->id) {
            return false;
        }
        if (publish_to_machine(locked, candidate, true)) {
            m_impl->metric_pinned_redirects.fetch_add(
                1, std::memory_order_relaxed);
            machine->locked_redirects.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        // 绑定 M 已经不存在：解除绑定，让本 M 执行，避免永久搁浅。
        candidate->unlock_from_m();
        return false;
    };
    const auto locked_elsewhere = [&](const std::shared_ptr<Task>& candidate) {
        const MId locked = candidate->locked_machine_id();
        return locked != 0 && locked != machine->id;
    };

    for (;;) {
        std::shared_ptr<Task> task;
        bool claimed = false;
        bool local_queue_pop = false;
        // B1 的补发布是延迟动作：三个找活路径都可能持有 admission /
        // incoming 条带锁，必须等本轮所有调度器锁释放后再 enqueue。
        std::shared_ptr<Task> unclaimed_republish;
        // 持锁路径发现「锁定到别的 M」的 G 时只把它摘出来，等所有调度器
        // 锁释放后再 enqueue（enqueue 会按绑定 M 重新投递）。
        std::shared_ptr<Task> redirect_pending;
        // step0: 快速槽——本 M 的定向槽（LockToCurrentM / 严格同 M 唤醒）
        // 与本 P 的 runnext。二者都是刚被唤醒的 G，直接续跑能保留缓存；
        // Go 的 runqget 也是先 runnext 再 runq。
        if (!m_impl->stopping()) {
            task = machine->inbox.take();
            if (task) {
                if (hot_stats) {
                    m_impl->metric_inbox_pops.fetch_add(
                        1, std::memory_order_relaxed);
                    machine->inbox_hits.fetch_add(1,
                                                  std::memory_order_relaxed);
                }
            } else {
                task = own_processor.runnext.take();
                if (task && hot_stats) {
                    m_impl->metric_runnext_pops.fetch_add(
                        1, std::memory_order_relaxed);
                    machine->runnext_hits.fetch_add(1,
                                                    std::memory_order_relaxed);
                }
            }
            if (task && redirect_locked(task)) {
                task.reset();
            }
            if (task) {
                m_impl->runnable.fetch_sub(1, std::memory_order_relaxed);
                claimed = task->claim_from_queue();
                if (claimed) {
                    GO2CPP_DEBUG_EVENT(kRunQueueClaimOk, task->id(), 0);
                } else {
                    GO2CPP_DEBUG_EVENT(kRunQueueClaimFail, task->id(), 0);
                    if (recover_unclaimed(task)) {
                        unclaimed_republish = task;
                    }
                    task.reset();
                }
            }
        }
        // step2: 快路径——只碰本 P 的本地队列，不加调度器总锁。
        // 绝大多数 yield 回来的 G 都会回到原 P。本地队列由同一 P 的
        // worker 独占消费；只有本地为空时才进入需要全局 admission 锁的
        // 全局队列/窃取路径。
        // 外部生产者现在直投本地环（见 enqueue），因此这里不能再要求
        // 「积压超过 P 数」才取本地环 —— 那会让刚投进来的 G 停在环里
        // 无人消费。无条件尝试一次 pop：环空时只是一次 head/tail 读。
        if (!task && !m_impl->stopping() &&
            m_impl->running_workers.load(std::memory_order_acquire) != 0) {
            task = own_processor.queue.pop();
            if (task) {
                local_queue_pop = true;
                GO2CPP_DEBUG_EVENT(kRunQueuePop, task->id(),
                                   own_processor.id);
            }
            if (task && redirect_locked(task)) {
                // 锁定到别的 M：已投回定向槽，本 M 继续找下一个 G。
                task.reset();
                local_queue_pop = false;
            }
            if (task) {
                m_impl->runnable.fetch_sub(1, std::memory_order_relaxed);
                claimed = task->claim_from_queue();
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
        if (task && m_impl->stopping()) {
            task->request_cancel(false);
        }
        if (!task && !m_impl->stopping()) {
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
                    // deque::erase(begin) 必须搬移全部剩余元素（O(n)）；
                    // 外部生产者按 FIFO 追加，worker 默认路径
                    // （require_class=false）永远取队首，长队列下这条路径
                    // 会把取任务退化成 O(n²)。取队首时改用 pop_front()。
                    if (selected == incoming_queue.begin()) {
                        incoming_queue.pop_front();
                    } else {
                        incoming_queue.erase(selected);
                    }
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
                    if (locked_elsewhere(task)) {
                        // 锁内只判定：锁外按绑定 M 重新投递。取出时已从
                        // runnable 减 1，但该 G 仍可运行（只是换投递位置），
                        // 这里补回计数；锁外 publish 不再重复记账。
                        m_impl->runnable.fetch_add(
                            1, std::memory_order_relaxed);
                        redirect_pending = task;
                        task.reset();
                        return true;
                    }
                    claimed = task->claim_from_queue();
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
        if (task && m_impl->stopping()) {
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
            if (!m_impl->stopping()) {
                const bool prefer_class =
                    machine->last_task_class.load(std::memory_order_relaxed) != 0 &&
                    machine->affinity_budget.load(std::memory_order_relaxed) != 0 &&
                    m_impl->config.task_affinity_budget != 0;
                const auto preferred_class =
                    machine->last_task_class.load(std::memory_order_relaxed);
                const auto scan_limit = m_impl->config.task_affinity_budget;
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
                // Rescue resumes are serviced first: they exist only after
                // queue allocation failure or a shutdown wake handoff.
                task = m_impl->emergency_pop_locked();

                // Prefer the local FIFO queue（无锁环，出队节点即独占）。
                if (!task) {
                    task = own_processor.queue.pop();
                    if (task) {
                        GO2CPP_DEBUG_EVENT(kRunQueuePop, task->id(),
                                           own_processor.id);
                    }
                }

                // Then inspect the global FIFO queue.
                if (!task && !m_impl->global_queue.empty()) {
                    task = take_global(m_impl->global_queue);
                }

                // Finally steal from another P. 照抄 Go runqgrab：一次抢
                // 一半（上限 32），自己留一个先跑，其余放回本 P 的环；
                // 本 P 环也满则转入全局队列（此处仍持有 admission 锁）。
                if (!task && m_impl->processors.size() > 1) {
                    const auto start = pick_processor_round_robin(
                        m_impl->next_processor, m_impl->processors.size());
                    [[maybe_unused]] std::uint64_t victim_id = 0;
                    std::shared_ptr<Task> stolen[32];
                    std::size_t stolen_count = 0;
                    Impl::Processor* stolen_from = nullptr;
                    for (std::size_t offset = 0;
                         offset < m_impl->processors.size(); ++offset) {
                        auto& victim = m_impl->processors[
                            (start + offset) % m_impl->processors.size()];
                        if (&victim == &own_processor) {
                            continue;
                        }
                        const auto victim_size = victim.queue.size();
                        if (victim_size == 0) {
                            continue;
                        }
                        const auto want = std::min<std::size_t>(
                            victim_size - victim_size / 2,
                            sizeof(stolen) / sizeof(stolen[0]));
                        stolen_count = victim.queue.steal(stolen, want);
                        if (stolen_count != 0) {
                            victim_id = static_cast<std::uint64_t>(victim.id);
                            stolen_from = &victim;
                            break;
                        }
                    }
                    // 不偷别的 P 的 runnext：它是「最近唤醒」的快速槽，
                    // owner M 下一轮就会取。把它偷走会让被唤醒的 G 继承
                    // 偷窃者的 M，破坏唤醒时保留缓存的初衷（实测 2P/2M 下
                    // 跨 M 恢复从 ~15% 升到 ~85%）。队列本身的窃取不受影响。
                    if (stolen_count != 0) {
                        task = std::move(stolen[0]);
                        for (std::size_t i = 1; i < stolen_count; ++i) {
                            if (!own_processor.queue.push(stolen[i])) {
                                try {
                                    m_impl->global_queue.push_back(
                                        std::move(stolen[i]));
                                } catch (...) {
                                    if (stolen_from != nullptr) {
                                        (void)stolen_from->queue.push(
                                            std::move(stolen[i]));
                                    }
                                }
                            }
                        }
                        if (hot_stats) {
                            m_impl->metric_steal_pops.fetch_add(
                                1, std::memory_order_relaxed);
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

                if (task && locked_elsewhere(task)) {
                    // 持锁上下文：只摘出来，锁外重新入队。
                    redirect_pending = task;
                    task.reset();
                }
                if (task) {
                    m_impl->runnable.fetch_sub(1, std::memory_order_relaxed);
                    claimed = task->claim_from_queue();
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
        if (redirect_pending) {
            // 锁定到别的 M 的 G 在锁外重新投递。此时节点已从队列取出、
            // 但尚未 claim，m_queued 仍为 true：普通 enqueue() 会被
            // prepare_enqueue() 以「已排队」拒绝，把 G 丢成「Runnable
            // 但没有任何队列节点」——实测这是 run_main 偶发全空闲挂死
            // 的根因（锁定 main G 被别的 M 取到 → redirect → enqueue
            // 失败 → 任务无人再取）。直接投回绑定 M 的定向槽，保留节点
            // 与 runnable 计数。
            const MId locked = redirect_pending->locked_machine_id();
            if (locked == 0 ||
                !publish_to_machine(locked, redirect_pending, true)) {
                // 绑定 M 已退场：解除绑定后放回本 P 队列（计数不变）。
                redirect_pending->unlock_from_m();
                if (!own_processor.queue.push(redirect_pending)) {
                    // 本地环也满：还原计数并清 queued 后走常规入队。
                    m_impl->runnable.fetch_sub(
                        1, std::memory_order_relaxed);
                    redirect_pending->clear_queued();
                    (void)enqueue(redirect_pending);
                }
            }
        }

        // Both the local fast path and the admission path arrive here with a
        // claimed G. Keep the M/P accounting in one place so a local dequeue
        // cannot skip running_workers/affinity bookkeeping.
        if (task && claimed) {
            m_impl->running_workers.fetch_add(1, std::memory_order_relaxed);
            machine->state.store(MState::kRunning, std::memory_order_release);
            // M 亲和命中统计：本次恢复与 G 最近一次运行的 M 是否相同。
            // 首次运行（last_machine==0）不计入，避免把冷启动算成命中。
            if (hot_stats) {
                const MId last_machine = task->last_machine_id();
                if (last_machine != 0) {
                    if (last_machine == t_machine_id) {
                        m_impl->metric_same_machine_resumes.fetch_add(
                            1, std::memory_order_relaxed);
                        machine->same_machine_resumes.fetch_add(
                            1, std::memory_order_relaxed);
                    } else {
                        m_impl->metric_cross_machine_resumes.fetch_add(
                            1, std::memory_order_relaxed);
                        machine->cross_machine_resumes.fetch_add(
                            1, std::memory_order_relaxed);
                    }
                }
            }
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
            // 用移动而不是拷贝赋值：任务所有权只是从局部变量搬到 TLS
            // 槽，shared_ptr 的拷贝会额外做两次原子引用计数（uncontended
            // lock inc/dec 在热路径上约 10ns/往返），而移动是纯指针搬运。
            // 本轮结束时再移动回 task，TLS 槽恢复为空，语义与原先的
            // t_task = task / t_task.reset() 完全一致。
            t_task = std::move(task);
            GO2CPP_DEBUG_BIND_TASK(t_task->id());
            Fiber::BindCurrentExecution(FiberExecutionBinding{
                reinterpret_cast<std::uintptr_t>(this), 0, t_machine_id,
                t_processor_id, true});
            m_impl->emit_event(SchedulerEvent{
                SchedulerEventType::kTaskStarted, t_task->id(), t_machine_id,
                t_processor_id, GState::kRunning});
            const bool collect_metrics = hot_stats;
            const auto run_started = collect_metrics ? steady_now_ns() : 0;
            // step5: 执行 G。返回值通过 task 的终态体现（Dead/Failed/
            // Cancelled/Yielded/Parked），这里只负责记账与后续回收。
            t_task->run();
            const auto task_state_after_run = t_task->state();
            if (task_state_after_run == GState::kDead ||
                task_state_after_run == GState::kCancelled ||
                task_state_after_run == GState::kFailed) {
                ++terminal_since_collect;
                // main G 返回：通知所有 M 退出循环（Go 的 main 返回即进程
                // 退出；这里保留 shutdown 的 draining 收尾，而不是直接杀
                // 线程），由 run_main 在主线程上完成 join。
                if (m_impl->main_task && t_task == m_impl->main_task) {
                    m_impl->main_done.store(true, std::memory_order_release);
                    m_impl->notify_all_workers();
                }
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
                task_event_type, t_task->id(), t_machine_id, t_processor_id,
                task_state_after_run});
            if (collect_metrics) {
                const auto resume_elapsed =
                    static_cast<std::uint64_t>(std::max<std::int64_t>(
                        0, steady_now_ns() - run_started));
                m_impl->metric_task_runs.fetch_add(
                    1, std::memory_order_relaxed);
                m_impl->metric_fiber_resume_ns.fetch_add(
                    resume_elapsed, std::memory_order_relaxed);
                if (t_task->terminal()) {
                    m_impl->metric_task_completions.fetch_add(
                        1, std::memory_order_relaxed);
                }
                if (local_queue_pop) {
                    m_impl->metric_local_queue_pops.fetch_add(
                        1, std::memory_order_relaxed);
                }
            }
            task = std::move(t_task);
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
                // yield 语义：放本地队列尾（Go 的 goyield 用
                // runqput(next=false)）。若把 yield 的 G 放进 runnext，
                // 持续 yield 的 G 会一直占住快速槽，把 park 后被唤醒的 G
                // 挤进队列而永远轮不到（1P/1M Join 场景实测饿死）。
                requeued_self = requeue_from_worker(task, false, false);
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
                m_impl->draining();
            const std::size_t collect_interval =
                m_impl->config.terminal_collect_interval;
            if (draining) {
                // 收尾阶段的完成判定是 O(1) 的：只有最后一个 G 进入终态
                // 才对全表做一次确认与回收。旧实现在这里每完成一个 G 就
                // 扫两遍注册表（collect + all_terminal），10 万 G 的
                // shutdown 因此退化成 O(n²)。
                if (m_impl->live_tasks_total() == 0) {
                    bool finished = false;
                    {
                        std::lock_guard<std::mutex> admission_lock(
                            m_impl->mutex);
                        m_impl->collect_terminal_locked(deferred_destruction);
                        finished = m_impl->finish_draining_locked();
                        terminal_since_collect = 0;
                    }
                    if (finished) {
                        m_impl->notify_all_workers();
                    }
                }
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
            if (!draining && !requeued_self &&
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
                // draining 中的全表回收推迟到存活计数归零（收尾完成点），
                // 否则每消费一个 stale 节点都全表扫描会退化成 O(n²)。
                if (!m_impl->draining() ||
                    m_impl->live_tasks_total() == 0) {
                    m_impl->collect_terminal_locked(deferred_destruction);
                }
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

        if (m_impl->stopping() ||
            m_impl->main_done.load(std::memory_order_acquire)) {
            // 定向槽里的 G 还没有执行者：放回本 P 队列，交给 shutdown 的
            // 排空逻辑统一处理（否则 shutdown 会等一个没人取的 G）。
            if (auto pending = machine->inbox.take()) {
                if (!own_processor.queue.push(pending)) {
                    std::lock_guard<std::mutex> admission_lock(m_impl->mutex);
                    try {
                        m_impl->global_queue.push_back(std::move(pending));
                    } catch (...) {
                        m_impl->emergency_enqueue_locked(pending);
                    }
                }
            }
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
        if (idle_poller != nullptr && !m_impl->draining() &&
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
                m_impl->stopping();
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

        // ------------------------------------------------------------------
        // 空闲自旋（Go 的 spinning M）：短任务 churn 下生产者交付下一个 G
        // 只要几百纳秒，而一次「condition_variable 睡眠 → futex 唤醒」往返
        // 是 1~3µs。没有这段自旋时，空闲 worker 每处理一个空任务都要先睡
        // 再被 futex 叫醒：实测 4P/4M 空任务 churn 的提交循环结束时任务
        // 已全部执行完（说明瓶颈是唤醒而非执行），1P 下提交侧 1055ns/task
        // 也几乎全是这次往返。
        //
        // 自旋有界（kIdleSpinRounds 次 pause），期间只读 runnable / stopping
        // 两个原子，不碰任何队列锁；轮数用尽后仍回落到条件变量等待，
        // 因此空闲 CPU 占用与 shutdown 收敛性都不变。
        for (std::size_t spin = 0; spin < kIdleSpinRounds; ++spin) {
            if (m_impl->runnable.load(std::memory_order_acquire) != 0 ||
                m_impl->stopping() ||
                m_impl->main_done.load(std::memory_order_acquire)) {
                break;
            }
            CpuRelax();
        }
        if (m_impl->runnable.load(std::memory_order_acquire) != 0) {
            // 有新工作：回主循环重扫（下次进入这里时重新获得完整自旋预算）。
            continue;
        }
        if (m_impl->stopping() ||
            m_impl->main_done.load(std::memory_order_acquire)) {
            continue;
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
        // 登记「本 M 即将睡在 condition 上」：notify_worker() 依据这个
        // 计数决定是否需要 condition.notify_one()。必须在检查谓词之前
        // 递增 —— 生产者先加 runnable 再读该计数，两者配合保证不会
        // 「生产者看到 0 不 notify，worker 随后睡下」而丢唤醒。
        m_impl->sleeping_workers.fetch_add(1, std::memory_order_relaxed);
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
                if (m_impl->stopping() ||
                    m_impl->main_done.load(std::memory_order_acquire) ||
                    m_impl->runnable.load(std::memory_order_acquire) != 0) {
                    return true;
                }
                // 阻塞区结束时立刻唤醒补充 M 做退休判定，避免等满一个
                // idle_worker_timeout 才回收。
                return replacement && !idle_shrink &&
                       m_impl->blocking_workers.load(
                           std::memory_order_acquire) == 0;
            });
        m_impl->sleeping_workers.fetch_sub(1, std::memory_order_relaxed);
        // 退休判定与计数修改仍在同一个持锁临界区内：wait_for 返回后锁没有
        // 释放，因此这里的 runnable/blocking 观测即最终状态，不会出现
        // 「判定空闲后、置 kStopping 前」被新 G 插队的窗口。
        const auto current_floor = required_workers();
        const auto runnable_now =
            m_impl->runnable.load(std::memory_order_acquire);
        const auto blocking_now =
            m_impl->blocking_workers.load(std::memory_order_acquire);
        const bool stopping_now =
            m_impl->stopping();
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
            m_impl->needs_grow.store(true, std::memory_order_relaxed);
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
            m_impl->needs_grow.store(true, std::memory_order_relaxed);
        }
        if (own_processor.active_machines.load(std::memory_order_relaxed) == 0) {
            own_processor.state.store(
                m_impl->stopping()
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
