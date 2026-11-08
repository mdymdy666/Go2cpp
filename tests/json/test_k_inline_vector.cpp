/**
 * @file test_k_inline_vector.cpp
 * @brief KInlineVector 的自建断言测试（不依赖 gtest / Catch2）。
 * @author DSH (DeepSeek Harness)
 * @date 2026-11-06
 *
 * @details
 * 编译与运行：
 * @code
 *   g++ -std=c++17 -Wall -Wextra -Wpedantic -I include \
 *       tests/test_k_inline_vector.cpp -o /tmp/kvec_test && /tmp/kvec_test
 * @endcode
 *
 * 覆盖项：
 * 1. 零堆分配证据：全局重载 operator new/new[]，按差值统计分配次数；
 * 2. 值与顺序：push 0..9 后逐个比对，以及 front/back/operator[]/data/begin/end；
 * 3. 非平凡类型：std::string 与带构造/析构计数器的结构体；
 * 4. 拷贝 / 移动 / 赋值：值独立、源为空、自赋值、模式切换；
 * 5. 边界：N=1、pop_back 到空、clear 后复用、reserve(0/N/2N)；
 * 6. 异常安全：构造抛异常后 size 不变、无泄漏、容器仍可继续使用；
 * 7. 随机对照：5000 步随机操作与 std::vector 模型逐元素比对；
 * 8. 真实场景模拟：顶点批处理、短消息列表、配置分发、工厂返回、坏数据重试。
 *
 * 说明：第 1 项只统计"分配次数"，不统计字节数；测试进程内 iostream、
 * std::string 等自身也会分配，所以所有断言都取一段代码前后的差值，
 * 不做全局绝对值为 0 的假设。
 */

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <new>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "go2cpp/json/kstd/k_inline_vector.hpp"

// ---------------------------------------------------------------------------
// 全局分配计数：替换 operator new / new[] 及其配套 delete。
// 这是"零堆分配"测试的证据来源。替换函数必须定义在全局命名空间。
// ---------------------------------------------------------------------------
namespace {
std::atomic<std::size_t> g_new_calls{0};
std::atomic<std::size_t> g_new_array_calls{0};
}  // namespace

void* operator new(std::size_t size) {
  g_new_calls.fetch_add(1, std::memory_order_relaxed);
  // new 要求返回非空且唯一的指针，size==0 时也至少给 1 字节。
  void* ptr = std::malloc(size == 0 ? 1 : size);
  if (ptr == nullptr) {
    throw std::bad_alloc();
  }
  return ptr;
}

void* operator new[](std::size_t size) {
  g_new_array_calls.fetch_add(1, std::memory_order_relaxed);
  void* ptr = std::malloc(size == 0 ? 1 : size);
  if (ptr == nullptr) {
    throw std::bad_alloc();
  }
  return ptr;
}

void operator delete(void* ptr) noexcept { std::free(ptr); }
void operator delete[](void* ptr) noexcept { std::free(ptr); }
void operator delete(void* ptr, std::size_t) noexcept { std::free(ptr); }
void operator delete[](void* ptr, std::size_t) noexcept { std::free(ptr); }

// ---------------------------------------------------------------------------
// 自建断言：失败打印文件与行号，并累计失败数。
// ---------------------------------------------------------------------------
namespace {
std::size_t g_check_count = 0;
std::size_t g_failure_count = 0;
}  // namespace

#define CHECK(condition)                                                        \
  do {                                                                          \
    ++g_check_count;                                                            \
    if (!(condition)) {                                                         \
      std::cerr << "  [FAILED] " << __FILE__ << ":" << __LINE__                 \
                << "  CHECK(" #condition ")\n";                                 \
      ++g_failure_count;                                                        \
    }                                                                           \
  } while (false)

// ---------------------------------------------------------------------------
// 测试用类型
// ---------------------------------------------------------------------------

/// @brief 统计构造 / 析构次数的类型，用于验证无泄漏、无重复析构。
struct Tracked {
  static int construct_count;
  static int destruct_count;
  static int live_count;

  int value = 0;

  Tracked() { AccountConstruct(); }
  explicit Tracked(int v) : value(v) { AccountConstruct(); }
  Tracked(const Tracked& other) : value(other.value) { AccountConstruct(); }
  Tracked(Tracked&& other) noexcept : value(other.value) { AccountConstruct(); }
  Tracked& operator=(const Tracked&) = default;
  Tracked& operator=(Tracked&&) = default;
  ~Tracked() {
    ++destruct_count;
    --live_count;
  }

  static void Reset() {
    construct_count = 0;
    destruct_count = 0;
    live_count = 0;
  }

 private:
  static void AccountConstruct() {
    ++construct_count;
    ++live_count;
  }
};

int Tracked::construct_count = 0;
int Tracked::destruct_count = 0;
int Tracked::live_count = 0;

/// @brief 可在"值构造 / 拷贝构造"时注入异常的类型。
/// @note 移动构造刻意不注入异常：KInlineVector 的迁移路径使用 move，
///       必须保证迁移本身不会抛，才能验证"迁移完成后 emplace 抛异常"
///       这一条独立的失败路径。
struct ThrowingValue {
  static int construct_count;
  static int destruct_count;
  static int live_count;
  static bool throw_enabled;
  static int constructions_before_throw;

  int value = 0;

