// Copyright 2026 Go2Cpp Authors. All rights reserved.
// MemTable：内存写缓冲（开放寻址哈希 + Arena）。
//
// 与 LevelDB 的差异（性能取向）：
//   * LevelDB 用跳表：O(log n) 且每次比较要跟指针追 2~3 次 cache miss。
//     这里用开放寻址哈希：平均 1.3 次探测、1 次 cache miss，短 key 查找
//     快 3~5 倍（见 docs/modules/storage-kv.md 的 objdump/实测）。
//   * 槽数组与 arena 分离（SoA）：探测只碰 16B 的槽，key/value 字节只在
//     命中后才读，避免把整个条目拉进 cache。
//   * 容量固定不 rehash：写满即交由 KV 刷盘换新表，把「扩容拷贝」从热路径
//     上彻底删掉。
//
// 并发：
//   * Put/Delete 由调用方持写锁（同 LevelDB 的 mutex）。
//   * Get 无锁：槽位用 state 原子发布（constructing -> occupied），读者看到
//     constructing 就重试；x86 上 acquire load 不产生任何额外指令。

#ifndef GO2CPP_STORAGE_MEMTABLE_HPP
#define GO2CPP_STORAGE_MEMTABLE_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "arena.hpp"
#include "format.hpp"

namespace go2cpp {
namespace storage {

struct Options;

class MemTable {
 public:
  explicit MemTable(const Options& options);
  ~MemTable();

  MemTable(const MemTable&) = delete;
  MemTable& operator=(const MemTable&) = delete;

  // 写入。返回 false 表示表已满（调用方应先刷盘再重试）。
  bool Put(std::string_view key, std::string_view value);
  bool Delete(std::string_view key);

  // 无锁查找。命中且是 tombstone 时返回 false（调用方据此判断删除）。
  // 为区分「不存在」与「已删除」，内部用 Lookup 语义：
  bool Get(std::string_view key, std::string* value) const;
  // 同上，但返回三态；供 KV 判断是否要继续查更旧的数据。
  internal::Lookup Lookup(std::string_view key, std::string* value) const;

  // 遍历（调用方需保证没有并发写）：fn(std::string_view key,
  // std::string_view value, bool deleted)。
  template <typename Fn>
  void ForEach(Fn&& fn) const {
    for (std::size_t i = 0; i < capacity_; ++i) {
      const Slot& slot = slots_[i];
      if (slot.state.load(std::memory_order_acquire) != kOccupied) {
        continue;
      }
      const Rec* rec =
          reinterpret_cast<const Rec*>(slot.rec.load(std::memory_order_acquire));
      const char* payload = reinterpret_cast<const char*>(rec + 1);
      const std::string_view k(payload, rec->key_len);
      const std::string_view v(payload + rec->key_len, rec->val_len);
      fn(k, v, rec->type == static_cast<std::uint8_t>(
                       internal::ValueType::kDeletion));
    }
  }

  std::size_t Budget() const { return budget_; }
  // 恢复期用双倍预算容纳 WAL 重放，重放完成后调回常规预算，让下一次写
  // 立即触发刷盘（容量已按双倍分配，不会越界）。
  void SetBudget(std::size_t bytes) { budget_ = bytes; }
  std::size_t MemoryUsage() const {
    return arena_.MemoryUsage() + capacity_ * sizeof(Slot);
  }
  std::size_t NumEntries() const {
    return size_.load(std::memory_order_relaxed);
  }
  bool ShouldFlush() const {
    return arena_.MemoryUsage() >= budget_ ||
           size_.load(std::memory_order_relaxed) * 2 >= capacity_;
  }
  bool Empty() const { return size_.load(std::memory_order_relaxed) == 0; }

 private:
  static constexpr std::uint8_t kEmpty = 0;
  static constexpr std::uint8_t kConstructing = 1;
  static constexpr std::uint8_t kOccupied = 2;
  static constexpr std::uint8_t kDeleted = 3;

  struct Slot {
    std::atomic<std::uint8_t> state;
    std::uint8_t pad[3];
    std::uint32_t hash;
    std::atomic<const char*> rec;
  };
  static_assert(sizeof(Slot) == 16, "Slot 必须紧凑到 16B（每 cache line 4 个）");

  struct Rec {
    std::uint32_t key_len;
    std::uint32_t val_len;
    std::uint8_t type;  // internal::ValueType
    std::uint8_t pad[3];
  };

  const char* NewRecord(std::string_view key, std::string_view value,
                        internal::ValueType type);
  bool Insert(std::string_view key, std::string_view value,
              internal::ValueType type);

  static std::uint32_t HashKey(std::string_view key) {
    return internal::Hash32(key.empty() ? "" : key.data(), key.size());
  }

  Arena arena_;
  Slot* slots_ = nullptr;
  std::size_t capacity_ = 0;
  std::size_t mask_ = 0;
  std::size_t budget_;
  std::atomic<std::size_t> size_{0};
};

}  // namespace storage
}  // namespace go2cpp

#endif  // GO2CPP_STORAGE_MEMTABLE_HPP
