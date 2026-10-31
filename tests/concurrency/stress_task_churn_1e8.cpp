/**
 * @file stress_task_churn_1e8.cpp
 * @brief 调度器任务生命周期独立压测：1e8 次创建/调度、100k 并发、Fiber+线程混合。
 *
 * ## 为什么单独压测
 *
 * 优化改动落在 Task 完成等待、Fiber 复用判定、incoming 队列与入队绑定路径上，
 * 这些路径的回归不会在「典型用法」里暴露（丢唤醒/少入队只在长跑、突发并发
 * 或混合提交时出现）。本压测用**独立进程**验证：
 *
 * 1. **churn**：分批提交 N 个空任务，每批校验「完成数 == 提交数」且
 *    「每个任务恰好执行一次」（用 id 累加和不变量证明无丢无重）；
 * 2. **100k 突发**：一次性提交 100k 个可运行任务，4P/4M 下全部完成；
 *    另用 100k/8 个 yield 任务验证重入队路径；
 * 3. **混合**：多个外部线程并发提交 + 任务内 yield。
 *
 * 全程有 watchdog：任何阶段 60 秒没有进展就打印诊断并 abort（非零退出），
 * 把「挂死」变成可定位的失败而不是超时。
 *
 * 编译（与 tests/concurrency 其它压测一致，不注册进 ctest）：
 *   g++ -std=c++20 -O3 -DNDEBUG -Iinclude tests/concurrency/stress_task_churn_1e8.cpp \
 *       -L.build-bento-wsl -Wl,-rpath,$PWD/.build-bento-wsl \
 *       -lgo2cpp_scheduler -lgo2cpp_fiber -lgo2cpp_error -lgo2cpp_debug \
 *       -lgo2cpp_debug_event -lpthread -o /tmp/stress_task_churn_1e8
 *
 * 用法：
 *   /tmp/stress_task_churn_1e8 [总任务数=100000000] [批次=1000000] [workers=4]
 */
#include "go2cpp/scheduler.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr std::size_t kShards = 64;

std::uint64_t NowNs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now().time_since_epoch())
            .count());
}

struct ShardCounters {
    // 完成计数用单原子：等待线程每轮只读一个 cacheline。若也按分片累加，
    // 等待线程每轮要读 64 个 cacheline，会把分片全部钉在本地核上，worker
    // 每完成一个任务都要跨核迁移缓存行，实测把压测本身拖慢一个数量级。
    std::atomic<std::uint64_t> done{0};
    // id 只用于「每个任务恰好执行一次」的校验，按分片累加避免争用；
    // 只在批次收尾读一次。
    std::atomic<std::uint64_t> id_sum[kShards];

    ShardCounters() {
        for (std::size_t i = 0; i < kShards; ++i) {
            id_sum[i].store(0, std::memory_order_relaxed);
        }
    }

    void Add(std::uint64_t id) {
        const std::size_t shard =
            static_cast<std::size_t>(id) & (kShards - 1);
        id_sum[shard].fetch_add(id, std::memory_order_relaxed);
        done.fetch_add(1, std::memory_order_release);
    }

    std::uint64_t Done() const {
        return done.load(std::memory_order_acquire);
    }

    std::uint64_t IdSum() const {
        std::uint64_t total = 0;
        for (std::size_t i = 0; i < kShards; ++i) {
            total += id_sum[i].load(std::memory_order_acquire);
        }
        return total;
    }
};

/// 看门狗：任何阶段超过 kStallLimit 没有进展就报错退出。
class Watchdog {
public:
    static constexpr std::chrono::seconds kStallLimit{60};

    Watchdog() {
        m_last_progress.store(NowNs(), std::memory_order_release);
        m_stage.store("bootstrap", std::memory_order_release);
        m_thread = std::thread([this] { Run(); });
    }

    ~Watchdog() {
        m_stop.store(true, std::memory_order_release);
        if (m_thread.joinable()) {
            m_thread.join();
        }
    }

    void Progress(const char* stage) {
        m_stage.store(stage, std::memory_order_release);
        m_last_progress.store(NowNs(), std::memory_order_release);
    }

private:
    void Run() {
        while (!m_stop.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(1s);
            const std::uint64_t last =
                m_last_progress.load(std::memory_order_acquire);
            const std::uint64_t idle_ns = NowNs() - last;
            if (idle_ns > static_cast<std::uint64_t>(kStallLimit.count()) *
                              1000000000ULL) {
                std::fprintf(
                    stderr,
                    "[watchdog] 无进展 %llu ms，阶段=%s，abort\n",
                    static_cast<unsigned long long>(idle_ns / 1000000ULL),
                    m_stage.load(std::memory_order_acquire));
                std::fflush(stderr);
                std::abort();
            }
        }
    }

