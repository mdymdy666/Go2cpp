/**
 * @file zip.cpp
 * @brief MOD-31：store-only ZIP 写入/读取实现。
 * @details 设计取舍：
 *
 *          - **固定 30B/46B 头、无 extra/注释/数据描述符**：读取端不需要
 *            处理可变长度扩展；写入端在写完数据后 seek 回填 CRC/大小，
 *            避免把整个文件缓存进内存。
 *          - **失败即 poison**：写入过程中任何 IO/上限错误都会把实例标成
 *            失败态，调用方只能 `Abort()`。这样不会在归档里留下「一半
 *            条目 + 正确中央目录」的不可诊断状态。
 *          - **读取先看声明**：解析中央目录时就按声明的未压缩大小检查
 *            上限，解压阶段不再相信任何未校验的字段。
 *          - **Zip Slip**：名字先做语法校验（相对、无 `..`、无反斜杠/
 *            冒号/控制字符），再把父目录 canonical 化后确认仍在输出根内。
 *
 * @author go2cpp contributors
 * @date 2026-10-28
 * @copyright MIT License
 */

#include "go2cpp/bento/app/zip.hpp"

#include "go2cpp/hash/crc32.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <ios>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

namespace go2cpp::bento::app {
namespace {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// ZIP 常量与字节序
// ---------------------------------------------------------------------------

constexpr std::uint32_t kLocalSignature = 0x04034b50u;   ///< "PK\x03\x04"
constexpr std::uint32_t kCentralSignature = 0x02014b50u; ///< "PK\x01\x02"
constexpr std::uint32_t kEocdSignature = 0x06054b50u;    ///< "PK\x05\x06"
constexpr std::size_t kLocalHeaderBytes = 30;            ///< 本地文件头固定长度
constexpr std::size_t kCentralHeaderBytes = 46;          ///< 中央目录项固定长度
constexpr std::size_t kEocdBytes = 22;                   ///< EOCD 固定长度
constexpr std::size_t kMaxCommentBytes = 65535;          ///< EOCD 注释上限
constexpr std::uint64_t kMaxCentralDirectoryBytes = 64ull << 20; ///< 中央目录内存上限
constexpr std::uint16_t kFlagUtf8 = 1u << 11;            ///< bit 11：名字是 UTF-8
constexpr std::uint16_t kFlagEncrypted = 1u << 0;        ///< bit 0：加密
constexpr std::uint16_t kFlagDataDescriptor = 1u << 3;   ///< bit 3：数据描述符
constexpr std::uint16_t kVersionStore = 20;              ///< 需要 2.0（store）
constexpr std::uint16_t kVersionMadeByUnix = (3u << 8) | 30u; ///< UNIX + 3.0
constexpr std::uint32_t kExternalFileAttrs = 0100644u << 16;  ///< 普通文件 0644
constexpr std::uint32_t kExternalDirAttrs = (0040755u << 16) | 0x10u; ///< 目录 0755 + DOS 目录位
constexpr std::uint64_t kInvalidOffset = std::numeric_limits<std::uint64_t>::max();

void PutU16(std::ostream& out, std::uint16_t value) {
    const char bytes[2] = {static_cast<char>(value & 0xffu),
                           static_cast<char>((value >> 8) & 0xffu)};
    out.write(bytes, 2);
}

void PutU32(std::ostream& out, std::uint32_t value) {
    const char bytes[4] = {static_cast<char>(value & 0xffu),
                           static_cast<char>((value >> 8) & 0xffu),
                           static_cast<char>((value >> 16) & 0xffu),
                           static_cast<char>((value >> 24) & 0xffu)};
    out.write(bytes, 4);
}

std::uint16_t GetU16(const std::uint8_t* data) noexcept {
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[0]) |
                                      (static_cast<std::uint16_t>(data[1]) << 8));
}

std::uint32_t GetU32(const std::uint8_t* data) noexcept {
    return static_cast<std::uint32_t>(data[0]) |
           (static_cast<std::uint32_t>(data[1]) << 8) |
           (static_cast<std::uint32_t>(data[2]) << 16) |
           (static_cast<std::uint32_t>(data[3]) << 24);
}

bool ReadExact(std::istream& in, void* data, std::size_t size) {
    if (size == 0) {
        return true;
    }
    in.read(reinterpret_cast<char*>(data), static_cast<std::streamsize>(size));
    return static_cast<std::size_t>(in.gcount()) == size;
}

std::uint64_t Tell(std::ostream& out) noexcept {
    const std::streampos pos = out.tellp();
    if (pos == std::streampos(-1)) {
        return kInvalidOffset;
    }
    return static_cast<std::uint64_t>(pos);
}

// ---------------------------------------------------------------------------
// DOS 时间
// ---------------------------------------------------------------------------

struct DosTime final {
    std::uint16_t time{0};    ///< 时/分/秒（秒按 2 秒粒度）
    std::uint16_t date{0x21}; ///< 1980-01-01
};

DosTime MakeDosTime() noexcept {
    const std::time_t now = std::time(nullptr);
    if (now == static_cast<std::time_t>(-1)) {
        return DosTime{};
    }
    const std::tm* raw = std::gmtime(&now);
    if (raw == nullptr) {
        return DosTime{};
    }
    std::tm tm = *raw;
    int year = tm.tm_year + 1900;
    if (year < 1980) {
        year = 1980;
        tm.tm_mon = 0;
        tm.tm_mday = 1;
        tm.tm_hour = 0;
        tm.tm_min = 0;
        tm.tm_sec = 0;
    } else if (year > 2107) {
        year = 2107;
        tm.tm_mon = 11;
        tm.tm_mday = 31;
        tm.tm_hour = 23;
        tm.tm_min = 59;
        tm.tm_sec = 58;
    }
    DosTime result;
    result.time = static_cast<std::uint16_t>((tm.tm_hour << 11) | (tm.tm_min << 5) |
                                             (tm.tm_sec / 2));
    result.date = static_cast<std::uint16_t>(((year - 1980) << 9) | ((tm.tm_mon + 1) << 5) |
                                             tm.tm_mday);
    return result;
}

