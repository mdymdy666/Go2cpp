/**
 * @file audit_fetch_random.cpp
 * @brief fetch 模块随机响应测试（1180 个随机/半随机用例）。
 * @details 覆盖：合法 Content-Length、合法 chunked、无 Content-Length（EOF）、
 *          截断、重定向循环、非法响应头、chunk 语法错误、超时、4xx/5xx、
 *          接受即关闭。每个用例断言：
 *          - 不崩溃（进程存活到结束）；
 *          - 错误码落在该形态允许的集合内；
 *          - 成功时字节数与 SHA-256 精确匹配；
 *          - 失败时不留下目标文件、不残留 `.go2cpp-part.*` 临时文件；
 *          最后还断言 fd 与 RSS 不随用例数增长（无泄漏）。
 *
 * @author go2cpp contributors
 * @date 2026-10-29
 * @copyright MIT License
 */

#include "audit_fetch_support.hpp"

#include <random>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

using namespace audit;  // NOLINT(google-build-using-namespace)：审计程序局部使用

/// 一个随机用例：响应脚本 + 合法结果集合 + 成功时的校验基准。
struct RandomCase final {
    ResponseSpec spec;
    std::vector<FetchStatus> allowed;
    std::string label;
    bool expects_success{false};
    std::uint64_t expected_bytes{0};
    std::string expected_sha;
    int max_retries{0};
    std::uint32_t read_timeout_ms{3000};
    std::uint32_t total_timeout_ms{5000};
    std::size_t max_redirects{5};
    std::uint64_t max_bytes{kFetchDefaultMaxBytes};
};

/// 构造响应头（含 CRLFCRLF）。
std::string Head(int status, const std::string& reason, std::uint64_t content_length,
                 const std::string& extra = {}) {
    return "HTTP/1.1 " + std::to_string(status) + " " + reason +
           "\r\nContent-Length: " + std::to_string(content_length) + "\r\n" + extra +
           "Connection: close\r\n\r\n";
}

/// 生成确定性字节串（用例 body 与期望摘要都用它）。
std::string MakeBody(std::uint32_t seed, std::uint64_t bytes) {
    PayloadReader reader(seed, bytes);
    std::string out(static_cast<std::size_t>(bytes), '\0');
    std::size_t filled = 0;
    while (filled < out.size()) {
        const std::size_t got = reader.Read(&out[filled], out.size() - filled);
        if (got == 0) {
            break;
        }
        filled += got;
    }
    return out;
}

/// 构造一个 302 响应（相对 Location）。
ResponseSpec RedirectTo(const std::string& location) {
    ResponseSpec spec;
    spec.kind = ResponseKind::kRaw;
    spec.raw = "HTTP/1.1 302 Found\r\nLocation: " + location +
               "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    return spec;
}

/// 构造 200 + 固定 body 的用例（成功）。
RandomCase SuccessCase(std::uint32_t seed, std::uint64_t bytes, bool chunked,
                       std::uint64_t chunk_bytes) {
    RandomCase item;
    item.spec.kind = ResponseKind::kBody;
    item.spec.head = chunked
                         ? "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
                         : Head(200, "OK", bytes);
    item.spec.body_bytes = bytes;
    item.spec.seed = seed;
    item.spec.chunk_bytes = chunked ? chunk_bytes : 0;
    item.allowed = {FetchStatus::kOk};
    item.label = chunked ? "chunked" : "content-length";
    item.expects_success = true;
    item.expected_bytes = bytes;
    item.expected_sha = PayloadSha256(seed, bytes);
    return item;
}

/**
 * @brief 生成全部随机用例。
 * @details 用例与服务器响应表按下标一一对应（case[i] ↔ /r/i），因此重定向
 *          用例可以在构造期就把相对 Location 指向自己的下标。
 */
