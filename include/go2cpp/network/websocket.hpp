#pragma once

/**
 * @file websocket.hpp
 * @brief RFC 6455 WebSocket 握手、帧编解码、分片消息和关闭流程。
 * @details WebSocket 建立在已经连接的 net::Socket 上，读写分别串行化，
 *          可在普通线程或 IOManager 管理的 Fiber 中使用。协议层不拥有调度器，
 *          Socket 的阻塞/唤醒策略由 Socket 自己决定。
 */

#include "go2cpp/network/http.hpp"
#include "go2cpp/sync.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace go2cpp::net {

/** @brief WebSocket 端点角色；角色决定收发帧是否必须使用掩码。 */
enum class WebSocketRole { Client, Server };

/** @brief RFC 6455 帧操作码。 */
enum class WebSocketOpcode : std::uint8_t {
    Continuation = 0x0,
    Text = 0x1,
    Binary = 0x2,
    Close = 0x8,
    Ping = 0x9,
    Pong = 0xA
};

/** @brief WebSocket API 结果类别。 */
enum class WebSocketError {
    None,
    Closed,
    Timeout,
    Io,
    Protocol,
    InvalidHandshake,
    MessageTooLarge,
    InvalidState
};

/** @brief WebSocket 操作结果；失败时 message 提供可记录的诊断信息。 */
struct WebSocketResult {
    WebSocketError error{WebSocketError::None};
    int system_error{0};
    std::string message;
    bool ok() const noexcept { return error == WebSocketError::None; }
    static WebSocketResult success() { return {}; }
};

/** @brief 已经组装完成的一条 WebSocket 业务消息。 */
struct WebSocketMessage {
    WebSocketOpcode opcode{WebSocketOpcode::Text};
    std::string payload;
    bool text() const noexcept { return opcode == WebSocketOpcode::Text; }
};

/** @brief WebSocket 解析和背压限制。 */
struct WebSocketConfig {
    std::size_t max_frame_payload{16 * 1024 * 1024};
    std::size_t max_message_payload{64 * 1024 * 1024};
    Socket::Duration io_timeout{Socket::Duration::max()};
    bool auto_pong{true};
    bool validate_utf8{true};
};

/**
 * @brief 单连接 WebSocket 协议对象。
 * @details 依赖 `net::Socket`、HTTP Header 和 SocketStream；向上层提供握手、
 *          文本/二进制消息、Ping/Pong、关闭和分片重组服务。一个对象允许一个
 *          读方向和多个并发写调用，读操作之间、写操作之间分别串行化；同一条
 *          连接的业务消息顺序由写锁保证。
 * @note WebSocket 不负责路由、鉴权、连接池和服务发现；这些能力应由上层 NetService 注入。
 */
class WebSocket final : public std::enable_shared_from_this<WebSocket> {
public:
    using Ptr = std::shared_ptr<WebSocket>;

    /** @brief 校验 HTTP Upgrade 请求并提取客户端 Sec-WebSocket-Key。 */
    static bool ValidateUpgradeRequest(const http::Request& request,
                                       std::string& key,
                                       std::string* reason = nullptr);
    /** @brief 根据合法 Upgrade 请求生成 101 Switching Protocols 响应。 */
    static http::Response BuildUpgradeResponse(const http::Request& request);
    /** @brief 服务端完成握手并创建 WebSocket；失败不关闭调用方持有的 Socket。 */
    static Ptr Accept(Socket::Ptr socket, const http::Request& request,
                      WebSocketConfig config = {}, WebSocketResult* result = nullptr);
    /** @brief 接管 HTTP Parser 已经读入但尚未消费的首段 WebSocket 字节。 */
    static Ptr AcceptWithPending(Socket::Ptr socket, const http::Request& request,
                                 std::string pending, WebSocketConfig config = {},
                                 WebSocketResult* result = nullptr);
    /** @brief 客户端发送 Upgrade 请求、验证 101 响应并创建 WebSocket。 */
    static Ptr Connect(Socket::Ptr socket, std::string host, std::string target = "/",
                       WebSocketConfig config = {}, WebSocketResult* result = nullptr);

