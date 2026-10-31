/**
 * @file coap.cpp
 * @brief CoAP（RFC 7252）最小骨架的实现：报文/选项编解码 + 客户端重传 +
 *        服务端去重与响应组装。
 * @author go2cpp contributors
 * @date 2026-10-30
 * @copyright MIT License
 */

#include "go2cpp/bento/net/coap.hpp"

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <random>
#include <utility>

namespace go2cpp::bento::net {
namespace {

/// 选项 delta/length 的 4 bit 字段取值：0..12 直接表示。
constexpr std::uint8_t kNibbleDirect = 12;
/// 13 表示后续 1 字节扩展（实际值 = 13 + 扩展字节）。
constexpr std::uint8_t kNibbleOneByte = 13;
/// 14 表示后续 2 字节扩展（实际值 = 269 + 扩展值）。
constexpr std::uint8_t kNibbleTwoByte = 14;
/// 15 是保留值；整个字节为 0xFF 时才是 Payload Marker。
constexpr std::uint8_t kNibbleReserved = 15;
/// 重传次数上限：防止 `1 << n` 溢出，也避免配置成无限重试。
constexpr unsigned kMaxRetransmitLimit = 16;

/// @brief 返回某个 delta/length 值对应的 4 bit 前缀。
std::uint8_t OptionNibble(std::uint32_t value) noexcept {
    if (value <= kNibbleDirect) {
        return static_cast<std::uint8_t>(value);
    }
    if (value <= 268) {
        return kNibbleOneByte;
    }
    return kNibbleTwoByte;
}

/// @brief 追加 delta/length 的扩展字节（不含 4 bit 前缀本身）。
void AppendOptionExtension(std::vector<std::uint8_t>* out, std::uint32_t value) {
    if (value <= kNibbleDirect) {
        return;
    }
    if (value <= 268) {
        out->push_back(static_cast<std::uint8_t>(value - 13));
        return;
    }
    const std::uint32_t extended = value - 269;
    out->push_back(static_cast<std::uint8_t>(extended >> 8));
    out->push_back(static_cast<std::uint8_t>(extended & 0xffu));
}

/// @brief 选项前缀字节数（1 字节前缀 + 扩展字节）。
std::size_t OptionPrefixBytes(std::uint32_t value) noexcept {
    if (value <= kNibbleDirect) {
        return 1;
    }
    if (value <= 268) {
        return 2;
    }
    return 3;
}

/**
 * @brief 从 `offset` 处解码一个选项（调用方已确认该字节不是 Payload Marker）。
 *
 * @param[in,out] offset 读取位置，成功时前进到选项值之后。
 * @param[in,out] number 上一个选项编号；成功时更新为当前选项编号。
 */
CoapStatus DecodeOptionAt(const std::uint8_t* data, std::size_t size,
                          std::size_t* offset, std::uint16_t* number,
                          CoapOption* option) {
    if (*offset >= size) {
        return CoapStatus::kTruncated;
    }
    const std::uint8_t header = data[(*offset)++];
    const std::uint8_t delta_nibble = static_cast<std::uint8_t>(header >> 4);
    const std::uint8_t length_nibble = static_cast<std::uint8_t>(header & 0x0fu);
    if (delta_nibble == kNibbleReserved) {
        return CoapStatus::kBadOptionDelta;
    }
    if (length_nibble == kNibbleReserved) {
        return CoapStatus::kBadOptionLength;
    }

    std::uint32_t delta = delta_nibble;
    if (delta_nibble == kNibbleOneByte) {
        if (*offset >= size) {
            return CoapStatus::kTruncated;
        }
        delta = 13u + data[(*offset)++];
    } else if (delta_nibble == kNibbleTwoByte) {
        if (*offset + 2 > size) {
            return CoapStatus::kTruncated;
        }
        delta = 269u + (static_cast<std::uint32_t>(data[*offset]) << 8) +
                data[*offset + 1];
        *offset += 2;
    }

    std::uint32_t length = length_nibble;
    if (length_nibble == kNibbleOneByte) {
        if (*offset >= size) {
            return CoapStatus::kTruncated;
        }
        length = 13u + data[(*offset)++];
    } else if (length_nibble == kNibbleTwoByte) {
        if (*offset + 2 > size) {
            return CoapStatus::kTruncated;
        }
        length = 269u + (static_cast<std::uint32_t>(data[*offset]) << 8) +
                 data[*offset + 1];
        *offset += 2;
    }

    if (length > size - *offset) {
        return CoapStatus::kTruncated;
    }
    const std::uint32_t next_number = static_cast<std::uint32_t>(*number) + delta;
    if (next_number > kCoapMaxOptionNumber) {
        return CoapStatus::kBadOptionDelta;
    }
    *number = static_cast<std::uint16_t>(next_number);
    option->number = *number;
    option->value.assign(data + *offset, data + *offset + length);
    *offset += length;
    return CoapStatus::kOk;
}

}  // namespace

// ---------------------------------------------------------------------------
// 名称
// ---------------------------------------------------------------------------

const char* CoapTypeName(CoapType type) noexcept {
    switch (type) {
        case CoapType::kCon:
            return "CON";
        case CoapType::kNon:
            return "NON";
        case CoapType::kAck:
            return "ACK";
        case CoapType::kRst:
            return "RST";
    }
    return "unknown";
}

const char* CoapCodeName(CoapCode code) noexcept {
    switch (code) {
        case CoapCode::kEmpty:
            return "0.00 Empty";
        case CoapCode::kGet:
            return "0.01 GET";
        case CoapCode::kPost:
            return "0.02 POST";
        case CoapCode::kPut:
            return "0.03 PUT";
        case CoapCode::kDelete:
            return "0.04 DELETE";
        case CoapCode::kCreated:
            return "2.01 Created";
        case CoapCode::kDeleted:
            return "2.02 Deleted";
        case CoapCode::kValid:
            return "2.03 Valid";
        case CoapCode::kChanged:
            return "2.04 Changed";
        case CoapCode::kContent:
            return "2.05 Content";
        case CoapCode::kBadRequest:
            return "4.00 Bad Request";
        case CoapCode::kUnauthorized:
            return "4.01 Unauthorized";
        case CoapCode::kBadOption:
            return "4.02 Bad Option";
        case CoapCode::kForbidden:
            return "4.03 Forbidden";
        case CoapCode::kNotFound:
            return "4.04 Not Found";
        case CoapCode::kMethodNotAllowed:
            return "4.05 Method Not Allowed";
        case CoapCode::kNotAcceptable:
            return "4.06 Not Acceptable";
        case CoapCode::kPreconditionFailed:
            return "4.12 Precondition Failed";
        case CoapCode::kRequestEntityTooLarge:
            return "4.13 Request Entity Too Large";
        case CoapCode::kUnsupportedContentFormat:
            return "4.15 Unsupported Content-Format";
        case CoapCode::kInternalServerError:
            return "5.00 Internal Server Error";
        case CoapCode::kNotImplemented:
            return "5.01 Not Implemented";
        case CoapCode::kBadGateway:
            return "5.02 Bad Gateway";
        case CoapCode::kServiceUnavailable:
            return "5.03 Service Unavailable";
        case CoapCode::kGatewayTimeout:
            return "5.04 Gateway Timeout";
        case CoapCode::kProxyingNotSupported:
            return "5.05 Proxying Not Supported";
    }
    return "unknown";
}

const char* CoapStatusName(CoapStatus status) noexcept {
    switch (status) {
        case CoapStatus::kOk:
            return "ok";
        case CoapStatus::kTruncated:
            return "truncated";
        case CoapStatus::kBadVersion:
            return "bad-version";
        case CoapStatus::kBadTokenLength:
            return "bad-token-length";
        case CoapStatus::kBadOptionDelta:
            return "bad-option-delta";
        case CoapStatus::kBadOptionLength:
            return "bad-option-length";
        case CoapStatus::kPayloadMarkerWithoutPayload:
            return "payload-marker-without-payload";
        case CoapStatus::kMessageTooLarge:
            return "message-too-large";
        case CoapStatus::kInvalidArgument:
            return "invalid-argument";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// 选项与消息辅助
// ---------------------------------------------------------------------------

CoapOption CoapOption::FromString(std::uint16_t number, std::string_view text) {
    CoapOption option;
    option.number = number;
    option.value.reserve(text.size());
    for (const char byte : text) {
        option.value.push_back(static_cast<std::uint8_t>(byte));
    }
    return option;
}

CoapOption CoapOption::FromUint(std::uint16_t number, std::uint32_t value) {
    CoapOption option;
    option.number = number;
    std::size_t bytes = 0;
    for (std::uint32_t remaining = value; remaining != 0; remaining >>= 8) {
        ++bytes;
    }
    option.value.reserve(bytes);
    for (std::size_t index = 0; index < bytes; ++index) {
        const std::size_t shift = (bytes - 1 - index) * 8;
        option.value.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffu));
    }
    return option;
}

std::string CoapOption::AsString() const {
    std::string text;
    text.reserve(value.size());
    for (const std::uint8_t byte : value) {
        text.push_back(static_cast<char>(byte));
    }
    return text;
}

std::optional<std::uint32_t> CoapOption::AsUint() const {
    if (value.size() > 4) {
        return std::nullopt;
    }
    std::uint32_t result = 0;
    for (const std::uint8_t byte : value) {
        result = (result << 8) | byte;
    }
    return result;
}

bool CoapOption::operator==(const CoapOption& other) const {
    return number == other.number && value == other.value;
}

bool CoapOption::operator!=(const CoapOption& other) const { return !(*this == other); }

void CoapMessage::AddOption(std::uint16_t number, std::vector<std::uint8_t> value) {
    options.push_back(CoapOption{number, std::move(value)});
}

void CoapMessage::AddStringOption(std::uint16_t number, std::string_view text) {
    options.push_back(CoapOption::FromString(number, text));
}

void CoapMessage::AddUintOption(std::uint16_t number, std::uint32_t value) {
    options.push_back(CoapOption::FromUint(number, value));
}

void CoapMessage::AddUriPath(std::string_view path) {
    std::size_t begin = 0;
    if (!path.empty() && path.front() == '/') {
        begin = 1;
    }
    while (begin < path.size()) {
        const std::size_t slash = path.find('/', begin);
        const std::size_t end = slash == std::string_view::npos ? path.size() : slash;
        if (end > begin) {
            AddStringOption(kCoapOptionUriPath, path.substr(begin, end - begin));
        }
        if (slash == std::string_view::npos) {
            break;
        }
        begin = slash + 1;
    }
}

void CoapMessage::AddContentFormat(std::uint16_t content_format) {
    AddUintOption(kCoapOptionContentFormat, content_format);
}

const CoapOption* CoapMessage::FindOption(std::uint16_t number) const noexcept {
    for (const CoapOption& option : options) {
        if (option.number == number) {
            return &option;
        }
    }
    return nullptr;
}

std::string CoapMessage::UriPath() const {
    std::string path;
    for (const CoapOption& option : options) {
        if (option.number != kCoapOptionUriPath) {
            continue;
        }
        path.push_back('/');
        path += option.AsString();
    }
    if (path.empty()) {
        return "/";
    }
    return path;
}

std::optional<std::uint16_t> CoapMessage::ContentFormat() const {
    const CoapOption* option = FindOption(kCoapOptionContentFormat);
    if (option == nullptr) {
        return std::nullopt;
    }
    const std::optional<std::uint32_t> value = option->AsUint();
    if (!value.has_value() || *value > 0xffffu) {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>(*value);
}

// ---------------------------------------------------------------------------
// 报文编解码
// ---------------------------------------------------------------------------

std::vector<std::uint8_t> EncodeCoapMessage(const CoapMessage& message,
                                            CoapStatus* status) {
    const auto fail = [status](CoapStatus value) {
        if (status != nullptr) {
            *status = value;
        }
        return std::vector<std::uint8_t>{};
    };

    if (message.token.size() > kCoapMaxTokenBytes) {
        return fail(CoapStatus::kBadTokenLength);
    }
    if (static_cast<std::uint8_t>(message.type) > 3u) {
        return fail(CoapStatus::kInvalidArgument);
    }

    // 稳定排序：同编号选项（Uri-Path 多段）保持调用方顺序。
    std::vector<CoapOption> options = message.options;
    std::stable_sort(options.begin(), options.end(),
                     [](const CoapOption& left, const CoapOption& right) {
                         return left.number < right.number;
                     });

    std::uint64_t total = 4 + message.token.size();
    std::uint32_t previous_number = 0;
    for (const CoapOption& option : options) {
        if (option.value.size() > kCoapMaxOptionValueBytes) {
            return fail(CoapStatus::kInvalidArgument);
        }
        const std::uint32_t delta = static_cast<std::uint32_t>(option.number) -
                                    previous_number;
        previous_number = option.number;
        const std::uint32_t length = static_cast<std::uint32_t>(option.value.size());
        total += OptionPrefixBytes(delta) + OptionPrefixBytes(length) + length;
    }
    if (!message.payload.empty()) {
        total += 1 + message.payload.size();
    }
    if (total > kCoapMaxMessageBytes) {
        return fail(CoapStatus::kMessageTooLarge);
    }

    std::vector<std::uint8_t> wire;
    wire.reserve(static_cast<std::size_t>(total));
    const auto token_length = static_cast<std::uint8_t>(message.token.size());
    const auto type_bits =
        static_cast<std::uint8_t>(static_cast<std::uint8_t>(message.type) << 4);
    wire.push_back(
        static_cast<std::uint8_t>((kCoapVersion << 6) | type_bits | token_length));
    wire.push_back(static_cast<std::uint8_t>(message.code));
    wire.push_back(static_cast<std::uint8_t>(message.message_id >> 8));
    wire.push_back(static_cast<std::uint8_t>(message.message_id & 0xffu));
    wire.insert(wire.end(), message.token.begin(), message.token.end());

    previous_number = 0;
    for (const CoapOption& option : options) {
        const std::uint32_t delta = static_cast<std::uint32_t>(option.number) -
                                    previous_number;
        previous_number = option.number;
        const std::uint32_t length = static_cast<std::uint32_t>(option.value.size());
        wire.push_back(static_cast<std::uint8_t>((OptionNibble(delta) << 4) |
                                                 OptionNibble(length)));
        AppendOptionExtension(&wire, delta);
        AppendOptionExtension(&wire, length);
        wire.insert(wire.end(), option.value.begin(), option.value.end());
    }

    if (!message.payload.empty()) {
        wire.push_back(0xffu);
        wire.insert(wire.end(), message.payload.begin(), message.payload.end());
    }
    if (status != nullptr) {
        *status = CoapStatus::kOk;
    }
    return wire;
}

CoapStatus DecodeCoapMessage(const std::uint8_t* data, std::size_t size,
                             CoapMessage* message) {
    if (data == nullptr || message == nullptr) {
        return CoapStatus::kInvalidArgument;
    }
    if (size > kCoapMaxMessageBytes) {
        return CoapStatus::kMessageTooLarge;
    }
    if (size < 4) {
        return CoapStatus::kTruncated;
    }
    const std::uint8_t first = data[0];
    if ((first >> 6) != kCoapVersion) {
        return CoapStatus::kBadVersion;
    }
    const auto token_length = static_cast<std::size_t>(first & 0x0fu);
    if (token_length > kCoapMaxTokenBytes) {
        return CoapStatus::kBadTokenLength;
    }
    if (4 + token_length > size) {
        return CoapStatus::kTruncated;
    }

    CoapMessage decoded;
    decoded.type = static_cast<CoapType>((first >> 4) & 0x03u);
    decoded.code = static_cast<CoapCode>(data[1]);
    decoded.message_id = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(data[2]) << 8) |
        static_cast<std::uint16_t>(data[3]));
    decoded.token.assign(data + 4, data + 4 + token_length);

