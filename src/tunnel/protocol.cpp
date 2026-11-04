/**
 * @file protocol.cpp
 * @brief 隧道协议载荷编解码 / 子域名校验与随机生成。
 *
 * @details 所有解码都走「先校验长度、再取值」的路径：载荷长度来自网络，
 *          绝不能用来直接 `resize()` 或做指针运算而不核对剩余字节数。
 *          编码失败一律清空输出缓冲，避免调用方把半个载荷发上线。
 *
 * @author go2cpp contributors
 * @date 2026-11-02
 * @copyright MIT License
 */

#include "go2cpp/tunnel/protocol.hpp"

#include "go2cpp/bento/net/crypto.hpp"
#include "go2cpp/hash/sha256.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace go2cpp {
namespace tunnel {
namespace {

/// 记录一次编码失败并返回 false（`err` 可为空）。
bool Fail(ErrorPtr* err, const char* message) {
    if (err != nullptr) {
        *err = NewError(message);
    }
    return false;
}

/// 小端写入器；容量固定为调用方传入的 vector。
class Writer final {
public:
    explicit Writer(std::vector<std::uint8_t>* out) : m_out(out) {}

    void U8(std::uint8_t value) { m_out->push_back(value); }
    void U16(std::uint16_t value) {
        m_out->push_back(static_cast<std::uint8_t>(value & 0xFFU));
        m_out->push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFU));
    }
    void U32(std::uint32_t value) {
        m_out->push_back(static_cast<std::uint8_t>(value & 0xFFU));
        m_out->push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFU));
        m_out->push_back(static_cast<std::uint8_t>((value >> 16) & 0xFFU));
        m_out->push_back(static_cast<std::uint8_t>((value >> 24) & 0xFFU));
    }
    void U64(std::uint64_t value) {
        for (int shift = 0; shift < 64; shift += 8) {
            m_out->push_back(
                static_cast<std::uint8_t>((value >> shift) & 0xFFU));
        }
    }
    void Bytes(const void* data, std::size_t length) {
        const auto* first = static_cast<const std::uint8_t*>(data);
        m_out->insert(m_out->end(), first, first + length);
    }
    /// 写入 `uint8` 长度前缀 + 字节；长度超过 255 时返回 false。
    bool Str8(const std::string& text) {
        if (text.size() > 255U) {
            return false;
        }
        U8(static_cast<std::uint8_t>(text.size()));
        Bytes(text.data(), text.size());
        return true;
    }
    /// 写入 `uint16` 长度前缀 + 字节；长度超过 65535 时返回 false。
    bool Str16(const std::string& text) {
        if (text.size() > 0xFFFFU) {
            return false;
        }
        U16(static_cast<std::uint16_t>(text.size()));
        Bytes(text.data(), text.size());
        return true;
    }

private:
    std::vector<std::uint8_t>* m_out;
};

/// 小端读取器；每次取数都先检查剩余字节，越界即失败。
class Reader final {
public:
    Reader(const std::uint8_t* data, std::size_t length)
        : m_data(data), m_length(length) {}

    bool U8(std::uint8_t* value) {
        if (Remaining() < 1U) {
            return false;
        }
        *value = m_data[m_offset++];
        return true;
    }
    bool U16(std::uint16_t* value) {
        if (Remaining() < 2U) {
            return false;
        }
        *value = static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(m_data[m_offset]) |
            (static_cast<std::uint16_t>(m_data[m_offset + 1]) << 8));
        m_offset += 2U;
        return true;
    }
    bool U32(std::uint32_t* value) {
        if (Remaining() < 4U) {
            return false;
        }
        *value = static_cast<std::uint32_t>(m_data[m_offset]) |
                 (static_cast<std::uint32_t>(m_data[m_offset + 1]) << 8) |
                 (static_cast<std::uint32_t>(m_data[m_offset + 2]) << 16) |
                 (static_cast<std::uint32_t>(m_data[m_offset + 3]) << 24);
        m_offset += 4U;
        return true;
    }
    bool U64(std::uint64_t* value) {
        if (Remaining() < 8U) {
            return false;
        }
        std::uint64_t result = 0;
        for (int shift = 0; shift < 64; shift += 8) {
            result |= static_cast<std::uint64_t>(m_data[m_offset++]) << shift;
        }
        *value = result;
        return true;
    }
    /// 读 `uint8` 长度前缀字符串。
    bool Str8(std::string* text) {
        std::uint8_t length = 0;
        if (!U8(&length)) {
            return false;
        }
        return Take(static_cast<std::size_t>(length), text);
    }
    /// 读 `uint16` 长度前缀字符串。
    bool Str16(std::string* text) {
        std::uint16_t length = 0;
        if (!U16(&length)) {
            return false;
        }
        return Take(static_cast<std::size_t>(length), text);
    }
    /// 取 `length` 字节原样拷贝成字符串。
    bool Take(std::size_t length, std::string* text) {
        if (Remaining() < length) {
            return false;
        }
        text->assign(reinterpret_cast<const char*>(m_data + m_offset), length);
        m_offset += length;
        return true;
    }

    std::size_t Remaining() const noexcept { return m_length - m_offset; }
    std::size_t offset() const noexcept { return m_offset; }

