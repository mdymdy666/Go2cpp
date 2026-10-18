/**
 * @file heap.hpp
 * @brief Go `container/heap` 的 C++ 对应：接口式（自由函数 + 鸭子类型）+ 模板式 `Heap<T, Less>`。
 *
 * @details 复用结论（先检索再动手）：
 *   | 候选 | 许可证 | 结论 |
 *   |---|---|---|
 *   | Go 1.23 `container/heap`（本机 `third_party/go1.23.0.src.tar.gz`） | BSD-3-Clause | **语义基准**：`Init/Push/Pop/Remove/Fix` 的行为、`Fix` 先 down 后 up 的顺序逐项对齐；独立实现，不复制源码 |
 *   | `<algorithm>` 的 `std::make_heap` / `std::push_heap` / `std::pop_heap` | C++ 标准库 | **采用为模板式 `Heap` 的建堆 / 插入 / 弹出实现**，底层 `std::vector` |
 *   | `std::priority_queue` | C++ 标准库 | **不采用**：底层容器 `c` 是 protected，拿不到迭代器就做不了 `Fix`/`Remove`；`pop_heap` + `vector::pop_back` 是等价的公开算法组合 |
 *   | Boost.Heap（`boost::heap::fibonacci_heap` 等） | Boost 1.0 | **不采用**：为二叉堆引入 Boost 头依赖，且其 API 不是 Go 形态 |
 *
 *   **模板式用公开算法而不是自实现**：二叉堆的 `push_heap`/`pop_heap`/`make_heap`
 *   已是标准库实现，`Fix`/`Remove` 只需要在 `std::vector` 上做 sift（约 30 行），
 *   没有必要再维护一整份堆逻辑。
 *
 *   **接口式为什么保留手写 sift**：Go 用 `heap.Interface`（`Len`/`Less`/`Swap`/
 *   `Push`/`Pop` 五方法）把堆算法与用户容器解耦；C++ 侧用鸭子类型表达同一契约。
 *   用户容器只暴露下标语义、没有迭代器，`std::*_heap` 无法作用于它，因此
 *   `detail::HeapDown`/`HeapUp` 必须保留（约 30 行）。这是对齐 Go API 形状的
 *   必要适配层，不是重复实现整个堆。
 *
 *   **关键语义**：
 *   - 默认 `std::less<>` 是 **min-heap**，`Pop()` 返回**最小元素**（与 Go 一致）；
 *     传 `std::greater<>` 即 max-heap。
 *   - 接口式 `Pop(h)`/`Remove(h, i)` 与 Go 完全相同：先把首元素与末元素交换，再对
 *     前 `n-1` 个元素 sift-down，最后调用用户的 `h.Pop()` 取走末元素。用户的
 *     `Pop()` 只能删除并返回最后一个元素，不能自己再做 sift（Go 文档同样如此）。
 *   - `Fix`/`Remove`：先尝试 down，未移动才 up（Go 的顺序）。元素变小时 down 不会
 *     移动，必须再 up；元素变大时 down 会移动，此时 up 一定无事可做。
 *
 * @note **边界/差异**：
 *       - Go 在空堆上 `Pop`/越界 `Remove` 会 panic；C++ 侧抛 `std::out_of_range`。
 *       - Go 的 `any` 允许同一堆混装任意类型；C++ 侧元素类型在编译期确定。
 *       - 模板式 `Heap` **不**参与接口式自由函数（它的成员 `Push`/`Pop` 已自带 sift，
 *         再套自由函数虽然幂等但语义混乱）；两套 API 请勿交叉使用。
 *       - 内部固定 `std::vector`，不提供 `std::priority_queue` 那样的底层容器模板参数。
 *
 * ## 模块定位
 *
 * 属于「Go 标准库容器语义」适配层的最底层：上游是 Go `container/heap` 的
 * `Init/Push/Pop/Remove/Fix` 契约，下游（当前仓库内）是 `tests/container/test_heap.cpp`
 * 的语义单元测试与 `tests/container/test_scenarios.cpp` 里的定时任务堆、K 路归并等场景。
 * 本模块**不依赖**运行时、调度器或 IO —— 容器可以脱离 Fiber/EventLoop 单独使用；
 * 上层（例如未来的定时器任务队列）只把它当成「可按下标修堆的 `std::vector`」。
 *
 * ## 本文件定义什么
 *
 * - `detail::HeapDown` / `detail::HeapUp`：接口式的两个 sift 原语，只依赖用户容器的
 *   `Len/Less/Swap` 鸭子类型，不认识迭代器；
 * - `Init` / `Push` / `Pop` / `Remove` / `Fix`：Go `container/heap` 的自由函数形态；
 * - `Heap<T, Less>`：模板式二叉堆，底层 `std::vector` + 标准库 `std::*_heap`；
 * - `Heap::Inverted` / `Heap::SiftDown` / `Heap::SiftUp`（私有）：前者做 min-heap 与
 *   标准库 max-heap 的比较器语义换算，后两者补上标准库缺失的「按下标修堆」能力。
 *
 * ## 阅读建议
 *
 * 先读文件头的复用结论，弄清「接口式为什么必须手写 sift、模板式为什么能复用标准库」；
 * 再看 `detail::HeapDown` 与 `Fix` 的「先 down、未移动再 up」顺序（Go 唯一的非显然约定）；
 * 最后看 `Heap<T, Less>`，注意它通过 `Inverted` 把 min-heap 语义翻转给标准库。
 *
 * ## 边界与陷阱
 *
 * - 两套 API **不可交叉使用**：模板式 `Heap` 的成员 `Push/Pop/Fix/Remove` 已自带 sift，
 *   再套一层自由函数会重复 sift。结果通常仍正确，但多付一次 O(log n)，且读代码的人
 *   会误以为堆可能处于「未修复」状态。
 * - 接口式 `Pop(h)` / `Remove(h, i)` 假定用户的 `Pop()` **只删末尾、不做 sift**；若用户
 *   实现自己又 sift 一次，等于在已修好的堆上再动一次下标，堆性质可能被破坏。
 * - 空堆 / 越界在 Go 是 panic（不可恢复），这里抛 `std::out_of_range`。逐行转译 Go 代码时
 *   必须自己决定是否捕获 —— 把异常当正常分支会掩盖真正的逻辑错误。
 */
