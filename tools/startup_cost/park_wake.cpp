/**
 * @file park_wake.cpp
 * @brief 「首个 park→wake 间歇性 ~10 ms 长尾」的分段测量与归因工具。
 *
 * ## 为什么要单独一个工具
 *
 * `tools/startup_cost` 的场景 C 只把一次 `sync::Mutex` 竞争的
 * `Unlock → waiter 完成` 记成一个数（`sync-wake`），它是双峰的：多数几十 µs，
 * 少数约 10 ms。那个数**无法**区分下面四种耗时，因此无法归因：
 *
 * - A：`Unlock()` 自身（Notify 路径的代码执行）；
 * - B：`Unlock` 返回到 worker 恢复 Fiber（**唤醒 + 调度延迟**）；
 * - C：worker 恢复后把 Fiber 跑完（执行延迟）；
 * - D：worker 完成后主线程观察到标志（主线程自己的自旋/被抢占）。
 *
 * 本工具在两侧同时打 TSC 时间戳，把这四段拆开，并额外记录主线程的
 * `CLOCK_THREAD_CPUTIME_ID` 与 `/proc/self/task/<tid>/schedstat` 两个增量：
 * 前者用来判断「墙钟长了但主线程其实没在跑」（主线程被调度出去），后者
 * 用来判断「主线程 runnable 但排队等 CPU」。
 *
 * ## 三个对照实验
 *
 * 1. `runtime`：真实 `Scheduler` + `sync::Mutex` 竞争（复现报告里的那段）；
 * 2. `futex`：同一台机器上的**裸 futex** 往返（不经项目代码）。若它也有
 *    10 ms 尾巴，根因在环境（WSL2/宿主/调频），不在运行时；
 * 3. `cond`：裸 `pthread_cond_signal` 往返（经典条件变量基线）；
 * 4. `sleep`：`clock_nanosleep(1 ms)` 的实际返回抖动（VM 定时器基线）。
 *
 * `--idle-wait-ms` 可以覆盖 `SchedulerConfig::idle_wait`（默认 10 ms）。如果
 * 长尾位置随这个值移动，就证明尾巴来自「worker 睡满空闲超时才发现队列非空」，
 * 即唤醒被吞掉，而不是执行或唤醒本身慢。
 *
 * ## 用法
 *
 * ```bash
 * # 复现：进程内 1000 次，默认 4 worker、idle_wait=10ms、worker 睡稳后再 Unlock
 * .build-startup-cost/startup_cost_park_wake --mode runtime --iters 1000
 *
 * # 决定性实验：把空闲超时改成 2ms，看长尾是否跟着搬
 * .build-startup-cost/startup_cost_park_wake --mode runtime --iters 200 --idle-wait-ms 2
 *
 * # 单次（跨进程首次）：由 shell 循环 21 次，避免进程内校准把 CPU 预热
 * for i in $(seq 21); do .build-startup-cost/startup_cost_park_wake --mode runtime --once; done
 *
 * # 环境基线
 * .build-startup-cost/startup_cost_park_wake --mode futex --iters 1000
 * ```
 */
#include "measure.hpp"

#include "go2cpp/scheduler.hpp"
#include "go2cpp/sync.hpp"

#include <linux/futex.h>
#include <pthread.h>
#include <sched.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// 时间与探针
// ---------------------------------------------------------------------------

/// @brief 每次运行由 `--tick` 或校准给定；TSC 在 x86_64 上是恒定频率计数器。
double g_ns_per_tick = 0.3829;

inline std::uint64_t Ticks() noexcept { return __rdtsc(); }

inline double Ns(std::uint64_t ticks) noexcept {
    return static_cast<double>(ticks) * g_ns_per_tick;
}

inline void CpuRelax() noexcept {
    for (int i = 0; i < 32; ++i) {
        __builtin_ia32_pause();
    }
}

pid_t SelfTid() noexcept { return static_cast<pid_t>(::syscall(SYS_gettid)); }

/// @brief `/proc/self/task/<tid>/schedstat` 的一次快照（纳秒）。
struct SchedStat {
    std::uint64_t running_ns = 0;  ///< 真正占 CPU 的时间
    std::uint64_t waiting_ns = 0;  ///< runnable 但排队等 CPU 的时间
    std::uint64_t slices = 0;      ///< 被调度上 CPU 的次数
    bool ok = false;
};

SchedStat ReadSchedStat(pid_t tid) noexcept {
    SchedStat stat;
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/self/task/%d/schedstat",
                  static_cast<int>(tid));
    FILE* file = std::fopen(path, "r");
    if (file == nullptr) {
        return stat;
    }
    unsigned long long running = 0;
    unsigned long long waiting = 0;
    unsigned long long slices = 0;
    if (std::fscanf(file, "%llu %llu %llu", &running, &waiting, &slices) == 3) {
        stat.running_ns = static_cast<std::uint64_t>(running);
        stat.waiting_ns = static_cast<std::uint64_t>(waiting);
        stat.slices = static_cast<std::uint64_t>(slices);
        stat.ok = true;
    }
    std::fclose(file);
    return stat;
}

