/**
 * @file 03-partial-write.cpp
 * @brief 真实异常场景三：大 body 只发出去一半（send 部分成功），默认语义与 3 种可选策略。
 *
 * ## 场景
 * 客户端上传 4 MiB 的 body，socket 的发送/接收缓冲只有 64 KiB。一次
 * `Socket::send` 只能提交其中一小部分字节就返回——**这不是错误，而是 TCP
 * 流式语义的正常现象**。策略 ③ 另外模拟服务端读取到一半就停读 3 秒。
 *
 * ## 默认行为（危险点）
 * - 库**没有** `SendAll`；唯一的发送接口是单次 `Socket::send`。它把
 *   `sendmsg` 的返回值原样带回：只要 `count >= 0` 就 `return {count, 0}`
 *   （src/network/socket.cpp:857-858）。
 * - 因此「4 MiB 只发出 32 KiB」时 `SocketResult::ok() == true`、`error == 0`。
 *   如果调用方只检查 `ok()` 就认为发送完成，会静默丢数据。
 * - 正确语义必须由调用方补齐：循环发送直到 `sent == total`；HTTP 客户端
 *   自己就是这么做的（src/network/http.cpp:809-823 的 `while (sent < wire.size())`）。
 *
 * ## 可选策略（本文件给出 3 种可运行实现）
 * 1. `SendAll` 循环：`send_all()` 按块循环，直到发完或单次等待超时；
 * 2. 分片重发：`send_chunked()` 把 body 切成固定大小的片，每片成功后记录
 *    `checkpoint`；失败时从 checkpoint 续传（示例展示成功路径与 checkpoint 语义）；
 * 3. 放弃并清理：超时后记录已发字节、`shutdown` + `close` 连接、释放 body，
 *    把失败暴露给上层，而不是假装成功。
 *
 * ## 如何选择
 * - **所有流式发送都必须用 ① 或 ②**，单次 `send` 只能用于 ≤ 内核缓冲的小包，
 *   且仍要检查 `transferred == length`；
 * - 大文件/断点续传：②，checkpoint 要持久化到可恢复的位置（示例放内存）；
 * - 对端长时间不读、超过业务总时限：③，及时释放 fd/内存，避免半开连接堆积。
 *
 * ## 编译与运行
 * ```sh
 * g++ -std=c++17 -O2 -pthread -Iinclude 03-partial-write.cpp -o 03-partial-write \
 *     -Lbuild-stack-unify -lgo2cpp_network -Wl,-rpath,$PWD/build-stack-unify
 * ./03-partial-write
 * ```
 * 退出码 0 表示全部断言通过。
 */

#include "go2cpp/network/socket.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using go2cpp::net::Address;
using go2cpp::net::Socket;

int g_failures = 0;

void check(const char* name, bool condition, const std::string& detail) {
    std::printf("[%s] %s（%s）\n", condition ? "PASS" : "FAIL", name, detail.c_str());
    if (!condition) ++g_failures;
}

/** @brief 与 01/02 相同的极简 mock 服务端；handler 负责读完并处理连接。 */
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

/** @brief 正常服务端：持续读，直到客户端关闭。 */
void fast_reader(int fd) {
    int receive_buffer = 65536;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &receive_buffer, sizeof receive_buffer);
    char buffer[65536];
    for (;;) {
        const ssize_t count = ::recv(fd, buffer, sizeof buffer, 0);
        if (count <= 0) return;
    }
}

/** @brief 故障服务端：只读 1 KiB 就停读 3s，用于演示发送超时后的放弃清理。 */
void stalled_reader(int fd) {
    int receive_buffer = 65536;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &receive_buffer, sizeof receive_buffer);
    char buffer[65536];
    (void)::recv(fd, buffer, 1024, 0);
    std::this_thread::sleep_for(3s);
    for (;;) {
        const ssize_t count = ::recv(fd, buffer, sizeof buffer, 0);
        if (count <= 0) return;
    }
}

