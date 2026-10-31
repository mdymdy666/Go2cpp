/**
 * @file workload.hpp
 * @brief 可复现的缓存负载生成器（测试与基准共用）。
 *
 * @details 五个场景对应真实系统的典型访问模式：
 *
 *   | 场景 | 模式 | 要验证的能力 |
 *   |---|---|---|
 *   | `kHotspot` | 80/20 热点（含冷尾） | 基本命中率、recency |
 *   | `kScan` | 热点中周期插入顺序扫描 | 防冷数据污染（scan resistance） |
 *   | `kWeighted` | 20% 大对象（1:1000） | 权重/大小感知淘汰 |
 *   | `kCdn` | Zipf 视频 + 分片顺序 + 大小分层 | 长尾 + 大对象混合 |
 *   | `kMixed` | 热点 + 扫描 + 短突发 | 综合压力 |
 *
 *          所有场景都由固定 seed 驱动，同一组参数生成的请求序列完全一致，
 *          因此不同策略可以在**逐请求相同**的输入上比较命中率，不存在
 *          「负载不同导致的不公平」。
 *
 *          key 空间约定：热点 `[0, hot_keys)`，冷尾 `[hot_keys,
 *          hot_keys + cold_keys)`，扫描 key 从 `kScanBase` 开始，突发 key
 *          从 `kBurstBase` 开始，CDN 从 `kCdnBase` 开始，互不重叠。
 */
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace go2cpp {
namespace cache {

/** @brief 负载类型。 */
enum class WorkloadKind {
    kHotspot,  ///< 80/20 热点 + 冷尾。
    kScan,     ///< 热点 + 周期性顺序扫描。
    kWeighted, ///< 20% 大对象（1:1000）的混合负载。
    kCdn,      ///< CDN：Zipf 视频 + 分片 + 大小分层。
    kMixed,    ///< 热点 + 扫描 + 突发。
};

/** @brief 场景名。 */
inline const char* WorkloadName(WorkloadKind kind) noexcept {
    switch (kind) {
        case WorkloadKind::kHotspot:
            return "hotspot";
        case WorkloadKind::kScan:
            return "scan";
        case WorkloadKind::kWeighted:
            return "weighted";
        case WorkloadKind::kCdn:
            return "cdn";
        case WorkloadKind::kMixed:
            return "mixed";
    }
    return "unknown";
}

/** @brief 负载生成参数。 */
struct WorkloadOptions {
    std::size_t hot_keys = 1000;
    std::size_t cold_keys = 100000;
    std::size_t operations = 200000;
    std::uint64_t seed = 0x5DEECE66DULL;
    /** 扫描/混合场景中每次扫描的长度（连续唯一 key 数）。 */
    std::size_t scan_length = 2000;
    /** 扫描触发周期（每 N 个热点请求插入一次扫描）。 */
    std::size_t scan_period = 2000;
    /** 混合场景突发长度。 */
    std::size_t burst_length = 8;
    /** CDN 视频数。 */
    std::size_t videos = 1000;
    /** CDN 每个视频的分片数。 */
    std::size_t segments_per_video = 100;
};

/** @brief 单条缓存请求。 */
struct CacheRequest {
    std::uint64_t key = 0;
    std::uint32_t weight = 1;
};

/** @brief key 空间分界，避免不同场景的 key 重叠。 */
constexpr std::uint64_t kScanBase = 1ULL << 32U;
constexpr std::uint64_t kBurstBase = 2ULL << 32U;
constexpr std::uint64_t kCdnBase = 3ULL << 32U;

/** @brief 可复现的负载生成器。 */
class Workload {
public:
    explicit Workload(WorkloadKind kind,
                      const WorkloadOptions& options = WorkloadOptions{})
        : kind_(kind), options_(options), rng_(options.seed) {}

    /** @brief 生成完整请求序列（长度 = options.operations）。 */
    std::vector<CacheRequest> Generate() {
        std::vector<CacheRequest> requests;
        requests.reserve(options_.operations);
        switch (kind_) {
            case WorkloadKind::kHotspot:
                GenerateHotspot(requests);
                break;
            case WorkloadKind::kScan:
                GenerateScan(requests);
                break;
            case WorkloadKind::kWeighted:
                GenerateWeighted(requests);
                break;
            case WorkloadKind::kCdn:
                GenerateCdn(requests);
                break;
            case WorkloadKind::kMixed:
                GenerateMixed(requests);
                break;
        }
        return requests;
    }

    WorkloadKind kind() const noexcept { return kind_; }

private:
    std::uint64_t UniformKey(std::size_t pool) {
        if (pool == 0) {
            return 0;
        }
        std::uniform_int_distribution<std::size_t> dist(0, pool - 1);
        return static_cast<std::uint64_t>(dist(rng_));
    }

    CacheRequest HotspotRequest() {
        std::bernoulli_distribution hot(0.8);
        const std::uint64_t key =
            hot(rng_) ? UniformKey(options_.hot_keys)
                      : options_.hot_keys + UniformKey(options_.cold_keys);
        return CacheRequest{key, 1};
    }

    void GenerateHotspot(std::vector<CacheRequest>& out) {
        while (out.size() < options_.operations) {
            out.push_back(HotspotRequest());
        }
    }

    void GenerateScan(std::vector<CacheRequest>& out) {
        std::uint64_t scan_cursor = kScanBase;
        std::size_t scan_remaining = 0;
        std::size_t since_scan = 0;
        while (out.size() < options_.operations) {
            if (scan_remaining == 0 && since_scan >= options_.scan_period) {
                scan_remaining = options_.scan_length;
                since_scan = 0;
            }
            if (scan_remaining > 0) {
                out.push_back(CacheRequest{scan_cursor++, 1});
                --scan_remaining;
            } else {
                out.push_back(HotspotRequest());
                ++since_scan;
            }
        }
    }

    void GenerateWeighted(std::vector<CacheRequest>& out) {
        std::bernoulli_distribution hot(0.8);
        std::bernoulli_distribution big(0.2);
        while (out.size() < options_.operations) {
            const bool from_hot = hot(rng_);
            const std::uint64_t key =
                from_hot ? UniformKey(options_.hot_keys)
                         : options_.hot_keys + UniformKey(options_.cold_keys);
            const std::uint32_t weight = big(rng_) ? 1000U : 1U;
            out.push_back(CacheRequest{key, weight});
        }
    }

    void GenerateCdn(std::vector<CacheRequest>& out) {
        // 视频热度服从 Zipf(alpha = 0.9)：少数视频占大部分请求。
        std::vector<double> weights(options_.videos);
        double total = 0.0;
        for (std::size_t i = 0; i < weights.size(); ++i) {
            weights[i] = 1.0 / std::pow(static_cast<double>(i + 1), 0.9);
            total += weights[i];
        }
        for (double& value : weights) {
            value /= total;
        }
        std::discrete_distribution<std::size_t> video_dist(weights.begin(),
                                                          weights.end());
        std::uniform_int_distribution<std::size_t> segment_dist(
            0, options_.segments_per_video - 1);
        while (out.size() < options_.operations) {
            const std::size_t video = video_dist(rng_);
            const std::size_t segment = segment_dist(rng_);
            const std::uint64_t key = kCdnBase +
                                      static_cast<std::uint64_t>(video) *
                                          options_.segments_per_video +
                                      segment;
            // 分片大小按 64 KiB ~ 4 MiB 分 7 档（以 64 KiB 为单位）。
            const std::uint32_t weight = 1U << (6U + (segment % 7U));
            out.push_back(CacheRequest{key, weight});
        }
    }

    void GenerateMixed(std::vector<CacheRequest>& out) {
        std::uint64_t scan_cursor = kScanBase;
        std::uint64_t burst_cursor = kBurstBase;
        std::size_t scan_remaining = 0;
        std::size_t burst_remaining = 0;
        std::size_t since_scan = 0;
        const std::size_t period =
            options_.scan_period == 0 ? 2000 : options_.scan_period;
        // 扫描占每周期 25% 的请求：用周期而不是「20% 概率触发 2000 长
        // 扫描」，否则扫描会级联占满整个序列（热点请求几乎没有机会出现）。
        const std::size_t scan_length = period / 4U == 0 ? 1U : period / 4U;
        std::bernoulli_distribution burst(0.10);
        while (out.size() < options_.operations) {
            if (scan_remaining > 0) {
                out.push_back(CacheRequest{scan_cursor++, 1});
                --scan_remaining;
                continue;
            }
            if (burst_remaining > 0) {
                out.push_back(CacheRequest{burst_cursor, 1});
                --burst_remaining;
                continue;
            }
            if (++since_scan >= period) {
                since_scan = 0;
                scan_remaining = scan_length;
                continue;
            }
            if (burst(rng_)) {
                ++burst_cursor;
                burst_remaining = options_.burst_length;
                continue;
            }
            out.push_back(HotspotRequest());
        }
    }

    WorkloadKind kind_;
    WorkloadOptions options_;
    std::mt19937_64 rng_;
};

}  // namespace cache
}  // namespace go2cpp
