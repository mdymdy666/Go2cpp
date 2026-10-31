/**
 * @file two_queue.hpp
 * @brief 2Q 缓存（Johnson & Shasha, VLDB 1994）。
 *
 * @details 2Q 用三个队列解决 LRU 的两个问题（扫描污染、只访问一次的数据
 *          挤掉热点）：
 *
 *   - **A1in**：FIFO 准入队列，新条目先进这里。命中不改变位置——因为
 *     「只读一次」的条目本就不该获得 LRU 的二次机会；
 *   - **A1out**：只存 key 的 ghost 队列（默认容量 = capacity/2）。从 A1in
 *     被挤出的 key 在这里留下「访问历史」；
 *   - **Am**：真正的 LRU 主缓存。只有命中过 A1out 的 key（说明它至少被
 *     访问过两次，且跨过了一次淘汰）才会进入 Am。
 *
 *          A1in 的目标份额是 capacity/4；总条目满时优先从 A1in 回收，A1in
 *          低于目标份额后从 Am 回收。ghost 命中会让 key 在**下一次 Put**
 *          直接进 Am，因此 Am 的份额会随着真实重复访问逐步增长。
 *
 *          复杂度：Get 均摊 O(1)，Put 均摊 O(1)，空间 O(capacity + ghost)。
 *          与论文的差异：论文的 ghost 命中发生在「读磁盘后重新插入」的
 *          路径上；本接口的 Get 不负责加载，因此 ghost 命中的 key 记入
 *          `pending_promote_`，由紧随其后的 Put 完成提升。
 */
#pragma once

#include <cstddef>
#include <list>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "go2cpp/cache/cache.hpp"

namespace go2cpp {
namespace cache {

/**
 * @brief 2Q：A1in（FIFO）+ A1out（ghost）+ Am（LRU）。
 */
template <class K, class V, class Hash = std::hash<K>,
          class Equal = std::equal_to<K>>
class TwoQueueCache final : public Cache<K, V> {
public:
    /** @brief 构造；capacity 必须 >= 1。 */
    explicit TwoQueueCache(CacheOptions options = CacheOptions{})
        : capacity_(options.capacity),
          a1in_capacity_(options.capacity < 4U ? 1U : options.capacity / 4U),
          a1out_capacity_(options.capacity < 2U ? 1U : options.capacity / 2U) {
        if (capacity_ == 0) {
            throw std::invalid_argument("TwoQueueCache: capacity must be >= 1");
        }
    }

    bool Get(const K& key, V* value) override {
        auto am_it = am_index_.find(key);
        if (am_it != am_index_.end()) {
            am_.splice(am_.begin(), am_, am_it->second);
            if (value != nullptr) {
                *value = am_it->second->value;
            }
            pending_.erase(key);
            stats_.RecordHit();
            return true;
        }
        auto a1in_it = a1in_index_.find(key);
        if (a1in_it != a1in_index_.end()) {
            // A1in 是 FIFO：命中不改变位置，这正是 2Q 与 LRU 的关键差别。
            if (value != nullptr) {
                *value = a1in_it->second->value;
            }
            pending_.erase(key);
            stats_.RecordHit();
            return true;
        }
        auto ghost_it = a1out_index_.find(key);
        if (ghost_it != a1out_index_.end()) {
            // ghost 命中：数据已不在缓存，记下来让下一次 Put 直接进 Am。
            const K ghost_key = ghost_it->first;
            a1out_.erase(ghost_it->second);
            a1out_index_.erase(ghost_it);
            if (pending_.size() >= a1out_capacity_) {
                pending_.erase(pending_.begin());
            }
            pending_.insert(ghost_key);
            stats_.RecordGhostHit();
            stats_.RecordMiss();
            return false;
        }
        stats_.RecordMiss();
        return false;
    }

    bool Peek(const K& key, V* value) const override {
        auto am_it = am_index_.find(key);
        if (am_it != am_index_.end()) {
            if (value != nullptr) {
                *value = am_it->second->value;
            }
            return true;
        }
        auto a1in_it = a1in_index_.find(key);
        if (a1in_it != a1in_index_.end()) {
            if (value != nullptr) {
                *value = a1in_it->second->value;
            }
            return true;
        }
        return false;
    }

    void Put(const K& key, V value, std::size_t weight = 1) override {
        auto am_it = am_index_.find(key);
        if (am_it != am_index_.end()) {
            Entry& entry = *am_it->second;
            weight_ = weight_ - entry.weight + weight;
            entry.value = std::move(value);
            entry.weight = weight;
            am_.splice(am_.begin(), am_, am_it->second);
            pending_.erase(key);
            stats_.RecordUpdate();
            return;
        }
        auto a1in_it = a1in_index_.find(key);
        if (a1in_it != a1in_index_.end()) {
            Entry& entry = *a1in_it->second;
            weight_ = weight_ - entry.weight + weight;
            entry.value = std::move(value);
            entry.weight = weight;
            pending_.erase(key);
            stats_.RecordUpdate();
            return;
        }
        if (pending_.erase(key) > 0U) {
            ReclaimForInsert();
            InsertAm(key, std::move(value), weight);
            stats_.RecordInsert();
            return;
        }
        ReclaimForInsert();
        InsertA1in(key, std::move(value), weight);
        stats_.RecordInsert();
    }

