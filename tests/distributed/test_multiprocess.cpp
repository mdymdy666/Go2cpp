/**
 * @file test_multiprocess.cpp
 * @brief 多进程：3 节点选主（SIGKILL 主进程）+ 4 进程抢锁（共享内存校验互斥）。
 *
 * 为什么用 `fork + execv(/proc/self/exe)` 而不是 fork 后直接跑线程代码：
 * 父进程已经启动了 accept/sweeper 等后台线程，fork 出来的子进程只复制了
 * 调用线程，其他线程持有的 mutex 可能永远处于锁定状态。exec 成新进程是
 * 唯一干净的隔离方式。
 *
 * 子进程通过两种方式与父进程协作：
 * - 选主：父进程持管道写端，子进程循环非阻塞读；关闭写端 = 通知退出。
 * - 抢锁：共享内存文件（mmap MAP_SHARED）记录临界区人数与错误计数，父进程
 *   在全部子进程退出后统一断言。
 */

#include "go2cpp/distributed/coordinator.hpp"
#include "go2cpp/distributed/election.hpp"
#include "go2cpp/distributed/lock.hpp"
#include "go2cpp/distributed/transport.hpp"

#include "distributed_test_support.hpp"

#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace go2cpp::distributed;
using namespace go2cpp::distributed::test;

/// 4 个进程共享的临界区状态。必须全部 lock-free（x86_64 上 int/long/u64
/// 都是），否则跨进程的 atomic 语义不成立。
struct SharedState {
    std::atomic<int> inside{0};
    std::atomic<int> max_inside{0};
    std::atomic<int> errors{0};
    std::atomic<long> acquisitions{0};
    std::atomic<std::uint64_t> max_fencing{0};
};

static_assert(std::atomic<int>::is_always_lock_free, "shared state must be lock-free");
static_assert(std::atomic<long>::is_always_lock_free, "shared state must be lock-free");
static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
              "shared state must be lock-free");

constexpr int kElectionChildren = 3;
constexpr int kLockChildren = 4;
constexpr int kLockIterations = 200;

