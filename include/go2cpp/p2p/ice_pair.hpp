#pragma once

/**
 * @file ice_pair.hpp
 * @brief ICE 候选对的运行时状态，以及「候选对热切换」（连接迁移）的决策引擎。
 * @details 本模块只做**决策**，不碰 socket：它把「哪条路径可写、哪条路径还在
 *          收包、要不要换路径」抽成可以注入时钟的纯逻辑，因此能被单元测试
 *          以纳秒精度驱动，也不会让 P2PChannel 的收发热路径背上额外耦合。
 *
 *          **为什么需要它**：`P2PChannel` 原本在 `Connect()` 里做完连通性检查
 *          就把 `peer` 锁死在一个地址上；网络切换（WiFi→4G、NAT 重绑定、端口
 *          映射过期）后该地址失效，只能断开重连。WebRTC 的做法是**同一连接内
 *          换候选对**（`p2p/base/basic_ice_controller.cc` 的
 *          `SelectConnection`/`ShouldSwitchConnection`，`p2p/base/connection.h`
 *          的 `writable()`/`receiving()`），上层传输（DTLS/SCTP/媒体流）完全
 *          不感知。本文件实现同样的两个状态与切换判定。
 *
 *          **两个状态的语义**（与 WebRTC `Connection::writable()`/
 *          `Connection::receiving()` 对齐）：
 *
 *          | 状态 | 判定 | 含义 | 用途 |
 *          |------|------|------|------|
 *          | `writable` | 该对**曾收到过**成功的 STUN 检查响应（或对端检查请求） | 发出的包能到达对端 | 有资格被选为切换目标 |
 *          | `receiving` | 最近 `no_receive_timeout` 内收到过该对的任意包 | 对端的包还能回来 | 判定「当前对已死」 |
 *
 *          **切换判定**（`Evaluate()`）：
 *          1. 当前选定对 `receiving == true` → 不动（可选回切，默认关闭）；
 *          2. 当前选定对 `receiving == false` → 切到**优先级最高的 writable 对**；
 *          3. 切换受 `switch_cooldown` 约束，冷却期内只报告 `kCooldownBlocked`，
 *             不改选定对（防止抖动）；
 *          4. 选定对刚被切换时有一个 `no_receive_timeout` 的宽限期
 *             （`selected_since_ns` 参与 receiving 起点计算），否则新对会因为
 *             「切换瞬间还没收到包」立刻又被判死，形成切来切去的振荡。
 *
 * @author go2cpp contributors
 * @date 2026-11-08
 * @copyright MIT License
 */

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>

#include "go2cpp/p2p/endpoint.hpp"

