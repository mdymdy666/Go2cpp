// Copyright 2026 Go2Cpp Authors. All rights reserved.
// MemCache 正确性测试：1e7 Get/Set、TTL 过期、LRU 淘汰、写穿 LSM、多线程。

#include "go2cpp/storage/kv.hpp"
#include "go2cpp/storage/memcache.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using go2cpp::storage::KV;
using go2cpp::storage::MemCache;
using go2cpp::storage::MemCacheOptions;

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                       \
  do {                                                                    \
    ++g_checks;                                                           \
    if (!(cond)) {                                                        \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                       \
    }                                                                     \
  } while (0)

#define CHECK_MSG(cond, ...)                                              \
  do {                                                                    \
    ++g_checks;                                                           \
    if (!(cond)) {                                                        \
      std::fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
      std::fprintf(stderr, __VA_ARGS__);                                  \
      std::fprintf(stderr, "\n");                                         \
      ++g_failures;                                                       \
    }                                                                     \
  } while (0)

std::string TempDir(const char* tag) {
  char tmpl[256];
  std::snprintf(tmpl, sizeof(tmpl), "/tmp/go2cpp_memcache_%s_XXXXXX", tag);
  char* dir = ::mkdtemp(tmpl);
  return dir != nullptr ? std::string(dir) : std::string();
}

// 32B 内联：key 16B + value 16B。基准与测试都用这个形状。
std::string Key(std::uint64_t i) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "key-%016llu",
                static_cast<unsigned long long>(i));
  return std::string(buf);
}

std::string Value(std::uint64_t i) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "val-%016llu",
                static_cast<unsigned long long>(i));
  return std::string(buf);
}

void TestBasic() {
  auto cache_p = MemCache::Open(nullptr);
  CHECK(cache_p != nullptr);
  if (cache_p == nullptr) {
    return;
  }
  MemCache& c = *cache_p;
  CHECK(c.Set("a", "1"));
  CHECK(c.Set("b", "2"));
  CHECK(c.Size() == 2);
  std::string v;
  CHECK(c.Get("a", &v));
  CHECK(v == "1");
  CHECK(!c.Get("missing", &v));
  CHECK(c.Peek("a", &v));
  CHECK(v == "1");
  CHECK(c.Delete("a"));
  CHECK(!c.Get("a", &v));
  CHECK(!c.Delete("a"));
  // 覆盖
  CHECK(c.Set("b", "22"));
  CHECK(c.Get("b", &v));
  CHECK(v == "22");
  // 长 value（走堆分配）
  const std::string big(4096, 'x');
  CHECK(c.Set("big", big));
  CHECK(c.Get("big", &v));
  CHECK(v.size() == 4096);
  CHECK(c.Delete("big"));
  const auto stats = c.GetStats();
  CHECK(stats.sets >= 4);
  CHECK(stats.hits >= 3);
  CHECK(stats.misses >= 2);
}

void TestTtl() {
  auto cache_p = MemCache::Open(nullptr);
  CHECK(cache_p != nullptr);
  if (cache_p == nullptr) {
    return;
  }
  MemCache& c = *cache_p;
  CHECK(c.Set("t1", "v1", std::chrono::milliseconds(50)));
  CHECK(c.Set("t2", "v2", std::chrono::milliseconds(0)));
  std::string v;
  CHECK(c.Get("t1", &v));
  CHECK(c.Get("t2", &v));
  std::this_thread::sleep_for(std::chrono::milliseconds(90));
  CHECK(!c.Get("t1", &v));  // 已过期
  CHECK(c.Get("t2", &v));   // 无 TTL，永不过期
  CHECK(c.GetStats().expirations >= 1);
  // 主动清理
  CHECK(c.Set("t3", "v3", std::chrono::milliseconds(20)));
  std::this_thread::sleep_for(std::chrono::milliseconds(40));
  const std::size_t purged = c.PurgeExpired();
  CHECK(purged >= 1);
  CHECK(!c.Get("t3", &v));
  // 覆盖时清掉旧 TTL
  CHECK(c.Set("t4", "v4", std::chrono::milliseconds(30)));
  CHECK(c.Set("t4", "v4b", std::chrono::milliseconds(0)));
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  CHECK(c.Get("t4", &v));
  CHECK(v == "v4b");
  // Expire 立刻失效
  CHECK(c.Set("t5", "v5"));
  CHECK(c.Expire("t5"));
  CHECK(!c.Get("t5", &v));
}

