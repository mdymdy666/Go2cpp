/**
 * @file server.cpp
 * @brief Go2Cpp WebSocket echo 服务端（压测用）。
 *
 * @details 直接复用 `bento::WsServer`（fiber-per-connection，与 Go 的
 *          goroutine-per-connection 同构）：HTTP Upgrade 走 `net::WebSocket`
 *          的 RFC 6455 握手，Text/Binary 消息原样回显，Ping 由协议层自动
 *          Pong。进程启动后打印 `READY pid=<pid> port=<port>`，供压测脚本
 *          读取端口与进程号；收到 SIGTERM/SIGINT 后 `Stop()` 并退出。
 *
 * 用法：
 * ```sh
 * ./ws_bench_server --port 0 --workers 4 --max-conn 0
 * ```
 * 参数：`--port` 监听端口（0 = 内核分配）；`--workers` OS 线程数（= IOManager
 * 的 P/M 数量）；`--max-conn` 可选连接上限（0 = 不限，超过时回 1013 关闭）。
 */

#include "go2cpp/bento/net/ws_server.hpp"

#include <atomic>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <unistd.h>

namespace {

/// SIGTERM/SIGINT 置位；主循环轮询该标记后优雅停止。
volatile std::sig_atomic_t g_stop = 0;

void HandleSignal(int) { g_stop = 1; }

struct Options {
    std::uint16_t port{0};
    int workers{4};
    std::size_t max_conn{0};
};

void PrintUsage(const char* program) {
    std::fprintf(stderr,
                 "usage: %s [--port N] [--workers N] [--max-conn N]\n",
                 program);
}

/// 解析 `--key value` 形式的参数；未知参数返回 false。
bool ParseOptions(int argc, char** argv, Options& options) {
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        const bool has_value = index + 1 < argc;
        if (argument == "--port") {
            if (!has_value) {
                return false;
            }
            options.port = static_cast<std::uint16_t>(std::atoi(argv[++index]));
        } else if (argument == "--workers") {
            if (!has_value) {
                return false;
            }
            options.workers = std::atoi(argv[++index]);
            if (options.workers <= 0) {
                options.workers = 1;
            }
        } else if (argument == "--max-conn") {
            if (!has_value) {
                return false;
            }
            options.max_conn =
                static_cast<std::size_t>(std::strtoull(argv[++index], nullptr, 10));
        } else if (argument == "--help" || argument == "-h") {
            PrintUsage(argv[0]);
            return false;
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", argument.c_str());
            return false;
        }
    }
    return true;
}

std::atomic<std::size_t> g_handshakes{0};
std::atomic<std::size_t> g_rejected{0};
std::atomic<std::size_t> g_echoed{0};

}  // namespace

int main(int argc, char** argv) {
    Options options;
    if (!ParseOptions(argc, argv, options)) {
        return 2;
    }

    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);
    std::signal(SIGPIPE, SIG_IGN);

    go2cpp::bento::WsServer server;
    go2cpp::bento::WsServerConfig config;
    config.worker_threads = static_cast<std::size_t>(options.workers);
    config.backlog = 8192;
    config.max_queued_connections = 65536;
    // 压测只关心帧层收发，不做 UTF-8 校验以降低单消息开销。
    config.websocket.validate_utf8 = false;

    const std::size_t max_conn = options.max_conn;
    server.OnConnection([max_conn](const go2cpp::bento::WsConnection::Ptr& connection) {
        const std::size_t count = g_handshakes.fetch_add(1) + 1;
        if (max_conn > 0 && count > max_conn) {
            g_rejected.fetch_add(1);
            (void)connection->Close(1013, "server busy");
        }
    });
    server.OnMessage([](const go2cpp::bento::WsConnection::Ptr& connection,
                        const go2cpp::net::WebSocketMessage& message) {
        g_echoed.fetch_add(1);
        if (message.text()) {
            (void)connection->SendText(message.payload);
        } else {
            (void)connection->SendBinary(message.payload);
        }
    });

    const auto listened = server.Listen("127.0.0.1", options.port, config);
    if (!listened.ok()) {
        std::fprintf(stderr, "listen failed: %s (errno=%d)\n",
                     listened.message.c_str(), listened.system_error);
        return 1;
    }
    const auto started = server.Start();
    if (!started.ok()) {
        std::fprintf(stderr, "start failed: %s\n", started.message.c_str());
        return 1;
    }

    std::printf("READY pid=%d port=%u\n", static_cast<int>(::getpid()),
                static_cast<unsigned>(server.Port()));
    std::printf("listening on ws://127.0.0.1:%u/ws\n",
                static_cast<unsigned>(server.Port()));
    std::fflush(stdout);

    while (g_stop == 0) {
        ::usleep(50 * 1000);
    }

    server.Stop();
    const auto stats = server.Stats();
    std::printf("STATS handshakes=%llu rejected=%llu messages=%llu active=%zu\n",
                static_cast<unsigned long long>(stats.handshakes),
                static_cast<unsigned long long>(stats.rejected_handshakes),
                static_cast<unsigned long long>(stats.received_messages),
                stats.active_connections);
    std::fflush(stdout);
    return 0;
}
