/**
 * @file test_http_e2e.cpp
 * @brief HTTP 端到端测试：Router（精确/参数/通配）、StaticFiles（sendfile/Range/
 *        304/HEAD）、keep-alive、pipelining、chunked、SSE 与并发连接。
 * @details 全部通过真实 TCP 字节流驱动（不是纯编解码单测）。测试文件不注册进
 *          ctest，用独立脚本按 `g++ ... tests/network/test_http_e2e.cpp` 编译，
 *          以便在 `/tmp/build-http` 独立构建目录下运行。
 * @author go2cpp contributors
 * @date 2026-11-02
 * @copyright MIT License
 */

#include "go2cpp/bento/net/http_server.hpp"
#include "go2cpp/network/http.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <unistd.h>

namespace {

using go2cpp::bento::HttpServer;
using go2cpp::bento::HttpServerConfig;
using go2cpp::http::Method;
using go2cpp::http::Request;
using go2cpp::http::Response;
using go2cpp::http::Status;
using go2cpp::net::DialTCP;
using go2cpp::net::Socket;
using go2cpp::net::SocketStream;

int g_checks = 0;
int g_failures = 0;

void Check(bool condition, const char* expression, int line) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::fprintf(stderr, "FAIL line %d: %s\n", line, expression);
    }
}

#define CHECK(expression) Check((expression), #expression, __LINE__)

/// 写一个固定内容的测试文件；父目录不存在时创建。
void WriteFile(const std::string& path, const std::string& content) {
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
}

/// 生成测试文档根：1 KiB / 10 KiB / 1 MiB 二进制 + HTML + 子目录索引。
std::string PrepareWww() {
    const std::string root = "/tmp/http_e2e_www";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    WriteFile(root + "/hello.html", "<html>hello</html>");
    WriteFile(root + "/index.html", "index page");
    WriteFile(root + "/sub/index.html", "sub index");
    WriteFile(root + "/data.bin", std::string(1024, 'A'));
    WriteFile(root + "/tenk.bin", std::string(10 * 1024, 'C'));
    WriteFile(root + "/big.bin", std::string(1024 * 1024, 'B'));
    return root;
}

std::string GetRequest(std::string_view path,
                       std::string_view extra_headers = std::string_view{}) {
    std::string request = "GET ";
    request.append(path);
    request.append(" HTTP/1.1\r\nHost: test\r\n");
    request.append(extra_headers);
    request.append("\r\n");
    return request;
}

std::string PostRequest(std::string_view path, std::string_view body) {
    std::string request = "POST ";
    request.append(path);
    request.append(" HTTP/1.1\r\nHost: test\r\nContent-Length: ");
    request.append(std::to_string(body.size()));
    request.append("\r\n\r\n");
    request.append(body);
    return request;
}

/// 简易 HTTP/1.1 客户端：keep-alive 复用同一 socket 与 Parser。
class HttpClient {
public:
    bool Connect(std::uint16_t port) {
        m_socket = DialTCP("127.0.0.1", port, std::chrono::milliseconds(2000));
        if (!m_socket) return false;
        m_socket->set_recv_timeout(std::chrono::milliseconds(10000));
        return true;
    }

    bool Send(std::string_view raw) {
        if (!m_socket) return false;
        SocketStream stream(m_socket, false);
        return stream.write_all(raw.data(), raw.size());
    }

    bool Read(Response& response, bool head_response = false) {
        response = Response{};
        m_parser.set_response_to_head(head_response);
        for (;;) {
            if (!m_pending.empty()) {
                const auto parsed = m_parser.feed(m_pending, response);
                m_pending.clear();
                if (parsed.state == go2cpp::http::ParseState::Error) return false;
                if (parsed.state == go2cpp::http::ParseState::Complete) {
                    m_pending = m_parser.take_remaining();
                    return true;
                }
                continue;
            }
            std::array<char, 65536> buffer{};
            const auto count = m_socket->recv(buffer.data(), buffer.size());
            if (count.transferred <= 0) return false;
            const auto parsed = m_parser.feed(
                std::string_view(buffer.data(), static_cast<std::size_t>(count.transferred)),
                response);
            if (parsed.state == go2cpp::http::ParseState::Error) return false;
            if (parsed.state == go2cpp::http::ParseState::Complete) {
                m_pending = m_parser.take_remaining();
                return true;
            }
        }
    }

