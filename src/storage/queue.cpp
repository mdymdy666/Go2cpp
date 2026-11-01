// Copyright 2026 Go2Cpp Authors. All rights reserved.
//
// Queue 实现：多 topic 共用一条 SegLog（LSM 的 WAL 顺序写路径），
// 消费游标以 KV 记录形式持久化（LSM 的 key/value 部件）。

#include "go2cpp/storage/queue.hpp"

#include <algorithm>
#include <atomic>
#include <deque>
#include <limits>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <utility>
#include <vector>

#include "format.hpp"
#include "go2cpp/storage/kv.hpp"
#include "seglog.hpp"

namespace go2cpp {
namespace storage {

namespace {

constexpr char kCursorPrefix[] = "queue/";
// 冷读一次批量预取的条数（把日志里的一段连续装进内存队列）。
constexpr std::size_t kRefillBatch = 4096;

std::string CursorKey(std::string_view topic, std::string_view consumer) {
  std::string key;
  key.reserve(sizeof(kCursorPrefix) + topic.size() + consumer.size() + 8);
  key.append(kCursorPrefix);
  key.append(topic.data(), topic.size());
  key.push_back('/');
  key.append(consumer.data(), consumer.size());
  key.append("/offset");
  return key;
}

// 记录格式：[topic_len:varint][topic][offset:varint][msg]
void EncodeRecord(std::string* out, std::string_view topic, std::uint64_t offset,
                  std::string_view msg) {
  out->clear();
  out->reserve(topic.size() + msg.size() + 24);
  char buf[16];
  char* end =
      internal::EncodeVarint32(buf, static_cast<std::uint32_t>(topic.size()));
  out->append(buf, static_cast<std::size_t>(end - buf));
  out->append(topic.data(), topic.size());
  end = internal::EncodeVarint64(buf, offset);
  out->append(buf, static_cast<std::size_t>(end - buf));
  out->append(msg.data(), msg.size());
}

bool DecodeRecord(std::string_view rec, std::string_view* topic,
                  std::uint64_t* offset, std::string_view* msg) {
  const char* p = rec.data();
  const char* limit = p + rec.size();
  std::uint32_t tlen = 0;
  p = internal::GetVarint32(p, limit, &tlen);
  if (p == nullptr || static_cast<std::size_t>(limit - p) < tlen) {
    return false;
  }
  *topic = std::string_view(p, tlen);
  p += tlen;
  p = internal::GetVarint64(p, limit, offset);
  if (p == nullptr) {
    return false;
  }
  *msg = std::string_view(p, static_cast<std::size_t>(limit - p));
  return true;
}

std::string EncodeCursor(std::uint64_t offset) {
  std::string val(8, '\0');
  internal::EncodeFixed64(&val[0], offset);
  return val;
}

bool DecodeCursor(std::string_view val, std::uint64_t* offset) {
  if (val.size() != 8) {
    return false;
  }
  *offset = internal::DecodeFixed64(val.data());
  return true;
}

}  // namespace

class Queue::Impl {
 public:
  Impl(KV* kv, std::string dir, QueueOptions options)
      : kv_(kv), opt_(options), dir_(std::move(dir)) {}

  ~Impl() { Close(); }

  bool Open() {
    SegLogOptions log_opt;
    log_opt.segment_size = opt_.segment_size;
    log_opt.write_buffer = opt_.write_buffer;
    log_opt.index_interval = opt_.index_interval;
    log_ = std::make_unique<SegLog>();
    if (!log_->Open(dir_, log_opt)) {
      return Fail(log_->last_error());
    }
    if (opt_.recover_topics && log_->next_seq() > 0) {
      RecoverTopics();
    }
    opened_ = true;
    return true;
  }

