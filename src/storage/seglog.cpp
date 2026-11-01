// Copyright 2026 Go2Cpp Authors. All rights reserved.
//
// SegLog 实现：段文件写端复用 WalWriter，读端用按偏移定位的 Scanner，
// 段封口时把稀疏检查点写成 SSTable（复用块/footer 格式）。

#include "seglog.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "format.hpp"
#include "go2cpp/storage/kv.hpp"
#include "sstable.hpp"
#include "wal.hpp"

namespace go2cpp {
namespace storage {

namespace {

// 与 wal.cpp 的 kBlockSize 保持一致（记录不跨 64KB 块）。
constexpr std::size_t kBlockSize = 64 * 1024;
constexpr std::size_t kHeaderSize = 7;  // crc32(4) + len(2) + type(1)

enum RecordType : std::uint8_t {
  kFullType = 1,
  kFirstType = 2,
  kMiddleType = 3,
  kLastType = 4,
};

void EncodeBe64(void* dst, std::uint64_t v) {
  auto* p = static_cast<std::uint8_t*>(dst);
  for (int i = 0; i < 8; ++i) {
    p[i] = static_cast<std::uint8_t>(v >> (56 - 8 * i));
  }
}

std::uint64_t DecodeBe64(const void* src) {
  const auto* p = static_cast<const std::uint8_t*>(src);
  std::uint64_t v = 0;
  for (int i = 0; i < 8; ++i) {
    v = (v << 8) | p[i];
  }
  return v;
}

void EncodeBe32(void* dst, std::uint32_t v) {
  auto* p = static_cast<std::uint8_t*>(dst);
  for (int i = 0; i < 4; ++i) {
    p[i] = static_cast<std::uint8_t>(v >> (24 - 8 * i));
  }
}

bool EnsureDir(const std::string& dir) {
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  return std::filesystem::is_directory(dir);
}

// 从目录名解析段号：必须是纯数字、长度 6。
bool ParseSegmentName(const std::string& name, std::uint32_t* no,
                      std::string* ext) {
  const std::size_t dot = name.rfind('.');
  if (dot == std::string::npos || dot != 6) {
    return false;
  }
  for (std::size_t i = 0; i < dot; ++i) {
    if (name[i] < '0' || name[i] > '9') {
      return false;
    }
  }
  *no = static_cast<std::uint32_t>(std::stoul(name.substr(0, dot)));
  *ext = name.substr(dot + 1);
  return true;
}

std::string IndexPathFor(const std::string& dir, std::uint32_t no) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%06u.idx", no);
  return dir + "/" + buf;
}

}  // namespace

// ---------------------------------------------------------------------------
// Scanner
// ---------------------------------------------------------------------------

SegLog::Scanner::Scanner() = default;

SegLog::Scanner::~Scanner() { Close(); }

bool SegLog::Scanner::Open(const std::string& path, bool verify_crc) {
  Close();
  fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd_ < 0) {
    return false;
  }
  struct stat st;
  if (::fstat(fd_, &st) != 0) {
    Close();
    return false;
  }
  file_size_ = static_cast<std::uint64_t>(st.st_size);
  verify_crc_ = verify_crc;
  block_offset_ = 0;
  pos_ = 0;
  block_.clear();
  record_.clear();
  // 立刻装载第 0 块：否则 ReadRecord 看到空 block_ 会直接跳到第 1 块，
  // 把整个首块（64KB）漏掉。
  if (!SeekTo(0)) {
    Close();
    return false;
  }
  return true;
}

void SegLog::Scanner::Close() {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
  block_.clear();
  record_.clear();
  pos_ = 0;
  block_offset_ = 0;
  file_size_ = 0;
}

bool SegLog::Scanner::LoadBlock(std::uint64_t block_offset) {
  if (block_offset >= file_size_) {
    return false;
  }
  const std::size_t want = static_cast<std::size_t>(
      std::min<std::uint64_t>(kBlockSize, file_size_ - block_offset));
  block_.resize(want);
  std::size_t got = 0;
  while (got < want) {
    const ssize_t n =
        ::pread(fd_, &block_[got], want - got,
                static_cast<off_t>(block_offset + got));
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    if (n == 0) {
      break;
    }
    got += static_cast<std::size_t>(n);
  }
  block_.resize(got);
  block_offset_ = block_offset;
  pos_ = 0;
  return true;
}

