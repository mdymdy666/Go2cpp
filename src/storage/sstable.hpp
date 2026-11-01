// Copyright 2026 Go2Cpp Authors. All rights reserved.
// SSTable：不可变有序文件（数据块 + 块索引 + Bloom + footer）。
//
// 与 LevelDB 的差异（性能取向）：
//   * 块内加 offset 数组，二分查找（LevelDB 用 restart 点 + 线性扫描）；
//   * 索引块同样带 offset 数组，二分定位数据块；
//   * 读路径默认 mmap（零拷贝，无 read()/memcpy），Get 只做两次二分；
//   * 不做前缀压缩：短 key 上省下的解码分支比省下的磁盘字节更值钱。
//
// 文件布局：
//   [data block 0] ... [data block N-1] [bloom] [index] [footer]
//
// 数据块：
//   [count:u32][offsets_start:u32] + entries + offsets[count]（u32，相对块首）
//   条目：[klen:varint][key][vlen:varint][type:u8][value]
//
// 索引块：与数据块同布局，条目为
//   [klen:varint][last_key][offset:varint][size:varint]
//
// footer（48B）：magic(8) index_off(8) index_size(8) bloom_off(8)
//                bloom_size(8) num_entries(8)

#ifndef GO2CPP_STORAGE_SSTABLE_HPP
#define GO2CPP_STORAGE_SSTABLE_HPP

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "format.hpp"
#include "go2cpp/storage/kv.hpp"

namespace go2cpp {
namespace storage {

struct Options;

// 块构建器：攒条目直到超过目标块大小。
class BlockBuilder {
 public:
  explicit BlockBuilder(std::size_t block_size);

  void Add(std::string_view key, internal::ValueType type,
           std::string_view value);
  bool Empty() const { return offsets_.empty(); }
  std::size_t EstimatedSize() const;  // 含 offsets 数组
  std::string Finish() const;
  std::string_view last_key() const { return last_key_; }
  void Reset();

 private:
  std::size_t block_size_;
  std::string entries_;
  std::vector<std::uint32_t> offsets_;
  std::string last_key_;
};

// 构建一个 SSTable 文件（先写 <path>.tmp，Finish 时 fsync + rename）。
class SSTableBuilder {
 public:
  SSTableBuilder(const Options& options, std::string path);
  ~SSTableBuilder();

  SSTableBuilder(const SSTableBuilder&) = delete;
  SSTableBuilder& operator=(const SSTableBuilder&) = delete;

  bool Open();
  // 调用方保证 key 递增。
  bool Add(std::string_view key, internal::ValueType type,
           std::string_view value);
  bool Finish();

  std::uint64_t file_size() const { return file_size_; }
  std::uint64_t num_entries() const { return num_entries_; }
  const std::string& path() const { return path_; }

 private:
  bool FlushBlock();
  bool WriteRaw(const void* data, std::size_t n);
  bool FlushOut();

  Options options_;  // 值拷贝：builder 生命周期可能长于调用方临时对象
  std::string path_;
  std::string tmp_path_;
  int fd_ = -1;
  std::string out_;
  BlockBuilder block_;
  std::vector<std::string> index_keys_;
  std::vector<std::uint64_t> index_offsets_;
  std::vector<std::uint64_t> index_sizes_;
  std::vector<std::uint32_t> key_hashes_;
  std::uint64_t file_size_ = 0;
  std::uint64_t num_entries_ = 0;
  bool finished_ = false;
};

// 只读 SSTable（mmap）。
class SSTable {
 public:
  static std::unique_ptr<SSTable> Open(const std::string& path,
                                       const Options& options);
  ~SSTable();

  SSTable(const SSTable&) = delete;
  SSTable& operator=(const SSTable&) = delete;

  internal::Lookup Get(std::string_view key, std::string* value) const;

  // 顺序扫描（迭代器/Compaction 用）。
  void ForEach(const std::function<void(std::string_view, internal::ValueType,
                                        std::string_view)>& fn) const;

  std::uint64_t num_entries() const { return num_entries_; }
  std::uint64_t file_size() const { return size_; }
  const std::string& path() const { return path_; }
  bool MayContain(std::string_view key) const;
  // 复用调用方算好的哈希（多段查询时省掉重复的 Hash32）。
  bool MayContain(std::string_view key, std::uint32_t hash) const;

 private:
  friend class SSTableIterator;
  SSTable() = default;

  struct BlockRef {
    const char* data;
    std::uint32_t size;
  };
  // 返回 [lo, count) 中第一个 last_key >= key 的块下标；没有则返回 count。
  std::size_t FindBlock(std::string_view key) const;
  BlockRef GetBlock(std::size_t idx) const;

  std::string path_;
  int fd_ = -1;
  const char* data_ = nullptr;
  std::size_t size_ = 0;
  bool mapped_ = false;
  std::string heap_buf_;
  const char* index_ = nullptr;
  std::size_t index_size_ = 0;
  const char* bloom_ = nullptr;
  std::size_t bloom_size_ = 0;
  std::uint64_t num_entries_ = 0;
};

// 顺序迭代一个 SSTable（Compaction 的 k 路归并输入）。
class SSTableIterator {
 public:
  explicit SSTableIterator(const SSTable& table);
  ~SSTableIterator();

  bool Valid() const { return valid_; }
  std::string_view key() const { return key_; }
  std::string_view value() const { return value_; }
  internal::ValueType type() const { return type_; }
  void Next();

 private:
  bool LoadEntry();
  bool NextBlock();

  const SSTable& table_;
  std::size_t block_idx_ = 0;
  std::size_t entry_idx_ = 0;
  std::size_t block_entries_ = 0;
  const char* block_ = nullptr;
  const char* block_end_ = nullptr;
  const char* offsets_ = nullptr;
  std::string_view key_;
  std::string_view value_;
  internal::ValueType type_ = internal::ValueType::kValue;
  bool valid_ = false;
};

}  // namespace storage
}  // namespace go2cpp

#endif  // GO2CPP_STORAGE_SSTABLE_HPP
