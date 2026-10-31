/**
 * @file cache.hpp
 * @brief 缓存淘汰策略的统一接口、统计模型与线程安全装饰器。
 *
 * @details 本模块把「淘汰策略」与「缓存容器语义」解耦：
 *   - `Cache<K, V>` 是纯虚接口，只定义 Get / Put / Erase / Stats 等语义；
 *   - 每种算法（LRU、LRU-K、2Q、W-TinyLFU、GDSF、ARC、CLOCK）各自实现
 *     一份**无锁**策略核心，便于单线程基准测试与算法对拍；
 *   - `ThreadSafeCache<K, V, Lock>` 用装饰器外加锁，把任意策略变成线程安全
 *     缓存；`Lock` 默认 `std::mutex`，也可传 `go2cpp::sync::Mutex` 获得
 *     Fiber-aware 的挂起语义（见文末示例）。
 *
 * ## 权重语义
 *
 * `Put(key, value, weight)` 的 `weight` 表示该条目的**占用单元**（通常是
 * 字节数）。所有策略都会维护并报告总权重；**只有 `GdsfCache` 用权重做
 * 淘汰决策**（`H = L + cost/size * freq`）。其它策略仍按条目数淘汰，权重
 * 仅用于观测与容量核算——这是有意的边界：LRU/2Q/ARC 的论文语义就是
 * 「按对象个数」，强行加权重会破坏它们的不变量（例如 ARC 的 |T1| 约束）。
 *
 * ## 线程模型
 *
 * 裸策略类**不是**线程安全的：并发调用 Get/Put 会破坏内部链表与索引，
 * 统计计数器也会丢更新。需要并发时用 `ThreadSafeCache` 包一层，或者由
 * 调用方保证每个实例只被一个线程访问。`ThreadSafeCache` 目前是粗粒度
 * 互斥（一次只允许一个读者或写者）；这是正确性优先的默认值，读多写少
 * 场景可换成分片包装 `ShardedCache`（见 factory.hpp）。
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

namespace go2cpp {
namespace cache {

/**
 * @brief 缓存构造参数。
 *
 * @details `capacity` 是条目数上限；`max_weight` 是总权重的上限（0 表示
 *          不按权重限制，只有 GDSF 真正消费该字段）。`seed` 用于
 *          TinyLFU 的 Count-Min Sketch / Bloom 哈希，固定 seed 可以让
 *          基准与复现实验得到完全一致的结果。
 */
struct CacheOptions {
    /** @brief 条目数上限；必须 >= 1（GDSF 允许 0 表示只按 max_weight 限制）。 */
    std::size_t capacity = 1024;
    /** @brief 总权重上限；0 表示不限制。当前仅 GDSF 使用。 */
    std::size_t max_weight = 0;
    /** @brief 哈希种子（TinyLFU 频率素描）。 */
    std::uint64_t seed = 0x243F6A8885A308D3ULL;
    /** @brief GDSF 的取回成本 cost（默认 1.0；仅 GDSF 使用）。 */
    double fetch_cost = 1.0;
};

/**
 * @brief 一次统计快照。
 *
 * @details 计数是**自构造以来**的累计值，`Clear()` 不会清零统计；这样
 *          基准可以在同一个实例上分段观察。`Peek()` 不计入命中/未命中。
 */
struct CacheStats {
    std::uint64_t hits = 0;          ///< Get 命中次数（含 2Q/ARC 的 ghost 之外）。
    std::uint64_t misses = 0;        ///< Get 未命中次数。
    std::uint64_t inserts = 0;       ///< 首次插入条目次数（含进入准入队列）。
    std::uint64_t updates = 0;       ///< 覆盖已有 key 的次数。
    std::uint64_t evictions = 0;     ///< 淘汰次数（不含 Clear / Erase）。
    std::uint64_t ghost_hits = 0;    ///< 2Q 的 A1out、ARC 的 B1/B2 命中次数。
    std::uint64_t rejections = 0;    ///< 被策略拒绝的插入（准入失败/容量 0）。
    std::uint64_t removals = 0;      ///< Erase 成功次数。
    std::size_t size = 0;            ///< 当前条目数。
    std::size_t capacity = 0;        ///< 配置的条目上限。
    std::size_t weight = 0;          ///< 当前总权重。
    std::size_t max_weight = 0;      ///< 配置的权重上限（0 = 未启用）。
    std::size_t ghost_size = 0;      ///< ghost / 历史队列的条目数。

