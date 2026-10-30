/**
 * @file parking_condition.cpp
 * @brief ParkingCondition 的等待/唤醒实现（侵入式队列 + 节点池 + per-node futex）。
 *
 * ## 这个文件实现什么
 *
 * 把 `parking_condition.hpp` 声明的"Fiber / 普通线程共用条件等待"落到具体
 * 机制上：
 *
 * - **Fiber 路径**：有 Scheduler 归属时 park G，唤醒走 wake_or_cancel；
 * - **线程路径**：每个 WaitNode 自己的 32 位 futex 代际字。notify_one 只推进
 *   选中节点（D2：共享字会唤醒错节点而丢唤醒）；notify_all 用 futex_waitv
 *   让等待者同时监听「节点字 + 共享广播字」，一次 FUTEX_WAKE(INT_MAX) 唤醒
 *   全部 native waiter，N 次系统调用降为 1 次（内核不支持时逐节点回退）；
 * - **park 前自旋**：kSpinBudget 次。无自旋时 2 个等待者场景实测比
 *   std::condition_variable 慢 3.3 倍；预算过大则白烧 CPU；
 * - **节点池（thread_local）**：一次 wait 不再堆分配 WaitNode，也不再为队列
 *   元素付 shared_ptr 引用计数。一个线程同一时刻最多只有一个活动等待节点
 *   （wait 是阻塞调用），所以池自然稳定在 1~2 个元素；池保留 shared_ptr 是
 *   为了让**定时器回调可以安全地弱引用**节点（回调迟到时 lock 仍成功，靠
 *   generation 判断是否还是同一代等待）；
 * - **侵入式双向链表**：出队/取消清理是 O(1)。旧实现用
 *   `std::deque<std::shared_ptr<WaitNode>>`，`cleanup` 要 `std::find` + `erase`
 *   中间元素（移动整段 deque），N 个等待者逐个被唤醒时退化成 O(N²)；
 * - **队列锁与节点门分离**：`m_queue_mutex` 只保护链表链接关系与计数（临界区
 *   只有几条指针赋值），节点结果字段是原子量，等待者自旋与通知者的
 *   `Finish` 都不需要队列锁；需要与 Scheduler 交互的 managed 唤醒走每个节点
 *   自己的 `m_wake_mutex`（Disarm 门）；
 * - **一次唤醒多个**：notify_all 在队列锁内 O(1) 摘整条链，锁外逐个 `Finish`，
 *   native 等待者由一次广播代际推进 + 一次 `FUTEX_WAKE(INT_MAX)` 覆盖；
 * - `FiberWaitUnsupported()`：平台能力探测，让上层在不支持时安全降级。
 *
 * ## 锁契约（最容易写错的地方）
 *
 * `wait*` 系列要求调用方**持有传入的外部 `unique_lock`**，返回时锁仍被持有
 * （与 `std::condition_variable` 一致）。实现内部会先释放再等待、被唤醒后
 * 重新获取，但**谓词判断永远在持锁状态下进行**，否则会出现丢失唤醒。
 *
 * ## 锁序
 *
 * 队列锁与节点门之间没有嵌套获取：入队/摘链只持队列锁，`Wake`/`Disarm`
 * 只持节点门，`cleanup` 先 `Disarm` 后取队列锁。因此不存在两条锁的环。
 */
#include "go2cpp/core/parking_condition.hpp"

#include "go2cpp/concurrency/core/futex_mutex.hpp"

#include "go2cpp/debug/debug_event_ring.hpp"
#include "go2cpp/debug/debug_resources.hpp"

#include "go2cpp/fiber.hpp"
#include "go2cpp/core/timer.hpp"
#include "go2cpp/scheduler.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <cerrno>
#include <climits>
#include <cstring>
#include <ctime>
#include <linux/futex.h>
#include <linux/time_types.h>
#include <sys/syscall.h>
#include <unistd.h>

// futex_waitv：Linux 5.16+ 系统调用，6.2+ 支持 FUTEX_PRIVATE_FLAG。
// 头文件或内核任一缺失就退回逐节点 FUTEX_WAKE。
#if defined(SYS_futex_waitv) && defined(FUTEX_32)
#define GO2CPP_PC_HAS_WAITV 1
#else
#define GO2CPP_PC_HAS_WAITV 0
#endif

