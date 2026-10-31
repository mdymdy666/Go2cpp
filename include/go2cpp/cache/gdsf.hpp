/**
 * @file gdsf.hpp
 * @brief GDSF（GreedyDual-Size-Frequency，Cao & Irani, 1997）加权缓存。
 *
 * @details GDSF 解决「对象大小差异巨大」时的公平性问题：LRU 只按个数算，
 *          一个 1 MB 对象和一个 1 KB 对象成本相同，结果是缓存被大对象
 *          占满、小对象频繁抖动。GDSF 给每个对象算一个**收益密度**：
 *
 *          ```text
 *              H(key) = L + cost / size × freq(key)
 *          ```
 *
 *          其中 `cost` 是取回成本（默认 1.0），`size` 是对象大小，
 *          `freq` 是访问频率；`L` 是全局「年龄/通胀」水位。每次淘汰当前
 *          H 最小的对象，并令 `L = H(victim)`。把 L 提出公因子后，实际
 *          只需比较 `key_value = cost/size × freq`，淘汰后 `L += key_value`。
 *
 *          这是 GreedyDual 家族（GDS、GDSF、GDSF*）里最常用的成员，
 *          也是 Squid 等代理缓存采用的加权淘汰策略。对「大对象 1:1000
 *          小对象」的场景，它天然偏向单位字节收益更高的小对象。
 *
 *          实现：`std::priority_queue` 小顶堆 + 惰性删除（每次命中 push
 *          新节点，旧节点在出堆时按 version 跳过）。堆项数超过条目数 4 倍
 *          时 O(n) 重建，避免长期运行时堆无限膨胀。
 *
 *          复杂度：Get / Put / Erase 均摊 O(log n)，重建 O(n)。
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <queue>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "go2cpp/cache/cache.hpp"

namespace go2cpp {
namespace cache {

/**
 * @brief GreedyDual-Size-Frequency 缓存。
 *
 * @note `capacity == 0` 表示「不限条目数，只按 max_weight 限制」；
 *       `max_weight == 0` 表示不限制总权重。两者不能同时为 0。
 */
template <class K, class V, class Hash = std::hash<K>,
          class Equal = std::equal_to<K>>
class GdsfCache final : public Cache<K, V> {
public:
    /** @brief 构造；capacity 与 max_weight 不能同时为 0。 */
    explicit GdsfCache(CacheOptions options = CacheOptions{})
        : capacity_(options.capacity),
          max_weight_(options.max_weight),
          cost_(options.fetch_cost) {
        if (capacity_ == 0 && max_weight_ == 0) {
            throw std::invalid_argument(
                "GdsfCache: capacity and max_weight cannot both be zero");
        }
        if (!(cost_ > 0.0)) {
            throw std::invalid_argument("GdsfCache: fetch_cost must be > 0");
        }
    }

    bool Get(const K& key, V* value) override {
        auto it = index_.find(key);
        if (it == index_.end()) {
            stats_.RecordMiss();
            return false;
        }
        Entry& entry = it->second;
        if (entry.freq < kMaxFrequency) {
            ++entry.freq;
        }
        entry.key_value = KeyValue(entry.size, entry.freq);
        // 必须递增 version：堆里可能还有同一 key 的旧节点，旧 key_value
        // 更小，若不失效会在下一次淘汰时被误判为「当前值最小」而淘汰
        // 这个其实很热的条目（曾导致 Get 后反而把自己淘汰掉）。
        entry.version = ++version_;
        PushHeap(key, entry.key_value, entry.version);
        if (value != nullptr) {
            *value = entry.value;
        }
        stats_.RecordHit();
        MaybeCompact();
        return true;
    }

    bool Peek(const K& key, V* value) const override {
        auto it = index_.find(key);
        if (it == index_.end()) {
            return false;
        }
        if (value != nullptr) {
            *value = it->second.value;
        }
        return true;
    }

    void Put(const K& key, V value, std::size_t weight = 1) override {
        if (weight == 0) {
            weight = 1;  // 零大小对象按 1 处理，避免 cost/size 除零。
        }
        if (max_weight_ != 0 && weight > max_weight_) {
            stats_.RecordRejection();
            return;
        }
        auto it = index_.find(key);
        if (it != index_.end()) {
            // 先摘出旧条目，再按插入流程重新评估占用；这样容量/权重
            // 回收逻辑不需要区分「更新」与「新增」，也不会把自己淘汰掉。
            Entry old = std::move(it->second);
            index_.erase(it);
            weight_ -= old.size;
            if (!MakeRoomForInsert(weight)) {
                weight_ += old.size;
                index_.emplace(key, std::move(old));
                stats_.RecordRejection();
                return;
            }
            old.value = std::move(value);
            old.size = weight;
            if (old.freq < kMaxFrequency) {
                ++old.freq;
            }
            old.key_value = KeyValue(old.size, old.freq);
            old.version = ++version_;
            const double key_value = old.key_value;
            const std::uint64_t version = old.version;
            index_.emplace(key, std::move(old));
            weight_ += weight;
            PushHeap(key, key_value, version);
            stats_.RecordUpdate();
            MaybeCompact();
            return;
        }
        if (!MakeRoomForInsert(weight)) {
            stats_.RecordRejection();
            return;
        }
        Entry entry;
        entry.value = std::move(value);
        entry.size = weight;
        entry.freq = 1;
        entry.key_value = KeyValue(weight, entry.freq);
        entry.version = ++version_;
        const double key_value = entry.key_value;
        const std::uint64_t version = entry.version;
        index_.emplace(key, std::move(entry));
        weight_ += weight;
        PushHeap(key, key_value, version);
        stats_.RecordInsert();
        MaybeCompact();
    }

