#include "go2cpp/channel.hpp"
#include "go2cpp/io.hpp"
#include "go2cpp/scheduler.hpp"
#include "go2cpp/sync.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <fcntl.h>
#include <poll.h>
#include <sys/syscall.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

[[noreturn]] void Fail(const char* message);
void Check(bool value, const char* message);

class ThreadPool final {
public:
    explicit ThreadPool(std::size_t worker_count) {
        m_workers.reserve(worker_count);
        for (std::size_t index = 0; index < worker_count; ++index) {
            m_workers.emplace_back([this] {
                for (;;) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lock(m_mutex);
                        m_condition.wait(lock, [this] {
                            return m_stopping || !m_tasks.empty();
                        });
                        if (m_tasks.empty()) {
                            if (m_stopping) {
                                return;
                            }
                            continue;
                        }
                        task = std::move(m_tasks.front());
                        m_tasks.pop_front();
                    }
                    task();
                }
            });
        }
    }

    ~ThreadPool() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stopping = true;
        }
        m_condition.notify_all();
        for (auto& worker : m_workers) {
            worker.join();
        }
    }

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    void Submit(std::function<void()> task) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            Check(static_cast<bool>(task), "empty thread-pool task");
            Check(!m_stopping, "submit after thread-pool stop");
            m_tasks.emplace_back(std::move(task));
        }
        m_condition.notify_one();
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_condition;
    std::deque<std::function<void()>> m_tasks;
    std::vector<std::thread> m_workers;
    bool m_stopping{false};
};

[[noreturn]] void Fail(const char* message) {
    std::fprintf(stderr, "high-load stress failure: %s\n", message);
    std::abort();
}

void Check(bool value, const char* message) {
    if (!value) {
        Fail(message);
    }
}

class TaskCompletionGuard final {
public:
    explicit TaskCompletionGuard(std::atomic<int>& completed)
        : m_completed(completed) {}
    ~TaskCompletionGuard() { m_completed.fetch_add(1, std::memory_order_release); }

    TaskCompletionGuard(const TaskCompletionGuard&) = delete;
    TaskCompletionGuard& operator=(const TaskCompletionGuard&) = delete;

private:
    std::atomic<int>& m_completed;
};

template <typename Predicate>
void WaitUntil(Predicate predicate, std::chrono::seconds timeout,
               const char* message) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    Check(predicate(), message);
}

/**
 * @brief 构造高负载压力测试使用的调度配置。
 * @return 适合大量任务、Fiber 和 IO 的 SchedulerConfig 值。
 */
go2cpp::SchedulerConfig StressConfig() {
    go2cpp::SchedulerConfig config;
    config.processor_count = 8;
    config.min_workers = 2;
    config.max_workers = 16;
    config.local_queue_limit = 128;
    config.idle_worker_timeout = 100ms;
    config.sysmon_interval = 2ms;
    config.long_syscall_threshold = 10ms;
    config.enable_sysmon = true;
    return config;
}

/**
 * @brief 执行可重复的计算内核，用于比较 Fiber 和线程池吞吐。
 * @param value 输入种子值。
 * @param rounds 迭代轮数。
 * @return 计算后的校验值。
 */
std::uint64_t ComputeKernel(std::uint64_t value, int rounds) {
    for (int round = 0; round < rounds; ++round) {
        value ^= value >> 29U;
        value *= 0x9e3779b97f4a7c15ULL;
        value ^= value << 17U;
        value += 0x517cc1b727220a95ULL;
    }
    return value;
}

/**
 * @brief 执行 Fiber 计算密集压力场景并返回耗时。
 * @return 场景耗时的毫秒值。
 */
