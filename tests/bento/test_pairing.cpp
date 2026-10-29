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
 *          - 场景 13：Activate 幂等（重复调用不重置 15s 超时/1s 重传计时器）。
 *          - 场景 14：成功 RESPONSE 必须回显 key_hash；未发 CONFIRM 拒绝 ACK。
 *          - 场景 15：30% 丢包 + 10% 重复 + 乱序链路下的配对收敛。
 *          - 场景 16：8 线程共 1000 个并发配对与 nonce 唯一性。
 *          - 场景 17：2000 帧随机 fuzz（非法 version/status/key/nonce）。
 *          - 场景 18：100 万次 32B 编解码往返与发送队列上限。
 * @author go2cpp contributors
 * @date 2026-10-28
 * @copyright MIT License
 */

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <unordered_set>
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

/// 场景 13：Activate 幂等——重复调用不得重置 15s 超时与 1s 重传计时器。
/// 回归背景：修复前每次 Activate 都重设 deadline/next_retransmit，应用在
/// 驱动循环里每帧调用一次就会让 CONFIRM 永不重传、超时永不触发。
void TestActivateIdempotent() {
    PairingSession server(Config(0, 4000, 500));
    PairingSession client(Config(0x4242ULL, 4000, 500));
    BENTO_CHECK(PairUntilPaired(server, client, 0));
    BENTO_CHECK_EQ(client.Activate(0), PairingError::kNone);
    auto confirm = Drain(client);
    BENTO_CHECK_EQ(confirm.size(), 1U);
    const std::uint64_t deadline = client.deadline_ms();
    BENTO_CHECK_EQ(deadline, 4000U);

    // 模拟驱动循环重复调用：计时器必须保持首次 Activate 的值。
    for (int step = 0; step < 100; ++step) {
        BENTO_CHECK_EQ(client.Activate(100 + static_cast<std::uint64_t>(step) * 10),
                       PairingError::kNone);
    }
    BENTO_CHECK_EQ(client.deadline_ms(), deadline);
    BENTO_CHECK_EQ(client.outbound_size(), 0U);  // 不重复排队 CONFIRM
    BENTO_CHECK_EQ(client.stats().confirms, 1U);

    BENTO_CHECK(!client.Tick(499));
    BENTO_CHECK_EQ(client.outbound_size(), 0U);
    BENTO_CHECK(!client.Tick(500));  // 仍按首次 Activate 的 500ms 节奏重传
    BENTO_CHECK_EQ(client.outbound_size(), 1U);
    BENTO_CHECK_EQ(client.stats().retransmits, 1U);
    BENTO_CHECK(client.Tick(4000));  // 超时仍然生效
    BENTO_CHECK_EQ(client.state(), PairingState::kClosed);
    BENTO_CHECK_EQ(client.error(), PairingError::kPeerTimeout);
}

/// 场景 14：成功 RESPONSE 必须回显本端 key_hash；客户端未发 CONFIRM 前
/// 收到 ACK 属于异常序列，必须拒绝而不是直接 Active。
void TestResponseKeyHashGuard() {
    PairingSession client(Config(0x7777ULL, 4000, 500));
    BENTO_CHECK_EQ(client.Connect("k", 0), PairingError::kNone);
    (void)Drain(client);

    PairingFrame response;
    response.type = PairingFrameType::kResponse;
    response.key_hash = client.key_hash() ^ 1ULL;
    response.nonce = client.nonce();
    response.pair_id = 42;
    response.status = PairingError::kNone;
    BENTO_CHECK_EQ(client.HandleFrame(response, 10), PairingError::kKeyMismatch);
    BENTO_CHECK_EQ(client.state(), PairingState::kPending);
    BENTO_CHECK_EQ(client.pair_id(), 0U);

    response.key_hash = client.key_hash();
    BENTO_CHECK_EQ(client.HandleFrame(response, 10), PairingError::kNone);
    BENTO_CHECK_EQ(client.state(), PairingState::kPaired);

    PairingFrame ack;
    ack.type = PairingFrameType::kConfirm;
    ack.key_hash = client.key_hash();
    ack.nonce = client.nonce();
    ack.pair_id = client.pair_id();
    BENTO_CHECK_EQ(client.HandleFrame(ack, 20), PairingError::kInvalidState);
    BENTO_CHECK_EQ(client.state(), PairingState::kPaired);

    BENTO_CHECK_EQ(client.Activate(30), PairingError::kNone);
    (void)Drain(client);
    BENTO_CHECK_EQ(client.HandleFrame(ack, 30), PairingError::kNone);
    BENTO_CHECK_EQ(client.state(), PairingState::kActive);
}

