#pragma once

/**
 * @file debug_cli.hpp
 * @brief `dump scheduler` 的命令行解析与输出（P1）。
 *
 * **形态说明（重要，不要误解）**：
 *   本文件提供的是**进程内** CLI —— 调用方把自己的 `Scheduler&` 交给它，
 *   它负责解析参数、采集快照、序列化并写出。
 *   真正的「跨进程 dump 另一个正在运行的进程」需要**信号或注入机制**
 *   （例如注册 SIGUSR1 处理函数把 dump 写到文件），那属于后续阶段，
 *   **本文件不声称具备该能力**。
 *
 * 命令行形状（对齐使用者给定的形式）：
 *     Go2Cpp dump scheduler [--options] ... [scheduler_name]
 *   其中 `scheduler_name` 当前**仅作标识显示**：项目没有"调度器名字"这一概念
 *   （`SchedulerConfig` 无 name 字段），因此名称由调用方通过 `DumpOptions::name`
 *   传入，缺省为 `main`。这一点在设计文档里也标注过。
 */

#include "go2cpp/debug/debug_events.hpp"
#include "go2cpp/debug/debug_scheduler.hpp"

#include <cstdio>
#include <string>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif

namespace go2cpp::debug {

/// dump 的目标对象。
enum class DumpTarget {
    kScheduler,  ///< dump scheduler（默认，保持向后兼容）。
    kEvents,     ///< dump events（需要调用方提供 DebugEventCollector）。
};

/// 解析后的 dump 命令。
struct DumpCommand {
    bool valid{false};
    DumpTarget target{DumpTarget::kScheduler};
    DumpFormat format{DumpFormat::kText};
    std::string output;      ///< 空或 "-" 表示 stdout。
    bool append{false};      ///< 追加而非覆盖（仅对文件输出有效）。
    std::string name{"main"};///< scheduler_name（仅作标识）。
    DumpOptions options;
    std::string error;       ///< 解析失败原因（valid=false 时有效）。
};

/// 解析形如 ["dump","scheduler","--format=json","--output=f"] 的参数列表。
/// @note 只解析，不产生任何副作用；失败时 `valid=false` 且 `error` 说明原因。
inline DumpCommand ParseDumpCommand(const std::vector<std::string>& args) {
    DumpCommand command;
    std::size_t index = 0;
    if (index < args.size() && args[index] == "dump") {
        ++index;
    }
    if (index < args.size() && args[index] == "scheduler") {
        command.target = DumpTarget::kScheduler;
        ++index;
    } else if (index < args.size() && args[index] == "events") {
        command.target = DumpTarget::kEvents;
        ++index;
    } else if (index < args.size() && !args[index].empty() && args[index][0] != '-') {
        // 位置参数出现在这里说明它是目标名而不是 scheduler_name。
        // 此时若它不是合法目标，必须报错，**不能**静默当成 scheduler。
        command.error = "unknown target: " + args[index];
        return command;
    }
    for (; index < args.size(); ++index) {
        const std::string& arg = args[index];
        if (arg == "--format=text") {
            command.format = DumpFormat::kText;
        } else if (arg == "--format=ini") {
            command.format = DumpFormat::kIni;
        } else if (arg == "--format=json") {
            command.format = DumpFormat::kJson;
        } else if (arg.rfind("--format=", 0) == 0) {
            command.error = "unknown format: " + arg.substr(9);
            return command;
        } else if (arg.rfind("--output=", 0) == 0) {
            command.output = arg.substr(9);
        } else if (arg == "-o" && index + 1 < args.size()) {
            command.output = args[++index];
        } else if (arg == "--append") {
            command.append = true;
        } else if (arg == "--truncate") {
            command.append = false;
        } else if (arg == "--summary") {
            command.options.include_processors = false;
            command.options.include_machines = false;
        } else if (arg == "--processors") {
            command.options.include_processors = true;
            command.options.include_machines = false;
            command.options.include_metrics = false;
        } else if (arg == "--machines") {
            command.options.include_processors = false;
            command.options.include_machines = true;
            command.options.include_metrics = false;
        } else if (arg == "--metrics") {
            command.options.include_processors = false;
            command.options.include_machines = false;
            command.options.include_metrics = true;
        } else if (arg == "--no-processors") {
            command.options.include_processors = false;
        } else if (arg == "--no-machines") {
            command.options.include_machines = false;
        } else if (arg == "--no-metrics") {
            command.options.include_metrics = false;
        } else if (arg == "--help" || arg == "-h") {
            command.error = "usage: dump {scheduler|events} [--format=text|ini|json] "
                            "[--output=<file>|-] [--append|--truncate] "
                            "[--summary|--processors|--machines|--metrics] "
                            "[--no-processors] [--no-machines] [--no-metrics] [name]";
            return command;
        } else if (!arg.empty() && arg[0] == '-') {
            command.error = "unknown option: " + arg;
            return command;
        } else {
            command.name = arg;  // 位置参数：scheduler_name
        }
    }
    command.valid = true;
    return command;
}

/// 生成默认文件名：含 pid 与墙上时间，避免多进程互相覆盖。
inline std::string DefaultDumpFileName() {
    char buffer[128];
    const auto now = std::chrono::system_clock::now();
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
                             now.time_since_epoch()).count();
#if defined(__unix__) || defined(__APPLE__)
    const long pid = static_cast<long>(::getpid());
#else
    const long pid = 0;
#endif
    std::snprintf(buffer, sizeof(buffer), "go2cpp-dump-%ld-%lld.txt", pid,
                  static_cast<long long>(seconds));
    return std::string(buffer);
}