  void Close() {
    if (!opened_) {
      return;
    }
    if (log_ != nullptr) {
      std::lock_guard<std::mutex> log_lk(log_mu_);
      log_->Sync();
    }
    FlushCursors();
    if (log_ != nullptr) {
      std::lock_guard<std::mutex> log_lk(log_mu_);
      log_->Close();
    }
    opened_ = false;
  }

  bool Append(std::string_view topic, std::string_view msg,
              std::uint64_t* offset) {
    if (!opened_) {
      return Fail("queue not open");
    }
    TopicState& state = GetOrCreate(topic);
    {
      std::lock_guard<std::mutex> lk(state.mu);
      const std::uint64_t off = state.next_offset;
      thread_local std::string rec;
      EncodeRecord(&rec, topic, off, msg);
      std::uint64_t gseq = 0;
      {
        std::lock_guard<std::mutex> log_lk(log_mu_);
        if (!log_->Append(rec, &gseq)) {
          return Fail(log_->last_error());
        }
      }
      Entry entry;
      entry.offset = off;
      entry.global_seq = gseq;
      entry.value.assign(msg.data(), msg.size());
      state.pending.push_back(std::move(entry));
      if (state.pending.size() == 1) {
        state.front_offset = off;
        state.front_global_seq = gseq;
      }
      state.next_offset = off + 1;
      TrimRetained(state);
      stats_.appends.fetch_add(1, std::memory_order_relaxed);
      stats_.produced_bytes.fetch_add(rec.size(), std::memory_order_relaxed);
      if (offset != nullptr) {
        *offset = off;
      }
    }
    if (opt_.sync_on_append) {
      std::lock_guard<std::mutex> log_lk(log_mu_);
      if (!log_->Sync()) {
        return Fail(log_->last_error());
      }
    }
    return true;
  }

  bool Consume(std::string_view topic, std::string_view consumer,
               QueueMessage* msg) {
    if (!opened_) {
      return Fail("queue not open");
    }
    if (msg == nullptr) {
      return Fail("null message");
    }
    TopicState& state = GetOrCreate(topic);
    std::lock_guard<std::mutex> lk(state.mu);
    const std::uint64_t cur = LoadCursorLocked(state, consumer);
    // 内存命中
    if (cur >= state.front_offset &&
        cur - state.front_offset < state.pending.size()) {
      FillMessage(state.pending[cur - state.front_offset], topic, msg);
      return true;
    }
    if (cur >= state.next_offset) {
      return false;  // 没有新消息
    }
    // 冷路径：内存里没有这条（重启后未预取，或已被 retain 回收）。
    if (!RefillLocked(state, topic, cur)) {
      return false;
    }
    if (cur < state.front_offset ||
        cur - state.front_offset >= state.pending.size()) {
      return false;
    }
    FillMessage(state.pending[cur - state.front_offset], topic, msg);
    return true;
  }

  bool Ack(std::string_view topic, std::string_view consumer,
           std::uint64_t offset) {
    if (!opened_) {
      return Fail("queue not open");
    }
    TopicState& state = GetOrCreate(topic);
    bool need_flush = false;
    {
      std::lock_guard<std::mutex> lk(state.mu);
      std::uint64_t cur = LoadCursorLocked(state, consumer);
      if (offset + 1 > cur) {
        cur = offset + 1;
        state.cursors[std::string(consumer)] = cur;
        MarkDirty(topic, consumer, cur);
        stats_.acks.fetch_add(1, std::memory_order_relaxed);
        ReclaimLocked(state);
        need_flush = BumpAckLocked();
      }
    }
    if (need_flush) {
      FlushCursors();
    }
    return true;
  }

  bool Seek(std::string_view topic, std::string_view consumer,
            std::uint64_t offset) {
    if (!opened_) {
      return Fail("queue not open");
    }
    TopicState& state = GetOrCreate(topic);
    std::lock_guard<std::mutex> lk(state.mu);
    state.cursors[std::string(consumer)] = offset;
    MarkDirty(topic, consumer, offset);
    ReclaimLocked(state);
    return true;
  }

