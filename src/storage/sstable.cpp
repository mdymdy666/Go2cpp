// Copyright 2026 Go2Cpp Authors. All rights reserved.

#include "sstable.hpp"

#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "go2cpp/storage/kv.hpp"

namespace go2cpp {
namespace storage {

namespace {

constexpr std::uint64_t kMagic = 0x314B5650324F4F47ull;  // "GO2OPVK1"
constexpr std::size_t kFooterSize = 48;
constexpr std::size_t kOutLimit = 256 * 1024;

inline int Compare(std::string_view a, std::string_view b) {
  const std::size_t n = a.size() < b.size() ? a.size() : b.size();
  if (n > 0) {
    const int r = std::memcmp(a.data(), b.data(), n);
    if (r != 0) {
      return r;
    }
  }
  if (a.size() < b.size()) {
    return -1;
  }
  if (a.size() > b.size()) {
    return 1;
  }
  return 0;
}

inline void AppendVarint32(std::string* out, std::uint32_t v) {
  char buf[5];
  char* end = internal::EncodeVarint32(buf, v);
  out->append(buf, static_cast<std::size_t>(end - buf));
}

inline void AppendVarint64(std::string* out, std::uint64_t v) {
  char buf[10];
  char* end = internal::EncodeVarint64(buf, v);
  out->append(buf, static_cast<std::size_t>(end - buf));
}

inline void AppendFixed32(std::string* out, std::uint32_t v) {
  char buf[4];
  internal::EncodeFixed32(buf, v);
  out->append(buf, 4);
}

// 解析一个条目；p/limit 给出边界，失败返回 false。
bool ParseEntry(const char* p, const char* limit, std::string_view* key,
                internal::ValueType* type, std::string_view* value) {
  std::uint32_t klen = 0;
  p = internal::GetVarint32(p, limit, &klen);
  if (p == nullptr || static_cast<std::size_t>(limit - p) < klen) {
    return false;
  }
  *key = std::string_view(p, klen);
  p += klen;
  std::uint32_t vlen = 0;
  p = internal::GetVarint32(p, limit, &vlen);
  if (p == nullptr || p >= limit) {
    return false;
  }
  *type = static_cast<internal::ValueType>(static_cast<std::uint8_t>(*p));
  ++p;
  if (static_cast<std::size_t>(limit - p) < vlen) {
    return false;
  }
  *value = std::string_view(p, vlen);
  return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// BlockBuilder
// ---------------------------------------------------------------------------

BlockBuilder::BlockBuilder(std::size_t block_size) : block_size_(block_size) {}

void BlockBuilder::Add(std::string_view key, internal::ValueType type,
                       std::string_view value) {
  // 偏移相对块首（块头 8 字节），读端 block.data + offset 直接定位。
  offsets_.push_back(static_cast<std::uint32_t>(8 + entries_.size()));
  AppendVarint32(&entries_, static_cast<std::uint32_t>(key.size()));
  if (!key.empty()) {
    entries_.append(key.data(), key.size());
  }
  AppendVarint32(&entries_, static_cast<std::uint32_t>(value.size()));
  entries_.push_back(static_cast<char>(type));
  if (!value.empty()) {
    entries_.append(value.data(), value.size());
  }
  last_key_.assign(key.data(), key.size());
}

std::size_t BlockBuilder::EstimatedSize() const {
  return 8 + entries_.size() + offsets_.size() * sizeof(std::uint32_t);
}

std::string BlockBuilder::Finish() const {
  const std::uint32_t count = static_cast<std::uint32_t>(offsets_.size());
  const std::uint32_t offsets_start =
      static_cast<std::uint32_t>(8 + entries_.size());
  std::string out;
  out.reserve(offsets_start + count * 4);
  out.resize(8);
  internal::EncodeFixed32(&out[0], count);
  internal::EncodeFixed32(&out[4], offsets_start);
  out.append(entries_);
  for (const std::uint32_t off : offsets_) {
    AppendFixed32(&out, off);
  }
  return out;
}

void BlockBuilder::Reset() {
  entries_.clear();
  offsets_.clear();
  last_key_.clear();
}

// ---------------------------------------------------------------------------
// SSTableBuilder
// ---------------------------------------------------------------------------

SSTableBuilder::SSTableBuilder(const Options& options, std::string path)
    : options_(options),
      path_(std::move(path)),
      tmp_path_(path_ + ".tmp"),
      block_(options.block_size < 512 ? 512 : options.block_size) {
  out_.reserve(kOutLimit + 64 * 1024);
}

SSTableBuilder::~SSTableBuilder() {
  if (fd_ >= 0) {
    ::close(fd_);
  }
  if (!finished_) {
    ::unlink(tmp_path_.c_str());
  }
}

bool SSTableBuilder::Open() {
  fd_ = ::open(tmp_path_.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  return fd_ >= 0;
}

bool SSTableBuilder::WriteRaw(const void* data, std::size_t n) {
  out_.append(static_cast<const char*>(data), n);
  file_size_ += n;
  if (out_.size() >= kOutLimit) {
    return FlushOut();
  }
  return true;
}

bool SSTableBuilder::FlushOut() {
  std::size_t off = 0;
  while (off < out_.size()) {
    const ssize_t n = ::write(fd_, out_.data() + off, out_.size() - off);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    off += static_cast<std::size_t>(n);
  }
  out_.clear();
  return true;
}

bool SSTableBuilder::FlushBlock() {
  if (block_.Empty()) {
    return true;
  }
  const std::uint64_t offset = file_size_;
  std::string block = block_.Finish();
  index_keys_.emplace_back(block_.last_key());
  index_offsets_.push_back(offset);
  index_sizes_.push_back(block.size());
  const bool ok = WriteRaw(block.data(), block.size());
  block_.Reset();
  return ok;
}

bool SSTableBuilder::Add(std::string_view key, internal::ValueType type,
                         std::string_view value) {
  if (fd_ < 0) {
    return false;
  }
  block_.Add(key, type, value);
  key_hashes_.push_back(internal::Hash32(key));
  ++num_entries_;
  if (block_.EstimatedSize() >= options_.block_size) {
    return FlushBlock();
  }
  return true;
}

bool SSTableBuilder::Finish() {
  if (fd_ < 0) {
    return false;
  }
  if (num_entries_ == 0) {
    return false;  // 空表不落盘（KV 层会跳过）
  }
  if (!FlushBlock()) {
    return false;
  }

  // ---- bloom ----
  std::uint64_t bloom_offset = 0;
  std::uint64_t bloom_size = 0;
  if (options_.bloom_bits_per_key > 0) {
    bloom_offset = file_size_;
    std::size_t bits = static_cast<std::size_t>(
        num_entries_ * static_cast<std::uint64_t>(options_.bloom_bits_per_key));
    if (bits < 64) {
      bits = 64;
    }
    std::size_t bytes = (bits + 7) / 8;
    bits = bytes * 8;
    std::size_t k = static_cast<std::size_t>(
        static_cast<double>(options_.bloom_bits_per_key) * 0.69);
    if (k < 1) {
      k = 1;
    }
    if (k > 30) {
      k = 30;
    }
    std::vector<char> array(bytes, 0);
    const std::uint32_t nbits = static_cast<std::uint32_t>(bits);
    for (const std::uint32_t h0 : key_hashes_) {
      std::uint32_t h = h0;
      const std::uint32_t delta = (h >> 17) | (h << 15);
      for (std::size_t j = 0; j < k; ++j) {
        const std::uint32_t bit = h % nbits;
        array[bit / 8] = static_cast<char>(array[bit / 8] | (1 << (bit % 8)));
        h += delta;
      }
    }
    char head[5];
    internal::EncodeFixed32(head, static_cast<std::uint32_t>(bytes));
    head[4] = static_cast<char>(k);
    if (!WriteRaw(head, 5) || !WriteRaw(array.data(), array.size())) {
      return false;
    }
    bloom_size = 5 + array.size();
  }

  // ---- index ----
  std::uint64_t index_offset = file_size_;
  {
    BlockBuilder index_block(1u << 20);  // 强制单块：索引整体驻留内存
    for (std::size_t i = 0; i < index_keys_.size(); ++i) {
      std::string handle;
      AppendVarint64(&handle, index_offsets_[i]);
      AppendVarint64(&handle, index_sizes_[i]);
      index_block.Add(index_keys_[i], internal::ValueType::kValue, handle);
    }
    // 索引条目 = [last_key][handle(offset,size)]，读端 GetBlock 按同布局解析。
    std::string index = index_block.Finish();
    if (!WriteRaw(index.data(), index.size())) {
      return false;
    }
  }
  std::uint64_t index_size = file_size_ - index_offset;

  // ---- footer ----
  char footer[kFooterSize];
  internal::EncodeFixed64(footer, kMagic);
  internal::EncodeFixed64(footer + 8, index_offset);
  internal::EncodeFixed64(footer + 16, index_size);
  internal::EncodeFixed64(footer + 24, bloom_offset);
  internal::EncodeFixed64(footer + 32, bloom_size);
  internal::EncodeFixed64(footer + 40, num_entries_);
  if (!WriteRaw(footer, sizeof(footer))) {
    return false;
  }

  if (!FlushOut()) {
    return false;
  }
#if defined(__linux__)
  if (::fdatasync(fd_) != 0) {
    return false;
  }
#endif
  if (::close(fd_) != 0) {
    fd_ = -1;
    return false;
  }
  fd_ = -1;
  if (::rename(tmp_path_.c_str(), path_.c_str()) != 0) {
    return false;
  }
  finished_ = true;
  return true;
}

// ---------------------------------------------------------------------------
// SSTable
// ---------------------------------------------------------------------------

SSTable::~SSTable() {
  if (mapped_ && data_ != nullptr) {
    ::munmap(const_cast<char*>(data_), size_);
  }
  if (fd_ >= 0) {
    ::close(fd_);
  }
}

std::unique_ptr<SSTable> SSTable::Open(const std::string& path,
                                       const Options& options) {
  std::unique_ptr<SSTable> table(new SSTable());
  table->path_ = path;
  table->fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (table->fd_ < 0) {
    return nullptr;
  }
  struct stat st;
  if (::fstat(table->fd_, &st) != 0 || st.st_size < static_cast<off_t>(kFooterSize)) {
    return nullptr;
  }
  table->size_ = static_cast<std::size_t>(st.st_size);

  if (options.use_mmap) {
    void* m = ::mmap(nullptr, table->size_, PROT_READ, MAP_PRIVATE, table->fd_, 0);
    if (m == MAP_FAILED) {
      return nullptr;
    }
    table->data_ = static_cast<const char*>(m);
    table->mapped_ = true;
  } else {
    table->heap_buf_.resize(table->size_);
    std::size_t off = 0;
    while (off < table->size_) {
      const ssize_t n = ::pread(table->fd_, &table->heap_buf_[off],
                                table->size_ - off, static_cast<off_t>(off));
      if (n <= 0) {
        return nullptr;
      }
      off += static_cast<std::size_t>(n);
    }
    table->data_ = table->heap_buf_.data();
  }

  const char* footer = table->data_ + table->size_ - kFooterSize;
  if (internal::DecodeFixed64(footer) != kMagic) {
    return nullptr;
  }
  const std::uint64_t index_offset = internal::DecodeFixed64(footer + 8);
  const std::uint64_t index_size = internal::DecodeFixed64(footer + 16);
  const std::uint64_t bloom_offset = internal::DecodeFixed64(footer + 24);
  const std::uint64_t bloom_size = internal::DecodeFixed64(footer + 32);
  table->num_entries_ = internal::DecodeFixed64(footer + 40);
  if (index_offset + index_size > table->size_ ||
      bloom_offset + bloom_size > table->size_) {
    return nullptr;
  }
  table->index_ = table->data_ + index_offset;
  table->index_size_ = static_cast<std::size_t>(index_size);
  if (bloom_size > 0) {
    table->bloom_ = table->data_ + bloom_offset;
    table->bloom_size_ = static_cast<std::size_t>(bloom_size);
  }
  return table;
}

std::size_t SSTable::FindBlock(std::string_view key) const {
  const std::uint32_t count = internal::DecodeFixed32(index_);
  const char* offsets = index_ + internal::DecodeFixed32(index_ + 4);
  std::size_t lo = 0;
  std::size_t hi = count;
  while (lo < hi) {
    const std::size_t mid = (lo + hi) / 2;
    const char* e = index_ + internal::DecodeFixed32(offsets + mid * 4);
    std::uint32_t klen = 0;
    const char* p = internal::GetVarint32(e, index_ + index_size_, &klen);
    if (p == nullptr) {
      return count;
    }
    const std::string_view last_key(p, klen);
    if (Compare(last_key, key) < 0) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return lo;
}

SSTable::BlockRef SSTable::GetBlock(std::size_t idx) const {
  const std::uint32_t count = internal::DecodeFixed32(index_);
  const char* offsets = index_ + internal::DecodeFixed32(index_ + 4);
  if (idx >= count) {
    return {nullptr, 0};
  }
  const char* e = index_ + internal::DecodeFixed32(offsets + idx * 4);
  std::uint32_t klen = 0;
  const char* p = internal::GetVarint32(e, index_ + index_size_, &klen);
  if (p == nullptr) {
    return {nullptr, 0};
  }
  p += klen;
  // BlockBuilder 写入的 value 长度 = varint 长度 + type 字节 + handle。
  std::uint32_t vlen = 0;
  p = internal::GetVarint32(p, index_ + index_size_, &vlen);
  if (p == nullptr || p >= index_ + index_size_) {
    return {nullptr, 0};
  }
  ++p;  // type
  std::uint64_t offset = 0;
  std::uint64_t size = 0;
  p = internal::GetVarint64(p, index_ + index_size_, &offset);
  if (p == nullptr) {
    return {nullptr, 0};
  }
  p = internal::GetVarint64(p, index_ + index_size_, &size);
  if (p == nullptr || offset + size > size_) {
    return {nullptr, 0};
  }
  return {data_ + offset, static_cast<std::uint32_t>(size)};
}

bool SSTable::MayContain(std::string_view key) const {
  return MayContain(key, internal::Hash32(key));
}

bool SSTable::MayContain(std::string_view key, std::uint32_t hash) const {
  (void)key;  // 只用哈希查询；保留 key 参数是为了和单参重载同形
  if (bloom_ == nullptr || bloom_size_ < 5) {
    return true;
  }
  const std::uint32_t bytes = internal::DecodeFixed32(bloom_);
  const std::size_t k = static_cast<std::uint8_t>(bloom_[4]);
  if (bytes == 0 || bytes + 5 > bloom_size_) {
    return true;
  }
  const char* bits = bloom_ + 5;
  const std::uint32_t nbits = bytes * 8;
  std::uint32_t h = hash;
  const std::uint32_t delta = (h >> 17) | (h << 15);
  for (std::size_t i = 0; i < k; ++i) {
    const std::uint32_t bit = h % nbits;
    if ((bits[bit / 8] & (1 << (bit % 8))) == 0) {
      return false;
    }
    h += delta;
  }
  return true;
}

internal::Lookup SSTable::Get(std::string_view key, std::string* value) const {
  if (!MayContain(key)) {
    return internal::Lookup::kNotFound;
  }
  const std::uint32_t block_count = internal::DecodeFixed32(index_);
  const std::size_t idx = FindBlock(key);
  if (idx >= block_count) {
    return internal::Lookup::kNotFound;
  }
  const BlockRef block = GetBlock(idx);
  if (block.data == nullptr) {
    return internal::Lookup::kNotFound;
  }
  const std::uint32_t count = internal::DecodeFixed32(block.data);
  const char* offsets = block.data + internal::DecodeFixed32(block.data + 4);
  const char* entries_end = offsets;
  // 块内顺序扫描（条目有序，c>0 立即退出）：4KB 块约 40 条，平均扫 20 条，
  // 顺序访问由硬件预取覆盖；二分在这里是负优化（每次比较换一条 cache line）。
  for (std::uint32_t i = 0; i < count; ++i) {
    const char* e = block.data + internal::DecodeFixed32(offsets + i * 4);
    std::string_view k;
    internal::ValueType t;
    std::string_view v;
    if (!ParseEntry(e, entries_end, &k, &t, &v)) {
      return internal::Lookup::kNotFound;
    }
    const int c = Compare(k, key);
    if (c < 0) {
      continue;
    }
    if (c > 0) {
      return internal::Lookup::kNotFound;
    }
    if (t == internal::ValueType::kDeletion) {
      return internal::Lookup::kDeleted;
    }
    if (value != nullptr) {
      value->assign(v.data(), v.size());
    }
    return internal::Lookup::kFound;
  }
  return internal::Lookup::kNotFound;
}

void SSTable::ForEach(
    const std::function<void(std::string_view, internal::ValueType,
                             std::string_view)>& fn) const {
  const std::uint32_t block_count = internal::DecodeFixed32(index_);
  for (std::uint32_t i = 0; i < block_count; ++i) {
    const BlockRef block = GetBlock(i);
    if (block.data == nullptr) {
      return;
    }
    const std::uint32_t count = internal::DecodeFixed32(block.data);
    const char* offsets = block.data + internal::DecodeFixed32(block.data + 4);
    for (std::uint32_t j = 0; j < count; ++j) {
      const char* e = block.data + internal::DecodeFixed32(offsets + j * 4);
      std::string_view k;
      internal::ValueType t;
      std::string_view v;
      if (!ParseEntry(e, offsets, &k, &t, &v)) {
        return;
      }
      fn(k, t, v);
    }
  }
}

// ---------------------------------------------------------------------------
// SSTableIterator
// ---------------------------------------------------------------------------

SSTableIterator::SSTableIterator(const SSTable& table) : table_(table) {
  if (table_.num_entries() == 0) {
    return;
  }
  if (!NextBlock()) {
    return;
  }
  entry_idx_ = 0;
  valid_ = LoadEntry();
}

SSTableIterator::~SSTableIterator() = default;

bool SSTableIterator::NextBlock() {
  const std::uint32_t block_count = internal::DecodeFixed32(table_.index_);
  if (block_idx_ >= block_count) {
    return false;
  }
  const SSTable::BlockRef block = table_.GetBlock(block_idx_);
  if (block.data == nullptr) {
    return false;
  }
  block_ = block.data;
  block_entries_ = internal::DecodeFixed32(block_);
  offsets_ = block_ + internal::DecodeFixed32(block_ + 4);
  block_end_ = offsets_;
  return true;
}

bool SSTableIterator::LoadEntry() {
  if (entry_idx_ >= block_entries_) {
    return false;
  }
  const char* e = block_ + internal::DecodeFixed32(offsets_ + entry_idx_ * 4);
  return ParseEntry(e, block_end_, &key_, &type_, &value_);
}

void SSTableIterator::Next() {
  ++entry_idx_;
  if (entry_idx_ >= block_entries_) {
    ++block_idx_;
    entry_idx_ = 0;
    if (!NextBlock()) {
      valid_ = false;
      return;
    }
  }
  valid_ = LoadEntry();
}

}  // namespace storage
}  // namespace go2cpp
