#include "go2cpp/network/sse.hpp"
#include "go2cpp/network/socket.hpp"
#include "test_support.hpp"

#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

/**
 * @brief 从 SSE 对端读取一段有限数据。
 * @details 测试只等待一个有界超时；生产代码应根据业务设置更严格的
 *          客户端读取上限，不能把网络输入当作无限字符串。
 */
std::string read_once(const go2cpp::net::Socket::Ptr& socket) {
    char buffer[4096]{};
    const auto result = socket->recv(buffer, sizeof(buffer), 0, 2s);
    if (!result.ok() || result.transferred <= 0) {
        return {};
    }
    return std::string(buffer, static_cast<std::size_t>(result.transferred));
}

/**
 * @brief 读取直到出现指定标记，适配 TCP 分片和短读。
 * @param socket 读取端 Socket。
 * @param marker 期望出现的协议标记。
 * @return 已读取的原始字节；超时或关闭时返回当前已读内容。
 */
std::string read_until(const go2cpp::net::Socket::Ptr& socket,
                       std::string_view marker) {
    std::string result;
    for (int attempt = 0; attempt < 20 && result.find(marker) == std::string::npos;
         ++attempt) {
        const auto part = read_once(socket);
        if (part.empty()) {
            break;
        }
        result += part;
        if (result.size() > 64 * 1024) {
            break;
        }
    }
    return result;
}

/**
 * @brief 验证事件编码、字段校验及多行 data 的 SSE 语义。
 * @return 无；失败由统一测试断言统计。
 */
void test_event_encoding() {
    go2cpp::sse::Event event;
    event.id = "42";
    event.event = "update";
    event.retry_ms = 1500;
    event.data = "first\nsecond";
    std::string encoded;
    const auto result = event.encode(encoded);
    GO2CPP_CHECK(result.ok());
    GO2CPP_CHECK(encoded ==
                 "id: 42\nevent: update\nretry: 1500\n"
                 "data: first\ndata: second\n\n");

    go2cpp::sse::Event empty;
    GO2CPP_CHECK(empty.encode(encoded).ok());
    GO2CPP_CHECK(encoded == "data:\n\n");
    go2cpp::sse::Event unlimited;
    unlimited.data.assign(4096, 'x');
    GO2CPP_CHECK(unlimited.encode(encoded, 0).ok());

    go2cpp::sse::Event bad_id;
    bad_id.id = "bad\nid";
    GO2CPP_CHECK(bad_id.encode(encoded).error == go2cpp::sse::Error::InvalidEvent);
    go2cpp::sse::Event bad_event;
    bad_event.event = "bad\r\nevent";
    GO2CPP_CHECK(bad_event.encode(encoded).error == go2cpp::sse::Error::InvalidEvent);
    go2cpp::sse::Event bad_retry;
    bad_retry.retry_ms = UINT64_MAX;
    GO2CPP_CHECK(bad_retry.encode(encoded, 4).error == go2cpp::sse::Error::MessageTooLarge);

    go2cpp::http::Response response;
    go2cpp::sse::PrepareResponse(response);
    GO2CPP_CHECK(response.header.get("Content-Type") == "text/event-stream" &&
                 response.header.get("Cache-Control") == "no-cache" &&
                 response.header.get("Connection") == "keep-alive");
    go2cpp::http::Request request;
    request.header.set("Last-Event-ID", "42");
    GO2CPP_CHECK(go2cpp::sse::LastEventId(request) == "42");
    request.header.set("Last-Event-ID", "bad\nvalue");
    GO2CPP_CHECK(go2cpp::sse::LastEventId(request).empty());
}

/**
 * @brief 验证增量解析、BOM、注释、CRLF、重复 data、retry 和 Last-Event-ID。
 * @return 无；失败由统一测试断言统计。
 */
