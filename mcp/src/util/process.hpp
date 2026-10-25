/**
 * @file process.hpp
 * @brief 子进程执行工具：捕获合并输出、支持超时与标准输入。
 *
 * @details
 * 两处需要执行外部程序：robot-control 的 `ros2` 命令、LLM 客户端对 HTTPS
 * 端点的 `curl` 调用。共同点是「不能走 shell（避免注入）+ 必须能杀超时进程」，
 * 所以集中在这里一份实现；底层已统一到 `go2cpp::os::Run`（启动、输出收集、
 * 超时阶梯与回收都由 os 负责），不再有第二套进程执行代码。
 *
 * 不变量：
 * - 子进程的 stdout 与 stderr **合并**到一个管道（调用方拿到的是原始交错输出）；
 * - 超时后按 TERM → 短宽限 → KILL 阶梯终止并回收，`timed_out == true`；
 * - 不经过 shell，`argv` 逐项 exec；裸程序名按 PATH 查找。
 */

#pragma once

#include <string>
#include <vector>

namespace go2cpp::mcp::util {

/// 子进程执行结果。
struct ProcessResult {
    int exit_code = -1;      ///< 退出码；被信号杀死或执行失败时为 -1（exec 失败为 127）
    bool timed_out = false;  ///< 是否因超时被终止
    std::string output;      ///< stdout 与 stderr 的合并输出
};

/**
 * @brief 执行外部命令。
 * @param argv 参数列表，argv[0] 按 PATH 查找。
 * @param timeout_ms 超时毫秒数；<=0 使用 5000。
 * @param stdin_data 写入子进程标准输入的内容（为空则重定向 /dev/null）。
 * @return 退出码、超时标志与合并输出。
 */
ProcessResult RunProcess(const std::vector<std::string>& argv, int timeout_ms,
                         const std::string& stdin_data = {});

}  // namespace go2cpp::mcp::util
