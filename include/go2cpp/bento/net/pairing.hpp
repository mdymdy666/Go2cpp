#pragma once

/**
 * @file pairing.hpp
 * @brief 通用配对协议（MOD-13）：Register/Connect/Response 帧、密钥指纹校验、
 *        单次状态迁移与 15s 超时回滚。
 * @details 背景：JinShan 的 Viewer（主控）与 Host（被控）在建立会话前必须先
 *          确认「双方拿的是同一个邀请密钥」，否则不能进入 RDP/媒体链路
 *          （FR-01 / RISK-013 / AX-009）。本组件只做**与传输层解耦的配对
 *          状态机 + 32B 帧编解码**，不碰 socket、不解析 RDP：
 *
 *          - 服务端 `Register(key)`：登记本端密钥指纹，等待 Connect；
 *          - 客户端 `Connect(key)`：发送 CONNECT（携带密钥指纹与 nonce）；
 *          - 服务端收到 CONNECT 后由应用调用 `Response(accept)` 显式接受或
 *            拒绝（这样鉴权策略、人工确认、限流都可以插在中间）；
 *          - 接受后双方进入 `kPaired`；客户端 `Activate()` 发出 CONFIRM，
 *            服务端收到 CONFIRM 才进入 `kActive`（RISK-013：**以收到对端
 *            确认为唯一成功条件**，避免 response 丢失时两侧状态不一致）；
 *          - 任一环节超过 15s 未完成 → `kPeerTimeout` 并回到 Closed，由应用
 *            执行 Reset/重试（RISK-013 的「回滚并重试」）。
 *
 *          状态机（单次迁移，Closed 是终态；只有 Reset 能回到 Idle）：
 *
 *              Idle ──Register/Connect──▶ Pending
 *              Pending ──Response(accept)/收到响应──▶ Paired
 *              Paired ──CONFIRM + 对端 ACK──▶ Active
 *              Pending/Paired ──超时──▶ Closed
 *              Active ──应用 Close──▶ Closed（不因等待超时关闭）
 *
 *          帧格式（固定 32B，网络字节序 / 大端）：
 *
 *          | 偏移 | 字段     | 类型 | 语义                                  |
 *          |------|----------|------|---------------------------------------|
 *          | 0    | magic    | u32  | 'B','P','A','R' = 0x42504152          |
 *          | 4    | version  | u8   | 协议版本，当前 1                       |
 *          | 5    | type     | u8   | REGISTER/CONNECT/RESPONSE/CONFIRM      |
 *          | 6    | flags    | u16  | 保留，必须 0（为扩展留位）              |
 *          | 8    | key_hash | u64  | 邀请密钥指纹（不是密钥本身）            |
 *          | 16   | nonce    | u64  | 客户端随机 nonce，绑定一次配对尝试      |
 *          | 24   | pair_id  | u32  | 服务端分配的配对 ID（RESPONSE 回显）    |
 *          | 28   | status   | u16  | RESPONSE 结果：0=接受，否则为失败码     |
 *          | 30   | reserved | u16  | 保留，必须 0                           |
 *
 *          为什么 key_hash 是 8 字节指纹而不是密钥：CONNECT 会在网络上
 *          明文出现，绝不能携带密钥。8 字节指纹仅用于「快速判断是否同一
 *          邀请」，可被暴力枚举弱密钥，因此**真正的认证/机密性必须由
 *          MOD-05 的 PSK AEAD 安全通道提供**；对高安全场景可注入自定义
 *          `PairingAuthPolicy`（例如 HMAC-SHA256 截断）替换默认的
 *          `FixedKeyAuthPolicy`（FNV-1a 64，非密码学摘要）。
 *
 *          重试语义：CONNECT/RESPONSE/CONFIRM 丢失时不需要新建配对——
 *          `Tick` 每 `retransmit_ms` 重发最后一帧；服务端在 Paired/Active
 *          状态收到同一 nonce 的重复 CONNECT 会**幂等重放同一 pair_id 的
 *          RESPONSE**，收到重复 CONFIRM 会重放 ACK；客户端重复收到
 *          RESPONSE 只忽略/重放 CONFIRM。这样 response 丢失不会产生第二个
 *          会话（RISK-013 的「无重复会话」）。
 *
 *          线程模型：`PairingSession` 是**单线程状态机**，与 `Link` /
 *          `Channel` 一样由调用方串行驱动（`Tick` / `HandleFrame` 不可
 *          并发调用）。若需要多连接配对管理，为每条连接创建一个实例，
 *          由 I/O 线程各自持有。
 *
 * @note 回调 `on_state_change` 在状态变更方法返回前同步调用；回调内可
 *       读取状态（state/error/pair_id），但**不得再次调用修改方法**，
 *       否则会破坏单次迁移约束。
 * @author go2cpp contributors
 * @date 2026-10-28
 * @copyright MIT License
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>

namespace go2cpp::bento {

/// 配对协议版本；任何 version 不符的帧在解析/处理阶段被拒绝。
inline constexpr std::uint8_t kPairingVersion = 1;

/// 配对帧固定字节数（单包，无需分片）。
inline constexpr std::size_t kPairingFrameBytes = 32;

/// 配对帧 magic："BPAR"。
inline constexpr std::uint32_t kPairingMagic = 0x42504152U;

/// 默认配对超时（毫秒）。AX-029 / RISK-013 规定的 15s。
inline constexpr std::uint32_t kPairingTimeoutMs = 15000;

/// 默认重传间隔（毫秒）。
inline constexpr std::uint32_t kPairingRetransmitMs = 1000;

/// 发送队列上限；超过时丢弃最旧帧，防止应用忘记取帧导致无界增长。
inline constexpr std::size_t kPairingMaxOutbound = 64;

/// @brief 配对帧类型。
enum class PairingFrameType : std::uint8_t {
    kRegister = 0x01,  ///< 服务端登记（本地 API 使用，通常不上线）。
    kConnect = 0x02,   ///< 客户端 → 服务端：请求配对。
    kResponse = 0x03,  ///< 服务端 → 客户端：接受/拒绝 + pair_id。
    kConfirm = 0x04,   ///< 客户端 → 服务端：确认收到响应（唯一成功条件）。
};

/// @brief 配对角色。
enum class PairingRole : std::uint8_t {
    kNone = 0,    ///< 尚未调用 Register/Connect。
    kServer = 1,  ///< 被控端（Host）：Register 后等待 Connect。
    kClient = 2,  ///< 主控端（Viewer）：Connect 后等待 Response。
};

/// @brief 配对状态。合法迁移见文件头状态机；Closed 只能经 Reset 离开。
enum class PairingState : std::uint8_t {
    kIdle = 0,     ///< 未开始。
    kPending = 1,  ///< 已发起/已登记，等待对端或本端应用决策。
    kPaired = 2,   ///< 双方已确认密钥一致，等待激活确认。
    kActive = 3,   ///< 配对完成，可进入传输/会话建立。
    kClosed = 4,   ///< 失败或主动关闭；需 Reset 后重试。
};

/// @brief 配对失败码（也作为 RESPONSE 帧的 status 上线）。
enum class PairingError : std::uint8_t {
    kNone = 0,              ///< 成功/无错误。
    kKeyMismatch = 1,       ///< 密钥指纹不匹配（密钥不匹配拒绝）。
    kPeerTimeout = 2,       ///< 15s 内未完成配对（响应丢失/对端无响应）。
    kAlreadyRegistered = 3, ///< 重复 Register/Connect（本端已有配对尝试）。
    kNotRegistered = 4,     ///< 服务端未 Register 就收到 CONNECT。
    kAlreadyPaired = 5,     ///< 同 nonce 的重复请求（通常幂等处理，不外抛）。
    kInvalidState = 6,      ///< 当前状态不允许该操作（例如客户端调用 Response）。
    kInvalidArgument = 7,   ///< 空 key、空策略等参数错误。
    kProtocolVersion = 8,   ///< 帧版本不符。
    kMalformedFrame = 9,    ///< 帧结构非法（magic/flags/保留位/status 越界）。
    kRejected = 10,         ///< 服务端/策略拒绝（密钥正确但应用不接受）。
    kClosed = 11,           ///< 会话已关闭，需 Reset。
};

/// @brief 状态名（稳定小写字符串，可做日志/指标标签）。
const char* PairingStateName(PairingState state) noexcept;

/// @brief 角色名（"none"/"server"/"client"）。
const char* PairingRoleName(PairingRole role) noexcept;

/// @brief 失败码名（稳定小写字符串，可做指标标签）。
const char* PairingErrorName(PairingError error) noexcept;

/// @brief 帧类型名（"register"/"connect"/"response"/"confirm"）。
const char* PairingFrameTypeName(PairingFrameType type) noexcept;

/// @brief 判断失败码是否在已定义范围内。
bool IsKnownPairingError(PairingError error) noexcept;

/// @brief 一帧配对消息（与传输层无关的纯值类型）。
struct PairingFrame {
    PairingFrameType type = PairingFrameType::kConnect;      ///< 帧类型。
    std::uint8_t version = kPairingVersion;                  ///< 协议版本。
    std::uint64_t key_hash = 0;                              ///< 邀请密钥指纹。
    std::uint64_t nonce = 0;                                 ///< 客户端 nonce。
    std::uint32_t pair_id = 0;                               ///< 服务端配对 ID。
    PairingError status = PairingError::kNone;               ///< RESPONSE 结果。
};

/// @brief 编码 32B 配对帧（大端）。`out` 必须至少 kPairingFrameBytes 字节。
void EncodePairingFrame(const PairingFrame& frame, std::uint8_t* out) noexcept;

/// @brief 解码并校验配对帧。
/// @param data 输入缓冲（允许尾部有多余字节，为未来扩展保留）。
/// @param size 输入长度，至少 kPairingFrameBytes。
/// @param out 成功时写入解码结果。
/// @return magic/version/type/flags/保留位/status 任一非法返回 false。
bool DecodePairingFrame(const std::uint8_t* data, std::size_t size,
                        PairingFrame* out) noexcept;

/// @brief 可替换的密钥指纹策略（module-map 的 IAuthPolicy 接缝）。
/// @details 默认实现 `FixedKeyAuthPolicy` 用 FNV-1a 64 做快速指纹；对
///          安全要求高的部署可换成 HMAC-SHA256 截断实现。配对协议只比较
///          `HashKey` 的结果，不接触明文密钥的比较逻辑。
class PairingAuthPolicy {
   public:
    virtual ~PairingAuthPolicy() = default;

    /// @brief 计算密钥指纹。空 key 应返回 false（由调用方转 kInvalidArgument）。
    virtual bool HashKey(const std::string& key, std::uint64_t* out) const = 0;

    /// @brief 用策略自身重算指纹并比较（便于应用验证原始 key）。
    virtual bool VerifyKey(const std::string& key, std::uint64_t expected_hash) const {
        std::uint64_t actual = 0;
        return HashKey(key, &actual) && actual == expected_hash;
    }
};

/// @brief 固定密钥指纹策略：FNV-1a 64 + 长度混合（非密码学摘要）。
class FixedKeyAuthPolicy final : public PairingAuthPolicy {
   public:
    bool HashKey(const std::string& key, std::uint64_t* out) const override;

    /// @brief 静态入口，便于测试/诊断直接取指纹。
    static std::uint64_t Hash(const std::string& key) noexcept;
};

/// @brief 配对配置。
struct PairingConfig {
    /// 配对超时（毫秒）；<=0 时钳到 1，保证 Tick 有明确截止时间。
    std::uint32_t timeout_ms = kPairingTimeoutMs;
    /// 重传间隔（毫秒）；0 表示每个 Tick 都重传（仅测试/诊断建议）。
    std::uint32_t retransmit_ms = kPairingRetransmitMs;
    /// 客户端 nonce；0 = 自动生成（时间 + 进程内计数器），非 0 = 固定值（测试/复现）。
    std::uint64_t nonce = 0;
    /// 服务端 pair_id；0 = 由 key_hash 与 nonce 派生，非 0 = 固定值。
    std::uint32_t pair_id = 0;
    /// 密钥指纹策略；空使用 FixedKeyAuthPolicy。
    std::shared_ptr<const PairingAuthPolicy> policy;
};

/// @brief 配对计数器快照。
struct PairingStats {
    std::uint64_t registers = 0;         ///< Register 成功次数。
    std::uint64_t connects = 0;          ///< Connect 成功发起次数。
    std::uint64_t responses = 0;         ///< 发出的 RESPONSE 数（含拒绝）。
    std::uint64_t confirms = 0;          ///< 发出的 CONFIRM 数。
    std::uint64_t frames_sent = 0;       ///< 入队帧总数（含重传）。
    std::uint64_t frames_received = 0;   ///< HandleFrame 收到的帧总数（含被拒帧）。
    std::uint64_t rejects = 0;           ///< 拒绝的 CONNECT 数。
    std::uint64_t key_mismatches = 0;    ///< 密钥不匹配次数。
    std::uint64_t timeouts = 0;          ///< 超时关闭次数。
    std::uint64_t retransmits = 0;       ///< 主动重传次数。
    std::uint64_t replays = 0;           ///< 幂等重放 RESPONSE 次数。
    std::uint64_t queue_drops = 0;       ///< 发送队列满丢弃的旧帧数。
};

/// @brief 单端配对状态机（非线程安全，由调用方串行驱动）。
class PairingSession final {
   public:
    explicit PairingSession(PairingConfig config = {});

    PairingSession(const PairingSession&) = delete;
    PairingSession& operator=(const PairingSession&) = delete;

    // ---- 发起方 API -------------------------------------------------------

    /// @brief 服务端登记邀请密钥，进入 Pending 等待 CONNECT。
    /// @return kNone 成功；kAlreadyRegistered（本端已有尝试）；kInvalidArgument。
    PairingError Register(const std::string& key, std::uint64_t now_ms);

    /// @brief 客户端发起配对，生成并排队 CONNECT 帧。
    /// @return kNone 成功；kAlreadyRegistered（重复调用）；kInvalidArgument。
    PairingError Connect(const std::string& key, std::uint64_t now_ms);

    /// @brief 服务端对挂起的 CONNECT 作出决定。
    /// @details accept=true：排队 RESPONSE(kNone) 并进入 Paired；
    ///          accept=false：排队 RESPONSE(kRejected)，回到 Pending 等待
    ///          其他客户端（本次拒绝不算失败，返回 kNone）。
    /// @return kNone 成功；kInvalidState（无挂起 CONNECT / 角色或状态不符）。
    PairingError Response(bool accept, std::uint64_t now_ms);

    /// @brief 客户端在 Paired 状态发出 CONFIRM，等待服务端确认。
    /// @details CONFIRM 可能丢失，因此本端仍停留在 Paired 并每
    ///          `retransmit_ms` 重发；收到服务端回发的 CONFIRM（ACK）
    ///          才迁移到 Active。这样两端都只在**收到对端确认**后成功
    ///          （RISK-013），不会出现一端 Active、另一端仍 Paired。
    ///          重复调用是幂等的：只返回 kNone，不重新排队 CONFIRM，也不
    ///          重置 15s 超时与 1s 重传计时器（否则驱动循环里每帧调用一次
    ///          就会让重传与超时永久失效）。
    /// @return kNone 成功（已发送或已 Active）；kInvalidState（尚未 Paired
    ///         或本端是服务端）。
    PairingError Activate(std::uint64_t now_ms);

    // ---- 驱动 -------------------------------------------------------------

    /// @brief 处理一帧对端消息（不自动做网络 I/O）。
    /// @return kNone 表示已处理（含幂等忽略）；其他为拒绝原因。
    /// @note 对密钥不匹配的 CONNECT，服务端会**同时**排队一帧
    ///       RESPONSE(kKeyMismatch) 并返回 kKeyMismatch，调用方应把
    ///       outbound 帧发给对端后再决定是否关闭。
    PairingError HandleFrame(const PairingFrame& frame, std::uint64_t now_ms);

    /// @brief 推进超时与重传；返回状态是否因此改变。
    /// @details Pending/Paired 超过 deadline → Closed + kPeerTimeout，并
    ///          清空发送队列（避免把陈旧帧发给下一次尝试）。未超时时按
    ///          `retransmit_ms` 重发 CONNECT（客户端 Pending）、CONFIRM
    ///          （客户端已 Activate 的 Paired）或 RESPONSE（服务端
    ///          Paired 阶段），重传不改变状态、返回 false。
    bool Tick(std::uint64_t now_ms);

    /// @brief 取出一帧待发送消息（FIFO）。队列空返回 false。
    bool PopOutbound(PairingFrame* out);

    /// @brief 应用主动关闭；幂等。已 Closed 时返回 false。
    bool Close();

    /// @brief 回到 Idle，允许重新 Register/Connect；统计计数器保留。
    void Reset();

    // ---- 查询 -------------------------------------------------------------

    PairingState state() const noexcept { return m_state; }
    PairingError error() const noexcept { return m_error; }
    PairingRole role() const noexcept { return m_role; }
    std::uint64_t key_hash() const noexcept { return m_key_hash; }
    std::uint64_t nonce() const noexcept { return m_nonce; }
    std::uint32_t pair_id() const noexcept { return m_pair_id; }
    /// @brief 是否已有挂起等待 Response 的 CONNECT（服务端）。
    bool HasPendingConnect() const noexcept { return m_has_pending_connect; }
    /// @brief 挂起 CONNECT 的 nonce；无挂起返回 0。
    std::uint64_t pending_connect_nonce() const noexcept { return m_pending_nonce; }
    /// @brief Pending/Paired 的截止时间（毫秒，绝对时钟）；Active/Closed 为 0。
    std::uint64_t deadline_ms() const noexcept { return m_deadline_ms; }
    /// @brief 待发送帧数量。
    std::size_t outbound_size() const noexcept { return m_outbound.size(); }
    PairingStats stats() const noexcept { return m_stats; }
    const PairingConfig& config() const noexcept { return m_config; }

    /// @brief 状态变更回调：from → to，error 为触发本次变更的错误（无则 kNone）。
    std::function<void(PairingState from, PairingState to, PairingError error)> on_state_change;

   private:
    struct StateChange {
        PairingState from = PairingState::kIdle;
        PairingState to = PairingState::kIdle;
        PairingError error = PairingError::kNone;
    };

    /// 单次 API 调用内的状态迁移上限。状态机每步最多产生一条迁移，
    /// 回调内禁止再次调用修改方法（见文件头线程/回调约束），8 是安全上界。
    /// 用固定数组而不是 deque，避免每次状态迁移都做一次堆分配。
    static constexpr std::size_t kMaxChanges = 8;

    /// 状态迁移（同状态只更新 error，不产生事件）。
    void Transition(PairingState next, PairingError error);
    /// 把累计的变更事件派发给回调（先拷贝到局部，避免回调重入污染）。
    void Dispatch();
    /// 入队一帧并统计；队列满时丢弃最旧帧。
    void Enqueue(const PairingFrame& frame);
    /// 清空发送队列与挂起请求（超时/关闭时调用）。
    void ClearPending();
    /// 服务端处理 CONNECT 的完整分支。
    PairingError HandleConnect(const PairingFrame& frame, std::uint64_t now_ms);
    /// 客户端处理 RESPONSE 的完整分支。
    PairingError HandleResponse(const PairingFrame& frame, std::uint64_t now_ms);
    /// 处理 CONFIRM：服务端收到确认后 Active 并回 ACK；客户端收到 ACK 后 Active。
    PairingError HandleConfirm(const PairingFrame& frame, std::uint64_t now_ms);
    /// HandleFrame 的实际实现（外层负责统计与派发）。
    PairingError HandleFrameImpl(const PairingFrame& frame, std::uint64_t now_ms);

    PairingConfig m_config;
    /// 实际使用的策略；默认指向进程级静态对象，注入时指向 m_config.policy
    /// 持有的对象（生命周期与 m_config 一致），因此不需要额外的所有权字段。
    const PairingAuthPolicy* m_policy = nullptr;

    PairingRole m_role = PairingRole::kNone;
    PairingState m_state = PairingState::kIdle;
    PairingError m_error = PairingError::kNone;

    std::uint64_t m_key_hash = 0;
    std::uint64_t m_nonce = 0;   ///< 客户端：本端 nonce；服务端：已配对对端的 nonce。
    std::uint32_t m_pair_id = 0;

    std::uint64_t m_deadline_ms = 0;
    std::uint64_t m_next_retransmit_ms = 0;

    bool m_has_pending_connect = false;
    bool m_confirm_sent = false;  ///< 客户端已发 Activate（Paired 阶段重发 CONFIRM）。
    std::uint64_t m_pending_nonce = 0;

    std::deque<PairingFrame> m_outbound;
    std::array<StateChange, kMaxChanges> m_changes{};
    std::size_t m_change_count = 0;
    PairingStats m_stats;
};

}  // namespace go2cpp::bento
