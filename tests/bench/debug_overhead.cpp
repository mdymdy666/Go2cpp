/**
 * @file debug_overhead.cpp
 * @brief `GO2CPP_DEBUG_DUMP` 开/关的端到端开销基准（同一源码链接不同构建）。
 *
 * ## 为什么需要
 * 已有单事件成本（`docs/debug-reliability.md`：约 19.65–22.47 ns/事件）只回答
 * 「记录一条事件多少钱」，回答不了「真实业务场景整体慢多少」：事件记录点分布
 * 在 channel 交接、mutex 争用、spawn/resume、timer、资源历史等不同路径上，
 * 每类路径每次操作产生的事件数与额外分支数都不同。本基准用同一份程序分别
 * 链接 `GO2CPP_DEBUG_DUMP=OFF` 与 `ON` 的库，在独立进程中测量每个场景的
 * 吞吐、尾延迟与 RSS。
 *
 * ## 场景（每个场景可独立运行，N ≥ 10 万次操作）
 * 1. `channel`   —— 两个 Fiber 通过无缓冲 channel 来回交接 N 次（2N 次收发）；
 * 2. `mutex`     —— 4 个 OS 线程争用一把 `sync::Mutex`，共 N 次 lock/unlock；
 * 3. `spawn`     —— N 个 Fiber 的 spawn/resume/complete（每批 2048 个）；
 * 4. `timer`     —— N 个 0 延迟定时器 arm/fire/回调（每批 1024 个）；
 * 5. `resources` —— RWMutex 读写 + WaitGroup Add/Done + ConditionVariable
 *                   Wait/Notify（每 32 轮一次真实唤醒），共约 6N 次原语操作；
 * 6. `select`    —— 两个 channel 上的多路 select（N 次等待与收发）。
 *
 * ## 测量口径（为什么这样设计）
 * - **吞吐**用场景前后各一次 `steady_clock` 的墙钟时间计算，避免逐操作计时
 *   污染吞吐；**延迟**另跑一轮逐操作采样（每场景最多 `--lat-n` 次），因此
 *   p50/p99 含 `steady_clock::now()` 的固定成本（本机约 20–30 ns），这一点在
 *   文档中明确标注，比较的是「同一程序 + 同一计时器」下的相对开销。
 * - Channel/Select/Timer/Spawn 的延迟是**端到端**口径（spawn→完成、
 *   arm→回调），包含排队时间；Mutex/Resources 是单次操作的墙钟耗时。
 * - 事件量用 `EventRing::TotalCount*` 的**累计计数增量**统计，不用
 *   `ResetTotals()`（调度器后台线程可能并发写事件，清零不是安全操作）。
 *   OFF 构建的累计计数恒为 0。
 * - RSS 读 `/proc/self/status` 的 `VmRSS`（场景前后）与 `VmHWM`（进程峰值）。
 *   每个场景由脚本在独立进程中拉起，避免上一场景的栈池/分配器缓存污染。
 *
 * ## 输出
 * 一行 TSV 到 stdout（表头由 `run_debug_overhead.sh` 添加）：
 * `config scenario n ops wall_ns ops_per_sec p50_ns p99_ns p999_ns
 *  rss_base_kb rss_after_kb rss_peak_kb events events_per_op lat_ops threads
 *  debug_build event_detail`
 *
 * ## 用法
 * ```text
 * debug_overhead --scenario=channel --n=200000 --config=ON_FULL
 * debug_overhead --scenario=mutex --n=200000 --threads=4 --lat-n=100000
 * debug_overhead --list
 * ```
 *
 * ## 编译期宏
 * `channel.hpp` / `rwmutex.hpp` 的事件记录点在模板/头文件里，基准程序必须
 * 与所链接库使用**相同**的 `GO2CPP_DEBUG_DUMP` / 事件环容量 / `GO2CPP_REPLAY`
 * 宏，否则会测出「库记录、程序不记录」的混合配置。脚本从构建目录的
 * `CMakeCache.txt` 读取这些值并传给编译器。
 */

#include "go2cpp/concurrency/channel.hpp"
#include "go2cpp/concurrency/rwmutex.hpp"
#include "go2cpp/concurrency/scheduler.hpp"
#include "go2cpp/debug/debug_event_ring.hpp"
#include "go2cpp/sync.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

/// 防止编译器把任务体判定为死代码。
std::atomic<std::uint64_t> g_sink{0};

using Clock = std::chrono::steady_clock;