namespace go2cpp::p2p {

/// 候选对状态（RFC 8445 §6.1.2.6 的状态子集 + 本实现的诊断语义）。
enum class IcePairState : std::uint8_t {
    kWaiting = 0,     ///< 尚未探测过。
    kInProgress = 1,  ///< 已发出检查，尚未收到响应。
    kSucceeded = 2,   ///< 至少一次检查成功（`writable == true`）。
    kFailed = 3,      ///< 连续失败达到阈值且从未成功（降频重试，不永久放弃）。
};

/// 状态名（日志/测试用）。
const char* IcePairStateName(IcePairState state) noexcept;

/**
 * @brief 热切换参数。
 *
 * @details 默认值按「WebRTC 量级 + 移动网络抖动」取值：WebRTC 用
 *          `Connection::receiving_timeout_`（由 ping 周期推导，量级 1s）判定
 *          receiving；这里默认取 1s，并把回切默认关掉（WebRTC 的
 *          `basic_ice_controller` 也不主动回切到优先级更高的旧对，
 *          否则弱网下会因为「时通时不通」而反复换路径）。
 */
struct IceSwitchoverConfig {
    /// 总开关；关闭时 `Evaluate()` 恒返回 `kNone`，P2PChannel 不发任何探测包。
    bool enabled = true;
    /**
     * @brief 「不接收」阈值：超过这么久没收到该对的任何包就判 `receiving == false`。
     * @details 0 表示禁用该判定（永不因超时切换）；负值按 0 处理。
     */
    std::chrono::milliseconds no_receive_timeout{1000};
    /// 是否允许回切到优先级更高的旧对（原对恢复后）。默认关闭以避免抖动。
    bool allow_switch_back = false;
    /// 切换冷却期：两次切换之间的最小间隔；0 表示不限制。
    std::chrono::milliseconds switch_cooldown{2000};
    /// 主动探测间隔：周期性向所有已注册对发 STUN 检查，用来刷新 writable/receiving。
    std::chrono::milliseconds probe_interval{500};
    /// 连续失败达到该次数且从未成功 → `kFailed`（探测降频）。0 表示不降级。
    std::uint32_t max_check_failures = 3;
    /// 失败对的探测间隔倍数（>=1），避免对黑洞地址持续打满。
    std::uint32_t failed_probe_backoff = 2;
    /// 最多注册的候选对数；超出后 `AddPair()` 返回 false。
    std::size_t max_pairs = 32;
};

/// 一次评估的结论。
enum class IceSwitchDecision : std::uint8_t {
    kNone = 0,             ///< 无需动作（当前对健康，或功能关闭）。
    kSwitch = 1,           ///< 当前对已死，切到另一条可写对。
    kSwitchBack = 2,       ///< 回切到恢复了的更优旧对（需 `allow_switch_back`）。
    kCooldownBlocked = 3,  ///< 本应切换，但冷却期未过；选定对**未**改变。
    kNoCandidate = 4,      ///< 当前对已死且没有可写的备用对（只能等）。
};

/// 结论名（日志/测试用）。
const char* IceSwitchDecisionName(IceSwitchDecision decision) noexcept;

/// 候选对的只读快照（`receiving` 已按传入的 `now_ns` 求值）。
struct IcePairSnapshot {
    Endpoint local;              ///< 本地端点（候选）。
    Endpoint remote;             ///< 对端端点（候选）。
    std::uint64_t priority = 0;  ///< 组合优先级，越大越优。
    IcePairState state = IcePairState::kWaiting;
    bool writable = false;   ///< 能发：该对曾成功完成检查。
    bool receiving = false;  ///< 能收：最近 `no_receive_timeout` 内收到过包。
    bool selected = false;   ///< 是否是当前选定对。
    std::int64_t last_receive_ns = 0;   ///< 最近收包时刻（steady_clock 纳秒；0=从未）。
    std::int64_t selected_since_ns = 0; ///< 被选为当前对的时刻（0=从未被选）。
    std::uint64_t checks_sent = 0;
    std::uint64_t checks_succeeded = 0;
    std::uint64_t checks_failed = 0;
    double rtt_ms = 0.0;  ///< 检查响应 RTT（EWMA；0=无样本）。
};

/// 一次 `Evaluate()` 的结果（同时描述「实际发生了什么」）。
struct IceSwitchEvent {
    IceSwitchDecision decision = IceSwitchDecision::kNone;
    Endpoint from;  ///< 切换前选定对的远端；`kNone`/`kNoCandidate` 时可能无效。
    Endpoint to;    ///< 切换后选定对的远端；仅 `kSwitch`/`kSwitchBack` 有效。
    std::uint64_t generation = 0;  ///< 切换后的代数（每次真正切换 +1）。
    const char* reason = "";       ///< 静态字符串，生命周期 = 程序（不分配）。
};

/**
 * @brief 候选对集合 + 热切换决策器。
 *
 * @details 线程模型：所有公开方法线程安全（内部互斥量）。设计上只应由
 *          「接收线程」驱动 `Evaluate()`/`OnXxx()`；`Snapshot()`/`Selected()`
 *          可从任意线程调用。互斥量与数据同处一个 cacheline 组，未竞争时
 *          一次加解锁约 20ns，相对一次 UDP 收发的 ~1μs 可忽略。
 *
 *          状态机不变量：
 *          - `writable == true` 蕴含 `state == kSucceeded`；
 *          - 选定对的 `selected_since_ns != 0`；
 *          - `generation()` 只增不减，且只在 `kSwitch`/`kSwitchBack` 时递增。
 */
class IcePairController {
public:
    explicit IcePairController(IceSwitchoverConfig config = IceSwitchoverConfig());

