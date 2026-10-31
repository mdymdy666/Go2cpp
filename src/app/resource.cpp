/**
 * @file resource.cpp
 * @brief 资源传输实现：元数据、块源/块汇、生产者/接收者、索引。
 *
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include "go2cpp/app/resource.hpp"

#include "go2cpp/app/kv_stream.hpp"

#include "go2cpp/hash/crc32.hpp"
#include "go2cpp/hash/sha256.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace go2cpp::app {
namespace {

constexpr std::uint64_t kMaxChunkCount = 1u << 24;
constexpr std::uint32_t kMaxChunkSize = 64u * 1024u * 1024u;
constexpr std::size_t kScanBuffer = 1024u * 1024u;

std::uint64_t NowMs() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

void SetError(std::string* error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
}

std::string ToHex(const std::uint8_t* data, std::size_t size) {
    static const char kHex[] = "0123456789abcdef";
    std::string out;
    out.resize(size * 2);
    for (std::size_t i = 0; i < size; ++i) {
        out[i * 2] = kHex[(data[i] >> 4) & 0x0f];
        out[i * 2 + 1] = kHex[data[i] & 0x0f];
    }
    return out;
}

bool IsHexSha256(const std::string& value) {
    if (value.size() != 64) {
        return false;
    }
    for (char c : value) {
        const bool digit = c >= '0' && c <= '9';
        const bool lower = c >= 'a' && c <= 'f';
        const bool upper = c >= 'A' && c <= 'F';
        if (!digit && !lower && !upper) {
            return false;
        }
    }
    return true;
}

bool ParseU64(const std::string* text, std::uint64_t* out) {
    if (text == nullptr || text->empty() || out == nullptr) {
        return false;
    }
    errno = 0;
    char* end = nullptr;
    const unsigned long long value = std::strtoull(text->c_str(), &end, 10);
    if (end == text->c_str() || *end != '\0' || errno == ERANGE) {
        return false;
    }
    *out = static_cast<std::uint64_t>(value);
    return true;
}

bool ParseU32(const std::string* text, std::uint32_t* out) {
    std::uint64_t value = 0;
    if (!ParseU64(text, &value) || value > 0xffffffffULL) {
        return false;
    }
    *out = static_cast<std::uint32_t>(value);
    return true;
}

/// @brief 规范化块大小（0=默认；夹到允许范围）。
std::uint32_t NormalizeChunkSize(std::uint32_t chunk_size) {
    if (chunk_size == 0) {
        return kDefaultResourceChunkSize;
    }
    return std::min(std::max(chunk_size, kMinResourceChunkSize), kMaxResourceChunkSize);
}

/// @brief 由 size/chunk_size 推导块数。
std::uint32_t DeriveChunkCount(std::uint64_t size, std::uint32_t chunk_size) {
    if (size == 0 || chunk_size == 0) {
        return 0;
    }
    const std::uint64_t count = (size + chunk_size - 1) / chunk_size;
    if (count > kMaxChunkCount) {
        return static_cast<std::uint32_t>(kMaxChunkCount);
    }
    return static_cast<std::uint32_t>(count);
}

std::size_t ReadFull(ChunkSource& source, std::uint64_t offset, std::uint8_t* out,
                     std::size_t want) {
    std::size_t got = 0;
    while (got < want) {
        const std::size_t n = source.Read(offset + got, out + got, want - got);
        if (n == 0) {
            break;
        }
        got += n;
    }
    return got;
}

}  // namespace

// ---------------------------------------------------------------------------
// ResourceMeta
// ---------------------------------------------------------------------------

std::uint32_t ResourceMeta::ChunkBytes(std::uint32_t index) const noexcept {
    if (index >= chunk_count || chunk_size == 0) {
        return 0;
    }
    const std::uint64_t offset = ChunkOffset(index);
    if (offset >= size) {
        return 0;
    }
    const std::uint64_t remaining = size - offset;
    return static_cast<std::uint32_t>(
        std::min<std::uint64_t>(remaining, static_cast<std::uint64_t>(chunk_size)));
}

KvTable ResourceMeta::ToKv() const {
    KvTable table = extra;
    table.Set(kAppKindKey, StreamKindKey(StreamKind::kResource));
    table.Set(kAppNameKey, name);
    table.Set(kAppSizeKey, std::to_string(size));
    if (!sha256_hex.empty()) {
        table.Set(kAppSha256Key, sha256_hex);
    }
    table.Set(kAppChunkSizeKey, std::to_string(chunk_size));
    table.Set(kAppChunksKey, std::to_string(chunk_count));
    table.Set(kAppCrc32Key, std::to_string(crc32));
    table.Set(kAppVersionKey, std::to_string(version));
    if (resume_chunk != 0) {
        table.Set("app.resume", std::to_string(resume_chunk));
    }
    return table;
}

bool ResourceMeta::FromKv(const KvTable& table, ResourceMeta* out, std::string* error) {
    if (out == nullptr) {
        SetError(error, "output meta is null");
        return false;
    }
    if (table.Get(kAppKindKey) != StreamKindKey(StreamKind::kResource)) {
        SetError(error, "app.kind is not resource");
        return false;
    }
    ResourceMeta meta;
    meta.name = table.Get(kAppNameKey);
    if (meta.name.empty()) {
        SetError(error, "missing app.name");
        return false;
    }
    if (!ParseU64(table.Find(kAppSizeKey), &meta.size)) {
        SetError(error, "invalid app.size");
        return false;
    }
    if (const std::string* hash = table.Find(kAppSha256Key); hash != nullptr && !hash->empty()) {
        if (!IsHexSha256(*hash)) {
            SetError(error, "invalid app.sha256");
            return false;
        }
        meta.sha256_hex = *hash;
    }
    std::uint32_t chunk_size = 0;
    if (const std::string* value = table.Find(kAppChunkSizeKey); value != nullptr) {
        if (!ParseU32(value, &chunk_size)) {
            SetError(error, "invalid app.chunk_size");
            return false;
        }
    }
    meta.chunk_size = NormalizeChunkSize(chunk_size);
    std::uint32_t chunk_count = 0;
    if (const std::string* value = table.Find(kAppChunksKey); value != nullptr) {
        if (!ParseU32(value, &chunk_count)) {
            SetError(error, "invalid app.chunks");
            return false;
        }
    }
    const std::uint32_t derived = DeriveChunkCount(meta.size, meta.chunk_size);
    if (chunk_count != 0 && chunk_count != derived) {
        SetError(error, "app.chunks does not match app.size/app.chunk_size");
        return false;
    }
    meta.chunk_count = derived;
    std::uint32_t crc32 = 0;
    if (const std::string* value = table.Find(kAppCrc32Key); value != nullptr) {
        if (!ParseU32(value, &crc32)) {
            SetError(error, "invalid app.crc32");
            return false;
        }
    }
    meta.crc32 = crc32;
    std::uint32_t version = 1;
    if (const std::string* value = table.Find(kAppVersionKey); value != nullptr) {
        if (!ParseU32(value, &version)) {
            SetError(error, "invalid app.version");
            return false;
        }
    }
    meta.version = version;
    std::uint32_t resume_chunk = 0;
    if (const std::string* value = table.Find("app.resume"); value != nullptr) {
        if (!ParseU32(value, &resume_chunk)) {
            SetError(error, "invalid app.resume");
            return false;
        }
    }
    meta.resume_chunk = resume_chunk;
    for (const auto& entry : table.entries()) {
        if (entry.first.rfind("app.", 0) == 0) {
            continue;
        }
        meta.extra.Set(entry.first, entry.second);
    }
    if (!meta.Validate(error)) {
        return false;
    }
    *out = std::move(meta);
    return true;
}

bool ResourceMeta::Validate(std::string* error) const {
    if (size > (1ULL << 62)) {
        SetError(error, "resource size too large");
        return false;
    }
    if (chunk_size == 0 || chunk_size > kMaxChunkSize) {
        SetError(error, "invalid chunk size");
        return false;
    }
    const std::uint32_t derived = DeriveChunkCount(size, chunk_size);
    if (derived > kMaxChunkCount) {
        SetError(error, "too many chunks");
        return false;
    }
    if (chunk_count != 0 && chunk_count != derived) {
        SetError(error, "chunk count mismatch");
        return false;
    }
    if (!sha256_hex.empty() && !IsHexSha256(sha256_hex)) {
        SetError(error, "invalid sha256 hex");
        return false;
    }
    if (resume_chunk > derived) {
        SetError(error, "resume chunk out of range");
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// ChunkSource 实现
// ---------------------------------------------------------------------------

MemoryChunkSource::MemoryChunkSource(std::vector<std::uint8_t> data)
    : m_data(std::make_shared<const std::vector<std::uint8_t>>(std::move(data))) {}

MemoryChunkSource::MemoryChunkSource(
    std::shared_ptr<const std::vector<std::uint8_t>> data)
    : m_data(std::move(data)) {
    if (!m_data) {
        m_data = std::make_shared<const std::vector<std::uint8_t>>();
    }
}

std::uint64_t MemoryChunkSource::size() const { return m_data->size(); }

std::size_t MemoryChunkSource::Read(std::uint64_t offset, std::uint8_t* out,
                                    std::size_t max_bytes) {
    if (out == nullptr || offset >= m_data->size()) {
        return 0;
    }
    const std::size_t available = m_data->size() - static_cast<std::size_t>(offset);
    const std::size_t take = std::min(available, max_bytes);
    std::memcpy(out, m_data->data() + static_cast<std::size_t>(offset), take);
    return take;
}

FileChunkSource::FileChunkSource(std::string path) : m_path(std::move(path)) {}

FileChunkSource::~FileChunkSource() {
    if (m_fd >= 0) {
        ::close(m_fd);
    }
}

FileChunkSource::FileChunkSource(FileChunkSource&& other) noexcept
    : m_path(std::move(other.m_path)), m_fd(other.m_fd), m_size(other.m_size) {
    other.m_fd = -1;
    other.m_size = 0;
}

FileChunkSource& FileChunkSource::operator=(FileChunkSource&& other) noexcept {
    if (this != &other) {
        if (m_fd >= 0) {
            ::close(m_fd);
        }
        m_path = std::move(other.m_path);
        m_fd = other.m_fd;
        m_size = other.m_size;
        other.m_fd = -1;
        other.m_size = 0;
    }
    return *this;
}

bool FileChunkSource::Open(std::string* error) {
    if (m_fd >= 0) {
        return true;
    }
    m_fd = ::open(m_path.c_str(), O_RDONLY);
    if (m_fd < 0) {
        SetError(error, "open failed: " + std::string(std::strerror(errno)));
        return false;
    }
    struct stat st {};
    if (::fstat(m_fd, &st) != 0 || st.st_size < 0) {
        SetError(error, "fstat failed");
        ::close(m_fd);
        m_fd = -1;
        return false;
    }
    m_size = static_cast<std::uint64_t>(st.st_size);
    return true;
}

std::size_t FileChunkSource::Read(std::uint64_t offset, std::uint8_t* out,
                                  std::size_t max_bytes) {
    if (m_fd < 0 || out == nullptr || offset >= m_size) {
        return 0;
    }
    const std::size_t available = static_cast<std::size_t>(m_size - offset);
    std::size_t want = std::min(available, max_bytes);
    std::size_t done = 0;
    while (done < want) {
        const ssize_t n = ::pread(m_fd, out + done, want - done,
                                  static_cast<off_t>(offset + done));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return done;
        }
        if (n == 0) {
            break;
        }
        done += static_cast<std::size_t>(n);
    }
    return done;
}

PatternChunkSource::PatternChunkSource(std::uint64_t size, std::uint64_t seed)
    : m_size(size), m_seed(seed) {}

std::uint64_t PatternChunkSource::ValueAt(std::uint64_t offset8) const noexcept {
    // 64 位 LCG（Knuth MMIX 常数）：每 8 字节一次乘法，生成 1 GiB 也只要
    // 万分之一秒量级，且接收端可脱离数据独立重算。
    return offset8 * 6364136223846793005ULL + m_seed;
}

std::size_t PatternChunkSource::Read(std::uint64_t offset, std::uint8_t* out,
                                     std::size_t max_bytes) {
    if (out == nullptr || offset >= m_size) {
        return 0;
    }
    std::size_t remaining =
        std::min<std::size_t>(static_cast<std::size_t>(m_size - offset), max_bytes);
    std::size_t written = 0;
    // 对齐前导字节。
    while (remaining > 0 && (offset % 8) != 0) {
        const std::uint64_t value = ValueAt(offset / 8);
        out[written] = static_cast<std::uint8_t>((value >> ((offset % 8) * 8)) & 0xFFu);
        ++offset;
        ++written;
        --remaining;
    }
    // 8 字节对齐批量生成。
    while (remaining >= 8) {
        const std::uint64_t value = ValueAt(offset / 8);
        std::memcpy(out + written, &value, 8);
        offset += 8;
        written += 8;
        remaining -= 8;
    }
    // 尾字节。
    if (remaining > 0) {
        const std::uint64_t value = ValueAt(offset / 8);
        for (std::size_t i = 0; i < remaining; ++i) {
            out[written + i] =
                static_cast<std::uint8_t>((value >> ((offset % 8 + i) * 8)) & 0xFFu);
        }
        written += remaining;
    }
    return written;
}

// ---------------------------------------------------------------------------
// ResourceSink 实现
// ---------------------------------------------------------------------------

bool ResourceSink::Open(const ResourceMeta& /*meta*/, std::string* /*error*/) {
    return true;
}

