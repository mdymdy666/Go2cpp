/**
 * @file client_main.cpp
 * @brief `tunnel-client` 命令行入口（跑在 NAT 后的本地机器）。
 *
 * @details 用法（一条命令）：
 * ```
 *   ./tunnel-client --server 203.0.113.7:7443 --local 8080 --subdomain shop
 * ```
 * 启动后客户端会打印完整公网 URL（如 `http://203.0.113.7:8443/shop/`），
 * 随后保持前台运行；`SIGINT`/`SIGTERM` 优雅退出。
 *
 * @author go2cpp contributors
 * @date 2026-11-02
 * @copyright MIT License
 */

#include "go2cpp/logging/log.hpp"
#include "go2cpp/tunnel/client.hpp"

#include <csignal>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include <chrono>
#include <thread>

namespace {

std::atomic<bool> g_stop{false};

void OnSignal(int /*signal_number*/) { g_stop.store(true); }

/**
 * @brief 初始化命令行入口的日志出口。
 *
 * @details 与 `server_main.cpp` 相同的策略：信息与警告走 stdout、错误走
 *          stderr，`{message}\n` 模式保持原先单行文本的输出形态。
 */
void InitLogging() {
    using go2cpp::log::Level;
    using go2cpp::log::LogItemWorker;
    using go2cpp::log::LogSink;
    using go2cpp::log::MinimumLevelFilter;
    using go2cpp::log::PatternFormatter;
    auto& manager = go2cpp::log::LoggerManager::Instance();
    manager.Configure(Level::Info, /*stdout_enabled=*/false, "log", "go2cpp.log",
                      "{message}\n");
    const auto make_worker = [](Level level, LogSink::ptr sink) {
        return std::make_shared<LogItemWorker>(
            std::make_shared<MinimumLevelFilter>(level),
            std::make_shared<PatternFormatter>("{message}\n"), std::move(sink));
    };
    // step1: 信息/警告 → stdout
    auto info_logger = manager.Get("tunnel");
    info_logger->SetLevel(Level::Info);
    info_logger->SetPropagate(false);
    info_logger->ConfigureDefaults(
        {make_worker(Level::Info, std::make_shared<go2cpp::log::StdoutSink>())});
    // step2: 错误 → stderr
    auto error_logger = manager.Get("tunnel.error");
    error_logger->SetLevel(Level::Error);
    error_logger->SetPropagate(false);
    error_logger->ConfigureDefaults(
        {make_worker(Level::Error, std::make_shared<go2cpp::log::StderrSink>())});
}

void PrintUsage(const char* program) {
    const std::string text =
        std::string("用法: ") + program +
        " --server <host[:port]> --local <port|host:port> "
        "[--subdomain <名字>]\n"
        "\n"
        "把本地 HTTP 服务发布成公网 URL（服务端需先运行 tunnel-server）。\n"
        "\n"
        "  --server <host[:port]>  服务端控制端口（默认端口 7443）\n"
        "  --local <port|host:port> 本地服务（默认 127.0.0.1:<port>）\n"
        "  --subdomain <名字>      期望子域名（默认服务端随机分配）\n"
        "  --token <令牌>          服务端开启鉴权时必填\n"
        "  --data-conns <数量>     额外数据连接数（0..64，默认 0；\n"
        "                          控制面与数据面分离，高并发时用）\n"
        "  --max-streams <数量>    本地并发流上限（默认 128）\n"
        "  --crc                   启用帧尾 CRC-32（两端必须一致）\n"
        "  --quiet                 不打印每次上线/下线事件\n"
        "  -h, --help              显示本帮助\n"
        "\n"
        "示例:\n"
        "  " +
        program +
        " --server 203.0.113.7:7443 --local 8080 --subdomain shop\n"
        "  → 已上线: http://203.0.113.7:8443/shop/  →  127.0.0.1:8080";
    GO2CPP_LOG_INFO(GO2CPP_LOG_NAME("tunnel")) << text;
}

bool ParseU16(const char* text, std::uint16_t* out) {
    if (text == nullptr || *text == '\0') {
        return false;
    }
    char* end = nullptr;
    const unsigned long value = std::strtoul(text, &end, 10);
    if (end == nullptr || *end != '\0' || value > 65535UL) {
        return false;
    }
    *out = static_cast<std::uint16_t>(value);
    return true;
}

bool ParseSize(const char* text, std::size_t* out) {
    if (text == nullptr || *text == '\0') {
        return false;
    }
    char* end = nullptr;
    const unsigned long long value = std::strtoull(text, &end, 10);
    if (end == nullptr || *end != '\0') {
        return false;
    }
    *out = static_cast<std::size_t>(value);
    return true;
}

const char* NextArg(int argc, char** argv, int* index, const char* name) {
    if (*index + 1 >= argc) {
        GO2CPP_LOG_ERROR(GO2CPP_LOG_NAME("tunnel.error"))
            << "错误: " << name << " 缺少参数";
        return nullptr;
    }
    *index += 1;
    return argv[*index];
}

/// 拆 `host:port` / `host` / `[v6]:port`；端口部分非法时返回 false。
bool SplitHostPort(const std::string& text, std::uint16_t default_port,
                   std::string* host, std::uint16_t* port) {
    *host = text;
    *port = default_port;
    if (text.empty()) {
        return false;
    }
    if (text.front() == '[') {
        const std::size_t close = text.find(']');
        if (close == std::string::npos) {
            return false;
        }
        *host = text.substr(1, close - 1);
        if (close + 1 < text.size() && text[close + 1] == ':') {
            return ParseU16(text.c_str() + close + 2, port);
        }
        return true;
    }
    const std::size_t colon = text.rfind(':');
    if (colon == std::string::npos || text.find(':') != colon) {
        return true;  // 没有端口，或裸 IPv6（不支持裸 IPv6 带端口）
    }
    *host = text.substr(0, colon);
    return ParseU16(text.c_str() + colon + 1, port);
}

}  // namespace

