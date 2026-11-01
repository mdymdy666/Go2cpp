// Copyright 2026 Go2Cpp Authors. All rights reserved.
// 自研 KV 存储引擎：零依赖、嵌入式、单进程多线程。
//
// 设计目标（参考 LevelDB，但刻意砍掉多版本/快照/多级 Compaction）：
//   * 写路径：WriteBatch -> WAL（批量追加 + group commit）-> MemTable
//   * 读路径：MemTable（无锁）-> immutable MemTable -> SSTable（mmap）
//   * 落盘：MemTable 满即刷成 SSTable；SSTable 数量超限时合并最旧的若干段
//
// 线程模型：任意线程可并发 Get；Put/Delete/Write 之间通过内部写锁串行，
// 读路径不持锁（槽位原子发布 + epoch 延迟回收，见 src/storage/kv.cpp）。

#ifndef GO2CPP_STORAGE_KV_HPP
#define GO2CPP_STORAGE_KV_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace go2cpp {
namespace storage {

// 打开参数。默认值面向「小 value、高吞吐」的嵌入式负载。
struct Options {
  // MemTable 数据预算（Arena 字节数）。超过后触发一次刷盘。
  std::size_t memtable_size = 8u << 20;
  // SSTable 数据块目标大小；单条记录超过该值时块会自然变长。
  // 4KB：块内顺序扫描（有序，可提前退出）比 16KB 块的二分更快——后者
  // 每次比较都跳到新的 cache line（实测 1e7 读 1915ns vs 4KB 的 ~700ns）。
  std::size_t block_size = 4096;
  // Compaction 一次合并的段数（从最旧的段开始）。
  std::size_t sstables_per_compaction = 2;
  // SSTable 段数超过该值时触发一次合并。段数决定每次 Get 的 Bloom 检查
  // 次数与写放大：2 段读最快但 compaction 太频繁（Put 慢 2x），取 4 折中。
  std::size_t max_sstables = 4;
  // WAL 用户态缓冲，攒满后一次 write() 落盘（而不是每条记录一次）。
  std::size_t write_buffer_size = 1u << 20;
  // 每个 key 分配的 Bloom 位数；0 表示关闭 Bloom。
  int bloom_bits_per_key = 10;
  // true：每次 Write 返回前 fdatasync（崩溃不丢已确认写）；
  // false：由内核回写，崩溃可能丢最后一段，但吞吐高一个数量级。
  bool sync_writes = false;
  // true：SSTable 用 mmap 读取（零拷贝）；false：pread 读取。
  bool use_mmap = true;
  // 目录不存在时创建。
  bool create_if_missing = true;
  // 目录已存在且非空时报错。
  bool error_if_exists = false;
};

// 原子批。序列化格式与 LevelDB WriteBatch 同构（count + 变长记录），
// 但去掉 sequence number（本引擎单版本）。
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

  // 内部序列化数据（KV::Write 使用）。
  const std::string& Data() const { return repr_; }

 private:
  friend class KV;
  std::string repr_;
  std::size_t count_ = 0;
};

// 有序迭代器。key()/value() 返回的视图在下一次 Next()/Seek() 之前有效。
class Iterator {
 public:
  virtual ~Iterator() = default;

  virtual bool Valid() const = 0;
  virtual std::string_view key() const = 0;
  virtual std::string_view value() const = 0;
  virtual void Next() = 0;
  virtual void Seek(std::string_view target) = 0;
  virtual void SeekToFirst() = 0;
  virtual void SeekToLast() = 0;
};

// 运行期统计（快照语义：GetStats() 拷贝一份）。
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

// 嵌入式 KV 引擎。
//
//   auto db = KV::Open("/tmp/db");
//   db->Put("k", "v");
//   std::string v;
//   db->Get("k", &v);
//   db->Delete("k");
//   auto it = db->NewIterator();
//
// 失败时返回 false，具体原因见 LastError()。
class KV {
 public:
  static std::unique_ptr<KV> Open(const std::string& path,
                                  const Options& options = Options());
  ~KV();

  KV(const KV&) = delete;
  KV& operator=(const KV&) = delete;

  bool Put(std::string_view key, std::string_view value);
  bool Get(std::string_view key, std::string* value) const;
  bool Delete(std::string_view key);

  // 原子批：要么全部可见，要么全不可见（WAL 单条记录 + 内存一次应用）。
  bool Write(const WriteBatch& batch);

  // 快照式有序迭代器（当前 MemTable + immutable + SSTable 的并集）。
  std::unique_ptr<Iterator> NewIterator();

  // 刷盘：把当前 MemTable 落成 SSTable，并切换新 WAL。
  void Flush();

  // 关闭：Flush + WAL fsync + 释放文件。
  void Close();

  bool ok() const;
  const std::string& LastError() const;
  Stats GetStats() const;
  std::string StatsString() const;

  // 仅测试/诊断用：当前目录下的文件列表。
  std::vector<std::string> Files() const;

 private:
  class Impl;
  explicit KV(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> impl_;
};

}  // namespace storage
}  // namespace go2cpp

#endif  // GO2CPP_STORAGE_KV_HPP
