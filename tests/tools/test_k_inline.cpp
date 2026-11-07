// go2cpp::KInlineVector / KInlineStack 单元测试（从 JSON 库迁移）
#include <cstdio>
#include <string>
#include <vector>

#include "go2cpp/tools/k_inline_stack.hpp"
#include "go2cpp/tools/k_inline_vector.hpp"

namespace {

int g_checks = 0;
int g_failures = 0;

#define CHECK(cond)                                              \
  do {                                                           \
    ++g_checks;                                                  \
    if (!(cond)) {                                               \
      ++g_failures;                                              \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    }                                                            \
  } while (0)

void TestInlineOnly() {
  go2cpp::KInlineVector<int, 4> v;
  CHECK(v.empty());
  CHECK(v.size() == 0);
  CHECK(v.capacity() >= 4);
  for (int i = 0; i < 4; ++i) v.push_back(i);
  CHECK(v.size() == 4);
  CHECK(!v.empty());
  for (int i = 0; i < 4; ++i) CHECK(v[i] == i);
  CHECK(v.data() != nullptr);
  v.pop_back();
  CHECK(v.size() == 3);
  CHECK(v.back() == 2);
}

void TestOverflowToHeap() {
  go2cpp::KInlineVector<int, 2> v;
  for (int i = 0; i < 100; ++i) v.push_back(i);
  CHECK(v.size() == 100);
  for (int i = 0; i < 100; ++i) CHECK(v[i] == i);
  // 迭代器遍历
  int sum = 0;
  for (auto it = v.begin(); it != v.end(); ++it) sum += *it;
  CHECK(sum == 4950);
}

void TestNonTrivialType() {
  go2cpp::KInlineVector<std::string, 4> v;
  v.push_back("a");
  v.push_back(std::string("bb"));
  v.emplace_back("ccc");
  CHECK(v.size() == 3);
  CHECK(v[0] == "a");
  CHECK(v[1] == "bb");
  CHECK(v[2] == "ccc");
}

void TestStack() {
  go2cpp::KInlineStack<int, 8> s;
  CHECK(s.empty());
  for (int i = 0; i < 8; ++i) s.push(i);
  CHECK(s.size() == 8);
  CHECK(s.top() == 7);
  for (int i = 7; i >= 0; --i) {
    CHECK(s.top() == i);
    s.pop();
  }
  CHECK(s.empty());
}

}  // namespace

int main() {
  TestInlineOnly();
  TestOverflowToHeap();
  TestNonTrivialType();
  TestStack();
  std::printf("KInline: %d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