  explicit ThrowingValue(int v) : value(v) { Init(); }
  ThrowingValue(const ThrowingValue& other) : value(other.value) { Init(); }
  ThrowingValue(ThrowingValue&& other) noexcept : value(other.value) {
    ++construct_count;
    ++live_count;
  }
  ~ThrowingValue() {
    ++destruct_count;
    --live_count;
  }

  static void Reset() {
    construct_count = 0;
    destruct_count = 0;
    live_count = 0;
    throw_enabled = false;
    constructions_before_throw = 0;
  }

 private:
  void Init() {
    if (throw_enabled) {
      if (constructions_before_throw == 0) {
        throw std::runtime_error("ThrowingValue 构造失败（测试注入）");
      }
      --constructions_before_throw;
    }
    ++construct_count;
    ++live_count;
  }
};

int ThrowingValue::construct_count = 0;
int ThrowingValue::destruct_count = 0;
int ThrowingValue::live_count = 0;
bool ThrowingValue::throw_enabled = false;
int ThrowingValue::constructions_before_throw = 0;

/// @brief 真实场景用的小对象（8 个顶点正好塞满内联容量）。
struct Point {
  double x = 0.0;
  double y = 0.0;
};

// ---------------------------------------------------------------------------
// 各测试用例
// ---------------------------------------------------------------------------

/// @brief 1. 零堆分配证据：内联阶段分配次数为 0，溢出后大于 0。
void TestZeroAllocation() {
  std::cout << "\n[1] 零堆分配证据...\n";
  // 先输出一次，让 iostream 自身的缓冲分配在基线采样之前完成。
  std::cout << "  基线分配计数: " << g_new_calls.load(std::memory_order_relaxed) << "\n";

  const std::size_t before_inline = g_new_calls.load(std::memory_order_relaxed);
  {
    go2cpp::KInlineVector<int, 4> vec;
    CHECK(vec.empty());
    CHECK(vec.size() == 0u);
    CHECK(vec.capacity() == 4u);
    CHECK(!vec.using_heap());
    for (int i = 0; i < 4; ++i) {
      vec.push_back(i);
    }
    CHECK(vec.size() == 4u);
    CHECK(vec.capacity() == 4u);
    CHECK(!vec.using_heap());
    CHECK(vec[0] == 0);
    CHECK(vec[3] == 3);
  }
  const std::size_t inline_allocations =
      g_new_calls.load(std::memory_order_relaxed) - before_inline;
  std::cout << "  内联 push 4 个 int 的堆分配次数: " << inline_allocations << "\n";
  CHECK(inline_allocations == 0u);

  const std::size_t before_growth = g_new_calls.load(std::memory_order_relaxed);
  go2cpp::KInlineVector<int, 4> vec;
  for (int i = 0; i < 4; ++i) {
    vec.push_back(i);
  }
  CHECK(!vec.using_heap());
  vec.push_back(4);
  const std::size_t growth_allocations =
      g_new_calls.load(std::memory_order_relaxed) - before_growth;
  std::cout << "  push 第 5 个 int 后的堆分配次数: " << growth_allocations << "\n";
  CHECK(vec.size() == 5u);
  CHECK(vec.using_heap());
  CHECK(vec.capacity() >= 8u);
  CHECK(growth_allocations > 0u);
  CHECK(vec[4] == 4);
}

/// @brief 2. 值与顺序：逐个元素比对，并校验各访问器彼此一致。
void TestValueAndOrder() {
  std::cout << "\n[2] 值、顺序与访问器...\n";
  go2cpp::KInlineVector<int, 4> vec;
  for (int i = 0; i < 10; ++i) {
    vec.push_back(i);
  }
  CHECK(vec.size() == 10u);
  CHECK(vec.capacity() >= 10u);
  CHECK(vec.using_heap());

  for (std::size_t i = 0; i < vec.size(); ++i) {
    CHECK(vec[i] == static_cast<int>(i));
  }
  CHECK(vec.front() == 0);
  CHECK(vec.back() == 9);
  CHECK(vec.data() == &vec[0]);
  CHECK(vec.begin() == vec.data());
  CHECK(vec.end() == vec.data() + vec.size());
  CHECK(static_cast<std::size_t>(vec.end() - vec.begin()) == vec.size());

  const go2cpp::KInlineVector<int, 4>& const_ref = vec;
  CHECK(const_ref.data() == vec.data());
  CHECK(const_ref.begin() == vec.begin());
  CHECK(const_ref.end() == vec.end());
  CHECK(const_ref[5] == 5);
  CHECK(const_ref.front() == 0);
  CHECK(const_ref.back() == 9);

  int expected = 0;
  for (int value : vec) {
    CHECK(value == expected);
    ++expected;
  }
  CHECK(expected == 10);

  // 内联模式下 data / begin / end 也必须正确。
  go2cpp::KInlineVector<int, 4> inline_vec;
  inline_vec.push_back(11);
  inline_vec.push_back(12);
  CHECK(inline_vec.begin() == inline_vec.data());
  CHECK(inline_vec.end() == inline_vec.data() + 2);
  CHECK(inline_vec[0] == 11);
  CHECK(*(inline_vec.begin() + 1) == 12);
}

