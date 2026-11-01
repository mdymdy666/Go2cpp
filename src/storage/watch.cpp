// Copyright 2026 Go2Cpp Authors. All rights reserved.
//
// Watch 实现：变更事件写 SegLog（WAL 块格式），订阅游标写 KV。

#include "go2cpp/storage/watch.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <utility>

#include "format.hpp"
#include "go2cpp/storage/kv.hpp"
#include "seglog.hpp"

namespace go2cpp {
namespace storage {

namespace {

constexpr char kOffsetPrefix[] = "watch/";

std::uint64_t NowNs() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

std::string OffsetKey(std::uint64_t id) {
  return std::string(kOffsetPrefix) + std::to_string(id) + "/offset";
}

// [key_len:varint][key][flags:u8][value]
void EncodeEvent(std::string* out, std::string_view key, std::string_view value,
                 bool deleted) {
  out->clear();
  out->reserve(key.size() + value.size() + 12);
  char buf[16];
  char* end =
      internal::EncodeVarint32(buf, static_cast<std::uint32_t>(key.size()));
  out->append(buf, static_cast<std::size_t>(end - buf));
  out->append(key.data(), key.size());
  out->push_back(static_cast<char>(deleted ? 1 : 0));
  out->append(value.data(), value.size());
}

bool DecodeEvent(std::string_view rec, std::string* key, std::string* value,
                 bool* deleted) {
  const char* p = rec.data();
  const char* limit = p + rec.size();
  std::uint32_t klen = 0;
  p = internal::GetVarint32(p, limit, &klen);
  if (p == nullptr || static_cast<std::size_t>(limit - p) < klen + 1) {
    return false;
  }
  key->assign(p, klen);
  p += klen;
  *deleted = (*p != 0);
  ++p;
  value->assign(p, static_cast<std::size_t>(limit - p));
  return true;
}

bool PrefixMatch(std::string_view prefix, std::string_view key) {
  return key.size() >= prefix.size() &&
         key.compare(0, prefix.size(), prefix) == 0;
}

}  // namespace

class Watcher::Impl {
 public:
  Impl(KV* kv, std::string dir, WatchOptions options)
      : kv_(kv), opt_(options), dir_(std::move(dir)) {}

  ~Impl() { Close(); }

  bool Open() {
    if (kv_ == nullptr) {
      return Fail("watcher requires a KV instance");
    }
    SegLogOptions log_opt;
    log_opt.segment_size = opt_.segment_size;
    log_opt.write_buffer = opt_.write_buffer;
    log_opt.index_interval = opt_.index_interval;
    log_ = std::make_unique<SegLog>();
    if (!log_->Open(dir_, log_opt)) {
      return Fail(log_->last_error());
    }
    last_seq_.store(log_->next_seq() == 0 ? 0 : log_->next_seq() - 1,
                    std::memory_order_relaxed);
    if (!opt_.sync_callbacks) {
      worker_ = std::thread(&Impl::WorkerLoop, this);
    }
    opened_ = true;
    return true;
  }

  void Close() {
    if (!opened_) {
      return;
    }
    opened_ = false;
    if (worker_.joinable()) {
      {
        std::lock_guard<std::mutex> lk(q_mu_);
        stop_ = true;
      }
      q_cv_.notify_all();
      q_space_cv_.notify_all();
      worker_.join();
    }
    FlushOffsets();
    if (log_ != nullptr) {
      std::lock_guard<std::mutex> lk(log_mu_);
      log_->Close();
    }
  }

  std::uint64_t Watch(std::string_view prefix, Callback cb,
                      std::uint64_t stable_id) {
    if (!opened_ || !cb) {
      Fail("watcher not open or null callback");
      return 0;
    }
    const std::uint64_t id = stable_id != 0 ? stable_id
                                            : next_id_.fetch_add(1);
    auto sub = std::make_shared<Subscription>();
    sub->id = id;
    sub->prefix.assign(prefix.data(), prefix.size());
    sub->cb = std::move(cb);

    std::uint64_t start = last_seq_.load(std::memory_order_relaxed);
    if (stable_id != 0) {
      std::string val;
      if (kv_->Get(OffsetKey(id), &val) && val.size() == 8) {
        start = internal::DecodeFixed64(val.data());
      }
    }
    sub->last_seq.store(start, std::memory_order_relaxed);

    std::lock_guard<std::mutex> lk(subs_mu_);
    subs_[id] = sub;
    sub_count_.store(subs_.size(), std::memory_order_release);
    return id;
  }

  bool Unwatch(std::uint64_t id) {
    std::lock_guard<std::mutex> lk(subs_mu_);
    const auto it = subs_.find(id);
    if (it == subs_.end()) {
      return false;
    }
    subs_.erase(it);
    sub_count_.store(subs_.size(), std::memory_order_release);
    return true;
  }

