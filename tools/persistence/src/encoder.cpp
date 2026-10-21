/**
 * @file encoder.cpp
 * @brief 定长资源事件编码实现（与 replay::EncodeEvent 逐字节一致）。
 */

#include "persistence/encoder.hpp"

#include <cstring>
#include <vector>

namespace go2cpp::persistence {
namespace {

using go2cpp::replay::Event;
using go2cpp::replay::EventType;

inline void Store16(std::uint8_t* out, std::uint16_t value) noexcept {
    std::memcpy(out, &value, sizeof(value));
}
inline void Store32(std::uint8_t* out, std::uint32_t value) noexcept {
    std::memcpy(out, &value, sizeof(value));
}
inline void Store64(std::uint8_t* out, std::uint64_t value) noexcept {
    std::memcpy(out, &value, sizeof(value));
}

}  // namespace

bool IsFixedResourceEvent(EventType type) noexcept {
    return go2cpp::replay::IsResourceEventType(type);
}

std::size_t FixedRecordSize(EventType type) noexcept {
    if (go2cpp::replay::IsResourceEventV5(type)) {
        return kResV5RecordSize;
    }
    if (go2cpp::replay::IsResourceEventType(type)) {
        return kResV4RecordSize;
    }
    return 0;
}

std::size_t EncodeFixed(const Event& event, std::uint8_t* dst) noexcept {
    const std::size_t payload_size =
        go2cpp::replay::IsResourceEventV5(event.type) ? 72U : 48U;
    if (!go2cpp::replay::IsResourceEventType(event.type) || dst == nullptr) {
        return 0;
    }
    const std::uint32_t total = static_cast<std::uint32_t>(
        go2cpp::replay::kRecordHeaderSize + payload_size);
    Store32(dst + 0, total);
    Store16(dst + 4, static_cast<std::uint16_t>(event.type));
    Store16(dst + 6, event.flags);
    Store64(dst + 8, event.seq);
    Store64(dst + 16, event.ts_ns);
    Store32(dst + 24, event.gid);
    Store32(dst + 28, event.channel);
    Store32(dst + 32, event.xfer_id);

    std::uint8_t* payload = dst + go2cpp::replay::kRecordHeaderSize;
    Store64(payload + 0, event.resource_id);
    Store64(payload + 8, event.actor_gid);
    Store64(payload + 16, event.target_gid);
    Store32(payload + 24, event.resource_result);
    Store64(payload + 28, event.address);
    Store32(payload + 36, event.waiters);
    Store64(payload + 40, event.vtime_ns);
    if (payload_size == 72U) {
        Store32(payload + 48, static_cast<std::uint32_t>(event.res_fd));
        Store32(payload + 52, event.res_bytes);
        Store64(payload + 56, event.res_aux);
        Store64(payload + 64, static_cast<std::uint64_t>(event.value));
    }

    // 与 EncodeEvent 相同的 CRC 覆盖范围：[0,36) + payload（跳过 crc 字段）。
    std::uint32_t crc = go2cpp::replay::Crc32(dst, 36);
    crc = go2cpp::replay::Crc32(payload, payload_size, crc);
    Store32(dst + 36, crc);
    return total;
}

std::size_t EncodeGeneric(const Event& event, std::uint8_t* dst,
                          std::size_t capacity) {
    thread_local std::vector<std::uint8_t> scratch;
    scratch.clear();
    go2cpp::replay::EncodeEvent(event, scratch);
    if (scratch.size() > capacity) {
        return 0;
    }
    std::memcpy(dst, scratch.data(), scratch.size());
    return scratch.size();
}

std::uint64_t MixValue(const Event& event) noexcept {
    // 不混入 seq：不同引擎的批次预留会让 seq 出现不同空洞，
    // 去掉 seq 后同一事件流在各引擎间应得到相同 checksum。
    return (static_cast<std::uint64_t>(event.type) << 48U) ^
           (static_cast<std::uint64_t>(
                static_cast<std::uint32_t>(event.value)) << 16U) ^
           (event.resource_id * 0x100000001B3ULL);
}

}  // namespace go2cpp::persistence