/// 当前单调时钟的纳秒值（仅同机可比）。
std::uint64_t NowNs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now().time_since_epoch())
            .count());
}

/**
 * @brief 读取 `/proc/self/status` 中某个以 `kB` 结尾的字段。
 * @param field 字段名（不带冒号），例如 "VmRSS"。
 * @return 数值（KB）；文件或字段不可用返回 -1。
 */
long ReadProcStatusKb(const char* field) {
    std::ifstream file("/proc/self/status");
    if (!file) {
        return -1;
    }
    const std::string prefix = std::string(field) + ":";
    std::string line;
    while (std::getline(file, line)) {
        if (line.rfind(prefix, 0) != 0) {
            continue;
        }
        const char* p = line.c_str() + prefix.size();
        while (*p != '\0' && (*p < '0' || *p > '9')) {
            ++p;
        }
        return std::strtol(p, nullptr, 10);
    }
    return -1;
}

/// 逐操作延迟收集器（单位 ns，饱和到 uint32）。
struct LatencyCollector {
    std::vector<std::uint32_t> samples;

    void Reserve(std::size_t n) { samples.reserve(n); }

    void AddNs(std::uint64_t ns) {
        if (ns > std::numeric_limits<std::uint32_t>::max()) {
            ns = std::numeric_limits<std::uint32_t>::max();
        }
        samples.push_back(static_cast<std::uint32_t>(ns));
    }

    void AddSpan(std::uint64_t begin_ns, std::uint64_t end_ns) {
        AddNs(end_ns > begin_ns ? end_ns - begin_ns : 0);
    }
};

struct LatStats {
    double p50{0.0};
    double p99{0.0};
    double p999{0.0};
    std::size_t count{0};
};

/// 排序后取分位数；空样本返回全 0。
LatStats Summarize(std::vector<std::uint32_t> samples) {
    LatStats stats;
    stats.count = samples.size();
    if (samples.empty()) {
        return stats;
    }
    std::sort(samples.begin(), samples.end());
    const auto pick = [&samples](double q) {
        const auto index = static_cast<std::size_t>(
            q * static_cast<double>(samples.size() - 1));
        return static_cast<double>(samples[index]);
    };
    stats.p50 = pick(0.50);
    stats.p99 = pick(0.99);
    stats.p999 = pick(0.999);
    return stats;
}

/// 事件计数快照（累计值；OFF 构建全部为 0）。
struct EventCounts {
    std::uint64_t total{0};
    std::uint64_t park{0};
    std::uint64_t wake{0};
    std::uint64_t ch_send{0};
    std::uint64_t ch_recv{0};
    std::uint64_t mutex_lock{0};
    std::uint64_t timer_arm{0};
    std::uint64_t timer_fire{0};
    std::uint64_t rw{0};
    std::uint64_t cv{0};
    std::uint64_t wg{0};
    std::uint64_t g_resumed{0};
    std::uint64_t g_created{0};
};

std::uint64_t Count(go2cpp::debug::SchedEventCode code) {
    return go2cpp::debug::EventRing::TotalCount(code);
}

EventCounts SnapshotEvents() {
    using go2cpp::debug::EventRing;
    using go2cpp::debug::SchedEventCode;
    EventCounts c;
    c.total = EventRing::TotalCountAll();
    c.park = Count(SchedEventCode::kParkEnter) +
             Count(SchedEventCode::kParkReturn);
    c.wake = Count(SchedEventCode::kWakeRequest) +
             Count(SchedEventCode::kWakeEnqueued) +
             Count(SchedEventCode::kWakePending);
    c.ch_send = Count(SchedEventCode::kChannelSend);
    c.ch_recv = Count(SchedEventCode::kChannelRecv);
    c.mutex_lock = Count(SchedEventCode::kMutexLock);
    c.timer_arm = Count(SchedEventCode::kTimerArm);
    c.timer_fire = Count(SchedEventCode::kTimerFire);
    c.rw = Count(SchedEventCode::kRwLockRead) +
           Count(SchedEventCode::kRwUnlockRead) +
           Count(SchedEventCode::kRwLockWrite) +
           Count(SchedEventCode::kRwUnlockWrite);
    c.cv = Count(SchedEventCode::kCvWait) +
           Count(SchedEventCode::kCvNotify) +
           Count(SchedEventCode::kCvNotifyAll);
    c.wg = Count(SchedEventCode::kWaitGroupAdd) +
           Count(SchedEventCode::kWaitGroupDone) +
           Count(SchedEventCode::kWaitGroupWait);
    c.g_resumed = Count(SchedEventCode::kGResumed);
    c.g_created = Count(SchedEventCode::kGCreated);
    return c;
}

