/**
 * @file test_fast_channel.cpp
 * @brief FastChannel / FastSpscChannel 的功能、并发、FIFO、close、随机与
 *        Fiber×Thread 四种组合、5 个真实场景测试（独立可执行）。
 *
 * 覆盖矩阵：
 *  1. 功能：TrySend/TryRecv 的满/空边界、FIFO、close 排空与零值、幂等 close；
 *  2. 并发：1P1C（cap0/cap64/cap1024）、4P3C（cap64）、SPSC 1P1C；
 *  3. close 唤醒：阻塞中的 N 个接收者/发送者被 Close 一次性唤醒；
 *  4. 随机：单线程随机操作序列（对拍 std::deque 模型）、多线程随机 Try 操作；
 *  5. Fiber×Thread：Fiber→Fiber / Fiber→Thread / Thread→Fiber / Thread→Thread；
 *  6. 5 个真实场景：网络收包流水线、请求-响应 rendezvous、日志汇聚、关闭排空、
 *     背压限流。
 *
 * 编译（独立，不并入 go2cpp_tests）：
 *   g++ -std=c++20 -O2 -Wall -Wextra -Wpedantic -Iinclude \
 *       tests/concurrency/test_fast_channel.cpp \
 *       -L.build-bento-wsl -lgo2cpp_runtime ... -o /tmp/test_fast_channel
 * 实际由 CMake target `go2cpp_fast_channel_test` 构建与注册。
 */

#include "go2cpp/concurrency/fast_channel.hpp"
#include "go2cpp/scheduler.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using go2cpp::FastChannel;
using go2cpp::FastSpscChannel;
using go2cpp::FastStatus;

std::atomic<int> g_failures{0};

void Check(bool condition, const char* expression, const char* file, int line) {
    if (condition) {
        return;
    }
    g_failures.fetch_add(1, std::memory_order_relaxed);
    std::fprintf(stderr, "%s:%d: check failed: %s\n", file, line, expression);
}

#define CHECK(expr) Check((expr), #expr, __FILE__, __LINE__)

