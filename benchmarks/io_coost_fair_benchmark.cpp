/**
 * @file io_coost_fair_benchmark.cpp
 * @brief 使用 coost 原生 coroutine/epoll 对同一批 pipe 做公平 IO 基准。
 *
 * 这个基准只用于建立可复现的外部参照，不把历史宣传数据当作本次结果。
 * 线程侧与协程侧复用同一批非阻塞 pipe、同样的 readiness 波次、payload
 * 校验、固定 worker 数和 watchdog。coost 侧通过 co::io_event 注册 fd，
 * 由 coost 自带的每 scheduler epoll 驱动；线程侧使用一个 epoll 轮询线程
 * 和固定数量的 worker。两侧都在一轮全部消费完以后才开始下一轮。
 */

#include <co/co.h>
#include <co/flag.h>

#include <sys/epoll.h>
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

/** @brief 预创建两种模型共用的非阻塞 pipe，避免 fd 分配差异污染结果。 */
class PipeSet final {
public:
    explicit PipeSet(std::size_t count) : m_pairs(count) {
        try {
            for (auto& pair : m_pairs) {
                int fds[2]{-1, -1};
                if (::pipe2(fds, O_NONBLOCK | O_CLOEXEC) != 0) {
                    throw std::system_error(errno, std::generic_category(),
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
 * @brief 用谓词等待精确的完成数量。
 * @details 计数使用 release/acquire，条件变量只负责降低等待开销。
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

void WriteByte(int fd, std::uint8_t byte) {
    for (;;) {
        const long result = ::syscall(SYS_write, fd, &byte, sizeof(byte));
        if (result == static_cast<long>(sizeof(byte))) return;
        if (result < 0 && errno == EINTR) continue;
        throw std::system_error(result < 0 ? errno : EIO,
                                std::generic_category(), "pipe write failed");
    }
}

std::uint8_t ReadByte(int fd) {
    std::uint8_t byte = 0;
    for (;;) {
        const long result = ::syscall(SYS_read, fd, &byte, sizeof(byte));
        if (result == static_cast<long>(sizeof(byte))) return byte;
        if (result < 0 && errno == EINTR) continue;
        throw std::system_error(result < 0 ? errno : EIO,
                                std::generic_category(), "pipe read failed");
    }
}

std::uint8_t Payload(std::size_t wave) noexcept {
    return static_cast<std::uint8_t>('A' + wave % 26);
}

void SendWave(const PipeSet& pipes, std::size_t wave) {
    const auto byte = Payload(wave);
    for (std::size_t index = 0; index < pipes.size(); ++index) {
        WriteByte(pipes[index].write_fd, byte);
    }
}

template <typename AbortPredicate>
std::pair<double, bool> DriveWaves(const PipeSet& pipes, std::size_t waves,
                                   Completion& completed,
                                   std::atomic<std::uint64_t>& errors,
                                   std::atomic<std::size_t>& expected_wave,
                                   AbortPredicate abort_requested) {
    const auto begin = Clock::now();
    bool timed_out = false;
    for (std::size_t wave = 0; wave < waves; ++wave) {
        if (abort_requested()) break;
        expected_wave.store(wave, std::memory_order_release);
        SendWave(pipes, wave);
        const auto target = static_cast<std::uint64_t>(wave + 1) * pipes.size();
        if (!completed.WaitFor(target, Clock::now() + kWatchdog)) {
            timed_out = true;
            errors.fetch_add(1, std::memory_order_relaxed);
            break;
        }
    }
    return {std::chrono::duration<double, std::milli>(Clock::now() - begin).count(),
            timed_out};
}

RunResult RunThreadEpoll(const Options& options, const PipeSet& pipes) {
    RunResult result;
    result.expected_events = options.descriptors * options.waves;
    std::atomic<std::uint64_t> errors{0};
    Completion completed;
    std::atomic<bool> stop{false};
    std::atomic<std::size_t> expected_wave{0};
    std::atomic<std::size_t> ready{0};
    const auto setup_begin = Clock::now();
    const int epoll_fd = ::epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd < 0) throw std::system_error(errno, std::generic_category(),
                                              "epoll_create1 failed");

    for (std::size_t index = 0; index < pipes.size(); ++index) {
        epoll_event event{};
        // 与 Go2Cpp fair benchmark 相同：worker 消费后再 rearm，避免重复排队。
        event.events = EPOLLIN | EPOLLERR | EPOLLHUP | EPOLLONESHOT;
        event.data.u32 = static_cast<std::uint32_t>(index);
        if (::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, pipes[index].read_fd, &event) != 0) {
            const int error = errno;
            ::close(epoll_fd);
            throw std::system_error(error, std::generic_category(),
                                    "epoll_ctl ADD failed");
        }
    }

    std::mutex queue_mutex;
    std::condition_variable queue_condition;
    std::vector<std::size_t> queue;
    queue.reserve(options.descriptors);
    auto worker = [&] {
        ready.fetch_add(1, std::memory_order_release);
        for (;;) {
            std::size_t index = 0;
            {
                std::unique_lock<std::mutex> lock(queue_mutex);
                queue_condition.wait(lock, [&] {
                    return stop.load(std::memory_order_acquire) || !queue.empty();
                });
                if (queue.empty() && stop.load(std::memory_order_acquire)) return;
                index = queue.back();
                queue.pop_back();
            }
            try {
                const auto value = ReadByte(pipes[index].read_fd);
                const auto wave = expected_wave.load(std::memory_order_acquire);
                if (value != Payload(wave)) {
                    errors.fetch_add(1, std::memory_order_relaxed);
                }
            } catch (...) {
                errors.fetch_add(1, std::memory_order_relaxed);
            }
            epoll_event event{};
            event.events = EPOLLIN | EPOLLERR | EPOLLHUP | EPOLLONESHOT;
            event.data.u32 = static_cast<std::uint32_t>(index);
            if (::epoll_ctl(epoll_fd, EPOLL_CTL_MOD, pipes[index].read_fd, &event) != 0) {
                errors.fetch_add(1, std::memory_order_relaxed);
            }
            completed.Complete();
        }
    };
    std::vector<std::thread> workers;
    workers.reserve(options.workers);
    for (std::size_t worker_index = 0; worker_index < options.workers; ++worker_index) {
        workers.emplace_back(worker);
    }
    while (ready.load(std::memory_order_acquire) != options.workers) {
        std::this_thread::yield();
    }
    result.setup_ms = std::chrono::duration<double, std::milli>(Clock::now() - setup_begin).count();

    std::thread poller([&] {
        std::array<epoll_event, 256> events{};
        while (!stop.load(std::memory_order_acquire)) {
            const int count = ::epoll_wait(epoll_fd, events.data(),
                                           static_cast<int>(events.size()), 100);
            if (count < 0 && errno == EINTR) continue;
            if (count < 0) {
                errors.fetch_add(1, std::memory_order_relaxed);
                break;
            }
            if (count == 0) continue;
            {
                std::lock_guard<std::mutex> lock(queue_mutex);
                for (int i = 0; i < count; ++i) queue.push_back(events[i].data.u32);
            }
            queue_condition.notify_all();
        }
    });

    const auto steady = DriveWaves(pipes, options.waves, completed, errors,
                                   expected_wave, [&] {
        return result.timed_out;
    });
    result.steady_ms = steady.first;
    result.timed_out = steady.second;
    stop.store(true, std::memory_order_release);
    queue_condition.notify_all();
    poller.join();
    for (auto& thread : workers) thread.join();
    ::close(epoll_fd);
    result.completed_events = completed.count();
    result.errors = errors.load(std::memory_order_acquire);
    return result;
}

RunResult RunCoost(const Options& options, const PipeSet& pipes) {
    RunResult result;
    result.expected_events = options.descriptors * options.waves;
    std::atomic<std::uint64_t> errors{0};
    std::atomic<std::size_t> registered{0};
    std::atomic<std::size_t> finished{0};
    Completion completed;
    std::mutex setup_mutex;
    std::condition_variable setup_condition;
    const auto setup_begin = Clock::now();

    for (std::size_t index = 0; index < pipes.size(); ++index) {
        co::go([&, index] {
            co::io_event event(pipes[index].read_fd, co::ev_read);
            // coost 的 Linux Epoll 要求注册发生在当前 coroutine 所属 scheduler。
            // 直接注册并标记 _added，析构时仍由 io_event 删除注册。
            co::add_io_event(pipes[index].read_fd, co::ev_read);
            event._added = true;
            registered.fetch_add(1, std::memory_order_release);
            setup_condition.notify_all();
            co::yield();
            for (std::size_t wave = 0; wave < options.waves; ++wave) {
                try {
                    const auto value = ReadByte(pipes[index].read_fd);
                    if (value != Payload(wave)) {
                        errors.fetch_add(1, std::memory_order_relaxed);
                    }
                } catch (...) {
                    errors.fetch_add(1, std::memory_order_relaxed);
                }
                completed.Complete();
                if (wave + 1 != options.waves) co::yield();
            }
            finished.fetch_add(1, std::memory_order_release);
        });
    }

    {
        std::unique_lock<std::mutex> lock(setup_mutex);
        if (!setup_condition.wait_for(lock, kWatchdog, [&] {
                return registered.load(std::memory_order_acquire) == pipes.size();
            })) {
            result.timed_out = true;
            errors.fetch_add(1, std::memory_order_relaxed);
        }
    }
    result.setup_ms = std::chrono::duration<double, std::milli>(Clock::now() - setup_begin).count();

    const auto steady_begin = Clock::now();
    for (std::size_t wave = 0; wave < options.waves && !result.timed_out; ++wave) {
        SendWave(pipes, wave);
        const auto target = static_cast<std::uint64_t>(wave + 1) * pipes.size();
        if (!completed.WaitFor(target, Clock::now() + kWatchdog)) {
            result.timed_out = true;
            errors.fetch_add(1, std::memory_order_relaxed);
            break;
        }
    }
    result.steady_ms = std::chrono::duration<double, std::milli>(Clock::now() - steady_begin).count();
    if (!result.timed_out) {
        const auto deadline = Clock::now() + kWatchdog;
        while (finished.load(std::memory_order_acquire) != pipes.size() && Clock::now() < deadline) {
            std::this_thread::yield();
        }
        if (finished.load(std::memory_order_acquire) != pipes.size()) {
            result.timed_out = true;
            errors.fetch_add(1, std::memory_order_relaxed);
        }
    }
    result.completed_events = completed.count();
    result.errors = errors.load(std::memory_order_acquire);
    return result;
}

Options ParseOptions(int argc, char** argv) {
    Options options;
    if (argc > 1) options.descriptors = std::strtoull(argv[1], nullptr, 10);
    if (argc > 2) options.waves = std::strtoull(argv[2], nullptr, 10);
    if (argc > 3) options.workers = std::strtoull(argv[3], nullptr, 10);
    if (argc > 4) options.repeats = std::strtoull(argv[4], nullptr, 10);
    if (options.descriptors == 0 || options.waves == 0 || options.workers == 0 || options.repeats == 0) {
        throw std::invalid_argument("descriptors/waves/workers/repeats must be positive");
    }
    return options;
}

void PrintResult(const char* name, const RunResult& result) {
    const double ratio = result.expected_events == 0
                             ? 0.0
                             : static_cast<double>(result.completed_events) /
                                   static_cast<double>(result.expected_events);
    std::cout << std::left << std::setw(14) << name
              << " setup_ms=" << std::fixed << std::setprecision(3) << result.setup_ms
              << " steady_ms=" << result.steady_ms
              << " completed=" << result.completed_events << "/" << result.expected_events
              << " complete_ratio=" << ratio
              << " errors=" << result.errors
              << " timed_out=" << (result.timed_out ? "yes" : "no") << '\n';
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const Options options = ParseOptions(argc, argv);
        // coost 的 scheduler 在 flag::parse 的 after-parse 回调中启动。
        // 设为与线程基准相同的 8 个 worker，避免默认 CPU 数造成不可比。
        if (!flag::set_value("co_sched_num", std::to_string(options.workers).c_str())) {
            throw std::runtime_error("coost does not expose co_sched_num");
        }
        char program[] = "io_coost_fair_benchmark";
        char* arguments[] = {program, nullptr};
        (void)flag::parse(1, arguments, true);

        std::cout << "coost fair IO benchmark: descriptors=" << options.descriptors
                  << " waves=" << options.waves << " workers=" << options.workers
                  << " repeats=" << options.repeats << '\n';
        std::cout << "coost source commit: c1cc11b32d5208912675a98fe091686848e60bc3\n";

        for (std::size_t repeat = 0; repeat < options.repeats; ++repeat) {
            PipeSet pipes(options.descriptors);
            const auto thread_result = RunThreadEpoll(options, pipes);
            const auto coost_result = RunCoost(options, pipes);
            PrintResult("thread-epoll", thread_result);
            PrintResult("coost", coost_result);
            if (thread_result.errors != 0 || coost_result.errors != 0 ||
                thread_result.timed_out || coost_result.timed_out ||
                thread_result.completed_events != thread_result.expected_events ||
                coost_result.completed_events != coost_result.expected_events) {
                std::cerr << "benchmark validation failed at repeat " << repeat << '\n';
                return EXIT_FAILURE;
            }
        }
    } catch (const std::exception& error) {
        std::cerr << "fatal: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
