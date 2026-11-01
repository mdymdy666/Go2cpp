// Copyright 2026 Go2Cpp Authors. All rights reserved.
//
// MemCache 实现：分片链式哈希 + 侵入式 LRU 链 + cache 的 256 桶 TTL 时间轮。
// 可选写穿 / 回填 KV（同一套 LSM）。

#include "go2cpp/storage/memcache.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <sstream>
#include <vector>

#include "format.hpp"
#include "go2cpp/cache/detail/intrusive.hpp"
#include "go2cpp/cache/detail/time_wheel.hpp"
#include "go2cpp/storage/kv.hpp"

namespace go2cpp {
namespace storage {

namespace {

using Clock = std::chrono::steady_clock;

std::uint64_t NowMs() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          Clock::now().time_since_epoch())
          .count());
}

// 内联在条目里的 key+value 字节数。32B 覆盖绝大多数小对象缓存
// （key 16B + value 16B），超过则单独分配一块。
constexpr std::size_t kInline = 32;

std::size_t NextPow2(std::size_t n) {
  std::size_t p = 1;
  while (p < n) {
    p <<= 1;
  }
  return p;
}

}  // namespace

class MemCache::Impl {
 public:
  Impl(KV* kv, MemCacheOptions options) : kv_(kv), opt_(options) {}

  ~Impl() { shards_.clear(); }

  bool Open() {
    if (opt_.persist && kv_ == nullptr) {
      return Fail("persist requires a KV instance");
    }
    if (opt_.read_through && kv_ == nullptr) {
      return Fail("read_through requires a KV instance");
    }
    std::size_t shards = NextPow2(std::max<std::size_t>(opt_.shards, 1));
    if (shards > 1024) {
      shards = 1024;
    }
    capacity_ = std::max<std::size_t>(opt_.capacity, 1);
    const std::size_t per_shard = (capacity_ + shards - 1) / shards;
    for (std::size_t i = 0; i < shards; ++i) {
      auto shard = std::make_unique<Shard>();
      shard->capacity = per_shard;
      shard->bucket_mask = NextPow2(per_shard) - 1;
      shard->buckets.assign(shard->bucket_mask + 1, nullptr);
      shard->pool.Reserve(per_shard);
      shards_.push_back(std::move(shard));
    }
    shard_mask_ = shards_.size() - 1;
    opened_ = true;
    return true;
  }

  bool Set(std::string_view key, std::string_view value,
           std::chrono::milliseconds ttl) {
    if (!opened_) {
      return Fail("memcache not open");
    }
    sets_.fetch_add(1, std::memory_order_relaxed);
    const std::uint32_t h = internal::Hash32(key);
    Shard& shard = ShardFor(h);
    const std::uint64_t now = NowMs();
    const std::uint64_t ttl_ms = ttl.count() > 0
                                     ? static_cast<std::uint64_t>(ttl.count())
                                     : 0;
    const std::uint64_t expire = ttl_ms == 0 ? 0 : now + ttl_ms;
    {
      std::lock_guard<std::mutex> lk(shard.mu);
      PurgeLocked(shard, now, opt_.purge_batch);
      Entry* entry = FindLocked(shard, h, key);
      if (entry != nullptr) {
        if (entry->in_wheel()) {
          shard.wheel.Erase(entry);
        }
        entry->Assign(key, value, expire);
        if (expire != 0) {
          entry->flags |= Entry::kFlagTtl | Entry::kFlagWheel;
          shard.wheel.Insert(entry);
        }
        LruTouch(shard, entry);
        return Persist(key, value);
      }
      if (shard.size >= shard.capacity) {
        EvictLocked(shard);
      }
      Entry* fresh = shard.pool.Create();
      fresh->Assign(key, value, expire);
      fresh->hash = h;
      if (expire != 0) {
        fresh->flags |= Entry::kFlagTtl | Entry::kFlagWheel;
        shard.wheel.Insert(fresh);
      }
      InsertIndex(shard, fresh);
      LruPushFront(shard, fresh);
      ++shard.size;
    }
    return Persist(key, value);
  }

