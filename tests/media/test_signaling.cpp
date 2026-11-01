/**
 * @file test_signaling.cpp
 * @brief 信令测试：offer/answer 往返、候选交换、状态机、SDP 往返、P2P 集成，
 *        以及 6 个真实协商场景（1v1、SFU 三方、trickle、relay 降级、超时、answer 校验）。
 * @details 断言使用 `media_test_util.hpp` 的轻量宏；SDP 一律经过
 *          `ToSdp() → ParseSdp()` 文本往返，模拟真实信令通道。
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "go2cpp/media/signaling.hpp"
#include "media_test_util.hpp"

#if defined(GO2CPP_MEDIA_HAVE_P2P)
#include "go2cpp/p2p/candidate.hpp"
#include "go2cpp/p2p/signaling.hpp"
#endif

namespace {

using go2cpp::media::ComputeIceCandidatePriority;
using go2cpp::media::DtlsSetup;
using go2cpp::media::IceCandidate;
using go2cpp::media::IceCandidateType;
using go2cpp::media::MediaKind;
using go2cpp::media::SdpDirection;
using go2cpp::media::SessionDescription;
using go2cpp::media::SignalingConfig;
using go2cpp::media::SignalingMessage;
using go2cpp::media::SignalingMessageType;
using go2cpp::media::SignalingRole;
using go2cpp::media::SignalingSession;
using go2cpp::media::SignalingState;
using go2cpp::media::SignalingStatus;

constexpr const char* kFingerprint =
    "sha-256 AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99:"
    "AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99";

/// @brief 可注入时钟，驱动协商超时测试。
struct FakeClock {
    std::uint64_t now = 1000;
};

IceCandidate MakeCandidate(IceCandidateType type, std::string foundation, std::string address,
                           std::uint16_t port, std::string related_address = {},
                           std::uint16_t related_port = 0) {
    IceCandidate candidate;
    candidate.type = type;
    candidate.foundation = std::move(foundation);
    candidate.address = std::move(address);
    candidate.port = port;
    candidate.related_address = std::move(related_address);
    candidate.related_port = related_port;
    candidate.priority = ComputeIceCandidatePriority(type);
    return candidate;
}

std::vector<IceCandidate> StandardCandidates(std::string_view prefix, std::uint16_t base_port) {
    std::vector<IceCandidate> candidates;
    candidates.push_back(MakeCandidate(IceCandidateType::kHost, std::string(prefix) + "h",
                                       "192.168.1.10", base_port));
    candidates.push_back(MakeCandidate(IceCandidateType::kSrflx, std::string(prefix) + "s",
                                       "203.0.113.7", static_cast<std::uint16_t>(base_port + 1),
                                       "192.168.1.10", base_port));
    candidates.push_back(MakeCandidate(IceCandidateType::kRelay, std::string(prefix) + "r",
                                       "198.51.100.9", static_cast<std::uint16_t>(base_port + 2),
                                       "198.51.100.9", 3478));
    return candidates;
}

SignalingConfig MakeOffererConfig(FakeClock& clock, std::vector<IceCandidate> candidates = {}) {
    SignalingConfig config;
    config.role = SignalingRole::kOfferer;
    config.session_id = "offerer-session";
    config.ice_ufrag = "uF1x";
    config.ice_pwd = "0123456789abcdef01234567";
    config.fingerprint = kFingerprint;
    config.local_candidates = std::move(candidates);
    config.clock = [&clock]() { return clock.now; };
    return config;
}

SignalingConfig MakeAnswererConfig(FakeClock& clock, std::vector<IceCandidate> candidates = {}) {
    SignalingConfig config;
    config.role = SignalingRole::kAnswerer;
    config.session_id = "answerer-session";
    config.ice_ufrag = "aN2y";
    config.ice_pwd = "abcdef0123456789abcdef01";
    config.fingerprint = kFingerprint;
    config.local_candidates = std::move(candidates);
    config.clock = [&clock]() { return clock.now; };
    return config;
}

struct Pair {
    std::unique_ptr<SignalingSession> offerer;
    std::unique_ptr<SignalingSession> answerer;
};

Pair MakePair(FakeClock& clock, bool with_candidates = true) {
    Pair pair;
    pair.offerer = std::make_unique<SignalingSession>(MakeOffererConfig(
        clock, with_candidates ? StandardCandidates("o", 5000) : std::vector<IceCandidate>{}));
    pair.answerer = std::make_unique<SignalingSession>(MakeAnswererConfig(
        clock, with_candidates ? StandardCandidates("a", 6000) : std::vector<IceCandidate>{}));
    return pair;
}

/// @brief 走完整 SignalingMessage + SDP 文本往返的 offer/answer。
bool ExchangeOfferAnswer(Pair* pair, std::string* error) {
    auto offer = pair->offerer->CreateOffer(error);
    if (!offer.has_value()) {
        return false;
    }
    SignalingMessage offer_message;
    offer_message.type = SignalingMessageType::kOffer;
    offer_message.sdp = offer->ToSdp();
    auto received_offer = SignalingMessage::Parse(offer_message.ToText(), error);
    if (!received_offer.has_value() || received_offer->type != SignalingMessageType::kOffer) {
        return false;
    }
    auto parsed_offer = SessionDescription::ParseSdp(received_offer->sdp, error);
    if (!parsed_offer.has_value()) {
        return false;
    }
    parsed_offer->type = go2cpp::media::SdpType::kOffer;
    if (pair->answerer->SetRemoteOffer(*parsed_offer, error) != SignalingStatus::kOk) {
        return false;
    }
    auto answer = pair->answerer->CreateAnswer(error);
    if (!answer.has_value()) {
        return false;
    }
    SignalingMessage answer_message;
    answer_message.type = SignalingMessageType::kAnswer;
    answer_message.sdp = answer->ToSdp();
    auto received_answer = SignalingMessage::Parse(answer_message.ToText(), error);
    if (!received_answer.has_value() || received_answer->type != SignalingMessageType::kAnswer) {
        return false;
    }
    auto parsed_answer = SessionDescription::ParseSdp(received_answer->sdp, error);
    if (!parsed_answer.has_value()) {
        return false;
    }
    parsed_answer->type = go2cpp::media::SdpType::kAnswer;
    return pair->offerer->SetRemoteAnswer(*parsed_answer, error) == SignalingStatus::kOk;
}

std::optional<IceCandidate> FindCandidate(const std::vector<IceCandidate>& candidates,
                                          IceCandidateType type) {
    for (const IceCandidate& candidate : candidates) {
        if (candidate.type == type) {
            return candidate;
        }
    }
    return std::nullopt;
}

bool ConnectPair(Pair* pair, std::string* error) {
    const auto offerer_local =
        FindCandidate(pair->offerer->local_candidates(), IceCandidateType::kHost);
    const auto offerer_remote =
        FindCandidate(pair->offerer->remote_candidates(), IceCandidateType::kHost);
    const auto answerer_local =
        FindCandidate(pair->answerer->local_candidates(), IceCandidateType::kHost);
    const auto answerer_remote =
        FindCandidate(pair->answerer->remote_candidates(), IceCandidateType::kHost);
    if (!offerer_local || !offerer_remote || !answerer_local || !answerer_remote) {
        if (error != nullptr) {
            *error = "缺少 host 候选";
        }
        return false;
    }
    if (pair->offerer->OnIceConnected(*offerer_local, *offerer_remote, error) !=
        SignalingStatus::kOk) {
        return false;
    }
    return pair->answerer->OnIceConnected(*answerer_local, *answerer_remote, error) ==
           SignalingStatus::kOk;
}

// ---------------------------------------------------------------------------
// 1. offer/answer 往返（SDP + 信令消息 + 状态）
// ---------------------------------------------------------------------------
void TestOfferAnswerRoundTrip() {
    FakeClock clock;
    Pair pair = MakePair(clock);
    std::string error;
    MEDIA_CHECK(ExchangeOfferAnswer(&pair, &error));
    if (!error.empty()) {
        std::printf("[ERROR] %s\n", error.c_str());
    }
    MEDIA_CHECK_EQ(pair.offerer->state(), SignalingState::kAnswering);
    MEDIA_CHECK_EQ(pair.answerer->state(), SignalingState::kAnswering);
    MEDIA_CHECK(pair.offerer->local_description() != nullptr);
    MEDIA_CHECK(pair.offerer->remote_description() != nullptr);
    MEDIA_CHECK(pair.answerer->local_description() != nullptr);
    MEDIA_CHECK(pair.answerer->remote_description() != nullptr);

    const SessionDescription* local_offer = pair.offerer->local_description();
    MEDIA_CHECK_EQ(local_offer->media.size(), std::size_t{2});
    MEDIA_CHECK_EQ(local_offer->FindMedia("0")->kind, MediaKind::kVideo);
    MEDIA_CHECK_EQ(local_offer->FindMedia("1")->kind, MediaKind::kAudio);
    MEDIA_CHECK_EQ(local_offer->FindMedia("0")->codecs.front().payload_type, 96);
    MEDIA_CHECK_EQ(local_offer->FindMedia("1")->codecs.front().payload_type, 111);
    MEDIA_CHECK_EQ(local_offer->bundle_mids.size(), std::size_t{2});
    MEDIA_CHECK_EQ(local_offer->AllCandidates().size(), std::size_t{6});  // 3 候选 × 2 m-line

    MEDIA_CHECK_EQ(pair.offerer->local_setup(), DtlsSetup::kActpass);
    MEDIA_CHECK_EQ(pair.answerer->local_setup(), DtlsSetup::kActive);
    MEDIA_CHECK_EQ(pair.offerer->remote_setup(), DtlsSetup::kActive);
    MEDIA_CHECK_EQ(pair.answerer->remote_setup(), DtlsSetup::kActpass);

    MEDIA_CHECK_EQ(pair.offerer->remote_candidates().size(), std::size_t{3});
    MEDIA_CHECK_EQ(pair.answerer->remote_candidates().size(), std::size_t{3});
    MEDIA_CHECK_EQ(pair.offerer->stats().offers_created, std::uint64_t{1});
    MEDIA_CHECK_EQ(pair.answerer->stats().offers_received, std::uint64_t{1});
    MEDIA_CHECK_EQ(pair.answerer->stats().answers_created, std::uint64_t{1});
    MEDIA_CHECK_EQ(pair.offerer->stats().answers_received, std::uint64_t{1});

    // SDP 文本往返必须逐字节一致（序列化顺序固定）。
    const std::string offer_sdp = local_offer->ToSdp();
    auto reparsed = SessionDescription::ParseSdp(offer_sdp, &error);
    MEDIA_CHECK(reparsed.has_value());
    MEDIA_CHECK_EQ(reparsed->ToSdp(), offer_sdp);

    // SDP 解析是控制面路径，这里给出 10k 次往返解析的吞吐基线。
    constexpr int kParseIterations = 10000;
    const auto parse_begin = std::chrono::steady_clock::now();
    for (int iteration = 0; iteration < kParseIterations; ++iteration) {
        std::string parse_error;
        auto round = SessionDescription::ParseSdp(offer_sdp, &parse_error);
        if (!round.has_value()) {
            MEDIA_CHECK(false);
            break;
        }
    }
    const auto parse_end = std::chrono::steady_clock::now();
    const double parse_ns =
        std::chrono::duration<double, std::nano>(parse_end - parse_begin).count() /
        kParseIterations;
    MEDIA_CHECK_GE(parse_ns, 0.0);
    std::printf("[PERF] SDP parse: %.0f ns/次 (%d 次)\n", parse_ns, kParseIterations);

    // 信令消息的 offer/candidate/bye 都能往返。
    SignalingMessage message;
    message.type = SignalingMessageType::kOffer;
    message.sdp = offer_sdp;
    auto decoded = SignalingMessage::Parse(message.ToText(), &error);
    MEDIA_CHECK(decoded.has_value());
    MEDIA_CHECK_EQ(decoded->sdp, offer_sdp);

    SignalingMessage candidate_message;
    candidate_message.type = SignalingMessageType::kCandidate;
    candidate_message.candidate = StandardCandidates("x", 7000).front();
    auto decoded_candidate = SignalingMessage::Parse(candidate_message.ToText(), &error);
    MEDIA_CHECK(decoded_candidate.has_value());
    MEDIA_CHECK(decoded_candidate->candidate == candidate_message.candidate);

    SignalingMessage bye_message;
    bye_message.type = SignalingMessageType::kBye;
    auto decoded_bye = SignalingMessage::Parse(bye_message.ToText(), &error);
    MEDIA_CHECK(decoded_bye.has_value());
    MEDIA_CHECK_EQ(decoded_bye->type, SignalingMessageType::kBye);
}

// ---------------------------------------------------------------------------
// 2. 候选交换 + connected 状态
// ---------------------------------------------------------------------------
void TestCandidateExchange() {
    FakeClock clock;
    Pair pair = MakePair(clock);
    std::string error;
    MEDIA_CHECK(ExchangeOfferAnswer(&pair, &error));

    // BestRemoteCandidate 按优先级取 host。
    auto best = pair.offerer->BestRemoteCandidate();
    MEDIA_CHECK(best.has_value());
    MEDIA_CHECK_EQ(best->type, IceCandidateType::kHost);
    MEDIA_CHECK_EQ(best->priority, 2130706431U);

    // 追加一个 relay 候选：去重 + 排序 + 计数。
    const IceCandidate extra = MakeCandidate(IceCandidateType::kRelay, "extra-r", "198.51.100.20",
                                             49500, "198.51.100.20", 3478);
    MEDIA_CHECK_EQ(pair.answerer->AddLocalCandidate(extra, &error), SignalingStatus::kOk);
    MEDIA_CHECK_EQ(pair.answerer->AddLocalCandidate(extra, &error), SignalingStatus::kOk);
    MEDIA_CHECK_EQ(pair.answerer->stats().local_candidates_added, std::uint64_t{4});
    MEDIA_CHECK_EQ(pair.offerer->AddRemoteCandidate(extra, &error), SignalingStatus::kOk);
    MEDIA_CHECK_EQ(pair.offerer->AddRemoteCandidate(extra, &error), SignalingStatus::kOk);
    MEDIA_CHECK_EQ(pair.offerer->stats().remote_candidates_duplicate, std::uint64_t{1});
    MEDIA_CHECK_EQ(pair.offerer->remote_candidates().size(), std::size_t{4});

    // SDP 里的候选同样会进入 remote_candidates。
    auto answer_sdp = pair.answerer->local_description()->ToSdp();
    MEDIA_CHECK(answer_sdp.find("a=candidate:") != std::string::npos);
    MEDIA_CHECK(answer_sdp.find(" typ host") != std::string::npos);
    MEDIA_CHECK(answer_sdp.find(" typ srflx") != std::string::npos);
    MEDIA_CHECK(answer_sdp.find(" typ relay") != std::string::npos);

    pair.offerer->EndOfRemoteCandidates();
    pair.answerer->EndOfRemoteCandidates();
    MEDIA_CHECK(pair.offerer->remote_end_of_candidates());
    MEDIA_CHECK(pair.answerer->remote_end_of_candidates());

    // 非法候选被拒绝并计数。
    IceCandidate invalid = extra;
    invalid.port = 0;
    MEDIA_CHECK_EQ(pair.offerer->AddRemoteCandidate(invalid, &error),
                   SignalingStatus::kCandidateRejected);
    MEDIA_CHECK_EQ(pair.offerer->stats().remote_candidates_rejected, std::uint64_t{1});

    MEDIA_CHECK(ConnectPair(&pair, &error));
    MEDIA_CHECK_EQ(pair.offerer->state(), SignalingState::kConnected);
    MEDIA_CHECK_EQ(pair.answerer->state(), SignalingState::kConnected);
    MEDIA_CHECK(pair.offerer->selected_local_candidate().has_value());
    MEDIA_CHECK(pair.offerer->selected_remote_candidate().has_value());
    MEDIA_CHECK_EQ(pair.offerer->selected_local_candidate()->type, IceCandidateType::kHost);
    MEDIA_CHECK_EQ(pair.offerer->selected_remote_candidate()->type, IceCandidateType::kHost);
    MEDIA_CHECK_EQ(pair.offerer->stats().ice_connected, std::uint64_t{1});

    // 已 connected 再上报选对：幂等拒绝，不改变状态。
    MEDIA_CHECK_EQ(pair.offerer->OnIceConnected(*pair.offerer->selected_local_candidate(),
                                                *pair.offerer->selected_remote_candidate(), &error),
                   SignalingStatus::kAlreadyConnected);

    // 优先级排序：host > srflx > relay。
    std::vector<IceCandidate> sorted = {
        MakeCandidate(IceCandidateType::kRelay, "r", "198.51.100.9", 5002),
        MakeCandidate(IceCandidateType::kHost, "h", "192.168.1.10", 5000),
        MakeCandidate(IceCandidateType::kSrflx, "s", "203.0.113.7", 5001),
    };
    go2cpp::media::SortCandidatesByPriority(&sorted);
    MEDIA_CHECK_EQ(sorted[0].type, IceCandidateType::kHost);
    MEDIA_CHECK_EQ(sorted[1].type, IceCandidateType::kSrflx);
    MEDIA_CHECK_EQ(sorted[2].type, IceCandidateType::kRelay);
    MEDIA_CHECK_EQ(ComputeIceCandidatePriority(IceCandidateType::kSrflx), 1694498815U);
    MEDIA_CHECK_EQ(ComputeIceCandidatePriority(IceCandidateType::kRelay), 16777215U);
}

// ---------------------------------------------------------------------------
// 3. 状态机：非法迁移、超时、关闭
// ---------------------------------------------------------------------------
void TestStateMachine() {
    FakeClock clock;
    std::string error;
    auto offerer = std::make_unique<SignalingSession>(MakeOffererConfig(clock));
    auto answerer = std::make_unique<SignalingSession>(MakeAnswererConfig(clock));

    // 角色错误：answerer 不能创建 offer；offerer 不能接收 offer。
    MEDIA_CHECK(!answerer->CreateOffer(&error).has_value());
    auto offer = offerer->CreateOffer(&error);
    MEDIA_CHECK(offer.has_value());
    MEDIA_CHECK_EQ(offerer->state(), SignalingState::kOffering);
    MEDIA_CHECK_EQ(offerer->CreateOffer(&error).has_value(), false);
    MEDIA_CHECK_EQ(offerer->state(), SignalingState::kOffering);
    MEDIA_CHECK_EQ(answerer->SetRemoteOffer(*offer, &error), SignalingStatus::kOk);
    MEDIA_CHECK_EQ(answerer->state(), SignalingState::kOffering);
    MEDIA_CHECK_EQ(answerer->SetRemoteOffer(*offer, &error), SignalingStatus::kInvalidState);
    MEDIA_CHECK_EQ(offerer->SetRemoteOffer(*offer, &error), SignalingStatus::kInvalidRole);

    // answer 之前不能接收 answer / 标记 connected（把 offer 当 answer 传会被
    // 描述校验拦下，返回 media-mismatch 而不是进入 answering）。
    MEDIA_CHECK_EQ(offerer->SetRemoteAnswer(*offer, &error), SignalingStatus::kMediaMismatch);
    MEDIA_CHECK_EQ(offerer->OnIceConnected(StandardCandidates("x", 9000).front(),
                                           StandardCandidates("y", 9100).front(), &error),
                   SignalingStatus::kInvalidState);

    auto answer = answerer->CreateAnswer(&error);
    MEDIA_CHECK(answer.has_value());
    MEDIA_CHECK_EQ(answerer->state(), SignalingState::kAnswering);
    MEDIA_CHECK_EQ(answerer->CreateAnswer(&error).has_value(), false);
    MEDIA_CHECK_EQ(offerer->SetRemoteAnswer(*answer, &error), SignalingStatus::kOk);
    MEDIA_CHECK_EQ(offerer->state(), SignalingState::kAnswering);
    MEDIA_CHECK_EQ(offerer->SetRemoteAnswer(*answer, &error), SignalingStatus::kInvalidState);

    // 超时：clock 前进，Tick 把 offering/answering 打到 failed。
    clock.now = 1000 + 30000 + 1;
    MEDIA_CHECK_EQ(offerer->Tick(), SignalingStatus::kTimeout);
    MEDIA_CHECK_EQ(offerer->state(), SignalingState::kFailed);
    MEDIA_CHECK_EQ(offerer->Tick(), SignalingStatus::kOk);
    MEDIA_CHECK_EQ(offerer->OnIceFailed(), SignalingStatus::kInvalidState);

    // 关闭之后所有协商/候选操作都拒绝。
    offerer->Close();
    MEDIA_CHECK_EQ(offerer->state(), SignalingState::kClosed);
    MEDIA_CHECK(!offerer->CreateOffer(&error).has_value());
    MEDIA_CHECK_EQ(offerer->AddLocalCandidate(StandardCandidates("z", 9200).front(), &error),
                   SignalingStatus::kClosed);

    // ICE 失败路径：answering → failed。
    FakeClock fresh_clock;
    Pair pair = MakePair(fresh_clock);
    MEDIA_CHECK(ExchangeOfferAnswer(&pair, &error));
    MEDIA_CHECK_EQ(pair.answerer->OnIceFailed(), SignalingStatus::kOk);
    MEDIA_CHECK_EQ(pair.answerer->state(), SignalingState::kFailed);
    MEDIA_CHECK_EQ(pair.answerer->stats().ice_failed, std::uint64_t{1});

    // 未收到对端描述时不能添加远端候选。
    FakeClock idle_clock;
    auto idle = std::make_unique<SignalingSession>(MakeOffererConfig(idle_clock));
    MEDIA_CHECK_EQ(idle->AddRemoteCandidate(StandardCandidates("i", 9300).front(), &error),
                   SignalingStatus::kNoRemoteDescription);
}

// ---------------------------------------------------------------------------
// 4. SDP 格式、解析容错与候选编解码
// ---------------------------------------------------------------------------
void TestSdpFormatAndParsing() {
    FakeClock clock;
    auto offerer =
        std::make_unique<SignalingSession>(MakeOffererConfig(clock, StandardCandidates("o", 5000)));
    std::string error;
    auto offer = offerer->CreateOffer(&error);
    MEDIA_CHECK(offer.has_value());
    const std::string sdp = offer->ToSdp();

    const char* required[] = {
        "v=0\n",
        "o=- offerer-session 1 IN IP4 127.0.0.1\n",
        "s=go2cpp-media\n",
        "t=0 0\n",
        "a=group:BUNDLE 0 1\n",
        "m=video 9 UDP/TLS/RTP/SAVPF 96\n",
        "m=audio 9 UDP/TLS/RTP/SAVPF 111\n",
        "c=IN IP4 0.0.0.0\n",
        "a=mid:0\n",
        "a=mid:1\n",
        "a=rtpmap:96 H264/90000\n",
        "a=fmtp:96 packetization-mode=1\n",
        "a=rtpmap:111 opus/48000/2\n",
        "a=sendrecv\n",
        "a=ice-ufrag:uF1x\n",
        "a=ice-options:trickle\n",
        "a=fingerprint:sha-256 ",
        "a=setup:actpass\n",
        "a=candidate:oh 1 udp 2130706431 192.168.1.10 5000 typ host",
        "a=candidate:os 1 udp 1694498815 203.0.113.7 5001 typ srflx raddr 192.168.1.10 rport 5000",
        "a=candidate:or 1 udp 16777215 198.51.100.9 5002 typ relay raddr 198.51.100.9 rport 3478",
    };
    for (const char* fragment : required) {
        const bool found = sdp.find(fragment) != std::string::npos;
        if (!found) {
            std::printf("[ERROR] SDP 缺少片段: %s\n", fragment);
        }
        MEDIA_CHECK(found);
    }

    auto parsed = SessionDescription::ParseSdp(sdp, &error);
    MEDIA_CHECK(parsed.has_value());
    MEDIA_CHECK_EQ(parsed->ToSdp(), sdp);
    MEDIA_CHECK_EQ(parsed->media.size(), std::size_t{2});
    MEDIA_CHECK_EQ(parsed->FindMedia("0")->candidates.size(), std::size_t{3});
    MEDIA_CHECK_EQ(parsed->FindMedia("0")->setup, DtlsSetup::kActpass);
    MEDIA_CHECK_EQ(parsed->FindMedia("1")->codecs.front().channels, 2);

    // 未知属性要忽略（浏览器会塞很多 rtcp-mux/extmap）。
    std::string tolerant = sdp;
    tolerant += "a=rtcp-mux\n";
    tolerant += "a=extmap:1 urn:ietf:params:rtp-hdrext:ssrc-audio-level\n";
    tolerant += "b=AS:300\n";
    MEDIA_CHECK(SessionDescription::ParseSdp(tolerant, &error).has_value());

    // 损坏的 SDP：必须整体失败并给出原因。
    auto expect_parse_failure = [](std::string_view text) {
        std::string message;
        const auto result = SessionDescription::ParseSdp(text, &message);
        MEDIA_CHECK(!result.has_value());
        MEDIA_CHECK(!message.empty());
    };
    expect_parse_failure("v=1\n");
    expect_parse_failure("v=0\no=- x 1 IN IP4 127.0.0.1\ns=x\nt=0 0\n");
    expect_parse_failure("v=0\no=- x 1 IN IP4 127.0.0.1\ns=x\nt=0 0\nm=video 9 udp 96\n");
    std::string bad_candidate = sdp;
    const std::size_t candidate_pos = bad_candidate.find("a=candidate:oh");
    MEDIA_CHECK(candidate_pos != std::string::npos);
    bad_candidate.replace(candidate_pos, bad_candidate.find('\n', candidate_pos) - candidate_pos,
                          "a=candidate:bad 1 udp 0 300.1.1.1 0 typ host");
    expect_parse_failure(bad_candidate);
    std::string bad_setup = sdp;
    const std::size_t setup_pos = bad_setup.find("a=setup:actpass");
    MEDIA_CHECK(setup_pos != std::string::npos);
    bad_setup.replace(setup_pos, 15, "a=setup:weird");
    expect_parse_failure(bad_setup);
    std::string duplicate_mid = sdp;
    const std::size_t second_mid = duplicate_mid.find("a=mid:1");
    MEDIA_CHECK(second_mid != std::string::npos);
    duplicate_mid.replace(second_mid, 7, "a=mid:0");
    expect_parse_failure(duplicate_mid);
    std::string unknown_bundle = sdp;
    const std::string bundle_line = "a=group:BUNDLE 0 1";
    const std::size_t bundle_pos = unknown_bundle.find(bundle_line);
    MEDIA_CHECK(bundle_pos != std::string::npos);
    unknown_bundle.replace(bundle_pos, bundle_line.size(), "a=group:BUNDLE 0 9");
    expect_parse_failure(unknown_bundle);

    // 候选文本格式与解析。
    for (const IceCandidateType type : {IceCandidateType::kHost, IceCandidateType::kSrflx,
                                        IceCandidateType::kRelay, IceCandidateType::kPrflx}) {
        IceCandidate candidate = MakeCandidate(type, "fnd", "192.0.2.10", 5100, "192.0.2.1", 5000);
        const std::string line = candidate.ToString();
        auto decoded = IceCandidate::Parse(line, &error);
        MEDIA_CHECK(decoded.has_value());
        MEDIA_CHECK(*decoded == candidate);
        MEDIA_CHECK_EQ(decoded->type, type);
        MEDIA_CHECK_EQ(decoded->related_port, 5000);
        MEDIA_CHECK_EQ(IceCandidate::Parse("a=" + line, &error).has_value(), true);
    }
    MEDIA_CHECK(!IceCandidate::Parse("candidate:1 1 udp 1 1.2.3.4 0 typ host", &error).has_value());
    MEDIA_CHECK(
        !IceCandidate::Parse("candidate:1 1 udp 1 1.2.3.4 5000 typ bogus", &error).has_value());
    MEDIA_CHECK(!IceCandidate::Parse("garbage", &error).has_value());

    // 信令消息格式错误。
    MEDIA_CHECK(!SignalingMessage::Parse("g2p-signal/1 offer\n", &error).has_value());
    MEDIA_CHECK(!SignalingMessage::Parse("bad header\nx", &error).has_value());
    MEDIA_CHECK(!SignalingMessage::Parse("g2p-signal/1 unknown\nx", &error).has_value());
}

// ---------------------------------------------------------------------------
// 5. 与 P2P（ICE）集成：候选互转 + LoopbackSignaling 承载信令
// ---------------------------------------------------------------------------
void TestP2PInterop() {
#if defined(GO2CPP_MEDIA_HAVE_P2P)
    for (const IceCandidateType type : {IceCandidateType::kHost, IceCandidateType::kSrflx,
                                        IceCandidateType::kRelay, IceCandidateType::kPrflx}) {
        const IceCandidate candidate =
            MakeCandidate(type, "fnd", "192.0.2.10", 5100, "192.0.2.1", 5000);
        const go2cpp::p2p::Candidate converted = go2cpp::media::ToP2PCandidate(candidate);
        MEDIA_CHECK_EQ(converted.endpoint.AddressString(), candidate.address);
        MEDIA_CHECK_EQ(converted.endpoint.port(), candidate.port);
        MEDIA_CHECK_EQ(converted.foundation, candidate.foundation);
        MEDIA_CHECK_EQ(converted.priority, candidate.priority);
        MEDIA_CHECK_EQ(std::string(go2cpp::p2p::CandidateTypeName(converted.type)),
                       std::string(go2cpp::media::IceCandidateTypeName(type)));
        MEDIA_CHECK_EQ(go2cpp::p2p::ComputeCandidatePriority(converted.type),
                       ComputeIceCandidatePriority(type));
        if (!candidate.related_address.empty()) {
            MEDIA_CHECK(converted.base.valid());
            MEDIA_CHECK_EQ(converted.base.AddressString(), candidate.related_address);
            MEDIA_CHECK_EQ(converted.base.port(), candidate.related_port);
        }
        auto roundtrip = go2cpp::media::FromP2PCandidate(converted);
        MEDIA_CHECK(roundtrip.has_value());
        MEDIA_CHECK(*roundtrip == candidate);
    }

    // 用 P2P 的 LoopbackSignaling 承载 offer/answer + 候选（真实信令通道替换）。
    auto sides = go2cpp::p2p::LoopbackSignaling::CreatePair();
    FakeClock clock;
    Pair pair = MakePair(clock);
    std::string error;
    auto offer = pair.offerer->CreateOffer(&error);
    MEDIA_CHECK(offer.has_value());
    SignalingMessage offer_message;
    offer_message.type = SignalingMessageType::kOffer;
    offer_message.sdp = offer->ToSdp();
    MEDIA_CHECK(sides.first->Send(offer_message.ToText()));
    std::string payload;
    MEDIA_CHECK_GE(sides.second->Recv(&payload, std::chrono::milliseconds(1000)), 1);
    auto received = SignalingMessage::Parse(payload, &error);
    MEDIA_CHECK(received.has_value());
    auto parsed_offer = SessionDescription::ParseSdp(received->sdp, &error);
    MEDIA_CHECK(parsed_offer.has_value());
    MEDIA_CHECK_EQ(pair.answerer->SetRemoteOffer(*parsed_offer, &error), SignalingStatus::kOk);
    auto answer = pair.answerer->CreateAnswer(&error);
    MEDIA_CHECK(answer.has_value());
    SignalingMessage answer_message;
    answer_message.type = SignalingMessageType::kAnswer;
    answer_message.sdp = answer->ToSdp();
    MEDIA_CHECK(sides.second->Send(answer_message.ToText()));
    MEDIA_CHECK_GE(sides.first->Recv(&payload, std::chrono::milliseconds(1000)), 1);
    auto received_answer = SignalingMessage::Parse(payload, &error);
    MEDIA_CHECK(received_answer.has_value());
    auto parsed_answer = SessionDescription::ParseSdp(received_answer->sdp, &error);
    MEDIA_CHECK(parsed_answer.has_value());
    MEDIA_CHECK_EQ(pair.offerer->SetRemoteAnswer(*parsed_answer, &error), SignalingStatus::kOk);

    // 候选消息单独 trickle。
    const IceCandidate extra =
        MakeCandidate(IceCandidateType::kSrflx, "late", "203.0.113.30", 6100, "10.0.0.5", 6000);
    SignalingMessage candidate_message;
    candidate_message.type = SignalingMessageType::kCandidate;
    candidate_message.candidate = extra;
    MEDIA_CHECK(sides.first->Send(candidate_message.ToText()));
    MEDIA_CHECK_GE(sides.second->Recv(&payload, std::chrono::milliseconds(1000)), 1);
    auto candidate_decoded = SignalingMessage::Parse(payload, &error);
    MEDIA_CHECK(candidate_decoded.has_value());
    MEDIA_CHECK_EQ(pair.answerer->AddRemoteCandidate(candidate_decoded->candidate, &error),
                   SignalingStatus::kOk);
    MEDIA_CHECK(ConnectPair(&pair, &error));
    std::printf("[INFO] P2P interop: 4 种候选互转 + LoopbackSignaling offer/answer OK\n");
#else
    std::printf("[SKIP] P2P interop: 构建时未链接 go2cpp_p2p\n");
    media_test::g_skips++;
#endif
}

// ---------------------------------------------------------------------------
// 场景 1：1v1 视频通话（3 候选，完整协商到 connected）
// ---------------------------------------------------------------------------
void SceneOneToOneCall() {
    FakeClock clock;
    Pair pair = MakePair(clock);
    std::string error;
    MEDIA_CHECK(ExchangeOfferAnswer(&pair, &error));
    MEDIA_CHECK(ConnectPair(&pair, &error));
    MEDIA_CHECK(pair.offerer->connected());
    MEDIA_CHECK(pair.answerer->connected());
    MEDIA_CHECK_EQ(pair.offerer->remote_description()->media.size(), std::size_t{2});
    MEDIA_CHECK_EQ(pair.offerer->selected_remote_candidate()->type, IceCandidateType::kHost);
    std::printf("[SCENE] 1v1 call: connected via host pair\n");
}

// ---------------------------------------------------------------------------
// 场景 2：3 方经 SFU（每方一条 offer/answer 会话）
// ---------------------------------------------------------------------------
void SceneThreePartyViaSfu() {
    FakeClock clock;
    std::vector<std::string> sfu_ufrags;
    for (int peer = 1; peer <= 3; ++peer) {
        auto client = std::make_unique<SignalingSession>(
            MakeOffererConfig(clock, StandardCandidates("p" + std::to_string(peer), 7000)));
        SignalingConfig sfu_config =
            MakeAnswererConfig(clock, StandardCandidates("s" + std::to_string(peer), 8000));
        // 清空凭据，让每个 SFU 会话自动生成不同的 ufrag/pwd（验证多会话隔离）。
        sfu_config.ice_ufrag.clear();
        sfu_config.ice_pwd.clear();
        auto sfu = std::make_unique<SignalingSession>(std::move(sfu_config));
        std::string error;
        auto offer = client->CreateOffer(&error);
        MEDIA_CHECK(offer.has_value());
        auto parsed_offer = SessionDescription::ParseSdp(offer->ToSdp(), &error);
        MEDIA_CHECK(parsed_offer.has_value());
        MEDIA_CHECK_EQ(sfu->SetRemoteOffer(*parsed_offer, &error), SignalingStatus::kOk);
        auto answer = sfu->CreateAnswer(&error);
        MEDIA_CHECK(answer.has_value());
        auto parsed_answer = SessionDescription::ParseSdp(answer->ToSdp(), &error);
        MEDIA_CHECK(parsed_answer.has_value());
        MEDIA_CHECK_EQ(client->SetRemoteAnswer(*parsed_answer, &error), SignalingStatus::kOk);
        const auto client_host = FindCandidate(client->local_candidates(), IceCandidateType::kHost);
        const auto sfu_host = FindCandidate(sfu->remote_candidates(), IceCandidateType::kHost);
        const auto sfu_local = FindCandidate(sfu->local_candidates(), IceCandidateType::kHost);
        const auto client_remote =
            FindCandidate(client->remote_candidates(), IceCandidateType::kHost);
        MEDIA_CHECK(client_host.has_value());
        MEDIA_CHECK(sfu_host.has_value());
        MEDIA_CHECK(sfu_local.has_value());
        MEDIA_CHECK(client_remote.has_value());
        MEDIA_CHECK_EQ(client->OnIceConnected(*client_host, *client_remote, &error),
                       SignalingStatus::kOk);
        MEDIA_CHECK_EQ(sfu->OnIceConnected(*sfu_local, *sfu_host, &error), SignalingStatus::kOk);
        MEDIA_CHECK(sfu->connected());
        sfu_ufrags.push_back(sfu->config().ice_ufrag);
    }
    MEDIA_CHECK_EQ(sfu_ufrags.size(), std::size_t{3});
    bool distinct = true;
    for (std::size_t i = 0; i < sfu_ufrags.size(); ++i) {
        for (std::size_t j = i + 1; j < sfu_ufrags.size(); ++j) {
            distinct = distinct && sfu_ufrags[i] != sfu_ufrags[j];
        }
    }
    MEDIA_CHECK(distinct);
    std::printf("[SCENE] 3-party via SFU: 3 sessions connected, distinct ufrags=%zu\n",
                sfu_ufrags.size());
}

// ---------------------------------------------------------------------------
// 场景 3：trickle —— 描述先行、候选后到
// ---------------------------------------------------------------------------
void SceneTrickleLateCandidates() {
    FakeClock clock;
    Pair pair = MakePair(clock, /*with_candidates=*/false);
    std::string error;
    MEDIA_CHECK(ExchangeOfferAnswer(&pair, &error));
    MEDIA_CHECK_EQ(pair.offerer->state(), SignalingState::kAnswering);
    MEDIA_CHECK(!pair.offerer->BestRemoteCandidate().has_value());

    const IceCandidate offerer_host =
        MakeCandidate(IceCandidateType::kHost, "oh", "10.0.0.1", 8000);
    const IceCandidate answerer_host =
        MakeCandidate(IceCandidateType::kHost, "ah", "10.0.0.2", 8001);
    MEDIA_CHECK_EQ(pair.offerer->AddLocalCandidate(offerer_host, &error), SignalingStatus::kOk);
    MEDIA_CHECK_EQ(pair.answerer->AddLocalCandidate(answerer_host, &error), SignalingStatus::kOk);

    SignalingMessage candidate_message;
    candidate_message.type = SignalingMessageType::kCandidate;
    candidate_message.candidate = answerer_host;
    auto decoded = SignalingMessage::Parse(candidate_message.ToText(), &error);
    MEDIA_CHECK(decoded.has_value());
    MEDIA_CHECK_EQ(pair.offerer->AddRemoteCandidate(decoded->candidate, &error),
                   SignalingStatus::kOk);
    MEDIA_CHECK_EQ(pair.answerer->AddRemoteCandidate(offerer_host, &error), SignalingStatus::kOk);
    auto best = pair.offerer->BestRemoteCandidate();
    MEDIA_CHECK(best.has_value());
    MEDIA_CHECK(*best == answerer_host);
    MEDIA_CHECK(ConnectPair(&pair, &error));
    std::printf("[SCENE] trickle: 0 候选完成描述协商，补候选后 connected\n");
}

