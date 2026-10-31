/**
 * @file lru.hpp
 * @brief 基线 LRU（Least Recently Used）缓存。
 *
 * @details 数据结构：
 *   - **侵入式双向链表**按最近访问排序，head 是 MRU、tail 是 LRU；
 *     条目对象自己就是链表节点（`Entry : detail::Link<Entry>`），
 *     不再有独立的 `std::list` 节点，命中时 `move_to_front` 只写 4 个指针。
 *   - **开放寻址哈希表** `detail::FlatMap<K, Entry*>` 做 O(1) 定位：
 *     连续槽数组 + 线性探测，查找不再有桶链指针追逐。
 *   - **节点池** `detail::NodePool<Entry>`：条目按 64 个一块分配，淘汰下来的
 *     节点进自由表复用。稳态（容量已满）下 Put 的堆分配次数为 **0**，
 *     而 `std::list + unordered_map` 版本是 **2 次/插入**。
 *
 *         复杂度：Get / Put / Erase 均摊 O(1)，空间 O(capacity)。
 *         缺点：对顺序扫描零抵抗——一次全表扫描会把所有热点挤出缓存，
 *         即「cache pollution / scan resistance」问题（见 docs 专节）。
 *
 *         语义与优化前逐位一致（同一淘汰顺序、同一统计口径），只是把容器
 *         层换成零分配版本；`tests/cache` 的命中率对拍可以直接验证这一点。
 */
#pragma once

#include <cstddef>
#include <stdexcept>
#include <string>
#include <utility>

#include "go2cpp/cache/cache.hpp"
#include "go2cpp/cache/detail/bucket_map.hpp"
#include "go2cpp/cache/detail/intrusive.hpp"

namespace go2cpp {
namespace cache {

/**
 * @brief 经典 LRU 缓存（哈希表 + 双向链表）。
 *
 * @tparam K key 类型。
 * @tparam V value 类型。
 * @tparam Hash key 哈希函数。
 * @tparam Equal key 相等比较。
 */
template <class K, class V, class Hash = std::hash<K>,
          class Equal = std::equal_to<K>>
class LruCache final : public Cache<K, V> {
public:
    /** @brief 构造；capacity 必须 >= 1。 */
    explicit LruCache(CacheOptions options = CacheOptions{})
        : capacity_(options.capacity), index_(options.capacity) {
        if (capacity_ == 0) {
            throw std::invalid_argument("LruCache: capacity must be >= 1");
        }
    }

    ~LruCache() override {
        // 节点池只回收整块内存，不调用仍存活条目的析构函数；非平凡 value
        // （如 std::string）必须在这里显式销毁，否则析构时泄漏。
        while (Entry* entry = lru_.pop_front()) {
            pool_.Destroy(entry);
        }
    }

    bool Get(const K& key, V* value) override {
        Entry* entry = Lookup(key);
        if (entry == nullptr) {
            stats_.RecordMiss();
            return false;
        }
        lru_.move_to_front(entry);
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
        const std::uint32_t hash = index_.HashKey(key);
        Entry* entry = index_.FindHashed(hash, key);
        if (entry != nullptr) {
            weight_ = weight_ - entry->weight + weight;
            entry->value = std::move(value);
            entry->weight = weight;
            lru_.move_to_front(entry);
            stats_.RecordUpdate();
            return;
        }
        while (lru_.size() >= capacity_) {
            EvictTail();
        }
        Entry* fresh = pool_.Create(key, std::move(value), weight);
        lru_.push_front(fresh);
        index_.InsertHashed(fresh, hash);
        weight_ += weight;
        stats_.RecordInsert();
    }

    bool Erase(const K& key) override {
        Entry* entry = Lookup(key);
        if (entry == nullptr) {
            return false;
        }
        weight_ -= entry->weight;
        index_.Erase(entry);
        lru_.erase(entry);
        pool_.Destroy(entry);
        stats_.RecordRemoval();
        return true;
    }

    void Clear() override {
        while (Entry* entry = lru_.pop_front()) {
            pool_.Destroy(entry);
        }
        index_.Clear();
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
        return stats;
    }

    const char* Name() const override { return "LRU"; }

    std::string Describe() const override {
        return "LRU{capacity=" + std::to_string(capacity_) +
               ", size=" + std::to_string(index_.size()) +
               ", weight=" + std::to_string(weight_) + "}";
    }

private:
    /** @brief 缓存条目：同时是链表节点（prev/next 由 Link 提供）。 */
    struct Entry : detail::Link<Entry>, detail::HashNode<Entry> {
        Entry(const K& entry_key, V entry_value, std::size_t entry_weight)
            : key(entry_key),
              value(std::move(entry_value)),
              weight(entry_weight) {}

        K key;
        V value;
        std::size_t weight;
    };

    Entry* Lookup(const K& key) const { return index_.Find(key); }    void EvictTail() {
        Entry* victim = lru_.back();
        if (victim == nullptr) {
            return;
        }
        weight_ -= victim->weight;
        index_.Erase(victim);
        lru_.erase(victim);
        pool_.Destroy(victim);
        stats_.RecordEviction();
    }

    std::size_t capacity_;
    detail::IntrusiveList<Entry> lru_;
    detail::BucketMap<K, Entry, Hash, Equal> index_;
    detail::NodePool<Entry> pool_;
    std::size_t weight_ = 0;
    StatsRecorder stats_;
};

}  // namespace cache
}  // namespace go2cpp
