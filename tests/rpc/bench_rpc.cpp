/**
 * @file bench_rpc.cpp
 * @brief RPC 性能基准：协议层 1e7、序列化下限、TCP echo 下限、1e6 延迟、前/后对比。
 *
 * @details 输出为 Markdown 片段，直接贴进 `docs/modules/rpc.md` 与 `/tmp/rpc.md`。
 *          规模可用环境变量调整：
 *          - `RPC_BENCH_ITERS`：RPC 同步调用次数（默认 1e6）；
 *          - `RPC_BENCH_PROTOCOL_ITERS`：协议层次数（默认 1e7）；
 *          - `RPC_BENCH_SERIALIZE_ITERS`：序列化次数（默认 1e7）；
 *          - `RPC_BENCH_LEGACY_ITERS`：每次新建连接的对比次数（默认 1e4）。
 *
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include "rpc_test_support.hpp"

#include <sys/resource.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "go2cpp/frame/frame_codec.hpp"
#include "go2cpp/rpc/client.hpp"
#include "go2cpp/rpc/server.hpp"
#include "go2cpp/rpc/transport.hpp"

namespace {

using namespace go2cpp;
using namespace rpctest;

/// @brief 防止优化器把基准循环消掉。
volatile std::uint64_t g_sink = 0;

/// @brief 打印一行延迟表（8 列，与表头一致）。
void PrintLatency(const char* name, const LatencySummary& summary, std::size_t count,
                  double wall_ms) {
    std::printf("| %s | %zu | %.1f | %.1f | %.1f | %.1f | %.1f | %.0f |\n", name, count,
                summary.p50_us, summary.p90_us, summary.p99_us, summary.p999_us,
                summary.max_us, static_cast<double>(count) * 1e3 / (wall_ms > 0 ? wall_ms : 1.0));
}

// ---------------------------------------------------------------------------
// 序列化下限：Serialize + 一次拷贝 + Deserialize
// ---------------------------------------------------------------------------

void BenchSerialize(std::int64_t iterations) {
    std::printf("\n### 序列化下限（%lld 次）\n\n", static_cast<long long>(iterations));
    UserId value{42};
    thread_local reflect::Buffer encode_buffer;
    thread_local reflect::Buffer decode_buffer;
    std::uint64_t sink = 0;
    const double start = NowMicros();
    for (std::int64_t index = 0; index < iterations; ++index) {
        encode_buffer.Clear();
        if (!reflect::Serialize(value, encode_buffer)) {
            break;
        }
        decode_buffer.Clear();
        decode_buffer.PutBytes(encode_buffer.data().data(), encode_buffer.size());
        UserId decoded;
        if (!reflect::Deserialize(decode_buffer, decoded)) {
            break;
        }
        value.id = decoded.id + 1;
        sink += static_cast<std::uint64_t>(decoded.id);
    }
    const double elapsed = NowMicros() - start;
    g_sink = sink;
    std::printf("| 项目 | 总耗时 | ns/op | 有效载荷 | 等效吞吐 |\n|---|---|---|---|---|\n");
    std::printf("| Serialize<UserId> + copy + Deserialize | %.1f ms | %.1f | %zu B | %.1f MB/s |\n",
                elapsed / 1000.0, elapsed * 1000.0 / static_cast<double>(iterations),
                encode_buffer.size(),
                static_cast<double>(iterations) * static_cast<double>(encode_buffer.size()) /
                    (elapsed / 1e6) / 1e6);
    std::fflush(stdout);
}

// ---------------------------------------------------------------------------
// 协议层：请求编码 → 帧编码（含 CRC32）→ 半包重组 → 请求解码
// ---------------------------------------------------------------------------

void BenchProtocol(std::int64_t iterations) {
    std::printf("\n### 协议层（%lld 次完整线上路径）\n\n", static_cast<long long>(iterations));
    UserId id{42};
    reflect::Buffer args;
    reflect::Serialize(id, args);

    frame::FrameCodec codec(
        frame::MakeFrameSpec(frame::MakeMagic("G2RP"), rpc::kProtocolVersion, 4 * 1024 * 1024));
    std::vector<std::uint8_t> payload;
    std::vector<std::uint8_t> wire;
    std::vector<frame::Frame> frames;
    frame::Frame frame;
    frame.type = static_cast<std::uint8_t>(rpc::FrameType::kRequest);
    rpc::RequestMessage message;
    frame::Error error;
    std::string decode_error;
    std::size_t consumed = 0;
    std::uint64_t sink = 0;
    std::int64_t failures = 0;
    const double start = NowMicros();
    for (std::int64_t index = 0; index < iterations; ++index) {
        if (!rpc::EncodeRequest(payload, static_cast<std::uint64_t>(index), "User.Get", args)) {
            ++failures;
            break;
        }
        frame.payload = payload;
        if (!codec.Encode(frame, &wire, &error)) {
            ++failures;
            break;
        }
        frames.clear();
        if (!codec.Feed(wire.data(), wire.size(), &frames, &consumed, &error) ||
            frames.size() != 1) {
            ++failures;
            break;
        }
        if (!rpc::DecodeRequest(frames[0], &message, &decode_error)) {
            ++failures;
            break;
        }
        sink += message.call_id;
    }
    const double elapsed = NowMicros() - start;
    g_sink = sink;
    const double ns_per_op = elapsed * 1000.0 / static_cast<double>(iterations);
    std::printf("| 项目 | 总耗时 | ns/op | 线上字节 | 等效吞吐 |\n|---|---|---|---|---|\n");
    std::printf("| EncodeRequest + FrameCodec(CRC32) + Feed + DecodeRequest | %.1f ms | %.1f | %zu B | %.1f MB/s |\n",
                elapsed / 1000.0, ns_per_op, wire.size(),
                static_cast<double>(wire.size()) /
                    (ns_per_op / 1e9) / 1e6);
    std::printf("\n- 失败：%lld\n", static_cast<long long>(failures));
    std::fflush(stdout);
}

// ---------------------------------------------------------------------------
// 网络下限：TCP echo（1 字节往返），只含 syscall 与内核回环
// ---------------------------------------------------------------------------

void BenchTcpEcho(std::int64_t iterations) {
    std::printf("\n### 网络下限：TCP echo RTT（%lld 次，1 字节）\n\n",
                static_cast<long long>(iterations));
    rpc::Endpoint endpoint;
    endpoint.host = "127.0.0.1";
    endpoint.port = 0;
    rpc::Status status;
    auto listener = rpc::TcpListener::Bind(endpoint, 16, &status);
    if (!listener) {
        std::printf("- 监听失败：%s\n", status.ToString().c_str());
        return;
    }
    std::atomic<bool> running{true};
    std::atomic<bool> ready{false};
    std::thread echo_thread([&listener, &running, &ready]() {
        rpc::Status accept_status;
        auto connection = listener->Accept(2000, &accept_status);
        if (!connection) {
            ready.store(true);
            return;
        }
        ready.store(true);
        while (running.load(std::memory_order_acquire)) {
            std::uint8_t byte = 0;
            rpc::Status read_status;
            const std::ptrdiff_t received = connection->Read(&byte, 1, &read_status);
            if (received <= 0) {
                break;
            }
            rpc::Status write_status;
            if (!connection->WriteAll(&byte, 1, &write_status)) {
                break;
            }
        }
    });

    rpc::Endpoint target;
    target.host = "127.0.0.1";
    target.port = listener->port();
    auto client = rpc::TcpTransport::Connect(target, std::chrono::milliseconds(1000), &status);
    if (!client) {
        std::printf("- 连接失败：%s\n", status.ToString().c_str());
        running.store(false);
        listener->Close();
        echo_thread.join();
        return;
    }

    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(iterations));
    const double start = NowMicros();
    std::uint8_t byte = 0x5a;
    for (std::int64_t index = 0; index < iterations; ++index) {
        const double begin = NowMicros();
        if (!client->WriteAll(&byte, 1, &status)) {
            break;
        }
        if (client->Read(&byte, 1, &status) != 1) {
            break;
        }
        samples.push_back(NowMicros() - begin);
    }
    const double elapsed = NowMicros() - start;
    const LatencySummary summary = Summarize(samples);
    std::printf("| 样本 | 数量 | p50 (µs) | p90 | p99 | p999 | max | 吞吐 (次/s) |\n");
    std::printf("|---|---|---|---|---|---|---|\n");
    PrintLatency("TCP echo 1B", summary, samples.size(), elapsed / 1000.0);
    std::fflush(stdout);

    client.reset();
    running.store(false);
    listener->Close();
    echo_thread.join();
}

// ---------------------------------------------------------------------------
// RPC 1e6：复用连接
// ---------------------------------------------------------------------------

void BenchRpc(const std::string& address, std::int64_t iterations) {
    std::printf("\n### RPC 同步调用（复用连接，%lld 次）\n\n",
                static_cast<long long>(iterations));
    rpc::Client client(address);
    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(iterations));
    std::int64_t failures = 0;
    const double start = NowMicros();
    for (std::int64_t index = 0; index < iterations; ++index) {
        UserId id;
        id.id = index;
        const double begin = NowMicros();
        User user;
        const rpc::Status status = client.TryCall("User.Get", &user, id);
        const double end = NowMicros();
        if (!status.ok() || user.id != id.id) {
            ++failures;
            continue;
        }
        samples.push_back(end - begin);
    }
    const double elapsed = NowMicros() - start;
    const LatencySummary summary = Summarize(samples);
    std::printf("| 样本 | 数量 | p50 (µs) | p90 | p99 | p999 | max | 吞吐 (次/s) |\n");
    std::printf("|---|---|---|---|---|---|---|\n");
    PrintLatency("rpc Call<User>", summary, samples.size(), elapsed / 1000.0);
    std::printf("\n- 失败：%lld，均值 %.1f µs\n", static_cast<long long>(failures),
                summary.mean_us);
    std::fflush(stdout);
}

// ---------------------------------------------------------------------------
// 前/后对比：每次新建连接（旧做法） vs 复用连接
// ---------------------------------------------------------------------------

void BenchLegacy(const std::string& address, std::int64_t iterations) {
    std::printf("\n### 前/后对比（每次新建连接 vs 连接复用，各 %lld 次）\n\n",
                static_cast<long long>(iterations));
    std::vector<double> legacy_samples;
    legacy_samples.reserve(static_cast<std::size_t>(iterations));
    std::int64_t legacy_failures = 0;
    const double start = NowMicros();
    for (std::int64_t index = 0; index < iterations; ++index) {
        const double begin = NowMicros();
        rpc::Client client(address);
        User user;
        const rpc::Status status = client.TryCall("User.Get", &user, UserId{index});
        const double elapsed = NowMicros() - begin;
        if (!status.ok()) {
            ++legacy_failures;
            continue;
        }
        legacy_samples.push_back(elapsed);
    }
    const double legacy_elapsed = NowMicros() - start;

    // 给服务端 accept 线程一点时间回收 legacy 阶段建立的连接（每 20 ms 一次）。
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    std::vector<double> pooled_samples;
    pooled_samples.reserve(static_cast<std::size_t>(iterations));
    std::int64_t pooled_failures = 0;
    rpc::Client client(address);
    if (!client.connected()) {
        std::printf("- 池化客户端连接失败: %s\n", client.last_error().ToString().c_str());
        return;
    }
    const double pooled_start = NowMicros();
    for (std::int64_t index = 0; index < iterations; ++index) {
        const double begin = NowMicros();
        User user;
        const rpc::Status status = client.TryCall("User.Get", &user, UserId{index});
        const double elapsed = NowMicros() - begin;
        if (!status.ok()) {
            ++pooled_failures;
            continue;
        }
        pooled_samples.push_back(elapsed);
    }
    const double pooled_elapsed = NowMicros() - pooled_start;

    const LatencySummary legacy = Summarize(legacy_samples);
    const LatencySummary pooled = Summarize(pooled_samples);
    std::printf("- legacy：成功 %zu 次，失败 %lld 次，耗时 %.1f ms\n", legacy_samples.size(),
                static_cast<long long>(legacy_failures), legacy_elapsed / 1000.0);
    std::printf("| 路径 | 成功样本 | p50 (µs) | p99 (µs) | 均值 (µs) | 总耗时 (ms) |\n");
    std::printf("|---|---|---|---|---|---|\n");
    std::printf("| 前：每次新建 TCP 连接 | %zu | %.1f | %.1f | %.1f | %.1f |\n",
                legacy_samples.size(), legacy.p50_us, legacy.p99_us, legacy.mean_us,
                legacy_elapsed / 1000.0);
    std::printf("| 后：连接复用 + call_id 多路复用 | %zu | %.1f | %.1f | %.1f | %.1f |\n",
                pooled_samples.size(), pooled.p50_us, pooled.p99_us, pooled.mean_us,
                pooled_elapsed / 1000.0);
    if (pooled.mean_us > 0.0) {
        std::printf("\n- 均值加速：%.1fx；失败（前/后）：%lld / %lld\n",
                    legacy.mean_us / pooled.mean_us,
                    static_cast<long long>(legacy_failures),
                    static_cast<long long>(pooled_failures));
    }
    std::fflush(stdout);
}

}  // namespace

int main() {
    // 前/后对比会连续建立上万条短连接，先把 fd 软上限抬到硬上限。
    rlimit file_limit{};
    if (getrlimit(RLIMIT_NOFILE, &file_limit) == 0) {
        file_limit.rlim_cur = file_limit.rlim_max;
        (void)setrlimit(RLIMIT_NOFILE, &file_limit);
    }
    std::printf("# RPC 性能基准（%s）\n", __DATE__);

    rpc::ServerOptions server_options;
    // 前/后对比会建立数千条短连接（每条 2 个服务端线程）；上限放宽但仍要
    // 控制在线程/资源限制内，避免 pthread_create 失败。
    server_options.max_connections = 4096;
    rpc::Server server(server_options);
    UserService service;
    std::string address;
    if (!StartTestServer(server, service, &address)) {
        std::printf("服务端启动失败\n");
        return 1;
    }
    std::printf("\n- 地址：%s\n", address.c_str());

    const char* only = std::getenv("RPC_BENCH_ONLY");
    const auto want = [only](const char* name) {
        return only == nullptr || std::strcmp(only, name) == 0;
    };
    if (want("serialize")) {
        BenchSerialize(EnvInt("RPC_BENCH_SERIALIZE_ITERS", 10000000));
    }
    if (want("protocol")) {
        BenchProtocol(EnvInt("RPC_BENCH_PROTOCOL_ITERS", 10000000));
    }
    if (want("echo")) {
        BenchTcpEcho(EnvInt("RPC_BENCH_ECHO_ITERS", 10000));
    }
    if (want("rpc")) {
        BenchRpc(address, EnvInt("RPC_BENCH_ITERS", 1000000));
    }
    if (want("legacy")) {
        BenchLegacy(address, EnvInt("RPC_BENCH_LEGACY_ITERS", 2000));
    }

    server.Stop();
    return 0;
}
