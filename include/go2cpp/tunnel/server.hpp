#pragma once

/**
 * @file server.hpp
 * @brief 公网侧的隧道服务端：控制端口（客户端连入）+ 公网 HTTP 端口（浏览器访问）。
 *
 * @details 部署形态是**一个跑在公网 VPS 上的独立进程**：
 *
 * ```
 *   浏览器 ──http──▶ :8443（公网端口）──┐
 *                                      ├─ 按 Host / 路径前缀路由
 *   客户端 ──tcp───▶ :7443（控制端口）──┘  每浏览器连接 = 一条 MuxStream
 * ```
 *
 * ## 路由规则（按优先级）
 *
 * 1. **路径前缀**：`GET /shop/a.png` → 隧道 `shop`，转发路径 `/a.png`
 *    （`strip_prefix=false` 时原样转发）。无需任何 DNS 配置，URL 形如
 *    `http://<vps>:8443/shop/`。
 * 2. **子域名**：`Host: shop.tunnel.example.com` → 隧道 `shop`，路径不变。
 *    需要泛域名解析 `*.tunnel.example.com` 指向 VPS；这是生产推荐方式
 *    （keep-alive 完整保留，无路径重写副作用）。
 * 3. 都没命中：`/` 返回隧道目录页，其它路径返回 404。
 *
 * ## 路径重写的代价
 *
 * 路径前缀模式会改写请求行并追加 `Connection: close`——因为服务端只解析
 * **第一个**请求的头，keep-alive 连接上的后续请求无法再按前缀路由。浏览器
 * 因此每个请求新建连接（多一次握手）。子域名模式没有这个问题。
 *
 * ## 生命周期
 *
 * `Start()` 返回后两个端口都在监听；`Stop()` 关闭监听、断开所有隧道与
 * 公网连接，并等待工作线程退出。`TunnelServer` 不拥有任何全局状态，可在
 * 同进程内创建多个实例（测试用，端口传 0 自动分配）。
 *
 * @author go2cpp contributors
 * @date 2026-11-02
 * @copyright MIT License
 */

#include "go2cpp/metrics/metrics.hpp"
#include "go2cpp/tunnel/mux.hpp"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace go2cpp {
namespace tunnel {

/// `TunnelServer` 配置。
struct TunnelServerConfig {
    /// 控制端口监听地址。
    std::string control_host = "0.0.0.0";
    /// 控制端口；`0` 表示内核分配（测试用）。
    std::uint16_t control_port = 7443;
    /// 公网 HTTP 端口监听地址。
    std::string public_host = "0.0.0.0";
    /// 公网 HTTP 端口；`0` 表示内核分配。
    std::uint16_t public_port = 8443;
    /// 公网 URL 里展示的 host（VPS 的公网 IP / 域名）；空表示自动探测。
    std::string display_host;
    /// 泛域名后缀，如 `tunnel.example.com`；为空表示只用路径前缀路由。
    std::string domain;
    /// 可选鉴权 token；非空时客户端 `HELLO` 必须携带同值。
    std::string token;
    /// 单隧道并发流上限；超出返回 503。
    std::size_t max_streams_per_tunnel = 256;
    /// 单流接收队列上限（字节）。
    std::size_t recv_queue_cap = 8U * 1024U * 1024U;
    /// 无入站帧多久判定客户端死亡（毫秒）。
    int idle_timeout_ms = 90000;
    /// 心跳间隔（毫秒）。
    int ping_interval_ms = 15000;
    /// 路径前缀模式下是否剥掉前缀；默认 true。
    bool strip_prefix = true;
    /// 是否启用帧尾 CRC（两端必须一致）。
    bool with_checksum = false;
    /// `accept()` 等待切片（毫秒），影响 `Stop()` 的响应速度。
    int accept_timeout_ms = 500;
    /// 优雅关闭时等待在途流结束的上限（毫秒）。
    int drain_timeout_ms = 5000;
    /// per-stream 裸 TCP 模式下等待客户端回拨数据连接的上限（毫秒）。
    int raw_bind_timeout_ms = 10000;
};

/// 一条在线隧道的快照（用于目录页 / 状态查询）。
struct TunnelSnapshot {
    std::string subdomain;     ///< 子域名。
    std::string public_url;    ///< 完整公网 URL。
    std::string peer;          ///< 客户端地址 `ip:port`。
    std::string local_target;  ///< 客户端上报的本地目标（`127.0.0.1:8080`）。
    std::string scheme;        ///< `path` 或 `host`。
    std::size_t streams_active = 0;
    std::uint64_t streams_total = 0;
    std::uint64_t bytes_in = 0;   ///< 公网 → 客户端。
    std::uint64_t bytes_out = 0;  ///< 客户端 → 公网。
    std::int64_t online_ms = 0;   ///< 已在线时长（毫秒）。
    std::size_t data_connections = 0;  ///< 该隧道的数据连接数（控制/数据分离）。
    bool draining = false;             ///< 是否正在优雅关闭。
};

/**
 * @brief 公网隧道服务端。
 *
 * @details 线程：2 条 accept 循环 + 每控制连接 1 条等待线程 + 每公网连接
 *          1 条 RelayPipe 线程（外加 MuxSession 自己的 2 条）。线程数随在线
 *          隧道数和并发请求数增长，见文档「限制」。
 */
class TunnelServer final {
public:
    /// 事件回调（上线 / 下线 / 错误）；在任意工作线程里调用，必须快速返回。
    using EventHandler = std::function<void(const std::string& message)>;