  std::uint64_t Cursor(std::string_view topic, std::string_view consumer) {
    TopicState& state = GetOrCreate(topic);
    std::lock_guard<std::mutex> lk(state.mu);
    return LoadCursorLocked(state, consumer);
  }

  std::uint64_t Head(std::string_view topic) {
    TopicState& state = GetOrCreate(topic);
    std::lock_guard<std::mutex> lk(state.mu);
    return state.next_offset;
  }

  bool FlushCursors() {
    std::vector<DirtyCursor> batch;
    {
      std::lock_guard<std::mutex> lk(cursor_mu_);
      batch.swap(dirty_);
      ack_since_flush_ = 0;
    }
    if (batch.empty() || kv_ == nullptr) {
      return true;  // 无 LSM 绑定：游标只存内存
    }
    WriteBatch wb;
    for (const auto& c : batch) {
      wb.Put(CursorKey(c.topic, c.consumer), EncodeCursor(c.offset));
    }
    if (!kv_->Write(wb)) {
      stats_.persist_errors.fetch_add(1, std::memory_order_relaxed);
      return Fail("cursor persist failed: " + kv_->LastError());
    }
    stats_.cursor_flushes.fetch_add(1, std::memory_order_relaxed);
    stats_.cursor_writes.fetch_add(batch.size(), std::memory_order_relaxed);
    return true;
  }

  bool Sync() {
    if (log_ != nullptr) {
      std::lock_guard<std::mutex> log_lk(log_mu_);
      if (!log_->Sync()) {
        return Fail(log_->last_error());
      }
    }
    return FlushCursors();
  }

  bool ReadRaw(std::uint64_t seq, std::string* out) {
    if (log_ == nullptr) {
      return false;
    }
    return log_->Read(seq, out);
  }

  QueueStats GetStats() const {
    QueueStats s;
    s.appends = stats_.appends.load(std::memory_order_relaxed);
    s.consumes = stats_.consumes.load(std::memory_order_relaxed);
    s.acks = stats_.acks.load(std::memory_order_relaxed);
    s.cold_reads = stats_.cold_reads.load(std::memory_order_relaxed);
    s.cold_scanned = stats_.cold_scanned.load(std::memory_order_relaxed);
    s.cursor_flushes = stats_.cursor_flushes.load(std::memory_order_relaxed);
    s.cursor_writes = stats_.cursor_writes.load(std::memory_order_relaxed);
    s.persist_errors = stats_.persist_errors.load(std::memory_order_relaxed);
    s.produced_bytes = stats_.produced_bytes.load(std::memory_order_relaxed);
    if (log_ != nullptr) {
      s.buffered_bytes = log_->buffered_bytes();
      s.segments = log_->segment_count();
    }
    std::lock_guard<std::mutex> lk(topics_mu_);
    for (const auto& kv : topics_) {
      const TopicState& st = *kv.second;
      std::lock_guard<std::mutex> topic_lk(st.mu);
      s.retained += st.pending.size();
      s.consumers += st.cursors.size();
    }
    s.topics = topics_.size();
    return s;
  }

  std::string StatsString() const {
    const QueueStats s = GetStats();
    std::ostringstream os;
    os << "Queue{appends=" << s.appends << ", consumes=" << s.consumes
       << ", acks=" << s.acks << ", cold_reads=" << s.cold_reads
       << ", retained=" << s.retained << ", topics=" << s.topics
       << ", consumers=" << s.consumers << ", segments=" << s.segments
       << ", bytes=" << s.produced_bytes << "}";
    return os.str();
  }

  std::vector<std::string> Topics() const {
    std::vector<std::string> out;
    std::lock_guard<std::mutex> lk(topics_mu_);
    out.reserve(topics_.size());
    for (const auto& kv : topics_) {
      out.push_back(kv.first);
    }
    return out;
  }

  std::size_t SegmentCount() const {
    return log_ != nullptr ? log_->segment_count() : 0;
  }

