#pragma once

/**
 * @file session.hpp
 * @brief 通用会话编排（MOD-39）：解析端点 → 建传输 → 配对 → 建通道 →
 *        控制权 → 心跳 → 优雅关闭。
 * @author go2cpp contributors
 * @date 2026-10-28
 * @copyright MIT License
 *
 * @details 背景：Bento 现有的 pairing / frame / heartbeat / session_control
 *          都是**可独立测试的原语**，但把它们按正确顺序串起来、处理每一步的
 *          超时与回滚，是每个接入方都要重复写一遍、且很容易写错的代码。
 *          本组件提供一个**与传输解耦**的会话编排器：
 *
 *              解析 endpoint → 创建 transport → Connect
 *                → PairingSession（Register/Connect/Response/CONFIRM）
 *                → 通道握手（ChannelOpen/ChannelAck）
 *                → SessionControl（参与者与控制权）
 *                → Heartbeat（30s/90s，可配）
 *                → Close（kClose 帧 + ShutdownWrite + 等待对端）
 *
 *          传输通过 `ITransport` 注入（`SessionConfig::transport_factory`），
 *          默认使用 `TcpTransport`；测试可直接注入 `MemoryTransport` 的一端，
 *          因此「连接 → 配对 → 活跃 → 关闭」全流程可以在无 socket、无端口、
 *          无权限的环境里确定性复现。
 *
 * ## 状态机
 *
 *              Idle ──Start()──▶ Connecting ──配对成功──▶ Paired
 *              Paired ──通道握手完成──▶ Active
 *              Connecting/Paired/Active ──Close()/错误──▶ Closing ──▶ Closed
 *              Idle ──Start() 参数错误──▶ Idle（只设置 last_error）
 *
 *          `Connecting` 覆盖「解析端点 + Connect + 配对」全过程；配对成功后
 *          进入 `Paired` 并立即发起通道握手；双方都确认通道后进入 `Active`。
 *          `Closing` 是优雅关闭窗口：本端已发送 `kClose` 并半关闭写方向，
 *          等待对端 `kClose`、EOF 或 `close_timeout_ms` 到期。半关闭后
 *          对端仍可能在 FIN 之前发出在途的 Data/Control/Channel 帧，
 *          这些帧在 `Closing` 内一律丢弃（不判协议错误）；配对/心跳/
 *          租约定时器在关闭窗口内暂停，正常关闭不会被改写成超时错误。
 *
 * ## 字节流上的会话协议
 *
 *          所有消息都复用 `net::Frame` 的 **20B 头 + CRC32** 布局，但使用
 *          会话层自己的协议标识：magic = `kSessionFrameMagic`（"SESS"，
 *          0x53455353）、version = `kSessionFrameVersion`（1）。这与 Link
 *          协议的 "BENT"/1 **不同**，因此两种协议混接到同一端口时会在
 *          magic/version 阶段直接失败（`kBadMagic`/`kBadVersion`），不会被
 *          误解释成对方的「未知帧类型」。帧类型使用独立编号段 0x8001 起，
 *          也不与 `net::FrameType` 的 1..7 重叠。
 *
 *          | type | 名称 | 载荷 |
 *          |---|---|---|
 *          | 0x8001 | Pairing | 32B 配对帧（`EncodePairingFrame`） |
 *          | 0x8002 | Heartbeat | 空 |
 *          | 0x8003 | Data | 2B channel_id + 应用数据 |
 *          | 0x8004 | Control | 1B action + 4B peer_id + 1B mode |
 *          | 0x8005 | Close | 1B `SessionError` |
 *          | 0x8006 | ChannelOpen | 2B channel_id + 4B peer_id |
 *          | 0x8007 | ChannelAck | 2B channel_id |
 *          | 0x8008 | ControlRequest | 4B peer_id + 1B 请求模式 |
 *
 *          帧类型/长度/peer id/channel id 任一不符都按 `kProtocolError` 处理：
 *          会话协议没有向后兼容的扩展位，宁可明确失败也不要猜测。
 *
 *          `Control` 帧里的 peer_id 必须属于本会话（本端或已握手完成的对端）：
 *          陌生 peer 的授权/撤销一律按协议错误关闭会话，避免任意对端把控制权
 *          授予会话外的 id。仲裁端广播的模式取自 `SessionControl` 的实际状态，
 *          不是写死的 `kInteract`；请求方申请的模式由 `RequestControl(mode)` /
 *          `SessionConfig::requested_control_mode` 决定。
 *
 * ## 控制权仲裁
 *
 *          `Session` 内部持有一个 `SessionControl`。配对成功（`Paired`）时
 *          双方 `Join` 本端 peer；进入 `Active` 后再 `Join` 对端 peer。
 *          控制权采用**服务端仲裁**：`server=true` 的一端是仲裁者，收到
 *          对端的 `ControlRequest` 后用本地 `SessionControl` 决策并广播
 *          `Control(grant)`；非仲裁端只提交请求并应用广播，避免双方同时
 *          请求导致控制权分叉。默认 `FirstCome` 策略下先请求者获胜。
 *
 * ## 时间源与确定性
 *
 *          所有超时判定（配对 deadline、心跳、关闭窗口）都走
 *          `SessionConfig::now`（默认 `MonotonicNowMs`）。测试注入假时钟后，
 *          推进时间只需改一个变量再调用 `Poll(0)`，不需要 sleep。
 *          `Poll` 的 `timeout_ms` 只影响底层一次 `Read` 的阻塞预算，不影响
 *          上述逻辑时钟。
 *
 * ## 线程模型与重入
 *
 *          `Session` **非线程安全**：`Start`/`Poll`/`Send`/`Close` 必须由同一
 *          线程串行调用（与 `Link::RunOnce` 一致）。回调在调用线程同步触发：
 *          - `on_state`：状态迁移后调用；
 *          - `on_data`：收到 Data 帧后调用，指针只在回调期间有效；
 *          - `on_error`：进入错误关闭前调用一次；
 *          - `on_control`：控制权变更后调用。
 *
 *          **回调内只读**：可以查询 `state()`/`stats()`/`control()`，但不得
 *          再调用 `Poll`/`Start`/`Close`/`Send`，否则会破坏状态机或重入。
 *          回调内抛异常会穿透 `Poll`，调用方应自行保证回调不抛。
 *
 * ## RAII 与关闭
 *
 *          析构等价于「立即关闭」：关闭 transport、停止心跳、不再派发回调。
 *          正常关闭请显式 `Close()` 并用 `Poll` 驱动到 `Closed`，这样对端
 *          能收到 `kClose` 并走优雅路径。
 */

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "go2cpp/bento/net/session_control.hpp"
#include "go2cpp/bento/net/transport.hpp"

