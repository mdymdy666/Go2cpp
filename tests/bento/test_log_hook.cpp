/**
 * @file test_log_hook.cpp
 * @brief 日志钩子测试：级别/计数/回调、并发 Emit、PeerHub 连接/重传/丢包事件。
 * @details 覆盖场景矩阵 12.5（NFR-03 / RISK-076 / AX-024）：
 *          - 场景 1：级别过滤、按级别/事件计数、Reset、卸载回调。
 *          - 场景 2：8 线程并发 Emit 4000 条，计数与回调条数一致（无丢更新）。
 *          - 场景 3：PeerHub 连接建立 → kPeerConnected。
 *          - 场景 4：HELLO 重传（对端不存在）→ kHandshakeRetry。
 *          - 场景 5：可靠层 PTO 重传/丢包 → kRetransmit + kPacketLost。
 *          - 场景 6：peer 超时 → kPeerTimeout；错误 PSK → kAuthFailure。
 *          - 场景 7：sink 内再次 Emit 被重入守卫丢弃并计数，不递归（LOGH-01）。
 *          - 场景 8：PeerHub 持锁调用 sink，sink 内 RemovePeer 不递归/不死锁。
 * @author go2cpp contributors
 * @date 2026-10-27
 * @copyright MIT License
 */

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "go2cpp/bento/net/log_hook.hpp"
#include "go2cpp/bento/net/peer_hub.hpp"
#include "test_util.hpp"