void TestLru() {
  MemCacheOptions opt;
  opt.capacity = 64;
  opt.shards = 1;
  auto cache_p = MemCache::Open(nullptr, opt);
  CHECK(cache_p != nullptr);
  if (cache_p == nullptr) {
    return;
  }
  MemCache& c = *cache_p;
  for (std::uint64_t i = 0; i < 128; ++i) {
    CHECK(c.Set(Key(i), Value(i)));
  }
  CHECK(c.Size() == 64);
  CHECK(c.GetStats().evictions == 64);
  std::string v;
  // 最早 64 个应被淘汰
  for (std::uint64_t i = 0; i < 64; ++i) {
    CHECK_MSG(!c.Get(Key(i), &v), "key %llu should be evicted",
              static_cast<unsigned long long>(i));
  }
  for (std::uint64_t i = 64; i < 128; ++i) {
    CHECK(c.Get(Key(i), &v));
    CHECK(v == Value(i));
  }
  // 命中会提升 LRU：把 key(64) 提升到 MRU，再插 32 条，key(64) 不该被淘汰
  CHECK(c.Get(Key(64), &v));
  for (std::uint64_t i = 0; i < 32; ++i) {
    CHECK(c.Set(Key(1000 + i), Value(1000 + i)));
  }
  CHECK(c.Get(Key(64), &v));
}

// 1e7 次 Get/Set：容量 1e6、key 空间 1e6（命中率应显著高于 0）。
void TestLargeVolume(std::uint64_t n) {
  MemCacheOptions opt;
  opt.capacity = 1u << 20;
  opt.shards = 8;
  auto cache_p = MemCache::Open(nullptr, opt);
  CHECK(cache_p != nullptr);
  if (cache_p == nullptr) {
    return;
  }
  MemCache& c = *cache_p;
  const std::uint64_t space = 1u << 20;
  std::uint64_t seed = 0x9e3779b97f4a7c15ull;
  const auto t0 = std::chrono::steady_clock::now();
  for (std::uint64_t i = 0; i < n; ++i) {
    seed ^= seed << 13;
    seed ^= seed >> 7;
    seed ^= seed << 17;
    const std::uint64_t k = seed % space;
    c.Set(Key(k), Value(k));
  }
  const double set_elapsed = std::chrono::duration<double>(
                                 std::chrono::steady_clock::now() - t0)
                                 .count();
  const auto t1 = std::chrono::steady_clock::now();
  std::uint64_t hits = 0;
  for (std::uint64_t i = 0; i < n; ++i) {
    seed ^= seed << 13;
    seed ^= seed >> 7;
    seed ^= seed << 17;
    const std::uint64_t k = seed % space;
    std::string v;
    if (c.Get(Key(k), &v)) {
      ++hits;
    }
  }
  const double get_elapsed = std::chrono::duration<double>(
                                 std::chrono::steady_clock::now() - t1)
                                 .count();
  const auto stats = c.GetStats();
  CHECK(stats.size <= opt.capacity);
  if (n > 4 * static_cast<std::uint64_t>(opt.capacity)) {
    CHECK(stats.evictions > 0);
  }
  std::printf("  [1e7] n=%llu set=%.2fs (%.0f/s) get=%.2fs (%.0f/s) "
              "hit=%.1f%% size=%llu evictions=%llu bytes=%llu\n",
              static_cast<unsigned long long>(n), set_elapsed, n / set_elapsed,
              get_elapsed, n / get_elapsed,
              n == 0 ? 0.0 : 100.0 * static_cast<double>(hits) / n,
              static_cast<unsigned long long>(stats.size),
              static_cast<unsigned long long>(stats.evictions),
              static_cast<unsigned long long>(stats.bytes));
}

