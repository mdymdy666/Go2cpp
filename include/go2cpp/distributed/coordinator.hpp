#pragma once

/**
 * @file coordinator.hpp
 * @brief 协调者：服务注册、分布式锁、选主共用的租约状态机（单进程权威源）。
 *
 * ## 为什么需要一个「协调者」
 *
 * 三个原语表面上是三套 API，实际共享同一条不变量：**任何资源的持有权都由
 * 一条带截止时间的租约表示，续约只能由持有者发起，过期只能由协调者判定**。
 * 把这条不变量收拢到一个 `Coordinator` 里，可以保证：
 *
 * 1. **不存在双主/双持锁**：锁与 leader 的授予只发生在这一个互斥区；
 * 2. **序号单调**：fencing token 与选举任期由同一处递增，永不复用；
 * 3. **过期判定唯一**：所有客户端对「过期」的理解都来自同一份 deadline。
 *
 * `Coordinator` 本身不联网：它可以在单进程内被多个线程共享（多线程模拟），
 * 也可以被 `CoordinatorServer` 通过 TCP 暴露给多个进程（多进程模拟/真实部署）。
 * 客户端侧的 `ServiceRegistry` / `DistributedLock` / `LeaderElection` 都只依赖
 * `ICoordinator` 接口，因此本地与远程可以无缝切换。
 *
 * ## 租约模型
 *
 * 一条租约 = `(id, kind, name, owner, deadline)`。过期判定是
 * `now >= deadline`（单调时钟）。过期后协调者仍可能暂时保留记录，直到清扫
 * 线程或下一次访问把它剔除；**所有查询都会过滤掉已过期记录**，因此外部
 * 永远看不到「过期但仍可用」的资源。
 *
 * 后台清扫线程默认 10ms 一轮（`CoordinatorOptions::sweep_interval`），负责
 * 把过期记录从索引里移除并重算选举。查询路径不依赖清扫及时性——它自己
 * 过滤——所以清扫慢只会推迟 `Stats` 与内存回收，不会推迟正确性。
 *
 * ## 线程模型与回调
 *
 * 单一互斥锁保护整张表，临界区只做容器操作，不做 I/O、不调用用户代码。
 * 因此 `Coordinator` 的任意方法都可以从任意线程调用（包括清扫线程）。
 * 它不保存也不回调用户函数，`Stop()` 只会停止清扫线程。
 *
 * ## 不做什么
 *
 * - 不做「租约持有权」的持久化：协调者重启后租约全部失效，客户端必须重新
 *   注册（这正是租约语义的一部分，调用方应把「重新注册」写成幂等操作）；
 * - 但 **fencing token / 选举 epoch / 租约编号水位会持久化**（P1）：
 *   见下面的「快照持久化」一节——这是「永不复用」的最后一道防线；
 * - 不做时钟同步：所有判定基于本进程 `steady_clock`；跨机部署时租约剩余
 *   时间只作为提示，安全边界是 fencing token；
 * - 不做配额：单机内存上限由调用方控制，`Stats` 只用于观测。
 *
 * ## 快照持久化（P1）
 *
 * 配置 `CoordinatorOptions::snapshot_path` 后：
 *
 * 1. 构造时 `Restore()` 从快照恢复 `m_fencing`（锁 key → 已授予最大 token）、
 *    选举 `epoch` 与租约编号水位（`m_next_lease`）；
 * 2. 每个**影响单调性**的写（授予新 token、分配新租约编号、epoch 变化、
 *    删除租约）会置 dirty，并在返回前 `Snapshot()` 落盘（`snapshot_on_write`）；
 * 3. 快照损坏（魔数/版本/CRC/长度任一不符）→ 构造期加载失败，`healthy()`
 *    为 false、`load_error()` 非空，且**所有写操作返回 `kUnavailable`**
 *    （fail-stop）。不会静默回退到「从 0 重新计数」——那正是 P1 要修的 bug；
 * 4. 恢复**不**重建租约持有权：旧租约一律视为失效，客户端重新抢锁会拿到
 *    `旧 token + 1`。这样即使协调者重启，下游看到的 token 也严格单调。
 *
 * 快照格式与 fsync 策略见 `src/distributed/coordinator_snapshot.cpp` 顶部注释。
 *
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include "go2cpp/distributed/types.hpp"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace go2cpp::distributed {

/**
 * @brief 协调者配置。
 *
 * `min_ttl`/`max_ttl` 是防御性上限：TTL 太短会让一次调度抖动就丢锁，太长
 * 会让故障发现变慢。默认 10ms~10min，覆盖测试与常见生产区间；调用方需要
 * 更大范围时显式放宽。
 */
