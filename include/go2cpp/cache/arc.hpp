/**
 * @file arc.hpp
 * @brief ARC（Adaptive Replacement Cache，Megiddo & Modha, FAST 2003）。
 *
 * @details ARC 同时维护四个列表，用 ghost 历史自动调节「新近性（recency）」
 *          与「频率（frequency）」的权重：
 *
 *   ```text
 *   T1: 只访问过一次的常驻条目（recency）    B1: T1 的 ghost（只存 key）
 *   T2: 至少访问过两次的常驻条目（frequency） B2: T2 的 ghost
 *   ```
 *
 *          参数 `p` 是 T1 的目标大小。命中 B1（淘汰过、又被重新访问的
 *          一次性数据）说明 recency 不够，`p` 增大；命中 B2（淘汰过的热
 *          数据）说明 frequency 不够，`p` 减小。调整量取两个 ghost 列表
 *          大小的比值，最大为 1，因此 ARC 不需要任何手工调参。
 *
 *          REPLACE 规则：当 `|T1| > p`（或待插入 key 在 B2 且 `|T1| == p`）
 *          时从 T1 尾部淘汰到 B1，否则从 T2 尾部淘汰到 B2。
 *
 *          复杂度：Get / Put 均摊 O(1)，空间 O(capacity + ghost)。
 *          扫描抗性：一次性扫描只会在 T1 里进进出出，T2 中的热数据不受
 *          影响；ghost 命中又会把 p 拉回，形成自适应恢复。
 *
 * ## 容器层（本次优化）
 *
 *          T1/T2 合成一张索引（条目自带 `in_t2` 标记），B1/B2 合成一张
 *          ghost 索引，Get 的最坏情况从 4 次 unordered_map 查找降为 2 次；
 *          四类节点（T1/T2 条目、B1/B2 ghost、pending）全部走节点池 +
 *          侵入式链表，插入路径的堆分配从 4 次降到 0 次（稳态）。
 */
#pragma once

#include <algorithm>
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
 * @brief 自适应替换缓存（ARC）。
 */
template <class K, class V, class Hash = std::hash<K>,
          class Equal = std::equal_to<K>>
class ArcCache final : public Cache<K, V> {
public:
    /** @brief 构造；capacity 必须 >= 1。 */
    explicit ArcCache(CacheOptions options = CacheOptions{})
        : capacity_(options.capacity),
          index_(options.capacity),
          ghost_index_(options.capacity),
          pending_index_(16U) {
        if (capacity_ == 0) {
            throw std::invalid_argument("ArcCache: capacity must be >= 1");
        }
    }

    ~ArcCache() override {
        // 节点池只回收整块内存，非平凡 value 必须在析构前显式销毁。
        while (Entry* entry = t1_.pop_front()) {
            pool_.Destroy(entry);
        }
        while (Entry* entry = t2_.pop_front()) {
            pool_.Destroy(entry);
        }
        while (GhostNode* ghost = b1_.pop_front()) {
            ghost_pool_.Destroy(ghost);
        }
        while (GhostNode* ghost = b2_.pop_front()) {
            ghost_pool_.Destroy(ghost);
        }
        while (PendingNode* node = pending_.pop_front()) {
            pending_pool_.Destroy(node);
        }
    }

    bool Get(const K& key, V* value) override {
        Entry* entry = Lookup(key);
        if (entry != nullptr) {
            if (!entry->in_t2) {
                t2_.move_to_front(t1_, entry);
                entry->in_t2 = true;
            } else {
                t2_.move_to_front(entry);
            }
            if (value != nullptr) {
                *value = entry->value;
            }
            stats_.RecordHit();
            return true;
        }
        GhostNode* ghost = LookupGhost(key);
        if (ghost != nullptr) {
            const bool from_b2 = ghost->in_b2;
            if (from_b2) {
                const double delta =
                    std::max(1.0, static_cast<double>(b1_.size()) /
                                      static_cast<double>(b2_.size()));
                p_ = std::max(0.0, p_ - delta);
            } else {
                const double delta =
                    std::max(1.0, static_cast<double>(b2_.size()) /
                                      static_cast<double>(b1_.size()));
                p_ = std::min(static_cast<double>(capacity_), p_ + delta);
            }
            const K ghost_key = ghost->key;
            RemoveGhost(ghost);
            RememberPending(ghost_key, from_b2);
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
            if (!entry->in_t2) {
                t2_.move_to_front(t1_, entry);
                entry->in_t2 = true;
            } else {
                t2_.move_to_front(entry);
            }
            stats_.RecordUpdate();
            return;
        }
        bool from_b2 = false;
        if (TakePending(key, &from_b2)) {
            EvictForInsert(from_b2);
            PushT2(key, std::move(value), weight);
            weight_ += weight;
            stats_.RecordInsert();
            return;
        }
        GhostNode* ghost = LookupGhost(key);
        if (ghost != nullptr) {
            const bool ghost_in_b2 = ghost->in_b2;
            // 先摘索引：key 的有效期到 RemoveGhost 为止，之后再 move 出去。
            ghost_index_.Erase(ghost);
            K ghost_key = std::move(ghost->key);
            if (ghost_in_b2) {
                b2_.erase(ghost);
            } else {
                b1_.erase(ghost);
            }
            ghost_pool_.Destroy(ghost);
            EvictForInsert(ghost_in_b2);
            PushT2(std::move(ghost_key), std::move(value), weight);
            weight_ += weight;
            stats_.RecordInsert();
            return;
        }
        EvictForInsert(false);
        PushT1(key, std::move(value), weight);
        weight_ += weight;
        stats_.RecordInsert();
    }

