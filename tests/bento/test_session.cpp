/**
 * @file test_session.cpp
 * @brief MOD-39 会话编排测试：端点解析、配置校验、连接→配对→通道→活跃→
 *        数据→优雅关闭、配对超时/拒绝、连接超时/失败、心跳、控制权仲裁、
 *        关闭窗口超时、写失败、协议错误与非法操作。
 * @author go2cpp contributors
 * @date 2026-10-28
 * @copyright MIT License
 */

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "go2cpp/bento/app/session.hpp"
#include "go2cpp/bento/net/frame.hpp"
#include "go2cpp/bento/net/memory_transport.hpp"
#include "go2cpp/bento/net/protocol.hpp"
#include "test_util.hpp"

namespace {

using go2cpp::bento::ControlChangeReason;
using go2cpp::bento::ControlMode;
using go2cpp::bento::app::IsSessionTransitionAllowed;
using go2cpp::bento::app::ParseEndpoint;
using go2cpp::bento::app::Session;
using go2cpp::bento::app::SessionConfig;
using go2cpp::bento::app::SessionError;
using go2cpp::bento::app::SessionState;
using go2cpp::bento::net::Code;
using go2cpp::bento::net::Endpoint;
using go2cpp::bento::net::ITransport;
using go2cpp::bento::net::MemoryTransport;
using go2cpp::bento::net::Result;

/// 可控逻辑时钟：两个会话共享同一实例，模拟同一时刻。
struct FakeClock {
    std::uint64_t now_ms = 1000;
};

/// 把 unique_ptr 端点包装成可拷贝的工厂（std::function 要求目标可拷贝）。
std::function<std::unique_ptr<ITransport>()> Factory(std::unique_ptr<ITransport> transport) {
    auto holder = std::make_shared<std::unique_ptr<ITransport>>(std::move(transport));
    return [holder]() -> std::unique_ptr<ITransport> { return std::move(*holder); };
}

SessionConfig BaseConfig(FakeClock& clock) {
    SessionConfig config;
    config.pairing_key = "invite-key";
    config.connect_timeout_ms = 1000;
    config.io_timeout_ms = 10;
    config.close_timeout_ms = 500;
    config.heartbeat_interval_ms = 1000;
    config.heartbeat_timeout_ms = 3000;
    config.pairing_timeout_ms = 5000;
    config.pairing_retransmit_ms = 500;
    config.pairing_nonce = 0x1234;
    config.now = [&clock]() { return clock.now_ms; };
    return config;
}

struct PairConfigs {
    SessionConfig client;
    SessionConfig server;
};

PairConfigs MakePairConfigs(FakeClock& clock, MemoryTransport::Pair pair) {
    PairConfigs configs;
    configs.client = BaseConfig(clock);
    configs.client.endpoint = "127.0.0.1:9000";
    configs.client.local_peer_id = 1;
    configs.client.transport_factory =
        Factory(std::unique_ptr<ITransport>(std::move(pair.first)));

    configs.server = BaseConfig(clock);
    configs.server.server = true;
    configs.server.local_peer_id = 2;
    configs.server.transport_factory =
        Factory(std::unique_ptr<ITransport>(std::move(pair.second)));
    return configs;
}

/// 记录状态序列，并检查每一次迁移都合法。
struct StateTrace {
    std::vector<SessionState> states;

    void Attach(Session& session) {
        states.push_back(session.state());
        session.on_state = [this](SessionState from, SessionState to) {
            BENTO_CHECK(IsSessionTransitionAllowed(from, to));
            states.push_back(to);
        };
    }

    bool Contains(SessionState state) const {
        return std::find(states.begin(), states.end(), state) != states.end();
    }
};

struct DataSink {
    std::vector<std::string> messages;

    void Attach(Session& session) {
        session.on_data = [this](std::uint32_t, const std::uint8_t* data, std::size_t size) {
            if (size == 0) {
                messages.emplace_back();
            } else {
                messages.emplace_back(reinterpret_cast<const char*>(data), size);
            }
        };
    }
};

struct ErrorTrace {
    std::vector<SessionError> errors;

    void Attach(Session& session) {
        session.on_error = [this](SessionError error, const std::string&) {
            errors.push_back(error);
        };
    }

    bool Contains(SessionError error) const {
        return std::find(errors.begin(), errors.end(), error) != errors.end();
    }
};

struct ControlTrace {
    std::vector<go2cpp::bento::ControlChange> changes;

    void Attach(Session& session) {
        session.on_control = [this](const go2cpp::bento::ControlChange& change) {
            changes.push_back(change);
        };
    }
};

template <typename Predicate>
bool PumpBoth(Session& first, Session& second, Predicate done, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(std::max(timeout_ms, 0));
    while (std::chrono::steady_clock::now() < deadline) {
        first.Poll(1);
        second.Poll(1);
        if (done()) {
            return true;
        }
    }
    return done();
}

bool Activate(Session& client, Session& server) {
    if (client.Start() != SessionError::kNone || server.Start() != SessionError::kNone) {
        return false;
    }
    return PumpBoth(client, server,
                    [&] {
                        return client.state() == SessionState::kActive &&
                               server.state() == SessionState::kActive;
                    },
                    5000);
}

/// 可编程传输桩：用于连接失败/写失败/协议错误等无法用 MemoryTransport 覆盖的路径。
class StubTransport final : public ITransport {
   public:
    explicit StubTransport(Code connect_code) : m_connect_code(connect_code) {}

    void SetWriteCode(Code code) { m_write_code = code; }
    void QueueRead(std::vector<std::uint8_t> bytes) { m_chunks.push_back(std::move(bytes)); }

    Result Connect(const Endpoint&, std::chrono::milliseconds) override {
        if (m_connect_code == Code::kOk) {
            return Result{Code::kOk, 0, 0};
        }
        const int error = (m_connect_code == Code::kTimeout) ? ETIMEDOUT : EACCES;
        return Result{m_connect_code, error, 0};
    }