namespace go2cpp::bento::app {

// ---------------------------------------------------------------------------
// 会话层线格式（与 Link 协议不互通）
// ---------------------------------------------------------------------------

/// @brief 会话层协议魔数：ASCII "SESS"（0x53 0x45 0x53 0x53）。
/// @details 与 Link 协议的 `net::kFrameMagic`（"BENT"）不同，这是会话层
///          与 Link 协议互不混淆的第一道判据。
inline constexpr std::uint32_t kSessionFrameMagic = 0x53455353u;

/// @brief 会话层协议版本；与 Link 协议的 `net::kFrameVersion` 相互独立。
inline constexpr std::uint16_t kSessionFrameVersion = 1;

/**
 * @brief 会话层帧类型。
 * @details 编号从 0x8001 起，刻意避开 `net::FrameType` 的 0x01..0x07：
 *          即便有人把 magic/version 校验绕过去，类型域也不会与 Link 帧
 *          的语义撞车。线格式见 `tests/bento/PROTOCOL.md` 的会话层章节。
 */
enum class SessionFrameType : std::uint16_t {
    kPairing = 0x8001,        ///< 配对帧：32B `EncodePairingFrame` 载荷。
    kHeartbeat = 0x8002,      ///< 心跳：空载荷。
    kData = 0x8003,           ///< 应用数据：2B channel_id + 数据。
    kControl = 0x8004,        ///< 控制权广播：1B action + 4B peer_id + 1B mode。
    kClose = 0x8005,          ///< 关闭：1B `SessionError`。
    kChannelOpen = 0x8006,    ///< 通道打开：2B channel_id + 4B peer_id。
    kChannelAck = 0x8007,     ///< 通道确认：2B channel_id。
    kControlRequest = 0x8008, ///< 控制权请求：4B peer_id + 1B 请求模式。
};

/// @brief 会话状态。数值稳定，可写入指标。
enum class SessionState : std::uint8_t {
    kIdle = 0,       ///< 未启动（初始状态）。
    kConnecting = 1, ///< 解析端点 / 建传输 / 连接 / 配对中。
    kPaired = 2,     ///< 配对完成，通道握手进行中。
    kActive = 3,     ///< 通道已建立，可以收发数据。
    kClosing = 4,    ///< 优雅关闭窗口：已发 kClose 并半关闭写方向。
    kClosed = 5,     ///< 终态；资源已释放。
};

/// @brief 状态名（稳定小写字符串）："idle"/"connecting"/"paired"/"active"/"closing"/"closed"。
const char* SessionStateName(SessionState state) noexcept;

/// @brief 状态迁移是否合法（用于自检与测试，不阻止运行时迁移）。
bool IsSessionTransitionAllowed(SessionState from, SessionState to) noexcept;

/// @brief 会话错误码（同时作为 kClose 帧的关闭原因）。
enum class SessionError : std::uint8_t {
    kNone = 0,                 ///< 无错误。
    kInvalidState = 1,         ///< 当前状态不允许该操作（重复 Start 等）。
    kInvalidArgument = 2,      ///< 配置非法（空 key、peer id 0、channel id 0）。
    kInvalidEndpoint = 3,      ///< endpoint 无法解析（缺少端口、端口越界）。
    kTransportUnavailable = 4, ///< 无法创建传输（factory 为空或返回空）。
    kConnectTimeout = 5,       ///< Connect 超过 connect_timeout_ms。
    kConnectFailed = 6,        ///< Connect 其它失败（拒绝/网络不可达）。
    kPairingFailed = 7,        ///< 配对状态机拒绝（版本/状态/参数错误）。
    kPairingRejected = 8,      ///< 密钥不匹配或对端拒绝。
    kPairingTimeout = 9,       ///< 配对超过 pairing_timeout_ms。
    kChannelFailed = 10,       ///< 通道握手失败（id/peer 冲突）。
    kHeartbeatTimeout = 11,    ///< 对端超过 heartbeat_timeout_ms 无任何消息。
    kTransportClosed = 12,     ///< 传输层 EOF/错误。
    kProtocolError = 13,       ///< 帧类型/长度/字段非法。
    kWriteFailed = 14,         ///< 发送失败（非超时）。
    kQueueFull = 15,           ///< 发送队列超过 max_outbox_frames。
    kMessageTooLarge = 16,     ///< 单条消息超过帧载荷上限。
    kControlDenied = 17,       ///< 仲裁端拒绝控制权请求（已有控制器）。
};

/// @brief 错误名（稳定小写字符串）："none"/"invalid_state"/...。
const char* SessionErrorName(SessionError error) noexcept;

/**
 * @brief 解析 `host:port` / `[ipv6]:port` 端点。
 * @param text 输入文本；允许前后空白，host 必须非空。
 * @param endpoint 输出；成功时写入 host/port。
 * @return 成功返回 true；缺少冒号、host 为空、端口非数字或不在 1..65535
 *         返回 false（不做 DNS 解析，纯语法校验）。
 *
 * @note 裸 IPv6 必须写成 `[::1]:8080`：`::1:8080` 无法区分「多个冒号分隔的
 *       地址」与「端口」，本函数按语法错误拒绝，避免把 8080 当成地址段。
 */
bool ParseEndpoint(std::string_view text, net::Endpoint* endpoint);

/// @brief 会话通道状态（当前实现为单通道）。
struct SessionChannel {
    std::uint32_t id = 0;            ///< 通道 id（来自配置）。
    bool open = false;               ///< 双方 Open/Ack 是否完成。
    std::uint64_t opened_at_ms = 0;  ///< 进入 Active 的逻辑时钟（毫秒）。
    std::uint64_t tx_bytes = 0;      ///< 已发送应用字节数。
    std::uint64_t rx_bytes = 0;      ///< 已接收应用字节数。
    std::uint64_t tx_frames = 0;     ///< 已发送 Data 帧数。
    std::uint64_t rx_frames = 0;     ///< 已接收 Data 帧数。
};

/// @brief 会话计数器快照。
struct SessionStats {
    std::uint64_t polls = 0;                ///< Poll 调用次数。
    std::uint64_t frames_sent = 0;          ///< 成功写出的帧数（含重传；部分写续传只计一次）。
    std::uint64_t frames_received = 0;      ///< 成功解码的帧数。
    std::uint64_t bytes_sent = 0;           ///< 写出的字节数。
    std::uint64_t bytes_received = 0;       ///< 读入的字节数。
    std::uint64_t data_frames_sent = 0;     ///< Data 帧发送数。
    std::uint64_t data_frames_received = 0; ///< Data 帧接收数。
    std::uint64_t heartbeats_sent = 0;      ///< 心跳发送数。
    std::uint64_t heartbeats_received = 0;  ///< 心跳接收数。
    std::uint64_t state_changes = 0;        ///< 状态迁移次数。
    std::uint64_t control_requests = 0;     ///< RequestControl 调用次数。
    std::uint64_t protocol_errors = 0;      ///< 协议错误次数。
    /// 发送队列满而丢弃的帧数（含内部帧）；`Closing` 窗口内丢弃的
    /// 接收帧不计入本字段（它们是正常关闭的在途数据，不是错误）。
    std::uint64_t dropped_frames = 0;
};

/**
 * @brief 会话配置。
 *
 * @details 默认值面向「客户端连本机服务端」的常规场景；测试通过
 *          `transport_factory`、`now`、`connect_timeout_ms` 等字段注入。
 */
struct SessionConfig {
    /// @brief 对端端点：`host:port` 或 `[ipv6]:port`。客户端必填；
    ///        服务端可留空（传输已由 factory 注入）。
    std::string endpoint;
    /// @brief true = 配对服务端（Host），同时是控制权仲裁者；false = 客户端。
    bool server = false;
    /// @brief 配对邀请密钥；空字符串在 Start 时返回 kInvalidArgument。
    std::string pairing_key;
    /// @brief 本端 peer id（0 非法）。默认客户端 1。
    std::uint32_t local_peer_id = 1;
    /// @brief 通道 id（0 非法）。默认 1。
    std::uint32_t channel_id = 1;

