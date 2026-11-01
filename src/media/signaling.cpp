/**
 * @file signaling.cpp
 * @brief SDP 子集解析/序列化、ICE 候选、信令消息与 offer/answer 状态机实现。
 * @details 解析器是严格的**行状态机**：
 *
 *          - 会话级行（v/o/s/t/a=group）只允许出现在第一个 `m=` 之前；
 *          - 媒体级行（m/c/a）归入当前媒体段，`m=` 出现即切段；
 *          - 未知属性（`a=rtcp-mux` 等）直接忽略，保证与浏览器 SDP 互通；
 *          - 已知但非法的行（坏 candidate、坏 rtpmap、坏方向）整体失败，
 *            不做「尽量解析」——半套候选比没有候选更难排查。
 *
 *          序列化按固定顺序输出，保证同一描述多次 `ToSdp()` 逐字节一致，
 *          测试可以对整段文本做断言。
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include "go2cpp/media/signaling.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace go2cpp::media {
namespace {

constexpr std::string_view kSignalMagic = "g2p-signal/1";

// ---------------------------------------------------------------------------
// 字符串/数字工具
// ---------------------------------------------------------------------------

std::string_view TrimView(std::string_view text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
        --end;
    }
    return text.substr(begin, end - begin);
}

std::string ToLower(std::string_view text) {
    std::string lowered(text);
    for (char& ch : lowered) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return lowered;
}

bool StartsWith(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

bool ParseUint(std::string_view text, std::uint64_t limit, std::uint64_t* value) {
    if (text.empty() || text.size() > 20) {
        return false;
    }
    std::uint64_t parsed = 0;
    for (const char ch : text) {
        if (ch < '0' || ch > '9') {
            return false;
        }
        parsed = parsed * 10U + static_cast<std::uint64_t>(ch - '0');
        if (parsed > limit) {
            return false;
        }
    }
    *value = parsed;
    return true;
}

bool ParseU8(std::string_view text, std::uint8_t* value) {
    std::uint64_t parsed = 0;
    if (!ParseUint(text, 255, &parsed)) {
        return false;
    }
    *value = static_cast<std::uint8_t>(parsed);
    return true;
}

bool ParseU16(std::string_view text, std::uint16_t* value) {
    std::uint64_t parsed = 0;
    if (!ParseUint(text, 65535, &parsed)) {
        return false;
    }
    *value = static_cast<std::uint16_t>(parsed);
    return true;
}

bool ParseU32(std::string_view text, std::uint32_t* value) {
    std::uint64_t parsed = 0;
    if (!ParseUint(text, 0xFFFFFFFFULL, &parsed)) {
        return false;
    }
    *value = static_cast<std::uint32_t>(parsed);
    return true;
}

bool ParseU64(std::string_view text, std::uint64_t* value) {
    return ParseUint(text, 0xFFFFFFFFFFFFFFFFULL, value);
}

/// @brief 按空白切分；token 只引用输入，不分配字符串（控制面解析热点）。
std::vector<std::string_view> SplitWhitespace(std::string_view text) {
    std::vector<std::string_view> tokens;
    tokens.reserve(8);
    std::size_t index = 0;
    while (index < text.size()) {
        while (index < text.size() && std::isspace(static_cast<unsigned char>(text[index])) != 0) {
            ++index;
        }
        const std::size_t begin = index;
        while (index < text.size() && std::isspace(static_cast<unsigned char>(text[index])) == 0) {
            ++index;
        }
        if (index > begin) {
            tokens.push_back(text.substr(begin, index - begin));
        }
    }
    return tokens;
}

std::vector<std::string_view> SplitChar(std::string_view text, char separator) {
    std::vector<std::string_view> parts;
    std::size_t begin = 0;
    while (begin <= text.size()) {
        const std::size_t end = text.find(separator, begin);
        if (end == std::string_view::npos) {
            parts.push_back(text.substr(begin));
            break;
        }
        parts.push_back(text.substr(begin, end - begin));
        begin = end + 1;
    }
    return parts;
}

/// @brief 按 `\n` 切行；行视图直接指向输入 SDP，避免逐行拷贝。
std::vector<std::string_view> SplitLines(std::string_view text) {
    std::vector<std::string_view> lines;
    lines.reserve(text.size() / 32 + 1);
    std::size_t begin = 0;
    while (begin < text.size()) {
        std::size_t end = text.find('\n', begin);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        std::string_view line = text.substr(begin, end - begin);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        lines.push_back(line);
        begin = end + 1;
    }
    return lines;
}

void SetError(std::string* error, std::string_view message) {
    if (error != nullptr) {
        *error = std::string(message);
    }
}

bool IsValidAddress(std::string_view address) {
    if (address.empty() || address.size() > 64) {
        return false;
    }
    if (address.find(':') != std::string_view::npos) {
        std::size_t colons = 0;
        bool has_hex = false;
        for (const char ch : address) {
            if (ch == ':') {
                ++colons;
                continue;
            }
            if (ch == '.') {
                continue;
            }
            if (ch == '%') {
                return has_hex;
            }
            if (std::isxdigit(static_cast<unsigned char>(ch)) == 0) {
                return false;
            }
            has_hex = true;
        }
        return colons >= 2 && has_hex;
    }
    const std::vector<std::string_view> parts = SplitChar(address, '.');
    if (parts.size() != 4) {
        return false;
    }
    for (const std::string_view part : parts) {
        std::uint64_t value = 0;
        if (part.empty() || !ParseUint(part, 255, &value)) {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// 随机凭据（ICE ufrag/pwd、session id、DTLS 指纹占位）
// ---------------------------------------------------------------------------

std::mt19937& Rng() {
    static std::mt19937 engine([] {
        std::random_device device;
        std::seed_seq seed{device(), device(), device(), device()};
        return std::mt19937(seed);
    }());
    return engine;
}

std::string RandomToken(std::size_t length, std::string_view alphabet) {
    std::uniform_int_distribution<std::size_t> distribution(0, alphabet.size() - 1);
    std::string token(length, ' ');
    for (char& ch : token) {
        ch = alphabet[distribution(Rng())];
    }
    return token;
}

std::string GenerateSessionId() {
    return RandomToken(16, "0123456789abcdef");
}

std::string GenerateUfrag() {
    return RandomToken(8, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789+/");
}

std::string GeneratePwd() {
    return RandomToken(24, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789+/");
}

std::string GenerateFingerprint() {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::uniform_int_distribution<int> distribution(0, 255);
    std::string fingerprint = "sha-256 ";
    for (int index = 0; index < 32; ++index) {
        const int byte = distribution(Rng());
        if (index != 0) {
            fingerprint.push_back(':');
        }
        fingerprint.push_back(kHex[(byte >> 4) & 0x0F]);
        fingerprint.push_back(kHex[byte & 0x0F]);
    }
    return fingerprint;
}

std::uint64_t SteadyNowMs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

// ---------------------------------------------------------------------------
// 默认媒体与描述填充
// ---------------------------------------------------------------------------

MediaDescription DefaultVideoMedia() {
    MediaDescription media;
    media.kind = MediaKind::kVideo;
    media.mid = "0";
    media.direction = SdpDirection::kSendRecv;
    media.codecs.push_back(RtpCodec{96, "H264", 90000, 1, "packetization-mode=1"});
    return media;
}

MediaDescription DefaultAudioMedia() {
    MediaDescription media;
    media.kind = MediaKind::kAudio;
    media.mid = "1";
    media.direction = SdpDirection::kSendRecv;
    media.codecs.push_back(RtpCodec{111, "opus", 48000, 2, "minptime=10;useinbandfec=1"});
    return media;
}

std::vector<MediaDescription> DefaultMediaList() {
    std::vector<MediaDescription> media;
    media.push_back(DefaultVideoMedia());
    media.push_back(DefaultAudioMedia());
    return media;
}

std::string ConnectionLine(std::string_view address) {
    const char* version = address.find(':') == std::string_view::npos ? "IP4" : "IP6";
    std::ostringstream line;
    line << "c=IN " << version << ' ' << address << '\n';
    return line.str();
}

std::string RtpmapLine(const RtpCodec& codec) {
    std::ostringstream line;
    line << "a=rtpmap:" << static_cast<int>(codec.payload_type) << ' ' << codec.encoding_name << '/'
         << codec.clock_rate;
    if (codec.channels > 1) {
        line << '/' << static_cast<int>(codec.channels);
    }
    line << '\n';
    return line.str();
}

bool IsSetupCompatible(DtlsSetup offer_setup, DtlsSetup answer_setup) noexcept {
    switch (offer_setup) {
        case DtlsSetup::kActpass:
            return answer_setup == DtlsSetup::kActive || answer_setup == DtlsSetup::kPassive;
        case DtlsSetup::kActive:
            return answer_setup == DtlsSetup::kPassive;
        case DtlsSetup::kPassive:
            return answer_setup == DtlsSetup::kActive;
    }
    return false;
}

void AddCandidateToDescription(SessionDescription* description, const IceCandidate& candidate) {
    for (MediaDescription& media : description->media) {
        if (std::find(media.candidates.begin(), media.candidates.end(), candidate) ==
            media.candidates.end()) {
            media.candidates.push_back(candidate);
        }
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// 枚举名
// ---------------------------------------------------------------------------

const char* IceCandidateTypeName(IceCandidateType type) noexcept {
    switch (type) {
        case IceCandidateType::kHost:
            return "host";
        case IceCandidateType::kSrflx:
            return "srflx";
        case IceCandidateType::kPrflx:
            return "prflx";
        case IceCandidateType::kRelay:
            return "relay";
    }
    return "unknown";
}

std::optional<IceCandidateType> ParseIceCandidateType(std::string_view name) {
    const std::string lowered = ToLower(name);
    if (lowered == "host") {
        return IceCandidateType::kHost;
    }
    if (lowered == "srflx") {
        return IceCandidateType::kSrflx;
    }
    if (lowered == "prflx") {
        return IceCandidateType::kPrflx;
    }
    if (lowered == "relay") {
        return IceCandidateType::kRelay;
    }
    return std::nullopt;
}

std::uint32_t IceCandidateTypePreference(IceCandidateType type) noexcept {
    switch (type) {
        case IceCandidateType::kHost:
            return 126;
        case IceCandidateType::kPrflx:
            return 110;
        case IceCandidateType::kSrflx:
            return 100;
        case IceCandidateType::kRelay:
            return 0;
    }
    return 0;
}

std::uint32_t ComputeIceCandidatePriority(IceCandidateType type, std::uint32_t local_pref,
                                          std::uint8_t component) noexcept {
    const std::uint32_t type_pref = IceCandidateTypePreference(type);
    const std::uint32_t local = local_pref & 0xFFFFU;
    const std::uint32_t component_value = 256U - (component == 0 ? 1U : component);
    return (type_pref << 24U) | (local << 8U) | (component_value & 0xFFU);
}

const char* SdpTypeName(SdpType type) noexcept {
    return type == SdpType::kOffer ? "offer" : "answer";
}

std::optional<SdpType> ParseSdpType(std::string_view name) {
    const std::string lowered = ToLower(name);
    if (lowered == "offer") {
        return SdpType::kOffer;
    }
    if (lowered == "answer") {
        return SdpType::kAnswer;
    }
    return std::nullopt;
}

const char* MediaKindName(MediaKind kind) noexcept {
    switch (kind) {
        case MediaKind::kVideo:
            return "video";
        case MediaKind::kAudio:
            return "audio";
        case MediaKind::kApplication:
            return "application";
    }
    return "unknown";
}

std::optional<MediaKind> ParseMediaKind(std::string_view name) {
    const std::string lowered = ToLower(name);
    if (lowered == "video") {
        return MediaKind::kVideo;
    }
    if (lowered == "audio") {
        return MediaKind::kAudio;
    }
    if (lowered == "application") {
        return MediaKind::kApplication;
    }
    return std::nullopt;
}

const char* SdpDirectionName(SdpDirection direction) noexcept {
    switch (direction) {
        case SdpDirection::kSendRecv:
            return "sendrecv";
        case SdpDirection::kSendOnly:
            return "sendonly";
        case SdpDirection::kRecvOnly:
            return "recvonly";
        case SdpDirection::kInactive:
            return "inactive";
    }
    return "unknown";
}

std::optional<SdpDirection> ParseSdpDirection(std::string_view name) {
    const std::string lowered = ToLower(name);
    if (lowered == "sendrecv") {
        return SdpDirection::kSendRecv;
    }
    if (lowered == "sendonly") {
        return SdpDirection::kSendOnly;
    }
    if (lowered == "recvonly") {
        return SdpDirection::kRecvOnly;
    }
    if (lowered == "inactive") {
        return SdpDirection::kInactive;
    }
    return std::nullopt;
}

SdpDirection ReverseDirection(SdpDirection direction) noexcept {
    switch (direction) {
        case SdpDirection::kSendRecv:
            return SdpDirection::kSendRecv;
        case SdpDirection::kSendOnly:
            return SdpDirection::kRecvOnly;
        case SdpDirection::kRecvOnly:
            return SdpDirection::kSendOnly;
        case SdpDirection::kInactive:
            return SdpDirection::kInactive;
    }
    return SdpDirection::kInactive;
}

bool IsDirectionCompatible(SdpDirection offer, SdpDirection answer) noexcept {
    switch (offer) {
        case SdpDirection::kSendRecv:
            return true;
        case SdpDirection::kSendOnly:
            return answer == SdpDirection::kRecvOnly || answer == SdpDirection::kInactive;
        case SdpDirection::kRecvOnly:
            return answer == SdpDirection::kSendOnly || answer == SdpDirection::kInactive;
        case SdpDirection::kInactive:
            return answer == SdpDirection::kInactive;
    }
    return false;
}

const char* DtlsSetupName(DtlsSetup setup) noexcept {
    switch (setup) {
        case DtlsSetup::kActpass:
            return "actpass";
        case DtlsSetup::kActive:
            return "active";
        case DtlsSetup::kPassive:
            return "passive";
    }
    return "unknown";
}

std::optional<DtlsSetup> ParseDtlsSetup(std::string_view name) {
    const std::string lowered = ToLower(name);
    if (lowered == "actpass") {
        return DtlsSetup::kActpass;
    }
    if (lowered == "active") {
        return DtlsSetup::kActive;
    }
    if (lowered == "passive") {
        return DtlsSetup::kPassive;
    }
    return std::nullopt;
}

DtlsSetup NegotiateAnswerSetup(DtlsSetup offer_setup, DtlsSetup preferred) noexcept {
    switch (offer_setup) {
        case DtlsSetup::kActpass:
            return preferred == DtlsSetup::kActpass ? DtlsSetup::kActive : preferred;
        case DtlsSetup::kActive:
            return DtlsSetup::kPassive;
        case DtlsSetup::kPassive:
            return DtlsSetup::kActive;
    }
    return DtlsSetup::kActive;
}

const char* SignalingRoleName(SignalingRole role) noexcept {
    return role == SignalingRole::kOfferer ? "offerer" : "answerer";
}

const char* SignalingStateName(SignalingState state) noexcept {
    switch (state) {
        case SignalingState::kIdle:
            return "idle";
        case SignalingState::kOffering:
            return "offering";
        case SignalingState::kAnswering:
            return "answering";
        case SignalingState::kConnected:
            return "connected";
        case SignalingState::kFailed:
            return "failed";
        case SignalingState::kClosed:
            return "closed";
    }
    return "unknown";
}

const char* SignalingStatusName(SignalingStatus status) noexcept {
    switch (status) {
        case SignalingStatus::kOk:
            return "ok";
        case SignalingStatus::kInvalidState:
            return "invalid-state";
        case SignalingStatus::kInvalidRole:
            return "invalid-role";
        case SignalingStatus::kInvalidArgument:
            return "invalid-argument";
        case SignalingStatus::kInvalidDescription:
            return "invalid-description";
        case SignalingStatus::kNoLocalDescription:
            return "no-local-description";
        case SignalingStatus::kNoRemoteDescription:
            return "no-remote-description";
        case SignalingStatus::kMediaMismatch:
            return "media-mismatch";
        case SignalingStatus::kCandidateRejected:
            return "candidate-rejected";
        case SignalingStatus::kAlreadyConnected:
            return "already-connected";
        case SignalingStatus::kTimeout:
            return "timeout";
        case SignalingStatus::kClosed:
            return "closed";
    }
    return "unknown";
}

const char* SignalingMessageTypeName(SignalingMessageType type) noexcept {
    switch (type) {
        case SignalingMessageType::kOffer:
            return "offer";
        case SignalingMessageType::kAnswer:
            return "answer";
        case SignalingMessageType::kCandidate:
            return "candidate";
        case SignalingMessageType::kEndOfCandidates:
            return "end-of-candidates";
        case SignalingMessageType::kBye:
            return "bye";
    }
    return "unknown";
}

std::optional<SignalingMessageType> ParseSignalingMessageType(std::string_view name) {
    const std::string lowered = ToLower(name);
    if (lowered == "offer") {
        return SignalingMessageType::kOffer;
    }
    if (lowered == "answer") {
        return SignalingMessageType::kAnswer;
    }
    if (lowered == "candidate") {
        return SignalingMessageType::kCandidate;
    }
    if (lowered == "end-of-candidates") {
        return SignalingMessageType::kEndOfCandidates;
    }
    if (lowered == "bye") {
        return SignalingMessageType::kBye;
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// ICE 候选
// ---------------------------------------------------------------------------

std::string IceCandidate::ToString() const {
    std::ostringstream line;
    line << "candidate:" << (foundation.empty() ? "0" : foundation) << ' ' << component << ' '
         << (transport.empty() ? "udp" : transport) << ' ' << priority << ' ' << address << ' '
         << port << " typ " << IceCandidateTypeName(type);
    if (!related_address.empty() && related_port != 0) {
        line << " raddr " << related_address << " rport " << related_port;
    }
    if (generation != 0) {
        line << " generation " << generation;
    }
    return line.str();
}

std::string IceCandidate::ToAttribute() const {
    return "a=" + ToString();
}

std::optional<IceCandidate> IceCandidate::Parse(std::string_view line, std::string* error) {
    std::string_view text = TrimView(line);
    if (StartsWith(text, "a=")) {
        text.remove_prefix(2);
    }
    if (!StartsWith(text, "candidate:")) {
        SetError(error, "候选行缺少 candidate: 前缀");
        return std::nullopt;
    }
    text.remove_prefix(10);
    const std::vector<std::string_view> tokens = SplitWhitespace(text);
    if (tokens.size() < 8) {
        SetError(error, "候选行字段不足（至少 8 个）");
        return std::nullopt;
    }
    IceCandidate candidate;
    candidate.foundation = tokens[0];
    std::uint64_t component = 0;
    if (!ParseUint(tokens[1], 256, &component) || component == 0) {
        SetError(error, "候选 component 非法");
        return std::nullopt;
    }
    candidate.component = static_cast<std::uint32_t>(component);
    candidate.transport = ToLower(tokens[2]);
    if (candidate.transport.empty()) {
        SetError(error, "候选 transport 为空");
        return std::nullopt;
    }
    if (!ParseU32(tokens[3], &candidate.priority)) {
        SetError(error, "候选 priority 非法");
        return std::nullopt;
    }
    candidate.address = tokens[4];
    if (!ParseU16(tokens[5], &candidate.port) || candidate.port == 0) {
        SetError(error, "候选 port 非法");
        return std::nullopt;
    }
    if (tokens[6] != "typ") {
        SetError(error, "候选行缺少 typ 关键字");
        return std::nullopt;
    }
    const auto type = ParseIceCandidateType(tokens[7]);
    if (!type.has_value()) {
        SetError(error, "候选类型未知");
        return std::nullopt;
    }
    candidate.type = *type;
    for (std::size_t index = 8; index < tokens.size();) {
        const std::string_view key = tokens[index];
        if (key == "raddr" || key == "rport" || key == "generation") {
            if (index + 1 >= tokens.size()) {
                SetError(error, "候选扩展字段缺少值");
                return std::nullopt;
            }
            const std::string_view value = tokens[index + 1];
            if (key == "raddr") {
                candidate.related_address = value;
            } else if (key == "rport") {
                if (!ParseU16(value, &candidate.related_port)) {
                    SetError(error, "候选 rport 非法");
                    return std::nullopt;
                }
            } else if (!ParseU32(value, &candidate.generation)) {
                SetError(error, "候选 generation 非法");
                return std::nullopt;
            }
            index += 2;
            continue;
        }
        // ufrag / network-cost / tcptype 等扩展：有值则跳过两段，无值跳一段。
        index += (index + 1 < tokens.size()) ? 2 : 1;
    }
    if (!IsValidIceCandidate(candidate, error)) {
        return std::nullopt;
    }
    return candidate;
}

bool IceCandidate::operator==(const IceCandidate& other) const noexcept {
    return type == other.type && foundation == other.foundation && component == other.component &&
           transport == other.transport && priority == other.priority && address == other.address &&
           port == other.port && related_address == other.related_address &&
           related_port == other.related_port && generation == other.generation;
}

bool IsValidIceCandidate(const IceCandidate& candidate, std::string* error) {
    if (!IsValidAddress(candidate.address)) {
        SetError(error, "候选地址非法");
        return false;
    }
    if (candidate.port == 0) {
        SetError(error, "候选端口为 0");
        return false;
    }
    if (candidate.component == 0 || candidate.component > 256) {
        SetError(error, "候选 component 越界");
        return false;
    }
    const std::string transport = ToLower(candidate.transport);
    if (transport != "udp" && transport != "tcp") {
        SetError(error, "候选 transport 只支持 udp/tcp");
        return false;
    }
    if (!candidate.related_address.empty() &&
        (!IsValidAddress(candidate.related_address) || candidate.related_port == 0)) {
        SetError(error, "候选 raddr/rport 非法");
        return false;
    }
    return true;
}

void SortCandidatesByPriority(std::vector<IceCandidate>* candidates) {
    if (candidates == nullptr) {
        return;
    }
    std::stable_sort(candidates->begin(), candidates->end(),
                     [](const IceCandidate& left, const IceCandidate& right) {
                         return left.priority > right.priority;
                     });
}

#if defined(GO2CPP_MEDIA_HAVE_P2P)
namespace {

p2p::CandidateType ToP2PType(IceCandidateType type) noexcept {
    switch (type) {
        case IceCandidateType::kHost:
            return p2p::CandidateType::kHost;
        case IceCandidateType::kSrflx:
            return p2p::CandidateType::kSrflx;
        case IceCandidateType::kPrflx:
            return p2p::CandidateType::kPrflx;
        case IceCandidateType::kRelay:
            return p2p::CandidateType::kRelay;
    }
    return p2p::CandidateType::kHost;
}

IceCandidateType FromP2PType(p2p::CandidateType type) noexcept {
    switch (type) {
        case p2p::CandidateType::kHost:
            return IceCandidateType::kHost;
        case p2p::CandidateType::kSrflx:
            return IceCandidateType::kSrflx;
        case p2p::CandidateType::kPrflx:
            return IceCandidateType::kPrflx;
        case p2p::CandidateType::kRelay:
            return IceCandidateType::kRelay;
    }
    return IceCandidateType::kHost;
}

p2p::Endpoint MakeEndpoint(std::string_view address, std::uint16_t port) {
    std::ostringstream text;
    if (address.find(':') != std::string_view::npos) {
        text << '[' << address << "]:" << port;
    } else {
        text << address << ':' << port;
    }
    const auto parsed = p2p::Endpoint::Parse(text.str());
    return parsed.has_value() ? *parsed : p2p::Endpoint{};
}

}  // namespace

p2p::Candidate ToP2PCandidate(const IceCandidate& candidate) {
    p2p::Candidate converted;
    converted.type = ToP2PType(candidate.type);
    converted.endpoint = MakeEndpoint(candidate.address, candidate.port);
    if (!candidate.related_address.empty() && candidate.related_port != 0) {
        converted.base = MakeEndpoint(candidate.related_address, candidate.related_port);
    }
    converted.foundation = candidate.foundation;
    converted.priority = candidate.priority != 0 ? candidate.priority
                                                 : p2p::ComputeCandidatePriority(converted.type);
    converted.component =
        static_cast<std::uint8_t>(std::min<std::uint32_t>(candidate.component, 255));
    return converted;
}

std::optional<IceCandidate> FromP2PCandidate(const p2p::Candidate& candidate) {
    if (!candidate.endpoint.valid()) {
        return std::nullopt;
    }
    IceCandidate converted;
    converted.type = FromP2PType(candidate.type);
    converted.foundation = candidate.foundation;
    converted.component = candidate.component;
    converted.priority = candidate.priority;
    converted.address = candidate.endpoint.AddressString();
    converted.port = candidate.endpoint.port();
    if (candidate.base.valid()) {
        converted.related_address = candidate.base.AddressString();
        converted.related_port = candidate.base.port();
    }
    return converted;
}
#endif

// ---------------------------------------------------------------------------
// SDP 序列化
// ---------------------------------------------------------------------------

std::string MediaDescription::ToSdp() const {
    std::ostringstream out;
    out << "m=" << MediaKindName(kind) << ' ' << port << ' ' << protocol;
    if (kind == MediaKind::kApplication) {
        out << " webrtc-datachannel";
    } else {
        for (const RtpCodec& codec : codecs) {
            out << ' ' << static_cast<int>(codec.payload_type);
        }
    }
    out << '\n';
    if (!connection_address.empty()) {
        out << ConnectionLine(connection_address);
    }
    out << "a=mid:" << mid << '\n';
    for (const RtpCodec& codec : codecs) {
        out << RtpmapLine(codec);
        if (!codec.fmtp.empty()) {
            out << "a=fmtp:" << static_cast<int>(codec.payload_type) << ' ' << codec.fmtp << '\n';
        }
    }
    if (kind == MediaKind::kApplication) {
        out << "a=sctp-port:5000\n";
    }
    out << "a=" << SdpDirectionName(direction) << '\n';
    out << "a=ice-ufrag:" << ice_ufrag << '\n';
    out << "a=ice-pwd:" << ice_pwd << '\n';
    out << "a=ice-options:trickle\n";
    if (!fingerprint.empty()) {
        out << "a=fingerprint:" << fingerprint << '\n';
    }
    out << "a=setup:" << DtlsSetupName(setup) << '\n';
    if (!msid.empty()) {
        out << "a=msid:" << msid << '\n';
    }
    for (const IceCandidate& candidate : candidates) {
        out << candidate.ToAttribute() << '\n';
    }
    if (end_of_candidates) {
        out << "a=end-of-candidates\n";
    }
    return out.str();
}

bool MediaDescription::Validate(std::string* error) const {
    if (mid.empty()) {
        SetError(error, "媒体段 mid 为空");
        return false;
    }
    if (port == 0) {
        SetError(error, "媒体段端口为 0");
        return false;
    }
    if (protocol.empty()) {
        SetError(error, "媒体段协议为空");
        return false;
    }
    if (!connection_address.empty() && !IsValidAddress(connection_address)) {
        SetError(error, "媒体段 c= 地址非法");
        return false;
    }
    if (kind != MediaKind::kApplication && codecs.empty()) {
        SetError(error, "音视频媒体段没有编解码");
        return false;
    }
    for (const RtpCodec& codec : codecs) {
        if (codec.payload_type > 127) {
            SetError(error, "payload type 超过 127");
            return false;
        }
        if (codec.encoding_name.empty() || codec.clock_rate == 0) {
            SetError(error, "rtpmap 字段非法");
            return false;
        }
    }
    if (ice_ufrag.size() < 4 || ice_ufrag.size() > 256) {
        SetError(error, "ICE ufrag 长度非法（4..256）");
        return false;
    }
    if (ice_pwd.size() < 22 || ice_pwd.size() > 256) {
        SetError(error, "ICE pwd 长度非法（22..256）");
        return false;
    }
    for (const IceCandidate& candidate : candidates) {
        if (!IsValidIceCandidate(candidate, error)) {
            return false;
        }
    }
    return true;
}

std::string SessionDescription::ToSdp() const {
    std::ostringstream out;
    out << "v=0\n";
    out << "o=- " << session_id << ' ' << session_version << " IN "
        << (origin_address.find(':') == std::string::npos ? "IP4" : "IP6") << ' ' << origin_address
        << '\n';
    out << "s=" << (session_name.empty() ? "go2cpp-media" : session_name) << '\n';
    out << "t=0 0\n";
    if (!bundle_mids.empty()) {
        out << "a=group:BUNDLE";
        for (const std::string& mid : bundle_mids) {
            out << ' ' << mid;
        }
        out << '\n';
    }
    for (const MediaDescription& media : this->media) {
        out << media.ToSdp();
    }
    return out.str();
}

bool SessionDescription::Validate(std::string* error) const {
    if (session_id.empty()) {
        SetError(error, "会话 id 为空");
        return false;
    }
    if (media.empty()) {
        SetError(error, "会话没有媒体段");
        return false;
    }
    std::vector<std::string_view> mids;
    mids.reserve(media.size());
    for (const MediaDescription& description : media) {
        if (!description.Validate(error)) {
            return false;
        }
        if (std::find(mids.begin(), mids.end(), description.mid) != mids.end()) {
            SetError(error, "媒体段 mid 重复");
            return false;
        }
        mids.push_back(description.mid);
    }
    for (const std::string& mid : bundle_mids) {
        if (std::find(mids.begin(), mids.end(), mid) == mids.end()) {
            SetError(error, "BUNDLE 引用了不存在的 mid");
            return false;
        }
    }
    return true;
}

const MediaDescription* SessionDescription::FindMedia(std::string_view mid) const {
    for (const MediaDescription& description : media) {
        if (description.mid == mid) {
            return &description;
        }
    }
    return nullptr;
}

MediaDescription* SessionDescription::FindMedia(std::string_view mid) {
    for (MediaDescription& description : media) {
        if (description.mid == mid) {
            return &description;
        }
    }
    return nullptr;
}

std::vector<IceCandidate> SessionDescription::AllCandidates() const {
    std::vector<IceCandidate> candidates;
    for (const MediaDescription& description : media) {
        candidates.insert(candidates.end(), description.candidates.begin(),
                          description.candidates.end());
    }
    return candidates;
}

// ---------------------------------------------------------------------------
// SDP 解析
// ---------------------------------------------------------------------------

std::optional<SessionDescription> SessionDescription::ParseSdp(std::string_view text,
                                                               std::string* error) {
    SessionDescription description;
    MediaDescription current;
    current.mid.clear();
    bool media_open = false;
    bool saw_version = false;
    std::vector<std::uint8_t> pending_payloads;

    for (const std::string_view raw_line : SplitLines(text)) {
        const std::string_view line = TrimView(raw_line);
        if (line.empty() || line.front() == '#') {
            continue;
        }
        if (StartsWith(line, "v=")) {
            if (line != "v=0") {
                SetError(error, "只支持 SDP v=0");
                return std::nullopt;
            }
            saw_version = true;
            continue;
        }
        if (StartsWith(line, "o=")) {
            const std::vector<std::string_view> tokens = SplitWhitespace(line.substr(2));
            if (tokens.size() < 6) {
                SetError(error, "o= 行字段不足");
                return std::nullopt;
            }
            description.session_id = tokens[1];
            if (!ParseU64(tokens[2], &description.session_version)) {
                SetError(error, "o= 行版本号非法");
                return std::nullopt;
            }
            description.origin_address = tokens[5];
            continue;
        }
        if (StartsWith(line, "s=")) {
            description.session_name = std::string(line.substr(2));
            continue;
        }
        if (StartsWith(line, "c=")) {
            const std::vector<std::string_view> tokens = SplitWhitespace(line.substr(2));
            if (tokens.size() < 3) {
                SetError(error, "c= 行字段不足");
                return std::nullopt;
            }
            if (media_open) {
                current.connection_address = tokens.back();
            }
            continue;
        }
        if (StartsWith(line, "m=")) {
            if (media_open) {
                for (const std::uint8_t payload_type : pending_payloads) {
                    const bool exists = std::any_of(current.codecs.begin(), current.codecs.end(),
                                                    [payload_type](const RtpCodec& codec) {
                                                        return codec.payload_type == payload_type;
                                                    });
                    if (!exists) {
                        current.codecs.push_back(RtpCodec{payload_type, "unknown", 90000, 1, ""});
                    }
                }
                description.media.push_back(std::move(current));
            }
            const std::vector<std::string_view> tokens = SplitWhitespace(line.substr(2));
            if (tokens.size() < 3) {
                SetError(error, "m= 行字段不足");
                return std::nullopt;
            }
            const auto kind = ParseMediaKind(tokens[0]);
            if (!kind.has_value()) {
                SetError(error, "m= 媒体类型未知");
                return std::nullopt;
            }
            current = MediaDescription{};
            current.mid.clear();
            current.kind = *kind;
            if (!ParseU16(tokens[1], &current.port)) {
                SetError(error, "m= 端口非法");
                return std::nullopt;
            }
            current.protocol = tokens[2];
            pending_payloads.clear();
            if (current.kind != MediaKind::kApplication) {
                for (std::size_t index = 3; index < tokens.size(); ++index) {
                    std::uint8_t payload_type = 0;
                    if (!ParseU8(tokens[index], &payload_type) || payload_type > 127) {
                        SetError(error, "m= payload type 非法");
                        return std::nullopt;
                    }
                    pending_payloads.push_back(payload_type);
                }
            }
            media_open = true;
            continue;
        }
        if (!StartsWith(line, "a=")) {
            continue;  // b=/i=/k= 等未知会话行：忽略
        }
        const std::string_view attribute = line.substr(2);
        if (StartsWith(attribute, "group:BUNDLE")) {
            std::string_view rest = attribute.substr(12);
            for (const std::string_view mid : SplitWhitespace(rest)) {
                description.bundle_mids.emplace_back(mid);
            }
            continue;
        }
        if (!media_open) {
            continue;  // 媒体段之外的其它属性（ice-options 等）忽略
        }
        if (StartsWith(attribute, "mid:")) {
            current.mid = std::string(attribute.substr(4));
            continue;
        }
        if (attribute == "sendrecv" || attribute == "sendonly" || attribute == "recvonly" ||
            attribute == "inactive") {
            current.direction = *ParseSdpDirection(attribute);
            continue;
        }
        if (StartsWith(attribute, "ice-ufrag:")) {
            current.ice_ufrag = std::string(attribute.substr(10));
            continue;
        }
        if (StartsWith(attribute, "ice-pwd:")) {
            current.ice_pwd = std::string(attribute.substr(8));
            continue;
        }
        if (StartsWith(attribute, "fingerprint:")) {
            current.fingerprint = std::string(attribute.substr(12));
            continue;
        }
        if (StartsWith(attribute, "setup:")) {
            const auto setup = ParseDtlsSetup(attribute.substr(6));
            if (!setup.has_value()) {
                SetError(error, "a=setup 值未知");
                return std::nullopt;
            }
            current.setup = *setup;
            continue;
        }
        if (StartsWith(attribute, "msid:")) {
            current.msid = std::string(attribute.substr(5));
            continue;
        }
        if (StartsWith(attribute, "rtpmap:")) {
            const std::string_view rest = attribute.substr(7);
            const std::size_t space = rest.find(' ');
            if (space == std::string_view::npos) {
                SetError(error, "a=rtpmap 缺少编码名");
                return std::nullopt;
            }
            RtpCodec codec;
            if (!ParseU8(rest.substr(0, space), &codec.payload_type) || codec.payload_type > 127) {
                SetError(error, "a=rtpmap payload type 非法");
                return std::nullopt;
            }
            const std::vector<std::string_view> parts = SplitChar(rest.substr(space + 1), '/');
            if (parts.size() < 2 || parts.size() > 3 || parts[0].empty()) {
                SetError(error, "a=rtpmap 编码格式非法");
                return std::nullopt;
            }
            codec.encoding_name = std::string(parts[0]);
            if (!ParseU32(parts[1], &codec.clock_rate) || codec.clock_rate == 0) {
                SetError(error, "a=rtpmap 时钟频率非法");
                return std::nullopt;
            }
            if (parts.size() == 3 && !ParseU8(parts[2], &codec.channels)) {
                SetError(error, "a=rtpmap 通道数非法");
                return std::nullopt;
            }
            const bool replaced = std::any_of(current.codecs.begin(), current.codecs.end(),
                                              [&codec](const RtpCodec& existing) {
                                                  if (existing.payload_type != codec.payload_type) {
                                                      return false;
                                                  }
                                                  return true;
                                              });
            if (replaced) {
                for (RtpCodec& existing : current.codecs) {
                    if (existing.payload_type == codec.payload_type) {
                        existing = codec;
                    }
                }
            } else {
                current.codecs.push_back(codec);
            }
            continue;
        }
        if (StartsWith(attribute, "fmtp:")) {
            const std::string_view rest = attribute.substr(5);
            const std::size_t space = rest.find(' ');
            if (space == std::string_view::npos) {
                SetError(error, "a=fmtp 缺少参数");
                return std::nullopt;
            }
            std::uint8_t payload_type = 0;
            if (!ParseU8(rest.substr(0, space), &payload_type) || payload_type > 127) {
                SetError(error, "a=fmtp payload type 非法");
                return std::nullopt;
            }
            for (RtpCodec& codec : current.codecs) {
                if (codec.payload_type == payload_type) {
                    codec.fmtp = std::string(rest.substr(space + 1));
                    break;
                }
            }
            continue;
        }
        if (StartsWith(attribute, "candidate:")) {
            auto candidate = IceCandidate::Parse(attribute, error);
            if (!candidate.has_value()) {
                return std::nullopt;
            }
            current.candidates.push_back(std::move(*candidate));
            continue;
        }
        if (attribute == "end-of-candidates") {
            current.end_of_candidates = true;
            continue;
        }
        // a=rtcp-mux / a=extmap / a=ice-options 等：忽略。
    }

    if (media_open) {
        for (const std::uint8_t payload_type : pending_payloads) {
            const bool exists = std::any_of(current.codecs.begin(), current.codecs.end(),
                                            [payload_type](const RtpCodec& codec) {
                                                return codec.payload_type == payload_type;
                                            });
            if (!exists) {
                current.codecs.push_back(RtpCodec{payload_type, "unknown", 90000, 1, ""});
            }
        }
        description.media.push_back(std::move(current));
    }
    if (!saw_version) {
        SetError(error, "缺少 v=0 行");
        return std::nullopt;
    }
    if (!description.Validate(error)) {
        return std::nullopt;
    }
    return description;
}

// ---------------------------------------------------------------------------
// 信令消息
// ---------------------------------------------------------------------------

std::string SignalingMessage::ToText() const {
    std::string text(kSignalMagic);
    text += ' ';
    text += SignalingMessageTypeName(type);
    text += '\n';
    switch (type) {
        case SignalingMessageType::kOffer:
        case SignalingMessageType::kAnswer:
            text += sdp;
            break;
        case SignalingMessageType::kCandidate:
            text += candidate.ToString();
            break;
        case SignalingMessageType::kEndOfCandidates:
        case SignalingMessageType::kBye:
            break;
    }
    return text;
}

std::optional<SignalingMessage> SignalingMessage::Parse(std::string_view text, std::string* error) {
    const std::size_t newline = text.find('\n');
    const std::string_view header = TrimView(text.substr(0, newline));
    const std::string_view body =
        newline == std::string_view::npos ? std::string_view{} : text.substr(newline + 1);
    const std::vector<std::string_view> tokens = SplitWhitespace(header);
    if (tokens.size() != 2 || tokens[0] != kSignalMagic) {
        SetError(error, "信令消息头非法");
        return std::nullopt;
    }
    const auto type = ParseSignalingMessageType(tokens[1]);
    if (!type.has_value()) {
        SetError(error, "信令消息类型未知");
        return std::nullopt;
    }
    SignalingMessage message;
    message.type = *type;
    switch (message.type) {
        case SignalingMessageType::kOffer:
        case SignalingMessageType::kAnswer: {
            if (TrimView(body).empty()) {
                SetError(error, "offer/answer 消息没有 SDP 正文");
                return std::nullopt;
            }
            message.sdp = std::string(body);
            break;
        }
        case SignalingMessageType::kCandidate: {
            auto candidate = IceCandidate::Parse(body, error);
            if (!candidate.has_value()) {
                return std::nullopt;
            }
            message.candidate = std::move(*candidate);
            break;
        }
        case SignalingMessageType::kEndOfCandidates:
        case SignalingMessageType::kBye:
            break;
    }
    return message;
}

// ---------------------------------------------------------------------------
// SignalingSession
// ---------------------------------------------------------------------------

struct SignalingSession::Impl {
    explicit Impl(SignalingConfig cfg) : config(std::move(cfg)) {
        role = config.role;
        if (config.local_media.empty()) {
            config.local_media = DefaultMediaList();
        }
        if (config.session_id.empty()) {
            config.session_id = GenerateSessionId();
        }
        if (config.ice_ufrag.empty()) {
            config.ice_ufrag = GenerateUfrag();
        }
        if (config.ice_pwd.empty()) {
            config.ice_pwd = GeneratePwd();
        }
        if (config.fingerprint.empty()) {
            config.fingerprint = GenerateFingerprint();
        }
        local_candidates = config.local_candidates;
        // 初始候选也计入「已添加」，让统计口径等于「本端候选总数」。
        stats.local_candidates_added = local_candidates.size();
    }

    std::uint64_t Now() const {
        return config.clock ? config.clock() : SteadyNowMs();
    }

    void FillSession(SessionDescription* description, SdpType type) const {
        description->type = type;
        description->session_id = config.session_id;
        description->session_version = 1;
        description->origin_address = config.origin_address;
        description->session_name = config.session_name;
    }

    void ApplyCredentials(MediaDescription* media, DtlsSetup setup) const {
        media->ice_ufrag = config.ice_ufrag;
        media->ice_pwd = config.ice_pwd;
        media->fingerprint = config.fingerprint;
        media->setup = setup;
        media->candidates = local_candidates;
        media->end_of_candidates = false;
    }

    const MediaDescription* FindLocalTemplate(std::string_view mid) const {
        for (const MediaDescription& media : config.local_media) {
            if (media.mid == mid) {
                return &media;
            }
        }
        return nullptr;
    }

    SessionDescription BuildOffer() const {
        SessionDescription description;
        FillSession(&description, SdpType::kOffer);
        for (const MediaDescription& template_media : config.local_media) {
            MediaDescription media = template_media;
            ApplyCredentials(&media, config.offer_setup);
            description.bundle_mids.push_back(media.mid);
            description.media.push_back(std::move(media));
        }
        return description;
    }

    SessionDescription BuildAnswer() const {
        SessionDescription description;
        FillSession(&description, SdpType::kAnswer);
        for (const MediaDescription& remote_media : remote->media) {
            MediaDescription media;
            media.kind = remote_media.kind;
            media.mid = remote_media.mid;
            media.port = remote_media.port;
            media.protocol = remote_media.protocol;
            media.connection_address = "0.0.0.0";
            media.direction = ReverseDirection(remote_media.direction);
            media.msid = remote_media.msid;
            const MediaDescription* local_template = FindLocalTemplate(remote_media.mid);
            if (remote_media.kind != MediaKind::kApplication) {
                if (local_template != nullptr) {
                    for (const RtpCodec& remote_codec : remote_media.codecs) {
                        for (const RtpCodec& local_codec : local_template->codecs) {
                            if (local_codec.payload_type == remote_codec.payload_type &&
                                local_codec.encoding_name == remote_codec.encoding_name) {
                                RtpCodec chosen = local_codec;
                                if (!remote_codec.fmtp.empty()) {
                                    chosen.fmtp = remote_codec.fmtp;
                                }
                                media.codecs.push_back(std::move(chosen));
                                break;
                            }
                        }
                    }
                }
                if (media.codecs.empty()) {
                    media.codecs = remote_media.codecs;
                }
            }
            ApplyCredentials(&media, NegotiateAnswerSetup(remote_media.setup, config.answer_setup));
            description.bundle_mids.push_back(media.mid);
            description.media.push_back(std::move(media));
        }
        return description;
    }

    void MergeRemoteCandidates(const std::vector<IceCandidate>& candidates) {
        std::vector<IceCandidate> merged;
        for (const IceCandidate& candidate : candidates) {
            // 同一份 SDP 每条 m= 行都重复列出同一组候选（BUNDLE 语义），
            // 先按「本次输入」去重，只有跨消息重复才计入 duplicate。
            if (std::find(merged.begin(), merged.end(), candidate) != merged.end()) {
                continue;
            }
            merged.push_back(candidate);
            if (std::find(remote_candidates.begin(), remote_candidates.end(), candidate) !=
                remote_candidates.end()) {
                ++stats.remote_candidates_duplicate;
                continue;
            }
            remote_candidates.push_back(candidate);
            ++stats.remote_candidates_added;
        }
    }

    std::uint64_t started_ms = 0;
    SignalingConfig config;
    SignalingRole role = SignalingRole::kOfferer;
    SignalingState state = SignalingState::kIdle;
    SignalingStats stats;
    std::optional<SessionDescription> local;
    std::optional<SessionDescription> remote;
    std::vector<IceCandidate> local_candidates;
    std::vector<IceCandidate> remote_candidates;
    bool remote_end_of_candidates = false;
    std::optional<IceCandidate> selected_local;
    std::optional<IceCandidate> selected_remote;
};

SignalingSession::SignalingSession(SignalingConfig config)
    : m_impl(std::make_unique<Impl>(std::move(config))) {}

SignalingSession::~SignalingSession() = default;

std::optional<SessionDescription> SignalingSession::CreateOffer(std::string* error) {
    if (m_impl->role != SignalingRole::kOfferer) {
        SetError(error, "answerer 不能创建 offer");
        return std::nullopt;
    }
    if (m_impl->state != SignalingState::kIdle) {
        SetError(error, "当前状态不允许创建 offer");
        return std::nullopt;
    }
    SessionDescription description = m_impl->BuildOffer();
    if (!description.Validate(error)) {
        return std::nullopt;
    }
    m_impl->local = description;
    m_impl->state = SignalingState::kOffering;
    m_impl->started_ms = m_impl->Now();
    ++m_impl->stats.offers_created;
    return description;
}

SignalingStatus SignalingSession::SetRemoteOffer(const SessionDescription& offer,
                                                 std::string* error) {
    if (m_impl->role != SignalingRole::kAnswerer) {
        SetError(error, "offerer 不能接收 offer");
        return SignalingStatus::kInvalidRole;
    }
    if (m_impl->state != SignalingState::kIdle) {
        SetError(error, "当前状态不允许接收 offer");
        return SignalingStatus::kInvalidState;
    }
    if (!offer.Validate(error)) {
        return SignalingStatus::kInvalidDescription;
    }
    m_impl->remote = offer;
    m_impl->state = SignalingState::kOffering;
    m_impl->started_ms = m_impl->Now();
    m_impl->MergeRemoteCandidates(offer.AllCandidates());
    ++m_impl->stats.offers_received;
    return SignalingStatus::kOk;
}

std::optional<SessionDescription> SignalingSession::CreateAnswer(std::string* error) {
    if (m_impl->role != SignalingRole::kAnswerer) {
        SetError(error, "offerer 不能创建 answer");
        return std::nullopt;
    }
    if (m_impl->state != SignalingState::kOffering) {
        SetError(error, "需要先接收 offer 才能创建 answer");
        return std::nullopt;
    }
    if (!m_impl->remote.has_value()) {
        SetError(error, "没有对端 offer");
        return std::nullopt;
    }
    SessionDescription description = m_impl->BuildAnswer();
    if (!description.Validate(error)) {
        return std::nullopt;
    }
    m_impl->local = description;
    m_impl->state = SignalingState::kAnswering;
    ++m_impl->stats.answers_created;
    return description;
}

SignalingStatus SignalingSession::SetRemoteAnswer(const SessionDescription& answer,
                                                  std::string* error) {
    if (m_impl->role != SignalingRole::kOfferer) {
        SetError(error, "answerer 不能接收 answer");
        return SignalingStatus::kInvalidRole;
    }
    if (m_impl->state != SignalingState::kOffering) {
        SetError(error, "当前状态不允许接收 answer");
        return SignalingStatus::kInvalidState;
    }
    if (!m_impl->local.has_value()) {
        SetError(error, "没有本端 offer");
        return SignalingStatus::kNoLocalDescription;
    }
    if (!answer.Validate(error)) {
        return SignalingStatus::kInvalidDescription;
    }
    if (answer.media.size() != m_impl->local->media.size()) {
        SetError(error, "answer 的媒体段数量与 offer 不一致");
        return SignalingStatus::kMediaMismatch;
    }
    for (std::size_t index = 0; index < answer.media.size(); ++index) {
        const MediaDescription& offer_media = m_impl->local->media[index];
        const MediaDescription& answer_media = answer.media[index];
        if (offer_media.mid != answer_media.mid) {
            SetError(error, "answer 的 mid 与 offer 不一致");
            return SignalingStatus::kMediaMismatch;
        }
        if (!IsDirectionCompatible(offer_media.direction, answer_media.direction)) {
            SetError(error, "answer 的媒体方向与 offer 不兼容");
            return SignalingStatus::kMediaMismatch;
        }
        if (!IsSetupCompatible(offer_media.setup, answer_media.setup)) {
            SetError(error, "answer 的 DTLS 角色与 offer 不兼容");
            return SignalingStatus::kMediaMismatch;
        }
    }
    m_impl->remote = answer;
    m_impl->state = SignalingState::kAnswering;
    m_impl->MergeRemoteCandidates(answer.AllCandidates());
    ++m_impl->stats.answers_received;
    return SignalingStatus::kOk;
}

SignalingStatus SignalingSession::AddLocalCandidate(const IceCandidate& candidate,
                                                    std::string* error) {
    if (m_impl->state == SignalingState::kClosed) {
        return SignalingStatus::kClosed;
    }
    if (m_impl->state == SignalingState::kFailed) {
        SetError(error, "会话已失败，不能再添加候选");
        return SignalingStatus::kInvalidState;
    }
    if (!IsValidIceCandidate(candidate, error)) {
        return SignalingStatus::kCandidateRejected;
    }
    if (std::find(m_impl->local_candidates.begin(), m_impl->local_candidates.end(), candidate) !=
        m_impl->local_candidates.end()) {
        return SignalingStatus::kOk;
    }
    m_impl->local_candidates.push_back(candidate);
    if (m_impl->local.has_value()) {
        AddCandidateToDescription(&*m_impl->local, candidate);
    }
    ++m_impl->stats.local_candidates_added;
    return SignalingStatus::kOk;
}

SignalingStatus SignalingSession::AddRemoteCandidate(const IceCandidate& candidate,
                                                     std::string* error) {
    if (m_impl->state == SignalingState::kIdle) {
        SetError(error, "还没有对端描述，不能添加远端候选");
        return SignalingStatus::kNoRemoteDescription;
    }
    if (m_impl->state == SignalingState::kClosed) {
        return SignalingStatus::kClosed;
    }
    if (m_impl->state == SignalingState::kFailed) {
        SetError(error, "会话已失败，不能再添加候选");
        return SignalingStatus::kInvalidState;
    }
    if (!IsValidIceCandidate(candidate, error)) {
        ++m_impl->stats.remote_candidates_rejected;
        return SignalingStatus::kCandidateRejected;
    }
    if (std::find(m_impl->remote_candidates.begin(), m_impl->remote_candidates.end(), candidate) !=
        m_impl->remote_candidates.end()) {
        ++m_impl->stats.remote_candidates_duplicate;
        return SignalingStatus::kOk;
    }
    m_impl->remote_candidates.push_back(candidate);
    if (m_impl->remote.has_value()) {
        AddCandidateToDescription(&*m_impl->remote, candidate);
    }
    ++m_impl->stats.remote_candidates_added;
    return SignalingStatus::kOk;
}

void SignalingSession::EndOfRemoteCandidates() {
    m_impl->remote_end_of_candidates = true;
    if (m_impl->remote.has_value()) {
        for (MediaDescription& media : m_impl->remote->media) {
            media.end_of_candidates = true;
        }
    }
}

SignalingStatus SignalingSession::OnIceConnected(const IceCandidate& local,
                                                 const IceCandidate& remote, std::string* error) {
    if (m_impl->state == SignalingState::kConnected) {
        return SignalingStatus::kAlreadyConnected;
    }
    if (m_impl->state != SignalingState::kAnswering) {
        SetError(error, "需要完成 offer/answer 后才能标记 connected");
        return SignalingStatus::kInvalidState;
    }
    if (!m_impl->local.has_value()) {
        return SignalingStatus::kNoLocalDescription;
    }
    if (!m_impl->remote.has_value()) {
        return SignalingStatus::kNoRemoteDescription;
    }
    if (!IsValidIceCandidate(local, error) || !IsValidIceCandidate(remote, error)) {
        return SignalingStatus::kCandidateRejected;
    }
    if (std::find(m_impl->local_candidates.begin(), m_impl->local_candidates.end(), local) ==
        m_impl->local_candidates.end()) {
        m_impl->local_candidates.push_back(local);
        AddCandidateToDescription(&*m_impl->local, local);
        ++m_impl->stats.local_candidates_added;
    }
    if (std::find(m_impl->remote_candidates.begin(), m_impl->remote_candidates.end(), remote) ==
        m_impl->remote_candidates.end()) {
        // ICE 检查发现的对端反射地址（prflx）在 SDP 里没有，需要补录。
        m_impl->remote_candidates.push_back(remote);
        AddCandidateToDescription(&*m_impl->remote, remote);
        ++m_impl->stats.remote_candidates_added;
    }
    m_impl->selected_local = local;
    m_impl->selected_remote = remote;
    m_impl->state = SignalingState::kConnected;
    ++m_impl->stats.ice_connected;
    return SignalingStatus::kOk;
}

SignalingStatus SignalingSession::OnIceFailed() {
    if (m_impl->state != SignalingState::kOffering && m_impl->state != SignalingState::kAnswering) {
        return SignalingStatus::kInvalidState;
    }
    m_impl->state = SignalingState::kFailed;
    ++m_impl->stats.ice_failed;
    return SignalingStatus::kOk;
}

SignalingStatus SignalingSession::Tick() {
    if (m_impl->state != SignalingState::kOffering && m_impl->state != SignalingState::kAnswering) {
        return SignalingStatus::kOk;
    }
    if (m_impl->started_ms == 0) {
        return SignalingStatus::kOk;
    }
    const std::uint64_t now = m_impl->Now();
    if (now >= m_impl->started_ms &&
        now - m_impl->started_ms >= m_impl->config.negotiation_timeout_ms) {
        m_impl->state = SignalingState::kFailed;
        ++m_impl->stats.ice_failed;
        return SignalingStatus::kTimeout;
    }
    return SignalingStatus::kOk;
}

void SignalingSession::Close() {
    m_impl->state = SignalingState::kClosed;
}

SignalingRole SignalingSession::role() const noexcept {
    return m_impl->role;
}

SignalingState SignalingSession::state() const noexcept {
    return m_impl->state;
}

bool SignalingSession::connected() const noexcept {
    return m_impl->state == SignalingState::kConnected;
}

const SignalingConfig& SignalingSession::config() const noexcept {
    return m_impl->config;
}

const SignalingStats& SignalingSession::stats() const noexcept {
    return m_impl->stats;
}

const SessionDescription* SignalingSession::local_description() const noexcept {
    return m_impl->local.has_value() ? &*m_impl->local : nullptr;
}

const SessionDescription* SignalingSession::remote_description() const noexcept {
    return m_impl->remote.has_value() ? &*m_impl->remote : nullptr;
}

std::vector<IceCandidate> SignalingSession::local_candidates() const {
    return m_impl->local_candidates;
}

std::vector<IceCandidate> SignalingSession::remote_candidates() const {
    return m_impl->remote_candidates;
}

bool SignalingSession::remote_end_of_candidates() const noexcept {
    return m_impl->remote_end_of_candidates;
}

std::optional<IceCandidate> SignalingSession::BestRemoteCandidate() const {
    if (m_impl->remote_candidates.empty()) {
        return std::nullopt;
    }
    return *std::max_element(m_impl->remote_candidates.begin(), m_impl->remote_candidates.end(),
                             [](const IceCandidate& left, const IceCandidate& right) {
                                 return left.priority < right.priority;
                             });
}

const std::optional<IceCandidate>& SignalingSession::selected_local_candidate() const noexcept {
    return m_impl->selected_local;
}

const std::optional<IceCandidate>& SignalingSession::selected_remote_candidate() const noexcept {
    return m_impl->selected_remote;
}

DtlsSetup SignalingSession::local_setup() const noexcept {
    if (m_impl->local.has_value() && !m_impl->local->media.empty()) {
        return m_impl->local->media.front().setup;
    }
    return DtlsSetup::kActpass;
}

DtlsSetup SignalingSession::remote_setup() const noexcept {
    if (m_impl->remote.has_value() && !m_impl->remote->media.empty()) {
        return m_impl->remote->media.front().setup;
    }
    return DtlsSetup::kActpass;
}

}  // namespace go2cpp::media