/// 逐字段相减；`after` 必须来自同一进程的后一次快照。
EventCounts Delta(const EventCounts& before, const EventCounts& after) {
    EventCounts d;
    d.total = after.total - before.total;
    d.park = after.park - before.park;
    d.wake = after.wake - before.wake;
    d.ch_send = after.ch_send - before.ch_send;
    d.ch_recv = after.ch_recv - before.ch_recv;
    d.mutex_lock = after.mutex_lock - before.mutex_lock;
    d.timer_arm = after.timer_arm - before.timer_arm;
    d.timer_fire = after.timer_fire - before.timer_fire;
    d.rw = after.rw - before.rw;
    d.cv = after.cv - before.cv;
    d.wg = after.wg - before.wg;
    d.g_resumed = after.g_resumed - before.g_resumed;
    d.g_created = after.g_created - before.g_created;
    return d;
}

/// 只输出非零类别，避免 TSV 过长；全零（OFF 构建）输出 "-"。
std::string FormatEventDetail(const EventCounts& d) {
    std::ostringstream out;
    bool first = true;
    const auto add = [&out, &first](const char* name, std::uint64_t value) {
        if (value == 0) {
            return;
        }
        if (!first) {
            out << ',';
        }
        out << name << '=' << value;
        first = false;
    };
    add("park", d.park);
    add("wake", d.wake);
    add("chsend", d.ch_send);
    add("chrecv", d.ch_recv);
    add("mutex", d.mutex_lock);
    add("timerarm", d.timer_arm);
    add("timerfire", d.timer_fire);
    add("rw", d.rw);
    add("cv", d.cv);
    add("wg", d.wg);
    add("gresume", d.g_resumed);
    add("gcreate", d.g_created);
    if (first) {
        return "-";
    }
    return out.str();
}

/// 一次场景运行的测量结果（对应 TSV 一行）。
struct Measurement {
    std::string config;
    std::string scenario;
    std::uint64_t n{0};
    std::uint64_t ops{0};
    std::uint64_t lat_ops{0};
    std::uint64_t threads{0};
    double wall_ns{0.0};
    double ops_per_sec{0.0};
    double p50_ns{0.0};
    double p99_ns{0.0};
    double p999_ns{0.0};
    long rss_base_kb{-1};
    long rss_after_kb{-1};
    long rss_peak_kb{-1};
    std::uint64_t events{0};
    std::string event_detail{"-"};
    bool debug_build{false};
};

/// 场景入参。
struct Options {
    std::uint64_t n{200000};
    std::uint64_t lat_n{100000};
    std::uint64_t threads{4};
    std::string config{"UNKNOWN"};
    long rss_base_kb{-1};
};

Measurement MakeMeasurement(const Options& opt, const char* scenario,
                            std::uint64_t n, std::uint64_t ops,
                            std::uint64_t threads) {
    Measurement m;
    m.config = opt.config;
    m.scenario = scenario;
    m.n = n;
    m.ops = ops;
    m.threads = threads;
    m.rss_base_kb = opt.rss_base_kb;
    m.debug_build = go2cpp::debug::EventRing::Enabled();
    return m;
}

/// 把多线程采集到的样本合并成一个向量。
std::vector<std::uint32_t> MergeSamples(
    const std::vector<LatencyCollector>& collectors) {
    std::size_t total = 0;
    for (const auto& c : collectors) {
        total += c.samples.size();
    }
    std::vector<std::uint32_t> merged;
    merged.reserve(total);
    for (const auto& c : collectors) {
        merged.insert(merged.end(), c.samples.begin(), c.samples.end());
    }
    return merged;
}

// ---------------------------------------------------------------------------
// 场景 1：无缓冲 channel 的 Fiber ping-pong
// ---------------------------------------------------------------------------

