/**
 * @file audit_session_stress.cpp
 * @brief app::Session（MOD-39）审查用压测/随机/场景程序（不注册 CTest）。
 * @author go2cpp contributors
 * @date 2026-10-29
 * @copyright MIT License
 *
 * @details 与 `tests/bento/test_session.cpp` 的分工：
 *          - `test_session.cpp` 是注册进 CTest 的确定性回归测试（毫秒级用例）；
 *          - 本程序是**审查压测工具**：1000 并发会话、10 万次状态转换、
 *            1000+ 随机状态机操作（含故障注入）、5 个真实场景（真实 TCP
 *            port 1 连接失败、配对超时、Closing 状态 in-flight 数据、
 *            TCP/Memory 重连、60s 长稳）与 RSS/fd 泄漏检查。
 *
 *          刻意**不** include `test_util.hpp`（它会拖入 `link.hpp`），
 *          以独立证明 `session` 只依赖 transport/pairing/channel/heartbeat/
 *          session_control，不依赖 link/peer_hub。
 *
 * 用法：
 * @code
 *   audit_session_stress --mode=all --scale=1.0           # 全量（含 60s 长稳）
 *   audit_session_stress --mode=bench                     # 1000 并发 + 10 万转换
 *   audit_session_stress --mode=random --scale=1.0        # 随机状态机
 *   audit_session_stress --mode=scenarios                 # 5 个场景（不含长稳）
 *   audit_session_stress --mode=soak --seconds=60         # 60s 长稳
 *   audit_session_stress --mode=leaks --scale=0.02        # valgrind / ASan
 * @endcode
 *
 * 退出码：0 = 全部断言通过；1 = 有失败。
 */

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <dirent.h>
#include <fstream>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "go2cpp/bento/app/session.hpp"
#include "go2cpp/bento/net/frame.hpp"
#include "go2cpp/bento/net/memory_transport.hpp"
#include "go2cpp/bento/net/tcp_transport.hpp"

namespace {

using go2cpp::bento::ControlMode;
using go2cpp::bento::app::IsSessionTransitionAllowed;
using go2cpp::bento::app::Session;
using go2cpp::bento::app::SessionConfig;
using go2cpp::bento::app::SessionError;
using go2cpp::bento::app::SessionFrameType;
using go2cpp::bento::app::SessionState;
using go2cpp::bento::net::Code;
using go2cpp::bento::net::Endpoint;
using go2cpp::bento::net::ITransport;
using go2cpp::bento::net::MemoryTransport;
using go2cpp::bento::net::Result;
using go2cpp::bento::net::TcpTransport;

using Clock = std::chrono::steady_clock;

long g_checks = 0;
long g_failures = 0;

#define CHECK(condition)                                                                        \
    do {                                                                                        \
        ++g_checks;                                                                             \
        if (!(condition)) {                                                                     \
            std::fprintf(stderr, "[FAIL] %s:%d: %s\n", __FILE__, __LINE__, #condition);         \
            ++g_failures;                                                                       \
        }                                                                                       \
    } while (0)

#define CHECK_EQ(actual, expected)                                                              \
    do {                                                                                        \
        ++g_checks;                                                                             \
        const auto actual_value = (actual);                                                     \
        const auto expected_value = (expected);                                                 \
        if (!(actual_value == expected_value)) {                                                \
            std::fprintf(stderr, "[FAIL] %s:%d: %s == %s (%lld != %lld)\n", __FILE__, __LINE__, \
                         #actual, #expected, static_cast<long long>(actual_value),              \
                         static_cast<long long>(expected_value));                               \
            ++g_failures;                                                                       \
        }                                                                                       \
    } while (0)

#if defined(__SANITIZE_ADDRESS__)
constexpr bool kSanitizerBuild = true;
#else
constexpr bool kSanitizerBuild = false;
#endif

double SecondsSince(Clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::duration<double>>(Clock::now() - start).count();
}

std::size_t RssKb() {
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        if (line.rfind("VmRSS:", 0) == 0) {
            return static_cast<std::size_t>(std::strtoull(line.c_str() + 6, nullptr, 10));
        }
    }
    return 0;
}

int FdCount() {
    DIR* dir = opendir("/proc/self/fd");
    if (dir == nullptr) {
        return -1;
    }
    int count = 0;
    while (readdir(dir) != nullptr) {
        ++count;
    }
    closedir(dir);
    return count;
}

/// 可控逻辑时钟：同一会话对共享一个实例。
struct FakeClock {
    std::uint64_t now_ms = 1000;
};

/// 把 unique_ptr 端点包装成可拷贝的工厂（std::function 要求目标可拷贝）。
std::function<std::unique_ptr<ITransport>()> Factory(std::unique_ptr<ITransport> transport) {
    auto holder = std::make_shared<std::unique_ptr<ITransport>>(std::move(transport));
    return [holder]() -> std::unique_ptr<ITransport> { return std::move(*holder); };
}

std::function<std::unique_ptr<ITransport>()> FactoryFromShared(
    const std::shared_ptr<std::unique_ptr<ITransport>>& holder) {
    return [holder]() -> std::unique_ptr<ITransport> { return std::move(*holder); };
}

SessionConfig BaseConfig(FakeClock* clock) {
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
    if (clock != nullptr) {
        config.now = [clock]() { return clock->now_ms; };
    }
    return config;
}

struct PairConfigs {
    SessionConfig client;
    SessionConfig server;
};

PairConfigs MakePairConfigs(FakeClock* clock, MemoryTransport::Pair pair) {
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

/// 记录状态迁移并校验合法性；状态迁移总数用于「10 万次转换」口径。
struct Guard {
    long transitions = 0;
    long illegal = 0;
    long errors = 0;

    void Attach(Session* session) {
        session->on_state = [this](SessionState from, SessionState to) {
            ++transitions;
            if (!IsSessionTransitionAllowed(from, to)) {
                ++illegal;
                std::fprintf(stderr, "[FAIL] illegal transition %s -> %s\n",
                             go2cpp::bento::app::SessionStateName(from),
                             go2cpp::bento::app::SessionStateName(to));
            }
        };
        session->on_error = [this](SessionError, const std::string&) { ++errors; };
    }
};

/// 用迭代次数而不是真实时间推进双端（MemoryTransport + 假时钟）。
template <typename Predicate>
bool PumpBoth(Session& first, Session& second, int max_rounds, Predicate done) {
    if (done()) {
        return true;
    }
    for (int round = 0; round < max_rounds; ++round) {
        first.Poll(0);
        second.Poll(0);
        if (done()) {
            return true;
        }
    }
    return done();
}

/// 真实时间 pump（真实 TCP 场景）。
template <typename Predicate>
bool RealPump(Session& first, Session& second, int timeout_ms, Predicate done) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(std::max(timeout_ms, 0));
    while (Clock::now() < deadline) {
        first.Poll(1);
        second.Poll(1);
        if (done()) {
            return true;
        }
    }
    return done();
}

std::vector<std::uint8_t> EncodeSessionBytes(SessionFrameType type,
                                             std::vector<std::uint8_t> payload) {
    go2cpp::bento::net::Frame frame;
    frame.type = static_cast<std::uint16_t>(type);
    frame.payload = std::move(payload);
    return go2cpp::bento::net::Encode(
        frame, go2cpp::bento::net::FrameFormat{go2cpp::bento::app::kSessionFrameMagic,
                                               go2cpp::bento::app::kSessionFrameVersion});
}

// ---------------------------------------------------------------------------
// 可编程故障传输：包住一端，可杀连接/写失败/向对端注入原始字节。
// ---------------------------------------------------------------------------

class ChaosTransport final : public ITransport {
   public:
    explicit ChaosTransport(std::unique_ptr<ITransport> inner) : m_inner(std::move(inner)) {}

