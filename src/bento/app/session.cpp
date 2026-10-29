/**
 * @file session.cpp
 * @brief 通用会话编排实现（MOD-39）。
 * @author go2cpp contributors
 * @date 2026-10-28
 * @copyright MIT License
 */

#include "go2cpp/bento/app/session.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "go2cpp/bento/net/frame.hpp"
#include "go2cpp/bento/net/heartbeat.hpp"
#include "go2cpp/bento/net/pairing.hpp"
#include "go2cpp/bento/net/tcp_transport.hpp"

namespace go2cpp::bento::app {
namespace {

// 会话层帧类型：公开的 SessionFrameType 是唯一真源，这里只是 uint16_t 别名，
// 避免在实现里重复写一遍魔数。
constexpr std::uint16_t kFramePairing = static_cast<std::uint16_t>(SessionFrameType::kPairing);
constexpr std::uint16_t kFrameHeartbeat = static_cast<std::uint16_t>(SessionFrameType::kHeartbeat);
constexpr std::uint16_t kFrameData = static_cast<std::uint16_t>(SessionFrameType::kData);
constexpr std::uint16_t kFrameControl = static_cast<std::uint16_t>(SessionFrameType::kControl);
constexpr std::uint16_t kFrameClose = static_cast<std::uint16_t>(SessionFrameType::kClose);
constexpr std::uint16_t kFrameChannelOpen =
    static_cast<std::uint16_t>(SessionFrameType::kChannelOpen);
constexpr std::uint16_t kFrameChannelAck =
    static_cast<std::uint16_t>(SessionFrameType::kChannelAck);
constexpr std::uint16_t kFrameControlRequest =
    static_cast<std::uint16_t>(SessionFrameType::kControlRequest);

// 会话层线格式：magic "SESS" + version 1，与 Link 协议（"BENT"/1）不互通。
constexpr net::FrameFormat kSessionFormat{kSessionFrameMagic, kSessionFrameVersion};

// 控制权广播动作。
constexpr std::uint8_t kControlGrant = 0;
constexpr std::uint8_t kControlRevoke = 1;

constexpr std::size_t kDataHeaderBytes = 2;
constexpr std::size_t kChannelOpenBytes = 6;
constexpr std::size_t kChannelAckBytes = 2;
constexpr std::size_t kControlBytes = 6;
constexpr std::size_t kControlRequestBytes = 5;  // 4B peer_id + 1B 请求模式

/// 单次 Read 的栈缓冲：一次读 4 KiB，帧解码器负责拼包。
constexpr std::size_t kReadBufferBytes = 4096;
/// 单次 Poll 最多连续读几屏（32 KiB）：读满一屏说明还有积压，
/// 但必须设上限，避免持续高速数据把定时器/关闭窗口饿死。
constexpr int kMaxReadsPerPoll = 8;

std::string ErrorText(int error) {
    return std::error_code(error, std::generic_category()).message();
}

void WriteU16(std::vector<std::uint8_t>* out, std::uint16_t value) {
    out->push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
    out->push_back(static_cast<std::uint8_t>(value & 0xffU));
}

void WriteU32(std::vector<std::uint8_t>* out, std::uint32_t value) {
    out->push_back(static_cast<std::uint8_t>((value >> 24U) & 0xffU));
    out->push_back(static_cast<std::uint8_t>((value >> 16U) & 0xffU));
    out->push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
    out->push_back(static_cast<std::uint8_t>(value & 0xffU));
}

bool ReadU16(const std::uint8_t* data, std::size_t size, std::uint16_t* value) {
    if (data == nullptr || size < 2) {
        return false;
    }
    *value = static_cast<std::uint16_t>((static_cast<std::uint16_t>(data[0]) << 8U) |
                                        static_cast<std::uint16_t>(data[1]));
    return true;
}

bool ReadU32(const std::uint8_t* data, std::size_t size, std::uint32_t* value) {
    if (data == nullptr || size < 4) {
        return false;
    }
    *value = (static_cast<std::uint32_t>(data[0]) << 24U) |
             (static_cast<std::uint32_t>(data[1]) << 16U) |
             (static_cast<std::uint32_t>(data[2]) << 8U) |
             static_cast<std::uint32_t>(data[3]);
    return true;
}

}  // namespace

const char* SessionStateName(SessionState state) noexcept {
    switch (state) {
        case SessionState::kIdle:
            return "idle";
        case SessionState::kConnecting:
            return "connecting";
        case SessionState::kPaired:
            return "paired";
        case SessionState::kActive:
            return "active";
        case SessionState::kClosing:
            return "closing";
        case SessionState::kClosed:
            return "closed";
    }
    return "unknown";
}

bool IsSessionTransitionAllowed(SessionState from, SessionState to) noexcept {
    switch (from) {
        case SessionState::kIdle:
            return to == SessionState::kConnecting || to == SessionState::kClosed;
        case SessionState::kConnecting:
            return to == SessionState::kPaired || to == SessionState::kClosing ||
                   to == SessionState::kClosed;
        case SessionState::kPaired:
            return to == SessionState::kActive || to == SessionState::kClosing ||
                   to == SessionState::kClosed;
        case SessionState::kActive:
            return to == SessionState::kClosing || to == SessionState::kClosed;
        case SessionState::kClosing:
            return to == SessionState::kClosed;
        case SessionState::kClosed:
            return false;
    }
    return false;
}

const char* SessionErrorName(SessionError error) noexcept {
    switch (error) {
        case SessionError::kNone:
            return "none";
        case SessionError::kInvalidState:
            return "invalid_state";
        case SessionError::kInvalidArgument:
            return "invalid_argument";
        case SessionError::kInvalidEndpoint:
            return "invalid_endpoint";
        case SessionError::kTransportUnavailable:
            return "transport_unavailable";
        case SessionError::kConnectTimeout:
            return "connect_timeout";
        case SessionError::kConnectFailed:
            return "connect_failed";
        case SessionError::kPairingFailed:
            return "pairing_failed";
        case SessionError::kPairingRejected:
            return "pairing_rejected";
        case SessionError::kPairingTimeout:
            return "pairing_timeout";
        case SessionError::kChannelFailed:
            return "channel_failed";
        case SessionError::kHeartbeatTimeout:
            return "heartbeat_timeout";
        case SessionError::kTransportClosed:
            return "transport_closed";
        case SessionError::kProtocolError:
            return "protocol_error";
        case SessionError::kWriteFailed:
            return "write_failed";
        case SessionError::kQueueFull:
            return "queue_full";
        case SessionError::kMessageTooLarge:
            return "message_too_large";
        case SessionError::kControlDenied:
            return "control_denied";
    }
    return "unknown";
}

bool ParseEndpoint(std::string_view text, net::Endpoint* endpoint) {
    if (endpoint == nullptr) {
        return false;
    }
    // 前后空白容忍：配置文件里常见行尾空格。
    const std::size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return false;
    }
    const std::size_t last = text.find_last_not_of(" \t\r\n");
    text = text.substr(first, last - first + 1);

