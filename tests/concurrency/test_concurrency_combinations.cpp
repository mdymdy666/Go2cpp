/**
 * @file test_concurrency_combinations.cpp
 * @brief 10 类并发原语的「组合场景」正确性测试。
 *
 * 覆盖 4 个真实组合场景，每个场景都包含四条路径：
 *   1. 正常完成；2. Context 取消；3. 超时；4. 关闭/停止。
 *
 * 场景：
 *   - 生产者-消费者：Channel<int> + WaitGroup + Context 取消；
 *   - 读写缓存：RWMutex + Channel 失效通知；
 *   - 工作池：Mutex + ConditionVariable + std::deque 队列；
 *   - 管道：Channel 串联（两段）+ select 超时。
 *
 * 所有 join 都带 watchdog，失败时打印 `[watchdog]` 而不是让测试挂死。
 */

#include "go2cpp/channel.hpp"
#include "go2cpp/concurrency/locks.hpp"
#include "go2cpp/context.hpp"
#include "go2cpp/sync.hpp"
#include "test_support.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <memory>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

using go2cpp::Background;
using go2cpp::ChannelPtr;
using go2cpp::MakeChannel;
using go2cpp::SelectResult;
using go2cpp::WithCancel;
using go2cpp::sync::ConditionVariable;
using go2cpp::sync::Mutex;
using go2cpp::sync::RWMutex;
using go2cpp::sync::WaitGroup;

void Report(const char* name, Clock::time_point started, long long ops) {
    const double seconds =
        std::chrono::duration<double>(Clock::now() - started).count();
    std::printf("[comb] %-34s ops=%-9lld elapsed=%7.3fs\n", name, ops, seconds);
}