bool ResourceSink::Finish(std::string* /*error*/) { return true; }

bool MemoryResourceSink::Open(const ResourceMeta& meta, std::string* error) {
    if (meta.size > (1ULL << 32)) {
        SetError(error, "memory sink refuses resource larger than 4 GiB");
        return false;
    }
    m_data.assign(static_cast<std::size_t>(meta.size), 0);
    m_received = 0;
    return true;
}

bool MemoryResourceSink::Write(std::uint64_t offset, const std::uint8_t* data,
                               std::size_t size) {
    if (data == nullptr && size != 0) {
        return false;
    }
    if (offset + size > m_data.size()) {
        return false;
    }
    if (size != 0) {
        std::memcpy(m_data.data() + static_cast<std::size_t>(offset), data, size);
    }
    m_received = std::max<std::size_t>(m_received,
                                       static_cast<std::size_t>(offset + size));
    return true;
}

bool MemoryResourceSink::Finish(std::string* error) {
    if (m_received != m_data.size()) {
        SetError(error, "memory sink incomplete");
        return false;
    }
    return true;
}

FileResourceSink::FileResourceSink(std::string path, bool truncate)
    : m_path(std::move(path)), m_truncate(truncate) {}

FileResourceSink::~FileResourceSink() {
    if (m_fd >= 0) {
        ::close(m_fd);
    }
}

