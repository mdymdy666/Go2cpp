/**
 * @file list.hpp
 * @brief Go `container/list` 的 C++ 对应实现：双向链表 + **指针稳定**的 `Element*`。
 *
 * @details 复用优先的结论（先检索再动手）：
 *   | 候选 | 许可证 | 结论 |
 *   |---|---|---|
 *   | Go 1.23 `container/list`（本机 `third_party/go1.23.0.src.tar.gz`） | BSD-3-Clause | **作为语义基准**：接口、移动/插入的 no-op 规则、`Element.Next/Prev` 在两端返回 nil 的语义逐项对齐；独立实现，不复制源码 |
 *   | `std::list` | C++ 标准库 | **采用为对照基线**（随机测试与压测）；**不直接包装**：Go 的 API 以 `*Element` 作为稳定句柄并提供 O(1) 的 `MoveToFront/Back/Before/After`。`std::list` 的节点地址不对外暴露，要拿到稳定句柄得额外存迭代器并再包一层；而且 `Remove` 需要「按句柄删除并返回值」，自己维护节点反而更直接 |
 *   | Boost.Intrusive `list` | Boost 1.0 | **不采用**：侵入式链表要求用户把 hook 嵌进元素类型，无法保持 Go 的 `Element{Value}` 形态；引入 Boost 依赖也与本项目定位不符 |
 *   | `std::unordered_map` + `std::list` 手写 LRU（常见片段） | — | **不采用为模块实现**：本项目要提供的是通用 `container::List`，LRU 只是它的一种用法；真实场景测试里正是用本模块搭 LRU |
 *
 *   **关键语义：`Element*` 指针稳定**
 *   - 每个元素由 `new Element` 单独分配，**绝不在插入/删除其他元素时搬移节点**。
 *     因此 `Element*` 在整个生命周期内保持不变，这正是 Go `container/list` 被用于
 *     LRU 缓存的原因：`unordered_map<Key, Element*>` 里的句柄永远不会因其他元素的
 *     增删而失效。
 *   - `Remove(e)` 会释放 `e` 本身（见下方差异说明）；**其它** `Element*` 仍然有效。
 *
 *   **与 Go 的差异（必须知道）**：
 *   - Go 由 GC 管理节点，`Remove` 后仍可读 `e.Value`；C++ 侧 `Remove(e)` 在返回
 *     元素值后立即 `delete e`，因此**被删元素自身的指针立即失效**。这避免了 LRU
 *     长时间运行时被淘汰节点无限堆积。
 *   - Go 的 `Remove` 对「不属于本链表」的元素是 no-op 并返回其值；本实现保持该
 *     行为（要求 `T` 可复制，否则抛 `std::invalid_argument`）。
 *   - `InsertValue(value, index)` 是本模块新增的按下标插入便捷接口（Go 里
 *     `insertValue` 是未导出的内部 helper，签名为 `(value, at)`）；复杂度 O(index)。
 *   - Go 的 `PushBackList`/`PushFrontList` 未实现（当前没有使用场景）。
 *   - 不提供迭代器；遍历请用 `Front()` + `Element::Next()`，与 Go 写法一致。
 *   - `List` 不可复制/移动：节点由链表独占持有，复制语义需要额外约定；如需要请显式
 *     逐元素复制。
 *
 * @note 复杂度：`PushFront/PushBack/InsertBefore/InsertAfter/Remove/Move*` 均为 O(1)；
 *       `InsertValue(index, ...)` 为 O(index)；`Len` 为 O(1)；`Clear` 为 O(n)。
 */
#pragma once

#include <cstddef>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace go2cpp::container {

template <typename T>
class List;

/**
 * @brief 链表节点（对应 Go `list.Element`）。
 * @tparam T 元素类型。
 *
 * @details `Value` 是公开字段（与 Go 的 `e.Value` 对应）；链接指针私有，只有
 *          `List<T>` 能修改。节点一旦创建，地址在链表的整个生命周期内不变。
 */
template <typename T>
struct Element {
    /** @brief 节点携带的值（Go 的 `Element.Value`）。 */
    T Value;

    Element() = default;
    explicit Element(T value) : Value(std::move(value)) {}

    /**
     * @brief 下一个元素；本节点是尾节点时返回 `nullptr`（Go 返回 nil）。
     */
    Element* Next() const noexcept { return m_next; }

