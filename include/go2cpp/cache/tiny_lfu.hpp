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
 *
 * ## 容器层（本次优化）
 *
 *          三段（window/protected/probation）原先各有一张
 *          `unordered_map` + 一条 `std::list`，Get 最坏要查 3 次哈希；
 *          更重要的是**每次段间搬家（窗口命中提升、protected 降级）都要
 *          重新分配一个 list 节点**——实测每次 Get 命中平均触发 3.93 次
 *          `operator new`。现在三段共用一张开放寻址索引（条目自带段标记），
 *          节点由池管理，段间搬家只改 4 个指针：命中路径堆分配降为 0。
 *          频率素描本身（哈希路数、halving 周期）保持不变，因此命中率
 *          与优化前逐位一致。
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
          index_(options.capacity),
          sketch_(options.capacity, options.seed),
          doorkeeper_(options.capacity),
          seed_(options.seed),
          sample_size_(options.capacity < 10U ? 100U : options.capacity * 10U) {
        if (capacity_ == 0) {
            throw std::invalid_argument("TinyLfuCache: capacity must be >= 1");
        }
    }

    ~TinyLfuCache() override {
        // 节点池只回收整块内存，非平凡 value 必须在析构前显式销毁。
        while (Entry* entry = window_.pop_front()) {
            pool_.Destroy(entry);
        }
        while (Entry* entry = protected_.pop_front()) {
            pool_.Destroy(entry);
        }
        while (Entry* entry = probation_.pop_front()) {
            pool_.Destroy(entry);
        }
    }

    bool Get(const K& key, V* value) override {
        const std::uint64_t hash = HashOf(key);
        RecordFrequency(hash);
        Entry* entry = index_.FindHashed(TableHash(hash), key);
        if (entry == nullptr) {
            stats_.RecordMiss();
            return false;
        }
        if (value != nullptr) {
            *value = entry->value;
        }
        switch (entry->segment) {
            case kSegmentWindow:
                if (main_capacity_ == 0) {
                    window_.move_to_front(entry);
                } else {
                    PromoteWindow(entry);
                }
                break;
            case kSegmentProtected:
                protected_.move_to_front(entry);
                break;
            default:
                PromoteProbation(entry);
                break;
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
        const std::uint64_t hash = HashOf(key);
        const std::uint32_t table_hash = TableHash(hash);
        Entry* entry = index_.FindHashed(table_hash, key);
        if (entry != nullptr) {
            weight_ = weight_ - entry->weight + weight;
            entry->value = std::move(value);
            entry->weight = weight;
            switch (entry->segment) {
                case kSegmentWindow:
                    window_.move_to_front(entry);
                    break;
                case kSegmentProtected:
                    protected_.move_to_front(entry);
                    break;
                default:
                    PromoteProbation(entry);
                    break;
            }
            stats_.RecordUpdate();
            return;
        }
        if (main_capacity_ == 0) {
            // 容量为 1：window 就是整个缓存，直接覆盖即可。
            while (!window_.empty()) {
                EvictWindowTail();
            }
            Entry* fresh = pool_.Create(key, std::move(value), hash, weight,
                                        kSegmentWindow);
            window_.push_front(fresh);
            index_.InsertHashed(fresh, table_hash);
            weight_ += weight;
            stats_.RecordInsert();
            return;
        }
        Entry* fresh = pool_.Create(key, std::move(value), hash, weight,
                                    kSegmentWindow);
        window_.push_front(fresh);
        index_.InsertHashed(fresh, table_hash);
        weight_ += weight;
        stats_.RecordInsert();
        TrimWindow();
    }

    bool Erase(const K& key) override {
        Entry* entry = Lookup(key);
        if (entry == nullptr) {
            return false;
        }
        weight_ -= entry->weight;
        switch (entry->segment) {
            case kSegmentWindow:
                window_.erase(entry);
                break;
            case kSegmentProtected:
                protected_.erase(entry);
                break;
            default:
                probation_.erase(entry);
                break;
        }
        index_.Erase(entry);
        pool_.Destroy(entry);
        stats_.RecordRemoval();
        return true;
    }

    void Clear() override {
        while (Entry* entry = window_.pop_front()) {
            pool_.Destroy(entry);
        }
        while (Entry* entry = protected_.pop_front()) {
            pool_.Destroy(entry);
        }
        while (Entry* entry = probation_.pop_front()) {
            pool_.Destroy(entry);
        }
        index_.Clear();
        sketch_.Clear();
        doorkeeper_.Clear();
        records_ = 0;
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

    const char* Name() const override { return "W-TinyLFU"; }

    std::string Describe() const override {
        return "W-TinyLFU{capacity=" + std::to_string(capacity_) +
               ", window=" + std::to_string(window_.size()) + "/" +
               std::to_string(window_capacity_) +
               ", protected=" + std::to_string(protected_.size()) + "/" +
               std::to_string(protected_capacity_) +
               ", probation=" + std::to_string(probation_.size()) + "/" +
               std::to_string(probation_capacity_) +
               ", sketch_width=" + std::to_string(sketch_.Width()) +
               ", doorkeeper_fill=" +
               std::to_string(doorkeeper_.FillRatio()) + "}";
    }

private:
    enum : std::uint8_t {
        kSegmentWindow = 0,     ///< 1% 窗口 LRU。
        kSegmentProtected = 1,  ///< SLRU 保护区。
        kSegmentProbation = 2,  ///< SLRU 准入区。
    };

    /** @brief 条目：三段共用，`segment` 决定挂在哪条链表。 */
    struct Entry : detail::Link<Entry>, detail::HashNode<Entry> {
        Entry(const K& entry_key, V entry_value, std::uint64_t entry_hash,
              std::size_t entry_weight, std::uint8_t entry_segment)
            : key(entry_key),
              value(std::move(entry_value)),
              sketch_hash(entry_hash),
              weight(entry_weight),
              segment(entry_segment) {}

        K key;
        V value;
        std::uint64_t sketch_hash;  ///< 频率素描用的 64 位混合哈希。
        std::size_t weight;
        std::uint8_t segment;
    };

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

    /** @brief 表索引用的 32 位哈希：把素描哈希的高低半折叠，省一次哈希。 */
    static std::uint32_t TableHash(std::uint64_t hash) noexcept {
        return static_cast<std::uint32_t>(hash ^ (hash >> 32U));
    }

    Entry* Lookup(const K& key) const {
        return index_.FindHashed(TableHash(HashOf(key)), key);
    }

    /** @brief 用条目自带的哈希摘索引（避免对 string key 重算哈希）。 */
    void EraseIndex(Entry* entry) {
        index_.Erase(entry);
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
        return sketch_.Estimate(entry.sketch_hash);
    }

    std::size_t MainSize() const noexcept {
        return protected_.size() + probation_.size();
    }

    void TrimWindow() {
        while (window_.size() > window_capacity_) {
            Entry* candidate = window_.back();
            if (candidate == nullptr) {
                return;
            }
            window_.erase(candidate);
            EraseIndex(candidate);
            AdmitToMain(candidate, false);
        }
    }

    void PromoteWindow(Entry* entry) {
        window_.erase(entry);
        EraseIndex(entry);
        AdmitToMain(entry, true);
    }

    void PromoteProbation(Entry* entry) {
        probation_.erase(entry);
        EraseIndex(entry);
        // 已在主缓存内，内部移动不增加主缓存总量。
        InsertProtected(entry);
    }

    /** @brief 候选进入主缓存：与 probation 尾部比较频率后决定接纳/拒绝。 */
    void AdmitToMain(Entry* candidate, bool promote) {
        if (main_capacity_ == 0) {
            weight_ -= candidate->weight;
            stats_.RecordEviction();
            pool_.Destroy(candidate);
            return;
        }
        if (MainSize() >= main_capacity_) {
            if (probation_.empty() && !protected_.empty()) {
                DemoteProtectedTail();
            }
            if (!probation_.empty()) {
                Entry* victim = probation_.back();
                if (Estimate(*candidate) > Estimate(*victim)) {
                    EvictProbationTail();
                } else {
                    weight_ -= candidate->weight;
                    stats_.RecordRejection();
                    pool_.Destroy(candidate);
                    return;
                }
            } else {
                weight_ -= candidate->weight;
                stats_.RecordRejection();
                pool_.Destroy(candidate);
                return;
            }
        }
        if (promote) {
            InsertProtected(candidate);
        } else {
            InsertProbation(candidate);
        }
    }

    void InsertProtected(Entry* entry) {
        entry->segment = kSegmentProtected;
        protected_.push_front(entry);
        index_.InsertHashed(entry, TableHash(entry->sketch_hash));
        while (protected_.size() > protected_capacity_) {
            DemoteProtectedTail();
        }
    }

    void InsertProbation(Entry* entry) {
        entry->segment = kSegmentProbation;
        probation_.push_front(entry);
        index_.InsertHashed(entry, TableHash(entry->sketch_hash));
        // 注意：这里**不**按 probation 目标份额淘汰。probation 是准入区，
        // 主缓存未满时必须允许它增长到主缓存容量，否则「工作集 > 20% ×
        // capacity」的负载会陷入「永远只有 probation 20% 命中、热数据
        // 永远进不了 protected」的死锁。总量由 AdmitToMain 统一把关。
    }

    void DemoteProtectedTail() {
        Entry* tail = protected_.back();
        if (tail == nullptr) {
            return;
        }
        protected_.erase(tail);
        EraseIndex(tail);
        InsertProbation(tail);
    }

    void EvictProbationTail() {
        Entry* tail = probation_.back();
        if (tail == nullptr) {
            return;
        }
        weight_ -= tail->weight;
        probation_.erase(tail);
        EraseIndex(tail);
        pool_.Destroy(tail);
        stats_.RecordEviction();
    }

    void EvictWindowTail() {
        Entry* tail = window_.back();
        if (tail == nullptr) {
            return;
        }
        weight_ -= tail->weight;
        window_.erase(tail);
        EraseIndex(tail);
        pool_.Destroy(tail);
        stats_.RecordEviction();
    }

    std::size_t capacity_;
    std::size_t window_capacity_;
    std::size_t main_capacity_;
    std::size_t probation_capacity_;
    std::size_t protected_capacity_;
    detail::IntrusiveList<Entry> window_;
    detail::IntrusiveList<Entry> protected_;
    detail::IntrusiveList<Entry> probation_;
    detail::BucketMap<K, Entry, Hash, Equal> index_;
    detail::NodePool<Entry> pool_;
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