/// @brief 3. 非平凡类型：std::string 与构造 / 析构计数。
void TestNonTrivialTypes() {
  std::cout << "\n[3] 非平凡类型...\n";

  go2cpp::KInlineVector<std::string, 2> words;
  words.push_back("alpha");
  words.push_back("beta");
  CHECK(words.size() == 2u);
  CHECK(!words.using_heap());
  CHECK(words[0] == "alpha");
  CHECK(words[1] == "beta");
  words.push_back("gamma");
  CHECK(words.size() == 3u);
  CHECK(words.using_heap());
  words.emplace_back(5, 'x');
  CHECK(words.size() == 4u);
  CHECK(words.front() == "alpha");
  CHECK(words.back() == "xxxxx");
  CHECK(words[2] == "gamma");

  Tracked::Reset();
  {
    go2cpp::KInlineVector<Tracked, 2> tracked;
    for (int i = 0; i < 5; ++i) {
      tracked.emplace_back(i);
    }
    CHECK(tracked.size() == 5u);
    CHECK(tracked.using_heap());
    for (std::size_t i = 0; i < tracked.size(); ++i) {
      CHECK(tracked[i].value == static_cast<int>(i));
    }
    tracked.pop_back();
    CHECK(tracked.size() == 4u);
    tracked.clear();
    CHECK(tracked.empty());
    Tracked& emplaced = tracked.emplace_back(99);
    CHECK(tracked.size() == 1u);
    CHECK(tracked[0].value == 99);
    CHECK(&emplaced == &tracked[0]);
  }
  CHECK(Tracked::live_count == 0);
  CHECK(Tracked::construct_count == Tracked::destruct_count);
  std::cout << "  Tracked: 构造 " << Tracked::construct_count << " 次, 析构 "
            << Tracked::destruct_count << " 次, 存活 " << Tracked::live_count << "\n";
}

/// @brief 4. 拷贝 / 移动 / 赋值：值独立、源为空、自赋值、模式切换。
void TestCopyMoveAssign() {
  std::cout << "\n[4] 拷贝 / 移动 / 赋值...\n";

  // 空容器拷贝 / 移动：不应分配，也不应改变模式。
  go2cpp::KInlineVector<int, 4> empty_src;
  go2cpp::KInlineVector<int, 4> empty_copy(empty_src);
  CHECK(empty_copy.empty());
  CHECK(!empty_copy.using_heap());
  go2cpp::KInlineVector<int, 4> empty_move(std::move(empty_src));
  CHECK(empty_move.empty());
  CHECK(empty_src.empty());

  // 内联拷贝：副本修改不影响源。
  go2cpp::KInlineVector<int, 4> a;
  a.push_back(1);
  a.push_back(2);
  go2cpp::KInlineVector<int, 4> b(a);
  CHECK(b.size() == 2u);
  CHECK(!b.using_heap());
  CHECK(b[0] == 1);
  CHECK(b[1] == 2);
  b[0] = 100;
  CHECK(a[0] == 1);

  // 堆拷贝。
  go2cpp::KInlineVector<int, 4> big;
  for (int i = 0; i < 6; ++i) {
    big.push_back(i);
  }
  CHECK(big.using_heap());
  go2cpp::KInlineVector<int, 4> big_copy(big);
  CHECK(big_copy.using_heap());
  CHECK(big_copy.size() == 6u);
  for (std::size_t i = 0; i < big_copy.size(); ++i) {
    CHECK(big_copy[i] == static_cast<int>(i));
  }
  big_copy[5] = 500;
  CHECK(big[5] == 5);

  // 内联移动：源变为空。
  go2cpp::KInlineVector<int, 4> move_source;
  move_source.push_back(7);
  move_source.push_back(8);
  go2cpp::KInlineVector<int, 4> move_target(std::move(move_source));
  CHECK(move_target.size() == 2u);
  CHECK(!move_target.using_heap());
  CHECK(move_target[1] == 8);
  CHECK(move_source.empty());
  CHECK(move_source.size() == 0u);

  // 堆移动：源变为空。
  go2cpp::KInlineVector<int, 4> heap_source;
  for (int i = 0; i < 6; ++i) {
    heap_source.push_back(i);
  }
  go2cpp::KInlineVector<int, 4> heap_target(std::move(heap_source));
  CHECK(heap_target.size() == 6u);
  CHECK(heap_target.using_heap());
  CHECK(heap_target[5] == 5);
  CHECK(heap_source.empty());
  CHECK(heap_source.size() == 0u);

  // 拷贝赋值：堆模式 ← 内联模式，赋值后模式跟随右操作数（回退内联）。
  go2cpp::KInlineVector<int, 4> assign_target;
  for (int i = 0; i < 6; ++i) {
    assign_target.push_back(i);
  }
  CHECK(assign_target.using_heap());
  assign_target = a;
  CHECK(!assign_target.using_heap());
  CHECK(assign_target.size() == a.size());
  CHECK(assign_target[0] == 1);
  CHECK(assign_target[1] == 2);

  // 拷贝赋值：内联模式 ← 堆模式。
  go2cpp::KInlineVector<int, 4> assign_small;
  assign_small.push_back(42);
  assign_small = big;
  CHECK(assign_small.using_heap());
  CHECK(assign_small.size() == 6u);
  CHECK(assign_small[0] == 0);
  CHECK(assign_small[5] == 5);

  // 自赋值：通过指针绕开编译器的自赋值告警。
  go2cpp::KInlineVector<int, 4>* self = &assign_small;
  assign_small = *self;
  CHECK(assign_small.size() == 6u);
  CHECK(assign_small[5] == 5);

  // 移动赋值：内联 ← 内联。
  go2cpp::KInlineVector<int, 4> move_assign_target;
  move_assign_target.push_back(1);
  move_assign_target = std::move(b);
  CHECK(move_assign_target.size() == 2u);
  CHECK(move_assign_target[0] == 100);
  CHECK(move_assign_target[1] == 2);
  CHECK(b.empty());

  // 移动赋值：堆 ← 堆。
  go2cpp::KInlineVector<int, 4> heap_assign_target;
  heap_assign_target.push_back(0);
  heap_assign_target = std::move(big);
  CHECK(heap_assign_target.size() == 6u);
  CHECK(heap_assign_target.using_heap());
  CHECK(heap_assign_target[5] == 5);
  CHECK(big.empty());
}

