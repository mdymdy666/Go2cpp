/**
 * @file 01-timeout.cpp
 * @brief 真实异常场景一：HTTP 请求超时（服务端 3s 不回），默认行为与 3 种可选策略。
 *
 * ## 场景
 * 客户端用 `go2cpp::http::Client` 请求一个已建连但长时间不返回响应的服务端
 * （示例 mock 服务端 accept 后 sleep 1.5s，客户端只给 300ms）。这是线上最常见的
 * 「慢依赖」故障：TCP 连接是通的，但应用层迟迟不给响应。
 *
 * ## 默认行为（不写任何兜底代码时）
 * - 底层 `net::Socket::recv(..., timeout)` 在预算耗尽时返回
 *   `SocketResult{transferred=-1, error=ETIMEDOUT}`（src/network/socket.cpp:887，
 *   `recv_impl` 的 `if (left.count() == 0 && left != Duration::max()) return {-1, ETIMEDOUT};`）。
 * - HTTP 客户端把该 errno 交给 `classify_io_error`，映射成公开错误码
 *   `Result::Error::Timeout`（src/network/http.cpp:728：
 *   `if (error == ETIMEDOUT) return Result::Error::Timeout;`）。
 * - `Client::do_request` 还会在每次 IO 前重算 deadline，预算归零时直接返回
 *   `Result::Error::Timeout`（src/network/http.cpp:794/814/842），message 为
 *   `request deadline exceeded`。
 * - 结论：**默认不重试、不降级、不抛异常**，把 `Result` 原样交给调用方；
 *   调用方必须检查 `result.ok()`，否则会把超时当成空响应继续处理。
 *
 * ## 可选策略（本文件给出 3 种可运行实现）
 * 1. 重试（指数退避）：`get_with_retry()`，只重试可重试错误（Timeout/Connect/
 *    Receive），退避 100ms → 200ms → 400ms，最多 3 次；
 * 2. 降级（返回缓存）：`load_with_stale_cache()`，超时后返回上一次成功缓存，
 *    并显式标记 `used_cache`，让上层决定是否接受陈旧数据；
 * 3. 快速失败（抛业务异常）：`load_or_throw()`，把 `Result::Error::Timeout`
 *    转成 `ServiceUnavailable`，由接入层统一回 503，不占用请求线程继续等。
 *
 * ## 如何选择
 * - **只读查询 + 幂等**：选 ①，退避能吃掉一次瞬时抖动；重试必须有次数/总时限。
 * - **读多写少、可接受陈旧**：选 ②，但必须带 `stale` 标记与缓存年龄，
 *   写请求和强一致读**不要**降级。
 * - **交互式请求、失败要立刻反馈**：选 ③，把超时挡在业务逻辑之外；
 *   注意异常只能在本层捕获，不要跨 Fiber/线程抛（库的 `Result` 正是为此设计）。
 *
 * ## 编译与运行
 * ```sh
 * g++ -std=c++17 -O2 -pthread -Iinclude 01-timeout.cpp -o 01-timeout \
 *     -Lbuild-stack-unify -lgo2cpp_network -Wl,-rpath,$PWD/build-stack-unify
 * ./01-timeout
 * ```
 * 退出码 0 表示全部断言通过。
 */

#include "go2cpp/http.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace {

using namespace std::chrono_literals;
using go2cpp::http::Client;
using go2cpp::http::Result;

int g_failures = 0;

/** @brief 打印一条断言结果，失败时累加全局失败计数。 */
void check(const char* name, bool condition, const std::string& detail) {
    std::printf("[%s] %s（%s）\n", condition ? "PASS" : "FAIL", name, detail.c_str());
    if (!condition) ++g_failures;
}

/** @brief 把 HTTP 错误码转成可打印名字（输出里不出现裸数字）。 */
const char* error_name(Result::Error error) {
    switch (error) {
        case Result::Error::None: return "None";
        case Result::Error::InvalidUrl: return "InvalidUrl";
        case Result::Error::Resolve: return "Resolve";
        case Result::Error::Connect: return "Connect";
        case Result::Error::Timeout: return "Timeout";
        case Result::Error::Send: return "Send";
        case Result::Error::Receive: return "Receive";
        case Result::Error::Parse: return "Parse";
        case Result::Error::Closed: return "Closed";
        case Result::Error::Cancelled: return "Cancelled";
    }
    return "Unknown";
}

/**
 * @brief 极简 mock HTTP 服务端：监听 127.0.0.1 的临时端口，每个连接一个线程。
 *
 * @details 只服务于本示例：`handler(fd, index)` 拿到已 accept 的连接和它的
 *          连接序号（从 0 开始），负责读请求、决定是否/何时回响应。监听线程
 *          用 50ms 的 `poll` 轮询而不是阻塞 `accept`，因此析构时能可靠退出。
 */