// ---------------------------------------------------------------------------
// 场景 4：只有 relay 候选（NAT 严格时的中继降级）
// ---------------------------------------------------------------------------
void SceneRelayFallback() {
    FakeClock clock;
    const IceCandidate relay_offerer =
        MakeCandidate(IceCandidateType::kRelay, "ro", "198.51.100.30", 9100, "198.51.100.30", 3478);
    const IceCandidate relay_answerer =
        MakeCandidate(IceCandidateType::kRelay, "ra", "198.51.100.31", 9101, "198.51.100.31", 3478);
    auto offerer = std::make_unique<SignalingSession>(MakeOffererConfig(clock, {relay_offerer}));
    auto answerer = std::make_unique<SignalingSession>(MakeAnswererConfig(clock, {relay_answerer}));
    Pair pair{std::move(offerer), std::move(answerer)};
    std::string error;
    MEDIA_CHECK(ExchangeOfferAnswer(&pair, &error));
    auto best = pair.offerer->BestRemoteCandidate();
    MEDIA_CHECK(best.has_value());
    MEDIA_CHECK_EQ(best->type, IceCandidateType::kRelay);
    MEDIA_CHECK_EQ(pair.offerer->OnIceConnected(relay_offerer, relay_answerer, &error),
                   SignalingStatus::kOk);
    MEDIA_CHECK_EQ(pair.answerer->OnIceConnected(relay_answerer, relay_offerer, &error),
                   SignalingStatus::kOk);
    MEDIA_CHECK_EQ(pair.offerer->selected_remote_candidate()->type, IceCandidateType::kRelay);
    std::printf("[SCENE] relay fallback: selected relay pair\n");
}