  std::size_t SubscriptionCount() const {
    return sub_count_.load(std::memory_order_acquire);
  }

  bool Put(std::string_view key, std::string_view value) {
    ++writes_;
    if (!kv_->Put(key, value)) {
      return Fail("kv put failed: " + kv_->LastError());
    }
    return Notify(key, value, false);
  }

  bool Delete(std::string_view key) {
    ++writes_;
    if (!kv_->Delete(key)) {
      return Fail("kv delete failed: " + kv_->LastError());
    }
    return Notify(key, std::string_view(), true);
  }

  bool Write(const WriteBatch& batch) {
    ++writes_;
    if (!kv_->Write(batch)) {
      return Fail("kv write failed: " + kv_->LastError());
    }
    if (sub_count_.load(std::memory_order_acquire) == 0) {
      return true;
    }
    // 解析批的序列化格式（与 KV::ApplyBatch 同构），逐条发事件。
    const std::string& data = batch.Data();
    if (data.size() < 4) {
      return true;
    }
    const char* p = data.data();
    const char* limit = p + data.size();
    const std::uint32_t count = internal::DecodeFixed32(p);
    p += 4;
    for (std::uint32_t i = 0; i < count; ++i) {
      if (p >= limit) {
        break;
      }
      const bool deleted = static_cast<std::uint8_t>(*p) == 0;
      ++p;
      std::uint32_t klen = 0;
      p = internal::GetVarint32(p, limit, &klen);
      if (p == nullptr || static_cast<std::size_t>(limit - p) < klen) {
        break;
      }
      const std::string_view key(p, klen);
      p += klen;
      std::uint32_t vlen = 0;
      p = internal::GetVarint32(p, limit, &vlen);
      if (p == nullptr || static_cast<std::size_t>(limit - p) < vlen) {
        break;
      }
      const std::string_view value(p, vlen);
      p += vlen;
      if (!Notify(key, value, deleted)) {
        return false;
      }
    }
    return true;
  }

  bool Notify(std::string_view key, std::string_view value, bool deleted) {
    if (sub_count_.load(std::memory_order_acquire) == 0) {
      return true;  // 零成本快路径：没人订阅就不记录
    }
    std::vector<std::shared_ptr<Subscription>> matched;
    {
      std::lock_guard<std::mutex> lk(subs_mu_);
      matched.reserve(subs_.size());
      for (auto& item : subs_) {
        if (PrefixMatch(item.second->prefix, key)) {
          matched.push_back(item.second);
        }
      }
    }
    if (matched.empty()) {
      return true;
    }

    WatchEvent event;
    event.key.assign(key.data(), key.size());
    event.value.assign(value.data(), value.size());
    event.deleted = deleted;
    event.timestamp_ns = NowNs();

    if (opt_.persist_events) {
      thread_local std::string rec;
      EncodeEvent(&rec, key, value, deleted);
      std::uint64_t seq = 0;
      std::lock_guard<std::mutex> lk(log_mu_);
      if (!log_->Append(rec, &seq)) {
        return Fail(log_->last_error());
      }
      event.seq = seq;
      last_seq_.store(seq, std::memory_order_relaxed);
    } else {
      event.seq = mem_seq_.fetch_add(1, std::memory_order_relaxed) + 1;
      last_seq_.store(event.seq, std::memory_order_relaxed);
    }
    events_.fetch_add(1, std::memory_order_relaxed);
    matched_.fetch_add(1, std::memory_order_relaxed);

    if (opt_.sync_callbacks) {
      Dispatch(matched, event);
    } else {
      Enqueue(std::move(matched), std::move(event));
    }
    return true;
  }

  bool Replay(std::uint64_t id, std::uint64_t from_seq) {
    if (!opt_.persist_events || log_ == nullptr) {
      return Fail("replay requires persisted events");
    }
    std::shared_ptr<Subscription> sub;
    {
      std::lock_guard<std::mutex> lk(subs_mu_);
      const auto it = subs_.find(id);
      if (it == subs_.end()) {
        return Fail("unknown subscription");
      }
      sub = it->second;
    }
    {
      // 重放读的是文件；先把写缓冲刷进内核，否则刚产生的事件还不可见。
      std::lock_guard<std::mutex> lk(log_mu_);
      log_->Flush();
    }
    SegLog::Cursor cursor;
    if (!cursor.Seek(log_.get(), from_seq)) {
      return false;
    }
    std::string_view rec;
    while (cursor.Next(&rec)) {
      WatchEvent event;
      if (!DecodeEvent(rec, &event.key, &event.value, &event.deleted)) {
        continue;
      }
      event.seq = cursor.seq() - 1;
      if (!PrefixMatch(sub->prefix, event.key)) {
        continue;
      }
      event.timestamp_ns = NowNs();
      sub->last_seq.store(event.seq, std::memory_order_relaxed);
      const std::uint64_t t0 = NowNs();
      try {
        sub->cb(event);
      } catch (...) {
        callback_errors_.fetch_add(1, std::memory_order_relaxed);
      }
      callback_ns_.fetch_add(NowNs() - t0, std::memory_order_relaxed);
      delivered_.fetch_add(1, std::memory_order_relaxed);
      replayed_.fetch_add(1, std::memory_order_relaxed);
    }
    return true;
  }

