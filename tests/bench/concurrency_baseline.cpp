/**
 * @file concurrency_baseline.cpp
 * @brief 并发原语「调试监控层」开销基线：同一程序链接 OFF / ON 库，
 *        运行时用环境变量切换采样与全开，量化事件编码、资源注册表、
 *        累计计数、backtrace 与环形写的成本。
 *
 * ## 为什么需要
 *
 * 已有历史数据只覆盖原语本身（Mutex latch 18–19 ns、PC RTT 3.4 µs、
 * Channel cap0 1.19 µs），回答不了「打开调试监控后每个原语慢多少」。
 * 本基准把**被监控的原语操作**作为工作负载，比较五种监控粒度：
 *
 * | 配置 | 环境变量 | 含义 |
 * |---|---|---|
 * | OFF | 无（OFF 构建） | 宏完全消除，零记录 |
 * | SAMPLED | `park,sched` + shift=5 | 只看 park/sched，1/32 采样 |
 * | SAMPLED_ALL | `all` + shift=5 | 全部类别，1/32 采样（含资源事件） |
 * | FULL_NO_CAPTURE | 全类别但排除 stacks/chains，shift=0 | 全量事件编码，不抓栈/链 |
 * | FULL | `all` + shift=0 + stacks/chains=1 | 全量事件 + 每次 backtrace/链捕获 |
 *
 * 后两者用于把「事件编码 + 资源路径」与「backtrace / Fiber 链」拆开归因。
 *
 * ## 场景
 *
 * 1. `mutex4` / `mutex8`：4/8 个 OS 线程争用 `sync::Mutex`；
 * 2. `rwmix`：6 读 2 写线程混合访问 `RWMutex`；
 * 3. `mpmc`：4 生产者 4 消费者 Fiber 经 cap=64 channel 传 N 条消息；
 * 4. `pc_rtt`：两个 OS 线程用 `ParkingCondition` 做 notify→wait 往返；
 * 5. `wg`：4 个线程并发 `WaitGroup::Done` N 次，主线程 `Wait`；
 * 6. `select2` / `select4`：2/4 路 channel select N 次；
 * 7. `mixed`：2 个工作线程同时用 Mutex + Channel + ParkingCondition；
 * 8. 微基准 `record` / `record_sampled32` / `lookup` / `stack` / `config`：
 *    直接测单次事件编码、采样判断、资源查找、栈捕获与类别检查。
 *
 * ## 测量口径
 *
 * - 吞吐用场景前后各一次 `steady_clock` 的墙钟时间；延迟另跑一轮逐操作
 *   采样（每样本含 `now()` 约 20–30 ns 固定成本，只做同机相对比较）。
 * - 事件量用 `EventRing::TotalCount*` 的累计增量；同一进程内先跑吞吐轮再
 *   跑延迟轮，事件统计只覆盖吞吐轮。
 * - 「窗口覆盖倍数」= 吞吐轮事件数 / (活跃 ring 数 × 每线程容量)。>1 表示
 *   压测期间每个 ring 至少被覆盖一遍，历史事件必有丢失；这不是精确的
 *   dropped 计数（事件环没有该计数器），而是可复现的下界。
 * - `stack_samples` / `stack_dropped` 来自栈样本表（容量 4096）；
 *   `dropped_threads` 只在活跃线程超过 128 时非零。
 * - 每场景由脚本在独立进程中运行，避免上一场景的线程/栈池污染。
 *
 * ## 输出
 *
 * 一行 TSV 到 stdout（列顺序与 `run_concurrency_baseline.sh` 表头一致）：
 * `config scenario n ops threads wall_ns ops_per_sec p50_ns p99_ns p999_ns
 *  lat_ops events events_per_op event_detail window_capacity window_coverage
 *  snapshot_events stack_samples stack_dropped dropped_threads rss_base_kb
 *  rss_after_kb rss_peak_kb debug_build`
 *
 * ## 用法
 * ```text
 * concurrency_baseline --scenario=mutex8 --n=100000 --config=FULL
 * concurrency_baseline --scenario=mpmc --n=50000 --lat-n=20000
 * concurrency_baseline --list
 * ```
 *
 * ## 边界
 *
 * - 不修改 `src/`，不 commit；结果只用于相对比较，绝对值随机器变化。
 * - OFF 构建的 `EventRing` 接口仍可调用，但计数恒 0、`ThreadCount()` 为 0。
 */

#include "go2cpp/concurrency/channel.hpp"
#include "go2cpp/concurrency/core/parking_condition.hpp"
#include "go2cpp/concurrency/locks.hpp"
#include "go2cpp/concurrency/scheduler.hpp"
#include "go2cpp/debug/debug_config.hpp"
#include "go2cpp/debug/debug_event_ring.hpp"
#include "go2cpp/debug/debug_resources.hpp"
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
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

/// 防止编译器把场景体判定为死代码。
std::atomic<std::uint64_t> g_sink{0};

using Clock = std::chrono::steady_clock;

std::uint64_t NowNs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now().time_since_epoch())
            .count());
}

/// 读取 `/proc/self/status` 的 `VmRSS` / `VmHWM`（KB）；失败返回 -1。
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

/// 逐操作延迟收集器（ns，饱和到 uint32）。
struct LatencyCollector {
    std::vector<std::uint32_t> samples;
    void Reserve(std::size_t n) { samples.reserve(n); }
    void AddNs(std::uint64_t ns) {
        samples.push_back(static_cast<std::uint32_t>(
            std::min<std::uint64_t>(
                ns, std::numeric_limits<std::uint32_t>::max())));
    }
    void AddSpan(std::uint64_t begin, std::uint64_t end) {
        AddNs(end > begin ? end - begin : 0);
    }
};

struct LatStats {
    double p50{0.0};
    double p99{0.0};
    double p999{0.0};
    std::size_t count{0};
};