    std::uint32_t connect_timeout_ms = 5000;   ///< Connect 总超时。
    std::uint32_t io_timeout_ms = 50;          ///< 单次 Read/Write 预算。
    std::uint32_t close_timeout_ms = 2000;     ///< 优雅关闭等待对端的上限。
    std::uint32_t heartbeat_interval_ms = 30000; ///< 心跳间隔（0 = 默认 30s）。
    std::uint32_t heartbeat_timeout_ms = 90000;  ///< 存活超时（0 = 默认 90s）。
    std::uint32_t pairing_timeout_ms = 15000;    ///< 配对总超时。
    std::uint32_t pairing_retransmit_ms = 1000;  ///< 配对重传间隔。
    std::uint64_t pairing_nonce = 0;             ///< 固定 nonce（测试/复现）；0 = 自动。
    std::uint32_t pair_id = 0;                   ///< 固定 pair_id；0 = 自动派生。
    bool auto_accept_pairing = true;             ///< 服务端收到 CONNECT 后自动接受。
    /// @brief `RequestControl()`（无参版本）默认申请的控制模式。
    /// @details 默认 `kInteract`（申请唯一控制权）。可配置为 `kView`，用于
    ///          「只看不控」的观察端；实际授予/广播的模式以仲裁结果为准。
    go2cpp::bento::ControlMode requested_control_mode = go2cpp::bento::ControlMode::kInteract;

