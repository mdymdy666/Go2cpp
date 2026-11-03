#pragma once

/// @file event_loop.hpp
/// @brief JinShan REQ-007：单线程事件循环 + 毫秒级定时器（rd::net）。
///
/// @details 职责边界：
///          - **单线程**：本类不建线程。Run() 阻塞在调用它的线程上，所有 IO /
///            定时器 / Post 回调都在该线程执行，上层不需要加锁；
///          - **不解析协议**：收到的数据报/流字节原样交给统一的
///            `rd::net::Protocol` / `rd::net::StreamProtocol` 事件循环层回调
///            （定义见 protocol.hpp），本层不认魔法数、不分帧、不做重排或重传决策
///            （属于 REQ-008/REQ-009）；
///          - **平台**：Linux/POSIX 为验收实现（poll(2) + eventfd(2)）；_WIN32 分支
///            只有占位，Create() 返回 IoStatus::kInvalid。socket.hpp 没有
///            kUnsupported，本层不新增枚举值，以免改动 REQ-006 产物；
///            将来接 Windows 时用 WSAPoll/select 须注意 select 的 FD_SETSIZE=64 上限。
///
///          单 tick 回调顺序（固定，测试断言）：
///          1. **Post 任务**：上一 tick 结束后新投递的任务，FIFO 保序，整批执行；
///          2. **到期定时器**：截止 tick 起点已到期的定时器，按 (deadline, id) 升序；
///          3. **IO 事件**：poll(2) 返回后派发，每个 socket 单 tick 最多 64 个数据报
///             / 读事件（防单个 socket 饿死其它 socket）。
///          回调中新投递的任务或新调度的定时器（即使 delay=0）进入下一个 tick，
///          不在当前 tick 内插队——顺序可预测，也避免回调自调度把单 tick 拖长。
///
///          精度：poll 超时按「最近定时器剩余时间向上取整到毫秒」计算，目标 ≤5ms；
///          poll 的时钟粒度为 1ms（Linux），定时器不会早于 deadline 触发。
///
///          线程安全：只有 Post() 与 Stop() 允许从其它线程调用；其余接口必须在
///          Run() 之前或 loop 线程内调用。
///
///          依赖方向：本头 include protocol.hpp（后者只前置声明 EventLoop），
///          `rd::net::Protocol` / `rd::net::StreamProtocol` 只有单一来源，不再有
///          占位定义，也无头文件循环。

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

#include "go2cpp/rd_net/protocol.hpp"
#include "go2cpp/rd_net/socket.hpp"

namespace rd::net {

/// @brief 事件循环使用的单调时钟（不受系统时间调整影响）。
using EventClock = std::chrono::steady_clock;
using EventTimePoint = EventClock::time_point;

/// @brief 单线程事件循环（poll(2) + 最小堆定时器 + eventfd 跨线程唤醒）。
class EventLoop {
public:
    /// @brief 创建事件循环。失败时返回 Err(kIoError, errno)。
    static Result<std::unique_ptr<EventLoop>> Create();

    ~EventLoop();

    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    /// @brief 绑定 UDP socket。fd 无效时返回 false（不注册）。重复绑定只更新 handler。
    bool AttachDatagram(DatagramSocket& socket, Protocol* handler);

    /// @brief 绑定 TCP socket。fd 无效时返回 false（不注册）。重复绑定只更新 handler。
    bool AttachStream(StreamSocket& socket, StreamProtocol* handler);

    /// @brief 解绑 UDP socket；已绑定时返回 true，并回调 handler->OnDetached()。
    bool Detach(DatagramSocket& socket);

    /// @brief 解绑 TCP socket；已绑定时返回 true，并回调 handler->OnDetached()。
    bool Detach(StreamSocket& socket);

    /// @brief 延迟 delay 后执行 callback，返回定时器 id。
    /// @note delay 为负按 0 处理；回调在 loop 线程执行；id 单调递增且永不为 0。
    std::uint64_t ScheduleAfter(std::chrono::milliseconds delay,
                                std::function<void()> callback);

    /// @brief 取消定时器。返回 false 表示 id 非法（0）；已触发或已在取消中的 id
    ///        返回 true 但不会再执行（惰性删除）。
    bool CancelTimer(std::uint64_t id);

    /// @brief 跨线程投递任务（唯一允许跨线程的入口之一）。唤醒阻塞中的 poll。
    void Post(std::function<void()> task);

    /// @brief 阻塞运行，直到 Stop()。返回后不再执行任何回调。
    void Run();

    /// @brief 请求停止（可从其它线程调用）。Run() 返回后未执行的任务被丢弃。
    void Stop();

    /// @brief 当前 tick 序号，从 1 开始；在回调里读到的是本 tick 的序号。
    std::uint64_t tick_count() const noexcept {
        return tick_.load(std::memory_order_acquire);
    }

    /// @brief 当前已绑定的 socket 数（诊断用，非线程安全）。
    std::size_t attached_count() const noexcept;

    /// @brief 当前未触发且未取消的定时器数（诊断用，非线程安全，含少量已取消项）。
    std::size_t timer_count() const noexcept;

private:
    EventLoop();

    void DrainPostedTasks();
    void FireDueTimers(EventTimePoint cutoff);
    void RefreshPollSet();
    int ComputePollTimeoutMs();
    void DispatchReadyEvents(int ready);
    void DrainDatagramSlot(std::size_t slot);
    void DrainStreamSlot(std::size_t slot);
    void DetachSlot(std::size_t slot);
    void DrainWakeFd();
    void Wake();

    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::atomic<bool> stop_{false};
    std::atomic<std::uint64_t> tick_{0};
};

}  // namespace rd::net