Measurement RunChannel(const Options& opt) {
    Measurement m = MakeMeasurement(opt, "channel", opt.n, opt.n * 2, 2);
    go2cpp::Scheduler scheduler(2);
    scheduler.start();
    auto a = go2cpp::MakeChannel<int>(0);
    auto b = go2cpp::MakeChannel<int>(0);
    const std::uint64_t n = opt.n;

    const EventCounts ev0 = SnapshotEvents();
    const std::uint64_t t0 = NowNs();
    auto f1 = scheduler.spawn([&a, &b, n] {
        for (std::uint64_t i = 0; i < n; ++i) {
            (void)a->Recv();
            (void)b->Send(1);
        }
    });
    auto f2 = scheduler.spawn([&a, &b, n] {
        for (std::uint64_t i = 0; i < n; ++i) {
            (void)a->Send(1);
            (void)b->Recv();
        }
    });
    f1->wait();
    f2->wait();
    m.wall_ns = static_cast<double>(NowNs() - t0);
    const EventCounts ev1 = SnapshotEvents();
    m.rss_after_kb = ReadProcStatusKb("VmRSS");

    // 延迟轮：每次往返的时间除以 2，近似单次交接延迟。
    const std::uint64_t lat_n = std::min<std::uint64_t>(opt.lat_n, n);
    LatencyCollector la;
    LatencyCollector lb;
    la.Reserve(static_cast<std::size_t>(lat_n));
    lb.Reserve(static_cast<std::size_t>(lat_n));
    auto g1 = scheduler.spawn([&a, &b, lat_n, &la] {
        for (std::uint64_t i = 0; i < lat_n; ++i) {
            const std::uint64_t s = NowNs();
            (void)a->Recv();
            (void)b->Send(1);
            la.AddNs((NowNs() - s) / 2);
        }
    });
    auto g2 = scheduler.spawn([&a, &b, lat_n, &lb] {
        for (std::uint64_t i = 0; i < lat_n; ++i) {
            const std::uint64_t s = NowNs();
            (void)a->Send(1);
            (void)b->Recv();
            lb.AddNs((NowNs() - s) / 2);
        }
    });
    g1->wait();
    g2->wait();
    std::vector<LatencyCollector> collectors;
    collectors.push_back(std::move(la));
    collectors.push_back(std::move(lb));
    const LatStats stats = Summarize(MergeSamples(collectors));
    m.lat_ops = stats.count;
    m.p50_ns = stats.p50;
    m.p99_ns = stats.p99;
    m.p999_ns = stats.p999;

    scheduler.shutdown();
    m.rss_peak_kb = ReadProcStatusKb("VmHWM");
    const EventCounts delta = Delta(ev0, ev1);
    m.events = delta.total;
    m.event_detail = FormatEventDetail(delta);
    if (m.wall_ns > 0.0) {
        m.ops_per_sec =
            static_cast<double>(m.ops) * 1e9 / m.wall_ns;
    }
    return m;
}

// ---------------------------------------------------------------------------
// 场景 2：多线程争用 sync::Mutex
// ---------------------------------------------------------------------------

Measurement RunMutex(const Options& opt) {
    const std::uint64_t threads = opt.threads == 0 ? 4 : opt.threads;
    Measurement m = MakeMeasurement(opt, "mutex", opt.n, opt.n, threads);
    go2cpp::sync::Mutex mutex;
    std::uint64_t counter = 0;
    const std::uint64_t per = opt.n / threads;
    const std::uint64_t remainder = opt.n % threads;

    const EventCounts ev0 = SnapshotEvents();
    const std::uint64_t t0 = NowNs();
    {
        std::vector<std::thread> workers;
        workers.reserve(static_cast<std::size_t>(threads));
        for (std::uint64_t ti = 0; ti < threads; ++ti) {
            const std::uint64_t count = per + (ti < remainder ? 1 : 0);
            workers.emplace_back([&mutex, &counter, count] {
                for (std::uint64_t i = 0; i < count; ++i) {
                    (void)mutex.Lock();
                    ++counter;
                    mutex.Unlock();
                }
            });
        }
        for (auto& worker : workers) {
            worker.join();
        }
    }
    m.wall_ns = static_cast<double>(NowNs() - t0);
    const EventCounts ev1 = SnapshotEvents();
    m.rss_after_kb = ReadProcStatusKb("VmRSS");

    // 延迟轮：每个线程单独采样，最后合并。
    std::vector<LatencyCollector> collectors(
        static_cast<std::size_t>(threads));
    for (auto& c : collectors) {
        c.Reserve(static_cast<std::size_t>(per + 1));
    }
    {
        std::vector<std::thread> workers;
        workers.reserve(static_cast<std::size_t>(threads));
        for (std::uint64_t ti = 0; ti < threads; ++ti) {
            const std::uint64_t count = per + (ti < remainder ? 1 : 0);
            workers.emplace_back([&mutex, &counter, count, &collectors, ti] {
                LatencyCollector& lat = collectors[ti];
                for (std::uint64_t i = 0; i < count; ++i) {
                    const std::uint64_t s = NowNs();
                    (void)mutex.Lock();
                    ++counter;
                    mutex.Unlock();
                    lat.AddSpan(s, NowNs());
                }
            });
        }
        for (auto& worker : workers) {
            worker.join();
        }
    }
    const LatStats stats = Summarize(MergeSamples(collectors));
    m.lat_ops = stats.count;
    m.p50_ns = stats.p50;
    m.p99_ns = stats.p99;
    m.p999_ns = stats.p999;

    m.rss_peak_kb = ReadProcStatusKb("VmHWM");
    const EventCounts delta = Delta(ev0, ev1);
    m.events = delta.total;
    m.event_detail = FormatEventDetail(delta);
    if (m.wall_ns > 0.0) {
        m.ops_per_sec = static_cast<double>(m.ops) * 1e9 / m.wall_ns;
    }
    return m;
}

