#pragma once

// Go2Cpp 自带的轻量日志接口。
// 日志模块只依赖 C++17 标准库，借助 Filter、Formatter、Worker、Sink 四层
// 组合实现扩展。这样既能独立使用，也能通过 CallbackSink 接入 spdlog/fmt。

#include <chrono>
#include <ctime>
#include <cstdint>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <optional>

namespace go2cpp::log {

enum class Level : std::uint8_t {
    Trace = 0,
    Debug,
    Info,
    Notice,
    Warn,
    Error,
    Crit,
    Alert,
    Critical,
    Off,
    Fatal = Critical,
};

const char* ToString(Level level) noexcept;
bool ParseLevel(std::string_view text, Level* level) noexcept;

struct LogRecord {
    Level level{Level::Info};
    std::string logger;
    std::string message;
    std::string file;
    std::string function;
    std::uint64_t timestamp_ms{0};
    std::uint64_t thread_id{0};
    std::uint64_t fiber_id{0};
    std::uint64_t elapse_ms{0};
    std::string thread_name;
    int line{0};
};

class LogFilter {
public:
    using ptr = std::shared_ptr<LogFilter>;
    virtual ~LogFilter() = default;
    virtual bool Accept(const LogRecord& record) const = 0;
};

class MinimumLevelFilter final : public LogFilter {
public:
    explicit MinimumLevelFilter(Level level) : m_level(level) {}
    bool Accept(const LogRecord& record) const override;
    void SetLevel(Level level) noexcept { m_level.store(level, std::memory_order_release); }
    Level level() const noexcept { return m_level.load(std::memory_order_acquire); }

private:
    std::atomic<Level> m_level;
};

class LogFormatter {
public:
    using ptr = std::shared_ptr<LogFormatter>;
    virtual ~LogFormatter() = default;
    virtual std::string Format(const LogRecord& record) const = 0;
};

// 默认格式支持 {time} {level} {logger} {thread} {fiber} {file} {line}
// {function} {message} 标记；未知标记会原样保留，便于用户逐步迁移格式。
class PatternFormatter final : public LogFormatter {
public:
    explicit PatternFormatter(std::string pattern =
                                  "{time} [{level}] {logger} "
                                  "({file}:{line}) {message}\n");
    std::string Format(const LogRecord& record) const override;
    const std::string& pattern() const noexcept { return m_pattern; }
    bool HasError() const noexcept { return m_error; }

private:
    std::string m_pattern;
    bool m_error{false};
};

class LogSink {
public:
    using ptr = std::shared_ptr<LogSink>;
    virtual ~LogSink() = default;
    virtual void Write(const LogRecord& record,
                       std::string_view formatted) = 0;
    virtual void Flush() {}
    virtual bool Reopen() { return true; }
};

class FileSink : public LogSink {
public:
    explicit FileSink(std::string path);
    void Write(const LogRecord& record, std::string_view formatted) override;
    void Flush() override;
    bool Reopen() override;
    const std::string& path() const noexcept { return m_path; }

protected:
    std::string m_path;
    std::mutex m_mutex;
    std::unique_ptr<std::ostream> m_stream;
};

class RotatingFileSink final : public FileSink {
public:
    RotatingFileSink(std::string path, std::size_t max_bytes,
                     std::size_t max_files = 3);
    void Write(const LogRecord& record, std::string_view formatted) override;

private:
    std::size_t m_max_bytes;
    std::size_t m_max_files;
    std::size_t m_bytes{0};
};

class StdoutSink final : public LogSink {
public:
    void Write(const LogRecord& record, std::string_view formatted) override;

private:
    std::mutex m_mutex;
};

class StderrSink final : public LogSink {
public:
    void Write(const LogRecord& record, std::string_view formatted) override;

private:
    std::mutex m_mutex;
};

class MemorySink final : public LogSink {
public:
    void Write(const LogRecord& record, std::string_view formatted) override;
    std::vector<std::string> Snapshot() const;
    void Clear();

private:
    mutable std::mutex m_mutex;
    std::vector<std::string> m_lines;
};

// 兼容 spdlog/fmt 等第三方系统的边界：调用方只需在回调里转发格式化文本。
class CallbackSink final : public LogSink {
public:
    using Callback = std::function<void(const LogRecord&, std::string_view)>;
    explicit CallbackSink(Callback callback) : m_callback(std::move(callback)) {}
    void Write(const LogRecord& record, std::string_view formatted) override;

private:
    Callback m_callback;
    std::mutex m_mutex;
};

class LogItemWorker {
public:
    using ptr = std::shared_ptr<LogItemWorker>;
    LogItemWorker(LogFilter::ptr filter, LogFormatter::ptr formatter,
                  LogSink::ptr sink);
    bool Submit(const LogRecord& record);
    void SetFilter(LogFilter::ptr filter);
    void SetMinimumLevel(Level level);
    void SetFormatter(LogFormatter::ptr formatter);
    void SetSink(LogSink::ptr sink);
    void Flush();
    LogSink::ptr Sink() const;

private:
    mutable std::mutex m_mutex;
    LogFilter::ptr m_filter;
    LogFormatter::ptr m_formatter;
    LogSink::ptr m_sink;
};

class Logger final : public std::enable_shared_from_this<Logger> {
public:
    using ptr = std::shared_ptr<Logger>;
    explicit Logger(std::string name);

