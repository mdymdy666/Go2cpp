/**
 * @file file_transfer.cpp
 * @brief MOD-28：文件传输的路径沙箱、摘要、帧编解码与收发状态机实现。
 * @details 设计取舍：
 *
 *          - **顺序写**：接收端只接受 `index == next_index` 的块，乱序一律
 *            `kSequenceError`（Bento 可靠流本身保证按序；顺序假设让 `.part`
 *            的语义简单可靠）。旧块重发是幂等的 `kDuplicateChunk`。
 *          - **校验先行**：块摘要不通过时**不写盘**，因此 `.part` 永远是
 *            「已知连续前缀」，断点续传只需看文件大小。
 *          - **临时文件**：所有数据先写 `<target>.part`，`Finish` 校验整文件
 *            摘要后才 rename，失败不会留下半截的目标文件。
 *          - **路径沙箱**：规范化后比较真实路径前缀，符号链接也逃不出去。
 *
 * @author go2cpp contributors
 * @date 2026-10-28
 * @copyright MIT License
 */

#include "go2cpp/bento/net/file_transfer.hpp"

#include "go2cpp/hash/sha256.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ios>
#include <istream>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace go2cpp::bento {
namespace {

// ---------------------------------------------------------------------------
// 大端读写（与 command.cpp / protocol.cpp 同构；字段少，不抽公共头）
// ---------------------------------------------------------------------------

/// 写 16 位大端。
void WriteU16(std::uint8_t* out, std::uint16_t value) noexcept {
    out[0] = static_cast<std::uint8_t>(value >> 8);
    out[1] = static_cast<std::uint8_t>(value);
}

/// 写 32 位大端。
void WriteU32(std::uint8_t* out, std::uint32_t value) noexcept {
    out[0] = static_cast<std::uint8_t>(value >> 24);
    out[1] = static_cast<std::uint8_t>(value >> 16);
    out[2] = static_cast<std::uint8_t>(value >> 8);
    out[3] = static_cast<std::uint8_t>(value);
}

/// 写 64 位大端。
void WriteU64(std::uint8_t* out, std::uint64_t value) noexcept {
    for (int i = 7; i >= 0; --i) {
        out[7 - i] = static_cast<std::uint8_t>(value >> (i * 8));
    }
}

/// 读 16 位大端。
std::uint16_t ReadU16(const std::uint8_t* in) noexcept {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(in[0]) << 8) |
                                      static_cast<std::uint16_t>(in[1]));
}

/// 读 32 位大端。
std::uint32_t ReadU32(const std::uint8_t* in) noexcept {
    return (static_cast<std::uint32_t>(in[0]) << 24) | (static_cast<std::uint32_t>(in[1]) << 16) |
           (static_cast<std::uint32_t>(in[2]) << 8) | static_cast<std::uint32_t>(in[3]);
}

/// 读 64 位大端。
std::uint64_t ReadU64(const std::uint8_t* in) noexcept {
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value = (value << 8) | static_cast<std::uint64_t>(in[i]);
    }
    return value;
}

// ---------------------------------------------------------------------------
// 配置/长度辅助
// ---------------------------------------------------------------------------

/// 校验配置是否在上限内；失败时写 `why`。
bool ValidateConfig(const FileTransferConfig& config, std::string* why) {
    const auto fail = [why](const char* reason) {
        if (why != nullptr) {
            *why = reason;
        }
        return false;
    };
    if (config.chunk_size == 0 || config.chunk_size > kFileMaxChunkBytes) {
        return fail("chunk_size 越界");
    }
    if (config.max_file_bytes == 0 || config.max_file_bytes > kFileMaxBytesCeiling) {
        return fail("max_file_bytes 越界");
    }
    if (config.max_retries < 0 || config.max_retries > kFileMaxRetriesCeiling) {
        return fail("max_retries 越界");
    }
    if (config.max_path_bytes == 0 || config.max_path_bytes > kFileMaxPathBytes) {
        return fail("max_path_bytes 越界");
    }
    return true;
}

/// 由文件大小与分块大小计算块数（可能超过 uint32，调用方再判）。
std::uint64_t ExpectedChunkCount(std::uint64_t size, std::uint32_t chunk_size) noexcept {
    if (chunk_size == 0 || size == 0) {
        return 0;
    }
    return (size + chunk_size - 1) / chunk_size;
}

/// 第 `index` 块的数据长度（尾块可能小于 chunk_size）。
std::size_t ExpectedChunkLength(std::uint64_t size, std::uint32_t chunk_size,
                                std::uint32_t index) noexcept {
    const std::uint64_t offset = static_cast<std::uint64_t>(index) * chunk_size;
    if (offset >= size) {
        return 0;
    }
    const std::uint64_t remaining = size - offset;
    return static_cast<std::size_t>(std::min<std::uint64_t>(chunk_size, remaining));
}

