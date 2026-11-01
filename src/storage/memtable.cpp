// Copyright 2026 Go2Cpp Authors. All rights reserved.

#include "memtable.hpp"

#include <cstring>
#include <new>

#include "go2cpp/storage/kv.hpp"

namespace go2cpp {
namespace storage {

namespace {
// 每条记录的大致内存占用：16B 槽 + 12B rec 头 + key + value。
// 槽容量按 48B/条估算（负载 ~0.7）。实测把它调成 32B/条（负载 ~0.5）反而
// 使 Get 慢 12%：槽数组翻倍带来的 cache/TLB 压力盖过了少探测几次的收益。
constexpr std::size_t kEntryEstimate = 48;

// Arena 块大小跟随 memtable 预算：块太大会让「小预算」的表第一次分配就
// 超过预算，导致每次写入都触发刷盘（实测 32KB 预算 + 64KB 块 = 每写必刷）。
std::size_t ArenaBlockFor(std::size_t budget) {
  std::size_t block = budget / 8;
  if (block < 4096) {
    block = 4096;
  }
  if (block > 64 * 1024) {
    block = 64 * 1024;
  }
  return block;
}

inline std::size_t NextPow2(std::size_t v) {
  std::size_t p = 1024;
  while (p < v) {
    p <<= 1;
  }
  return p;
}
}  // namespace

MemTable::MemTable(const Options& options)
    : arena_(ArenaBlockFor(options.memtable_size)),
      budget_(options.memtable_size) {
  const std::size_t want = budget_ / kEntryEstimate;
  capacity_ = NextPow2(want);
  mask_ = capacity_ - 1;
  slots_ = new Slot[capacity_];
  for (std::size_t i = 0; i < capacity_; ++i) {
    slots_[i].state.store(kEmpty, std::memory_order_relaxed);
    slots_[i].hash = 0;
    slots_[i].rec.store(nullptr, std::memory_order_relaxed);
  }
}

MemTable::~MemTable() { delete[] slots_; }

const char* MemTable::NewRecord(std::string_view key, std::string_view value,
                                internal::ValueType type) {
  const std::size_t bytes = sizeof(Rec) + key.size() + value.size();
  char* mem = arena_.Allocate(bytes);
  Rec* rec = new (mem) Rec;
  rec->key_len = static_cast<std::uint32_t>(key.size());
  rec->val_len = static_cast<std::uint32_t>(value.size());
  rec->type = static_cast<std::uint8_t>(type);
  rec->pad[0] = rec->pad[1] = rec->pad[2] = 0;
  char* payload = mem + sizeof(Rec);
  if (!key.empty()) {
    std::memcpy(payload, key.data(), key.size());
  }
  if (!value.empty()) {
    std::memcpy(payload + key.size(), value.data(), value.size());
  }
  return mem;
}

bool MemTable::Put(std::string_view key, std::string_view value) {
  return Insert(key, value, internal::ValueType::kValue);
}

bool MemTable::Delete(std::string_view key) {
  return Insert(key, std::string_view(), internal::ValueType::kDeletion);
}

bool MemTable::Insert(std::string_view key, std::string_view value,
                      internal::ValueType type) {
  const std::uint32_t h = HashKey(key);
  std::size_t idx = h & mask_;
  const char* kd = key.empty() ? "" : key.data();

  // 最多探测 capacity_ 次；表满时返回 false 交给上层刷盘。
  for (std::size_t probe = 0; probe < capacity_; ++probe) {
    Slot& slot = slots_[idx];
    const std::uint8_t state = slot.state.load(std::memory_order_relaxed);
    if (state == kOccupied) {
      const Rec* old =
          reinterpret_cast<const Rec*>(slot.rec.load(std::memory_order_relaxed));
      if (slot.hash == h && old->key_len == key.size() &&
          (key.empty() || std::memcmp(
                              reinterpret_cast<const char*>(old + 1), kd,
                              key.size()) == 0)) {
        // 覆盖：把 rec 指针换掉，state 中间态让并发读重试。
        const char* fresh = NewRecord(key, value, type);
        slot.state.store(kConstructing, std::memory_order_relaxed);
        slot.rec.store(fresh, std::memory_order_release);
        slot.state.store(kOccupied, std::memory_order_release);
        return true;
      }
    } else if (state == kEmpty || state == kDeleted) {
      const char* fresh = NewRecord(key, value, type);
      slot.hash = h;
      slot.state.store(kConstructing, std::memory_order_relaxed);
      slot.rec.store(fresh, std::memory_order_release);
      slot.state.store(kOccupied, std::memory_order_release);
      size_.fetch_add(1, std::memory_order_relaxed);
      return true;
    }
    idx = (idx + 1) & mask_;
  }
  return false;
}

internal::Lookup MemTable::Lookup(std::string_view key,
                                  std::string* value) const {
  const std::uint32_t h = HashKey(key);
  const char* kd = key.empty() ? "" : key.data();

  // 极少数情况下会撞上写者的 constructing 窗口，重试几次即可；
  // 重试仍失败按「未找到」处理（写者完成前读者本来就可以看不到）。
  for (int attempt = 0; attempt < 4; ++attempt) {
    std::size_t idx = h & mask_;
    bool retry = false;
    for (std::size_t probe = 0; probe < capacity_; ++probe) {
      const Slot& slot = slots_[idx];
      const std::uint8_t state = slot.state.load(std::memory_order_acquire);
      if (state == kEmpty) {
        return internal::Lookup::kNotFound;
      }
      if (state == kConstructing) {
        retry = true;
        break;
      }
      if (state == kOccupied) {
        const Rec* rec = reinterpret_cast<const Rec*>(
            slot.rec.load(std::memory_order_acquire));
        if (slot.hash == h && rec->key_len == key.size() &&
            (key.empty() || std::memcmp(
                                reinterpret_cast<const char*>(rec + 1), kd,
                                key.size()) == 0)) {
          if (rec->type == static_cast<std::uint8_t>(
                              internal::ValueType::kDeletion)) {
            return internal::Lookup::kDeleted;
          }
          if (value != nullptr) {
            value->assign(reinterpret_cast<const char*>(rec + 1) + rec->key_len,
                          rec->val_len);
          }
          return internal::Lookup::kFound;
        }
      }
      idx = (idx + 1) & mask_;
    }
    if (!retry) {
      break;
    }
  }
  return internal::Lookup::kNotFound;
}

bool MemTable::Get(std::string_view key, std::string* value) const {
  return Lookup(key, value) == internal::Lookup::kFound;
}

}  // namespace storage
}  // namespace go2cpp