bool FileResourceSink::Open(const ResourceMeta& meta, std::string* error) {
    if (m_fd >= 0) {
        return true;
    }
    m_size = meta.size;
    m_fd = ::open(m_path.c_str(), O_RDWR | O_CREAT | (m_truncate ? O_TRUNC : 0), 0644);
    if (m_fd < 0) {
        SetError(error, "open failed: " + std::string(std::strerror(errno)));
        return false;
    }
    if (m_size > 0 && ::ftruncate(m_fd, static_cast<off_t>(m_size)) != 0) {
        SetError(error, "ftruncate failed");
        ::close(m_fd);
        m_fd = -1;
        return false;
    }
    return true;
}

bool FileResourceSink::Write(std::uint64_t offset, const std::uint8_t* data,
                             std::size_t size) {
    if (m_fd < 0 || (data == nullptr && size != 0)) {
        return false;
    }
    std::size_t done = 0;
    while (done < size) {
        const ssize_t n = ::pwrite(m_fd, data + done, size - done,
                                   static_cast<off_t>(offset + done));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (n == 0) {
            return false;
        }
        done += static_cast<std::size_t>(n);
    }
    return true;
}

bool FileResourceSink::Finish(std::string* error) {
    if (m_fd < 0) {
        SetError(error, "file sink not open");
        return false;
    }
    return true;
}