    bool Erase(const K& key) override {
        Entry* entry = Lookup(key);
        if (entry != nullptr) {
            weight_ -= entry->weight;
            index_.Erase(entry);
            if (entry->in_t2) {
                t2_.erase(entry);
            } else {
                t1_.erase(entry);
            }
            pool_.Destroy(entry);
            stats_.RecordRemoval();
            return true;
        }
        GhostNode* ghost = LookupGhost(key);
        if (ghost != nullptr) {
            RemoveGhost(ghost);
            return false;
        }
        TakePending(key, nullptr);
        return false;
    }

    void Clear() override {
        while (Entry* entry = t1_.pop_front()) {
            pool_.Destroy(entry);
        }
        while (Entry* entry = t2_.pop_front()) {
            pool_.Destroy(entry);
        }
        while (GhostNode* ghost = b1_.pop_front()) {
            ghost_pool_.Destroy(ghost);
        }
        while (GhostNode* ghost = b2_.pop_front()) {
            ghost_pool_.Destroy(ghost);
        }
        while (PendingNode* node = pending_.pop_front()) {
            pending_pool_.Destroy(node);
        }
        index_.Clear();
        ghost_index_.Clear();
        pending_index_.Clear();
        p_ = 0.0;
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
        stats.ghost_size = b1_.size() + b2_.size();
        return stats;
    }

    const char* Name() const override { return "ARC"; }

    std::string Describe() const override {
        return "ARC{capacity=" + std::to_string(capacity_) +
               ", p=" + std::to_string(p_) +
               ", t1=" + std::to_string(t1_.size()) +
               ", t2=" + std::to_string(t2_.size()) +
               ", b1=" + std::to_string(b1_.size()) +
               ", b2=" + std::to_string(b2_.size()) +
               ", pending=" + std::to_string(pending_.size()) + "}";
    }

private:
    /** @brief 常驻条目；`in_t2` 决定它挂在 T1 还是 T2 链表上。 */
    struct Entry : detail::Link<Entry>, detail::HashNode<Entry> {
        Entry(const K& entry_key, V entry_value, std::size_t entry_weight,
              bool entry_in_t2)
            : key(entry_key),
              value(std::move(entry_value)),
              weight(entry_weight),
              in_t2(entry_in_t2) {}

        K key;
        V value;
        std::size_t weight;
        bool in_t2;
    };

    /** @brief ghost 节点：只存 key，`in_b2` 决定挂在 B1 还是 B2。 */
    struct GhostNode : detail::Link<GhostNode>, detail::HashNode<GhostNode> {
        GhostNode(const K& ghost_key, bool ghost_in_b2)
            : key(ghost_key), in_b2(ghost_in_b2) {}
        K key;
        bool in_b2;
    };

    /** @brief 待提升记录：ghost 命中后，下一次 Put 直接进 T2。 */
    struct PendingNode : detail::Link<PendingNode>, detail::HashNode<PendingNode> {
        PendingNode(const K& pending_key, bool pending_from_b2)
            : key(pending_key), from_b2(pending_from_b2) {}
        K key;
        bool from_b2;
    };

    Entry* Lookup(const K& key) const { return index_.Find(key); }

    GhostNode* LookupGhost(const K& key) const { return ghost_index_.Find(key); }

    void PushT1(K key, V value, std::size_t weight) {
        Entry* entry = pool_.Create(key, std::move(value), weight, false);
        t1_.push_front(entry);
        index_.Insert(entry);
    }

