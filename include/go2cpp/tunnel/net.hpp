#pragma once

/**
 * @file net.hpp
 * @brief 隧道模块的最小 TCP 层 + `relay::Stream` 适配器。
 *
 * @details 公网穿透要做成**可独立部署的两个可执行程序**（`tunnel-server` 跑在
 *          公网 VPS、`tunnel-client` 跑在 NAT 后），因此这里刻意只用
 *          POSIX socket + `poll(2)`，**不链接 `go2cpp_network`**：
 *
 *          - `go2cpp::net::Socket` 的等待路径与 IOManager / Fiber / hook 耦合，
 *            放进独立守护进程会连带运行时初始化与 `LD_PRELOAD` 语义；
 *          - RelayPipe 对 `Stream::Close()` 的要求是「**任意线程**调用后必须
 *            尽快打断阻塞中的 `Read`/`Write`」，这里用 `shutdown(2)` 直接满足，
 *            不依赖 Fiber 取消代际。
 *
 *          与 `src/p2p/` 的取舍一致：独立工具零运行时依赖，行为可预测。
 *
 * ## 关闭语义（RelayPipe 的硬契约）
 *
 * - `TcpConn::Shutdown()` 幂等、可在任意线程调用；内部只做 `shutdown(SHUT_RDWR)`
 *   并置关闭标记，**不 `close(fd)`**——fd 由析构函数在最后释放，避免「关闭后
 *   fd 被复用、读线程仍握着旧 fd」的 ABA 竞态。
 * - 阻塞在 `poll` 的读线程会被 `shutdown` 唤醒（返回 `POLLHUP`，`recv` 得 0）。
 * - 析构前调用方必须保证已没有线程在使用该 `TcpConn`（RelayPipe 返回即满足）。
 *
 * @author go2cpp contributors
 * @date 2026-11-02
 * @copyright MIT License
 */

#include "go2cpp/relay/relay_pipe.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace go2cpp {
namespace tunnel {

/// 一次读操作的分类结果。
struct IoResult {
    std::size_t count = 0;  ///< 读到的字节数（`eof` 时为 0）。
    int error = 0;          ///< errno；0 表示成功（含 `eof`）。
    bool eof = false;       ///< 对端已关闭（FIN 或 RST）。

    bool ok() const noexcept { return error == 0 && !eof; }
};

/**
 * @brief 一条已建立的 TCP 连接（非阻塞 fd + `poll` 等待）。
 *
 * @details 线程模型：`Read` / `WriteAll` / `Shutdown` 可由不同线程并发调用；
 *          写侧用互斥量串行化，保证每次 `WriteAll` 的字节不交错。
 */
class TcpConn final {
public:
    using Ptr = std::shared_ptr<TcpConn>;

    /**
     * @brief 主动连接 `host:port`。
     * @param host 主机名或点分 IP。
     * @param port 端口。
     * @param timeout_ms 连接超时（毫秒）。
     * @param err 失败原因（可为空）。
     * @return 成功返回连接对象；失败返回空指针。
     */
    static Ptr Dial(const std::string& host, std::uint16_t port, int timeout_ms,
                    std::string* err);

    /// 从已 accept 的 fd 接管（设置非阻塞 / TCP_NODELAY / KEEPALIVE）。
    static Ptr FromFd(int fd);

    ~TcpConn();

    TcpConn(const TcpConn&) = delete;
    TcpConn& operator=(const TcpConn&) = delete;

    /**
     * @brief 读一段数据。
     * @param data 目标缓冲。
     * @param length 最多读多少字节。
     * @param timeout_ms 最多等多久（毫秒）；`<0` 表示无限等（收盘时由
     *        `Shutdown()` 打断）。
     * @return `ok()` 为真表示读到字节；`eof` 为真表示对端关闭；
     *         否则 `error` 为 errno（`ETIMEDOUT` 超时、`ECANCELED` 已关闭）。
     */
    IoResult Read(void* data, std::size_t length, int timeout_ms);

    /**
     * @brief 写满 `length` 字节。
     * @param data 数据首地址。
     * @param length 字节数。
     * @param timeout_ms 单次写等待上限（毫秒），整体也受它约束。
     * @param err 失败原因（可为空）。
     * @return 全部写完返回 true。
     */
    bool WriteAll(const void* data, std::size_t length, int timeout_ms = 30000,
                  std::string* err = nullptr);

    /// 关闭读/写方向（幂等、可跨线程）；不释放 fd。
    void Shutdown() noexcept;

    /// 是否已调用过 `Shutdown()`（或对端关闭导致内部标记）。
    bool closed() const noexcept { return m_closed.load(std::memory_order_acquire); }

    /// 底层 fd（仅诊断；关闭后不可再用）。
    int fd() const noexcept { return m_fd; }

    /// 对端地址文本 `ip:port`；取不到时返回空串。
    std::string PeerText() const;

    /// 本端地址文本 `ip:port`。
    std::string LocalText() const;

