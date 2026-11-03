/**
 * @file loadgen.cpp
 * @brief 抖音 C9 自研压测工具：多线程 HTTP/1.1，统计 QPS、p50/p99、非 200。
 *
 * @details 不引入任何第三方库：只用 POSIX socket。每个请求独立建连
 *          （`Connection: close`），与网关「一连接一请求」的模型一致。
 *          延迟口径为「connect + 发送 + 读完响应」的墙钟时间。
 *
 * 用法：
 * @code
 *   loadgen --url http://127.0.0.1:28080/api/feed?limit=20 \\
 *           --concurrency 32 --requests 2000 --method GET
 *   loadgen --url http://127.0.0.1:28080/api/like --method POST \\
 *           --body '{"video_id":1}' --concurrency 4 --requests 100
 * @endcode
 *
 * 输出：人类可读摘要 + 一行机器可读 `RESULT ...`。
 *
 * @author douyin demo
 * @date 2026-11-03
 * @copyright MIT License
 */

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <netdb.h>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

/// @brief 目标 URL 拆分结果。
struct Target {
    std::string host;
    std::uint16_t port{80};
    std::string path{"/"};
    std::string host_header;
};

/// @brief 命令行参数。
struct Options {
    std::string url;
    int concurrency{1};
    int requests{1};
    std::string method{"GET"};
    std::string body;
};

/// @brief 单线程统计（每线程独写，最后合并）。
struct WorkerStats {
    std::vector<long> latencies_us;
    std::uint64_t non_200{0};
    std::uint64_t errors{0};
};

/// @brief 把方法名转大写。
std::string ToUpper(std::string text) {
    for (char& c : text) {
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
    }
    return text;
}

/// @brief 解析 `http://host[:port]/path?query`（不支持 IPv6 字面量）。
bool ParseTarget(const std::string& url, Target* target) {
    constexpr const char* kPrefix = "http://";
    const std::size_t prefix_length = std::strlen(kPrefix);
    if (url.size() <= prefix_length || url.compare(0, prefix_length, kPrefix) != 0) {
        std::fprintf(stderr, "[loadgen] 仅支持 http:// URL：%s\n", url.c_str());
        return false;
    }
    const std::size_t authority_begin = prefix_length;
    const std::size_t slash = url.find('/', authority_begin);
    const std::string authority = url.substr(
        authority_begin, slash == std::string::npos ? std::string::npos
                                                    : slash - authority_begin);
    target->path = slash == std::string::npos ? "/" : url.substr(slash);
    if (authority.empty()) {
        std::fprintf(stderr, "[loadgen] URL 缺少主机名：%s\n", url.c_str());
        return false;
    }
    const std::size_t colon = authority.rfind(':');
    if (colon == std::string::npos) {
        target->host = authority;
        target->port = 80;
    } else {
        target->host = authority.substr(0, colon);
        const std::string port_text = authority.substr(colon + 1);
        char* end = nullptr;
        const long port = std::strtol(port_text.c_str(), &end, 10);
        if (end == port_text.c_str() || *end != '\0' || port <= 0 || port > 65535) {
            std::fprintf(stderr, "[loadgen] 端口非法：%s\n", port_text.c_str());
            return false;
        }
        target->port = static_cast<std::uint16_t>(port);
    }
    target->host_header = authority;
    return true;
}

/// @brief 解析正整数参数。
bool ParsePositiveInt(const char* text, int* value) {
    char* end = nullptr;
    const long parsed = std::strtol(text, &end, 10);
    if (end == text || *end != '\0' || parsed <= 0 || parsed > 10000000) {
        return false;
    }
    *value = static_cast<int>(parsed);
    return true;
}

/// @brief 打印用法并返回错误码。
int Usage(const char* program) {
    std::fprintf(stderr,
                 "用法：%s --url http://host:port/path --concurrency N "
                 "--requests M [--method GET|POST] [--body JSON]\n",
                 program);
    return 2;
}

/// @brief 解析命令行；失败返回 false 并设置退出码。
bool ParseOptions(int argc, char** argv, Options* options, int* exit_code) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const bool has_value = i + 1 < argc;
        if (arg == "--url" && has_value) {
            options->url = argv[++i];
        } else if (arg == "--concurrency" && has_value) {
            if (!ParsePositiveInt(argv[++i], &options->concurrency)) {
                std::fprintf(stderr, "[loadgen] --concurrency 非法：%s\n", argv[i]);
                *exit_code = 2;
                return false;
            }
        } else if (arg == "--requests" && has_value) {
            if (!ParsePositiveInt(argv[++i], &options->requests)) {
                std::fprintf(stderr, "[loadgen] --requests 非法：%s\n", argv[i]);
                *exit_code = 2;
                return false;
            }
        } else if (arg == "--method" && has_value) {
            options->method = ToUpper(argv[++i]);
        } else if (arg == "--body" && has_value) {
            options->body = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            *exit_code = 0;
            Usage(argv[0]);
            return false;
        } else {
            std::fprintf(stderr, "[loadgen] 未知或不完整参数：%s\n", arg.c_str());
            *exit_code = Usage(argv[0]);
            return false;
        }
    }
    if (options->url.empty()) {
        *exit_code = Usage(argv[0]);
        return false;
    }
    return true;
}

