/**
 * @file format.cpp
 * @brief 稳定二进制格式的编解码实现（v1 / v2）。
 *
 * 编码路径刻意逐字段写小端，而不是 `memcpy(struct)`：后者依赖编译器
 * padding 与宿主端序，跨机器/跨编译器就再也读不回来。
 *
 * v2 的兼容策略：**只在 v1 payload 尾部追加**。v1 解码器读完自己认识的
 * 字段后不会检查剩余字节，因此 v2 文件在 v1 工具里仍可读；v2 解码器遇到
 * v1 文件时按 `remaining()` 判断追加字段不存在。
 */

#include "replay/format.hpp"

#include <array>
#include <cstring>

namespace go2cpp::replay {
namespace {

std::array<std::uint32_t, 256> MakeCrcTable() noexcept {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t index = 0; index < 256; ++index) {
        std::uint32_t value = index;
        for (int bit = 0; bit < 8; ++bit) {
            value = (value & 1U) != 0U ? (value >> 1U) ^ 0xEDB88320U : (value >> 1U);
        }
        table[index] = value;
    }
    return table;
}

const std::array<std::uint32_t, 256>& CrcTable() noexcept {
    static const std::array<std::uint32_t, 256> table = MakeCrcTable();
    return table;
}

}  // namespace

std::uint32_t Crc32(const void* data, std::size_t size, std::uint32_t seed) noexcept {
    const auto& table = CrcTable();
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    std::uint32_t crc = seed ^ 0xFFFFFFFFU;
    for (std::size_t index = 0; index < size; ++index) {
        crc = table[(crc ^ bytes[index]) & 0xFFU] ^ (crc >> 8U);
    }
    return crc ^ 0xFFFFFFFFU;
}

std::uint64_t Fnv1a64(const void* data, std::size_t size, std::uint64_t basis) noexcept {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    std::uint64_t hash = basis;
    for (std::size_t index = 0; index < size; ++index) {
        hash ^= static_cast<std::uint64_t>(bytes[index]);
        hash *= 1099511628211ULL;
    }
    return hash;
}

const char* EventTypeName(EventType type) noexcept {
    switch (type) {
        case EventType::kMeta: return "meta";
        case EventType::kSpawn: return "spawn";
        case EventType::kExit: return "exit";
        case EventType::kSend: return "send";
        case EventType::kRecv: return "recv";
        case EventType::kSelect: return "select";
        case EventType::kClose: return "close";
        case EventType::kTimer: return "timer";
        case EventType::kCtx: return "ctx";
        case EventType::kPark: return "park";
        case EventType::kWake: return "wake";
        case EventType::kSched: return "sched";
        case EventType::kNote: return "note";
        case EventType::kResMutexLock: return "res-mutex-lock";
        case EventType::kResMutexUnlock: return "res-mutex-unlock";
        case EventType::kResPCWait: return "res-pc-wait";
        case EventType::kResPCNotify: return "res-pc-notify";
        case EventType::kResChannelSend: return "res-channel-send";
        case EventType::kResChannelRecv: return "res-channel-recv";
        case EventType::kEnd: return "end";
    }
    return "unknown";
}

const char* ValueKindName(ValueKind kind) noexcept {
    switch (kind) {
        case ValueKind::kI64: return "i64";
        case ValueKind::kBytes: return "bytes";
        case ValueKind::kMixed: return "mixed";
    }
    return "unknown";
}

const char* SelectCaseKindName(SelectCaseKind kind) noexcept {
    switch (kind) {
        case SelectCaseKind::kRecv: return "recv";
        case SelectCaseKind::kSend: return "send";
        case SelectCaseKind::kDefault: return "default";
        case SelectCaseKind::kTimeout: return "timeout";
        case SelectCaseKind::kContext: return "context";
    }
    return "unknown";
}

const char* TimerKindName(TimerKind kind) noexcept {
    switch (kind) {
        case TimerKind::kSleep: return "sleep";
        case TimerKind::kDeadlineSet: return "deadline-set";
        case TimerKind::kDeadlineExceeded: return "deadline-exceeded";
        case TimerKind::kTimeoutSelected: return "timeout-selected";
        case TimerKind::kServiceArm: return "service-arm";
        case TimerKind::kServiceFire: return "service-fire";
        case TimerKind::kServiceCancel: return "service-cancel";
        case TimerKind::kServiceReschedule: return "service-reschedule";
    }
    return "unknown";
}

const char* ContextEventKindName(ContextEventKind kind) noexcept {
    switch (kind) {
        case ContextEventKind::kCancelled: return "cancelled";
        case ContextEventKind::kDeadlineSet: return "deadline-set";
        case ContextEventKind::kDeadlineExceeded: return "deadline-exceeded";
    }
    return "unknown";
}

