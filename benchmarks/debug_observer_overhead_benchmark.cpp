/**
 * @file debug_observer_overhead_benchmark.cpp
 * @brief 调度器观察者（调试插件）的三档开销对照基准。
 *
 * 目的：回答「开了调试模块，程序会慢多少」——这是 DebugTooling/design/07 §4 的
 *       "零开销证明"的端到端部分。
 *
 * 三档（同一负载，只改 observer）：
 *   档 0：observer = nullptr                       —— 基线（框架自身）
 *   档 1：observer = 只做一次原子递增              —— 测"事件机制本身的固定开销"
 *   档 2：observer = 入队到有界环形缓冲 + 独立消费线程 —— 测"真实使用"的开销
 *
 * 负载：spawn N 个任务并等待全部完成（每个任务产生 kTaskStarted 与
 *       kTaskCompleted/Failed/Suspended 两个事件）。
 *
 * 编译：由 CMake 在 GO2CPP_BUILD_BENCHMARKS 下登记，另加 -O2；必须用 build-bench(Release)。
 * 用法：./go2cpp_debug_observer_benchmark [每档轮数] [任务数]
 */

#include "go2cpp/scheduler.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

/// 有界环形缓冲：非阻塞入队，满则丢弃并计数（与设计文档的 sink 约束一致）。
class EventRing {
public:
    static constexpr std::size_t kCapacity = 8192;

    /// @return 入队成功返回 true；满时返回 false（调用方记丢弃）。
    bool Push(const go2cpp::SchedulerEvent& event) noexcept {
        const std::size_t head = m_head.load(std::memory_order_relaxed);
        const std::size_t next = (head + 1) % kCapacity;
        if (next == m_tail.load(std::memory_order_acquire)) {
            return false;
        }
        m_buffer[head] = event;
        m_head.store(next, std::memory_order_release);
        return true;
    }

    bool Pop(go2cpp::SchedulerEvent* out) noexcept {
        const std::size_t tail = m_tail.load(std::memory_order_relaxed);
        if (tail == m_head.load(std::memory_order_acquire)) {
            return false;
        }
        *out = m_buffer[tail];
        m_tail.store((tail + 1) % kCapacity, std::memory_order_release);
        return true;
    }

private:
    std::array<go2cpp::SchedulerEvent, kCapacity> m_buffer{};
    std::atomic<std::size_t> m_head{0};
    std::atomic<std::size_t> m_tail{0};
};

enum class Mode { kNone, kCountOnly, kEnqueue };

class BenchObserver final : public go2cpp::SchedulerObserver {
public:
    explicit BenchObserver(Mode mode, EventRing* ring)
        : m_mode(mode), m_ring(ring) {}

