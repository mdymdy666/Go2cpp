// Copyright 2026 Go2Cpp Authors. All rights reserved.
// SegLog：把 LSM 的「WAL 顺序追加」与「SSTable 有序索引」两件事拆开复用。
//
// Queue（MQ 日志）与 Watch（变更日志）共用这一层：它们的写入都是纯追加，
// 不需要 MemTable 的随机读能力；但都要求「按全局序号定位/顺序重放」。
//
// 磁盘布局（目录 dir/）：
//   NNNNNN.seg   段文件，格式与 KV 的 WAL 完全一致（64KB 块 + 7B 头 + CRC32C）
//   NNNNNN.idx   段索引，SSTable 格式（复用块 + footer + Bloom 容器）
//
// 段索引：每 index_interval 条记录写一条 (seq:u64 BE -> offset:u64 BE,
// length:u32 BE)。BE 编码让 SSTable 的字典序比较等价于数值序，冷启动时可以
// 从「最近的检查点」开始重放，而不是从头扫整个日志。
//
// 与 wal.hpp 的关系：
//   * 写端直接复用 WalWriter（同样的 1MB 用户态缓冲 + CRC32C + 分块）；
//   * 读端不能复用 WalReader —— 它只能从文件头顺序扫，而 MQ 的消费者要
//     从任意 seq 开始。这里自带一个按偏移定位的扫描器（同一块格式，约
//     120 行），代价是不改底座（kv.hpp/kv.cpp/wal.* 均不动）。
//
// 崩溃语义：Append 成功 = 记录已进用户态缓冲或已 write() 进内核；
// Sync() 之后 = fdatasync 完成。恢复时按段号顺序重放，遇到不完整尾记录
// （kill -9 的典型残留）就停在该段末尾，不回滚已确认的记录。

#ifndef GO2CPP_STORAGE_SEGLOG_HPP
#define GO2CPP_STORAGE_SEGLOG_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace go2cpp {
namespace storage {

class WalWriter;

// 段日志参数。
struct SegLogOptions {
  // 段滚动阈值（字节）。达到后封段、写索引、开新段。
  std::size_t segment_size = 64u << 20;
  // 写端用户态缓冲（与 KV 的 write_buffer_size 同义）。
  std::size_t write_buffer = 1u << 20;
  // 稀疏索引间隔：每 N 条记录写一个检查点。
  std::size_t index_interval = 512;
  // 封段时是否 fdatasync（保证段文件完整，便于按段淘汰）。
  bool sync_on_roll = true;
  // 恢复时是否校验 CRC。关掉可让 1e7 条日志的恢复从 ~200ms 降到 ~30ms，
  // 代价是位翻转要等到下一次顺序读才发现。
  bool verify_crc_on_recover = false;
};

// 段内记录位置（索引项的值）。
struct LogSlot {
  std::uint64_t offset = 0;  // 记录头在段文件中的字节偏移
  std::uint32_t length = 0;  // 载荷长度（不含 7B 头）
};

// 顺序追加日志（多段）。
class SegLog {
 public:
  class Cursor;

  SegLog();
  ~SegLog();

  SegLog(const SegLog&) = delete;
  SegLog& operator=(const SegLog&) = delete;

  // 打开（不存在则创建）。目录里已有的段会被扫描并接到 seq 链上。
  bool Open(const std::string& dir,
            const SegLogOptions& options = SegLogOptions());

  // 追加一条记录。成功时 *seq 为它的全局序号（从 0 连续递增）。
  bool Append(std::string_view record, std::uint64_t* seq = nullptr);

  // 随机读第 seq 条（走段索引）。seq 不存在返回 false。
  bool Read(std::uint64_t seq, std::string* out);

  bool Flush();
  bool Sync();
  void Close();

  std::uint64_t next_seq() const { return next_seq_; }
  std::uint64_t bytes() const { return total_bytes_; }
  std::size_t segment_count() const { return segments_.size(); }
  std::size_t buffered_bytes() const;
  bool ok() const { return error_.empty(); }
  const std::string& last_error() const { return error_; }
  const SegLogOptions& options() const { return opt_; }
  const std::string& dir() const { return dir_; }