bool SegLog::Scanner::SeekTo(std::uint64_t byte_offset) {
  const std::uint64_t base = (byte_offset / kBlockSize) * kBlockSize;
  if (base >= file_size_) {
    block_.clear();
    block_offset_ = base;
    pos_ = 0;
    return true;  // 落在文件尾之后：合法（等新数据）
  }
  if (!LoadBlock(base)) {
    return false;
  }
  pos_ = static_cast<std::size_t>(byte_offset - base);
  if (pos_ > block_.size()) {
    pos_ = block_.size();
  }
  return true;
}

bool SegLog::Scanner::ReadRecord(std::string_view* out) {
  return ReadRecord(out, nullptr);
}

bool SegLog::Scanner::ReadRecord(std::string_view* out,
                                 std::uint64_t* record_offset) {
  if (fd_ < 0) {
    return false;
  }
  record_.clear();
  bool in_fragment = false;
  std::uint64_t start_off = 0;
  bool have_start = false;

  for (;;) {
    if (pos_ + kHeaderSize > block_.size()) {
      const std::uint64_t next = block_offset_ + kBlockSize;
      if (next >= file_size_) {
        // 可能是「尾段还在写」：刷新一次文件大小再看。
        struct stat st;
        if (::fstat(fd_, &st) == 0 &&
            static_cast<std::uint64_t>(st.st_size) > file_size_) {
          file_size_ = static_cast<std::uint64_t>(st.st_size);
          if (next < file_size_ && LoadBlock(next)) {
            continue;
          }
        }
        return false;
      }
      if (!LoadBlock(next)) {
        return false;
      }
      continue;
    }

    const char* h = block_.data() + pos_;
    const std::uint32_t crc = internal::DecodeFixed32(h);
    const std::uint32_t len =
        static_cast<std::uint32_t>(static_cast<unsigned char>(h[4])) |
        (static_cast<std::uint32_t>(static_cast<unsigned char>(h[5])) << 8);
    const auto type = static_cast<std::uint8_t>(h[6]);

    if (len == 0 && type == 0) {
      // 块尾填充（写端在 leftover < kHeaderSize 时补 0）。
      pos_ = block_.size();
      continue;
    }
    if (pos_ + kHeaderSize + len > block_.size()) {
      return false;  // 截断
    }

    const std::string_view frag(h + kHeaderSize, len);
    if (verify_crc_) {
      const std::uint32_t want = internal::Crc32cMask(
          internal::Crc32cExtend(internal::Crc32cValue(h + 6, 1), frag.data(),
                                 frag.size()));
      if (want != crc) {
        return false;
      }
    }

    if (!have_start) {
      start_off = block_offset_ + pos_;
      have_start = true;
    }
    pos_ += kHeaderSize + len;

    switch (type) {
      case kFullType:
        record_.assign(frag.data(), frag.size());
        if (record_offset != nullptr) {
          *record_offset = start_off;
        }
        *out = std::string_view(record_);
        return true;
      case kFirstType:
        record_.assign(frag.data(), frag.size());
        in_fragment = true;
        break;
      case kMiddleType:
        if (!in_fragment) {
          return false;
        }
        record_.append(frag.data(), frag.size());
        break;
      case kLastType:
        if (!in_fragment) {
          return false;
        }
        record_.append(frag.data(), frag.size());
        if (record_offset != nullptr) {
          *record_offset = start_off;
        }
        *out = std::string_view(record_);
        return true;
      default:
        return false;
    }
  }
}

// ---------------------------------------------------------------------------
// SegLog
// ---------------------------------------------------------------------------

SegLog::SegLog() = default;

SegLog::~SegLog() { Close(); }

std::string SegLog::PathFor(std::uint32_t no, const char* ext) const {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%06u", no);
  return dir_ + "/" + buf + "." + ext;
}

bool SegLog::Fail(const std::string& what) {
  if (error_.empty()) {
    error_ = what;
  }
  return false;
}