/// 相对路径的纯字符串校验（不做文件系统解析）。
bool IsValidRelativePath(std::string_view path, std::size_t max_path_bytes) noexcept {
    if (path.empty() || path.size() > max_path_bytes || path.size() > kFileMaxPathBytes) {
        return false;
    }
    if (path.front() == '/') {
        return false;
    }
    bool has_segment = false;
    std::size_t start = 0;
    while (start <= path.size()) {
        const std::size_t slash = path.find('/', start);
        const std::size_t end = slash == std::string_view::npos ? path.size() : slash;
        const std::string_view segment = path.substr(start, end - start);
        for (const char ch : segment) {
            const auto byte = static_cast<unsigned char>(ch);
            if (byte == 0 || byte < 0x20 || byte == 0x7f) {
                return false;
            }
            if (ch == '\\' || ch == ':') {
                return false;
            }
        }
        if (segment == "..") {
            return false;
        }
        if (!segment.empty() && segment != ".") {
            has_segment = true;
        }
        if (slash == std::string_view::npos) {
            break;
        }
        start = slash + 1;
    }
    return has_segment;
}

/// 把相对路径规范化成 filesystem 路径（去掉空段与 "."，不处理 ".."）。
std::filesystem::path NormalizeRelative(std::string_view path) {
    std::filesystem::path normalized;
    std::size_t start = 0;
    while (start <= path.size()) {
        const std::size_t slash = path.find('/', start);
        const std::size_t end = slash == std::string_view::npos ? path.size() : slash;
        const std::string_view segment = path.substr(start, end - start);
        if (!segment.empty() && segment != ".") {
            normalized /= std::filesystem::path(segment);
        }
        if (slash == std::string_view::npos) {
            break;
        }
        start = slash + 1;
    }
    return normalized;
}

// ---------------------------------------------------------------------------
// 摘要
// ---------------------------------------------------------------------------

/// 流式计算一个输入流的前 `size` 字节摘要。
bool HashStream(std::istream& in, std::uint64_t size, FileHash* out) {
    go2cpp::hash::Sha256 hasher;
    in.clear();
    in.seekg(0, std::ios::beg);
    if (!in) {
        return false;
    }
    std::vector<std::uint8_t> buffer(64 * 1024);
    std::uint64_t remaining = size;
    while (remaining > 0) {
        const std::size_t want =
            static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buffer.size()));
        in.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(want));
        const std::streamsize got = in.gcount();
        if (got <= 0) {
            return false;
        }
        hasher.Write(go2cpp::hash::AsBytes(buffer.data(), static_cast<std::size_t>(got)));
        remaining -= static_cast<std::uint64_t>(got);
        in.clear();
    }
    hasher.Sum(go2cpp::hash::AsBytes(*out));
    return true;
}

/// 打开文件并计算前 `size` 字节摘要。
bool HashFile(const std::filesystem::path& path, std::uint64_t size, FileHash* out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    return HashStream(in, size, out);
}

/// 读取文件大小；失败返回 false。
bool FileSize(const std::filesystem::path& path, std::uint64_t* size) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) {
        return false;
    }
    const std::uintmax_t bytes = std::filesystem::file_size(path, ec);
    if (ec) {
        return false;
    }
    *size = static_cast<std::uint64_t>(bytes);
    return true;
}

/// 删除文件（不存在不算失败）。
void RemoveFile(const std::filesystem::path& path) noexcept {
    std::error_code ec;
    (void)std::filesystem::remove(path, ec);
}

// ---------------------------------------------------------------------------
// 发送端 Impl
// ---------------------------------------------------------------------------

struct SenderState {
    FileTransferConfig config;    ///< 打开时使用的配置。
    FileHeader header;            ///< 当前文件描述。
    std::ifstream file;           ///< 源文件读句柄。
    std::uint32_t next_index{0};  ///< 下一次 BuildNextChunkFrame 的块号。
    std::uint64_t next_offset{0}; ///< 下一次发送的字节偏移。
    bool opened{false};           ///< 是否已成功打开。
};

// ---------------------------------------------------------------------------
// 接收端 Impl
// ---------------------------------------------------------------------------