    void Kill() {
        m_killed = true;
        if (m_inner) {
            m_inner->Close();
        }
    }
    void SetFailWrite(bool fail) { m_fail_write = fail; }
    bool InjectToPeer(const std::vector<std::uint8_t>& bytes) {
        if (!m_inner) {
            return false;
        }
        return m_inner->WriteFull(bytes.data(), bytes.size(), std::chrono::milliseconds(50)).ok();
    }

    Result Connect(const Endpoint& endpoint, std::chrono::milliseconds timeout) override {
        if (m_killed) {
            return Result{Code::kClosed, ENOTCONN, 0};
        }
        return m_inner->Connect(endpoint, timeout);
    }
    Result Read(std::uint8_t* buffer, std::size_t capacity,
                std::chrono::milliseconds timeout) override {
        if (m_killed) {
            return Result{Code::kClosed, ENOTCONN, 0};
        }
        return m_inner->Read(buffer, capacity, timeout);
    }
    Result Write(const std::uint8_t* data, std::size_t size,
                 std::chrono::milliseconds timeout) override {
        if (m_killed) {
            return Result{Code::kClosed, ENOTCONN, 0};
        }
        if (m_fail_write) {
            return Result{Code::kError, EIO, 0};
        }
        return m_inner->Write(data, size, timeout);
    }
    Result ShutdownWrite() override { return m_inner->ShutdownWrite(); }
    Result Close() override {
        m_killed = true;
        return m_inner->Close();
    }

