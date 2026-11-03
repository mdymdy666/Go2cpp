/**
 * @file stun_client.cpp
 * @brief STUN 客户端与 RFC 3489 NAT 类型检测实现。
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include "go2cpp/p2p/stun_client.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <vector>

#include "go2cpp/p2p/transport.hpp"

namespace go2cpp::p2p {
namespace {

using Clock = std::chrono::steady_clock;

std::chrono::milliseconds Remaining(Clock::time_point deadline) {
    const auto now = Clock::now();
    if (now >= deadline) {
        return std::chrono::milliseconds(0);
    }
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    return std::max(left, std::chrono::milliseconds(1));
}

/// 判断映射地址是否等于「本机地址」：用于无 NAT 判定。
bool IsLocalMappedEndpoint(DatagramTransport* transport, const Endpoint& mapped) {
    const Endpoint local = transport->LocalEndpoint();
    if (!local.valid()) {
        return false;
    }
    if (!local.IsUnspecified()) {
        return local == mapped;
    }
    // 绑定在 0.0.0.0/:: 时 getsockname 拿不到具体地址，枚举网卡比对。
    if (mapped.IsUnspecified()) {
        return false;
    }
    const std::vector<Endpoint> addresses =
        EnumerateLocalAddresses(mapped.port(), true);
    return std::find(addresses.begin(), addresses.end(), mapped) != addresses.end();
}

}  // namespace

StunClient::StunClient(DatagramTransport* transport, Endpoint server)
    : m_transport(transport), m_server(std::move(server)), m_buffer(2048) {}

/**
 * @brief 发送一条 STUN 请求并等待事务 ID 匹配的成功/错误响应（通用路径）。
 *
 * @param request [in] 已构造的 STUN 请求，事务 ID 由此决定匹配条件——引用在本调用期间有效。
 * @param timeout [in] 接收上限毫秒；<=0 时首轮 poll 立即超时，通常应传正数。
 * @param error [out] 失败原因，可为 nullptr；成功时不修改。
 *
 * @return
 * - 有值 `StunExchangeResult`：收到匹配响应，含消息、来源地址与 RTT——调用方**必须**检查 klass 再取属性；
 * - `std::nullopt`：超时/发送失败/未配置——调用方**必须**读 error，不要假定网络不可用。
 *
 * ## 定位
 * STUN 事务的唯一通用收发点。漏掉事务 ID 匹配会把过期响应当成本次结果；
 * 不丢弃不相关数据报会让 NAT 探测在多客户端场景下串包。
 *
 * ## 步骤
 * step1. 校验 transport 与服务器地址；
 * step2. 序列化请求，失败即返回；
 * step3. 记录起始时间并发送；
 * step4. 循环接收，直到超过 deadline；
 * step5. 解析失败或事务 ID/方法/类型不匹配的数据报直接丢弃并继续；
 * step6. 命中则移动消息、记录来源与 RTT 返回。
 *
 * ## 并发/失败语义
 * - 调用约束：同一 StunClient 的 m_buffer 被复用，不可并发调用 Exchange/Binding。
 * - 原子性：一次事务的发送与接收之间不持锁，其他线程的收发可能插入。
 * - 失败语义：超时返回 nullopt 且不修改 request；error 只在失败分支写入。
 * - 不变量：成功返回的 message.transaction_id 一定等于 request.transaction_id。
 */
std::optional<StunExchangeResult> StunClient::Exchange(const StunMessage& request,
                                                       std::chrono::milliseconds timeout,
                                                       std::string* error) {
    if (m_transport == nullptr || !m_server.valid()) {
        if (error != nullptr) {
            *error = "STUN 客户端未配置传输或服务器地址";
        }
        return std::nullopt;
    }
    std::string serialize_error;
    const std::vector<std::uint8_t> wire = SerializeStunMessage(request, &serialize_error);
    if (wire.empty()) {
        if (error != nullptr) {
            *error = serialize_error.empty() ? "STUN 序列化失败" : serialize_error;
        }
        return std::nullopt;
    }
    const Clock::time_point started = Clock::now();
    if (!m_transport->SendTo(m_server, wire.data(), wire.size())) {
        if (error != nullptr) {
            *error = "STUN 请求发送失败（" + m_server.ToString() + "）";
        }
        return std::nullopt;
    }
    const Clock::time_point deadline = started + timeout;
    std::vector<std::uint8_t>& buffer = m_buffer;
    for (;;) {
        const std::chrono::milliseconds left = Remaining(deadline);
        if (left.count() <= 0) {
            break;
        }
        Endpoint source;
        const int received =
            m_transport->RecvFrom(&source, buffer.data(), buffer.size(), left);
        if (received == 0) {
            continue;
        }
        if (received < 0) {
            if (error != nullptr) {
                *error = "STUN 响应接收失败";
            }
            return std::nullopt;
        }
        std::string parse_error;
        auto message = ParseStunMessage(buffer.data(), static_cast<std::size_t>(received),
                                        &parse_error);
        if (!message.has_value()) {
            continue;
        }
        if (message->method != request.method ||
            message->transaction_id != request.transaction_id) {
            continue;
        }
        if (message->klass != StunClass::kSuccessResponse &&
            message->klass != StunClass::kErrorResponse) {
            continue;
        }
        StunExchangeResult result;
        // 直接移动解析结果：省掉 attributes vector 与每个属性 value 的深拷贝。
        result.message = std::move(*message);
        result.source = source;
        result.rtt = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started);
        return result;
    }
    if (error != nullptr) {
        *error = "STUN 请求超时（" + m_server.ToString() + "）";
    }
    return std::nullopt;
}

