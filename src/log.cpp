#include "go2cpp/log.hpp"

#include <filesystem>
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>
#include <cstdio>

namespace go2cpp::log {
namespace {

std::string FormatTime(std::uint64_t timestamp_ms);

std::string FormatPattern(std::string pattern, const LogRecord& record) {
    // 一次线性扫描同时支持 {message} 风格和 Sylar 的 %d/%p/%m 风格，
    // 避免旧实现为每个字段建立 unordered_map 并反复 find/replace。
    const auto append_token = [&record](std::string_view token, std::string& output) {
        if (token == "time") output += FormatTime(record.timestamp_ms);
        else if (token == "level") output += ToString(record.level);
        else if (token == "logger") output += record.logger;
        else if (token == "thread") output += std::to_string(record.thread_id);
        else if (token == "fiber") output += std::to_string(record.fiber_id);
        else if (token == "elapse") output += std::to_string(record.elapse_ms);
        else if (token == "thread_name") output += record.thread_name;
        else if (token == "file") output += record.file;
        else if (token == "line") output += std::to_string(record.line);
        else if (token == "function") output += record.function;
        else if (token == "message") output += record.message;
        else return false;
        return true;
    };
    std::string output;
    output.reserve(pattern.size() + record.message.size());
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        if (pattern[i] == '{') {
            const auto end = pattern.find('}', i + 1);
            if (end != std::string::npos && append_token(
                    std::string_view(pattern).substr(i + 1, end - i - 1), output)) {
                i = end;
                continue;
            }
        }
        if (pattern[i] != '%') {
            output.push_back(pattern[i]);
            continue;
        }
        if (i + 1 >= pattern.size()) {
            output.push_back('%');
            continue;
        }
        const char code = pattern[++i];
        switch (code) {
            case '%': output.push_back('%'); break;
            case 'm': output += record.message; break;
            case 'p': output += ToString(record.level); break;
            case 'N': output += record.logger; break;
            case 'f': output += record.file; break;
            case 'l': output += std::to_string(record.line); break;
            case 't': output += std::to_string(record.thread_id); break;
            case 'F': output += std::to_string(record.fiber_id); break;
            case 'T': output.push_back('\t'); break;
            case 'n': output.push_back('\n'); break;
            case 'E': output += std::to_string(record.elapse_ms); break;
            case 'c': output += record.thread_name; break;
            case 'd': {
                // %d{...} 的完整 strftime 解析；没有格式时使用默认时间。
                std::string format = "%Y-%m-%d %H:%M:%S";
                if (i + 1 < pattern.size() && pattern[i + 1] == '{') {
                    const auto end = pattern.find('}', i + 2);
                    if (end != std::string::npos) {
                        format = pattern.substr(i + 2, end - i - 2);
                        i = end;
                    }
                }
                const auto seconds = static_cast<std::time_t>(record.timestamp_ms / 1000);
                std::tm tm{};
#if defined(_WIN32)
                localtime_s(&tm, &seconds);
#else
                localtime_r(&seconds, &tm);
#endif
                char buffer[128]{};
                if (std::strftime(buffer, sizeof(buffer), format.c_str(), &tm) != 0) output += buffer;
                break;
            }
            default:
                output.push_back('%');
                output.push_back(code);
                break;
        }
    }
    return output;
}

std::string FormatTime(std::uint64_t timestamp_ms) {
    const auto seconds = static_cast<std::time_t>(timestamp_ms / 1000);
    std::tm local_time{};
#if defined(_WIN32)
    localtime_s(&local_time, &seconds);
#else
    localtime_r(&seconds, &local_time);
#endif
    char date[64]{};
    if (std::strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S", &local_time) == 0) {
        return {};
    }
    char millis[5]{};
    std::snprintf(millis, sizeof(millis), ".%03llu",
                  static_cast<unsigned long long>(timestamp_ms % 1000));
    std::string result(date);
    result += millis;
    return result;
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
        case Level::Notice: return "NOTICE";
        case Level::Warn: return "WARN";
        case Level::Error: return "ERROR";
        case Level::Crit: return "CRIT";
        case Level::Alert: return "ALERT";
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
    else if (value == "notice") *level = Level::Notice;
    else if (value == "warn" || value == "warning") *level = Level::Warn;
    else if (value == "error") *level = Level::Error;
    else if (value == "crit") *level = Level::Crit;
    else if (value == "alert") *level = Level::Alert;
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
    return FormatPattern(m_pattern, record);
}

FileSink::FileSink(std::string path) : m_path(std::move(path)) {
    Reopen();
}

void FileSink::Write(const LogRecord& record, std::string_view formatted) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_stream) return;
    auto* stream = dynamic_cast<std::ofstream*>(m_stream.get());
    if (!stream || !stream->good()) return;
    stream->write(formatted.data(), static_cast<std::streamsize>(formatted.size()));
    // 发布默认只记录告警及错误；这些记录必须在调用返回后可被外部
    // 日志收集器立即读取。Info/Debug 仍保留缓冲写入，避免把高吞吐
    // 日志退化成每条一次 flush。
    if (record.level >= Level::Warn) stream->flush();
}

