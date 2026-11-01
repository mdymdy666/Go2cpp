#pragma once

/**
 * @file signaling.hpp
 * @brief SDP 风格信令：会话描述、ICE 候选交换与 offer/answer 状态机。
 * @details 本模块负责「媒体会话怎么谈」的那一层，和 P2P 模块的分工是：
 *
 *          - `go2cpp::p2p::Candidate` / `p2p::P2PChannel` 负责**连通性**：
 *            候选优先级、STUN 检查、打洞、TURN 中继；
 *          - 本模块负责**会话描述**：把媒体行（m=）、编解码（rtpmap/fmtp）、
 *            ICE 凭据（ufrag/pwd）、DTLS 指纹（fingerprint）和候选打包成
 *            SDP 文本，并驱动 `idle → offering → answering → connected`
 *            的协商状态机。
 *
 *          文本格式是 SDP 的子集（RFC 4566 行格式 + RFC 8839 ICE 属性）：
 *
 *          ```
 *          v=0
 *          o=- 5f2c... 1 IN IP4 127.0.0.1
 *          s=go2cpp-media
 *          t=0 0
 *          a=group:BUNDLE 0 1
 *          m=video 9 UDP/TLS/RTP/SAVPF 96
 *          c=IN IP4 0.0.0.0
 *          a=mid:0
 *          a=rtpmap:96 H264/90000
 *          a=fmtp:96 packetization-mode=1
 *          a=sendrecv
 *          a=ice-ufrag:Ab3d
 *          a=ice-pwd:0123456789abcdef01234567
 *          a=ice-options:trickle
 *          a=fingerprint:sha-256 AA:BB:...
 *          a=setup:actpass
 *          a=candidate:1 1 udp 2130706431 192.168.1.5 5000 typ host
 *          a=end-of-candidates
 *          ```
 *
 *          为什么不用 JSON：SDP 的 `m=` 行天然是「多路媒体 + 每路属性」的
 *          结构，解析器只要按行状态机走，不引入外部依赖；同时候选行可以直接
 *          贴到现有 P2P 信令消息里，和 IM/WebSocket 传输保持解耦。
 *
 *          与 P2P 集成：`ToP2PCandidate()` / `FromP2PCandidate()` 在
 *          `GO2CPP_MEDIA_HAVE_P2P` 打开时提供双向转换（候选类型、优先级、
 *          foundation、base 地址一一对应）；媒体构建时链接了 `go2cpp_p2p`
 *          就会自动打开。候选交换本身可以用 `p2p::LoopbackSignaling` 承载
 *          （见测试），生产环境换成 IM/WebSocket 即可。
 *
 *          线程模型：单线程；`SignalingSession` 不加锁，和 `VideoChannel`
 *          的 pump 模型一致。
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#if defined(GO2CPP_MEDIA_HAVE_P2P)
#include "go2cpp/p2p/candidate.hpp"
#endif

namespace go2cpp::media {

// ---------------------------------------------------------------------------
// ICE 候选
// ---------------------------------------------------------------------------

/// @brief ICE 候选类型（数值只用于日志，不参与优先级计算）。
enum class IceCandidateType : std::uint8_t {
    kHost = 0,   ///< 本机网卡地址。
    kSrflx = 1,  ///< STUN 服务器反射地址（NAT 映射）。
    kPrflx = 2,  ///< 连通性检查中观察到的对端反射地址。
    kRelay = 3,  ///< TURN 中继地址。
};

/// @brief 候选类型名（host/srflx/prflx/relay）。
const char* IceCandidateTypeName(IceCandidateType type) noexcept;

/// @brief 解析候选类型名；非法返回 nullopt。
std::optional<IceCandidateType> ParseIceCandidateType(std::string_view name);

/// @brief 类型偏好（RFC 8445：host=126、prflx=110、srflx=100、relay=0）。
std::uint32_t IceCandidateTypePreference(IceCandidateType type) noexcept;

/// @brief 按 RFC 8445 §5.1.2.1 计算候选优先级。
std::uint32_t ComputeIceCandidatePriority(IceCandidateType type, std::uint32_t local_pref = 65535,
                                          std::uint8_t component = 1) noexcept;

/// @brief 一条 ICE 候选（SDP `a=candidate:` 行）。
struct IceCandidate {
    IceCandidateType type = IceCandidateType::kHost;
    std::string foundation;          ///< 同类型同 base 共享的短标识。
    std::uint32_t component = 1;     ///< 组件号，固定 1。
    std::string transport = "udp";   ///< 传输协议（udp/tcp）。
    std::uint32_t priority = 0;      ///< 优先级；0 表示未计算。
    std::string address;             ///< 候选 IP 字面量（IPv4/IPv6）。
    std::uint16_t port = 0;          ///< 候选端口。
    std::string related_address;     ///< raddr（srflx/relay 的 base）。
    std::uint16_t related_port = 0;  ///< rport。
    std::uint32_t generation = 0;    ///< ICE generation。

    /// @brief 单行文本（`candidate:...`，不含 `a=` 前缀）。
    std::string ToString() const;

    /// @brief 带 `a=` 前缀的 SDP 属性行。
    std::string ToAttribute() const;

    /// @brief 解析 `candidate:` 行（可带 `a=` 前缀）；失败返回 nullopt。
    static std::optional<IceCandidate> Parse(std::string_view line, std::string* error = nullptr);

    bool operator==(const IceCandidate& other) const noexcept;
    bool operator!=(const IceCandidate& other) const noexcept {
        return !(*this == other);
    }
};

/// @brief 候选是否可用：地址/端口/传输合法。
bool IsValidIceCandidate(const IceCandidate& candidate, std::string* error = nullptr);

/// @brief 按优先级从高到低排序（稳定排序，同优先级保持输入顺序）。
void SortCandidatesByPriority(std::vector<IceCandidate>* candidates);

#if defined(GO2CPP_MEDIA_HAVE_P2P)
/// @brief 转成 P2P 模块的候选类型（复用它的比较/文本/排序逻辑）。
p2p::Candidate ToP2PCandidate(const IceCandidate& candidate);

/// @brief 从 P2P 模块的候选转回；地址非法时返回 nullopt。
std::optional<IceCandidate> FromP2PCandidate(const p2p::Candidate& candidate);
#endif

// ---------------------------------------------------------------------------
// SDP 描述
// ---------------------------------------------------------------------------

/// @brief SDP 描述的语义类型（SDP 文本本身不携带，由信令消息标记）。
enum class SdpType : std::uint8_t {
    kOffer = 0,
    kAnswer = 1,
};

const char* SdpTypeName(SdpType type) noexcept;
std::optional<SdpType> ParseSdpType(std::string_view name);

/// @brief 媒体类型。
enum class MediaKind : std::uint8_t {
    kVideo = 0,
    kAudio = 1,
    kApplication = 2,
};

const char* MediaKindName(MediaKind kind) noexcept;
std::optional<MediaKind> ParseMediaKind(std::string_view name);

/// @brief 媒体方向。
enum class SdpDirection : std::uint8_t {
    kSendRecv = 0,
    kSendOnly = 1,
    kRecvOnly = 2,
    kInactive = 3,
};

const char* SdpDirectionName(SdpDirection direction) noexcept;
std::optional<SdpDirection> ParseSdpDirection(std::string_view name);

/// @brief 从应答方视角反转方向（offer sendonly → answer recvonly）。
SdpDirection ReverseDirection(SdpDirection direction) noexcept;

/// @brief offer/answer 方向是否兼容。
bool IsDirectionCompatible(SdpDirection offer, SdpDirection answer) noexcept;

/// @brief DTLS 角色（RFC 5763 `a=setup`）。
enum class DtlsSetup : std::uint8_t {
    kActpass = 0,
    kActive = 1,
    kPassive = 2,
};

const char* DtlsSetupName(DtlsSetup setup) noexcept;
std::optional<DtlsSetup> ParseDtlsSetup(std::string_view name);

/// @brief answer 侧在给定 offer 下可用的 DTLS 角色。
DtlsSetup NegotiateAnswerSetup(DtlsSetup offer_setup, DtlsSetup preferred) noexcept;

/// @brief 一路 RTP 编解码。
struct RtpCodec {
    std::uint8_t payload_type = 96;
    std::string encoding_name = "H264";
    std::uint32_t clock_rate = 90000;
    std::uint8_t channels = 1;  ///< 音频通道数；视频固定 1。
    std::string fmtp;           ///< `a=fmtp` 参数（不含 payload type）。

    bool operator==(const RtpCodec& other) const noexcept {
        return payload_type == other.payload_type && encoding_name == other.encoding_name &&
               clock_rate == other.clock_rate && channels == other.channels && fmtp == other.fmtp;
    }
};

/// @brief 一路媒体（SDP 的 m= 段）。
struct MediaDescription {
    MediaKind kind = MediaKind::kVideo;
    std::string mid = "0";                       ///< `a=mid`，会话内唯一。
    std::uint16_t port = 9;                      ///< 端口（9 = discard，ICE 下无意义）。
    std::string protocol = "UDP/TLS/RTP/SAVPF";  ///< m= 行的传输协议。
    std::string connection_address = "0.0.0.0";  ///< `c=IN IP4 ...`。
    SdpDirection direction = SdpDirection::kSendRecv;
    std::vector<RtpCodec> codecs;  ///< 该媒体的编解码列表。
    std::string ice_ufrag;         ///< ICE 用户名片段（4..256）。
    std::string ice_pwd;           ///< ICE 密码（22..256）。
    std::vector<IceCandidate> candidates;
    bool end_of_candidates = false;
    std::string fingerprint;  ///< `sha-256 AA:BB:...`。
    DtlsSetup setup = DtlsSetup::kActpass;
    std::string msid;  ///< 可选 `stream_id track_id`。

    /// @brief 序列化为 SDP 媒体段（以 `m=` 开头，每行含 `\n`）。
    std::string ToSdp() const;

    /// @brief 校验字段合法性；失败写入 error。
    bool Validate(std::string* error = nullptr) const;
};

/// @brief 一份会话描述（SDP 子集）。
struct SessionDescription {
    SdpType type = SdpType::kOffer;
    std::string session_id;                     ///< o= 行的会话 id。
    std::uint64_t session_version = 1;          ///< o= 行的版本号。
    std::string origin_address = "127.0.0.1";   ///< o= 行地址。
    std::string session_name = "go2cpp-media";  ///< s= 行。
    std::vector<std::string> bundle_mids;       ///< `a=group:BUNDLE`。
    std::vector<MediaDescription> media;        ///< 媒体段。

    /// @brief 序列化为完整 SDP 文本（末尾带 `\n`）。
    std::string ToSdp() const;

    /// @brief 解析 SDP 文本；格式错误返回 nullopt 并写 error。
    static std::optional<SessionDescription> ParseSdp(std::string_view text,
                                                      std::string* error = nullptr);

    /// @brief 按 mid 查找媒体段；找不到返回 nullptr。
    const MediaDescription* FindMedia(std::string_view mid) const;
    MediaDescription* FindMedia(std::string_view mid);

    /// @brief 所有媒体段的候选（按媒体顺序拼接）。
    std::vector<IceCandidate> AllCandidates() const;

    /// @brief 校验会话描述；失败写 error。
    bool Validate(std::string* error = nullptr) const;
};

// ---------------------------------------------------------------------------
// 信令消息（可直接走 IM/WebSocket 传输）
// ---------------------------------------------------------------------------

/// @brief 信令消息类型。
enum class SignalingMessageType : std::uint8_t {
    kOffer = 0,
    kAnswer = 1,
    kCandidate = 2,
    kEndOfCandidates = 3,
    kBye = 4,
};

const char* SignalingMessageTypeName(SignalingMessageType type) noexcept;
std::optional<SignalingMessageType> ParseSignalingMessageType(std::string_view name);

/// @brief 一条信令消息：`g2p-signal/1 <type>\n<body>`。
struct SignalingMessage {
    SignalingMessageType type = SignalingMessageType::kOffer;
    std::string sdp;         ///< offer/answer 的 SDP 文本。
    IceCandidate candidate;  ///< candidate 消息的候选。

    /// @brief 序列化为可传输文本（正文可能是多行 SDP）。
    std::string ToText() const;

    /// @brief 解析文本消息；格式错误返回 nullopt。
    static std::optional<SignalingMessage> Parse(std::string_view text,
                                                 std::string* error = nullptr);
};

// ---------------------------------------------------------------------------
// 协商状态机
// ---------------------------------------------------------------------------

/// @brief 本端角色。
enum class SignalingRole : std::uint8_t {
    kOfferer = 0,
    kAnswerer = 1,
};

const char* SignalingRoleName(SignalingRole role) noexcept;

/// @brief 协商状态。
enum class SignalingState : std::uint8_t {
    kIdle = 0,       ///< 尚未开始。
    kOffering = 1,   ///< 本端已发 offer，或已收到对端 offer。
    kAnswering = 2,  ///< 本端已发 answer，或已收到对端 answer。
    kConnected = 3,  ///< ICE 选对成功，媒体可通。
    kFailed = 4,     ///< 协商超时/ICE 失败。
    kClosed = 5,     ///< 主动关闭。
};

const char* SignalingStateName(SignalingState state) noexcept;

/// @brief 信令操作结果。
enum class SignalingStatus : std::uint8_t {
    kOk = 0,
    kInvalidState,         ///< 当前状态不允许该操作。
    kInvalidRole,          ///< 角色不允许该操作（如 answerer 调 CreateOffer）。
    kInvalidArgument,      ///< 参数为空/越界。
    kInvalidDescription,   ///< SDP 描述校验失败。
    kNoLocalDescription,   ///< 还没有本端描述。
    kNoRemoteDescription,  ///< 还没有对端描述。
    kMediaMismatch,        ///< answer 的 m-line/方向/DTLS 角色与 offer 不兼容。
    kCandidateRejected,    ///< 候选非法。
    kAlreadyConnected,     ///< 已经 connected。
    kTimeout,              ///< 协商超时。
    kClosed,               ///< 会话已关闭。
};

const char* SignalingStatusName(SignalingStatus status) noexcept;

/// @brief 信令统计。
struct SignalingStats {
    std::uint64_t offers_created = 0;
    std::uint64_t offers_received = 0;
    std::uint64_t answers_created = 0;
    std::uint64_t answers_received = 0;
    std::uint64_t local_candidates_added = 0;
    std::uint64_t remote_candidates_added = 0;
    std::uint64_t remote_candidates_duplicate = 0;
    std::uint64_t remote_candidates_rejected = 0;
    std::uint64_t sdp_parse_errors = 0;
    std::uint64_t ice_connected = 0;
    std::uint64_t ice_failed = 0;
};

/// @brief 信令会话配置。
struct SignalingConfig {
    SignalingRole role = SignalingRole::kOfferer;
    std::string session_id;  ///< 空则随机生成。
    std::string origin_address = "127.0.0.1";
    std::string session_name = "go2cpp-media";
    std::string ice_ufrag;    ///< 空则随机生成（8 字符）。
    std::string ice_pwd;      ///< 空则随机生成（24 字符）。
    std::string fingerprint;  ///< 空则生成占位 sha-256 指纹。
    /// offerer 的 `a=setup`；默认 actpass（让 answerer 决定角色）。
    DtlsSetup offer_setup = DtlsSetup::kActpass;
    /// answerer 的首选 `a=setup`；不兼容 offer 时会自动纠正。
    DtlsSetup answer_setup = DtlsSetup::kActive;
    /// 本端媒体声明；空则用默认「视频 H264 + 音频 opus」两路。
    std::vector<MediaDescription> local_media;
    /// 初始本地候选（也可以在协商过程中用 AddLocalCandidate 追加）。
    std::vector<IceCandidate> local_candidates;
    bool trickle = true;                           ///< 是否声明 `a=ice-options:trickle`。
    std::uint64_t negotiation_timeout_ms = 30000;  ///< offer→connected 超时。
    /// 可注入时钟（毫秒）；空则用 steady_clock。测试用它驱动 Tick 超时。
    std::function<std::uint64_t()> clock;
};

/**
 * @brief 一端信令会话。
 *
 * @details 状态迁移（offerer 与 answerer 走同一串状态名）：
 *
 * ```
 * offerer : idle --CreateOffer--> offering --SetRemoteAnswer--> answering --OnIceConnected-->
 * connected answerer: idle --SetRemoteOffer--> offering --CreateAnswer--> answering
 * --OnIceConnected--> connected
 * ```
 *
 *          `Tick()` 在 offering/answering 超过 `negotiation_timeout_ms` 后置
 *          failed；`OnIceFailed()` 直接置 failed；`Close()` 置 closed。
 *
 *          调用方负责把 `CreateOffer()/CreateAnswer()` 产出的 SDP 经
 *          `SignalingMessage` 发给对端；收到候选后用 `AddRemoteCandidate()`
 *          或 `SignalingMessage` 里的 `kCandidate` 处理。本类**不建 socket、
 *          不跑 ICE 检查**——那是 `p2p::P2PChannel` 的职责；选对成功后调用
 *          `OnIceConnected(selected_local, selected_remote)` 即可。
 */
