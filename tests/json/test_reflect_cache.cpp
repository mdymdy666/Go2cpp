/**
 * @file test_reflect_cache.cpp
 * @brief J3：`TypeRegistry` 适配层的线程本地查询缓存——命中、回填、轮转与一致性。
 *
 * 覆盖：
 * - 命中路径：同一名字连续查询返回同一 id（免锁路径）；
 * - 未命中路径：未注册名字返回 false 且写入的是 `kUnknownType`；
 * - 轮转：64 个名字 > 缓存槽数时两轮全量查询，结果逐个正确（不串味）；
 * - 各种长度：空名字、1/4/7/8/9/16/17/64 字节名字（首尾 8 字节比较的边界）；
 * - 幂等注册：同名重复注册返回同一 id，且注册后 `Find` 立即可用；
 * - 转发语义：`Name` / `Contains` / `Describe` / `Size` / `RegisterWithId`；
 * - 多线程：每个线程各自缓存，跨线程结果一致。
 */

#include <cstdio>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "go2cpp/reflect/type.hpp"

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(condition)                                                        \
  do {                                                                          \
    ++g_checks;                                                                 \
    if (!(condition)) {                                                         \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
      ++g_failures;                                                             \
    }                                                                           \
  } while (false)

using go2cpp::reflect::kUnknownType;
using go2cpp::reflect::TypeId;
using go2cpp::reflect::TypeInfo;
using go2cpp::reflect::TypeRegistry;

/// @brief 命中：连续 1000 次查询同一名字，id 恒定且有效。
void TestHit() {
  std::printf("\n[1] 命中路径...\n");
  auto& registry = TypeRegistry::Instance();
  const TypeId id = registry.Register("cachetest.hit", 8);
  CHECK(id != kUnknownType);

  TypeId first = kUnknownType;
  CHECK(registry.Find("cachetest.hit", first));
  CHECK(first == id);
  for (int i = 0; i < 1000; ++i) {
    TypeId current = kUnknownType;
    CHECK(registry.Find("cachetest.hit", current));
    CHECK(current == id);
  }
}

/// @brief 未命中：返回 false，`out` 保持调用方给的初值（接口约定：成功才写）。
void TestMiss() {
  std::printf("\n[2] 未命中路径...\n");
  auto& registry = TypeRegistry::Instance();
  TypeId out = kUnknownType;
  CHECK(!registry.Find("cachetest.not_registered", out));
  CHECK(out == kUnknownType);
  CHECK(!registry.Find("", out));
  // 未命中不进负缓存：注册后立刻能查到。
  const TypeId id = registry.Register("cachetest.not_registered", 16);
  CHECK(id != kUnknownType);
  CHECK(registry.Find("cachetest.not_registered", out));
  CHECK(out == id);
}

/// @brief 轮转：64 个名字（> 64 槽的一半）两轮全量查询，逐个正确。
void TestRotation() {
  std::printf("\n[3] 名字轮转（64 个名字 × 2 轮）...\n");
  auto& registry = TypeRegistry::Instance();
  constexpr int kNames = 64;
  std::vector<TypeId> ids(static_cast<std::size_t>(kNames), kUnknownType);
  for (int i = 0; i < kNames; ++i) {
    const std::string name = "cachetest.rotate." + std::to_string(i);
    ids[static_cast<std::size_t>(i)] = registry.Register(name, static_cast<std::size_t>(i) + 1);
    CHECK(ids[static_cast<std::size_t>(i)] != kUnknownType);
  }
  for (int round = 0; round < 2; ++round) {
    for (int i = 0; i < kNames; ++i) {
      const std::string name = "cachetest.rotate." + std::to_string(i);
      TypeId out = kUnknownType;
      CHECK(registry.Find(name, out));
      CHECK(out == ids[static_cast<std::size_t>(i)]);
    }
  }
}

/// @brief 名字长度边界：首尾 8 字节比较覆盖 8~16，其余走 memcmp。
void TestNameLengths() {
  std::printf("\n[4] 名字长度边界（0/1/4/7/8/9/16/17/64）...\n");
  auto& registry = TypeRegistry::Instance();
  for (const std::size_t length : {std::size_t{0}, std::size_t{1}, std::size_t{4},
                                   std::size_t{7}, std::size_t{8}, std::size_t{9},
                                   std::size_t{16}, std::size_t{17}, std::size_t{64}}) {
    std::string name = "L";
    name.assign(length, 'x');
    if (length > 0) {
      name[0] = 'L';
    }
    TypeId out = kUnknownType;
    if (length == 0) {
      // 空名字不是合法类型名：注册拒绝、查询也查不到（不进负缓存）。
      CHECK(registry.Register(name, 1) == kUnknownType);
      CHECK(!registry.Find(name, out));
      continue;
    }
    const TypeId id = registry.Register(name, length + 1);
    CHECK(id != kUnknownType);
    CHECK(registry.Find(name, out));
    CHECK(out == id);
    // 只差最后一个字节：必须查不到。
    std::string different = name;
    different.back() = different.back() == 'y' ? 'z' : 'y';
    TypeId missing = kUnknownType;
    CHECK(!registry.Find(different, missing));
  }
}