struct CoordinatorOptions {
    Duration sweep_interval{10};         ///< 后台清扫周期；<=0 表示不启动清扫线程。
    Duration min_ttl{10};                ///< 允许的最小租约。
    Duration max_ttl{10 * 60 * 1000};    ///< 允许的最大租约（10 分钟）。
    bool start_sweeper{true};            ///< 是否在构造时启动后台清扫线程。
    /// 快照文件路径；空 = 不持久化（重启后 fencing 从 0 开始，仅测试可用）。
    std::string snapshot_path;
    /// true：影响单调性的写返回前立即落盘（fencing 安全的关键，默认开）；
    /// false：只在显式 `Snapshot()` / 析构时落盘——省 IO，但崩溃会丢最后一段。
    bool snapshot_on_write{true};
    /// true：落盘时 `fsync` 文件与目录（崩溃/掉电不丢已确认的 token）。
    bool snapshot_fsync{true};
};

/**
 * @brief 三个原语依赖的协调者抽象。
 *
 * 本地实现是 `Coordinator`；远程实现是 `transport.hpp` 的
 * `CoordinatorClient`（TCP）。所有方法都必须是线程安全的：客户端的心跳
 * 线程与业务线程会并发调用同一个实例。
 *
 * 方法命名以「租约」为中心：注册服务、抢锁、加入选举都返回 `LeaseView`
 * （含 `LeaseId`），续约与释放统一走 `RenewLease` / `ReleaseLease`，避免
 * 三套几乎相同的生命周期接口。
 */
class ICoordinator {
public:
    virtual ~ICoordinator() = default;

    /**
     * @brief 注册服务实例并获得租约。
     * @details 同一 `(service, instance)` 已存在且未过期时：
     *          - 同一 `owner`：原地续约并更新 endpoint（幂等，租约编号不变）；
     *          - 不同 `owner`：视为旧实例已被替换，回收旧租约并新建（编号递增）。
     * @return 成功返回新/更新后的租约视图；参数非法返回 `kInvalidArgument`，
     *         TTL 越界返回 `kInvalidArgument`。
     */
    virtual Result<LeaseView> RegisterService(const ServiceRegistration& registration) = 0;

    /**
     * @brief 按租约编号续约。
     * @param lease 注册/抢锁/加入选举时获得的编号。
     * @param owner 持有者节点 id，必须与授予时一致（防止误续）。
     * @return 续约后的租约视图（剩余时间按协调者时钟计算）。
     *         `kNotFound`：编号不存在；`kNotOwner`：owner 不匹配；
     *         `kExpired`：已经过期，不能复活。
     */
    virtual Result<LeaseView> RenewLease(LeaseId lease, std::string_view owner) = 0;

    /**
     * @brief 主动释放租约（注销服务 / 解锁 / 退出选举）。
     * @return `kOk`；`kNotFound` 或 `kNotOwner` 时状态不变。释放是幂等的：
     *         重复调用不会产生副作用，调用方可以安全地忽略错误。
     */
    virtual Outcome ReleaseLease(LeaseId lease, std::string_view owner) = 0;

    /// @brief 查询服务下所有未过期实例，按实例 id 字典序排序。
    virtual std::vector<ServiceInstance> LookupService(std::string_view service) const = 0;

    /// @brief 返回至少有一个存活实例的服务名，按字典序排序。
    virtual std::vector<std::string> ListServices() const = 0;

    /**
     * @brief 尝试获取分布式锁。
     * @param key 锁名，非空。
     * @param owner 持有者节点 id，非空。
     * @param ttl 租约时长；持有者必须在到期前续约。
     * @return 成功返回租约视图（`fencing` 为本次授予的 token，严格单调递增）；
     *         锁被有效持有返回 `kConflict`（`message` 含当前持有者）。
     */
    virtual Result<LeaseView> AcquireLock(std::string_view key, std::string_view owner,
                                          Duration ttl) = 0;

    /// @brief 查询 key 当前的有效持有者；未持有返回 `std::nullopt`。
    virtual std::optional<LockInfo> QueryLock(std::string_view key) const = 0;

    /// @brief 列出所有有效锁，按 key 字典序排序。
    virtual std::vector<LockInfo> ListLocks() const = 0;

    /**
     * @brief 加入选举并获得候选者租约。
     * @details 同一节点重复加入（未过期）会原地续约并返回同一租约；已经
     *          离开/过期的节点重新加入会拿到更大的租约编号，因此排在候选
     *          队列末尾——这是「先到先得」的公平性来源。
     */
    virtual Result<LeaseView> JoinElection(std::string_view election, std::string_view candidate,
                                           Duration ttl) = 0;

    /// @brief 查询选举状态（候选者按加入顺序排列，过期候选者被过滤）。
    virtual ElectionState QueryElection(std::string_view election) const = 0;

    /// @brief 列出所有选举名（含暂时无候选者的历史 election），按字典序排序。
    virtual std::vector<std::string> ListElections() const = 0;