namespace {

/// 在 futex 字上等待。
/// @return 0 被唤醒；-1 且 errno==EAGAIN 表示值已变；-1 且 errno==ETIMEDOUT 表示超时。
int FutexWait(std::atomic<std::uint32_t>* word, std::uint32_t expected,
              const std::optional<std::chrono::steady_clock::time_point>& deadline) {
    timespec rel{};
    const timespec* ts = nullptr;
    if (deadline) {
        const auto now = std::chrono::steady_clock::now();
        if (*deadline <= now) {
            errno = ETIMEDOUT;
            return -1;
        }
        const auto diff =
            std::chrono::duration_cast<std::chrono::nanoseconds>(*deadline - now);
        rel.tv_sec = static_cast<time_t>(diff.count() / 1000000000LL);
        rel.tv_nsec = static_cast<long>(diff.count() % 1000000000LL);
        ts = &rel;
    }
    return static_cast<int>(::syscall(SYS_futex,
                                      reinterpret_cast<std::uint32_t*>(word),
                                      FUTEX_WAIT_PRIVATE, expected, ts, nullptr, 0));
}

int FutexWake(std::atomic<std::uint32_t>* word, int n) {
    return static_cast<int>(::syscall(SYS_futex,
                                      reinterpret_cast<std::uint32_t*>(word),
                                      FUTEX_WAKE_PRIVATE, n, nullptr, nullptr, 0));
}

#if GO2CPP_PC_HAS_WAITV
/// 探测 futex_waitv 是否真的可用（不睡眠：val 不匹配会立刻 EAGAIN）。
/// 结果在进程内缓存一次；等待侧与唤醒侧必然读到同一个值。
bool FutexWaitvEnabled() noexcept {
    static const bool enabled = [] {
        std::uint32_t word = 0;
        struct futex_waitv entry;
        std::memset(&entry, 0, sizeof(entry));
        // 与 word 的 0 不匹配：内核支持且接受 private flag 时返回 EAGAIN。
        entry.val = 1;
        entry.uaddr = reinterpret_cast<std::uint64_t>(&word);
        entry.flags = FUTEX_32 | FUTEX_PRIVATE_FLAG;
        errno = 0;
        const long rc = ::syscall(SYS_futex_waitv, &entry, 1U, 0U, nullptr, 0);
        return rc == -1 && errno == EAGAIN;
    }();
    return enabled;
}

/// 同时等两个 futex 字：节点自己的字 + 条件对象的广播字。
/// futex_waitv 的 timeout 是「绝对时刻 + clockid」，与 FUTEX_WAIT 的相对超时不同，
/// 这里用 CLOCK_MONOTONIC 绝对时刻；十年以上按无限等待处理。
int FutexWaitvTwo(std::atomic<std::uint32_t>* first,
                  std::uint32_t first_expected,
                  std::atomic<std::uint32_t>* second,
                  std::uint32_t second_expected,
                  const std::optional<std::chrono::steady_clock::time_point>& deadline) {
    struct futex_waitv entries[2];
    std::memset(entries, 0, sizeof(entries));
    entries[0].val = first_expected;
    entries[0].uaddr = reinterpret_cast<std::uint64_t>(first);
    entries[0].flags = FUTEX_32 | FUTEX_PRIVATE_FLAG;
    entries[1].val = second_expected;
    entries[1].uaddr = reinterpret_cast<std::uint64_t>(second);
    entries[1].flags = FUTEX_32 | FUTEX_PRIVATE_FLAG;

    __kernel_timespec absolute{};
    const __kernel_timespec* ts = nullptr;
    if (deadline) {
        const auto remaining =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                *deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0) {
            errno = ETIMEDOUT;
            return -1;
        }
        constexpr long long kForeverNs = 10LL * 365 * 24 * 3600 * 1000000000LL;
        if (remaining.count() < kForeverNs) {
            timespec mono{};
            (void)::clock_gettime(CLOCK_MONOTONIC, &mono);
            long long seconds = static_cast<long long>(mono.tv_sec) +
                                remaining.count() / 1000000000LL;
            long long nanos = static_cast<long long>(mono.tv_nsec) +
                              remaining.count() % 1000000000LL;
            if (nanos >= 1000000000LL) {
                ++seconds;
                nanos -= 1000000000LL;
            }
            absolute.tv_sec = seconds;
            absolute.tv_nsec = nanos;
            ts = &absolute;
        }
    }
    return static_cast<int>(::syscall(SYS_futex_waitv, entries, 2U, 0U,
                                      const_cast<__kernel_timespec*>(ts),
                                      CLOCK_MONOTONIC));
}
#else
/// 编译期没有 futex_waitv：保持旧的逐节点 FUTEX_WAIT/WAKE。
bool FutexWaitvEnabled() noexcept { return false; }
#endif

}  // namespace
#endif  // __linux__

namespace go2cpp::core {

namespace {
/// FUTEX_WAIT 前的自旋预算（约 2048 * 0.32ns ≈ 0.65us）。
/// 目标是覆盖「唤醒已在路上」的窗口：实测 WSL2 上一次 futex 唤醒往返约
/// 20-30us，而自旋命中只需 0.65us，命中时 pc-rtt 从 ~30us 降到 ~9-17us。
/// 代价是每次真正需要睡眠的 park 多烧约 0.65us CPU（见 /tmp/perf_pc.md）。
constexpr int kSpinBudget = 2048;

/// 节点复用代际：每次 `Reset` 分配一个新值。定时器回调捕获注册时的代际，
/// 迟到回调只有在代际仍然匹配时才允许改写结果 —— 这保证「失效回调」不会
/// 把**下一位等待者**的节点提前置成 kTimedOut。
///
/// 为什么用进程级计数器而不是节点内自增：节点来自**线程本地**池，可能被
/// 同一线程带到另一个 ParkingCondition 上复用，只有全局唯一值才能区分
/// 「同一个节点对象的不同代等待」。
std::atomic<std::uint64_t> g_next_node_generation{1};
}  // namespace

/**
 * @brief 单个等待者的节点（一个 `wait*` 调用对应一个）。
 *
 * ## 定位
 *
 * `ParkingCondition::wait*` 在阻塞前从线程本地池取出它：受管 G 把它绑到
 * `Fiber::Suspend` 的 continuation 上，普通线程把它挂到侵入式等待队列。
 * 唤醒方（`notify_one/notify_all`、定时器）通过 `Finish(...)` 认领它。
 *
 * ## 状态机（四态、单向）
 *
 * ```text
 * kWaiting -(Finish(kNotified))->  kNotified
 * kWaiting -(Finish(kTimedOut))->  kTimedOut
 * kWaiting -(Finish(kCancelled))-> kCancelled
 * ```
 *
 * `Finish` 用 CAS，**只有第一个调用者成功** —— 与 `io::IOManager` 的 `claim`
 * 是同一模式：一个等待者只能被一个源完成，重复唤醒会让同一个 G 被两个 worker
 * 执行。`result()` 在被唤醒后只读。
 *
 * ## Disarm 门（为什么 Wake 要加锁）
 *
 * managed 的 `Wake()` 持 `m_wake_mutex` 并检查 `m_active`：`Disarm` 会等待
 * 所有在途回调离开这道门后才允许等待者的栈被回收。否则会出现「Scheduler
 * 指针已随 G 的栈失效、回调仍拿它调 `wake_or_cancel`」的 UAF。
 * native 等待者只推进节点私有的 futex 字，不碰 Scheduler 指针，因此不需要
 * 这道门（`Disarm` 不写这两个原子字）。
 */
class ParkingCondition::WaitNode final {
public:
    /// @brief 等待结果四态；`kWaiting` 是唯一可被 Finish 改写的初态。
    enum class Result { kWaiting, kNotified, kTimedOut, kCancelled };