bool SegLog::Open(const std::string& dir, const SegLogOptions& options) {
  opt_ = options;
  dir_ = dir;
  error_.clear();
  if (!EnsureDir(dir_)) {
    return Fail("cannot create dir: " + dir_);
  }

  std::vector<std::pair<std::uint32_t, std::string>> files;
  DIR* d = ::opendir(dir_.c_str());
  if (d == nullptr) {
    return Fail("opendir failed: " + dir_);
  }
  struct dirent* ent;
  while ((ent = ::readdir(d)) != nullptr) {
    const std::string name(ent->d_name);
    std::uint32_t no = 0;
    std::string ext;
    if (ParseSegmentName(name, &no, &ext) && ext == "seg") {
      files.emplace_back(no, name);
    }
  }
  ::closedir(d);
  std::sort(files.begin(), files.end());

  segments_.clear();
  std::uint64_t seq = 0;
  for (std::size_t i = 0; i < files.size(); ++i) {
    const auto& f = files[i];
    SegmentInfo info;
    info.no = f.first;
    info.start_seq = seq;
    std::uint64_t count = 0;
    std::uint64_t bytes = 0;
    std::uint64_t last_valid = 0;
    const std::string path = dir_ + "/" + f.second;
    if (!ScanSegment(path, 0, &count, &bytes, &last_valid)) {
      return Fail("corrupt segment: " + f.second);
    }
    // 最后一段若尾部有不完整记录（kill -9 的典型残留），截断到最后一个
    // 完整记录的末尾再继续追加；否则新记录会被尾部垃圾挡住读不出来。
    if (i + 1 == files.size() && last_valid < bytes) {
      if (::truncate(path.c_str(), static_cast<off_t>(last_valid)) != 0) {
        return Fail("truncate failed: " + f.second);
      }
      bytes = last_valid;
    }
    info.count = count;
    info.bytes = bytes;
    seq += count;
    segments_.push_back(info);
  }
  next_seq_ = seq;
  total_bytes_ = 0;
  for (const auto& s : segments_) {
    total_bytes_ += s.bytes;
  }

  // 写端：最后一段；没有则新建 0 号段。
  std::uint32_t last_no = 0;
  std::uint64_t last_bytes = 0;
  if (!segments_.empty()) {
    last_no = segments_.back().no;
    last_bytes = segments_.back().bytes;
  } else {
    segments_.push_back(SegmentInfo{0, 0, 0, 0});
  }
  if (!OpenWriteSegment(last_no, last_bytes)) {
    return false;
  }
  write_no_ = last_no;
  write_bytes_ = last_bytes;
  slot_seqs_.clear();
  slots_.clear();
  opened_ = true;
  closed_ = false;
  return true;
}

bool SegLog::OpenWriteSegment(std::uint32_t no, std::uint64_t existing_size) {
  Options wal_opt;
  wal_opt.write_buffer_size = opt_.write_buffer;
  writer_ = std::make_unique<WalWriter>(wal_opt);
  if (!writer_->Open(PathFor(no, "seg"), existing_size)) {
    return Fail("cannot open segment: " + PathFor(no, "seg"));
  }
  return true;
}

bool SegLog::ScanSegment(const std::string& path, std::uint64_t count_limit,
                         std::uint64_t* count, std::uint64_t* bytes,
                         std::uint64_t* last_valid) {
  Scanner scanner;
  if (!scanner.Open(path, opt_.verify_crc_on_recover)) {
    return false;
  }
  std::uint64_t n = 0;
  std::string_view rec;
  while (count_limit == 0 || n < count_limit) {
    if (!scanner.ReadRecord(&rec)) {
      break;
    }
    ++n;
  }
  *count = n;
  *last_valid = scanner.position();
  struct stat st;
  *bytes = 0;
  if (::stat(path.c_str(), &st) == 0) {
    *bytes = static_cast<std::uint64_t>(st.st_size);
  }
  return true;
}

