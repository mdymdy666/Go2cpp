/**
 * @file help.cpp
 * @brief 帮助文本实现（中文）。
 *
 * @details 每份帮助都包含：用法、说明、选项、示例、待对齐。协议版本与后端
 *          可执行文件名直接写在帮助里，避免使用者去翻文档才能知道
 *          「为什么连不上 / 该把哪个二进制放到 PATH」。
 */

#include "help.hpp"

#include "cli.hpp"

#include <cstdio>

namespace go2cpp::cli {
namespace {

void Print(const char* text) {
    std::fputs(text, stdout);
}

}  // namespace

void PrintVersion() {
    std::printf("go2cpp-cli %s\n", kVersion);
}

void PrintMainHelp() {
    Print(
        "go2cpp-cli — Go2Cpp 运行时统一命令行入口\n"
        "\n"
        "用法:\n"
        "  go2cpp-cli <command> [options]\n"
        "  go2cpp-cli <command> --help\n"
        "\n"
        "命令:\n"
        "  dump      从运行中的进程抓取协程 / 调度 / 锁 dump\n"
        "  status    读取运行中进程的运行时统计\n"
        "  replay    记录或离线重放一次执行（子命令 record / play）\n"
        "  migrate   导出或恢复可迁移协程（子命令 dump / restore）\n"
        "  debug     离线分析 dump / trace / 二进制记录（统计与时间线）\n"
        "  doctor    在线诊断 + 受控唤醒（G2CD/1 帧，单线程命令平面）\n"
        "\n"
        "全局选项:\n"
        "  -h, --help     显示帮助（可放在任意子命令之后）\n"
        "  -v, --version  显示版本\n"
        "\n"
        "示例:\n"
        "  go2cpp-cli status --pid 1234\n"
        "  go2cpp-cli dump --pid 1234 --out /tmp/dump.json\n"
        "  go2cpp-cli replay play /tmp/run.g2cr\n"
        "  go2cpp-cli migrate restore /tmp/fiber.mig\n"
        "  go2cpp-cli debug /tmp/dump.json --top 20\n"
        "  go2cpp-cli doctor --pid 1234 status\n"
        "\n"
        "退出码:\n"
        "  0 成功   1 一般失败   2 用法错误   3 后端不可用\n"
        "  4 文件格式不匹配   5 目标进程返回失败\n"
        "\n"
        "环境变量:\n"
        "  GO2CPP_DEBUG_SOCKET  覆盖 DebugServer socket 路径（默认 /tmp/go2cpp-<pid>.sock）\n"
        "  GO2CPP_CLI_TRACE=1   把协议请求/响应打到 stderr\n"
        "  GO2CPP_REPLAY_TOOL   指定 replay play 的后端（tools/replay 的 replay）\n"
        "  GO2CPP_MIGRATE_TOOL  指定 migrate 的后端（tools/migration 的 migrate）\n"
        "\n"
        "文档: docs/cli.md\n");
}

void PrintDumpHelp() {
    Print(
        "用法: go2cpp-cli dump --pid <pid> [options]\n"
        "\n"
        "通过 DebugServer（协议 GO2CPP-DEBUG/1）读取 dump。\n"
        "默认发 `JSON ALL`；--format text 发 `DUMP ALL`。\n"
        "指定 --sections 时改为逐组件请求（JSON <component> / DUMP <component>），\n"
        "目标进程未注册该组件会明确报错，不会静默省略。\n"
        "\n"
        "选项:\n"
        "  --pid <PID>          必需。目标进程 pid。\n"
        "  --out <FILE>         写入文件；默认 -（stdout）。写入使用临时文件 + rename。\n"
        "  --sections <LIST>    all（默认）| goroutines | scheduler | locks | 组件名。\n"
        "                       goroutines → scheduler+fiber；locks → mutex+channel+\n"
        "                       parking_condition；也可直接写组件名（runtime/events/io 等）。\n"
        "  --format <FMT>       json（默认）或 text。\n"
        "  --timeout-ms <N>     连接 + 发送 + 等待响应的总超时，默认 5000。\n"
        "  -h, --help           显示本帮助。\n"
        "\n"
        "示例:\n"
        "  go2cpp-cli dump --pid 1234\n"
        "  go2cpp-cli dump --pid 1234 --sections goroutines,locks --out /tmp/dump.json\n"
        "  go2cpp-cli dump --pid 1234 --sections runtime --format text\n"
        "\n"
        "前提:\n"
        "  目标进程需要以 GO2CPP_DEBUG_DUMP=ON 构建，并在启动后调用\n"
        "  go2cpp::debug::DebugServer::Instance().Start()。\n");
}

void PrintStatusHelp() {
    Print(
        "用法: go2cpp-cli status --pid <pid> [options]\n"
        "\n"
        "读取运行时统计：先发 STATUS（DebugServer 基本信息），再发 `JSON scheduler`\n"
        "（协程/任务、runqueue、调度指标）。目标进程未注册 scheduler 组件时，\n"
        "仍会显示 STATUS 信息并明确说明缺哪个组件。\n"
        "\n"
        "选项:\n"
        "  --pid <PID>          必需。目标进程 pid。\n"
        "  --json               输出 {status: {...}, scheduler: {...}|null} 的 JSON。\n"
        "  --timeout-ms <N>     总超时，默认 5000。\n"
        "  -h, --help           显示本帮助。\n"
        "\n"
        "示例:\n"
        "  go2cpp-cli status --pid 1234\n"
        "  go2cpp-cli status --pid 1234 --json\n"
        "\n"
        "说明:\n"
        "  DebugServer 目前不提供内存统计；输出会如实标注，而不是显示 0。\n");
}

void PrintReplayHelp() {
    Print(
        "用法:\n"
        "  go2cpp-cli replay record <exe> [args...] --out <file>\n"
        "  go2cpp-cli replay play <file>\n"
        "\n"
        "record:\n"
        "  设置 GO2CPP_REPLAY=ON 与 GO2CPP_REPLAY_RECORD=<绝对路径> 后启动 <exe>，\n"
        "  等待其退出。--out 可写在任意位置；<exe> 之后的其他参数原样透传，\n"
        "  包括以 - 开头的参数。若要把 --out 传给目标程序，写在 -- 之后。\n"
        "  退出码与 <exe> 相同；成功退出但没有生成记录文件时退出 3。\n"
        "\n"
        "play:\n"
        "  校验记录文件（魔数 G2CRPLY，tools/replay 的二进制格式），\n"
        "  然后调用 `replay <file>`（GO2CPP_REPLAY_TOOL > 同目录 > PATH）。\n"
        "  后端缺失时退出 3，并给出构建命令。\n"
        "\n"
        "选项:\n"
        "  record --out <FILE>  必需。记录文件路径；写入前会删除同名旧文件。\n"
        "  -h, --help           显示本帮助。\n"
        "\n"
        "示例:\n"
        "  go2cpp-cli replay record ./build/my_server --out /tmp/run.g2cr\n"
        "  go2cpp-cli replay play /tmp/run.g2cr\n"
        "\n"
        "待对齐:\n"
        "  tools/replay 当前提供 demo 记录器 go2cpp_replay_record（固定场景），\n"
        "  通用 GO2CPP_REPLAY 注入待重放服务（2c3fb6f2）补齐。\n");
}

void PrintMigrateHelp() {
    Print(
        "用法:\n"
        "  go2cpp-cli migrate dump --pid <pid> --gid <gid> --out <file>\n"
        "  go2cpp-cli migrate restore <file>\n"
        "\n"
        "dump:\n"
        "  调用 `migrate dump <file>`（tools/migration）。\n"
        "  当前后端是 demo：只写它自己进程 A 的快照，不按 pid/gid 抓取；\n"
        "  --pid/--gid 会经 MIGRATION_PID/MIGRATION_GID 传给后端，并在 stderr 提示。\n"
        "\n"
        "restore:\n"
        "  校验快照（魔数 G2CMIG02，要求 version=2），然后调用 `migrate restore <file>`。\n"
        "  后端缺失时退出 3，并给出构建命令。\n"
        "\n"
        "选项:\n"
        "  dump --pid <PID>     必需。目标进程 pid（校验存在性）。\n"
        "  dump --gid <GID>     必需。要迁移的协程 id。\n"
        "  dump --out <FILE>    必需。快照文件路径（二进制，不能写 stdout）。\n"
        "  -h, --help           显示本帮助。\n"
        "\n"
        "示例:\n"
        "  go2cpp-cli migrate dump --pid 1234 --gid 42 --out /tmp/fiber.mig\n"
        "  go2cpp-cli migrate restore /tmp/fiber.mig\n"
        "\n"
        "前提:\n"
        "  v2 快照允许 PIE/ASLR；恢复方与生成方仍需是同一可执行文件\n"
        "  （快照带 ELF build-id 校验），但 Fiber 主体所在的 SO 可以换版本。\n");
}

void PrintDebugHelp() {
    Print(
        "用法: go2cpp-cli debug <file> [options]\n"
        "\n"
        "离线分析，不需要目标进程。识别：\n"
        "  - JSON dump（DebugServer 的 JSON payload，或任意 dump JSON）\n"
        "  - # go2cpp-trace / # go2cpp-replay 文本\n"
        "  - ini dump（[section] + key = value）\n"
        "  - G2CRPLY 重放记录（输出格式版本与文件大小）\n"
        "  - G2CMIG02 迁移快照（输出版本、段数、fiber_id、栈大小）\n"
        "\n"
        "选项:\n"
        "  --top <N>      时间线显示前 N 条，默认 20；0 表示全部。\n"
        "  --json         以 JSON 输出统计结果。\n"
        "  -h, --help     显示本帮助。\n"
        "\n"
        "示例:\n"
        "  go2cpp-cli debug /tmp/dump.json\n"
        "  go2cpp-cli debug /tmp/run.trace --top 5\n"
        "  go2cpp-cli debug /tmp/run.g2cr --json\n"
        "\n"
        "边界:\n"
        "  JSON 文件会在内存中完整解析，上限 64 MiB。\n");
}

void PrintDoctorHelp() {
    Print(
        "用法: go2cpp-cli doctor [--pid <PID> | --socket <PATH>] [options] [command]\n"
        "\n"
        "通过 G2CD/1 帧协议（24 字节头 + payload，FNV-1a/32 校验）连接目标进程\n"
        "的 DiagnosticAgent（Doctor）。Doctor 的接收/执行线程不经过 Scheduler，\n"
        "因此进程卡死时仍能返回现场；force-wake 只作用于白名单里的可重试等待。\n"
        "\n"
        "命令（默认 status）：\n"
        "  status [json]                 协程/M-P/runqueue/锁等待/模块 build-id\n"
        "  dump [sections=a,b]           复用 DebugServer 的组件 dump\n"
        "  force-wake scope=<SCOPE>      scheduler-idle|parking-condition|channel|all\n"
        "  deadlock-report [json]        等待图与 deadlock 判定\n"
        "  mutex-unlock name=<NAME>      受控解锁（仅 owner 已终止时）\n"
        "  inject-fiber fn=<NAME> [json] 在诊断 Fiber 的 GMP 上下文里执行\n"
        "  ping | stats | help\n"
        "\n"
        "选项:\n"
        "  --pid <PID>          按 pid 推导 /tmp/go2cpp-<pid>-doctor.sock\n"
        "  --socket <PATH>      直接指定 socket（优先于 --pid）\n"
        "  --timeout-ms <N>     连接/收发总超时，默认 5000\n"
        "  --json               给命令追加 json=1（响应为 JSON）\n"
        "  --concurrency <N>    并发 N 条命令（验收单线程命令平面/stream 匹配）\n"
        "  -h, --help           显示本帮助\n"
        "\n"
        "示例:\n"
        "  go2cpp-cli doctor --pid 1234 status\n"
        "  go2cpp-cli doctor --socket /tmp/demo.sock deadlock-report\n"
        "  go2cpp-cli doctor --socket /tmp/demo.sock \"force-wake scope=all\"\n"
        "  go2cpp-cli doctor --socket /tmp/demo.sock --concurrency 10 --command status\n"
        "\n"
        "退出码:\n"
        "  0 成功  1 协议/超时  2 用法  3 连接不上（Doctor 未启用）\n"
        "  5 目标返回 error 帧，或命令 verdict=refused\n"
        "\n"
        "环境变量:\n"
        "  GO2CPP_DOCTOR_SOCKET  覆盖 Doctor socket 路径\n");
}

}  // namespace go2cpp::cli