/// @brief 5. 边界：N=1、pop 到空、clear 复用、reserve 的三种输入。
void TestBoundary() {
  std::cout << "\n[5] 边界条件...\n";

  // N=1：第 2 个元素触发迁移。
  go2cpp::KInlineVector<int, 1> one;
  CHECK(one.capacity() == 1u);
  one.push_back(10);
  CHECK(!one.using_heap());
  CHECK(one.size() == 1u);
  one.push_back(11);
  CHECK(one.using_heap());
  CHECK(one.size() == 2u);
  CHECK(one.capacity() >= 2u);
  CHECK(one[0] == 10);
  CHECK(one[1] == 11);

  // pop_back 到空（内联模式）。
  go2cpp::KInlineVector<int, 2> inline_vec;
  inline_vec.push_back(1);
  inline_vec.push_back(2);
  inline_vec.pop_back();
  inline_vec.pop_back();
  CHECK(inline_vec.empty());
  CHECK(inline_vec.size() == 0u);
  CHECK(!inline_vec.using_heap());

  // pop_back 到空（堆模式）：模式不回退，容量保留。
  go2cpp::KInlineVector<int, 2> heap_vec;
  for (int i = 0; i < 5; ++i) {
    heap_vec.push_back(i);
  }
  CHECK(heap_vec.using_heap());
  const std::size_t heap_capacity = heap_vec.capacity();
  while (!heap_vec.empty()) {
    heap_vec.pop_back();
  }
  CHECK(heap_vec.empty());
  CHECK(heap_vec.using_heap());
  CHECK(heap_vec.capacity() == heap_capacity);

  // clear 后继续 push（内联模式）：不做任何堆操作。
  go2cpp::KInlineVector<int, 4> inline_cleared;
  inline_cleared.push_back(1);
  inline_cleared.push_back(2);
  CHECK(!inline_cleared.using_heap());
  inline_cleared.clear();
  CHECK(inline_cleared.empty());
  CHECK(!inline_cleared.using_heap());
  inline_cleared.push_back(3);
  CHECK(inline_cleared.size() == 1u);
  CHECK(inline_cleared[0] == 3);

  // clear 后继续 push：堆模式保持，元素可正常写入。
  go2cpp::KInlineVector<int, 2> cleared;
  cleared.push_back(1);
  cleared.push_back(2);
  cleared.push_back(3);
  CHECK(cleared.using_heap());
  cleared.clear();
  CHECK(cleared.empty());
  CHECK(cleared.using_heap());
  cleared.push_back(9);
  CHECK(cleared.size() == 1u);
  CHECK(cleared[0] == 9);

  // reserve(0) / reserve(N) 在内联模式下无操作。
  go2cpp::KInlineVector<int, 4> r;
  r.reserve(0);
  CHECK(!r.using_heap());
  CHECK(r.capacity() == 4u);
  r.reserve(4);
  CHECK(!r.using_heap());
  CHECK(r.capacity() == 4u);

  // reserve(2*N) 激活堆模式，且不改变已有元素。
  r.push_back(5);
  r.reserve(8);
  CHECK(r.using_heap());
  CHECK(r.capacity() >= 8u);
  CHECK(r.size() == 1u);
  CHECK(r[0] == 5);

  // reserve 小于当前 size 时由 std::vector 处理（不缩容、不丢元素）。
  r.reserve(0);
  CHECK(r.size() == 1u);
  CHECK(r[0] == 5);
}

