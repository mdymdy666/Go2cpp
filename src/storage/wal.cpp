// Copyright 2026 Go2Cpp Authors. All rights reserved.

#include "wal.hpp"

#include <cerrno>
#include <cstring>
#include <string>

#include <fcntl.h>
#include <unistd.h>

#include "format.hpp"
#include "go2cpp/storage/kv.hpp"

#include "go2cpp/logging/log.hpp"

#if defined(__x86_64__) || defined(__i386__)
#include <nmmintrin.h>
#define GO2CPP_HAVE_SSE42 1
#endif

namespace go2cpp {
namespace storage {

namespace {

// ---------------------------------------------------------------------------
// CRC32C
// ---------------------------------------------------------------------------

constexpr std::size_t kBlockSize = 64 * 1024;
constexpr std::size_t kHeaderSize = 7;  // crc32(4) + len(2) + type(1)

enum RecordType : std::uint8_t {
  kFullType = 1,
  kFirstType = 2,
  kMiddleType = 3,
  kLastType = 4,
};

// 4 路切片表（slicing-by-4）：每 4 字节查 4 张表，比逐字节快 ~3x。
struct Crc32cTables {
  std::uint32_t t[4][256];
  Crc32cTables() {
    constexpr std::uint32_t kPoly = 0x82F63B78u;  // 反射 Castagnoli
    for (std::uint32_t i = 0; i < 256; ++i) {
      std::uint32_t c = i;
      for (int k = 0; k < 8; ++k) {
        c = (c & 1) ? (kPoly ^ (c >> 1)) : (c >> 1);
      }
      t[0][i] = c;
    }
    for (std::uint32_t i = 0; i < 256; ++i) {
      std::uint32_t c = t[0][i];
      for (int j = 1; j < 4; ++j) {
        c = t[0][c & 0xff] ^ (c >> 8);
        t[j][i] = c;
      }
    }
  }
};

const Crc32cTables& Tables() {
  static const Crc32cTables tables;
  return tables;
}

std::uint32_t Crc32cSoft(std::uint32_t crc, const char* data, std::size_t n) {
  const auto& t = Tables().t;
  const std::uint8_t* p = reinterpret_cast<const std::uint8_t*>(data);
  crc = ~crc;
  while (n >= 4) {
    std::uint32_t word;
    std::memcpy(&word, p, 4);
    word ^= crc;
    crc = t[3][word & 0xff] ^ t[2][(word >> 8) & 0xff] ^
          t[1][(word >> 16) & 0xff] ^ t[0][(word >> 24) & 0xff];
    p += 4;
    n -= 4;
  }
  while (n-- > 0) {
    crc = t[0][(crc ^ *p) & 0xff] ^ (crc >> 8);
    ++p;
  }
  return ~crc;
}

#if defined(GO2CPP_HAVE_SSE42)
__attribute__((target("sse4.2"))) std::uint32_t Crc32cHw(std::uint32_t crc,
                                                         const char* data,
                                                         std::size_t n) {
  const std::uint8_t* p = reinterpret_cast<const std::uint8_t*>(data);
  std::uint64_t l = crc ^ 0xffffffffu;
  while (n >= 8) {
    std::uint64_t v;
    std::memcpy(&v, p, 8);
    l = _mm_crc32_u64(l, v);
    p += 8;
    n -= 8;
  }
  if (n >= 4) {
    std::uint32_t v;
    std::memcpy(&v, p, 4);
    l = _mm_crc32_u32(static_cast<std::uint32_t>(l), v);
    p += 4;
    n -= 4;
  }
  while (n-- > 0) {
    l = _mm_crc32_u8(static_cast<std::uint32_t>(l), *p);
    ++p;
  }
  return static_cast<std::uint32_t>(l) ^ 0xffffffffu;
}
#endif

bool CpuHasSse42() {
#if defined(GO2CPP_HAVE_SSE42)
  __builtin_cpu_init();
  return __builtin_cpu_supports("sse4.2") != 0;
#else
  return false;
#endif
}

}  // namespace

namespace internal {

std::uint32_t Crc32cExtend(std::uint32_t crc, const char* data, std::size_t n) {
#if defined(GO2CPP_HAVE_SSE42)
  static const bool has_hw = CpuHasSse42();
  if (has_hw) {
    return Crc32cHw(crc, data, n);
  }
#endif
  return Crc32cSoft(crc, data, n);
}

std::uint32_t Crc32cValue(const char* data, std::size_t n) {
  return Crc32cExtend(0, data, n);
}

}  // namespace internal

// ---------------------------------------------------------------------------
// WalWriter
// ---------------------------------------------------------------------------

WalWriter::WalWriter(const Options& options)
    : flush_threshold_(options.write_buffer_size < 4096
                           ? 4096
                           : options.write_buffer_size) {
  buf_.reserve(flush_threshold_ + kBlockSize);
}

WalWriter::~WalWriter() { Close(); }

bool WalWriter::Open(const std::string& path, std::uint64_t existing_size) {
  path_ = path;
  fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
  if (fd_ < 0) {
    return false;
  }
  block_offset_ = static_cast<std::size_t>(existing_size % kBlockSize);
  return true;
}

bool WalWriter::AddRecord(std::string_view record) {
  if (fd_ < 0) {
    return false;
  }
  const char* ptr = record.data();
  std::size_t left = record.size();
  bool begin = true;

  do {
    const std::size_t leftover = kBlockSize - block_offset_;
    if (leftover < kHeaderSize) {
      if (leftover > 0) {
        // 块尾不足一个 header 时用 0 填充，读端按块边界跳过。
        buf_.append(leftover, '\0');
        block_offset_ += leftover;
      }
      block_offset_ = 0;
    }

    const std::size_t avail = kBlockSize - block_offset_ - kHeaderSize;
    const std::size_t fragment = left < avail ? left : avail;
    const bool end = (left == fragment);

    std::uint8_t type;
    if (begin && end) {
      type = kFullType;
    } else if (begin) {
      type = kFirstType;
    } else if (end) {
      type = kLastType;
    } else {
      type = kMiddleType;
    }

    const std::uint32_t crc = internal::Crc32cMask(
        internal::Crc32cExtend(internal::Crc32cValue(
                                   reinterpret_cast<const char*>(&type), 1),
                               ptr, fragment));

    char header[kHeaderSize];
    internal::EncodeFixed32(header, crc);
    header[4] = static_cast<char>(fragment & 0xff);
    header[5] = static_cast<char>(fragment >> 8);
    header[6] = static_cast<char>(type);

    buf_.append(header, kHeaderSize);
    if (fragment > 0) {
      buf_.append(ptr, fragment);
    }
    block_offset_ += kHeaderSize + fragment;

    ptr += fragment;
    left -= fragment;
    begin = false;

    if (buf_.size() >= flush_threshold_) {
      if (!FlushBuffer()) {
        return false;
      }
    }
  } while (left > 0 || begin);

  return true;
}

/**
 * @brief 把用户态写缓冲交给内核；部分写失败时只保留未落盘后缀。
 *
 * @return
 * - true：缓冲区已全部写入内核，buf_ 为空——调用方可以继续追加；
 * - false：内核没有接受全部字节（ENOSPC/EDQUOT/EIO/EFBIG），buf_ 中只剩
 *   未落盘的后缀。调用方**必须**把这次失败上抛给写路径；稍后重试时只会
 *   写后缀，不会重复写已落盘前缀。
 *
 * ## 定位
 * WAL 字节流的唯一提交点。部分写之后若保留整个缓冲区，重试会把已落盘
 * 前缀再写一遍：WAL 流中出现重复记录，恢复端按长度/CRC 解析立刻错位，
 * 整段已确认写被当作损坏尾部丢弃（P1-2，数据丢失）。
 *
 * ## 步骤
 * step1. 循环 write 直到缓冲区写完；EINTR 重试，返回 0 视为失败防死循环；
 * step2. 失败时把已落盘的 off 字节从 buf_ 前缀丢弃并计入 bytes_written_；
 * step3. 返回 buf_ 是否已清空——只有清空才算提交成功。
 *
 * ## 并发/失败语义
 * - 调用约束：由 KV 写锁串行化，非线程安全。
 * - 失败语义：fd_ 不变，buf_ 只保留未落盘后缀，可安全重试。
 */
bool WalWriter::FlushBuffer() {
  if (fd_ < 0) {
    return false;
  }
  if (buf_.empty()) {
    return true;
  }
  std::size_t off = 0;
  while (off < buf_.size()) {
    const ssize_t n = ::write(fd_, buf_.data() + off, buf_.size() - off);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;  // 失败收尾：丢弃已落盘前缀，保留后缀。
    }
    if (n == 0) {
      break;  // 内核拒绝推进：按失败处理，避免永久自旋。
    }
    off += static_cast<std::size_t>(n);
  }
  if (off > 0) {
    // 已落盘前缀必须先记账再丢弃：重试只能写剩余字节，否则重复写会让
    // WAL 流出现重复记录（P1-2）。
    bytes_written_ += off;
    buf_.erase(0, off);
  }
  return buf_.empty();
}

bool WalWriter::Sync() {
  if (fd_ < 0) {
    return false;
  }
  if (!FlushBuffer()) {
    return false;
  }
#if defined(__linux__)
  const int sync_rc = ::fdatasync(fd_);
#else
  const int sync_rc = ::fsync(fd_);
#endif
  if (sync_rc != 0) {
    // 同步失败 = WAL 记录可能未落盘，崩溃后数据丢失，必须可见。
    GO2CPP_LOG_ERROR(GO2CPP_LOG_NAME("storage"))
        << "WAL sync failed: errno=" << errno;
    return false;
  }
  return true;
}

bool WalWriter::Close() {
  if (fd_ < 0) {
    return true;
  }
  const bool ok = FlushBuffer();
  if (::close(fd_) != 0) {
    fd_ = -1;
    return false;
  }
  fd_ = -1;
  return ok;
}

// ---------------------------------------------------------------------------
// WalReader
//
// 严格按 64KB 块读取：写入端保证一条记录不跨块（只在块内分段），
// 因此块尾不足 7 字节的填充可以整体跳过，解析不会错位。
// ---------------------------------------------------------------------------

WalReader::WalReader(const Options& options) {
  (void)options;
  buf_.resize(kBlockSize);
}

WalReader::~WalReader() {
  if (fd_ >= 0) {
    ::close(fd_);
  }
}

bool WalReader::Open(const std::string& path) {
  fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  return fd_ >= 0;
}

bool WalReader::NextBlock() {
  std::size_t filled = 0;
  while (filled < kBlockSize) {
    const ssize_t n = ::read(fd_, buf_.data() + filled, kBlockSize - filled);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      corrupted_ = true;
      break;
    }
    if (n == 0) {
      eof_ = true;
      break;
    }
    filled += static_cast<std::size_t>(n);
    bytes_read_ += static_cast<std::uint64_t>(n);
  }
  pos_ = 0;
  limit_ = filled;
  return filled > 0;
}

