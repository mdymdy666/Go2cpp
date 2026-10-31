/**
 * @file lru.hpp
 * @brief 基线 LRU（Least Recently Used）缓存。
 *
 * @details 数据结构：
 *   - `std::list<Entry>` 按最近访问排序，head 是 MRU、tail 是 LRU；
 *   - `std::unordered_map<K, list::iterator>` 做 O(1) 定位。
 *
 *         命中时用 `splice` 把节点摘到 head——只改 4 个指针，不分配、
 *         不移动 value，也不会让其它 iterator 失效。淘汰取 tail。
 *         这是「哈希表 + 双向链表」的经典 O(1) 实现，也是所有工业缓存
 *         （memcached、Redis、Guava）的默认策略基线。
 *
 *         复杂度：Get / Put / Erase 均摊 O(1)，空间 O(capacity)。
 *         缺点：对顺序扫描零抵抗——一次全表扫描会把所有热点挤出缓存，
 *         即「cache pollution / scan resistance」问题（见 docs 专节）。
 */
#pragma once

#include <cstddef>
#include <list>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

#include "go2cpp/cache/cache.hpp"

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
        : capacity_(options.capacity) {
        if (capacity_ == 0) {
            throw std::invalid_argument("LruCache: capacity must be >= 1");
        }
    }

    bool Get(const K& key, V* value) override {
        auto it = index_.find(key);
        if (it == index_.end()) {
            stats_.RecordMiss();
            return false;
        }
        lru_.splice(lru_.begin(), lru_, it->second);
        if (value != nullptr) {
            *value = it->second->value;
        }
        stats_.RecordHit();
        return true;
    }

    bool Peek(const K& key, V* value) const override {
        auto it = index_.find(key);
        if (it == index_.end()) {
            return false;
        }
        if (value != nullptr) {
            *value = it->second->value;
        }
        return true;
    }

    void Put(const K& key, V value, std::size_t weight = 1) override {
        auto it = index_.find(key);
        if (it != index_.end()) {
            Entry& entry = *it->second;
            weight_ = weight_ - entry.weight + weight;
            entry.value = std::move(value);
            entry.weight = weight;
            lru_.splice(lru_.begin(), lru_, it->second);
            stats_.RecordUpdate();
            return;
        }
        while (lru_.size() >= capacity_) {
            EvictTail();
        }
        lru_.push_front(Entry{key, std::move(value), weight});
        index_.emplace(key, lru_.begin());
        weight_ += weight;
        stats_.RecordInsert();
    }

    bool Erase(const K& key) override {
        auto it = index_.find(key);
        if (it == index_.end()) {
            return false;
        }
        weight_ -= it->second->weight;
        lru_.erase(it->second);
        index_.erase(it);
        stats_.RecordRemoval();
        return true;
    }

    void Clear() override {
        lru_.clear();
        index_.clear();
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
    struct Entry {
        K key;
        V value;
        std::size_t weight;
    };

    using List = std::list<Entry>;

    void EvictTail() {
        if (lru_.empty()) {
            return;
        }
        auto tail = std::prev(lru_.end());
        weight_ -= tail->weight;
        index_.erase(tail->key);
        lru_.erase(tail);
        stats_.RecordEviction();
    }

    std::size_t capacity_;
    List lru_;
    std::unordered_map<K, typename List::iterator, Hash, Equal> index_;
    std::size_t weight_ = 0;
    StatsRecorder stats_;
};

}  // namespace cache
}  // namespace go2cpp