    /**
     * @brief 上一个元素；本节点是头节点时返回 `nullptr`（Go 返回 nil）。
     */
    Element* Prev() const noexcept { return m_prev; }

 private:
    friend class List<T>;

    Element* m_next = nullptr;
    Element* m_prev = nullptr;
    List<T>* m_owner = nullptr;
};

/**
 * @brief 双向链表（对应 Go `container/list`）。
 * @tparam T 元素类型；只要求可移动构造/可析构，**不要求默认构造**。
 *
 * @details 不设哨兵节点，直接维护 `m_front`/`m_back` 两个指针。这样 `Element`
 *          不需要为哨兵构造一个 `T`，因此 `T` 不必默认构造（Go 的哨兵是
 *          `Element{Value:nil}`，对应到 C++ 会强加一个不合理的约束）。
 *
 * @code
 * container::List<int> l;
 * auto* e = l.PushBack(1);
 * l.PushFront(0);
 * l.MoveToBack(e);       // 0, 1
 * int v = l.Remove(e);   // 0，e 本身失效；其它 Element* 不受影响
 * @endcode
 */
template <typename T>
class List {
 public:
    using value_type = T;
    using element_type = Element<T>;
    using size_type = std::size_t;

    List() = default;

    ~List() { Clear(); }

    List(const List&) = delete;
    List& operator=(const List&) = delete;
    List(List&&) = delete;
    List& operator=(List&&) = delete;

    /** @brief 元素个数，O(1)（对应 Go `l.Len()`）。 */
    size_type Len() const noexcept { return m_len; }

    /** @brief 对应 Go `l.Init()`：清空链表并恢复到可立即使用的空状态。 */
    void Init() { Clear(); }

    /**
     * @brief 清空链表并释放全部节点。
     * @details 不是 Go 的 API（GC 语言不需要），但 C++ 侧必须有显式释放入口。
     */
    void Clear() {
        element_type* node = m_front;
        while (node != nullptr) {
            element_type* next = node->m_next;
            delete node;
            node = next;
        }
        m_front = nullptr;
        m_back = nullptr;
        m_len = 0;
    }

    /** @brief 首元素；空链表返回 `nullptr`（对应 Go 返回 nil）。 */
    element_type* Front() noexcept { return m_front; }

    /** @brief 尾元素；空链表返回 `nullptr`（对应 Go 返回 nil）。 */
    element_type* Back() noexcept { return m_back; }

    /**
     * @brief 头插，返回新节点（对应 Go `PushFront`）。
     */
    element_type* PushFront(T value) {
        return InsertNodeBefore(std::move(value), m_front);
    }

    /**
     * @brief 尾插，返回新节点（对应 Go `PushBack`）。
     */
    element_type* PushBack(T value) {
        return InsertNodeBefore(std::move(value), nullptr);
    }

    /**
     * @brief 在 `mark` 之前插入，返回新节点（对应 Go `InsertBefore`）。
     * @return `mark` 为 `nullptr` 或不属于本链表时返回 `nullptr` 且不修改链表
     *         （与 Go 的「不是本链表元素则不改动」一致）。
     */
    element_type* InsertBefore(T value, element_type* mark) {
        if (mark == nullptr || mark->m_owner != this) {
            return nullptr;
        }
        return InsertNodeBefore(std::move(value), mark);
    }

    /**
     * @brief 在 `mark` 之后插入，返回新节点（对应 Go `InsertAfter`）。
     * @return `mark` 为 `nullptr` 或不属于本链表时返回 `nullptr` 且不修改链表。
     */
    element_type* InsertAfter(T value, element_type* mark) {
        if (mark == nullptr || mark->m_owner != this) {
            return nullptr;
        }
        return InsertNodeBefore(std::move(value), mark->m_next);
    }

    /**
     * @brief 把 `value` 插入到下标 `index` 之前（下标从 0 开始）。
     * @details `index >= Len()` 时追加到尾部。这是本模块的 C++ 侧便捷接口，
     *          Go 的 `insertValue` 是未导出的内部 helper，语义不同。
     * @note 复杂度 O(index)；链表没有随机访问。
     */
    element_type* InsertValue(T value, size_type index) {
        if (index >= m_len) {
            return InsertNodeBefore(std::move(value), nullptr);
        }
        element_type* pos = m_front;
        for (size_type i = 0; i < index; ++i) {
            pos = pos->m_next;
        }
        return InsertNodeBefore(std::move(value), pos);
    }

