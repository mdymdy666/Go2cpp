// Copyright 2026 Go2Cpp Authors. All rights reserved.
// 官方 LevelDB（1.23.x）C++ 包装层，接口形状对齐 include/go2cpp/storage/kv.hpp。
//
// **定位：opt-in 对照后端，不是生产默认路径。** 默认/生产存储是自研的
// `include/go2cpp/storage/kv.hpp`；本后端由 `GO2CPP_WITH_LEVELDB`（默认 OFF）
// 显式打开，当前仓库里唯一使用方是 `tests/storage/test_leveldb_kv.cpp`
// （含对拍、边界、kill -9 与 1e7/1e8 压测），没有任何生产代码在运行时切换到它。
//
// 为什么还保留：自研 KV 的块索引 / SSTable 元数据随数据量增长，1e8 级数据的
// RSS 不可控；LevelDB 用固定容量的 block cache + L0-L6 多级 compaction，RSS
// 有界（1e8 实测见 docs/modules/storage-leveldb.md）。这份「RSS 有界」是
// **该后端自身**的实测结论，说明它具备作为可选后端的资格，**不代表**生产
// 已切换。两套实现保留对照，头文件可在同一个 TU 内共存，便于逐操作对拍。
//
// 语义差异（使用前必读）：
//   * Flush() = CompactRange(nullptr, nullptr)：刷 memtable + 全层 compaction，
//     比自研的「只刷 memtable」重一个量级；写路径上不要频繁调用。
//   * WriteBatch 的序列化格式与 LevelDB 同构（12B header + 变长记录），
//     Data() 可直接解析；sequence 字段由 DB::Write 在提交时分配。
//   * Stats 中 LevelDB 不暴露的计数（wal_* / memtable_flushes / compactions 等）
//     恒为 0；memtable_bytes 取 "approximate-memory-usage"（含 memtable 与
//     block cache 的估算值），语义与自研同名统计不同。
//   * Iterator 必须在 DB 关闭前销毁（LevelDB 原生约束）；key()/value() 返回的
//     视图在下一次 Next()/Seek() 之前有效。
//
// 用法：
//   auto db = go2cpp::storage::ldb::DB::Open("/tmp/db");
//   if (!db) { std::fprintf(stderr, "%s\n", DB::LastOpenError().c_str()); }
//   db->Put("k", "v");
//   std::string v;
//   db->Get("k", &v);
//   auto it = db->NewIterator();
//   for (; it->Valid(); it->Next()) { it->Key(); it->Value(); }

#ifndef GO2CPP_STORAGE_LEVELDB_KV_HPP
#define GO2CPP_STORAGE_LEVELDB_KV_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// 前置声明：公开头文件不引入 leveldb 的宏与类型，避免污染使用方。
// 定义（leveldb/db.h 等）只在 src/storage/leveldb_kv.cpp 内出现。
namespace leveldb {
class DB;
class Iterator;
class Cache;
class FilterPolicy;
}  // namespace leveldb

namespace go2cpp {
namespace storage {
namespace ldb {

// 实现细节（定义在 .cpp）：DB 与 Iterator 共享它（见 Iterator::owner_）。
// 这样即使 DB 包装对象先销毁，只要迭代器还活着，底层 leveldb::DB 就不会
// 被关闭；显式 Close() 后再析构迭代器则会跳过 delete，避免访问已释放的
// LevelDB 内部结构（宁可泄漏一个迭代器对象，也不崩溃）。
struct DbImpl;

// 打开参数。字段与 leveldb::Options 一一对应，默认值面向「小 value、高吞吐」
// 的嵌入式负载。
struct Options {
  // MemTable 预算：写满转 immutable，触发后台 compaction。
  std::size_t write_buffer_size = 4u << 20;
  // 单个 SSTable 文件上限。
  std::size_t max_file_size = 2u << 20;
  // SSTable 数据块大小。
  std::size_t block_size = 4096;
  // BlockCache（LRU）容量；0 表示关闭，完全依赖内核 page cache。
  // 这是 RSS 有界的关键：块内容与块索引都被这个上限约束。
  std::size_t block_cache_size = 8u << 20;
  // 每个 key 的 Bloom 位数；0 关闭。10 位约 1% 假阳性。
  int bloom_bits_per_key = 10;
  // 压缩开关。本机 libleveldb.a 未链接 snappy，true 会在写表时退化为
  // 不压缩（LevelDB 在压缩不可用时自动回退，不报错），保留开关对齐官方 API。
  bool compression = true;
  bool create_if_missing = true;
  bool error_if_exists = false;
  bool paranoid_checks = false;
  // 默认写策略：true 时每次写返回前 fdatasync（崩溃不丢已确认写）。
  bool sync_writes = false;
  // 默认读策略。
  bool verify_checksums = false;
  bool fill_cache = true;
  // 是否在包装层统计 ops（每次读写多一次 relaxed 原子加）。
  // 压测对比时关掉，让 LevelDB 侧开销最接近裸库。
  bool enable_stats = true;
};

// 单次写的策略（覆盖 Options::sync_writes）。
struct WriteOptions {
  bool sync = false;
};

// 单次读的策略（覆盖 Options::verify_checksums / fill_cache）。
struct ReadOptions {
  bool verify_checksums = false;
  bool fill_cache = true;
};

// 原子批。序列化格式与 leveldb::WriteBatch 相同：
//   fixed64 sequence + fixed32 count + 若干 { type(1) + varint32 len + key +
//   [varint32 len + value] }
// sequence 恒为 0：真正的序列号由 LevelDB 在 DB::Write 内部分配。
class WriteBatch {
 public:
  WriteBatch();
  ~WriteBatch();