    /**
     * @brief 等待 fd 就绪（`poll(2)` 封装，供监听器复用）。
     * @param fd 文件描述符。
     * @param events `POLLIN` / `POLLOUT`。
     * @param timeout_ms 超时；负数表示无限（EINTR 会重试）。
     * @return 0 就绪；`ETIMEDOUT` 超时；其它 errno 失败。
     */
    static int PollFd(int fd, short events, int timeout_ms);

private:
    explicit TcpConn(int fd) : m_fd(fd) {}

    int m_fd = -1;
    std::atomic<bool> m_closed{false};
    std::mutex m_write_mutex;
};

/**
 * @brief TCP 监听器。
 *
 * @details `Accept()` 带超时，返回空表示「本次没有新连接」（超时或错误）；
 *          `Close()` 幂等，会打断阻塞中的 `Accept()`。
 */
class TcpListener final {
public:
    using Ptr = std::shared_ptr<TcpListener>;

    /**
     * @brief 绑定并监听。
     * @param host 监听地址，`0.0.0.0` 表示所有 IPv4 地址。
     * @param port 端口；`0` 表示由内核分配（可用 `BoundPort()` 查询）。
     * @param err 失败原因（可为空）。
     * @param backlog 已完成队列上限。
     * @return 监听器；失败返回空指针。
     */
    static Ptr Listen(const std::string& host, std::uint16_t port,
                      std::string* err, int backlog = 512);

    ~TcpListener();

    TcpListener(const TcpListener&) = delete;
    TcpListener& operator=(const TcpListener&) = delete;

    /// 接受一条连接；超时/关闭返回空。
    TcpConn::Ptr Accept(int timeout_ms);

    /// 关闭监听 fd（幂等、可跨线程）。
    void Close() noexcept;

    /// 实际绑定端口（`port` 传 0 时用于回读内核分配的端口）。
    std::uint16_t BoundPort() const noexcept { return m_port; }

    bool valid() const noexcept { return m_fd >= 0; }

private:
    TcpListener(int fd, std::uint16_t port) : m_fd(fd), m_port(port) {}
    int m_fd = -1;
    std::uint16_t m_port = 0;
    std::atomic<bool> m_closed{false};
};

// ============================================================================
// relay::Stream 适配器
// ============================================================================

/**
 * @brief 把 `TcpConn` 适配成 `relay::Stream`。
 *
 * @details `Read` 在内部按 `read_timeout_ms` 分片等待：超时不算失败，继续等，
 *          直到读到数据、对端关闭或 `Close()` 被调用。这样 RelayPipe 的写
 *          deadline 判定不会被无数据的空闲期误触发（HTTP keep-alive 常见）。
 */
class SocketStream final : public relay::Stream {
public:
    using Ptr = std::shared_ptr<SocketStream>;

    /**
     * @param conn 底层连接（不转移所有权，调用方保证存活）。
     * @param read_timeout_ms 单次等待分片（毫秒），默认 1000。
     */
    explicit SocketStream(TcpConn::Ptr conn, int read_timeout_ms = 1000);

    streams::ReadResult Read(char* data, std::size_t size) override;
    streams::WriteResult Write(const char* data, std::size_t size) override;
    ErrorPtr Close() override;

    const TcpConn::Ptr& conn() const noexcept { return m_conn; }

private:
    TcpConn::Ptr m_conn;
    int m_read_timeout_ms;
};

/**
 * @brief 带「预读字节」的流包装。
 *
 * @details 服务端为了按 `Host` / 路径路由，必须先读掉 HTTP 请求头；这次
 *          `recv` 很可能把请求体也一起读进来。这些字节不能丢，因此先把它们
 *          吐给隧道，再回落到真实连接。
 */
class PrefixedStream final : public relay::Stream {
public:
    /**
     * @param inner 底层流（不转移所有权）。
     * @param prefix 已经读到的字节。
     */
    PrefixedStream(relay::Stream* inner, std::string prefix);

    streams::ReadResult Read(char* data, std::size_t size) override;
    streams::WriteResult Write(const char* data, std::size_t size) override;
    ErrorPtr Close() override;

private:
    relay::Stream* m_inner;
    std::string m_prefix;
    std::size_t m_offset = 0;
};

/**
 * @brief 在一端读到 EOF 时关闭**另一条**流的包装器。
 *
 * @details RelayPipe 的正常 EOF 语义是「不关闭任何端点」（半关闭由调用方决定）。
 *          HTTP 场景需要的是全关闭：浏览器一关连接，隧道流就必须关，否则
 *          客户端另一方向永远等不到数据、RelayPipe 永远不返回。本包装器把
 *          「读到 EOF」翻译成「关闭对端 Stream」，从而让两条方向都能收尾。
 */
class EofClosingStream final : public relay::Stream {
public:
    /**
     * @param inner 本方向读取的流（不转移所有权）。
     * @param peer 读到 EOF 时要关闭的流（不转移所有权）。
     */
    EofClosingStream(relay::Stream* inner, relay::Stream* peer);

    streams::ReadResult Read(char* data, std::size_t size) override;
    streams::WriteResult Write(const char* data, std::size_t size) override;
    ErrorPtr Close() override;

private:
    relay::Stream* m_inner;
    relay::Stream* m_peer;
};

}  // namespace tunnel
}  // namespace go2cpp
