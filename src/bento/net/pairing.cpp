/**
 * @file pairing.cpp
 * @brief 通用配对协议实现：32B 帧编解码、密钥指纹、单次状态迁移与超时重传。
 * @author go2cpp contributors
 * @date 2026-10-28
 * @copyright MIT License
 */

#include "go2cpp/bento/net/pairing.hpp"

#include <atomic>
#include <cstddef>
#include <utility>

namespace go2cpp::bento {
namespace {

// ---------------------------------------------------------------------------
// 大端读写（与 protocol.cpp 同样不依赖 htonl/ntohl，便于手工构造异常帧）
// ---------------------------------------------------------------------------

void WriteU16(std::uint8_t* out, std::uint16_t value) noexcept {
    out[0] = static_cast<std::uint8_t>(value >> 8);
    out[1] = static_cast<std::uint8_t>(value);
}

void WriteU32(std::uint8_t* out, std::uint32_t value) noexcept {
    out[0] = static_cast<std::uint8_t>(value >> 24);
    out[1] = static_cast<std::uint8_t>(value >> 16);
    out[2] = static_cast<std::uint8_t>(value >> 8);
    out[3] = static_cast<std::uint8_t>(value);
}

void WriteU64(std::uint8_t* out, std::uint64_t value) noexcept {
    for (int index = 0; index < 8; ++index) {
        out[index] = static_cast<std::uint8_t>(value >> ((7 - index) * 8));
    }
}

std::uint16_t ReadU16(const std::uint8_t* in) noexcept {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(in[0]) << 8) |
                                      static_cast<std::uint16_t>(in[1]));
}

std::uint32_t ReadU32(const std::uint8_t* in) noexcept {
    return (static_cast<std::uint32_t>(in[0]) << 24) | (static_cast<std::uint32_t>(in[1]) << 16) |
           (static_cast<std::uint32_t>(in[2]) << 8) | static_cast<std::uint32_t>(in[3]);
}

std::uint64_t ReadU64(const std::uint8_t* in) noexcept {
    std::uint64_t value = 0;
    for (int index = 0; index < 8; ++index) {
        value = (value << 8) | static_cast<std::uint64_t>(in[index]);
    }
    return value;
}

/// 64 位雪崩混合（splitmix64 的 finalizer），用于 nonce/pair_id 派生。
std::uint64_t Mix64(std::uint64_t value) noexcept {
    value ^= value >> 30;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27;
    value *= 0x94d049bb133111ebULL;
    value ^= value >> 31;
    return value;
}

/// pair_id 由 (key_hash, nonce) 确定性派生：同一请求重放得到同一 ID。
std::uint32_t DerivePairId(std::uint64_t key_hash, std::uint64_t nonce) noexcept {
    const std::uint64_t mixed =
        Mix64(key_hash ^ Mix64(nonce ^ 0x9e3779b97f4a7c15ULL));
    const std::uint32_t id = static_cast<std::uint32_t>(mixed >> 32) ^
                             static_cast<std::uint32_t>(mixed);
    return id == 0 ? 1U : id;
}

/// 自动 nonce：时间 + 进程内单调计数器，保证同进程内不重复。
std::uint64_t NextNonce(std::uint64_t now_ms) noexcept {
    static std::atomic<std::uint64_t> counter{0};
    const std::uint64_t sequence = counter.fetch_add(1, std::memory_order_relaxed) + 1;
    const std::uint64_t mixed = Mix64(now_ms ^ (sequence * 0x9e3779b97f4a7c15ULL));
    return mixed == 0 ? 1U : mixed;
}

bool IsKnownFrameType(PairingFrameType type) noexcept {
    switch (type) {
        case PairingFrameType::kRegister:
        case PairingFrameType::kConnect:
        case PairingFrameType::kResponse:
        case PairingFrameType::kConfirm:
            return true;
    }
    return false;
}

PairingFrame MakeConnect(std::uint64_t key_hash, std::uint64_t nonce) noexcept {
    PairingFrame frame;
    frame.type = PairingFrameType::kConnect;
    frame.key_hash = key_hash;
    frame.nonce = nonce;
    return frame;
}

PairingFrame MakeResponse(PairingError status, std::uint32_t pair_id, std::uint64_t nonce,
                          std::uint64_t key_hash) noexcept {
    PairingFrame frame;
    frame.type = PairingFrameType::kResponse;
    frame.key_hash = key_hash;
    frame.nonce = nonce;
    frame.pair_id = pair_id;
    frame.status = status;
    return frame;
}

