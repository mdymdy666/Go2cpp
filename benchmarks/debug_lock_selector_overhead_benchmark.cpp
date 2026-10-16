/**
 * @file debug_lock_selector_overhead_benchmark.cpp
 * @brief D3 锁争用选择钩子的开销实测（build-bench / Release）。
 *
 * 要回答的问题：新增的 `WaitSelector` 接入点**对默认路径有多少开销**。
 *
 * 三档：
 *   档 0：不注册选择器（框架默认）
 *   档 1：注册一个"永远不干预"的选择器（返回 nullopt）
 *   档 2：注册一个"总是选队尾"的选择器（真的重排队列）
 *
 * 两个场景：
 *   A 无竞争：单线程 TryLock/Unlock。`Unlock` 走 `compare_exchange` 快路径，
 *     **根本不会进入选择器分支** —— 因此预期三档无差异。
 *   B 有竞争：4 线程 Lock/Unlock。此时 `Unlock` 必须走队列慢路径，
 *     选择器被调用，可量化其开销。
 *
 * 编译：CMake 在 GO2CPP_BUILD_BENCHMARKS 下登记并加 -O2；必须用 build-bench(Release)。
 * 用法：./go2cpp_debug_lock_selector_benchmark [轮数] [无竞争迭代数] [有竞争每线程迭代数]
 */

#include "go2cpp/sync.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using go2cpp::sync::Mutex;
using go2cpp::sync::WaitSelector;
using go2cpp::sync::WaiterCandidate;

/// 永远不干预。
class PassiveSelector final : public WaitSelector {
public:
    std::optional<std::size_t> Select(const std::vector<WaiterCandidate>&) noexcept override {
        return std::nullopt;
    }
};

/// 总是让队尾优先（真实重排）。
class LastSelector final : public WaitSelector {
public:
    std::optional<std::size_t> Select(const std::vector<WaiterCandidate>& candidates) noexcept override {
        if (candidates.size() < 2) {
            return std::nullopt;
        }
        return candidates.back().index;
    }
};

enum class Mode { kNone, kPassive, kLast };

const char* ModeName(Mode mode) {
    switch (mode) {
        case Mode::kNone: return "档0 未注册选择器";
        case Mode::kPassive: return "档1 不干预选择器";
        case Mode::kLast: return "档2 队尾优先选择器";
    }
    return "?";
}

void ApplyMode(Mode mode) {
    switch (mode) {
        case Mode::kNone:
            go2cpp::sync::ResetWaitSelectorFactory();
            break;
        case Mode::kPassive:
            go2cpp::sync::SetWaitSelectorFactory(
                [] { return std::make_shared<PassiveSelector>(); });
            break;
        case Mode::kLast:
            go2cpp::sync::SetWaitSelectorFactory(
                [] { return std::make_shared<LastSelector>(); });
            break;
    }
}

/// 场景 A：单线程无竞争。
double ContendedFree(Mode mode, long iterations) {
    ApplyMode(mode);
    Mutex mutex;
    const auto started = Clock::now();
    for (long i = 0; i < iterations; ++i) {
        if (mutex.TryLock()) {
            mutex.Unlock();
        }
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started);
    return static_cast<double>(elapsed.count()) / static_cast<double>(iterations);
}

/// 场景 B：4 线程竞争。
struct ContendedResult {
    double ns_per_op{0};
    long completed{0};
};

