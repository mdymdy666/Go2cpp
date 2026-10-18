/**
 * @file parking_condition.cpp
 * @brief ParkingCondition 的 futex 无状态等待实现。
 *
 * ## 这个文件实现什么
 *
 * 把 `parking_condition.hpp` 声明的"Fiber / 普通线程共用条件等待"落到具体
 * 机制上：
 *
 * - **Fiber 路径**：Linux futex 无状态等待。用 32 位代际字 `m_futex_seq` 作为
 *   等待条件 —— 等待者记下代际后 `FUTEX_WAIT`，唤醒方 +1 后 `FUTEX_WAKE`。
 *   "无状态"指不需要为每个等待者分配内核对象，避免"先建没建"的竞态；
 * - **线程路径**：`m_waiters` 队列 + `std::mutex`；
 * - **park 前自旋**：`kSpinBudget = 128` 次。这不是可有可无的优化 ——
 *   无自旋时 2 个等待者的场景实测比 `std::condition_variable` 慢 3.3 倍，
 *   因为"检查完条件、还没进内核"的窗口里到达的唤醒会丢失，等待者被迫睡到超时；
 * - `FiberWaitUnsupported()`：平台能力探测，让上层在不支持时安全降级。
 *
 * ## 锁契约（最容易写错的地方）
 *
 * `wait*` 系列要求调用方**持有传入的外部 `unique_lock`**，返回时锁仍被持有
 * （与 `std::condition_variable` 一致）。实现内部会先释放再等待、被唤醒后
 * 重新获取，但**谓词判断永远在持锁状态下进行**，否则会出现丢失唤醒。
 */
#include "go2cpp/core/parking_condition.hpp"

#include "go2cpp/concurrency/core/futex_mutex.hpp"

#include "go2cpp/fiber.hpp"
#include "go2cpp/core/timer.hpp"
#include "go2cpp/scheduler.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <utility>


#if defined(__linux__)
#include <cerrno>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

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

}  // namespace
#endif  // __linux__

namespace go2cpp::core {

namespace {
/// FUTEX_WAIT 前的自旋预算。
/// 取较小值：只覆盖「唤醒已在路上」的窄窗口；过大则真需要睡眠时白烧 CPU。
constexpr int kSpinBudget = 128;
}  // namespace

/**
 * @brief 单个等待者的节点（一个 `wait*` 调用对应一个）。
 *
 * ## 定位
 *
 * `ParkingCondition::wait*` 在阻塞前创建它：受管 G 把它绑到
 * `Fiber::Suspend` 的 continuation 上，普通线程把它挂到 `m_waiters` 队列。
 * 唤醒方（`notify_one/notify_all`、定时器、Context 取消回调）通过
 * `Finish(...)` 认领它。上层使用者：`Scheduler` 的 park 路径、`sync::Mutex` 的
 * 普通线程等待、`channel`/`future`。
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
 * `Wake()` 持 `m_wake_mutex` 并检查 `m_active`：`Disarm` 会等待所有在途回调
 * 离开这道门后才允许等待者的栈被回收。否则会出现「Scheduler 指针已随 G 的栈
 * 失效、回调仍拿它调 `wake_or_cancel`」的 UAF。
 */
class ParkingCondition::WaitNode final {
public:
    /// @brief 等待结果四态；`kWaiting` 是唯一可被 Finish 改写的初态。
    enum class Result { kWaiting, kNotified, kTimedOut, kCancelled };

    WaitNode(ParkingCondition* owner, Scheduler* scheduler,
             std::shared_ptr<Task> task)
        : owner(owner), m_scheduler(scheduler), m_task(std::move(task)) {}

    bool Finish(Result result) noexcept {
        auto expected = Result::kWaiting;
        return m_result.compare_exchange_strong(
            expected, result, std::memory_order_acq_rel);
    }

    Result result() const noexcept {
        return m_result.load(std::memory_order_acquire);
    }

    void Wake() noexcept {
        // Disarm waits for any in-flight callback to leave this gate, so no
        // raw Scheduler access survives the waiting G's stack cleanup.
        std::lock_guard<FutexMutex> lock(m_wake_mutex);
        if (!m_active) {
            return;
        }
        if (m_scheduler != nullptr && m_task) {
            try {
                (void)m_scheduler->wake_or_cancel(m_task);
            } catch (...) {
            }
        } else {
#if defined(__linux__)
            owner->m_futex_seq.fetch_add(1, std::memory_order_release);
            if (owner->m_futex_waiters.load(std::memory_order_acquire) != 0) {
                (void)FutexWake(&owner->m_futex_seq, 1);
            }
#else
            m_native_cv.notify_one();
#endif
        }
    }

    void Disarm() noexcept {
        std::lock_guard<FutexMutex> lock(m_wake_mutex);
        m_active = false;
        m_scheduler = nullptr;
    }