/// @brief 6. 异常安全：构造抛异常后 size 不变、无泄漏、后续可继续 push。
void TestExceptionSafety() {
  std::cout << "\n[6] 异常安全...\n";

  // 情况 A：内联未满时元素构造抛异常。
  ThrowingValue::Reset();
  {
    go2cpp::KInlineVector<ThrowingValue, 4> vec;
    vec.emplace_back(1);
    vec.emplace_back(2);
    CHECK(vec.size() == 2u);
    CHECK(ThrowingValue::live_count == 2);

    ThrowingValue::throw_enabled = true;
    ThrowingValue::constructions_before_throw = 0;
    bool caught = false;
    try {
      vec.emplace_back(3);
    } catch (const std::runtime_error&) {
      caught = true;
    }
    CHECK(caught);
    CHECK(vec.size() == 2u);
    CHECK(!vec.using_heap());
    CHECK(ThrowingValue::live_count == 2);
    CHECK(vec[0].value == 1);
    CHECK(vec[1].value == 2);

    // 关闭注入后容器仍可正常追加。
    ThrowingValue::throw_enabled = false;
    vec.emplace_back(3);
    CHECK(vec.size() == 3u);
    CHECK(vec[2].value == 3);
  }
  CHECK(ThrowingValue::live_count == 0);
  CHECK(ThrowingValue::construct_count == ThrowingValue::destruct_count);

  // 情况 B：内联已满，构造临时元素时抛异常（迁移尚未发生）。
  ThrowingValue::Reset();
  {
    go2cpp::KInlineVector<ThrowingValue, 2> vec;
    vec.emplace_back(1);
    vec.emplace_back(2);
    CHECK(vec.size() == 2u);
    CHECK(!vec.using_heap());

    ThrowingValue::throw_enabled = true;
    ThrowingValue::constructions_before_throw = 0;
    bool caught = false;
    try {
      vec.emplace_back(3);
    } catch (const std::runtime_error&) {
      caught = true;
    }
    CHECK(caught);
    // 临时元素在迁移之前构造，所以构造失败时容器仍是内联模式、没有额外分配。
    CHECK(!vec.using_heap());
    CHECK(vec.size() == 2u);  // size 不变
    CHECK(ThrowingValue::live_count == 2);
    CHECK(vec[0].value == 1);
    CHECK(vec[1].value == 2);

    ThrowingValue::throw_enabled = false;
    vec.emplace_back(3);
    CHECK(vec.size() == 3u);
    CHECK(vec[2].value == 3);
  }
  CHECK(ThrowingValue::live_count == 0);
  CHECK(ThrowingValue::construct_count == ThrowingValue::destruct_count);
}

/// @brief 6b. 自引用 push：参数引用本容器元素时，迁移后不能读到 moved-from 对象。
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
// GCC 13 在 -O1 下对 KInlineVector 迁移路径的误报：pending 先读内联元素、再迁移，
// 逻辑上元素已初始化；ASan/UBSan 已验证无真实 UB。
void TestSelfReferencePush() {
  std::cout << "\n[6b] 自引用 push_back...\n";

  // 内联已满 → 迁移路径：必须在迁移前读取 v[0]，否则拿到的是 moved-from 的串。
  go2cpp::KInlineVector<std::string, 2> full;
  full.push_back("aaa");
  full.push_back("bbb");
  full.push_back(full[0]);
  CHECK(full.size() == 3u);
  CHECK(full.using_heap());
  CHECK(full[0] == "aaa");
  CHECK(full[1] == "bbb");
  CHECK(full[2] == "aaa");

  // 内联未满：直接构造到新槽位，不涉及迁移。
  go2cpp::KInlineVector<std::string, 4> roomy;
  roomy.push_back("x");
  roomy.push_back(roomy[0]);
  CHECK(roomy.size() == 2u);
  CHECK(!roomy.using_heap());
  CHECK(roomy[1] == "x");

  // 堆模式：交给 std::vector，标准保证自引用安全。
  go2cpp::KInlineVector<std::string, 1> heap;
  heap.push_back("p");
  heap.push_back("q");  // 触发迁移
  heap.push_back(heap[0]);
  CHECK(heap.size() == 3u);
  CHECK(heap.using_heap());
  CHECK(heap[2] == "p");
}
#pragma GCC diagnostic pop

/// @brief 7. 随机对照：与 std::vector 模型逐元素比对。
void TestRandomModel() {
  std::cout << "\n[7] 随机对照测试...\n";
  std::mt19937 rng(20261106u);
  std::uniform_int_distribution<int> dice(0, 9);
  std::uniform_int_distribution<int> values(-1000, 1000);
  std::uniform_int_distribution<int> extras(0, 16);

  go2cpp::KInlineVector<int, 3> vec;
  std::vector<int> model;
  std::size_t mismatches = 0;

  for (int step = 0; step < 5000; ++step) {
    const int roll = dice(rng);
    if (roll < 5) {
      const int value = values(rng);
      vec.push_back(value);
      model.push_back(value);
    } else if (roll < 8) {
      if (!model.empty()) {
        vec.pop_back();
        model.pop_back();
      }
    } else if (roll < 9) {
      vec.clear();
      model.clear();
    } else {
      const std::size_t target =
          model.size() + static_cast<std::size_t>(extras(rng));
      vec.reserve(target);
      model.reserve(target);
    }

    if (vec.size() != model.size() ||
        !std::equal(model.begin(), model.end(), vec.begin())) {
      ++mismatches;
      if (mismatches <= 3) {
        std::cerr << "  随机步骤 " << step << " 与模型不一致\n";
      }
    }
  }
  CHECK(mismatches == 0u);
  CHECK(vec.size() == model.size());
  std::cout << "  5000 步随机 push/pop/clear/reserve 后元素数: " << vec.size()
            << "，不一致次数: " << mismatches << "\n";
}

/// @brief 工厂函数：验证返回值（NRVO 或移动）跨过内联容量的场景。
go2cpp::KInlineVector<int, 4> MakeSequence(int count) {
  go2cpp::KInlineVector<int, 4> result;
  for (int i = 0; i < count; ++i) {
    result.push_back(i);
  }
  return result;
}