std::int64_t StressFiberCompute() {
    constexpr int kTaskCount = 8000;
    constexpr int kRounds = 100000;
    const auto started = Clock::now();
    go2cpp::Scheduler scheduler(StressConfig());
    scheduler.start();
    std::vector<std::uint64_t> results(kTaskCount);
    std::vector<std::shared_ptr<go2cpp::Task>> tasks;
    tasks.reserve(kTaskCount);
    for (int index = 0; index < kTaskCount; ++index) {
        tasks.emplace_back(scheduler.spawn([&, index] {
            results[static_cast<std::size_t>(index)] =
                ComputeKernel(static_cast<std::uint64_t>(index) + 1U,
                              kRounds);
        }));
    }
    for (const auto& task : tasks) {
        Check(task->wait_for(120s), "Fiber compute task did not finish");
    }
    for (int index = 0; index < kTaskCount; ++index) {
        Check(results[static_cast<std::size_t>(index)] ==
                  ComputeKernel(static_cast<std::uint64_t>(index) + 1U,
                                kRounds),
              "Fiber compute result mismatch");
    }
    Check(scheduler.shutdown_for(30s), "Fiber compute scheduler shutdown failed");
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               Clock::now() - started)
        .count();
}

/**
 * @brief 执行传统线程池计算密集压力场景并返回耗时。
 * @return 场景耗时的毫秒值。
 */
std::int64_t StressThreadCompute() {
    constexpr int kTaskCount = 8000;
    constexpr int kRounds = 100000;
    const auto started = Clock::now();
    ThreadPool pool(16);
    std::vector<std::uint64_t> results(kTaskCount);
    std::atomic<int> completed{0};
    for (int index = 0; index < kTaskCount; ++index) {
        pool.Submit([&, index] {
            results[static_cast<std::size_t>(index)] =
                ComputeKernel(static_cast<std::uint64_t>(index) + 1U,
                              kRounds);
            completed.fetch_add(1, std::memory_order_release);
        });
    }
    WaitUntil([&] { return completed.load(std::memory_order_acquire) == kTaskCount; }, 120s,
              "thread compute task did not finish");
    for (int index = 0; index < kTaskCount; ++index) {
        Check(results[static_cast<std::size_t>(index)] ==
                  ComputeKernel(static_cast<std::uint64_t>(index) + 1U,
                                kRounds),
              "thread compute result mismatch");
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               Clock::now() - started)
        .count();
}

/**
 * @brief 执行 Scheduler 大量任务提交、窃取和完成压力场景。
 * @return 场景耗时的毫秒值。
 */
std::int64_t StressScheduler() {
    constexpr int kTaskCount = 50000;
    constexpr int kProducerCount = 8;
    constexpr int kYieldsPerTask = 4;

    const auto started = Clock::now();
    go2cpp::Scheduler scheduler(StressConfig());
    scheduler.start();

    std::atomic<int> completed{0};
    std::vector<std::atomic<int>> executions(kTaskCount);
    for (auto& count : executions) {
        count.store(0, std::memory_order_relaxed);
    }

    std::mutex tasks_mutex;
    std::vector<std::shared_ptr<go2cpp::Task>> tasks;
    tasks.reserve(kTaskCount);
    std::vector<std::thread> producers;
    producers.reserve(kProducerCount);
    for (int producer = 0; producer < kProducerCount; ++producer) {
        producers.emplace_back([&, producer] {
            for (int index = producer; index < kTaskCount;
                 index += kProducerCount) {
                auto task = scheduler.spawn([&, index] {
                    executions[index].fetch_add(1, std::memory_order_relaxed);
                    for (int round = 0; round < kYieldsPerTask; ++round) {
                        Check(scheduler.yield_current(),
                              "scheduler yield failed under load");
                    }
                    completed.fetch_add(1, std::memory_order_release);
                });
                std::lock_guard<std::mutex> lock(tasks_mutex);
                tasks.emplace_back(std::move(task));
            }
        });
    }
    for (auto& producer : producers) {
        producer.join();
    }

    WaitUntil(
        [&] { return completed.load(std::memory_order_acquire) == kTaskCount; },
        90s, "scheduler did not complete 50000 yielding tasks");
    for (const auto& task : tasks) {
        Check(task && task->wait_for(5s), "task completion wait failed");
        Check(task->state() == go2cpp::GState::kDead,
              "task did not reach dead state");
    }
    for (const auto& count : executions) {
        Check(count.load(std::memory_order_acquire) == 1,
              "task executed more than once or was lost");
    }
    Check(scheduler.runnable_count() == 0,
          "runnable queue was not empty after stress");
    Check(scheduler.shutdown_for(30s), "scheduler shutdown timed out");
    const auto metrics = scheduler.metrics();
    std::fprintf(stderr,
                 "[metrics] task-runs=%llu completions=%llu resume-ns=%llu "
                 "local-pops=%llu steals=%llu\n",
                 static_cast<unsigned long long>(metrics.task_runs),
                 static_cast<unsigned long long>(metrics.task_completions),
                 static_cast<unsigned long long>(metrics.fiber_resume_ns),
                 static_cast<unsigned long long>(metrics.local_queue_pops),
                 static_cast<unsigned long long>(metrics.steal_pops));
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               Clock::now() - started)
        .count();
}

