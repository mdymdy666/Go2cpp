// Copyright 2026 Go2Cpp Authors. All rights reserved.
// 崩溃恢复测试：kill -9 后 WAL 重放、WAL 截断、WAL 位翻转。

#include "go2cpp/storage/kv.hpp"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <fcntl.h>
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

// 返回目录里第一个扩展名为 ext 的文件路径。
std::string FindFile(const std::string& dir, const std::string& ext) {
  for (const auto& entry : std::filesystem::directory_iterator(dir)) {
    const std::string name = entry.path().filename().string();
    if (name.size() > ext.size() &&
        name.compare(name.size() - ext.size(), ext.size(), ext) == 0) {
      return entry.path().string();
    }
  }
  return std::string();
}

void TestCrashWithSync() {
  const std::string dir = TempDir("crash_sync");
  const int kN = 1000;
  const pid_t pid = ::fork();
  if (pid == 0) {
    Options opt;
    opt.sync_writes = true;
    auto db = KV::Open(dir, opt);
    if (!db) {
      ::_exit(2);
    }
    for (int i = 0; i < kN; ++i) {
      if (!db->Put(Key(i), Value(i))) {
        ::_exit(3);
      }
    }
    // 模拟进程被 kill -9：不走析构、不刷盘。
    ::raise(SIGKILL);
    ::_exit(4);
  }
  int status = 0;
  ::waitpid(pid, &status, 0);
  CHECK(WIFSIGNALED(status));
  if (WIFSIGNALED(status)) {
    CHECK(WTERMSIG(status) == SIGKILL);
  }

  auto db = KV::Open(dir);
  CHECK(db != nullptr);
  if (!db) {
    return;
  }
  int found = 0;
  for (int i = 0; i < kN; ++i) {
    std::string v;
    if (db->Get(Key(i), &v) && v == Value(i)) {
      ++found;
    }
  }
  CHECK(found == kN);
  CHECK(db->GetStats().recover_records >= static_cast<std::uint64_t>(kN));
  db->Close();
  std::filesystem::remove_all(dir);
}

void TestTruncatedWal() {
  const std::string dir = TempDir("crash_trunc");
  const int kN = 200;
  const pid_t pid = ::fork();
  if (pid == 0) {
    Options opt;
    opt.sync_writes = true;
    auto db = KV::Open(dir, opt);
    if (!db) {
      ::_exit(2);
    }
    for (int i = 0; i < kN; ++i) {
      if (!db->Put(Key(i), Value(i))) {
        ::_exit(3);
      }
    }
    ::_exit(0);  // 不析构：WAL 保留
  }
  int status = 0;
  ::waitpid(pid, &status, 0);
  CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);

  const std::string wal = FindFile(dir, ".log");
  CHECK(!wal.empty());
  if (wal.empty()) {
    return;
  }
  struct stat st;
  CHECK(::stat(wal.c_str(), &st) == 0);
  const off_t cut = st.st_size - 3;  // 截掉最后 3 字节：最后一条记录不完整
  CHECK(::truncate(wal.c_str(), cut) == 0);

  auto db = KV::Open(dir);
  CHECK(db != nullptr);
  if (!db) {
    return;
  }
  int found = 0;
  for (int i = 0; i < kN; ++i) {
    std::string v;
    if (db->Get(Key(i), &v) && v == Value(i)) {
      ++found;
    }
  }
  // 最后一条记录被截断：至少恢复 kN-1 条，且不崩溃。
  CHECK(found >= kN - 1);
  db->Close();
  std::filesystem::remove_all(dir);
}

void TestCorruptedWalByte() {
  const std::string dir = TempDir("crash_corrupt");
  const int kN = 100;
  const pid_t pid = ::fork();
  if (pid == 0) {
    Options opt;
    opt.sync_writes = true;
    auto db = KV::Open(dir, opt);
    if (!db) {
      ::_exit(2);
    }
    for (int i = 0; i < kN; ++i) {
      if (!db->Put(Key(i), Value(i))) {
        ::_exit(3);
      }
    }
    ::_exit(0);
  }
  int status = 0;
  ::waitpid(pid, &status, 0);

  const std::string wal = FindFile(dir, ".log");
  CHECK(!wal.empty());
  if (wal.empty()) {
    return;
  }
  // 翻转文件中间一个字节，破坏该条记录的 CRC。
  const int fd = ::open(wal.c_str(), O_RDWR);
  CHECK(fd >= 0);
  if (fd < 0) {
    return;
  }
  struct stat st;
  CHECK(::fstat(fd, &st) == 0);
  const off_t pos = st.st_size / 2;
  char byte = 0;
  CHECK(::pread(fd, &byte, 1, pos) == 1);
  byte = static_cast<char>(byte ^ 0x5a);
  CHECK(::pwrite(fd, &byte, 1, pos) == 1);
  ::close(fd);

  auto db = KV::Open(dir);
  CHECK(db != nullptr);
  if (db) {
    // 损坏点之后的数据不再保证，但必须能打开，且损坏点之前的数据可读。
    int found = 0;
    for (int i = 0; i < kN; ++i) {
      std::string v;
      if (db->Get(Key(i), &v) && v == Value(i)) {
        ++found;
      }
    }
    CHECK(found >= kN / 4);
    db->Close();
  }
  std::filesystem::remove_all(dir);
}

void TestCrashDuringFlush() {
  // 大数据量 + 小 memtable：写到一半 kill，重开后数据要么在 sstable、
  // 要么在 WAL，不能丢也不能错。
  const std::string dir = TempDir("crash_flush");
  const int kN = 20000;
  const pid_t pid = ::fork();
  if (pid == 0) {
    Options opt;
    opt.memtable_size = 32 * 1024;  // 频繁 flush
    auto db = KV::Open(dir, opt);
    if (!db) {
      ::_exit(2);
    }
    for (int i = 0; i < kN; ++i) {
      if (!db->Put(Key(i), Value(i))) {
        ::_exit(3);
      }
    }
    ::raise(SIGKILL);
    ::_exit(4);
  }
  int status = 0;
  ::waitpid(pid, &status, 0);
  CHECK(WIFSIGNALED(status));

  auto db = KV::Open(dir);
  CHECK(db != nullptr);
  if (!db) {
    return;
  }
  // sync=false：最后一段可能丢，但已刷盘的部分必须完整且无错值。
  int good = 0;
  int wrong = 0;
  for (int i = 0; i < kN; ++i) {
    std::string v;
    if (db->Get(Key(i), &v)) {
      if (v == Value(i)) {
        ++good;
      } else {
        ++wrong;
      }
    }
  }
  CHECK(wrong == 0);
  CHECK(good >= kN / 2);
  std::printf("  crash_during_flush: recovered %d/%d\n", good, kN);
  db->Close();
  std::filesystem::remove_all(dir);
}

}  // namespace

int main() {
  TestCrashWithSync();
  TestTruncatedWal();
  TestCorruptedWalByte();
  TestCrashDuringFlush();
  std::printf("kv_recovery_test: %d checks, %d failures\n", g_checks,
              g_failures);
  return g_failures == 0 ? 0 : 1;
}