namespace {

void EncodePayload(const Event& event, std::vector<std::uint8_t>& payload) {
    ByteWriter writer(payload);
    switch (event.type) {
        case EventType::kMeta:
            writer.U32(event.gid);                    // n_gids
            writer.U32(event.channel);                // n_channels
            writer.U32(static_cast<std::uint32_t>(event.value));  // value_kind
            writer.U32(0);                            // reserved
            writer.Str(event.note);                   // demo name
            // v2 追加
            writer.U32(event.ext_flags);
            writer.U32(event.clock_mode);
            break;
        case EventType::kSpawn:
            break;
        case EventType::kExit:
            writer.I64(event.value);                  // sent
            writer.I64(static_cast<std::int64_t>(event.aux64));  // recv
            break;
        case EventType::kSend:
        case EventType::kRecv:
            writer.U8(static_cast<std::uint8_t>(event.decision));
            writer.U8(event.blocked);
            writer.U16(0);
            writer.U32(event.op_index);
            writer.I64(event.value);
            // v2 追加：虚拟时间 + bytes 值
            writer.U64(event.vtime_ns);
            writer.Str(event.data);
            break;
        case EventType::kSelect:
            writer.U8(static_cast<std::uint8_t>(event.decision));
            writer.U8(event.blocked);
            writer.U16(event.chosen);
            writer.U32(event.op_index);
            writer.I64(event.value);
            // v2 追加：虚拟时间 + bytes 值 + case 表
            writer.U64(event.vtime_ns);
            writer.Str(event.data);
            writer.U32(static_cast<std::uint32_t>(event.cases.size()));
            for (const auto& select_case : event.cases) {
                writer.U32(select_case.channel);
                writer.U8(static_cast<std::uint8_t>(select_case.kind));
                writer.U8(0);
                writer.U16(0);
            }
            break;
        case EventType::kClose:
            writer.U8(static_cast<std::uint8_t>(event.decision));
            writer.U8(event.blocked);
            writer.U16(0);
            writer.U32(event.op_index);
            writer.U64(event.vtime_ns);
            break;
        case EventType::kTimer:
            writer.U8(event.timer_kind);
            writer.U8(0);
            writer.U16(0);
            writer.U32(event.op_index);
            writer.U64(event.timer_ns);
            writer.U64(event.vtime_ns);
            writer.Str(event.note);
            // v3 追加：TimerService 的 timer 生命周期字段（旧解码器按尾部
            // 剩余字节判断，v1/v2 文件不受影响）。
            writer.U32(event.timer_id);
            writer.U32(event.timer_source);
            writer.U64(event.deadline_ns);
            writer.U64(event.fired_at_ns);
            writer.I64(event.callback_result);
            break;
        case EventType::kCtx:
            writer.U32(event.ctx_id);
            writer.U8(event.ctx_kind);
            writer.U8(0);
            writer.U16(0);
            writer.U32(event.op_index);
            writer.U64(event.vtime_ns);
            break;
        case EventType::kPark:
        case EventType::kWake:
            writer.U8(static_cast<std::uint8_t>(event.reason));
            writer.U8(0);
            writer.U16(0);
            writer.U32(event.op_index);
            writer.U64(event.vtime_ns);
            break;
        case EventType::kSched:
            writer.U32(event.sched_type);
            writer.U32(event.machine);
            writer.U32(event.processor);
            break;
        case EventType::kNote:
            writer.Str(event.note);
            break;
        // v4：资源事件固定 48 字节 payload。字段顺序即磁盘顺序，
        // 后续版本只能在其后追加（v4 读端按 remaining 判断）。
        case EventType::kResMutexLock:
        case EventType::kResMutexUnlock:
        case EventType::kResPCWait:
        case EventType::kResPCNotify:
        case EventType::kResChannelSend:
        case EventType::kResChannelRecv:
            writer.U64(event.resource_id);
            writer.U64(event.actor_gid);
            writer.U64(event.target_gid);
            writer.U32(event.resource_result);
            writer.U64(event.address);
            writer.U32(event.waiters);
            // vtime_ns 必须在 payload 里：资源事件没有 v2 的
            // 「通用 vtime 尾字段」，漏掉它会让锁持有时间恒为 0。
            writer.U64(event.vtime_ns);
            break;
        case EventType::kEnd:
            writer.U64(static_cast<std::uint64_t>(event.value));  // records
            writer.U64(event.aux64);                              // dropped
            writer.U32(event.sched_type);                         // file_crc
            writer.U32(0);
            break;
    }
}

}  // namespace

