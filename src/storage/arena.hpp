// Copyright 2026 Go2Cpp Authors. All rights reserved.
// 内存池：按块分配，避免每个 key/value 一次 malloc。
//
// 与 LevelDB util/arena 的差异：
//   * 块大小可配（默认 64KB，LevelDB 固定 4KB）——把 malloc 次数再降一个
//     数量级，1e7 次写入从 ~1e7 次 malloc 降到 ~1.6e4 次；
//   * blocks_ 用裸数组 + 二倍扩容，不用 std::vector（避免 vector 自身的
//     分配器调用与迭代器开销）；
//   * 不提供 MemoryUsage 的原子版本：本 Arena 只在写锁内使用，统计用普通
//     size_t 即可。

#ifndef GO2CPP_STORAGE_ARENA_HPP
#define GO2CPP_STORAGE_ARENA_HPP

#include <cstddef>
#include <cstdint>

namespace go2cpp {
namespace storage {

class Arena {
 public:
  explicit Arena(std::size_t block_size = 64 * 1024);
  ~Arena();

  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;

  // 返回至少 bytes 字节的可写内存（8 字节对齐）。
  char* Allocate(std::size_t bytes);

  // 返回按 align（2 的幂）对齐的内存。
  char* AllocateAligned(std::size_t bytes, std::size_t align = 8);

  std::size_t MemoryUsage() const { return usage_; }

 private:
  char* AllocateFallback(std::size_t bytes);
  char* AllocateNewBlock(std::size_t bytes);

  char* ptr_ = nullptr;          // 当前块剩余空间的起点
  std::size_t remaining_ = 0;    // 当前块剩余字节
  std::size_t block_size_;       // 常规块大小
  char** blocks_ = nullptr;      // 所有块的起始地址
  std::size_t num_blocks_ = 0;
  std::size_t blocks_cap_ = 0;
  std::size_t usage_ = 0;        // 已分配字节（含块头浪费）
};

}  // namespace storage
}  // namespace go2cpp

#endif  // GO2CPP_STORAGE_ARENA_HPP