    /// @brief 内部 SessionControl 配置（策略/租约/参与者上限）。
    go2cpp::bento::SessionControlConfig control;

    /// @brief 传输工厂；为空时客户端用 `TcpTransport`，服务端直接报错。
    /// @note 工厂可能被调用一次；返回空指针视为 kTransportUnavailable。
    std::function<std::unique_ptr<go2cpp::bento::net::ITransport>()> transport_factory;
    /// @brief 逻辑时钟（毫秒）；为空时用 `MonotonicNowMs`。测试注入假时钟。
    std::function<std::uint64_t()> now;
    /// @brief 发送队列上限（帧数）；超过后 Send 返回 kQueueFull。
    std::size_t max_outbox_frames = 256;
};

/// @brief 状态迁移回调：from → to。
using SessionStateHandler = std::function<void(SessionState from, SessionState to)>;
/// @brief 数据回调：仅在 Active 且 channel 匹配时触发；指针只在回调期间有效。
using SessionDataHandler = std::function<void(std::uint32_t channel_id, const std::uint8_t* data,
                                              std::size_t size)>;
/// @brief 错误回调：进入错误关闭前同步调用一次。
using SessionErrorHandler = std::function<void(SessionError error, const std::string& message)>;
/// @brief 控制权变更回调（来自本端操作或对端广播）。
using SessionControlHandler =
    std::function<void(const go2cpp::bento::ControlChange& change)>;

/**
 * @brief 通用会话编排器（单线程、非阻塞轮询）。
 *
 * 典型用法：
 * @code
 *   SessionConfig config;
 *   config.endpoint = "127.0.0.1:9000";
 *   config.pairing_key = "invite-key";
 *   Session session(config);
 *   session.on_data = [](std::uint32_t, const std::uint8_t* data, std::size_t size) {
 *       // 处理应用数据
 *   };
 *   if (session.Start() != SessionError::kNone) { ... }
 *   while (running) {
 *       session.Poll(10);
 *       // ... 其它事件
 *   }
 *   session.Close();
 *   while (session.Poll(10)) { }
 * @endcode
 */
class Session final {
   public:
    /// @brief 构造；不发起任何 I/O（Start 才发起）。
    explicit Session(SessionConfig config = {});
    /// @brief 析构等价于立即关闭；不派发回调。
    ~Session();

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    Session(Session&&) = delete;
    Session& operator=(Session&&) = delete;