// ---------------------------------------------------------------------------
// 场景 3：Fiber spawn / resume / complete
// ---------------------------------------------------------------------------

Measurement RunSpawn(const Options& opt) {
    Measurement m = MakeMeasurement(opt, "spawn", opt.n, opt.n, 4);
    go2cpp::Scheduler scheduler(4);
    scheduler.start();
    constexpr std::size_t kBatch = 2048;

    const EventCounts ev0 = SnapshotEvents();
    const std::uint64_t t0 = NowNs();
    for (std::uint64_t done = 0; done < opt.n;) {
        const auto batch = static_cast<std::size_t>(
            std::min<std::uint64_t>(kBatch, opt.n - done));
        std::vector<std::shared_ptr<go2cpp::Task>> tasks;
        tasks.reserve(batch);
        for (std::size_t i = 0; i < batch; ++i) {
            tasks.push_back(scheduler.spawn([] {
                g_sink.fetch_add(1, std::memory_order_relaxed);
            }));
        }
        for (auto& task : tasks) {
            task->wait();
        }
        done += batch;
    }
    m.wall_ns = static_cast<double>(NowNs() - t0);
    const EventCounts ev1 = SnapshotEvents();
    m.rss_after_kb = ReadProcStatusKb("VmRSS");

    // 延迟轮：端到端口径（spawn 时刻 → 任务体执行时刻），含排队时间。
    const std::uint64_t lat_n = std::min<std::uint64_t>(opt.lat_n, opt.n);
    LatencyCollector lat;
    lat.Reserve(static_cast<std::size_t>(lat_n));
    for (std::uint64_t done = 0; done < lat_n;) {
        const auto batch = static_cast<std::size_t>(
            std::min<std::uint64_t>(kBatch, lat_n - done));
        std::vector<std::uint64_t> starts(batch);
        std::vector<std::uint64_t> ends(batch);
        std::vector<std::shared_ptr<go2cpp::Task>> tasks;
        tasks.reserve(batch);
        for (std::size_t i = 0; i < batch; ++i) {
            starts[i] = NowNs();
            tasks.push_back(scheduler.spawn([&ends, i] {
                ends[i] = NowNs();
            }));
        }
        for (auto& task : tasks) {
            task->wait();
        }
        for (std::size_t i = 0; i < batch; ++i) {
            lat.AddSpan(starts[i], ends[i]);
        }
        done += batch;
    }
    const LatStats stats = Summarize(std::move(lat.samples));
    m.lat_ops = stats.count;
    m.p50_ns = stats.p50;
    m.p99_ns = stats.p99;
    m.p999_ns = stats.p999;

    scheduler.shutdown();
    m.rss_peak_kb = ReadProcStatusKb("VmHWM");
    const EventCounts delta = Delta(ev0, ev1);
    m.events = delta.total;
    m.event_detail = FormatEventDetail(delta);
    if (m.wall_ns > 0.0) {
        m.ops_per_sec = static_cast<double>(m.ops) * 1e9 / m.wall_ns;
    }
    return m;
}

// ---------------------------------------------------------------------------
// 场景 4：定时器 arm / fire / 回调
// ---------------------------------------------------------------------------

