/**
 * @file test_upload_timeout.cpp
 * @brief 上传 error 回归测试：网关读取大 body 的时限与定界行为。
 *
 * @details 直接编译 src/app/gateway.cpp 起真实网关（127.0.0.1:0），用裸 socket
 *          客户端构造定长 / Content-Length 异常 / chunked / 慢速上传，验证：
 *          - 1/10/32 MiB 上传成功，33/64 MiB 被 400 拒绝；
 *          - 32 MiB 上限内的慢速上行（4 KiB/50 ms，约 80 KiB/s）能在读取
 *            总时限内完成，不被静默断连；
 *          - 多连接同时慢速上行互不影响。
 *
 *          中间件镜像 gateway.cpp 的 LimitUploadSize；读取时限镜像
 *          gateway.cpp 的 `config.request_timeout = 120 s`（组件默认 10 s 会让
 *          32 MiB 上传要求 3.2 MiB/s 上行，慢速上传在 body 中途被断连且无响应）。
 */

#include "go2cpp/app/gateway.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace gw = go2cpp::app::gateway;
namespace http = go2cpp::http;

namespace {

constexpr std::size_t kMib = 1024 * 1024;
constexpr std::size_t kMaxUploadBytes = 32 * kMib;
/// 与 app/douyin/src/gateway.cpp 的 config.request_timeout 保持一致。
constexpr std::chrono::milliseconds kReadTimeout{120000};
constexpr auto kReadBudget = std::chrono::milliseconds(150000);
int g_failures = 0;

struct Outcome {
    int status{0};
    std::string body;
    std::string transport{"none"};
    long long elapsed_ms{0};
    std::size_t sent{0};
};

struct UploadSpec {
    std::size_t body_bytes{0};
    bool chunked{false};
    std::size_t declared{0};  ///< 0 表示与 body_bytes 相同
    std::size_t chunk_bytes{64 * 1024};
    int gap_ms{0};
    bool half_close{false};  ///< 发完 body 后 shutdown(SHUT_WR)
};

class UploadMatcher final : public gw::Matcher {
public:
    bool Match(const gw::Request& request, gw::PathParams*) const override {
        return request.path_view() == "/api/video/upload";
    }
};

struct Harness {
    gw::Config config;
    std::unique_ptr<gw::Gateway> gateway;
    std::thread thread;

