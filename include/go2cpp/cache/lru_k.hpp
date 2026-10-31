/**
 * @file lru_k.hpp
 * @brief LRU-K（O'Neil, O'Neil & Weikum, SIGMOD 1993）的 K = 2 实现。
 *
 * @details LRU 的扫描污染来自「只看最近一次访问时间」。LRU-K 改用**第 K
 *          次最近访问**的时间排序：一个只被访问过一次的冷数据，无论多新，
 *          都不会排到热数据前面。K = 2 时等价于 LRU-2，是数据库缓冲区
 *          管理里最常用的配置。
 *
 *          本实现把缓存分成两层：
 *           1. **corral（历史队列）**：容量 `max(4, capacity)` 的 FIFO，
 *              新条目先落到这里；只有被再次访问（累计 2 次）才晋升主表。
 *              这也是防扫描的关键：扫描产生的「一次性 key」永远留在
 *              corral 并随 FIFO 被挤出，不会进入主表。
 *           2. **主表**：按 reference time（倒数第二次访问时间）排序，
 *              淘汰时取主表中 reference time 最小的条目。严格符合 LRU-2
 *              的定义，而不是用 LRU 近似。
 *
 *          复杂度：主表 Get/Put O(log n)（`std::map` 定位最小引用时间），
 *          corral Get/Put 均摊 O(1)。空间 O(capacity)。
 *
 *          与论文的差异：论文的 corral 容量是「页面数的一个小比例」，
 *          这里是 100%；论文用页面 ID + 时间戳，这里用通用 key/value。
 *          边界：扫描抗性针对「读未命中 + 按需加载」；如果应用对冷数据
 *          主动 `Put`，本策略仍会把重复写入的冷数据晋升（写缓存语义）。
 *
 * ## 容器层（本次优化）
 *
 *          优化前这条路径是三套容器的叠加：corral 的 `list` + `unordered_map`、
 *          主表的 `unordered_map`、以及参考时间的 `std::map<uint64_t, K>`。
 *          实测每次 `Get` 命中要发生 **1.02 次 `operator new`**——全部来自
 *          `std::map` 的 erase + emplace（参考时间每次访问都要换一个位置）。
 *
 *          现在：
 *            - 主表与 corral 合并成一张开放寻址索引，条目用 `in_main` 标记；
 *              命中路径只查一次哈希（原先是主表查一次、未命中再查 corral 一次）。
 *            - corral 是侵入式 FIFO 链表，节点来自池，插入 0 分配。
 *            - 参考时间索引仍然是 `std::map`（严格有序语义不变），但用
 *              C++17 的 `extract` + `insert` 原地换 key，**不再分配/释放节点**。
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

/**
 * @brief K = 2 的 LRU-K 缓存（corral + 严格 reference-time 主表）。
 */
template <class K, class V, class Hash = std::hash<K>,
          class Equal = std::equal_to<K>>
class LruKCache final : public Cache<K, V> {
public:
    /** @brief 构造；capacity 必须 >= 1。 */
    explicit LruKCache(CacheOptions options = CacheOptions{})
        : capacity_(options.capacity),
          // corral 与主缓存同容量：LRU-2 需要保留「只访问过一次」的
          // 页面，容量小于工作集时热点会在第二次访问前被挤出（实测
          // 1/4 容量下 100 个热 key 只剩 25 个能晋升）。代价是总内存
          // 约为 2×capacity，论文也建议 corral 独立于主缓存分配。
          history_limit_(options.capacity < 4U ? 4U : options.capacity),
          index_(options.capacity * 2U) {
        if (capacity_ == 0) {
            throw std::invalid_argument("LruKCache: capacity must be >= 1");
        }
    }

    ~LruKCache() override {
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

}  // namespace cache
}  // namespace go2cpp