Measurement RunTimer(const Options& opt) {
    Measurement m = MakeMeasurement(opt, "timer", opt.n, opt.n, 3);
    go2cpp::Scheduler scheduler(2);
    scheduler.start();
    constexpr std::size_t kBatch = 1024;

    const EventCounts ev0 = SnapshotEvents();
    const std::uint64_t t0 = NowNs();
    for (std::uint64_t done = 0; done < opt.n;) {
        const auto batch = static_cast<std::size_t>(
            std::min<std::uint64_t>(kBatch, opt.n - done));
        auto wg = std::make_shared<go2cpp::sync::WaitGroup>();
        wg->Add(static_cast<std::int64_t>(batch));
        std::vector<go2cpp::core::Timer> timers;
        timers.reserve(batch);
        for (std::size_t i = 0; i < batch; ++i) {
            timers.push_back(scheduler.schedule_after(
                std::chrono::nanoseconds(0), [wg] { wg->Done(); }));
        }
        // Wait 返回时计数归零，说明所有回调都已执行；此时 timers 可以安全析构。
        wg->Wait();
        done += batch;
    }
    m.wall_ns = static_cast<double>(NowNs() - t0);
    const EventCounts ev1 = SnapshotEvents();
    m.rss_after_kb = ReadProcStatusKb("VmRSS");

    // 延迟轮：arm 时刻 → 回调执行时刻（含计时器线程排队）。
    const std::uint64_t lat_n = std::min<std::uint64_t>(opt.lat_n, opt.n);
    LatencyCollector lat;
    lat.Reserve(static_cast<std::size_t>(lat_n));
    for (std::uint64_t done = 0; done < lat_n;) {
        const auto batch = static_cast<std::size_t>(
            std::min<std::uint64_t>(kBatch, lat_n - done));
        auto wg = std::make_shared<go2cpp::sync::WaitGroup>();
        wg->Add(static_cast<std::int64_t>(batch));
        std::vector<std::uint64_t> starts(batch);
        std::vector<std::uint64_t> ends(batch);
        std::vector<go2cpp::core::Timer> timers;
        timers.reserve(batch);
        for (std::size_t i = 0; i < batch; ++i) {
            starts[i] = NowNs();
            timers.push_back(scheduler.schedule_after(
                std::chrono::nanoseconds(0),
                [wg, &ends, i] {
                    ends[i] = NowNs();
                    wg->Done();
                }));
        }
        wg->Wait();
        for (std::size_t i = 0; i < batch; ++i) {
            lat.AddSpan(starts[i], ends[i]);
        }
        done += batch;
    }
    const LatStats stats = Summarize(std::move(lat.samples));
    m.lat_ops = stats.count;
    m.p50_ns = stats.p50;
    m.p99_ns = stats.p99;
    m.p999_ns = stats.p999;

    scheduler.shutdown();
    m.rss_peak_kb = ReadProcStatusKb("VmHWM");
    const EventCounts delta = Delta(ev0, ev1);
    m.events = delta.total;
    m.event_detail = FormatEventDetail(delta);
    if (m.wall_ns > 0.0) {
        m.ops_per_sec = static_cast<double>(m.ops) * 1e9 / m.wall_ns;
    }
    return m;
}

// ---------------------------------------------------------------------------
// 场景 5：资源历史（RWMutex + WaitGroup + ConditionVariable）
// ---------------------------------------------------------------------------

