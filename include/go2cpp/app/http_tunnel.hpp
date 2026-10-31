#pragma once

/**
 * @file http_tunnel.hpp
 * @brief 应用层帧流 over HTTP（TCP）的批式隧道：客户端传输 + 服务端会话。
 *
 * @details 有些部署环境只允许 HTTP 出网（企业代理、CDN、负载均衡），UDP 或
 *          长连接不可用。本文件提供一种**批式半双工隧道**：
 *
 *          ```
 *          Client.Connection --WriteAll--> m_out
 *                 Read() --POST(body=m_out)--> HttpServer handler
 *                                                    |
 *                                          HttpTunnelServer::HandleRequest
 *                                                    |
 *                                                    v
 *                                          Server.Connection（处理帧、产生响应帧）
 *                                                    |
 *                 Read() <--response body-- 返回 m_out
 *          ```
 *
 *          每次 `Read` 在没有待处理响应时，把当前所有待发帧作为一个 HTTP
 *          body POST 出去；服务端 handler 把 body 喂给服务端 `Connection`，
 *          驱动它处理完（可能产生 ACK/WINDOW_UPDATE/业务响应），再把响应帧
 *          作为 body 返回。HTTP/1.1 的可靠有序与长度语义替我们完成了
 *          「传输层」的职责，应用层继续使用同一套流表/帧格式。
 *
 * ## 适用边界（如实说明）
 *
 *          - **半双工批式**：同一时刻只有一个方向的数据在 HTTP body 里，
 *            不是全双工长连接；每次交换有 HTTP 请求开销；
 *          - **适合**资源下发、KV 同步、清单拉取等「请求-响应」明显的场景；
 *            不适合实时双向聊天/游戏指令流（那用 Bento Link）；
 *          - 一次 HTTP 请求的 body 受服务端/代理大小限制（通常 1–64 MiB）；
 *          - 一个 `HttpTunnelServer` 绑定一个 Connection，**不可并发**处理
 *            多个请求（HttpServer 的多个 worker 同时进来会竞争流表）；需要
 *            并发时按连接/会话创建多个实例。
 *
 * ## 线程模型
 *
 *          - `HttpTunnelClientTransport` 在 Connection 的 IO 线程使用；
 *            `exchange` 回调内部会发起同步 HTTP 请求，因此该线程会阻塞到
 *            HTTP 往返完成；
 *          - `HttpTunnelServer::HandleRequest` 在 HttpServer 的 worker 线程
 *            执行；事件回调同线程。调用方需自行保证同一实例不并发进入。
 *
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "go2cpp/app/connection.hpp"
#include "go2cpp/app/transport.hpp"

namespace go2cpp::app {

/**
 * @brief 一次 HTTP 交换：把请求 body 发出去，取回响应 body。
 * @return 成功返回 true；网络/HTTP 错误返回 false。
 */
using HttpExchange =
    std::function<bool(const std::vector<std::uint8_t>& request,
                       std::vector<std::uint8_t>* response)>;

/**
 * @brief 客户端传输：把帧字节流攒进请求 body，`Read` 时触发一次交换。
 */
class HttpTunnelClientTransport final : public ITransport {
   public:
    explicit HttpTunnelClientTransport(HttpExchange exchange);

    bool WriteAll(const std::uint8_t* data, std::size_t size) override;
    int Read(std::uint8_t* data, std::size_t capacity,
             std::uint32_t timeout_ms) override;
    void Close() override;
    bool closed() const override;
    std::string Describe() const override;

    /// @brief 已完成的 HTTP 交换次数。
    std::size_t exchanges() const noexcept { return m_exchanges; }
    /// @brief 待发字节数。
    std::size_t pending_out() const noexcept { return m_out.size(); }
    /// @brief 待读字节数。
    std::size_t pending_in() const noexcept { return m_in.size() - m_in_offset; }

   private:
    HttpExchange m_exchange;             ///< 交换回调。
    std::vector<std::uint8_t> m_out;     ///< 待发字节。
    std::vector<std::uint8_t> m_in;      ///< 待读字节。
    std::size_t m_in_offset = 0;         ///< 已读偏移。
    std::size_t m_exchanges = 0;         ///< 交换次数。
    bool m_closed = false;               ///< 是否关闭。
};

/**
 * @brief 服务端会话：一个 Connection + 一次 HTTP body 的处理入口。
 */
class HttpTunnelServer {
   public:
    explicit HttpTunnelServer(ConnectionConfig config = {});
    ~HttpTunnelServer();

    HttpTunnelServer(const HttpTunnelServer&) = delete;
    HttpTunnelServer& operator=(const HttpTunnelServer&) = delete;

    /// @brief 服务端 Connection（仅允许在 `HandleRequest` 同一线程访问）。
    Connection& connection() { return *m_conn; }

    /// @brief 资源落点工厂（转发给 Connection）。
    void SetResourceSinkFactory(
        std::function<std::unique_ptr<ResourceSink>(const ResourceMeta&)> factory);
    /// @brief 事件回调（在 HttpServer worker 线程执行）。
    void SetEventHandler(std::function<void(const Event&)> handler);

    /**
     * @brief 处理一次 HTTP 请求 body。
     * @param body 请求字节（应用层帧流）。
     * @param max_steps 最多驱动的 Step 次数（防止病态输入占满 worker）。
     * @return 响应字节（包含服务端 preface/SETTINGS 与本次产生的所有帧）。
     */
    std::vector<std::uint8_t> HandleRequest(const std::vector<std::uint8_t>& body,
                                            std::uint32_t max_steps = 256);

    /// @brief 已处理的请求数。
    std::size_t handled_requests() const noexcept { return m_handled_requests; }
    /// @brief 服务端是否已关闭。
    bool closed() const { return m_conn->closed(); }

   private:
    class BatchTransport;

    BatchTransport* m_transport = nullptr;  ///< 观察指针（所有权在 m_conn）。
    std::unique_ptr<Connection> m_conn;     ///< 连接。
    std::size_t m_handled_requests = 0;     ///< 请求计数。
};

}  // namespace go2cpp::app
