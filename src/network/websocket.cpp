/**
 * @file websocket.cpp
 * @brief RFC 6455 WebSocket 握手、帧状态机和消息重组实现。
 *
 * ## 模块定位
 * `websocket.hpp` 的实现：握手报文构造/校验、帧编解码、消息重组与关闭握手。
 * 所有阻塞都落在 `Socket::recv/send` 上（Fiber 中即 park），本文件不感知调度器。
 *
 * ## 本文件定义什么
 * - 匿名命名空间里的纯工具：trim、大小写不敏感比较、base64 编解码、SHA-1、错误文案；
 * - 握手：`ValidateUpgradeRequest()` / `BuildUpgradeResponse()` / `Accept*()` / `Connect()`；
 * - 帧层：`read_frame()` / `write_frame_unlocked()`（长度编码、掩码、控制帧校验）；
 * - 消息层：`read_message()` 分片重组、`close()`/`protocol_error()` 关闭流程。
 *
 * ## 阅读建议
 * 先看 `read_frame()` 的四个阶段（基础头 -> 扩展长度 -> 掩码键 -> payload），
 * 再看 `read_message()` 的分片状态机，最后看 `close()`/`abort_transport()`。
 *
 * ## 边界与陷阱
 * - 服务端收到未掩码帧、客户端收到掩码帧都是协议错误（方向校验在 `read_frame()`）；
 * - 读失败只返回错误、不自动 `close()`：是否关闭由调用方决定；
 * - `write_frame_unlocked()` 要求调用方已持有 `m_write_mutex`（名字里的 unlocked 指锁外）。
 */
#include "go2cpp/network/websocket.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <limits>
#include <new>
#include <random>
#include <sstream>

namespace go2cpp::net {

namespace {

constexpr std::string_view kWebSocketGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
constexpr std::string_view kBase64 =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string trim_copy(std::string_view value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.remove_prefix(1);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.remove_suffix(1);
    return std::string(value);
}

bool equal_ci(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) return false;
    for (std::size_t i = 0; i < left.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(left[i])) !=
            std::tolower(static_cast<unsigned char>(right[i]))) return false;
    }
    return true;
}

bool decode_base64(std::string_view input, std::vector<std::uint8_t>& output) {
    output.clear();
    if (input.empty() || input.size() % 4 != 0) return false;
    auto value_of = [](unsigned char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    output.reserve(input.size() / 4 * 3);
    for (std::size_t offset = 0; offset < input.size(); offset += 4) {
        const bool last = offset + 4 == input.size();
        const char a = input[offset];
        const char b = input[offset + 1];
        const char c = input[offset + 2];
        const char d = input[offset + 3];
        const int va = value_of(static_cast<unsigned char>(a));
        const int vb = value_of(static_cast<unsigned char>(b));
        if (va < 0 || vb < 0) return false;
        if (c == '=') {
            if (!last || d != '=') return false;
            output.push_back(static_cast<std::uint8_t>((va << 2) | (vb >> 4)));
            continue;
        }
        const int vc = value_of(static_cast<unsigned char>(c));
        if (vc < 0) return false;
        output.push_back(static_cast<std::uint8_t>((va << 2) | (vb >> 4)));
        output.push_back(static_cast<std::uint8_t>((vb << 4) | (vc >> 2)));
        if (d == '=') {
            if (!last) return false;
            continue;
        }
        const int vd = value_of(static_cast<unsigned char>(d));
        if (vd < 0) return false;
        output.push_back(static_cast<std::uint8_t>((vc << 6) | vd));
    }
    return true;
}

std::string encode_base64(const std::uint8_t* data, std::size_t size) {
    std::string output;
    output.reserve((size + 2) / 3 * 4);
    for (std::size_t offset = 0; offset < size; offset += 3) {
        const std::size_t remaining = size - offset;
        const std::uint32_t value = (static_cast<std::uint32_t>(data[offset]) << 16) |
            (remaining > 1 ? static_cast<std::uint32_t>(data[offset + 1]) << 8 : 0) |
            (remaining > 2 ? static_cast<std::uint32_t>(data[offset + 2]) : 0);
        output.push_back(kBase64[(value >> 18) & 0x3F]);
        output.push_back(kBase64[(value >> 12) & 0x3F]);
        output.push_back(remaining > 1 ? kBase64[(value >> 6) & 0x3F] : '=');
        output.push_back(remaining > 2 ? kBase64[value & 0x3F] : '=');
    }
    return output;
}

std::array<std::uint8_t, 20> sha1(std::string_view input) {
    std::array<std::uint32_t, 5> state{
        0x67452301U, 0xEFCDAB89U, 0x98BADCFEU, 0x10325476U, 0xC3D2E1F0U};
    const std::uint64_t bit_length = static_cast<std::uint64_t>(input.size()) * 8U;
    std::vector<std::uint8_t> bytes(input.begin(), input.end());
    bytes.push_back(0x80U);
    while ((bytes.size() % 64) != 56) bytes.push_back(0);
    for (int shift = 56; shift >= 0; shift -= 8)
        bytes.push_back(static_cast<std::uint8_t>((bit_length >> shift) & 0xFFU));
    for (std::size_t offset = 0; offset < bytes.size(); offset += 64) {
        std::array<std::uint32_t, 80> words{};
        for (std::size_t i = 0; i < 16; ++i) {
            words[i] = (static_cast<std::uint32_t>(bytes[offset + i * 4]) << 24) |
                       (static_cast<std::uint32_t>(bytes[offset + i * 4 + 1]) << 16) |
                       (static_cast<std::uint32_t>(bytes[offset + i * 4 + 2]) << 8) |
                       static_cast<std::uint32_t>(bytes[offset + i * 4 + 3]);
        }
        for (std::size_t i = 16; i < 80; ++i) {
            const auto value = words[i - 3] ^ words[i - 8] ^ words[i - 14] ^ words[i - 16];
            words[i] = (value << 1) | (value >> 31);
        }
        std::uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];
        for (std::size_t i = 0; i < 80; ++i) {
            std::uint32_t function = 0, constant = 0;
            if (i < 20) {
                function = (b & c) | ((~b) & d); constant = 0x5A827999U;
            } else if (i < 40) {
                function = b ^ c ^ d; constant = 0x6ED9EBA1U;
            } else if (i < 60) {
                function = (b & c) | (b & d) | (c & d); constant = 0x8F1BBCDCU;
            } else {
                function = b ^ c ^ d; constant = 0xCA62C1D6U;
            }
            const std::uint32_t rotated = (a << 5) | (a >> 27);
            const std::uint32_t next = rotated + function + e + constant + words[i];
            e = d; d = c; c = (b << 30) | (b >> 2); b = a; a = next;
        }
        state[0] += a; state[1] += b; state[2] += c; state[3] += d; state[4] += e;
    }
    std::array<std::uint8_t, 20> output{};
    for (std::size_t i = 0; i < state.size(); ++i) {
        output[i * 4] = static_cast<std::uint8_t>(state[i] >> 24);
        output[i * 4 + 1] = static_cast<std::uint8_t>(state[i] >> 16);
        output[i * 4 + 2] = static_cast<std::uint8_t>(state[i] >> 8);
        output[i * 4 + 3] = static_cast<std::uint8_t>(state[i]);
    }
    return output;
}