/// @brief 解析主机到 sockaddr（只取第一个地址）。
bool Resolve(const std::string& host, std::uint16_t port, sockaddr_storage* address,
             socklen_t* address_length) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    const std::string service = std::to_string(port);
    addrinfo* resolved = nullptr;
    const int rc = ::getaddrinfo(host.c_str(), service.c_str(), &hints, &resolved);
    if (rc != 0) {
        std::fprintf(stderr, "[loadgen] 解析 %s 失败：%s\n", host.c_str(),
                     gai_strerror(rc));
        return false;
    }
    bool ok = false;
    for (addrinfo* entry = resolved; entry != nullptr; entry = entry->ai_next) {
        if (entry->ai_addrlen <= sizeof(sockaddr_storage)) {
            std::memcpy(address, entry->ai_addr, entry->ai_addrlen);
            *address_length = static_cast<socklen_t>(entry->ai_addrlen);
            ok = true;
            break;
        }
    }
    ::freeaddrinfo(resolved);
    return ok;
}

/// @brief 构造 HTTP/1.1 请求（Connection: close）。
std::string BuildRequest(const Target& target, const std::string& method,
                         const std::string& body) {
    std::string request = method + " " + target.path + " HTTP/1.1\r\n";
    request += "Host: " + target.host_header + "\r\n";
    request += "Connection: close\r\n";
    request += "Accept: */*\r\n";
    if (method == "POST" || method == "PUT" || method == "PATCH") {
        request += "Content-Type: application/json\r\n";
        request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    }
    request += "\r\n";
    request += body;
    return request;
}

/**
 * @brief 向已解析上游发出一个 HTTP/1.1 请求并读完响应。
 *
 * @param address [in] 已解析的目标地址（connect 用）——非空，生命周期覆盖本次调用。
 * @param address_length [in] address 的有效字节数——来自 getaddrinfo，必须与地址族匹配。
 * @param request [in] 完整请求字节流（请求行 + 头 + body）——只读借用，本函数不持有。
 * @param latency_us [out] 本次「connect + 发送 + 接收」墙钟微秒——成功/失败都会写。
 * @param status [out] HTTP 状态码——仅返回 true 时有效；返回 false 时保持未定义。
 *
 * @return
 * - true：收到可解析的状态行且 status>0——调用方按 status 统计非 200；
 * - false：建连/发送/接收/解析失败——调用方计入 errors，不要读 status。
 *
 * ## 定位
 * 压测热路径的唯一 I/O 点：每请求独立建连（`Connection: close`），与网关
 * 「一连接一请求」模型一致。统计错了会把网络错误混进延迟分布。
 *
 * ## 步骤
 * step1. 建 socket 并设 10s 收发超时，防止单个请求卡死压测线程；
 * step2. connect；失败直接返回 false（延迟仍记录）；
 * step3. 循环 send 直到请求发完，EINTR 重试；
 * step4. 循环 recv 直到对端关闭，累计响应（8 MiB 防御上限）；
 * step5. 解析 "HTTP/1.x <code>" 状态行，写 status 与 latency。
 *
 * ## 并发/失败语义
 * - 调用约束：每个 worker 线程独立调用；入参只读，共享计数由调用方分片；
 * - 原子性：无共享写入；fd 在本函数内 close；
 * - 失败语义：不抛异常；返回 false 时 latency_us 已写、status 未定义。
 */
bool DoRequest(const sockaddr_storage& address, socklen_t address_length,
               const std::string& request, long* latency_us, int* status) {
    // step1: 建 socket + 收发超时（10s），避免阻塞压测线程。
    const auto started = std::chrono::steady_clock::now();
    const int fd = ::socket(address.ss_family, SOCK_STREAM, 0);
    if (fd < 0) {
        return false;
    }
    timeval timeout{};
    timeout.tv_sec = 10;
    timeout.tv_usec = 0;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    bool ok = false;
    // step2: connect；失败直接落到末尾（延迟仍记录）。
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&address), address_length) == 0) {
        // step3: 循环 send 到发完，EINTR 重试。
        std::size_t sent = 0;
        bool send_ok = true;
        while (sent < request.size()) {
            const ssize_t written =
                ::send(fd, request.data() + sent, request.size() - sent, 0);
            if (written < 0) {
                if (errno == EINTR) {
                    continue;
                }
                send_ok = false;
                break;
            }
            if (written == 0) {
                send_ok = false;
                break;
            }
            sent += static_cast<std::size_t>(written);
        }
        if (send_ok) {
            // step4: 循环 recv 到 EOF；超过 8 MiB 视为异常响应并提前停止。
            std::string response;
            char buffer[4096];
            for (;;) {
                const ssize_t received = ::recv(fd, buffer, sizeof(buffer), 0);
                if (received < 0) {
                    if (errno == EINTR) {
                        continue;
                    }
                    break;
                }
                if (received == 0) {
                    break;
                }
                response.append(buffer, static_cast<std::size_t>(received));
                if (response.size() > 8u * 1024u * 1024u) {
                    break;
                }
            }
            // step5: 解析 "HTTP/1.x <code>" 状态行。
            if (response.rfind("HTTP/1.", 0) == 0) {
                const std::size_t space = response.find(' ');
                if (space != std::string::npos && response.size() >= space + 4) {
                    const std::string code_text = response.substr(space + 1, 3);
                    *status = std::atoi(code_text.c_str());
                    ok = *status > 0;
                }
            }
        }
    }
    ::close(fd);
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - started);
    *latency_us = static_cast<long>(elapsed.count());
    return ok;
}