  void Put(std::string_view key, std::string_view value);
  void Delete(std::string_view key);
  void Clear();

  std::size_t Count() const { return count_; }
  std::size_t ApproximateSize() const { return repr_.size(); }
  bool Empty() const { return count_ == 0; }

  // 内部序列化数据（DB::Write 使用）。
  const std::string& Data() const { return repr_; }

 private:
  friend class DB;

  void Append(char type, std::string_view key, std::string_view value);

  std::string repr_;
  std::size_t count_ = 0;
};

// 有序迭代器：leveldb::Iterator 的 Go 风格包装。
//   * Key()/Value() 与 kv.hpp 的 key()/value() 同义，二者都提供，便于两套
//     代码无差别切换；
//   * 视图在下一次 Next()/Prev()/Seek() 之前有效（零拷贝）。
class Iterator {
 public:
  ~Iterator();

  Iterator(const Iterator&) = delete;
  Iterator& operator=(const Iterator&) = delete;

  bool Valid() const;
  std::string_view Key() const;
  std::string_view Value() const;
  std::string_view key() const { return Key(); }
  std::string_view value() const { return Value(); }

  void Next();
  void Prev();
  void Seek(std::string_view target);
  void SeekToFirst();
  void SeekToLast();

  // 迭代是否发生错误（如磁盘读失败 / checksum 校验失败）。
  bool ok() const;
  std::string LastError() const;

 private:
  friend class DB;

  Iterator(leveldb::Iterator* iter, std::shared_ptr<DbImpl> owner);

  // 与 DB 共享实现对象（见文件内 DbImpl 的说明）。
  std::shared_ptr<DbImpl> owner_;
  leveldb::Iterator* iter_ = nullptr;
};

// 运行期统计快照。字段与 storage::Stats 完全一致，便于对拍与统一报表；
// LevelDB 不暴露的项保持 0（见文件头说明）。
struct Stats {
  std::uint64_t puts = 0;
  std::uint64_t gets = 0;
  std::uint64_t hits = 0;
  std::uint64_t deletes = 0;
  std::uint64_t write_batches = 0;
  std::uint64_t wal_records = 0;
  std::uint64_t wal_bytes = 0;
  std::uint64_t wal_syncs = 0;
  std::uint64_t memtable_flushes = 0;
  std::uint64_t compactions = 0;
  std::uint64_t memtable_hits = 0;
  std::uint64_t immutable_hits = 0;
  std::uint64_t sstable_hits = 0;
  std::uint64_t bloom_negatives = 0;
  std::uint64_t bytes_read = 0;
  std::uint64_t bytes_written = 0;
  std::uint64_t recover_records = 0;
  std::uint64_t recover_bytes = 0;
  std::uint64_t memtable_bytes = 0;
  std::uint64_t sstable_count = 0;
  std::uint64_t immutable_count = 0;
};

// 嵌入式 KV 引擎（LevelDB 后端）。
//
//   auto db = DB::Open("/tmp/db");
//   db->Put("k", "v");
//   std::string v;
//   db->Get("k", &v);
//   db->Delete("k");
//   auto it = db->NewIterator();
//
// 失败时返回 false，原因见 LastError()。Close() 之后除 ok()/LastError()
// 外的操作返回 false（不会崩溃）。
class DB {
 public:
  static std::unique_ptr<DB> Open(const std::string& path,
                                  const Options& options = Options());
  ~DB();

  DB(const DB&) = delete;
  DB& operator=(const DB&) = delete;

  // ---- 写 ----
  bool Put(std::string_view key, std::string_view value);
  bool Put(std::string_view key, std::string_view value,
           const WriteOptions& write_options);
  bool Delete(std::string_view key);
  bool Delete(std::string_view key, const WriteOptions& write_options);
  // 原子批：一次 WAL 记录 + 一次可见性切换。
  bool Write(const WriteBatch& batch);
  bool Write(const WriteBatch& batch, const WriteOptions& write_options);

  // ---- 读 ----
  bool Get(std::string_view key, std::string* value) const;
  bool Get(std::string_view key, std::string* value,
           const ReadOptions& read_options) const;
  // 快照式有序迭代器（迭代开始时 LevelDB 分配的版本快照）。
  std::unique_ptr<Iterator> NewIterator();
  std::unique_ptr<Iterator> NewIterator(const ReadOptions& read_options);

  // ---- 生命周期 ----
  // 强制刷盘 + 全层 compaction（见文件头）。
  void Flush();
  // 关闭：释放文件句柄与 block cache。之后 Put/Get 等返回 false。
  void Close();

  // ---- 诊断 ----
  bool ok() const;
  const std::string& LastError() const;
  Stats GetStats() const;
  std::string StatsString() const;
  std::vector<std::string> Files() const;

  // 最近一次 Open 失败原因（线程局部；Open 返回 nullptr 后用它定位）。
  static const std::string& LastOpenError();

 private:
  DB();
  std::shared_ptr<DbImpl> impl_;
};

}  // namespace ldb
}  // namespace storage
}  // namespace go2cpp

#endif  // GO2CPP_STORAGE_LEVELDB_KV_HPP