/// 场景 15：30% 丢包 + 10% 重复 + 0..3 步乱序下，配对在 15s 内收敛。
/// 该场景同时覆盖「应用每步都调用 Activate」这一真实驱动写法。
void TestLossyLinkConvergence() {
    struct InFlight {
        int deliver_step;
        bool to_server;
        PairingFrame frame;
    };
    std::mt19937_64 rng(20261029);
    constexpr int kTrials = 200;
    int completed = 0;
    for (int trial = 0; trial < kTrials; ++trial) {
        PairingSession server(Config());
        PairingSession client(Config());
        std::vector<InFlight> pipe;
        std::uint64_t now = 0;
        bool done = false;
        BENTO_CHECK_EQ(server.Register("k", 0), PairingError::kNone);
        BENTO_CHECK_EQ(client.Connect("k", 0), PairingError::kNone);
        for (int step = 0; step < 400; ++step) {
            now += 50;
            auto flush = [&](PairingSession& from, bool to_server) {
                PairingFrame frame;
                while (from.PopOutbound(&frame)) {
                    if (rng() % 100 < 30) {
                        continue;  // 丢包 30%
                    }
                    const int deliver = step + static_cast<int>(rng() % 4);
                    pipe.push_back(InFlight{deliver, to_server, frame});
                    if (rng() % 100 < 10) {
                        pipe.push_back(InFlight{deliver, to_server, frame});  // 重复 10%
                    }
                }
            };
            flush(server, false);
            flush(client, true);
            for (std::size_t index = 0; index < pipe.size();) {
                if (pipe[index].deliver_step <= step) {
                    if (pipe[index].to_server) {
                        (void)server.HandleFrame(pipe[index].frame, now);
                    } else {
                        (void)client.HandleFrame(pipe[index].frame, now);
                    }
                    pipe.erase(pipe.begin() + static_cast<std::ptrdiff_t>(index));
                } else {
                    ++index;
                }
            }
            if (server.HasPendingConnect()) {
                (void)server.Response(true, now);
            }
            if (client.state() == PairingState::kPaired) {
                (void)client.Activate(now);  // 每步调用：验证 Activate 幂等
            }
            (void)server.Tick(now);
            (void)client.Tick(now);
            if (server.state() == PairingState::kActive &&
                client.state() == PairingState::kActive) {
                done = true;
                break;
            }
        }
        if (done) {
            ++completed;
        }
        if (client.state() == PairingState::kActive) {
            BENTO_CHECK_EQ(server.state(), PairingState::kActive);
        }
        if ((server.state() == PairingState::kPaired ||
             server.state() == PairingState::kActive) &&
            (client.state() == PairingState::kPaired ||
             client.state() == PairingState::kActive)) {
            BENTO_CHECK_EQ(server.pair_id(), client.pair_id());
        }
    }
    BENTO_CHECK(completed * 100 >= kTrials * 98);  // 30% 丢包下 ≥98% 收敛
    std::printf("[INFO] lossy link: %d/%d trials converged\n", completed, kTrials);
}

