#pragma once

/**
 * @file mqtt.hpp
 * @brief MQTT 3.1.1 最小协议骨架：帧编解码 + QoS 0/1 会话 + 纯内存 broker。
 *
 * @details 本模块只依赖 C++17 标准库，不引入第三方 MQTT 实现，也不持有
 *          socket、线程或定时器。它负责「字节 <-> 报文」与「报文 <-> 会话
 *          状态」两件事，传输层（TCP/TLS/内存管道）由调用方提供：
 *
 *          - **编解码**：CONNECT / CONNACK / PUBLISH / PUBACK / SUBSCRIBE /
 *            SUBACK / PINGREQ / PINGRESP / DISCONNECT 九种控制报文；
 *          - **变长剩余长度（VLQ）**：1~4 字节，上限 268435455；
 *          - **流式解码**：`MqttDecoder` 处理 TCP 粘包与半包，非法帧清空
 *            缓冲并由调用方关闭连接（与 `net::FrameDecoder` 的失败策略一致）；
 *          - **会话状态**：client_id、订阅表、keepalive 计时、遗嘱；
 *          - **内存 broker**：订阅路由（`+` / `#` 通配符）、QoS 降级、
 *            packet id 分配、遗嘱投递。
 *
 * ## 已实现 / 未实现（骨架边界）
 *
 *          已实现：MQTT 3.1.1 九种控制报文、VLQ、严格固定头标志校验、主题名
 *          与过滤器语法校验、订阅路由、QoS 0/1（含 PUBACK）、keepalive
 *          超时判定（1.5 倍）、遗嘱投递、粘包/半包、随机输入 fuzz。
 *
 *          未实现（不做静默降级，调用方会得到明确状态）：
 *          - **QoS 2**：线格式可编解码，但 `MqttSession` 收到 QoS 2 PUBLISH
 *            返回 `kUnsupported`，不会伪造 PUBREC/PUBREL/PUBCOMP；
 *          - **持久会话**：`CONNACK` 的 session present 恒为 false，断开后
 *            订阅表不跨连接保留；clean session 语义只作用于当前连接；
 *          - **retained 消息存储**：`retain` 标志原样转发，broker 不保存；
 *          - **TLS/WebSocket、用户名密码鉴权、ACL、共享订阅、最大报文协商**；
 *          - **完整 UTF-8 校验**：MQTT 字符串只拒绝内嵌 NUL 与长度越界，
 *            不检查代理对/控制字符（文档允许的实现下限）。
 *
 * ## 线程模型
 *
 *          `MqttDecoder`、`MqttSession`、`MqttBroker` 全部非线程安全：一个
 *          连接的状态由同一线程串行驱动。多连接并发时应按连接分片，或由
 *          调用方在上层加锁；本模块不内建锁，避免在协议骨架里隐藏同步开销。
 *
 * ## 典型用法
 *
 * @code
 * using namespace go2cpp::bento::net;
 *
 * MqttSession client{MqttSessionConfig{"sensor-1", 60, true}};
 * std::vector<std::uint8_t> connack;
 * MqttPacket connect;
 * connect.type = MqttPacketType::kConnect;
 * connect.connect.client_id = "sensor-1";
 * connect.connect.keepalive_seconds = 60;
 * client.HandlePacket(connect, &connack);          // 生成 CONNACK 字节
 *
 * MqttBroker broker;
 * broker.OpenSession("sensor-1", nullptr);
 * std::vector<MqttOutbound> outbound;
 * broker.HandleBytes("sensor-1", data, size, &outbound);
 * for (const auto& message : outbound) { SendTo(message.client_id, message.bytes); }
 * @endcode
 *
 * @author go2cpp contributors
 * @date 2026-10-30
 * @copyright MIT License
 */

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace go2cpp::bento::net {

/// MQTT 控制报文类型（MQTT 3.1.1 表 2.1）。0 与 15 保留，解码时拒绝。
enum class MqttPacketType : std::uint8_t {
    kConnect = 1,     ///< 客户端请求建立连接。
    kConnack = 2,     ///< 服务端确认连接。
    kPublish = 3,     ///< 发布消息（两个方向）。
    kPuback = 4,      ///< QoS 1 发布确认。
    kSubscribe = 8,   ///< 客户端订阅主题过滤器。
    kSuback = 9,      ///< 服务端返回订阅结果。
    kPingreq = 12,    ///< 客户端心跳请求。
    kPingresp = 13,   ///< 服务端心跳应答。
    kDisconnect = 14, ///< 客户端正常断开。
};

/// 协议名与级别（MQTT 3.1.1，CONNECT 可变头）。
inline constexpr char kMqttProtocolName[] = "MQTT";
inline constexpr std::uint8_t kMqttProtocolLevel = 4;

/// 剩余长度理论上限：4 字节 VLQ 全 7 位有效，即 2^28 - 1。
inline constexpr std::uint32_t kMqttMaxRemainingLength = 268435455u;

/// VLQ 最大编码字节数。
inline constexpr std::size_t kMqttMaxRemainingLengthBytes = 4;

/// 单报文默认上限 1 MiB：与 `net::kFrameMaxPayload` 对齐，避免声明超大长度的
/// 固定头诱导按 268 MiB 预分配。需要更大报文时显式构造 `MqttDecoder`。
inline constexpr std::uint32_t kMqttDefaultMaxPacketBytes = 1024u * 1024u;

/// 骨架支持的最高 QoS。QoS 2 的线格式可解析，但会话层返回 kUnsupported。
inline constexpr std::uint8_t kMqttHighestSupportedQos = 1;

/// SUBACK 失败返回码。
inline constexpr std::uint8_t kMqttSubackFailure = 0x80;

/// 「未订阅 / 无此过滤器的授权 QoS」哨兵值（合法 QoS 只有 0/1/2）。
inline constexpr std::uint8_t kMqttNoSubscription = 0xFF;

/// `MqttSession::HandlePacket` 与解码器统一的结果码。
enum class MqttStatus : std::uint8_t {
    kOk = 0,              ///< 成功；协议层应生成的回应已写入输出参数。
    kNeedMoreData = 1,    ///< 半包：字节还不足一个完整报文，缓冲已保留。
    kMalformed = 2,       ///< 线格式非法（类型/标志/长度/UTF-8 字符串/主题）。
    kTooLarge = 3,        ///< 声明长度超过解码器配置上限。
    kUnsupported = 4,     ///< 语法合法但骨架不支持（QoS 2、协议版本 5 等）。
    kProtocolViolation = 5, ///< 状态机不允许（未 CONNECT 就 SUBSCRIBE 等）。
    kInvalidArgument = 6, ///< 调用方参数为空/越界；报文本身可能合法。
};

/// @brief 状态名（日志与测试失败信息用）。
const char* MqttStatusName(MqttStatus status) noexcept;

/// @brief 报文类型名；保留类型返回 "Reserved"。
const char* MqttPacketTypeName(MqttPacketType type) noexcept;

/**
 * @brief 编码剩余长度（VLQ，7 位一组，低位在前）。
 * @param value 剩余长度，必须 <= kMqttMaxRemainingLength。
 * @param out 至少 4 字节的输出缓冲。
 * @return 写入字节数（1~4）；`out == nullptr` 或 `value` 超限返回 0。
 */
std::size_t EncodeRemainingLength(std::uint32_t value, std::uint8_t out[4]) noexcept;

/**
 * @brief 解码剩余长度。
 * @param data 输入字节。
 * @param size 可用字节数。
 * @param value 成功时写入解码值；可为 nullptr。
 * @param consumed 成功时写入消耗字节数；可为 nullptr。
 * @return `kOk` / `kNeedMoreData`（不足 4 字节且尚未终止）/
 *         `kMalformed`（第 4 字节仍带继续位，即超过 4 字节）。
 *
 * @note 该函数不会分配内存，可在任意线程调用。
 */
MqttStatus DecodeRemainingLength(const std::uint8_t* data, std::size_t size,
                                 std::uint32_t* value = nullptr,
                                 std::size_t* consumed = nullptr) noexcept;

/**
 * @brief 校验主题名（PUBLISH 的 topic）：非空、不含 NUL、不含 `+`/`#`。
 * @note 按规范允许 `/` 出现在任意位置（含首尾）。
 */
bool IsValidTopicName(const std::string& topic) noexcept;

/**
 * @brief 校验主题过滤器（SUBSCRIBE）：非空、不含 NUL，`+` 独占一层，
 *        `#` 只能是最后一层且独占一层。
 */
bool IsValidTopicFilter(const std::string& filter) noexcept;

/**
 * @brief 判断主题是否命中过滤器（MQTT 3.1.1 §4.7）。
 *
 * @details 支持 `+`（单层）与 `#`（多层，含父层：`sport/#` 匹配 `sport`）。
 *          以 `$` 开头的主题不会被以 `#` 或 `+` 开头的过滤器匹配（§4.7.2），
 *          避免 `#` 意外吞掉 `$SYS/` 系统主题。
 */
bool TopicMatchesFilter(const std::string& filter, const std::string& topic) noexcept;

/// CONNECT 报文的连接选项（可变头 + 载荷，不含固定头）。
struct MqttConnectOptions {
    std::string protocol_name = kMqttProtocolName;    ///< 协议名，必须为 "MQTT"。
    std::uint8_t protocol_level = kMqttProtocolLevel; ///< 协议级别，3.1.1 为 4。
    bool clean_session = true;        ///< 是否要求丢弃旧会话。
    std::uint16_t keepalive_seconds = 0; ///< 心跳周期（秒），0 表示不启用。
    std::string client_id;            ///< 客户端标识（UTF-8）。
    bool has_will = false;            ///< 是否携带遗嘱。
    std::string will_topic;           ///< 遗嘱主题（has_will 时有效）。
    std::vector<std::uint8_t> will_message; ///< 遗嘱载荷（二进制）。
    std::uint8_t will_qos = 0;        ///< 遗嘱 QoS，0~2。
    bool will_retain = false;         ///< 遗嘱 retain 标志。
    bool has_username = false;        ///< 是否携带用户名。
    std::string username;             ///< 用户名（UTF-8）。
    bool has_password = false;        ///< 是否携带密码。
    std::vector<std::uint8_t> password; ///< 密码（二进制，允许内嵌 NUL）。
};

/// CONNACK 报文体。
struct MqttConnack {
    bool session_present = false; ///< 是否复用已有会话（骨架恒 false）。
    std::uint8_t return_code = 0; ///< 0=接受；其余为拒绝码（见 3.2.2.3）。
};

/// 订阅项：主题过滤器 + 请求/授予的 QoS。
struct MqttSubscription {
    std::string filter;   ///< 主题过滤器。
    std::uint8_t qos = 0; ///< QoS（0/1/2 为合法请求值）。
};

/// PUBLISH 报文体（不含固定头）。
struct MqttPublish {
    std::string topic;                 ///< 主题名。
    std::vector<std::uint8_t> payload; ///< 载荷（二进制）。
    std::uint8_t qos = 0;              ///< 0/1/2；骨架会话只支持 0/1。
    bool dup = false;                  ///< 重传标志（QoS 0 时必须为 false）。
    bool retain = false;               ///< 保留标志。
    std::uint16_t packet_id = 0;       ///< QoS>0 时的报文标识符（非 0）。
};

/**
 * @brief 解码后的控制报文。不同 `type` 只读取对应字段，其余字段保持默认值。
 *
 * @details 用「一个聚合 + type 判别」而不是 `std::variant`，是为了让
 *          `MqttDecoder` 在循环里复用同一个 `MqttPacket` 时无需访问者样板；
 *          字段默认值让「未使用字段」可被测试当哨兵检查。
 */
struct MqttPacket {
    MqttPacketType type = MqttPacketType::kConnect; ///< 报文类型。
    std::uint8_t flags = 0;        ///< 固定头低 4 位（原样保留，便于诊断）。
    MqttConnectOptions connect;    ///< type == kConnect 时有效。
    MqttConnack connack;           ///< type == kConnack 时有效。
    MqttPublish publish;           ///< type == kPublish 时有效。
    std::uint16_t packet_id = 0;   ///< PUBACK/SUBSCRIBE/SUBACK 的报文标识符。
    std::vector<MqttSubscription> subscriptions; ///< type == kSubscribe 时有效。
    std::vector<std::uint8_t> suback_codes; ///< type == kSuback 时有效。
};

/**
 * @brief 编码 CONNECT。
 * @return 完整报文字节；参数非法（协议名非 "MQTT"、will QoS > 2、
 *         有密码却无用户名、will 标志与 QoS/retain 矛盾、client_id 含 NUL 等）
 *         返回空 vector。协议级别按原值编码（3.1.1 为 4）：允许编码其他级别
 *         是为了让「服务端拒绝不支持的版本」这条路径可测试。合法报文最短
 *         14 字节（固定头 2 + body 12），与「空」不会混淆。
 */
std::vector<std::uint8_t> EncodeConnect(const MqttConnectOptions& options);

/**
 * @brief 编码 CONNACK。
 * @return 4 字节报文；`return_code > 5` 返回空 vector。
 */
std::vector<std::uint8_t> EncodeConnack(const MqttConnack& connack);

/**
 * @brief 编码 PUBLISH。
 * @return 完整报文；topic 非法、qos > 2、qos == 0 但 packet_id != 0、
 *         qos > 0 但 packet_id == 0、QoS 0 却置 dup 均返回空 vector。
 */
std::vector<std::uint8_t> EncodePublish(const MqttPublish& publish);

/**
 * @brief 编码 PUBACK。
 * @return 4 字节报文；packet_id 为 0 返回空 vector。
 */
std::vector<std::uint8_t> EncodePuback(std::uint16_t packet_id);

/**
 * @brief 编码 SUBSCRIBE（固定头标志必须为 0x2）。
 * @return 完整报文；packet_id 为 0、订阅列表为空、过滤器非法、qos > 2
 *         返回空 vector。
 */
std::vector<std::uint8_t> EncodeSubscribe(std::uint16_t packet_id,
                                          const std::vector<MqttSubscription>& subscriptions);

/**
 * @brief 编码 SUBACK。
 * @return 完整报文；packet_id 为 0、返回码列表为空、返回码不是 0/1/2/0x80
 *         返回空 vector。
 */
std::vector<std::uint8_t> EncodeSuback(std::uint16_t packet_id,
                                       const std::vector<std::uint8_t>& codes);

/// @brief 编码 PINGREQ（2 字节：0xC0 0x00）。
std::vector<std::uint8_t> EncodePingreq();

/// @brief 编码 PINGRESP（2 字节：0xD0 0x00）。
std::vector<std::uint8_t> EncodePingresp();

/// @brief 编码 DISCONNECT（2 字节：0xE0 0x00）。
std::vector<std::uint8_t> EncodeDisconnect();

/**
 * @brief 一次性解码一个完整报文。
 * @param data 输入字节；`size == 0` 时可为 nullptr。
 * @param size 可用字节数。
 * @param out 输出报文；不可为 nullptr。
 * @param consumed 非空时写入本报文消耗的字节数（仅 kOk 时写）。
 * @return `kOk` / `kNeedMoreData`（字节不够，不消费）/ `kMalformed` /
 *         `kInvalidArgument`（out 为空）。
 *
 * @note 协议级别不在解码阶段拒绝：MQTT 3.1(3) / 3.1.1(4) / 5.0(5) 都能解出
 *       CONNECT，由 `MqttSession` 决定回应 CONNACK 0x01。协议名不是 "MQTT"
 *       则按 `kMalformed` 拒绝（无法判断对端在说什么协议）。
 * @note 本函数无状态；`kMalformed` 时不会修改除 `out` 以外的外部状态。
 *       校验是严格的：固定头保留位非 0、SUBSCRIBE 标志不是 0x2、PUBLISH
 *       QoS 为 3、QoS 0 带 DUP、报文尾部有多余字节，全部按 `kMalformed` 拒绝。
 */
MqttStatus DecodePacket(const std::uint8_t* data, std::size_t size, MqttPacket* out,
                        std::size_t* consumed = nullptr);

/// @brief `DecodePacket` 的 vector 重载，方便测试与上层缓存。
MqttStatus DecodePacket(const std::vector<std::uint8_t>& data, MqttPacket* out,
                        std::size_t* consumed = nullptr);

/**
 * @brief 流式报文解码器：处理 TCP 粘包与半包。
 *
 * @details 用法与 `net::FrameDecoder` 一致：
 * @code
 * decoder.Append(bytes, n);
 * MqttPacket packet;
 * while (decoder.Next(&packet) == MqttStatus::kOk) { Handle(packet); }
 * @endcode
 *          - 粘包：一次 Append 多包，Next 依次返回直到 kNeedMoreData；
 *          - 半包：Next 返回 kNeedMoreData，缓冲保留等待后续字节；
 *          - 非法/超限：返回对应错误并**清空缓冲**。此时报文边界已不可信，
 *            调用方应关闭连接，而不是继续在同一字节流上猜测。
 *
 * @note 非线程安全；每条连接一个实例。
 */
class MqttDecoder {
public:
    /// @param max_packet_bytes 单报文剩余长度上限；传 0 表示只接受空载荷报文。
    explicit MqttDecoder(std::uint32_t max_packet_bytes = kMqttDefaultMaxPacketBytes) noexcept;

    /// 追加收到的字节；空指针或 0 字节是无操作。
    void Append(const std::uint8_t* data, std::size_t size);

    /// 尝试取出下一包：kOk 已填充；kNeedMoreData 继续等；其余为流错误（缓冲已清空）。
    MqttStatus Next(MqttPacket* out);

    /// 丢弃未消费字节并复位（错误恢复或连接重建时用）。
    void Clear() noexcept;

    /// 尚未消费的字节数（含半包）。
    std::size_t PendingBytes() const noexcept { return m_buffer.size() - m_offset; }

    /// 配置的单报文上限。
    std::uint32_t max_packet_bytes() const noexcept { return m_max_packet_bytes; }

private:
    std::vector<std::uint8_t> m_buffer; ///< 待解析字节；m_offset 之前是已消费前缀。
    std::size_t m_offset = 0;           ///< 已消费前缀长度；Append 时整体压缩。
    std::uint32_t m_max_packet_bytes;   ///< 单报文上限。
};

/// 会话配置（服务端由 broker 填入 client_id；客户端由调用方填入）。
struct MqttSessionConfig {
    std::string client_id;               ///< 客户端标识。
    std::uint16_t keepalive_seconds = 0; ///< 心跳周期（秒），0 表示不启用。
    bool clean_session = true;           ///< 连接时是否清空旧订阅。
};

/**
 * @brief 单个 MQTT 连接的协议状态机。
 *
 * @details 处理入站报文并生成协议应答，维护 client_id、订阅表、keepalive
 *          计时与遗嘱。它**不做路由**：PUBLISH 的转发由 `MqttBroker` 负责。
 *          本类是**服务端会话状态机**：只接受 CONNECT/SUBSCRIBE/PUBLISH/
 *          PUBACK/PINGREQ/PINGRESP/DISCONNECT，收到 CONNACK/SUBACK 视为
 *          方向错误返回 `kProtocolViolation`。客户端侧心跳/定时由调用方用
 *          `Touch` + `KeepaliveExpired` 驱动。
 *
 *          状态迁移：
 *          - 初始 `disconnected`；收到 CONNECT 且校验通过 → `connected`，
 *            回应 CONNACK；协议版本不符回应 CONNACK 0x01，client_id 为空且
 *            clean_session=false 回应 CONNACK 0x02（均保持未连接）；
 *          - 已连接时重复 CONNECT → `kProtocolViolation`；
 *          - 未连接时收到 CONNECT 之外的报文 → `kProtocolViolation`；
 *          - DISCONNECT → 回到未连接，且清除待投递遗嘱（正常断开不投递）。
 *
 * @note 非线程安全。
 */
class MqttSession {
public:
    explicit MqttSession(MqttSessionConfig config = {});

    /**
     * @brief 处理一个入站报文。
     * @param packet 已解码报文。
     * @param response 非空时写入要回给对端的字节（0 或 1 个报文）；可为 nullptr。
     * @return `kOk`（含「已生成拒绝 CONNACK」）/ `kProtocolViolation`
     *         （状态机不允许，或收到 CONNACK/SUBACK 这类反向报文）/
     *         `kUnsupported`（QoS 2）/ `kMalformed`（报文内容非法）/
     *         `kInvalidArgument`。
     */
    MqttStatus HandlePacket(const MqttPacket& packet, std::vector<std::uint8_t>* response);

    /**
     * @brief 处理一段字节流（可含多个报文或半包），全部应答追加到 `responses`。
     * @return `kOk`（含半包等待）/ 首个错误的 `MqttStatus`。
     */
    MqttStatus HandleBytes(const std::uint8_t* data, std::size_t size,
                           std::vector<std::vector<std::uint8_t>>* responses);

    /// 是否已完成 CONNECT。
    bool connected() const noexcept { return m_connected; }

    /// 是否收到过 DISCONNECT（区分「正常关闭」与「异常断开」）。
    bool disconnected() const noexcept { return m_disconnected; }

    /// 当前配置（CONNECT 成功后 client_id/keepalive 已更新）。
    const MqttSessionConfig& config() const noexcept { return m_config; }

    /// 当前 client_id。
    const std::string& client_id() const noexcept { return m_config.client_id; }

    /// 当前 keepalive（秒）。
    std::uint16_t keepalive_seconds() const noexcept { return m_config.keepalive_seconds; }

    /// 最近一次 CONNECT 的完整选项（遗嘱、用户名等）。
    const MqttConnectOptions& connect_options() const noexcept { return m_connect_options; }

    /// 订阅表：过滤器 -> 授予 QoS。
    const std::map<std::string, std::uint8_t>& subscriptions() const noexcept {
        return m_subscriptions;
    }

    /// 是否订阅了该过滤器（精确匹配，不做通配符展开）。
    bool HasSubscription(const std::string& filter) const noexcept;

    /// 该过滤器的授予 QoS；未订阅返回 `kMqttNoSubscription`。
    std::uint8_t SubscriptionQos(const std::string& filter) const noexcept;

    /// 是否存在待投递遗嘱（CONNECT 带了 will，且尚未正常断开）。
    bool HasPendingWill() const noexcept { return m_has_pending_will; }

    /// 遗嘱对应的 PUBLISH 模板；`HasPendingWill()` 为 false 时内容无意义。
    const MqttPublish& will() const noexcept { return m_will; }

    /// 标记遗嘱已投递（broker 在路由成功后调用，避免重复投递）。
    void ConsumeWill() noexcept { m_has_pending_will = false; }

    /// 记录一次活动（收到任意合法报文或发送心跳时调用）。
    void Touch(std::uint64_t now_ms) noexcept { m_last_activity_ms = now_ms; }

    /// 最近一次活动时间（毫秒，调用方时钟）。
    std::uint64_t last_activity_ms() const noexcept { return m_last_activity_ms; }

    /// keepalive 超时阈值（毫秒）= 1.5 × keepalive；keepalive 为 0 时返回 0。
    std::uint64_t KeepaliveTimeoutMs() const noexcept;

    /**
     * @brief 是否已超过 keepalive 窗口。
     * @param now_ms 毫秒时间戳，与 `Touch` 同一时钟。
     * @details keepalive 为 0 或尚未 CONNECT 时恒为 false；边界为严格大于
     *          （恰好等于 1.5 倍不算超时）。
     */
    bool KeepaliveExpired(std::uint64_t now_ms) const noexcept;

    /// 重连/复位：清空连接状态与订阅，保留配置中的 client_id。
    void Reset() noexcept;

private:
    MqttStatus HandleConnect(const MqttPacket& packet, std::vector<std::uint8_t>* response);
    MqttStatus HandleSubscribe(const MqttPacket& packet, std::vector<std::uint8_t>* response);

    MqttSessionConfig m_config;           ///< 会话配置。
    MqttConnectOptions m_connect_options; ///< 最近一次 CONNECT 选项。
    std::map<std::string, std::uint8_t> m_subscriptions; ///< 订阅表。
    MqttPublish m_will;                   ///< 遗嘱模板。
    MqttDecoder m_decoder;                ///< HandleBytes 使用的流式解码器。
    bool m_connected = false;             ///< 是否已完成 CONNECT。
    bool m_disconnected = false;          ///< 是否收到 DISCONNECT。
    bool m_has_pending_will = false;      ///< 遗嘱是否待投递。
    std::uint64_t m_last_activity_ms = 0; ///< 最近活动时间。
};

/// broker 生成的一条待发送消息：目标客户端 + 完整报文字节。
struct MqttOutbound {
    std::string client_id;           ///< 目标客户端。
    std::vector<std::uint8_t> bytes; ///< 完整 MQTT 报文。
};

/**
 * @brief 纯内存 MQTT broker：会话表 + 订阅路由 + QoS 降级 + 遗嘱投递。
 *
 * @details 不阻塞、不启动线程、不依赖时钟（keepalive 由调用方传入毫秒时间戳）。
 *          典型驱动方式：
 * @code
 * broker.OpenSession("phone", nullptr);
 * broker.HandleBytes("phone", bytes, size, &outbound);  // 内部完成 CONNECT
 * // outbound 里是发回 phone 的 CONNACK/SUBACK/PUBACK，以及路由给订阅者的
 * // PUBLISH；由调用方按 client_id 写到各自的连接上。
 * @endcode
 *
 *          **client_id 约定**：`OpenSession` 先登记连接标识，随后 CONNECT
 *          报文里的 client_id 必须与之一致，否则返回 `kProtocolViolation`。
 *          这是骨架为「无传输层的纯内存回环」做的简化：真实 broker 会从
 *          CONNECT 报文里提取 client_id 再建会话，但那需要把连接句柄与
 *          客户端标识分开，超出本模块范围。
 *
 *          同一 client_id 重复 `OpenSession` 会顶替旧连接（旧连接上的
 *          半包缓冲与订阅表一并丢弃），不产生 session present。
 *
 * @note 非线程安全；所有方法由同一线程串行调用。
 */
class MqttBroker {
public:
    explicit MqttBroker(std::uint32_t max_packet_bytes = kMqttDefaultMaxPacketBytes);

    /**
     * @brief 登记一个新连接。
     * @param requested_client_id 期望的客户端标识；空表示由 broker 分配
     *        `auto-<n>`。
     * @param assigned_id 非空时写入实际使用的 client_id。
     * @return `kOk`；`requested_client_id` 含 NUL 返回 `kInvalidArgument`。
     */
    MqttStatus OpenSession(const std::string& requested_client_id, std::string* assigned_id);

    /**
     * @brief 处理某客户端连接上收到的字节流（可含多包/半包）。
     * @param client_id 已由 `OpenSession` 登记。
     * @param data 输入字节。
     * @param size 可用字节数。
     * @param outbound 非空时追加所有待发送消息（按产生顺序）。
     * @return `kOk`（含半包等待）/ 首个错误。
     */
    MqttStatus HandleBytes(const std::string& client_id, const std::uint8_t* data,
                           std::size_t size, std::vector<MqttOutbound>* outbound);

    /**
     * @brief 处理一个已解码报文（`HandleBytes` 的内核，测试可直接调用）。
     * @return 同 `HandleBytes`；PUBLISH 的路由结果追加到 `outbound`。
     */
    MqttStatus HandlePacket(const std::string& client_id, const MqttPacket& packet,
                            std::vector<MqttOutbound>* outbound);

    /**
     * @brief 关闭连接。
     * @param client_id 客户端标识。
     * @param graceful true 表示正常断开（不投递遗嘱）；false 表示异常断开
     *        （若存在待投递遗嘱，则路由给匹配的订阅者）。
     * @param outbound 非空时追加遗嘱投递消息。
     * @return `kOk`；客户端不存在返回 `kInvalidArgument`。
     */
    MqttStatus CloseSession(const std::string& client_id, bool graceful,
                            std::vector<MqttOutbound>* outbound);

    /**
     * @brief 关闭所有 keepalive 超时的连接（按异常断开处理，投递遗嘱）。
     * @param now_ms 当前毫秒时间戳，与内部 `NowMs()` 同一时钟。
     * @param outbound 非空时追加遗嘱投递。
     * @return 被关闭的连接数。
     */
    std::size_t CloseExpiredSessions(std::uint64_t now_ms,
                                     std::vector<MqttOutbound>* outbound);

    /// 是否存在该客户端。
    bool HasSession(const std::string& client_id) const noexcept;

    /// 当前连接数。
    std::size_t session_count() const noexcept { return m_sessions.size(); }

    /// 查询会话（只读）；不存在返回 nullptr。指针在下次 Open/Close 之前有效。
    const MqttSession* FindSession(const std::string& client_id) const noexcept;

private:
    struct ClientEntry {
        MqttSession session; ///< 协议状态机。
        MqttDecoder decoder; ///< 该连接的流式解码器。
    };

    MqttSession* FindMutable(const std::string& client_id) noexcept;
    std::uint16_t AllocatePacketId() noexcept;
    MqttStatus HandleConnect(const std::string& client_id, const MqttPacket& packet,
                             std::vector<MqttOutbound>* outbound);
    MqttStatus HandleSubscribe(const std::string& client_id, const MqttPacket& packet,
                               std::vector<MqttOutbound>* outbound);
    MqttStatus HandlePublish(const std::string& client_id, const MqttPacket& packet,
                             std::vector<MqttOutbound>* outbound);
    void RoutePublish(const MqttPublish& publish, std::vector<MqttOutbound>* outbound);

    std::map<std::string, ClientEntry> m_sessions; ///< client_id -> 连接。
    std::uint32_t m_max_packet_bytes;              ///< 单报文上限。
    std::uint16_t m_next_packet_id = 1;            ///< 服务端分配的下一个 packet id。
    std::uint32_t m_next_auto_id = 1;              ///< 自动 client_id 序号。
};

}  // namespace go2cpp::bento::net
