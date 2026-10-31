/**
 * @file tests/concurrency/test_d1d2d3_regressions.cpp
 * @brief 分结构矩阵 D1/D2/D3 三个缺陷的最小回归测试（独立可执行，不依赖框架）。
 *
 * 背景（docs/reports/2026-10-31-stress-matrix.md §6.2.1）：
 *
 * - **D1**：`Channel::Close()` 与 `Select` 并发时丢 1 个缓冲值。根因是
 *   Select 的 probe 按扫描顺序返回「某个通道已关闭」，遮蔽了同一 select 中
 *   另一个通道缓冲里尚存的值；同时 `Close()` 对已挂起的 select/普通接收者
 *   直接判 kClosed，没有先交付自身缓冲值。修复见 `channel.hpp` 的
 *   `Close()` 与 `src/concurrency/channel.cpp` 的 probe 阶段。
 * - **D2**：`Scheduler::shutdown()` 丢弃未开始的已提交任务。这是**设计
 *   语义**（合作式关停：关闭 admission + 取消未开始 G + 等运行中 G 自然
 *   退出，不排空队列），已在 `src/concurrency/scheduler.cpp` 的
 *   `shutdown_for` 注释中写明。本测试锁定该语义：立即 shutdown 时任务
 *   允许未执行，但必须全部进入终态；需要全部执行时应先等待任务完成。
 * - **D3**：FastChannel 在 Close 后卡死/丢值。根因有两层：
 *   1) `Cap == 1` 时 Vyukov ring 的 seq 协议退化（写窗口互相覆盖 + 消费者
 *      `TryDequeue` 活锁，`dequeue_pos` 永远为 0），修复为单槽特化走
 *      `FastChannelLock`；
 *   2) `Recv/TryRecv` 在 `TryDequeue` 失败与读取 `m_closed` 之间 producer
 *      可能刚入环最后一个值再 Close，直接返回 kClosed 会丢掉它；修复为
 *      返回 kClosed 前再排空一次。
 *
 * 运行：`./test_d1d2d3_regressions [rounds]`（默认 100 轮 D3 / 50 轮 D1 /
 * 10 轮 D2）。任何子测试失败返回非 0；内置看门狗在 180s 无完成时打印
 * `[FAIL] ... 超时` 并 `_Exit(124)`，避免 CI 挂死。
 */

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>

#include "go2cpp/channel.hpp"
#include "go2cpp/concurrency/fast_channel.hpp"
#include "go2cpp/concurrency/scheduler.hpp"