private:
    const std::uint8_t* m_data;
    std::size_t m_length;
    std::size_t m_offset = 0;
};

/// 解码后必须**恰好**消费完载荷：多余字节说明两端协议不一致。
bool ExpectExact(const Reader& reader, ErrorPtr* err) {
    if (reader.Remaining() != 0) {
        return Fail(err, "tunnel: trailing bytes in payload");
    }
    return true;
}

constexpr char kBase32[] = "abcdefghijklmnopqrstuvwxyz0123456789";

}  // namespace

const char* FrameTypeName(FrameType type) noexcept {
    switch (type) {
        case FrameType::kHello:
            return "HELLO";
        case FrameType::kHelloAck:
            return "HELLO_ACK";
        case FrameType::kOpen:
            return "OPEN";
        case FrameType::kData:
            return "DATA";
        case FrameType::kClose:
            return "CLOSE";
        case FrameType::kPing:
            return "PING";
        case FrameType::kPong:
            return "PONG";
        case FrameType::kChallenge:
            return "CHALLENGE";
        case FrameType::kAuth:
            return "AUTH";
        case FrameType::kGoAway:
            return "GOAWAY";
        case FrameType::kOpenRaw:
            return "OPEN_RAW";
        case FrameType::kRawBind:
            return "RAW_BIND";
    }
    return "UNKNOWN";
}

const char* StatusName(Status status) noexcept {
    switch (status) {
        case Status::kOk:
            return "ok";
        case Status::kRejected:
            return "rejected";
        case Status::kBadVersion:
            return "bad_version";
        case Status::kUnauthorized:
            return "unauthorized";
        case Status::kServerShutdown:
            return "server_shutdown";
    }
    return "unknown";
}

frame::FrameSpec MakeTunnelFrameSpec(bool with_checksum) {
    frame::FrameSpec spec =
        frame::MakeFrameSpec(frame::MakeMagic("TUNL"), 1,
                             static_cast<int>(kMaxPayload));
    if (!with_checksum) {
        spec.checksum = nullptr;
    }
    return spec;
}

// ============================================================================
// HELLO / HELLO_ACK
// ============================================================================

bool EncodeHello(const Hello& hello, std::vector<std::uint8_t>* out,
                 ErrorPtr* err) {
    if (out == nullptr) {
        return Fail(err, "tunnel: EncodeHello null output");
    }
    out->clear();
    if (hello.subdomain.size() > kMaxSubdomainLen) {
        return Fail(err, "tunnel: subdomain too long");
    }
    if (!hello.subdomain.empty() && !IsValidSubdomain(hello.subdomain)) {
        return Fail(err, "tunnel: invalid subdomain");
    }
    if (hello.token.size() > kMaxTokenLen) {
        return Fail(err, "tunnel: token too long");
    }
    Writer writer(out);
    writer.U16(hello.version);
    writer.U32(hello.flags);
    if (!writer.Str8(hello.subdomain)) {
        out->clear();
        return Fail(err, "tunnel: subdomain too long");
    }
    writer.U16(hello.local_port);
    if (!writer.Str16(hello.token)) {
        out->clear();
        return Fail(err, "tunnel: token too long");
    }
    return true;
}

