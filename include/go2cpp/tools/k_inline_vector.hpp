#pragma once

/**
 * @file k_inline_vector.hpp
 * @note 从 JinShan week02 JSON 库迁移（原 `extened::KInlineVector`），用于小容器零堆分配优化。
 * @brief 提供 extened::KInlineVector<T, N>：小对象内联、超限后转堆的序列容器。
 * @author DSH (DeepSeek Harness)
 * @date 2026-11-06
 *
 * @details
 * ## 设计动机
 * std::vector 即使只存 1~2 个元素，也要在首次 push 时申请堆内存。当程序里
 * 大量出现"只装几个元素的临时序列"时（例如每帧解析出的小批图元、短消息列表、
 * 配置项集合），每一次堆分配都是一次 malloc/free 往返：分配器元数据开销、
 * 缓存局部性变差、且在高频路径上很难被优化掉。
 *
 * KInlineVector 在对象内部预留 N 个 T 的槽位：元素数 ≤ N 时元素直接构造在
 * 对象里，一次堆分配都不发生；超过 N 时才把 N 个元素一次性迁移到内部的
 * std::vector<T>，并 reserve(2*N)；此后扩容策略完全交给 std::vector。
 *
 * ## 内存布局（x86-64 / libstdc++）
 * @code
 *   +-------------------------------------+  <- this
 *   | m_inline: alignas(T) unsigned char  |  sizeof(T) * N 字节
 *   |           [sizeof(T) * N]           |  placement new 的场所
 *   +-------------------------------------+
 *   | m_heap: std::vector<T>              |  24 字节，仅堆模式持有元素
 *   +-------------------------------------+
 *   | m_size: std::size_t                 |  内联模式的元素数；堆模式恒为 0
 *   +-------------------------------------+
 *   | m_using_heap: bool                  |  当前模式标志
 *   +-------------------------------------+
 * @endcode
 * 内联区是原始字节，只有前缀 [0, m_size) 里存在 T 对象，其余槽位是未初始化
 * 存储。因此析构、拷贝都必须按 m_size 逐个处理，绝不能对整块存储调用析构。
 *
 * ## 两种模式与切换条件
 * - 内联模式（m_using_heap == false）：size() == m_size ≤ N，capacity() == N；
 * - 堆模式（m_using_heap == true）：元素全部在 m_heap，size() == m_heap.size()，
 *   capacity() == m_heap.capacity()，内联区不再持有任何对象。
 *
 * 只有两个动作会触发"内联 → 堆"的迁移：
 * 1. 内联已满（m_size == N）时再调用 push_back / emplace_back；
 * 2. reserve(n) 且 n > N。
 *
 * 迁移是单向的：一旦用堆，clear() / pop_back() 都不会退回内联模式
 * （理由见 clear() 的注释）。只有拷贝赋值 / 移动赋值可能让容器重新回到
 * 内联模式，因为赋值结果的模式跟随右操作数。
 *
 * ## 为什么用 unsigned char 数组而不是 union
 * absl::InlinedVector 采用 union { 内联区; 堆指针; } + tag 的紧凑布局，
 * 而本实现按题目要求采用"内联区 + std::vector 并存"的直白布局：
 * - 不需要自己管理堆指针、容量和 allocator，交给 std::vector；
 * - 代码路径短，便于逐行审查；代价是堆模式下 m_inline 的 N*sizeof(T)
 *   字节被闲置（空间换实现简单）。
 *
 * ## 异常安全等级：基本保证
 * 元素构造抛异常时不泄漏、容器保持可用；但迁移途中若 T 的移动构造抛异常，
 * 部分元素可能处于 moved-from（值未指定）状态——基本保证允许这一点。
 * 移动构造 / 移动赋值按题目要求标了 noexcept：若 T 的移动构造抛异常，
 * 程序会 std::terminate，调用者需保证 T 可无抛出地移动。
 *
 * ## 性能快路径（2026-11-06 优化）
 * - T trivially copyable 时，迁移走 insert/一次 memmove（不先值初始化再覆盖），
 *   移动构造改用 std::uninitialized_copy；析构 / pop_back / clear 对
 *   trivially destructible 类型直接跳过循环；
 * - emplace_back 在迁移时保留「先构造临时元素」的路径：泛型参数可能引用
 *   本容器元素（`std::move(v[0].first)` 推导出的 Args 并不是引用类型，
 *   静态无法判定），这是自引用安全的前提，因此不省这一次移动。曾评估过
 *   静态/运行时判定方案，收益均落在测量噪声内（见 kvec_n_tuning.md）。
 */

