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
 *          `pending_`，由紧随其后的 Put 完成提升。
 *
 * ## 容器层（本次优化）
 *
 *          A1in/Am 共用一张**开放寻址索引**（两段互斥，条目自带段标记），
 *          Get 从「查 am -> 查 a1in -> 查 ghost」三次哈希降为两次；
 *          条目/A1out 节点/待提升记录全部走侵入式链表 + 节点池，
 *          插入路径的堆分配从 4 次降到 0 次（稳态）。
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>

#include "go2cpp/cache/cache.hpp"
#include "go2cpp/cache/detail/bucket_map.hpp"
#include "go2cpp/cache/detail/intrusive.hpp"

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
          a1out_capacity_(options.capacity < 2U ? 1U : options.capacity / 2U),
          index_(options.capacity),
          ghost_index_(options.capacity / 2U + 1U),
          pending_index_(16U) {
        if (capacity_ == 0) {
            throw std::invalid_argument("TwoQueueCache: capacity must be >= 1");
        }
    }

    ~TwoQueueCache() override {
        // 节点池只回收整块内存，非平凡 value 必须在析构前显式销毁。
        while (Entry* entry = a1in_.pop_front()) {
            pool_.Destroy(entry);
        }
        while (Entry* entry = am_.pop_front()) {
            pool_.Destroy(entry);
        }
        while (GhostNode* ghost = a1out_.pop_front()) {
            ghost_pool_.Destroy(ghost);
        }
        while (PendingNode* node = pending_.pop_front()) {
            pending_pool_.Destroy(node);
        }
    }

    bool Get(const K& key, V* value) override {
        Entry* entry = Lookup(key);
        if (entry != nullptr) {
            if (entry->segment == kSegmentAm) {
                am_.move_to_front(entry);
            }
            // A1in 是 FIFO：命中不改变位置，这正是 2Q 与 LRU 的关键差别。
            if (value != nullptr) {
                *value = entry->value;
            }
            TakePending(key);
            stats_.RecordHit();
            return true;
        }
        GhostNode* ghost = LookupGhost(key);
        if (ghost != nullptr) {
            // ghost 命中：数据已不在缓存，记下来让下一次 Put 直接进 Am。
            const K ghost_key = ghost->key;
            ghost_index_.Erase(ghost);
            a1out_.erase(ghost);
            ghost_pool_.Destroy(ghost);
            if (pending_.size() >= a1out_capacity_) {
                DropOldestPending();
            }
            PendingNode* node = pending_pool_.Create(ghost_key);
            pending_.push_back(node);
            pending_index_.Insert(node);
            stats_.RecordGhostHit();
            stats_.RecordMiss();
            return false;
        }
        stats_.RecordMiss();
        return false;
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
        if (entry != nullptr) {
            weight_ = weight_ - entry->weight + weight;
            entry->value = std::move(value);
            entry->weight = weight;
            if (entry->segment == kSegmentAm) {
                am_.move_to_front(entry);
            }
            TakePending(key);
            stats_.RecordUpdate();
            return;
        }
        if (TakePending(key)) {
            ReclaimForInsert();
            Insert(entry, kSegmentAm, key, std::move(value), weight);
            stats_.RecordInsert();
            return;
        }
        ReclaimForInsert();
        Insert(entry, kSegmentA1in, key, std::move(value), weight);
        stats_.RecordInsert();
    }

    bool Erase(const K& key) override {
        Entry* entry = Lookup(key);
        if (entry != nullptr) {
            weight_ -= entry->weight;
            index_.Erase(entry);
            if (entry->segment == kSegmentAm) {
                am_.erase(entry);
            } else {
                a1in_.erase(entry);
            }
            pool_.Destroy(entry);
            TakePending(key);
            stats_.RecordRemoval();
            return true;
        }
        GhostNode* ghost = LookupGhost(key);
        if (ghost != nullptr) {
            ghost_index_.Erase(ghost);
            a1out_.erase(ghost);
            ghost_pool_.Destroy(ghost);
            TakePending(key);
        }
        return false;
    }

    void Clear() override {
        while (Entry* entry = a1in_.pop_front()) {
            pool_.Destroy(entry);
        }
        while (Entry* entry = am_.pop_front()) {
            pool_.Destroy(entry);
        }
        while (GhostNode* node = a1out_.pop_front()) {
            ghost_pool_.Destroy(node);
        }
        while (PendingNode* node = pending_.pop_front()) {
            pending_pool_.Destroy(node);
        }
        index_.Clear();
        ghost_index_.Clear();
        pending_index_.Clear();
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
        stats.ghost_size = ghost_index_.size();
        return stats;
    }

    const char* Name() const override { return "2Q"; }

    std::string Describe() const override {
        return "2Q{capacity=" + std::to_string(capacity_) +
               ", a1in=" + std::to_string(a1in_.size()) +
               ", am=" + std::to_string(am_.size()) +
               ", a1out=" + std::to_string(ghost_index_.size()) +
               ", pending=" + std::to_string(pending_.size()) + "}";
    }