LatStats Summarize(std::vector<std::uint32_t> samples) {
    LatStats stats;
    stats.count = samples.size();
    if (samples.empty()) {
        return stats;
    }
    std::sort(samples.begin(), samples.end());
    const auto pick = [&samples](double q) {
        return static_cast<double>(samples[static_cast<std::size_t>(
            q * static_cast<double>(samples.size() - 1))]);
    };
    stats.p50 = pick(0.50);
    stats.p99 = pick(0.99);
    stats.p999 = pick(0.999);
    return stats;
}

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

/// 完成门：工作线程跑完业务后停在 `WaitRelease()`，主线程读事件环后放行。
/// 这样 ring 在统计时仍然存活（线程退出会摘除 ring）。
struct Gate {
    std::atomic<int> arrived{0};
    std::atomic<bool> released{false};

    void Arrive() { arrived.fetch_add(1, std::memory_order_acq_rel); }
    void WaitAll(int n) {
        while (arrived.load(std::memory_order_acquire) < n) {
            std::this_thread::yield();
        }
    }
    void WaitRelease() {
        while (!released.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    }
    void Release() { released.store(true, std::memory_order_release); }
};

/// 原子地记录「最晚完成时刻」，避免用 join 时间污染墙钟。
void UpdateMax(std::atomic<std::uint64_t>* target, std::uint64_t value) {
    std::uint64_t current = target->load(std::memory_order_relaxed);
    while (value > current &&
           !target->compare_exchange_weak(current, value,
                                          std::memory_order_relaxed)) {
    }
}

// ---------------------------------------------------------------------------
// 事件统计
// ---------------------------------------------------------------------------

/// 按原语归类的累计事件数（0 表示 OFF 构建或该类别未开启）。
struct EventCounts {
    std::uint64_t total{0};
    std::uint64_t park{0};
    std::uint64_t wake{0};
    std::uint64_t sched{0};
    std::uint64_t mutex{0};
    std::uint64_t rw{0};
    std::uint64_t cv{0};
    std::uint64_t channel{0};
    std::uint64_t pc{0};
    std::uint64_t wg{0};
    std::uint64_t lifecycle{0};
    std::uint64_t timer{0};
    std::uint64_t other{0};
};

std::uint64_t Count(go2cpp::debug::SchedEventCode code) {
    return go2cpp::debug::EventRing::TotalCount(code);
}

EventCounts SnapshotEvents() {
    using go2cpp::debug::SchedEventCode;
    EventCounts c;
    c.total = go2cpp::debug::EventRing::TotalCountAll();
    c.park = Count(SchedEventCode::kParkEnter) +
             Count(SchedEventCode::kParkReturn) +
             Count(SchedEventCode::kParkRejected) +
             Count(SchedEventCode::kParkCaptured);
    c.wake = Count(SchedEventCode::kWakeRequest) +
             Count(SchedEventCode::kWakeEnqueued) +
             Count(SchedEventCode::kWakePending) +
             Count(SchedEventCode::kWakeRejected) +
             Count(SchedEventCode::kWakeBatch);
    c.sched = Count(SchedEventCode::kEnqueue) +
              Count(SchedEventCode::kEnqueueRejected) +
              Count(SchedEventCode::kRequeueWorker) +
              Count(SchedEventCode::kRequeueProcessor) +
              Count(SchedEventCode::kRunQueuePush) +
              Count(SchedEventCode::kRunQueuePop) +
              Count(SchedEventCode::kRunQueueClaimOk) +
              Count(SchedEventCode::kRunQueueClaimFail) +
              Count(SchedEventCode::kStealOk) +
              Count(SchedEventCode::kStealFail) +
              Count(SchedEventCode::kWorkerPark) +
              Count(SchedEventCode::kWorkerUnpark) +
              Count(SchedEventCode::kSysmonPass);
    c.mutex = Count(SchedEventCode::kMutexLock) +
              Count(SchedEventCode::kMutexUnlock) +
              Count(SchedEventCode::kMutexFastLock);
    c.rw = Count(SchedEventCode::kRwLockRead) +
           Count(SchedEventCode::kRwUnlockRead) +
           Count(SchedEventCode::kRwLockWrite) +
           Count(SchedEventCode::kRwUnlockWrite);
    c.cv = Count(SchedEventCode::kCvWait) +
           Count(SchedEventCode::kCvNotify) +
           Count(SchedEventCode::kCvNotifyAll);
    c.channel = Count(SchedEventCode::kChannelSend) +
                Count(SchedEventCode::kChannelRecv) +
                Count(SchedEventCode::kChannelWake) +
                Count(SchedEventCode::kChannelSendOk) +
                Count(SchedEventCode::kChannelRecvOk) +
                Count(SchedEventCode::kChannelSelectEnter) +
                Count(SchedEventCode::kChannelSelect);
    c.pc = Count(SchedEventCode::kPCWait) +
           Count(SchedEventCode::kPCNotify) +
           Count(SchedEventCode::kPCNotifyAll);
    c.wg = Count(SchedEventCode::kWaitGroupWait) +
           Count(SchedEventCode::kWaitGroupDone) +
           Count(SchedEventCode::kWaitGroupAdd);
    c.lifecycle = Count(SchedEventCode::kSchedulerStart) +
                  Count(SchedEventCode::kSchedulerStop) +
                  Count(SchedEventCode::kThreadStart) +
                  Count(SchedEventCode::kThreadStop) +
                  Count(SchedEventCode::kGCreated) +
                  Count(SchedEventCode::kGStarted) +
                  Count(SchedEventCode::kGResumed) +
                  Count(SchedEventCode::kGCompleted) +
                  Count(SchedEventCode::kGFailed) +
                  Count(SchedEventCode::kGCancelled);
    c.timer = Count(SchedEventCode::kTimerArm) +
              Count(SchedEventCode::kTimerFire) +
              Count(SchedEventCode::kTimerCancel) +
              Count(SchedEventCode::kTimerReschedule) +
              Count(SchedEventCode::kTimerEnqueue) +
              Count(SchedEventCode::kTimerCallback);
    const std::uint64_t accounted = c.park + c.wake + c.sched + c.mutex +
                                    c.rw + c.cv + c.channel + c.pc + c.wg +
                                    c.lifecycle + c.timer;
    c.other = c.total > accounted ? c.total - accounted : 0;
    return c;
}

EventCounts Delta(const EventCounts& before, const EventCounts& after) {
    EventCounts d;
    d.total = after.total - before.total;
    d.park = after.park - before.park;
    d.wake = after.wake - before.wake;
    d.sched = after.sched - before.sched;
    d.mutex = after.mutex - before.mutex;
    d.rw = after.rw - before.rw;
    d.cv = after.cv - before.cv;
    d.channel = after.channel - before.channel;
    d.pc = after.pc - before.pc;
    d.wg = after.wg - before.wg;
    d.lifecycle = after.lifecycle - before.lifecycle;
    d.timer = after.timer - before.timer;
    d.other = after.other - before.other;
    return d;
}

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
    add("sched", d.sched);
    add("mutex", d.mutex);
    add("rw", d.rw);
    add("cv", d.cv);
    add("ch", d.channel);
    add("pc", d.pc);
    add("wg", d.wg);
    add("life", d.lifecycle);
    add("timer", d.timer);
    add("other", d.other);
    return first ? "-" : out.str();
}

