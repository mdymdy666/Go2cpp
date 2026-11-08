/**
 * @file test_k_inline_stack.cpp
 * @brief KInlineStack 回归测试：栈语义、零分配、非平凡类型、深嵌套模拟。
 */
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <stack>
#include <string>
#include <utility>

#include "go2cpp/json/kstd/k_inline_stack.hpp"

namespace {

int g_failures = 0;

#define CHECK(condition)                                                             \
  do {                                                                               \
    if (!(condition)) {                                                              \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);      \
      ++g_failures;                                                                  \
    }                                                                                \
  } while (false)

std::atomic<std::size_t> g_new_calls{0};

/// @brief 构造/析构计数类型，用于验证无泄漏。
struct Tracked {
  static int live;
  static int constructed;
  static int destroyed;

  int value;

  explicit Tracked(int v = 0) : value(v) {
    ++live;
    ++constructed;
  }
  Tracked(const Tracked& other) : value(other.value) {
    ++live;
    ++constructed;
  }
  Tracked(Tracked&& other) noexcept : value(other.value) {
    ++live;
    ++constructed;
  }
  Tracked& operator=(const Tracked&) = default;
  Tracked& operator=(Tracked&&) = default;
  ~Tracked() {
    --live;
    ++destroyed;
  }

  static void Reset() {
    live = 0;
    constructed = 0;
    destroyed = 0;
  }
};

int Tracked::live = 0;
int Tracked::constructed = 0;
int Tracked::destroyed = 0;

/// @brief 1. 零分配证据：深度 ≤ N 不分配，超过 N 才分配。
void TestZeroAllocation() {
  std::printf("\n[1] 零分配证据...\n");
  const std::size_t base = g_new_calls.load(std::memory_order_relaxed);
  {
    extened::KInlineStack<int, 8> stack;
    for (int i = 0; i < 8; ++i) {
      stack.push(i);
    }
    CHECK(stack.size() == 8u);
    CHECK(!stack.using_heap());
  }
  const std::size_t inline_allocs =
      g_new_calls.load(std::memory_order_relaxed) - base;
  std::printf("  深度 8（N=8）分配次数: %zu\n", inline_allocs);
  CHECK(inline_allocs == 0u);

  extened::KInlineStack<int, 8> stack;
  for (int i = 0; i < 8; ++i) {
    stack.push(i);
  }
  const std::size_t before = g_new_calls.load(std::memory_order_relaxed);
  stack.push(8);  // 第 9 个触发迁移
  const std::size_t overflow_allocs = g_new_calls.load(std::memory_order_relaxed) - before;
  std::printf("  第 9 个元素后分配次数: %zu\n", overflow_allocs);
  CHECK(overflow_allocs > 0u);
  CHECK(stack.using_heap());
  CHECK(stack.size() == 9u);
}

/// @brief 2. LIFO 顺序。
void TestLifoOrder() {
  std::printf("\n[2] LIFO 顺序...\n");
  extened::KInlineStack<std::string, 4> stack;
  stack.push("a");
  stack.push("b");
  stack.push("c");
  CHECK(stack.size() == 3u);
  CHECK(stack.top() == "c");
  stack.pop();
  CHECK(stack.top() == "b");
  stack.pop();
  CHECK(stack.top() == "a");
  stack.pop();
  CHECK(stack.empty());
}

/// @brief 3. 非平凡类型构造/析构配平。
void TestNonTrivial() {
  std::printf("\n[3] 非平凡类型...\n");
  Tracked::Reset();
  {
    extened::KInlineStack<Tracked, 3> stack;
    for (int i = 0; i < 6; ++i) {
      stack.push(Tracked(i));
    }
    CHECK(stack.size() == 6u);
    CHECK(stack.using_heap());
    while (!stack.empty()) {
      stack.pop();
    }
  }
  std::printf("  Tracked: 构造 %d 次, 析构 %d 次, 存活 %d\n", Tracked::constructed,
              Tracked::destroyed, Tracked::live);
  CHECK(Tracked::constructed == Tracked::destroyed);
  CHECK(Tracked::live == 0);
}

/// @brief 4. 拷贝/移动/赋值。
void TestCopyMove() {
  std::printf("\n[4] 拷贝 / 移动...\n");
  extened::KInlineStack<int, 3> source;
  for (int i = 0; i < 5; ++i) {
    source.push(i);
  }

  extened::KInlineStack<int, 3> copied(source);
  CHECK(copied.size() == source.size());
  CHECK(copied.top() == source.top());
  copied.pop();
  CHECK(source.size() == 5u);  // 拷贝独立

  extened::KInlineStack<int, 3> moved(std::move(source));
  CHECK(moved.size() == 5u);
  CHECK(source.empty());

  extened::KInlineStack<int, 3> assigned;
  assigned = moved;
  CHECK(assigned.size() == moved.size());

  extened::KInlineStack<int, 3> move_assigned;
  move_assigned = std::move(assigned);
  CHECK(move_assigned.size() == 5u);
}

