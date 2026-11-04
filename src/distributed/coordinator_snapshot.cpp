/**
 * @file coordinator_snapshot.cpp
 * @brief `Coordinator` 的快照持久化：fencing 表 + 租约表 + 选举 epoch。
 *
 * ## 为什么单独一个文件
 *
 * `coordinator.cpp` 是租约状态机（640 行，含三套索引），快照是它的**边界**
 * 能力：只读取状态、不改变语义。独立文件便于单独审阅格式与崩溃语义。
 *
 * ## 快照格式（版本 1，全部大端）
 *
 * ```
 * 偏移  长度  字段
 * 0     8     魔数 "G2CSNP01"
 * 8     2     格式版本（当前 1）
 * 10    2     保留标志（写 0，读取时忽略但计入 CRC）
 * 12    4     记录数 N
 * 16    ...   N 条记录（kind u8 + payload）
 * 末尾  4     CRC32（覆盖本文件除 CRC 外的全部字节）
 *
 * kind 1 = fencing：u32 key 长度 + key + u64 token
 * kind 2 = lease  ：u64 id + u8 kind + u64 ttl_ms + u64 fencing
 *                  + u64 deadline_unix_ms（绝对墙钟，仅供审计）
 *                  + 四个长度前缀字符串 name/instance/owner/endpoint
 * kind 3 = election：u32 name 长度 + name + u64 epoch
 * ```
 *
 * 租约表**会被序列化但不会在恢复时重建持有权**：重启后旧持有者一律视为
 * 失效（客户端重新抢锁），租约表只用于恢复 `m_next_lease` 水位，保证租约
 * 编号不复用、旧句柄不会误伤新租约。
 *
 * ## fsync 策略
 *
 * `snapshot_fsync=true`（默认）时：写 `<path>.tmp` → `fsync(file)` →
 * `rename(tmp, path)` → `fsync(目录)`。rename 保证原子替换（读者要么看到
 * 旧快照、要么看到新快照），两次 fsync 保证「函数返回成功 = 掉电后仍在」。
 * 关闭 fsync 可以把单次快照从 ~10µs 降到 ~2µs，代价是崩溃可能退回上一份
 * 快照（fencing 仍单调，但可能少推进一步）。
 *
 * ## 失败语义
 *
 * - 文件不存在：视为全新部署，`Restore()` 成功；
 * - 文件存在但任何一项校验失败（魔数/版本/CRC/长度/未知 kind）：**拒绝
 *   启动**，不猜测、不截断、不回退；
 * - 写失败：保留磁盘上的旧快照，`dirty` 置回，下次写重试。
 */

#include "go2cpp/distributed/coordinator.hpp"

#include "snapshot_codec.hpp"
#include "go2cpp/distributed/metrics.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <unordered_map>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace go2cpp::distributed {
namespace {

using detail::Crc32;
using detail::SnapshotReader;
using detail::SnapshotWriter;

constexpr char kSnapshotMagic[8] = {'G', '2', 'C', 'S', 'N', 'P', '0', '1'};
constexpr std::uint16_t kSnapshotVersion = 1;
constexpr std::size_t kSnapshotHeaderBytes = 16;
constexpr std::size_t kSnapshotCrcBytes = 4;
constexpr std::size_t kMaxSnapshotBytes = 64U << 20;
constexpr std::size_t kMaxStringBytes = 1U << 20;

constexpr std::uint8_t kRecordFencing = 1;
constexpr std::uint8_t kRecordLease = 2;
constexpr std::uint8_t kRecordElection = 3;

/// 解析后的快照内容：提交前先全部解析，失败时不影响内存状态。
struct ParsedSnapshot {
    std::unordered_map<std::string, std::uint64_t> fencing;
    std::unordered_map<std::string, std::uint64_t> epochs;
    std::uint64_t max_lease_id{0};
};

std::string DescribeErrno(const char *what) {
    return std::string(what) + ": " + std::strerror(errno);
}

std::string DirNameOf(const std::string &path) {
    const std::size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) {
        return ".";
    }
    if (slash == 0) {
        return "/";
    }
    return path.substr(0, slash);
}

enum class ReadStatus { kMissing, kOk, kError };

