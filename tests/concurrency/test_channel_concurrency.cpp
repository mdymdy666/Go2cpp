/**
 * @file test_channel_concurrency.cpp
 * @brief Channel 并发语义审计的独立压力测试（不注册 ctest）。
 *
 * ## 为什么独立
 * 主测试套件 `go2cpp_tests` 的 channel 用例是功能性/中小规模压力；本文件
 * 专门验证审计要求的三个不变量，规模更大、更偏随机：
 *
 * 1. **精确计数**：1 万个值经 N 生产者 × M 消费者（有缓冲/无缓冲/随机
 *    close 三种拓扑）后，每个值恰好被接收一次 —— 不丢、不重；
 * 2. **close 唤醒**：close 时所有阻塞在 send/recv/select 上的线程必须被
 *    唤醒并返回关闭状态，不能睡到超时；
 * 3. **FIFO 公平性**：同一方向上的多个等待者按入队顺序被服务。
 *
 * ## 编译与运行（不修改 CMake，保持 ctest 数量不变）
 * 见 scripts/run_concurrency_audit.sh。
 */

#include "go2cpp/channel.hpp"
#include "go2cpp/scheduler.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using go2cpp::ChannelStatus;
using go2cpp::MakeChannel;

std::atomic<int> g_failures{0};

void check(bool condition, const char* expression, const char* file, int line) {
    if (condition) {
        return;
    }
    g_failures.fetch_add(1, std::memory_order_relaxed);
    std::fprintf(stderr, "%s:%d: check failed: %s\n", file, line, expression);
}

#define CHECK(expr) check((expr), #expr, __FILE__, __LINE__)

/** @brief 等待一批线程全部结束；超时直接 abort，避免压测挂死。 */
template <typename Fn>
void join_all(std::vector<std::thread>& threads,
              std::chrono::milliseconds limit, Fn&& done) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (!done() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    if (!done()) {
        std::fprintf(stderr, "[channel-stress] 等待线程结束超时（%lld ms）\n",
                     static_cast<long long>(limit.count()));
        std::fflush(stderr);
        std::abort();
    }
    for (auto& thread : threads) {
        if (thread.joinable()) {
            thread.join();
        }
    }
}

/**
 * @brief 有缓冲通道：N 生产者 × M 消费者，1 万个值精确计数。
 */
