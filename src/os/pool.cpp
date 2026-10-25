/**
 * @file pool.cpp
 * @brief 固定大小进程池实现：有界队列、拒绝策略、缩扩容与优雅关闭。
 */
#include "go2cpp/os/pool.hpp"

#include "raw_io.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <signal.h>
#include <thread>
#include <vector>

namespace go2cpp::os {

namespace {

struct Slot final {
    std::mutex mutex;
    std::condition_variable cv;
    bool ready{false};
    PoolTaskResult result;
};

struct QueuedTask final {
    std::uint64_t id{0};
    PoolTask task;
    std::shared_ptr<Slot> slot;
};

}  // namespace

struct ProcessPool::Impl final {
    PoolOptions options;
    mutable std::mutex mutex;
    std::condition_variable work_cv;
    std::condition_variable space_cv;
    std::condition_variable done_cv;
    std::deque<QueuedTask> queue;
    /// worker 槽位表：下标就是线程身份（WorkerLoop 的 index 判断依据）。
    /// 缩容退出的线程把槽位腾空（std::thread 变为非 joinable），槽位本身保留；
    /// 后续扩容优先复用低位空槽 —— 新线程的 index 必须 < target，否则它一进
    /// WorkerLoop 就会被判定为「缩容待退出」而立刻返回。
    std::vector<std::thread> threads;
    /// 已决定退出、但尚未 join 的线程。缩容退出的 worker 把自身的 std::thread
    /// 对象从槽位移到这里：线程不能 join 自己，而 Shutdown 又必须等到所有线程
    /// 真正结束，所以登记与 join 分开，由下一次 Resize / Shutdown 负责 join。
    std::vector<std::thread> retired;
    /// 存活 worker 数（mutex 保护）。Stats().workers 报告这个值而不是槽位表
    /// 长度：缩容后槽位表仍保留历史上的槽位，长度不再等于线程数。
    std::size_t alive_workers{0};
    std::map<pid_t, int> running_pids;
    std::atomic<std::uint64_t> next_id{1};
    std::atomic<bool> running{false};
    std::atomic<bool> stopping{false};
    std::atomic<std::size_t> target_workers{0};
    bool started{false};
    PoolStats stats;

    Impl();
    ~Impl();
    Result<void> Start(const PoolOptions& pool_options);
    Result<std::uint64_t> Submit(PoolTask task);
    Result<PoolTaskResult> SubmitAndWait(PoolTask task,
                                         std::chrono::milliseconds timeout);
    Result<void> Resize(std::size_t workers);
    void Shutdown(std::chrono::milliseconds grace);
    PoolStats Stats() const;