void FileSink::Flush() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (auto* stream = dynamic_cast<std::ofstream*>(m_stream.get())) stream->flush();
}

bool FileSink::Reopen() {
    std::lock_guard<std::mutex> lock(m_mutex);
    try {
        const auto parent = std::filesystem::path(m_path).parent_path();
        if (!parent.empty()) std::filesystem::create_directories(parent);
        auto stream = std::make_unique<std::ofstream>(m_path, std::ios::app | std::ios::out);
        if (!stream->is_open()) return false;
        m_stream = std::move(stream);
        return true;
    } catch (...) {
        m_stream.reset();
        return false;
    }
}

RotatingFileSink::RotatingFileSink(std::string path, std::size_t max_bytes,
                                   std::size_t max_files)
    : FileSink(std::move(path)), m_max_bytes(max_bytes), m_max_files(max_files) {
    // 进程重启后继续追加同一个文件时，旋转阈值必须包含已有字节数；
    // 否则第一次写入会暂时突破 max_bytes。
    std::error_code error;
    const auto existing = std::filesystem::file_size(m_path, error);
    if (!error) {
        m_bytes = existing;
    }
}

void RotatingFileSink::Write(const LogRecord&, std::string_view formatted) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_max_bytes != 0 && m_bytes + formatted.size() > m_max_bytes) {
        if (m_stream) dynamic_cast<std::ofstream*>(m_stream.get())->close();
        for (std::size_t i = m_max_files; i > 0; --i) {
            const auto source = i == 1 ? m_path : m_path + "." + std::to_string(i - 1);
            const auto target = m_path + "." + std::to_string(i);
            if (std::filesystem::exists(source)) {
                std::error_code error;
                std::filesystem::remove(target, error);
                std::filesystem::rename(source, target, error);
            }
        }
        std::error_code error;
        std::filesystem::remove(m_path, error);
        m_stream.reset();
        auto stream = std::make_unique<std::ofstream>(m_path, std::ios::app | std::ios::out);
        if (stream->is_open()) m_stream = std::move(stream);
        m_bytes = 0;
    }
    if (m_stream) {
        auto* stream = dynamic_cast<std::ofstream*>(m_stream.get());
        stream->write(formatted.data(), static_cast<std::streamsize>(formatted.size()));
        m_bytes += formatted.size();
    }
}

void StdoutSink::Write(const LogRecord&, std::string_view formatted) {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::cout.write(formatted.data(), static_cast<std::streamsize>(formatted.size()));
}

void StderrSink::Write(const LogRecord&, std::string_view formatted) {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::cerr.write(formatted.data(), static_cast<std::streamsize>(formatted.size()));
}

void MemorySink::Write(const LogRecord&, std::string_view formatted) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_lines.emplace_back(formatted);
}

std::vector<std::string> MemorySink::Snapshot() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_lines;
}

void MemorySink::Clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_lines.clear();
}

void CallbackSink::Write(const LogRecord& record, std::string_view formatted) {
    if (m_callback) m_callback(record, formatted);
}

LogItemWorker::LogItemWorker(LogFilter::ptr filter, LogFormatter::ptr formatter,
                             LogSink::ptr sink)
    : m_snapshot(std::make_shared<const Snapshot>(Snapshot{
          std::move(filter), std::move(formatter), std::move(sink)})) {}

bool LogItemWorker::Submit(const LogRecord& record) {
    const auto snapshot = std::atomic_load_explicit(&m_snapshot, std::memory_order_acquire);
    if (!snapshot) return false;
    // 不在 Worker 锁内调用用户 Formatter/Sink；这样自定义 Sink 再次记录
    // 日志时不会形成同一 Worker 的递归锁死。
    if (!snapshot->filter || !snapshot->formatter || !snapshot->sink ||
        !snapshot->filter->Accept(record)) return false;
    const auto formatted = snapshot->formatter->Format(record);
    snapshot->sink->Write(record, formatted);
    return true;
}