  // 段级统计（诊断/测试用）。
  struct SegmentStat {
    std::uint32_t no = 0;
    std::uint64_t start_seq = 0;
    std::uint64_t count = 0;
    std::uint64_t bytes = 0;
    bool has_index = false;
  };
  std::vector<SegmentStat> GetSegmentStats() const;

  // 单段只读扫描器（不共享；每次 Open 一个 fd）。
  class Scanner {
   public:
    Scanner();
    ~Scanner();
    Scanner(const Scanner&) = delete;
    Scanner& operator=(const Scanner&) = delete;

    bool Open(const std::string& path, bool verify_crc);
    // 定位到文件的某个字节偏移（向下对齐到 64KB 块边界，WAL 记录不跨块）。
    bool SeekTo(std::uint64_t byte_offset);
    // 读下一条完整记录。out 指向内部缓冲，下次调用前有效。
    bool ReadRecord(std::string_view* out);
    // 同上，额外给出该记录在文件中的起始偏移（写索引时用）。
    bool ReadRecord(std::string_view* out, std::uint64_t* record_offset);
    // 当前读位置（= 已成功读出的最后一条记录的结束偏移），用于截断尾部垃圾。
    std::uint64_t position() const { return block_offset_ + pos_; }
    void Close();

   private:
    bool LoadBlock(std::uint64_t block_offset);

    int fd_ = -1;
    std::string block_;
    std::uint64_t block_offset_ = 0;  // 当前块在文件中的起始偏移
    std::size_t pos_ = 0;             // 块内读位置
    std::uint64_t file_size_ = 0;
    bool verify_crc_ = false;
    std::string record_;              // 跨块记录的重组缓冲
  };

  // 顺序游标：从某个 seq 起连续读，跨段自动衔接。
  // 正在写的尾段读到当前文件尾时返回 false，下一次 Next() 会重新探测增长。
  class Cursor {
   public:
    Cursor();
    ~Cursor();
    Cursor(const Cursor&) = delete;
    Cursor& operator=(const Cursor&) = delete;

    bool Seek(SegLog* log, std::uint64_t seq);
    bool Next(std::string_view* out);
    std::uint64_t seq() const { return seq_; }
    bool valid() const { return valid_; }
    void Close();

   private:
    bool OpenSegment(std::size_t idx);
    bool AdvanceSegment();

    SegLog* log_ = nullptr;
    std::size_t seg_idx_ = 0;
    std::uint64_t seq_ = 0;
    std::uint64_t seg_end_seq_ = 0;
    Scanner scanner_;
    bool valid_ = false;
  };

 private:
  friend class Cursor;

  struct SegmentInfo {
    std::uint32_t no = 0;
    std::uint64_t start_seq = 0;
    std::uint64_t count = 0;
    std::uint64_t bytes = 0;
  };

  bool Fail(const std::string& what);
  bool OpenWriteSegment(std::uint32_t no, std::uint64_t existing_size);
  bool Roll();
  bool WriteIndex(const SegmentInfo& seg,
                  const std::vector<std::uint64_t>& slot_seqs,
                  const std::vector<LogSlot>& slots);
  // 扫描一个段，返回记录数与字节数；count_limit=0 表示扫到底。
  // last_valid：已成功读出的最后一条记录的结束偏移（尾部截断用）。
  bool ScanSegment(const std::string& path, std::uint64_t count_limit,
                   std::uint64_t* count, std::uint64_t* bytes,
                   std::uint64_t* last_valid);
  std::size_t FindSegment(std::uint64_t seq) const;

  std::string PathFor(std::uint32_t no, const char* ext) const;

  SegLogOptions opt_;
  std::string dir_;
  std::string error_;
  bool opened_ = false;
  bool closed_ = false;

  std::vector<SegmentInfo> segments_;
  std::uint64_t next_seq_ = 0;
  std::uint64_t total_bytes_ = 0;

  std::unique_ptr<WalWriter> writer_;
  std::uint32_t write_no_ = 0;
  std::uint64_t write_bytes_ = 0;  // 当前段已写字节（估算，含 7B 头）

  // 当前段的稀疏检查点（封段时一次性写成 SSTable）。
  std::vector<std::uint64_t> slot_seqs_;
  std::vector<LogSlot> slots_;
};

}  // namespace storage
}  // namespace go2cpp

#endif  // GO2CPP_STORAGE_SEGLOG_HPP