// ---------------------------------------------------------------------------
// 名字/路径校验
// ---------------------------------------------------------------------------

/**
 * @brief 校验归档内名字。
 * @param allow_trailing_slash true 时允许一个末尾 `/`（目录条目）。
 *
 * @details 拒绝空名、绝对路径、空段、`.`/`..` 段、反斜杠、冒号、
 *          控制字符与超过 u16 的长度。名字按 UTF-8 字节原样保留。
 */
bool IsValidEntryName(std::string_view name, bool allow_trailing_slash) noexcept {
    if (name.empty() || name.size() > 0xffffu) {
        return false;
    }
    if (name.front() == '/') {
        return false;
    }
    std::string_view body = name;
    if (body.back() == '/') {
        if (!allow_trailing_slash) {
            return false;
        }
        body.remove_suffix(1);
    }
    if (body.empty()) {
        return false;
    }
    std::size_t start = 0;
    while (start <= body.size()) {
        const std::size_t slash = body.find('/', start);
        const std::size_t end = slash == std::string_view::npos ? body.size() : slash;
        const std::string_view segment = body.substr(start, end - start);
        if (segment.empty() || segment == "." || segment == "..") {
            return false;
        }
        for (const char ch : segment) {
            const auto byte = static_cast<unsigned char>(ch);
            if (byte < 0x20u || byte == 0x7fu || ch == '\\' || ch == ':') {
                return false;
            }
        }
        if (slash == std::string_view::npos) {
            break;
        }
        start = slash + 1;
    }
    return true;
}

/// 规范化目录前缀：去掉末尾 `/`，其余按条目名校验。空前缀合法。
bool NormalizePrefix(const std::string& prefix, std::string* out) {
    if (prefix.empty()) {
        out->clear();
        return true;
    }
    std::string value = prefix;
    while (!value.empty() && value.back() == '/') {
        value.pop_back();
    }
    if (value.empty() || !IsValidEntryName(value, false)) {
        return false;
    }
    *out = std::move(value);
    return true;
}

/// 把 prefix 与相对路径拼成条目名（prefix 为空时直接返回相对路径）。
std::string JoinEntryName(const std::string& prefix, const std::string& relative) {
    if (prefix.empty()) {
        return relative;
    }
    return prefix + "/" + relative;
}

/// 绝对化并规范化路径，用于排除比较。
std::string AbsoluteNormalized(const std::string& path) {
    std::error_code ec;
    const fs::path absolute = fs::absolute(path, ec);
    if (ec) {
        return path;
    }
    return absolute.lexically_normal().string();
}

/// 判断 candidate 是否位于 root 之下（两者都应是规范化绝对路径）。
bool IsWithinRoot(const fs::path& root, const fs::path& candidate) {
    const fs::path relative = candidate.lexically_relative(root);
    if (relative.empty()) {
        return false;
    }
    if (relative == ".") {
        return true;
    }
    const auto first = relative.begin();
    return first != relative.end() && first->string() != "..";
}

/// 校验上限配置。
bool ValidateLimits(const ZipLimits& limits, std::string* error) {
    if (limits.max_entries == 0 || limits.max_entries > kZipMaxEntriesCeiling) {
        if (error != nullptr) {
            *error = "max_entries 越界";
        }
        return false;
    }
    if (limits.max_entry_bytes == 0 || limits.max_entry_bytes > kZipMaxBytesCeiling) {
        if (error != nullptr) {
            *error = "max_entry_bytes 越界";
        }
        return false;
    }
    if (limits.max_total_bytes == 0 || limits.max_total_bytes > kZipMaxBytesCeiling) {
        if (error != nullptr) {
            *error = "max_total_bytes 越界";
        }
        return false;
    }
    return true;
}

