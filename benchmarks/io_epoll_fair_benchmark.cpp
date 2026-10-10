/**
 * @file io_epoll_fair_benchmark.cpp
 * @brief 使用同一批管道和就绪波次比较 IOManager/Fiber 与线程池 epoll。
 *
 * 两种模型复用相同的 4096 个非阻塞 pipe、相同的 25 轮逐 fd 可读事件、
 * 相同的固定 worker 数和一个 epoll 轮询线程。每轮都要求所有 fd 消费
 * 完成后才发送下一轮，避免把积压事件误算成处理吞吐。setup 与稳态时间
 * 分开统计；该基准不包含 socket 协议、连接建立或网络传输成本。
 */
#include "go2cpp/io.hpp"

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr std::size_t kDefaultDescriptors = 4096;
constexpr std::size_t kDefaultWaves = 25;
constexpr std::size_t kDefaultWorkers = 8;
constexpr std::size_t kDefaultRepeats = 3;
constexpr auto kWatchdog = 10s;

struct Options final {
    std::size_t descriptors{kDefaultDescriptors};
    std::size_t waves{kDefaultWaves};
    std::size_t workers{kDefaultWorkers};
    std::size_t repeats{kDefaultRepeats};
};

struct PipePair final {
    int read_fd{-1};
    int write_fd{-1};
};

/**
 * @brief 预创建基准共用的非阻塞 pipe 集合。
 * @details 生命周期覆盖线程池与 IOManager 两种模型，避免两侧 fd 分配差异。
 */
class PipeSet final {
public:
    explicit PipeSet(std::size_t count) : m_pairs(count) {
        try {
            for (auto& pair : m_pairs) {
                int fds[2]{-1, -1};
                if (::pipe2(fds, O_NONBLOCK | O_CLOEXEC) != 0) {
                    throw std::system_error(
                        errno, std::generic_category(),
                        "pipe2 failed; check RLIMIT_NOFILE");
                }
                pair.read_fd = fds[0];
                pair.write_fd = fds[1];
            }
        } catch (...) {
            CloseAll();
            throw;
        }
    }

    ~PipeSet() { CloseAll(); }
    PipeSet(const PipeSet&) = delete;
    PipeSet& operator=(const PipeSet&) = delete;

    std::size_t size() const noexcept { return m_pairs.size(); }
    const PipePair& operator[](std::size_t index) const noexcept {
        return m_pairs[index];
    }

private:
    void CloseAll() noexcept {
        for (auto& pair : m_pairs) {
            if (pair.read_fd >= 0) {
                (void)::close(pair.read_fd);
                pair.read_fd = -1;
            }
            if (pair.write_fd >= 0) {
                (void)::close(pair.write_fd);
                pair.write_fd = -1;
            }
        }
    }

    std::vector<PipePair> m_pairs;
};

struct RunResult final {
    double setup_ms{0.0};
    double steady_ms{0.0};
    std::uint64_t expected_events{0};
    std::uint64_t completed_events{0};
    std::uint64_t errors{0};
    bool timed_out{false};
};

/**
 * @brief 使用原子计数和条件变量等待精确的事件完成目标。
 * @details 更新计数后也获取同一互斥量再通知，避免检查谓词与睡眠之间丢通知。
 */
class Completion final {
public:
    void Complete() noexcept {
        m_count.fetch_add(1, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
        }
        m_condition.notify_all();
    }

    std::uint64_t count() const noexcept {
        return m_count.load(std::memory_order_acquire);
    }

    bool WaitFor(std::uint64_t target, Clock::time_point deadline) {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_condition.wait_until(lock, deadline, [this, target] {
            return m_count.load(std::memory_order_acquire) >= target;
        });
    }

private:
    std::atomic<std::uint64_t> m_count{0};
    std::mutex m_mutex;
    std::condition_variable m_condition;
};

/** @brief 将单字节写入一个非阻塞管道；失败时抛出系统错误。 */
void WriteByte(int fd, std::uint8_t byte) {
    for (;;) {
        const long result = ::syscall(SYS_write, fd, &byte, sizeof(byte));
        if (result == static_cast<long>(sizeof(byte))) {
            return;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }
        throw std::system_error(result < 0 ? errno : EIO,
                                std::generic_category(), "pipe write failed");
    }
}