  bool Get(std::string_view key, std::string* value) {
    if (!opened_) {
      Fail("memcache not open");
      return false;
    }
    gets_.fetch_add(1, std::memory_order_relaxed);
    const std::uint32_t h = internal::Hash32(key);
    Shard& shard = ShardFor(h);
    const std::uint64_t now = NowMs();
    {
      std::lock_guard<std::mutex> lk(shard.mu);
      // 过期扫描节流：每 64 次 Get 才批量清理一次；命中的条目额外做一次
      // 「这一条是否已过期」的判定，保证语义精确。
      if (opt_.purge_on_get) {
        PurgeLocked(shard, now, opt_.purge_batch);
      } else if ((++shard.purge_tick & 63u) == 0) {
        PurgeLocked(shard, now, opt_.purge_batch);
      }
      Entry* entry = FindLocked(shard, h, key);
      if (entry != nullptr && entry->expired(now)) {
        RemoveLocked(shard, entry);
        expirations_.fetch_add(1, std::memory_order_relaxed);
        entry = nullptr;
      }
      if (entry != nullptr) {
        hits_.fetch_add(1, std::memory_order_relaxed);
        LruTouch(shard, entry);
        if (value != nullptr) {
          const std::string_view v = entry->value();
          value->assign(v.data(), v.size());
        }
        return true;
      }
      misses_.fetch_add(1, std::memory_order_relaxed);
    }
    if (opt_.read_through && kv_ != nullptr) {
      std::string loaded;
      if (kv_->Get(key, &loaded)) {
        read_throughs_.fetch_add(1, std::memory_order_relaxed);
        Set(key, loaded, std::chrono::milliseconds(0));
        if (value != nullptr) {
          *value = std::move(loaded);
        }
        return true;
      }
    }
    return false;
  }

  bool Peek(std::string_view key, std::string* value) {
    if (!opened_) {
      return false;
    }
    const std::uint32_t h = internal::Hash32(key);
    Shard& shard = ShardFor(h);
    std::lock_guard<std::mutex> lk(shard.mu);
    PurgeLocked(shard, NowMs(), opt_.purge_batch);
    Entry* entry = FindLocked(shard, h, key);
    if (entry == nullptr) {
      return false;
    }
    if (value != nullptr) {
      const std::string_view v = entry->value();
      value->assign(v.data(), v.size());
    }
    return true;
  }

  bool Delete(std::string_view key) {
    if (!opened_) {
      return Fail("memcache not open");
    }
    const std::uint32_t h = internal::Hash32(key);
    Shard& shard = ShardFor(h);
    bool removed = false;
    {
      std::lock_guard<std::mutex> lk(shard.mu);
      Entry* entry = FindLocked(shard, h, key);
      if (entry != nullptr) {
        RemoveLocked(shard, entry);
        ++deletes_;
        removed = true;
      }
    }
    if (opt_.persist && kv_ != nullptr) {
      kv_->Delete(key);
    }
    return removed;
  }

  bool Expire(std::string_view key) {
    if (!opened_) {
      return Fail("memcache not open");
    }
    const std::uint32_t h = internal::Hash32(key);
    Shard& shard = ShardFor(h);
    std::lock_guard<std::mutex> lk(shard.mu);
    Entry* entry = FindLocked(shard, h, key);
    if (entry == nullptr) {
      return false;
    }
    RemoveLocked(shard, entry);
    expirations_.fetch_add(1, std::memory_order_relaxed);
    return true;
  }

  void Clear() {
    for (auto& shard_ptr : shards_) {
      Shard& shard = *shard_ptr;
      std::lock_guard<std::mutex> lk(shard.mu);
      while (shard.lru_head != nullptr) {
        RemoveLocked(shard, shard.lru_head);
      }
      shard.wheel.Clear();
      shard.pool.Release();
      shard.buckets.assign(shard.bucket_mask + 1, nullptr);
    }
  }

