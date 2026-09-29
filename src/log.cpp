#include "go2cpp/log.hpp"

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>
#include <unordered_map>

namespace go2cpp::log {
namespace {

std::string ReplaceTokens(std::string result,
                          const std::unordered_map<std::string, std::string>& values) {
    for (const auto& [key, value] : values) {
        const std::string token = "{" + key + "}";
        std::size_t position = 0;
        while ((position = result.find(token, position)) != std::string::npos) {
            result.replace(position, token.size(), value);
            position += value.size();
        }
    }
    return result;
}

std::string FormatTime(std::uint64_t timestamp_ms) {
    const auto seconds = static_cast<std::time_t>(timestamp_ms / 1000);
    std::tm local_time{};
#if defined(_WIN32)
    localtime_s(&local_time, &seconds);
#else
    localtime_r(&seconds, &local_time);
#endif
    std::ostringstream stream;
    stream << std::put_time(&local_time, "%Y-%m-%d %H:%M:%S") << '.'
           << std::setfill('0') << std::setw(3) << (timestamp_ms % 1000);
    return stream.str();
}

std::uint64_t CurrentTimeMs() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

std::uint64_t CurrentThreadId() noexcept {
    return static_cast<std::uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
}

}  // namespace

const char* ToString(Level level) noexcept {
    switch (level) {
        case Level::Trace: return "TRACE";
        case Level::Debug: return "DEBUG";
        case Level::Info: return "INFO";
        case Level::Warn: return "WARN";
        case Level::Error: return "ERROR";
        case Level::Critical: return "CRITICAL";
        case Level::Off: return "OFF";
    }
    return "UNKNOWN";
}

bool ParseLevel(std::string_view text, Level* level) noexcept {
    if (!level) return false;
    std::string value(text);
    for (auto& character : value) {
        if (character >= 'A' && character <= 'Z') character = static_cast<char>(character - 'A' + 'a');
    }
    if (value == "trace") *level = Level::Trace;
    else if (value == "debug") *level = Level::Debug;
    else if (value == "info") *level = Level::Info;
    else if (value == "warn" || value == "warning") *level = Level::Warn;
    else if (value == "error") *level = Level::Error;
    else if (value == "critical" || value == "fatal") *level = Level::Critical;
    else if (value == "off") *level = Level::Off;
    else return false;
    return true;
}

bool MinimumLevelFilter::Accept(const LogRecord& record) const {
    const auto minimum = m_level.load(std::memory_order_acquire);
    return record.level >= minimum && minimum != Level::Off;
}

PatternFormatter::PatternFormatter(std::string pattern) : m_pattern(std::move(pattern)) {}

std::string PatternFormatter::Format(const LogRecord& record) const {
    return ReplaceTokens(m_pattern, {
        {"time", FormatTime(record.timestamp_ms)},
        {"level", ToString(record.level)},
        {"logger", record.logger},
        {"thread", std::to_string(record.thread_id)},
        {"fiber", std::to_string(record.fiber_id)},
        {"file", record.file},
        {"line", std::to_string(record.line)},
        {"function", record.function},
        {"message", record.message},
    });
}

FileSink::FileSink(std::string path) : m_path(std::move(path)) {
    try {
        const auto parent = std::filesystem::path(m_path).parent_path();
        if (!parent.empty()) std::filesystem::create_directories(parent);
        m_stream = std::make_unique<std::ofstream>(m_path, std::ios::app | std::ios::out);
    } catch (...) {
        m_stream.reset();
    }
}

void FileSink::Write(const LogRecord&, std::string_view formatted) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_stream) return;
    auto* stream = dynamic_cast<std::ofstream*>(m_stream.get());
    if (!stream || !stream->good()) return;
    stream->write(formatted.data(), static_cast<std::streamsize>(formatted.size()));
    stream->flush();
}

void StdoutSink::Write(const LogRecord&, std::string_view formatted) {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::cout.write(formatted.data(), static_cast<std::streamsize>(formatted.size()));
    std::cout.flush();
}

void CallbackSink::Write(const LogRecord& record, std::string_view formatted) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_callback) m_callback(record, formatted);
}

LogItemWorker::LogItemWorker(LogFilter::ptr filter, LogFormatter::ptr formatter,
                             LogSink::ptr sink)
    : m_filter(std::move(filter)), m_formatter(std::move(formatter)), m_sink(std::move(sink)) {}

