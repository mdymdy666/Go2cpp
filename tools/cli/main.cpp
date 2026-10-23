/**
 * @file main.cpp
 * @brief go2cpp-cli 入口：命令分发 + 统一错误出口。
 *
 * @details 这里只做三件事：
 *   1. 把 `argc/argv` 转成 `std::vector<std::string>`（后续逻辑不再碰 C 字符串）；
 *   2. 处理全局开关（`--help`/`--version`/`help <command>`）并分发子命令；
 *   3. 捕获 `CliError` 并按约定退出码退出，其它异常统一按 1 处理。
 *
 * ## 为什么在这里忽略 SIGPIPE
 *
 * CLI 会往 socket 与 stdout 写数据。对端提前关闭时，默认的 SIGPIPE 会直接杀掉
 * 进程（退出码 141），使用者只会看到「命令莫名死了」。忽略后，`write` 返回 EPIPE，
 * 由 `transport.cpp` 转成带上下文的错误文案。
 *
 * ## 边界与陷阱
 *
 * - 不打印栈回溯、不写日志文件：这是命令行工具，错误只走 stderr，便于管道处理。
 * - 未知命令不给「最接近的猜测」：静默纠错会让脚本把拼写错误当成成功。
 */

#include "cli.hpp"
#include "commands.hpp"
#include "doctor.hpp"
#include "graph.hpp"
#include "help.hpp"

#include <csignal>
#include <cstdio>
#include <exception>
#include <string>
#include <vector>

namespace {

using go2cpp::cli::CliError;
using go2cpp::cli::ExitCode;

/// `go2cpp-cli help <command>` 的分发；未知命令按用法错误处理。
int PrintCommandHelp(const std::string& command) {
    if (command == "dump") {
        go2cpp::cli::PrintDumpHelp();
        return 0;
    }
    if (command == "status") {
        go2cpp::cli::PrintStatusHelp();
        return 0;
    }
    if (command == "replay") {
        go2cpp::cli::PrintReplayHelp();
        return 0;
    }
    if (command == "migrate") {
        go2cpp::cli::PrintMigrateHelp();
        return 0;
    }
    if (command == "debug") {
        go2cpp::cli::PrintDebugHelp();
        return 0;
    }
    if (command == "doctor") {
        go2cpp::cli::PrintDoctorHelp();
        return 0;
    }
    if (command == "graph") {
        go2cpp::cli::PrintGraphHelp();
        return 0;
    }
    throw CliError(ExitCode::kUsage,
                   "未知命令 '" + command + "'；运行 `go2cpp-cli --help` 查看可用命令");
}

}  // namespace

int main(int argc, char** argv) {
    std::signal(SIGPIPE, SIG_IGN);

    std::vector<std::string> arguments;
    if (argc > 1) {
        arguments.reserve(static_cast<std::size_t>(argc - 1));
        for (int index = 1; index < argc; ++index) {
            arguments.emplace_back(argv[index]);
        }
    }

    try {
        if (arguments.empty()) {
            go2cpp::cli::PrintMainHelp();
            return 0;
        }

        const std::string command = arguments.front();
        const std::vector<std::string> rest(arguments.begin() + 1, arguments.end());

        if (command == "-h" || command == "--help" || command == "help") {
            if (rest.empty()) {
                go2cpp::cli::PrintMainHelp();
                return 0;
            }
            return PrintCommandHelp(rest.front());
        }
        if (command == "-v" || command == "--version") {
            go2cpp::cli::PrintVersion();
            return 0;
        }
        if (command == "dump") {
            return go2cpp::cli::RunDump(rest);
        }
        if (command == "status") {
            return go2cpp::cli::RunStatus(rest);
        }
        if (command == "replay") {
            return go2cpp::cli::RunReplay(rest);
        }
        if (command == "migrate") {
            return go2cpp::cli::RunMigrate(rest);
        }
        if (command == "debug") {
            return go2cpp::cli::RunDebug(rest);
        }
        if (command == "doctor") {
            return go2cpp::cli::RunDoctor(rest);
        }
        if (command == "graph") {
            return go2cpp::cli::RunGraph(rest);
        }

        throw CliError(ExitCode::kUsage,
                       "未知命令 '" + command + "'；运行 `go2cpp-cli --help` 查看可用命令");
    } catch (const CliError& error) {
        std::fprintf(stderr, "go2cpp-cli: %s\n", error.what());
        return static_cast<int>(error.code());
    } catch (const std::exception& error) {
        std::fprintf(stderr, "go2cpp-cli: 内部错误：%s\n", error.what());
        return static_cast<int>(ExitCode::kFailure);
    }
}