    const std::string& name() const noexcept { return m_name; }
    void SetLevel(Level level) noexcept;
    Level level() const noexcept;
    bool ShouldLog(Level level) const noexcept;
    void AddWorker(LogItemWorker::ptr worker);
    void AddDefaultWorker(LogItemWorker::ptr worker);
    void ConfigureDefaults(std::vector<LogItemWorker::ptr> workers);
    void ClearWorkers();
    void SetParent(Logger::ptr parent);
    void SetPropagate(bool propagate) noexcept;
    bool propagate() const noexcept;
    void Flush() const;
    void Log(LogRecord record) const;

private:
    std::string m_name;
    mutable std::mutex m_mutex;
    Level m_level{Level::Warn};
    std::vector<LogItemWorker::ptr> m_workers;
    std::vector<LogItemWorker::ptr> m_default_workers;
    std::vector<LogItemWorker::ptr> m_custom_workers;
    std::weak_ptr<Logger> m_parent;
    bool m_propagate{false};
};

class LoggerManager final {
public:
    static LoggerManager& Instance();
    Logger::ptr Get(const std::string& name);
    Logger::ptr Root();
    void Configure(Level level, bool stdout_enabled, const std::string& directory,
                   const std::string& file, const std::string& pattern = {});
    void ConfigureLogger(const std::string& name, Level level, bool propagate,
                         std::vector<LogItemWorker::ptr> workers = {});
    void Remove(const std::string& name);
    std::vector<Logger::ptr> List() const;
    void Flush();

private:
    LoggerManager();
    mutable std::mutex m_mutex;
    std::vector<Logger::ptr> m_loggers;
    Logger::ptr m_root;
    Level m_level{Level::Warn};
    bool m_stdout_enabled{false};
    std::string m_directory{"log"};
    std::string m_file{"go2cpp.log"};
    std::string m_pattern;
};

class LogLine final {
public:
    LogLine(Logger::ptr logger, Level level, const char* file, int line,
            const char* function);
    ~LogLine() noexcept;
    std::ostream& stream() noexcept { return m_stream; }

private:
    Logger::ptr m_logger;
    LogRecord m_record;
    std::ostringstream m_stream;
};

}  // namespace go2cpp::log

namespace go2cpp {
using Logger = log::Logger;
using LoggerManager = log::LoggerManager;
using LogLevel = log::Level;
}  // namespace go2cpp

// 兼容用户给出的大小写命名示例。实现仍然位于规范的 go2cpp 命名空间。
namespace GO2CPP = ::go2cpp;

#define GO2CPP_LOG_NAME(name) ::go2cpp::log::LoggerManager::Instance().Get(name)
#define GO2CPP_LOG_LEVEL(logger, level)                                             \
    ::go2cpp::log::LogLine((logger), (level), __FILE__, __LINE__, __func__).stream()
#define GO2CPP_LOG_TRACE(logger) GO2CPP_LOG_LEVEL(logger, ::go2cpp::log::Level::Trace)
#define GO2CPP_LOG_DEBUG(logger) GO2CPP_LOG_LEVEL(logger, ::go2cpp::log::Level::Debug)
#define GO2CPP_LOG_INFO(logger) GO2CPP_LOG_LEVEL(logger, ::go2cpp::log::Level::Info)
#define GO2CPP_LOG_WARN(logger) GO2CPP_LOG_LEVEL(logger, ::go2cpp::log::Level::Warn)
#define GO2CPP_LOG_ERROR(logger) GO2CPP_LOG_LEVEL(logger, ::go2cpp::log::Level::Error)
#define GO2CPP_LOG_CRITICAL(logger) GO2CPP_LOG_LEVEL(logger, ::go2cpp::log::Level::Critical)
#define GO2CPP_LOG_FATAL(logger) GO2CPP_LOG_CRITICAL(logger)