namespace {

using go2cpp::bento::LogEvent;
using go2cpp::bento::LogEventName;
using go2cpp::bento::LogHook;
using go2cpp::bento::LogLevel;
using go2cpp::bento::LogLevelName;
using go2cpp::bento::LogRecord;
using go2cpp::bento::PeerHub;
using go2cpp::bento::PeerHubConfig;

PeerHubConfig MakeConfig(const char* address) {
    PeerHubConfig config;
    // 本文件聚焦明文链路行为（加密路径由独立测试覆盖），显式关闭默认策略。
    config.require_encryption = false;
    config.bind_address = address;
    config.bind_port = 0;
    config.heartbeat_interval_ms = 100;
    config.peer_timeout_ms = 5000;
    config.max_peers = 8;
    config.accept_unknown_peers = true;
    return config;
}

bool PumpHubs(const std::vector<PeerHub*>& hubs, const std::function<bool()>& done, int timeout_ms) {
    const std::uint64_t deadline =
        go2cpp::bento::NowMs() + static_cast<std::uint64_t>(timeout_ms);
    while (go2cpp::bento::NowMs() < deadline) {
        for (PeerHub* hub : hubs) {
            hub->RunOnce(1);
        }
        if (done()) {
            return true;
        }
    }
    return done();
}

void PumpFor(const std::vector<PeerHub*>& hubs, int duration_ms) {
    const std::uint64_t deadline =
        go2cpp::bento::NowMs() + static_cast<std::uint64_t>(duration_ms);
    while (go2cpp::bento::NowMs() < deadline) {
        for (PeerHub* hub : hubs) {
            hub->RunOnce(1);
        }
    }
}

/// 场景 1：级别过滤、计数、Reset 与卸载。
void TestBasics() {
    LogHook hook;
    BENTO_CHECK(!hook.has_sink());
    BENTO_CHECK_EQ(hook.min_level(), LogLevel::kInfo);
    hook.Emit(LogLevel::kInfo, LogEvent::kRetransmit, 1, "no sink");
    BENTO_CHECK_EQ(hook.Stats().filtered, 1U);
    BENTO_CHECK_EQ(hook.Stats().emitted, 0U);

    std::vector<LogRecord> records;
    std::vector<std::string> messages;
    hook.SetSink([&](const LogRecord& record) {
        records.push_back(record);
        messages.emplace_back(record.message != nullptr ? record.message : "");
    });
    BENTO_CHECK(hook.has_sink());
    hook.SetMinLevel(LogLevel::kWarn);
    hook.Info(LogEvent::kPeerConnected, 1, "filtered info");
    hook.Warn(LogEvent::kPacketLost, 7, "loss event", 3);
    hook.Error(LogEvent::kSendError, 7, "send error", 5);
    BENTO_CHECK_EQ(records.size(), static_cast<std::size_t>(2));
    BENTO_CHECK_EQ(messages.size(), static_cast<std::size_t>(2));
    if (records.size() == 2) {
        BENTO_CHECK_EQ(records[0].level, LogLevel::kWarn);
        BENTO_CHECK_EQ(records[0].event, LogEvent::kPacketLost);
        BENTO_CHECK_EQ(records[0].peer_id, 7U);
        BENTO_CHECK_EQ(records[0].value, 3U);
        BENTO_CHECK(records[0].timestamp_ms > 0);
        BENTO_CHECK_EQ(messages[0], std::string("loss event"));
        BENTO_CHECK_EQ(records[1].level, LogLevel::kError);
        BENTO_CHECK_EQ(records[1].value, 5U);
    }
    const auto stats = hook.Stats();
    BENTO_CHECK_EQ(stats.emitted, 2U);
    BENTO_CHECK_EQ(stats.filtered, 2U);  // 第一次无 sink + 一条 Info
    BENTO_CHECK_EQ(stats.per_level[static_cast<std::size_t>(LogLevel::kWarn)], 1U);
    BENTO_CHECK_EQ(stats.per_level[static_cast<std::size_t>(LogLevel::kError)], 1U);
    BENTO_CHECK_EQ(stats.per_event[static_cast<std::size_t>(LogEvent::kPacketLost)], 1U);
    BENTO_CHECK_EQ(stats.per_event[static_cast<std::size_t>(LogEvent::kSendError)], 1U);
    BENTO_CHECK_EQ(hook.Count(LogLevel::kWarn), 1U);
    BENTO_CHECK_EQ(hook.Count(LogEvent::kSendError), 1U);
    BENTO_CHECK_EQ(hook.Count(LogEvent::kRetransmit), 0U);

    hook.ResetStats();
    BENTO_CHECK_EQ(hook.Stats().emitted, 0U);
    BENTO_CHECK_EQ(hook.Stats().filtered, 0U);
    // Reset 不影响 sink / 级别。
    BENTO_CHECK(hook.has_sink());
    BENTO_CHECK_EQ(hook.min_level(), LogLevel::kWarn);

    hook.SetSink(nullptr);
    BENTO_CHECK(!hook.has_sink());
    hook.Warn(LogEvent::kPeerTimeout, 2, "after unset");
    BENTO_CHECK_EQ(hook.Stats().filtered, 1U);
}

/// 场景 2：8 线程 × 500 条并发 Emit，计数与回调条数一致。
void TestConcurrentEmit() {
    LogHook hook;
    std::atomic<int> delivered{0};
    hook.SetSink([&](const LogRecord& record) {
        if (record.event == LogEvent::kRetransmit && record.peer_id == 3) {
            delivered.fetch_add(1, std::memory_order_relaxed);
        }
    });
    constexpr int kThreads = 8;
    constexpr int kPerThread = 500;
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&]() {
            for (int j = 0; j < kPerThread; ++j) {
                hook.Emit(LogLevel::kInfo, LogEvent::kRetransmit, 3, "concurrent", j);
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    const auto stats = hook.Stats();
    BENTO_CHECK_EQ(stats.emitted, static_cast<std::uint64_t>(kThreads * kPerThread));
    BENTO_CHECK_EQ(stats.per_event[static_cast<std::size_t>(LogEvent::kRetransmit)],
                   static_cast<std::uint64_t>(kThreads * kPerThread));
    BENTO_CHECK_EQ(stats.per_level[static_cast<std::size_t>(LogLevel::kInfo)],
                   static_cast<std::uint64_t>(kThreads * kPerThread));
    BENTO_CHECK_EQ(delivered.load(), kThreads * kPerThread);
    std::printf("[INFO] concurrent_emit: emitted=%llu delivered=%d\n",
                static_cast<unsigned long long>(stats.emitted), delivered.load());
}

/// 场景 3：连接建立触发 kPeerConnected（两端都收到）。
void TestConnectedEvent() {
    PeerHub server(MakeConfig("127.0.0.1"));
    PeerHub client(MakeConfig("127.0.0.1"));
    std::string error;
    BENTO_CHECK(server.Open(&error));
    BENTO_CHECK(client.Open(&error));
    LogHook server_hook;
    LogHook client_hook;
    std::vector<std::uint32_t> server_connected;
    std::vector<std::uint32_t> client_connected;
    server_hook.SetSink([&](const LogRecord& record) {
        if (record.event == LogEvent::kPeerConnected) {
            server_connected.push_back(record.peer_id);
        }
    });
    client_hook.SetSink([&](const LogRecord& record) {
        if (record.event == LogEvent::kPeerConnected) {
            client_connected.push_back(record.peer_id);
        }
    });
    server.SetLogHook(&server_hook);
    client.SetLogHook(&client_hook);
    BENTO_CHECK(server.log_hook() == &server_hook);
    BENTO_CHECK(client.AddPeer(1, "127.0.0.1", server.local_port(), &error));
    BENTO_CHECK(PumpHubs({&server, &client},
                         [&]() {
                             return server.peer_connected(1) && client.peer_connected(1);
                         },
                         8000));
    BENTO_CHECK_EQ(server_connected.size(), static_cast<std::size_t>(1));
    BENTO_CHECK_EQ(client_connected.size(), static_cast<std::size_t>(1));
    if (!server_connected.empty()) {
        BENTO_CHECK_EQ(server_connected.front(), 1U);
    }
    BENTO_CHECK_EQ(server_hook.Count(LogEvent::kPeerConnected), 1U);
    BENTO_CHECK_EQ(client_hook.Count(LogEvent::kPeerConnected), 1U);
    // 卸载后不再增长。
    server.SetLogHook(nullptr);
    BENTO_CHECK(server.log_hook() == nullptr);
    std::printf("[INFO] connected_event: server=%zu client=%zu\n", server_connected.size(),
                client_connected.size());
}

/// 场景 4：对端不存在时 HELLO 重传触发 kHandshakeRetry。
void TestHandshakeRetryEvent() {
    // 先占用一个端口再关闭，确保目标端口无人监听（UDP 不会 RST）。
    std::uint16_t dead_port = 0;
    {
        PeerHub probe(MakeConfig("127.0.0.1"));
        std::string error;
        BENTO_CHECK(probe.Open(&error));
        dead_port = probe.local_port();
    }
    BENTO_CHECK(dead_port != 0);

    PeerHubConfig config = MakeConfig("127.0.0.1");
    config.handshake_timeout_ms = 1200;
    config.hello_initial_pto_ms = 50;
    config.hello_max_pto_ms = 100;
    config.hello_max_attempts = 8;
    PeerHub client(config);
    std::string error;
    BENTO_CHECK(client.Open(&error));
    LogHook hook;
    hook.SetMinLevel(LogLevel::kDebug);
    hook.SetSink([](const LogRecord&) {});
    client.SetLogHook(&hook);
    BENTO_CHECK(client.AddPeer(1, "127.0.0.1", dead_port, &error));
    PumpFor({&client}, 700);
    BENTO_CHECK(hook.Count(LogEvent::kHandshakeRetry) > 0);
    BENTO_CHECK(!client.peer_connected(1));
    std::printf("[INFO] handshake_retry: count=%llu\n",
                static_cast<unsigned long long>(hook.Count(LogEvent::kHandshakeRetry)));
}

/// 场景 5：PTO 重传与丢包事件（服务端暂停一段时间）。
void TestRetransmitAndLossEvents() {
    PeerHub server(MakeConfig("127.0.0.1"));
    PeerHub client(MakeConfig("127.0.0.1"));
    std::string error;
    BENTO_CHECK(server.Open(&error));
    BENTO_CHECK(client.Open(&error));
    LogHook hook;
    hook.SetMinLevel(LogLevel::kDebug);
    hook.SetSink([](const LogRecord&) {});
    client.SetLogHook(&hook);
    int received = 0;
    server.on_stream = [&](std::uint32_t, std::uint8_t, const std::uint8_t*, std::size_t) {
        ++received;
    };
    BENTO_CHECK(client.AddPeer(1, "127.0.0.1", server.local_port(), &error));
    BENTO_CHECK(PumpHubs({&server, &client},
                         [&]() {
                             return server.peer_connected(1) && client.peer_connected(1);
                         },
                         8000));
    hook.ResetStats();

    constexpr std::uint32_t kTotal = 20;
    std::vector<std::uint8_t> payload(64, 0x5A);
    for (std::uint32_t seq = 0; seq < kTotal; ++seq) {
        payload[0] = static_cast<std::uint8_t>(seq);
        BENTO_CHECK(client.SendStream(1, 0, payload.data(), payload.size()));
    }
    // 服务端不处理：首批全部丢失，客户端 PTO 重传并降窗。
    PumpFor({&client}, 1500);
    BENTO_CHECK(hook.Count(LogEvent::kRetransmit) > 0);
    BENTO_CHECK(hook.Count(LogEvent::kPacketLost) > 0);
    // 服务端恢复后仍全量送达（重传帧去重）。
    BENTO_CHECK(PumpHubs({&server, &client},
                         [&]() { return received >= static_cast<int>(kTotal); }, 20000));
    BENTO_CHECK_EQ(received, static_cast<int>(kTotal));
    std::printf("[INFO] retransmit_events: retransmit=%llu loss=%llu rx=%d\n",
                static_cast<unsigned long long>(hook.Count(LogEvent::kRetransmit)),
                static_cast<unsigned long long>(hook.Count(LogEvent::kPacketLost)), received);
}

/// 场景 6：peer 超时事件；错误 PSK 触发认证失败事件。
void TestTimeoutAndAuthEvents() {
    PeerHubConfig server_config = MakeConfig("127.0.0.1");
    server_config.peer_timeout_ms = 300;
    server_config.heartbeat_interval_ms = 100;
    PeerHub server(server_config);
    PeerHub client(MakeConfig("127.0.0.1"));
    std::string error;
    BENTO_CHECK(server.Open(&error));
    BENTO_CHECK(client.Open(&error));
    LogHook server_hook;
    server_hook.SetSink([](const LogRecord&) {});
    server.SetLogHook(&server_hook);
    BENTO_CHECK(client.AddPeer(1, "127.0.0.1", server.local_port(), &error));
    BENTO_CHECK(PumpHubs({&server, &client},
                         [&]() { return server.peer_connected(1); }, 8000));
    // 客户端不再运行：服务端按 peer_timeout 判死。
    PumpFor({&server}, 900);
    BENTO_CHECK(server_hook.Count(LogEvent::kPeerTimeout) > 0);
    // PEB-04：被动 peer 超时后已从表中回收。
    BENTO_CHECK_EQ(server.peer_state(1), go2cpp::bento::PeerState::kClosed);

    // 错误 PSK：服务端 AEAD 失败 → kAuthFailure。
    PeerHubConfig secure_server_config = MakeConfig("127.0.0.1");
    secure_server_config.pre_shared_key.assign(32, 0x31);
    secure_server_config.handshake_timeout_ms = 600;
    secure_server_config.hello_initial_pto_ms = 50;
    secure_server_config.hello_max_pto_ms = 100;
    PeerHub secure_server(secure_server_config);
    BENTO_CHECK(secure_server.Open(&error));
    LogHook secure_hook;
    secure_hook.SetSink([](const LogRecord&) {});
    secure_server.SetLogHook(&secure_hook);
    PeerHubConfig bad_client_config = MakeConfig("127.0.0.1");
    bad_client_config.pre_shared_key.assign(32, 0x32);
    bad_client_config.handshake_timeout_ms = 600;
    bad_client_config.hello_initial_pto_ms = 50;
    bad_client_config.hello_max_pto_ms = 100;
    PeerHub bad_client(bad_client_config);
    BENTO_CHECK(bad_client.Open(&error));
    BENTO_CHECK(bad_client.AddPeer(1, "127.0.0.1", secure_server.local_port(), &error));
    PumpFor({&secure_server, &bad_client}, 900);
    BENTO_CHECK(secure_hook.Count(LogEvent::kAuthFailure) > 0);
    std::printf("[INFO] timeout_auth_events: timeout=%llu auth_failure=%llu\n",
                static_cast<unsigned long long>(server_hook.Count(LogEvent::kPeerTimeout)),
                static_cast<unsigned long long>(secure_hook.Count(LogEvent::kAuthFailure)));
}

/// 场景 7（LOGH-01）：sink 内再次 Emit 被丢弃并计数，不递归。
void TestReentrantSink() {
    LogHook hook;
    std::atomic<int> sink_calls{0};
    std::atomic<int> nested_attempts{0};
    hook.SetSink([&](const LogRecord& record) {
        sink_calls.fetch_add(1);
        // 转发型 sink：每条记录再 Emit 一次（旧实现会无限递归到栈溢出）。
        for (int i = 0; i < 3; ++i) {
            nested_attempts.fetch_add(1);
            hook.Emit(LogLevel::kInfo, LogEvent::kRetransmit, record.peer_id, "nested", i);
        }
    });
    hook.Info(LogEvent::kPeerConnected, 9, "outer");
    BENTO_CHECK_EQ(sink_calls.load(), 1);
    BENTO_CHECK_EQ(nested_attempts.load(), 3);
    const auto stats = hook.Stats();
    BENTO_CHECK_EQ(stats.emitted, 1U);
    BENTO_CHECK_EQ(stats.reentrant_dropped, 3U);
    BENTO_CHECK_EQ(stats.filtered, 0U);
    BENTO_CHECK_EQ(stats.per_event[static_cast<std::size_t>(LogEvent::kRetransmit)], 0U);
    // 外层回调返回后守卫复位，后续 Emit 正常。
    hook.Info(LogEvent::kPeerConnected, 9, "after");
    BENTO_CHECK_EQ(sink_calls.load(), 2);
    BENTO_CHECK_EQ(hook.Stats().emitted, 2U);

    // 守卫按线程生效：主线程在 sink 内阻塞时，另一线程的 Emit 正常送达。
    std::mutex mutex;
    std::condition_variable cv;
    bool in_sink = false;
    bool other_sent = false;
    int other_delivered = 0;
    LogHook cross_hook;
    cross_hook.SetSink([&](const LogRecord& record) {
        if (record.peer_id == 1) {
            std::unique_lock<std::mutex> lock(mutex);
            in_sink = true;
            cv.notify_all();
            cv.wait(lock, [&]() { return other_sent; });
        } else if (record.peer_id == 2) {
            std::lock_guard<std::mutex> lock(mutex);
            ++other_delivered;
        }
    });
    std::thread other([&]() {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [&]() { return in_sink; });
        lock.unlock();
        cross_hook.Info(LogEvent::kPeerConnected, 2, "other thread");
        {
            std::lock_guard<std::mutex> guard(mutex);
            other_sent = true;
        }
        cv.notify_all();
    });
    cross_hook.Info(LogEvent::kPeerConnected, 1, "main thread");
    other.join();
    BENTO_CHECK_EQ(other_delivered, 1);
    std::printf("[INFO] reentrant_sink: calls=%d dropped=%llu other_thread=%d\n", sink_calls.load(),
                static_cast<unsigned long long>(hook.Stats().reentrant_dropped), other_delivered);
}

/// 场景 8（LOGH-01 + PEB-01）：PeerHub 在持锁内调用 sink，sink 调 RemovePeer
/// 会再次触发 disconnected 日志；重入守卫必须丢弃该条且不递归/不死锁。
void TestHubSinkReentry() {
    PeerHubConfig server_config = MakeConfig("127.0.0.1");
    server_config.peer_timeout_ms = 5000;
    PeerHub server(server_config);
    PeerHub client(MakeConfig("127.0.0.1"));
    std::string error;
    BENTO_CHECK(server.Open(&error));
    BENTO_CHECK(client.Open(&error));
    LogHook hook;
    int sink_calls = 0;
    int connected_events = 0;
    hook.SetSink([&](const LogRecord& record) {
        ++sink_calls;
        if (record.event != LogEvent::kPeerConnected) {
            return;
        }
        ++connected_events;
        // 回调内删除当前 peer：RemovePeer 会再 Emit 一条 disconnected 日志，
        // 该条必须被 LogHook 的重入守卫丢弃。
        (void)server.RemovePeer(record.peer_id);
    });
    server.SetLogHook(&hook);
    BENTO_CHECK(client.AddPeer(1, "127.0.0.1", server.local_port(), &error));
    BENTO_CHECK(PumpHubs({&server, &client},
                         [&]() { return connected_events > 0; }, 8000));
    BENTO_CHECK_EQ(connected_events, 1);
    BENTO_CHECK_EQ(sink_calls, 1);
    BENTO_CHECK(hook.Stats().reentrant_dropped >= 1);
    BENTO_CHECK(!server.HasPeer(1));
    BENTO_CHECK_EQ(server.PeerIds().size(), 0U);
    std::printf("[INFO] hub_sink_reentry: sink_calls=%d dropped=%llu\n", sink_calls,
                static_cast<unsigned long long>(hook.Stats().reentrant_dropped));
}

/// 事件/级别名稳定且互不相同（可做指标标签）。
void TestNames() {
    std::vector<std::string> level_names;
    for (std::size_t i = 0; i < go2cpp::bento::kLogLevelCount; ++i) {
        level_names.emplace_back(LogLevelName(static_cast<LogLevel>(i)));
    }
    std::vector<std::string> event_names;
    for (std::size_t i = 0; i < go2cpp::bento::kLogEventCount; ++i) {
        event_names.emplace_back(LogEventName(static_cast<LogEvent>(i)));
    }
    for (const std::string& name : level_names) {
        BENTO_CHECK(!name.empty());
    }
    for (const std::string& name : event_names) {
        BENTO_CHECK(!name.empty());
        BENTO_CHECK(name != std::string("unknown"));
    }
    // 去重后数量不变。
    BENTO_CHECK_EQ(std::set<std::string>(level_names.begin(), level_names.end()).size(),
                   go2cpp::bento::kLogLevelCount);
    BENTO_CHECK_EQ(std::set<std::string>(event_names.begin(), event_names.end()).size(),
                   go2cpp::bento::kLogEventCount);
}

}  // namespace

int main() {
    TestBasics();
    TestConcurrentEmit();
    TestConnectedEvent();
    TestHandshakeRetryEvent();
    TestRetransmitAndLossEvents();
    TestTimeoutAndAuthEvents();
    TestReentrantSink();
    TestHubSinkReentry();
    TestNames();
    return bento_test::Finish("test_log_hook");
}
