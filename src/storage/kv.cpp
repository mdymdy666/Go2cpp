// Copyright 2026 Go2Cpp Authors. All rights reserved.
// KV 引擎主体：写路径（WAL + MemTable + 刷盘 + 合并）、读路径（无锁）、
// 崩溃恢复、epoch 延迟回收、快照式迭代器。
//
// 读路径设计（性能关键）：
//   1. 先查 mutable MemTable —— 只做一次 atomic load，命中则完全不碰锁/
//      读者注册/epoch，热读开销 = 哈希 + 1 次 cache miss；
//   2. 未命中才进入慢路径：epoch Guard 保护下读 immutable + SSTable；
//   3. SSTable 读是 mmap 上的两次二分 + 一次 value memcpy，无 read() 系统调用。
//
// 写路径：一把写锁串行化 WAL 追加与内存更新；fsync 用 group commit 放到锁外，
// 并发 sync 写只付一次 fdatasync 的钱。刷盘/合并在写锁内完成（与 LevelDB 的
// 后台 compaction 不同，换来实现简单和读路径的强一致）。合并只挑「大小相近的
// 相邻段」：固定合并最旧两段会让最旧的大段被之后每个新段反复重写（1e7 条实测
// 17.7 GiB、写放大 36x、Put 16 万/s）。

#include "go2cpp/storage/kv.hpp"

#include "go2cpp/logging/log.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "format.hpp"
#include "memtable.hpp"
#include "sstable.hpp"
#include "wal.hpp"

namespace go2cpp {
namespace storage {

namespace {

// ---------------------------------------------------------------------------
// 工具
// ---------------------------------------------------------------------------

inline int Compare(std::string_view a, std::string_view b) {
  const std::size_t n = a.size() < b.size() ? a.size() : b.size();
  if (n > 0) {
    const int r = std::memcmp(a.data(), b.data(), n);
    if (r != 0) {
      return r;
    }
  }
  if (a.size() < b.size()) {
    return -1;
  }
  if (a.size() > b.size()) {
    return 1;
  }
  return 0;
}

std::string JoinPath(const std::string& dir, const std::string& name) {
  if (!dir.empty() && (dir.back() == '/' || dir.back() == '\\')) {
    return dir + name;
  }
  return dir + "/" + name;
}

std::string FileName(std::uint64_t no, const char* ext) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%06llu.%s",
                static_cast<unsigned long long>(no), ext);
  return std::string(buf);
}

bool ParseFileName(const std::string& name, std::uint64_t* no,
                   std::string* ext) {
  const std::size_t dot = name.find('.');
  if (dot == std::string::npos || dot == 0) {
    return false;
  }
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < dot; ++i) {
    const char c = name[i];
    if (c < '0' || c > '9') {
      return false;
    }
    value = value * 10 + static_cast<std::uint64_t>(c - '0');
    if (value > (1ull << 48)) {
      return false;
    }
  }
  *no = value;
  *ext = name.substr(dot + 1);
  return true;
}

// ---------------------------------------------------------------------------
// 分片计数器：读热路径按线程分片，避免多线程读同一个 cache line。
// ---------------------------------------------------------------------------

class Counters {
 public:
  enum Field : std::size_t {
    kPuts,
    kGets,
    kHits,
    kDeletes,
    kWriteBatches,
    kWalRecords,
    kWalBytes,
    kWalSyncs,
    kMemtableFlushes,
    kCompactions,
    kMemtableHits,
    kImmutableHits,
    kSstableHits,
    kBloomNegatives,
    kBytesRead,
    kBytesWritten,
    kRecoverRecords,
    kRecoverBytes,
    kNumFields,
  };

  Counters() {
    for (std::size_t i = 0; i < kShards; ++i) {
      for (std::size_t j = 0; j < kNumFields; ++j) {
        shards_[i].v[j].store(0, std::memory_order_relaxed);
      }
    }
  }

  void Add(Field field, std::uint64_t n = 1) const {
    shards_[TlsShard()].v[field].fetch_add(n, std::memory_order_relaxed);
  }

  Stats Snapshot() const {
    Stats s;
    const std::uint64_t* sum = Sum();
    s.puts = sum[kPuts];
    s.gets = sum[kGets];
    s.hits = sum[kHits];
    s.deletes = sum[kDeletes];
    s.write_batches = sum[kWriteBatches];
    s.wal_records = sum[kWalRecords];
    s.wal_bytes = sum[kWalBytes];
    s.wal_syncs = sum[kWalSyncs];
    s.memtable_flushes = sum[kMemtableFlushes];
    s.compactions = sum[kCompactions];
    s.memtable_hits = sum[kMemtableHits];
    s.immutable_hits = sum[kImmutableHits];
    s.sstable_hits = sum[kSstableHits];
    s.bloom_negatives = sum[kBloomNegatives];
    s.bytes_read = sum[kBytesRead];
    s.bytes_written = sum[kBytesWritten];
    s.recover_records = sum[kRecoverRecords];
    s.recover_bytes = sum[kRecoverBytes];
    return s;
  }

 private:
  static constexpr std::size_t kShards = 8;

  struct alignas(64) Shard {
    std::atomic<std::uint64_t> v[kNumFields];
  };

