/**
 * @file cache_bench.cpp
 * @brief 多场景缓存基准：命中率 / 吞吐 / 峰值内存 / 扫描后恢复时间。
 *
 * @details 五个场景（hotspot / scan / weighted / cdn / mixed）× 七种策略，
 *          每个场景用同一份请求序列跑所有策略，保证逐请求可比。
 *
 *          - 命中率：Get 命中数 / 总请求数；
 *          - 吞吐：整段请求序列的 Mops/s（含未命中后的 Put）；
 *          - 内存：替换全局 `operator new/delete`，记录缓存存活期间的
 *            **峰值净分配字节**（构造前基线 -> 运行峰值），销毁后回落到
 *            基线才认为无泄漏；
 *          - 恢复时间：scan 场景在扫描结束后，按窗口统计热点命中率，
 *            首个达到「稳态命中率 90%」的窗口位置（单位：请求数）。
 *
 *          用法：
 *          ```text
 *          cache_bench --quick          # ctest 默认，约 1~2 秒
 *          cache_bench --full           # 完整规模，约 10~20 秒
 *          cache_bench --ops=100000 --capacity=1000
 *          ```
 */
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "go2cpp/cache/factory.hpp"
#include "go2cpp/cache/workload.hpp"

namespace {

// ---------------------------------------------------------------------------
// 全局分配统计（只在本基准可执行内替换，不影响库代码）。
// ---------------------------------------------------------------------------
constexpr std::size_t kAllocHeader = alignof(std::max_align_t);

std::atomic<std::size_t> g_live_bytes{0};
std::atomic<std::size_t> g_peak_bytes{0};

void TrackPeak(std::size_t live) noexcept {
    std::size_t peak = g_peak_bytes.load(std::memory_order_relaxed);
    while (live > peak &&
           !g_peak_bytes.compare_exchange_weak(peak, live,
                                               std::memory_order_relaxed)) {
    }
}

void* AllocateTracked(std::size_t size) {
    void* raw = std::malloc(size + kAllocHeader);
    if (raw == nullptr) {
        throw std::bad_alloc();
    }
    *static_cast<std::size_t*>(raw) = size;
    const std::size_t live = g_live_bytes.fetch_add(size) + size;
    TrackPeak(live);
    return static_cast<char*>(raw) + kAllocHeader;
}

void DeallocateTracked(void* pointer) noexcept {
    if (pointer == nullptr) {
        return;
    }
    char* raw = static_cast<char*>(pointer) - kAllocHeader;
    const std::size_t size = *reinterpret_cast<std::size_t*>(raw);
    g_live_bytes.fetch_sub(size, std::memory_order_relaxed);
    std::free(raw);
}

}  // namespace

void* operator new(std::size_t size) { return AllocateTracked(size); }
void* operator new[](std::size_t size) { return AllocateTracked(size); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return AllocateTracked(size);
    } catch (...) {
        return nullptr;
    }
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return AllocateTracked(size);
    } catch (...) {
        return nullptr;
    }
}
void operator delete(void* pointer) noexcept { DeallocateTracked(pointer); }
void operator delete[](void* pointer) noexcept { DeallocateTracked(pointer); }
void operator delete(void* pointer, std::size_t) noexcept {
    DeallocateTracked(pointer);
}
void operator delete[](void* pointer, std::size_t) noexcept {
    DeallocateTracked(pointer);
}
void operator delete(void* pointer, const std::nothrow_t&) noexcept {
    DeallocateTracked(pointer);
}
void operator delete[](void* pointer, const std::nothrow_t&) noexcept {
    DeallocateTracked(pointer);
}