struct ReceiverState {
    FileTransferConfig config;        ///< 打开时使用的配置。
    FileHeader header;                ///< 当前文件描述。
    std::filesystem::path target;     ///< 目标文件绝对路径。
    std::filesystem::path temp;       ///< 临时文件（`.part`）绝对路径。
    std::fstream file;                ///< 临时文件读写句柄。
    std::uint32_t next_index{0};      ///< 下一次期望的块号。
    std::uint64_t next_offset{0};     ///< 已连续写入的字节数。
    std::uint64_t resume_offset{0};   ///< Open 时从 `.part` 恢复的偏移。
    int retries{0};                   ///< 累计重试次数。
    int failures{0};                  ///< 当前块连续失败次数。
    bool opened{false};               ///< 是否已成功打开。
    bool completed{false};            ///< 是否已完成。
};

}  // namespace

// ---------------------------------------------------------------------------
// 状态文本与摘要
// ---------------------------------------------------------------------------

const char* FileTransferStatusToString(FileTransferStatus status) noexcept {
    switch (status) {
        case FileTransferStatus::kOk:
            return "ok";
        case FileTransferStatus::kInvalidArgument:
            return "invalid-argument";
        case FileTransferStatus::kPathRejected:
            return "path-rejected";
        case FileTransferStatus::kTooLarge:
            return "too-large";
        case FileTransferStatus::kIoError:
            return "io-error";
        case FileTransferStatus::kChecksumMismatch:
            return "checksum-mismatch";
        case FileTransferStatus::kSequenceError:
            return "sequence-error";
        case FileTransferStatus::kRetryExhausted:
            return "retry-exhausted";
        case FileTransferStatus::kDuplicateChunk:
            return "duplicate-chunk";
        case FileTransferStatus::kNotReady:
            return "not-ready";
        case FileTransferStatus::kAlreadyCompleted:
            return "already-completed";
        case FileTransferStatus::kTargetExists:
            return "target-exists";
        case FileTransferStatus::kCancelled:
            return "cancelled";
    }
    return "unknown";
}

std::string FileHashHex(const FileHash& hash) {
    static const char* kHex = "0123456789abcdef";
    std::string text;
    text.reserve(kFileHashBytes * 2);
    for (const std::uint8_t byte : hash) {
        text.push_back(kHex[byte >> 4]);
        text.push_back(kHex[byte & 0x0f]);
    }
    return text;
}

FileHash Sha256Of(const std::uint8_t* data, std::size_t size) {
    return go2cpp::hash::Sha256::Digest(go2cpp::hash::AsBytes(data, size));
}

// ---------------------------------------------------------------------------
// 路径沙箱
// ---------------------------------------------------------------------------

FileTransferStatus ResolveTransferPath(const std::string& root,
                                      const std::string& relative_path,
                                      std::size_t max_path_bytes, std::string* resolved) {
    if (resolved == nullptr) {
        return FileTransferStatus::kInvalidArgument;
    }
    if (!IsValidRelativePath(relative_path, max_path_bytes)) {
        return FileTransferStatus::kPathRejected;
    }
    std::error_code ec;
    const std::filesystem::path root_path(root);
    if (!std::filesystem::is_directory(root_path, ec) || ec) {
        return FileTransferStatus::kPathRejected;
    }
    const std::filesystem::path root_canon = std::filesystem::weakly_canonical(root_path, ec);
    if (ec) {
        return FileTransferStatus::kPathRejected;
    }
    const std::filesystem::path candidate = root_canon / NormalizeRelative(relative_path);
    const std::filesystem::path candidate_canon = std::filesystem::weakly_canonical(candidate, ec);
    if (ec) {
        return FileTransferStatus::kPathRejected;
    }
    const std::filesystem::path relative = candidate_canon.lexically_relative(root_canon);
    if (relative.empty()) {
        return FileTransferStatus::kPathRejected;
    }
    const auto first = relative.begin();
    if (first == relative.end() || first->string() == "..") {
        return FileTransferStatus::kPathRejected;
    }
    *resolved = candidate_canon.string();
    return FileTransferStatus::kOk;
}

// ---------------------------------------------------------------------------
// HEADER 帧
// ---------------------------------------------------------------------------

std::vector<std::uint8_t> EncodeFileHeader(const FileHeader& header) {
    if (!IsValidRelativePath(header.path, kFileMaxPathBytes)) {
        return {};
    }
    if (header.chunk_size == 0 || header.chunk_size > kFileMaxChunkBytes) {
        return {};
    }
    const std::uint64_t expected = ExpectedChunkCount(header.size, header.chunk_size);
    if (expected > std::numeric_limits<std::uint32_t>::max() ||
        expected != header.chunk_count) {
        return {};
    }
    const std::size_t total = kFileHeaderFrameHeaderBytes + header.path.size();
    std::vector<std::uint8_t> frame(total, 0);
    frame[0] = kFileFrameMagic0;
    frame[1] = kFileFrameMagic1;
    frame[2] = kFileProtocolVersion;
    frame[3] = kFileFrameTypeHeader;
    WriteU32(frame.data() + 4, header.transfer_id);
    WriteU64(frame.data() + 8, header.size);
    WriteU32(frame.data() + 16, header.chunk_size);
    WriteU32(frame.data() + 20, header.chunk_count);
    std::memcpy(frame.data() + 24, header.sha256.data(), kFileHashBytes);
    WriteU16(frame.data() + 56, static_cast<std::uint16_t>(header.path.size()));
    WriteU16(frame.data() + 58, 0);
    std::memcpy(frame.data() + kFileHeaderFrameHeaderBytes, header.path.data(), header.path.size());
    return frame;
}

