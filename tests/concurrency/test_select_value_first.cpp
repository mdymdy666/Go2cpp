/**
 * @file tests/concurrency/test_select_value_first.cpp
 * @brief Select「有值优先」（审计 D1 真正修复）的回归、并发与随机压力测试。
 *
 * ## 背景
 * 审计 D1：`Select` 与 `Close` 并发时，probe 阶段按线程本地随机起点扫描
 * case。若先命中一个「已关闭且缓冲为空」的 recv case，就立刻返回 `kClosed`；
 * 同一 select 中另一个通道里的缓冲值仍然留在队列里，调用方看到 `kClosed`
 * 退出后该值再也没人取走（`Close` 不会丢值，丢值发生在 Select 的选择顺序）。
 *
 * ## 修复语义
 * probe 前先用无副作用的 `peek` 给内建 channel case 分类，按
 * 「有值 > 可发送 > 关闭/其它」三档探测（详见
 * `docs/modules/select-semantics.md`）。probe 本身有副作用（recv 会取走值、
 * send 会完成发送），所以不能「全部探测一遍再挑最好的」。
 *
 * ## 用例
 * - T1 确定性复现：关闭且空的通道不得遮蔽另一个通道的缓冲值（旧实现约
 *   50% 轮次失败）；取走值后的下一次 select 才返回 `kClosed`。
 * - T2 两个通道都关闭且都有值：先交付两个值，再返回 `kClosed`（任务要求
 *   「两通道都关的场景：先返回值，再 closed」）。
 * - T3 并发 Close + Select：`a` 关闭时 `b` 已有值（Close 与 Select 并发），
 *   select 必须优先交付 `b` 的值，而不是 `a` 的 `kClosed`。
 * - T4 随机 Select 压力：默认 1000 万次单线程 + 400 万次多线程，校验结果
 *   合法、计数守恒、无挂起。
 * - S1~S5 真实场景：优雅关闭、关闭+取消竞争、default 轮询、双向
 *   ping-pong、超时与关闭竞争。
 *
 * ## 运行
 * `./test_select_value_first [single_thread_iters]`
 *
 * 默认 `single_thread_iters = 10000000`。看门狗 600s 未完成直接 `_Exit(124)`，
 * 避免 CI 挂在 join 上而不是给出可读失败。
 */

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
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
int g_failures = 0;

/// @brief 看门狗：整体超时直接退出，避免测试进程挂在 join 上。
void Watchdog(int seconds) {
    const auto start = Clock::now();
    while (!g_finished.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (Clock::now() - start > std::chrono::seconds(seconds)) {
            std::fprintf(stderr, "[FAIL] 测试整体超时（疑似挂死/丢唤醒）\n");
            std::fflush(stderr);
            std::_Exit(124);
        }
    }
}

bool Check(bool ok, const char* name) {
    std::printf("%s %s\n", ok ? "[ok]  " : "[FAIL]", name);
    std::fflush(stdout);
    if (!ok) {
        ++g_failures;
    }
    return ok;
}

// ---------------------------------------------------------------------------
// T1：关闭且空的通道不能遮蔽另一个通道的缓冲值（D1 的确定性复现）
// ---------------------------------------------------------------------------

bool TestClosedDoesNotShadowValue() {
    constexpr int kRounds = 2000;
    for (int round = 0; round < kRounds; ++round) {
        auto closed_channel = Channel<int>::Create(1);
        auto valued_channel = Channel<int>::Create(1);
        if (!valued_channel->TrySend(round).Ok()) {
            std::fprintf(stderr, "[FAIL] T1 无法预置值 round=%d\n", round);
            return false;
        }
        (void)closed_channel->Close();

        // 旧实现：随机起点若先 probe 到 closed_channel（空且已关闭），直接
        // 返回 kClosed，valued_channel 里的 round 被留在缓冲里。
        const auto first = Select({RecvCase(closed_channel), RecvCase(valued_channel)},
                                  {}, std::optional<ContextDuration>(10ms));
        if (!(first.selected && first.index == 1 && first.ok &&
              first.status == ChannelStatus::kReady)) {
            std::fprintf(stderr,
                         "[FAIL] T1 round=%d 关闭 case 遮蔽了有值 case："
                         "index=%zu selected=%d status=%d\n",
                         round, first.index, static_cast<int>(first.selected),
                         static_cast<int>(first.status));
            return false;
        }
        const auto value = first.Value<int>();
        if (!value.has_value() || *value != round) {
            std::fprintf(stderr, "[FAIL] T1 round=%d 取到的值不匹配\n", round);
            return false;
        }

        // 值取走后，下一次 select 才允许返回 closed（先值后 closed）。
        const auto second = Select({RecvCase(closed_channel), RecvCase(valued_channel)},
                                   {}, std::optional<ContextDuration>(10ms));
        if (!(second.selected && second.index == 0 &&
              second.status == ChannelStatus::kClosed)) {
            std::fprintf(stderr,
                         "[FAIL] T1 round=%d 取走值后未返回 closed："
                         "index=%zu status=%d\n",
                         round, second.index, static_cast<int>(second.status));
            return false;
        }
    }
    return Check(true, "T1 关闭且空的 case 不遮蔽另一个通道的缓冲值");
}