    void Close() {
        if (m_socket) {
            (void)m_socket->close();
            m_socket.reset();
        }
    }

    ~HttpClient() { Close(); }

private:
    Socket::Ptr m_socket;
    go2cpp::http::Parser m_parser;
    std::string m_pending;
};

/// 断言响应 body 全为 `value`（用来校验 1 MiB sendfile 的完整内容）。
bool BodyAll(const std::string& body, char value) {
    return !body.empty() &&
           std::all_of(body.begin(), body.end(),
                       [value](char item) { return item == value; });
}

// ---------------------------------------------------------------------------
// 1. Router：精确哈希 + 参数 + 通配
// ---------------------------------------------------------------------------

void TestRouter() {
    go2cpp::http::Router router;
    router.add(Method::Get, "/hello", [](const Request&) {
        Response response;
        response.body = "hello";
        return response;
    });
    for (int index = 0; index < 64; ++index) {
        const std::string pattern = "/r" + std::to_string(index);
        const std::string body = "r" + std::to_string(index);
        router.add(Method::Get, pattern, [body](const Request&) {
            Response response;
            response.body = body;
            return response;
        });
    }
    CHECK(router.size() == 65);
    CHECK(router.match(Method::Get, "/r63", nullptr) != nullptr);
    CHECK((*router.match(Method::Get, "/r63", nullptr))({}).body == "r63");

    // 参数段：捕获非空段，多参数按名字写入。
    router.add(Method::Get, "/api/items/:id", [](const Request&) {
        Response response;
        response.body = "item";
        return response;
    });
    router.add(Method::Get, "/users/:uid/posts/:pid", [](const Request&) {
        Response response;
        response.body = "post";
        return response;
    });
    go2cpp::http::RouteParams params;
    const auto* item = router.match(Method::Get, "/api/items/42", &params);
    CHECK(item != nullptr && params["id"] == "42");
    params.clear();
    const auto* post = router.match(Method::Get, "/users/7/posts/9", &params);
    CHECK(post != nullptr && params["uid"] == "7" && params["pid"] == "9");

    // 尾部通配：捕获剩余全部路径（含 '/'）。
    router.add(Method::Get, "/static/*path", [](const Request&) {
        Response response;
        response.body = "static";
        return response;
    });
    params.clear();
    const auto* wildcard = router.match(Method::Get, "/static/css/app.css", &params);
    CHECK(wildcard != nullptr && params["path"] == "css/app.css");
    params.clear();
    CHECK(router.match(Method::Get, "/static/", &params) != nullptr &&
          params["path"].empty());

    // 静态优先于参数：先注册 `:id` 再注册 `me` 也必须命中静态。
    router.add(Method::Get, "/users/:id", [](const Request&) {
        Response response;
        response.body = "user";
        return response;
    });
    router.add(Method::Get, "/users/me", [](const Request&) {
        Response response;
        response.body = "me";
        return response;
    });
    CHECK((*router.match(Method::Get, "/users/me", nullptr))({}).body == "me");
    params.clear();
    CHECK((*router.match(Method::Get, "/users/123", &params))({}).body == "user" &&
          params["id"] == "123");

    // 方法隔离 + 未命中 + 重复注册保留第一次。
    CHECK(router.match(Method::Post, "/hello", nullptr) == nullptr);
    CHECK(router.match(Method::Get, "/missing", nullptr) == nullptr);
    router.add(Method::Get, "/dup", [](const Request&) {
        Response response;
        response.body = "first";
        return response;
    });
    router.add(Method::Get, "/dup", [](const Request&) {
        Response response;
        response.body = "second";
        return response;
    });
    CHECK((*router.match(Method::Get, "/dup", nullptr))({}).body == "first");

    // 尾斜杠归一：注册 `/trailing/` 后 `/trailing` 也能命中。
    router.add(Method::Get, "/trailing/", [](const Request&) {
        Response response;
        response.body = "trailing";
        return response;
    });
    CHECK(router.match(Method::Get, "/trailing", nullptr) != nullptr);
    go2cpp::http::Router catch_all;
    catch_all.add(Method::Get, "*", [](const Request&) {
        Response response;
        response.body = "any";
        return response;
    });
    CHECK((*catch_all.match(Method::Get, "/a/b/c", nullptr))({}).body == "any");
}

