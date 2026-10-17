/**
 * @file mixed_random_benchmark.cpp
 * @brief 混合随机负载仿真：随机任务类型 + 随机长度，对比 Scheduler 与线程池。
 *
 * ## 设计意图
 *
 * 现有压测（high_load_stress）的五个场景都是**固定形状**的：固定任务数、
 * 固定计算量、固定等待次数。真实服务不是这样 —— 请求长度长尾分布、
 * 一部分纯计算、一部分中途要让出等待。本基准补这一类负载。
 *
 * ## 任务的三种形态（按随机比例抽取）
 *
 * - `pure`  : 只做计算，不让出（模拟纯 CPU 请求）
 * - `yield` : 计算到一半让出 1~3 次（模拟等待下游/锁/IO 的请求）
 * - `short` : 极短计算（模拟缓存命中、参数校验这类轻请求）
 *
 * ## 与线程池的公平性
 *
 * - 两侧用**同一个任务序列**（同一随机种子生成，含同样的计算量与让出次数）；
 * - 线程池把"让出"实现为 `std::this_thread::yield()`，Scheduler 用
 *   `yield_current()` —— 两者都是"主动放弃剩余时间片"的语义；
 * - 两侧线程数相同（`--workers`）。
 *
 * ## 已知限制（必须随数据一起读）
 *
 * `yield` 在这里是**协作让出的代理指标**，不是真实 IO 阻塞。真实 IO 等待
 * （fd 就绪）的成本由 io_epoll_fair_benchmark / high_load_stress 的 io-wait
 * 场景覆盖。本基准只回答"混合形状负载下调度开销如何"。
 */
#include "go2cpp/scheduler.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <numeric>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct Options final {
    std::size_t tasks{8000};
    std::size_t workers{8};
    std::size_t repeats{5};
    std::size_t compute_rounds{30000};
    std::uint64_t seed{20261017};
    int pure_percent{60};
    int yield_percent{30};
};

std::uint64_t ComputeKernel(std::uint64_t seed, std::size_t rounds) {
    std::uint64_t value = seed * 0x9E3779B97F4A7C15ULL + 0x123456789ABCDEFULL;
    for (std::size_t i = 0; i < rounds; ++i) {
        value ^= value >> 33;
        value *= 0xff51afd7ed558ccdULL;
        value ^= value >> 29;
        value += static_cast<std::uint64_t>(i) * 0x2545F4914F6CDD1DULL;
        value ^= value >> 32;
    }
    return value;
}

/** @brief 一个任务的可复现描述，两侧共用。 */
struct TaskSpec final {
    int kind{0};             // 0=pure, 1=yield, 2=short
    std::size_t rounds{0};
    int yields{0};
};

std::vector<TaskSpec> BuildSpecs(const Options& opt) {
    std::vector<TaskSpec> specs(opt.tasks);
    std::mt19937_64 rng(opt.seed);
    std::uniform_int_distribution<int> pick(1, 100);
    // 长度长尾：大部接近基准，少数只有 1/8 到 1/4
    std::uniform_int_distribution<std::size_t> tail_div(1, 8);
    std::uniform_int_distribution<int> yield_count(1, 3);
    for (auto& s : specs) {
        const int roll = pick(rng);
        if (roll <= opt.pure_percent) {
            s.kind = 0;
            s.rounds = opt.compute_rounds / tail_div(rng) + 1;
            s.yields = 0;
        } else if (roll <= opt.pure_percent + opt.yield_percent) {
            s.kind = 1;
            s.rounds = opt.compute_rounds / tail_div(rng) + 1;
            s.yields = yield_count(rng);
        } else {
            s.kind = 2;
            s.rounds = std::max<std::size_t>(1, opt.compute_rounds / 64);
            s.yields = 0;
        }
    }
    return specs;
}

struct Outcome final {
    std::vector<std::uint64_t> results;
    std::vector<double> task_us;
    double wall_ms{0.0};
    std::size_t completed{0};
    bool correct{false};
};