bool DecodeHello(const std::uint8_t* data, std::size_t length, Hello* out,
                 ErrorPtr* err) {
    if (out == nullptr || (data == nullptr && length != 0)) {
        return Fail(err, "tunnel: DecodeHello invalid argument");
    }
    Reader reader(data, length);
    Hello hello;
    if (!reader.U16(&hello.version) || !reader.U32(&hello.flags) ||
        !reader.Str8(&hello.subdomain) || !reader.U16(&hello.local_port) ||
        !reader.Str16(&hello.token)) {
        return Fail(err, "tunnel: truncated HELLO");
    }
    if (!ExpectExact(reader, err)) {
        return false;
    }
    if (hello.subdomain.size() > kMaxSubdomainLen) {
        return Fail(err, "tunnel: HELLO subdomain too long");
    }
    if (hello.token.size() > kMaxTokenLen) {
        return Fail(err, "tunnel: HELLO token too long");
    }
    *out = std::move(hello);
    return true;
}

bool EncodeHelloAck(const HelloAck& ack, std::vector<std::uint8_t>* out,
                    ErrorPtr* err) {
    if (out == nullptr) {
        return Fail(err, "tunnel: EncodeHelloAck null output");
    }
    out->clear();
    Writer writer(out);
    writer.U8(static_cast<std::uint8_t>(ack.status));
    if (!writer.Str8(ack.subdomain)) {
        out->clear();
        return Fail(err, "tunnel: ack subdomain too long");
    }
    if (!writer.Str16(ack.public_url)) {
        out->clear();
        return Fail(err, "tunnel: ack url too long");
    }
    if (!writer.Str16(ack.message)) {
        out->clear();
        return Fail(err, "tunnel: ack message too long");
    }
    return true;
}

bool DecodeHelloAck(const std::uint8_t* data, std::size_t length,
                    HelloAck* out, ErrorPtr* err) {
    if (out == nullptr || (data == nullptr && length != 0)) {
        return Fail(err, "tunnel: DecodeHelloAck invalid argument");
    }
    Reader reader(data, length);
    std::uint8_t status = 0;
    HelloAck ack;
    if (!reader.U8(&status) || !reader.Str8(&ack.subdomain) ||
        !reader.Str16(&ack.public_url) || !reader.Str16(&ack.message)) {
        return Fail(err, "tunnel: truncated HELLO_ACK");
    }
    if (!ExpectExact(reader, err)) {
        return false;
    }
    ack.status = static_cast<Status>(status);
    *out = std::move(ack);
    return true;
}

// ============================================================================
// OPEN / DATA / CLOSE
// ============================================================================

bool EncodeOpen(const Open& open, std::vector<std::uint8_t>* out,
                ErrorPtr* err) {
    if (out == nullptr) {
        return Fail(err, "tunnel: EncodeOpen null output");
    }
    out->clear();
    if (open.peer_ip.size() > 255U) {
        return Fail(err, "tunnel: peer ip too long");
    }
    Writer writer(out);
    writer.U32(open.stream_id);
    if (!writer.Str8(open.peer_ip)) {
        out->clear();
        return Fail(err, "tunnel: peer ip too long");
    }
    writer.U16(open.peer_port);
    return true;
}

bool DecodeOpen(const std::uint8_t* data, std::size_t length, Open* out,
                ErrorPtr* err) {
    if (out == nullptr || (data == nullptr && length != 0)) {
        return Fail(err, "tunnel: DecodeOpen invalid argument");
    }
    Reader reader(data, length);
    Open open;
    if (!reader.U32(&open.stream_id) || !reader.Str8(&open.peer_ip) ||
        !reader.U16(&open.peer_port)) {
        return Fail(err, "tunnel: truncated OPEN");
    }
    if (!ExpectExact(reader, err)) {
        return false;
    }
    *out = std::move(open);
    return true;
}

bool EncodeData(std::uint32_t stream_id, const std::uint8_t* body,
                std::size_t length, std::vector<std::uint8_t>* out,
                ErrorPtr* err) {
    if (out == nullptr) {
        return Fail(err, "tunnel: EncodeData null output");
    }
    out->clear();
    if (length > kMaxDataChunk) {
        return Fail(err, "tunnel: DATA chunk too large");
    }
    if (body == nullptr && length != 0) {
        return Fail(err, "tunnel: EncodeData null body");
    }
    Writer writer(out);
    writer.U32(stream_id);
    if (length != 0) {
        writer.Bytes(body, length);
    }
    return true;
}