bool LogItemWorker::Submit(const LogRecord& record) {
    LogFilter::ptr filter;
    LogFormatter::ptr formatter;
    LogSink::ptr sink;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        filter = m_filter;
        formatter = m_formatter;
        sink = m_sink;
    }
    // 不在 Worker 锁内调用用户 Formatter/Sink；这样自定义 Sink 再次记录
    // 日志时不会形成同一 Worker 的递归锁死。
    if (!filter || !formatter || !sink || !filter->Accept(record)) return false;
    const auto formatted = formatter->Format(record);
    sink->Write(record, formatted);
    return true;
}

void LogItemWorker::SetFilter(LogFilter::ptr filter) { std::lock_guard<std::mutex> lock(m_mutex); m_filter = std::move(filter); }
void LogItemWorker::SetMinimumLevel(Level level) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (auto filter = std::dynamic_pointer_cast<MinimumLevelFilter>(m_filter)) filter->SetLevel(level);
}
void LogItemWorker::SetFormatter(LogFormatter::ptr formatter) { std::lock_guard<std::mutex> lock(m_mutex); m_formatter = std::move(formatter); }
void LogItemWorker::SetSink(LogSink::ptr sink) { std::lock_guard<std::mutex> lock(m_mutex); m_sink = std::move(sink); }

Logger::Logger(std::string name) : m_name(std::move(name)) {}
void Logger::SetLevel(Level level) noexcept {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_level = level;
    for (const auto& worker : m_workers) {
        // LoggerManager 重新加载配置时同步更新默认过滤器；用户自定义过滤器
        // 不是 MinimumLevelFilter 时保持原样。
        if (worker) worker->SetMinimumLevel(level);
    }
}
Level Logger::level() const noexcept { std::lock_guard<std::mutex> lock(m_mutex); return m_level; }
bool Logger::ShouldLog(Level value) const noexcept { return value >= level() && value != Level::Off; }
void Logger::AddWorker(LogItemWorker::ptr worker) { if (worker) { std::lock_guard<std::mutex> lock(m_mutex); m_workers.push_back(std::move(worker)); } }
void Logger::ClearWorkers() { std::lock_guard<std::mutex> lock(m_mutex); m_workers.clear(); }
void Logger::Log(LogRecord record) const {
    std::vector<LogItemWorker::ptr> workers;
    { std::lock_guard<std::mutex> lock(m_mutex); if (record.level < m_level || m_level == Level::Off) return; workers = m_workers; }
    for (const auto& worker : workers) (void)worker->Submit(record);
}

LoggerManager& LoggerManager::Instance() { static LoggerManager manager; return manager; }
LoggerManager::LoggerManager() : m_root(std::make_shared<Logger>("root")) { Configure(m_level, m_stdout_enabled, m_directory, m_file); }

Logger::ptr LoggerManager::Get(const std::string& name) {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto& logger : m_loggers) if (logger->name() == name) return logger;
    auto logger = std::make_shared<Logger>(name);
    logger->SetLevel(m_level);
    const auto formatter = std::make_shared<PatternFormatter>(m_pattern);
    logger->AddWorker(std::make_shared<LogItemWorker>(std::make_shared<MinimumLevelFilter>(m_level), formatter,
                                                      std::make_shared<FileSink>((std::filesystem::path(m_directory) / m_file).string())));
    if (m_stdout_enabled) logger->AddWorker(std::make_shared<LogItemWorker>(std::make_shared<MinimumLevelFilter>(m_level), formatter, std::make_shared<StdoutSink>()));
    m_loggers.push_back(logger);
    return logger;
}

Logger::ptr LoggerManager::Root() { return Get("root"); }

void LoggerManager::Configure(Level level, bool stdout_enabled, const std::string& directory,
                              const std::string& file, const std::string& pattern) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_level = level; m_stdout_enabled = stdout_enabled; m_directory = directory.empty() ? "log" : directory;
    m_file = file.empty() ? "go2cpp.log" : file; m_pattern = pattern.empty() ? "{time} [{level}] {logger} ({file}:{line}) {message}\n" : pattern;
    for (const auto& logger : m_loggers) logger->SetLevel(level);
    if (m_root) m_root->SetLevel(level);
}

LogLine::LogLine(Logger::ptr logger, Level level, const char* file, int line, const char* function)
    : m_logger(std::move(logger)) {
    m_record.level = level; m_record.file = file ? file : ""; m_record.line = line; m_record.function = function ? function : "";
    if (m_logger) { m_record.logger = m_logger->name(); }
    m_record.timestamp_ms = CurrentTimeMs(); m_record.thread_id = CurrentThreadId();
}

LogLine::~LogLine() noexcept {
    if (!m_logger || !m_logger->ShouldLog(m_record.level)) return;
    try { m_record.message = m_stream.str(); m_logger->Log(std::move(m_record)); } catch (...) {}
}

}  // namespace go2cpp::log