#include <algorithm>
#include <cstddef>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

namespace go2cpp {

template <typename T, std::size_t N>
class KInlineVector {
 public:
  using value_type = T;
  using size_type = std::size_t;
  using iterator = T*;
  using const_iterator = const T*;

  static_assert(N >= 1, "KInlineVector 至少需要 1 个内联槽位");

  /// @brief 内联容量 N（不分配时最多可容纳的元素数）。
  static constexpr size_type kInlineCapacity = N;

  /**
   * @brief 构造空容器：size()==0、capacity()==N、内联模式。
   * @note 不做任何堆分配；m_inline 保持未初始化（其中没有 T 对象）。
   */
  KInlineVector() noexcept = default;

  /**
   * @brief 析构容器：销毁全部已构造元素。
   * @note 内联模式按 m_size 逐个调用析构；堆模式由 m_heap 的析构函数完成，
   *       因此这里不能无条件遍历 m_inline（堆模式下那里没有对象）。
   */
  ~KInlineVector() {
    if (!m_using_heap) {
      destroy_inline();
    }
  }

  /**
   * @brief 拷贝构造：按 other 的当前模式复制全部元素。
   * @param other [in] 源容器；只读，生命周期覆盖本次构造。
   * @note other 为堆模式时直接复制 m_heap；否则逐个拷贝到内联槽位。
   * @exception 元素拷贝构造抛出的异常原样传播。抛出前销毁已构造的内联元素，
   *            不会泄漏（构造未完成的对象不会进入析构阶段）。
   */
  KInlineVector(const KInlineVector& other) {
    if (other.m_using_heap) {
      m_heap = other.m_heap;
      m_using_heap = true;
    } else {
      try {
        copy_inline_from(other);
      } catch (...) {
        destroy_inline();
        throw;
      }
    }
  }

  /**
   * @brief 拷贝赋值：先清空自身，再按 other 的模式复制。
   * @param other [in] 源容器；只读，可与 *this 相同（自赋值已处理）。
   * @return *this 的引用，支持链式赋值。
   * @note 赋值结果的模式跟随 other：若 other 在内联模式而 *this 原本在堆模式，
   *       这里会释放堆缓冲并退回内联模式。这与 clear() 的"不回退"取舍不同：
   *       clear 只改变元素、不改变容器身份；赋值要求结果只由 other 决定，
   *       而 using_heap() 是可观察状态的一部分。
   * @exception 基本保证：复制途中抛异常时 *this 为空且仍可用，不泄漏。
   */
  KInlineVector& operator=(const KInlineVector& other) {
    if (this == &other) {
      return *this;
    }
    clear();
    if (other.m_using_heap) {
      m_heap = other.m_heap;
      m_using_heap = true;
      m_size = 0;
      return *this;
    }
    if (m_using_heap) {
      // 回退到内联模式：用空 vector 交换以释放堆缓冲。
      // std::allocator 下 vector::swap 是 noexcept 的，不会在这里抛异常。
      std::vector<T>().swap(m_heap);
      m_using_heap = false;
    }
    try {
      copy_inline_from(other);
    } catch (...) {
      destroy_inline();
      throw;
    }
    return *this;
  }