std::string result_message(WebSocketError error, int system_error,
                           std::string message) {
    if (message.empty()) {
        switch (error) {
        case WebSocketError::Closed: message = "websocket closed"; break;
        case WebSocketError::Timeout: message = "websocket timeout"; break;
        case WebSocketError::Protocol: message = "websocket protocol error"; break;
        case WebSocketError::InvalidHandshake: message = "invalid websocket handshake"; break;
        case WebSocketError::MessageTooLarge: message = "websocket message too large"; break;
        case WebSocketError::InvalidState: message = "invalid websocket state"; break;
        case WebSocketError::Io: message = "websocket I/O error"; break;
        case WebSocketError::None: break;
        }
    }
    if (system_error != 0 && message.empty()) message = std::strerror(system_error);
    return message;
}

}  // namespace

bool WebSocket::header_token(std::string_view value, std::string_view token) noexcept {
    while (!value.empty()) {
        const auto comma = value.find(',');
        auto item = value.substr(0, comma);
        while (!item.empty() && std::isspace(static_cast<unsigned char>(item.front()))) item.remove_prefix(1);
        while (!item.empty() && std::isspace(static_cast<unsigned char>(item.back()))) item.remove_suffix(1);
        if (equal_ci(item, token)) return true;
        if (comma == std::string_view::npos) break;
        value.remove_prefix(comma + 1);
    }
    return false;
}

/**
 * @brief 校验 Upgrade 请求并提取 Sec-WebSocket-Key。
 *
 * ## 步骤
 * step1. 必须是 `GET` + `HTTP/1.1`，且不能带 body（Content-Length 必须缺失或 0）；
 * step2. Host 必须存在；Upgrade/Connection 必须含 websocket/upgrade token；
 * step3. `Sec-WebSocket-Version` 必须是 13；
 * step4. `Sec-WebSocket-Key` 必须是合法 base64 且解码后恰好 16 字节（无控制字符）。
 */
