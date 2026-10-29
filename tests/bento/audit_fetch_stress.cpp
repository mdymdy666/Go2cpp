/**
 * @file audit_fetch_stress.cpp
 * @brief fetch 模块压测与复杂场景审计。
 * @details 覆盖：
 *          - 压测：100 MiB 下载（Content-Length 与 chunked 两种编码）、
 *            1000 并发 fetch；
 *          - 场景 1：重定向链（302→302→200、5 跳、超出上限、跨端口绝对 URL）；
 *          - 场景 2：三层超时（连接/读空闲/总）+ 重试 3 次；
 *          - 场景 3：磁盘写失败（只读目录、RLIMIT_FSIZE 触发 EFBIG）；
 *          - 场景 4：大文件流式超限（默认 64 MiB，CL 预检与 chunked 累计）；
 *          - 场景 5：100 个混合大小下载并发。
 *
 * @author go2cpp contributors
 * @date 2026-10-29
 * @copyright MIT License
 */

#include "audit_fetch_support.hpp"

#include <sys/resource.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

using namespace audit;  // NOLINT(google-build-using-namespace)：审计程序局部使用

// ---------------------------------------------------------------------------
// 压测 1：100 MiB 下载
// ---------------------------------------------------------------------------

/**
 * @brief 100 MiB 下载：吞吐、峰值内存、fd、磁盘写入、摘要一致性。
 * @details 服务端用生成器流式发送（不预分配 100 MiB），客户端校验 SHA-256，
 *          因此可以同时观察「模块是否真的流式写盘」：峰值 RSS 不应随文件
 *          大小线性增长。
 */
void Stress100MiB() {
    constexpr std::uint64_t kTotal = 100ull * 1024 * 1024;
    constexpr std::uint32_t kSeed = 0x1001u;
    constexpr std::uint32_t kSeedSmall = 0x1002u;
    constexpr std::uint64_t kSmall = 1ull * 1024 * 1024;

    std::printf("\n== 压测 1：100 MiB 下载（流式，本地 HTTP）==\n");
    TrimMemory();
    TempDir dir("stress100mb");
    AUDIT_CHECK(dir.ok());
    if (!dir.ok()) {
        return;
    }

    // 基准：单独测一次 100 MiB 的 SHA-256 计算耗时，用来判断下载吞吐的
    // 上限是否被摘要计算（而不是 IO/拷贝）卡住。
    const auto sha_begin = std::chrono::steady_clock::now();
    const std::string expected = PayloadSha256Repeated(kSeed, kTotal);
    const double sha_ms = MillisecondsSince(sha_begin);
    std::printf("[DATA] SHA-256 纯计算基准（100MiB）: %8.1fms => %.1f MiB/s\n", sha_ms,
                100.0 / (sha_ms / 1000.0));
    const std::string expected_small = PayloadSha256(kSeedSmall, kSmall);
    AUDIT_CHECK_EQ(expected.size(), static_cast<std::size_t>(64));

    std::vector<ResponseSpec> specs;
    ResponseSpec identity;
    identity.kind = ResponseKind::kBody;
    identity.head = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(kTotal) +
                    "\r\nConnection: close\r\n\r\n";
    identity.body_bytes = kTotal;
    identity.seed = kSeed;
    // 重复块模式：服务端不再花时间生成数据，吞吐量反映的是客户端（fetch）
    // 的读取/写盘/摘要路径，而不是审计服务器自己的 LCG 循环。
    identity.repeat_block = true;
    specs.push_back(identity);

    ResponseSpec chunked = identity;
    chunked.head =
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n";
    chunked.chunk_bytes = 64 * 1024;
    specs.push_back(chunked);

    ResponseSpec warmup;
    warmup.kind = ResponseKind::kBody;
    warmup.head = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(kSmall) +
                  "\r\nConnection: close\r\n\r\n";
    warmup.body_bytes = kSmall;
    warmup.seed = kSeedSmall;
    specs.push_back(warmup);

    AuditServer server;
    server.SetResponses(std::move(specs));
    if (!server.Start(2)) {
        AUDIT_CHECK_MSG(false, "启动 100 MiB 服务器失败");
        return;
    }

    // 预热：把 socket 栈、分配器与页缓存加热，并让基线 RSS 稳定下来。
    {
        FetchOptions options;
        options.filename = "warmup.bin";
        options.max_bytes = 4 * 1024 * 1024;
        options.total_timeout_ms = 60 * 1000;
        const FetchResult warm = Fetch(server.Url(2), dir.str(), options);
        AUDIT_CHECK_STATUS(warm, FetchStatus::kOk, "100 MiB 预热下载");
        AUDIT_CHECK_EQ(warm.sha256, expected_small);
        AUDIT_CHECK_EQ(server.requests(), 1);
        // 预热文件删掉，后续目录字节数只反映被测文件。
        std::error_code ec;
        fs::remove(warm.path, ec);
        AUDIT_CHECK_EQ(CountEntries(dir.path()), static_cast<std::size_t>(0));
    }

    const std::uint64_t baseline_rss = CurrentRss();
    for (int round = 0; round < 2; ++round) {
        const bool is_chunked = round == 1;
        FetchOptions options;
        options.filename = is_chunked ? "big-chunked.bin" : "big-identity.bin";
        options.max_bytes = 256ull * 1024 * 1024;
        options.connect_timeout_ms = 5000;
        options.read_timeout_ms = 60 * 1000;
        options.total_timeout_ms = 120 * 1000;
        options.max_retries = 0;

        ResourceSampler sampler;
        sampler.Start();
        const auto started = std::chrono::steady_clock::now();
        const FetchResult result =
            Fetch(server.Url(static_cast<std::size_t>(round)), dir.str(), options);
        const double elapsed = MillisecondsSince(started);
        sampler.Stop();

        const std::string label =
            is_chunked ? "100MiB 下载（chunked）" : "100MiB 下载（Content-Length）";
        AUDIT_CHECK_STATUS(result, FetchStatus::kOk, label);
        AUDIT_CHECK_EQ(result.bytes, kTotal);
        AUDIT_CHECK_EQ(result.sha256, expected);
        AUDIT_CHECK_EQ(result.attempts, 1);
        AUDIT_CHECK_EQ(result.http_status, 200);
        if (result.ok()) {
            std::error_code ec;
            const auto size = fs::file_size(result.path, ec);
            AUDIT_CHECK_MSG(!ec && size == kTotal,
                            label + ": 落盘大小应为 " + std::to_string(kTotal));
        }
        AUDIT_CHECK_EQ(DirectoryBytes(dir.path()),
                       static_cast<std::uint64_t>(round + 1) * kTotal);
        AUDIT_CHECK_MSG(!HasTempArtifacts(dir.path()), label + ": 不应残留临时文件");

        const std::uint64_t peak = sampler.peak_rss();
        Report(label, kTotal, elapsed, peak, sampler.peak_fd());
        const std::uint64_t growth = peak > baseline_rss ? peak - baseline_rss : 0;
        std::printf("        peak_rss_delta=%5.1fMiB（相对压测前基线）\n",
                    static_cast<double>(growth) / (1024.0 * 1024.0));
        AUDIT_CHECK_MSG(growth < 24ull * 1024 * 1024,
                        label + ": 峰值 RSS 增长应 < 24 MiB（实际 " +
                            std::to_string(growth / (1024 * 1024)) +
                            " MiB），说明没有把整个文件读进内存");
        AUDIT_CHECK_MSG(sampler.peak_fd() < 16,
                        label + ": 单次下载的 fd 峰值应 < 16（实际 " +
                            std::to_string(sampler.peak_fd()) + "）");
    }

    // 服务端确实把 2×100 MiB + 1 MiB 发出去了，排除「客户端提前成功」的假象。
    AUDIT_CHECK_MSG(server.bytes_sent() >= 2 * kTotal + kSmall,
                    "服务端发送字节数应覆盖两轮 100 MiB（实际 " +
                        std::to_string(server.bytes_sent()) + "）");
    server.Stop();
    RemoveTree(dir.path());
}