void EncodeEvent(const Event& event, std::vector<std::uint8_t>& out) {
    std::vector<std::uint8_t> payload;
    payload.reserve(64);
    EncodePayload(event, payload);

    const std::uint32_t total =
        kRecordHeaderSize + static_cast<std::uint32_t>(payload.size());
    const std::size_t base = out.size();

    ByteWriter writer(out);
    writer.U32(total);
    writer.U16(static_cast<std::uint16_t>(event.type));
    writer.U16(event.flags);
    writer.U64(event.seq);
    writer.U64(event.ts_ns);
    writer.U32(event.gid);
    writer.U32(event.channel);
    writer.U32(event.xfer_id);

    if (out.size() - base != kRecordHeaderSize - 4U) {
        // 头部字段布局与 kRecordHeaderSize 不一致说明改坏了格式常量。
        out.resize(base);
        return;
    }

    std::uint32_t crc = Crc32(out.data() + base, out.size() - base);
    crc = Crc32(payload.data(), payload.size(), crc);
    writer.U32(crc);

    out.insert(out.end(), payload.begin(), payload.end());
}

bool DecodeEvent(const std::uint8_t* data, std::size_t size, Event* event,
                 std::size_t* consumed, std::string* error) {
    if (consumed != nullptr) {
        *consumed = 0;
    }
    if (error != nullptr) {
        error->clear();
    }
    if (data == nullptr || event == nullptr) {
        if (error != nullptr) *error = "null buffer";
        return false;
    }
    if (size < kRecordHeaderSize) {
        if (error != nullptr) *error = "truncated record header";
        return false;
    }

    ByteReader header(data, size);
    const std::uint32_t total = header.U32();
    const auto raw_type = header.U16();
    const std::uint16_t flags = header.U16();
    const std::uint64_t seq = header.U64();
    const std::uint64_t ts_ns = header.U64();
    const std::uint32_t gid = header.U32();
    const std::uint32_t channel = header.U32();
    const std::uint32_t aux = header.U32();
    const std::uint32_t stored_crc = header.U32();
    if (!header.ok()) {
        if (error != nullptr) *error = "truncated record header";
        return false;
    }
    if (total < kRecordHeaderSize || total > kMaxRecordSize) {
        if (error != nullptr) *error = "invalid record size " + std::to_string(total);
        return false;
    }
    if (total > size) {
        if (error != nullptr) *error = "truncated record payload";
        return false;
    }

    const std::uint8_t* payload = data + kRecordHeaderSize;
    const std::size_t payload_size = total - kRecordHeaderSize;
    std::uint32_t crc = Crc32(data, kRecordHeaderSize - 4U);
    crc = Crc32(payload, payload_size, crc);
    if (crc != stored_crc) {
        if (error != nullptr) *error = "crc mismatch";
        return false;
    }

    Event out;
    out.type = static_cast<EventType>(raw_type);
    out.flags = flags;
    out.seq = seq;
    out.ts_ns = ts_ns;
    out.gid = gid;
    out.channel = channel;
    out.xfer_id = aux;

    ByteReader reader(payload, payload_size);
    switch (out.type) {
        case EventType::kMeta:
            out.gid = reader.U32();
            out.channel = reader.U32();
            out.value = static_cast<std::int64_t>(reader.U32());
            (void)reader.U32();
            out.note = reader.Str();
            if (reader.ok() && reader.remaining() >= 8U) {
                out.ext_flags = reader.U32();
                out.clock_mode = reader.U32();
            }
            break;
        case EventType::kSpawn:
            break;
        case EventType::kExit:
            out.value = reader.I64();
            out.aux64 = static_cast<std::uint64_t>(reader.I64());
            break;
        case EventType::kSend:
        case EventType::kRecv:
            out.decision = static_cast<Decision>(reader.U8());
            out.blocked = reader.U8();
            (void)reader.U16();
            out.op_index = reader.U32();
            out.value = reader.I64();
            if (reader.ok() && reader.remaining() >= 8U) {
                out.vtime_ns = reader.U64();
            }
            if (reader.ok() && reader.remaining() > 0U) {
                out.data = reader.Str();
            }
            break;
        case EventType::kSelect:
            out.decision = static_cast<Decision>(reader.U8());
            out.blocked = reader.U8();
            out.chosen = reader.U16();
            out.op_index = reader.U32();
            out.value = reader.I64();
            if (reader.ok() && reader.remaining() >= 8U) {
                out.vtime_ns = reader.U64();
            }
            if (reader.ok() && reader.remaining() > 0U) {
                out.data = reader.Str();
            }
            if (reader.ok() && reader.remaining() >= 4U) {
                const std::uint32_t case_count = reader.U32();
                if (case_count > 1024U) {
                    if (error != nullptr) *error = "select case count too large";
                    return false;
                }
                out.cases.reserve(case_count);
                for (std::uint32_t index = 0; index < case_count && reader.ok(); ++index) {
                    SelectCaseRecord select_case;
                    select_case.channel = reader.U32();
                    select_case.kind = static_cast<SelectCaseKind>(reader.U8());
                    (void)reader.U8();
                    (void)reader.U16();
                    out.cases.push_back(select_case);
                }
            }
            break;
        case EventType::kClose:
            out.decision = static_cast<Decision>(reader.U8());
            out.blocked = reader.U8();
            (void)reader.U16();
            out.op_index = reader.U32();
            if (reader.ok() && reader.remaining() >= 8U) {
                out.vtime_ns = reader.U64();
            }
            break;
        case EventType::kTimer:
            out.timer_kind = reader.U8();
            (void)reader.U8();
            (void)reader.U16();
            out.op_index = reader.U32();
            out.timer_ns = reader.U64();
            out.vtime_ns = reader.U64();
            out.note = reader.Str();
            if (reader.ok() && reader.remaining() >= 32U) {
                out.timer_id = reader.U32();
                out.timer_source = reader.U32();
                out.deadline_ns = reader.U64();
                out.fired_at_ns = reader.U64();
                out.callback_result = reader.I64();
            }
            break;
        case EventType::kCtx:
            out.ctx_id = reader.U32();
            out.ctx_kind = reader.U8();
            (void)reader.U8();
            (void)reader.U16();
            out.op_index = reader.U32();
            out.vtime_ns = reader.U64();
            break;
        case EventType::kPark:
        case EventType::kWake:
            out.reason = static_cast<ParkReason>(reader.U8());
            (void)reader.U8();
            (void)reader.U16();
            out.op_index = reader.U32();
            if (reader.ok() && reader.remaining() >= 8U) {
                out.vtime_ns = reader.U64();
            }
            break;
        case EventType::kSched:
            out.sched_type = reader.U32();
            out.machine = reader.U32();
            out.processor = reader.U32();
            break;
        case EventType::kNote:
            out.note = reader.Str();
            break;
        case EventType::kResMutexLock:
        case EventType::kResMutexUnlock:
        case EventType::kResPCWait:
        case EventType::kResPCNotify:
        case EventType::kResChannelSend:
        case EventType::kResChannelRecv:
            if (reader.ok() && reader.remaining() >= 40U) {
                out.resource_id = reader.U64();
                out.actor_gid = reader.U64();
                out.target_gid = reader.U64();
                out.resource_result = reader.U32();
                out.address = reader.U64();
                out.waiters = reader.U32();
                if (reader.remaining() >= 8U) {
                    out.vtime_ns = reader.U64();
                }
            }
            break;
        case EventType::kEnd:
            out.value = static_cast<std::int64_t>(reader.U64());
            out.aux64 = reader.U64();
            out.sched_type = reader.U32();
            (void)reader.U32();
            break;
        default:
            // 前向兼容：未知类型保留原始枚举，payload 原样跳过（不解释）。
            break;
    }
    if (!reader.ok()) {
        if (error != nullptr) *error = "malformed payload";
        return false;
    }

    *event = std::move(out);
    if (consumed != nullptr) {
        *consumed = total;
    }
    return true;
}