#pragma once

#include <algorithm>
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
 * @brief 基于 `std::vector` + `<algorithm>` 堆算法的二叉堆。
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
 *
 * ## 定位
 *
 * 模板式容器层，供**不适合接口式鸭子类型**的调用方使用（元素是普通结构体、比较器是
 * 普通函数对象）。当前仓库内由 `tests/container/test_heap.cpp` 与
 * `tests/container/test_scenarios.cpp` 直接实例化；上层复用时应当把它当作
 * 「可以按下标 `Fix`/`Remove` 的 `std::vector`」，而不是 `std::priority_queue` 的替代品。
 *
 * ## 不变量
 *
 * - `m_data` 始终满足 `m_less` 意义下的**小根堆**性质：任一父节点不大于其孩子；
 *   唯一能破坏它的入口是 `T& Top()` 返回的可写引用（改完必须自行调 `Fix(0)`）。
 * - `Inverted` 只持有 `m_less` 的**指针**，所以比较器必须在本对象生命周期内有效；
 *   默认构造的 `Heap` 用成员 `m_less`，天然满足。
 * - `Fix` / `Remove` 先用 `SiftDown` 的返回值判断是否移动：移动过就不再 up
 *   （元素变大只会下沉），未移动才 up（元素变小只会上升）。
 */
template <typename T, typename Less = std::less<>>
class Heap {
 public:
    using value_type = T;
    using size_type = std::size_t;
    using comparator_type = Less;

    Heap() = default;
    explicit Heap(Less less) : m_less(std::move(less)) {}

    /**
     * @brief 用一批乱序元素重建堆，O(n)（对应 Go `heap.Init` 的批量场景）。
     * @details 直接调用 `std::make_heap`；原有元素被丢弃。
     */
    void Init(std::vector<T> values) {
        m_data = std::move(values);
        std::make_heap(m_data.begin(), m_data.end(), Inverted(&m_less));
    }

    /** @brief 插入一个元素（左值拷贝），O(log n)。 */
    void Push(const T& value) {
        m_data.push_back(value);
        std::push_heap(m_data.begin(), m_data.end(), Inverted(&m_less));
    }

    /** @brief 插入一个元素（移动），O(log n)。 */
    void Push(T&& value) {
        m_data.push_back(std::move(value));
        std::push_heap(m_data.begin(), m_data.end(), Inverted(&m_less));
    }

