/**
 * @file test_tunnel.cpp
 * @brief 公网穿透模块的行为、端到端、重连、并发与性能对比测试。
 *
 * @details 覆盖：
 *          1. 协议载荷编解码（HELLO / HELLO_ACK / OPEN / DATA / CLOSE / nonce）
 *             与畸形输入（截断、超长、尾随字节）；
 *          2. 子域名校验与随机生成；
 *          3. TCP 封装（Dial / Listen / Read / WriteAll / Shutdown）；
 *          4. 端到端：目录页、GET、POST、二进制、4 MiB 大文件、404；
 *          5. Host 子域名路由（`--domain` 模式）；
 *          6. 断线重连：客户端停止 → 502 → 新客户端接上 → 恢复；
 *          7. 并发：10 个连接同时请求，内容互不串扰；
 *          8. 性能：直连 vs 隧道的中位延迟对比。
 *
 *          运行：`./go2cpp_tunnel_test`；失败时退出码 1 并打印文件:行号。
 *
 * @author go2cpp contributors
 * @date 2026-11-02
 * @copyright MIT License
 */

#include "go2cpp/tunnel/client.hpp"
#include "go2cpp/frame/frame_codec.hpp"
#include "go2cpp/tunnel/mux.hpp"
#include "go2cpp/tunnel/net.hpp"
#include "go2cpp/tunnel/protocol.hpp"
#include "go2cpp/tunnel/server.hpp"

#include <memory>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

using go2cpp::tunnel::Close;
using go2cpp::tunnel::DataFrame;
using go2cpp::tunnel::FrameType;
using go2cpp::tunnel::Hello;
using go2cpp::tunnel::HelloAck;
using go2cpp::tunnel::Open;
using go2cpp::tunnel::Status;
using go2cpp::tunnel::TcpConn;
using go2cpp::tunnel::TcpListener;
using go2cpp::tunnel::TunnelClient;
using go2cpp::tunnel::TunnelClientStats;
using go2cpp::tunnel::TunnelClientConfig;
using go2cpp::tunnel::TunnelServer;
using go2cpp::tunnel::TunnelServerConfig;

using Clock = std::chrono::steady_clock;

int g_failures = 0;
int g_checks = 0;