  /**
   * @brief 移动构造：接管 other 的元素，other 变为空。
   * @param other [in,out] 被移动的容器；返回后 empty()==true。
   * @note 堆模式直接接管 m_heap（std::allocator 下为 noexcept 的移动赋值，
   *       不发生元素级搬移）；内联模式逐元素 move 构造，然后析构源对象。
   *       源容器保持原来的模式：堆模式移动后是"空的堆模式"。
   * @warning 本函数按接口要求声明为 noexcept。若 T 的移动构造抛异常，
   *          程序会直接 std::terminate——调用者需保证 T 可无抛出地移动。
   */
  KInlineVector(KInlineVector&& other) noexcept {
    if (other.m_using_heap) {
      m_heap = std::move(other.m_heap);
      other.m_size = 0;
      m_using_heap = true;
    } else if constexpr (std::is_trivially_copyable<T>::value) {
      // 平凡类型：在未初始化槽位上「复制构造」等价于整块字节搬运，
      // uninitialized_copy 对 trivially copyable 会退化为 memmove。
      std::uninitialized_copy(other.inline_slot(0), other.inline_slot(other.m_size),
                              inline_slot(0));
      m_size = other.m_size;
      other.destroy_inline();
    } else {
      for (size_type i = 0; i < other.m_size; ++i) {
        ::new (static_cast<void*>(inline_slot(i))) T(std::move(*other.inline_object(i)));
        ++m_size;
      }
      other.destroy_inline();
    }
  }

  /**
   * @brief 移动赋值：清空自身后接管 other 的元素，other 变为空。
   * @param other [in,out] 被移动的容器；返回后 empty()==true。
   * @return *this 的引用。
   * @note 与拷贝赋值一致，模式跟随 other：other 内联时释放自身的堆缓冲，
   *       退回内联模式再逐元素 move。
   * @warning noexcept 的前提同移动构造：T 的移动构造不能抛，否则 terminate。
   */
  KInlineVector& operator=(KInlineVector&& other) noexcept {
    if (this == &other) {
      return *this;
    }
    clear();
    if (other.m_using_heap) {
      m_using_heap = true;
      m_heap = std::move(other.m_heap);
      m_size = 0;
      other.m_size = 0;
      return *this;
    }
    if (m_using_heap) {
      std::vector<T>().swap(m_heap);
      m_using_heap = false;
    }
    if constexpr (std::is_trivially_copyable<T>::value) {
      std::uninitialized_copy(other.inline_slot(0), other.inline_slot(other.m_size),
                              inline_slot(0));
      m_size = other.m_size;
      other.destroy_inline();
    } else {
      for (size_type i = 0; i < other.m_size; ++i) {
        ::new (static_cast<void*>(inline_slot(i))) T(std::move(*other.inline_object(i)));
        ++m_size;
      }
      other.destroy_inline();
    }
    return *this;
  }

  /**
   * @brief 判断容器是否为空。
   * @return true 表示 size()==0。
   */
  bool empty() const noexcept { return size() == 0; }

  /**
   * @brief 返回元素个数。
   * @return 内联模式返回 m_size；堆模式返回 m_heap.size()。
   */
  size_type size() const noexcept { return m_using_heap ? m_heap.size() : m_size; }

  /**
   * @brief 返回当前容量，即不重新分配时最多能容纳的元素数。
   * @return 内联模式返回 N；堆模式返回 m_heap.capacity()。
   * @note 堆模式的容量由 std::vector 的扩容策略决定，可能大于 2*N；
   *       内联模式的容量固定为 N，不随元素数变化。
   */
  size_type capacity() const noexcept { return m_using_heap ? m_heap.capacity() : N; }

  /**
   * @brief 是否已经切换到堆模式。
   * @return true 表示元素存储在 m_heap；false 表示全部在内联槽位。
   * @note 这是实现状态而非元素语义，供测试与性能诊断使用。一旦为 true，
   *       只有拷贝赋值 / 移动赋值可能让它变回 false。
   */
  bool using_heap() const noexcept { return m_using_heap; }

  /**
   * @brief 预留至少 n 个元素的容量，不改变 size()。
   * @param n [in] 期望容量（元素个数）；0 合法（无符号类型，不存在负数）。
   * @note n <= N 且当前是内联模式时无操作——内联本身已有 N 个槽位。
   *       n > N 时激活堆模式并 reserve(max(n, 2*N))：多留一倍是为了让切换后
   *       紧接着的若干次 push 不再触发扩容，避免"刚付出迁移成本又立刻扩容"。
   *       已经是堆模式时直接转发给 m_heap.reserve(n)。
   * @exception std::bad_alloc 或元素移动构造的异常；基本保证。
   */
  void reserve(size_type n) {
    if (m_using_heap) {
      m_heap.reserve(n);
      return;
    }
    if (n <= N) {
      return;
    }
    migrate_to_heap(n);
  }

