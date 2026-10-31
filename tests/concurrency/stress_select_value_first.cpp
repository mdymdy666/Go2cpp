/**
 * @file tests/concurrency/stress_select_value_first.cpp
 * @brief Select「有值优先」（D1 真正修复）的独立压测程序。
 *
 * 与 `test_select_value_first.cpp`（回归/单测）分开：本文件只做大操作量压测，
 * 每个场景都可单独运行，默认 4 个场景各 2500 万操作（合计 1e8 通道操作），
 * 全部带断言与看门狗，任何不一致都打印上下文并返回非 0。
 *
 * ## 场景
 * - `close`  ：多 case + Close 并发（D1 场景）。每通道一个生产者发送严格递增
 *              序号后关闭，单消费者用 Select 接收。断言：每通道序号严格
 *              0..N-1（**无丢失、无重复、有序**）、总数一致、无死锁。
 * - `random` ：随机 case 多线程压力。4 生产者随机写 6 通道，6 消费者各用
 *              6-case Select（带 default）接收；断言计数守恒、每通道值集合
 *              完整（count + sum 双校验）、无死锁。
 * - `fiber`  ：Fiber + 原生线程混合。2 个原生线程 + 1 个 Fiber 各自生产一个
 *              通道，3 个 Fiber 用 Select 接收；断言每通道序号严格递增、
 *              总数一致（覆盖受管 G 的 park 唤醒路径）。
 * - `cancel` ：取消/超时竞争。已取消 context + 有值 / 已取消 + 空 / 已过
 *              超时 / AfterCase(0) 四种形状交替；断言每次结果合法、有值
 *              优先于取消、值不丢。
 * - `bench`  ：吞吐基准（Send+Select、D1 两 case 形状），输出 ops/s 供
 *              改动前后对比。
 *
 * ## 运行
 * `./stress_select_value_first [close|random|fiber|cancel|bench|all] [ops]`
 *
 * 默认 `all` + `ops=25000000`。`ops` 是**每个场景**的目标通道操作数
 * （发送与接收各算 1）。看门狗 1800s。
 */

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <thread>
#include <vector>

#include "go2cpp/channel.hpp"
#include "go2cpp/concurrency/context.hpp"
#include "go2cpp/concurrency/scheduler.hpp"
#include "go2cpp/runtime.hpp"