// ---------------------------------------------------------------------------
// 场景 5：协商超时后重建会话（失败状态隔离）
// ---------------------------------------------------------------------------
void SceneNegotiationTimeout() {
    FakeClock clock;
    std::string error;
    SignalingConfig config = MakeOffererConfig(clock);
    config.negotiation_timeout_ms = 2000;
    auto session = std::make_unique<SignalingSession>(config);
    auto offer = session->CreateOffer(&error);
    MEDIA_CHECK(offer.has_value());
    clock.now += 2001;
    MEDIA_CHECK_EQ(session->Tick(), SignalingStatus::kTimeout);
    MEDIA_CHECK_EQ(session->state(), SignalingState::kFailed);

    FakeClock retry_clock;
    Pair retry = MakePair(retry_clock);
    MEDIA_CHECK(ExchangeOfferAnswer(&retry, &error));
    MEDIA_CHECK(ConnectPair(&retry, &error));
    MEDIA_CHECK(retry.offerer->connected());
    std::printf("[SCENE] timeout: failed session dropped, retry connected\n");
}

// ---------------------------------------------------------------------------
// 场景 6：answer 校验（mid / DTLS 角色 / 媒体数量不匹配都要拒绝）
// ---------------------------------------------------------------------------
void SceneAnswerValidation() {
    FakeClock clock;
    std::string error;
    auto offerer = std::make_unique<SignalingSession>(MakeOffererConfig(clock));
    auto answerer = std::make_unique<SignalingSession>(MakeAnswererConfig(clock));
    auto offer = offerer->CreateOffer(&error);
    MEDIA_CHECK(offer.has_value());
    MEDIA_CHECK_EQ(answerer->SetRemoteOffer(*offer, &error), SignalingStatus::kOk);
    auto answer = answerer->CreateAnswer(&error);
    MEDIA_CHECK(answer.has_value());

    SessionDescription tampered = *answer;
    tampered.media[0].mid = "9";
    tampered.bundle_mids = {"9", "1"};
    MEDIA_CHECK_EQ(offerer->SetRemoteAnswer(tampered, &error), SignalingStatus::kMediaMismatch);
    MEDIA_CHECK_EQ(offerer->state(), SignalingState::kOffering);

    tampered = *answer;
    tampered.media[0].setup = DtlsSetup::kActpass;
    MEDIA_CHECK_EQ(offerer->SetRemoteAnswer(tampered, &error), SignalingStatus::kMediaMismatch);

    tampered = *answer;
    tampered.media.pop_back();
    tampered.bundle_mids.pop_back();
    MEDIA_CHECK_EQ(offerer->SetRemoteAnswer(tampered, &error), SignalingStatus::kMediaMismatch);

    MEDIA_CHECK_EQ(offerer->SetRemoteAnswer(*answer, &error), SignalingStatus::kOk);
    std::printf("[SCENE] answer validation: mid/setup/media-count 三类错误均拒绝\n");
}

