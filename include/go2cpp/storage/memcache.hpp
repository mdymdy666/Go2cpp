// Copyright 2026 Go2Cpp Authors. All rights reserved.
// MemCache：基于 LSM 的内存缓存（可选写穿），TTL + LRU。
//
// 设计（对应 LSM 的 MemTable 部件）：
//   * 条目布局 = MemTable 的「槽/载荷分离」思路：桶数组只存指针（8B），
//     key/value 字节在条目内联（≤32B）或单独一块，命中后只碰一条 cache line。
//   * TTL 复用 cache/detail/time_wheel.hpp 的 256 桶时间轮（桶内侵入式
//     链表 + 桶 min 的 256 叶锦标赛树）：插入/删除 O(1)，取最早到期 O(1)，
//     回绕由绝对时间戳比较保护。
//   * LRU 复用 cache/detail/intrusive.hpp 的侵入式链表与定长节点池：稳态
//     （容量已满，插一条淘汰一条）下堆分配次数为 0。
//   * 可选持久化：persist = true 时 Set 写穿到 KV（同一套 LSM）；read_through
//     时 Get 未命中从 KV 回填，构成 cache-aside。
//
// 并发：按哈希分片（默认 8 片），每片一把锁 + 独立的表/LRU/TTL 轮。
// 单线程访问时锁无竞争（std::mutex 无竞争约 20ns）。

#ifndef GO2CPP_STORAGE_MEMCACHE_HPP
#define GO2CPP_STORAGE_MEMCACHE_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace go2cpp {
namespace storage {

class KV;

struct MemCacheOptions {
  // 条目数上限（所有分片合计）。达到后按 LRU 淘汰。
  std::size_t capacity = 1u << 20;
  // 分片数（2 的幂）。每片独立锁，容量均分。
  std::size_t shards = 8;
  // Set 时写穿到 KV（需要 Open 时传 KV）。
  bool persist = false;
  // Get 未命中时从 KV 回填（需要 persist 或独立于 persist 使用）。
  bool read_through = false;
  // 每次 Get/Set 顺带清理的过期条目上限（避免长尾，主动清理用 PurgeExpired）。
  std::size_t purge_batch = 4;
  // true：每次 Get 都扫一次 TTL 轮（旧行为，用于前后对比）；
  // false：每 64 次 Get 扫一次 + 命中时逐条判过期（默认）。
  bool purge_on_get = false;
};

struct MemCacheStats {
  std::uint64_t gets = 0;
  std::uint64_t sets = 0;
  std::uint64_t hits = 0;
  std::uint64_t misses = 0;
  std::uint64_t deletes = 0;
  std::uint64_t expirations = 0;   // TTL 到期删除
  std::uint64_t evictions = 0;     // LRU 淘汰
  std::uint64_t write_throughs = 0;  // 写穿 LSM 的次数
  std::uint64_t read_throughs = 0;   // 从 LSM 回填的次数
  std::size_t size = 0;
  std::size_t capacity = 0;
  std::size_t bytes = 0;
};

class MemCache {
 public:
  // kv 可为 nullptr（纯内存）；persist/read_through 需要非空。
  static std::unique_ptr<MemCache> Open(KV* kv,
                                        const MemCacheOptions& options =
                                            MemCacheOptions());
  ~MemCache();

  MemCache(const MemCache&) = delete;
  MemCache& operator=(const MemCache&) = delete;

  // 写入/覆盖。ttl 为 0 表示不过期。
  bool Set(std::string_view key, std::string_view value,
           std::chrono::milliseconds ttl = std::chrono::milliseconds(0));

  // 读取（命中会提升 LRU）。未命中返回 false。
  bool Get(std::string_view key, std::string* value);

  // 只查询，不提升 LRU、不计命中统计。
  bool Peek(std::string_view key, std::string* value);

  bool Delete(std::string_view key);

  // 立刻让某个 key 过期（等价于 Delete，但计入 expirations）。
  bool Expire(std::string_view key);

  // 清空所有分片（含 TTL 轮与 LRU 链）。
  void Clear();

  // 主动清理已过期条目，返回清理条数。
  std::size_t PurgeExpired();

  std::size_t Size() const;
  std::size_t Capacity() const;

  MemCacheStats GetStats() const;
  std::string StatsString() const;

  bool ok() const;
  const std::string& LastError() const;
  const MemCacheOptions& options() const;

 private:
  class Impl;
  explicit MemCache(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace storage
}  // namespace go2cpp

#endif  // GO2CPP_STORAGE_MEMCACHE_HPP
