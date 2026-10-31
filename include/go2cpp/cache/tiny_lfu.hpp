/**
 * @file tiny_lfu.hpp
 * @brief W-TinyLFU（Caffeine, 2015）实现：窗口 LRU + SLRU 主缓存 + 频率准入。
 *
 * @details TinyLFU 的核心不是「淘汰谁」，而是「谁有资格进来」：
 *          用一个近似频率素描（Count-Min Sketch）估计 key 的历史访问次数，
 *          新条目只有在频率高于当前受害者时才允许替换它。W-TinyLFU 在此
 *          基础上加了三段式布局：
 *
 *   ```text
 *   ┌──────────── 1% ────────────┬──────── main 99% ──────────────┐
 *   │ window (LRU)               │ probation (20%)   protected(80%)│
 *   │ 新条目先落这里，抵御突发     │ 准入区            热数据保护区   │
 *   └────────────────────────────┴────────────────────────────────┘
 *   ```
 *
 *          - 新条目进 window；window 满后，尾部成为「候选」；
 *          - 候选与 probation 尾部（受害者）比较频率，胜者留在主缓存；
 *          - window / probation 命中都提升到 protected（真正热的数据）；
 *          - protected 满时把尾部降级回 probation。
 *
 *          频率素描用 4-bit 饱和计数器 + 定期 halving（见 detail/sketch.hpp），
 *          门卫 Bloom 让「一次性扫描 key」不消耗素描计数。扫描抗性来自
 *          两点：窗口只占 1%，扫描条目在窗口内互相淘汰；即使进入准入比较，
 *          其频率也低于真实热点，被拒绝。
 *
 *          复杂度：Get / Put 均摊 O(1)（素描固定 4 路），空间 O(capacity)
 *          + 素描（约 16 bytes/entry 量级）。
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <list>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

#include "go2cpp/cache/cache.hpp"
#include "go2cpp/cache/detail/sketch.hpp"

namespace go2cpp {
namespace cache {

/**
 * @brief W-TinyLFU 缓存。
 */
template <class K, class V, class Hash = std::hash<K>,
          class Equal = std::equal_to<K>>
class TinyLfuCache final : public Cache<K, V> {
public:
    /** @brief 构造；capacity 必须 >= 1。 */
    explicit TinyLfuCache(CacheOptions options = CacheOptions{})
        : capacity_(options.capacity),
          window_capacity_(WindowCapacity(options.capacity)),
          main_capacity_(capacity_ - window_capacity_),
          probation_capacity_(ProbationCapacity(main_capacity_)),
          protected_capacity_(main_capacity_ - probation_capacity_),
          sketch_(options.capacity, options.seed),
          doorkeeper_(options.capacity),
          seed_(options.seed),
          sample_size_(options.capacity < 10U ? 100U : options.capacity * 10U) {
        if (capacity_ == 0) {
            throw std::invalid_argument("TinyLfuCache: capacity must be >= 1");
        }
    }
    bool Get(const K& key, V* value) override {
        const std::uint64_t hash = HashOf(key);
        RecordFrequency(hash);
        auto window_it = window_index_.find(key);
        if (window_it != window_index_.end()) {
            if (value != nullptr) {
                *value = window_it->second->value;
            }
            stats_.RecordHit();
            if (main_capacity_ == 0) {
                window_.splice(window_.begin(), window_, window_it->second);
            } else {
                PromoteWindow(window_it);
            }
            return true;
        }
        auto protected_it = protected_index_.find(key);
        if (protected_it != protected_index_.end()) {
            protected_.splice(protected_.begin(), protected_,
                              protected_it->second);
            if (value != nullptr) {
                *value = protected_it->second->value;
            }
            stats_.RecordHit();
            return true;
        }
        auto probation_it = probation_index_.find(key);
        if (probation_it != probation_index_.end()) {
            if (value != nullptr) {
                *value = probation_it->second->value;
            }
            PromoteProbation(probation_it);
            stats_.RecordHit();
            return true;
        }
        stats_.RecordMiss();
        return false;
    }

    bool Peek(const K& key, V* value) const override {
        auto window_it = window_index_.find(key);
        if (window_it != window_index_.end()) {
            if (value != nullptr) {
                *value = window_it->second->value;
            }
            return true;
        }
        auto protected_it = protected_index_.find(key);
        if (protected_it != protected_index_.end()) {
            if (value != nullptr) {
                *value = protected_it->second->value;
            }
            return true;
        }
        auto probation_it = probation_index_.find(key);
        if (probation_it != probation_index_.end()) {
            if (value != nullptr) {
                *value = probation_it->second->value;
            }
            return true;
        }
        return false;
    }

