/**
 * @file detail/intrusive.hpp
 * @brief 缓存策略共用的**侵入式双向链表**与**定长节点池**。
 *
 * @details 7 个淘汰策略原先都用 `std::list<Entry>` + `std::unordered_map`
 *          的双容器组合。汇编级审查（objdump）显示这条路径上有两类固定
 *          开销：
 *
 *           1. **两次堆分配 / 释放**：`list::push_front` 分配一个节点，
 *              `unordered_map::emplace` 再分配一个桶链节点。`operator new`
 *              每次至少 40~100 ns 量级，而一次 Get 的算法本身只有十几个
 *              周期——分配器成了主成本。
 *           2. **三次指针追逐**：桶数组 -> 桶链节点 -> list 节点 -> 条目
 *              载荷，每一跳都可能是一次 cache miss；节点的 16 字节
 *              `prev/next` 与条目数据分离，进一步降低局部性。
 *
 *          本文件把这两层都换掉：
 *
 *          - `Link<Derived>`：条目类型**公开继承**它，于是 `prev/next`
 *            成为条目对象的一部分——不再有独立的链表节点，也就没有第二次
 *            分配。基类指针与派生对象地址相同，`static_cast` 是零成本的。
 *          - `IntrusiveList<T>`：只维护 head/tail/count 三个字，`erase` 与
 *            `move_to_front` 只写 4~6 个指针，不做任何内存管理。
 *          - `NodePool<T>`：按固定块（默认 64 个节点一块）向系统申请内存，
 *            释放的节点挂在自由表里复用，因此稳态下（容量已满、每插入一条
 *            就淘汰一条）**再没有任何 malloc/free**。
 *
 *          组合后的单条插入路径：池命中（0 次分配）+ 链表插头（2 次写）+
 *          哈希表写槽，替代原先的 2 次分配 + 3 次指针追逐。
 */
#pragma once

#include <cstddef>
#include <new>
#include <utility>
#include <vector>

namespace go2cpp {
namespace cache {
namespace detail {

/**
 * @brief 侵入式链表挂载点。
 *
 * @details 条目类型以 `struct Entry : Link<Entry>` 的方式公开继承，链表就能
 *          直接在条目对象内部串指针。派生类必须是公开、非虚、单继承，这样
 *          `Link<T>&` 与 `T&` 的地址一致，转换不产生任何指令。
 *
 * @tparam Derived 条目类型（CRTP）。
 */
template <class Derived>
struct Link {
    Derived* prev = nullptr;  ///< 前驱；链表头为 nullptr。
    Derived* next = nullptr;  ///< 后继；链表尾为 nullptr。
};

/**
 * @brief 侵入式双向链表（**不拥有**节点，节点生命周期由调用方/池管理）。
 *
 * @details 所有操作 O(1) 且不分配内存。链表本身不可拷贝：拷贝会让两个
 *          链表共享同一批节点指针。需要把节点搬家时用
 *          `move_to_front(other, node)` / `move_to_back(other, node)`，
 *          它们只改指针，不移动条目载荷——这是 std::list::splice 的
 *          零开销等价物。
 *
 * @tparam T 条目类型，必须公开继承 `Link<T>`。
 */
template <class T>
class IntrusiveList {
public:
    IntrusiveList() = default;
    IntrusiveList(const IntrusiveList&) = delete;
    IntrusiveList& operator=(const IntrusiveList&) = delete;

    /** @brief 链表是否为空。 */
    bool empty() const noexcept { return head_ == nullptr; }

    /** @brief 当前节点数（O(1)，独立计数，不遍历）。 */
    std::size_t size() const noexcept { return size_; }

    T* front() noexcept { return head_; }
    const T* front() const noexcept { return head_; }
    T* back() noexcept { return tail_; }
    const T* back() const noexcept { return tail_; }

    /** @brief 在头部挂入一个**不属于任何链表**的节点。 */
    void push_front(T* node) noexcept {
        Link<T>& link = LinkOf(node);
        link.prev = nullptr;
        link.next = head_;
        if (head_ != nullptr) {
            LinkOf(head_).prev = node;
        } else {
            tail_ = node;
        }
        head_ = node;
        ++size_;
    }

    /** @brief 在尾部挂入一个**不属于任何链表**的节点。 */
    void push_back(T* node) noexcept {
        Link<T>& link = LinkOf(node);
        link.next = nullptr;
        link.prev = tail_;
        if (tail_ != nullptr) {
            LinkOf(tail_).next = node;
        } else {
            head_ = node;
        }
        tail_ = node;
        ++size_;
    }

    /** @brief 摘下头节点并返回（返回 nullptr 表示空表）；节点不析构。 */
    T* pop_front() noexcept {
        T* node = head_;
        if (node != nullptr) {
            Unlink(node);
        }
        return node;
    }

    /** @brief 摘下尾节点并返回；节点不析构。 */
    T* pop_back() noexcept {
        T* node = tail_;
        if (node != nullptr) {
            Unlink(node);
        }
        return node;
    }

    /** @brief 摘下任意节点（不析构）；已是游离节点时为空操作。 */
    void erase(T* node) noexcept {
        if (node != nullptr) {
            Unlink(node);
        }
    }

    /** @brief 同一链表内把 node 移到头部（命中时的 LRU 提升）。 */
    void move_to_front(T* node) noexcept {
        if (head_ == node || node == nullptr) {
            return;
        }
        Unlink(node);
        push_front(node);
    }

    /** @brief 同一链表内把 node 移到尾部。 */
    void move_to_back(T* node) noexcept {
        if (tail_ == node || node == nullptr) {
            return;
        }
        Unlink(node);
        push_back(node);
    }

