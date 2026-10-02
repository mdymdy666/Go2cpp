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

/**
 * @brief 日志严重级别。
 * @details 数值从详细到严重递增；Off 禁止输出，Fatal 为 Critical 的兼容别名。
 */
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

/** @brief 将日志级别转换为稳定的 ASCII 名称。 */
const char* ToString(Level level) noexcept;
/** @brief 解析不区分大小写的级别文本。@return 成功返回 true。 */
bool ParseLevel(std::string_view text, Level* level) noexcept;

/**
 * @brief 一条日志的结构化数据。
 * @details Formatter 和 Sink 只读取此对象；字符串字段不依赖调用方临时内存。
 */
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

/**
 * @brief 日志过滤器接口。
 * @details 在格式化前判断记录是否进入某个 LogItemWorker。
 */
class LogFilter {
public:
    using ptr = std::shared_ptr<LogFilter>;
    virtual ~LogFilter() = default;
    /** @brief 判断记录是否允许继续输出。@param record 待判断日志。 */
    virtual bool Accept(const LogRecord& record) const = 0;
};

/** @brief 按最小级别过滤日志记录的线程安全实现。 */
class MinimumLevelFilter final : public LogFilter {
public:
    /** @brief 创建过滤器。@param level 允许的最低级别。 */
    explicit MinimumLevelFilter(Level level) : m_level(level) {}
    /** @brief 按当前级别判断记录。 */
    bool Accept(const LogRecord& record) const override;
    /** @brief 更新最低级别。 */
    void SetLevel(Level level) noexcept { m_level.store(level, std::memory_order_release); }
    /** @brief 读取最低级别。 */
    Level level() const noexcept { return m_level.load(std::memory_order_acquire); }

private:
    std::atomic<Level> m_level;
};

/**
 * @brief 日志文本格式化接口。
 * @details 将结构化 LogRecord 转为可写入 Sink 的文本。
 */
class LogFormatter {
public:
    using ptr = std::shared_ptr<LogFormatter>;
    virtual ~LogFormatter() = default;
    /** @brief 格式化一条日志。@return 格式化后的文本。 */
    virtual std::string Format(const LogRecord& record) const = 0;
};

// 默认格式支持 {time} {level} {logger} {thread} {fiber} {file} {line}
// {function} {message} 标记；未知标记会原样保留，便于用户逐步迁移格式。
/** @brief 支持占位符模式的内置格式化器。 */
class PatternFormatter final : public LogFormatter {
public:
    /** @brief 创建模式格式化器。@param pattern 占位符模式。 */
    explicit PatternFormatter(std::string pattern =
                                  "{time} [{level}] {logger} "
                                  "({file}:{line}) {message}\n");
    /** @brief 按模式生成一条日志文本。 */
    std::string Format(const LogRecord& record) const override;
    /** @brief 返回当前模式文本。 */
    const std::string& pattern() const noexcept { return m_pattern; }
    /** @brief 返回构造时是否发现未知或非法占位符。 */
    bool HasError() const noexcept { return m_error; }

private:
    std::string m_pattern;
    bool m_error{false};
};

/**
 * @brief 日志输出目标接口。
 * @details Sink 负责线程安全地写入文件、标准流或外部日志系统。
 */
class LogSink {
public:
    using ptr = std::shared_ptr<LogSink>;
    virtual ~LogSink() = default;
    /** @brief 写入已格式化记录。 */
    virtual void Write(const LogRecord& record,
                       std::string_view formatted) = 0;
    /** @brief 刷新缓冲区；默认无操作。 */
    virtual void Flush() {}
    /** @brief 重新打开输出资源；默认认为无需操作。 */
    virtual bool Reopen() { return true; }
};

/** @brief 写入普通文件的 Sink，使用互斥锁保护流对象。 */
class FileSink : public LogSink {
public:
    /** @brief 创建文件输出。@param path 日志文件路径。 */
    explicit FileSink(std::string path);
    /** @brief 写入一条格式化日志。 */
    void Write(const LogRecord& record, std::string_view formatted) override;
    /** @brief 刷新文件缓冲。 */
    void Flush() override;
    /** @brief 关闭并重新打开日志文件。 */
    bool Reopen() override;
    /** @brief 返回日志文件路径。 */
    const std::string& path() const noexcept { return m_path; }

protected:
    std::string m_path;
    std::mutex m_mutex;
    std::unique_ptr<std::ostream> m_stream;
};