    WaitNode() = default;
    ~WaitNode() = default;
    WaitNode(const WaitNode&) = delete;
    WaitNode& operator=(const WaitNode&) = delete;

    /**
     * @brief 从线程本地池取一个节点（稳态零分配）。
     * @param owner 等待者所属的条件变量（只用于 `m_broadcast` 访问）。
     * @param scheduler 受管等待的调度器；native 等待为 nullptr。
     * @param task 受管等待的 Task；native 等待为空。
     * @note 取出的节点已经 `Reset`：结果回到 kWaiting、代际已刷新。
     */
    static std::shared_ptr<WaitNode> Acquire(ParkingCondition* owner,
                                             Scheduler* scheduler,
                                             std::shared_ptr<Task> task) {
        if (!t_pool().nodes.empty()) {
            std::shared_ptr<WaitNode> node = std::move(t_pool().nodes.back());
            t_pool().nodes.pop_back();
            node->Reset(owner, scheduler, std::move(task));
            return node;
        }
        if (std::shared_ptr<WaitNode> recycled = TakeOrphan()) {
            recycled->Reset(owner, scheduler, std::move(task));
            return recycled;
        }
        auto node = std::make_shared<WaitNode>();
        node->Reset(owner, scheduler, std::move(task));
        return node;
    }

    /// @brief 把节点放回线程本地池；超出容量时交给 shared_ptr 释放。
    /// @note 归还前 `Reset(nullptr, ...)` 清空 Scheduler/Task 引用，不延长用户
    ///       对象生命期；定时器回调的 weak_ptr 仍可能锁住这个节点，但它们只能
    ///       看到空 owner 与已变更的代际，无法影响下一轮等待。
    static void Release(std::shared_ptr<WaitNode> node) noexcept {
        if (!node) {
            return;
        }
        node->Reset(nullptr, nullptr, {});
        // 池不设上限：一个线程同一时刻只有一个活动等待节点，正常只会缓存
        // 1 个。保留（而不是销毁）对象是安全前提 —— notify 的唤醒动作在
        // 队列锁外执行，可能晚于等待者返回，此时节点地址必须仍然有效。
        t_pool().nodes.push_back(std::move(node));
    }

    /// @brief 追加到等待队列尾部；调用方必须持有条件变量的队列锁。
    static void LinkTail(ParkingCondition* owner, WaitNode* node) noexcept {
        node->m_prev = owner->m_tail;
        node->m_next = nullptr;
        if (owner->m_tail != nullptr) {
            owner->m_tail->m_next = node;
        } else {
            owner->m_head = node;
        }
        owner->m_tail = node;
        node->m_linked = true;
        ++owner->m_waiter_count;
    }

    /// @brief O(1) 摘链；节点不在队列里时无副作用。调用方必须持有队列锁。
    static void Unlink(ParkingCondition* owner, WaitNode* node) noexcept {
        if (node == nullptr || !node->m_linked) {
            return;
        }
        if (node->m_prev != nullptr) {
            node->m_prev->m_next = node->m_next;
        } else {
            owner->m_head = node->m_next;
        }
        if (node->m_next != nullptr) {
            node->m_next->m_prev = node->m_prev;
        } else {
            owner->m_tail = node->m_prev;
        }
        node->m_prev = nullptr;
        node->m_next = nullptr;
        node->m_linked = false;
        --owner->m_waiter_count;
    }

    /// @brief 摘除队首节点；队列为空返回 nullptr。调用方必须持有队列锁。
    static WaitNode* PopFront(ParkingCondition* owner) noexcept {
        WaitNode* const node = owner->m_head;
        if (node != nullptr) {
            Unlink(owner, node);
        }
        return node;
    }

    /// @brief 链表后继；只在遍历已摘链的节点链时使用。
    WaitNode* next() const noexcept { return m_next; }

    bool Finish(Result result) noexcept {
        auto expected = Result::kWaiting;
        return m_hot.result.compare_exchange_strong(
            expected, result, std::memory_order_acq_rel);
    }

    Result result() const noexcept {
        return m_hot.result.load(std::memory_order_acquire);
    }

    bool IsNative() const noexcept {
        return !m_managed.load(std::memory_order_acquire);
    }

    /// Doctor 等待图用：该节点对应的受管 G 编号；native 节点恒为 0。
    /// 只读一个构造/Reset 时写入的原子快照，不做所有权转移。
    std::uint64_t gid() const noexcept {
        return m_gid.load(std::memory_order_relaxed);
    }

    /// @brief 本代等待的唯一编号（定时器回调据此判断是否已被复用）。
    std::uint64_t generation() const noexcept { return m_generation; }

    void Wake() noexcept {
        if (IsNative()) {
#if defined(__linux__)
            // native 等待者只推进节点自己的代际字并定向 FUTEX_WAKE，不碰
            // Scheduler 指针，因此无需 m_wake_mutex（Disarm 不写这两个原子字）。
            m_hot.futex_seq.fetch_add(1, std::memory_order_release);
            if (m_hot.futex_waiters.load(std::memory_order_acquire) != 0) {
                (void)FutexWake(&m_hot.futex_seq, 1);
            }
#else
            std::lock_guard<FutexMutex> lock(m_wake_mutex);
            m_native_cv.notify_one();
#endif
            return;
        }
        // Disarm waits for any in-flight callback to leave this gate, so no
        // raw Scheduler access survives the waiting G stack cleanup.
        std::lock_guard<FutexMutex> lock(m_wake_mutex);
        if (!m_active) {
            return;
        }
        WakeManagedLocked();
    }

