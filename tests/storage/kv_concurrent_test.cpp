// Copyright 2026 Go2Cpp Authors. All rights reserved.
// 并发测试：多线程写、读写混合、flush/compaction 期间的读一致性。

#include "go2cpp/storage/kv.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

using go2cpp::storage::KV;
using go2cpp::storage::Options;

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                        \
  do {                                                                     \
    ++g_checks;                                                            \
    if (!(cond)) {                                                         \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                        \
    }                                                                      \
  } while (0)

std::string TempDir(const char* tag) {
  char tmpl[256];
  std::snprintf(tmpl, sizeof(tmpl), "/tmp/go2cpp_kv_%s_XXXXXX", tag);
  char* dir = ::mkdtemp(tmpl);
  return dir != nullptr ? std::string(dir) : std::string();
}

std::string Key(int i) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "key-%08d", i);
  return std::string(buf);
}

std::string Value(int i) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "val-%08d", i);
  return std::string(buf);
}

void TestConcurrentWrites() {
  const std::string dir = TempDir("conc_w");
  Options opt;
  opt.memtable_size = 256 * 1024;  // 频繁刷盘，压并发路径
  opt.max_sstables = 8;
  auto db = KV::Open(dir, opt);
  CHECK(db != nullptr);
  if (!db) {
    return;
  }
  constexpr int kThreads = 8;
  constexpr int kPer = 4000;
  std::atomic<int> errors{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      for (int i = 0; i < kPer; ++i) {
        const int id = t * kPer + i;
        if (!db->Put(Key(id), Value(id))) {
          ++errors;
        }
        if (i % 16 == 0) {
          std::string v;
          if (!db->Get(Key(id), &v) || v != Value(id)) {
            ++errors;
          }
        }
      }
    });
  }
  for (auto& th : threads) {
    th.join();
  }
  CHECK(errors.load() == 0);
  int bad = 0;
  for (int id = 0; id < kThreads * kPer; ++id) {
    std::string v;
    if (!db->Get(Key(id), &v) || v != Value(id)) {
      ++bad;
    }
  }
  CHECK(bad == 0);
  db->Close();
  std::filesystem::remove_all(dir);
}

void TestReadWriteMix() {
  const std::string dir = TempDir("conc_rw");
  Options opt;
  opt.memtable_size = 512 * 1024;
  auto db = KV::Open(dir, opt);
  CHECK(db != nullptr);
  if (!db) {
    return;
  }
  constexpr int kN = 5000;
  for (int i = 0; i < kN; ++i) {
    CHECK(db->Put(Key(i), Value(i)));
  }
  std::atomic<bool> stop{false};
  std::atomic<int> errors{0};
  std::atomic<long> reads{0};
  std::vector<std::thread> readers;
  for (int t = 0; t < 4; ++t) {
    readers.emplace_back([&, t] {
      std::mt19937 rng(1234 + t);
      std::uniform_int_distribution<int> dist(0, kN - 1);
      while (!stop.load(std::memory_order_relaxed)) {
        const int id = dist(rng);
        std::string v;
        if (db->Get(Key(id), &v)) {
          if (v != Value(id) && v != "updated") {
            ++errors;
          }
        } else {
          ++errors;  // 这些 key 不会被删除
        }
        reads.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }
  for (int round = 0; round < 30; ++round) {
    for (int i = 0; i < kN; ++i) {
      if (!db->Put(Key(i), "updated")) {
        ++errors;
      }
    }
  }
  stop.store(true);
  for (auto& th : readers) {
    th.join();
  }
  CHECK(errors.load() == 0);
  CHECK(reads.load() > 0);
  db->Close();
  std::filesystem::remove_all(dir);
}

void TestConcurrentDeletes() {
  const std::string dir = TempDir("conc_d");
  auto db = KV::Open(dir);
  CHECK(db != nullptr);
  if (!db) {
    return;
  }
  constexpr int kN = 3000;
  for (int i = 0; i < kN; ++i) {
    CHECK(db->Put(Key(i), Value(i)));
  }
  std::atomic<int> errors{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([&, t] {
      for (int i = t; i < kN; i += 4) {
        if (!db->Delete(Key(i))) {
          ++errors;
        }
      }
    });
  }
  for (auto& th : threads) {
    th.join();
  }
  CHECK(errors.load() == 0);
  int alive = 0;
  for (int i = 0; i < kN; ++i) {
    std::string v;
    if (db->Get(Key(i), &v)) {
      ++alive;
    }
  }
  CHECK(alive == 0);
  db->Close();
  std::filesystem::remove_all(dir);
}

void TestIterateWhileWriting() {
  const std::string dir = TempDir("conc_iter");
  Options opt;
  opt.memtable_size = 128 * 1024;
  auto db = KV::Open(dir, opt);
  CHECK(db != nullptr);
  if (!db) {
    return;
  }
  for (int i = 0; i < 2000; ++i) {
    CHECK(db->Put(Key(i), Value(i)));
  }
  std::atomic<bool> stop{false};
  std::atomic<int> errors{0};
  std::thread writer([&] {
    for (int round = 0; round < 50 && !stop.load(); ++round) {
      for (int i = 0; i < 500; ++i) {
        if (!db->Put("w-" + Key(i), Value(i))) {
          ++errors;
        }
      }
    }
  });
  for (int r = 0; r < 10; ++r) {
    auto it = db->NewIterator();
    CHECK(it != nullptr);
    int count = 0;
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
      const std::string k(it->key());
      if (k.empty()) {
        ++errors;
      }
      ++count;
    }
    CHECK(count >= 2000);
  }
  stop.store(true);
  writer.join();
  CHECK(errors.load() == 0);
  db->Close();
  std::filesystem::remove_all(dir);
}

}  // namespace

int main() {
  TestConcurrentWrites();
  TestReadWriteMix();
  TestConcurrentDeletes();
  TestIterateWhileWriting();
  std::printf("kv_concurrent_test: %d checks, %d failures\n", g_checks,
              g_failures);
  return g_failures == 0 ? 0 : 1;
}