    Result<std::uint64_t> Enqueue(PoolTask task, std::shared_ptr<Slot> slot);
    void WorkerLoop(std::size_t index);
    void RetireWorkerLocked(std::size_t index);
    void Execute(QueuedTask& queued);
    void CompleteSlot(const std::shared_ptr<Slot>& slot, PoolTaskResult result);
    void CancelQueueLocked();
};

ProcessPool::Impl::Impl() = default;

ProcessPool::Impl::~Impl() {
    Shutdown(std::chrono::milliseconds(1000));
}

Result<void> ProcessPool::Impl::Start(const PoolOptions& pool_options) {
    std::lock_guard<std::mutex> lock(mutex);
    if (started) {
        return MakeError(ProcErrorCode::kInvalidArgument, EEXIST, "进程池已启动",
                         "ProcessPool::Start");
    }
    if (pool_options.workers == 0 || pool_options.queue_capacity == 0) {
        return MakeError(ProcErrorCode::kInvalidArgument, EINVAL,
                         "workers/queue_capacity 必须 > 0", "ProcessPool::Start");
    }
    options = pool_options;
    stats = PoolStats{};
    stopping.store(false, std::memory_order_release);
    running.store(true, std::memory_order_release);
    started = true;
    target_workers.store(options.workers, std::memory_order_release);
    retired.clear();
    alive_workers = 0;
    threads.reserve(options.workers);
    for (std::size_t index = 0; index < options.workers; ++index) {
        threads.emplace_back([this, index] { WorkerLoop(index); });
        ++alive_workers;
    }
    return OkResult();
}

Result<std::uint64_t> ProcessPool::Impl::Enqueue(PoolTask task,
                                                 std::shared_ptr<Slot> slot) {
    std::unique_lock<std::mutex> lock(mutex);
    if (!running.load(std::memory_order_acquire)) {
        return MakeError(ProcErrorCode::kNotRunning, ESHUTDOWN, "进程池未运行",
                         "ProcessPool::Submit");
    }
    if (queue.size() >= options.queue_capacity) {
        if (!options.block_when_full) {
            ++stats.rejected;
            return MakeError(ProcErrorCode::kQueueFull, ENOBUFS, "进程池队列已满",
                             "ProcessPool::Submit");
        }
        const auto can_push = [this] {
            return !running.load(std::memory_order_acquire) ||
                   queue.size() < options.queue_capacity;
        };
        if (options.submit_timeout.count() <= 0) {
            space_cv.wait(lock, can_push);
        } else {
            const auto deadline = std::chrono::steady_clock::now() + options.submit_timeout;
            if (!space_cv.wait_until(lock, deadline, can_push)) {
                ++stats.rejected;
                return MakeError(ProcErrorCode::kTimeout, ETIMEDOUT, "等待队列空位超时",
                                 "ProcessPool::Submit");
            }
        }
        if (!running.load(std::memory_order_acquire)) {
            return MakeError(ProcErrorCode::kNotRunning, ESHUTDOWN, "进程池已关闭",
                             "ProcessPool::Submit");
        }
    }
    const std::uint64_t id = next_id.fetch_add(1, std::memory_order_relaxed);
    QueuedTask queued;
    queued.id = id;
    queued.task = std::move(task);
    queued.slot = std::move(slot);
    queue.push_back(std::move(queued));
    ++stats.submitted;
    // 必须 notify_all，不能 notify_one：缩容后等待谓词是
    // `!queue.empty() && index < target_workers`，index >= target 的 worker
    // 被唤醒后仍不满足条件、会继续等待。若 notify_one 恰好选中这类线程，
    // 新任务就无人消费，直到下一次 Submit/Resize/Shutdown 的 notify ——
    // 实测缩容后提交的任务会滞留到 SubmitAndWait 超时（单机约 10%）。
    // 唤醒全部等待者，保证仍有资格的 worker 必然重新检查队列。
    work_cv.notify_all();
    return id;
}

Result<std::uint64_t> ProcessPool::Impl::Submit(PoolTask task) {
    return Enqueue(std::move(task), nullptr);
}

void ProcessPool::Impl::CompleteSlot(const std::shared_ptr<Slot>& slot,
                                     PoolTaskResult result) {
    if (!slot) {
        return;
    }
    std::lock_guard<std::mutex> lock(slot->mutex);
    slot->result = std::move(result);
    slot->ready = true;
    slot->cv.notify_all();
}

void ProcessPool::Impl::Execute(QueuedTask& queued) {
    ProcessOptions process_options = queued.task.options;
    std::chrono::milliseconds timeout = queued.task.timeout;
    if (timeout.count() <= 0) {
        timeout = process_options.timeout;
    }
    if (timeout.count() <= 0) {
        timeout = options.default_timeout;
    }
    if (timeout.count() > 0) {
        process_options.timeout = timeout;
    }
    if (process_options.grace.count() <= 0) {
        process_options.grace = options.grace;
    }
    if (process_options.max_output_bytes == 0) {
        process_options.max_output_bytes = options.max_output_bytes;
    }
    if (options.forward_stderr) {
        process_options.forward_stderr = true;
    }

    PoolTaskResult result;
    result.id = queued.id;
    pid_t spawned_pid = -1;
    const auto on_spawn = [this, &spawned_pid](pid_t pid) {
        spawned_pid = pid;
        std::lock_guard<std::mutex> lock(mutex);
        running_pids[pid] = 0;
    };
    auto run = Run(process_options, queued.task.input, on_spawn);
    if (run.ok()) {
        result.run = std::move(run.value());
    } else {
        result.error = run.error();
    }
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (spawned_pid > 0) {
            running_pids.erase(spawned_pid);
        }
        if (run.ok()) {
            ++stats.completed;
            if (result.run.status.timed_out) {
                ++stats.timed_out;
            }
        } else {
            ++stats.failed;
        }
    }
    if (queued.task.on_complete) {
        try {
            queued.task.on_complete(result);
        } catch (...) {
        }
    }
    CompleteSlot(queued.slot, std::move(result));
}

void ProcessPool::Impl::RetireWorkerLocked(std::size_t index) {
    // 把即将退出的线程从槽位腾空，交给下一次 Resize / Shutdown 在锁外 join。
    // 这里只做登记：线程不能 join 自己，而 Shutdown 又必须等所有线程结束。
    if (index < threads.size() && threads[index].joinable()) {
        retired.push_back(std::move(threads[index]));
    }
    if (alive_workers > 0) {
        --alive_workers;
    }
}