// ---------------------------------------------------------------------------
// 测量结果
// ---------------------------------------------------------------------------

struct Measurement {
    std::string config;
    std::string scenario;
    std::uint64_t n{0};
    std::uint64_t ops{0};
    std::uint64_t threads{0};
    double wall_ns{0.0};
    double ops_per_sec{0.0};
    double p50_ns{0.0};
    double p99_ns{0.0};
    double p999_ns{0.0};
    std::uint64_t lat_ops{0};
    std::uint64_t events{0};
    std::string event_detail{"-"};
    std::uint64_t window_capacity{0};
    double window_coverage{0.0};
    std::uint64_t snapshot_events{0};
    std::uint64_t stack_samples{0};
    std::uint64_t stack_dropped{0};
    std::uint64_t dropped_threads{0};
    long rss_base_kb{-1};
    long rss_after_kb{-1};
    long rss_peak_kb{-1};
    bool debug_build{false};
};

struct Options {
    std::string config{"UNKNOWN"};
    std::string scenario;
    std::uint64_t n{100000};
    std::uint64_t lat_n{20000};
    std::uint64_t threads{8};
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

/// 吞吐轮结束后填充事件/窗口/RSS 指标（必须在工作线程仍存活时调用）。
void FinishEvents(Measurement* m, const EventCounts& before) {
    const EventCounts after = SnapshotEvents();
    const EventCounts d = Delta(before, after);
    m->events = d.total;
    m->event_detail = FormatEventDetail(d);
    m->window_capacity =
        static_cast<std::uint64_t>(go2cpp::debug::EventRing::ThreadCount()) *
        go2cpp::debug::EventRing::kTotalCapacity;
    m->window_coverage =
        m->window_capacity == 0
            ? 0.0
            : static_cast<double>(d.total) /
                  static_cast<double>(m->window_capacity);
    m->snapshot_events =
        static_cast<std::uint64_t>(go2cpp::debug::EventRing::Snapshot().size());
    m->stack_samples =
        static_cast<std::uint64_t>(go2cpp::debug::StackSampleCount());
    m->stack_dropped = go2cpp::debug::StackSampleDropped();
    m->dropped_threads = go2cpp::debug::EventRing::DroppedThreads();
    m->rss_after_kb = ReadProcStatusKb("VmRSS");
}

void FinishLatency(Measurement* m, const std::vector<LatencyCollector>& cs) {
    const LatStats stats = Summarize(MergeSamples(cs));
    m->lat_ops = stats.count;
    m->p50_ns = stats.p50;
    m->p99_ns = stats.p99;
    m->p999_ns = stats.p999;
}

void FinishThroughput(Measurement* m, std::uint64_t t0) {
    const double wall = static_cast<double>(NowNs() - t0);
    m->wall_ns = wall;
    if (wall > 0.0) {
        m->ops_per_sec = static_cast<double>(m->ops) * 1e9 / wall;
    }
}

// ---------------------------------------------------------------------------
// 场景 1/2：sync::Mutex 4/8 线程争用
// ---------------------------------------------------------------------------

Measurement RunMutex(const Options& opt, std::uint64_t threads) {
    const char* name = threads >= 8 ? "mutex8" : "mutex4";
    Measurement m = MakeMeasurement(opt, name, opt.n, opt.n, threads);
    go2cpp::sync::Mutex mutex;
    std::uint64_t counter = 0;
    const std::uint64_t per = opt.n / threads;
    const std::uint64_t extra = opt.n % threads;
    Gate gate;
    std::atomic<std::uint64_t> finish{0};

    EventCounts ev0 = SnapshotEvents();
    const std::uint64_t t0 = NowNs();
    std::vector<std::thread> workers;
    workers.reserve(static_cast<std::size_t>(threads));
    for (std::uint64_t ti = 0; ti < threads; ++ti) {
        const std::uint64_t count = per + (ti < extra ? 1 : 0);
        workers.emplace_back([&, count] {
            for (std::uint64_t i = 0; i < count; ++i) {
                mutex.Lock();
                ++counter;
                mutex.Unlock();
            }
            UpdateMax(&finish, NowNs());
            gate.Arrive();
            gate.WaitRelease();
        });
    }
    gate.WaitAll(static_cast<int>(threads));
    FinishThroughput(&m, t0);
    FinishEvents(&m, ev0);
    gate.Release();
    for (auto& worker : workers) {
        worker.join();
    }

    // 延迟轮：每线程独立采样 Lock→Unlock 的墙钟。
    std::vector<LatencyCollector> collectors(
        static_cast<std::size_t>(threads));
    for (auto& c : collectors) {
        c.Reserve(static_cast<std::size_t>(per + 1));
    }
    {
        std::vector<std::thread> lat_workers;
        lat_workers.reserve(static_cast<std::size_t>(threads));
        for (std::uint64_t ti = 0; ti < threads; ++ti) {
            const std::uint64_t count = per + (ti < extra ? 1 : 0);
            lat_workers.emplace_back([&, ti, count] {
                LatencyCollector& lat = collectors[ti];
                for (std::uint64_t i = 0; i < count; ++i) {
                    const std::uint64_t s = NowNs();
                    mutex.Lock();
                    ++counter;
                    mutex.Unlock();
                    lat.AddSpan(s, NowNs());
                }
            });
        }
        for (auto& worker : lat_workers) {
            worker.join();
        }
    }
    FinishLatency(&m, collectors);
    m.rss_peak_kb = ReadProcStatusKb("VmHWM");
    return m;
}

// ---------------------------------------------------------------------------
// 场景 3：RWMutex 读多写少（6 读 2 写）
// ---------------------------------------------------------------------------

Measurement RunRwMix(const Options& opt) {
    constexpr std::uint64_t kReaders = 6;
    constexpr std::uint64_t kWriter = 2;
    constexpr std::uint64_t kThreads = kReaders + kWriter;
    Measurement m = MakeMeasurement(opt, "rwmix", opt.n, opt.n, kThreads);
    go2cpp::sync::RWMutex rw;
    std::uint64_t sink = 0;
    const std::uint64_t per = opt.n / kThreads;
    const std::uint64_t extra = opt.n % kThreads;
    Gate gate;
    std::atomic<std::uint64_t> finish{0};

    EventCounts ev0 = SnapshotEvents();
    const std::uint64_t t0 = NowNs();
    std::vector<std::thread> workers;
    workers.reserve(static_cast<std::size_t>(kThreads));
    for (std::uint64_t ti = 0; ti < kThreads; ++ti) {
        const std::uint64_t count = per + (ti < extra ? 1 : 0);
        const bool writer = ti >= kReaders;
        workers.emplace_back([&, writer, count] {
            for (std::uint64_t i = 0; i < count; ++i) {
                if (writer) {
                    rw.Lock();
                    ++sink;
                    rw.Unlock();
                } else {
                    rw.RLock();
                    sink += 0;
                    rw.RUnlock();
                }
            }
            UpdateMax(&finish, NowNs());
            gate.Arrive();
            gate.WaitRelease();
        });
    }
    gate.WaitAll(static_cast<int>(kThreads));
    FinishThroughput(&m, t0);
    FinishEvents(&m, ev0);
    gate.Release();
    for (auto& worker : workers) {
        worker.join();
    }

    std::vector<LatencyCollector> collectors(
        static_cast<std::size_t>(kThreads));
    for (auto& c : collectors) {
        c.Reserve(static_cast<std::size_t>(per + 1));
    }
    {
        std::vector<std::thread> lat_workers;
        lat_workers.reserve(static_cast<std::size_t>(kThreads));
        for (std::uint64_t ti = 0; ti < kThreads; ++ti) {
            const std::uint64_t count = per + (ti < extra ? 1 : 0);
            const bool writer = ti >= kReaders;
            lat_workers.emplace_back([&, ti, writer, count] {
                LatencyCollector& lat = collectors[ti];
                for (std::uint64_t i = 0; i < count; ++i) {
                    const std::uint64_t s = NowNs();
                    if (writer) {
                        rw.Lock();
                        ++sink;
                        rw.Unlock();
                    } else {
                        rw.RLock();
                        sink += 0;
                        rw.RUnlock();
                    }
                    lat.AddSpan(s, NowNs());
                }
            });
        }
        for (auto& worker : lat_workers) {
            worker.join();
        }
    }
    FinishLatency(&m, collectors);
    g_sink.fetch_add(sink, std::memory_order_relaxed);
    m.rss_peak_kb = ReadProcStatusKb("VmHWM");
    return m;
}

// ---------------------------------------------------------------------------
// 场景 4：Channel MPMC（4 生产者 + 4 消费者，cap=64）
// ---------------------------------------------------------------------------

Measurement RunMpmc(const Options& opt) {
    constexpr std::uint64_t kProducers = 4;
    constexpr std::uint64_t kConsumers = 4;
    constexpr std::uint64_t kCap = 64;
    Measurement m = MakeMeasurement(opt, "mpmc", opt.n, opt.n * 2,
                                    kProducers + kConsumers);
    go2cpp::Scheduler scheduler(
        static_cast<std::size_t>(kProducers + kConsumers));
    scheduler.start();
    auto channel = go2cpp::MakeChannel<int>(kCap);
    const std::uint64_t per_p = opt.n / kProducers;
    const std::uint64_t extra_p = opt.n % kProducers;
    const std::uint64_t per_c = opt.n / kConsumers;
    const std::uint64_t extra_c = opt.n % kConsumers;

    EventCounts ev0 = SnapshotEvents();
    const std::uint64_t t0 = NowNs();
    std::vector<std::shared_ptr<go2cpp::Task>> tasks;
    tasks.reserve(static_cast<std::size_t>(kProducers + kConsumers));
    for (std::uint64_t p = 0; p < kProducers; ++p) {
        const std::uint64_t count = per_p + (p < extra_p ? 1 : 0);
        tasks.push_back(scheduler.spawn([channel, count] {
            for (std::uint64_t i = 0; i < count; ++i) {
                (void)channel->Send(static_cast<int>(i));
            }
        }));
    }
    for (std::uint64_t c = 0; c < kConsumers; ++c) {
        const std::uint64_t count = per_c + (c < extra_c ? 1 : 0);
        tasks.push_back(scheduler.spawn([channel, count] {
            for (std::uint64_t i = 0; i < count; ++i) {
                (void)channel->Recv();
            }
        }));
    }
    for (auto& task : tasks) {
        task->wait();
    }
    FinishThroughput(&m, t0);
    // Scheduler 的 worker ring 在 shutdown 前仍存活，先采样再关闭。
    FinishEvents(&m, ev0);
    scheduler.shutdown();

    // 延迟轮：Send/Recv 各采样一份，合并后反映单次交接成本。
    std::vector<LatencyCollector> collectors(
        static_cast<std::size_t>(kProducers + kConsumers));
    for (auto& c : collectors) {
        c.Reserve(static_cast<std::size_t>(opt.n / 4 + 1));
    }
    const std::uint64_t lat_n = std::min<std::uint64_t>(opt.lat_n, opt.n);
    const std::uint64_t lat_per_p = lat_n / kProducers;
    const std::uint64_t lat_extra_p = lat_n % kProducers;
    const std::uint64_t lat_per_c = lat_n / kConsumers;
    const std::uint64_t lat_extra_c = lat_n % kConsumers;
    go2cpp::Scheduler lat_sched(
        static_cast<std::size_t>(kProducers + kConsumers));
    lat_sched.start();
    auto lat_channel = go2cpp::MakeChannel<int>(kCap);
    std::vector<std::shared_ptr<go2cpp::Task>> lat_tasks;
    for (std::uint64_t p = 0; p < kProducers; ++p) {
        const std::uint64_t count = lat_per_p + (p < lat_extra_p ? 1 : 0);
        lat_tasks.push_back(
            lat_sched.spawn([lat_channel, count, &collectors, p] {
                LatencyCollector& lat = collectors[p];
                for (std::uint64_t i = 0; i < count; ++i) {
                    const std::uint64_t s = NowNs();
                    (void)lat_channel->Send(static_cast<int>(i));
                    lat.AddSpan(s, NowNs());
                }
            }));
    }
    for (std::uint64_t c = 0; c < kConsumers; ++c) {
        const std::uint64_t count = lat_per_c + (c < lat_extra_c ? 1 : 0);
        lat_tasks.push_back(
            lat_sched.spawn([lat_channel, count, &collectors, c] {
                LatencyCollector& lat = collectors[kProducers + c];
                for (std::uint64_t i = 0; i < count; ++i) {
                    const std::uint64_t s = NowNs();
                    (void)lat_channel->Recv();
                    lat.AddSpan(s, NowNs());
                }
            }));
    }
    for (auto& task : lat_tasks) {
        task->wait();
    }
    lat_sched.shutdown();
    FinishLatency(&m, collectors);
    m.rss_peak_kb = ReadProcStatusKb("VmHWM");
    return m;
}

// ---------------------------------------------------------------------------
// 场景 5：ParkingCondition notify→wait 往返（2 个 OS 线程）
// ---------------------------------------------------------------------------

Measurement RunPcRtt(const Options& opt) {
    // Fiber 路径：两个 G 用 ParkingCondition + std::mutex 做 ping-pong。
    // 普通线程路径在 WSL2 上被 futex 唤醒延迟（~78 µs/RTT）主导，监测
    // 开销几乎不可见；fiber 路径的 RTT 在微秒级，才能暴露事件成本。
    Measurement m = MakeMeasurement(opt, "pc_rtt", opt.n, opt.n * 2, 2);
    go2cpp::Scheduler scheduler(2);
    scheduler.start();
    go2cpp::core::ParkingCondition pc;
    std::mutex mutex;
    std::uint64_t flag = 0;

    EventCounts ev0 = SnapshotEvents();
    const std::uint64_t t0 = NowNs();
    auto responder = scheduler.spawn([&] {
        for (std::uint64_t i = 0; i < opt.n; ++i) {
            {
                std::unique_lock<std::mutex> lock(mutex);
                pc.wait(lock, [&flag] { return flag == 1; });
                flag = 0;
            }
            pc.notify_one();
        }
    });
    auto requester = scheduler.spawn([&] {
        for (std::uint64_t i = 0; i < opt.n; ++i) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                flag = 1;
            }
            pc.notify_one();
            std::unique_lock<std::mutex> lock(mutex);
            pc.wait(lock, [&flag] { return flag == 0; });
        }
    });
    responder->wait();
    requester->wait();
    FinishThroughput(&m, t0);
    FinishEvents(&m, ev0);
    scheduler.shutdown();

    // 延迟轮：requester 一端测完整往返（notify → 对端 wait 返回 → 再 notify）。
    LatencyCollector lat;
    const std::uint64_t lat_n = std::min<std::uint64_t>(opt.lat_n, opt.n);
    lat.Reserve(static_cast<std::size_t>(lat_n));
    go2cpp::Scheduler lat_sched(2);
    lat_sched.start();
    std::uint64_t lat_flag = 0;
    auto lat_responder = lat_sched.spawn([&] {
        for (std::uint64_t i = 0; i < lat_n; ++i) {
            {
                std::unique_lock<std::mutex> lock(mutex);
                pc.wait(lock, [&lat_flag] { return lat_flag == 1; });
                lat_flag = 0;
            }
            pc.notify_one();
        }
    });
    auto lat_requester = lat_sched.spawn([&] {
        for (std::uint64_t i = 0; i < lat_n; ++i) {
            const std::uint64_t s = NowNs();
            {
                std::lock_guard<std::mutex> lock(mutex);
                lat_flag = 1;
            }
            pc.notify_one();
            std::unique_lock<std::mutex> lock(mutex);
            pc.wait(lock, [&lat_flag] { return lat_flag == 0; });
            lat.AddSpan(s, NowNs());
        }
    });
    lat_responder->wait();
    lat_requester->wait();
    lat_sched.shutdown();
    std::vector<LatencyCollector> collectors;
    collectors.push_back(std::move(lat));
    FinishLatency(&m, collectors);
    m.rss_peak_kb = ReadProcStatusKb("VmHWM");
    return m;
}

