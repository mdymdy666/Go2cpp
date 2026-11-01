#pragma once

/**
 * @file registry.hpp
 * @brief 服务发现：服务名 → 存活实例列表（心跳保活，租约过期自动剔除）。
 *
 * ## 语义
 *
 * - `Register` 在协调者上登记一个 `(service, instance)` 租约，并返回一个
 *   自动续约的 `Registration` 句柄；
 * - 句柄在后台按 `ttl / 3` 续约；进程崩溃/线程卡死超过 TTL 后，协调者会把
 *   实例剔除，`Lookup` 不再返回它；
 * - `Registration::Stop()` 主动注销（滚动下线），注销后立即从列表消失；
 * - `Lookup` 返回的每个实例都带 `remaining`，调用方可以据此做负载均衡或
 *   提前告警。
 *
 * ## 与 relay::Hub 的关系
 *
 * 服务发现只维护「名字 → 地址」的元数据；真正的连接池由调用方用
 * `relay::Hub` 管理（一个 room/服务下挂多条连接）。两者刻意不合并：
 * 注册中心不应该持有业务连接，否则一次租约抖动会连带关闭正在传输的流。
 * 参考 `docs/modules/distributed.md` 的「服务发现 + 连接池」组合示例。
 *
 * ## 线程模型
 *
 * `ServiceRegistry` 本身无状态（只保存协调者指针与默认配置），可以多线程
 * 共享。每个 `Registration` 最多两个后台线程（Heartbeat + 本地 Lease），
 * 由 `detail::LeaseSession` 管理；`Stop()`/析构会 join 它们。
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

/// 服务注册的默认策略。
struct ServiceRegistryOptions {
    Duration default_ttl{3000};  ///< `Register` 未显式给 TTL 时使用。
    bool auto_renew{true};       ///< 是否启动后台心跳（关掉后需手动 `Renew`）。
    Duration renew_interval{0};  ///< 续约间隔；0 表示 `ttl / 3`。
};

class ServiceRegistry;

/**
 * @brief 一次服务注册的句柄（move-only，RAII）。
 *
 * 析构等价于 `Stop()`：先停心跳，再注销租约。因此临时对象离开作用域就会
 * 从发现列表消失，不会留下「进程还活着但实例已不再服务」的幽灵。
 *
 * @warning 句柄失效（`lost()`）后不会自动重新注册：重新注册会拿到新的
 *          租约编号，语义上是一个新实例。调用方应在 `OnLost` 里重建。
 */
class Registration final {
public:
    Registration() noexcept;
    ~Registration();

    Registration(Registration &&other) noexcept;
    Registration &operator=(Registration &&other) noexcept;
    Registration(const Registration &) = delete;
    Registration &operator=(const Registration &) = delete;

    /// @brief 是否仍然有效（已启动、未停止、未失效、未超过本地 deadline）。
    bool valid() const;
    /// @brief 是否因为续约失败/本地租约到期而失效（主动 `Stop()` 不算）。
    bool lost() const;
    /// @brief 协调者分配的租约编号；无效句柄返回 0。
    LeaseId lease() const;
    /// @brief 当前实例快照（`remaining` 按本地时钟估算）。
    ServiceInstance instance() const;

    /**
     * @brief 同步续约一次。
     * @return 协调者结果；已停止/已失效时返回 `kClosed`。
     * @note 后台心跳已开启时通常不需要手动调用，重复续约是幂等的。
     */
    Outcome Renew();

    /// @brief 停止心跳并注销（幂等）。返回后 `valid()` 为 false。
    void Stop();

    /**
     * @brief 设置失效回调（最多一次，在后台线程执行）。
     * @note 回调必须短小；若设置时已经失效，会立即在当前线程执行一次。
     */
    void OnLost(std::function<void()> callback);

private:
    struct Impl;

    explicit Registration(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> m_impl;

    friend class ServiceRegistry;
};

/**
 * @brief 服务发现的调用入口。
 *
 * 典型用法：
 * @code
 * Coordinator coordinator;
 * ServiceRegistry registry(coordinator);
 * auto registration = registry.Register("order-api", "10.0.0.7:9000#1", "10.0.0.7:9000");
 * for (const auto& instance : registry.Lookup("order-api")) {
 *     // 建立连接 / 更新负载均衡表
 * }
 * @endcode
 */
class ServiceRegistry final {
public:
    explicit ServiceRegistry(ICoordinator &coordinator, ServiceRegistryOptions options = {});
    ~ServiceRegistry();

    ServiceRegistry(const ServiceRegistry &) = delete;
    ServiceRegistry &operator=(const ServiceRegistry &) = delete;
    ServiceRegistry(ServiceRegistry &&) = delete;
    ServiceRegistry &operator=(ServiceRegistry &&) = delete;

    /**
     * @brief 注册实例并（默认）启动心跳。
     * @param ttl 租约时长；`Duration::zero()` 表示使用 `options().default_ttl`。
     * @return 成功返回句柄；参数非法/TTL 越界返回 `kInvalidArgument`；
     *         协调者不可达返回 `kUnavailable`。
     */
    Result<Registration> Register(std::string service, std::string instance, std::string endpoint,
                                  Duration ttl = Duration::zero());

    /// @brief 查询服务下的存活实例（按实例 id 排序）。
    std::vector<ServiceInstance> Lookup(std::string_view service) const;

    /// @brief 至少有一个存活实例的服务名（排序）。
    std::vector<std::string> Services() const;

    /// @brief 查询单个实例；不存在或已过期返回 `std::nullopt`。
    std::optional<ServiceInstance> Get(std::string_view service,
                                       std::string_view instance) const;

    const ServiceRegistryOptions &options() const noexcept { return m_options; }

private:
    ICoordinator &m_coordinator;
    ServiceRegistryOptions m_options;
};

} // namespace go2cpp::distributed