/// @brief 幂等注册：同名重复注册返回同一 id，且不改变查表结果。
void TestIdempotentRegister() {
  std::printf("\n[5] 幂等注册...\n");
  auto& registry = TypeRegistry::Instance();
  const TypeId first = registry.Register("cachetest.idempotent", 8);
  CHECK(first != kUnknownType);
  for (int i = 0; i < 100; ++i) {
    CHECK(registry.Register("cachetest.idempotent", 8) == first);
    CHECK(registry.Register("cachetest.idempotent", 999) == first);  // 尺寸被忽略
  }
  TypeId out = kUnknownType;
  CHECK(registry.Find("cachetest.idempotent", out));
  CHECK(out == first);
}

/// @brief 转发语义：Name / Contains / Describe / Size / RegisterWithId。
void TestForwarding() {
  std::printf("\n[6] 转发语义...\n");
  auto& registry = TypeRegistry::Instance();
  const TypeId id = registry.Register("cachetest.forward", 24);
  CHECK(id != kUnknownType);

  std::string_view name;
  CHECK(registry.Name(id, name));
  CHECK(name == "cachetest.forward");

  CHECK(registry.Contains(id));
  CHECK(!registry.Contains(kUnknownType));

  TypeInfo info;
  CHECK(registry.Describe(id, info));
  CHECK(info.name == "cachetest.forward");
  CHECK(info.size == 24);
  CHECK(!info.builtin);

  const std::size_t size_before = registry.Size();
  CHECK(size_before >= 8);  // 至少 8 个内置类型

  // RegisterWithId：固定 id 注册，随后 Find 必须命中同一 id。
  const TypeId fixed = registry.RegisterWithId(900, "cachetest.fixed", 4);
  CHECK(fixed == 900);
  TypeId out = kUnknownType;
  CHECK(registry.Find("cachetest.fixed", out));
  CHECK(out == fixed);
  std::string_view fixed_name;
  CHECK(registry.Name(900, fixed_name));
  CHECK(fixed_name == "cachetest.fixed");
}

/// @brief `RegisterType<T>` 走适配层（与 `Register` 结果一致）。
struct CacheProbe {
  int a;
  double b;
};

void TestRegisterTypeTemplate() {
  std::printf("\n[7] RegisterType<T>...\n");
  const TypeId id = go2cpp::reflect::RegisterType<CacheProbe>("cachetest.template");
  CHECK(id != kUnknownType);
  TypeId out = kUnknownType;
  CHECK(TypeRegistry::Instance().Find("cachetest.template", out));
  CHECK(out == id);
  TypeInfo info;
  CHECK(TypeRegistry::Instance().Describe(id, info));
  CHECK(info.size == sizeof(CacheProbe));
}

/// @brief 多线程：每个线程独立缓存，结果跨线程一致（注册表只增不删）。
void TestThreads() {
  std::printf("\n[8] 多线程一致性...\n");
  auto& registry = TypeRegistry::Instance();
  const TypeId id = registry.Register("cachetest.thread", 32);
  CHECK(id != kUnknownType);

  constexpr int kThreads = 4;
  constexpr int kIterations = 20000;
  std::vector<int> failures(static_cast<std::size_t>(kThreads), 0);
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([t, id, &failures]() {
      auto& local = TypeRegistry::Instance();
      for (int i = 0; i < kIterations; ++i) {
        TypeId out = kUnknownType;
        if (!local.Find("cachetest.thread", out) || out != id) {
          ++failures[static_cast<std::size_t>(t)];
          return;
        }
        // 中间穿插未命中，验证未命中不进负缓存。
        TypeId missing = kUnknownType;
        if (local.Find("cachetest.thread.missing", missing)) {
          ++failures[static_cast<std::size_t>(t)];
          return;
        }
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  for (int t = 0; t < kThreads; ++t) {
    CHECK(failures[static_cast<std::size_t>(t)] == 0);
  }
}

}  // namespace

int main() {
  TestHit();
  TestMiss();
  TestRotation();
  TestNameLengths();
  TestIdempotentRegister();
  TestForwarding();
  TestRegisterTypeTemplate();
  TestThreads();

  if (g_failures != 0) {
    std::fprintf(stderr, "\n%d FAILURES (%d checks)\n", g_failures, g_checks);
    return 1;
  }
  std::printf("\nALL TESTS PASSED (%d checks)\n", g_checks);
  return 0;
}