    explicit TunnelServer(TunnelServerConfig config);
    ~TunnelServer();

    TunnelServer(const TunnelServer&) = delete;
    TunnelServer& operator=(const TunnelServer&) = delete;

    /**
     * @brief 绑定两个端口并启动 accept 线程。
     * @param err 失败原因（可为空）。
     * @return 成功 true。
     */
    bool Start(ErrorPtr* err);

    /// 停止服务：关闭监听、断开所有隧道与公网连接、等待工作线程退出。
    void Stop();

    /// 控制端口实际端口。
    std::uint16_t control_port() const noexcept { return m_control_port; }
    /// 公网端口实际端口。
    std::uint16_t public_port() const noexcept { return m_public_port; }

    /// 展示用 host（`display_host` 为空时为自动探测结果）。
    const std::string& display_host() const noexcept { return m_display_host; }

    /// 当前所有在线隧道快照（按子域名排序）。
    std::vector<TunnelSnapshot> tunnels() const;

    /// 某子域名的公网 URL；不在线时返回空串。
    std::string PublicUrlFor(const std::string& subdomain) const;

    /// 设置事件回调。
    void set_event_handler(EventHandler handler);

    /**
     * @brief 进入优雅关闭：给所有会话发 `GOAWAY`，等待在途流结束。
     * @details 期间不再接受新流；超时后由 `Stop()` 强制断开。
     */
    void BeginDrain();

    /// 是否处于 drain 状态。
    bool draining() const noexcept {
        return m_draining.load(std::memory_order_acquire);
    }

    /// 当前鉴权是否启用（配置了 token）。
    bool auth_required() const noexcept { return m_auth_required; }

    /// 累计公网连接数。
    std::uint64_t connections_total() const noexcept {
        return m_connections_total.load(std::memory_order_relaxed);
    }

    /// `--public-host` 为空时的自动探测（UDP connect 取源地址，不发包）。
    static std::string DetectLocalAddress();

private:
    /// 一条在线隧道的内部记录。
    ///
    /// @details `session` 是控制连接；`data_sessions` 是可选的数据连接池
    ///          （`HELLO.flags` 带 `kHelloFlagDataConnection` 的会话）。
    ///          开流时优先在数据连接上轮询，把控制面与数据面隔开。
    /// 一条「等待客户端回拨数据连接」的 raw 流槽位。
    struct RawWaiter {
        std::mutex mutex;
        std::condition_variable cv;
        TcpConn::Ptr conn;   ///< 绑定成功后的数据连接。
        bool bound = false;
    };

    struct Entry {
        std::string subdomain;
        MuxSession::Ptr session;
        std::string peer;
        std::string local_target;
        std::string scheme;
        std::int64_t started_ms = 0;
        std::mutex mutex;                              ///< 保护下面两个容器。
        std::vector<MuxSession::Ptr> data_sessions;    ///< 数据连接池。
        std::map<std::uint32_t, std::shared_ptr<RawWaiter>> raw_waiters;
        /// raw 流并发数：raw 不建 MuxStream，`session->streams_active()` 不增长，
        /// 必须在此单独计数才能与 mux 流共用 `max_streams_per_tunnel` 上限。
        std::atomic<std::size_t> raw_active{0};
        std::atomic<std::size_t> round_robin{0};       ///< 轮询下标。
        std::atomic<bool> draining{false};             ///< 正在 drain。
    };

    void ControlAcceptLoop();
    void PublicAcceptLoop();
    void HandleControl(TcpConn::Ptr conn);
    void HandlePublic(TcpConn::Ptr conn);
    /// per-stream 裸 TCP：发 `kOpenRaw` → 等客户端回拨数据连接 → 双向 splice。
    void ServeRawStream(const TcpConn::Ptr& conn,
                        const std::shared_ptr<Entry>& entry,
                        std::string prefix);
    /// 客户端数据连接绑定到等待中的 raw 流；找不到等待者则关闭连接。
    void BindRawStream(const std::string& subdomain,
                       std::uint32_t stream_id, const TcpConn::Ptr& conn);
    void Emit(const std::string& message) const;

    std::string BuildPublicUrl(const std::string& subdomain,
                               const std::string& scheme) const;
    /// 查表；不存在返回空。
    std::shared_ptr<Entry> Lookup(const std::string& subdomain) const;
    /// 注册（同名旧会话会被踢掉）；返回最终 entry。
    std::shared_ptr<Entry> Register(const std::string& subdomain,
                                    const MuxSession::Ptr& session,
                                    const std::string& peer,
                                    const std::string& local_target,
                                    const std::string& scheme);
    void Unregister(const std::string& subdomain, const MuxSession* session);

