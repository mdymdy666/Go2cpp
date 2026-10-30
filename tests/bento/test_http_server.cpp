/**
 * @file test_http_server.cpp
 * @brief bento::HttpServer 骨架测试：GET/POST/SSE/404/关闭/并发/异常隔离。
 * @author go2cpp contributors
 * @date 2026-10-30
 * @copyright MIT License
 */

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "go2cpp/bento/net/http_server.hpp"
#include "test_util.hpp"

namespace {

using go2cpp::bento::HttpConcurrencyModel;
using go2cpp::bento::HttpServer;
using go2cpp::bento::HttpServerConfig;
using go2cpp::bento::HttpServerError;
using go2cpp::bento::HttpServerResult;
using go2cpp::http::Method;
using go2cpp::http::Request;
using go2cpp::http::Response;
using go2cpp::http::Status;
using go2cpp::net::DialTCP;
using go2cpp::net::Socket;
using go2cpp::net::SocketStream;
using go2cpp::sse::Event;
using go2cpp::sse::Stream;

using std::chrono::milliseconds;
using std::chrono::steady_clock;

/// 简易 HTTP/1.1 客户端：同一 socket 上复用 Parser，支持 keep-alive 多响应。
class HttpClient {
public:
    bool Connect(std::uint16_t port) {
        m_socket = DialTCP("127.0.0.1", port, milliseconds(2000));
        if (!m_socket) {
            return false;
        }
        m_socket->set_recv_timeout(milliseconds(3000));
        return true;
    }

    bool Send(std::string_view raw) {
        if (!m_socket) {
            return false;
        }
        SocketStream stream(m_socket, false);
        return stream.write_all(raw.data(), raw.size());
    }

    bool ReadResponse(Response& response) {
        for (;;) {
            if (!m_pending.empty()) {
                const auto parsed = m_parser.feed(m_pending, response);
                m_pending.clear();
                if (parsed.state == go2cpp::http::ParseState::Error) {
                    return false;
                }
                if (parsed.state == go2cpp::http::ParseState::Complete) {
                    m_pending = m_parser.take_remaining();
                    return true;
                }
                continue;
            }
            char buffer[4096];
            const auto count = m_socket->recv(buffer, sizeof(buffer));
            if (count.transferred <= 0) {
                return false;
            }
            const auto parsed = m_parser.feed(
                std::string_view(buffer, static_cast<std::size_t>(count.transferred)),
                response);
            if (parsed.state == go2cpp::http::ParseState::Error) {
                return false;
            }
            if (parsed.state == go2cpp::http::ParseState::Complete) {
                m_pending = m_parser.take_remaining();
                return true;
            }
        }
    }

    bool ReadUntil(std::string_view needle, std::string& collected, int timeout_ms) {
        if (!m_socket) {
            return false;
        }
        m_socket->set_recv_timeout(milliseconds(100));
        const auto deadline = steady_clock::now() + milliseconds(timeout_ms);
        while (steady_clock::now() < deadline) {
            if (collected.find(needle) != std::string::npos) {
                return true;
            }
            char buffer[4096];
            const auto count = m_socket->recv(buffer, sizeof(buffer));
            if (count.transferred <= 0) {
                continue;
            }
            collected.append(buffer, static_cast<std::size_t>(count.transferred));
        }
        return collected.find(needle) != std::string::npos;
    }

    bool WaitForEof(int timeout_ms) {
        if (!m_socket) {
            return false;
        }
        m_socket->set_recv_timeout(milliseconds(100));
        const auto deadline = steady_clock::now() + milliseconds(timeout_ms);
        while (steady_clock::now() < deadline) {
            char buffer[64];
            const auto count = m_socket->recv(buffer, sizeof(buffer));
            if (count.transferred == 0) {
                return true;
            }
            if (count.transferred < 0 && count.error != ETIMEDOUT &&
                count.error != EAGAIN) {
                return true;
            }
        }
        return false;
    }

