/**
 * @file reader.cpp
 * @brief 持久化原型读取端实现（mmap + 逐条 CRC 解码 + 段级范围筛选）。
 */

#include "persistence/reader.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>

namespace go2cpp::persistence {
namespace {

std::uint64_t NowNs() noexcept {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ULL +
           static_cast<std::uint64_t>(ts.tv_nsec);
}

std::string SegmentPath(const std::string& base, std::uint32_t id) {
    char suffix[32];
    std::snprintf(suffix, sizeof(suffix), ".seg-%06u", id);
    return base + suffix;
}

std::string ManifestPath(const std::string& base) {
    return base + ".manifest";
}

class MappedFile {
public:
    explicit MappedFile(const std::string& path) {
        m_fd = ::open(path.c_str(), O_RDONLY);
        if (m_fd < 0) {
            m_error = "open " + path + ": " + std::strerror(errno);
            return;
        }
        struct stat st {};
        if (::fstat(m_fd, &st) != 0) {
            m_error = "fstat " + path + ": " + std::strerror(errno);
            return;
        }
        m_size = static_cast<std::size_t>(st.st_size);
        if (m_size == 0) {
            return;
        }
        void* address =
            ::mmap(nullptr, m_size, PROT_READ, MAP_PRIVATE, m_fd, 0);
        if (address == MAP_FAILED) {
            m_error = "mmap " + path + ": " + std::strerror(errno);
            return;
        }
        m_data = static_cast<const std::uint8_t*>(address);
    }