// ---------------------------------------------------------------------------
// 压测 2：1000 并发 fetch
// ---------------------------------------------------------------------------

/**
 * @brief 1000 个线程同时 Fetch（每次 4 KiB），验证 fd/线程/临时文件在
 *        高并发下不失控，且全部成功、内容摘要一致。
 */
void Stress1000Concurrent() {
    constexpr int kConcurrency = 1000;
    constexpr std::uint64_t kBody = 4096;
    constexpr std::uint32_t kSeed = 0x2002u;

    std::printf("\n== 压测 2：%d 并发 fetch（每次 4 KiB）==\n", kConcurrency);
    TempDir dir("stress1000c");
    AUDIT_CHECK(dir.ok());
    if (!dir.ok()) {
        return;
    }

    ResponseSpec spec;
    spec.kind = ResponseKind::kBody;
    spec.head = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(kBody) +
                "\r\nConnection: close\r\n\r\n";
    spec.body_bytes = kBody;
    spec.seed = kSeed;
    std::vector<ResponseSpec> specs;
    specs.push_back(spec);

    AuditServer server;
    server.SetResponses(std::move(specs));
    if (!server.Start(8)) {
        AUDIT_CHECK_MSG(false, "启动 1000 并发服务器失败");
        return;
    }

    const std::string expected = PayloadSha256(kSeed, kBody);
    std::vector<FetchResult> results(static_cast<std::size_t>(kConcurrency));
    std::atomic<int> spawn_failures{0};

    const std::uint64_t baseline_rss = CurrentRss();
    ResourceSampler sampler;
    sampler.Start();
    const auto started = std::chrono::steady_clock::now();
    {
        ThreadGroup group;
        for (int index = 0; index < kConcurrency; ++index) {
            const bool spawned = group.Spawn([&server, &dir, &results, index]() {
                FetchOptions options;
                options.filename = "f-" + std::to_string(index) + ".bin";
                options.max_bytes = 8192;
                options.max_retries = 0;
                options.connect_timeout_ms = 10 * 1000;
                options.read_timeout_ms = 20 * 1000;
                options.total_timeout_ms = 30 * 1000;
                results[static_cast<std::size_t>(index)] =
                    Fetch(server.Url(0), dir.str(), options);
            });
            if (!spawned) {
                spawn_failures.fetch_add(1, std::memory_order_relaxed);
            }
        }
        AUDIT_CHECK_EQ(spawn_failures.load(), 0);
        group.JoinAll();
    }
    const double elapsed = MillisecondsSince(started);
    sampler.Stop();

    int ok_count = 0;
    int reported = 0;
    for (const FetchResult& result : results) {
        if (result.ok()) {
            ++ok_count;
            continue;
        }
        if (reported < 5) {
            ++reported;
            std::fprintf(stderr, "  失败样本: %s / %s\n",
                         go2cpp::bento::app::FetchStatusToString(result.status),
                         result.message.c_str());
        }
    }
    AUDIT_CHECK_MSG(ok_count == kConcurrency,
                    "1000 并发应全部成功（实际 " + std::to_string(ok_count) + "）");

    int sha_mismatch = 0;
    for (const FetchResult& result : results) {
        if (result.ok() && result.sha256 != expected) {
            ++sha_mismatch;
        }
    }
    AUDIT_CHECK_EQ(sha_mismatch, 0);
    AUDIT_CHECK_EQ(CountEntries(dir.path()), static_cast<std::size_t>(kConcurrency));
    AUDIT_CHECK_MSG(!HasTempArtifacts(dir.path()), "1000 并发后不应残留临时文件");
    AUDIT_CHECK_EQ(DirectoryBytes(dir.path()),
                   static_cast<std::uint64_t>(kConcurrency) * kBody);

    Report("1000 并发 fetch（4KiB）", static_cast<std::uint64_t>(kConcurrency) * kBody, elapsed,
           sampler.peak_rss(), sampler.peak_fd());
    const double seconds = elapsed / 1000.0;
    std::printf("        qps=%.0f peak_rss_delta=%.1fMiB\n",
                seconds > 0.0 ? static_cast<double>(kConcurrency) / seconds : 0.0,
                static_cast<double>(sampler.peak_rss() > baseline_rss
                                        ? sampler.peak_rss() - baseline_rss
                                        : 0) /
                    (1024.0 * 1024.0));
    AUDIT_CHECK_MSG(sampler.peak_fd() < 2 * static_cast<std::size_t>(kConcurrency) + 64,
                    "fd 峰值不应超过 2×并发数+64（实际 " + std::to_string(sampler.peak_fd()) +
                        "）");

    server.Stop();
    RemoveTree(dir.path());
}

