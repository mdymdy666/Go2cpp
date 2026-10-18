/**
 * @file heap.hpp
 * @brief Go `container/heap` 的 C++ 对应实现：接口式 + 模板式两种形态。
 *
 * @details 复用优先的结论（先检索再动手）：
 *   | 候选 | 许可证 | 结论 |
 *   |---|---|---|
 *   | Go 1.23 `container/heap`（本机 `third_party/go1.23.0.src.tar.gz`） | BSD-3-Clause | **作为语义基准**：本文件是独立实现，逐行对照其 `Init/Push/Pop/Remove/Fix` 的 sift 流程，不复制源码 |
 *   | `std::priority_queue` / `std::push_heap` / `std::pop_heap` | C++ 标准库 | **采用为底层存储与对照基线**：模板式 `Heap` 用 `std::vector` 存元素；压测里直接与 `std::priority_queue` 对比。未直接调用 `std::push_heap`，因为 Go 的 `Fix`/`Remove` 需要「sift 是否真的移动过」这一返回值，而标准算法不暴露该信息 |
 *   | Boost.Heap（`boost::heap::fibonacci_heap` 等） | Boost 1.0 | **不采用**：引入 Boost 头依赖只为一套本可 200 行写清的二叉堆，且其 API 不是 Go 形态；项目当前只在非 native 上下文平台链接 Boost.Context |
 *   | [skarupke/flat_set](https://github.com/skarupke/flat_set) 等第三方扁平容器 | MIT | **不采用**：不是 heap，语义无法对齐 `container/heap` |
 *
 *   **两种形态为什么都要**：
 *   - **接口式**（自由函数 `Init`/`Push`/`Pop`/`Fix`/`Remove`）逐项对齐 Go：
 *     Go 用 `heap.Interface`（`Len`/`Less`/`Swap`/`Push`/`Pop` 五方法）把「堆算法」与
 *     「用户容器」解耦；C++ 侧用**鸭子类型**（模板 + 不写虚接口）表达同一契约，
 *     零虚调用开销。用户容器只需同名同签名地实现五个方法。
 *   - **模板式**（`Heap<T, Less>`）符合 C++ 习惯：内部 `std::vector`，直接
 *     `Push`/`Pop`/`Top`/`Size`/`Empty`，无需用户再写五个方法。
 *
 *   **关键语义**：
 *   - 默认 `std::less<>` 是 **min-heap**，`Pop()` 返回**最小元素**（与 Go 一致）；
 *     传 `std::greater<>` 即 max-heap。
 *   - 接口式的 `Pop(h)` 与 Go 完全相同：先把首元素与末元素交换，再对前 `n-1` 个元素
 *     sift-down，最后调用用户的 `h.Pop()` 取走末元素。因此用户的 `Pop()` 方法
 *     **只能删除并返回最后一个元素**，不能自己再做 sift（Go 文档同样如此）。
 *   - `Fix(h, i)`：Go 先尝试 down，未移动才 up；`Remove(h, i)` 同理。这个「先 down
 *     后 up」的顺序不是随意选的——元素变小时 down 不会移动，必须再 up；元素变大时
 *     down 会移动，此时 up 一定无事可做。本实现保留该顺序。
 *
 * @note **边界/差异**：
 *       - Go 在空堆上 `Pop`/越界 `Remove` 会 panic；C++ 侧抛 `std::out_of_range`
 *         （模板式 `Pop`/`Top` 亦同）。
 *       - Go 的 `any` 允许同一堆混装任意类型；C++ 侧要求元素类型在编译期确定。
 *       - 模板式 `Heap` **不**参与接口式自由函数（它的成员 `Push`/`Pop` 已自带 sift，
 *         再套自由函数虽然幂等但语义混乱）；两套 API 请勿交叉使用。
 *       - 未提供 `std::priority_queue` 那样的底层容器模板参数；内部固定 `std::vector`。
 */
#pragma once

#include <cstddef>
#include <functional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace go2cpp::container {

// ============================================================================
// 接口式：对应 Go 的 heap.Interface
//
// 用户容器需实现（鸭子类型，不要求继承任何基类）：
//   std::size_t Len() const;
//   bool Less(std::size_t i, std::size_t j) const;
//   void Swap(std::size_t i, std::size_t j);
//   void Push(T value);   // 只追加到末尾，不做 sift
//   T    Pop();           // 只删除并返回末尾元素，不做 sift
// ============================================================================