int RunElectionChild(int port, int index, int pipe_fd) {
    const int flags = fcntl(pipe_fd, F_GETFL, 0);
    if (flags < 0 || fcntl(pipe_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return 2;
    }
    CoordinatorClient client("127.0.0.1", static_cast<std::uint16_t>(port));
    LeaderElection election(client);
    auto joined = election.Join("main", "node-" + std::to_string(index), 200ms);
    if (!joined.ok()) {
        std::fprintf(stderr, "[child %d] join failed: %s\n", index,
                     StatusName(joined.status));
        return 3;
    }
    char byte = 0;
    for (;;) {
        const ssize_t got = ::read(pipe_fd, &byte, 1);
        if (got == 0) {
            break;  // 父进程关闭写端：测试结束。
        }
        if (got < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            break;
        }
        if (!joined.value.Refresh().ok()) {
            return 4;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return 0;
}

int RunLockChild(int port, int index, const char *shared_path) {
    const int fd = ::open(shared_path, O_RDWR);
    if (fd < 0) {
        return 2;
    }
    void *memory = ::mmap(nullptr, sizeof(SharedState), PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                         0);
    ::close(fd);
    if (memory == MAP_FAILED) {
        return 2;
    }
    auto *shared = static_cast<SharedState *>(memory);

    CoordinatorClient client("127.0.0.1", static_cast<std::uint16_t>(port));
    DistributedLock locks(client);
    LockOptions manual;
    manual.auto_renew = false;
    std::uint64_t last_token = 0;
    int completed = 0;
    while (completed < kLockIterations) {
        auto acquired =
            locks.Acquire("multi", "proc-" + std::to_string(index), 1000ms, manual);
        if (!acquired.ok()) {
            if (acquired.status == Status::kConflict) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            shared->errors.fetch_add(1, std::memory_order_relaxed);
            return 3;
        }
        LockHandle handle = std::move(acquired.value);
        // 同一持有者的连续授予必须严格递增（fencing 的最小可验证性质）。
        if (handle.fencing_token() <= last_token) {
            shared->errors.fetch_add(1, std::memory_order_relaxed);
        }
        last_token = handle.fencing_token();
        const std::uint64_t previous =
            shared->max_fencing.load(std::memory_order_relaxed);
        if (handle.fencing_token() > previous) {
            shared->max_fencing.store(handle.fencing_token(), std::memory_order_relaxed);
        }
        const int inside = shared->inside.fetch_add(1, std::memory_order_acq_rel) + 1;
        int observed = shared->max_inside.load(std::memory_order_relaxed);
        while (inside > observed &&
               !shared->max_inside.compare_exchange_weak(observed, inside)) {
        }
        if (inside != 1 || !handle.valid()) {
            shared->errors.fetch_add(1, std::memory_order_relaxed);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        shared->inside.fetch_sub(1, std::memory_order_acq_rel);
        handle.Release();
        shared->acquisitions.fetch_add(1, std::memory_order_relaxed);
        ++completed;
    }
    ::munmap(memory, sizeof(SharedState));
    return 0;
}

pid_t SpawnChild(const std::vector<std::string> &arguments, int *pipe_write_fd) {
    int fds[2] = {-1, -1};
    if (pipe_write_fd != nullptr && ::pipe(fds) != 0) {
        return -1;
    }
    const pid_t pid = ::fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        if (pipe_write_fd != nullptr) {
            ::close(fds[1]);
        }
        std::string child_pipe = pipe_write_fd != nullptr ? std::to_string(fds[0]) : "0";
        std::vector<std::string> args = arguments;
        if (pipe_write_fd != nullptr) {
            args.push_back(child_pipe);
        }
        std::vector<char *> argv;
        argv.reserve(args.size() + 2);
        argv.push_back(const_cast<char *>("/proc/self/exe"));
        for (std::string &argument : args) {
            argv.push_back(const_cast<char *>(argument.c_str()));
        }
        argv.push_back(nullptr);
        ::execv("/proc/self/exe", argv.data());
        _exit(127);
    }
    if (pipe_write_fd != nullptr) {
        ::close(fds[0]);
        *pipe_write_fd = fds[1];
    }
    return pid;
}

void CloseAndReap(const std::vector<pid_t> &pids, const std::vector<int> &write_fds,
                  bool expect_killed_index_minus_one, int killed_index) {
    for (const int fd : write_fds) {
        if (fd >= 0) {
            ::close(fd);
        }
    }
    for (std::size_t index = 0; index < pids.size(); ++index) {
        int status = 0;
        ::waitpid(pids[index], &status, 0);
        const bool killed = static_cast<int>(index) + 1 == killed_index;
        if (killed) {
            GO2CPP_CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
            continue;
        }
        (void)expect_killed_index_minus_one;
        GO2CPP_CHECK(WIFEXITED(status));
        if (WIFEXITED(status)) {
            GO2CPP_CHECK(WEXITSTATUS(status) == 0);
        }
    }
}

void ScenarioMultiProcessElection() {
    go2cpp_tests::announce("multiprocess.election_failover");
    const auto started = std::chrono::steady_clock::now();
    auto stamp = [&started](const char *phase) {
        std::printf("[mp] %-18s %8.1f ms\n", phase,
                    std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - started)
                        .count());
    };
    Coordinator coordinator(MakeOptions());
    CoordinatorServer server(coordinator);
    GO2CPP_REQUIRE(server.Start().ok());
    const std::uint16_t port = server.port();
    stamp("server started");

    std::vector<pid_t> pids;
    std::vector<int> write_fds;
    for (int index = 1; index <= kElectionChildren; ++index) {
        int write_fd = -1;
        const pid_t pid = SpawnChild(
            {"--child-election", std::to_string(port), std::to_string(index)}, &write_fd);
        GO2CPP_REQUIRE(pid > 0);
        pids.push_back(pid);
        write_fds.push_back(write_fd);
    }
    stamp("children spawned");

    LeaderElection election(coordinator);
    GO2CPP_CHECK(WaitUntil(
        [&] {
            const auto leader = election.Leader("main");
            return leader.has_value() && !leader->node.empty();
        },
        10s));
    stamp("leader elected");
    const auto initial = election.Leader("main");
    GO2CPP_REQUIRE(initial.has_value());
    const std::string old_leader = initial->node;
    const int old_index = std::stoi(old_leader.substr(old_leader.find('-') + 1));
    GO2CPP_CHECK(old_index >= 1 && old_index <= kElectionChildren);

    // 真实崩溃：SIGKILL 主进程（不走 Leave，连接与心跳同时消失）。
    GO2CPP_REQUIRE(::kill(pids[static_cast<std::size_t>(old_index - 1)], SIGKILL) == 0);
    GO2CPP_CHECK(WaitUntil(
        [&] {
            const auto leader = election.Leader("main");
            return leader.has_value() && leader->node != old_leader;
        },
        10s));
    stamp("leader re-elected");
    const auto next = election.Leader("main");
    GO2CPP_REQUIRE(next.has_value());
    GO2CPP_CHECK(next->node != old_leader);
    GO2CPP_CHECK(next->epoch > initial->epoch);

    CloseAndReap(pids, write_fds, true, old_index);
    stamp("children reaped");
    server.Stop();
    stamp("server stopped");
}

void ScenarioMultiProcessLock() {
    go2cpp_tests::announce("multiprocess.lock_mutex");
    const auto started = std::chrono::steady_clock::now();
    auto stamp = [&started](const char *phase) {
        std::printf("[mp] %-18s %8.1f ms\n", phase,
                    std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - started)
                        .count());
    };
    Coordinator coordinator(MakeOptions());
    CoordinatorServer server(coordinator);
    GO2CPP_REQUIRE(server.Start().ok());
    const std::uint16_t port = server.port();

    char path[] = "/tmp/go2cpp_distributed_shm_XXXXXX";
    const int fd = ::mkstemp(path);
    GO2CPP_REQUIRE(fd >= 0);
    GO2CPP_REQUIRE(::ftruncate(fd, static_cast<off_t>(sizeof(SharedState))) == 0);
    void *memory = ::mmap(nullptr, sizeof(SharedState), PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                          0);
    GO2CPP_REQUIRE(memory != MAP_FAILED);
    auto *shared = new (memory) SharedState();
    ::close(fd);

    std::vector<pid_t> pids;
    for (int index = 1; index <= kLockChildren; ++index) {
        const pid_t pid = SpawnChild(
            {"--child-lock", std::to_string(port), std::to_string(index), path}, nullptr);
        GO2CPP_REQUIRE(pid > 0);
        pids.push_back(pid);
    }
    stamp("children spawned");
    for (const pid_t pid : pids) {
        int status = 0;
        ::waitpid(pid, &status, 0);
        GO2CPP_CHECK(WIFEXITED(status));
        if (WIFEXITED(status)) {
            GO2CPP_CHECK(WEXITSTATUS(status) == 0);
        }
    }
    stamp("children finished");

    GO2CPP_CHECK(shared->errors.load() == 0);
    GO2CPP_CHECK(shared->max_inside.load() == 1);
    GO2CPP_CHECK(shared->acquisitions.load() ==
                 static_cast<long>(kLockChildren) * kLockIterations);
    GO2CPP_CHECK(shared->max_fencing.load() >=
                 static_cast<std::uint64_t>(kLockChildren) * kLockIterations);
    GO2CPP_CHECK(!coordinator.QueryLock("multi").has_value());
    shared->~SharedState();
    ::munmap(memory, sizeof(SharedState));
    ::unlink(path);
    server.Stop();
    stamp("server stopped");
}

} // namespace

int main(int argc, char **argv) {
    if (argc >= 2 && std::string(argv[1]) == "--child-election") {
        if (argc < 5) {
            return 2;
        }
        return RunElectionChild(std::atoi(argv[2]), std::atoi(argv[3]), std::atoi(argv[4]));
    }
    if (argc >= 2 && std::string(argv[1]) == "--child-lock") {
        if (argc < 5) {
            return 2;
        }
        return RunLockChild(std::atoi(argv[2]), std::atoi(argv[3]), argv[4]);
    }

    go2cpp_tests::announce("distributed.multiprocess");
    ScenarioMultiProcessElection();
    ScenarioMultiProcessLock();

    const int failures = go2cpp_tests::g_failures.load();
    if (failures != 0) {
        std::fprintf(stderr, "distributed_multiprocess_test: %d checks failed\n", failures);
        return 1;
    }
    std::printf("distributed_multiprocess_test: all scenarios passed\n");
    return 0;
}
