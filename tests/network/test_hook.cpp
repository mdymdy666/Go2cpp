#include "go2cpp/hook.hpp"
#include "go2cpp/fiber.hpp"
#include "test_support.hpp"

#include <fcntl.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <memory>
#include <mutex>
#include <chrono>
#include <thread>
#include <vector>

#if defined(__has_include)
#if __has_include(<valgrind/valgrind.h>)
#include <valgrind/valgrind.h>
#define GO2CPP_TEST_HAS_VALGRIND 1
#endif
#endif

namespace {

using namespace std::chrono_literals;

/**
 * @brief 构造 Hook 测试使用的单 Worker 配置。
 * @return 适合测试 IO 阻塞转让的 SchedulerConfig 值。
 */
go2cpp::SchedulerConfig one_worker_config() {
    go2cpp::SchedulerConfig config;
    config.processor_count = 1;
    config.min_workers = 1;
    config.max_workers = 1;
    return config;
}

bool wait_until(const std::atomic<bool>& value,
                std::chrono::milliseconds timeout = 2s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!value.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(100us);
    }
    return value.load(std::memory_order_acquire);
}

template <typename Predicate>
bool wait_until_predicate(Predicate predicate,
                          std::chrono::milliseconds timeout = 2s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(100us);
    }
    return predicate();
}

/**
 * @brief 创建一对供 Hook 测试使用的本地 socket。
 * @param fds 输出的两个文件描述符，成功时均为非负值。
 * @return 创建成功返回 true。
 */
bool make_pair(int (&fds)[2]) {
    return ::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) == 0;
}

/**
 * @brief 关闭并清空 Hook 测试使用的 socket 对。
 * @param fds 待关闭的两个文件描述符。
 * @return 无。
 */
void close_pair(int (&fds)[2]) {
    if (fds[0] >= 0) {
        (void)::close(fds[0]);
        fds[0] = -1;
    }
    if (fds[1] >= 0) {
        (void)::close(fds[1]);
        fds[1] = -1;
    }
}