/**
 * @brief 执行 Fiber/线程混合 Mutex 竞争压力场景。
 * @return 场景耗时的毫秒值。
 */
std::int64_t StressMixedMutex() {
    constexpr int kFiberCount = 2000;
    constexpr int kNativeCount = 32;
    constexpr int kIterations = 100;

    const auto started = Clock::now();
    go2cpp::Scheduler scheduler(StressConfig());
    scheduler.start();
    go2cpp::sync::Mutex mutex;
    std::atomic<int> operations{0};
    std::atomic<int> fiber_done{0};
    std::atomic<int> failures{0};
    std::vector<std::shared_ptr<go2cpp::Task>> fibers;
    fibers.reserve(kFiberCount);
    for (int index = 0; index < kFiberCount; ++index) {
        fibers.emplace_back(scheduler.spawn([&] {
            for (int round = 0; round < kIterations; ++round) {
                if (!mutex.Lock()) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                operations.fetch_add(1, std::memory_order_relaxed);
                mutex.Unlock();
            }
            fiber_done.fetch_add(1, std::memory_order_release);
        }));
    }

    std::vector<std::thread> native;
    native.reserve(kNativeCount);
    for (int index = 0; index < kNativeCount; ++index) {
        native.emplace_back([&] {
            for (int round = 0; round < kIterations * 2; ++round) {
                if (!mutex.Lock()) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                operations.fetch_add(1, std::memory_order_relaxed);
                mutex.Unlock();
            }
        });
    }
    for (auto& thread : native) {
        thread.join();
    }
    WaitUntil([&] { return fiber_done.load(std::memory_order_acquire) ==
                            kFiberCount; },
              60s, "mixed Fiber/native mutex load did not complete");
    Check(failures.load(std::memory_order_acquire) == 0,
          "mixed Fiber/native mutex operation failed");
    const int expected = kFiberCount * kIterations +
                         kNativeCount * kIterations * 2;
    Check(operations.load(std::memory_order_acquire) == expected,
          "mixed mutex operation count mismatch");
    for (const auto& task : fibers) {
        Check(task->wait_for(5s), "mixed mutex Fiber did not finish");
    }
    Check(scheduler.shutdown_for(30s), "mixed mutex scheduler shutdown failed");
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               Clock::now() - started)
        .count();
}

/**
 * @brief 执行 Fiber Channel 多生产者/消费者压力场景。
 * @return 场景耗时的毫秒值。
 */
