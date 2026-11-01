/**
 * @file pools.cpp
 * @brief 各池化资源的启动开销探针实现。
 *
 * ## 组织方式
 *
 * 每个探针只做一件事：在**干净的子进程**里触发一个池，读操作前后的内存
 * 与耗时。之所以每个池一个进程：
 *
 * - `TaskBlockPool` / `FiberBin` / `WaitNode` 池是进程级单例或 TLS，
 *   一旦初始化就不会重新付费，同进程内无法重复测「首次」；
 * - 内存增量要能相减，前一个池留下的堆缓存会污染后一个池的读数。
 *
 * ## 惰性池的时间口径
 *
 * `TaskBlockPool`、`FiberBin`、`WaitNode` 池都在**首次使用时**才分配，
 * 无法从外部把「池初始化」与「首次任务执行」剥开。对它们：
 *
 * - `init_ns` = 首次使用的端到端耗时（诚实标注为不可剥离）；
 * - `steady_ns` = 同一进程内第二次使用的耗时；
 * - 两者之差就是一次性初始化成本的上界（`steady_ns` 也可能含首次未覆盖的
 *   冷路径，所以是上界而不是精确值）。
 */
#include "pools.hpp"

#include "go2cpp/app/stream_table.hpp"
#include "go2cpp/cache/lru_k.hpp"
#include "go2cpp/concurrency/channel.hpp"
#include "go2cpp/fiber.hpp"
#include "go2cpp/media/jitter_buffer.hpp"
#include "go2cpp/media/sfu.hpp"
#include "go2cpp/os/pool.hpp"
#include "go2cpp/scheduler.hpp"
#include "go2cpp/sync.hpp"