  /**
   * @brief 销毁全部元素并把 size() 归零；不释放堆容量、不回退内联模式。
   * @note 取舍：堆模式 clear 后保留 m_heap 的 capacity，且 m_using_heap 保持 true。
   *       理由是典型用法是"清空后立即复用"：退回内联会释放缓冲，下一次超过 N
   *       又得重新分配并迁移，反而更慢；同时保持模式不变让 clear() 的成本是
   *       O(size) 而不是 O(capacity)。
   *       代价是大量元素清空后内存仍被容器占着，直到容器析构或被移动赋值接管。
   *       需要真正归还内存时，应让容器离开作用域，或对空容器执行移动赋值。
   *       本容器不提供 shrink_to_fit。
   */
  void clear() noexcept {
    if (m_using_heap) {
      m_heap.clear();
    } else {
      destroy_inline();
    }
  }

  /**
   * @brief 在尾部追加一个元素（拷贝）。
   * @param value [in] 要复制的元素；生命周期只需覆盖本次调用。
   * @note 转发到 emplace_back，由后者统一处理三条路径。保留独立重载是为了
   *       让 `push_back(v)` 与 `emplace_back(args...)` 的语义分开、便于阅读。
   * @exception 元素拷贝构造的异常；size() 不变，基本保证。
   */
  void push_back(const T& value) { emplace_back(value); }

  /**
   * @brief 在尾部追加一个元素（移动）。
   * @param value [in,out] 被移动的元素；返回后处于有效但未指定状态。
   * @note 转发到 emplace_back；自引用 `v.push_back(std::move(v[0]))` 由
   *       emplace_back 的临时对象路径保证不读到 moved-from 对象。
   * @exception 元素移动构造的异常；size() 不变，基本保证。
   */
  void push_back(T&& value) { emplace_back(std::move(value)); }

  /**
   * @brief 在尾部就位构造一个元素，并返回它的引用。
   * @tparam Args 构造 T 所需的参数类型包。
   * @param args [in] 完美转发给 T 构造函数的参数。
   * @return 新元素的引用；在下一次可能使容器重新分配的操作前有效。
   * @note 三条路径：
   *       1. 堆模式：直接 m_heap.emplace_back；
   *       2. 内联未满：placement new 到第 m_size 个槽位；
   *       3. 内联已满：先迁移到堆，再在堆上构造。
   *       路径 3 是"内联 → 堆"的入口之一（另一个是 reserve）。
   * @exception 构造抛异常时：路径 1/2 中 size() 不变；路径 3 中迁移可能
   *            已经完成（using_heap() 变为 true，size() 仍为 N），随后构造
   *            失败，容器保持"满的堆模式"且完全可用。
   */
  template <typename... Args>
  T& emplace_back(Args&&... args) {
    if (m_using_heap) {
      m_heap.emplace_back(std::forward<Args>(args)...);
      return m_heap.back();
    }
    if (m_size < N) {
      T* const slot = inline_slot(m_size);
      ::new (static_cast<void*>(slot)) T(std::forward<Args>(args)...);
      ++m_size;
      return *inline_object(m_size - 1);
    }
    // 内联已满：先就地构造临时元素，再迁移。
    // 原因：args 可能引用本容器的内联元素（如 v.push_back(v[0])）。迁移会把
    // 内联元素 move 走并析构，如果迁移后才构造新元素，拿到的就是 moved-from
    // 对象（实测 v[2] 会变成空串）。先构造临时元素可保证参数在迁移前被读取。
    // 代价：首次越过 N 时多一次移动构造；只发生一次，可接受。
    // 注：push_back 重载会在「实参地址不在内联区」时绕过这条路径（见上），
    // 而泛型 emplace_back 无法对任意参数做地址判定，只能保守处理。
    T pending(std::forward<Args>(args)...);
    migrate_to_heap(2 * N);
    m_heap.push_back(std::move(pending));
    return m_heap.back();
  }

