#pragma once

/**
 * @file tcp_proxy.hpp
 * @brief 通用 TCP 回环代理：本地监听 → 连接远端 → 双向字节转发。
 * @details 对应 JinShan 远程桌面方案的「回环中继」场景（见
 *          docs/modules/bento-scenarios.md 场景 4）：主控端把系统生成的
 *          invitation 里的真实地址改写成 127.0.0.1:本地端口，由本组件
 *          在本地接住 Viewer 的连接，再按 1:1 管道转发到真正的对端；
 *          中转/中继只搬运字节，不解析应用协议。
 *
 *          与 JinShan 的差异：JinShan 的回环中继绑定 IRDPSRAPI 的
 *          invitation/Viewer 生命周期，本组件是**与业务无关的通用端口转发
 *          器**，可复用于任何 TCP 协议（RDP、HTTP、自定义 RPC）。
 *
 *          实现要点（为什么这样写）：
 *          - **单线程 poll 驱动**：一条连接两个 fd，连接数是演示/工具级
 *            （默认上限 64），poll 足够；不引入线程池，便于在现有的事件
 *            循环里调用（Poll/RunFor 风格与 Link 一致）。
 *          - **半关闭必须传播**：TCP 是双工，一方写完 FIN 后另一方往往还要
 *            回一大段数据（请求-响应协议）。若读到 EOF 就 close 整个连接，
 *            会把响应截断。这里用 shutdown(SHUT_WR) 把 EOF 传到对端，并在
 *            两个方向都结束后才真正关闭。
 *          - **每方向一个有界缓冲**：读到的数据未写完时留在 buffer 里，
 *            等目标可写再续写；缓冲区满就停止从源读，形成天然背压，
 *            不会因为慢对端把内存吃光。
 *          - **所有超时都有界**：connect 超时、双向空闲超时、半关闭后的
 *            linger 超时，避免半开连接长期占用 fd。
 *
 * @author go2cpp contributors
 * @date 2026-10-27
 * @copyright MIT License
 */

#include <sys/socket.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace go2cpp::bento {

/// @brief 回环代理配置。listen_port 为 0 时由内核分配（测试/多实例场景必需）。
struct TcpProxyConfig {
    /// 监听地址：IPv4/IPv6 字面量或主机名；"0.0.0.0"/"::" 表示通配。
    std::string listen_address = "127.0.0.1";
    std::uint16_t listen_port = 0;
    /// 远端目标地址（IPv4/IPv6/主机名）。Open 时解析一次；DNS 失败直接报错。
    std::string target_address = "127.0.0.1";
    std::uint16_t target_port = 0;
    /// connect() 到目标的超时（毫秒）。超时连接被关闭并计入 dial_failed。
    std::uint32_t connect_timeout_ms = 3000;
    /// 双向都没有字节流动的空闲超时（毫秒）；0 = 不限制。
    std::uint32_t idle_timeout_ms = 30000;
    /// 一侧半关闭后，等待另一侧结束的最长时间（毫秒）；0 = 不限制。
    std::uint32_t half_close_timeout_ms = 5000;
    /// 每个方向的转发缓冲字节数（也是单次 read 上限）。
    std::size_t buffer_bytes = 64 * 1024;
    /// 同时存在的连接上限；超过后新 accept 的连接被立即关闭并计入 rejected。
    std::size_t max_connections = 64;
    /// listen(2) 的 backlog。
    int backlog = 64;
};

/// @brief 代理计数器。字段语义与 JinShan AX-014/AX-028 的可观测要求对齐：
///        任何拒绝/超时都必须有独立计数，不能只有一句日志。
struct TcpProxyStats {
    std::uint64_t accepted = 0;      ///< accept 成功的连接数。
    std::uint64_t rejected = 0;      ///< 超过 max_connections 被拒的连接数。
    std::uint64_t dial_ok = 0;       ///< 成功连上目标的连接数。
    std::uint64_t dial_failed = 0;   ///< 连接目标失败/超时的连接数。
    std::uint64_t active = 0;        ///< 当前处于转发状态的连接数。
    std::uint64_t closed = 0;        ///< 已关闭（含正常与异常）的连接数。
    std::uint64_t bytes_to_target = 0;  ///< 客户端 → 目标方向累计字节。
    std::uint64_t bytes_to_client = 0;  ///< 目标 → 客户端方向累计字节。
    std::uint64_t half_closes = 0;      ///< 传播过的 FIN 次数（单向计一次）。
    std::uint64_t idle_timeouts = 0;    ///< 空闲超时关闭数。
    std::uint64_t linger_timeouts = 0;  ///< 半关闭后 linger 超时强关数。
    std::uint64_t connect_timeouts = 0; ///< connect 目标超时数。
    std::uint64_t resets = 0;          ///< 读写错误（ECONNRESET 等）导致的中断数。
};

