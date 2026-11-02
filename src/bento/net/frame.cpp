/**
 * @file frame.cpp
 * @brief 通用帧编解码实现（MOD-10）。
 * @details 所有多字节字段按网络字节序（大端）写入/读取，不依赖主机字节序，
 *          也不依赖 htonl/ntohl，便于测试手工构造异常帧。
 *
 *          本文件同时提供两套所有权模型：
 *          - 拷贝版：`Encode` / `Decode` / `FrameDecoder::Next`，每帧一次分配；
 *          - 零拷贝版：`EncodeInto` / `DecodeView` / `FrameDecoder::NextView`，
 *            热路径复用调用方缓冲，不做堆分配。
 *
 *          两套路径共用同一个 `ParseHeader` 校验函数，避免「拷贝版与零拷贝版
 *          给出不同错误码」这类分叉（错误码顺序在 `ParseHeader` 的注释里固定）。
 *
 * @author go2cpp contributors
 * @date 2026-10-28（零拷贝 API 2026-11-01）
 * @copyright MIT License
 */

#include "go2cpp/bento/net/frame.hpp"

#include <cstring>
#include <limits>
#include <new>

#include "go2cpp/hash/crc32.hpp"
#include "go2cpp/network/endian.hpp"

namespace go2cpp::bento::net {
namespace {

void WriteU16(std::uint8_t* out, std::uint16_t value) noexcept {
    go2cpp::net::WriteBE16(out, value);
}

void WriteU32(std::uint8_t* out, std::uint32_t value) noexcept {
    go2cpp::net::WriteBE32(out, value);
}

std::uint16_t ReadU16(const std::uint8_t* in) noexcept {
    return go2cpp::net::ReadBE16(in);
}

std::uint32_t ReadU32(const std::uint8_t* in) noexcept {
    return go2cpp::net::ReadBE32(in);
}

/// 解析后的帧头字段 + 载荷位置；`payload` 指向输入缓冲，不拷贝。
struct ParsedHeader {
    std::uint32_t magic = 0;
    std::uint16_t version = 0;
    std::uint16_t type = 0;
    std::uint16_t flags = 0;
    std::uint16_t reserved = 0;
    std::uint32_t length = 0;
    std::uint32_t checksum = 0;
    const std::uint8_t* payload = nullptr;
    std::size_t total = 0;
};

/// @brief 从缓冲区开头解析一帧的头部并校验 CRC；不修改输入，不做任何缓冲管理。
/// @note 校验顺序固定为 magic → version → reserved → length → checksum，
///       拷贝版与零拷贝版必须共用本函数，避免两条路径给出不同错误码。
FrameStatus ParseHeader(const FrameFormat& format, const std::uint8_t* data, std::size_t size,
                        std::uint32_t max_payload, ParsedHeader* out) noexcept {
    if (size < kGenericFrameHeaderBytes) {
        return FrameStatus::kNeedMoreData;
    }
    const std::uint32_t magic = ReadU32(data);
    if (magic != format.magic) {
        return FrameStatus::kBadMagic;
    }
    const std::uint16_t version = ReadU16(data + 4);
    if (version != format.version) {
        return FrameStatus::kBadVersion;
    }
    const std::uint16_t reserved = ReadU16(data + 10);
    if (reserved != 0) {
        return FrameStatus::kBadReserved;
    }
    const std::uint32_t length = ReadU32(data + 12);
    if (length > max_payload) {
        return FrameStatus::kLengthExceeded;
    }
    // 32 位平台上 20 + UINT32_MAX 会溢出 size_t，使下面的 size < total 判断失效，
    // 进而把「声明 4 GiB」误当成完整帧去算 CRC。64 位平台该条件恒为假，会被消除。
    if (static_cast<std::size_t>(length) >
        std::numeric_limits<std::size_t>::max() - kGenericFrameHeaderBytes) {
        return FrameStatus::kLengthExceeded;
    }
    const std::size_t total = kGenericFrameHeaderBytes + static_cast<std::size_t>(length);
    if (size < total) {
        return FrameStatus::kNeedMoreData;
    }
    const std::uint32_t checksum = ReadU32(data + 16);
    if (checksum != ComputeFrameChecksum(data, data + kGenericFrameHeaderBytes, length)) {
        return FrameStatus::kChecksumMismatch;
    }
    out->magic = magic;
    out->version = version;
    out->type = ReadU16(data + 6);
    out->flags = ReadU16(data + 8);
    out->reserved = reserved;
    out->length = length;
    out->checksum = checksum;
    out->payload = data + kGenericFrameHeaderBytes;
    out->total = total;
    return FrameStatus::kOk;
}

/// 拷贝版：头部校验通过后把载荷复制进 `Frame::payload`。
FrameStatus ParseOne(const FrameFormat& format, const std::uint8_t* data, std::size_t size,
                     std::uint32_t max_payload, Frame* out, std::size_t* consumed) noexcept {
    ParsedHeader header;
    const FrameStatus status = ParseHeader(format, data, size, max_payload, &header);
    if (status != FrameStatus::kOk) {
        return status;
    }
    out->magic = header.magic;
    out->version = header.version;
    out->type = header.type;
    out->flags = header.flags;
    out->reserved = header.reserved;
    out->checksum = header.checksum;
    try {
        out->payload.assign(header.payload, header.payload + header.length);
    } catch (const std::bad_alloc&) {
        // 本函数与公开的 Decode 都是 noexcept：分配失败必须转成状态码，否则会
        // 直接 std::terminate。载荷已被 max_payload 限死，正常环境不会触发。
        return FrameStatus::kOutOfMemory;
    }
    if (consumed != nullptr) {
        *consumed = header.total;
    }
    return FrameStatus::kOk;
}

/// 零拷贝版：只填视图，载荷指针指向输入缓冲内部。
FrameStatus ParseOneView(const FrameFormat& format, const std::uint8_t* data, std::size_t size,
                         std::uint32_t max_payload, FrameView* out,
                         std::size_t* consumed) noexcept {
    ParsedHeader header;
    const FrameStatus status = ParseHeader(format, data, size, max_payload, &header);
    if (status != FrameStatus::kOk) {
        return status;
    }
    out->magic = header.magic;
    out->version = header.version;
    out->type = header.type;
    out->flags = header.flags;
    out->reserved = header.reserved;
    out->checksum = header.checksum;
    out->payload = header.payload;
    out->payload_bytes = header.length;
    if (consumed != nullptr) {
        *consumed = header.total;
    }
    return FrameStatus::kOk;
}

/// 编码前的公共合法性检查：reserved 必须 0、magic 非 0、载荷不超限。
bool IsEncodable(const Frame& frame, const FrameFormat& format) noexcept {
    // reserved 是给未来扩展留的零值字段：当前版本出现非零值就不能保证
    // 对方能正确解析，宁可在发送端拒绝，也不要把隐患放到线上。
    if (frame.reserved != 0) {
        return false;
    }
    // magic == 0 无法与「全零缓冲区」区分，接收端也没有任何协议标识可用。
    if (format.magic == 0) {
        return false;
    }
    return frame.payload.size() <= kFrameMaxPayload;
}

}  // namespace

const char* FrameStatusName(FrameStatus status) noexcept {
    switch (status) {
        case FrameStatus::kOk:
            return "ok";
        case FrameStatus::kNeedMoreData:
            return "need_more_data";
        case FrameStatus::kBadMagic:
            return "bad_magic";
        case FrameStatus::kBadVersion:
            return "bad_version";
        case FrameStatus::kBadReserved:
            return "bad_reserved";
        case FrameStatus::kLengthExceeded:
            return "length_exceeded";
        case FrameStatus::kChecksumMismatch:
            return "checksum_mismatch";
        case FrameStatus::kInvalidArgument:
            return "invalid_argument";
        case FrameStatus::kOutOfMemory:
            return "out_of_memory";
    }
    return "unknown";
}

std::uint32_t ComputeFrameChecksum(const std::uint8_t* header_prefix,
                                   const std::uint8_t* payload,
                                   std::size_t payload_bytes) noexcept {
    // 校验和本身不在覆盖范围内，所以这里只需要头的前 16 字节。
    const std::uint32_t crc = hash::Crc32::ChecksumIEEE(
        hash::AsBytes(header_prefix, kGenericFrameHeaderBytes - sizeof(std::uint32_t)));
    if (payload_bytes == 0) {
        return crc;
    }
    return hash::Crc32::Update(crc, hash::IEEETable,
                               hash::AsBytes(payload, payload_bytes));
}

std::vector<std::uint8_t> Encode(const Frame& frame) {
    return Encode(frame, kDefaultFrameFormat);
}

std::vector<std::uint8_t> Encode(const Frame& frame, const FrameFormat& format) {
    if (!IsEncodable(frame, format)) {
        return {};
    }
    std::vector<std::uint8_t> out(kGenericFrameHeaderBytes + frame.payload.size());
    if (!EncodeInto(frame, format, out.data(), out.size(), nullptr)) {
        return {};
    }
    return out;
}

bool EncodeInto(const Frame& frame, std::uint8_t* out, std::size_t capacity,
                std::size_t* written) noexcept {
    return EncodeInto(frame, kDefaultFrameFormat, out, capacity, written);
}

bool EncodeInto(const Frame& frame, const FrameFormat& format, std::uint8_t* out,
                std::size_t capacity, std::size_t* written) noexcept {
    if (out == nullptr || !IsEncodable(frame, format)) {
        return false;
    }
    const std::size_t total = kGenericFrameHeaderBytes + frame.payload.size();
    if (capacity < total) {
        return false;
    }
    WriteU32(out, format.magic);
    WriteU16(out + 4, format.version);
    WriteU16(out + 6, frame.type);
    WriteU16(out + 8, frame.flags);
    WriteU16(out + 10, 0);
    WriteU32(out + 12, static_cast<std::uint32_t>(frame.payload.size()));
    // 必须先把 payload 写入 out，再计算 checksum：ComputeFrameChecksum 读取的是
    // 输出缓冲中的 payload 区域；顺序反了会把全零当成 payload 计算。
    if (!frame.payload.empty()) {
        std::memcpy(out + kGenericFrameHeaderBytes, frame.payload.data(),
                    frame.payload.size());
    }
    // 计算 CRC 时直接用源 payload（刚写过、仍在 cache 里），避免再读一遍 out。
    const std::uint32_t checksum =
        ComputeFrameChecksum(out, frame.payload.data(), frame.payload.size());
    WriteU32(out + 16, checksum);
    if (written != nullptr) {
        *written = total;
    }
    return true;
}

FrameStatus Decode(const std::uint8_t* data, std::size_t size, Frame* out,
                   std::size_t* consumed) noexcept {
    if (out == nullptr) {
        return FrameStatus::kInvalidArgument;
    }
    if (data == nullptr) {
        return size == 0 ? FrameStatus::kNeedMoreData : FrameStatus::kInvalidArgument;
    }
    return ParseOne(kDefaultFrameFormat, data, size, kFrameMaxPayload, out, consumed);
}

FrameStatus Decode(const FrameFormat& format, const std::uint8_t* data, std::size_t size,
                   Frame* out, std::size_t* consumed) noexcept {
    if (out == nullptr) {
        return FrameStatus::kInvalidArgument;
    }
    if (format.magic == 0) {
        return FrameStatus::kInvalidArgument;
    }
    if (data == nullptr) {
        return size == 0 ? FrameStatus::kNeedMoreData : FrameStatus::kInvalidArgument;
    }
    return ParseOne(format, data, size, kFrameMaxPayload, out, consumed);
}

FrameStatus Decode(const FrameFormat& format, std::uint32_t max_payload,
                   const std::uint8_t* data, std::size_t size, Frame* out,
                   std::size_t* consumed) noexcept {
    if (out == nullptr) {
        return FrameStatus::kInvalidArgument;
    }
    if (format.magic == 0) {
        return FrameStatus::kInvalidArgument;
    }
    if (data == nullptr) {
        return size == 0 ? FrameStatus::kNeedMoreData : FrameStatus::kInvalidArgument;
    }
    return ParseOne(format, data, size, max_payload, out, consumed);
}

FrameStatus DecodeView(const std::uint8_t* data, std::size_t size, FrameView* out,
                       std::size_t* consumed) noexcept {
    if (out == nullptr) {
        return FrameStatus::kInvalidArgument;
    }
    if (data == nullptr) {
        return size == 0 ? FrameStatus::kNeedMoreData : FrameStatus::kInvalidArgument;
    }
    return ParseOneView(kDefaultFrameFormat, data, size, kFrameMaxPayload, out, consumed);
}

FrameStatus DecodeView(const FrameFormat& format, const std::uint8_t* data, std::size_t size,
                       FrameView* out, std::size_t* consumed) noexcept {
    if (out == nullptr) {
        return FrameStatus::kInvalidArgument;
    }
    if (format.magic == 0) {
        return FrameStatus::kInvalidArgument;
    }
    if (data == nullptr) {
        return size == 0 ? FrameStatus::kNeedMoreData : FrameStatus::kInvalidArgument;
    }
    return ParseOneView(format, data, size, kFrameMaxPayload, out, consumed);
}

FrameStatus DecodeView(const FrameFormat& format, std::uint32_t max_payload,
                       const std::uint8_t* data, std::size_t size, FrameView* out,
                       std::size_t* consumed) noexcept {
    if (out == nullptr) {
        return FrameStatus::kInvalidArgument;
    }
    if (format.magic == 0) {
        return FrameStatus::kInvalidArgument;
    }
    if (data == nullptr) {
        return size == 0 ? FrameStatus::kNeedMoreData : FrameStatus::kInvalidArgument;
    }
    return ParseOneView(format, data, size, max_payload, out, consumed);
}

FrameDecoder::FrameDecoder(std::uint32_t max_payload, FrameFormat format) noexcept
    : m_max_payload(max_payload), m_format(format) {}

void FrameDecoder::Append(const std::uint8_t* data, std::size_t size) {
    if (data == nullptr || size == 0) {
        return;
    }
    if (m_offset > 0) {
        // 只在有新数据时压缩一次，均摊 O(1)：解析阶段只推进 m_offset，
        // 不逐帧 erase，避免粘包场景下反复搬移。
        // 用 memmove + resize 而非 erase(begin, begin+offset)：后者对
        // vector 迭代器做移动赋值，实测每帧多出约 100 条指令，且对 POD
        // 字节类型没有额外语义。
        const std::size_t remaining = m_buffer.size() - m_offset;
        if (remaining != 0) {
            std::memmove(m_buffer.data(), m_buffer.data() + m_offset, remaining);
        }
        m_buffer.resize(remaining);
        m_offset = 0;
    }
    m_buffer.insert(m_buffer.end(), data, data + size);
}

void FrameDecoder::Consume(std::size_t consumed) noexcept {
    m_offset += consumed;
    if (m_offset == m_buffer.size()) {
        // 整段消费完：清空而不是保留容量，语义与旧实现一致（缓冲不再需要）。
        m_buffer.clear();
        m_offset = 0;
    }
}

FrameStatus FrameDecoder::Next(Frame* out) {
    if (out == nullptr) {
        return FrameStatus::kInvalidArgument;
    }
    const std::size_t available = m_buffer.size() - m_offset;
    if (available == 0) {
        return FrameStatus::kNeedMoreData;
    }
    std::size_t consumed = 0;
    const FrameStatus status = ParseOne(m_format, m_buffer.data() + m_offset, available,
                                        m_max_payload, out, &consumed);
    if (status == FrameStatus::kOk) {
        Consume(consumed);
        return status;
    }
    if (status != FrameStatus::kNeedMoreData) {
        // 帧边界已不可信：清空缓冲，由调用方关闭/重建连接（fail-fast）。
        Clear();
    }
    return status;
}

FrameStatus FrameDecoder::NextView(FrameView* out) {
    if (out == nullptr) {
        return FrameStatus::kInvalidArgument;
    }
    const std::size_t available = m_buffer.size() - m_offset;
    if (available == 0) {
        return FrameStatus::kNeedMoreData;
    }
    std::size_t consumed = 0;
    const FrameStatus status = ParseOneView(m_format, m_buffer.data() + m_offset, available,
                                            m_max_payload, out, &consumed);
    if (status == FrameStatus::kOk) {
        Consume(consumed);
        return status;
    }
    if (status != FrameStatus::kNeedMoreData) {
        Clear();
    }
    return status;
}

void FrameDecoder::Clear() noexcept {
    m_buffer.clear();
    m_offset = 0;
}

}  // namespace go2cpp::bento::net
