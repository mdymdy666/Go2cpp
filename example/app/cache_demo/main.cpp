/**
 * @file main.cpp
 * @brief 缓存淘汰策略对比：7 种策略 × 3 个真实负载场景，打印命中率。
 * @details 演示 `go2cpp::cache` 的统一接口：
 *
 *          - `Workload` 用固定 seed 生成**逐请求相同**的三组负载，保证 7 种
 *            策略在完全相同的输入上比较（热点 / 顺序扫描 / 大小权重）；
 *          - `MakeCache<uint64_t, std::string>(Policy, CacheOptions)` 返回统一
 *            `Cache` 接口，业务代码只依赖 Get/Put/Stats；
 *          - 同时统计「对象命中率」与「字节命中率」：前者是传统 hit rate，
 *            后者按请求的 weight 加权，能看出 GDSF 这类大小感知策略的取舍。
 *
 *          场景（对应 workload.hpp 的真实访问模式）：
 *            hotspot  —— 80/20 热点 + 长冷尾，容量刚好装下热点 key；
 *            scan     —— 热点中周期性插入 2000 条顺序扫描，考察抗污染；
 *            weighted —— 20% 请求带 weight=1000 的大对象，考察权重语义。
 *
 *          运行：./go2cpp_cache_demo [--ops 200000] [--capacity 1000]
 *                                    [--seed 12345] [--max-weight 5000]
 *
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <string>
#include <vector>

#include "go2cpp/cache/cache.hpp"
#include "go2cpp/cache/factory.hpp"
#include "go2cpp/cache/workload.hpp"

namespace {

using go2cpp::cache::Cache;
using go2cpp::cache::CacheOptions;
using go2cpp::cache::CacheRequest;
using go2cpp::cache::CacheStats;
using go2cpp::cache::MakeCache;
using go2cpp::cache::Policy;
using go2cpp::cache::PolicyName;
using go2cpp::cache::Workload;
using go2cpp::cache::WorkloadKind;
using go2cpp::cache::WorkloadOptions;

/// @brief 一次对比的参数。
struct Options {
    std::size_t operations = 200000;   ///< 每个场景的请求数。
    std::size_t capacity = 1000;       ///< 条目容量。
    std::size_t max_weight = 5000;     ///< GDSF 的权重预算。
    std::uint64_t seed = 0x5DEECE66DULL;  ///< 负载种子。
};

/// @brief 一个策略在一个场景下的结果。
struct Result {
    const char* policy = "";      ///< 策略名。
    std::uint64_t hits = 0;       ///< 对象命中数。
    std::uint64_t total = 0;      ///< 请求总数。
    std::uint64_t hit_bytes = 0;  ///< 命中的 weight 之和。
    std::uint64_t total_bytes = 0;///< 请求的 weight 之和。
    CacheStats stats;             ///< 缓存统计快照。

    double HitRate() const noexcept {
        return total == 0 ? 0.0 : static_cast<double>(hits) / static_cast<double>(total);
    }
    double ByteHitRate() const noexcept {
        return total_bytes == 0
                   ? 0.0
                   : static_cast<double>(hit_bytes) / static_cast<double>(total_bytes);
    }
};

/// @brief 一个场景的定义。
struct Scenario {
    const char* name = "";
    const char* description = "";
    WorkloadKind kind = WorkloadKind::kHotspot;
    bool uses_weight = false;  ///< 是否启用 GDSF 的权重上限。
};

/// @brief 7 种策略（顺序固定，便于表格逐行对照）。
constexpr Policy kPolicies[] = {
    Policy::kLru,     Policy::kLruK, Policy::kTwoQueue, Policy::kTinyLfu,
    Policy::kGdsf,    Policy::kArc,  Policy::kClock,
};

/// @brief 生成 3 个场景的负载参数。
std::vector<Scenario> MakeScenarios() {
    return {
        {"hotspot", "80/20 热点 + 长冷尾（容量≈热点 key 数）", WorkloadKind::kHotspot, false},
        {"scan", "热点中每 2000 请求插入 2000 条顺序扫描", WorkloadKind::kScan, false},
        {"weighted", "20% 大对象（weight=1000，小对象=1）", WorkloadKind::kWeighted, true},
    };
}

/// @brief 生成某个场景的请求序列。
std::vector<CacheRequest> GenerateWorkload(const Scenario& scenario, const Options& options) {
    WorkloadOptions workload_options;
    workload_options.operations = options.operations;
    workload_options.seed = options.seed;
    // 热点 key 数 = 容量，让「刚好装满」成为基线；冷尾是热点的 100 倍。
    workload_options.hot_keys = options.capacity;
    workload_options.cold_keys = options.capacity * 100;
    workload_options.scan_period = 2000;
    workload_options.scan_length = 2000;
    return Workload(scenario.kind, workload_options).Generate();
}

/// @brief 跑一个策略，返回命中率与统计。
Result RunPolicy(Policy policy, const std::vector<CacheRequest>& requests,
                 const Options& options, bool uses_weight) {
    CacheOptions cache_options;
    cache_options.capacity = options.capacity;
    // 只有 GDSF 消费 max_weight；其他策略按条目数淘汰，这里传同样的值不产生
    // 影响（接口语义见 cache.hpp 的「权重语义」一节）。
    cache_options.max_weight = uses_weight ? options.max_weight : 0;
    const auto cache = MakeCache<std::uint64_t, std::string>(policy, cache_options);

    Result result;
    result.policy = PolicyName(policy);
    result.total = requests.size();
    for (const CacheRequest& request : requests) {
        std::string value;
        result.total_bytes += request.weight;
        if (cache->Get(request.key, &value)) {
            ++result.hits;
            result.hit_bytes += request.weight;
        } else {
            cache->Put(request.key, std::to_string(request.key), request.weight);
        }
    }
    result.stats = cache->Stats();
    return result;
}

/// @brief 打印一个场景的对比表。
void PrintScenario(const Scenario& scenario, const std::vector<Result>& results,
                   const Options& options) {
    const std::string weight_note =
        scenario.uses_weight
            ? "权重上限=" + std::to_string(options.max_weight) + "（仅 GDSF 使用）"
            : "按条目数淘汰";
    std::printf("\n场景 %-8s %s\n", scenario.name, scenario.description);
    std::printf("容量=%zu 条，请求=%zu，%s\n", options.capacity, options.operations,
                weight_note.c_str());
    std::printf("%-12s %10s %12s %10s %8s %10s %11s\n", "策略", "命中率", "字节命中率",
                "淘汰", "拒绝", "ghost命中", "条目/容量");
    for (const Result& result : results) {
        const std::string occupancy = std::to_string(result.stats.size) + "/" +
                                      std::to_string(result.stats.capacity);
        std::printf("%-12s %9.2f%% %11.2f%% %10llu %8llu %10llu %11s\n", result.policy,
                    result.HitRate() * 100.0, result.ByteHitRate() * 100.0,
                    static_cast<unsigned long long>(result.stats.evictions),
                    static_cast<unsigned long long>(result.stats.rejections),
                    static_cast<unsigned long long>(result.stats.ghost_hits),
                    occupancy.c_str());
    }

    const Result* best = nullptr;
    for (const Result& result : results) {
        if (best == nullptr || result.HitRate() > best->HitRate()) {
            best = &result;
        }
    }
    if (best != nullptr) {
        std::printf("→ 对象命中率最高：%s（%.2f%%）\n", best->policy, best->HitRate() * 100.0);
    }
    best = nullptr;
    for (const Result& result : results) {
        if (best == nullptr || result.ByteHitRate() > best->ByteHitRate()) {
            best = &result;
        }
    }
    if (best != nullptr) {
        std::printf("→ 字节命中率最高：%s（%.2f%%）\n", best->policy,
                    best->ByteHitRate() * 100.0);
    }
}

/// @brief 打印三个场景的平均命中率排名。
void PrintRanking(const std::vector<std::vector<Result>>& per_scenario) {
    struct Average {
        const char* policy = "";
        double hit_rate = 0.0;
        double byte_hit_rate = 0.0;
    };
    std::vector<Average> averages;
    for (const Result& first : per_scenario.front()) {
        Average average;
        average.policy = first.policy;
        for (const std::vector<Result>& results : per_scenario) {
            for (const Result& result : results) {
                if (std::string(result.policy) == average.policy) {
                    average.hit_rate += result.HitRate();
                    average.byte_hit_rate += result.ByteHitRate();
                }
            }
        }
        average.hit_rate /= static_cast<double>(per_scenario.size());
        average.byte_hit_rate /= static_cast<double>(per_scenario.size());
        averages.push_back(average);
    }
    std::sort(averages.begin(), averages.end(), [](const Average& left, const Average& right) {
        return left.hit_rate > right.hit_rate;
    });
    std::printf("\n三场景平均（按对象命中率排序）\n");
    std::printf("%-4s %-12s %10s %12s\n", "名次", "策略", "平均命中率", "平均字节命中率");
    for (std::size_t i = 0; i < averages.size(); ++i) {
        std::printf("%-4zu %-12s %9.2f%% %11.2f%%\n", i + 1, averages[i].policy,
                    averages[i].hit_rate * 100.0, averages[i].byte_hit_rate * 100.0);
    }
}

/// @brief 解析无符号整数，失败返回 false。
bool ParseUint(const char* text, unsigned long long* out) {
    if (text == nullptr || *text == '\0') {
        return false;
    }
    char* end = nullptr;
    const unsigned long long value = std::strtoull(text, &end, 10);
    if (end == nullptr || *end != '\0') {
        return false;
    }
    *out = value;
    return true;
}

/// @brief 打印用法。
void PrintUsage(const char* program) {
    std::printf(
        "用法: %s [--ops N] [--capacity N] [--max-weight N] [--seed N]\n"
        "  --ops        每个场景的请求数（默认 200000）\n"
        "  --capacity   条目容量（默认 1000）\n"
        "  --max-weight GDSF 的权重预算（默认 5000，仅 weighted 场景生效）\n"
        "  --seed       负载随机种子（默认 0x5DEECE66D）\n",
        program);
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        unsigned long long value = 0;
        if (arg == "--help" || arg == "-h") {
            PrintUsage(argv[0]);
            return 0;
        }
        if (arg == "--ops" && i + 1 < argc) {
            if (!ParseUint(argv[++i], &value) || value == 0) {
                std::fprintf(stderr, "--ops 非法\n");
                return 2;
            }
            options.operations = static_cast<std::size_t>(value);
        } else if (arg == "--capacity" && i + 1 < argc) {
            if (!ParseUint(argv[++i], &value) || value == 0) {
                std::fprintf(stderr, "--capacity 非法\n");
                return 2;
            }
            options.capacity = static_cast<std::size_t>(value);
        } else if (arg == "--max-weight" && i + 1 < argc) {
            if (!ParseUint(argv[++i], &value) || value == 0) {
                std::fprintf(stderr, "--max-weight 非法\n");
                return 2;
            }
            options.max_weight = static_cast<std::size_t>(value);
        } else if (arg == "--seed" && i + 1 < argc) {
            if (!ParseUint(argv[++i], &value)) {
                std::fprintf(stderr, "--seed 非法\n");
                return 2;
            }
            options.seed = static_cast<std::uint64_t>(value);
        } else {
            std::fprintf(stderr, "未知参数: %s\n", arg.c_str());
            PrintUsage(argv[0]);
            return 2;
        }
    }

    const std::vector<Scenario> scenarios = MakeScenarios();
    std::vector<std::vector<Result>> per_scenario;
    for (const Scenario& scenario : scenarios) {
        const std::vector<CacheRequest> requests = GenerateWorkload(scenario, options);
        std::vector<Result> results;
        results.reserve(std::size(kPolicies));
        for (Policy policy : kPolicies) {
            results.push_back(RunPolicy(policy, requests, options, scenario.uses_weight));
        }
        PrintScenario(scenario, results, options);
        per_scenario.push_back(std::move(results));
    }
    PrintRanking(per_scenario);
    return 0;
}
