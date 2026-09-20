#include "go2cpp/io.hpp"
#include "test_support.hpp"

#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using go2cpp::IOEvent;
using go2cpp::IOManager;
using go2cpp::IOWaitStatus;

bool wait_until(const std::atomic<bool>& flag,
                std::chrono::milliseconds timeout = 2s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!flag.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(100us);
    }
    return flag.load(std::memory_order_acquire);
}

bool make_pair(int (&fds)[2]) {
    return ::socketpair(AF_UNIX,
                        SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                        0, fds) == 0;
}

void raw_close(int fd) {
    if (fd >= 0) {
        (void)::syscall(SYS_close, fd);
    }
}

void close_pair(IOManager& manager, int (&fds)[2]) {
    go2cpp::io::DescriptorGuard guard;
    (void)manager.NotifyClose(fds[0]);
    (void)manager.NotifyClose(fds[1]);
    raw_close(fds[0]);
    raw_close(fds[1]);
    fds[0] = -1;
    fds[1] = -1;
}

go2cpp::SchedulerConfig one_worker_config() {
    go2cpp::SchedulerConfig config;
    config.processor_count = 1;
    config.max_workers = 1;
    return config;
}

void test_single_p_and_wait_results() {
    IOManager manager(one_worker_config());
    GO2CPP_CHECK(manager.Start());

    int cooperative[2]{-1, -1};
    GO2CPP_CHECK(make_pair(cooperative));
    std::atomic<bool> reader_entered{false};
    std::atomic<bool> reader_done{false};
    std::atomic<bool> writer_ran{false};
    std::atomic<IOWaitStatus> reader_status{IOWaitStatus::kError};

    manager.Go([&] {
        GO2CPP_CHECK(IOManager::Current() == &manager);
        reader_entered.store(true, std::memory_order_release);
        const auto result = manager.WaitFor(cooperative[0], IOEvent::kRead, 1s);
        reader_status.store(result.status, std::memory_order_release);
        if (result.ready()) {
            char byte = 0;
            GO2CPP_CHECK(::syscall(SYS_read, cooperative[0], &byte, 1) == 1);
            GO2CPP_CHECK(byte == 'x');
        }
        reader_done.store(true, std::memory_order_release);
    });
    GO2CPP_CHECK(wait_until(reader_entered));
    manager.Go([&] {
        const char byte = 'x';
        GO2CPP_CHECK(::syscall(SYS_write, cooperative[1], &byte, 1) == 1);
        writer_ran.store(true, std::memory_order_release);
    });
    GO2CPP_CHECK(wait_until(reader_done));
    GO2CPP_CHECK(writer_ran.load(std::memory_order_acquire));
    GO2CPP_CHECK(reader_status.load(std::memory_order_acquire) ==
                  IOWaitStatus::kReady);
    close_pair(manager, cooperative);

    int immediate[2]{-1, -1};
    GO2CPP_CHECK(make_pair(immediate));
    const char immediate_byte = 'i';
    GO2CPP_CHECK(::syscall(SYS_write, immediate[1], &immediate_byte, 1) == 1);
    std::atomic<bool> immediate_done{false};
    std::atomic<IOWaitStatus> immediate_status{IOWaitStatus::kError};
    manager.Go([&] {
        const auto result = manager.WaitFor(immediate[0], IOEvent::kRead, 1s);
        immediate_status.store(result.status, std::memory_order_release);
        immediate_done.store(true, std::memory_order_release);
    });
    GO2CPP_CHECK(wait_until(immediate_done));
    GO2CPP_CHECK(immediate_status.load(std::memory_order_acquire) ==
                  IOWaitStatus::kReady);
    close_pair(manager, immediate);

    int timeout_pair[2]{-1, -1};
    GO2CPP_CHECK(make_pair(timeout_pair));
    std::atomic<bool> timeout_done{false};
    std::atomic<IOWaitStatus> timeout_status{IOWaitStatus::kError};
    manager.Go([&] {
        const auto result = manager.WaitFor(timeout_pair[0], IOEvent::kRead,
                                            10ms);
        timeout_status.store(result.status, std::memory_order_release);
        timeout_done.store(true, std::memory_order_release);
    });
    GO2CPP_CHECK(wait_until(timeout_done));
    GO2CPP_CHECK(timeout_status.load(std::memory_order_acquire) ==
                  IOWaitStatus::kTimeout);
    close_pair(manager, timeout_pair);

    int context_pair[2]{-1, -1};
    GO2CPP_CHECK(make_pair(context_pair));
    auto cancellation = go2cpp::WithCancel(go2cpp::Background());
    std::atomic<bool> context_entered{false};
    std::atomic<bool> context_done{false};
    std::atomic<IOWaitStatus> context_status{IOWaitStatus::kError};
    manager.Go([&] {
        context_entered.store(true, std::memory_order_release);
        const auto result = manager.Wait(context_pair[0], IOEvent::kRead,
                                         std::nullopt, cancellation.first);
        context_status.store(result.status, std::memory_order_release);
        context_done.store(true, std::memory_order_release);
    });
    GO2CPP_CHECK(wait_until(context_entered));
    cancellation.second();
    GO2CPP_CHECK(wait_until(context_done));
    GO2CPP_CHECK(context_status.load(std::memory_order_acquire) ==
                  IOWaitStatus::kCancelled);
    close_pair(manager, context_pair);

    int cancelled_pair[2]{-1, -1};
    GO2CPP_CHECK(make_pair(cancelled_pair));
    std::atomic<bool> cancel_entered{false};
    std::atomic<bool> cancel_done{false};
    std::atomic<IOWaitStatus> cancel_status{IOWaitStatus::kError};
    manager.Go([&] {
        cancel_entered.store(true, std::memory_order_release);
        const auto result = manager.Wait(cancelled_pair[0], IOEvent::kRead);
        cancel_status.store(result.status, std::memory_order_release);
        cancel_done.store(true, std::memory_order_release);
    });
    GO2CPP_CHECK(wait_until(cancel_entered));
    const auto cancel_deadline = std::chrono::steady_clock::now() + 1s;
    while (!manager.Cancel(cancelled_pair[0], IOEvent::kRead) &&
           !cancel_done.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < cancel_deadline) {
        go2cpp_tests::yield_for_watchdog();
    }
    GO2CPP_CHECK(wait_until(cancel_done));
    GO2CPP_CHECK(cancel_status.load(std::memory_order_acquire) ==
                  IOWaitStatus::kCancelled);
    close_pair(manager, cancelled_pair);

    int closed_pair[2]{-1, -1};
    GO2CPP_CHECK(make_pair(closed_pair));
    std::atomic<bool> close_entered{false};
    std::atomic<bool> close_done{false};
    std::atomic<IOWaitStatus> close_status{IOWaitStatus::kError};
    manager.Go([&] {
        close_entered.store(true, std::memory_order_release);
        const auto result = manager.Wait(closed_pair[0], IOEvent::kRead);
        close_status.store(result.status, std::memory_order_release);
        close_done.store(true, std::memory_order_release);
    });
    GO2CPP_CHECK(wait_until(close_entered));
    const auto close_deadline = std::chrono::steady_clock::now() + 1s;
    while (!manager.NotifyClose(closed_pair[0]) &&
           !close_done.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < close_deadline) {
        go2cpp_tests::yield_for_watchdog();
    }
    raw_close(closed_pair[0]);
    closed_pair[0] = -1;
    GO2CPP_CHECK(wait_until(close_done));
    GO2CPP_CHECK(close_status.load(std::memory_order_acquire) ==
                  IOWaitStatus::kClosed);
    close_pair(manager, closed_pair);

    manager.Shutdown();
}

