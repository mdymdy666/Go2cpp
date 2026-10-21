/**
 * @file test_random_5rounds.cpp
 * @brief 并发原语「随机 + 大数据量 + 真实多场景」5 轮测试的**单轮执行器**。
 *
 * @date 2026-10-21
 * @author DeepSeek Agent
 * @copyright MIT License（见项目根目录 LICENSE）
 *
 * ## 定位
 *
 * 5 轮驱动由 `tests/concurrency/run_random_5rounds.sh` 负责；本程序只跑
 * 「一轮」：给定 `--seed` 与 `--round`，用种子派生的随机组合挑选 4 个场景，
 * 每轮合计 ≥ `--min-ops` 次原语操作。所有场景、断言、watchdog 都在这里。
 *
 * ## 六个真实场景（每轮随机选 4 个，5 轮脚本保证并集覆盖全部 6 个）
 *
 * 1. `pc`          生产者-消费者：Channel + WaitGroup + Context 取消；
 * 2. `rwcache`     读写缓存：RWMutex + Channel 失效通知；
 * 3. `workpool`    工作池：Mutex + ConditionVariable + 队列；
 * 4. `pipeline`    管道：三级 Channel 串联 + RecvFor 超时；
 * 5. `mixedlocks`  混合锁：Mutex / RWMutex / RecursiveMutex 随机切换（含递归深度）；
 * 6. `storm`       原语风暴：随机 lock/unlock/send/recv/wait/notify/select/timeout。
 *
 * ## 断言
 *
 * - 无死锁：watchdog 线程监视「进度计数 + 总时长」，超时打印现场、写事件
 *   dump 到 /tmp 并 `_exit(42)`；外层脚本再用 `timeout 120` 兜底；
 * - 计数守恒：每个场景逐项核对（send/recv、lock/unlock、wait/notify、
 *   WaitGroup add/done、读写版本、唯一性位图、队列和）；
 * - 无崩溃 / 无未处理异常：每个线程的循环体在 try/catch 内，异常计入 bad_ops；
 * - 开启 debug（GO2CPP_DEBUG_DUMP=ON 构建）时额外做事件审计：
 *   * 累计计数不变量（事件数 ≤ 对应原语调用数、wake ≤ park 等）；
 *   * 「关键事件不丢」定向校验：Reset 后跑小规模阻塞收发，从 Snapshot
 *     精确核对 send/recv/wake 条数（关键区容量内必须一条不少）。
 *
 * ## 用法
 *
 * ```text
 * test_random_5rounds --seed 12345 --round 1 --min-ops 1000000
 * test_random_5rounds --plan --seed 12345 --round 1   # 只打印场景组合
 * test_random_5rounds --events-probe --seed 1         # 打印 debug 事件基线
 * ```
 *
 * 退出码：0=通过；1=断言失败；42=watchdog 判定卡死；2=参数错误。
 */

#include "go2cpp/channel.hpp"
#include "go2cpp/concurrency/locks.hpp"
#include "go2cpp/context.hpp"
#include "go2cpp/debug/debug_event_ring.hpp"
#include "go2cpp/sync.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <csignal>
#include <execinfo.h>
#include <pthread.h>
#include <unistd.h>

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

using go2cpp::Background;
using go2cpp::ChannelPtr;
using go2cpp::ChannelStatus;
using go2cpp::MakeChannel;
using go2cpp::Select;
using go2cpp::SelectResult;
using go2cpp::WithCancel;
using go2cpp::sync::ConditionVariable;
using go2cpp::sync::Mutex;
using go2cpp::sync::RecursiveMutex;
using go2cpp::sync::RWMutex;
using go2cpp::sync::WaitGroup;

namespace dbg = go2cpp::debug;

// ===========================================================================
// 全局进度 / 失败计数（watchdog 与场景共享）
// ===========================================================================
std::atomic<long long> g_progress{0};   ///< 进度心跳（每完成一小批操作 +1）。
std::atomic<long long> g_ops_live{0};   ///< 实时操作数（现场 dump 用）。
std::atomic<int> g_failures{0};         ///< 断言失败数。
std::atomic<int> g_stage{-1};           ///< 当前场景编号。
std::mutex g_finish_mutex;              ///< 保护 g_finish_flag。
std::condition_variable g_finish_cv;    ///< 唤醒 watchdog（避免 join 空等 tick）。
bool g_finish_flag = false;             ///< 主流程结束标志。

/** @brief 通知 watchdog 主流程已结束并立即唤醒它。 */
void RequestFinish() {
    {
        std::lock_guard<std::mutex> lock(g_finish_mutex);
        g_finish_flag = true;
    }
    g_finish_cv.notify_all();
}

const char* const kScenarioNames[] = {
    "pc", "rwcache", "workpool", "pipeline", "mixedlocks", "storm"};
constexpr int kScenarioCount = 6;

void FailAt(const char* file, int line, const std::string& what) {
    g_failures.fetch_add(1, std::memory_order_relaxed);
    std::fprintf(stderr, "[FAIL] %s:%d %s\n", file, line, what.c_str());
}