    std::atomic<bool> m_stop{false};
    std::atomic<std::uint64_t> m_last_progress{0};
    std::atomic<const char*> m_stage{nullptr};
    std::thread m_thread;
};

/// 等待完成计数到 expect。
///
/// 不用紧循环 `yield()`：完成计数与多个 worker 的 fetch_add 共享同一条
/// cacheline，紧循环读会让它不断在核间迁移，把 worker 每次完成计数拖慢
/// 到微秒级（实测把整轮压测放慢 10 倍）。低频轮询的代价是每批最多多等
/// 一个轮询间隔，对「ns/task」统计可以忽略。
void WaitDone(const ShardCounters& counters, std::uint64_t expect) {
    while (counters.Done() != expect) {
        std::this_thread::sleep_for(50us);
    }
}

[[noreturn]] void Fail(const char* what) {
    std::fprintf(stderr, "[FAIL] %s\n", what);
    std::fflush(stderr);
    std::abort();
}

go2cpp::SchedulerConfig MakeConfig(std::size_t workers) {
    go2cpp::SchedulerConfig config;
    config.processor_count = workers;
    config.max_workers = workers;
    config.min_workers = workers;
    config.idle_wait = 2ms;
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
    // 插桩会给每层栈帧加红区/影子栈记录，调度器默认的 2KB 初始栈
    // （页对齐后 4KB）在 sanitizer 下不够用，会在 Fiber 首次进入的
    // 动态链接路径上溢出。压测在插桩构建里显式放大初始栈。
    config.fiber_stack_size = 256U * 1024U;
#endif
    return config;
}

/// 场景 1：分批 churn。每批校验完成数与 id 和，证明无丢、无重。
void ScenarioChurn(std::uint64_t total, std::uint64_t batch,
                   std::size_t workers, Watchdog& dog) {
    std::printf("[churn] total=%llu batch=%llu workers=%zu\n",
                static_cast<unsigned long long>(total),
                static_cast<unsigned long long>(batch), workers);
    go2cpp::Scheduler scheduler(MakeConfig(workers));
    scheduler.start();
    const std::uint64_t started = NowNs();
    std::uint64_t completed = 0;
    std::uint64_t rounds = 0;
    while (completed < total) {
        const std::uint64_t this_batch =
            std::min<std::uint64_t>(batch, total - completed);
        auto counters = std::make_shared<ShardCounters>();
        for (std::uint64_t i = 0; i < this_batch; ++i) {
            const std::uint64_t id = completed + i;
            // 不保留 Task 句柄：走 fire-and-forget，强引用由 scheduler
            // 注册表持有，完成后由 worker 节拍回收。
            scheduler.spawn([counters, id] { counters->Add(id); });
            if ((i & 0x3FFF) == 0) {
                dog.Progress("churn->spawn");
            }
        }
        dog.Progress("churn->wait");
        WaitDone(*counters, this_batch);
        const std::uint64_t got = counters->IdSum();
        const std::uint64_t base = completed;
        const std::uint64_t expect =
            this_batch * (2 * base + (this_batch - 1)) / 2;
        if (got != expect) {
            std::fprintf(stderr,
                         "[FAIL] churn id 和不符: round=%llu got=%llu "
                         "expect=%llu\n",
                         static_cast<unsigned long long>(rounds),
                         static_cast<unsigned long long>(got),
                         static_cast<unsigned long long>(expect));
            std::abort();
        }
        completed += this_batch;
        ++rounds;
        if ((rounds % 10) == 0 || completed == total) {
            const double ns = static_cast<double>(NowNs() - started);
            std::printf("[churn] %llu/%llu  %.1f ns/task\n",
                        static_cast<unsigned long long>(completed),
                        static_cast<unsigned long long>(total), ns / completed);
            std::fflush(stdout);
        }
        dog.Progress("churn->next");
    }
    scheduler.shutdown();
    const double ns = static_cast<double>(NowNs() - started);
    std::printf("[churn] PASS total=%llu rounds=%llu  %.1f ns/task\n",
                static_cast<unsigned long long>(completed),
                static_cast<unsigned long long>(rounds), ns / completed);
    if (completed != total) {
        Fail("churn 完成数不等于提交数");
    }
}