  static std::size_t TlsShard() {
    static std::atomic<std::size_t> next{0};
    thread_local std::size_t shard =
        next.fetch_add(1, std::memory_order_relaxed) % kShards;
    return shard;
  }

  const std::uint64_t* Sum() const {
    thread_local std::uint64_t sum[kNumFields];
    for (std::size_t i = 0; i < kNumFields; ++i) {
      std::uint64_t total = 0;
      for (std::size_t j = 0; j < kShards; ++j) {
        total += shards_[j].v[i].load(std::memory_order_relaxed);
      }
      sum[i] = total;
    }
    return sum;
  }

  mutable Shard shards_[kShards];
};

// ---------------------------------------------------------------------------
// Epoch / 延迟回收
//
// 读者在慢路径上注册一个「当前 epoch」到自己的槽位（每个线程独占一条
// cache line），退出时恢复。回收者推进 epoch 后扫描所有槽位，只要最小活跃
// epoch > 被回收对象的 retire_epoch，就说明所有可能持有该对象的读者都已退出。
// 读热路径不经过这里（只有 mutable 未命中才注册）。
// ---------------------------------------------------------------------------

class EpochManager {
 public:
  static constexpr std::uint64_t kIdle = 0;
  static constexpr std::size_t kMaxSlots = 128;

  struct alignas(64) Slot {
    std::atomic<std::uint64_t> value{kIdle};
  };

  class Guard {
   public:
    explicit Guard(EpochManager& m) : m_(m) {
      Slot* s = m_.AcquireSlot();
      prev_ = s->value.load(std::memory_order_relaxed);
      s->value.store(m_.epoch_.load(std::memory_order_acquire),
                     std::memory_order_release);
      slot_ = s;
    }
    ~Guard() { slot_->value.store(prev_, std::memory_order_release); }

    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;

   private:
    EpochManager& m_;
    Slot* slot_ = nullptr;
    std::uint64_t prev_ = kIdle;
  };

  static EpochManager& Global() {
    // 故意不析构：线程退出时 thread_local 槽还要写回自己的 value。
    static EpochManager* instance = new EpochManager();
    return *instance;
  }

  void Retire(std::function<void()> fn) {
    std::lock_guard<std::mutex> lk(mu_);
    const std::uint64_t e = epoch_.load(std::memory_order_relaxed);
    epoch_.store(e + 1, std::memory_order_release);
    retired_.push_back({std::move(fn), e});
  }

  void Reclaim() {
    std::vector<Retired> free_now;
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (retired_.empty()) {
        return;
      }
      std::uint64_t min_active = UINT64_MAX;
      const std::uint32_t n =
          std::min(next_slot_.load(std::memory_order_relaxed),
                   static_cast<std::uint32_t>(kMaxSlots));
      for (std::uint32_t i = 0; i < n; ++i) {
        const std::uint64_t v = slots_[i].value.load(std::memory_order_acquire);
        if (v != kIdle && v < min_active) {
          min_active = v;
        }
      }
      auto it = retired_.begin();
      while (it != retired_.end()) {
        if (it->epoch < min_active) {
          free_now.push_back(std::move(*it));
          it = retired_.erase(it);
        } else {
          ++it;
        }
      }
    }
    for (auto& r : free_now) {
      r.fn();
    }
  }

 private:
  struct Retired {
    std::function<void()> fn;
    std::uint64_t epoch;
  };

  EpochManager() = default;

  struct Tls {
    Slot* slot = nullptr;
    ~Tls() {
      if (slot != nullptr) {
        slot->value.store(kIdle, std::memory_order_release);
      }
    }
  };

  static Tls& TlsRef() {
    static thread_local Tls tls;
    return tls;
  }

  Slot* AcquireSlot() {
    Tls& tls = TlsRef();
    if (tls.slot != nullptr) {
      return tls.slot;
    }
    const std::uint32_t idx =
        next_slot_.fetch_add(1, std::memory_order_relaxed);
    tls.slot = &slots_[idx < kMaxSlots ? idx : 0];
    return tls.slot;
  }

  std::atomic<std::uint64_t> epoch_{1};
  Slot slots_[kMaxSlots];
  std::atomic<std::uint32_t> next_slot_{1};  // 0 号留给超限兜底
  std::mutex mu_;
  std::vector<Retired> retired_;
};

// ---------------------------------------------------------------------------
// 快照：immutable MemTable + SSTable 列表（新 -> 旧）
// ---------------------------------------------------------------------------

struct Snapshot {
  std::vector<std::shared_ptr<MemTable>> immutables;
  std::vector<std::shared_ptr<SSTable>> sstables;
};

}  // namespace

// ---------------------------------------------------------------------------
// VectorIterator
// ---------------------------------------------------------------------------

class VectorIterator : public Iterator {
 public:
  VectorIterator(std::vector<std::string> keys, std::vector<std::string> values)
      : keys_(std::move(keys)), values_(std::move(values)) {}

  bool Valid() const override { return index_ < keys_.size(); }
  std::string_view key() const override { return keys_[index_]; }
  std::string_view value() const override { return values_[index_]; }
  void Next() override {
    if (index_ < keys_.size()) {
      ++index_;
    }
  }
  void Seek(std::string_view target) override {
    index_ = static_cast<std::size_t>(
        std::lower_bound(keys_.begin(), keys_.end(), target) - keys_.begin());
  }
  void SeekToFirst() override { index_ = 0; }
  void SeekToLast() override {
    index_ = keys_.empty() ? 0 : keys_.size() - 1;
  }