  std::size_t PurgeExpired() {
    std::size_t purged = 0;
    const std::uint64_t now = NowMs();
    for (auto& shard_ptr : shards_) {
      Shard& shard = *shard_ptr;
      std::lock_guard<std::mutex> lk(shard.mu);
      const std::uint64_t before =
          expirations_.load(std::memory_order_relaxed);
      PurgeLocked(shard, now, static_cast<std::size_t>(-1));
      purged += static_cast<std::size_t>(
          expirations_.load(std::memory_order_relaxed) - before);
    }
    return purged;
  }

  std::size_t Capacity() const { return capacity_; }

  const MemCacheOptions& options() const { return opt_; }

  std::size_t Size() const {
    std::size_t total = 0;
    for (auto& shard_ptr : shards_) {
      Shard& shard = *shard_ptr;
      std::lock_guard<std::mutex> lk(shard.mu);
      total += shard.size;
    }
    return total;
  }

  MemCacheStats GetStats() const {
    MemCacheStats s;
    s.gets = gets_.load(std::memory_order_relaxed);
    s.sets = sets_.load(std::memory_order_relaxed);
    s.hits = hits_.load(std::memory_order_relaxed);
    s.misses = misses_.load(std::memory_order_relaxed);
    s.deletes = deletes_.load(std::memory_order_relaxed);
    s.expirations = expirations_.load(std::memory_order_relaxed);
    s.evictions = evictions_.load(std::memory_order_relaxed);
    s.write_throughs = write_throughs_.load(std::memory_order_relaxed);
    s.read_throughs = read_throughs_.load(std::memory_order_relaxed);
    s.capacity = capacity_;
    for (auto& shard_ptr : shards_) {
      Shard& shard = *shard_ptr;
      std::lock_guard<std::mutex> lk(shard.mu);
      s.size += shard.size;
      s.bytes += shard.size * sizeof(Entry) +
                 shard.buckets.size() * sizeof(Entry*);
    }
    return s;
  }

  std::string StatsString() const {
    const MemCacheStats s = GetStats();
    std::ostringstream os;
    os << "MemCache{size=" << s.size << ", capacity=" << s.capacity
       << ", hits=" << s.hits << ", misses=" << s.misses
       << ", evictions=" << s.evictions << ", expirations=" << s.expirations
       << ", bytes=" << s.bytes << "}";
    return os.str();
  }

  bool ok() const {
    std::lock_guard<std::mutex> lk(err_mu_);
    return error_.empty();
  }

  const std::string& LastError() const { return error_; }

  bool Fail(const std::string& what) {
    std::lock_guard<std::mutex> lk(err_mu_);
    if (error_.empty()) {
      error_ = what;
    }
    return false;
  }

 private:
  // 成员顺序按「查找路径的访问顺序」排：hash/klen/flags 与内联 key 位于
  // 同一条 64B cache line（偏移 0~47），一次 miss 就能完成 hash + key 比较；
  // 指针字段（TTL 轮 / LRU 链 / 桶链）放在后面，命中后才会写到。
  struct Entry {
    static constexpr std::uint32_t kFlagHeap = 1;
    static constexpr std::uint32_t kFlagTtl = 2;
    static constexpr std::uint32_t kFlagWheel = 4;

    std::uint32_t hash = 0;
    std::uint16_t klen = 0;
    std::uint16_t vlen = 0;
    std::uint8_t flags = 0;
    std::uint8_t pad[3] = {0, 0, 0};

    union Storage {
      char inl[kInline];
      char* heap;
      Storage() : heap(nullptr) {}
    };
    Storage storage;  // 偏移 16：内联 key+value（≤32B）

    Entry* prev = nullptr;  // TTL 时间轮的桶内链表
    Entry* next = nullptr;
    Entry* lru_prev = nullptr;
    Entry* lru_next = nullptr;
    Entry* hnext = nullptr;
    Entry* hprev = nullptr;
    std::uint64_t reference_time = 0;  // 到期毫秒（时间轮的键）

    bool heap_mode() const { return (flags & kFlagHeap) != 0; }
    bool in_wheel() const { return (flags & kFlagWheel) != 0; }

    std::string_view key() const {
      return heap_mode() ? std::string_view(storage.heap, klen)
                         : std::string_view(storage.inl, klen);
    }

