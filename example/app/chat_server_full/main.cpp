/**
 * @file main.cpp
 * @brief chat_server_full 入口：--server 启动聊天服务，--client 启动测试客户端。
 * @details 服务端先屏蔽 SIGINT/SIGTERM 再启动 LinkServer 线程，主线程 sigwait
 *          后调用 Stop，保证断开回调与日志收尾完整执行。客户端不屏蔽信号，
 *          Ctrl-C 直接终止即可（它只是测试工具）。
 *
 *          运行（仓库根目录）：
 *            ./build/example/app/go2cpp_chat_server_full --server
 *            ./build/example/app/go2cpp_chat_server_full --client alice
 *            ./go2cpp_chat_server_full --client alice --token tok-1
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include "app_server.h"
#include "client.h"
#include "config.h"
#include "logger.h"

#include <pthread.h>

#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>

namespace {

void PrintUsage() {
    std::cout << "用法:\n"
                 "  go2cpp_chat_server_full --server [--config <ini>] [--host <ip>] [--port <p>]\n"
                 "  go2cpp_chat_server_full --client <name> [--token <token>] [--host <ip>]\n"
                 "                           [--port <p>] [--config <ini>] [--no-join]\n";
}

}  // namespace

int main(int argc, char** argv) {
    bool client_mode = false;
    std::string config_path = "example/app/chat_server_full/server.ini";
    std::string host_override;
    bool has_port_override = false;
    std::uint16_t port_override = 0;
    std::string client_name = "guest";
    std::string client_token;
    bool auto_join = true;

    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--help" || argument == "-h") {
            PrintUsage();
            return 0;
        }
        if (argument == "--server") {
            client_mode = false;
        } else if (argument == "--client" && index + 1 < argc) {
            client_mode = true;
            client_name = argv[++index];
        } else if (argument == "--token" && index + 1 < argc) {
            client_token = argv[++index];
        } else if (argument == "--no-join") {
            auto_join = false;
        } else if (argument == "--config" && index + 1 < argc) {
            config_path = argv[++index];
        } else if (argument == "--host" && index + 1 < argc) {
            host_override = argv[++index];
        } else if (argument == "--port" && index + 1 < argc) {
            const int port = std::atoi(argv[++index]);
            if (port <= 0 || port > 65535) {
                std::cerr << "端口非法: " << argv[index] << "\n";
                return 2;
            }
            port_override = static_cast<std::uint16_t>(port);
            has_port_override = true;
        } else {
            std::cerr << "未知参数: " << argument << "\n";
            PrintUsage();
            return 2;
        }
    }

    chat::Config config = chat::Config::Default();
    std::error_code file_error;
    if (std::filesystem::exists(config_path, file_error)) {
        std::string error;
        if (!config.LoadFromFile(config_path, &error)) {
            std::cerr << "[chat_full] 配置加载失败: " << error << "\n";
            return 1;
        }
    }
    if (!host_override.empty()) config.host = host_override;
    if (has_port_override) config.port = port_override;

    if (client_mode) {
        chat::ChatClient::Options options;
        options.host = config.host;
        options.port = config.port;
        options.name = client_name;
        options.token = client_token;
        options.auto_join = auto_join;
        chat::ChatClient client(options);
        std::string error;
        if (!client.Start(&error)) {
            std::cerr << "[chat_full][client] 启动失败: " << error << "\n";
            return 1;
        }
        std::cout << "[chat_full][client] 已连接 udp://" << options.host << ":" << options.port
                  << "，昵称 " << options.name << "（/quit 退出）\n";
        client.Run();
        return 0;
    }

    std::string log_error;
    if (!chat::InitLogger(config.log_file, config.log_level, &log_error)) {
        std::cerr << "[chat_full] 日志文件不可写，仅输出 stdout: " << log_error << "\n";
    }
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    if (pthread_sigmask(SIG_BLOCK, &signals, nullptr) != 0) {
        std::cerr << "[chat_full] pthread_sigmask 失败\n";
        return 1;
    }

    chat::ChatServer server(config);
    std::string start_error;
    if (!server.Start(&start_error)) {
        std::cerr << "[chat_full] 启动失败: " << start_error << "\n";
        return 1;
    }
    std::cout << "[chat_full] 已启动 udp://" << config.host << ":" << server.port() << "\n";
    int signal_number = 0;
    if (sigwait(&signals, &signal_number) != 0) {
        std::cerr << "[chat_full] sigwait 失败\n";
    }
    std::cout << "[chat_full] 收到信号 " << signal_number << "，开始关闭\n";
    server.Stop();
    std::cout << "[chat_full] 已退出\n";
    return 0;
}