 private:
  std::vector<std::string> keys_;
  std::vector<std::string> values_;
  std::size_t index_ = 0;
};

// ---------------------------------------------------------------------------
// WriteBatch
// ---------------------------------------------------------------------------

WriteBatch::WriteBatch() { repr_.resize(4, '\0'); }
WriteBatch::~WriteBatch() = default;

void WriteBatch::Put(std::string_view key, std::string_view value) {
  repr_.push_back(static_cast<char>(internal::ValueType::kValue));
  char buf[16];
  char* end =
      internal::EncodeVarint32(buf, static_cast<std::uint32_t>(key.size()));
  repr_.append(buf, static_cast<std::size_t>(end - buf));
  if (!key.empty()) {
    repr_.append(key.data(), key.size());
  }
  end = internal::EncodeVarint32(buf, static_cast<std::uint32_t>(value.size()));
  repr_.append(buf, static_cast<std::size_t>(end - buf));
  if (!value.empty()) {
    repr_.append(value.data(), value.size());
  }
  ++count_;
  internal::EncodeFixed32(&repr_[0], static_cast<std::uint32_t>(count_));
}

void WriteBatch::Delete(std::string_view key) {
  repr_.push_back(static_cast<char>(internal::ValueType::kDeletion));
  char buf[16];
  char* end =
      internal::EncodeVarint32(buf, static_cast<std::uint32_t>(key.size()));
  repr_.append(buf, static_cast<std::size_t>(end - buf));
  if (!key.empty()) {
    repr_.append(key.data(), key.size());
  }
  end = internal::EncodeVarint32(buf, 0);
  repr_.append(buf, static_cast<std::size_t>(end - buf));
  ++count_;
  internal::EncodeFixed32(&repr_[0], static_cast<std::uint32_t>(count_));
}

void WriteBatch::Clear() {
  repr_.resize(4, '\0');
  count_ = 0;
}

// ---------------------------------------------------------------------------
// KV::Impl
// ---------------------------------------------------------------------------

class KV::Impl {
 public:
  Impl(std::string dir, Options options)
      : opt_(options), dir_(std::move(dir)), epoch_(EpochManager::Global()) {}

  ~Impl() {
    Close();
    const Snapshot* s = snapshot_.exchange(nullptr, std::memory_order_acq_rel);
    delete s;
  }

  bool Open();
  bool Get(std::string_view key, std::string* value) const;
  bool Put(std::string_view key, std::string_view value);
  bool Delete(std::string_view key);
  bool Write(const WriteBatch& batch);
  std::unique_ptr<Iterator> NewIterator();
  void Flush();
  void Close();

  bool ok() const { return last_error_.empty(); }
  const std::string& LastError() const { return last_error_; }
  Stats GetStats() const;
  std::string StatsString() const;
  std::vector<std::string> Files() const;

 private:
  struct EntryRef {
    std::string_view key;
    std::string_view value;
    internal::ValueType type;
  };

  bool Recover();
  bool ApplyBatch(std::string_view data);
  bool WriteLocked(std::string_view data);
  bool WriteSingle(std::string_view key, std::string_view value,
                   internal::ValueType type);
  bool FlushLocked();
  void MaybeCompactLocked();
  void PublishSnapshot(std::unique_ptr<Snapshot> next);
  bool GroupSync(std::uint64_t seq);
  std::string PathFor(std::uint64_t no, const char* ext) const;
  void SetError(const std::string& what);

  Options opt_;
  std::string dir_;
  mutable std::mutex mu_;
  std::unique_ptr<MemTable> mutable_;
  std::atomic<MemTable*> mutable_ptr_{nullptr};
  std::atomic<const Snapshot*> snapshot_{nullptr};
  EpochManager& epoch_;
  std::unique_ptr<WalWriter> wal_;
  std::string wal_path_;
  std::uint64_t next_file_no_ = 1;
  std::atomic<std::uint64_t> issued_{0};
  std::atomic<std::uint64_t> sync_done_{0};
  std::atomic<bool> sync_failed_{false};
  std::mutex sync_mu_;
  mutable Counters counters_;
  std::string last_error_;
  std::string scratch_;
  bool closed_ = false;
};

std::string KV::Impl::PathFor(std::uint64_t no, const char* ext) const {
  return JoinPath(dir_, FileName(no, ext));
}

void KV::Impl::SetError(const std::string& what) {
  if (last_error_.empty()) {
    last_error_ = what;
  }
}

bool KV::Impl::Open() {
  std::error_code ec;
  if (opt_.create_if_missing) {
    std::filesystem::create_directories(dir_, ec);
  }
  if (!std::filesystem::is_directory(dir_)) {
    SetError("directory not found: " + dir_);
    return false;
  }
  if (opt_.error_if_exists && !Files().empty()) {
    SetError("directory not empty: " + dir_);
    return false;
  }
  return Recover();
}

