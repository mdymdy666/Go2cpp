// Copyright 2026 Go2Cpp Authors. All rights reserved.
// WAL：预写日志（CRC32C + 分段记录），格式与 LevelDB log 兼容。
//
// 与 LevelDB 的差异（性能取向）：
//   * 用户态 1MB 缓冲：多条记录攒一次 write()，而不是每条记录一次 Append；
//   * 批量 fsync 由 KV 层的 group commit 负责（一个线程 fdatasync，其他
//     线程等待同一批），sync=true 时并发写只付一次 fsync 的钱；
//   * CRC32C 走 SSE4.2 的 crc32 指令（LevelDB 用软件表 + 运行时检测）。

#ifndef GO2CPP_STORAGE_WAL_HPP
#define GO2CPP_STORAGE_WAL_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace go2cpp {
namespace storage {

struct Options;

// 写入端。所有方法非线程安全，由 KV 的写锁串行化。
class WalWriter {
 public:
  explicit WalWriter(const Options& options);
  ~WalWriter();

  WalWriter(const WalWriter&) = delete;
  WalWriter& operator=(const WalWriter&) = delete;

  // 打开（创建/追加）。existing_size 用于恢复已有日志的块偏移。
  bool Open(const std::string& path, std::uint64_t existing_size = 0);
  // 追加一条逻辑记录（超过块大小时自动分段）。
  bool AddRecord(std::string_view record);
  // 把用户态缓冲写入内核。
  bool FlushBuffer();
  // fdatasync。
  bool Sync();
  // 关闭（先 flush）。
  bool Close();

  std::uint64_t BytesWritten() const { return bytes_written_; }
  std::size_t PendingBytes() const { return buf_.size(); }
  bool is_open() const { return fd_ >= 0; }

 private:
  int fd_ = -1;
  std::string buf_;
  std::size_t block_offset_ = 0;
  std::size_t flush_threshold_;
  std::uint64_t bytes_written_ = 0;
  std::string path_;
};

// 读取端：扫描一个日志文件，逐条返回完整记录。
// 返回 false 表示到达文件尾或遇到损坏记录（见 corrupted()）。
class WalReader {
 public:
  explicit WalReader(const Options& options);
  ~WalReader();

  WalReader(const WalReader&) = delete;
  WalReader& operator=(const WalReader&) = delete;

  bool Open(const std::string& path);
  // 读下一条完整记录。返回 false：结束（eof 或损坏）。
  bool ReadRecord(std::string* record);
  bool corrupted() const { return corrupted_; }
  std::uint64_t bytes_read() const { return bytes_read_; }
  std::uint64_t records() const { return records_; }

 private:
  bool NextBlock();

  int fd_ = -1;
  std::string buf_;
  std::size_t pos_ = 0;
  std::size_t limit_ = 0;
  bool eof_ = false;
  bool corrupted_ = false;
  std::uint64_t bytes_read_ = 0;
  std::uint64_t records_ = 0;
};

}  // namespace storage
}  // namespace go2cpp

#endif  // GO2CPP_STORAGE_WAL_HPP