    bool expired(std::uint64_t now) const {
      return (flags & kFlagTtl) != 0 && reference_time <= now;
    }

    std::string_view value() const {
      const char* p = heap_mode() ? storage.heap : storage.inl;
      return std::string_view(p + klen, vlen);
    }

    void FreeHeap() {
      if (heap_mode()) {
        ::operator delete(storage.heap);
        storage.heap = nullptr;
      }
    }

    void Assign(std::string_view k, std::string_view v, std::uint64_t expire) {
      FreeHeap();
      flags = 0;
      klen = static_cast<std::uint16_t>(
          k.size() > 0xffff ? 0xffff : k.size());
      vlen = static_cast<std::uint16_t>(
          v.size() > 0xffff ? 0xffff : v.size());
      reference_time = expire;
      if (k.size() + v.size() <= kInline) {
        if (!k.empty()) {
          std::memcpy(storage.inl, k.data(), k.size());
        }
        if (!v.empty()) {
          std::memcpy(storage.inl + k.size(), v.data(), v.size());
        }
      } else {
        char* p = static_cast<char*>(::operator new(k.size() + v.size()));
        if (!k.empty()) {
          std::memcpy(p, k.data(), k.size());
        }
        if (!v.empty()) {
          std::memcpy(p + k.size(), v.data(), v.size());
        }
        storage.heap = p;
        flags |= kFlagHeap;
      }
    }
  };

  struct Shard {
    std::mutex mu;
    std::vector<Entry*> buckets;
    std::size_t bucket_mask = 0;
    Entry* lru_head = nullptr;
    Entry* lru_tail = nullptr;
    cache::detail::TimestampWheel<Entry> wheel;
    cache::detail::NodePool<Entry> pool;
    std::size_t size = 0;
    std::size_t capacity = 0;
    std::uint32_t purge_tick = 0;  // Get 侧过期扫描节流
  };

  Shard& ShardFor(std::uint32_t hash) {
    return *shards_[(hash >> 16) & shard_mask_];
  }

  const Shard& ShardFor(std::uint32_t hash) const {
    return *shards_[(hash >> 16) & shard_mask_];
  }

  Entry* FindLocked(Shard& shard, std::uint32_t hash,
                    std::string_view key) const {
    Entry* entry = shard.buckets[hash & shard.bucket_mask];
    while (entry != nullptr) {
      if (entry->hash == hash && entry->key() == key) {
        return entry;
      }
      entry = entry->hnext;
    }
    return nullptr;
  }

  void InsertIndex(Shard& shard, Entry* entry) {
    const std::size_t b = entry->hash & shard.bucket_mask;
    entry->hprev = nullptr;
    entry->hnext = shard.buckets[b];
    if (shard.buckets[b] != nullptr) {
      shard.buckets[b]->hprev = entry;
    }
    shard.buckets[b] = entry;
  }

  void EraseIndex(Shard& shard, Entry* entry) {
    if (entry->hprev != nullptr) {
      entry->hprev->hnext = entry->hnext;
    } else {
      shard.buckets[entry->hash & shard.bucket_mask] = entry->hnext;
    }
    if (entry->hnext != nullptr) {
      entry->hnext->hprev = entry->hprev;
    }
    entry->hprev = nullptr;
    entry->hnext = nullptr;
  }

  void LruPushFront(Shard& shard, Entry* entry) {
    entry->lru_prev = nullptr;
    entry->lru_next = shard.lru_head;
    if (shard.lru_head != nullptr) {
      shard.lru_head->lru_prev = entry;
    } else {
      shard.lru_tail = entry;
    }
    shard.lru_head = entry;
  }

  void LruErase(Shard& shard, Entry* entry) {
    if (entry->lru_prev != nullptr) {
      entry->lru_prev->lru_next = entry->lru_next;
    } else {
      shard.lru_head = entry->lru_next;
    }
    if (entry->lru_next != nullptr) {
      entry->lru_next->lru_prev = entry->lru_prev;
    } else {
      shard.lru_tail = entry->lru_prev;
    }
    entry->lru_prev = nullptr;
    entry->lru_next = nullptr;
  }

