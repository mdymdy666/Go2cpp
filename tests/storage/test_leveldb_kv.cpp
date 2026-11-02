// Copyright 2026 Go2Cpp Authors. All rights reserved.
// LevelDB 集成测试。
//
// 覆盖：
//   1) 与自研 KV 的同 API 对拍：Put/Get/Delete/WriteBatch/Iterator/Reopen，
//      同一输入序列必须得到同一状态（全量扫描逐条比较 + 抽样 Get）；
//   2) 边界：空 key/value、二进制 key、1MB value、错误路径、Close 后行为、
//      error_if_exists、Seek/SeekToLast/Prev；
//   3) 崩溃恢复：子进程 sync 写 500 条后继续异步写，父进程 kill -9，
//      重开后已 sync 的写必须全部恢复且 DB 可继续使用；
//   4) --bench：1e7 Put/Get 对比（LevelDB vs 自研 KV），Put/Get ops/s 与
//      VmHWM（两个引擎各跑在独立子进程，RSS 互不污染）；
//   5) --mem：1e8 条写入，采样 VmRSS 并断言峰值有界（LevelDB 替换自研的
//      核心动机）。
//
// 用法：
//   ./go2cpp_leveldb_kv_test              # 功能 + 对拍 + 崩溃恢复（ctest）
//   ./go2cpp_leveldb_kv_test --bench      # 1e7 对比；建议 taskset -c 2
//   ./go2cpp_leveldb_kv_test --bench --n=1000000
//   ./go2cpp_leveldb_kv_test --mem        # 1e8 RSS；建议 taskset -c 2

#include "go2cpp/storage/leveldb_kv.hpp"

#ifdef GO2CPP_LEVELDB_HAVE_KV
#include "go2cpp/storage/kv.hpp"
#endif

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace ldb = go2cpp::storage::ldb;

namespace {

using Clock = std::chrono::steady_clock;

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

double NowNs() {
  return std::chrono::duration<double, std::nano>(
             Clock::now().time_since_epoch())
      .count();
}

std::string TempDir(const char* tag) {
  char tmpl[256];
  std::snprintf(tmpl, sizeof(tmpl), "/tmp/go2cpp_leveldb_%s_XXXXXX", tag);
  char* dir = ::mkdtemp(tmpl);
  return dir != nullptr ? std::string(dir) : std::string();
}

std::string Key(int i) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "key-%08d", i);
  return std::string(buffer);
}

std::string Value(int i) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "val-%08d", i);
  return std::string(buffer);
}

// 读取 /proc/self/status 中的字段（单位 kB）。
std::uint64_t ReadStatusKb(const char* field) {
  std::FILE* file = std::fopen("/proc/self/status", "r");
  if (file == nullptr) {
    return 0;
  }
  const std::size_t field_len = std::strlen(field);
  char line[512];
  std::uint64_t result = 0;
  while (std::fgets(line, sizeof(line), file) != nullptr) {
    if (std::strncmp(line, field, field_len) == 0) {
      unsigned long long parsed = 0;
      if (std::sscanf(line + field_len, "%llu", &parsed) == 1) {
        result = static_cast<std::uint64_t>(parsed);
      }
      break;
    }
  }
  std::fclose(file);
  return result;
}

std::uint64_t ReadVmRssKb() { return ReadStatusKb("VmRSS:"); }
std::uint64_t ReadVmHwmKb() { return ReadStatusKb("VmHWM:"); }

std::uint64_t DirSizeBytes(const std::string& dir) {
  std::uint64_t total = 0;
  std::error_code error;
  std::filesystem::recursive_directory_iterator iterator(dir, error);
  if (error) {
    return 0;
  }
  for (const auto& entry : iterator) {
    std::error_code entry_error;
    if (entry.is_regular_file(entry_error)) {
      total += entry.file_size(entry_error);
    }
  }
  return total;
}

// ---------------------------------------------------------------------------
// 统一引擎适配：让自研 KV 与 LevelDB 能吃同一份操作序列
// ---------------------------------------------------------------------------

struct Op {
  bool put = true;
  std::string key;
  std::string value;
};

class Engine {
 public:
  virtual ~Engine() = default;
  virtual bool Put(std::string_view key, std::string_view value) = 0;
  virtual bool Get(std::string_view key, std::string* value) = 0;
  virtual bool GetExists(std::string_view key) = 0;
  virtual bool Delete(std::string_view key) = 0;
  virtual bool Write(const std::vector<Op>& ops) = 0;
  virtual std::vector<std::pair<std::string, std::string>> Scan() = 0;
  virtual std::vector<std::pair<std::string, std::string>> ScanFrom(
      std::string_view seek) = 0;
  virtual bool Reopen() = 0;
  virtual void Close() = 0;
  virtual const char* Name() const = 0;
};

class LevelDbEngine : public Engine {
 public:
  explicit LevelDbEngine(const std::string& dir) : dir_(dir) { Open(); }