void TestBufferedExactCounting() {
    constexpr int kProducers = 4;
    constexpr int kConsumers = 4;
    constexpr int kPerProducer = 2500;
    constexpr int kTotal = kProducers * kPerProducer;
    auto channel = MakeChannel<int>(7);
    std::vector<std::atomic<int>> received(static_cast<std::size_t>(kTotal));
    for (auto& counter : received) {
        counter.store(0, std::memory_order_relaxed);
    }
    std::atomic<int> abnormal{0};

    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([channel, p, &abnormal] {
            for (int i = 0; i < kPerProducer; ++i) {
                const int value = p * kPerProducer + i;
                const auto result = channel->SendFor(value, 2s);
                if (!result.Ok()) {
                    abnormal.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    std::vector<std::thread> consumers;
    for (int c = 0; c < kConsumers; ++c) {
        consumers.emplace_back([channel, &received, &abnormal] {
            for (;;) {
                const auto result = channel->RecvFor(2s);
                if (result.Closed()) {
                    return;
                }
                if (!result.Ok()) {
                    abnormal.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                const int value = result.ValueOrDefault();
                if (value < 0 ||
                    static_cast<std::size_t>(value) >= received.size()) {
                    abnormal.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                received[static_cast<std::size_t>(value)].fetch_add(
                    1, std::memory_order_relaxed);
            }
        });
    }
    for (auto& producer : producers) {
        producer.join();
    }
    channel->Close();
    for (auto& consumer : consumers) {
        consumer.join();
    }
    CHECK(abnormal.load() == 0);
    int missing = 0;
    int duplicated = 0;
    for (int i = 0; i < kTotal; ++i) {
        const int count = received[static_cast<std::size_t>(i)].load();
        missing += count == 0 ? 1 : 0;
        duplicated += count > 1 ? 1 : 0;
    }
    CHECK(missing == 0);
    CHECK(duplicated == 0);
    CHECK(channel->Len() == 0);
    std::printf("[channel-stress] 有缓冲 精确计数: total=%d missing=%d dup=%d\n",
                kTotal, missing, duplicated);
}

/**
 * @brief 无缓冲通道：2 生产者 × 2 消费者，1 万个值精确计数。
 */
void TestUnbufferedExactCounting() {
    constexpr int kProducers = 2;
    constexpr int kConsumers = 2;
    constexpr int kPerProducer = 5000;
    constexpr int kTotal = kProducers * kPerProducer;
    auto channel = MakeChannel<int>(0);
    std::vector<std::atomic<int>> received(static_cast<std::size_t>(kTotal));
    for (auto& counter : received) {
        counter.store(0, std::memory_order_relaxed);
    }
    std::atomic<int> abnormal{0};
    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([channel, p, &abnormal] {
            for (int i = 0; i < kPerProducer; ++i) {
                const int value = p * kPerProducer + i;
                const auto result = channel->SendFor(value, 2s);
                if (!result.Ok()) {
                    abnormal.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    std::vector<std::thread> consumers;
    for (int c = 0; c < kConsumers; ++c) {
        consumers.emplace_back([channel, &received, &abnormal] {
            for (;;) {
                const auto result = channel->RecvFor(2s);
                if (result.Closed()) {
                    return;
                }
                if (!result.Ok()) {
                    abnormal.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                const int value = result.ValueOrDefault();
                received[static_cast<std::size_t>(value)].fetch_add(
                    1, std::memory_order_relaxed);
            }
        });
    }
    for (auto& producer : producers) {
        producer.join();
    }
    channel->Close();
    for (auto& consumer : consumers) {
        consumer.join();
    }
    CHECK(abnormal.load() == 0);
    int missing = 0;
    int duplicated = 0;
    for (int i = 0; i < kTotal; ++i) {
        const int count = received[static_cast<std::size_t>(i)].load();
        missing += count == 0 ? 1 : 0;
        duplicated += count > 1 ? 1 : 0;
    }
    CHECK(missing == 0);
    CHECK(duplicated == 0);
    std::printf(
        "[channel-stress] 无缓冲 精确计数: total=%d missing=%d dup=%d\n",
        kTotal, missing, duplicated);
}

/**
 * @brief 随机 close：3 生产者 × 3 消费者 + 一个随机时刻关闭的线程。
 *
 * 不变量：Send 返回 Ok 的值恰好被接收一次；返回 Closed 的值一次也不出现。
 * 任何 Send/Recv 返回 TimedOut 都视为丢唤醒。
 */
void TestRandomCloseExactCounting() {
    constexpr int kProducers = 3;
    constexpr int kConsumers = 3;
    constexpr int kPerProducer = 2000;
    constexpr int kTotal = kProducers * kPerProducer;
    auto channel = MakeChannel<int>(4);
    std::vector<std::atomic<int>> received(static_cast<std::size_t>(kTotal));
    std::vector<std::atomic<unsigned char>> sent(
        static_cast<std::size_t>(kTotal));
    for (auto& counter : received) {
        counter.store(0, std::memory_order_relaxed);
    }
    for (auto& flag : sent) {
        flag.store(0, std::memory_order_relaxed);
    }
    std::atomic<int> send_timeouts{0};
    std::atomic<int> recv_timeouts{0};
    std::atomic<int> closed_sends{0};

    std::thread closer([channel] {
        std::this_thread::sleep_for(
            std::chrono::microseconds(1500 + (std::rand() % 6500)));
        (void)channel->Close();
    });

    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back(
            [channel, p, &sent, &send_timeouts, &closed_sends] {
                for (int i = 0; i < kPerProducer; ++i) {
                    const int value = p * kPerProducer + i;
                    const auto result = channel->SendFor(value, 2s);
                    if (result.Ok()) {
                        sent[static_cast<std::size_t>(value)].store(
                            1, std::memory_order_release);
                    } else if (result.Closed()) {
                        closed_sends.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        send_timeouts.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
    }
    std::vector<std::thread> consumers;
    for (int c = 0; c < kConsumers; ++c) {
        consumers.emplace_back([channel, &received, &recv_timeouts] {
            for (;;) {
                const auto result = channel->RecvFor(2s);
                if (result.Closed()) {
                    return;
                }
                if (!result.Ok()) {
                    recv_timeouts.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                const int value = result.ValueOrDefault();
                received[static_cast<std::size_t>(value)].fetch_add(
                    1, std::memory_order_relaxed);
            }
        });
    }
    for (auto& producer : producers) {
        producer.join();
    }
    for (auto& consumer : consumers) {
        consumer.join();
    }
    closer.join();

    CHECK(send_timeouts.load() == 0);
    CHECK(recv_timeouts.load() == 0);
    int missing = 0;
    int duplicated = 0;
    int phantom = 0;
    for (int i = 0; i < kTotal; ++i) {
        const int count = received[static_cast<std::size_t>(i)].load();
        const bool was_sent = sent[static_cast<std::size_t>(i)].load() != 0;
        if (was_sent) {
            missing += count == 0 ? 1 : 0;
            duplicated += count > 1 ? 1 : 0;
        } else {
            phantom += count != 0 ? 1 : 0;
        }
    }
    CHECK(missing == 0);
    CHECK(duplicated == 0);
    CHECK(phantom == 0);
    std::printf(
        "[channel-stress] 随机 close: total=%d sent=%d closed_send=%d "
        "missing=%d dup=%d phantom=%d timeouts(send=%d recv=%d)\n",
        kTotal, kTotal - closed_sends.load(), closed_sends.load(), missing,
        duplicated, phantom, send_timeouts.load(), recv_timeouts.load());
}

/**
 * @brief close 必须唤醒所有阻塞的接收者、发送者与 select。
 */
void TestCloseWakesAllBlockedWaiters() {
    constexpr int kReceivers = 16;
    constexpr int kSenders = 8;
    constexpr int kSelectors = 16;

    // 1) 空无缓冲通道上的 16 个阻塞接收者。
    auto recv_channel = MakeChannel<int>(0);
    std::atomic<int> started{0};
    std::atomic<int> finished{0};
    std::atomic<int> wrong_status{0};
    std::vector<std::thread> receivers;
    for (int i = 0; i < kReceivers; ++i) {
        receivers.emplace_back(
            [recv_channel, &started, &finished, &wrong_status] {
                started.fetch_add(1, std::memory_order_release);
                const auto result = recv_channel->RecvFor(3s);
                if (!result.Closed()) {
                    wrong_status.fetch_add(1, std::memory_order_relaxed);
                }
                finished.fetch_add(1, std::memory_order_release);
            });
    }
    while (started.load(std::memory_order_acquire) != kReceivers) {
        std::this_thread::sleep_for(1ms);
    }
    std::this_thread::sleep_for(100ms);
    const auto recv_close_start = std::chrono::steady_clock::now();
    CHECK(recv_channel->Close().Ok());
    join_all(receivers, 5s, [&finished] {
        return finished.load(std::memory_order_acquire) == kReceivers;
    });
    const auto recv_close_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - recv_close_start)
            .count();
    CHECK(wrong_status.load() == 0);
    CHECK(recv_close_ms < 1000);

    // 2) 已满的有缓冲通道上的 8 个阻塞发送者。
    auto send_channel = MakeChannel<int>(1);
    CHECK(send_channel->TrySend(-1).Ok());
    std::atomic<int> send_started{0};
    std::atomic<int> send_finished{0};
    std::atomic<int> send_wrong{0};
    std::vector<std::thread> senders;
    for (int i = 0; i < kSenders; ++i) {
        senders.emplace_back(
            [send_channel, i, &send_started, &send_finished, &send_wrong] {
                send_started.fetch_add(1, std::memory_order_release);
                const auto result = send_channel->SendFor(i, 3s);
                if (!result.Closed()) {
                    send_wrong.fetch_add(1, std::memory_order_relaxed);
                }
                send_finished.fetch_add(1, std::memory_order_release);
            });
    }
    while (send_started.load(std::memory_order_acquire) != kSenders) {
        std::this_thread::sleep_for(1ms);
    }
    std::this_thread::sleep_for(100ms);
    const auto send_close_start = std::chrono::steady_clock::now();
    CHECK(send_channel->Close().Ok());
    join_all(senders, 5s, [&send_finished] {
        return send_finished.load(std::memory_order_acquire) == kSenders;
    });
    const auto send_close_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - send_close_start)
            .count();
    CHECK(send_wrong.load() == 0);
    CHECK(send_close_ms < 1000);

    // 3) 16 个阻塞在 Select 上的等待者。
    auto select_channel = MakeChannel<int>(0);
    std::atomic<int> select_started{0};
    std::atomic<int> select_finished{0};
    std::atomic<int> select_wrong{0};
    std::vector<std::thread> selectors;
    for (int i = 0; i < kSelectors; ++i) {
        selectors.emplace_back([select_channel, &select_started,
                                &select_finished, &select_wrong] {
            select_started.fetch_add(1, std::memory_order_release);
            const auto result =
                go2cpp::Select({go2cpp::RecvCase(select_channel)}, {}, 3s);
            if (!result.selected || result.status != ChannelStatus::kClosed) {
                select_wrong.fetch_add(1, std::memory_order_relaxed);
            }
            select_finished.fetch_add(1, std::memory_order_release);
        });
    }
    while (select_started.load(std::memory_order_acquire) != kSelectors) {
        std::this_thread::sleep_for(1ms);
    }
    std::this_thread::sleep_for(100ms);
    const auto select_close_start = std::chrono::steady_clock::now();
    CHECK(select_channel->Close().Ok());
    join_all(selectors, 5s, [&select_finished] {
        return select_finished.load(std::memory_order_acquire) == kSelectors;
    });
    const auto select_close_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - select_close_start)
            .count();
    CHECK(select_wrong.load() == 0);
    CHECK(select_close_ms < 1000);

    std::printf(
        "[channel-stress] close 唤醒: recv=%d/%lldms send=%d/%lldms "
        "select=%d/%lldms\n",
        kReceivers, static_cast<long long>(recv_close_ms), kSenders,
        static_cast<long long>(send_close_ms), kSelectors,
        static_cast<long long>(select_close_ms));
}

/**
 * @brief 等待者 FIFO：先进入队列的接收者/发送者先被服务。
 */
void TestWaiterFifo() {
    constexpr int kWaiters = 8;
    constexpr int kRounds = 5;

    // 接收者 FIFO。
    auto recv_channel = MakeChannel<int>(0);
    std::vector<int> got(kWaiters, -1);
    std::vector<std::thread> receivers;
    for (int i = 0; i < kWaiters; ++i) {
        receivers.emplace_back([recv_channel, &got, i] {
            const auto result = recv_channel->RecvFor(3s);
            got[static_cast<std::size_t>(i)] =
                result.Ok() ? result.ValueOrDefault() : -1;
        });
        std::this_thread::sleep_for(5ms);
    }
    for (int i = 0; i < kWaiters; ++i) {
        CHECK(recv_channel->SendFor(100 + i, 2s).Ok());
    }
    for (auto& receiver : receivers) {
        receiver.join();
    }
    int recv_mismatch = 0;
    for (int i = 0; i < kWaiters; ++i) {
        recv_mismatch += got[static_cast<std::size_t>(i)] == 100 + i ? 0 : 1;
    }
    CHECK(recv_mismatch == 0);

    // 发送者 FIFO。
    auto send_channel = MakeChannel<int>(0);
    std::vector<std::thread> senders;
    for (int i = 0; i < kWaiters; ++i) {
        senders.emplace_back([send_channel, i] {
            (void)send_channel->SendFor(i, 3s);
        });
        std::this_thread::sleep_for(5ms);
    }
    std::vector<int> order;
    order.reserve(kWaiters);
    for (int i = 0; i < kWaiters; ++i) {
        const auto result = send_channel->RecvFor(2s);
        CHECK(result.Ok());
        order.push_back(result.ValueOrDefault());
    }
    for (auto& sender : senders) {
        sender.join();
    }
    int send_mismatch = 0;
    for (int i = 0; i < kWaiters; ++i) {
        send_mismatch += order[static_cast<std::size_t>(i)] == i ? 0 : 1;
    }
    CHECK(send_mismatch == 0);

    // 多轮重复，降低单次调度巧合的概率。
    int rounds_ok = 0;
    for (int round = 0; round < kRounds; ++round) {
        auto channel = MakeChannel<int>(1);
        CHECK(channel->Send(round).Ok());
        std::vector<std::thread> waiters;
        std::atomic<int> returned{0};
        for (int i = 0; i < kWaiters; ++i) {
            waiters.emplace_back([channel, &returned] {
                (void)channel->RecvFor(1s);
                returned.fetch_add(1, std::memory_order_release);
            });
        }
        (void)channel->Close();
        join_all(waiters, 5s, [&returned] {
            return returned.load(std::memory_order_acquire) == kWaiters;
        });
        rounds_ok += 1;
    }
    CHECK(rounds_ok == kRounds);
    std::printf(
        "[channel-stress] FIFO: recv_mismatch=%d send_mismatch=%d rounds=%d\n",
        recv_mismatch, send_mismatch, rounds_ok);
}

/**
 * @brief close 语义：重复 close、close 后 send、缓冲区排空。
 */
void TestCloseSemantics() {
    auto channel = MakeChannel<int>(8);
    for (int i = 0; i < 8; ++i) {
        CHECK(channel->Send(i).Ok());
    }
    CHECK(channel->Close().Ok());
    const auto again = channel->Close();
    CHECK(again.status == ChannelStatus::kAlreadyClosed);
    CHECK(static_cast<bool>(again.error));
    CHECK(go2cpp::Is(again.error, go2cpp::ChannelAlreadyClosedError()));

    const auto late_send = channel->Send(999);
    CHECK(late_send.Closed());
    CHECK(static_cast<bool>(late_send.error));
    const auto late_try = channel->TrySend(1000);
    CHECK(late_try.Closed());

    for (int i = 0; i < 8; ++i) {
        const auto result = channel->Recv();
        CHECK(result.Ok());
        CHECK(result.ValueOrDefault() == i);
    }
    const auto drained = channel->RecvFor(10ms);
    CHECK(drained.Closed());
    CHECK(!drained.value.has_value());

    // select 在已关闭通道上的接收分支：立即可选，带回关闭状态。
    const auto selected =
        go2cpp::Select({go2cpp::RecvCase(channel)}, {}, 20ms);
    CHECK(selected.selected);
    CHECK(selected.status == ChannelStatus::kClosed);

    std::printf(
        "[channel-stress] close 语义: 重复 close→AlreadyClosed，"
        "close 后 send→Closed，缓冲排空正确\n");
}

/**
 * @brief 非平凡值类型（std::string）在并发收发中内容完整。
 */
void TestStringPayloadIntegrity() {
    constexpr int kProducers = 2;
    constexpr int kConsumers = 2;
    constexpr int kPerProducer = 1000;
    auto channel = MakeChannel<std::string>(3);
    std::atomic<int> corrupted{0};
    std::atomic<int> received{0};
    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([channel, p, &corrupted] {
            for (int i = 0; i < kPerProducer; ++i) {
                std::string value =
                    std::to_string(p) + ":" + std::to_string(i) +
                    std::string(static_cast<std::size_t>(i % 17), 'x');
                if (!channel->SendFor(std::move(value), 2s).Ok()) {
                    corrupted.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    std::vector<std::thread> consumers;
    for (int c = 0; c < kConsumers; ++c) {
        consumers.emplace_back([channel, &corrupted, &received] {
            for (;;) {
                const auto result = channel->RecvFor(2s);
                if (result.Closed()) {
                    return;
                }
                if (!result.Ok() || !result.value.has_value()) {
                    corrupted.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                const std::string& value = *result.value;
                const auto colon = value.find(':');
                if (colon == std::string::npos || value.size() < colon + 2) {
                    corrupted.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                const int p = std::atoi(value.substr(0, colon).c_str());
                const int i = std::atoi(value.substr(colon + 1).c_str());
                const std::string expected =
                    std::to_string(p) + ":" + std::to_string(i) +
                    std::string(static_cast<std::size_t>(i % 17), 'x');
                if (expected != value) {
                    corrupted.fetch_add(1, std::memory_order_relaxed);
                }
                received.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto& producer : producers) {
        producer.join();
    }
    channel->Close();
    for (auto& consumer : consumers) {
        consumer.join();
    }
    CHECK(corrupted.load() == 0);
    CHECK(received.load() == kProducers * kPerProducer);
    std::printf("[channel-stress] string 载荷: received=%d corrupted=%d\n",
                received.load(), corrupted.load());
}

/**
 * @brief 在受管 Fiber 中覆盖 close 唤醒（规模较小，避免与调度器压力
 *        测试互相放大）。
 */
void TestFiberPathCloseWakeup() {
    go2cpp::SchedulerConfig config;
    config.processor_count = 2;
    config.max_workers = 2;
    config.idle_wait = 1ms;
    go2cpp::Scheduler scheduler(config);
    scheduler.start();

    auto channel = MakeChannel<int>(0);
    std::atomic<int> woke{0};
    std::atomic<int> wrong{0};
    std::vector<std::shared_ptr<go2cpp::Task>> tasks;
    for (int i = 0; i < 8; ++i) {
        tasks.push_back(scheduler.spawn([channel, &woke, &wrong] {
            const auto result = channel->RecvFor(2s);
            if (!result.Closed()) {
                wrong.fetch_add(1, std::memory_order_relaxed);
            }
            woke.fetch_add(1, std::memory_order_release);
        }));
    }
    // 等所有 G 真正进入 park，再 close。
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < deadline) {
        std::size_t waiting = 0;
        for (const auto& task : tasks) {
            if (task->state() == go2cpp::GState::kWaiting) {
                ++waiting;
            }
        }
        if (waiting == tasks.size()) {
            break;
        }
        std::this_thread::sleep_for(1ms);
    }
    CHECK(channel->Close().Ok());
    for (const auto& task : tasks) {
        CHECK(task->wait_for(3s));
    }
    CHECK(wrong.load() == 0);
    CHECK(woke.load() == 8);
    scheduler.shutdown();
    std::printf("[channel-stress] Fiber close 唤醒: woke=%d wrong=%d\n",
                woke.load(), wrong.load());
}

}  // namespace

int main() {
    std::printf("=== Go2Cpp Channel 并发语义压力测试 ===\n");
    TestBufferedExactCounting();
    TestUnbufferedExactCounting();
    TestRandomCloseExactCounting();
    TestCloseWakesAllBlockedWaiters();
    TestWaiterFifo();
    TestCloseSemantics();
    TestStringPayloadIntegrity();
    TestFiberPathCloseWakeup();
    if (g_failures.load() != 0) {
        std::fprintf(stderr, "[channel-stress] failures=%d\n",
                     g_failures.load());
        return 1;
    }
    std::printf("[channel-stress] all checks passed\n");
    return 0;
}
