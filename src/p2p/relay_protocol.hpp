#pragma once

/**
 * @file relay_protocol.hpp
 * @brief P2P 中继（简化 TURN）的内部线格式，不对外导出。
 * @details 这是**自定义协议**，不是 RFC 5766 TURN。保留 TURN 的
 *          Allocation / Send / Data 三段式语义，但把属性 TLV、鉴权、
 *          ChannelData 全部去掉，换成固定 16 字节头 + 紧凑 body：
 *
 *          ```
 *          0               1               2               3
 *          +---------------+---------------+---------------+---------------+
 *          |                          magic = "G2TR"                      |
 *          +---------------+---------------+---------------+---------------+
 *          | version (1)   | type (1)      | body length (2, BE)           |
 *          +---------------+---------------+---------------+---------------+
 *          |                        request_id (4, BE)                     |
 *          +---------------+---------------+---------------+---------------+
 *          |                        reserved (4, 必须为 0)                 |
 *          +---------------+---------------+---------------+---------------+
 *          | body ...
 *          ```
 *
 *          body 内的端点编码：`family(1) + port(2) + address(4/16)`，
 *          family 1 = IPv4、2 = IPv6。
 *
 *          为什么不用 RFC 5766：完整 TURN 需要 long-term credential、
 *          MESSAGE-INTEGRITY、ChannelData 绑定、权限与配额，代码量是本模块
 *          的数倍；而本项目的目标是「打洞失败时有一个可用的降级通道」，
 *          中继服务器通常部署在可信内网，不需要跨域鉴权。安全边界见
 *          `docs/modules/p2p-nat-traversal.md`。
 *
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "go2cpp/p2p/endpoint.hpp"

namespace go2cpp::p2p::detail {

/// "G2TR"（Go2cpp TRansport relay）。
inline constexpr std::uint32_t kRelayMagic = 0x47325452U;
inline constexpr std::uint8_t kRelayVersion = 1;
inline constexpr std::size_t kRelayHeaderSize = 16;
inline constexpr std::size_t kRelayMaxBody = 65000;

/// 消息类型。
enum class RelayMessageType : std::uint8_t {
    kAllocateRequest = 1,   ///< 申请一个中继端点。
    kAllocateResponse = 2,  ///< 返回中继端点与生命周期。
    kSendIndication = 3,    ///< 客户端 -> 服务器：向 peer 转发数据。
    kDataIndication = 4,    ///< 服务器 -> 客户端：peer 发来的数据。
    kRefreshRequest = 5,    ///< 续租。
    kRefreshResponse = 6,   ///< 续租响应。
    kReleaseRequest = 7,    ///< 主动释放。
    kReleaseResponse = 8,   ///< 释放响应。
    kErrorResponse = 9,     ///< 错误（status 非 0）。
};

/// 状态码。
enum class RelayStatus : std::uint8_t {
    kOk = 0,
    kInvalidRequest = 1,
    kNoAllocation = 2,
    kAllocationLimit = 3,
    kExpired = 4,
    kUnsupported = 5,
    kInvalidAddress = 6,
};

/// 一条中继消息。
struct RelayMessage {
    RelayMessageType type = RelayMessageType::kAllocateRequest;
    std::uint32_t request_id = 0;
    std::vector<std::uint8_t> body;
};

/// 序列化；body 超过上限返回空 vector。
std::vector<std::uint8_t> SerializeRelayMessage(const RelayMessage& message);

/// 反序列化；格式非法返回 false 并写 error。
bool ParseRelayMessage(const void* data, std::size_t length, RelayMessage* message,
                       std::string* error = nullptr);

/// 向 body 追加一个端点。
void AppendEndpointBody(std::vector<std::uint8_t>* body, const Endpoint& endpoint);

/// 从 body 的 `*offset` 处读一个端点并推进 offset。
bool ReadEndpointBody(const std::vector<std::uint8_t>& body, std::size_t* offset,
                      Endpoint* endpoint);

/// 向 body 追加 32 位大端整数。
void AppendU32Body(std::vector<std::uint8_t>* body, std::uint32_t value);

/// 从 body 的 `*offset` 处读 32 位大端整数并推进 offset。
bool ReadU32Body(const std::vector<std::uint8_t>& body, std::size_t* offset, std::uint32_t* value);

}  // namespace go2cpp::p2p::detail