namespace detail {

/**
 * @brief 从 i0 开始向下 sift。
 * @return 是否真的移动过元素（`Fix`/`Remove` 依赖这个返回值决定要不要再 up）。
 * @details 与 Go `container/heap.down` 一致：先选较小的孩子，若孩子不小于自己就停。
 *          返回 `i > i0` 而不是额外的 bool，语义与 Go 完全相同。
 */
template <typename H>
inline bool HeapDown(H& h, std::size_t i0, std::size_t n) {
    std::size_t i = i0;
    while (true) {
        const std::size_t left = 2 * i + 1;
        if (left >= n) {
            break;
        }
        std::size_t child = left;
        const std::size_t right = left + 1;
        if (right < n && h.Less(right, left)) {
            child = right;
        }
        if (!h.Less(child, i)) {
            break;
        }
        h.Swap(i, child);
        i = child;
    }
    return i > i0;
}

/**
 * @brief 从 j 开始向上 sift。
 * @details 与 Go `container/heap.up` 一致：只要比父节点小就交换，直到根。
 */
template <typename H>
inline void HeapUp(H& h, std::size_t j) {
    while (j > 0) {
        const std::size_t parent = (j - 1) / 2;
        if (!h.Less(j, parent)) {
            break;
        }
        h.Swap(parent, j);
        j = parent;
    }
}

}  // namespace detail

/**
 * @brief 对应 `heap.Init`：把任意数组原地整理成堆，时间复杂度 O(n)。
 * @details 从最后一个非叶节点向前逐个 sift-down。空容器是合法的空操作。
 */
template <typename H>
inline void Init(H& h) {
    const std::size_t n = static_cast<std::size_t>(h.Len());
    for (std::size_t i = n / 2; i-- > 0;) {
        detail::HeapDown(h, i, n);
    }
}

/**
 * @brief 对应 `heap.Push`：先调用用户 `Push` 追加到末尾，再向上 sift。
 * @param value 待插入元素；可复制或移动。
 */
template <typename H, typename T>
inline void Push(H& h, T&& value) {
    h.Push(std::forward<T>(value));
    detail::HeapUp(h, static_cast<std::size_t>(h.Len()) - 1);
}

/**
 * @brief 对应 `heap.Pop`：删除并返回**堆顶（默认比较下的最小元素）**。
 * @throws std::out_of_range 当 `h.Len() == 0`（Go 在此 panic）。
 * @details 交换首尾 → 对前 n-1 个元素 sift-down → 调用用户 `Pop()` 取末元素。
 */
template <typename H>
inline auto Pop(H& h) -> decltype(h.Pop()) {
    if (static_cast<std::size_t>(h.Len()) == 0) {
        throw std::out_of_range("go2cpp::container::Pop on empty heap");
    }
    const std::size_t n = static_cast<std::size_t>(h.Len()) - 1;
    h.Swap(0, n);
    detail::HeapDown(h, 0, n);
    return h.Pop();
}

/**
 * @brief 对应 `heap.Remove`：删除下标 i 的元素并返回它。
 * @throws std::out_of_range 当 i 越界或堆为空。
 * @details 把 i 与末尾交换后，对 i 先 down 后 up（与 Go 一致）。
 */
template <typename H>
inline auto Remove(H& h, std::size_t i) -> decltype(h.Pop()) {
    const std::size_t len = static_cast<std::size_t>(h.Len());
    if (len == 0 || i >= len) {
        throw std::out_of_range("go2cpp::container::Remove index out of range");
    }
    const std::size_t n = len - 1;
    if (n != i) {
        h.Swap(i, n);
        if (!detail::HeapDown(h, i, n)) {
            detail::HeapUp(h, i);
        }
    }
    return h.Pop();
}

/**
 * @brief 对应 `heap.Fix`：下标 i 的元素优先级变化后，就地恢复堆性质。
 * @throws std::out_of_range 当 i 越界。
 * @details 这正是标准库 `std::priority_queue` **没有**的能力：任务 deadline 被修改后
 *          无需重建整个堆。先 down（变大时触发），未移动再 up（变小时触发）。
 */
template <typename H>
inline void Fix(H& h, std::size_t i) {
    const std::size_t n = static_cast<std::size_t>(h.Len());
    if (i >= n) {
        throw std::out_of_range("go2cpp::container::Fix index out of range");
    }
    if (!detail::HeapDown(h, i, n)) {
        detail::HeapUp(h, i);
    }
}