std::int64_t StressFiberChannel() {
    constexpr int kProducerCount = 16;
    constexpr int kConsumerCount = 16;
    constexpr int kMessagesPerProducer = 2000;
    constexpr int kMessageCount = kProducerCount * kMessagesPerProducer;

    const auto started = Clock::now();
    go2cpp::Scheduler scheduler(StressConfig());
    scheduler.start();
    const auto channel = go2cpp::MakeChannel<int>(256);
    std::vector<std::atomic<int>> seen(kMessageCount);
    for (auto& count : seen) {
        count.store(0, std::memory_order_relaxed);
    }
    std::atomic<int> producers_done{0};
    std::atomic<int> received{0};
    std::atomic<int> failures{0};
    std::vector<std::shared_ptr<go2cpp::Task>> tasks;
    tasks.reserve(kProducerCount + kConsumerCount);

    for (int producer = 0; producer < kProducerCount; ++producer) {
        tasks.emplace_back(scheduler.spawn([&, producer] {
            const int base = producer * kMessagesPerProducer;
            for (int offset = 0; offset < kMessagesPerProducer; ++offset) {
                const auto result = channel->SendFor(base + offset, 30s);
                if (result.status != go2cpp::ChannelStatus::kReady) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
            }
            if (producers_done.fetch_add(1, std::memory_order_acq_rel) + 1 ==
                kProducerCount) {
                (void)channel->Close();
            }
        }));
    }
    for (int consumer = 0; consumer < kConsumerCount; ++consumer) {
        tasks.emplace_back(scheduler.spawn([&] {
            for (;;) {
                const auto result = channel->RecvFor(30s);
                if (result.status == go2cpp::ChannelStatus::kClosed) {
                    return;
                }
                if (result.status != go2cpp::ChannelStatus::kReady ||
                    !result.value || *result.value < 0 ||
                    *result.value >= kMessageCount) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                seen[static_cast<std::size_t>(*result.value)].fetch_add(
                    1, std::memory_order_relaxed);
                received.fetch_add(1, std::memory_order_release);
            }
        }));
    }

    WaitUntil(
        [&] { return received.load(std::memory_order_acquire) == kMessageCount; },
        90s, "Fiber channel did not deliver all messages");
    for (const auto& task : tasks) {
        Check(task->wait_for(10s), "channel Fiber did not finish");
    }
    Check(failures.load(std::memory_order_acquire) == 0,
          "Fiber channel operation failed");
    for (const auto& count : seen) {
        Check(count.load(std::memory_order_acquire) == 1,
              "channel duplicated or lost a message");
    }
    Check(scheduler.shutdown_for(30s), "channel scheduler shutdown failed");
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               Clock::now() - started)
        .count();
}

/**
 * @brief 执行线程池任务提交基准场景。
 * @return 场景耗时的毫秒值。
 */
std::int64_t StressThreadPoolTasks() {
    constexpr int kTaskCount = 50000;
    constexpr int kProducerCount = 8;
    constexpr int kYieldsPerTask = 4;

    const auto started = Clock::now();
    ThreadPool pool(16);
    std::atomic<int> completed{0};
    std::vector<std::atomic<int>> executions(kTaskCount);
    for (auto& count : executions) {
        count.store(0, std::memory_order_relaxed);
    }
    std::vector<std::thread> producers;
    producers.reserve(kProducerCount);
    for (int producer = 0; producer < kProducerCount; ++producer) {
        producers.emplace_back([&, producer] {
            for (int index = producer; index < kTaskCount;
                 index += kProducerCount) {
                pool.Submit([&, index] {
                    executions[index].fetch_add(1, std::memory_order_relaxed);
                    for (int round = 0; round < kYieldsPerTask; ++round) {
                        std::this_thread::yield();
                    }
                    completed.fetch_add(1, std::memory_order_release);
                });
            }
        });
    }
    for (auto& producer : producers) {
        producer.join();
    }
    WaitUntil(
        [&] { return completed.load(std::memory_order_acquire) == kTaskCount; },
        90s, "thread pool did not complete 50000 tasks");
    for (const auto& count : executions) {
        Check(count.load(std::memory_order_acquire) == 1,
              "thread-pool task executed more than once or was lost");
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               Clock::now() - started)
        .count();
}

