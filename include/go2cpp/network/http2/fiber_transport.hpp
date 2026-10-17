/**
 * @file fiber_transport.hpp
 * @brief 用本项目的 Socket + Fiber 实现 `Transport`。
 *
 * ## 这一层做了什么
 *
 * 就两件事：把 `Transport::Read` 映射到 `Socket::recv`，把 `Write` 映射到
 * `Socket::send`。**连接层的协议逻辑一行不改** —— 这正是把 IO 抽象出来的收益。
 *
 * `Socket::recv/send` 内部已经接了本项目的 hook 与 `IOManager`：在 Fiber 里
 * 调用会挂起当前协程、等 IO 就绪后恢复，所以本类是**同步写法、协程调度**。
 *
 * ## 为什么读要返回 0 而不是抛异常
 *
 * `Transport::Read` 的契约是"返回 0 表示对端关闭"。`Socket::recv` 在
 * 对端关闭时返回 `transferred == 0`、出错时 `ok() == false` —— 两种情况对
 * HTTP/2 连接层而言都是"读不到更多数据了"，统一返回 0 让上层报 `kClosed`。
 * 错误细节保留在 `last_error()` 里供诊断。
 */
#pragma once

#include "go2cpp/network/http2/transport.hpp"
#include "go2cpp/network/socket.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace go2cpp::net::http2 {

/**
 * @brief 基于 `Socket` 的 Transport 实现（在 Fiber 中阻塞、由 IOManager 调度）。
 * @note 本类**不是线程安全的**：一个 `Transport` 只应由一个 Fiber 使用，
 *       与 `Connection` 的单驱动者约束一致。
 */
class FiberTransport final : public Transport {
public:
    using Duration = std::chrono::milliseconds;

    /**
     * @param socket 已连接的 socket（所有权转入）。
     * @param timeout 单次读写的超时；`Duration::max()` 表示不超时。
     * @note 超时后 `Read`/`Write` 返回 0，上层会把连接判为关闭。生产环境
     *       通常给一个较大的值（如 30s），避免半开连接永久占住 Fiber。
     */
    explicit FiberTransport(Socket::Ptr socket, Duration timeout = Duration::max())
        : m_socket(std::move(socket)), m_timeout(timeout) {}

    std::size_t Read(std::uint8_t* buffer, std::size_t capacity) override {
        if (!m_socket || buffer == nullptr || capacity == 0) {
            return 0;
        }
        const auto result = m_socket->recv(buffer, capacity, 0, m_timeout);
        if (!result.ok() || result.transferred <= 0) {
            m_last_error = result.error;
            if (!result.ok()) {
                m_failed = true;
            }
            return 0;
        }
        m_last_error = 0;
        return static_cast<std::size_t>(result.transferred);
    }

    std::size_t Write(const std::uint8_t* data, std::size_t length) override {
        if (!m_socket || data == nullptr || length == 0) {
            return 0;
        }
        // 循环写直到写完：TCP 的 send 可能只写出一部分。
        std::size_t total = 0;
        while (total < length) {
            const auto result = m_socket->send(data + total, length - total, 0, m_timeout);
            if (!result.ok() || result.transferred <= 0) {
                m_last_error = result.error;
                m_failed = true;
                break;
            }
            total += static_cast<std::size_t>(result.transferred);
        }
        return total;
    }

    bool Closed() const noexcept override {
        return !m_socket || !m_socket->valid() || m_failed;
    }

    /** @brief 最近一次读写的 errno（0 表示成功）。 */
    int last_error() const noexcept { return m_last_error; }
    /** @brief 底层 socket（便于诊断或转移所有权）。 */
    const Socket::Ptr& socket() const noexcept { return m_socket; }

private:
    Socket::Ptr m_socket;
    Duration m_timeout{Duration::max()};
    int m_last_error{0};
    /// 写失败后标记关闭，避免上层反复重试同一个坏连接。
    bool m_failed{false};
};

}  // namespace go2cpp::net::http2
