/**
 * @file logger.cpp
 * @brief logger.h 的实现：时间戳格式化、stdout/文件双写、互斥保护。
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include "logger.h"

#include <atomic>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>

namespace app {
namespace {

std::mutex g_log_mutex;
std::ofstream g_log_file;
std::atomic<LogLevel> g_min_level{LogLevel::kInfo};
bool g_file_ready = false;

/// @brief 生成本地时间 "YYYY-MM-DD HH:MM:SS.mmm"；失败时退化为空串。
std::string NowStamp() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
                            now.time_since_epoch()) % 1000;
    std::tm local{};
    if (localtime_r(&seconds, &local) == nullptr) {
        return {};
    }
    char buffer[32];
    if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &local) == 0) {
        return {};
    }
    char suffix[8];
    std::snprintf(suffix, sizeof(suffix), ".%03d", static_cast<int>(millis.count()));
    return std::string(buffer) + suffix;
}

}  // namespace

bool ParseLogLevel(const std::string& text, LogLevel* out) {
    if (text == "debug") {
        *out = LogLevel::kDebug;
    } else if (text == "info") {
        *out = LogLevel::kInfo;
    } else if (text == "warn" || text == "warning") {
        *out = LogLevel::kWarn;
    } else if (text == "error") {
        *out = LogLevel::kError;
    } else {
        return false;
    }
    return true;
}

const char* LogLevelName(LogLevel level) {
    switch (level) {
        case LogLevel::kDebug: return "debug";
        case LogLevel::kInfo: return "info";
        case LogLevel::kWarn: return "warn";
        case LogLevel::kError: return "error";
    }
    return "unknown";
}

bool InitLogger(const std::string& file_path, const std::string& level_text, std::string* error) {
    LogLevel level = LogLevel::kInfo;
    if (!ParseLogLevel(level_text, &level)) {
        if (error != nullptr) {
            *error = "未知日志级别: " + level_text;
        }
        return false;
    }
    std::lock_guard<std::mutex> lock(g_log_mutex);
    g_min_level.store(level, std::memory_order_release);
    g_log_file.close();
    g_log_file.open(file_path, std::ios::out | std::ios::app);
    g_file_ready = g_log_file.is_open();
    if (!g_file_ready) {
        if (error != nullptr) {
            *error = "无法打开日志文件: " + file_path;
        }
        return false;
    }
    g_log_file << "# web_server_full logger started\n" << std::flush;
    return true;
}

void ShutdownLogger() {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    if (g_file_ready) {
        g_log_file.flush();
        g_log_file.close();
        g_file_ready = false;
    }
}

void SetLogLevel(LogLevel level) { g_min_level.store(level, std::memory_order_release); }

void LogMessage(LogLevel level, const std::string& message) {
    if (static_cast<int>(level) < static_cast<int>(g_min_level.load(std::memory_order_acquire))) {
        return;
    }
    const std::string line = "[" + NowStamp() + "] [" + LogLevelName(level) + "] " + message + "\n";
    std::lock_guard<std::mutex> lock(g_log_mutex);
    std::cout << line << std::flush;
    if (g_file_ready && g_log_file.is_open()) {
        g_log_file << line << std::flush;
    }
}

void LogDebug(const std::string& message) { LogMessage(LogLevel::kDebug, message); }
void LogInfo(const std::string& message) { LogMessage(LogLevel::kInfo, message); }
void LogWarn(const std::string& message) { LogMessage(LogLevel::kWarn, message); }
void LogError(const std::string& message) { LogMessage(LogLevel::kError, message); }

}  // namespace app