/**
 * @brief 快速 Binding 探测：栈上写线格式请求，零拷贝视图解析，返回映射地址。
 *
 * @param timeout [in] 接收上限毫秒；<=0 立即超时。
 * @param error [out] 失败原因，可为 nullptr；成功时不修改。
 *
 * @return
 * - 有值 `Endpoint`：服务器观察到的本端公网地址——调用方**必须**把它作为 srflx 候选，不要当本地地址；
 * - `std::nullopt`：超时/无 XOR-MAPPED-ADDRESS/未配置——调用方**必须**降级处理。
 *
 * ## 定位
 * NAT 类型探测与 srflx 候选采集的热路径。它绕过通用序列化，因此字段布局必须与协议严格一致；
 * 一旦解析出的地址错位，ICE 会拿错误候选做连通性检查，表现为「永远连不上」。
 *
 * ## 步骤
 * step1. 校验 transport 与服务器地址；
 * step2. 用 64B 栈缓冲直接写 Binding 请求并生成事务 ID；
 * step3. 发送并记录 deadline；
 * step4. 循环接收，解析为只读视图；
 * step5. 非 Binding 响应或事务 ID 不匹配则丢弃；
 * step6. 提取 XOR-MAPPED-ADDRESS 返回。
 *
 * ## 并发/失败语义
 * - 调用约束：与 Exchange 共用 m_buffer，不可并发。
 * - 原子性：单次发送/接收；不持锁。
 * - 失败语义：不抛异常；超时返回 nullopt。
 * - 不变量：成功返回的地址 family 与 socket 家族一致。
 */
std::optional<Endpoint> StunClient::Binding(std::chrono::milliseconds timeout,
                                            std::string* error) {
    // 快路径：请求用栈缓冲直接写线格式（SOFTWARE 属性 + 事务 ID），响应用
    // 零拷贝视图读取。整条路径不分配堆内存（错误路径除外），比走
    // MakeBindingRequest + Serialize + Parse 的通用路径少 6~8 次分配/拷贝。
    if (m_transport == nullptr || !m_server.valid()) {
        if (error != nullptr) {
            *error = "STUN 客户端未配置传输或服务器地址";
        }
        return std::nullopt;
    }
    std::array<std::uint8_t, 64> wire{};
    std::array<std::uint8_t, kStunTransactionIdSize> transaction_id{};
    const std::size_t wire_size =
        BuildBindingRequestWire(wire.data(), wire.size(), &transaction_id);
    if (wire_size == 0) {
        if (error != nullptr) {
            *error = "STUN 序列化失败";
        }
        return std::nullopt;
    }
    const Clock::time_point started = Clock::now();
    if (!m_transport->SendTo(m_server, wire.data(), wire_size)) {
        if (error != nullptr) {
            *error = "STUN 请求发送失败（" + m_server.ToString() + "）";
        }
        return std::nullopt;
    }
    const Clock::time_point deadline = started + timeout;
    std::vector<std::uint8_t>& buffer = m_buffer;
    // 与 relay 接收路径相同：拿到响应就返回，不为每个数据报读一次时钟。
    std::chrono::milliseconds left = timeout;
    for (;;) {
        if (left.count() <= 0) {
            break;
        }
        Endpoint source;
        const int received = m_transport->RecvFrom(&source, buffer.data(), buffer.size(), left);
        if (received == 0) {
            left = Remaining(deadline);
            continue;
        }
        if (received < 0) {
            if (error != nullptr) {
                *error = "STUN 响应接收失败";
            }
            return std::nullopt;
        }
        const auto view =
            ParseStunMessageView(buffer.data(), static_cast<std::size_t>(received));
        if (!view.has_value() || view->method() != kStunMethodBinding) {
            left = Remaining(deadline);
            continue;
        }
        if (std::memcmp(view->transaction_id(), transaction_id.data(), transaction_id.size()) !=
            0) {
            left = Remaining(deadline);
            continue;
        }
        if (view->klass() == StunClass::kErrorResponse) {
            std::uint16_t code = 0;
            std::string reason;
            view->GetErrorCode(&code, &reason);
            if (error != nullptr) {
                *error = "STUN 服务器返回错误 " + std::to_string(code) + ": " + reason;
            }
            return std::nullopt;
        }
        if (view->klass() != StunClass::kSuccessResponse) {
            left = Remaining(deadline);
            continue;
        }
        const auto mapped = view->GetMappedAddress();
        if (!mapped.has_value() && error != nullptr) {
            *error = "STUN 响应缺少 XOR-MAPPED-ADDRESS/MAPPED-ADDRESS";
        }
        return mapped;
    }
    if (error != nullptr) {
        *error = "STUN 请求超时（" + m_server.ToString() + "）";
    }
    return std::nullopt;
}