    std::string_view host;
    std::string_view port_text;
    if (text.front() == '[') {
        const std::size_t close = text.find(']');
        if (close == std::string_view::npos || close + 2 >= text.size() ||
            text[close + 1] != ':') {
            return false;
        }
        host = text.substr(1, close - 1);
        port_text = text.substr(close + 2);
    } else {
        const std::size_t colon = text.rfind(':');
        if (colon == std::string_view::npos) {
            return false;
        }
        host = text.substr(0, colon);
        port_text = text.substr(colon + 1);
        // 裸 IPv6 必须加方括号，否则端口与地址段无法区分。
        if (host.find(':') != std::string_view::npos) {
            return false;
        }
    }
    if (host.empty() || port_text.empty()) {
        return false;
    }
    std::uint32_t port = 0;
    for (const char character : port_text) {
        if (character < '0' || character > '9') {
            return false;
        }
        port = port * 10U + static_cast<std::uint32_t>(character - '0');
        if (port > 65535U) {
            return false;
        }
    }
    if (port == 0) {
        return false;
    }
    endpoint->host.assign(host.data(), host.size());
    endpoint->port = static_cast<std::uint16_t>(port);
    return true;
}

struct Session::Impl {
    Impl(Session* session, SessionConfig session_config)
        : owner(session), config(std::move(session_config)) {
        control = std::make_unique<SessionControl>(config.control);
        control->on_change = [this](const ControlChange& change) {
            if (owner->on_control) {
                owner->on_control(change);
            }
        };
    }

    ~Impl() {
        // 析构不派发回调：调用方可能已销毁回调捕获的对象。
        if (transport) {
            transport->Close();
        }
        if (heartbeat) {
            heartbeat->Stop();
        }
        if (pairing) {
            pairing->Close();
        }
        state = SessionState::kClosed;
    }

    std::uint64_t NowMs() const {
        return config.now ? config.now() : MonotonicNowMs();
    }

    void Transition(SessionState next) {
        if (state == next) {
            return;
        }
        const SessionState from = state;
        state = next;
        ++stats.state_changes;
        if (owner->on_state) {
            owner->on_state(from, next);
        }
    }

    void RecordError(SessionError error, std::string message) {
        last_error = error;
        last_error_message = std::move(message);
        if (owner->on_error) {
            owner->on_error(last_error, last_error_message);
        }
    }

    /// 配置错误：只记录，不迁移状态（保持 Idle，便于修正后重试）。
    SessionError FailIdle(SessionError error, std::string message) {
        RecordError(error, std::move(message));
        return error;
    }

    /// 运行期错误：记录 → Closing → 释放 → Closed。
    void Fail(SessionError error, std::string message) {
        if (state == SessionState::kClosed) {
            return;
        }
        RecordError(error, std::move(message));
        if (state != SessionState::kClosing) {
            Transition(SessionState::kClosing);
        }
        FinishClose();
    }

    void FinishClose() {
        if (state == SessionState::kClosed) {
            return;
        }
        if (transport) {
            transport->Close();
        }
        if (pairing) {
            pairing->Close();
        }
        if (heartbeat) {
            heartbeat->Stop();
        }
        outbox.clear();
        outbox_offset = 0;
        if (decoder) {
            decoder->Clear();
        }
        Transition(SessionState::kClosed);
    }

    PairingConfig MakePairingConfig() const {
        PairingConfig pairing_config;
        pairing_config.timeout_ms = config.pairing_timeout_ms;
        pairing_config.retransmit_ms = config.pairing_retransmit_ms;
        pairing_config.nonce = config.pairing_nonce;
        pairing_config.pair_id = config.pair_id;
        return pairing_config;
    }