/** @brief 从一个非阻塞管道读取单字节；失败时抛出系统错误。 */
std::uint8_t ReadByte(int fd) {
    std::uint8_t byte = 0;
    for (;;) {
        const long result = ::syscall(SYS_read, fd, &byte, sizeof(byte));
        if (result == static_cast<long>(sizeof(byte))) {
            return byte;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }
        throw std::system_error(result < 0 ? errno : EIO,
                                std::generic_category(), "pipe read failed");
    }
}

/** @brief 返回每个 fd 在指定轮次应该收到的校验字节。 */
std::uint8_t Payload(std::size_t wave) noexcept {
    return static_cast<std::uint8_t>('A' + (wave % 26));
}

/** @brief 向全部管道写入一轮数据；任务侧需全部消费后才允许进入下一轮。 */
void SendWave(const PipeSet& pipes, std::size_t wave) {
    const auto byte = Payload(wave);
    for (std::size_t index = 0; index < pipes.size(); ++index) {
        WriteByte(pipes[index].write_fd, byte);
    }
}

/**
 * @brief 驱动固定轮数的 readiness 波次并测量稳态时间。
 * @param pipes 两种模型共享的管道集合。
 * @param waves readiness 轮数。
 * @param completed 事件完成计数器。
 * @param errors 异步错误计数器。
 * @param abort_requested 提前终止谓词。
 * @return 稳态耗时及 watchdog 是否超时。
 */
template <typename AbortPredicate>
std::pair<double, bool> DriveWaves(const PipeSet& pipes, std::size_t waves,
                                   Completion& completed,
                                   std::atomic<std::uint64_t>& errors,
                                   AbortPredicate abort_requested) {
    const auto begin = Clock::now();
    bool timed_out = false;
    for (std::size_t wave = 0; wave < waves; ++wave) {
        if (abort_requested()) {
            break;
        }
        SendWave(pipes, wave);
        const auto target = static_cast<std::uint64_t>(wave + 1) * pipes.size();
        if (!completed.WaitFor(target, Clock::now() + kWatchdog)) {
            timed_out = true;
            errors.fetch_add(1, std::memory_order_relaxed);
            break;
        }
    }
    return {std::chrono::duration<double, std::milli>(Clock::now() - begin)
                .count(),
            timed_out};
}

/**
 * @brief 单 epoll poller 加固定 worker 的线程池基线。
 * @details EPOLLONESHOT 使一个 fd 同时最多投递一个任务。worker 消费当前
 *          字节并重新布防后，驱动线程才会发送下一轮数据。
 */
class ThreadEpollPool final {
public:
    ThreadEpollPool(const PipeSet& pipes, Completion& completion,
                    std::atomic<std::uint64_t>& errors)
        : m_pipes(pipes), m_completion(completion), m_errors(errors),
          m_queue(pipes.size()), m_progress(pipes.size(), 0) {
        try {
            m_epoll_fd = ::epoll_create1(EPOLL_CLOEXEC);
            if (m_epoll_fd < 0) {
                ThrowSystemError("epoll_create1 failed");
            }
            m_stop_fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
            if (m_stop_fd < 0) {
                ThrowSystemError("eventfd failed");
            }
            AddEpollFd(m_stop_fd, 0);
            for (std::size_t index = 0; index < m_pipes.size(); ++index) {
                AddEpollFd(m_pipes[index].read_fd, index + 1);
            }
            m_workers.reserve(pipes.size());
        } catch (...) {
            ClosePollerFds();
            throw;
        }
    }

    ~ThreadEpollPool() { Stop(); }
    ThreadEpollPool(const ThreadEpollPool&) = delete;
    ThreadEpollPool& operator=(const ThreadEpollPool&) = delete;