    std::size_t offset = 4 + token_length;
    std::uint16_t number = 0;
    while (offset < size) {
        if (data[offset] == 0xffu) {
            ++offset;
            if (offset >= size) {
                return CoapStatus::kPayloadMarkerWithoutPayload;
            }
            decoded.payload.assign(data + offset, data + size);
            offset = size;
            break;
        }
        CoapOption option;
        const CoapStatus status = DecodeOptionAt(data, size, &offset, &number, &option);
        if (status != CoapStatus::kOk) {
            return status;
        }
        decoded.options.push_back(std::move(option));
    }

    *message = std::move(decoded);
    return CoapStatus::kOk;
}

CoapStatus DecodeCoapMessage(const std::vector<std::uint8_t>& bytes,
                             CoapMessage* message) {
    // 空 vector 的 data() 可能是 nullptr；这里固定成「零长度数据报」语义，
    // 返回 kTruncated，而不是随实现变化的 kInvalidArgument。
    const std::uint8_t empty = 0;
    return DecodeCoapMessage(bytes.empty() ? &empty : bytes.data(), bytes.size(),
                             message);
}

std::uint64_t CoapNowMs() noexcept {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

// ---------------------------------------------------------------------------
// 响应快捷构造
// ---------------------------------------------------------------------------

CoapResponse CoapResponse::Content(std::vector<std::uint8_t> payload,
                                   std::uint16_t content_format) {
    CoapResponse response;
    response.code = CoapCode::kContent;
    response.content_format = content_format;
    response.payload = std::move(payload);
    return response;
}

CoapResponse CoapResponse::Changed() {
    CoapResponse response;
    response.code = CoapCode::kChanged;
    return response;
}

CoapResponse CoapResponse::Created() {
    CoapResponse response;
    response.code = CoapCode::kCreated;
    return response;
}

// ---------------------------------------------------------------------------
// 客户端
// ---------------------------------------------------------------------------

std::uint64_t CoapDefaultRandomSeed() noexcept {
    // 进程内序号：同一毫秒内并发构造多个客户端也能拿到不同种子。
    static std::atomic<std::uint64_t> sequence{0};
    std::uint64_t entropy = 0;
    try {
        std::random_device device;
        entropy = (static_cast<std::uint64_t>(device()) << 32) ^
                  static_cast<std::uint64_t>(device());
    } catch (...) {
        // 没有可用熵源时退化为时间 + pid + 序号，仍然保证实例间不同。
    }
    const std::uint64_t now_ns = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const std::uint64_t pid = static_cast<std::uint64_t>(::getpid());
    const std::uint64_t seq = sequence.fetch_add(1, std::memory_order_relaxed);
    const std::uint64_t seed =
        entropy ^ now_ns ^ (pid << 32) ^ (seq * 0x9e3779b97f4a7c15ull);
    // 0 保留给「自动种子」，避免与默认构造的语义冲突。
    return seed == 0 ? 0x9e3779b97f4a7c15ull : seed;
}

namespace {
/// 客户端种子解析：0 = 自动生成，非 0 = 调用方显式注入（可复现）。
std::uint64_t ResolveClientSeed(std::uint64_t seed) noexcept {
    return seed == 0 ? CoapDefaultRandomSeed() : seed;
}
}  // namespace

CoapClient::CoapClient(CoapSendFn send, CoapClientConfig config)
    : m_send(std::move(send)), m_config(config),
      m_rng_state(ResolveClientSeed(config.random_seed)) {
    if (m_config.ack_timeout_ms == 0) {
        m_config.ack_timeout_ms = 1;
    }
    if (m_config.max_retransmit > kMaxRetransmitLimit) {
        m_config.max_retransmit = kMaxRetransmitLimit;
    }
    if (m_config.token_bytes == 0) {
        m_config.token_bytes = 1;
    }
    if (m_config.token_bytes > kCoapMaxTokenBytes) {
        m_config.token_bytes = kCoapMaxTokenBytes;
    }
    m_next_message_id = static_cast<std::uint16_t>(NextRandom());
}

void CoapClient::SetSendFn(CoapSendFn send) { m_send = std::move(send); }

void CoapClient::OnEvent(
    std::function<void(CoapClientEvent, const CoapMessage&)> handler) {
    m_on_event = std::move(handler);
}

std::uint64_t CoapClient::NextRandom() {
    // splitmix64：确定性随机源，测试可用 random_seed 复现同一 Token/ID 序列。
    m_rng_state += 0x9e3779b97f4a7c15ull;
    std::uint64_t z = m_rng_state;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

std::uint16_t CoapClient::NextMessageId() {
    ++m_next_message_id;
    if (m_next_message_id == 0) {
        ++m_next_message_id;  // 0 保留给「还没发过请求」的查询语义。
    }
    return m_next_message_id;
}

std::vector<std::uint8_t> CoapClient::MakeToken() {
    std::vector<std::uint8_t> token;
    token.reserve(m_config.token_bytes);
    while (token.size() < m_config.token_bytes) {
        std::uint64_t random = NextRandom();
        for (int index = 0; index < 8 && token.size() < m_config.token_bytes; ++index) {
            token.push_back(static_cast<std::uint8_t>(random & 0xffu));
            random >>= 8;
        }
    }
    return token;
}

bool CoapClient::Send(const CoapRequest& request, std::uint64_t now_ms) {
    if (!m_send) {
        m_status = CoapClientStatus::kSendFailed;
        return false;
    }
    if (m_pending.has_value()) {
        m_status = CoapClientStatus::kBusy;
        return false;
    }
    if (!IsCoapRequestCode(request.method)) {
        m_status = CoapClientStatus::kEncodeError;
        return false;
    }
    if (request.type != CoapType::kCon && request.type != CoapType::kNon) {
        m_status = CoapClientStatus::kEncodeError;
        return false;
    }
    if (request.token.size() > kCoapMaxTokenBytes) {
        m_status = CoapClientStatus::kEncodeError;
        return false;
    }

    CoapMessage message;
    message.type = request.type;
    message.code = request.method;
    message.message_id = request.message_id != 0 ? request.message_id : NextMessageId();
    message.token = request.token.empty() ? MakeToken() : request.token;
    if (!request.uri_host.empty()) {
        message.AddStringOption(kCoapOptionUriHost, request.uri_host);
    }
    if (request.uri_port != 0) {
        message.AddUintOption(kCoapOptionUriPort, request.uri_port);
    }
    message.AddUriPath(request.uri_path);
    if (request.content_format.has_value()) {
        message.AddContentFormat(*request.content_format);
    }
    message.payload = request.payload;

    CoapStatus encode_status = CoapStatus::kOk;
    std::vector<std::uint8_t> wire = EncodeCoapMessage(message, &encode_status);
    if (encode_status != CoapStatus::kOk) {
        m_status = CoapClientStatus::kEncodeError;
        return false;
    }

    PendingRequest pending;
    pending.request = std::move(message);
    pending.wire = std::move(wire);
    const std::uint64_t start_ms = now_ms == 0 ? CoapNowMs() : now_ms;
    pending.deadline_ms = start_ms + m_config.ack_timeout_ms;
    m_pending = std::move(pending);
    m_last_message_id = m_pending->request.message_id;
    m_status = CoapClientStatus::kOk;

    // 拷贝一份 wire：发送回调可能同步完成交换并清空 pending，
    // 直接传 m_pending->wire 会在回调返回后悬垂。
    const std::vector<std::uint8_t> wire_copy = m_pending->wire;
    if (!m_send(wire_copy)) {
        if (m_pending.has_value()) {
            m_pending.reset();
        }
        m_status = CoapClientStatus::kSendFailed;
        return false;
    }
    return true;
}

bool CoapClient::SendRequest(CoapCode method, std::string_view uri_path,
                             const std::vector<std::uint8_t>& payload,
                             std::optional<std::uint16_t> content_format,
                             std::uint64_t now_ms) {
    CoapRequest request;
    request.method = method;
    request.uri_path = std::string(uri_path);
    request.payload = payload;
    request.content_format = content_format;
    return Send(request, now_ms);
}

bool CoapClient::HandleDatagram(const std::uint8_t* data, std::size_t size) {
    CoapMessage message;
    const CoapStatus status = DecodeCoapMessage(data, size, &message);
    if (status != CoapStatus::kOk) {
        m_status = CoapClientStatus::kDecodeError;
        return false;
    }
    if (!m_pending.has_value()) {
        m_status = CoapClientStatus::kNoPending;
        return false;
    }

    const std::uint16_t pending_id = m_pending->request.message_id;
    const std::vector<std::uint8_t> pending_token = m_pending->request.token;

    if (message.type == CoapType::kRst && message.message_id == pending_id) {
        const CoapMessage original = m_pending->request;
        m_pending.reset();
        m_status = CoapClientStatus::kOk;
        Deliver(CoapClientEvent::kReset, original);
        return true;
    }

    if (message.type == CoapType::kAck && message.message_id == pending_id) {
        if (message.code == CoapCode::kEmpty) {
            // 空 ACK 只停止重传，响应稍后以 separate response 到达。
            m_pending->acknowledged = true;
            m_status = CoapClientStatus::kOk;
            return true;
        }
        if (message.token == pending_token) {
            m_pending.reset();
            m_status = CoapClientStatus::kOk;
            Deliver(CoapClientEvent::kResponse, message);
            return true;
        }
        m_status = CoapClientStatus::kIgnored;
        return false;
    }

    if ((message.type == CoapType::kCon || message.type == CoapType::kNon) &&
        IsCoapResponseCode(message.code) && message.token == pending_token) {
        if (message.type == CoapType::kCon) {
            SendAck(message.message_id);
        }
        m_pending.reset();
        m_status = CoapClientStatus::kOk;
        Deliver(CoapClientEvent::kResponse, message);
        return true;
    }

    m_status = CoapClientStatus::kIgnored;
    return false;
}

bool CoapClient::HandleDatagram(const std::vector<std::uint8_t>& bytes) {
    return HandleDatagram(bytes.data(), bytes.size());
}

void CoapClient::Poll(std::uint64_t now_ms) {
    if (!m_pending.has_value() || !m_send) {
        // 没有待确认请求，或发送回调已被清除（SetSendFn({})）：无重传可做。
        // 后者若继续调用会抛 std::bad_function_call，因此提前返回。
        return;
    }
    if (now_ms == 0) {
        now_ms = CoapNowMs();
    }
    while (m_pending.has_value() && !m_pending->acknowledged &&
           now_ms >= m_pending->deadline_ms) {
        if (m_pending->request.type == CoapType::kNon) {
            // RFC 7252 §4.3：NON 报文不得重传。到点即按「等待响应超时」收尾。
            const CoapMessage original = m_pending->request;
            m_pending.reset();
            Deliver(CoapClientEvent::kTimeout, original);
            return;
        }
        if (m_pending->retransmits >= m_config.max_retransmit) {
            const CoapMessage original = m_pending->request;
            m_pending.reset();
            Deliver(CoapClientEvent::kTimeout, original);
            return;
        }
        const unsigned next_retransmit = m_pending->retransmits + 1;
        const std::uint64_t factor = std::uint64_t{1} << next_retransmit;
        m_pending->retransmits = next_retransmit;
        m_pending->deadline_ms =
            now_ms + static_cast<std::uint64_t>(m_config.ack_timeout_ms) * factor;

        // 同 Send：回调可能同步完成交换并清空 pending，先拷一份。
        const std::vector<std::uint8_t> wire = m_pending->wire;
        m_send(wire);
    }
}

bool CoapClient::HasPending() const noexcept { return m_pending.has_value(); }

std::uint16_t CoapClient::LastMessageId() const noexcept { return m_last_message_id; }

unsigned CoapClient::PendingRetransmits() const noexcept {
    return m_pending.has_value() ? m_pending->retransmits : 0;
}

CoapClientStatus CoapClient::LastStatus() const noexcept { return m_status; }

void CoapClient::Deliver(CoapClientEvent event, const CoapMessage& message) {
    if (m_on_event) {
        m_on_event(event, message);
    }
}

void CoapClient::SendAck(std::uint16_t message_id) {
    if (!m_send) {
        return;
    }
    CoapMessage ack;
    ack.type = CoapType::kAck;
    ack.code = CoapCode::kEmpty;
    ack.message_id = message_id;
    CoapStatus status = CoapStatus::kOk;
    const std::vector<std::uint8_t> wire = EncodeCoapMessage(ack, &status);
    if (status == CoapStatus::kOk) {
        m_send(wire);
    }
}

// ---------------------------------------------------------------------------
// 服务端
// ---------------------------------------------------------------------------

CoapServer::CoapServer(CoapSendFn send, CoapServerConfig config)
    : m_send(std::move(send)), m_config(config), m_rng_state(config.random_seed) {
    m_next_message_id = static_cast<std::uint16_t>(NextRandom());
}

void CoapServer::SetSendFn(CoapSendFn send) { m_send = std::move(send); }

void CoapServer::SetHandler(std::function<CoapResponse(const CoapMessage&)> handler) {
    m_handler = std::move(handler);
}

std::uint64_t CoapServer::NextRandom() {
    m_rng_state += 0x9e3779b97f4a7c15ull;
    std::uint64_t z = m_rng_state;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

std::uint16_t CoapServer::NextMessageId() {
    ++m_next_message_id;
    if (m_next_message_id == 0) {
        ++m_next_message_id;
    }
    return m_next_message_id;
}

bool CoapServer::SendMessage(const CoapMessage& message) {
    if (!m_send) {
        return false;
    }
    CoapStatus status = CoapStatus::kOk;
    const std::vector<std::uint8_t> wire = EncodeCoapMessage(message, &status);
    if (status != CoapStatus::kOk) {
        m_last_status = status;
        return false;
    }
    return m_send(wire);
}

std::optional<CoapMessage> CoapServer::Handle(const std::uint8_t* data,
                                              std::size_t size) {
    CoapMessage request;
    m_last_status = DecodeCoapMessage(data, size, &request);
    if (m_last_status != CoapStatus::kOk) {
        ++m_dropped;
        return std::nullopt;
    }
    if (!IsCoapRequestCode(request.code)) {
        ++m_dropped;
        return std::nullopt;
    }
    if (request.type == CoapType::kAck || request.type == CoapType::kRst) {
        ++m_dropped;
        return std::nullopt;
    }

    for (const CachedExchange& cached : m_cache) {
        if (cached.message_id == request.message_id && cached.token == request.token) {
            ++m_response_count;
            SendMessage(cached.response);
            return cached.response;
        }
    }

    ++m_handler_calls;
    CoapResponse response =
        m_handler ? m_handler(request) : DefaultResponse(request.code);
    if (!IsCoapResponseCode(response.code)) {
        // handler 返回了请求码/空码：按服务端内部错误处理，绝不发出非法响应。
        response.code = CoapCode::kInternalServerError;
        response.payload.clear();
    }

    CoapMessage reply;
    reply.token = request.token;
    reply.code = response.code;
    if (response.content_format.has_value()) {
        reply.AddContentFormat(*response.content_format);
    }
    reply.payload = std::move(response.payload);
    if (request.type == CoapType::kCon) {
        reply.type = CoapType::kAck;
        reply.message_id = request.message_id;
    } else {
        reply.type = CoapType::kNon;
        reply.message_id = NextMessageId();
    }

    if (m_config.dedup_cache_size > 0) {
        CachedExchange entry;
        entry.message_id = request.message_id;
        entry.token = request.token;
        entry.response = reply;
        m_cache.push_back(std::move(entry));
        while (m_cache.size() > m_config.dedup_cache_size) {
            m_cache.pop_front();
        }
    }

    ++m_response_count;
    SendMessage(reply);
    return reply;
}

std::optional<CoapMessage> CoapServer::Handle(
    const std::vector<std::uint8_t>& bytes) {
    return Handle(bytes.data(), bytes.size());
}

std::uint64_t CoapServer::HandlerCalls() const noexcept { return m_handler_calls; }

std::uint64_t CoapServer::ResponseCount() const noexcept { return m_response_count; }

std::uint64_t CoapServer::DroppedCount() const noexcept { return m_dropped; }

CoapStatus CoapServer::LastStatus() const noexcept { return m_last_status; }

CoapResponse CoapServer::DefaultResponse(CoapCode method) {
    CoapResponse response;
    switch (method) {
        case CoapCode::kGet:
            response.code = CoapCode::kContent;
            break;
        case CoapCode::kPost:
            response.code = CoapCode::kCreated;
            break;
        case CoapCode::kPut:
            response.code = CoapCode::kChanged;
            break;
        case CoapCode::kDelete:
            response.code = CoapCode::kDeleted;
            break;
        default:
            response.code = CoapCode::kMethodNotAllowed;
            break;
    }
    return response;
}

}  // namespace go2cpp::bento::net