void test_incremental_parser() {
    go2cpp::sse::Parser parser;
    std::vector<go2cpp::sse::Event> events;
    const std::string first = "\xEF\xBB\xBF: heartbeat\r\nid: 7\r\nevent: update\r\n";
    GO2CPP_CHECK(parser.feed(first, events).ok() && events.empty());
    const auto second = parser.feed("data: first\r\ndata: second\r\nretry: 2500\r\n\r\n",
                                   events);
    GO2CPP_CHECK(second.ok() && events.size() == 1);
    GO2CPP_CHECK(events.front().id == "7" && events.front().event == "update" &&
                 events.front().data == "first\nsecond" &&
                 events.front().retry_ms.value_or(0) == 2500);
    GO2CPP_CHECK(parser.last_event_id() == "7");
    GO2CPP_CHECK(parser.retry_after().value_or(0) == 2500);
    events.clear();
    GO2CPP_CHECK(parser.feed("id: before-disconnect\n", events).ok() &&
                 parser.last_event_id() == "before-disconnect" && events.empty());

    // 未知字段必须忽略，空 id 会清除 Last-Event-ID；空行只派发一次事件。
    events.clear();
    GO2CPP_CHECK(parser.feed("unknown: ignored\nid\ndata: one\n\n", events).ok());
    GO2CPP_CHECK(events.size() == 1 && events.front().id.empty() &&
                 parser.last_event_id().empty());
    go2cpp::http::Request request;
    request.header.set("Last-Event-ID", "7");
    GO2CPP_CHECK(go2cpp::sse::LastEventId(request) == "7");
    request.header.set("Last-Event-ID", "bad\nheader");
    GO2CPP_CHECK(go2cpp::sse::LastEventId(request).empty());
    // 超长单行不能让解析器无限增长。
    go2cpp::sse::ParserLimits limits;
    limits.max_line_bytes = 4;
    go2cpp::sse::Parser limited(limits);
    events.clear();
    GO2CPP_CHECK(limited.feed("data: too-long\n", events).error == go2cpp::sse::Error::MessageTooLarge);
    // 失败后必须拒绝继续喂数据，避免业务误把异常连接的后续字节拼回旧事件；
    // reset 明确开启一个新的连接解析周期。
    GO2CPP_CHECK(limited.feed(": ok\n\n", events).error == go2cpp::sse::Error::Parse);
    limited.reset();
    GO2CPP_CHECK(limited.feed(": ok\n\n", events).ok());
}

/**
 * @brief 验证 SSE 流头、chunked 事件、心跳、tick 和关闭状态机。
 * @return 无；失败由统一测试断言统计。
 */
void test_stream_lifecycle() {
    int pair[2]{};
    GO2CPP_REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    auto server = go2cpp::net::Socket::FromFd(pair[0], AF_UNIX, SOCK_STREAM);
    auto peer = go2cpp::net::Socket::FromFd(pair[1], AF_UNIX, SOCK_STREAM);
    go2cpp::sse::StreamOptions options;
    options.heartbeat_interval = 1ms;
    options.write_timeout = 2s;
    go2cpp::sse::Stream stream(server, go2cpp::Context::Background(), options);
    GO2CPP_CHECK(stream.start().ok() && stream.state() == go2cpp::sse::StreamState::Open);
    const auto headers = read_until(peer, "\r\n\r\n");
    GO2CPP_CHECK(headers.find("Content-Type: text/event-stream") != std::string::npos);
    GO2CPP_CHECK(headers.find("Transfer-Encoding: chunked") != std::string::npos);

    go2cpp::sse::Event event;
    event.id = "1";
    event.data = "hello";
    GO2CPP_CHECK(stream.send(event).ok());
    const auto chunk = read_once(peer);
    GO2CPP_CHECK(chunk.find("data: hello\n\n") != std::string::npos);
    std::this_thread::sleep_for(3ms);
    GO2CPP_CHECK(stream.heartbeat_due());
    GO2CPP_CHECK(stream.tick().ok());
    const auto heartbeat = read_once(peer);
    GO2CPP_CHECK(heartbeat.find(": heartbeat\n\n") != std::string::npos);
    GO2CPP_CHECK(stream.close().ok() && stream.state() == go2cpp::sse::StreamState::Closed);
    GO2CPP_CHECK(!stream.send(event).ok());
    GO2CPP_CHECK(stream.close().ok());
    server->close();
    peer->close();
}

/**
 * @brief 验证取消上下文使后续写入快速失败，避免 Fiber 无限等待慢消费者。
 * @return 无；失败由统一测试断言统计。
 */
void test_context_cancellation() {
    int pair[2]{};
    GO2CPP_REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    auto server = go2cpp::net::Socket::FromFd(pair[0], AF_UNIX, SOCK_STREAM);
    auto peer = go2cpp::net::Socket::FromFd(pair[1], AF_UNIX, SOCK_STREAM);
    auto child_and_cancel = go2cpp::Context::WithCancel(go2cpp::Context::Background());
    go2cpp::sse::Stream stream(server, child_and_cancel.first);
    GO2CPP_CHECK(stream.start().ok());
    (void)read_once(peer);
    child_and_cancel.second();
    go2cpp::sse::Event event;
    event.data = "after-cancel";
    const auto result = stream.send(event);
    GO2CPP_CHECK(result.error == go2cpp::sse::Error::Cancelled ||
                 result.error == go2cpp::sse::Error::Closed);
    server->close();
    peer->close();
}