// ---------------------------------------------------------------------------
// 2. StaticFiles：文件体/Range/304/目录穿越/索引
// ---------------------------------------------------------------------------

void TestStaticFiles(const std::string& www) {
    go2cpp::http::StaticFilesOptions options;
    options.root = www;
    options.max_bytes = 4 * 1024 * 1024;
    go2cpp::http::StaticFiles files(options);

    Request request;
    request.method = Method::Get;
    request.target = "/data.bin";
    Response response;
    CHECK(files.serve(request, &response));
    CHECK(response.status == Status::OK);
    CHECK(response.has_file());
    CHECK(response.file->length == 1024);
    CHECK(response.file->path == www + "/data.bin");
    CHECK(response.header.get("Content-Type") == "application/octet-stream");
    CHECK(response.header.get("Content-Length") == "1024");
    CHECK(response.header.get("Accept-Ranges") == "bytes");
    const std::string etag = response.header.get("ETag");
    CHECK(!etag.empty() && etag.front() == '"' && etag.back() == '"');

    // Range：首段、后缀、开区间、越界 416。
    request.header.set("Range", "bytes=0-9");
    CHECK(files.serve(request, &response));
    CHECK(response.status == Status::PartialContent);
    CHECK(response.file->offset == 0 && response.file->length == 10);
    CHECK(response.header.get("Content-Range") == "bytes 0-9/1024");

    request.header.set("Range", "bytes=-5");
    CHECK(files.serve(request, &response));
    CHECK(response.file->offset == 1019 && response.file->length == 5);
    CHECK(response.header.get("Content-Range") == "bytes 1019-1023/1024");

    request.header.set("Range", "bytes=1000-");
    CHECK(files.serve(request, &response));
    CHECK(response.file->offset == 1000 && response.file->length == 24);

    request.header.set("Range", "bytes=2000-");
    CHECK(files.serve(request, &response));
    CHECK(response.status == Status::RangeNotSatisfiable);
    CHECK(response.header.get("Content-Range") == "bytes */1024");
    CHECK(!response.has_file());

    // 多区间按 RFC 允许的方式忽略，返回 200 全量。
    request.header.set("Range", "bytes=0-1,3-4");
    CHECK(files.serve(request, &response));
    CHECK(response.status == Status::OK && response.file->length == 1024);

    // 条件请求：If-None-Match / If-Modified-Since 都回 304。
    request.header.erase("Range");
    request.header.set("If-None-Match", etag);
    CHECK(files.serve(request, &response));
    CHECK(response.status == Status::NotModified);
    CHECK(!response.has_file() && response.header.get("ETag") == etag);

    request.header.erase("If-None-Match");
    request.header.set("If-Modified-Since", response.header.get("Last-Modified"));
    CHECK(files.serve(request, &response));
    CHECK(response.status == Status::NotModified);

    // 条件不满足时忽略 Range（If-Range 不匹配）。
    request.header.erase("If-Modified-Since");
    request.header.set("Range", "bytes=0-9");
    request.header.set("If-Range", "\"deadbeef\"");
    CHECK(files.serve(request, &response));
    CHECK(response.status == Status::OK && response.file->length == 1024);

    // 路径安全：`..`、百分号编码的 `..`、非法编码、绝对路径。
    request.header.erase("Range");
    request.header.erase("If-Range");
    request.target = "/../etc/passwd";
    CHECK(files.serve(request, &response));
    CHECK(response.status == Status::Forbidden);
    request.target = "/%2e%2e/etc/passwd";
    CHECK(files.serve(request, &response));
    CHECK(response.status == Status::Forbidden);
    request.target = "/%zz";
    CHECK(files.serve(request, &response));
    CHECK(response.status == Status::BadRequest);
    request.target = "/a%00b";
    CHECK(files.serve(request, &response));
    CHECK(response.status == Status::BadRequest);

    // 不存在返回 false；索引文件与 MIME。
    request.target = "/no-such-file";
    CHECK(!files.serve(request, &response));
    request.target = "/";
    CHECK(files.serve(request, &response));
    CHECK(response.status == Status::OK && response.file->path == www + "/index.html");
    CHECK(response.header.get("Content-Type") == "text/html; charset=utf-8");
    request.target = "/sub/";
    CHECK(files.serve(request, &response));
    CHECK(response.file->path == www + "/sub/index.html");

    // 非 GET/HEAD 不处理（由上层路由决定是否 405）。
    request.method = Method::Post;
    request.target = "/hello.html";
    CHECK(!files.serve(request, &response));

    // 超过 max_bytes → 413。
    go2cpp::http::StaticFilesOptions small = options;
    small.max_bytes = 512;
    go2cpp::http::StaticFiles small_files(small);
    request.method = Method::Get;
    request.target = "/data.bin";
    CHECK(small_files.serve(request, &response));
    CHECK(response.status == Status::PayloadTooLarge);

    // 挂载前缀：只匹配 `/static` 与 `/static/...`，不匹配 `/staticx`。
    go2cpp::http::StaticFilesOptions mounted_options = options;
    mounted_options.prefix = "/static";
    go2cpp::http::StaticFiles mounted(mounted_options);
    request.header.clear();
    request.method = Method::Get;
    request.target = "/static/data.bin";
    CHECK(mounted.serve(request, &response));
    CHECK(response.status == Status::OK && response.file->path == www + "/data.bin");
    request.target = "/data.bin";
    CHECK(!mounted.serve(request, &response));
    request.target = "/staticx/data.bin";
    CHECK(!mounted.serve(request, &response));
    request.target = "/static";
    CHECK(mounted.serve(request, &response));
    CHECK(response.status == Status::OK && response.file->path == www + "/index.html");
}