PairingFrame MakeConfirm(std::uint64_t key_hash, std::uint64_t nonce,
                         std::uint32_t pair_id) noexcept {
    PairingFrame frame;
    frame.type = PairingFrameType::kConfirm;
    frame.key_hash = key_hash;
    frame.nonce = nonce;
    frame.pair_id = pair_id;
    return frame;
}

}  // namespace

// ---------------------------------------------------------------------------
// 名字与校验
// ---------------------------------------------------------------------------

const char* PairingStateName(PairingState state) noexcept {
    switch (state) {
        case PairingState::kIdle:
            return "idle";
        case PairingState::kPending:
            return "pending";
        case PairingState::kPaired:
            return "paired";
        case PairingState::kActive:
            return "active";
        case PairingState::kClosed:
            return "closed";
    }
    return "unknown";
}

const char* PairingRoleName(PairingRole role) noexcept {
    switch (role) {
        case PairingRole::kNone:
            return "none";
        case PairingRole::kServer:
            return "server";
        case PairingRole::kClient:
            return "client";
    }
    return "unknown";
}

const char* PairingErrorName(PairingError error) noexcept {
    switch (error) {
        case PairingError::kNone:
            return "none";
        case PairingError::kKeyMismatch:
            return "key_mismatch";
        case PairingError::kPeerTimeout:
            return "peer_timeout";
        case PairingError::kAlreadyRegistered:
            return "already_registered";
        case PairingError::kNotRegistered:
            return "not_registered";
        case PairingError::kAlreadyPaired:
            return "already_paired";
        case PairingError::kInvalidState:
            return "invalid_state";
        case PairingError::kInvalidArgument:
            return "invalid_argument";
        case PairingError::kProtocolVersion:
            return "protocol_version";
        case PairingError::kMalformedFrame:
            return "malformed_frame";
        case PairingError::kRejected:
            return "rejected";
        case PairingError::kClosed:
            return "closed";
    }
    return "unknown";
}

const char* PairingFrameTypeName(PairingFrameType type) noexcept {
    switch (type) {
        case PairingFrameType::kRegister:
            return "register";
        case PairingFrameType::kConnect:
            return "connect";
        case PairingFrameType::kResponse:
            return "response";
        case PairingFrameType::kConfirm:
            return "confirm";
    }
    return "unknown";
}

bool IsKnownPairingError(PairingError error) noexcept {
    return static_cast<std::uint8_t>(error) <= static_cast<std::uint8_t>(PairingError::kClosed);
}

// ---------------------------------------------------------------------------
// 帧编解码
// ---------------------------------------------------------------------------

void EncodePairingFrame(const PairingFrame& frame, std::uint8_t* out) noexcept {
    if (out == nullptr) {
        return;
    }
    WriteU32(out, kPairingMagic);
    out[4] = frame.version;
    out[5] = static_cast<std::uint8_t>(frame.type);
    WriteU16(out + 6, 0);  // flags 保留
    WriteU64(out + 8, frame.key_hash);
    WriteU64(out + 16, frame.nonce);
    WriteU32(out + 24, frame.pair_id);
    WriteU16(out + 28, static_cast<std::uint16_t>(frame.status));
    WriteU16(out + 30, 0);  // reserved
}

bool DecodePairingFrame(const std::uint8_t* data, std::size_t size,
                        PairingFrame* out) noexcept {
    if (data == nullptr || out == nullptr || size < kPairingFrameBytes) {
        return false;
    }
    if (ReadU32(data) != kPairingMagic) {
        return false;
    }
    if (data[4] != kPairingVersion) {
        return false;
    }
    const auto raw_type = static_cast<PairingFrameType>(data[5]);
    if (!IsKnownFrameType(raw_type)) {
        return false;
    }
    if (ReadU16(data + 6) != 0 || ReadU16(data + 30) != 0) {
        return false;
    }
    // 先按 u16 原值判断范围，避免 257 之类的值被截断成合法错误码。
    const std::uint16_t raw_status = ReadU16(data + 28);
    if (raw_status > static_cast<std::uint16_t>(PairingError::kClosed)) {
        return false;
    }
    const auto status = static_cast<PairingError>(raw_status);
    if (!IsKnownPairingError(status)) {
        return false;
    }

    PairingFrame frame;
    frame.type = raw_type;
    frame.version = data[4];
    frame.key_hash = ReadU64(data + 8);
    frame.nonce = ReadU64(data + 16);
    frame.pair_id = ReadU32(data + 24);
    frame.status = status;
    *out = frame;
    return true;
}