// ============================================================================
// 模板式：C++ 习惯的 `Heap<T, Less>`
// ============================================================================

/**
 * @brief 基于 `std::vector` 的二叉堆。
 * @tparam T 元素类型；要求可移动构造/可移动赋值（`Pop` 用移动赋值搬移末元素）。
 * @tparam Less 严格弱序比较器；默认 `std::less<>` 得到 **min-heap**。
 *
 * @details 默认形态与 Go 对齐：`Pop()` 返回最小元素。做 max-heap 传
 *          `std::greater<>`；自定义结构体则传自己的比较器。
 *
 * @code
 * container::Heap<int> min_heap;
 * min_heap.Push(3);
 * min_heap.Push(1);
 * int x = min_heap.Pop();   // x == 1
 *
 * container::Heap<int, std::greater<>> max_heap;
 * @endcode
 */
template <typename T, typename Less = std::less<>>
class Heap {
 public:
    using value_type = T;
    using size_type = std::size_t;
    using comparator_type = Less;

    Heap() = default;
    explicit Heap(Less less) : m_less(std::move(less)) {}

    /** @brief 插入一个元素（左值拷贝 / 右值移动），O(log n)。 */
    void Push(const T& value) {
        m_data.push_back(value);
        SiftUp(m_data.size() - 1);
    }

    /** @brief 插入一个元素（移动），O(log n)。 */
    void Push(T&& value) {
        m_data.push_back(std::move(value));
        SiftUp(m_data.size() - 1);
    }

    /**
     * @brief 删除并返回堆顶（默认比较下的最小值），O(log n)。
     * @throws std::out_of_range 堆为空时。
     * @details 用「空洞下沉」而不是逐层 swap：把末元素取出后，沿较小孩子路径
     *          把空洞一路下移（每层只移动一次），最后再尝试把末元素上移补洞。
     *          接口式 `Pop(h)` 只能用 `Swap`，做不到这一点；模板式可以，故两者
     *          内部实现不同但结果语义一致。
     */
    T Pop() {
        if (m_data.empty()) {
            throw std::out_of_range("go2cpp::container::Heap::Pop on empty heap");
        }
        T result = std::move(m_data.front());
        if (m_data.size() == 1) {
            m_data.pop_back();
            return result;
        }

        T last = std::move(m_data.back());
        m_data.pop_back();
        const size_type n = m_data.size();
        size_type hole = 0;
        while (true) {
            size_type child = 2 * hole + 1;
            if (child >= n) {
                break;
            }
            if (child + 1 < n && m_less(m_data[child + 1], m_data[child])) {
                ++child;
            }
            m_data[hole] = std::move(m_data[child]);
            hole = child;
        }
        // 末元素可能比补洞位置的父节点还小，需要再上移（与 std::pop_heap 同理）。
        while (hole > 0) {
            const size_type parent = (hole - 1) / 2;
            if (!m_less(last, m_data[parent])) {
                break;
            }
            m_data[hole] = std::move(m_data[parent]);
            hole = parent;
        }
        m_data[hole] = std::move(last);
        return result;
    }

    /** @brief 读取堆顶；空堆抛 `std::out_of_range`。 */
    const T& Top() const {
        if (m_data.empty()) {
            throw std::out_of_range("go2cpp::container::Heap::Top on empty heap");
        }
        return m_data.front();
    }

    /** @brief 可写堆顶；空堆抛 `std::out_of_range`。改完值后需自行保证堆性质。 */
    T& Top() {
        if (m_data.empty()) {
            throw std::out_of_range("go2cpp::container::Heap::Top on empty heap");
        }
        return m_data.front();
    }

    /** @brief 元素个数。 */
    size_type Size() const noexcept { return m_data.size(); }

    /** @brief 是否为空。 */
    bool Empty() const noexcept { return m_data.empty(); }

    /** @brief 清空所有元素，保留已分配的容量。 */
    void Clear() { m_data.clear(); }

    /** @brief 预分配至少 `capacity` 个元素的空间（压测时消除扩容噪声）。 */
    void Reserve(size_type capacity) { m_data.reserve(capacity); }

 private:
    void SiftUp(size_type j) {
        while (j > 0) {
            const size_type parent = (j - 1) / 2;
            if (!m_less(m_data[j], m_data[parent])) {
                break;
            }
            std::swap(m_data[parent], m_data[j]);
            j = parent;
        }
    }

    std::vector<T> m_data;
    Less m_less{};
};

}  // namespace go2cpp::container
