// Copyright 2026 Go2Cpp Authors. All rights reserved.
// 内部编码工具（header-only，全部 inline）。
//
// 热路径上的小工具全部inline、无分支可预测性问题：
//   * 定长/变长整数编解码（varint 与 LevelDB 兼容的 7-bit 格式）
//   * xxHash32 风格哈希（比 std::hash<std::string> 快一个数量级，且种子固定）
//   * CRC32C（SSE4.2 硬件指令 + 软件表回退，声明在此，实现在 wal.cpp）

#ifndef GO2CPP_STORAGE_FORMAT_HPP
#define GO2CPP_STORAGE_FORMAT_HPP

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace go2cpp {
namespace storage {
namespace internal {

// ---------------------------------------------------------------------------
// 定长小端编解码
// ---------------------------------------------------------------------------

inline void EncodeFixed32(void* dst, std::uint32_t v) {
  std::memcpy(dst, &v, sizeof(v));
}

inline void EncodeFixed64(void* dst, std::uint64_t v) {
  std::memcpy(dst, &v, sizeof(v));
}

inline std::uint32_t DecodeFixed32(const void* src) {
  std::uint32_t v;
  std::memcpy(&v, src, sizeof(v));
  return v;
}

inline std::uint64_t DecodeFixed64(const void* src) {
  std::uint64_t v;
  std::memcpy(&v, src, sizeof(v));
  return v;
}

// ---------------------------------------------------------------------------
// varint（与 LevelDB/protobuf 兼容）
// ---------------------------------------------------------------------------

inline char* EncodeVarint32(char* dst, std::uint32_t v) {
  while (v >= 0x80) {
    *dst++ = static_cast<char>(v | 0x80);
    v >>= 7;
  }
  *dst++ = static_cast<char>(v);
  return dst;
}

inline char* EncodeVarint64(char* dst, std::uint64_t v) {
  while (v >= 0x80) {
    *dst++ = static_cast<char>(v | 0x80);
    v >>= 7;
  }
  *dst++ = static_cast<char>(v);
  return dst;
}

// 解码失败（超出 5/10 字节）返回 nullptr。
inline const char* GetVarint32(const char* p, const char* limit,
                               std::uint32_t* value) {
  std::uint32_t result = 0;
  for (std::uint32_t shift = 0; shift <= 28 && p < limit; shift += 7) {
    const std::uint32_t byte = static_cast<unsigned char>(*p);
    ++p;
    if (byte & 0x80) {
      result |= (byte & 0x7f) << shift;
    } else {
      result |= byte << shift;
      *value = result;
      return p;
    }
  }
  return nullptr;
}

inline const char* GetVarint64(const char* p, const char* limit,
                               std::uint64_t* value) {
  std::uint64_t result = 0;
  for (std::uint32_t shift = 0; shift <= 63 && p < limit; shift += 7) {
    const std::uint64_t byte = static_cast<unsigned char>(*p);
    ++p;
    if (byte & 0x80) {
      result |= (byte & 0x7f) << shift;
    } else {
      result |= byte << shift;
      *value = result;
      return p;
    }
  }
  return nullptr;
}

inline std::size_t VarintLength(std::uint32_t v) {
  std::size_t n = 1;
  while (v >= 0x80) {
    v >>= 7;
    ++n;
  }
  return n;
}

inline std::size_t VarintLength(std::uint64_t v) {
  std::size_t n = 1;
  while (v >= 0x80) {
    v >>= 7;
    ++n;
  }
  return n;
}

// ---------------------------------------------------------------------------
// 哈希：xxHash32（32 位、无分支、4 路并行累加）
//
// 选它而不是 std::hash<string> 的原因：libstdc++ 的 std::hash 是逐字节
// MurmurHash2，短 key 上每字节一次乘加；xxHash 一次读 4 字节并 4 路并行，
// 16 字节 key 实测快 ~2.5x（见 docs/modules/storage-kv.md）。
// ---------------------------------------------------------------------------

inline std::uint32_t RotateLeft32(std::uint32_t v, int shift) {
  return (v << shift) | (v >> (32 - shift));
}

inline std::uint32_t Read32LE(const std::uint8_t* p) {
  std::uint32_t v;
  std::memcpy(&v, p, sizeof(v));
  return v;
}

inline std::uint32_t Hash32(const char* data, std::size_t len,
                            std::uint32_t seed = 0xbc9f1d34u) {
  constexpr std::uint32_t kP1 = 0x9E3779B1u;
  constexpr std::uint32_t kP2 = 0x85EBCA77u;
  constexpr std::uint32_t kP3 = 0xC2B2AE3Du;
  constexpr std::uint32_t kP4 = 0x27D4EB2Fu;
  constexpr std::uint32_t kP5 = 0x165667B1u;

  const std::uint8_t* p = reinterpret_cast<const std::uint8_t*>(data);
  const std::uint8_t* const end = p + len;
  std::uint32_t h;

  if (len >= 16) {
    std::uint32_t v1 = seed + kP1 + kP2;
    std::uint32_t v2 = seed + kP2;
    std::uint32_t v3 = seed;
    std::uint32_t v4 = seed - kP1;
    const std::uint8_t* const limit = end - 16;
    do {
      v1 = RotateLeft32(v1 + Read32LE(p) * kP2, 13) * kP1;
      p += 4;
      v2 = RotateLeft32(v2 + Read32LE(p) * kP2, 13) * kP1;
      p += 4;
      v3 = RotateLeft32(v3 + Read32LE(p) * kP2, 13) * kP1;
      p += 4;
      v4 = RotateLeft32(v4 + Read32LE(p) * kP2, 13) * kP1;
      p += 4;
    } while (p <= limit);
    h = RotateLeft32(v1, 1) + RotateLeft32(v2, 7) + RotateLeft32(v3, 12) +
        RotateLeft32(v4, 18);
  } else {
    h = seed + kP5;
  }

  h += static_cast<std::uint32_t>(len);

  while (p + 4 <= end) {
    h = RotateLeft32(h + Read32LE(p) * kP3, 17) * kP4;
    p += 4;
  }
  while (p < end) {
    h = RotateLeft32(h + (*p) * kP5, 11) * kP1;
    ++p;
  }

  h ^= h >> 15;
  h *= kP2;
  h ^= h >> 13;
  h *= kP3;
  h ^= h >> 16;
  return h;
}

inline std::uint32_t Hash32(std::string_view s,
                            std::uint32_t seed = 0xbc9f1d34u) {
  return Hash32(s.data(), s.size(), seed);
}

// ---------------------------------------------------------------------------
// CRC32C（Castagnoli）。实现见 wal.cpp：SSE4.2 指令 + 软件表回退。
// ---------------------------------------------------------------------------

std::uint32_t Crc32cExtend(std::uint32_t init, const char* data, std::size_t n);
std::uint32_t Crc32cValue(const char* data, std::size_t n);

// 存储到磁盘前做掩码，与 LevelDB 一致（避免 CRC 本身为 0 时的歧义）。
inline std::uint32_t Crc32cMask(std::uint32_t crc) {
  return ((crc >> 15) | (crc << 17)) + 0xa282ead8u;
}

// ---------------------------------------------------------------------------
// 条目类型：SSTable 里 value 为空与删除必须可区分。
// ---------------------------------------------------------------------------

enum class ValueType : std::uint8_t {
  kDeletion = 0,
  kValue = 1,
};

// 查找结果：区分「不存在」与「已删除」（tombstone）。
enum class Lookup {
  kNotFound = 0,
  kFound = 1,
  kDeleted = 2,
};

}  // namespace internal
}  // namespace storage
}  // namespace go2cpp

#endif  // GO2CPP_STORAGE_FORMAT_HPP