/// @brief 移动构造可注入异常、析构可计数的类型，用于验证「迁移失败回滚」。
struct ThrowingMove {
  static int live_count;
  static int move_budget;  ///< 还能成功移动多少次；0 表示下一次 move 抛异常。

  int value = 0;

  explicit ThrowingMove(int v) : value(v) { ++live_count; }
  ThrowingMove(const ThrowingMove& other) : value(other.value) { ++live_count; }
  ThrowingMove(ThrowingMove&& other) : value(other.value) {
    if (move_budget == 0) {
      throw std::runtime_error("ThrowingMove 移动失败（测试注入）");
    }
    --move_budget;
    ++live_count;
  }
  ThrowingMove& operator=(const ThrowingMove&) = default;
  ThrowingMove& operator=(ThrowingMove&&) = default;
  ~ThrowingMove() { --live_count; }

  static void Reset() {
    live_count = 0;
    move_budget = 1000000;
  }
};

int ThrowingMove::live_count = 0;
int ThrowingMove::move_budget = 1000000;

/// @brief 32 字节对齐的类型，验证 alignas(T) 对每个内联槽位都成立。
struct alignas(32) AlignedBlob {
  int value = 0;
};

/// @brief 9. 迁移途中元素移动构造抛异常：回滚后容器仍内联、size 不变、无泄漏。
void TestMigrationException() {
  std::cout << "\n[9] 迁移异常回滚（基本保证）...\n";
  ThrowingMove::Reset();
  {
    go2cpp::KInlineVector<ThrowingMove, 2> vec;
    vec.emplace_back(1);
    vec.emplace_back(2);
    CHECK(vec.size() == 2u);
    CHECK(!vec.using_heap());
    CHECK(ThrowingMove::live_count == 2);

    // 第一个内联元素 move 进堆时就抛：migrate_to_heap 必须回滚。
    ThrowingMove::move_budget = 0;
    bool caught = false;
    try {
      vec.emplace_back(3);
    } catch (const std::runtime_error&) {
      caught = true;
    }
    CHECK(caught);
    CHECK(vec.size() == 2u);               // size 不变
    CHECK(!vec.using_heap());              // 仍是内联模式
    CHECK(ThrowingMove::live_count == 2);  // 临时对象与堆上元素都销毁，无泄漏

    // 关闭注入后容器仍可正常迁移并继续使用。
    ThrowingMove::move_budget = 1000000;
    vec.emplace_back(3);
    CHECK(vec.size() == 3u);
    CHECK(vec.using_heap());
    CHECK(vec[0].value == 1);
    CHECK(vec[1].value == 2);
    CHECK(vec[2].value == 3);
  }
  CHECK(ThrowingMove::live_count == 0);
}

/// @brief 10. trivially copyable 快路径：迁移、自引用、拷贝、移动、clear/pop 重用。
void TestTrivialFastPath() {
  std::cout << "\n[10] trivial 迁移 / 自引用 / 拷贝移动...\n";

  // 迁移 8 个 Point（N=4），逐元素校验 memmove 路径的值与顺序。
  go2cpp::KInlineVector<Point, 4> polygon;
  for (int i = 0; i < 8; ++i) {
    polygon.push_back(Point{static_cast<double>(i), static_cast<double>(i) * 2.0});
  }
  CHECK(polygon.size() == 8u);
  CHECK(polygon.using_heap());
  for (std::size_t i = 0; i < polygon.size(); ++i) {
    CHECK(polygon[i].x == static_cast<double>(i));
    CHECK(polygon[i].y == static_cast<double>(i) * 2.0);
  }

  // int 自引用 push：迁移必须发生在参数被读取之前（临时对象路径）。
  go2cpp::KInlineVector<int, 4> ints;
  for (int i = 0; i < 4; ++i) {
    ints.push_back(i * 10);
  }
  ints.push_back(ints[0]);
  CHECK(ints.size() == 5u);
  CHECK(ints.using_heap());
  CHECK(ints[4] == 0);
  ints.emplace_back(ints[3]);
  CHECK(ints.size() == 6u);
  CHECK(ints[5] == 30);
  CHECK(ints[0] == 0);
  CHECK(ints[3] == 30);

  // Point 自引用 push。
  go2cpp::KInlineVector<Point, 2> points;
  points.push_back(Point{1.0, 2.0});
  points.push_back(Point{3.0, 4.0});
  points.push_back(points[0]);
  CHECK(points.size() == 3u);
  CHECK(points.using_heap());
  CHECK(points[2].x == 1.0);
  CHECK(points[2].y == 2.0);

  // 拷贝构造（trivial 走 uninitialized_copy）：值独立。
  go2cpp::KInlineVector<Point, 4> heap_copy = polygon;
  CHECK(heap_copy.size() == 8u);
  CHECK(heap_copy[7].y == 14.0);
  heap_copy[0].x = 99.0;
  CHECK(polygon[0].x == 0.0);

  // 移动构造（内联 uninitialized_copy 路径）：值接管、源为空。
  go2cpp::KInlineVector<Point, 4> small;
  small.push_back(Point{5.0, 6.0});
  small.push_back(Point{7.0, 8.0});
  go2cpp::KInlineVector<Point, 4> moved(std::move(small));
  CHECK(moved.size() == 2u);
  CHECK(moved[0].x == 5.0);
  CHECK(moved[1].y == 8.0);
  CHECK(small.empty());

  // clear / pop_back 后重用（trivial 跳过析构）。
  go2cpp::KInlineVector<int, 4> reuse;
  for (int i = 0; i < 4; ++i) {
    reuse.push_back(i);
  }
  reuse.clear();
  CHECK(reuse.empty());
  CHECK(!reuse.using_heap());
  for (int i = 0; i < 4; ++i) {
    reuse.push_back(i + 100);
  }
  CHECK(reuse.size() == 4u);
  CHECK(reuse[3] == 103);
  reuse.pop_back();
  reuse.pop_back();
  CHECK(reuse.size() == 2u);
  CHECK(reuse[1] == 101);
  reuse.push_back(200);
  CHECK(reuse.size() == 3u);
  CHECK(reuse[2] == 200);
}

