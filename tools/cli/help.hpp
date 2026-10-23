#pragma once

/**
 * @file help.hpp
 * @brief 帮助与版本文本：每个子命令一份，`--help` 与 `--help` 后的错误提示共用。
 *
 * @details 帮助文本集中在这里而不是散落在命令实现里，原因有两个：
 *          1. 验收要求每个子命令的 `--help` 输出「完整」，集中后可以逐份对照检查；
 *          2. 错误文案里反复出现「运行 --help 查看可用选项」，集中后不会出现
 *             某个子命令忘了写帮助的情况。
 *
 *          文本走 stdout（不是 stderr），退出码 0 —— 与 `git`/`docker` 等工具一致，
 *          方便 `go2cpp-cli dump --help | less`。
 */

namespace go2cpp::cli {

void PrintMainHelp();
void PrintDumpHelp();
void PrintStatusHelp();
void PrintReplayHelp();
void PrintMigrateHelp();
void PrintDebugHelp();
void PrintDoctorHelp();
void PrintGraphHelp();
void PrintVersion();

}  // namespace go2cpp::cli