// ---------------------------------------------------------------------------
// 3. base http::Server：真实 socket 上的路由 + 发送路径
// ---------------------------------------------------------------------------

/// 启动 base::Server：accept 循环 + 连接内 keep-alive（max_requests 足够大）。
class BaseServerRunner {
public:
    explicit BaseServerRunner(const std::string& www) {
        m_listener = Socket::CreateTCP(AF_INET);
        CHECK(static_cast<bool>(m_listener));
        CHECK(m_listener->bind(*go2cpp::net::IPv4Address::Create("127.0.0.1", 0)));
        CHECK(m_listener->listen(128));
        const auto address =
            std::dynamic_pointer_cast<go2cpp::net::IPv4Address>(m_listener->local_address());
        CHECK(static_cast<bool>(address));
        m_port = address ? address->port() : 0;

        go2cpp::http::StaticFilesOptions options;
        options.root = www;
        options.prefix = "/static";
        m_files = std::make_shared<go2cpp::http::StaticFiles>(options);
        m_server = std::make_shared<go2cpp::http::Server>(m_listener);
        m_server->route(Method::Get, "/hello", [](const Request&) {
            Response response;
            response.header.set("Content-Type", "text/plain");
            response.body = "hello world\n";
            return response;
        });
        m_server->route(Method::Get, "/api/items/:id", [](const Request& request) {
            Response response;
            response.body = "item:" + request.path_value("id");
            return response;
        });
        m_server->route(Method::Get, "/static/*path", m_files->handler());
        m_server->set_fallback([](const Request&) {
            Response response;
            response.status = Status::NotFound;
            response.body = "not found\n";
            return response;
        });
        m_thread = std::thread([this]() { AcceptLoop(); });
    }

    ~BaseServerRunner() {
        m_stop.store(true);
        if (m_listener) (void)m_listener->close();
        if (m_thread.joinable()) m_thread.join();
        m_server.reset();
    }

    std::uint16_t port() const noexcept { return m_port; }

private:
    void AcceptLoop() {
        while (!m_stop.load(std::memory_order_acquire)) {
            auto connection = m_listener->accept(std::chrono::milliseconds(100));
            if (!connection) continue;
            m_server->serve_connection(connection, 1000000);
            (void)connection->close();
        }
    }

    Socket::Ptr m_listener;
    std::shared_ptr<go2cpp::http::Server> m_server;
    std::shared_ptr<go2cpp::http::StaticFiles> m_files;
    std::thread m_thread;
    std::atomic<bool> m_stop{false};
    std::uint16_t m_port{0};
};