// LSM Arena 目前是 storage 模块的私有实现头（src/storage/arena.hpp），
// 没有对外导出。基准作为一个树内工具直接引用它，构建脚本额外加
// `-Isrc/storage`；这样不需要为了让基准能测而改动核心的头文件布局。
#include "arena.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace startup_cost {
namespace {

/// @brief 探针统一使用的调度器规模（贴近典型 4 核 Web 服务）。
constexpr std::size_t kWorkers = 4;

TscClock& MutableClock() {
    static TscClock clock;
    return clock;
}

inline std::uint64_t Now() { return MutableClock().NowNs(); }

void SleepMicros(std::uint64_t micros) {
    timespec ts{};
    ts.tv_sec = static_cast<time_t>(micros / 1'000'000ULL);
    ts.tv_nsec = static_cast<long>((micros % 1'000'000ULL) * 1000ULL);
    ::nanosleep(&ts, nullptr);
}

/// @brief 自旋等待谓词成立；返回 false 表示超时。
bool SpinWaitUntil(const std::function<bool()>& predicate,
                   std::uint64_t timeout_ns) {
    const std::uint64_t deadline = MonotonicNs() + timeout_ns;
    while (!predicate()) {
        if (MonotonicNs() >= deadline) {
            return false;
        }
        std::this_thread::yield();
    }
    return true;
}

go2cpp::SchedulerConfig MakeSchedulerConfig(std::size_t workers) {
    go2cpp::SchedulerConfig config;
    config.processor_count = workers;
    config.max_workers = workers;
    return config;
}

/// @brief 一个已启动的 Scheduler；析构时关停。
class Fixture {
public:
    explicit Fixture(std::size_t workers = kWorkers)
        : scheduler_(MakeSchedulerConfig(workers)) {
        scheduler_.start();
        // start() 返回时 worker 可能还在登记中；等它们全部就绪，避免把
        // 线程启动时间算进「首次 spawn」。
        (void)SpinWaitUntil(
            [this, workers] { return scheduler_.worker_count() >= workers; },
            2'000'000'000ULL);
        SleepMicros(200);
    }

    ~Fixture() { scheduler_.shutdown(); }

    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    go2cpp::Scheduler& scheduler() { return scheduler_; }

private:
    go2cpp::Scheduler scheduler_;
};

void FillDelta(ProbeResult& result, const MemSnapshot& before,
               const MemSnapshot& after) {
    result.rss_kib = static_cast<std::int64_t>(after.rss_kib) -
                     static_cast<std::int64_t>(before.rss_kib);
    result.vm_kib = static_cast<std::int64_t>(after.vm_kib) -
                    static_cast<std::int64_t>(before.vm_kib);
    result.hwm_kib = static_cast<std::int64_t>(after.hwm_kib) -
                     static_cast<std::int64_t>(before.hwm_kib);
    result.heap_bytes = static_cast<std::int64_t>(after.heap_bytes) -
                        static_cast<std::int64_t>(before.heap_bytes);
    result.mmap_bytes = static_cast<std::int64_t>(after.mmap_bytes) -
                        static_cast<std::int64_t>(before.mmap_bytes);
}

void FinishTiming(ProbeResult& result, std::vector<double> samples) {
    if (samples.empty()) {
        return;
    }
    result.init_ns = static_cast<std::uint64_t>(Median(samples));
    result.use_ns = result.init_ns;
    result.min_ns = static_cast<std::uint64_t>(
        *std::min_element(samples.begin(), samples.end()));
    result.max_ns = static_cast<std::uint64_t>(
        *std::max_element(samples.begin(), samples.end()));
    result.p95_ns = static_cast<std::uint64_t>(Percentile(samples, 0.95));
    result.samples = samples.size();
}

go2cpp::cache::CacheOptions CacheOptions() {
    go2cpp::cache::CacheOptions options;
    options.capacity = 1024;
    options.max_weight = 0;
    options.seed = 0x243F6A8885A308D3ULL;
    options.fetch_cost = 1.0;
    return options;
}

// ---------------------------------------------------------------------------
// 惰性池：只能触发一次
// ---------------------------------------------------------------------------

/// @brief Task 共享块池：首次 spawn 触发 45 KiB chunk + 96 槽 TLS 缓存。
ProbeResult ProbeTaskBlockPool() {
    ProbeResult result;
    Fixture fixture(kWorkers);
    const MemSnapshot before = Capture();

    std::atomic<bool> first_done{false};
    const std::uint64_t first_begin = Now();
    fixture.scheduler().spawn([&first_done] {
        first_done.store(true, std::memory_order_release);
    });
    const bool first_ok = SpinWaitUntil(
        [&first_done] { return first_done.load(std::memory_order_acquire); },
        10'000'000'000ULL);
    const std::uint64_t first_end = Now();
    const MemSnapshot after_first = Capture();

    std::atomic<bool> second_done{false};
    const std::uint64_t second_begin = Now();
    fixture.scheduler().spawn([&second_done] {
        second_done.store(true, std::memory_order_release);
    });
    const bool second_ok = SpinWaitUntil(
        [&second_done] { return second_done.load(std::memory_order_acquire); },
        10'000'000'000ULL);
    const std::uint64_t second_end = Now();

    result.init_ns = first_end - first_begin;
    result.use_ns = result.init_ns;
    result.steady_ns = second_end - second_begin;
    result.min_ns = result.init_ns;
    result.p95_ns = result.init_ns;
    result.max_ns = result.init_ns;
    result.samples = 1;
    FillDelta(result, before, after_first);
    result.ok = first_ok && second_ok;
    if (!result.ok) {
        result.error = "spawn 未在 10s 内完成";
    }
    return result;
}

/// @brief FiberBin + StackArena：首次 Fiber 创建 + 首次 resume 触发 8 槽 arena。
/// @note Fiber 的构造函数**不分配栈**：栈在第一次 `resume()` 的
///       `initialize_context()` 里才向 arena 申请。只构造不 resume 只能测到
///       Fiber 对象本身（约 1 KiB），测不到栈池 —— 这是实现细节，不是基准
///       口径问题，所以这里显式 resume 一次。
ProbeResult ProbeFiberBin(ProbeMode mode, std::size_t repeat) {
    ProbeResult result;
    if (mode == ProbeMode::kTime) {
        std::vector<double> samples;
        samples.reserve(repeat);
        for (std::size_t i = 0; i < repeat; ++i) {
            const std::uint64_t begin = Now();
            {
                go2cpp::Fiber fiber([] {}, 4096);
                fiber.resume();
            }
            samples.push_back(static_cast<double>(Now() - begin));
        }
        FinishTiming(result, std::move(samples));
        result.ok = true;
        return result;
    }

    const MemSnapshot before = Capture();
    std::uint64_t first_ns = 0;
    std::uint64_t second_ns = 0;
    {
        const std::uint64_t begin = Now();
        auto first = std::make_unique<go2cpp::Fiber>([] {}, 4096);
        first->resume();
        first_ns = Now() - begin;

        const std::uint64_t second_begin = Now();
        auto second = std::make_unique<go2cpp::Fiber>([] {}, 4096);
        second->resume();
        second_ns = Now() - second_begin;

        const MemSnapshot after = Capture();
        FillDelta(result, before, after);
    }
    result.init_ns = first_ns;
    result.use_ns = first_ns;
    result.steady_ns = second_ns;
    result.min_ns = first_ns;
    result.p95_ns = first_ns;
    result.max_ns = first_ns;
    result.samples = 1;
    result.ok = true;
    return result;
}

/// @brief sync WaitNode TLS 池：一次真实竞争（fiber 侧 park）触发节点分配。
///
/// 只报内存：这个池的「初始化」就是一次 `make_shared<WaitNode>`（约 200 B）
/// 加上一次 park。park 期间等待持锁方释放的时间远大于分配本身，从外部
/// 无法把两者剥离；硬报一个「耗时」只会得到被 sleep 窗口污染的数。
/// 节点是否真的分配，用 Δheap 证明。
///
/// 等待「已经 park」用 `Task::state() == kWaiting` 精确判定，而不是 sleep
/// 一个固定窗口：sleep 太短会在 waiter 还没进队列时就 Unlock（那时它根本
/// 不会分配 WaitNode），太长则把观察时间算进测量。
ProbeResult ProbeWaitNodePool() {
    ProbeResult result;
    Fixture fixture(2);
    go2cpp::sync::Mutex mutex;
    mutex.Lock();  // 主线程（非 fiber）持锁：走 native 路径，不建 WaitNode

    std::atomic<bool> finished{false};
    const MemSnapshot before = Capture();

    auto waiter = fixture.scheduler().spawn([&] {
        mutex.Lock();  // 竞争：AcquireWaitNode → 入队 → park
        mutex.Unlock();
        finished.store(true, std::memory_order_release);
    });
    (void)SpinWaitUntil(
        [&waiter] {
            return waiter->state() == go2cpp::scheduler::GState::kWaiting;
        },
        5'000'000'000ULL);
    const MemSnapshot parked = Capture();
    mutex.Unlock();  // 唤醒 waiter
    const bool ok = SpinWaitUntil(
        [&finished] { return finished.load(std::memory_order_acquire); },
        10'000'000'000ULL);

    FillDelta(result, before, parked);
    result.ok = ok;
    if (!result.ok) {
        result.error = "park/wake 未在 10s 内完成";
    }
    return result;
}

/// @brief channel 自己的 WaitNode 池（128 槽 TLS，独立于 sync.cpp 那套）。
/// @note 与 sync 版同理：只报内存，不报被等待时间污染的「耗时」。
ProbeResult ProbeChannelWaitNode() {
    ProbeResult result;
    Fixture fixture(2);
    auto channel = go2cpp::Channel<int>::Create(0);

    std::atomic<bool> recv_finished{false};
    const MemSnapshot before = Capture();

    auto receiver = fixture.scheduler().spawn([&] {
        auto received = channel->Recv();  // 无缓冲：阻塞 → channel WaitNode
        (void)received;
        recv_finished.store(true, std::memory_order_release);
    });
    (void)SpinWaitUntil(
        [&receiver] {
            return receiver->state() == go2cpp::scheduler::GState::kWaiting;
        },
        5'000'000'000ULL);
    const MemSnapshot parked = Capture();
    fixture.scheduler().spawn([&channel] {
        auto sent = channel->Send(1);
        (void)sent;
    });
    const bool ok = SpinWaitUntil(
        [&recv_finished] {
            return recv_finished.load(std::memory_order_acquire);
        },
        10'000'000'000ULL);

    FillDelta(result, before, parked);
    result.ok = ok;
    if (!result.ok) {
        result.error = "channel recv/send 未在 10s 内完成";
    }
    return result;
}

/// @brief 只启动 Scheduler（场景 C 的对照组）。
ProbeResult ProbeSchedulerCore() {
    ProbeResult result;
    const MemSnapshot before = Capture();

    const std::uint64_t begin = Now();
    auto* scheduler = new go2cpp::Scheduler(MakeSchedulerConfig(kWorkers));
    scheduler->start();
    (void)SpinWaitUntil(
        [scheduler] { return scheduler->worker_count() >= kWorkers; },
        2'000'000'000ULL);
    const std::uint64_t end = Now();

    const MemSnapshot after = Capture();
    FillDelta(result, before, after);

    scheduler->shutdown();
    delete scheduler;

    result.init_ns = end - begin;
    result.use_ns = result.init_ns;
    result.min_ns = result.init_ns;
    result.p95_ns = result.init_ns;
    result.max_ns = result.init_ns;
    result.samples = 1;
    result.ok = true;
    return result;
}

// ---------------------------------------------------------------------------
// 急切池：构造即分配，可重复
// ---------------------------------------------------------------------------

/// @brief LRU-K 时间轮 + 侵入式节点池（LruKCache 构造 + 一次 Put）。
ProbeResult ProbeCacheNodePool(ProbeMode mode, std::size_t repeat) {
    ProbeResult result;
    if (mode == ProbeMode::kTime) {
        std::vector<double> samples;
        samples.reserve(repeat);
        for (std::size_t i = 0; i < repeat; ++i) {
            const std::uint64_t begin = Now();
            {
                go2cpp::cache::LruKCache<std::uint64_t, std::uint64_t> cache(
                    CacheOptions());
                cache.Put(1, 2, 16);
            }
            samples.push_back(static_cast<double>(Now() - begin));
        }
        FinishTiming(result, std::move(samples));
        result.ok = true;
        return result;
    }

    const MemSnapshot before = Capture();
    {
        const std::uint64_t begin = Now();
        go2cpp::cache::LruKCache<std::uint64_t, std::uint64_t> cache(
            CacheOptions());
        cache.Put(1, 2, 16);
        const std::uint64_t end = Now();

        const MemSnapshot after = Capture();
        FillDelta(result, before, after);
        result.init_ns = end - begin;
        result.use_ns = result.init_ns;
        result.min_ns = result.init_ns;
        result.p95_ns = result.init_ns;
        result.max_ns = result.init_ns;
        result.samples = 1;
    }
    result.ok = true;
    return result;
}

/// @brief LSM Arena：构造零分配，首次 Allocate 申请 64 KiB 块。
ProbeResult ProbeLsmArena(ProbeMode mode, std::size_t repeat) {
    ProbeResult result;
    if (mode == ProbeMode::kTime) {
        std::vector<double> samples;
        samples.reserve(repeat);
        for (std::size_t i = 0; i < repeat; ++i) {
            const std::uint64_t begin = Now();
            {
                go2cpp::storage::Arena arena;
                [[maybe_unused]] char* block = arena.Allocate(64);
            }
            samples.push_back(static_cast<double>(Now() - begin));
        }
        FinishTiming(result, std::move(samples));
        result.ok = true;
        return result;
    }

    const MemSnapshot before = Capture();
    {
        const std::uint64_t begin = Now();
        go2cpp::storage::Arena arena;
        [[maybe_unused]] char* block = arena.Allocate(64);
        const std::uint64_t end = Now();

        const MemSnapshot after = Capture();
        FillDelta(result, before, after);
        result.init_ns = end - begin;
        result.use_ns = result.init_ns;
        result.min_ns = result.init_ns;
        result.p95_ns = result.init_ns;
        result.max_ns = result.init_ns;
        result.samples = 1;
    }
    result.ok = true;
    return result;
}

/// @brief JitterBuffer：512 槽 × 1536 B 负载池 + 512 × 24 B 元数据。
ProbeResult ProbeJitterBuffer(ProbeMode mode, std::size_t repeat) {
    ProbeResult result;
    if (mode == ProbeMode::kTime) {
        std::vector<double> samples;
        samples.reserve(repeat);
        for (std::size_t i = 0; i < repeat; ++i) {
            const std::uint64_t begin = Now();
            {
                go2cpp::media::JitterBuffer buffer(
                    go2cpp::media::JitterBufferConfig{});
            }
            samples.push_back(static_cast<double>(Now() - begin));
        }
        FinishTiming(result, std::move(samples));
        result.ok = true;
        return result;
    }

    const MemSnapshot before = Capture();
    {
        const std::uint64_t begin = Now();
        go2cpp::media::JitterBuffer buffer(
            go2cpp::media::JitterBufferConfig{});
        const std::uint64_t end = Now();

        const MemSnapshot after = Capture();
        FillDelta(result, before, after);
        result.init_ns = end - begin;
        result.use_ns = result.init_ns;
        result.min_ns = result.init_ns;
        result.p95_ns = result.init_ns;
        result.max_ns = result.init_ns;
        result.samples = 1;
    }
    result.ok = true;
    return result;
}

/// @brief 流表：构造空表 + 一条本地流。
ProbeResult ProbeStreamTable(ProbeMode mode, std::size_t repeat) {
    ProbeResult result;
    if (mode == ProbeMode::kTime) {
        std::vector<double> samples;
        samples.reserve(repeat);
        for (std::size_t i = 0; i < repeat; ++i) {
            const std::uint64_t begin = Now();
            {
                go2cpp::app::StreamTable table;
                [[maybe_unused]] auto* stream =
                    table.CreateLocal(go2cpp::app::StreamKind::kBytes);
            }
            samples.push_back(static_cast<double>(Now() - begin));
        }
        FinishTiming(result, std::move(samples));
        result.ok = true;
        return result;
    }

    const MemSnapshot before = Capture();
    {
        const std::uint64_t begin = Now();
        go2cpp::app::StreamTable table;
        [[maybe_unused]] auto* stream =
            table.CreateLocal(go2cpp::app::StreamKind::kBytes);
        const std::uint64_t end = Now();

        const MemSnapshot after = Capture();
        FillDelta(result, before, after);
        result.init_ns = end - begin;
        result.use_ns = result.init_ns;
        result.min_ns = result.init_ns;
        result.p95_ns = result.init_ns;
        result.max_ns = result.init_ns;
        result.samples = 1;
    }
    result.ok = true;
    return result;
}

/// @brief SFU 房间表：构造空路由 + 一个房间 + 一个 peer + 一条流。
ProbeResult ProbeSfuRouter(ProbeMode mode, std::size_t repeat) {
    ProbeResult result;
    if (mode == ProbeMode::kTime) {
        std::vector<double> samples;
        samples.reserve(repeat);
        for (std::size_t i = 0; i < repeat; ++i) {
            const std::uint64_t begin = Now();
            {
                go2cpp::media::SfuRouter router;
                router.CreateRoom("room-1");
                router.Join("room-1", 1);
                router.Publish("room-1", 1, 1);
            }
            samples.push_back(static_cast<double>(Now() - begin));
        }
        FinishTiming(result, std::move(samples));
        result.ok = true;
        return result;
    }

    const MemSnapshot before = Capture();
    {
        const std::uint64_t begin = Now();
        go2cpp::media::SfuRouter router;
        router.CreateRoom("room-1");
        router.Join("room-1", 1);
        router.Publish("room-1", 1, 1);
        const std::uint64_t end = Now();

        const MemSnapshot after = Capture();
        FillDelta(result, before, after);
        result.init_ns = end - begin;
        result.use_ns = result.init_ns;
        result.min_ns = result.init_ns;
        result.p95_ns = result.init_ns;
        result.max_ns = result.init_ns;
        result.samples = 1;
    }
    result.ok = true;
    return result;
}

/// @brief 进程池：构造（不 Start，不 fork 任何子进程）。
ProbeResult ProbeProcessPool(ProbeMode mode, std::size_t repeat) {
    ProbeResult result;
    if (mode == ProbeMode::kTime) {
        std::vector<double> samples;
        samples.reserve(repeat);
        for (std::size_t i = 0; i < repeat; ++i) {
            const std::uint64_t begin = Now();
            {
                go2cpp::os::ProcessPool pool;
                [[maybe_unused]] bool running = pool.running();
            }
            samples.push_back(static_cast<double>(Now() - begin));
        }
        FinishTiming(result, std::move(samples));
        result.ok = true;
        return result;
    }

    const MemSnapshot before = Capture();
    {
        const std::uint64_t begin = Now();
        go2cpp::os::ProcessPool pool;
        [[maybe_unused]] bool running = pool.running();
        const std::uint64_t end = Now();

        const MemSnapshot after = Capture();
        FillDelta(result, before, after);
        result.init_ns = end - begin;
        result.use_ns = result.init_ns;
        result.min_ns = result.init_ns;
        result.p95_ns = result.init_ns;
        result.max_ns = result.init_ns;
        result.samples = 1;
    }
    result.ok = true;
    return result;
}

}  // namespace

// ---------------------------------------------------------------------------
// 清单
// ---------------------------------------------------------------------------

const std::vector<PoolInfo>& PoolCatalog() {
    static const std::vector<PoolInfo> catalog = {
        {"scheduler_core", "调度器核心（对照）", PoolKind::kLazy,
         "src/concurrency/scheduler.cpp", 4, 0, 0,
         "P/M/G 结构、64 片 runnable 计数（4 KiB）、全局队列", false},

        {"task_block_pool", "Task 共享块池", PoolKind::kLazy,
         "src/concurrency/scheduler.cpp:667", 96, 704, 45056,
         "per-thread 96 槽（768 B TLS）+ 进程 free-list 48 + 64 块/chunk", false},

        {"fiber_bin", "FiberBin + 栈 arena", PoolKind::kLazy,
         "src/concurrency/fiber.cpp:276", 128, 4096, 1048576,
         "TLS 128 槽（1 KiB）+ arena 首建 8 槽 × 8 MiB reserve（仅 VA）", false},

        {"wait_node_pool", "sync WaitNode 池", PoolKind::kLazy,
         "src/concurrency/sync.cpp:813", 32, 0, 0,
         "TLS vector 上限 32 个 shared_ptr<WaitNode>", false},

        {"channel_wait_node", "channel WaitNode 池", PoolKind::kLazy,
         "include/go2cpp/concurrency/channel.hpp:949", 128, 0, 0,
         "TLS vector 上限 128 个裸指针节点", false},

        {"cache_node_pool", "LRU-K 时间轮 + 节点池", PoolKind::kEager,
         "include/go2cpp/cache/lru_k.hpp", 64, 0, 14344,
         "256 桶时间轮 + 512 槽锦标赛树 + NodePool 64 节点/块", true},

        {"lsm_arena", "LSM Arena", PoolKind::kEager,
         "src/storage/arena.cpp:23", 0, 65536, 0,
         "按需 64 KiB 块；构造零分配", true},

        {"jitter_buffer", "JitterBuffer", PoolKind::kEager,
         "src/media/jitter_buffer.cpp:50", 512, 1536, 798720,
         "512 槽 × 1536 B 负载 + 512 × 24 B SlotMeta", true},

        {"stream_table", "流表", PoolKind::kEager,
         "src/app/stream_table.cpp:108", 256, 0, 0,
         "空表零分配；每流 ~sizeof(Stream) 字节", true},

        {"sfu_router", "SFU 房间表", PoolKind::kEager,
         "src/media/sfu.cpp:226", 0, 0, 0,
         "空路由零分配；按房间/peer 增长", true},

        {"process_pool", "进程池", PoolKind::kEager,
         "src/os/pool.cpp:39", 4, 0, 0,
         "构造零 fork；Start() 才创建 worker", true},
    };
    return catalog;
}

const PoolInfo* FindPool(const std::string& id) {
    for (const PoolInfo& info : PoolCatalog()) {
        if (id == info.id) {
            return &info;
        }
    }
    return nullptr;
}

void SetClock(const TscClock& clock) { MutableClock() = clock; }

const TscClock& Clock() { return MutableClock(); }

// ---------------------------------------------------------------------------
// 分发
// ---------------------------------------------------------------------------

ProbeResult RunProbe(const std::string& id, ProbeMode mode,
                     std::size_t repeat) {
    if (id == "scheduler_core") {
        return ProbeSchedulerCore();
    }
    if (id == "task_block_pool") {
        return ProbeTaskBlockPool();
    }
    if (id == "fiber_bin") {
        return ProbeFiberBin(mode, repeat);
    }
    if (id == "wait_node_pool") {
        return ProbeWaitNodePool();
    }
    if (id == "channel_wait_node") {
        return ProbeChannelWaitNode();
    }
    if (id == "cache_node_pool") {
        return ProbeCacheNodePool(mode, repeat);
    }
    if (id == "lsm_arena") {
        return ProbeLsmArena(mode, repeat);
    }
    if (id == "jitter_buffer") {
        return ProbeJitterBuffer(mode, repeat);
    }
    if (id == "stream_table") {
        return ProbeStreamTable(mode, repeat);
    }
    if (id == "sfu_router") {
        return ProbeSfuRouter(mode, repeat);
    }
    if (id == "process_pool") {
        return ProbeProcessPool(mode, repeat);
    }
    ProbeResult result;
    result.error = "未知池 id: " + id;
    return result;
}

// ---------------------------------------------------------------------------
// 场景 C：全部池一起初始化
// ---------------------------------------------------------------------------

ProbeResult RunAllPools(bool include_scheduler) {
    ProbeResult result;
    // 场景 C 分段跨毫秒，且线程/协程可能被调度到别的核。TSC 在虚拟化环境里
    // 跨核不一定同步，毫秒级测量一旦读到偏移就是「19 ms 的唤醒」这种假数。
    // 这里统一用 CLOCK_MONOTONIC：20 ns 的读开销对毫秒级分段可以忽略。
    auto wall_now = [] { return MonotonicNs(); };
    const bool trace = std::getenv("STARTUP_COST_TRACE") != nullptr;
    auto mark = [&result, trace, &wall_now](const char* name,
                                            std::uint64_t begin) {
        const std::uint64_t end = wall_now();
        const std::uint64_t delta = end - begin;
        result.stages.push_back(StageTiming{name, delta});
        if (trace) {
            std::fprintf(stderr, "[trace] %-20s %8.3f ms\n", name,
                         static_cast<double>(delta) / 1'000'000.0);
        }
        return end;
    };
    auto add_wait = [&result](std::uint64_t ns) {
        result.non_pool_wait_ns += ns;
    };

    const MemSnapshot before = Capture();
    const std::uint64_t begin = wall_now();

    if (!include_scheduler) {
        // 只构造急切池（不含 Scheduler 相关），用于把「池」与「运行时」
        // 分开归因。
        go2cpp::cache::LruKCache<std::uint64_t, std::uint64_t> cache(
            CacheOptions());
        cache.Put(1, 2, 16);
        go2cpp::storage::Arena arena;
        [[maybe_unused]] char* block = arena.Allocate(64);
        go2cpp::media::JitterBuffer buffer(
            go2cpp::media::JitterBufferConfig{});
        go2cpp::app::StreamTable table;
        [[maybe_unused]] auto* stream =
            table.CreateLocal(go2cpp::app::StreamKind::kBytes);
        go2cpp::media::SfuRouter router;
        router.CreateRoom("room-1");
        go2cpp::os::ProcessPool pool;

        const std::uint64_t end = wall_now();
        const MemSnapshot after = Capture();
        FillDelta(result, before, after);
        result.init_ns = end - begin;
        result.use_ns = result.init_ns;
        result.min_ns = result.init_ns;
        result.p95_ns = result.init_ns;
        result.max_ns = result.init_ns;
        result.samples = 1;
        result.ok = true;
        return result;
    }

    Fixture fixture(kWorkers);
    std::uint64_t stage = mark("scheduler", begin);
    std::atomic<bool> task_done{false};
    fixture.scheduler().spawn([&task_done] {
        task_done.store(true, std::memory_order_release);
    });
    (void)SpinWaitUntil(
        [&task_done] { return task_done.load(std::memory_order_acquire); },
        10'000'000'000ULL);
    stage = mark("first-spawn", stage);

    {
        go2cpp::Fiber fiber([] {}, 4096);
    }
    stage = mark("fiber", stage);

    // 一次真实竞争，触发 sync WaitNode 池。用 Task 状态精确等到「已 park」，
    // 不 sleep：sleep 要么太短（waiter 还没入队，节点没分配），要么把观察
    // 时间算进测量。
    go2cpp::sync::Mutex mutex;
    mutex.Lock();
    std::uint64_t sub = mark("sync-lock-main", stage);
    std::atomic<bool> waiter_finished{false};
    auto waiter = fixture.scheduler().spawn([&] {
        mutex.Lock();
        mutex.Unlock();
        waiter_finished.store(true, std::memory_order_release);
    });
    sub = mark("sync-spawn", sub);
    (void)SpinWaitUntil(
        [&waiter] {
            return waiter->state() == go2cpp::scheduler::GState::kWaiting;
        },
        5'000'000'000ULL);
    sub = mark("sync-parked", sub);
    mutex.Unlock();
    sub = mark("sync-unlock", sub);
    {
        const std::uint64_t wake_begin = wall_now();
        (void)SpinWaitUntil(
            [&waiter_finished] {
                return waiter_finished.load(std::memory_order_acquire);
            },
            10'000'000'000ULL);
        // 这一段是「唤醒后被重新调度」的往返，与池无关，单独扣掉。
        add_wait(wall_now() - wake_begin);
    }
    sub = mark("sync-wake", sub);
    (void)sub;
    // 不单独记 "sync-waitnode" 汇总段：它会在分段表里与上面几个子段重复
    // 计账，读者容易误以为时间是相加的。
    stage = wall_now();

    // channel WaitNode 池。
    {
        auto channel = go2cpp::Channel<int>::Create(0);
        std::atomic<bool> recv_done{false};
        auto receiver = fixture.scheduler().spawn([&] {
            auto received = channel->Recv();
            (void)received;
            recv_done.store(true, std::memory_order_release);
        });
        (void)SpinWaitUntil(
            [&receiver] {
                return receiver->state() ==
                       go2cpp::scheduler::GState::kWaiting;
            },
            5'000'000'000ULL);
        fixture.scheduler().spawn([&channel] {
            auto sent = channel->Send(1);
            (void)sent;
        });
        (void)SpinWaitUntil(
            [&recv_done] { return recv_done.load(std::memory_order_acquire); },
            10'000'000'000ULL);
    }
    stage = mark("channel-waitnode", stage);

    // 急切池。用 optional 让它们的生命周期延伸到函数返回：如果在 Capture
    // 之前析构，Δheap 会把已经还回去的 784 KiB JitterBuffer 算成 0。
    std::optional<go2cpp::cache::LruKCache<std::uint64_t, std::uint64_t>> cache;
    cache.emplace(CacheOptions());
    cache->Put(1, 2, 16);
    go2cpp::storage::Arena arena;
    [[maybe_unused]] char* block = arena.Allocate(64);
    std::optional<go2cpp::media::JitterBuffer> buffer;
    buffer.emplace(go2cpp::media::JitterBufferConfig{});
    go2cpp::app::StreamTable table;
    [[maybe_unused]] auto* stream =
        table.CreateLocal(go2cpp::app::StreamKind::kBytes);
    go2cpp::media::SfuRouter router;
    router.CreateRoom("room-1");
    router.Join("room-1", 1);
    go2cpp::os::ProcessPool pool;
    stage = mark("eager-pools", stage);

    const std::uint64_t end = wall_now();
    (void)stage;
    const MemSnapshot after = Capture();
    FillDelta(result, before, after);
    result.init_ns = end - begin;
    result.use_ns = result.init_ns;
    result.min_ns = result.init_ns;
    result.p95_ns = result.init_ns;
    result.max_ns = result.init_ns;
    result.samples = 1;
    result.ok = true;
    return result;
}

ProbeResult RunSchedulerOnly() { return ProbeSchedulerCore(); }

}  // namespace startup_cost