bool KV::Impl::Recover() {
  std::vector<std::pair<std::uint64_t, std::string>> sst_files;
  std::vector<std::pair<std::uint64_t, std::string>> log_files;

  DIR* dir = ::opendir(dir_.c_str());
  if (dir == nullptr) {
    SetError("opendir failed");
    return false;
  }
  struct dirent* ent;
  while ((ent = ::readdir(dir)) != nullptr) {
    const std::string name(ent->d_name);
    if (name == "." || name == "..") {
      continue;
    }
    std::uint64_t no = 0;
    std::string ext;
    if (!ParseFileName(name, &no, &ext)) {
      continue;
    }
    if (ext == "sst") {
      sst_files.emplace_back(no, name);
    } else if (ext == "log") {
      log_files.emplace_back(no, name);
    }
  }
  ::closedir(dir);

  std::sort(sst_files.begin(), sst_files.end());
  std::sort(log_files.begin(), log_files.end());

  auto snap = std::make_unique<Snapshot>();
  for (const auto& f : sst_files) {
    auto table = SSTable::Open(JoinPath(dir_, f.second), opt_);
    if (table == nullptr) {
      SetError("corrupt sstable: " + f.second);
      return false;
    }
    snap->sstables.push_back(std::move(table));
  }
  std::reverse(snap->sstables.begin(), snap->sstables.end());

  // 恢复期的 MemTable 给双倍预算：一个 WAL 里的记录可能刚好超过常规预算，
  // 若在这里装不下会误判为损坏。
  Options recover_opt = opt_;
  recover_opt.memtable_size = opt_.memtable_size * 2;
  mutable_ = std::make_unique<MemTable>(recover_opt);
  std::uint64_t recovered_records = 0;
  std::uint64_t recovered_bytes = 0;
  for (const auto& f : log_files) {
    WalReader reader(opt_);
    // 打开失败（权限/IO）不能静默跳过：跳过等于把这段已确认写丢弃，
    // 且 Open 仍返回成功，调用方无从得知。
    if (!reader.Open(JoinPath(dir_, f.second))) {
      SetError("cannot open wal: " + f.second);
      return false;
    }
    std::string record;
    while (reader.ReadRecord(&record)) {
      if (!ApplyBatch(record)) {
        SetError("corrupt wal batch: " + f.second);
        return false;
      }
      ++recovered_records;
      recovered_bytes += record.size();
    }
    if (reader.corrupted()) {
      // 半写尾部是崩溃恢复的正常现象：截断点之前的记录已重放，之后的
      // 记录未确认，丢弃即可；但必须留下可见痕迹，便于排查数据少了一块。
      GO2CPP_LOG_WARN(GO2CPP_LOG_NAME("storage"))
          << "WAL " << f.second << " has a partial/corrupt tail; "
          << "records after the damage are dropped";
    }
  }
  if (recovered_records > 0) {
    counters_.Add(Counters::kRecoverRecords, recovered_records);
    counters_.Add(Counters::kRecoverBytes, recovered_bytes);
  }

  std::uint64_t max_no = 0;
  if (!sst_files.empty()) {
    max_no = std::max(max_no, sst_files.back().first);
  }
  if (!log_files.empty()) {
    max_no = std::max(max_no, log_files.back().first);
  }
  next_file_no_ = max_no + 1;

  // 重放期用双倍预算，结束后回到常规预算：否则恢复出来的表会一直以
  // 双倍阈值判断刷盘，1e6 条数据可能永远不落盘（实测 flushes=0）。
  mutable_->SetBudget(opt_.memtable_size);
  mutable_ptr_.store(mutable_.get(), std::memory_order_release);
  snapshot_.store(snap.release(), std::memory_order_release);

  wal_ = std::make_unique<WalWriter>(opt_);
  wal_path_ = PathFor(next_file_no_++, "log");
  if (!wal_->Open(wal_path_)) {
    SetError("cannot create wal: " + wal_path_);
    return false;
  }

  // 重放过的 WAL 必须先落成 SST，再删除旧文件。旧 log 留在目录里会在下次
  // 重启时被再次重放，而它携带的是更旧的版本，会盖掉之后写入 SST 的新值
  // （实测：崩溃恢复 v1 → Put v2 + Flush → 重启读回 v1）。
  // 先 flush 再删：SST 内容就是这些 log 的重放结果，即使删前崩溃，再次
  // 重放也是幂等的；反过来先删后 flush 则会在崩溃窗口丢数据。
  if (recovered_records > 0 && !FlushLocked()) {
    SetError("flush recovered wal failed");
    return false;
  }
  for (const auto& f : log_files) {
    const std::string path = JoinPath(dir_, f.second);
    if (::unlink(path.c_str()) != 0 && errno != ENOENT) {
      SetError("cannot remove recovered wal: " + f.second);
      return false;
    }
  }
  return true;
}

bool KV::Impl::ApplyBatch(std::string_view data) {
  if (data.size() < 4) {
    return false;
  }
  const char* p = data.data();
  const char* limit = p + data.size();
  const std::uint32_t count = internal::DecodeFixed32(p);
  p += 4;
  for (std::uint32_t i = 0; i < count; ++i) {
    if (p >= limit) {
      return false;
    }
    const auto type =
        static_cast<internal::ValueType>(static_cast<std::uint8_t>(*p));
    ++p;
    std::uint32_t klen = 0;
    p = internal::GetVarint32(p, limit, &klen);
    if (p == nullptr || static_cast<std::size_t>(limit - p) < klen) {
      return false;
    }
    const std::string_view key(p, klen);
    p += klen;
    std::uint32_t vlen = 0;
    p = internal::GetVarint32(p, limit, &vlen);
    if (p == nullptr || static_cast<std::size_t>(limit - p) < vlen) {
      return false;
    }
    const std::string_view value(p, vlen);
    p += vlen;
    if (type == internal::ValueType::kValue) {
      if (!mutable_->Put(key, value)) {
        return false;
      }
    } else {
      if (!mutable_->Delete(key)) {
        return false;
      }
    }
  }
  return true;
}

