#pragma once

/**
 * @file transport.hpp
 * @brief 传输层：Bento VideoChannel（1200B 分片 + 4 路重组）+ UDP 回环。
 * @details 复用（不修改）`go2cpp::bento::VideoChannel`：
 *
 *          - 发送侧 `SendFrame(frame_id, key, data, bytes)` 把一段字节拆成
 *            `<=1200B` 的 VIDEO 分片，每个分片是完整的 16B 帧头 + 8B 视频子头 +
 *            数据，可直接 `sendto`；
 *          - 接收侧 `OnFrame(header, payload, bytes)` 做乱序重组、旧帧丢弃
 *            （DropStale）、缺片超时清理（1s）；
 *          - 单帧上限 256 片 ≈ 300KB，足够 640x480 BGRA（1.2MB）以外的路径；
 *            实际 BGRA 640x480 超过上限，这也是「默认走 I420/差分」的原因之一。
 *
 *          UDP 端点用 POSIX socket（AF_INET/SOCK_DGRAM），绑定 127.0.0.1:0 由
 *          内核分配端口，`Recv` 用 `poll` 实现带超时的非阻塞读，便于单线程
 *          pump 事件循环（不引入线程，测试可复现）。
 *
 *          `InProcessVideoLink` 是同一套分片逻辑的进程内直连版本：不经过 socket，
 *          用于隔离「编解码正确性」与「UDP 丢包」两类问题（单测更快更稳）。
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "go2cpp/bento/net/channel.hpp"

namespace go2cpp::media {

/// @brief UDP 端点（IPv4 回环）。
class UdpEndpoint {
   public:
    UdpEndpoint() = default;
    ~UdpEndpoint();

    UdpEndpoint(const UdpEndpoint&) = delete;
    UdpEndpoint& operator=(const UdpEndpoint&) = delete;

    /// @brief 绑定 127.0.0.1:0（内核分配端口）。
    bool Open(std::string* error);

    /// @brief 记录对端端口（不调用 connect，保留收任意源的能力）。
    void SetPeerPort(std::uint16_t port) noexcept {
        m_peer_port = port;
    }

    /// @brief 已绑定端口；未打开返回 0。
    std::uint16_t port() const noexcept {
        return m_port;
    }

    /// @brief 发送一个数据报给对端。
    bool Send(const std::uint8_t* data, std::size_t bytes) noexcept;

    /// @brief 带超时收一个数据报。
    /// @param timeout_ms <=0 表示立即返回（非阻塞）。
    /// @return 收到返回字节数；0 表示超时；<0 表示错误。
    int Recv(std::uint8_t* buffer, std::size_t capacity, int timeout_ms) noexcept;

    /// @brief 关闭 socket。
    void Close() noexcept;

    /// @brief socket 是否已打开。
    bool opened() const noexcept {
        return m_fd >= 0;
    }

   private:
    int m_fd = -1;
    std::uint16_t m_port = 0;
    std::uint16_t m_peer_port = 0;
};

/// @brief 帧交付回调：frame_id、是否关键帧、重组后的完整帧字节。
using FrameDeliverFn =
    std::function<void(std::uint32_t frame_id, bool key, const std::uint8_t* data, std::size_t bytes)>;

/// @brief 基于 UDP 的视频发送端。
class UdpVideoSender {
   public:
    UdpVideoSender();
    ~UdpVideoSender();

    UdpVideoSender(const UdpVideoSender&) = delete;
    UdpVideoSender& operator=(const UdpVideoSender&) = delete;

    /// @brief 打开本地 socket 并指向 `peer_port`。
    bool Open(std::uint16_t peer_port, std::string* error);

    /// @brief 本端端口。
    std::uint16_t local_port() const noexcept;

    /// @brief 分片发送一帧（`<=300KB`）。
    bool SendFrame(std::uint32_t frame_id, bool key, const std::uint8_t* data, std::size_t bytes);

    /// @brief VideoChannel 统计。
    const go2cpp::bento::VideoChannel::Stats& stats() const noexcept;

    /// @brief 最近一次 sendto 的失败次数。
    std::uint64_t send_failures() const noexcept;

   private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

/// @brief 基于 UDP 的视频接收端。
class UdpVideoReceiver {
   public:
    explicit UdpVideoReceiver(FrameDeliverFn deliver);
    ~UdpVideoReceiver();

    UdpVideoReceiver(const UdpVideoReceiver&) = delete;
    UdpVideoReceiver& operator=(const UdpVideoReceiver&) = delete;

    /// @brief 绑定本地端口。
    bool Open(std::string* error);

    /// @brief 本地端口。
    std::uint16_t local_port() const noexcept;

    /// @brief 收包并驱动重组，直到超时或处理完 `max_packets` 个数据报。
    /// @return 交付的完整帧数。
    int Pump(int timeout_ms, int max_packets);

    /// @brief 清理超时重组项（应周期性调用）。
    void OnTimer(std::uint64_t now_ms);

    /// @brief VideoChannel 统计。
    const go2cpp::bento::VideoChannel::Stats& stats() const noexcept;

    /// @brief 收到的 UDP 数据报数。
    std::uint64_t datagrams() const noexcept {
        return m_datagrams;
    }

    /// @brief 解析失败（非 Bento 帧/版本不符）的数据报数。
    std::uint64_t malformed() const noexcept {
        return m_malformed;
    }

   private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    FrameDeliverFn m_deliver;
    std::uint64_t m_datagrams = 0;
    std::uint64_t m_malformed = 0;
};

/// @brief 进程内直连链路（同一套分片/重组，不走 socket）。
class InProcessVideoLink {
   public:
    InProcessVideoLink();
    ~InProcessVideoLink();

    InProcessVideoLink(const InProcessVideoLink&) = delete;
    InProcessVideoLink& operator=(const InProcessVideoLink&) = delete;

    /// @brief 发送一帧。
    bool SendFrame(std::uint32_t frame_id, bool key, const std::uint8_t* data, std::size_t bytes);

    /// @brief 把「已发送但未投递」的分片全部注入接收端（默认不丢包）。
    void Flush();

    /// @brief 模拟丢包：跳过第 `drop_every` 个分片（0/1 表示不丢）。
    void SetDropEvery(std::uint32_t drop_every) noexcept;

    /// @brief 设置交付回调。
    void SetDeliver(FrameDeliverFn deliver);

    /// @brief 发送端统计。
    const go2cpp::bento::VideoChannel::Stats& send_stats() const noexcept;

    /// @brief 接收端统计。
    const go2cpp::bento::VideoChannel::Stats& recv_stats() const noexcept;

    /// @brief 已交付帧数。
    std::uint64_t delivered() const noexcept {
        return m_delivered;
    }

    /// @brief 接收端超时清理（驱动缺片丢弃）。
    void OnTimer(std::uint64_t now_ms);

   private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    std::uint64_t m_delivered = 0;
};

}  // namespace go2cpp::media
