/**
 * @file test_rpc.cpp
 * @brief RPC 端到端验收测试：基本调用/错误/超时/取消/并发/1e6 延迟/断连。
 *
 * @details 测试规模可用环境变量调整（默认按任务要求取满）：
 * - `RPC_ITERATIONS`：单次调用延迟样本数（默认 1e6）；
 * - `RPC_CONCURRENT_TOTAL`：并发调用总次数（默认 1e5）；
 * - `RPC_CONCURRENT_CLIENTS`：并发客户端数（默认 8）。
 *
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include "rpc_test_support.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace go2cpp;
using namespace rpctest;

// ---------------------------------------------------------------------------
// 基本调用：覆盖值返回 / out 参数 / 仅状态 / void / 多参数 / 字符串 / 字节 / 大载荷
// ---------------------------------------------------------------------------

void TestBasicCalls(const std::string& address) {
    Announce("call: basic types");
    rpc::Client client(address);
    RPC_CHECK(client.connected());

    const User user = client.Call<User>("User.Get", UserId{7});
    RPC_CHECK_EQ(user.id, static_cast<std::int64_t>(7));
    RPC_CHECK_EQ(user.name, std::string("user-7"));
    RPC_CHECK_EQ(user.level, static_cast<std::int32_t>(7));

    User created;
    RPC_CHECK(client.TryCall("User.Create", &created, CreateRequest{"carol", 5}).ok());
    RPC_CHECK_EQ(created.name, std::string("carol"));
    RPC_CHECK_EQ(created.level, static_cast<std::int32_t>(5));
    RPC_CHECK(created.id > 0);

    RPC_CHECK(client.CallStatus("User.Remove", UserId{1}).ok());
    RPC_CHECK(client.CallStatus("User.Remove", UserId{0}).code ==
              rpc::StatusCode::kNotFound);
    RPC_CHECK(client.CallStatus("User.Touch", UserId{2}).ok());

    std::int64_t sum = 0;
    RPC_CHECK(client.TryCall("User.Add", &sum, static_cast<std::int64_t>(20),
                             static_cast<std::int64_t>(22))
                  .ok());
    RPC_CHECK_EQ(sum, static_cast<std::int64_t>(42));

    std::string echoed;
    RPC_CHECK(client.TryCall("User.EchoString", &echoed, std::string("hello rpc")).ok());
    RPC_CHECK_EQ(echoed, std::string("hello rpc"));

    const std::vector<std::uint8_t> bytes{0, 1, 2, 127, 128, 255};
    std::vector<std::uint8_t> echoed_bytes;
    RPC_CHECK(client.TryCall("User.EchoBytes", &echoed_bytes, bytes).ok());
    RPC_CHECK(echoed_bytes == bytes);

    // 1 MiB 大载荷：验证帧上限内的最大规格与序列化拷贝路径。
    std::vector<std::uint8_t> big;
    RPC_CHECK(client.TryCall("User.Big", &big, BigRequest{1024 * 1024, 0x5a}).ok());
    RPC_CHECK_EQ(big.size(), static_cast<std::size_t>(1024 * 1024));
    RPC_CHECK(!big.empty() && big.front() == 0x5a && big.back() == 0x5a);

    const CounterSnapshot snapshot = client.Call<CounterSnapshot>("User.Snapshot");
    RPC_CHECK(snapshot.calls > 0);
    RPC_CHECK(snapshot.touches >= 1);

    // 便捷重载：失败抛 RpcError，且状态码可程序化判断。
    bool threw = false;
    try {
        (void)client.Call<User>("User.Missing", UserId{1});
    } catch (const rpc::RpcError& error) {
        threw = true;
        RPC_CHECK(error.status().code == rpc::StatusCode::kNotFound);
    }
    RPC_CHECK(threw);

    RPC_CHECK(client.Ping().ok());
    RPC_CHECK(client.connected());
}

// ---------------------------------------------------------------------------
// 错误路径
// ---------------------------------------------------------------------------

void TestErrorPaths(const std::string& address) {
    Announce("call: error paths");
    rpc::Client client(address);

    // 业务错误文本原样透传。
    const rpc::Status failed = client.CallStatus("User.Fail", UserId{1});
    RPC_CHECK(failed.code == rpc::StatusCode::kPermissionDenied);
    RPC_CHECK_EQ(failed.message, std::string("permission denied"));

    // 未注册的方法/服务。
    RPC_CHECK(client.CallStatus("User.NoSuchMethod").code == rpc::StatusCode::kNotFound);
    RPC_CHECK(client.CallStatus("NoSuchService.Get").code == rpc::StatusCode::kNotFound);

    // 流式方法被当成普通调用：服务端拒绝。
    RPC_CHECK(client.CallStatus("User.Watch", WatchRequest{1, 0}).code ==
              rpc::StatusCode::kInvalidArgument);

    // 普通方法被当成流打开：服务端用 StreamEnd 拒绝，客户端 Next 返回错误。
    auto invalid = client.OpenStream("User.Get", UserId{1});
    ChatMessage unused_message;
    rpc::Status stream_status;
    RPC_CHECK(!invalid.Next(&unused_message, &stream_status,
                            std::chrono::milliseconds(2000)));
    RPC_CHECK(stream_status.code == rpc::StatusCode::kInvalidArgument);

    // 参数非法（空名字）由 handler 返回 INVALID_ARGUMENT。
    User ignored;
    RPC_CHECK(client.TryCall("User.Create", &ignored, CreateRequest{"", 0}).code ==
              rpc::StatusCode::kInvalidArgument);

    // 非法 endpoint：构造后 connected() == false，调用返回 INVALID_ARGUMENT。
    rpc::Client bad_client("not-an-endpoint");
    RPC_CHECK(!bad_client.connected());
    RPC_CHECK(bad_client.CallStatus("User.Get", UserId{1}).code ==
              rpc::StatusCode::kInvalidArgument);
}

// ---------------------------------------------------------------------------
// 超时
// ---------------------------------------------------------------------------

void TestTimeout(const std::string& address) {
    Announce("call: deadline exceeded + recovery");
    rpc::Client client(address);
    client.set_timeout(std::chrono::milliseconds(5));

    const auto start = std::chrono::steady_clock::now();
    const rpc::Status status = client.CallStatus("User.Slow", UserId{1});
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - start)
                             .count();
    RPC_CHECK(status.code == rpc::StatusCode::kDeadlineExceeded);
    RPC_CHECK(elapsed >= 4);
    RPC_CHECK(elapsed < 90);  // 不能等满 handler 的 100ms。

    // 超时后连接仍可用：迟到的响应被丢弃，不串到下一次调用。
    client.set_timeout(std::chrono::milliseconds(5000));
    RPC_CHECK(client.CallStatus("User.Touch", UserId{3}).ok());
    const User user = client.Call<User>("User.Get", UserId{11});
    RPC_CHECK_EQ(user.id, static_cast<std::int64_t>(11));

    // 连续超时 20 次，等待表不泄漏，之后仍能正常调用。
    client.set_timeout(std::chrono::milliseconds(1));
    for (int index = 0; index < 20; ++index) {
        RPC_CHECK(client.CallStatus("User.Slow", UserId{1}).code ==
                  rpc::StatusCode::kDeadlineExceeded);
    }
    client.set_timeout(std::chrono::milliseconds(5000));
    RPC_CHECK(client.CallStatus("User.Touch", UserId{4}).ok());
}

// ---------------------------------------------------------------------------
// 取消
// ---------------------------------------------------------------------------

void TestCancel(const std::string& address) {
    Announce("call: async cancel");
    rpc::Client client(address);

    auto pending = client.AsyncCall<CounterSnapshot>("User.Slow", UserId{1});
    RPC_CHECK(pending.valid());
    RPC_CHECK(!pending.done());
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    RPC_CHECK(pending.Cancel());

    CounterSnapshot snapshot;
    const rpc::Status status = pending.Wait(&snapshot);
    RPC_CHECK(status.code == rpc::StatusCode::kCancelled);
    RPC_CHECK(pending.done());
    RPC_CHECK(!pending.Cancel());  // 已取消：幂等返回 false。

    // 取消后同一连接还能继续调用。
    RPC_CHECK(client.CallStatus("User.Touch", UserId{5}).ok());

    // 已完成后再取消返回 false。
    auto completed = client.AsyncCall<User>("User.Get", UserId{21});
    const User user = completed.Wait();
    RPC_CHECK_EQ(user.id, static_cast<std::int64_t>(21));
    RPC_CHECK(!completed.Cancel());
}

// ---------------------------------------------------------------------------
// 并发：多客户端 × 多线程，共享同一连接
// ---------------------------------------------------------------------------

void TestConcurrency(const std::string& address) {
    Announce("call: concurrent clients");
    const std::int64_t total = EnvInt("RPC_CONCURRENT_TOTAL", 100000);
    const int client_count = static_cast<int>(EnvInt("RPC_CONCURRENT_CLIENTS", 8));
    const int threads_per_client = 4;
    const std::int64_t per_thread = total / (client_count * threads_per_client);

    std::atomic<std::int64_t> completed{0};
    std::atomic<std::int64_t> failed{0};
    const double start = NowMicros();
    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(client_count) * threads_per_client);
    for (int client_index = 0; client_index < client_count; ++client_index) {
        auto client = std::make_shared<rpc::Client>(address);
        RPC_CHECK(client->connected());
        for (int thread_index = 0; thread_index < threads_per_client; ++thread_index) {
            threads.emplace_back([client, per_thread, client_index, thread_index, &completed,
                                  &failed]() {
                for (std::int64_t index = 0; index < per_thread; ++index) {
                    UserId id;
                    id.id = client_index * 1000000 + thread_index * 100000 + index;
                    User user;
                    const rpc::Status status = client->TryCall("User.Get", &user, id);
                    if (status.ok() && user.id == id.id) {
                        completed.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        failed.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        }
    }
    for (auto& thread : threads) {
        thread.join();
    }
    const double elapsed = NowMicros() - start;

    std::printf(
        "  clients=%d threads=%d total=%lld ok=%lld fail=%lld wall=%.1fms throughput=%.0f/s\n",
        client_count, client_count * threads_per_client, static_cast<long long>(total),
        static_cast<long long>(completed.load()), static_cast<long long>(failed.load()),
        elapsed / 1000.0, static_cast<double>(completed.load()) * 1e6 / elapsed);
    std::fflush(stdout);
    RPC_CHECK_EQ(failed.load(), static_cast<std::int64_t>(0));
    RPC_CHECK_EQ(completed.load(), per_thread * client_count * threads_per_client);
}

// ---------------------------------------------------------------------------
// 单次调用延迟：1e6 次
// ---------------------------------------------------------------------------

void TestLatency(const std::string& address) {
    Announce("call: 1e6 latency");
    const std::int64_t iterations = EnvInt("RPC_ITERATIONS", 1000000);
    rpc::Client client(address);
    RPC_CHECK(client.connected());

    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(iterations));
    std::int64_t failed = 0;
    const double start = NowMicros();
    for (std::int64_t index = 0; index < iterations; ++index) {
        UserId id;
        id.id = index;
        const double begin = NowMicros();
        User user;
        const rpc::Status status = client.TryCall("User.Get", &user, id);
        const double end = NowMicros();
        if (!status.ok() || user.id != id.id) {
            ++failed;
            continue;
        }
        samples.push_back(end - begin);
    }
    const double elapsed = NowMicros() - start;
    const LatencySummary summary = Summarize(samples);
    std::printf("  n=%lld ok=%zu fail=%lld wall=%.1fms throughput=%.0f/s\n",
                static_cast<long long>(iterations), samples.size(),
                static_cast<long long>(failed), elapsed / 1000.0,
                static_cast<double>(samples.size()) * 1e6 / elapsed);
    std::printf("  p50=%.1fus p90=%.1fus p99=%.1fus p999=%.1fus max=%.1fus mean=%.1fus\n",
                summary.p50_us, summary.p90_us, summary.p99_us, summary.p999_us,
                summary.max_us, summary.mean_us);
    std::fflush(stdout);
    RPC_CHECK_EQ(failed, static_cast<std::int64_t>(0));
    RPC_CHECK_EQ(samples.size(), static_cast<std::size_t>(iterations));
    RPC_CHECK(summary.p50_us < 500.0);
    RPC_CHECK(summary.p99_us < 5000.0);
}

// ---------------------------------------------------------------------------
// 断连
// ---------------------------------------------------------------------------

void TestDisconnect(rpc::Server& server, const std::string& address) {
    Announce("call: server shutdown");
    rpc::Client client(address);
    RPC_CHECK(client.connected());
    RPC_CHECK(client.CallStatus("User.Touch", UserId{6}).ok());

    server.Stop();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    const rpc::Status status = client.CallStatus("User.Touch", UserId{7});
    RPC_CHECK(status.code == rpc::StatusCode::kUnavailable ||
              status.code == rpc::StatusCode::kDeadlineExceeded);
    RPC_CHECK(!client.connected());

    // 已停下之后注册表冻结：再注册返回 false。
    UserService extra;
    RPC_CHECK(!server.Register("Extra", &extra));
}

}  // namespace

int main() {
    rpc::Server server;
    UserService service;
    std::string address;
    if (!StartTestServer(server, service, &address)) {
        std::printf("[rpc] 服务端启动失败\n");
        return 1;
    }
    std::printf("[rpc] server listening on %s\n", address.c_str());

    TestBasicCalls(address);
    TestErrorPaths(address);
    TestTimeout(address);
    TestCancel(address);
    TestConcurrency(address);
    TestLatency(address);
    TestDisconnect(server, address);
    return Summary("rpc");
}