private:
    enum : std::uint8_t {
        kSegmentA1in = 0,  ///< 准入 FIFO 段。
        kSegmentAm = 1,    ///< 主 LRU 段。
    };

    /** @brief 驻留条目：A1in 与 Am 共用，`segment` 决定在哪个链表。 */
    struct Entry : detail::Link<Entry>, detail::HashNode<Entry> {
        Entry(const K& entry_key, V entry_value, std::size_t entry_weight,
              std::uint8_t entry_segment)
            : key(entry_key),
              value(std::move(entry_value)),
              weight(entry_weight),
              segment(entry_segment) {}

        K key;
        V value;
        std::size_t weight;
        std::uint8_t segment;
    };

    /** @brief A1out 里的 ghost 节点：只存 key。 */
    struct GhostNode : detail::Link<GhostNode>, detail::HashNode<GhostNode> {
        explicit GhostNode(const K& ghost_key) : key(ghost_key) {}
        K key;
    };

    /** @brief 待提升记录（ghost 命中后，下一次 Put 直接进 Am）。 */
    struct PendingNode : detail::Link<PendingNode>, detail::HashNode<PendingNode> {
        explicit PendingNode(const K& pending_key) : key(pending_key) {}
        K key;
    };

    Entry* Lookup(const K& key) const { return index_.Find(key); }

    GhostNode* LookupGhost(const K& key) const { return ghost_index_.Find(key); }

    /** @brief 若 key 在待提升表里，摘掉并销毁；返回是否找到。 */
    bool TakePending(const K& key) {
        PendingNode* node = pending_index_.Find(key);
        if (node == nullptr) {
            return false;
        }
        pending_index_.Erase(node);
        pending_.erase(node);
        pending_pool_.Destroy(node);
        return true;
    }

    /** @brief pending 超过容量时丢弃最老的一条（与原实现「丢任意一条」等价）。 */
    void DropOldestPending() {
        PendingNode* oldest = pending_.front();
        if (oldest == nullptr) {
            return;
        }
        pending_index_.Erase(oldest);
        pending_.pop_front();
        pending_pool_.Destroy(oldest);
    }

    void Insert(Entry*& out, std::uint8_t segment, const K& key, V value,
                std::size_t weight) {
        out = pool_.Create(key, std::move(value), weight, segment);
        index_.Insert(out);
        if (segment == kSegmentAm) {
            am_.push_front(out);
        } else {
            a1in_.push_front(out);
        }
        weight_ += weight;
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
        Entry* tail = a1in_.back();
        if (tail == nullptr) {
            return;
        }
        const K ghost_key = tail->key;
        weight_ -= tail->weight;
        index_.Erase(tail);
        a1in_.erase(tail);
        pool_.Destroy(tail);
        stats_.RecordEviction();
        GhostNode* ghost = ghost_pool_.Create(ghost_key);
        a1out_.push_front(ghost);
        ghost_index_.Insert(ghost);
        while (a1out_.size() > a1out_capacity_) {
            GhostNode* oldest = a1out_.back();
            ghost_index_.Erase(oldest);
            a1out_.erase(oldest);
            ghost_pool_.Destroy(oldest);
        }
    }

    void EvictAmTail() {
        Entry* tail = am_.back();
        if (tail == nullptr) {
            return;
        }
        weight_ -= tail->weight;
        index_.Erase(tail);
        am_.erase(tail);
        pool_.Destroy(tail);
        stats_.RecordEviction();
    }

    std::size_t capacity_;
    std::size_t a1in_capacity_;
    std::size_t a1out_capacity_;
    detail::IntrusiveList<Entry> a1in_;
    detail::IntrusiveList<Entry> am_;
    detail::IntrusiveList<GhostNode> a1out_;
    detail::IntrusiveList<PendingNode> pending_;
    detail::BucketMap<K, Entry, Hash, Equal> index_;
    detail::BucketMap<K, GhostNode, Hash, Equal> ghost_index_;
    detail::BucketMap<K, PendingNode, Hash, Equal> pending_index_;
    detail::NodePool<Entry> pool_;
    detail::NodePool<GhostNode> ghost_pool_;
    detail::NodePool<PendingNode> pending_pool_;
    std::size_t weight_ = 0;
    StatsRecorder stats_;
};

}  // namespace cache
}  // namespace go2cpp