  void LruTouch(Shard& shard, Entry* entry) {
    if (shard.lru_head == entry) {
      return;
    }
    LruErase(shard, entry);
    LruPushFront(shard, entry);
  }

  void RemoveLocked(Shard& shard, Entry* entry) {
    EraseIndex(shard, entry);
    LruErase(shard, entry);
    if (entry->in_wheel()) {
      shard.wheel.Erase(entry);
      entry->flags &= ~Entry::kFlagWheel;
    }
    entry->FreeHeap();
    shard.pool.Destroy(entry);
    --shard.size;
  }

  void EvictLocked(Shard& shard) {
    if (shard.lru_tail == nullptr) {
      return;
    }
    RemoveLocked(shard, shard.lru_tail);
    evictions_.fetch_add(1, std::memory_order_relaxed);
  }

  void PurgeLocked(Shard& shard, std::uint64_t now,
                   std::size_t budget) {
    while (budget > 0) {
      Entry* entry = shard.wheel.Min();
      if (entry == nullptr || entry->reference_time > now) {
        return;
      }
      RemoveLocked(shard, entry);
      expirations_.fetch_add(1, std::memory_order_relaxed);
      --budget;
    }
  }

  bool Persist(std::string_view key, std::string_view value) {
    if (!opt_.persist || kv_ == nullptr) {
      return true;
    }
    if (!kv_->Put(key, value)) {
      return Fail("write-through failed: " + kv_->LastError());
    }
    write_throughs_.fetch_add(1, std::memory_order_relaxed);
    return true;
  }

  KV* kv_ = nullptr;
  MemCacheOptions opt_;
  std::size_t capacity_ = 0;
  std::vector<std::unique_ptr<Shard>> shards_;
  std::size_t shard_mask_ = 0;
  bool opened_ = false;

  mutable std::mutex err_mu_;
  std::string error_;

  std::atomic<std::uint64_t> gets_{0};
  std::atomic<std::uint64_t> sets_{0};
  std::atomic<std::uint64_t> hits_{0};
  std::atomic<std::uint64_t> misses_{0};
  std::atomic<std::uint64_t> deletes_{0};
  std::atomic<std::uint64_t> expirations_{0};
  std::atomic<std::uint64_t> evictions_{0};
  std::atomic<std::uint64_t> write_throughs_{0};
  std::atomic<std::uint64_t> read_throughs_{0};
};

MemCache::MemCache(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

MemCache::~MemCache() = default;

std::unique_ptr<MemCache> MemCache::Open(KV* kv,
                                         const MemCacheOptions& options) {
  auto impl = std::make_unique<Impl>(kv, options);
  if (!impl->Open()) {
    return nullptr;
  }
  return std::unique_ptr<MemCache>(new MemCache(std::move(impl)));
}

bool MemCache::Set(std::string_view key, std::string_view value,
                   std::chrono::milliseconds ttl) {
  return impl_->Set(key, value, ttl);
}

bool MemCache::Get(std::string_view key, std::string* value) {
  return impl_->Get(key, value);
}

bool MemCache::Peek(std::string_view key, std::string* value) {
  return impl_->Peek(key, value);
}

bool MemCache::Delete(std::string_view key) { return impl_->Delete(key); }

bool MemCache::Expire(std::string_view key) { return impl_->Expire(key); }

void MemCache::Clear() { impl_->Clear(); }

std::size_t MemCache::PurgeExpired() { return impl_->PurgeExpired(); }

std::size_t MemCache::Size() const { return impl_->Size(); }

std::size_t MemCache::Capacity() const { return impl_->Capacity(); }

const MemCacheOptions& MemCache::options() const { return impl_->options(); }

MemCacheStats MemCache::GetStats() const { return impl_->GetStats(); }

std::string MemCache::StatsString() const { return impl_->StatsString(); }

bool MemCache::ok() const { return impl_->ok(); }

const std::string& MemCache::LastError() const { return impl_->LastError(); }

}  // namespace storage
}  // namespace go2cpp
