/**
 * @file lru_k_reference.hpp
 * @brief LRU-K(2) 的**旧实现参考副本**：参考时间索引用 `std::map`。
 *
 * @details 本文件是 057257b 版本 `include/go2cpp/cache/lru_k.hpp` 的逐字
 *          副本，仅把类名改为 `LruKReferenceCache` 并放进
 *          `go2cpp::cache::test` 命名空间，供 50 万次随机对拍使用。
 *          **不要在生产代码里包含它**；新增功能只改 `lru_k.hpp`。
 *
 *          保留它的理由：命中率/淘汰顺序必须与旧实现逐位一致，而
 *          「用另一种算法再写一遍」无法排除两边同时写错。这里直接复用
 *          旧代码，保证对拍基准就是线上跑过的语义。
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>

#include "go2cpp/cache/cache.hpp"
#include "go2cpp/cache/detail/bucket_map.hpp"
#include "go2cpp/cache/detail/intrusive.hpp"

namespace go2cpp {
namespace cache {
namespace test {

/**
 * @brief K = 2 的 LRU-K 缓存（corral + 严格 reference-time 主表）。
 */
template <class K, class V, class Hash = std::hash<K>,
          class Equal = std::equal_to<K>>
class LruKReferenceCache final : public Cache<K, V> {
public:
    /** @brief 构造；capacity 必须 >= 1。 */
    explicit LruKReferenceCache(CacheOptions options = CacheOptions{})
        : capacity_(options.capacity),
          // corral 与主缓存同容量：LRU-2 需要保留「只访问过一次」的
          // 页面，容量小于工作集时热点会在第二次访问前被挤出（实测
          // 1/4 容量下 100 个热 key 只剩 25 个能晋升）。代价是总内存
          // 约为 2×capacity，论文也建议 corral 独立于主缓存分配。
          history_limit_(options.capacity < 4U ? 4U : options.capacity),
          index_(options.capacity * 2U) {
        if (capacity_ == 0) {
            throw std::invalid_argument("LruKReferenceCache: capacity must be >= 1");
        }
    }

    ~LruKReferenceCache() override {
        // 节点池只回收整块内存，非平凡 value 必须在析构前显式销毁。
        index_.ForEach([this](Entry* entry) { pool_.Destroy(entry); });
    }

    bool Get(const K& key, V* value) override {
        Entry* entry = Lookup(key);
        if (entry == nullptr) {
            stats_.RecordMiss();
            return false;
        }
        if (!entry->in_main) {
            const std::uint64_t previous_visit = entry->last_visit;
            ++entry->access_count;
            entry->last_visit = NextTimestamp();
            if (value != nullptr) {
                *value = entry->value;
            }
            stats_.RecordHit();
            if (entry->access_count >= 2U) {
                PromoteToMain(entry, previous_visit);
            }
            return true;
        }
        TouchMain(entry);
        if (value != nullptr) {
            *value = entry->value;
        }
        stats_.RecordHit();
        return true;
    }

    bool Peek(const K& key, V* value) const override {
        Entry* entry = Lookup(key);
        if (entry == nullptr) {
            return false;
        }
        if (value != nullptr) {
            *value = entry->value;
        }
        return true;
    }

    void Put(const K& key, V value, std::size_t weight = 1) override {
        Entry* entry = Lookup(key);
        if (entry == nullptr) {
            InsertHistory(key, std::move(value), weight);
            stats_.RecordInsert();
            return;
        }
        weight_ = weight_ - entry->weight + weight;
        entry->value = std::move(value);
        entry->weight = weight;
        if (entry->in_main) {
            TouchMain(entry);
            stats_.RecordUpdate();
            return;
        }
        const std::uint64_t previous_visit = entry->last_visit;
        ++entry->access_count;
        entry->last_visit = NextTimestamp();
        stats_.RecordUpdate();
        if (entry->access_count >= 2U) {
            PromoteToMain(entry, previous_visit);
        }
    }

    bool Erase(const K& key) override {
        Entry* entry = Lookup(key);
        if (entry == nullptr) {
            return false;
        }
        weight_ -= entry->weight;
        if (entry->in_main) {
            refs_.erase(entry->reference_time);
        } else {
            history_.erase(entry);
        }
        index_.Erase(entry);
        pool_.Destroy(entry);
        stats_.RecordRemoval();
        return true;
    }

    void Clear() override {
        index_.ForEach([this](Entry* entry) { pool_.Destroy(entry); });
        index_.Clear();
        refs_.clear();
        history_.clear_links();
        weight_ = 0;
    }