class SignalingSession {
   public:
    explicit SignalingSession(SignalingConfig config = {});
    ~SignalingSession();

    SignalingSession(const SignalingSession&) = delete;
    SignalingSession& operator=(const SignalingSession&) = delete;

    // --- 描述协商 ---
    /// @brief offerer：创建 offer（idle → offering）。
    std::optional<SessionDescription> CreateOffer(std::string* error = nullptr);
    /// @brief answerer：应用对端 offer（idle → offering）。
    SignalingStatus SetRemoteOffer(const SessionDescription& offer, std::string* error = nullptr);
    /// @brief answerer：创建 answer（offering → answering）。
    std::optional<SessionDescription> CreateAnswer(std::string* error = nullptr);
    /// @brief offerer：应用对端 answer（offering → answering）。
    SignalingStatus SetRemoteAnswer(const SessionDescription& answer, std::string* error = nullptr);

    // --- 候选交换 ---
    /// @brief 追加本端候选（可发生在协商前后；idle/offering/answering 都允许）。
    /// @note 候选按 BUNDLE 语义复制到每一路媒体段（浏览器对同一 transport 的
    ///       候选也是每条 m= 行重复出现），不区分单路候选。
    SignalingStatus AddLocalCandidate(const IceCandidate& candidate, std::string* error = nullptr);
    /// @brief 追加对端候选（需要已有对端描述）。
    SignalingStatus AddRemoteCandidate(const IceCandidate& candidate, std::string* error = nullptr);
    /// @brief 标记对端候选收集结束（`a=end-of-candidates`）。
    void EndOfRemoteCandidates();