void test_shutdown_wakes_infinite_wait() {
    IOManager manager(one_worker_config());
    GO2CPP_CHECK(manager.Start());
    int fds[2]{-1, -1};
    GO2CPP_CHECK(make_pair(fds));
    std::atomic<bool> entered{false};
    std::atomic<bool> returned{false};
    std::atomic<IOWaitStatus> status{IOWaitStatus::kError};
    manager.Go([&] {
        entered.store(true, std::memory_order_release);
        const auto result = manager.Wait(fds[0], IOEvent::kRead);
        status.store(result.status, std::memory_order_release);
        returned.store(true, std::memory_order_release);
    });
    GO2CPP_CHECK(wait_until(entered));
    std::this_thread::sleep_for(2ms);
    manager.Shutdown();
    GO2CPP_CHECK(returned.load(std::memory_order_acquire));
    GO2CPP_CHECK(status.load(std::memory_order_acquire) ==
                  IOWaitStatus::kCancelled);
    raw_close(fds[0]);
    raw_close(fds[1]);
}

void test_readiness_timeout_race() {
    go2cpp::SchedulerConfig config;
    config.processor_count = 4;
    config.max_workers = 4;
    IOManager manager(config);
    GO2CPP_CHECK(manager.Start());

    constexpr int kRounds = 10000;
    constexpr int kBatch = 32;
    std::atomic<int> ready{0};
    std::atomic<int> timed_out{0};
    std::atomic<int> invalid{0};

    for (int base = 0; base < kRounds; base += kBatch) {
        const int count = std::min(kBatch, kRounds - base);
        std::vector<std::pair<int, int>> pairs;
        pairs.reserve(static_cast<std::size_t>(count));
        std::atomic<int> done{0};
        for (int index = 0; index < count; ++index) {
            int fds[2]{-1, -1};
            GO2CPP_CHECK(make_pair(fds));
            pairs.emplace_back(fds[0], fds[1]);
            const int read_fd = fds[0];
            manager.Go([&, read_fd] {
                const auto result =
                    manager.WaitFor(read_fd, IOEvent::kRead, 2ms);
                if (result.status == IOWaitStatus::kReady) {
                    ready.fetch_add(1, std::memory_order_relaxed);
                } else if (result.status == IOWaitStatus::kTimeout) {
                    timed_out.fetch_add(1, std::memory_order_relaxed);
                } else {
                    invalid.fetch_add(1, std::memory_order_relaxed);
                }
                done.fetch_add(1, std::memory_order_release);
            });
        }

        for (int index = 0; index < count; index += 2) {
            const char byte = 'r';
            (void)::syscall(SYS_write, pairs[static_cast<std::size_t>(index)].second,
                            &byte, 1);
        }

        const auto watchdog = std::chrono::steady_clock::now() + 2s;
        while (done.load(std::memory_order_acquire) != count &&
               std::chrono::steady_clock::now() < watchdog) {
            std::this_thread::sleep_for(100us);
        }
        GO2CPP_CHECK(done.load(std::memory_order_acquire) == count);
        for (const auto& pair : pairs) {
            (void)manager.NotifyClose(pair.first);
            (void)manager.NotifyClose(pair.second);
            raw_close(pair.first);
            raw_close(pair.second);
        }
    }

    GO2CPP_CHECK(invalid.load(std::memory_order_acquire) == 0);
    GO2CPP_CHECK(ready.load(std::memory_order_acquire) +
                      timed_out.load(std::memory_order_acquire) ==
                  kRounds);
    manager.Shutdown();
}