std::size_t SegLog::FindSegment(std::uint64_t seq) const {
  if (segments_.empty()) {
    return 0;
  }
  // 最后一个 start_seq <= seq 的段。
  std::size_t lo = 0;
  std::size_t hi = segments_.size();
  while (lo < hi) {
    const std::size_t mid = lo + (hi - lo) / 2;
    if (segments_[mid].start_seq <= seq) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return lo == 0 ? 0 : lo - 1;
}

bool SegLog::Append(std::string_view record, std::uint64_t* seq) {
  if (!opened_ || closed_) {
    return Fail("seglog not open");
  }
  if (writer_ == nullptr) {
    return Fail("seglog writer missing");
  }
  const std::uint64_t prev_write =
      writer_->BytesWritten() + writer_->PendingBytes();
  const std::uint64_t local = next_seq_ - segments_.back().start_seq;
  if (local % opt_.index_interval == 0) {
    slot_seqs_.push_back(next_seq_);
    LogSlot slot;
    slot.offset = prev_write;
    slot.length = static_cast<std::uint32_t>(
        std::min<std::size_t>(record.size(), 0xffffffffu));
    slots_.push_back(slot);
  }
  if (!writer_->AddRecord(record)) {
    return Fail("segment append failed");
  }
  ++segments_.back().count;
  ++next_seq_;
  if (seq != nullptr) {
    *seq = next_seq_ - 1;
  }
  write_bytes_ = writer_->BytesWritten() + writer_->PendingBytes();
  total_bytes_ += write_bytes_ - prev_write;
  segments_.back().bytes = write_bytes_;
  if (write_bytes_ >= opt_.segment_size) {
    if (!Roll()) {
      return false;
    }
  }
  return true;
}

bool SegLog::Roll() {
  if (writer_ == nullptr) {
    return true;
  }
  if (opt_.sync_on_roll) {
    writer_->Sync();
  }
  writer_->FlushBuffer();
  writer_->Close();
  const SegmentInfo seg = segments_.back();
  if (!slot_seqs_.empty()) {
    WriteIndex(seg, slot_seqs_, slots_);
  }
  slot_seqs_.clear();
  slots_.clear();

  const std::uint32_t next_no = seg.no + 1;
  SegmentInfo fresh;
  fresh.no = next_no;
  fresh.start_seq = next_seq_;
  if (!OpenWriteSegment(next_no, 0)) {
    return false;
  }
  segments_.push_back(fresh);
  write_no_ = next_no;
  write_bytes_ = 0;
  return true;
}

bool SegLog::WriteIndex(const SegmentInfo& seg,
                        const std::vector<std::uint64_t>& slot_seqs,
                        const std::vector<LogSlot>& slots) {
  if (slot_seqs.empty() || slot_seqs.size() != slots.size()) {
    return true;
  }
  Options idx_opt;
  idx_opt.block_size = 4096;
  idx_opt.bloom_bits_per_key = 0;
  SSTableBuilder builder(idx_opt, PathFor(seg.no, "idx"));
  if (!builder.Open()) {
    return false;
  }
  char key[8];
  char val[12];
  for (std::size_t i = 0; i < slot_seqs.size(); ++i) {
    EncodeBe64(key, slot_seqs[i]);
    EncodeBe64(val, slots[i].offset);
    EncodeBe32(val + 8, slots[i].length);
    if (!builder.Add(std::string_view(key, sizeof(key)),
                     internal::ValueType::kValue,
                     std::string_view(val, sizeof(val)))) {
      return false;
    }
  }
  return builder.Finish();
}

bool SegLog::Flush() {
  if (!opened_ || writer_ == nullptr) {
    return false;
  }
  return writer_->FlushBuffer();
}

bool SegLog::Sync() {
  if (!opened_ || writer_ == nullptr) {
    return false;
  }
  if (!writer_->FlushBuffer()) {
    return false;
  }
  return writer_->Sync();
}

std::size_t SegLog::buffered_bytes() const {
  return writer_ != nullptr ? writer_->PendingBytes() : 0;
}

void SegLog::Close() {
  if (!opened_) {
    return;
  }
  if (writer_ != nullptr) {
    writer_->FlushBuffer();
    writer_->Sync();
    writer_->Close();
    if (!slot_seqs_.empty() && !segments_.empty()) {
      const SegmentInfo seg = segments_.back();
      WriteIndex(seg, slot_seqs_, slots_);
    }
    writer_.reset();
  }
  slot_seqs_.clear();
  slots_.clear();
  opened_ = false;
  closed_ = true;
}

std::vector<SegLog::SegmentStat> SegLog::GetSegmentStats() const {
  std::vector<SegmentStat> out;
  out.reserve(segments_.size());
  for (const auto& s : segments_) {
    SegmentStat stat;
    stat.no = s.no;
    stat.start_seq = s.start_seq;
    stat.count = s.count;
    stat.bytes = s.bytes;
    if (s.no != write_no_) {
      stat.bytes = s.bytes;
    } else {
      stat.bytes = write_bytes_;
    }
    stat.has_index =
        ::access(IndexPathFor(dir_, s.no).c_str(), F_OK) == 0;
    out.push_back(stat);
  }
  return out;
}

// ---------------------------------------------------------------------------
// 索引定位 + 随机读
// ---------------------------------------------------------------------------

namespace {

// 在段索引里找 <= target 的最近检查点。失败时 *base_seq = 段首、*offset = 0。
void LocateInIndex(const std::string& idx_path, std::uint64_t seg_start_seq,
                   std::uint64_t target, std::uint64_t* base_seq,
                   std::uint64_t* offset) {
  *base_seq = seg_start_seq;
  *offset = 0;
  Options idx_opt;
  idx_opt.block_size = 4096;
  idx_opt.bloom_bits_per_key = 0;
  auto table = SSTable::Open(idx_path, idx_opt);
  if (table == nullptr) {
    return;
  }
  SSTableIterator it(*table);
  for (; it.Valid(); it.Next()) {
    if (it.key().size() != 8) {
      break;
    }
    const std::uint64_t k = DecodeBe64(it.key().data());
    if (k > target) {
      break;
    }
    if (it.value().size() >= 8) {
      *base_seq = k;
      *offset = DecodeBe64(it.value().data());
    }
  }
}

}  // namespace

bool SegLog::Read(std::uint64_t seq, std::string* out) {
  if (!opened_ || seq >= next_seq_ || segments_.empty()) {
    return false;
  }
  const std::size_t idx = FindSegment(seq);
  if (idx >= segments_.size()) {
    return false;
  }
  const SegmentInfo& seg = segments_[idx];
  std::uint64_t base_seq = seg.start_seq;
  std::uint64_t offset = 0;
  if (::access(IndexPathFor(dir_, seg.no).c_str(), F_OK) == 0) {
    LocateInIndex(IndexPathFor(dir_, seg.no), seg.start_seq, seq, &base_seq,
                  &offset);
  }
  Scanner scanner;
  if (!scanner.Open(PathFor(seg.no, "seg"), true)) {
    return false;
  }
  if (!scanner.SeekTo(offset)) {
    return false;
  }
  std::string_view rec;
  for (std::uint64_t cur = base_seq;; ++cur) {
    if (!scanner.ReadRecord(&rec)) {
      return false;
    }
    if (cur == seq) {
      out->assign(rec.data(), rec.size());
      return true;
    }
  }
}

// ---------------------------------------------------------------------------
// Cursor
// ---------------------------------------------------------------------------

SegLog::Cursor::Cursor() = default;

SegLog::Cursor::~Cursor() { Close(); }

void SegLog::Cursor::Close() {
  scanner_.Close();
  log_ = nullptr;
  valid_ = false;
  seq_ = 0;
  seg_end_seq_ = 0;
}

bool SegLog::Cursor::OpenSegment(std::size_t idx) {
  if (log_ == nullptr || idx >= log_->segments_.size()) {
    return false;
  }
  const SegmentInfo& seg = log_->segments_[idx];
  if (!scanner_.Open(log_->PathFor(seg.no, "seg"), true)) {
    return false;
  }
  if (!scanner_.SeekTo(0)) {
    return false;
  }
  seg_idx_ = idx;
  seg_end_seq_ = seg.start_seq + seg.count;
  return true;
}

bool SegLog::Cursor::Seek(SegLog* log, std::uint64_t seq) {
  Close();
  if (log == nullptr || !log->opened_) {
    return false;
  }
  log_ = log;
  seq_ = seq;
  if (log_->segments_.empty()) {
    return false;
  }
  std::size_t idx = log_->FindSegment(seq);
  if (idx >= log_->segments_.size()) {
    idx = log_->segments_.size() - 1;
  }
  const SegmentInfo& seg = log_->segments_[idx];
  std::uint64_t base_seq = seg.start_seq;
  std::uint64_t offset = 0;
  if (::access(IndexPathFor(log_->dir_, seg.no).c_str(), F_OK) == 0) {
    LocateInIndex(IndexPathFor(log_->dir_, seg.no), seg.start_seq, seq,
                  &base_seq, &offset);
  }
  if (!scanner_.Open(log_->PathFor(seg.no, "seg"), true)) {
    return false;
  }
  if (!scanner_.SeekTo(offset)) {
    return false;
  }
  seg_idx_ = idx;
  seg_end_seq_ = seg.start_seq + seg.count;
  // 从检查点跳到目标 seq（最多 index_interval 条）。
  std::string_view rec;
  std::uint64_t cur = base_seq;
  while (cur < seq) {
    if (!scanner_.ReadRecord(&rec)) {
      // 目标在写端之外：停在当前段尾，等待新记录。
      valid_ = true;
      return true;
    }
    ++cur;
  }
  valid_ = true;
  return true;
}

bool SegLog::Cursor::AdvanceSegment() {
  if (log_ == nullptr) {
    return false;
  }
  const std::size_t next = seg_idx_ + 1;
  if (next >= log_->segments_.size()) {
    return false;
  }
  const std::uint64_t start = log_->segments_[next].start_seq;
  if (!OpenSegment(next)) {
    return false;
  }
  seq_ = start;
  return true;
}

bool SegLog::Cursor::Next(std::string_view* out) {
  if (log_ == nullptr || !valid_) {
    return false;
  }
  for (;;) {
    std::string_view rec;
    if (scanner_.ReadRecord(&rec)) {
      *out = rec;
      ++seq_;
      return true;
    }
    // 本段读完：切下一段；没有下一段说明追上了写端。
    if (!AdvanceSegment()) {
      return false;
    }
  }
}

}  // namespace storage
}  // namespace go2cpp