// ---------------------------------------------------------------------------
// 观察：并发写同一目标
// ---------------------------------------------------------------------------

/**
 * @brief 观察 8 个线程同时下载到同一目标（`overwrite=false`）时的实际行为。
 * @details `Fetch` 在开头检查目标是否存在、在结束时才 `rename`，两步之间
 *          没有锁，因此理论上存在 TOCTOU：多个请求可能都通过"不存在"检查，
 *          最终互相覆盖。这里只观察并记录，不把「多于一个成功」判为失败
 *          （当前接口契约没有承诺并发原子性），但会打印实测结果供报告引用。
 */
void ProbeSameTargetConcurrency() {
    std::printf("\n== 观察：8 线程并发写同一目标（overwrite=false）==\n");
    TempDir dir("race");
    AUDIT_CHECK(dir.ok());
    if (!dir.ok()) {
        return;
    }
    constexpr std::uint32_t kSeed = 0x2102u;
    constexpr std::uint64_t kBody = 8192;
    ResponseSpec spec;
    spec.kind = ResponseKind::kBody;
    spec.head = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(kBody) +
                "\r\nConnection: close\r\n\r\n";
    spec.body_bytes = kBody;
    spec.seed = kSeed;
    std::vector<ResponseSpec> specs;
    specs.push_back(spec);
    AuditServer server;
    server.SetResponses(std::move(specs));
    if (!server.Start(8)) {
        AUDIT_CHECK_MSG(false, "启动并发同目标服务器失败");
        return;
    }

    constexpr int kThreads = 8;
    std::vector<FetchResult> results(static_cast<std::size_t>(kThreads));
    {
        ThreadGroup group;
        for (int index = 0; index < kThreads; ++index) {
            const bool spawned = group.Spawn([&server, &dir, &results, index]() {
                FetchOptions options;
                options.filename = "same-target.bin";
                options.max_retries = 0;
                options.max_bytes = 64 * 1024;
                results[static_cast<std::size_t>(index)] = Fetch(server.Url(0), dir.str(), options);
            });
            AUDIT_CHECK(spawned);
        }
        group.JoinAll();
    }

    int succeeded = 0;
    int target_exists = 0;
    int other = 0;
    for (const FetchResult& result : results) {
        if (result.ok()) {
            ++succeeded;
        } else if (result.status == FetchStatus::kTargetExists) {
            ++target_exists;
        } else {
            ++other;
            std::fprintf(stderr, "  异常: %s / %s\n",
                         go2cpp::bento::app::FetchStatusToString(result.status),
                         result.message.c_str());
        }
    }
    std::printf("[DATA] 同一目标 8 并发: ok=%d target_exists=%d other=%d\n", succeeded,
                target_exists, other);
    if (succeeded > 1) {
        std::printf("        注意：%d 个并发请求都成功落盘（目标存在检查与 rename 之间存在 "
                    "TOCTOU 窗口）\n",
                    succeeded);
    }
    AUDIT_CHECK_MSG(succeeded + target_exists == kThreads, "每个请求都应有明确结果");
    // P1 回归：排他落盘（RENAME_NOREPLACE）之后，同一目标的并发请求只允许一个成功。
    AUDIT_CHECK_MSG(succeeded == 1, "同一目标并发时只应有一个请求落盘（实际 " +
                                        std::to_string(succeeded) + "）");
    AUDIT_CHECK_EQ(target_exists, kThreads - 1);
    AUDIT_CHECK_EQ(CountEntries(dir.path()), static_cast<std::size_t>(1));
    AUDIT_CHECK_MSG(!HasTempArtifacts(dir.path()), "并发同目标不应残留临时文件");
    server.Stop();
    RemoveTree(dir.path());
}

// ---------------------------------------------------------------------------
// 场景 1：重定向链
// ---------------------------------------------------------------------------

/// 追加一条 302 响应。
void AddRedirect(std::vector<ResponseSpec>* specs, const std::string& location) {
    ResponseSpec spec;
    spec.kind = ResponseKind::kRaw;
    spec.raw = "HTTP/1.1 302 Found\r\nLocation: " + location +
               "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    specs->push_back(spec);
}