// ===========================================================================
// 场景 1：生产者-消费者（Channel + WaitGroup + Context）
// ===========================================================================
void TestProducerConsumerNormal() {
    go2cpp_tests::announce("combination: producer-consumer normal completion");
    const auto started = Clock::now();
    constexpr int kProducers = 4;
    constexpr int kConsumers = 4;
    constexpr int kPerProducer = 500;
    constexpr int kTotal = kProducers * kPerProducer;

    auto channel = MakeChannel<int>(32);
    auto ctx = Background();
    WaitGroup producers;
    producers.Add(kProducers);
    std::vector<std::atomic<int>> seen(kTotal);
    std::atomic<int> consumed{0};
    std::atomic<int> bad_status{0};

    std::vector<std::thread> threads;
    for (int p = 0; p < kProducers; ++p) {
        threads.emplace_back([&, p] {
            for (int i = 0; i < kPerProducer; ++i) {
                const auto result = channel->Send(p * kPerProducer + i);
                if (!result.Ok()) {
                    bad_status.fetch_add(1, std::memory_order_relaxed);
                }
            }
            producers.Done();
        });
    }
    std::thread closer([&] {
        producers.Wait();
        channel->Close();
    });
    for (int c = 0; c < kConsumers; ++c) {
        threads.emplace_back([&] {
            for (;;) {
                const auto result = channel->Recv(ctx);
                if (!result.Ok()) {
                    if (!result.Closed()) {
                        bad_status.fetch_add(1, std::memory_order_relaxed);
                    }
                    return;
                }
                const int value = result.ValueOrDefault();
                if (value >= 0 && value < kTotal) {
                    seen[static_cast<std::size_t>(value)].fetch_add(
                        1, std::memory_order_relaxed);
                } else {
                    bad_status.fetch_add(1, std::memory_order_relaxed);
                }
                consumed.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto& thread : threads) {
        GO2CPP_JOIN_WITH_WATCHDOG(thread, 30s);
    }
    GO2CPP_JOIN_WITH_WATCHDOG(closer, 30s);

    int missing = 0;
    int duplicated = 0;
    for (const auto& slot : seen) {
        const int count = slot.load(std::memory_order_acquire);
        missing += count == 0 ? 1 : 0;
        duplicated += count > 1 ? 1 : 0;
    }
    GO2CPP_CHECK(bad_status.load() == 0);
    GO2CPP_CHECK(consumed.load() == kTotal);
    GO2CPP_CHECK(missing == 0);
    GO2CPP_CHECK(duplicated == 0);
    Report("producer-consumer/normal", started, kTotal);
}

void TestProducerConsumerCancel() {
    go2cpp_tests::announce("combination: producer-consumer context cancel");
    const auto started = Clock::now();
    constexpr int kProducers = 4;
    constexpr int kConsumers = 4;
    constexpr int kPerProducer = 100000;  // 远大于取消前能完成的数量

    auto channel = MakeChannel<int>(16);
    auto [ctx, cancel] = WithCancel(Background());
    std::atomic<int> sent{0};
    std::atomic<int> received{0};
    std::atomic<int> unexpected_errors{0};
    std::vector<std::thread> threads;

    for (int p = 0; p < kProducers; ++p) {
        threads.emplace_back([&] {
            for (int i = 0; i < kPerProducer; ++i) {
                const auto result = channel->SendFor(i, 5s, ctx);
                if (result.Ok()) {
                    sent.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                if (result.status == go2cpp::ChannelStatus::kCancelled ||
                    result.status == go2cpp::ChannelStatus::kTimedOut ||
                    result.status == go2cpp::ChannelStatus::kClosed) {
                    return;
                }
                unexpected_errors.fetch_add(1, std::memory_order_relaxed);
                return;
            }
        });
    }
    for (int c = 0; c < kConsumers; ++c) {
        threads.emplace_back([&] {
            for (;;) {
                const auto result = channel->Recv(ctx);
                if (result.Ok()) {
                    received.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                if (result.status == go2cpp::ChannelStatus::kCancelled ||
                    result.status == go2cpp::ChannelStatus::kClosed) {
                    return;
                }
                unexpected_errors.fetch_add(1, std::memory_order_relaxed);
                return;
            }
        });
    }

    std::this_thread::sleep_for(60ms);
    cancel();
    channel->Close();  // 双保险：即使 Cancel 回调路径异常，也能唤醒原生等待者
    for (auto& thread : threads) {
        GO2CPP_JOIN_WITH_WATCHDOG(thread, 30s);
    }
    GO2CPP_CHECK(unexpected_errors.load() == 0);
    GO2CPP_CHECK(sent.load() >= received.load());
    std::printf("[comb] producer-consumer/cancel sent=%d received=%d\n",
                sent.load(), received.load());
    Report("producer-consumer/cancel", started,
           static_cast<long long>(sent.load() + received.load()));
}

void TestProducerConsumerTimeoutAndClose() {
    go2cpp_tests::announce("combination: producer-consumer timeout + close");
    const auto started = Clock::now();

    // 1) 满缓冲发送超时。
    auto full = MakeChannel<int>(1);
    GO2CPP_CHECK(full->Send(1).Ok());
    const auto full_result = full->SendFor(2, 30ms);
    GO2CPP_CHECK(!full_result.Ok());
    GO2CPP_CHECK(full_result.status == go2cpp::ChannelStatus::kTimedOut);

    // 2) 空缓冲接收超时。
    auto empty = MakeChannel<int>(1);
    const auto empty_result = empty->RecvFor(30ms);
    GO2CPP_CHECK(!empty_result.Ok());
    GO2CPP_CHECK(empty_result.status == go2cpp::ChannelStatus::kTimedOut);

    // 3) 关闭唤醒阻塞消费者。
    auto blocked = MakeChannel<int>(0);
    std::atomic<int> wake_reason{0};
    std::thread consumer([&] {
        const auto result = blocked->Recv();
        if (result.Closed()) {
            wake_reason.store(1, std::memory_order_release);
        } else if (result.Ok()) {
            wake_reason.store(2, std::memory_order_release);
        } else {
            wake_reason.store(3, std::memory_order_release);
        }
    });
    std::this_thread::sleep_for(20ms);
    blocked->Close();
    GO2CPP_JOIN_WITH_WATCHDOG(consumer, 10s);
    GO2CPP_CHECK(wake_reason.load() == 1);

    // 4) 关闭唤醒阻塞发送者，并让后续 Send/Recv 都返回 closed。
    auto blocked_send = MakeChannel<int>(0);
    std::atomic<int> send_reason{0};
    std::thread sender([&] {
        const auto result = blocked_send->Send(7);
        send_reason.store(result.Closed() ? 1 : 2, std::memory_order_release);
    });
    std::this_thread::sleep_for(20ms);
    blocked_send->Close();
    GO2CPP_JOIN_WITH_WATCHDOG(sender, 10s);
    GO2CPP_CHECK(send_reason.load() == 1);
    GO2CPP_CHECK(blocked_send->Send(8).Closed());
    GO2CPP_CHECK(blocked_send->Recv().Closed());
    GO2CPP_CHECK(blocked_send->TryRecv().Closed());
    blocked_send->Close();  // 幂等：不得崩溃
    GO2CPP_CHECK(blocked_send->IsClosed());
    Report("producer-consumer/timeout+close", started, 8);
}

// ===========================================================================
// 场景 2：读写缓存（RWMutex + Channel 失效通知）
// ===========================================================================
struct CacheSnapshot {
    std::int64_t version{0};
    std::int64_t payload{0};
};

void TestReadWriteCacheNormal() {
    go2cpp_tests::announce("combination: rw-cache normal (2 writers, 4 readers)");
    const auto started = Clock::now();
    constexpr int kWriters = 2;
    constexpr int kReaders = 4;
    constexpr int kPerWriter = 5000;
    constexpr int kReadsPerReader = 20000;

    RWMutex mutex;
    CacheSnapshot cache;
    auto notification = MakeChannel<std::int64_t>(128);
    std::atomic<int> torn{0};
    std::atomic<int> reads{0};
    std::atomic<int> writes{0};
    std::atomic<int> notified{0};
    std::vector<std::thread> threads;

    for (int w = 0; w < kWriters; ++w) {
        threads.emplace_back([&] {
            for (int i = 0; i < kPerWriter; ++i) {
                mutex.Lock();
                ++cache.version;
                cache.payload = cache.version * 2;
                const auto version = cache.version;
                mutex.Unlock();
                writes.fetch_add(1, std::memory_order_relaxed);
                const auto sent = notification->TrySend(version);
                if (sent.Ok()) {
                    notified.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (int r = 0; r < kReaders; ++r) {
        threads.emplace_back([&] {
            for (int i = 0; i < kReadsPerReader; ++i) {
                mutex.RLock();
                const CacheSnapshot snapshot = cache;
                mutex.RUnlock();
                if (snapshot.payload != snapshot.version * 2) {
                    torn.fetch_add(1, std::memory_order_relaxed);
                }
                reads.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto& thread : threads) {
        GO2CPP_JOIN_WITH_WATCHDOG(thread, 60s);
    }
    GO2CPP_CHECK(torn.load() == 0);
    GO2CPP_CHECK(writes.load() == kWriters * kPerWriter);
    GO2CPP_CHECK(reads.load() == kReaders * kReadsPerReader);
    GO2CPP_CHECK(cache.version == kWriters * kPerWriter);
    GO2CPP_CHECK(cache.payload == cache.version * 2);
    GO2CPP_CHECK(!mutex.IsWriteLocked());
    std::printf("[comb] rw-cache/normal writes=%d reads=%d notified=%d\n",
                writes.load(), reads.load(), notified.load());
    Report("rw-cache/normal", started,
           static_cast<long long>(writes.load() + reads.load()));
}

void TestReadWriteCacheCancel() {
    go2cpp_tests::announce("combination: rw-cache cancellation + channel close");
    const auto started = Clock::now();
    constexpr int kReaders = 4;

    RWMutex mutex;
    CacheSnapshot cache;
    auto invalidation = MakeChannel<int>(32);
    auto [ctx, cancel] = WithCancel(Background());
    std::atomic<int> read_versions{0};
    std::atomic<int> exits{0};
    std::vector<std::thread> readers;

    for (int r = 0; r < kReaders; ++r) {
        readers.emplace_back([&] {
            for (;;) {
                // 先等失效通知（带取消）；取消/关闭都退出，超时继续读。
                const auto notice = invalidation->RecvFor(20ms, ctx);
                if (notice.status == go2cpp::ChannelStatus::kCancelled ||
                    notice.Closed()) {
                    exits.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                mutex.RLock();
                const auto version = cache.version;
                mutex.RUnlock();
                read_versions.fetch_add(static_cast<int>(version),
                                        std::memory_order_relaxed);
            }
        });
    }
    std::thread writer([&] {
        for (int i = 0; i < 1000; ++i) {
            mutex.Lock();
            ++cache.version;
            mutex.Unlock();
            invalidation->TrySend(i);
            std::this_thread::sleep_for(100us);
        }
    });
    GO2CPP_JOIN_WITH_WATCHDOG(writer, 30s);
    cancel();
    invalidation->Close();
    for (auto& reader : readers) {
        GO2CPP_JOIN_WITH_WATCHDOG(reader, 30s);
    }
    GO2CPP_CHECK(exits.load() == kReaders);
    GO2CPP_CHECK(cache.version == 1000);
    std::printf("[comb] rw-cache/cancel readers_exited=%d version=%lld\n",
                exits.load(), static_cast<long long>(cache.version));
    Report("rw-cache/cancel", started, 1000 + kReaders);
}

void TestReadWriteCacheTimeout() {
    go2cpp_tests::announce("combination: rw-cache lock timeout while writer holds");
    const auto started = Clock::now();

    RWMutex mutex;
    GO2CPP_CHECK(mutex.LockFor(10ms));
    std::atomic<bool> write_timeout{false};
    std::atomic<bool> read_timeout{false};
    std::thread contender([&] {
        write_timeout.store(!mutex.LockFor(30ms), std::memory_order_release);
        read_timeout.store(!mutex.RLockFor(30ms), std::memory_order_release);
    });
    GO2CPP_JOIN_WITH_WATCHDOG(contender, 10s);
    GO2CPP_CHECK(write_timeout.load());
    GO2CPP_CHECK(read_timeout.load());
    GO2CPP_CHECK(mutex.IsWriteLocked());
    mutex.Unlock();

    // 释放后同一线程应能拿到读锁。
    GO2CPP_CHECK(mutex.RLockFor(100ms));
    mutex.RUnlock();
    Report("rw-cache/timeout", started, 2);
}

// ===========================================================================
// 场景 3：工作池（Mutex + ConditionVariable + 队列）
// ===========================================================================
void TestWorkPoolNormal() {
    go2cpp_tests::announce("combination: work-pool normal completion");
    const auto started = Clock::now();
    constexpr int kWorkers = 4;
    constexpr int kTasks = 2000;

    Mutex mutex;
    ConditionVariable condition;
    std::deque<int> queue;
    bool stopped = false;
    std::atomic<int> processed{0};
    std::atomic<std::int64_t> sum{0};
    std::int64_t expected_sum = 0;
    for (int i = 0; i < kTasks; ++i) {
        expected_sum += i;
    }

    std::vector<std::thread> workers;
    for (int w = 0; w < kWorkers; ++w) {
        workers.emplace_back([&] {
            for (;;) {
                int task = 0;
                {
                    GO2CPP_CHECK(mutex.Lock());
                    while (queue.empty() && !stopped) {
                        condition.Wait(mutex);
                    }
                    if (queue.empty() && stopped) {
                        mutex.Unlock();
                        return;
                    }
                    task = queue.front();
                    queue.pop_front();
                    mutex.Unlock();
                }
                sum.fetch_add(task, std::memory_order_relaxed);  // 每个任务恰好被一个 worker 处理
                processed.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    for (int i = 0; i < kTasks; ++i) {
        GO2CPP_CHECK(mutex.Lock());
        queue.push_back(i);
        condition.NotifyOne();
        mutex.Unlock();
    }
    // 等到全部处理完再停止。
    GO2CPP_REQUIRE_EVENTUALLY(processed.load() == kTasks, 30s);
    {
        GO2CPP_CHECK(mutex.Lock());
        stopped = true;
        condition.NotifyAll();
        mutex.Unlock();
    }
    for (auto& worker : workers) {
        GO2CPP_JOIN_WITH_WATCHDOG(worker, 30s);
    }
    GO2CPP_CHECK(processed.load() == kTasks);
    GO2CPP_CHECK(sum.load() == expected_sum);
    GO2CPP_CHECK(queue.empty());
    Report("work-pool/normal", started, kTasks);
}

void TestWorkPoolCancelAndTimeout() {
    go2cpp_tests::announce("combination: work-pool cancellation + cv timeout");
    const auto started = Clock::now();
    constexpr int kWorkers = 4;

    Mutex mutex;
    ConditionVariable condition;
    std::deque<int> queue;
    auto [ctx, cancel] = WithCancel(Background());
    std::atomic<int> cancelled_workers{0};
    std::atomic<int> timeout_observed{0};
    std::atomic<int> processed{0};

    // 超时路径：无通知时 WaitFor 返回 false，且调用方仍持有锁。
    GO2CPP_CHECK(mutex.Lock());
    GO2CPP_CHECK(!condition.WaitFor(mutex, 20ms));
    timeout_observed.fetch_add(1, std::memory_order_relaxed);
    mutex.Unlock();

    std::vector<std::thread> workers;
    for (int w = 0; w < kWorkers; ++w) {
        workers.emplace_back([&] {
            for (;;) {
                GO2CPP_CHECK(mutex.Lock());
                while (queue.empty()) {
                    if (ctx->IsDone()) {
                        mutex.Unlock();
                        cancelled_workers.fetch_add(1,
                                                    std::memory_order_relaxed);
                        return;
                    }
                    // 带超时的等待：超时返回 false 但锁仍归调用方，
                    // 避免把「取消时是否重锁」的实现差异带进场景。
                    if (!condition.WaitFor(mutex, 10ms)) {
                        timeout_observed.fetch_add(1,
                                                   std::memory_order_relaxed);
                    }
                }
                const int task = queue.front();
                queue.pop_front();
                mutex.Unlock();
                (void)task;
                processed.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::sleep_for(200us);
            }
        });
    }
    for (int i = 0; i < 200; ++i) {
        GO2CPP_CHECK(mutex.Lock());
        queue.push_back(i);
        condition.NotifyOne();
        mutex.Unlock();
    }
    std::this_thread::sleep_for(30ms);
    cancel();
    condition.NotifyAll();
    for (auto& worker : workers) {
        GO2CPP_JOIN_WITH_WATCHDOG(worker, 30s);
    }
    GO2CPP_CHECK(cancelled_workers.load() == kWorkers);
    GO2CPP_CHECK(timeout_observed.load() >= 1);
    std::printf("[comb] work-pool/cancel processed=%d cancelled=%d timeouts=%d\n",
                processed.load(), cancelled_workers.load(),
                timeout_observed.load());
    Report("work-pool/cancel+timeout", started,
           static_cast<long long>(processed.load() + kWorkers));
}

void TestWorkPoolStopDrainsQueue() {
    go2cpp_tests::announce("combination: work-pool stop wakes all workers");
    const auto started = Clock::now();
    constexpr int kWorkers = 4;

    Mutex mutex;
    ConditionVariable condition;
    std::deque<int> queue;
    bool stopped = false;
    std::atomic<int> exits{0};
    std::vector<std::thread> workers;
    for (int w = 0; w < kWorkers; ++w) {
        workers.emplace_back([&] {
            GO2CPP_CHECK(mutex.Lock());
            while (!stopped) {
                condition.Wait(mutex);
            }
            mutex.Unlock();
            exits.fetch_add(1, std::memory_order_relaxed);
        });
    }
    std::this_thread::sleep_for(20ms);
    {
        GO2CPP_CHECK(mutex.Lock());
        stopped = true;
        condition.NotifyAll();
        mutex.Unlock();
    }
    for (auto& worker : workers) {
        GO2CPP_JOIN_WITH_WATCHDOG(worker, 10s);
    }
    GO2CPP_CHECK(exits.load() == kWorkers);
    GO2CPP_CHECK(queue.empty());
    Report("work-pool/stop", started, kWorkers);
}

// ===========================================================================
// 场景 4：管道（Channel 串联 + 超时）
// ===========================================================================
void TestPipelineNormal() {
    go2cpp_tests::announce("combination: pipeline normal (2 stages, 3+2 workers)");
    const auto started = Clock::now();
    constexpr int kValues = 2000;

    auto input = MakeChannel<int>(32);
    auto middle = MakeChannel<int>(32);
    auto output = MakeChannel<int>(32);
    std::atomic<std::int64_t> sum{0};
    std::atomic<int> received{0};
    std::atomic<int> errors{0};

    WaitGroup stage1_done;
    WaitGroup stage2_done;
    stage1_done.Add(3);
    stage2_done.Add(2);

    std::vector<std::thread> stage1;
    for (int i = 0; i < 3; ++i) {
        stage1.emplace_back([&] {
            for (;;) {
                const auto result = input->Recv();
                if (!result.Ok()) {
                    stage1_done.Done();
                    return;
                }
                const int value = result.ValueOrDefault();
                if (!middle->SendFor(value * 2, 5s).Ok()) {
                    errors.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    std::thread closer1([&] {
        stage1_done.Wait();
        middle->Close();
    });
    std::vector<std::thread> stage2;
    for (int i = 0; i < 2; ++i) {
        stage2.emplace_back([&] {
            for (;;) {
                const auto result = middle->Recv();
                if (!result.Ok()) {
                    stage2_done.Done();
                    return;
                }
                const int value = result.ValueOrDefault();
                if (!output->SendFor(value + 1, 5s).Ok()) {
                    errors.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    std::thread closer2([&] {
        stage2_done.Wait();
        output->Close();
    });
    std::thread sink([&] {
        for (;;) {
            const auto result = output->Recv();
            if (!result.Ok()) {
                return;
            }
            sum.fetch_add(result.ValueOrDefault(), std::memory_order_relaxed);
            received.fetch_add(1, std::memory_order_relaxed);
        }
    });
    std::thread producer([&] {
        for (int i = 0; i < kValues; ++i) {
            if (!input->Send(i).Ok()) {
                return;
            }
        }
        input->Close();
    });

    GO2CPP_JOIN_WITH_WATCHDOG(producer, 30s);
    GO2CPP_JOIN_WITH_WATCHDOG(closer1, 30s);
    for (auto& thread : stage1) {
        GO2CPP_JOIN_WITH_WATCHDOG(thread, 30s);
    }
    GO2CPP_JOIN_WITH_WATCHDOG(closer2, 30s);
    for (auto& thread : stage2) {
        GO2CPP_JOIN_WITH_WATCHDOG(thread, 30s);
    }
    GO2CPP_JOIN_WITH_WATCHDOG(sink, 30s);

    std::int64_t expected = 0;
    for (int i = 0; i < kValues; ++i) {
        expected += static_cast<std::int64_t>(i) * 2 + 1;
    }
    GO2CPP_CHECK(received.load() == kValues);
    GO2CPP_CHECK(sum.load() == expected);
    GO2CPP_CHECK(errors.load() == 0);
    Report("pipeline/normal", started, kValues);
}

void TestPipelineCancelAndClose() {
    go2cpp_tests::announce("combination: pipeline cancellation + close cascade");
    const auto started = Clock::now();
    constexpr int kValues = 100000;

    auto input = MakeChannel<int>(16);
    auto middle = MakeChannel<int>(16);
    auto output = MakeChannel<int>(16);
    auto [ctx, cancel] = WithCancel(Background());
    std::atomic<int> stage_exits{0};
    std::atomic<int> received{0};
    std::atomic<int> timeouts{0};
    std::atomic<int> cancels{0};

    auto stage = [&](ChannelPtr<int> in, ChannelPtr<int> out, int transform) {
        return std::thread([&, in, out, transform] {
            for (;;) {
                const SelectResult selected = go2cpp::Select(
                    {go2cpp::RecvCase(in), go2cpp::DoneCase(ctx)},
                    ctx, std::nullopt);
                if (selected.selected && selected.index == 0) {
                    if (!selected.ok) {
                        out->Close();
                        stage_exits.fetch_add(1, std::memory_order_relaxed);
                        return;
                    }
                    const int value = selected.Value<int>().value_or(0);
                    if (!out->SendFor(value + transform, 5s, ctx).Ok()) {
                        timeouts.fetch_add(1, std::memory_order_relaxed);
                    }
                } else if (selected.selected && selected.index == 1) {
                    cancels.fetch_add(1, std::memory_order_relaxed);
                    stage_exits.fetch_add(1, std::memory_order_relaxed);
                    out->Close();
                    return;
                } else {
                    timeouts.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    };
    std::vector<std::thread> stages;
    stages.push_back(stage(input, middle, 1));
    stages.push_back(stage(middle, output, 2));
    std::thread sink([&] {
        for (;;) {
            const auto result = output->RecvFor(200ms, ctx);
            if (result.Ok()) {
                received.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            if (result.status == go2cpp::ChannelStatus::kTimedOut) {
                if (ctx->IsDone() || output->IsClosed()) {
                    stage_exits.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                continue;
            }
            stage_exits.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    });
    std::thread producer([&] {
        for (int i = 0; i < kValues; ++i) {
            if (!input->SendFor(i, 5s, ctx).Ok()) {
                return;
            }
        }
        input->Close();
    });
    std::this_thread::sleep_for(50ms);
    cancel();
    input->Close();
    middle->Close();
    output->Close();
    GO2CPP_JOIN_WITH_WATCHDOG(producer, 30s);
    GO2CPP_JOIN_WITH_WATCHDOG(sink, 30s);
    for (auto& thread : stages) {
        GO2CPP_JOIN_WITH_WATCHDOG(thread, 30s);
    }
    GO2CPP_CHECK(stage_exits.load() == 3);
    std::printf(
        "[comb] pipeline/cancel received=%d cancels=%d timeouts=%d exits=%d\n",
        received.load(), cancels.load(), timeouts.load(), stage_exits.load());
    Report("pipeline/cancel+close", started, received.load() + 3);
}

void TestPipelineSelectTimeout() {
    go2cpp_tests::announce("combination: pipeline select done/default/ready");
    const auto started = Clock::now();

    auto input = MakeChannel<int>(1);

    // 方案说明：普通 OS 线程没有启动 TimerService，真正等待 AfterCase 的
    // Select 会 park 在定时器上；这里不测该路径（超时由 RecvFor 覆盖），
    // 只测 DoneCase / DefaultCase / 数据就绪三条不依赖定时器线程的分支。
    auto [ctx, cancel] = WithCancel(Background());
    cancel();
    const auto done = go2cpp::Select(
        {go2cpp::RecvCase(input), go2cpp::DoneCase(ctx)}, ctx, std::nullopt);
    GO2CPP_CHECK(done.selected);
    GO2CPP_CHECK(done.index == 1);
    GO2CPP_CHECK(done.status == go2cpp::ChannelStatus::kCancelled);

    // default 分支立即返回。
    const auto immediate = go2cpp::Select(
        {go2cpp::RecvCase(input), go2cpp::DefaultCase()}, Background(),
        std::nullopt);
    GO2CPP_CHECK(immediate.selected);
    GO2CPP_CHECK(immediate.index == 1);
    GO2CPP_CHECK(immediate.status == go2cpp::ChannelStatus::kReady);

    // 有数据时 Recv 分支获胜（probe 阶段就绪，不会登记定时器）。
    GO2CPP_CHECK(input->Send(42).Ok());
    const auto ready = go2cpp::Select(
        {go2cpp::RecvCase(input), go2cpp::DefaultCase()}, Background(),
        std::nullopt);
    GO2CPP_CHECK(ready.selected);
    GO2CPP_CHECK(ready.index == 0);
    GO2CPP_CHECK(ready.Value<int>().value_or(-1) == 42);
    Report("pipeline/select-done", started, 3);
}

}  // namespace

int main() {
    std::printf("[comb] Go2Cpp concurrency combination tests\n");
    TestProducerConsumerNormal();
    TestProducerConsumerCancel();
    TestProducerConsumerTimeoutAndClose();
    TestReadWriteCacheNormal();
    TestReadWriteCacheCancel();
    TestReadWriteCacheTimeout();
    TestWorkPoolNormal();
    TestWorkPoolCancelAndTimeout();
    TestWorkPoolStopDrainsQueue();
    TestPipelineNormal();
    TestPipelineCancelAndClose();
    TestPipelineSelectTimeout();

    if (go2cpp_tests::g_failures.load(std::memory_order_acquire) == 0) {
        std::printf("[comb] PASS (12 scenarios)\n");
        return 0;
    }
    std::fprintf(stderr, "[comb] FAIL: %d checks failed\n",
                 go2cpp_tests::g_failures.load(std::memory_order_acquire));
    return 1;
}
