#pragma once

/**
 * @file logger.h
 * @brief chat_server_full 的线程安全日志（stdout + 可选文件）。
 * @details LinkServer 的回调在接收线程执行，应用层心跳在 Scheduler Fiber
 *          执行，两边都可能写日志；这里用一把互斥量保证整行不交错。
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include <string>

namespace chat {

enum class LogLevel : int { kDebug = 0, kInfo = 1, kWarn = 2, kError = 3 };

/// @brief 初始化文件日志；文件不可写返回 false，但仍可只用 stdout。
bool InitLogger(const std::string& file_path, const std::string& level_text, std::string* error);
void ShutdownLogger();
void LogMessage(LogLevel level, const std::string& message);

void LogDebug(const std::string& message);
void LogInfo(const std::string& message);
void LogWarn(const std::string& message);
void LogError(const std::string& message);

}  // namespace chat