    Result Read(std::uint8_t* buffer, std::size_t capacity,
                std::chrono::milliseconds) override {
        if (m_closed) {
            return Result{Code::kClosed, ENOTCONN, 0};
        }
        if (capacity == 0) {
            return Result{Code::kOk, 0, 0};
        }
        while (!m_chunks.empty() && m_chunks.front().empty()) {
            m_chunks.pop_front();
        }
        if (m_chunks.empty()) {
            return Result{Code::kTimeout, ETIMEDOUT, 0};
        }
        std::vector<std::uint8_t>& chunk = m_chunks.front();
        const std::size_t count = std::min(capacity, chunk.size());
        std::copy(chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(count), buffer);
        chunk.erase(chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(count));
        return Result{Code::kOk, 0, count};
    }

    Result Write(const std::uint8_t* data, std::size_t size,
                 std::chrono::milliseconds) override {
        if (m_closed) {
            return Result{Code::kClosed, ENOTCONN, 0};
        }
        if (m_write_code != Code::kOk) {
            return Result{m_write_code, EPIPE, 0};
        }
        m_written.insert(m_written.end(), data, data + size);
        return Result{Code::kOk, 0, size};
    }

    Result ShutdownWrite() override { return Result{Code::kOk, 0, 0}; }
    Result Close() override {
        m_closed = true;
        return Result{Code::kOk, 0, 0};
    }

    const std::vector<std::uint8_t>& written() const { return m_written; }

   private:
    Code m_connect_code;
    Code m_write_code = Code::kOk;
    bool m_closed = false;
    std::deque<std::vector<std::uint8_t>> m_chunks;
    std::vector<std::uint8_t> m_written;
};

SessionConfig StubConfig(FakeClock& clock, std::unique_ptr<ITransport> transport) {
    SessionConfig config = BaseConfig(clock);
    config.endpoint = "127.0.0.1:9100";
    config.transport_factory = Factory(std::move(transport));
    return config;
}

/// 会话层帧编码器（测试侧）：与会话实现共用公开的 magic/version/类型段。
std::vector<std::uint8_t> EncodeSessionFrame(go2cpp::bento::app::SessionFrameType type,
                                             std::vector<std::uint8_t> payload) {
    go2cpp::bento::net::Frame frame;
    frame.type = static_cast<std::uint16_t>(type);
    frame.payload = std::move(payload);
    return go2cpp::bento::net::Encode(
        frame, go2cpp::bento::net::FrameFormat{go2cpp::bento::app::kSessionFrameMagic,
                                               go2cpp::bento::app::kSessionFrameVersion});
}

/**
 * @brief 转发传输：可切换写失败/部分写，也可绕过 Session 直接注入原始字节。
 *
 * @details 包住 MemoryTransport 的一端：正常路径完全转发；`kTimeout` 用于
 *          制造「发送队列积压」；`kPartialThenTimeout` 让一次 WriteFull
 *          只写出 prefix，然后超时（覆盖 SESS-01 的部分写恢复路径）；
 *          `InjectWrite` 模拟一个恶意/故障对端直接往会话里塞帧。
 */
class TapTransport final : public ITransport {
   public:
    enum class WriteMode { kForward, kTimeout, kPartialThenTimeout };

    explicit TapTransport(std::unique_ptr<ITransport> inner) : m_inner(std::move(inner)) {}

    void SetWriteMode(WriteMode mode, std::size_t partial_bytes = 0) {
        m_mode = mode;
        m_partial_bytes = partial_bytes;
        m_partial_done = false;
    }

    /// 绕过 Session 直接向对端写原始字节。
    bool InjectWrite(const std::vector<std::uint8_t>& bytes) {
        const Result result =
            m_inner->WriteFull(bytes.data(), bytes.size(), std::chrono::milliseconds(100));
        return result.ok();
    }

    Result Connect(const Endpoint& endpoint, std::chrono::milliseconds timeout) override {
        return m_inner->Connect(endpoint, timeout);
    }
    Result Read(std::uint8_t* buffer, std::size_t capacity,
                std::chrono::milliseconds timeout) override {
        return m_inner->Read(buffer, capacity, timeout);
    }
    Result Write(const std::uint8_t* data, std::size_t size,
                 std::chrono::milliseconds timeout) override {
        if (m_mode == WriteMode::kForward) {
            return m_inner->Write(data, size, timeout);
        }
        if (m_mode == WriteMode::kTimeout) {
            return Result{Code::kTimeout, ETIMEDOUT, 0};
        }
        if (!m_partial_done) {
            m_partial_done = true;
            const std::size_t take = std::min(m_partial_bytes, size);
            if (take == 0) {
                return Result{Code::kTimeout, ETIMEDOUT, 0};
            }
            const Result written = m_inner->WriteFull(data, take, timeout);
            if (!written.ok()) {
                return written;
            }
            return Result{Code::kOk, 0, take};
        }
        return Result{Code::kTimeout, ETIMEDOUT, 0};
    }
    Result ShutdownWrite() override { return m_inner->ShutdownWrite(); }
    Result Close() override { return m_inner->Close(); }

