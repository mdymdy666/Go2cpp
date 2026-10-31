/**
 * @file clock.hpp
 * @brief CLOCK（二次机会）缓存：用引用位近似 LRU，淘汰路径无链表操作。
 *
 * @details CLOCK 把缓存槽位排成一个环，每个槽位一个 referenced 位：
 *          - 命中：置位 referenced；
 *          - 插入：若环上有空槽直接用；否则从 hand 指针顺时针扫描，
 *            遇到 referenced=1 就清 0 并跳过（给它「第二次机会」），
 *            遇到 referenced=0 就替换。
 *
 *          它不需要维护 LRU 链表，也不需要在每次命中时移动节点，因此
 *          命中路径只有一次位写入，内存布局连续（vector<Slot>），对
 *          CPU cache 友好。代价是淘汰顺序只是 LRU 的近似，命中率通常
 *          比严格 LRU 低 1~3 个百分点；在「缓存条目小、QPS 极高」的
 *          场景里这个交换是值得的（Linux 页缓存、PostgreSQL buffer
 *          manager 都用 CLOCK 家族）。
 *
 *          复杂度：Get / Put / Erase 均摊 O(1)（淘汰最多扫描 2×capacity
 *          个槽位），空间 O(capacity) 且无逐条目的堆分配。
 */
#pragma once

#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "go2cpp/cache/cache.hpp"

namespace go2cpp {
namespace cache {

/**
 * @brief CLOCK / second-chance 缓存。
 */
template <class K, class V, class Hash = std::hash<K>,
          class Equal = std::equal_to<K>>
class ClockCache final : public Cache<K, V> {
public:
    /** @brief 构造；capacity 必须 >= 1。 */
    explicit ClockCache(CacheOptions options = CacheOptions{})
        : capacity_(options.capacity), slots_(options.capacity) {
        if (capacity_ == 0) {
            throw std::invalid_argument("ClockCache: capacity must be >= 1");
        }
        free_slots_.reserve(capacity_);
        for (std::size_t i = 0; i < capacity_; ++i) {
            free_slots_.push_back(i);
        }
    }

    bool Get(const K& key, V* value) override {
        auto it = index_.find(key);
        if (it == index_.end()) {
            stats_.RecordMiss();
            return false;
        }
        Slot& slot = slots_[it->second];
        slot.referenced = true;
        if (value != nullptr) {
            *value = *slot.value;
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
            *value = *slots_[it->second].value;
        }
        return true;
    }

    void Put(const K& key, V value, std::size_t weight = 1) override {
        auto it = index_.find(key);
        if (it != index_.end()) {
            Slot& slot = slots_[it->second];
            weight_ = weight_ - slot.weight + weight;
            slot.value = std::move(value);
            slot.weight = weight;
            slot.referenced = true;
            stats_.RecordUpdate();
            return;
        }
        std::size_t victim = 0;
        if (!free_slots_.empty()) {
            victim = free_slots_.back();
            free_slots_.pop_back();
        } else {
            victim = FindVictim();
            Slot& old = slots_[victim];
            index_.erase(*old.key);
            weight_ -= old.weight;
            old.key.reset();
            old.value.reset();
            stats_.RecordEviction();
        }
        Slot& slot = slots_[victim];
        slot.key = key;
        slot.value = std::move(value);
        slot.weight = weight;
        slot.referenced = true;
        index_.emplace(key, victim);
        weight_ += weight;
        stats_.RecordInsert();
    }

    bool Erase(const K& key) override {
        auto it = index_.find(key);
        if (it == index_.end()) {
            return false;
        }
        const std::size_t slot_index = it->second;
        Slot& slot = slots_[slot_index];
        weight_ -= slot.weight;
        slot.key.reset();
        slot.value.reset();
        slot.referenced = false;
        index_.erase(it);
        free_slots_.push_back(slot_index);
        stats_.RecordRemoval();
        return true;
    }

    void Clear() override {
        index_.clear();
        weight_ = 0;
        hand_ = 0;
        free_slots_.clear();
        for (std::size_t i = 0; i < capacity_; ++i) {
            slots_[i] = Slot{};
            free_slots_.push_back(i);
        }
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

    const char* Name() const override { return "CLOCK"; }

    std::string Describe() const override {
        return "CLOCK{capacity=" + std::to_string(capacity_) +
               ", size=" + std::to_string(index_.size()) +
               ", free=" + std::to_string(free_slots_.size()) +
               ", hand=" + std::to_string(hand_) + "}";
    }

private:
    struct Slot {
        std::optional<K> key;
        std::optional<V> value;
        bool referenced = false;
        std::size_t weight = 0;
    };

    std::size_t FindVictim() noexcept {
        while (true) {
            Slot& slot = slots_[hand_];
            if (!slot.key.has_value()) {
                const std::size_t victim = hand_;
                hand_ = (hand_ + 1U) % capacity_;
                return victim;
            }
            if (!slot.referenced) {
                const std::size_t victim = hand_;
                hand_ = (hand_ + 1U) % capacity_;
                return victim;
            }
            slot.referenced = false;
            hand_ = (hand_ + 1U) % capacity_;
        }
    }

    std::size_t capacity_;
    std::vector<Slot> slots_;
    std::unordered_map<K, std::size_t, Hash, Equal> index_;
    std::vector<std::size_t> free_slots_;
    std::size_t hand_ = 0;
    std::size_t weight_ = 0;
    StatsRecorder stats_;
};

}  // namespace cache
}  // namespace go2cpp