// ---------------------------------------------------------------------------
// 随机测试：SDP 变异 fuzz、随机候选编解码、随机字节串喂解析器
// ---------------------------------------------------------------------------
void TestRandomizedParsing() {
    FakeClock clock;
    auto offerer =
        std::make_unique<SignalingSession>(MakeOffererConfig(clock, StandardCandidates("o", 5000)));
    std::string error;
    auto offer = offerer->CreateOffer(&error);
    MEDIA_CHECK(offer.has_value());
    const std::string base = offer->ToSdp();

    std::mt19937 rng(20261102U);
    auto pick = [&rng](std::size_t count) {
        return std::uniform_int_distribution<std::size_t>(0, count - 1)(rng);
    };

    int accepted = 0;
    int rejected = 0;
    for (int iteration = 0; iteration < 500; ++iteration) {
        std::string mutated = base;
        const int edits = 1 + static_cast<int>(pick(4));
        for (int edit = 0; edit < edits; ++edit) {
            const std::size_t position = mutated.empty() ? 0 : pick(mutated.size());
            const int action = static_cast<int>(pick(3));
            if (action == 0 && !mutated.empty()) {
                mutated[position] = static_cast<char>(pick(256));
            } else if (action == 1) {
                mutated.insert(mutated.begin() + static_cast<long>(position),
                               static_cast<char>(pick(256)));
            } else if (!mutated.empty()) {
                mutated.erase(position, 1);
            }
        }
        std::string mutation_error;
        auto parsed = SessionDescription::ParseSdp(mutated, &mutation_error);
        if (parsed.has_value()) {
            ++accepted;
            // 解析成功的描述必须能再次序列化/解析（幂等）。
            std::string again_error;
            auto again = SessionDescription::ParseSdp(parsed->ToSdp(), &again_error);
            MEDIA_CHECK(again.has_value());
            MEDIA_CHECK(!parsed->session_id.empty());
        } else {
            ++rejected;
            MEDIA_CHECK(!mutation_error.empty());
        }
    }
    MEDIA_CHECK_EQ(accepted + rejected, 500);

    // 随机合法候选：ToString → Parse → 逐字段相等。
    const IceCandidateType types[] = {IceCandidateType::kHost, IceCandidateType::kSrflx,
                                      IceCandidateType::kRelay, IceCandidateType::kPrflx};
    for (int iteration = 0; iteration < 1000; ++iteration) {
        const std::string address = "10." + std::to_string(pick(256)) + "." +
                                    std::to_string(pick(256)) + "." + std::to_string(pick(256));
        const IceCandidate candidate =
            MakeCandidate(types[pick(4)], "f" + std::to_string(iteration), address,
                          static_cast<std::uint16_t>(1 + pick(65535)));
        auto decoded = IceCandidate::Parse(candidate.ToString(), &error);
        MEDIA_CHECK(decoded.has_value());
        MEDIA_CHECK(*decoded == candidate);
        MEDIA_CHECK_EQ(decoded->priority, candidate.priority);
    }

    // 随机字节串：解析器只允许「拒绝」或「解析出合法结构」，不能崩溃。
    for (int iteration = 0; iteration < 300; ++iteration) {
        std::string bytes;
        const std::size_t length = pick(200);
        for (std::size_t index = 0; index < length; ++index) {
            bytes.push_back(static_cast<char>(pick(256)));
        }
        std::string fuzz_error;
        (void)SessionDescription::ParseSdp(bytes, &fuzz_error);
        (void)IceCandidate::Parse(bytes, &fuzz_error);
        (void)SignalingMessage::Parse(bytes, &fuzz_error);
    }
    std::printf("[INFO] randomized parsing: sdp accepted=%d rejected=%d, 1000 candidates\n",
                accepted, rejected);
}

}  // namespace

int main() {
    TestOfferAnswerRoundTrip();
    TestCandidateExchange();
    TestStateMachine();
    TestSdpFormatAndParsing();
    TestRandomizedParsing();
    TestP2PInterop();
    SceneOneToOneCall();
    SceneThreePartyViaSfu();
    SceneTrickleLateCandidates();
    SceneRelayFallback();
    SceneNegotiationTimeout();
    SceneAnswerValidation();
    return media_test::Finish("test_signaling");
}
