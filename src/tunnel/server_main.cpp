/**
 * @file server_main.cpp
 * @brief `tunnel-server` 命令行入口（部署在公网 VPS）。
 *
 * @details 用法：
 * ```
 *   ./tunnel-server --public-port 8443 --control-port 7443
 *   ./tunnel-server --public-port 80 --domain tunnel.example.com --token s3cret
 * ```
 *
 * 前台运行，`SIGINT`/`SIGTERM` 优雅停止；事件（隧道上线/下线）经统一日志模块
 * 输出，便于 `systemd` / `docker logs` 收走。
 *
 * @author go2cpp contributors
 * @date 2026-11-02
 * @copyright MIT License
 */

#include "go2cpp/logging/log.hpp"
#include "go2cpp/tunnel/server.hpp"

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
 * @details 迁移前入口直接用 `printf`/`fprintf(stderr)`：信息与错误分属
 *          stdout/stderr 两个流、每行立即可见。为保持同样的可观测语义：
 *          - `tunnel` logger 只挂 `StdoutSink`，承接信息与警告；
 *          - `tunnel.error` logger 只挂 `StderrSink`，承接错误；
 *          - 两者都用 `{message}\n` 模式，输出形态与原先的单行文本一致，
 *            不额外引入时间戳/文件位置噪声。
 *
 * ## 定位
 * 只在本可执行入口调用一次；未初始化时 `Logger` 默认级别是 `Warn` 且不写
 * stdout，信息级输出会被静默丢弃，因此这一步不能省。
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
    // step1: 信息/警告 → stdout，与迁移前的 printf 一致
    auto info_logger = manager.Get("tunnel");
    info_logger->SetLevel(Level::Info);
    info_logger->SetPropagate(false);
    info_logger->ConfigureDefaults(
        {make_worker(Level::Info, std::make_shared<go2cpp::log::StdoutSink>())});
    // step2: 错误 → stderr，与迁移前的 fprintf(stderr) 一致
    auto error_logger = manager.Get("tunnel.error");
    error_logger->SetLevel(Level::Error);
    error_logger->SetPropagate(false);
    error_logger->ConfigureDefaults(
        {make_worker(Level::Error, std::make_shared<go2cpp::log::StderrSink>())});
}

void PrintUsage(const char* program) {
    const std::string text =
        std::string("用法: ") + program +
        " [选项]\n"
        "\n"
        "公网侧隧道服务端：控制端口供 tunnel-client 连入，公网端口供浏览器访问。\n"
        "\n"
        "  --control-port <端口>   客户端控制端口（默认 7443）\n"
        "  --public-port <端口>    浏览器访问端口（默认 8443）\n"
        "  --control-host <地址>   控制端口监听地址（默认 0.0.0.0）\n"
        "  --public-host <地址>    公网端口监听地址（默认 0.0.0.0）\n"
        "  --display-host <主机>   公网 URL 里展示的主机（默认自动探测本机地址）\n"
        "  --domain <后缀>         泛域名后缀，如 tunnel.example.com；\n"
        "                          设置后优先用 <子域名>.<后缀> 路由\n"
        "  --token <令牌>          要求客户端携带同值 token\n"
        "  --no-strip              路径前缀模式下不剥掉 /<子域名> 前缀\n"
        "  --crc                   启用帧尾 CRC-32（两端必须一致）\n"
        "  --max-streams <数量>    单隧道并发流上限（默认 256）\n"
        "  -h, --help              显示本帮助\n"
        "\n"
        "示例:\n"
        "  " +
        program +
        " --public-port 8443 --control-port 7443\n"
        "\n"
        "启动后客户端执行:\n"
        "  tunnel-client --server <vps-ip>:7443 --local 8080 --subdomain shop";
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

/// 取下一个参数；缺失时记录错误并返回 nullptr。
const char* NextArg(int argc, char** argv, int* index, const char* name) {
    if (*index + 1 >= argc) {
        GO2CPP_LOG_ERROR(GO2CPP_LOG_NAME("tunnel.error"))
            << "错误: " << name << " 缺少参数";
        return nullptr;
    }
    *index += 1;
    return argv[*index];
}

}  // namespace