    IcePairController(const IcePairController&) = delete;
    IcePairController& operator=(const IcePairController&) = delete;

    /**
     * @brief 注册一条候选对。
     * @param local  [in] 本地端点；必须有效。
     * @param remote [in] 对端端点；必须有效，且是切换时 `peer` 的取值。
     * @param priority [in] 组合优先级（RFC 8445 §6.1.2.3），越大越优。
     * @return true 注册成功；false 端点无效 / 重复 / 超过 `max_pairs`。
     * @note 重复注册（local+remote 相同）是幂等的失败，不改变已有状态。
     */
    bool AddPair(const Endpoint& local, const Endpoint& remote, std::uint64_t priority);

    /// 清空所有候选对与选定状态（重新收集候选时调用）。`generation()` 不变。
    void Clear() noexcept;

    /**
     * @brief 显式选定一条已注册的对（连接建立时调用）。
     * @return true 已选定；false 该对未注册。
     * @note 这是「建链」而非「切换」：不增加 `generation()`，不更新冷却期。
     */
    bool Select(const Endpoint& remote, std::int64_t now_ns);

    /// 记录在该对上收到任意包（数据/检查请求/检查响应）。
    void OnPacketReceived(const Endpoint& remote, std::int64_t now_ns);

    /// 记录向该对发出一次检查（更新探测调度与 `checks_sent`）。
    void OnCheckSent(const Endpoint& remote, std::int64_t now_ns);

    /// 记录该对一次检查成功：标记 writable + receiving，更新 RTT。
    void OnCheckSucceeded(const Endpoint& remote, std::int64_t now_ns, double rtt_ms = 0.0);

    /// 记录该对一次检查失败（仅统计；`kFailed` 只在从未成功且连续失败时置位）。
    void OnCheckFailed(const Endpoint& remote, std::int64_t now_ns);

    /**
     * @brief 评估是否应切换，并在 `kSwitch`/`kSwitchBack` 时更新选定对。
     *
     * @param now_ns [in] 当前 `steady_clock` 纳秒时间戳（调用方取一次时钟）。
     *
     * @return 本次评估的事件：
     * - `kSwitch`/`kSwitchBack`：**已经**改选定对，调用方必须把 `peer` 更新成 `to`；
     * - `kCooldownBlocked`：想切但被冷却期挡住，选定对**未**变——调用方不要改 peer；
     * - `kNoCandidate`：当前对已死但没有可写备用对——调用方应继续等待，不要断链；
     * - `kNone`：无需动作。
     *
     * ## 定位
     * 这是「网络切换不重建连接」的唯一决策点。判错会导致：切换过于灵敏 →
     * 弱网下路径抖振（每包重传都换地址，对端全丢）；过于迟钝 → 网络已换
     * 但流量仍走死路（表现为「假死但 TCP 没断」）。
     *
     * ## 步骤
     * step1. 功能关闭 / 未选定 → `kNone`；
     * step2. 求当前对的有效 receiving（收包时刻与选定时刻取较大者作起点）；
     * step3. 当前对仍在收包：仅在允许回切、存在「可写且在收包且优先级更高」的
     *        旧对、且冷却期已过时回切，否则不动；
     * step4. 当前对已死：找优先级最高的可写对；没有 → `kNoCandidate`；
     * step5. 冷却期未过 → `kCooldownBlocked`（**不**改选定对）；
     * step6. 更新选定对、重置宽限期起点、`generation+1`、记录冷却期起点。
     *
     * ## 并发/失败语义
     * - 调用约束：内部加锁，可从任意线程调用；但由单一接收线程驱动才有时序意义。
     * - 原子性：判定与选定对更新在同一临界区内完成，不会出现「报告切了但没切」。
     * - 失败语义：不抛异常；`noexcept` 语义之外的分配失败由调用方（vector）
     *   自然传播，内部不做兜底。
     * - 不变量：返回 `kSwitch`/`kSwitchBack` 后 `Selected()` 必然等于 `event.to`。
     */
    IceSwitchEvent Evaluate(std::int64_t now_ns);

