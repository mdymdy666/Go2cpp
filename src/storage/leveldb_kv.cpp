// Copyright 2026 Go2Cpp Authors. All rights reserved.
// LevelDB 包装层实现，接口声明见 include/go2cpp/storage/leveldb_kv.hpp。
//
// 设计要点：
//   * leveldb 头文件只在本文件出现，公开头不泄漏第三方宏/类型；
//   * DB 用 pimpl（Impl 在 .cpp），Open 失败通过 thread_local 记录原因；
//   * 热路径零拷贝：std::string_view -> leveldb::Slice 只取指针与长度；
//   * 统计计数全部 relaxed 原子，压测时可用 Options::enable_stats 关闭。

#include "go2cpp/storage/leveldb_kv.hpp"

#include <leveldb/cache.h>
#include <leveldb/db.h>
#include <leveldb/filter_policy.h>
#include <leveldb/iterator.h>
#include <leveldb/options.h>
#include <leveldb/status.h>
#include <leveldb/write_batch.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <system_error>
#include <utility>

namespace go2cpp {
namespace storage {
namespace ldb {

namespace {

// LevelDB WriteBatch 记录类型（db/dbformat.h 的 ValueType）。
constexpr char kTypeDeletion = 0;
constexpr char kTypeValue = 1;
// fixed64 sequence + fixed32 count。
constexpr std::size_t kBatchHeaderSize = 12;

// string_view 为空时 data() 可能为 nullptr，而 std::string::append/leveldb::Slice
// 要求指针有效（即使长度为 0），统一换成静态空串。
const char* SafeData(std::string_view text) {
  return text.empty() ? "" : text.data();
}

void EncodeFixed32(char* dst, std::uint32_t value) {
  dst[0] = static_cast<char>(value & 0xffu);
  dst[1] = static_cast<char>((value >> 8) & 0xffu);
  dst[2] = static_cast<char>((value >> 16) & 0xffu);
  dst[3] = static_cast<char>((value >> 24) & 0xffu);
}

void PutVarint32(std::string* dst, std::uint32_t value) {
  char buffer[5];
  int length = 0;
  while (value >= 0x80u) {
    buffer[length++] = static_cast<char>(value | 0x80u);
    value >>= 7;
  }
  buffer[length++] = static_cast<char>(value);
  dst->append(buffer, static_cast<std::size_t>(length));
}

// 解析 varint32；越界 / 超过 5 字节返回 false。
bool GetVarint32(std::string_view src, std::size_t* position,
                 std::uint32_t* value) {
  std::uint32_t result = 0;
  for (int shift = 0; shift <= 28; shift += 7) {
    if (*position >= src.size()) {
      return false;
    }
    const auto byte = static_cast<unsigned char>(src[(*position)++]);
    result |= static_cast<std::uint32_t>(byte & 0x7fu) << shift;
    if ((byte & 0x80u) == 0) {
      *value = result;
      return true;
    }
  }
  return false;
}

leveldb::Slice ToSlice(std::string_view text) {
  return leveldb::Slice(SafeData(text), text.size());
}

std::uint64_t ParseU64(const std::string& text) {
  if (text.empty()) {
    return 0;
  }
  return std::strtoull(text.c_str(), nullptr, 10);
}

// 最近一次 Open 失败原因。thread_local：多线程并发 Open 时互不覆盖。
thread_local std::string g_last_open_error;

}  // namespace

// ---------------------------------------------------------------------------
// WriteBatch
// ---------------------------------------------------------------------------

WriteBatch::WriteBatch() : repr_(kBatchHeaderSize, '\0') {}
WriteBatch::~WriteBatch() = default;

void WriteBatch::Put(std::string_view key, std::string_view value) {
  Append(kTypeValue, key, value);
}

void WriteBatch::Delete(std::string_view key) {
  Append(kTypeDeletion, key, std::string_view());
}

void WriteBatch::Clear() {
  repr_.assign(kBatchHeaderSize, '\0');
  count_ = 0;
}

void WriteBatch::Append(char type, std::string_view key,
                        std::string_view value) {
  repr_.push_back(type);
  PutVarint32(&repr_, static_cast<std::uint32_t>(key.size()));
  repr_.append(SafeData(key), key.size());
  if (type == kTypeValue) {
    PutVarint32(&repr_, static_cast<std::uint32_t>(value.size()));
    repr_.append(SafeData(value), value.size());
  }
  ++count_;
  // header 的 count 字段（offset 8..12）：让 Data() 与 LevelDB 的 rep_ 同构。
  EncodeFixed32(&repr_[8], static_cast<std::uint32_t>(count_));
}

// ---------------------------------------------------------------------------
// Iterator（构造/析构在 DbImpl 定义之后：析构需要判断底层 DB 是否存活）
// ---------------------------------------------------------------------------

bool Iterator::Valid() const { return iter_ != nullptr && iter_->Valid(); }

std::string_view Iterator::Key() const {
  if (!Valid()) {
    return std::string_view();
  }
  const leveldb::Slice slice = iter_->key();
  return slice.data() == nullptr
             ? std::string_view()
             : std::string_view(slice.data(), slice.size());
}

std::string_view Iterator::Value() const {
  if (!Valid()) {
    return std::string_view();
  }
  const leveldb::Slice slice = iter_->value();
  return slice.data() == nullptr
             ? std::string_view()
             : std::string_view(slice.data(), slice.size());
}

void Iterator::Next() {
  if (iter_ != nullptr) {
    iter_->Next();
  }
}

void Iterator::Prev() {
  if (iter_ != nullptr) {
    iter_->Prev();
  }
}

void Iterator::Seek(std::string_view target) {
  if (iter_ != nullptr) {
    iter_->Seek(ToSlice(target));
  }
}

void Iterator::SeekToFirst() {
  if (iter_ != nullptr) {
    iter_->SeekToFirst();
  }
}

void Iterator::SeekToLast() {
  if (iter_ != nullptr) {
    iter_->SeekToLast();
  }
}

bool Iterator::ok() const {
  return iter_ != nullptr && iter_->status().ok();
}

std::string Iterator::LastError() const {
  if (iter_ == nullptr) {
    return "iterator is null";
  }
  const leveldb::Status status = iter_->status();
  return status.ok() ? std::string() : status.ToString();
}

// ---------------------------------------------------------------------------
// DbImpl（DB 与 Iterator 共享）
// ---------------------------------------------------------------------------

struct DbImpl {
  ~DbImpl() { delete db.exchange(nullptr); }