/// 场景 2：一次性 100k 并发任务 + yield 重入队任务。
void ScenarioBurst(std::size_t burst, std::size_t workers, Watchdog& dog) {
    std::printf("[burst] n=%zu workers=%zu\n", burst, workers);
    go2cpp::Scheduler scheduler(MakeConfig(workers));
    scheduler.start();
    {
        auto counters = std::make_shared<ShardCounters>();
        dog.Progress("burst->spawn");
        for (std::size_t i = 0; i < burst; ++i) {
            const std::uint64_t id = i;
            scheduler.spawn([counters, id] { counters->Add(id); });
        }
        dog.Progress("burst->wait");
        WaitDone(*counters, burst);
        const std::uint64_t expect =
            static_cast<std::uint64_t>(burst) * (burst - 1) / 2;
        if (counters->IdSum() != expect) {
            Fail("burst id 和不符（有任务丢失或重复执行）");
        }
    }
    {
        // yield 路径：每个任务重入队 8 次，覆盖 requeue/steal 路径。
        auto counters = std::make_shared<ShardCounters>();
        const std::size_t yields = burst / 8;
        dog.Progress("burst->yield-spawn");
        for (std::size_t i = 0; i < yields; ++i) {
            const std::uint64_t id = i;
            scheduler.spawn([&scheduler, counters, id] {
                for (int round = 0; round < 8; ++round) {
                    if (!scheduler.yield_current()) {
                        break;
                    }
                }
                counters->Add(id);
            });
        }
        dog.Progress("burst->yield-wait");
        WaitDone(*counters, yields);
        const std::uint64_t expect =
            static_cast<std::uint64_t>(yields) * (yields - 1) / 2;
        if (counters->IdSum() != expect) {
            Fail("yield 批次 id 和不符");
        }
    }
    scheduler.shutdown();
    std::printf("[burst] PASS n=%zu\n", burst);
}

/// 场景 3：多线程并发提交 + 任务内 yield（Fiber 与线程混合）。
void ScenarioMixed(std::size_t producers, std::size_t per_producer,
                   std::size_t workers, Watchdog& dog) {
    std::printf("[mixed] producers=%zu per=%zu workers=%zu\n", producers,
                per_producer, workers);
    go2cpp::Scheduler scheduler(MakeConfig(workers));
    scheduler.start();
    auto counters = std::make_shared<ShardCounters>();
    std::atomic<std::size_t> spawned{0};
    std::vector<std::thread> threads;
    threads.reserve(producers);
    for (std::size_t p = 0; p < producers; ++p) {
        threads.emplace_back([&, p] {
            for (std::size_t i = 0; i < per_producer; ++i) {
                const std::uint64_t id =
                    static_cast<std::uint64_t>(p) * per_producer + i;
                const bool do_yield = (i % 4) == 0;
                scheduler.spawn([&scheduler, counters, id, do_yield] {
                    if (do_yield) {
                        for (int round = 0; round < 3; ++round) {
                            if (!scheduler.yield_current()) {
                                break;
                            }
                        }
                    }
                    counters->Add(id);
                });
                spawned.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    dog.Progress("mixed->join-producers");
    for (auto& thread : threads) {
        thread.join();
    }
    const std::size_t total = producers * per_producer;
    if (spawned.load() != total) {
        Fail("mixed 提交数不符");
    }
    dog.Progress("mixed->wait");
    WaitDone(*counters, total);
    const std::uint64_t expect =
        static_cast<std::uint64_t>(total) * (total - 1) / 2;
    if (counters->IdSum() != expect) {
        Fail("mixed id 和不符");
    }
    scheduler.shutdown();
    std::printf("[mixed] PASS total=%zu\n", total);
}

}  // namespace

int main(int argc, char** argv) {
    std::uint64_t total = argc > 1 ? std::strtoull(argv[1], nullptr, 10)
                                   : 100000000ULL;
    std::uint64_t batch = argc > 2 ? std::strtoull(argv[2], nullptr, 10)
                                   : 1000000ULL;
    std::size_t workers =
        argc > 3 ? static_cast<std::size_t>(std::atoi(argv[3])) : 4;
    if (workers == 0) {
        workers = 1;
    }
    if (batch == 0) {
        batch = 1;
    }
    std::printf(
        "[cfg] total=%llu batch=%llu workers=%zu scheduler churn stress\n",
        static_cast<unsigned long long>(total),
        static_cast<unsigned long long>(batch), workers);
    Watchdog dog;
    dog.Progress("burst");
    ScenarioBurst(100000, workers, dog);
    dog.Progress("mixed");
    ScenarioMixed(8, 12500, workers, dog);
    dog.Progress("churn");
    ScenarioChurn(total, batch, workers, dog);
    std::printf("[ALL PASS] stress_task_churn_1e8\n");
    return 0;
}
