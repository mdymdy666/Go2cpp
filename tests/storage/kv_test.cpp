// Copyright 2026 Go2Cpp Authors. All rights reserved.
// KV 功能 / 边界 / 持久化 / 迭代器 / 批 / Compaction 测试。

#include "go2cpp/storage/kv.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <unistd.h>

using go2cpp::storage::Iterator;
using go2cpp::storage::KV;
using go2cpp::storage::Options;
using go2cpp::storage::WriteBatch;

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

void TestBasic() {
  const std::string dir = TempDir("basic");
  {
    auto db = KV::Open(dir);
    CHECK(db != nullptr);
    if (!db) {
      return;
    }
    CHECK(db->Get("missing", nullptr) == false);
    CHECK(db->Put("a", "1"));
    CHECK(db->Put("b", "2"));
    std::string v;
    CHECK(db->Get("a", &v) && v == "1");
    CHECK(db->Get("b", &v) && v == "2");
    CHECK(db->Put("a", "3"));
    CHECK(db->Get("a", &v) && v == "3");
    CHECK(db->Delete("a"));
    CHECK(db->Get("a", &v) == false);
    CHECK(db->Get("b", &v) && v == "2");
    CHECK(db->ok());
  }
  {
    auto db = KV::Open(dir);
    CHECK(db != nullptr);
    if (db) {
      std::string v;
      CHECK(db->Get("b", &v) && v == "2");
      CHECK(db->Get("a", &v) == false);
    }
  }
  std::filesystem::remove_all(dir);
}

void TestBoundary() {
  const std::string dir = TempDir("boundary");
  auto db = KV::Open(dir);
  CHECK(db != nullptr);
  if (!db) {
    return;
  }
  // 空 key / 空 value
  CHECK(db->Put("", "empty-key"));
  std::string v;
  CHECK(db->Get("", &v) && v == "empty-key");
  CHECK(db->Put("empty-val", ""));
  CHECK(db->Get("empty-val", &v) && v.empty());
  CHECK(db->Delete(""));
  CHECK(db->Get("", &v) == false);

  // 二进制 key（含 '\0'）
  const std::string bin_key("bin\0key", 7);
  CHECK(db->Put(bin_key, "bin-value"));
  CHECK(db->Get(bin_key, &v) && v == "bin-value");
  CHECK(db->Delete(bin_key));

  // 大 value（1MB，超过块大小，跨块）
  const std::string big(1 << 20, 'x');
  CHECK(db->Put("big", big));
  CHECK(db->Get("big", &v) && v == big);

  // 超长 key（64KB）
  const std::string long_key(64 * 1024, 'k');
  CHECK(db->Put(long_key, "long-key-value"));
  CHECK(db->Get(long_key, &v) && v == "long-key-value");

  // 大 value 落盘后仍可读
  db->Flush();
  CHECK(db->Get("big", &v) && v == big);
  CHECK(db->Get(long_key, &v) && v == "long-key-value");
  std::filesystem::remove_all(dir);
}

void TestWriteBatch() {
  const std::string dir = TempDir("batch");
  auto db = KV::Open(dir);
  CHECK(db != nullptr);
  if (!db) {
    return;
  }
  WriteBatch batch;
  for (int i = 0; i < 1000; ++i) {
    batch.Put(Key(i), Value(i));
  }
  CHECK(batch.Count() == 1000);
  CHECK(db->Write(batch));
  for (int i = 0; i < 1000; ++i) {
    std::string v;
    CHECK(db->Get(Key(i), &v) && v == Value(i));
  }
  WriteBatch del;
  for (int i = 0; i < 500; ++i) {
    del.Delete(Key(i));
  }
  CHECK(db->Write(del));
  for (int i = 0; i < 500; ++i) {
    std::string v;
    CHECK(db->Get(Key(i), &v) == false);
  }
  for (int i = 500; i < 1000; ++i) {
    std::string v;
    CHECK(db->Get(Key(i), &v) && v == Value(i));
  }
  WriteBatch empty;
  CHECK(db->Write(empty));
  std::filesystem::remove_all(dir);
}

void TestIterator() {
  const std::string dir = TempDir("iter");
  Options opt;
  opt.memtable_size = 64 * 1024;
  auto db = KV::Open(dir, opt);
  CHECK(db != nullptr);
  if (!db) {
    return;
  }
  for (int i = 0; i < 3000; ++i) {
    CHECK(db->Put(Key(i), Value(i)));
  }
  for (int i = 0; i < 3000; i += 3) {
    CHECK(db->Delete(Key(i)));
  }
  auto it = db->NewIterator();
  CHECK(it != nullptr);
  int count = 0;
  bool ordered = true;
  bool deleted_absent = true;
  std::string prev;
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    const std::string k(it->key());
    if (!prev.empty() && !(prev < k)) {
      ordered = false;
    }
    prev = k;
    ++count;
    int idx = std::atoi(k.c_str() + 4);
    if (idx % 3 == 0) {
      deleted_absent = false;
    }
  }
  CHECK(count == 2000);
  CHECK(ordered);
  CHECK(deleted_absent);

  // Seek：1500 已被删除（i%3==0），应定位到下一个存活 key。
  it->Seek(Key(1500));
  CHECK(it->Valid());
  CHECK(std::string(it->key()) == Key(1501));
  it->Seek(Key(1502));
  CHECK(it->Valid());
  CHECK(std::string(it->key()) == Key(1502));
  it->SeekToLast();
  CHECK(it->Valid());
  CHECK(std::string(it->key()) == Key(2999));

  // 空库迭代器
  const std::string dir2 = TempDir("iter-empty");
  auto db2 = KV::Open(dir2);
  auto it2 = db2->NewIterator();
  it2->SeekToFirst();
  CHECK(!it2->Valid());
  it2->SeekToLast();
  CHECK(!it2->Valid());
  std::filesystem::remove_all(dir2);
  std::filesystem::remove_all(dir);
}