std::optional<StunExchangeResult> StunClient::BindingWithChange(
    std::uint32_t change_flags, std::chrono::milliseconds timeout, std::string* error) {
    StunMessage request = StunMessage::MakeBindingRequest();
    request.AddU32(kStunAttributeChangeRequest, change_flags);
    return Exchange(request, timeout, error);
}

std::string NatTypeName(NatType type) {
    switch (type) {
        case NatType::kUnknown:
            return "Unknown";
        case NatType::kOpenInternet:
            return "OpenInternet";
        case NatType::kFullCone:
            return "FullCone";
        case NatType::kRestrictedCone:
            return "RestrictedCone";
        case NatType::kPortRestrictedCone:
            return "PortRestrictedCone";
        case NatType::kSymmetric:
            return "Symmetric";
    }
    return "Unknown";
}

NatTypeDetector::NatTypeDetector(DatagramTransport* transport, NatDetectionConfig config)
    : m_transport(transport), m_config(std::move(config)) {}

NatDetectionResult NatTypeDetector::Detect() {
    NatDetectionResult result;
    if (m_transport == nullptr || !m_config.primary_server.valid()) {
        result.detail = "未配置主 STUN 服务器";
        return result;
    }
    StunClient primary(m_transport, m_config.primary_server);

    // Test I：普通 Binding。
    std::string error;
    const auto first = primary.Binding(m_config.timeout, &error);
    if (!first.has_value()) {
        result.detail = "Test I 失败: " + error;
        return result;
    }
    result.mapped = *first;
    result.detail = "Test I 映射=" + first->ToString();
    if (IsLocalMappedEndpoint(m_transport, *first)) {
        result.type = NatType::kOpenInternet;
        result.detail += "；映射即本机地址，判定 OpenInternet";
        return result;
    }

    const bool has_alternate = m_config.alternate_server.valid();
    const bool alternate_changes_ip =
        has_alternate && m_config.alternate_server.AddressString() !=
                             m_config.primary_server.AddressString();

    // 顺序遵循 RFC 3489 §10.1：Test II（换 IP+端口）-> Test III（换端口）
    // -> Test IV（向备用服务器看映射是否变化）。顺序不能颠倒：Test IV 会
    // 让 NAT 的过滤表把备用服务器地址记为「已通信」，此后再做 Test II/III
    // 就会收到本应被端口受限 NAT 丢弃的响应，从而误判成 Full Cone。
    //
    // Test II：change IP + port。只有备用服务器换了 IP 才有意义。
    if (alternate_changes_ip) {
        const auto changed_ip = primary.BindingWithChange(kStunChangeIp | kStunChangePort,
                                                          m_config.timeout, &error);
        result.change_ip_ok = changed_ip.has_value();
        if (result.change_ip_ok) {
            result.type = NatType::kFullCone;
            result.detail += "；Test II 收到换 IP+端口的响应，判定 FullCone";
            return result;
        }
        result.detail += "；Test II 无响应";
    } else {
        result.detail += "；缺少不同 IP 的备用服务器，跳过 Test II";
    }

    // Test III：change port。
    if (has_alternate) {
        const auto changed_port = primary.BindingWithChange(kStunChangePort, m_config.timeout,
                                                            &error);
        result.change_port_ok = changed_port.has_value();
        if (result.change_port_ok) {
            result.type = NatType::kRestrictedCone;
            result.detail += "；Test III 收到换端口响应，判定 RestrictedCone";
            return result;
        }
        result.detail += "；Test III 无响应";
    }

    // Test IV：向备用服务器请求，比较映射是否变化（Symmetric 的直接证据）。
    if (has_alternate) {
        StunClient alternate(m_transport, m_config.alternate_server);
        const auto second = alternate.Binding(m_config.timeout, &error);
        if (second.has_value()) {
            result.alternate_mapped = *second;
            result.mapping_changed = (*second != *first);
            result.detail += "；Test IV 备用映射=" + second->ToString();
            if (result.mapping_changed) {
                result.type = NatType::kSymmetric;
                result.detail += "；两次映射不同，判定 Symmetric";
                return result;
            }
        } else {
            result.detail += "；Test IV 备用服务器无响应(" + error + ")";
        }
        result.type = NatType::kPortRestrictedCone;
        result.detail += "；四类测试均未通过，判定 PortRestrictedCone";
        return result;
    }

    // 只有一个服务器：只能区分 Symmetric 与「未知锥形」。这里保守地报
    // Unknown，并把已知信息写进 detail，避免把猜测当成结论。
    result.detail += "；未配置备用服务器，信息不足，无法判定具体 NAT 类型";
    return result;
}

}  // namespace go2cpp::p2p