bool WebSocket::ValidateUpgradeRequest(const http::Request& request,
                                       std::string& key, std::string* reason) {
    auto fail = [reason](const char* message) {
        if (reason) *reason = message;
        return false;
    };
    auto has_ctl = [](std::string_view value) {
        for (unsigned char c : value) if (c < 0x20U || c == 0x7FU) return true;
        return false;
    };
    // step1: 方法与版本。
    if (request.method != http::Method::Get || request.version != "HTTP/1.1")
        return fail("websocket requires GET HTTP/1.1");
    const auto content_length = trim_copy(request.header.get("Content-Length"));
    if (!request.body.empty() || (!content_length.empty() && content_length != "0") ||
        request.header.has("Transfer-Encoding"))
        return fail("websocket upgrade must not contain an HTTP body");
    if (trim_copy(request.header.get("Host")).empty()) return fail("missing host header");
    if (!header_token(request.header.get("Upgrade"), "websocket") ||
        !header_token(request.header.get("Connection"), "upgrade"))
        return fail("missing websocket upgrade headers");
    if (trim_copy(request.header.get("Sec-WebSocket-Version")) != "13")
        return fail("unsupported websocket version");
    // step3/4: 取 key 并检查控制字符，随后做 base64 长度校验。
    key = trim_copy(request.header.get("Sec-WebSocket-Key"));
    if (has_ctl(key)) return fail("invalid websocket key");
    std::vector<std::uint8_t> decoded;
    if (!decode_base64(key, decoded) || decoded.size() != 16)
        return fail("invalid websocket key");
    return true;
}

http::Response WebSocket::BuildUpgradeResponse(const http::Request& request) {
    http::Response response;
    std::string key;
    if (!ValidateUpgradeRequest(request, key, nullptr)) {
        response.status = http::Status::BadRequest;
        response.close = true;
        return response;
    }
    response.status = static_cast<http::Status>(101);
    response.header.set("Upgrade", "websocket");
    response.header.set("Connection", "Upgrade");
    response.header.set("Sec-WebSocket-Accept", make_accept_key(key));
    return response;
}

void WebSocket::set_result(WebSocketResult* output, const WebSocketResult& result) {
    if (output) *output = result;
}

WebSocket::Ptr WebSocket::Accept(Socket::Ptr socket, const http::Request& request,
                                  WebSocketConfig config, WebSocketResult* result) {
    return AcceptWithPending(std::move(socket), request, {}, config, result);
}

WebSocket::Ptr WebSocket::AcceptWithPending(Socket::Ptr socket, const http::Request& request,
                                            std::string pending, WebSocketConfig config,
                                            WebSocketResult* result) {
    std::string reason;
    std::string key;
    if (!socket || !ValidateUpgradeRequest(request, key, &reason)) {
        const WebSocketResult failure{WebSocketError::InvalidHandshake, 0,
                                      result_message(WebSocketError::InvalidHandshake, 0, reason)};
        set_result(result, failure); return {};
    }
    const auto response = BuildUpgradeResponse(request);
    const auto wire = response.to_string();
    if (config.io_timeout != Socket::Duration::max() && !socket->set_send_timeout(config.io_timeout)) {
        const WebSocketResult failure{WebSocketError::Io, errno == 0 ? EIO : errno,
                                      "unable to configure websocket handshake timeout"};
        set_result(result, failure); return {};
    }
    SocketStream stream(socket, false);
    if (!stream.write_all(wire.data(), wire.size())) {
        const int error = errno == 0 ? EIO : errno;
        const WebSocketResult failure{WebSocketError::Io, error,
                                      result_message(WebSocketError::Io, error, {})};
        set_result(result, failure); return {};
    }
    auto websocket = Ptr(new WebSocket(std::move(socket), WebSocketRole::Server, config,
                                       std::move(pending)));
    set_result(result, WebSocketResult::success());
    return websocket;
}

/**
 * @brief 客户端发起握手：发 Upgrade 请求并校验 101 响应。
 *
 * ## 步骤
 * step1. 校验 target 以 '/' 开头、host/target 无控制字符；
 * step2. 生成 16 字节随机 key 并拼 Upgrade 请求写出；
 * step3. 逐字节读到 `\r\n\r\n`（带绝对 deadline，超过 64KiB 报错）；
 * step4. 用 `http::Parser` 解析响应，校验 101 + Upgrade/Connection + `Sec-WebSocket-Accept`。
 *
 * ## 失败语义
 * 任何一步失败都返回空 `Ptr` 并填 `result`，**不关闭**传入的 socket。
 */