    /**
     * @brief 启动会话：解析端点、创建传输、Connect、启动配对。
     * @return kNone 表示已进入 Connecting；其它值表示启动失败（状态可能
     *         仍是 Idle 或已 Closed，见文件头状态机）。
     *
     * @details 参数错误（空 key / peer id 0 / channel id 0 / 客户端缺端点）
     *          只设置 last_error 并保持 Idle，方便修正配置后重试；一旦开始
     *          建传输后失败则进入 Closed（需要重新构造 Session）。
     */
    SessionError Start();

    /**
     * @brief 驱动一轮 I/O 与定时器。
     * @param timeout_ms 首次 Read 的阻塞预算（毫秒）；负值按 0 处理。
     *        读满 4 KiB 后会以 0 预算继续读下一批，单次 Poll 最多 32 KiB，
     *        用来把高吞吐场景的读上限提高 8 倍而不饿死定时器。
     * @return true 表示会话尚未 Closed；false 表示已进入终态。
     *
     * @details 顺序：读传输 → 解帧并处理 → 自动接受配对 → 定时器
     *          （配对/心跳/租约/关闭窗口）→ 发送队列。即使本次没有事件，
     *          也要定期调用，否则超时与重传不会发生。
     */
    bool Poll(int timeout_ms = 0);

