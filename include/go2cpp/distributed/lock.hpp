#pragma once

/**
 * @file lock.hpp
 * @brief 分布式锁：key → 持有者 + 租约（续约，过期释放，fencing token）。
 *
 * ## 语义
 *
 * - `Acquire` 在协调者上原子地「检查 + 授予」：同一 key 同时只有一个有效
 *   持有者；被占用时返回 `kConflict`（不阻塞、不排队）；
 * - 每次授予都会分配一个**严格单调递增**的 fencing token。即使发生
 *   「旧持有者以为自己还持有、实际租约已过期」的经典场景，新持有者的
 *   token 也一定更大；下游资源（数据库/文件/配置中心）只要拒绝更小的
 *   token，就能把双主写入挡在门外；
 * - 租约过期由协调者判定并释放，不依赖客户端主动 `Release`；
 * - `LockHandle` 默认后台续约；`Stop()`/析构会释放锁。
 *
 * ## 不丢锁 vs 不双持锁
 *
 * 这两个目标在异步网络下不可能同时绝对成立（FLP/租约的经典取舍），本模块
 * 的取舍是：
 *
 * - **不双持锁**优先：协调者一旦判定过期就允许新持有者进入，旧持有者的
 *   本地 `valid()` 立即变 false（或最迟在下一个本地 deadline 变 false），
 *   并且它的 token 更小；
 * - **不丢锁**靠续约窗口：后台按 `ttl/3` 续约，连续失败才会丢；短暂 GC
 *   或调度抖动不会丢锁。
 *
 * 因此正确用法是：**持有锁期间每次写操作都带上 `fencing_token()`**，
 * 下游校验 token 单调；不要只依赖 `valid()` 做互斥。
 *
 * ## 线程模型
 *
 * `DistributedLock` 无状态、可多线程共享。`LockHandle` 是 move-only：
 * 一个句柄只对应一次授予，跨线程使用需要外部同步（句柄内部的状态查询
 * 是安全的，但「用这个句柄保护的业务状态」需要调用方自己同步）。
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

/// 获取锁时的本地续约策略。
struct LockOptions {
    bool auto_renew{true};      ///< 是否启动后台心跳；关闭后需手动 `Renew()`。
    Duration renew_interval{0}; ///< 续约间隔；0 表示 `ttl / 3`。
};

class DistributedLock;

/**
 * @brief 一次锁授予的句柄（move-only，RAII）。
 *
 * 析构等价于 `Release()`。释放顺序是「先停心跳，再通知协调者」，避免
 * 释放后又被一次迟到的续约重新拉活。
 */
class LockHandle final {
public:
    LockHandle() noexcept;
    ~LockHandle();

    LockHandle(LockHandle &&other) noexcept;
    LockHandle &operator=(LockHandle &&other) noexcept;
    LockHandle(const LockHandle &) = delete;
    LockHandle &operator=(const LockHandle &) = delete;

    /// @brief 本地视角是否仍然持有：未释放、未失效、未超过本地 deadline。
    bool valid() const;
    /// @brief 是否已经失效（续约失败/本地到期）。已释放的句柄返回 false。
    bool lost() const;

    LeaseId lease() const;
    /// @brief 本次授予的 fencing token（严格单调递增，永不复用）。
    std::uint64_t fencing_token() const;
    const std::string &key() const;
    const std::string &owner() const;
    Duration remaining() const;

    /// @brief 当前状态快照（`held` 反映本地 `valid()`）。
    LockInfo info() const;

    /**
     * @brief 同步续约一次。
     * @return `kOk`；已释放返回 `kClosed`；协调者判定过期返回 `kExpired`
     *         （此时句柄进入 lost 状态，调用方必须停止写操作）。
     */
    Outcome Renew();

    /// @brief 释放锁（幂等）。返回后 `valid()` 为 false。
    void Release();

    /**
     * @brief 设置失效回调（最多一次，后台线程执行）。
     * @note 设置时若已经失效，会立即在当前线程执行一次。
     */
    void OnLost(std::function<void()> callback);

private:
    struct Impl;

    explicit LockHandle(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> m_impl;

    friend class DistributedLock;
};

/**
 * @brief 分布式锁入口。
 *
 * @code
 * DistributedLock locks(coordinator);
 * auto handle = locks.Acquire("job:42", "worker-3", std::chrono::seconds(30));
 * if (handle.ok()) {
 *     // handle.value().fencing_token() 必须随写请求传给下游
 * }
 * @endcode
 */
class DistributedLock final {
public:
    explicit DistributedLock(ICoordinator &coordinator) noexcept;

    /**
     * @brief 尝试获取锁（非阻塞）。
     * @param ttl 租约时长；必须落在协调者允许的区间内。
     * @return 成功返回句柄；已被占用返回 `kConflict`；参数非法返回
     *         `kInvalidArgument`；协调者不可达返回 `kUnavailable`。
     */
    Result<LockHandle> Acquire(std::string key, std::string owner, Duration ttl,
                               LockOptions options = {});

    /// @brief 查询当前持有者；未持有返回 `std::nullopt`。
    std::optional<LockInfo> Query(std::string_view key) const;

    /// @brief 列出所有有效锁（按 key 排序）。
    std::vector<LockInfo> Locks() const;

private:
    ICoordinator &m_coordinator;
};

} // namespace go2cpp::distributed