void EncodeFileHeader(const FileHeader& header, std::vector<std::uint8_t>& out) {
    ByteWriter writer(out);
    for (char byte : kMagic) {
        writer.U8(static_cast<std::uint8_t>(byte));
    }
    writer.U16(header.version);
    writer.U16(header.header_size);
    writer.U32(header.flags);
    writer.U64(header.created_unix_ns);
    writer.U64(header.producer_id);
}

bool DecodeFileHeader(const std::uint8_t* data, std::size_t size, FileHeader* header,
                      std::string* error) {
    if (error != nullptr) {
        error->clear();
    }
    if (data == nullptr || header == nullptr || size < kFileHeaderSize) {
        if (error != nullptr) *error = "truncated file header";
        return false;
    }
    if (std::memcmp(data, kMagic, sizeof(kMagic)) != 0) {
        if (error != nullptr) *error = "bad magic (not a go2cpp replay file)";
        return false;
    }
    ByteReader reader(data + sizeof(kMagic), size - sizeof(kMagic));
    FileHeader out;
    out.version = reader.U16();
    out.header_size = reader.U16();
    out.flags = reader.U32();
    out.created_unix_ns = reader.U64();
    out.producer_id = reader.U64();
    if (!reader.ok() || out.header_size != kFileHeaderSize) {
        if (error != nullptr) *error = "unsupported/invalid file header";
        return false;
    }
    if (out.version < kMinFormatVersion || out.version > kFormatVersion) {
        if (error != nullptr) {
            *error = "unsupported format version " + std::to_string(out.version);
        }
        return false;
    }
    *header = out;
    return true;
}

}  // namespace go2cpp::replay
