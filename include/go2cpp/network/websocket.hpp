#pragma once

/**
 * @file websocket.hpp
 * @brief RFC 6455 WebSocket 握手、帧编解码、分片消息和关闭流程。
 * @details WebSocket 建立在已经连接的 net::Socket 上，读写分别串行化，
 *          可在普通线程或 IOManager 管理的 Fiber 中使用。协议层不拥有调度器，
 *          Socket 的阻塞/唤醒策略由 Socket 自己决定。
 *
 * ## 模块定位
 * RFC 6455 应用层：在**已经建立**的 `net::Socket` 上跑 WebSocket。它不做
 * 监听/连接/路由，握手只负责收发 Upgrade 报文；字节读写全部委托给 `Socket`，
 * 因此普通线程与 `IOManager` 的 Fiber 都能用。
 *
 * ## 本文件定义什么
 * - `WebSocketRole` / `WebSocketOpcode`：角色与帧操作码；
 * - `WebSocketError` / `WebSocketResult` / `WebSocketMessage`：错误分类、结果与消息；
 * - `WebSocketConfig`：帧/消息上限、IO 超时、auto_pong、UTF-8 校验开关；
 * - `WebSocket`：握手（`Accept*`/`Connect`）、消息收发、Ping/Pong、关闭。
 *
 * ## 阅读建议
 * 先看角色与掩码规则（客户端必须掩码），再看 `read_message()` 的分片重组，
 * 最后对照 `docs/state-machines/WebSocket.md` 的握手 / 关闭 / 帧读取阶段表。
 *
 * ## 边界与陷阱
 * - 客户端发出的帧必须带掩码、服务端必须不带（RFC 6455 §5.1），反向即协议错误；
 * - 一个对象允许「单读、多写」：`m_read_mutex` 串行化读，`m_write_mutex` 串行化写；
 * - `Accept()` / `Connect()` 失败时**不关闭**调用方持有的 Socket（所有权仍归调用方）；
 * - 控制帧（Ping/Pong/Close）payload <= 125 且不允许分片。
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
 *
 * ## 定位
 * 由上层业务/NetService 通过 `Accept()`/`Connect()` 创建并持有 `Ptr`；
 * 依赖 `net::Socket`、`http::Request/Response` 与 `SocketStream`；与 `sse` 模块
 * 并列 —— 二者都是「在已建立连接上跑应用协议」。
 *
 * ## 生命周期与线程模型
 * `std::shared_ptr` 持有；读方向单线程（`m_read_mutex`），写方向可多线程
 * （`m_write_mutex`）。析构调 `abort_transport()` 唤醒其他 M/Fiber 上的阻塞读写，
 * 但**不 close fd**（Socket 所有权可能与他人共享）。
 *
 * ## 状态机
 * 状态转化表见 `docs/state-machines/WebSocket.md`
 * （握手 / 关闭流程 / 帧读取阶段 / 分片重组四张表）。
 *
 * ## 不变量
 * - `closed()` 为真后所有发送返回 `Closed`，不会再有字节上线；
 * - `m_close_sent` 为真时 `close()` 幂等成功；
 * - 分片期间 `m_fragmented` 为真，只接受 Continuation 帧；
 * - 客户端发出的帧必带掩码，服务端发出的帧必不带。
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
    /**
     * @brief 阻塞读取并重组一条完整业务消息，Ping/Pong 在内部处理。
     *
     * ## 步骤
     * step1. 取读锁并检查 `m_closed`；
     * step2. `read_frame()` 读一个原始帧（长度/掩码/payload 校验都在里面）；
     * step3. 控制帧优先处理：Ping 自动回 Pong、Pong 丢弃、Close 走关闭流程；
     * step4. 数据帧：首片记录 opcode，Continuation 追加到 `m_fragment_payload`；
     * step5. 收到 FIN 后交付消息，文本消息按 `validate_utf8` 校验。
     *
     * ## 失败语义
     * 协议错误走 `protocol_error()`：先发对应 close code，再返回 `Protocol`。
     */
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
        /// 是否 FIN（消息/分片结束）；read_frame() 写入、read_message() 读取。
        bool fin{false};
        /// 帧操作码；read_frame() 校验后写入，write_frame_unlocked() 构造帧时使用。
        WebSocketOpcode opcode{WebSocketOpcode::Continuation};
        /// 是否带掩码；read_frame() 按角色校验方向，write_frame_unlocked() 决定是否加掩码。
        bool masked{false};
        /// 帧 payload（读取时已解掩码）；read_frame() 填充，read_message() 消费。
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

    /// 底层已连接 socket（所有权在本类）；所有读写都经它。
    Socket::Ptr m_socket;
    /// 角色（Client/Server）；构造时固定，决定掩码方向与握手形态。
    WebSocketRole m_role;
    /// 解析与背压配置；构造时固定，read_frame()/send_* 读取。
    WebSocketConfig m_config;
    /// 关闭标记（原子）；close()/写失败/析构置位，closed() 与发送路径读取。
    std::atomic<bool> m_closed{false};
    // 使用项目的混合 Mutex，避免受管 Fiber 在同一个 M 上阻塞 OS 线程。
    /// 读锁（Fiber/线程混合）；read_message() 持有，串行化读方向。
    sync::Mutex m_read_mutex;
    /// 写锁（Fiber/线程混合）；write_frame()/close() 持有，保证帧不交错。
    sync::Mutex m_write_mutex;
    /// 是否已发 Close 帧；close()/收到 Close 时置位，用于幂等。
    bool m_close_sent{false};
    /// 是否正在重组分片消息；read_message() 读写，分片完成时清零。
    bool m_fragmented{false};
    /// 分片消息的原始 opcode（Text/Binary）；首片记录，重组完成时决定消息类型。
    WebSocketOpcode m_fragment_opcode{WebSocketOpcode::Continuation};
    /// 已累积的分片 payload；首片初始化、Continuation 追加、完成时 move 出去。
    std::string m_fragment_payload;
    /// 握手时 Parser 多读的字节；AcceptWithPending() 注入，read_exact() 优先消费。
    std::string m_prefetched;
    /// m_prefetched 的读游标；read_exact() 推进。
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