void test_sequential_and_spurious_waits() {
    IOManager manager(one_worker_config());
    GO2CPP_CHECK(manager.Start());
    int fds[2]{-1, -1};
    GO2CPP_CHECK(make_pair(fds));
    const char byte = 's';
    GO2CPP_CHECK(::syscall(SYS_write, fds[1], &byte, 1) == 1);
    std::atomic<bool> second_entered{false};
    auto waiter = manager.Go([&] {
        GO2CPP_CHECK(manager.WaitFor(fds[0], IOEvent::Read, 1s).ready());
        char received = 0;
        GO2CPP_CHECK(::syscall(SYS_read, fds[0], &received, 1) == 1);
        second_entered.store(true, std::memory_order_release);
        GO2CPP_CHECK(manager.WaitFor(fds[0], IOEvent::Read, 20ms).status ==
                     IOWaitStatus::Timeout);
    });
    GO2CPP_CHECK(wait_until(second_entered));
    for (int index = 0; index < 16; ++index) {
        (void)manager.scheduler().wake(waiter);
        std::this_thread::sleep_for(100us);
    }
    GO2CPP_CHECK(waiter->wait_for(1s));
    close_pair(manager, fds);
    manager.Shutdown();
}

void test_generation_reuse_and_cross_manager_close() {
    IOManager first(one_worker_config());
    IOManager second(one_worker_config());
    GO2CPP_CHECK(first.Start());
    GO2CPP_CHECK(second.Start());
    int fds[2]{-1, -1};
    GO2CPP_CHECK(make_pair(fds));
    const int target = fds[0];
    const auto old_token = go2cpp::io::DescriptorGuard::Capture(target);
    std::atomic<int> entered{0};
    auto one = first.Go([&] {
        entered.fetch_add(1, std::memory_order_release);
        GO2CPP_CHECK(first.Wait(target, IOEvent::Read).status == IOWaitStatus::Closed);
    });
    auto two = second.Go([&] {
        entered.fetch_add(1, std::memory_order_release);
        GO2CPP_CHECK(second.Wait(target, IOEvent::Read).status == IOWaitStatus::Closed);
    });
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while ((entered.load(std::memory_order_acquire) != 2 ||
            one->state() != go2cpp::GState::Waiting ||
            two->state() != go2cpp::GState::Waiting) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(100us);
    }
    GO2CPP_CHECK(entered.load(std::memory_order_acquire) == 2);
    {
        go2cpp::io::DescriptorGuard guard;
        IOManager::NotifyCloseAll(target);
        raw_close(target);
        fds[0] = -1;
    }
    GO2CPP_CHECK(one->wait_for(1s));
    GO2CPP_CHECK(two->wait_for(1s));
    GO2CPP_CHECK(!old_token->valid());
    int replacement[2]{-1, -1};
    GO2CPP_CHECK(make_pair(replacement));
    {
        go2cpp::io::DescriptorGuard guard;
        if (replacement[0] != target) {
            IOManager::NotifyCloseAll(target);
            GO2CPP_CHECK(::syscall(SYS_dup2, replacement[0], target) == target);
        }
    }
    auto stale = first.Go([&] {
        const auto result = first.wait(target, IOEvent::Read, std::nullopt,
                                        {}, old_token);
        GO2CPP_CHECK(result.status == IOWaitStatus::Error);
        GO2CPP_CHECK(result.system_error == EBADF);
    });
    GO2CPP_CHECK(stale->wait_for(1s));
    if (replacement[0] != target) {
        IOManager::NotifyCloseAll(target);
        raw_close(target);
    }
    close_pair(first, replacement);
    close_pair(first, fds);
    first.Shutdown();
    second.Shutdown();
}

}  // namespace

void run_io_tests() {
    go2cpp_tests::announce("Linux epoll IO manager and cancellation races");
    test_single_p_and_wait_results();
    test_shutdown_wakes_infinite_wait();
    test_readiness_timeout_race();
    test_sequential_and_spurious_waits();
    test_generation_reuse_and_cross_manager_close();
}
