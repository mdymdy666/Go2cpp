#pragma once

/// @file protocol.hpp
/// @brief JinShan REQ-008：协议注入入口（rd::net）。
///
/// @details 边界（G6）：本层只提供「把自定义协议挂到 EventLoop 上」的窄接口——
///          借用视图、SendTo 入队、定时器、detach 原因；**不解析任何字节**：
///          不认 magic、不校验字段、不分帧、不重排、不做重传决策、不做握手、
///          不统计协议层丢包。回调全部在 loop 线程（沿用 REQ-007 的单线程约束）。
///
///          命名空间：REQ-007 / REQ-008 统一到 `rd::net`（不再有 `rd::net::plugin`）。
///          依赖方向是单向的：`event_loop.hpp` → `protocol.hpp` → `socket.hpp`；
///          本头只前置声明 `EventLoop` / `ProtocolContext`，不反向 include
///          `event_loop.hpp`，因此没有头文件循环。
///
///          同一个类型里有两组回调，用重载区分：
///          - **事件循环层**：`OnAttached(EventLoop&)`、`OnDatagram(ByteView,
///            const Endpoint&)`、`OnDetached()`、`OnData(ByteView)`、`OnClosed()`。
///            `EventLoop::AttachDatagram/AttachStream` 直接回调这一组，不需要
///            `ProtocolContext`；
///          - **协议层**：一律带 `ProtocolContext&`。`AttachProtocol` 挂载后由
///            桥接器回调这一组，插件作者实现的是这一组。
///          桥接器（`protocol.cpp` 内部）同时是 `Protocol` 与 `ProtocolContext`，
///          把事件循环层转发成协议层，所以 JinShan 侧只写一个
///          `class Xxx : public rd::net::Protocol` 即可。
///
///          零拷贝契约：
///          - 收到的 ByteView 是**借用**视图，回调返回后立即失效；
///          - SendTo **不拷贝 payload**，入队后真正 send 发生在同一 tick 的回调
///            返回之后（队列超过单 tick 预算时顺延到后续 tick），因此调用方必须
///            保证这块内存在 OnSendComplete 之前不被复用或释放；要提前复用请自己
///            拷一份（调用方拷贝，而不是本层偷偷拷）。
///
///          单 tick 预算：每个 socket 每个 tick 最多入队发送 64 个数据报（与 REQ-007
///          的收包上限一致），剩余部分由 0ms 定时器在下一个 tick 继续，避免一个
///          协议把 loop 卡住。

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>

#include "go2cpp/rd_net/socket.hpp"

namespace rd::net {

/// @brief 事件循环（定义见 event_loop.hpp；本头只前置声明，避免循环依赖）。
class EventLoop;

/// @brief 借用字节视图沿用 socket.hpp 的只读视图（data + size，不拥有内存）。

/// @brief 解绑原因。
enum class DetachReason : std::uint8_t {
    kEof = 0,     ///< 对端有序关闭（TCP FIN）。
    kReset = 1,   ///< 连接被重置（TCP RST）。
    kTimeout = 2, ///< 上层判定超时（由协议层决定，本层不产生）。
    kError = 3,   ///< 底层 I/O 错误。
    kClosed = 4,  ///< 本端显式解绑，或 socket 被事件循环摘除。
};

/// @brief 返回解绑原因的可读名称，用于日志与测试失败信息；未知值返回 "Unknown"。
const char* DetachReasonName(DetachReason reason) noexcept;

class ProtocolContext;

/// @brief UDP 协议插件：事件循环层 + 协议层两组回调。
/// @note 插件作者通常只实现协议层（带 `ProtocolContext&` 的那组）；事件循环层
///       是 `EventLoop::AttachDatagram` 直接使用的入口，由 `AttachProtocol` 的
///       桥接器实现并转发。
class Protocol {
public:
    virtual ~Protocol() = default;

    // ---- 事件循环层：EventLoop 直接回调（AttachProtocol 挂载时由桥接器实现）----

    /// @brief 绑定到事件循环后回调一次（loop 线程）。
    virtual void OnAttached(EventLoop& loop) { (void)loop; }

    /// @brief 收到一个数据报。data 是借用视图，回调返回后失效。
    virtual void OnDatagram(ByteView data, const Endpoint& from) {
        (void)data;
        (void)from;
    }

    /// @brief 从事件循环解绑或 socket 因错误被摘除后回调一次。
    virtual void OnDetached() {}

    // ---- 协议层：AttachProtocol 挂载后由桥接器回调（插件作者实现这一组）----