    void Start(std::size_t worker_count) {
        for (std::size_t index = 0; index < worker_count; ++index) {
            m_workers.emplace_back([this] { WorkerLoop(); });
        }
        m_poller = std::thread([this] { PollerLoop(); });
        std::unique_lock<std::mutex> lock(m_start_mutex);
        if (!m_start_condition.wait_for(lock, 5s, [this] {
                return m_poller_ready;
            })) {
            throw std::runtime_error("epoll poller did not start");
        }
    }

    RunResult Run(std::size_t waves) {
        const auto [steady_ms, timed_out] = DriveWaves(
            m_pipes, waves, m_completion, m_errors,
            [this] { return m_stopping.load(std::memory_order_acquire); });
        RunResult result;
        result.steady_ms = steady_ms;
        result.expected_events =
            static_cast<std::uint64_t>(m_pipes.size()) * waves;
        result.completed_events = m_completion.count();
        result.errors = m_errors.load(std::memory_order_acquire);
        result.timed_out = timed_out;
        for (std::size_t index = 0; index < m_progress.size(); ++index) {
            if (m_progress[index] != waves) {
                ++result.errors;
            }
        }
        return result;
    }

    void Stop() noexcept {
        bool expected = false;
        if (!m_stopped.compare_exchange_strong(expected, true,
                                              std::memory_order_acq_rel)) {
            return;
        }
        m_stopping.store(true, std::memory_order_release);
        if (m_stop_fd >= 0) {
            const std::uint64_t one = 1;
            (void)::syscall(SYS_write, m_stop_fd, &one, sizeof(one));
        }
        if (m_poller.joinable()) {
            m_poller.join();
        }
        {
            std::lock_guard<std::mutex> lock(m_queue_mutex);
            m_workers_stopping = true;
        }
        m_queue_not_empty.notify_all();
        m_queue_not_full.notify_all();
        for (auto& worker : m_workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        ClosePollerFds();
    }

private:
    void ThrowSystemError(const char* message) {
        throw std::system_error(errno, std::generic_category(), message);
    }

    void ClosePollerFds() noexcept {
        if (m_stop_fd >= 0) {
            (void)::close(m_stop_fd);
            m_stop_fd = -1;
        }
        if (m_epoll_fd >= 0) {
            (void)::close(m_epoll_fd);
            m_epoll_fd = -1;
        }
    }

    void AddEpollFd(int fd, std::uint64_t tag) {
        epoll_event event{};
        event.events = EPOLLIN | (tag == 0 ? 0U : EPOLLONESHOT);
        event.data.u64 = tag;
        if (::epoll_ctl(m_epoll_fd, EPOLL_CTL_ADD, fd, &event) != 0) {
            ThrowSystemError("epoll_ctl ADD failed");
        }
    }

    void PollerLoop() noexcept {
        {
            std::lock_guard<std::mutex> lock(m_start_mutex);
            m_poller_ready = true;
        }
        m_start_condition.notify_one();

        std::array<epoll_event, 256> events{};
        while (!m_stopping.load(std::memory_order_acquire)) {
            const int count = ::epoll_wait(m_epoll_fd, events.data(),
                                          static_cast<int>(events.size()), -1);
            if (count < 0) {
                if (errno == EINTR) {
                    continue;
                }
                m_errors.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            for (int index = 0; index < count; ++index) {
                const auto tag = events[static_cast<std::size_t>(index)].data.u64;
                if (tag == 0) {
                    std::uint64_t value = 0;
                    (void)::syscall(SYS_read, m_stop_fd, &value, sizeof(value));
                    return;
                }
                if (m_stopping.load(std::memory_order_acquire)) {
                    return;
                }
                const auto fd_index = static_cast<std::size_t>(tag - 1);
                if (fd_index >= m_pipes.size() || !Submit(fd_index)) {
                    m_errors.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
            }
        }
    }

    bool Submit(std::size_t fd_index) noexcept {
        std::unique_lock<std::mutex> lock(m_queue_mutex);
        m_queue_not_full.wait(lock, [this] {
            return m_workers_stopping || m_queue_size < m_queue.size();
        });
        if (m_workers_stopping || m_queue.empty()) {
            return false;
        }
        m_queue[m_queue_tail] = fd_index;
        m_queue_tail = (m_queue_tail + 1) % m_queue.size();
        ++m_queue_size;
        lock.unlock();
        m_queue_not_empty.notify_one();
        return true;
    }

    void WorkerLoop() noexcept {
        for (;;) {
            std::size_t fd_index = 0;
            {
                std::unique_lock<std::mutex> lock(m_queue_mutex);
                m_queue_not_empty.wait(lock, [this] {
                    return m_workers_stopping || m_queue_size != 0;
                });
                if (m_queue_size == 0 && m_workers_stopping) {
                    return;
                }
                fd_index = m_queue[m_queue_head];
                m_queue_head = (m_queue_head + 1) % m_queue.size();
                --m_queue_size;
            }
            m_queue_not_full.notify_one();
            Process(fd_index);
        }
    }

    void Process(std::size_t fd_index) noexcept {
        try {
            const auto byte = ReadByte(m_pipes[fd_index].read_fd);
            if (byte != Payload(m_progress[fd_index])) {
                m_errors.fetch_add(1, std::memory_order_relaxed);
            }
            ++m_progress[fd_index];

            epoll_event event{};
            event.events = EPOLLIN | EPOLLONESHOT;
            event.data.u64 = fd_index + 1;
            if (::epoll_ctl(m_epoll_fd, EPOLL_CTL_MOD,
                            m_pipes[fd_index].read_fd, &event) != 0) {
                m_errors.fetch_add(1, std::memory_order_relaxed);
            }
        } catch (...) {
            m_errors.fetch_add(1, std::memory_order_relaxed);
        }
        m_completion.Complete();
    }

    const PipeSet& m_pipes;
    Completion& m_completion;
    std::atomic<std::uint64_t>& m_errors;
    int m_epoll_fd{-1};
    int m_stop_fd{-1};
    std::vector<std::size_t> m_queue;
    std::size_t m_queue_head{0};
    std::size_t m_queue_tail{0};
    std::size_t m_queue_size{0};
    std::vector<std::uint32_t> m_progress;
    std::mutex m_queue_mutex;
    std::condition_variable m_queue_not_empty;
    std::condition_variable m_queue_not_full;
    bool m_workers_stopping{false};
    std::vector<std::thread> m_workers;
    std::thread m_poller;
    std::mutex m_start_mutex;
    std::condition_variable m_start_condition;
    bool m_poller_ready{false};
    std::atomic<bool> m_stopping{false};
    std::atomic<bool> m_stopped{false};
};

/**
 * @brief 使用 IOManager 的 Fiber 等待相同管道 readiness 并校验消费字节。
 * @param pipes 与线程池基准复用的管道集合。
 * @param options worker 数和 readiness 轮数。
 * @return setup、稳态、正确性和 watchdog 结果。
 */
RunResult RunFiberIO(const PipeSet& pipes, const Options& options) {
    go2cpp::SchedulerConfig config;
    config.processor_count = options.workers;
    config.min_workers = options.workers;
    config.max_workers = options.workers;
    config.allow_worker_oversubscription = false;
    config.enable_sysmon = false;
    config.collect_metrics = false;
    config.idle_wait = 1ms;

    const auto setup_begin = Clock::now();
    go2cpp::IOManager manager(config);
    if (!manager.Start()) {
        throw std::runtime_error("IOManager start failed");
    }

    Completion completion;
    std::atomic<std::uint64_t> errors{0};
    std::atomic<std::size_t> started{0};
    std::atomic<bool> abort_requested{false};
    std::vector<std::uint32_t> progress(pipes.size(), 0);
    std::vector<std::shared_ptr<go2cpp::Task>> tasks;
    tasks.reserve(pipes.size());

    for (std::size_t fd_index = 0; fd_index < pipes.size(); ++fd_index) {
        tasks.emplace_back(manager.Go([&, fd_index] {
            started.fetch_add(1, std::memory_order_release);
            for (std::size_t wave = 0; wave < options.waves; ++wave) {
                if (abort_requested.load(std::memory_order_acquire)) {
                    return;
                }
                const auto result = manager.Wait(pipes[fd_index].read_fd,
                                                 go2cpp::IOEvent::kRead);
                if (!result.ready()) {
                    errors.fetch_add(1, std::memory_order_relaxed);
                    abort_requested.store(true, std::memory_order_release);
                    return;
                }
                try {
                    const auto byte = ReadByte(pipes[fd_index].read_fd);
                    if (byte != Payload(progress[fd_index])) {
                        errors.fetch_add(1, std::memory_order_relaxed);
                    }
                    ++progress[fd_index];
                    completion.Complete();
                } catch (...) {
                    errors.fetch_add(1, std::memory_order_relaxed);
                    abort_requested.store(true, std::memory_order_release);
                    return;
                }
            }
        }));
    }

    const auto startup_deadline = Clock::now() + 10s;
    while (started.load(std::memory_order_acquire) != pipes.size() &&
           Clock::now() < startup_deadline) {
        std::this_thread::yield();
    }
    if (started.load(std::memory_order_acquire) != pipes.size()) {
        abort_requested.store(true, std::memory_order_release);
        manager.Shutdown();
        throw std::runtime_error("Fiber waiters failed to start");
    }
    const double setup_ms =
        std::chrono::duration<double, std::milli>(Clock::now() - setup_begin)
            .count();

    const auto [steady_ms, timed_out] = DriveWaves(
        pipes, options.waves, completion, errors,
        [&abort_requested] {
            return abort_requested.load(std::memory_order_acquire);
        });
    if (timed_out || abort_requested.load(std::memory_order_acquire)) {
        abort_requested.store(true, std::memory_order_release);
    }
    manager.Shutdown();

    RunResult result;
    result.setup_ms = setup_ms;
    result.steady_ms = steady_ms;
    result.expected_events =
        static_cast<std::uint64_t>(pipes.size()) * options.waves;
    result.completed_events = completion.count();
    result.errors = errors.load(std::memory_order_acquire);
    result.timed_out = timed_out;
    for (const auto count : progress) {
        if (count != options.waves) {
            ++result.errors;
        }
    }
    return result;
}

/** @brief 运行线程池 epoll 模型，并单独统计 setup 与 readiness 稳态。 */
RunResult RunThreadEpoll(const PipeSet& pipes, const Options& options) {
    Completion completion;
    std::atomic<std::uint64_t> errors{0};
    const auto setup_begin = Clock::now();
    ThreadEpollPool pool(pipes, completion, errors);
    pool.Start(options.workers);
    const double setup_ms =
        std::chrono::duration<double, std::milli>(Clock::now() - setup_begin)
            .count();
    auto result = pool.Run(options.waves);
    result.setup_ms = setup_ms;
    pool.Stop();
    return result;
}

/** @brief 确认上一轮模型已消费全部管道数据，避免跨模型残留。 */
void CheckPipesEmpty(const PipeSet& pipes) {
    for (std::size_t index = 0; index < pipes.size(); ++index) {
        std::uint8_t byte = 0;
        const long result = ::syscall(SYS_read, pipes[index].read_fd,
                                      &byte, sizeof(byte));
        if (result >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
            throw std::runtime_error("pipe is not empty between model runs");
        }
    }
}

/** @brief 严格读取有范围限制的无符号环境变量。 */
std::size_t ReadOption(const char* name, std::size_t fallback,
                       std::size_t minimum, std::size_t maximum) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return fallback;
    }
    char* end = nullptr;
    errno = 0;
    const auto parsed = std::strtoull(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed < minimum ||
        parsed > maximum) {
        std::cerr << "警告：环境变量 " << name << " 值无效，使用默认值 "
                  << fallback << '\n';
        return fallback;
    }
    return static_cast<std::size_t>(parsed);
}

Options ReadOptions() {
    Options options;
    options.descriptors =
        ReadOption("GO2CPP_IO_BENCH_FDS", kDefaultDescriptors, 1, 65536);
    options.waves =
        ReadOption("GO2CPP_IO_BENCH_WAVES", kDefaultWaves, 1, 10000);
    options.workers =
        ReadOption("GO2CPP_IO_BENCH_WORKERS", kDefaultWorkers, 1, 32);
    options.repeats =
        ReadOption("GO2CPP_IO_BENCH_REPEATS", kDefaultRepeats, 1, 20);
    return options;
}

void PrintResult(const char* model, std::size_t repeat,
                 const RunResult& result) {
    const double rate = result.steady_ms > 0.0
                            ? static_cast<double>(result.completed_events) *
                                  1000.0 / result.steady_ms
                            : 0.0;
    std::cout << std::left << std::setw(20) << model << " repeat=" << repeat
              << " setup_ms=" << std::fixed << std::setprecision(2)
              << result.setup_ms << " steady_ms=" << result.steady_ms
              << " events=" << result.completed_events << '/'
              << result.expected_events << " events_per_sec="
              << std::setprecision(0) << rate << " errors=" << result.errors
              << " watchdog=" << (result.timed_out ? "timeout" : "ok")
              << '\n';
}

double Median(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    if (values.empty()) {
        return 0.0;
    }
    const auto middle = values.size() / 2;
    if ((values.size() % 2) != 0) {
        return values[middle];
    }
    return (values[middle - 1] + values[middle]) / 2.0;
}

}  // namespace