  bool valid() const { return db_ != nullptr; }

  bool Put(std::string_view key, std::string_view value) override {
    return db_ != nullptr && db_->Put(key, value);
  }
  bool Get(std::string_view key, std::string* value) override {
    return db_ != nullptr && db_->Get(key, value);
  }
  bool GetExists(std::string_view key) override {
    return db_ != nullptr && db_->Get(key, nullptr);
  }
  bool Delete(std::string_view key) override {
    return db_ != nullptr && db_->Delete(key);
  }
  bool Write(const std::vector<Op>& ops) override {
    if (db_ == nullptr) {
      return false;
    }
    ldb::WriteBatch batch;
    for (const auto& op : ops) {
      if (op.put) {
        batch.Put(op.key, op.value);
      } else {
        batch.Delete(op.key);
      }
    }
    return db_->Write(batch);
  }
  std::vector<std::pair<std::string, std::string>> Scan() override {
    std::vector<std::pair<std::string, std::string>> out;
    if (db_ == nullptr) {
      return out;
    }
    auto iterator = db_->NewIterator();
    for (; iterator != nullptr && iterator->Valid(); iterator->Next()) {
      out.emplace_back(std::string(iterator->Key()),
                       std::string(iterator->Value()));
    }
    return out;
  }
  std::vector<std::pair<std::string, std::string>> ScanFrom(
      std::string_view seek) override {
    std::vector<std::pair<std::string, std::string>> out;
    if (db_ == nullptr) {
      return out;
    }
    auto iterator = db_->NewIterator();
    if (iterator == nullptr) {
      return out;
    }
    iterator->Seek(seek);
    for (; iterator->Valid(); iterator->Next()) {
      out.emplace_back(std::string(iterator->Key()),
                       std::string(iterator->Value()));
    }
    return out;
  }
  bool Reopen() override {
    if (db_ != nullptr) {
      db_->Close();
    }
    db_.reset();
    Open();
    return db_ != nullptr;
  }
  void Close() override {
    if (db_ != nullptr) {
      db_->Close();
    }
    db_.reset();
  }
  const char* Name() const override { return "LevelDB"; }

 private:
  void Open() {
    ldb::Options options;
    options.create_if_missing = true;
    db_ = ldb::DB::Open(dir_, options);
  }

  std::string dir_;
  std::unique_ptr<ldb::DB> db_;
};

#ifdef GO2CPP_LEVELDB_HAVE_KV
class KvEngine : public Engine {
 public:
  explicit KvEngine(const std::string& dir) : dir_(dir) { Open(); }

  bool valid() const { return db_ != nullptr; }

  bool Put(std::string_view key, std::string_view value) override {
    return db_ != nullptr && db_->Put(key, value);
  }
  bool Get(std::string_view key, std::string* value) override {
    return db_ != nullptr && db_->Get(key, value);
  }
  bool GetExists(std::string_view key) override {
    return db_ != nullptr && db_->Get(key, nullptr);
  }
  bool Delete(std::string_view key) override {
    return db_ != nullptr && db_->Delete(key);
  }
  bool Write(const std::vector<Op>& ops) override {
    if (db_ == nullptr) {
      return false;
    }
    go2cpp::storage::WriteBatch batch;
    for (const auto& op : ops) {
      if (op.put) {
        batch.Put(op.key, op.value);
      } else {
        batch.Delete(op.key);
      }
    }
    return db_->Write(batch);
  }
  std::vector<std::pair<std::string, std::string>> Scan() override {
    std::vector<std::pair<std::string, std::string>> out;
    if (db_ == nullptr) {
      return out;
    }
    auto iterator = db_->NewIterator();
    for (; iterator != nullptr && iterator->Valid(); iterator->Next()) {
      out.emplace_back(std::string(iterator->key()),
                       std::string(iterator->value()));
    }
    return out;
  }
  std::vector<std::pair<std::string, std::string>> ScanFrom(
      std::string_view seek) override {
    std::vector<std::pair<std::string, std::string>> out;
    if (db_ == nullptr) {
      return out;
    }
    auto iterator = db_->NewIterator();
    if (iterator == nullptr) {
      return out;
    }
    iterator->Seek(seek);
    for (; iterator->Valid(); iterator->Next()) {
      out.emplace_back(std::string(iterator->key()),
                       std::string(iterator->value()));
    }
    return out;
  }
  bool Reopen() override {
    if (db_ != nullptr) {
      db_->Close();
    }
    db_.reset();
    Open();
    return db_ != nullptr;
  }
  void Close() override {
    if (db_ != nullptr) {
      db_->Close();
    }
    db_.reset();
  }
  const char* Name() const override { return "自研 KV"; }

 private:
  void Open() {
    go2cpp::storage::Options options;
    options.create_if_missing = true;
    db_ = go2cpp::storage::KV::Open(dir_, options);
  }

