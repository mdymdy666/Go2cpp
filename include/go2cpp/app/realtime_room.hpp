#pragma once

/**
 * @file realtime_room.hpp
 * @brief 通用实时房间组件：房间聚合任意连接 + 每连接有界队列的异步广播。
 *
 * @details 从 `app/bilibili` 的弹幕房间抽出的**业务无关**版本：房间不再绑定
 *          video_id，连接不再绑定 WebSocket，发送动作由业务通过 `SendFn` 注入。
 *          形状是「一个 hub 管若干房间，一个房间管若干连接，一条连接一个
 *          发送线程 + 一个有界队列」：
 *
 *          - `Broadcast()` 只入队、不碰 socket：慢连接填满自己的队列后被踢出，
 *            房间内其它连接不受影响；
 *          - 队列长度由 `RoomConfig::send_queue_cap` 控制（默认 256）；
 *          - 所有公开方法线程安全；`Leave()` / `CloseAll()` 幂等。
 *
 * ## SendFn 约定
 *
 *          返回 true 表示这条消息已交给对端，false（或抛异常）表示连接已坏，
 *          hub 会停止该连接的发送线程并在后续操作中摘除它。`SendFn` 在**该连接
 *          的发送线程**上执行，因此允许阻塞（例如写一个慢 socket）；但不要回调
 *          同一个 `RoomHub`——`Leave()` / `CloseAll()` 会等发送线程退出。
 *
 * ## 用法
 *
 * ```cpp
 * go2cpp::app::realtime::RoomHub hub;
 * const auto id = hub.Join("lobby", [this](std::string_view text) {
 *     return socket->Send(text);     // 业务自己决定怎么发
 * });
 * hub.Broadcast("lobby", R"({"type":"ready"})");
 * hub.Leave(id);                     // 停线程并从房间摘除
 * ```
 *
 * @author go2cpp contributors
 * @date 2026-11-03
 * @copyright MIT License
 */

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>

namespace go2cpp::app::realtime {

/// @brief hub 级配置。
struct RoomConfig {
    /// @brief 每条连接的发送队列上限；队列满即踢出该连接（0 视为 1）。
    std::size_t send_queue_cap{256};
};

/// @brief 房间注册表：room → 连接集合，所有方法线程安全。
class RoomHub {
   public:
    /// @brief 连接 id；进程内单调递增、不复用，0 表示无效。
    using ConnId = std::uint64_t;

    /// @brief 业务注入的发送动作：把一条完整消息发给该连接，失败返回 false。
    using SendFn = std::function<bool(std::string_view)>;

    /// @brief 构造；cfg 为空时使用默认配置（队列上限 256）。
    explicit RoomHub(RoomConfig cfg = {});

    /// @brief 析构：等价于 `CloseAll()`，会等待所有发送线程退出。
    ~RoomHub();

    RoomHub(const RoomHub&) = delete;
    RoomHub& operator=(const RoomHub&) = delete;

    /// @brief 加入房间并启动该连接的发送线程；`send` 为空时返回 0。
    ConnId Join(std::string_view room, SendFn send);

    /// @brief 离开房间并停止发送线程（幂等）；id 不存在时无操作。
    void Leave(ConnId id);

    /// @brief 向房间内所有连接入队一条消息；返回成功入队的连接数。
    std::size_t Broadcast(std::string_view room, std::string_view message);

    /// @brief 房间内仍在线的连接数（已被踢出、等待回收的不计）。
    std::size_t Online(std::string_view room) const;

    /// @brief 所有房间在线连接总数。
    std::size_t TotalOnline() const;

    /// @brief 停止并移除全部连接（幂等）。
    void CloseAll();

   private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace go2cpp::app::realtime
