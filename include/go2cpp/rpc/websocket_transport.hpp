#pragma once

/**
 * @file websocket_transport.hpp
 * @brief 把项目已有的 `net::WebSocket` 接入 RPC：每条 WebSocket 二进制消息承载一段字节流。
 *
 * @details
 * RPC 的 `Transport` 接口是**字节流**（`WriteAll` / `Read`），而 WebSocket 是
 * **消息流**。适配规则很直接：
 *
 * - `WriteAll(data, size)`：把这段字节作为**一条**二进制消息发出去。上层
 *   （`Client` / `ServerConnection`）每次总是写一整帧，因此一条消息 = 一帧；
 * - `Read(buf, size)`：优先从当前消息的剩余字节拷贝；当前消息读空后再
 *   `read_message()` 取下一整条消息并缓存。这样帧层看到的仍然是连续字节流，
 *   `FrameCodec` 的半包/粘包逻辑不需要改动；
 * - `Close()`：发 Close 帧并标记关闭，唤醒阻塞中的读。
 *
 * ## 用法
 *
 * 服务端（已完成 HTTP 升级后）：
 * @code
 *   auto ws = net::WebSocket::Accept(socket, upgrade_request);
 *   server.AddConnection(std::make_unique<rpc::WebSocketTransport>(ws));
 * @endcode
 *
 * 客户端：
 * @code
 *   auto ws = net::WebSocket::Connect(socket, host, "/rpc");
 *   rpc::Client client(std::make_unique<rpc::WebSocketTransport>(ws));
 * @endcode
 *
 * ## 边界
 *
 * - 本适配器不负责 HTTP 握手、路由、鉴权；这些仍由 `net::WebSocket` 与上层完成；
 * - 文本消息会被当作字节流接受（服务端/客户端都应发二进制，见 `WriteAll`）；
 * - 单条消息上限由 `WebSocketConfig::max_message_payload` 决定，默认 64 MiB，
 *   大于 RPC 默认帧上限（4 MiB），因此不会成为瓶颈；
 * - `Read` 只允许一个线程调用（RPC 每条连接只有一个读线程），`WriteAll` 可多线程
 *   （WebSocket 内部写锁串行化）。
 *
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

#include "go2cpp/network/websocket.hpp"
#include "go2cpp/rpc/status.hpp"
#include "go2cpp/rpc/transport.hpp"

namespace go2cpp::rpc {

/**
 * @brief 基于 `net::WebSocket` 的 RPC 传输。
 */
class WebSocketTransport final : public Transport {
   public:
    /**
     * @brief 接管一条已完成握手的 WebSocket。
     * @param socket WebSocket 对象（所有权共享）。
     */
    explicit WebSocketTransport(net::WebSocket::Ptr socket) : m_socket(std::move(socket)) {}

    ~WebSocketTransport() override { Close(); }

    WebSocketTransport(const WebSocketTransport&) = delete;
    WebSocketTransport& operator=(const WebSocketTransport&) = delete;

    /**
     * @brief 把一段字节作为一条二进制消息发送。
     * @param data 数据（`size == 0` 时发空消息）。
     * @param size 字节数。
     * @param status 失败状态。
     * @return 成功 true。
     */
    bool WriteAll(const void* data, std::size_t size, Status* status) override {
        if (m_closed.load(std::memory_order_acquire)) {
            if (status != nullptr) {
                *status = Status(StatusCode::kUnavailable, "websocket transport closed");
            }
            return false;
        }
        if (m_socket == nullptr) {
            if (status != nullptr) {
                *status = Status(StatusCode::kFailedPrecondition, "null websocket");
            }
            return false;
        }
        const char* bytes = size == 0 ? "" : static_cast<const char*>(data);
        const net::WebSocketResult result =
            m_socket->send_binary(std::string_view(bytes, size));
        if (!result.ok()) {
            if (status != nullptr) {
                *status = Status(StatusCode::kUnavailable,
                                 "websocket send: " + result.message);
            }
            return false;
        }
        if (status != nullptr) {
            *status = Status::Ok();
        }
        return true;
    }

    /**
     * @brief 从当前消息剩余字节读取；不足时取下一条 WebSocket 消息。
     * @param data 目标缓冲。
     * @param size 容量。
     * @param status 失败状态。
     * @return 实际字节数；0 表示对端关闭；-1 表示错误。
     */
    std::ptrdiff_t Read(void* data, std::size_t size, Status* status) override {
        if (size == 0) {
            if (status != nullptr) {
                *status = Status::Ok();
            }
            return 0;
        }
        while (m_offset >= m_pending.size()) {
            if (m_closed.load(std::memory_order_acquire) || m_socket == nullptr) {
                if (status != nullptr) {
                    *status = Status(StatusCode::kUnavailable, "websocket transport closed");
                }
                return 0;
            }
            net::WebSocketMessage message;
            const net::WebSocketResult result = m_socket->read_message(message);
            if (!result.ok()) {
                // 对端正常关闭：read_message 可能返回 Closed。
                if (status != nullptr) {
                    *status = (result.error == net::WebSocketError::Closed)
                                  ? Status::Ok()
                                  : Status(StatusCode::kUnavailable,
                                           "websocket read: " + result.message);
                }
                return result.error == net::WebSocketError::Closed ? 0 : -1;
            }
            m_pending = std::move(message.payload);
            m_offset = 0;
            if (m_pending.empty()) {
                continue;  // 空消息：跳过，继续等下一条。
            }
        }
        const std::size_t available = m_pending.size() - m_offset;
        const std::size_t copied = available < size ? available : size;
        std::memcpy(data, m_pending.data() + m_offset, copied);
        m_offset += copied;
        if (m_offset >= m_pending.size()) {
            m_pending.clear();
            m_offset = 0;
        }
        if (status != nullptr) {
            *status = Status::Ok();
        }
        return static_cast<std::ptrdiff_t>(copied);
    }

    /// @brief 关闭（发 Close 帧并唤醒读）。
    void Close() noexcept override {
        if (m_closed.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        if (m_socket != nullptr) {
            (void)m_socket->close(1000, "rpc shutdown");
        }
    }

    /// @brief 底层 socket fd（诊断用）。
    int fd() const noexcept override {
        if (m_socket == nullptr) {
            return -1;
        }
        const net::Socket::Ptr socket = m_socket->socket();
        return socket != nullptr ? socket->fd() : -1;
    }

    /// @brief 是否已关闭。
    bool closed() const noexcept { return m_closed.load(std::memory_order_acquire); }

   private:
    net::WebSocket::Ptr m_socket;      ///< WebSocket 对象。
    std::string m_pending;             ///< 当前消息未消费的字节。
    std::size_t m_offset = 0;          ///< `m_pending` 的读游标。
    std::atomic<bool> m_closed{false}; ///< 关闭标志。
};

}  // namespace go2cpp::rpc
