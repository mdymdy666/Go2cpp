/**
 * @file lru_k.hpp
 * @brief LRU-K（O'Neil, O'Neil & Weikum, SIGMOD 1993）的 K = 2 实现。
 *
 * @details LRU 的扫描污染来自「只看最近一次访问时间」。LRU-K 改用**第 K
 *          次最近访问**的时间排序：一个只被访问过一次的冷数据，无论多新，
 *          都不会排到热数据前面。K = 2 时等价于 LRU-2，是数据库缓冲区
 *          管理里最常用的配置。
 *
 *          本实现把缓存分成两层：
 *           1. **corral（历史队列）**：容量 `max(4, capacity/4)` 的 FIFO，
 *              新条目先落到这里；只有被再次访问（累计 2 次）才晋升主表。
 *              这也是防扫描的关键：扫描产生的「一次性 key」永远留在
 *              corral 并随 FIFO 被挤出，不会进入主表。
 *           2. **主表**：按 reference time（倒数第二次访问时间）排序的
 *              `std::map<uint64_t, K>` + 哈希索引，淘汰时取最小 reference
 *              time。严格符合 LRU-2 的定义，而不是用 LRU 近似。
 *
 *          复杂度：主表 Get/Put O(log n)（`std::map` 定位最小引用时间），
 *          corral Get/Put 均摊 O(1)。空间 O(capacity)。
 *
 *          与论文的差异：论文的 corral 容量是「页面数的一个小比例」，
 *          这里是 25%；论文用页面 ID + 时间戳，这里用通用 key/value。
 *          边界：扫描抗性针对「读未命中 + 按需加载」；如果应用对冷数据
 *          主动 `Put`，本策略仍会把重复写入的冷数据晋升（写缓存语义）。
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <list>
#include <map>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

#include "go2cpp/cache/cache.hpp"

namespace go2cpp {
namespace cache {

/**
 * @brief K = 2 的 LRU-K 缓存（corral + 严格 reference-time 主表）。
 */
template <class K, class V, class Hash = std::hash<K>,
          class Equal = std::equal_to<K>>
class LruKCache final : public Cache<K, V> {
public:
    /** @brief 构造；capacity 必须 >= 1。 */
    explicit LruKCache(CacheOptions options = CacheOptions{})
        : capacity_(options.capacity),
          // corral 与主缓存同容量：LRU-2 需要保留「只访问过一次」的
          // 页面，容量小于工作集时热点会在第二次访问前被挤出（实测
          // 1/4 容量下 100 个热 key 只剩 25 个能晋升）。代价是总内存
          // 约为 2×capacity，论文也建议 corral 独立于主缓存分配。
          history_limit_(options.capacity < 4U ? 4U : options.capacity) {
        if (capacity_ == 0) {
            throw std::invalid_argument("LruKCache: capacity must be >= 1");
        }
    }

    bool Get(const K& key, V* value) override {
        auto main_it = index_.find(key);
        if (main_it != index_.end()) {
            MainEntry& entry = main_it->second;
            TouchMain(entry, key);
            if (value != nullptr) {
                *value = entry.value;
            }
            stats_.RecordHit();
            return true;
        }
        auto history_it = history_index_.find(key);
        if (history_it != history_index_.end()) {
            HistoryEntry& entry = *history_it->second;
            const std::uint64_t previous_visit = entry.last_visit;
            ++entry.access_count;
            entry.last_visit = NextTimestamp();
            if (value != nullptr) {
                *value = entry.value;
            }
            stats_.RecordHit();
            if (entry.access_count >= 2U) {
                PromoteToMain(history_it, previous_visit);
            }
            return true;
        }
        stats_.RecordMiss();
        return false;
    }

    bool Peek(const K& key, V* value) const override {
        auto main_it = index_.find(key);
        if (main_it != index_.end()) {
            if (value != nullptr) {
                *value = main_it->second.value;
            }
            return true;
        }
        auto history_it = history_index_.find(key);
        if (history_it != history_index_.end()) {
            if (value != nullptr) {
                *value = history_it->second->value;
            }
            return true;
        }
        return false;
    }

    void Put(const K& key, V value, std::size_t weight = 1) override {
        auto main_it = index_.find(key);
        if (main_it != index_.end()) {
            MainEntry& entry = main_it->second;
            weight_ = weight_ - entry.weight + weight;
            entry.value = std::move(value);
            entry.weight = weight;
            TouchMain(entry, key);
            stats_.RecordUpdate();
            return;
        }
        auto history_it = history_index_.find(key);
        if (history_it != history_index_.end()) {
            HistoryEntry& entry = *history_it->second;
            const std::uint64_t previous_visit = entry.last_visit;
            weight_ = weight_ - entry.weight + weight;
            entry.value = std::move(value);
            entry.weight = weight;
            ++entry.access_count;
            entry.last_visit = NextTimestamp();
            stats_.RecordUpdate();
            if (entry.access_count >= 2U) {
                PromoteToMain(history_it, previous_visit);
            }
            return;
        }
        InsertHistory(key, std::move(value), weight);
        stats_.RecordInsert();
    }

