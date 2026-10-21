/**
 * @file test_deadlock_detection.cpp
 * @brief 故意构造的死锁场景必须在 watchdog 超时内被「检测到」，而不是挂死。
 *
 * 实现方式：每个场景在 **fork 出的子进程**里运行，父进程用 `waitpid(WNOHANG)`
 * 轮询；超过预算仍未退出则判定 STUCK，`SIGKILL` 子进程并记录。这样可以
 * 安全地观察真正的死锁（无法在进程内安全杀死卡住的线程）。
 *
 * 场景：
 *   1. lock-order 死锁：两个线程以相反顺序获取 A/B；
 *   2. channel 互等：两个无缓冲 channel，两个线程各先 Send 再 Recv；
 *   3. 条件变量丢唤醒：等待者没有 notify 永远不会醒来；
 *   4. 负对照：顺序一致的两线程加锁必须正常退出（watchdog 不误报）。
 *
 * 判定：
 *   - 场景 1~3 必须得到 STUCK（若子进程自己退出或崩溃 -> 测试失败）；
 *   - 场景 4 必须正常退出且退出码为 0。
 */

#include "go2cpp/channel.hpp"
#include "go2cpp/context.hpp"
#include "go2cpp/sync.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

using go2cpp::MakeChannel;
using go2cpp::sync::ConditionVariable;
using go2cpp::sync::Mutex;

enum class Outcome {
    kExited,   ///< 子进程自行退出
    kStuck,    ///< 超时未退出（检测到 stuck）
    kCrashed,  ///< 被信号终止（非父进程的 SIGKILL）
};

const char* OutcomeName(Outcome outcome) {
    switch (outcome) {
        case Outcome::kExited: return "exited";
        case Outcome::kStuck: return "stuck";
        case Outcome::kCrashed: return "crashed";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// 子进程场景（fork 后执行；正常返回表示「没有卡住」）
// ---------------------------------------------------------------------------
void LockOrderDeadlock() {
    Mutex a;
    Mutex b;
    std::atomic<int> ready{0};
    std::thread first([&] {
        a.Lock();
        ready.fetch_add(1, std::memory_order_release);
        while (ready.load(std::memory_order_acquire) < 2) {
            std::this_thread::yield();
        }
        b.Lock();  // 与 second 相反的顺序
        b.Unlock();
        a.Unlock();
    });
    std::thread second([&] {
        b.Lock();
        ready.fetch_add(1, std::memory_order_release);
        while (ready.load(std::memory_order_acquire) < 2) {
            std::this_thread::yield();
        }
        a.Lock();
        a.Unlock();
        b.Unlock();
    });
    first.join();
    second.join();
}

void ChannelDeadlock() {
    auto left = MakeChannel<int>(0);
    auto right = MakeChannel<int>(0);
    std::thread first([&] {
        left->Send(1);   // 无接收者：永久 park
        right->Recv();
    });
    std::thread second([&] {
        right->Send(2);  // 无接收者：永久 park
        left->Recv();
    });
    first.join();
    second.join();
}

void LostWakeupDeadlock() {
    Mutex mutex;
    ConditionVariable condition;
    bool ready = false;
    std::thread waiter([&] {
        mutex.Lock();
        while (!ready) {
            condition.Wait(mutex);  // 故意没有 notify
        }
        mutex.Unlock();
    });
    waiter.join();
}

void NormalControl() {
    Mutex first;
    Mutex second;
    std::thread one([&] {
        first.Lock();
        second.Lock();
        second.Unlock();
        first.Unlock();
    });
    std::thread two([&] {
        first.Lock();
        second.Lock();
        second.Unlock();
        first.Unlock();
    });
    one.join();
    two.join();
}

/// 在子进程中跑场景；父进程等待最多 `timeout_ms`。
Outcome RunChild(void (*scenario)(), int timeout_ms, int* exit_code) {
    *exit_code = -1;
    const pid_t pid = fork();
    if (pid < 0) {
        std::perror("fork");
        return Outcome::kCrashed;
    }
    if (pid == 0) {
        scenario();
        _exit(0);
    }

    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        int status = 0;
        const pid_t result = waitpid(pid, &status, WNOHANG);
        if (result == pid) {
            if (WIFEXITED(status)) {
                *exit_code = WEXITSTATUS(status);
                return Outcome::kExited;
            }
            return Outcome::kCrashed;
        }
        if (result < 0) {
            std::perror("waitpid");
            return Outcome::kCrashed;
        }
        if (Clock::now() >= deadline) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            return Outcome::kStuck;
        }
        std::this_thread::sleep_for(5ms);
    }
}

void CheckScenario(const char* name, void (*scenario)(), int timeout_ms,
                   bool expect_stuck, int* failures) {
    int exit_code = -1;
    const auto started = Clock::now();
    const Outcome outcome = RunChild(scenario, timeout_ms, &exit_code);
    const double elapsed =
        std::chrono::duration<double>(Clock::now() - started).count();
    const bool passed = expect_stuck ? outcome == Outcome::kStuck
                                     : outcome == Outcome::kExited &&
                                           exit_code == 0;
    std::printf(
        "[deadlock] %-22s expected=%-5s actual=%-7s exit=%-3d elapsed=%6.3fs %s\n",
        name, expect_stuck ? "stuck" : "exit0", OutcomeName(outcome),
        exit_code, elapsed, passed ? "PASS" : "FAIL");
    std::fflush(stdout);
    if (!passed) {
        ++(*failures);
    }
}

}  // namespace

int main() {
    std::printf("[deadlock] watchdog-based deadlock detection\n");
    int failures = 0;

    // 故意死锁：必须在 1.5s 左右被父进程判定为 STUCK 并 kill。
    CheckScenario("lock-order(A/B reversed)", &LockOrderDeadlock, 1500, true,
                  &failures);
    CheckScenario("channel mutual wait", &ChannelDeadlock, 1500, true,
                  &failures);
    CheckScenario("lost wakeup (no notify)", &LostWakeupDeadlock, 1500, true,
                  &failures);

    // 负对照：正常场景必须自行退出 0，证明 watchdog 不会误报。
    CheckScenario("normal control", &NormalControl, 1500, false, &failures);

    if (failures == 0) {
        std::printf("[deadlock] PASS: 3 stuck scenarios detected, control clean\n");
        return 0;
    }
    std::fprintf(stderr, "[deadlock] FAIL: %d scenario(s)\n", failures);
    return 1;
}