// ---------------------------------------------------------------------------
// 场景 6：WaitGroup 并发 Add/Done + 一次 Wait
// ---------------------------------------------------------------------------

Measurement RunWaitGroup(const Options& opt) {
    constexpr std::uint64_t kThreads = 4;
    Measurement m = MakeMeasurement(opt, "wg", opt.n, opt.n + 2, kThreads);
    go2cpp::sync::WaitGroup wg;
    const std::uint64_t per = opt.n / kThreads;
    const std::uint64_t extra = opt.n % kThreads;
    Gate gate;
    std::atomic<std::uint64_t> finish{0};

    EventCounts ev0 = SnapshotEvents();
    const std::uint64_t t0 = NowNs();
    wg.Add(static_cast<std::int64_t>(opt.n));
    std::vector<std::thread> workers;
    workers.reserve(static_cast<std::size_t>(kThreads));
    for (std::uint64_t ti = 0; ti < kThreads; ++ti) {
        const std::uint64_t count = per + (ti < extra ? 1 : 0);
        workers.emplace_back([&, count] {
            for (std::uint64_t i = 0; i < count; ++i) {
                wg.Done();
            }
            UpdateMax(&finish, NowNs());
            gate.Arrive();
            gate.WaitRelease();
        });
    }
    const bool waited = wg.Wait();
    FinishThroughput(&m, t0);
    FinishEvents(&m, ev0);
    gate.Release();
    for (auto& worker : workers) {
        worker.join();
    }
    if (!waited) {
        std::fprintf(stderr, "WaitGroup::Wait 未归零\n");
    }

    // 延迟轮：每个 Done 的墙钟（含计数争用）。
    std::vector<LatencyCollector> collectors(
        static_cast<std::size_t>(kThreads));
    for (auto& c : collectors) {
        c.Reserve(static_cast<std::size_t>(per + 1));
    }
    wg.Add(static_cast<std::int64_t>(opt.n));
    {
        std::vector<std::thread> lat_workers;
        lat_workers.reserve(static_cast<std::size_t>(kThreads));
        for (std::uint64_t ti = 0; ti < kThreads; ++ti) {
            const std::uint64_t count = per + (ti < extra ? 1 : 0);
            lat_workers.emplace_back([&, ti, count] {
                LatencyCollector& lat = collectors[ti];
                for (std::uint64_t i = 0; i < count; ++i) {
                    const std::uint64_t s = NowNs();
                    wg.Done();
                    lat.AddSpan(s, NowNs());
                }
            });
        }
        const bool lat_waited = wg.Wait();
        for (auto& worker : lat_workers) {
            worker.join();
        }
        if (!lat_waited) {
            std::fprintf(stderr, "WaitGroup 延迟轮未归零\n");
        }
    }
    FinishLatency(&m, collectors);
    m.rss_peak_kb = ReadProcStatusKb("VmHWM");
    return m;
}