namespace {

using namespace go2cpp;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

std::atomic<bool> g_finished{false};

void Watchdog(int seconds) {
    const auto start = Clock::now();
    while (!g_finished.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (Clock::now() - start > std::chrono::seconds(seconds)) {
            std::fprintf(stderr, "[FAIL] 压测整体超时（疑似死锁/丢唤醒）\n");
            std::fflush(stderr);
            std::_Exit(124);
        }
    }
}

bool Fail(const char* scenario, const char* message) {
    std::fprintf(stderr, "[FAIL] %s: %s\n", scenario, message);
    std::fflush(stderr);
    return false;
}

double ElapsedSeconds(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

void Report(const char* scenario, long long ops, double seconds) {
    std::printf("[perf] %-7s ops=%lld elapsed=%.3fs throughput=%.2f Mops/s\n",
                scenario, ops, seconds, seconds > 0 ? ops / seconds / 1e6 : 0.0);
    std::fflush(stdout);
}

// ---------------------------------------------------------------------------
// close：多 case + Close 并发（D1）
// ---------------------------------------------------------------------------

bool RunCloseRace(long long ops) {
    constexpr int kChannels = 4;
    const long long per_channel = ops / (2LL * kChannels);
    if (per_channel < 1000) {
        return Fail("close", "ops 太小");
    }
    std::vector<ChannelPtr<long long>> channels;
    for (int i = 0; i < kChannels; ++i) {
        channels.push_back(Channel<long long>::Create(4));
    }
    const std::vector<SelectCase> cases{RecvCase(channels[0]),
                                        RecvCase(channels[1]),
                                        RecvCase(channels[2]),
                                        RecvCase(channels[3])};
    std::vector<std::jthread> producers;
    producers.reserve(kChannels);
    for (int c = 0; c < kChannels; ++c) {
        producers.emplace_back([channel = channels[c], per_channel] {
            for (long long value = 0; value < per_channel; ++value) {
                for (;;) {
                    const auto result = channel->TrySend(value);
                    if (result.Ok()) {
                        break;
                    }
                    if (result.Closed()) {
                        return;
                    }
                    std::this_thread::yield();
                }
            }
            (void)channel->Close();
        });
    }

    std::vector<long long> expected(kChannels, 0);
    long long received = 0;
    long long closed_events = 0;
    const auto start = Clock::now();
    for (;;) {
        bool all_closed = true;
        for (const auto& channel : channels) {
            all_closed = all_closed && channel->IsClosed();
        }
        if (all_closed) {
            // 通道全部关闭后按 FIFO 排空，序号必须连续。
            for (int c = 0; c < kChannels; ++c) {
                for (;;) {
                    const auto result = channels[c]->TryRecv();
                    if (!result.Ok()) {
                        break;
                    }
                    if (result.ValueOrDefault() != expected[c]) {
                        return Fail("close", "排空时序号不连续（丢值/重复/乱序）");
                    }
                    ++expected[c];
                    ++received;
                }
            }
            break;
        }
        const auto result =
            Select(cases, {}, std::optional<ContextDuration>(200ms));
        if (result.selected && result.status == ChannelStatus::kReady &&
            result.ok) {
            if (result.index >= static_cast<std::size_t>(kChannels)) {
                return Fail("close", "Select 返回越界下标");
            }
            const long long value = result.Value<long long>().value_or(-1);
            if (value != expected[result.index]) {
                std::fprintf(stderr,
                             "[FAIL] close: ch=%zu 期望 %lld 实际 %lld（丢值/"
                             "重复/乱序）\n",
                             result.index, expected[result.index], value);
                return false;
            }
            ++expected[result.index];
            ++received;
            continue;
        }
        if (result.status == ChannelStatus::kClosed) {
            ++closed_events;
            // 有值优先的直接证据：closed 返回时，所有**已关闭**通道的缓冲
            // 必须为空（关闭后不会有新值入队，Len 只减不增）。
            for (const auto& channel : channels) {
                if (channel->IsClosed() && channel->Len() != 0) {
                    return Fail("close", "closed 返回时已关闭通道仍有缓冲值");
                }
            }
            continue;
        }
        if (result.status == ChannelStatus::kTimedOut) {
            continue;
        }
        std::fprintf(stderr, "[FAIL] close: 非法结果 index=%zu status=%d\n",
                     result.index, static_cast<int>(result.status));
        return false;
    }
    for (auto& producer : producers) {
        producer.join();
    }
    const auto elapsed = ElapsedSeconds(start);
    long long total = 0;
    for (int c = 0; c < kChannels; ++c) {
        if (expected[c] != per_channel) {
            std::fprintf(stderr, "[FAIL] close: ch=%d 收到 %lld/%lld\n", c,
                         expected[c], per_channel);
            return false;
        }
        total += expected[c];
    }
    if (received != total || total != per_channel * kChannels) {
        return Fail("close", "总数不一致");
    }
    std::printf(
        "[ok]   close: 值=%lld 通道=%d closed_events=%lld（序号连续、无丢无重）\n",
        total, kChannels, closed_events);
    Report("close", 2 * total, elapsed);
    return true;
}

// ---------------------------------------------------------------------------
// random：随机 case 多线程压力（6 通道 × 4 生产者 × 6 消费者）
// ---------------------------------------------------------------------------

bool RunRandomSelect(long long ops) {
    constexpr int kChannels = 6;
    constexpr int kProducers = 4;
    constexpr int kConsumers = 6;
    const long long per_channel = ops / (2LL * kChannels);
    if (per_channel < 1000) {
        return Fail("random", "ops 太小");
    }
    std::vector<ChannelPtr<long long>> channels;
    for (int i = 0; i < kChannels; ++i) {
        channels.push_back(Channel<long long>::Create(16));
    }
    // 每通道的值由原子计数生成：值唯一，消费者用 sum + count 校验完整性。
    std::vector<std::shared_ptr<std::atomic<long long>>> next_value;
    for (int i = 0; i < kChannels; ++i) {
        next_value.push_back(std::make_shared<std::atomic<long long>>(0));
    }
    std::atomic<bool> producers_done{false};

    std::vector<std::jthread> producers;
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([p, channels, next_value, per_channel,
                                kProducers] {
            std::mt19937_64 rng(0x1234u + static_cast<unsigned>(p));
            const long long quota = per_channel * kChannels / kProducers;
            for (long long i = 0; i < quota; ++i) {
                const std::size_t slot =
                    static_cast<std::size_t>(rng() % kChannels);
                const long long value =
                    next_value[slot]->fetch_add(1, std::memory_order_relaxed);
                for (;;) {
                    const auto result = channels[slot]->TrySend(value);
                    if (result.Ok()) {
                        break;
                    }
                    if (result.Closed()) {
                        return;
                    }
                    std::this_thread::yield();
                }
            }
        });
    }

    std::vector<std::jthread> consumers;
    std::vector<std::shared_ptr<std::atomic<long long>>> recv_count;
    std::vector<std::shared_ptr<std::atomic<long long>>> recv_sum;
    for (int i = 0; i < kChannels; ++i) {
        recv_count.push_back(std::make_shared<std::atomic<long long>>(0));
        recv_sum.push_back(std::make_shared<std::atomic<long long>>(0));
    }
    std::shared_ptr<std::atomic<long long>> failures =
        std::make_shared<std::atomic<long long>>(0);
    const auto start = Clock::now();
    for (int c = 0; c < kConsumers; ++c) {
        consumers.emplace_back([c, channels, recv_count, recv_sum, failures] {
            std::vector<SelectCase> cases;
            cases.reserve(channels.size() + 1);
            for (const auto& channel : channels) {
                cases.push_back(RecvCase(channel));
            }
            cases.push_back(DefaultCase());
            std::mt19937_64 rng(0x99u + static_cast<unsigned>(c));
            while (true) {
                bool all_closed = true;
                for (const auto& channel : channels) {
                    all_closed = all_closed && channel->IsClosed();
                }
                if (all_closed) {
                    break;
                }
                const auto result = Select(cases);
                if (!result.selected) {
                    failures->fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                if (result.index < channels.size() &&
                    result.status == ChannelStatus::kReady && result.ok) {
                    const long long value = result.Value<long long>().value_or(-1);
                    recv_count[result.index]->fetch_add(1,
                                                        std::memory_order_relaxed);
                    recv_sum[result.index]->fetch_add(value,
                                                      std::memory_order_relaxed);
                } else if (result.index == channels.size()) {
                    // default：短暂让出，避免空转烧 CPU。
                    if ((rng() & 0x3Fu) == 0) {
                        std::this_thread::yield();
                    }
                }
            }
        });
    }
    for (auto& producer : producers) {
        producer.join();
    }
    producers_done.store(true, std::memory_order_release);
    for (auto& channel : channels) {
        (void)channel->Close();
    }
    for (auto& consumer : consumers) {
        consumer.join();
    }
    const auto elapsed = ElapsedSeconds(start);
    long long total = 0;
    for (int i = 0; i < kChannels; ++i) {
        const long long produced = next_value[i]->load(std::memory_order_relaxed);
        const long long count = recv_count[i]->load(std::memory_order_relaxed);
        const long long sum = recv_sum[i]->load(std::memory_order_relaxed);
        const long long expected_sum = produced * (produced - 1) / 2;
        if (count != produced || sum != expected_sum) {
            std::fprintf(stderr,
                         "[FAIL] random: ch=%d produced=%lld count=%lld sum=%lld "
                         "expect_sum=%lld\n",
                         i, produced, count, sum, expected_sum);
            return false;
        }
        total += produced;
    }
    if (failures->load(std::memory_order_relaxed) != 0) {
        return Fail("random", "出现未选中的 Select");
    }
    std::printf("[ok]   random: 值=%lld 通道=%d 生产者=%d 消费者=%d（count+sum 守恒）\n",
                total, kChannels, kProducers, kConsumers);
    Report("random", 2 * total, elapsed);
    return true;
}

// ---------------------------------------------------------------------------
// fiber：Fiber + 原生线程混合 Select
// ---------------------------------------------------------------------------

bool RunFiberMixed(long long ops) {
    constexpr int kChannels = 3;
    const long long per_channel = ops / (2LL * kChannels);
    if (per_channel < 1000) {
        return Fail("fiber", "ops 太小");
    }
    Scheduler scheduler(4);
    scheduler.start();

    std::vector<ChannelPtr<long long>> channels;
    for (int i = 0; i < kChannels; ++i) {
        channels.push_back(Channel<long long>::Create(8));
    }
    std::vector<std::shared_ptr<std::atomic<long long>>> expected;
    std::vector<std::shared_ptr<std::atomic<long long>>> received;
    for (int i = 0; i < kChannels; ++i) {
        expected.push_back(std::make_shared<std::atomic<long long>>(0));
        received.push_back(std::make_shared<std::atomic<long long>>(0));
    }
    std::shared_ptr<std::atomic<long long>> failures =
        std::make_shared<std::atomic<long long>>(0);
    const auto start = Clock::now();

    // 消费者：3 个受管 Fiber，各 Select 自己的通道；用 Select 自带超时
    // （5ms）兜底，避免每个值都创建/取消一个 AfterCase 定时器。
    std::vector<std::shared_ptr<Task>> consumers;
    for (int c = 0; c < kChannels; ++c) {
        consumers.push_back(go(scheduler, [channels, expected, received, failures,
                                           c] {
            for (;;) {
                const auto result =
                    Select({RecvCase(channels[c])}, {},
                           std::optional<ContextDuration>(5ms));
                if (result.status == ChannelStatus::kClosed) {
                    break;
                }
                if (result.selected && result.status == ChannelStatus::kReady &&
                    result.ok) {
                    const long long value =
                        result.Value<long long>().value_or(-1);
                    if (value != expected[c]->load(std::memory_order_relaxed)) {
                        failures->fetch_add(1, std::memory_order_relaxed);
                    }
                    expected[c]->fetch_add(1, std::memory_order_relaxed);
                    received[c]->fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                if (result.status == ChannelStatus::kTimedOut) {
                    continue;  // 超时兜底：继续等
                }
                failures->fetch_add(1, std::memory_order_relaxed);
                break;
            }
        }));
    }

    // 生产者：3 个原生线程（每通道一个，保证序号严格递增）。
    std::vector<std::jthread> producers;
    for (int c = 0; c < kChannels; ++c) {
        producers.emplace_back([channel = channels[c], per_channel] {
            for (long long value = 0; value < per_channel; ++value) {
                for (;;) {
                    const auto result = channel->TrySend(value);
                    if (result.Ok()) {
                        break;
                    }
                    if (result.Closed()) {
                        return;
                    }
                    std::this_thread::yield();
                }
            }
            (void)channel->Close();
        });
    }
    for (auto& producer : producers) {
        producer.join();
    }
    for (auto& consumer : consumers) {
        consumer->wait();
    }
    scheduler.shutdown();
    const auto elapsed = ElapsedSeconds(start);

    for (int i = 0; i < kChannels; ++i) {
        if (received[i]->load(std::memory_order_relaxed) != per_channel ||
            expected[i]->load(std::memory_order_relaxed) != per_channel) {
            std::fprintf(stderr, "[FAIL] fiber: ch=%d 收到 %lld/%lld\n", i,
                         received[i]->load(std::memory_order_relaxed),
                         per_channel);
            return false;
        }
    }
    if (failures->load(std::memory_order_relaxed) != 0) {
        return Fail("fiber", "出现序号错乱/非法结果");
    }
    const long long total = per_channel * kChannels;
    std::printf("[ok]   fiber: 值=%lld 通道=%d（3 Fiber 消费 + 3 线程生产）\n",
                total, kChannels);
    Report("fiber", 2 * total, elapsed);
    return true;
}

// ---------------------------------------------------------------------------
// cancel：取消/超时竞争
// ---------------------------------------------------------------------------

bool RunCancelTimeout(long long ops) {
    auto channel = Channel<long long>::Create(1);
    auto cancelled_pair = Context::WithCancel(Context::Background());
    cancelled_pair.second();
    const auto cancelled_ctx = cancelled_pair.first;
    long long iterations = ops;
    for (long long i = 0; i < iterations; ++i) {
        switch (i % 4) {
            case 0: {
                // 已取消 context + 有值：有值优先，必须交付值。
                while (!channel->TrySend(i).Ok()) {
                    (void)channel->TryRecv();
                }
                const auto result = Select(
                    {RecvCase(channel), DoneCase(cancelled_ctx)}, cancelled_ctx,
                    std::optional<ContextDuration>(10ms));
                if (!(result.selected && result.index == 0 && result.ok &&
                      result.Value<long long>().value_or(-1) == i)) {
                    std::fprintf(stderr,
                                 "[FAIL] cancel: 有值+已取消 未交付值 index=%zu "
                                 "status=%d\n",
                                 result.index, static_cast<int>(result.status));
                    return false;
                }
                break;
            }
            case 1: {
                // 已取消 context + 空：返回取消分支。
                const auto result = Select(
                    {RecvCase(channel), DoneCase(cancelled_ctx)}, cancelled_ctx,
                    std::optional<ContextDuration>(10ms));
                if (!(result.selected && result.index == 1 &&
                      result.status == ChannelStatus::kCancelled)) {
                    std::fprintf(stderr,
                                 "[FAIL] cancel: 空+已取消 结果非法 index=%zu "
                                 "status=%d\n",
                                 result.index, static_cast<int>(result.status));
                    return false;
                }
                break;
            }
            case 2: {
                // 已过超时：返回 timed out，不阻塞。
                const auto result =
                    Select({RecvCase(channel)}, {},
                           std::optional<ContextDuration>(0ms));
                if (result.status != ChannelStatus::kTimedOut) {
                    std::fprintf(stderr,
                                 "[FAIL] cancel: 0ms 超时 status=%d\n",
                                 static_cast<int>(result.status));
                    return false;
                }
                break;
            }
            default: {
                // AfterCase(0) 立即就绪：定时器分支获胜。
                const auto result =
                    Select({RecvCase(channel), AfterCase(0ms)}, {},
                           std::optional<ContextDuration>(10ms));
                if (!(result.selected && result.index == 1)) {
                    std::fprintf(stderr,
                                 "[FAIL] cancel: AfterCase(0) 结果非法 index=%zu "
                                 "status=%d\n",
                                 result.index, static_cast<int>(result.status));
                    return false;
                }
                break;
            }
        }
    }
    // 值不丢：最后一个 Send 的值还在缓冲里，必须能取到。
    (void)channel->Close();
    long long residual = 0;
    for (;;) {
        const auto result = channel->TryRecv();
        if (!result.Ok()) {
            break;
        }
        ++residual;
    }
    if (residual > 1) {
        return Fail("cancel", "残留值超过 1（重复发送/未消费）");
    }
    std::printf("[ok]   cancel: %lld 次取消/超时/定时器竞争\n", iterations);
    Report("cancel", iterations, 0.0);
    return true;
}

// ---------------------------------------------------------------------------
// bench：吞吐基准（供改动前后对比）
// ---------------------------------------------------------------------------

bool RunBench(long long ops) {
    // B1：Send + Select(Recv + Default) 单线程往返。
    {
        auto channel = Channel<int>::Create(1);
        const std::vector<SelectCase> cases{RecvCase(channel), DefaultCase()};
        long long hits = 0;
        const auto start = Clock::now();
        for (long long i = 0; i < ops; ++i) {
            (void)channel->TrySend(static_cast<int>(i));
            const auto result = Select(cases);
            if (result.selected && result.index == 0) {
                ++hits;
            }
        }
        const auto elapsed = ElapsedSeconds(start);
        if (hits != ops) {
            return Fail("bench", "B1 Send+Select 命中数不一致");
        }
        Report("bench-1", ops, elapsed);
    }
    // B2：D1 两 case 形状（一 closed、一有值）的 Select 吞吐。
    // 旧实现会有一半轮次先命中 closed（有值被遮蔽），因此这里统计命中率
    // 而不是断言 100%，用于改动前后对比。
    {
        auto closed_channel = Channel<int>::Create(1);
        auto valued_channel = Channel<int>::Create(1);
        (void)closed_channel->Close();
        const std::vector<SelectCase> cases{RecvCase(closed_channel),
                                            RecvCase(valued_channel)};
        long long hits = 0;
        const auto start = Clock::now();
        for (long long i = 0; i < ops; ++i) {
            (void)valued_channel->TrySend(static_cast<int>(i));
            const auto result = Select(cases);
            if (result.selected && result.index == 1 && result.ok) {
                ++hits;
            }
        }
        const auto elapsed = ElapsedSeconds(start);
        std::printf("[perf] bench-2 有值优先命中率 = %.2f%%（旧实现约 50%%）\n",
                    100.0 * static_cast<double>(hits) / static_cast<double>(ops));
        Report("bench-2", ops, elapsed);
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    const char* scenario = argc > 1 ? argv[1] : "all";
    const long long ops = argc > 2 ? std::atoll(argv[2]) : 25000000LL;
    std::thread watchdog(Watchdog, 1800);
    watchdog.detach();

    std::printf("== Select 有值优先压测：scenario=%s ops=%lld ==\n", scenario,
                ops);
    bool ok = true;
    if (std::strcmp(scenario, "close") == 0 || std::strcmp(scenario, "all") == 0) {
        ok = RunCloseRace(ops) && ok;
    }
    if (std::strcmp(scenario, "random") == 0 ||
        std::strcmp(scenario, "all") == 0) {
        ok = RunRandomSelect(ops) && ok;
    }
    if (std::strcmp(scenario, "fiber") == 0 || std::strcmp(scenario, "all") == 0) {
        ok = RunFiberMixed(ops) && ok;
    }
    if (std::strcmp(scenario, "cancel") == 0 ||
        std::strcmp(scenario, "all") == 0) {
        ok = RunCancelTimeout(ops) && ok;
    }
    if (std::strcmp(scenario, "bench") == 0) {
        ok = RunBench(ops) && ok;
    }
    g_finished.store(true, std::memory_order_release);
    std::printf("RESULT %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