    void Put(const K& key, V value, std::size_t weight = 1) override {
        const std::uint64_t hash = HashOf(key);
        // 与 Caffeine 一致：只读访问才计入频率素描。写路径不计频有两个
        // 好处：① 扫描/回填不会把一次性 key 的频率抬高；② 频率老化的
        // 采样只由真实读流量驱动，不会被批量写入提前触发。
        auto window_it = window_index_.find(key);
        if (window_it != window_index_.end()) {
            Entry& entry = *window_it->second;
            weight_ = weight_ - entry.weight + weight;
            entry.value = std::move(value);
            entry.weight = weight;
            window_.splice(window_.begin(), window_, window_it->second);
            stats_.RecordUpdate();
            return;
        }
        auto protected_it = protected_index_.find(key);
        if (protected_it != protected_index_.end()) {
            Entry& entry = *protected_it->second;
            weight_ = weight_ - entry.weight + weight;
            entry.value = std::move(value);
            entry.weight = weight;
            protected_.splice(protected_.begin(), protected_,
                              protected_it->second);
            stats_.RecordUpdate();
            return;
        }
        auto probation_it = probation_index_.find(key);
        if (probation_it != probation_index_.end()) {
            Entry& entry = *probation_it->second;
            weight_ = weight_ - entry.weight + weight;
            entry.value = std::move(value);
            entry.weight = weight;
            PromoteProbation(probation_it);
            stats_.RecordUpdate();
            return;
        }
        if (main_capacity_ == 0) {
            // 容量为 1：window 就是整个缓存，直接覆盖即可。
            while (!window_.empty()) {
                EvictWindowTail();
            }
            window_.push_front(Entry{key, std::move(value), hash, weight});
            window_index_.emplace(key, window_.begin());
            weight_ += weight;
            stats_.RecordInsert();
            return;
        }
        window_.push_front(Entry{key, std::move(value), hash, weight});
        window_index_.emplace(key, window_.begin());
        weight_ += weight;
        stats_.RecordInsert();
        TrimWindow();
    }

    bool Erase(const K& key) override {
        if (EraseFrom(window_, window_index_, key) ||
            EraseFrom(protected_, protected_index_, key) ||
            EraseFrom(probation_, probation_index_, key)) {
            stats_.RecordRemoval();
            return true;
        }
        return false;
    }

    void Clear() override {
        window_.clear();
        window_index_.clear();
        protected_.clear();
        protected_index_.clear();
        probation_.clear();
        probation_index_.clear();
        sketch_.Clear();
        doorkeeper_.Clear();
        records_ = 0;
        weight_ = 0;
    }

    std::size_t Size() const override {
        return window_index_.size() + protected_index_.size() +
               probation_index_.size();
    }
    std::size_t Capacity() const override { return capacity_; }
    std::size_t Weight() const override { return weight_; }

    CacheStats Stats() const override {
        CacheStats stats = stats_.Snapshot();
        stats.size = Size();
        stats.capacity = capacity_;
        stats.weight = weight_;
        return stats;
    }

    const char* Name() const override { return "W-TinyLFU"; }

    std::string Describe() const override {
        return "W-TinyLFU{capacity=" + std::to_string(capacity_) +
               ", window=" + std::to_string(window_index_.size()) + "/" +
               std::to_string(window_capacity_) +
               ", protected=" + std::to_string(protected_index_.size()) + "/" +
               std::to_string(protected_capacity_) +
               ", probation=" + std::to_string(probation_index_.size()) + "/" +
               std::to_string(probation_capacity_) +
               ", sketch_width=" + std::to_string(sketch_.Width()) +
               ", doorkeeper_fill=" +
               std::to_string(doorkeeper_.FillRatio()) + "}";
    }

private:
    struct Entry {
        K key;
        V value;
        std::uint64_t hash;
        std::size_t weight;
    };

    using EntryList = std::list<Entry>;
    using EntryIndex =
        std::unordered_map<K, typename EntryList::iterator, Hash, Equal>;

    static std::size_t WindowCapacity(std::size_t capacity) noexcept {
        if (capacity < 100U) {
            return 1U;
        }
        const std::size_t one_percent = capacity / 100U;
        return one_percent == 0U ? 1U : one_percent;
    }

    static std::size_t ProbationCapacity(std::size_t main_capacity) noexcept {
        if (main_capacity < 5U) {
            return main_capacity;
        }
        const std::size_t twenty_percent = main_capacity / 5U;
        return twenty_percent == 0U ? 1U : twenty_percent;
    }

    std::uint64_t HashOf(const K& key) const {
        return detail::MixHash(
            static_cast<std::uint64_t>(hasher_(key)), seed_);
    }