int main(int argc, char** argv) {
    InitLogging();
    go2cpp::tunnel::TunnelClientConfig config;
    bool quiet = false;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "-h" || argument == "--help") {
            PrintUsage(argv[0]);
            return 0;
        }
        if (argument == "--server") {
            const char* value = NextArg(argc, argv, &index, "--server");
            if (value == nullptr ||
                !SplitHostPort(value, 7443, &config.server_host,
                               &config.server_port)) {
                GO2CPP_LOG_ERROR(GO2CPP_LOG_NAME("tunnel.error"))
                    << "错误: 非法 --server（示例 1.2.3.4:7443）";
                return 2;
            }
        } else if (argument == "--local") {
            const char* value = NextArg(argc, argv, &index, "--local");
            if (value == nullptr ||
                !SplitHostPort(value, 80, &config.local_host,
                               &config.local_port)) {
                GO2CPP_LOG_ERROR(GO2CPP_LOG_NAME("tunnel.error"))
                    << "错误: 非法 --local（示例 8080）";
                return 2;
            }
        } else if (argument == "--subdomain") {
            const char* value = NextArg(argc, argv, &index, "--subdomain");
            if (value == nullptr) {
                return 2;
            }
            config.subdomain = value;
        } else if (argument == "--token") {
            const char* value = NextArg(argc, argv, &index, "--token");
            if (value == nullptr) {
                return 2;
            }
            config.token = value;
        } else if (argument == "--data-conns") {
            const char* value = NextArg(argc, argv, &index, "--data-conns");
            char* end = nullptr;
            const long parsed = value == nullptr ? -1 : std::strtol(value, &end, 10);
            if (value == nullptr || end == nullptr || *end != '\0' ||
                parsed < 0 || parsed > 64) {
                GO2CPP_LOG_ERROR(GO2CPP_LOG_NAME("tunnel.error"))
                    << "错误: 非法 --data-conns（0..64）";
                return 2;
            }
            config.data_conns = static_cast<int>(parsed);
        } else if (argument == "--max-streams") {
            const char* value = NextArg(argc, argv, &index, "--max-streams");
            if (value == nullptr || !ParseSize(value, &config.max_local_streams)) {
                GO2CPP_LOG_ERROR(GO2CPP_LOG_NAME("tunnel.error"))
                    << "错误: 非法 --max-streams";
                return 2;
            }
        } else if (argument == "--crc") {
            config.with_checksum = true;
        } else if (argument == "--quiet") {
            quiet = true;
        } else {
            GO2CPP_LOG_ERROR(GO2CPP_LOG_NAME("tunnel.error"))
                << "错误: 未知参数 " << argument << "（--help 查看用法）";
            return 2;
        }
    }

    // `--local 8080` 会把 local_host 解析成 "8080"？不会：SplitHostPort 在没有
    // 冒号时把整串当主机。这里补一条兼容：纯数字视为端口。
    {
        bool all_digits = !config.local_host.empty();
        for (char character : config.local_host) {
            if (character < '0' || character > '9') {
                all_digits = false;
                break;
            }
        }
        if (all_digits) {
            std::uint16_t parsed = 0;
            if (ParseU16(config.local_host.c_str(), &parsed)) {
                config.local_port = parsed;
                config.local_host = "127.0.0.1";
            }
        }
    }

    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);
    std::signal(SIGPIPE, SIG_IGN);

    go2cpp::tunnel::TunnelClient client(config);
    if (!quiet) {
        client.set_event_handler([](const std::string& message) {
            GO2CPP_LOG_INFO(GO2CPP_LOG_NAME("tunnel")) << message;
        });
    }

    go2cpp::ErrorPtr error;
    if (!client.Start(&error)) {
        GO2CPP_LOG_ERROR(GO2CPP_LOG_NAME("tunnel.error"))
            << "启动失败: " << go2cpp::ErrorMessage(error);
        return 1;
    }
    GO2CPP_LOG_INFO(GO2CPP_LOG_NAME("tunnel"))
        << "tunnel-client 正在连接 " << config.server_host << ":"
        << static_cast<unsigned>(config.server_port) << " …";

    if (!client.WaitOnline(15000)) {
        GO2CPP_LOG_ERROR(GO2CPP_LOG_NAME("tunnel.error"))
            << "错误: 15 秒内未能上线（检查服务端地址、token、子域名）";
        client.Stop();
        return 1;
    }
    GO2CPP_LOG_INFO(GO2CPP_LOG_NAME("tunnel"))
        << "\n========================================\n"
        << " 公网 URL: " << client.public_url() << "\n"
        << " 本地目标: " << config.local_host << ":"
        << static_cast<unsigned>(config.local_port) << "\n"
        << "========================================\n";

    while (!g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    client.Stop();
    GO2CPP_LOG_INFO(GO2CPP_LOG_NAME("tunnel")) << "tunnel-client 已停止";
    return 0;
}