    void Close() {
        if (m_socket) {
            (void)m_socket->close();
            m_socket.reset();
        }
    }

private:
    Socket::Ptr m_socket;
    go2cpp::http::Parser m_parser;
    std::string m_pending;
};

/// 构造并启动一个最小的测试服务端；失败返回 false。
bool StartServer(HttpServer& server, HttpServerConfig config = {}) {
    const auto listened = server.Listen("127.0.0.1:0", config);
    if (!listened.ok()) {
        std::fprintf(stderr, "[FAIL] Listen: %s\n", listened.message.c_str());
        return false;
    }
    const auto started = server.Start();
    if (!started.ok()) {
        std::fprintf(stderr, "[FAIL] Start: %s\n", started.message.c_str());
        return false;
    }
    return true;
}

std::string GetRequest(const std::string& path) {
    return "GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
}

void TestGetKeepAliveAndNotFoundHandler() {
    HttpServer server;
    server.OnRequest([](const Request& request) {
        Response response;
        if (request.path() == "/hello") {
            response.body = "hello";
            response.header.set("X-Server-Test", "bento");
        } else {
            response.status = Status::NotFound;
        }
        return response;
    });
    if (!StartServer(server)) {
        return;
    }
    BENTO_CHECK(server.Port() != 0);

    HttpClient client;
    BENTO_CHECK(client.Connect(server.Port()));

    // 第一条：GET /hello。
    Response first;
    BENTO_CHECK(client.Send(GetRequest("/hello")));
    BENTO_CHECK(client.ReadResponse(first));
    BENTO_CHECK(first.status == Status::OK);
    BENTO_CHECK_EQ(first.body, std::string("hello"));
    BENTO_CHECK_EQ(first.header.get("X-Server-Test"), std::string("bento"));

    // 同一连接第二条：验证 keep-alive 与路由分流。
    Response second;
    BENTO_CHECK(client.Send(GetRequest("/hello")));
    BENTO_CHECK(client.ReadResponse(second));
    BENTO_CHECK(second.status == Status::OK);
    BENTO_CHECK_EQ(second.body, std::string("hello"));

    // 同一连接第三条：业务 404。
    Response missing;
    BENTO_CHECK(client.Send(GetRequest("/missing")));
    BENTO_CHECK(client.ReadResponse(missing));
    BENTO_CHECK(missing.status == Status::NotFound);

    client.Close();
    server.Stop();
}

void TestPostBodyHeadersAndChunked() {
    HttpServer server;
    server.OnRequest([](const Request& request) {
        Response response;
        if (request.method != Method::Post) {
            response.status = Status::MethodNotAllowed;
            return response;
        }
        response.status = Status::Created;
        response.body = request.header.get("X-Message") + "|" + request.body;
        return response;
    });
    if (!StartServer(server)) {
        return;
    }

    HttpClient client;
    BENTO_CHECK(client.Connect(server.Port()));

    const std::string body = "payload-123";
    const std::string request = "POST /echo HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                                "X-Message: hello\r\nContent-Type: text/plain\r\n"
                                "Content-Length: " +
                                std::to_string(body.size()) + "\r\n\r\n" + body;
    Response response;
    BENTO_CHECK(client.Send(request));
    BENTO_CHECK(client.ReadResponse(response));
    BENTO_CHECK(response.status == Status::Created);
    BENTO_CHECK_EQ(response.body, std::string("hello|payload-123"));

    // chunked 请求 + 半包发送：验证 base Parser 的增量解析被正确串起来。
    const std::string chunked = "POST /echo HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                                "X-Message: chunk\r\nTransfer-Encoding: chunked\r\n\r\n";
    BENTO_CHECK(client.Send(chunked));
    BENTO_CHECK(client.Send("5\r\nhello\r\n"));
    BENTO_CHECK(client.Send("6\r\n world\r\n0\r\n\r\n"));
    Response chunked_response;
    BENTO_CHECK(client.ReadResponse(chunked_response));
    BENTO_CHECK(chunked_response.status == Status::Created);
    BENTO_CHECK_EQ(chunked_response.body, std::string("chunk|hello world"));

    client.Close();
    server.Stop();
}

void TestSseStreaming() {
    HttpServer server;
    server.AsSse("/events", [](const Request&, Stream& stream) {
        for (int index = 0; index < 3; ++index) {
            Event event;
            event.id = std::to_string(index);
            event.event = "tick";
            event.data = "data-" + std::to_string(index);
            if (!stream.send(event).ok()) {
                return;
            }
            std::this_thread::sleep_for(milliseconds(60));
        }
    });
    if (!StartServer(server)) {
        return;
    }

    HttpClient client;
    BENTO_CHECK(client.Connect(server.Port()));
    BENTO_CHECK(client.Send("GET /events HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                            "Accept: text/event-stream\r\n\r\n"));

    std::string collected;
    BENTO_CHECK(client.ReadUntil("data: data-0", collected, 3000));
    const auto first_seen = steady_clock::now();
    BENTO_CHECK(client.ReadUntil("data: data-2", collected, 3000));
    const auto third_seen = steady_clock::now();

    // handler 每个事件之间 sleep 60ms，读到第 3 个事件至少比第 1 个晚 80ms，
    // 证明是逐条推流而不是攒完最后一次写出。
    BENTO_CHECK(third_seen - first_seen >= milliseconds(80));
    BENTO_CHECK(collected.find("HTTP/1.1 200") != std::string::npos);
    BENTO_CHECK(collected.find("Content-Type: text/event-stream") != std::string::npos);
    BENTO_CHECK(collected.find("Transfer-Encoding: chunked") != std::string::npos);
    BENTO_CHECK(collected.find("event: tick") != std::string::npos);
    const auto first_data = collected.find("data: data-0");
    const auto second_data = collected.find("data: data-1");
    const auto third_data = collected.find("data: data-2");
    BENTO_CHECK(first_data != std::string::npos);
    BENTO_CHECK(first_data < second_data);
    BENTO_CHECK(second_data < third_data);
    BENTO_CHECK(third_data != std::string::npos);
    BENTO_CHECK(client.ReadUntil("0\r\n\r\n", collected, 3000));

    client.Close();
    server.Stop();
}

void TestNotFoundWithoutHandler() {
    HttpServer server;
    if (!StartServer(server)) {
        return;
    }
    HttpClient client;
    BENTO_CHECK(client.Connect(server.Port()));
    Response response;
    BENTO_CHECK(client.Send(GetRequest("/anything")));
    BENTO_CHECK(client.ReadResponse(response));
    BENTO_CHECK(response.status == Status::NotFound);
    BENTO_CHECK(response.body.empty());
    BENTO_CHECK_EQ(response.header.get("Content-Length"), std::string("0"));
    client.Close();
    server.Stop();
}

void TestInvalidConfigAndListenErrors() {
    HttpServer server;
    const HttpServerResult bad_address = server.Listen("this is not an address");
    BENTO_CHECK(!bad_address.ok());
    BENTO_CHECK(bad_address.error == HttpServerError::InvalidAddress);

    const HttpServerResult not_listening = server.Start();
    BENTO_CHECK(not_listening.error == HttpServerError::NotListening);

    BENTO_CHECK(server.Listen("127.0.0.1:0").ok());
    const HttpServerResult again = server.Listen("127.0.0.1:0");
    BENTO_CHECK(again.error == HttpServerError::AlreadyListening);
    const std::uint16_t occupied_port = server.Port();
    BENTO_CHECK(occupied_port != 0);

    // 端口已被 server 的 LISTEN socket 占用（没有 SO_REUSEPORT）。
    HttpServer second;
    const HttpServerResult occupied =
        second.Listen("127.0.0.1:" + std::to_string(occupied_port));
    BENTO_CHECK(!occupied.ok());
    BENTO_CHECK(occupied.error == HttpServerError::BindFailed);

    HttpServerConfig nonblocking;
    nonblocking.model = HttpConcurrencyModel::NonBlocking;
    HttpServer reserved;
    BENTO_CHECK(reserved.Listen("127.0.0.1:0", nonblocking).ok());
    const HttpServerResult unsupported = reserved.Start();
    BENTO_CHECK(unsupported.error == HttpServerError::UnsupportedModel);
    BENTO_CHECK(!reserved.IsRunning());

    server.Stop();
    reserved.Stop();
}

void TestGracefulStop() {
    HttpServer server;
    server.OnRequest([](const Request&) {
        Response response;
        response.body = "alive";
        return response;
    });
    if (!StartServer(server)) {
        return;
    }
    const std::uint16_t port = server.Port();

    HttpClient idle;
    BENTO_CHECK(idle.Connect(port));
    Response response;
    BENTO_CHECK(idle.Send(GetRequest("/alive")));
    BENTO_CHECK(idle.ReadResponse(response));
    BENTO_CHECK(response.status == Status::OK);
    // 不关闭连接：让 worker 阻塞在 keep-alive 的 recv 上，验证 Stop 能唤醒它。
    const auto started = steady_clock::now();
    server.Stop();
    const auto elapsed = steady_clock::now() - started;
    BENTO_CHECK(elapsed < milliseconds(2000));
    BENTO_CHECK(!server.IsRunning());
    BENTO_CHECK_EQ(server.Port(), std::uint16_t{0});
    BENTO_CHECK_EQ(server.ActiveConnections(), std::size_t{0});

    // 空闲连接应被服务端关闭。
    BENTO_CHECK(idle.WaitForEof(2000));
    idle.Close();

    // Stop 幂等。
    const auto stopped_again = steady_clock::now();
    server.Stop();
    BENTO_CHECK(steady_clock::now() - stopped_again < milliseconds(1000));

    // 端口应已释放，可以立即重新绑定。
    HttpServer rebound;
    const HttpServerResult result = rebound.Listen("127.0.0.1:" + std::to_string(port));
    BENTO_CHECK(result.ok());
    rebound.Stop();
}

void TestStopWakesBlockedSse() {
    HttpServer server;
    std::atomic<bool> handler_returned{false};
    std::atomic<int> sent{0};
    server.AsSse("/stream", [&](const Request&, Stream& stream) {
        for (int index = 0; index < 100000 && !server.IsStopping(); ++index) {
            Event event;
            event.event = "tick";
            event.data = "tick-" + std::to_string(index);
            if (!stream.send(event).ok()) {
                break;
            }
            sent.fetch_add(1);
            std::this_thread::sleep_for(milliseconds(5));
        }
        handler_returned.store(true);
    });
    if (!StartServer(server)) {
        return;
    }

    HttpClient client;
    BENTO_CHECK(client.Connect(server.Port()));
    BENTO_CHECK(client.Send("GET /stream HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n"));
    std::string collected;
    BENTO_CHECK(client.ReadUntil("data: tick-1", collected, 3000));
    // 客户端可能先于 handler 的 sent++ 看到字节，轮询到计数稳定即可。
    const auto sent_deadline = steady_clock::now() + milliseconds(1000);
    while (sent.load() < 2 && steady_clock::now() < sent_deadline) {
        std::this_thread::sleep_for(milliseconds(5));
    }
    BENTO_CHECK(sent.load() >= 2);

    const auto started = steady_clock::now();
    server.Stop();
    const auto elapsed = steady_clock::now() - started;
    BENTO_CHECK(elapsed < milliseconds(2000));
    BENTO_CHECK(handler_returned.load());
    BENTO_CHECK_EQ(server.ActiveConnections(), std::size_t{0});
    client.Close();
}

void TestConcurrentRequests() {
    HttpServerConfig config;
    config.worker_threads = 4;
    HttpServer server;
    std::atomic<int> handled{0};
    server.OnRequest([&handled](const Request&) {
        handled.fetch_add(1);
        Response response;
        response.body = "ok";
        return response;
    });
    if (!StartServer(server, config)) {
        return;
    }
    const std::uint16_t port = server.Port();

    constexpr int kClients = 12;
    constexpr int kRequestsPerClient = 5;
    std::atomic<int> succeeded{0};
    std::vector<std::thread> clients;
    clients.reserve(kClients);
    for (int client_index = 0; client_index < kClients; ++client_index) {
        clients.emplace_back([port, &succeeded]() {
            for (int request_index = 0; request_index < kRequestsPerClient;
                 ++request_index) {
                HttpClient client;
                if (!client.Connect(port)) {
                    continue;
                }
                if (!client.Send(GetRequest("/work"))) {
                    continue;
                }
                Response response;
                if (client.ReadResponse(response) && response.status == Status::OK &&
                    response.body == "ok") {
                    succeeded.fetch_add(1);
                }
            }
        });
    }
    for (auto& client : clients) {
        client.join();
    }
    BENTO_CHECK_EQ(succeeded.load(), kClients * kRequestsPerClient);
    BENTO_CHECK_EQ(handled.load(), kClients * kRequestsPerClient);
    server.Stop();
}

void TestHandlerExceptionIsolation() {
    HttpServer server;
    server.OnRequest([](const Request& request) {
        if (request.path() == "/boom") {
            throw std::runtime_error("handler boom");
        }
        Response response;
        response.body = "fine";
        return response;
    });
    if (!StartServer(server)) {
        return;
    }
    HttpClient client;
    BENTO_CHECK(client.Connect(server.Port()));
    Response response;
    BENTO_CHECK(client.Send(GetRequest("/boom")));
    BENTO_CHECK(client.ReadResponse(response));
    BENTO_CHECK(response.status == Status::InternalError);
    BENTO_CHECK_EQ(response.header.get("Connection"), std::string("close"));
    // 500 后连接必须关闭，而不是继续 keep-alive。
    BENTO_CHECK(client.WaitForEof(2000));

    // 服务端仍健康：新连接可以继续工作。
    HttpClient healthy;
    BENTO_CHECK(healthy.Connect(server.Port()));
    Response ok;
    BENTO_CHECK(healthy.Send(GetRequest("/fine")));
    BENTO_CHECK(healthy.ReadResponse(ok));
    BENTO_CHECK(ok.status == Status::OK);
    BENTO_CHECK_EQ(ok.body, std::string("fine"));
    healthy.Close();
    client.Close();
    server.Stop();
}

}  // namespace

int main() {
    TestGetKeepAliveAndNotFoundHandler();
    TestPostBodyHeadersAndChunked();
    TestSseStreaming();
    TestNotFoundWithoutHandler();
    TestInvalidConfigAndListenErrors();
    TestGracefulStop();
    TestStopWakesBlockedSse();
    TestConcurrentRequests();
    TestHandlerExceptionIsolation();
    return bento_test::Finish("test_http_server");
}