void LogItemWorker::SetFilter(LogFilter::ptr filter) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto current = std::atomic_load_explicit(&m_snapshot, std::memory_order_acquire);
    auto next = std::make_shared<Snapshot>(current ? *current : Snapshot{});
    next->filter = std::move(filter);
    std::atomic_store_explicit(&m_snapshot, std::shared_ptr<const Snapshot>(std::move(next)),
                               std::memory_order_release);
}
void LogItemWorker::SetMinimumLevel(Level level) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto current = std::atomic_load_explicit(&m_snapshot, std::memory_order_acquire);
    if (current) {
        if (auto filter = std::dynamic_pointer_cast<MinimumLevelFilter>(current->filter)) {
            filter->SetLevel(level);
        }
    }
}
void LogItemWorker::SetFormatter(LogFormatter::ptr formatter) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto current = std::atomic_load_explicit(&m_snapshot, std::memory_order_acquire);
    auto next = std::make_shared<Snapshot>(current ? *current : Snapshot{});
    next->formatter = std::move(formatter);
    std::atomic_store_explicit(&m_snapshot, std::shared_ptr<const Snapshot>(std::move(next)),
                               std::memory_order_release);
}
void LogItemWorker::SetSink(LogSink::ptr sink) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto current = std::atomic_load_explicit(&m_snapshot, std::memory_order_acquire);
    auto next = std::make_shared<Snapshot>(current ? *current : Snapshot{});
    next->sink = std::move(sink);
    std::atomic_store_explicit(&m_snapshot, std::shared_ptr<const Snapshot>(std::move(next)),
                               std::memory_order_release);
}
LogSink::ptr LogItemWorker::Sink() const {
    const auto snapshot = std::atomic_load_explicit(&m_snapshot, std::memory_order_acquire);
    return snapshot ? snapshot->sink : nullptr;
}
void LogItemWorker::Flush() {
    const auto snapshot = std::atomic_load_explicit(&m_snapshot, std::memory_order_acquire);
    if (snapshot && snapshot->sink) snapshot->sink->Flush();
}

Logger::Logger(std::string name)
    : m_name(std::move(name)),
      m_worker_snapshot(std::make_shared<const std::vector<LogItemWorker::ptr>>()) {}
void Logger::SetLevel(Level level) noexcept {
    m_level.store(level, std::memory_order_release);
    const auto workers = std::atomic_load_explicit(&m_worker_snapshot, std::memory_order_acquire);
    for (const auto& worker : *workers) {
        // LoggerManager 重新加载配置时同步更新默认过滤器；用户自定义过滤器
        // 不是 MinimumLevelFilter 时保持原样。
        if (worker) worker->SetMinimumLevel(level);
    }
}
Level Logger::level() const noexcept { return m_level.load(std::memory_order_acquire); }
bool Logger::ShouldLog(Level value) const noexcept {
    const auto minimum = m_level.load(std::memory_order_relaxed);
    return value >= minimum && minimum != Level::Off;
}
void PublishLoggerWorkers(std::vector<LogItemWorker::ptr>* workers,
                          std::shared_ptr<const std::vector<LogItemWorker::ptr>>* snapshot) {
    std::atomic_store_explicit(snapshot,
        std::make_shared<const std::vector<LogItemWorker::ptr>>(*workers),
        std::memory_order_release);
}
void Logger::AddWorker(LogItemWorker::ptr worker) {
    if (!worker) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    m_custom_workers.push_back(std::move(worker));
    m_workers = m_default_workers;
    m_workers.insert(m_workers.end(), m_custom_workers.begin(), m_custom_workers.end());
    PublishLoggerWorkers(&m_workers, &m_worker_snapshot);
}
void Logger::AddDefaultWorker(LogItemWorker::ptr worker) {
    if (!worker) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    m_default_workers.push_back(std::move(worker));
    m_workers = m_default_workers;
    m_workers.insert(m_workers.end(), m_custom_workers.begin(), m_custom_workers.end());
    PublishLoggerWorkers(&m_workers, &m_worker_snapshot);
}
void Logger::ConfigureDefaults(std::vector<LogItemWorker::ptr> workers) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_default_workers = std::move(workers);
    m_workers = m_default_workers;
    m_workers.insert(m_workers.end(), m_custom_workers.begin(), m_custom_workers.end());
    PublishLoggerWorkers(&m_workers, &m_worker_snapshot);
}
void Logger::ClearWorkers() { std::lock_guard<std::mutex> lock(m_mutex); m_workers.clear(); m_default_workers.clear(); m_custom_workers.clear(); PublishLoggerWorkers(&m_workers, &m_worker_snapshot); }
void Logger::SetParent(Logger::ptr parent) { std::lock_guard<std::mutex> lock(m_mutex); m_parent = std::move(parent); }
void Logger::SetPropagate(bool propagate) noexcept { m_propagate.store(propagate, std::memory_order_release); }
bool Logger::propagate() const noexcept { return m_propagate.load(std::memory_order_acquire); }
void Logger::Flush() const { const auto workers = std::atomic_load_explicit(&m_worker_snapshot, std::memory_order_acquire); for (const auto& worker : *workers) if (worker) worker->Flush(); }
void Logger::Log(LogRecord record) const {
    Logger::ptr parent;
    if (!ShouldLog(record.level)) return;
    const auto workers = std::atomic_load_explicit(&m_worker_snapshot, std::memory_order_acquire);
    for (const auto& worker : *workers) (void)worker->Submit(record);
    const bool propagate = m_propagate.load(std::memory_order_acquire);
    if (propagate) { std::lock_guard<std::mutex> lock(m_mutex); parent = m_parent.lock(); }
    if (propagate && parent) parent->Log(std::move(record));
}