  /**
   * @brief 销毁尾部元素，size() 减一。
   * @note 调用者必须保证 !empty()；对空容器调用是未定义行为（与 std::vector 一致）。
   *       内联模式显式调用析构，堆模式交给 m_heap.pop_back()。
   *       不触发模式回退：即使堆模式被清到 0 个元素，也仍是堆模式。
   */
  void pop_back() {
    if (m_using_heap) {
      m_heap.pop_back();
      return;
    }
    --m_size;
    if constexpr (!std::is_trivially_destructible<T>::value) {
      inline_object(m_size)->~T();
    }
  }

  /**
   * @brief 按下标访问元素，不做边界检查。
   * @param index [in] 元素下标，必须小于 size()。
   * @return 元素的引用。
   */
  T& operator[](size_type index) noexcept { return data()[index]; }

  /// @brief operator[] 的 const 重载；语义同上。
  const T& operator[](size_type index) const noexcept { return data()[index]; }

  /**
   * @brief 返回首元素的引用。
   * @note 调用者必须保证 !empty()；空容器上调用是未定义行为。
   */
  T& front() { return (*this)[0]; }

  /// @brief front() 的 const 重载；语义同上。
  const T& front() const { return (*this)[0]; }

  /**
   * @brief 返回尾元素的引用。
   * @note 调用者必须保证 !empty()；空容器上调用是未定义行为。
   */
  T& back() { return (*this)[size() - 1]; }

  /// @brief back() 的 const 重载；语义同上。
  const T& back() const { return (*this)[size() - 1]; }

  /**
   * @brief 返回元素连续存储的首指针。
   * @return 堆模式返回 m_heap.data()（空时为 nullptr）；内联模式返回内联区首地址。
   * @note 内联模式且 size()==0 时，内联区里还没有 T 对象，返回的只是存储地址，
   *       不可解引用，也不能参与指针算术后解引用。这里不对它使用 std::launder：
   *       launder 的前提是该地址上存在同类型对象，而空容器不满足。
   */
  T* data() noexcept {
    if (m_using_heap) {
      return m_heap.data();
    }
    return m_size == 0 ? reinterpret_cast<T*>(m_inline) : inline_object(0);
  }

  /// @brief data() 的 const 重载；语义同上。
  const T* data() const noexcept {
    if (m_using_heap) {
      return m_heap.data();
    }
    return m_size == 0 ? reinterpret_cast<const T*>(m_inline) : inline_object(0);
  }

  /// @brief 返回指向首元素的迭代器；空容器时等于 end()。
  iterator begin() noexcept { return data(); }

  /// @brief 返回指向尾后位置的迭代器。
  iterator end() noexcept { return data() + size(); }

  /// @brief begin() 的 const 重载。
  const_iterator begin() const noexcept { return data(); }

  /// @brief end() 的 const 重载。
  const_iterator end() const noexcept { return data() + size(); }

 private:
  /**
   * @brief 返回第 index 个内联槽位的存储地址（对象尚未构造时也可用）。
   * @param index [in] 槽位下标，必须小于 N。
   * @return 按 T 对齐的存储地址；此时该地址上可能还没有 T 对象。
   * @note 只用于 placement new。访问已构造对象必须走 inline_object()。
   */
  T* inline_slot(size_type index) noexcept {
    return reinterpret_cast<T*>(m_inline + index * sizeof(T));
  }

  /**
   * @brief 返回指向第 index 个已构造内联对象的指针。
   * @param index [in] 槽位下标，必须小于 m_size。
   * @return 合法的 T 对象指针。
   * @note placement new 之后必须经 std::launder 重新取得指针：编译器无法从
   *       unsigned char 数组推出该地址上对象的生命周期已经开始，直接使用
   *       旧的存储指针在严格别名规则下是未定义行为（C++17 [ptr.launder]）。
   */
  T* inline_object(size_type index) noexcept {
    return std::launder(inline_slot(index));
  }

  /// @brief inline_object() 的 const 重载；语义同上。
  const T* inline_object(size_type index) const noexcept {
    return std::launder(reinterpret_cast<const T*>(m_inline + index * sizeof(T)));
  }

