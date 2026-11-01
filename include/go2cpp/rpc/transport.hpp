#pragma once

/**
 * @file transport.hpp
 * @brief 传输抽象 + TCP 实现（Linux/POSIX 阻塞 socket）。
 *
 * @details
 * RPC 的 Server/Client 只依赖 `Transport` 的四个动作：写全部、读一段、关闭、
 * 拿 fd。这样同一套 RPC 语义可以跑在 TCP 上，也可以跑在 WebSocket 上
 * （见 `go2cpp/rpc/websocket_transport.hpp`）——后者把一条 WebSocket 二进制
 * 消息当作一段字节流，内部做重组。
 *
 * ## 为什么不用 `go2cpp::net::Socket`
 *
 * `net::Socket` 绑定 IOManager/Fiber 调度器，链接面是整个 runtime；RPC 的
 * 数据面只需要「阻塞 send/recv + poll 超时」，裸 syscall 的路径更短、依赖
 * 更小（`go2cpp_rpc` 只链 `go2cpp_frame` + `go2cpp_reflect`）。需要接入
 * 项目网络栈时，实现一个 `Transport` 适配器即可，不需要改 RPC 核心。
 *
 * ## 延迟相关的默认设置
 *
 * - `TCP_NODELAY`：默认开启。小请求-响应在 Nagle + delayed ACK 下会出现
 *   40 ms 级停顿，这是 RPC 延迟最大的单点风险；
 * - `SO_REUSEADDR`：监听端开启，便于测试反复 bind 同一端口；
 * - 连接建立用非阻塞 connect + poll，超时可控；
 * - 读路径不做额外缓冲：`Read` 直接 recv 到调用方缓冲。
 *
 * ## 生命周期
 *
 * `TcpTransport::Close()` 只 `shutdown` 不 `close`：它负责唤醒阻塞中的读线程，
 * 真正的 fd 释放在析构里做。这样「读线程退出 → Connection 析构 → close」
 * 的顺序不会让 fd 在别的线程仍读时被复用。
 *
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "go2cpp/rpc/status.hpp"

namespace go2cpp::rpc {

/**
 * @brief 监听/连接地址。
 *
 * 支持 `":8080"`（监听任意地址 / 连接 127.0.0.1）、`"127.0.0.1:8080"`、
 * `"localhost:8080"`。IPv6 字面量（`[::1]:port`）暂不支持，解析返回 false。
 */
struct Endpoint {
    std::string host;     ///< 主机名或 IPv4 字面量；空表示任意/回环。
    std::uint16_t port = 0;  ///< 端口；0 表示由内核分配（仅监听端）。

    /// @brief 解析 `"host:port"`；失败返回 false。
    static bool Parse(std::string_view text, Endpoint* out);

    /// @brief 规范化文本（`"host:port"`，空 host 输出 `":port"`）。
    std::string ToString() const;
};

/**
 * @brief 字节流传输接口。
 *
 * @details 实现必须满足：
 * - `WriteAll` 要么写完全部字节，要么返回失败（不允许部分写入后返回成功）；
 * - `Read` 阻塞到至少 1 字节、EOF 或错误；返回 0 表示对端关闭；
 * - `Close` 可被其他线程调用以唤醒阻塞中的 `Read`/`WriteAll`，且幂等；
 * - `Close` 之后 `WriteAll` 必须快速失败（不阻塞）。
 */
class Transport {
   public:
    virtual ~Transport() = default;

    /**
     * @brief 写完全部字节。
     * @param data 数据首地址（`size == 0` 时可为 nullptr）。
     * @param size 字节数。
     * @param status 失败状态（可为 nullptr）。
     * @return 成功 true。
     */
    virtual bool WriteAll(const void* data, std::size_t size, Status* status) = 0;

    /**
     * @brief 读一段字节（阻塞）。
     * @param data 目标缓冲。
     * @param size 缓冲容量。
     * @param status 失败状态（可为 nullptr）。
     * @return 实际字节数；0 表示对端关闭；-1 表示错误。
     */
    virtual std::ptrdiff_t Read(void* data, std::size_t size, Status* status) = 0;

    /// @brief 唤醒并标记关闭（幂等）。
    virtual void Close() noexcept = 0;

    /// @brief 底层 fd（诊断用；非 socket 实现返回 -1）。
    virtual int fd() const noexcept { return -1; }
};

/**
 * @brief TCP 传输（POSIX socket，阻塞 + poll 超时）。
 */
class TcpTransport final : public Transport {
   public:
    /**
     * @brief 建立到远端的连接。
     * @param endpoint 目标地址（host 为空时连 127.0.0.1）。
     * @param timeout 连接超时。
     * @param status 失败状态。
     * @return 成功返回对象；失败返回 nullptr。
     */
    static std::unique_ptr<TcpTransport> Connect(const Endpoint& endpoint,
                                                 std::chrono::milliseconds timeout,
                                                 Status* status);

    /**
     * @brief 接管一个已连接的 fd（accept 之后）。
     * @param fd 已连接 socket；本类拥有它。
     */
    explicit TcpTransport(int fd) noexcept;

    ~TcpTransport() override;
    TcpTransport(const TcpTransport&) = delete;
    TcpTransport& operator=(const TcpTransport&) = delete;

    bool WriteAll(const void* data, std::size_t size, Status* status) override;
    std::ptrdiff_t Read(void* data, std::size_t size, Status* status) override;
    void Close() noexcept override;
    int fd() const noexcept override { return m_fd; }

    /// @brief 是否已关闭。
    bool closed() const noexcept { return m_closed.load(std::memory_order_acquire); }

   private:
    int m_fd{-1};                          ///< socket fd；-1 表示无效。
    std::atomic<bool> m_closed{false};     ///< 关闭标志（Close 后写路径快速失败）。
};

/**
 * @brief TCP 监听器。
 */
class TcpListener {
   public:
    /**
     * @brief 绑定并监听。
     * @param endpoint 地址（host 为空表示 0.0.0.0）。
     * @param backlog listen backlog。
     * @param status 失败状态。
     * @return 成功返回对象；失败返回 nullptr。
     */
    static std::unique_ptr<TcpListener> Bind(const Endpoint& endpoint, int backlog,
                                             Status* status);

    ~TcpListener();
    TcpListener(const TcpListener&) = delete;
    TcpListener& operator=(const TcpListener&) = delete;

    /**
     * @brief 接受一个连接（最多等 `timeout_ms` 毫秒）。
     * @param timeout_ms 超时；< 0 表示无限等待。
     * @param status 失败状态；超时返回 nullptr 且 `status` 为 kOk。
     * @return 新连接；超时/关闭返回 nullptr。
     */
    std::unique_ptr<TcpTransport> Accept(int timeout_ms, Status* status);

    /// @brief 实际监听端口（`Bind` 传 0 时由内核分配）。
    std::uint16_t port() const noexcept { return m_port; }

    /// @brief 监听 fd。
    int fd() const noexcept { return m_fd; }

    /// @brief 是否有效。
    bool valid() const noexcept { return m_fd >= 0; }

    /// @brief 关闭监听（可从其他线程调用以唤醒 `Accept`）。
    void Close() noexcept;

   private:
    TcpListener(int fd, std::uint16_t port) noexcept : m_fd(fd), m_port(port) {}

    int m_fd{-1};                        ///< 监听 fd。
    std::uint16_t m_port{0};             ///< 实际端口。
    std::atomic<bool> m_closed{false};   ///< 关闭标志。
};

/// @brief 把 `errno` 转成 `kUnavailable` 状态文本。
Status SocketErrorStatus(const char* action, int error_number);

}  // namespace go2cpp::rpc
