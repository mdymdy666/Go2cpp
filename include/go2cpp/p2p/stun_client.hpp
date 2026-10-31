#pragma once

/**
 * @file stun_client.hpp
 * @brief RFC 5389 STUN 客户端：Binding 请求/响应与 NAT 类型检测。
 * @details 客户端只依赖 `DatagramTransport`，因此既能在真实 UDP 上工作，
 *          也能被测试注入「模拟 NAT 的传输」来验证 NAT 类型检测算法。
 *
 *          `Exchange()` 会丢弃事务 ID 不匹配的数据报（同一个 socket 上可能
 *          还有打洞检查包），但**不会**把丢弃的包交还给调用方。因此它只能
 *          在「收集候选阶段」独占使用；连接建立后的检查包由 P2PChannel
 *          自己的收发循环处理。
 *
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "go2cpp/p2p/endpoint.hpp"
#include "go2cpp/p2p/stun_message.hpp"
#include "go2cpp/p2p/transport.hpp"

namespace go2cpp::p2p {

/// 一次 STUN 交换的完整结果。
struct StunExchangeResult {
    StunMessage message;         ///< 解析后的响应。
    Endpoint source;             ///< 响应实际来源（CHANGE-REQUEST 时会与请求目标不同）。
    std::chrono::milliseconds rtt{0};  ///< 从发送到收到响应的耗时。
};

/**
 * @brief 单个 STUN 服务器的客户端。
 *
 * @details 不使用 `connect()`：NAT 类型检测依赖接收来自**不同源地址**的
 *          响应（CHANGE-REQUEST），connected UDP socket 会静默丢弃这些包。
 */
class StunClient {
public:
    StunClient(DatagramTransport* transport, Endpoint server);

    /// 发送一次普通 Binding Request，返回 MAPPED-ADDRESS/XOR-MAPPED-ADDRESS。
    std::optional<Endpoint> Binding(std::chrono::milliseconds timeout,
                                    std::string* error = nullptr);

    /// 发送一次 Binding Request 并返回完整响应（不含 CHANGE-REQUEST）。
    std::optional<StunExchangeResult> Exchange(const StunMessage& request,
                                               std::chrono::milliseconds timeout,
                                               std::string* error = nullptr);

    /// 发送带 CHANGE-REQUEST 的 Binding Request（NAT 类型检测用）。
    std::optional<StunExchangeResult> BindingWithChange(std::uint32_t change_flags,
                                                        std::chrono::milliseconds timeout,
                                                        std::string* error = nullptr);

    Endpoint server() const { return m_server; }
    void set_server(Endpoint server) { m_server = server; }

private:
    DatagramTransport* m_transport = nullptr;
    Endpoint m_server;
    /// 响应缓冲（成员而非栈数组：避免在 Fiber 的 4KB 栈上放大对象）。
    std::vector<std::uint8_t> m_buffer;
};

/// NAT 类型（RFC 3489 的四类 + 未识别/公网直连）。
enum class NatType : std::uint8_t {
    kUnknown = 0,           ///< 探测失败或信息不足。
    kOpenInternet = 1,      ///< 映射地址等于本地地址：没有 NAT。
    kFullCone = 2,          ///< 全锥形：映射与目标无关，且不限制来源。
    kRestrictedCone = 3,    ///< 受限锥形：映射与目标无关，只允许已通信 IP。
    kPortRestrictedCone = 4,///< 端口受限锥形：只允许已通信的 IP:port。
    kSymmetric = 5,         ///< 对称型：每个目标分配不同映射端口。
};

/// 类型的可读名称（日志/报告用）。
std::string NatTypeName(NatType type);

struct NatDetectionConfig {
    /// 主 STUN 服务器（必须支持 CHANGE-REQUEST）。
    Endpoint primary_server;
    /**
     * @brief 备用 STUN 服务器（另一个 IP，或至少另一个端口）。
     * @details 只有提供了与 primary **不同 IP** 的备用地址，才能区分
     *          Full Cone / Restricted / Port Restricted；只给不同端口时
     *          只能区分 Symmetric 与「锥形（未细分）」。
     */
    Endpoint alternate_server;
    /// 单次探测超时。
    std::chrono::milliseconds timeout{500};
};

struct NatDetectionResult {
    NatType type = NatType::kUnknown;
    Endpoint mapped;                 ///< 主服务器看到的映射地址。
    Endpoint alternate_mapped;       ///< 备用服务器看到的映射地址。
    bool change_ip_ok = false;       ///< CHANGE-REQUEST(IP+port) 是否收到响应。
    bool change_port_ok = false;     ///< CHANGE-REQUEST(port) 是否收到响应。
    bool mapping_changed = false;    ///< 两次映射是否不同（Symmetric 的直接证据）。
    std::string detail;              ///< 判定过程的可读说明（诊断用）。
};

/**
 * @brief 基于 RFC 3489 §10.1 的 NAT 类型检测器（简化版）。
 *
 * @details 检测步骤：
 *          1. Test I：向主服务器发普通 Binding，得到映射 M1；
 *             若 M1 就是本机地址 → `kOpenInternet`。
 *          2. Test II：向主服务器发 CHANGE-REQUEST(change IP + port)；
 *             收到响应 → `kFullCone`（NAT 不限制来源地址与端口）。
 *          3. Test III：向主服务器发 CHANGE-REQUEST(change port)；
 *             收到响应 → `kRestrictedCone`（只限制来源 IP，不限制端口）。
 *          4. Test IV：向备用服务器发普通 Binding，得到 M2；
 *             M1 != M2 → `kSymmetric`（映射随目标变化）。
 *          5. 以上都不成立 → `kPortRestrictedCone`。
 *
 *          如果备用服务器与主服务器是**同一个 IP**，Test II 无法改变 IP，
 *          结果会退化为 `kPortRestrictedCone`/`kUnknown`；此时
 *          `NatDetectionResult::detail` 会写明「缺少双 IP 服务器」。
 *
 *          这个检测只覆盖「映射行为 + 过滤行为」的经典四类，不区分
 *          RFC 4787 的 Address-Dependent Mapping 等更细的分类。
 */
class NatTypeDetector {
public:
    NatTypeDetector(DatagramTransport* transport, NatDetectionConfig config);

    /// 执行检测（同步阻塞，最长约 4 × timeout）。
    NatDetectionResult Detect();

private:
    DatagramTransport* m_transport = nullptr;
    NatDetectionConfig m_config;
};

}  // namespace go2cpp::p2p