WebSocket::Ptr WebSocket::Connect(Socket::Ptr socket, std::string host, std::string target,
                                  WebSocketConfig config, WebSocketResult* result) {
    auto has_ctl = [](std::string_view value) {
        for (unsigned char c : value) if (c < 0x20U || c == 0x7FU) return true;
        return false;
    };
    if (!socket || target.empty() || target.front() != '/' || host.empty() ||
        has_ctl(host) || has_ctl(target)) {
        const WebSocketResult failure{WebSocketError::InvalidHandshake, 0,
                                      result_message(WebSocketError::InvalidHandshake, 0, "invalid websocket target")};
        set_result(result, failure); return {};
    }
    // step1/2: 生成随机 key 与请求行。
    const std::string key = make_client_key();
    if (key.empty()) {
        const WebSocketResult failure{WebSocketError::InvalidHandshake, EIO,
                                      "unable to create websocket key"};
        set_result(result, failure); return {};
    }
    std::ostringstream request;
    request << "GET " << target << " HTTP/1.1\r\n"
            << "Host: " << host << "\r\n"
            << "Upgrade: websocket\r\n"
            << "Connection: Upgrade\r\n"
            << "Sec-WebSocket-Key: " << key << "\r\n"
            << "Sec-WebSocket-Version: 13\r\n\r\n";
    const auto request_wire = request.str();
    if (config.io_timeout != Socket::Duration::max() && !socket->set_send_timeout(config.io_timeout)) {
        const WebSocketResult failure{WebSocketError::Io, errno == 0 ? EIO : errno,
                                      "unable to configure websocket handshake timeout"};
        set_result(result, failure); return {};
    }
    SocketStream stream(socket, false);
    if (!stream.write_all(request_wire.data(), request_wire.size())) {
        const int error = errno == 0 ? EIO : errno;
        const WebSocketResult failure{WebSocketError::Io, error,
                                      result_message(WebSocketError::Io, error, {})};
        set_result(result, failure); return {};
    }
    std::string response_wire;
    response_wire.reserve(1024);
    const auto start = std::chrono::steady_clock::now();
    for (;;) {
        char byte = 0;
        auto left = config.io_timeout;
        if (left != Socket::Duration::max()) {
            const auto elapsed = std::chrono::duration_cast<Socket::Duration>(
                std::chrono::steady_clock::now() - start);
            if (elapsed >= left) left = Socket::Duration::zero();
            else left -= elapsed;
        }
        const auto received = socket->recv(&byte, 1, 0, left);
        if (!received.ok()) {
            const auto category = received.error == ETIMEDOUT ? WebSocketError::Timeout : WebSocketError::Io;
            const WebSocketResult failure{category, received.error,
                                          result_message(category, received.error, {})};
            set_result(result, failure); return {};
        }
        if (received.transferred == 0) {
            const WebSocketResult failure{WebSocketError::Closed, 0,
                                          result_message(WebSocketError::Closed, 0, {})};
            set_result(result, failure); return {};
        }
        response_wire.push_back(byte);
        if (response_wire.size() > 64 * 1024) {
            const WebSocketResult failure{WebSocketError::InvalidHandshake, 0, "websocket response headers too large"};
            set_result(result, failure); return {};
        }
        if (response_wire.size() >= 4 && response_wire.compare(response_wire.size() - 4, 4, "\r\n\r\n") == 0)
            break;
    }
    // step3/4: 解析响应并校验 Accept 值。
    http::Parser parser;
    http::Response response;
    const auto parsed = parser.feed(response_wire, response);
    if (parsed.state != http::ParseState::Complete || static_cast<int>(response.status) != 101 ||
        !header_token(response.header.get("Upgrade"), "websocket") ||
        !header_token(response.header.get("Connection"), "upgrade") ||
        response.header.get("Sec-WebSocket-Accept") != make_accept_key(key)) {
        const WebSocketResult failure{WebSocketError::InvalidHandshake, 0,
                                      result_message(WebSocketError::InvalidHandshake, 0, parsed.error)};
        set_result(result, failure); return {};
    }
    auto websocket = Ptr(new WebSocket(std::move(socket), WebSocketRole::Client, config));
    set_result(result, WebSocketResult::success());
    return websocket;
}

WebSocket::WebSocket(Socket::Ptr socket, WebSocketRole role, WebSocketConfig config,
                     std::string pending)
    : m_socket(std::move(socket)), m_role(role), m_config(config),
      m_prefetched(std::move(pending)) {}

WebSocket::~WebSocket() {
    m_closed.store(true, std::memory_order_release);
    abort_transport();
}

void WebSocket::abort_transport() noexcept {
    if (!m_socket) return;
    // 代际取消会唤醒正在其他 M/Fiber 上 read/write 的调用；不在这里
    // 直接 close fd，避免破坏仍由调用方共享的 Socket 所有权。
    (void)m_socket->cancel_read();
    (void)m_socket->cancel_write();
}

WebSocketResult WebSocket::read_exact(void* data, std::size_t size) {
    auto* output = static_cast<std::uint8_t*>(data);
    const auto start = std::chrono::steady_clock::now();
    std::size_t offset = 0;
    while (offset < size) {
        if (m_prefetched_offset < m_prefetched.size()) {
            const auto available = m_prefetched.size() - m_prefetched_offset;
            const auto copied = std::min(available, size - offset);
            std::memcpy(output + offset, m_prefetched.data() + m_prefetched_offset, copied);
            m_prefetched_offset += copied;
            offset += copied;
            continue;
        }
        auto timeout = m_config.io_timeout;
        if (timeout != Socket::Duration::max()) {
            const auto elapsed = std::chrono::duration_cast<Socket::Duration>(
                std::chrono::steady_clock::now() - start);
            if (elapsed >= timeout) timeout = Socket::Duration::zero();
            else timeout -= elapsed;
        }
        const auto result = m_socket->recv(output + offset, size - offset, 0, timeout);
        if (!result.ok()) {
            const auto error = result.error == ETIMEDOUT ? WebSocketError::Timeout :
                (result.error == ECANCELED || result.error == EBADF ? WebSocketError::Closed : WebSocketError::Io);
            return {error, result.error, result_message(error, result.error, {})};
        }
        if (result.transferred == 0) return {WebSocketError::Closed, 0, "peer closed"};
        offset += static_cast<std::size_t>(result.transferred);
    }
    return WebSocketResult::success();
}

