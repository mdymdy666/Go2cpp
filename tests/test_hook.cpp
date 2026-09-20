#include "go2cpp/hook.hpp"
#include "test_support.hpp"

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <thread>

#if defined(__has_include)
#if __has_include(<valgrind/valgrind.h>)
#include <valgrind/valgrind.h>
#define GO2CPP_TEST_HAS_VALGRIND 1
#endif
#endif

namespace {

using namespace std::chrono_literals;

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

bool make_pair(int (&fds)[2]) {
    return ::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) == 0;
}

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
    GO2CPP_CHECK(timeout_errno.load(std::memory_order_acquire) == ETIMEDOUT);
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
    // ETIMEDOUT rather than leaking the internal EAGAIN probe result.
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
    timeout_reader.join();
    GO2CPP_CHECK(native_timeout_errno.load(std::memory_order_acquire) ==
                 ETIMEDOUT);

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
    ordinary_reader.join();
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

void run_hook_tests() {
    go2cpp_tests::announce("transparent Linux socket hook interposer");
    go2cpp::hook::ScopedEnable enable;
    test_transparent_read_and_sleep();
    test_timeout_and_user_nonblocking();
    test_close_and_dup_metadata();
    test_native_fallback_and_failed_dup();
    test_ioctl_and_urgent_data_boundaries();
    test_dup2_replacement_and_identity();
}