ContendedResult Contended(Mode mode, long per_thread, int threads) {
    ApplyMode(mode);
    Mutex mutex;
    std::atomic<long> completed{0};
    std::vector<std::thread> workers;
    workers.reserve(static_cast<std::size_t>(threads));
    const auto started = Clock::now();
    for (int t = 0; t < threads; ++t) {
        workers.emplace_back([&mutex, &completed, per_thread] {
            for (long i = 0; i < per_thread; ++i) {
                if (mutex.Lock()) {
                    mutex.Unlock();
                    completed.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started);
    const long total = completed.load(std::memory_order_relaxed);
    ContendedResult result;
    result.completed = total;
    result.ns_per_op = total > 0 ? static_cast<double>(elapsed.count()) / static_cast<double>(total) : 0.0;
    return result;
}

template <typename Fn>
void Report(const char* label, int repeats, Fn&& run) {
    std::vector<double> samples;
    for (int i = 0; i < repeats; ++i) {
        samples.push_back(run());
    }
    std::sort(samples.begin(), samples.end());
    std::printf("  %-38s min %9.2f  median %9.2f  max %9.2f  ns/op\n",
                label, samples.front(), samples[samples.size() / 2], samples.back());
}

}  // namespace

int main(int argc, char** argv) {
    int repeats = 5;
    long free_iterations = 2000000;
    long contended_iterations = 20000;
    if (argc > 1) { const int v = std::atoi(argv[1]); if (v > 0) repeats = v; }
    if (argc > 2) { const long v = std::atol(argv[2]); if (v > 0) free_iterations = v; }
    if (argc > 3) { const long v = std::atol(argv[3]); if (v > 0) contended_iterations = v; }
    std::string mode = "all";
    if (argc > 4) { mode = argv[4]; }
    const bool want_none = (mode == "all" || mode == "0");
    const bool want_passive = (mode == "all" || mode == "1");
    const bool want_last = (mode == "all" || mode == "2");

    std::printf("=== D3 锁选择钩子开销（build-bench / Release） ===\n");
    std::printf("参数：轮数=%d 无竞争迭代=%ld 有竞争每线程迭代=%ld\n", repeats,
                free_iterations, contended_iterations);

    std::printf("\n[A] 无竞争（单线程 TryLock/Unlock）—— 预期三档无差异\n");
    for (Mode m : {Mode::kNone, Mode::kPassive, Mode::kLast}) {
        if ((m == Mode::kNone && !want_none) || (m == Mode::kPassive && !want_passive) ||
            (m == Mode::kLast && !want_last)) {
            continue;
        }
        // 热身一轮（避免首轮冷启动影响，与 observer 基准同一个教训）
        (void)ContendedFree(m, free_iterations / 10);
        const auto t0 = Clock::now();
        Report(ModeName(m), repeats, [m, free_iterations] {
            return ContendedFree(m, free_iterations);
        });
        std::printf("        （该档总耗时 %.2f s）\n",
                    std::chrono::duration<double>(Clock::now() - t0).count());
        std::fflush(stdout);
    }

    std::printf("\n[B] 有竞争（4 线程 Lock/Unlock）—— 选择器在此被真正调用\n");
    for (Mode m : {Mode::kNone, Mode::kPassive, Mode::kLast}) {
        if ((m == Mode::kNone && !want_none) || (m == Mode::kPassive && !want_passive) ||
            (m == Mode::kLast && !want_last)) {
            continue;
        }
        const auto warm = Contended(m, contended_iterations / 10, 4);
        std::printf("  [%s] 热身完成 %ld 次，继续 %d 轮...\n", ModeName(m), warm.completed, repeats);
        std::fflush(stdout);
        std::vector<double> samples;
        long last_completed = 0;
        const auto t0 = Clock::now();
        for (int i = 0; i < repeats; ++i) {
            const auto result = Contended(m, contended_iterations, 4);
            samples.push_back(result.ns_per_op);
            last_completed = result.completed;
            std::printf("      第 %d 轮: %.2f ns/op (完成 %ld)\n", i + 1,
                        result.ns_per_op, result.completed);
            std::fflush(stdout);
        }
        const double seconds = std::chrono::duration<double>(Clock::now() - t0).count();
        std::sort(samples.begin(), samples.end());
        std::printf("  %-38s min %9.2f  median %9.2f  max %9.2f  ns/op  [完成=%ld]\n",
                    ModeName(m), samples.front(), samples[samples.size() / 2], samples.back(),
                    last_completed);
        std::printf("        （该档 %d 轮总耗时 %.2f s）\n", repeats, seconds);
        std::fflush(stdout);
    }

    go2cpp::sync::ResetWaitSelectorFactory();
    std::printf("\n说明：ns/op 来自 build-bench（Release, -O2）；每档先热身一轮再做 %d 轮取区间。\n", repeats);
    std::printf("      场景 A 走 Unlock 的 CAS 快路径，不进入选择器分支；\n");
    std::printf("      场景 B 必须走队列慢路径，差值即选择器的净开销。\n");
    return 0;
}