WebSocketResult WebSocket::write_all(const void* data, std::size_t size) {
    const auto* input = static_cast<const std::uint8_t*>(data);
    const auto start = std::chrono::steady_clock::now();
    std::size_t offset = 0;
    while (offset < size) {
        auto timeout = m_config.io_timeout;
        if (timeout != Socket::Duration::max()) {
            const auto elapsed = std::chrono::duration_cast<Socket::Duration>(
                std::chrono::steady_clock::now() - start);
            if (elapsed >= timeout) timeout = Socket::Duration::zero();
            else timeout -= elapsed;
        }
        const auto result = m_socket->send(input + offset, size - offset, 0, timeout);
        if (!result.ok()) {
            const auto error = result.error == ETIMEDOUT ? WebSocketError::Timeout :
                (result.error == ECANCELED || result.error == EBADF ? WebSocketError::Closed : WebSocketError::Io);
            return {error, result.error, result_message(error, result.error, {})};
        }
        if (result.transferred == 0) return {WebSocketError::Closed, 0, "peer closed"};
        offset += static_cast<std::size_t>(result.transferred);
    }
    return WebSocketResult::success();
}

/**
 * @brief 读一个完整帧（基础头 -> 扩展长度 -> 掩码键 -> payload）。
 *
 * ## 步骤
 * step1. 读 2 字节基础头，校验 RSV 位与 opcode，按角色校验掩码方向；
 * step2. 读扩展长度（126 -> 2 字节、127 -> 8 字节）并拒绝非最小编码；
 * step3. 校验控制帧（FIN 必须置位、长度 <= 125）与帧上限；
 * step4. 读掩码键（若有）与 payload，按掩码逐字节 XOR。
 *
 * ## 失败语义
 * 协议违规走 `protocol_error()`（发 Close 后返回 `Protocol`）；IO 失败原样返回。
 */
