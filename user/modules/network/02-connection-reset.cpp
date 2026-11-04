/**
 * @file 02-connection-reset.cpp
 * @brief 真实异常场景二：连接被对端复位（服务端 kill -9 / RST），默认行为与 3 种可选策略。
 *
 * ## 场景
 * 客户端向 `POST /api/pay` 提交一笔支付。服务端进程被 `kill -9`（或中间设备
 * 发 RST、容器被强制回收），内核向客户端回 `ECONNRESET`。客户端可能已经
 * 把请求发完，但永远收不到响应——**请求是否已被处理是未知的**，这正是
 * 「连接复位」比「超时」更难处理的地方。
 *
 * ## 默认行为（不写任何兜底代码时）
 * - `Socket::recv` 返回 `SocketResult{-1, ECONNRESET}`（src/network/socket.cpp:877-879，
 *   非 EAGAIN 的 errno 原样带回）；
 * - HTTP 客户端用 `classify_io_error` 把 `ECONNRESET`/`EPIPE` 统一映射成
 *   `Result::Error::Closed`（src/network/http.cpp:733：
 *   `if (error == ECONNRESET || error == EPIPE) return Result::Error::Closed;`）；
 * - `do_request` 不会在同一个已死 fd 上重试；调用方拿到 `Closed` 后**必须**
 *   重建连接（src/network/http.cpp:668 附近注释：Closed 表示连接级失败）。
 * - 结论：默认既不重连、也不上报、更不会保证 POST 只执行一次。调用方若把
 *   `Closed` 当成「没发出去」而直接重发，可能造成重复扣款。
 *
 * ## 可选策略（本文件给出 3 种可运行实现）
 * 1. 重连（带退避）：`post_with_reconnect()`，只对 `Closed`/`Timeout` 重试，
 *    每次重试重新 `do_request`（新建连接），退避 100ms → 200ms；
 * 2. 上报：`post_and_report()`，把 `Closed` 归类成结构化事件交监控/告警，
 *    本请求快速返回 503，不盲目重试；
 * 3. 幂等重发：`post_with_idempotency_key()`，POST 带 `Idempotency-Key`，
 *    复位后用**同一个 key** 重发；服务端按 key 命中已处理结果时直接回放，
 *    不再扣款。**写请求重试的前提就是幂等键**。
 *
 * ## 如何选择
 * - 查询类（GET）：① 重连最直接；
 * - 写类（POST/PUT）：**必须** ③ 幂等键，或 ② 上报后交人工/对账处理；
 *   没有幂等键时不要自动重发（这是示例里最危险的错误用法）；
 * - 大面积 RST（服务端整体重启）：② 优先，重连风暴会拖垮刚恢复的服务。
 *
 * ## 编译与运行
 * ```sh
 * g++ -std=c++17 -O2 -pthread -Iinclude 02-connection-reset.cpp -o 02-connection-reset \
 *     -Lbuild-stack-unify -lgo2cpp_network -Wl,-rpath,$PWD/build-stack-unify
 * ./02-connection-reset
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
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

namespace {

using namespace std::chrono_literals;
using go2cpp::http::Client;
using go2cpp::http::Method;
using go2cpp::http::Request;
using go2cpp::http::Result;

int g_failures = 0;

void check(const char* name, bool condition, const std::string& detail) {
    std::printf("[%s] %s（%s）\n", condition ? "PASS" : "FAIL", name, detail.c_str());
    if (!condition) ++g_failures;
}

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

/** @brief 与 01 相同的极简 mock 服务端；handler 拿连接序号决定行为。 */
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
                auto handler = m_handler;
                // 连接由 handler 自己关闭：RST 场景必须先 setsockopt(SO_LINGER)
                // 再 close，外层不能再 close 同一个 fd（fd 可能已被复用）。
                std::thread([handler, fd, index] { handler(fd, index); }).detach();
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

