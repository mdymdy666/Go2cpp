#pragma once

/**
 * @file client.hpp
 * @brief NAT 后的隧道客户端：一条命令把本地端口发布成公网 URL。
 *
 * @details 客户端只做四件事：
 *
 * 1. 连服务端**控制端口**，发 `HELLO`（子域名 / 本地端口 / token）；
 * 2. 收到 `HELLO_ACK` 后进入在线状态，把 `public_url` 暴露给 main 打印；
 * 3. 收到 `OPEN` 就**新建一条到本地服务的 TCP**，用 `RelayPipe` 双向搬运；
 * 4. 控制连接断开后按指数退避重连（0.5 s → 15 s，带抖动），重连成功时
 *    用同一子域名重新注册。
 *
 * ## 为什么本地每条流新建一条 TCP
 *
 * 隧道流与浏览器连接一一对应，本地也一条一条地连——这样本地服务看到的是
 * 标准的并发连接，不需要任何改造（不要求支持 HTTP/2、不要求长连接复用）。
 * 代价是每个浏览器连接多一次本地 `connect`（回环，几十微秒）。
 *
 * ## 退避
 *
 * `reconnect_min_ms` 起步、每次失败翻倍、封顶 `reconnect_max_ms`，并叠加
 * ±20% 抖动，避免大量客户端在服务端重启后同时重连。
 *
 * @author go2cpp contributors
 * @date 2026-11-02
 * @copyright MIT License
 */

#include "go2cpp/bento/net/backoff.hpp"
#include "go2cpp/metrics/metrics.hpp"
#include "go2cpp/tunnel/mux.hpp"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace go2cpp {
namespace tunnel {

/// `TunnelClient` 配置。
struct TunnelClientConfig {
    /// 服务端控制端口主机。
    std::string server_host = "127.0.0.1";
    /// 服务端控制端口。
    std::uint16_t server_port = 7443;
    /// 本地服务监听地址。
    std::string local_host = "127.0.0.1";
    /// 本地服务端口。
    std::uint16_t local_port = 8080;
    /// 期望子域名；空表示由服务端随机分配。
    std::string subdomain;
    /// 鉴权 token（服务端开启时必须一致）。
    std::string token;
    /// 连接服务端超时（毫秒）。
    int dial_timeout_ms = 5000;
    /// 重连最小/最大间隔（毫秒）。
    int reconnect_min_ms = 500;
    int reconnect_max_ms = 15000;
    /// 单流接收队列上限（字节）。
    std::size_t recv_queue_cap = 8U * 1024U * 1024U;
    /// 空闲超时 / 心跳间隔（毫秒）。
    int idle_timeout_ms = 90000;
    int ping_interval_ms = 15000;
    /// 本地并发流上限；超出直接回 503。
    std::size_t max_local_streams = 128;
    /// 是否启用帧尾 CRC（两端必须一致）。
    bool with_checksum = false;
    /// `HELLO` 等待 `HELLO_ACK` 的超时（毫秒）。
    int hello_timeout_ms = 10000;
    /// 数据连接数（对应 frp 的 work conn / rathole 的 data channel）。
    /// 0 表示所有流都走控制连接；>0 时额外建立 N 条只承载流的连接，
    /// 由服务端轮询分配，避免控制面与数据面争用同一条 TCP。
    int data_conns = 0;
    /// 重连抖动比例（透传给 `bento::Backoff`）。
    double reconnect_jitter = 0.2;
};

/// 客户端运行期统计。
struct TunnelClientStats {
    bool online = false;
    std::uint64_t reconnects = 0;
    std::uint64_t streams_total = 0;
    std::uint64_t bytes_in = 0;
    std::uint64_t bytes_out = 0;
    std::string subdomain;
    std::string public_url;
};

/**
 * @brief 隧道客户端。
 *
 * @details 生命周期：`Start()` 后后台线程持续「连接 → 在线 → 重连」；`Stop()`
 *          置停并 join。`Stop()` 返回后不会再有工作线程访问本对象。
 */
class TunnelClient final {
public:
    /// 事件回调（上线 / 下线 / 重连）；在后台线程调用，必须快速返回。
    using EventHandler = std::function<void(const std::string&)>;

    explicit TunnelClient(TunnelClientConfig config);
    ~TunnelClient();

    TunnelClient(const TunnelClient&) = delete;
    TunnelClient& operator=(const TunnelClient&) = delete;

    /**
     * @brief 启动后台连接线程（立即返回）。
     * @param err 失败原因（可为空）。
     */
    bool Start(ErrorPtr* err);

    /// 停止并等待后台线程与本地转发线程退出。
    void Stop();

