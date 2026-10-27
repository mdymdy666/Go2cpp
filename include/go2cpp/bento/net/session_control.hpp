#pragma once

/**
 * @file session_control.hpp
 * @brief 通用会话控制权仲裁：View/Interact 权限、唯一控制器、租约回收与变更广播。
 * @details 背景：JinShan 的「1 被控 : N 主控」要求**同时可看、仅一人可控**
 *          （FR-03 / FR-06 / RISK-024 / AX-036）。`PeerHub` 只解决「一个
 *          socket 服务 N 个 peer」的路由问题，不表达「谁能操作」。本组件
 *          把控制权仲裁做成**与传输层解耦的通用会话权限模型**：
 *
 *          - 每个参与者有一个 `ControlMode`：`kNone`（未加入/无权限）、
 *            `kView`（只能看）、`kInteract`（唯一控制器，可操作）。
 *          - **唯一控制器**：任一时刻最多一个 `kInteract`，由
 *            `ControlPolicy` 决定新请求的行为：
 *            `kFirstCome`（先到先得，已有控制器时拒绝后来者）或
 *            `kPreempt`（后来者抢占，原控制器降为 `kView`）。
 *          - **租约**：控制器持有 `lease_ms` 的租约，期间需 `Renew` 续租
 *            （典型接法：每次真实输入事件调用一次）。`Tick` 发现过期即
 *            回收为 `kView`，避免主控崩溃后控制权永久占用（AX-037）。
 *          - **断开回收**：`Leave` / `OnPeerDisconnected` 立即回收该 peer
 *            的控制权；`PeerHub::on_peer_disconnected` 可直接接入。
 *          - **变更广播**：每次状态变化生成 `ControlChange` 并经 `on_change`
 *            同步回调；`Snapshot().ToJson()` 给出可发给所有 peer 的完整
 *            视图（谁在看、谁在控、generation）。
 *
 *          为什么不在 SessionControl 里直接发网络消息：
 *          - 仲裁是纯状态机，可单测、可复用（RDP attendee、VNC、Web 协作
 *            都是同一语义）；发送走 `PeerHub::SendStream(peer_id, ...)`
 *            由应用遍历 `PeerIds()` 完成，避免把 socket 依赖塞进状态机。
 *
 *          线程模型：所有公开方法线程安全（内部 `std::mutex`）。回调
 *          `on_change` 在**释放锁之后**调用，回调里可以再次调用本类接口
 *          （例如收到变更后查询 Snapshot），不会自锁。`on_change` 本身
 *          建议在初始化阶段设置；运行期换回调需外部保证不与回调并发。
 *
 * @author go2cpp contributors
 * @date 2026-10-27
 * @copyright MIT License
 */

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace go2cpp::bento {

/// @brief 参与者的控制权限。数值有序：None < View < Interact。
enum class ControlMode : std::uint8_t {
    kNone = 0,      ///< 未加入会话或已被移除（无任何权限）。
    kView = 1,      ///< 只能接收画面/事件，不能操作。
    kInteract = 2,  ///< 唯一控制器，可操作（同一时刻最多一个）。
};

/// @brief 模式名（"none"/"view"/"interact"）。
const char* ControlModeName(ControlMode mode) noexcept;

/// @brief 控制权冲突策略。
enum class ControlPolicy : std::uint8_t {
    kFirstCome = 0,  ///< 先到先得：已有控制器时新请求被拒绝。
    kPreempt = 1,    ///< 可抢占：新请求获胜，原控制器降为 kView。
};

/// @brief 状态变化原因（用于日志与广播）。
enum class ControlChangeReason : std::uint8_t {
    kJoined = 0,       ///< 参与者加入（默认 View，或配置为 Interact）。
    kLeft = 1,         ///< 参与者主动离开（应用调用 Leave）。
    kGranted = 2,      ///< 显式 Grant 授予/降级。
    kRequested = 3,    ///< RequestControl 成功获得控制权。
    kPreempted = 4,    ///< 被其他参与者抢占，降为 View。
    kRevoked = 5,      ///< 管理员 Revoke 撤销控制权。
    kDisconnected = 6, ///< peer 断开导致回收（OnPeerDisconnected）。
    kLeaseExpired = 7, ///< 租约过期自动回收。
};

/// @brief 原因名（"joined"/"left"/...，稳定字符串，可做指标标签）。
const char* ControlChangeReasonName(ControlChangeReason reason) noexcept;

/// @brief 仲裁器配置。
struct SessionControlConfig {
    ControlPolicy policy = ControlPolicy::kFirstCome;  ///< 冲突策略。
    /// 控制租约（毫秒）；0 = 不过期（仅调试/单测建议）。
    std::uint32_t lease_ms = 30000;
    /// 新参与者 Join 时的默认模式（通常是 kView）。
    ControlMode default_mode = ControlMode::kView;
    /// 最大参与者数；超限 Join 返回 false，防止 peer 表被撑爆。
    std::size_t max_participants = 64;
};

/// @brief 一次控制权变更事件。
struct ControlChange {
    std::uint32_t peer_id = 0;                          ///< 发生变化的 peer。
    ControlMode from = ControlMode::kNone;              ///< 变化前模式。
    ControlMode to = ControlMode::kNone;                ///< 变化后模式。
    ControlChangeReason reason = ControlChangeReason::kGranted;  ///< 原因。
    std::uint32_t controller = 0;                       ///< 变化后的唯一控制器（0=无）。
    std::uint64_t generation = 0;                       ///< 全局变更代数（单调递增）。
};

/// @brief 参与者快照项（供广播/状态页）。
struct ControlParticipant {
    std::uint32_t peer_id = 0;
    ControlMode mode = ControlMode::kNone;
    bool controller = false;  ///< 是否当前唯一控制器。
    bool revoked = false;     ///< 是否被 Revoke 禁止再次请求（需 Grant 解除）。
};

/// @brief 控制权状态快照。
struct ControlSnapshot {
    std::uint32_t controller = 0;       ///< 唯一控制器 peer_id；0 = 无人控制。
    std::uint64_t generation = 0;       ///< 变更代数。
    std::uint32_t lease_remaining_ms = 0;  ///< 控制器租约剩余；0 = 无租约/无控制器。
    std::vector<ControlParticipant> participants;  ///< 按 peer_id 升序。

    /// @brief 导出单行 JSON（可直接作为广播消息体）。
    std::string ToJson() const;
};

/// @brief 仲裁器计数器快照。
struct SessionControlStats {
    std::uint64_t requests = 0;           ///< RequestControl 调用次数。
    std::uint64_t grants = 0;             ///< 成功 Grant 次数。
    std::uint64_t denials = 0;            ///< 请求被拒次数（先到先得/撤销）。
    std::uint64_t revocations = 0;        ///< 撤销次数。
    std::uint64_t preemptions = 0;        ///< 抢占次数。
    std::uint64_t lease_expirations = 0;  ///< 租约过期回收次数。
    std::uint64_t disconnects = 0;        ///< 断开回收次数。
    std::uint64_t changes = 0;            ///< 变更事件总数（与 generation 同步）。
};

/// @brief 通用会话控制权仲裁器（线程安全）。
class SessionControl final {
   public:
    explicit SessionControl(SessionControlConfig config = {});

    SessionControl(const SessionControl&) = delete;
    SessionControl& operator=(const SessionControl&) = delete;

    /// @brief 加入会话，按 default_mode 授予初始权限。
    /// @return peer_id 为 0、已存在、或超过 max_participants 返回 false。
    bool Join(std::uint32_t peer_id);

    /// @brief 主动离开；若其正持有控制权则自动回收。
    bool Leave(std::uint32_t peer_id);

    /// @brief peer 断开（PeerHub::on_peer_disconnected 接入点）；语义同 Leave。
    bool OnPeerDisconnected(std::uint32_t peer_id);

    /// @brief 授予 kView（同时解除 Revoke 的禁入标记）。
    bool Grant(std::uint32_t peer_id);

    /// @brief 显式设置权限（不存在则创建参与者）。
    /// @details `kInteract` 是管理操作：直接抢占现有控制器（发 kPreempted
    ///          事件给原控制器），不受 `policy` 限制。`kNone` 等价于收回
    ///          权限但保留参与者记录。
    bool Grant(std::uint32_t peer_id, ControlMode mode);

    /// @brief 撤销当前控制器的控制权，并禁止其再次请求（需 Grant 解除）。
    /// @return 当前没有控制器返回 false。
    bool Revoke();

    /// @brief 撤销指定 peer 的控制权并禁止其再次请求。
    /// @return peer 不存在返回 false；幂等（已撤销时返回 true 不重复回调）。
    bool Revoke(std::uint32_t peer_id);

    /// @brief 请求控制权（kInteract）。
    /// @details 无控制器时直接成功；有控制器时按 policy 拒绝或抢占。
    ///          被 Revoke 的 peer 在 Grant 解除前一律失败。
    /// @return 是否获得控制权。
    bool RequestControl(std::uint32_t peer_id);

    /// @brief 续租（典型接法：收到输入事件时调用）。非控制器返回 false。
    bool Renew(std::uint32_t peer_id);

    /// @brief 查询 peer 的权限；不存在返回 kNone。
    ControlMode mode(std::uint32_t peer_id) const;
    /// @brief 是否当前唯一控制器。
    bool is_controller(std::uint32_t peer_id) const;
    /// @brief 是否被 Revoke 禁止请求。
    bool revoked(std::uint32_t peer_id) const;
    /// @brief 当前唯一控制器 peer_id；0 = 无人控制。
    std::uint32_t controller() const;
    bool has_controller() const;
    /// @brief 全部参与者 peer_id（升序）。
    std::vector<std::uint32_t> participants() const;
    /// @brief 状态快照（含租约剩余，便于广播）。
    ControlSnapshot Snapshot() const;
    /// @brief 变更代数（每次状态变化 +1）。
    std::uint64_t generation() const;
    /// @brief 计数器快照。
    SessionControlStats stats() const;

    /// @brief 用 NowMs() 检查租约是否过期；返回是否发生回收。
    bool Tick();
    /// @brief 用调用方时钟检查租约（测试可注入）。
    bool Tick(std::uint64_t now_ms);

    /// @brief 变更回调（释放内部锁后同步调用）。
    /// @note 建议初始化阶段设置；运行期替换需外部保证不与回调并发。
    std::function<void(const ControlChange&)> on_change;

   private:
    struct Entry {
        ControlMode mode = ControlMode::kNone;
        bool revoked = false;
    };

    /// 持锁更新状态并追加一条变更事件；返回是否真的发生变化。
    void AppendChange(std::vector<ControlChange>* out, std::uint32_t peer_id, ControlMode from,
                      ControlMode to, ControlChangeReason reason);
    /// 移除参与者并回收其控制权（Leave / OnPeerDisconnected 共用）。
    bool RemoveParticipant(std::uint32_t peer_id, ControlChangeReason reason);
    void Dispatch(const std::vector<ControlChange>& changes);

    SessionControlConfig m_config;
    std::map<std::uint32_t, Entry> m_participants;
    std::uint32_t m_controller = 0;
    std::uint64_t m_lease_deadline_ms = 0;
    std::uint64_t m_generation = 0;
    SessionControlStats m_stats;
    mutable std::mutex m_mutex;
};

}  // namespace go2cpp::bento