/** @brief 读完整请求（头 + Content-Length 指定的 body）；失败返回空串。 */
std::string read_full_request(int fd) {
    std::string data;
    char buffer[4096];
    std::size_t header_end = std::string::npos;
    for (;;) {
        const ssize_t count = ::recv(fd, buffer, sizeof buffer, 0);
        if (count <= 0) return {};
        data.append(buffer, static_cast<std::size_t>(count));
        header_end = data.find("\r\n\r\n");
        if (header_end != std::string::npos) break;
        if (data.size() > 1 << 20) return {};
    }
    std::size_t body_length = 0;
    const auto content_length = data.find("Content-Length:");
    if (content_length != std::string::npos && content_length < header_end) {
        try {
            body_length = static_cast<std::size_t>(
                std::stoul(data.substr(content_length + 15, 32)));
        } catch (...) {
            body_length = 0;
        }
    }
    const std::size_t body_start = header_end + 4;
    while (data.size() - body_start < body_length) {
        const ssize_t count = ::recv(fd, buffer, sizeof buffer, 0);
        if (count <= 0) return {};
        data.append(buffer, static_cast<std::size_t>(count));
    }
    return data;
}

/** @brief 从请求头里取 Idempotency-Key（缺失返回空串）。 */
std::string idempotency_key(const std::string& request) {
    const std::string name = "Idempotency-Key:";
    const auto start = request.find(name);
    if (start == std::string::npos) return {};
    auto value_start = start + name.size();
    while (value_start < request.size() && request[value_start] == ' ') ++value_start;
    const auto end = request.find("\r\n", value_start);
    if (end == std::string::npos) return {};
    return request.substr(value_start, end - value_start);
}

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

/** @brief 用 SO_LINGER{1,0} 主动发 RST，等价于服务端被 kill -9。 */
void reset_connection(int fd) {
    linger option{1, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_LINGER, &option, sizeof option);
    ::close(fd);
}

/** @brief 扣款状态：按幂等键保存已处理结果，并统计真正扣款次数。 */
struct PaymentState {
    std::mutex mutex;
    std::unordered_map<std::string, std::string> processed;
    std::atomic<int> charges{0};
};

Result do_post(std::uint16_t port, const std::string& key,
               std::chrono::milliseconds timeout) {
    Request request;
    request.method = Method::Post;
    request.target = "/api/pay";
    request.header.set("Content-Type", "application/json");
    if (!key.empty()) request.header.set("Idempotency-Key", key);
    request.body = R"({"order":42,"amount":100})";
    Client client(timeout);
    return client.do_request(request, "127.0.0.1", port);
}

// ---------------------------------------------------------------------------
// 默认行为：不写兜底，POST 遇到 RST 得到 Result::Error::Closed
// ---------------------------------------------------------------------------
void demo_default_closed() {
    MockServer server([](int fd, int index) {
        (void)read_full_request(fd);
        (void)index;
        reset_connection(fd);  // 服务端读完请求后直接 RST（kill -9 的等价物）。
    });

    std::printf("\n[默认行为] POST http://127.0.0.1:%u/api/pay\n",
                static_cast<unsigned>(server.port()));
    const auto result = do_post(server.port(), {}, 500ms);
    std::printf("  error=%s ok=%d message=%s\n", error_name(result.error),
                static_cast<int>(result.ok()), result.message.c_str());
    check("RST 被映射为 Result::Error::Closed", result.error == Result::Error::Closed,
          "ECONNRESET → Closed（src/network/http.cpp:733）");
    check("默认不会在已死连接上自动重试", !result.ok() && result.response == nullptr,
          "调用方必须自己决定重连还是上报");
}

// ---------------------------------------------------------------------------
// 策略 ①：重连 + 退避
// ---------------------------------------------------------------------------
Result post_with_reconnect(std::uint16_t port, const std::string& key, int max_attempts,
                           int& attempts) {
    Result last;
    int backoff_ms = 100;
    for (attempts = 1; attempts <= max_attempts; ++attempts) {
        last = do_post(port, key, 500ms);
        std::printf("  [reconnect] attempt=%d/%d error=%s\n", attempts, max_attempts,
                    error_name(last.error));
        if (last.ok()) return last;
        const bool retryable =
            last.error == Result::Error::Closed || last.error == Result::Error::Timeout;
        if (!retryable || attempts == max_attempts) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms));
        backoff_ms *= 2;
    }
    return last;
}

void demo_reconnect() {
    MockServer server([](int fd, int index) {
        (void)read_full_request(fd);
        if (index == 0) {
            reset_connection(fd);  // 第一次连接被复位。
            return;
        }
        send_ok(fd, "charged-100");  // 重连后成功。
        ::close(fd);
    });

    std::printf("\n[策略① 重连] POST http://127.0.0.1:%u/api/pay\n",
                static_cast<unsigned>(server.port()));
    int attempts = 0;
    const auto result = post_with_reconnect(server.port(), "order-42", 3, attempts);
    check("重连后第二次请求成功", result.ok() && result.response != nullptr &&
                                      result.response->body == "charged-100" && attempts == 2,
          "第一次 Closed，退避 100ms 后重连成功，attempts=2");
}