    ~WebSocket();
    WebSocket(const WebSocket&) = delete;
    WebSocket& operator=(const WebSocket&) = delete;

    /** @brief 阻塞读取并重组一条完整业务消息，Ping/Pong 在内部处理。 */
    WebSocketResult read_message(WebSocketMessage& message);
    /** @brief 发送文本消息；payload 为空也是合法的空文本消息。 */
    WebSocketResult send_text(std::string_view payload);
    /** @brief 发送二进制消息。 */
    WebSocketResult send_binary(std::string_view payload);
    /** @brief 发送 Ping 控制帧，payload 不得超过 125 字节。 */
    WebSocketResult ping(std::string_view payload = {});
    /** @brief 发送 Pong 控制帧。 */
    WebSocketResult pong(std::string_view payload = {});
    /** @brief 发送 Close 控制帧并进入关闭状态；重复调用是幂等的。 */
    WebSocketResult close(std::uint16_t code = 1000, std::string_view reason = {});

    WebSocketRole role() const noexcept { return m_role; }
    bool closed() const noexcept { return m_closed.load(std::memory_order_acquire); }
    Socket::Ptr socket() const noexcept { return m_socket; }

private:
    struct Frame {
        bool fin{false};
        WebSocketOpcode opcode{WebSocketOpcode::Continuation};
        bool masked{false};
        std::vector<std::uint8_t> payload;
    };

    WebSocket(Socket::Ptr socket, WebSocketRole role, WebSocketConfig config,
              std::string pending = {});
    WebSocketResult read_exact(void* data, std::size_t size);
    WebSocketResult write_all(const void* data, std::size_t size);
    WebSocketResult read_frame(Frame& frame);
    WebSocketResult write_frame(WebSocketOpcode opcode, std::string_view payload,
                                bool fin = true);
    WebSocketResult write_frame_unlocked(WebSocketOpcode opcode,
                                         std::string_view payload, bool fin);
    WebSocketResult send_control(WebSocketOpcode opcode, std::string_view payload);
    /** @brief 发送指定 RFC close code 并将连接置为不可复用。 */
    WebSocketResult protocol_error(std::string message, std::uint16_t code = 1002);
    /** @brief 取消底层读写等待，唤醒跨线程/跨 Fiber 的阻塞调用。 */
    void abort_transport() noexcept;
    static bool valid_utf8(std::string_view value) noexcept;
    static bool valid_close_code(std::uint16_t code) noexcept;
    static bool header_token(std::string_view value, std::string_view token) noexcept;
    static std::string make_accept_key(std::string_view key);
    static std::string make_client_key();
    static void set_result(WebSocketResult* output, const WebSocketResult& result);

    Socket::Ptr m_socket;
    WebSocketRole m_role;
    WebSocketConfig m_config;
    std::atomic<bool> m_closed{false};
    // 使用项目的混合 Mutex，避免受管 Fiber 在同一个 M 上阻塞 OS 线程。
    sync::Mutex m_read_mutex;
    sync::Mutex m_write_mutex;
    bool m_close_sent{false};
    bool m_fragmented{false};
    WebSocketOpcode m_fragment_opcode{WebSocketOpcode::Continuation};
    std::string m_fragment_payload;
    std::string m_prefetched;
    std::size_t m_prefetched_offset{0};
};

}  // namespace go2cpp::net

namespace go2cpp {
using WebSocket = net::WebSocket;
using WebSocketConfig = net::WebSocketConfig;
using WebSocketMessage = net::WebSocketMessage;
using WebSocketResult = net::WebSocketResult;
using WebSocketError = net::WebSocketError;
using WebSocketOpcode = net::WebSocketOpcode;
}  // namespace go2cpp