/**
 * @brief 执行线程池 Mutex 竞争基准场景。
 * @return 场景耗时的毫秒值。
 */
std::int64_t StressThreadPoolMutex() {
    constexpr int kTaskCount = 2000;
    constexpr int kNativeCount = 32;
    constexpr int kIterations = 100;

    const auto started = Clock::now();
    ThreadPool pool(16);
    std::mutex mutex;
    std::atomic<int> operations{0};
    for (int index = 0; index < kTaskCount; ++index) {
        pool.Submit([&] {
            for (int round = 0; round < kIterations; ++round) {
                std::lock_guard<std::mutex> lock(mutex);
                operations.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    std::vector<std::thread> native;
    native.reserve(kNativeCount);
    for (int index = 0; index < kNativeCount; ++index) {
        native.emplace_back([&] {
            for (int round = 0; round < kIterations * 2; ++round) {
                std::lock_guard<std::mutex> lock(mutex);
                operations.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto& thread : native) {
        thread.join();
    }
    const int expected = kTaskCount * kIterations +
                         kNativeCount * kIterations * 2;
    WaitUntil([&] { return operations.load(std::memory_order_acquire) == expected; },
              60s, "thread-pool mutex load did not complete");
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               Clock::now() - started)
        .count();
}

/**
 * @brief 执行线程池消息队列基准场景。
 * @return 场景耗时的毫秒值。
 */
std::int64_t StressThreadPoolChannel() {
    constexpr int kProducerCount = 16;
    constexpr int kConsumerCount = 16;
    constexpr int kMessagesPerProducer = 2000;
    constexpr int kMessageCount = kProducerCount * kMessagesPerProducer;

    const auto started = Clock::now();
    ThreadPool pool(32);
    const auto channel = go2cpp::MakeChannel<int>(256);
    std::vector<std::atomic<int>> seen(kMessageCount);
    for (auto& count : seen) {
        count.store(0, std::memory_order_relaxed);
    }
    std::atomic<int> producers_done{0};
    std::atomic<int> received{0};
    std::atomic<int> failures{0};
    std::atomic<int> task_done{0};
    for (int producer = 0; producer < kProducerCount; ++producer) {
        pool.Submit([&, producer] {
            TaskCompletionGuard completion(task_done);
            const int base = producer * kMessagesPerProducer;
            for (int offset = 0; offset < kMessagesPerProducer; ++offset) {
                const auto result = channel->SendFor(base + offset, 30s);
                if (result.status != go2cpp::ChannelStatus::kReady) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
            }
            if (producers_done.fetch_add(1, std::memory_order_acq_rel) + 1 ==
                kProducerCount) {
                (void)channel->Close();
            }
        });
    }
    for (int consumer = 0; consumer < kConsumerCount; ++consumer) {
        pool.Submit([&] {
            TaskCompletionGuard completion(task_done);
            for (;;) {
                const auto result = channel->RecvFor(30s);
                if (result.status == go2cpp::ChannelStatus::kClosed) {
                    return;
                }
                if (result.status != go2cpp::ChannelStatus::kReady ||
                    !result.value || *result.value < 0 ||
                    *result.value >= kMessageCount) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                seen[static_cast<std::size_t>(*result.value)].fetch_add(
                    1, std::memory_order_relaxed);
                received.fetch_add(1, std::memory_order_release);
            }
        });
    }
    WaitUntil(
        [&] { return received.load(std::memory_order_acquire) == kMessageCount; },
        90s, "thread-pool channel did not deliver all messages");
    Check(failures.load(std::memory_order_acquire) == 0,
          "thread-pool channel operation failed");
    for (const auto& count : seen) {
        Check(count.load(std::memory_order_acquire) == 1,
              "thread-pool channel duplicated or lost a message");
    }
    WaitUntil([&] {
        return task_done.load(std::memory_order_acquire) ==
               kProducerCount + kConsumerCount;
    }, 30s, "thread-pool channel tasks did not exit before channel cleanup");
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               Clock::now() - started)
        .count();
}

/**
 * @brief 创建并设置非阻塞管道，供 IO 压力测试使用。
 * @param fds 输出读端和写端文件描述符。
 * @return 创建成功返回 0，否则返回负值。
 */
int MakeNonBlockingPipe(int (&fds)[2]) {
    fds[0] = -1;
    fds[1] = -1;
    return static_cast<int>(::syscall(SYS_pipe2, fds, O_NONBLOCK | O_CLOEXEC));
}

/**
 * @brief 关闭压力测试文件描述符并忽略重复关闭错误。
 * @param fd 待关闭的文件描述符。
 * @return 无。
 */
void RawClose(int fd) {
    if (fd >= 0) {
        (void)::syscall(SYS_close, fd);
    }
}

template <typename WriteFunction>
void DriveIORounds(const std::vector<int>& write_fds, int rounds,
                   const std::atomic<int>& ready, WriteFunction write_one) {
    const int expected_per_round = static_cast<int>(write_fds.size());
    for (int round = 0; round < rounds; ++round) {
        const int expected = (round + 1) * expected_per_round;
        WaitUntil([&] { return ready.load(std::memory_order_acquire) >= expected; },
                  30s, "IO readers did not publish readiness");
        for (const int fd : write_fds) {
            write_one(fd);
        }
    }
}

/**
 * @brief 执行 Fiber socket IO 多路等待压力场景。
 * @return 场景耗时的毫秒值。
 */
std::int64_t StressFiberIO() {
    constexpr int kReaders = 4096;
    constexpr int kRounds = 25;

    const auto started = Clock::now();
    go2cpp::SchedulerConfig config = StressConfig();
    config.processor_count = 1;
    config.min_workers = 1;
    config.max_workers = 1;
    go2cpp::io::IOManager manager(config);
    Check(manager.start(), "IO manager failed to start");
    std::vector<int> read_fds;
    std::vector<int> write_fds;
    std::vector<go2cpp::io::DescriptorTokenPtr> read_tokens;
    read_fds.reserve(kReaders);
    write_fds.reserve(kReaders);
    read_tokens.reserve(kReaders);
    for (int index = 0; index < kReaders; ++index) {
        int fds[2];
        Check(MakeNonBlockingPipe(fds) == 0, "pipe creation failed");
        read_fds.push_back(fds[0]);
        write_fds.push_back(fds[1]);
        read_tokens.push_back(go2cpp::io::DescriptorGuard::Capture(fds[0]));
    }

    std::atomic<int> ready{0};
    std::atomic<int> completed{0};
    std::atomic<int> failures{0};
    std::vector<std::shared_ptr<go2cpp::Task>> tasks;
    tasks.reserve(kReaders);
    for (std::size_t index = 0; index < read_fds.size(); ++index) {
        const int fd = read_fds[index];
        const auto token = read_tokens[index];
        tasks.emplace_back(manager.go([&, fd, token] {
            char value = 0;
            for (int round = 0; round < kRounds; ++round) {
                for (;;) {
                    const auto count =
                        ::syscall(SYS_read, fd, &value, sizeof(value));
                    if (count == 1) {
                        break;
                    }
                    if (count < 0 && errno == EINTR) {
                        continue;
                    }
                    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                        ready.fetch_add(1, std::memory_order_release);
                        // 吞吐基准不把 30 秒超时计时器的建表/删除成本混入
                        // readiness 路径；超时语义由独立单元测试覆盖。
                        const auto result = manager.wait(
                            fd, go2cpp::io::IOEvent::kRead, std::nullopt, {},
                            token);
                        if (!result.ready()) {
                            failures.fetch_add(1, std::memory_order_relaxed);
                            return;
                        }
                        continue;
                    }
                    failures.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
            }
            completed.fetch_add(1, std::memory_order_release);
        }));
    }

    std::thread writer([&] {
        DriveIORounds(write_fds, kRounds, ready, [](int fd) {
            const char value = 'x';
            for (;;) {
                const auto count =
                    ::syscall(SYS_write, fd, &value, sizeof(value));
                if (count == 1) {
                    return;
                }
                if (count < 0 && errno == EINTR) {
                    continue;
                }
                if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    std::this_thread::yield();
                    continue;
                }
                Fail("Fiber IO writer failed");
            }
        });
    });
    WaitUntil([&] { return completed.load(std::memory_order_acquire) == kReaders; },
              120s, "Fiber IO readers did not complete");
    writer.join();
    Check(failures.load(std::memory_order_acquire) == 0,
          "Fiber IO wait failed");
    for (const auto& task : tasks) {
        Check(task->wait_for(10s), "Fiber IO task did not finish");
    }
    manager.shutdown();
    for (const int fd : read_fds) {
        RawClose(fd);
    }
    for (const int fd : write_fds) {
        RawClose(fd);
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               Clock::now() - started)
        .count();
}

/**
 * @brief 执行传统线程 IO 多路等待基准场景。
 * @return 场景耗时的毫秒值。
 */
std::int64_t StressThreadIO() {
    constexpr int kReaders = 4096;
    constexpr int kRounds = 25;

    const auto started = Clock::now();
    std::vector<int> read_fds;
    std::vector<int> write_fds;
    read_fds.reserve(kReaders);
    write_fds.reserve(kReaders);
    for (int index = 0; index < kReaders; ++index) {
        int fds[2];
        Check(MakeNonBlockingPipe(fds) == 0, "thread pipe creation failed");
        read_fds.push_back(fds[0]);
        write_fds.push_back(fds[1]);
    }

    std::atomic<int> ready{0};
    std::atomic<int> completed{0};
    std::atomic<int> failures{0};
    std::vector<std::thread> readers;
    readers.reserve(kReaders);
    for (const int fd : read_fds) {
        readers.emplace_back([&, fd] {
            char value = 0;
            for (int round = 0; round < kRounds; ++round) {
                for (;;) {
                    const auto count =
                        ::syscall(SYS_read, fd, &value, sizeof(value));
                    if (count == 1) {
                        break;
                    }
                    if (count < 0 && errno == EINTR) {
                        continue;
                    }
                    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                        ready.fetch_add(1, std::memory_order_release);
                        pollfd descriptor{fd, POLLIN, 0};
                        if (::poll(&descriptor, 1, -1) <= 0) {
                            failures.fetch_add(1, std::memory_order_relaxed);
                            return;
                        }
                        continue;
                    }
                    failures.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
            }
            completed.fetch_add(1, std::memory_order_release);
        });
    }
    std::thread writer([&] {
        DriveIORounds(write_fds, kRounds, ready, [](int fd) {
            const char value = 'x';
            for (;;) {
                const auto count =
                    ::syscall(SYS_write, fd, &value, sizeof(value));
                if (count == 1) {
                    return;
                }
                if (count < 0 && errno == EINTR) {
                    continue;
                }
                if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    std::this_thread::yield();
                    continue;
                }
                Fail("thread IO writer failed");
            }
        });
    });
    for (auto& reader : readers) {
        reader.join();
    }
    writer.join();
    Check(completed.load(std::memory_order_acquire) == kReaders,
          "thread IO readers did not complete");
    Check(failures.load(std::memory_order_acquire) == 0,
          "thread IO wait failed");
    for (const int fd : read_fds) {
        RawClose(fd);
    }
    for (const int fd : write_fds) {
        RawClose(fd);
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               Clock::now() - started)
        .count();
}

}  // namespace

/**
 * @brief 高负载压力测试入口，可按命令行选择场景或全部运行。
 * @param argc 命令行参数数量。
 * @param argv 命令行参数数组。
 * @return 所有场景通过返回 0，否则返回非零值。
 */
int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--compute-only") == 0) {
        const auto fiber_ms = StressFiberCompute();
        const auto thread_ms = StressThreadCompute();
        std::fprintf(stderr,
                     "[compare] compute/fiber=%lld ms thread-pool=%lld ms\n",
                     static_cast<long long>(fiber_ms),
                     static_cast<long long>(thread_ms));
        return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "--scheduler-only") == 0) {
        std::fprintf(stderr, "[high-load] scheduler-only\n");
        std::fprintf(stderr, "[compare] scheduler/fiber=%lld ms\n",
                     static_cast<long long>(StressScheduler()));
        return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "--mutex-only") == 0) {
        std::fprintf(stderr, "[high-load] mutex-only\n");
        std::fprintf(stderr, "[compare] mixed-mutex/fiber=%lld ms\n",
                     static_cast<long long>(StressMixedMutex()));
        return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "--channel-only") == 0) {
        std::fprintf(stderr, "[high-load] channel-only\n");
        std::fprintf(stderr, "[compare] channel/fiber=%lld ms\n",
                     static_cast<long long>(StressFiberChannel()));
        return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "--io-only") == 0) {
        const auto fiber_io_ms = StressFiberIO();
        const auto thread_io_ms = StressThreadIO();
        std::fprintf(stderr,
                     "[compare] io-wait/fiber=%lld ms thread-poll=%lld ms\n",
                     static_cast<long long>(fiber_io_ms),
                     static_cast<long long>(thread_io_ms));
        return 0;
    }
    std::fprintf(stderr, "[high-load] scheduler: 50000 tasks x 4 yields\n");
    std::fprintf(stderr, "[high-load] compute: 8000 tasks x 100000 rounds\n");
    const auto fiber_compute_ms = StressFiberCompute();
    const auto thread_compute_ms = StressThreadCompute();
    std::fprintf(stderr,
                 "[compare] compute/fiber=%lld ms thread-pool=%lld ms\n",
                 static_cast<long long>(fiber_compute_ms),
                 static_cast<long long>(thread_compute_ms));
    const auto fiber_scheduler_ms = StressScheduler();
    const auto thread_scheduler_ms = StressThreadPoolTasks();
    std::fprintf(stderr,
                 "[compare] scheduler/fiber=%lld ms thread-pool=%lld ms\n",
                 static_cast<long long>(fiber_scheduler_ms),
                 static_cast<long long>(thread_scheduler_ms));
    std::fprintf(stderr, "[high-load] mixed mutex: 2000 Fibers + 32 threads\n");
    const auto fiber_mutex_ms = StressMixedMutex();
    const auto thread_mutex_ms = StressThreadPoolMutex();
    std::fprintf(stderr,
                 "[compare] mixed-mutex/fiber=%lld ms thread-pool/std-mutex=%lld ms\n",
                 static_cast<long long>(fiber_mutex_ms),
                 static_cast<long long>(thread_mutex_ms));
    std::fprintf(stderr, "[high-load] channel: 32000 messages, 32 Fibers\n");
    const auto fiber_channel_ms = StressFiberChannel();
    const auto thread_channel_ms = StressThreadPoolChannel();
    std::fprintf(stderr,
                 "[compare] channel/fiber=%lld ms thread-pool/channel=%lld ms\n",
                 static_cast<long long>(fiber_channel_ms),
                 static_cast<long long>(thread_channel_ms));
    std::fprintf(stderr, "[high-load] IO: 4096 readers x 25 readiness rounds\n");
    const auto fiber_io_ms = StressFiberIO();
    const auto thread_io_ms = StressThreadIO();
    std::fprintf(stderr,
                 "[compare] io-wait/fiber=%lld ms thread-poll=%lld ms\n",
                 static_cast<long long>(fiber_io_ms),
                 static_cast<long long>(thread_io_ms));
    std::fprintf(stderr, "[high-load] all checks passed\n");
    return 0;
}
