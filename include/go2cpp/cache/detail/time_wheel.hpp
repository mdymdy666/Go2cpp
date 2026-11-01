/**
 * @file time_wheel.hpp
 * @brief LRU-K 参考时间索引：**256 桶时间轮 + 桶内侵入式链表 + 桶最小值
 *        256 叶锦标赛树**。
 *
 * @details LRU-K(2) 的主表需要始终能回答「谁的 reference time 最小」。
 *          057257b 之前用 `std::map<uint64_t, Entry*>`：每次命中都要
 *          `extract` 旧 key、`insert` 新 key，各走一次红黑树（约 11 层
 *          指针追逐）；淘汰再走一次 `begin()`。虽然 C++17 的节点复用已经
 *          把每次命中的堆分配降到 0，但 **O(log n) 的树遍历本身仍在热路径上**。
 *
 *          本文件用时间轮替换这棵树。时间戳由 `LruKCache::clock_` 单调
 *          递增产生，天然适合按「时间片」分桶：
 *
 *            - **256 个桶**，桶号 = `(timestamp >> 8) & 255`，即每桶覆盖
 *              256 个 tick，一圈覆盖 65536 个 tick；
 *            - 每个桶是一条**侵入式双向链表**（复用条目自己的 `prev/next`），
 *              插入/删除都是 6 次指针改写，**O(1)、零分配**；
 *            - 每个桶维护自己的 `min`（桶内 reference time 最小的条目），
 *              当桶的 min 被摘走时桶内重扫一次。时间戳唯一，同一圈
 *              （65536 tick）的同一 256 tick 窗口内最多 256 个条目；
 *              跨圈共用同一个桶时链表会变长，但只在该桶 min 被摘时重扫，
 *              常规命中路径不付这个成本；
 *            - 256 个桶的 min 组成一棵**定长锦标赛树**（512 个槽位），
 *              根就是全局最老条目，读取 O(1)；只有桶 min 变化时才自底向上
 *              刷新 8 层，常规命中路径通常一次都不刷。
 *
 *          **回绕保护**：桶号只负责「分桶」，桶内与锦标赛树里的比较一律用
 *          **绝对时间戳**，绝不用桶号大小。即使两个时间戳相差 65536 的整数倍
 *          落进同一个桶，淘汰仍然取绝对时间最小的那个；跨圈不会把新条目
 *          误判成最老条目。
 *
 *          为什么不是「从最老桶开始逐个扫」：桶号在回绕后不再单调，且
 *          时间戳可能跨很多圈（corral 里的旧条目晋升时 reference time 可以
 *          远小于当前时钟）。逐个桶扫既会扫到空桶，也无法只凭桶号判断
 *          新旧。锦标赛树的叶子是**各桶的绝对最小值**，根节点直接给出
 *          全局最老条目，既保留「从最老桶淘汰」的语义，又消除回绕歧义。
 *
 *          契约：`Entry` 必须公开继承 `detail::Link<Entry>`（提供 `prev` /
 *          `next`），并含 `std::uint64_t reference_time`。时间轮只接管
 *          **主表条目**；corral 条目仍由 `IntrusiveList` 使用同一对
 *          `prev/next`，两者互斥（一个条目不可能同时位于 corral 与主表）。
 */
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace go2cpp {
namespace cache {
namespace detail {

/**
 * @brief 以 64 位时间戳为键的定长侵入式时间轮。
 *
 * @tparam Entry 条目类型；需公开继承 `Link<Entry>` 且含 `reference_time`。
 *
 * @note 不拥有条目：`Clear()` 只清空桶头/树，不析构任何条目。
 */
template <class Entry>
class TimestampWheel {
public:
    /** @brief 桶数（2 的幂，桶号用位与取模）。 */
    static constexpr std::size_t kBuckets = 256;
    /** @brief 每个桶覆盖的 tick 位数：256 tick/桶。 */
    static constexpr unsigned kBucketShift = 8;
    /** @brief 一圈覆盖的 tick 数（256 × 256 = 65536）。 */
    static constexpr std::uint64_t kTicksPerRound = 65536;

    TimestampWheel() = default;
    TimestampWheel(const TimestampWheel&) = delete;
    TimestampWheel& operator=(const TimestampWheel&) = delete;
    ~TimestampWheel() = default;

    /** @brief 当前条目数。 */
    std::size_t size() const noexcept { return size_; }

    /** @brief 是否为空。 */
    bool empty() const noexcept { return size_ == 0; }

    /** @brief 全局 reference time 最小的条目；空轮返回 nullptr。 */
    Entry* Min() const noexcept { return tree_[1]; }

    /** @brief 插入一个**不在轮内**的条目（键取 `entry->reference_time`）。 */
    void Insert(Entry* entry) noexcept {
        const std::size_t bucket = BucketOf(entry->reference_time);
        entry->prev = nullptr;
        entry->next = heads_[bucket];
        if (heads_[bucket] != nullptr) {
            heads_[bucket]->prev = entry;
        }
        heads_[bucket] = entry;
        ++size_;
        if (mins_[bucket] == nullptr ||
            entry->reference_time < min_keys_[bucket]) {
            mins_[bucket] = entry;
            min_keys_[bucket] = entry->reference_time;
            Refresh(bucket);
        }
    }

    /**
     * @brief 摘除一个**在轮内**的条目（键取 `entry->reference_time`）。
     *
     * @details 只有当被摘条目正好是桶 min 时才重扫该桶；普通命中（条目
     *          不是桶 min）是纯指针操作。
     */
    void Erase(Entry* entry) noexcept {
        const std::size_t bucket = BucketOf(entry->reference_time);
        if (entry->prev != nullptr) {
            entry->prev->next = entry->next;
        } else {
            heads_[bucket] = entry->next;
        }
        if (entry->next != nullptr) {
            entry->next->prev = entry->prev;
        }
        entry->prev = nullptr;
        entry->next = nullptr;
        --size_;
        if (mins_[bucket] == entry) {
            RescanBucket(bucket);
            Refresh(bucket);
        }
    }

    /** @brief 清空桶头与锦标赛树（不析构条目）。 */
    void Clear() noexcept {
        heads_.fill(nullptr);
        mins_.fill(nullptr);
        min_keys_.fill(0);
        tree_.fill(nullptr);
        tree_keys_.fill(0);
        size_ = 0;
    }

private:
    static std::size_t BucketOf(std::uint64_t timestamp) noexcept {
        return static_cast<std::size_t>(
            (timestamp >> kBucketShift) & (kBuckets - 1U));
    }

    /** @brief 重扫一个桶，更新它的 min（桶可能已空）。 */
    void RescanBucket(std::size_t bucket) noexcept {
        Entry* best = heads_[bucket];
        if (best == nullptr) {
            mins_[bucket] = nullptr;
            min_keys_[bucket] = 0;
            return;
        }
        for (Entry* node = best->next; node != nullptr; node = node->next) {
            if (node->reference_time < best->reference_time) {
                best = node;
            }
        }
        mins_[bucket] = best;
        min_keys_[bucket] = best->reference_time;
    }

    /** @brief 自底向上刷新锦标赛树的一条路径（8 层，定长无分配）。 */
    void Refresh(std::size_t bucket) noexcept {
        std::size_t node = kBuckets + bucket;
        tree_[node] = mins_[bucket];
        tree_keys_[node] = min_keys_[bucket];
        while (node > 1) {
            node >>= 1;
            const std::size_t left = node << 1;
            const std::size_t right = left | 1U;
            // 空槽视为 +inf；比较绝对时间戳，回绕不影响顺序。
            const bool take_left =
                tree_[left] != nullptr &&
                (tree_[right] == nullptr ||
                 tree_keys_[left] < tree_keys_[right]);
            const std::size_t chosen = take_left ? left : right;
            tree_[node] = tree_[chosen];
            tree_keys_[node] = tree_keys_[chosen];
        }
    }

    std::array<Entry*, kBuckets> heads_{};            ///< 桶头（侵入式链表）。
    std::array<Entry*, kBuckets> mins_{};             ///< 桶内最老条目。
    std::array<std::uint64_t, kBuckets> min_keys_{};  ///< 桶 min 的时间戳。
    std::array<Entry*, 2U * kBuckets> tree_{};        ///< 锦标赛树（1 为根）。
    std::array<std::uint64_t, 2U * kBuckets> tree_keys_{};  ///< 树节点键。
    std::size_t size_ = 0;
};

}  // namespace detail
}  // namespace cache
}  // namespace go2cpp
