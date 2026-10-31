/**
 * @file detail/bucket_map.hpp
 * @brief 缓存策略共用的**侵入式链式哈希索引**（零分配、O(1) 插入/删除）。
 *
 * @details 为什么不是 `std::unordered_map`：它为每个键值对单独 malloc 一个
 *          桶链节点，插入/淘汰各一次分配+释放。为什么也不是开放寻址表：
 *          开放寻址的删除要么留墓碑（墓碑积累到阈值就要整表重建，实测让
 *          插入路径慢 1.5 倍），要么后移填补（每次删除都要搬动探测链上的
 *          若干条目）——而缓存的稳态正是「插一条、淘汰一条」，插入删除各占
 *          一半流量，这两种成本每次都要付。
 *
 *          所以这里用**链式哈希 + 节点侵入**：
 *
 *          - 条目类型公开继承 `HashNode<Derived>`（两条桶链指针 + 32 位哈希），
 *            节点就是条目本身，插入/删除是纯指针改写，**0 次分配**；
 *          - 桶链是双向链表，`Erase(node)` O(1)，不需要从链头找前驱；
 *          - 能力上限（capacity、ghost 数、corral 数）在构造时就已知，
 *            桶数组一次定好、**永不扩容**，因此不需要遍历所有节点重挂；
 *          - 桶数取 2 的幂（`hash & mask`），装载率约 0.5，平均链长 1.0~1.2。
 *
 *          实测在 LRU / 2Q / ARC / W-TinyLFU / CLOCK / LRU-K 上，这套索引
 *          把插入路径的堆分配从 1~4 次降到 0 次，综合吞吐提升 1.5~2.5 倍。
 */
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "go2cpp/cache/detail/hash.hpp"

namespace go2cpp {
namespace cache {
namespace detail {

/**
 * @brief 侵入式哈希节点：条目类型公开继承它，桶链指针就长在条目上。
 *
 * @tparam Derived 条目类型（CRTP）。
 */
template <class Derived>
struct HashNode {
    Derived* hnext = nullptr;      ///< 同桶后继。
    Derived* hprev = nullptr;      ///< 同桶前驱（支持 O(1) 摘除）。
    std::uint32_t hash = 0;        ///< 预计算的 32 位混合哈希。
};

/**
 * @brief 侵入式链式哈希索引（不拥有节点）。
 *
 * @tparam K key 类型。
 * @tparam T 节点类型：必须公开继承 `HashNode<T>`，并含名为 `key` 的成员。
 * @tparam Hash key 哈希函数。
 * @tparam Equal key 相等比较。
 *
 * @note 桶数组在构造时定死，不扩容。调用方必须保证条目数不超过
 *       `expected` 的 2 倍，否则链变长（正确性不受影响，只是变慢）。
 */
template <class K, class T, class Hash = std::hash<K>,
          class Equal = std::equal_to<K>>
class BucketMap {
public:
    /** @brief 构造；`expected` 是预估的最大条目数。 */
    explicit BucketMap(std::size_t expected = 0) {
        buckets_.assign(BucketCountFor(expected), nullptr);
        mask_ = buckets_.size() - 1U;
    }

    BucketMap(const BucketMap&) = delete;
    BucketMap& operator=(const BucketMap&) = delete;

    /** @brief 当前条目数。 */
    std::size_t size() const noexcept { return size_; }

    /** @brief 桶数组长度（2 的幂）。 */
    std::size_t bucket_count() const noexcept { return buckets_.size(); }

    /** @brief 计算 key 的 32 位混合哈希。 */
    std::uint32_t HashKey(const K& key) const noexcept {
        return static_cast<std::uint32_t>(
            SplitMix64(static_cast<std::uint64_t>(hasher_(key))));
    }

    /** @brief 查找；未命中返回 nullptr。 */
    T* Find(const K& key) const noexcept {
        return FindHashed(HashKey(key), key);
    }

    /** @brief 用预计算哈希查找。 */
    T* FindHashed(std::uint32_t hash, const K& key) const noexcept {
        for (T* node = buckets_[static_cast<std::size_t>(hash) & mask_];
             node != nullptr; node = node->hnext) {
            if (node->hash == hash && equal_(node->key, key)) {
                return node;
            }
        }
        return nullptr;
    }

    /** @brief 插入一个**不在表内**的节点；哈希由节点 key 现算。 */
    void Insert(T* node) noexcept { InsertHashed(node, HashKey(node->key)); }

    /** @brief 用预计算哈希插入（调用方保证 key 不在表内）。 */
    void InsertHashed(T* node, std::uint32_t hash) noexcept {
        node->hash = hash;
        const std::size_t bucket = static_cast<std::size_t>(hash) & mask_;
        node->hnext = buckets_[bucket];
        node->hprev = nullptr;
        if (buckets_[bucket] != nullptr) {
            buckets_[bucket]->hprev = node;
        }
        buckets_[bucket] = node;
        ++size_;
    }

    /** @brief O(1) 摘除一个已知节点（不析构，由调用方/池回收）。 */
    void Erase(T* node) noexcept {
        if (node->hprev != nullptr) {
            node->hprev->hnext = node->hnext;
        } else {
            buckets_[static_cast<std::size_t>(node->hash) & mask_] = node->hnext;
        }
        if (node->hnext != nullptr) {
            node->hnext->hprev = node->hprev;
        }
        node->hnext = nullptr;
        node->hprev = nullptr;
        --size_;
    }

    /** @brief 清空索引（不销毁节点）。 */
    void Clear() noexcept {
        std::fill(buckets_.begin(), buckets_.end(), nullptr);
        size_ = 0;
    }

    /** @brief 遍历所有条目：`fn(T*)`。 */
    template <class F>
    void ForEach(F&& fn) {
        for (std::size_t bucket = 0; bucket < buckets_.size(); ++bucket) {
            for (T* node = buckets_[bucket]; node != nullptr;) {
                T* next = node->hnext;
                fn(node);
                node = next;
            }
        }
    }

private:
    static std::size_t BucketCountFor(std::size_t expected) noexcept {
        const std::size_t want = expected < 8U ? 8U : expected * 2U;
        return NextPowerOfTwo(want);
    }

    std::vector<T*> buckets_;
    std::size_t mask_ = 0;
    std::size_t size_ = 0;
    Hash hasher_{};
    Equal equal_{};
};

}  // namespace detail
}  // namespace cache
}  // namespace go2cpp