bool DecodeData(const std::uint8_t* data, std::size_t length, DataFrame* out,
                ErrorPtr* err) {
    if (out == nullptr || (data == nullptr && length != 0)) {
        return Fail(err, "tunnel: DecodeData invalid argument");
    }
    if (length < 4U) {
        return Fail(err, "tunnel: truncated DATA");
    }
    out->stream_id = static_cast<std::uint32_t>(data[0]) |
                     (static_cast<std::uint32_t>(data[1]) << 8) |
                     (static_cast<std::uint32_t>(data[2]) << 16) |
                     (static_cast<std::uint32_t>(data[3]) << 24);
    out->body = data + 4;
    out->length = length - 4U;
    return true;
}

bool EncodeClose(const Close& close, std::vector<std::uint8_t>* out,
                 ErrorPtr* err) {
    if (out == nullptr) {
        return Fail(err, "tunnel: EncodeClose null output");
    }
    out->clear();
    Writer writer(out);
    writer.U32(close.stream_id);
    writer.U8(close.code);
    return true;
}

bool DecodeClose(const std::uint8_t* data, std::size_t length, Close* out,
                 ErrorPtr* err) {
    if (out == nullptr || (data == nullptr && length != 0)) {
        return Fail(err, "tunnel: DecodeClose invalid argument");
    }
    Reader reader(data, length);
    std::uint8_t code = 0;
    Close close;
    if (!reader.U32(&close.stream_id) || !reader.U8(&code)) {
        return Fail(err, "tunnel: truncated CLOSE");
    }
    if (!ExpectExact(reader, err)) {
        return false;
    }
    close.code = code;
    *out = close;
    return true;
}

std::vector<std::uint8_t> EncodeNonce(std::uint64_t nonce) {
    std::vector<std::uint8_t> out;
    out.reserve(8);
    for (int shift = 0; shift < 64; shift += 8) {
        out.push_back(static_cast<std::uint8_t>((nonce >> shift) & 0xFFU));
    }
    return out;
}

bool DecodeNonce(const std::uint8_t* data, std::size_t length,
                 std::uint64_t* out) {
    if (out == nullptr || data == nullptr || length != 8U) {
        return false;
    }
    std::uint64_t value = 0;
    for (int shift = 0; shift < 64; shift += 8) {
        value |= static_cast<std::uint64_t>(data[shift / 8]) << shift;
    }
    *out = value;
    return true;
}

// ============================================================================
// 子域名
// ============================================================================

// ============================================================================
// 鉴权（PSK + ChaCha20-Poly1305，复用 bento::net::crypto）
// ============================================================================

Psk DerivePsk(const std::string& secret) {
    Psk psk{};
    const auto digest = go2cpp::hash::Sha256::Digest(go2cpp::hash::ByteSpan(
        reinterpret_cast<std::uint8_t*>(const_cast<char*>(secret.data())),
        secret.size()));
    std::memcpy(psk.data(), digest.data(), psk.size());
    return psk;
}

std::vector<std::uint8_t> MakeChallenge() {
    std::vector<std::uint8_t> challenge(kChallengeBytes, 0);
    try {
        bento::SecureRandomBytes(challenge.data(), challenge.size());
    } catch (...) {
        challenge.clear();
    }
    return challenge;
}

namespace {

/// 把 `psk` 派生成本方向的挑战应答密钥。
void DeriveAuthKey(const Psk& psk, std::uint8_t key[32]) {
    bento::DeriveHandshakeKey(psk.data(), bento::SecureDirection::kClientToServer,
                              key);
}

}  // namespace

bool ComputeAuthTag(const Psk& psk, const std::uint8_t* challenge,
                    std::uint8_t* tag) {
    if (challenge == nullptr || tag == nullptr) {
        return false;
    }
    std::uint8_t key[32] = {};
    DeriveAuthKey(psk, key);
    std::uint8_t nonce[bento::kChaCha20NonceBytes] = {};
    std::memcpy(nonce, challenge, sizeof(nonce));
    // 空明文 + challenge 作为 AAD：tag 唯一绑定这 16 字节挑战。
    const std::uint8_t empty = 0;
    bento::AeadSeal(key, nonce, challenge, kChallengeBytes, &empty, 0,
                    nullptr, tag);
    bento::SecureZero(key, sizeof(key));
    return true;
}