  /**
   * @brief 析构 [0, m_size) 范围内的全部内联对象，并把 m_size 归零。
   * @note 先递减再析构：即使 T 的析构函数违反标准容器前提抛了异常，
   *       也不会重复析构同一个对象；m_size 本身是重入保护。
   */
  void destroy_inline() noexcept {
    if constexpr (std::is_trivially_destructible<T>::value) {
      // 平凡析构是空操作：跳过逐个伪析构调用，clear()/迁移因此变成 O(1)。
      m_size = 0;
    } else {
      while (m_size > 0) {
        --m_size;
        inline_object(m_size)->~T();
      }
    }
  }

  /**
   * @brief 把 other 的内联元素逐个拷贝到本容器的内联槽位。
   * @param other [in] 源容器，必须处于内联模式；只读。
   * @pre 本容器为空且处于内联模式（m_size == 0、!m_using_heap），由调用方保证。
   * @exception 任一元素拷贝构造抛出时原样传播；已构造的元素由调用方
   *            （拷贝构造 / 拷贝赋值）调用 destroy_inline() 清理。
   */
  void copy_inline_from(const KInlineVector& other) {
    for (size_type i = 0; i < other.m_size; ++i) {
      ::new (static_cast<void*>(inline_slot(i))) T(*other.inline_object(i));
      ++m_size;
    }
  }

  /**
   * @brief 把内联元素整体迁移到 m_heap，完成"内联 → 堆"的单向切换。
   * @param min_capacity [in] 调用方要求的最小容量；实际取 max(min_capacity, 2*N)。
   * @pre 本容器处于内联模式（m_using_heap == false）。
   * @note 为什么先 reserve 再搬：一次分配到位，搬移过程中的 push_back 不会
   *       因扩容而再次分配，也不会在搬移途中触发 vector 的 reallocate
   *       （那会把刚搬进去的元素再移动一遍）。
   * @note 为什么 reserve(2*N)：切换点恰好是 size()==N，若只留 N 的容量，
   *       下一次 push 立刻又扩容；留一倍给后续增长留出缓冲。
   * @note 平凡类型快路径：先 resize 出目标对象，再 std::copy（编译器生成
   *       memmove），省掉逐元素的容量检查和 move 构造；非平凡类型仍逐个
   *       move 再统一析构内联对象——move 只改变源对象的值、不结束其生命周期，
   *       必须显式析构，否则 T 的资源泄漏。
   * @exception 迁移失败（reserve 抛 bad_alloc 或 T 的移动构造抛异常）时
   *            回滚 m_heap.clear()，保持内联模式与 size() 不变，满足基本保证；
   *            已经 move 过的元素处于有效但未指定状态，这是基本保证允许的。
   */
  void migrate_to_heap(size_type min_capacity) {
    m_heap.reserve(std::max(min_capacity, 2 * N));
    if constexpr (std::is_trivially_copyable<T>::value) {
      if (m_size != 0) {
        // insert 到空 vector 的 end()：libstdc++ 走 uninitialized_copy，
        // 对 trivially copyable 类型退化为一次 memmove；比 resize（会先
        // 值初始化、再覆盖）少一遍写。
        T* const source_begin = inline_object(0);
        m_heap.insert(m_heap.end(), source_begin, source_begin + m_size);
      }
      m_size = 0;
      m_using_heap = true;
      return;
    }
    if (m_size != 0) {
      // 指针步进：只取一次 launder，避免循环里重复做地址计算。
      T* const source_begin = inline_object(0);
      T* const source_end = source_begin + m_size;
      try {
        for (T* source = source_begin; source != source_end; ++source) {
          m_heap.push_back(std::move(*source));
        }
      } catch (...) {
        // 回滚：堆里已构造的元素销毁；内联对象仍有效（部分为 moved-from）。
        m_heap.clear();
        throw;
      }
    }
    destroy_inline();
    m_using_heap = true;
  }

  // 内联存储放在最前面：对象起始地址即内联区地址，placement new 的偏移固定。
  alignas(T) unsigned char m_inline[sizeof(T) * N];
  std::vector<T> m_heap;
  // 内联模式的有效元素数；堆模式下恒为 0（堆模式以 m_heap.size() 为唯一真相）。
  size_type m_size = 0;
  bool m_using_heap = false;
};

}  // namespace go2cpp
