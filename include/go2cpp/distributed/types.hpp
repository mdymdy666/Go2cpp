#pragma once

/**
 * @file types.hpp
 * @brief 分布式设施公共类型：状态码、租约视图、服务实例、锁与选举快照。
 *
 * ## 模块定位
 *
 * `go2cpp/distributed/` 在既有的 `relay::Lease` / `relay::Heartbeat` /
 * `relay::Hub` 之上实现三个原语：
 *
 * - `ServiceRegistry`：服务名 → 实例列表（心跳保活，租约过期剔除）；
 * - `DistributedLock`：key → 持有者 + 租约（续约，过期释放，fencing token）；
 * - `LeaderElection`：候选者租约最小者为主，主失效自动重选。
 *
 * 三者共享同一个**协调者**（`Coordinator`）：它是进程内唯一的状态机，
 * 也是全局单调性的来源（fencing token、选举任期 epoch、租约序号）。
 * 本文件只定义跨模块传递的值对象，不包含任何状态。
 *
 * ## 时间口径
 *
 * 所有对外暴露的 `remaining` 都是**剩余毫秒数**，不是绝对时间点。原因：
 * 协调者可能运行在另一个进程里，`steady_clock` 的 epoch 跨进程不保证一致
 * （Linux 的 `CLOCK_MONOTONIC` 同机一致，但跨机器不一致）。租约的安全边界
 * 永远是「协调者判定的 deadline + fencing token」，客户端的本地估算只用于
 * 快速失败，不能当作持有权的证明。
 *
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace go2cpp::distributed {

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;
using Duration = std::chrono::milliseconds;

/// 租约编号：协调者进程内单调递增，0 表示无效。
using LeaseId = std::uint64_t;
inline constexpr LeaseId kInvalidLease = 0;

/// 操作结果分类。成功以外的取值都表示**状态没有被改变**，除非契约另有说明。
enum class Status : std::uint8_t {
    kOk = 0,               ///< 成功。
    kInvalidArgument = 1,  ///< 参数非法（空名、TTL 越界等）。
    kNotFound = 2,         ///< 目标租约/服务/锁/选举不存在。
    kConflict = 3,         ///< 锁已被其他持有者占用。
    kExpired = 4,          ///< 租约已经过期，续约被拒绝。
    kNotOwner = 5,         ///< 租约编号与持有者不匹配（防止误续/误放）。
    kUnavailable = 6,      ///< 协调者不可达（远程客户端断线/超时）。
    kProtocolError = 7,    ///< 远端响应无法解析（版本不匹配/帧损坏）。
    kClosed = 8,           ///< 协调者或服务器已经停止。
};

/// 状态码的可读名称（英文标识，便于日志与断言）。
const char* StatusName(Status status) noexcept;

/**
 * @brief 只有状态、没有返回值的操作结果。
 * @details 与 `Result<T>` 共用 `Status`/`message` 两个字段，便于调用方统一
 *          判断；`message` 只用于诊断，不参与逻辑。
 */
struct Outcome {
    Status status{Status::kOk};
    std::string message;

    bool ok() const noexcept { return status == Status::kOk; }

    static Outcome Success() { return {}; }

    static Outcome Failure(Status status, std::string message = {}) {
        Outcome outcome;
        outcome.status = status;
        outcome.message = std::move(message);
        return outcome;
    }
};

/**
 * @brief 带值的操作结果。
 * @details 只在 `status == kOk` 时 `value` 才有意义。没有用异常：这些原语
 *          的失败是**正常业务分支**（锁被占用、租约过期），不是编程错误。
 */
template <typename T>
struct Result {
    T value{};
    Status status{Status::kOk};
    std::string message;

    bool ok() const noexcept { return status == Status::kOk; }

    static Result Success(T value) {
        Result result;
        result.value = std::move(value);
        return result;
    }

    static Result Failure(Status status, std::string message = {}) {
        Result result;
        result.status = status;
        result.message = std::move(message);
        return result;
    }
};

/// 租约类别：同一个租约表承载服务注册、锁、选举候选三种生命周期。
enum class LeaseKind : std::uint8_t {
    kService = 0,   ///< 服务实例注册。
    kLock = 1,      ///< 分布式锁持有。
    kElection = 2,  ///< 选主候选者。
};