bool VerifyAuthTag(const Psk& psk, const std::uint8_t* challenge,
                   const std::uint8_t* tag) {
    if (challenge == nullptr || tag == nullptr) {
        return false;
    }
    std::uint8_t key[32] = {};
    DeriveAuthKey(psk, key);
    std::uint8_t nonce[bento::kChaCha20NonceBytes] = {};
    std::memcpy(nonce, challenge, sizeof(nonce));
    std::uint8_t empty = 0;
    const bool ok = bento::AeadOpen(key, nonce, challenge, kChallengeBytes, &empty,
                                    0, tag, nullptr);
    bento::SecureZero(key, sizeof(key));
    return ok;
}

std::vector<std::uint8_t> EncodeChallenge(const std::uint8_t* challenge) {
    if (challenge == nullptr) {
        return {};
    }
    return std::vector<std::uint8_t>(challenge, challenge + kChallengeBytes);
}

bool DecodeChallenge(const std::uint8_t* data, std::size_t length,
                     std::uint8_t* out) {
    if (data == nullptr || out == nullptr || length != kChallengeBytes) {
        return false;
    }
    std::memcpy(out, data, kChallengeBytes);
    return true;
}

std::vector<std::uint8_t> EncodeAuth(const std::uint8_t* tag) {
    if (tag == nullptr) {
        return {};
    }
    return std::vector<std::uint8_t>(tag, tag + kAuthTagBytes);
}

bool DecodeAuth(const std::uint8_t* data, std::size_t length,
                std::uint8_t* out) {
    if (data == nullptr || out == nullptr || length != kAuthTagBytes) {
        return false;
    }
    std::memcpy(out, data, kAuthTagBytes);
    return true;
}

bool EncodeGoAway(const GoAway& goaway, std::vector<std::uint8_t>* out,
                  ErrorPtr* err) {
    if (out == nullptr) {
        return Fail(err, "tunnel: EncodeGoAway null output");
    }
    out->clear();
    Writer writer(out);
    writer.U8(goaway.reason);
    writer.U32(goaway.retry_after_ms);
    return true;
}

bool DecodeGoAway(const std::uint8_t* data, std::size_t length, GoAway* out,
                  ErrorPtr* err) {
    if (out == nullptr || (data == nullptr && length != 0)) {
        return Fail(err, "tunnel: DecodeGoAway invalid argument");
    }
    Reader reader(data, length);
    std::uint8_t reason = 0;
    GoAway goaway;
    if (!reader.U8(&reason) || !reader.U32(&goaway.retry_after_ms)) {
        return Fail(err, "tunnel: truncated GOAWAY");
    }
    if (!ExpectExact(reader, err)) {
        return false;
    }
    goaway.reason = reason;
    *out = goaway;
    return true;
}

bool IsValidSubdomain(const std::string& subdomain) noexcept {
    if (subdomain.empty() || subdomain.size() > kMaxSubdomainLen) {
        return false;
    }
    if (subdomain.front() == '-' || subdomain.back() == '-') {
        return false;
    }
    for (char character : subdomain) {
        const bool lower = character >= 'a' && character <= 'z';
        const bool digit = character >= '0' && character <= '9';
        if (!lower && !digit && character != '-') {
            return false;
        }
    }
    return true;
}

std::string MakeRandomSubdomain(std::size_t length) {
    if (length < 4U) {
        length = 4U;
    }
    if (length > 32U) {
        length = 32U;
    }
    std::string result;
    result.reserve(length);
    try {
        std::random_device device;
        std::mt19937_64 engine(static_cast<std::uint64_t>(device()) ^
                               static_cast<std::uint64_t>(
                                   std::chrono::steady_clock::now()
                                       .time_since_epoch()
                                       .count()));
        std::uniform_int_distribution<std::size_t> pick(0, sizeof(kBase32) - 2U);
        for (std::size_t index = 0; index < length; ++index) {
            result.push_back(kBase32[pick(engine)]);
        }
    } catch (...) {
        result.clear();
    }
    if (result.empty()) {
        const auto stamp = static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        for (std::size_t index = 0; index < length; ++index) {
            result.push_back(kBase32[(stamp >> (index * 5U)) % 36U]);
        }
    }
    return result;
}

bool RawStreamEnabledFromEnv() {
    const char* raw = std::getenv("GO2CPP_TUNNEL_RAW_STREAM");
    if (raw == nullptr) {
        return false;
    }
    std::string value(raw);
    for (char& ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value == "1" || value == "true" || value == "on" ||
           value == "yes";
}

}  // namespace tunnel
}  // namespace go2cpp