bool KV::Impl::WriteLocked(std::string_view data) {
  if (mutable_->ShouldFlush()) {
    if (!FlushLocked()) {
      return false;
    }
  }
  if (wal_ == nullptr || !wal_->AddRecord(data)) {
    SetError("wal append failed");
    return false;
  }
  if (!ApplyBatch(data)) {
    SetError("apply batch failed");
    return false;
  }
  counters_.Add(Counters::kWalRecords);
  counters_.Add(Counters::kWalBytes, data.size());
  issued_.fetch_add(1, std::memory_order_relaxed);
  return true;
}

bool KV::Impl::WriteSingle(std::string_view key, std::string_view value,
                           internal::ValueType type) {
  if (key.size() > 0xffffffffull || value.size() > 0xffffffffull) {
    SetError("key/value too large");
    return false;
  }
  std::uint64_t my = 0;
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (closed_) {
      SetError("closed");
      return false;
    }
    scratch_.clear();
    scratch_.resize(4, '\0');
    scratch_.push_back(static_cast<char>(type));
    char buf[16];
    char* end = internal::EncodeVarint32(
        buf, static_cast<std::uint32_t>(key.size()));
    scratch_.append(buf, static_cast<std::size_t>(end - buf));
    if (!key.empty()) {
      scratch_.append(key.data(), key.size());
    }
    end =
        internal::EncodeVarint32(buf, static_cast<std::uint32_t>(value.size()));
    scratch_.append(buf, static_cast<std::size_t>(end - buf));
    if (!value.empty()) {
      scratch_.append(value.data(), value.size());
    }
    internal::EncodeFixed32(&scratch_[0], 1);
    if (!WriteLocked(scratch_)) {
      return false;
    }
    my = issued_.load(std::memory_order_relaxed);
    counters_.Add(type == internal::ValueType::kValue ? Counters::kPuts
                                                      : Counters::kDeletes);
  }
  if (opt_.sync_writes) {
    return GroupSync(my);
  }
  return true;
}

bool KV::Impl::Put(std::string_view key, std::string_view value) {
  return WriteSingle(key, value, internal::ValueType::kValue);
}

bool KV::Impl::Delete(std::string_view key) {
  return WriteSingle(key, std::string_view(), internal::ValueType::kDeletion);
}

bool KV::Impl::Write(const WriteBatch& batch) {
  if (batch.Empty()) {
    return true;
  }
  std::uint64_t my = 0;
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (closed_) {
      SetError("closed");
      return false;
    }
    if (!WriteLocked(batch.Data())) {
      return false;
    }
    my = issued_.load(std::memory_order_relaxed);
    counters_.Add(Counters::kWriteBatches);
  }
  if (opt_.sync_writes) {
    return GroupSync(my);
  }
  return true;
}

bool KV::Impl::GroupSync(std::uint64_t seq) {
  // 一轮 = 抢 leader 资格 + 把自己批次之前的所有待同步记录写盘 + 一次
  // fdatasync。等待者必须回到循环顶部重新抢锁：leader 的 target 可能不含
  // 自己的序号，只自旋等 sync_done_ 会永久阻塞（实测 8 线程 sync 写死锁）。
  for (;;) {
    if (sync_done_.load(std::memory_order_acquire) >= seq) {
      return true;
    }
    if (sync_failed_.load(std::memory_order_acquire)) {
      return false;
    }
    std::unique_lock<std::mutex> leader(sync_mu_, std::try_to_lock);
    if (!leader.owns_lock()) {
      std::this_thread::yield();
      continue;
    }
    std::uint64_t target = 0;
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (wal_ == nullptr || !wal_->FlushBuffer()) {
        sync_failed_.store(true, std::memory_order_release);
        return false;
      }
      target = issued_.load(std::memory_order_acquire);
    }
    if (!wal_->Sync()) {
      sync_failed_.store(true, std::memory_order_release);
      return false;
    }
    counters_.Add(Counters::kWalSyncs);
    sync_done_.store(target, std::memory_order_release);
  }
}