class MockServer final {
public:
    using Handler = std::function<void(int fd, int index)>;

    explicit MockServer(Handler handler) : m_handler(std::move(handler)) {
        m_lfd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (m_lfd < 0) throw std::runtime_error("socket() failed");
        int one = 1;
        ::setsockopt(m_lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (::bind(m_lfd, reinterpret_cast<sockaddr*>(&address), sizeof address) != 0 ||
            ::listen(m_lfd, 16) != 0) {
            ::close(m_lfd);
            m_lfd = -1;
            throw std::runtime_error("bind/listen failed");
        }
        socklen_t length = sizeof address;
        ::getsockname(m_lfd, reinterpret_cast<sockaddr*>(&address), &length);
        m_port = ntohs(address.sin_port);
        m_accept = std::thread([this] {
            int index = 0;
            while (!m_stop.load(std::memory_order_acquire)) {
                pollfd entry{m_lfd, POLLIN, 0};
                if (::poll(&entry, 1, 50) <= 0) continue;
                const int fd = ::accept(m_lfd, nullptr, nullptr);
                if (fd < 0) continue;
                auto handler = m_handler;  // 拷贝进线程，避免析构后访问 this。
                std::thread([handler, fd, index] {
                    handler(fd, index);
                    ::close(fd);
                }).detach();
                ++index;
            }
        });
    }

    ~MockServer() {
        m_stop.store(true, std::memory_order_release);
        if (m_accept.joinable()) m_accept.join();
        if (m_lfd >= 0) ::close(m_lfd);
    }

    MockServer(const MockServer&) = delete;
    MockServer& operator=(const MockServer&) = delete;

    std::uint16_t port() const noexcept { return m_port; }

private:
    int m_lfd{-1};
    std::uint16_t m_port{0};
    std::atomic<bool> m_stop{false};
    std::thread m_accept;
    Handler m_handler;
};

/** @brief 读掉请求头（示例不解析内容，只保证对端已把请求发完）。 */
void read_request(int fd) {
    char buffer[4096];
    std::string data;
    for (;;) {
        const ssize_t count = ::recv(fd, buffer, sizeof buffer, 0);
        if (count <= 0) return;
        data.append(buffer, static_cast<std::size_t>(count));
        if (data.find("\r\n\r\n") != std::string::npos) return;
        if (data.size() > 1 << 20) return;  // 防御异常客户端，避免无界增长。
    }
}

/** @brief 回一个 200 响应；小响应直接 send 循环，示例不需要 partial-write 处理。 */
void send_ok(int fd, const std::string& body) {
    const std::string wire = "HTTP/1.1 200 OK\r\nContent-Length: " +
                             std::to_string(body.size()) +
                             "\r\nConnection: close\r\n\r\n" + body;
    std::size_t offset = 0;
    while (offset < wire.size()) {
        const ssize_t count =
            ::send(fd, wire.data() + offset, wire.size() - offset, MSG_NOSIGNAL);
        if (count <= 0) return;
        offset += static_cast<std::size_t>(count);
    }
}

std::string endpoint(std::uint16_t port, const std::string& path) {
    return "http://127.0.0.1:" + std::to_string(port) + path;
}

// ---------------------------------------------------------------------------
// 默认行为：不写兜底，超时就是 Result::Error::Timeout
// ---------------------------------------------------------------------------
void demo_default_timeout() {
    MockServer server([](int fd, int) {
        read_request(fd);
        std::this_thread::sleep_for(1500ms);  // 服务端 1.5s 不回，模拟慢依赖。
    });
    const auto url = endpoint(server.port(), "/api/orders/42");

    Client client(300ms);
    const auto started = std::chrono::steady_clock::now();
    const auto result = client.get(url);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();

    std::printf("\n[默认行为] GET %s\n", url.c_str());
    std::printf("  error=%s ok=%d message=%s elapsed=%lldms\n", error_name(result.error),
                static_cast<int>(result.ok()), result.message.c_str(),
                static_cast<long long>(elapsed));
    check("默认行为在 300ms deadline 返回 Result::Error::Timeout",
          result.error == Result::Error::Timeout, "期望 error=Timeout，且不自动重试");
    check("默认行为不会把超时伪装成成功", !result.ok() && result.response == nullptr,
          "response 必须为空");
}

// ---------------------------------------------------------------------------
// 策略 ①：重试 + 指数退避
// ---------------------------------------------------------------------------
Result get_with_retry(const std::string& url, int max_attempts) {
    Result last;
    int backoff_ms = 100;
    for (int attempt = 1; attempt <= max_attempts; ++attempt) {
        Client client(300ms);
        last = client.get(url);
        std::printf("  [retry] attempt=%d/%d error=%s\n", attempt, max_attempts,
                    error_name(last.error));
        if (last.ok()) return last;
        const bool retryable = last.error == Result::Error::Timeout ||
                               last.error == Result::Error::Connect ||
                               last.error == Result::Error::Receive;
        if (!retryable || attempt == max_attempts) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms));
        backoff_ms *= 2;  // 指数退避：100 → 200 → 400，避免打爆刚恢复的依赖。
    }
    return last;
}