/// @brief 有超时的 join：任何场景挂死都明确失败，而不是让 ctest 超时。
template <typename Fn>
void JoinWithDeadline(std::vector<std::thread>& threads,
                      std::chrono::milliseconds limit, Fn&& done) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (!done() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    if (!done()) {
        std::fprintf(stderr, "[fast-channel] 等待线程结束超时（%lld ms）\n",
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

// ---------------------------------------------------------------------------
// 1. 功能
// ---------------------------------------------------------------------------

void TestFastChannelBasics() {
    std::printf("[test] TestFastChannelBasics\n");
    std::fflush(stdout);
    FastChannel<int, 64> channel;
    CHECK(channel.Capacity() == 64);
    CHECK(channel.Len() == 0);
    CHECK(!channel.IsClosed());

    for (int i = 0; i < 64; ++i) {
        CHECK(channel.TrySend(i) == FastStatus::kOk);
    }
    CHECK(channel.Len() == 64);
    CHECK(channel.TrySend(64) == FastStatus::kWouldBlock);
    CHECK(channel.Len() == 64);

    for (int i = 0; i < 64; ++i) {
        int value = -1;
        CHECK(channel.TryRecv(value) == FastStatus::kOk);
        CHECK(value == i);
    }
    int value = -1;
    CHECK(channel.TryRecv(value) == FastStatus::kWouldBlock);
    CHECK(value == -1);  // 失败不应写 out

    channel.Close();
    CHECK(channel.IsClosed());
    CHECK(channel.TrySend(1) == FastStatus::kClosed);
    value = 12345;
    CHECK(channel.TryRecv(value) == FastStatus::kClosed);
    CHECK(value == 0);  // Go 语义：closed 写零值
    channel.Close();    // 幂等
}

void TestFastChannelCap0() {
    std::printf("[test] TestFastChannelCap0\n");
    std::fflush(stdout);
    FastChannel<std::uint64_t, 0> channel;
    std::uint64_t value = 0;
    CHECK(channel.TrySend(1) == FastStatus::kWouldBlock);
    CHECK(channel.TryRecv(value) == FastStatus::kWouldBlock);

    std::atomic<std::uint64_t> got{0};
    std::thread consumer([&] {
        for (std::uint64_t i = 0; i < 2000; ++i) {
            std::uint64_t received = 0;
            CHECK(channel.Recv(received) == FastStatus::kOk);
            CHECK(received == i);
            got.store(i + 1, std::memory_order_release);
        }
    });
    for (std::uint64_t i = 0; i < 2000; ++i) {
        CHECK(channel.Send(i) == FastStatus::kOk);
    }
    consumer.join();
    CHECK(got.load() == 2000);
    channel.Close();
    CHECK(channel.TryRecv(value) == FastStatus::kClosed);
    CHECK(value == 0);
}

void TestFastChannelLargePayload() {
    std::printf("[test] TestFastChannelLargePayload\n");
    std::fflush(stdout);
    FastChannel<std::string, 8> channel;
    CHECK(channel.TrySend(std::string(200, 'x')) == FastStatus::kOk);
    std::string value;
    CHECK(channel.TryRecv(value) == FastStatus::kOk);
    CHECK(value.size() == 200);
    CHECK(channel.TrySend("hello") == FastStatus::kOk);
    channel.Close();
    CHECK(channel.TryRecv(value) == FastStatus::kOk);  // 排空
    CHECK(value == "hello");
    CHECK(channel.TryRecv(value) == FastStatus::kClosed);
    CHECK(value.empty());  // 零值
}

void TestSpscBasics() {
    std::printf("[test] TestSpscBasics\n");
    std::fflush(stdout);
    FastSpscChannel<int, 16> channel;
    CHECK(channel.Capacity() == 16);
    for (int i = 0; i < 16; ++i) {
        CHECK(channel.TrySend(i) == FastStatus::kOk);
    }
    CHECK(channel.TrySend(16) == FastStatus::kWouldBlock);
    CHECK(channel.Len() == 16);
    for (int i = 0; i < 16; ++i) {
        int value = -1;
        CHECK(channel.TryRecv(value) == FastStatus::kOk);
        CHECK(value == i);
    }
    int value = -1;
    CHECK(channel.TryRecv(value) == FastStatus::kWouldBlock);
    channel.Close();
    CHECK(channel.IsClosed());
    CHECK(channel.TrySend(1) == FastStatus::kClosed);
    value = 7;
    CHECK(channel.TryRecv(value) == FastStatus::kClosed);
    CHECK(value == 0);

    // 阻塞 fill/drain：生产 5000 条，消费 5000 条。
    FastSpscChannel<std::uint64_t, 32> pipe;
    std::atomic<std::uint64_t> received{0};
    std::thread consumer([&] {
        for (std::uint64_t i = 0; i < 5000; ++i) {
            std::uint64_t item = 0;
            CHECK(pipe.Recv(item) == FastStatus::kOk);
            CHECK(item == i);
            received.store(i + 1, std::memory_order_release);
        }
    });
    for (std::uint64_t i = 0; i < 5000; ++i) {
        CHECK(pipe.Send(i) == FastStatus::kOk);
    }
    consumer.join();
    CHECK(received.load() == 5000);
}

// ---------------------------------------------------------------------------
// 2. 并发正确性
// ---------------------------------------------------------------------------

void TestFastChannelSpscThreads() {
    std::printf("[test] TestFastChannelSpscThreads\n");
    std::fflush(stdout);
    constexpr int kTotal = 200000;
    FastChannel<int, 64> channel;
    std::atomic<int> bad{0};
    std::thread producer([&] {
        for (int i = 0; i < kTotal; ++i) {
            if (channel.Send(i) != FastStatus::kOk) {
                bad.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });
    std::thread consumer([&] {
        for (int i = 0; i < kTotal; ++i) {
            int value = -1;
            if (channel.Recv(value) != FastStatus::kOk || value != i) {
                bad.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });
    producer.join();
    consumer.join();
    CHECK(bad.load() == 0);
    CHECK(channel.Len() == 0);
}

void TestFastChannelMpmc() {
    std::printf("[test] TestFastChannelMpmc\n");
    std::fflush(stdout);
    constexpr int kProducers = 4;
    constexpr int kConsumers = 3;
    constexpr int kPerProducer = 20000;
    constexpr int kTotal = kProducers * kPerProducer;
    FastChannel<int, 64> channel;
    std::vector<std::atomic<int>> seen(static_cast<std::size_t>(kTotal));
    for (auto& slot : seen) {
        slot.store(0, std::memory_order_relaxed);
    }
    std::atomic<int> duplicates{0};
    std::atomic<int> consumed{0};

    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&, p] {
            for (int i = 0; i < kPerProducer; ++i) {
                CHECK(channel.Send(p * kPerProducer + i) == FastStatus::kOk);
            }
        });
    }
    std::vector<std::thread> consumers;
    for (int c = 0; c < kConsumers; ++c) {
        consumers.emplace_back([&] {
            for (;;) {
                int value = 0;
                const auto status = channel.Recv(value);
                if (status == FastStatus::kClosed) {
                    return;
                }
                if (status != FastStatus::kOk) {
                    continue;
                }
                if (seen[static_cast<std::size_t>(value)].fetch_add(
                        1, std::memory_order_relaxed) != 0) {
                    duplicates.fetch_add(1, std::memory_order_relaxed);
                }
                consumed.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto& producer : producers) {
        producer.join();
    }
    channel.Close();
    for (auto& consumer : consumers) {
        consumer.join();
    }
    CHECK(consumed.load() == kTotal);
    CHECK(duplicates.load() == 0);
}

void TestFastChannelCloseWakesAll() {
    std::printf("[test] TestFastChannelCloseWakesAll\n");
    std::fflush(stdout);
    FastChannel<int, 0> channel;
    constexpr int kWaiters = 8;
    std::atomic<int> closed_count{0};
    std::vector<std::thread> receivers;
    for (int i = 0; i < kWaiters; ++i) {
        receivers.emplace_back([&] {
            int value = -1;
            if (channel.Recv(value) == FastStatus::kClosed && value == 0) {
                closed_count.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    // 等所有接收者真正阻塞（用 Len 无法判断；直接睡一小段后 close）。
    std::this_thread::sleep_for(50ms);
    channel.Close();
    for (auto& thread : receivers) {
        thread.join();
    }
    CHECK(closed_count.load() == kWaiters);

    // 发送者侧：cap0 无接收者，8 个发送者阻塞后 Close 全部返回 kClosed。
    FastChannel<int, 0> send_channel;
    std::atomic<int> senders_closed{0};
    std::vector<std::thread> senders;
    for (int i = 0; i < kWaiters; ++i) {
        senders.emplace_back([&, i] {
            if (send_channel.Send(i) == FastStatus::kClosed) {
                senders_closed.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    std::this_thread::sleep_for(50ms);
    send_channel.Close();
    for (auto& thread : senders) {
        thread.join();
    }
    CHECK(senders_closed.load() == kWaiters);
}

void TestSpscThreads() {
    std::printf("[test] TestSpscThreads\n");
    std::fflush(stdout);
    constexpr int kTotal = 200000;
    FastSpscChannel<std::uint64_t, 1024> channel;
    std::atomic<int> bad{0};
    std::thread producer([&] {
        for (std::uint64_t i = 0; i < kTotal; ++i) {
            if (channel.Send(i) != FastStatus::kOk) {
                bad.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });
    std::thread consumer([&] {
        for (std::uint64_t i = 0; i < kTotal; ++i) {
            std::uint64_t value = 0;
            if (channel.Recv(value) != FastStatus::kOk || value != i) {
                bad.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });
    producer.join();
    consumer.join();
    CHECK(bad.load() == 0);
}

// ---------------------------------------------------------------------------
// 3. 随机测试
// ---------------------------------------------------------------------------

void TestRandomSingleThread() {
    std::printf("[test] TestRandomSingleThread\n");
    std::fflush(stdout);
    // 对拍模型：TrySend 成功则入 deque；TryRecv 成功则必须与 deque 队首一致。
    FastChannel<int, 32> channel;
    std::deque<int> model;
    std::mt19937 rng(20261030);
    std::uniform_int_distribution<int> op(0, 3);
    std::uniform_int_distribution<int> value_dist(0, 1000000);
    int next_value = 0;
    for (int step = 0; step < 200000; ++step) {
        switch (op(rng)) {
            case 0:
            case 1: {
                const int item = value_dist(rng);
                const auto status = channel.TrySend(item);
                if (static_cast<int>(model.size()) < 32) {
                    CHECK(status == FastStatus::kOk);
                    model.push_back(item);
                } else {
                    CHECK(status == FastStatus::kWouldBlock);
                }
                break;
            }
            case 2:
            case 3: {
                int out = -1;
                const auto status = channel.TryRecv(out);
                if (model.empty()) {
                    CHECK(status == FastStatus::kWouldBlock);
                } else {
                    CHECK(status == FastStatus::kOk);
                    CHECK(out == model.front());
                    model.pop_front();
                }
                break;
            }
            default:
                break;
        }
        ++next_value;
    }
    CHECK(static_cast<int>(model.size()) == static_cast<int>(channel.Len()));
    channel.Close();
    while (!model.empty()) {
        int out = -1;
        CHECK(channel.TryRecv(out) == FastStatus::kOk);
        CHECK(out == model.front());
        model.pop_front();
    }
    int out = -1;
    CHECK(channel.TryRecv(out) == FastStatus::kClosed);
    (void)next_value;
}

void TestRandomMultiThreadTry() {
    std::printf("[test] TestRandomMultiThreadTry\n");
    std::fflush(stdout);
    // 3 生产者 × 3 消费者，随机在 TrySend/TryRecv 之间切换；总量守恒。
    constexpr int kProducers = 3;
    constexpr int kConsumers = 3;
    constexpr int kPerProducer = 30000;
    FastChannel<int, 16> channel;
    std::atomic<int> sent{0};
    std::atomic<int> received{0};
    std::atomic<int> duplicates{0};
    std::vector<std::atomic<int>> seen(
        static_cast<std::size_t>(kProducers * kPerProducer));
    for (auto& slot : seen) {
        slot.store(0, std::memory_order_relaxed);
    }

    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&, p] {
            std::mt19937 rng(static_cast<unsigned>(p) + 7);
            for (int i = 0; i < kPerProducer; ++i) {
                const int item = p * kPerProducer + i;
                for (;;) {
                    const auto status = channel.TrySend(item);
                    if (status == FastStatus::kOk) {
                        sent.fetch_add(1, std::memory_order_relaxed);
                        break;
                    }
                    if (status == FastStatus::kClosed) {
                        return;
                    }
                    if ((rng() & 7) == 0) {
                        std::this_thread::yield();
                    }
                }
            }
        });
    }
    std::vector<std::thread> consumers;
    for (int c = 0; c < kConsumers; ++c) {
        consumers.emplace_back([&] {
            for (;;) {
                int value = 0;
                const auto status = channel.TryRecv(value);
                if (status == FastStatus::kOk) {
                    if (seen[static_cast<std::size_t>(value)].fetch_add(
                            1, std::memory_order_relaxed) != 0) {
                        duplicates.fetch_add(1, std::memory_order_relaxed);
                    }
                    received.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                if (status == FastStatus::kClosed) {
                    return;
                }
                std::this_thread::yield();
            }
        });
    }
    for (auto& producer : producers) {
        producer.join();
    }
    // 等消费者收完（最多 5 秒），再关闭。
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (received.load(std::memory_order_relaxed) <
               kProducers * kPerProducer &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    channel.Close();
    for (auto& consumer : consumers) {
        consumer.join();
    }
    CHECK(sent.load() == kProducers * kPerProducer);
    CHECK(received.load() == kProducers * kPerProducer);
    CHECK(duplicates.load() == 0);
}

// ---------------------------------------------------------------------------
// 4. Fiber × Thread 四种组合
// ---------------------------------------------------------------------------

void TestFiberThreadCombinations() {
    std::printf("[test] TestFiberThreadCombinations\n");
    std::fflush(stdout);
    constexpr int kTotal = 5000;
    go2cpp::SchedulerConfig config;
    config.processor_count = 1;
    config.max_workers = 1;
    config.idle_wait = 1ms;
    go2cpp::Scheduler scheduler(config);
    scheduler.start();

    // 组合 1：Fiber 生产 → 线程消费（FastChannel cap64）。
    {
        auto channel = std::make_shared<FastChannel<int, 64>>();
        std::atomic<int> bad{0};
        std::thread consumer([&] {
            for (int i = 0; i < kTotal; ++i) {
                int value = -1;
                if (channel->Recv(value) != FastStatus::kOk || value != i) {
                    bad.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
        auto task = scheduler.spawn([channel] {
            for (int i = 0; i < kTotal; ++i) {
                channel->Send(i);
            }
        });
        CHECK(task->wait_for(20s));
        consumer.join();
        CHECK(bad.load() == 0);
    }

    // 组合 2：线程生产 → Fiber 消费。
    {
        auto channel = std::make_shared<FastChannel<int, 64>>();
        std::atomic<int> bad{0};
        auto task = scheduler.spawn([channel, &bad] {
            for (int i = 0; i < kTotal; ++i) {
                int value = -1;
                if (channel->Recv(value) != FastStatus::kOk || value != i) {
                    bad.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
        std::thread producer([&] {
            for (int i = 0; i < kTotal; ++i) {
                channel->Send(i);
            }
        });
        producer.join();
        CHECK(task->wait_for(20s));
        CHECK(bad.load() == 0);
    }

    // 组合 3：Fiber ↔ Fiber（cap0 rendezvous）。
    {
        auto channel = std::make_shared<FastChannel<int, 0>>();
        std::atomic<int> bad{0};
        auto producer = scheduler.spawn([channel] {
            for (int i = 0; i < kTotal; ++i) {
                channel->Send(i);
            }
        });
        auto consumer = scheduler.spawn([channel, &bad] {
            for (int i = 0; i < kTotal; ++i) {
                int value = -1;
                if (channel->Recv(value) != FastStatus::kOk || value != i) {
                    bad.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
        CHECK(producer->wait_for(20s));
        CHECK(consumer->wait_for(20s));
        CHECK(bad.load() == 0);
    }

    // 组合 4：线程 ↔ 线程（SPSC cap1024）。
    {
        FastSpscChannel<std::uint64_t, 1024> channel;
        std::atomic<int> bad{0};
        std::thread producer([&] {
            for (std::uint64_t i = 0; i < kTotal; ++i) {
                channel.Send(i);
            }
        });
        std::thread consumer([&] {
            for (std::uint64_t i = 0; i < kTotal; ++i) {
                std::uint64_t value = 0;
                if (channel.Recv(value) != FastStatus::kOk || value != i) {
                    bad.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
        producer.join();
        consumer.join();
        CHECK(bad.load() == 0);
    }

    // 组合 4b：Fiber 生产 → Fiber 消费（SPSC，同一 Scheduler）。
    {
        auto channel = std::make_shared<FastSpscChannel<int, 64>>();
        std::atomic<int> bad{0};
        auto producer = scheduler.spawn([channel] {
            for (int i = 0; i < kTotal; ++i) {
                channel->Send(i);
            }
        });
        auto consumer = scheduler.spawn([channel, &bad] {
            for (int i = 0; i < kTotal; ++i) {
                int value = -1;
                if (channel->Recv(value) != FastStatus::kOk || value != i) {
                    bad.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
        CHECK(producer->wait_for(20s));
        CHECK(consumer->wait_for(20s));
        CHECK(bad.load() == 0);
    }

    scheduler.shutdown();
}

// ---------------------------------------------------------------------------
// 5. 五个真实场景
// ---------------------------------------------------------------------------

/// 场景 1：网络收包流水线（SPSC cap1024，生产者线程 → 消费者线程，逐包序号）。
void ScenarioNetworkPipeline() {
    std::printf("[test] ScenarioNetworkPipeline\n");
    std::fflush(stdout);
    constexpr int kPackets = 100000;
    FastSpscChannel<std::uint64_t, 1024> channel;
    std::atomic<int> bad{0};
    std::atomic<std::uint64_t> checksum{0};
    std::thread producer([&] {
        for (int i = 0; i < kPackets; ++i) {
            channel.Send(static_cast<std::uint64_t>(i) * 3 + 1);
        }
    });
    std::thread consumer([&] {
        std::uint64_t sum = 0;
        for (int i = 0; i < kPackets; ++i) {
            std::uint64_t value = 0;
            if (channel.Recv(value) != FastStatus::kOk ||
                value != static_cast<std::uint64_t>(i) * 3 + 1) {
                bad.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            sum += value;
        }
        checksum.store(sum, std::memory_order_relaxed);
    });
    producer.join();
    consumer.join();
    CHECK(bad.load() == 0);
    CHECK(checksum.load() ==
          static_cast<std::uint64_t>(kPackets) * (kPackets - 1) / 2 * 3 +
              static_cast<std::uint64_t>(kPackets));
    std::printf("[scenario] 网络收包流水线: packets=%d bad=%d\n", kPackets,
                bad.load());
}

/// 场景 2：请求-响应 rendezvous（cap0，线程客户端 ↔ Fiber 服务端）。
void ScenarioRequestResponse() {
    std::printf("[test] ScenarioRequestResponse\n");
    std::fflush(stdout);
    constexpr int kRequests = 2000;
    auto requests = std::make_shared<FastChannel<int, 0>>();
    auto responses = std::make_shared<FastChannel<int, 0>>();
    go2cpp::SchedulerConfig config;
    config.processor_count = 1;
    config.max_workers = 1;
    go2cpp::Scheduler scheduler(config);
    scheduler.start();
    std::atomic<int> bad{0};
    auto server = scheduler.spawn([requests, responses] {
        for (int i = 0; i < kRequests; ++i) {
            int value = -1;
            requests->Recv(value);
            responses->Send(value * 2);
        }
    });
    std::thread client([&] {
        for (int i = 0; i < kRequests; ++i) {
            requests->Send(i);
            int response = -1;
            if (responses->Recv(response) != FastStatus::kOk ||
                response != i * 2) {
                bad.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });
    client.join();
    CHECK(server->wait_for(20s));
    CHECK(bad.load() == 0);
    scheduler.shutdown();
    std::printf("[scenario] 请求-响应 rendezvous: requests=%d bad=%d\n",
                kRequests, bad.load());
}

/// 场景 3：多生产者日志汇聚（4P1C，cap64，统计总量与序号完整性）。
void ScenarioLogAggregation() {
    std::printf("[test] ScenarioLogAggregation\n");
    std::fflush(stdout);
    constexpr int kProducers = 4;
    constexpr int kPerProducer = 20000;
    constexpr int kTotal = kProducers * kPerProducer;
    FastChannel<int, 64> channel;
    std::vector<std::atomic<int>> seen(static_cast<std::size_t>(kTotal));
    for (auto& slot : seen) {
        slot.store(0, std::memory_order_relaxed);
    }
    std::atomic<int> duplicates{0};
    std::atomic<int> received{0};
    std::thread consumer([&] {
        for (;;) {
            int value = 0;
            const auto status = channel.Recv(value);
            if (status == FastStatus::kClosed) {
                return;
            }
            if (status != FastStatus::kOk) {
                continue;
            }
            if (seen[static_cast<std::size_t>(value)].fetch_add(
                    1, std::memory_order_relaxed) != 0) {
                duplicates.fetch_add(1, std::memory_order_relaxed);
            }
            received.fetch_add(1, std::memory_order_relaxed);
        }
    });
    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&, p] {
            for (int i = 0; i < kPerProducer; ++i) {
                channel.Send(p * kPerProducer + i);
            }
        });
    }
    for (auto& producer : producers) {
        producer.join();
    }
    channel.Close();
    consumer.join();
    CHECK(received.load() == kTotal);
    CHECK(duplicates.load() == 0);
    std::printf("[scenario] 日志汇聚: total=%d received=%d dup=%d\n", kTotal,
                received.load(), duplicates.load());
}

/// 场景 4：关闭排空（生产者发 1000 条后 Close，消费者必须收到全部再 closed）。
void ScenarioCloseDrain() {
    std::printf("[test] ScenarioCloseDrain\n");
    std::fflush(stdout);
    constexpr int kTotal = 1000;
    FastChannel<int, 32> channel;
    std::thread producer([&] {
        for (int i = 0; i < kTotal; ++i) {
            channel.Send(i);
        }
        channel.Close();
    });
    int expected = 0;
    for (;;) {
        int value = -1;
        const auto status = channel.Recv(value);
        if (status == FastStatus::kClosed) {
            CHECK(value == 0);
            break;
        }
        CHECK(status == FastStatus::kOk);
        CHECK(value == expected);
        ++expected;
    }
    producer.join();
    CHECK(expected == kTotal);
    std::printf("[scenario] 关闭排空: drained=%d\n", expected);
}

/// 场景 5：背压限流（cap8，慢消费者，生产者被阻塞但顺序/完整性不变）。
void ScenarioBackpressure() {
    std::printf("[test] ScenarioBackpressure\n");
    std::fflush(stdout);
    constexpr int kTotal = 5000;
    FastSpscChannel<int, 8> channel;
    std::atomic<int> bad{0};
    std::thread producer([&] {
        for (int i = 0; i < kTotal; ++i) {
            if (channel.Send(i) != FastStatus::kOk) {
                bad.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });
    std::thread consumer([&] {
        int expected = 0;
        for (int i = 0; i < kTotal; ++i) {
            int value = -1;
            if (channel.Recv(value) != FastStatus::kOk || value != expected) {
                bad.fetch_add(1, std::memory_order_relaxed);
            }
            ++expected;
            if ((i & 255) == 0) {
                std::this_thread::sleep_for(100us);  // 模拟慢消费者
            }
        }
    });
    producer.join();
    consumer.join();
    CHECK(bad.load() == 0);
    std::printf("[scenario] 背压限流: total=%d bad=%d\n", kTotal, bad.load());
}

}  // namespace

int main() {
    std::printf("=== Go2Cpp FastChannel 测试 ===\n");
    TestFastChannelBasics();
    TestFastChannelCap0();
    TestFastChannelLargePayload();
    TestSpscBasics();
    TestFastChannelSpscThreads();
    TestFastChannelMpmc();
    TestFastChannelCloseWakesAll();
    TestSpscThreads();
    TestRandomSingleThread();
    TestRandomMultiThreadTry();
    TestFiberThreadCombinations();
    ScenarioNetworkPipeline();
    ScenarioRequestResponse();
    ScenarioLogAggregation();
    ScenarioCloseDrain();
    ScenarioBackpressure();
    if (g_failures.load() != 0) {
        std::fprintf(stderr, "[fast-channel] failures=%d\n",
                     g_failures.load());
        return 1;
    }
    std::printf("[fast-channel] all checks passed\n");
    return 0;
}