/// @brief 读整个文件；不存在返回 kMissing（不是错误）。
ReadStatus ReadWholeFile(const std::string &path, std::string *content,
                         std::string *error) {
    struct stat info {};
    if (::stat(path.c_str(), &info) != 0) {
        if (errno == ENOENT) {
            return ReadStatus::kMissing;
        }
        *error = DescribeErrno("stat snapshot");
        return ReadStatus::kError;
    }
    if (info.st_size < 0 || static_cast<std::uint64_t>(info.st_size) > kMaxSnapshotBytes) {
        *error = "snapshot size out of range";
        return ReadStatus::kError;
    }
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        *error = DescribeErrno("open snapshot");
        return ReadStatus::kError;
    }
    content->assign(static_cast<std::size_t>(info.st_size), '\0');
    std::size_t offset = 0;
    while (offset < content->size()) {
        const ssize_t got = ::read(fd, content->data() + offset, content->size() - offset);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            *error = DescribeErrno("read snapshot");
            ::close(fd);
            return ReadStatus::kError;
        }
        if (got == 0) {
            break;
        }
        offset += static_cast<std::size_t>(got);
    }
    ::close(fd);
    if (offset != content->size()) {
        *error = "short read on snapshot";
        return ReadStatus::kError;
    }
    return ReadStatus::kOk;
}

/// @brief 原子替换写：临时文件 + fsync + rename + fsync 目录。
bool WriteWholeFileAtomic(const std::string &path, const std::string &blob,
                          bool do_fsync, std::string *error) {
    const std::string tmp = path + ".tmp";
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        *error = DescribeErrno("open snapshot tmp");
        return false;
    }
    std::size_t written = 0;
    while (written < blob.size()) {
        const ssize_t result = ::write(fd, blob.data() + written, blob.size() - written);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            *error = DescribeErrno("write snapshot");
            ::close(fd);
            (void)::unlink(tmp.c_str());
            return false;
        }
        written += static_cast<std::size_t>(result);
    }
    if (do_fsync && ::fsync(fd) != 0) {
        *error = DescribeErrno("fsync snapshot");
        ::close(fd);
        (void)::unlink(tmp.c_str());
        return false;
    }
    if (::close(fd) != 0) {
        *error = DescribeErrno("close snapshot");
        (void)::unlink(tmp.c_str());
        return false;
    }
    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        *error = DescribeErrno("rename snapshot");
        (void)::unlink(tmp.c_str());
        return false;
    }
    if (do_fsync) {
        const std::string dir = DirNameOf(path);
        const int dir_fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
        if (dir_fd < 0) {
            // rename 已生效但目录项可能没落盘：必须报失败（调用方置回 dirty
            // 重试），否则「返回成功 = 掉电后仍在」的承诺失真。
            *error = DescribeErrno("open snapshot dir");
            return false;
        }
        const int sync_status = ::fsync(dir_fd);
        const int sync_errno = errno;
        ::close(dir_fd);
        if (sync_status != 0) {
            errno = sync_errno;
            *error = DescribeErrno("fsync snapshot dir");
            return false;
        }
    }
    return true;
}

