#pragma once

/**
 * @file commands.hpp
 * @brief 五个子命令的入口：每个函数接收「不含子命令名」的参数列表并返回退出码。
 *
 * @details 约定：
 *   - 成功返回 0；失败一律抛 `CliError`（由 `main` 统一打印），
 *     只有「子进程的退出码需要原样透传」时才直接返回非零；
 *   - `--help` 在函数内部处理并返回 0，不抛异常；
 *   - 命令函数不读 `argv[0]`、不写全局状态，便于逐个测试。
 */

#include <string>
#include <vector>

namespace go2cpp::cli {

int RunDump(const std::vector<std::string>& args);
int RunStatus(const std::vector<std::string>& args);
int RunReplay(const std::vector<std::string>& args);
int RunMigrate(const std::vector<std::string>& args);
int RunDebug(const std::vector<std::string>& args);

}  // namespace go2cpp::cli