void ProcessPool::Impl::WorkerLoop(std::size_t index) {
    for (;;) {
        QueuedTask queued;
        bool retire = false;
        {
            std::unique_lock<std::mutex> lock(mutex);
            // 唤醒条件三选一：
            // 1) Shutdown；
            // 2) 队列非空 —— 任何仍存活的 worker 都可以领任务，缩容中的多余
            //    worker 也先把已排队任务消化掉，避免「缩容即丢任务」；
            // 3) 队列为空且本 worker 的 index 已超出目标（缩容）—— 退出。
            work_cv.wait(lock, [this, index] {
                if (stopping.load(std::memory_order_acquire)) {
                    return true;
                }
                if (!queue.empty()) {
                    return true;
                }
                return index >= target_workers.load(std::memory_order_acquire);
            });
            if (stopping.load(std::memory_order_acquire)) {
                // Shutdown：线程对象由 Shutdown 统一 move + join，直接返回。
                return;
            }
            if (!queue.empty()) {
                queued = std::move(queue.front());
                queue.pop_front();
                ++stats.running;
                space_cv.notify_one();
            } else if (index >= target_workers.load(std::memory_order_acquire)) {
                // 队列已空且自己被缩容：登记退出。判断与登记在同一个临界区，
                // 与并发 Resize 的扩容串行化 —— 不会「刚决定退出、target 又
                // 变大」而让池里少一个 worker。
                RetireWorkerLocked(index);
                retire = true;
            } else {
                // 队列空且仍在目标内：通知竞争导致的空唤醒，继续等。
                continue;
            }
        }
        if (retire) {
            return;
        }
        Execute(queued);
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (stats.running > 0) {
                --stats.running;
            }
        }
        done_cv.notify_all();
    }
}

Result<PoolTaskResult> ProcessPool::Impl::SubmitAndWait(
    PoolTask task, std::chrono::milliseconds timeout) {
    auto slot = std::make_shared<Slot>();
    auto id = Enqueue(std::move(task), slot);
    if (!id.ok()) {
        return id.error();
    }
    std::unique_lock<std::mutex> lock(slot->mutex);
    if (timeout.count() < 0) {
        slot->cv.wait(lock, [&slot] { return slot->ready; });
    } else if (timeout.count() == 0) {
        if (!slot->ready) {
            return MakeError(ProcErrorCode::kTimeout, ETIMEDOUT, "等待任务完成超时",
                             "ProcessPool::SubmitAndWait");
        }
    } else if (!slot->cv.wait_for(lock, timeout, [&slot] { return slot->ready; })) {
        return MakeError(ProcErrorCode::kTimeout, ETIMEDOUT, "等待任务完成超时",
                         "ProcessPool::SubmitAndWait");
    }
    return slot->result;
}

Result<void> ProcessPool::Impl::Resize(std::size_t workers) {
    if (workers == 0) {
        return MakeError(ProcErrorCode::kInvalidArgument, EINVAL, "workers 必须 > 0",
                         "ProcessPool::Resize");
    }
    std::vector<std::thread> to_join;
    {
        std::unique_lock<std::mutex> lock(mutex);
        if (!running.load(std::memory_order_acquire)) {
            return MakeError(ProcErrorCode::kNotRunning, ESHUTDOWN, "进程池未运行",
                             "ProcessPool::Resize");
        }
        // 先更新目标值再补线程：缩容时 [workers, 旧目标) 的 worker 会在下次
        // 醒来且队列为空时自行退出；扩容时新线程创建后立刻看到新目标。
        target_workers.store(workers, std::memory_order_release);
        // 保证 [0, workers) 内每个槽位都有一个「不会退出」的 worker：
        //   - 槽位已有 joinable 线程且其 index < 新目标：它下次检查不会退出
        //     （决定退出与登记在同一临界区，与这里的锁串行化），直接复用；
        //   - 槽位已空（缩容线程登记退出时腾空）或从未创建：补一个新线程。
        // 只看 alive_workers 不够：它仍包含 index >= 新目标、即将退出的线程，
        // 按它补位会漏掉「刚退出线程留下的空槽」—— 例如 Start(4) 后
        // Resize(1)，在 index 1 已退、index 2/3 未退的窗口里再 Resize(3)，
        // 会永久少一个 worker。逐槽检查与线程的实际去留一一对应。
        for (std::size_t slot = 0; slot < workers; ++slot) {
            if (slot < threads.size() && threads[slot].joinable()) {
                continue;
            }
            if (slot < threads.size()) {
                threads[slot] = std::thread([this, slot] { WorkerLoop(slot); });
            } else {
                threads.emplace_back([this, slot] { WorkerLoop(slot); });
            }
            ++alive_workers;
        }
        to_join.swap(retired);
    }
    // 通知放在锁外：被唤醒的 worker 立刻能拿到 mutex 检查新目标。
    work_cv.notify_all();
    // join 缩容退出的线程：它们已不再访问共享状态，join 不会等任务。
    for (auto& thread : to_join) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    return OkResult();
}