// ---------------------------------------------------------------------------
// 场景 7：select 2/4 路
// ---------------------------------------------------------------------------

Measurement RunSelect(const Options& opt, std::uint64_t ways) {
    const char* name = ways >= 4 ? "select4" : "select2";
    Measurement m = MakeMeasurement(opt, name, opt.n, opt.n, ways + 1);
    go2cpp::Scheduler scheduler(4);
    scheduler.start();
    std::vector<go2cpp::ChannelPtr<int>> channels;
    channels.reserve(static_cast<std::size_t>(ways));
    for (std::uint64_t i = 0; i < ways; ++i) {
        channels.push_back(go2cpp::MakeChannel<int>(0));
    }

    EventCounts ev0 = SnapshotEvents();
    const std::uint64_t t0 = NowNs();
    const std::uint64_t n = opt.n;
    auto sender = scheduler.spawn([channels, ways, n] {
        for (std::uint64_t i = 0; i < n; ++i) {
            (void)channels[static_cast<std::size_t>(i % ways)]->Send(1);
        }
    });
    auto receiver = scheduler.spawn([channels, ways, n] {
        for (std::uint64_t i = 0; i < n; ++i) {
            std::vector<go2cpp::SelectCase> cases;
            cases.reserve(static_cast<std::size_t>(ways));
            for (const auto& channel : channels) {
                cases.push_back(go2cpp::RecvCase(channel));
            }
            auto result = go2cpp::Select(std::move(cases));
            (void)result;
        }
    });
    sender->wait();
    receiver->wait();
    FinishThroughput(&m, t0);
    FinishEvents(&m, ev0);
    scheduler.shutdown();

    // 延迟轮：每次 Select 调用。
    LatencyCollector lat;
    const std::uint64_t lat_n = std::min<std::uint64_t>(opt.lat_n, opt.n);
    lat.Reserve(static_cast<std::size_t>(lat_n));
    go2cpp::Scheduler lat_sched(4);
    lat_sched.start();
    auto lat_sender = lat_sched.spawn([channels, ways, lat_n] {
        for (std::uint64_t i = 0; i < lat_n; ++i) {
            (void)channels[static_cast<std::size_t>(i % ways)]->Send(1);
        }
    });
    auto lat_receiver = lat_sched.spawn([channels, ways, lat_n, &lat] {
        for (std::uint64_t i = 0; i < lat_n; ++i) {
            std::vector<go2cpp::SelectCase> cases;
            cases.reserve(static_cast<std::size_t>(ways));
            for (const auto& channel : channels) {
                cases.push_back(go2cpp::RecvCase(channel));
            }
            const std::uint64_t s = NowNs();
            auto result = go2cpp::Select(std::move(cases));
            (void)result;
            lat.AddSpan(s, NowNs());
        }
    });
    lat_sender->wait();
    lat_receiver->wait();
    lat_sched.shutdown();
    std::vector<LatencyCollector> collectors;
    collectors.push_back(std::move(lat));
    FinishLatency(&m, collectors);
    m.rss_peak_kb = ReadProcStatusKb("VmHWM");
    return m;
}