    void PushT2(K key, V value, std::size_t weight) {
        Entry* entry = pool_.Create(key, std::move(value), weight, true);
        t2_.push_front(entry);
        index_.Insert(entry);
    }

    void PushGhost(K key, bool in_b2) {
        GhostNode* ghost = ghost_pool_.Create(key, in_b2);
        if (in_b2) {
            b2_.push_front(ghost);
        } else {
            b1_.push_front(ghost);
        }
        ghost_index_.Insert(ghost);
    }

    void RemoveGhost(GhostNode* ghost) {
        ghost_index_.Erase(ghost);
        if (ghost->in_b2) {
            b2_.erase(ghost);
        } else {
            b1_.erase(ghost);
        }
        ghost_pool_.Destroy(ghost);
    }

    void RememberPending(const K& key, bool from_b2) {
        PendingNode* existing = pending_index_.Find(key);
        if (existing != nullptr) {
            existing->from_b2 = from_b2;
            return;
        }
        if (pending_.size() >= capacity_) {
            PendingNode* oldest = pending_.front();
            if (oldest != nullptr) {
                pending_index_.Erase(oldest);
                pending_.pop_front();
                pending_pool_.Destroy(oldest);
            }
        }
        PendingNode* node = pending_pool_.Create(key, from_b2);
        pending_.push_back(node);
        pending_index_.Insert(node);
    }

    bool TakePending(const K& key, bool* from_b2) {
        PendingNode* node = pending_index_.Find(key);
        if (node == nullptr) {
            return false;
        }
        if (from_b2 != nullptr) {
            *from_b2 = node->from_b2;
        }
        pending_index_.Erase(node);
        pending_.erase(node);
        pending_pool_.Destroy(node);
        return true;
    }

    void EvictForInsert(bool from_b2) {
        while (t1_.size() + t2_.size() >= capacity_) {
            if (t1_.empty() && t2_.empty()) {
                return;
            }
            Replace(from_b2);
        }
    }

    void Replace(bool x_in_b2) {
        const bool evict_t1 =
            !t1_.empty() &&
            (static_cast<double>(t1_.size()) > p_ ||
             (x_in_b2 && static_cast<double>(t1_.size()) == p_));
        if (evict_t1) {
            DemoteT1Tail();
        } else if (!t2_.empty()) {
            DemoteT2Tail();
        } else if (!t1_.empty()) {
            DemoteT1Tail();
        }
        TrimGhosts();
        stats_.RecordEviction();
    }

    void DemoteT1Tail() {
        Entry* tail = t1_.back();
        if (tail == nullptr) {
            return;
        }
        index_.Erase(tail);
        weight_ -= tail->weight;
        K key = std::move(tail->key);
        t1_.erase(tail);
        pool_.Destroy(tail);
        PushGhost(std::move(key), false);
    }

    void DemoteT2Tail() {
        Entry* tail = t2_.back();
        if (tail == nullptr) {
            return;
        }
        index_.Erase(tail);
        weight_ -= tail->weight;
        K key = std::move(tail->key);
        t2_.erase(tail);
        pool_.Destroy(tail);
        PushGhost(std::move(key), true);
    }

    void TrimGhosts() {
        while (b1_.size() + b2_.size() > capacity_) {
            if (b1_.size() >= b2_.size()) {
                GhostNode* tail = b1_.back();
                if (tail == nullptr) {
                    return;
                }
                b1_.erase(tail);
                ghost_index_.Erase(tail);
                ghost_pool_.Destroy(tail);
            } else {
                GhostNode* tail = b2_.back();
                if (tail == nullptr) {
                    return;
                }
                b2_.erase(tail);
                ghost_index_.Erase(tail);
                ghost_pool_.Destroy(tail);
            }
        }
    }

    std::size_t capacity_;
    detail::IntrusiveList<Entry> t1_;
    detail::IntrusiveList<Entry> t2_;
    detail::IntrusiveList<GhostNode> b1_;
    detail::IntrusiveList<GhostNode> b2_;
    detail::IntrusiveList<PendingNode> pending_;
    detail::BucketMap<K, Entry, Hash, Equal> index_;
    detail::BucketMap<K, GhostNode, Hash, Equal> ghost_index_;
    detail::BucketMap<K, PendingNode, Hash, Equal> pending_index_;
    detail::NodePool<Entry> pool_;
    detail::NodePool<GhostNode> ghost_pool_;
    detail::NodePool<PendingNode> pending_pool_;
    double p_ = 0.0;
    std::size_t weight_ = 0;
    StatsRecorder stats_;
};

}  // namespace cache
}  // namespace go2cpp