bool DiscardResourceSink::Open(const ResourceMeta& /*meta*/, std::string* /*error*/) {
    m_bytes = 0;
    return true;
}

bool DiscardResourceSink::Write(std::uint64_t /*offset*/, const std::uint8_t* /*data*/,
                                std::size_t size) {
    m_bytes += size;
    return true;
}

bool DiscardResourceSink::Finish(std::string* /*error*/) { return true; }

// ---------------------------------------------------------------------------
// 校验工具
// ---------------------------------------------------------------------------

std::string Sha256Hex(const std::uint8_t* data, std::size_t size) {
    go2cpp::hash::Sha256 sha;
    sha.Write(go2cpp::hash::AsBytes(data, size));
    const std::vector<std::uint8_t> digest = sha.Sum();
    return ToHex(digest.data(), digest.size());
}

std::uint32_t Crc32Of(const std::uint8_t* data, std::size_t size) {
    return go2cpp::hash::Crc32::ChecksumIEEE(go2cpp::hash::AsBytes(data, size));
}

// ---------------------------------------------------------------------------
// ResourceReceiver
// ---------------------------------------------------------------------------

struct ResourceReceiver::Sha256State {
    go2cpp::hash::Sha256 sha;  ///< 增量 SHA-256。
};

ResourceReceiver::ResourceReceiver(ResourceMeta meta, std::unique_ptr<ResourceSink> sink,
                                   std::uint32_t start_chunk)
    : m_meta(std::move(meta)),
      m_sink(std::move(sink)),
      m_start_chunk(start_chunk),
      m_completed_chunks(start_chunk),
      m_started_at_ms(NowMs()) {
    if (m_sink == nullptr) {
        m_sink = std::make_unique<DiscardResourceSink>();
    }
    std::string error;
    if (!m_meta.Validate(&error)) {
        m_error = "invalid resource meta: " + error;
        m_finished = true;
        return;
    }
    m_meta.chunk_count = DeriveChunkCount(m_meta.size, m_meta.chunk_size);
    if (m_start_chunk > m_meta.chunk_count) {
        m_error = "start chunk out of range";
        m_finished = true;
        return;
    }
    m_partial = m_start_chunk > 0;
    m_stats.chunks_expected = m_meta.chunk_count;
    m_sha = std::make_unique<Sha256State>();
    if (!m_sink->Open(m_meta, &error)) {
        m_error = "sink open failed: " + error;
        m_finished = true;
        return;
    }
}

