/**
 * @file client.hpp
 * @brief 基于 picoquic 自带 `picohttp`（h3zero + QPACK）的 HTTP/3 客户端薄封装。
 *
 * ## 模块定位
 *
 * 本文件位于 HTTP 客户端协议栈的**最上层**（应用直接可见的那一层）：
 *
 * ```
 *   Client（本文件）        HTTP/3：HEADERS/DATA 帧、:status、响应体
 *      |  换掉 picoquic 回调
 *   PicoquicConnection      QUIC：握手、ACK、重传、流控（net::quic）
 *      |  datagram 读写
 *   Transport              字节通道（net::http2::Transport，UDP 或内存实现）
 * ```
 *
 * - **下游依赖**：`net::quic::PicoquicConnection` 只在 `client.cpp` 里 include，
 *   本头文件仅前置声明 `net::http2::Transport`，因此不会把 QUIC/picoquic 的
 *   头文件传染给调用方；QPACK 编解码全部交给 picohttp 的 h3zero。
 * - **上游调用方**：就当前仓库而言，**没有生产代码调用本文件** ——
 *   `grep -rn 'http3::'` 只命中本头文件与 `src/network/http3/client.cpp`。
 *   它是为「需要 HTTP/3 客户端」的后续模块（以及基于内存 Transport 的回环
 *   自测）准备的 API，而不是被某个既有业务模块驱动。
 * - **与 http2 的关系**：仅复用 `http2::Transport` 这个字节通道抽象，不共享
 *   `http2::Connection` 的帧/状态机（HTTP/2 与 HTTP/3 的帧层差别太大）。
 *
 * ## 本文件定义什么
 *
 * - `http3::Request`：一次请求的输入（method/path/headers/body），隐含
 *   「body 非空即按 POST 编码」的选路规则；
 * - `http3::Response`：一次请求的输出（status/headers/body/stream_id），
 *   其中 headers 受 picohttp 解析器限制，只可能包含 `content-type`；
 * - `http3::Client`：一条 HTTP/3 连接的门面（Connect/Send/Receive/Poll 与
 *   若干只读查询），用 pimpl 隐藏 picoquic/h3zero 细节；
 * - `Client::kDefaultTimeoutMs`：握手与单次等待响应的统一超时（5000 ms）。
 *
 * 本文件**只有声明**，全部实现（回调、帧解析、picoquic 交互）在
 * `src/network/http3/client.cpp`；想弄清楚「为什么这样发」应去读那个文件。
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
 *
 * ## 阅读建议
 *
 * 1. 先读下面的「与 `PicoquicConnection` 的关系」，建立「QUIC 由 picoquic
 *    驱动、本类只在流上拼 HTTP/3 帧」的心智模型；
 * 2. 再看 `Request` / `Response` 两个纯数据 struct，确认 GET/POST 的推导规则；
 * 3. 然后看 `Client` 的成员顺序：`Connect`（只建对象）→ `Send`（推握手 +
 *    排队）→ `Receive`（FIFO 等响应）→ `Poll`（单步收发）；
 * 4. 最后读 `client.cpp` 文件头的「三个关键实现点」，尤其是「回调是换掉而不是
 *    再包一层」——这决定了 `PicoquicConnection` 的握手/读流接口在本类里不可用。
 *
 * ## 边界与陷阱
 *
 * **未实现的功能边界（如实声明，不是 TODO）**：
 * - 方法只支持 GET / POST；其他 method 在 `Send` 里失败（且要先等握手完成，
 *   见下）；
 * - 响应头只解出 `content-type`，任意名字的响应头被 picohttp 解析器跳过；
 * - 不使用 QPACK 动态表（只用静态表 + literal），压缩率较低；
 * - 超时**不可配置**：`Impl::timeout_ms` 初值就是 `kDefaultTimeoutMs`，
 *   本类没有 setter / 构造参数能改它；
 * - `Connect` 里 `verify_peer = false`（沿用项目现有 QUIC 封装的本地自签
 *   默认），生产环境必须另行打开证书校验；
 * - 没有 0-RTT、没有服务端推送处理、没有连接迁移/多路径。
 *
 * **本文件没有未实现的桩**：`client.cpp` 中 `Connect` / `Send` / `Receive` /
 * `Poll` / `HandshakeComplete` / `Closed` / `Alpn` / `LastStreamId` /
 * `ConnectionError` 均有实体实现，无空函数体、无 `TODO`、无「直接返回默认
 * 值」的占位。上面列的是**设计上不做**的能力，不要误当成待补的桩。
 *
 * **容易踩的反直觉点**：
 * - `Connect` 只建对象并生成 ClientHello，**不做握手**；握手在第一次
 *   `Send`/`Receive`/`Poll` 时推进。因此 `Connect` 成功不代表对端可达。
 * - `Send` 先驱动握手（最多 `kDefaultTimeoutMs`），**之后**才校验 method/path；
 *   传了非法 method 也要先等握手，参数错误的反馈可能被握手超时掩盖。
 * - `Send` 只是把字节交给 picoquic 的发送队列，真正发包发生在后续
 *   `Poll`/`Receive`；只 `Send` 不 `Poll`，对端永远收不到。
 * - `Receive` 按 `Send` 的 FIFO 顺序交付（HTTP/3 本身允许流间乱序完成）；
 *   某个流卡住会阻塞它后面所有响应的交付（受统一超时约束）。
 * - `Receive` 超时/失败返回 false 时**不清理** `pending`/`streams`（只有成功
 *   或流级失败才 pop），可以再次调用重试，但超时时间会重新从头计算。
 * - POST 的 `content-type` 被硬编码为 `text/plain`；若 `headers` 里再给一个
 *   `content-type`，会以 literal 追加成**第二个**同名字段，由对端决定取舍。
 * - `Closed()` 在 `m_impl == nullptr`（对象被 move 走后的空壳）时返回 true；
 *   `Alpn()` 未协商时返回空串，不要当成 "h3"。
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
 *
 * ## 定位
 * 纯输入结构：调用方填好后传给 `Client::Send`；`Send` 内部把它交给
 * `BuildRequestBytes`（`client.cpp` 匿名 namespace）编码成一条流上的
 * HEADERS [+ DATA] 字节。本结构不持有任何连接状态，可反复复用。
 *
 * ## 不变量
 * - `method` 大小写不敏感，但语义上只有 GET / POST 两种；其他值必然让
 *   `Send` 返回 false（不会静默降级成 GET）；
 * - `path` 必须以 '/' 开头，否则 `Send` 失败；
 * - `headers` 里的名字在编码时统一转小写，并拒绝 ':' 开头的伪头；
 *   `user-agent` 被抽出来单独作为 UA，不会重复追加；
 * - `body` 为空时只发 HEADERS（GET 语义），非空时追加 DATA 帧；
 *   注意 `method` 与 `body` 不做交叉校验：`method="GET"` + 非空 body 仍按
 *   GET 编码，但会带上 DATA 帧。
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
 *
 * ## 定位
 * 纯输出结构：由 `Client::Receive` 在该流收到 FIN 且 HEADERS 已解析后整体
 * 赋值（`*resp = stream.response`）。调用方不需要预先清零；失败时
 * `Receive` 不会写它。
 *
 * ## 不变量
 * - `status == 0` 表示没有解析到 `:status`（正常响应应是 1xx~5xx）；
 * - `headers` 至多包含一个 `content-type`（picohttp 解析器的限制）；
 * - `body` 是全部 DATA 帧的拼接，不含 HEADERS 字段段；
 * - `stream_id` 是该响应所在的客户端双向流（0, 4, 8, ...），由 `Send`
 *   分配并在 `Receive` 时原样带出，可用于把响应与请求对上号。
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
 *
 * ## 定位
 * HTTP/3 客户端门面：把 `http2::Transport` 的字节通道与 picoquic 的 QUIC
 * 连接，包装成「Connect → Send → Receive」的请求/响应接口。上层只需这一个
 * 类型，不必接触 picoquic / h3zero 的任何类型（pimpl 隔离）。
 *
 * ## 生命周期与线程模型
 * - `Connect` 返回 `unique_ptr<Client>`，所有权唯一，拷贝被删除；
 * - `Transport` 与底层 picoquic 连接的生命周期都必须长于本对象；析构时先销毁
 *   `Impl`（内部 `connection.reset()` 会触发一次 close 回调）；
 * - **非线程安全**：所有方法必须在同一线程 / 同一 Fiber 序列里调用；内部没有
 *   锁，`streams` / `pending` 都是普通容器。
 *
 * ## 不变量
 * - `m_impl` 非空（除非对象被 move 走，此时查询接口退化为 Closed()==true、
 *   Alpn() 空串、错误码 0）；
 * - `pending` 中每个 stream_id 都在 `streams` 里有对应的 `StreamState`；
 * - `streams` 只保存客户端发起的双向流（stream_id & 0x3 == 0），服务端控制流
 *   与 QPACK 单向流在回调里被直接忽略；
 * - 每个 `StreamState` 在 `header_captured` 之前不会被当作可交付响应。
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