#define TUNNEL_CHECK(condition)                                             \
    do {                                                                    \
        ++g_checks;                                                         \
        if (!(condition)) {                                                 \
            std::fprintf(stderr, "[tunnel-test] FAIL %s:%d: %s\n", __FILE__, \
                         __LINE__, #condition);                             \
            ++g_failures;                                                   \
        }                                                                   \
    } while (false)

#define TUNNEL_CHECK_EQ(actual, expected)                                     \
    do {                                                                      \
        ++g_checks;                                                           \
        const auto& tunnel_actual = (actual);                                 \
        const auto& tunnel_expected = (expected);                             \
        if (!(tunnel_actual == tunnel_expected)) {                            \
            std::fprintf(stderr, "[tunnel-test] FAIL %s:%d: %s != %s\n",      \
                         __FILE__, __LINE__, #actual, #expected);             \
            ++g_failures;                                                     \
        }                                                                     \
    } while (false)

/// 毫秒时间点。
Clock::time_point Now() { return Clock::now(); }

/// 两时间点之间的毫秒（double）。
double MsBetween(Clock::time_point begin, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - begin).count();
}

/// 简易 FNV-1a（大文件校验用，避免引入 hash 头）。
std::uint64_t Fnv1a(const std::string& bytes) {
    std::uint64_t value = 1469598103934665603ULL;
    for (unsigned char character : bytes) {
        value ^= character;
        value *= 1099511628211ULL;
    }
    return value;
}

/// 构造一个完整 HTTP 响应。
std::string BuildHttpResponse(int code, const std::string& content_type,
                              const std::string& body) {
    return "HTTP/1.1 " + std::to_string(code) + " OK\r\nContent-Type: " +
           content_type + "\r\nContent-Length: " + std::to_string(body.size()) +
           "\r\nConnection: close\r\n\r\n" + body;
}

/// 极简 HTTP 服务：每个连接读一个请求，返回 handler 给出的完整响应。
class MiniHttpServer {
public:
    /// 返回完整 HTTP 响应字节；`path` 是请求行里的路径。
    using Handler =
        std::function<std::string(const std::string& path,
                                  const std::string& body)>;

    ~MiniHttpServer() { Stop(); }

    bool Start(Handler handler) {
        m_handler = std::move(handler);
        std::string error;
        m_listener = TcpListener::Listen("127.0.0.1", 0, &error);
        if (!m_listener) {
            std::fprintf(stderr, "mini http listen failed: %s\n", error.c_str());
            return false;
        }
        m_port = m_listener->BoundPort();
        m_running.store(true);
        m_thread = std::thread([this] { AcceptLoop(); });
        return true;
    }

    void Stop() {
        if (!m_running.exchange(false)) {
            return;
        }
        if (m_listener) {
            m_listener->Close();
        }
        if (m_thread.joinable()) {
            m_thread.join();
        }
        for (;;) {
            {
                std::lock_guard<std::mutex> lock(m_worker_mutex);
                if (m_workers == 0) {
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    std::uint16_t port() const { return m_port; }

private:
    void AcceptLoop() {
        while (m_running.load()) {
            TcpConn::Ptr conn = m_listener->Accept(100);
            if (!conn) {
                continue;
            }
            {
                std::lock_guard<std::mutex> lock(m_worker_mutex);
                ++m_workers;
            }
            std::thread([this, conn] {
                Handle(conn);
                {
                    std::lock_guard<std::mutex> lock(m_worker_mutex);
                    --m_workers;
                }
            }).detach();
        }
    }

    void Handle(TcpConn::Ptr conn) {
        std::string buffer;
        std::vector<char> chunk(4096);
        while (buffer.find("\r\n\r\n") == std::string::npos) {
            const go2cpp::tunnel::IoResult result =
                conn->Read(chunk.data(), chunk.size(), 3000);
            if (!result.ok()) {
                conn->Shutdown();
                return;
            }
            buffer.append(chunk.data(), result.count);
            if (buffer.size() > 65536) {
                break;
            }
        }
        const std::size_t line_end = buffer.find("\r\n");
        std::string path = "/";
        if (line_end != std::string::npos) {
            const std::string line = buffer.substr(0, line_end);
            const std::size_t first = line.find(' ');
            const std::size_t second =
                first == std::string::npos ? std::string::npos
                                           : line.find(' ', first + 1);
            if (first != std::string::npos && second != std::string::npos) {
                path = line.substr(first + 1, second - first - 1);
            }
        }
        std::string body;
        const std::size_t header_end = buffer.find("\r\n\r\n");
        if (header_end != std::string::npos) {
            const std::size_t content_length = FindContentLength(buffer);
            std::string rest = buffer.substr(header_end + 4);
            while (rest.size() < content_length) {
                const go2cpp::tunnel::IoResult result =
                    conn->Read(chunk.data(), chunk.size(), 3000);
                if (!result.ok()) {
                    break;
                }
                rest.append(chunk.data(), result.count);
            }
            if (rest.size() > content_length) {
                rest.resize(content_length);
            }
            body = rest;
        }
        const std::string response =
            m_handler ? m_handler(path, body)
                      : BuildHttpResponse(500, "text/plain", "no handler");
        (void)conn->WriteAll(response.data(), response.size(), 5000, nullptr);
        conn->Shutdown();
    }

    static std::size_t FindContentLength(const std::string& head) {
        const std::string key = "Content-Length:";
        const std::size_t position = head.find(key);
        if (position == std::string::npos) {
            return 0;
        }
        std::size_t begin = position + key.size();
        while (begin < head.size() && (head[begin] == ' ' || head[begin] == '\t')) {
            ++begin;
        }
        std::size_t end = begin;
        while (end < head.size() && head[end] >= '0' && head[end] <= '9') {
            ++end;
        }
        return static_cast<std::size_t>(std::stoul(head.substr(begin, end - begin)));
    }

    TcpListener::Ptr m_listener;
    std::uint16_t m_port = 0;
    std::thread m_thread;
    Handler m_handler;
    std::atomic<bool> m_running{false};
    std::mutex m_worker_mutex;
    int m_workers = 0;
};

/// 发一个请求并读回完整响应（按 Content-Length）。
std::string HttpRequest(std::uint16_t port, const std::string& host,
                        const std::string& path,
                        const std::string& method = "GET",
                        const std::string& body = std::string()) {
    std::string error;
    TcpConn::Ptr conn = TcpConn::Dial("127.0.0.1", port, 3000, &error);
    if (!conn) {
        return {};
    }
    std::string request = method + " " + path + " HTTP/1.1\r\nHost: " + host +
                          "\r\nConnection: close\r\n";
    if (!body.empty() || method == "POST") {
        request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    }
    request += "\r\n";
    request += body;
    if (!conn->WriteAll(request.data(), request.size(), 5000, nullptr)) {
        conn->Shutdown();
        return {};
    }
    std::string response;
    std::vector<char> chunk(16384);
    for (;;) {
        const go2cpp::tunnel::IoResult result =
            conn->Read(chunk.data(), chunk.size(), 3000);
        if (result.ok()) {
            response.append(chunk.data(), result.count);
            continue;
        }
        break;
    }
    conn->Shutdown();
    return response;
}

/// 从响应里取出 body（按第一个 \r\n\r\n 切）。
std::string ResponseBody(const std::string& response) {
    const std::size_t header_end = response.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        return {};
    }
    return response.substr(header_end + 4);
}

/// 取响应状态码。
int ResponseStatus(const std::string& response) {
    if (response.size() < 12 || response.compare(0, 5, "HTTP/") != 0) {
        return 0;
    }
    const std::size_t space = response.find(' ');
    if (space == std::string::npos) {
        return 0;
    }
    return std::atoi(response.c_str() + space + 1);
}

/// 取响应头里某个字段的值。
std::string ResponseHeader(const std::string& response, const std::string& name) {
    const std::string wanted = name + ":";
    const std::size_t header_end = response.find("\r\n\r\n");
    const std::string head = header_end == std::string::npos
                                 ? response
                                 : response.substr(0, header_end);
    std::size_t offset = 0;
    while (offset < head.size()) {
        const std::size_t line_end = head.find("\r\n", offset);
        const std::string line =
            head.substr(offset, line_end == std::string::npos
                                    ? std::string::npos
                                    : line_end - offset);
        if (line.size() > wanted.size() &&
            line.compare(0, wanted.size(), wanted) == 0) {
            std::string value = line.substr(wanted.size());
            while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
                value.erase(value.begin());
            }
            return value;
        }
        if (line_end == std::string::npos) {
            break;
        }
        offset = line_end + 2;
    }
    return {};
}

// ---------------------------------------------------------------------------
// 1. 协议编解码
// ---------------------------------------------------------------------------
void TestProtocol() {
    std::printf("[test] 1. 协议载荷编解码\n");
    go2cpp::ErrorPtr error;

    {
        Hello hello;
        hello.subdomain = "shop";
        hello.local_port = 8080;
        hello.token = "s3cret";
        std::vector<std::uint8_t> payload;
        TUNNEL_CHECK(go2cpp::tunnel::EncodeHello(hello, &payload, &error));
        Hello decoded;
        TUNNEL_CHECK(go2cpp::tunnel::DecodeHello(payload.data(), payload.size(),
                                                 &decoded, &error));
        TUNNEL_CHECK_EQ(decoded.subdomain, hello.subdomain);
        TUNNEL_CHECK_EQ(decoded.local_port, hello.local_port);
        TUNNEL_CHECK_EQ(decoded.token, hello.token);
        TUNNEL_CHECK_EQ(decoded.version, go2cpp::tunnel::kProtocolVersion);

        // 截断
        Hello truncated;
        TUNNEL_CHECK(!go2cpp::tunnel::DecodeHello(
            payload.data(), payload.size() - 1, &truncated, &error));
        // 尾随字节
        std::vector<std::uint8_t> longer = payload;
        longer.push_back(0x7F);
        TUNNEL_CHECK(!go2cpp::tunnel::DecodeHello(longer.data(), longer.size(),
                                                  &truncated, &error));
        // 非法子域名
        Hello bad = hello;
        bad.subdomain = "Bad_Name";
        std::vector<std::uint8_t> ignored;
        TUNNEL_CHECK(!go2cpp::tunnel::EncodeHello(bad, &ignored, &error));
    }
    {
        HelloAck ack;
        ack.status = Status::kRejected;
        ack.subdomain = "shop";
        ack.public_url = "http://203.0.113.7:8443/shop/";
        ack.message = "busy";
        std::vector<std::uint8_t> payload;
        TUNNEL_CHECK(go2cpp::tunnel::EncodeHelloAck(ack, &payload, &error));
        HelloAck decoded;
        TUNNEL_CHECK(go2cpp::tunnel::DecodeHelloAck(payload.data(),
                                                    payload.size(), &decoded,
                                                    &error));
        TUNNEL_CHECK(decoded.status == Status::kRejected);
        TUNNEL_CHECK_EQ(decoded.public_url, ack.public_url);
        TUNNEL_CHECK_EQ(decoded.message, ack.message);
    }
    {
        Open open;
        open.stream_id = 42;
        open.peer_ip = "198.51.100.9";
        open.peer_port = 51234;
        std::vector<std::uint8_t> payload;
        TUNNEL_CHECK(go2cpp::tunnel::EncodeOpen(open, &payload, &error));
        Open decoded;
        TUNNEL_CHECK(go2cpp::tunnel::DecodeOpen(payload.data(), payload.size(),
                                                &decoded, &error));
        TUNNEL_CHECK_EQ(decoded.stream_id, open.stream_id);
        TUNNEL_CHECK_EQ(decoded.peer_ip, open.peer_ip);
        TUNNEL_CHECK_EQ(decoded.peer_port, open.peer_port);

        // DATA 边界：超过 kMaxDataChunk 拒绝
        std::vector<std::uint8_t> data;
        std::vector<std::uint8_t> big(go2cpp::tunnel::kMaxDataChunk + 1, 0);
        TUNNEL_CHECK(!go2cpp::tunnel::EncodeData(7, big.data(), big.size(),
                                                 &data, &error));
        std::vector<std::uint8_t> small(go2cpp::tunnel::kMaxDataChunk, 0xAB);
        TUNNEL_CHECK(go2cpp::tunnel::EncodeData(7, small.data(), small.size(),
                                                &data, &error));
        DataFrame frame;
        TUNNEL_CHECK(go2cpp::tunnel::DecodeData(data.data(), data.size(),
                                                &frame, &error));
        TUNNEL_CHECK_EQ(frame.stream_id, 7u);
        TUNNEL_CHECK_EQ(frame.length, go2cpp::tunnel::kMaxDataChunk);
        TUNNEL_CHECK(frame.body[0] == 0xAB);
        // 截断
        TUNNEL_CHECK(!go2cpp::tunnel::DecodeData(data.data(), 3, &frame, &error));
    }
    {
        Close close;
        close.stream_id = 9;
        close.code = 3;
        std::vector<std::uint8_t> payload;
        TUNNEL_CHECK(go2cpp::tunnel::EncodeClose(close, &payload, &error));
        Close decoded;
        TUNNEL_CHECK(go2cpp::tunnel::DecodeClose(payload.data(), payload.size(),
                                                 &decoded, &error));
        TUNNEL_CHECK_EQ(decoded.stream_id, close.stream_id);
        TUNNEL_CHECK_EQ(decoded.code, close.code);
    }
    {
        const std::vector<std::uint8_t> payload =
            go2cpp::tunnel::EncodeNonce(0x0102030405060708ULL);
        TUNNEL_CHECK_EQ(payload.size(), std::size_t{8});
        std::uint64_t nonce = 0;
        TUNNEL_CHECK(go2cpp::tunnel::DecodeNonce(payload.data(), payload.size(),
                                                 &nonce));
        TUNNEL_CHECK_EQ(nonce, 0x0102030405060708ULL);
        TUNNEL_CHECK(!go2cpp::tunnel::DecodeNonce(payload.data(), 7, &nonce));
    }
    {
        // 帧格式：magic 必须是 TUNL，且默认不启用校验和。
        const go2cpp::frame::FrameSpec spec = go2cpp::tunnel::MakeTunnelFrameSpec();
        TUNNEL_CHECK(spec.HasChecksum() == false);
        TUNNEL_CHECK_EQ(spec.max_payload, 65536);
        TUNNEL_CHECK_EQ(spec.magic[0], static_cast<std::uint8_t>('T'));
        TUNNEL_CHECK_EQ(spec.magic[3], static_cast<std::uint8_t>('L'));
        // 用 codec 编一帧再解回来（半包 + 粘包）
        go2cpp::frame::FrameCodec codec(spec);
        go2cpp::frame::Frame frame;
        frame.type = static_cast<std::uint8_t>(FrameType::kData);
        frame.payload = {1, 2, 3, 4, 5};
        std::vector<std::uint8_t> wire;
        go2cpp::frame::Error frame_error;
        TUNNEL_CHECK(codec.Encode(frame, &wire, &frame_error));
        std::vector<go2cpp::frame::Frame> frames;
        std::size_t consumed = 0;
        TUNNEL_CHECK(codec.Feed(wire.data(), 5, &frames, &consumed, &frame_error));
        TUNNEL_CHECK_EQ(frames.size(), std::size_t{0});  // 半包
        TUNNEL_CHECK(codec.Feed(wire.data() + 5, wire.size() - 5, &frames,
                                &consumed, &frame_error));
        TUNNEL_CHECK_EQ(frames.size(), std::size_t{1});
        TUNNEL_CHECK_EQ(frames[0].payload.size(), std::size_t{5});
    }
}

// ---------------------------------------------------------------------------
// 2. 子域名
// ---------------------------------------------------------------------------
void TestSubdomain() {
    std::printf("[test] 2. 子域名校验与随机生成\n");
    TUNNEL_CHECK(go2cpp::tunnel::IsValidSubdomain("shop"));
    TUNNEL_CHECK(go2cpp::tunnel::IsValidSubdomain("a-b-9"));
    TUNNEL_CHECK(!go2cpp::tunnel::IsValidSubdomain(""));
    TUNNEL_CHECK(!go2cpp::tunnel::IsValidSubdomain("-shop"));
    TUNNEL_CHECK(!go2cpp::tunnel::IsValidSubdomain("shop-"));
    TUNNEL_CHECK(!go2cpp::tunnel::IsValidSubdomain("Shop"));
    TUNNEL_CHECK(!go2cpp::tunnel::IsValidSubdomain("sh op"));
    TUNNEL_CHECK(!go2cpp::tunnel::IsValidSubdomain(
        std::string(64, 'a')));
    TUNNEL_CHECK(go2cpp::tunnel::IsValidSubdomain(std::string(63, 'a')));

    std::string previous;
    for (int round = 0; round < 16; ++round) {
        const std::string generated = go2cpp::tunnel::MakeRandomSubdomain(6);
        TUNNEL_CHECK_EQ(generated.size(), std::size_t{6});
        TUNNEL_CHECK(go2cpp::tunnel::IsValidSubdomain(generated));
        TUNNEL_CHECK(generated != previous);  // 极低概率重复，出现即说明随机源坏了
        previous = generated;
    }
}

// ---------------------------------------------------------------------------
// 3. TCP 封装
// ---------------------------------------------------------------------------
void TestTcp() {
    std::printf("[test] 3. TCP 封装（Dial/Listen/读写/关闭）\n");
    std::string error;
    TcpListener::Ptr listener = TcpListener::Listen("127.0.0.1", 0, &error);
    TUNNEL_CHECK(listener != nullptr);
    if (!listener) {
        return;
    }
    const std::uint16_t port = listener->BoundPort();
    TUNNEL_CHECK(port != 0);

    std::atomic<bool> served{false};
    std::thread server_thread([&] {
        TcpConn::Ptr conn = listener->Accept(3000);
        if (!conn) {
            return;
        }
        std::string received;
        std::vector<char> buffer(4096);
        while (received.size() < 100000) {
            const go2cpp::tunnel::IoResult result =
                conn->Read(buffer.data(), buffer.size(), 3000);
            if (!result.ok()) {
                break;
            }
            received.append(buffer.data(), result.count);
            if (received.size() >= 100000) {
                (void)conn->WriteAll(received.data(), received.size(), 3000,
                                     nullptr);
            }
        }
        conn->Shutdown();
        served.store(true);
    });

    TcpConn::Ptr client = TcpConn::Dial("127.0.0.1", port, 3000, &error);
    TUNNEL_CHECK(client != nullptr);
    if (client) {
        std::string payload(100000, '\0');
        for (std::size_t index = 0; index < payload.size(); ++index) {
            payload[index] = static_cast<char>(index % 251);
        }
        TUNNEL_CHECK(client->WriteAll(payload.data(), payload.size(), 3000,
                                      &error));
        std::string echoed;
        std::vector<char> buffer(16384);
        while (echoed.size() < payload.size()) {
            const go2cpp::tunnel::IoResult result =
                client->Read(buffer.data(), buffer.size(), 3000);
            if (!result.ok()) {
                break;
            }
            echoed.append(buffer.data(), result.count);
        }
        TUNNEL_CHECK_EQ(echoed.size(), payload.size());
        TUNNEL_CHECK(echoed == payload);
        client->Shutdown();
        // 关闭后读：返回错误或 EOF，不阻塞。
        const go2cpp::tunnel::IoResult after =
            client->Read(buffer.data(), buffer.size(), 200);
        TUNNEL_CHECK(!after.ok());
    }
    server_thread.join();
    TUNNEL_CHECK(served.load());
    listener->Close();

    // 连接不存在的端口必须失败
    TcpConn::Ptr refused = TcpConn::Dial("127.0.0.1", 1, 200, &error);
    TUNNEL_CHECK(refused == nullptr);
}


// ---------------------------------------------------------------------------
// 4. 端到端：TunnelServer + TunnelClient + 本地 HTTP 服务
// ---------------------------------------------------------------------------

/// 起一套「公网服务端 + 客户端 + 本地 HTTP」的最小栈。
struct EndToEnd {
    std::unique_ptr<MiniHttpServer> local;
    std::unique_ptr<TunnelServer> server;
    std::unique_ptr<TunnelClient> client;
    std::string public_url;
};

/// 本地站点：`/` 返回 HTML、`/echo` 回显 POST、`/big` 返回 1 MiB、`/missing` 404。
std::string LocalHandler(const std::string& path, const std::string& body) {
    if (path == "/echo") {
        return BuildHttpResponse(200, "application/octet-stream", body);
    }
    if (path == "/big") {
        return BuildHttpResponse(200, "application/octet-stream",
                                 std::string(1024U * 1024U, 'x'));
    }
    if (path == "/missing") {
        return BuildHttpResponse(404, "text/plain", "missing");
    }
    return BuildHttpResponse(200, "text/html",
                             "<html><body><h1>shop</h1></body></html>");
}

/// 启动整套栈；失败返回空。
std::unique_ptr<EndToEnd> StartStack(const std::string& token,
                                    int data_conns = 0) {
    auto stack = std::make_unique<EndToEnd>();
    stack->local = std::make_unique<MiniHttpServer>();
    if (!stack->local->Start(LocalHandler)) {
        return nullptr;
    }
    TunnelServerConfig server_config;
    server_config.control_port = 0;  // 内核分配
    server_config.public_port = 0;
    server_config.display_host = "127.0.0.1";
    server_config.token = token;
    server_config.recv_queue_cap = 4U * 1024U * 1024U;
    server_config.drain_timeout_ms = 2000;
    stack->server = std::make_unique<TunnelServer>(server_config);
    go2cpp::ErrorPtr error;
    if (!stack->server->Start(&error)) {
        std::fprintf(stderr, "server start failed: %s\n",
                     go2cpp::ErrorMessage(error).c_str());
        return nullptr;
    }
    TunnelClientConfig client_config;
    client_config.server_host = "127.0.0.1";
    client_config.server_port = stack->server->control_port();
    client_config.local_host = "127.0.0.1";
    client_config.local_port = stack->local->port();
    client_config.subdomain = "shop";
    client_config.token = token;
    client_config.data_conns = data_conns;
    client_config.reconnect_min_ms = 200;
    client_config.reconnect_max_ms = 1000;
    client_config.hello_timeout_ms = 5000;
    stack->client = std::make_unique<TunnelClient>(client_config);
    if (!stack->client->Start(&error) || !stack->client->WaitOnline(8000)) {
        std::fprintf(stderr, "client online failed\n");
        return nullptr;
    }
    stack->public_url = stack->client->public_url();
    return stack;
}

void TestEndToEnd() {
    std::printf("[test] 4. 端到端（转发/状态页/大文件）\n");
    std::unique_ptr<EndToEnd> stack = StartStack("e2e-token");
    TUNNEL_CHECK(stack != nullptr);
    if (!stack) {
        return;
    }
    const std::uint16_t public_port = stack->server->public_port();
    const std::string host = "127.0.0.1:" + std::to_string(public_port);

    // 目录页
    {
        const std::string response = HttpRequest(public_port, host, "/");
        TUNNEL_CHECK_EQ(ResponseStatus(response), 200);
        TUNNEL_CHECK(ResponseBody(response).find("Go2Cpp Tunnel") !=
                     std::string::npos);
    }
    // 状态页 JSON
    {
        const std::string response =
            HttpRequest(public_port, host, "/__tunnel__/status");
        TUNNEL_CHECK_EQ(ResponseStatus(response), 200);
        const std::string json = ResponseBody(response);
        TUNNEL_CHECK(json.find("\"subdomain\":\"shop\"") != std::string::npos);
        TUNNEL_CHECK(json.find("\"draining\":false") != std::string::npos);
    }
    // 路径前缀转发（strip 后 / 命中本地站点）
    {
        const std::string response = HttpRequest(public_port, host, "/shop/");
        TUNNEL_CHECK_EQ(ResponseStatus(response), 200);
        TUNNEL_CHECK(ResponseBody(response).find("<h1>shop</h1>") !=
                     std::string::npos);
    }
    // POST 回显
    {
        const std::string payload = "abcdef-0123456789";
        const std::string response =
            HttpRequest(public_port, host, "/shop/echo", "POST", payload);
        TUNNEL_CHECK_EQ(ResponseStatus(response), 200);
        TUNNEL_CHECK_EQ(ResponseBody(response), payload);
    }
    // 1 MiB 二进制
    {
        const std::string response = HttpRequest(public_port, host, "/shop/big");
        TUNNEL_CHECK_EQ(ResponseStatus(response), 200);
        TUNNEL_CHECK_EQ(ResponseBody(response).size(), std::size_t{1024U * 1024U});
        TUNNEL_CHECK_EQ(Fnv1a(ResponseBody(response)), Fnv1a(std::string(1024U * 1024U, 'x')));
    }
    // 本地 404 透传
    {
        const std::string response =
            HttpRequest(public_port, host, "/shop/missing");
        TUNNEL_CHECK_EQ(ResponseStatus(response), 404);
    }
    // 未知隧道
    {
        const std::string response = HttpRequest(public_port, host, "/nosuch/x");
        TUNNEL_CHECK_EQ(ResponseStatus(response), 404);
    }
    // 客户端统计
    {
        const TunnelClientStats stats = stack->client->Stats();
        TUNNEL_CHECK(stats.online);
        TUNNEL_CHECK(stats.streams_total >= 4);
        TUNNEL_CHECK_EQ(stats.subdomain, std::string("shop"));
    }
}

void TestAuth() {
    std::printf("[test] 5. 鉴权（token -> PSK 挑战应答）\n");
    // 错误 token：必须拿不到在线状态。
    {
        TunnelServerConfig config;
        config.control_port = 0;
        config.public_port = 0;
        config.display_host = "127.0.0.1";
        config.token = "right-token";
        TunnelServer server(config);
        go2cpp::ErrorPtr error;
        TUNNEL_CHECK(server.Start(&error));
        MiniHttpServer local;
        TUNNEL_CHECK(local.Start(LocalHandler));
        TunnelClientConfig client_config;
        client_config.server_host = "127.0.0.1";
        client_config.server_port = server.control_port();
        client_config.local_port = local.port();
        client_config.subdomain = "shop";
        client_config.token = "wrong-token";
        client_config.hello_timeout_ms = 3000;
        TunnelClient client(client_config);
        TUNNEL_CHECK(client.Start(&error));
        TUNNEL_CHECK(!client.WaitOnline(3000));
        // 错误 token 之后，公网端口不应有可用隧道。
        const std::string response = HttpRequest(
            server.public_port(), "127.0.0.1:" + std::to_string(server.public_port()),
            "/shop/");
        TUNNEL_CHECK_EQ(ResponseStatus(response), 404);
        client.Stop();
        server.Stop();
    }
    // 不配 token：服务端发全零挑战，客户端不鉴权也能上线。
    {
        TunnelServerConfig config;
        config.control_port = 0;
        config.public_port = 0;
        config.display_host = "127.0.0.1";
        TunnelServer server(config);
        go2cpp::ErrorPtr error;
        TUNNEL_CHECK(server.Start(&error));
        MiniHttpServer local;
        TUNNEL_CHECK(local.Start(LocalHandler));
        TunnelClientConfig client_config;
        client_config.server_host = "127.0.0.1";
        client_config.server_port = server.control_port();
        client_config.local_port = local.port();
        client_config.subdomain = "open";
        TunnelClient client(client_config);
        TUNNEL_CHECK(client.Start(&error));
        TUNNEL_CHECK(client.WaitOnline(5000));
        const std::uint16_t port = server.public_port();
        const std::string response =
            HttpRequest(port, "127.0.0.1:" + std::to_string(port), "/open/");
        TUNNEL_CHECK_EQ(ResponseStatus(response), 200);
        client.Stop();
        server.Stop();
    }
}

void TestDataConnections() {
    std::printf("[test] 6. 数据连接（控制/数据分离）\n");
    std::unique_ptr<EndToEnd> stack = StartStack("dc-token", 2);
    TUNNEL_CHECK(stack != nullptr);
    if (!stack) {
        return;
    }
    const std::uint16_t port = stack->server->public_port();
    const std::string host = "127.0.0.1:" + std::to_string(port);
    // 10 个请求会轮询到 2 条数据连接 + 控制连接上。
    int ok_count = 0;
    for (int index = 0; index < 10; ++index) {
        const std::string response = HttpRequest(port, host, "/shop/");
        if (ResponseStatus(response) == 200 &&
            ResponseBody(response).find("<h1>shop</h1>") != std::string::npos) {
            ++ok_count;
        }
    }
    TUNNEL_CHECK_EQ(ok_count, 10);
    const std::string status = ResponseBody(
        HttpRequest(port, host, "/__tunnel__/status"));
    TUNNEL_CHECK(status.find("\"data_connections\":2") != std::string::npos);
}

void TestReconnect() {
    std::printf("[test] 7. 断线重连（旧客户端退出 → 502 → 新客户端恢复）\n");
    std::unique_ptr<EndToEnd> stack = StartStack("reconnect-token");
    TUNNEL_CHECK(stack != nullptr);
    if (!stack) {
        return;
    }
    const std::uint16_t port = stack->server->public_port();
    const std::string host = "127.0.0.1:" + std::to_string(port);
    TUNNEL_CHECK_EQ(ResponseStatus(HttpRequest(port, host, "/shop/")), 200);

    stack->client->Stop();
    // 服务端每 200 ms 轮询一次会话存活；给足 2 s 让墓碑生效。
    bool saw_502 = false;
    for (int attempt = 0; attempt < 20 && !saw_502; ++attempt) {
        const std::string response = HttpRequest(port, host, "/shop/");
        if (ResponseStatus(response) == 502) {
            saw_502 = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    TUNNEL_CHECK(saw_502);

    // 新客户端（同一子域名）应立即接管。
    TunnelClientConfig client_config;
    client_config.server_host = "127.0.0.1";
    client_config.server_port = stack->server->control_port();
    client_config.local_port = stack->local->port();
    client_config.subdomain = "shop";
    client_config.token = "reconnect-token";
    TunnelClient client(client_config);
    go2cpp::ErrorPtr error;
    TUNNEL_CHECK(client.Start(&error));
    TUNNEL_CHECK(client.WaitOnline(5000));
    TUNNEL_CHECK_EQ(ResponseStatus(HttpRequest(port, host, "/shop/")), 200);
    client.Stop();
}

void TestConcurrency() {
    std::printf("[test] 8. 并发（10 连接互不串扰）\n");
    std::unique_ptr<EndToEnd> stack = StartStack("concurrent-token");
    TUNNEL_CHECK(stack != nullptr);
    if (!stack) {
        return;
    }
    const std::uint16_t port = stack->server->public_port();
    const std::string host = "127.0.0.1:" + std::to_string(port);
    std::atomic<int> ok_count{0};
    std::vector<std::thread> workers;
    for (int index = 0; index < 10; ++index) {
        workers.emplace_back([port, host, &ok_count, index] {
            const std::string payload = "worker-" + std::to_string(index);
            const std::string response =
                HttpRequest(port, host, "/shop/echo", "POST", payload);
            if (ResponseStatus(response) == 200 &&
                ResponseBody(response) == payload) {
                ok_count.fetch_add(1);
            }
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
    TUNNEL_CHECK_EQ(ok_count.load(), 10);
}

void TestPerformance() {
    std::printf("[test] 9. 性能（直连 vs 隧道，中位延迟）\n");
    std::unique_ptr<EndToEnd> stack = StartStack("perf-token");
    TUNNEL_CHECK(stack != nullptr);
    if (!stack) {
        return;
    }
    const std::uint16_t public_port = stack->server->public_port();
    const std::string host = "127.0.0.1:" + std::to_string(public_port);
    const int rounds = 120;
    std::vector<double> direct;
    std::vector<double> tunnel;
    for (int index = 0; index < rounds; ++index) {
        const auto begin = Now();
        const std::string response =
            HttpRequest(stack->local->port(),
                        "127.0.0.1:" + std::to_string(stack->local->port()),
                        "/small");
        const auto end = Now();
        if (ResponseStatus(response) == 200) {
            direct.push_back(MsBetween(begin, end));
        }
        const auto tunnel_begin = Now();
        const std::string tunneled = HttpRequest(public_port, host, "/shop/small");
        const auto tunnel_end = Now();
        if (ResponseStatus(tunneled) == 200) {
            tunnel.push_back(MsBetween(tunnel_begin, tunnel_end));
        }
    }
    TUNNEL_CHECK(!direct.empty());
    TUNNEL_CHECK(!tunnel.empty());
    if (direct.empty() || tunnel.empty()) {
        return;
    }
    std::sort(direct.begin(), direct.end());
    std::sort(tunnel.begin(), tunnel.end());
    const double direct_p50 = direct[direct.size() / 2];
    const double tunnel_p50 = tunnel[tunnel.size() / 2];
    const double ratio = tunnel_p50 / direct_p50;
    std::printf("  直连 p50=%.3f ms  隧道 p50=%.3f ms  比值=%.2fx\n",
                direct_p50, tunnel_p50, ratio);
    // 本机回环、且直连基线只有 0.36 ms 时，隧道固定开销（两端各 5 条
    // RelayPipe 线程 + 2 次 connect）会放大比值。这里用 4x 防回归；
    // 独立进程 demo（直连基线 0.7 ms）实测 1.9x，见 docs/modules/tunnel.md。
    TUNNEL_CHECK(ratio < 4.0);
}

/// raw 数据连接握手回归：HELLO_ACK 之后紧跟的裸字节不能被帧读取器预读吞掉。
/// @details 旧实现用 `FrameReader::Next`（一次 recv 8 KiB），服务端在 ACK
///          后立刻写 HTTP 请求头时，请求头会被当成下一帧的半包留在
///          `FrameCodec` 缓冲里，reader 析构即丢失；表现为随机 bad magic /
///          请求缺头。`NextExact` 按帧长精确读取，读完 ACK 后一个字节不丢。
void TestRawAckNoPreread() {
    std::string error;
    TcpListener::Ptr listener = TcpListener::Listen("127.0.0.1", 0, &error);
    TUNNEL_CHECK(listener != nullptr);
    if (!listener) {
        return;
    }
    const std::string raw = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
    std::thread server([&listener, &raw] {
        TcpConn::Ptr peer = listener->Accept(3000);
        if (!peer) {
            return;
        }
        go2cpp::frame::FrameCodec codec(
            go2cpp::tunnel::MakeTunnelFrameSpec(false));
        go2cpp::frame::Frame ack;
        ack.type = static_cast<std::uint8_t>(FrameType::kHelloAck);
        const std::string payload = "ACKPAYLOAD";
        ack.payload.assign(payload.begin(), payload.end());
        std::vector<std::uint8_t> wire;
        go2cpp::frame::Error encode_error;
        if (codec.Encode(ack, &wire, &encode_error)) {
            wire.insert(wire.end(), raw.begin(), raw.end());
            (void)peer->WriteAll(wire.data(), wire.size(), 3000, nullptr);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        peer->Shutdown();
    });
    TcpConn::Ptr conn =
        TcpConn::Dial("127.0.0.1", listener->BoundPort(), 3000, &error);
    TUNNEL_CHECK(conn != nullptr);
    if (conn) {
        go2cpp::tunnel::FrameReader reader(conn, false);
        go2cpp::frame::Frame frame;
        std::string read_error;
        TUNNEL_CHECK(reader.NextExact(2000, &frame, &read_error));
        TUNNEL_CHECK(static_cast<FrameType>(frame.type) == FrameType::kHelloAck);
        char buffer[512] = {};
        const go2cpp::tunnel::IoResult rest =
            conn->Read(buffer, sizeof(buffer), 1000);
        TUNNEL_CHECK(rest.count == raw.size());
        TUNNEL_CHECK(rest.count == raw.size() &&
                     std::memcmp(buffer, raw.data(), raw.size()) == 0);
        conn->Shutdown();
    }
    listener->Close();
    server.join();
}

}  // namespace

int main() {
    std::printf("=== go2cpp tunnel test ===\n");
    TestProtocol();
    TestSubdomain();
    TestTcp();
    TestEndToEnd();
    TestAuth();
    TestDataConnections();
    TestReconnect();
    TestConcurrency();
    TestPerformance();
    TestRawAckNoPreread();

    std::printf("\n[summary] checks=%d failures=%d\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