    SessionError ValidateConfig() {
        if (state != SessionState::kIdle) {
            return FailIdle(SessionError::kInvalidState, "Start() requires Idle state");
        }
        if (config.pairing_key.empty()) {
            return FailIdle(SessionError::kInvalidArgument, "pairing_key must not be empty");
        }
        if (config.local_peer_id == 0) {
            return FailIdle(SessionError::kInvalidArgument, "local_peer_id must be non-zero");
        }
        if (config.channel_id == 0 || config.channel_id > 0xffffU) {
            return FailIdle(SessionError::kInvalidArgument, "channel_id must be in 1..65535");
        }
        if (config.max_outbox_frames == 0) {
            return FailIdle(SessionError::kInvalidArgument, "max_outbox_frames must be non-zero");
        }
        if (config.requested_control_mode != ControlMode::kView &&
            config.requested_control_mode != ControlMode::kInteract) {
            return FailIdle(SessionError::kInvalidArgument,
                            "requested_control_mode must be kView or kInteract");
        }
        if (config.endpoint.empty()) {
            if (!config.server) {
                return FailIdle(SessionError::kInvalidEndpoint,
                                "client requires a non-empty endpoint");
            }
        } else if (!ParseEndpoint(config.endpoint, &endpoint)) {
            return FailIdle(SessionError::kInvalidEndpoint,
                            "cannot parse endpoint \"" + config.endpoint + "\"");
        }
        if (config.server && !config.transport_factory) {
            // ITransport 没有 Listen/Accept；服务端必须注入已接受的连接。
            return FailIdle(SessionError::kTransportUnavailable,
                            "server requires transport_factory with an accepted transport");
        }
        return SessionError::kNone;
    }

    SessionError OpenTransport() {
        if (config.transport_factory) {
            try {
                transport = config.transport_factory();
            } catch (const std::exception& error) {
                Fail(SessionError::kTransportUnavailable,
                     std::string("transport factory threw: ") + error.what());
                return last_error;
            } catch (...) {
                Fail(SessionError::kTransportUnavailable,
                     "transport factory threw an unknown exception");
                return last_error;
            }
        } else {
            transport = std::make_unique<net::TcpTransport>();
        }
        if (!transport) {
            Fail(SessionError::kTransportUnavailable, "transport factory returned null");
            return last_error;
        }
        return SessionError::kNone;
    }

    SessionError Start() {
        const SessionError validation = ValidateConfig();
        if (validation != SessionError::kNone) {
            return validation;
        }

        Transition(SessionState::kConnecting);
        if (OpenTransport() != SessionError::kNone) {
            return last_error;
        }
        if (!config.server) {
            const net::Result result = transport->Connect(
                endpoint, std::chrono::milliseconds(config.connect_timeout_ms));
            if (result.code == net::Code::kTimeout) {
                Fail(SessionError::kConnectTimeout,
                     "connect to " + config.endpoint + " timed out after " +
                         std::to_string(config.connect_timeout_ms) + " ms");
                return last_error;
            }
            if (!result.ok()) {
                Fail(SessionError::kConnectFailed,
                     "connect to " + config.endpoint + " failed: " +
                         ErrorText(result.system_error));
                return last_error;
            }
        }

        decoder = std::make_unique<net::FrameDecoder>(net::kFrameMaxPayload, kSessionFormat);
        pairing = std::make_unique<PairingSession>(MakePairingConfig());
        const PairingError pairing_error =
            config.server ? pairing->Register(config.pairing_key, NowMs())
                          : pairing->Connect(config.pairing_key, NowMs());
        if (pairing_error != PairingError::kNone) {
            Fail(SessionError::kPairingFailed,
                 std::string("pairing start failed: ") + PairingErrorName(pairing_error));
            return last_error;
        }

        // 心跳在配对成功后 Start；此时构造只是为了持有同一时间源。
        heartbeat = std::make_unique<Heartbeat>(
            config.heartbeat_interval_ms, config.heartbeat_timeout_ms, config.now,
            [this] { OnHeartbeatTimeout(); });

        // 立即把 REGISTER/CONNECT 发出去，避免调用方必须先 Poll 一次。
        Flush();
        return state == SessionState::kClosed ? last_error : SessionError::kNone;
    }

    bool Poll(int timeout_ms) {
        if (state == SessionState::kClosed) {
            return false;
        }
        ++stats.polls;
        PumpRead(timeout_ms);
        if (state == SessionState::kClosed) {
            return false;
        }
        MaybeAutoAcceptPairing();
        if (state == SessionState::kClosed) {
            return false;
        }
        TickTimers();
        if (state == SessionState::kClosed) {
            return false;
        }
        Flush();
        return state != SessionState::kClosed;
    }

    void PumpRead(int timeout_ms) {
        if (!transport) {
            return;
        }
        std::uint8_t buffer[kReadBufferBytes];
        const std::chrono::milliseconds first_budget(timeout_ms > 0 ? timeout_ms : 0);
        for (int attempt = 0; attempt < kMaxReadsPerPoll; ++attempt) {
            const std::chrono::milliseconds budget =
                attempt == 0 ? first_budget : std::chrono::milliseconds(0);
            const net::Result result = transport->Read(buffer, sizeof(buffer), budget);
            if (result.code == net::Code::kTimeout) {
                return;
            }
            if (result.code == net::Code::kClosed) {
                if (state == SessionState::kClosing) {
                    FinishClose();
                    return;
                }
                Fail(SessionError::kTransportClosed, "peer closed the transport");
                return;
            }
            if (result.code != net::Code::kOk) {
                if (state == SessionState::kClosing) {
                    // 关闭窗口内读失败按正常关闭收尾，不覆盖 last_error。
                    FinishClose();
                    return;
                }
                Fail(SessionError::kTransportClosed,
                     "read failed: " + ErrorText(result.system_error));
                return;
            }
            if (result.bytes == 0) {
                return;
            }
            stats.bytes_received += result.bytes;
            decoder->Append(buffer, result.bytes);
            ProcessFrames();
            if (state == SessionState::kClosed) {
                return;
            }
            // 读满一屏说明传输里可能还有积压：继续用 0 预算取下一批，
            // 把「每次 Poll 只读 4 KiB」的吞吐上限提高 8 倍。
            if (result.bytes < sizeof(buffer)) {
                return;
            }
        }
    }

