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
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <sys/resource.h>
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

// 场景 5：刷盘失败（目录 0555）后写路径必须报错并进入故障态；恢复权限后
// 一次成功 Flush 应把失败前已确认的写全部落盘，重启不得丢数据（P1-1）。
void TestFlushFailureKeepsData() {
  const std::string dir = TempDir("flush_fail");
  CHECK(!dir.empty());
  if (dir.empty()) {
    return;
  }
  CHECK(::chmod(dir.c_str(), 0777) == 0);
  const bool drop = ::geteuid() == 0;
  if (drop) {
    CHECK(::chown(dir.c_str(), 65534, 65534) == 0);
  }
  const pid_t pid = ::fork();
  if (pid == 0) {
    if (drop && (::setgid(65534) != 0 || ::setuid(65534) != 0)) {
      ::_exit(20);
    }
    Options opt;
    opt.memtable_size = 16 * 1024;  // 失败前最后一批写仍留在内存表
    opt.sync_writes = true;
    auto db = KV::Open(dir, opt);
    if (!db) {
      ::_exit(21);
    }
    for (int i = 0; i < 10; ++i) {
      if (!db->Put("k" + std::to_string(i), std::string(300, 'k'))) {
        ::_exit(22);
      }
    }
    if (::chmod(dir.c_str(), 0555) != 0) {
      ::_exit(23);
    }
    bool failed = false;
    for (int i = 0; i < 300 && !failed; ++i) {
      failed = !db->Put("b" + std::to_string(i), std::string(400, 'b'));
    }
    if (!failed) {
      ::_exit(24);  // 表满必须撞上刷盘失败
    }
    if (::chmod(dir.c_str(), 0755) != 0) {
      ::_exit(25);
    }
    // 故障态语义：还没有一次成功 flush 之前，写路径必须继续报错。
    if (db->Put("stalled", "1")) {
      ::_exit(26);
    }
    db->Flush();  // 恢复点：权限恢复后这次 flush 必须成功
    if (!db->Put("after", "ok")) {
      ::_exit(27);
    }
    for (int i = 0; i < 20; ++i) {
      if (!db->Put("c" + std::to_string(i), std::string(200, 'c'))) {
        ::_exit(28);
      }
    }
    db->Close();
    ::_exit(0);
  }
  int status = 0;
  ::waitpid(pid, &status, 0);
  CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  {
    auto db = KV::Open(dir);
    CHECK(db != nullptr);
    if (db) {
      for (int i = 0; i < 10; ++i) {
        std::string v;
        CHECK(db->Get("k" + std::to_string(i), &v) &&
              v == std::string(300, 'k'));
      }
      std::string v;
      CHECK(db->Get("after", &v) && v == "ok");
      db->Close();
    }
  }
  std::filesystem::remove_all(dir);
}

// 场景 6：WAL 部分写失败（RLIMIT_FSIZE 截断）后重试，已落盘前缀不得重写：
// 重启后 WAL 必须能完整解析，k9/k10 不得因重复前缀而损坏丢失（P1-2）。
void TestWalPartialWriteRetry() {
  const std::string dir = TempDir("wal_partial");
  CHECK(!dir.empty());
  if (dir.empty()) {
    return;
  }
  const pid_t pid = ::fork();
  if (pid == 0) {
    ::signal(SIGXFSZ, SIG_IGN);
    Options opt;
    opt.write_buffer_size = 4096;  // 记录攒在用户态缓冲，由阈值触发 write
    opt.sync_writes = false;
    auto db = KV::Open(dir, opt);
    if (!db) {
      ::_exit(30);
    }
    // 7 条 × (510B 记录 + 7B 块头) ≈ 3619B，不到 4096 阈值，尚未落盘。
    for (int i = 1; i <= 7; ++i) {
      if (!db->Put("k" + std::to_string(i), std::string(500, 'a'))) {
        ::_exit(31);
      }
    }
    struct rlimit saved;
    if (::getrlimit(RLIMIT_FSIZE, &saved) != 0) {
      ::_exit(32);
    }
    struct rlimit rl = saved;
    rl.rlim_cur = 64;  // WAL 仍为 0 字节：只允许 64 字节落地 -> 部分写
    if (::setrlimit(RLIMIT_FSIZE, &rl) != 0) {
      ::_exit(33);
    }
    // 第 8 条把缓冲顶过阈值 -> 部分写失败（Put 必须报错）。
    if (db->Put("k8", std::string(500, 'b'))) {
      ::_exit(34);
    }
    if (::setrlimit(RLIMIT_FSIZE, &saved) != 0) {
      ::_exit(35);
    }
    // 重试只允许写剩余后缀；若重写前缀，WAL 流会重复损坏。
    if (!db->Put("k9", std::string(500, 'c'))) {
      ::_exit(36);
    }
    // 大 value 再次顶过阈值，把 k10 与剩余后缀一起落盘。
    if (!db->Put("k10", std::string(4500, 'd'))) {
      ::_exit(37);
    }
    ::_exit(0);  // 模拟崩溃：不 Close，恢复只能依赖 WAL
  }
  int status = 0;
  ::waitpid(pid, &status, 0);
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    std::fprintf(stderr, "  partial-write child status: exited=%d code=%d sig=%d\n",
                 WIFEXITED(status), WIFEXITED(status) ? WEXITSTATUS(status) : -1,
                 WIFSIGNALED(status) ? WTERMSIG(status) : 0);
  }
  CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  auto db = KV::Open(dir);
  CHECK(db != nullptr);
  if (db) {
    for (int i = 1; i <= 7; ++i) {
      std::string v;
      CHECK(db->Get("k" + std::to_string(i), &v) &&
            v == std::string(500, 'a'));
    }
    std::string v8;
    CHECK(db->Get("k8", &v8) && v8 == std::string(500, 'b'));
    std::string v9;
    CHECK(db->Get("k9", &v9) && v9 == std::string(500, 'c'));
    std::string v10;
    CHECK(db->Get("k10", &v10) && v10 == std::string(4500, 'd'));
    db->Close();
  }
  std::filesystem::remove_all(dir);
}