Socket::Ptr connect_to(std::uint16_t port) {
    auto socket = Socket::CreateTCP(AF_INET);
    if (!socket) return nullptr;
    auto address = Address::LookupAny("127.0.0.1:" + std::to_string(port));
    if (!address || !socket->connect(*address, 1000ms)) return nullptr;
    int send_buffer = 65536;  // 远小于 4 MiB body，保证单次 send 必然部分写。
    ::setsockopt(socket->fd(), SOL_SOCKET, SO_SNDBUF, &send_buffer, sizeof send_buffer);
    return socket;
}

struct SendOutcome {
    bool complete{false};
    std::size_t sent{0};   ///< 已成功交给内核的字节数（失败时用于续传）。
    int error{0};          ///< 失败 errno；完成为 0。
    std::size_t calls{0};  ///< 调用了多少次 send（观测部分写次数）。
};

/**
 * @brief SendAll 循环：反复调用 Socket::send，直到发完或单次等待超时。
 * @param socket [in,out] 已连接的 socket；本函数不关闭它。
 * @param data [in] 待发缓冲区首地址，长度 size，借用期间有效。
 * @param size [in] 总字节数。
 * @param timeout [in] **单次** send 的等待预算（不是整个 SendAll 的总时限）。
 * @param chunk [in] 单次最多提交的字节数；0 表示不限制。
 * @return 完成标志、已发字节数、失败 errno 与调用次数；调用方**必须**用
 *         `complete` 判断，不能只看某一次 send 的 `ok()`。
 */
SendOutcome send_all(Socket& socket, const char* data, std::size_t size,
                     std::chrono::milliseconds timeout, std::size_t chunk) {
    SendOutcome outcome;
    while (outcome.sent < size) {
        const std::size_t want =
            chunk == 0 ? size - outcome.sent : std::min(chunk, size - outcome.sent);
        const auto result = socket.send(data + outcome.sent, want, 0, timeout);
        ++outcome.calls;
        if (!result.ok()) {
            outcome.error = result.error;
            return outcome;
        }
        if (result.transferred == 0) {
            outcome.error = EIO;
            return outcome;
        }
        outcome.sent += static_cast<std::size_t>(result.transferred);
    }
    outcome.complete = true;
    return outcome;
}

struct ChunkedOutcome {
    bool complete{false};
    std::size_t checkpoint{0};  ///< 已确认发送到的偏移，失败时可从这里续传。
    std::size_t chunks{0};
    int error{0};
};

/**
 * @brief 分片重发：每片走一次 SendAll，整片成功后才推进 checkpoint。
 * @details 失败时 checkpoint 停在最后一片的边界；片内已发字节由 `sent` 单独
 *          给出，续传时可从 `checkpoint + sent` 继续（本示例只展示边界语义）。
 */
ChunkedOutcome send_chunked(Socket& socket, const char* data, std::size_t size,
                            std::size_t slice, std::chrono::milliseconds timeout,
                            std::size_t chunk) {
    ChunkedOutcome outcome;
    while (outcome.checkpoint < size) {
        const std::size_t length = std::min(slice, size - outcome.checkpoint);
        const auto sent = send_all(socket, data + outcome.checkpoint, length, timeout, chunk);
        if (!sent.complete) {
            outcome.error = sent.error;
            return outcome;  // checkpoint 不前进，交给上层决定续传/放弃。
        }
        outcome.checkpoint += length;
        ++outcome.chunks;
    }
    outcome.complete = true;
    return outcome;
}

// ---------------------------------------------------------------------------
// 默认行为：单次 send 部分成功，但 ok() == true（危险点）
// ---------------------------------------------------------------------------
void demo_default_partial() {
    MockServer server([](int fd, int) { fast_reader(fd); });
    auto socket = connect_to(server.port());
    if (!socket) {
        check("默认行为：连接 mock 服务端", false, "connect failed");
        return;
    }
    std::vector<char> body(4 * 1024 * 1024, 'x');

    std::printf("\n[默认行为] 单次 send %zu 字节\n", body.size());
    const auto result = socket->send(body.data(), body.size(), 0, 300ms);
    std::printf("  transferred=%zd total=%zu ok=%d error=%d\n", result.transferred,
                body.size(), static_cast<int>(result.ok()), result.error);
    check("单次 send 只发一部分，但 ok() 仍为 true",
          result.transferred > 0 &&
              static_cast<std::size_t>(result.transferred) < body.size() && result.ok(),
          "调用方若只看 ok() 会静默丢数据（src/network/socket.cpp:857）");
    socket->close();
}