bool DecodeFileHeader(const std::uint8_t* data, std::size_t size, FileHeader* out,
                      std::string* error) {
    const auto fail = [error](const char* why) {
        if (error != nullptr) {
            *error = why;
        }
        return false;
    };
    if (data == nullptr || out == nullptr) {
        return fail("data/out 为空");
    }
    if (size < kFileHeaderFrameHeaderBytes) {
        return fail("帧短于固定头");
    }
    if (data[0] != kFileFrameMagic0 || data[1] != kFileFrameMagic1) {
        return fail("magic 不匹配");
    }
    if (data[2] != kFileProtocolVersion) {
        return fail("版本不支持");
    }
    if (data[3] != kFileFrameTypeHeader) {
        return fail("帧类型不是 HEADER");
    }
    const std::uint16_t path_len = ReadU16(data + 56);
    const std::uint16_t reserved = ReadU16(data + 58);
    if (reserved != 0) {
        return fail("保留字段非 0");
    }
    if (static_cast<std::size_t>(path_len) > kFileMaxPathBytes ||
        size != kFileHeaderFrameHeaderBytes + static_cast<std::size_t>(path_len)) {
        return fail("path 长度与帧总长不符");
    }
    FileHeader decoded;
    decoded.transfer_id = ReadU32(data + 4);
    decoded.size = ReadU64(data + 8);
    decoded.chunk_size = ReadU32(data + 16);
    decoded.chunk_count = ReadU32(data + 20);
    std::memcpy(decoded.sha256.data(), data + 24, kFileHashBytes);
    decoded.path.assign(reinterpret_cast<const char*>(data + kFileHeaderFrameHeaderBytes),
                        path_len);
    if (!IsValidRelativePath(decoded.path, kFileMaxPathBytes)) {
        return fail("path 未通过相对路径校验");
    }
    if (decoded.chunk_size == 0 || decoded.chunk_size > kFileMaxChunkBytes) {
        return fail("chunk_size 越界");
    }
    const std::uint64_t expected = ExpectedChunkCount(decoded.size, decoded.chunk_size);
    if (expected > std::numeric_limits<std::uint32_t>::max() ||
        expected != decoded.chunk_count) {
        return fail("chunk_count 与 size/chunk_size 不一致");
    }
    *out = std::move(decoded);
    return true;
}

// ---------------------------------------------------------------------------
// CHUNK 帧
// ---------------------------------------------------------------------------

std::vector<std::uint8_t> EncodeFileChunk(const FileChunk& chunk) {
    if (chunk.data.size() > kFileMaxChunkBytes) {
        return {};
    }
    const std::size_t total = kFileChunkFrameHeaderBytes + chunk.data.size();
    std::vector<std::uint8_t> frame(total, 0);
    frame[0] = kFileFrameMagic0;
    frame[1] = kFileFrameMagic1;
    frame[2] = kFileProtocolVersion;
    frame[3] = kFileFrameTypeChunk;
    WriteU32(frame.data() + 4, chunk.transfer_id);
    WriteU32(frame.data() + 8, chunk.index);
    WriteU64(frame.data() + 12, chunk.offset);
    WriteU32(frame.data() + 20, static_cast<std::uint32_t>(chunk.data.size()));
    std::memcpy(frame.data() + 24, chunk.sha256.data(), kFileHashBytes);
    if (!chunk.data.empty()) {
        std::memcpy(frame.data() + kFileChunkFrameHeaderBytes, chunk.data.data(),
                    chunk.data.size());
    }
    return frame;
}