// ---------------------------------------------------------------------------
// T2：两个通道都关闭且都有值 —— 先交付两个值，再返回 closed
// ---------------------------------------------------------------------------

bool TestBothClosedDeliverValuesFirst() {
    constexpr int kRounds = 2000;
    for (int round = 0; round < kRounds; ++round) {
        auto a = Channel<int>::Create(1);
        auto b = Channel<int>::Create(1);
        if (!a->TrySend(round * 2).Ok() || !b->TrySend(round * 2 + 1).Ok()) {
            std::fprintf(stderr, "[FAIL] T2 预置值失败 round=%d\n", round);
            return false;
        }
        (void)a->Close();
        (void)b->Close();

        int values = 0;
        int closed_hits = 0;
        // 收集到「两个值 + 两次 closed」即可停止；排空完成后继续 select 只会
        // 一直返回 closed，不能要求恰好两次。
        for (int attempt = 0; attempt < 12 && (values < 2 || closed_hits < 2);
             ++attempt) {
            const auto result = Select({RecvCase(a), RecvCase(b)}, {},
                                       std::optional<ContextDuration>(5ms));
            if (!result.selected) {
                std::fprintf(stderr,
                             "[FAIL] T2 round=%d attempt=%d 未选中：status=%d\n",
                             round, attempt, static_cast<int>(result.status));
                return false;
            }
            if (result.status == ChannelStatus::kReady && result.ok) {
                ++values;
                continue;
            }
            if (result.status == ChannelStatus::kClosed) {
                // 关键顺序：closed 只能出现在两个值都交付之后。
                if (values < 2) {
                    std::fprintf(stderr,
                                 "[FAIL] T2 round=%d closed 先于值出现："
                                 "values=%d attempt=%d\n",
                                 round, values, attempt);
                    return false;
                }
                ++closed_hits;
                continue;
            }
            std::fprintf(stderr,
                         "[FAIL] T2 round=%d 非法结果 index=%zu status=%d\n",
                         round, result.index, static_cast<int>(result.status));
            return false;
        }
        // 两个值必须全部交付，之后两次 closed 才算排空完成。
        if (values != 2 || closed_hits != 2) {
            std::fprintf(stderr,
                         "[FAIL] T2 round=%d 先值后 closed 不成立："
                         "values=%d closed=%d\n",
                         round, values, closed_hits);
            return false;
        }
    }
    return Check(true, "T2 两通道都关闭：先交付全部值，再返回 closed");
}

// ---------------------------------------------------------------------------
// T3：Close 与 Select 并发 —— a 关闭时 b 已有值，必须优先交付 b 的值
// ---------------------------------------------------------------------------