/// @brief 5. try_pop 的空栈行为。
void TestTryPop() {
  std::printf("\n[5] try_pop...\n");
  extened::KInlineStack<int, 2> stack;
  int out = -1;
  CHECK(!stack.try_pop(out));
  CHECK(out == -1);  // 失败不修改 out

  stack.push(7);
  stack.push(8);
  CHECK(stack.try_pop(out));
  CHECK(out == 8);
  CHECK(stack.try_pop(out));
  CHECK(out == 7);
  CHECK(stack.empty());
}

/// @brief 6. 边界：N=1、clear 后复用、reserve。
void TestBoundary() {
  std::printf("\n[6] 边界条件...\n");
  extened::KInlineStack<int, 1> stack;
  stack.push(1);
  CHECK(!stack.using_heap());
  stack.push(2);
  CHECK(stack.using_heap());
  CHECK(stack.top() == 2);

  stack.clear();
  CHECK(stack.empty());
  stack.push(3);
  CHECK(stack.top() == 3);

  stack.reserve(16);
  CHECK(stack.capacity() >= 16u);
}

/// @brief 7. 与 std::stack 随机对照。
void TestAgainstStdStack() {
  std::printf("\n[7] 随机对照 std::stack...\n");
  extened::KInlineStack<int, 4> ours;
  std::stack<int> model;
  unsigned seed = 20261106u;
  std::size_t mismatches = 0;

  for (int step = 0; step < 10000; ++step) {
    seed = seed * 1103515245u + 12345u;
    const int roll = static_cast<int>((seed >> 16) % 10);
    if (roll < 6) {
      ours.push(step);
      model.push(step);
    } else if (roll < 9) {
      if (!model.empty()) {
        if (ours.top() != model.top()) {
          ++mismatches;
        }
        ours.pop();
        model.pop();
      }
    } else {
      ours.clear();
      while (!model.empty()) {
        model.pop();
      }
    }
    if (ours.size() != model.size()) {
      ++mismatches;
    }
  }
  std::printf("  10000 步后 size=%zu，不一致次数=%zu\n", ours.size(), mismatches);
  CHECK(mismatches == 0u);
}

/// @brief 8. 模拟迭代解析：显式栈保存 1000 层嵌套状态，不爆栈。
void TestDeepNesting() {
  std::printf("\n[8] 深嵌套模拟...\n");
  constexpr int kDepth = 1000;
  const std::size_t base = g_new_calls.load(std::memory_order_relaxed);
  extened::KInlineStack<int, 16> stack;
  for (int i = 0; i < kDepth; ++i) {
    stack.push(i);  // 模拟进入一层嵌套
  }
  const std::size_t allocs = g_new_calls.load(std::memory_order_relaxed) - base;

  int expected = kDepth - 1;
  int mismatches = 0;
  while (!stack.empty()) {
    if (stack.top() != expected) {
      ++mismatches;
    }
    stack.pop();
    --expected;
  }
  std::printf("  深度 %d：分配次数=%zu，顺序不一致=%d\n", kDepth, allocs, mismatches);
  CHECK(mismatches == 0);
  CHECK(expected == -1);
  // 1000 层只应发生 vector 扩容的少数几次分配（迁移 1 次 + 扩容 ~6 次）
  CHECK(allocs <= 12u);
}

}  // namespace

void* operator new(std::size_t size) {
  g_new_calls.fetch_add(1, std::memory_order_relaxed);
  void* pointer = std::malloc(size == 0 ? 1 : size);
  if (pointer == nullptr) {
    throw std::bad_alloc();
  }
  return pointer;
}

void* operator new[](std::size_t size) { return ::operator new(size); }

namespace {
#if defined(__GNUC__)
/// @brief 用 noinline 包住 free：避免 GCC 把 operator new/delete 内联后
///        误报 -Wmismatched-new-delete（本来就是 malloc/free 配对替换）。
__attribute__((noinline)) void FreeRaw(void* pointer) noexcept { std::free(pointer); }
#else
void FreeRaw(void* pointer) noexcept { std::free(pointer); }
#endif
}  // namespace

void operator delete(void* pointer) noexcept { FreeRaw(pointer); }
void operator delete[](void* pointer) noexcept { FreeRaw(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { FreeRaw(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { FreeRaw(pointer); }

int main() {
  std::printf("=== KInlineStack 测试开始 ===\n");
  TestZeroAllocation();
  TestLifoOrder();
  TestNonTrivial();
  TestCopyMove();
  TestTryPop();
  TestBoundary();
  TestAgainstStdStack();
  TestDeepNesting();

  if (g_failures == 0) {
    std::printf("\nALL TESTS PASSED\n");
    return 0;
  }
  std::fprintf(stderr, "\n%d check(s) failed\n", g_failures);
  return 1;
}
