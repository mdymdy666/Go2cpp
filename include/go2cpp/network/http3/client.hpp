/**
 * @file client.hpp
 * @brief 基于 picoquic 自带 `picohttp`（h3zero + QPACK）的 HTTP/3 客户端薄封装。
 *
 * ## 为什么复用 `picohttp` 而不是自己写 QPACK
 *
 * HTTP/3 的难点不在 QUIC（已由 `net::quic::PicoquicConnection` 封装），而在
 * **QPACK 编解码**：静态表 99 项、动态表、Huffman 编码、field section 前缀、
 * encoder/decoder 指令流。任何一项写错都表现为"对端直接断连"，极难调试。
 *
 * picoquic 仓库自带 `picohttp/`，其中：
 *
 * - `h3zero.c` —— QPACK 静态表、整数/字符串编解码、请求/响应 HEADERS 构造函数
 * - `h3zero_common.c` —— HTTP/3 帧解析状态机（`h3zero_parse_data_stream`）
 * - `h3zero_client.c` —— 客户端请求的帧拼装示例
 * - `democlient.c` —— 客户端回调的完整示例
 *
 * 本文件**只做协议层与项目 `Transport` 的粘合**：
 *
 * - 发：用 `h3zero_create_request_header_frame_ex` /
 *   `h3zero_create_post_header_frame_ex` 生成 QPACK field section，
 *   再用 QUIC varint 包成 HEADERS 帧；
 * - 收：用 `h3zero_parse_data_stream` 把字节流还原成 HEADERS/DATA 帧，
 *   取出 `:status` 与 body。
 *
 * **没有一行自写的 QPACK 编解码**。这是刻意的：自写 QPACK 是"QUIC 自实现
 * 1702 行然后全删"的同一个陷阱。
 *
 * ## 与 `PicoquicConnection` 的关系
 *
 * 本类**复用** `net::quic::PicoquicConnection` 驱动 QUIC 传输（握手、ACK、
 * 丢包重传、流控），只是在连接建立后把 picoquic 的应用回调换成 HTTP/3 的
 * 回调（`picoquic_set_callback`）。这样：
 *
 * - UDP/datagram 的读写仍然只经 `Transport`，不引入第二个线程；
 * - QUIC 状态机、定时器、重传全部由 picoquic 处理；
 * - HTTP/3 层只关心"流上这些字节是什么帧"。
 *
 * ## 已知边界（如实声明，不是"以后再说"）
 *
 * 1. **请求方法只支持 GET / POST**。`picohttp` 只提供这两个方法的构造函数，
 *    其他方法需要自行拼 QPACK 静态表引用 —— 不在本封装范围内。
 * 2. **响应头只能解出 `content-type`**。`picohttp` 的
 *    `h3zero_header_parts_t` 只保留少量"有意思"的字段（`:status`、
 *    `content-type`、`range` 等），任意名字的响应头会被解析器跳过。
 *    要拿到全部响应头必须扩展 `picohttp` 的解析器，而不是在这里重写 QPACK。
 * 3. **不使用 QPACK 动态表**。与 picoquic 的 demo 客户端一致：设置里不声明
 *    动态表容量，编码只用静态表 + literal。这避免了 encoder/decoder 指令流
 *    的同步问题，代价是头部压缩率较低。
 * 4. **非线程安全**：所有方法必须在同一线程 / 同一 Fiber 序列里调用。
 *
 * ## 用法
 *
 * ```
 * auto client = http3::Client::Connect(&udp, "localhost", &err);
 * http3::Request req;
 * req.path = "/index.html";
 * http3::Response resp;
 * if (client->Send(req, &err) && client->Receive(&resp, &err)) {
 *     // resp.status == 200, resp.alpn 由 client->Alpn() 给出
 * }
 * ```
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace go2cpp::net::http2 {
class Transport;
}

namespace go2cpp::net::http3 {

/**
 * @brief 一个 HTTP/3 请求。
 *
 * `body` 非空时按 POST 发送（HEADERS + DATA），否则按 GET 发送
 * （只有 HEADERS）。方法只能取 `GET` / `POST`（大小写不敏感），
 * 其他值会让 `Send` 失败并给出明确原因。
 */
struct Request {
    /// 请求方法，仅支持 "GET" / "POST"（大小写不敏感）。
    std::string method{"GET"};

    /// 请求路径，必须以 '/' 开头（例如 "/index.html"）。默认 "/"。
    std::string path{"/"};