Measurement RunResources(const Options& opt) {
    // 每轮 6 个原语操作（RLock/RUnlock/Lock/Unlock/Add/Done），
    // 外加每 32 轮一次真实的 CV Notify（有等待者）。
    constexpr std::uint64_t kCvPeriod = 32;
    const std::uint64_t n = opt.n;
    const std::uint64_t ops = n * 6 + (n / kCvPeriod + 1);
    Measurement m = MakeMeasurement(opt, "resources", n, ops, 2);

    go2cpp::sync::RWMutex rw;
    go2cpp::sync::WaitGroup wg;
    go2cpp::sync::Mutex cv_mutex;
    go2cpp::sync::ConditionVariable cv;
    std::atomic<bool> stop{false};

    std::thread helper([&cv_mutex, &cv, &stop] {
        for (;;) {
            cv_mutex.Lock();
            if (stop.load(std::memory_order_relaxed)) {
                cv_mutex.Unlock();
                break;
            }
            // 等待主线程的 Notify；stop 检查与 Wait 都在锁内，避免丢唤醒。
            cv.Wait(cv_mutex);
            cv_mutex.Unlock();
        }
    });

    const auto body = [&](std::uint64_t count, LatencyCollector* latency) {
        for (std::uint64_t i = 0; i < count; ++i) {
            const std::uint64_t s = latency != nullptr ? NowNs() : 0;
            rw.RLock();
            rw.RUnlock();
            rw.Lock();
            rw.Unlock();
            wg.Add(1);
            wg.Done();
            if ((i % kCvPeriod) == 0) {
                cv_mutex.Lock();
                cv.NotifyOne();
                cv_mutex.Unlock();
            }
            if (latency != nullptr) {
                latency->AddSpan(s, NowNs());
            }
        }
    };

    const EventCounts ev0 = SnapshotEvents();
    const std::uint64_t t0 = NowNs();
    body(n, nullptr);
    m.wall_ns = static_cast<double>(NowNs() - t0);
    const EventCounts ev1 = SnapshotEvents();
    m.rss_after_kb = ReadProcStatusKb("VmRSS");

    LatencyCollector lat;
    lat.Reserve(static_cast<std::size_t>(
        std::min<std::uint64_t>(opt.lat_n, n)));
    body(std::min<std::uint64_t>(opt.lat_n, n), &lat);
    const LatStats stats = Summarize(std::move(lat.samples));
    m.lat_ops = stats.count;
    m.p50_ns = stats.p50;
    m.p99_ns = stats.p99;
    m.p999_ns = stats.p999;

    cv_mutex.Lock();
    stop.store(true, std::memory_order_relaxed);
    cv.NotifyAll();
    cv_mutex.Unlock();
    helper.join();

    m.rss_peak_kb = ReadProcStatusKb("VmHWM");
    const EventCounts delta = Delta(ev0, ev1);
    m.events = delta.total;
    m.event_detail = FormatEventDetail(delta);
    if (m.wall_ns > 0.0) {
        m.ops_per_sec = static_cast<double>(m.ops) * 1e9 / m.wall_ns;
    }
    return m;
}

// ---------------------------------------------------------------------------
// 场景 6：多路 select
// ---------------------------------------------------------------------------

Measurement RunSelect(const Options& opt) {
    Measurement m = MakeMeasurement(opt, "select", opt.n, opt.n, 2);
    go2cpp::Scheduler scheduler(2);
    scheduler.start();
    auto a = go2cpp::MakeChannel<int>(0);
    auto b = go2cpp::MakeChannel<int>(0);
    const std::uint64_t n = opt.n;

    const EventCounts ev0 = SnapshotEvents();
    const std::uint64_t t0 = NowNs();
    auto sender = scheduler.spawn([&a, n] {
        for (std::uint64_t i = 0; i < n; ++i) {
            (void)a->Send(1);
        }
    });
    auto receiver = scheduler.spawn([&a, &b, n] {
        for (std::uint64_t i = 0; i < n; ++i) {
            auto result = go2cpp::Select(
                {go2cpp::RecvCase(a), go2cpp::RecvCase(b)});
            (void)result;
        }
    });
    sender->wait();
    receiver->wait();
    m.wall_ns = static_cast<double>(NowNs() - t0);
    const EventCounts ev1 = SnapshotEvents();
    m.rss_after_kb = ReadProcStatusKb("VmRSS");

    const std::uint64_t lat_n = std::min<std::uint64_t>(opt.lat_n, n);
    LatencyCollector lat;
    lat.Reserve(static_cast<std::size_t>(lat_n));
    auto s2 = scheduler.spawn([&a, lat_n] {
        for (std::uint64_t i = 0; i < lat_n; ++i) {
            (void)a->Send(1);
        }
    });
    auto r2 = scheduler.spawn([&a, &b, lat_n, &lat] {
        for (std::uint64_t i = 0; i < lat_n; ++i) {
            const std::uint64_t s = NowNs();
            auto result = go2cpp::Select(
                {go2cpp::RecvCase(a), go2cpp::RecvCase(b)});
            (void)result;
            lat.AddSpan(s, NowNs());
        }
    });
    s2->wait();
    r2->wait();
    const LatStats stats = Summarize(std::move(lat.samples));
    m.lat_ops = stats.count;
    m.p50_ns = stats.p50;
    m.p99_ns = stats.p99;
    m.p999_ns = stats.p999;

    scheduler.shutdown();
    m.rss_peak_kb = ReadProcStatusKb("VmHWM");
    const EventCounts delta = Delta(ev0, ev1);
    m.events = delta.total;
    m.event_detail = FormatEventDetail(delta);
    if (m.wall_ns > 0.0) {
        m.ops_per_sec = static_cast<double>(m.ops) * 1e9 / m.wall_ns;
    }
    return m;
}