bool TestConcurrentClosePrefersValue() {
    constexpr int kRounds = 1000;
    for (int round = 0; round < kRounds; ++round) {
        auto a = Channel<int>::Create(1);
        auto b = Channel<int>::Create(1);
        std::atomic<bool> produced{false};

        // producer：先让 b 有值，再关闭 a。consumer 只在观察到 a 关闭后才
        // 开始消费，因此此刻 b 必定仍有值（Close 与 Select 并发）。
        std::thread producer([&] {
            while (!b->TrySend(round).Ok()) {
                std::this_thread::yield();
            }
            (void)a->Close();
            produced.store(true, std::memory_order_release);
        });

        while (!a->IsClosed()) {
            std::this_thread::yield();
        }
        const auto result = Select({RecvCase(a), RecvCase(b)}, {},
                                   std::optional<ContextDuration>(50ms));
        const bool ok = result.selected && result.index == 1 && result.ok &&
                        result.Value<int>().value_or(-1) == round;
        if (!ok) {
            std::fprintf(stderr,
                         "[FAIL] T3 round=%d a 关闭后未优先交付 b 的值："
                         "index=%zu selected=%d status=%d ok=%d\n",
                         round, result.index, static_cast<int>(result.selected),
                         static_cast<int>(result.status),
                         static_cast<int>(result.ok));
            producer.join();
            return false;
        }
        const auto after = Select({RecvCase(a), RecvCase(b)}, {},
                                  std::optional<ContextDuration>(50ms));
        producer.join();
        if (!(after.selected && after.index == 0 &&
              after.status == ChannelStatus::kClosed)) {
            std::fprintf(stderr,
                         "[FAIL] T3 round=%d 取走值后未返回 a 的 closed："
                         "index=%zu status=%d\n",
                         round, after.index, static_cast<int>(after.status));
            return false;
        }
        if (!produced.load(std::memory_order_acquire)) {
            std::fprintf(stderr, "[FAIL] T3 round=%d producer 未完成\n", round);
            return false;
        }
    }
    return Check(true, "T3 Close 与 Select 并发时有值 case 优先于 closed case");
}

// ---------------------------------------------------------------------------
// T4a：单线程随机 Select 压力（默认 1000 万次，全部落在「有值」快路径）
// ---------------------------------------------------------------------------

bool TestSingleThreadStress(long long iters) {
    auto channels = std::vector<ChannelPtr<long long>>{
        Channel<long long>::Create(1), Channel<long long>::Create(1),
        Channel<long long>::Create(4), Channel<long long>::Create(4)};
    const std::vector<SelectCase> cases{
        RecvCase(channels[0]), RecvCase(channels[1]), RecvCase(channels[2]),
        RecvCase(channels[3])};
    std::mt19937_64 rng(0xC0FFEEull);
    long long selected_count = 0;
    for (long long i = 0; i < iters; ++i) {
        const std::size_t slot =
            static_cast<std::size_t>(rng() % channels.size());
        auto& channel = channels[slot];
        // 保证本轮至少有一个 case 就绪：先放一个值，若通道已满先取走旧值。
        while (!channel->TrySend(i).Ok()) {
            if (!channel->TryRecv().Ok()) {
                break;
            }
        }
        const auto result = Select(cases);
        if (!result.selected || result.index >= cases.size() ||
            result.status != ChannelStatus::kReady || !result.ok) {
            std::fprintf(stderr,
                         "[FAIL] T4a iter=%lld 非法结果 index=%zu status=%d\n",
                         i, result.index, static_cast<int>(result.status));
            return false;
        }
        ++selected_count;
    }
    if (selected_count != iters) {
        std::fprintf(stderr, "[FAIL] T4a 计数不匹配 selected=%lld/%lld\n",
                     selected_count, iters);
        return false;
    }
    std::printf("[perf] T4a 单线程 Select 压力完成：%lld 次\n", iters);
    return true;
}

// ---------------------------------------------------------------------------
// T4b：多线程随机 Select 压力 —— 发送总数必须等于接收总数（不丢值）
// ---------------------------------------------------------------------------