bool KV::Impl::Get(std::string_view key, std::string* value) const {
  counters_.Add(Counters::kGets);
  MemTable* m = mutable_ptr_.load(std::memory_order_acquire);
  if (m != nullptr) {
    const internal::Lookup r = m->Lookup(key, value);
    if (r == internal::Lookup::kFound) {
      counters_.Add(Counters::kHits);
      counters_.Add(Counters::kMemtableHits);
      return true;
    }
    if (r == internal::Lookup::kDeleted) {
      return false;
    }
  }

  EpochManager::Guard guard(epoch_);
  const Snapshot* snap = snapshot_.load(std::memory_order_acquire);
  if (snap == nullptr) {
    return false;
  }
  for (const auto& im : snap->immutables) {
    const internal::Lookup r = im->Lookup(key, value);
    if (r == internal::Lookup::kFound) {
      counters_.Add(Counters::kHits);
      counters_.Add(Counters::kImmutableHits);
      return true;
    }
    if (r == internal::Lookup::kDeleted) {
      return false;
    }
  }
  const std::uint32_t key_hash = internal::Hash32(key);
  for (const auto& table : snap->sstables) {
    if (!table->MayContain(key, key_hash)) {
      counters_.Add(Counters::kBloomNegatives);
      continue;
    }
    const internal::Lookup r = table->Get(key, value);
    if (r == internal::Lookup::kFound) {
      counters_.Add(Counters::kHits);
      counters_.Add(Counters::kSstableHits);
      counters_.Add(Counters::kBytesRead,
                    value != nullptr ? value->size() : 0);
      return true;
    }
    if (r == internal::Lookup::kDeleted) {
      return false;
    }
  }
  return false;
}

void KV::Impl::PublishSnapshot(std::unique_ptr<Snapshot> next) {
  const Snapshot* old =
      snapshot_.exchange(next.release(), std::memory_order_acq_rel);
  if (old != nullptr) {
    epoch_.Retire([old]() { delete old; });
  }
}

bool KV::Impl::FlushLocked() {
  if (mutable_ == nullptr || mutable_->Empty()) {
    return true;
  }
  std::shared_ptr<MemTable> old(mutable_.release());
  mutable_ = std::make_unique<MemTable>(opt_);
  MemTable* fresh = mutable_.get();

  auto next =
      std::make_unique<Snapshot>(*snapshot_.load(std::memory_order_acquire));
  next->immutables.insert(next->immutables.begin(), old);
  PublishSnapshot(std::move(next));
  mutable_ptr_.store(fresh, std::memory_order_release);

  const std::uint64_t no = next_file_no_++;
  const std::string path = PathFor(no, "sst");
  SSTableBuilder builder(opt_, path);
  if (!builder.Open()) {
    SetError("open sstable failed: " + path);
    return false;
  }
  std::vector<EntryRef> refs;
  refs.reserve(old->NumEntries());
  old->ForEach([&refs](std::string_view k, std::string_view v, bool deleted) {
    refs.push_back({k, v, deleted ? internal::ValueType::kDeletion
                                  : internal::ValueType::kValue});
  });
  std::sort(refs.begin(), refs.end(),
            [](const EntryRef& a, const EntryRef& b) { return a.key < b.key; });
  for (const auto& r : refs) {
    if (!builder.Add(r.key, r.type, r.value)) {
      SetError("sstable add failed: " + path);
      return false;
    }
  }
  if (!builder.Finish()) {
    SetError("sstable finish failed: " + path);
    return false;
  }
  auto table = SSTable::Open(path, opt_);
  if (table == nullptr) {
    SetError("reopen sstable failed: " + path);
    return false;
  }

  next =
      std::make_unique<Snapshot>(*snapshot_.load(std::memory_order_acquire));
  next->sstables.insert(next->sstables.begin(), std::move(table));
  for (auto it = next->immutables.begin(); it != next->immutables.end(); ++it) {
    if (it->get() == old.get()) {
      next->immutables.erase(it);
      break;
    }
  }
  PublishSnapshot(std::move(next));

  const std::uint64_t new_wal_no = next_file_no_++;
  const std::string new_wal_path = PathFor(new_wal_no, "log");
  wal_->Close();
  wal_ = std::make_unique<WalWriter>(opt_);
  if (!wal_->Open(new_wal_path)) {
    SetError("cannot create wal: " + new_wal_path);
    return false;
  }
  ::unlink(wal_path_.c_str());
  wal_path_ = new_wal_path;

  counters_.Add(Counters::kMemtableFlushes);
  counters_.Add(Counters::kBytesWritten,
                static_cast<std::uint64_t>(builder.file_size()));
  epoch_.Reclaim();
  MaybeCompactLocked();
  return true;
}