    /**
     * @brief 等待首次注册成功。
     * @param timeout_ms 最长等待（毫秒）。
     * @return 成功在线返回 true；超时/被拒绝返回 false。
     */
    bool WaitOnline(int timeout_ms);

    bool online() const noexcept {
        return m_online.load(std::memory_order_acquire);
    }

    /// 当前公网 URL（未上线时为空）。
    std::string public_url() const;

    /// 当前生效的子域名（未上线时为空）。
    std::string subdomain() const;

    /// 运行期统计快照。
    TunnelClientStats Stats() const;

    /// 服务端是否已发出 `GOAWAY`（优雅关闭中；客户端不再重连）。
    bool server_draining() const noexcept {
        return m_server_draining.load(std::memory_order_acquire);
    }

    /// 当前数据连接数。
    int data_connections() const noexcept {
        return m_config.data_conns;
    }

    /// 设置事件回调。
    void set_event_handler(EventHandler handler);

private:
    void RunLoop();
    /// 连接一次并跑到断开；返回是否曾经注册成功。
    bool RunOnce();

    /// 完整的「挑战-应答 → HELLO → HELLO_ACK」握手，成功返回已上线的会话。
    /// @param raw_stream_id 非 0 时走 per-stream 裸 TCP：HELLO 后立刻发
    ///        `kRawBind`，**不** Start MuxSession，返回的会话只持有连接。
    bool ConnectAndRegister(bool data_role, MuxSession::Ptr* session,
                            HelloAck* ack, std::string* err,
                            std::uint32_t raw_stream_id = 0);
    /// per-stream 裸 TCP：拨数据连接 → 绑定 → 本地拨号 → splice。
    void RelayRawStream(std::uint32_t stream_id);
    /// 启动数据连接线程（只启动一次）。
    void StartDataConnections();
    /// 一条数据连接的「连接 → 在线 → 重连」循环。
    void DataLoop();
    /// 读 `bento::Backoff` 配置。
    bento::Backoff MakeBackoff() const;
    /// 处理一条新隧道流（在接收线程里被调用，立即起线程并返回）。
    void HandleOpen(const MuxStream::Ptr& stream);
    /// 在独立线程里把隧道流与本地 TCP 对接。
    void RelayLocal(const MuxStream::Ptr& stream);
    void Emit(const std::string& message) const;
    void SetState(bool online_now, const std::string& subdomain,
                  const std::string& url);

    TunnelClientConfig m_config;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_online{false};
    std::thread m_run_thread;

    mutable std::mutex m_state_mutex;
    std::condition_variable m_state_cv;
    std::string m_subdomain;
    std::string m_public_url;

    mutable std::mutex m_event_mutex;
    EventHandler m_event_handler;

    std::atomic<std::uint64_t> m_reconnects{0};
    std::atomic<std::uint64_t> m_streams_total{0};
    std::atomic<std::uint64_t> m_bytes_in{0};
    std::atomic<std::uint64_t> m_bytes_out{0};
    /// 控制连接的代际：断开时 +1，数据连接据此主动重连。
    std::atomic<std::uint64_t> m_generation{0};
    std::atomic<bool> m_server_draining{false};
    std::atomic<bool> m_data_started{false};
    std::vector<std::thread> m_data_threads;

    /// 鉴权：token -> 32B PSK（与 Bento 的 PSK 派生一致）。
    Psk m_psk{};
    bool m_auth_required = false;

    go2cpp::metrics::Counter* m_metric_reconnects = nullptr;
    go2cpp::metrics::Counter* m_metric_streams = nullptr;
    go2cpp::metrics::Gauge* m_metric_online = nullptr;

    /// 本地转发 / raw 流线程的共享记账块（detached 线程经 `shared_ptr` 持有）。
    ///
    /// @details 与 `TunnelServer::WorkerRegistry` 同型：`Stop()` 带超时等待，
    ///          超时后对象仍会析构；记账块放 `shared_ptr` 里，线程最后一次
    ///          `--active` / `notify_all` 落在存活对象上，不会出现 condvar
    ///          已销毁后仍被 broadcast 的 use-after-destroy。
    struct WorkerRegistry {
        std::mutex mutex;              ///< 保护下面三个字段。
        std::condition_variable cv;    ///< 计数归零时唤醒 `Stop()`。
        std::size_t active = 0;        ///< 在跑的转发线程数（兼作并发额度）。
        bool stopping = false;         ///< `Stop()` 已开始，拒绝再起线程。
    };

    /// 所有转发线程共享同一个记账块；`shared_ptr` 保证线程退出时它存活。
    std::shared_ptr<WorkerRegistry> m_workers =
        std::make_shared<WorkerRegistry>();
};

}  // namespace tunnel
}  // namespace go2cpp
