/**
 * @file test_pairing.cpp
 * @brief 配对协议测试：正常配对、错误密钥、超时、重复注册、响应丢失重试、
 *        确认丢失重放、并发 CONNECT、状态机随机序列与帧编解码。
 * @details 覆盖 MOD-13 / FR-01 / RISK-013 / AX-009：
 *          - 场景 1：正常配对（Register → Connect → Response → CONFIRM/ACK）。
 *          - 场景 2：错误密钥被拒，服务端状态不被破坏。
 *          - 场景 3：15s 超时（边界 15999/16000），Active 不超时。
 *          - 场景 4：重复 Register/Connect 返回 kAlreadyRegistered。
 *          - 场景 5：RESPONSE 丢失后客户端重发 CONNECT，服务端幂等重放。
 *          - 场景 6：应用拒绝 Response，客户端收到 kRejected 并关闭。
 *          - 场景 7：CONFIRM/ACK 丢失后的重发与幂等。
 *          - 场景 8：陈旧/伪造 RESPONSE 被丢弃。
 *          - 场景 9：两个 Client 同时 CONNECT 同一 key，先到先得。
 *          - 场景 10：参数/状态校验、Close/Reset、版本与畸形帧。
 *          - 场景 11：回调顺序与统计计数。
 *          - 场景 12：2000 步随机操作，状态迁移始终合法。
 * @author go2cpp contributors
 * @date 2026-10-28
 * @copyright MIT License
 */

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "go2cpp/bento/net/pairing.hpp"
#include "test_util.hpp"