    // --- ICE / 生命周期 ---
    /// @brief ICE 检查选出有效候选对后调用（answering → connected）。
    SignalingStatus OnIceConnected(const IceCandidate& local, const IceCandidate& remote,
                                   std::string* error = nullptr);
    /// @brief ICE 层报告失败（offering/answering → failed）。
    SignalingStatus OnIceFailed();
    /// @brief 驱动协商超时检查。
    SignalingStatus Tick();
    /// @brief 主动关闭会话。
    void Close();

    // --- 查询 ---
    SignalingRole role() const noexcept;
    SignalingState state() const noexcept;
    bool connected() const noexcept;
    const SignalingConfig& config() const noexcept;
    const SignalingStats& stats() const noexcept;

    const SessionDescription* local_description() const noexcept;
    const SessionDescription* remote_description() const noexcept;
    std::vector<IceCandidate> local_candidates() const;
    std::vector<IceCandidate> remote_candidates() const;
    bool remote_end_of_candidates() const noexcept;

    /// @brief 优先级最高的对端候选（ICE 检查的起点）；没有则 nullopt。
    std::optional<IceCandidate> BestRemoteCandidate() const;

    /// @brief ICE 选中的候选对（connected 后有效）。
    const std::optional<IceCandidate>& selected_local_candidate() const noexcept;
    const std::optional<IceCandidate>& selected_remote_candidate() const noexcept;

    /// @brief 本端/对端 DTLS 角色（描述就绪后有效）。
    DtlsSetup local_setup() const noexcept;
    DtlsSetup remote_setup() const noexcept;

   private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace go2cpp::media
