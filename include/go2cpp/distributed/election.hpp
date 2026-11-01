#pragma once

/**
 * @file election.hpp
 * @brief 选主：候选者租约最小者为主，主失效后自动重选。
 *
 * ## 选举规则
 *
 * 每个候选者在协调者上持有一条租约；**租约编号最小（最早加入）的存活
 * 候选者为主**。编号在加入时分配、续约时不变，因此：
 *
 * - 先加入者优先，天然避免「谁剩余时间多谁当主」造成的抖动；
 * - leader 续约不会改变自己的编号，不会因为某次续约晚了一点就丢掉主；
 * - 主失效（进程崩溃 / 停止续约超过 TTL）后，协调者清扫时把它从候选者
 *   集合里移除，并立即把 leader 改判给下一个编号最小的候选者；
 * - 每次 leader 变化（包括「有主 → 无主」）都会让 `epoch` 加一，客户端可以
 *   用 `epoch` 判断自己看到的 leader 是否已经过时。
 *
 * 「租约最短者为主」在这里按**最早取得的租约**落地。按剩余时间排序会让
 * leader 随续约时序漂移，甚至出现两个节点交替自认为主的窗口；按编号排序
 * 是唯一稳定且可验证的解释（详见 `docs/modules/distributed.md` §3）。
 *
 * ## 不双主
 *
 * leader 的唯一性由协调者的单锁保证；`epoch` 单调递增，等价于选主场景的
 * fencing token。旧主恢复后不会「记起」自己还是主——它自己的租约已经过期，
 * `Candidate::lost()` 为 true，且新主的 epoch 更大。
 *
 * ## 线程模型
 *
 * `LeaderElection` 无状态、可多线程共享；`Candidate` 是 move-only 句柄，
 * 后台按 `ttl/3` 续约并刷新 leader 缓存。`is_leader()` 读的是缓存，可能比
 * 协调者晚一个心跳间隔；需要强一致判断时调 `Refresh()`。
 *
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include "go2cpp/distributed/coordinator.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace go2cpp::distributed {

/// 加入选举时的本地续约策略。
struct CandidateOptions {
    bool auto_renew{true};      ///< 是否启动后台心跳；关闭后需手动 `Renew()`。
    Duration renew_interval{0}; ///< 续约间隔；0 表示 `ttl / 3`。
};

class LeaderElection;

/**
 * @brief 一次候选人身份的句柄（move-only，RAII）。
 *
 * 析构等价于 `Leave()`：停止续约并退出选举，让下一个候选者立即接任。
 */
class Candidate final {
public:
    Candidate() noexcept;
    ~Candidate();

    Candidate(Candidate &&other) noexcept;
    Candidate &operator=(Candidate &&other) noexcept;
    Candidate(const Candidate &) = delete;
    Candidate &operator=(const Candidate &) = delete;

    /// @brief 本地候选者身份是否仍有效（未退出、未失效、未超过 deadline）。
    bool valid() const;
    /// @brief 是否因续约失败/本地到期而失效（主动 `Leave()` 不算）。
    bool lost() const;

    /// @brief 缓存视角下本节点是否当前 leader（可能滞后一个心跳周期）。
    bool is_leader() const;
    /// @brief 缓存的当前 leader 节点 id；无主为空串。
    std::string leader_id() const;
    /// @brief 缓存的当前 leader 任期。
    std::uint64_t epoch() const;
    const std::string &node() const;
    const std::string &election() const;
    LeaseId lease() const;
    Duration remaining() const;

    /**
     * @brief 主动查询协调者并刷新 leader 缓存。
     * @return 查询结果；选举暂时无候选者时返回 `kOk` 且 `leader_id()` 为空。
     */
    Outcome Refresh();

    /// @brief 同步续约一次，并用返回值刷新 leader 缓存。
    Outcome Renew();

    /// @brief 退出选举（幂等）。返回后 `valid()` 为 false。
    void Leave();

    /**
     * @brief 设置 leader 变化回调。
     * @param callback 参数为空表示当前无主；在后台线程执行，必须短小。
     * @note 只有当 leader 节点或 epoch 发生变化时才触发；设置时不会补发
     *       当前状态。若已经退出，回调不再触发。
     */
    void OnLeaderChange(std::function<void(const std::optional<LeaderInfo> &)> callback);

private:
    struct Impl;

    explicit Candidate(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> m_impl;

    friend class LeaderElection;
};

/**
 * @brief 选主入口。
 *
 * @code
 * LeaderElection election(coordinator);
 * auto candidate = election.Join("primary", "node-a", std::chrono::seconds(5));
 * if (candidate.ok() && candidate.value().is_leader()) { ... }
 * @endcode
 */
class LeaderElection final {
public:
    explicit LeaderElection(ICoordinator &coordinator) noexcept;

    /**
     * @brief 加入选举（幂等：同一节点重复加入是原地续约）。
     * @param ttl 候选者租约时长；主必须在到期前续约。
     * @return 成功返回句柄；参数非法返回 `kInvalidArgument`；协调者不可达
     *         返回 `kUnavailable`。
     */
    Result<Candidate> Join(std::string election, std::string node, Duration ttl,
                           CandidateOptions options = {});

    /// @brief 查询选举快照（候选者按加入顺序，已过期者被过滤）。
    ElectionState State(std::string_view election) const;

    /// @brief 当前 leader；无主或选举不存在返回 `std::nullopt`。
    std::optional<LeaderInfo> Leader(std::string_view election) const;

    /// @brief 所有已知选举名（排序）。
    std::vector<std::string> Elections() const;

private:
    ICoordinator &m_coordinator;
};

} // namespace go2cpp::distributed