/// @brief 单线程循环。
void Worker(const sockaddr_storage& address, socklen_t address_length,
            const std::string& request, int count, WorkerStats* stats) {
    stats->latencies_us.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        long latency_us = 0;
        int status = 0;
        if (DoRequest(address, address_length, request, &latency_us, &status)) {
            stats->latencies_us.push_back(latency_us);
            if (status != 200) {
                ++stats->non_200;
            }
        } else {
            ++stats->errors;
        }
    }
}

/// @brief 百分位（最近秩法，1-based ceil）。
double Percentile(const std::vector<long>& sorted, double ratio) {
    if (sorted.empty()) {
        return 0.0;
    }
    std::size_t index =
        static_cast<std::size_t>(std::ceil(ratio * static_cast<double>(sorted.size())));
    if (index > 0) {
        --index;
    }
    if (index >= sorted.size()) {
        index = sorted.size() - 1;
    }
    return static_cast<double>(sorted[index]);
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    int exit_code = 0;
    if (!ParseOptions(argc, argv, &options, &exit_code)) {
        return exit_code;
    }

    Target target;
    if (!ParseTarget(options.url, &target)) {
        return 2;
    }
    sockaddr_storage address{};
    socklen_t address_length = 0;
    if (!Resolve(target.host, target.port, &address, &address_length)) {
        return 2;
    }

    const std::string request = BuildRequest(target, options.method, options.body);
    std::printf("[loadgen] %s %s 并发=%d 请求=%d\n", options.method.c_str(),
                options.url.c_str(), options.concurrency, options.requests);

    const int base = options.requests / options.concurrency;
    const int extra = options.requests % options.concurrency;
    std::vector<WorkerStats> stats(static_cast<std::size_t>(options.concurrency));
    std::vector<std::thread> workers;
    workers.reserve(static_cast<std::size_t>(options.concurrency));

    const auto started = std::chrono::steady_clock::now();
    for (int i = 0; i < options.concurrency; ++i) {
        const int count = base + (i < extra ? 1 : 0);
        workers.emplace_back([&address, &address_length, &request, count, &stats, i]() {
            Worker(address, address_length, request, count,
                   &stats[static_cast<std::size_t>(i)]);
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - started);
    const double wall_us = static_cast<double>(elapsed.count());

    std::vector<long> latencies;
    latencies.reserve(static_cast<std::size_t>(options.requests));
    std::uint64_t non_200 = 0;
    std::uint64_t errors = 0;
    for (const WorkerStats& item : stats) {
        latencies.insert(latencies.end(), item.latencies_us.begin(),
                         item.latencies_us.end());
        non_200 += item.non_200;
        errors += item.errors;
    }
    std::sort(latencies.begin(), latencies.end());

    const std::uint64_t success = latencies.size();
    const double qps =
        wall_us > 0.0
            ? static_cast<double>(options.requests) * 1000000.0 / wall_us
            : 0.0;
    const double wall_ms = wall_us / 1000.0;
    const double p50_ms = Percentile(latencies, 0.50) / 1000.0;
    const double p99_ms = Percentile(latencies, 0.99) / 1000.0;

    std::printf(
        "[loadgen] 成功=%llu 非200=%llu 错误=%llu 墙钟=%.1fms QPS=%.1f "
        "p50=%.3fms p99=%.3fms\n",
        static_cast<unsigned long long>(success),
        static_cast<unsigned long long>(non_200),
        static_cast<unsigned long long>(errors), wall_ms, qps, p50_ms, p99_ms);
    std::printf(
        "RESULT url=%s method=%s concurrency=%d requests=%d success=%llu non200=%llu "
        "errors=%llu wall_ms=%.1f qps=%.1f p50_ms=%.3f p99_ms=%.3f\n",
        options.url.c_str(), options.method.c_str(), options.concurrency,
        options.requests, static_cast<unsigned long long>(success),
        static_cast<unsigned long long>(non_200),
        static_cast<unsigned long long>(errors), wall_ms, qps, p50_ms, p99_ms);
    std::fflush(stdout);
    return errors == 0 ? 0 : 1;
}