    void Disarm() noexcept {
        std::lock_guard<FutexMutex> lock(m_wake_mutex);
        m_active = false;
    }

    /**
     * @brief 定时器到期的落点（可能在等待者返回很久之后被执行）。
     * @param generation 注册回调时节点的代际。
     *
     * 只有代际仍然匹配才允许认领 —— 节点已放回池并被下一次 wait 取走时，
     * 旧回调绝不能把新等待的 result 改成 kTimedOut。
     */
    void TimeoutFromTimer(std::uint64_t generation) noexcept {
        std::lock_guard<FutexMutex> lock(m_wake_mutex);
        if (generation != m_generation) {
            return;
        }
        if (!Finish(Result::kTimedOut)) {
            return;
        }
        // native 节点的唤醒只需要推进它自己的 futex 字（与 managed 不同，
        // 它不经过 Disarm 门）；managed 节点只有在已 Arm/入场时才需要真正
        // 唤醒，未入场时发布终态即可，等待者会在 park 前看到。
        if (IsNative()) {
            WakeNativeLocked();
        } else if (m_active) {
            WakeManagedLocked();
        }
    }

    Result WaitNative(std::unique_lock<std::mutex>& outer,
                      std::optional<Clock::time_point> deadline) {
#if defined(__linux__)
        // 双字等待：节点字（notify_one 定向唤醒）+ 广播字（notify_all 一次唤醒全部）。
        // 三个关键点（第一版曾因此崩溃/丢唤醒）：
        //   1. 每轮循环先判终态，不能只依赖 futex 的返回码；
        //   2. 真正睡眠前用 acquire 重读两个代际字并再判一次终态：只要读到唤醒方
        //      推进后的值，release 侧对 result 的写在 acquire 下必然可见；
        //   3. 区分 EAGAIN（值已变，继续等）与 ETIMEDOUT（真超时）。
        const bool use_waitv = FutexWaitvEnabled();
        std::uint32_t expected = m_hot.futex_seq.load(std::memory_order_acquire);
        std::uint32_t expected_broadcast = 0;
        m_hot.futex_waiters.fetch_add(1, std::memory_order_acq_rel);
        if (use_waitv) {
            m_owner->m_broadcast.waiters.fetch_add(1, std::memory_order_acq_rel);
        }
        outer.unlock();
        for (;;) {
            if (result() != Result::kWaiting) {
                break;
            }
            if (deadline && Clock::now() >= *deadline) {
                (void)Finish(Result::kTimedOut);
                break;
            }
            // 短暂自旋：无竞争时（唤醒就在眼前）完全避免睡眠系统调用。
            bool spun_through = false;
            for (int spin = 0; spin < kSpinBudget; ++spin) {
                if (result() != Result::kWaiting) {
                    break;
                }
                if (m_hot.futex_seq.load(std::memory_order_acquire) != expected) {
                    spun_through = true;
                    break;
                }
                if (use_waitv &&
                    m_owner->m_broadcast.seq.load(std::memory_order_acquire) !=
                        expected_broadcast) {
                    spun_through = true;
                    break;
                }
                // deadline 每 128 次自旋查一次：clock_gettime 走 vDSO 也要
                // ~20ns，2048 次自旋若每 16 次查一次就是 2.6us 白烧；128 次的
                // 超时精度损失 < 50ns，相对一次 futex 唤醒（~20us）可忽略。
                if (deadline && (spin & 127) == 0 && Clock::now() >= *deadline) {
                    break;
                }
                std::atomic_signal_fence(std::memory_order_seq_cst);
            }
            if (result() != Result::kWaiting) {
                break;
            }
            if (spun_through) {
                expected = m_hot.futex_seq.load(std::memory_order_acquire);
                if (use_waitv) {
                    expected_broadcast =
                        m_owner->m_broadcast.seq.load(std::memory_order_acquire);
                }
                continue;
            }
            if (deadline && Clock::now() >= *deadline) {
                (void)Finish(Result::kTimedOut);
                break;
            }
            expected = m_hot.futex_seq.load(std::memory_order_acquire);
            if (use_waitv) {
                expected_broadcast =
                    m_owner->m_broadcast.seq.load(std::memory_order_acquire);
            }
            if (result() != Result::kWaiting) {
                break;
            }
            errno = 0;
            int rc;
            if (use_waitv) {
                rc = FutexWaitvTwo(&m_hot.futex_seq, expected,
                                   &m_owner->m_broadcast.seq, expected_broadcast,
                                   deadline);
            } else {
                rc = FutexWait(&m_hot.futex_seq, expected, deadline);
            }
            if (result() != Result::kWaiting) {
                break;
            }
            if (rc == 0) {
                continue;  // 被唤醒：重判终态
            }
            if (errno == EAGAIN || errno == ETIMEDOUT || errno == EINTR) {
                // 值已变/超时/信号打断：交给下一轮循环判终态与 deadline
                continue;
            }
            // 其它错误：保守地再判一次终态，仍等待就按超时处理，绝不空转。
            if (result() == Result::kWaiting) {
                (void)Finish(Result::kTimedOut);
            }
            break;
        }
        if (use_waitv) {
            m_owner->m_broadcast.waiters.fetch_sub(1, std::memory_order_acq_rel);
        }
        m_hot.futex_waiters.fetch_sub(1, std::memory_order_acq_rel);
        // 注意：不重新加锁。WaitOnce 的契约是「WaitNative 返回时外部锁仍处于
        // 解锁状态，由 WaitOnce 自己关锁」。
        return result();
#else
        std::unique_lock<FutexMutex> lock(m_wake_mutex);
        outer.unlock();
        if (deadline) {
            if (!m_native_cv.wait_until(lock, *deadline, [this] {
                    return result() != Result::kWaiting;
                })) {
                (void)Finish(Result::kTimedOut);
            }
        } else {
            m_native_cv.wait(lock, [this] {
                return result() != Result::kWaiting;
            });
        }
        return result();
#endif
    }