int main(int argc, char** argv) {
    InitLogging();
    go2cpp::tunnel::TunnelServerConfig config;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "-h" || argument == "--help") {
            PrintUsage(argv[0]);
            return 0;
        }
        if (argument == "--control-port") {
            const char* value = NextArg(argc, argv, &index, "--control-port");
            if (value == nullptr || !ParseU16(value, &config.control_port)) {
                GO2CPP_LOG_ERROR(GO2CPP_LOG_NAME("tunnel.error"))
                    << "错误: 非法 --control-port";
                return 2;
            }
        } else if (argument == "--public-port") {
            const char* value = NextArg(argc, argv, &index, "--public-port");
            if (value == nullptr || !ParseU16(value, &config.public_port)) {
                GO2CPP_LOG_ERROR(GO2CPP_LOG_NAME("tunnel.error"))
                    << "错误: 非法 --public-port";
                return 2;
            }
        } else if (argument == "--control-host") {
            const char* value = NextArg(argc, argv, &index, "--control-host");
            if (value == nullptr) {
                return 2;
            }
            config.control_host = value;
        } else if (argument == "--public-host") {
            const char* value = NextArg(argc, argv, &index, "--public-host");
            if (value == nullptr) {
                return 2;
            }
            config.public_host = value;
        } else if (argument == "--display-host") {
            const char* value = NextArg(argc, argv, &index, "--display-host");
            if (value == nullptr) {
                return 2;
            }
            config.display_host = value;
        } else if (argument == "--domain") {
            const char* value = NextArg(argc, argv, &index, "--domain");
            if (value == nullptr) {
                return 2;
            }
            config.domain = value;
        } else if (argument == "--token") {
            const char* value = NextArg(argc, argv, &index, "--token");
            if (value == nullptr) {
                return 2;
            }
            config.token = value;
        } else if (argument == "--no-strip") {
            config.strip_prefix = false;
        } else if (argument == "--crc") {
            config.with_checksum = true;
        } else if (argument == "--max-streams") {
            const char* value = NextArg(argc, argv, &index, "--max-streams");
            if (value == nullptr || !ParseSize(value, &config.max_streams_per_tunnel)) {
                GO2CPP_LOG_ERROR(GO2CPP_LOG_NAME("tunnel.error"))
                    << "错误: 非法 --max-streams";
                return 2;
            }
        } else {
            GO2CPP_LOG_ERROR(GO2CPP_LOG_NAME("tunnel.error"))
                << "错误: 未知参数 " << argument << "（--help 查看用法）";
            return 2;
        }
    }

    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);
    std::signal(SIGPIPE, SIG_IGN);

    go2cpp::tunnel::TunnelServer server(config);
    server.set_event_handler([](const std::string& message) {
        GO2CPP_LOG_INFO(GO2CPP_LOG_NAME("tunnel")) << message;
    });

    go2cpp::ErrorPtr error;
    if (!server.Start(&error)) {
        GO2CPP_LOG_ERROR(GO2CPP_LOG_NAME("tunnel.error"))
            << "启动失败: " << go2cpp::ErrorMessage(error);
        return 1;
    }

    GO2CPP_LOG_INFO(GO2CPP_LOG_NAME("tunnel"))
        << "tunnel-server 已启动\n"
        << "  控制端口   : " << config.control_host << ":"
        << static_cast<unsigned>(server.control_port())
        << "（tunnel-client 连这里）\n"
        << "  公网端口   : " << config.public_host << ":"
        << static_cast<unsigned>(server.public_port())
        << "（浏览器访问这里）\n";
    if (config.domain.empty()) {
        GO2CPP_LOG_INFO(GO2CPP_LOG_NAME("tunnel"))
            << "  公网地址   : http://" << server.display_host() << ":"
            << static_cast<unsigned>(server.public_port()) << "/<子域名>/";
    } else {
        GO2CPP_LOG_INFO(GO2CPP_LOG_NAME("tunnel"))
            << "  公网地址   : http://<子域名>." << config.domain << ":"
            << static_cast<unsigned>(server.public_port()) << "/";
    }
    GO2CPP_LOG_INFO(GO2CPP_LOG_NAME("tunnel"))
        << "  提示       : 客户端执行 tunnel-client --server <本机-ip>:"
        << static_cast<unsigned>(server.control_port())
        << " --local 8080 --subdomain shop";

    while (!g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    GO2CPP_LOG_INFO(GO2CPP_LOG_NAME("tunnel")) << "正在停止…";
    server.Stop();
    GO2CPP_LOG_INFO(GO2CPP_LOG_NAME("tunnel")) << "tunnel-server 已停止";
    return 0;
}