/// @brief 解析快照；任何不合法输入都返回 false 并写 `error`。
bool ParseSnapshot(const std::string &blob, ParsedSnapshot *parsed, std::string *error) {
    if (blob.size() < kSnapshotHeaderBytes + kSnapshotCrcBytes) {
        *error = "snapshot too short (" + std::to_string(blob.size()) + " bytes)";
        return false;
    }
    if (std::memcmp(blob.data(), kSnapshotMagic, sizeof(kSnapshotMagic)) != 0) {
        *error = "snapshot magic mismatch";
        return false;
    }
    // step1: 先校验整体 CRC，再解析内容——截断/翻转字节都在这里被挡住。
    std::uint32_t stored_crc = 0;
    {
        SnapshotReader tail(blob);
        tail.Skip(blob.size() - kSnapshotCrcBytes);
        if (!tail.U32(&stored_crc)) {
            *error = "snapshot crc field truncated";
            return false;
        }
    }
    const std::uint32_t actual_crc =
        Crc32(blob.data(), blob.size() - kSnapshotCrcBytes);
    if (stored_crc != actual_crc) {
        *error = "snapshot crc mismatch";
        return false;
    }
    SnapshotReader reader(blob);
    if (!reader.Skip(sizeof(kSnapshotMagic))) {
        *error = "snapshot magic truncated";
        return false;
    }
    std::uint16_t version = 0;
    std::uint16_t flags = 0;
    std::uint32_t count = 0;
    if (!reader.U16(&version) || !reader.U16(&flags) || !reader.U32(&count)) {
        *error = "snapshot header truncated";
        return false;
    }
    if (version != kSnapshotVersion) {
        *error = "unsupported snapshot version " + std::to_string(version);
        return false;
    }
    if (count > (blob.size() - kSnapshotHeaderBytes) / 2) {
        *error = "snapshot record count out of range";
        return false;
    }
    // step2: 逐条解析；未知 kind、长度越界、kind 值非法都视为损坏。
    for (std::uint32_t index = 0; index < count; ++index) {
        std::uint8_t kind = 0;
        if (!reader.U8(&kind)) {
            *error = "snapshot record truncated";
            return false;
        }
        if (kind == kRecordFencing) {
            std::string key;
            std::uint64_t token = 0;
            if (!reader.String(&key, kMaxStringBytes) || !reader.U64(&token)) {
                *error = "fencing record truncated";
                return false;
            }
            if (key.empty()) {
                *error = "fencing record with empty key";
                return false;
            }
            auto &slot = parsed->fencing[key];
            slot = std::max(slot, token);
        } else if (kind == kRecordLease) {
            std::uint64_t id = 0;
            std::uint8_t lease_kind = 0;
            std::uint64_t ttl_ms = 0;
            std::uint64_t fencing = 0;
            std::uint64_t deadline_ms = 0;
            std::string name;
            std::string instance;
            std::string owner;
            std::string endpoint;
            if (!reader.U64(&id) || !reader.U8(&lease_kind) || !reader.U64(&ttl_ms) ||
                !reader.U64(&fencing) || !reader.U64(&deadline_ms) ||
                !reader.String(&name, kMaxStringBytes) ||
                !reader.String(&instance, kMaxStringBytes) ||
                !reader.String(&owner, kMaxStringBytes) ||
                !reader.String(&endpoint, kMaxStringBytes)) {
                *error = "lease record truncated";
                return false;
            }
            if (lease_kind > 2 || id == 0) {
                *error = "lease record has invalid kind/id";
                return false;
            }
            parsed->max_lease_id = std::max(parsed->max_lease_id, id);
        } else if (kind == kRecordElection) {
            std::string name;
            std::uint64_t epoch = 0;
            if (!reader.String(&name, kMaxStringBytes) || !reader.U64(&epoch)) {
                *error = "election record truncated";
                return false;
            }
            if (name.empty()) {
                *error = "election record with empty name";
                return false;
            }
            auto &slot = parsed->epochs[name];
            slot = std::max(slot, epoch);
        } else {
            *error = "unknown snapshot record kind " + std::to_string(kind);
            return false;
        }
    }
    // step3: 记录之后必须恰好只剩 CRC 字段，多一个字节都说明格式不对。
    if (reader.remaining() != kSnapshotCrcBytes) {
        *error = "trailing bytes after snapshot records";
        return false;
    }
    return true;
}

} // namespace

std::string Coordinator::persist_error() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_persist_error;
}

Outcome Coordinator::EnsureWritable() const {
    if (m_degraded) {
        return Outcome::Failure(Status::kUnavailable,
                                "coordinator degraded: " + m_load_error);
    }
    return Outcome::Success();
}