void TestBaseServer(const std::string& www) {
    BaseServerRunner server(www);
    CHECK(server.port() != 0);

    HttpClient client;
    CHECK(client.Connect(server.port()));

    // 普通路由 + 参数路由 + 404。
    Response response;
    CHECK(client.Send(GetRequest("/hello")));
    CHECK(client.Read(response));
    CHECK(response.status == Status::OK && response.body == "hello world\n");

    CHECK(client.Send(GetRequest("/api/items/42")));
    CHECK(client.Read(response));
    CHECK(response.body == "item:42");

    CHECK(client.Send(GetRequest("/missing")));
    CHECK(client.Read(response));
    CHECK(response.status == Status::NotFound);

    // keep-alive 连续 20 个请求。
    for (int index = 0; index < 20; ++index) {
        CHECK(client.Send(GetRequest("/hello")));
        CHECK(client.Read(response));
        CHECK(response.status == Status::OK && response.body == "hello world\n");
    }

    // 静态文件：1 KiB 全量 + Range + HEAD + 1 MiB sendfile。
    CHECK(client.Send(GetRequest("/static/data.bin")));
    CHECK(client.Read(response));
    CHECK(response.status == Status::OK && response.body.size() == 1024 &&
          BodyAll(response.body, 'A'));

    CHECK(client.Send(GetRequest("/static/data.bin", "Range: bytes=100-199\r\n")));
    CHECK(client.Read(response));
    CHECK(response.status == Status::PartialContent && response.body.size() == 100 &&
          BodyAll(response.body, 'A'));
    CHECK(response.header.get("Content-Range") == "bytes 100-199/1024");

    CHECK(client.Send("HEAD /static/data.bin HTTP/1.1\r\nHost: test\r\n\r\n"));
    CHECK(client.Read(response, true));
    CHECK(response.status == Status::OK && response.body.empty());
    CHECK(response.header.get("Content-Length") == "1024");

    CHECK(client.Send(GetRequest("/static/big.bin")));
    CHECK(client.Read(response));
    CHECK(response.status == Status::OK && response.body.size() == 1024 * 1024 &&
          BodyAll(response.body, 'B'));

    // pipelining：一次写 8 条（混合路由/静态文件），按序读 8 条。
    std::string batch;
    for (int index = 0; index < 8; ++index) {
        if (index % 3 == 0) batch += GetRequest("/hello");
        else if (index % 3 == 1) batch += GetRequest("/static/tenk.bin");
        else batch += GetRequest("/api/items/" + std::to_string(index));
    }
    CHECK(client.Send(batch));
    for (int index = 0; index < 8; ++index) {
        CHECK(client.Read(response));
        if (index % 3 == 0) {
            CHECK(response.body == "hello world\n");
        } else if (index % 3 == 1) {
            CHECK(response.body.size() == 10 * 1024 && BodyAll(response.body, 'C'));
        } else {
            CHECK(response.body == "item:" + std::to_string(index));
        }
    }
    client.Close();
}

// ---------------------------------------------------------------------------
// 4. bento::HttpServer：Route 注册 + sendfile + 并发 + pipelining + SSE
// ---------------------------------------------------------------------------

struct BentoServer {
    std::unique_ptr<HttpServer> server;
    std::uint16_t port{0};
};

std::unique_ptr<BentoServer> StartBento(const std::string& www, bool use_bufio) {
    auto holder = std::make_unique<BentoServer>();
    holder->server = std::make_unique<HttpServer>();
    HttpServerConfig config;
    config.worker_threads = 4;
    config.max_requests_per_connection = 1000000;
    config.use_bufio = use_bufio;
    go2cpp::http::StaticFilesOptions options;
    options.root = www;
    options.prefix = "/static";
    go2cpp::http::StaticFiles files(options);

    holder->server->Route(Method::Get, "/hello", [](const Request&) {
        Response response;
        response.header.set("Content-Type", "text/plain");
        response.body = "hello world\n";
        return response;
    });
    holder->server->Route(Method::Get, "/api/items/:id", [](const Request& request) {
        Response response;
        response.body = "item:" + request.path_value("id");
        return response;
    });
    holder->server->Route(Method::Post, "/echo", [](const Request& request) {
        Response response;
        response.body = request.body;
        return response;
    });
    holder->server->Route(Method::Get, "/static/*path", files.handler());
    holder->server->AsSse("/events", [](const Request&, go2cpp::sse::Stream& stream) {
        for (int index = 0; index < 3; ++index) {
            go2cpp::sse::Event event;
            event.event = "tick";
            event.data = std::to_string(index);
            if (!stream.send(event).ok()) break;
        }
    });
    const auto listened = holder->server->Listen("127.0.0.1", 0, config);
    CHECK(listened.ok());
    const auto started = holder->server->Start();
    CHECK(started.ok());
    holder->port = holder->server->Port();
    CHECK(holder->port != 0);
    return holder;
}

