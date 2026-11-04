// Copyright 2026 Go2Cpp Authors. All rights reserved.

#include "arena.hpp"

#include <cstdlib>
#include <new>

namespace go2cpp {
namespace storage {

namespace {
constexpr std::size_t kAlign = 8;

inline std::size_t AlignUp(std::size_t bytes, std::size_t align) {
  return (bytes + align - 1) & ~(align - 1);
}
}  // namespace

Arena::Arena(std::size_t block_size)
    : block_size_(block_size < 1024 ? 1024 : block_size) {}

Arena::~Arena() {
  for (std::size_t i = 0; i < num_blocks_; ++i) {
    std::free(blocks_[i]);
  }
  std::free(blocks_);
}

char* Arena::Allocate(std::size_t bytes) {
  // 按 kAlign 向上取整推进：块起点由 malloc 保证 8 字节对齐，只要每次
  // 推进量为 8 的倍数，返回的首地址就始终 8 字节对齐。否则上一条任意
  // 长度的记录会把后续分配整体错位（UBSan 实报 memtable.cpp 的 Rec 落在
  // 0x...80e 这类不满足 4 字节对齐的地址）。
  const std::size_t need = AlignUp(bytes, kAlign);
  // 热路径：当前块还有空间时只做两次减法。
  if (need <= remaining_) {
    char* result = ptr_;
    ptr_ += need;
    remaining_ -= need;
    return result;
  }
  return AllocateFallback(bytes);
}

char* Arena::AllocateAligned(std::size_t bytes, std::size_t align) {
  std::size_t slop = 0;
  if (align > kAlign) {
    const std::uintptr_t mod = reinterpret_cast<std::uintptr_t>(ptr_) & (align - 1);
    slop = mod == 0 ? 0 : align - mod;
  }
  const std::size_t need = AlignUp(bytes, kAlign);
  if (need + slop <= remaining_) {
    char* result = ptr_ + slop;
    // 推进量按 kAlign 取整：保证下一个 Allocate 拿到的起点仍 8 字节对齐。
    ptr_ = result + need;
    remaining_ -= need + slop;
    return result;
  }
  // 新块由 malloc 返回，天然满足 16 字节对齐（超过 16 的场景本模块不用）。
  return AllocateFallback(bytes);
}

char* Arena::AllocateFallback(std::size_t bytes) {
  // 大对象单独分配，避免把整块剩余空间浪费掉。
  if (bytes > block_size_ / 4) {
    const std::size_t alloc = AlignUp(bytes, kAlign);
    char* block = static_cast<char*>(std::malloc(alloc));
    if (block == nullptr) {
      std::abort();
    }
    usage_ += alloc;
    // 大对象不进入 blocks_ 的常规块链表？仍需在析构时释放，统一记录。
    if (num_blocks_ == blocks_cap_) {
      const std::size_t cap = blocks_cap_ == 0 ? 64 : blocks_cap_ * 2;
      char** grown = static_cast<char**>(std::realloc(blocks_, cap * sizeof(char*)));
      if (grown == nullptr) {
        std::abort();
      }
      blocks_ = grown;
      blocks_cap_ = cap;
    }
    blocks_[num_blocks_++] = block;
    return block;
  }

  char* block = AllocateNewBlock(block_size_);
  ptr_ = block;
  remaining_ = block_size_;
  char* result = ptr_;
  const std::size_t need = AlignUp(bytes, kAlign);
  ptr_ += need;
  remaining_ -= need;
  return result;
}

char* Arena::AllocateNewBlock(std::size_t bytes) {
  char* block = static_cast<char*>(std::malloc(bytes));
  if (block == nullptr) {
    std::abort();
  }
  usage_ += bytes;
  if (num_blocks_ == blocks_cap_) {
    const std::size_t cap = blocks_cap_ == 0 ? 64 : blocks_cap_ * 2;
    char** grown = static_cast<char**>(std::realloc(blocks_, cap * sizeof(char*)));
    if (grown == nullptr) {
      std::abort();
    }
    blocks_ = grown;
    blocks_cap_ = cap;
  }
  blocks_[num_blocks_++] = block;
  return block;
}

}  // namespace storage
}  // namespace go2cpp