  std::uint64_t Offset(std::uint64_t id) const {
    std::lock_guard<std::mutex> lk(subs_mu_);
    const auto it = subs_.find(id);
    if (it == subs_.end()) {
      return 0;
    }
    return it->second->last_seq.load(std::memory_order_relaxed);
  }

  bool FlushOffsets() {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> pending;
    {
      std::lock_guard<std::mutex> lk(subs_mu_);
      pending.reserve(subs_.size());
      for (auto& item : subs_) {
        const std::uint64_t seq =
            item.second->last_seq.load(std::memory_order_relaxed);
        if (persisted_[item.first] != seq) {
          pending.emplace_back(item.first, seq);
        }
      }
      for (const auto& p : pending) {
        persisted_[p.first] = p.second;
      }
    }
    if (pending.empty()) {
      return true;
    }
    WriteBatch wb;
    for (const auto& p : pending) {
      std::string val(8, '\0');
      internal::EncodeFixed64(&val[0], p.second);
      wb.Put(OffsetKey(p.first), val);
    }
    if (!kv_->Write(wb)) {
      return Fail("offset persist failed: " + kv_->LastError());
    }
    cursor_flushes_.fetch_add(1, std::memory_order_relaxed);
    return true;
  }

  std::uint64_t last_seq() const {
    return last_seq_.load(std::memory_order_relaxed);
  }

  bool Flush() {
    if (log_ == nullptr) {
      return false;
    }
    std::lock_guard<std::mutex> lk(log_mu_);
    return log_->Flush();
  }

  bool Sync() {
    bool ok = true;
    if (log_ != nullptr) {
      std::lock_guard<std::mutex> lk(log_mu_);
      ok = log_->Sync();
    }
    return FlushOffsets() && ok;
  }

  WatchStats GetStats() const {
    WatchStats s;
    s.writes = writes_.load(std::memory_order_relaxed);
    s.events = events_.load(std::memory_order_relaxed);
    s.delivered = delivered_.load(std::memory_order_relaxed);
    s.matched = matched_.load(std::memory_order_relaxed);
    s.replayed = replayed_.load(std::memory_order_relaxed);
    s.cursor_flushes = cursor_flushes_.load(std::memory_order_relaxed);
    s.callback_ns = callback_ns_.load(std::memory_order_relaxed);
    s.async_backpressure =
        backpressure_.load(std::memory_order_relaxed);
    s.subscriptions = SubscriptionCount();
    s.last_seq = last_seq();
    s.log_bytes = log_ != nullptr ? log_->bytes() : 0;
    return s;
  }

  std::string StatsString() const {
    const WatchStats s = GetStats();
    std::ostringstream os;
    os << "Watcher{writes=" << s.writes << ", events=" << s.events
       << ", delivered=" << s.delivered << ", subs=" << s.subscriptions
       << ", last_seq=" << s.last_seq << ", log_bytes=" << s.log_bytes << "}";
    return os.str();
  }

  bool ok() const {
    std::lock_guard<std::mutex> lk(err_mu_);
    return error_.empty();
  }

  const std::string& LastError() const { return error_; }

  const WatchOptions& options() const { return opt_; }

  bool Fail(const std::string& what) {
    std::lock_guard<std::mutex> lk(err_mu_);
    if (error_.empty()) {
      error_ = what;
    }
    return false;
  }

 private:
  struct Subscription {
    std::uint64_t id = 0;
    std::string prefix;
    Callback cb;
    std::atomic<std::uint64_t> last_seq{0};
  };

  struct AsyncItem {
    std::vector<std::shared_ptr<Subscription>> subs;
    WatchEvent event;
  };

  void Dispatch(const std::vector<std::shared_ptr<Subscription>>& subs,
                const WatchEvent& event) {
    for (const auto& sub : subs) {
      sub->last_seq.store(event.seq, std::memory_order_relaxed);
      const std::uint64_t t0 = NowNs();
      try {
        sub->cb(event);
      } catch (...) {
        callback_errors_.fetch_add(1, std::memory_order_relaxed);
      }
      callback_ns_.fetch_add(NowNs() - t0, std::memory_order_relaxed);
      delivered_.fetch_add(1, std::memory_order_relaxed);
    }
  }