void ScenarioRedirectChain() {
    std::printf("\n== 场景 1：重定向链（302→302→200）==\n");
    TempDir dir("redirect");
    AUDIT_CHECK(dir.ok());
    if (!dir.ok()) {
        return;
    }

    constexpr std::uint32_t kSeed = 0x3003u;
    constexpr std::uint64_t kBody = 8192;
    const std::string expected = PayloadSha256(kSeed, kBody);

    ResponseSpec terminal;
    terminal.kind = ResponseKind::kBody;
    terminal.head = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(kBody) +
                    "\r\nConnection: close\r\n\r\n";
    terminal.body_bytes = kBody;
    terminal.seed = kSeed;

    // 先起「另一个端口」的服务器：跨端口重定向需要它的真实端口拼绝对 URL。
    std::vector<ResponseSpec> other_specs;
    other_specs.push_back(terminal);
    AuditServer other;
    other.SetResponses(std::move(other_specs));
    if (!other.Start(2)) {
        AUDIT_CHECK_MSG(false, "启动跨端口服务器失败");
        return;
    }

    std::vector<ResponseSpec> specs;
    // 0 → 1 → 2（200）
    AddRedirect(&specs, "/r/1");
    AddRedirect(&specs, "/r/2");
    specs.push_back(terminal);
    // 3..7：5 跳链（等于 max_redirects 上限，应成功）
    for (int index = 3; index < 8; ++index) {
        AddRedirect(&specs, "/r/" + std::to_string(index + 1));
    }
    specs.push_back(terminal);
    // 9..15：7 跳链（超过默认 max_redirects=5，应失败）
    for (int index = 9; index < 16; ++index) {
        AddRedirect(&specs, "/r/" + std::to_string(index + 1));
    }
    specs.push_back(terminal);
    // 17：跨端口绝对 URL
    const std::size_t cross_index = specs.size();
    AddRedirect(&specs, other.BaseUrl() + "/r/0");

    AuditServer server;
    server.SetResponses(std::move(specs));
    if (!server.Start(4)) {
        AUDIT_CHECK_MSG(false, "启动重定向服务器失败");
        other.Stop();
        return;
    }

    // ---- 3 跳链 ----
    {
        FetchOptions options;
        options.max_retries = 0;
        const FetchResult result = Fetch(server.Url(0), dir.str(), options);
        AUDIT_CHECK_STATUS(result, FetchStatus::kOk, "302→302→200");
        AUDIT_CHECK_EQ(result.bytes, kBody);
        AUDIT_CHECK_EQ(result.sha256, expected);
        AUDIT_CHECK_EQ(result.attempts, 1);
        AUDIT_CHECK_EQ(server.requests(), 3);
        AUDIT_CHECK_MSG(fs::path(result.path).filename() == "0",
                        "落盘文件名应取自原始 URL 最后一段（实际 " +
                            fs::path(result.path).filename().string() + "）");
    }

    // ---- 5 跳链（正好在上限内）----
    {
        const int before = server.requests();
        FetchOptions options;
        options.max_retries = 0;
        options.max_redirects = 5;
        options.filename = "five-hops.bin";
        const FetchResult result = Fetch(server.Url(3), dir.str(), options);
        AUDIT_CHECK_STATUS(result, FetchStatus::kOk, "5 跳重定向链");
        AUDIT_CHECK_EQ(result.sha256, expected);
        AUDIT_CHECK_EQ(server.requests() - before, 6);
    }

    // ---- 7 跳链（超过上限）----
    {
        const int before = server.requests();
        FetchOptions options;
        options.max_retries = 0;
        options.max_redirects = 5;
        options.filename = "too-many-hops.bin";
        const FetchResult result = Fetch(server.Url(9), dir.str(), options);
        AUDIT_CHECK_STATUS(result, FetchStatus::kHttpError, "7 跳重定向链");
        AUDIT_CHECK_MSG(result.message.find("重定向次数") != std::string::npos,
                        "诊断应说明重定向次数超限（实际 " + result.message + "）");
        CheckFailureCleanup(result, dir.path(), "重定向超限");
        AUDIT_CHECK_EQ(server.requests() - before, 6);  // 第 6 次 302 后放弃
    }

    // ---- 跨端口绝对 URL ----
    {
        FetchOptions options;
        options.max_retries = 0;
        options.filename = "cross-port.bin";
        const FetchResult result = Fetch(server.Url(cross_index), dir.str(), options);
        AUDIT_CHECK_STATUS(result, FetchStatus::kOk, "跨端口绝对 URL 重定向");
        AUDIT_CHECK_EQ(result.sha256, expected);
        AUDIT_CHECK_EQ(other.requests(), 1);
    }

    other.Stop();
    server.Stop();
    AUDIT_CHECK_MSG(!HasTempArtifacts(dir.path()), "重定向场景不应残留临时文件");
    RemoveTree(dir.path());
}

// ---------------------------------------------------------------------------
// 场景 2：三层超时 + 重试 3 次
// ---------------------------------------------------------------------------

/// 占满 accept 队列的监听器：不再 accept，后续连接的 SYN 被丢弃。
class BacklogTrap final {
   public:
    ~BacklogTrap() { Stop(); }

