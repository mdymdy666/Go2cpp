/**
 * @file ring.hpp
 * @brief Go `container/ring` 的 C++ 对应实现：环形链表。
 *
 * @details 复用优先的结论（先检索再动手）：
 *   | 候选 | 许可证 | 结论 |
 *   |---|---|---|
 *   | Go 1.23 `container/ring`（本机 `third_party/go1.23.0.src.tar.gz`） | BSD-3-Clause | **作为语义基准**：`Link`/`Unlink`/`Move`/`Len`/`Do` 的实现流程逐分支对照，尤其是「同一个环自链接时切出子环」这条反直觉语义；独立实现，不复制源码 |
 *   | Boost.CircularBuffer | Boost 1.0 | **不是同一数据结构**：固定容量数组环形缓冲，不支持任意 `Link`/`Unlink` 拼接，不采用 |
 *   | `std::list` + 手工首尾相接 | C++ 标准库 | **不采用为模块实现**：可以模拟，但拿不到 `Link` 的「返回原 `r.Next()`」与 `Unlink` 的「返回被摘下的子环」这两个指针级语义；且 `std::list` 节点地址不可作为稳定句柄 |
 *   | `std::vector` + 下标取模 | C++ 标准库 | **仅用作压测中的参考基线**（顺序扫描 vs 指针追逐），不实现模块 |
 *
 *   **关键语义（逐条对齐 Go）**：
 *   - `New(n)`：创建 n 个节点的环；`n <= 0` 返回 `nullptr`（Go 返回 nil）。
 *   - 环没有起点/终点，任何节点指针都代表整个环。
 *   - `Link(s)`：把环 s 接到本节点之后，**返回链接前的 `r.Next()`**。
 *     - `s == nullptr`：不改动，直接返回 `r.Next()`。
 *     - `r` 与 `s` 在**同一个环**：`r` 与 `s` 之间的元素被切出来形成一个子环，
 *       返回值指向该子环；若 `s` 就是 `r.Next()` 则没有元素被切出。
 *     - 不同环：把 s 的整个环插入到 r 之后，返回值指向「s 的最后一个元素之后」
 *       的那个原 r 环元素。
 *   - `Unlink(n)`：从 `r.Next()` 开始摘掉 `n % r.Len()` 个元素，返回被摘下元素组成
 *     的环；`n <= 0` 或 `n % Len == 0` 时返回 `nullptr` 且本环不变。
 *   - `Move(n)`：沿环前进 n 步（n < 0 后退），返回新位置；`n == 0` 返回自身。
 *   - `Do(f)`：从本节点开始按 `Next()` 顺序对每个节点的 `Value` 调用 `f`（传引用）。
 *     `f` 修改环结构时行为未定义（与 Go 文档一致）。
 *
 *   **C++ 侧必须补充的内存语义（Go 有 GC，C++ 没有）**：
 *   - 节点由调用方持有。由于环是循环引用，`shared_ptr` 无法自动回收，本类用裸指针。
 *   - **释放整个环请调用 `Ring::DeleteRing(r)`**（静态方法），它会逐个 `delete` 环内
 *     所有节点；参数可以是环内任意节点，传 `nullptr` 是安全的空操作。
 *   - 单独 `delete p` 只会把 p 从环中摘掉（析构函数修正左右邻居指针），其余节点仍然
 *     存活；这不是 Go 的 API，只是为了避免 C++ 中「删一个节点导致邻居指向已释放内存」。
 *   - `Link`/`Unlink` 只搬移指针，不分配也不释放节点；跨环拼接后原指针不再代表原来的
 *     环，调用方需要自己维护「哪组指针现在构成哪个环」。
 *
 * @note `New(n)` 与默认构造都要求 `T` 可默认构造（Go 的零值语义）；若 `T` 不可默认
 *       构造，请先用 `Ring<T>(value)` 创建单个节点再 `Link`。
 *
 * ## 模块定位
 *
 * 属于「Go 标准库容器语义」适配层，是三个容器里唯一**手工管理节点生命周期**的一个：
 * 上游是 Go `container/ring` 的指针语义，下游（当前仓库内）是 `tests/container/test_ring.cpp`
 * （逐项移植 Go 官方测试）、`test_random.cpp`、`test_stress.cpp` 与 `test_scenarios.cpp`
 * 的轮转分配场景。不依赖运行时，可作为纯数据结构单独使用。
 *
 * ## 本文件定义什么
 *
 * - `Ring<T>`：环形链表节点兼容器句柄。环没有头/尾，任意节点指针都代表整个环；
 * - `Ring::New` / `Ring::DeleteRing`：整环的创建与释放（C++ 侧必须显式提供）；
 * - `Next` / `Prev` / `Move` / `Link` / `Unlink` / `Len` / `Do`：与 Go 一一对应的操作。
 *
 * ## 阅读建议
 *
 * 先读文件头「关键语义」里 `Link` 的三种分支（尤其**同环自链接切出子环**），这是唯一
 * 反直觉的地方；再看 `Unlink` 如何用 `Link(Move(n + 1))` 一行表达；最后看析构函数与
 * `DeleteRing`，理解 C++ 侧为什么要额外约定释放方式。
 *
 * ## 边界与陷阱
 *
 * - 环是循环引用，`shared_ptr` 永远回收不了，所以本类只能用裸指针；**释放整环必须调
 *   `DeleteRing`**，逐个 `delete` 只会把节点依次摘掉。
 * - `Link` / `Unlink` 只搬指针，不分配、不释放；跨环拼接后，原来的指针不再代表原来的环。
 * - `Do(f)` 期间若 `f` 修改环结构，行为未定义（与 Go 文档一致），因为遍历依赖 `m_next`。
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>

namespace go2cpp::container {

/**
 * @brief 环形链表节点（对应 Go `ring.Ring`）。
 * @tparam T 元素类型。
 *
 * @details `Value` 是公开字段（Go `r.Value`）；`next`/`prev` 私有，只能通过
 *          `Next`/`Prev`/`Move`/`Link`/`Unlink` 操作。
 *
 * ## 定位
 *
 * 节点与容器合一：调用方拿到的每个 `Ring*` 既是元素句柄，也是「整个环」的唯一入口。
 * 供 `tests/container/*` 与未来的轮转调度/工作窃取游标使用，不依赖运行时。
 *
 * ## 不变量
 *
 * - `m_next` / `m_prev` 恒非空，且互为逆：`m_next->m_prev == this` 且
 *   `m_prev->m_next == this`（单元素环时两者都等于 `this`）。
 * - 环中不存在头/尾标记，`Len()` 从 `this` 起绕一圈计数，结果与起点无关。
 * - 除析构函数与 `DeleteRing` 外，任何操作都不改变节点数量。
 */