    void ProcessFrames() {
        while (state != SessionState::kClosed) {
            net::Frame frame;
            const net::FrameStatus status = decoder->Next(&frame);
            if (status == net::FrameStatus::kNeedMoreData) {
                return;
            }
            if (status != net::FrameStatus::kOk) {
                if (state == SessionState::kClosing) {
                    // 关闭窗口内的残余/损坏字节：直接收尾，不把正常关闭
                    // 改写成 kProtocolError。
                    FinishClose();
                    return;
                }
                ++stats.protocol_errors;
                Fail(SessionError::kProtocolError,
                     std::string("frame decode failed: ") + net::FrameStatusName(status));
                return;
            }
            ++stats.frames_received;
            if (heartbeat) {
                heartbeat->OnReceive();
            }
            HandleFrame(frame);
        }
    }

    void HandleFrame(const net::Frame& frame) {
        if (state == SessionState::kClosing) {
            // 优雅关闭窗口：只等对端的 kClose / EOF。FIN 之后对端仍可能
            // 发出在途的 Data/Control/Channel 帧（TCP 半关闭语义），它们
            // 不是协议错误；丢弃而不是关成 kProtocolError。
            if (frame.type == kFrameClose) {
                FinishClose();
            }
            return;
        }
        switch (frame.type) {
            case kFramePairing:
                HandlePairing(frame.payload);
                break;
            case kFrameHeartbeat:
                if (!frame.payload.empty()) {
                    ProtocolFail("heartbeat frame must be empty");
                    return;
                }
                ++stats.heartbeats_received;
                break;
            case kFrameData:
                HandleData(frame.payload);
                break;
            case kFrameControl:
                HandleControl(frame.payload);
                break;
            case kFrameClose:
                HandleRemoteClose();
                break;
            case kFrameChannelOpen:
                HandleChannelOpen(frame.payload);
                break;
            case kFrameChannelAck:
                HandleChannelAck(frame.payload);
                break;
            case kFrameControlRequest:
                HandleControlRequest(frame.payload);
                break;
            default:
                ProtocolFail("unknown frame type " + std::to_string(frame.type));
                break;
        }
    }

    void ProtocolFail(const std::string& message) {
        ++stats.protocol_errors;
        Fail(SessionError::kProtocolError, message);
    }

    void HandlePairing(const std::vector<std::uint8_t>& payload) {
        if (!pairing) {
            ProtocolFail("pairing frame before Start");
            return;
        }
        if (payload.size() != kPairingFrameBytes) {
            ProtocolFail("pairing payload size " + std::to_string(payload.size()) + " != " +
                         std::to_string(kPairingFrameBytes));
            return;
        }
        PairingFrame pairing_frame;
        if (!DecodePairingFrame(payload.data(), payload.size(), &pairing_frame)) {
            ProtocolFail("cannot decode pairing frame");
            return;
        }
        const PairingError error = pairing->HandleFrame(pairing_frame, NowMs());
        if (error == PairingError::kNone || error == PairingError::kAlreadyPaired) {
            CheckPairing();
            return;
        }
        if (error == PairingError::kKeyMismatch) {
            // 对端还需要收到 RESPONSE(kKeyMismatch) 才能给出明确失败原因；
            // 先尽量发出，再关闭本地会话。
            Flush();
            Fail(SessionError::kPairingRejected, "pairing key mismatch");
            return;
        }
        if (error == PairingError::kRejected) {
            Fail(SessionError::kPairingRejected, "pairing rejected by peer");
            return;
        }
        Fail(SessionError::kPairingFailed,
             std::string("pairing failed: ") + PairingErrorName(error));
    }

    void HandleData(const std::vector<std::uint8_t>& payload) {
        if (state != SessionState::kActive) {
            ProtocolFail("data frame before active");
            return;
        }
        std::uint16_t channel_id = 0;
        if (!ReadU16(payload.data(), payload.size(), &channel_id)) {
            ProtocolFail("data frame shorter than channel header");
            return;
        }
        if (channel_id != config.channel_id) {
            ProtocolFail("data frame channel " + std::to_string(channel_id) + " != " +
                         std::to_string(config.channel_id));
            return;
        }
        const std::size_t data_size = payload.size() - kDataHeaderBytes;
        channel.rx_bytes += data_size;
        ++channel.rx_frames;
        ++stats.data_frames_received;
        if (owner->on_data) {
            owner->on_data(channel_id, payload.data() + kDataHeaderBytes, data_size);
        }
    }