void test_transparent_read_and_sleep() {
    go2cpp::IOManager manager(one_worker_config());
    GO2CPP_CHECK(manager.Start());

    int fds[2]{-1, -1};
    GO2CPP_CHECK(make_pair(fds));
    std::atomic<bool> reader_entered{false};
    std::atomic<bool> reader_done{false};
    std::atomic<bool> writer_done{false};
    std::atomic<char> received{0};

    auto reader = manager.Go([&] {
        reader_entered.store(true, std::memory_order_release);
        char byte = 0;
        const ssize_t count = ::read(fds[0], &byte, 1);
        GO2CPP_CHECK(count == 1);
        received.store(byte, std::memory_order_release);
        reader_done.store(true, std::memory_order_release);
    });
    GO2CPP_CHECK(wait_until(reader_entered));
    auto writer = manager.Go([&] {
        const char value = 'h';
        GO2CPP_CHECK(::write(fds[1], &value, 1) == 1);
        writer_done.store(true, std::memory_order_release);
    });
    GO2CPP_CHECK(reader->wait_for(2s));
    GO2CPP_CHECK(writer->wait_for(2s));
    GO2CPP_CHECK(reader_done.load(std::memory_order_acquire));
    GO2CPP_CHECK(writer_done.load(std::memory_order_acquire));
    GO2CPP_CHECK(received.load(std::memory_order_acquire) == 'h');
    close_pair(fds);

    std::atomic<bool> sleeping{false};
    std::atomic<bool> sleeper_done{false};
    std::atomic<bool> peer_ran_while_sleeping{false};
    auto sleeper = manager.Go([&] {
        sleeping.store(true, std::memory_order_release);
        GO2CPP_CHECK(::usleep(20'000) == 0);
        sleeper_done.store(true, std::memory_order_release);
    });
    GO2CPP_CHECK(wait_until(sleeping));
    auto peer = manager.Go([&] {
        peer_ran_while_sleeping.store(
            !sleeper_done.load(std::memory_order_acquire),
            std::memory_order_release);
    });
    GO2CPP_CHECK(sleeper->wait_for(2s));
    GO2CPP_CHECK(peer->wait_for(2s));
    GO2CPP_CHECK(peer_ran_while_sleeping.load(std::memory_order_acquire));
    manager.Shutdown();
}

void test_timeout_and_user_nonblocking() {
    go2cpp::IOManager manager(one_worker_config());
    GO2CPP_CHECK(manager.Start());

    int timeout_fds[2]{-1, -1};
    GO2CPP_CHECK(make_pair(timeout_fds));
    const timeval timeout{0, 20'000};
    GO2CPP_CHECK(::setsockopt(timeout_fds[0], SOL_SOCKET, SO_RCVTIMEO,
                             &timeout, sizeof(timeout)) == 0);
    std::atomic<int> timeout_errno{0};
    auto timed = manager.Go([&] {
        char byte = 0;
        GO2CPP_CHECK(::recv(timeout_fds[0], &byte, 1, 0) == -1);
        timeout_errno.store(errno, std::memory_order_release);
    });
    GO2CPP_CHECK(timed->wait_for(2s));
    // SO_RCVTIMEO 到期按 POSIX 返回 EAGAIN（非阻塞语义），不是 ETIMEDOUT。
    GO2CPP_CHECK(timeout_errno.load(std::memory_order_acquire) == EAGAIN);
    close_pair(timeout_fds);

    int nonblocking_fds[2]{-1, -1};
    GO2CPP_CHECK(make_pair(nonblocking_fds));
    const int old_flags = ::fcntl(nonblocking_fds[0], F_GETFL);
    GO2CPP_CHECK(old_flags >= 0);
    GO2CPP_CHECK(::fcntl(nonblocking_fds[0], F_SETFL,
                         old_flags | O_NONBLOCK) == 0);
    std::atomic<int> nonblocking_errno{0};
    auto nonblocking = manager.Go([&] {
        char byte = 0;
        GO2CPP_CHECK(::recv(nonblocking_fds[0], &byte, 1, 0) == -1);
        nonblocking_errno.store(errno, std::memory_order_release);
    });
    GO2CPP_CHECK(nonblocking->wait_for(2s));
    const int error = nonblocking_errno.load(std::memory_order_acquire);
    GO2CPP_CHECK(error == EAGAIN || error == EWOULDBLOCK);
    close_pair(nonblocking_fds);
    manager.Shutdown();
}

void test_close_and_dup_metadata() {
    go2cpp::IOManager manager(one_worker_config());
    GO2CPP_CHECK(manager.Start());

    int close_fds[2]{-1, -1};
    GO2CPP_CHECK(make_pair(close_fds));
    std::atomic<bool> entered{false};
    std::atomic<int> close_errno{0};
    auto blocked = manager.Go([&] {
        entered.store(true, std::memory_order_release);
        char byte = 0;
        GO2CPP_CHECK(::read(close_fds[0], &byte, 1) == -1);
        close_errno.store(errno, std::memory_order_release);
    });
    GO2CPP_CHECK(wait_until(entered));
    std::this_thread::sleep_for(2ms);
    GO2CPP_CHECK(::close(close_fds[0]) == 0);
    close_fds[0] = -1;
    GO2CPP_CHECK(blocked->wait_for(2s));
    GO2CPP_CHECK(close_errno.load(std::memory_order_acquire) == EBADF);
    close_pair(close_fds);

    int dup_fds[2]{-1, -1};
    GO2CPP_CHECK(make_pair(dup_fds));
    const char value = 'd';
    GO2CPP_CHECK(::write(dup_fds[1], &value, 1) == 1);
    auto adopter = manager.Go([&] {
        char byte = 0;
        GO2CPP_CHECK(::read(dup_fds[0], &byte, 1) == 1);
        GO2CPP_CHECK(byte == value);
    });
    GO2CPP_CHECK(adopter->wait_for(2s));

    const int duplicate = ::dup(dup_fds[0]);
    GO2CPP_CHECK(duplicate >= 0);
    int flags = ::fcntl(duplicate, F_GETFL);
    GO2CPP_CHECK(flags >= 0);
    GO2CPP_CHECK((flags & O_NONBLOCK) == 0);
    GO2CPP_CHECK(::fcntl(duplicate, F_SETFL, flags | O_NONBLOCK) == 0);
    GO2CPP_CHECK((::fcntl(dup_fds[0], F_GETFL) & O_NONBLOCK) != 0);
    GO2CPP_CHECK(::fcntl(dup_fds[0], F_SETFL, flags & ~O_NONBLOCK) == 0);
    GO2CPP_CHECK((::fcntl(duplicate, F_GETFL) & O_NONBLOCK) == 0);
    GO2CPP_CHECK(::close(duplicate) == 0);
    close_pair(dup_fds);
    manager.Shutdown();
}

void test_native_fallback_and_failed_dup() {
    go2cpp::IOManager manager(one_worker_config());
    GO2CPP_CHECK(manager.Start());
    int fds[2]{-1, -1};
    GO2CPP_CHECK(make_pair(fds));
    const char adopted_byte = 'a';
    GO2CPP_CHECK(::write(fds[1], &adopted_byte, 1) == 1);
    auto adopter = manager.Go([&] {
        char byte = 0;
        GO2CPP_CHECK(::read(fds[0], &byte, 1) == 1);
    });
    GO2CPP_CHECK(adopter->wait_for(2s));

    // Once a Fiber has adopted the socket, an ordinary thread uses the
    // bounded native-poll fallback. Its absolute deadline must surface as
    // EAGAIN: POSIX socket(7) defines SO_RCVTIMEO expiry as EAGAIN, the
    // same errno as "no data available" on a nonblocking descriptor.
    const timeval native_timeout{0, 20'000};
    GO2CPP_CHECK(::setsockopt(fds[0], SOL_SOCKET, SO_RCVTIMEO, &native_timeout,
                              sizeof(native_timeout)) == 0);
    std::atomic<int> native_timeout_errno{0};
    std::thread timeout_reader([&] {
        char byte = 0;
        const ssize_t result = ::recv(fds[0], &byte, 1, 0);
        native_timeout_errno.store(result < 0 ? errno : 0,
                                   std::memory_order_release);
    });
    GO2CPP_JOIN_WITH_WATCHDOG(timeout_reader, 3s);
    GO2CPP_CHECK(native_timeout_errno.load(std::memory_order_acquire) ==
                 EAGAIN);

    // A failed replacement must not invalidate the still-open target.
    GO2CPP_CHECK(::dup2(-1, fds[0]) == -1);
    GO2CPP_CHECK(errno == EBADF);
    GO2CPP_CHECK(::fcntl(fds[0], F_GETFL) >= 0);
    std::atomic<bool> entered{false};
    std::atomic<bool> done{false};
    std::atomic<char> received{0};
    std::atomic<int> native_error{0};
    std::thread ordinary_reader([&] {
        entered.store(true, std::memory_order_release);
        char byte = 0;
        const ssize_t result = ::read(fds[0], &byte, 1);
        received.store(result == 1 ? byte : 0, std::memory_order_release);
        native_error.store(result < 0 ? errno : 0, std::memory_order_release);
        done.store(true, std::memory_order_release);
    });
    GO2CPP_CHECK(wait_until(entered));
    std::this_thread::sleep_for(2ms);
    GO2CPP_CHECK(!done.load(std::memory_order_acquire));
    const char byte = 'n';
    GO2CPP_CHECK(::write(fds[1], &byte, 1) == 1);
    const bool completed = wait_until(done);
    GO2CPP_CHECK(completed);
    if (!completed) {
        close_pair(fds);
    }
    GO2CPP_JOIN_WITH_WATCHDOG(ordinary_reader, 3s);
    GO2CPP_CHECK(received.load(std::memory_order_acquire) == byte);
    GO2CPP_CHECK(native_error.load(std::memory_order_acquire) == 0);
    close_pair(fds);

    GO2CPP_CHECK(::fcntl(-1, 0x7fffffff) == -1);
    GO2CPP_CHECK(errno == ENOTSUP);
    manager.Shutdown();
}

void test_ioctl_and_urgent_data_boundaries() {
    go2cpp::IOManager manager(one_worker_config());
    GO2CPP_CHECK(manager.Start());

    int fds[2]{-1, -1};
    GO2CPP_CHECK(make_pair(fds));
    const char value = 'f';
    GO2CPP_CHECK(::write(fds[1], &value, 1) == 1);
    auto adopter = manager.Go([&] {
        char byte = 0;
        GO2CPP_CHECK(::read(fds[0], &byte, 1) == 1);
        GO2CPP_CHECK(byte == value);
    });
    GO2CPP_CHECK(adopter->wait_for(2s));

    // A tracked FIONBIO call must preserve libc's EFAULT contract instead of
    // dereferencing an invalid user pointer inside the interposer.
#if defined(GO2CPP_TEST_HAS_VALGRIND)
    const bool under_valgrind = RUNNING_ON_VALGRIND != 0;
#else
    const bool under_valgrind = false;
#endif
    if (!under_valgrind) {
        const long page_size = ::sysconf(_SC_PAGESIZE);
        GO2CPP_CHECK(page_size > 0);
        void* invalid_pointer =
            ::mmap(nullptr, static_cast<std::size_t>(page_size), PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        GO2CPP_CHECK(invalid_pointer != MAP_FAILED);
        GO2CPP_CHECK(::ioctl(fds[0], FIONBIO, invalid_pointer) == -1);
        GO2CPP_CHECK(errno == EFAULT);
        GO2CPP_CHECK(::munmap(invalid_pointer,
                              static_cast<std::size_t>(page_size)) == 0);
    }
    GO2CPP_CHECK(::fcntl(fds[0], F_GETFL) >= 0);

#ifdef MSG_OOB
    std::atomic<int> urgent_errno{0};
    auto urgent = manager.Go([&] {
        char byte = 0;
        const ssize_t result = ::recv(fds[0], &byte, 1, MSG_OOB);
        urgent_errno.store(result < 0 ? errno : 0,
                           std::memory_order_release);
    });
    GO2CPP_CHECK(urgent->wait_for(2s));
    GO2CPP_CHECK(urgent_errno.load(std::memory_order_acquire) == ENOTSUP);
#endif

    close_pair(fds);
    manager.Shutdown();
}

void test_dup2_replacement_and_identity() {
    go2cpp::IOManager manager(one_worker_config());
    GO2CPP_CHECK(manager.Start());

    int target[2]{-1, -1};
    int source[2]{-1, -1};
    GO2CPP_CHECK(make_pair(target));
    GO2CPP_CHECK(make_pair(source));

    std::atomic<bool> entered{false};
    std::atomic<int> read_error{0};
    auto waiter = manager.Go([&] {
        entered.store(true, std::memory_order_release);
        char byte = 0;
        const ssize_t result = ::read(target[0], &byte, 1);
        read_error.store(result < 0 ? errno : 0, std::memory_order_release);
    });
    GO2CPP_CHECK(wait_until(entered));
    const auto waiting_deadline = std::chrono::steady_clock::now() + 2s;
    while (waiter->state() != go2cpp::GState::Waiting &&
           std::chrono::steady_clock::now() < waiting_deadline) {
        std::this_thread::sleep_for(100us);
    }
    GO2CPP_CHECK(waiter->state() == go2cpp::GState::Waiting);

    // Make the old registration observable if a stale epoll HUP wins the
    // replacement race. The hook must invalidate the old generation first,
    // so the waiter reports EBADF rather than consuming source data.
    const char source_byte = 's';
    GO2CPP_CHECK(::write(source[1], &source_byte, 1) == 1);
    GO2CPP_CHECK(::dup2(source[0], target[0]) == target[0]);
    GO2CPP_CHECK(waiter->wait_for(2s));
    GO2CPP_CHECK(read_error.load(std::memory_order_acquire) == EBADF);

    // Replacing a descriptor with itself is a no-op and must preserve the
    // newly cloned open description and its pending data.
    GO2CPP_CHECK(::dup2(target[0], target[0]) == target[0]);
    char received = 0;
    GO2CPP_CHECK(::syscall(SYS_read, target[0], &received, 1) == 1);
    GO2CPP_CHECK(received == source_byte);

    close_pair(target);
    close_pair(source);
    manager.Shutdown();
}

}  // namespace

void test_plain_scheduler_lazy_adoption_timeout() {
    // With hooks enabled but no IOManager, a managed Fiber still gets the
    // nonblocking descriptor plus native poll fallback and a real deadline.
    go2cpp::SchedulerConfig config;
    config.processor_count = 1;
    config.min_workers = 1;
    config.max_workers = 0;
    config.idle_worker_timeout = 10ms;
    go2cpp::Scheduler scheduler(config);
    scheduler.start();

    int fds[2]{-1, -1};
    GO2CPP_CHECK(make_pair(fds));
    timeval timeout{0, 40 * 1000};
    GO2CPP_CHECK(::setsockopt(fds[0], SOL_SOCKET, SO_RCVTIMEO, &timeout,
                              sizeof(timeout)) == 0);
    std::atomic<int> read_error{0};
    std::atomic<bool> peer_done{false};
    auto waiter = scheduler.spawn([&] {
        char byte = 0;
        const ssize_t result = ::recv(fds[0], &byte, 1, 0);
        read_error.store(result < 0 ? errno : 0, std::memory_order_release);
    });
    auto peer = scheduler.spawn([&] {
        peer_done.store(true, std::memory_order_release);
    });
    GO2CPP_CHECK(peer->wait_for(2s));
    GO2CPP_CHECK(peer_done.load(std::memory_order_acquire));
    GO2CPP_CHECK(waiter->wait_for(2s));
    GO2CPP_CHECK(read_error.load(std::memory_order_acquire) == EAGAIN);
    close_pair(fds);
    scheduler.shutdown();
}

void test_iomanager_untracked_fd_publishes_blocking() {
    go2cpp::SchedulerConfig config;
    config.processor_count = 1;
    config.min_workers = 1;
    config.max_workers = 2;
    config.sysmon_interval = 2ms;
    config.long_syscall_threshold = 10ms;
    config.idle_worker_timeout = 100ms;
    go2cpp::IOManager manager(config);
    GO2CPP_CHECK(manager.Start());

    // FIFO/pipe 现在会被 hook 接管并 park（见 test_fifo_read_parks），不再走
    // BlockingRegion/sysmon。这个用例要守住的契约仍然成立：**未接管的 fd**
    // 必须发布 M::Blocking，sysmon 才能为长系统调用申请替代 M。
    //
    // 因此换用 hook 明确不接管的 fd：eventfd 既不是 socket
    // （getsockopt → ENOTSOCK）也不是 FIFO（fstat → S_IFIFO 为假），
    // 它的 read 会真阻塞 worker —— 与改动前的 pipe 完全同类。
    // 不用 /dev/tty：CI 里没有控制终端，open 会 ENXIO。
    const int blocking_fd = ::eventfd(0, 0);
    GO2CPP_CHECK(blocking_fd >= 0);
    std::atomic<bool> entered{false};
    std::atomic<bool> peer_done{false};
    std::atomic<bool> detached{false};
    std::atomic<ssize_t> read_result{-2};
    auto reader = manager.Go([&] {
        entered.store(true, std::memory_order_release);
        std::uint64_t counter = 0;
        read_result.store(::read(blocking_fd, &counter, sizeof(counter)),
                          std::memory_order_release);
        GO2CPP_CHECK(counter == 1);
    });
    GO2CPP_CHECK(wait_until(entered));
    GO2CPP_CHECK(wait_until_predicate([&] {
        for (const auto& machine : manager.scheduler().machines()) {
            if (machine.processor_detached) {
                detached.store(true, std::memory_order_release);
                return true;
            }
        }
        return false;
    }, 2s));
    auto peer = manager.Go([&] {
        peer_done.store(true, std::memory_order_release);
    });
    GO2CPP_CHECK(wait_until(peer_done));
    GO2CPP_CHECK(detached.load(std::memory_order_acquire));
    const std::uint64_t signal = 1;
    GO2CPP_CHECK(::write(blocking_fd, &signal, sizeof(signal)) ==
                 static_cast<ssize_t>(sizeof(signal)));
    GO2CPP_CHECK(reader->wait_for(2s));
    GO2CPP_CHECK(read_result.load(std::memory_order_acquire) ==
                 static_cast<ssize_t>(sizeof(signal)));
    GO2CPP_CHECK(peer->wait_for(2s));
    (void)::close(blocking_fd);
    manager.Shutdown();
}

void test_native_fallback_grows_replacement_m() {
    go2cpp::SchedulerConfig config;
    config.processor_count = 1;
    config.min_workers = 1;
    config.max_workers = 0;
    config.idle_worker_timeout = 10ms;
    go2cpp::Scheduler scheduler(config);
    scheduler.start();

    std::atomic<bool> entered{false};
    std::atomic<int> peers_done{0};
    auto sleeper = scheduler.spawn([&] {
        go2cpp::ScopedThreadHookMode disabled(
            go2cpp::ThreadHookMode::kDisabled);
        entered.store(true, std::memory_order_release);
        const timespec request{0, 50 * 1000 * 1000};
        (void)::nanosleep(&request, nullptr);
    });
    GO2CPP_CHECK(wait_until(entered));

    std::vector<std::shared_ptr<go2cpp::Task>> peers;
    for (int index = 0; index < 4; ++index) {
        peers.emplace_back(scheduler.spawn([&] {
            peers_done.fetch_add(1, std::memory_order_release);
        }));
    }
    GO2CPP_CHECK(wait_until_predicate([&] {
        return peers_done.load(std::memory_order_acquire) == 4;
    }, 2s));
    GO2CPP_CHECK(scheduler.worker_count() >= 2);
    GO2CPP_CHECK(scheduler.worker_count() <= 2);
    GO2CPP_CHECK(sleeper->wait_for(2s));
    for (const auto& peer : peers) {
        GO2CPP_CHECK(peer->wait_for(2s));
    }
    GO2CPP_CHECK(wait_until_predicate(
        [&] { return scheduler.worker_count() == 1; }, 2s));
    scheduler.shutdown();
}


void test_managed_poll_fallback_publishes_blocking() {
    go2cpp::SchedulerConfig config;
    config.processor_count = 1;
    config.min_workers = 1;
    config.max_workers = 2;
    config.sysmon_interval = 2ms;
    config.long_syscall_threshold = 5ms;
    config.idle_worker_timeout = 100ms;
    go2cpp::Scheduler scheduler(config);
    scheduler.start();

    int pipe_fds[2]{-1, -1};
    GO2CPP_CHECK(::pipe(pipe_fds) == 0);
    std::atomic<bool> entered{false};
    std::atomic<bool> peer_done{false};
    std::atomic<int> poll_result{-2};
    auto waiter = scheduler.spawn([&] {
        pollfd descriptor{pipe_fds[0], POLLIN, 0};
        entered.store(true, std::memory_order_release);
        poll_result.store(::poll(&descriptor, 1, 40),
                          std::memory_order_release);
    });
    GO2CPP_CHECK(wait_until(entered));
    auto peer = scheduler.spawn([&] {
        peer_done.store(true, std::memory_order_release);
    });
    GO2CPP_CHECK(wait_until(peer_done));
    GO2CPP_CHECK(peer->wait_for(2s));
    GO2CPP_CHECK(wait_until_predicate([&] {
        for (const auto& machine : scheduler.machines()) {
            if (machine.processor_detached) {
                return true;
            }
        }
        return false;
    }));
    GO2CPP_CHECK(waiter->wait_for(2s));
    GO2CPP_CHECK(poll_result.load(std::memory_order_acquire) == 0);
    (void)::close(pipe_fds[0]);
    (void)::close(pipe_fds[1]);
    scheduler.shutdown();
}

void test_managed_write_error_is_reported() {
    go2cpp::IOManager manager(one_worker_config());
    GO2CPP_CHECK(manager.Start());

    int fds[2]{-1, -1};
    GO2CPP_CHECK(make_pair(fds));
    GO2CPP_CHECK(::close(fds[1]) == 0);
    fds[1] = -1;

    std::atomic<ssize_t> result{0};
    std::atomic<int> system_error{0};
    auto task = manager.Go([&] {
        const char value = 'x';
        errno = 0;
        const ssize_t written =
            ::send(fds[0], &value, 1, MSG_NOSIGNAL);
        result.store(written, std::memory_order_release);
        system_error.store(written < 0 ? errno : 0,
                           std::memory_order_release);
    });
    GO2CPP_CHECK(task->wait_for(2s));
    GO2CPP_CHECK(result.load(std::memory_order_acquire) < 0);
    const int error = system_error.load(std::memory_order_acquire);
    GO2CPP_CHECK(error == EPIPE || error == ECONNRESET || error == EBADF);
    GO2CPP_CHECK(task->state() == go2cpp::GState::kDead);

    close_pair(fds);
    manager.Shutdown();
}

void test_nested_fiber_io_timeout_and_wake() {
    go2cpp::SchedulerConfig config;
    config.processor_count = 1;
    config.min_workers = 1;
    config.max_workers = 2;
    config.sysmon_interval = 2ms;
    config.long_syscall_threshold = 10ms;
    go2cpp::IOManager manager(config);
    GO2CPP_CHECK(manager.Start());

    int timeout_fds[2]{-1, -1};
    int wake_fds[2]{-1, -1};
    GO2CPP_CHECK(make_pair(timeout_fds));
    GO2CPP_CHECK(make_pair(wake_fds));
    const timeval timeout{0, 200'000};
    GO2CPP_CHECK(::setsockopt(timeout_fds[0], SOL_SOCKET, SO_RCVTIMEO,
                               &timeout, sizeof(timeout)) == 0);

    std::mutex child_mutex;
    std::shared_ptr<go2cpp::Fiber> observed_child;
    std::atomic<int> phase{0};
    std::atomic<int> timeout_errno{0};
    std::atomic<int> wake_errno{0};
    std::atomic<char> received{0};

    auto task = manager.Go([&] {
        auto timeout_child = std::make_shared<go2cpp::Fiber>([&] {
            phase.store(1, std::memory_order_release);
            char byte = 0;
            const ssize_t result = ::recv(timeout_fds[0], &byte, 1, 0);
            timeout_errno.store(result < 0 ? errno : 0,
                                std::memory_order_release);
        });
        {
            std::lock_guard<std::mutex> lock(child_mutex);
            observed_child = timeout_child;
        }
        GO2CPP_CHECK(timeout_child->resume());
        GO2CPP_CHECK(timeout_errno.load(std::memory_order_acquire) ==
                     EAGAIN);
        phase.store(2, std::memory_order_release);

        auto wake_child = std::make_shared<go2cpp::Fiber>([&] {
            phase.store(3, std::memory_order_release);
            char byte = 0;
            const ssize_t result = ::recv(wake_fds[0], &byte, 1, 0);
            wake_errno.store(result < 0 ? errno : 0,
                             std::memory_order_release);
            if (result == 1) {
                received.store(byte, std::memory_order_release);
            }
        });
        {
            std::lock_guard<std::mutex> lock(child_mutex);
            observed_child = wake_child;
        }
        GO2CPP_CHECK(wake_child->resume());
        phase.store(4, std::memory_order_release);
    });

    GO2CPP_REQUIRE_EVENTUALLY(phase.load(std::memory_order_acquire) == 1,
                              2s);
    const auto child_is_io_suspended = [&] {
        std::shared_ptr<go2cpp::Fiber> child;
        {
            std::lock_guard<std::mutex> lock(child_mutex);
            child = observed_child;
        }
        if (!child) {
            return false;
        }
        const auto snapshot = child->context_snapshot();
        return snapshot.size() >= 2 &&
               snapshot.back().state == go2cpp::FiberState::Suspended &&
               snapshot.back().suspend_reason == go2cpp::SuspendReason::Io;
    };
    GO2CPP_REQUIRE_EVENTUALLY(child_is_io_suspended(), 2s);
    // phase=2 是 timeout child 完成后的瞬时状态，随后立即进入
    // 第二个等待 child 的 phase=3；观察线程不能要求恰好采到 2。
    GO2CPP_REQUIRE_EVENTUALLY(phase.load(std::memory_order_acquire) >= 2,
                              3s);
    GO2CPP_REQUIRE_EVENTUALLY(phase.load(std::memory_order_acquire) == 3,
                              2s);
    GO2CPP_REQUIRE_EVENTUALLY(child_is_io_suspended(), 2s);
    {
        const char value = 'n';
        GO2CPP_CHECK(::write(wake_fds[1], &value, 1) == 1);
    }
    GO2CPP_CHECK(task->wait_for(3s));
    GO2CPP_CHECK(phase.load(std::memory_order_acquire) == 4);
    GO2CPP_CHECK(wake_errno.load(std::memory_order_acquire) == 0);
    GO2CPP_CHECK(received.load(std::memory_order_acquire) == 'n');
    close_pair(timeout_fds);
    close_pair(wake_fds);
    manager.Shutdown();
}


void test_deep_nested_fiber_io_chain() {
    go2cpp::SchedulerConfig config;
    config.processor_count = 1;
    config.min_workers = 1;
    config.max_workers = 2;
    config.sysmon_interval = 2ms;
    config.long_syscall_threshold = 10ms;
    go2cpp::IOManager manager(config);
    GO2CPP_CHECK(manager.Start());

    int timeout_fds[2]{-1, -1};
    int wake_fds[2]{-1, -1};
    GO2CPP_CHECK(make_pair(timeout_fds));
    GO2CPP_CHECK(make_pair(wake_fds));
    const timeval timeout{0, 150'000};
    GO2CPP_CHECK(::setsockopt(timeout_fds[0], SOL_SOCKET, SO_RCVTIMEO,
                               &timeout, sizeof(timeout)) == 0);

    std::mutex deepest_mutex;
    std::shared_ptr<go2cpp::Fiber> deepest;
    std::atomic<int> phase{0};
    std::atomic<int> timeout_errno{0};
    std::atomic<int> wake_errno{0};
    std::atomic<char> received{0};

    auto task = manager.Go([&] {
        auto level_one = std::make_shared<go2cpp::Fiber>([&] {
            auto level_two = std::make_shared<go2cpp::Fiber>([&] {
                auto level_three = std::make_shared<go2cpp::Fiber>([&] {
                    phase.store(1, std::memory_order_release);
                    char byte = 0;
                    const ssize_t timeout_result =
                        ::recv(timeout_fds[0], &byte, 1, 0);
                    timeout_errno.store(timeout_result < 0 ? errno : 0,
                                        std::memory_order_release);
                    phase.store(2, std::memory_order_release);

                    phase.store(3, std::memory_order_release);
                    const ssize_t wake_result =
                        ::recv(wake_fds[0], &byte, 1, 0);
                    wake_errno.store(wake_result < 0 ? errno : 0,
                                     std::memory_order_release);
                    if (wake_result == 1) {
                        received.store(byte, std::memory_order_release);
                    }
                    phase.store(4, std::memory_order_release);
                });
                {
                    std::lock_guard<std::mutex> lock(deepest_mutex);
                    deepest = level_three;
                }
                GO2CPP_CHECK(level_three->resume());
                GO2CPP_CHECK(level_three->state() ==
                             go2cpp::FiberState::Completed);
                phase.store(5, std::memory_order_release);
            });
            GO2CPP_CHECK(level_two->resume());
            phase.store(6, std::memory_order_release);
        });
        GO2CPP_CHECK(level_one->resume());
        phase.store(7, std::memory_order_release);
    });

    GO2CPP_REQUIRE_EVENTUALLY(phase.load(std::memory_order_acquire) == 1,
                              2s);
    const auto deepest_is_io_suspended = [&] {
        std::shared_ptr<go2cpp::Fiber> fiber;
        {
            std::lock_guard<std::mutex> lock(deepest_mutex);
            fiber = deepest;
        }
        if (!fiber) {
            return false;
        }
        const auto snapshot = fiber->context_snapshot();
        return snapshot.size() >= 5 && snapshot.back().depth >= 4 &&
               snapshot.back().state == go2cpp::FiberState::Suspended &&
               snapshot.back().suspend_reason == go2cpp::SuspendReason::Io;
    };
    GO2CPP_REQUIRE_EVENTUALLY(deepest_is_io_suspended(), 2s);
    GO2CPP_REQUIRE_EVENTUALLY(phase.load(std::memory_order_acquire) >= 2,
                              3s);
    GO2CPP_REQUIRE_EVENTUALLY(phase.load(std::memory_order_acquire) == 3,
                              2s);
    GO2CPP_REQUIRE_EVENTUALLY(deepest_is_io_suspended(), 2s);

    const char value = 'd';
    GO2CPP_CHECK(::write(wake_fds[1], &value, 1) == 1);
    GO2CPP_CHECK(task->wait_for(3s));
    GO2CPP_CHECK(phase.load(std::memory_order_acquire) == 7);
    // SO_RCVTIMEO 到期按 POSIX 返回 EAGAIN（非阻塞语义），不是 ETIMEDOUT。
    GO2CPP_CHECK(timeout_errno.load(std::memory_order_acquire) == EAGAIN);
    GO2CPP_CHECK(wake_errno.load(std::memory_order_acquire) == 0);
    GO2CPP_CHECK(received.load(std::memory_order_acquire) == value);
    close_pair(timeout_fds);
    close_pair(wake_fds);
    manager.Shutdown();
}

/**
 * @brief 验证 FIFO/pipe 上的 read 会 park Fiber，而不是阻塞 worker。
 *
 * @details 判定指标是单 worker 上每 10ms 一次的心跳 Fiber：
 *   - FIFO park：worker 立即空出，400ms 观察窗应接近 40 次心跳；
 *   - BlockingRegion + sysmon 替代 M：前段真阻塞，实测只有 14~18 次
 *     （差值是「检测 → 申请替代 M → 迁移」的延迟）。
 *   先断言探针 before >= 3，证明心跳机制本身在跑，否则 delta 没有意义。
 *
 *   观察窗内同时断言 read 仍未返回：确保窗口测到的是「等待中的 park」，
 *   而不是读已经完成后的空转。
 */
void test_fifo_read_parks() {
    go2cpp::IOManager manager(one_worker_config());
    GO2CPP_CHECK(manager.Start());

    int pipe_fds[2]{-1, -1};
    GO2CPP_CHECK(::pipe(pipe_fds) == 0);

    std::atomic<int> heartbeats{0};
    std::atomic<bool> stop{false};
    manager.Go([&] {
        while (!stop.load(std::memory_order_acquire)) {
            heartbeats.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::sleep_for(10ms);
        }
    });
    std::this_thread::sleep_for(60ms);

    // 基线：先测 200ms 的心跳速率。断言用「同进程内的相对速率」而不是写死
    // 的绝对次数 —— 绝对阈值在并发 ctest / 慢机器上会把调度抖动误判成
    // 「没有 park」，那正是本测试要避免的时序假设。
    const int baseline_begin = heartbeats.load(std::memory_order_relaxed);
    std::this_thread::sleep_for(200ms);
    const int baseline =
        heartbeats.load(std::memory_order_relaxed) - baseline_begin;
    // 探针：200ms / 10ms ≈ 20 跳；留 2.5 倍余量，证明心跳机制真的在跑。
    GO2CPP_CHECK(baseline >= 8);

    std::atomic<bool> entered{false};
    std::atomic<ssize_t> read_result{-2};
    std::atomic<char> received{0};
    auto reader = manager.Go([&] {
        entered.store(true, std::memory_order_release);
        char value = 0;
        const ssize_t result = ::read(pipe_fds[0], &value, 1);
        received.store(value, std::memory_order_release);
        read_result.store(result, std::memory_order_release);
    });
    GO2CPP_CHECK(wait_until(entered));

    // 观察窗是基线的两倍。park：worker 空闲，速率不变 → 约 2x 基线；
    // 退化为 BlockingRegion + sysmon 替代 M：worker 真阻塞一段，
    // 实测只有 0.4~0.9x 基线（差值是「检测 → 申请 M → 迁移」的延迟）。
    std::this_thread::sleep_for(400ms);
    const int delta = heartbeats.load(std::memory_order_relaxed) -
                      baseline_begin - baseline;
    GO2CPP_CHECK(read_result.load(std::memory_order_acquire) == -2);
    GO2CPP_CHECK(delta * 2 >= baseline * 3);
    std::printf(
        "[hook] FIFO read(pipe): 基线 200ms=%d，观察 400ms=%d（park 预期 2x）\n",
        baseline, delta);

    stop.store(true, std::memory_order_release);
    const char value = 'p';
    GO2CPP_CHECK(::write(pipe_fds[1], &value, 1) == 1);
    GO2CPP_CHECK(reader->wait_for(2s));
    GO2CPP_CHECK(read_result.load(std::memory_order_acquire) == 1);
    GO2CPP_CHECK(received.load(std::memory_order_acquire) == value);
    (void)::close(pipe_fds[0]);
    (void)::close(pipe_fds[1]);
    manager.Shutdown();
}

/**
 * @brief 运行 socket Hook、超时、dup 和阻塞转移测试。
 * @return 无；测试失败由统一断言统计。
 */
void run_hook_tests() {
    go2cpp_tests::announce("transparent Linux socket hook interposer");
    go2cpp::hook::ScopedEnable enable;
    GO2CPP_CHECK(go2cpp::hook::IsEnabled());
    {
        go2cpp::hook::ScopedThreadHookMode disabled(
            go2cpp::ThreadHookMode::kDisabled);
        GO2CPP_CHECK(!go2cpp::hook::IsEnabled());
    }
    GO2CPP_CHECK(go2cpp::hook::IsEnabled());
    {
        go2cpp::hook::ScopedThreadHookMode enabled(
            go2cpp::ThreadHookMode::kEnabled);
        GO2CPP_CHECK(go2cpp::hook::IsEnabled());
    }
    test_transparent_read_and_sleep();
    test_timeout_and_user_nonblocking();
    test_close_and_dup_metadata();
    test_native_fallback_and_failed_dup();
    test_ioctl_and_urgent_data_boundaries();
    test_dup2_replacement_and_identity();
    test_plain_scheduler_lazy_adoption_timeout();
    test_native_fallback_grows_replacement_m();
    test_iomanager_untracked_fd_publishes_blocking();
    test_fifo_read_parks();
    test_managed_poll_fallback_publishes_blocking();
    test_managed_write_error_is_reported();
    test_nested_fiber_io_timeout_and_wake();
    test_deep_nested_fiber_io_chain();
}