Outcome RunScheduler(const Options& opt, const std::vector<TaskSpec>& specs) {
    Outcome out;
    out.results.assign(specs.size(), 0);
    out.task_us.assign(specs.size(), 0.0);
    std::atomic<std::size_t> done{0};
    const auto start = Clock::now();

    {
        go2cpp::SchedulerConfig config;
        config.processor_count = opt.workers;
        config.min_workers = opt.workers;
        config.max_workers = opt.workers;
        go2cpp::Scheduler scheduler(config);
        scheduler.start();

        std::vector<std::shared_ptr<go2cpp::Task>> handles;
        handles.reserve(specs.size());
        for (std::size_t i = 0; i < specs.size(); ++i) {
            handles.emplace_back(scheduler.spawn([&, i] {
                const auto t0 = Clock::now();
                const auto& spec = specs[i];
                std::uint64_t acc = ComputeKernel(i + 1, spec.rounds);
                for (int y = 0; y < spec.yields; ++y) {
                    if (!scheduler.yield_current()) {
                        break;
                    }
                    acc ^= ComputeKernel(acc, spec.rounds / 8 + 1);
                }
                out.results[i] = acc;
                out.task_us[i] = std::chrono::duration<double, std::micro>(
                                     Clock::now() - t0).count();
                done.fetch_add(1, std::memory_order_release);
            }));
        }

        const auto deadline = start + std::chrono::seconds(240);
        while (done.load(std::memory_order_acquire) < specs.size()) {
            if (Clock::now() > deadline) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        // 注意：这里刻意**不**对每个 Task 调 wait_for —— done 计数已经保证
        // 所有任务体执行完毕，再逐个 wait_for 只会把 Task 状态机的等待时间
        // 计入 wall clock，污染"任务执行耗时"这一指标。
        (void)scheduler.shutdown_for(std::chrono::seconds(30));
    }

    out.wall_ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    out.completed = done.load(std::memory_order_acquire);
    out.correct = (out.completed == specs.size());
    return out;
}

class ThreadPool final {
public:
    explicit ThreadPool(std::size_t workers) {
        m_workers.reserve(workers);
        for (std::size_t i = 0; i < workers; ++i) {
            m_workers.emplace_back([this] { Loop(); });
        }
    }
    ~ThreadPool() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stopping = true;
        }
        m_cv.notify_all();
        for (auto& w : m_workers) {
            if (w.joinable()) {
                w.join();
            }
        }
    }
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    void Submit(std::function<void()> fn) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_tasks.push_back(std::move(fn));
        }
        m_cv.notify_one();
    }

private:
    void Loop() {
        for (;;) {
            std::function<void()> fn;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_cv.wait(lock, [this] { return m_stopping || !m_tasks.empty(); });
                if (m_tasks.empty()) {
                    if (m_stopping) {
                        return;
                    }
                    continue;
                }
                fn = std::move(m_tasks.front());
                m_tasks.pop_front();
            }
            fn();
        }
    }

    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<std::function<void()>> m_tasks;
    std::vector<std::thread> m_workers;
    bool m_stopping{false};
};