    /**
     * @brief 附加请求头。
     *
     * - 名字会被强制转成小写（HTTP/3 要求小写）；
     * - 不允许伪头（名字以 ':' 开头）；
     * - `user-agent` 会被用作请求的 UA，而不是重复添加；
     * - 其余名字以 QPACK literal-with-name 形式追加在标准字段之后。
     */
    std::vector<std::pair<std::string, std::string>> headers;

    /// 请求体。非空时以数据帧发送，`method` 自动按 POST 处理。
    std::string body;
};

/**
 * @brief 一个 HTTP/3 响应。
 */
struct Response {
    /// HTTP 状态码（`:status`）。0 表示没解析到状态。
    int status{0};

    /**
     * @brief 响应头。
     *
     * **注意**：受 `picohttp` 解析器限制，这里目前只可能包含
     * `content-type`（见文件头"已知边界"第 2 条）。
     */
    std::vector<std::pair<std::string, std::string>> headers;

    /// 响应体（所有 DATA 帧拼接）。
    std::string body;

    /// 该响应来自哪个 QUIC 流（客户端发起的双向流：0, 4, 8, ...）。
    std::uint64_t stream_id{0};
};

/**
 * @brief 一条 HTTP/3 客户端连接。
 *
 * 生命周期：`Connect` 之后可反复 `Send` / `Receive`。多个请求按 `Send`
 * 顺序由 `Receive` 依次取回（HTTP/3 允许流并发，本封装按顺序交付）。
 *
 * **Transport 必须是 datagram 语义**（QUIC 基于 UDP），且生命周期必须长于
 * 本对象 —— 与 `net::quic::PicoquicConnection` 的约束相同。
 */
class Client {
public:
    /// 默认超时：握手/等待响应的总时长（毫秒）。
    static constexpr std::uint32_t kDefaultTimeoutMs = 5000;

    /**
     * @brief 建立到 `sni` 的 HTTP/3 连接（ALPN 固定为 "h3"）。
     *
     * **只创建对象并生成 ClientHello**，TLS 1.3 握手在第一次 `Send` /
     * `Receive` / `Poll` 时推进。连接成功后 `Alpn()` 返回 "h3"。
     *
     * @param transport 字节通道，**必须一次 `Read` 返回一个完整 datagram**。
     *                  不接管所有权。
     * @param sni TLS SNI，同时用作 `:authority`。不能为空。
     * @param error 失败原因；可为 nullptr。
     * @return 成功返回对象，失败返回 nullptr。
     */
    static std::unique_ptr<Client> Connect(net::http2::Transport* transport,
                                           const std::string& sni, std::string* error);

    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    /**
     * @brief 发送一个请求。
     *
     * 如果握手尚未完成，本方法会先驱动握手直到完成或超时。
     * 请求会立即排入 QUIC 发送队列；真正的数据在后续 `Poll` / `Receive`
     * 中由 picoquic 打包发出。对应的流号见 `LastStreamId()`。
     *
     * @param req 请求。GET/POST 之外的 method 会失败。
     * @param error 失败原因；可为 nullptr。
     * @return 成功返回 true。
     */
    bool Send(const Request& req, std::string* error);

    /**
     * @brief 等待并取回**最早发出的那个**请求的响应。
     *
     * 内部循环 `Poll` 直到该流的 HEADERS 解析完且收到 FIN，或超时。
     *
     * @param resp 输出响应；失败时不修改内容。
     * @param error 失败原因；可为 nullptr。
     * @return 成功返回 true。
     */
    bool Receive(Response* resp, std::string* error);

    /**
     * @brief 推进一次收发：写出所有待发包，读入一个 datagram 并交给 picoquic。
     *
     * @return 连接仍可用返回 true；连接已关闭/出错返回 false 并填 `error`。
     */
    bool Poll(std::string* error);

    /// TLS 1.3 握手是否已完成。
    bool HandshakeComplete() const noexcept;

    /// 连接是否已关闭（对端关闭、出错或本地关闭）。
    bool Closed() const noexcept;

    /// 协商出的 ALPN。握手前返回空串；成功时必须是 "h3"。
    std::string Alpn() const;

    /// 最近一次 Send 分配的流号。
    std::uint64_t LastStreamId() const noexcept;

    /// 最近一次连接级错误码（HTTP/3 或 QUIC 错误码）。
    std::uint64_t ConnectionError() const noexcept;

private:
    struct Impl;
    explicit Client(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> m_impl;
};

}  // namespace go2cpp::net::http3
