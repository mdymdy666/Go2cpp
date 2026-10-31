/**
 * @file factory.hpp
 * @brief 策略工厂、线程安全工厂与分片缓存。
 *
 * @details 工厂让「按配置选型」成为可能：调用方只需要一个 `Policy`
 *          枚举和一份 `CacheOptions`，就能拿到统一接口的缓存实例。
 *          `ShardedCache` 是面向多核的横向扩展：按 key 哈希把请求分散到
 *          N 个独立分片，每个分片有自己的锁，避免单一全局锁在 16 核上
 *          成为串行点。代价是容量被切分、LRU 语义变成「每分片 LRU」，
 *          跨分片的全局顺序不再严格。
 */
#pragma once

#include <algorithm>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "go2cpp/cache/arc.hpp"
#include "go2cpp/cache/cache.hpp"
#include "go2cpp/cache/clock.hpp"
#include "go2cpp/cache/gdsf.hpp"
#include "go2cpp/cache/lru.hpp"
#include "go2cpp/cache/lru_k.hpp"
#include "go2cpp/cache/tiny_lfu.hpp"
#include "go2cpp/cache/two_queue.hpp"

namespace go2cpp {
namespace cache {

/** @brief 可选淘汰策略。 */
enum class Policy {
    kLru,      ///< 基线：最近最少使用。
    kLruK,     ///< LRU-2：二次访问才进主缓存，防扫描。
    kTwoQueue, ///< 2Q：A1in / A1out / Am。
    kTinyLfu,  ///< W-TinyLFU：窗口 + SLRU + 频率准入。
    kGdsf,     ///< GDSF：按 cost/size × freq 的加权淘汰。
    kArc,      ///< ARC：自适应 T1/T2/B1/B2。
    kClock,    ///< CLOCK：低开销近似 LRU。
};

/** @brief 策略短名。 */
inline const char* PolicyName(Policy policy) noexcept {
    switch (policy) {
        case Policy::kLru:
            return "LRU";
        case Policy::kLruK:
            return "LRU-K(2)";
        case Policy::kTwoQueue:
            return "2Q";
        case Policy::kTinyLfu:
            return "W-TinyLFU";
        case Policy::kGdsf:
            return "GDSF";
        case Policy::kArc:
            return "ARC";
        case Policy::kClock:
            return "CLOCK";
    }
    return "UNKNOWN";
}

/**
 * @brief 按策略构造缓存。
 *
 * @throws std::invalid_argument 策略参数非法（例如容量为 0）。
 */
template <class K, class V>
std::unique_ptr<Cache<K, V>> MakeCache(Policy policy,
                                       const CacheOptions& options = {}) {
    switch (policy) {
        case Policy::kLru:
            return std::make_unique<LruCache<K, V>>(options);
        case Policy::kLruK:
            return std::make_unique<LruKCache<K, V>>(options);
        case Policy::kTwoQueue:
            return std::make_unique<TwoQueueCache<K, V>>(options);
        case Policy::kTinyLfu:
            return std::make_unique<TinyLfuCache<K, V>>(options);
        case Policy::kGdsf:
            return std::make_unique<GdsfCache<K, V>>(options);
        case Policy::kArc:
            return std::make_unique<ArcCache<K, V>>(options);
        case Policy::kClock:
            return std::make_unique<ClockCache<K, V>>(options);
    }
    throw std::invalid_argument("MakeCache: unknown policy");
}

/**
 * @brief 构造带粗粒度互斥锁的缓存。
 *
 * @tparam Lock 锁类型，默认 `std::mutex`；可传 `go2cpp::sync::Mutex`
 *              获得 Fiber-aware 语义。
 */
template <class K, class V, class Lock = std::mutex>
std::unique_ptr<Cache<K, V>> MakeThreadSafeCache(
    Policy policy, const CacheOptions& options = {}) {
    return std::make_unique<ThreadSafeCache<K, V, Lock>>(
        MakeCache<K, V>(policy, options));
}

/**
 * @brief 分片缓存：按 key 哈希把请求分散到多个独立加锁的子缓存。
 *
 * @details 适用读多写少、多核吞吐优先的场景。容量按分片均分（余数给前
 *          几个分片）；当 `capacity < shards` 时每个分片至少 1 条，实际
 *          总容量会大于配置值（`Capacity()` 返回实际值）。
 *
 *          注意：跨分片的全局 LRU/ARC 顺序不再成立；TinyLFU 的频率素描
 *          也变成每分片独立。需要全局语义时用单个 `ThreadSafeCache`。
 */
template <class K, class V, class Hash = std::hash<K>>
class ShardedCache final : public Cache<K, V> {
public:
    ShardedCache(Policy policy, CacheOptions options, std::size_t shards)
        : policy_(policy), shard_count_(std::max<std::size_t>(1, shards)) {
        shards_.reserve(shard_count_);
        const std::size_t base = options.capacity / shard_count_;
        const std::size_t extra = options.capacity % shard_count_;
        for (std::size_t i = 0; i < shard_count_; ++i) {
            CacheOptions shard_options = options;
            shard_options.capacity =
                base + (i < extra ? 1U : 0U);
            if (shard_options.capacity == 0) {
                shard_options.capacity = 1;
            }
            if (options.max_weight != 0) {
                const std::size_t weight_base =
                    options.max_weight / shard_count_;
                const std::size_t weight_extra =
                    options.max_weight % shard_count_;
                shard_options.max_weight =
                    weight_base + (i < weight_extra ? 1U : 0U);
                if (shard_options.max_weight == 0) {
                    shard_options.max_weight = 1;
                }
            }
            auto cache = MakeCache<K, V>(policy, shard_options);
            capacity_ += cache->Capacity();
            max_weight_ += shard_options.max_weight;
            shards_.push_back(
                std::make_unique<Shard>(std::move(cache)));
        }
    }