    bool Start() {
        m_fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (m_fd < 0) {
            return false;
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (::bind(m_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
            ::listen(m_fd, 1) != 0) {
            Stop();
            return false;
        }
        socklen_t length = sizeof(address);
        if (::getsockname(m_fd, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
            Stop();
            return false;
        }
        m_port = ntohs(address.sin_port);
        return true;
    }

    /// 用两个已建立连接的客户端占满队列（保持不读不写）。
    bool Saturate() {
        for (int index = 0; index < 2; ++index) {
            const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
            if (fd < 0) {
                return false;
            }
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            address.sin_port = htons(m_port);
            if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
                ::close(fd);
                return false;
            }
            m_held.push_back(fd);
        }
        return true;
    }

    std::uint16_t port() const noexcept { return m_port; }

    void Stop() {
        for (int fd : m_held) {
            ::close(fd);
        }
        m_held.clear();
        if (m_fd >= 0) {
            ::close(m_fd);
            m_fd = -1;
        }
    }

   private:
    int m_fd{-1};
    std::uint16_t m_port{0};
    std::vector<int> m_held;
};

void ScenarioTimeoutRetry() {
    std::printf("\n== 场景 2：三层超时 + 重试 3 次 ==\n");
    TempDir dir("timeout");
    AUDIT_CHECK(dir.ok());
    if (!dir.ok()) {
        return;
    }

    // ---- (a) 连接超时：SYN 被丢弃 ----
    {
        BacklogTrap trap;
        const bool ready = trap.Start() && trap.Saturate();
        AUDIT_CHECK_MSG(ready, "backlog 占满环境准备成功");
        if (ready) {
            FetchOptions options;
            options.max_retries = 3;
            options.retry_backoff_ms = 10;
            options.connect_timeout_ms = 300;
            options.read_timeout_ms = 300;
            options.total_timeout_ms = 5000;
            options.filename = "connect-timeout.bin";
            const auto begin = std::chrono::steady_clock::now();
            const FetchResult result =
                Fetch("http://127.0.0.1:" + std::to_string(trap.port()) + "/x.bin", dir.str(),
                      options);
            const double elapsed = MillisecondsSince(begin);
            std::printf("[DATA] 连接超时（SYN 丢弃）: status=%s attempts=%d elapsed=%.0fms\n",
                        go2cpp::bento::app::FetchStatusToString(result.status), result.attempts,
                        elapsed);
            AUDIT_CHECK_MSG(result.status == FetchStatus::kTimeout ||
                                result.status == FetchStatus::kRetryExhausted ||
                                result.status == FetchStatus::kConnectFailed,
                            "连接超时场景状态应属于 timeout/retry_exhausted/connect_failed（实际 " +
                                std::string(
                                    go2cpp::bento::app::FetchStatusToString(result.status)) +
                                "）");
            AUDIT_CHECK_EQ(result.attempts, 4);
            AUDIT_CHECK(!result.ok());
            CheckFailureCleanup(result, dir.path(), "连接超时");
        }
        trap.Stop();
    }

    // ---- (a2) 不可达地址：重试 3 次后 kRetryExhausted ----
    {
        FetchOptions options;
        options.max_retries = 3;
        options.retry_backoff_ms = 5;
        options.connect_timeout_ms = 200;
        options.read_timeout_ms = 200;
        options.total_timeout_ms = 2000;
        options.filename = "unreachable.bin";
        const FetchResult result = Fetch("http://10.255.255.1:81/x.bin", dir.str(), options);
        std::printf("[DATA] 不可达地址: status=%s attempts=%d message=%s\n",
                    go2cpp::bento::app::FetchStatusToString(result.status), result.attempts,
                    result.message.c_str());
        AUDIT_CHECK_STATUS(result, FetchStatus::kRetryExhausted, "不可达地址");
        AUDIT_CHECK_EQ(result.attempts, 4);
        AUDIT_CHECK_MSG(result.message.find("重试 3 次后仍失败") != std::string::npos,
                        "kRetryExhausted 的诊断应包含重试次数（实际 " + result.message + "）");
        CheckFailureCleanup(result, dir.path(), "不可达地址");
    }

    // ---- (b) 读空闲超时 ----
    {
        std::vector<ResponseSpec> specs;
        ResponseSpec silent;
        silent.kind = ResponseKind::kSilent;
        specs.push_back(silent);
        AuditServer server;
        server.SetResponses(std::move(specs));
        AUDIT_CHECK(server.Start(8));
        FetchOptions options;
        options.max_retries = 3;
        options.retry_backoff_ms = 10;
        options.read_timeout_ms = 200;
        options.connect_timeout_ms = 1000;
        options.total_timeout_ms = 10 * 1000;
        options.filename = "read-timeout.bin";
        const auto begin = std::chrono::steady_clock::now();
        const FetchResult result = Fetch(server.Url(0), dir.str(), options);
        const double elapsed = MillisecondsSince(begin);
        std::printf("[DATA] 读空闲超时: status=%s attempts=%d requests=%d elapsed=%.0fms\n",
                    go2cpp::bento::app::FetchStatusToString(result.status), result.attempts,
                    server.requests(), elapsed);
        AUDIT_CHECK_STATUS(result, FetchStatus::kRetryExhausted, "读空闲超时（重试用尽）");
        AUDIT_CHECK_EQ(result.attempts, 4);
        AUDIT_CHECK_EQ(server.requests(), 4);
        AUDIT_CHECK_MSG(elapsed >= 600.0 && elapsed < 4000.0,
                        "读超时总耗时应约 4×200ms+退避（实际 " + std::to_string(elapsed) + "ms）");
        AUDIT_CHECK_MSG(result.message.find("超时") != std::string::npos,
                        "诊断应说明超时（实际 " + result.message + "）");
        CheckFailureCleanup(result, dir.path(), "读空闲超时");

        // (b2) 不重试时不该被包装成 kRetryExhausted，应保留原始分类 kTimeout。
        {
            FetchOptions single;
            single.max_retries = 0;
            single.read_timeout_ms = 150;
            single.connect_timeout_ms = 1000;
            single.total_timeout_ms = 5000;
            single.filename = "read-timeout-single.bin";
            const int before = server.requests();
            const FetchResult once = Fetch(server.Url(0), dir.str(), single);
            AUDIT_CHECK_STATUS(once, FetchStatus::kTimeout, "读空闲超时（max_retries=0）");
            AUDIT_CHECK_EQ(once.attempts, 1);
            AUDIT_CHECK_EQ(server.requests() - before, 1);
        }
        server.Stop();
    }

    // ---- (c) 总超时（读空闲超时故意放大，确保是总超时先触发）----
    {
        std::vector<ResponseSpec> specs;
        ResponseSpec drip;
        drip.kind = ResponseKind::kSlowDrip;
        drip.head = "HTTP/1.1 200 OK\r\nContent-Length: 40\r\nConnection: close\r\n\r\n";
        drip.body_bytes = 40;
        drip.drip_ms = 50;
        drip.seed = 0x4004u;
        specs.push_back(drip);
        AuditServer server;
        server.SetResponses(std::move(specs));
        AUDIT_CHECK(server.Start(8));
        FetchOptions options;
        options.max_retries = 3;
        options.retry_backoff_ms = 10;
        options.read_timeout_ms = 1000;  // 远大于总超时
        options.connect_timeout_ms = 1000;
        options.total_timeout_ms = 600;
        options.filename = "total-timeout.bin";
        const auto begin = std::chrono::steady_clock::now();
        const FetchResult result = Fetch(server.Url(0), dir.str(), options);
        const double elapsed = MillisecondsSince(begin);
        std::printf("[DATA] 总超时: status=%s attempts=%d requests=%d elapsed=%.0fms\n",
                    go2cpp::bento::app::FetchStatusToString(result.status), result.attempts,
                    server.requests(), elapsed);
        AUDIT_CHECK_STATUS(result, FetchStatus::kRetryExhausted, "总超时（重试用尽）");
        AUDIT_CHECK_EQ(result.attempts, 4);
        AUDIT_CHECK_EQ(server.requests(), 4);
        AUDIT_CHECK_MSG(elapsed >= 1800.0 && elapsed < 6000.0,
                        "总超时耗时应约 4×600ms+退避（实际 " + std::to_string(elapsed) + "ms）");
        AUDIT_CHECK_MSG(result.message.find("超时") != std::string::npos,
                        "诊断应说明超时（实际 " + result.message + "）");
        CheckFailureCleanup(result, dir.path(), "总超时");
        server.Stop();
    }

    RemoveTree(dir.path());
}

// ---------------------------------------------------------------------------
// 场景 3：磁盘写失败
// ---------------------------------------------------------------------------

void ScenarioDiskWriteFailure() {
    std::printf("\n== 场景 3：磁盘满/写失败 ==\n");
    TempDir dir("diskfail");
    AUDIT_CHECK(dir.ok());
    if (!dir.ok()) {
        return;
    }

    constexpr std::uint32_t kSeed = 0x5005u;
    constexpr std::uint64_t kBody = 8ull * 1024 * 1024;
    std::vector<ResponseSpec> specs;
    ResponseSpec spec;
    spec.kind = ResponseKind::kBody;
    spec.head = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(kBody) +
                "\r\nConnection: close\r\n\r\n";
    spec.body_bytes = kBody;
    spec.seed = kSeed;
    specs.push_back(spec);
    AuditServer server;
    server.SetResponses(std::move(specs));
    AUDIT_CHECK(server.Start(4));

    // ---- (a) 只读目录：创建临时文件即失败 ----
    {
        const fs::path readonly = dir.path() / "readonly";
        std::error_code ec;
        fs::create_directories(readonly, ec);
        AUDIT_CHECK(!ec);
        AUDIT_CHECK_EQ(::chmod(readonly.c_str(), 0555), 0);
        FetchOptions options;
        options.max_retries = 3;
        options.retry_backoff_ms = 5;
        options.max_bytes = 16ull * 1024 * 1024;
        options.filename = "ro.bin";
        const FetchResult result = Fetch(server.Url(0), readonly.string(), options);
        std::printf("[DATA] 只读目录: status=%s attempts=%d message=%s\n",
                    go2cpp::bento::app::FetchStatusToString(result.status), result.attempts,
                    result.message.c_str());
        AUDIT_CHECK_STATUS(result, FetchStatus::kIoError, "只读目录");
        AUDIT_CHECK_EQ(result.attempts, 1);  // 本地 IO 错误不可重试
        AUDIT_CHECK(!result.ok());
        AUDIT_CHECK_EQ(CountEntries(readonly), 0);
        AUDIT_CHECK_EQ(::chmod(readonly.c_str(), 0755), 0);
        // 沙箱校验通过、本地 open 失败：此时还没有发起网络请求。
        AUDIT_CHECK_MSG(server.requests() == 0,
                        "本地 IO 失败时不应发起网络请求（实际 " +
                            std::to_string(server.requests()) + "）");
    }

    // ---- (b) RLIMIT_FSIZE：写入超限触发 EFBIG ----
    {
        struct rlimit original {};
        AUDIT_CHECK_EQ(::getrlimit(RLIMIT_FSIZE, &original), 0);
        struct rlimit tiny {};
        tiny.rlim_cur = 1024 * 1024;
        tiny.rlim_max = original.rlim_max;
        AUDIT_CHECK_EQ(::setrlimit(RLIMIT_FSIZE, &tiny), 0);
        FetchOptions options;
        options.max_retries = 3;
        options.retry_backoff_ms = 5;
        options.max_bytes = 16ull * 1024 * 1024;
        options.filename = "rlimit.bin";
        const FetchResult result = Fetch(server.Url(0), dir.str(), options);
        AUDIT_CHECK_EQ(::setrlimit(RLIMIT_FSIZE, &original), 0);  // 立刻恢复
        std::printf("[DATA] RLIMIT_FSIZE=1MiB: status=%s attempts=%d message=%s\n",
                    go2cpp::bento::app::FetchStatusToString(result.status), result.attempts,
                    result.message.c_str());
        AUDIT_CHECK_STATUS(result, FetchStatus::kIoError, "RLIMIT_FSIZE 写失败");
        AUDIT_CHECK_EQ(result.attempts, 1);
        AUDIT_CHECK_MSG(!HasTempArtifacts(dir.path()), "写失败后不应残留 .go2cpp-part.* 临时文件");
        AUDIT_CHECK(!fs::exists(dir.path() / "rlimit.bin"));
    }

    server.Stop();
    RemoveTree(dir.path());
}

// ---------------------------------------------------------------------------
// 场景 4：大文件流式超限
// ---------------------------------------------------------------------------

void ScenarioStreamingLimit() {
    std::printf("\n== 场景 4：大文件流式上限（默认 64 MiB）==\n");
    TempDir dir("limit");
    AUDIT_CHECK(dir.ok());
    if (!dir.ok()) {
        return;
    }

    constexpr std::uint64_t kSixtyFourMiB = 64ull * 1024 * 1024;
    constexpr std::uint64_t kSixtyFiveMiB = 65ull * 1024 * 1024;
    constexpr std::uint32_t kSeed = 0x6006u;

    std::vector<ResponseSpec> specs;
    ResponseSpec oversized_cl;
    oversized_cl.kind = ResponseKind::kBody;
    oversized_cl.head = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(kSixtyFiveMiB) +
                        "\r\nConnection: close\r\n\r\n";
    oversized_cl.body_bytes = kSixtyFiveMiB;
    oversized_cl.seed = kSeed;
    specs.push_back(oversized_cl);

    ResponseSpec oversized_chunked = oversized_cl;
    oversized_chunked.head =
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n";
    oversized_chunked.chunk_bytes = 256 * 1024;
    specs.push_back(oversized_chunked);

    ResponseSpec exact;
    exact.kind = ResponseKind::kBody;
    exact.head = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(kSixtyFourMiB) +
                 "\r\nConnection: close\r\n\r\n";
    exact.body_bytes = kSixtyFourMiB;
    exact.seed = kSeed;
    specs.push_back(exact);

    AuditServer server;
    server.SetResponses(std::move(specs));
    if (!server.Start(3)) {
        AUDIT_CHECK_MSG(false, "启动大文件服务器失败");
        return;
    }

    // ---- (a) Content-Length 预检：一个字节都不该发 ----
    {
        const std::uint64_t before = server.bytes_sent();
        FetchOptions options;
        options.filename = "oversized-cl.bin";
        options.max_retries = 3;
        options.retry_backoff_ms = 5;
        const auto begin = std::chrono::steady_clock::now();
        const FetchResult result = Fetch(server.Url(0), dir.str(), options);
        const double elapsed = MillisecondsSince(begin);
        const std::uint64_t sent = server.bytes_sent() - before;
        std::printf("[DATA] 65MiB/CL 预检: status=%s attempts=%d elapsed=%.1fms sent=%llu\n",
                    go2cpp::bento::app::FetchStatusToString(result.status), result.attempts, elapsed,
                    static_cast<unsigned long long>(sent));
        AUDIT_CHECK_STATUS(result, FetchStatus::kTooLarge, "65 MiB Content-Length");
        AUDIT_CHECK_EQ(result.attempts, 1);  // 超过大小上限不可重试
        AUDIT_CHECK_MSG(sent < 1024 * 1024,
                        "CL 预检应在响应体之前拒绝（服务端实际发送 " + std::to_string(sent) +
                            " 字节）");
        CheckFailureCleanup(result, dir.path(), "65 MiB CL");
        AUDIT_CHECK(!fs::exists(dir.path() / "oversized-cl.bin"));
    }

    // ---- (b) chunked：边下边累计，到 64 MiB 中止 ----
    {
        const std::uint64_t before = server.bytes_sent();
        FetchOptions options;
        options.filename = "oversized-chunked.bin";
        options.max_retries = 0;
        const auto begin = std::chrono::steady_clock::now();
        const FetchResult result = Fetch(server.Url(1), dir.str(), options);
        const double elapsed = MillisecondsSince(begin);
        const std::uint64_t sent = server.bytes_sent() - before;
        std::printf("[DATA] 65MiB/chunked: status=%s attempts=%d elapsed=%.1fms sent=%llu\n",
                    go2cpp::bento::app::FetchStatusToString(result.status), result.attempts, elapsed,
                    static_cast<unsigned long long>(sent));
        AUDIT_CHECK_STATUS(result, FetchStatus::kTooLarge, "65 MiB chunked");
        AUDIT_CHECK_EQ(result.attempts, 1);
        CheckFailureCleanup(result, dir.path(), "65 MiB chunked");
        AUDIT_CHECK(!fs::exists(dir.path() / "oversized-chunked.bin"));
    }

    // ---- (c) 恰好 64 MiB：边界应成功 ----
    {
        FetchOptions options;
        options.filename = "exact-limit.bin";
        options.max_retries = 0;
        const auto begin = std::chrono::steady_clock::now();
        const FetchResult result = Fetch(server.Url(2), dir.str(), options);
        const double elapsed = MillisecondsSince(begin);
        Report("64MiB 恰好等于上限", kSixtyFourMiB, elapsed, PeakRss(), OpenFdCount());
        AUDIT_CHECK_STATUS(result, FetchStatus::kOk, "恰好 64 MiB");
        AUDIT_CHECK_EQ(result.bytes, kSixtyFourMiB);
        AUDIT_CHECK_EQ(result.sha256, PayloadSha256(kSeed, kSixtyFourMiB));
        AUDIT_CHECK(!HasTempArtifacts(dir.path()));
    }

    // ---- (d) 参数硬上限 ----
    {
        FetchOptions options;
        options.max_bytes = kFetchMaxBytesCeiling + 1;
        const FetchResult result = Fetch(server.Url(2), dir.str(), options);
        AUDIT_CHECK_STATUS(result, FetchStatus::kInvalidArgument, "max_bytes 超过 4 GiB 硬上限");
    }

    server.Stop();
    RemoveTree(dir.path());
}

// ---------------------------------------------------------------------------
// 场景 5：100 个混合大小下载并发
// ---------------------------------------------------------------------------

void ScenarioConcurrent100() {
    std::printf("\n== 场景 5：100 个混合大小下载并发 ==\n");
    TempDir dir("concurrent100");
    AUDIT_CHECK(dir.ok());
    if (!dir.ok()) {
        return;
    }

    struct Case {
        std::uint64_t bytes;
        std::uint32_t seed;
    };
    const Case cases[] = {
        {1024, 0x7001u},
        {256 * 1024, 0x7002u},
        {1024 * 1024, 0x7003u},
    };
    constexpr int kPerCase = 34;  // 34×3 = 102，略多于 100
    constexpr int kTotal = kPerCase * 3;

    std::vector<ResponseSpec> specs;
    std::vector<std::string> expected;
    for (const Case& item : cases) {
        ResponseSpec spec;
        spec.kind = ResponseKind::kBody;
        spec.head = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(item.bytes) +
                    "\r\nConnection: close\r\n\r\n";
        spec.body_bytes = item.bytes;
        spec.seed = item.seed;
        specs.push_back(spec);
        expected.push_back(PayloadSha256(item.seed, item.bytes));
    }

    AuditServer server;
    server.SetResponses(std::move(specs));
    if (!server.Start(8)) {
        AUDIT_CHECK_MSG(false, "启动混合并发服务器失败");
        return;
    }

    std::vector<FetchResult> results(static_cast<std::size_t>(kTotal));
    std::atomic<int> spawn_failures{0};
    std::uint64_t planned_bytes = 0;
    for (int index = 0; index < kTotal; ++index) {
        planned_bytes += cases[index % 3].bytes;
    }

    ResourceSampler sampler;
    sampler.Start();
    const auto started = std::chrono::steady_clock::now();
    {
        ThreadGroup group;
        for (int index = 0; index < kTotal; ++index) {
            const std::size_t case_index = static_cast<std::size_t>(index % 3);
            const bool spawned = group.Spawn(
                [&server, &dir, &results, &expected, index, case_index]() {
                    FetchOptions options;
                    options.filename = "mix-" + std::to_string(index) + ".bin";
                    options.max_bytes = 4 * 1024 * 1024;
                    options.max_retries = 1;
                    options.retry_backoff_ms = 5;
                    options.connect_timeout_ms = 10 * 1000;
                    options.read_timeout_ms = 20 * 1000;
                    options.total_timeout_ms = 30 * 1000;
                    options.expected_sha256 = expected[case_index];
                    results[static_cast<std::size_t>(index)] =
                        Fetch(server.Url(case_index), dir.str(), options);
                });
            if (!spawned) {
                spawn_failures.fetch_add(1, std::memory_order_relaxed);
            }
        }
        AUDIT_CHECK_EQ(spawn_failures.load(), 0);
        group.JoinAll();
    }
    const double elapsed = MillisecondsSince(started);
    sampler.Stop();

    int ok_count = 0;
    for (const FetchResult& result : results) {
        if (result.ok()) {
            ++ok_count;
        }
    }
    AUDIT_CHECK_MSG(ok_count == kTotal,
                    "100+ 混合并发应全部成功（实际 " + std::to_string(ok_count) + "）");
    AUDIT_CHECK_EQ(CountEntries(dir.path()), static_cast<std::size_t>(kTotal));
    AUDIT_CHECK_MSG(!HasTempArtifacts(dir.path()), "混合并发后不应残留临时文件");
    AUDIT_CHECK_EQ(DirectoryBytes(dir.path()), planned_bytes);
    Report("100+ 混合并发下载", planned_bytes, elapsed, sampler.peak_rss(), sampler.peak_fd());

    server.Stop();
    RemoveTree(dir.path());
}

}  // namespace

int main() {
    // SIGPIPE：对端提前关闭时 write 返回 EPIPE 而不是杀进程。
    ::signal(SIGPIPE, SIG_IGN);
    // SIGXFSZ：RLIMIT_FSIZE 触顶时让 write 返回 EFBIG 而不是杀进程。
    ::signal(SIGXFSZ, SIG_IGN);

    std::printf("fetch 压测/场景审计开始（pid=%d）\n", static_cast<int>(::getpid()));
    Stress100MiB();
    Stress1000Concurrent();
    ProbeSameTargetConcurrency();
    ScenarioRedirectChain();
    ScenarioTimeoutRetry();
    ScenarioDiskWriteFailure();
    ScenarioStreamingLimit();
    ScenarioConcurrent100();
    return audit::Finish("audit_fetch_stress");
}