    void HandleControl(const std::vector<std::uint8_t>& payload) {
        if (state != SessionState::kActive) {
            ProtocolFail("control frame before active");
            return;
        }
        if (payload.size() != kControlBytes) {
            ProtocolFail("control payload size " + std::to_string(payload.size()) + " != " +
                         std::to_string(kControlBytes));
            return;
        }
        const std::uint8_t action = payload[0];
        std::uint32_t peer_id = 0;
        if (!ReadU32(payload.data() + 1, payload.size() - 1, &peer_id) || peer_id == 0) {
            ProtocolFail("control frame has invalid peer id");
            return;
        }
        // 只接受本会话成员（本端或已完成通道握手、身份确定的对端）。否则
        // 任意对端都能把控制权授予会话外的 id，绕过参与者表与审计。
        if (peer_id != config.local_peer_id &&
            (remote_peer_id == 0 || peer_id != remote_peer_id)) {
            ProtocolFail("control frame targets peer " + std::to_string(peer_id) +
                         " outside this session");
            return;
        }
        const std::uint8_t mode_value = payload[5];
        if (mode_value > static_cast<std::uint8_t>(ControlMode::kInteract)) {
            ProtocolFail("control frame has invalid mode");
            return;
        }
        if (action == kControlGrant) {
            control->Grant(peer_id, static_cast<ControlMode>(mode_value));
            return;
        }
        if (action == kControlRevoke) {
            control->Revoke(peer_id);
            return;
        }
        ProtocolFail("control frame has unknown action " + std::to_string(action));
    }

    void HandleChannelOpen(const std::vector<std::uint8_t>& payload) {
        if (state != SessionState::kPaired && state != SessionState::kActive) {
            ProtocolFail("channel open before paired");
            return;
        }
        if (payload.size() != kChannelOpenBytes) {
            ProtocolFail("channel open payload size " + std::to_string(payload.size()) + " != " +
                         std::to_string(kChannelOpenBytes));
            return;
        }
        std::uint16_t channel_id = 0;
        std::uint32_t peer_id = 0;
        if (!ReadU16(payload.data(), payload.size(), &channel_id) ||
            !ReadU32(payload.data() + 2, payload.size() - 2, &peer_id)) {
            ProtocolFail("cannot decode channel open");
            return;
        }
        if (channel_id != config.channel_id) {
            ProtocolFail("channel open id " + std::to_string(channel_id) + " != " +
                         std::to_string(config.channel_id));
            return;
        }
        if (peer_id == 0 || peer_id == config.local_peer_id) {
            ProtocolFail("channel open carries an invalid peer id");
            return;
        }
        if (remote_peer_id != 0 && remote_peer_id != peer_id) {
            ProtocolFail("channel open peer id changed");
            return;
        }
        remote_peer_id = peer_id;
        if (!open_sent) {
            SendChannelOpen();
        }
        SendChannelAck(channel_id);
        remote_open_received = true;
        channel.open = open_sent && (ack_received || remote_open_received);
        MaybeEnterActive();
    }

    void HandleChannelAck(const std::vector<std::uint8_t>& payload) {
        if (state != SessionState::kPaired && state != SessionState::kActive) {
            ProtocolFail("channel ack before paired");
            return;
        }
        if (payload.size() != kChannelAckBytes) {
            ProtocolFail("channel ack payload size " + std::to_string(payload.size()) + " != " +
                         std::to_string(kChannelAckBytes));
            return;
        }
        std::uint16_t channel_id = 0;
        if (!ReadU16(payload.data(), payload.size(), &channel_id) ||
            channel_id != config.channel_id) {
            ProtocolFail("channel ack id mismatch");
            return;
        }
        if (!open_sent) {
            ProtocolFail("channel ack before open");
            return;
        }
        ack_received = true;
        channel.open = open_sent && (ack_received || remote_open_received);
        MaybeEnterActive();
    }

    void HandleControlRequest(const std::vector<std::uint8_t>& payload) {
        if (state != SessionState::kActive) {
            ProtocolFail("control request before active");
            return;
        }
        if (!config.server) {
            ProtocolFail("control request received by non-arbiter");
            return;
        }
        if (payload.size() != kControlRequestBytes) {
            ProtocolFail("control request payload size " + std::to_string(payload.size()) + " != " +
                         std::to_string(kControlRequestBytes));
            return;
        }
        std::uint32_t peer_id = 0;
        if (!ReadU32(payload.data(), payload.size(), &peer_id) || peer_id == 0) {
            ProtocolFail("control request has invalid peer id");
            return;
        }
        if (peer_id != remote_peer_id) {
            // 请求者只能是已握手完成的对端；冒用其它 id 视为协议错误。
            ProtocolFail("control request from peer " + std::to_string(peer_id) +
                         " that is not the remote peer");
            return;
        }
        const std::uint8_t mode_value = payload[4];
        if (mode_value == static_cast<std::uint8_t>(ControlMode::kNone) ||
            mode_value > static_cast<std::uint8_t>(ControlMode::kInteract)) {
            ProtocolFail("control request has invalid mode");
            return;
        }
        const ControlMode requested_mode = static_cast<ControlMode>(mode_value);
        if (requested_mode == ControlMode::kInteract) {
            if (!control->RequestControl(peer_id)) {
                return;  // 被策略拒绝：请求方通过 on_control 观察不到变更。
            }
        } else if (!control->Grant(peer_id, ControlMode::kView)) {
            // 只读申请不参与「唯一控制器」仲裁；参与者不存在/超限时静默失败。
            return;
        }
        // 广播**实际**授予的模式：不再写死 kInteract，避免 Config/策略与
        // 线上状态不一致（D-02）。
        const ControlMode granted = control->mode(peer_id);
        if (granted == ControlMode::kNone) {
            return;
        }
        BroadcastControl(kControlGrant, peer_id, granted);
    }