/** @brief 交替运行两种模型，验证并输出统一口径的 readiness 基准。 */
int main() {
    try {
        const auto options = ReadOptions();
        rlimit limit{};
        if (::getrlimit(RLIMIT_NOFILE, &limit) != 0) {
            throw std::system_error(errno, std::generic_category(),
                                    "getrlimit failed");
        }
        const auto required_fds = options.descriptors * 2 + 64;
        if (limit.rlim_cur != RLIM_INFINITY && limit.rlim_cur < required_fds) {
            throw std::runtime_error(
                "RLIMIT_NOFILE is too small; raise ulimit -n for this run");
        }

        const auto setup_begin = Clock::now();
        PipeSet pipes(options.descriptors);
        const double pipe_setup_ms =
            std::chrono::duration<double, std::milli>(Clock::now() - setup_begin)
                .count();

        std::cout << "公平 IO readiness 基准：fd=" << options.descriptors
                  << " waves=" << options.waves
                  << " workers=" << options.workers
                  << " poller=1 repeats=" << options.repeats
                  << " 每模型线程数=" << options.workers + 1 << '\n'
                  << "两边共用预创建 pipe；每轮逐 fd 写 1 字节，全部消费后再发下一轮；"
                     "setup 与稳态分别计时。\n"
                  << "pipe_setup_ms=" << std::fixed << std::setprecision(2)
                  << pipe_setup_ms << '\n';

        std::vector<double> fiber_times;
        std::vector<double> thread_times;
        fiber_times.reserve(options.repeats);
        thread_times.reserve(options.repeats);
        bool failed = false;

        for (std::size_t repeat = 0; repeat < options.repeats; ++repeat) {
            // 交替执行先后顺序，降低缓存和温度变化造成的顺序偏差。
            const bool thread_first = (repeat % 2) == 0;
            for (int position = 0; position < 2; ++position) {
                const bool run_thread = (position == 0) == thread_first;
                const char* model = run_thread ? "thread-pool+epoll"
                                               : "IOManager+Fiber";
                const auto result = run_thread
                                        ? RunThreadEpoll(pipes, options)
                                        : RunFiberIO(pipes, options);
                PrintResult(model, repeat + 1, result);
                failed = failed || result.timed_out || result.errors != 0 ||
                         result.completed_events != result.expected_events;
                (run_thread ? thread_times : fiber_times)
                    .push_back(result.steady_ms);
                CheckPipesEmpty(pipes);
            }
        }

        const auto thread_median = Median(thread_times);
        const auto fiber_median = Median(fiber_times);
        std::cout << "median_steady_ms: thread-pool+epoll=" << thread_median
                  << " IOManager+Fiber=" << fiber_median
                  << " ratio_fiber_over_thread="
                  << std::setprecision(3)
                  << (thread_median > 0.0 ? fiber_median / thread_median : 0.0)
                  << '\n';
        return failed ? 2 : 0;
    } catch (const std::exception& error) {
        std::cerr << "IO readiness benchmark failed: " << error.what() << '\n';
        return 1;
    }
}