std::vector<RandomCase> BuildCases() {
    std::mt19937_64 rng(0x5eed5eed1234abcdULL);
    std::vector<RandomCase> cases;
    cases.reserve(1200);

    // ---- 1) 合法 Content-Length（180）----
    for (int index = 0; index < 180; ++index) {
        const std::uint64_t bytes = rng() % 65537;
        RandomCase item = SuccessCase(static_cast<std::uint32_t>(rng()), bytes, false, 0);
        item.label = "合法-CL";
        cases.push_back(std::move(item));
    }

    // ---- 2) 合法 chunked（180，chunk 大小随机）----
    for (int index = 0; index < 180; ++index) {
        const std::uint64_t bytes = rng() % 65537;
        const std::uint64_t chunk = 1 + rng() % 8192;
        RandomCase item = SuccessCase(static_cast<std::uint32_t>(rng()), bytes, true, chunk);
        item.label = "合法-chunked";
        cases.push_back(std::move(item));
    }

    // ---- 3) 无 Content-Length（EOF 模式，180）----
    for (int index = 0; index < 180; ++index) {
        const std::uint64_t bytes = rng() % 65537;
        RandomCase item = SuccessCase(static_cast<std::uint32_t>(rng()), bytes, false, 0);
        item.spec.head = "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n";
        item.label = "无CL-EOF";
        cases.push_back(std::move(item));
    }

    // ---- 4) 截断：声明 CL 大于实际（150）----
    for (int index = 0; index < 150; ++index) {
        const std::uint64_t actual = rng() % 10000;
        const std::uint64_t declared = actual + 1 + rng() % 4096;
        RandomCase item;
        item.spec.kind = ResponseKind::kRaw;
        item.spec.raw = Head(200, "OK", declared) + MakeBody(static_cast<std::uint32_t>(rng()), actual);
        item.allowed = {FetchStatus::kTruncated};
        item.label = "截断";
        cases.push_back(std::move(item));
    }

    // ---- 5) 重定向循环（自指 120 + 两跳互指 20 对）----
    for (int index = 0; index < 120; ++index) {
        const std::size_t self = cases.size();
        RandomCase item;
        item.spec = RedirectTo("/r/" + std::to_string(self));
        item.allowed = {FetchStatus::kHttpError};
        item.label = "重定向-自指";
        item.max_redirects = rng() % 6;  // 0..5
        cases.push_back(std::move(item));
    }
    for (int index = 0; index < 20; ++index) {
        const std::size_t first = cases.size();
        const std::size_t second = first + 1;
        RandomCase a;
        a.spec = RedirectTo("/r/" + std::to_string(second));
        a.allowed = {FetchStatus::kHttpError};
        a.label = "重定向-互指";
        a.max_redirects = 5;
        cases.push_back(std::move(a));
        RandomCase b;
        b.spec = RedirectTo("/r/" + std::to_string(first));
        b.allowed = {FetchStatus::kHttpError};
        b.label = "重定向-互指";
        b.max_redirects = 5;
        cases.push_back(std::move(b));
    }

    // ---- 6) 非法响应头（150）----
    for (int index = 0; index < 150; ++index) {
        RandomCase item;
        item.spec.kind = ResponseKind::kRaw;
        item.allowed = {FetchStatus::kProtocolError};
        switch (index % 9) {
            case 0:
                item.spec.raw = "NOT-HTTP/1.1 200\r\n\r\n";
                item.label = "非法-状态行";
                break;
            case 1:
                item.spec.raw = "HTTP/1.1 999 Weird\r\nContent-Length: 0\r\n\r\n";
                item.label = "非法-状态码999";
                break;
            case 2:
                item.spec.raw = "HTTP/1.1 200 OK\r\nContent-Length: abc\r\n\r\n";
                item.label = "非法-Content-Length";
                break;
            case 3:
                item.spec.raw =
                    "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\nhello!";
                item.label = "非法-CL重复不一致";
                break;
            case 4: {
                std::string head = "HTTP/1.1 200 OK\r\nX-Filler: ";
                head += std::string(70 * 1024, 'a');
                head += "\r\nContent-Length: 0\r\n\r\n";
                item.spec.raw = std::move(head);
                item.label = "非法-头超64KiB";
                break;
            }
            case 5:
                item.spec.raw =
                    "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nZZZ\r\nhello\r\n0\r\n\r\n";
                item.label = "非法-chunk大小";
                break;
            case 6:
                item.spec.raw =
                    "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhelloX\r\n0\r\n\r\n";
                item.label = "非法-chunk缺CRLF";
                break;
            case 7:
                item.spec.raw =
                    "HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\nContent-Length: 5\r\n\r\nhello";
                item.label = "非法-Content-Encoding";
                break;
            default:
                item.spec.raw = "HTTP/1.1";
                item.label = "非法-状态行截断";
                break;
        }
        cases.push_back(std::move(item));
    }

    // ---- 7) 超时：静默 / 慢滴流（80）----
    for (int index = 0; index < 80; ++index) {
        RandomCase item;
        if (index % 2 == 0) {
            item.spec.kind = ResponseKind::kSilent;
            item.label = "超时-静默";
        } else {
            item.spec.kind = ResponseKind::kSlowDrip;
            item.spec.head = Head(200, "OK", 60);
            item.spec.body_bytes = 60;
            item.spec.drip_ms = 40;
            item.spec.seed = static_cast<std::uint32_t>(rng());
            item.label = "超时-慢滴流";
        }
        item.allowed = {FetchStatus::kTimeout};
        item.read_timeout_ms = 80;
        item.total_timeout_ms = 400;
        cases.push_back(std::move(item));
    }

    // ---- 8) 4xx / 5xx（100）----
    const int error_codes[] = {400, 403, 404, 418, 500, 502, 503, 408, 429, 501};
    for (int index = 0; index < 100; ++index) {
        const int code = error_codes[index % 10];
        RandomCase item;
        item.spec.kind = ResponseKind::kRaw;
        item.spec.raw = Head(code, "Error", 4) + "oops";
        item.allowed = {FetchStatus::kHttpError};
        item.label = "HTTP-" + std::to_string(code);
        cases.push_back(std::move(item));
    }

    // ---- 9) 接受即关闭（40）----
    for (int index = 0; index < 40; ++index) {
        RandomCase item;
        item.spec.kind = ResponseKind::kCloseNow;
        item.allowed = {FetchStatus::kProtocolError, FetchStatus::kTruncated,
                        FetchStatus::kConnectFailed};
        item.label = "接受即关闭";
        cases.push_back(std::move(item));
    }

    return cases;
}