    /**
     * @brief 删除并返回元素值（对应 Go `l.Remove(e)`）。
     * @throws std::invalid_argument `e == nullptr`（Go 在此 panic）。
     * @details 若 `e` 不属于本链表：不做修改并返回它的值（与 Go 一致）；
     *          若 `T` 不可复制，该分支抛 `std::invalid_argument`。
     *          删除成功后 `e` 立即失效（见文件头差异说明），其它 `Element*` 不受影响。
     */
    T Remove(element_type* e) {
        if (e == nullptr) {
            throw std::invalid_argument("go2cpp::container::List::Remove(nullptr)");
        }
        if (e->m_owner != this) {
            if constexpr (std::is_copy_constructible<T>::value) {
                return e->Value;
            } else {
                throw std::invalid_argument(
                    "go2cpp::container::List::Remove: element belongs to another list");
            }
        }
        Detach(e);
        T value = std::move(e->Value);
        delete e;
        return value;
    }

    /** @brief 移到头部（对应 Go `MoveToFront`）；不属于本链表时 no-op。 */
    void MoveToFront(element_type* e) {
        if (e == nullptr || e->m_owner != this || m_front == e) {
            return;
        }
        MoveNodeBefore(e, m_front);
    }

    /** @brief 移到尾部（对应 Go `MoveToBack`）；不属于本链表时 no-op。 */
    void MoveToBack(element_type* e) {
        if (e == nullptr || e->m_owner != this || m_back == e) {
            return;
        }
        MoveNodeBefore(e, nullptr);
    }

    /**
     * @brief 移到 `mark` 之前（对应 Go `MoveBefore`）。
     * @details `e`/`mark` 不属于本链表、`e == mark` 或 `mark == nullptr` 时 no-op。
     */
    void MoveBefore(element_type* e, element_type* mark) {
        if (e == nullptr || mark == nullptr || e == mark || e->m_owner != this ||
            mark->m_owner != this) {
            return;
        }
        MoveNodeBefore(e, mark);
    }

    /**
     * @brief 移到 `mark` 之后（对应 Go `MoveAfter`）。
     * @details `e`/`mark` 不属于本链表、`e == mark` 或 `mark == nullptr` 时 no-op。
     */
    void MoveAfter(element_type* e, element_type* mark) {
        if (e == nullptr || mark == nullptr || e == mark || e->m_owner != this ||
            mark->m_owner != this) {
            return;
        }
        element_type* pos = mark->m_next;
        if (pos == e) {
            return;  // 已经在 mark 之后
        }
        MoveNodeBefore(e, pos);
    }

 private:
    /** @brief 把新节点接到 `pos` 之前；`pos == nullptr` 表示接到尾部。 */
    element_type* InsertNodeBefore(T value, element_type* pos) {
        element_type* node = new element_type(std::move(value));
        AttachBefore(node, pos);
        return node;
    }

    /** @brief 把已经分配好的节点接到 `pos` 之前，并维护首尾指针与长度。 */
    void AttachBefore(element_type* node, element_type* pos) noexcept {
        node->m_prev = (pos != nullptr) ? pos->m_prev : m_back;
        node->m_next = pos;
        if (node->m_prev != nullptr) {
            node->m_prev->m_next = node;
        } else {
            m_front = node;
        }
        if (pos != nullptr) {
            pos->m_prev = node;
        } else {
            m_back = node;
        }
        node->m_owner = this;
        ++m_len;
    }

    /** @brief 从链表中摘除节点（不释放内存、不修改节点值）。 */
    void Detach(element_type* node) noexcept {
        if (node->m_prev != nullptr) {
            node->m_prev->m_next = node->m_next;
        } else {
            m_front = node->m_next;
        }
        if (node->m_next != nullptr) {
            node->m_next->m_prev = node->m_prev;
        } else {
            m_back = node->m_prev;
        }
        node->m_next = nullptr;
        node->m_prev = nullptr;
        node->m_owner = nullptr;
        --m_len;
    }

    /**
     * @brief 把链内节点移动到 `pos` 之前，净长度不变。
     * @details 先 Detach 再 Attach；调用方已保证 `node != pos`，因此 `pos` 在
     *          Detach 之后仍然有效。
     */
    void MoveNodeBefore(element_type* node, element_type* pos) noexcept {
        Detach(node);
        AttachBefore(node, pos);
    }

    element_type* m_front = nullptr;
    element_type* m_back = nullptr;
    size_type m_len = 0;
};

}  // namespace go2cpp::container