    /// @brief 绑定后回调一次（loop 线程），可在此调度定时器。
    virtual void OnAttached(ProtocolContext& context) { (void)context; }

    /// @brief 收到一个数据报。data 是借用视图，回调返回后失效。
    virtual void OnDatagram(ProtocolContext& context, ByteView data,
                            const Endpoint& from) {
        (void)context;
        (void)data;
        (void)from;
    }

    /// @brief 定时器到期：先回调本方法（带 id），再执行 ScheduleAfter 注册的
    ///        std::function，两者都在 loop 线程。
    virtual void OnTimer(ProtocolContext& context, std::uint64_t timer_id) {
        (void)context;
        (void)timer_id;
    }

    /// @brief 一个已入队的数据报真正发完（成功或失败）：token 与 SendTo 返回值一致。
    virtual void OnSendComplete(ProtocolContext& context, std::uint64_t token,
                                SendResult result) {
        (void)context;
        (void)token;
        (void)result;
    }

    /// @brief 解绑（随后不会再有任何回调）。
    virtual void OnDetached(ProtocolContext& context, DetachReason reason) {
        (void)context;
        (void)reason;
    }
};

/// @brief TCP 流协议插件：事件循环层 + 协议层两组回调。
class StreamProtocol {
public:
    virtual ~StreamProtocol() = default;

    // ---- 事件循环层：EventLoop 直接回调（AttachProtocol 挂载时由桥接器实现）----

    /// @brief 绑定到事件循环后回调一次（loop 线程）。
    virtual void OnAttached(EventLoop& loop) { (void)loop; }

    /// @brief 收到一段字节。data 是借用视图，回调返回后失效。
    virtual void OnData(ByteView data) { (void)data; }

    /// @brief 对端关闭/重置（随后会自动 Detach，并回调 OnDetached）。
    virtual void OnClosed() {}

    /// @brief 从事件循环解绑后回调一次。
    virtual void OnDetached() {}

    // ---- 协议层：AttachProtocol 挂载后由桥接器回调（插件作者实现这一组）----

    /// @brief 绑定后回调一次（loop 线程）。
    virtual void OnAttached(ProtocolContext& context) { (void)context; }

    /// @brief 收到一段字节。data 是借用视图，回调返回后失效。
    virtual void OnData(ProtocolContext& context, ByteView data) {
        (void)context;
        (void)data;
    }

    /// @brief 解绑（对端关闭时原因通常是 kEof）。
    virtual void OnDetached(ProtocolContext& context, DetachReason reason) {
        (void)context;
        (void)reason;
    }
};

/// @brief 回调里拿到的上下文：发送、定时器、本地端点。
/// @note 只在回调期间有效；不要在回调返回后保存引用。
class ProtocolContext {
public:
    virtual ~ProtocolContext() = default;

    /// @brief 入队一个数据报，**不同步 send**。返回 token（从 1 开始单调递增），
    ///        真正发送完成后用同一个 token 回调 OnSendComplete。
    /// @return status=kOk 表示已入队；kWouldBlock 表示队列满（未入队，token=0）；
    ///         kInvalid 表示已解绑（未入队，token=0）。
    virtual SendResult SendTo(ByteView data, const Endpoint& to) = 0;

    /// @brief 延迟 delay 后在 loop 线程回调（返回定时器 id，>0；0 表示已解绑）。
    virtual std::uint64_t ScheduleAfter(std::chrono::milliseconds delay,
                                       std::function<void()> callback) = 0;

    /// @brief 本端绑定端点（Attach 时的快照）。
    virtual const Endpoint& LocalEndpoint() const = 0;
};

/// @brief 一次挂载的 RAII 句柄：析构时自动解绑（并回调 OnDetached）。
/// @note 必须比 Protocol/StreamProtocol 与 EventLoop 先析构。
class ProtocolAttachment {
public:
    ProtocolAttachment() = default;
    virtual ~ProtocolAttachment() = default;
    ProtocolAttachment(const ProtocolAttachment&) = delete;
    ProtocolAttachment& operator=(const ProtocolAttachment&) = delete;
};

/// @brief 把 UDP 协议挂到一个已绑定的事件循环 socket 上；失败返回 nullptr。
std::unique_ptr<ProtocolAttachment> AttachProtocol(EventLoop& loop,
                                                  DatagramSocket& socket,
                                                  Protocol& handler);

/// @brief 把 TCP 流协议挂到一个已绑定的事件循环 socket 上；失败返回 nullptr。
std::unique_ptr<ProtocolAttachment> AttachProtocol(EventLoop& loop,
                                                  StreamSocket& socket,
                                                  StreamProtocol& handler);

}  // namespace rd::net