Outcome RunPool(const Options& opt, const std::vector<TaskSpec>& specs) {
    Outcome out;
    out.results.assign(specs.size(), 0);
    out.task_us.assign(specs.size(), 0.0);
    std::atomic<std::size_t> done{0};
    const auto start = Clock::now();

    {
        ThreadPool pool(opt.workers);
        for (std::size_t i = 0; i < specs.size(); ++i) {
            pool.Submit([&, i] {
                const auto t0 = Clock::now();
                const auto& spec = specs[i];
                std::uint64_t acc = ComputeKernel(i + 1, spec.rounds);
                for (int y = 0; y < spec.yields; ++y) {
                    std::this_thread::yield();
                    acc ^= ComputeKernel(acc, spec.rounds / 8 + 1);
                }
                out.results[i] = acc;
                out.task_us[i] = std::chrono::duration<double, std::micro>(
                                     Clock::now() - t0).count();
                done.fetch_add(1, std::memory_order_release);
            });
        }
        const auto deadline = start + std::chrono::seconds(240);
        while (done.load(std::memory_order_acquire) < specs.size()) {
            if (Clock::now() > deadline) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

    out.wall_ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    out.completed = done.load(std::memory_order_acquire);
    out.correct = (out.completed == specs.size());
    return out;
}

double Percentile(std::vector<double> v, double p) {
    if (v.empty()) {
        return 0.0;
    }
    std::sort(v.begin(), v.end());
    const double pos = p * static_cast<double>(v.size() - 1);
    const auto idx = static_cast<std::size_t>(pos);
    const double frac = pos - static_cast<double>(idx);
    if (idx + 1 >= v.size()) {
        return v.back();
    }
    return v[idx] * (1.0 - frac) + v[idx + 1] * frac;
}

void PrintOutcome(const char* model, std::size_t round, const Outcome& o) {
    std::printf("  %-10s #%zu wall=%9.2fms done=%zu/%zu %s p50=%8.1fus "
                "p90=%9.1f p99=%9.1f max=%10.1f\n",
                model, round, o.wall_ms, o.completed, o.results.size(),
                o.correct ? "ok" : "BAD", Percentile(o.task_us, 0.50),
                Percentile(o.task_us, 0.90), Percentile(o.task_us, 0.99),
                o.task_us.empty()
                    ? 0.0
                    : *std::max_element(o.task_us.begin(), o.task_us.end()));
}

Options ReadOptions(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&](const char* name) {
            if (i + 1 >= argc) {
                throw std::runtime_error(std::string("missing value for ") + name);
            }
            return std::string(argv[++i]);
        };
        if (arg == "--tasks") {
            opt.tasks = static_cast<std::size_t>(std::stoull(next("--tasks")));
        } else if (arg == "--workers") {
            opt.workers = static_cast<std::size_t>(std::stoull(next("--workers")));
        } else if (arg == "--repeats") {
            opt.repeats = static_cast<std::size_t>(std::stoull(next("--repeats")));
        } else if (arg == "--compute-rounds") {
            opt.compute_rounds = static_cast<std::size_t>(std::stoull(next("--compute-rounds")));
        } else if (arg == "--seed") {
            opt.seed = std::stoull(next("--seed"));
        } else if (arg == "--pure-percent") {
            opt.pure_percent = std::stoi(next("--pure-percent"));
        } else if (arg == "--yield-percent") {
            opt.yield_percent = std::stoi(next("--yield-percent"));
        } else if (arg == "--help" || arg == "-h") {
            std::printf(
                "用法: %s [选项]\n"
                "  --tasks N          任务数（默认 8000）\n"
                "  --workers N        两侧线程/P 数（默认 8）\n"
                "  --repeats N        计时轮数（默认 5）\n"
                "  --compute-rounds N 计算基准轮次（默认 30000）\n"
                "  --seed N           随机种子（默认 20261017）\n"
                "  --pure-percent N   纯计算任务占比（默认 60）\n"
                "  --yield-percent N  让出型任务占比（默认 30）\n"
                "                     剩下的是短任务\n",
                argv[0]);
            std::exit(0);
        } else {
            throw std::runtime_error("unknown option: " + arg);
        }
    }
    if (opt.workers == 0 || opt.tasks == 0) {
        throw std::runtime_error("workers/tasks must be positive");
    }
    if (opt.pure_percent < 0 || opt.yield_percent < 0 ||
        opt.pure_percent + opt.yield_percent > 100) {
        throw std::runtime_error("percent options out of range");
    }
    return opt;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const auto opt = ReadOptions(argc, argv);
        const auto specs = BuildSpecs(opt);

        std::size_t pure = 0, yields = 0, shorts = 0;
        for (const auto& s : specs) {
            pure += s.kind == 0 ? 1 : 0;
            yields += s.kind == 1 ? 1 : 0;
            shorts += s.kind == 2 ? 1 : 0;
        }
        std::printf("mixed_random_benchmark\n");
        std::printf("tasks=%zu workers=%zu repeats=%zu compute_rounds=%zu seed=%llu\n",
                    opt.tasks, opt.workers, opt.repeats, opt.compute_rounds,
                    static_cast<unsigned long long>(opt.seed));
        std::printf("任务分布: pure=%zu yield=%zu short=%zu（总 %zu）\n", pure, yields,
                    shorts, specs.size());

        // warmup
        (void)RunScheduler(opt, specs);
        (void)RunPool(opt, specs);

        std::vector<double> sched_walls, pool_walls;
        std::size_t bad = 0;
        for (std::size_t rep = 0; rep < opt.repeats; ++rep) {
            const bool pool_first = (rep % 2) == 1;
            if (pool_first) {
                auto p = RunPool(opt, specs);
                PrintOutcome("pool", rep + 1, p);
                bad += p.correct ? 0 : 1;
                pool_walls.push_back(p.wall_ms);
                auto s = RunScheduler(opt, specs);
                PrintOutcome("scheduler", rep + 1, s);
                bad += s.correct ? 0 : 1;
                sched_walls.push_back(s.wall_ms);
            } else {
                auto s = RunScheduler(opt, specs);
                PrintOutcome("scheduler", rep + 1, s);
                bad += s.correct ? 0 : 1;
                sched_walls.push_back(s.wall_ms);
                auto p = RunPool(opt, specs);
                PrintOutcome("pool", rep + 1, p);
                bad += p.correct ? 0 : 1;
                pool_walls.push_back(p.wall_ms);
            }
        }

        auto med = [](std::vector<double> v) {
            std::sort(v.begin(), v.end());
            const auto n = v.size();
            return n == 0 ? 0.0 : (n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0);
        };
        const double sm = med(sched_walls);
        const double pm = med(pool_walls);
        std::printf("\n汇总: scheduler_median=%.2fms pool_median=%.2fms "
                    "ratio_sched_over_pool=%.3f 失败轮次=%zu\n",
                    sm, pm, pm > 0.0 ? sm / pm : 0.0, bad);
        return bad == 0 ? 0 : 2;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "mixed_random_benchmark failed: %s\n", e.what());
        return 1;
    }
}