/// 服务注册请求。名字不能为空；`ttl` 由调用方给出，协调者做范围校验。
struct ServiceRegistration {
    std::string service;   ///< 服务名，例如 `"order-api"`。
    std::string instance;  ///< 实例 id，同一服务内唯一，例如 `"10.0.0.7:9000#3"`。
    std::string endpoint;  ///< 实例地址（不解析，仅透传），例如 `"10.0.0.7:9000"`。
    std::string owner;     ///< 注册节点 id，用于续约归属校验。
    Duration ttl{3000};    ///< 租约时长；调用方必须在到期前续约。
};

/// 服务发现返回的一个存活实例。
struct ServiceInstance {
    std::string service;
    std::string instance;
    std::string endpoint;
    std::string owner;
    LeaseId lease{kInvalidLease};
    Duration ttl{0};
    Duration remaining{0};  ///< 距协调者判定过期的剩余时间。
};

/**
 * @brief 租约的对外快照（注册、续约、加入选举的统一返回）。
 * @details 服务/锁/选举共用同一结构，避免三套几乎相同的返回类型；与类别
 *          无关的字段保持默认值（例如服务租约的 `fencing` 为 0）。
 */
struct LeaseView {
    LeaseId id{kInvalidLease};
    LeaseKind kind{LeaseKind::kService};
    std::string name;      ///< 服务名 / 锁 key / 选举名。
    std::string instance;  ///< 服务实例 id；锁与选举为空。
    std::string owner;     ///< 持有者节点 id。
    std::string endpoint;  ///< 服务地址；锁与选举为空。
    Duration ttl{0};
    Duration remaining{0};
    /// 锁：授予时的 fencing token（单调递增，永不复用）；选举：当前任期号。
    std::uint64_t fencing{0};
    /// 选举：本租约是否当前 leader；服务/锁恒为 false。
    bool leader{false};
    /// 选举：当前 leader 节点 id；无主为空。服务/锁为空。
    std::string leader_id;
    /// 选举：当前leader任期号；其他类别为 0。
    std::uint64_t epoch{0};
};

/// 锁的当前状态快照。
struct LockInfo {
    std::string key;
    std::string owner;
    LeaseId lease{kInvalidLease};
    std::uint64_t fencing{0};
    Duration ttl{0};
    Duration remaining{0};
    /// 为 true 表示当前仍被有效持有（构造时只产生有效快照，保留字段便于
    /// 调用方在拷贝/缓存场景下显式判断）。
    bool held{false};
};

/// 选举中的一个候选者。
struct CandidateInfo {
    std::string node;
    LeaseId lease{kInvalidLease};
    Duration remaining{0};
    bool leader{false};
};

/// 当前 leader 的信息（`LeaderElection::Leader()` 的返回）。
struct LeaderInfo {
    std::string node;       ///< leader 节点 id。
    std::uint64_t epoch{0}; ///< 该 leader 的任期号。
    Duration remaining{0};  ///< leader 租约剩余时间。
};

/// 选举快照。
struct ElectionState {
    std::string election;
    std::string leader;         ///< 空表示当前无主。
    std::uint64_t epoch{0};     ///< 当前任期；每次 leader 变更（含变空）递增。
    std::vector<CandidateInfo> candidates;  ///< 按加入顺序（租约序号）排列。
};

/// 协调者计数器，用于观测与容量规划（不参与正确性判断）。
struct CoordinatorStats {
    std::size_t active_leases{0};
    std::size_t services{0};
    std::size_t instances{0};
    std::size_t locks{0};
    std::size_t held_locks{0};
    std::size_t elections{0};
    std::size_t candidates{0};
    std::uint64_t expired_total{0};  ///< 累计被过期剔除的租约数。
    std::uint64_t sweeps{0};         ///< 累计清扫轮数。
};

/// 把绝对截止时间换算成剩余毫秒（向上取整，避免 0.9ms 显示为 0）。
inline Duration RemainingUntil(TimePoint deadline, TimePoint now) noexcept {
    if (deadline <= now) {
        return Duration::zero();
    }
    // 余量按纳秒取整（向上取整到 1ms）：先截断到微秒会让 (0, 1µs) 的存活
    // 余量算出 0，调用方据此把「协调者判定仍有效」的租约当成已过期——语义
    // 两侧不一致，表现为随机测试里偶发的 remaining == 0 与无谓的重新抢锁。
    const auto remaining =
        std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now);
    Duration millis = std::chrono::duration_cast<Duration>(remaining);
    if (std::chrono::duration_cast<std::chrono::nanoseconds>(millis) < remaining) {
        millis += Duration(1);
    }
    return millis > Duration::zero() ? millis : Duration(1);
}

} // namespace go2cpp::distributed