   private:
    std::unique_ptr<ITransport> m_inner;
    WriteMode m_mode = WriteMode::kForward;
    std::size_t m_partial_bytes = 0;
    bool m_partial_done = false;
};

/// 用 TapTransport 包住 server 端端点；返回工厂并把裸指针交给调用方。
std::function<std::unique_ptr<ITransport>()> TapFactory(std::unique_ptr<ITransport> inner,
                                                        TapTransport** out) {
    auto holder = std::make_unique<TapTransport>(std::move(inner));
    *out = holder.get();
    return Factory(std::unique_ptr<ITransport>(std::move(holder)));
}

/// 与 MakePairConfigs 相同，但 server 端经过 TapTransport。
PairConfigs MakeTapPairConfigs(FakeClock& clock, MemoryTransport::Pair pair,
                               TapTransport** server_tap) {
    PairConfigs configs;
    configs.client = BaseConfig(clock);
    configs.client.endpoint = "127.0.0.1:9000";
    configs.client.local_peer_id = 1;
    configs.client.transport_factory =
        Factory(std::unique_ptr<ITransport>(std::move(pair.first)));

    configs.server = BaseConfig(clock);
    configs.server.server = true;
    configs.server.local_peer_id = 2;
    configs.server.transport_factory = TapFactory(std::move(pair.second), server_tap);
    return configs;
}

/// 场景 1：端点语法解析的合法/非法输入。
void TestEndpointParsing() {
    Endpoint endpoint;
    BENTO_CHECK(ParseEndpoint("127.0.0.1:8080", &endpoint));
    BENTO_CHECK(endpoint.host == "127.0.0.1");
    BENTO_CHECK_EQ(endpoint.port, 8080);

    BENTO_CHECK(ParseEndpoint("  example.com:80  ", &endpoint));
    BENTO_CHECK(endpoint.host == "example.com");
    BENTO_CHECK_EQ(endpoint.port, 80);

    BENTO_CHECK(ParseEndpoint("[::1]:443", &endpoint));
    BENTO_CHECK(endpoint.host == "::1");
    BENTO_CHECK_EQ(endpoint.port, 443);

    BENTO_CHECK(ParseEndpoint("host:65535", &endpoint));
    BENTO_CHECK_EQ(endpoint.port, 65535);

    for (const char* invalid : {"", "localhost", ":8080", "host:", "host:0", "host:65536",
                                "host:abc", "::1:8080", "[::1]", "[::1]443", "[::1]:"}) {
        Endpoint unused;
        BENTO_CHECK(!ParseEndpoint(invalid, &unused));
    }
    BENTO_CHECK(!ParseEndpoint("host:80", nullptr));
}

/// 场景 2：Start 的配置校验（失败保持 Idle，便于修正后重试）。
void TestConfigValidation() {
    FakeClock clock;
    {
        SessionConfig config = BaseConfig(clock);
        config.endpoint = "127.0.0.1:1";
        config.pairing_key.clear();
        Session session(config);
        BENTO_CHECK(session.Start() == SessionError::kInvalidArgument);
        BENTO_CHECK(session.state() == SessionState::kIdle);
        BENTO_CHECK(session.last_error() == SessionError::kInvalidArgument);
        BENTO_CHECK(!session.last_error_message().empty());
    }
    {
        SessionConfig config = BaseConfig(clock);
        config.pairing_key = "k";
        Session session(config);
        BENTO_CHECK(session.Start() == SessionError::kInvalidEndpoint);
        BENTO_CHECK(session.state() == SessionState::kIdle);
    }
    {
        SessionConfig config = BaseConfig(clock);
        config.endpoint = "127.0.0.1:1";
        config.channel_id = 0;
        Session session(config);
        BENTO_CHECK(session.Start() == SessionError::kInvalidArgument);
        BENTO_CHECK(session.state() == SessionState::kIdle);
    }
    {
        SessionConfig config = BaseConfig(clock);
        config.endpoint = "127.0.0.1:1";
        config.local_peer_id = 0;
        Session session(config);
        BENTO_CHECK(session.Start() == SessionError::kInvalidArgument);
        BENTO_CHECK(session.state() == SessionState::kIdle);
    }
    {
        SessionConfig config = BaseConfig(clock);
        config.server = true;
        config.pairing_key = "k";
        Session session(config);
        BENTO_CHECK(session.Start() == SessionError::kTransportUnavailable);
        BENTO_CHECK(session.state() == SessionState::kIdle);
    }
    {
        SessionConfig config = BaseConfig(clock);
        config.endpoint = "127.0.0.1:1";
        config.requested_control_mode = ControlMode::kNone;
        Session session(config);
        BENTO_CHECK(session.Start() == SessionError::kInvalidArgument);
        BENTO_CHECK(session.state() == SessionState::kIdle);
    }
    {
        // 合法配置启动后再次 Start -> kInvalidState，且不破坏连接中的状态。
        auto pair = MemoryTransport::CreatePair();
        auto configs = MakePairConfigs(clock, std::move(pair));
        Session client(configs.client);
        BENTO_CHECK(client.Start() == SessionError::kNone);
        BENTO_CHECK(client.state() == SessionState::kConnecting);
        BENTO_CHECK(client.Start() == SessionError::kInvalidState);
        BENTO_CHECK(client.state() == SessionState::kConnecting);
        BENTO_CHECK(client.last_error() == SessionError::kInvalidState);
        client.Close();
        BENTO_CHECK(client.state() == SessionState::kClosing);
    }
}

/// 场景 3：完整正常路径：连接 → 配对 → 通道 → 活跃 → 双向数据 → 优雅关闭。
void TestHappyPath() {
    auto pair = MemoryTransport::CreatePair();
    FakeClock clock;
    auto configs = MakePairConfigs(clock, std::move(pair));
    Session client(configs.client);
    Session server(configs.server);

    StateTrace client_states;
    StateTrace server_states;
    ErrorTrace client_errors;
    DataSink client_data;
    DataSink server_data;
    client_states.Attach(client);
    server_states.Attach(server);
    client_errors.Attach(client);
    server_data.Attach(server);
    client_data.Attach(client);

    BENTO_CHECK_EQ(client.state(), SessionState::kIdle);
    BENTO_CHECK_EQ(server.state(), SessionState::kIdle);
    BENTO_CHECK(client.Start() == SessionError::kNone);
    BENTO_CHECK(server.Start() == SessionError::kNone);
    BENTO_CHECK_EQ(client.state(), SessionState::kConnecting);
    BENTO_CHECK_EQ(server.state(), SessionState::kConnecting);
    BENTO_CHECK(client.endpoint().host == "127.0.0.1");
    BENTO_CHECK_EQ(client.endpoint().port, 9000);

    BENTO_CHECK(PumpBoth(client, server,
                         [&] {
                             return client.state() == SessionState::kActive &&
                                    server.state() == SessionState::kActive;
                         },
                         5000));
    BENTO_CHECK(client_states.Contains(SessionState::kConnecting));
    BENTO_CHECK(client_states.Contains(SessionState::kPaired));
    BENTO_CHECK(client_states.Contains(SessionState::kActive));
    BENTO_CHECK(server_states.Contains(SessionState::kPaired));
    BENTO_CHECK(server_states.Contains(SessionState::kActive));
    BENTO_CHECK_EQ(client.remote_peer_id(), 2U);
    BENTO_CHECK_EQ(server.remote_peer_id(), 1U);
    BENTO_CHECK_EQ(client.local_peer_id(), 1U);
    BENTO_CHECK(client.channel().open);
    BENTO_CHECK_EQ(client.channel().id, 1U);
    BENTO_CHECK(server.channel().open);
    BENTO_CHECK(client.channel().opened_at_ms > 0);
    BENTO_CHECK(client_errors.errors.empty());

    const std::vector<std::uint32_t> participants = client.control().participants();
    BENTO_CHECK(std::find(participants.begin(), participants.end(), 1U) != participants.end());
    BENTO_CHECK(std::find(participants.begin(), participants.end(), 2U) != participants.end());

    // 双向数据。
    BENTO_CHECK(client.Send("hello") == SessionError::kNone);
    BENTO_CHECK(PumpBoth(client, server, [&] { return !server_data.messages.empty(); }, 3000));
    BENTO_CHECK_EQ(server_data.messages.size(), std::size_t{1});
    if (!server_data.messages.empty()) {
        BENTO_CHECK(server_data.messages[0] == "hello");
    }
    BENTO_CHECK_EQ(server.channel().rx_bytes, std::uint64_t{5});

    BENTO_CHECK(server.Send("world!") == SessionError::kNone);
    BENTO_CHECK(PumpBoth(client, server, [&] { return !client_data.messages.empty(); }, 3000));
    BENTO_CHECK(client_data.messages.size() == std::size_t{1});
    if (!client_data.messages.empty()) {
        BENTO_CHECK(client_data.messages[0] == "world!");
    }
    BENTO_CHECK(client.stats().data_frames_sent >= 1);
    BENTO_CHECK(server.stats().data_frames_received >= 1);
    BENTO_CHECK(client.stats().frames_sent > 0);
    BENTO_CHECK(client.stats().bytes_sent > 0);

    // 关闭：client 主动，server 对端关闭，双方最终 Closed。
    client.Close();
    BENTO_CHECK_EQ(client.state(), SessionState::kClosing);
    BENTO_CHECK(PumpBoth(client, server,
                         [&] {
                             return client.state() == SessionState::kClosed &&
                                    server.state() == SessionState::kClosed;
                         },
                         5000));
    BENTO_CHECK(client_states.Contains(SessionState::kClosing));
    BENTO_CHECK(client_states.Contains(SessionState::kClosed));
    BENTO_CHECK(server_states.Contains(SessionState::kClosed));
    BENTO_CHECK(!client.Poll(0));
    BENTO_CHECK(!server.Poll(0));
    BENTO_CHECK(client.Send("late") == SessionError::kInvalidState);
}

/// T-03：WaitFor 阻塞等待正常路径（server 线程驱动）与超时返回 false。
void TestWaitFor() {
    {
        auto pair = MemoryTransport::CreatePair();
        FakeClock clock;
        auto configs = MakePairConfigs(clock, std::move(pair));
        Session client(configs.client);
        Session server(configs.server);
        BENTO_CHECK(client.Start() == SessionError::kNone);
        BENTO_CHECK(server.Start() == SessionError::kNone);
        std::atomic<bool> server_running{true};
        std::thread server_thread([&server, &server_running]() {
            while (server_running.load()) {
                server.Poll(1);
            }
        });
        const bool reached = client.WaitFor(SessionState::kActive, 2000);
        server_running.store(false);
        server_thread.join();
        BENTO_CHECK(reached);
        BENTO_CHECK_EQ(client.state(), SessionState::kActive);
        BENTO_CHECK(PumpBoth(client, server,
                             [&] {
                                 return client.state() == SessionState::kActive &&
                                        server.state() == SessionState::kActive;
                             },
                             2000));
        BENTO_CHECK_EQ(server.remote_peer_id(), 1U);
        BENTO_CHECK_EQ(server.channel().id, 1U);
        BENTO_CHECK(server.channel().open);
    }
    {
        auto pair = MemoryTransport::CreatePair();
        FakeClock clock;
        auto configs = MakePairConfigs(clock, std::move(pair));
        Session client(configs.client);
        BENTO_CHECK(client.Start() == SessionError::kNone);
        BENTO_CHECK(!client.WaitFor(SessionState::kActive, 120));
        BENTO_CHECK_EQ(client.state(), SessionState::kConnecting);
    }
}

/// T-03：auto_accept_pairing=false 时显式 AcceptPairing 的正常/非法状态。
void TestManualAcceptPairing() {
    auto pair = MemoryTransport::CreatePair();
    FakeClock clock;
    auto configs = MakePairConfigs(clock, std::move(pair));
    configs.server.auto_accept_pairing = false;
    Session client(configs.client);
    Session server(configs.server);

    BENTO_CHECK(server.Start() == SessionError::kNone);
    BENTO_CHECK_EQ(server.AcceptPairing(), SessionError::kInvalidState);
    BENTO_CHECK(client.Start() == SessionError::kNone);

    SessionError accepted = SessionError::kInvalidState;
    const bool got_pending = PumpBoth(
        client, server,
        [&] {
            accepted = server.AcceptPairing();
            return accepted == SessionError::kNone;
        },
        3000);
    BENTO_CHECK(got_pending);
    BENTO_CHECK_EQ(accepted, SessionError::kNone);
    BENTO_CHECK(client.state() != SessionState::kActive);

    BENTO_CHECK(PumpBoth(client, server,
                         [&] {
                             return client.state() == SessionState::kActive &&
                                    server.state() == SessionState::kActive;
                         },
                         3000));
    BENTO_CHECK_EQ(server.remote_peer_id(), 1U);
    BENTO_CHECK_EQ(server.AcceptPairing(), SessionError::kInvalidState);
}

/// 场景 4：配对超时（对端不响应），client 进入 Closed + kPairingTimeout。
void TestPairingTimeout() {
    auto pair = MemoryTransport::CreatePair();
    FakeClock clock;
    auto configs = MakePairConfigs(clock, std::move(pair));
    Session client(configs.client);
    StateTrace states;
    ErrorTrace errors;
    states.Attach(client);
    errors.Attach(client);

    BENTO_CHECK(client.Start() == SessionError::kNone);
    BENTO_CHECK_EQ(client.state(), SessionState::kConnecting);
    clock.now_ms += configs.client.pairing_timeout_ms + 1;
    BENTO_CHECK(!client.Poll(0));
    BENTO_CHECK_EQ(client.state(), SessionState::kClosed);
    BENTO_CHECK_EQ(client.last_error(), SessionError::kPairingTimeout);
    BENTO_CHECK(errors.Contains(SessionError::kPairingTimeout));
    BENTO_CHECK(states.Contains(SessionState::kClosing));
    BENTO_CHECK(states.Contains(SessionState::kClosed));
}

/// 场景 5：配对失败（密钥不匹配）：双方都给出明确失败并关闭。
void TestPairingRejected() {
    auto pair = MemoryTransport::CreatePair();
    FakeClock clock;
    auto configs = MakePairConfigs(clock, std::move(pair));
    configs.client.pairing_key = "wrong-key";
    configs.server.pairing_key = "right-key";
    Session client(configs.client);
    Session server(configs.server);
    ErrorTrace client_errors;
    ErrorTrace server_errors;
    client_errors.Attach(client);
    server_errors.Attach(server);

    BENTO_CHECK(client.Start() == SessionError::kNone);
    BENTO_CHECK(server.Start() == SessionError::kNone);
    BENTO_CHECK(PumpBoth(client, server,
                         [&] {
                             return client.state() == SessionState::kClosed &&
                                    server.state() == SessionState::kClosed;
                         },
                         5000));
    BENTO_CHECK_EQ(server.last_error(), SessionError::kPairingRejected);
    BENTO_CHECK(server_errors.Contains(SessionError::kPairingRejected));
    // client 可能先读到 RESPONSE(kKeyMismatch)，也可能先看到 EOF。
    BENTO_CHECK(client.last_error() == SessionError::kPairingRejected ||
                client.last_error() == SessionError::kTransportClosed);
    BENTO_CHECK(client_errors.Contains(SessionError::kPairingRejected) ||
                client_errors.Contains(SessionError::kTransportClosed));
}

/// 场景 6：Connect 超时 -> kConnectTimeout。
void TestConnectTimeout() {
    FakeClock clock;
    auto transport = std::make_unique<StubTransport>(Code::kTimeout);
    Session session(StubConfig(clock, std::move(transport)));
    StateTrace states;
    ErrorTrace errors;
    states.Attach(session);
    errors.Attach(session);

    BENTO_CHECK(session.Start() == SessionError::kConnectTimeout);
    BENTO_CHECK_EQ(session.state(), SessionState::kClosed);
    BENTO_CHECK(errors.Contains(SessionError::kConnectTimeout));
    BENTO_CHECK(states.Contains(SessionState::kConnecting));
    BENTO_CHECK(states.Contains(SessionState::kClosed));
}

/// 场景 7：Connect 其它失败 -> kConnectFailed。
void TestConnectFailure() {
    FakeClock clock;
    auto transport = std::make_unique<StubTransport>(Code::kError);
    Session session(StubConfig(clock, std::move(transport)));
    BENTO_CHECK(session.Start() == SessionError::kConnectFailed);
    BENTO_CHECK_EQ(session.state(), SessionState::kClosed);
    BENTO_CHECK(session.last_error_message().find("permission denied") != std::string::npos ||
                session.last_error_message().find("Permission denied") != std::string::npos);
}

/// 场景 8：心跳发送与接收；超时后进入 Closed + kHeartbeatTimeout。
void TestHeartbeatAndTimeout() {
    {
        auto pair = MemoryTransport::CreatePair();
        FakeClock clock;
        auto configs = MakePairConfigs(clock, std::move(pair));
        Session client(configs.client);
        Session server(configs.server);
        BENTO_CHECK(Activate(client, server));
        const std::uint64_t active_at = client.channel().opened_at_ms;
        clock.now_ms = active_at + configs.client.heartbeat_interval_ms;
        BENTO_CHECK(PumpBoth(client, server,
                             [&] { return server.stats().heartbeats_received >= 1; }, 3000));
        BENTO_CHECK(client.stats().heartbeats_sent >= 1);
        BENTO_CHECK(server.stats().heartbeats_received >= 1);
        BENTO_CHECK_EQ(client.state(), SessionState::kActive);
    }
    {
        auto pair = MemoryTransport::CreatePair();
        FakeClock clock;
        auto configs = MakePairConfigs(clock, std::move(pair));
        Session client(configs.client);
        Session server(configs.server);
        BENTO_CHECK(Activate(client, server));
        const std::uint64_t active_at = client.channel().opened_at_ms;
        clock.now_ms = active_at + configs.client.heartbeat_timeout_ms + 1;
        BENTO_CHECK(!client.Poll(0));
        BENTO_CHECK_EQ(client.state(), SessionState::kClosed);
        BENTO_CHECK_EQ(client.last_error(), SessionError::kHeartbeatTimeout);
        BENTO_CHECK_EQ(server.state(), SessionState::kActive);
    }
}

/// 场景 9：控制权服务端仲裁：client 请求成功并双端一致；server 后请求被拒。
void TestControlArbitration() {
    auto pair = MemoryTransport::CreatePair();
    FakeClock clock;
    auto configs = MakePairConfigs(clock, std::move(pair));
    Session client(configs.client);
    Session server(configs.server);
    ControlTrace client_control;
    ControlTrace server_control;
    client_control.Attach(client);
    server_control.Attach(server);
    BENTO_CHECK(Activate(client, server));

    BENTO_CHECK(client.RequestControl() == SessionError::kNone);
    BENTO_CHECK(PumpBoth(client, server,
                         [&] {
                             return client.control().controller() == 1U &&
                                    server.control().controller() == 1U;
                         },
                         3000));
    BENTO_CHECK(client.control().is_controller(1));
    BENTO_CHECK(server.control().is_controller(1));
    BENTO_CHECK(!client_control.changes.empty());
    BENTO_CHECK(!server_control.changes.empty());
    BENTO_CHECK_EQ(client.stats().control_requests, std::uint64_t{1});

    // FirstCome：server 的本地控制器已是 client，后请求被拒。
    BENTO_CHECK(server.RequestControl() == SessionError::kControlDenied);
    BENTO_CHECK_EQ(server.control().controller(), 1U);
    BENTO_CHECK_EQ(client.control().controller(), 1U);
    BENTO_CHECK(server.control().mode(1) == ControlMode::kInteract);
}

/// 场景 10：优雅关闭窗口超时（对端不响应 kClose）。
void TestCloseTimeout() {
    auto pair = MemoryTransport::CreatePair();
    FakeClock clock;
    auto configs = MakePairConfigs(clock, std::move(pair));
    Session client(configs.client);
    Session server(configs.server);
    BENTO_CHECK(Activate(client, server));
    client.Close();
    BENTO_CHECK_EQ(client.state(), SessionState::kClosing);
    client.Poll(1);
    clock.now_ms += configs.client.close_timeout_ms + 1;
    client.Poll(1);
    BENTO_CHECK_EQ(client.state(), SessionState::kClosed);
    BENTO_CHECK_EQ(client.last_error(), SessionError::kNone);
}

/// 场景 11：写失败 -> kWriteFailed，Start 直接返回错误。
void TestWriteFailed() {
    FakeClock clock;
    auto transport = std::make_unique<StubTransport>(Code::kOk);
    transport->SetWriteCode(Code::kError);
    StubTransport* raw = transport.get();
    Session session(StubConfig(clock, std::move(transport)));
    ErrorTrace errors;
    errors.Attach(session);
    BENTO_CHECK(session.Start() == SessionError::kWriteFailed);
    BENTO_CHECK_EQ(session.state(), SessionState::kClosed);
    BENTO_CHECK(errors.Contains(SessionError::kWriteFailed));
    BENTO_CHECK(raw->written().empty());
}

/// 场景 12：非法帧（magic 错误）-> kProtocolError。
void TestProtocolError() {
    FakeClock clock;
    auto transport = std::make_unique<StubTransport>(Code::kOk);
    transport->QueueRead(std::vector<std::uint8_t>(20, 0xab));
    Session session(StubConfig(clock, std::move(transport)));
    StateTrace states;
    states.Attach(session);
    BENTO_CHECK(session.Start() == SessionError::kNone);
    BENTO_CHECK(!session.Poll(0));
    BENTO_CHECK_EQ(session.state(), SessionState::kClosed);
    BENTO_CHECK_EQ(session.last_error(), SessionError::kProtocolError);
    BENTO_CHECK(session.stats().protocol_errors >= 1);
    BENTO_CHECK(states.Contains(SessionState::kClosed));
}

/// 场景 13：非法操作与边界（未激活发送、错误通道、超大消息、关闭后 API）。
void TestInvalidOperations() {
    auto pair = MemoryTransport::CreatePair();
    FakeClock clock;
    auto configs = MakePairConfigs(clock, std::move(pair));
    Session client(configs.client);
    Session server(configs.server);

    // 连接/配对阶段不允许发送或请求控制权。
    BENTO_CHECK(client.Start() == SessionError::kNone);
    BENTO_CHECK(server.Start() == SessionError::kNone);
    BENTO_CHECK(client.Send("too-early") == SessionError::kInvalidState);
    BENTO_CHECK(client.RequestControl() == SessionError::kInvalidState);
    BENTO_CHECK(client.AcceptPairing() == SessionError::kInvalidState);
    BENTO_CHECK(client.Send(1, nullptr, 0) == SessionError::kInvalidState);

    BENTO_CHECK(PumpBoth(client, server,
                         [&] {
                             return client.state() == SessionState::kActive &&
                                    server.state() == SessionState::kActive;
                         },
                         5000));

    const std::uint8_t byte = 0x41;
    BENTO_CHECK(client.Send(99, &byte, 1) == SessionError::kInvalidArgument);
    BENTO_CHECK(client.Send(1, nullptr, 1) == SessionError::kInvalidArgument);
    const std::vector<std::uint8_t> huge(
        static_cast<std::size_t>(go2cpp::bento::net::kFrameMaxPayload), 0x42);
    BENTO_CHECK(client.Send(huge.data(), huge.size()) == SessionError::kMessageTooLarge);
    BENTO_CHECK(client.Send(static_cast<const std::uint8_t*>(nullptr), 0) == SessionError::kNone);

    client.Close();
    BENTO_CHECK(PumpBoth(client, server,
                         [&] {
                             return client.state() == SessionState::kClosed &&
                                    server.state() == SessionState::kClosed;
                         },
                         5000));
    BENTO_CHECK(client.Send("closed") == SessionError::kInvalidState);
    BENTO_CHECK(client.RequestControl() == SessionError::kInvalidState);
    BENTO_CHECK(!client.Poll(0));
    client.Close();  // 幂等
    BENTO_CHECK_EQ(client.state(), SessionState::kClosed);
}

/// 场景 14：会话层线格式与 Link 协议显式区分（A-03）。
void TestSessionWireFormat() {
    // 客户端 Start() 的第一帧是 Pairing，必须带会话层 magic "SESS"/version 1。
    FakeClock clock;
    auto transport = std::make_unique<StubTransport>(Code::kOk);
    StubTransport* raw = transport.get();
    Session session(StubConfig(clock, std::move(transport)));
    BENTO_CHECK(session.Start() == SessionError::kNone);
    const std::vector<std::uint8_t> bytes = raw->written();
    BENTO_CHECK(bytes.size() >= go2cpp::bento::net::kGenericFrameHeaderBytes);

    go2cpp::bento::net::Frame frame;
    const go2cpp::bento::net::FrameFormat session_format{
        go2cpp::bento::app::kSessionFrameMagic, go2cpp::bento::app::kSessionFrameVersion};
    BENTO_CHECK(go2cpp::bento::net::Decode(session_format, bytes.data(), bytes.size(), &frame) ==
                go2cpp::bento::net::FrameStatus::kOk);
    BENTO_CHECK_EQ(frame.magic, go2cpp::bento::app::kSessionFrameMagic);
    BENTO_CHECK_EQ(frame.version, go2cpp::bento::app::kSessionFrameVersion);
    BENTO_CHECK_EQ(frame.type,
                   static_cast<std::uint16_t>(go2cpp::bento::app::SessionFrameType::kPairing));

    // 同一串字节用 Link 解码器必须失败（magic 不同），不能误当成 Link 帧。
    BENTO_CHECK(go2cpp::bento::net::Decode(bytes.data(), bytes.size(), &frame) ==
                go2cpp::bento::net::FrameStatus::kBadMagic);
    BENTO_CHECK(go2cpp::bento::app::kSessionFrameMagic != go2cpp::bento::net::kFrameMagic);

    // 反向：Link 帧喂给会话层 → magic 阶段失败并关闭会话。
    auto link_stub = std::make_unique<StubTransport>(Code::kOk);
    StubTransport* link_raw = link_stub.get();
    go2cpp::bento::net::Frame hello;
    hello.type = static_cast<std::uint16_t>(go2cpp::bento::FrameType::kHello);
    link_raw->QueueRead(go2cpp::bento::net::Encode(hello));
    FakeClock other_clock;
    Session other(StubConfig(other_clock, std::move(link_stub)));
    BENTO_CHECK(other.Start() == SessionError::kNone);
    BENTO_CHECK(!other.Poll(0));
    BENTO_CHECK_EQ(other.last_error(), SessionError::kProtocolError);

    // 类型段与 Link 的 0x01..0x07 不重叠。
    BENTO_CHECK(static_cast<std::uint16_t>(go2cpp::bento::app::SessionFrameType::kPairing) >
                0x00ffU);
    BENTO_CHECK(static_cast<std::uint16_t>(go2cpp::bento::app::SessionFrameType::kControlRequest) >
                0x00ffU);
}

/// 场景 15：Control 帧只接受本会话成员，陌生 peer 的授权按协议错误关闭（D-01）。
void TestControlRejectsForeignPeer() {
    auto pair = MemoryTransport::CreatePair();
    FakeClock clock;
    TapTransport* tap = nullptr;
    PairConfigs configs = MakeTapPairConfigs(clock, std::move(pair), &tap);
    Session client(configs.client);
    Session server(configs.server);
    ErrorTrace errors;
    errors.Attach(client);
    StateTrace states;
    states.Attach(client);
    BENTO_CHECK(Activate(client, server));

    // 对端伪造 Control(action=Grant, peer=99, mode=kInteract)：99 不属于会话。
    const std::vector<std::uint8_t> payload = {0, 0, 0, 0, 99, 2};
    BENTO_CHECK(tap->InjectWrite(
        EncodeSessionFrame(go2cpp::bento::app::SessionFrameType::kControl, payload)));
    for (int attempt = 0; attempt < 100 && client.state() != SessionState::kClosed; ++attempt) {
        client.Poll(1);
    }
    BENTO_CHECK_EQ(client.state(), SessionState::kClosed);
    BENTO_CHECK_EQ(client.last_error(), SessionError::kProtocolError);
    BENTO_CHECK(errors.Contains(SessionError::kProtocolError));
    BENTO_CHECK(states.Contains(SessionState::kClosed));
    BENTO_CHECK(client.stats().protocol_errors >= 1);
    BENTO_CHECK_EQ(server.control().controller(), 0U);
}

/// 场景 16：授予模式来自调用方/配置，广播的是实际模式而不是写死的 kInteract（D-02）。
/// @note 先取得 kInteract 再申请 kView：状态发生 kInteract→kView 的变化时才会
///       产生 kGranted 回调，因此能真正观察到「广播里的 mode」；若实现把 mode
///       写死成 kInteract，客户端的状态不会变化，下面的等待会超时失败。
bool SawGrantedWithMode(const ControlTrace& trace, std::uint32_t peer_id, ControlMode mode) {
    for (const auto& change : trace.changes) {
        if (change.peer_id == peer_id && change.reason == ControlChangeReason::kGranted &&
            change.to == mode) {
            return true;
        }
    }
    return false;
}

void TestControlGrantMode() {
    {
        // 调用方显式申请 kView：仲裁端按申请把控制器降为只读。
        auto pair = MemoryTransport::CreatePair();
        FakeClock clock;
        auto configs = MakePairConfigs(clock, std::move(pair));
        Session client(configs.client);
        Session server(configs.server);
        ControlTrace client_control;
        client_control.Attach(client);
        BENTO_CHECK(Activate(client, server));
        BENTO_CHECK(client.RequestControl() == SessionError::kNone);
        BENTO_CHECK(PumpBoth(client, server,
                             [&] { return client.control().controller() == 1U; }, 3000));
        BENTO_CHECK_EQ(client.control().mode(1), ControlMode::kInteract);

        BENTO_CHECK(client.RequestControl(ControlMode::kView) == SessionError::kNone);
        BENTO_CHECK(PumpBoth(
            client, server,
            [&] { return SawGrantedWithMode(client_control, 1, ControlMode::kView); }, 3000));
        BENTO_CHECK(SawGrantedWithMode(client_control, 1, ControlMode::kView));
        BENTO_CHECK_EQ(client.control().mode(1), ControlMode::kView);
        BENTO_CHECK_EQ(client.control().controller(), 0U);
        BENTO_CHECK_EQ(server.control().controller(), 0U);
        BENTO_CHECK_EQ(server.control().mode(1), ControlMode::kView);
        BENTO_CHECK(client.RequestControl(ControlMode::kNone) ==
                    SessionError::kInvalidArgument);
    }
    {
        // 仲裁端配置 requested_control_mode = kView：无参 RequestControl 按配置执行。
        auto pair = MemoryTransport::CreatePair();
        FakeClock clock;
        auto configs = MakePairConfigs(clock, std::move(pair));
        configs.server.requested_control_mode = ControlMode::kView;
        Session client(configs.client);
        Session server(configs.server);
        ControlTrace client_control;
        client_control.Attach(client);
        BENTO_CHECK(Activate(client, server));
        BENTO_CHECK(server.RequestControl(ControlMode::kInteract) == SessionError::kNone);
        BENTO_CHECK(PumpBoth(client, server,
                             [&] { return client.control().controller() == 2U; }, 3000));
        BENTO_CHECK_EQ(client.control().mode(2), ControlMode::kInteract);

        BENTO_CHECK(server.RequestControl() == SessionError::kNone);
        BENTO_CHECK(PumpBoth(
            client, server,
            [&] { return SawGrantedWithMode(client_control, 2, ControlMode::kView); }, 3000));
        BENTO_CHECK(SawGrantedWithMode(client_control, 2, ControlMode::kView));
        BENTO_CHECK_EQ(client.control().controller(), 0U);
        BENTO_CHECK_EQ(server.control().mode(2), ControlMode::kView);
    }
}

/// 场景 17：队列满丢弃内部帧必须计数，不能静默（D-07）。
void TestQueueFullDropsInternalFrames() {
    auto pair = MemoryTransport::CreatePair();
    FakeClock clock;
    TapTransport* tap = nullptr;
    PairConfigs configs = MakeTapPairConfigs(clock, std::move(pair), &tap);
    // 上限设为 2：激活期的 ChannelOpen/ChannelAck 正好占满一次 Flush，
    // 不会在握手阶段就丢帧（那会让本用例的计数不干净）。
    configs.server.max_outbox_frames = 2;
    Session client(configs.client);
    Session server(configs.server);
    BENTO_CHECK(Activate(client, server));

    tap->SetWriteMode(TapTransport::WriteMode::kTimeout);
    BENTO_CHECK(server.Send("stuck-a") == SessionError::kNone);
    BENTO_CHECK(server.Send("stuck-b") == SessionError::kNone);
    server.Poll(1);  // 写超时：两个数据帧留在 outbox，占满名额
    BENTO_CHECK_EQ(server.stats().dropped_frames, std::uint64_t{0});

    // 客户端请求控制权；server 处理请求后需要广播 Grant，但队列已满。
    BENTO_CHECK(client.RequestControl() == SessionError::kNone);
    for (int attempt = 0; attempt < 100 && server.stats().dropped_frames == 0; ++attempt) {
        client.Poll(1);
        server.Poll(1);
    }
    BENTO_CHECK_EQ(server.stats().dropped_frames, std::uint64_t{1});
    BENTO_CHECK_EQ(client.control().controller(), 0U);  // 广播确实没发出
    BENTO_CHECK_EQ(server.control().controller(), 1U);  // 仲裁端本地状态已更新
    tap->SetWriteMode(TapTransport::WriteMode::kForward);
}

/// 场景 18：写超时只重发未写出的剩余字节，不能整帧重发（SESS-01）。
void TestPartialWriteResumesRemainder() {
    auto pair = MemoryTransport::CreatePair();
    FakeClock clock;
    TapTransport* tap = nullptr;
    PairConfigs configs = MakeTapPairConfigs(clock, std::move(pair), &tap);
    Session client(configs.client);
    Session server(configs.server);
    DataSink client_data;
    client_data.Attach(client);
    BENTO_CHECK(Activate(client, server));

    tap->SetWriteMode(TapTransport::WriteMode::kPartialThenTimeout, 7);
    BENTO_CHECK(server.Send("hello-world") == SessionError::kNone);
    server.Poll(1);  // 只写出 7 字节后超时
    BENTO_CHECK(client_data.messages.empty());

    tap->SetWriteMode(TapTransport::WriteMode::kForward);
    BENTO_CHECK(PumpBoth(client, server,
                         [&] { return !client_data.messages.empty(); }, 3000));
    BENTO_CHECK_EQ(client.state(), SessionState::kActive);
    BENTO_CHECK_EQ(server.state(), SessionState::kActive);
    BENTO_CHECK_EQ(client.stats().protocol_errors, std::uint64_t{0});
    BENTO_CHECK_EQ(client_data.messages.size(), std::size_t{1});
    if (!client_data.messages.empty()) {
        BENTO_CHECK_EQ(client_data.messages[0], std::string("hello-world"));
    }
}

}  // namespace

int main() {
    TestEndpointParsing();
    TestConfigValidation();
    TestHappyPath();
    TestWaitFor();
    TestManualAcceptPairing();
    TestPairingTimeout();
    TestPairingRejected();
    TestConnectTimeout();
    TestConnectFailure();
    TestHeartbeatAndTimeout();
    TestControlArbitration();
    TestCloseTimeout();
    TestWriteFailed();
    TestProtocolError();
    TestInvalidOperations();
    TestSessionWireFormat();
    TestControlRejectsForeignPeer();
    TestControlGrantMode();
    TestQueueFullDropsInternalFrames();
    TestPartialWriteResumesRemainder();
    return bento_test::Finish("bento_session");
}
