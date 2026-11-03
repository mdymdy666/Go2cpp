/**
 * @file lru.hpp
 * @brief 基线 LRU（Least Recently Used）缓存。
 *
 * @details 数据结构：
 *   - **侵入式双向链表**按最近访问排序，head 是 MRU、tail 是 LRU；
 *     条目对象自己就是链表节点（`Entry : detail::Link<Entry>`），
 *     不再有独立的 `std::list` 节点，命中时 `move_to_front` 只写 4 个指针。
 *   - **开放寻址哈希表** `detail::FlatMap<K, Entry*>` 做 O(1) 定位：
 *     连续槽数组 + 线性探测，查找不再有桶链指针追逐。
 *   - **节点池** `detail::NodePool<Entry>`：条目按 64 个一块分配，淘汰下来的
 *     节点进自由表复用。稳态（容量已满）下 Put 的堆分配次数为 **0**，
 *     而 `std::list + unordered_map` 版本是 **2 次/插入**。
 *
 *         复杂度：Get / Put / Erase 均摊 O(1)，空间 O(capacity)。
 *         缺点：对顺序扫描零抵抗——一次全表扫描会把所有热点挤出缓存，
 *         即「cache pollution / scan resistance」问题（见 docs 专节）。
 *
 *         语义与优化前逐位一致（同一淘汰顺序、同一统计口径），只是把容器
 *         层换成零分配版本；`tests/cache` 的命中率对拍可以直接验证这一点。
 */
#pragma once

#include <cstddef>
#include <stdexcept>
#include <string>
#include <utility>

#include "go2cpp/cache/cache.hpp"
#include "go2cpp/cache/detail/bucket_map.hpp"
#include "go2cpp/cache/detail/intrusive.hpp"

namespace go2cpp {
namespace cache {

/**
 * @brief 经典 LRU 缓存（哈希表 + 双向链表）。
 *
 * @tparam K key 类型。
 * @tparam V value 类型。
 * @tparam Hash key 哈希函数。
 * @tparam Equal key 相等比较。
 */
template <class K, class V, class Hash = std::hash<K>,
          class Equal = std::equal_to<K>>
class LruCache final : public Cache<K, V> {
public:
    /** @brief 构造；capacity 必须 >= 1。 */
    explicit LruCache(CacheOptions options = CacheOptions{})
        : capacity_(options.capacity), index_(options.capacity) {
        if (capacity_ == 0) {
            throw std::invalid_argument("LruCache: capacity must be >= 1");
        }
    }

    ~LruCache() override {
        // 节点池只回收整块内存，不调用仍存活条目的析构函数；非平凡 value
        // （如 std::string）必须在这里显式销毁，否则析构时泄漏。
        while (Entry* entry = lru_.pop_front()) {
            pool_.Destroy(entry);
        }
    }