    ~MappedFile() {
        if (m_data != nullptr) {
            ::munmap(const_cast<std::uint8_t*>(m_data), m_size);
        }
        if (m_fd >= 0) {
            ::close(m_fd);
        }
    }

    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    const std::uint8_t* data() const noexcept { return m_data; }
    std::size_t size() const noexcept { return m_size; }
    bool ok() const noexcept { return m_error.empty(); }
    const std::string& error() const noexcept { return m_error; }

private:
    int m_fd{-1};
    const std::uint8_t* m_data{nullptr};
    std::size_t m_size{0};
    std::string m_error;
};

}  // namespace

SessionReader::~SessionReader() = default;

bool SessionReader::NeedSegment(const SegmentInfo& segment, bool by_time,
                                std::uint64_t low, std::uint64_t high) {
    if (by_time) {
        return !(high < segment.first_ts || low > segment.last_ts);
    }
    return !(high < segment.first_seq || low > segment.last_seq);
}

std::unique_ptr<SessionReader> SessionReader::Open(const std::string& base,
                                                   std::string* error) {
    auto reader = std::unique_ptr<SessionReader>(new SessionReader());
    reader->m_base = base;
    std::ifstream manifest(ManifestPath(base));
    if (manifest) {
        reader->m_segmented = true;
        std::string line;
        if (!std::getline(manifest, line) || line.rfind("G2CSESSION", 0) != 0) {
            if (error != nullptr) {
                *error = "manifest 魔数错误";
            }
            return nullptr;
        }
        std::uint64_t declared_segments = 0;
        while (std::getline(manifest, line)) {
            std::istringstream in(line);
            std::string key;
            in >> key;
            if (key == "records") {
                in >> reader->m_expected_records;
            } else if (key == "bytes") {
                in >> reader->m_expected_bytes;
            } else if (key == "checksum") {
                in >> reader->m_expected_checksum;
            } else if (key == "segments") {
                in >> declared_segments;
            } else if (key == "seg") {
                SegmentFile file;
                in >> file.info.id >> file.info.bytes >> file.info.first_seq >>
                    file.info.last_seq >> file.info.count >>
                    file.info.first_ts >> file.info.last_ts;
                if (!in) {
                    if (error != nullptr) {
                        *error = "manifest seg 行格式错误: " + line;
                    }
                    return nullptr;
                }
                file.path = SegmentPath(base, file.info.id);
                struct stat st {};
                if (::stat(file.path.c_str(), &st) != 0) {
                    if (error != nullptr) {
                        *error = "段文件不存在: " + file.path;
                    }
                    return nullptr;
                }
                if (static_cast<std::uint64_t>(st.st_size) != file.info.bytes) {
                    if (error != nullptr) {
                        *error = "段文件大小与 manifest 不一致: " + file.path;
                    }
                    return nullptr;
                }
                reader->m_segments.push_back(std::move(file));
            }
        }
        if (reader->m_segments.size() != declared_segments) {
            if (error != nullptr) {
                *error = "manifest 段数不一致";
            }
            return nullptr;
        }
    } else {
        std::ifstream file(base, std::ios::binary);
        if (!file) {
            if (error != nullptr) {
                *error = "既无 manifest 也无单文件: " + base;
            }
            return nullptr;
        }
        file.seekg(0, std::ios::end);
        const std::streamoff size = file.tellg();
        reader->m_single_file = true;
        SegmentFile only;
        only.info.id = 0;
        only.info.bytes = static_cast<std::uint64_t>(size);
        only.info.first_seq = 0;
        only.info.last_seq = UINT64_MAX;
        only.info.first_ts = 0;
        only.info.last_ts = UINT64_MAX;
        only.path = base;
        reader->m_segments.push_back(std::move(only));
    }
    return reader;
}

ReadStats SessionReader::Scan(bool by_time, std::uint64_t low,
                              std::uint64_t high) {
    ReadStats stats;
    const std::uint64_t begin = NowNs();
    std::uint64_t last_seq = 0;
    bool saw_seq = false;
    bool saw_end = false;
    for (const auto& segment : m_segments) {
        if (m_segmented &&
            !NeedSegment(segment.info, by_time, low, high)) {
            continue;
        }
        MappedFile file(segment.path);
        if (!file.ok()) {
            stats.error = file.error();
            stats.crc_ok = false;
            break;
        }
        std::size_t offset = 0;
        if (m_single_file) {
            if (file.size() < go2cpp::replay::kFileHeaderSize) {
                stats.error = "单文件小于文件头";
                stats.crc_ok = false;
                break;
            }
            offset = go2cpp::replay::kFileHeaderSize;
        }
        bool stop_segment = false;
        while (offset + go2cpp::replay::kRecordHeaderSize <= file.size()) {
            go2cpp::replay::Event event;
            std::size_t consumed = 0;
            std::string decode_error;
            if (!go2cpp::replay::DecodeEvent(file.data() + offset,
                                             file.size() - offset, &event,
                                             &consumed, &decode_error)) {
                stats.crc_ok = false;
                stats.error = "offset " + std::to_string(offset) + ": " +
                              decode_error;
                stop_segment = true;
                break;
            }
            if (event.type == go2cpp::replay::EventType::kEnd) {
                offset += consumed;
                saw_end = true;
                break;
            }
            if (go2cpp::replay::IsResourceEventType(event.type)) {
                const bool in_range =
                    by_time ? (event.ts_ns >= low && event.ts_ns <= high)
                            : (event.seq >= low && event.seq <= high);
                if (in_range) {
                    ++stats.records;
                    stats.checksum += MixValue(event);
                    if (saw_seq && event.seq <= last_seq) {
                        stats.seq_ok = false;
                    }
                    last_seq = event.seq;
                    saw_seq = true;
                }
                if (!by_time && event.seq > high) {
                    offset += consumed;
                    stop_segment = true;
                    break;
                }
            }
            offset += consumed;
        }
        stats.bytes += offset;
        ++stats.segments_read;
        if (stop_segment && !stats.error.empty()) {
            break;
        }
    }
    stats.elapsed_ns = NowNs() - begin;
    if (m_single_file && !saw_end && stats.error.empty()) {
        // 单文件没有 kEnd：录制被截断或写入未完成，不能当作完整会话。
        stats.error = "单文件缺少 kEnd（录制被截断）";
        stats.crc_ok = false;
    }
    return stats;
}

ReadStats SessionReader::ScanAll() {
    return Scan(false, 0, UINT64_MAX);
}

ReadStats SessionReader::ReadSeqRange(std::uint64_t from_seq,
                                      std::uint64_t to_seq) {
    return Scan(false, from_seq, to_seq);
}

ReadStats SessionReader::ReadTimeRange(std::uint64_t from_ts,
                                       std::uint64_t to_ts) {
    return Scan(true, from_ts, to_ts);
}

}  // namespace go2cpp::persistence
