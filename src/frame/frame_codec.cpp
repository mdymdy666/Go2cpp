/**
 * @file frame_codec.cpp
 * @brief `go2cpp::frame::FrameCodec` 的实现（长度前缀帧 + 可选校验和 + 半包重组）。
 *
 * @details 关键实现取舍：
 *
 * 1. **两段式解析**：先把内部缓冲里的半包补全（阶段 1），缓冲为空后直接在调用方
 *    的输入上解析（阶段 2）。常见情况（输入里已有整帧）因此只做一次载荷拷贝
 *    （`Frame::payload` 本来就持有副本），不额外复制整帧。
 * 2. **缓冲区有界**：只有「帧头还没齐」或「帧不完整」的尾部才会进内部缓冲，
 *    且长度上限是 `FrameSpec::MaxFrameBytes()`。帧头一旦声明
 *    `length > max_payload` 就立刻报错，不会按声明值预分配内存。
 * 3. **错误即清空**：解码路径上任何畸形都意味着帧边界不可信，此时内部缓冲
 *    直接丢弃，调用方应断开连接（与 `bento::net::FrameDecoder` 的策略一致）。
 * 4. **异常边界**：`Encode`/`Feed` 只把 `std::bad_alloc` 转成 `kOutOfMemory`，
 *    其它异常（例如自定义 checksum 抛异常）按 C++ 惯例向调用方传播。
 *
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include "go2cpp/frame/frame_codec.hpp"

#include "go2cpp/hash/crc32.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>
#include <utility>

namespace go2cpp::frame {

namespace {

/// 读 2 字节小端。
std::uint16_t LoadLe16(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>(static_cast<unsigned>(p[0]) |
                                      (static_cast<unsigned>(p[1]) << 8));
}

/// 读 4 字节小端。
std::uint32_t LoadLe32(const std::uint8_t* p) noexcept {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

/// 写 2 字节小端。
void StoreLe16(std::uint8_t* p, std::uint16_t value) noexcept {
    p[0] = static_cast<std::uint8_t>(value & 0xFFU);
    p[1] = static_cast<std::uint8_t>((value >> 8) & 0xFFU);
}

/// 写 4 字节小端。
void StoreLe32(std::uint8_t* p, std::uint32_t value) noexcept {
    p[0] = static_cast<std::uint8_t>(value & 0xFFU);
    p[1] = static_cast<std::uint8_t>((value >> 8) & 0xFFU);
    p[2] = static_cast<std::uint8_t>((value >> 16) & 0xFFU);
    p[3] = static_cast<std::uint8_t>((value >> 24) & 0xFFU);
}

/// 把 4 字节格式化成 "52 44 52 41"，写进调用方给的 20 字节栈缓冲。
void FormatMagic(const std::uint8_t* magic, char (&out)[20]) noexcept {
    std::snprintf(out, sizeof(out), "%02x %02x %02x %02x", magic[0], magic[1],
                  magic[2], magic[3]);
}

/// 生成「spec 非法」的具体文本。
std::string DescribeInvalidSpec(const FrameSpec& spec) {
    char buffer[160] = {};
    if (spec.header_len < kMinHeaderLen || spec.header_len > kMaxHeaderLen) {
        std::snprintf(buffer, sizeof(buffer),
                      "invalid FrameSpec: header_len %d outside [%d, %d]",
                      spec.header_len, kMinHeaderLen, kMaxHeaderLen);
    } else {
        std::snprintf(buffer, sizeof(buffer),
                      "invalid FrameSpec: max_payload %d is negative",
                      spec.max_payload);
    }
    return std::string(buffer);
}

}  // namespace

std::uint32_t Crc32IeeeFrameChecksum(const std::uint8_t* data,
                                     std::size_t len) noexcept {
    // 复用 hash 模块的 slicing-by-8 实现；AsBytes(const T*, size) 只读视图。
    return ::go2cpp::hash::Crc32::ChecksumIEEE(
        ::go2cpp::hash::AsBytes(data, len));
}

const char* ErrorCodeName(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::kNone:
            return "none";
        case ErrorCode::kInvalidArgument:
            return "invalid-argument";
        case ErrorCode::kInvalidSpec:
            return "invalid-spec";
        case ErrorCode::kPayloadTooLarge:
            return "payload-too-large";
        case ErrorCode::kLengthExceeded:
            return "length-exceeded";
        case ErrorCode::kBadMagic:
            return "bad-magic";
        case ErrorCode::kBadVersion:
            return "bad-version";
        case ErrorCode::kChecksumMismatch:
            return "checksum-mismatch";
        case ErrorCode::kOutOfMemory:
            return "out-of-memory";
    }
    return "unknown";
}

FrameCodec::FrameCodec(FrameSpec spec) : m_spec(std::move(spec)) {}

void FrameCodec::SetError(Error* err, ErrorCode code, std::string message) {
    if (err == nullptr) {
        return;
    }
    err->code = code;
    err->message = std::move(message);
}

bool FrameCodec::Encode(const Frame& frame, std::vector<std::uint8_t>* out,
                        Error* err) const {
    if (err != nullptr) {
        err->Clear();
    }
    if (out == nullptr) {
        SetError(err, ErrorCode::kInvalidArgument, "out must not be null");
        return false;
    }
    out->clear();
    if (!m_spec.Valid()) {
        SetError(err, ErrorCode::kInvalidSpec, DescribeInvalidSpec(m_spec));
        return false;
    }
    const std::size_t payload_bytes = frame.payload.size();
    if (payload_bytes > static_cast<std::size_t>(m_spec.max_payload)) {
        char buffer[128] = {};
        std::snprintf(buffer, sizeof(buffer),
                      "payload %zu bytes exceeds max_payload %d", payload_bytes,
                      m_spec.max_payload);
        SetError(err, ErrorCode::kPayloadTooLarge, buffer);
        return false;
    }

    const std::size_t header_len = static_cast<std::size_t>(m_spec.header_len);
    const std::size_t total =
        header_len + payload_bytes + (m_spec.HasChecksum() ? kChecksumBytes : 0);
    try {
        out->resize(total);
    } catch (const std::bad_alloc&) {
        out->clear();
        SetError(err, ErrorCode::kOutOfMemory,
                 "out of memory while encoding frame");
        return false;
    }

    std::uint8_t* const bytes = out->data();
    std::memcpy(bytes, m_spec.magic.data(), kMagicBytes);
    bytes[4] = m_spec.version;
    bytes[5] = frame.type;
    StoreLe16(bytes + 6, frame.flags);
    StoreLe32(bytes + 8, static_cast<std::uint32_t>(payload_bytes));
    // 扩展区（header_len > 12 时）编码一律写 0；它仍参与校验和，解码端不解释。
    if (header_len > static_cast<std::size_t>(kMinHeaderLen)) {
        std::memset(bytes + kMinHeaderLen, 0,
                    header_len - static_cast<std::size_t>(kMinHeaderLen));
    }
    if (payload_bytes != 0) {
        std::memcpy(bytes + header_len, frame.payload.data(), payload_bytes);
    }
    if (m_spec.HasChecksum()) {
        const std::uint32_t checksum = m_spec.checksum(bytes, header_len + payload_bytes);
        StoreLe32(bytes + header_len + payload_bytes, checksum);
    }
    return true;
}

bool FrameCodec::Feed(const std::uint8_t* data, std::size_t len,
                      std::vector<Frame>* frames, std::size_t* consumed,
                      Error* err) {
    if (err != nullptr) {
        err->Clear();
    }
    if (consumed != nullptr) {
        *consumed = 0;
    }
    if (frames == nullptr || consumed == nullptr) {
        SetError(err, ErrorCode::kInvalidArgument,
                 "frames/consumed must not be null");
        return false;
    }
    if (data == nullptr && len != 0) {
        SetError(err, ErrorCode::kInvalidArgument,
                 "data is null but len != 0");
        return false;
    }
    if (!m_spec.Valid()) {
        SetError(err, ErrorCode::kInvalidSpec, DescribeInvalidSpec(m_spec));
        return false;
    }
    try {
        return FeedImpl(data, len, frames, consumed, err);
    } catch (const std::bad_alloc&) {
        m_buffer.clear();
        SetError(err, ErrorCode::kOutOfMemory,
                 "out of memory while decoding frame");
        return false;
    }
}

bool FrameCodec::FeedImpl(const std::uint8_t* data, std::size_t len,
                          std::vector<Frame>* frames, std::size_t* consumed,
                          Error* err) {
    const std::size_t header_len = static_cast<std::size_t>(m_spec.header_len);
    std::size_t pos = 0;
    // 显式 resize + memcpy 而不是 vector::insert(end, src, src+count)：
    // GCC 13 在 -O3 内联 insert 时会误报 -Wstringop-overflow（已知假阳性），
    // 而这里本来就知道目标区间与拷贝长度，直接写更快也更清楚。
    const auto append = [this](const std::uint8_t* src, std::size_t count) {
        if (count == 0) {
            return;
        }
        const std::size_t old_size = m_buffer.size();
        m_buffer.resize(old_size + count);
        std::memcpy(m_buffer.data() + old_size, src, count);
    };

    // 阶段 1：内部缓冲里有半包，先用本次输入把它补全（最多补出一帧）。
    while (!m_buffer.empty()) {
        if (m_buffer.size() < header_len) {
            const std::size_t take = std::min(header_len - m_buffer.size(), len - pos);
            if (take != 0) {
                append(data + pos, take);
            }
            pos += take;
            if (m_buffer.size() < header_len) {
                *consumed = pos;
                return true;  // 帧头仍不完整：继续等
            }
        }
        std::size_t frame_bytes = 0;
        if (!ValidateHeader(m_buffer.data(), &frame_bytes, err)) {
            m_buffer.clear();
            *consumed = pos;
            return false;
        }
        const std::size_t take = std::min(frame_bytes - m_buffer.size(), len - pos);
        if (take != 0) {
            append(data + pos, take);
        }
        pos += take;
        if (m_buffer.size() < frame_bytes) {
            *consumed = pos;
            return true;  // 半包
        }
        if (!ExtractFrame(m_buffer.data(), frame_bytes, frames, err)) {
            m_buffer.clear();
            *consumed = pos;
            return false;
        }
        m_buffer.clear();
    }

    // 阶段 2：缓冲为空，直接在输入上解析，直到剩下的是不完整尾部。
    for (;;) {
        const std::size_t remaining = len - pos;
        if (remaining < header_len) {
            if (remaining != 0) {
                append(data + pos, remaining);
            }
            pos = len;
            break;
        }
        std::size_t frame_bytes = 0;
        if (!ValidateHeader(data + pos, &frame_bytes, err)) {
            *consumed = pos;
            return false;
        }
        if (remaining < frame_bytes) {
            append(data + pos, remaining);
            pos = len;
            break;
        }
        if (!ExtractFrame(data + pos, frame_bytes, frames, err)) {
            *consumed = pos;
            return false;
        }
        pos += frame_bytes;
    }
    *consumed = pos;
    return true;
}

bool FrameCodec::ValidateHeader(const std::uint8_t* header,
                                std::size_t* frame_bytes, Error* err) const {
    if (std::memcmp(header, m_spec.magic.data(), kMagicBytes) != 0) {
        char got[20] = {};
        char want[20] = {};
        FormatMagic(header, got);
        FormatMagic(m_spec.magic.data(), want);
        SetError(err, ErrorCode::kBadMagic,
                 std::string("bad magic ") + got + ", want " + want);
        return false;
    }
    if (header[4] != m_spec.version) {
        char buffer[96] = {};
        std::snprintf(buffer, sizeof(buffer), "bad version %u, want %u",
                      static_cast<unsigned>(header[4]),
                      static_cast<unsigned>(m_spec.version));
        SetError(err, ErrorCode::kBadVersion, buffer);
        return false;
    }
    const std::uint32_t length = LoadLe32(header + 8);
    if (static_cast<std::uint64_t>(length) >
        static_cast<std::uint64_t>(m_spec.max_payload)) {
        char buffer[128] = {};
        std::snprintf(buffer, sizeof(buffer),
                      "length %u exceeds max_payload %d", length,
                      m_spec.max_payload);
        SetError(err, ErrorCode::kLengthExceeded, buffer);
        return false;
    }
    *frame_bytes = static_cast<std::size_t>(m_spec.header_len) +
                   static_cast<std::size_t>(length) +
                   (m_spec.HasChecksum() ? kChecksumBytes : 0);
    return true;
}

bool FrameCodec::ExtractFrame(const std::uint8_t* frame,
                              std::size_t frame_bytes,
                              std::vector<Frame>* frames, Error* err) const {
    const std::size_t header_len = static_cast<std::size_t>(m_spec.header_len);
    // 帧边界已由 ValidateHeader 保证，载荷长度直接从整帧字节数反推。
    const std::size_t payload_bytes =
        frame_bytes - header_len - (m_spec.HasChecksum() ? kChecksumBytes : 0);
    if (m_spec.HasChecksum()) {
        const std::uint32_t want = LoadLe32(frame + header_len + payload_bytes);
        const std::uint32_t got = m_spec.checksum(frame, header_len + payload_bytes);
        if (got != want) {
            char buffer[128] = {};
            std::snprintf(buffer, sizeof(buffer),
                          "checksum mismatch: got 0x%08x, want 0x%08x", got,
                          want);
            SetError(err, ErrorCode::kChecksumMismatch, buffer);
            return false;
        }
    }
    Frame out;
    out.type = frame[5];
    out.flags = LoadLe16(frame + 6);
    out.payload.assign(frame + header_len, frame + header_len + payload_bytes);
    frames->push_back(std::move(out));
    return true;
}

void FrameCodec::Reset() noexcept {
    // 交换空 vector：既丢弃半包，也把容量还给分配器（重连场景不保留大缓冲）。
    std::vector<std::uint8_t>().swap(m_buffer);
}

}  // namespace go2cpp::frame