// 场景 7：刷盘时 WAL Close 失败（尾部写不进，RLIMIT_FSIZE）必须被上抛：
// 错误可见、写路径进入故障态；本次数据已进 SST，重启不得回滚（P1-3）。
void TestWalCloseFailureVisible() {
  const std::string dir = TempDir("wal_close_fail");
  CHECK(!dir.empty());
  if (dir.empty()) {
    return;
  }
  const pid_t pid = ::fork();
  if (pid == 0) {
    ::signal(SIGXFSZ, SIG_IGN);
    Options opt;
    opt.memtable_size = 64u << 20;    // 不自动刷盘，让数据停在内存/WAL 缓冲
    opt.write_buffer_size = 64u << 20;  // 不自动落 WAL，Close 时一次性写
    opt.sync_writes = false;
    auto db = KV::Open(dir, opt);
    if (!db) {
      ::_exit(40);
    }
    for (int round = 0; round < 200; ++round) {
      for (int i = 0; i < 10; ++i) {
        const std::string v(500, static_cast<char>('a' + i));
        if (!db->Put("k" + std::to_string(i), v)) {
          ::_exit(41);
        }
      }
    }
    struct rlimit saved;
    if (::getrlimit(RLIMIT_FSIZE, &saved) != 0) {
      ::_exit(42);
    }
    struct rlimit rl = saved;
    rl.rlim_cur = 64 * 1024;  // SST(~6KB) 可落盘，WAL(~1MB) 只能写前缀
    if (::setrlimit(RLIMIT_FSIZE, &rl) != 0) {
      ::_exit(43);
    }
    db->Flush();
    if (db->LastError().find("wal close failed") == std::string::npos) {
      ::_exit(44);  // 旧实现静默忽略 Close 返回值
    }
    if (db->Put("x", "y")) {
      ::_exit(45);  // 故障态：没有成功 flush 之前写必须失败
    }
    if (::setrlimit(RLIMIT_FSIZE, &saved) != 0) {
      ::_exit(46);
    }
    db->Flush();  // 恢复：重新接上 WAL
    if (!db->Put("after", "ok")) {
      ::_exit(47);
    }
    db->Close();
    ::_exit(0);
  }
  int status = 0;
  ::waitpid(pid, &status, 0);
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    std::fprintf(stderr, "  wal-close child status: exited=%d code=%d sig=%d\n",
                 WIFEXITED(status), WIFEXITED(status) ? WEXITSTATUS(status) : -1,
                 WIFSIGNALED(status) ? WTERMSIG(status) : 0);
  }
  CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  auto db = KV::Open(dir);
  CHECK(db != nullptr);
  if (db) {
    for (int i = 0; i < 10; ++i) {
      std::string v;
      CHECK(db->Get("k" + std::to_string(i), &v) &&
            v == std::string(500, static_cast<char>('a' + i)));
    }
    std::string v;
    CHECK(db->Get("after", &v) && v == "ok");
    db->Close();
  }
  std::filesystem::remove_all(dir);
}

}  // namespace

int main() {
  TestStaleWalAfterRestart();
  TestTruncatedWalAfterRestart();
  TestReadOnlyDirectory();
  TestFlushFailureKeepsData();
  TestWalPartialWriteRetry();
  TestWalCloseFailureVisible();
  TestConcurrentFlushCompaction();
  std::printf("storage_audit_test: %d checks, %d failures\n", g_checks,
              g_failures);
  return g_failures == 0 ? 0 : 1;
}