// ---------------------------------------------------------------------------
// 密钥指纹策略
// ---------------------------------------------------------------------------

std::uint64_t FixedKeyAuthPolicy::Hash(const std::string& key) noexcept {
    // FNV-1a 64：足够做「是否同一邀请」的快速判断；不是密码学摘要，
    // 真正的认证由安全传输层（MOD-05）负责。
    std::uint64_t hash = 1469598103934665603ULL;
    for (const char ch : key) {
        hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(ch));
        hash *= 1099511628211ULL;
    }
    hash ^= static_cast<std::uint64_t>(key.size()) * 0x9e3779b97f4a7c15ULL;
    return Mix64(hash);
}

bool FixedKeyAuthPolicy::HashKey(const std::string& key, std::uint64_t* out) const {
    if (out == nullptr || key.empty()) {
        return false;
    }
    *out = Hash(key);
    return true;
}

// ---------------------------------------------------------------------------
// PairingSession
// ---------------------------------------------------------------------------

namespace {

/// 进程级默认策略：无状态、线程安全初始化、不产生堆分配。
const PairingAuthPolicy& DefaultPolicy() noexcept {
    static const FixedKeyAuthPolicy policy;
    return policy;
}

}  // namespace

PairingSession::PairingSession(PairingConfig config) : m_config(std::move(config)) {
    if (m_config.timeout_ms == 0) {
        m_config.timeout_ms = 1;
    }
    // 默认策略进程内共享：避免每个会话一次 make_shared（实测占构造成本
    // 的 70% 以上）。注入策略的所有权留在 m_config.policy 里。
    m_policy = m_config.policy ? m_config.policy.get() : &DefaultPolicy();
}

void PairingSession::Transition(PairingState next, PairingError error) {
    if (error != PairingError::kNone) {
        m_error = error;
    }
    if (next == m_state) {
        return;
    }
    if (m_change_count < kMaxChanges) {
        StateChange& change = m_changes[m_change_count];
        change.from = m_state;
        change.to = next;
        change.error = error;
        ++m_change_count;
    }
    m_state = next;
}

void PairingSession::Dispatch() {
    if (m_change_count == 0) {
        return;
    }
    const std::size_t count = m_change_count;
    m_change_count = 0;
    const std::array<StateChange, kMaxChanges> changes = m_changes;
    const auto callback = on_state_change;
    if (!callback) {
        return;
    }
    for (std::size_t index = 0; index < count; ++index) {
        callback(changes[index].from, changes[index].to, changes[index].error);
    }
}

void PairingSession::Enqueue(const PairingFrame& frame) {
    if (m_outbound.size() >= kPairingMaxOutbound) {
        m_outbound.pop_front();
        ++m_stats.queue_drops;
    }
    m_outbound.push_back(frame);
    ++m_stats.frames_sent;
}

void PairingSession::ClearPending() {
    m_outbound.clear();
    m_has_pending_connect = false;
    m_pending_nonce = 0;
    m_confirm_sent = false;
}

PairingError PairingSession::Register(const std::string& key, std::uint64_t now_ms) {
    if (m_state != PairingState::kIdle) {
        return PairingError::kAlreadyRegistered;
    }
    std::uint64_t key_hash = 0;
    if (!m_policy->HashKey(key, &key_hash)) {
        return PairingError::kInvalidArgument;
    }

    m_role = PairingRole::kServer;
    m_key_hash = key_hash;
    m_nonce = 0;
    m_pair_id = 0;
    m_error = PairingError::kNone;
    m_has_pending_connect = false;
    m_confirm_sent = false;
    m_pending_nonce = 0;
    m_deadline_ms = now_ms + m_config.timeout_ms;
    m_next_retransmit_ms = now_ms + m_config.retransmit_ms;
    Transition(PairingState::kPending, PairingError::kNone);
    ++m_stats.registers;
    Dispatch();
    return PairingError::kNone;
}

PairingError PairingSession::Connect(const std::string& key, std::uint64_t now_ms) {
    if (m_state != PairingState::kIdle) {
        return PairingError::kAlreadyRegistered;
    }
    std::uint64_t key_hash = 0;
    if (!m_policy->HashKey(key, &key_hash)) {
        return PairingError::kInvalidArgument;
    }

    m_role = PairingRole::kClient;
    m_key_hash = key_hash;
    m_nonce = m_config.nonce != 0 ? m_config.nonce : NextNonce(now_ms);
    m_pair_id = 0;
    m_error = PairingError::kNone;
    m_has_pending_connect = false;
    m_confirm_sent = false;
    m_pending_nonce = 0;
    m_deadline_ms = now_ms + m_config.timeout_ms;
    m_next_retransmit_ms = now_ms + m_config.retransmit_ms;
    Transition(PairingState::kPending, PairingError::kNone);
    Enqueue(MakeConnect(m_key_hash, m_nonce));
    ++m_stats.connects;
    Dispatch();
    return PairingError::kNone;
}

