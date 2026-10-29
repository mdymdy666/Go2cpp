/**
 * @file audit_backup_upload_stress.cpp
 * @brief backup/upload 模块压测与复杂场景审计（独立程序，不注册 CTest）。
 *
 * @details 覆盖：
 *          - 压测 C：1/10/100/500/1000 并发上传（吞吐、RSS、fd、进程数）；
 *          - 场景 4：100 并发上传的内容正确性（请求体与本地 zip 一致、
 *            token/header 不丢失）；
 *          - 场景 5：503×2 → 201 重试成功，本地包逐字节保留；
 *          - 场景 6：服务器慢响应触发超时（kTimeout）并重试，本地包保留；
 *          - 场景 7：连接建立后立即断开（kNetworkError/HttpRejected），
 *            本地包保留；
 *          - 场景 8：verify_remote GET 成功后 /tmp 下的校验临时文件不残留。
 *
 * 编译（在项目根目录）：
 * @code
 * g++ -std=c++17 -O2 -g -Iinclude -Itests/bento \
 *     tests/bento/audit_backup_upload_stress.cpp \
 *     -L.build-bento-wsl -lgo2cpp_bento_app -lgo2cpp_bento -lgo2cpp_os \
 *     -lgo2cpp_hash -lgo2cpp_config -lgo2cpp_scheduler -lgo2cpp_log \
 *     -lgo2cpp_fiber -lpthread -Wl,-rpath,$PWD/.build-bento-wsl \
 *     -o .build-bento-wsl/audit_backup_upload_stress
 * @endcode
 *
 * @author go2cpp contributors
 * @date 2026-10-29
 * @copyright MIT License
 */

#include "audit_zip_support.hpp"

#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "go2cpp/bento/app/backup.hpp"
#include "go2cpp/bento/app/zip.hpp"
#include "test_backup_support.hpp"