namespace {

using go2cpp::cache::CacheOptions;
using go2cpp::cache::CacheRequest;
using go2cpp::cache::Policy;
using go2cpp::cache::Workload;
using go2cpp::cache::WorkloadKind;
using go2cpp::cache::WorkloadOptions;
using Clock = std::chrono::steady_clock;

const Policy kPolicies[] = {
    Policy::kLru,   Policy::kLruK, Policy::kTwoQueue, Policy::kTinyLfu,
    Policy::kGdsf,  Policy::kArc,  Policy::kClock,
};

struct ScenarioData {
    WorkloadKind kind = WorkloadKind::kHotspot;
    std::vector<CacheRequest> requests;
    std::size_t warm_end = 0;   ///< scan：预热阶段结束位置。
    std::size_t scan_end = 0;   ///< scan：扫描阶段结束位置。
};

struct BenchConfig {
    std::size_t capacity = 500;
    std::size_t ops = 30000;
    std::size_t scan_length = 5000;
    std::size_t recovery_ops = 20000;
    std::size_t window = 1000;
    std::uint64_t seed = 0x5DEECE66DULL;
};

struct RunResult {
    double hit_rate = 0.0;
    long recovery_ops = -1;
    double mops = 0.0;
    std::size_t peak_bytes = 0;
    std::size_t leaked_bytes = 0;
    std::size_t size = 0;
    std::size_t weight = 0;
};

CacheOptions MakeOptions(Policy policy, std::size_t capacity,
                         WorkloadKind kind) {
    CacheOptions options;
    options.capacity = capacity;
    if (policy == Policy::kGdsf) {
        if (kind == WorkloadKind::kWeighted) {
            // 平均对象大小 = 0.8*1 + 0.2*1000 = 200.8，按字节预算对齐
            // 其它策略的条目容量。
            options.capacity = 0;
            options.max_weight = capacity * 201U;
        } else if (kind == WorkloadKind::kCdn) {
            // CDN 分片平均约 1160 个 64KiB 单位。
            options.capacity = 0;
            options.max_weight = capacity * 1160U;
        } else {
            options.capacity = capacity;
        }
    }
    return options;
}

ScenarioData BuildScenario(WorkloadKind kind, const BenchConfig& config) {
    ScenarioData data;
    data.kind = kind;
    if (kind == WorkloadKind::kScan) {
        WorkloadOptions warm_options;
        warm_options.hot_keys = config.capacity;
        warm_options.cold_keys = config.capacity * 100U;
        warm_options.operations = config.recovery_ops;
        warm_options.seed = config.seed;
        Workload warm(WorkloadKind::kHotspot, warm_options);
        data.requests = warm.Generate();
        data.warm_end = data.requests.size();

        for (std::size_t i = 0; i < config.scan_length; ++i) {
            data.requests.push_back(
                CacheRequest{100000 + static_cast<std::uint64_t>(i), 1});
        }
        data.scan_end = data.requests.size();

        WorkloadOptions recover_options;
        recover_options.hot_keys = config.capacity;
        recover_options.cold_keys = config.capacity * 100U;
        recover_options.operations = config.recovery_ops;
        recover_options.seed = config.seed + 1U;
        Workload recover(WorkloadKind::kHotspot, recover_options);
        const auto tail = recover.Generate();
        data.requests.insert(data.requests.end(), tail.begin(), tail.end());
        return data;
    }

    WorkloadOptions options;
    options.hot_keys = config.capacity;
    options.cold_keys = config.capacity * 100U;
    options.operations = config.ops;
    options.seed = config.seed;
    options.videos = config.capacity / 20U + 10U;
    options.segments_per_video = 100;
    Workload workload(kind, options);
    data.requests = workload.Generate();
    return data;
}

RunResult RunPolicy(const ScenarioData& data, Policy policy,
                    const BenchConfig& config) {
    RunResult result;
    CacheOptions options =
        MakeOptions(policy, config.capacity, data.kind);
    const std::size_t baseline = g_live_bytes.load(std::memory_order_relaxed);
    g_peak_bytes.store(baseline, std::memory_order_relaxed);

    auto cache = go2cpp::cache::MakeCache<std::uint64_t, std::uint64_t>(
        policy, options);
    std::uint64_t value = 0;
    std::size_t hits = 0;
    for (const CacheRequest& request : data.requests) {
        if (cache->Get(request.key, &value)) {
            ++hits;
        } else {
            cache->Put(request.key, request.key, request.weight);
        }
    }

    const std::size_t peak = g_peak_bytes.load(std::memory_order_relaxed);
    result.peak_bytes = peak > baseline ? peak - baseline : 0;
    result.hit_rate =
        data.requests.empty()
            ? 0.0
            : static_cast<double>(hits) /
                  static_cast<double>(data.requests.size());
    result.size = cache->Size();
    result.weight = cache->Weight();
    cache.reset();
    const std::size_t after = g_live_bytes.load(std::memory_order_relaxed);
    result.leaked_bytes = after > baseline ? after - baseline : 0;
    return result;
}

RunResult RunScanPolicy(const ScenarioData& data, Policy policy,
                        const BenchConfig& config) {
    RunResult result;
    CacheOptions options =
        MakeOptions(policy, config.capacity, data.kind);
    const std::size_t baseline = g_live_bytes.load(std::memory_order_relaxed);
    g_peak_bytes.store(baseline, std::memory_order_relaxed);

    auto cache = go2cpp::cache::MakeCache<std::uint64_t, std::uint64_t>(
        policy, options);
    std::uint64_t value = 0;
    std::size_t hits = 0;
    std::size_t total = 0;
    const std::size_t warm_begin =
        data.warm_end > 5000 ? data.warm_end - 5000 : 0;

    // 预热阶段：同时统计稳态命中率。
    std::size_t steady_hits = 0;
    std::size_t steady_total = 0;
    for (std::size_t i = 0; i < data.warm_end; ++i) {
        const CacheRequest& request = data.requests[i];
        const bool hit = cache->Get(request.key, &value);
        if (hit) {
            ++hits;
        } else {
            cache->Put(request.key, request.key, request.weight);
        }
        ++total;
        if (i >= warm_begin) {
            ++steady_total;
            if (hit) {
                ++steady_hits;
            }
        }
    }
    const double steady = steady_total == 0
                              ? 0.0
                              : static_cast<double>(steady_hits) /
                                    static_cast<double>(steady_total);

    // 扫描阶段：真实缓存层不知道这是扫描，miss 后照常回填——这正是
    // 「冷数据污染」发生的条件，也是 LRU-K/TinyLFU/ARC 要防的东西。
    for (std::size_t i = data.warm_end; i < data.scan_end; ++i) {
        const CacheRequest& request = data.requests[i];
        if (cache->Get(request.key, &value)) {
            ++hits;
        } else {
            cache->Put(request.key, request.key, request.weight);
        }
        ++total;
    }

    // 恢复阶段：窗口命中率首次达到稳态 90% 的位置。
    bool recovered = false;
    std::size_t window_hits = 0;
    std::size_t window_total = 0;
    for (std::size_t i = data.scan_end; i < data.requests.size(); ++i) {
        const bool hit = cache->Get(data.requests[i].key, &value);
        if (!hit) {
            cache->Put(data.requests[i].key, data.requests[i].key,
                       data.requests[i].weight);
        } else {
            ++hits;
        }
        ++total;
        ++window_total;
        if (hit) {
            ++window_hits;
        }
        if (window_total == config.window) {
            const double rate =
                static_cast<double>(window_hits) /
                static_cast<double>(window_total);
            if (!recovered && steady > 0.0 && rate >= steady * 0.9) {
                result.recovery_ops =
                    static_cast<long>(i + 1 - data.scan_end);
                recovered = true;
            }
            window_hits = 0;
            window_total = 0;
        }
    }

    result.hit_rate =
        total == 0 ? 0.0
                   : static_cast<double>(hits) / static_cast<double>(total);
    result.size = cache->Size();
    result.weight = cache->Weight();
    const std::size_t peak = g_peak_bytes.load(std::memory_order_relaxed);
    result.peak_bytes = peak > baseline ? peak - baseline : 0;
    cache.reset();
    const std::size_t after = g_live_bytes.load(std::memory_order_relaxed);
    result.leaked_bytes = after > baseline ? after - baseline : 0;
    return result;
}

BenchConfig ParseConfig(int argc, char** argv) {
    BenchConfig config;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--full") {
            config.capacity = 2000;
            config.ops = 300000;
            config.scan_length = 50000;
            config.recovery_ops = 50000;
            config.window = 2000;
        } else if (arg == "--quick") {
            config.capacity = 500;
            config.ops = 30000;
            config.scan_length = 5000;
            config.recovery_ops = 20000;
            config.window = 1000;
        } else if (arg.rfind("--ops=", 0) == 0) {
            config.ops = static_cast<std::size_t>(
                std::strtoull(arg.c_str() + 6, nullptr, 10));
        } else if (arg.rfind("--capacity=", 0) == 0) {
            config.capacity = static_cast<std::size_t>(
                std::strtoull(arg.c_str() + 11, nullptr, 10));
        }
    }
    if (config.capacity == 0) {
        config.capacity = 1;
    }
    if (config.ops == 0) {
        config.ops = 1;
    }
    return config;
}

}  // namespace