bool DecodeFileChunk(const std::uint8_t* data, std::size_t size, FileChunk* out,
                     std::string* error) {
    const auto fail = [error](const char* why) {
        if (error != nullptr) {
            *error = why;
        }
        return false;
    };
    if (data == nullptr || out == nullptr) {
        return fail("data/out 为空");
    }
    if (size < kFileChunkFrameHeaderBytes) {
        return fail("帧短于固定头");
    }
    if (data[0] != kFileFrameMagic0 || data[1] != kFileFrameMagic1) {
        return fail("magic 不匹配");
    }
    if (data[2] != kFileProtocolVersion) {
        return fail("版本不支持");
    }
    if (data[3] != kFileFrameTypeChunk) {
        return fail("帧类型不是 CHUNK");
    }
    const std::uint32_t data_len = ReadU32(data + 20);
    if (static_cast<std::size_t>(data_len) > kFileMaxChunkBytes ||
        size != kFileChunkFrameHeaderBytes + static_cast<std::size_t>(data_len)) {
        return fail("data_len 与帧总长不符");
    }
    FileChunk decoded;
    decoded.transfer_id = ReadU32(data + 4);
    decoded.index = ReadU32(data + 8);
    decoded.offset = ReadU64(data + 12);
    std::memcpy(decoded.sha256.data(), data + 24, kFileHashBytes);
    decoded.data.resize(data_len);
    if (data_len > 0) {
        std::memcpy(decoded.data.data(), data + kFileChunkFrameHeaderBytes, data_len);
    }
    *out = std::move(decoded);
    return true;
}

// ---------------------------------------------------------------------------
// ACK 帧
// ---------------------------------------------------------------------------

std::vector<std::uint8_t> EncodeFileAck(const FileAck& ack) {
    if (static_cast<std::uint8_t>(ack.reason) > static_cast<std::uint8_t>(FileAckReason::kCancelled)) {
        return {};
    }
    std::vector<std::uint8_t> frame(kFileAckFrameBytes, 0);
    frame[0] = kFileFrameMagic0;
    frame[1] = kFileFrameMagic1;
    frame[2] = kFileProtocolVersion;
    frame[3] = kFileFrameTypeAck;
    WriteU32(frame.data() + 4, ack.transfer_id);
    WriteU32(frame.data() + 8, ack.next_index);
    WriteU64(frame.data() + 12, ack.next_offset);
    frame[20] = static_cast<std::uint8_t>(ack.reason);
    return frame;
}

bool DecodeFileAck(const std::uint8_t* data, std::size_t size, FileAck* out,
                   std::string* error) {
    const auto fail = [error](const char* why) {
        if (error != nullptr) {
            *error = why;
        }
        return false;
    };
    if (data == nullptr || out == nullptr) {
        return fail("data/out 为空");
    }
    if (size != kFileAckFrameBytes) {
        return fail("ACK 帧长度不匹配");
    }
    if (data[0] != kFileFrameMagic0 || data[1] != kFileFrameMagic1) {
        return fail("magic 不匹配");
    }
    if (data[2] != kFileProtocolVersion) {
        return fail("版本不支持");
    }
    if (data[3] != kFileFrameTypeAck) {
        return fail("帧类型不是 ACK");
    }
    if (data[21] != 0 || data[22] != 0 || data[23] != 0) {
        return fail("保留字段非 0");
    }
    const std::uint8_t reason = data[20];
    if (reason > static_cast<std::uint8_t>(FileAckReason::kCancelled)) {
        return fail("reason 越界");
    }
    FileAck decoded;
    decoded.transfer_id = ReadU32(data + 4);
    decoded.next_index = ReadU32(data + 8);
    decoded.next_offset = ReadU64(data + 12);
    decoded.reason = static_cast<FileAckReason>(reason);
    *out = decoded;
    return true;
}

// ---------------------------------------------------------------------------
// FileSender
// ---------------------------------------------------------------------------

struct FileSender::Impl final : SenderState {};
struct FileReceiver::Impl final : ReceiverState {};

FileSender::FileSender() : m_impl(std::make_unique<Impl>()) {}
FileSender::~FileSender() = default;

FileTransferStatus FileSender::Open(const std::string& source_path,
                                    const std::string& transfer_path,
                                    std::uint32_t transfer_id,
                                    const FileTransferConfig& config) {
    Close();
    std::string why;
    if (!ValidateConfig(config, &why)) {
        return FileTransferStatus::kInvalidArgument;
    }
    if (!IsValidRelativePath(transfer_path, config.max_path_bytes)) {
        return FileTransferStatus::kPathRejected;
    }
    if (source_path.empty()) {
        return FileTransferStatus::kInvalidArgument;
    }
    std::ifstream file(source_path, std::ios::binary);
    if (!file) {
        return FileTransferStatus::kIoError;
    }
    std::error_code size_ec;
    const std::uintmax_t raw_size = std::filesystem::file_size(source_path, size_ec);
    if (size_ec) {
        return FileTransferStatus::kIoError;
    }
    const std::uint64_t size = static_cast<std::uint64_t>(raw_size);
    if (size > config.max_file_bytes || size > kFileMaxBytesCeiling) {
        return FileTransferStatus::kTooLarge;
    }
    const std::uint64_t chunk_count = ExpectedChunkCount(size, static_cast<std::uint32_t>(config.chunk_size));
    if (chunk_count > std::numeric_limits<std::uint32_t>::max()) {
        return FileTransferStatus::kTooLarge;
    }
    FileHash hash{};
    if (!HashStream(file, size, &hash)) {
        return FileTransferStatus::kIoError;
    }
    m_impl->config = config;
    m_impl->header = FileHeader{};
    m_impl->header.transfer_id = transfer_id;
    m_impl->header.path = transfer_path;
    m_impl->header.size = size;
    m_impl->header.chunk_size = static_cast<std::uint32_t>(config.chunk_size);
    m_impl->header.chunk_count = static_cast<std::uint32_t>(chunk_count);
    m_impl->header.sha256 = hash;
    m_impl->file = std::move(file);
    m_impl->next_index = 0;
    m_impl->next_offset = 0;
    m_impl->opened = true;
    return FileTransferStatus::kOk;
}