std::string Coordinator::SerializeSnapshotLocked() const {
    SnapshotWriter writer;
    writer.buffer.append(kSnapshotMagic, sizeof(kSnapshotMagic));
    writer.U16(kSnapshotVersion);
    writer.U16(0);
    const std::size_t count =
        m_fencing.size() + m_leases.size() + m_elections.size();
    writer.U32(static_cast<std::uint32_t>(count));

    // fencing：key → 已授予的最大 token。**永不清除**的语义在这里落盘。
    for (const auto &[key, token] : m_fencing) {
        writer.U8(kRecordFencing);
        writer.String(key);
        writer.U64(token);
    }
    // 租约表：只保留恢复 lease-id 水位所需字段 + 审计字段。
    const auto steady_now = Clock::now();
    const auto wall_now = std::chrono::system_clock::now();
    for (const auto &[id, record] : m_leases) {
        writer.U8(kRecordLease);
        writer.U64(id);
        writer.U8(static_cast<std::uint8_t>(record.kind));
        writer.U64(static_cast<std::uint64_t>(record.ttl.count()));
        writer.U64(record.fencing);
        const auto deadline_wall =
            wall_now + std::chrono::duration_cast<std::chrono::system_clock::duration>(
                           record.deadline - steady_now);
        const auto deadline_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline_wall.time_since_epoch())
                .count();
        writer.U64(static_cast<std::uint64_t>(deadline_ms));
        writer.String(record.name);
        writer.String(record.instance);
        writer.String(record.owner);
        writer.String(record.endpoint);
    }
    // 选举任期：恢复后新任期只能更大。
    for (const auto &[name, election] : m_elections) {
        writer.U8(kRecordElection);
        writer.String(name);
        writer.U64(election.epoch);
    }
    writer.AppendCrc();
    return std::move(writer.buffer);
}

Outcome Coordinator::Restore() {
    if (m_options.snapshot_path.empty()) {
        return Outcome::Success();
    }
    std::string blob;
    std::string error;
    const ReadStatus status = ReadWholeFile(m_options.snapshot_path, &blob, &error);
    if (status == ReadStatus::kMissing) {
        // 全新部署：没有快照不是错误（第一次 Snapshot 会创建）。
        return Outcome::Success();
    }
    if (status == ReadStatus::kError) {
        return Outcome::Failure(Status::kUnavailable, error);
    }
    ParsedSnapshot parsed;
    if (!ParseSnapshot(blob, &parsed, &error)) {
        return Outcome::Failure(Status::kProtocolError, error);
    }
    // step4: 解析全部成功后一次性提交；与内存状态取较大值（只升不降）。
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto &[key, token] : parsed.fencing) {
        std::uint64_t &slot = m_fencing[key];
        slot = std::max(slot, token);
    }
    for (const auto &[name, epoch] : parsed.epochs) {
        std::uint64_t &slot = m_elections[name].epoch;
        slot = std::max(slot, epoch);
    }
    m_next_lease = std::max(m_next_lease, parsed.max_lease_id + 1);
    m_dirty = false;
    return Outcome::Success();
}

Outcome Coordinator::Snapshot() {
    if (m_options.snapshot_path.empty()) {
        return Outcome::Success();
    }
    if (m_degraded) {
        return Outcome::Failure(Status::kUnavailable,
                                "coordinator degraded: " + m_load_error);
    }
    std::lock_guard<std::mutex> persist_lock(m_snapshot_mutex);
    std::string blob;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        blob = SerializeSnapshotLocked();
        // 写入上限必须与恢复侧 `ReadWholeFile` 的 `kMaxSnapshotBytes` 一致：
        // 否则会写出一个「自己重启时拒绝加载」的快照，把可恢复状态变成拒绝启动
        // （fail-stop 的代价是整机不可用，不能由自己造出来）。
        if (blob.size() > kMaxSnapshotBytes) {
            m_persist_error = "snapshot too large (" + std::to_string(blob.size()) +
                              " bytes > " + std::to_string(kMaxSnapshotBytes) + ")";
            m_dirty = true;
            return Outcome::Failure(Status::kUnavailable, m_persist_error);
        }
        m_dirty = false;
    }
    std::string error;
    if (!WriteWholeFileAtomic(m_options.snapshot_path, blob, m_options.snapshot_fsync,
                              &error)) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_dirty = true;
        m_persist_error = error;
        return Outcome::Failure(Status::kUnavailable, error);
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    m_persist_error.clear();
    return Outcome::Success();
}

Outcome Coordinator::PersistAfterWrite() {
    if (m_options.snapshot_path.empty() || !m_options.snapshot_on_write) {
        return Outcome::Success();
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_dirty) {
            return Outcome::Success();
        }
    }
    return Snapshot();
}

} // namespace go2cpp::distributed