/** @brief 达到容量上限后滚动备份文件的文件 Sink。 */
class RotatingFileSink final : public FileSink {
public:
    /**
     * @brief 创建滚动文件 Sink。
     * @param path 当前日志文件路径。
     * @param max_bytes 单文件最大字节数。
     * @param max_files 保留的历史文件数量。
     */
    RotatingFileSink(std::string path, std::size_t max_bytes,
                     std::size_t max_files = 3);
    /** @brief 写入日志并在超过容量时执行滚动。 */
    void Write(const LogRecord& record, std::string_view formatted) override;

private:
    std::size_t m_max_bytes;
    std::size_t m_max_files;
    std::size_t m_bytes{0};
};

/** @brief 将日志写入标准输出的 Sink。 */
class StdoutSink final : public LogSink {
public:
    /** @brief 线程安全地写入标准输出。 */
    void Write(const LogRecord& record, std::string_view formatted) override;

private:
    std::mutex m_mutex;
};

/** @brief 将日志写入标准错误的 Sink。 */
class StderrSink final : public LogSink {
public:
    /** @brief 线程安全地写入标准错误。 */
    void Write(const LogRecord& record, std::string_view formatted) override;

private:
    std::mutex m_mutex;
};

/** @brief 保存日志文本到内存，主要用于测试和诊断。 */
class MemorySink final : public LogSink {
public:
    /** @brief 追加一条日志到内存。 */
    void Write(const LogRecord& record, std::string_view formatted) override;
    /** @brief 获取当前日志快照。 */
    std::vector<std::string> Snapshot() const;
    /** @brief 清空已保存日志。 */
    void Clear();

private:
    mutable std::mutex m_mutex;
    std::vector<std::string> m_lines;
};

// 兼容 spdlog/fmt 等第三方系统的边界：调用方只需在回调里转发格式化文本。
/** @brief 将日志转发给用户回调或第三方日志库的适配 Sink。 */
class CallbackSink final : public LogSink {
public:
    using Callback = std::function<void(const LogRecord&, std::string_view)>;
    // 回调对象在构造后不可变，Write 不再为每条日志加内部锁；若回调
    // 访问共享状态，应由回调自己选择合适的同步策略。
    explicit CallbackSink(Callback callback) : m_callback(std::move(callback)) {}
    /** @brief 调用用户回调写入日志。 */
    void Write(const LogRecord& record, std::string_view formatted) override;

private:
    Callback m_callback;
};

/**
 * @brief 组合 Filter、Formatter、Sink 的日志工作单元。
 * @details Submit() 对单条记录执行过滤、格式化和输出；配置快照以共享指针
 *          原子替换，避免日志热路径长时间持有配置锁。
 */
class LogItemWorker {
public:
    using ptr = std::shared_ptr<LogItemWorker>;
    /** @brief 创建工作单元。@param filter 过滤器。@param formatter 格式化器。@param sink 输出目标。 */
    LogItemWorker(LogFilter::ptr filter, LogFormatter::ptr formatter,
                  LogSink::ptr sink);
    /** @brief 提交一条记录，过滤通过时写入 Sink。 */
    bool Submit(const LogRecord& record);
    /** @brief 替换过滤器。 */
    void SetFilter(LogFilter::ptr filter);
    /** @brief 设置内置最低级别过滤器。 */
    void SetMinimumLevel(Level level);
    /** @brief 替换格式化器。 */
    void SetFormatter(LogFormatter::ptr formatter);
    /** @brief 替换输出 Sink。 */
    void SetSink(LogSink::ptr sink);
    /** @brief 刷新当前 Sink。 */
    void Flush();
    /** @brief 获取当前 Sink。 */
    LogSink::ptr Sink() const;

private:
    // 热路径只做一次原子 shared_ptr 读取。配置变更时生成新的快照，
    // 因此格式化器和 Sink 不会在每条日志上争用 Worker 配置锁。
    struct Snapshot {
        LogFilter::ptr filter;
        LogFormatter::ptr formatter;
        LogSink::ptr sink;
    };
    mutable std::mutex m_mutex;
    std::shared_ptr<const Snapshot> m_snapshot;
};