PairingError PairingSession::Response(bool accept, std::uint64_t now_ms) {
    if (m_role != PairingRole::kServer || m_state != PairingState::kPending ||
        !m_has_pending_connect) {
        return PairingError::kInvalidState;
    }

    const std::uint64_t peer_nonce = m_pending_nonce;
    m_has_pending_connect = false;
    m_pending_nonce = 0;

    if (accept) {
        m_nonce = peer_nonce;
        m_pair_id = m_config.pair_id != 0 ? m_config.pair_id
                                          : DerivePairId(m_key_hash, m_nonce);
        m_deadline_ms = now_ms + m_config.timeout_ms;
        m_next_retransmit_ms = now_ms + m_config.retransmit_ms;
        Enqueue(MakeResponse(PairingError::kNone, m_pair_id, m_nonce, m_key_hash));
        Transition(PairingState::kPaired, PairingError::kNone);
    } else {
        ++m_stats.rejects;
        m_deadline_ms = now_ms + m_config.timeout_ms;
        Enqueue(MakeResponse(PairingError::kRejected, 0, peer_nonce, m_key_hash));
    }
    ++m_stats.responses;
    Dispatch();
    return PairingError::kNone;
}

PairingError PairingSession::Activate(std::uint64_t now_ms) {
    if (m_role != PairingRole::kClient) {
        return PairingError::kInvalidState;
    }
    if (m_state == PairingState::kActive) {
        return PairingError::kNone;
    }
    if (m_state != PairingState::kPaired) {
        return PairingError::kInvalidState;
    }
    // 幂等：重复 Activate 只返回成功，不再重置超时/重传计时器。
    // 若每次调用都重置，应用在驱动循环里每帧调一次就会把 deadline 与
    // next_retransmit 无限推后：CONFIRM 丢失后永不重传、15s 超时永不触发，
    // 有损链路下客户端会永久卡在 Paired。
    if (m_confirm_sent) {
        return PairingError::kNone;
    }
    m_confirm_sent = true;
    Enqueue(MakeConfirm(m_key_hash, m_nonce, m_pair_id));
    ++m_stats.confirms;
    m_deadline_ms = now_ms + m_config.timeout_ms;
    m_next_retransmit_ms = now_ms + m_config.retransmit_ms;
    return PairingError::kNone;
}

PairingError PairingSession::HandleFrame(const PairingFrame& frame, std::uint64_t now_ms) {
    ++m_stats.frames_received;
    const PairingError result = HandleFrameImpl(frame, now_ms);
    Dispatch();
    return result;
}

PairingError PairingSession::HandleFrameImpl(const PairingFrame& frame, std::uint64_t now_ms) {
    if (frame.version != kPairingVersion) {
        return PairingError::kProtocolVersion;
    }
    if (!IsKnownPairingError(frame.status)) {
        return PairingError::kMalformedFrame;
    }
    if (frame.type == PairingFrameType::kRegister) {
        // REGISTER 是本地 API（密钥不上线）；收到一律视为非法。
        return PairingError::kInvalidState;
    }
    if (frame.type != PairingFrameType::kResponse && frame.status != PairingError::kNone) {
        // CONNECT/CONFIRM 不携带结果码；带非零 status 视为畸形帧。
        return PairingError::kMalformedFrame;
    }
    if (m_role == PairingRole::kNone) {
        return frame.type == PairingFrameType::kConnect ? PairingError::kNotRegistered
                                                        : PairingError::kInvalidState;
    }
    if (m_state == PairingState::kClosed) {
        return PairingError::kClosed;
    }

    switch (frame.type) {
        case PairingFrameType::kConnect:
            return HandleConnect(frame, now_ms);
        case PairingFrameType::kResponse:
            return HandleResponse(frame, now_ms);
        case PairingFrameType::kConfirm:
            return HandleConfirm(frame, now_ms);
        case PairingFrameType::kRegister:
            return PairingError::kInvalidState;
    }
    return PairingError::kInvalidState;
}

