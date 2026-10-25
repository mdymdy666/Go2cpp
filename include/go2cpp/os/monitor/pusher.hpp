#pragma once

// Pusher：周期性把 SnapshotSource 的最新快照以 JSON 形式 HTTP POST 到 Manager。
//
// 解耦：Pusher 只依赖 SnapshotSource（Aggregator 是默认实现），自身实现
// SnapshotSink，因此 Aggregator 也可以直接把 Pusher 当 sink 注入；HTTP 细节
// 下沉到 HttpSnapshotSink。失败只记录统计与 last_error，不抛异常、不中断采集。

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

#include "go2cpp/os/monitor/interfaces.hpp"
#include "go2cpp/os/monitor/monitor.hpp"

namespace go2cpp::os::monitor {

// 前置声明即可：兼容构造只把 Aggregator 当 SnapshotSource 用，Pusher 的
// 公开接口不暴露聚合器的任何成员。pusher.hpp 因此不依赖具体聚合器定义。
class Aggregator;

/** @brief 推送配置。 */
struct PusherConfig {
    /// Manager 的推送地址（完整 URL）。
    std::string manager_url{"http://127.0.0.1:9101/api/push"};
    /// 推送周期。
    std::chrono::milliseconds interval{1000};
    /// 单次 HTTP 请求超时。
    std::chrono::milliseconds timeout{5000};
};

/** @brief 推送统计。 */
struct PusherStats {
    std::uint64_t pushed{0};
    std::uint64_t failed{0};
    std::string last_error;
};

/**
 * @brief 周期性把 Snapshot 推送到 Manager。
 *
 * Pusher 不拥有数据源：调用方负责其生命周期，且 source 必须比 Pusher 活得久。
 * PushSnapshot()/Push() 允许调用方推送任意快照（测试用）。
 */
class Pusher : public SnapshotSink {
public:
    Pusher(PusherConfig config, SnapshotSource& source);
    /// 兼容构造：Aggregator 本身是 SnapshotSource。
    Pusher(PusherConfig config, Aggregator& aggregator);
    ~Pusher() override;

    Pusher(const Pusher&) = delete;
    Pusher& operator=(const Pusher&) = delete;

    /** @brief 启动后台推送 Fiber。重复调用返回 false。 */
    bool Start();

    /** @brief 停止并等待推送 Fiber 退出；可重复调用。 */
    void Stop();

    bool running() const noexcept;

    /** @brief 立即把 source.Latest() 推送一次。 */
    bool PushOnce();

    /** @brief 推送指定快照。 */
    bool PushSnapshot(const Snapshot& snapshot);

    /** @brief SnapshotSink 接口：等价于 PushSnapshot。 */
    bool Push(const Snapshot& snapshot) override;
    const char* kind() const noexcept override { return "pusher"; }

    /** @brief 统计快照。 */
    PusherStats Stats() const;

    /** @brief 当前配置。 */
    const PusherConfig& config() const noexcept { return config_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    PusherConfig config_;
};

}  // namespace go2cpp::os::monitor