    bool Erase(const K& key) override {
        auto it = index_.find(key);
        if (it == index_.end()) {
            return false;
        }
        weight_ -= it->second.size;
        index_.erase(it);
        stats_.RecordRemoval();
        return true;
    }

    void Clear() override {
        index_.clear();
        heap_ = HeapQueue{};
        weight_ = 0;
        l_ = 0.0;
    }

    std::size_t Size() const override { return index_.size(); }
    std::size_t Capacity() const override { return capacity_; }
    std::size_t Weight() const override { return weight_; }

    CacheStats Stats() const override {
        CacheStats stats = stats_.Snapshot();
        stats.size = index_.size();
        stats.capacity = capacity_;
        stats.weight = weight_;
        stats.max_weight = max_weight_;
        stats.ghost_size =
            heap_.size() > index_.size() ? heap_.size() - index_.size() : 0;
        return stats;
    }

    const char* Name() const override { return "GDSF"; }

    std::string Describe() const override {
        return "GDSF{L=" + std::to_string(l_) +
               ", capacity=" + std::to_string(capacity_) +
               ", max_weight=" + std::to_string(max_weight_) +
               ", size=" + std::to_string(index_.size()) +
               ", weight=" + std::to_string(weight_) +
               ", heap=" + std::to_string(heap_.size()) + "}";
    }

private:
    static constexpr std::uint64_t kMaxFrequency = 1ULL << 40U;

    struct Entry {
        V value;
        std::size_t size = 1;
        std::uint64_t freq = 1;
        double key_value = 0.0;
        std::uint64_t version = 0;
    };

    struct HeapNode {
        double key_value;
        std::uint64_t version;
        K key;
    };

    struct MinHeap {
        bool operator()(const HeapNode& lhs, const HeapNode& rhs) const {
            return lhs.key_value > rhs.key_value;  // 小顶堆。
        }
    };

    using HeapQueue = std::priority_queue<HeapNode, std::vector<HeapNode>,
                                          MinHeap>;

    double KeyValue(std::size_t size, std::uint64_t freq) const {
        return cost_ / static_cast<double>(size) *
               static_cast<double>(freq);
    }

    void PushHeap(const K& key, double key_value, std::uint64_t version) {
        heap_.push(HeapNode{key_value, version, key});
    }

    bool CanFit(std::size_t weight) const {
        if (capacity_ != 0 && index_.size() >= capacity_) {
            return false;
        }
        if (max_weight_ != 0) {
            if (weight > max_weight_ || weight_ > max_weight_ - weight) {
                return false;
            }
        }
        return true;
    }

    bool MakeRoomForInsert(std::size_t weight) {
        while (!CanFit(weight)) {
            if (!EvictOne()) {
                return false;
            }
        }
        return true;
    }

    bool EvictOne() {
        while (!heap_.empty()) {
            const HeapNode node = heap_.top();
            heap_.pop();
            auto it = index_.find(node.key);
            if (it == index_.end() || it->second.version != node.version) {
                continue;  // 惰性删除：过期堆项直接跳过。
            }
            l_ += node.key_value;
            weight_ -= it->second.size;
            index_.erase(it);
            stats_.RecordEviction();
            return true;
        }
        return false;
    }

    void MaybeCompact() {
        if (heap_.size() <= index_.size() * 4U + 64U) {
            return;
        }
        HeapQueue rebuilt;
        for (const auto& item : index_) {
            rebuilt.push(HeapNode{item.second.key_value, item.second.version,
                                  item.first});
        }
        heap_ = std::move(rebuilt);
    }

    std::size_t capacity_;
    std::size_t max_weight_;
    double cost_;
    std::unordered_map<K, Entry, Hash, Equal> index_;
    HeapQueue heap_;
    double l_ = 0.0;
    std::uint64_t version_ = 0;
    std::size_t weight_ = 0;
    StatsRecorder stats_;
};

}  // namespace cache
}  // namespace go2cpp