PairingError PairingSession::HandleConnect(const PairingFrame& frame, std::uint64_t now_ms) {
    if (m_role != PairingRole::kServer) {
        return PairingError::kInvalidState;
    }

    // 密钥不匹配：拒绝但不破坏本端状态（错误请求不能把服务端打偏）。
    if (frame.key_hash != m_key_hash) {
        ++m_stats.key_mismatches;
        Enqueue(MakeResponse(PairingError::kKeyMismatch, 0, frame.nonce, m_key_hash));
        ++m_stats.responses;
        return PairingError::kKeyMismatch;
    }

    // 已配对/已激活：同一 nonce 的重复 CONNECT 是 response 丢失后的重试，
    // 幂等重放同一 pair_id 的 RESPONSE（RISK-013：不产生第二个会话）。
    if ((m_state == PairingState::kPaired || m_state == PairingState::kActive) &&
        frame.nonce == m_nonce) {
        Enqueue(MakeResponse(PairingError::kNone, m_pair_id, m_nonce, m_key_hash));
        ++m_stats.responses;
        ++m_stats.replays;
        return PairingError::kNone;
    }
    if (m_state == PairingState::kPaired || m_state == PairingState::kActive) {
        ++m_stats.rejects;
        Enqueue(MakeResponse(PairingError::kRejected, 0, frame.nonce, m_key_hash));
        ++m_stats.responses;
        return PairingError::kRejected;
    }

    // Pending：先到先得（AX-009：两个 Client 同时 connect 同一 key）。
    if (m_has_pending_connect) {
        if (frame.nonce == m_pending_nonce) {
            return PairingError::kNone;  // 同一请求重复到达，忽略
        }
        ++m_stats.rejects;
        Enqueue(MakeResponse(PairingError::kRejected, 0, frame.nonce, m_key_hash));
        ++m_stats.responses;
        return PairingError::kRejected;
    }

    m_has_pending_connect = true;
    m_pending_nonce = frame.nonce;
    m_deadline_ms = now_ms + m_config.timeout_ms;
    return PairingError::kNone;
}

PairingError PairingSession::HandleResponse(const PairingFrame& frame, std::uint64_t now_ms) {
    if (m_role != PairingRole::kClient) {
        return PairingError::kInvalidState;
    }
    if (m_state == PairingState::kActive) {
        return PairingError::kNone;  // 重复响应，忽略
    }
    if (m_state == PairingState::kPaired) {
        // 重复响应：pair_id 相同则重放一次 CONFIRM（若已 Activate）。
        if (frame.status == PairingError::kNone && frame.pair_id == m_pair_id &&
            frame.key_hash == m_key_hash && m_confirm_sent) {
            Enqueue(MakeConfirm(m_key_hash, m_nonce, m_pair_id));
            ++m_stats.retransmits;
        }
        return PairingError::kNone;
    }

    // Pending：只接受回显本端 nonce 的响应；陈旧/伪造响应丢弃并继续等。
    if (frame.nonce != m_nonce) {
        return PairingError::kNone;
    }
    if (frame.status == PairingError::kNone) {
        if (frame.pair_id == 0) {
            return PairingError::kMalformedFrame;
        }
        // 成功响应必须回显本端 key_hash：非本会话的响应（串话/伪造）不能
        // 把客户端推进 Paired。失败响应不回显本端指纹，故只在此处校验。
        if (frame.key_hash != m_key_hash) {
            return PairingError::kKeyMismatch;
        }
        m_pair_id = frame.pair_id;
        m_outbound.clear();  // 丢弃尚未发出的重复 CONNECT
        m_deadline_ms = now_ms + m_config.timeout_ms;
        m_next_retransmit_ms = now_ms + m_config.retransmit_ms;
        Transition(PairingState::kPaired, PairingError::kNone);
        return PairingError::kNone;
    }

    // 失败响应：关闭并记录原因（密钥不匹配/拒绝）。
    if (frame.status == PairingError::kKeyMismatch) {
        ++m_stats.key_mismatches;
    }
    ClearPending();
    m_deadline_ms = 0;
    m_next_retransmit_ms = 0;
    Transition(PairingState::kClosed, frame.status);
    return frame.status;
}