    std::size_t Size() const override { return index_.size(); }
    std::size_t Capacity() const override { return capacity_; }
    std::size_t Weight() const override { return weight_; }

    CacheStats Stats() const override {
        CacheStats stats = stats_.Snapshot();
        stats.size = index_.size();
        stats.capacity = capacity_;
        stats.weight = weight_;
        stats.ghost_size = history_.size();
        return stats;
    }

    const char* Name() const override { return "LRU-K(2)"; }

    std::string Describe() const override {
        return "LRU-K(2){capacity=" + std::to_string(capacity_) +
               ", main=" + std::to_string(MainSize()) +
               ", corral=" + std::to_string(history_.size()) +
               ", corral_cap=" + std::to_string(history_limit_) +
               ", history_evictions=" + std::to_string(history_evictions_) +
               "}";
    }

private:
    /** @brief 条目：corral 时挂 FIFO 链表，主表时只被参考时间索引引用。 */
    struct Entry : detail::Link<Entry>, detail::HashNode<Entry> {
        Entry(const K& entry_key, V entry_value, std::size_t entry_weight,
              std::uint64_t last_visit_time)
            : key(entry_key),
              value(std::move(entry_value)),
              weight(entry_weight),
              last_visit(last_visit_time),
              access_count(1U) {}

        K key;
        V value;
        std::size_t weight;
        std::uint64_t reference_time = 0;  ///< 倒数第二次访问时间（主表）。
        std::uint64_t last_visit = 0;      ///< 最近一次访问时间。
        std::uint32_t access_count = 0;
        bool in_main = false;
    };

    std::uint64_t NextTimestamp() noexcept { return ++clock_; }

    Entry* Lookup(const K& key) const { return index_.Find(key); }

    std::size_t MainSize() const noexcept {
        return index_.size() - history_.size();
    }

    void TouchMain(Entry* entry) {
        // 参考时间整体右移：ref <- last <- now。用 extract/insert 原地换 key，
        // 避免 std::map 每次 erase+emplace 的节点分配/释放。
        auto node = refs_.extract(entry->reference_time);
        entry->reference_time = entry->last_visit;
        entry->last_visit = NextTimestamp();
        if (!node.empty()) {
            node.key() = entry->reference_time;
            node.mapped() = entry;
            refs_.insert(std::move(node));
        } else {
            refs_.emplace(entry->reference_time, entry);
        }
    }

    void PromoteToMain(Entry* entry, std::uint64_t reference_time) {
        const std::uint64_t last_visit = NextTimestamp();
        history_.erase(entry);
        entry->in_main = true;
        // 先把超过容量的主表条目淘汰出空位，再挂参考时间——否则刚晋升的
        // 这一条自己会被挤掉（capacity = 1 时曾经直接死循环）。
        while (MainSize() > capacity_ && !refs_.empty()) {
            EvictMain();
        }
        entry->reference_time = reference_time;
        entry->last_visit = last_visit;
        refs_.emplace(reference_time, entry);
        // 历史 -> 主表只是换了一个容器，总权重不变（weight_ 已包含它）。
    }

    void InsertHistory(const K& key, V value, std::size_t weight) {
        Entry* entry = pool_.Create(key, std::move(value), weight,
                                    NextTimestamp());
        history_.push_back(entry);
        index_.Insert(entry);
        weight_ += weight;
        while (history_.size() > history_limit_) {
            Entry* oldest = history_.front();
            if (oldest == nullptr) {
                break;
            }
            weight_ -= oldest->weight;
            index_.Erase(oldest);
            history_.erase(oldest);
            pool_.Destroy(oldest);
            ++history_evictions_;
        }
    }

    void EvictMain() {
        if (refs_.empty()) {
            return;
        }
        const auto victim = refs_.begin();
        Entry* entry = victim->second;
        weight_ -= entry->weight;
        index_.Erase(entry);
        refs_.erase(victim);
        pool_.Destroy(entry);
        stats_.RecordEviction();
    }

    std::size_t capacity_;
    std::size_t history_limit_;
    detail::IntrusiveList<Entry> history_;
    detail::BucketMap<K, Entry, Hash, Equal> index_;
    detail::NodePool<Entry> pool_;
    std::map<std::uint64_t, Entry*> refs_;
    std::uint64_t clock_ = 0;
    std::uint64_t history_evictions_ = 0;
    std::size_t weight_ = 0;
    StatsRecorder stats_;
};

}  // namespace test
}  // namespace cache
}  // namespace go2cpp