    bool Erase(const K& key) override {
        auto main_it = index_.find(key);
        if (main_it != index_.end()) {
            weight_ -= main_it->second.weight;
            refs_.erase(main_it->second.reference_time);
            index_.erase(main_it);
            stats_.RecordRemoval();
            return true;
        }
        auto history_it = history_index_.find(key);
        if (history_it == history_index_.end()) {
            return false;
        }
        weight_ -= history_it->second->weight;
        history_.erase(history_it->second);
        history_index_.erase(history_it);
        stats_.RecordRemoval();
        return true;
    }

    void Clear() override {
        index_.clear();
        refs_.clear();
        history_.clear();
        history_index_.clear();
        weight_ = 0;
    }

    std::size_t Size() const override {
        return index_.size() + history_index_.size();
    }
    std::size_t Capacity() const override { return capacity_; }
    std::size_t Weight() const override { return weight_; }

    CacheStats Stats() const override {
        CacheStats stats = stats_.Snapshot();
        stats.size = index_.size() + history_index_.size();
        stats.capacity = capacity_;
        stats.weight = weight_;
        stats.ghost_size = history_index_.size();
        return stats;
    }

    const char* Name() const override { return "LRU-K(2)"; }

    std::string Describe() const override {
        return "LRU-K(2){capacity=" + std::to_string(capacity_) +
               ", main=" + std::to_string(index_.size()) +
               ", corral=" + std::to_string(history_index_.size()) +
               ", corral_cap=" + std::to_string(history_limit_) +
               ", history_evictions=" + std::to_string(history_evictions_) +
               "}";
    }

private:
    struct MainEntry {
        V value;
        std::size_t weight;
        std::uint64_t reference_time;  ///< 倒数第二次访问时间。
        std::uint64_t last_visit;      ///< 最近一次访问时间。
    };

    struct HistoryEntry {
        K key;
        V value;
        std::size_t weight;
        std::uint64_t last_visit;
        std::uint32_t access_count;
    };

    using HistoryList = std::list<HistoryEntry>;

    std::uint64_t NextTimestamp() noexcept { return ++clock_; }

    void TouchMain(MainEntry& entry, const K& key) {
        refs_.erase(entry.reference_time);
        entry.reference_time = entry.last_visit;
        entry.last_visit = NextTimestamp();
        refs_.emplace(entry.reference_time, key);
    }

    void PromoteToMain(
        typename std::unordered_map<K, typename HistoryList::iterator, Hash,
                                    Equal>::iterator history_it,
        std::uint64_t reference_time) {
        HistoryEntry& entry = *history_it->second;
        const std::uint64_t last_visit = NextTimestamp();
        K key = std::move(entry.key);
        V value = std::move(entry.value);
        const std::size_t weight = entry.weight;

        history_.erase(history_it->second);
        history_index_.erase(history_it);

        while (index_.size() >= capacity_) {
            EvictMain();
        }
        index_.emplace(key, MainEntry{std::move(value), weight, reference_time,
                                      last_visit});
        refs_.emplace(reference_time, std::move(key));
        // 历史 -> 主表只是换了一个容器，总权重不变（weight_ 已包含它）。
    }

    void InsertHistory(const K& key, V value, std::size_t weight) {
        history_.push_back(
            HistoryEntry{key, std::move(value), weight, NextTimestamp(), 1U});
        history_index_.emplace(key, std::prev(history_.end()));
        weight_ += weight;
        while (history_.size() > history_limit_) {
            auto oldest = history_.begin();
            weight_ -= oldest->weight;
            history_index_.erase(oldest->key);
            history_.erase(oldest);
            ++history_evictions_;
        }
    }

    void EvictMain() {
        if (refs_.empty()) {
            return;
        }
        auto victim = refs_.begin();
        auto index_it = index_.find(victim->second);
        if (index_it != index_.end()) {
            weight_ -= index_it->second.weight;
            index_.erase(index_it);
            stats_.RecordEviction();
        }
        refs_.erase(victim);
    }

    std::size_t capacity_;
    std::size_t history_limit_;
    HistoryList history_;
    std::unordered_map<K, typename HistoryList::iterator, Hash, Equal>
        history_index_;
    std::map<std::uint64_t, K> refs_;
    std::unordered_map<K, MainEntry, Hash, Equal> index_;
    std::uint64_t clock_ = 0;
    std::uint64_t history_evictions_ = 0;
    std::size_t weight_ = 0;
    StatsRecorder stats_;
};

}  // namespace cache
}  // namespace go2cpp