    /** @brief Get 总次数。 */
    std::uint64_t Total() const noexcept { return hits + misses; }

    /** @brief 命中率；总次数为 0 时返回 0。 */
    double HitRate() const noexcept {
        const std::uint64_t total = Total();
        return total == 0 ? 0.0
                          : static_cast<double>(hits) / static_cast<double>(total);
    }

    /** @brief 未命中率。 */
    double MissRate() const noexcept { return 1.0 - HitRate(); }
};

/**
 * @brief 策略实现共用的计数器。
 *
 * @details 刻意使用普通 `std::uint64_t` 而非原子：裸策略本来就不保证
 *          线程安全，原子只会给单线程基准增加无谓开销。线程安全由
 *          `ThreadSafeCache` 的锁提供，锁内计数天然串行。
 */
class StatsRecorder {
public:
    void RecordHit() noexcept { ++hits_; }
    void RecordMiss() noexcept { ++misses_; }
    void RecordInsert() noexcept { ++inserts_; }
    void RecordUpdate() noexcept { ++updates_; }
    void RecordEviction() noexcept { ++evictions_; }
    void RecordGhostHit() noexcept { ++ghost_hits_; }
    void RecordRejection() noexcept { ++rejections_; }
    void RecordRemoval() noexcept { ++removals_; }

    /** @brief 生成只含计数的快照（size/weight 由调用方补齐）。 */
    CacheStats Snapshot() const noexcept {
        CacheStats stats;
        stats.hits = hits_;
        stats.misses = misses_;
        stats.inserts = inserts_;
        stats.updates = updates_;
        stats.evictions = evictions_;
        stats.ghost_hits = ghost_hits_;
        stats.rejections = rejections_;
        stats.removals = removals_;
        return stats;
    }

private:
    std::uint64_t hits_ = 0;
    std::uint64_t misses_ = 0;
    std::uint64_t inserts_ = 0;
    std::uint64_t updates_ = 0;
    std::uint64_t evictions_ = 0;
    std::uint64_t ghost_hits_ = 0;
    std::uint64_t rejections_ = 0;
    std::uint64_t removals_ = 0;
};

/**
 * @brief 所有淘汰策略的统一接口。
 *
 * @tparam K key 类型（需可哈希、可比较相等）。
 * @tparam V value 类型（需可拷贝或可移动）。
 *
 * @note `Get`/`Put`/`Erase`/`Clear` 会改变策略状态；`Peek` 是纯查询，
 *       不更新 LRU 顺序、不增加频率、不计统计。`Stats()` 返回累计快照。
 */
template <class K, class V>
class Cache {
public:
    virtual ~Cache() = default;

    /**
     * @brief 读取并更新策略状态（LRU 置 MRU、频率 +1 等）。
     * @param[out] value 命中时写入；允许为空指针（只查询是否存在）。
     * @return 命中返回 true。
     */
    virtual bool Get(const K& key, V* value) = 0;

    /** @brief 纯查询，不改变淘汰顺序/频率，不计命中统计。 */
    virtual bool Peek(const K& key, V* value) const = 0;

    /**
     * @brief 插入或覆盖条目。
     * @param weight 该条目的占用单元（字节数）；只有 GDSF 用它做决策。
     */
    virtual void Put(const K& key, V value, std::size_t weight = 1) = 0;

    /** @brief 删除条目；返回是否真的删除了一个缓存条目。 */
    virtual bool Erase(const K& key) = 0;

    /** @brief 清空所有条目并重置权重；累计统计保持不变。 */
    virtual void Clear() = 0;