ResourceReceiver::~ResourceReceiver() = default;

bool ResourceReceiver::OnData(const std::uint8_t* payload, std::size_t size,
                              std::string* error) {
    if (m_finished) {
        SetError(error, m_error.empty() ? "resource already finished" : m_error);
        return false;
    }
    if (!m_error.empty()) {
        SetError(error, m_error);
        return false;
    }
    if (payload == nullptr || size < kResourceChunkHeaderBytes) {
        m_error = "resource chunk shorter than header";
        SetError(error, m_error);
        return false;
    }
    const std::uint32_t index = GetU32(payload);
    const std::uint32_t offset = GetU32(payload + 4);
    const std::uint32_t crc = GetU32(payload + 8);
    const std::uint8_t* data = payload + kResourceChunkHeaderBytes;
    const std::size_t data_bytes = size - kResourceChunkHeaderBytes;
    if (index < m_completed_chunks) {
        m_error = "duplicate resource chunk " + std::to_string(index);
        SetError(error, m_error);
        return false;
    }
    if (index > m_completed_chunks) {
        m_error = "resource chunk gap: expected " + std::to_string(m_completed_chunks) +
                  " got " + std::to_string(index);
        SetError(error, m_error);
        return false;
    }
    if (index >= m_meta.chunk_count) {
        m_error = "resource chunk index out of range";
        SetError(error, m_error);
        return false;
    }
    const std::uint32_t expected = m_meta.ChunkBytes(index);
    if (offset != m_chunk_buffer.size()) {
        m_error = "resource chunk offset discontinuity";
        SetError(error, m_error);
        return false;
    }
    if (static_cast<std::uint64_t>(offset) + data_bytes > expected) {
        m_error = "resource chunk exceeds chunk size";
        SetError(error, m_error);
        return false;
    }
    m_chunk_buffer.insert(m_chunk_buffer.end(), data, data + data_bytes);
    if (m_chunk_buffer.size() != expected) {
        return true;
    }
    // 块完整：先校验 CRC，再落盘 + 更新哈希，坏块不污染 sink。
    const std::uint32_t actual = Crc32Of(m_chunk_buffer.data(), m_chunk_buffer.size());
    if (actual != crc) {
        ++m_stats.crc_failures;
        m_error = "resource chunk crc mismatch at " + std::to_string(index) + ": " +
                  std::to_string(actual) + " != " + std::to_string(crc);
        SetError(error, m_error);
        return false;
    }
    const std::uint64_t absolute =
        m_meta.ChunkOffset(index);
    if (!m_sink->Write(absolute, m_chunk_buffer.data(), m_chunk_buffer.size())) {
        m_error = "resource sink write failed at chunk " + std::to_string(index);
        SetError(error, m_error);
        return false;
    }
    if (!m_partial || index >= m_start_chunk) {
        m_sha->sha.Write(go2cpp::hash::AsBytes(m_chunk_buffer.data(),
                                               m_chunk_buffer.size()));
    }
    m_stats.bytes += m_chunk_buffer.size();
    ++m_stats.chunks;
    ++m_completed_chunks;
    m_chunk_buffer.clear();
    return true;
}