/// 场景 16：1000 个并发配对（8 线程 × 125），nonce 全局唯一。
void TestThousandPairings() {
    constexpr unsigned kThreads = 8;
    constexpr std::size_t kPerThread = 125;
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    std::vector<std::vector<std::uint64_t>> nonces(kThreads);
    std::atomic<int> failures{0};
    for (unsigned thread_index = 0; thread_index < kThreads; ++thread_index) {
        threads.emplace_back([&, thread_index]() {
            nonces[thread_index].reserve(kPerThread);
            for (std::size_t index = 0; index < kPerThread; ++index) {
                PairingSession server(Config());
                PairingSession client(Config());
                if (!PairUntilActive(server, client, 0)) {
                    ++failures;
                    continue;
                }
                nonces[thread_index].push_back(client.nonce());
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    BENTO_CHECK_EQ(failures.load(), 0);
    std::unordered_set<std::uint64_t> unique;
    for (const auto& bucket : nonces) {
        for (const std::uint64_t nonce : bucket) {
            BENTO_CHECK(nonce != 0);
            unique.insert(nonce);
        }
    }
    BENTO_CHECK_EQ(unique.size(), static_cast<std::size_t>(kThreads) * kPerThread);
    std::printf("[INFO] concurrent pairings: %zu\n", unique.size());
}

/// 场景 17：2000 帧随机 fuzz，覆盖非法 version/status/key/nonce 与状态混合。
void TestRandomFrameFuzz() {
    std::mt19937_64 rng(0x5f5f5f5fULL);
    PairingSession server(Config(0x1000ULL, 4000, 500));
    PairingSession client(Config(0x2000ULL, 4000, 500));
    for (int step = 0; step < 2000; ++step) {
        if (rng() % 6 == 0) {
            switch (rng() % 6) {
                case 0:
                    (void)server.Register("k", static_cast<std::uint64_t>(step));
                    break;
                case 1:
                    (void)client.Connect("k", static_cast<std::uint64_t>(step));
                    break;
                case 2:
                    if (server.HasPendingConnect()) {
                        (void)server.Response((rng() % 2) == 0, static_cast<std::uint64_t>(step));
                    }
                    break;
                case 3:
                    (void)client.Activate(static_cast<std::uint64_t>(step));
                    break;
                case 4: {
                    PairingFrame frame;
                    while (client.PopOutbound(&frame)) {
                        (void)server.HandleFrame(frame, static_cast<std::uint64_t>(step));
                    }
                    break;
                }
                default: {
                    PairingFrame frame;
                    while (server.PopOutbound(&frame)) {
                        (void)client.HandleFrame(frame, static_cast<std::uint64_t>(step));
                    }
                    break;
                }
            }
        }
        PairingFrame frame;
        frame.type = static_cast<PairingFrameType>(static_cast<std::uint8_t>(1 + rng() % 4));
        frame.version = (rng() % 32 == 0) ? static_cast<std::uint8_t>(rng() % 4)
                                          : static_cast<std::uint8_t>(1);
        frame.status = (rng() % 4 == 0)
                           ? PairingError::kNone
                           : static_cast<PairingError>(static_cast<std::uint8_t>(rng() % 250));
        frame.key_hash = (rng() % 3 == 0) ? rng() : server.key_hash();
        frame.nonce = (rng() % 3 == 0) ? rng()
                                       : (server.nonce() != 0 ? server.nonce() : client.nonce());
        frame.pair_id = static_cast<std::uint32_t>(rng());
        (void)server.HandleFrame(frame, static_cast<std::uint64_t>(step));
        (void)client.HandleFrame(frame, static_cast<std::uint64_t>(step));
        BENTO_CHECK(ValidState(server.state()));
        BENTO_CHECK(ValidState(client.state()));
        BENTO_CHECK(server.outbound_size() <= go2cpp::bento::kPairingMaxOutbound);
        BENTO_CHECK(client.outbound_size() <= go2cpp::bento::kPairingMaxOutbound);
        // 客户端只会在服务端回 ACK（服务端已 Active）之后 Active。
        if (client.state() == PairingState::kActive) {
            BENTO_CHECK_EQ(server.state(), PairingState::kActive);
        }
        PairingFrame drained;
        while (server.PopOutbound(&drained)) {
        }
        while (client.PopOutbound(&drained)) {
        }
    }
}

/// 场景 18：100 万次 32B 编解码往返 + 发送队列上限。
void TestCodecThroughputSanity() {
    constexpr std::size_t kIterations = 1000000;
    PairingFrame frame;
    frame.type = PairingFrameType::kConnect;
    frame.key_hash = 0x1122334455667788ULL;
    frame.nonce = 0x99aabbccddeeff00ULL;
    frame.pair_id = 0x12345678U;
    std::uint8_t buffer[kPairingFrameBytes];
    std::size_t matched = 0;
    for (std::size_t index = 0; index < kIterations; ++index) {
        frame.pair_id = static_cast<std::uint32_t>(index);
        EncodePairingFrame(frame, buffer);
        PairingFrame decoded;
        if (DecodePairingFrame(buffer, sizeof(buffer), &decoded) &&
            decoded.pair_id == frame.pair_id && decoded.key_hash == frame.key_hash &&
            decoded.nonce == frame.nonce) {
            ++matched;
        }
    }
    BENTO_CHECK_EQ(matched, kIterations);

    PairingSession client(Config(0x1ULL, 4000, 1));
    BENTO_CHECK_EQ(client.Connect("k", 0), PairingError::kNone);
    for (int step = 1; step <= 2000; ++step) {
        (void)client.Tick(static_cast<std::uint64_t>(step));
    }
    BENTO_CHECK_EQ(client.outbound_size(), go2cpp::bento::kPairingMaxOutbound);
    BENTO_CHECK(client.stats().queue_drops > 0);
    PairingFrame out;
    std::size_t popped = 0;
    while (client.PopOutbound(&out)) {
        ++popped;
        BENTO_CHECK_EQ(out.type, PairingFrameType::kConnect);
    }
    BENTO_CHECK_EQ(popped, go2cpp::bento::kPairingMaxOutbound);
}

/// 场景 19：枚举名字/边界 API 全覆盖（日志与指标依赖稳定名字）。
void TestNamesAndBoundaries() {
    const PairingState states[] = {PairingState::kIdle, PairingState::kPending,
                                   PairingState::kPaired, PairingState::kActive,
                                   PairingState::kClosed};
    for (const PairingState state : states) {
        BENTO_CHECK(std::string(go2cpp::bento::PairingStateName(state)) != "unknown");
    }
    const PairingRole roles[] = {PairingRole::kNone, PairingRole::kServer, PairingRole::kClient};
    for (const PairingRole role : roles) {
        BENTO_CHECK(std::string(go2cpp::bento::PairingRoleName(role)) != "unknown");
    }
    const PairingError errors[] = {
        PairingError::kNone,        PairingError::kKeyMismatch,
        PairingError::kPeerTimeout, PairingError::kAlreadyRegistered,
        PairingError::kNotRegistered, PairingError::kAlreadyPaired,
        PairingError::kInvalidState, PairingError::kInvalidArgument,
        PairingError::kProtocolVersion, PairingError::kMalformedFrame,
        PairingError::kRejected,    PairingError::kClosed};
    for (const PairingError error : errors) {
        BENTO_CHECK(std::string(go2cpp::bento::PairingErrorName(error)) != "unknown");
        BENTO_CHECK(go2cpp::bento::IsKnownPairingError(error));
    }
    const PairingFrameType types[] = {PairingFrameType::kRegister, PairingFrameType::kConnect,
                                      PairingFrameType::kResponse, PairingFrameType::kConfirm};
    for (const PairingFrameType type : types) {
        BENTO_CHECK(std::string(go2cpp::bento::PairingFrameTypeName(type)) != "unknown");
    }
    BENTO_CHECK(!go2cpp::bento::IsKnownPairingError(static_cast<PairingError>(12)));
    BENTO_CHECK(!go2cpp::bento::IsKnownPairingError(static_cast<PairingError>(255)));
    BENTO_CHECK_EQ(std::string(go2cpp::bento::PairingStateName(static_cast<PairingState>(9))),
                   std::string("unknown"));
    BENTO_CHECK_EQ(std::string(go2cpp::bento::PairingRoleName(static_cast<PairingRole>(9))),
                   std::string("unknown"));
    BENTO_CHECK_EQ(std::string(go2cpp::bento::PairingErrorName(static_cast<PairingError>(200))),
                   std::string("unknown"));
    BENTO_CHECK_EQ(std::string(go2cpp::bento::PairingFrameTypeName(
                       static_cast<PairingFrameType>(9))),
                   std::string("unknown"));

    // 空指针与非法长度必须安全返回，不写输出。
    PairingSession idle(Config());
    BENTO_CHECK(!idle.Close());                // Idle 关闭返回 false
    BENTO_CHECK(!idle.Tick(0));                // Idle 不因 Tick 迁移
    BENTO_CHECK(!idle.PopOutbound(nullptr));   // 空指针安全
    EncodePairingFrame(PairingFrame{}, nullptr);  // 空输出缓冲安全
    BENTO_CHECK(!DecodePairingFrame(nullptr, 32, nullptr));

    // 允许尾部有多余字节；长度不足必须拒绝。
    std::uint8_t wide[64] = {};
    PairingFrame frame;
    frame.type = PairingFrameType::kConnect;
    EncodePairingFrame(frame, wide);
    PairingFrame decoded;
    BENTO_CHECK(DecodePairingFrame(wide, sizeof(wide), &decoded));
    BENTO_CHECK_EQ(decoded.type, PairingFrameType::kConnect);
    BENTO_CHECK(!DecodePairingFrame(wide, 0, &decoded));

    // Reset 保留统计计数器。
    PairingSession reused(Config());
    BENTO_CHECK_EQ(reused.Register("k", 0), PairingError::kNone);
    reused.Reset();
    BENTO_CHECK_EQ(reused.stats().registers, 1U);
    BENTO_CHECK_EQ(reused.state(), PairingState::kIdle);
    BENTO_CHECK_EQ(reused.deadline_ms(), 0U);
}

/// 场景 20：边缘分支（timeout 钳位、Pending 收 ACK/CONFIRM、Active 收错配帧）。
void TestEdgeBranches() {
    // timeout_ms=0 钳到 1ms，保证 Tick 有明确截止时间。
    PairingConfig zero_timeout;
    zero_timeout.timeout_ms = 0;
    zero_timeout.nonce = 0xabcdefULL;
    PairingSession clamped(zero_timeout);
    BENTO_CHECK_EQ(clamped.config().timeout_ms, 1U);
    BENTO_CHECK_EQ(clamped.Connect("k", 100), PairingError::kNone);
    BENTO_CHECK_EQ(clamped.deadline_ms(), 101U);
    BENTO_CHECK(clamped.Tick(101));
    BENTO_CHECK_EQ(clamped.state(), PairingState::kClosed);

    // 客户端 Pending 收到 ACK（服务端不可能此时发）→ kInvalidState。
    PairingSession client(Config(0x1234ULL, 4000, 500));
    BENTO_CHECK_EQ(client.Connect("k", 0), PairingError::kNone);
    (void)Drain(client);
    PairingFrame ack;
    ack.type = PairingFrameType::kConfirm;
    ack.key_hash = client.key_hash();
    ack.nonce = client.nonce();
    ack.pair_id = 1;
    BENTO_CHECK_EQ(client.HandleFrame(ack, 0), PairingError::kInvalidState);
    BENTO_CHECK_EQ(client.state(), PairingState::kPending);

    // 服务端 Pending 收到 CONFIRM → kInvalidState。
    PairingSession server(Config());
    BENTO_CHECK_EQ(server.Register("k", 0), PairingError::kNone);
    BENTO_CHECK_EQ(server.HandleFrame(ack, 0), PairingError::kInvalidState);
    BENTO_CHECK_EQ(server.state(), PairingState::kPending);

    // 服务端 Active 后收到 nonce 不匹配的 CONFIRM → kKeyMismatch，状态不变。
    PairingSession active_server(Config());
    PairingSession active_client(Config(0x2233ULL));
    BENTO_CHECK(PairUntilActive(active_server, active_client, 0));
    PairingFrame bad_confirm;
    bad_confirm.type = PairingFrameType::kConfirm;
    bad_confirm.key_hash = active_server.key_hash();
    bad_confirm.nonce = active_server.nonce() ^ 1ULL;
    bad_confirm.pair_id = active_server.pair_id();
    BENTO_CHECK_EQ(active_server.HandleFrame(bad_confirm, 0), PairingError::kKeyMismatch);
    BENTO_CHECK_EQ(active_server.state(), PairingState::kActive);

    // 客户端 Active 后收到重复 RESPONSE → 忽略且不入队。
    PairingFrame response;
    response.type = PairingFrameType::kResponse;
    response.key_hash = active_client.key_hash();
    response.nonce = active_client.nonce();
    response.pair_id = active_client.pair_id();
    response.status = PairingError::kNone;
    BENTO_CHECK_EQ(active_client.HandleFrame(response, 0), PairingError::kNone);
    BENTO_CHECK_EQ(active_client.outbound_size(), 0U);
    BENTO_CHECK_EQ(active_client.state(), PairingState::kActive);
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
    TestActivateIdempotent();
    TestResponseKeyHashGuard();
    TestLossyLinkConvergence();
    TestThousandPairings();
    TestRandomFrameFuzz();
    TestCodecThroughputSanity();
    TestNamesAndBoundaries();
    TestEdgeBranches();
    return bento_test::Finish("test_pairing");
}