void SleepMicros(std::uint64_t micros) noexcept {
    timespec ts{};
    ts.tv_sec = static_cast<time_t>(micros / 1'000'000ULL);
    ts.tv_nsec = static_cast<long>((micros % 1'000'000ULL) * 1000ULL);
    while (::nanosleep(&ts, &ts) == -1 && errno == EINTR) {
    }
}

/// @brief 自旋等待谓词；`yield` 为 true 时让出 CPU（绑核场景必须）。
template <typename Predicate>
bool SpinUntil(Predicate predicate, std::uint64_t timeout_ns, bool yield) {
    const std::uint64_t deadline = startup_cost::MonotonicNs() + timeout_ns;
    std::uint64_t rounds = 0;
    while (!predicate()) {
        if (yield) {
            ::sched_yield();
        } else {
            CpuRelax();
        }
        if ((++rounds & 0x3fU) == 0U &&
            startup_cost::MonotonicNs() >= deadline) {
            return false;
        }
    }
    return true;
}

/// @brief 固定工作量的频率探针：返回执行它消耗的 TSC tick。
///
/// TSC 是恒定频率的，所以同一段依赖链在高频下花更少的 tick。若某个样本的
/// tick 数明显偏大，说明那一刻 CPU 在低频（或刚退出低功耗状态）。
std::uint64_t FrequencyProbeTicks() noexcept {
    static volatile std::uint64_t sink = 0;
    const std::uint64_t begin = Ticks();
    std::uint64_t x = 1;
    for (int i = 0; i < 4000; ++i) {
        x = x * 6364136223846793005ULL + 1442695040888963407ULL;
    }
    const std::uint64_t end = Ticks();
    sink = x;
    (void)sink;  // 读一次，显式声明这个 volatile 写不是死代码
    return end - begin;
}

// ---------------------------------------------------------------------------
// 参数
// ---------------------------------------------------------------------------

enum class Mode { kAll, kRuntime, kFutex, kCond, kSleep };

struct Options {
    Mode mode = Mode::kAll;
    int iters = 1000;
    int warm = 0;
    int workers = 4;
    int idle_wait_ms = 10;
    int pre_sleep_us = 200;
    int pin_cpu = -1;
    int wake_policy = 0;  ///< 0=runnext 1=lastmachine 2=roundrobin
    bool once = false;
    bool csv = false;
    bool calibrate = false;
    bool worker_schedstat = false;
    bool help = false;
};

[[noreturn]] void Usage() {
    std::printf(
        "用法: startup_cost_park_wake [选项]\n"
        "  --mode <all|runtime|futex|cond|sleep>  实验（默认 all）\n"
        "  --iters <N>        每实验样本数（默认 1000）\n"
        "  --warm <N>         正式统计前先跑 N 次（默认 0）\n"
        "  --once             只跑 1 次并打印明细（等同 --iters 1 --csv-detail）\n"
        "  --workers <N>      Scheduler worker 数（默认 4）\n"
        "  --idle-wait-ms <N> SchedulerConfig.idle_wait（默认 10）\n"
        "  --pre-sleep-us <N> 等 worker 睡稳后再 Unlock 的延迟（默认 200）\n"
        "  --pin <CPU>        把进程（含 worker）绑到指定 CPU\n"
        "  --wake-policy <p>  runnext|lastmachine|roundrobin（默认 runnext）\n"
        "  --tick <ns>        TSC ns/tick（默认 0.3829）\n"
        "  --calibrate        现场校准 TSC（会忙等 15ms，预热 CPU）\n"
        "  --csv              逐样本 CSV 输出到 stdout\n"
        "  --worker-schedstat 额外读 worker 的 schedstat（会污染 C 段）\n"
        "  -h, --help         显示本帮助\n");
    std::exit(0);
}

// ---------------------------------------------------------------------------
// 统计
// ---------------------------------------------------------------------------

struct Summary {
    std::size_t n = 0;
    double min = 0.0;
    double p50 = 0.0;
    double p90 = 0.0;
    double p99 = 0.0;
    double max = 0.0;
    std::size_t over_100us = 0;
    std::size_t over_1ms = 0;
    std::size_t over_5ms = 0;
};

Summary Summarize(const std::vector<double>& values) {
    Summary summary;
    if (values.empty()) {
        return summary;
    }
    std::vector<double> sorted = values;
    std::sort(sorted.begin(), sorted.end());
    summary.n = sorted.size();
    summary.min = sorted.front();
    summary.p50 = startup_cost::Percentile(sorted, 0.50);
    summary.p90 = startup_cost::Percentile(sorted, 0.90);
    summary.p99 = startup_cost::Percentile(sorted, 0.99);
    summary.max = sorted.back();
    for (const double value : sorted) {
        if (value > 100'000.0) {
            ++summary.over_100us;
        }
        if (value > 1'000'000.0) {
            ++summary.over_1ms;
        }
        if (value > 5'000'000.0) {
            ++summary.over_5ms;
        }
    }
    return summary;
}

