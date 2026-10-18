/**
 * @file list.hpp
 * @brief Go `container/list` 的 C++ 对应：`std::list` 薄封装 + **指针稳定**的 `Element*`。
 *
 * @details 复用结论（先检索再动手）：
 *   | 候选 | 许可证 | 结论 |
 *   |---|---|---|
 *   | Go 1.23 `container/list`（本机 `third_party/go1.23.0.src.tar.gz`） | BSD-3-Clause | **语义基准**：接口、移动/插入的 no-op 规则、`Element.Next/Prev` 两端返回 nil 的语义逐项对齐；独立实现，不复制源码 |
 *   | `std::list` | C++ 标准库 | **采用为唯一存储与全部结构操作**：标准保证插入/删除/接合**其他**元素时，已有元素的引用与迭代器不失效——这正是 Go `Element*` 的句柄语义。每个 `Element<T>` 保存指向自身节点的迭代器，`Remove`/`MoveToFront`/`MoveToBack`/`MoveBefore`/`MoveAfter` 直接映射到 `erase`/`splice`，全部 O(1) |
 *   | Boost.Intrusive `list` | Boost 1.0 | **不采用**：侵入式链表要求用户把 hook 嵌进元素类型，无法保持 Go 的 `Element{Value}` 形态；引入 Boost 依赖也与本项目定位不符 |
 *   | 手写双向链表（重构前） | — | **已移除**：与 `std::list` 能力完全重合，多出约 300 行维护成本，而压测没有性能优势（1.02x，基本打平） |
 *
 *   **关键语义：`Element*` 指针稳定**
 *   - `std::list` 的节点在插入/删除/接合其他元素时**不会搬移**；每个
 *     `Element<T>` 还保存了指向自身节点的迭代器，因此 `Element*` 在链表的整个
 *     生命周期内保持不变。这正是 Go `container/list` 被用于 LRU 缓存的原因：
 *     `unordered_map<Key, Element*>` 里的句柄不会因其他元素的增删而失效。
 *   - `Remove(e)` 会销毁 `e` 本身；**其它** `Element*` 仍然有效。
 *
 *   **与 Go 的差异（必须知道）**：
 *   - Go 由 GC 管理节点，`Remove` 后仍可读 `e.Value`；C++ 侧 `Remove(e)` 在取出
 *     元素值后立即销毁节点，因此**被删元素自身的指针立即失效**。这避免了 LRU
 *     长时间运行时被淘汰节点无限堆积。
 *   - Go 的 `Remove` 对「不属于本链表」的元素是 no-op 并返回其值；本实现保持该
 *     行为（要求 `T` 可复制，否则抛 `std::invalid_argument`）。
 *   - `InsertValue(value, index)` 是本模块新增的按下标插入便捷接口（Go 里
 *     `insertValue` 是未导出的内部 helper，签名为 `(value, at)`）；复杂度 O(index)。
 *   - `PushBackList`/`PushFrontList` 按 Go 语义**复制** `other` 的每个值
 *     （Go 的 `any` 也是值拷贝）；复杂度 O(other.Len())。`other == this` 合法：
 *     先记住原长度再迭代，不会无限增长。要求 `T` 可复制构造（与 `Remove`
 *     的跨链表分支同一约束）。
 *   - 不提供迭代器；遍历请用 `Front()` + `Element::Next()`，与 Go 写法一致。
 *   - `List` 不可复制/移动：节点由链表独占持有，复制语义需要额外约定；如需要请显式
 *     逐元素复制。
 *
 * @note 复杂度：`PushFront/PushBack/InsertBefore/InsertAfter/Remove/Move*` 均为 O(1)；
 *       `InsertValue(index, ...)` 为 O(index)；`Len` 为 O(1)；`Clear` 为 O(n)。
 */
#pragma once