/// 创建一个父目录（已存在不算错误）。
bool EnsureParentDirectory(const fs::path& path, std::string* error) {
    const fs::path parent = path.parent_path();
    if (parent.empty()) {
        return true;
    }
    std::error_code ec;
    fs::create_directories(parent, ec);
    if (ec && !fs::is_directory(parent, ec)) {
        if (error != nullptr) {
            *error = "无法创建父目录";
        }
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// 共享读取结构
// ---------------------------------------------------------------------------

struct CentralEntry final {
    std::string name;                 ///< 条目名。
    std::uint32_t crc32{0};           ///< 声明的 CRC。
    std::uint32_t compressed_size{0}; ///< 声明的大小（store 下等于未压缩大小）。
    std::uint32_t uncompressed_size{0};
    std::uint64_t data_offset{0};     ///< 本地头之后的数据偏移。
    bool is_directory{false};         ///< 名字以 `/` 结尾。
};

struct ParsedArchive final {
    std::ifstream file;               ///< 打开的归档。
    std::vector<CentralEntry> entries;///< 中央目录内容。
    std::uint64_t archive_bytes{0};   ///< 归档文件总字节数。
};

/// 写入一条本地文件头。
void WriteLocalHeader(std::ostream& out, const std::string& name, std::uint32_t crc,
                      std::uint32_t size, const DosTime& dos_time) {
    PutU32(out, kLocalSignature);
    PutU16(out, kVersionStore);
    PutU16(out, kFlagUtf8);
    PutU16(out, 0); // method = store
    PutU16(out, dos_time.time);
    PutU16(out, dos_time.date);
    PutU32(out, crc);
    PutU32(out, size);
    PutU32(out, size);
    PutU16(out, static_cast<std::uint16_t>(name.size()));
    PutU16(out, 0); // extra field 长度
    out.write(name.data(), static_cast<std::streamsize>(name.size()));
}

/// 写入一条中央目录项。
void WriteCentralHeader(std::ostream& out, const std::string& name, std::uint32_t crc,
                        std::uint32_t size, std::uint32_t local_offset, bool directory,
                        const DosTime& dos_time) {
    PutU32(out, kCentralSignature);
    PutU16(out, kVersionMadeByUnix);
    PutU16(out, kVersionStore);
    PutU16(out, kFlagUtf8);
    PutU16(out, 0); // method = store
    PutU16(out, dos_time.time);
    PutU16(out, dos_time.date);
    PutU32(out, crc);
    PutU32(out, size);
    PutU32(out, size);
    PutU16(out, static_cast<std::uint16_t>(name.size()));
    PutU16(out, 0); // extra
    PutU16(out, 0); // comment
    PutU16(out, 0); // disk start
    PutU16(out, 0); // internal attributes
    PutU32(out, directory ? kExternalDirAttrs : kExternalFileAttrs);
    PutU32(out, local_offset);
    out.write(name.data(), static_cast<std::streamsize>(name.size()));
}

} // namespace

// ---------------------------------------------------------------------------
// ZipWriter::Impl
// ---------------------------------------------------------------------------

struct ZipWriter::Impl final {
    /// 一条待写入中央目录的记录。
    struct EntryRecord final {
        std::string name;
        std::uint32_t crc32{0};
        std::uint32_t size{0};
        std::uint32_t local_offset{0};
        DosTime dos_time;
        bool is_directory{false};
    };

    /// 数据写入回调：写字节到 out，并同步更新 CRC。
    using Filler = std::function<bool(std::ostream&, go2cpp::hash::Crc32&)>;

    std::ofstream out;                          ///< 输出流。
    std::string output_path;                    ///< 目标路径（Abort 时删除）。
    ZipLimits limits;                           ///< 生效上限。
    ZipStats stats;                             ///< 统计。
    std::vector<EntryRecord> records;           ///< 已写条目。
    std::unordered_set<std::string> names;      ///< 已用名字，防重复。
    std::vector<std::string> exclusions;        ///< 递归排除的绝对路径。
    bool opened{false};                         ///< 是否已 Open。
    bool closed{false};                         ///< 是否已成功 Close。
    bool failed{false};                         ///< 是否进入失败态。

    /// 标记失败并返回状态。
    ZipStatus Fail(ZipStatus status) noexcept {
        failed = true;
        return status;
    }

    /// 检查实例是否可继续写入。
    ZipStatus CheckWritable() const noexcept {
        if (failed) {
            return ZipStatus::kIoError;
        }
        if (closed) {
            return ZipStatus::kAlreadyClosed;
        }
        if (!opened) {
            return ZipStatus::kNotOpen;
        }
        return ZipStatus::kOk;
    }

    /// 是否是被排除的输出文件。
    bool IsExcluded(const fs::path& path) const {
        if (exclusions.empty()) {
            return false;
        }
        const std::string absolute = AbsoluteNormalized(path.string());
        return std::find(exclusions.begin(), exclusions.end(), absolute) != exclusions.end();
    }

    /// 写一个条目（文件数据来自 filler；目录用 AddDirectoryEntry）。
    ZipStatus AddEntry(const std::string& name, std::uint64_t size, const Filler& filler) {
        if (!IsValidEntryName(name, false)) {
            return Fail(ZipStatus::kPathRejected);
        }
        if (names.count(name) != 0) {
            return Fail(ZipStatus::kInvalidArgument);
        }
        if (records.size() >= limits.max_entries) {
            return Fail(ZipStatus::kTooManyEntries);
        }
        if (size > limits.max_entry_bytes || size > kZipMaxBytesCeiling) {
            return Fail(ZipStatus::kEntryTooLarge);
        }
        if (size > limits.max_total_bytes ||
            stats.input_bytes > limits.max_total_bytes - size) {
            return Fail(ZipStatus::kArchiveTooLarge);
        }
        const std::uint64_t offset = Tell(out);
        if (offset == kInvalidOffset || offset > kZipMaxBytesCeiling ||
            offset + kLocalHeaderBytes + name.size() + size > kZipMaxBytesCeiling) {
            return Fail(ZipStatus::kArchiveTooLarge);
        }
        const DosTime dos_time = MakeDosTime();
        WriteLocalHeader(out, name, 0, 0, dos_time);
        if (!out) {
            return Fail(ZipStatus::kIoError);
        }
        // 数据起点可以由「条目头偏移 + 固定头长 + 名字长」直接算出，
        // 省掉一次 tellp（每次 tellp 在 ofstream 上都是一次 lseek）。
        const std::uint64_t data_start = offset + kLocalHeaderBytes + name.size();
        go2cpp::hash::Crc32 crc;
        if (!filler(out, crc)) {
            return Fail(ZipStatus::kIoError);
        }
        const std::uint64_t data_end = Tell(out);
        if (data_end == kInvalidOffset || data_end < data_start ||
            data_end - data_start != size) {
            return Fail(ZipStatus::kIoError);
        }
        const auto size32 = static_cast<std::uint32_t>(size);
        out.seekp(static_cast<std::streamoff>(offset + 14));
        PutU32(out, crc.Sum32());
        PutU32(out, size32);
        PutU32(out, size32);
        out.seekp(static_cast<std::streamoff>(data_end));
        if (!out) {
            return Fail(ZipStatus::kIoError);
        }
        records.push_back(EntryRecord{name, crc.Sum32(), size32,
                                      static_cast<std::uint32_t>(offset), dos_time, false});
        names.insert(name);
        ++stats.entries;
        ++stats.files;
        stats.input_bytes += size;
        return ZipStatus::kOk;
    }

    /// 写入一个已知大小的普通文件。
    /// @details 递归遍历阶段已经 stat 过类型与大小，这里不再重复 lstat，
    ///          直接按 size 精确读满（也避免多一次读到 EOF 的 read 调用）。
    ZipStatus AddFileFromPath(const std::string& source_path, const std::string& entry_name,
                              std::uint64_t size) {
        std::ifstream in(source_path, std::ios::binary);
        if (!in) {
            return Fail(ZipStatus::kInputNotFound);
        }
        return AddEntry(entry_name, size,
                        [&in, size](std::ostream& out, go2cpp::hash::Crc32& crc) {
                            std::array<char, 64 * 1024> buffer{};
                            std::uint64_t remaining = size;
                            while (remaining > 0) {
                                const std::size_t want = static_cast<std::size_t>(
                                    std::min<std::uint64_t>(remaining, buffer.size()));
                                in.read(buffer.data(),
                                        static_cast<std::streamsize>(want));
                                const std::streamsize got = in.gcount();
                                if (got <= 0) {
                                    return false;
                                }
                                crc.Write(go2cpp::hash::AsBytes(
                                    buffer.data(), static_cast<std::size_t>(got)));
                                out.write(buffer.data(), got);
                                if (!out) {
                                    return false;
                                }
                                remaining -= static_cast<std::uint64_t>(got);
                            }
                            return true;
                        });
    }

    /// 写一个目录条目（name 必须以 `/` 结尾）。
    ZipStatus AddDirectoryEntry(const std::string& name) {
        if (!IsValidEntryName(name, true) || name.back() != '/') {
            return Fail(ZipStatus::kPathRejected);
        }
        if (names.count(name) != 0) {
            return Fail(ZipStatus::kInvalidArgument);
        }
        if (records.size() >= limits.max_entries) {
            return Fail(ZipStatus::kTooManyEntries);
        }
        const std::uint64_t offset = Tell(out);
        if (offset == kInvalidOffset || offset > kZipMaxBytesCeiling ||
            offset + kLocalHeaderBytes + name.size() > kZipMaxBytesCeiling) {
            return Fail(ZipStatus::kArchiveTooLarge);
        }
        const DosTime dos_time = MakeDosTime();
        WriteLocalHeader(out, name, 0, 0, dos_time);
        if (!out) {
            return Fail(ZipStatus::kIoError);
        }
        records.push_back(EntryRecord{name, 0, 0, static_cast<std::uint32_t>(offset), dos_time,
                                      true});
        names.insert(name);
        ++stats.entries;
        ++stats.directories;
        return ZipStatus::kOk;
    }

    /// 清理全部状态（不删除文件）。
    void Reset() noexcept {
        out.close();
        out.clear();
        output_path.clear();
        limits = ZipLimits{};
        stats = ZipStats{};
        records.clear();
        names.clear();
        exclusions.clear();
        opened = false;
        closed = false;
        failed = false;
    }
};

// ---------------------------------------------------------------------------
// ZipWriter
// ---------------------------------------------------------------------------

ZipWriter::ZipWriter() : m_impl(std::make_unique<Impl>()) {}
ZipWriter::~ZipWriter() {
    if (m_impl != nullptr && m_impl->opened) {
        Abort();
    }
}

ZipStatus ZipWriter::Open(const std::string& output_path, const ZipLimits& limits) {
    if (m_impl->opened) {
        Abort();
    } else {
        m_impl->Reset();
    }
    std::string error;
    if (!ValidateLimits(limits, &error)) {
        return ZipStatus::kInvalidArgument;
    }
    if (output_path.empty()) {
        return ZipStatus::kInvalidArgument;
    }
    if (!EnsureParentDirectory(output_path, &error)) {
        return ZipStatus::kIoError;
    }
    m_impl->out.open(output_path, std::ios::binary | std::ios::trunc);
    if (!m_impl->out) {
        return ZipStatus::kIoError;
    }
    m_impl->output_path = output_path;
    m_impl->limits = limits;
    m_impl->opened = true;
    m_impl->closed = false;
    m_impl->failed = false;
    return ZipStatus::kOk;
}

ZipStatus ZipWriter::AddFile(const std::string& source_path, const std::string& entry_name) {
    const ZipStatus writable = m_impl->CheckWritable();
    if (writable != ZipStatus::kOk) {
        return writable;
    }
    std::error_code ec;
    const fs::file_status status = fs::symlink_status(source_path, ec);
    if (ec || status.type() == fs::file_type::not_found) {
        return m_impl->Fail(ZipStatus::kInputNotFound);
    }
    if (status.type() != fs::file_type::regular) {
        return m_impl->Fail(ZipStatus::kPathRejected);
    }
    const std::uintmax_t raw_size = fs::file_size(source_path, ec);
    if (ec) {
        return m_impl->Fail(ZipStatus::kIoError);
    }
    return m_impl->AddFileFromPath(source_path, entry_name,
                                   static_cast<std::uint64_t>(raw_size));
}

ZipStatus ZipWriter::AddBytes(const std::string& entry_name, const std::uint8_t* data,
                              std::size_t size) {
    const ZipStatus writable = m_impl->CheckWritable();
    if (writable != ZipStatus::kOk) {
        return writable;
    }
    if (size > 0 && data == nullptr) {
        return m_impl->Fail(ZipStatus::kInvalidArgument);
    }
    return m_impl->AddEntry(entry_name, static_cast<std::uint64_t>(size),
                            [data, size](std::ostream& out, go2cpp::hash::Crc32& crc) {
                                if (size > 0) {
                                    crc.Write(go2cpp::hash::AsBytes(data, size));
                                    out.write(reinterpret_cast<const char*>(data),
                                              static_cast<std::streamsize>(size));
                                }
                                return static_cast<bool>(out);
                            });
}

ZipStatus ZipWriter::AddDirectory(const std::string& source_dir,
                                  const std::string& entry_prefix) {
    const ZipStatus writable = m_impl->CheckWritable();
    if (writable != ZipStatus::kOk) {
        return writable;
    }
    std::string prefix;
    if (!NormalizePrefix(entry_prefix, &prefix)) {
        return m_impl->Fail(ZipStatus::kPathRejected);
    }
    std::error_code ec;
    const fs::file_status root_status = fs::symlink_status(source_dir, ec);
    if (ec || root_status.type() == fs::file_type::not_found) {
        return m_impl->Fail(ZipStatus::kInputNotFound);
    }
    if (root_status.type() != fs::file_type::directory) {
        return m_impl->Fail(ZipStatus::kPathRejected);
    }
    // 绝对化根目录：recursive_directory_iterator 以 root 为前缀生成
    // entry.path()，绝对化后可以用纯字符串的 lexically_relative 剥离前缀。
    // 不能用 fs::relative —— 它内部走 weakly_canonical，对每个路径段调用
    // readlink；实测 2 万个小文件触发 16 万次失败的 readlink，占打包时间 26%。
    const fs::path root = [&source_dir]() {
        std::error_code absolute_ec;
        const fs::path absolute = fs::absolute(source_dir, absolute_ec);
        return absolute_ec ? fs::path(source_dir) : absolute.lexically_normal();
    }();
    // 遍历阶段一次性记下类型与大小，写入阶段不再重复 stat。
    struct PendingEntry final {
        std::string relative;
        std::uintmax_t size{0};
        bool is_directory{false};
    };
    std::vector<PendingEntry> pending;
    fs::recursive_directory_iterator it(root, fs::directory_options::none, ec);
    if (ec) {
        return m_impl->Fail(ZipStatus::kIoError);
    }
    const fs::recursive_directory_iterator end;
    for (; it != end; it.increment(ec)) {
        if (ec) {
            return m_impl->Fail(ZipStatus::kIoError);
        }
        const fs::directory_entry& entry = *it;
        std::error_code status_ec;
        const fs::file_status status = entry.symlink_status(status_ec);
        if (status_ec) {
            return m_impl->Fail(ZipStatus::kIoError);
        }
        if (status.type() == fs::file_type::symlink) {
            ++m_impl->stats.symlinks_skipped;
            it.disable_recursion_pending();
            continue;
        }
        if (status.type() != fs::file_type::directory &&
            status.type() != fs::file_type::regular) {
            ++m_impl->stats.special_skipped;
            it.disable_recursion_pending();
            continue;
        }
        if (m_impl->IsExcluded(entry.path())) {
            continue;
        }
        const fs::path relative = entry.path().lexically_relative(root);
        if (relative.empty() || *relative.begin() == "..") {
            return m_impl->Fail(ZipStatus::kIoError);
        }
        PendingEntry item;
        item.relative = relative.generic_string();
        if (status.type() == fs::file_type::directory) {
            item.is_directory = true;
        } else {
            std::error_code size_ec;
            item.size = fs::file_size(entry.path(), size_ec);
            if (size_ec) {
                return m_impl->Fail(ZipStatus::kIoError);
            }
        }
        pending.push_back(std::move(item));
    }
    std::sort(pending.begin(), pending.end(),
              [](const PendingEntry& left, const PendingEntry& right) {
                  return left.relative < right.relative;
              });
    const std::size_t root_entries = prefix.empty() ? 0u : 1u;
    if (m_impl->records.size() + pending.size() + root_entries >
        m_impl->limits.max_entries) {
        return m_impl->Fail(ZipStatus::kTooManyEntries);
    }
    if (!prefix.empty()) {
        const ZipStatus status = m_impl->AddDirectoryEntry(prefix + "/");
        if (status != ZipStatus::kOk) {
            return status;
        }
    }
    for (const PendingEntry& item : pending) {
        const std::string name = JoinEntryName(prefix, item.relative);
        if (item.is_directory) {
            const ZipStatus added = m_impl->AddDirectoryEntry(name + "/");
            if (added != ZipStatus::kOk) {
                return added;
            }
            continue;
        }
        const ZipStatus added =
            m_impl->AddFileFromPath((root / item.relative).string(), name, item.size);
        if (added != ZipStatus::kOk) {
            return added;
        }
    }
    return ZipStatus::kOk;
}

ZipStatus ZipWriter::AddExclusion(const std::string& path) {
    const ZipStatus writable = m_impl->CheckWritable();
    if (writable != ZipStatus::kOk) {
        return writable;
    }
    if (path.empty()) {
        return ZipStatus::kInvalidArgument;
    }
    m_impl->exclusions.push_back(AbsoluteNormalized(path));
    return ZipStatus::kOk;
}

ZipStatus ZipWriter::Close() {
    if (!m_impl->opened) {
        return m_impl->closed ? ZipStatus::kAlreadyClosed : ZipStatus::kNotOpen;
    }
    if (m_impl->failed) {
        Abort();
        return ZipStatus::kIoError;
    }
    if (m_impl->records.size() > kZipMaxEntriesCeiling) {
        Abort();
        return ZipStatus::kTooManyEntries;
    }
    const std::uint64_t central_offset = Tell(m_impl->out);
    if (central_offset == kInvalidOffset || central_offset > kZipMaxBytesCeiling) {
        Abort();
        return ZipStatus::kArchiveTooLarge;
    }
    std::uint64_t central_bytes = 0;
    for (const Impl::EntryRecord& record : m_impl->records) {
        WriteCentralHeader(m_impl->out, record.name, record.crc32, record.size,
                           record.local_offset, record.is_directory, record.dos_time);
        central_bytes += kCentralHeaderBytes + record.name.size();
    }
    const std::uint64_t central_end = Tell(m_impl->out);
    if (central_end == kInvalidOffset || central_end > kZipMaxBytesCeiling ||
        central_offset + central_bytes > kZipMaxBytesCeiling) {
        Abort();
        return ZipStatus::kArchiveTooLarge;
    }
    PutU32(m_impl->out, kEocdSignature);
    PutU16(m_impl->out, 0); // 当前磁盘号
    PutU16(m_impl->out, 0); // 中央目录起始磁盘号
    PutU16(m_impl->out, static_cast<std::uint16_t>(m_impl->records.size()));
    PutU16(m_impl->out, static_cast<std::uint16_t>(m_impl->records.size()));
    PutU32(m_impl->out, static_cast<std::uint32_t>(central_bytes));
    PutU32(m_impl->out, static_cast<std::uint32_t>(central_offset));
    PutU16(m_impl->out, 0); // 注释长度
    m_impl->out.flush();
    const bool write_ok = static_cast<bool>(m_impl->out);
    m_impl->out.close();
    if (!write_ok || !m_impl->out) {
        Abort();
        return ZipStatus::kIoError;
    }
    m_impl->stats.archive_bytes = central_end + kEocdBytes;
    m_impl->opened = false;
    m_impl->closed = true;
    return ZipStatus::kOk;
}

void ZipWriter::Abort() noexcept {
    if (m_impl == nullptr) {
        return;
    }
    if (m_impl->out.is_open()) {
        m_impl->out.close();
    }
    if (m_impl->opened && !m_impl->closed && !m_impl->output_path.empty()) {
        std::error_code ec;
        fs::remove(m_impl->output_path, ec);
    }
    m_impl->Reset();
}

bool ZipWriter::opened() const noexcept { return m_impl->opened; }
bool ZipWriter::closed() const noexcept { return m_impl->closed; }
const ZipStats& ZipWriter::stats() const noexcept { return m_impl->stats; }
const ZipLimits& ZipWriter::limits() const noexcept { return m_impl->limits; }

const char* ZipStatusToString(ZipStatus status) noexcept {
    switch (status) {
        case ZipStatus::kOk:
            return "ok";
        case ZipStatus::kInvalidArgument:
            return "invalid_argument";
        case ZipStatus::kInputNotFound:
            return "input_not_found";
        case ZipStatus::kIoError:
            return "io_error";
        case ZipStatus::kTooManyEntries:
            return "too_many_entries";
        case ZipStatus::kEntryTooLarge:
            return "entry_too_large";
        case ZipStatus::kArchiveTooLarge:
            return "archive_too_large";
        case ZipStatus::kUnsupportedCompression:
            return "unsupported_compression";
        case ZipStatus::kCorruptArchive:
            return "corrupt_archive";
        case ZipStatus::kPathRejected:
            return "path_rejected";
        case ZipStatus::kNotOpen:
            return "not_open";
        case ZipStatus::kAlreadyClosed:
            return "already_closed";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// 读取器
// ---------------------------------------------------------------------------

namespace {

ZipStatus ReadFail(ZipStatus status, std::string* error, const char* message) {
    if (error != nullptr) {
        *error = message;
    }
    return status;
}

/**
 * @brief 解析 EOCD、中央目录与每个条目的本地头。
 *
 * @details 所有长度/偏移都做边界检查，中央目录一次性读入内存（上限
 *          64MiB）。成功时 `archive->file` 保持打开，调用方可以继续
 *          seek 读取数据。
 */
ZipStatus ParseArchive(const std::string& zip_path, const ZipLimits& limits,
                       ParsedArchive* archive, std::string* error) {
    std::string limit_error;
    if (!ValidateLimits(limits, &limit_error)) {
        return ReadFail(ZipStatus::kInvalidArgument, error, "limits 非法");
    }
    if (archive == nullptr || zip_path.empty()) {
        return ReadFail(ZipStatus::kInvalidArgument, error, "zip 路径为空");
    }
    archive->file.open(zip_path, std::ios::binary);
    if (!archive->file) {
        return ReadFail(ZipStatus::kInputNotFound, error, "无法打开 zip");
    }
    std::error_code ec;
    const std::uintmax_t raw_size = fs::file_size(zip_path, ec);
    if (ec) {
        return ReadFail(ZipStatus::kIoError, error, "无法读取 zip 大小");
    }
    archive->archive_bytes = static_cast<std::uint64_t>(raw_size);
    if (archive->archive_bytes < kEocdBytes) {
        return ReadFail(ZipStatus::kCorruptArchive, error, "文件短于 EOCD");
    }
    const std::uint64_t tail_size =
        std::min<std::uint64_t>(archive->archive_bytes, kEocdBytes + kMaxCommentBytes);
    std::vector<std::uint8_t> tail(static_cast<std::size_t>(tail_size));
    archive->file.seekg(static_cast<std::streamoff>(archive->archive_bytes - tail_size));
    if (!ReadExact(archive->file, tail.data(), tail.size())) {
        return ReadFail(ZipStatus::kIoError, error, "读取文件尾部失败");
    }
    std::size_t eocd_pos = tail.size();
    for (std::size_t i = tail.size(); i > 0;) {
        --i;
        if (i + kEocdBytes > tail.size()) {
            continue;
        }
        if (GetU32(tail.data() + i) == kEocdSignature &&
            i + kEocdBytes + GetU16(tail.data() + i + 20) == tail.size()) {
            eocd_pos = i;
            break;
        }
    }
    if (eocd_pos == tail.size()) {
        return ReadFail(ZipStatus::kCorruptArchive, error, "找不到 EOCD");
    }
    const std::uint8_t* eocd = tail.data() + eocd_pos;
    const std::uint16_t disk = GetU16(eocd + 4);
    const std::uint16_t central_disk = GetU16(eocd + 6);
    const std::uint16_t entries_disk = GetU16(eocd + 8);
    const std::uint16_t entries_total = GetU16(eocd + 10);
    const std::uint32_t central_size = GetU32(eocd + 12);
    const std::uint32_t central_offset = GetU32(eocd + 16);
    const std::uint64_t eocd_absolute = archive->archive_bytes - tail_size + eocd_pos;
    if (disk != 0 || central_disk != 0 || entries_disk != entries_total) {
        return ReadFail(ZipStatus::kUnsupportedCompression, error, "不支持分卷 zip");
    }
    if (entries_total == 0xffffu || central_size == 0xffffffffu ||
        central_offset == 0xffffffffu) {
        return ReadFail(ZipStatus::kArchiveTooLarge, error, "不支持 ZIP64");
    }
    if (entries_total > limits.max_entries) {
        return ReadFail(ZipStatus::kTooManyEntries, error, "条目数超过上限");
    }
    if (central_size > kMaxCentralDirectoryBytes) {
        return ReadFail(ZipStatus::kArchiveTooLarge, error, "中央目录过大");
    }
    if (central_offset > eocd_absolute || central_size > eocd_absolute - central_offset) {
        return ReadFail(ZipStatus::kCorruptArchive, error, "中央目录越界");
    }
    if (static_cast<std::uint64_t>(entries_total) * kCentralHeaderBytes > central_size) {
        return ReadFail(ZipStatus::kCorruptArchive, error, "中央目录长度不足");
    }
    std::vector<std::uint8_t> central(central_size);
    archive->file.seekg(static_cast<std::streamoff>(central_offset));
    if (!ReadExact(archive->file, central.data(), central.size())) {
        return ReadFail(ZipStatus::kIoError, error, "读取中央目录失败");
    }
    std::unordered_set<std::string> seen;
    std::uint64_t total_bytes = 0;
    std::size_t pos = 0;
    for (std::uint32_t index = 0; index < entries_total; ++index) {
        if (pos > central.size() || kCentralHeaderBytes > central.size() - pos) {
            return ReadFail(ZipStatus::kCorruptArchive, error, "中央目录项截断");
        }
        const std::uint8_t* item = central.data() + pos;
        if (GetU32(item) != kCentralSignature) {
            return ReadFail(ZipStatus::kCorruptArchive, error, "中央目录签名错误");
        }
        const std::uint16_t flags = GetU16(item + 8);
        const std::uint16_t method = GetU16(item + 10);
        const std::uint32_t crc = GetU32(item + 16);
        const std::uint32_t compressed_size = GetU32(item + 20);
        const std::uint32_t uncompressed_size = GetU32(item + 24);
        const std::uint16_t name_len = GetU16(item + 28);
        const std::uint16_t extra_len = GetU16(item + 30);
        const std::uint16_t comment_len = GetU16(item + 32);
        const std::uint32_t local_offset = GetU32(item + 42);
        if (name_len == 0) {
            return ReadFail(ZipStatus::kCorruptArchive, error, "条目名为空");
        }
        const std::size_t record_bytes = kCentralHeaderBytes + static_cast<std::size_t>(name_len) +
                                         static_cast<std::size_t>(extra_len) +
                                         static_cast<std::size_t>(comment_len);
        if (record_bytes > central.size() - pos) {
            return ReadFail(ZipStatus::kCorruptArchive, error, "中央目录项越界");
        }
        if (method != 0) {
            return ReadFail(ZipStatus::kUnsupportedCompression, error, "条目不是 store");
        }
        if ((flags & (kFlagEncrypted | kFlagDataDescriptor)) != 0) {
            return ReadFail(ZipStatus::kUnsupportedCompression, error, "不支持加密/描述符条目");
        }
        if (compressed_size != uncompressed_size) {
            return ReadFail(ZipStatus::kCorruptArchive, error, "store 条目大小不一致");
        }
        std::string name(reinterpret_cast<const char*>(item + kCentralHeaderBytes), name_len);
        if (!IsValidEntryName(name, true)) {
            return ReadFail(ZipStatus::kPathRejected, error, "条目名未通过路径校验");
        }
        if (!seen.insert(name).second) {
            return ReadFail(ZipStatus::kCorruptArchive, error, "条目名重复");
        }
        if (uncompressed_size > limits.max_entry_bytes) {
            return ReadFail(ZipStatus::kEntryTooLarge, error, "条目超过单条上限");
        }
        if (uncompressed_size > limits.max_total_bytes ||
            total_bytes > limits.max_total_bytes - uncompressed_size) {
            return ReadFail(ZipStatus::kArchiveTooLarge, error, "解压总量超过上限");
        }
        total_bytes += uncompressed_size;
        if (local_offset > archive->archive_bytes ||
            archive->archive_bytes - local_offset < kLocalHeaderBytes) {
            return ReadFail(ZipStatus::kCorruptArchive, error, "本地头偏移越界");
        }
        std::uint8_t local[kLocalHeaderBytes]{};
        archive->file.seekg(static_cast<std::streamoff>(local_offset));
        if (!ReadExact(archive->file, local, sizeof(local))) {
            return ReadFail(ZipStatus::kIoError, error, "读取本地头失败");
        }
        if (GetU32(local) != kLocalSignature) {
            return ReadFail(ZipStatus::kCorruptArchive, error, "本地头签名错误");
        }
        if (GetU16(local + 8) != 0) {
            return ReadFail(ZipStatus::kUnsupportedCompression, error, "本地头 method 非 store");
        }
        const std::uint16_t local_name_len = GetU16(local + 26);
        const std::uint16_t local_extra_len = GetU16(local + 28);
        const std::uint64_t data_offset = static_cast<std::uint64_t>(local_offset) +
                                          kLocalHeaderBytes + local_name_len + local_extra_len;
        if (data_offset > archive->archive_bytes ||
            compressed_size > archive->archive_bytes - data_offset) {
            return ReadFail(ZipStatus::kCorruptArchive, error, "条目数据越界");
        }
        if (GetU32(local + 14) != crc || GetU32(local + 18) != compressed_size ||
            GetU32(local + 22) != uncompressed_size) {
            return ReadFail(ZipStatus::kCorruptArchive, error, "本地头字段与中央目录不一致");
        }
        CentralEntry parsed;
        parsed.name = std::move(name);
        parsed.crc32 = crc;
        parsed.compressed_size = compressed_size;
        parsed.uncompressed_size = uncompressed_size;
        parsed.data_offset = data_offset;
        parsed.is_directory = !parsed.name.empty() && parsed.name.back() == '/';
        archive->entries.push_back(std::move(parsed));
        pos += record_bytes;
    }
    if (pos != central.size()) {
        return ReadFail(ZipStatus::kCorruptArchive, error, "中央目录长度与条目不符");
    }
    return ZipStatus::kOk;
}

/**
 * @brief 把条目名解析为输出根下的安全路径。
 *
 * @details 除了语法校验，还会对父目录做 `weakly_canonical` 并确认其仍在
 *          root 内，防止输出目录内预先存在的符号链接把写入引到外部。
 */
bool ResolveOutputPath(const fs::path& root, const std::string& name, bool is_directory,
                       fs::path* target, std::string* error) {
    std::string trimmed = name;
    if (is_directory && !trimmed.empty() && trimmed.back() == '/') {
        trimmed.pop_back();
    }
    if (trimmed.empty()) {
        *error = "空条目名";
        return false;
    }
    const fs::path relative(trimmed);
    if (relative.is_absolute()) {
        *error = "绝对路径";
        return false;
    }
    for (const fs::path& part : relative) {
        const std::string segment = part.string();
        if (segment.empty() || segment == "." || segment == "..") {
            *error = "非法路径段";
            return false;
        }
    }
    const fs::path candidate = root / relative;
    std::error_code ec;
    const fs::path parent = fs::weakly_canonical(candidate.parent_path(), ec);
    if (ec || !IsWithinRoot(root, parent)) {
        *error = "路径逃逸输出根";
        return false;
    }
    if (fs::is_symlink(candidate, ec)) {
        *error = "目标已存在且是符号链接";
        return false;
    }
    if (is_directory) {
        if (fs::exists(candidate, ec) && !fs::is_directory(candidate, ec)) {
            *error = "目录条目与已有文件冲突";
            return false;
        }
    } else if (fs::is_directory(candidate, ec)) {
        *error = "文件条目与已有目录冲突";
        return false;
    }
    *target = candidate;
    return true;
}

} // namespace

ZipListing ListZip(const std::string& zip_path, const ZipLimits& limits) {
    ZipListing listing;
    ParsedArchive archive;
    listing.status = ParseArchive(zip_path, limits, &archive, &listing.error);
    if (listing.status != ZipStatus::kOk) {
        return listing;
    }
    listing.entries.reserve(archive.entries.size());
    for (const CentralEntry& entry : archive.entries) {
        ZipEntryInfo info;
        info.name = entry.name;
        info.size = entry.uncompressed_size;
        info.crc32 = entry.crc32;
        info.is_directory = entry.is_directory;
        listing.entries.push_back(std::move(info));
        if (entry.is_directory) {
            ++listing.stats.directories;
        } else {
            ++listing.stats.files;
        }
        listing.stats.input_bytes += entry.uncompressed_size;
    }
    listing.stats.entries = archive.entries.size();
    listing.stats.archive_bytes = archive.archive_bytes;
    return listing;
}

ZipResult ExtractZip(const std::string& zip_path, const std::string& output_dir,
                     const ZipLimits& limits) {
    ZipResult result;
    if (output_dir.empty()) {
        result.status = ZipStatus::kInvalidArgument;
        result.error = "输出目录为空";
        return result;
    }
    ParsedArchive archive;
    result.status = ParseArchive(zip_path, limits, &archive, &result.error);
    if (result.status != ZipStatus::kOk) {
        return result;
    }
    std::error_code ec;
    fs::create_directories(output_dir, ec);
    if (ec && !fs::is_directory(output_dir)) {
        result.status = ZipStatus::kIoError;
        result.error = "无法创建输出目录";
        return result;
    }
    fs::path root = fs::weakly_canonical(output_dir, ec);
    if (ec) {
        root = fs::absolute(output_dir, ec);
        if (ec) {
            result.status = ZipStatus::kIoError;
            result.error = "无法解析输出目录";
            return result;
        }
    }
    result.stats.archive_bytes = archive.archive_bytes;
    std::array<char, 64 * 1024> buffer{};
    for (const CentralEntry& entry : archive.entries) {
        fs::path target;
        if (!ResolveOutputPath(root, entry.name, entry.is_directory, &target, &result.error)) {
            result.status = ZipStatus::kPathRejected;
            return result;
        }
        if (entry.is_directory) {
            std::error_code dir_ec;
            fs::create_directories(target, dir_ec);
            if (dir_ec && !fs::is_directory(target)) {
                result.status = ZipStatus::kIoError;
                result.error = "创建目录失败";
                return result;
            }
            ++result.stats.directories;
            ++result.stats.entries;
            continue;
        }
        if (!EnsureParentDirectory(target, &result.error)) {
            result.status = ZipStatus::kIoError;
            return result;
        }
        archive.file.clear();
        archive.file.seekg(static_cast<std::streamoff>(entry.data_offset));
        if (!archive.file) {
            result.status = ZipStatus::kIoError;
            result.error = "无法定位条目数据";
            return result;
        }
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        if (!out) {
            result.status = ZipStatus::kIoError;
            result.error = "无法创建输出文件";
            return result;
        }
        go2cpp::hash::Crc32 crc;
        std::uint64_t remaining = entry.compressed_size;
        bool io_ok = true;
        while (remaining > 0) {
            const std::size_t want = static_cast<std::size_t>(
                std::min<std::uint64_t>(remaining, buffer.size()));
            archive.file.read(buffer.data(), static_cast<std::streamsize>(want));
            if (static_cast<std::size_t>(archive.file.gcount()) != want) {
                io_ok = false;
                break;
            }
            crc.Write(go2cpp::hash::AsBytes(buffer.data(), want));
            out.write(buffer.data(), static_cast<std::streamsize>(want));
            if (!out) {
                io_ok = false;
                break;
            }
            remaining -= want;
        }
        out.flush();
        const bool write_ok = static_cast<bool>(out);
        out.close();
        if (!io_ok || !write_ok || !out) {
            std::error_code remove_ec;
            fs::remove(target, remove_ec);
            result.status = ZipStatus::kIoError;
            result.error = "写出条目失败";
            return result;
        }
        if (crc.Sum32() != entry.crc32) {
            std::error_code remove_ec;
            fs::remove(target, remove_ec);
            result.status = ZipStatus::kCorruptArchive;
            result.error = "条目 CRC 校验失败";
            return result;
        }
        ++result.stats.files;
        ++result.stats.entries;
        result.stats.input_bytes += entry.uncompressed_size;
    }
    result.status = ZipStatus::kOk;
    return result;
}

} // namespace go2cpp::bento::app