    void RecordFrequency(std::uint64_t hash) {
        if (doorkeeper_.TestAndSet(hash)) {
            sketch_.Increment(hash);
        }
        ++records_;
        if (records_ >= sample_size_) {
            sketch_.Halve();
            records_ = 0;
        }
    }

    std::uint32_t Estimate(const Entry& entry) const {
        return sketch_.Estimate(entry.hash);
    }

    std::size_t MainSize() const noexcept {
        return protected_index_.size() + probation_index_.size();
    }

    void TrimWindow() {
        while (window_.size() > window_capacity_) {
            auto tail = std::prev(window_.end());
            window_index_.erase(tail->key);
            Entry candidate = std::move(*tail);
            window_.erase(tail);
            AdmitToMain(std::move(candidate), false);
        }
    }

    void PromoteWindow(typename EntryIndex::iterator window_it) {
        auto list_it = window_it->second;
        Entry candidate = std::move(*list_it);
        window_index_.erase(window_it);
        window_.erase(list_it);
        AdmitToMain(std::move(candidate), true);
    }

    void PromoteProbation(typename EntryIndex::iterator probation_it) {
        auto list_it = probation_it->second;
        Entry entry = std::move(*list_it);
        probation_index_.erase(probation_it);
        probation_.erase(list_it);
        // 已在主缓存内，内部移动不增加主缓存总量。
        InsertProtected(std::move(entry));
    }

    void AdmitToMain(Entry&& candidate, bool promote) {
        if (main_capacity_ == 0) {
            weight_ -= candidate.weight;
            stats_.RecordEviction();
            return;
        }
        if (MainSize() >= main_capacity_) {
            if (probation_.empty() && !protected_.empty()) {
                DemoteProtectedTail();
            }
            if (!probation_.empty()) {
                Entry& victim = probation_.back();
                if (Estimate(candidate) > Estimate(victim)) {
                    EvictProbationTail();
                } else {
                    weight_ -= candidate.weight;
                    stats_.RecordRejection();
                    return;
                }
            } else {
                weight_ -= candidate.weight;
                stats_.RecordRejection();
                return;
            }
        }
        if (promote) {
            InsertProtected(std::move(candidate));
        } else {
            InsertProbation(std::move(candidate));
        }
    }

    void InsertProtected(Entry&& entry) {
        protected_.push_front(std::move(entry));
        protected_index_.emplace(protected_.front().key, protected_.begin());
        while (protected_.size() > protected_capacity_) {
            DemoteProtectedTail();
        }
    }

    void InsertProbation(Entry&& entry) {
        probation_.push_front(std::move(entry));
        probation_index_.emplace(probation_.front().key, probation_.begin());
        // 注意：这里**不**按 probation 目标份额淘汰。probation 是准入区，
        // 主缓存未满时必须允许它增长到主缓存容量，否则「工作集 > 20% ×
        // capacity」的负载会陷入「永远只有 probation 20% 命中、热数据
        // 永远进不了 protected」的死锁。总量由 AdmitToMain 统一把关。
    }

    void DemoteProtectedTail() {
        if (protected_.empty()) {
            return;
        }
        auto tail = std::prev(protected_.end());
        protected_index_.erase(tail->key);
        Entry entry = std::move(*tail);
        protected_.erase(tail);
        InsertProbation(std::move(entry));
    }

    void EvictProbationTail() {
        if (probation_.empty()) {
            return;
        }
        auto tail = std::prev(probation_.end());
        weight_ -= tail->weight;
        probation_index_.erase(tail->key);
        probation_.erase(tail);
        stats_.RecordEviction();
    }

    void EvictWindowTail() {
        if (window_.empty()) {
            return;
        }
        auto tail = std::prev(window_.end());
        weight_ -= tail->weight;
        window_index_.erase(tail->key);
        window_.erase(tail);
        stats_.RecordEviction();
    }

    bool EraseFrom(EntryList& list, EntryIndex& index, const K& key) {
        auto it = index.find(key);
        if (it == index.end()) {
            return false;
        }
        weight_ -= it->second->weight;
        list.erase(it->second);
        index.erase(it);
        return true;
    }

    std::size_t capacity_;
    std::size_t window_capacity_;
    std::size_t main_capacity_;
    std::size_t probation_capacity_;
    std::size_t protected_capacity_;
    EntryList window_;
    EntryIndex window_index_;
    EntryList protected_;
    EntryIndex protected_index_;
    EntryList probation_;
    EntryIndex probation_index_;
    detail::CountMinSketch4 sketch_;
    detail::BloomDoorkeeper doorkeeper_;
    Hash hasher_{};
    std::uint64_t seed_;
    std::uint64_t records_ = 0;
    std::uint64_t sample_size_;
    std::size_t weight_ = 0;
    StatsRecorder stats_;
};

}  // namespace cache
}  // namespace go2cpp