/// 输出一行 TSV（列顺序与脚本表头严格一致）。
void PrintMeasurement(const Measurement& m) {
    const double events_per_op =
        m.ops == 0 ? 0.0 : static_cast<double>(m.events) /
                               static_cast<double>(m.ops);
    std::printf(
        "%s\t%s\t%llu\t%llu\t%.0f\t%.1f\t%.1f\t%.1f\t%.1f\t%ld\t%ld\t%ld\t"
        "%llu\t%.4f\t%llu\t%llu\t%d\t%s\n",
        m.config.c_str(), m.scenario.c_str(),
        static_cast<unsigned long long>(m.n),
        static_cast<unsigned long long>(m.ops), m.wall_ns, m.ops_per_sec,
        m.p50_ns, m.p99_ns, m.p999_ns, m.rss_base_kb, m.rss_after_kb,
        m.rss_peak_kb, static_cast<unsigned long long>(m.events),
        events_per_op, static_cast<unsigned long long>(m.lat_ops),
        static_cast<unsigned long long>(m.threads), m.debug_build ? 1 : 0,
        m.event_detail.c_str());
}

void PrintUsage(const char* program) {
    std::printf(
        "用法: %s --scenario=<name> [--n=200000] [--lat-n=100000] "
        "[--threads=4] [--config=LABEL]\n"
        "场景: channel mutex spawn timer resources select\n"
        "  --list  列出场景名\n",
        program);
}

bool ParseU64(const char* text, std::uint64_t* out) {
    if (text == nullptr || *text == '\0') {
        return false;
    }
    char* end = nullptr;
    const unsigned long long value = std::strtoull(text, &end, 10);
    if (end == text || *end != '\0') {
        return false;
    }
    *out = static_cast<std::uint64_t>(value);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    std::string scenario;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--list") {
            std::printf("channel\nmutex\nspawn\ntimer\nresources\nselect\n");
            return 0;
        }
        if (arg.rfind("--scenario=", 0) == 0) {
            scenario = arg.substr(std::strlen("--scenario="));
        } else if (arg.rfind("--config=", 0) == 0) {
            opt.config = arg.substr(std::strlen("--config="));
        } else if (arg.rfind("--n=", 0) == 0) {
            if (!ParseU64(arg.c_str() + std::strlen("--n="), &opt.n)) {
                std::fprintf(stderr, "非法 --n: %s\n", arg.c_str());
                return 2;
            }
        } else if (arg.rfind("--lat-n=", 0) == 0) {
            if (!ParseU64(arg.c_str() + std::strlen("--lat-n="),
                          &opt.lat_n)) {
                std::fprintf(stderr, "非法 --lat-n: %s\n", arg.c_str());
                return 2;
            }
        } else if (arg.rfind("--threads=", 0) == 0) {
            if (!ParseU64(arg.c_str() + std::strlen("--threads="),
                          &opt.threads)) {
                std::fprintf(stderr, "非法 --threads: %s\n", arg.c_str());
                return 2;
            }
        } else if (arg == "--help" || arg == "-h") {
            PrintUsage(argv[0]);
            return 0;
        } else {
            std::fprintf(stderr, "未知参数: %s\n", arg.c_str());
            PrintUsage(argv[0]);
            return 2;
        }
    }
    if (scenario.empty()) {
        PrintUsage(argv[0]);
        return 2;
    }
    if (opt.n == 0) {
        opt.n = 200000;
    }
    if (opt.lat_n == 0) {
        opt.lat_n = std::min<std::uint64_t>(opt.n, 100000);
    }

    opt.rss_base_kb = ReadProcStatusKb("VmRSS");

    try {
        Measurement m;
        if (scenario == "channel") {
            m = RunChannel(opt);
        } else if (scenario == "mutex") {
            m = RunMutex(opt);
        } else if (scenario == "spawn") {
            m = RunSpawn(opt);
        } else if (scenario == "timer") {
            m = RunTimer(opt);
        } else if (scenario == "resources") {
            m = RunResources(opt);
        } else if (scenario == "select") {
            m = RunSelect(opt);
        } else {
            std::fprintf(stderr, "未知场景: %s\n", scenario.c_str());
            PrintUsage(argv[0]);
            return 2;
        }
        PrintMeasurement(m);
        std::fflush(stdout);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "场景 %s 失败: %s\n", scenario.c_str(),
                     error.what());
        return 1;
    }
    return 0;
}