/// @brief 11. 对齐：alignas(T) 保证每个内联槽位满足 T 的对齐要求。
void TestAlignment() {
  std::cout << "\n[11] 对齐与内联容量常量...\n";
  CHECK((go2cpp::KInlineVector<int, 4>::kInlineCapacity == 4u));
  go2cpp::KInlineVector<AlignedBlob, 3> blobs;
  for (int i = 0; i < 3; ++i) {
    blobs.push_back(AlignedBlob{i});
  }
  CHECK(blobs.size() == 3u);
  CHECK(!blobs.using_heap());
  CHECK(reinterpret_cast<std::uintptr_t>(blobs.data()) % alignof(AlignedBlob) == 0u);
  for (std::size_t i = 0; i < blobs.size(); ++i) {
    CHECK(reinterpret_cast<std::uintptr_t>(&blobs[i]) % alignof(AlignedBlob) == 0u);
  }
  // 迁移到堆后依然对齐。
  blobs.push_back(AlignedBlob{3});
  CHECK(blobs.using_heap());
  CHECK(reinterpret_cast<std::uintptr_t>(blobs.data()) % alignof(AlignedBlob) == 0u);
  CHECK(blobs[3].value == 3);
}

/// @brief 12. move-only 类型：迁移与容器移动都不得要求拷贝。
void TestMoveOnly() {
  std::cout << "\n[12] move-only 类型（unique_ptr）...\n";
  go2cpp::KInlineVector<std::unique_ptr<int>, 2> vec;
  vec.push_back(std::make_unique<int>(1));
  vec.push_back(std::make_unique<int>(2));
  CHECK(!vec.using_heap());
  CHECK(*vec[0] == 1);
  vec.push_back(std::make_unique<int>(3));  // 第 3 个触发迁移
  CHECK(vec.using_heap());
  CHECK(vec.size() == 3u);
  CHECK(*vec[0] == 1);
  CHECK(*vec[2] == 3);

  go2cpp::KInlineVector<std::unique_ptr<int>, 2> moved(std::move(vec));
  CHECK(moved.size() == 3u);
  CHECK(*moved[1] == 2);
  CHECK(vec.empty());
}

/// @brief 13. jobject 真实元素：kvp + N=5，验证 2/5 字段零分配、6 字段一次迁移。
void TestJobjectLikeKvp() {
  std::cout << "\n[13] jobject 场景（kvp、N=5）...\n";
  using kvp = std::pair<std::string, std::string>;

  // 2 字段：零分配、零迁移。
  std::size_t before = g_new_calls.load(std::memory_order_relaxed);
  {
    go2cpp::KInlineVector<kvp, 5> object;
    object.push_back(kvp("id", "1"));
    object.push_back(kvp("name", "demo"));
    CHECK(object.size() == 2u);
    CHECK(!object.using_heap());
    CHECK(object[0].first == "id");
    CHECK(object[1].second == "demo");
  }
  CHECK(g_new_calls.load(std::memory_order_relaxed) - before == 0u);

  // 5 字段：恰好塞满 N=5，仍然零分配。
  before = g_new_calls.load(std::memory_order_relaxed);
  {
    go2cpp::KInlineVector<kvp, 5> object;
    for (int i = 0; i < 5; ++i) {
      object.push_back(kvp("k" + std::to_string(i), "v" + std::to_string(i)));
    }
    CHECK(object.size() == 5u);
    CHECK(!object.using_heap());
    CHECK(object[4].first == "k4");
  }
  CHECK(g_new_calls.load(std::memory_order_relaxed) - before == 0u);

  // 6 字段：第 6 个触发迁移，恰好一次堆分配。
  before = g_new_calls.load(std::memory_order_relaxed);
  {
    go2cpp::KInlineVector<kvp, 5> object;
    for (int i = 0; i < 6; ++i) {
      object.push_back(kvp("k" + std::to_string(i), "v" + std::to_string(i)));
    }
    CHECK(object.size() == 6u);
    CHECK(object.using_heap());
    CHECK(object.capacity() >= 10u);
    CHECK(object[5].first == "k5");
  }
  CHECK(g_new_calls.load(std::memory_order_relaxed) - before == 1u);
}