    Result WaitManaged() {
        Scheduler* const scheduler = m_scheduler;
        const auto task = m_task;
        // 原语提示：外层（channel/mutex）已设置时不会被覆盖。
        GO2CPP_DEBUG_PARK_HINT(kParkingCondition);
        for (;;) {
            // Notify 可能在当前 G 进入 park 前完成。终态已经通过
            // WaitNode::Finish 发布时，直接返回可跳过 Scheduler admission
            // 锁和一次无意义的挂起/恢复，减少 Fiber/线程混合条件变量的
            // 短等待开销，同时不改变 notify-before-park 的语义。
            const Result published = result();
            if (published != Result::kWaiting) {
                return published;
            }
            if (task->cancellation_requested()) {
                (void)Finish(Result::kCancelled);
                return result();
            }
            // A notifier can run before park. Always consume its pending
            // wake permit, even when the terminal result is visible already.
            const bool suspended = scheduler->park_wait(task);
            if (result() != Result::kWaiting) {
                return result();
            }
            if (task->cancellation_requested() ||
                (!suspended && !scheduler->is_running()) ||
                Scheduler::current_scheduler() != scheduler ||
                Scheduler::current_task().get() != task.get()) {
                (void)Finish(Result::kCancelled);
                return result();
            }
            // Unrelated wake permits and unparks are spurious. Re-park the
            // same published node; never busy-yield to emulate waiting.
        }
    }

private:
    /**
     * @brief 线程本地节点池。
     *
     * ## 为什么析构时不能释放节点（TSan 实测的 UAF）
     *
     * `notify_all` 在**队列锁外**对已认领的节点调用 `IsNative()/Wake()`；
     * 这期间等待者可能已经返回并把节点还回自己的 TLS 池。如果该等待线程
     * 恰好此时退出，TLS 池析构会释放节点内存，而通知方仍在读它的原子字段
     * —— ThreadSanitizer 报的正是这条链路（`__call_tls_dtors` 里的
     * `operator delete` 与 `notify_all` 的 `IsNative()` 竞争）。
     *
     * 因此池析构时把节点**移交**给进程级回收站（不释放），后续新线程可以
     * 复用；节点复用仍然靠 generation 区分代次，不改变任何语义。
     */
    struct LocalPool {
        std::vector<std::shared_ptr<WaitNode>> nodes;
        ~LocalPool() { HandOffToOrphanPool(nodes); }
    };

    /// @brief 线程本地节点池（只被本线程的 Acquire/Release 访问）。
    static LocalPool& t_pool() {
        static thread_local LocalPool pool;
        return pool;
    }

    /// @brief 进程级回收站：接收线程退出时移交的节点，永不释放。
    static std::vector<std::shared_ptr<WaitNode>>& OrphanPool() noexcept {
        static auto* pool = new std::vector<std::shared_ptr<WaitNode>>();
        return *pool;
    }

    static std::mutex& OrphanMutex() noexcept {
        static std::mutex mutex;
        return mutex;
    }

    /// @brief 把本地池里的节点整体移交给回收站（线程退出路径）。
    static void HandOffToOrphanPool(
        std::vector<std::shared_ptr<WaitNode>>& nodes) noexcept {
        if (nodes.empty()) {
            return;
        }
        std::lock_guard<std::mutex> lock(OrphanMutex());
        auto& orphan = OrphanPool();
        for (auto& node : nodes) {
            orphan.push_back(std::move(node));
        }
        nodes.clear();
    }

    /// @brief 从回收站取一个节点（本地池空时才走，避免反复给新线程分配）。
    static std::shared_ptr<WaitNode> TakeOrphan() noexcept {
        std::lock_guard<std::mutex> lock(OrphanMutex());
        auto& orphan = OrphanPool();
        if (orphan.empty()) {
            return nullptr;
        }
        std::shared_ptr<WaitNode> node = std::move(orphan.back());
        orphan.pop_back();
        return node;
    }

    /// @brief 复位到「一次全新等待」的状态，并分配新代际。
    /// @note 持 `m_wake_mutex`：与在途的 managed Wake / 超时回调互斥，
    ///       保证新等待者不会读到上一代的 scheduler/task。
    void Reset(ParkingCondition* owner, Scheduler* scheduler,
               std::shared_ptr<Task> task) noexcept {
        std::lock_guard<FutexMutex> lock(m_wake_mutex);
        m_owner = owner;
        m_scheduler = scheduler;
        m_task = std::move(task);
        m_managed.store(scheduler != nullptr && static_cast<bool>(m_task),
                        std::memory_order_relaxed);
        m_gid.store(m_task ? static_cast<std::uint64_t>(m_task->id()) : 0U,
                    std::memory_order_relaxed);
        m_hot.result.store(Result::kWaiting, std::memory_order_release);
        m_hot.futex_seq.store(0, std::memory_order_relaxed);
        m_hot.futex_waiters.store(0, std::memory_order_relaxed);
        m_generation = g_next_node_generation.fetch_add(
            1, std::memory_order_relaxed);
        m_next = nullptr;
        m_prev = nullptr;
        m_linked = false;
        // 入队即可被通知：m_active 在 Reset 时就置位，避免「节点已进队列、
        // 但还没 Arm，通知方的 managed Wake 看到 m_active=false 而丢唤醒」。
        m_active = true;
    }