    void HandleRemoteClose() {
        if (state == SessionState::kClosed || state == SessionState::kClosing) {
            FinishClose();
            return;
        }
        Transition(SessionState::kClosing);
        if (!closing_frame_sent) {
            SendCloseFrame(SessionError::kNone);
        }
        Flush();
        if (transport) {
            transport->ShutdownWrite();
        }
        FinishClose();
    }

    void SendCloseFrame(SessionError reason) {
        std::vector<std::uint8_t> payload;
        payload.push_back(static_cast<std::uint8_t>(reason));
        if (SendFrame(kFrameClose, std::move(payload))) {
            closing_frame_sent = true;
        }
    }

    void MaybeAutoAcceptPairing() {
        if (!config.server || !config.auto_accept_pairing || !pairing) {
            return;
        }
        if (state != SessionState::kConnecting || !pairing->HasPendingConnect()) {
            return;
        }
        if (pairing->state() != PairingState::kPending) {
            return;
        }
        const PairingError error = pairing->Response(true, NowMs());
        if (error != PairingError::kNone) {
            Fail(SessionError::kPairingFailed,
                 std::string("auto accept failed: ") + PairingErrorName(error));
            return;
        }
        CheckPairing();
    }

    void CheckPairing() {
        if (!pairing) {
            return;
        }
        if (state == SessionState::kConnecting &&
            pairing->state() == PairingState::kPaired) {
            EnterPaired();
        }
        if (state == SessionState::kPaired &&
            pairing->state() == PairingState::kActive) {
            MaybeEnterActive();
        }
    }

    void EnterPaired() {
        Transition(SessionState::kPaired);
        control->Join(config.local_peer_id);
        // 配对层的 RESPONSE/CONFIRM 必须先于通道帧发出：否则对端会在
        // Connecting 状态收到 ChannelOpen，被判为协议错误。
        DrainPairingOutbound();
        SendChannelOpen();
        if (config.server) {
            return;
        }
        // 客户端发出 CONFIRM；服务端收到后才进入 Active（RISK-013）。
        const PairingError error = pairing->Activate(NowMs());
        if (error != PairingError::kNone) {
            Fail(SessionError::kPairingFailed,
                 std::string("activate failed: ") + PairingErrorName(error));
        }
    }

    void MaybeEnterActive() {
        if (state != SessionState::kPaired || !pairing ||
            pairing->state() != PairingState::kActive) {
            return;
        }
        if (!channel.open) {
            return;
        }
        channel.opened_at_ms = NowMs();
        Transition(SessionState::kActive);
        if (remote_peer_id != 0) {
            control->Join(remote_peer_id);
        }
        if (heartbeat) {
            heartbeat->Start(config.heartbeat_interval_ms);
        }
    }

    void SendChannelOpen() {
        if (open_sent) {
            return;
        }
        std::vector<std::uint8_t> payload;
        payload.reserve(kChannelOpenBytes);
        WriteU16(&payload, static_cast<std::uint16_t>(config.channel_id));
        WriteU32(&payload, config.local_peer_id);
        if (!SendFrame(kFrameChannelOpen, std::move(payload))) {
            return;
        }
        open_sent = true;
    }

    void SendChannelAck(std::uint16_t channel_id) {
        std::vector<std::uint8_t> payload;
        payload.reserve(kChannelAckBytes);
        WriteU16(&payload, channel_id);
        SendFrame(kFrameChannelAck, std::move(payload));
    }

    void BroadcastControl(std::uint8_t action, std::uint32_t peer_id, ControlMode mode) {
        std::vector<std::uint8_t> payload;
        payload.reserve(kControlBytes);
        payload.push_back(action);
        WriteU32(&payload, peer_id);
        payload.push_back(static_cast<std::uint8_t>(mode));
        SendFrame(kFrameControl, std::move(payload));
    }

    void TickTimers() {
        const std::uint64_t now = NowMs();
        if (state == SessionState::kClosing) {
            // 关闭窗口内只等对端或超时：配对/心跳/租约定时器不应再把
            // 正常关闭改写成 kPairingTimeout/kHeartbeatTimeout。
            if (now >= close_deadline_ms) {
                FinishClose();
            }
            return;
        }
        if (pairing && state != SessionState::kClosed) {
            const bool changed = pairing->Tick(now);
            if (changed && pairing->state() == PairingState::kClosed &&
                pairing->error() == PairingError::kPeerTimeout) {
                Fail(SessionError::kPairingTimeout,
                     "pairing timed out after " + std::to_string(config.pairing_timeout_ms) + " ms");
                return;
            }
            CheckPairing();
        }
        if (state == SessionState::kClosed) {
            return;
        }
        if (heartbeat && heartbeat->Poll()) {
            if (SendFrame(kFrameHeartbeat, std::vector<std::uint8_t>())) {
                ++stats.heartbeats_sent;
            }
        }
        if (state == SessionState::kClosed) {
            return;
        }
        control->Tick(now);
        if (state == SessionState::kClosing && now >= close_deadline_ms) {
            FinishClose();
        }
    }

    void OnHeartbeatTimeout() {
        Fail(SessionError::kHeartbeatTimeout,
             "no message from peer within " + std::to_string(config.heartbeat_timeout_ms) + " ms");
    }