// ---------------------------------------------------------------------------
// 场景 8：Mutex + Channel + ParkingCondition 混合
// ---------------------------------------------------------------------------

Measurement RunMixed(const Options& opt) {
    // 用 Fiber 路径模拟业务混合负载：两个 worker fiber 同时走
    // Mutex → Channel → ParkingCondition，consumer/helper 各一个 fiber。
    // 这样 Channel 的等待走 Scheduler park 路径，避免普通线程 Channel
    // 在极高频率交接下依赖 futex 唤醒（该路径的唤醒丢失风险另行记录）。
    constexpr std::uint64_t kWorkers = 2;
    constexpr std::uint64_t kCap = 64;
    Measurement m = MakeMeasurement(opt, "mixed", opt.n, opt.n * 6,
                                    kWorkers + 2);
    go2cpp::Scheduler scheduler(4);
    scheduler.start();
    go2cpp::sync::Mutex mutex;
    go2cpp::core::ParkingCondition pc;
    std::mutex pc_mutex;
    std::uint64_t flag = 0;
    std::uint64_t counter = 0;
    auto channel = go2cpp::MakeChannel<int>(kCap);
    std::atomic<bool> stop{false};
    const std::uint64_t per = opt.n / kWorkers;
    const std::uint64_t extra = opt.n % kWorkers;

    EventCounts ev0 = SnapshotEvents();
    const std::uint64_t t0 = NowNs();
    auto consumer = scheduler.spawn([&] {
        for (std::uint64_t i = 0; i < opt.n; ++i) {
            (void)channel->Recv();
        }
    });
    auto helper = scheduler.spawn([&] {
        std::uint64_t seen = 0;
        for (;;) {
            std::unique_lock<std::mutex> lock(pc_mutex);
            pc.wait(lock, [&] { return flag != seen || stop.load(); });
            seen = flag;
            if (stop.load()) {
                break;
            }
        }
    });
    std::vector<std::shared_ptr<go2cpp::Task>> workers;
    workers.reserve(static_cast<std::size_t>(kWorkers));
    for (std::uint64_t ti = 0; ti < kWorkers; ++ti) {
        const std::uint64_t count = per + (ti < extra ? 1 : 0);
        workers.push_back(scheduler.spawn([&, count] {
            for (std::uint64_t i = 0; i < count; ++i) {
                mutex.Lock();
                ++counter;
                mutex.Unlock();
                (void)channel->Send(static_cast<int>(i));
                {
                    std::lock_guard<std::mutex> lock(pc_mutex);
                    ++flag;
                }
                pc.notify_one();
            }
        }));
    }
    for (auto& worker : workers) {
        worker->wait();
    }
    consumer->wait();
    FinishThroughput(&m, t0);
    FinishEvents(&m, ev0);
    {
        std::lock_guard<std::mutex> lock(pc_mutex);
        stop.store(true);
    }
    pc.notify_all();
    helper->wait();
    scheduler.shutdown();

    // 延迟轮：每轮端到端（锁 → channel 发送 → PC notify），2 个 worker
    // fiber 各采样一半。
    const std::uint64_t lat_n = std::min<std::uint64_t>(opt.lat_n, opt.n);
    const std::uint64_t lat_per = lat_n / kWorkers;
    const std::uint64_t lat_extra = lat_n % kWorkers;
    std::vector<LatencyCollector> collectors(
        static_cast<std::size_t>(kWorkers));
    for (auto& c : collectors) {
        c.Reserve(static_cast<std::size_t>(lat_per + 1));
    }
    go2cpp::Scheduler lat_sched(4);
    lat_sched.start();
    std::atomic<bool> lat_stop{false};
    std::uint64_t lat_flag = 0;
    auto lat_consumer = lat_sched.spawn([&] {
        for (std::uint64_t i = 0; i < lat_n; ++i) {
            (void)channel->Recv();
        }
    });
    auto lat_helper = lat_sched.spawn([&] {
        std::uint64_t seen = 0;
        for (;;) {
            std::unique_lock<std::mutex> lock(pc_mutex);
            pc.wait(lock, [&] { return lat_flag != seen || lat_stop.load(); });
            seen = lat_flag;
            if (lat_stop.load()) {
                break;
            }
        }
    });
    std::vector<std::shared_ptr<go2cpp::Task>> lat_workers;
    lat_workers.reserve(static_cast<std::size_t>(kWorkers));
    for (std::uint64_t ti = 0; ti < kWorkers; ++ti) {
        const std::uint64_t count = lat_per + (ti < lat_extra ? 1 : 0);
        lat_workers.push_back(lat_sched.spawn([&, ti, count] {
            LatencyCollector& lat = collectors[ti];
            for (std::uint64_t i = 0; i < count; ++i) {
                const std::uint64_t s = NowNs();
                mutex.Lock();
                ++counter;
                mutex.Unlock();
                (void)channel->Send(static_cast<int>(i));
                {
                    std::lock_guard<std::mutex> lock(pc_mutex);
                    ++lat_flag;
                }
                pc.notify_one();
                lat.AddSpan(s, NowNs());
            }
        }));
    }
    for (auto& worker : lat_workers) {
        worker->wait();
    }
    lat_consumer->wait();
    {
        std::lock_guard<std::mutex> lock(pc_mutex);
        lat_stop.store(true);
    }
    pc.notify_all();
    lat_helper->wait();
    lat_sched.shutdown();
    FinishLatency(&m, collectors);
    g_sink.fetch_add(counter, std::memory_order_relaxed);
    m.rss_peak_kb = ReadProcStatusKb("VmHWM");
    return m;
}