   private:
    std::unique_ptr<ITransport> m_inner;
    bool m_killed = false;
    bool m_fail_write = false;
};

// ---------------------------------------------------------------------------
// 1. 1000 并发会话（MemoryTransport）
// ---------------------------------------------------------------------------

struct SessionPairSlot {
    std::unique_ptr<Session> client;
    std::unique_ptr<Session> server;
};

void BenchThousandSessions(double scale) {
    const int count = std::max(1, static_cast<int>(1000.0 * scale));
    const std::size_t messages_per_session = 10;
    std::printf("\n[BENCH] 并发会话 count=%d messages/session=%zu\n", count, messages_per_session);

    const std::size_t rss_before = RssKb();
    const int fd_before = FdCount();
    FakeClock clock;
    std::vector<SessionPairSlot> slots;
    slots.reserve(static_cast<std::size_t>(count));
    std::vector<Guard> guards(static_cast<std::size_t>(count));
    std::vector<std::size_t> received(static_cast<std::size_t>(count), 0);
    std::vector<std::uint64_t> reply_bytes(static_cast<std::size_t>(count), 0);

    const auto build_begin = Clock::now();
    for (int index = 0; index < count; ++index) {
        PairConfigs configs = MakePairConfigs(&clock, MemoryTransport::CreatePair());
        SessionPairSlot slot;
        slot.client = std::make_unique<Session>(std::move(configs.client));
        slot.server = std::make_unique<Session>(std::move(configs.server));
        guards[static_cast<std::size_t>(index)].Attach(slot.client.get());
        guards[static_cast<std::size_t>(index)].Attach(slot.server.get());
        const std::size_t slot_index = static_cast<std::size_t>(index);
        slot.server->on_data = [&received, &reply_bytes, slot_index](std::uint32_t,
                                                                     const std::uint8_t*,
                                                                     std::size_t size) {
            ++received[slot_index];
            reply_bytes[slot_index] += size;
        };
        slots.push_back(std::move(slot));
    }
    const double build_seconds = SecondsSince(build_begin);

    const auto connect_begin = Clock::now();
    for (auto& slot : slots) {
        CHECK(slot.client->Start() == SessionError::kNone);
        CHECK(slot.server->Start() == SessionError::kNone);
    }
    int rounds = 0;
    while (rounds < 200) {
        for (auto& slot : slots) {
            slot.client->Poll(0);
            slot.server->Poll(0);
        }
        ++rounds;
        std::size_t active_now = 0;
        for (auto& slot : slots) {
            if (slot.client->state() == SessionState::kActive &&
                slot.server->state() == SessionState::kActive) {
                ++active_now;
            }
        }
        if (active_now == slots.size()) {
            break;
        }
    }
    const double connect_seconds = SecondsSince(connect_begin);
    std::size_t active = 0;
    for (auto& slot : slots) {
        if (slot.client->state() == SessionState::kActive) {
            ++active;
        }
    }
    CHECK_EQ(active, slots.size());
    std::printf("[BENCH] 建连 %.3fs（构造 %.3fs） active=%zu/%zu rounds=%d\n", connect_seconds,
                build_seconds, active, slots.size(), rounds);

    // 双端各发 10 条 64B 数据，pump 到全部收到。
    const auto data_begin = Clock::now();
    const std::string payload(64, 'x');
    for (auto& slot : slots) {
        for (std::size_t index = 0; index < messages_per_session; ++index) {
            CHECK(slot.client->Send(payload) == SessionError::kNone);
        }
    }
    int data_rounds = 0;
    while (data_rounds < 200) {
        for (auto& slot : slots) {
            slot.client->Poll(0);
            slot.server->Poll(0);
        }
        ++data_rounds;
        std::size_t done = 0;
        for (std::size_t index = 0; index < slots.size(); ++index) {
            if (received[index] >= messages_per_session) {
                ++done;
            }
        }
        if (done == slots.size()) {
            break;
        }
    }
    const double data_seconds = SecondsSince(data_begin);
    std::size_t total_messages = 0;
    std::uint64_t total_bytes = 0;
    for (std::size_t index = 0; index < slots.size(); ++index) {
        CHECK_EQ(received[index], messages_per_session);
        total_messages += received[index];
        total_bytes += reply_bytes[index];
    }
    std::printf("[BENCH] 数据 %zu 条 / %llu B，%.3fs，%.0f msg/s，%.2f MiB/s\n", total_messages,
                static_cast<unsigned long long>(total_bytes), data_seconds,
                data_seconds > 0.0 ? static_cast<double>(total_messages) / data_seconds : 0.0,
                data_seconds > 0.0 ? static_cast<double>(total_bytes) / data_seconds / 1048576.0
                                   : 0.0);

    // 关闭：全部 client 主动关闭，pump 到双方 Closed。
    const auto close_begin = Clock::now();
    for (auto& slot : slots) {
        slot.client->Close();
    }
    int close_rounds = 0;
    while (close_rounds < 100) {
        for (auto& slot : slots) {
            slot.client->Poll(0);
            slot.server->Poll(0);
        }
        ++close_rounds;
        std::size_t closed_now = 0;
        for (auto& slot : slots) {
            if (slot.client->state() == SessionState::kClosed &&
                slot.server->state() == SessionState::kClosed) {
                ++closed_now;
            }
        }
        if (closed_now == slots.size()) {
            break;
        }
    }
    const double close_seconds = SecondsSince(close_begin);
    std::size_t closed = 0;
    long illegal = 0;
    long errors = 0;
    for (std::size_t index = 0; index < slots.size(); ++index) {
        if (slots[index].client->state() == SessionState::kClosed) {
            ++closed;
        }
        CHECK(slots[index].server->state() == SessionState::kClosed);
        illegal += guards[index].illegal;
        errors += guards[index].errors;
    }
    CHECK_EQ(closed, slots.size());
    CHECK_EQ(illegal, 0L);
    CHECK_EQ(errors, 0L);
    std::printf("[BENCH] 关闭 %zu/%zu，%.3fs\n", closed, slots.size(), close_seconds);

    const std::size_t rss_alive = RssKb();
    slots.clear();
    const std::size_t rss_after = RssKb();
    const int fd_after = FdCount();
    std::printf("[BENCH] RSS %zu -> %zu（活跃） -> %zu KiB；fd %d -> %d\n", rss_before, rss_alive,
                rss_after, fd_before, fd_after);
    CHECK_EQ(fd_after, fd_before);
    if (!kSanitizerBuild) {
        // 1000 个 Session 常驻时 RSS 必然上涨；析构后只允许保留分配器缓存。
        CHECK(rss_after < rss_before + 16 * 1024);
    }
}

// ---------------------------------------------------------------------------
// 2. 10 万次状态转换
// ---------------------------------------------------------------------------

void BenchStateTransitions(double scale) {
    const long target = std::max<long>(1000, static_cast<long>(100000.0 * scale));
    std::printf("\n[BENCH] 状态转换 target=%ld\n", target);
    const std::size_t rss_before = RssKb();
    const auto begin = Clock::now();
    long transitions = 0;
    long rounds = 0;
    long failures = 0;
    FakeClock clock;
    while (transitions < target) {
        PairConfigs configs = MakePairConfigs(&clock, MemoryTransport::CreatePair());
        Session client(std::move(configs.client));
        Session server(std::move(configs.server));
        Guard guard;
        guard.Attach(&client);
        guard.Attach(&server);
        std::size_t received = 0;
        server.on_data = [&received](std::uint32_t, const std::uint8_t*, std::size_t) {
            ++received;
        };
        if (client.Start() != SessionError::kNone || server.Start() != SessionError::kNone) {
            ++failures;
            continue;
        }
        const bool active = PumpBoth(client, server, 50, [&] {
            return client.state() == SessionState::kActive &&
                   server.state() == SessionState::kActive;
        });
        if (!active) {
            ++failures;
            continue;
        }
        const std::string one = "x";
        if (client.Send(one) != SessionError::kNone) {
            ++failures;
        }
        if (!PumpBoth(client, server, 50, [&] { return received == 1; })) {
            ++failures;
        }
        client.Close();
        if (!PumpBoth(client, server, 50, [&] {
                return client.state() == SessionState::kClosed &&
                       server.state() == SessionState::kClosed;
            })) {
            ++failures;
        }
        transitions += guard.transitions;
        if (guard.illegal != 0 || guard.errors != 0) {
            ++failures;
        }
        ++rounds;
    }
    const double seconds = SecondsSince(begin);
    const std::size_t rss_after = RssKb();
    std::printf(
        "[BENCH] %ld transitions / %ld rounds / %.3fs = %.0f transitions/s（%.0f ns/transition），"
        "failures=%ld\n",
        transitions, rounds, seconds,
        seconds > 0.0 ? static_cast<double>(transitions) / seconds : 0.0,
        seconds > 0.0 ? seconds * 1e9 / static_cast<double>(transitions) : 0.0, failures);
    std::printf("[BENCH] RSS %zu -> %zu KiB\n", rss_before, rss_after);
    CHECK_EQ(failures, 0L);
    CHECK(transitions >= target);
    if (!kSanitizerBuild) {
        CHECK(rss_after < rss_before + 8 * 1024);
    }
}

// ---------------------------------------------------------------------------
// 3. 随机状态机（1000+ 操作，故障注入）
// ---------------------------------------------------------------------------

struct RandomSlot {
    FakeClock clock;
    std::unique_ptr<Session> client;
    std::unique_ptr<Session> server;
    ChaosTransport* chaos = nullptr;
    Guard guard;
    bool auto_accept = true;
};

void ResetRandomSlot(RandomSlot* slot, std::mt19937* rng) {
    slot->client.reset();
    slot->server.reset();
    slot->chaos = nullptr;
    slot->guard = Guard{};
    slot->auto_accept = ((*rng)() % 4) != 0;

    MemoryTransport::Pair pair = MemoryTransport::CreatePair();
    auto chaos = std::make_unique<ChaosTransport>(std::move(pair.first));
    slot->chaos = chaos.get();
    auto holder = std::make_shared<std::unique_ptr<ITransport>>(std::move(chaos));

    PairConfigs configs = MakePairConfigs(&slot->clock, std::move(pair));
    configs.client.transport_factory = FactoryFromShared(holder);
    configs.server.auto_accept_pairing = slot->auto_accept;
    slot->client = std::make_unique<Session>(std::move(configs.client));
    slot->server = std::make_unique<Session>(std::move(configs.server));
    slot->guard.Attach(slot->client.get());
    slot->guard.Attach(slot->server.get());
}

SessionError SendRandom(Session* session, std::mt19937* rng) {
    const std::size_t size = (*rng)() % 513;
    const std::string payload(size, 'r');
    const std::uint32_t modes[] = {1, 99};
    if (((*rng)() % 10) == 0) {
        return session->Send(modes[(*rng)() % 2],
                             reinterpret_cast<const std::uint8_t*>(payload.data()), payload.size());
    }
    return session->Send(payload);
}

void RunRandom(double scale, std::uint32_t seed) {
    const int operations = std::max(1000, static_cast<int>(20000.0 * scale));
    const int slot_count = 8;
    std::printf("\n[RANDOM] operations=%d slots=%d seed=%u\n", operations, slot_count, seed);
    const std::size_t rss_before = RssKb();
    std::mt19937 rng(seed);
    std::vector<RandomSlot> slots(static_cast<std::size_t>(slot_count));
    for (auto& slot : slots) {
        ResetRandomSlot(&slot, &rng);
        CHECK(slot.client->Start() == SessionError::kNone);
        CHECK(slot.server->Start() == SessionError::kNone);
    }

    long transitions = 0;
    long illegal = 0;
    long injected = 0;
    long killed = 0;
    long rebuilds = 0;
    long polls = 0;
    long sends = 0;
    long controls = 0;
    long closes = 0;

    for (int step = 0; step < operations; ++step) {
        RandomSlot& slot = slots[rng() % static_cast<std::uint32_t>(slots.size())];
        const std::uint32_t action = rng() % 100;
        if (action < 34) {
            slot.client->Poll(0);
            slot.server->Poll(0);
            ++polls;
        } else if (action < 48) {
            const bool to_client = (rng() % 2) == 0;
            const SessionError error =
                SendRandom(to_client ? slot.client.get() : slot.server.get(), &rng);
            CHECK(error == SessionError::kNone || error == SessionError::kInvalidState ||
                  error == SessionError::kInvalidArgument ||
                  error == SessionError::kMessageTooLarge);
            ++sends;
        } else if (action < 56) {
            Session* target = (rng() % 2) == 0 ? slot.client.get() : slot.server.get();
            const ControlMode mode =
                (rng() % 3) == 0 ? ControlMode::kView : ControlMode::kInteract;
            const SessionError error = target->RequestControl(mode);
            CHECK(error == SessionError::kNone || error == SessionError::kInvalidState ||
                  error == SessionError::kControlDenied ||
                  error == SessionError::kInvalidArgument);
            ++controls;
        } else if (action < 64) {
            Session* target = (rng() % 2) == 0 ? slot.client.get() : slot.server.get();
            target->Close();
            CHECK(target->state() == SessionState::kClosing ||
                  target->state() == SessionState::kClosed);
            ++closes;
        } else if (action < 68) {
            slot.server->AcceptPairing();
        } else if (action < 80) {
            slot.clock.now_ms += (rng() % 4000);
        } else if (action < 86) {
            // 向对端注入一帧：合法 Control（陌生 peer）或完全错误的 magic。
            if ((rng() % 2) == 0) {
                const std::vector<std::uint8_t> payload = {0, 0, 0, 0, 99, 2};
                slot.chaos->InjectToPeer(EncodeSessionBytes(SessionFrameType::kControl, payload));
            } else {
                const std::vector<std::uint8_t> junk(20, 0xab);
                slot.chaos->InjectToPeer(junk);
            }
            ++injected;
        } else if (action < 90) {
            slot.chaos->Kill();
            ++killed;
        } else if (action < 94) {
            slot.chaos->SetFailWrite((rng() % 2) == 0);
        } else {
            if (slot.client->state() == SessionState::kClosed ||
                slot.server->state() == SessionState::kClosed || (rng() % 3) == 0) {
                ResetRandomSlot(&slot, &rng);
                CHECK(slot.client->Start() == SessionError::kNone);
                CHECK(slot.server->Start() == SessionError::kNone);
                ++rebuilds;
            }
        }

        for (auto& item : slots) {
            CHECK(item.client->state() <= SessionState::kClosed);
            CHECK(item.server->state() <= SessionState::kClosed);
            transitions += item.guard.transitions;
            illegal += item.guard.illegal;
            item.guard.transitions = 0;
            item.guard.illegal = 0;
        }
    }

    long errors = 0;
    for (auto& slot : slots) {
        errors += slot.guard.errors;
        slot.client.reset();
        slot.server.reset();
    }
    slots.clear();
    const std::size_t rss_after = RssKb();
    std::printf(
        "[RANDOM] polls=%ld sends=%ld controls=%ld closes=%ld inject=%ld kill=%ld rebuild=%ld "
        "transitions=%ld illegal=%ld errors=%ld\n",
        polls, sends, controls, closes, injected, killed, rebuilds, transitions, illegal, errors);
    std::printf("[RANDOM] RSS %zu -> %zu KiB\n", rss_before, rss_after);
    CHECK_EQ(illegal, 0L);
    CHECK(rebuilds > 0);
    if (!kSanitizerBuild) {
        CHECK(rss_after < rss_before + 16 * 1024);
    }
}

// ---------------------------------------------------------------------------
// 4. 真实 TCP 辅助
// ---------------------------------------------------------------------------

struct TcpLink {
    TcpTransport listener;
    Endpoint local;

