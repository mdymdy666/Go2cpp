#pragma once

/**
 * @file metrics.hpp
 * @brief 分布式原语的指标接入点：指标名 + 进程级 `Counter` 获取函数。
 *
 * ## 模块定位
 *
 * P0 要求「`CoordinatorStats` 接入 `metrics::Registry`」。本文件只做**适配**：
 * 把 8 个命名计数器注册到既有的 `go2cpp::metrics::Registry::Default()`，
 * 不新造指标框架、不复制存储。业务代码（`coordinator.cpp` /
 * `lease_session.cpp` / `lock.cpp`）通过 `metrics::LeasesGranted()` 等
 * 函数拿引用后 `Inc()`，与 `src/tunnel/` 下的用法一致。
 *
 * ## 命名约定
 *
 * 全部带 `go2cpp_distributed_` 前缀，避免与业务指标撞名；`_total` 后缀
 * 表明单调计数器（Prometheus 惯例）。标签维度暂不引入：单个协调者是
 * 进程内单例，标签只会增加抓取成本。
 *
 * ## 线程安全
 *
 * `Registry::GetCounter` 内部加锁且返回的引用在注册表生命周期内稳定；
 * 函数内 `static` 引用由 C++11 保证线程安全初始化。`Counter::Inc()` 是
 * 无锁原子操作，可在任意线程调用。
 *
 * ## 失败语义
 *
 * 首次注册时同名指标若类型/help 不一致会抛 `std::invalid_argument`（由
 * `Registry` 决定）。这些函数**不吞异常**：指标名冲突是编程错误，启动期
 * 暴露比运行期静默丢弃好。
 *
 * @author go2cpp contributors
 * @date 2026-11-04
 * @copyright MIT License
 */

#include "go2cpp/metrics/metrics.hpp"

namespace go2cpp::distributed::metrics {

/// 协调者累计授予的租约数（服务注册 / 抢锁 / 加入选举的成功路径）。
inline constexpr const char* kLeasesGrantedTotal = "go2cpp_distributed_leases_granted_total";
/// 协调者累计判定过期的租约数（清扫 + 访问路径）。
inline constexpr const char* kLeasesExpiredTotal = "go2cpp_distributed_leases_expired_total";
/// 锁 fencing token 累计推进次数（每次成功授予一个更大的 token）。
inline constexpr const char* kFencingAdvancedTotal = "go2cpp_distributed_fencing_advanced_total";
/// 租约会话累计成功续约次数。
inline constexpr const char* kLeaseRenewTotal = "go2cpp_distributed_lease_renew_total";
/// 租约会话累计失效次数（心跳失败 / 本地到期 / 显式标记）。
inline constexpr const char* kLeaseLostTotal = "go2cpp_distributed_lease_lost_total";
/// 分布式锁累计获取尝试次数（含失败）。
inline constexpr const char* kLockAcquireTotal = "go2cpp_distributed_lock_acquire_total";
/// 分布式锁累计获取失败次数（冲突/非法/不可达）。
inline constexpr const char* kLockAcquireFailTotal =
    "go2cpp_distributed_lock_acquire_fail_total";
/// 分布式锁累计释放次数（仅统计真正执行的释放，幂等重复调用不计）。
inline constexpr const char* kLockReleaseTotal = "go2cpp_distributed_lock_release_total";

/// 指标 help 文本（与注册时一致，测试复用时避免重复字符串）。
inline constexpr const char* kLeasesGrantedHelp = "Leases granted by the coordinator.";
inline constexpr const char* kLeasesExpiredHelp = "Leases expired and removed by the coordinator.";
inline constexpr const char* kFencingAdvancedHelp = "Fencing tokens advanced on lock grants.";
inline constexpr const char* kLeaseRenewHelp = "Successful lease renewals from client sessions.";
inline constexpr const char* kLeaseLostHelp = "Lease sessions that lost their lease.";
inline constexpr const char* kLockAcquireHelp = "Distributed lock acquire attempts.";
inline constexpr const char* kLockAcquireFailHelp = "Distributed lock acquire failures.";
inline constexpr const char* kLockReleaseHelp = "Distributed lock releases.";

/// @brief 进程级 `leases_granted_total` 计数器。
inline ::go2cpp::metrics::Counter& LeasesGranted() {
    static auto& counter = ::go2cpp::metrics::Registry::Default().GetCounter(
        kLeasesGrantedTotal, kLeasesGrantedHelp);
    return counter;
}

/// @brief 进程级 `leases_expired_total` 计数器。
inline ::go2cpp::metrics::Counter& LeasesExpired() {
    static auto& counter = ::go2cpp::metrics::Registry::Default().GetCounter(
        kLeasesExpiredTotal, kLeasesExpiredHelp);
    return counter;
}

/// @brief 进程级 `fencing_advanced_total` 计数器。
inline ::go2cpp::metrics::Counter& FencingAdvanced() {
    static auto& counter = ::go2cpp::metrics::Registry::Default().GetCounter(
        kFencingAdvancedTotal, kFencingAdvancedHelp);
    return counter;
}

/// @brief 进程级 `lease_renew_total` 计数器。
inline ::go2cpp::metrics::Counter& LeaseRenew() {
    static auto& counter = ::go2cpp::metrics::Registry::Default().GetCounter(
        kLeaseRenewTotal, kLeaseRenewHelp);
    return counter;
}

/// @brief 进程级 `lease_lost_total` 计数器。
inline ::go2cpp::metrics::Counter& LeaseLost() {
    static auto& counter = ::go2cpp::metrics::Registry::Default().GetCounter(
        kLeaseLostTotal, kLeaseLostHelp);
    return counter;
}

/// @brief 进程级 `lock_acquire_total` 计数器。
inline ::go2cpp::metrics::Counter& LockAcquire() {
    static auto& counter = ::go2cpp::metrics::Registry::Default().GetCounter(
        kLockAcquireTotal, kLockAcquireHelp);
    return counter;
}

/// @brief 进程级 `lock_acquire_fail_total` 计数器。
inline ::go2cpp::metrics::Counter& LockAcquireFail() {
    static auto& counter = ::go2cpp::metrics::Registry::Default().GetCounter(
        kLockAcquireFailTotal, kLockAcquireFailHelp);
    return counter;
}

/// @brief 进程级 `lock_release_total` 计数器。
inline ::go2cpp::metrics::Counter& LockRelease() {
    static auto& counter = ::go2cpp::metrics::Registry::Default().GetCounter(
        kLockReleaseTotal, kLockReleaseHelp);
    return counter;
}

} // namespace go2cpp::distributed::metrics