    /// 当前选定对的快照；未选定返回 `nullopt`。
    std::optional<IcePairSnapshot> Selected(std::int64_t now_ns) const;

    /// 指定远端是否已注册为候选对（不要求可写）；与 `HasPair` 同名语义，
    /// 保留两个名字是让调用点读起来更接近意图（「这是不是已知的路径」）。
    bool IsKnownPair(const Endpoint& remote) const { return HasPair(remote); }

    /// 指定远端是否已注册为候选对（不要求可写）。
    bool HasPair(const Endpoint& remote) const;

    /// 指定远端是否是「可写」的候选对（曾成功完成检查）。
    bool IsWritable(const Endpoint& remote) const;

    /// 某条对的快照（`receiving` 按 `now_ns` 求值）；不存在返回 `nullopt`。
    std::optional<IcePairSnapshot> Pair(const Endpoint& remote, std::int64_t now_ns) const;

    /// 全部候选对快照，按优先级降序（同优先级按注册顺序）。
    std::vector<IcePairSnapshot> Snapshot(std::int64_t now_ns) const;

    /**
     * @brief 现在该探测哪些对（已按优先级降序）。
     * @details 只返回「距上次探测超过 `probe_interval`」的对；失败对按
     *          `failed_probe_backoff` 降频。调用方只管发包，探测调度由这里决定。
     */
    std::vector<Endpoint> DueProbeTargets(std::int64_t now_ns);

    /// 已注册的候选对数量。
    std::size_t size() const;

    /// 切换代数（每次真正切换 +1；显式 `Select()` 不增加）。
    std::uint64_t generation() const;

    /// 配置快照（只读）。
    IceSwitchoverConfig config() const;

private:
    struct PairRecord {
        Endpoint local;
        Endpoint remote;
        std::uint64_t priority = 0;
        IcePairState state = IcePairState::kWaiting;
        bool writable = false;
        std::int64_t last_receive_ns = 0;
        std::int64_t last_probe_ns = 0;
        std::int64_t selected_since_ns = 0;
        std::uint64_t checks_sent = 0;
        std::uint64_t checks_succeeded = 0;
        std::uint64_t checks_failed = 0;
        double rtt_ms = 0.0;
    };

    static constexpr std::size_t kNoSelection = static_cast<std::size_t>(-1);

    // 以下 Locked 版本要求调用方已持锁。
    std::size_t FindLocked(const Endpoint& remote) const noexcept;
    bool IsReceivingLocked(const PairRecord& record, std::int64_t now_ns) const noexcept;
    std::size_t BestWritableLocked(std::size_t exclude, bool require_receiving,
                                   std::int64_t now_ns) const noexcept;
    IcePairSnapshot MakeSnapshotLocked(const PairRecord& record, std::size_t index,
                                       std::int64_t now_ns) const noexcept;
    IceSwitchEvent EvaluateLocked(std::int64_t now_ns);

    mutable std::mutex m_mutex;
    IceSwitchoverConfig m_config;
    std::vector<PairRecord> m_pairs;
    std::size_t m_selected = kNoSelection;
    std::int64_t m_last_switch_ns = 0;
    std::uint64_t m_generation = 0;
};

}  // namespace go2cpp::p2p
