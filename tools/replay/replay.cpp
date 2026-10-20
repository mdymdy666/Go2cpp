/**
 * @file replay.cpp
 * @brief 离线工具：静态分析 / 自动完整重放 / 前缀分片。
 *
 * ```
 * replay <recording.g2cr>                    静态校验，stdout JSON
 * replay info <recording.g2cr>               同上
 * replay play <recording.g2cr> [--until=SEQ] 用 RECORD_FIBER_ENTRY 自动重跑
 * replay split <recording.g2cr> --every-events=N
 * ```
 *
 * **不实现参数解析框架**：手写位置参数 + `--key=value`，保持零依赖。统一
 * CLI 负责子命令/格式化；本工具与库函数是它的调用点。
 *
 * 退出码：
 *   0  记录自洽 / 重放一致 / 按 until 截断成功
 *   1  stuck 或不一致 / 分片失败
 *   2  用法错误或文件格式错误
 *   3  play 时进程内没有注册入口（RECORD_FIBER_ENTRY）
 */

#include "replay/auto_entry.hpp"
#include "replay/replay_engine.hpp"
#include "replay/split.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct Args {
    std::string command{"info"};
    std::string path;
    std::uint64_t until{0};
    std::uint32_t every_events{0};
    std::uint64_t every_ms{0};
    std::string out_dir;
    std::string prefix;
    bool json{false};
};

void Usage(const char* program) {
    std::fprintf(
        stderr,
        "usage:\n"
        "  %s <recording.g2cr>                      static validation (JSON)\n"
        "  %s info <recording.g2cr>                 same as above\n"
        "  %s play <recording.g2cr> [options]        full replay via RECORD_FIBER_ENTRY\n"
        "  %s split <recording.g2cr> [options]       prefix chunking\n"
        "\n"
        "options:\n"
        "  --until=SEQ            stop at seq (play)\n"
        "  --every-events=N       checkpoint every N events (play/split)\n"
        "  --every-ms=M           checkpoint every M virtual ms (play/split)\n"
        "  --out-dir=DIR          split output directory (default: source dir)\n"
        "  --prefix=NAME          split file prefix (default: source stem)\n"
        "  --json                 keep timeline in play JSON output\n",
        program, program, program, program);
}

bool StartsWith(const std::string& text, const char* prefix) {
    return text.rfind(prefix, 0) == 0;
}

bool ParseU64(const std::string& text, std::uint64_t* out) {
    if (text.empty()) {
        return false;
    }
    errno = 0;
    char* end = nullptr;
    const unsigned long long value = std::strtoull(text.c_str(), &end, 10);
    if (errno != 0 || end == text.c_str() || *end != '\0') {
        return false;
    }
    *out = static_cast<std::uint64_t>(value);
    return true;
}

std::uint32_t EnvU32(const char* name, std::uint32_t fallback) {
    const char* raw = std::getenv(name);
    if (raw == nullptr || *raw == '\0') {
        return fallback;
    }
    errno = 0;
    char* end = nullptr;
    const unsigned long value = std::strtoul(raw, &end, 10);
    if (errno != 0 || end == raw || *end != '\0' || value > 0xFFFFFFFFUL) {
        std::fprintf(stderr, "[replay] bad %s value: %s\n", name, raw);
        return fallback;
    }
    return static_cast<std::uint32_t>(value);
}

bool ParseOptions(int argc, char** argv, int start, Args* args) {
    for (int index = start; index < argc; ++index) {
        const std::string option = argv[index];
        std::uint64_t number = 0;
        if (StartsWith(option, "--until=")) {
            if (!ParseU64(option.substr(8), &number)) {
                std::fprintf(stderr, "[replay] bad --until value\n");
                return false;
            }
            args->until = number;
        } else if (StartsWith(option, "--every-events=")) {
            if (!ParseU64(option.substr(15), &number) || number == 0 ||
                number > 0xFFFFFFFFULL) {
                std::fprintf(stderr, "[replay] bad --every-events value\n");
                return false;
            }
            args->every_events = static_cast<std::uint32_t>(number);
        } else if (StartsWith(option, "--every-ms=")) {
            if (!ParseU64(option.substr(11), &number)) {
                std::fprintf(stderr, "[replay] bad --every-ms value\n");
                return false;
            }
            args->every_ms = number;
        } else if (StartsWith(option, "--out-dir=")) {
            args->out_dir = option.substr(10);
        } else if (StartsWith(option, "--prefix=")) {
            args->prefix = option.substr(9);
        } else if (option == "--json") {
            args->json = true;
        } else {
            std::fprintf(stderr, "[replay] unknown option: %s\n", option.c_str());
            return false;
        }
    }
    return true;
}

void WriteJson(const go2cpp::replay::ReplayReport& report) {
    const std::string json = go2cpp::replay::ReplayReportJson(report);
    std::fwrite(json.data(), 1, json.size(), stdout);
}

