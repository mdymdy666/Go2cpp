/**
 * @file test_p2p_peer_validation.cpp
 * @brief 回归测试：连通性检查必须来自已交换候选（防止第三方劫持 peer）。
 * @details 攻防场景：victim 只与一个不存在的 decoy 候选交换过；攻击者向
 *          victim 的 UDP 端口猛发 STUN Binding Request。修复前 victim 会
 *          把 peer 锁定到攻击者并让 Connect 成功；修复后必须继续等待
 *          真实候选并最终超时失败。第二个用例保证正常直连路径未被收紧
 *          破坏（双端互相交换候选后仍能连通并收发数据）。
 * @author go2cpp contributors
 * @date 2026-11-04
 * @copyright MIT License
 */

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "go2cpp/p2p/candidate.hpp"
#include "go2cpp/p2p/endpoint.hpp"
#include "go2cpp/p2p/p2p_channel.hpp"
#include "go2cpp/p2p/stun_message.hpp"
#include "go2cpp/p2p/transport.hpp"

namespace {

using namespace go2cpp::p2p;

using namespace std::chrono_literals;

int g_failures = 0;

#define P2P_VALIDATION_CHECK(condition)                                        \
    do {                                                                       \
        if (!(condition)) {                                                    \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);   \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

P2PConfig MakeConfig() {
    P2PConfig config;
    config.bind_endpoint = Endpoint::Loopback(0);
    config.gather_host_candidates = false;
    config.enable_relay = false;
    config.keepalive_interval = 0ms;
    config.peer_timeout = 0ms;
    return config;
}

bool SendRecv(P2PChannel* from, P2PChannel* to, const char* text) {
    const std::size_t length = std::strlen(text);
    if (!from->Send(text, length)) {
        return false;
    }
    char buffer[64] = {};
    const auto deadline = std::chrono::steady_clock::now() + 1000ms;
    while (std::chrono::steady_clock::now() < deadline) {
        const std::size_t received = to->Recv(buffer, sizeof(buffer), 50ms);
        if (received == length && std::memcmp(buffer, text, length) == 0) {
            return true;
        }
    }
    return false;
}

/// 攻击者用任意源地址发 Binding Request，不能被当作对端建立连接。
void TestRejectsThirdPartyRequest() {
    P2PChannel victim{MakeConfig()};
    const Endpoint victim_local = victim.LocalEndpoint();
    P2P_VALIDATION_CHECK(victim_local.valid());

    Candidate decoy;
    decoy.type = CandidateType::kHost;
    decoy.endpoint = Endpoint::Loopback(47999);
    victim.AddRemoteCandidate(decoy);

    UdpTransportConfig attacker_config;
    attacker_config.bind_endpoint = Endpoint::Loopback(0);
    UdpTransport attacker;
    std::string error;
    P2P_VALIDATION_CHECK(attacker.Start(&error));

    std::string serialize_error;
    const std::vector<std::uint8_t> wire =
        SerializeStunMessage(StunMessage::MakeBindingRequest(), &serialize_error);
    P2P_VALIDATION_CHECK(!wire.empty());

    std::atomic<bool> connected{false};
    std::thread connect_thread([&victim, &connected]() { connected.store(victim.Connect(400ms)); });

    const auto deadline = std::chrono::steady_clock::now() + 300ms;
    std::size_t sent = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        if (attacker.SendTo(victim_local, wire.data(), wire.size())) {
            ++sent;
        }
        std::this_thread::sleep_for(3ms);
    }
    connect_thread.join();

    P2P_VALIDATION_CHECK(sent > 10);
    P2P_VALIDATION_CHECK(!connected.load());
    P2P_VALIDATION_CHECK(victim.state() != P2PState::kConnected);
    P2P_VALIDATION_CHECK(victim.state() != P2PState::kRelayed);
    const P2PState final_state = victim.state();
    victim.Close();
    std::printf("[INFO] 第三方 Binding Request 被拒绝（发送 %zu 个），state=%s\n", sent,
                P2PStateName(final_state));
}

/// 正常直连路径：双方互相交换候选后应连通并双向收发。
void TestDirectConnectStillWorks() {
    P2PChannel a{MakeConfig()};
    P2PChannel b{MakeConfig()};
    P2P_VALIDATION_CHECK(a.LocalEndpoint().valid());
    P2P_VALIDATION_CHECK(b.LocalEndpoint().valid());

    Candidate candidate_a;
    candidate_a.type = CandidateType::kHost;
    candidate_a.endpoint = a.LocalEndpoint();
    Candidate candidate_b;
    candidate_b.type = CandidateType::kHost;
    candidate_b.endpoint = b.LocalEndpoint();
    a.AddRemoteCandidate(candidate_b);
    b.AddRemoteCandidate(candidate_a);

    std::atomic<bool> ok_a{false};
    std::atomic<bool> ok_b{false};
    std::thread thread_a([&a, &ok_a]() { ok_a.store(a.Connect(1500ms)); });
    std::thread thread_b([&b, &ok_b]() { ok_b.store(b.Connect(1500ms)); });
    thread_a.join();
    thread_b.join();

    P2P_VALIDATION_CHECK(ok_a.load());
    P2P_VALIDATION_CHECK(ok_b.load());
    if (ok_a.load() && ok_b.load()) {
        P2P_VALIDATION_CHECK(a.PeerEndpoint() == b.LocalEndpoint());
        P2P_VALIDATION_CHECK(b.PeerEndpoint() == a.LocalEndpoint());
        P2P_VALIDATION_CHECK(SendRecv(&a, &b, "direct-forward"));
        P2P_VALIDATION_CHECK(SendRecv(&b, &a, "direct-backward"));
    } else {
        std::printf("[FAIL] 正常直连失败: a=%s b=%s\n", a.LastError().c_str(),
                    b.LastError().c_str());
    }
    a.Close();
    b.Close();
}

}  // namespace

int main() {
    TestRejectsThirdPartyRequest();
    TestDirectConnectStillWorks();
    if (g_failures == 0) {
        std::printf("[PASS] p2p peer validation\n");
        return 0;
    }
    std::printf("[FAIL] p2p peer validation: %d failures\n", g_failures);
    return 1;
}