// ---------------------------------------------------------------------------
// 微基准：单事件编码 / 采样判断 / 资源查找 / 栈捕获 / 类别检查
// ---------------------------------------------------------------------------

Measurement RunMicro(const Options& opt, const std::string& kind) {
    Measurement m = MakeMeasurement(opt, kind.c_str(), opt.n, opt.n, 1);
    EventCounts ev0 = SnapshotEvents();
    std::uint64_t sink = 0;
    const std::uint64_t t0 = NowNs();
    if (kind == "record") {
        for (std::uint64_t i = 0; i < opt.n; ++i) {
            go2cpp::debug::EventRing::Record(
                go2cpp::debug::SchedEventCode::kMutexLock, 0, i);
        }
    } else if (kind == "record_sampled32") {
        for (std::uint64_t i = 0; i < opt.n; ++i) {
            go2cpp::debug::EventRing::RecordSampled(
                go2cpp::debug::SchedEventCode::kMutexLock, 0, i, 32);
        }
    } else if (kind == "lookup") {
        go2cpp::sync::Mutex mutex;
        for (std::uint64_t i = 0; i < opt.n; ++i) {
            sink += go2cpp::debug::FindResourceByAddress(&mutex);
        }
    } else if (kind == "stack") {
        for (std::uint64_t i = 0; i < opt.n; ++i) {
            sink += go2cpp::debug::CaptureStackSample(0, false);
        }
    } else if (kind == "config") {
        for (std::uint64_t i = 0; i < opt.n; ++i) {
            sink += go2cpp::debug::DebugConfig::Enabled(
                go2cpp::debug::DebugCategory::kPrimitives);
        }
    } else {
        std::fprintf(stderr, "未知微基准: %s\n", kind.c_str());
        std::exit(2);
    }
    FinishThroughput(&m, t0);
    FinishEvents(&m, ev0);
    g_sink.fetch_add(sink, std::memory_order_relaxed);
    m.rss_peak_kb = ReadProcStatusKb("VmHWM");
    return m;
}