    bool Start() {
        config.listen_host = "127.0.0.1";
        config.port = 0;
        auto created = gw::Gateway::Create(config);
        if (!created.ok()) {
            std::printf("  FAIL create gateway: %s\n", created.error().c_str());
            ++g_failures;
            return false;
        }
        gateway = std::move(created.value());
        thread = std::thread([this]() { gateway->Run(); });
        for (int i = 0; i < 400; ++i) {
            if (gateway->running()) {
                // 镜像 gateway.cpp:306-334：32 MiB 上传闸门。
                gateway->Use([](gw::Request& request, gw::Response& response,
                                const gw::Next& next) -> gw::Result<void> {
                    if (request.method != http::Method::Post ||
                        request.path_view() != "/api/video/upload") {
                        return next();
                    }
                    if (request.body.size() > kMaxUploadBytes) {
                        response.status = http::Status::BadRequest;
                        response.header.set("Content-Type",
                                            "application/json; charset=utf-8");
                        response.body =
                            "{\"ok\":false,\"error\":\"视频不能超过 32 MiB\","
                            "\"code\":400}";
                        return gw::Result<void>::Ok();
                    }
                    return next();
                });
                const auto route = gateway->AddRoute(
                    std::make_shared<UploadMatcher>(),
                    std::make_shared<gw::Handler>(
                        [](gw::Request& request, gw::Response& response) {
                            response.status = http::Status::OK;
                            response.header.set("Content-Type", "application/json");
                            response.body =
                                "{\"ok\":true,\"bytes\":" +
                                std::to_string(request.body.size()) + "}";
                            return gw::Result<void>::Ok();
                        }));
                if (!route.ok()) {
                    std::printf("  FAIL AddRoute: %s\n", route.error().c_str());
                    ++g_failures;
                    return false;
                }
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        std::printf("  FAIL gateway did not start\n");
        ++g_failures;
        return false;
    }

    void Stop() {
        if (gateway) {
            gateway->Stop();
        }
        if (thread.joinable()) {
            thread.join();
        }
        gateway.reset();
    }

    ~Harness() { Stop(); }
};

int Connect(std::uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

bool SendAll(int fd, const char* data, std::size_t size, std::size_t* total) {
    std::size_t written_total = 0;
    while (written_total < size) {
        const ssize_t written =
            ::send(fd, data + written_total, size - written_total, MSG_NOSIGNAL);
        if (written <= 0) {
            return false;
        }
        written_total += static_cast<std::size_t>(written);
    }
    if (total != nullptr) {
        *total += size;
    }
    return true;
}

std::string Hex(std::size_t value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%zx", value);
    return buffer;
}

Outcome RunUpload(std::uint16_t port, const UploadSpec& spec) {
    Outcome outcome;
    const int fd = Connect(port);
    if (fd < 0) {
        outcome.transport = "connect-failed";
        return outcome;
    }
    const std::size_t declared = spec.declared == 0 ? spec.body_bytes : spec.declared;
    std::string head = "POST /api/video/upload HTTP/1.1\r\nHost: upload.test\r\n";
    if (spec.chunked) {
        head += "Transfer-Encoding: chunked\r\n";
    } else {
        head += "Content-Length: " + std::to_string(declared) + "\r\n";
    }
    head += "Connection: close\r\n\r\n";

    const auto begin = std::chrono::steady_clock::now();
    bool send_ok = SendAll(fd, head.data(), head.size(), &outcome.sent);

    std::string block(spec.chunk_bytes, 'x');
    std::size_t remaining = spec.body_bytes;
    while (send_ok && remaining > 0) {
        const std::size_t take = remaining < spec.chunk_bytes ? remaining : spec.chunk_bytes;
        std::string wire;
        if (spec.chunked) {
            wire = Hex(take) + "\r\n" + block.substr(0, take) + "\r\n";
        } else {
            wire = block.substr(0, take);
        }
        send_ok = SendAll(fd, wire.data(), wire.size(), &outcome.sent);
        remaining -= take;
        if (send_ok && spec.gap_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(spec.gap_ms));
        }
    }
    if (send_ok && spec.chunked) {
        const char tail[] = "0\r\n\r\n";
        send_ok = SendAll(fd, tail, sizeof(tail) - 1, &outcome.sent);
    }
    if (!send_ok) {
        outcome.transport = "send-failed";
    }
    if (spec.half_close) {
        ::shutdown(fd, SHUT_WR);
    }

    std::string raw;
    const auto deadline = std::chrono::steady_clock::now() + kReadBudget;
    while (std::chrono::steady_clock::now() < deadline) {
        pollfd entry{};
        entry.fd = fd;
        entry.events = POLLIN;
        const int ready = ::poll(&entry, 1, 200);
        if (ready < 0) {
            outcome.transport = std::string("poll:") + std::strerror(errno);
            break;
        }
        if (ready == 0) {
            continue;
        }
        char buffer[8192];
        const ssize_t got = ::recv(fd, buffer, sizeof(buffer), 0);
        if (got > 0) {
            raw.append(buffer, static_cast<std::size_t>(got));
            continue;
        }
        if (got == 0) {
            if (outcome.transport == "none") {
                outcome.transport = "eof";
            }
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        outcome.transport = errno == ECONNRESET ? "rst" : std::string("recv:") +
                                                          std::strerror(errno);
        break;
    }
    if (outcome.transport == "none") {
        outcome.transport = "read-timeout";
    }
    if (raw.rfind("HTTP/1.1 ", 0) == 0 && raw.size() >= 12) {
        outcome.status = std::atoi(raw.substr(9, 3).c_str());
        const std::size_t body_begin = raw.find("\r\n\r\n");
        if (body_begin != std::string::npos) {
            outcome.body = raw.substr(body_begin + 4);
        }
    }
    outcome.elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - begin)
                             .count();
    ::close(fd);
    return outcome;
}

void Report(const char* name, const Outcome& outcome, int expect_status) {
    const bool ok = expect_status == 0 ? outcome.status == 200
                                       : outcome.status == expect_status;
    if (!ok) {
        ++g_failures;
    }
    std::printf("[%s] %-34s status=%-3d elapsed=%7lldms sent=%-9zu transport=%-12s %s\n",
                ok ? "PASS" : "FAIL", name, outcome.status, outcome.elapsed_ms,
                outcome.sent, outcome.transport.c_str(),
                outcome.body.empty() ? "" : outcome.body.substr(0, 64).c_str());
}

void Info(const char* name, const Outcome& outcome) {
    std::printf("[INFO] %-34s status=%-3d elapsed=%7lldms sent=%-9zu transport=%-12s\n",
                name, outcome.status, outcome.elapsed_ms, outcome.sent,
                outcome.transport.c_str());
}

void RunAlone(const char* name, std::uint16_t port, const UploadSpec& spec,
              int expect_status) {
    Report(name, RunUpload(port, spec), expect_status);
}

/// @brief 同时发起 n 条慢速上传，全部应成功（验证多连接慢速上行互不影响）。
void RunConcurrent(std::uint16_t port, std::size_t count, std::size_t bytes,
                   std::size_t chunk, int gap_ms) {
    std::vector<Outcome> outcomes(count);
    std::vector<std::thread> threads;
    threads.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        threads.emplace_back([&, i]() {
            UploadSpec spec;
            spec.body_bytes = bytes;
            spec.chunk_bytes = chunk;
            spec.gap_ms = gap_ms;
            outcomes[i] = RunUpload(port, spec);
        });
    }
    for (auto& worker : threads) {
        worker.join();
    }
    int failed = 0;
    long long slowest = 0;
    for (const auto& outcome : outcomes) {
        if (outcome.status != 200) {
            ++failed;
        }
        if (outcome.elapsed_ms > slowest) {
            slowest = outcome.elapsed_ms;
        }
    }
    if (failed != 0) {
        ++g_failures;
    }
    std::printf("[%s] %zu 并发慢速上传 (%zu B @ %zu B/%dms) 失败=%d 最慢=%lldms\n",
                failed == 0 ? "PASS" : "FAIL", count, bytes, chunk, gap_ms, failed,
                slowest);
}

}  // namespace

int main() {
    Harness harness;
    harness.config.worker_threads = 4;
    harness.config.request_timeout = kReadTimeout;
    if (!harness.Start()) {
        return 1;
    }
    const std::uint16_t port = harness.gateway->port();
    std::printf("[gateway] 127.0.0.1:%u request_timeout=%lldms\n",
                static_cast<unsigned>(port),
                static_cast<long long>(harness.config.request_timeout.count()));

    struct BigCase {
        const char* name;
        std::size_t bytes;
        int expect;
    };
    const BigCase big_cases[] = {
        {"1 MiB 正常", 1 * kMib, 0},
        {"10 MiB 正常", 10 * kMib, 0},
        {"32 MiB 边界（等于上限）", 32 * kMib, 0},
        {"33 MiB 超限", 33 * kMib, 400},
        {"64 MiB 超限", 64 * kMib, 400},
    };
    for (const auto& item : big_cases) {
        UploadSpec spec;
        spec.body_bytes = item.bytes;
        RunAlone(item.name, port, spec, item.expect);
    }

    {
        UploadSpec spec;
        spec.body_bytes = 1 * kMib;
        spec.declared = 2 * kMib;
        spec.half_close = true;
        // 已知框架限制：body 短读时 ServeConnection 直接关闭连接、不回 400。
        Info("Content-Length 比实际大 1 MiB（已知）", RunUpload(port, spec));
    }
    {
        UploadSpec spec;
        spec.body_bytes = 1 * kMib;
        spec.declared = 512 * 1024;
        RunAlone("Content-Length 比实际小 512 KiB", port, spec, 200);
    }
    {
        UploadSpec spec;
        spec.body_bytes = 1 * kMib;
        spec.chunked = true;
        RunAlone("chunked 1 MiB", port, spec, 200);
    }
    {
        UploadSpec spec;
        spec.body_bytes = 1 * kMib;
        spec.chunk_bytes = 4 * 1024;
        spec.gap_ms = 50;
        // 12.8 s > 组件默认 10 s：修复前这里必然断连无响应。
        RunAlone("慢速 1 MiB（4 KiB/50ms，12.8s）", port, spec, 200);
    }
    RunConcurrent(port, 4, 1 * kMib, 4 * 1024, 50);
    harness.Stop();

    std::printf("\n[summary] failures=%d\n", g_failures);
    return g_failures == 0 ? 0 : 2;
}