/// 把状态集合渲染成可读文本。
std::string DescribeAllowed(const std::vector<FetchStatus>& allowed) {
    std::string out = "{";
    for (std::size_t index = 0; index < allowed.size(); ++index) {
        if (index != 0) {
            out += ",";
        }
        out += go2cpp::bento::app::FetchStatusToString(allowed[index]);
    }
    out += "}";
    return out;
}

}  // namespace

int main() {
    ::signal(SIGPIPE, SIG_IGN);
    ::signal(SIGXFSZ, SIG_IGN);

    std::printf("fetch 随机响应审计开始（pid=%d）\n", static_cast<int>(::getpid()));
    std::vector<RandomCase> cases = BuildCases();
    std::printf("随机用例数=%zu\n", cases.size());
    AUDIT_CHECK_MSG(cases.size() >= 1000, "随机用例数应达到 1000+");

    TempDir dir("random");
    AUDIT_CHECK(dir.ok());
    if (!dir.ok()) {
        return audit::Finish("audit_fetch_random");
    }

    // 快速用例（除超时类）与慢用例分两个服务器，避免慢响应占满 acceptor。
    std::vector<std::size_t> fast_indices;
    std::vector<std::size_t> slow_indices;
    for (std::size_t index = 0; index < cases.size(); ++index) {
        if (cases[index].read_timeout_ms <= 100) {
            slow_indices.push_back(index);
        } else {
            fast_indices.push_back(index);
        }
    }

    std::vector<ResponseSpec> specs;
    specs.reserve(cases.size());
    for (const RandomCase& item : cases) {
        specs.push_back(item.spec);
    }

    // 慢用例服务器：只放慢用例，acceptors 足够多让它们并发跑。
    std::vector<ResponseSpec> slow_specs;
    slow_specs.reserve(slow_indices.size());
    for (std::size_t index : slow_indices) {
        slow_specs.push_back(cases[index].spec);
    }

    // 为了复用同一套 URL 下标规则，这里让快速/慢速服务器各自用原始下标寻址：
    // 快速服务器按「排序后的位置」寻址，因此需要一张位置表。
    std::vector<ResponseSpec> fast_specs;
    fast_specs.reserve(fast_indices.size());
    for (std::size_t index : fast_indices) {
        fast_specs.push_back(cases[index].spec);
    }

    AuditServer fast_server;
    fast_server.SetResponses(std::move(fast_specs));
    AuditServer slow_server;
    slow_server.SetResponses(std::move(slow_specs));
    const bool fast_ok = fast_server.Start(8);
    const bool slow_ok = slow_server.Start(8);
    AUDIT_CHECK(fast_ok && slow_ok);
    if (!fast_ok || !slow_ok) {
        return audit::Finish("audit_fetch_random");
    }

    // 基线在服务器启动之后取：acceptor 线程与监听 fd 不算 fetch 的开销。
    const std::size_t baseline_fd = OpenFdCount();
    const std::uint64_t baseline_rss = CurrentRss();

    int success_cases = 0;
    std::uint64_t success_bytes = 0;
    int unexpected = 0;
    const auto started = std::chrono::steady_clock::now();

    // ---- 快速用例 ----
    for (std::size_t position = 0; position < fast_indices.size(); ++position) {
        const std::size_t case_index = fast_indices[position];
        const RandomCase& item = cases[case_index];
        FetchOptions options;
        options.filename = "case-" + std::to_string(case_index) + ".bin";
        options.max_retries = item.max_retries;
        options.retry_backoff_ms = 1;
        options.read_timeout_ms = item.read_timeout_ms;
        options.total_timeout_ms = item.total_timeout_ms;
        options.connect_timeout_ms = 1000;
        options.max_redirects = item.max_redirects;
        options.max_bytes = item.max_bytes;
        const FetchResult result = Fetch(fast_server.Url(position), dir.str(), options);

        bool allowed = false;
        for (FetchStatus candidate : item.allowed) {
            if (result.status == candidate) {
                allowed = true;
                break;
            }
        }
        if (!allowed) {
            ++unexpected;
            if (unexpected <= 10) {
                std::fprintf(stderr, "[FAIL] 用例 #%zu（%s）: 期望 %s，实际 %s（%s）\n",
                             case_index, item.label.c_str(),
                             DescribeAllowed(item.allowed).c_str(),
                             go2cpp::bento::app::FetchStatusToString(result.status),
                             result.message.c_str());
            }
        }
        AUDIT_CHECK_MSG(allowed, "用例 #" + std::to_string(case_index) + "（" + item.label +
                                     "）状态应属于 " + DescribeAllowed(item.allowed));

        if (result.ok()) {
            ++success_cases;
            success_bytes += result.bytes;
            AUDIT_CHECK_EQ(result.bytes, item.expected_bytes);
            AUDIT_CHECK_EQ(result.sha256, item.expected_sha);
            AUDIT_CHECK_EQ(result.path.empty(), false);
        } else {
            AUDIT_CHECK_MSG(result.path.empty(),
                            "用例 #" + std::to_string(case_index) + " 失败时 path 应为空");
        }
    }

    // ---- 慢用例（超时）----
    for (std::size_t position = 0; position < slow_indices.size(); ++position) {
        const std::size_t case_index = slow_indices[position];
        const RandomCase& item = cases[case_index];
        FetchOptions options;
        options.filename = "case-" + std::to_string(case_index) + ".bin";
        options.max_retries = item.max_retries;
        options.retry_backoff_ms = 1;
        options.read_timeout_ms = item.read_timeout_ms;
        options.total_timeout_ms = item.total_timeout_ms;
        options.connect_timeout_ms = 500;
        options.max_redirects = item.max_redirects;
        options.max_bytes = item.max_bytes;
        const FetchResult result = Fetch(slow_server.Url(position), dir.str(), options);
        bool allowed = false;
        for (FetchStatus candidate : item.allowed) {
            if (result.status == candidate) {
                allowed = true;
                break;
            }
        }
        if (!allowed) {
            ++unexpected;
            if (unexpected <= 10) {
                std::fprintf(stderr, "[FAIL] 慢用例 #%zu（%s）: 期望 %s，实际 %s（%s）\n",
                             case_index, item.label.c_str(),
                             DescribeAllowed(item.allowed).c_str(),
                             go2cpp::bento::app::FetchStatusToString(result.status),
                             result.message.c_str());
            }
        }
        AUDIT_CHECK_MSG(allowed, "慢用例 #" + std::to_string(case_index) + "（" + item.label +
                                     "）状态应属于 " + DescribeAllowed(item.allowed));
        AUDIT_CHECK(result.path.empty() || result.ok());
    }

    const double elapsed = MillisecondsSince(started);

    // ---- 全局不变量 ----
    AUDIT_CHECK_EQ(CountEntries(dir.path()), static_cast<std::size_t>(success_cases));
    AUDIT_CHECK_EQ(DirectoryBytes(dir.path()), success_bytes);
    AUDIT_CHECK_MSG(!HasTempArtifacts(dir.path()),
                    "随机用例结束后不应残留 .go2cpp-part.* 临时文件");

    // 先停服务器再测资源：慢用例的处理线程此时可能仍持有连接 fd，
    // 那是服务器的资源，不能算成 fetch 的泄漏。
    fast_server.Stop();
    slow_server.Stop();

    const std::size_t end_fd = OpenFdCount();
    const std::uint64_t end_rss = CurrentRss();
    std::printf("[DATA] 随机用例: total=%zu fast=%zu slow=%zu success=%zu elapsed=%.0fms\n",
                cases.size(), fast_indices.size(), slow_indices.size(),
                static_cast<std::size_t>(success_cases), elapsed);
    std::printf("[DATA] 资源: fd %zu→%zu, rss %.1fMiB→%.1fMiB\n", baseline_fd, end_fd,
                static_cast<double>(baseline_rss) / (1024.0 * 1024.0),
                static_cast<double>(end_rss) / (1024.0 * 1024.0));
    AUDIT_CHECK_MSG(end_fd <= baseline_fd + 2,
                    "用例跑完后 fd 应回到基线（" + std::to_string(baseline_fd) + " → " +
                        std::to_string(end_fd) + "），说明没有泄漏");
    AUDIT_CHECK_MSG(end_rss < baseline_rss + 64ull * 1024 * 1024,
                    "用例跑完后 RSS 增长应 < 64 MiB（实际 " +
                        std::to_string((end_rss > baseline_rss ? end_rss - baseline_rss : 0) /
                                       (1024 * 1024)) +
                        " MiB）");

    RemoveTree(dir.path());
    return audit::Finish("audit_fetch_random");
}
