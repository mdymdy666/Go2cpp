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
 */
#pragma once

#include <algorithm>
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
 * @brief 自适应替换缓存（ARC）。
 */
template <class K, class V, class Hash = std::hash<K>,
          class Equal = std::equal_to<K>>
class ArcCache final : public Cache<K, V> {
public:
    /** @brief 构造；capacity 必须 >= 1。 */
    explicit ArcCache(CacheOptions options = CacheOptions{})
        : capacity_(options.capacity) {
        if (capacity_ == 0) {
            throw std::invalid_argument("ArcCache: capacity must be >= 1");
        }
    }

    bool Get(const K& key, V* value) override {
        auto t1_it = t1_index_.find(key);
        if (t1_it != t1_index_.end()) {
            auto list_it = t1_it->second;
            Entry entry = std::move(*list_it);
            t1_index_.erase(t1_it);
            t1_.erase(list_it);
            PushT2(std::move(entry));
            if (value != nullptr) {
                *value = t2_.front().value;
            }
            stats_.RecordHit();
            return true;
        }
        auto t2_it = t2_index_.find(key);
        if (t2_it != t2_index_.end()) {
            t2_.splice(t2_.begin(), t2_, t2_it->second);
            if (value != nullptr) {
                *value = t2_it->second->value;
            }
            stats_.RecordHit();
            return true;
        }
        auto b1_it = b1_index_.find(key);
        if (b1_it != b1_index_.end()) {
            const double delta =
                std::max(1.0, static_cast<double>(b2_.size()) /
                                  static_cast<double>(b1_.size()));
            p_ = std::min(static_cast<double>(capacity_), p_ + delta);
            const K ghost_key = b1_it->first;
            b1_.erase(b1_it->second);
            b1_index_.erase(b1_it);
            RememberPending(ghost_key, false);
            stats_.RecordGhostHit();
            stats_.RecordMiss();
            return false;
        }
        auto b2_it = b2_index_.find(key);
        if (b2_it != b2_index_.end()) {
            const double delta =
                std::max(1.0, static_cast<double>(b1_.size()) /
                                  static_cast<double>(b2_.size()));
            p_ = std::max(0.0, p_ - delta);
            const K ghost_key = b2_it->first;
            b2_.erase(b2_it->second);
            b2_index_.erase(b2_it);
            RememberPending(ghost_key, true);
            stats_.RecordGhostHit();
            stats_.RecordMiss();
            return false;
        }
        stats_.RecordMiss();
        return false;
    }

    bool Peek(const K& key, V* value) const override {
        auto t1_it = t1_index_.find(key);
        if (t1_it != t1_index_.end()) {
            if (value != nullptr) {
                *value = t1_it->second->value;
            }
            return true;
        }
        auto t2_it = t2_index_.find(key);
        if (t2_it != t2_index_.end()) {
            if (value != nullptr) {
                *value = t2_it->second->value;
            }
            return true;
        }
        return false;
    }

    void Put(const K& key, V value, std::size_t weight = 1) override {
        auto t1_it = t1_index_.find(key);
        if (t1_it != t1_index_.end()) {
            auto list_it = t1_it->second;
            Entry entry = std::move(*list_it);
            weight_ = weight_ - entry.weight + weight;
            entry.value = std::move(value);
            entry.weight = weight;
            t1_index_.erase(t1_it);
            t1_.erase(list_it);
            PushT2(std::move(entry));
            stats_.RecordUpdate();
            return;
        }
        auto t2_it = t2_index_.find(key);
        if (t2_it != t2_index_.end()) {
            Entry& entry = *t2_it->second;
            weight_ = weight_ - entry.weight + weight;
            entry.value = std::move(value);
            entry.weight = weight;
            t2_.splice(t2_.begin(), t2_, t2_it->second);
            stats_.RecordUpdate();
            return;
        }
        auto pending_it = pending_.find(key);
        if (pending_it != pending_.end()) {
            const bool from_b2 = pending_it->second;
            pending_.erase(pending_it);
            EvictForInsert(from_b2);
            PushT2(Entry{key, std::move(value), weight});
            weight_ += weight;
            stats_.RecordInsert();
            return;
        }
        auto b1_it = b1_index_.find(key);
        if (b1_it != b1_index_.end()) {
            const K ghost_key = b1_it->first;
            b1_.erase(b1_it->second);
            b1_index_.erase(b1_it);
            EvictForInsert(false);
            PushT2(Entry{std::move(ghost_key), std::move(value), weight});
            weight_ += weight;
            stats_.RecordInsert();
            return;
        }
        auto b2_it = b2_index_.find(key);
        if (b2_it != b2_index_.end()) {
            const K ghost_key = b2_it->first;
            b2_.erase(b2_it->second);
            b2_index_.erase(b2_it);
            EvictForInsert(true);
            PushT2(Entry{std::move(ghost_key), std::move(value), weight});
            weight_ += weight;
            stats_.RecordInsert();
            return;
        }
        EvictForInsert(false);
        PushT1(Entry{key, std::move(value), weight});
        weight_ += weight;
        stats_.RecordInsert();
    }

