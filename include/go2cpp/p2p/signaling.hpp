#pragma once

/**
 * @file signaling.hpp
 * @brief P2P 信令抽象：交换候选、连接结果等控制信息。
 * @details P2P 打洞的关键前提是「双方几乎同时向对方发包」，因此必须先通过
 *          一个**已连通的旁路**交换候选。本模块不绑定具体 IM/消息队列，
 *          只定义最小接口；实际部署时用 IM 私聊、WebSocket、HTTP 轮询或
 *          Redis Pub/Sub 实现即可。
 *
 *          接口语义：
 *          - `Send()` 把一段文本放进对端可读的队列，返回是否成功入队；
 *          - `Recv()` 在超时内取一条文本，返回 `> 0` 收到、`0` 超时、
 *            `< 0` 通道关闭/错误；
 *          - 消息是**一次性**的（不是广播），双方各自实现自己的通道。
 *
 *          `LoopbackSignaling` 是进程内实现，用于测试与单进程演示；跨进程
 *          部署请自行实现（例如把 `payload` 原样作为 IM 消息体发送）。
 *
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>

namespace go2cpp::p2p {

namespace detail {
/// 进程内信令邮箱的内部实现（定义在 signaling.cpp）。
class SignalingMailbox;
}  // namespace detail

class Signaling {
public:
    virtual ~Signaling() = default;

    /// 发送一条信令消息；返回是否成功入队。
    virtual bool Send(const std::string& payload) = 0;

    /// 接收一条信令消息；`> 0` 字节数、`0` 超时、`< 0` 通道关闭。
    virtual int Recv(std::string* payload, std::chrono::milliseconds timeout) = 0;
};

/**
 * @brief 进程内双向信令通道（测试/演示用）。
 *
 * @details 两个实例共享一个线程安全的邮箱，A 发出的消息只能被 B 收到，
 *          反之亦然。删除任一实例不会影响另一个实例的 `Recv()`（返回超时）。
 */
class LoopbackSignaling final : public Signaling {
public:
    /**
     * @brief 创建一对互通的信令端点。
     * @return `{side_a, side_b}`；两者都非空。
     */
    static std::pair<std::unique_ptr<LoopbackSignaling>, std::unique_ptr<LoopbackSignaling>>
    CreatePair();

    /**
     * @brief 构造一个信令端点。
     * @param mailbox 与对端共享的邮箱（由 `CreatePair()` 创建）。
     * @param side 0 或 1；0 发出的消息由 1 接收，反之亦然。
     * @note 通常不直接调用；用 `CreatePair()` 保证两侧成对。
     */
    LoopbackSignaling(std::shared_ptr<detail::SignalingMailbox> mailbox, int side);

    ~LoopbackSignaling() override = default;

    LoopbackSignaling(const LoopbackSignaling&) = delete;
    LoopbackSignaling& operator=(const LoopbackSignaling&) = delete;

    bool Send(const std::string& payload) override;
    int Recv(std::string* payload, std::chrono::milliseconds timeout) override;

private:
    std::shared_ptr<detail::SignalingMailbox> m_mailbox;
    int m_side = 0;
};

}  // namespace go2cpp::p2p