void KV::Impl::MaybeCompactLocked() {
  const Snapshot* cur = snapshot_.load(std::memory_order_acquire);
  if (cur == nullptr || cur->sstables.size() <= opt_.max_sstables) {
    return;
  }
  const std::size_t total = cur->sstables.size();
  // 选「大小最接近的相邻两段」合并：每段只在遇到同量级邻居时才重写，单字节
  // 重写 O(log N) 次。旧实现固定取最旧两段，最旧的大段会被之后每个新段反复
  // 重写——1e7 条实测 17.7 GiB / 写放大 36x / Put 163k/s。
  std::size_t first = total - 2;
  std::uint64_t best_ratio = ~static_cast<std::uint64_t>(0);
  for (std::size_t i = total; i > 1; --i) {
    const std::uint64_t a = cur->sstables[i - 2]->file_size();
    const std::uint64_t b = cur->sstables[i - 1]->file_size();
    const std::uint64_t hi = a > b ? a : b;
    const std::uint64_t lo = a > b ? b : a;
    const std::uint64_t ratio =
        lo == 0 ? ~static_cast<std::uint64_t>(0) : (hi * 1024) / lo;
    if (ratio < best_ratio) {
      best_ratio = ratio;
      first = i - 2;
    }
  }
  std::vector<std::shared_ptr<SSTable>> inputs(
      cur->sstables.begin() + static_cast<std::ptrdiff_t>(first),
      cur->sstables.begin() + static_cast<std::ptrdiff_t>(first) + 2);

  const std::uint64_t no = next_file_no_++;
  const std::string path = PathFor(no, "sst");
  SSTableBuilder builder(opt_, path);
  if (!builder.Open()) {
    SetError("open compaction output failed: " + path);
    return;
  }

  std::vector<std::unique_ptr<SSTableIterator>> iters;
  iters.reserve(inputs.size());
  for (const auto& t : inputs) {
    iters.push_back(std::make_unique<SSTableIterator>(*t));
  }

  struct Node {
    std::string_view key;
    std::size_t src;
  };
  struct NodeCmp {
    bool operator()(const Node& a, const Node& b) const {
      const int c = Compare(a.key, b.key);
      if (c != 0) {
        return c > 0;  // 最小堆
      }
      return a.src > b.src;  // 相同 key：src 小（更新）优先
    }
  };
  std::priority_queue<Node, std::vector<Node>, NodeCmp> heap;
  for (std::size_t i = 0; i < iters.size(); ++i) {
    if (iters[i]->Valid()) {
      heap.push({iters[i]->key(), i});
    }
  }

  const bool drop_tombstones = (inputs.size() == total);
  while (!heap.empty()) {
    const Node top = heap.top();
    heap.pop();
    const std::string_view key = top.key;
    std::vector<std::size_t> same_key;
    same_key.push_back(top.src);
    while (!heap.empty() && Compare(heap.top().key, key) == 0) {
      same_key.push_back(heap.top().src);
      heap.pop();
    }
    const std::size_t winner = same_key.front();
    const internal::ValueType type = iters[winner]->type();
    const std::string_view value = iters[winner]->value();
    const bool drop = (type == internal::ValueType::kDeletion) && drop_tombstones;
    if (!drop && !builder.Add(key, type, value)) {
      SetError("compaction add failed");
      return;
    }
    for (const std::size_t s : same_key) {
      iters[s]->Next();
      if (iters[s]->Valid()) {
        heap.push({iters[s]->key(), s});
      }
    }
  }
  if (!builder.Finish()) {
    SetError("compaction finish failed: " + path);
    return;
  }
  auto table = SSTable::Open(path, opt_);
  if (table == nullptr) {
    SetError("reopen compaction output failed: " + path);
    return;
  }

  auto next =
      std::make_unique<Snapshot>(*snapshot_.load(std::memory_order_acquire));
  // 原地替换 [first, first+2) 为新段：合并只发生在相邻段之间，新旧顺序不变
  // （新段仍比更旧的段新，比更新的段旧）。
  next->sstables[first] = std::move(table);
  next->sstables.erase(next->sstables.begin() +
                       static_cast<std::ptrdiff_t>(first) + 1);
  PublishSnapshot(std::move(next));
  // 旧文件可以立即 unlink：已经 mmap 的读者仍持有 inode，新读者从新快照
  // 读不到它们。不删会让目录里的 .sst 随合并次数无限增长（实测 2.6 万）。
  for (const auto& t : inputs) {
    ::unlink(t->path().c_str());
  }
  counters_.Add(Counters::kCompactions);
  counters_.Add(Counters::kBytesWritten,
                static_cast<std::uint64_t>(builder.file_size()));
  epoch_.Reclaim();
}

void KV::Impl::Flush() {
  std::lock_guard<std::mutex> lk(mu_);
  if (closed_) {
    return;
  }
  FlushLocked();
}

std::unique_ptr<Iterator> KV::Impl::NewIterator() {
  struct Item {
    std::string key;
    std::string value;
    std::uint32_t priority;
    bool deleted;
  };
  std::vector<Item> items;
  std::vector<std::shared_ptr<SSTable>> tables;
  std::uint32_t next_priority = 1;
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (closed_) {
      return nullptr;
    }
    const Snapshot* snap = snapshot_.load(std::memory_order_acquire);
    if (mutable_ != nullptr) {
      mutable_->ForEach([&](std::string_view k, std::string_view v, bool del) {
        items.push_back({std::string(k), std::string(v), 0, del});
      });
    }
    std::uint32_t priority = 1;
    for (const auto& im : snap->immutables) {
      im->ForEach([&](std::string_view k, std::string_view v, bool del) {
        items.push_back({std::string(k), std::string(v), priority, del});
      });
      ++priority;
    }
    next_priority = priority;
    tables = snap->sstables;
  }
  // 锁外扫描不可变 SSTable，不阻塞写入。
  std::uint32_t priority = next_priority;
  for (const auto& table : tables) {
    table->ForEach([&](std::string_view k, internal::ValueType t,
                       std::string_view v) {
      items.push_back({std::string(k), std::string(v), priority,
                       t == internal::ValueType::kDeletion});
    });
    ++priority;
  }
  std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
    if (a.key != b.key) {
      return a.key < b.key;
    }
    return a.priority < b.priority;
  });
  std::vector<std::string> keys;
  std::vector<std::string> values;
  keys.reserve(items.size());
  values.reserve(items.size());
  for (std::size_t i = 0; i < items.size(); ++i) {
    if (i > 0 && items[i].key == items[i - 1].key) {
      continue;
    }
    if (items[i].deleted) {
      continue;
    }
    keys.push_back(std::move(items[i].key));
    values.push_back(std::move(items[i].value));
  }
  return std::make_unique<VectorIterator>(std::move(keys), std::move(values));
}