    bool Erase(const K& key) override {
        auto t1_it = t1_index_.find(key);
        if (t1_it != t1_index_.end()) {
            weight_ -= t1_it->second->weight;
            t1_.erase(t1_it->second);
            t1_index_.erase(t1_it);
            stats_.RecordRemoval();
            return true;
        }
        auto t2_it = t2_index_.find(key);
        if (t2_it != t2_index_.end()) {
            weight_ -= t2_it->second->weight;
            t2_.erase(t2_it->second);
            t2_index_.erase(t2_it);
            stats_.RecordRemoval();
            return true;
        }
        auto b1_it = b1_index_.find(key);
        if (b1_it != b1_index_.end()) {
            b1_.erase(b1_it->second);
            b1_index_.erase(b1_it);
            return false;
        }
        auto b2_it = b2_index_.find(key);
        if (b2_it != b2_index_.end()) {
            b2_.erase(b2_it->second);
            b2_index_.erase(b2_it);
            return false;
        }
        pending_.erase(key);
        return false;
    }

    void Clear() override {
        t1_.clear();
        t1_index_.clear();
        t2_.clear();
        t2_index_.clear();
        b1_.clear();
        b1_index_.clear();
        b2_.clear();
        b2_index_.clear();
        pending_.clear();
        p_ = 0.0;
        weight_ = 0;
    }

    std::size_t Size() const override {
        return t1_index_.size() + t2_index_.size();
    }
    std::size_t Capacity() const override { return capacity_; }
    std::size_t Weight() const override { return weight_; }

    CacheStats Stats() const override {
        CacheStats stats = stats_.Snapshot();
        stats.size = Size();
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
    struct Entry {
        K key;
        V value;
        std::size_t weight;
    };

    using EntryList = std::list<Entry>;
    using EntryIndex =
        std::unordered_map<K, typename EntryList::iterator, Hash, Equal>;
    using KeyList = std::list<K>;
    using KeyIndex =
        std::unordered_map<K, typename KeyList::iterator, Hash, Equal>;

    void PushT1(Entry entry) {
        t1_.push_front(std::move(entry));
        t1_index_.emplace(t1_.front().key, t1_.begin());
    }

    void PushT2(Entry entry) {
        t2_.push_front(std::move(entry));
        t2_index_.emplace(t2_.front().key, t2_.begin());
    }

    void PushGhost(KeyList& ghost, KeyIndex& index, K key) {
        ghost.push_front(std::move(key));
        index.emplace(ghost.front(), ghost.begin());
    }

    void RememberPending(const K& key, bool from_b2) {
        if (pending_.size() >= capacity_) {
            pending_.erase(pending_.begin());
        }
        pending_[key] = from_b2;
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
        auto tail = std::prev(t1_.end());
        t1_index_.erase(tail->key);
        weight_ -= tail->weight;
        K key = std::move(tail->key);
        t1_.erase(tail);
        PushGhost(b1_, b1_index_, std::move(key));
    }

    void DemoteT2Tail() {
        auto tail = std::prev(t2_.end());
        t2_index_.erase(tail->key);
        weight_ -= tail->weight;
        K key = std::move(tail->key);
        t2_.erase(tail);
        PushGhost(b2_, b2_index_, std::move(key));
    }

    void TrimGhosts() {
        while (b1_.size() + b2_.size() > capacity_) {
            if (b1_.size() >= b2_.size()) {
                auto tail = std::prev(b1_.end());
                b1_index_.erase(*tail);
                b1_.erase(tail);
            } else {
                auto tail = std::prev(b2_.end());
                b2_index_.erase(*tail);
                b2_.erase(tail);
            }
        }
    }

    std::size_t capacity_;
    EntryList t1_;
    EntryIndex t1_index_;
    EntryList t2_;
    EntryIndex t2_index_;
    KeyList b1_;
    KeyIndex b1_index_;
    KeyList b2_;
    KeyIndex b2_index_;
    std::unordered_map<K, bool, Hash, Equal> pending_;
    double p_ = 0.0;
    std::size_t weight_ = 0;
    StatsRecorder stats_;
};

}  // namespace cache
}  // namespace go2cpp