void demo_retry() {
    MockServer server([](int fd, int index) {
        read_request(fd);
        if (index == 0) {
            std::this_thread::sleep_for(1500ms);  // 第一次故意超时。
            return;
        }
        send_ok(fd, "retry-ok");  // 第二次（重试）立即成功。
    });

    const auto url = endpoint(server.port(), "/api/orders/42");
    std::printf("\n[策略① 重试] GET %s\n", url.c_str());
    const auto result = get_with_retry(url, 3);
    check("重试在第二次尝试成功", result.ok() && result.response != nullptr &&
                                      result.response->body == "retry-ok",
          "第一次 Timeout，退避 100ms 后第二次 200");
}

// ---------------------------------------------------------------------------
// 策略 ②：降级返回缓存
// ---------------------------------------------------------------------------
struct CacheEntry {
    std::string body;
    std::chrono::steady_clock::time_point fetched_at;
    bool valid{false};
};

struct LoadResult {
    std::string body;
    bool used_cache{false};
    long long cache_age_ms{0};
    Result::Error error{Result::Error::None};
};

LoadResult load_with_stale_cache(const std::string& url, const CacheEntry& cache) {
    Client client(300ms);
    const auto result = client.get(url);
    if (result.ok() && result.response) {
        return LoadResult{result.response->body, false, 0, Result::Error::None};
    }
    if (cache.valid) {
        const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - cache.fetched_at)
                             .count();
        return LoadResult{cache.body, true, age, result.error};
    }
    return LoadResult{{}, false, 0, result.error};
}

void demo_stale_cache() {
    MockServer server([](int fd, int) {
        read_request(fd);
        std::this_thread::sleep_for(1200ms);  // 慢依赖：本次一定超时。
    });
    const auto url = endpoint(server.port(), "/api/orders/42");

    CacheEntry cache;
    cache.body = R"({"orders":[{"id":42,"status":"paid"}]})";
    cache.fetched_at = std::chrono::steady_clock::now() - 5s;  // 5 秒前的旧数据。
    cache.valid = true;

    std::printf("\n[策略② 降级] GET %s\n", url.c_str());
    const auto loaded = load_with_stale_cache(url, cache);
    std::printf("  used_cache=%d cache_age=%lldms upstream_error=%s\n",
                static_cast<int>(loaded.used_cache), loaded.cache_age_ms,
                error_name(loaded.error));
    check("超时后返回缓存并标记 used_cache",
          loaded.used_cache && loaded.body == cache.body && loaded.error == Result::Error::Timeout,
          "上层必须能看到 stale 标记与缓存年龄");
}

// ---------------------------------------------------------------------------
// 策略 ③：快速失败，抛业务异常
// ---------------------------------------------------------------------------
class ServiceUnavailable final : public std::runtime_error {
public:
    explicit ServiceUnavailable(const std::string& message) : std::runtime_error(message) {}
};

std::string load_or_throw(const std::string& url) {
    Client client(300ms);
    const auto result = client.get(url);
    if (!result.ok() || !result.response) {
        throw ServiceUnavailable("upstream GET failed: " + std::string(error_name(result.error)) +
                                 " (" + result.message + ")");
    }
    return result.response->body;
}

void demo_fail_fast() {
    MockServer server([](int fd, int) {
        read_request(fd);
        std::this_thread::sleep_for(1200ms);
    });
    const auto url = endpoint(server.port(), "/api/orders/42");

    std::printf("\n[策略③ 快速失败] GET %s\n", url.c_str());
    bool caught = false;
    std::string detail;
    try {
        (void)load_or_throw(url);
    } catch (const ServiceUnavailable& error) {
        caught = true;
        detail = error.what();
        std::printf("  caught=ServiceUnavailable -> 接入层回 503\n  what=%s\n", error.what());
    }
    check("超时被转成业务异常，不进入后续业务逻辑", caught, detail);
}

}  // namespace

int main() {
    std::printf("=== 01 HTTP 超时：默认行为 + 3 种可选策略 ===\n");
    demo_default_timeout();
    demo_retry();
    demo_stale_cache();
    demo_fail_fast();
    std::printf("\n结果：%s（失败 %d 项）\n", g_failures == 0 ? "全部通过" : "存在失败",
                g_failures);
    return g_failures == 0 ? 0 : 1;
}