    /**
     * @brief 查 key；命中时把条目提到 LRU 头部并写回值。
     *
     * @param key [in] 查找键，const 引用——生命周期只需覆盖本次调用。
     * @param value [out] 命中时写入；可为 nullptr 表示只探热——未命中时不修改。
     *
     * @return
     * - true：命中且已刷新热度——调用方**必须**把写回值当最新数据；
     * - false：未命中并计入 miss——调用方**必须**回源加载，不要当缓存为空。
     *
     * ## 定位
     * 唯一读取路径。命中却不移动链表会让 LRU 退化为 FIFO，热数据被顺序扫描挤掉。
     *
     * ## 步骤
     * step1. 在哈希索引里查找条目；
     * step2. 未命中记 miss 并返回；
     * step3. 命中先把节点移到链表头部，刷新「最近使用」次序；
     * step4. 调用方给了输出指针才拷贝值；
     * step5. 记 hit。
     *
     * ## 并发/失败语义
     * - 调用约束：非线程安全，调用方必须外部加锁（如 factory 的分片锁）。
     * - 原子性：单线程内「查找 + 移头 + 写回」不可分割；跨线程需外部互斥。
     * - 失败语义：noexcept 语义上不抛；value 为 nullptr 时只更新次序。
     * - 不变量：返回 true 后该条目一定在链表头部。
     */
    bool Get(const K& key, V* value) override {
        Entry* entry = Lookup(key);
        if (entry == nullptr) {
            stats_.RecordMiss();
            return false;
        }
        lru_.move_to_front(entry);
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

    /**
     * @brief 插入或更新 key；已存在则原地更新并提升到头部。
     *
     * @param key [in] 键，const 引用——生命周期只需覆盖本次调用。
     * @param value [in] 值，按值移交（内部 move 进节点）——调用后源对象不再被读取。
     * @param weight [in] 权重，默认 1；0 表示不占权重但仍占一个名额，负数无符号语义不适用。
     *
     * @return 无返回值；插入失败（内存不足）由节点池抛异常。
     *
     * ## 定位
     * 唯一写入路径。把「更新」误当「插入」会增加 Size 并触发多余淘汰；容量判定按条数而非权重，错改会超容。
     *
     * ## 步骤
     * step1. 先查已有条目（更新热路径）；
     * step2. 更新分支：调整权重、替换值、移到头部、记 update 后返回；
     * step3. 插入分支：先淘汰到容量以下，保证不会超容；
     * step4. 新建节点、压入链表头、写索引、累加权重、记 insert。
     *
     * ## 并发/失败语义
     * - 调用约束：非线程安全，需外部加锁。
     * - 原子性：更新分支不改变 Size；插入分支先腾位再入表。
     * - 失败语义：节点池分配失败时抛异常，缓存保持旧状态。
     * - 不变量：返回后 Size() <= Capacity()，且该 key 在链表头部。
     */
    void Put(const K& key, V value, std::size_t weight = 1) override {
        const std::uint32_t hash = index_.HashKey(key);
        Entry* entry = index_.FindHashed(hash, key);
        if (entry != nullptr) {
            weight_ = weight_ - entry->weight + weight;
            entry->value = std::move(value);
            entry->weight = weight;
            lru_.move_to_front(entry);
            stats_.RecordUpdate();
            return;
        }
        while (lru_.size() >= capacity_) {
            EvictTail();
        }
        Entry* fresh = pool_.Create(key, std::move(value), weight);
        lru_.push_front(fresh);
        index_.InsertHashed(fresh, hash);
        weight_ += weight;
        stats_.RecordInsert();
    }

    bool Erase(const K& key) override {
        Entry* entry = Lookup(key);
        if (entry == nullptr) {
            return false;
        }
        weight_ -= entry->weight;
        index_.Erase(entry);
        lru_.erase(entry);
        pool_.Destroy(entry);
        stats_.RecordRemoval();
        return true;
    }

    void Clear() override {
        while (Entry* entry = lru_.pop_front()) {
            pool_.Destroy(entry);
        }
        index_.Clear();
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

    const char* Name() const override { return "LRU"; }

    std::string Describe() const override {
        return "LRU{capacity=" + std::to_string(capacity_) +
               ", size=" + std::to_string(index_.size()) +
               ", weight=" + std::to_string(weight_) + "}";
    }

private:
    /** @brief 缓存条目：同时是链表节点（prev/next 由 Link 提供）。 */
    struct Entry : detail::Link<Entry>, detail::HashNode<Entry> {
        Entry(const K& entry_key, V entry_value, std::size_t entry_weight)
            : key(entry_key),
              value(std::move(entry_value)),
              weight(entry_weight) {}

        K key;
        V value;
        std::size_t weight;
    };

    Entry* Lookup(const K& key) const { return index_.Find(key); }

    /**
     * @brief 淘汰链表尾部（最久未使用）条目并释放节点。
     *
     * @return 无返回值；空表时安静返回。
     *
     * ## 定位
     * 唯一淘汰点。漏摘索引会留下悬空指针；漏减 weight_ 会让统计永久偏高。
     *
     * ## 步骤
     * step1. 取尾节点，空表直接返回；
     * step2. 从总权重里扣掉该条目；
     * step3. 先摘索引再摘链表，避免比较器访问已销毁节点；
     * step4. 归还节点池；
     * step5. 记 eviction。
     *
     * ## 并发/失败语义
     * - 调用约束：与 Put/Get 同一线程或同一外部锁内。
     * - 原子性：单线程内摘除是原子的；异常安全由节点池保证（不抛）。
     * - 不变量：返回后该条目不再可被 Get 查到，weight_ 与存活节点一致。
     */
    void EvictTail() {
        Entry* victim = lru_.back();
        if (victim == nullptr) {
            return;
        }
        weight_ -= victim->weight;
        index_.Erase(victim);
        lru_.erase(victim);
        pool_.Destroy(victim);
        stats_.RecordEviction();
    }

    std::size_t capacity_;
    detail::IntrusiveList<Entry> lru_;
    detail::BucketMap<K, Entry, Hash, Equal> index_;
    detail::NodePool<Entry> pool_;
    std::size_t weight_ = 0;
    StatsRecorder stats_;
};

}  // namespace cache
}  // namespace go2cpp