/// @brief 通用 TCP 回环代理（单线程 poll 驱动，非阻塞）。
/// @note 线程模型：Open/Poll/RunFor/Close 由同一线程调用；回调在该线程内
///       同步执行。不要在回调里再次调用 Poll（会重入连接表）。
class TcpProxy final {
   public:
    explicit TcpProxy(TcpProxyConfig config);
    ~TcpProxy();

    TcpProxy(const TcpProxy&) = delete;
    TcpProxy& operator=(const TcpProxy&) = delete;

    /// @brief 创建监听 socket 并解析目标地址（只解析，不连接）。
    /// @param error 失败原因（可为 nullptr）。
    bool Open(std::string* error = nullptr);

    /// @brief 关闭监听与全部连接（幂等）。
    void Close();

    /// @brief 处理一轮 accept + 转发 + 超时检查。返回本轮搬运的字节数。
    /// @param timeout_ms poll 的等待上限；0 = 只轮询不等待。
    int Poll(int timeout_ms);

    /// @brief 循环 Poll 指定时长。
    void RunFor(int duration_ms);

    /// @brief 实际监听端口（listen_port 为 0 时由内核分配）。
    std::uint16_t local_port() const noexcept {
        return m_local_port;
    }
    bool is_open() const noexcept {
        return m_listen_fd >= 0;
    }
    std::size_t connection_count() const noexcept {
        return m_connections.size();
    }
    /// @brief 计数器快照（线程内调用，不加锁）。
    TcpProxyStats Stats() const;

    // ---- 回调（Poll 内同步触发；只用于观测，不要在里面重入 Poll） ----
    /// 新连接已接入（尚未连目标）。
    std::function<void(std::uint64_t connection_id)> on_accept;
    /// 已连上目标，开始转发。
    std::function<void(std::uint64_t connection_id)> on_connected;
    /// 连接关闭；reason 为静态字符串（"eof"/"reset"/"idle"/"linger"/"dial"）。
    std::function<void(std::uint64_t connection_id, const char* reason)> on_closed;

   private:
    /// 一条转发管道：client_fd（accept 得到）+ target_fd（connect 得到）。
    struct Connection;
    /// 关闭并移除一条连接；reason 决定计入哪个计数器。
    void CloseConnection(std::size_t index, const char* reason);
    /// 接受所有待处理连接（非阻塞）。
    void AcceptReady();
    /// 推进一条连接的状态机；返回本轮搬运字节数。
    int ServiceConnection(std::size_t index, std::uint64_t now_ms);
    /// 把 src 的数据搬给 dst（先写 pending 再读），返回写入字节数。
    /// @return >=0 搬运字节数；-1 表示遇到不可恢复的读写错误。
    int PumpDirection(int src_fd, int dst_fd, std::uint8_t* buffer, std::size_t capacity,
                      std::size_t* pending_bytes, std::size_t* pending_offset, bool* eof);
    /// 统一设置非阻塞 + CLOEXEC。
    static bool SetNonBlocking(int fd) noexcept;

    TcpProxyConfig m_config;
    int m_listen_fd = -1;
    std::uint16_t m_local_port = 0;
    std::uint64_t m_next_connection_id = 1;
    std::vector<std::unique_ptr<Connection>> m_connections;
    /// 目标地址在 Open 时解析并缓存，accept 时不再阻塞在 DNS 上。
    sockaddr_storage m_target_addr{};
    socklen_t m_target_addr_len = 0;
    TcpProxyStats m_stats;
};

}  // namespace go2cpp::bento