template <typename T>
class Ring {
 public:
    /** @brief 构造一个单元素环（对应 Go 的零值 `Ring`，但立即完成自环初始化）。 */
    Ring() noexcept : Value{}, m_next(this), m_prev(this) {}

    /** @brief 构造一个带初值的单元素环。 */
    explicit Ring(T value) : Value(std::move(value)), m_next(this), m_prev(this) {}

    /**
     * @brief 析构时把自己从环中摘除，避免邻居留下悬空指针。
     * @warning 只释放一个节点用 `delete`；释放整个环必须用 `DeleteRing`。
     */
    ~Ring() {
        if (m_next != nullptr && m_next != this) {
            m_prev->m_next = m_next;
            m_next->m_prev = m_prev;
        }
        m_next = nullptr;
        m_prev = nullptr;
    }

    Ring(const Ring&) = delete;
    Ring& operator=(const Ring&) = delete;
    Ring(Ring&&) = delete;
    Ring& operator=(Ring&&) = delete;

    /**
     * @brief 创建 n 个节点的环（对应 Go `ring.New`）。
     * @return 首节点指针；`n <= 0` 返回 `nullptr`。
     * @throws std::bad_alloc 内存不足；此时已分配节点会全部释放。
     */
    static Ring* New(std::size_t n) {
        if (n == 0) {
            return nullptr;
        }
        Ring* first = new Ring();
        Ring* tail = first;
        try {
            for (std::size_t i = 1; i < n; ++i) {
                Ring* node = new Ring();
                tail->m_next = node;
                node->m_prev = tail;
                tail = node;
            }
        } catch (...) {
            // 先把已分配节点闭合成环，再统一释放，避免异常路径泄漏。
            tail->m_next = first;
            first->m_prev = tail;
            DeleteRing(first);
            throw;
        }
        tail->m_next = first;
        first->m_prev = tail;
        return first;
    }