void ProcessPool::Impl::CancelQueueLocked() {
    while (!queue.empty()) {
        QueuedTask queued = std::move(queue.front());
        queue.pop_front();
        ++stats.cancelled;
        PoolTaskResult result;
        result.id = queued.id;
        result.error = MakeError(ProcErrorCode::kNotRunning, ESHUTDOWN,
                                 "进程池关闭，任务未执行", "ProcessPool::Shutdown");
        CompleteSlot(queued.slot, std::move(result));
    }
}

void ProcessPool::Impl::Shutdown(std::chrono::milliseconds grace) {
    std::vector<std::thread> join_threads;
    std::map<pid_t, int> pids_to_kill;
    {
        std::unique_lock<std::mutex> lock(mutex);
        if (!started) {
            return;
        }
        stopping.store(true, std::memory_order_release);
        running.store(false, std::memory_order_release);
        CancelQueueLocked();
        work_cv.notify_all();
        space_cv.notify_all();
        const auto is_idle = [this] { return stats.running == 0; };
        if (stats.running > 0) {
            if (grace.count() <= 0) {
                done_cv.wait(lock, is_idle);
            } else {
                done_cv.wait_for(lock, grace, is_idle);
            }
        }
        pids_to_kill = running_pids;
        join_threads = std::move(threads);
        threads.clear();
        for (auto& thread : retired) {
            if (thread.joinable()) {
                join_threads.push_back(std::move(thread));
            }
        }
        retired.clear();
    }
    // 关闭超时后仍未结束的子进程：直接 SIGKILL（worker 的 Run 会回收）。
    for (const auto& item : pids_to_kill) {
        (void)::kill(item.first, SIGKILL);
    }
    for (auto& thread : join_threads) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    {
        std::lock_guard<std::mutex> lock(mutex);
        started = false;
        stopping.store(false, std::memory_order_release);
        alive_workers = 0;
    }
}

PoolStats ProcessPool::Impl::Stats() const {
    std::lock_guard<std::mutex> lock(mutex);
    PoolStats snapshot = stats;
    snapshot.workers = alive_workers;
    snapshot.idle = snapshot.workers > snapshot.running
                        ? snapshot.workers - snapshot.running
                        : 0;
    snapshot.queued = queue.size();
    return snapshot;
}

// ===========================================================================
// ProcessPool 公共接口
// ===========================================================================

ProcessPool::ProcessPool() noexcept : m_impl(std::make_unique<Impl>()) {}

ProcessPool::~ProcessPool() = default;

ProcessPool::ProcessPool(ProcessPool&& other) noexcept : m_impl(std::move(other.m_impl)) {}

ProcessPool& ProcessPool::operator=(ProcessPool&& other) noexcept {
    if (this != &other) {
        m_impl = std::move(other.m_impl);
    }
    return *this;
}

Result<void> ProcessPool::Start(const PoolOptions& options) {
    return m_impl->Start(options);
}

bool ProcessPool::running() const {
    return m_impl->running.load(std::memory_order_acquire);
}

Result<std::uint64_t> ProcessPool::Submit(PoolTask task) {
    return m_impl->Submit(std::move(task));
}

Result<PoolTaskResult> ProcessPool::SubmitAndWait(PoolTask task,
                                                  std::chrono::milliseconds timeout) {
    return m_impl->SubmitAndWait(std::move(task), timeout);
}

Result<void> ProcessPool::Resize(std::size_t workers) {
    return m_impl->Resize(workers);
}

void ProcessPool::Shutdown(std::chrono::milliseconds grace) {
    m_impl->Shutdown(grace);
}

PoolStats ProcessPool::Stats() const { return m_impl->Stats(); }

}  // namespace go2cpp::os