  bool ok() const {
    std::lock_guard<std::mutex> lk(err_mu_);
    return error_.empty();
  }

  const std::string& LastError() const { return error_; }

  const QueueOptions& options() const { return opt_; }

  bool Fail(const std::string& what) {
    std::lock_guard<std::mutex> lk(err_mu_);
    if (error_.empty()) {
      error_ = what;
    }
    return false;
  }

 private:
  struct Entry {
    std::uint64_t offset = 0;      // topic 内序号
    std::uint64_t global_seq = 0;  // 日志全局序号（冷读起点）
    std::string value;
  };

  struct TopicState {
    std::string name;
    mutable std::mutex mu;
    std::deque<Entry> pending;
    std::uint64_t front_offset = 0;      // pending.front().offset
    std::uint64_t front_global_seq = 0;  // pending.front().global_seq
    std::uint64_t next_offset = 0;
    std::unordered_map<std::string, std::uint64_t> cursors;
  };

  struct DirtyCursor {
    std::string topic;
    std::string consumer;
    std::uint64_t offset = 0;
  };

  struct Counters {
    std::atomic<std::uint64_t> appends{0};
    std::atomic<std::uint64_t> consumes{0};
    std::atomic<std::uint64_t> acks{0};
    std::atomic<std::uint64_t> cold_reads{0};
    std::atomic<std::uint64_t> cold_scanned{0};
    std::atomic<std::uint64_t> cursor_flushes{0};
    std::atomic<std::uint64_t> cursor_writes{0};
    std::atomic<std::uint64_t> persist_errors{0};
    std::atomic<std::uint64_t> produced_bytes{0};
  };

  void FillMessage(const Entry& entry, std::string_view topic,
                   QueueMessage* msg) {
    msg->offset = entry.offset;
    msg->topic.assign(topic.data(), topic.size());
    msg->value = entry.value;
    stats_.consumes.fetch_add(1, std::memory_order_relaxed);
  }

  // 热路径优化：thread_local 记住最近用过的 topic，稳态下省掉一次全局锁
  // 与一次 std::string 构造（1e7 次 Append 各一次，实测占 append 的 15%）。
  // TopicState 只增不删，指针稳定；用内容比较而不是指针比较，避免调用方
  // 复用同一块 buffer 导致误命中。
  TopicState& GetOrCreate(std::string_view topic) {
    static thread_local TopicState* cached = nullptr;
    if (opt_.topic_cache && cached != nullptr &&
        cached->name.size() == topic.size() &&
        std::memcmp(cached->name.data(), topic.data(), topic.size()) == 0) {
      return *cached;
    }
    std::lock_guard<std::mutex> lk(topics_mu_);
    auto it = topics_.find(std::string(topic));
    if (it != topics_.end()) {
      if (opt_.topic_cache) {
        cached = it->second.get();
      }
      return *it->second;
    }
    auto state = std::make_unique<TopicState>();
    state->name.assign(topic.data(), topic.size());
    TopicState* raw = state.get();
    topics_.emplace(std::string(topic), std::move(state));
    if (opt_.topic_cache) {
      cached = raw;
    }
    return *raw;
  }

  // 需要持 state.mu。
  std::uint64_t LoadCursorLocked(TopicState& state,
                                 std::string_view consumer) {
    auto it = state.cursors.find(std::string(consumer));
    if (it != state.cursors.end()) {
      return it->second;
    }
    std::uint64_t cur = 0;
    if (kv_ != nullptr) {
      std::string val;
      if (kv_->Get(CursorKey(state.name, consumer), &val)) {
        DecodeCursor(val, &cur);
      }
    }
    state.cursors.emplace(std::string(consumer), cur);
    return cur;
  }