#include <cstddef>
#include <iterator>
#include <list>
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
 * @details `Value` 是公开字段（与 Go 的 `e.Value` 对应）；节点元数据私有，只有
 *          `List<T>` 能修改。节点地址由 `std::list` 保证在插入/删除其他元素时不变。
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
    Element* Next() const noexcept {
        if (m_list == nullptr) {
            return nullptr;
        }
        auto next = m_it;
        ++next;
        return next == m_list->end() ? nullptr : &*next;
    }

    /**
     * @brief 上一个元素；本节点是头节点时返回 `nullptr`（Go 返回 nil）。
     */
    Element* Prev() const noexcept {
        if (m_list == nullptr || m_it == m_list->begin()) {
            return nullptr;
        }
        auto prev = m_it;
        --prev;
        return &*prev;
    }

 private:
    friend class List<T>;

    /** @brief 节点所属的 `std::list`；`nullptr` 表示未挂入任何链表。 */
    using list_type = std::list<Element<T>>;
    using iterator = typename list_type::iterator;

    list_type* m_list = nullptr;
    iterator m_it{};
};

/**
 * @brief 双向链表（对应 Go `container/list`），内部就是 `std::list`。
 * @tparam T 元素类型；只要求可移动构造/可析构，**不要求默认构造**。
 *
 * @details `Element<T>` 包一层以保持 Go 的 API 形状（`Element*` + `Value`），
 *          节点本身由 `std::list` 分配，`Element*` 的稳定性由标准保证。
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
    ~List() = default;

    List(const List&) = delete;
    List& operator=(const List&) = delete;
    List(List&&) = delete;
    List& operator=(List&&) = delete;

    /** @brief 元素个数，O(1)（对应 Go `l.Len()`）。 */
    size_type Len() const noexcept { return m_data.size(); }

    /** @brief 对应 Go `l.Init()`：清空链表并恢复到可立即使用的空状态。 */
    void Init() { Clear(); }

    /**
     * @brief 清空链表并释放全部节点（`std::list::clear`）。
     * @details 不是 Go 的 API（GC 语言不需要），但 C++ 侧必须有显式释放入口。
     */
    void Clear() { m_data.clear(); }

    /** @brief 首元素；空链表返回 `nullptr`（对应 Go 返回 nil）。 */
    element_type* Front() noexcept {
        return m_data.empty() ? nullptr : &m_data.front();
    }

    /** @brief 尾元素；空链表返回 `nullptr`（对应 Go 返回 nil）。 */
    element_type* Back() noexcept {
        return m_data.empty() ? nullptr : &m_data.back();
    }

    /** @brief 首元素（只读重载，供 `Push*List` 等 const 接口使用）。 */
    const element_type* Front() const noexcept {
        return m_data.empty() ? nullptr : &m_data.front();
    }

    /** @brief 尾元素（只读重载，供 `Push*List` 等 const 接口使用）。 */
    const element_type* Back() const noexcept {
        return m_data.empty() ? nullptr : &m_data.back();
    }

    /**
     * @brief 头插，返回新节点（对应 Go `PushFront`）。
     */
    element_type* PushFront(T value) {
        m_data.emplace_front(std::move(value));
        return Adopt(m_data.begin());
    }

    /**
     * @brief 尾插，返回新节点（对应 Go `PushBack`）。
     */
    element_type* PushBack(T value) {
        m_data.emplace_back(std::move(value));
        auto it = m_data.end();
        --it;
        return Adopt(it);
    }

    /**
     * @brief 对应 Go `PushBackList`：把 `other` 的每个值依次复制到本链表尾部。
     * @param other 源链表，可为 `*this`（先记住原长度，等价于把前 `Len()` 个值
     *              再追加一遍）。
     * @note `T` 必须可复制构造；`Go` 的 `any` 本身就是值拷贝，语义一致。
     */
    void PushBackList(const List& other) {
        const size_type count = other.Len();
        const element_type* element = other.Front();
        for (size_type i = 0; i < count && element != nullptr; ++i) {
            const element_type* const next = element->Next();
            PushBack(element->Value);  // 复制（Go 的 insertValue(e.Value, ...)）
            element = next;
        }
    }

    /**
     * @brief 对应 Go `PushFrontList`：把 `other` 的每个值依次复制到本链表头部。
     * @param other 源链表，可为 `*this`。
     * @note 从 `other.Back()` 向前遍历并逐个 `PushFront`，因此复制后 `other`
     *       的顺序保持不变（与 Go 的实现一致，不是逆序）。
     */
    void PushFrontList(const List& other) {
        const size_type count = other.Len();
        const element_type* element = other.Back();
        for (size_type i = 0; i < count && element != nullptr; ++i) {
            const element_type* const previous = element->Prev();
            PushFront(element->Value);
            element = previous;
        }
    }

    /**
     * @brief 在 `mark` 之前插入，返回新节点（对应 Go `InsertBefore`）。
     * @return `mark` 为 `nullptr` 或不属于本链表时返回 `nullptr` 且不修改链表
     *         （与 Go 的「不是本链表元素则不改动」一致）。
     */
    element_type* InsertBefore(T value, element_type* mark) {
        if (!Belongs(mark)) {
            return nullptr;
        }
        return Adopt(m_data.emplace(mark->m_it, std::move(value)));
    }

    /**
     * @brief 在 `mark` 之后插入，返回新节点（对应 Go `InsertAfter`）。
     * @return `mark` 为 `nullptr` 或不属于本链表时返回 `nullptr` 且不修改链表。
     */
    element_type* InsertAfter(T value, element_type* mark) {
        if (!Belongs(mark)) {
            return nullptr;
        }
        auto pos = mark->m_it;
        ++pos;
        return Adopt(m_data.emplace(pos, std::move(value)));
    }

    /**
     * @brief 把 `value` 插入到下标 `index` 之前（下标从 0 开始）。
     * @details `index >= Len()` 时追加到尾部。这是本模块的 C++ 侧便捷接口，
     *          Go 的 `insertValue` 是未导出的内部 helper，语义不同。
     * @note 复杂度 O(index)；链表没有随机访问。
     */
    element_type* InsertValue(T value, size_type index) {
        if (index >= m_data.size()) {
            return PushBack(std::move(value));
        }
        auto pos = m_data.begin();
        std::advance(pos, static_cast<std::ptrdiff_t>(index));
        return Adopt(m_data.emplace(pos, std::move(value)));
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
        if (!Belongs(e)) {
            if constexpr (std::is_copy_constructible<T>::value) {
                return e->Value;
            } else {
                throw std::invalid_argument(
                    "go2cpp::container::List::Remove: element belongs to another list");
            }
        }
        T value = std::move(e->Value);
        m_data.erase(e->m_it);
        return value;
    }

    /** @brief 移到头部（对应 Go `MoveToFront`）；不属于本链表时 no-op。 */
    void MoveToFront(element_type* e) {
        if (!Belongs(e) || e == Front()) {
            return;
        }
        m_data.splice(m_data.begin(), m_data, e->m_it);
    }

    /** @brief 移到尾部（对应 Go `MoveToBack`）；不属于本链表时 no-op。 */
    void MoveToBack(element_type* e) {
        if (!Belongs(e) || e == Back()) {
            return;
        }
        m_data.splice(m_data.end(), m_data, e->m_it);
    }

    /**
     * @brief 移到 `mark` 之前（对应 Go `MoveBefore`）。
     * @details `e`/`mark` 不属于本链表、`e == mark` 或 `mark == nullptr` 时 no-op。
     */
    void MoveBefore(element_type* e, element_type* mark) {
        if (!Belongs(e) || !Belongs(mark) || e == mark) {
            return;
        }
        m_data.splice(mark->m_it, m_data, e->m_it);
    }

    /**
     * @brief 移到 `mark` 之后（对应 Go `MoveAfter`）。
     * @details `e`/`mark` 不属于本链表、`e == mark` 或 `mark == nullptr` 时 no-op。
     */
    void MoveAfter(element_type* e, element_type* mark) {
        if (!Belongs(e) || !Belongs(mark) || e == mark) {
            return;
        }
        auto pos = mark->m_it;
        ++pos;
        m_data.splice(pos, m_data, e->m_it);
    }

 private:
    /** @brief `e` 是否由本链表持有。 */
    bool Belongs(const element_type* e) const noexcept {
        return e != nullptr && e->m_list == &m_data;
    }

    /** @brief 给刚插入的节点补上「所属链表 + 自身迭代器」元数据，返回稳定句柄。 */
    element_type* Adopt(typename std::list<element_type>::iterator it) noexcept {
        it->m_list = &m_data;
        it->m_it = it;
        return &*it;
    }

    std::list<element_type> m_data;
};

}  // namespace go2cpp::container