  std::string dir_;
  std::unique_ptr<go2cpp::storage::KV> db_;
};
#endif  // GO2CPP_LEVELDB_HAVE_KV

// ---------------------------------------------------------------------------
// 对拍
// ---------------------------------------------------------------------------

#ifdef GO2CPP_LEVELDB_HAVE_KV
void CheckSameState(Engine& left, Engine& right, const char* phase) {
  const auto left_rows = left.Scan();
  const auto right_rows = right.Scan();
  bool same = left_rows.size() == right_rows.size();
  if (same) {
    for (std::size_t i = 0; i < left_rows.size(); ++i) {
      if (left_rows[i] != right_rows[i]) {
        same = false;
        std::fprintf(stderr, "  对拍差异 phase=%s row=%zu\n", phase, i);
        break;
      }
    }
  }
  if (!same) {
    std::fprintf(stderr, "  对拍差异 phase=%s left=%zu right=%zu\n", phase,
                 left_rows.size(), right_rows.size());
  }
  CHECK(same);

  // 抽样 Get（含命中与未命中）。
  for (int i = 0; i < 2000; i += 37) {
    std::string left_value;
    std::string right_value;
    const bool left_hit = left.Get(Key(i), &left_value);
    const bool right_hit = right.Get(Key(i), &right_value);
    CHECK(left_hit == right_hit);
    if (left_hit && right_hit) {
      CHECK(left_value == right_value);
    }
  }

  // 迭代器 Seek 之后的序列必须一致。
  const auto left_seek = left.ScanFrom(Key(1000));
  const auto right_seek = right.ScanFrom(Key(1000));
  CHECK(left_seek == right_seek);
}

void DifferentialTest() {
  const std::string leveldb_dir = TempDir("ldb_diff");
  const std::string kv_dir = TempDir("kv_diff");
  LevelDbEngine leveldb_engine(leveldb_dir);
  KvEngine kv_engine(kv_dir);
  CHECK(leveldb_engine.valid());
  CHECK(kv_engine.valid());
  if (!leveldb_engine.valid() || !kv_engine.valid()) {
    return;
  }

  // 阶段 1：顺序写 2000 条。
  for (int i = 0; i < 2000; ++i) {
    CHECK(leveldb_engine.Put(Key(i), Value(i)));
    CHECK(kv_engine.Put(Key(i), Value(i)));
  }
  CheckSameState(leveldb_engine, kv_engine, "put-2000");

  // 阶段 2：覆盖写（每 3 条改一条）。
  for (int i = 0; i < 2000; i += 3) {
    const std::string updated = "val2-" + std::to_string(i);
    CHECK(leveldb_engine.Put(Key(i), updated));
    CHECK(kv_engine.Put(Key(i), updated));
  }
  CheckSameState(leveldb_engine, kv_engine, "overwrite");

  // 阶段 3：删除偶数 key。
  for (int i = 0; i < 2000; i += 2) {
    CHECK(leveldb_engine.Delete(Key(i)));
    CHECK(kv_engine.Delete(Key(i)));
  }
  CheckSameState(leveldb_engine, kv_engine, "delete-even");

  // 阶段 4：混合原子批（500 条 put + 250 条 delete）。
  std::vector<Op> ops;
  ops.reserve(750);
  for (int i = 0; i < 500; ++i) {
    ops.push_back(Op{true, Key(3000 + i), "batch-" + std::to_string(i)});
  }
  for (int i = 1; i < 2000; i += 8) {
    ops.push_back(Op{false, Key(i), std::string()});
  }
  CHECK(leveldb_engine.Write(ops));
  CHECK(kv_engine.Write(ops));
  CheckSameState(leveldb_engine, kv_engine, "batch");

  // 阶段 5：空批与重复删除。
  CHECK(leveldb_engine.Write(std::vector<Op>()));
  CHECK(kv_engine.Write(std::vector<Op>()));
  CHECK(leveldb_engine.Delete(Key(9999)));
  CHECK(kv_engine.Delete(Key(9999)));
  CheckSameState(leveldb_engine, kv_engine, "empty-batch");

  // 阶段 6：重开后状态一致（WAL 恢复正确性）。
  CHECK(leveldb_engine.Reopen());
  CHECK(kv_engine.Reopen());
  CheckSameState(leveldb_engine, kv_engine, "reopen");

  // 阶段 7：仅判断存在性的路径（value == nullptr）。
  for (int i = 0; i < 2000; i += 7) {
    CHECK(leveldb_engine.GetExists(Key(i)) == kv_engine.GetExists(Key(i)));
  }

  leveldb_engine.Close();
  kv_engine.Close();
  std::filesystem::remove_all(leveldb_dir);
  std::filesystem::remove_all(kv_dir);
}
#endif  // GO2CPP_LEVELDB_HAVE_KV