    /// 编码并入队一帧；队列满或编码失败返回 false。
    bool SendFrame(std::uint16_t type, std::vector<std::uint8_t> payload) {
        if (!transport || state == SessionState::kClosed) {
            return false;
        }
        if (outbox.size() >= config.max_outbox_frames) {
            // 内部帧（ACK/控制广播/心跳）的调用方未必检查返回值；丢帧必须
            // 至少在统计里可见，否则通道握手可能静默卡到超时（D-07）。
            ++stats.dropped_frames;
            return false;
        }
        net::Frame frame;
        frame.type = type;
        frame.payload = std::move(payload);
        std::vector<std::uint8_t> bytes = net::Encode(frame, kSessionFormat);
        if (bytes.empty()) {
            return false;
        }
        outbox.push_back(std::move(bytes));
        return true;
    }

    /// 把配对层出站帧搬到 outbox 队首：配对/确认是通道建立的前置条件，
    /// 必须优先于已入队的数据帧。多个配对帧之间保持原有 FIFO 顺序。
    void DrainPairingOutbound() {
        if (!pairing) {
            return;
        }
        std::deque<std::vector<std::uint8_t>> frames;
        PairingFrame pairing_frame;
        while (pairing->PopOutbound(&pairing_frame)) {
            std::vector<std::uint8_t> payload(kPairingFrameBytes);
            EncodePairingFrame(pairing_frame, payload.data());
            net::Frame frame;
            frame.type = kFramePairing;
            frame.payload = std::move(payload);
            std::vector<std::uint8_t> bytes = net::Encode(frame, kSessionFormat);
            if (bytes.empty()) {
                continue;
            }
            frames.push_back(std::move(bytes));
        }
        if (outbox_offset == 0) {
            for (auto it = frames.rbegin(); it != frames.rend(); ++it) {
                outbox.push_front(std::move(*it));
            }
            return;
        }
        // 队首帧已部分写出：插到它前面会让对端收到「半帧 + 新帧」，只能
        // 排到队尾，等剩余字节发完再发配对帧。
        for (auto& frame : frames) {
            outbox.push_back(std::move(frame));
        }
    }

    void Flush() {
        if (!transport || state == SessionState::kClosed) {
            return;
        }
        DrainPairingOutbound();
        while (!outbox.empty()) {
            std::vector<std::uint8_t>& bytes = outbox.front();
            if (outbox_offset >= bytes.size()) {
                // 防御：偏移等于帧长说明已写完（正常路径会立即 pop）。
                outbox.pop_front();
                outbox_offset = 0;
                continue;
            }
            const net::Result result = transport->WriteFull(
                bytes.data() + outbox_offset, bytes.size() - outbox_offset,
                std::chrono::milliseconds(config.io_timeout_ms));
            if (result.code == net::Code::kOk) {
                stats.bytes_sent += bytes.size() - outbox_offset;
                ++stats.frames_sent;
                outbox.pop_front();
                outbox_offset = 0;
                continue;
            }
            if (result.bytes > 0) {
                const std::size_t remaining = bytes.size() - outbox_offset;
                const std::size_t written = std::min(result.bytes, remaining);
                stats.bytes_sent += written;
                outbox_offset += written;
            }
            if (result.code == net::Code::kTimeout) {
                // 已写出的前缀记在 outbox_offset 里，下次从断点续写；不再
                // erase 前缀（大帧 + 多次超时时是 O(n²) 拷贝），也不会重发
                // 整帧（SESS-01）。
                return;
            }
            if (outbox_offset == bytes.size()) {
                // 防御：已全部写出但状态非 OK（派生类违约）时不重发整帧。
                outbox.pop_front();
                outbox_offset = 0;
            }
            if (state == SessionState::kClosing) {
                FinishClose();
                return;
            }
            Fail(SessionError::kWriteFailed, "write failed: " + ErrorText(result.system_error));
            return;
        }
    }

    SessionError SendMessage(std::uint32_t channel_id, const std::uint8_t* data,
                             std::size_t size) {
        if (state != SessionState::kActive) {
            return SessionError::kInvalidState;
        }
        if (channel_id != config.channel_id) {
            return SessionError::kInvalidArgument;
        }
        if (data == nullptr && size != 0) {
            return SessionError::kInvalidArgument;
        }
        if (size > net::kFrameMaxPayload - kDataHeaderBytes) {
            return SessionError::kMessageTooLarge;
        }
        std::vector<std::uint8_t> payload;
        payload.reserve(kDataHeaderBytes + size);
        WriteU16(&payload, static_cast<std::uint16_t>(channel_id));
        if (size != 0) {
            payload.insert(payload.end(), data, data + size);
        }
        if (!SendFrame(kFrameData, std::move(payload))) {
            return SessionError::kQueueFull;
        }
        ++channel.tx_frames;
        channel.tx_bytes += size;
        ++stats.data_frames_sent;
        return SessionError::kNone;
    }