bool ResourceReceiver::Finish(std::string* error) {
    if (m_finished) {
        if (!m_error.empty()) {
            SetError(error, m_error);
            return false;
        }
        return true;
    }
    if (!m_error.empty()) {
        SetError(error, m_error);
        return false;
    }
    if (m_completed_chunks != m_meta.chunk_count) {
        m_error = "resource incomplete: " + std::to_string(m_completed_chunks) + "/" +
                  std::to_string(m_meta.chunk_count) + " chunks";
        SetError(error, m_error);
        return false;
    }
    if (!m_chunk_buffer.empty()) {
        m_error = "resource has unflushed chunk bytes";
        SetError(error, m_error);
        return false;
    }
    std::string sink_error;
    if (!m_sink->Finish(&sink_error)) {
        m_error = "sink finish failed: " + sink_error;
        SetError(error, m_error);
        return false;
    }
    const std::vector<std::uint8_t> digest = m_sha->sha.Sum();
    m_stats.sha256_hex = ToHex(digest.data(), digest.size());
    bool verified = true;
    if (!m_meta.sha256_hex.empty() && !m_partial) {
        verified = m_meta.sha256_hex == m_stats.sha256_hex;
        if (!verified) {
            m_error = "resource sha256 mismatch: " + m_stats.sha256_hex + " != " +
                      m_meta.sha256_hex;
            SetError(error, m_error);
            return false;
        }
    }
    m_stats.verified = verified;
    m_stats.elapsed_ms = NowMs() - m_started_at_ms;
    if (m_stats.elapsed_ms == 0) {
        m_stats.elapsed_ms = 1;
    }
    m_stats.throughput_mbps =
        (static_cast<double>(m_stats.bytes) / (1024.0 * 1024.0)) /
        (static_cast<double>(m_stats.elapsed_ms) / 1000.0);
    m_finished = true;
    return true;
}

// ---------------------------------------------------------------------------
// BuildResourceMeta
// ---------------------------------------------------------------------------

ResourceMeta BuildResourceMeta(std::string name, ChunkSource& source,
                               std::uint32_t chunk_size, const KvTable& extra,
                               std::string* error) {
    ResourceMeta meta;
    meta.name = std::move(name);
    meta.size = source.size();
    meta.chunk_size = NormalizeChunkSize(chunk_size);
    meta.chunk_count = DeriveChunkCount(meta.size, meta.chunk_size);
    meta.extra = extra;
    go2cpp::hash::Sha256 sha;
    go2cpp::hash::Crc32 crc;
    std::vector<std::uint8_t> buffer(kScanBuffer);
    std::uint64_t offset = 0;
    while (offset < meta.size) {
        const std::size_t want = static_cast<std::size_t>(
            std::min<std::uint64_t>(buffer.size(), meta.size - offset));
        const std::size_t got = ReadFull(source, offset, buffer.data(), want);
        if (got != want) {
            SetError(error, "resource source truncated");
            return {};
        }
        sha.Write(go2cpp::hash::AsBytes(buffer.data(), got));
        crc.Write(go2cpp::hash::AsBytes(buffer.data(), got));
        offset += got;
    }
    const std::vector<std::uint8_t> digest = sha.Sum();
    meta.sha256_hex = ToHex(digest.data(), digest.size());
    meta.crc32 = crc.Sum32();
    if (!meta.Validate(error)) {
        return {};
    }
    return meta;
}