  void Enqueue(std::vector<std::shared_ptr<Subscription>> subs,
               WatchEvent event) {
    std::unique_lock<std::mutex> lk(q_mu_);
    if (q_.size() >= opt_.queue_capacity) {
      backpressure_.fetch_add(1, std::memory_order_relaxed);
      q_space_cv_.wait(lk, [this] {
        return q_.size() < opt_.queue_capacity || stop_;
      });
    }
    AsyncItem item;
    item.subs = std::move(subs);
    item.event = std::move(event);
    q_.push_back(std::move(item));
    q_cv_.notify_one();
  }

  void WorkerLoop() {
    for (;;) {
      AsyncItem item;
      {
        std::unique_lock<std::mutex> lk(q_mu_);
        q_cv_.wait(lk, [this] { return stop_ || !q_.empty(); });
        if (q_.empty()) {
          return;
        }
        item = std::move(q_.front());
        q_.pop_front();
      }
      q_space_cv_.notify_one();
      Dispatch(item.subs, item.event);
    }
  }

  KV* kv_ = nullptr;
  WatchOptions opt_;
  std::string dir_;
  std::unique_ptr<SegLog> log_;
  std::mutex log_mu_;

  mutable std::mutex subs_mu_;
  std::unordered_map<std::uint64_t, std::shared_ptr<Subscription>> subs_;
  std::unordered_map<std::uint64_t, std::uint64_t> persisted_;
  std::atomic<std::size_t> sub_count_{0};
  std::atomic<std::uint64_t> next_id_{1};
  std::atomic<std::uint64_t> last_seq_{0};
  std::atomic<std::uint64_t> mem_seq_{0};

  mutable std::mutex err_mu_;
  std::string error_;
  bool opened_ = false;

  std::mutex q_mu_;
  std::condition_variable q_cv_;
  std::condition_variable q_space_cv_;
  std::deque<AsyncItem> q_;
  std::thread worker_;
  bool stop_ = false;

  std::atomic<std::uint64_t> writes_{0};
  std::atomic<std::uint64_t> events_{0};
  std::atomic<std::uint64_t> matched_{0};
  std::atomic<std::uint64_t> delivered_{0};
  std::atomic<std::uint64_t> replayed_{0};
  std::atomic<std::uint64_t> cursor_flushes_{0};
  std::atomic<std::uint64_t> callback_ns_{0};
  std::atomic<std::uint64_t> backpressure_{0};
  std::atomic<std::uint64_t> callback_errors_{0};
};

Watcher::Watcher(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

Watcher::~Watcher() = default;

std::unique_ptr<Watcher> Watcher::Open(KV* kv, const std::string& dir,
                                       const WatchOptions& options) {
  auto impl = std::make_unique<Impl>(kv, dir, options);
  if (!impl->Open()) {
    return nullptr;
  }
  return std::unique_ptr<Watcher>(new Watcher(std::move(impl)));
}

std::uint64_t Watcher::Watch(std::string_view prefix, Callback cb,
                             std::uint64_t stable_id) {
  return impl_->Watch(prefix, std::move(cb), stable_id);
}

bool Watcher::Unwatch(std::uint64_t id) { return impl_->Unwatch(id); }

std::size_t Watcher::SubscriptionCount() const {
  return impl_->SubscriptionCount();
}

bool Watcher::Put(std::string_view key, std::string_view value) {
  return impl_->Put(key, value);
}

bool Watcher::Delete(std::string_view key) { return impl_->Delete(key); }

bool Watcher::Write(const WriteBatch& batch) { return impl_->Write(batch); }

bool Watcher::Notify(std::string_view key, std::string_view value,
                     bool deleted) {
  return impl_->Notify(key, value, deleted);
}

bool Watcher::Replay(std::uint64_t id, std::uint64_t from_seq) {
  return impl_->Replay(id, from_seq);
}

std::uint64_t Watcher::Offset(std::uint64_t id) const {
  return impl_->Offset(id);
}

bool Watcher::FlushOffsets() { return impl_->FlushOffsets(); }

std::uint64_t Watcher::last_seq() const { return impl_->last_seq(); }

bool Watcher::Flush() { return impl_->Flush(); }

bool Watcher::Sync() { return impl_->Sync(); }

WatchStats Watcher::GetStats() const { return impl_->GetStats(); }

std::string Watcher::StatsString() const { return impl_->StatsString(); }

bool Watcher::ok() const { return impl_->ok(); }

const std::string& Watcher::LastError() const { return impl_->LastError(); }

const WatchOptions& Watcher::options() const { return impl_->options(); }

}  // namespace storage
}  // namespace go2cpp