bool TestConcurrentStress(int workers, long long per_worker) {
    constexpr std::size_t kChannelCount = 3;
    std::vector<ChannelPtr<long long>> channels;
    for (std::size_t i = 0; i < kChannelCount; ++i) {
        channels.push_back(Channel<long long>::Create(4));
    }
    std::atomic<long long> sent{0};
    std::atomic<long long> received{0};
    std::atomic<long long> failures{0};
    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(workers));
    for (int w = 0; w < workers; ++w) {
        threads.emplace_back([w, per_worker, channels, &sent, &received,
                              &failures] {
            std::mt19937_64 rng(0x5EEDu + static_cast<unsigned>(w));
            // 多线程同时 select 时不能保证任意时刻都有值可读，因此必须带
            // DefaultCase；否则所有 worker 可能一起阻塞在有空通道的 select
            // 上（无人发送）——那是测试自身死锁，不是被测语义。
            const std::vector<SelectCase> cases{
                RecvCase(channels[0]), RecvCase(channels[1]),
                RecvCase(channels[2]), DefaultCase()};
            for (long long i = 0; i < per_worker; ++i) {
                const std::size_t slot =
                    static_cast<std::size_t>(rng() % channels.size());
                // 随机发送/接收：发送成功才计发送数，接收成功才计接收数。
                if ((rng() & 1u) != 0) {
                    if (channels[slot]->TrySend(i).Ok()) {
                        sent.fetch_add(1, std::memory_order_relaxed);
                    }
                } else {
                    const auto result = Select(cases);
                    if (result.selected && result.ok &&
                        result.status == ChannelStatus::kReady &&
                        result.index < 3) {
                        received.fetch_add(1, std::memory_order_relaxed);
                    } else if (!result.selected) {
                        failures.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    // 关闭后把缓冲里的剩余值全部取出来；守恒式：sent == received + drained。
    long long drained = 0;
    for (const auto& channel : channels) {
        (void)channel->Close();
        for (;;) {
            const auto result = channel->TryRecv();
            if (!result.Ok()) {
                break;
            }
            ++drained;
        }
    }
    const long long total_sent = sent.load(std::memory_order_relaxed);
    const long long total_received = received.load(std::memory_order_relaxed);
    const long long total_failed = failures.load(std::memory_order_relaxed);
    std::printf(
        "[perf] T4b 多线程 Select 压力：workers=%d ops=%lld sent=%lld "
        "received=%lld drained=%lld\n",
        workers, per_worker * workers, total_sent, total_received, drained);
    if (total_failed != 0) {
        std::fprintf(stderr, "[FAIL] T4b 出现非法状态 %lld 次\n", total_failed);
        return false;
    }
    if (total_sent != total_received + drained) {
        std::fprintf(stderr,
                     "[FAIL] T4b 丢值：sent=%lld != received=%lld + drained=%lld\n",
                     total_sent, total_received, drained);
        return false;
    }
    return Check(true, "T4b 多线程随机 Select 不丢值");
}

// ---------------------------------------------------------------------------
// S1：优雅关闭 —— 多生产者关闭后 consumer 用 Select 排空，值一个不少
// ---------------------------------------------------------------------------

bool ScenarioGracefulClose() {
    constexpr int kProducers = 3;
    constexpr int kPerProducer = 3000;
    std::vector<ChannelPtr<int>> channels;
    for (int i = 0; i < kProducers; ++i) {
        channels.push_back(Channel<int>::Create(4));
    }
    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([p, channels] {
            for (int i = 0; i < kPerProducer; ++i) {
                for (;;) {
                    const auto result = channels[p]->TrySend(i);
                    if (result.Ok()) {
                        break;
                    }
                    // 通道已被关闭（不应发生：关闭只由本线程在发送完成后
                    // 发起）；直接放弃剩余值，避免测试自身忙等卡死。
                    if (result.Closed()) {
                        (void)channels[p]->Close();
                        return;
                    }
                    std::this_thread::yield();
                }
            }
            (void)channels[p]->Close();
        });
    }
    std::vector<SelectCase> cases;
    for (const auto& channel : channels) {
        cases.push_back(RecvCase(channel));
    }
    long long received = 0;
    // 以「所有通道都已关闭」为退出条件：旧实现可能因 closed 遮蔽值而提前
    // 返回 closed，这里用 IsClosed 而不是 closed 计数，避免测试自身死锁。
    for (;;) {
        bool all_closed = true;
        for (const auto& channel : channels) {
            all_closed = all_closed && channel->IsClosed();
        }
        if (all_closed) {
            break;
        }
        const auto result =
            Select(cases, {}, std::optional<ContextDuration>(50ms));
        if (result.selected && result.status == ChannelStatus::kReady &&
            result.ok) {
            ++received;
            continue;
        }
        if (result.status == ChannelStatus::kClosed) {
            continue;
        }
        // 超时：生产者还没发完/关闭，继续等。
        if (result.status == ChannelStatus::kTimedOut) {
            continue;
        }
        std::fprintf(stderr, "[FAIL] S1 意外结果 status=%d received=%lld\n",
                     static_cast<int>(result.status), received);
        for (auto& producer : producers) {
            producer.join();
        }
        return false;
    }
    for (auto& producer : producers) {
        producer.join();
    }
    // 关闭后再排空一次：若 closed 返回时还有别的通道的值在缓冲里，说明
    // 有值优先失效；这里同时兜底统计，确保值一个不少。
    for (const auto& channel : channels) {
        for (;;) {
            const auto result = channel->TryRecv();
            if (!result.Ok()) {
                break;
            }
            ++received;
        }
    }
    const long long expected = static_cast<long long>(kProducers) * kPerProducer;
    if (received != expected) {
        std::fprintf(stderr, "[FAIL] S1 丢值 received=%lld/%lld\n", received,
                     expected);
        return false;
    }
    return Check(true, "S1 优雅关闭：多生产者值全部收到");
}

// ---------------------------------------------------------------------------
// S2：关闭 + 取消 + 值竞争 —— 返回结果必须合法，且不残留注册节点
// ---------------------------------------------------------------------------

bool ScenarioCloseCancelRace() {
    constexpr int kRounds = 500;
    for (int round = 0; round < kRounds; ++round) {
        auto channel = Channel<int>::Create(1);
        auto pair = Context::WithCancel(Context::Background());
        std::atomic<int> done{0};
        std::thread first([&] {
            const auto result =
                Select({RecvCase(channel), DoneCase(pair.first)}, pair.first,
                       std::optional<ContextDuration>(100ms));
            if (!result.selected) {
                std::fprintf(stderr, "[FAIL] S2 round=%d 未选中 status=%d\n",
                             round, static_cast<int>(result.status));
                return;
            }
            done.fetch_add(1, std::memory_order_relaxed);
        });
        std::thread second([&] {
            (void)channel->TrySend(round);
            (void)channel->Close();
            pair.second();
        });
        first.join();
        second.join();
        if (done.load(std::memory_order_relaxed) != 1) {
            return false;
        }
        const auto snapshot = channel->DebugSnapshot();
        if (snapshot.select_receivers != 0 || snapshot.select_senders != 0) {
            std::fprintf(stderr,
                         "[FAIL] S2 round=%d 残留 select 注册节点 recv=%zu "
                         "send=%zu\n",
                         round, snapshot.select_receivers,
                         snapshot.select_senders);
            return false;
        }
    }
    return Check(true, "S2 关闭/取消/有值三路竞争：结果合法且无残留");
}

// ---------------------------------------------------------------------------
// S3：default 轮询 —— 有值取值、无值走 default、关闭返回 closed
// ---------------------------------------------------------------------------

bool ScenarioDefaultPolling() {
    constexpr int kRounds = 5000;
    auto channel = Channel<int>::Create(1);
    int received = 0;
    int default_hits = 0;
    for (int i = 0; i < kRounds; ++i) {
        if ((i % 3) == 0 && channel->TrySend(i).Ok()) {
            // 有值时 Select 必须在 default 之前命中 recv。
            const auto result = Select({RecvCase(channel), DefaultCase()});
            if (!(result.selected && result.index == 0 && result.ok)) {
                std::fprintf(stderr,
                             "[FAIL] S3 i=%d 有值却走了 default index=%zu\n", i,
                             result.index);
                return false;
            }
            ++received;
            continue;
        }
        const auto result = Select({RecvCase(channel), DefaultCase()});
        if (!result.selected) {
            std::fprintf(stderr, "[FAIL] S3 i=%d 未选中\n", i);
            return false;
        }
        if (result.index == 1) {
            ++default_hits;
        } else {
            ++received;
        }
    }
    (void)channel->Close();
    const auto closed = Select({RecvCase(channel), DefaultCase()});
    if (!(closed.selected && closed.index == 0 &&
          closed.status == ChannelStatus::kClosed)) {
        std::fprintf(stderr,
                     "[FAIL] S3 关闭后未返回 closed index=%zu status=%d\n",
                     closed.index, static_cast<int>(closed.status));
        return false;
    }
    std::printf("[info] S3 received=%d default_hits=%d\n", received,
                default_hits);
    return Check(true, "S3 default 轮询：有值优先于 default，关闭返回 closed");
}

// ---------------------------------------------------------------------------
// S4：双向 ping-pong —— 两个 select 通道互相配对，有值优先不破坏配对
// ---------------------------------------------------------------------------

bool ScenarioPingPong() {
    constexpr int kMessages = 20000;
    auto first = Channel<int>::Create(0);
    auto second = Channel<int>::Create(0);
    std::atomic<int> completed{0};
    auto worker = [&](bool reversed) {
        for (int i = 0; i < kMessages; ++i) {
            std::vector<SelectCase> cases;
            if (!reversed) {
                cases.push_back(RecvCase(first));
                cases.push_back(SendCase(second, i));
            } else {
                cases.push_back(RecvCase(second));
                cases.push_back(SendCase(first, i));
            }
            const auto result =
                Select(cases, {}, std::optional<ContextDuration>(1s));
            if (!result.selected || result.status != ChannelStatus::kReady) {
                std::fprintf(stderr, "[FAIL] S4 ping-pong 状态=%d\n",
                             static_cast<int>(result.status));
                return;
            }
            completed.fetch_add(1, std::memory_order_relaxed);
        }
    };
    std::thread first_worker(worker, false);
    std::thread second_worker(worker, true);
    first_worker.join();
    second_worker.join();
    (void)first->Close();
    (void)second->Close();
    if (completed.load(std::memory_order_relaxed) != kMessages * 2) {
        std::fprintf(stderr, "[FAIL] S4 完成数=%d/%d\n",
                     completed.load(std::memory_order_relaxed), kMessages * 2);
        return false;
    }
    return Check(true, "S4 双向 ping-pong：有值优先不破坏 select 配对");
}

// ---------------------------------------------------------------------------
// S5：超时与关闭竞争 —— 超时返回时值不得丢失，关闭后必须排空
// ---------------------------------------------------------------------------

bool ScenarioTimeoutVersusClose() {
    constexpr int kRounds = 1000;
    for (int round = 0; round < kRounds; ++round) {
        auto channel = Channel<int>::Create(1);
        std::thread producer([&] {
            std::this_thread::sleep_for(std::chrono::microseconds(round % 800));
            (void)channel->TrySend(round);
            std::this_thread::sleep_for(std::chrono::microseconds(100));
            (void)channel->Close();
        });
        const auto result =
            Select({RecvCase(channel), AfterCase(std::chrono::milliseconds(1))},
                   {}, std::optional<ContextDuration>(100ms));
        producer.join();
        if (!result.selected) {
            std::fprintf(stderr, "[FAIL] S5 round=%d 未选中 status=%d\n", round,
                         static_cast<int>(result.status));
            return false;
        }
        if (result.index == 0 && result.status == ChannelStatus::kReady) {
            if (result.Value<int>().value_or(-1) != round) {
                std::fprintf(stderr, "[FAIL] S5 round=%d 值不匹配\n", round);
                return false;
            }
            continue;
        }
        if (result.index == 1) {
            // 超时/定时器获胜：此时值可能已经在缓冲里，必须确认没有丢。
            long long residual = 0;
            for (;;) {
                const auto drained = channel->TryRecv();
                if (!drained.Ok()) {
                    break;
                }
                if (drained.ValueOrDefault() != round) {
                    std::fprintf(stderr, "[FAIL] S5 round=%d 残留值错误\n",
                                 round);
                    return false;
                }
                ++residual;
            }
            if (residual != 1) {
                std::fprintf(
                    stderr, "[FAIL] S5 round=%d 超时返回后残留值=%lld（应为 1）\n",
                    round, residual);
                return false;
            }
            continue;
        }
        std::fprintf(stderr, "[FAIL] S5 round=%d 非法结果 index=%zu status=%d\n",
                     round, result.index, static_cast<int>(result.status));
        return false;
    }
    return Check(true, "S5 超时与关闭竞争：超时返回不丢值、关闭后能排空");
}

}  // namespace

int main(int argc, char** argv) {
    // stdout 可能是管道（ctest / tee），默认全缓冲会让长压测看不到进度；
    // 改为行缓冲，失败时能立刻定位到用例。
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    const long long stress_iters = argc > 1 ? std::atoll(argv[1]) : 10000000LL;
    std::thread watchdog(Watchdog, 600);
    watchdog.detach();

    bool ok = true;
    ok = TestClosedDoesNotShadowValue() && ok;
    ok = TestBothClosedDeliverValuesFirst() && ok;
    ok = TestConcurrentClosePrefersValue() && ok;
    ok = TestSingleThreadStress(stress_iters) && ok;
    ok = TestConcurrentStress(4, stress_iters / 25) && ok;
    ok = ScenarioGracefulClose() && ok;
    ok = ScenarioCloseCancelRace() && ok;
    ok = ScenarioDefaultPolling() && ok;
    ok = ScenarioPingPong() && ok;
    ok = ScenarioTimeoutVersusClose() && ok;

    g_finished.store(true, std::memory_order_release);
    std::printf("RESULT %s (failures=%d)\n",
                ok && g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return ok && g_failures == 0 ? 0 : 1;
}