const FileHeader& FileSender::header() const noexcept { return m_impl->header; }

std::uint32_t FileSender::chunk_count() const noexcept { return m_impl->header.chunk_count; }

std::uint32_t FileSender::next_index() const noexcept { return m_impl->next_index; }

std::uint64_t FileSender::next_offset() const noexcept { return m_impl->next_offset; }

bool FileSender::opened() const noexcept { return m_impl->opened; }

FileTransferStatus FileSender::Seek(std::uint64_t offset) {
    if (!m_impl->opened) {
        return FileTransferStatus::kNotReady;
    }
    if (offset > m_impl->header.size) {
        return FileTransferStatus::kInvalidArgument;
    }
    const std::uint64_t index = offset / m_impl->header.chunk_size;
    m_impl->next_index = static_cast<std::uint32_t>(index);
    m_impl->next_offset = index * m_impl->header.chunk_size;
    return FileTransferStatus::kOk;
}

void FileSender::Rewind() {
    m_impl->next_index = 0;
    m_impl->next_offset = 0;
}

FileTransferStatus FileSender::BuildChunkFrame(std::uint32_t index,
                                               std::vector<std::uint8_t>* frame) {
    if (!m_impl->opened) {
        return FileTransferStatus::kNotReady;
    }
    if (frame == nullptr) {
        return FileTransferStatus::kInvalidArgument;
    }
    if (index >= m_impl->header.chunk_count) {
        return FileTransferStatus::kInvalidArgument;
    }
    const std::uint64_t offset = static_cast<std::uint64_t>(index) * m_impl->header.chunk_size;
    const std::size_t length =
        ExpectedChunkLength(m_impl->header.size, m_impl->header.chunk_size, index);
    FileChunk chunk;
    chunk.transfer_id = m_impl->header.transfer_id;
    chunk.index = index;
    chunk.offset = offset;
    chunk.data.resize(length);
    m_impl->file.clear();
    m_impl->file.seekg(static_cast<std::streamoff>(offset));
    if (!m_impl->file) {
        return FileTransferStatus::kIoError;
    }
    if (length > 0) {
        m_impl->file.read(reinterpret_cast<char*>(chunk.data.data()),
                          static_cast<std::streamsize>(length));
        if (static_cast<std::size_t>(m_impl->file.gcount()) != length) {
            return FileTransferStatus::kIoError;
        }
    }
    chunk.sha256 = Sha256Of(chunk.data.data(), chunk.data.size());
    std::vector<std::uint8_t> encoded = EncodeFileChunk(chunk);
    if (encoded.empty()) {
        return FileTransferStatus::kInvalidArgument;
    }
    *frame = std::move(encoded);
    return FileTransferStatus::kOk;
}

FileTransferStatus FileSender::BuildNextChunkFrame(std::vector<std::uint8_t>* frame) {
    if (!m_impl->opened) {
        return FileTransferStatus::kNotReady;
    }
    if (m_impl->next_index >= m_impl->header.chunk_count) {
        return FileTransferStatus::kAlreadyCompleted;
    }
    const std::uint32_t index = m_impl->next_index;
    std::vector<std::uint8_t> encoded;
    const FileTransferStatus status = BuildChunkFrame(index, &encoded);
    if (status != FileTransferStatus::kOk) {
        return status;
    }
    const std::size_t length =
        ExpectedChunkLength(m_impl->header.size, m_impl->header.chunk_size, index);
    m_impl->next_offset = static_cast<std::uint64_t>(index) * m_impl->header.chunk_size + length;
    ++m_impl->next_index;
    *frame = std::move(encoded);
    return FileTransferStatus::kOk;
}