// ---------------------------------------------------------------------------
// 策略 ①：SendAll 循环
// ---------------------------------------------------------------------------
void demo_send_all() {
    MockServer server([](int fd, int) { fast_reader(fd); });
    auto socket = connect_to(server.port());
    if (!socket) {
        check("策略① SendAll：连接 mock 服务端", false, "connect failed");
        return;
    }
    std::vector<char> body(4 * 1024 * 1024, 'x');

    std::printf("\n[策略① SendAll] 循环发送 %zu 字节\n", body.size());
    const auto outcome = send_all(*socket, body.data(), body.size(), 500ms, 64 * 1024);
    std::printf("  complete=%d sent=%zu calls=%zu error=%d\n",
                static_cast<int>(outcome.complete), outcome.sent, outcome.calls,
                outcome.error);
    check("SendAll 循环把 4 MiB 全部发出", outcome.complete && outcome.sent == body.size(),
          "以 returned sent 为准，不以单次 ok() 为准");
    socket->close();
}

// ---------------------------------------------------------------------------
// 策略 ②：分片重发（checkpoint）
// ---------------------------------------------------------------------------
void demo_chunked() {
    MockServer server([](int fd, int) { fast_reader(fd); });
    auto socket = connect_to(server.port());
    if (!socket) {
        check("策略② 分片重发：连接 mock 服务端", false, "connect failed");
        return;
    }
    std::vector<char> body(4 * 1024 * 1024, 'x');
    constexpr std::size_t kSlice = 256 * 1024;

    std::printf("\n[策略② 分片重发] slice=%zu 字节\n", kSlice);
    const auto outcome = send_chunked(*socket, body.data(), body.size(), kSlice, 500ms,
                                      64 * 1024);
    std::printf("  complete=%d checkpoint=%zu chunks=%zu error=%d\n",
                static_cast<int>(outcome.complete), outcome.checkpoint, outcome.chunks,
                outcome.error);
    check("分片发送完整，checkpoint 可作断点续传位置",
          outcome.complete && outcome.checkpoint == body.size() &&
              outcome.chunks == body.size() / kSlice,
          "每片成功后才推进 checkpoint，失败时从这里续传");
    socket->close();
}

// ---------------------------------------------------------------------------
// 策略 ③：放弃并清理
// ---------------------------------------------------------------------------
void demo_abort_and_cleanup() {
    MockServer server([](int fd, int) { stalled_reader(fd); });
    auto socket = connect_to(server.port());
    if (!socket) {
        check("策略③ 放弃并清理：连接 mock 服务端", false, "connect failed");
        return;
    }
    std::vector<char> body(4 * 1024 * 1024, 'x');

    std::printf("\n[策略③ 放弃并清理] 200ms 预算，服务端停读 3s\n");
    const auto outcome = send_all(*socket, body.data(), body.size(), 200ms, 64 * 1024);
    std::printf("  complete=%d sent=%zu error=%d\n", static_cast<int>(outcome.complete),
                outcome.sent, outcome.error);
    const bool aborted = !outcome.complete && outcome.sent > 0 && outcome.error == ETIMEDOUT;
    // 清理顺序：先半关闭（让对端尽快看到 FIN），再 close 释放 fd，最后释放 body。
    socket->shutdown(SHUT_RDWR);
    socket->close();
    body.clear();
    body.shrink_to_fit();
    std::printf("  cleanup: fd closed, body released, last_checkpoint=%zu\n", outcome.sent);
    check("超时后放弃发送并完成资源清理", aborted && !socket->valid(),
          "记录已发字节后关闭连接，不把半成功当成功");
}

}  // namespace

int main() {
    std::printf("=== 03 部分写：默认语义 + 3 种可选策略 ===\n");
    demo_default_partial();
    demo_send_all();
    demo_chunked();
    demo_abort_and_cleanup();
    std::printf("\n结果：%s（失败 %d 项）\n", g_failures == 0 ? "全部通过" : "存在失败",
                g_failures);
    return g_failures == 0 ? 0 : 1;
}
