// Copyright 2026 Go2Cpp Authors. All rights reserved.
// storage 异常路径 / 生命周期 / 并发审计测试（2026-11-04）。
//
// 覆盖：
//   1. 崩溃后残留旧 WAL，重启再写并刷盘，二次重启不得被旧 WAL 覆盖（数据回滚）。
//   2. 截断 WAL（半写尾部）恢复后，旧 WAL 不得在二次重启时覆盖新值。
//   3. 只读目录下 Open/Put 失败必须可见（返回错误，不静默成功）。
//   4. 8 线程读写 + 小 memtable 强制 flush/compaction，读到的值必须完整正确。

#include "go2cpp/storage/kv.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <sys/stat.h>
#include <sys/wait.h>
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
  std::snprintf(tmpl, sizeof(tmpl), "/tmp/go2cpp_storage_audit_%s_XXXXXX", tag);
  char* dir = ::mkdtemp(tmpl);
  return dir != nullptr ? std::string(dir) : std::string();
}

// 子进程：写入 k=value 后 _exit(0)，模拟崩溃（WAL 未刷、无 SST）。
void WriteThenAbort(const std::string& dir, const char* value) {
  const pid_t pid = ::fork();
  if (pid == 0) {
    Options opt;
    opt.sync_writes = true;  // 记录必须真正落到 WAL，崩溃后才有东西可重放
    auto db = KV::Open(dir, opt);
    if (!db || !db->Put("k", value)) {
      ::_exit(2);
    }
    ::_exit(0);
  }
  int status = 0;
  ::waitpid(pid, &status, 0);
  CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

// 场景 1：崩溃 -> 恢复 -> 写新值并 Flush -> 再重启必须读新值。
void TestStaleWalAfterRestart() {
  const std::string dir = TempDir("stale_wal");
  WriteThenAbort(dir, "v1");

  {
    auto db = KV::Open(dir);
    CHECK(db != nullptr);
    if (!db) {
      std::filesystem::remove_all(dir);
      return;
    }
    std::string v;
    CHECK(db->Get("k", &v) && v == "v1");
    CHECK(db->Put("k", "v2"));
    db->Flush();
    db->Close();
  }
  // 旧 WAL（v1）若残留，重启重放会覆盖 SST 中的 v2。
  {
    auto db = KV::Open(dir);
    CHECK(db != nullptr);
    if (!db) {
      std::filesystem::remove_all(dir);
      return;
    }
    std::string v;
    const bool found = db->Get("k", &v);
    CHECK(found && v == "v2");
    db->Close();
  }
  std::filesystem::remove_all(dir);
}

// 场景 2：半写尾部 WAL -> 恢复 -> 写新值并刷盘 -> 再重启不得回滚。
void TestTruncatedWalAfterRestart() {
  const std::string dir = TempDir("trunc_wal");
  WriteThenAbort(dir, "old");
  std::string wal;
  for (const auto& entry : std::filesystem::directory_iterator(dir)) {
    if (entry.path().extension() == ".log") {
      wal = entry.path().string();
    }
  }
  CHECK(!wal.empty());
  if (wal.empty()) {
    std::filesystem::remove_all(dir);
    return;
  }
  struct stat st;
  CHECK(::stat(wal.c_str(), &st) == 0);
  CHECK(::truncate(wal.c_str(), st.st_size - 2) == 0);  // 半写尾部

  {
    auto db = KV::Open(dir);
    CHECK(db != nullptr);
    if (!db) {
      std::filesystem::remove_all(dir);
      return;
    }
    CHECK(db->Put("k", "new"));
    db->Flush();
    db->Close();
  }
  {
    auto db = KV::Open(dir);
    CHECK(db != nullptr);
    if (!db) {
      std::filesystem::remove_all(dir);
      return;
    }
    std::string v;
    CHECK(db->Get("k", &v) && v == "new");
    db->Close();
  }
  std::filesystem::remove_all(dir);
}

// 场景 3：只读目录（0555）——非 root 子进程必须得到可见失败。
void TestReadOnlyDirectory() {
  const std::string dir = TempDir("readonly");
  CHECK(::chmod(dir.c_str(), 0555) == 0);
  const pid_t pid = ::fork();
  if (pid == 0) {
    if (::geteuid() == 0) {
      if (::setgid(65534) != 0 || ::setuid(65534) != 0) {
        ::_exit(5);
      }
    }
    auto db = KV::Open(dir);
    // 目录不可写：Open 必须失败，或者打开后 Put 必须失败并给出 LastError。
    if (db == nullptr) {
      ::_exit(0);
    }
    const bool ok = db->Put("k", "v");
    ::_exit(ok ? 3 : (db->LastError().empty() ? 4 : 0));
  }
  int status = 0;
  ::waitpid(pid, &status, 0);
  CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  CHECK(::chmod(dir.c_str(), 0755) == 0);
  std::filesystem::remove_all(dir);
}

// 场景 4：8 线程读写 + 强制 flush/compaction。
void TestConcurrentFlushCompaction() {
  const std::string dir = TempDir("concurrent_flush");
  Options opt;
  opt.memtable_size = 64 * 1024;
  opt.max_sstables = 2;
  opt.sstables_per_compaction = 2;
  opt.sync_writes = false;
  auto db = KV::Open(dir, opt);
  CHECK(db != nullptr);
  if (!db) {
    std::filesystem::remove_all(dir);
    return;
  }
  constexpr int kThreads = 8;
  constexpr int kPerThread = 1500;
  std::vector<std::thread> threads;
  std::atomic<int> bad{0};
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t]() {
      for (int i = 0; i < kPerThread; ++i) {
        const std::string key =
            "t" + std::to_string(t) + "-k" + std::to_string(i);
        const std::string value =
            "v" + std::to_string(t) + "-" + std::to_string(i);
        if (t % 2 == 0) {
          if (!db->Put(key, value)) {
            bad.fetch_add(1);
          }
        } else {
          std::string got;
          db->Get(key, &got);
          if (!got.empty() && got != value) {
            bad.fetch_add(1);  // 并发读只要求「读到的内容正确」
          }
        }
      }
    });
  }
  for (auto& th : threads) {
    th.join();
  }
  CHECK(bad.load() == 0);
  // 全量校验：写线程的 key 在写返回后必须能读到正确值。
  int wrong = 0;
  for (int t = 0; t < kThreads; t += 2) {
    for (int i = 0; i < kPerThread; ++i) {
      const std::string key =
          "t" + std::to_string(t) + "-k" + std::to_string(i);
      const std::string value =
          "v" + std::to_string(t) + "-" + std::to_string(i);
      std::string got;
      if (!db->Get(key, &got) || got != value) {
        ++wrong;
      }
    }
  }
  CHECK(wrong == 0);
  db->Close();
  std::filesystem::remove_all(dir);
}

}  // namespace

int main() {
  TestStaleWalAfterRestart();
  TestTruncatedWalAfterRestart();
  TestReadOnlyDirectory();
  TestConcurrentFlushCompaction();
  std::printf("storage_audit_test: %d checks, %d failures\n", g_checks,
              g_failures);
  return g_failures == 0 ? 0 : 1;
}