/**
 * @brief 验证多个普通线程同时生产时 chunk 不交错。
 * @details Stream 只负责字节级串行化；业务事件的顺序不作承诺，因此这里只统计
 *          完整事件数量，不比较线程之间的先后。
 */
void test_concurrent_producers() {
    int pair[2]{};
    GO2CPP_REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    auto server = go2cpp::net::Socket::FromFd(pair[0], AF_UNIX, SOCK_STREAM);
    auto peer = go2cpp::net::Socket::FromFd(pair[1], AF_UNIX, SOCK_STREAM);
    go2cpp::sse::StreamOptions options;
    options.heartbeat_interval = std::chrono::milliseconds::zero();
    options.write_timeout = 2s;
    go2cpp::sse::Stream stream(server, go2cpp::Context::Background(), options);
    GO2CPP_REQUIRE(stream.start().ok());
    (void)read_once(peer);

    constexpr int kThreads = 8;
    constexpr int kEventsPerThread = 20;
    std::atomic<int> successes{0};
    std::vector<std::thread> producers;
    producers.reserve(kThreads);
    for (int thread_index = 0; thread_index < kThreads; ++thread_index) {
        producers.emplace_back([&, thread_index] {
            for (int event_index = 0; event_index < kEventsPerThread; ++event_index) {
                go2cpp::sse::Event event;
                event.id = std::to_string(thread_index) + "-" + std::to_string(event_index);
                event.data = "producer" + std::to_string(thread_index);
                if (stream.send(event).ok()) {
                    successes.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    std::string payload;
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (successes.load(std::memory_order_acquire) != kThreads * kEventsPerThread &&
           std::chrono::steady_clock::now() < deadline) {
        char buffer[4096]{};
        const auto received = peer->recv(buffer, sizeof(buffer), 0, 100ms);
        if (received.ok() && received.transferred > 0) {
            payload.append(buffer, static_cast<std::size_t>(received.transferred));
        }
    }
    for (auto& producer : producers) {
        GO2CPP_JOIN_WITH_WATCHDOG(producer, 3s);
    }
    (void)stream.close();
    server->close();
    for (;;) {
        char buffer[4096]{};
        const auto received = peer->recv(buffer, sizeof(buffer), 0, 1s);
        if (!received.ok() || received.transferred <= 0) {
            break;
        }
        payload.append(buffer, static_cast<std::size_t>(received.transferred));
    }
    GO2CPP_CHECK(successes.load(std::memory_order_acquire) == kThreads * kEventsPerThread);
    std::size_t event_count = 0;
    for (std::size_t position = 0;;) {
        position = payload.find("data: producer", position);
        if (position == std::string::npos) {
            break;
        }
        ++event_count;
        position += 13;
    }
    GO2CPP_CHECK(event_count == static_cast<std::size_t>(kThreads * kEventsPerThread));
    peer->close();
}

/**
 * @brief 验证 Context 取消回调依赖的 Socket::cancel_write/read 可以跨线程打断等待。
 * @details cancel_* 不再只依赖调用方线程的 IOManager；普通线程也会在有限等待片段内
 *          观察取消代际，避免 SSE 慢消费者或 Fiber 迁移时永久挂起。
 */
void test_cross_thread_socket_cancel() {
    int pair[2]{};
    GO2CPP_REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    auto reader = go2cpp::net::Socket::FromFd(pair[0], AF_UNIX, SOCK_STREAM);
    auto peer = go2cpp::net::Socket::FromFd(pair[1], AF_UNIX, SOCK_STREAM);
    std::atomic<bool> entered{false};
    std::atomic<int> error{0};
    std::thread waiter([&] {
        entered.store(true, std::memory_order_release);
        char value{};
        const auto result = reader->recv(&value, 1);
        error.store(result.error, std::memory_order_release);
    });
    GO2CPP_REQUIRE_EVENTUALLY(entered.load(std::memory_order_acquire), 1s);
    std::this_thread::sleep_for(20ms);
    GO2CPP_CHECK(reader->cancel_read());
    GO2CPP_JOIN_WITH_WATCHDOG(waiter, 2s);
    GO2CPP_CHECK(error.load(std::memory_order_acquire) == ECANCELED);
    reader->close();
    peer->close();
}

}  // namespace

/**
 * @brief 运行 SSE 协议、流生命周期和取消边界测试。
 * @return 无；测试失败计数由 test_support.h 统一维护。
 */
void run_sse_tests() {
    go2cpp_tests::announce("network sse");
    test_event_encoding();
    test_incremental_parser();
    test_stream_lifecycle();
    test_context_cancellation();
    test_concurrent_producers();
    test_cross_thread_socket_cancel();
}