    /// @brief 立即执行一轮过期清扫，返回本轮剔除的租约数（测试/运维用）。
    virtual std::size_t SweepExpired() = 0;

    /// @brief 当前计数器快照。
    virtual CoordinatorStats Stats() const = 0;
};

/**
 * @brief 进程内权威协调者：租约表 + 服务索引 + 锁索引 + 选举索引。
 *
 * 生命周期：构造即启动清扫线程（`start_sweeper`），析构自动停止并 join。
 * 对象不可拷贝/移动；需要共享时用 `std::shared_ptr` 或引用。
 */
class Coordinator final : public ICoordinator {
public:
    explicit Coordinator(CoordinatorOptions options = {});
    ~Coordinator() override;

    Coordinator(const Coordinator &) = delete;
    Coordinator &operator=(const Coordinator &) = delete;
    Coordinator(Coordinator &&) = delete;
    Coordinator &operator=(Coordinator &&) = delete;

    const CoordinatorOptions &options() const noexcept { return m_options; }

    /// @brief 快照文件路径；未启用快照持久化时为空字符串。
    const std::string &snapshot_path() const noexcept { return m_options.snapshot_path; }

    /**
     * @brief 从 `snapshot_path` 加载 fencing 表、选举 epoch 与租约编号水位。
     *
     * @return
     * - `kOk`：加载成功，或文件不存在（视为全新部署）；调用方继续正常使用；
     * - `kProtocolError`：文件存在但魔数/版本/CRC/长度任一不符——调用方
     *   **必须**停止服务（构造期会置 `healthy() == false`，写操作 fail-stop）；
     * - `kUnavailable`：文件存在但读不了（权限/IO）。
     *
     * ## 定位
     * 单调性的**唯一恢复入口**。加载失败绝不能退化成「空表」：空表意味着
     * 新 token 可能小于重启前的 token，下游会接受过期持有者的写入。
     *
     * ## 步骤
     * step1. 未配置路径 → 直接成功（内存模式）；
     * step2. 读整文件；不存在 → 成功；读失败 → `kUnavailable`；
     * step3. 校验尾部 CRC 后逐条解析（任何越界/未知 kind 都是损坏）；
     * step4. 与内存状态**取较大值**合并（fencing/epoch/next_lease 只升不降）；
     * step5. 清 dirty 并返回。
     *
     * ## 并发/失败语义
     * - 调用约束：构造期或显式重载时调用；内部锁 `m_mutex`，可与读并发；
     * - 失败语义：失败时内存状态**保持调用前**（解析在锁外完成，最后一步
     *   才提交），调用方决定是否降级；
     * - 不变量：成功返回后，任何新授予的 token 都严格大于快照里的值。
     */
    Outcome Restore();

    /**
     * @brief 把 fencing 表、租约表、选举 epoch 原子写入 `snapshot_path`。
     *
     * @return `kOk`（已落盘，或未配置快照）；`kUnavailable`（写盘失败，
     *         内部把 dirty 重新置位，下次写会重试）。
     *
     * ## 定位
     * 单调性的**唯一持久化点**。采用「临时文件 + fsync + rename + fsync 目录」
     * 的原子替换：任何时刻磁盘上要么是旧快照、要么是新快照，不会出现半个文件。
     *
     * ## 步骤
     * step1. 未配置路径 → 成功；降级状态 → `kUnavailable`（不覆盖损坏文件）；
     * step2. 持 `m_snapshot_mutex` 串行化写者，持 `m_mutex` 序列化状态并清 dirty；
     * step3. 写 `<path>.tmp`、按配置 fsync、rename、fsync 目录；
     * step4. 失败则把 dirty 置回并记录 `persist_error()`。
     *
     * ## 并发/失败语义
     * - 调用约束：任意线程；多个写者由 `m_snapshot_mutex` 串行，不阻塞读路径；
     * - 原子性：rename 是 POSIX 原子操作，读者看不到写了一半的快照；
     * - 失败语义：返回错误时磁盘上仍是上一份完整快照，内存状态不受影响。
     */
    Outcome Snapshot();

    /// @brief 构造期加载是否成功（未启用快照恒为 true）。
    bool healthy() const noexcept { return !m_degraded; }

    /// @brief 构造期加载失败的原因；成功时为空。
    const std::string &load_error() const noexcept { return m_load_error; }

    /// @brief 最近一次快照写入失败的原因；成功时为空。
    std::string persist_error() const;

    /// @brief 启动清扫线程（幂等；构造时已按配置启动）。
    void Start();
    /// @brief 停止清扫线程（幂等；停止后仍可同步调用各方法，但不再自动清扫）。
    void Stop();
    bool running() const;