    void OnEvent(const go2cpp::SchedulerEvent& event) override {
        if (m_mode == Mode::kCountOnly) {
            m_events.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (m_mode == Mode::kEnqueue && m_ring != nullptr) {
            m_events.fetch_add(1, std::memory_order_relaxed);
            if (!m_ring->Push(event)) {
                m_dropped.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    std::uint64_t events() const noexcept { return m_events.load(std::memory_order_relaxed); }
    std::uint64_t dropped() const noexcept { return m_dropped.load(std::memory_order_relaxed); }

private:
    Mode m_mode;
    EventRing* m_ring;
    std::atomic<std::uint64_t> m_events{0};
    std::atomic<std::uint64_t> m_dropped{0};
};

struct RunResult {
    double ns_per_task{0};
    std::uint64_t observer_events{0};
    std::uint64_t dropped{0};
    std::size_t consumed{0};
};

RunResult RunOnce(Mode mode, std::size_t task_count, std::size_t workers,
                    std::size_t local_queue_limit) {
    go2cpp::SchedulerConfig config;
    config.processor_count = workers;
    config.max_workers = workers;
    config.min_workers = workers;
    if (local_queue_limit != 0) {
        config.local_queue_limit = local_queue_limit;
    }
    config.collect_metrics = false;  // 隔离 observer 的影响
    go2cpp::Scheduler scheduler(config);

    EventRing ring;
    std::shared_ptr<BenchObserver> observer;
    std::atomic<bool> consumer_stop{false};
    std::atomic<std::size_t> consumed{0};
    std::thread consumer;
    if (mode == Mode::kCountOnly || mode == Mode::kEnqueue) {
        observer = std::make_shared<BenchObserver>(mode, &ring);
        scheduler.set_observer(observer);
    }
    if (mode == Mode::kEnqueue) {
        consumer = std::thread([&ring, &consumer_stop, &consumed] {
            go2cpp::SchedulerEvent event;
            while (!consumer_stop.load(std::memory_order_relaxed)) {
                if (ring.Pop(&event)) {
                    consumed.fetch_add(1, std::memory_order_relaxed);
                } else {
                    std::this_thread::yield();
                }
            }
            while (ring.Pop(&event)) {
                consumed.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    scheduler.start();
    std::vector<std::shared_ptr<go2cpp::Task>> tasks;
    tasks.reserve(task_count);

    const auto started = Clock::now();
    for (std::size_t i = 0; i < task_count; ++i) {
        tasks.push_back(scheduler.spawn([i] {
            // 极短任务：让事件开销占主导（这是本基准要放大的部分）
            volatile std::uint64_t acc = i;
            (void)acc;
        }));
    }
    for (auto& task : tasks) {
        task->wait();
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started);

    if (mode == Mode::kEnqueue) {
        consumer_stop.store(true, std::memory_order_relaxed);
        consumer.join();
    }
    scheduler.shutdown();

    RunResult result;
    result.ns_per_task = static_cast<double>(elapsed.count()) / static_cast<double>(task_count);
    result.observer_events = observer ? observer->events() : 0;
    result.dropped = observer ? observer->dropped() : 0;
    result.consumed = consumed.load(std::memory_order_relaxed);
    return result;
}

const char* ModeName(Mode mode) {
    switch (mode) {
        case Mode::kNone: return "档0 observer=nullptr（基线）";
        case Mode::kCountOnly: return "档1 observer=原子计数（最轻观察者）";
        case Mode::kEnqueue: return "档2 observer=有界缓冲+消费线程";
    }
    return "?";
}

}  // namespace

int main(int argc, char** argv) {
    int repeats = 5;
    std::size_t task_count = 20000;
    std::size_t workers = 4;
    if (argc > 1) {
        const int v = std::atoi(argv[1]);
        if (v > 0) repeats = v;
    }
    if (argc > 2) {
        const long v = std::atol(argv[2]);
        if (v > 0) task_count = static_cast<std::size_t>(v);
    }
    if (argc > 3) {
        const long v = std::atol(argv[3]);
        if (v > 0) workers = static_cast<std::size_t>(v);
    }
    std::size_t local_queue_limit = 0;   // 0 = 用默认(256)
    if (argc > 5) {
        const long v = std::atol(argv[5]);
        if (v > 0) local_queue_limit = static_cast<std::size_t>(v);
    }

    std::printf("=== 调度器观察者开销三档对照（build-bench / Release） ===\n");
    std::printf("参数：任务数=%zu  工作线程=%zu  每档轮数=%d\n\n", task_count, workers, repeats);

    for (Mode mode : {Mode::kNone, Mode::kCountOnly, Mode::kEnqueue}) {
        // 热身轮：不计入统计。首轮包含 Fiber 栈池、worker 线程与调度器结构的
        // 冷启动成本，若不排除会让"先测的档"显著偏慢（实测过：档0 曾因冷启动
        // 比档1 慢约 20 倍，属测量错误而非真实开销）。
        (void)RunOnce(mode, task_count, workers, local_queue_limit);
        std::vector<double> samples;
        RunResult last;
        for (int i = 0; i < repeats; ++i) {
            last = RunOnce(mode, task_count, workers, local_queue_limit);
            samples.push_back(last.ns_per_task);
        }
        std::sort(samples.begin(), samples.end());
        std::printf("%-42s min %9.1f  median %9.1f  max %9.1f  ns/任务",
                    ModeName(mode), samples.front(), samples[samples.size() / 2], samples.back());
        if (mode != Mode::kNone) {
            std::printf("   [事件=%llu 丢弃=%llu 消费=%zu]",
                        static_cast<unsigned long long>(last.observer_events),
                        static_cast<unsigned long long>(last.dropped), last.consumed);
        }
        std::printf("\n");
    }

    std::printf("\n说明：数字为 ns/任务，来自 build-bench（Release, -O2）；每档先跑 1 轮热身（不计入）再做 %d 轮取区间。\n", repeats);
    std::printf("      档1/档2 相对档0 的差值即观察者的端到端开销；\n");
    std::printf("      不得把最小轮次当保证，也不得与其他日期/宿主的结果横向比较。\n");
    return 0;
}