void TestFlushAndCompaction() {
  const std::string dir = TempDir("flush");
  Options opt;
  opt.memtable_size = 64 * 1024;
  opt.max_sstables = 4;
  opt.sstables_per_compaction = 2;
  opt.block_size = 1024;
  auto db = KV::Open(dir, opt);
  CHECK(db != nullptr);
  if (!db) {
    return;
  }
  const int kN = 20000;
  for (int i = 0; i < kN; ++i) {
    CHECK(db->Put(Key(i), Value(i)));
  }
  const auto stats = db->GetStats();
  CHECK(stats.memtable_flushes >= 2);
  CHECK(stats.compactions >= 1);

  // 跨 flush/compaction 的读
  int bad = 0;
  for (int i = 0; i < kN; ++i) {
    std::string v;
    if (!db->Get(Key(i), &v) || v != Value(i)) {
      ++bad;
    }
  }
  CHECK(bad == 0);

  // 覆盖写：旧值被新值遮蔽
  for (int i = 0; i < 1000; ++i) {
    CHECK(db->Put(Key(i), "updated-" + Value(i)));
  }
  db->Flush();
  for (int i = 0; i < 1000; ++i) {
    std::string v;
    CHECK(db->Get(Key(i), &v) && v == "updated-" + Value(i));
  }
  for (int i = 1000; i < kN; ++i) {
    std::string v;
    CHECK(db->Get(Key(i), &v) && v == Value(i));
  }
  std::filesystem::remove_all(dir);
}

void TestPersistence() {
  const std::string dir = TempDir("persist");
  {
    Options opt;
    opt.memtable_size = 32 * 1024;
    opt.max_sstables = 3;
    auto db = KV::Open(dir, opt);
    CHECK(db != nullptr);
    if (!db) {
      return;
    }
    for (int i = 0; i < 5000; ++i) {
      CHECK(db->Put(Key(i), Value(i)));
    }
    db->Close();
    // Close 之后的目录只剩 sstable
    int sst = 0;
    for (const auto& f : db->Files()) {
      if (f.size() > 4 && f.substr(f.size() - 4) == ".sst") {
        ++sst;
      }
    }
    CHECK(sst >= 1);
  }
  {
    auto db = KV::Open(dir);
    CHECK(db != nullptr);
    if (db) {
      int bad = 0;
      for (int i = 0; i < 5000; ++i) {
        std::string v;
        if (!db->Get(Key(i), &v) || v != Value(i)) {
          ++bad;
        }
      }
      CHECK(bad == 0);
    }
  }
  std::filesystem::remove_all(dir);
}

void TestOverwriteDeleteReverse() {
  const std::string dir = TempDir("ow");
  Options opt;
  opt.memtable_size = 32 * 1024;
  auto db = KV::Open(dir, opt);
  CHECK(db != nullptr);
  if (!db) {
    return;
  }
  // 同一 key 反复写，最后一次生效；跨 flush 依然正确
  for (int round = 0; round < 200; ++round) {
    CHECK(db->Put("hot", "round-" + std::to_string(round)));
    db->Flush();
    std::string v;
    CHECK(db->Get("hot", &v) && v == "round-" + std::to_string(round));
  }
  CHECK(db->Delete("hot"));
  db->Flush();
  std::string v;
  CHECK(db->Get("hot", &v) == false);
  CHECK(db->Put("hot", "resurrected"));
  CHECK(db->Get("hot", &v) && v == "resurrected");
  std::filesystem::remove_all(dir);
}

void TestReopenAfterKill() {
  // 不做 fork：直接 Close 前多次 Flush，再重开验证。
  const std::string dir = TempDir("reopen");
  {
    auto db = KV::Open(dir);
    CHECK(db != nullptr);
    for (int i = 0; i < 100; ++i) {
      CHECK(db->Put(Key(i), Value(i)));
    }
    db->Flush();
    for (int i = 100; i < 200; ++i) {
      CHECK(db->Put(Key(i), Value(i)));
    }
    // 不 Close，直接析构（析构会 Close 并刷盘）
  }
  auto db = KV::Open(dir);
  CHECK(db != nullptr);
  if (db) {
    int bad = 0;
    for (int i = 0; i < 200; ++i) {
      std::string v;
      if (!db->Get(Key(i), &v) || v != Value(i)) {
        ++bad;
      }
    }
    CHECK(bad == 0);
  }
  std::filesystem::remove_all(dir);
}

}  // namespace

int main() {
  TestBasic();
  TestBoundary();
  TestWriteBatch();
  TestIterator();
  TestFlushAndCompaction();
  TestPersistence();
  TestOverwriteDeleteReverse();
  TestReopenAfterKill();
  std::printf("kv_test: %d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