int RunInfo(const Args& args) {
    go2cpp::replay::ReplayOptions options;
    options.keep_timeline = true;
    options.stop_at_seq = args.until;
    options.checkpoint_every_events = args.every_events;
    options.checkpoint_every_ns = args.every_ms * 1000000ULL;
    const auto report = go2cpp::replay::ReplayFile(args.path, options);
    WriteJson(report);
    if (report.status == go2cpp::replay::ReplayStatus::kFormatError) {
        return 2;
    }
    return report.status == go2cpp::replay::ReplayStatus::kOk ||
                   report.status == go2cpp::replay::ReplayStatus::kPartial
               ? 0
               : 1;
}

int RunPlay(const Args& args) {
    go2cpp::replay::ReplayOptions options;
    options.keep_timeline = args.json;
    options.stop_at_seq = args.until;
    options.checkpoint_every_events = args.every_events;
    options.checkpoint_every_ns = args.every_ms * 1000000ULL;
    // 故障注入：与 demo 入口保持一致，读取 GO2CPP_REPLAY_DROP_WAKE。
    // CLI（go2cpp-cli replay play）通过 fork/exec 启动本工具，环境变量天然
    // 透传（包括其它 GO2CPP_REPLAY_*），因此只需在这里解析即可让 CLI 复现
    // stuck；此前不读取导致 CLI 路径的丢唤醒注入被静默忽略（D4）。
    options.drop_wake_at = EnvU32("GO2CPP_REPLAY_DROP_WAKE", 0);
    const auto report = go2cpp::replay::ReplayFileAuto(args.path, options);
    WriteJson(report);
    std::fprintf(stderr, "%s", report.Summary().c_str());
    if (!report.replay_executed) {
        std::fprintf(stderr,
                     "[replay] play: %s\n"
                     "[replay] hint: link the program's RECORD_FIBER_ENTRY into this "
                     "process, or call ReplayFileAuto() from it\n",
                     report.error.c_str());
        return 3;
    }
    switch (report.status) {
        case go2cpp::replay::ReplayStatus::kOk:
        case go2cpp::replay::ReplayStatus::kPartial:
            return 0;
        case go2cpp::replay::ReplayStatus::kNotRun:
            return 3;
        case go2cpp::replay::ReplayStatus::kFormatError:
            return 2;
        default:
            return 1;
    }
}

int RunSplit(const Args& args) {
    go2cpp::replay::SplitOptions options;
    options.out_dir = args.out_dir;
    options.prefix = args.prefix;
    options.every_events = args.every_events;
    options.every_ns = args.every_ms * 1000000ULL;
    const auto result = go2cpp::replay::SplitRecording(args.path, options);
    std::string json = "{\"ok\":";
    json += result.ok ? "true" : "false";
    json += ",\"source_events\":" + std::to_string(result.source_events);
    if (!result.error.empty()) {
        json += ",\"error\":\"" + result.error + "\"";
    }
    json += ",\"parts\":[";
    for (std::size_t index = 0; index < result.parts.size(); ++index) {
        const auto& part = result.parts[index];
        if (index != 0) {
            json += ",";
        }
        json += "{\"path\":\"" + part.path + "\"";
        json += ",\"first_seq\":" + std::to_string(part.first_seq);
        json += ",\"last_seq\":" + std::to_string(part.last_seq);
        json += ",\"events\":" + std::to_string(part.events);
        json += ",\"vtime_ns\":" + std::to_string(part.vtime_ns) + "}";
    }
    json += "]}\n";
    std::fwrite(json.data(), 1, json.size(), stdout);
    if (!result.ok) {
        std::fprintf(stderr, "[replay] split failed: %s\n", result.error.c_str());
        return 1;
    }
    for (const auto& part : result.parts) {
        std::fprintf(stderr, "[replay] %s (events=%llu last_seq=%llu)\n",
                     part.path.c_str(),
                     static_cast<unsigned long long>(part.events),
                     static_cast<unsigned long long>(part.last_seq));
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        Usage(argv[0]);
        return 2;
    }
    Args args;
    const std::string first = argv[1];
    int option_start = 2;
    if (first == "info" || first == "play" || first == "split") {
        if (argc < 3) {
            Usage(argv[0]);
            return 2;
        }
        args.command = first;
        args.path = argv[2];
        option_start = 3;
    } else if (first == "--help" || first == "-h") {
        Usage(argv[0]);
        return 0;
    } else {
        args.command = "info";
        args.path = first;
        option_start = 2;
    }
    if (!ParseOptions(argc, argv, option_start, &args)) {
        Usage(argv[0]);
        return 2;
    }
    if (args.command == "play") {
        return RunPlay(args);
    }
    if (args.command == "split") {
        return RunSplit(args);
    }
    return RunInfo(args);
}