bool WalReader::ReadRecord(std::string* record) {
  record->clear();
  bool in_fragmented = false;
  for (;;) {
    if (limit_ - pos_ < kHeaderSize) {
      if (!NextBlock()) {
        if (in_fragmented) {
          corrupted_ = true;
        }
        return false;
      }
      continue;
    }
    const char* header = buf_.data() + pos_;
    const std::uint32_t crc = internal::DecodeFixed32(header);
    const std::uint32_t length =
        static_cast<std::uint32_t>(static_cast<unsigned char>(header[4])) |
        (static_cast<std::uint32_t>(static_cast<unsigned char>(header[5])) << 8);
    const std::uint8_t type = static_cast<std::uint8_t>(header[6]);

    if (type == kFullType && length == 0 && crc == 0) {
      // 块尾填充：整块剩余字节都是 0，直接跳到下一块。
      pos_ = limit_;
      continue;
    }
    if (length > kBlockSize) {
      corrupted_ = true;
      return false;
    }
    pos_ += kHeaderSize;
    if (length > limit_ - pos_) {
      // 记录跨块：写入端不会产生，视为损坏。
      corrupted_ = true;
      return false;
    }

    const std::size_t old_size = record->size();
    record->resize(old_size + length);
    if (length > 0) {
      std::memcpy(&(*record)[old_size], buf_.data() + pos_, length);
    }
    pos_ += length;

    const std::uint32_t actual = internal::Crc32cMask(internal::Crc32cExtend(
        internal::Crc32cValue(reinterpret_cast<const char*>(&type), 1),
        record->data() + old_size, length));
    if (actual != crc) {
      corrupted_ = true;
      return false;
    }

    switch (type) {
      case kFullType:
        if (in_fragmented) {
          corrupted_ = true;
          return false;
        }
        ++records_;
        return true;
      case kFirstType:
        if (in_fragmented) {
          corrupted_ = true;
          return false;
        }
        in_fragmented = true;
        break;
      case kMiddleType:
        if (!in_fragmented) {
          corrupted_ = true;
          return false;
        }
        break;
      case kLastType:
        if (!in_fragmented) {
          corrupted_ = true;
          return false;
        }
        ++records_;
        return true;
      default:
        corrupted_ = true;
        return false;
    }
  }
}

}  // namespace storage
}  // namespace go2cpp