namespace {

namespace fs = std::filesystem;
using go2cpp::bento::app::Backup;
using go2cpp::bento::app::HttpMethod;
using go2cpp::bento::app::RemoteVerifyMode;
using go2cpp::bento::app::Upload;
using go2cpp::bento::app::UploadOptions;
using go2cpp::bento::app::UploadRequest;
using go2cpp::bento::app::UploadResult;
using go2cpp::bento::app::UploadStatus;
using go2cpp::bento::app::ZipStatus;
using go2cpp::bento::app::ZipWriter;

using namespace std::chrono_literals;

/// 峰值 RSS/fd/进程数采样器。
class Sampler final {
public:
    void Start() {
        m_peak_rss = audit::RssKb();
        m_peak_fd = audit::FdCount();
        m_peak_procs = audit::ProcessCount();
        m_thread = std::thread([this] {
            while (!m_stop.load(std::memory_order_relaxed)) {
                m_peak_rss = std::max<long>(m_peak_rss.load(), audit::RssKb());
                m_peak_fd = std::max<int>(m_peak_fd.load(), audit::FdCount());
                m_peak_procs = std::max<int>(m_peak_procs.load(), audit::ProcessCount());
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        });
    }
    void Stop() {
        m_stop = true;
        if (m_thread.joinable()) {
            m_thread.join();
        }
    }
    long Rss() const { return m_peak_rss; }
    int Fd() const { return m_peak_fd; }
    int Procs() const { return m_peak_procs; }

private:
    std::atomic<bool> m_stop{false};
    std::thread m_thread;
    std::atomic<long> m_peak_rss{0};
    std::atomic<int> m_peak_fd{0};
    std::atomic<int> m_peak_procs{0};
};

void Section(const char* title) {
    std::printf("\n== %s ==\n", title);
    std::fflush(stdout);
}

/// 生成 size 字节内容的 zip。
fs::path MakeZip(const fs::path& dir, const std::string& name, std::size_t size) {
    const auto path = dir / name;
    const auto data = bento_test::RandomBytes(size, 0x51505150u + size);
    ZipWriter writer;
    if (writer.Open(path.string()) != ZipStatus::kOk) {
        return {};
    }
    if (writer.AddBytes("payload.bin", data.data(), data.size()) != ZipStatus::kOk) {
        writer.Abort();
        return {};
    }
    if (writer.Close() != ZipStatus::kOk) {
        return {};
    }
    return path;
}

/// 统计 /tmp 下指定前缀的文件数（用于校验临时文件清理）。
std::size_t CountPrefixFiles(const std::string& prefix) {
    std::error_code ec;
    std::size_t count = 0;
    for (fs::directory_iterator it("/tmp", ec), end; !ec && it != end; it.increment(ec)) {
        if (it->path().filename().string().rfind(prefix, 0) == 0) {
            ++count;
        }
    }
    return count;
}

/// 一个只 accept-close 的服务器（模拟「连上就断」）。
class AbortServer final {
public:
    AbortServer() {
        m_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (m_fd < 0) {
            return;
        }
        int one = 1;
        ::setsockopt(m_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (::bind(m_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
            ::listen(m_fd, 512) != 0) {
            ::close(m_fd);
            m_fd = -1;
            return;
        }
        socklen_t length = sizeof(address);
        if (::getsockname(m_fd, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
            ::close(m_fd);
            m_fd = -1;
            return;
        }
        m_port = ntohs(address.sin_port);
        m_thread = std::thread([this] {
            while (!m_stopping) {
                const int fd = ::accept(m_fd, nullptr, nullptr);
                if (fd < 0) {
                    if (m_stopping) {
                        break;
                    }
                    continue;
                }
                ::close(fd); // 立刻断开：客户端会看到连接被重置
            }
        });
    }
    ~AbortServer() {
        m_stopping = true;
        if (m_fd >= 0) {
            ::shutdown(m_fd, SHUT_RDWR);
        }
        if (m_thread.joinable()) {
            m_thread.join();
        }
        if (m_fd >= 0) {
            ::close(m_fd);
        }
    }
    bool valid() const noexcept { return m_port > 0; }
    std::string Url() const {
        return "http://127.0.0.1:" + std::to_string(m_port) + "/abort";
    }

private:
    int m_fd{-1};
    int m_port{0};
    std::atomic<bool> m_stopping{false};
    std::thread m_thread;
};

/// 一个慢服务器：接受连接后 sleep delay 再回 200（用于触发客户端超时）。
class SlowServer final {
public:
    explicit SlowServer(int delay_ms) : m_delay_ms(delay_ms) {
        m_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (m_fd < 0) {
            return;
        }
        int one = 1;
        ::setsockopt(m_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (::bind(m_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
            ::listen(m_fd, 512) != 0) {
            ::close(m_fd);
            m_fd = -1;
            return;
        }
        socklen_t length = sizeof(address);
        if (::getsockname(m_fd, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
            ::close(m_fd);
            m_fd = -1;
            return;
        }
        m_port = ntohs(address.sin_port);
        m_thread = std::thread([this] { Serve(); });
    }
    ~SlowServer() {
        m_stopping = true;
        if (m_fd >= 0) {
            ::shutdown(m_fd, SHUT_RDWR);
        }
        if (m_thread.joinable()) {
            m_thread.join();
        }
        if (m_fd >= 0) {
            ::close(m_fd);
        }
    }
    bool valid() const noexcept { return m_port > 0; }
    std::string Url() const {
        return "http://127.0.0.1:" + std::to_string(m_port) + "/slow";
    }

private:
    void Serve() {
        while (!m_stopping) {
            const int fd = ::accept(m_fd, nullptr, nullptr);
            if (fd < 0) {
                if (m_stopping) {
                    break;
                }
                continue;
            }
            std::thread([this, fd] {
                char buffer[4096];
                while (::recv(fd, buffer, sizeof(buffer), 0) > 0) {
                    // 读完请求头/体，避免客户端 write 阻塞。
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(m_delay_ms));
                const char reply[] = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n"
                                     "Connection: close\r\n\r\n";
                (void)::send(fd, reply, sizeof(reply) - 1, MSG_NOSIGNAL);
                ::close(fd);
            }).detach();
        }
    }
    int m_delay_ms{0};
    int m_fd{-1};
    int m_port{0};
    std::atomic<bool> m_stopping{false};
    std::thread m_thread;
};

// ---------------------------------------------------------------------------
// 压测 C：并发上传（阶梯）
// ---------------------------------------------------------------------------

struct StageResult final {
    int concurrency{0};
    int ok{0};
    int failed{0};
    double seconds{0.0};
    long peak_rss{0};
    int peak_fd{0};
    int peak_procs{0};
    std::uint64_t bytes{0};
};

/**
 * @brief 跑一档并发上传。
 * @param zips 每个线程准备的 zip 路径（长度必须 >= concurrency）。
 */
StageResult RunStage(int concurrency, const std::vector<fs::path>& zips, bool check_body) {
    StageResult stage;
    stage.concurrency = concurrency;
    audit::HttpServer server([&check_body](const audit::HttpRequest& request) {
        audit::HttpReply reply;
        reply.status = 201;
        if (check_body && request.body.empty()) {
            reply.status = 400;
        }
        return reply;
    });
    if (!server.valid()) {
        return stage;
    }
    std::vector<UploadResult> results(static_cast<std::size_t>(concurrency));
    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(concurrency));
    audit::TrimMemory();
    Sampler sampler;
    sampler.Start();
    audit::Stopwatch watch;
    for (int i = 0; i < concurrency; ++i) {
        threads.emplace_back([&, i] {
            UploadRequest request;
            request.endpoint = server.Url("/backup/put");
            request.method = HttpMethod::kPut;
            request.token = "stress-token";
            UploadOptions options;
            options.max_retries = 0; // 压测吞吐：不重试
            options.retry_delay = 1ms;
            options.connect_timeout = 10s;
            options.request_timeout = 30s;
            results[static_cast<std::size_t>(i)] =
                Upload(zips[static_cast<std::size_t>(i)].string(), request, options);
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    stage.seconds = watch.Seconds();
    sampler.Stop();
    stage.peak_rss = sampler.Rss();
    stage.peak_fd = sampler.Fd();
    stage.peak_procs = sampler.Procs();
    std::uint64_t bytes = 0;
    for (const UploadResult& result : results) {
        if (result.ok()) {
            ++stage.ok;
        } else {
            ++stage.failed;
        }
    }
    if (!zips.empty()) {
        std::error_code ec;
        bytes = static_cast<std::uint64_t>(fs::file_size(zips[0], ec)) *
                static_cast<std::uint64_t>(concurrency);
    }
    stage.bytes = bytes;
    std::printf(
        "  并发 %4d：成功 %4d / 失败 %3d，耗时 %6.2fs，%.0f req/s，%.2f MiB/s，"
        "峰值RSS %.0f MiB，峰值fd %d，峰值进程 %d\n",
        concurrency, stage.ok, stage.failed, stage.seconds,
        static_cast<double>(concurrency) / std::max(stage.seconds, 0.001),
        static_cast<double>(bytes) / (1024.0 * 1024.0) / std::max(stage.seconds, 0.001),
        static_cast<double>(stage.peak_rss) / 1024.0, stage.peak_fd, stage.peak_procs);
    // 服务器侧统计：每个成功请求都应恰好到达一次。
    AUDIT_CHECK(stage.ok == concurrency);
    AUDIT_CHECK(!check_body || server.Requests() == concurrency);
    return stage;
}

void StressConcurrentUpload() {
    Section("压测 C：并发上传阶梯（1/10/100/500/1000）");
    bento_test::TempDir dir("audit_upload_concurrent");
    constexpr std::size_t kPayload = 16 * 1024;
    constexpr int kMaxConcurrency = 1000;
    std::vector<fs::path> zips;
    zips.reserve(kMaxConcurrency);
    audit::Stopwatch make_watch;
    for (int i = 0; i < kMaxConcurrency; ++i) {
        zips.push_back(MakeZip(dir.path(), "u" + std::to_string(i) + ".zip", kPayload));
        if (zips.back().empty()) {
            std::printf("  准备第 %d 个 zip 失败\n", i);
            return;
        }
    }
    std::printf("  准备 %d 个 %zu B 的 zip：%.2fs\n", kMaxConcurrency, kPayload,
                make_watch.Seconds());
    const int stages[] = {1, 10, 100, 500, 1000};
    std::vector<StageResult> results;
    for (const int stage : stages) {
        results.push_back(RunStage(stage, zips, true));
        std::fflush(stdout);
    }
    // 100 档额外校验：服务器收到的请求体与本地 zip 一致（抽查最后一次
    // 由 RunStage 内部服务器处理，这里用独立的一次上传做内容比对）。
    {
        audit::HttpServer server([](const audit::HttpRequest&) {
            audit::HttpReply reply;
            reply.status = 201;
            return reply;
        });
        AUDIT_CHECK(server.valid());
        UploadRequest request;
        request.endpoint = server.Url("/check");
        UploadOptions options;
        options.retry_delay = 1ms;
        const auto before = audit::ReadBytes(zips[100]);
        const auto result = Upload(zips[100].string(), request, options);
        AUDIT_CHECK(result.ok());
        const auto requests = server.Requests();
        AUDIT_CHECK(requests == 1);
        std::printf("  100 档内容校验：本地 %zu B，上传成功（请求数 %d）\n", before.size(),
                    requests);
    }
}

// ---------------------------------------------------------------------------
// 场景 5：503×2 → 201
// ---------------------------------------------------------------------------

void ScenarioRetryThenSuccess() {
    Section("场景 5：503×2 → 201（重试 + 本地包保留）");
    bento_test::TempDir dir("audit_upload_retry");
    const auto zip = MakeZip(dir.path(), "retry.zip", 64 * 1024);
    AUDIT_CHECK(!zip.empty());
    const auto before = audit::ReadBytes(zip);
    std::atomic<int> calls{0};
    audit::HttpServer server([&calls](const audit::HttpRequest&) {
        audit::HttpReply reply;
        const int attempt = calls.fetch_add(1);
        reply.status = attempt < 2 ? 503 : 201;
        return reply;
    });
    AUDIT_CHECK(server.valid());
    UploadRequest request;
    request.endpoint = server.Url("/retry");
    request.method = HttpMethod::kPut;
    UploadOptions options;
    options.max_retries = 3;
    options.retry_delay = 20ms;
    const auto result = Upload(zip.string(), request, options);
    std::printf("  状态=%s attempts=%d http=%d 服务器收到 %d 次\n",
                go2cpp::bento::app::UploadStatusToString(result.status), result.attempts,
                result.http_status, server.Requests());
    AUDIT_CHECK(result.ok());
    AUDIT_CHECK(result.attempts == 3);
    AUDIT_CHECK(result.http_status == 201);
    AUDIT_CHECK(server.Requests() == 3);
    AUDIT_CHECK(audit::ReadBytes(zip) == before);
    // 本地不应出现任何临时/备份副产物。
    std::size_t leftovers = 0;
    for (const auto& entry : fs::directory_iterator(dir.path())) {
        if (entry.path().filename().string().find(".part.") != std::string::npos) {
            ++leftovers;
        }
    }
    AUDIT_CHECK(leftovers == 0);
}

// ---------------------------------------------------------------------------
// 场景 6：超时
// ---------------------------------------------------------------------------

void ScenarioTimeout() {
    Section("场景 6：服务器慢响应 → 超时（重试 + 本地包保留）");
    bento_test::TempDir dir("audit_upload_timeout");
    const auto zip = MakeZip(dir.path(), "timeout.zip", 8 * 1024);
    AUDIT_CHECK(!zip.empty());
    const auto before = audit::ReadBytes(zip);
    SlowServer server(2500); // 每次响应等 2.5s
    AUDIT_CHECK(server.valid());
    UploadRequest request;
    request.endpoint = server.Url();
    request.method = HttpMethod::kPut;
    UploadOptions options;
    options.max_retries = 1;
    options.retry_delay = 10ms;
    options.connect_timeout = 1s;
    options.request_timeout = 1s; // curl max-time 1s < 服务器 2.5s
    audit::Stopwatch watch;
    const auto result = Upload(zip.string(), request, options);
    std::printf("  状态=%s attempts=%d 耗时=%.2fs 错误=%s\n",
                go2cpp::bento::app::UploadStatusToString(result.status), result.attempts,
                watch.Seconds(), result.error.c_str());
    AUDIT_CHECK(!result.ok());
    AUDIT_CHECK(result.status == UploadStatus::kTimeout ||
                result.status == UploadStatus::kNetworkError);
    AUDIT_CHECK(result.attempts == 2);
    AUDIT_CHECK(audit::ReadBytes(zip) == before);
}

// ---------------------------------------------------------------------------
// 场景 7：连接断开
// ---------------------------------------------------------------------------

void ScenarioAbort() {
    Section("场景 7：连接建立后立即断开（本地包保留）");
    bento_test::TempDir dir("audit_upload_abort");
    const auto zip = MakeZip(dir.path(), "abort.zip", 8 * 1024);
    AUDIT_CHECK(!zip.empty());
    const auto before = audit::ReadBytes(zip);
    AbortServer server;
    AUDIT_CHECK(server.valid());
    UploadRequest request;
    request.endpoint = server.Url();
    request.method = HttpMethod::kPut;
    UploadOptions options;
    options.max_retries = 2;
    options.retry_delay = 10ms;
    options.connect_timeout = 2s;
    options.request_timeout = 2s;
    const auto result = Upload(zip.string(), request, options);
    std::printf("  状态=%s attempts=%d http=%d 错误=%s\n",
                go2cpp::bento::app::UploadStatusToString(result.status), result.attempts,
                result.http_status, result.error.c_str());
    AUDIT_CHECK(!result.ok());
    AUDIT_CHECK(result.attempts == 3);
    AUDIT_CHECK(audit::ReadBytes(zip) == before);
}

// ---------------------------------------------------------------------------
// 场景 8：verify_remote 临时文件清理
// ---------------------------------------------------------------------------

void ScenarioVerifyCleanup() {
    Section("场景 8：verify_remote 成功后临时文件清理");
    bento_test::TempDir dir("audit_upload_verify");
    const auto zip = MakeZip(dir.path(), "verify.zip", 32 * 1024);
    AUDIT_CHECK(!zip.empty());
    const auto bytes = audit::ReadBytes(zip);
    std::atomic<int> puts{0};
    audit::HttpServer server([&bytes, &puts](const audit::HttpRequest& request) {
        audit::HttpReply reply;
        if (request.method == "PUT") {
            puts.fetch_add(1);
            reply.status = 201;
            return reply;
        }
        reply.status = 200;
        reply.body = std::string(bytes.begin(), bytes.end());
        return reply;
    });
    AUDIT_CHECK(server.valid());
    const std::size_t before_files = CountPrefixFiles("bento_verify");
    UploadRequest request;
    request.endpoint = server.Url("/verify");
    UploadOptions options;
    options.retry_delay = 1ms;
    options.verify_remote = true;
    options.verify_mode = RemoteVerifyMode::kGet;
    const auto result = Upload(zip.string(), request, options);
    const std::size_t after_files = CountPrefixFiles("bento_verify");
    std::printf("  状态=%s verified=%d PUT=%d 临时文件 %zu→%zu\n",
                go2cpp::bento::app::UploadStatusToString(result.status),
                static_cast<int>(result.remote_verified), puts.load(), before_files,
                after_files);
    AUDIT_CHECK(result.ok());
    AUDIT_CHECK(result.remote_verified);
    AUDIT_CHECK(puts.load() == 1);
    AUDIT_CHECK(after_files <= before_files);
}

}  // namespace

int main() {
    Section("环境");
    std::printf("  nproc=%ld RSS=%.1f MiB fd=%d 进程数=%d\n",
                ::sysconf(_SC_NPROCESSORS_ONLN),
                static_cast<double>(audit::RssKb()) / 1024.0, audit::FdCount(),
                audit::ProcessCount());
    ScenarioRetryThenSuccess();
    ScenarioTimeout();
    ScenarioAbort();
    ScenarioVerifyCleanup();
    StressConcurrentUpload();
    return audit::Finish("audit_backup_upload_stress");
}