namespace {

using go2cpp::bento::DecodePairingFrame;
using go2cpp::bento::EncodePairingFrame;
using go2cpp::bento::FixedKeyAuthPolicy;
using go2cpp::bento::kPairingFrameBytes;
using go2cpp::bento::PairingAuthPolicy;
using go2cpp::bento::PairingConfig;
using go2cpp::bento::PairingError;
using go2cpp::bento::PairingFrame;
using go2cpp::bento::PairingFrameType;
using go2cpp::bento::PairingRole;
using go2cpp::bento::PairingSession;
using go2cpp::bento::PairingState;

PairingConfig Config(std::uint64_t nonce = 0, std::uint32_t timeout_ms = 15000,
                     std::uint32_t retransmit_ms = 1000) {
    PairingConfig config;
    config.nonce = nonce;
    config.timeout_ms = timeout_ms;
    config.retransmit_ms = retransmit_ms;
    return config;
}

std::vector<PairingFrame> Drain(PairingSession& session) {
    std::vector<PairingFrame> frames;
    PairingFrame frame;
    while (session.PopOutbound(&frame)) {
        frames.push_back(frame);
    }
    return frames;
}

/// 完成 Register → Connect → Response，停在双方 Paired（尚未 Activate）。
bool PairUntilPaired(PairingSession& server, PairingSession& client, std::uint64_t now_ms) {
    if (server.Register("invite-key", now_ms) != PairingError::kNone) {
        return false;
    }
    if (client.Connect("invite-key", now_ms) != PairingError::kNone) {
        return false;
    }
    auto connect = Drain(client);
    if (connect.size() != 1) {
        return false;
    }
    if (server.HandleFrame(connect[0], now_ms) != PairingError::kNone) {
        return false;
    }
    if (server.Response(true, now_ms) != PairingError::kNone) {
        return false;
    }
    auto response = Drain(server);
    if (response.size() != 1) {
        return false;
    }
    if (client.HandleFrame(response[0], now_ms) != PairingError::kNone) {
        return false;
    }
    return client.state() == PairingState::kPaired &&
           server.state() == PairingState::kPaired &&
           client.pair_id() == server.pair_id();
}

/// 完整配对并激活；若尚未 Register/Connect 则先完成 Paired 阶段。
bool PairUntilActive(PairingSession& server, PairingSession& client, std::uint64_t now_ms) {
    if (server.state() == PairingState::kIdle && client.state() == PairingState::kIdle) {
        if (!PairUntilPaired(server, client, now_ms)) {
            return false;
        }
    }
    if (client.Activate(now_ms) != PairingError::kNone) {
        return false;
    }
    auto confirm = Drain(client);
    if (confirm.size() != 1) {
        return false;
    }
    if (server.HandleFrame(confirm[0], now_ms) != PairingError::kNone) {
        return false;
    }
    auto ack = Drain(server);
    if (ack.size() != 1) {
        return false;
    }
    if (client.HandleFrame(ack[0], now_ms) != PairingError::kNone) {
        return false;
    }
    return server.state() == PairingState::kActive &&
           client.state() == PairingState::kActive;
}

bool ValidState(PairingState state) {
    switch (state) {
        case PairingState::kIdle:
        case PairingState::kPending:
        case PairingState::kPaired:
        case PairingState::kActive:
        case PairingState::kClosed:
            return true;
    }
    return false;
}

/// 场景 1：正常配对，验证状态、pair_id、nonce 与回调顺序。
void TestHappyPath() {
    PairingSession server(Config());
    PairingSession client(Config(0x1122334455667788ULL));

    std::vector<PairingState> server_states;
    server.on_state_change = [&](PairingState from, PairingState to, PairingError error) {
        BENTO_CHECK_EQ(error, PairingError::kNone);
        server_states.push_back(to);
        if (from == to) {
            BENTO_CHECK(false);
        }
    };

    BENTO_CHECK_EQ(server.Register("invite-key", 1000), PairingError::kNone);
    BENTO_CHECK_EQ(server.state(), PairingState::kPending);
    BENTO_CHECK_EQ(server.role(), PairingRole::kServer);
    BENTO_CHECK_EQ(server.deadline_ms(), 16000U);

    BENTO_CHECK_EQ(client.Connect("invite-key", 1000), PairingError::kNone);
    BENTO_CHECK_EQ(client.role(), PairingRole::kClient);
    BENTO_CHECK_EQ(client.nonce(), 0x1122334455667788ULL);
    BENTO_CHECK_EQ(client.key_hash(), server.key_hash());

    auto connect = Drain(client);
    BENTO_CHECK_EQ(connect.size(), 1U);
    BENTO_CHECK_EQ(connect[0].type, PairingFrameType::kConnect);
    BENTO_CHECK_EQ(connect[0].key_hash, server.key_hash());
    BENTO_CHECK_EQ(connect[0].nonce, client.nonce());

    BENTO_CHECK_EQ(server.HandleFrame(connect[0], 1100), PairingError::kNone);
    BENTO_CHECK(server.HasPendingConnect());
    BENTO_CHECK_EQ(server.pending_connect_nonce(), client.nonce());
    BENTO_CHECK_EQ(server.Response(true, 1200), PairingError::kNone);
    BENTO_CHECK_EQ(server.state(), PairingState::kPaired);
    BENTO_CHECK(server.pair_id() != 0);

    auto response = Drain(server);
    BENTO_CHECK_EQ(response.size(), 1U);
    BENTO_CHECK_EQ(response[0].type, PairingFrameType::kResponse);
    BENTO_CHECK_EQ(response[0].status, PairingError::kNone);
    BENTO_CHECK_EQ(response[0].pair_id, server.pair_id());

    BENTO_CHECK_EQ(client.HandleFrame(response[0], 1200), PairingError::kNone);
    BENTO_CHECK_EQ(client.state(), PairingState::kPaired);
    BENTO_CHECK_EQ(client.pair_id(), server.pair_id());

    BENTO_CHECK_EQ(client.Activate(1300), PairingError::kNone);
    BENTO_CHECK_EQ(client.state(), PairingState::kPaired);  // 等 ACK
    auto confirm = Drain(client);
    BENTO_CHECK_EQ(confirm.size(), 1U);
    BENTO_CHECK_EQ(confirm[0].type, PairingFrameType::kConfirm);
    BENTO_CHECK_EQ(confirm[0].pair_id, server.pair_id());

    BENTO_CHECK_EQ(server.HandleFrame(confirm[0], 1300), PairingError::kNone);
    BENTO_CHECK_EQ(server.state(), PairingState::kActive);
    auto ack = Drain(server);
    BENTO_CHECK_EQ(ack.size(), 1U);
    BENTO_CHECK_EQ(client.HandleFrame(ack[0], 1300), PairingError::kNone);
    BENTO_CHECK_EQ(client.state(), PairingState::kActive);

    BENTO_CHECK_EQ(server_states.size(), 3U);  // Pending → Paired → Active
    BENTO_CHECK_EQ(server_states[0], PairingState::kPending);
    BENTO_CHECK_EQ(server_states[1], PairingState::kPaired);
    BENTO_CHECK_EQ(server_states[2], PairingState::kActive);
    BENTO_CHECK_EQ(server.error(), PairingError::kNone);
    BENTO_CHECK_EQ(client.error(), PairingError::kNone);
    std::printf("[INFO] happy path: pair_id=%u hash=%llu\n", server.pair_id(),
                static_cast<unsigned long long>(server.key_hash()));
}

/// 场景 2：错误密钥被拒，服务端仍保持 Pending，客户端关闭。
void TestWrongKey() {
    PairingSession server(Config());
    PairingSession client(Config());
    BENTO_CHECK_EQ(server.Register("alpha", 1000), PairingError::kNone);
    BENTO_CHECK_EQ(client.Connect("beta", 1000), PairingError::kNone);
    BENTO_CHECK(client.key_hash() != server.key_hash());

    auto connect = Drain(client);
    BENTO_CHECK_EQ(connect.size(), 1U);
    BENTO_CHECK_EQ(server.HandleFrame(connect[0], 1000), PairingError::kKeyMismatch);
    BENTO_CHECK_EQ(server.state(), PairingState::kPending);
    BENTO_CHECK(!server.HasPendingConnect());
    BENTO_CHECK_EQ(server.stats().key_mismatches, 1U);

    auto reject = Drain(server);
    BENTO_CHECK_EQ(reject.size(), 1U);
    BENTO_CHECK_EQ(reject[0].status, PairingError::kKeyMismatch);
    BENTO_CHECK_EQ(client.HandleFrame(reject[0], 1000), PairingError::kKeyMismatch);
    BENTO_CHECK_EQ(client.state(), PairingState::kClosed);
    BENTO_CHECK_EQ(client.error(), PairingError::kKeyMismatch);

    // 服务端还能接受持有正确密钥的另一个客户端。
    PairingSession good(Config());
    BENTO_CHECK_EQ(good.Connect("alpha", 1000), PairingError::kNone);
    auto good_connect = Drain(good);
    BENTO_CHECK_EQ(server.HandleFrame(good_connect[0], 1000), PairingError::kNone);
    BENTO_CHECK(server.HasPendingConnect());
}

/// 场景 3：15s 超时边界；Active 后不超时。
void TestTimeout() {
    PairingSession server(Config());
    PairingSession client(Config());
    BENTO_CHECK_EQ(server.Register("k", 1000), PairingError::kNone);
    BENTO_CHECK_EQ(client.Connect("k", 1000), PairingError::kNone);
    BENTO_CHECK_EQ(client.deadline_ms(), 16000U);

    BENTO_CHECK(!client.Tick(15999));
    BENTO_CHECK_EQ(client.state(), PairingState::kPending);
    BENTO_CHECK(client.Tick(16000));
    BENTO_CHECK_EQ(client.state(), PairingState::kClosed);
    BENTO_CHECK_EQ(client.error(), PairingError::kPeerTimeout);
    BENTO_CHECK_EQ(client.stats().timeouts, 1U);

    BENTO_CHECK(server.Tick(16000));
    BENTO_CHECK_EQ(server.state(), PairingState::kClosed);
    BENTO_CHECK_EQ(server.error(), PairingError::kPeerTimeout);

    // Active 状态不因时间流逝关闭。
    PairingSession active_server(Config());
    PairingSession active_client(Config());
    BENTO_CHECK(PairUntilActive(active_server, active_client, 0));
    BENTO_CHECK(!active_server.Tick(100000000U));
    BENTO_CHECK(!active_client.Tick(100000000U));
    BENTO_CHECK_EQ(active_server.state(), PairingState::kActive);
    BENTO_CHECK_EQ(active_client.state(), PairingState::kActive);
}

/// 场景 4：重复 Register/Connect 与空 key。
void TestDuplicateRegister() {
    PairingSession server(Config());
    BENTO_CHECK_EQ(server.Register("k", 0), PairingError::kNone);
    BENTO_CHECK_EQ(server.Register("k", 0), PairingError::kAlreadyRegistered);
    BENTO_CHECK_EQ(server.Register("other", 0), PairingError::kAlreadyRegistered);
    BENTO_CHECK_EQ(server.state(), PairingState::kPending);

    PairingSession client(Config());
    BENTO_CHECK_EQ(client.Connect("k", 0), PairingError::kNone);
    BENTO_CHECK_EQ(client.Connect("k", 0), PairingError::kAlreadyRegistered);

    PairingSession fresh(Config());
    BENTO_CHECK_EQ(fresh.Register("", 0), PairingError::kInvalidArgument);
    BENTO_CHECK_EQ(fresh.state(), PairingState::kIdle);
    BENTO_CHECK_EQ(fresh.Connect("", 0), PairingError::kInvalidArgument);
    BENTO_CHECK_EQ(fresh.state(), PairingState::kIdle);
    BENTO_CHECK_EQ(fresh.role(), PairingRole::kNone);
}

/// 场景 5：RESPONSE 丢失，客户端重发 CONNECT，服务端幂等重放同一 pair_id。
void TestResponseLostRetry() {
    PairingSession server(Config());
    PairingSession client(Config(0xabcdef0123456789ULL));
    BENTO_CHECK_EQ(server.Register("k", 0), PairingError::kNone);
    BENTO_CHECK_EQ(client.Connect("k", 0), PairingError::kNone);
    auto connect = Drain(client);
    BENTO_CHECK_EQ(connect.size(), 1U);
    BENTO_CHECK_EQ(server.HandleFrame(connect[0], 0), PairingError::kNone);
    BENTO_CHECK_EQ(server.Response(true, 0), PairingError::kNone);
    auto lost = Drain(server);
    BENTO_CHECK_EQ(lost.size(), 1U);  // 模拟 RESPONSE 在网络上丢失

    BENTO_CHECK_EQ(client.state(), PairingState::kPending);
    BENTO_CHECK(!client.Tick(1000));  // 状态不变
    auto retry = Drain(client);
    BENTO_CHECK_EQ(retry.size(), 1U);
    BENTO_CHECK_EQ(retry[0].type, PairingFrameType::kConnect);
    BENTO_CHECK_EQ(retry[0].nonce, connect[0].nonce);

    const std::uint32_t pair_id = server.pair_id();
    BENTO_CHECK_EQ(server.HandleFrame(retry[0], 1000), PairingError::kNone);
    BENTO_CHECK_EQ(server.state(), PairingState::kPaired);
    BENTO_CHECK_EQ(server.pair_id(), pair_id);  // 幂等：没有新会话
    auto replay = Drain(server);
    BENTO_CHECK_EQ(replay.size(), 1U);
    BENTO_CHECK_EQ(replay[0].pair_id, pair_id);
    BENTO_CHECK_EQ(server.stats().replays, 1U);
    BENTO_CHECK_EQ(client.HandleFrame(replay[0], 1000), PairingError::kNone);
    BENTO_CHECK_EQ(client.state(), PairingState::kPaired);
    BENTO_CHECK_EQ(client.pair_id(), pair_id);
}

/// 场景 6：服务端应用拒绝，客户端收到 kRejected；随后其他客户端可重试。
void TestRejectResponse() {
    PairingSession server(Config());
    PairingSession client(Config());
    BENTO_CHECK_EQ(server.Register("k", 0), PairingError::kNone);
    BENTO_CHECK_EQ(client.Connect("k", 0), PairingError::kNone);
    auto connect = Drain(client);
    BENTO_CHECK_EQ(server.HandleFrame(connect[0], 0), PairingError::kNone);
    BENTO_CHECK_EQ(server.Response(false, 0), PairingError::kNone);
    BENTO_CHECK_EQ(server.state(), PairingState::kPending);
    BENTO_CHECK(!server.HasPendingConnect());

    auto reject = Drain(server);
    BENTO_CHECK_EQ(reject.size(), 1U);
    BENTO_CHECK_EQ(reject[0].status, PairingError::kRejected);
    BENTO_CHECK_EQ(client.HandleFrame(reject[0], 0), PairingError::kRejected);
    BENTO_CHECK_EQ(client.state(), PairingState::kClosed);
    BENTO_CHECK_EQ(client.error(), PairingError::kRejected);

    // 另一个客户端仍能配对。
    PairingSession second(Config());
    BENTO_CHECK_EQ(second.Connect("k", 0), PairingError::kNone);
    auto second_connect = Drain(second);
    BENTO_CHECK_EQ(server.HandleFrame(second_connect[0], 0), PairingError::kNone);
    BENTO_CHECK_EQ(server.Response(true, 0), PairingError::kNone);
}

/// 场景 7：CONFIRM 丢失后客户端重发，服务端 Active 后幂等重放 ACK。
void TestConfirmAckRetry() {
    PairingSession server(Config());
    PairingSession client(Config());
    BENTO_CHECK(PairUntilPaired(server, client, 0));
    BENTO_CHECK_EQ(client.Activate(0), PairingError::kNone);
    auto confirm = Drain(client);
    BENTO_CHECK_EQ(confirm.size(), 1U);  // 模拟 CONFIRM 丢失

    BENTO_CHECK_EQ(client.state(), PairingState::kPaired);
    BENTO_CHECK(!client.Tick(1000));
    auto retry = Drain(client);
    BENTO_CHECK_EQ(retry.size(), 1U);
    BENTO_CHECK_EQ(retry[0].type, PairingFrameType::kConfirm);

    BENTO_CHECK_EQ(server.HandleFrame(retry[0], 1000), PairingError::kNone);
    BENTO_CHECK_EQ(server.state(), PairingState::kActive);
    auto ack = Drain(server);
    BENTO_CHECK_EQ(ack.size(), 1U);  // 模拟 ACK 丢失
    BENTO_CHECK_EQ(client.state(), PairingState::kPaired);

    BENTO_CHECK(!client.Tick(2000));
    auto confirm_again = Drain(client);
    BENTO_CHECK_EQ(confirm_again.size(), 1U);
    BENTO_CHECK_EQ(server.HandleFrame(confirm_again[0], 2000), PairingError::kNone);
    BENTO_CHECK_EQ(server.state(), PairingState::kActive);
    BENTO_CHECK_EQ(server.stats().replays, 1U);
    auto ack_again = Drain(server);
    BENTO_CHECK_EQ(ack_again.size(), 1U);
    BENTO_CHECK_EQ(client.HandleFrame(ack_again[0], 2000), PairingError::kNone);
    BENTO_CHECK_EQ(client.state(), PairingState::kActive);
}

/// 场景 8：陈旧/伪造 RESPONSE（nonce 不匹配）被丢弃，不改变状态。
void TestStaleResponseIgnored() {
    PairingSession client(Config(0x1111ULL));
    BENTO_CHECK_EQ(client.Connect("k", 0), PairingError::kNone);
    (void)Drain(client);

    PairingFrame stale;
    stale.type = PairingFrameType::kResponse;
    stale.key_hash = client.key_hash();
    stale.nonce = 0x2222ULL;
    stale.pair_id = 7;
    stale.status = PairingError::kNone;
    BENTO_CHECK_EQ(client.HandleFrame(stale, 100), PairingError::kNone);
    BENTO_CHECK_EQ(client.state(), PairingState::kPending);

    PairingFrame good = stale;
    good.nonce = client.nonce();
    good.pair_id = 0;
    BENTO_CHECK_EQ(client.HandleFrame(good, 100), PairingError::kMalformedFrame);
    BENTO_CHECK_EQ(client.state(), PairingState::kPending);

    good.pair_id = 9;
    BENTO_CHECK_EQ(client.HandleFrame(good, 100), PairingError::kNone);
    BENTO_CHECK_EQ(client.state(), PairingState::kPaired);
    BENTO_CHECK_EQ(client.pair_id(), 9U);
}

/// 场景 9：两个 Client 同时 CONNECT 同一 key，先到先得（AX-009）。
void TestConcurrentConnectRejected() {
    PairingSession server(Config());
    PairingSession first(Config(0xaaaaULL));
    PairingSession second(Config(0xbbbbULL));
    BENTO_CHECK_EQ(server.Register("k", 0), PairingError::kNone);
    BENTO_CHECK_EQ(first.Connect("k", 0), PairingError::kNone);
    BENTO_CHECK_EQ(second.Connect("k", 0), PairingError::kNone);

    auto first_connect = Drain(first);
    auto second_connect = Drain(second);
    BENTO_CHECK_EQ(server.HandleFrame(first_connect[0], 0), PairingError::kNone);
    BENTO_CHECK_EQ(server.HandleFrame(second_connect[0], 0), PairingError::kRejected);
    BENTO_CHECK_EQ(server.state(), PairingState::kPending);
    auto reject = Drain(server);
    BENTO_CHECK_EQ(reject.size(), 1U);
    BENTO_CHECK_EQ(reject[0].status, PairingError::kRejected);
    BENTO_CHECK_EQ(reject[0].nonce, 0xbbbbULL);

    BENTO_CHECK_EQ(server.Response(true, 0), PairingError::kNone);
    auto accept = Drain(server);
    BENTO_CHECK_EQ(accept.size(), 1U);
    BENTO_CHECK_EQ(first.HandleFrame(accept[0], 0), PairingError::kNone);
    BENTO_CHECK_EQ(first.state(), PairingState::kPaired);
    BENTO_CHECK_EQ(second.state(), PairingState::kPending);  // 已收到 reject 帧但未处理
    BENTO_CHECK_EQ(second.HandleFrame(reject[0], 0), PairingError::kRejected);
    BENTO_CHECK_EQ(second.state(), PairingState::kClosed);
}

/// 场景 10：参数/状态校验、Close/Reset、版本与畸形帧。
void TestValidation() {
    // 未注册收到 CONNECT。
    PairingSession unregistered(Config());
    PairingFrame connect;
    connect.type = PairingFrameType::kConnect;
    connect.key_hash = 1;
    connect.nonce = 2;
    BENTO_CHECK_EQ(unregistered.HandleFrame(connect, 0), PairingError::kNotRegistered);

    PairingSession server(Config());
    PairingSession client(Config());
    BENTO_CHECK_EQ(server.Register("k", 0), PairingError::kNone);
    BENTO_CHECK_EQ(server.Response(true, 0), PairingError::kInvalidState);
    BENTO_CHECK_EQ(server.Activate(0), PairingError::kInvalidState);
    BENTO_CHECK_EQ(client.Response(true, 0), PairingError::kInvalidState);
    BENTO_CHECK_EQ(client.Activate(0), PairingError::kInvalidState);
    BENTO_CHECK_EQ(client.Connect("k", 0), PairingError::kNone);
    BENTO_CHECK_EQ(client.Activate(0), PairingError::kInvalidState);
    (void)Drain(client);

    // 版本/类型/status 校验。
    PairingFrame bad = connect;
    bad.version = 9;
    BENTO_CHECK_EQ(client.HandleFrame(bad, 0), PairingError::kProtocolVersion);
    bad = connect;
    bad.status = PairingError::kRejected;
    BENTO_CHECK_EQ(client.HandleFrame(bad, 0), PairingError::kMalformedFrame);
    PairingFrame register_frame;
    register_frame.type = PairingFrameType::kRegister;
    BENTO_CHECK_EQ(client.HandleFrame(register_frame, 0), PairingError::kInvalidState);

    // Close 幂等；Closed 后操作被拒绝；Reset 后可重来。
    BENTO_CHECK(server.Close());
    BENTO_CHECK(!server.Close());
    BENTO_CHECK_EQ(server.state(), PairingState::kClosed);
    BENTO_CHECK_EQ(server.HandleFrame(connect, 0), PairingError::kClosed);
    BENTO_CHECK_EQ(server.Register("k", 0), PairingError::kAlreadyRegistered);
    server.Reset();
    BENTO_CHECK_EQ(server.state(), PairingState::kIdle);
    BENTO_CHECK_EQ(server.role(), PairingRole::kNone);
    BENTO_CHECK_EQ(server.Register("k2", 0), PairingError::kNone);

    // 空 key 的指纹策略：VerifyKey 与空输入。
    FixedKeyAuthPolicy policy;
    std::uint64_t hash = 0;
    BENTO_CHECK(!policy.HashKey("", &hash));
    BENTO_CHECK(policy.HashKey("k", &hash));
    BENTO_CHECK(policy.VerifyKey("k", hash));
    BENTO_CHECK(!policy.VerifyKey("k2", hash));
    BENTO_CHECK_EQ(FixedKeyAuthPolicy::Hash("k"), hash);
}

/// 场景 11：帧编解码往返与异常帧拒绝。
void TestFrameCodec() {
    PairingFrame frame;
    frame.type = PairingFrameType::kResponse;
    frame.key_hash = 0x0102030405060708ULL;
    frame.nonce = 0x1112131415161718ULL;
    frame.pair_id = 0xdeadbeefU;
    frame.status = PairingError::kKeyMismatch;

    std::uint8_t buffer[kPairingFrameBytes] = {};
    EncodePairingFrame(frame, buffer);
    BENTO_CHECK_EQ(buffer[0], 0x42);  // 'B'
    BENTO_CHECK_EQ(buffer[1], 0x50);  // 'P'
    BENTO_CHECK_EQ(buffer[2], 0x41);  // 'A'
    BENTO_CHECK_EQ(buffer[3], 0x52);  // 'R'
    BENTO_CHECK_EQ(buffer[4], 1);     // version
    BENTO_CHECK_EQ(buffer[5], 0x03);  // RESPONSE
    BENTO_CHECK_EQ(buffer[6], 0);
    BENTO_CHECK_EQ(buffer[7], 0);
    BENTO_CHECK_EQ(buffer[8], 0x01);
    BENTO_CHECK_EQ(buffer[15], 0x08);
    BENTO_CHECK_EQ(buffer[16], 0x11);
    BENTO_CHECK_EQ(buffer[23], 0x18);
    BENTO_CHECK_EQ(buffer[24], 0xde);
    BENTO_CHECK_EQ(buffer[27], 0xef);
    BENTO_CHECK_EQ(buffer[28], 0x00);
    BENTO_CHECK_EQ(buffer[29], 0x01);  // kKeyMismatch = 1
    BENTO_CHECK_EQ(buffer[30], 0);
    BENTO_CHECK_EQ(buffer[31], 0);

    PairingFrame decoded;
    BENTO_CHECK(DecodePairingFrame(buffer, sizeof(buffer), &decoded));
    BENTO_CHECK_EQ(decoded.type, frame.type);
    BENTO_CHECK_EQ(decoded.version, frame.version);
    BENTO_CHECK_EQ(decoded.key_hash, frame.key_hash);
    BENTO_CHECK_EQ(decoded.nonce, frame.nonce);
    BENTO_CHECK_EQ(decoded.pair_id, frame.pair_id);
    BENTO_CHECK_EQ(decoded.status, frame.status);

    // 长度不足 / magic / 版本 / 类型 / flags / 保留位 / status 越界。
    BENTO_CHECK(!DecodePairingFrame(buffer, kPairingFrameBytes - 1, &decoded));
    std::uint8_t bad[kPairingFrameBytes] = {};
    std::memcpy(bad, buffer, sizeof(bad));
    bad[0] = 0;
    BENTO_CHECK(!DecodePairingFrame(bad, sizeof(bad), &decoded));
    std::memcpy(bad, buffer, sizeof(bad));
    bad[4] = 9;
    BENTO_CHECK(!DecodePairingFrame(bad, sizeof(bad), &decoded));
    std::memcpy(bad, buffer, sizeof(bad));
    bad[5] = 0x7f;
    BENTO_CHECK(!DecodePairingFrame(bad, sizeof(bad), &decoded));
    std::memcpy(bad, buffer, sizeof(bad));
    bad[7] = 1;
    BENTO_CHECK(!DecodePairingFrame(bad, sizeof(bad), &decoded));
    std::memcpy(bad, buffer, sizeof(bad));
    bad[30] = 1;
    BENTO_CHECK(!DecodePairingFrame(bad, sizeof(bad), &decoded));
    std::memcpy(bad, buffer, sizeof(bad));
    bad[28] = 0x01;
    bad[29] = 0x00;  // status = 256，越界
    BENTO_CHECK(!DecodePairingFrame(bad, sizeof(bad), &decoded));
    BENTO_CHECK(!DecodePairingFrame(nullptr, sizeof(buffer), &decoded));
    BENTO_CHECK(!DecodePairingFrame(buffer, sizeof(buffer), nullptr));
    BENTO_CHECK_EQ(std::string(go2cpp::bento::PairingErrorName(PairingError::kPeerTimeout)),
                   std::string("peer_timeout"));
    BENTO_CHECK_EQ(std::string(go2cpp::bento::PairingStateName(PairingState::kActive)),
                   std::string("active"));
}

/// 场景 12：完整流程统计计数。
void TestStats() {
    PairingSession server(Config());
    PairingSession client(Config());
    BENTO_CHECK(PairUntilActive(server, client, 0));
    const auto server_stats = server.stats();
    BENTO_CHECK_EQ(server_stats.registers, 1U);
    BENTO_CHECK_EQ(server_stats.responses, 1U);
    BENTO_CHECK_EQ(server_stats.frames_received, 2U);  // CONNECT + CONFIRM
    BENTO_CHECK_EQ(server_stats.frames_sent, 2U);      // RESPONSE + ACK
    const auto client_stats = client.stats();
    BENTO_CHECK_EQ(client_stats.connects, 1U);
    BENTO_CHECK_EQ(client_stats.confirms, 1U);
    BENTO_CHECK_EQ(client_stats.frames_received, 2U);
    BENTO_CHECK_EQ(client_stats.frames_sent, 2U);

    // 自动 nonce 非 0 且两次 Connect 不同（用 Reset 复用一个实例）。
    PairingSession auto_nonce(Config());
    BENTO_CHECK_EQ(auto_nonce.Connect("k", 0), PairingError::kNone);
    const std::uint64_t first = auto_nonce.nonce();
    BENTO_CHECK(first != 0);
    auto_nonce.Reset();
    BENTO_CHECK_EQ(auto_nonce.Connect("k", 0), PairingError::kNone);
    BENTO_CHECK(auto_nonce.nonce() != first);
}

/// 场景 13：2000 步随机操作，状态始终合法、不崩溃。
void TestRandomized() {
    std::mt19937 rng(20261028);
    PairingSession server(Config(0x1234ULL, 4000, 500));
    PairingSession client(Config(0x5678ULL, 4000, 500));
    std::uint64_t now_ms = 1000;
    int transitions = 0;

    for (int step = 0; step < 2000; ++step) {
        const auto before_server = server.state();
        const auto before_client = client.state();
        switch (rng() % 10U) {
            case 0:
                (void)server.Register("k", now_ms);
                break;
            case 1:
                (void)client.Connect("k", now_ms);
                break;
            case 2:
                (void)server.Response((rng() % 2U) == 0, now_ms);
                break;
            case 3:
                (void)client.Activate(now_ms);
                break;
            case 4: {
                PairingFrame frame;
                if (client.PopOutbound(&frame)) {
                    (void)server.HandleFrame(frame, now_ms);
                }
                break;
            }
            case 5: {
                PairingFrame frame;
                if (server.PopOutbound(&frame)) {
                    (void)client.HandleFrame(frame, now_ms);
                }
                break;
            }
            case 6:
                (void)server.Tick(now_ms);
                break;
            case 7:
                (void)client.Tick(now_ms);
                break;
            case 8:
                now_ms += 700;
                break;
            default:
                if (server.state() == PairingState::kClosed) {
                    server.Reset();
                }
                if (client.state() == PairingState::kClosed) {
                    client.Reset();
                }
                break;
        }
        if (server.state() != before_server) {
            ++transitions;
        }
        if (client.state() != before_client) {
            ++transitions;
        }
        BENTO_CHECK(ValidState(server.state()));
        BENTO_CHECK(ValidState(client.state()));
        BENTO_CHECK(server.outbound_size() <= go2cpp::bento::kPairingMaxOutbound);
        BENTO_CHECK(client.outbound_size() <= go2cpp::bento::kPairingMaxOutbound);
    }
    BENTO_CHECK(transitions > 0);
    std::printf("[INFO] randomized: transitions=%d server=%s client=%s\n", transitions,
                go2cpp::bento::PairingStateName(server.state()),
                go2cpp::bento::PairingStateName(client.state()));
}

}  // namespace

int main() {
    TestHappyPath();
    TestWrongKey();
    TestTimeout();
    TestDuplicateRegister();
    TestResponseLostRetry();
    TestRejectResponse();
    TestConfirmAckRetry();
    TestStaleResponseIgnored();
    TestConcurrentConnectRejected();
    TestValidation();
    TestFrameCodec();
    TestStats();
    TestRandomized();
    return bento_test::Finish("test_pairing");
}