    virtual std::size_t Size() const = 0;
    virtual std::size_t Capacity() const = 0;
    virtual std::size_t Weight() const = 0;

    /** @brief 累计统计 + 当前水位快照。 */
    virtual CacheStats Stats() const = 0;

    /** @brief 策略短名（"LRU"、"ARC" 等），用于报表。 */
    virtual const char* Name() const = 0;

    /** @brief 可读的诊断字符串（各策略追加内部参数，例如 ARC 的 p）。 */
    virtual std::string Describe() const { return Name(); }
};

/**
 * @brief 线程安全装饰器：在任意 `Cache<K, V>` 外包一层互斥锁。
 *
 * @tparam Lock 满足 BasicLockable 的锁类型，默认 `std::mutex`。
 *
 * @details 用装饰器而不是给每个策略加锁，有三个好处：
 *   1. 单线程基准测到的是策略本身的开销，不含锁；
 *   2. 锁策略可替换（普通线程 / Fiber-aware / 分片）；
 *   3. 策略实现只需要关心算法，不需要在每个 Get 里想锁顺序。
 *
 * Fiber-aware 用法（需要包含 go2cpp/sync.hpp）：
 * @code
 *   using FiberSafeLru = go2cpp::cache::ThreadSafeCache<
 *       std::string, std::string, go2cpp::sync::Mutex>;
 *   FiberSafeLru cache(std::make_unique<go2cpp::cache::LruCache<...>>(opts));
 * @endcode
 * `go2cpp::sync::Mutex` 同时提供 `lock()/unlock()`，可以直接作为
 * `std::lock_guard` 的模板参数；在受管 G 上竞争会挂起而不是阻塞线程。
 */
template <class K, class V, class Lock = std::mutex>
class ThreadSafeCache final : public Cache<K, V> {
public:
    /** @brief 接管一个策略实例；inner 为空时抛 std::invalid_argument。 */
    explicit ThreadSafeCache(std::unique_ptr<Cache<K, V>> inner)
        : inner_(std::move(inner)) {
        if (!inner_) {
            throw std::invalid_argument("ThreadSafeCache: inner cache is null");
        }
    }

    ThreadSafeCache(const ThreadSafeCache&) = delete;
    ThreadSafeCache& operator=(const ThreadSafeCache&) = delete;
    ThreadSafeCache(ThreadSafeCache&&) = delete;
    ThreadSafeCache& operator=(ThreadSafeCache&&) = delete;

    bool Get(const K& key, V* value) override {
        std::lock_guard<Lock> guard(mutex_);
        return inner_->Get(key, value);
    }

    bool Peek(const K& key, V* value) const override {
        std::lock_guard<Lock> guard(mutex_);
        return inner_->Peek(key, value);
    }

    void Put(const K& key, V value, std::size_t weight = 1) override {
        std::lock_guard<Lock> guard(mutex_);
        inner_->Put(key, std::move(value), weight);
    }

    bool Erase(const K& key) override {
        std::lock_guard<Lock> guard(mutex_);
        return inner_->Erase(key);
    }

    void Clear() override {
        std::lock_guard<Lock> guard(mutex_);
        inner_->Clear();
    }

    std::size_t Size() const override {
        std::lock_guard<Lock> guard(mutex_);
        return inner_->Size();
    }

    std::size_t Capacity() const override { return inner_->Capacity(); }

    std::size_t Weight() const override {
        std::lock_guard<Lock> guard(mutex_);
        return inner_->Weight();
    }

    CacheStats Stats() const override {
        std::lock_guard<Lock> guard(mutex_);
        return inner_->Stats();
    }

    const char* Name() const override { return inner_->Name(); }

    std::string Describe() const override {
        std::lock_guard<Lock> guard(mutex_);
        return inner_->Describe();
    }

private:
    mutable Lock mutex_;
    std::unique_ptr<Cache<K, V>> inner_;
};

}  // namespace cache
}  // namespace go2cpp
