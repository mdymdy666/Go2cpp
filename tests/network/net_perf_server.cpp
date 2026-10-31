/**
 * @file net_perf_server.cpp
 * @brief 网络协议对标 Go 的基准服务端：TCP echo / UDP echo / HTTP。
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 *
 * @details 三类服务端都运行在 `io::IOManager` 上，与 Go 的
 *          goroutine-per-connection 模型同构：
 *
 *          - **TCP**：accept 循环本身是一个 Fiber，每接受一条连接再起一个
 *            Fiber 跑 echo 循环；连接数不受 OS 线程数限制。
 *          - **UDP**：Fiber 内用 `recvmmsg`/`sendmmsg` 批量收发，一次系统
 *            调用处理多条数据报。
 *          - **HTTP**：`bento::HttpServer`（内部已是每连接一 Fiber），
 *            handler 返回固定 12 字节 body。
 *
 *          程序只依赖 go2cpp 运行时与协议库，不引入测试框架；供驱动脚本
 *          以子进程方式拉起，打印 `READY pid=<pid> port=<port>` 后进入服务
 *          循环，收到 SIGTERM 退出。
 *
 * ## 用法
 * ```sh
 * taskset -c 2,3 ./net_perf_server http 0 2
 * taskset -c 2,3 ./net_perf_server tcp 0 2
 * taskset -c 2,3 ./net_perf_server udp 0 2
 * ```
 * 参数依次为：模式、端口（0 = 内核分配）、worker 线程数（与 Go 的
 * `GOMAXPROCS` 对齐）。
 */

#include "go2cpp/bento/net/http_server.hpp"
#include "go2cpp/bento/net/ws_server.hpp"
#include "go2cpp/bento/net/mqtt.hpp"
#include "go2cpp/bento/net/coap.hpp"

#include "go2cpp/network/address.hpp"
#include "go2cpp/network/io.hpp"
#include "go2cpp/network/socket.hpp"

#include <array>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <unistd.h>

