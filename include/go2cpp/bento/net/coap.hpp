#pragma once

/**
 * @file coap.hpp
 * @brief CoAP（RFC 7252）最小骨架：报文编解码 + 选项 delta/length 编码 +
 *        简单客户端/服务端交换（GET/PUT/POST 与 2.05/2.04/2.01 响应）。
 *
 * @details 本模块只依赖 C++17 标准库，面向「无第三方库」的受限场景，
 *          提供 CoAP 的**线格式**与**最小交换语义**，不实现观察（Observe）、
 *          分块传输（Block1/Block2）、代理与 DTLS。
 *
 * ## 线格式
 *
 * ```
 *  0                   1                   2                   3
 *  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |Ver| T |  TKL  |      Code     |          Message ID           |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |   Token（TKL 字节）...
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |   Options（按编号升序，delta/length 编码）...
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |1 1 1 1 1 1 1 1|    Payload（可选，标记后至少 1 字节）...
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * ```
 *
 * - `Version` 固定 1；`Type` 为 CON/NON/ACK/RST；`TKL` 为 token 长度（0..8），
 *   9..15 是保留值，解码时按格式错误拒绝；
 * - `Code` 的高 3 位是 class（0 请求、2 成功、4 客户端错误、5 服务端错误），
 *   低 5 位是 detail；`0.00` 表示空消息（仅用于 ACK/RST 或 ping）；
 * - 选项前缀字节的高 4 位是 `delta`、低 4 位是 `length`。两者取值 0..12 直接
 *   表示；13 表示「后续 1 字节 + 13」；14 表示「后续 2 字节 + 269」；15 保留
 *   （仅 `0xFF` 整体作为 Payload Marker）。同一编号的选项可以重复出现
 *   （Uri-Path 就是靠重复表达多段路径），delta 为 0 即可；
 * - Payload Marker 之后**必须**至少 1 字节，否则按格式错误拒绝。
 *
 * ## 已实现的交换语义
 *
 * - 客户端：`Send()` 组装请求（自动分配 Message ID 与 Token），`Poll()` 驱动
 *   指数退避重传，`HandleDatagram()` 消费 ACK/RST/响应并校验 Token；
 * - 服务端：`Handle()` 解码请求、按 (Message ID, Token) 去重，生成
 *   piggyback ACK（CON 请求）或 NON 响应（NON 请求），再交给 `CoapSendFn`；
 * - 无 handler 时使用 `DefaultResponse()` 的方法映射：GET→2.05、PUT→2.04、
 *   POST→2.01、DELETE→2.02、其他→4.05。
 *
 * ## 骨架边界（明确不做的部分）
 *
 * - 无 DTLS/OSCORE，无 Observe/Blockwise/Proxy；
 * - 客户端同时只保留**一个**待确认请求（`Send()` 在已有 pending 时返回
 *   false）；服务端去重缓存只保留最近 `dedup_cache_size` 条，没有 RFC 要求的
 *   247s EXCHANGE_LIFETIME 时间窗；
 * - 不自动重发「响应丢失」后的查询（CoAP 的 separate response 只支持
 *   piggyback 与「ACK 后再来 CON 响应」两种路径，不实现客户端主动查询）；
 * - 无抖动：重传间隔从 `ack_timeout_ms` 起严格翻倍，便于测试复现；
 * - 类不加锁，由调用方保证单线程事件循环语义（与 `Link` 相同约定）。
 *
 * ## 典型用法
 *
 * @code
 *   go2cpp::bento::net::CoapServer server([](const std::vector<std::uint8_t>& wire) {
 *       // 把 wire 交给 UDP socket 发送到请求方
 *       return true;
 *   });
 *   server.SetHandler([](const go2cpp::bento::net::CoapMessage& request) {
 *       go2cpp::bento::net::CoapResponse response;
 *       response.code = go2cpp::bento::net::CoapCode::kContent;
 *       response.content_format = go2cpp::bento::net::kCoapContentFormatTextPlain;
 *       response.payload = {'o', 'k'};
 *       return response;
 *   });
 *
 *   go2cpp::bento::net::CoapClient client(send_fn);
 *   client.OnEvent([](go2cpp::bento::net::CoapClientEvent event,
 *                     const go2cpp::bento::net::CoapMessage& message) { ... });
 *   client.SendRequest(go2cpp::bento::net::CoapCode::kGet, "/sensors/temp");
 *   client.Poll();  // 周期调用：驱动重传与超时
 * @endcode
 *
 * @author go2cpp contributors
 * @date 2026-10-30
 * @copyright MIT License
 */

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace go2cpp::bento::net {

/// CoAP 版本号（RFC 7252 固定为 1；其他值一律拒绝）。
inline constexpr std::uint8_t kCoapVersion = 1;

/// Token 字节数上限：TKL 为 4 bit，0..8 有效，9..15 是保留值。
inline constexpr std::size_t kCoapMaxTokenBytes = 8;

/// 单条报文字节数上限（含头/Token/选项/载荷）。
/// RFC 7252 建议消息不超过 1152 字节，这里放宽到 1 MiB，只为让选项长度编码的
/// 完整范围（单个选项最多 65804 字节）可测；生产环境应由传输层限制到 MTU。
inline constexpr std::uint32_t kCoapMaxMessageBytes = 1024u * 1024u;

/// 选项编号上限：RFC 7252 的 Option Number 是 16 位无符号数。
inline constexpr std::uint32_t kCoapMaxOptionNumber = 65535u;

/// 单个选项值长度上限：length 用 14 + 2 字节扩展时最大 65804。
inline constexpr std::uint32_t kCoapMaxOptionValueBytes = 65804u;

// ---------------------------------------------------------------------------
// 选项编号（RFC 7252 §12.2；只列出骨架用得到/常见的部分）
// ---------------------------------------------------------------------------

inline constexpr std::uint16_t kCoapOptionIfMatch = 1;             ///< If-Match。
inline constexpr std::uint16_t kCoapOptionUriHost = 3;             ///< Uri-Host。
inline constexpr std::uint16_t kCoapOptionETag = 4;                ///< ETag。
inline constexpr std::uint16_t kCoapOptionIfNoneMatch = 5;         ///< If-None-Match。
inline constexpr std::uint16_t kCoapOptionObserve = 6;             ///< Observe（骨架不解释）。
inline constexpr std::uint16_t kCoapOptionUriPort = 7;             ///< Uri-Port。
inline constexpr std::uint16_t kCoapOptionLocationPath = 8;        ///< Location-Path。
inline constexpr std::uint16_t kCoapOptionUriPath = 11;            ///< Uri-Path。
inline constexpr std::uint16_t kCoapOptionContentFormat = 12;      ///< Content-Format。
inline constexpr std::uint16_t kCoapOptionMaxAge = 14;             ///< Max-Age。
inline constexpr std::uint16_t kCoapOptionUriQuery = 15;           ///< Uri-Query。
inline constexpr std::uint16_t kCoapOptionAccept = 17;             ///< Accept。
inline constexpr std::uint16_t kCoapOptionLocationQuery = 20;      ///< Location-Query。
inline constexpr std::uint16_t kCoapOptionBlock2 = 23;             ///< Block2（骨架不解释）。
inline constexpr std::uint16_t kCoapOptionBlock1 = 27;             ///< Block1（骨架不解释）。
inline constexpr std::uint16_t kCoapOptionSize2 = 28;              ///< Size2。
inline constexpr std::uint16_t kCoapOptionProxyUri = 35;           ///< Proxy-Uri。
inline constexpr std::uint16_t kCoapOptionProxyScheme = 39;        ///< Proxy-Scheme。
inline constexpr std::uint16_t kCoapOptionSize1 = 60;              ///< Size1。

// ---------------------------------------------------------------------------
// Content-Format 注册值（RFC 7252 §12.3；子集）
// ---------------------------------------------------------------------------

inline constexpr std::uint16_t kCoapContentFormatTextPlain = 0;      ///< text/plain; charset=utf-8。
inline constexpr std::uint16_t kCoapContentFormatLinkFormat = 40;    ///< application/link-format。
inline constexpr std::uint16_t kCoapContentFormatXml = 41;           ///< application/xml。
inline constexpr std::uint16_t kCoapContentFormatOctetStream = 42;   ///< application/octet-stream。
inline constexpr std::uint16_t kCoapContentFormatExi = 47;           ///< application/exi。
inline constexpr std::uint16_t kCoapContentFormatJson = 50;          ///< application/json。
inline constexpr std::uint16_t kCoapContentFormatCbor = 60;          ///< application/cbor。

/**
 * @brief 报文类型（RFC 7252 §3 的 2 bit Type 字段）。
 */
enum class CoapType : std::uint8_t {
    kCon = 0,  ///< Confirmable：需要 ACK 确认，未收到时重传。
    kNon = 1,  ///< Non-confirmable：不重传，可丢失。
    kAck = 2,  ///< Acknowledgement：确认某个 CON，也可 piggyback 携带响应。
    kRst = 3,  ///< Reset：拒绝对端消息（例如无法解析或不需要的 CON）。
};

/// @brief 类型的可读名称（日志/测试断言用）。
const char* CoapTypeName(CoapType type) noexcept;

/**
 * @brief 方法/响应码（RFC 7252 §12.1，8 bit：3 bit class + 5 bit detail）。
 *
 * @details 枚举只列出骨架定义的标准码；线上未知码解码时仍保留原始字节
 *          （`static_cast<CoapCode>(byte)`），由 `CoapCodeClass()` 判断类别。
 */
enum class CoapCode : std::uint8_t {
    kEmpty = 0x00,   ///< 0.00 空消息（ACK/RST 或 ping）。
    kGet = 0x01,     ///< 0.01 GET。
    kPost = 0x02,    ///< 0.02 POST。
    kPut = 0x03,     ///< 0.03 PUT。
    kDelete = 0x04,  ///< 0.04 DELETE。

    kCreated = 0x41,  ///< 2.01 Created。
    kDeleted = 0x42,  ///< 2.02 Deleted。
    kValid = 0x43,    ///< 2.03 Valid。
    kChanged = 0x44,  ///< 2.04 Changed。
    kContent = 0x45,  ///< 2.05 Content。

    kBadRequest = 0x80,                ///< 4.00 Bad Request。
    kUnauthorized = 0x81,              ///< 4.01 Unauthorized。
    kBadOption = 0x82,                 ///< 4.02 Bad Option。
    kForbidden = 0x83,                 ///< 4.03 Forbidden。
    kNotFound = 0x84,                  ///< 4.04 Not Found。
    kMethodNotAllowed = 0x85,          ///< 4.05 Method Not Allowed。
    kNotAcceptable = 0x86,             ///< 4.06 Not Acceptable。
    kPreconditionFailed = 0x8c,        ///< 4.12 Precondition Failed。
    kRequestEntityTooLarge = 0x8d,     ///< 4.13 Request Entity Too Large。
    kUnsupportedContentFormat = 0x8f,  ///< 4.15 Unsupported Content-Format。

    kInternalServerError = 0xa0,   ///< 5.00 Internal Server Error。
    kNotImplemented = 0xa1,        ///< 5.01 Not Implemented。
    kBadGateway = 0xa2,            ///< 5.02 Bad Gateway。
    kServiceUnavailable = 0xa3,    ///< 5.03 Service Unavailable。
    kGatewayTimeout = 0xa4,        ///< 5.04 Gateway Timeout。
    kProxyingNotSupported = 0xa5,  ///< 5.05 Proxying Not Supported。
};

/// @brief 取 code 的 class（高 3 位）。
constexpr std::uint8_t CoapCodeClass(CoapCode code) noexcept {
    return static_cast<std::uint8_t>(static_cast<std::uint8_t>(code) >> 5);
}

/// @brief 取 code 的 detail（低 5 位）。
constexpr std::uint8_t CoapCodeDetail(CoapCode code) noexcept {
    return static_cast<std::uint8_t>(static_cast<std::uint8_t>(code) & 0x1fu);
}

/// @brief 是否为请求码（0.01..0.04）。
constexpr bool IsCoapRequestCode(CoapCode code) noexcept {
    return CoapCodeClass(code) == 0 && CoapCodeDetail(code) >= 1 &&
           CoapCodeDetail(code) <= 4;
}

/// @brief 是否为响应码（class 2/4/5；class 3 是保留值，不算响应）。
constexpr bool IsCoapResponseCode(CoapCode code) noexcept {
    const std::uint8_t code_class = CoapCodeClass(code);
    return code_class == 2 || code_class == 4 || code_class == 5;
}

/// @brief 是否为成功响应码（class 2）。
constexpr bool IsCoapSuccessCode(CoapCode code) noexcept {
    return CoapCodeClass(code) == 2;
}

/// @brief 码的可读名称（如 "2.05 Content"；未知码返回 "unknown"）。
const char* CoapCodeName(CoapCode code) noexcept;

/**
 * @brief 一个 CoAP 选项（编号 + 原始值字节）。
 *
 * @details `value` 保持线上字节，不按选项类型提前解释：同一个 CoAP 选项在
 *          不同场景可能是字符串（Uri-Path）、无符号整数（Content-Format）或
 *          不透明字节（ETag）。`AsString()` / `AsUint()` 是调用方按需使用的
 *          解释辅助。
 */
struct CoapOption {
    std::uint16_t number = 0;         ///< 选项编号（0..65535）。
    std::vector<std::uint8_t> value;  ///< 选项值的原始字节。

    /// @brief 构造一个字符串选项（值直接取 `text` 的字节）。
    static CoapOption FromString(std::uint16_t number, std::string_view text);

    /**
     * @brief 构造一个无符号整数选项。
     * @details RFC 7252 规定 uint 选项采用**最短长度**编码：0 编码为零长度，
     *          1..255 为 1 字节，依此类推，且不允许前导零字节。
     */
    static CoapOption FromUint(std::uint16_t number, std::uint32_t value);

    /// @brief 把值按字节拼成字符串。
    std::string AsString() const;

    /**
     * @brief 把值解释为无符号整数。
     * @return 值（最多 4 字节）；长度超过 4 字节时返回 `std::nullopt`。
     */
    std::optional<std::uint32_t> AsUint() const;

    /// @brief 编号与值都相等。
    bool operator==(const CoapOption& other) const;
    /// @brief 编号或值不等。
    bool operator!=(const CoapOption& other) const;
};

/**
 * @brief 一条 CoAP 报文（编解码的输入/输出）。
 */
struct CoapMessage {
    CoapType type = CoapType::kCon;     ///< 报文类型。
    CoapCode code = CoapCode::kEmpty;   ///< 方法/响应码。
    std::uint16_t message_id = 0;       ///< Message ID（16 位）。
    std::vector<std::uint8_t> token;    ///< Token（0..8 字节）。
    std::vector<CoapOption> options;    ///< 选项（编码时自动按编号稳定排序）。
    std::vector<std::uint8_t> payload;  ///< 载荷（空表示没有 Payload Marker）。

    /// @brief 追加一个原始字节选项（不做排序，编码时统一稳定排序）。
    void AddOption(std::uint16_t number, std::vector<std::uint8_t> value);
    /// @brief 追加一个字符串选项。
    void AddStringOption(std::uint16_t number, std::string_view text);
    /// @brief 追加一个最短长度编码的 uint 选项。
    void AddUintOption(std::uint16_t number, std::uint32_t value);

    /**
     * @brief 按 URI 路径追加 Uri-Path 选项。
     * @param path 形如 "/sensors/temp" 或 "sensors/temp"。
     * @details 前导 '/' 被忽略；连续 '/' 之间的空段被跳过（骨架简化，
     *          不区分 "a//b" 与 "a/b"）；根路径 "/" 不产生任何 Uri-Path 选项。
     */
    void AddUriPath(std::string_view path);

    /// @brief 追加 Content-Format 选项（最短长度 uint）。
    void AddContentFormat(std::uint16_t content_format);

    /// @brief 查找第一个指定编号的选项；没有返回 nullptr。
    const CoapOption* FindOption(std::uint16_t number) const noexcept;

    /// @brief 把所有 Uri-Path 选项拼回路径；没有选项时返回 "/"。
    std::string UriPath() const;

    /// @brief 读取第一个 Content-Format 选项；没有或超过 16 位返回 nullopt。
    std::optional<std::uint16_t> ContentFormat() const;
};

/**
 * @brief 编解码结果分类。
 */
enum class CoapStatus : std::uint8_t {
    kOk = 0,                           ///< 成功。
    kTruncated = 1,                    ///< 报文被截断（头/Token/选项前缀/选项值不够）。
    kBadVersion = 2,                   ///< Version 字段不是 1。
    kBadTokenLength = 3,               ///< TKL 为 9..15，或是编码时 token 超过 8 字节。
    kBadOptionDelta = 4,               ///< delta 的 4 bit 字段为 15，或编号累加超过 65535。
    kBadOptionLength = 5,              ///< length 的 4 bit 字段为 15。
    kPayloadMarkerWithoutPayload = 6,  ///< 出现 0xFF 但后面没有载荷字节。
    kMessageTooLarge = 7,              ///< 超过 kCoapMaxMessageBytes。
    kInvalidArgument = 8,              ///< 空指针、类型字段非法、单个选项值超长等。
};

/// @brief 状态的可读名称（日志/测试失败信息用）。
const char* CoapStatusName(CoapStatus status) noexcept;

/**
 * @brief 把报文编码为线上字节。
 *
 * @param message 待编码报文。
 * @param status  可选输出：成功为 `kOk`，失败为具体原因。
 * @return 线上字节；失败时返回空 vector。
 *
 * @details 选项会先按编号**稳定排序**再编码（同编号选项保持调用方顺序，
 *          Uri-Path 的段顺序因此不会被打乱）。编码失败不抛异常。
 */
std::vector<std::uint8_t> EncodeCoapMessage(const CoapMessage& message,
                                            CoapStatus* status = nullptr);

/**
 * @brief 从一段字节解码一条报文。
 *
 * @param data    输入字节。
 * @param size    输入字节数；整段必须恰好是一条报文（CoAP 基于数据报，
 *                一个数据报就是一条消息）。
 * @param message 输出；仅在返回 `kOk` 时被修改，失败时保持原值。
 * @return 解码状态。
 */
CoapStatus DecodeCoapMessage(const std::uint8_t* data, std::size_t size,
                             CoapMessage* message);

/// @brief 上面的 vector 重载。
CoapStatus DecodeCoapMessage(const std::vector<std::uint8_t>& bytes,
                             CoapMessage* message);

/// @brief 单调毫秒时钟（`steady_clock` 起点任意，只用于差值）。
std::uint64_t CoapNowMs() noexcept;

/**
 * @brief 发送一条完整报文的回调。
 * @param wire 完整线上字节（调用方保证生命周期到回调返回）。
 * @return true 表示已交给传输层；false 表示立即失败（如 socket 已关闭）。
 *
 * @note 骨架不关心底层是 UDP socket 还是内存回环；测试用内存回环注入。
 */
using CoapSendFn = std::function<bool(const std::vector<std::uint8_t>& wire)>;

/**
 * @brief 客户端请求参数。
 */
struct CoapRequest {
    CoapCode method = CoapCode::kGet;  ///< 方法（必须是请求码，0.01..0.04）。
    CoapType type = CoapType::kCon;    ///< CON（需确认）或 NON（不等确认）。
    std::string uri_path;              ///< URI 路径，如 "/sensors/temp"。
    std::string uri_host;              ///< 可选 Uri-Host；空表示不携带。
    std::uint16_t uri_port = 0;        ///< 可选 Uri-Port；0 表示不携带。
    std::optional<std::uint16_t> content_format;  ///< 有载荷时通常需要 Content-Format。
    std::vector<std::uint8_t> payload;            ///< 请求载荷。
    std::uint16_t message_id = 0;                 ///< 0 表示自动分配。
    std::vector<std::uint8_t> token;              ///< 空表示自动生成 4 字节 Token。
};

/**
 * @brief 服务端响应内容（不含类型/Message ID/Token，由服务端补齐）。
 */
struct CoapResponse {
    CoapCode code = CoapCode::kContent;           ///< 响应码（必须是响应类别）。
    std::optional<std::uint16_t> content_format;  ///< 可选 Content-Format。
    std::vector<std::uint8_t> payload;            ///< 响应载荷。

    /// @brief 快捷构造 2.05 Content。
    static CoapResponse Content(std::vector<std::uint8_t> payload,
                                std::uint16_t content_format);
    /// @brief 快捷构造 2.04 Changed。
    static CoapResponse Changed();
    /// @brief 快捷构造 2.01 Created。
    static CoapResponse Created();
};

/**
 * @brief 客户端状态（最后一次操作的结果分类）。
 */
enum class CoapClientStatus : std::uint8_t {
    kOk = 0,           ///< 成功。
    kBusy = 1,         ///< 已有待确认请求，`Send()` 被拒绝。
    kEncodeError = 2,  ///< 请求参数非法或编码失败。
    kSendFailed = 3,   ///< `CoapSendFn` 返回 false。
    kDecodeError = 4,  ///< 入站报文解码失败。
    kIgnored = 5,      ///< 入站报文与当前待确认请求无关（迟到/重复/Token 不符）。
    kNoPending = 6,    ///< 没有待确认请求时收到报文。
};

/// @brief 客户端事件类型。
enum class CoapClientEvent : std::uint8_t {
    kResponse = 0,  ///< 收到匹配的响应；事件参数是响应报文。
    kReset = 1,     ///< 收到 RST；事件参数是原始请求报文。
    kTimeout = 2,   ///< 重传耗尽或 NON 请求等待响应超时；事件参数是原始请求报文。
};

/**
 * @brief 客户端配置。
 */
struct CoapClientConfig {
    /// 首次重传前的等待时间；实际间隔从此值起每次翻倍（无抖动）。
    std::uint32_t ack_timeout_ms = 2000;
    /// 最多重传次数（不含首发）；0 表示首发后超时即放弃。
    unsigned max_retransmit = 4;
    /// 随机种子：相同种子在同一请求序列下产生相同的 Token/Message ID，
    /// 便于测试复现；生产环境可用 `CoapNowMs()` 或系统熵初始化。
    std::uint64_t random_seed = 0x636f61705f636c69ull;
    /// 自动生成 Token 的字节数（1..8）。
    std::size_t token_bytes = 4;
};

/**
 * @brief 最小 CoAP 客户端（单请求在途 + 重传状态机）。
 *
 * @details 生命周期：构造（可选 `SetSendFn`）-> `OnEvent` -> `Send()` ->
 *          循环 `Poll()` 与 `HandleDatagram()`。
 *
 *          重传规则：`Send()` 记录发送时刻，`Poll(now)` 在超过 deadline 时
 *          重发**与首发字节完全相同**的报文（RFC 7252 要求重传字节一致），
 *          间隔依次为 `ack_timeout * 2^1`、`* 2^2`……最多 `max_retransmit`
 *          次；再超时触发 `kTimeout` 事件并清空 pending。NON 请求按 RFC 7252
 *          §4.3 不做任何重传：`ack_timeout` 到期即触发 `kTimeout` 并清空 pending。
 *
 *          收到与 Message ID 匹配的空 ACK 时停止重传（等待 separate
 *          response）；收到 piggyback ACK 响应或 Token 匹配的 CON/NON 响应
 *          时完成交换并触发 `kResponse`。
 *
 * @note 非线程安全；`CoapSendFn` 内不要重入同一个客户端对象的 `Send()`。
 */
class CoapClient {
 public:
    explicit CoapClient(CoapSendFn send = {}, CoapClientConfig config = {});

    CoapClient(const CoapClient&) = delete;
    CoapClient& operator=(const CoapClient&) = delete;

    /// @brief 替换发送回调（空回调会让 `Send()` 返回 kSendFailed）。
    void SetSendFn(CoapSendFn send);

    /// @brief 设置事件回调；同一时刻只保留一个回调。
    void OnEvent(std::function<void(CoapClientEvent, const CoapMessage&)> handler);

    /**
     * @brief 发送一个请求。
     * @param request 请求参数。
     * @param now_ms  当前时钟；0 表示用 `CoapNowMs()`（测试可传固定时间线）。
     * @return true 表示已调用发送回调且回调返回 true；false 见 `LastStatus()`。
     */
    bool Send(const CoapRequest& request, std::uint64_t now_ms = 0);

    /**
     * @brief 便捷请求：构造 GET/PUT/POST/DELETE 并发送。
     * @param method         方法（请求码）。
     * @param uri_path       URI 路径。
     * @param payload        载荷。
     * @param content_format 可选的 Content-Format。
     * @param now_ms         时钟注入，0 表示当前时间。
     */
    bool SendRequest(CoapCode method, std::string_view uri_path,
                     const std::vector<std::uint8_t>& payload = {},
                     std::optional<std::uint16_t> content_format = std::nullopt,
                     std::uint64_t now_ms = 0);

    /// @brief 处理一个入站数据报；返回是否消费了它（更新了 pending 或触发事件）。
    bool HandleDatagram(const std::uint8_t* data, std::size_t size);
    /// @brief 上面的 vector 重载。
    bool HandleDatagram(const std::vector<std::uint8_t>& bytes);

    /**
     * @brief 驱动重传/超时。
     * @param now_ms 当前时钟；0 表示用 `CoapNowMs()`。
     *
     * @details 一次调用可连续推进多个已经到期的 deadline（例如测试直接把
     *          时间拨快），但每次最多发送 `max_retransmit` 次。
     */
    void Poll(std::uint64_t now_ms = 0);

    /// @brief 是否有待确认请求。
    bool HasPending() const noexcept;
    /// @brief 最近一次 `Send()` 分配的 Message ID（0 表示还没发过）。
    std::uint16_t LastMessageId() const noexcept;
    /// @brief 当前 pending 请求已重传的次数。
    unsigned PendingRetransmits() const noexcept;
    /// @brief 最后一次操作的状态。
    CoapClientStatus LastStatus() const noexcept;

 private:
    /// 在途请求：请求报文 + 首发字节（重传必须字节一致）+ 计时状态。
    struct PendingRequest {
        CoapMessage request;
        std::vector<std::uint8_t> wire;
        std::uint64_t deadline_ms = 0;
        unsigned retransmits = 0;
        bool acknowledged = false;
    };

    void Deliver(CoapClientEvent event, const CoapMessage& message);
    void SendAck(std::uint16_t message_id);
    std::uint16_t NextMessageId();
    std::vector<std::uint8_t> MakeToken();
    std::uint64_t NextRandom();

    CoapSendFn m_send;
    CoapClientConfig m_config;
    std::function<void(CoapClientEvent, const CoapMessage&)> m_on_event;
    std::optional<PendingRequest> m_pending;
    std::uint64_t m_rng_state;
    CoapClientStatus m_status = CoapClientStatus::kOk;
    std::uint16_t m_last_message_id = 0;
    std::uint16_t m_next_message_id = 0;
};

/**
 * @brief 服务端配置。
 */
struct CoapServerConfig {
    /// 去重缓存条数（0 表示关闭去重）；缓存按插入顺序淘汰最旧条目。
    std::size_t dedup_cache_size = 8;
    /// 随机种子（决定 NON 响应的 Message ID 起点），便于测试复现。
    std::uint64_t random_seed = 0x7365727665725f31ull;
};

/**
 * @brief 最小 CoAP 服务端（请求解码 + 响应组装 + 简单去重）。
 *
 * @details `Handle()` 对每条入站报文：
 *          1. 解码失败、非请求码、ACK/RST 请求类型 → 丢弃并计数；
 *          2. 命中去重缓存（Message ID + Token 相同）→ 重发缓存响应，
 *             **不重复调用 handler**；
 *          3. 调用 handler（未设置则用 `DefaultResponse()`）得到响应；
 *          4. CON 请求回复 piggyback ACK（同 Message ID），NON 请求回复新的
 *             NON（新 Message ID），两者都回显请求 Token。
 *
 * @note 非线程安全；handler 与 `CoapSendFn` 都在 `Handle()` 的调用线程执行。
 *       缓存没有 RFC 7252 的 247s 时间窗，只用于抑制短时间内的重传重复。
 */
class CoapServer {
 public:
    explicit CoapServer(CoapSendFn send = {}, CoapServerConfig config = {});

    CoapServer(const CoapServer&) = delete;
    CoapServer& operator=(const CoapServer&) = delete;

    /// @brief 替换发送回调。
    void SetSendFn(CoapSendFn send);

    /// @brief 设置请求处理函数（返回响应码/载荷）。
    void SetHandler(std::function<CoapResponse(const CoapMessage&)> handler);

    /// @brief 处理一个入站数据报；返回实际组装并尝试发送的响应（无响应为空）。
    std::optional<CoapMessage> Handle(const std::uint8_t* data, std::size_t size);
    /// @brief 上面的 vector 重载。
    std::optional<CoapMessage> Handle(const std::vector<std::uint8_t>& bytes);

    /// @brief handler 被调用的次数（去重命中不计）。
    std::uint64_t HandlerCalls() const noexcept;
    /// @brief 发送响应的次数（含去重重发）。
    std::uint64_t ResponseCount() const noexcept;
    /// @brief 丢弃的报文数（解码失败/非请求/ACK/RST）。
    std::uint64_t DroppedCount() const noexcept;
    /// @brief 最后一次 `Handle()` 的解码状态。
    CoapStatus LastStatus() const noexcept;

    /// @brief 无 handler 时的默认方法映射：GET→2.05、POST→2.01、PUT→2.04、
    ///        DELETE→2.02、其他→4.05。
    static CoapResponse DefaultResponse(CoapCode method);

 private:
    struct CachedExchange {
        std::uint16_t message_id = 0;
        std::vector<std::uint8_t> token;
        CoapMessage response;
    };

    bool SendMessage(const CoapMessage& message);
    std::uint16_t NextMessageId();
    std::uint64_t NextRandom();

    CoapSendFn m_send;
    CoapServerConfig m_config;
    std::function<CoapResponse(const CoapMessage&)> m_handler;
    std::deque<CachedExchange> m_cache;
    std::uint64_t m_rng_state;
    std::uint16_t m_next_message_id = 0;
    std::uint64_t m_handler_calls = 0;
    std::uint64_t m_response_count = 0;
    std::uint64_t m_dropped = 0;
    CoapStatus m_last_status = CoapStatus::kOk;
};

}  // namespace go2cpp::bento::net