/**
 * @brief 同名日志记录器。
 * @details 管理多个 LogItemWorker、日志级别和可选父记录器；Log() 只负责
 *          分发，具体输出由工作单元完成。
 */
class Logger final : public std::enable_shared_from_this<Logger> {
public:
    using ptr = std::shared_ptr<Logger>;
    /** @brief 创建指定名称的记录器。 */
    explicit Logger(std::string name);

    /** @brief 返回记录器名称。 */
    const std::string& name() const noexcept { return m_name; }
    /** @brief 设置最低输出级别。 */
    void SetLevel(Level level) noexcept;
    /** @brief 读取最低输出级别。 */
    Level level() const noexcept;
    /** @brief 判断给定级别是否应记录。 */
    bool ShouldLog(Level level) const noexcept;
    /** @brief 添加自定义工作单元。 */
    void AddWorker(LogItemWorker::ptr worker);
    /** @brief 添加默认工作单元。 */
    void AddDefaultWorker(LogItemWorker::ptr worker);
    /** @brief 替换默认工作单元集合。 */
    void ConfigureDefaults(std::vector<LogItemWorker::ptr> workers);
    /** @brief 清除自定义工作单元。 */
    void ClearWorkers();
    /** @brief 设置父记录器。 */
    void SetParent(Logger::ptr parent);
    /** @brief 设置是否向父记录器传播。 */
    void SetPropagate(bool propagate) noexcept;
    /** @brief 返回是否向父记录器传播。 */
    bool propagate() const noexcept;
    /** @brief 刷新该记录器所有输出。 */
    void Flush() const;
    /** @brief 分发一条结构化日志记录。 */
    void Log(LogRecord record) const;

private:
    std::string m_name;
    mutable std::mutex m_mutex;
    std::atomic<Level> m_level{Level::Warn};
    std::vector<LogItemWorker::ptr> m_workers;
    std::vector<LogItemWorker::ptr> m_default_workers;
    std::vector<LogItemWorker::ptr> m_custom_workers;
    std::weak_ptr<Logger> m_parent;
    std::atomic<bool> m_propagate{false};
    // m_workers 只在配置时修改；日志线程通过原子快照无锁读取。
    std::shared_ptr<const std::vector<LogItemWorker::ptr>> m_worker_snapshot;
};

/** @brief 全局 Logger 注册表及默认输出配置管理器。 */
class LoggerManager final {
public:
    /** @brief 返回进程内唯一的 LoggerManager。 */
    static LoggerManager& Instance();
    /** @brief 获取或创建指定名称的 Logger。 */
    Logger::ptr Get(const std::string& name);
    /** @brief 获取根 Logger。 */
    Logger::ptr Root();
    /** @brief 配置默认级别、输出位置和格式。 */
    void Configure(Level level, bool stdout_enabled, const std::string& directory,
                   const std::string& file, const std::string& pattern = {});
    /** @brief 配置指定 Logger 的级别、传播策略和工作单元。 */
    void ConfigureLogger(const std::string& name, Level level, bool propagate,
                         std::vector<LogItemWorker::ptr> workers = {});
    /** @brief 删除指定名称的 Logger（根 Logger 除外）。 */
    void Remove(const std::string& name);
    /** @brief 返回当前已注册 Logger 快照。 */
    std::vector<Logger::ptr> List() const;
    /** @brief 刷新所有 Logger 的输出。 */
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

/**
 * @brief 流式日志临时对象。
 * @details 构造时记录调用位置，析构时将 stream() 中的文本提交给 Logger；
 *          级别被过滤时 stream 仍可安全使用但不会产生输出。
 */
class LogLine final {
public:
    /** @brief 创建日志行。@param logger 目标记录器。@param level 级别。 */
    LogLine(Logger::ptr logger, Level level, const char* file, int line,
            const char* function);
    /** @brief 析构时提交日志记录。 */
    ~LogLine() noexcept;
    /** @brief 返回用于流式拼接消息的输出流。 */
    std::ostream& stream() noexcept { return m_stream; }

private:
    Logger::ptr m_logger;
    LogRecord m_record;
    std::ostringstream m_stream;
    bool m_enabled{false};
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
