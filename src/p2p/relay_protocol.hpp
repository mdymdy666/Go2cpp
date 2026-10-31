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
 *          数据面（每包）走零拷贝路径：`ParseRelayMessageView` 只校验头部并
 *          把 body 指进原缓冲区，`BuildRelayMessage`/`BuildRelayHeader` +
 *          `EncodeEndpointBody` 直接写调用方 scratch（或 iovec 分段），
 *          因此转发一个包不需要任何堆分配，也不需要中间拷贝。
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

/**
 * @brief 中继消息的零拷贝视图：`body` 指向调用方缓冲区，不拥有内存。
 * @note 生命周期由调用方保证；解析后不再访问原始数据以外的内存。
 */
struct RelayMessageView {
    RelayMessageType type = RelayMessageType::kAllocateRequest;
    std::uint32_t request_id = 0;
    const std::uint8_t* body = nullptr;
    std::size_t body_length = 0;
};

/// 序列化；body 超过上限返回空 vector。
std::vector<std::uint8_t> SerializeRelayMessage(const RelayMessage& message);

/// 反序列化；格式非法返回 false 并写 error。
bool ParseRelayMessage(const void* data, std::size_t length, RelayMessage* message,
                       std::string* error = nullptr);

/**
 * @brief 零拷贝解析：只校验头部并把 body 指进原始缓冲区（不做任何分配）。
 * @return 格式非法返回 false（可写 error）。
 */
bool ParseRelayMessageView(const void* data, std::size_t length, RelayMessageView* message,
                           std::string* error = nullptr);

/**
 * @brief 把完整消息写进调用方缓冲区（头部 + body），返回写入字节数；失败返回 0。
 * @details 热路径（每包转发）用它替代「先拼 body 再序列化」的两次分配 + 两次拷贝。
 */
std::size_t BuildRelayMessage(std::uint8_t* out, std::size_t capacity, RelayMessageType type,
                              std::uint32_t request_id, const void* body,
                              std::size_t body_length);

/// 只写 16 字节头（供 iovec 零拷贝发送用）；容量不足返回 0。
std::size_t BuildRelayHeader(std::uint8_t* out, std::size_t capacity, RelayMessageType type,
                             std::uint32_t request_id, std::size_t body_length);

/// 把端点编码进调用方缓冲区（family + port + address），返回字节数；失败返回 0。
std::size_t EncodeEndpointBody(std::uint8_t* out, std::size_t capacity, const Endpoint& endpoint);

/// 向 body 追加一个端点。
void AppendEndpointBody(std::vector<std::uint8_t>* body, const Endpoint& endpoint);

/// 从 body 的 `*offset` 处读一个端点并推进 offset。
bool ReadEndpointBody(const std::vector<std::uint8_t>& body, std::size_t* offset,
                      Endpoint* endpoint);

/// 零拷贝版本：从裸 body 指针读端点并推进 offset。
bool ReadEndpointBodyView(const std::uint8_t* body, std::size_t body_length, std::size_t* offset,
                          Endpoint* endpoint);

/// 向 body 追加 32 位大端整数。
void AppendU32Body(std::vector<std::uint8_t>* body, std::uint32_t value);

/// 从 body 的 `*offset` 处读 32 位大端整数并推进 offset。
bool ReadU32Body(const std::vector<std::uint8_t>& body, std::size_t* offset, std::uint32_t* value);

/// 零拷贝版本：从裸 body 指针读 32 位大端整数并推进 offset。
bool ReadU32BodyView(const std::uint8_t* body, std::size_t body_length, std::size_t* offset,
                     std::uint32_t* value);

}  // namespace go2cpp::p2p::detail
