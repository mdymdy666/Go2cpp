#pragma once

/**
 * @file server.hpp
 * @brief RPC 服务端：服务注册、TCP 监听、同步/流式方法分发。
 *
 * @details
 * 服务端为每条连接起两个线程：
 *
 * - **读线程**：`recv → FrameCodec::Feed → 解析帧`。请求入队；`Cancel` 帧
 *   直接置位对应调用的取消标志；心跳立刻回 `Pong`。
 * - **工作线程**：串行执行该连接上的请求（handler 内可阻塞）。流式 handler
 *   在同一线程里通过 `ServerStream::Send` 逐条推送，返回后框架补发
 *   `StreamEnd`。
 *
 * 为什么拆两个线程：如果 handler 和读帧共用一个线程，一个慢 handler 或
 * 长时间流式 handler 会让 `Cancel` 帧永远读不到，取消就变成了「等它跑完」。
 * 拆开后，取消/心跳在 handler 阻塞期间仍然可用；代价是每连接 2 个线程
 * （线程数由客户端连接数决定，RPC 场景可接受）。
 *
 * ## 线程与生命周期
 *
 * - `Register`/`Listen`/`Stop` 由调用方线程使用；
 * - `Listen` 启动后台 accept 线程并立即返回；
 * - `Stop` 关闭监听、shutdown 所有连接、join 全部线程，可重复调用；
 * - 服务对象必须比 `Server` 活得久（注册表只存裸指针）。
 *
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "go2cpp/reflect/buffer.hpp"
#include "go2cpp/reflect/serializer_impl.hpp"
#include "go2cpp/rpc/protocol.hpp"
#include "go2cpp/rpc/registry.hpp"
#include "go2cpp/rpc/status.hpp"
#include "go2cpp/rpc/transport.hpp"

namespace go2cpp::rpc {

namespace detail {
class StreamSender;
class ServerImpl;
}  // namespace detail

/**
 * @brief 服务端流：handler 里逐条推送消息，返回时框架发 StreamEnd。
 *
 * @details `Send` 失败通常意味着对端断开或缓冲写失败；handler 应停止推送
 *          并返回错误状态。`cancelled()` 反映对端 `Cancel`，长循环里应周期检查。
 */
class ServerStream {
   public:
    /// @brief 空流（仅默认构造/测试用）。
    ServerStream() = default;

    /**
     * @brief 用内部发送器构造（框架使用）。
     * @param sender 发送器（所有权共享）。
     */
    explicit ServerStream(std::shared_ptr<detail::StreamSender> sender)
        : m_sender(std::move(sender)) {}

    /**
     * @brief 发送一条消息（序列化后发 StreamData 帧）。
     * @tparam T 消息类型（必须可序列化）。
     * @param value 消息。
     * @return 成功 true；流已结束/连接断开/序列化失败返回 false。
     */
    template <typename T>
    bool Send(const T& value) {
        thread_local reflect::Buffer scratch;
        scratch.Clear();
        if (!reflect::Serialize(value, scratch)) {
            return false;
        }
        return SendSerialized(scratch.data().data(), scratch.size());
    }

    /**
     * @brief 发送已序列化的消息字节。
     * @param data 字节首地址（`size == 0` 时可为 nullptr）。
     * @param size 字节数。
     * @return 成功 true。
     */
    bool SendSerialized(const void* data, std::size_t size);

    /**
     * @brief 显式结束流（幂等）。
     * @param status 最终状态（默认成功）。
     * @return 首次结束返回 true；已结束返回 false。
     */
    Status Finish(Status status = Status::Ok());

    /// @brief 对端是否已请求取消。
    bool cancelled() const;

    /// @brief 流 id（等于请求的 call id）。
    std::uint64_t id() const;

    /// @brief 是否有效（由框架构造）。
    bool valid() const noexcept { return static_cast<bool>(m_sender); }

   private:
    std::shared_ptr<detail::StreamSender> m_sender;  ///< 帧发送器。
};

/**
 * @brief 服务端选项。
 */
struct ServerOptions {
    /// 单帧载荷上限（必须与客户端一致）。
    std::size_t max_payload_bytes = 4u * 1024u * 1024u;
    /// 是否启用帧校验和（必须与客户端一致）。
    bool checksum = true;
    /// listen backlog。
    int backlog = 128;
    /// accept 轮询间隔（毫秒）：`Stop` 最坏在这么多毫秒后返回。
    int accept_poll_ms = 20;
    /// 同时在线连接上限；超过后新连接被立即关闭。
    std::size_t max_connections = 1024;
};

/**
 * @brief RPC 服务端。
 *
 * @details 典型用法：
 * @code
 *   rpc::Server server;
 *   server.Register("User", &user_service);   // 方法表由 GO2CPP_RPC_SERVICE 提供
 *   server.Listen(":8080");
 *   ...
 *   server.Stop();
 * @endcode
 */
class Server {
   public:
    /// @brief 构造（不监听）。
    explicit Server(ServerOptions options = {});
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    /**
     * @brief 注册服务（模板版）。
     * @tparam Service 服务类型。
     * @param service_name 服务名（`"User"`）。
     * @param service 服务对象（生命周期覆盖 Server）。
     * @return 成功 true；重名/非法名/方法表缺失 false。
     */
    template <typename Service>
    bool Register(std::string_view service_name, Service* service) {
        return MutableRegistry().AddService(service_name, service);
    }

    /**
     * @brief 注册一组显式方法（不用宏时）。
     * @param service_name 服务名。
     * @param service 服务对象。
     * @param bindings 方法描述数组。
     * @param count 数组长度。
     * @return 成功 true。
     */
    bool Register(std::string_view service_name, void* service,
                  const MethodBinding* bindings, std::size_t count) {
        return MutableRegistry().AddBindings(service_name, service, bindings, count);
    }

    /**
     * @brief 开始监听（后台 accept 线程）。
     * @param address `":8080"` / `"127.0.0.1:8080"`。
     * @return 成功 true；地址非法/端口占用/已监听 false。
     */
    bool Listen(std::string_view address);

    /// @brief 开始监听（Endpoint 版）。
    bool Listen(const Endpoint& endpoint);

    /**
     * @brief 接管一条已建立的传输（WebSocket 等），交给服务端处理。
     * @param transport 传输对象（所有权转移）。
     * @return 成功 true；服务未运行或连接数已满 false（传输被关闭）。
     */
    bool AddConnection(std::unique_ptr<Transport> transport);

    /// @brief 停止监听并等待所有连接线程结束（可重复调用）。
    void Stop();

    /// @brief 是否正在监听。
    bool running() const;

    /// @brief 实际监听端口（`Listen` 之后有效）。
    std::uint16_t port() const;

    /// @brief 监听地址文本。
    std::string address() const;

    /// @brief 注册表（只读）。
    const ServiceRegistry& registry() const;

   private:
    /// 注册表可写引用（模板 `Register` 用；实现里做冻结检查在 AddBindings）。
    ServiceRegistry& MutableRegistry();

    std::unique_ptr<detail::ServerImpl> m_impl;  ///< PIMPL（accept/连接线程/注册表）。
};

}  // namespace go2cpp::rpc