PairingError PairingSession::HandleConfirm(const PairingFrame& frame, std::uint64_t now_ms) {
    // CONFIRM 与 ACK 都不携带结果码；now_ms 仅为接口一致性保留。
    (void)now_ms;
    if (frame.status != PairingError::kNone) {
        return PairingError::kMalformedFrame;
    }
    if (m_role == PairingRole::kClient) {
        // 客户端收到服务端 ACK：双方都确认后才进入 Active。
        if (m_state == PairingState::kActive) {
            return PairingError::kNone;
        }
        if (m_state != PairingState::kPaired) {
            return PairingError::kInvalidState;
        }
        // 未发出 CONFIRM 就收到 ACK 属于异常序列（服务端只有收到 CONFIRM
        // 才会回 ACK），拒绝它可以避免伪造 ACK 绕过「双向确认」。
        if (!m_confirm_sent) {
            return PairingError::kInvalidState;
        }
        if (frame.key_hash != m_key_hash || frame.nonce != m_nonce ||
            frame.pair_id != m_pair_id) {
            return PairingError::kKeyMismatch;
        }
        m_deadline_ms = 0;
        m_next_retransmit_ms = 0;
        Transition(PairingState::kActive, PairingError::kNone);
        return PairingError::kNone;
    }

    if (m_role != PairingRole::kServer) {
        return PairingError::kInvalidState;
    }
    if (m_state == PairingState::kActive) {
        if (frame.key_hash == m_key_hash && frame.nonce == m_nonce &&
            frame.pair_id == m_pair_id) {
            Enqueue(MakeConfirm(m_key_hash, m_nonce, m_pair_id));
            ++m_stats.replays;
            return PairingError::kNone;
        }
        return PairingError::kKeyMismatch;
    }
    if (m_state != PairingState::kPaired) {
        return PairingError::kInvalidState;
    }
    if (frame.key_hash != m_key_hash || frame.nonce != m_nonce ||
        frame.pair_id != m_pair_id) {
        return PairingError::kKeyMismatch;
    }

    // 收到客户端确认：服务端先 Active，再回 ACK 让客户端也进入 Active。
    m_deadline_ms = 0;
    m_next_retransmit_ms = 0;
    Transition(PairingState::kActive, PairingError::kNone);
    Enqueue(MakeConfirm(m_key_hash, m_nonce, m_pair_id));
    return PairingError::kNone;
}

bool PairingSession::Tick(std::uint64_t now_ms) {
    const PairingState before = m_state;
    if (m_state != PairingState::kPending && m_state != PairingState::kPaired) {
        Dispatch();
        return false;
    }

    if (now_ms >= m_deadline_ms) {
        ++m_stats.timeouts;
        ClearPending();
        m_deadline_ms = 0;
        m_next_retransmit_ms = 0;
        Transition(PairingState::kClosed, PairingError::kPeerTimeout);
        Dispatch();
        return m_state != before;
    }

    if (now_ms >= m_next_retransmit_ms) {
        m_next_retransmit_ms = now_ms + m_config.retransmit_ms;
        if (m_role == PairingRole::kClient && m_state == PairingState::kPending) {
            Enqueue(MakeConnect(m_key_hash, m_nonce));
            ++m_stats.retransmits;
        } else if (m_role == PairingRole::kClient && m_state == PairingState::kPaired &&
                   m_confirm_sent) {
            Enqueue(MakeConfirm(m_key_hash, m_nonce, m_pair_id));
            ++m_stats.retransmits;
        } else if (m_role == PairingRole::kServer && m_state == PairingState::kPaired) {
            Enqueue(MakeResponse(PairingError::kNone, m_pair_id, m_nonce, m_key_hash));
            ++m_stats.responses;
            ++m_stats.retransmits;
        }
    }
    Dispatch();
    return m_state != before;
}

bool PairingSession::PopOutbound(PairingFrame* out) {
    if (out == nullptr || m_outbound.empty()) {
        return false;
    }
    *out = m_outbound.front();
    m_outbound.pop_front();
    return true;
}

bool PairingSession::Close() {
    if (m_state == PairingState::kIdle || m_state == PairingState::kClosed) {
        return false;
    }
    ClearPending();
    m_deadline_ms = 0;
    m_next_retransmit_ms = 0;
    Transition(PairingState::kClosed, PairingError::kNone);
    Dispatch();
    return true;
}

void PairingSession::Reset() {
    m_role = PairingRole::kNone;
    m_state = PairingState::kIdle;
    m_error = PairingError::kNone;
    m_key_hash = 0;
    m_nonce = 0;
    m_pair_id = 0;
    m_deadline_ms = 0;
    m_next_retransmit_ms = 0;
    m_has_pending_connect = false;
    m_confirm_sent = false;
    m_pending_nonce = 0;
    m_outbound.clear();
    m_change_count = 0;
}

}  // namespace go2cpp::bento