  void MarkDirty(std::string_view topic, std::string_view consumer,
                 std::uint64_t offset) {
    std::lock_guard<std::mutex> lk(cursor_mu_);
    for (auto& c : dirty_) {
      if (c.consumer == consumer && c.topic == topic) {
        c.offset = offset;
        return;
      }
    }
    DirtyCursor c;
    c.topic.assign(topic.data(), topic.size());
    c.consumer.assign(consumer.data(), consumer.size());
    c.offset = offset;
    dirty_.push_back(std::move(c));
  }

  // 需要持 state.mu。
  bool BumpAckLocked() {
    std::lock_guard<std::mutex> lk(cursor_mu_);
    ++ack_since_flush_;
    return opt_.cursor_flush_interval != 0 &&
           ack_since_flush_ >= opt_.cursor_flush_interval;
  }

  // 需要持 state.mu。回收所有 consumer 都已确认的内存条目。
  void ReclaimLocked(TopicState& state) {
    if (state.pending.empty() || state.cursors.empty()) {
      return;
    }
    std::uint64_t min_cur = std::numeric_limits<std::uint64_t>::max();
    for (const auto& kv : state.cursors) {
      min_cur = std::min(min_cur, kv.second);
    }
    while (!state.pending.empty() && state.pending.front().offset < min_cur) {
      state.front_global_seq = state.pending.front().global_seq + 1;
      ++state.front_offset;
      state.pending.pop_front();
    }
  }

  // retain 上限：超过后丢内存副本（消费转冷路径），不影响已持久化日志。
  // 注意这里**不**推进 front_global_seq：被丢掉的条目还没有被任何 consumer
  // 确认，冷读仍可能要从它们之前的位置开始扫。
  void TrimRetained(TopicState& state) {
    if (opt_.retain == 0) {
      return;
    }
    while (state.pending.size() > opt_.retain) {
      ++state.front_offset;
      state.pending.pop_front();
    }
  }

  // 从日志预取一段消息进内存队列。需要持 state.mu。
  bool RefillLocked(TopicState& state, std::string_view topic,
                    std::uint64_t want) {
    if (log_ == nullptr) {
      return false;
    }
    {
      // 冷读可能发生在写缓冲尚未落盘时（同进程刚 Append 完就 Consume），
      // 先把用户态缓冲刷进内核文件，否则读端看不到尾部记录。
      std::lock_guard<std::mutex> log_lk(log_mu_);
      if (!log_->Flush()) {
        return false;
      }
    }
    // 游标落后于已回收区间（Seek 回退 / retain 丢弃）时，front_global_seq
    // 指向的已经不是目标消息之前的记录，必须从头扫。
    std::uint64_t start_seq = state.front_global_seq;
    if (want < state.front_offset) {
      start_seq = 0;
    }
    SegLog::Cursor cursor;
    if (!cursor.Seek(log_.get(), start_seq)) {
      return false;
    }
    std::vector<Entry> batch;
    batch.reserve(kRefillBatch);
    std::string_view rec;
    std::uint64_t scanned = 0;
    while (batch.size() < kRefillBatch && cursor.Next(&rec)) {
      const std::uint64_t gseq = cursor.seq() - 1;
      ++scanned;
      std::string_view rt;
      std::uint64_t roff = 0;
      std::string_view rval;
      if (!DecodeRecord(rec, &rt, &roff, &rval)) {
        continue;
      }
      if (rt != topic) {
        continue;
      }
      if (roff < want) {
        continue;
      }
      if (batch.empty() && roff > want) {
        want = roff;  // 目标已被淘汰：从现存最早的一条继续
      }
      Entry entry;
      entry.offset = roff;
      entry.global_seq = gseq;
      entry.value.assign(rval.data(), rval.size());
      batch.push_back(std::move(entry));
    }
    stats_.cold_scanned.fetch_add(scanned, std::memory_order_relaxed);
    stats_.cold_reads.fetch_add(1, std::memory_order_relaxed);
    if (batch.empty()) {
      return false;
    }
    state.pending.clear();
    for (auto& e : batch) {
      state.pending.push_back(std::move(e));
    }
    state.front_offset = state.pending.front().offset;
    state.front_global_seq = state.pending.front().global_seq;
    if (state.pending.back().offset + 1 > state.next_offset) {
      state.next_offset = state.pending.back().offset + 1;
    }
    // 这里刻意不 TrimRetained：刚预取的一批必须留给消费者，否则下一条
    // 消费又落到冷路径，每次都从头扫，退化成 O(n^2)。retain 只约束
    // Append 侧新消息的堆积。
    return true;
  }

