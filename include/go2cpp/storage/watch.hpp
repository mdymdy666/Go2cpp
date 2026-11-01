// Copyright 2026 Go2Cpp Authors. All rights reserved.
// Watch：基于同一套 LSM 的键空间变更订阅。
//
// 设计（对应 LSM 的 WAL 变更通知）：
//   * 写路径包装 KV::Put / KV::Delete：写成功之后，按前缀匹配订阅者，
//     把变更事件追加进变更日志（复用 SegLog = WAL 块格式 + SSTable 段索引），
//     再派发回调。
//   * 订阅者游标（每个订阅已派发到哪个 seq）持久化进 KV：
//     key = "watch/<id>/offset"。订阅者「从哪个 offset 开始」就是这条记录，
//     重启后可以按游标重放（Replay）没来得及处理的历史事件。
//   * 没有订阅者时写路径只多一次原子读（~1ns），不写事件日志 —— 这是
//     Subscribe 相对 Queue 的关键差别：通知是「有人听才记录」。
//
// 派发模式：
//   * 同步（默认）：写线程内直接回调，顺序严格 = 写入顺序，零队列开销；
//   * 异步：写线程入队、单后台线程派发。队列满时阻塞（背压），不丢事件。
//
// 顺序保证：同一 Watcher 实例内，事件 seq 严格递增，派发顺序与 seq 一致；
// 同步模式下回调返回时对应 KV 写入已完成（可以立刻 Get 到新值）。

#ifndef GO2CPP_STORAGE_WATCH_HPP
#define GO2CPP_STORAGE_WATCH_HPP

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace go2cpp {
namespace storage {

class KV;
class SegLog;
class WriteBatch;

struct WatchOptions {
  // 事件日志的段滚动阈值 / 写缓冲 / 索引间隔（同 SegLog）。
  std::size_t segment_size = 64u << 20;
  std::size_t write_buffer = 1u << 20;
  std::size_t index_interval = 512;
  // true：写线程内同步回调（顺序最强）；false：单后台线程异步派发。
  bool sync_callbacks = true;
  // 异步模式的待派发队列上限；满了写线程阻塞（背压），不丢事件。
  std::size_t queue_capacity = 65536;
  // 是否把事件写进变更日志（false = 纯内存通知，不能 Replay）。
  bool persist_events = true;
  // 每 N 次游标推进批量写一次 KV（0 = 只在 Flush 时写）。
  std::size_t cursor_flush_interval = 1024;
};

// 一次变更事件。
struct WatchEvent {
  std::uint64_t seq = 0;         // 全局事件序号（与变更日志一致）
  std::string key;
  std::string value;             // 删除事件为空
  bool deleted = false;
  std::uint64_t timestamp_ns = 0;
};

struct WatchStats {
  std::uint64_t writes = 0;        // 包装的 Put/Delete/Write 次数
  std::uint64_t events = 0;        // 产生的事件数（有订阅者且匹配）
  std::uint64_t delivered = 0;     // 回调调用次数（多订阅者分别计数）
  std::uint64_t matched = 0;       // 命中至少一个订阅者的事件数
  std::uint64_t replayed = 0;      // Replay 派发的事件数
  std::uint64_t cursor_flushes = 0;
  std::uint64_t callback_ns = 0;   // 回调累计耗时（同步模式）
  std::uint64_t async_backpressure = 0;  // 异步队列满导致写线程等待的次数
  std::uint64_t subscriptions = 0;       // 当前订阅数
  std::uint64_t last_seq = 0;
  std::uint64_t log_bytes = 0;
};

class Watcher {
 public:
  // 回调在「写已经落到 LSM（MemTable+WAL）」之后被调用。
  using Callback = std::function<void(const WatchEvent&)>;

  // kv 必须非空。dir 是变更日志目录（通常 <kv_dir>/watch）。
  static std::unique_ptr<Watcher> Open(KV* kv, const std::string& dir,
                                       const WatchOptions& options =
                                           WatchOptions());
  ~Watcher();

  Watcher(const Watcher&) = delete;
  Watcher& operator=(const Watcher&) = delete;

  // 订阅一个 key 前缀。stable_id = 0 时自动分配；非 0 时用它作为游标
  // 在 KV 里的名字（重启后可用同一个 id 续上）。
  std::uint64_t Watch(std::string_view prefix, Callback cb,
                      std::uint64_t stable_id = 0);
  bool Unwatch(std::uint64_t id);
  std::size_t SubscriptionCount() const;

  // 写路径（等价于 kv->Put/Delete/Write，额外触发订阅回调）。
  bool Put(std::string_view key, std::string_view value);
  bool Delete(std::string_view key);
  bool Write(const WriteBatch& batch);

  // 通知一个「不经过本 Watcher」的变更（外部写 / 复制流）。
  bool Notify(std::string_view key, std::string_view value, bool deleted);

  // 从变更日志重放 seq >= from_seq 且匹配订阅前缀的事件。
  bool Replay(std::uint64_t id, std::uint64_t from_seq);

  // 订阅游标（已派发到的 seq）。
  std::uint64_t Offset(std::uint64_t id) const;
  // 把全部订阅游标写进 KV。
  bool FlushOffsets();

  std::uint64_t last_seq() const;
  bool Flush();
  bool Sync();

  WatchStats GetStats() const;
  std::string StatsString() const;

  bool ok() const;
  const std::string& LastError() const;
  const WatchOptions& options() const;

 private:
  class Impl;
  explicit Watcher(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace storage
}  // namespace go2cpp

#endif  // GO2CPP_STORAGE_WATCH_HPP