void PrintSummary(const char* name, const std::vector<double>& values) {
    const Summary s = Summarize(values);
    std::printf(
        "%-22s n=%-5zu min=%-10s p50=%-10s p90=%-10s p99=%-10s max=%-10s "
        ">100us=%-4zu >1ms=%-4zu >5ms=%-4zu\n",
        name, s.n, startup_cost::HumanNs(s.min).c_str(),
        startup_cost::HumanNs(s.p50).c_str(),
        startup_cost::HumanNs(s.p90).c_str(),
        startup_cost::HumanNs(s.p99).c_str(),
        startup_cost::HumanNs(s.max).c_str(), s.over_100us, s.over_1ms,
        s.over_5ms);
    std::fflush(stdout);
}

// ---------------------------------------------------------------------------
// 实验 1：真实 Scheduler + sync::Mutex 的 park → wake
// ---------------------------------------------------------------------------

/// @brief 一轮 park→wake 的原始时间戳与派生段。
struct RuntimeSample {
    double prepare_us = 0.0;   ///< spawn → 观察到已 park
    double unlock_ns = 0.0;    ///< A：Unlock() 自身
    double wake_ns = 0.0;      ///< B：Unlock 返回 → worker 恢复 Fiber
    double run_ns = 0.0;       ///< C：worker 恢复 → Fiber 走完
    double observe_ns = 0.0;   ///< D：Fiber 走完 → 主线程看到标志
    double total_ns = 0.0;     ///< Unlock → 主线程看到标志（= A+B+C+D）
    double main_cpu_ns = 0.0;  ///< 主线程在 total 内的 CPU 时间
    double main_wait_ns = 0.0; ///< 主线程 schedstat waiting 增量
    double worker_wait_ns = -1.0;  ///< worker schedstat waiting 增量（可选）
    double freq_ticks = 0.0;    ///< 频率探针
};

/// @brief `--wake-policy` 的整数编码 → 调度器的唤醒投递策略。
go2cpp::scheduler::WakePolicy PolicyFromInt(int value) noexcept {
    switch (value) {
        case 1:
            return go2cpp::scheduler::WakePolicy::kLastMachine;
        case 2:
            return go2cpp::scheduler::WakePolicy::kRoundRobin;
        case 0:
        default:
            return go2cpp::scheduler::WakePolicy::kRunNext;
    }
}