void TestBentoServer(const std::string& www) {
    for (const bool use_bufio : {false, true}) {
        auto holder = StartBento(www, use_bufio);
        HttpClient client;
        CHECK(client.Connect(holder->port));

        Response response;
        CHECK(client.Send(GetRequest("/hello")));
        CHECK(client.Read(response));
        CHECK(response.status == Status::OK && response.body == "hello world\n");

        CHECK(client.Send(GetRequest("/api/items/7")));
        CHECK(client.Read(response));
        CHECK(response.body == "item:7");

        // POST body（Content-Length 定界）原样回显。
        CHECK(client.Send(PostRequest("/echo", "payload-123")));
        CHECK(client.Read(response));
        CHECK(response.status == Status::OK && response.body == "payload-123");

        // 静态文件 1 MiB（sendfile）+ Range。
        CHECK(client.Send(GetRequest("/static/big.bin")));
        CHECK(client.Read(response));
        CHECK(response.status == Status::OK && response.body.size() == 1024 * 1024 &&
              BodyAll(response.body, 'B'));

        CHECK(client.Send(GetRequest("/static/tenk.bin", "Range: bytes=10-19\r\n")));
        CHECK(client.Read(response));
        CHECK(response.status == Status::PartialContent && response.body.size() == 10 &&
              BodyAll(response.body, 'C'));

        // pipelining 深度 8。
        std::string batch;
        for (int index = 0; index < 8; ++index) batch += GetRequest("/hello");
        CHECK(client.Send(batch));
        for (int index = 0; index < 8; ++index) {
            CHECK(client.Read(response));
            CHECK(response.body == "hello world\n");
        }

        // SSE：chunked 流，事件按序。
        CHECK(client.Send(GetRequest("/events")));
        CHECK(client.Read(response));
        CHECK(response.status == Status::OK);
        CHECK(response.header.get("Content-Type").find("text/event-stream") !=
              std::string::npos);
        CHECK(response.body.find("event: tick\ndata: 0") != std::string::npos);
        CHECK(response.body.find("event: tick\ndata: 2") != std::string::npos);
        client.Close();

        // 并发：8 连接 × 50 请求，全部 200 且内容正确。
        std::atomic<int> failures{0};
        std::vector<std::thread> threads;
        for (int worker = 0; worker < 8; ++worker) {
            threads.emplace_back([&holder, &failures, worker]() {
                HttpClient parallel;
                if (!parallel.Connect(holder->port)) {
                    failures.fetch_add(1);
                    return;
                }
                for (int index = 0; index < 50; ++index) {
                    Response item;
                    const std::string path =
                        (index % 2 == 0) ? "/hello" : "/static/data.bin";
                    if (!parallel.Send(GetRequest(path)) || !parallel.Read(item)) {
                        failures.fetch_add(1);
                        return;
                    }
                    if (path == "/hello") {
                        if (item.status != Status::OK || item.body != "hello world\n") {
                            failures.fetch_add(1);
                        }
                    } else if (item.status != Status::OK || item.body.size() != 1024 ||
                               !BodyAll(item.body, 'A')) {
                        failures.fetch_add(1);
                    }
                }
                parallel.Close();
                (void)worker;
            });
        }
        for (auto& thread : threads) thread.join();
        CHECK(failures.load() == 0);

        holder->server->Stop();
    }
}

}  // namespace

int main() {
    const std::string www = PrepareWww();
    TestRouter();
    TestStaticFiles(www);
    TestBaseServer(www);
    TestBentoServer(www);
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
