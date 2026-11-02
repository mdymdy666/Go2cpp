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
 * 前台运行，`SIGINT`/`SIGTERM` 优雅停止；事件（隧道上线/下线）直接打到 stdout，
 * 便于 `systemd` / `docker logs` 收走。
 *
 * @author go2cpp contributors
 * @date 2026-11-02
 * @copyright MIT License
 */

#include "go2cpp/tunnel/server.hpp"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <chrono>
#include <thread>

namespace {

std::atomic<bool> g_stop{false};

void OnSignal(int /*signal_number*/) { g_stop.store(true); }

void PrintUsage(const char* program) {
    std::printf(
        "用法: %s [选项]\n"
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
        "  %s --public-port 8443 --control-port 7443\n"
        "\n"
        "启动后客户端执行:\n"
        "  tunnel-client --server <vps-ip>:7443 --local 8080 --subdomain shop\n",
        program, program);
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

/// 取下一个参数；缺失时打印错误并返回 nullptr。
const char* NextArg(int argc, char** argv, int* index, const char* name) {
    if (*index + 1 >= argc) {
        std::fprintf(stderr, "错误: %s 缺少参数\n", name);
        return nullptr;
    }
    *index += 1;
    return argv[*index];
}

}  // namespace

int main(int argc, char** argv) {
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
                std::fprintf(stderr, "错误: 非法 --control-port\n");
                return 2;
            }
        } else if (argument == "--public-port") {
            const char* value = NextArg(argc, argv, &index, "--public-port");
            if (value == nullptr || !ParseU16(value, &config.public_port)) {
                std::fprintf(stderr, "错误: 非法 --public-port\n");
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
                std::fprintf(stderr, "错误: 非法 --max-streams\n");
                return 2;
            }
        } else {
            std::fprintf(stderr, "错误: 未知参数 %s（--help 查看用法）\n",
                         argument.c_str());
            return 2;
        }
    }

    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);
    std::signal(SIGPIPE, SIG_IGN);

    go2cpp::tunnel::TunnelServer server(config);
    server.set_event_handler([](const std::string& message) {
        std::printf("%s\n", message.c_str());
        std::fflush(stdout);
    });

    go2cpp::ErrorPtr error;
    if (!server.Start(&error)) {
        std::fprintf(stderr, "启动失败: %s\n",
                     go2cpp::ErrorMessage(error).c_str());
        return 1;
    }

    std::printf("tunnel-server 已启动\n");
    std::printf("  控制端口   : %s:%u（tunnel-client 连这里）\n",
                config.control_host.c_str(),
                static_cast<unsigned>(server.control_port()));
    std::printf("  公网端口   : %s:%u（浏览器访问这里）\n",
                config.public_host.c_str(),
                static_cast<unsigned>(server.public_port()));
    if (config.domain.empty()) {
        std::printf("  公网地址   : http://%s:%u/<子域名>/\n",
                    server.display_host().c_str(),
                    static_cast<unsigned>(server.public_port()));
    } else {
        std::printf("  公网地址   : http://<子域名>.%s:%u/\n",
                    config.domain.c_str(),
                    static_cast<unsigned>(server.public_port()));
    }
    std::printf("  提示       : 客户端执行 tunnel-client --server <本机-ip>:%u "
                "--local 8080 --subdomain shop\n",
                static_cast<unsigned>(server.control_port()));
    std::fflush(stdout);

    while (!g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    std::printf("正在停止…\n");
    std::fflush(stdout);
    server.Stop();
    std::printf("tunnel-server 已停止\n");
    return 0;
}