// ---------------------------------------------------------------------------
// 输出与入口
// ---------------------------------------------------------------------------

void PrintMeasurement(const Measurement& m) {
    const double events_per_op =
        m.ops == 0 ? 0.0
                   : static_cast<double>(m.events) /
                         static_cast<double>(m.ops);
    std::printf(
        "%s\t%s\t%llu\t%llu\t%llu\t%.0f\t%.1f\t%.1f\t%.1f\t%.1f\t%llu\t"
        "%llu\t%.4f\t%s\t%llu\t%.3f\t%llu\t%llu\t%llu\t%llu\t%ld\t%ld\t%ld\t"
        "%d\n",
        m.config.c_str(), m.scenario.c_str(),
        static_cast<unsigned long long>(m.n),
        static_cast<unsigned long long>(m.ops),
        static_cast<unsigned long long>(m.threads), m.wall_ns, m.ops_per_sec,
        m.p50_ns, m.p99_ns, m.p999_ns,
        static_cast<unsigned long long>(m.lat_ops),
        static_cast<unsigned long long>(m.events), events_per_op,
        m.event_detail.c_str(),
        static_cast<unsigned long long>(m.window_capacity),
        m.window_coverage,
        static_cast<unsigned long long>(m.snapshot_events),
        static_cast<unsigned long long>(m.stack_samples),
        static_cast<unsigned long long>(m.stack_dropped),
        static_cast<unsigned long long>(m.dropped_threads), m.rss_base_kb,
        m.rss_after_kb, m.rss_peak_kb, m.debug_build ? 1 : 0);
}

void PrintUsage(const char* program) {
    std::printf(
        "用法: %s --scenario=<name> [--n=100000] [--lat-n=20000] "
        "[--config=LABEL] [--threads=8]\n"
        "场景: mutex4 mutex8 rwmix mpmc pc_rtt wg select2 select4 mixed "
        "record record_sampled32 lookup stack config\n"
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
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--list") {
            std::printf(
                "mutex4\nmutex8\nrwmix\nmpmc\npc_rtt\nwg\nselect2\nselect4\n"
                "mixed\nrecord\nrecord_sampled32\nlookup\nstack\nconfig\n");
            return 0;
        }
        if (arg.rfind("--scenario=", 0) == 0) {
            opt.scenario = arg.substr(std::strlen("--scenario="));
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
    if (opt.scenario.empty()) {
        PrintUsage(argv[0]);
        return 2;
    }
    if (opt.n == 0) {
        opt.n = 100000;
    }
    if (opt.lat_n == 0) {
        opt.lat_n = std::min<std::uint64_t>(opt.n, 20000);
    }
    opt.rss_base_kb = ReadProcStatusKb("VmRSS");

    try {
        Measurement m;
        if (opt.scenario == "mutex4") {
            m = RunMutex(opt, 4);
        } else if (opt.scenario == "mutex8") {
            m = RunMutex(opt, 8);
        } else if (opt.scenario == "rwmix") {
            m = RunRwMix(opt);
        } else if (opt.scenario == "mpmc") {
            m = RunMpmc(opt);
        } else if (opt.scenario == "pc_rtt") {
            m = RunPcRtt(opt);
        } else if (opt.scenario == "wg") {
            m = RunWaitGroup(opt);
        } else if (opt.scenario == "select2") {
            m = RunSelect(opt, 2);
        } else if (opt.scenario == "select4") {
            m = RunSelect(opt, 4);
        } else if (opt.scenario == "mixed") {
            m = RunMixed(opt);
        } else if (opt.scenario == "record" ||
                   opt.scenario == "record_sampled32" ||
                   opt.scenario == "lookup" || opt.scenario == "stack" ||
                   opt.scenario == "config") {
            m = RunMicro(opt, opt.scenario);
        } else {
            std::fprintf(stderr, "未知场景: %s\n", opt.scenario.c_str());
            PrintUsage(argv[0]);
            return 2;
        }
        PrintMeasurement(m);
        std::fflush(stdout);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "场景 %s 失败: %s\n", opt.scenario.c_str(),
                     error.what());
        return 1;
    }
    return 0;
}