/**
 * @brief 原子写入文本文件（临时文件 + rename）。
 *
 * 依赖：POSIX rename 的原子性（同目录内）。
 * 对上层提供：要么完整写入，要么不产生目标文件 —— 避免"半截 dump"。
 * @note `append=true` 时退化为**直接追加**（不保证原子），因为追加本身无法原子完成；
 *       这一点在文档中明确说明，不隐瞒。
 * @return 成功返回 true；失败时 `error` 填原因。
 */
inline bool WriteDumpFile(const std::string& path, const std::string& text,
                          bool append, std::string* error) {
    if (append) {
        std::FILE* file = std::fopen(path.c_str(), "ab");
        if (file == nullptr) {
            if (error) *error = "cannot open for append: " + path;
            return false;
        }
        const std::size_t written = std::fwrite(text.data(), 1, text.size(), file);
        std::fclose(file);
        if (written != text.size()) {
            if (error) *error = "short write: " + path;
            return false;
        }
        return true;
    }
    const std::string temporary = path + ".tmp";
    std::FILE* file = std::fopen(temporary.c_str(), "wb");
    if (file == nullptr) {
        if (error) *error = "cannot open temp file: " + temporary;
        return false;
    }
    const std::size_t written = std::fwrite(text.data(), 1, text.size(), file);
    std::fclose(file);
    if (written != text.size()) {
        std::remove(temporary.c_str());
        if (error) *error = "short write: " + temporary;
        return false;
    }
    if (std::rename(temporary.c_str(), path.c_str()) != 0) {
        std::remove(temporary.c_str());
        if (error) *error = "rename failed: " + path;
        return false;
    }
    return true;
}

/// 执行一次 events dump：序列化事件缓冲并写到 stdout 或文件。
/// @note 使用只读快照（Peek），**不会消耗**采集器里的事件。
inline bool RunEventsDump(const DumpCommand& command, DebugEventCollector& collector,
                          std::string* text_out, std::string* error) {
    if (!command.valid) {
        if (error) *error = command.error.empty() ? "invalid command" : command.error;
        return false;
    }
    std::string text;
    switch (command.format) {
        case DumpFormat::kIni:
            text = collector.DumpIni(command.options.event_limit);
            break;
        case DumpFormat::kJson:
            text = collector.DumpJson(command.options.event_limit);
            break;
        case DumpFormat::kText:
            text = collector.DumpText(command.options.event_limit);
            break;
    }
    if (text_out != nullptr) {
        *text_out = text;
    }
    if (command.output.empty() || command.output == "-") {
        const std::size_t written = std::fwrite(text.data(), 1, text.size(), stdout);
        if (written != text.size()) {
            if (error) *error = "short write to stdout";
            return false;
        }
        return true;
    }
    return WriteDumpFile(command.output, text, command.append, error);
}

/// 执行一次 dump：序列化并写到 stdout 或文件。
/// @return 成功返回 true；失败时 `error` 填原因。
inline bool RunDump(const DumpCommand& command, Scheduler& scheduler,
                    std::string* text_out, std::string* error) {
    if (!command.valid) {
        if (error) *error = command.error.empty() ? "invalid command" : command.error;
        return false;
    }
    DebugScheduler proxy(scheduler);
    const std::string text = proxy.Dump(command.format, command.options);
    if (text_out != nullptr) {
        *text_out = text;
    }
    if (command.output.empty() || command.output == "-") {
        const std::size_t written = std::fwrite(text.data(), 1, text.size(), stdout);
        if (written != text.size()) {
            if (error) *error = "short write to stdout";
            return false;
        }
        return true;
    }
    return WriteDumpFile(command.output, text, command.append, error);
}

/// 统一入口：按 command.target 分派到 scheduler 或 events。
inline bool RunDump(const DumpCommand& command, Scheduler& scheduler,
                    DebugEventCollector& collector, std::string* text_out,
                    std::string* error) {
    if (command.target == DumpTarget::kEvents) {
        return RunEventsDump(command, collector, text_out, error);
    }
    return RunDump(command, scheduler, text_out, error);
}

/// 便捷入口：直接吃 argc/argv。
/// @return 0 表示成功；1 表示参数错误；2 表示写出失败。
inline int RunDumpCli(int argc, const char* const* argv, Scheduler& scheduler,
                      std::string* error) {
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        // 防御：调用方把 argc 写得比实际数组长时，argv[i] 可能越界读到 nullptr，
        // 直接 emplace_back 会抛 basic_string: construction from null 并终止进程。
        // 这里遇到 nullptr 就停止解析，返回参数错误而不是崩溃。
        if (argv == nullptr || argv[i] == nullptr) {
            if (error) *error = "null argument in argv";
            return 1;
        }
        args.emplace_back(argv[i]);
    }
    const auto command = ParseDumpCommand(args);
    if (!command.valid) {
        if (error) *error = command.error;
        return 1;
    }
    std::string local_error;
    if (!RunDump(command, scheduler, nullptr, &local_error)) {
        if (error) *error = local_error;
        return 2;
    }
    return 0;
}

}  // namespace go2cpp::debug