int RunRuntime(const Options& options) {
    go2cpp::SchedulerConfig config;
    config.processor_count = static_cast<std::size_t>(options.workers);
    config.max_workers = static_cast<std::size_t>(options.workers);
    config.idle_wait = std::chrono::milliseconds(options.idle_wait_ms);

    go2cpp::Scheduler scheduler(config);
    scheduler.start();
    scheduler.set_wake_policy(PolicyFromInt(options.wake_policy));
    if (!SpinUntil(
            [&scheduler, &options] {
                return scheduler.worker_count() >=
                       static_cast<std::size_t>(options.workers);
            },
            2'000'000'000ULL, options.pin_cpu >= 0)) {
        std::fprintf(stderr, "[runtime] worker 未在 2s 内就绪\n");
        return 1;
    }
    // 让 worker 完成首次 idle 循环，进入稳定态（首次的线程启动成本不该混进
    // 每一次样本里）。
    SleepMicros(2000);

    std::printf(
        "# runtime: workers=%d idle_wait=%dms pre_sleep=%dus pin=%d "
        "worker_schedstat=%d\n",
        options.workers, options.idle_wait_ms, options.pre_sleep_us,
        options.pin_cpu, options.worker_schedstat ? 1 : 0);
    std::printf("# runtime: wake_policy=%d\n", options.wake_policy);

    if (options.csv) {
        std::printf(
            "iter,prepare_us,unlock_ns,wake_ns,run_ns,observe_ns,total_ns,"
            "main_cpu_ns,main_wait_ns,worker_wait_ns,freq_ticks\n");
    }

    const bool yield = options.pin_cpu >= 0;
    std::vector<RuntimeSample> samples;
    samples.reserve(static_cast<std::size_t>(options.iters));
    std::size_t invalid_samples = 0;
    std::size_t pre_unlock_wakes = 0;

    const int rounds_total = options.warm + options.iters;
    for (int iter = 0; iter < rounds_total; ++iter) {
        const bool record = iter >= options.warm;

        // 用 shared_ptr 而不是栈对象：Task 可能被调度器继续持有，
        // lambda 若捕获栈对象的引用，下一轮同地址的新 Mutex 会被上一轮
        // 的残留 Task 串用（实测会产生 worker_begin < unlock_end 的样本）。
        auto mutex = std::make_shared<go2cpp::sync::Mutex>();
        mutex->Lock();

        std::atomic<bool> done{false};
        std::atomic<std::uint64_t> worker_begin{0};
        std::atomic<std::uint64_t> worker_done{0};
        std::atomic<pid_t> worker_tid{0};

        const std::uint64_t spawn_ticks = Ticks();
        auto task = scheduler.spawn([mutex, &done, &worker_begin, &worker_done,
                                     &worker_tid, &options] {
            mutex->Lock();
            worker_begin.store(Ticks(), std::memory_order_relaxed);
            if (options.worker_schedstat) {
                worker_tid.store(SelfTid(), std::memory_order_relaxed);
            }
            mutex->Unlock();
            worker_done.store(Ticks(), std::memory_order_relaxed);
            done.store(true, std::memory_order_release);
        });
        if (!task) {
            std::fprintf(stderr, "[runtime] spawn 失败\n");
            return 1;
        }
        if (!SpinUntil(
                [&task] {
                    return task->state() == go2cpp::scheduler::GState::kWaiting;
                },
                5'000'000'000ULL, yield)) {
            std::fprintf(stderr, "[runtime] waiter 未在 5s 内 park\n");
            return 1;
        }
        const std::uint64_t parked_ticks = Ticks();

        // 让 worker 走完 512 轮空闲自旋、真正睡到 futex 上。否则 Unlock 会
        // 撞上自旋窗口，测到的是「自旋命中」而不是「futex 唤醒」。
        if (options.pre_sleep_us > 0) {
            SleepMicros(static_cast<std::uint64_t>(options.pre_sleep_us));
        }

        const std::uint64_t freq_ticks = FrequencyProbeTicks();
        const std::uint64_t cpu_before = startup_cost::ThreadCpuNs();
        const SchedStat main_before = ReadSchedStat(SelfTid());
        const bool done_before_unlock = done.load(std::memory_order_acquire);
        const std::uint64_t unlock_begin = Ticks();
        mutex->Unlock();
        const std::uint64_t unlock_end = Ticks();
        if (!SpinUntil(
                [&done] { return done.load(std::memory_order_acquire); },
                20'000'000'000ULL, yield)) {
            std::fprintf(stderr, "[runtime] waiter 未在 20s 内完成\n");
            return 1;
        }
        const std::uint64_t observed = Ticks();
        const std::uint64_t cpu_after = startup_cost::ThreadCpuNs();
        const SchedStat main_after = ReadSchedStat(SelfTid());

        RuntimeSample sample;
        sample.prepare_us = Ns(parked_ticks - spawn_ticks) / 1000.0;
        sample.unlock_ns = Ns(unlock_end - unlock_begin);
        // worker 可能比主线程记录 unlock_end 还快：Unlock 内部的 futex
        // wake 会立刻把被唤醒的 M 调度上去（绑核时尤其明显）。差值为负
        // 不是错误，而是「唤醒发生在 Unlock 内部」的最快一类，按 0 计并
        // 单独计数，避免无符号回绕把好事记成 7e9 秒。
        const std::int64_t wake_delta =
            static_cast<std::int64_t>(
                worker_begin.load(std::memory_order_relaxed)) -
            static_cast<std::int64_t>(unlock_end);
        sample.wake_ns = wake_delta > 0
                             ? static_cast<double>(wake_delta) * g_ns_per_tick
                             : 0.0;
        if (record && wake_delta <= 0) {
            ++pre_unlock_wakes;
        }
        sample.run_ns = Ns(worker_done.load(std::memory_order_relaxed) -
                           worker_begin.load(std::memory_order_relaxed));
        sample.observe_ns =
            Ns(observed - worker_done.load(std::memory_order_relaxed));
        sample.total_ns = Ns(observed - unlock_end);
        sample.main_cpu_ns = static_cast<double>(cpu_after - cpu_before);
        if (main_before.ok && main_after.ok) {
            sample.main_wait_ns = static_cast<double>(
                main_after.waiting_ns - main_before.waiting_ns);
        }
        sample.worker_wait_ns = -1.0;
        sample.freq_ticks = Ns(freq_ticks);

        if (options.worker_schedstat) {
            const pid_t tid = worker_tid.load(std::memory_order_relaxed);
            if (tid > 0) {
                const SchedStat worker = ReadSchedStat(tid);
                if (worker.ok) {
                    sample.worker_wait_ns =
                        static_cast<double>(worker.waiting_ns);
                }
            }
        }

        if (record && done_before_unlock) {
            // 健全性检查：Unlock 之前 waiter 就完成了，说明这一轮**没有**
            // 真正 park，B 段无意义，剔除而不是当成 0 或负数统计。
            ++invalid_samples;
        } else if (record) {
            if (options.csv) {
                std::printf(
                    "%d,%.3f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f\n",
                    iter, sample.prepare_us, sample.unlock_ns,
                    sample.wake_ns, sample.run_ns, sample.observe_ns,
                    sample.total_ns, sample.main_cpu_ns, sample.main_wait_ns,
                    sample.worker_wait_ns, sample.freq_ticks);
            }
            samples.push_back(sample);
        }
        // task 在这里析构：与测量段无关。
    }

    scheduler.shutdown();

    if (options.csv) {
        return 0;
    }

    std::vector<double> prepare;
    std::vector<double> unlock;
    std::vector<double> wake;
    std::vector<double> run;
    std::vector<double> observe;
    std::vector<double> total;
    std::vector<double> main_cpu;
    std::vector<double> main_wait;
    std::vector<double> freq;
    std::vector<double> gap;  // total - main_cpu：主线程「没在跑」的时间
    prepare.reserve(samples.size());
    unlock.reserve(samples.size());
    wake.reserve(samples.size());
    run.reserve(samples.size());
    observe.reserve(samples.size());
    total.reserve(samples.size());
    main_cpu.reserve(samples.size());
    main_wait.reserve(samples.size());
    freq.reserve(samples.size());
    gap.reserve(samples.size());
    for (const RuntimeSample& sample : samples) {
        prepare.push_back(sample.prepare_us * 1000.0);
        unlock.push_back(sample.unlock_ns);
        wake.push_back(sample.wake_ns);
        run.push_back(sample.run_ns);
        observe.push_back(sample.observe_ns);
        total.push_back(sample.total_ns);
        main_cpu.push_back(sample.main_cpu_ns);
        main_wait.push_back(sample.main_wait_ns);
        freq.push_back(sample.freq_ticks);
        gap.push_back(sample.total_ns - sample.main_cpu_ns);
    }
    std::printf(
        "\n== runtime 分段（%zu 样本，剔除 %zu 个非 park 样本，%zu 次唤醒在 "
        "Unlock 内完成）==\n",
        samples.size(), invalid_samples, pre_unlock_wakes);
    PrintSummary("prepare(spawn→park)", prepare);
    PrintSummary("A unlock", unlock);
    PrintSummary("B wake→worker-run", wake);
    PrintSummary("C worker-run→done", run);
    PrintSummary("D done→observed", observe);
    PrintSummary("TOTAL unlock→obs", total);
    PrintSummary("main thread CPU", main_cpu);
    PrintSummary("main wall-CPU gap", gap);
    PrintSummary("main schedstat wait", main_wait);
    PrintSummary("freq probe", freq);

    // 长尾样本明细：wake 段最大的前 8 个。
    std::vector<std::size_t> order(samples.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    std::sort(order.begin(), order.end(), [&samples](std::size_t lhs,
                                                     std::size_t rhs) {
        return samples[lhs].total_ns > samples[rhs].total_ns;
    });
    std::printf("\n== 最慢的 8 个样本 ==\n");
    std::printf("%-6s %-12s %-10s %-14s %-12s %-12s %-14s %-12s\n", "iter",
                "prepare", "A unlock", "B wake", "C run", "D observe",
                "TOTAL", "main CPU");
    const std::size_t top = std::min<std::size_t>(8, order.size());
    for (std::size_t k = 0; k < top; ++k) {
        const RuntimeSample& s = samples[order[k]];
        std::printf("%-6zu %-12s %-10s %-14s %-12s %-12s %-14s %-12s\n",
                    order[k],
                    startup_cost::HumanNs(s.prepare_us * 1000.0).c_str(),
                    startup_cost::HumanNs(s.unlock_ns).c_str(),
                    startup_cost::HumanNs(s.wake_ns).c_str(),
                    startup_cost::HumanNs(s.run_ns).c_str(),
                    startup_cost::HumanNs(s.observe_ns).c_str(),
                    startup_cost::HumanNs(s.total_ns).c_str(),
                    startup_cost::HumanNs(s.main_cpu_ns).c_str());
    }
    return 0;
}

// ---------------------------------------------------------------------------
// 实验 2：裸 futex 往返（不经项目代码，判断环境基线）
// ---------------------------------------------------------------------------

struct FutexShared {
    alignas(64) std::atomic<std::uint32_t> word{0};
    alignas(64) std::atomic<int> phase{0};  // 0 空闲 / 1 开始 / 2 将睡 / 3 已醒
    std::atomic<std::uint64_t> wake_ticks{0};
    std::atomic<bool> stop{false};
};

int RunFutex(const Options& options) {
    FutexShared shared;
    std::atomic<std::uint64_t> worst{0};

    std::printf("# futex: iters=%d pin=%d\n", options.iters, options.pin_cpu);

    pthread_t thread;
    auto body = [](void* argument) -> void* {
        FutexShared* s = static_cast<FutexShared*>(argument);
        std::uint32_t expected = s->word.load(std::memory_order_relaxed);
        while (!s->stop.load(std::memory_order_acquire)) {
            if (s->phase.load(std::memory_order_acquire) != 1) {
                CpuRelax();
                continue;
            }
            s->phase.store(2, std::memory_order_release);
            const std::uint32_t current =
                s->word.load(std::memory_order_acquire);
            if (current == expected) {
                (void)::syscall(SYS_futex,
                                reinterpret_cast<std::uint32_t*>(&s->word),
                                FUTEX_WAIT_PRIVATE, expected, nullptr, nullptr,
                                0);
            }
            s->wake_ticks.store(Ticks(), std::memory_order_relaxed);
            expected = s->word.load(std::memory_order_acquire);
            s->phase.store(3, std::memory_order_release);
        }
        return nullptr;
    };
    if (::pthread_create(&thread, nullptr, body, &shared) != 0) {
        std::fprintf(stderr, "[futex] pthread_create 失败\n");
        return 1;
    }

    std::vector<double> latency;
    latency.reserve(static_cast<std::size_t>(options.iters));
    std::vector<double> freq;
    freq.reserve(static_cast<std::size_t>(options.iters));

    const bool yield = options.pin_cpu >= 0;
    for (int iter = 0; iter < options.warm + options.iters; ++iter) {
        // 上一轮结束时 worker 已把 phase 置 3 并在自旋等下一轮的 1，
        // 这里直接开新一轮；不要再等一次 3，否则每轮都会等到超时。
        const std::uint64_t probe = FrequencyProbeTicks();
        shared.phase.store(1, std::memory_order_release);
        if (!SpinUntil([&shared] { return shared.phase.load() == 2; },
                       2'000'000'000ULL, yield)) {
            std::fprintf(stderr, "[futex] worker 未进入等待\n");
            break;
        }
        const std::uint64_t wake_begin = Ticks();
        shared.word.fetch_add(1, std::memory_order_release);
        (void)::syscall(SYS_futex,
                        reinterpret_cast<std::uint32_t*>(&shared.word),
                        FUTEX_WAKE_PRIVATE, 1, nullptr, nullptr, 0);
        if (!SpinUntil([&shared] { return shared.phase.load() == 3; },
                       2'000'000'000ULL, yield)) {
            std::fprintf(stderr, "[futex] 未在 2s 内醒来\n");
            break;
        }
        const std::uint64_t observed = Ticks();
        if (iter >= options.warm) {
            latency.push_back(
                Ns(shared.wake_ticks.load(std::memory_order_relaxed) -
                   wake_begin));
            freq.push_back(Ns(probe));
            if (latency.back() > 1'000'000.0) {
                std::printf("  [futex] iter=%d 长尾 %s\n", iter,
                            startup_cost::HumanNs(latency.back()).c_str());
            }
        }
        (void)observed;
    }

    shared.stop.store(true, std::memory_order_release);
    shared.phase.store(1, std::memory_order_release);
    shared.word.fetch_add(1, std::memory_order_release);
    (void)::syscall(SYS_futex, reinterpret_cast<std::uint32_t*>(&shared.word),
                    FUTEX_WAKE_PRIVATE, 1, nullptr, nullptr, 0);
    ::pthread_join(thread, nullptr);

    if (options.csv) {
        std::printf("futex_latency_ns,freq_ns\n");
        for (std::size_t i = 0; i < latency.size(); ++i) {
            std::printf("%.1f,%.1f\n", latency[i], freq[i]);
        }
        return 0;
    }
    std::printf("\n== 裸 futex wake→被唤醒线程恢复 ==\n");
    PrintSummary("wake→resume", latency);
    PrintSummary("freq probe", freq);
    (void)worst;
    return 0;
}

// ---------------------------------------------------------------------------
// 实验 3：裸 pthread_cond 往返
// ---------------------------------------------------------------------------

struct CondShared {
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
    alignas(64) std::atomic<int> phase{0};
    std::atomic<std::uint64_t> wake_ticks{0};
    std::atomic<bool> stop{false};
    int generation = 0;
};

int RunCond(const Options& options) {
    CondShared shared;
    std::printf("# cond: iters=%d pin=%d\n", options.iters, options.pin_cpu);

    pthread_t thread;
    auto body = [](void* argument) -> void* {
        CondShared* s = static_cast<CondShared*>(argument);
        ::pthread_mutex_lock(&s->mutex);
        int seen = s->generation;
        while (!s->stop.load(std::memory_order_acquire)) {
            if (s->phase.load(std::memory_order_acquire) != 1) {
                ::pthread_mutex_unlock(&s->mutex);
                CpuRelax();
                ::pthread_mutex_lock(&s->mutex);
                continue;
            }
            s->phase.store(2, std::memory_order_release);
            while (s->generation == seen) {
                ::pthread_cond_wait(&s->cond, &s->mutex);
            }
            seen = s->generation;
            s->wake_ticks.store(Ticks(), std::memory_order_relaxed);
            s->phase.store(3, std::memory_order_release);
        }
        ::pthread_mutex_unlock(&s->mutex);
        return nullptr;
    };
    if (::pthread_create(&thread, nullptr, body, &shared) != 0) {
        std::fprintf(stderr, "[cond] pthread_create 失败\n");
        return 1;
    }

    std::vector<double> latency;
    latency.reserve(static_cast<std::size_t>(options.iters));
    std::vector<double> freq;
    freq.reserve(static_cast<std::size_t>(options.iters));

    const bool yield = options.pin_cpu >= 0;
    for (int iter = 0; iter < options.warm + options.iters; ++iter) {
        const std::uint64_t probe = FrequencyProbeTicks();
        shared.phase.store(1, std::memory_order_release);
        if (!SpinUntil([&shared] { return shared.phase.load() == 2; },
                       2'000'000'000ULL, yield)) {
            std::fprintf(stderr, "[cond] worker 未进入等待\n");
            break;
        }
        const std::uint64_t wake_begin = Ticks();
        ::pthread_mutex_lock(&shared.mutex);
        ++shared.generation;
        ::pthread_cond_signal(&shared.cond);
        ::pthread_mutex_unlock(&shared.mutex);
        if (!SpinUntil([&shared] { return shared.phase.load() == 3; },
                       2'000'000'000ULL, yield)) {
            std::fprintf(stderr, "[cond] 未在 2s 内醒来\n");
            break;
        }
        if (iter >= options.warm) {
            latency.push_back(
                Ns(shared.wake_ticks.load(std::memory_order_relaxed) -
                   wake_begin));
            freq.push_back(Ns(probe));
            if (latency.back() > 1'000'000.0) {
                std::printf("  [cond] iter=%d 长尾 %s\n", iter,
                            startup_cost::HumanNs(latency.back()).c_str());
            }
        }
        shared.phase.store(0, std::memory_order_release);
    }

    shared.stop.store(true, std::memory_order_release);
    ::pthread_mutex_lock(&shared.mutex);
    ++shared.generation;
    ::pthread_cond_signal(&shared.cond);
    ::pthread_mutex_unlock(&shared.mutex);
    ::pthread_join(thread, nullptr);
    ::pthread_mutex_destroy(&shared.mutex);
    ::pthread_cond_destroy(&shared.cond);

    if (options.csv) {
        std::printf("cond_latency_ns,freq_ns\n");
        for (std::size_t i = 0; i < latency.size(); ++i) {
            std::printf("%.1f,%.1f\n", latency[i], freq[i]);
        }
        return 0;
    }
    std::printf("\n== 裸 pthread_cond signal→被唤醒线程恢复 ==\n");
    PrintSummary("signal→resume", latency);
    PrintSummary("freq probe", freq);
    return 0;
}

// ---------------------------------------------------------------------------
// 实验 4：VM 定时器基线（clock_nanosleep 1ms 的实际返回时刻）
// ---------------------------------------------------------------------------

int RunSleep(const Options& options) {
    std::printf("# sleep: iters=%d pin=%d\n", options.iters, options.pin_cpu);
    constexpr long kSleepNs = 1'000'000;
    std::vector<double> overshoot;
    overshoot.reserve(static_cast<std::size_t>(options.iters));
    for (int iter = 0; iter < options.warm + options.iters; ++iter) {
        timespec deadline{};
        ::clock_gettime(CLOCK_MONOTONIC, &deadline);
        deadline.tv_nsec += kSleepNs;
        if (deadline.tv_nsec >= 1'000'000'000L) {
            deadline.tv_nsec -= 1'000'000'000L;
            deadline.tv_sec += 1;
        }
        const std::uint64_t begin = startup_cost::MonotonicNs();
        (void)::clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline,
                                nullptr);
        const double elapsed =
            static_cast<double>(startup_cost::MonotonicNs() - begin);
        if (iter >= options.warm) {
            overshoot.push_back(elapsed - static_cast<double>(kSleepNs));
        }
    }
    std::printf("\n== clock_nanosleep(1ms) 超时量 ==\n");
    PrintSummary("overshoot", overshoot);
    return 0;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int SetAffinity(int cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (::sched_setaffinity(0, sizeof(set), &set) != 0) {
        std::fprintf(stderr, "[pin] sched_setaffinity(%d) 失败: %s\n", cpu,
                     std::strerror(errno));
        return 1;
    }
    return 0;
}

/// @brief `--once` 的明细输出：一行 key=value，便于 shell 聚合。
int RunOnceRuntime(const Options& options) {
    go2cpp::SchedulerConfig config;
    config.processor_count = static_cast<std::size_t>(options.workers);
    config.max_workers = static_cast<std::size_t>(options.workers);
    config.idle_wait = std::chrono::milliseconds(options.idle_wait_ms);

    const std::uint64_t t_start = Ticks();
    go2cpp::Scheduler scheduler(config);
    scheduler.start();
    scheduler.set_wake_policy(PolicyFromInt(options.wake_policy));
    const std::uint64_t t_started = Ticks();
    if (!SpinUntil(
            [&scheduler, &options] {
                return scheduler.worker_count() >=
                       static_cast<std::size_t>(options.workers);
            },
            2'000'000'000ULL, false)) {
        std::fprintf(stderr, "[once] worker 未就绪\n");
        return 1;
    }
    const std::uint64_t t_workers = Ticks();

    auto mutex = std::make_shared<go2cpp::sync::Mutex>();
    mutex->Lock();
    std::atomic<bool> done{false};
    std::atomic<std::uint64_t> worker_begin{0};
    std::atomic<std::uint64_t> worker_done{0};
    const std::uint64_t t_spawn0 = Ticks();
    auto task = scheduler.spawn([mutex, &done, &worker_begin, &worker_done] {
        mutex->Lock();
        worker_begin.store(Ticks(), std::memory_order_relaxed);
        mutex->Unlock();
        worker_done.store(Ticks(), std::memory_order_relaxed);
        done.store(true, std::memory_order_release);
    });
    if (!task) {
        std::fprintf(stderr, "[once] spawn 失败\n");
        return 1;
    }
    const bool parked = SpinUntil(
        [&task] {
            return task->state() == go2cpp::scheduler::GState::kWaiting;
        },
        5'000'000'000ULL, false);
    const std::uint64_t t_parked = Ticks();
    if (options.pre_sleep_us > 0) {
        SleepMicros(static_cast<std::uint64_t>(options.pre_sleep_us));
    }
    const SchedStat main_before = ReadSchedStat(SelfTid());
    const std::uint64_t cpu_before = startup_cost::ThreadCpuNs();
    const bool done_before_unlock = done.load(std::memory_order_acquire);
    const std::uint64_t unlock_begin = Ticks();
    mutex->Unlock();
    const std::uint64_t unlock_end = Ticks();
    const bool finished = SpinUntil(
        [&done] { return done.load(std::memory_order_acquire); },
        20'000'000'000ULL, false);
    const std::uint64_t observed = Ticks();
    const std::uint64_t cpu_after = startup_cost::ThreadCpuNs();
    const SchedStat main_after = ReadSchedStat(SelfTid());
    scheduler.shutdown();

    std::printf(
        "parked=%d finished=%d done_before_unlock=%d start_us=%.1f "
        "spawn_us=%.1f prepare_us=%.1f "
        "unlock_ns=%.1f wake_ns=%.1f run_ns=%.1f observe_ns=%.1f "
        "total_ns=%.1f main_cpu_ns=%.1f main_wait_ns=%.1f\n",
        parked ? 1 : 0, finished ? 1 : 0, done_before_unlock ? 1 : 0,
        Ns(t_started - t_start) / 1000.0,
        Ns(t_workers - t_started) / 1000.0, Ns(t_parked - t_spawn0) / 1000.0,
        Ns(unlock_end - unlock_begin),
        (worker_begin.load(std::memory_order_relaxed) > unlock_end
             ? Ns(worker_begin.load(std::memory_order_relaxed) -
                  unlock_end)
             : 0.0),
        Ns(worker_done.load(std::memory_order_relaxed) -
           worker_begin.load(std::memory_order_relaxed)),
        Ns(observed - worker_done.load(std::memory_order_relaxed)),
        Ns(observed - unlock_end),
        static_cast<double>(cpu_after - cpu_before),
        static_cast<double>(main_after.waiting_ns - main_before.waiting_ns));
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto value = [&argc, &argv, &i]() -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "缺少参数值: %s\n", argv[i]);
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--mode") {
            const std::string mode = value();
            if (mode == "all") {
                options.mode = Mode::kAll;
            } else if (mode == "runtime") {
                options.mode = Mode::kRuntime;
            } else if (mode == "futex") {
                options.mode = Mode::kFutex;
            } else if (mode == "cond") {
                options.mode = Mode::kCond;
            } else if (mode == "sleep") {
                options.mode = Mode::kSleep;
            } else {
                std::fprintf(stderr, "未知 mode: %s\n", mode.c_str());
                return 2;
            }
        } else if (arg == "--iters") {
            options.iters = std::atoi(value());
        } else if (arg == "--warm") {
            options.warm = std::atoi(value());
        } else if (arg == "--workers") {
            options.workers = std::atoi(value());
        } else if (arg == "--idle-wait-ms") {
            options.idle_wait_ms = std::atoi(value());
        } else if (arg == "--pre-sleep-us") {
            options.pre_sleep_us = std::atoi(value());
        } else if (arg == "--pin") {
            options.pin_cpu = std::atoi(value());
        } else if (arg == "--wake-policy") {
            const std::string policy = value();
            if (policy == "runnext") {
                options.wake_policy = 0;
            } else if (policy == "lastmachine") {
                options.wake_policy = 1;
            } else if (policy == "roundrobin") {
                options.wake_policy = 2;
            } else {
                std::fprintf(stderr, "未知 wake-policy: %s\n",
                             policy.c_str());
                return 2;
            }
        } else if (arg == "--tick") {
            g_ns_per_tick = std::atof(value());
        } else if (arg == "--calibrate") {
            options.calibrate = true;
        } else if (arg == "--once") {
            options.once = true;
        } else if (arg == "--csv") {
            options.csv = true;
        } else if (arg == "--worker-schedstat") {
            options.worker_schedstat = true;
        } else if (arg == "-h" || arg == "--help") {
            options.help = true;
        } else {
            std::fprintf(stderr, "未知参数: %s\n", arg.c_str());
            return 2;
        }
    }
    if (options.help) {
        Usage();
    }
    if (options.calibrate) {
        const startup_cost::TscClock clock = startup_cost::TscClock::Calibrate();
        if (clock.valid()) {
            g_ns_per_tick = clock.ns_per_tick();
        }
    }
    if (options.pin_cpu >= 0) {
        if (SetAffinity(options.pin_cpu) != 0) {
            return 1;
        }
    }

    if (options.once) {
        return RunOnceRuntime(options);
    }
    switch (options.mode) {
        case Mode::kRuntime:
            return RunRuntime(options);
        case Mode::kFutex:
            return RunFutex(options);
        case Mode::kCond:
            return RunCond(options);
        case Mode::kSleep:
            return RunSleep(options);
        case Mode::kAll:
        default:
            break;
    }
    const int runtime = RunRuntime(options);
    if (runtime != 0) {
        return runtime;
    }
    const int futex = RunFutex(options);
    if (futex != 0) {
        return futex;
    }
    const int cond = RunCond(options);
    if (cond != 0) {
        return cond;
    }
    return RunSleep(options);
}
