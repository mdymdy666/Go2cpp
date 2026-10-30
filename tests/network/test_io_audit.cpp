/**
 * @file test_io_audit.cpp
 * @brief IOManager 并发审计测试（已注册为 ctest 用例 `go2cpp_io_audit`）。
 *
 * 覆盖审计清单：
 * - EPOLLHUP / EPOLLERR 必须唤醒等待者；
 * - IOManager 停止时所有等待 Fiber 必须返回（不能永久 park）；
 * - 同一 fd 上的多个等待者：cancel_all / notify_close 必须全部唤醒；
 * - fd 代际（close 通知后同一 fd 继续使用）不会误唤醒旧等待者；
 * - fd 注册/注销竞态压力：队头正被 Context 取消路径完成时，
 *   notify_close 必须仍唤醒队列中的其余等待者（当前实现的 drain 在
 *   队头 active 时提前 break，本测试用于暴露该窗口）；
 * - wait_any 在 fd 关闭时返回关闭状态。
 *
 * 也可由 scripts/run_concurrency_audit.sh 独立编译运行；CMake 目标
 * `go2cpp_io_audit` 注册为 ctest 用例（见 cmake/Go2CppTests.cmake）。
 */

#include "go2cpp/io.hpp"

#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using go2cpp::IOEvent;
using go2cpp::IOManager;
using go2cpp::IOWaitStatus;

std::atomic<int> g_failures{0};

void check(bool condition, const char* expression, const char* file, int line) {
    if (condition) {
        return;
    }
    g_failures.fetch_add(1, std::memory_order_relaxed);
    std::fprintf(stderr, "%s:%d: check failed: %s\n", file, line, expression);
}

#define CHECK(expr) check((expr), #expr, __FILE__, __LINE__)

bool make_pair(int (&fds)[2]) {
    return ::socketpair(AF_UNIX,
                        SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0,
                        fds) == 0;
}

void raw_close(int fd) {
    if (fd >= 0) {
        (void)::syscall(SYS_close, fd);
    }
}

bool wait_until(const std::atomic<bool>& flag,
                std::chrono::milliseconds timeout = 3s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!flag.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(100us);
    }
    return flag.load(std::memory_order_acquire);
}

bool wait_for_task_state(const std::shared_ptr<go2cpp::Task>& task,
                         go2cpp::GState expected,
                         std::chrono::milliseconds timeout = 3s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (task && task->state() != expected &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(100us);
    }
    return task && task->state() == expected;
}

go2cpp::SchedulerConfig many_worker_config() {
    go2cpp::SchedulerConfig config;
    config.processor_count = 4;
    config.max_workers = 4;
    config.idle_wait = 1ms;
    return config;
}

/**
 * @brief EPOLLHUP：对端关闭必须让 Wait 返回 ready，而不是睡到超时。
 */
void TestHangupWakesWaiter() {
    IOManager manager(many_worker_config());
    CHECK(manager.Start());
    int fds[2]{-1, -1};
    CHECK(make_pair(fds));
    std::atomic<bool> entered{false};
    std::atomic<IOWaitStatus> status{IOWaitStatus::kError};
    auto task = manager.Go([&] {
        entered.store(true, std::memory_order_release);
        status.store(manager.WaitFor(fds[0], IOEvent::kRead, 1s).status,
                     std::memory_order_release);
    });
    CHECK(wait_until(entered));
    std::this_thread::sleep_for(5ms);
    raw_close(fds[1]);  // 直接关闭对端，不经过 NotifyClose，产生 EPOLLHUP。
    fds[1] = -1;
    CHECK(task->wait_for(3s));
    const auto observed = status.load(std::memory_order_acquire);
    CHECK(observed == IOWaitStatus::kReady);
    raw_close(fds[0]);
    manager.Shutdown();
    std::printf("[io-audit] EPOLLHUP: status=%s\n",
                observed == IOWaitStatus::kReady ? "ready" : "unexpected");
}

/**
 * @brief shutdown 必须唤醒全部无限等待的 Fiber。
 */