void FileSender::Close() {
    m_impl->file.close();
    m_impl->opened = false;
    m_impl->header = FileHeader{};
    m_impl->next_index = 0;
    m_impl->next_offset = 0;
}

// ---------------------------------------------------------------------------
// FileReceiver
// ---------------------------------------------------------------------------

FileReceiver::FileReceiver() : m_impl(std::make_unique<Impl>()) {}

FileReceiver::~FileReceiver() {
    if (m_impl && m_impl->file.is_open()) {
        m_impl->file.close();
    }
}

FileTransferStatus FileReceiver::Open(const FileHeader& header, const std::string& root_dir,
                                      const FileTransferConfig& config) {
    Cancel();
    std::string why;
    if (!ValidateConfig(config, &why)) {
        return FileTransferStatus::kInvalidArgument;
    }
    if (header.size > config.max_file_bytes || header.size > kFileMaxBytesCeiling) {
        return FileTransferStatus::kTooLarge;
    }
    if (header.chunk_size == 0 || header.chunk_size > kFileMaxChunkBytes) {
        return FileTransferStatus::kInvalidArgument;
    }
    const std::uint64_t expected = ExpectedChunkCount(header.size, header.chunk_size);
    if (expected > std::numeric_limits<std::uint32_t>::max() ||
        expected != header.chunk_count) {
        return FileTransferStatus::kInvalidArgument;
    }
    if (!IsValidRelativePath(header.path, config.max_path_bytes)) {
        return FileTransferStatus::kPathRejected;
    }
    std::string target_text;
    const FileTransferStatus resolved =
        ResolveTransferPath(root_dir, header.path, config.max_path_bytes, &target_text);
    if (resolved != FileTransferStatus::kOk) {
        return resolved;
    }
    const std::filesystem::path target(target_text);
    std::error_code ec;
    if (std::filesystem::is_directory(target, ec) && !ec) {
        return FileTransferStatus::kPathRejected;
    }
    if (!config.overwrite && std::filesystem::exists(target, ec) && !ec) {
        return FileTransferStatus::kTargetExists;
    }
    const std::filesystem::path parent = target.parent_path();
    if (!parent.empty()) {
        std::error_code dir_ec;
        std::filesystem::create_directories(parent, dir_ec);
        if (dir_ec) {
            return FileTransferStatus::kIoError;
        }
    }
    std::filesystem::path temp = target;
    temp += ".part";
    std::uint64_t resume = 0;
    if (std::filesystem::exists(temp, ec) && !ec) {
        std::uint64_t part_size = 0;
        if (!FileSize(temp, &part_size)) {
            return FileTransferStatus::kIoError;
        }
        if (part_size > header.size) {
            RemoveFile(temp);
        } else {
            resume = (part_size / header.chunk_size) * header.chunk_size;
            if (resume < part_size) {
                std::error_code truncate_ec;
                std::filesystem::resize_file(temp, resume, truncate_ec);
                if (truncate_ec) {
                    return FileTransferStatus::kIoError;
                }
            }
        }
    } else {
        std::ofstream create(temp, std::ios::binary);
        if (!create) {
            return FileTransferStatus::kIoError;
        }
        create.close();
        if (!create) {
            return FileTransferStatus::kIoError;
        }
    }
    std::fstream file(temp, std::ios::in | std::ios::out | std::ios::binary);
    if (!file) {
        return FileTransferStatus::kIoError;
    }
    m_impl->config = config;
    m_impl->header = header;
    m_impl->target = target;
    m_impl->temp = temp;
    m_impl->file = std::move(file);
    m_impl->resume_offset = resume;
    m_impl->next_index = static_cast<std::uint32_t>(resume / header.chunk_size);
    m_impl->next_offset = resume;
    m_impl->retries = 0;
    m_impl->failures = 0;
    m_impl->opened = true;
    m_impl->completed = false;
    return FileTransferStatus::kOk;
}

const FileHeader& FileReceiver::header() const noexcept { return m_impl->header; }

std::string FileReceiver::target_path() const { return m_impl->target.string(); }

std::string FileReceiver::temp_path() const { return m_impl->temp.string(); }

std::uint32_t FileReceiver::next_index() const noexcept { return m_impl->next_index; }

std::uint64_t FileReceiver::next_offset() const noexcept { return m_impl->next_offset; }

std::uint64_t FileReceiver::resume_offset() const noexcept { return m_impl->resume_offset; }

int FileReceiver::retries_used() const noexcept { return m_impl->retries; }

int FileReceiver::chunk_failures() const noexcept { return m_impl->failures; }

bool FileReceiver::opened() const noexcept { return m_impl->opened; }

bool FileReceiver::completed() const noexcept { return m_impl->completed; }

