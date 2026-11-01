#pragma once

/**
 * @file lease_session.hpp
 * @brief 内部工具：一条租约的本地会话（后台续约 + 本地失效保护）。
 *
 * ## 为什么需要它
 *
 * 三个原语的句柄（服务注册、锁、候选者）有完全相同的生命周期：
 *
 * 1. 在协调者上拿到一个 `LeaseId`；
 * 2. 按 `ttl / 3` 周期续约（复用 `relay::Heartbeat`，自带单线程 + 取消）；
 * 3. 用本地 `relay::Lease` 兜底：即使心跳线程被长时间阻塞，超过一个 TTL
 *    没有成功续约就标记失效，避免调用方误以为自己还持有资源；
 * 4. 失效时只触发一次回调，随后句柄的所有 `valid()` 立即返回 false。
 *
 * 把这套逻辑抽出来，避免在三个 facade 里各写一遍（并且各写错一次）。
 *
 * ## 失效判定的边界
 *
 * 本地 `alive()` 使用「最后一次成功续约的本地时间 + ttl」。协调者使用
 * 自己的时钟判定。两者之间最多相差一个 RTT，因此本地 `alive()` 只是
 * **快速失败**，不是持有权证明：真正防止双主的是 fencing token（见
 * `DistributedLock` 与 `LeaderElection` 的文档）。
 *
 * ## 线程模型
 *
 * 每个会话最多两个后台线程（Heartbeat + Lease），由 relay 组件管理；
 * 回调在后台线程执行，必须短小。`Stop()` 会 join 它们，因此不要在回调
 * 里同步 `Stop()` 同一个会话（relay 已对自连接做了 detach 处理，但语义上
 * 应避免），也不要在持有本对象内部锁时调用用户回调。
 *
 * @note 本文件属于 `go2cpp/distributed/` 的公共头文件，但 `detail` 命名
 *       空间不承诺 ABI 稳定；业务代码请使用 registry/lock/election 的
 *       公开句柄。
 *
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include "go2cpp/distributed/types.hpp"

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace go2cpp::distributed::detail {

/**
 * @brief 单条租约的续约会话。
 *
 * 用法：
 * @code
 * auto session = std::make_unique<LeaseSession>(ttl, ttl / 3,
 *     [this] { return coordinator.RenewLease(lease_id, owner); },
 *     [this] { NotifyLost(); });
 * session->Start();
 * ...
 * session->Stop();   // 幂等
 * @endcode
 */
class LeaseSession final {
public:
    /// 续约函数：成功返回协调者的租约视图，失败返回对应状态。
    using RenewFn = std::function<Result<LeaseView>()>;
    /// 失效回调：最多执行一次，在后台线程上。
    using LostFn = std::function<void()>;

    /**
     * @param ttl 租约时长（必须 > 0）。
     * @param renew_interval 后台续约间隔；`<= 0` 时取 `ttl / 3`（至少 1ms）。
     * @param renew 续约函数；不能为空。
     * @param on_lost 失效回调；可为空。
     */
    LeaseSession(Duration ttl, Duration renew_interval, RenewFn renew, LostFn on_lost);
    ~LeaseSession();

    LeaseSession(const LeaseSession &) = delete;
    LeaseSession &operator=(const LeaseSession &) = delete;
    LeaseSession(LeaseSession &&) = delete;
    LeaseSession &operator=(LeaseSession &&) = delete;

    /**
     * @brief 启动会话。
     * @param background `true` 启动 Heartbeat/Lease 后台续约；`false` 只记录
     *        起始 deadline，由调用方显式 `RenewNow()`（用于压测/手动续约，
     *        避免每个句柄两个线程）。两种模式都可以 `Stop()`。
     */
    void Start(bool background = true);

    /// @brief 停止后台续约并 join（幂等）。不会触发 `on_lost`。
    void Stop();

    /// @brief 是否已经启动过。
    bool started() const;
    /// @brief 是否已经失效（心跳失败/本地租约到期/显式标记）。
    bool lost() const;
    /// @brief 是否已经停止（`Stop()` 之后）。
    bool stopped() const;
    /**
     * @brief 本地视角是否仍然有效：已启动、未停止、未失效、且 `now < deadline`。
     * @warning 只是快速失败；跨进程场景请以协调者 + fencing token 为准。
     */
    bool alive() const;

    Duration ttl() const;
    Duration remaining() const;

    /**
     * @brief 同步续约一次（成功会刷新本地 deadline）。
     * @return 与 `RenewFn` 相同；已停止/已失效时返回 `kClosed`。
     * @note 与后台心跳并发调用是安全的；两次续约都成功只会把 deadline
     *       往后推两次，不会产生额外租约。
     */
    Result<LeaseView> RenewNow();

    /// @brief 显式标记失效（例如调用方发现 fencing 已被抢先）。回调最多一次。
    void MarkLost(std::string reason = {});

    /**
     * @brief 设置/替换失效回调。
     * @note 如果设置时已经失效，回调会在当前线程同步执行一次。
     */
    void SetLostCallback(LostFn callback);

private:
    struct State;

    static bool RenewFromHeartbeat(const std::shared_ptr<State> &state);
    static void MarkLostState(const std::shared_ptr<State> &state, std::string reason);

    std::shared_ptr<State> m_state;
};

} // namespace go2cpp::distributed::detail