    SessionError RequestControl(ControlMode mode) {
        if (state != SessionState::kActive) {
            return SessionError::kInvalidState;
        }
        if (mode != ControlMode::kView && mode != ControlMode::kInteract) {
            return SessionError::kInvalidArgument;
        }
        ++stats.control_requests;
        if (config.server) {
            if (mode == ControlMode::kInteract) {
                if (!control->RequestControl(config.local_peer_id)) {
                    return SessionError::kControlDenied;
                }
            } else if (!control->Grant(config.local_peer_id, ControlMode::kView)) {
                return SessionError::kControlDenied;
            }
            // 广播实际模式（可能因策略/状态与请求不同），不硬编码。
            const ControlMode granted = control->mode(config.local_peer_id);
            if (granted != ControlMode::kNone) {
                BroadcastControl(kControlGrant, config.local_peer_id, granted);
            }
            return SessionError::kNone;
        }
        std::vector<std::uint8_t> payload;
        payload.reserve(kControlRequestBytes);
        WriteU32(&payload, config.local_peer_id);
        payload.push_back(static_cast<std::uint8_t>(mode));
        if (!SendFrame(kFrameControlRequest, std::move(payload))) {
            return SessionError::kQueueFull;
        }
        return SessionError::kNone;
    }

    SessionError RequestControl() { return RequestControl(config.requested_control_mode); }

    void Close() {
        if (state == SessionState::kClosed || state == SessionState::kClosing) {
            return;
        }
        if (state == SessionState::kIdle) {
            Transition(SessionState::kClosed);
            return;
        }
        Transition(SessionState::kClosing);
        close_deadline_ms = NowMs() + config.close_timeout_ms;
        if (!closing_frame_sent) {
            SendCloseFrame(SessionError::kNone);
        }
        Flush();
        if (transport && state != SessionState::kClosed) {
            transport->ShutdownWrite();
        }
    }

    // 数据成员
    Session* owner;
    SessionConfig config;
    SessionState state = SessionState::kIdle;
    SessionError last_error = SessionError::kNone;
    std::string last_error_message;
    net::Endpoint endpoint;
    std::unique_ptr<net::ITransport> transport;
    std::unique_ptr<PairingSession> pairing;
    std::unique_ptr<Heartbeat> heartbeat;
    std::unique_ptr<SessionControl> control;
    std::unique_ptr<net::FrameDecoder> decoder;
    std::deque<std::vector<std::uint8_t>> outbox;
    /// 队首帧已写出的前缀字节数；部分写后从这里续写，避免 erase 前缀。
    std::size_t outbox_offset = 0;
    SessionChannel channel;
    SessionStats stats;
    std::uint32_t remote_peer_id = 0;
    bool open_sent = false;
    bool ack_received = false;
    bool remote_open_received = false;
    bool closing_frame_sent = false;
    std::uint64_t close_deadline_ms = 0;
};

Session::Session(SessionConfig config)
    : m_impl(std::make_unique<Impl>(this, std::move(config))) {
    m_impl->channel.id = m_impl->config.channel_id;
}

Session::~Session() = default;

SessionError Session::Start() {
    return m_impl->Start();
}

bool Session::Poll(int timeout_ms) {
    return m_impl->Poll(timeout_ms);
}

void Session::Close() {
    m_impl->Close();
}

SessionError Session::AcceptPairing() {
    if (!m_impl->config.server || !m_impl->pairing) {
        return SessionError::kInvalidState;
    }
    if (m_impl->state != SessionState::kConnecting ||
        !m_impl->pairing->HasPendingConnect()) {
        return SessionError::kInvalidState;
    }
    if (m_impl->pairing->Response(true, m_impl->NowMs()) != PairingError::kNone) {
        return SessionError::kPairingFailed;
    }
    m_impl->CheckPairing();
    return SessionError::kNone;
}

SessionError Session::Send(const std::uint8_t* data, std::size_t size) {
    return m_impl->SendMessage(m_impl->config.channel_id, data, size);
}

SessionError Session::Send(std::string_view data) {
    return m_impl->SendMessage(m_impl->config.channel_id,
                               reinterpret_cast<const std::uint8_t*>(data.data()), data.size());
}

SessionError Session::Send(std::uint32_t channel_id, const std::uint8_t* data,
                           std::size_t size) {
    return m_impl->SendMessage(channel_id, data, size);
}

SessionError Session::RequestControl() {
    return m_impl->RequestControl(m_impl->config.requested_control_mode);
}

SessionError Session::RequestControl(go2cpp::bento::ControlMode mode) {
    return m_impl->RequestControl(mode);
}

bool Session::WaitFor(SessionState target, int timeout_ms) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(timeout_ms, 0));
    while (true) {
        if (m_impl->state == target) {
            return true;
        }
        if (m_impl->state == SessionState::kClosed) {
            return target == SessionState::kClosed;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        m_impl->Poll(1);
    }
}

SessionState Session::state() const noexcept {
    return m_impl->state;
}

SessionError Session::last_error() const noexcept {
    return m_impl->last_error;
}

const std::string& Session::last_error_message() const noexcept {
    return m_impl->last_error_message;
}

const net::Endpoint& Session::endpoint() const noexcept {
    return m_impl->endpoint;
}

const SessionChannel& Session::channel() const noexcept {
    return m_impl->channel;
}

const SessionStats& Session::stats() const noexcept {
    return m_impl->stats;
}

const SessionConfig& Session::config() const noexcept {
    return m_impl->config;
}

std::uint32_t Session::local_peer_id() const noexcept {
    return m_impl->config.local_peer_id;
}

std::uint32_t Session::remote_peer_id() const noexcept {
    return m_impl->remote_peer_id;
}

const net::ITransport* Session::transport() const noexcept {
    return m_impl->transport.get();
}

const SessionControl& Session::control() const noexcept {
    return *m_impl->control;
}

}  // namespace go2cpp::bento::app