// ---------------------------------------------------------------------------
// 策略 ②：上报（结构化事件 + 快速 503）
// ---------------------------------------------------------------------------
struct Incident {
    std::string code;
    std::string route;
    std::string message;
};

Incident report_incident(const Result& result) {
    Incident incident{"network.connection_reset", "POST /api/pay", result.message};
    std::printf("  [alert] event=%s route=%s message=%s action=page-oncall\n",
                incident.code.c_str(), incident.route.c_str(), incident.message.c_str());
    return incident;
}

bool post_and_report(std::uint16_t port) {
    const auto result = do_post(port, "order-42", 500ms);
    if (!result.ok() && result.error == Result::Error::Closed) {
        const auto incident = report_incident(result);
        std::printf("  [response] 503 Service Unavailable, incident=%s\n",
                    incident.code.c_str());
        return true;  // 已上报，交给告警/对账，不在本请求里盲目重试。
    }
    return false;
}

void demo_report() {
    MockServer server([](int fd, int) {
        (void)read_full_request(fd);
        reset_connection(fd);
    });
    std::printf("\n[策略② 上报] POST http://127.0.0.1:%u/api/pay\n",
                static_cast<unsigned>(server.port()));
    const bool reported = post_and_report(server.port());
    check("Closed 被转成告警并快速返回 503", reported, "不重试、不吞错误");
}

// ---------------------------------------------------------------------------
// 策略 ③：幂等重发（POST 必须带幂等键）
// ---------------------------------------------------------------------------
std::pair<Result, int> post_with_idempotency_key(std::uint16_t port,
                                                 const std::string& key) {
    auto first = do_post(port, key, 500ms);
    std::printf("  [idempotent] first error=%s\n", error_name(first.error));
    if (first.ok() || first.error != Result::Error::Closed) return {first, 1};
    // 关键：重发必须复用同一个 Idempotency-Key，服务端才能去重。
    std::this_thread::sleep_for(100ms);
    auto second = do_post(port, key, 500ms);
    std::printf("  [idempotent] replay error=%s body=%s\n", error_name(second.error),
                second.response ? second.response->body.c_str() : "(null)");
    return {second, 2};
}

void demo_idempotent_replay() {
    auto state = std::make_shared<PaymentState>();
    MockServer server([state](int fd, int index) {
        const auto raw = read_full_request(fd);
        const auto key = idempotency_key(raw);
        if (index == 0) {
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->processed[key] = "charged-100";
            }
            state->charges.fetch_add(1);  // 第一次请求已扣款，但响应被 RST 吞掉。
            reset_connection(fd);
            return;
        }
        std::string replay;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            const auto found = state->processed.find(key);
            if (found != state->processed.end()) {
                // 幂等命中：回放第一次的结果，绝不再扣一次。
                replay = "already-charged:" + found->second;
            } else {
                state->processed[key] = "charged-100";
            }
        }
        if (!replay.empty()) {
            send_ok(fd, replay);
            ::close(fd);
            return;
        }
        state->charges.fetch_add(1);
        send_ok(fd, "charged-100");
        ::close(fd);
    });

    std::printf("\n[策略③ 幂等重发] POST http://127.0.0.1:%u/api/pay\n",
                static_cast<unsigned>(server.port()));
    const auto [result, attempts] = post_with_idempotency_key(server.port(), "idem-order-42-1");
    const bool replayed = result.ok() && result.response != nullptr &&
                          result.response->body == "already-charged:charged-100";
    std::printf("  charges=%d\n", state->charges.load());
    check("同 key 重发命中服务端去重，不重复扣款",
          replayed && attempts == 2 && state->charges.load() == 1,
          "第一次已扣款后 RST；第二次 already-charged，charges 仍为 1");
}

}  // namespace

int main() {
    std::printf("=== 02 连接复位（RST）：默认行为 + 3 种可选策略 ===\n");
    demo_default_closed();
    demo_reconnect();
    demo_report();
    demo_idempotent_replay();
    std::printf("\n结果：%s（失败 %d 项）\n", g_failures == 0 ? "全部通过" : "存在失败",
                g_failures);
    return g_failures == 0 ? 0 : 1;
}
