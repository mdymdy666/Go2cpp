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
 *
 * ## 本文件定义什么
 * - `FiberTransport`：`Transport` 的生产实现，把 `Read`/`Write` 映射到
 *   `Socket::recv/send`（内部经 hook + `IOManager`，阻塞即协程挂起）。
 *
 * ## 阅读建议
 * 先看 `Read`/`Write` 如何处理短写与超时，再看 `Closed()` 为什么把 `m_failed`
 * 也算进去；最后对照 `transport.hpp` 里的三条契约。
 *
 * ## 边界与陷阱
 * - `Write` 用循环处理 partial write：TCP 的 `send` 只保证写出一部分；
 * - 超时、取消、对端关闭都收敛为「返回 0」，细节只在 `last_error()`；
 * - 非线程安全：一个 Transport 只应由一个 Fiber 使用（与 `Connection` 一致）。
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
 *
 * ## 定位
 * 被 `http2::Connection` 以 `Transport*` 注入；由上层客户端、示例与测试在
 * `IOManager` 的 Fiber 里创建。与 `TlsTransport` 是并列的生产实现。
 *
 * ## 不变量
 * - `Read` 返回 0 后该连接即视为不可用（EOF 或 `m_failed`），上层不会复用；
 * - `Write` 只有在全部字节写完时才返回 `length`，否则返回已写字节数；
 * - `Closed()` 同时覆盖「socket 无效」与「曾经写失败」，避免反复重试坏连接。
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

    /**
     * @brief 读取最多 capacity 字节（在 Fiber 中阻塞、由 IOManager 调度）。
     *
     * ## 步骤
     * step1. 无 socket / 空指针 / 零容量 -> 返回 0；
     * step2. 调 `Socket::recv`（`m_timeout` 到期即返回，不无限挂起）；
     * step3. 失败或读到 0 -> 记 `m_last_error`，真错误置 `m_failed`，统一返回 0；
     * step4. 成功 -> 清 `m_last_error` 并返回实际字节数。
     */
    std::size_t Read(std::uint8_t* buffer, std::size_t capacity) override {
        // step1: 参数/连接不可用直接返回 0。
        if (!m_socket || buffer == nullptr || capacity == 0) {
            return 0;
        }
        // step2: 同步调用，Fiber 中会在此挂起直到可读或超时。
        const auto result = m_socket->recv(buffer, capacity, 0, m_timeout);
        // step3: 失败与 EOF 都归一到返回 0（上层据此判 kClosed）。
        if (!result.ok() || result.transferred <= 0) {
            m_last_error = result.error;
            if (!result.ok()) {
                m_failed = true;
            }
            return 0;
        }
        // step4: 成功路径清错误码。
        m_last_error = 0;
        return static_cast<std::size_t>(result.transferred);
    }

    /**
     * @brief 写入全部 length 字节（循环处理短写）。
     *
     * ## 步骤
     * step1. 无 socket / 空指针 / 零长度 -> 返回 0；
     * step2. 循环 `Socket::send` 直到写满：TCP 可能一次只写出一部分；
     * step3. 任一次失败 -> 记 errno、置 `m_failed` 并跳出，返回已写字节数；
     * step4. 返回 `total`（正常情况下等于 `length`）。
     */
    std::size_t Write(const std::uint8_t* data, std::size_t length) override {
        if (!m_socket || data == nullptr || length == 0) {
            return 0;
        }
        // 循环写直到写完：TCP 的 send 可能只写出一部分。
        std::size_t total = 0;
        // step2: 循环 send 直到写满（TCP 允许短写）。
        while (total < length) {
            // step2: 每次从 total 偏移继续写。
            const auto result = m_socket->send(data + total, length - total, 0, m_timeout);
            // step3: 写失败即标记连接不可用并停止重试。
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
    /// 底层已连接 socket（所有权在本类）；Read/Write/Closed 都读它。
    Socket::Ptr m_socket;
    /// 单次读写的等待上限；构造时固定，Read/Write 透传给 recv/send。
    Duration m_timeout{Duration::max()};
    /// 最近一次 errno；Read/Write 写，last_error() 读（0 表示成功）。
    int m_last_error{0};
    /// 写失败后标记关闭，避免上层反复重试同一个坏连接。
    bool m_failed{false};
};

}  // namespace go2cpp::net::http2