    /// @brief 调用方必须持有 `m_wake_mutex`，且已确认 `m_active`。
    void WakeManagedLocked() noexcept {
        if (m_scheduler != nullptr && m_task) {
            try {
                (void)m_scheduler->wake_or_cancel(m_task);
            } catch (...) {
                // Scheduler 的可靠唤醒交接会保留所有已经启动的 G。
            }
        }
    }

    /// @brief native 路径的唤醒字推进（无锁语义，不读 scheduler/task）。
    void WakeNativeLocked() noexcept {
#if defined(__linux__)
        m_hot.futex_seq.fetch_add(1, std::memory_order_release);
        if (m_hot.futex_waiters.load(std::memory_order_acquire) != 0) {
            (void)FutexWake(&m_hot.futex_seq, 1);
        }
#endif
    }

    ParkingCondition* m_owner{nullptr};
    Scheduler* m_scheduler{nullptr};
    std::shared_ptr<Task> m_task;
    /// @brief 是否受管（有 Scheduler 且有 Task）。原子快照：Wake 在锁外读它
    ///        决定走哪条路径，而 Reset 在门内改写。
    std::atomic<bool> m_managed{false};
    /// @brief 受管等待者的 G 编号；native/未知为 0（调试事件用）。
    std::atomic<std::uint64_t> m_gid{0};
    /// @brief 本代等待编号（只在 Reset/超时回调里读写，不在热路径上）。
    std::uint64_t m_generation{0};
    /// @brief 侵入式链表指针（只在队列锁内读写）。
    WaitNode* m_next{nullptr};
    WaitNode* m_prev{nullptr};
    bool m_linked{false};
    // 等待/唤醒热路径的字段放在同一条 cacheline：等待者自旋同时读 result 与
    // seq，唤醒者写完 result 立刻推进 seq；同一行只需要一次行所有权转移，
    // 也避免 node 之间的 false sharing（对象本身 64 字节对齐）。
    struct alignas(64) HotState {
        std::atomic<Result> result{Result::kWaiting};
        // per-node futex：notify_one 只推进**选中节点**自己的代际字并只唤醒它。
        // 共享代际字时 FUTEX_WAKE 会唤醒任意等待者，被误唤醒的节点看到
        // result 仍是 kWaiting 就重新 park，而选中节点已摘出队列，
        // notify_all 也救不了它（审计 D2，多 native waiter 下 ~0.125%/轮）。
        std::atomic<std::uint32_t> futex_seq{0};
        std::atomic<std::uint32_t> futex_waiters{0};
    } m_hot;
    FutexMutex m_wake_mutex;
#if !defined(__linux__)
    std::condition_variable_any m_native_cv;
#endif
    bool m_active{false};
};

ParkingConditionDebugSnapshot ParkingCondition::DebugSnapshot() const {
    ParkingConditionDebugSnapshot snapshot;
    std::lock_guard<FutexMutex> lock(m_queue_mutex);
    snapshot.waiters = m_waiter_count;
    snapshot.nodes.reserve(m_waiter_count);
    std::size_t index = 0;
    for (WaitNode* node = m_head; node != nullptr; node = node->next(), ++index) {
        ParkingWaiterDebugSnapshot item;
        item.index = index;
        item.native = node->IsNative();
        item.state = static_cast<int>(node->result());
        item.gid = node->gid();
        if (item.native) {
            ++snapshot.native_waiters;
        } else {
            ++snapshot.fiber_waiters;
        }
        if (item.state == 0) {
            ++snapshot.waiting_nodes;
        } else if (item.state == 1) {
            ++snapshot.notified_nodes;
        }
        snapshot.nodes.push_back(item);
    }
    snapshot.broadcast_seq = m_broadcast.seq.load(std::memory_order_relaxed);
    snapshot.broadcast_waiters =
        m_broadcast.waiters.load(std::memory_order_relaxed);
    return snapshot;
}

ParkingCondition::ParkingCondition() {
#if defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP
    GO2CPP_DEBUG_RESOURCE_REGISTER(kParkingCondition, this, nullptr);
#endif
}
ParkingCondition::~ParkingCondition() {
#if defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP
    GO2CPP_DEBUG_RESOURCE_UNREGISTER_ADDR(this);
#endif
}

bool ParkingCondition::CancellationRequested() noexcept {
    const auto task = Scheduler::current_task();
    return task && task->cancellation_requested();
}

bool ParkingCondition::FiberWaitUnsupported() noexcept {
    return Fiber::Current() != nullptr && !Scheduler::current_task();
}

/**
 * @brief 唤醒**一个**等待者；没有等待者时什么也不做。
 *
 * ## 步骤
 *
 * step1. 在 `m_queue_mutex` 下从队首逐个取节点，用 `Finish(kNotified)` 抢占
 *        结果——只有**第一个抢占成功**的节点才是本次被唤醒者（其余可能已被
 *        超时/取消）；僵尸节点顺手摘除；
 * step2. 释放队列锁后调用 `selected->Wake()`：唤醒动作不能持队列锁，
 *        否则会与等待者重新获取外部锁形成锁序反转。
 *
 * ## 并发/失败语义
 *
 * - **抢占式**：`Finish()` 用 CAS 决定唯一结果，超时线程与唤醒线程只有一个能赢；
 * - `Finish()` 失败的节点（已被超时/取消）会被跳过并继续找下一个，
 *   因此 `notify_one` 不会因为队首是「僵尸节点」而丢失唤醒；
 * - 队列锁的临界区只有「摘指针 + 一次 CAS」，节点热字段的争用不会扩大到
 *   整个队列；
 * - `noexcept`：唤醒路径不允许抛异常（它在解锁路径上，抛异常会破坏状态）。
 */
void ParkingCondition::notify_one() noexcept {
    WaitNode* selected = nullptr;
    // DEBUG_DUMP 关闭时只用于事件参数，显式标记避免 -Wunused-but-set-variable。
    [[maybe_unused]] std::uint32_t waiters_after = 0;
    {
        std::lock_guard<FutexMutex> lock(m_queue_mutex);
        while (WaitNode* node = WaitNode::PopFront(this)) {
            if (node->Finish(WaitNode::Result::kNotified)) {
                selected = node;
                break;
            }
        }
        waiters_after = static_cast<std::uint32_t>(m_waiter_count);
    }
    if (selected != nullptr) {
#if defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP
        GO2CPP_DEBUG_RESOURCE_EVENT(
            kPCNotify, GO2CPP_DEBUG_RESOURCE_ID(this),
            GO2CPP_DEBUG_BOUND_TASK(), selected->gid(), 0,
            ::go2cpp::debug::ResourceResult::kWake, waiters_after, true);
#endif
        GO2CPP_DEBUG_WAKE_HINT(kParkingCondition);
        selected->Wake();
    }
}

/**
 * @brief 唤醒**所有**当前等待者（批量路径）。
 *
 * ## 步骤
 *
 * step1. 在队列锁内 O(1) 摘走整条链（`DetachAll`）：只做指针交换与
 *        `linked` 标记，临界区长度与等待者数量无关；
 * step2. 锁外逐个 `Finish(kNotified)`：被摘走的节点只属于本次通知，不会与
 *        其它通知者竞争，所以不需要在锁内做 N 次 CAS；
 * step3. managed 节点逐个 `Wake()`（交给 Scheduler）；native 节点由一次广播
 *        代际推进 + 一次 `FUTEX_WAKE(INT_MAX)` 覆盖（futex_waitv 方案；
 *        不可用时逐节点 Wake）。已被超时/取消的节点不会重复唤醒。
 *
 * ## 并发/失败语义
 *
 * - 摘链后新来的等待者进入空队列，不会被本次 `notify_all` 漏掉；
 * - 与 `notify_one` 一致：唤醒在锁外，避免锁序反转；
 * - `noexcept`。
 */
void ParkingCondition::notify_all() noexcept {
    // 已摘链且被本次通知认领成功的节点。**必须用局部容器记录**：不能在锁外
    // 沿 `next()` 遍历已摘链 —— 等待者随时可能 cleanup 并把节点放回池，
    // `Reset` 会清掉它的 next 指针，遍历会漏掉后面的节点（丢唤醒），
    // 甚至顺着被复用的地址走进无关内存。
    static thread_local std::vector<WaitNode*> t_wake_list;
    t_wake_list.clear();
    [[maybe_unused]] std::size_t detached_count = 0;
    std::size_t native_notified = 0;
    {
        std::lock_guard<FutexMutex> lock(m_queue_mutex);
        detached_count = m_waiter_count;
        t_wake_list.reserve(detached_count);
        // 摘链与认领都在锁内：等待者的 cleanup 也要拿这把锁，所以节点在
        // 我们的临界区里不会消失；锁外只剩「唤醒」动作，不再读链表。
        while (WaitNode* node = WaitNode::PopFront(this)) {
            if (!node->Finish(WaitNode::Result::kNotified)) {
                continue;  // 已被超时/取消认领的僵尸节点
            }
            if (node->IsNative()) {
                ++native_notified;
            }
            t_wake_list.push_back(node);
        }
    }

    // 有 futex_waitv 时，native 节点由「一次广播代际推进 + 一次 FUTEX_WAKE」
    // 覆盖，不再逐个 wake；managed (Fiber) 节点仍必须逐个交给 Scheduler。
    bool broadcast = false;
#if defined(__linux__)
    broadcast = native_notified != 0 && FutexWaitvEnabled();
#endif
    GO2CPP_DEBUG_EVENT(kPCNotifyAll, 0,
                       static_cast<std::uint64_t>(detached_count));
    GO2CPP_DEBUG_RESOURCE_EVENT(
        kPCNotifyAll, GO2CPP_DEBUG_RESOURCE_ID(this),
        GO2CPP_DEBUG_BOUND_TASK(), 0,
        static_cast<std::uint64_t>(detached_count),
        ::go2cpp::debug::ResourceResult::kWake,
        static_cast<std::uint32_t>(detached_count), true);
    GO2CPP_DEBUG_WAKE_HINT(kParkingCondition);
    for (WaitNode* node : t_wake_list) {
        // 锁外重读 IsNative()：节点可能已被等待者回收复用，读到的是下一代的
        // 类型。这只影响「是否跳过逐个唤醒」，不会误唤醒本次通知的对象
        // （本次对象已由锁内的 Finish 认领、终态不会被改写）。
        if (broadcast && node->IsNative()) {
            continue;
        }
        node->Wake();
    }
    if (broadcast) {
#if defined(__linux__)
        m_broadcast.seq.fetch_add(1, std::memory_order_release);
        if (m_broadcast.waiters.load(std::memory_order_acquire) != 0) {
            (void)FutexWake(&m_broadcast.seq, INT_MAX);
        }
#endif
    }
}

/**
 * @brief 在条件上等待一次（可带超时与谓词），返回本次等待的结局。
 * @param lock 调用方持有的外部互斥量；等待期间会被解锁，返回时保证重新持有。
 * @param deadline 绝对超时点；`nullopt` 表示无限等待。
 * @param predicate 谓词；在**入队前**会被重检一次，为真则直接返回 `kNotified`。
 * @return `kNotified` / `kTimedOut` / `kCancelled`。
 *
 * ## 步骤
 *
 * step1. 判定是否为「受管等待」（有 Scheduler 且有当前 Task）。手动 Fiber
 *        没有调度器归属时无法安全 park，直接返回 `kCancelled`，绝不阻塞
 *        carrier 线程；
 * step2. 提前返回两类情况：已请求取消 → `kCancelled`；已过 deadline → `kTimedOut`；
 * step3. 从线程本地池取节点，在 `m_queue_mutex` 下**重检谓词**（为真则不入队，
 *        直接 `kNotified`），否则入队——重检是必须的，因为取消不持有外部锁，
 *        存在「通知先于入队」的窗口；
 * step4. 定义 `cleanup`：抢结果 `kCancelled`、撤 timer、从队列摘除。它在
 *        正常返回、超时、异常三条路径上都必须执行；
 * step5. 若受管且有 deadline，注册超时 timer（到期时按代际认领节点并唤醒）；
 * step6. **解锁外部 mutex** 后进入等待：受管走 `WaitManaged()`（Fiber park），
 *        否则走 `WaitNative()`（阻塞线程）；
 * step7. 等待返回后 `cleanup()` 并**重新加锁**，最后把 `WaitNode::Result`
 *        映射为 `WaitStatus`。
 */
ParkingCondition::WaitStatus ParkingCondition::WaitOnce(
    std::unique_lock<std::mutex>& lock,
    std::optional<Clock::time_point> deadline,
    const std::function<bool()>& predicate) {
    Scheduler* const scheduler = Scheduler::current_scheduler();
    const auto task = Scheduler::current_task();
    const bool managed = scheduler != nullptr && static_cast<bool>(task);
    if (!managed && Fiber::Current() != nullptr) {
        // 没有 Scheduler 归属的手动 Fiber 无法安全 park；保持外部
        // mutex 锁定并返回取消状态，避免阻塞 carrier OS 线程。
        return WaitStatus::kCancelled;
    }
    if (managed && task->cancellation_requested()) {
        return WaitStatus::kCancelled;
    }
    if (deadline && Clock::now() >= *deadline) {
        return WaitStatus::kTimedOut;
    }

    std::shared_ptr<WaitNode> node_holder = WaitNode::Acquire(
        this, managed ? scheduler : nullptr, managed ? task : nullptr);
    WaitNode* const node = node_holder.get();

    bool enqueued = false;
    [[maybe_unused]] std::uint32_t waiters_after = 0;
    {
        std::lock_guard<FutexMutex> queue_lock(m_queue_mutex);
        // Recheck after acquiring the notification lock. Context cancellation
        // does not hold the channel's external lock, so this second check is
        // required to close its notify-before-publication window.
        if (!predicate()) {
            WaitNode::LinkTail(this, node);
            enqueued = true;
            waiters_after = static_cast<std::uint32_t>(m_waiter_count);
        }
    }
    if (!enqueued) {
        // 节点从未入队：直接归还池，不注册定时器、不动队列。
        WaitNode::Release(std::move(node_holder));
        return WaitStatus::kNotified;
    }
#if defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP
    const go2cpp::debug::ResourceId debug_resource =
        GO2CPP_DEBUG_RESOURCE_ID(this);
    const std::uint64_t debug_gid =
        task ? static_cast<std::uint64_t>(task->id()) : 0ULL;
    GO2CPP_DEBUG_RESOURCE_EVENT(
        kPCWait, debug_resource, debug_gid, 0, 0,
        ::go2cpp::debug::ResourceResult::kBlocked, waiters_after, true);
    const auto finish_wait = [&](WaitStatus status) {
        go2cpp::debug::ResourceResult result =
            go2cpp::debug::ResourceResult::kCancelled;
        switch (status) {
            case WaitStatus::kNotified:
                result = go2cpp::debug::ResourceResult::kOk;
                break;
            case WaitStatus::kTimedOut:
                result = go2cpp::debug::ResourceResult::kTimeout;
                break;
            case WaitStatus::kCancelled:
                result = go2cpp::debug::ResourceResult::kCancelled;
                break;
        }
        GO2CPP_DEBUG_RESOURCE_EVENT(
            kPCWait, debug_resource, debug_gid, 0, 0, result, 0, true);
        return status;
    };
#else
    const auto finish_wait = [](WaitStatus status) { return status; };
#endif

    Timer timer;
    const auto cleanup = [&] {
        (void)node->Finish(WaitNode::Result::kCancelled);
        node->Disarm();
        timer.Cancel();
        std::lock_guard<FutexMutex> queue_lock(m_queue_mutex);
        WaitNode::Unlink(this, node);
    };

    WaitNode::Result result;
    try {
        if (managed && deadline) {
            const std::weak_ptr<WaitNode> weak(node_holder);
            const std::uint64_t generation = node->generation();
            timer = TimerService::Default().Schedule(*deadline, [weak, generation] {
                if (auto current = weak.lock()) {
                    current->TimeoutFromTimer(generation);
                }
            });
        }
        if (managed) {
            lock.unlock();
            result = node->WaitManaged();
        } else {
            result = node->WaitNative(lock, deadline);
        }
        cleanup();
        lock.lock();
    } catch (...) {
        cleanup();
        WaitNode::Release(std::move(node_holder));
        if (!lock.owns_lock()) {
            lock.lock();
        }
        throw;
    }
    WaitNode::Release(std::move(node_holder));
    switch (result) {
    case WaitNode::Result::kNotified:
        return finish_wait(WaitStatus::kNotified);
    case WaitNode::Result::kTimedOut:
        return finish_wait(WaitStatus::kTimedOut);
    case WaitNode::Result::kWaiting:
    case WaitNode::Result::kCancelled:
        return finish_wait(WaitStatus::kCancelled);
    }
    return finish_wait(WaitStatus::kCancelled);
}

}  // namespace go2cpp::core
