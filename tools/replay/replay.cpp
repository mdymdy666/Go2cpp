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

#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
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
    std::uint32_t gid{0};
    bool has_gid{false};
    bool json{false};
};

void Usage(const char* program) {
    std::fprintf(
        stderr,
        "usage:\n"
        "  %s <recording.g2cr>                      static validation (JSON)\n"
        "  %s info <recording.g2cr>                 same as above\n"
"  %s play <recording.g2cr> [options]        full replay via RECORD_FIBER_ENTRY\n"
"  %s step <recording.g2cr> [options]        interactive single-step viewer\n"
"  %s split <recording.g2cr> [options]       prefix chunking\n"
        "\n"
        "options:\n"
        "  --until=SEQ            stop at seq (play/step)\n"
"  --gid=ID               only show this gid (step)\n"
        "  --every-events=N       checkpoint every N events (play/split)\n"
        "  --every-ms=M           checkpoint every M virtual ms (play/split)\n"
        "  --out-dir=DIR          split output directory (default: source dir)\n"
        "  --prefix=NAME          split file prefix (default: source stem)\n"
        "  --json                 keep timeline in play JSON output\n",
        program, program, program, program, program);
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
        } else if (StartsWith(option, "--gid=")) {
            if (!ParseU64(option.substr(6), &number) ||
                number > 0xFFFFFFFFULL) {
                std::fprintf(stderr, "[replay] bad --gid value\n");
                return false;
            }
            args->gid = static_cast<std::uint32_t>(number);
            args->has_gid = true;
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


namespace {

std::string TrimText(const std::string& text) {
    std::size_t begin = 0;
    while (begin < text.size() &&
           std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
        ++begin;
    }
    std::size_t end = text.size();
    while (end > begin &&
           std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
        --end;
    }
    return text.substr(begin, end - begin);
}

std::string ChannelText(std::uint32_t channel) {
    if (channel == go2cpp::replay::kNoChannel) {
        return "-";
    }
    return std::to_string(channel);
}

/// 打印一条事件详情（`replay step` 的单步输出）。
void PrintEventLine(const go2cpp::replay::Event& event, std::size_t index) {
    using namespace go2cpp::replay;
    std::printf("#%zu seq=%llu type=%s gid=%u channel=%s vtime=%lluns",
                index, static_cast<unsigned long long>(event.seq),
                EventTypeName(event.type), event.gid,
                ChannelText(event.channel).c_str(),
                static_cast<unsigned long long>(event.vtime_ns));
    if (event.flags != 0U) {
        std::printf(" flags=0x%x", event.flags);
    }
    switch (event.type) {
        case EventType::kMeta:
            std::printf(" gids=%u channels=%u value_kind=%s demo=\"%s\" clock=%u",
                        event.gid, event.channel,
                        ValueKindName(static_cast<ValueKind>(event.value)),
                        event.note.c_str(), event.clock_mode);
            break;
        case EventType::kSpawn:
            break;
        case EventType::kExit:
            std::printf(" sent=%lld recv=%lld",
                        static_cast<long long>(event.value),
                        static_cast<long long>(event.aux64));
            break;
        case EventType::kSend:
        case EventType::kRecv:
            std::printf(" op=%u decision=%s blocked=%u value=%lld data_len=%zu",
                        event.op_index, DecisionName(event.decision),
                        event.blocked, static_cast<long long>(event.value),
                        event.data.size());
            break;
        case EventType::kSelect:
            std::printf(" op=%u decision=%s blocked=%u chosen=%u value=%lld cases=%zu",
                        event.op_index, DecisionName(event.decision),
                        event.blocked, event.chosen,
                        static_cast<long long>(event.value),
                        event.cases.size());
            break;
        case EventType::kClose:
            std::printf(" op=%u decision=%s", event.op_index,
                        DecisionName(event.decision));
            break;
        case EventType::kTimer:
            std::printf(" timer=%s op=%u timer_id=%u timer_ns=%llu deadline=%llu "
                        "fired_at=%llu source=%u result=%lld",
                        TimerKindName(static_cast<TimerKind>(event.timer_kind)),
                        event.op_index, event.timer_id,
                        static_cast<unsigned long long>(event.timer_ns),
                        static_cast<unsigned long long>(event.deadline_ns),
                        static_cast<unsigned long long>(event.fired_at_ns),
                        event.timer_source,
                        static_cast<long long>(event.callback_result));
            break;
        case EventType::kCtx:
            std::printf(" ctx_id=%u ctx=%s op=%u", event.ctx_id,
                        ContextEventKindName(
                            static_cast<ContextEventKind>(event.ctx_kind)),
                        event.op_index);
            break;
        case EventType::kPark:
        case EventType::kWake:
            std::printf(" op=%u reason=%s", event.op_index,
                        ParkReasonName(event.reason));
            break;
        case EventType::kSched:
            std::printf(" sched_type=%u machine=%u processor=%u", event.sched_type,
                        event.machine, event.processor);
            break;
        case EventType::kNote:
            std::printf(" note=\"%s\"", event.note.c_str());
            break;
        case EventType::kResMutexLock:
        case EventType::kResMutexUnlock:
        case EventType::kResPCWait:
        case EventType::kResPCNotify:
        case EventType::kResChannelSend:
        case EventType::kResChannelRecv:
            std::printf(" resource=%llu actor=%llu target=%llu result=%u "
                        "address=0x%llx waiters=%u",
                        static_cast<unsigned long long>(event.resource_id),
                        static_cast<unsigned long long>(event.actor_gid),
                        static_cast<unsigned long long>(event.target_gid),
                        event.resource_result,
                        static_cast<unsigned long long>(event.address),
                        event.waiters);
            break;
        case EventType::kEnd:
            std::printf(" records=%lld dropped=%llu file_crc=0x%08x",
                        static_cast<long long>(event.value),
                        static_cast<unsigned long long>(event.aux64),
                        event.sched_type);
            break;
        default:
            break;
    }
    std::printf("\n");
}

/// 交互式单步：`replay step <recording.g2cr>`。
int RunStep(const Args& args) {
    using namespace go2cpp::replay;
    std::vector<Event> events;
    std::string error;
    if (!LoadRecordingEvents(args.path, &events, &error)) {
        std::fprintf(stderr, "[replay] step: %s\n", error.c_str());
        return 2;
    }
    if (events.empty()) {
        std::fprintf(stderr, "[replay] step: recording contains no events\n");
        return 2;
    }

    std::size_t index = 0;  ///< 下一条待推进的事件
    bool filter_active = args.has_gid;
    std::uint32_t filter_gid = args.gid;
    std::uint64_t last_processed_seq = 0;
    bool any_processed = false;

    const auto print_if_visible = [&](const Event& event, std::size_t at) {
        if (filter_active && event.gid != filter_gid) {
            return;
        }
        PrintEventLine(event, at);
    };
    /// 推进一个事件；返回 false 表示已到结尾。
    const auto advance_one = [&]() -> bool {
        if (index >= events.size()) {
            return false;
        }
        const Event& event = events[index];
        print_if_visible(event, index);
        last_processed_seq = event.seq;
        any_processed = true;
        ++index;
        return true;
    };
    const auto advance_until = [&](std::uint64_t target_seq) -> std::size_t {
        std::size_t processed = 0;
        while (index < events.size() && events[index].seq <= target_seq) {
            advance_one();
            ++processed;
        }
        return processed;
    };
    const auto report_position = [&]() {
        std::printf("position=%zu/%zu", index, events.size());
        if (index < events.size()) {
            std::printf(" next_seq=%llu next_type=%s",
                        static_cast<unsigned long long>(events[index].seq),
                        EventTypeName(events[index].type));
        } else {
            std::printf(" next_seq=- next_type=end");
        }
        std::printf(" remaining=%zu filter_gid=%s\n", events.size() - index,
                    filter_active ? std::to_string(filter_gid).c_str() : "all");
    };

    std::printf("loaded %zu events from %s\n", events.size(), args.path.c_str());
    std::size_t resource_events = 0;
    for (const auto& event : events) {
        if (event.type == EventType::kResMutexLock ||
            event.type == EventType::kResMutexUnlock ||
            event.type == EventType::kResPCWait ||
            event.type == EventType::kResPCNotify ||
            event.type == EventType::kResChannelSend ||
            event.type == EventType::kResChannelRecv) {
            ++resource_events;
        }
    }
    std::printf("resource_events=%zu filter_gid=%s\n", resource_events,
                filter_active ? std::to_string(filter_gid).c_str() : "all");
    std::printf("commands: step | next <n> | until <seq> | gid <id|all> | "
                "where | help | quit\n");

    if (args.until != 0U) {
        const std::size_t processed = advance_until(args.until);
        std::printf("until %llu: processed=%zu", 
                    static_cast<unsigned long long>(args.until), processed);
        if (any_processed) {
            std::printf(" last_seq=%llu",
                        static_cast<unsigned long long>(last_processed_seq));
        }
        std::printf("\n");
    }

    std::string line;
    while (true) {
        if (::isatty(STDIN_FILENO) != 0) {
            std::printf("> ");
            std::fflush(stdout);
        }
        if (!std::getline(std::cin, line)) {
            break;
        }
        line = TrimText(line);
        if (line.empty()) {
            continue;
        }
        std::string command = line;
        std::string argument;
        const std::size_t space = line.find_first_of(" \t");
        if (space != std::string::npos) {
            command = line.substr(0, space);
            argument = TrimText(line.substr(space + 1));
        }

        if (command == "quit" || command == "q" || command == "exit") {
            break;
        }
        if (command == "help" || command == "h" || command == "?") {
            std::printf("  step          前进一个事件并打印详情\n"
                        "  next <n>      前进 n 个事件\n"
                        "  until <seq>   前进到 seq 为止（含）\n"
                        "  gid <id|all>  只显示该 gid 的事件\n"
                        "  where         当前位置/剩余\n"
                        "  quit          退出\n");
            continue;
        }
        if (command == "step") {
            if (!advance_one()) {
                std::printf("end of recording (%zu events)\n", events.size());
            }
            continue;
        }
        if (command == "next") {
            std::uint64_t count = 0;
            if (!ParseU64(argument, &count) || count == 0U) {
                std::printf("next: expected a positive count\n");
                continue;
            }
            std::size_t processed = 0;
            while (processed < count && advance_one()) {
                ++processed;
            }
            if (processed < count) {
                std::printf("end of recording (%zu events)\n", events.size());
            }
            continue;
        }
        if (command == "until") {
            std::uint64_t target = 0;
            if (!ParseU64(argument, &target)) {
                std::printf("until: expected a seq number\n");
                continue;
            }
            const std::size_t processed = advance_until(target);
            std::printf("until %llu: processed=%zu",
                        static_cast<unsigned long long>(target), processed);
            if (any_processed) {
                std::printf(" last_seq=%llu",
                            static_cast<unsigned long long>(last_processed_seq));
            }
            if (index < events.size() && events[index].seq > target) {
                std::printf(" stopped_before_seq=%llu",
                            static_cast<unsigned long long>(events[index].seq));
            }
            std::printf("\n");
            continue;
        }
        if (command == "gid") {
            if (argument == "all" || argument == "none" || argument.empty()) {
                filter_active = false;
                std::printf("gid filter cleared\n");
                continue;
            }
            std::uint64_t gid = 0;
            if (!ParseU64(argument, &gid) || gid > 0xFFFFFFFFULL) {
                std::printf("gid: expected <id> or all\n");
                continue;
            }
            filter_active = true;
            filter_gid = static_cast<std::uint32_t>(gid);
            std::printf("gid filter = %u\n", filter_gid);
            continue;
        }
        if (command == "where") {
            report_position();
            continue;
        }
        std::printf("unknown command: %s (try: help)\n", command.c_str());
    }
    return 0;
}

}  // namespace

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
    if (first == "info" || first == "play" || first == "step" ||
        first == "split") {
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
    if (args.command == "step") {
        return RunStep(args);
    }
    if (args.command == "split") {
        return RunSplit(args);
    }
    return RunInfo(args);
}