    /**
     * @brief 释放 `r` 所在的整个环；`r == nullptr` 时为空操作。
     * @warning `r` 必须是环内仍然有效的节点；释放后环内所有指针都失效。
     */
    static void DeleteRing(Ring* r) {
        if (r == nullptr) {
            return;
        }
        const std::size_t n = r->Len();
        Ring* node = r;
        for (std::size_t i = 0; i < n; ++i) {
            Ring* next = node->m_next;
            delete node;  // 析构函数会修正 next/prev，next 仍然有效
            node = next;
        }
    }

    /** @brief 下一个节点（对应 Go `Next`）；单元素环返回自身。 */
    Ring* Next() noexcept { return m_next; }

    /** @brief 下一个节点（const 版本）。 */
    const Ring* Next() const noexcept { return m_next; }

    /** @brief 上一个节点（对应 Go `Prev`）；单元素环返回自身。 */
    Ring* Prev() noexcept { return m_prev; }

    /** @brief 上一个节点（const 版本）。 */
    const Ring* Prev() const noexcept { return m_prev; }

    /**
     * @brief 沿环移动 n 步：n > 0 前进，n < 0 后退（对应 Go `Move`）。
     * @return 目标节点；`n == 0` 返回 `this`。
     * @note 复杂度 O(|n|)（与 Go 实现一致；`Unlink` 内部依赖这一点）。
     */
    Ring* Move(std::int64_t n) noexcept {
        Ring* node = this;
        if (n < 0) {
            for (; n < 0; ++n) {
                node = node->m_prev;
            }
        } else {
            for (; n > 0; --n) {
                node = node->m_next;
            }
        }
        return node;
    }

    /**
     * @brief 把环 `s` 链接到本节点之后（对应 Go `Link`）。
     * @return 链接前的 `this->Next()`。
     * @details `s == nullptr` 时不修改任何指针；`s` 与本节点同环时切出子环。
     *          完整语义见文件头注释。
     */
    Ring* Link(Ring* s) noexcept {
        Ring* next = m_next;
        if (s != nullptr) {
            Ring* s_prev = s->m_prev;
            // 与 Go 相同的四步改链。不能写成多重赋值：C++ 的求值顺序同样
            // 不保证「先读旧值再写新值」的直觉。
            m_next = s;
            s->m_prev = this;
            next->m_prev = s_prev;
            s_prev->m_next = next;
        }
        return next;
    }

    /**
     * @brief 从 `this->Next()` 开始摘掉 n 个元素（对应 Go `Unlink`）。
     * @return 被摘下的元素组成的环；没有元素被摘下时返回 `nullptr`。
     * @details 实现与 Go 相同：`n <= 0` 返回 nil，否则 `Link(Move(n + 1))`。
     *          `n` 超过环长时按 `n % Len` 处理。
     */
    Ring* Unlink(std::int64_t n) noexcept {
        if (n <= 0) {
            return nullptr;
        }
        return Link(Move(n + 1));
    }

    /**
     * @brief 环中节点个数（对应 Go `Len`），O(n)。
     */
    std::size_t Len() const noexcept {
        std::size_t count = 1;
        for (const Ring* node = m_next; node != this; node = node->m_next) {
            ++count;
        }
        return count;
    }

    /**
     * @brief 从本节点开始按 Next() 顺序遍历，对每个 `Value` 调用 `f`（对应 Go `Do`）。
     * @details `f` 接收 `T&`；若 `f` 修改环结构，行为未定义（与 Go 文档一致）。
     */
    template <typename F>
    void Do(F&& f) {
        f(Value);
        for (Ring* node = m_next; node != this; node = node->m_next) {
            f(node->Value);
        }
    }

    /** @brief 元素值（对应 Go `Ring.Value`）。 */
    T Value;

 private:
    Ring* m_next;  ///< 后继；Link/Unlink/析构改写，Next/Move/Do/Len 读取
    Ring* m_prev;  ///< 前驱；Link/Unlink/析构改写，Prev/Move 读取
};

}  // namespace go2cpp::container