// ---------------------------------------------------------------------------
// LevelDB 侧边界 / 错误路径
// ---------------------------------------------------------------------------

void TestLevelDbEdges() {
  const std::string dir = TempDir("ldb_edges");
  CHECK(!dir.empty());

  std::string binary_key("bin\0key", 7);
  {
    ldb::Options options;
    options.create_if_missing = true;
    auto db = ldb::DB::Open(dir, options);
    CHECK(db != nullptr);
    if (db == nullptr) {
      return;
    }

    // 空 key / 空 value / 二进制 key / 1 MiB value。
    std::string value;
    CHECK(db->Put("", "empty-key"));
    CHECK(db->Get("", &value) && value == "empty-key");
    CHECK(db->Put("empty-value", ""));
    CHECK(db->Get("empty-value", &value) && value.empty());
    CHECK(db->Put(binary_key, "bin-value"));
    CHECK(db->Get(binary_key, &value) && value == "bin-value");

    const std::string big(1u << 20, 'x');
    CHECK(db->Put("big", big));
    CHECK(db->Get("big", &value) && value == big);

    // 不存在的 key：false，但 ok() 仍为 true（NotFound 不是错误）。
    CHECK(!db->Get("missing", &value));
    CHECK(db->ok());
    CHECK(!db->Get("missing", nullptr));

    // 删除不存在的 key 视为成功（幂等）。
    CHECK(db->Delete("missing"));

    // 空批直接成功。
    ldb::WriteBatch empty_batch;
    CHECK(empty_batch.Empty());
    CHECK(db->Write(empty_batch));

    // 重复写同一个 key 以不同值。
    CHECK(db->Put("dup", "v1"));
    CHECK(db->Put("dup", "v2"));
    CHECK(db->Get("dup", &value) && value == "v2");

    // 迭代器：SeekToLast / Prev / 越界 Seek。
    auto iterator = db->NewIterator();
    CHECK(iterator != nullptr);
    if (iterator != nullptr) {
      iterator->SeekToLast();
      CHECK(iterator->Valid());
      const std::string last_key(iterator->Key());
      iterator->Prev();
      CHECK(iterator->Valid());
      CHECK(iterator->Key() < last_key);
      iterator->Seek("zzzzz");
      CHECK(!iterator->Valid());
      iterator->SeekToFirst();
      CHECK(iterator->Valid());
      // 第一个 key 是空串（排序最小）。
      CHECK(iterator->Key().empty());
      CHECK(iterator->ok());
    }

    // 统计计数：5 次 Put + 3 次 Delete（空批不计）。
    const ldb::Stats stats = db->GetStats();
    CHECK(stats.puts == 6);
    CHECK(stats.deletes == 1);
    CHECK(stats.gets >= 6);
    CHECK(stats.hits >= 5);
    CHECK(!db->StatsString().empty());
    CHECK(!db->Files().empty());

    // Flush 后数据仍可读。
    db->Flush();
    CHECK(db->Get("big", &value) && value.size() == (1u << 20));

    // LevelDB 约束：迭代器必须早于 DB 关闭销毁。
    iterator.reset();

    // Close 之后：读/写/迭代器全部失败但不崩溃。
    db->Close();
    CHECK(!db->Put("after-close", "x"));
    CHECK(!db->Get("big", &value));
    CHECK(db->NewIterator() == nullptr);
    CHECK(!db->ok());
    CHECK(!db->LastError().empty());
  }

  // error_if_exists：目录已存在且非空。
  {
    ldb::Options options;
    options.error_if_exists = true;
    auto db = ldb::DB::Open(dir, options);
    CHECK(db == nullptr);
    CHECK(!ldb::DB::LastOpenError().empty());
  }

  // 重开验证持久化。
  {
    ldb::Options options;
    auto db = ldb::DB::Open(dir, options);
    CHECK(db != nullptr);
    if (db != nullptr) {
      std::string value;
      CHECK(db->Get("big", &value) && value.size() == (1u << 20));
      CHECK(db->Get(binary_key, &value) && value == "bin-value");
      CHECK(db->Get("", &value) && value == "empty-key");
      db->Close();
    }
  }

  std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------------------
// 迭代器生命周期：DB 先销毁 / 先 Close，迭代器后析构都不能崩溃
// ---------------------------------------------------------------------------

void TestIteratorLifetime() {
  const std::string dir = TempDir("ldb_iter_life");
  CHECK(!dir.empty());
  ldb::Options options;
  options.create_if_missing = true;

  // 场景 1：DB 包装对象销毁后，迭代器仍可用（共享 DbImpl 保活底层 DB）。
  std::unique_ptr<ldb::Iterator> iterator;
  {
    auto db = ldb::DB::Open(dir, options);
    CHECK(db != nullptr);
    if (db == nullptr) {
      return;
    }
    CHECK(db->Put("a", "1"));
    iterator = db->NewIterator();
    CHECK(iterator != nullptr && iterator->Valid());
  }
  CHECK(iterator != nullptr && iterator->Valid());
  CHECK(std::string(iterator->Key()) == "a");
  CHECK(std::string(iterator->Value()) == "1");
  iterator.reset();  // 到这一步底层 DB 才真正关闭

  // 场景 2：显式 Close() 后再析构迭代器：跳过 delete，不崩溃。
  {
    auto db = ldb::DB::Open(dir, options);
    CHECK(db != nullptr);
    if (db == nullptr) {
      return;
    }
    CHECK(db->Put("b", "2"));
    auto late_iterator = db->NewIterator();
    CHECK(late_iterator != nullptr);
    db->Close();
    late_iterator.reset();
  }
  std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------------------
// 崩溃恢复：kill -9 后重开
// ---------------------------------------------------------------------------

void TestCrashRecovery() {
  const std::string dir = TempDir("ldb_crash");
  CHECK(!dir.empty());
  constexpr int kSynced = 500;

  int pipe_fds[2];
  CHECK(::pipe(pipe_fds) == 0);
  const pid_t pid = ::fork();
  if (pid == 0) {
    // 子进程：先 sync 写 kSynced 条（返回即落盘），通知父进程，然后持续
    // 异步写（模拟时刻被 kill 的真实进程）。
    ::close(pipe_fds[0]);
    ldb::Options options;
    options.create_if_missing = true;
    auto db = ldb::DB::Open(dir, options);
    if (db == nullptr) {
      _exit(2);
    }
    ldb::WriteOptions sync_write;
    sync_write.sync = true;
    for (int i = 0; i < kSynced; ++i) {
      if (!db->Put(Key(i), Value(i), sync_write)) {
        _exit(3);
      }
    }
    const char ready = 'R';
    if (::write(pipe_fds[1], &ready, 1) != 1) {
      _exit(4);
    }
    for (std::uint64_t counter = 0;; ++counter) {
      const int index = kSynced + static_cast<int>(counter % 50000);
      if (!db->Put(Key(index), Value(index))) {
        _exit(5);
      }
    }
  }

  ::close(pipe_fds[1]);
  char ready = 0;
  const ssize_t received = ::read(pipe_fds[0], &ready, 1);
  ::close(pipe_fds[0]);
  CHECK(received == 1 && ready == 'R');

  // 给子进程 200ms 继续写，让 WAL 尾部存在未 sync 的数据。
  ::usleep(200 * 1000);
  CHECK(::kill(pid, SIGKILL) == 0);
  int status = 0;
  CHECK(::waitpid(pid, &status, 0) == pid);
  CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);

  // 重开：已 sync 的写必须全部恢复。
  ldb::Options options;
  options.create_if_missing = true;
  auto db = ldb::DB::Open(dir, options);
  CHECK(db != nullptr);
  if (db == nullptr) {
    std::filesystem::remove_all(dir);
    return;
  }

  int missing = 0;
  std::string value;
  for (int i = 0; i < kSynced; ++i) {
    if (!db->Get(Key(i), &value) || value != Value(i)) {
      ++missing;
    }
  }
  CHECK(missing == 0);
  std::printf("  崩溃恢复：已 sync 的 %d 条全部恢复（missing=%d）\n", kSynced,
              missing);

  // 重开后仍可正常读写，全量扫描不报错。
  CHECK(db->Put("after-crash", "ok"));
  CHECK(db->Get("after-crash", &value) && value == "ok");
  auto iterator = db->NewIterator();
  std::size_t count = 0;
  for (; iterator != nullptr && iterator->Valid(); iterator->Next()) {
    ++count;
  }
  CHECK(iterator != nullptr && iterator->ok());
  CHECK(count >= static_cast<std::size_t>(kSynced));
  // LevelDB 约束：迭代器必须早于 DB 关闭销毁。
  iterator.reset();
  db->Close();
  std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------------------
// 基准
// ---------------------------------------------------------------------------

struct BenchResult {
  std::uint64_t n = 0;
  double put_ns = 0;
  double get_ns = 0;
  std::uint64_t rss_peak_kb = 0;
  std::uint64_t disk_bytes = 0;
  int failed = 0;
};

int MakeKey(char* buffer, std::uint64_t i) {
  return std::snprintf(buffer, 32, "key-%016llu",
                       static_cast<unsigned long long>(i));
}

int MakeValue(char* buffer, std::uint64_t i) {
  return std::snprintf(buffer, 32, "value-%016llu",
                       static_cast<unsigned long long>(i));
}

void ShuffleOrder(std::vector<std::uint64_t>* order) {
  std::uint64_t state = 88172645463325252ull;
  const std::size_t n = order->size();
  for (std::size_t i = n; i > 1; --i) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    std::swap((*order)[i - 1], (*order)[state % i]);
  }
}

BenchResult RunLevelDbBench(std::uint64_t n, const std::string& dir) {
  BenchResult result;
  result.n = n;
  ldb::Options options;
  options.create_if_missing = true;
  options.enable_stats = false;  // 与裸库同口径
  auto db = ldb::DB::Open(dir, options);
  if (db == nullptr) {
    result.failed = 1;
    return result;
  }

  std::vector<std::uint64_t> order(n);
  for (std::uint64_t i = 0; i < n; ++i) {
    order[i] = i;
  }
  ShuffleOrder(&order);

  char key[32];
  char value[32];
  const double put_start = NowNs();
  for (std::uint64_t i = 0; i < n; ++i) {
    const int key_len = MakeKey(key, i);
    const int value_len = MakeValue(value, i);
    if (!db->Put(std::string_view(key, static_cast<std::size_t>(key_len)),
                 std::string_view(value,
                                  static_cast<std::size_t>(value_len)))) {
      result.failed = 1;
      break;
    }
  }
  const double put_end = NowNs();

  std::string scratch;
  const double get_start = NowNs();
  for (std::uint64_t i = 0; i < n; ++i) {
    const int key_len = MakeKey(key, order[i]);
    if (!db->Get(std::string_view(key, static_cast<std::size_t>(key_len)),
                 &scratch)) {
      result.failed = 1;
      break;
    }
  }
  const double get_end = NowNs();

  result.put_ns = (put_end - put_start) / static_cast<double>(n);
  result.get_ns = (get_end - get_start) / static_cast<double>(n);
  result.rss_peak_kb = ReadVmHwmKb();
  result.disk_bytes = DirSizeBytes(dir);
  db->Close();
  return result;
}

#ifdef GO2CPP_LEVELDB_HAVE_KV
BenchResult RunKvBench(std::uint64_t n, const std::string& dir) {
  BenchResult result;
  result.n = n;
  go2cpp::storage::Options options;
  options.create_if_missing = true;
  auto db = go2cpp::storage::KV::Open(dir, options);
  if (db == nullptr) {
    result.failed = 1;
    return result;
  }

  std::vector<std::uint64_t> order(n);
  for (std::uint64_t i = 0; i < n; ++i) {
    order[i] = i;
  }
  ShuffleOrder(&order);

  char key[32];
  char value[32];
  const double put_start = NowNs();
  for (std::uint64_t i = 0; i < n; ++i) {
    const int key_len = MakeKey(key, i);
    const int value_len = MakeValue(value, i);
    if (!db->Put(std::string_view(key, static_cast<std::size_t>(key_len)),
                 std::string_view(value,
                                  static_cast<std::size_t>(value_len)))) {
      result.failed = 1;
      break;
    }
  }
  const double put_end = NowNs();

  std::string scratch;
  const double get_start = NowNs();
  for (std::uint64_t i = 0; i < n; ++i) {
    const int key_len = MakeKey(key, order[i]);
    if (!db->Get(std::string_view(key, static_cast<std::size_t>(key_len)),
                 &scratch)) {
      result.failed = 1;
      break;
    }
  }
  const double get_end = NowNs();

  result.put_ns = (put_end - put_start) / static_cast<double>(n);
  result.get_ns = (get_end - get_start) / static_cast<double>(n);
  result.rss_peak_kb = ReadVmHwmKb();
  result.disk_bytes = DirSizeBytes(dir);
  db->Close();
  return result;
}
#endif  // GO2CPP_LEVELDB_HAVE_KV

bool WriteAll(int fd, const void* data, std::size_t size) {
  const char* cursor = static_cast<const char*>(data);
  while (size > 0) {
    const ssize_t written = ::write(fd, cursor, size);
    if (written <= 0) {
      return false;
    }
    cursor += written;
    size -= static_cast<std::size_t>(written);
  }
  return true;
}

bool ReadAll(int fd, void* data, std::size_t size) {
  char* cursor = static_cast<char*>(data);
  while (size > 0) {
    const ssize_t received = ::read(fd, cursor, size);
    if (received <= 0) {
      return false;
    }
    cursor += received;
    size -= static_cast<std::size_t>(received);
  }
  return true;
}

// 每个引擎跑在独立子进程：VmHWM 互不污染，page cache 也按引擎隔离。
BenchResult RunBenchChild(bool use_leveldb, std::uint64_t n,
                          const std::string& dir) {
  int pipe_fds[2];
  if (::pipe(pipe_fds) != 0) {
    BenchResult failed;
    failed.failed = 1;
    return failed;
  }
  const pid_t pid = ::fork();
  if (pid == 0) {
    ::close(pipe_fds[0]);
    BenchResult result;
#ifdef GO2CPP_LEVELDB_HAVE_KV
    result = use_leveldb ? RunLevelDbBench(n, dir) : RunKvBench(n, dir);
#else
    (void)use_leveldb;
    result = RunLevelDbBench(n, dir);
#endif
    WriteAll(pipe_fds[1], &result, sizeof(result));
    ::close(pipe_fds[1]);
    _exit(result.failed != 0 ? 1 : 0);
  }
  ::close(pipe_fds[1]);
  BenchResult result{};
  const bool received = ReadAll(pipe_fds[0], &result, sizeof(result));
  ::close(pipe_fds[0]);
  int status = 0;
  ::waitpid(pid, &status, 0);
  if (!received) {
    result.failed = 1;
  }
  return result;
}

void PrintBenchRow(const char* name, const BenchResult& result) {
  if (result.failed != 0) {
    std::printf("%-10s FAILED\n", name);
    return;
  }
  const double put_ops = 1e9 / result.put_ns;
  const double get_ops = 1e9 / result.get_ns;
  std::printf("%-10s %10.1f ns %12.0f/s  %10.1f ns %12.0f/s  %8.1f MiB %8.1f MiB\n",
              name, result.put_ns, put_ops, result.get_ns, get_ops,
              static_cast<double>(result.rss_peak_kb) / 1024.0,
              static_cast<double>(result.disk_bytes) / (1024.0 * 1024.0));
}

void RunBench(std::uint64_t n) {
  std::printf("== %llu 条 Put/Get 对比（单线程，key 20B / value 22B）==\n",
              static_cast<unsigned long long>(n));
  const std::string leveldb_dir = TempDir("ldb_bench");
  const std::string kv_dir = TempDir("kv_bench");
  CHECK(!leveldb_dir.empty());
  CHECK(!kv_dir.empty());

  const double start = NowNs();
  const BenchResult leveldb_result = RunBenchChild(true, n, leveldb_dir);
#ifdef GO2CPP_LEVELDB_HAVE_KV
  const BenchResult kv_result = RunBenchChild(false, n, kv_dir);
#endif
  const double elapsed = NowNs() - start;

  std::printf("%-10s %14s %12s  %14s %12s  %10s %10s\n", "引擎", "Put ns/op",
              "Put ops/s", "Get ns/op", "Get ops/s", "VmHWM", "磁盘");
  PrintBenchRow("LevelDB", leveldb_result);
#ifdef GO2CPP_LEVELDB_HAVE_KV
  PrintBenchRow("自研 KV", kv_result);
#else
  std::printf("自研 KV    未编译（GO2CPP_WITH_KV=OFF），跳过对拍\n");
#endif
  std::printf("总耗时 %.1f s\n", elapsed / 1e9);

  CHECK(leveldb_result.failed == 0);
#ifdef GO2CPP_LEVELDB_HAVE_KV
  CHECK(kv_result.failed == 0);
#endif
  std::filesystem::remove_all(leveldb_dir);
  std::filesystem::remove_all(kv_dir);
}

// ---------------------------------------------------------------------------
// 1e8 RSS 有界验证
// ---------------------------------------------------------------------------

void RunMemTest(std::uint64_t n) {
  std::printf("== %llu 条写入的 RSS 有界验证（8B key / 8B value）==\n",
              static_cast<unsigned long long>(n));
  const std::string dir = TempDir("ldb_mem");
  CHECK(!dir.empty());

  ldb::Options options;
  options.create_if_missing = true;
  options.enable_stats = false;
  options.write_buffer_size = 8u << 20;
  options.block_cache_size = 8u << 20;
  auto db = ldb::DB::Open(dir, options);
  CHECK(db != nullptr);
  if (db == nullptr) {
    return;
  }

  const std::uint64_t baseline_kb = ReadVmRssKb();
  std::uint64_t peak_kb = baseline_kb;
  const double start = NowNs();
  std::uint64_t written = 0;
  for (std::uint64_t i = 0; i < n; ++i) {
    const std::string_view key(reinterpret_cast<const char*>(&i), sizeof(i));
    const std::string_view value(reinterpret_cast<const char*>(&i), sizeof(i));
    if (!db->Put(key, value)) {
      std::fprintf(stderr, "Put 失败于 i=%llu\n",
                   static_cast<unsigned long long>(i));
      break;
    }
    ++written;
    if ((i + 1) % (1ull << 20) == 0) {
      const std::uint64_t rss = ReadVmRssKb();
      if (rss > peak_kb) {
        peak_kb = rss;
      }
      if ((i + 1) % (10ull << 20) == 0) {
        std::printf("  已写 %llu 条  RSS=%.1f MiB  用时 %.1f s\n",
                    static_cast<unsigned long long>(i + 1),
                    static_cast<double>(rss) / 1024.0, (NowNs() - start) / 1e9);
        std::fflush(stdout);
      }
    }
  }
  const double elapsed = NowNs() - start;
  const std::uint64_t hwm_kb = ReadVmHwmKb();
  const std::uint64_t disk_bytes = DirSizeBytes(dir);

  std::printf("写入 %llu 条，用时 %.1f s，%.0f ops/s\n",
              static_cast<unsigned long long>(written), elapsed / 1e9,
              static_cast<double>(written) / (elapsed / 1e9));
  std::printf("RSS 基线 %.1f MiB，采样峰值 %.1f MiB，VmHWM %.1f MiB，磁盘 %.1f MiB\n",
              static_cast<double>(baseline_kb) / 1024.0,
              static_cast<double>(peak_kb) / 1024.0,
              static_cast<double>(hwm_kb) / 1024.0,
              static_cast<double>(disk_bytes) / (1024.0 * 1024.0));

  // 随机读 10 万条校验（数据必须真的是写进去了，不是只测了内存）。
  std::uint64_t state = 0x9e3779b97f4a7c15ull;
  std::uint64_t hits = 0;
  std::uint64_t checked = 0;
  const std::uint64_t samples = 100000;
  for (std::uint64_t i = 0; i < samples; ++i) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    const std::uint64_t index = state % written;
    std::string value;
    if (db->Get(std::string_view(reinterpret_cast<const char*>(&index),
                                 sizeof(index)),
                &value) &&
        value.size() == sizeof(index) &&
        std::memcmp(value.data(), &index, sizeof(index)) == 0) {
      ++hits;
    }
    ++checked;
  }
  std::printf("随机读校验：%llu/%llu 命中\n",
              static_cast<unsigned long long>(hits),
              static_cast<unsigned long long>(checked));
  CHECK(hits == checked);

  // 核心断言：写入了 1e8 条（约 2.4GB 逻辑数据）而 RSS 峰值必须远小于数据量。
  const std::uint64_t rss_limit_kb = 512ull * 1024ull;
  CHECK(peak_kb < rss_limit_kb);
  CHECK(hwm_kb < rss_limit_kb);
  std::printf("断言：RSS 峰值 < 512 MiB —— %s\n",
              (peak_kb < rss_limit_kb && hwm_kb < rss_limit_kb) ? "PASS"
                                                                : "FAIL");

  db->Close();
  std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

void TestLevelDbBasics() {
  const std::string dir = TempDir("ldb_basic");
  CHECK(!dir.empty());
  ldb::Options options;
  options.create_if_missing = true;
  auto db = ldb::DB::Open(dir, options);
  CHECK(db != nullptr);
  if (db == nullptr) {
    return;
  }
  std::string value;
  CHECK(db->Get("a", &value) == false);
  CHECK(db->Put("a", "1"));
  CHECK(db->Put("b", "2"));
  CHECK(db->Get("a", &value) && value == "1");
  CHECK(db->Get("b", &value) && value == "2");
  CHECK(db->Put("a", "3"));
  CHECK(db->Get("a", &value) && value == "3");
  CHECK(db->Delete("a"));
  CHECK(db->Get("a", &value) == false);

  ldb::WriteBatch batch;
  batch.Put("c", "3");
  batch.Put("d", "4");
  batch.Delete("b");
  CHECK(batch.Count() == 3);
  CHECK(db->Write(batch));
  CHECK(db->Get("c", &value) && value == "3");
  CHECK(db->Get("d", &value) && value == "4");
  CHECK(db->Get("b", &value) == false);

  auto iterator = db->NewIterator();
  CHECK(iterator != nullptr);
  std::vector<std::string> keys;
  if (iterator != nullptr) {
    for (; iterator->Valid(); iterator->Next()) {
      keys.emplace_back(iterator->Key());
    }
  }
  CHECK((keys == std::vector<std::string>{"c", "d"}));
  // LevelDB 约束：迭代器必须早于 DB 关闭销毁。
  iterator.reset();
  db->Close();
  std::filesystem::remove_all(dir);
}

}  // namespace

int main(int argc, char** argv) {
  bool bench = false;
  bool mem = false;
  std::uint64_t n = 10000000;
  std::uint64_t mem_n = 100000000;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--bench") == 0) {
      bench = true;
    } else if (std::strcmp(argv[i], "--mem") == 0) {
      mem = true;
    } else if (std::strncmp(argv[i], "--n=", 4) == 0) {
      n = std::strtoull(argv[i] + 4, nullptr, 10);
      mem_n = n;
    }
  }

  if (bench) {
    RunBench(n);
    return g_failures == 0 ? 0 : 1;
  }
  if (mem) {
    RunMemTest(mem_n);
    return g_failures == 0 ? 0 : 1;
  }

  std::printf("[1/3] LevelDB 基础/边界\n");
  TestLevelDbBasics();
  TestLevelDbEdges();
  TestIteratorLifetime();
  std::printf("[2/3] 与自研 KV 对拍\n");
#ifdef GO2CPP_LEVELDB_HAVE_KV
  DifferentialTest();
#else
  std::printf("  跳过：GO2CPP_WITH_KV=OFF\n");
#endif
  std::printf("[3/3] 崩溃恢复（kill -9）\n");
  TestCrashRecovery();

  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