    /// @brief 发起优雅关闭；幂等。之后用 Poll 驱动到 Closed。
    void Close();

    /// @brief 服务端手动接受挂起的 CONNECT（`auto_accept_pairing=false` 时）。
    /// @return kNone 成功；kInvalidState 无挂起请求或角色不符。
    SessionError AcceptPairing();

    /// @brief 在默认通道上发送应用数据。
    SessionError Send(const std::uint8_t* data, std::size_t size);
    /// @brief 在默认通道上发送应用数据（字符串视图）。
    SessionError Send(std::string_view data);
    /// @brief 在指定通道上发送；channel_id 必须等于配置值。
    SessionError Send(std::uint32_t channel_id, const std::uint8_t* data, std::size_t size);

    /**
     * @brief 按 `SessionConfig::requested_control_mode` 请求控制权。
     * @return 仲裁端：kNone 表示已获得并已广播，kControlDenied 表示被拒；
     *         非仲裁端：kNone 表示请求已发送（结果通过 on_control 观察）。
     *
     * @note 非 Active 状态返回 kInvalidState。
     */
    SessionError RequestControl();

    /**
     * @brief 请求指定控制模式（调用方传入，覆盖配置默认值）。
     * @param mode 只接受 `kView` 或 `kInteract`；`kNone` 返回 kInvalidArgument。
     * @return 语义同无参版本：仲裁端直接决策并广播实际授予的模式，非仲裁端
     *         把申请模式随 `ControlRequest` 帧发给仲裁端。
     *
     * @note 请求 `kView` 不会抢占当前控制器，只把本端权限设为只读。
     */
    SessionError RequestControl(go2cpp::bento::ControlMode mode);

    /**
     * @brief 轮询直到状态达到 target 或真实时间超时。
     * @return 是否达到目标状态。
     *
     * @note 本方法只驱动本端 Poll；双端会话需要外部同时驱动对端，否则会
     *       等到超时。测试里的双端 pump 应显式交替调用 `Poll`。
     */
    bool WaitFor(SessionState target, int timeout_ms);

    // ---- 查询 -------------------------------------------------------------

    SessionState state() const noexcept;
    SessionError last_error() const noexcept;
    const std::string& last_error_message() const noexcept;
    const go2cpp::bento::net::Endpoint& endpoint() const noexcept;
    const SessionChannel& channel() const noexcept;
    const SessionStats& stats() const noexcept;
    const SessionConfig& config() const noexcept;
    std::uint32_t local_peer_id() const noexcept;
    /// @brief 对端 peer id；通道握手完成前为 0。
    std::uint32_t remote_peer_id() const noexcept;
    /// @brief 底层传输；未创建时为 nullptr（生命周期归 Session）。
    const go2cpp::bento::net::ITransport* transport() const noexcept;
    /// @brief 内部仲裁器（只读查询；修改需通过 Session 的 API 以保持广播一致）。
    const go2cpp::bento::SessionControl& control() const noexcept;

    // ---- 回调（初始化阶段赋值；运行期替换需外部保证不与 Poll 并发） -------

    SessionStateHandler on_state;      ///< 状态迁移。
    SessionDataHandler on_data;        ///< 应用数据。
    SessionErrorHandler on_error;      ///< 错误（关闭前一次）。
    SessionControlHandler on_control;  ///< 控制权变更。

   private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace go2cpp::bento::app
