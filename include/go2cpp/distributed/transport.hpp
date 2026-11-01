#pragma once

/**
 * @file transport.hpp
 * @brief 把协调者暴露给其他进程：TCP 服务端 + 客户端（二进制帧协议）。
 *
 * ## 为什么是 TCP 而不是 HTTP
 *
 * 协调者的每个操作都是一次小请求/小响应（几十字节），HTTP 的头解析与
 * 连接管理在这个场景里是纯开销；而项目已有成熟的 `net::Socket`（非阻塞 +
 * poll 等待、跨线程 close 取消）与 `relay::Hub`（连接生命周期），直接复用
 * 比引入 HTTP 服务器轻得多。协议只有「4 字节大端长度 + 负载」，负载首字节
 * 是操作码，第二段是状态码，其余是字段；没有版本协商——服务端与客户端
 * 来自同一个构建，跨版本部署需要重新编译（文档里明确标注）。
 *
 * ## 断线与租约
 *
 * 连接断开**不会**立即撤销该客户端持有的租约。原因：一次 TCP 重传抖动
 * 不应该让一个正在执行任务的进程瞬间丢锁。租约按 TTL 自然过期，客户端
 * 重连后继续用原 `LeaseId` 续约（若还没过期）。这也是为什么服务端不做
 * 「连接 → 租约」的反向清理。
 *
 * ## 线程模型
 *
 * - 服务端：1 个 accept 线程 + 每连接 1 个处理线程；所有连接登记在
 *   `relay::Hub` 里，`Stop()` 通过 Hub 一次性关闭，处理线程随之退出。
 * - 客户端：所有请求用一把互斥锁串行化（一个连接上不能并发写帧）。需要
 *   更高吞吐时按线程/句柄创建多个客户端实例，而不是在协议层做多路复用。
 *
 * 客户端不自动重试「已经发出但没收到响应」的请求：重试一次 `AcquireLock`
 * 可能让调用方误判自己是新持有者。正确做法是失败后 `QueryLock` 对账。
 *
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include "go2cpp/distributed/coordinator.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace go2cpp::distributed {

/// 服务端配置。
struct CoordinatorServerOptions {
    std::string host{"127.0.0.1"};       ///< 监听地址；空串表示通配。
    std::uint16_t port{0};               ///< 监听端口；0 表示内核分配（`port()` 查询）。
    int backlog{64};                     ///< listen backlog。
    Duration accept_poll{100};           ///< accept 超时轮询周期（用于响应 Stop）。
    Duration io_timeout{2000};           ///< 单次连接读/写超时。
    std::size_t max_frame_bytes{1U << 20};  ///< 单帧上限（防御超大请求）。
};

/**
 * @brief 单进程协调者的 TCP 服务端。
 *
 * 生命周期：`Start()` 之后才能接受连接；析构会 `Stop()`。同一进程可以同时
 * 有多个服务端（不同端口），它们共享同一个协调者时语义等价于多入口。
 */
class CoordinatorServer final {
public:
    explicit CoordinatorServer(ICoordinator &coordinator, CoordinatorServerOptions options = {});
    ~CoordinatorServer();

    CoordinatorServer(const CoordinatorServer &) = delete;
    CoordinatorServer &operator=(const CoordinatorServer &) = delete;
    CoordinatorServer(CoordinatorServer &&) = delete;
    CoordinatorServer &operator=(CoordinatorServer &&) = delete;

    /**
     * @brief 绑定端口并启动 accept 线程。
     * @return 成功 `kOk`；端口被占用/地址非法 `kUnavailable`；已启动 `kClosed`。
     */
    Outcome Start();

    /// @brief 停止监听、关闭全部连接并 join 处理线程（幂等）。
    void Stop();

    bool running() const;
    /// @brief 实际监听端口（未启动时为 0）。
    std::uint16_t port() const;
    /// @brief 当前活动连接数。
    std::size_t active_connections() const;
    /// @brief 累计成功处理的请求数。
    std::uint64_t requests_served() const;
    const CoordinatorServerOptions &options() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

/// 客户端配置。
struct CoordinatorClientOptions {
    Duration connect_timeout{2000};  ///< 建连超时。
    Duration io_timeout{2000};       ///< 单次请求读/写超时。
    std::size_t max_frame_bytes{4U << 20};  ///< 单帧上限（覆盖较大的服务列表）。
};

/**
 * @brief 远程协调者客户端：实现 `ICoordinator`，可直接传给三个原语的 facade。
 *
 * @code
 * CoordinatorClient client("127.0.0.1", port);
 * ServiceRegistry registry(client);
 * DistributedLock locks(client);
 * LeaderElection election(client);
 * @endcode
 *
 * 失败语义：连接失败或超时返回 `kUnavailable`，帧损坏返回 `kProtocolError`。
 * 网络错误会关闭当前连接，下一次调用自动重连（不会重放失败请求）。
 */
class CoordinatorClient final : public ICoordinator {
public:
    CoordinatorClient(std::string host, std::uint16_t port,
                      CoordinatorClientOptions options = {});
    ~CoordinatorClient() override;

    CoordinatorClient(const CoordinatorClient &) = delete;
    CoordinatorClient &operator=(const CoordinatorClient &) = delete;
    CoordinatorClient(CoordinatorClient &&) = delete;
    CoordinatorClient &operator=(CoordinatorClient &&) = delete;

    /// @brief 显式建连（幂等）。首次调用任一操作时也会自动建连。
    Outcome Connect();
    /// @brief 关闭连接（幂等）。关闭后下一次操作会重新建连，除非对象已析构。
    void Close();
    bool connected() const;

    const std::string &host() const;
    std::uint16_t port() const;

    // ---- ICoordinator ----
    Result<LeaseView> RegisterService(const ServiceRegistration &registration) override;
    Result<LeaseView> RenewLease(LeaseId lease, std::string_view owner) override;
    Outcome ReleaseLease(LeaseId lease, std::string_view owner) override;
    std::vector<ServiceInstance> LookupService(std::string_view service) const override;
    std::vector<std::string> ListServices() const override;
    Result<LeaseView> AcquireLock(std::string_view key, std::string_view owner,
                                  Duration ttl) override;
    std::optional<LockInfo> QueryLock(std::string_view key) const override;
    std::vector<LockInfo> ListLocks() const override;
    Result<LeaseView> JoinElection(std::string_view election, std::string_view candidate,
                                   Duration ttl) override;
    ElectionState QueryElection(std::string_view election) const override;
    std::vector<std::string> ListElections() const override;
    std::size_t SweepExpired() override;
    CoordinatorStats Stats() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace go2cpp::distributed
