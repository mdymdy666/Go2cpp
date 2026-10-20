/**
 * @file split.cpp
 * @brief 前缀分片实现：每个分片都是「从开头到检查点」的完整可读记录。
 */

#include "replay/split.hpp"

#include "replay/format.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace go2cpp::replay {
namespace {

constexpr std::uint64_t kMaxFileSize = 256ULL * 1024ULL * 1024ULL;

bool ReadRecording(const std::string& path, std::vector<Event>* events,
                   std::string* error) {
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        *error = "cannot open '" + path + "': " + std::strerror(errno);
        return false;
    }
    std::fseek(file, 0, SEEK_END);
    const long length = std::ftell(file);
    if (length < 0) {
        std::fclose(file);
        *error = "tell failed";
        return false;
    }
    std::rewind(file);
    if (static_cast<std::uint64_t>(length) > kMaxFileSize) {
        std::fclose(file);
        *error = "file too large (>256MiB)";
        return false;
    }
    std::vector<std::uint8_t> data(static_cast<std::size_t>(length));
    if (!data.empty() &&
        std::fread(data.data(), 1, data.size(), file) != data.size()) {
        std::fclose(file);
        *error = "short read";
        return false;
    }
    std::fclose(file);

    FileHeader header;
    if (!DecodeFileHeader(data.data(), data.size(), &header, error)) {
        return false;
    }
    std::size_t offset = kFileHeaderSize;
    const std::size_t body_start = offset;
    bool complete = false;
    std::uint32_t computed_crc = 0;
    while (offset + kRecordHeaderSize <= data.size()) {
        Event event;
        std::size_t consumed = 0;
        std::string decode_error;
        if (!DecodeEvent(data.data() + offset, data.size() - offset, &event,
                         &consumed, &decode_error)) {
            *error = "record at offset " + std::to_string(offset) + ": " +
                     decode_error;
            return false;
        }
        if (event.type == EventType::kEnd) {
            computed_crc = Crc32(data.data() + body_start, offset - body_start);
            if (static_cast<std::uint32_t>(event.sched_type) != computed_crc) {
                *error = "source file checksum mismatch";
                return false;
            }
            complete = true;
            break;
        }
        events->push_back(std::move(event));
        offset += consumed;
    }
    if (!complete) {
        *error = "source recording is truncated (no kEnd)";
        return false;
    }
    return true;
}

/// 把 `[0, end_index]` 的事件写成独立分片。
bool WritePart(const std::string& path, const std::vector<Event>& events,
               std::size_t end_index, std::uint64_t part_index,
               std::uint64_t total_parts, bool final_part, std::string* error) {
    std::vector<std::uint8_t> out;
    FileHeader header;
    EncodeFileHeader(header, out);

    std::uint32_t body_crc = 0;
    std::uint64_t records = 0;
    std::vector<std::uint8_t> record;

    auto append = [&](const Event& event) {
        record.clear();
        EncodeEvent(event, record);
        body_crc = Crc32(record.data(), record.size(), body_crc);
        out.insert(out.end(), record.begin(), record.end());
        ++records;
    };

    for (std::size_t index = 0; index <= end_index && index < events.size(); ++index) {
        append(events[index]);
    }

    const Event& last = events[std::min(end_index, events.size() - 1U)];
    std::uint64_t end_seq = last.seq + 1U;
    if (!final_part) {
        // 中间分片标记「到此为止」；最后一个分片是完整记录，不写 checkpoint，
        // 否则重放会把它当成 partial。
        Event note;
        note.type = EventType::kNote;
        note.flags = kFlagSynthetic;
        note.seq = end_seq;
        note.ts_ns = last.ts_ns;
        note.vtime_ns = last.vtime_ns;
        note.gid = 0;
        note.channel = kNoChannel;
        note.note = "checkpoint: part " + std::to_string(part_index) + "/" +
                    std::to_string(total_parts) + " last_seq=" +
                    std::to_string(last.seq) + " events=" +
                    std::to_string(end_index + 1U);
        append(note);
        ++end_seq;
    }

    Event end;
    end.type = EventType::kEnd;
    end.flags = kFlagSynthetic;
    end.seq = end_seq;
    end.ts_ns = last.ts_ns;
    end.vtime_ns = last.vtime_ns;
    end.value = static_cast<std::int64_t>(records);
    end.aux64 = 0;
    end.sched_type = body_crc;
    record.clear();
    EncodeEvent(end, record);
    out.insert(out.end(), record.begin(), record.end());

    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) {
        *error = "cannot create '" + path + "': " + std::strerror(errno);
        return false;
    }
    const std::size_t written = std::fwrite(out.data(), 1, out.size(), file);
    std::fclose(file);
    if (written != out.size()) {
        *error = "short write to '" + path + "'";
        return false;
    }
    return true;
}