namespace {

using namespace go2cpp;
using Clock = std::chrono::steady_clock;

std::atomic<bool> g_finished{false};

/// @brief 看门狗：整体超时即打印并退出，避免测试挂死在 join 上。
void Watchdog(int seconds) {
    const auto start = Clock::now();
    while (!g_finished.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (Clock::now() - start > std::chrono::seconds(seconds)) {
            std::fprintf(stderr, "[FAIL] 回归测试整体超时（疑似挂死）\n");
            std::fflush(stderr);
            std::_Exit(124);
        }
    }
}

// ---------------------------------------------------------------------------
// D3：FastChannel / FastSpscChannel 关闭唤醒 + 排空
// ---------------------------------------------------------------------------

template <typename ChannelT>
bool RunD3One(const char* name, int rounds, int count) {
    for (int r = 0; r < rounds; ++r) {
        ChannelT channel;
        std::atomic<int> consumed{0};
        std::atomic<int> last_status{99};
        std::thread consumer([&] {
            int value = 0;
            for (;;) {
                const FastStatus status = channel.Recv(value);
                last_status.store(static_cast<int>(status),
                                  std::memory_order_release);
                if (status != FastStatus::kOk) {
                    break;
                }
                consumed.fetch_add(1, std::memory_order_relaxed);
            }
        });
        for (int i = 0; i < count; ++i) {
            while (channel.TrySend(i) == FastStatus::kWouldBlock) {
                std::this_thread::yield();
            }
        }
        channel.Close();
        consumer.join();
        if (consumed.load() != count ||
            last_status.load() != static_cast<int>(FastStatus::kClosed)) {
            std::fprintf(stderr,
                         "[FAIL] D3 %s round=%d consumed=%d/%d status=%d\n",
                         name, r, consumed.load(), count, last_status.load());
            return false;
        }
    }
    std::printf("[PASS] D3 %s rounds=%d count=%d\n", name, rounds, count);
    return true;
}

bool RunD3(int rounds, int count) {
    bool ok = true;
    ok = RunD3One<FastChannel<int, 0>>("FastChannel<cap=0>", rounds, count) && ok;
    ok = RunD3One<FastChannel<int, 1>>("FastChannel<cap=1>", rounds, count) && ok;
    ok = RunD3One<FastChannel<int, 2>>("FastChannel<cap=2>", rounds, count) && ok;
    ok = RunD3One<FastChannel<int, 64>>("FastChannel<cap=64>", rounds, count) && ok;
    ok = RunD3One<FastSpscChannel<int, 1>>("FastSpsc<cap=1>", rounds, count) && ok;
    ok = RunD3One<FastSpscChannel<int, 64>>("FastSpsc<cap=64>", rounds, count) && ok;
    return ok;
}

// ---------------------------------------------------------------------------
// D1：Close 与 Select 并发不丢缓冲值
// ---------------------------------------------------------------------------

bool RunD1(int rounds, long long target) {
    for (int r = 0; r < rounds; ++r) {
        auto a = Channel<long long>::Create(1);
        auto b = Channel<long long>::Create(1);
        std::atomic<long long> produced{0};
        std::atomic<long long> consumed{0};
        std::thread producer([&] {
            for (long long i = 0; i < target; ++i) {
                auto& channel = (i & 1) ? b : a;
                while (true) {
                    const auto result = channel->TrySend(i);
                    if (result.Ok()) {
                        produced.fetch_add(1, std::memory_order_relaxed);
                        break;
                    }
                    if (result.Closed()) {
                        return;
                    }
                    std::this_thread::yield();
                }
            }
            (void)a->Close();
            (void)b->Close();
        });
        std::thread consumer([&] {
            for (;;) {
                const auto result = Select({RecvCase(a), RecvCase(b)}, {},
                                           std::chrono::milliseconds(50));
                if (result.status == ChannelStatus::kClosed) {
                    // D1：select 返回某个已关闭 case 合法，但必须把两个
                    // 通道的缓冲排空，两个都关闭才退出。
                    for (const auto& channel : {a, b}) {
                        for (;;) {
                            auto drained = channel->TryRecv();
                            if (!drained.Ok()) {
                                break;
                            }
                            consumed.fetch_add(1,
                                               std::memory_order_relaxed);
                        }
                    }
                    if (a->IsClosed() && b->IsClosed()) {
                        break;
                    }
                    continue;
                }
                if (result.selected && result.status != ChannelStatus::kReady) {
                    break;
                }
                if (result.selected) {
                    const auto value = result.Value<long long>();
                    if (!value) {
                        break;
                    }
                    consumed.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                if (produced.load(std::memory_order_acquire) >= target &&
                    consumed.load(std::memory_order_relaxed) >=
                        produced.load(std::memory_order_relaxed)) {
                    break;
                }
            }
        });
        producer.join();
        consumer.join();
        const std::size_t residual = a->Len() + b->Len();
        if (consumed.load() != target || residual != 0) {
            std::fprintf(stderr,
                         "[FAIL] D1 round=%d consumed=%lld/%lld residual=%zu "
                         "len(a)=%zu len(b)=%zu\n",
                         r, consumed.load(), target, residual, a->Len(),
                         b->Len());
            return false;
        }
    }
    std::printf("[PASS] D1 rounds=%d target=%lld\n", rounds, target);
    return true;
}

// ---------------------------------------------------------------------------
// D2：shutdown 设计语义（取消未开始，不排空）
// ---------------------------------------------------------------------------

bool WaitTaskTerminal(const std::shared_ptr<Task>& task,
                      std::chrono::milliseconds timeout) {
    const auto deadline = Clock::now() + timeout;
    while (Clock::now() < deadline) {
        const auto state = task->state();
        if (state == scheduler::GState::kDead ||
            state == scheduler::GState::kCancelled) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto state = task->state();
    return state == scheduler::GState::kDead ||
           state == scheduler::GState::kCancelled;
}

bool RunD2(int rounds) {
    for (int r = 0; r < rounds; ++r) {
        // 立即 shutdown：未开始的 G 被取消，允许 done < 16，但必须全部终态。
        {
            auto scheduler = std::make_unique<Scheduler>(2);
            scheduler->start();
            std::atomic<int> done{0};
            std::vector<std::shared_ptr<Task>> tasks;
            tasks.reserve(16);
            for (int i = 0; i < 16; ++i) {
                tasks.push_back(scheduler->spawn(
                    [&] { done.fetch_add(1, std::memory_order_relaxed); }));
            }
            scheduler->shutdown();
            int terminated = 0;
            for (const auto& task : tasks) {
                const auto state = task->state();
                if (state == scheduler::GState::kDead ||
                    state == scheduler::GState::kCancelled) {
                    ++terminated;
                }
            }
            if (terminated != 16 || done.load() > 16) {
                std::fprintf(stderr,
                             "[FAIL] D2 立即 shutdown round=%d terminated=%d "
                             "done=%d\n",
                             r, terminated, done.load());
                return false;
            }
        }
        // 正向对照：先等任务完成再 shutdown，16 个必须全部执行。
        {
            Scheduler scheduler(2);
            scheduler.start();
            std::atomic<int> done{0};
            std::vector<std::shared_ptr<Task>> tasks;
            tasks.reserve(16);
            for (int i = 0; i < 16; ++i) {
                tasks.push_back(scheduler.spawn(
                    [&] { done.fetch_add(1, std::memory_order_relaxed); }));
            }
            bool all_terminal = true;
            for (const auto& task : tasks) {
                all_terminal = WaitTaskTerminal(task, std::chrono::seconds(5)) &&
                               all_terminal;
            }
            scheduler.shutdown();
            if (!all_terminal || done.load() != 16) {
                std::fprintf(stderr,
                             "[FAIL] D2 前置排空 round=%d done=%d terminal=%d\n",
                             r, done.load(), static_cast<int>(all_terminal));
                return false;
            }
        }
    }
    std::printf("[PASS] D2 rounds=%d\n", rounds);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    const int rounds = argc > 1 ? std::atoi(argv[1]) : 100;
    std::thread watchdog(Watchdog, 180);
    watchdog.detach();

    bool ok = true;
    ok = RunD3(rounds, 2000) && ok;
    ok = RunD1(rounds > 50 ? 50 : rounds, 3000) && ok;
    ok = RunD2(rounds > 10 ? 10 : rounds) && ok;

    g_finished.store(true, std::memory_order_release);
    std::printf("RESULT %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