    bool Get(const K& key, V* value) override {
        Shard& shard = *shards_[ShardIndex(key)];
        std::lock_guard<std::mutex> guard(shard.mutex);
        return shard.cache->Get(key, value);
    }

    bool Peek(const K& key, V* value) const override {
        Shard& shard = *shards_[ShardIndex(key)];
        std::lock_guard<std::mutex> guard(shard.mutex);
        return shard.cache->Peek(key, value);
    }

    void Put(const K& key, V value, std::size_t weight = 1) override {
        Shard& shard = *shards_[ShardIndex(key)];
        std::lock_guard<std::mutex> guard(shard.mutex);
        shard.cache->Put(key, std::move(value), weight);
    }

    bool Erase(const K& key) override {
        Shard& shard = *shards_[ShardIndex(key)];
        std::lock_guard<std::mutex> guard(shard.mutex);
        return shard.cache->Erase(key);
    }

    void Clear() override {
        for (auto& shard : shards_) {
            std::lock_guard<std::mutex> guard(shard->mutex);
            shard->cache->Clear();
        }
    }

    std::size_t Size() const override {
        std::size_t total = 0;
        for (const auto& shard : shards_) {
            std::lock_guard<std::mutex> guard(shard->mutex);
            total += shard->cache->Size();
        }
        return total;
    }

    std::size_t Capacity() const override { return capacity_; }
    std::size_t Weight() const override {
        std::size_t total = 0;
        for (const auto& shard : shards_) {
            std::lock_guard<std::mutex> guard(shard->mutex);
            total += shard->cache->Weight();
        }
        return total;
    }

    CacheStats Stats() const override {
        CacheStats total;
        for (const auto& shard : shards_) {
            std::lock_guard<std::mutex> guard(shard->mutex);
            const CacheStats part = shard->cache->Stats();
            total.hits += part.hits;
            total.misses += part.misses;
            total.inserts += part.inserts;
            total.updates += part.updates;
            total.evictions += part.evictions;
            total.ghost_hits += part.ghost_hits;
            total.rejections += part.rejections;
            total.removals += part.removals;
            total.size += part.size;
            total.weight += part.weight;
            total.ghost_size += part.ghost_size;
        }
        total.capacity = capacity_;
        total.max_weight = max_weight_;
        return total;
    }

    const char* Name() const override {
        return shards_.empty() ? "SHARDED" : shards_.front()->cache->Name();
    }

    std::string Describe() const override {
        return "Sharded{policy=" + std::string(PolicyName(policy_)) +
               ", shards=" + std::to_string(shard_count_) +
               ", capacity=" + std::to_string(capacity_) + "}";
    }

private:
    struct Shard {
        explicit Shard(std::unique_ptr<Cache<K, V>> inner)
            : cache(std::move(inner)) {}
        mutable std::mutex mutex;
        std::unique_ptr<Cache<K, V>> cache;
    };

    std::size_t ShardIndex(const K& key) const {
        return static_cast<std::size_t>(hasher_(key)) % shard_count_;
    }

    Policy policy_;
    std::size_t shard_count_;
    std::size_t capacity_ = 0;
    std::size_t max_weight_ = 0;
    Hash hasher_{};
    std::vector<std::unique_ptr<Shard>> shards_;
};

}  // namespace cache
}  // namespace go2cpp