void TestMultiThread() {
  MemCacheOptions opt;
  opt.capacity = 1u << 18;
  opt.shards = 8;
  auto cache_p = MemCache::Open(nullptr, opt);
  CHECK(cache_p != nullptr);
  if (cache_p == nullptr) {
    return;
  }
  MemCache& c = *cache_p;
  constexpr int kThreads = 4;
  constexpr std::uint64_t kOps = 500000;
  std::atomic<std::uint64_t> hits{0};
  std::vector<std::thread> threads;
  const auto t0 = std::chrono::steady_clock::now();
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&c, &hits, t] {
      std::uint64_t seed = 0x1234567ull + static_cast<std::uint64_t>(t);
      for (std::uint64_t i = 0; i < kOps; ++i) {
        seed ^= seed << 13;
        seed ^= seed >> 7;
        seed ^= seed << 17;
        const std::uint64_t k = seed % (1u << 20);
        std::string v;
        if (c.Get(Key(k), &v)) {
          hits.fetch_add(1, std::memory_order_relaxed);
        }
        c.Set(Key(k), Value(k));
        if ((i & 1023) == 0) {
          c.Delete(Key(k + 1));
        }
      }
    });
  }
  for (auto& th : threads) {
    th.join();
  }
  const double elapsed = std::chrono::duration<double>(
                             std::chrono::steady_clock::now() - t0)
                             .count();
  const auto stats = c.GetStats();
  CHECK(stats.size <= opt.capacity);
  CHECK(stats.gets == kThreads * kOps);
  std::printf("  [mt] %d threads x %llu ops = %.2fs (%.0f ops/s) hits=%llu "
              "size=%llu\n",
              kThreads, static_cast<unsigned long long>(kOps), elapsed,
              kThreads * kOps / elapsed,
              static_cast<unsigned long long>(hits.load()),
              static_cast<unsigned long long>(stats.size));
}

void TestPersist() {
  const std::string dir = TempDir("persist");
  auto kv = KV::Open(dir);
  CHECK(kv != nullptr);
  if (kv == nullptr) {
    return;
  }
  MemCacheOptions opt;
  opt.capacity = 1024;
  opt.persist = true;
  opt.read_through = true;
  auto cache_p = MemCache::Open(kv.get(), opt);
  CHECK(cache_p != nullptr);
  if (cache_p == nullptr) {
    return;
  }
  MemCache& c = *cache_p;
  CHECK(c.Set("p/1", "v1"));
  // 写穿：KV 里应该能读到
  std::string stored;
  CHECK(kv->Get("p/1", &stored));
  CHECK(stored == "v1");
  CHECK(c.GetStats().write_throughs == 1);

  // 回填：KV 有值但缓存没有
  CHECK(kv->Put("p/2", "v2"));
  std::string v;
  CHECK(c.Get("p/2", &v));
  CHECK(v == "v2");
  CHECK(c.GetStats().read_throughs == 1);
  CHECK(c.Peek("p/2", &v));

  CHECK(c.Delete("p/1"));
  CHECK(!kv->Get("p/1", &stored));
}

void TestClearAndPurge() {
  MemCacheOptions opt;
  opt.capacity = 256;
  opt.shards = 2;
  auto cache_p = MemCache::Open(nullptr, opt);
  CHECK(cache_p != nullptr);
  if (cache_p == nullptr) {
    return;
  }
  MemCache& c = *cache_p;
  for (std::uint64_t i = 0; i < 100; ++i) {
    CHECK(c.Set(Key(i), Value(i), std::chrono::milliseconds(10)));
  }
  CHECK(c.Size() == 100);
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  CHECK(c.PurgeExpired() == 100);
  CHECK(c.Size() == 0);
  for (std::uint64_t i = 0; i < 50; ++i) {
    CHECK(c.Set(Key(i), Value(i)));
  }
  c.Clear();
  CHECK(c.Size() == 0);
  std::string v;
  CHECK(!c.Get(Key(0), &v));
  // Clear 后还能继续用
  CHECK(c.Set("after-clear", "ok"));
  CHECK(c.Get("after-clear", &v));
}

}  // namespace

int main(int argc, char** argv) {
  std::uint64_t n = 10000000;
  for (int i = 1; i < argc; ++i) {
    if (std::strncmp(argv[i], "--n=", 4) == 0) {
      n = std::strtoull(argv[i] + 4, nullptr, 10);
    }
  }
  std::printf("go2cpp_memcache_test (n=%llu)\n",
              static_cast<unsigned long long>(n));
  TestBasic();
  TestTtl();
  TestLru();
  TestClearAndPurge();
  TestPersist();
  TestMultiThread();
  TestLargeVolume(n);
  std::printf("%s: %d checks, %d failures\n",
              g_failures == 0 ? "PASS" : "FAIL", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