namespace {

using go2cpp::bento::HttpServer;
using go2cpp::bento::HttpServerConfig;
using go2cpp::bento::WsServer;
using go2cpp::bento::WsServerConfig;
using go2cpp::net::RecvBatchEntry;
using go2cpp::net::SendBatchEntry;
using go2cpp::net::Socket;

/// SIGTERM/SIGINT 置位，服务循环看到后退出。
volatile std::sig_atomic_t g_stop = 0;

void HandleSignal(int) { g_stop = 1; }

void PrintReady(unsigned port) {
    std::printf("READY pid=%d port=%u\n", static_cast<int>(::getpid()), port);
    std::fflush(stdout);
}

/// 构造与 Go 基线一致的调度配置：P / M 数量都等于 worker。
go2cpp::SchedulerConfig MakeSchedulerConfig(int workers) {
    go2cpp::SchedulerConfig config;
    config.processor_count = static_cast<std::size_t>(workers);
    config.max_workers = static_cast<std::size_t>(workers);
    config.min_workers = static_cast<std::size_t>(workers);
    config.collect_metrics = false;
    return config;
}

/// 单条 TCP 连接的 echo 循环：一次读、循环写回，直到对端关闭或出错。
void EchoConnection(const Socket::Ptr& connection) {
    std::vector<std::uint8_t> buffer(64 * 1024);
    for (;;) {
        const auto read = connection->recv(buffer.data(), buffer.size());
        if (!read.ok() || read.transferred <= 0) {
            return;
        }
        std::size_t offset = 0;
        while (offset < static_cast<std::size_t>(read.transferred)) {
            const auto written = connection->send(
                buffer.data() + offset,
                static_cast<std::size_t>(read.transferred) - offset);
            if (!written.ok() || written.transferred <= 0) {
                return;
            }
            offset += static_cast<std::size_t>(written.transferred);
        }
    }
}

/// 每连接一 Fiber 的 TCP echo：accept 与连接处理都在 IOManager 上。
void ServeTcp(std::uint16_t port, int workers) {
    go2cpp::io::IOManager manager(MakeSchedulerConfig(workers));
    if (!manager.start()) {
        std::fprintf(stderr, "tcp: manager start failed\n");
        std::exit(1);
    }
    manager.go([&manager, port]() {
        auto listener = Socket::CreateTCP(AF_INET);
        if (!listener) {
            std::fprintf(stderr, "tcp: create socket failed\n");
            std::exit(1);
        }
        int reuse = 1;
        (void)listener->set_option(SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        const auto address = go2cpp::net::Address::LookupAny(
            "127.0.0.1:" + std::to_string(port), AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (!address || !listener->bind(*address) || !listener->listen(512)) {
            std::fprintf(stderr, "tcp: bind/listen failed: %s\n", std::strerror(errno));
            std::exit(1);
        }
        unsigned bound = 0;
        if (const auto ip = std::dynamic_pointer_cast<go2cpp::net::IPAddress>(
                listener->local_address())) {
            bound = ip->port();
        }
        PrintReady(bound);
        while (g_stop == 0) {
            auto connection = listener->accept();
            if (!connection) {
                continue;
            }
            manager.go([connection]() {
                EchoConnection(connection);
                (void)connection->close();
            });
        }
        (void)listener->close();
    });
    while (g_stop == 0) {
        ::sleep(1);
    }
    manager.shutdown();
}

/// UDP 批量参数：一次 recvmmsg 最多 64 条，单条上限 2KB（与压测负载 64B 匹配）。
constexpr std::size_t kUdpBatch = 64;
constexpr std::size_t kUdpDatagram = 2048;

/// UDP echo 的一个批量收发循环。
void UdpWorker(const Socket::Ptr& socket) {
    std::vector<std::array<std::uint8_t, kUdpDatagram>> buffers(kUdpBatch);
    std::vector<RecvBatchEntry> entries(kUdpBatch);
    std::vector<SendBatchEntry> sends(kUdpBatch);
    while (g_stop == 0) {
        for (std::size_t i = 0; i < kUdpBatch; ++i) {
            entries[i].data = buffers[i].data();
            entries[i].capacity = buffers[i].size();
            entries[i].received = 0;
            entries[i].from_length = 0;
        }
        const auto received = socket->recv_batch(entries.data(), kUdpBatch);
        if (!received.ok() || received.transferred <= 0) {
            if (received.error == EBADF || received.error == ECANCELED) {
                return;
            }
            continue;
        }
        const auto count = static_cast<std::size_t>(received.transferred);
        for (std::size_t i = 0; i < count; ++i) {
            sends[i].data = entries[i].data;
            sends[i].length = entries[i].received;
            sends[i].to = reinterpret_cast<const sockaddr*>(&entries[i].from);
            sends[i].to_length = entries[i].from_length;
        }
        std::size_t offset = 0;
        while (offset < count) {
            const auto sent = socket->send_batch(sends.data() + offset, count - offset);
            if (!sent.ok() || sent.transferred <= 0) {
                break;
            }
            offset += static_cast<std::size_t>(sent.transferred);
        }
    }
}

/// UDP echo：一个共享 socket + workers 个批量收发 Fiber。
void ServeUdp(std::uint16_t port, int workers) {
    go2cpp::io::IOManager manager(MakeSchedulerConfig(workers));
    if (!manager.start()) {
        std::fprintf(stderr, "udp: manager start failed\n");
        std::exit(1);
    }
    auto socket = Socket::CreateUDP(AF_INET);
    if (!socket) {
        std::fprintf(stderr, "udp: create socket failed\n");
        std::exit(1);
    }
    int reuse = 1;
    (void)socket->set_option(SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    const auto address = go2cpp::net::Address::LookupAny(
        "127.0.0.1:" + std::to_string(port), AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (!address || !socket->bind(*address)) {
        std::fprintf(stderr, "udp: bind failed: %s\n", std::strerror(errno));
        std::exit(1);
    }
    unsigned bound = 0;
    if (const auto ip = std::dynamic_pointer_cast<go2cpp::net::IPAddress>(
            socket->local_address())) {
        bound = ip->port();
    }
    PrintReady(bound);
    for (int worker = 0; worker < workers; ++worker) {
        manager.go([socket]() { UdpWorker(socket); });
    }
    while (g_stop == 0) {
        ::sleep(1);
    }
    (void)socket->close();
    manager.shutdown();
}

/// 单条 MQTT 连接：一个 broker + 流式解码，出站包原样写回。
void MqttConnectionLoop(const Socket::Ptr& connection) {
    go2cpp::bento::net::MqttBroker broker;
    std::string assigned;
    if (broker.OpenSession("bench-0", &assigned) != go2cpp::bento::net::MqttStatus::kOk) {
        return;
    }
    std::vector<char> buffer(64 * 1024);
    std::vector<go2cpp::bento::net::MqttOutbound> outbound;
    for (;;) {
        const auto received = connection->recv(buffer.data(), buffer.size());
        if (received.transferred <= 0) {
            return;
        }
        outbound.clear();
        (void)broker.HandleBytes(
            assigned, reinterpret_cast<const std::uint8_t*>(buffer.data()),
            static_cast<std::size_t>(received.transferred), &outbound);
        for (const auto& message : outbound) {
            (void)connection->send(message.bytes.data(), message.bytes.size());
        }
    }
}

/// MQTT：每连接一 Fiber + 每连接一个 broker（单连接自订阅回环）。
void ServeMqtt(std::uint16_t port, int workers) {
    go2cpp::io::IOManager manager(MakeSchedulerConfig(workers));
    if (!manager.start()) {
        std::fprintf(stderr, "mqtt: manager start failed\n");
        std::exit(1);
    }
    manager.go([&manager, port]() {
        auto listener = Socket::CreateTCP(AF_INET);
        if (!listener) {
            std::exit(1);
        }
        int reuse = 1;
        (void)listener->set_option(SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        const auto address = go2cpp::net::Address::LookupAny(
            "127.0.0.1:" + std::to_string(port), AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (!address || !listener->bind(*address) || !listener->listen(512)) {
            std::fprintf(stderr, "mqtt: bind/listen failed: %s\n", std::strerror(errno));
            std::exit(1);
        }
        unsigned bound = 0;
        if (const auto ip = std::dynamic_pointer_cast<go2cpp::net::IPAddress>(
                listener->local_address())) {
            bound = ip->port();
        }
        PrintReady(bound);
        while (g_stop == 0) {
            auto connection = listener->accept();
            if (!connection) {
                continue;
            }
            manager.go([connection]() {
                MqttConnectionLoop(connection);
                (void)connection->close();
            });
        }
        (void)listener->close();
    });
    while (g_stop == 0) {
        ::sleep(1);
    }
    manager.shutdown();
}

/// CoAP：一个 Fiber 串行收发包，CoapServer 负责解码/去重/组装响应。
void ServeCoap(std::uint16_t port, int workers) {
    go2cpp::io::IOManager manager(MakeSchedulerConfig(workers));
    if (!manager.start()) {
        std::fprintf(stderr, "coap: manager start failed\n");
        std::exit(1);
    }
    auto socket = Socket::CreateUDP(AF_INET);
    if (!socket) {
        std::exit(1);
    }
    int reuse = 1;
    (void)socket->set_option(SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    const auto address = go2cpp::net::Address::LookupAny(
        "127.0.0.1:" + std::to_string(port), AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (!address || !socket->bind(*address)) {
        std::fprintf(stderr, "coap: bind failed: %s\n", std::strerror(errno));
        std::exit(1);
    }
    unsigned bound = 0;
    if (const auto ip = std::dynamic_pointer_cast<go2cpp::net::IPAddress>(
            socket->local_address())) {
        bound = ip->port();
    }
    PrintReady(bound);
    manager.go([socket]() {
        go2cpp::net::AddressPtr peer;
        go2cpp::bento::net::CoapServer server(
            [&socket, &peer](const std::vector<std::uint8_t>& wire) {
                if (!peer) {
                    return false;
                }
                const auto result = socket->send_to(wire.data(), wire.size(), *peer);
                return result.transferred == static_cast<ssize_t>(wire.size());
            });
        server.SetHandler([](const go2cpp::bento::net::CoapMessage& request) {
            if (request.code == go2cpp::bento::net::CoapCode::kGet) {
                return go2cpp::bento::net::CoapResponse::Content({'o', 'k'}, 0);
            }
            return go2cpp::bento::net::CoapServer::DefaultResponse(request.code);
        });
        std::vector<char> buffer(4096);
        while (g_stop == 0) {
            go2cpp::net::AddressPtr from;
            const auto received = socket->recv_from(buffer.data(), buffer.size(), from, 0);
            if (received.transferred <= 0) {
                continue;
            }
            peer = std::move(from);
            (void)server.Handle(reinterpret_cast<const std::uint8_t*>(buffer.data()),
                                static_cast<std::size_t>(received.transferred));
        }
    });
    while (g_stop == 0) {
        ::sleep(1);
    }
    (void)socket->close();
    manager.shutdown();
}

/// HTTP：bento::HttpServer 内部每连接一 Fiber；handler 返回固定 12 字节。
void ServeHttp(std::uint16_t port, int workers) {
    HttpServer server;
    HttpServerConfig config;
    config.worker_threads = static_cast<std::size_t>(workers > 0 ? workers : 2);
    config.max_requests_per_connection = 1000000;
    server.OnRequest([](const go2cpp::http::Request& request) {
        go2cpp::http::Response response;
        response.header.set("Content-Type", "text/plain");
        if (request.path() == "/hello") {
            response.status = go2cpp::http::Status::OK;
            response.body = "hello world";
        } else if (request.path() == "/big") {
            response.status = go2cpp::http::Status::OK;
            response.body.assign(16 * 1024, 'x');
        } else {
            response.status = go2cpp::http::Status::NotFound;
            response.body = "not found";
        }
        return response;
    });
    const auto listened = server.Listen("127.0.0.1", port, config);
    if (!listened.ok()) {
        std::fprintf(stderr, "http: listen failed: %s\n", listened.message.c_str());
        std::exit(1);
    }
    if (!server.Start().ok()) {
        std::fprintf(stderr, "http: start failed\n");
        std::exit(1);
    }
    PrintReady(server.Port());
    while (g_stop == 0) {
        ::sleep(1);
    }
    server.Stop();
}

/// WebSocket echo：bento::WsServer；文本/二进制原样回显。
void ServeWs(std::uint16_t port, int workers) {
    WsServer server;
    WsServerConfig config;
    config.worker_threads = static_cast<std::size_t>(workers > 0 ? workers : 2);
    server.OnMessage([](const go2cpp::bento::WsConnection::Ptr& connection,
                        const go2cpp::net::WebSocketMessage& message) {
        if (message.text()) {
            (void)connection->SendText(message.payload);
        } else {
            (void)connection->SendBinary(message.payload);
        }
    });
    const auto listened = server.Listen("127.0.0.1", port, config);
    if (!listened.ok()) {
        std::fprintf(stderr, "ws: listen failed: %s\n", listened.message.c_str());
        std::exit(1);
    }
    if (!server.Start().ok()) {
        std::fprintf(stderr, "ws: start failed\n");
        std::exit(1);
    }
    PrintReady(server.Port());
    while (g_stop == 0) {
        ::sleep(1);
    }
    server.Stop();
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <tcp|udp|http> [port] [workers]\n", argv[0]);
        return 2;
    }
    const std::string mode = argv[1];
    const std::uint16_t port = argc > 2 ? static_cast<std::uint16_t>(std::atoi(argv[2])) : 0;
    const int workers = argc > 3 ? std::atoi(argv[3]) : 2;

    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);
    std::signal(SIGPIPE, SIG_IGN);

    if (mode == "tcp") {
        ServeTcp(port, workers);
    } else if (mode == "udp") {
        ServeUdp(port, workers);
    } else if (mode == "http") {
        ServeHttp(port, workers);
    } else if (mode == "ws") {
        ServeWs(port, workers);
    } else if (mode == "mqtt") {
        ServeMqtt(port, workers);
    } else if (mode == "coap") {
        ServeCoap(port, workers);
    } else {
        std::fprintf(stderr, "unknown mode: %s\n", mode.c_str());
        return 2;
    }
    return 0;
}