    Result WaitNative(std::unique_lock<std::mutex>& outer,
                      std::optional<Clock::time_point> deadline) {
#if defined(__linux__)
        // futex 无状态等待。三个关键点（第一版曾因此崩溃）：
        //   1. 每轮循环**先判终态**，不能只依赖 FUTEX_WAIT 的返回码；
        //   2. 区分 EAGAIN（值已变，继续等）与 ETIMEDOUT（真超时）；
        //   3. 被唤醒后重读代际，作为下一轮 FUTEX_WAIT 的 expected。
        std::uint32_t expected =
            owner->m_futex_seq.load(std::memory_order_acquire);
        owner->m_futex_waiters.fetch_add(1, std::memory_order_acq_rel);
        outer.unlock();
        for (;;) {
            if (result() != Result::kWaiting) {
                break;
            }
            if (deadline && Clock::now() >= *deadline) {
                (void)Finish(Result::kTimedOut);
                break;
            }
            // 短暂自旋：无竞争时（唤醒就在眼前）完全避免 FUTEX_WAIT 系统调用。
            // 这是 Go runtime / parking_lot 的标准做法，也是本实现在「等待者少」
            // 场景下相对 condition_variable_any 唯一的劣势来源。
            bool spun_through = false;
            for (int spin = 0; spin < kSpinBudget; ++spin) {
                if (result() != Result::kWaiting) {
                    break;
                }
                if (owner->m_futex_seq.load(std::memory_order_acquire) != expected) {
                    spun_through = true;
                    break;
                }
                if (deadline && Clock::now() >= *deadline) {
                    break;
                }
                std::atomic_signal_fence(std::memory_order_seq_cst);
            }
            if (result() != Result::kWaiting) {
                break;
            }
            if (spun_through) {
                expected = owner->m_futex_seq.load(std::memory_order_acquire);
                continue;
            }
            if (deadline && Clock::now() >= *deadline) {
                (void)Finish(Result::kTimedOut);
                break;
            }
            errno = 0;
            const int rc = FutexWait(&owner->m_futex_seq, expected, deadline);
            if (result() != Result::kWaiting) {
                break;
            }
            const std::uint32_t latest =
                owner->m_futex_seq.load(std::memory_order_acquire);
            if (latest != expected) {
                expected = latest;
            }
            if (rc == 0) {
                continue;  // 被唤醒：重判终态
            }
            if (errno == EAGAIN) {
                continue;  // 值已变（可能是别人推进的代际）：重判
            }
            if (errno == ETIMEDOUT || errno == EINTR) {
                // 超时或被信号打断：交给下一轮循环判 deadline / 终态
                continue;
            }
            // 其它错误：保守地再判一次终态，仍等待就按超时处理，绝不空转。
            if (result() == Result::kWaiting) {
                (void)Finish(Result::kTimedOut);
            }
            break;
        }
        owner->m_futex_waiters.fetch_sub(1, std::memory_order_acq_rel);
        // 注意：**不重新加锁**。WaitOnce 的契约是「WaitNative 返回时外部锁仍处于
        // 解锁状态，由 WaitOnce 自己关锁」。原实现把锁交给 cv.wait 管理，返回时
        // outer 也是解锁的；这里若 outer.lock() 会与 WaitOnce 的后续 lock 冲突，
        // 触发 EDEADLK（"Resource deadlock avoided"）。
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
    ParkingCondition* const owner;
    Scheduler* m_scheduler;
    std::shared_ptr<Task> m_task;
    std::atomic<Result> m_result{Result::kWaiting};
    FutexMutex m_wake_mutex;
    std::condition_variable_any m_native_cv;
    bool m_active{true};
};

ParkingCondition::ParkingCondition() = default;
ParkingCondition::~ParkingCondition() = default;

bool ParkingCondition::CancellationRequested() noexcept {
    const auto task = Scheduler::current_task();
    return task && task->cancellation_requested();
}

bool ParkingCondition::FiberWaitUnsupported() noexcept {
    return Fiber::Current() != nullptr && !Scheduler::current_task();
}

void ParkingCondition::notify_one() noexcept {
    std::shared_ptr<WaitNode> selected;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        while (!m_waiters.empty()) {
            auto node = std::move(m_waiters.front());
            m_waiters.pop_front();
            if (node->Finish(WaitNode::Result::kNotified)) {
                selected = std::move(node);
                break;
            }
        }
    }
    if (selected) {
        selected->Wake();
    }
}

void ParkingCondition::notify_all() noexcept {
    std::deque<std::shared_ptr<WaitNode>> waiters;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        waiters.swap(m_waiters);
        for (const auto& node : waiters) {
            (void)node->Finish(WaitNode::Result::kNotified);
        }
    }
    for (const auto& node : waiters) {
        if (node->result() == WaitNode::Result::kNotified) {
            node->Wake();
        }
    }
}

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

    const auto node = std::make_shared<WaitNode>(this, 
        managed ? scheduler : nullptr, managed ? task : nullptr);
    {
        std::lock_guard<std::mutex> queue_lock(m_mutex);
        // Recheck after acquiring the notification lock. Context cancellation
        // does not hold the channel's external lock, so this second check is
        // required to close its notify-before-publication window.
        if (predicate()) {
            return WaitStatus::kNotified;
        }
        m_waiters.push_back(node);
    }

    Timer timer;
    const auto cleanup = [&] {
        (void)node->Finish(WaitNode::Result::kCancelled);
        node->Disarm();
        timer.Cancel();
        std::lock_guard<std::mutex> queue_lock(m_mutex);
        const auto position =
            std::find(m_waiters.begin(), m_waiters.end(), node);
        if (position != m_waiters.end()) {
            m_waiters.erase(position);
        }
    };

    WaitNode::Result result;
    try {
        if (managed && deadline) {
            const std::weak_ptr<WaitNode> weak(node);
            timer = TimerService::Default().Schedule(*deadline, [weak] {
                if (auto current = weak.lock()) {
                    if (current->Finish(WaitNode::Result::kTimedOut)) {
                        current->Wake();
                    }
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
        if (!lock.owns_lock()) {
            lock.lock();
        }
        throw;
    }
    switch (result) {
    case WaitNode::Result::kNotified:
        return WaitStatus::kNotified;
    case WaitNode::Result::kTimedOut:
        return WaitStatus::kTimedOut;
    case WaitNode::Result::kWaiting:
    case WaitNode::Result::kCancelled:
        return WaitStatus::kCancelled;
    }
    return WaitStatus::kCancelled;
}

}  // namespace go2cpp::core
