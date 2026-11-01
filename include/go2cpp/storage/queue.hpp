// Copyright 2026 Go2Cpp Authors. All rights reserved.
// Queue：基于同一套 LSM 的顺序消息队列（MQ）。
//
// 设计（对应 LSM 的 WAL 部件）：
//   * Append 只做「顺序追加」——不写 MemTable、不做随机读，所以不需要
//     MemTable/Arena 的随机写能力。落盘复用 WAL 的块格式（64KB 块 + 7B 头
//     + CRC32C + 1MB 用户态缓冲），多 topic 共用一条日志（一个段序列）。
//   * 消费游标（每 topic × consumer）持久化进 KV：key = "queue/<topic>/
//     <consumer>/offset"。也就是说 MQ 的「位点」本身是 LSM 里的一条普通
//     KV 记录 —— 这是三个功能「共用一套 LSM」的落点。
//   * 段滚动：段文件达到 segment_size 即封段，同时把该段的稀疏检查点写成
//     SSTable（复用块 + footer 格式）。冷启动从检查点定位，不必从头扫。
//
// 语义：at-least-once。Consume 只读取、不推进游标；Ack 才推进。崩溃或
// 进程重启后游标不回退（批量落盘窗口内最多重复投递 N 条，不会丢）。
//
// 并发：任意线程可并发 Append/Consume/Ack；不同 topic 之间不共享锁，
// 同一 topic 内串行（一把 topic 锁，与 LSM 写锁同粒度）。

#ifndef GO2CPP_STORAGE_QUEUE_HPP
#define GO2CPP_STORAGE_QUEUE_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace go2cpp {
namespace storage {

class KV;
class SegLog;

struct QueueOptions {
  // 单个段文件的滚动阈值（字节）。
  std::size_t segment_size = 64u << 20;
  // 写端用户态缓冲。攒满一次 write()，而不是每条消息一次 syscall。
  std::size_t write_buffer = 1u << 20;
  // 段索引的稀疏检查点间隔（条）。
  std::size_t index_interval = 512;
  // Append 后是否 fdatasync（true = 已确认不丢，false = 由内核回写）。
  bool sync_on_append = false;
  // 每 N 次 Ack 把游标批量写一次 KV（0 = 不自动写，只能显式 FlushCursors）。
  std::size_t cursor_flush_interval = 4096;
  // 内存里最多保留多少条已追加消息（0 = 不限制，直到全部被确认回收）。
  // 超过后即使未确认也丢弃内存副本，消费转走日志扫描（冷路径）。
  std::size_t retain = 0;
  // 打开时是否扫描日志重建 topic 状态（next_offset / 条数）。
  bool recover_topics = true;
  // 用 thread_local 缓存最近 topic（稳态下省掉全局锁 + string 构造）。
  // 关掉即回到「每次查找都走 map」的旧路径，用于前后对比。
  bool topic_cache = true;
};

// 一条消息。
struct QueueMessage {
  std::string value;
  std::string topic;
  std::uint64_t offset = 0;  // topic 内序号，从 0 连续递增
};

struct QueueStats {
  std::uint64_t appends = 0;
  std::uint64_t consumes = 0;
  std::uint64_t acks = 0;
  std::uint64_t cold_reads = 0;      // 走日志扫描的消费次数
  std::uint64_t cold_scanned = 0;    // 冷读时扫过的日志记录数
  std::uint64_t cursor_flushes = 0;  // 批量写 KV 的次数
  std::uint64_t cursor_writes = 0;   // 写进 KV 的游标条数
  std::uint64_t persist_errors = 0;
  std::uint64_t produced_bytes = 0;
  std::uint64_t buffered_bytes = 0;
  std::uint64_t segments = 0;
  std::uint64_t retained = 0;        // 当前内存中保留的消息条数
  std::uint64_t topics = 0;
  std::uint64_t consumers = 0;
};

class Queue {
 public:
  // kv 可以为 nullptr：此时游标只存内存（进程退出即丢）。
  // dir 是日志目录（通常放在 LSM 数据目录下的 queue/ 子目录）。
  static std::unique_ptr<Queue> Open(KV* kv, const std::string& dir,
                                     const QueueOptions& options = QueueOptions());
  ~Queue();

  Queue(const Queue&) = delete;
  Queue& operator=(const Queue&) = delete;

  // 追加一条消息到 topic。返回 topic 内序号（从 0 连续递增）。
  bool Append(std::string_view topic, std::string_view msg,
              std::uint64_t* offset = nullptr);

  // 读取 consumer 的下一条消息（不推进游标）。没有新消息返回 false。
  bool Consume(std::string_view topic, std::string_view consumer,
               QueueMessage* msg);

  // 确认已处理到 offset（含）：游标推进到 offset + 1，不会回退。
  bool Ack(std::string_view topic, std::string_view consumer,
           std::uint64_t offset);

  // 直接设置 consumer 的游标（重放/跳过/重置到 0）。
  bool Seek(std::string_view topic, std::string_view consumer,
            std::uint64_t offset);

  // consumer 的下一条待投递位置（游标）。不存在时返回 0。
  std::uint64_t Cursor(std::string_view topic, std::string_view consumer);

  // topic 的下一个 offset（即已追加条数）。
  std::uint64_t Head(std::string_view topic);

  // 待持久化游标写入 KV（一次 WriteBatch，原子）。
  bool FlushCursors();

  // 把日志与游标都落到磁盘。
  bool Sync();

  QueueStats GetStats() const;
  std::string StatsString() const;

  // 诊断：最近一次错误 / 配置 / topic 列表 / 段数。
  bool ok() const;
  const std::string& LastError() const;
  const QueueOptions& options() const;
  std::vector<std::string> Topics() const;
  std::size_t SegmentCount() const;

  // 日志层直接读取（诊断/冷读复用）：第 seq 条原始记录。
  bool ReadRaw(std::uint64_t seq, std::string* out);

 private:
  class Impl;
  explicit Queue(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace storage
}  // namespace go2cpp

#endif  // GO2CPP_STORAGE_QUEUE_HPP