    // ---- ICoordinator ----
    Result<LeaseView> RegisterService(const ServiceRegistration &registration) override;
    Result<LeaseView> RenewLease(LeaseId lease, std::string_view owner) override;
    Outcome ReleaseLease(LeaseId lease, std::string_view owner) override;
    std::vector<ServiceInstance> LookupService(std::string_view service) const override;
    std::vector<std::string> ListServices() const override;
    Result<LeaseView> AcquireLock(std::string_view key, std::string_view owner,
                                  Duration ttl) override;
    std::optional<LockInfo> QueryLock(std::string_view key) const override;
    std::vector<LockInfo> ListLocks() const override;
    Result<LeaseView> JoinElection(std::string_view election, std::string_view candidate,
                                   Duration ttl) override;
    ElectionState QueryElection(std::string_view election) const override;
    std::vector<std::string> ListElections() const override;
    std::size_t SweepExpired() override;
    CoordinatorStats Stats() const override;

private:
    /// 一条租约的内部记录。字段与 `LeaseView` 对应，另存索引所需信息。
    struct LeaseRecord {
        LeaseId id{kInvalidLease};
        LeaseKind kind{LeaseKind::kService};
        std::string name;
        std::string instance;
        std::string owner;
        std::string endpoint;
        TimePoint deadline{};
        Duration ttl{0};
        std::uint64_t fencing{0};  ///< 锁的 token；选举保留 0，任期在 ElectionRecord 里。
        std::uint64_t revision{0};
    };

    /// 一次选举的内部状态。`candidates` 保存 node → lease，用于重算 leader。
    struct ElectionRecord {
        std::string name;
        std::uint64_t epoch{0};
        std::string leader;
        std::unordered_map<std::string, LeaseId> candidates;
    };

    void SweepLoop();
    /// @brief 降级检查：构造期加载失败后拒绝一切写（fail-stop，不静默回退）。
    Outcome EnsureWritable() const;
    /// @brief dirty 时调用 `Snapshot()`；未启用/未脏时零开销返回。
    Outcome PersistAfterWrite();
    /// @brief 序列化当前状态（fencing/租约表/选举 epoch）；调用方必须持有 `m_mutex`。
    std::string SerializeSnapshotLocked() const;
    /// @brief 删除一条租约并清理所有索引；调用方必须已持有 `m_mutex`。
    bool EraseLeaseLocked(LeaseId id, TimePoint now);
    /// @brief 按当前存活候选者重算 leader；leader 变化时递增任期。
    void RecomputeLeaderLocked(ElectionRecord &election, TimePoint now);
    /// @brief 把内部记录转换为对外视图；调用方必须已持有 `m_mutex`。
    LeaseView MakeViewLocked(const LeaseRecord &record, TimePoint now) const;
    /// @brief 仅更新租约期限，返回错误状态（用于续约公共路径）。
    Status RenewRecordLocked(LeaseRecord &record, TimePoint now, std::string *message);
    /// @brief 校验 TTL 是否落在配置区间。
    bool ValidTtl(Duration ttl) const noexcept;
    /// @brief 查找仍有效的锁租约；不存在或已过期都返回 nullptr。
    const LeaseRecord *FindLiveLockLocked(std::string_view key, TimePoint now) const;

    CoordinatorOptions m_options;

    mutable std::mutex m_mutex;
    /// 串行化快照写入；写者之间互斥，读者（`Get`/`Lookup*`）完全不碰它。
    mutable std::mutex m_snapshot_mutex;
    /// 自上次快照以来是否有影响单调性的变更。
    bool m_dirty{false};
    /// 构造期快照加载失败（fail-stop）；true 时一切写操作返回 kUnavailable。
    bool m_degraded{false};
    std::string m_load_error;
    std::string m_persist_error;
    std::unordered_map<LeaseId, LeaseRecord> m_leases;
    /// service → instance → lease id。
    std::unordered_map<std::string, std::unordered_map<std::string, LeaseId>> m_services;
    /// lock key → lease id（可能指向已过期但尚未清扫的记录）。
    std::unordered_map<std::string, LeaseId> m_locks;
    /// election name → 选举状态。
    std::unordered_map<std::string, ElectionRecord> m_elections;
    /// 锁 key → 已授予的最大 fencing token。**永不清除**：清除会让新持有者
    /// 拿到更小的 token，破坏 fencing 的单调性保证。
    std::unordered_map<std::string, std::uint64_t> m_fencing;
    LeaseId m_next_lease{1};
    std::uint64_t m_sweeps{0};
    std::uint64_t m_expired_total{0};

    mutable std::mutex m_sweep_mutex;    std::condition_variable m_sweep_cv;
    std::thread m_sweeper;
    bool m_stop_sweeper{false};
    bool m_sweeper_started{false};
};

} // namespace go2cpp::distributed