  void RecoverTopics() {
    if (log_ == nullptr) {
      return;
    }
    SegLog::Cursor cursor;
    if (!cursor.Seek(log_.get(), 0)) {
      return;
    }
    std::string_view rec;
    while (cursor.Next(&rec)) {
      std::string_view topic;
      std::uint64_t offset = 0;
      std::string_view value;
      if (!DecodeRecord(rec, &topic, &offset, &value)) {
        continue;
      }
      TopicState& state = GetOrCreate(topic);
      std::lock_guard<std::mutex> lk(state.mu);
      if (offset + 1 > state.next_offset) {
        state.next_offset = offset + 1;
      }
    }
  }

  KV* kv_ = nullptr;
  QueueOptions opt_;
  std::string dir_;
  std::unique_ptr<SegLog> log_;
  mutable std::mutex topics_mu_;
  std::unordered_map<std::string, std::unique_ptr<TopicState>> topics_;
  std::mutex log_mu_;  // 保护 SegLog 写端
  mutable std::mutex err_mu_;
  std::string error_;
  Counters stats_;
  std::mutex cursor_mu_;
  std::vector<DirtyCursor> dirty_;
  std::size_t ack_since_flush_ = 0;
  bool opened_ = false;
};

Queue::Queue(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

Queue::~Queue() = default;

std::unique_ptr<Queue> Queue::Open(KV* kv, const std::string& dir,
                                   const QueueOptions& options) {
  auto impl = std::make_unique<Impl>(kv, dir, options);
  if (!impl->Open()) {
    return nullptr;
  }
  return std::unique_ptr<Queue>(new Queue(std::move(impl)));
}

bool Queue::Append(std::string_view topic, std::string_view msg,
                   std::uint64_t* offset) {
  return impl_->Append(topic, msg, offset);
}

bool Queue::Consume(std::string_view topic, std::string_view consumer,
                    QueueMessage* msg) {
  return impl_->Consume(topic, consumer, msg);
}

bool Queue::Ack(std::string_view topic, std::string_view consumer,
                std::uint64_t offset) {
  return impl_->Ack(topic, consumer, offset);
}

bool Queue::Seek(std::string_view topic, std::string_view consumer,
                 std::uint64_t offset) {
  return impl_->Seek(topic, consumer, offset);
}

std::uint64_t Queue::Cursor(std::string_view topic,
                            std::string_view consumer) {
  return impl_->Cursor(topic, consumer);
}

std::uint64_t Queue::Head(std::string_view topic) { return impl_->Head(topic); }

bool Queue::FlushCursors() { return impl_->FlushCursors(); }

bool Queue::Sync() { return impl_->Sync(); }

QueueStats Queue::GetStats() const { return impl_->GetStats(); }

std::string Queue::StatsString() const { return impl_->StatsString(); }

bool Queue::ok() const { return impl_->ok(); }

const std::string& Queue::LastError() const { return impl_->LastError(); }

const QueueOptions& Queue::options() const { return impl_->options(); }

std::vector<std::string> Queue::Topics() const { return impl_->Topics(); }

std::size_t Queue::SegmentCount() const { return impl_->SegmentCount(); }

bool Queue::ReadRaw(std::uint64_t seq, std::string* out) {
  return impl_->ReadRaw(seq, out);
}

}  // namespace storage
}  // namespace go2cpp