FileTransferStatus FileReceiver::ApplyChunk(const FileChunk& chunk) {
    if (!m_impl->opened) {
        return FileTransferStatus::kNotReady;
    }
    if (m_impl->completed) {
        return FileTransferStatus::kAlreadyCompleted;
    }
    if (chunk.transfer_id != m_impl->header.transfer_id) {
        return FileTransferStatus::kInvalidArgument;
    }
    if (chunk.index >= m_impl->header.chunk_count) {
        return FileTransferStatus::kInvalidArgument;
    }
    if (chunk.index < m_impl->next_index) {
        return FileTransferStatus::kDuplicateChunk;
    }
    if (chunk.index > m_impl->next_index) {
        return FileTransferStatus::kSequenceError;
    }
    const std::uint64_t expected_offset =
        static_cast<std::uint64_t>(m_impl->next_index) * m_impl->header.chunk_size;
    if (chunk.offset != expected_offset) {
        return FileTransferStatus::kInvalidArgument;
    }
    const std::size_t expected_length = ExpectedChunkLength(
        m_impl->header.size, m_impl->header.chunk_size, m_impl->next_index);
    if (chunk.data.size() != expected_length) {
        return FileTransferStatus::kInvalidArgument;
    }
    if (Sha256Of(chunk.data.data(), chunk.data.size()) != chunk.sha256) {
        ++m_impl->retries;
        ++m_impl->failures;
        if (m_impl->failures > m_impl->config.max_retries) {
            return FileTransferStatus::kRetryExhausted;
        }
        return FileTransferStatus::kChecksumMismatch;
    }
    m_impl->file.clear();
    m_impl->file.seekp(static_cast<std::streamoff>(m_impl->next_offset));
    if (!m_impl->file) {
        return FileTransferStatus::kIoError;
    }
    if (!chunk.data.empty()) {
        m_impl->file.write(reinterpret_cast<const char*>(chunk.data.data()),
                           static_cast<std::streamsize>(chunk.data.size()));
    }
    m_impl->file.flush();
    if (!m_impl->file) {
        return FileTransferStatus::kIoError;
    }
    m_impl->next_offset += chunk.data.size();
    ++m_impl->next_index;
    m_impl->failures = 0;
    return FileTransferStatus::kOk;
}

FileTransferStatus FileReceiver::ApplyChunkFrame(const std::uint8_t* data, std::size_t size,
                                                 std::string* error) {
    FileChunk chunk;
    if (!DecodeFileChunk(data, size, &chunk, error)) {
        return FileTransferStatus::kInvalidArgument;
    }
    return ApplyChunk(chunk);
}

FileTransferStatus FileReceiver::Finish() {
    if (!m_impl->opened) {
        return FileTransferStatus::kNotReady;
    }
    if (m_impl->completed) {
        return FileTransferStatus::kAlreadyCompleted;
    }
    if (m_impl->next_index != m_impl->header.chunk_count) {
        return FileTransferStatus::kSequenceError;
    }
    m_impl->file.flush();
    if (!m_impl->file) {
        Cancel();
        return FileTransferStatus::kIoError;
    }
    m_impl->file.close();
    FileHash actual{};
    std::uint64_t actual_size = 0;
    if (!FileSize(m_impl->temp, &actual_size) ||
        !HashFile(m_impl->temp, m_impl->header.size, &actual)) {
        Cancel();
        return FileTransferStatus::kIoError;
    }
    if (actual_size != m_impl->header.size || actual != m_impl->header.sha256) {
        Cancel();
        return FileTransferStatus::kChecksumMismatch;
    }
    std::error_code ec;
    std::filesystem::rename(m_impl->temp, m_impl->target, ec);
    if (ec) {
        return FileTransferStatus::kIoError;
    }
    m_impl->completed = true;
    return FileTransferStatus::kOk;
}

void FileReceiver::Cancel() {
    if (m_impl->file.is_open()) {
        m_impl->file.close();
    }
    if (!m_impl->temp.empty()) {
        RemoveFile(m_impl->temp);
    }
    m_impl->config = FileTransferConfig{};
    m_impl->header = FileHeader{};
    m_impl->target.clear();
    m_impl->temp.clear();
    m_impl->next_index = 0;
    m_impl->next_offset = 0;
    m_impl->resume_offset = 0;
    m_impl->retries = 0;
    m_impl->failures = 0;
    m_impl->opened = false;
    m_impl->completed = false;
}

FileAck FileReceiver::MakeAck(FileAckReason reason) const {
    FileAck ack;
    ack.transfer_id = m_impl->header.transfer_id;
    ack.next_index = m_impl->next_index;
    ack.next_offset = m_impl->next_offset;
    ack.reason = reason;
    return ack;
}

}  // namespace go2cpp::bento