  std::string path;
  Options options;
  // 析构顺序（成员逆序）：db 先于 filter_policy/cache 释放，因为 LevelDB
  // 的 DBImpl 持有二者的指针直到自身析构完成。
  std::unique_ptr<leveldb::Cache> cache;
  std::unique_ptr<const leveldb::FilterPolicy> filter_policy;
  // 原子指针：DB 操作与 Close() / 迭代器析构可能发生在不同线程。
  std::atomic<leveldb::DB*> db{nullptr};

  mutable std::mutex error_mutex;
  mutable std::string last_error;
  std::atomic<bool> ok{true};

  // 包装层统计（relaxed）。
  std::atomic<std::uint64_t> puts{0};
  std::atomic<std::uint64_t> gets{0};
  std::atomic<std::uint64_t> hits{0};
  std::atomic<std::uint64_t> deletes{0};
  std::atomic<std::uint64_t> write_batches{0};
  std::atomic<std::uint64_t> bytes_read{0};
  std::atomic<std::uint64_t> bytes_written{0};

  leveldb::DB* LoadDb() const { return db.load(std::memory_order_acquire); }

  bool Fail(const leveldb::Status& status) {
    {
      std::lock_guard<std::mutex> lock(error_mutex);
      last_error = status.ToString();
    }
    ok.store(false, std::memory_order_relaxed);
    return false;
  }

  bool Fail(const char* message) {
    {
      std::lock_guard<std::mutex> lock(error_mutex);
      last_error = message;
    }
    ok.store(false, std::memory_order_relaxed);
    return false;
  }

