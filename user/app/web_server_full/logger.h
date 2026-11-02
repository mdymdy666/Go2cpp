#pragma once

/**
 * @file logger.h
 * @brief 线程安全的轻量日志：同时写标准输出与可选文件。
 * @details HttpServer 的 handler 在多个 worker 线程并发执行，日志必须
 *          整行串行化，否则访问日志会互相穿插。这里用一把互斥量保护
 *          「格式化 + 写 stdout + 写文件」，不做异步队列——示例保持可读。
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include <string>

namespace app {

/// @brief 日志级别；数值越大越严重，低于当前级别的记录被丢弃。
enum class LogLevel : int { kDebug = 0, kInfo = 1, kWarn = 2, kError = 3 };

/// @brief 解析级别名；无法识别时返回 false 并保留 out。
bool ParseLogLevel(const std::string& text, LogLevel* out);

/// @brief 级别名（日志行使用固定宽度）。
const char* LogLevelName(LogLevel level);

/// @brief 打开日志文件并设置级别；文件打不开返回 false（仍可只写 stdout）。
bool InitLogger(const std::string& file_path, const std::string& level_text, std::string* error);

/// @brief 刷新并关闭日志文件；可重复调用。
void ShutdownLogger();

/// @brief 运行期调整级别（原子读，不加日志锁）。
void SetLogLevel(LogLevel level);

/// @brief 写一条日志；低于当前级别直接返回。
void LogMessage(LogLevel level, const std::string& message);

void LogDebug(const std::string& message);
void LogInfo(const std::string& message);
void LogWarn(const std::string& message);
void LogError(const std::string& message);

}  // namespace app