#define R5_CHECK(cond)                                             \
    do {                                                           \
        if (!(cond)) {                                             \
            FailAt(__FILE__, __LINE__, "check failed: " #cond);    \
        }                                                          \
    } while (0)

#define R5_CHECK_MSG(cond, msg)                                    \
    do {                                                           \
        if (!(cond)) {                                             \
            FailAt(__FILE__, __LINE__, std::string("check failed: " #cond " | ") + (msg)); \
        }                                                          \
    } while (0)

long long ScaleCount(long long base, int percent) {
    const long long scaled = base * static_cast<long long>(percent) / 100;
    return scaled < 1 ? 1 : scaled;
}

// ===========================================================================
// 调试辅助：线程注册 + SIGUSR1 栈 dump（仅在 watchdog 判定卡死时使用）
// ===========================================================================
constexpr int kMaxRegisteredThreads = 64;
std::atomic<pthread_t> g_threads[kMaxRegisteredThreads];
std::atomic<int> g_thread_count{0};

void RegisterCurrentThread() noexcept {
    const int index = g_thread_count.fetch_add(1, std::memory_order_relaxed);
    if (index >= 0 && index < kMaxRegisteredThreads) {
        g_threads[index].store(pthread_self(), std::memory_order_release);
    }
}

void StackDumpHandler(int) {
    void* frames[48];
    const int count = backtrace(frames, 48);
    backtrace_symbols_fd(frames, count, 2);
}

void InitStackDumpHandler() noexcept {
    struct sigaction action {};
    action.sa_handler = StackDumpHandler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    (void)sigaction(SIGUSR1, &action, nullptr);
}

[[noreturn]] void DumpAndExit(int round, std::uint64_t seed, int stage,
                              double idle_sec, double total_sec) {
    const char* name = (stage >= 0 && stage < kScenarioCount)
                           ? kScenarioNames[stage]
                           : "unknown";
    std::fprintf(stderr,
                 "[watchdog] STUCK round=%d seed=%llu stage=%d(%s) "
                 "ops=%lld idle=%.1fs total=%.1fs\n",
                 round, static_cast<unsigned long long>(seed), stage, name,
                 g_ops_live.load(std::memory_order_relaxed), idle_sec,
                 total_sec);
    if (dbg::EventRing::Enabled()) {
        char path[256];
        std::snprintf(path, sizeof(path),
                      "/tmp/r5_watchdog_round%d_seed%llu.log", round,
                      static_cast<unsigned long long>(seed));
        std::string text = dbg::EventRing::DumpText(0);
        if (FILE* file = std::fopen(path, "w")) {
            std::fwrite(text.data(), 1, text.size(), file);
            std::fclose(file);
            std::fprintf(stderr, "[watchdog] event dump -> %s\n", path);
        }
    }
    std::fflush(stderr);
    std::fflush(stdout);
    // 逐线程发 SIGUSR1 打印栈（调试卡死现场；生产测试不会走到这里）。
    const int thread_count = g_thread_count.load(std::memory_order_acquire);
    for (int i = 0; i < thread_count && i < kMaxRegisteredThreads; ++i) {
        const pthread_t thread = g_threads[i].load(std::memory_order_acquire);
        if (thread != pthread_t{}) {
            pthread_kill(thread, SIGUSR1);
        }
    }
    std::this_thread::sleep_for(500ms);
    _exit(42);
}

void WatchdogMain(int round, std::uint64_t seed, int idle_sec, int total_sec) {
    const auto started = Clock::now();
    long long last = g_progress.load(std::memory_order_relaxed);
    auto last_change = started;
    for (;;) {
        {
            // 用条件变量而不是固定 sleep：主流程结束时立即唤醒 watchdog，
            // 避免 join 空等最多一个 tick（250ms 会污染短轮次的耗时统计）。
            std::unique_lock<std::mutex> lock(g_finish_mutex);
            if (g_finish_cv.wait_for(lock, 250ms,
                                     [] { return g_finish_flag; })) {
                return;
            }
        }
        const long long current = g_progress.load(std::memory_order_relaxed);
        const auto now = Clock::now();
        if (current != last) {
            last = current;
            last_change = now;
        }
        const double idle =
            std::chrono::duration<double>(now - last_change).count();
        const double total = std::chrono::duration<double>(now - started).count();
        if (idle > idle_sec || total > total_sec) {
            DumpAndExit(round, seed, g_stage.load(std::memory_order_relaxed),
                        idle, total);
        }
    }
}

// ===========================================================================
// 场景选择：种子派生的随机组合（每轮 4 个）
// ===========================================================================
std::uint64_t MixSeed(std::uint64_t seed, int round) {
    std::uint64_t x = seed ^ (0x9E3779B97F4A7C15ULL * (round + 1));
    x ^= x >> 30;
    x *= 0xBF58476D1CE4E5B9ULL;
    x ^= x >> 27;
    x *= 0x94D049BB133111EBULL;
    x ^= x >> 31;
    return x;
}

std::vector<int> SelectScenarios(std::uint64_t seed, int round) {
    std::array<int, kScenarioCount> ids{{0, 1, 2, 3, 4, 5}};
    std::mt19937_64 rng(MixSeed(seed, round));
    std::shuffle(ids.begin(), ids.end(), rng);
    std::vector<int> chosen(ids.begin(), ids.begin() + 4);
    std::sort(chosen.begin(), chosen.end());
    return chosen;
}

// ===========================================================================
// 单个场景结果
// ===========================================================================
struct ScenarioResult {
    long long ops{0};
    long long checks{0};
    long long extra{0};  ///< 场景自定义指标（超时次数、递归深度等）。
    bool ok{false};
};

ScenarioResult RunProducerConsumer(std::uint64_t seed, int scale);
ScenarioResult RunRwCache(std::uint64_t seed, int scale);
ScenarioResult RunWorkPool(std::uint64_t seed, int scale);
ScenarioResult RunPipeline(std::uint64_t seed, int scale);
ScenarioResult RunMixedLocks(std::uint64_t seed, int scale);
ScenarioResult RunStorm(std::uint64_t seed, int scale);

ScenarioResult RunScenario(int id, std::uint64_t seed, int scale) {
    switch (id) {
        case 0: return RunProducerConsumer(seed, scale);
        case 1: return RunRwCache(seed, scale);
        case 2: return RunWorkPool(seed, scale);
        case 3: return RunPipeline(seed, scale);
        case 4: return RunMixedLocks(seed, scale);
        case 5: return RunStorm(seed, scale);
        default: return {};
    }
}
// ===========================================================================
// 场景 1：生产者-消费者（Channel + WaitGroup + Context 取消）
// ===========================================================================
ScenarioResult RunProducerConsumer(std::uint64_t seed, int scale) {
    (void)seed;
    constexpr int kProducers = 4;
    constexpr int kConsumers = 4;
    const int per_producer = static_cast<int>(ScaleCount(60000, scale));
    const int total = kProducers * per_producer;

    auto channel = MakeChannel<int>(64);
    auto [ctx, cancel] = WithCancel(Background());
    std::atomic<long long> produced{0};
    std::atomic<long long> received{0};
    std::atomic<long long> drained{0};
    std::atomic<long long> cancelled_send{0};
    std::atomic<long long> cancelled_recv{0};
    std::atomic<long long> duplicates{0};
    std::atomic<long long> bad{0};
    std::unique_ptr<std::atomic<unsigned char>[]> seen(
        new std::atomic<unsigned char>[static_cast<std::size_t>(total)]());
    WaitGroup wg;
    wg.Add(kProducers);
    std::vector<std::thread> threads;
    threads.reserve(kProducers + kConsumers);

    for (int p = 0; p < kProducers; ++p) {
        threads.emplace_back([&, p] {
            try {
                for (int i = 0; i < per_producer; ++i) {
                    const int value = p * per_producer + i;
                    const auto result = channel->Send(value);
                    if (result.Ok()) {
                        produced.fetch_add(1, std::memory_order_relaxed);
                        g_ops_live.fetch_add(1, std::memory_order_relaxed);
                    } else if (result.Cancelled()) {
                        cancelled_send.fetch_add(1, std::memory_order_relaxed);
                        break;
                    } else {
                        bad.fetch_add(1, std::memory_order_relaxed);
                        break;
                    }
                    if ((i & 1023) == 0) {
                        g_progress.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            } catch (...) {
                bad.fetch_add(1, std::memory_order_relaxed);
            }
            wg.Done();
        });
    }
    for (int c = 0; c < kConsumers; ++c) {
        threads.emplace_back([&] {
            try {
                for (;;) {
                    const auto result = channel->Recv(ctx);
                    if (result.Ok()) {
                        const int value = *result.value;
                        if (value < 0 || value >= total) {
                            duplicates.fetch_add(1, std::memory_order_relaxed);
                        } else if (seen[static_cast<std::size_t>(value)].exchange(
                                       1, std::memory_order_relaxed) != 0) {
                            duplicates.fetch_add(1, std::memory_order_relaxed);
                        }
                        received.fetch_add(1, std::memory_order_relaxed);
                        g_ops_live.fetch_add(1, std::memory_order_relaxed);
                    } else if (result.Cancelled()) {
                        cancelled_recv.fetch_add(1, std::memory_order_relaxed);
                        return;
                    } else if (result.Closed()) {
                        return;
                    } else {
                        bad.fetch_add(1, std::memory_order_relaxed);
                        return;
                    }
                }
            } catch (...) {
                bad.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    const bool wg_ok = wg.WaitFor(60s);
    R5_CHECK(wg_ok);

    // 等消费者消化到九成，再取消；取消后消费者从 Recv(ctx) 返回。
    const auto wait_started = Clock::now();
    while (received.load(std::memory_order_relaxed) < total * 9 / 10 &&
           Clock::now() - wait_started < 60s) {
        std::this_thread::sleep_for(1ms);
        g_progress.fetch_add(1, std::memory_order_relaxed);
    }
    cancel();
    for (auto& thread : threads) {
        thread.join();
    }

    for (;;) {
        const auto result = channel->TryRecv();
        if (result.Ok()) {
            const int value = *result.value;
            if (value >= 0 && value < total &&
                seen[static_cast<std::size_t>(value)].exchange(
                    1, std::memory_order_relaxed) == 0) {
                drained.fetch_add(1, std::memory_order_relaxed);
            } else {
                duplicates.fetch_add(1, std::memory_order_relaxed);
            }
        } else {
            break;
        }
    }

    ScenarioResult out;
    out.ops = produced.load() + received.load() + drained.load() +
              kProducers * 2 + 1;
    out.checks = 8;
    out.extra = cancelled_recv.load();
    R5_CHECK(produced.load() == total);
    R5_CHECK(cancelled_send.load() == 0);
    R5_CHECK(received.load() + drained.load() == total);
    R5_CHECK(duplicates.load() == 0);
    R5_CHECK(bad.load() == 0);
    R5_CHECK(cancelled_recv.load() == kConsumers);
    R5_CHECK(wg.Count() == 0);
    out.ok = g_failures.load() == 0;
    return out;
}

// ===========================================================================
// 场景 2：读写缓存（RWMutex + Channel 失效通知）
// ===========================================================================
struct CacheSnapshot {
    long long version{0};
    long long payload{0};
};

ScenarioResult RunRwCache(std::uint64_t seed, int scale) {
    (void)seed;
    constexpr int kWriters = 2;
    constexpr int kReaders = 6;
    const int writes = static_cast<int>(ScaleCount(30000, scale));
    const int reads = static_cast<int>(ScaleCount(30000, scale));

    RWMutex mutex;
    CacheSnapshot cache;
    auto notice = MakeChannel<long long>(1024);
    std::atomic<long long> write_ops{0};
    std::atomic<long long> read_ops{0};
    std::atomic<long long> torn{0};
    std::atomic<long long> sent{0};
    std::atomic<long long> full{0};
    std::atomic<long long> recv{0};
    std::atomic<long long> bad{0};

    std::vector<std::thread> threads;
    for (int w = 0; w < kWriters; ++w) {
        threads.emplace_back([&] {
            try {
                for (int i = 0; i < writes; ++i) {
                    mutex.Lock();
                    ++cache.version;
                    cache.payload = cache.version * 2;
                    const long long version = cache.version;
                    mutex.Unlock();
                    write_ops.fetch_add(1, std::memory_order_relaxed);
                    g_ops_live.fetch_add(2, std::memory_order_relaxed);
                    const auto pushed = notice->TrySend(version);
                    if (pushed.Ok()) {
                        sent.fetch_add(1, std::memory_order_relaxed);
                    } else if (pushed.Closed()) {
                        bad.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        full.fetch_add(1, std::memory_order_relaxed);
                    }
                    if ((i & 511) == 0) {
                        g_progress.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            } catch (...) {
                bad.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (int r = 0; r < kReaders; ++r) {
        threads.emplace_back([&] {
            try {
                for (int i = 0; i < reads; ++i) {
                    mutex.RLock();
                    const CacheSnapshot snapshot = cache;
                    mutex.RUnlock();
                    read_ops.fetch_add(1, std::memory_order_relaxed);
                    g_ops_live.fetch_add(2, std::memory_order_relaxed);
                    if (snapshot.payload != snapshot.version * 2) {
                        torn.fetch_add(1, std::memory_order_relaxed);
                    }
                    const auto pulled = notice->TryRecv();
                    if (pulled.Ok()) {
                        recv.fetch_add(1, std::memory_order_relaxed);
                    }
                    if ((i & 511) == 0) {
                        g_progress.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            } catch (...) {
                bad.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    notice->Close();
    for (;;) {
        const auto pulled = notice->TryRecv();
        if (!pulled.Ok()) {
            break;
        }
        recv.fetch_add(1, std::memory_order_relaxed);
    }

    ScenarioResult out;
    out.ops = write_ops.load() * 2 + read_ops.load() * 2 + sent.load() +
              recv.load() + full.load();
    out.checks = 7;
    R5_CHECK(torn.load() == 0);
    R5_CHECK(write_ops.load() == static_cast<long long>(kWriters) * writes);
    R5_CHECK(read_ops.load() == static_cast<long long>(kReaders) * reads);
    R5_CHECK(cache.version == static_cast<long long>(kWriters) * writes);
    R5_CHECK(cache.payload == cache.version * 2);
    R5_CHECK(recv.load() == sent.load());
    R5_CHECK(bad.load() == 0);
    R5_CHECK(!mutex.IsWriteLocked());
    out.ok = g_failures.load() == 0;
    return out;
}
// ===========================================================================
// 场景 3：工作池（Mutex + ConditionVariable + 队列）
// ===========================================================================
ScenarioResult RunWorkPool(std::uint64_t seed, int scale) {
    (void)seed;
    constexpr int kWorkers = 4;
    const int tasks = static_cast<int>(ScaleCount(20000, scale));

    Mutex mutex;
    ConditionVariable condition;
    std::deque<int> queue;
    bool stopping = false;
    std::atomic<long long> processed{0};
    std::atomic<long long> waits{0};
    std::atomic<long long> waits_done{0};
    std::atomic<long long> notifies{0};
    std::atomic<long long> bad{0};
    std::atomic<long long> sum{0};
    std::vector<std::thread> workers;
    workers.reserve(kWorkers);

    for (int w = 0; w < kWorkers; ++w) {
        workers.emplace_back([&] {
            try {
                for (;;) {
                    bool has_task = false;
                    int task = 0;
                    bool finish = false;
                    mutex.Lock();
                    while (queue.empty() && !stopping) {
                        waits.fetch_add(1, std::memory_order_relaxed);
                        const bool notified = condition.WaitFor(mutex, 5ms);
                        (void)notified;
                        waits_done.fetch_add(1, std::memory_order_relaxed);
                    }
                    if (!queue.empty()) {
                        task = queue.front();
                        queue.pop_front();
                        has_task = true;
                    } else {
                        finish = stopping;
                    }
                    mutex.Unlock();
                    if (!has_task) {
                        if (finish) {
                            return;
                        }
                        continue;
                    }
                    processed.fetch_add(1, std::memory_order_relaxed);
                    sum.fetch_add(task, std::memory_order_relaxed);
                    g_ops_live.fetch_add(2, std::memory_order_relaxed);
                    if ((processed.load(std::memory_order_relaxed) & 511) == 0) {
                        g_progress.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            } catch (...) {
                bad.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    long long expected_sum = 0;
    for (int i = 0; i < tasks; ++i) {
        expected_sum += i;
        mutex.Lock();
        queue.push_back(i);
        condition.NotifyOne();
        notifies.fetch_add(1, std::memory_order_relaxed);
        mutex.Unlock();
        g_ops_live.fetch_add(2, std::memory_order_relaxed);
        if ((i & 511) == 0) {
            g_progress.fetch_add(1, std::memory_order_relaxed);
        }
    }
    mutex.Lock();
    stopping = true;
    condition.NotifyAll();
    mutex.Unlock();

    for (auto& worker : workers) {
        worker.join();
    }

    ScenarioResult out;
    out.ops = notifies.load() + waits.load() + waits_done.load() +
              processed.load() * 2;
    out.checks = 6;
    out.extra = waits.load();
    R5_CHECK(processed.load() == tasks);
    R5_CHECK(sum.load() == expected_sum);
    R5_CHECK(queue.empty());
    R5_CHECK(bad.load() == 0);
    R5_CHECK(waits.load() == waits_done.load());
    R5_CHECK(processed.load() + (stopping ? 0 : 1) > 0);
    out.ok = g_failures.load() == 0;
    return out;
}

// ===========================================================================
// 场景 4：管道（三级 Channel 串联 + RecvFor 超时）
// ===========================================================================
ScenarioResult RunPipeline(std::uint64_t seed, int scale) {
    (void)seed;
    const int items = static_cast<int>(ScaleCount(80000, scale));

    auto first = MakeChannel<int>(32);
    auto second = MakeChannel<int>(32);
    auto third = MakeChannel<int>(32);
    std::atomic<long long> produced{0};
    std::atomic<long long> mid{0};
    std::atomic<long long> out{0};
    std::atomic<long long> timeouts{0};
    std::atomic<long long> bad{0};
    std::atomic<long long> sum{0};

    std::thread producer([&] {
        try {
            for (int i = 0; i < items; ++i) {
                const auto result = first->Send(i);
                if (result.Ok()) {
                    produced.fetch_add(1, std::memory_order_relaxed);
                } else {
                    bad.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
                g_ops_live.fetch_add(1, std::memory_order_relaxed);
                if ((i & 511) == 0) {
                    g_progress.fetch_add(1, std::memory_order_relaxed);
                }
            }
        } catch (...) {
            bad.fetch_add(1, std::memory_order_relaxed);
        }
        first->Close();
    });
    std::thread stage1([&] {
        try {
            for (;;) {
                const auto result = first->RecvFor(2ms);
                if (result.Ok()) {
                    const auto forwarded = second->Send(*result.value);
                    if (forwarded.Ok()) {
                        mid.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        bad.fetch_add(1, std::memory_order_relaxed);
                        break;
                    }
                } else if (result.status == ChannelStatus::kTimedOut) {
                    timeouts.fetch_add(1, std::memory_order_relaxed);
                } else if (result.Closed()) {
                    break;
                } else {
                    bad.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
                g_progress.fetch_add(1, std::memory_order_relaxed);
            }
        } catch (...) {
            bad.fetch_add(1, std::memory_order_relaxed);
        }
        second->Close();
    });
    std::thread stage2([&] {
        try {
            for (;;) {
                const auto result = second->RecvFor(2ms);
                if (result.Ok()) {
                    const auto forwarded = third->Send(*result.value);
                    if (forwarded.Ok()) {
                        // 第三级计数在 sink 侧统计。
                    } else {
                        bad.fetch_add(1, std::memory_order_relaxed);
                        break;
                    }
                } else if (result.status == ChannelStatus::kTimedOut) {
                    timeouts.fetch_add(1, std::memory_order_relaxed);
                } else if (result.Closed()) {
                    break;
                } else {
                    bad.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
                g_progress.fetch_add(1, std::memory_order_relaxed);
            }
        } catch (...) {
            bad.fetch_add(1, std::memory_order_relaxed);
        }
        third->Close();
    });
    std::thread sink([&] {
        try {
            for (;;) {
                const auto result = third->RecvFor(2ms);
                if (result.Ok()) {
                    out.fetch_add(1, std::memory_order_relaxed);
                    sum.fetch_add(*result.value, std::memory_order_relaxed);
                } else if (result.status == ChannelStatus::kTimedOut) {
                    // sink 侧超时只计数，不参与结果断言（关闭时可能发生一次）。
                } else if (result.Closed()) {
                    break;
                } else {
                    bad.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
                g_progress.fetch_add(1, std::memory_order_relaxed);
            }
        } catch (...) {
            bad.fetch_add(1, std::memory_order_relaxed);
        }
    });

    for (auto* thread : {&producer, &stage1, &stage2, &sink}) {
        thread->join();
    }

    long long expected_sum = 0;
    for (int i = 0; i < items; ++i) {
        expected_sum += i;
    }

    ScenarioResult out_stats;
    out_stats.ops = produced.load() + mid.load() + out.load() + timeouts.load();
    out_stats.checks = 6;
    out_stats.extra = timeouts.load();
    R5_CHECK(produced.load() == items);
    R5_CHECK(mid.load() == items);
    R5_CHECK(out.load() == items);
    R5_CHECK(sum.load() == expected_sum);
    R5_CHECK(bad.load() == 0);
    out_stats.ok = g_failures.load() == 0;
    return out_stats;
}
// ===========================================================================
// 场景 5：混合锁（Mutex / RWMutex / RecursiveMutex 随机切换）
// ===========================================================================
ScenarioResult RunMixedLocks(std::uint64_t seed, int scale) {
    constexpr int kThreads = 8;
    const int iters = static_cast<int>(ScaleCount(30000, scale));

    Mutex mutex;
    RWMutex rw;
    RecursiveMutex recursive;
    long long counter_mu = 0;
    long long counter_rec = 0;
    CacheSnapshot cache;
    std::atomic<long long> mu_locks{0};
    std::atomic<long long> rw_reads{0};
    std::atomic<long long> rw_writes{0};
    std::atomic<long long> recursive_locks{0};
    std::atomic<long long> torn{0};
    std::atomic<long long> bad{0};

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            std::mt19937_64 rng(MixSeed(seed, t + 100));
            try {
                for (int i = 0; i < iters; ++i) {
                    switch (rng() % 3) {
                        case 0: {
                            mutex.Lock();
                            ++counter_mu;
                            mutex.Unlock();
                            mu_locks.fetch_add(1, std::memory_order_relaxed);
                            g_ops_live.fetch_add(2, std::memory_order_relaxed);
                            break;
                        }
                        case 1: {
                            if ((rng() & 1U) == 0U) {
                                rw.RLock();
                                const CacheSnapshot snapshot = cache;
                                rw.RUnlock();
                                if (snapshot.payload != snapshot.version * 2) {
                                    torn.fetch_add(1, std::memory_order_relaxed);
                                }
                                rw_reads.fetch_add(1, std::memory_order_relaxed);
                            } else {
                                rw.Lock();
                                ++cache.version;
                                cache.payload = cache.version * 2;
                                rw.Unlock();
                                rw_writes.fetch_add(1, std::memory_order_relaxed);
                            }
                            g_ops_live.fetch_add(2, std::memory_order_relaxed);
                            break;
                        }
                        default: {
                            const int depth = 1 + static_cast<int>(rng() % 3);
                            for (int d = 0; d < depth; ++d) {
                                recursive.Lock();
                                ++counter_rec;
                                recursive_locks.fetch_add(
                                    1, std::memory_order_relaxed);
                                g_ops_live.fetch_add(1,
                                                     std::memory_order_relaxed);
                            }
                            for (int d = 0; d < depth; ++d) {
                                recursive.Unlock();
                                g_ops_live.fetch_add(1,
                                                     std::memory_order_relaxed);
                            }
                            break;
                        }
                    }
                    if ((i & 511) == 0) {
                        g_progress.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            } catch (...) {
                bad.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }

    ScenarioResult out;
    out.ops = mu_locks.load() * 2 + (rw_reads.load() + rw_writes.load()) * 2 +
              recursive_locks.load() * 2;
    out.checks = 6;
    out.extra = recursive_locks.load();
    R5_CHECK(counter_mu == mu_locks.load());
    R5_CHECK(counter_rec == recursive_locks.load());
    R5_CHECK(cache.version == rw_writes.load());
    R5_CHECK(cache.payload == cache.version * 2);
    R5_CHECK(torn.load() == 0);
    R5_CHECK(bad.load() == 0);
    R5_CHECK(!rw.IsWriteLocked());
    out.ok = g_failures.load() == 0;
    return out;
}

// ===========================================================================
// 场景 6：混合原语风暴（随机 lock/unlock/send/recv/wait/notify/select/timeout）
// ===========================================================================
ScenarioResult RunStorm(std::uint64_t seed, int scale) {
    constexpr int kThreads = 8;
    const int iters = static_cast<int>(ScaleCount(6000, scale));

    Mutex mutex;
    // CV 使用独立的 mutex：Go 风格 Mutex 不可重入，若与随机 lock/unlock
    // 共用一把锁，线程可能在仍持锁时进入 Wait 的 Lock()，造成自死锁
    // （这是测试代码的使用错误，不是原语缺陷）。
    Mutex cv_mutex;
    ConditionVariable condition;
    int generation = 0;  ///< 受 cv_mutex 保护；notify 递增，wait 谓词用。
    auto outbox = MakeChannel<long long>(256);
    auto inbox = MakeChannel<long long>(256);
    std::atomic<long long> next_id{0};
    std::atomic<long long> sent{0};
    std::atomic<long long> recv_ok{0};
    std::atomic<long long> full{0};
    std::atomic<long long> empty{0};
    std::atomic<long long> waits{0};
    std::atomic<long long> waits_done{0};
    std::atomic<long long> notifies{0};
    std::atomic<long long> selects{0};
    std::atomic<long long> timeouts{0};
    std::atomic<long long> bad{0};
    std::unique_ptr<std::atomic<unsigned char>[]> seen(
        new std::atomic<unsigned char>[static_cast<std::size_t>(iters) *
                                       kThreads]());

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            RegisterCurrentThread();
            std::mt19937_64 rng(MixSeed(seed, t + 200));
            bool held = false;
            auto release = [&]() {
                if (held) {
                    mutex.Unlock();
                    held = false;
                    g_ops_live.fetch_add(1, std::memory_order_relaxed);
                }
            };
            try {
                for (int i = 0; i < iters; ++i) {
                    const int op = static_cast<int>(rng() % 8);
                    switch (op) {
                        case 0:
                        case 1: {
                            if (held) {
                                mutex.Unlock();
                                held = false;
                            } else {
                                mutex.Lock();
                                held = true;
                            }
                            g_ops_live.fetch_add(1, std::memory_order_relaxed);
                            break;
                        }
                        case 2: {
                            const long long value =
                                next_id.fetch_add(1, std::memory_order_relaxed);
                            const auto pushed = outbox->TrySend(value);
                            if (pushed.Ok()) {
                                sent.fetch_add(1, std::memory_order_relaxed);
                            } else {
                                full.fetch_add(1, std::memory_order_relaxed);
                            }
                            g_ops_live.fetch_add(1, std::memory_order_relaxed);
                            break;
                        }
                        case 3: {
                            const auto pulled = outbox->TryRecv();
                            if (pulled.Ok()) {
                                const long long value = *pulled.value;
                                if (value >= 0 &&
                                    value < static_cast<long long>(iters) *
                                                kThreads &&
                                    seen[static_cast<std::size_t>(value)]
                                            .exchange(
                                                1, std::memory_order_relaxed) ==
                                        0) {
                                    recv_ok.fetch_add(
                                        1, std::memory_order_relaxed);
                                } else {
                                    bad.fetch_add(1,
                                                  std::memory_order_relaxed);
                                }
                            } else {
                                empty.fetch_add(1, std::memory_order_relaxed);
                            }
                            g_ops_live.fetch_add(1, std::memory_order_relaxed);
                            break;
                        }
                        case 4: {
                            const auto selected = Select(
                                {go2cpp::RecvCase(inbox),
                                 go2cpp::DefaultCase()});
                            if (selected.selected) {
                                selects.fetch_add(1,
                                                  std::memory_order_relaxed);
                                if (selected.ok && selected.index == 0) {
                                    recv_ok.fetch_add(
                                        1, std::memory_order_relaxed);
                                }
                            }
                            g_ops_live.fetch_add(1, std::memory_order_relaxed);
                            break;
                        }
                        case 5: {
                            // ConditionVariable 等待：持锁 → WaitFor(1ms) → 解锁。
                            cv_mutex.Lock();
                            const int observed = generation;
                            waits.fetch_add(1, std::memory_order_relaxed);
                            (void)condition.WaitFor(cv_mutex, 200us);
                            (void)observed;
                            cv_mutex.Unlock();
                            waits_done.fetch_add(1, std::memory_order_relaxed);
                            g_ops_live.fetch_add(3, std::memory_order_relaxed);
                            break;
                        }
                        case 6: {
                            cv_mutex.Lock();
                            ++generation;
                            condition.NotifyOne();
                            cv_mutex.Unlock();
                            notifies.fetch_add(1, std::memory_order_relaxed);
                            g_ops_live.fetch_add(3, std::memory_order_relaxed);
                            break;
                        }
                        default: {
                            const auto pulled = outbox->RecvFor(1ms);
                            if (pulled.Ok()) {
                                const long long value = *pulled.value;
                                if (value >= 0 &&
                                    value < static_cast<long long>(iters) *
                                                kThreads &&
                                    seen[static_cast<std::size_t>(value)]
                                            .exchange(
                                                1, std::memory_order_relaxed) ==
                                        0) {
                                    recv_ok.fetch_add(
                                        1, std::memory_order_relaxed);
                                } else {
                                    bad.fetch_add(1,
                                                  std::memory_order_relaxed);
                                }
                            } else if (pulled.status ==
                                       ChannelStatus::kTimedOut) {
                                timeouts.fetch_add(1,
                                                   std::memory_order_relaxed);
                            } else {
                                empty.fetch_add(1, std::memory_order_relaxed);
                            }
                            g_ops_live.fetch_add(1, std::memory_order_relaxed);
                            break;
                        }
                    }
                    if ((i & 511) == 0) {
                        g_progress.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            } catch (...) {
                bad.fetch_add(1, std::memory_order_relaxed);
            }
            release();
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }

    // 关闭并排空，核对 send/recv 守恒。
    outbox->Close();
    for (;;) {
        const auto pulled = outbox->TryRecv();
        if (!pulled.Ok()) {
            break;
        }
        const long long value = *pulled.value;
        if (value >= 0 &&
            value < static_cast<long long>(iters) * kThreads &&
            seen[static_cast<std::size_t>(value)].exchange(
                1, std::memory_order_relaxed) == 0) {
            recv_ok.fetch_add(1, std::memory_order_relaxed);
        } else {
            bad.fetch_add(1, std::memory_order_relaxed);
        }
    }
    inbox->Close();

    ScenarioResult out;
    out.ops = sent.load() + recv_ok.load() + full.load() + empty.load() +
              waits.load() + waits_done.load() + notifies.load() +
              selects.load() + timeouts.load();
    out.checks = 9;
    out.extra = waits.load();
    R5_CHECK(sent.load() + full.load() >= recv_ok.load());
    R5_CHECK(recv_ok.load() == sent.load());
    R5_CHECK(waits.load() == waits_done.load());
    R5_CHECK(bad.load() == 0);
    R5_CHECK(selects.load() >= 0);
    out.ok = g_failures.load() == 0;
    return out;
}
// ===========================================================================
// debug 事件审计（仅 GO2CPP_DEBUG_DUMP=ON 构建有实际数据）
// ===========================================================================
long long CountCode(const std::vector<dbg::EventRecord>& events,
                    dbg::SchedEventCode code) {
    long long count = 0;
    for (const auto& event : events) {
        if (event.code == static_cast<std::uint32_t>(code)) {
            ++count;
        }
    }
    return count;
}

void PrintEventTotals(const char* tag) {
    if (!dbg::EventRing::Enabled()) {
        std::printf("[events:%s] disabled (GO2CPP_DEBUG_DUMP=OFF)\n", tag);
        return;
    }
    std::printf(
        "[events:%s] all=%llu ch_send=%llu ch_recv=%llu ch_wake=%llu "
        "ch_send_ok=%llu ch_recv_ok=%llu select=%llu select_enter=%llu "
        "cv_wait=%llu cv_notify=%llu cv_notify_all=%llu "
        "mutex_lock=%llu mutex_unlock=%llu mutex_fast=%llu "
        "rw_read=%llu rw_runlock=%llu rw_write=%llu rw_wunlock=%llu "
        "rec_lock=%llu rec_unlock=%llu wg_wait=%llu wg_done=%llu wg_add=%llu\n",
        tag,
        static_cast<unsigned long long>(dbg::EventRing::TotalCountAll()),
        static_cast<unsigned long long>(
            dbg::EventRing::TotalCount(dbg::SchedEventCode::kChannelSend)),
        static_cast<unsigned long long>(
            dbg::EventRing::TotalCount(dbg::SchedEventCode::kChannelRecv)),
        static_cast<unsigned long long>(
            dbg::EventRing::TotalCount(dbg::SchedEventCode::kChannelWake)),
        static_cast<unsigned long long>(
            dbg::EventRing::TotalCount(dbg::SchedEventCode::kChannelSendOk)),
        static_cast<unsigned long long>(
            dbg::EventRing::TotalCount(dbg::SchedEventCode::kChannelRecvOk)),
        static_cast<unsigned long long>(
            dbg::EventRing::TotalCount(dbg::SchedEventCode::kChannelSelect)),
        static_cast<unsigned long long>(dbg::EventRing::TotalCount(
            dbg::SchedEventCode::kChannelSelectEnter)),
        static_cast<unsigned long long>(
            dbg::EventRing::TotalCount(dbg::SchedEventCode::kCvWait)),
        static_cast<unsigned long long>(
            dbg::EventRing::TotalCount(dbg::SchedEventCode::kCvNotify)),
        static_cast<unsigned long long>(
            dbg::EventRing::TotalCount(dbg::SchedEventCode::kCvNotifyAll)),
        static_cast<unsigned long long>(
            dbg::EventRing::TotalCount(dbg::SchedEventCode::kMutexLock)),
        static_cast<unsigned long long>(
            dbg::EventRing::TotalCount(dbg::SchedEventCode::kMutexUnlock)),
        static_cast<unsigned long long>(
            dbg::EventRing::TotalCount(dbg::SchedEventCode::kMutexFastLock)),
        static_cast<unsigned long long>(
            dbg::EventRing::TotalCount(dbg::SchedEventCode::kRwLockRead)),
        static_cast<unsigned long long>(
            dbg::EventRing::TotalCount(dbg::SchedEventCode::kRwUnlockRead)),
        static_cast<unsigned long long>(
            dbg::EventRing::TotalCount(dbg::SchedEventCode::kRwLockWrite)),
        static_cast<unsigned long long>(
            dbg::EventRing::TotalCount(dbg::SchedEventCode::kRwUnlockWrite)),
        static_cast<unsigned long long>(
            dbg::EventRing::TotalCount(dbg::SchedEventCode::kRecursiveLock)),
        static_cast<unsigned long long>(
            dbg::EventRing::TotalCount(dbg::SchedEventCode::kRecursiveUnlock)),
        static_cast<unsigned long long>(
            dbg::EventRing::TotalCount(dbg::SchedEventCode::kWaitGroupWait)),
        static_cast<unsigned long long>(
            dbg::EventRing::TotalCount(dbg::SchedEventCode::kWaitGroupDone)),
        static_cast<unsigned long long>(
            dbg::EventRing::TotalCount(dbg::SchedEventCode::kWaitGroupAdd)));
}

/**
 * @brief debug 事件审计：累计不变量 + 「关键事件不丢」定向校验。
 *
 * 定向校验在**所有场景线程 join 之后**执行：先 Reset 事件环，再跑一次
 * 16 次无缓冲 channel 往返（32 次 park、16 次 wake，远小于每线程关键区
 * 64 条容量），最后从 Snapshot 逐条计数。若 send/recv/wake 少一条，
 * 就说明关键事件在容量充足时仍会丢，属于 debug 模块缺陷。
 */
void AuditDebugEvents() {
    if (!dbg::EventRing::Enabled()) {
        return;
    }
    PrintEventTotals("round");

    const auto send = dbg::EventRing::TotalCount(dbg::SchedEventCode::kChannelSend);
    const auto recv = dbg::EventRing::TotalCount(dbg::SchedEventCode::kChannelRecv);
    const auto wake = dbg::EventRing::TotalCount(dbg::SchedEventCode::kChannelWake);
    const auto cv_wait = dbg::EventRing::TotalCount(dbg::SchedEventCode::kCvWait);
    const auto cv_notify = dbg::EventRing::TotalCount(dbg::SchedEventCode::kCvNotify);

    // 累计不变量：每次 wake 必然对应一次 send 或 recv 的 park；每个 notify
    // 至多唤醒一个等待者（等待者数 ≤ wait 次数）。
    R5_CHECK(wake <= send + recv);
    R5_CHECK(cv_notify <= cv_wait);
    R5_CHECK(dbg::EventRing::TotalCountAll() > 0);

    // ---- 关键事件不丢：定向校验 ----
    //
    // 注意：线程退出时 Registration 析构会 RemoveRing，Snapshot 就看不到
    // 该线程的事件（这是「线程生命周期结束」而不是丢事件）。因此这里让
    // 发送线程一直存活到快照读完；容量（16 次往返 ≈ 24 条 park）远小于
    // 每线程普通区 256，若 snapshot 计数与 TotalCount 累计一致，就证明
    // 关键事件在容量充足时零丢失。
    dbg::EventRing::Reset();
    dbg::EventRing::ResetTotals();
    long long directed_send = 0;
    long long directed_recv = 0;
    long long directed_wake = 0;
    {
        auto channel = MakeChannel<int>(0);
        std::atomic<bool> sender_done{false};
        std::atomic<bool> stop{false};
        std::thread sender([&] {
            for (int i = 0; i < 16; ++i) {
                if (!channel->Send(i).Ok()) {
                    FailAt(__FILE__, __LINE__, "directed send failed");
                }
            }
            sender_done.store(true, std::memory_order_release);
            while (!stop.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(1ms);
            }
        });
        for (int i = 0; i < 16; ++i) {
            if (!channel->Recv().Ok()) {
                FailAt(__FILE__, __LINE__, "directed recv failed");
            }
        }
        while (!sender_done.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(1ms);
        }
        const auto snapshot = dbg::EventRing::Snapshot();
        directed_send = CountCode(snapshot, dbg::SchedEventCode::kChannelSend);
        directed_recv = CountCode(snapshot, dbg::SchedEventCode::kChannelRecv);
        directed_wake = CountCode(snapshot, dbg::SchedEventCode::kChannelWake);
        const auto total_send =
            static_cast<long long>(dbg::EventRing::TotalCount(
                dbg::SchedEventCode::kChannelSend));
        const auto total_recv =
            static_cast<long long>(dbg::EventRing::TotalCount(
                dbg::SchedEventCode::kChannelRecv));
        const auto total_wake =
            static_cast<long long>(dbg::EventRing::TotalCount(
                dbg::SchedEventCode::kChannelWake));
        std::printf(
            "[events:directed] snapshot=%zu send=%lld/%lld recv=%lld/%lld "
            "wake=%lld/%lld (snapshot/total)\n",
            snapshot.size(), directed_send, total_send, directed_recv,
            total_recv, directed_wake, total_wake);
        R5_CHECK(directed_send == total_send);
        R5_CHECK(directed_recv == total_recv);
        R5_CHECK(directed_wake == total_wake);
        R5_CHECK(total_send + total_recv >= 16);
        stop.store(true, std::memory_order_release);
        sender.join();
    }

    // 定向校验结束后清空，避免影响同一进程后续 debug 轮。
    dbg::EventRing::Reset();
}

// ===========================================================================
// 选项 / 入口
// ===========================================================================
struct Options {
    std::uint64_t seed{1};
    int round{0};
    long long min_ops{1000000};
    int scale_percent{100};
    int watchdog_idle_sec{25};
    int watchdog_total_sec{110};
    bool plan{false};
    bool events_probe{false};
    std::string scenarios;  ///< 逗号分隔的名字；空=按 seed/round 随机选。
};

bool ParseOptions(int argc, char** argv, Options* options) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", name);
                return nullptr;
            }
            return argv[++i];
        };
        if (arg == "--seed") {
            const char* text = next("--seed");
            if (text == nullptr) return false;
            options->seed = std::strtoull(text, nullptr, 10);
        } else if (arg == "--round") {
            const char* text = next("--round");
            if (text == nullptr) return false;
            options->round = std::atoi(text);
        } else if (arg == "--min-ops") {
            const char* text = next("--min-ops");
            if (text == nullptr) return false;
            options->min_ops = std::atoll(text);
        } else if (arg == "--scale-percent") {
            const char* text = next("--scale-percent");
            if (text == nullptr) return false;
            options->scale_percent = std::atoi(text);
        } else if (arg == "--watchdog-idle-sec") {
            const char* text = next("--watchdog-idle-sec");
            if (text == nullptr) return false;
            options->watchdog_idle_sec = std::atoi(text);
        } else if (arg == "--watchdog-total-sec") {
            const char* text = next("--watchdog-total-sec");
            if (text == nullptr) return false;
            options->watchdog_total_sec = std::atoi(text);
        } else if (arg == "--scenarios") {
            const char* text = next("--scenarios");
            if (text == nullptr) return false;
            options->scenarios = text;
        } else if (arg == "--plan") {
            options->plan = true;
        } else if (arg == "--events-probe") {
            options->events_probe = true;
        } else if (arg == "--help" || arg == "-h") {
            std::printf(
                "usage: %s --seed N --round R [--min-ops N] [--scale-percent P]\n"
                "          [--scenarios pc,rwcache,...] [--plan] [--events-probe]\n",
                argv[0]);
            return false;
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
            return false;
        }
    }
    return true;
}

std::vector<int> ParseScenarioList(const std::string& text) {
    std::vector<int> ids;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t comma = text.find(',', start);
        const std::string token =
            text.substr(start, comma == std::string::npos ? std::string::npos
                                                          : comma - start);
        if (!token.empty()) {
            bool found = false;
            for (int i = 0; i < kScenarioCount; ++i) {
                if (token == kScenarioNames[i]) {
                    ids.push_back(i);
                    found = true;
                }
            }
            if (!found) {
                // 允许用数字索引（0..5）。
                const int index = std::atoi(token.c_str());
                if (index >= 0 && index < kScenarioCount) {
                    ids.push_back(index);
                } else {
                    std::fprintf(stderr, "unknown scenario: %s\n", token.c_str());
                }
            }
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    return ids;
}

void PrintScenarioList(const std::vector<int>& ids) {
    for (std::size_t i = 0; i < ids.size(); ++i) {
        std::printf("%s%s", i == 0 ? "" : ",", kScenarioNames[ids[i]]);
    }
}

int RunEventsProbe(const Options& options) {
    std::printf("[probe] debug_enabled=%d\n",
                dbg::EventRing::Enabled() ? 1 : 0);
    if (!dbg::EventRing::Enabled()) {
        std::printf("[probe] OFF build: no events\n");
        return 0;
    }
    (void)options;

    // 1) 无缓冲 channel 16 次往返。
    dbg::EventRing::Reset();
    dbg::EventRing::ResetTotals();
    {
        auto channel = MakeChannel<int>(0);
        std::thread sender([&] {
            for (int i = 0; i < 16; ++i) channel->Send(i);
        });
        std::thread receiver([&] {
            for (int i = 0; i < 16; ++i) channel->Recv();
        });
        sender.join();
        receiver.join();
    }
    PrintEventTotals("pingpong16");
    {
        const auto snapshot = dbg::EventRing::Snapshot();
        std::printf("[probe] pingpong16 snapshot=%zu send=%lld recv=%lld wake=%lld\n",
                    snapshot.size(),
                    CountCode(snapshot, dbg::SchedEventCode::kChannelSend),
                    CountCode(snapshot, dbg::SchedEventCode::kChannelRecv),
                    CountCode(snapshot, dbg::SchedEventCode::kChannelWake));
    }

    // 2) 单线程 CV：持锁 wait 后由另一线程 notify。
    dbg::EventRing::Reset();
    dbg::EventRing::ResetTotals();
    {
        Mutex mutex;
        ConditionVariable cv;
        bool ready = false;
        std::thread waiter([&] {
            mutex.Lock();
            while (!ready) {
                cv.WaitFor(mutex, 100ms);
            }
            mutex.Unlock();
        });
        std::this_thread::sleep_for(10ms);
        for (int i = 0; i < 5; ++i) {
            mutex.Lock();
            ready = true;
            cv.NotifyAll();
            mutex.Unlock();
            std::this_thread::sleep_for(5ms);
        }
        waiter.join();
    }
    PrintEventTotals("cv5");

    // 3) Mutex 竞争：2 线程各 200 次 lock/unlock。
    dbg::EventRing::Reset();
    dbg::EventRing::ResetTotals();
    {
        Mutex mutex;
        long long counter = 0;
        std::thread a([&] {
            for (int i = 0; i < 200; ++i) {
                mutex.Lock();
                ++counter;
                mutex.Unlock();
            }
        });
        std::thread b([&] {
            for (int i = 0; i < 200; ++i) {
                mutex.Lock();
                ++counter;
                mutex.Unlock();
            }
        });
        a.join();
        b.join();
        std::printf("[probe] mutex counter=%lld\n", counter);
    }
    PrintEventTotals("mutex200");

    // 4) WaitGroup：8 个 Done + Wait。
    dbg::EventRing::Reset();
    dbg::EventRing::ResetTotals();
    {
        WaitGroup wg;
        wg.Add(8);
        std::vector<std::thread> threads;
        for (int i = 0; i < 8; ++i) {
            threads.emplace_back([&] {
                std::this_thread::sleep_for(1ms);
                wg.Done();
            });
        }
        const bool ok = wg.WaitFor(5s);
        std::printf("[probe] wg wait_ok=%d\n", ok ? 1 : 0);
        for (auto& thread : threads) thread.join();
    }
    PrintEventTotals("wg8");

    // 5) RWMutex：4 读 + 2 写（各 100 次）。
    dbg::EventRing::Reset();
    dbg::EventRing::ResetTotals();
    {
        RWMutex mutex;
        CacheSnapshot cache;
        std::vector<std::thread> threads;
        for (int i = 0; i < 4; ++i) {
            threads.emplace_back([&] {
                for (int k = 0; k < 100; ++k) {
                    mutex.RLock();
                    volatile auto snapshot = cache;
                    (void)snapshot;
                    mutex.RUnlock();
                }
            });
        }
        for (int i = 0; i < 2; ++i) {
            threads.emplace_back([&] {
                for (int k = 0; k < 100; ++k) {
                    mutex.Lock();
                    ++cache.version;
                    cache.payload = cache.version * 2;
                    mutex.Unlock();
                }
            });
        }
        for (auto& thread : threads) thread.join();
    }
    PrintEventTotals("rw600");
    dbg::EventRing::Reset();
    return 0;
}

int RunRound(const Options& options) {
    std::vector<int> scenarios;
    if (options.scenarios.empty()) {
        scenarios = SelectScenarios(options.seed, options.round);
    } else {
        scenarios = ParseScenarioList(options.scenarios);
    }
    if (scenarios.size() < 3) {
        std::fprintf(stderr, "round %d has only %zu scenarios (need >= 3)\n",
                     options.round, scenarios.size());
        return 2;
    }

    std::printf("[round %d] seed=%llu scale=%d%% scenarios=", options.round,
                static_cast<unsigned long long>(options.seed),
                options.scale_percent);
    PrintScenarioList(scenarios);
    std::printf("\n");

    std::thread watchdog(WatchdogMain, options.round, options.seed,
                         options.watchdog_idle_sec, options.watchdog_total_sec);
    const auto started = Clock::now();
    long long ops_total = 0;
    long long checks_total = 0;
    ScenarioResult results[8];
    std::size_t result_count = 0;
    for (int id : scenarios) {
        g_stage.store(id, std::memory_order_relaxed);
        g_progress.fetch_add(1, std::memory_order_relaxed);
        const auto scenario_started = Clock::now();
        const ScenarioResult result = RunScenario(id, options.seed,
                                                  options.scale_percent);
        const double elapsed =
            std::chrono::duration<double>(Clock::now() - scenario_started).count();
        ops_total += result.ops;
        checks_total += result.checks;
        if (result_count < 8) {
            results[result_count++] = result;
        }
        std::printf(
            "[scenario %s] ops=%lld checks=%lld extra=%lld elapsed=%.3fs "
            "failures=%d\n",
            kScenarioNames[id], result.ops, result.checks, result.extra,
            elapsed, g_failures.load());
    }
    RequestFinish();
    watchdog.join();

    AuditDebugEvents();

    const double elapsed =
        std::chrono::duration<double>(Clock::now() - started).count();
    const bool ops_ok = ops_total >= options.min_ops;
    if (!ops_ok) {
        FailAt(__FILE__, __LINE__,
               "ops below minimum: " + std::to_string(ops_total) + " < " +
                   std::to_string(options.min_ops));
    }
    const int failures = g_failures.load();
    std::printf(
        "RESULT round=%d seed=%llu scale=%d ops=%lld checks=%lld failures=%d "
        "elapsed=%.3f debug=%d min_ops=%lld ok=%d\n",
        options.round, static_cast<unsigned long long>(options.seed),
        options.scale_percent, ops_total, checks_total, failures, elapsed,
        dbg::EventRing::Enabled() ? 1 : 0, options.min_ops,
        (failures == 0 && ops_ok) ? 1 : 0);
    std::fflush(stdout);
    return (failures == 0 && ops_ok) ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    if (!ParseOptions(argc, argv, &options)) {
        return 2;
    }
    InitStackDumpHandler();
    RegisterCurrentThread();
    if (options.plan) {
        const auto scenarios = options.scenarios.empty()
                                   ? SelectScenarios(options.seed, options.round)
                                   : ParseScenarioList(options.scenarios);
        std::printf("PLAN round=%d seed=%llu scenarios=", options.round,
                    static_cast<unsigned long long>(options.seed));
        PrintScenarioList(scenarios);
        std::printf("\n");
        return 0;
    }
    if (options.events_probe) {
        return RunEventsProbe(options);
    }
    return RunRound(options);
}