  void NoteSuccess() { ok.store(true, std::memory_order_relaxed); }
};

Iterator::Iterator(leveldb::Iterator* iter, std::shared_ptr<DbImpl> owner)
    : owner_(std::move(owner)), iter_(iter) {}

Iterator::~Iterator() {
  if (iter_ == nullptr) {
    return;
  }
  // 只有底层 DB 仍存活时才析构 LevelDB 的迭代器：显式 Close() 之后，迭代器
  // 内部的 table cache / memtable 引用已随 DB 释放，delete 会 UAF。
  // 此时选择泄漏一个迭代器对象（错误用法的兜底），而不是崩溃。
  if (owner_ != nullptr && owner_->LoadDb() != nullptr) {
    delete iter_;
  }
  iter_ = nullptr;
}

DB::DB() : impl_(std::make_shared<DbImpl>()) {}
DB::~DB() = default;

const std::string& DB::LastOpenError() { return g_last_open_error; }

std::unique_ptr<DB> DB::Open(const std::string& path, const Options& options) {
  auto wrapper = std::unique_ptr<DB>(new DB());
  DbImpl& impl = *wrapper->impl_;
  impl.path = path;
  impl.options = options;

  leveldb::Options level_options;
  level_options.create_if_missing = options.create_if_missing;
  level_options.error_if_exists = options.error_if_exists;
  level_options.paranoid_checks = options.paranoid_checks;
  level_options.write_buffer_size = options.write_buffer_size;
  level_options.max_file_size = options.max_file_size;
  level_options.block_size = options.block_size;
  level_options.compression =
      options.compression ? leveldb::kSnappyCompression : leveldb::kNoCompression;

  if (options.block_cache_size > 0) {
    impl.cache.reset(leveldb::NewLRUCache(options.block_cache_size));
    level_options.block_cache = impl.cache.get();
  }
  if (options.bloom_bits_per_key > 0) {
    impl.filter_policy.reset(
        leveldb::NewBloomFilterPolicy(options.bloom_bits_per_key));
    level_options.filter_policy = impl.filter_policy.get();
  }

  leveldb::DB* raw = nullptr;
  const leveldb::Status status = leveldb::DB::Open(level_options, path, &raw);
  if (!status.ok()) {
    g_last_open_error = status.ToString();
    return nullptr;
  }
  impl.db.store(raw, std::memory_order_release);
  g_last_open_error.clear();
  return wrapper;
}

// ---------------------------------------------------------------------------
// 写路径
// ---------------------------------------------------------------------------

bool DB::Put(std::string_view key, std::string_view value) {
  return Put(key, value, WriteOptions{impl_->options.sync_writes});
}

bool DB::Put(std::string_view key, std::string_view value,
             const WriteOptions& write_options) {
  leveldb::DB* db = impl_->LoadDb();
  if (db == nullptr) {
    return impl_->Fail("leveldb: database is closed");
  }
  leveldb::WriteOptions level_write;
  level_write.sync = write_options.sync;
  const leveldb::Status status =
      db->Put(level_write, ToSlice(key), ToSlice(value));
  if (impl_->options.enable_stats) {
    impl_->puts.fetch_add(1, std::memory_order_relaxed);
    impl_->bytes_written.fetch_add(key.size() + value.size(),
                                   std::memory_order_relaxed);
  }
  if (!status.ok()) {
    return impl_->Fail(status);
  }
  impl_->NoteSuccess();
  return true;
}

bool DB::Delete(std::string_view key) {
  return Delete(key, WriteOptions{impl_->options.sync_writes});
}

bool DB::Delete(std::string_view key,
                const WriteOptions& write_options) {
  leveldb::DB* db = impl_->LoadDb();
  if (db == nullptr) {
    return impl_->Fail("leveldb: database is closed");
  }
  leveldb::WriteOptions level_write;
  level_write.sync = write_options.sync;
  const leveldb::Status status =
      db->Delete(level_write, ToSlice(key));
  if (impl_->options.enable_stats) {
    impl_->deletes.fetch_add(1, std::memory_order_relaxed);
  }
  if (!status.ok()) {
    return impl_->Fail(status);
  }
  impl_->NoteSuccess();
  return true;
}

bool DB::Write(const WriteBatch& batch) {
  return Write(batch, WriteOptions{impl_->options.sync_writes});
}

bool DB::Write(const WriteBatch& batch, const WriteOptions& write_options) {
  leveldb::DB* db = impl_->LoadDb();
  if (db == nullptr) {
    return impl_->Fail("leveldb: database is closed");
  }
  if (batch.Empty()) {
    return true;
  }

  // 把我们维护的 rep_ 逐条转成 leveldb::WriteBatch：一次 WAL 记录、
  // 一次可见性切换，与自研 KV 的原子批语义一致。
  leveldb::WriteBatch level_batch;
  const std::string& repr = batch.Data();
  std::size_t position = kBatchHeaderSize;
  while (position < repr.size()) {
    const char type = repr[position++];
    std::uint32_t key_size = 0;
    if (!GetVarint32(repr, &position, &key_size)) {
      return impl_->Fail("malformed WriteBatch: key length");
    }
    if (key_size > repr.size() - position) {
      return impl_->Fail("malformed WriteBatch: key overrun");
    }
    const leveldb::Slice key(repr.data() + position, key_size);
    position += key_size;

    if (type == kTypeValue) {
      std::uint32_t value_size = 0;
      if (!GetVarint32(repr, &position, &value_size)) {
        return impl_->Fail("malformed WriteBatch: value length");
      }
      if (value_size > repr.size() - position) {
        return impl_->Fail("malformed WriteBatch: value overrun");
      }
      const leveldb::Slice value(repr.data() + position, value_size);
      position += value_size;
      level_batch.Put(key, value);
    } else if (type == kTypeDeletion) {
      level_batch.Delete(key);
    } else {
      return impl_->Fail("malformed WriteBatch: unknown record type");
    }
  }

  leveldb::WriteOptions level_write;
  level_write.sync = write_options.sync;
  const leveldb::Status status = db->Write(level_write, &level_batch);
  if (impl_->options.enable_stats) {
    impl_->write_batches.fetch_add(1, std::memory_order_relaxed);
    impl_->bytes_written.fetch_add(repr.size(), std::memory_order_relaxed);
  }
  if (!status.ok()) {
    return impl_->Fail(status);
  }
  impl_->NoteSuccess();
  return true;
}

// ---------------------------------------------------------------------------
// 读路径
// ---------------------------------------------------------------------------

bool DB::Get(std::string_view key, std::string* value) const {
  return Get(key, value,
             ReadOptions{impl_->options.verify_checksums,
                         impl_->options.fill_cache});
}

bool DB::Get(std::string_view key, std::string* value,
             const ReadOptions& read_options) const {
  leveldb::DB* db = impl_->LoadDb();
  if (db == nullptr) {
    return impl_->Fail("leveldb: database is closed");
  }
  if (impl_->options.enable_stats) {
    impl_->gets.fetch_add(1, std::memory_order_relaxed);
  }

  std::string scratch;
  std::string* destination = value != nullptr ? value : &scratch;
  leveldb::ReadOptions level_read;
  level_read.verify_checksums = read_options.verify_checksums;
  level_read.fill_cache = read_options.fill_cache;
  const leveldb::Status status =
      db->Get(level_read, ToSlice(key), destination);

  if (status.ok()) {
    if (impl_->options.enable_stats) {
      impl_->hits.fetch_add(1, std::memory_order_relaxed);
      impl_->bytes_read.fetch_add(destination->size(),
                                  std::memory_order_relaxed);
    }
    return true;
  }
  if (status.IsNotFound()) {
    return false;
  }
  impl_->Fail(status);
  return false;
}

std::unique_ptr<Iterator> DB::NewIterator() {
  return NewIterator(ReadOptions{impl_->options.verify_checksums,
                                 impl_->options.fill_cache});
}

std::unique_ptr<Iterator> DB::NewIterator(const ReadOptions& read_options) {
  leveldb::DB* db = impl_->LoadDb();
  if (db == nullptr) {
    impl_->Fail("leveldb: database is closed");
    return nullptr;
  }
  leveldb::ReadOptions level_read;
  level_read.verify_checksums = read_options.verify_checksums;
  level_read.fill_cache = read_options.fill_cache;
  leveldb::Iterator* raw = db->NewIterator(level_read);
  if (raw == nullptr) {
    return nullptr;
  }
  // 与自研 KV 一致：迭代器构造后停在第一个元素。
  std::unique_ptr<Iterator> iterator(new Iterator(raw, impl_));
  iterator->SeekToFirst();
  return iterator;
}

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------

void DB::Flush() {
  leveldb::DB* db = impl_->LoadDb();
  if (db == nullptr) {
    return;
  }
  // LevelDB 没有「只刷 memtable」的公开 API；CompactRange(nullptr, nullptr)
  // 会先 TEST_CompactMemTable 再逐层合并，是唯一保证落盘的手段。
  db->CompactRange(nullptr, nullptr);
}

void DB::Close() {
  delete impl_->db.exchange(nullptr, std::memory_order_acq_rel);
  impl_->filter_policy.reset();
  impl_->cache.reset();
}

// ---------------------------------------------------------------------------
// 诊断
// ---------------------------------------------------------------------------

bool DB::ok() const { return impl_->ok.load(std::memory_order_relaxed); }

const std::string& DB::LastError() const {
  std::lock_guard<std::mutex> lock(impl_->error_mutex);
  return impl_->last_error;
}

Stats DB::GetStats() const {
  Stats stats;
  const auto load = [](const std::atomic<std::uint64_t>& counter) {
    return counter.load(std::memory_order_relaxed);
  };
  stats.puts = load(impl_->puts);
  stats.gets = load(impl_->gets);
  stats.hits = load(impl_->hits);
  stats.deletes = load(impl_->deletes);
  stats.write_batches = load(impl_->write_batches);
  stats.bytes_read = load(impl_->bytes_read);
  stats.bytes_written = load(impl_->bytes_written);

  leveldb::DB* db = impl_->LoadDb();
  if (db != nullptr) {
    std::string property;
    if (db->GetProperty("approximate-memory-usage", &property)) {
      stats.memtable_bytes = ParseU64(property);
    }
    for (int level = 0; level <= 6; ++level) {
      char name[32];
      std::snprintf(name, sizeof(name), "num-files-at-level%d", level);
      if (db->GetProperty(name, &property)) {
        stats.sstable_count += ParseU64(property);
      }
    }
  }
  return stats;
}

std::string DB::StatsString() const {
  const Stats stats = GetStats();
  char buffer[512];
  std::snprintf(
      buffer, sizeof(buffer),
      "leveldb puts=%llu gets=%llu hits=%llu deletes=%llu batches=%llu "
      "bytes_read=%llu bytes_written=%llu memtable_bytes=%llu sstables=%llu",
      static_cast<unsigned long long>(stats.puts),
      static_cast<unsigned long long>(stats.gets),
      static_cast<unsigned long long>(stats.hits),
      static_cast<unsigned long long>(stats.deletes),
      static_cast<unsigned long long>(stats.write_batches),
      static_cast<unsigned long long>(stats.bytes_read),
      static_cast<unsigned long long>(stats.bytes_written),
      static_cast<unsigned long long>(stats.memtable_bytes),
      static_cast<unsigned long long>(stats.sstable_count));
  std::string out(buffer);
  leveldb::DB* db = impl_->LoadDb();
  if (db != nullptr) {
    std::string property;
    if (db->GetProperty("leveldb.stats", &property)) {
      out += "\n";
      out += property;
    }
  }
  return out;
}

std::vector<std::string> DB::Files() const {
  std::vector<std::string> files;
  std::error_code error;
  std::filesystem::directory_iterator iterator(impl_->path, error);
  if (error) {
    return files;
  }
  for (const auto& entry : iterator) {
    std::error_code entry_error;
    if (entry.is_regular_file(entry_error)) {
      files.push_back(entry.path().filename().string());
    }
  }
  std::sort(files.begin(), files.end());
  return files;
}

}  // namespace ldb
}  // namespace storage
}  // namespace go2cpp