    /// 选一条会话开流：优先数据连接池，否则回落到控制连接。
    MuxSession::Ptr PickSession(const std::shared_ptr<Entry>& entry);
    /**
     * @brief 为一条 raw 流预留并发额度——raw 流不建 MuxStream，必须单独计数。
     *
     * @param entry [in] 目标隧道记录；可为空（为空时直接失败）。所有权不转移，
     *              调用期间由 shared_ptr 保证存活。
     * @return
     * - true：额度已预留，调用方**必须**在流结束时对 `entry->raw_active`
     *   执行 `fetch_sub(1)`（`ServeRawStream` 用 RAII 租约完成）；
     * - false：已达 `max_streams_per_tunnel`（mux 流 + raw 流合计），调用方
     *   应返回 503，**不要**继续建流。
     *
     * ## 定位
     * raw 流的唯一并发闸门。少了它，raw 路径绕过 `streams_active()`，单隧道
     * 可以无限并发裸 TCP 流，把服务端的 fd 与工作线程打满。
     *
     * ## 并发/失败语义
     * - 调用约束：任意线程；内部用 CAS 循环预留，不需要外部锁。
     * - 原子性：raw 计数精确不超限；与 mux 流数之和是瞬时快照，并发建 mux
     *   流时可能短暂超出 1 条（与原有 `streams_active()` 检查同级）。
     * - 失败语义：不抛异常；返回 false 时计数未改变。
     * - 不变量：返回 true 后 `raw_active` 至少加 1，直到调用方归还。
     */
    bool TryAcquireRawSlot(const std::shared_ptr<Entry>& entry);
    /// 子域名是否在最近 5 分钟内上线过（用于把 404 变成更明确的 502）。
    bool WasRecentlyOffline(const std::string& subdomain) const;
    /// 生成 `/__tunnel__/status` 的 JSON。
    std::string BuildStatusJson() const;
    /// 注册 metrics（进程级 Registry，重复构造同进程不会重复注册）。
    void InitMetrics();

    /// 工作线程记账：`Stop()` 需要等它们全部退出后才能析构。
    void Spawn(std::function<void()> task);
    bool WaitWorkers(int timeout_ms);

    TunnelServerConfig m_config;
    std::string m_display_host;

    TcpListener::Ptr m_control_listener;
    TcpListener::Ptr m_public_listener;
    std::uint16_t m_control_port = 0;
    std::uint16_t m_public_port = 0;

    std::atomic<bool> m_running{false};
    std::thread m_control_thread;
    std::thread m_public_thread;

    mutable std::mutex m_tunnels_mutex;
    std::unordered_map<std::string, std::shared_ptr<Entry>> m_tunnels;
    /// 最近下线墓碑：subdomain -> 下线时刻（毫秒）。让刚掉线的隧道路由
    /// 返回 502（"tunnel offline"）而不是 404。
    mutable std::unordered_map<std::string, std::int64_t> m_offline;

    mutable std::mutex m_event_mutex;
    EventHandler m_event_handler;

    std::atomic<std::uint64_t> m_connections_total{0};
    std::atomic<bool> m_draining{false};

    /// 鉴权：token -> 32B PSK（复用 Bento 的 PSK 派生）。
    Psk m_psk{};
    bool m_auth_required = false;

    // 可观测：进程级 metrics（go2cpp::metrics::Registry::Default()）。
    go2cpp::metrics::Counter* m_metric_connections = nullptr;
    go2cpp::metrics::Counter* m_metric_streams = nullptr;
    go2cpp::metrics::Counter* m_metric_auth_failures = nullptr;
    go2cpp::metrics::Counter* m_metric_bytes_in = nullptr;
    go2cpp::metrics::Counter* m_metric_bytes_out = nullptr;
    go2cpp::metrics::Gauge* m_metric_online = nullptr;

    /// 工作线程共享记账块（detached 线程只经 `shared_ptr` 持有它）。
    ///
    /// @details 为什么不用裸成员：`Stop()` 的等待是**带超时**的，超时后对象
    ///          仍会被析构。若 worker 退出时直接访问成员 mutex/cv，一旦
    ///          "计数先归零、析构先发生"而线程随后才 `notify_all`，就是
    ///          condvar use-after-destroy（TSan 实测 P1）。把记账块放进
    ///          `shared_ptr` 后，worker 的最后一次访问落在仍存活的对象上，
    ///          与析构彻底解耦。
    struct WorkerRegistry {
        std::mutex mutex;              ///< 保护下面三个字段。
        std::condition_variable cv;    ///< 计数归零时唤醒 `WaitWorkers`。
        std::size_t active = 0;        ///< 尚未退出的 worker 数。
        bool stopping = false;         ///< `Stop()` 已开始，拒绝再起线程。
    };

    /// 所有 worker 共享同一个记账块；`shared_ptr` 保证线程退出时它一定存活。
    std::shared_ptr<WorkerRegistry> m_workers =
        std::make_shared<WorkerRegistry>();
};

}  // namespace tunnel
}  // namespace go2cpp