    /** @brief 把 src 中的 node 摘到本链表头部（跨链表搬家，载荷不动）。 */
    void move_to_front(IntrusiveList& src, T* node) noexcept {
        src.Unlink(node);
        push_front(node);
    }

    /** @brief 把 src 中的 node 摘到本链表尾部。 */
    void move_to_back(IntrusiveList& src, T* node) noexcept {
        src.Unlink(node);
        push_back(node);
    }

    /** @brief 把 src 的所有节点拼到本链表尾部（src 变空）。 */
    void splice_back(IntrusiveList& src) noexcept {
        if (src.head_ == nullptr) {
            return;
        }
        if (tail_ != nullptr) {
            LinkOf(tail_).next = src.head_;
            LinkOf(src.head_).prev = tail_;
        } else {
            head_ = src.head_;
        }
        tail_ = src.tail_;
        size_ += src.size_;
        src.clear_links();
    }

    /**
     * @brief 只把链表指针结构清零，**不**碰节点内存。
     *
     * @details 用于 `Clear()`：调用方先 `pop_front()` 逐个 Destroy 到池，
     *          或确认节点由外部统一释放。直接丢弃节点会造成泄漏/悬垂。
     */
    void clear_links() noexcept {
        head_ = nullptr;
        tail_ = nullptr;
        size_ = 0;
    }

    /** @brief 遍历：返回 node 的后继（用于手写循环）。 */
    static T* next(T* node) noexcept { return LinkOf(node).next; }
    static T* prev(T* node) noexcept { return LinkOf(node).prev; }

private:
    static Link<T>& LinkOf(T* node) noexcept {
        return static_cast<Link<T>&>(*node);
    }

    void Unlink(T* node) noexcept {
        Link<T>& link = LinkOf(node);
        if (link.prev != nullptr) {
            LinkOf(link.prev).next = link.next;
        } else {
            head_ = link.next;
        }
        if (link.next != nullptr) {
            LinkOf(link.next).prev = link.prev;
        } else {
            tail_ = link.prev;
        }
        link.prev = nullptr;
        link.next = nullptr;
        --size_;
    }

    T* head_ = nullptr;
    T* tail_ = nullptr;
    std::size_t size_ = 0;
};

/**
 * @brief 定长块节点池：把「每条目一次 malloc」变成「每 64 条一次 malloc」。
 *
 * @details 稳态（容量已满，插入前先淘汰）下 `Destroy` 放回的自由节点会被
 *          下一次 `Create` 立刻复用，分配次数为 0。池不缩容、不整理，节点
 *          地址稳定——这一点对「哈希表存节点指针」至关重要。
 *
 *          `Release()` 只释放整块内存，不会调用仍存活节点的析构函数；
 *          正确用法是上层容器先逐个 `Destroy`。
 *
 * @tparam T 节点类型（必须可析构）。
 * @tparam kBlockSize 每块节点数；块越大分配次数越少、尾部浪费越多。
 */
template <class T, std::size_t kBlockSize = 64>
class NodePool {
public:
    NodePool() = default;
    ~NodePool() { Release(); }

    NodePool(const NodePool&) = delete;
    NodePool& operator=(const NodePool&) = delete;

    /** @brief 预分配可容纳 `expected` 个节点的块（只减少后续 malloc 次数）。 */
    explicit NodePool(std::size_t expected) { Reserve(expected); }

    /** @brief 预分配可容纳 `expected` 个节点的块。 */
    void Reserve(std::size_t expected) {
        const std::size_t blocks = (expected + kBlockSize - 1) / kBlockSize;
        while (blocks_.size() < blocks) {
            AppendBlock();
        }
    }

    /** @brief 构造一个节点（优先复用自由表）。 */
    template <class... Args>
    T* Create(Args&&... args) {
        T* slot;
        if (!free_.empty()) {
            slot = free_.back();
            free_.pop_back();
        } else {
            slot = AcquireFresh();
        }
        ++live_;
        return ::new (static_cast<void*>(slot)) T(std::forward<Args>(args)...);
    }

    /** @brief 析构节点并放回自由表。 */
    void Destroy(T* node) noexcept {
        node->~T();
        free_.push_back(node);
        --live_;
    }

    /** @brief 释放所有块（调用前必须已 Destroy 全部存活节点）。 */
    void Release() noexcept {
        for (void* block : blocks_) {
            ::operator delete(block);
        }
        blocks_.clear();
        free_.clear();
        next_slot_ = nullptr;
        remaining_ = 0;
        live_ = 0;
    }

    /** @brief 当前存活节点数。 */
    std::size_t live() const noexcept { return live_; }

    /** @brief 已经向系统申请的总槽位数（含空闲）。 */
    std::size_t slot_count() const noexcept {
        return blocks_.size() * kBlockSize;
    }

private:
    void AppendBlock() {
        void* block = ::operator new(sizeof(T) * kBlockSize);
        blocks_.push_back(block);
        next_slot_ = static_cast<T*>(block);
        remaining_ = kBlockSize;
    }

    T* AcquireFresh() {
        if (remaining_ == 0) {
            AppendBlock();
        }
        T* slot = next_slot_;
        ++next_slot_;
        --remaining_;
        return slot;
    }

    std::vector<void*> blocks_;
    std::vector<T*> free_;
    T* next_slot_ = nullptr;
    std::size_t remaining_ = 0;
    std::size_t live_ = 0;
};

}  // namespace detail
}  // namespace cache
}  // namespace go2cpp