void TestShutdownWakesAllWaiters() {
    IOManager manager(many_worker_config());
    CHECK(manager.Start());
    constexpr int kWaiters = 16;
    std::vector<std::pair<int, int>> pairs;
    pairs.reserve(kWaiters);
    std::atomic<int> parked{0};
    std::atomic<int> returned{0};
    std::atomic<int> wrong{0};
    std::vector<std::shared_ptr<go2cpp::Task>> tasks;
    for (int i = 0; i < kWaiters; ++i) {
        int fds[2]{-1, -1};
        CHECK(make_pair(fds));
        pairs.emplace_back(fds[0], fds[1]);
        const int read_fd = fds[0];
        tasks.push_back(manager.Go([read_fd, &parked, &returned, &wrong, &manager] {
            parked.fetch_add(1, std::memory_order_release);
            const auto result = manager.Wait(read_fd, IOEvent::kRead);
            if (result.status != IOWaitStatus::kCancelled) {
                wrong.fetch_add(1, std::memory_order_relaxed);
            }
            returned.fetch_add(1, std::memory_order_release);
        }));
    }
    for (const auto& task : tasks) {
        CHECK(wait_for_task_state(task, go2cpp::GState::kWaiting));
    }
    CHECK(parked.load() == kWaiters);
    manager.Shutdown();
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (returned.load(std::memory_order_acquire) != kWaiters &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    CHECK(returned.load() == kWaiters);
    CHECK(wrong.load() == 0);
    for (const auto& pair : pairs) {
        raw_close(pair.first);
        raw_close(pair.second);
    }
    std::printf("[io-audit] shutdown 唤醒: returned=%d wrong=%d\n",
                returned.load(), wrong.load());
}

/**
 * @brief 同一 fd 上 3 个等待者，cancel_all 必须全部唤醒（不能只处理队头）。
 */
void TestCancelAllDrainsEveryWaiter() {
    IOManager manager(many_worker_config());
    CHECK(manager.Start());
    int fds[2]{-1, -1};
    CHECK(make_pair(fds));
    constexpr int kWaiters = 3;
    std::atomic<int> returned{0};
    std::atomic<int> wrong{0};
    std::vector<std::shared_ptr<go2cpp::Task>> tasks;
    for (int i = 0; i < kWaiters; ++i) {
        tasks.push_back(manager.Go([&manager, &returned, &wrong, fd = fds[0]] {
            const auto result = manager.Wait(fd, IOEvent::kRead);
            if (result.status != IOWaitStatus::kCancelled) {
                wrong.fetch_add(1, std::memory_order_relaxed);
            }
            returned.fetch_add(1, std::memory_order_release);
        }));
    }
    for (const auto& task : tasks) {
        CHECK(wait_for_task_state(task, go2cpp::GState::kWaiting));
    }
    CHECK(manager.CancelAll(fds[0]));
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (returned.load(std::memory_order_acquire) != kWaiters &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    CHECK(returned.load() == kWaiters);
    CHECK(wrong.load() == 0);
    (void)manager.NotifyClose(fds[0]);
    raw_close(fds[0]);
    raw_close(fds[1]);
    manager.Shutdown();
    std::printf("[io-audit] cancel_all 排空: returned=%d wrong=%d\n",
                returned.load(), wrong.load());
}

/**
 * @brief 同一 fd 上 2 个等待者，notify_close 必须全部收到 kClosed。
 */
void TestNotifyCloseDrainsEveryWaiter() {
    IOManager manager(many_worker_config());
    CHECK(manager.Start());
    int fds[2]{-1, -1};
    CHECK(make_pair(fds));
    constexpr int kWaiters = 2;
    std::atomic<int> returned{0};
    std::atomic<int> closed{0};
    std::vector<std::shared_ptr<go2cpp::Task>> tasks;
    for (int i = 0; i < kWaiters; ++i) {
        tasks.push_back(manager.Go([&manager, &returned, &closed, fd = fds[0]] {
            const auto result = manager.Wait(fd, IOEvent::kRead);
            if (result.status == IOWaitStatus::kClosed) {
                closed.fetch_add(1, std::memory_order_relaxed);
            }
            returned.fetch_add(1, std::memory_order_release);
        }));
    }
    for (const auto& task : tasks) {
        CHECK(wait_for_task_state(task, go2cpp::GState::kWaiting));
    }
    CHECK(manager.NotifyClose(fds[0]));
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (returned.load(std::memory_order_acquire) != kWaiters &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    CHECK(returned.load() == kWaiters);
    CHECK(closed.load() == kWaiters);
    raw_close(fds[0]);
    raw_close(fds[1]);
    manager.Shutdown();
    std::printf("[io-audit] notify_close 排空: returned=%d closed=%d\n",
                returned.load(), closed.load());
}

/**
 * @brief close 通知后同一数字 fd 仍可用：新等待使用新代际，旧状态不残留。
 */
void TestGenerationReuseAfterNotifyClose() {
    IOManager manager(many_worker_config());
    CHECK(manager.Start());
    int fds[2]{-1, -1};
    CHECK(make_pair(fds));

    std::atomic<bool> first_entered{false};
    std::atomic<IOWaitStatus> first_status{IOWaitStatus::kError};
    auto first = manager.Go([&] {
        first_entered.store(true, std::memory_order_release);
        first_status.store(manager.Wait(fds[0], IOEvent::kRead).status,
                           std::memory_order_release);
    });
    CHECK(wait_until(first_entered));
    CHECK(wait_for_task_state(first, go2cpp::GState::kWaiting));
    CHECK(manager.NotifyClose(fds[0]));
    CHECK(first->wait_for(3s));
    CHECK(first_status.load(std::memory_order_acquire) == IOWaitStatus::kClosed);

    // fd 没有真正关闭，继续可用；新等待必须先超时，再在写入后 ready。
    std::atomic<bool> second_done{false};
    std::atomic<int> phase{0};
    std::atomic<IOWaitStatus> timeout_status{IOWaitStatus::kError};
    std::atomic<IOWaitStatus> ready_status{IOWaitStatus::kError};
    auto second = manager.Go([&] {
        timeout_status.store(
            manager.WaitFor(fds[0], IOEvent::kRead, 30ms).status,
            std::memory_order_release);
        phase.store(1, std::memory_order_release);
        ready_status.store(manager.WaitFor(fds[0], IOEvent::kRead, 1s).status,
                           std::memory_order_release);
        second_done.store(true, std::memory_order_release);
    });
    while (phase.load(std::memory_order_acquire) != 1) {
        std::this_thread::sleep_for(100us);
    }
    CHECK(::syscall(SYS_write, fds[1], "g", 1) == 1);
    CHECK(wait_until(second_done, 3s));
    CHECK(second->wait_for(3s));
    CHECK(timeout_status.load(std::memory_order_acquire) ==
          IOWaitStatus::kTimeout);
    CHECK(ready_status.load(std::memory_order_acquire) == IOWaitStatus::kReady);
    raw_close(fds[0]);
    raw_close(fds[1]);
    manager.Shutdown();
    std::printf(
        "[io-audit] 代际复用: close 通知后同 fd 可重新等待"
        "（timeout=%s ready=%s）\n",
        timeout_status.load() == IOWaitStatus::kTimeout ? "yes" : "no",
        ready_status.load() == IOWaitStatus::kReady ? "yes" : "no");
}

/**
 * @brief wait_any 在某个 fd 被关闭时返回 kClosed。
 */
void TestWaitAnyClose() {
    IOManager manager(many_worker_config());
    CHECK(manager.Start());
    int first[2]{-1, -1};
    int second[2]{-1, -1};
    CHECK(make_pair(first));
    CHECK(make_pair(second));
    std::atomic<bool> done{false};
    std::atomic<IOWaitStatus> status{IOWaitStatus::kError};
    auto task = manager.Go([&] {
        const auto result = manager.WaitAnyFor(
            std::vector<go2cpp::IOWaitRequest>{
                {first[0], IOEvent::kRead, {}},
                {second[0], IOEvent::kRead, {}}},
            3s);
        status.store(result.status, std::memory_order_release);
        done.store(true, std::memory_order_release);
    });
    CHECK(wait_for_task_state(task, go2cpp::GState::kWaiting));
    CHECK(manager.NotifyClose(first[0]));
    CHECK(wait_until(done));
    CHECK(task->wait_for(3s));
    CHECK(status.load(std::memory_order_acquire) == IOWaitStatus::kClosed);
    raw_close(first[0]);
    raw_close(first[1]);
    raw_close(second[0]);
    raw_close(second[1]);
    manager.Shutdown();
    std::printf("[io-audit] wait_any close: status=%s\n",
                status.load() == IOWaitStatus::kClosed ? "closed" : "unexpected");
}

/**
 * @brief 队头 active 时的 close 竞态压力：每个 fd 挂两个等待者，A 带
 *        Context、B 只有 500ms 兜底 deadline；同时取消 A 并 NotifyClose。
 *
 * 不变量：B 必须收到 kClosed，而不是被 close 漏掉后等到 kTimeout。
 * 当前实现里 drain_queue_locked 在队头 begin_operation 失败时 break，
 * 而 notify_close 随后无条件删除 FdSlot，队列中 B 的节点会失去唤醒源；
 * 本测试用高频并发放大该窗口。
 */
void TestCloseWhileHeadCancelRace() {
    IOManager manager(many_worker_config());
    CHECK(manager.Start());
    constexpr int kPairs = 8;
    constexpr int kRounds = 200;

    struct Pair {
        int read_fd{-1};
        int write_fd{-1};
        go2cpp::ContextPtr context;
        std::function<void()> cancel;
    };

    int lost_wakeups = 0;
    int rounds_with_lost = 0;
    for (int round = 0; round < kRounds; ++round) {
        std::vector<Pair> pairs(static_cast<std::size_t>(kPairs));
        std::vector<std::shared_ptr<go2cpp::Task>> tasks;
        tasks.reserve(static_cast<std::size_t>(kPairs * 2));
        std::vector<std::atomic<go2cpp::io::WaitStatus>> status_a(
            static_cast<std::size_t>(kPairs));
        std::vector<std::atomic<go2cpp::io::WaitStatus>> status_b(
            static_cast<std::size_t>(kPairs));
        for (int i = 0; i < kPairs; ++i) {
            int fds[2]{-1, -1};
            CHECK(make_pair(fds));
            pairs[static_cast<std::size_t>(i)].read_fd = fds[0];
            pairs[static_cast<std::size_t>(i)].write_fd = fds[1];
            auto context = go2cpp::WithCancel(go2cpp::Background());
            pairs[static_cast<std::size_t>(i)].context = context.first;
            pairs[static_cast<std::size_t>(i)].cancel = context.second;
            status_a[static_cast<std::size_t>(i)].store(
                go2cpp::io::WaitStatus::kError, std::memory_order_relaxed);
            status_b[static_cast<std::size_t>(i)].store(
                go2cpp::io::WaitStatus::kError, std::memory_order_relaxed);
        }
        for (int i = 0; i < kPairs; ++i) {
            const int index = i;
            const int read_fd = pairs[static_cast<std::size_t>(i)].read_fd;
            auto context = pairs[static_cast<std::size_t>(i)].context;
            tasks.push_back(manager.Go([&, index, read_fd, context] {
                const auto result = manager.Wait(
                    read_fd, IOEvent::kRead, std::nullopt, context);
                status_a[static_cast<std::size_t>(index)].store(
                    result.status, std::memory_order_release);
            }));
            tasks.push_back(manager.Go([&, index, read_fd] {
                const auto result = manager.WaitFor(read_fd, IOEvent::kRead,
                                                    100ms);
                status_b[static_cast<std::size_t>(index)].store(
                    result.status, std::memory_order_release);
            }));
        }
        for (const auto& task : tasks) {
            CHECK(wait_for_task_state(task, go2cpp::GState::kWaiting));
        }

        std::atomic<bool> go{false};
        std::thread canceller([&pairs, &go] {
            while (!go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            if ((std::rand() & 1) == 0) {
                std::this_thread::yield();
            }
            for (auto& pair : pairs) {
                pair.cancel();
            }
        });
        std::thread closer([&pairs, &go, &manager] {
            while (!go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (const auto& pair : pairs) {
                (void)manager.NotifyClose(pair.read_fd);
            }
        });
        go.store(true, std::memory_order_release);
        canceller.join();
        closer.join();

        for (const auto& task : tasks) {
            CHECK(task->wait_for(5s));
        }
        int round_lost = 0;
        for (int i = 0; i < kPairs; ++i) {
            const auto observed = status_b[static_cast<std::size_t>(i)].load(
                std::memory_order_acquire);
            if (observed != go2cpp::io::WaitStatus::kClosed) {
                ++round_lost;
                ++lost_wakeups;
            }
        }
        if (round_lost != 0) {
            ++rounds_with_lost;
            std::fprintf(stderr,
                         "[io-audit] round=%d: %d/%d 个第二等待者未被 "
                         "notify_close 唤醒（kClosed 丢失）\n",
                         round, round_lost, kPairs);
        }
        for (const auto& pair : pairs) {
            raw_close(pair.read_fd);
            raw_close(pair.write_fd);
        }
    }
    manager.Shutdown();
    CHECK(lost_wakeups == 0);
    std::printf(
        "[io-audit] close 竞态压力: rounds=%d pairs=%d lost_wakeups=%d "
        "rounds_with_lost=%d\n",
        kRounds, kPairs, lost_wakeups, rounds_with_lost);
}

}  // namespace

int main() {
    std::printf("=== Go2Cpp IOManager 并发审计测试 ===\n");
    TestHangupWakesWaiter();
    TestShutdownWakesAllWaiters();
    TestCancelAllDrainsEveryWaiter();
    TestNotifyCloseDrainsEveryWaiter();
    TestGenerationReuseAfterNotifyClose();
    TestWaitAnyClose();
    TestCloseWhileHeadCancelRace();
    if (g_failures.load() != 0) {
        std::fprintf(stderr, "[io-audit] failures=%d\n", g_failures.load());
        return 1;
    }
    std::printf("[io-audit] all checks passed\n");
    return 0;
}