    /**
     * @brief 删除并返回堆顶（默认比较下的最小值），O(log n)。
     * @throws std::out_of_range 堆为空时。
     * @details `std::pop_heap` 把堆顶移到末尾并恢复前 n-1 个元素的堆性质，
     *          再 `pop_back` 取走它；两步都是标准库公开算法。
     */
    T Pop() {
        if (m_data.empty()) {
            throw std::out_of_range("go2cpp::container::Heap::Pop on empty heap");
        }
        std::pop_heap(m_data.begin(), m_data.end(), Inverted(&m_less));
        T result = std::move(m_data.back());
        m_data.pop_back();
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

    /**
     * @brief 下标 i 的元素优先级变化后，就地恢复堆性质，O(log n)。
     * @throws std::out_of_range 当 i 越界。
     * @details 与接口式 `Fix(h, i)` 同序：先 down，未移动再 up。
     *          语义等价于 Go `heap.Fix`，对应「任务 deadline 被修改」这类用法。
     */
    void Fix(size_type i) {
        if (i >= m_data.size()) {
            throw std::out_of_range("go2cpp::container::Heap::Fix index out of range");
        }
        if (!SiftDown(i, m_data.size())) {
            SiftUp(i);
        }
    }

    /**
     * @brief 删除下标 i 的元素并返回它，O(log n)。
     * @throws std::out_of_range 当 i 越界。
     * @details 与接口式 `Remove(h, i)` 同序：末尾元素换到 i → 先 down、未移动再 up
     *          → 弹出末元素；对应 Go `heap.Remove`。
     */
    T Remove(size_type i) {
        if (i >= m_data.size()) {
            throw std::out_of_range(
                "go2cpp::container::Heap::Remove index out of range");
        }
        const size_type last = m_data.size() - 1;
        if (i != last) {
            std::swap(m_data[i], m_data[last]);
            if (!SiftDown(i, last)) {
                SiftUp(i);
            }
        }
        T result = std::move(m_data.back());
        m_data.pop_back();
        return result;
    }

 private:
    /**
     * @brief 把用户的 `Less`（Go 的 min-heap 语义）反向后交给 `std::*_heap`。
     * @details 标准库的 `make_heap`/`push_heap`/`pop_heap` 以 `comp` 为「小于」
     *          建 **max-heap**（堆顶是 `comp` 意义下的最大元素）；Go 的
     *          `container/heap` 用 `Less` 建 **min-heap**。因此把比较器反向
     *          （`Inverted(a, b) = Less(b, a)`）后，标准算法的堆顶正好是最小元素。
     *          这是薄封装 `std::*_heap` 时唯一需要留意的语义换算。
     *
     * ## 定位
     * 私有辅助，只被 `Heap::Init/Push/Pop` 用来把用户比较器适配给 `std::*_heap`；
     * 不对外暴露，也不参与接口式自由函数。
     *
     * ## 不变量
     * - 恒有 `Inverted(a,b) == m_less(b,a)`，即严格反转用户比较器；
     * - 只保存**指针** `less`、不拥有比较器：`m_less` 必须比所有 `Inverted` 临时
     *   对象活得久（它们只在单次标准库调用期间存在）；
     * - 若 `m_less` 是严格弱序，反转后仍是严格弱序（等价类不变、方向相反）。
     */
    struct Inverted {
        const Less* less;

        explicit Inverted(const Less* cmp) : less(cmp) {}

        bool operator()(const T& lhs, const T& rhs) const {
            return (*less)(rhs, lhs);
        }
    };

    /**
     * @brief 在下标区间 `[0, n)` 内把 i0 向下 sift。
     * @return 是否真的移动过元素（`Fix`/`Remove` 据此决定要不要再 up）。
     * @details 只在 `std::vector` 上操作，是 `std::*_heap` 没有公开的
     *          「按下标修堆」能力的必要补充；与接口式 `detail::HeapDown` 同构，
     *          但直接作用于 vector，不经过鸭子类型。
     */
    bool SiftDown(size_type i0, size_type n) {
        size_type i = i0;
        while (true) {
            size_type child = 2 * i + 1;
            if (child >= n) {
                break;
            }
            if (child + 1 < n && m_less(m_data[child + 1], m_data[child])) {
                ++child;
            }
            if (!m_less(m_data[child], m_data[i])) {
                break;
            }
            std::swap(m_data[i], m_data[child]);
            i = child;
        }
        return i > i0;
    }

    /** @brief 从 j 向上 sift（与 Go `container/heap.up` 一致）。 */
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

    std::vector<T> m_data;  ///< 堆的存储；由 Push/Pop/Remove/Init 改写，始终维持小根堆
    Less m_less{};          ///< 用户比较器（min-heap 语义）；`Inverted` 反向借用它给标准库算法
};

}  // namespace go2cpp::container
