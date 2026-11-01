/**
 * @file test_relay_pipe.cpp
 * @brief `RelayPipe` 的行为、边界、随机与压测（独立 main，不并入 go2cpp_tests）。
 *
 * @details 覆盖：
 *          1. 双向 1e7 字节 byte-exact（两端 SHA-256 对照期望摘要）；
 *          2. 一端慢读 → 队列满 → 只断该端，另一条管道继续；
 *          3. 写 deadline 触发 → Timeouts+1；
 *          4. 4 条管道并发互不串流；
 *          5. 1e8 字节压测（双向各 1e8）；
 *          6. 40 轮随机规模/队列/短写/慢写；
 *          7. Context 取消 → 强制关闭两端并回收线程；
 *          8. 参数归一化、空 Stream、重复 Run、已取消 Context 的 API 边界。
 *
 *          运行：`./go2cpp_relay_test`；fail 时退出码 1，并打印文件:行号。
 *
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include "relay_test_support.hpp"

#include "go2cpp/concurrency/context.hpp"
#include "go2cpp/error.hpp"
#include "go2cpp/relay/relay_pipe.hpp"

#include <array>
#include <chrono>
#include <cstdio>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace go2cpp_relay_test;

using go2cpp::Context;
using go2cpp::ContextPtr;
using go2cpp::ErrorPtr;
using go2cpp::relay::PipeConfig;
using go2cpp::relay::PipeStats;
using go2cpp::relay::RelayPipe;

/// 把字节数与耗时换算成 MiB/s。
double MibPerSecond(std::uint64_t bytes, Duration elapsed) {
    const double seconds = static_cast<double>(elapsed.count()) / 1000.0;
    if (seconds <= 0.0) {
        return 0.0;
    }
    return static_cast<double>(bytes) / (1024.0 * 1024.0) / seconds;
}

/// 失败时打印错误文本，便于定位。
void ReportErrorIfAny(const char* where, const ErrorPtr& error) {
    if (error) {
        std::fprintf(stderr, "[relay-test] %s error: %s\n", where,
                     go2cpp::ErrorMessage(error).c_str());
    }
}

/// 校验一个方向的干净传输：源端读满、目的端写满、摘要一致、两端未被关闭。
void CheckStreamDirection(TestStream& source, TestStream& sink, std::uint64_t source_seed,
                          std::uint64_t bytes) {
    RELAY_CHECK_U64(source.read_bytes(), bytes);
    RELAY_CHECK_U64(sink.written(), bytes);
    RELAY_CHECK(sink.Digest() == PatternDigest(source_seed, bytes));
    RELAY_CHECK(!source.closed());
    RELAY_CHECK(!sink.closed());
}

// ---------------------------------------------------------------------------
// 1. 双向 1e7 字节 byte-exact
// ---------------------------------------------------------------------------
void TestBidirectional10MB() {
    std::printf("[test] 1. 双向 1e7 字节 byte-exact\n");
    std::fflush(stdout);

    constexpr std::uint64_t kBytes = 10'000'000;
    constexpr std::uint64_t kSeedA = 0xA11CEULL;
    constexpr std::uint64_t kSeedB = 0xB0B5EEDULL;

    StreamOptions a_opts;
    a_opts.seed = kSeedA;
    a_opts.inbound_bytes = kBytes;
    StreamOptions b_opts;
    b_opts.seed = kSeedB;
    b_opts.inbound_bytes = kBytes;

    TestStream a(a_opts);
    TestStream b(b_opts);
    RelayPipe pipe(&a, &b, PipeConfig{});

    const auto expected_for_a = PatternDigest(kSeedB, kBytes);
    const auto expected_for_b = PatternDigest(kSeedA, kBytes);

    ErrorPtr error;
    const TimePoint start = Clock::now();
    const bool ok = pipe.Run(&error);
    const auto elapsed = std::chrono::duration_cast<Duration>(Clock::now() - start);

    RELAY_CHECK(ok);
    RELAY_CHECK(!error);
    ReportErrorIfAny("1e7", error);
    const PipeStats stats = pipe.Stats();
    RELAY_CHECK_U64(stats.a_to_b, kBytes);
    RELAY_CHECK_U64(stats.b_to_a, kBytes);
    RELAY_CHECK_U64(stats.dropped, 0);
    RELAY_CHECK_U64(stats.timeouts, 0);
    CheckStreamDirection(a, b, kSeedA, kBytes);
    CheckStreamDirection(b, a, kSeedB, kBytes);
    RELAY_CHECK(a.Digest() == expected_for_a);
    RELAY_CHECK(b.Digest() == expected_for_b);

    std::printf("  2 x %llu bytes, %lld ms, %.1f MiB/s\n",
                static_cast<unsigned long long>(kBytes),
                static_cast<long long>(elapsed.count()),
                MibPerSecond(kBytes * 2, elapsed));
    std::fflush(stdout);
}

// ---------------------------------------------------------------------------
// 2. 一端慢读 → 队列满 → 只断该端；另一条管道继续
// ---------------------------------------------------------------------------
void TestSlowPeerQueueStall() {
    std::printf("[test] 2. 慢 peer → 队列满 → 只断该端\n");
    std::fflush(stdout);

    // 慢管道：A 有 8 MiB 待发，B 每次写入睡 30 ms（模拟读得很慢的 viewer）。
    StreamOptions slow_a_opts;
    slow_a_opts.seed = 0x5101;
    slow_a_opts.inbound_bytes = 8U * 1024U * 1024U;
    StreamOptions slow_b_opts;
    slow_b_opts.seed = 0x5102;
    slow_b_opts.inbound_bytes = 64U * 1024U;
    slow_b_opts.write_delay = std::chrono::milliseconds(30);

    TestStream slow_a(slow_a_opts);
    TestStream slow_b(slow_b_opts);
    PipeConfig slow_cfg;
    slow_cfg.copy_buf = 4096;
    slow_cfg.queue_cap = 32U * 1024U;
    slow_cfg.write_timeout = std::chrono::milliseconds(100);
    RelayPipe slow_pipe(&slow_a, &slow_b, slow_cfg);

    // 健康管道：与慢管道并发，证明慢 peer 不会拖垮另一条管道。
    StreamOptions healthy_a_opts;
    healthy_a_opts.seed = 0x5201;
    healthy_a_opts.inbound_bytes = 1U * 1024U * 1024U;
    StreamOptions healthy_b_opts;
    healthy_b_opts.seed = 0x5202;
    healthy_b_opts.inbound_bytes = 1U * 1024U * 1024U;
    TestStream healthy_a(healthy_a_opts);
    TestStream healthy_b(healthy_b_opts);
    RelayPipe healthy_pipe(&healthy_a, &healthy_b, PipeConfig{});
    AsyncRun healthy_run(&healthy_pipe, ContextPtr{});
    healthy_run.Start();

    AsyncRun slow_run(&slow_pipe, ContextPtr{});
    const TimePoint start = Clock::now();
    slow_run.Start();

    const bool closed = WaitUntil([&slow_b] { return slow_b.closed(); },
                                  std::chrono::milliseconds(5000));
    RELAY_CHECK(closed);
    slow_run.JoinOrAbort(std::chrono::milliseconds(5000), "slow pipe", __FILE__, __LINE__);
    healthy_run.JoinOrAbort(std::chrono::milliseconds(5000), "healthy pipe", __FILE__,
                            __LINE__);
    const auto elapsed = std::chrono::duration_cast<Duration>(Clock::now() - start);

    ReportErrorIfAny("slow", slow_run.error());
    RELAY_CHECK(!slow_run.ok());
    RELAY_CHECK(go2cpp::Is(slow_run.error(), go2cpp::relay::ErrTimeout()));

    const PipeStats slow_stats = slow_pipe.Stats();
    std::printf(
        "  slow: a_to_b=%llu b_to_a=%llu dropped=%llu timeouts=%llu, %lld ms\n",
        static_cast<unsigned long long>(slow_stats.a_to_b),
        static_cast<unsigned long long>(slow_stats.b_to_a),
        static_cast<unsigned long long>(slow_stats.dropped),
        static_cast<unsigned long long>(slow_stats.timeouts),
        static_cast<long long>(elapsed.count()));

    RELAY_CHECK_U64(slow_stats.timeouts, 1);
    RELAY_CHECK(slow_stats.dropped > 0);
    // 有界队列：只允许队列容量量级的丢弃，绝不应把 8 MiB 全吞进内存。
    RELAY_CHECK(slow_stats.dropped < 256U * 1024U);
    RELAY_CHECK_U64(slow_stats.b_to_a, 64U * 1024U);
    RELAY_CHECK(slow_b.closed());
    RELAY_CHECK(!slow_a.closed());

    RELAY_CHECK(healthy_run.ok());
    const PipeStats healthy_stats = healthy_pipe.Stats();
    RELAY_CHECK_U64(healthy_stats.a_to_b, 1U * 1024U * 1024U);
    RELAY_CHECK_U64(healthy_stats.b_to_a, 1U * 1024U * 1024U);
    RELAY_CHECK_U64(healthy_stats.dropped, 0);
    RELAY_CHECK_U64(healthy_stats.timeouts, 0);
    CheckStreamDirection(healthy_a, healthy_b, 0x5201, 1U * 1024U * 1024U);
    CheckStreamDirection(healthy_b, healthy_a, 0x5202, 1U * 1024U * 1024U);
    std::fflush(stdout);
}

// ---------------------------------------------------------------------------
// 3. 写 deadline → Timeouts + 1
// ---------------------------------------------------------------------------
void TestWriteDeadline() {
    std::printf("[test] 3. 写 deadline → Timeouts+1\n");
    std::fflush(stdout);

    StreamOptions a_opts;
    a_opts.seed = 0x6101;
    // 数据量小于队列容量：读线程可以全部入队后干净 EOF，不会触发队列停滞，
    // 这样 timeouts 只能来自「写 deadline」这一条路径。
    a_opts.inbound_bytes = 64U * 1024U;
    StreamOptions b_opts;
    b_opts.seed = 0x6102;
    b_opts.inbound_bytes = 0;
    b_opts.block_writes = true;  // 模拟完全不读、也不返回的对端

    TestStream a(a_opts);
    TestStream b(b_opts);
    PipeConfig cfg;
    cfg.copy_buf = 4096;
    cfg.queue_cap = 1024U * 1024U;
    cfg.write_timeout = std::chrono::milliseconds(50);
    RelayPipe pipe(&a, &b, cfg);

    AsyncRun run(&pipe, ContextPtr{});
    const TimePoint start = Clock::now();
    run.Start();
    const bool closed =
        WaitUntil([&b] { return b.closed(); }, std::chrono::milliseconds(3000));
    RELAY_CHECK(closed);
    run.JoinOrAbort(std::chrono::milliseconds(5000), "write deadline", __FILE__,
                    __LINE__);
    const auto elapsed = std::chrono::duration_cast<Duration>(Clock::now() - start);

    ReportErrorIfAny("deadline", run.error());
    RELAY_CHECK(!run.ok());
    RELAY_CHECK(b.closed());
    const PipeStats stats = pipe.Stats();
    RELAY_CHECK_U64(stats.timeouts, 1);
    RELAY_CHECK(stats.dropped > 0);
    RELAY_CHECK_U64(stats.a_to_b, 0);
    RELAY_CHECK(go2cpp::Is(run.error(), go2cpp::relay::ErrTimeout()));
    RELAY_CHECK(go2cpp::ErrorMessage(run.error()).find("write") != std::string::npos);
    RELAY_CHECK(elapsed < std::chrono::milliseconds(3000));

    std::printf("  timeouts=%llu dropped=%llu, %lld ms\n",
                static_cast<unsigned long long>(stats.timeouts),
                static_cast<unsigned long long>(stats.dropped),
                static_cast<long long>(elapsed.count()));
    std::fflush(stdout);
}

// ---------------------------------------------------------------------------
// 4. 4 条管道并发互不串流
// ---------------------------------------------------------------------------
void TestFourPipesIsolation() {
    std::printf("[test] 4. 4 条管道并发互不串流\n");
    std::fflush(stdout);

    constexpr std::uint64_t kBytes = 1'000'000;
    constexpr int kPipes = 4;

    struct PipeCase {
        PipeCase(StreamOptions a_opts, StreamOptions b_opts, PipeConfig cfg)
            : a(std::move(a_opts)), b(std::move(b_opts)), pipe(&a, &b, cfg) {}

        TestStream a;
        TestStream b;
        RelayPipe pipe;
    };

    std::vector<std::unique_ptr<PipeCase>> cases;
    cases.reserve(kPipes);
    std::array<std::uint64_t, kPipes> seeds_a{};
    std::array<std::uint64_t, kPipes> seeds_b{};
    for (int index = 0; index < kPipes; ++index) {
        StreamOptions a_opts;
        a_opts.seed = 0x7000 + static_cast<std::uint64_t>(index) * 2;
        a_opts.inbound_bytes = kBytes;
        StreamOptions b_opts;
        b_opts.seed = 0x7001 + static_cast<std::uint64_t>(index) * 2;
        b_opts.inbound_bytes = kBytes;
        seeds_a[static_cast<std::size_t>(index)] = a_opts.seed;
        seeds_b[static_cast<std::size_t>(index)] = b_opts.seed;
        PipeConfig cfg;
        cfg.copy_buf = 8192;
        cfg.queue_cap = 32U * 1024U;
        cfg.write_timeout = std::chrono::milliseconds(2000);
        cases.push_back(
            std::make_unique<PipeCase>(std::move(a_opts), std::move(b_opts), cfg));
    }

    std::array<bool, kPipes> oks{};
    std::array<ErrorPtr, kPipes> errors{};
    std::vector<std::thread> threads;
    threads.reserve(kPipes);
    for (int index = 0; index < kPipes; ++index) {
        threads.emplace_back([&cases, &oks, &errors, index] {
            oks[static_cast<std::size_t>(index)] =
                cases[static_cast<std::size_t>(index)]->pipe.Run(
                    &errors[static_cast<std::size_t>(index)]);
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }

    for (int index = 0; index < kPipes; ++index) {
        const auto pos = static_cast<std::size_t>(index);
        RELAY_CHECK(oks[pos]);
        ReportErrorIfAny("4-pipes", errors[pos]);
        const PipeStats stats = cases[pos]->pipe.Stats();
        RELAY_CHECK_U64(stats.a_to_b, kBytes);
        RELAY_CHECK_U64(stats.b_to_a, kBytes);
        RELAY_CHECK_U64(stats.dropped, 0);
        RELAY_CHECK_U64(stats.timeouts, 0);
        // A 收到 B 的 seed_b 模式，B 收到 A 的 seed_a 模式；种子逐个不同，
        // 任一串流都会让摘要对不上。
        RELAY_CHECK(cases[pos]->a.Digest() == PatternDigest(seeds_b[pos], kBytes));
        RELAY_CHECK(cases[pos]->b.Digest() == PatternDigest(seeds_a[pos], kBytes));
        RELAY_CHECK(!cases[pos]->a.closed());
        RELAY_CHECK(!cases[pos]->b.closed());
    }
    std::printf("  %d pipes x %llu bytes/direction all byte-exact\n", kPipes,
                static_cast<unsigned long long>(kBytes));
    std::fflush(stdout);
}

// ---------------------------------------------------------------------------
// 5. 1e8 字节压测
// ---------------------------------------------------------------------------
void Test100MBStress() {
    std::printf("[test] 5. 1e8 字节压测（双向各 1e8）\n");
    std::fflush(stdout);

    constexpr std::uint64_t kBytes = 100'000'000;
    constexpr std::uint64_t kSeedA = 0xDEADBEEFULL;
    constexpr std::uint64_t kSeedB = 0xFEEDFACEULL;

    StreamOptions a_opts;
    a_opts.seed = kSeedA;
    a_opts.inbound_bytes = kBytes;
    StreamOptions b_opts;
    b_opts.seed = kSeedB;
    b_opts.inbound_bytes = kBytes;

    TestStream a(a_opts);
    TestStream b(b_opts);
    RelayPipe pipe(&a, &b, PipeConfig{});

    const TimePoint digest_start = Clock::now();
    const auto expected_for_a = PatternDigest(kSeedB, kBytes);
    const auto expected_for_b = PatternDigest(kSeedA, kBytes);
    const auto digest_ms = std::chrono::duration_cast<Duration>(Clock::now() - digest_start);

    ErrorPtr error;
    const TimePoint start = Clock::now();
    const bool ok = pipe.Run(&error);
    const auto elapsed = std::chrono::duration_cast<Duration>(Clock::now() - start);

    RELAY_CHECK(ok);
    RELAY_CHECK(!error);
    ReportErrorIfAny("1e8", error);
    const PipeStats stats = pipe.Stats();
    RELAY_CHECK_U64(stats.a_to_b, kBytes);
    RELAY_CHECK_U64(stats.b_to_a, kBytes);
    RELAY_CHECK_U64(stats.dropped, 0);
    RELAY_CHECK_U64(stats.timeouts, 0);
    RELAY_CHECK(a.Digest() == expected_for_a);
    RELAY_CHECK(b.Digest() == expected_for_b);

    std::printf("  2 x %llu bytes, %lld ms, %.1f MiB/s (digest oracle %lld ms)\n",
                static_cast<unsigned long long>(kBytes),
                static_cast<long long>(elapsed.count()),
                MibPerSecond(kBytes * 2, elapsed),
                static_cast<long long>(digest_ms.count()));
    std::fflush(stdout);
}

// ---------------------------------------------------------------------------
// 6. 40 轮随机：规模 / 队列 / 短写 / 慢写
// ---------------------------------------------------------------------------
void TestRandomized() {
    std::printf("[test] 6. 40 轮随机规模/队列/短写/慢写\n");
    std::fflush(stdout);

    std::mt19937_64 rng(0x20261101ULL);
    const std::array<std::size_t, 3> copy_bufs{512, 4096, 32768};
    int rounds_with_data = 0;
    std::uint64_t total_bytes = 0;

    for (int round = 0; round < 40; ++round) {
        const std::uint64_t bytes = rng() % 300001ULL;
        const std::size_t copy_buf = copy_bufs[rng() % copy_bufs.size()];
        const std::size_t queue_cap = copy_buf * (1 + (rng() % 6));

        StreamOptions a_opts;
        a_opts.seed = rng();
        a_opts.inbound_bytes = bytes;
        StreamOptions b_opts;
        b_opts.seed = rng();
        b_opts.inbound_bytes = bytes;

        // 短写与慢写各只加在一侧，避免把随机测试拖成分钟级。
        const bool short_writes = (rng() % 2) == 0;
        if (short_writes) {
            if ((rng() % 2) == 0) {
                a_opts.max_write_chunk = 64 + rng() % 4032;
            } else {
                b_opts.max_write_chunk = 64 + rng() % 4032;
            }
        } else if ((rng() % 3) == 0) {
            const auto delay = std::chrono::microseconds(1 + rng() % 20);
            if ((rng() % 2) == 0) {
                a_opts.write_delay = delay;
            } else {
                b_opts.write_delay = delay;
            }
        }

        PipeConfig cfg;
        cfg.copy_buf = copy_buf;
        cfg.queue_cap = queue_cap;
        cfg.write_timeout = std::chrono::milliseconds(2000);

        TestStream a(a_opts);
        TestStream b(b_opts);
        RelayPipe pipe(&a, &b, cfg);
        ErrorPtr error;
        const bool ok = pipe.Run(&error);
        RELAY_CHECK(ok);
        if (!ok) {
            std::fprintf(stderr, "[relay-test] round %d bytes=%llu failed: %s\n", round,
                         static_cast<unsigned long long>(bytes),
                         go2cpp::ErrorMessage(error).c_str());
            continue;
        }
        const PipeStats stats = pipe.Stats();
        RELAY_CHECK_U64(stats.a_to_b, bytes);
        RELAY_CHECK_U64(stats.b_to_a, bytes);
        RELAY_CHECK_U64(stats.dropped, 0);
        RELAY_CHECK_U64(stats.timeouts, 0);
        RELAY_CHECK(a.Digest() == PatternDigest(b_opts.seed, bytes));
        RELAY_CHECK(b.Digest() == PatternDigest(a_opts.seed, bytes));
        if (bytes > 0) {
            ++rounds_with_data;
            total_bytes += bytes;
        }
    }
    std::printf("  %d/40 rounds with data, %llu bytes total\n", rounds_with_data,
                static_cast<unsigned long long>(total_bytes));
    std::fflush(stdout);
}

// ---------------------------------------------------------------------------
// 7. Context 取消 → 强制关闭两端
// ---------------------------------------------------------------------------
void TestContextCancel() {
    std::printf("[test] 7. Context 取消 → 关闭两端并回收线程\n");
    std::fflush(stdout);

    StreamOptions idle_opts;
    idle_opts.inbound_eof = false;
    idle_opts.wait_for_input = true;
    TestStream a(idle_opts);
    TestStream b(idle_opts);
    RelayPipe pipe(&a, &b, PipeConfig{});

    const auto ctx_pair = Context::WithCancel(Context::Background());
    AsyncRun run(&pipe, ctx_pair.first);
    run.Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    ctx_pair.second();
    run.JoinOrAbort(std::chrono::milliseconds(3000), "context cancel", __FILE__,
                    __LINE__);

    ReportErrorIfAny("cancel", run.error());
    RELAY_CHECK(!run.ok());
    RELAY_CHECK(go2cpp::Is(run.error(), go2cpp::CanceledError()));
    RELAY_CHECK(a.closed());
    RELAY_CHECK(b.closed());
    const PipeStats stats = pipe.Stats();
    RELAY_CHECK_U64(stats.a_to_b, 0);
    RELAY_CHECK_U64(stats.b_to_a, 0);
    RELAY_CHECK_U64(stats.timeouts, 0);
    std::printf("  canceled, both endpoints closed\n");
    std::fflush(stdout);
}

// ---------------------------------------------------------------------------
// 8. API 边界：参数归一化 / 空 Stream / 重复 Run / 已取消 Context
// ---------------------------------------------------------------------------
void TestApiEdges() {
    std::printf("[test] 8. API 边界\n");
    std::fflush(stdout);

    // 参数归一化
    {
        StreamOptions empty;
        TestStream a(empty);
        TestStream b(empty);
        PipeConfig zero;
        zero.copy_buf = 0;
        zero.queue_cap = 0;
        zero.write_timeout = std::chrono::milliseconds(0);
        RelayPipe pipe(&a, &b, zero);
        RELAY_CHECK_U64(pipe.Config().copy_buf, 32U * 1024U);
        RELAY_CHECK_U64(pipe.Config().queue_cap, 256U * 1024U);
        RELAY_CHECK_U64(pipe.Config().write_timeout.count(), 30000);

        ErrorPtr error;
        RELAY_CHECK(pipe.Run(&error));
        RELAY_CHECK(!error);
        const PipeStats stats = pipe.Stats();
        RELAY_CHECK_U64(stats.a_to_b, 0);
        RELAY_CHECK_U64(stats.b_to_a, 0);
        RELAY_CHECK_U64(stats.dropped, 0);
        RELAY_CHECK_U64(stats.timeouts, 0);
        // 正常 EOF 不关闭 Stream
        RELAY_CHECK(!a.closed());
        RELAY_CHECK(!b.closed());

        // 重复 Run 必须拒绝，而不是再起一轮线程。
        ErrorPtr again;
        RELAY_CHECK(!pipe.Run(&again));
        RELAY_CHECK(go2cpp::ErrorMessage(again).find("already") != std::string::npos);
    }

    // 空 Stream
    {
        RelayPipe pipe(nullptr, nullptr, PipeConfig{});
        ErrorPtr error;
        RELAY_CHECK(!pipe.Run(&error));
        RELAY_CHECK(go2cpp::ErrorMessage(error).find("null") != std::string::npos);
    }

    // 已取消的 Context：不启动线程，直接关闭两端并返回取消原因。
    {
        StreamOptions idle_opts;
        idle_opts.inbound_eof = false;
        idle_opts.wait_for_input = true;
        TestStream a(idle_opts);
        TestStream b(idle_opts);
        RelayPipe pipe(&a, &b, PipeConfig{});
        const auto ctx_pair = Context::WithCancel(Context::Background());
        ctx_pair.second();
        ErrorPtr error;
        RELAY_CHECK(!pipe.Run(ctx_pair.first, &error));
        RELAY_CHECK(go2cpp::Is(error, go2cpp::CanceledError()));
        RELAY_CHECK(a.closed());
        RELAY_CHECK(b.closed());
    }

    std::printf("  config/empty/already-run/canceled all rejected as expected\n");
    std::fflush(stdout);
}

}  // namespace

int main() {
    std::printf("=== RelayPipe (REQ-002) tests ===\n");
    std::fflush(stdout);

    TestBidirectional10MB();
    TestSlowPeerQueueStall();
    TestWriteDeadline();
    TestFourPipesIsolation();
    TestRandomized();
    TestContextCancel();
    TestApiEdges();
    Test100MBStress();

    const int failures = Failures().load(std::memory_order_relaxed);
    if (failures != 0) {
        std::fprintf(stderr, "=== relay pipe: %d failure(s) ===\n", failures);
        return 1;
    }
    std::printf("=== relay pipe: all checks passed ===\n");
    return 0;
}