int main(int argc, char** argv) {
    const BenchConfig config = ParseConfig(argc, argv);
    std::printf(
        "# cache benchmark (capacity=%zu, ops=%zu, scan=%zu, window=%zu)\n",
        config.capacity, config.ops, config.scan_length, config.window);
    std::printf(
        "| scenario | policy | hit_rate | recovery_ops | Mops/s | "
        "peak_KiB | size | weight |\n");
    std::printf(
        "|---|---|---|---|---|---|---|---|\n");

    const WorkloadKind scenarios[] = {
        WorkloadKind::kHotspot, WorkloadKind::kScan, WorkloadKind::kWeighted,
        WorkloadKind::kCdn, WorkloadKind::kMixed,
    };

    for (WorkloadKind kind : scenarios) {
        const ScenarioData data = BuildScenario(kind, config);
        for (Policy policy : kPolicies) {
            const auto start = Clock::now();
            const RunResult result = kind == WorkloadKind::kScan
                                         ? RunScanPolicy(data, policy, config)
                                         : RunPolicy(data, policy, config);
            const double elapsed_ms =
                std::chrono::duration<double, std::milli>(Clock::now() - start)
                    .count();
            const double mops = static_cast<double>(data.requests.size()) /
                                (elapsed_ms * 1000.0);
            std::printf(
                "| %s | %s | %.4f | %ld | %.2f | %zu | %zu | %zu |\n",
                go2cpp::cache::WorkloadName(kind),
                go2cpp::cache::PolicyName(policy), result.hit_rate,
                result.recovery_ops, mops, result.peak_bytes / 1024U,
                result.size, result.weight);
            if (result.leaked_bytes != 0) {
                std::fprintf(stderr,
                             "[bench] WARNING: %s/%s leaked %zu bytes\n",
                             go2cpp::cache::WorkloadName(kind),
                             go2cpp::cache::PolicyName(policy),
                             result.leaked_bytes);
            }
        }
    }
    std::printf("\n");
    std::printf(
        "说明：recovery_ops 是扫描结束后首个达到稳态命中率 90%% 的窗口"
        "位置（-1 = 未恢复）；peak_KiB 是缓存存活期间的净分配峰值。\n");
    return 0;
}