/// @brief 14. 大 N 与 reserve：N=16 零分配、reserve(100) 一次迁移。
void TestLargeNAndReserve() {
  std::cout << "\n[14] 大 N 与 reserve...\n";
  go2cpp::KInlineVector<int, 16> big;
  const std::size_t before = g_new_calls.load(std::memory_order_relaxed);
  for (int i = 0; i < 16; ++i) {
    big.push_back(i);
  }
  CHECK(big.size() == 16u);
  CHECK(!big.using_heap());
  CHECK(big.capacity() == 16u);
  CHECK(g_new_calls.load(std::memory_order_relaxed) - before == 0u);

  const std::size_t before_reserve = g_new_calls.load(std::memory_order_relaxed);
  big.reserve(100);
  CHECK(big.using_heap());
  CHECK(big.capacity() >= 100u);
  CHECK(big.size() == 16u);
  CHECK(big[15] == 15);
  CHECK(g_new_calls.load(std::memory_order_relaxed) - before_reserve == 1u);
  CHECK(big.data() == &big[0]);
}

/// @brief 8. 真实场景模拟：5 个贴近实际用法的场景。
void TestRealWorldScenarios() {
  std::cout << "\n[8] 真实场景模拟...\n";

  // 场景 1：8 个顶点的小多边形，内联零分配。
  const std::size_t before_vertices = g_new_calls.load(std::memory_order_relaxed);
  {
    go2cpp::KInlineVector<Point, 8> polygon;
    for (int i = 0; i < 8; ++i) {
      polygon.push_back(Point{static_cast<double>(i), static_cast<double>(i) * 2.0});
    }
    CHECK(polygon.size() == 8u);
    CHECK(!polygon.using_heap());
    CHECK(polygon[3].x == 3.0);
    CHECK(polygon[3].y == 6.0);
    CHECK(polygon.back().x == 7.0);
  }
  const std::size_t vertex_allocations =
      g_new_calls.load(std::memory_order_relaxed) - before_vertices;
  std::cout << "  场景 1（8 顶点内联）分配次数: " << vertex_allocations << "\n";
  CHECK(vertex_allocations == 0u);

  // 场景 2：短消息列表，超过 4 条后转堆。
  go2cpp::KInlineVector<std::string, 4> inbox;
  for (int i = 0; i < 4; ++i) {
    inbox.emplace_back("msg-" + std::to_string(i));
  }
  CHECK(!inbox.using_heap());
  inbox.emplace_back("msg-4");
  CHECK(inbox.using_heap());
  CHECK(inbox.size() == 5u);
  CHECK(inbox.front() == "msg-0");
  CHECK(inbox.back() == "msg-4");

  // 场景 3：默认配置分发，拷贝后互不影响。
  go2cpp::KInlineVector<int, 8> defaults;
  for (int i = 0; i < 5; ++i) {
    defaults.push_back(i * 10);
  }
  go2cpp::KInlineVector<int, 8> profile_a = defaults;
  go2cpp::KInlineVector<int, 8> profile_b;
  profile_b = defaults;
  profile_a[0] = 999;
  CHECK(profile_a[0] == 999);
  CHECK(profile_b[0] == 0);
  CHECK(defaults[0] == 0);
  CHECK(profile_b[4] == 40);

  // 场景 4：工厂函数返回（NRVO 或移动），分别覆盖内联与堆两种结果。
  go2cpp::KInlineVector<int, 4> inline_seq = MakeSequence(3);
  CHECK(inline_seq.size() == 3u);
  CHECK(!inline_seq.using_heap());
  CHECK(inline_seq[2] == 2);
  go2cpp::KInlineVector<int, 4> heap_seq = MakeSequence(6);
  CHECK(heap_seq.size() == 6u);
  CHECK(heap_seq.using_heap());
  CHECK(heap_seq[5] == 5);

  // 场景 5：批量导入时跳过坏记录，异常后容器仍可用。
  ThrowingValue::Reset();
  {
    go2cpp::KInlineVector<ThrowingValue, 3> batch;
    int accepted = 0;
    for (int i = 0; i < 5; ++i) {
      ThrowingValue::throw_enabled = (i == 3);
      ThrowingValue::constructions_before_throw = 0;
      try {
        batch.emplace_back(i);
        ++accepted;
      } catch (const std::runtime_error&) {
        // 真实场景：坏记录被拒绝，继续处理下一条。
      }
    }
    ThrowingValue::throw_enabled = false;
    CHECK(accepted == 4);
    CHECK(batch.size() == 4u);
    CHECK(batch.using_heap());
    CHECK(batch[0].value == 0);
    CHECK(batch[3].value == 4);
  }
  CHECK(ThrowingValue::live_count == 0);
  CHECK(ThrowingValue::construct_count == ThrowingValue::destruct_count);
  std::cout << "  场景 5（异常跳过）已接受记录: 4 / 5\n";
}

int main() {
  std::cout << "=== KInlineVector 测试开始 ===\n";

  TestZeroAllocation();
  TestValueAndOrder();
  TestNonTrivialTypes();
  TestCopyMoveAssign();
  TestBoundary();
  TestExceptionSafety();
  TestSelfReferencePush();
  TestRandomModel();
  TestRealWorldScenarios();
  TestMigrationException();
  TestTrivialFastPath();
  TestAlignment();
  TestMoveOnly();
  TestJobjectLikeKvp();
  TestLargeNAndReserve();

  std::cout << "\n共执行 " << g_check_count << " 项断言，失败 " << g_failure_count
            << " 项\n";
  if (g_failure_count != 0) {
    std::cout << "TESTS FAILED\n";
    return 1;
  }
  std::cout << "ALL TESTS PASSED\n";
  return 0;
}