    bool Erase(const K& key) override {
        auto am_it = am_index_.find(key);
        if (am_it != am_index_.end()) {
            weight_ -= am_it->second->weight;
            am_.erase(am_it->second);
            am_index_.erase(am_it);
            pending_.erase(key);
            stats_.RecordRemoval();
            return true;
        }
        auto a1in_it = a1in_index_.find(key);
        if (a1in_it != a1in_index_.end()) {
            weight_ -= a1in_it->second->weight;
            a1in_.erase(a1in_it->second);
            a1in_index_.erase(a1in_it);
            pending_.erase(key);
            stats_.RecordRemoval();
            return true;
        }
        auto ghost_it = a1out_index_.find(key);
        if (ghost_it != a1out_index_.end()) {
            a1out_.erase(ghost_it->second);
            a1out_index_.erase(ghost_it);
            pending_.erase(key);
        }
        return false;
    }

    void Clear() override {
        a1in_.clear();
        a1in_index_.clear();
        am_.clear();
        am_index_.clear();
        a1out_.clear();
        a1out_index_.clear();
        pending_.clear();
        weight_ = 0;
    }

    std::size_t Size() const override {
        return a1in_index_.size() + am_index_.size();
    }
    std::size_t Capacity() const override { return capacity_; }
    std::size_t Weight() const override { return weight_; }

    CacheStats Stats() const override {
        CacheStats stats = stats_.Snapshot();
        stats.size = a1in_index_.size() + am_index_.size();
        stats.capacity = capacity_;
        stats.weight = weight_;
        stats.ghost_size = a1out_index_.size();
        return stats;
    }

    const char* Name() const override { return "2Q"; }

    std::string Describe() const override {
        return "2Q{capacity=" + std::to_string(capacity_) +
               ", a1in=" + std::to_string(a1in_index_.size()) +
               ", am=" + std::to_string(am_index_.size()) +
               ", a1out=" + std::to_string(a1out_index_.size()) +
               ", pending=" + std::to_string(pending_.size()) + "}";
    }

private:
    struct Entry {
        K key;
        V value;
        std::size_t weight;
    };

    using EntryList = std::list<Entry>;
    using KeyList = std::list<K>;

    void InsertA1in(const K& key, V value, std::size_t weight) {
        a1in_.push_front(Entry{key, std::move(value), weight});
        a1in_index_.emplace(key, a1in_.begin());
        weight_ += weight;
    }

    void InsertAm(const K& key, V value, std::size_t weight) {
        am_.push_front(Entry{key, std::move(value), weight});
        am_index_.emplace(key, am_.begin());
        weight_ += weight;
    }

    void InsertA1out(K key) {
        a1out_.push_front(std::move(key));
        a1out_index_.emplace(a1out_.front(), a1out_.begin());
        while (a1out_.size() > a1out_capacity_) {
            auto tail = std::prev(a1out_.end());
            a1out_index_.erase(*tail);
            a1out_.erase(tail);
        }
    }

    void ReclaimForInsert() {
        while (a1in_.size() + am_.size() >= capacity_) {
            if (!am_.empty() &&
                (a1in_.size() < a1in_capacity_ || a1in_.empty())) {
                EvictAmTail();
                continue;
            }
            if (!a1in_.empty()) {
                DemoteA1inTail();
                continue;
            }
            if (!am_.empty()) {
                EvictAmTail();
            }
        }
    }

    void DemoteA1inTail() {
        auto tail = std::prev(a1in_.end());
        a1in_index_.erase(tail->key);
        K ghost_key = std::move(tail->key);
        weight_ -= tail->weight;
        a1in_.erase(tail);
        stats_.RecordEviction();
        InsertA1out(std::move(ghost_key));
    }

    void EvictAmTail() {
        if (am_.empty()) {
            return;
        }
        auto tail = std::prev(am_.end());
        weight_ -= tail->weight;
        am_index_.erase(tail->key);
        am_.erase(tail);
        stats_.RecordEviction();
    }

    std::size_t capacity_;
    std::size_t a1in_capacity_;
    std::size_t a1out_capacity_;
    EntryList a1in_;
    std::unordered_map<K, typename EntryList::iterator, Hash, Equal>
        a1in_index_;
    EntryList am_;
    std::unordered_map<K, typename EntryList::iterator, Hash, Equal> am_index_;
    KeyList a1out_;
    std::unordered_map<K, typename KeyList::iterator, Hash, Equal>
        a1out_index_;
    std::unordered_set<K, Hash, Equal> pending_;
    std::size_t weight_ = 0;
    StatsRecorder stats_;
};

}  // namespace cache
}  // namespace go2cpp
