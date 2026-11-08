/**
 * @file k_inline_stack.hpp
 * @brief 基于 KInlineVector 的内联小栈：小深度零堆分配，供迭代式解析器使用。
 *
 * @details
 * 为什么需要它：
 * simpleson 的解析是递归的（`reader::sub_reader`、`jobject::parse` 对嵌套值再解析），
 * 递归深度 = JSON 嵌套深度。深嵌套 JSON（几千层）会耗尽原生栈。改成迭代解析后，
 * 嵌套状态要放在一个显式栈里；如果这个栈用 `std::stack`/`std::deque`，每个小 JSON
 * 也要为栈申请堆块，把「小对象零分配」的收益吃掉。
 *
 * 因此这里用 `KInlineVector<T, N>` 做底层：
 * - 深度 ≤ N 时栈元素在对象内部，**零堆分配**；
 * - 超过 N 才迁移到堆，之后交给 vector 扩容。
 *
 * 接口对齐 `std::stack`（push/pop/top/empty/size），另加：
 * - `emplace`：就地构造；
 * - `try_pop(T&)`：空栈返回 false，避免调用方先判空；
 * - `reserve/clear/data/begin/end`：底层转发，便于解析器预分配与遍历。
 *
 * 约定与 `std::stack` 一致：`top()` / `pop()` 在空栈上是未定义行为；
 * 需要安全版本时用 `try_pop()`。
 *
 * @author week02 重构
 * @date 2026-11-06
 */
#pragma once

#include <cstddef>
#include <utility>

#include "go2cpp/json/kstd/k_inline_vector.hpp"

namespace go2cpp {

/**
 * @brief 小深度零堆分配的栈。
 *
 * @tparam T 元素类型；要求可移动构造（`try_pop` 会移动栈顶）。
 * @tparam N 内联容量；`N >= 1`，由 KInlineVector 保证。
 */
template <typename T, std::size_t N>
class KInlineStack {
 public:
  using value_type = T;
  using size_type = std::size_t;
  using iterator = T*;
  using const_iterator = const T*;

  /// @brief 构造空栈；零堆分配。
  KInlineStack() = default;

  // 五规则由 KInlineVector 成员自动生成（拷贝/移动/析构均正确）。

  /// @brief 拷贝构造栈。
  KInlineStack(const KInlineStack&) = default;

  /// @brief 拷贝赋值。
  KInlineStack& operator=(const KInlineStack&) = default;

  /// @brief 移动构造栈。
  KInlineStack(KInlineStack&&) noexcept = default;

  /// @brief 移动赋值。
  KInlineStack& operator=(KInlineStack&&) noexcept = default;

  /// @brief 析构栈。
  ~KInlineStack() = default;

  /**
   * @brief 压入一个元素（拷贝）。
   * @param value [in] 要复制的元素。
   */
  void push(const T& value) { m_data.push_back(value); }

  /**
   * @brief 压入一个元素（移动）。
   * @param value [in,out] 被移动的元素；返回后处于有效但未指定状态。
   */
  void push(T&& value) { m_data.push_back(std::move(value)); }

  /**
   * @brief 就地构造并压入一个元素。
   * @tparam Args 构造 T 的参数类型包。
   * @param args [in] 完美转发给 T 的构造函数。
   * @return 新元素的引用；在下一次可能重新分配的操作前有效。
   */
  template <typename... Args>
  T& emplace(Args&&... args) {
    return m_data.emplace_back(std::forward<Args>(args)...);
  }

  /**
   * @brief 返回栈顶元素。
   * @return 栈顶引用；空栈调用是未定义行为（与 std::stack 一致）。
   */
  T& top() { return m_data.back(); }

  /// @brief top() 的 const 重载。
  const T& top() const { return m_data.back(); }

  /**
   * @brief 弹出栈顶；空栈调用是未定义行为（与 std::stack 一致）。
   * @note 只销毁，不返回值；需要返回值用 `try_pop`。
   */
  void pop() { m_data.pop_back(); }

  /**
   * @brief 弹出栈顶并取回值；空栈返回 false。
   * @param out [out] 成功时写入被弹出的值；失败时保持原值。
   * @return true 表示弹出成功；false 表示栈空、out 未被修改。
   * @note 先移动再 pop：移动构造抛异常时元素仍在栈上，不丢数据。
   */
  bool try_pop(T& out) {
    if (m_data.empty()) {
      return false;
    }
    out = std::move(m_data.back());
    m_data.pop_back();
    return true;
  }

  /// @brief 栈是否为空。
  bool empty() const noexcept { return m_data.empty(); }

  /// @brief 元素个数。
  size_type size() const noexcept { return m_data.size(); }

  /// @brief 当前容量（内联模式为 N，堆模式为 vector 的 capacity）。
  size_type capacity() const noexcept { return m_data.capacity(); }

  /// @brief 是否已经切到堆（供测试与解析器做策略判断）。
  bool using_heap() const noexcept { return m_data.using_heap(); }

  /// @brief 清空全部元素；不释放堆容量、不回退内联（与 KInlineVector 一致）。
  void clear() noexcept { m_data.clear(); }

  /// @brief 预留容量；`n <= N` 且未用堆时无操作。
  void reserve(size_type n) { m_data.reserve(n); }

  /// @brief 栈底指针（元素按下标从底到顶连续存放）。
  T* data() noexcept { return m_data.data(); }
  /// @brief data() 的 const 重载。
  const T* data() const noexcept { return m_data.data(); }

  /// @brief 自底向顶的起始迭代器。
  iterator begin() noexcept { return m_data.begin(); }
  /// @brief 自底向顶的结束迭代器。
  iterator end() noexcept { return m_data.end(); }
  /// @brief begin() 的 const 重载。
  const_iterator begin() const noexcept { return m_data.begin(); }
  /// @brief end() 的 const 重载。
  const_iterator end() const noexcept { return m_data.end(); }

 private:
  KInlineVector<T, N> m_data;  ///< 底层容器：栈顶对应 back()。
};

}  // namespace extened