void KV::Impl::Close() {
  bool ok = true;
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (closed_) {
      return;
    }
    ok = FlushLocked();
    if (wal_ != nullptr) {
      if (ok && opt_.sync_writes) {
        ok = wal_->Sync();
      }
      if (!wal_->Close()) {
        ok = false;
      }
      if (ok) {
        ::unlink(wal_path_.c_str());
      }
      wal_.reset();
    }
    closed_ = true;
    const Snapshot* dummy = new Snapshot();
    const Snapshot* old = snapshot_.exchange(dummy, std::memory_order_acq_rel);
    if (old != nullptr) {
      epoch_.Retire([old]() { delete old; });
    }
    mutable_ptr_.store(nullptr, std::memory_order_release);
    mutable_.reset();
  }
  epoch_.Reclaim();
}

Stats KV::Impl::GetStats() const {
  Stats s = counters_.Snapshot();
  const Snapshot* snap = snapshot_.load(std::memory_order_acquire);
  if (snap != nullptr) {
    s.immutable_count = snap->immutables.size();
    s.sstable_count = snap->sstables.size();
  }
  if (mutable_ != nullptr) {
    s.memtable_bytes = mutable_->MemoryUsage();
  }
  return s;
}

std::string KV::Impl::StatsString() const {
  const Stats s = GetStats();
  char buf[1024];
  std::snprintf(
      buf, sizeof(buf),
      "puts=%llu gets=%llu hits=%llu deletes=%llu wal_records=%llu "
      "wal_bytes=%llu wal_syncs=%llu flushes=%llu compactions=%llu "
      "mem_hits=%llu imm_hits=%llu sst_hits=%llu bloom_neg=%llu "
      "memtable_bytes=%llu sstables=%llu immutables=%llu "
      "recover_records=%llu",
      static_cast<unsigned long long>(s.puts),
      static_cast<unsigned long long>(s.gets),
      static_cast<unsigned long long>(s.hits),
      static_cast<unsigned long long>(s.deletes),
      static_cast<unsigned long long>(s.wal_records),
      static_cast<unsigned long long>(s.wal_bytes),
      static_cast<unsigned long long>(s.wal_syncs),
      static_cast<unsigned long long>(s.memtable_flushes),
      static_cast<unsigned long long>(s.compactions),
      static_cast<unsigned long long>(s.memtable_hits),
      static_cast<unsigned long long>(s.immutable_hits),
      static_cast<unsigned long long>(s.sstable_hits),
      static_cast<unsigned long long>(s.bloom_negatives),
      static_cast<unsigned long long>(s.memtable_bytes),
      static_cast<unsigned long long>(s.sstable_count),
      static_cast<unsigned long long>(s.immutable_count),
      static_cast<unsigned long long>(s.recover_records));
  return std::string(buf);
}

std::vector<std::string> KV::Impl::Files() const {
  std::vector<std::string> out;
  DIR* dir = ::opendir(dir_.c_str());
  if (dir == nullptr) {
    return out;
  }
  struct dirent* ent;
  while ((ent = ::readdir(dir)) != nullptr) {
    const std::string name(ent->d_name);
    if (name == "." || name == "..") {
      continue;
    }
    out.push_back(name);
  }
  ::closedir(dir);
  std::sort(out.begin(), out.end());
  return out;
}

// ---------------------------------------------------------------------------
// KV 公共接口
// ---------------------------------------------------------------------------

KV::KV(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
KV::~KV() = default;

std::unique_ptr<KV> KV::Open(const std::string& path, const Options& options) {
  auto impl = std::make_unique<Impl>(path, options);
  if (!impl->Open()) {
    // 返回 nullptr 而丢弃 LastError() 会让「打不开」变成无信息失败。
    GO2CPP_LOG_ERROR(GO2CPP_LOG_NAME("storage"))
        << "KV::Open failed: path=" << path << " error=" << impl->LastError();
    return nullptr;
  }
  return std::unique_ptr<KV>(new KV(std::move(impl)));
}

bool KV::Put(std::string_view key, std::string_view value) {
  return impl_->Put(key, value);
}

bool KV::Get(std::string_view key, std::string* value) const {
  return impl_->Get(key, value);
}

bool KV::Delete(std::string_view key) { return impl_->Delete(key); }

bool KV::Write(const WriteBatch& batch) { return impl_->Write(batch); }

std::unique_ptr<Iterator> KV::NewIterator() { return impl_->NewIterator(); }

void KV::Flush() { impl_->Flush(); }
void KV::Close() { impl_->Close(); }

bool KV::ok() const { return impl_->ok(); }
const std::string& KV::LastError() const { return impl_->LastError(); }
Stats KV::GetStats() const { return impl_->GetStats(); }
std::string KV::StatsString() const { return impl_->StatsString(); }
std::vector<std::string> KV::Files() const { return impl_->Files(); }

}  // namespace storage
}  // namespace go2cpp