// ---------------------------------------------------------------------------
// ResourceProducer
// ---------------------------------------------------------------------------

ResourceProducer::ResourceProducer(ResourceMeta meta, std::shared_ptr<ChunkSource> source,
                                   std::uint32_t start_chunk)
    : m_meta(std::move(meta)),
      m_source(std::move(source)),
      m_start_chunk(start_chunk) {
    m_meta.chunk_size = NormalizeChunkSize(m_meta.chunk_size);
    if (m_meta.chunk_count == 0) {
        m_meta.chunk_count = DeriveChunkCount(m_meta.size, m_meta.chunk_size);
    }
    m_next_chunk = std::min(m_start_chunk, m_meta.chunk_count);
    m_start_chunk = m_next_chunk;
    if (m_source == nullptr || m_meta.chunk_count == 0) {
        m_empty = true;
    }
}

bool ResourceProducer::BuildHeaders(std::uint32_t max_payload, Frame* out) {
    KvTable table = m_meta.ToKv();
    if (m_start_chunk != 0) {
        table.Set("app.resume", std::to_string(m_start_chunk));
    }
    std::vector<std::uint8_t> payload = table.ToBinary();
    if (payload.size() > max_payload || payload.size() > kMaxFramePayloadLimit) {
        m_done = true;
        return false;
    }
    out->type = FrameType::kHeaders;
    out->flags = kFlagEndHeaders;
    out->payload = std::move(payload);
    m_headers_sent = true;
    if (m_empty || m_meta.chunk_count == 0) {
        out->flags = static_cast<std::uint8_t>(out->flags | kFlagEndStream);
        m_done = true;
    }
    return true;
}

bool ResourceProducer::LoadChunk(std::string* /*error*/) {
    if (m_next_chunk >= m_meta.chunk_count) {
        return false;
    }
    const std::uint32_t bytes = m_meta.ChunkBytes(m_next_chunk);
    if (bytes == 0) {
        return false;
    }
    m_chunk.assign(bytes, 0);
    const std::uint64_t offset = m_meta.ChunkOffset(m_next_chunk);
    const std::size_t got = ReadFull(*m_source, offset, m_chunk.data(), bytes);
    if (got != bytes) {
        m_done = true;
        return false;
    }
    m_chunk_index = m_next_chunk;
    m_chunk_offset = 0;
    m_chunk_crc = Crc32Of(m_chunk.data(), m_chunk.size());
    ++m_next_chunk;
    return true;
}

bool ResourceProducer::Produce(std::uint32_t max_payload, Frame* out) {
    if (m_done || out == nullptr) {
        return false;
    }
    if (!m_headers_sent) {
        return BuildHeaders(max_payload, out);
    }
    if (m_empty) {
        return false;
    }
    if (m_chunk_offset >= m_chunk.size()) {
        m_chunk.clear();
        m_chunk_offset = 0;
        if (!LoadChunk(nullptr)) {
            return false;
        }
    }
    if (max_payload <= kResourceChunkHeaderBytes) {
        return false;
    }
    const std::size_t capacity =
        static_cast<std::size_t>(max_payload - kResourceChunkHeaderBytes);
    const std::size_t take = std::min(capacity, m_chunk.size() - m_chunk_offset);
    out->type = FrameType::kData;
    out->flags = 0;
    out->payload.clear();
    out->payload.reserve(kResourceChunkHeaderBytes + take);
    PutU32(out->payload, m_chunk_index);
    PutU32(out->payload, m_chunk_offset);
    PutU32(out->payload, m_chunk_crc);
    out->payload.insert(out->payload.end(),
                        m_chunk.begin() + static_cast<std::ptrdiff_t>(m_chunk_offset),
                        m_chunk.begin() +
                            static_cast<std::ptrdiff_t>(m_chunk_offset + take));
    m_chunk_offset += static_cast<std::uint32_t>(take);
    m_produced_bytes += take;
    if (m_next_chunk >= m_meta.chunk_count && m_chunk_offset == m_chunk.size()) {
        out->SetFlag(kFlagEndStream);
        m_done = true;
    }
    return true;
}