    bool Open() {
        if (!listener.Listen(Endpoint{"127.0.0.1", 0}).ok()) {
            return false;
        }
        return listener.LocalEndpoint(&local).ok();
    }
    std::string EndpointText() const { return local.host + ":" + std::to_string(local.port); }
};

SessionConfig RealBaseConfig() {
    SessionConfig config;
    config.pairing_key = "invite-key";
    config.connect_timeout_ms = 2000;
    config.io_timeout_ms = 20;
    config.close_timeout_ms = 1000;
    config.heartbeat_interval_ms = 30000;
    config.heartbeat_timeout_ms = 90000;
    config.pairing_timeout_ms = 5000;
    config.pairing_retransmit_ms = 500;
    return config;
}

bool AcceptServer(TcpLink* link, SessionConfig config, std::unique_ptr<Session>* server) {
    auto accepted = std::make_unique<TcpTransport>();
    if (!link->listener.Accept(accepted.get(), std::chrono::milliseconds(2000)).ok()) {
        return false;
    }
    config.server = true;
    config.local_peer_id = 2;
    auto holder = std::make_shared<std::unique_ptr<ITransport>>(std::move(accepted));
    config.transport_factory = FactoryFromShared(holder);
    *server = std::make_unique<Session>(std::move(config));
    return true;
}

// ---------------------------------------------------------------------------
// 场景 1：真实 TCP 连接失败（127.0.0.1:1）
// ---------------------------------------------------------------------------

void ScenarioConnectRefused() {
    std::printf("\n[SCENARIO] 1. 真实 TCP 连接失败 127.0.0.1:1\n");
    const int fd_before = FdCount();
    SessionConfig config;
    config.endpoint = "127.0.0.1:1";
    config.pairing_key = "invite-key";
    config.connect_timeout_ms = 1000;
    Session session(config);
    const auto begin = Clock::now();
    const SessionError error = session.Start();
    const double seconds = SecondsSince(begin);
    std::printf("[SCENARIO] Start -> %s（%.3fs）state=%s message=%s\n",
                go2cpp::bento::app::SessionErrorName(error), seconds,
                go2cpp::bento::app::SessionStateName(session.state()),
                session.last_error_message().c_str());
    CHECK(error == SessionError::kConnectFailed || error == SessionError::kConnectTimeout);
    CHECK_EQ(session.state(), SessionState::kClosed);
    CHECK(!session.last_error_message().empty());
    if (error == SessionError::kConnectTimeout) {
        // 某些环境会 DROP 到 1 端口的包；记录但不判失败。
        std::printf("[SCENARIO] 注意：port 1 返回 kConnectTimeout（防火墙 DROP）\n");
    }

    // 失败路径不得泄漏 fd：重复 200 次，fd 回到基线。
    for (int index = 0; index < 200; ++index) {
        Session retry(config);
        const SessionError retry_error = retry.Start();
        CHECK(retry_error == SessionError::kConnectFailed ||
              retry_error == SessionError::kConnectTimeout);
        CHECK_EQ(retry.state(), SessionState::kClosed);
    }
    const int fd_after = FdCount();
    std::printf("[SCENARIO] 200 次失败重试后 fd %d -> %d\n", fd_before, fd_after);
    CHECK_EQ(fd_after, fd_before);

    // 语法非法端点 / 服务端缺 factory 仍在 Idle，便于修正配置后重试。
    {
        SessionConfig bad = config;
        bad.endpoint = "127.0.0.1";
        Session session_bad(bad);
        CHECK_EQ(session_bad.Start(), SessionError::kInvalidEndpoint);
        CHECK_EQ(session_bad.state(), SessionState::kIdle);
    }
    {
        SessionConfig bad = config;
        bad.endpoint.clear();
        bad.server = true;
        bad.pairing_key = "k";
        Session session_bad(bad);
        CHECK_EQ(session_bad.Start(), SessionError::kTransportUnavailable);
        CHECK_EQ(session_bad.state(), SessionState::kIdle);
    }
}

// ---------------------------------------------------------------------------
// 场景 2：配对超时
// ---------------------------------------------------------------------------

void ScenarioPairingTimeout() {
    std::printf("\n[SCENARIO] 2. 配对超时（假时钟）\n");
    FakeClock clock;
    PairConfigs configs = MakePairConfigs(&clock, MemoryTransport::CreatePair());
    const std::uint32_t pairing_timeout_ms = configs.client.pairing_timeout_ms;
    Session client(std::move(configs.client));
    Guard guard;
    guard.Attach(&client);
    std::vector<SessionState> states;
    states.push_back(client.state());
    client.on_state = [&states, &guard](SessionState from, SessionState to) {
        ++guard.transitions;
        if (!IsSessionTransitionAllowed(from, to)) {
            ++guard.illegal;
        }
        states.push_back(to);
    };
    CHECK(client.Start() == SessionError::kNone);
    clock.now_ms += pairing_timeout_ms + 1;
    CHECK(!client.Poll(0));
    CHECK_EQ(client.state(), SessionState::kClosed);
    CHECK_EQ(client.last_error(), SessionError::kPairingTimeout);
    CHECK_EQ(guard.errors, 1L);
    CHECK_EQ(guard.illegal, 0L);
    CHECK(states.size() >= 2);
    bool saw_closing = false;
    for (SessionState state : states) {
        if (state == SessionState::kClosing) {
            saw_closing = true;
        }
    }
    CHECK(saw_closing);
    CHECK(client.last_error_message().find("pairing timed out") != std::string::npos);
    std::printf("[SCENARIO] Closed, last_error=%s, transitions=%ld\n",
                go2cpp::bento::app::SessionErrorName(client.last_error()), guard.transitions);
}

// ---------------------------------------------------------------------------
// 场景 3：Closing 状态收到 in-flight 数据/控制/坏帧
// ---------------------------------------------------------------------------

struct TapServerPair {
    SessionConfig client;
    SessionConfig server;
    ChaosTransport* server_tap = nullptr;
};

TapServerPair MakeServerTapConfigs(FakeClock* clock, MemoryTransport::Pair pair) {
    TapServerPair result;
    // 可编程层包在 server 端：InjectToPeer 直接写 client 的读队列，用来
    // 模拟「对端在本端 FIN 之后仍发出的 in-flight 帧」。
    auto tap = std::make_unique<ChaosTransport>(std::move(pair.second));
    result.server_tap = tap.get();
    auto holder = std::make_shared<std::unique_ptr<ITransport>>(std::move(tap));

    result.client = BaseConfig(clock);
    result.client.endpoint = "127.0.0.1:9000";
    result.client.local_peer_id = 1;
    result.client.transport_factory =
        Factory(std::unique_ptr<ITransport>(std::move(pair.first)));
    result.client.close_timeout_ms = 500;

    result.server = BaseConfig(clock);
    result.server.server = true;
    result.server.local_peer_id = 2;
    result.server.transport_factory = FactoryFromShared(holder);
    return result;
}

void ScenarioClosingInflight() {
    std::printf("\n[SCENARIO] 3. Closing 状态 in-flight 数据/控制/坏帧\n");
    const std::size_t rss_before = RssKb();
    for (int iteration = 0; iteration < 50; ++iteration) {
        FakeClock clock;
        TapServerPair configs = MakeServerTapConfigs(&clock, MemoryTransport::CreatePair());
        Session client(std::move(configs.client));
        Session server(std::move(configs.server));
        Guard client_guard;
        Guard server_guard;
        client_guard.Attach(&client);
        server_guard.Attach(&server);
        CHECK(client.Start() == SessionError::kNone);
        CHECK(server.Start() == SessionError::kNone);
        CHECK(PumpBoth(client, server, 50, [&] {
            return client.state() == SessionState::kActive &&
                   server.state() == SessionState::kActive;
        }));
        client.Close();
        CHECK_EQ(client.state(), SessionState::kClosing);

        // 对端在收到 FIN 前发出的数据（真实 TCP 上很常见）：不应判协议错误。
        const std::vector<std::uint8_t> data_payload = {0x00, 0x01, 'l', 'a', 't', 'e'};
        CHECK(configs.server_tap->InjectToPeer(
            EncodeSessionBytes(SessionFrameType::kData, data_payload)));
        client.Poll(0);
        CHECK_EQ(client.last_error(), SessionError::kNone);
        CHECK_EQ(client_guard.errors, 0L);
        CHECK_EQ(client.stats().protocol_errors, 0);

        // in-flight Control / ChannelOpen 同样应被忽略，而不是协议错误。
        const std::vector<std::uint8_t> control_payload = {0, 0, 0, 0, 99, 2};
        CHECK(configs.server_tap->InjectToPeer(
            EncodeSessionBytes(SessionFrameType::kControl, control_payload)));
        client.Poll(0);
        CHECK_EQ(client.last_error(), SessionError::kNone);

        const std::vector<std::uint8_t> open_payload = {0, 1, 0, 0, 0, 2};
        CHECK(configs.server_tap->InjectToPeer(
            EncodeSessionBytes(SessionFrameType::kChannelOpen, open_payload)));
        client.Poll(0);
        CHECK_EQ(client.last_error(), SessionError::kNone);
        CHECK_EQ(client.stats().protocol_errors, 0);

        // 对端正常回 kClose，关闭窗口以 kNone 结束。
        server.Poll(0);
        if (server.state() != SessionState::kClosed) {
            server.Close();
        }
        CHECK(PumpBoth(client, server, 100, [&] {
            return client.state() == SessionState::kClosed &&
                   server.state() == SessionState::kClosed;
        }));
        CHECK_EQ(client.last_error(), SessionError::kNone);
        CHECK_EQ(server.last_error(), SessionError::kNone);
        CHECK_EQ(client_guard.illegal, 0L);
    }
    const std::size_t rss_after = RssKb();
    std::printf("[SCENARIO] 50 轮 in-flight 关闭完成，last_error=none，RSS %zu -> %zu KiB\n",
                rss_before, rss_after);
    if (!kSanitizerBuild) {
        CHECK(rss_after < rss_before + 8 * 1024);
    }

    // 反面用例：Closing 前（Connecting）收到坏 magic 仍应判协议错误。
    {
        FakeClock clock;
        TapServerPair configs = MakeServerTapConfigs(&clock, MemoryTransport::CreatePair());
        Session client(std::move(configs.client));
        CHECK(client.Start() == SessionError::kNone);
        const std::vector<std::uint8_t> junk(20, 0xab);
        CHECK(configs.server_tap->InjectToPeer(junk));
        client.Poll(0);
        CHECK_EQ(client.state(), SessionState::kClosed);
        CHECK_EQ(client.last_error(), SessionError::kProtocolError);
    }
}

// ---------------------------------------------------------------------------
// 场景 4：重连（断开 → 重建 → Active），MemoryTransport + 真实 TCP
// ---------------------------------------------------------------------------

void RunReconnectMemory(int rounds) {
    const std::size_t rss_before = RssKb();
    FakeClock clock;
    long failures = 0;
    for (int round = 0; round < rounds; ++round) {
        PairConfigs configs = MakePairConfigs(&clock, MemoryTransport::CreatePair());
        Session client(std::move(configs.client));
        Session server(std::move(configs.server));
        std::size_t received = 0;
        server.on_data = [&received](std::uint32_t, const std::uint8_t*, std::size_t) {
            ++received;
        };
        if (client.Start() != SessionError::kNone || server.Start() != SessionError::kNone) {
            ++failures;
            continue;
        }
        if (!PumpBoth(client, server, 50, [&] {
                return client.state() == SessionState::kActive &&
                       server.state() == SessionState::kActive;
            })) {
            ++failures;
            continue;
        }
        if (client.Send("reconnect") != SessionError::kNone) {
            ++failures;
        }
        if (!PumpBoth(client, server, 50, [&] { return received == 1; })) {
            ++failures;
        }
        client.Close();
        if (!PumpBoth(client, server, 50, [&] {
                return client.state() == SessionState::kClosed &&
                       server.state() == SessionState::kClosed;
            })) {
            ++failures;
        }
    }
    const std::size_t rss_after = RssKb();
    CHECK_EQ(failures, 0L);
    std::printf("[SCENARIO] Memory 重连 %d 轮，failures=%ld，RSS %zu -> %zu KiB\n", rounds,
                failures, rss_before, rss_after);
    if (!kSanitizerBuild) {
        CHECK(rss_after < rss_before + 8 * 1024);
    }
}

void RunReconnectTcp(int rounds) {
    const int fd_before = FdCount();
    long failures = 0;
    long checked = 0;
    for (int round = 0; round < rounds; ++round) {
        TcpLink link;
        if (!link.Open()) {
            ++failures;
            std::fprintf(stderr, "[SCENARIO] TCP round %d: listen failed\n", round);
            continue;
        }
        SessionConfig client_config = RealBaseConfig();
        client_config.endpoint = link.EndpointText();
        Session client(client_config);
        const SessionError client_error = client.Start();
        if (client_error != SessionError::kNone) {
            ++failures;
            std::fprintf(stderr, "[SCENARIO] TCP round %d: client start %s (%s)\n", round,
                         go2cpp::bento::app::SessionErrorName(client_error),
                         client.last_error_message().c_str());
            continue;
        }
        std::unique_ptr<Session> server;
        if (!AcceptServer(&link, RealBaseConfig(), &server)) {
            ++failures;
            std::fprintf(stderr, "[SCENARIO] TCP round %d: accept failed\n", round);
            continue;
        }
        const SessionError server_error = server->Start();
        if (server_error != SessionError::kNone) {
            ++failures;
            std::fprintf(stderr, "[SCENARIO] TCP round %d: server start %s (%s)\n", round,
                         go2cpp::bento::app::SessionErrorName(server_error),
                         server->last_error_message().c_str());
            continue;
        }
        if (!RealPump(client, *server, 5000, [&] {
                return client.state() == SessionState::kActive &&
                       server->state() == SessionState::kActive;
            })) {
            ++failures;
            std::fprintf(
                stderr, "[SCENARIO] TCP round %d: not active client=%s(%s:%s) server=%s(%s:%s)\n",
                round, go2cpp::bento::app::SessionStateName(client.state()),
                go2cpp::bento::app::SessionErrorName(client.last_error()),
                client.last_error_message().c_str(),
                go2cpp::bento::app::SessionStateName(server->state()),
                go2cpp::bento::app::SessionErrorName(server->last_error()),
                server->last_error_message().c_str());
            continue;
        }
        std::size_t received = 0;
        server->on_data = [&received](std::uint32_t, const std::uint8_t*, std::size_t) {
            ++received;
        };
        if (client.Send("tcp-reconnect") != SessionError::kNone) {
            ++failures;
        }
        if (!RealPump(client, *server, 3000, [&] { return received == 1; })) {
            ++failures;
            std::fprintf(stderr, "[SCENARIO] TCP round %d: data not received\n", round);
        }
        client.Close();
        if (!RealPump(client, *server, 3000, [&] {
                return client.state() == SessionState::kClosed &&
                       server->state() == SessionState::kClosed;
            })) {
            ++failures;
            std::fprintf(stderr, "[SCENARIO] TCP round %d: close not finished client=%s server=%s\n",
                         round, go2cpp::bento::app::SessionStateName(client.state()),
                         go2cpp::bento::app::SessionStateName(server->state()));
        }
        ++checked;
    }
    const int fd_after = FdCount();
    CHECK_EQ(failures, 0L);
    CHECK(checked > 0);
    std::printf("[SCENARIO] TCP 重连 %ld/%d 轮，failures=%ld，fd %d -> %d\n", checked, rounds,
                failures, fd_before, fd_after);
    CHECK_EQ(fd_after, fd_before);
}
void ScenarioReconnect(double scale) {
    std::printf("\n[SCENARIO] 4. 重连（断开 -> 重建 -> Active）\n");
    RunReconnectMemory(std::max(20, static_cast<int>(200.0 * scale)));
    RunReconnectTcp(std::max(1, static_cast<int>(50.0 * scale)));
}

// ---------------------------------------------------------------------------
// 场景 5：60s 长稳（反复建连/断开）
// ---------------------------------------------------------------------------

void ScenarioSoak(int seconds) {
    std::printf("\n[SCENARIO] 5. 长稳 %d s（反复建连/断开）\n", seconds);
    const auto deadline = Clock::now() + std::chrono::seconds(seconds);
    const std::size_t rss_before = RssKb();
    const int fd_before = FdCount();
    std::size_t rss_peak = rss_before;
    long rounds = 0;
    long failures = 0;
    long transitions = 0;
    FakeClock clock;
    while (Clock::now() < deadline) {
        PairConfigs configs = MakePairConfigs(&clock, MemoryTransport::CreatePair());
        Session client(std::move(configs.client));
        Session server(std::move(configs.server));
        Guard guard;
        guard.Attach(&client);
        guard.Attach(&server);
        std::size_t received = 0;
        server.on_data = [&received](std::uint32_t, const std::uint8_t*, std::size_t) {
            ++received;
        };
        if (client.Start() != SessionError::kNone || server.Start() != SessionError::kNone) {
            ++failures;
            continue;
        }
        if (!PumpBoth(client, server, 50, [&] {
                return client.state() == SessionState::kActive &&
                       server.state() == SessionState::kActive;
            })) {
            ++failures;
            continue;
        }
        if (client.Send("soak") != SessionError::kNone) {
            ++failures;
        }
        if (!PumpBoth(client, server, 50, [&] { return received == 1; })) {
            ++failures;
        }
        client.Close();
        if (!PumpBoth(client, server, 50, [&] {
                return client.state() == SessionState::kClosed &&
                       server.state() == SessionState::kClosed;
            })) {
            ++failures;
        }
        transitions += guard.transitions;
        if (guard.illegal != 0 || guard.errors != 0) {
            ++failures;
        }
        ++rounds;
        if ((rounds % 200) == 0) {
            rss_peak = std::max(rss_peak, RssKb());
        }
    }
    const std::size_t rss_after = RssKb();
    const int fd_after = FdCount();
    std::printf("[SCENARIO] rounds=%ld transitions=%ld failures=%ld\n", rounds, transitions,
                failures);
    std::printf("[SCENARIO] RSS %zu -> %zu（峰值 %zu）KiB；fd %d -> %d\n", rss_before, rss_after,
                rss_peak, fd_before, fd_after);
    CHECK_EQ(failures, 0L);
    CHECK(rounds > 0);
    CHECK_EQ(fd_after, fd_before);
    if (!kSanitizerBuild) {
        CHECK(rss_after < rss_before + 16 * 1024);
    }
}

// ---------------------------------------------------------------------------
// 主入口
// ---------------------------------------------------------------------------

std::string ArgValue(int argc, char** argv, const char* flag, const char* fallback) {
    const std::string prefix = std::string(flag) + "=";
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument.rfind(prefix, 0) == 0) {
            return argument.substr(prefix.size());
        }
        if (argument == flag && index + 1 < argc) {
            return argv[index + 1];
        }
    }
    return fallback;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string mode = ArgValue(argc, argv, "--mode", "all");
    const double scale = std::max(0.001, std::atof(ArgValue(argc, argv, "--scale", "1.0").c_str()));
    const std::uint32_t seed = static_cast<std::uint32_t>(
        std::strtoul(ArgValue(argc, argv, "--seed", "20261029").c_str(), nullptr, 10));
    const int seconds = std::max(1, std::atoi(ArgValue(argc, argv, "--seconds", "60").c_str()));

    std::printf("[INFO] mode=%s scale=%.3f seed=%u seconds=%d\n", mode.c_str(), scale, seed,
                seconds);
    std::printf("[INFO] RSS=%zuKiB fd=%d\n", RssKb(), FdCount());
    const auto begin = Clock::now();

    if (mode == "bench" || mode == "all") {
        BenchThousandSessions(scale);
        BenchStateTransitions(scale);
    }
    if (mode == "random" || mode == "all") {
        RunRandom(scale, seed);
    }
    if (mode == "scenarios" || mode == "all") {
        ScenarioConnectRefused();
        ScenarioPairingTimeout();
        ScenarioClosingInflight();
        ScenarioReconnect(scale);
        if (mode == "all" && scale >= 1.0) {
            ScenarioSoak(seconds);
        }
    }
    if (mode == "soak") {
        ScenarioSoak(seconds);
    }
    if (mode == "leaks") {
        ScenarioClosingInflight();
        RunReconnectMemory(std::max(20, static_cast<int>(100.0 * scale)));
    }

    const double elapsed = SecondsSince(begin);
    std::printf("\n[SUMMARY] checks=%ld failures=%ld elapsed=%.2fs peak_rss=%zuKiB\n", g_checks,
                g_failures, elapsed, RssKb());
    if (g_failures != 0) {
        std::fprintf(stderr, "[FAIL] %ld 处断言失败\n", g_failures);
        return 1;
    }
    std::printf("[PASS] audit_session_stress\n");
    return 0;
}