std::string DefaultPrefix(const std::string& path) {
    const std::filesystem::path source(path);
    return source.stem().string();
}

}  // namespace

SplitResult SplitRecording(const std::string& path, const SplitOptions& options) {
    SplitResult result;
    if (options.every_events == 0 && options.every_ns == 0) {
        result.error = "split: need --every-events or --every-ms";
        return result;
    }
    std::vector<Event> events;
    if (!ReadRecording(path, &events, &result.error)) {
        return result;
    }
    if (events.size() < 2) {
        result.error = "split: recording has too few events";
        return result;
    }
    result.source_events = events.size();

    // 选择分片点（事件下标；0 是 kMeta，不参与切分）。
    std::vector<std::size_t> breakpoints;
    std::uint32_t since_last = 0;
    std::uint64_t last_vtime = 0;
    for (std::size_t index = 1; index < events.size(); ++index) {
        ++since_last;
        const auto& event = events[index];
        bool due = false;
        if (options.every_events > 0 && since_last >= options.every_events) {
            due = true;
        }
        if (options.every_ns > 0 &&
            event.vtime_ns >= last_vtime + options.every_ns) {
            due = true;
        }
        if (due) {
            breakpoints.push_back(index);
            since_last = 0;
            last_vtime = event.vtime_ns;
        }
    }
    // 最后一个分片必须覆盖完整记录（除非最后一个检查点就是最后一条事件）。
    if (breakpoints.empty() || breakpoints.back() != events.size() - 1U) {
        breakpoints.push_back(events.size() - 1U);
    }

    std::filesystem::path out_dir =
        options.out_dir.empty() ? std::filesystem::path(path).parent_path()
                                : std::filesystem::path(options.out_dir);
    if (out_dir.empty()) {
        out_dir = ".";
    }
    std::error_code ec;
    std::filesystem::create_directories(out_dir, ec);
    if (ec) {
        result.error = "split: cannot create output dir '" + out_dir.string() +
                       "': " + ec.message();
        return result;
    }
    const std::string prefix =
        options.prefix.empty() ? DefaultPrefix(path) : options.prefix;
    const std::uint64_t total_parts = breakpoints.size();

    for (std::size_t part = 0; part < breakpoints.size(); ++part) {
        char number[32];
        std::snprintf(number, sizeof(number), "%04llu",
                      static_cast<unsigned long long>(part + 1U));
        const std::filesystem::path part_path =
            out_dir / (prefix + ".part" + number + ".g2cr");
        const bool final_part = (part + 1U == total_parts);
        if (!WritePart(part_path.string(), events, breakpoints[part],
                       part + 1U, total_parts, final_part, &result.error)) {
            return result;
        }
        SplitPart info;
        info.path = part_path.string();
        // 前缀分片：每片都从 events[0]（kMeta）开始。
        info.first_seq = events[0].seq;
        info.last_seq = events[breakpoints[part]].seq;
        info.events = breakpoints[part] + 1U;
        info.vtime_ns = events[breakpoints[part]].vtime_ns;
        result.parts.push_back(std::move(info));
    }
    result.ok = true;
    return result;
}

}  // namespace go2cpp::replay