WebSocketResult WebSocket::read_frame(Frame& frame) {
    std::array<std::uint8_t, 2> header{};
    auto result = read_exact(header.data(), header.size());
    if (!result.ok()) return result;
    // step1: 解析 FIN/RSV/opcode/MASK，并做方向校验。
    frame.fin = (header[0] & 0x80U) != 0;
    if ((header[0] & 0x70U) != 0) return protocol_error("reserved bits are not supported");
    const auto opcode_value = static_cast<std::uint8_t>(header[0] & 0x0FU);
    if (opcode_value != 0 && opcode_value != 1 && opcode_value != 2 &&
        opcode_value != 8 && opcode_value != 9 && opcode_value != 10)
        return protocol_error("unknown websocket opcode");
    frame.opcode = static_cast<WebSocketOpcode>(opcode_value);
    frame.masked = (header[1] & 0x80U) != 0;
    if ((m_role == WebSocketRole::Server && !frame.masked) ||
        (m_role == WebSocketRole::Client && frame.masked))
        return protocol_error("invalid websocket mask direction");
    // step2: 扩展长度编码（拒绝非最小表示）。
    std::uint64_t length = header[1] & 0x7FU;
    if (length == 126) {
        std::array<std::uint8_t, 2> extended{};
        if (!(result = read_exact(extended.data(), extended.size())).ok()) return result;
        length = (static_cast<std::uint64_t>(extended[0]) << 8) | extended[1];
        if (length < 126) return protocol_error("non-minimal websocket length");
    } else if (length == 127) {
        std::array<std::uint8_t, 8> extended{};
        if (!(result = read_exact(extended.data(), extended.size())).ok()) return result;
        if ((extended[0] & 0x80U) != 0) return protocol_error("invalid 64 bit payload length");
        length = 0;
        for (const auto byte : extended) length = (length << 8) | byte;
        if (length < 65536) return protocol_error("non-minimal websocket length");
    }
    const bool control = opcode_value >= 8;
    if ((control && (!frame.fin || length > 125)) || length > m_config.max_frame_payload)
        return control ? protocol_error("invalid control frame") :
            protocol_error("frame payload too large", 1009);
    // step3/4: 读掩码键与 payload。
    std::array<std::uint8_t, 4> mask{};
    if (frame.masked && !(result = read_exact(mask.data(), mask.size())).ok()) return result;
    if (length > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
        return protocol_error("frame payload overflows size_t", 1009);
    try {
        frame.payload.resize(static_cast<std::size_t>(length));
    } catch (const std::bad_alloc&) {
        abort_transport();
        m_closed.store(true, std::memory_order_release);
        return {WebSocketError::MessageTooLarge, ENOMEM, "frame allocation failed"};
    }
    if (length != 0 && !(result = read_exact(frame.payload.data(), frame.payload.size())).ok()) return result;
    // step4: 解掩码（服务端收到的帧必须带掩码）。
    if (frame.masked) {
        for (std::size_t i = 0; i < frame.payload.size(); ++i) frame.payload[i] ^= mask[i % 4];
    }
    return WebSocketResult::success();
}

WebSocketResult WebSocket::write_frame(WebSocketOpcode opcode, std::string_view payload, bool fin) {
    std::lock_guard<sync::Mutex> lock(m_write_mutex);
    return write_frame_unlocked(opcode, payload, fin);
}

/**
 * @brief 构造并写出一个帧（调用方必须已持有写锁）。
 *
 * ## 步骤
 * step1. 校验连接状态、opcode、控制帧约束与帧上限；
 * step2. 按角色决定是否掩码，生成 4 字节随机掩码键；
 * step3. 编码长度（<126 / <=0xFFFF / 64 位三种形态）；
 * step4. 先写头（含掩码键），再写 payload（客户端需先拷贝并异或掩码）。
 *
 * ## 失败语义
 * 写失败调用 `abort_transport()` 并置 `m_closed`，返回原始错误。
 */
WebSocketResult WebSocket::write_frame_unlocked(WebSocketOpcode opcode, std::string_view payload, bool fin) {
    if (!m_socket || m_closed.load(std::memory_order_acquire))
        return {WebSocketError::Closed, EBADF, "websocket is closed"};
    const auto value = static_cast<std::uint8_t>(opcode);
    const bool control = value >= 8;
    if ((control && (!fin || payload.size() > 125)) ||
        (value != 0 && value != 1 && value != 2 && value != 8 && value != 9 && value != 10))
        return {WebSocketError::Protocol, 0, "invalid websocket frame"};
    if (payload.size() > m_config.max_frame_payload)
        return {WebSocketError::MessageTooLarge, 0, "frame payload too large"};
    // step2: 客户端必须掩码、服务端必须不掩码。
    const bool mask = m_role == WebSocketRole::Client;
    std::array<std::uint8_t, 14> header{};
    std::size_t header_size = 2;
    header[0] = static_cast<std::uint8_t>((fin ? 0x80U : 0) | value);
    const std::uint8_t mask_bit = mask ? 0x80U : 0;
    // step3: 三种长度编码，选择最短的合法形态。
    if (payload.size() < 126) {
        header[1] = static_cast<std::uint8_t>(mask_bit | payload.size());
    } else if (payload.size() <= 0xFFFFU) {
        header[1] = static_cast<std::uint8_t>(mask_bit | 126U);
        header[2] = static_cast<std::uint8_t>(payload.size() >> 8);
        header[3] = static_cast<std::uint8_t>(payload.size());
        header_size = 4;
    } else {
        header[1] = static_cast<std::uint8_t>(mask_bit | 127U);
        const auto length = static_cast<std::uint64_t>(payload.size());
        for (int i = 0; i < 8; ++i) header[2 + i] = static_cast<std::uint8_t>(length >> (56 - i * 8));
        header_size = 10;
    }
    std::array<std::uint8_t, 4> mask_key{};
    if (mask) {
        try {
            std::random_device random;
            for (auto& byte : mask_key) byte = static_cast<std::uint8_t>(random());
        } catch (...) {
            return {WebSocketError::Io, EIO, "unable to create websocket mask"};
        }
        std::copy(mask_key.begin(), mask_key.end(), header.begin() + header_size);
        header_size += mask_key.size();
    }
    // step4: 先写头；失败即认为连接不可用。
    auto result = write_all(header.data(), header_size);
    if (!result.ok()) {
        abort_transport();
        m_closed.store(true, std::memory_order_release);
        return result;
    }
    if (payload.empty()) return result;
    if (!mask) {
        result = write_all(payload.data(), payload.size());
        if (!result.ok()) {
            abort_transport();
            m_closed.store(true, std::memory_order_release);
        }
        return result;
    }
    std::vector<std::uint8_t> masked;
    try {
        masked.assign(payload.begin(), payload.end());
    } catch (const std::bad_alloc&) {
        return {WebSocketError::MessageTooLarge, ENOMEM, "frame allocation failed"};
    }
    for (std::size_t i = 0; i < masked.size(); ++i) masked[i] ^= mask_key[i % 4];
    result = write_all(masked.data(), masked.size());
    if (!result.ok()) {
        abort_transport();
        m_closed.store(true, std::memory_order_release);
    }
    return result;
}

WebSocketResult WebSocket::send_message(WebSocketOpcode opcode, std::string_view payload) {
    // 整条消息持锁：分片帧之间不允许插入其它消息的帧（否则对端重组错乱）。
    std::lock_guard<sync::Mutex> lock(m_write_mutex);
    if (!m_socket || m_closed.load(std::memory_order_acquire)) {
        return {WebSocketError::Closed, EBADF, "websocket is closed"};
    }
    if (m_config.max_frame_payload == 0) {
        return {WebSocketError::MessageTooLarge, 0, "frame payload limit is zero"};
    }
    std::size_t offset = 0;
    do {
        const std::size_t remaining = payload.size() - offset;
        const std::size_t chunk = std::min(remaining, m_config.max_frame_payload);
        const bool last = remaining <= m_config.max_frame_payload;
        const WebSocketOpcode frame_opcode =
            offset == 0 ? opcode : WebSocketOpcode::Continuation;
        const auto result =
            write_frame_unlocked(frame_opcode, payload.substr(offset, chunk), last);
        if (!result.ok()) {
            return result;
        }
        offset += chunk;
    } while (offset < payload.size());
    return WebSocketResult::success();
}

WebSocketResult WebSocket::send_control(WebSocketOpcode opcode, std::string_view payload) {
    if (payload.size() > 125) return {WebSocketError::Protocol, 0, "control payload too large"};
    return write_frame(opcode, payload, true);
}

WebSocketResult WebSocket::protocol_error(std::string message, std::uint16_t code) {
    const auto result = close(code, "protocol error");
    (void)result;
    return {WebSocketError::Protocol, 0, std::move(message)};
}

/**
 * @brief 读一条完整业务消息（控制帧内联处理、数据帧按分片重组）。
 *
 * ## 步骤
 * step1. 取读锁并检查 `m_closed`；
 * step2. 循环 `read_frame()`：Ping 自动回 Pong、Pong 丢弃；
 * step3. Close：校验 code/reason，必要时回 Close，置 `m_closed` 后交付 Close 消息；
 * step4. 数据帧：首片记录 opcode，Continuation 追加，FIN 后交付并校验 UTF-8。
 *
 * ## 失败语义
 * 分片期间收到新的 Text/Binary、非法 close code、UTF-8 非法都走 `protocol_error()`。
 */
WebSocketResult WebSocket::read_message(WebSocketMessage& message) {
    std::lock_guard<sync::Mutex> lock(m_read_mutex);
    if (m_closed.load(std::memory_order_acquire)) return {WebSocketError::Closed, 0, "websocket is closed"};
    for (;;) {
        Frame frame;
        auto result = read_frame(frame);
        if (!result.ok()) return result;
        if (frame.opcode == WebSocketOpcode::Ping) {
            if (m_config.auto_pong) {
                const std::string_view ping_payload = frame.payload.empty()
                    ? std::string_view{}
                    : std::string_view(reinterpret_cast<const char*>(frame.payload.data()), frame.payload.size());
                result = send_control(WebSocketOpcode::Pong, ping_payload);
                if (!result.ok()) return result;
            }
            continue;
        }
        if (frame.opcode == WebSocketOpcode::Pong) continue;
        if (frame.opcode == WebSocketOpcode::Close) {
            if (frame.payload.size() == 1) return protocol_error("invalid close payload");
            std::uint16_t code = 1000;
            std::string_view reason;
            if (frame.payload.size() >= 2) {
                code = static_cast<std::uint16_t>(frame.payload[0] << 8 | frame.payload[1]);
                reason = std::string_view(reinterpret_cast<const char*>(frame.payload.data() + 2), frame.payload.size() - 2);
                if (!valid_close_code(code) || (m_config.validate_utf8 && !valid_utf8(reason)))
                    return protocol_error("invalid close code or reason");
            }
            {
                std::lock_guard<sync::Mutex> write_lock(m_write_mutex);
                if (!m_close_sent) {
                    const std::string payload = frame.payload.empty()
                        ? std::string{} : std::string(reinterpret_cast<const char*>(frame.payload.data()), frame.payload.size());
                    const auto close_result = write_frame_unlocked(WebSocketOpcode::Close, payload, true);
                    m_close_sent = true;
                    if (!close_result.ok()) {
                        m_closed.store(true, std::memory_order_release);
                        return close_result;
                    }
                }
            }
            m_closed.store(true, std::memory_order_release);
            message.opcode = WebSocketOpcode::Close;
            message.payload.assign(reinterpret_cast<const char*>(frame.payload.data()), frame.payload.size());
            return WebSocketResult::success();
        }
        if (frame.opcode == WebSocketOpcode::Continuation) {
            if (!m_fragmented) return protocol_error("unexpected continuation");
            if (frame.payload.size() > m_config.max_message_payload ||
                m_fragment_payload.size() > m_config.max_message_payload - frame.payload.size())
                return protocol_error("message payload too large", 1009);
            m_fragment_payload.append(reinterpret_cast<const char*>(frame.payload.data()), frame.payload.size());
            if (!frame.fin) continue;
            message.opcode = m_fragment_opcode;
            message.payload = std::move(m_fragment_payload);
            m_fragment_payload.clear(); m_fragmented = false;
            if (message.text() && m_config.validate_utf8 && !valid_utf8(message.payload))
                return protocol_error("invalid UTF-8 text message", 1007);
            return WebSocketResult::success();
        }
        if (m_fragmented) return protocol_error("new data frame during fragmentation");
        if (frame.opcode != WebSocketOpcode::Text && frame.opcode != WebSocketOpcode::Binary)
            return protocol_error("invalid data opcode");
        if (frame.payload.size() > m_config.max_message_payload)
            return protocol_error("message payload too large", 1009);
        if (frame.fin) {
            message.opcode = frame.opcode;
            message.payload.assign(reinterpret_cast<const char*>(frame.payload.data()), frame.payload.size());
            if (message.text() && m_config.validate_utf8 && !valid_utf8(message.payload))
                return protocol_error("invalid UTF-8 text message", 1007);
            return WebSocketResult::success();
        }
        m_fragmented = true;
        m_fragment_opcode = frame.opcode;
        m_fragment_payload.assign(reinterpret_cast<const char*>(frame.payload.data()), frame.payload.size());
    }
}

WebSocketResult WebSocket::send_text(std::string_view payload) {
    if (m_config.validate_utf8 && !valid_utf8(payload)) return {WebSocketError::Protocol, 0, "invalid UTF-8 text message"};
    if (payload.size() > m_config.max_message_payload) return {WebSocketError::MessageTooLarge, 0, "message payload too large"};
    if (!m_config.fragment_outgoing || payload.size() <= m_config.max_frame_payload) {
        return write_frame(WebSocketOpcode::Text, payload, true);
    }
    return send_message(WebSocketOpcode::Text, payload);
}
WebSocketResult WebSocket::send_binary(std::string_view payload) {
    if (payload.size() > m_config.max_message_payload) return {WebSocketError::MessageTooLarge, 0, "message payload too large"};
    if (!m_config.fragment_outgoing || payload.size() <= m_config.max_frame_payload) {
        return write_frame(WebSocketOpcode::Binary, payload, true);
    }
    return send_message(WebSocketOpcode::Binary, payload);
}
WebSocketResult WebSocket::ping(std::string_view payload) { return send_control(WebSocketOpcode::Ping, payload); }
WebSocketResult WebSocket::pong(std::string_view payload) { return send_control(WebSocketOpcode::Pong, payload); }

WebSocketResult WebSocket::close(std::uint16_t code, std::string_view reason) {
    if (!valid_close_code(code) || reason.size() > 123 || (m_config.validate_utf8 && !valid_utf8(reason)))
        return {WebSocketError::Protocol, 0, "invalid close code or reason"};
    std::lock_guard<sync::Mutex> lock(m_write_mutex);
    if (m_close_sent) {
        m_closed.store(true, std::memory_order_release);
        return WebSocketResult::success();
    }
    std::string payload;
    payload.reserve(2 + reason.size());
    payload.push_back(static_cast<char>(code >> 8));
    payload.push_back(static_cast<char>(code));
    payload.append(reason.data(), reason.size());
    const auto result = write_frame_unlocked(WebSocketOpcode::Close, payload, true);
    m_close_sent = true;
    m_closed.store(true, std::memory_order_release);
    abort_transport();
    return result;
}

bool WebSocket::valid_close_code(std::uint16_t code) noexcept {
    return (code >= 1000 && code <= 1014 && code != 1004 && code != 1005 && code != 1006 && code != 1015) ||
           (code >= 3000 && code <= 4999);
}

bool WebSocket::valid_utf8(std::string_view value) noexcept {
    std::size_t index = 0;
    while (index < value.size()) {
        const auto c = static_cast<unsigned char>(value[index++]);
        if (c <= 0x7F) continue;
        std::size_t extra = 0;
        std::uint32_t code = 0;
        if ((c & 0xE0U) == 0xC0U) { extra = 1; code = c & 0x1FU; if (code == 0) return false; }
        else if ((c & 0xF0U) == 0xE0U) { extra = 2; code = c & 0x0FU; }
        else if ((c & 0xF8U) == 0xF0U) { extra = 3; code = c & 0x07U; }
        else return false;
        if (index + extra > value.size()) return false;
        for (std::size_t i = 0; i < extra; ++i) {
            const auto next = static_cast<unsigned char>(value[index++]);
            if ((next & 0xC0U) != 0x80U) return false;
            code = (code << 6) | (next & 0x3FU);
        }
        if ((extra == 1 && code < 0x80U) || (extra == 2 && code < 0x800U) ||
            (extra == 3 && code < 0x10000U) ||
            code > 0x10FFFFU || (code >= 0xD800U && code <= 0xDFFFU)) return false;
    }
    return true;
}

std::string WebSocket::make_accept_key(std::string_view key) {
    const auto digest = sha1(std::string(key) + std::string(kWebSocketGuid));
    return encode_base64(digest.data(), digest.size());
}

std::string WebSocket::make_client_key() {
    std::array<std::uint8_t, 16> random_bytes{};
    try {
        std::random_device random;
        for (auto& byte : random_bytes) byte = static_cast<std::uint8_t>(random());
    } catch (...) {
        return {};
    }
    return encode_base64(random_bytes.data(), random_bytes.size());
}

}  // namespace go2cpp::net