LoggerManager& LoggerManager::Instance() { static LoggerManager manager; return manager; }
LoggerManager::LoggerManager() : m_root(std::make_shared<Logger>("root")) {
    m_loggers.push_back(m_root);
    Configure(m_level, m_stdout_enabled, m_directory, m_file);
}

Logger::ptr LoggerManager::Get(const std::string& name) {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto& logger : m_loggers) if (logger->name() == name) return logger;
    auto logger = std::make_shared<Logger>(name);
    logger->SetLevel(m_level);
    const auto formatter = std::make_shared<PatternFormatter>(m_pattern);
    std::vector<LogItemWorker::ptr> defaults;
    defaults.push_back(std::make_shared<LogItemWorker>(std::make_shared<MinimumLevelFilter>(m_level), formatter,
                                                       std::make_shared<FileSink>((std::filesystem::path(m_directory) / m_file).string())));
    if (m_stdout_enabled) defaults.push_back(std::make_shared<LogItemWorker>(std::make_shared<MinimumLevelFilter>(m_level), formatter, std::make_shared<StdoutSink>()));
    logger->ConfigureDefaults(std::move(defaults));
    if (name != "root") logger->SetParent(m_root);
    m_loggers.push_back(logger);
    return logger;
}

Logger::ptr LoggerManager::Root() { return Get("root"); }

void LoggerManager::Configure(Level level, bool stdout_enabled, const std::string& directory,
                              const std::string& file, const std::string& pattern) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_level = level; m_stdout_enabled = stdout_enabled; m_directory = directory.empty() ? "log" : directory;
    m_file = file.empty() ? "go2cpp.log" : file; m_pattern = pattern.empty() ? "{time} [{level}] {logger} ({file}:{line}) {message}\n" : pattern;
    const auto make_defaults = [this, level]() {
        std::vector<LogItemWorker::ptr> defaults;
        const auto formatter = std::make_shared<PatternFormatter>(m_pattern);
        defaults.push_back(std::make_shared<LogItemWorker>(std::make_shared<MinimumLevelFilter>(level), formatter,
                                                           std::make_shared<FileSink>((std::filesystem::path(m_directory) / m_file).string())));
        if (m_stdout_enabled) defaults.push_back(std::make_shared<LogItemWorker>(std::make_shared<MinimumLevelFilter>(level), formatter, std::make_shared<StdoutSink>()));
        return defaults;
    };
    for (const auto& logger : m_loggers) { logger->SetLevel(level); logger->ConfigureDefaults(make_defaults()); }
}

void LoggerManager::ConfigureLogger(const std::string& name, Level level, bool propagate,
                                    std::vector<LogItemWorker::ptr> workers) {
    auto logger = Get(name);
    logger->SetLevel(level);
    logger->SetPropagate(propagate);
    if (!workers.empty()) logger->ConfigureDefaults(std::move(workers));
}

void LoggerManager::Remove(const std::string& name) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (name == "root") return;
    m_loggers.erase(std::remove_if(m_loggers.begin(), m_loggers.end(), [&](const Logger::ptr& logger) { return logger && logger->name() == name; }), m_loggers.end());
}

std::vector<Logger::ptr> LoggerManager::List() const { std::lock_guard<std::mutex> lock(m_mutex); return m_loggers; }
void LoggerManager::Flush() { for (const auto& logger : List()) if (logger) logger->Flush(); }

LogLine::LogLine(Logger::ptr logger, Level level, const char* file, int line, const char* function)
    : m_logger(std::move(logger)) {
    m_enabled = m_logger && m_logger->ShouldLog(level);
    if (!m_enabled) return;
    m_record.level = level; m_record.file = file ? file : ""; m_record.line = line; m_record.function = function ? function : "";
    if (m_logger) { m_record.logger = m_logger->name(); }
    m_record.timestamp_ms = CurrentTimeMs(); m_record.thread_id = CurrentThreadId();
}

LogLine::~LogLine() noexcept {
    if (!m_enabled || !m_logger) return;
    try { m_record.message = m_stream.str(); m_logger->Log(std::move(m_record)); } catch (...) {}
}

}  // namespace go2cpp::log