// ---------------------------------------------------------------------------
// ResourceIndex
// ---------------------------------------------------------------------------

KvTable ResourceIndex::ToKv() const {
    KvTable table;
    table.Set("count", std::to_string(resources.size()));
    for (std::size_t i = 0; i < resources.size(); ++i) {
        const ResourceMeta& meta = resources[i];
        const std::string prefix = "r" + std::to_string(i) + ".";
        table.Set(prefix + "name", meta.name);
        table.Set(prefix + "size", std::to_string(meta.size));
        table.Set(prefix + "sha256", meta.sha256_hex);
        table.Set(prefix + "chunk_size", std::to_string(meta.chunk_size));
        table.Set(prefix + "chunks", std::to_string(meta.chunk_count));
        table.Set(prefix + "crc32", std::to_string(meta.crc32));
        table.Set(prefix + "version", std::to_string(meta.version));
    }
    return table;
}

bool ResourceIndex::FromKv(const KvTable& table, ResourceIndex* out, std::string* error) {
    if (out == nullptr) {
        SetError(error, "output index is null");
        return false;
    }
    std::uint64_t count = 0;
    if (!ParseU64(table.Find("count"), &count) || count > 100000) {
        SetError(error, "invalid resource index count");
        return false;
    }
    ResourceIndex index;
    index.resources.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t i = 0; i < count; ++i) {
        const std::string prefix = "r" + std::to_string(i) + ".";
        ResourceMeta meta;
        meta.name = table.Get(prefix + "name");
        std::uint64_t size = 0;
        std::uint32_t chunk_size = 0;
        std::uint32_t chunks = 0;
        std::uint32_t crc32 = 0;
        std::uint32_t version = 1;
        if (meta.name.empty() || !ParseU64(table.Find(prefix + "size"), &size)) {
            SetError(error, "invalid resource index entry " + std::to_string(i));
            return false;
        }
        meta.size = size;
        if (!ParseU32(table.Find(prefix + "chunk_size"), &chunk_size)) {
            SetError(error, "invalid resource index chunk_size");
            return false;
        }
        meta.chunk_size = NormalizeChunkSize(chunk_size);
        if (!ParseU32(table.Find(prefix + "chunks"), &chunks)) {
            SetError(error, "invalid resource index chunks");
            return false;
        }
        const std::uint32_t derived = DeriveChunkCount(meta.size, meta.chunk_size);
        if (chunks != derived) {
            SetError(error, "resource index chunks mismatch");
            return false;
        }
        meta.chunk_count = derived;
        if (!ParseU32(table.Find(prefix + "crc32"), &crc32)) {
            SetError(error, "invalid resource index crc32");
            return false;
        }
        meta.crc32 = crc32;
        if (ParseU32(table.Find(prefix + "version"), &version)) {
            meta.version = version;
        }
        if (const std::string* hash = table.Find(prefix + "sha256");
            hash != nullptr && !hash->empty()) {
            if (!IsHexSha256(*hash)) {
                SetError(error, "invalid resource index sha256");
                return false;
            }
            meta.sha256_hex = *hash;
        }
        if (!meta.Validate(error)) {
            return false;
        }
        index.resources.push_back(std::move(meta));
    }
    *out = std::move(index);
    return true;
}

std::vector<std::uint8_t> ResourceIndex::Encode() const { return ToKv().ToBinary(); }

bool ResourceIndex::Decode(const std::uint8_t* data, std::size_t size, ResourceIndex* out,
                           std::string* error) {
    KvTable table;
    if (!KvTable::ParseBinary(data, size, &table, error)) {
        return false;
    }
    return FromKv(table, out, error);
}

}  // namespace go2cpp::app
