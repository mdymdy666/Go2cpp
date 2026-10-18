/**
 * @file src/logging/log.cpp
 * @brief 轻量日志实现：格式项注册表、内置 Sink、Worker 快照与 Logger 分发。
 *
 * ## 模块定位
 * `log.hpp` 的实现：格式项解析与注册表、内置 Formatter/Sink、Worker 快照、
 * Logger 分发与 LoggerManager 配置。全部只依赖 C++17 标准库。
 *
 * ## 本文件定义什么
 * - 格式项注册表与内置项（`EnsureBuiltinFormats()` / `ParsePattern()`）；
 * - `PatternFormatter`、`MinimumLevelFilter`、`ParseLevel`；
 * - 全部 Sink 的写入与刷盘实现（含 `BufferedSink` 的批量逻辑）；
 * - `LogItemWorker` 的快照替换、`Logger`/`LoggerManager` 的生命周期与传播。
 *
 * ## 阅读建议
 * 先看 `FileSink::Write()` 的刷盘时机，再看 `BufferedSink::Write()` 的三条路径
 * （绕过 / 立即 / 缓冲）与 `FlushBatch()` 的锁外 IO，最后看 `Logger::Log()`。
 *
 * ## 边界与陷阱
 * - 所有 Sink 在锁内只做写流/追加，**不刷盘**（`Warn`+ 的 FileSink 例外）；
 * - `BufferedSink` 的两把锁不嵌套：先释放 `m_mutex` 再取 `m_io_mutex`，无锁序死锁；
 * - `LogItemWorker::Submit()` 不在锁内调用用户代码，避免自定义 Sink 重入死锁；
 * - `LogLine` 析构吞异常：日志失败不影响业务，但也意味着错误被静默。
 */
#include "go2cpp/log.hpp"

#include <filesystem>
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <sstream>
#include <thread>
#include <cstdio>
#include <unordered_set>

namespace go2cpp::log {
namespace {

std::string FormatTime(std::uint64_t timestamp_ms);

/** @brief 将一个格式项对象追加到流中。 */
class TextItem final : public LogFormatter::Item {
public:
    explicit TextItem(std::string text) : m_text(std::move(text)) {}

    void Format(std::ostream& stream, const LogRecord&) const override {
        stream << m_text;
    }

private:
    std::string m_text;  ///< 固定文本；TextItem 构造时写入，Format 只读
};

/** @brief 输出日志记录中一个简单字符串或数字字段的格式项。 */
class FieldItem final : public LogFormatter::Item {
public:
    enum class Field : std::uint8_t {
        Level,
        Logger,
        Message,
        Thread,
        Fiber,
        Elapse,
        ThreadName,
        File,
        Line,
        Function,
    };

    explicit FieldItem(Field field) : m_field(field) {}

    void Format(std::ostream& stream, const LogRecord& record) const override {
        switch (m_field) {
            case Field::Level: stream << ToString(record.level); break;
            case Field::Logger: stream << record.logger; break;
            case Field::Message: stream << record.message; break;
            case Field::Thread: stream << record.thread_id; break;
            case Field::Fiber: stream << record.fiber_id; break;
            case Field::Elapse: stream << record.elapse_ms; break;
            case Field::ThreadName: stream << record.thread_name; break;
            case Field::File: stream << record.file; break;
            case Field::Line: stream << record.line; break;
            case Field::Function: stream << record.function; break;
        }
    }

private:
    Field m_field;  ///< 要输出的字段枚举；构造时固定，Format 按它 switch
};

/** @brief 按 strftime 规则输出日志时间的格式项。 */
class DateItem final : public LogFormatter::Item {
public:
    explicit DateItem(std::string format) : m_format(std::move(format)) {}

    void Format(std::ostream& stream, const LogRecord& record) const override {
        const auto seconds = static_cast<std::time_t>(record.timestamp_ms / 1000);
        std::tm local_time{};
#if defined(_WIN32)
        localtime_s(&local_time, &seconds);
#else
        localtime_r(&seconds, &local_time);
#endif
        char buffer[128]{};
        if (std::strftime(buffer, sizeof(buffer), m_format.c_str(), &local_time) != 0) {
            stream << buffer;
        }
    }

private:
    std::string m_format;  ///< strftime 模式；DateItem 构造时固定，Format 读
};

/** @brief 输出带毫秒部分的默认时间格式项，对应 `{time}`。 */
class TimeItem final : public LogFormatter::Item {
public:
    void Format(std::ostream& stream, const LogRecord& record) const override {
        stream << FormatTime(record.timestamp_ms);
    }
};

std::unordered_map<std::string, LogFormatter::ItemFactory>& FormatRegistry() {
    static std::unordered_map<std::string, LogFormatter::ItemFactory> registry;
    return registry;
}

std::mutex& FormatRegistryMutex() {
    static std::mutex mutex;
    return mutex;
}

std::once_flag& BuiltinFormatFlag() {
    static std::once_flag flag;
    return flag;
}

/**
 * @brief 用 `std::call_once` 注册全部内置格式项，保证多线程只初始化一次。
 *
 * ## 步骤
 * step1. `call_once` 保证注册只执行一次，避免重复插入改变已有工厂；
 * step2. `add` 辅助函数每次注册都取注册表锁；
 * step3. 注册单字符别名与命名别名（`m`/`level` 等都指向同一字段）；
 * step4. 注册 `%d`/`%%`/`%T`/`%n`/`{time}` 等需要特殊处理的项。
 *
 * @note 工厂对象只捕获不可变的字段枚举，格式化热路径不查表。
 */
void EnsureBuiltinFormats() {
    // step1: 内置项必须只注册一次；重复注册会与用户覆盖冲突
    std::call_once(BuiltinFormatFlag(), [] {
        // 内置项的工厂只保存不可变的字段枚举，构造后格式化无需查表。
        // step2: 每个 add 都在锁内 emplace，已存在的 key 不会被覆盖
        const auto add = [](const char* key, FieldItem::Field field) {
            std::lock_guard<std::mutex> lock(FormatRegistryMutex());
            FormatRegistry().emplace(key, [field](const std::string&) {
                return std::make_shared<FieldItem>(field);
            });
        };
        // step3: 单字符与命名别名映射到同一 Field，保持 Sylar 兼容
        add("m", FieldItem::Field::Message);
        add("p", FieldItem::Field::Level);
        add("N", FieldItem::Field::Logger);
        add("e", FieldItem::Field::Elapse);
        add("E", FieldItem::Field::Elapse);
        add("f", FieldItem::Field::File);
        add("l", FieldItem::Field::Line);
        add("t", FieldItem::Field::Thread);
        add("F", FieldItem::Field::Fiber);
        add("c", FieldItem::Field::ThreadName);
        add("logger", FieldItem::Field::Logger);
        add("level", FieldItem::Field::Level);
        add("message", FieldItem::Field::Message);
        add("thread", FieldItem::Field::Thread);
        add("fiber", FieldItem::Field::Fiber);
        add("elapse", FieldItem::Field::Elapse);
        add("thread_name", FieldItem::Field::ThreadName);
        add("file", FieldItem::Field::File);
        add("line", FieldItem::Field::Line);
        add("function", FieldItem::Field::Function);
        // step4: 需要选项/特殊文本的项单独注册
        {
            std::lock_guard<std::mutex> lock(FormatRegistryMutex());
            FormatRegistry().emplace("d", [](const std::string& option) {
                return std::make_shared<DateItem>(
                    option.empty() ? "%Y-%m-%d %H:%M:%S" : option);
            });
            FormatRegistry().emplace("%", [](const std::string&) {
                return std::make_shared<TextItem>("%");
            });
            FormatRegistry().emplace("T", [](const std::string&) {
                return std::make_shared<TextItem>("\t");
            });
            FormatRegistry().emplace("n", [](const std::string&) {
                return std::make_shared<TextItem>("\n");
            });
            FormatRegistry().emplace("__time", [](const std::string&) {
                return std::make_shared<TimeItem>();
            });
        }
    });
}

LogFormatter::Item::ptr FindFormatItem(const std::string& key, const std::string& option) {
    LogFormatter::ItemFactory factory;
    {
        std::lock_guard<std::mutex> lock(FormatRegistryMutex());
        const auto it = FormatRegistry().find(key);
        if (it != FormatRegistry().end()) factory = it->second;
    }
    // 工厂属于用户代码，不能在注册表锁内执行，避免回调再次注册格式项时死锁。
    return factory ? factory(option) : nullptr;
}

std::string ResolveBraceKey(std::string_view token) {
    if (token == "time") return "__time";
    return std::string(token);
}

/**
 * @brief 把 `{name}` 或 `%x{opt}` 模式解析成 Item 链。
 *
 * ## 步骤
 * step1. 确保内置格式项已注册；
 * step2. 准备输出链与「待 flush 的纯文本」缓冲；
 * step3. `append_item`：查工厂；找不到就把原文字面量留在文本里并置 error；
 * step4. 扫描 `{...}` 命名占位符；闭合则解析并跳过整段；
 * step5. 普通字符累积到文本；单独的 `%` 原样保留；
 * step6. `%x{option}`：解析选项；缺 `}` 时把剩余模式原样保留并置 error；
 * step7. 收尾 flush 文本，返回 Item 链。
 *
 * ## 失败语义
 * 未知/未闭合占位符不会抛异常，而是原样保留 + `*error = true`；调用方
 * （`PatternFormatter`）据此让 `HasError()` 为真。
 */
std::vector<LogFormatter::Item::ptr> ParsePattern(const std::string& pattern, bool* error) {
    EnsureBuiltinFormats();
    // step2: items 是最终链；text 暂存相邻纯文本，避免每个字符一个 Item
    std::vector<LogFormatter::Item::ptr> items;
    std::string text;
    const auto flush_text = [&] {
        if (!text.empty()) {
            items.push_back(std::make_shared<TextItem>(std::move(text)));
            text.clear();
        }
    };
    // step3: 工厂查不到时保留字面量，便于用户看到原始模式
    const auto append_item = [&](const std::string& key, const std::string& option,
                                 const std::string& literal) {
        auto item = FindFormatItem(key, option);
        if (!item) {
            text += literal;
            if (error) *error = true;
            return;
        }
        flush_text();
        items.push_back(std::move(item));
    };
    // step4: 主扫描循环；两种语法共用同一条 Item 链
    for (std::size_t index = 0; index < pattern.size(); ++index) {
        if (pattern[index] == '{') {
            const auto end = pattern.find('}', index + 1);
            if (end != std::string::npos) {
                const auto token = pattern.substr(index + 1, end - index - 1);
                const auto key = ResolveBraceKey(token);
                append_item(key, {}, pattern.substr(index, end - index + 1));
                index = end;
                continue;
            }
        }
        // step5: 非 `%` 字符进入文本缓冲，连续文本最后合并成一个 TextItem
        if (pattern[index] != '%') {
            text.push_back(pattern[index]);
            continue;
        }
        if (index + 1 >= pattern.size()) {
            text.push_back('%');
            continue;
        }
        // step6: `%` 后可能跟 `{option}`；缺右花括号时整段保留
        const auto token_start = index;
        const char code = pattern[++index];
        std::string key(1, code);
        std::string option;
        if (index + 1 < pattern.size() && pattern[index + 1] == '{') {
            const auto end = pattern.find('}', index + 2);
            if (end == std::string::npos) {
                text.append(pattern, token_start, pattern.size() - token_start);
                if (error) *error = true;
                index = pattern.size();
                continue;
            }
            option = pattern.substr(index + 2, end - index - 2);
            index = end;
        }
        append_item(key, option, pattern.substr(token_start, index - token_start + 1));
    }
    // step7: 收尾：把最后一段纯文本物化成 TextItem
    flush_text();
    return items;
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

/// 注册用户自定义日志格式项。
/// 执行流程：
/// 1. 校验 key 和工厂函数；
/// 2. 获取格式项注册表锁；
/// 3. 拒绝重复 key，成功后发布工厂；
/// 4. 后续创建的 PatternFormatter 会把工厂实例化为 Item 链。
/// @param key 格式项名称，可以是单字符或命名字符串。
/// @param factory Item 工厂，参数为格式项附加选项。
/// @return 注册成功返回 true，参数无效或名称冲突返回 false。
bool LogFormatter::AddFormat(std::string key, ItemFactory factory) {
    if (key.empty() || !factory) return false;
    EnsureBuiltinFormats();
    std::lock_guard<std::mutex> lock(FormatRegistryMutex());
    return FormatRegistry().emplace(std::move(key), std::move(factory)).second;
}

/// 注册不带格式选项的日志格式项工厂。
/// @param key 格式项名称。
/// @param factory 无参数 Item 工厂。
/// @return 注册成功返回 true，参数无效或名称冲突返回 false。
bool LogFormatter::AddFormat(std::string key, std::function<Item::ptr()> factory) {
    if (!factory) return false;
    return AddFormat(std::move(key), [factory = std::move(factory)](const std::string&) {
        return factory();
    });
}

/// 删除日志格式项注册。
/// @param key 要删除的格式项名称。
/// @return 存在并删除返回 true，否则返回 false。
bool LogFormatter::RemoveFormat(const std::string& key) {
    EnsureBuiltinFormats();
    std::lock_guard<std::mutex> lock(FormatRegistryMutex());
    return FormatRegistry().erase(key) != 0;
}

/// 查询日志格式项是否已经注册。
/// @param key 要查询的格式项名称。
/// @return 已注册返回 true。
bool LogFormatter::HasFormat(const std::string& key) {
    EnsureBuiltinFormats();
    std::lock_guard<std::mutex> lock(FormatRegistryMutex());
    return FormatRegistry().find(key) != FormatRegistry().end();
}

/// 返回格式项注册表名称快照。
/// @return 当前注册的名称列表；调用方可以安全地在锁外遍历。
std::vector<std::string> LogFormatter::Formats() {
    EnsureBuiltinFormats();
    std::vector<std::string> result;
    std::lock_guard<std::mutex> lock(FormatRegistryMutex());
    result.reserve(FormatRegistry().size());
    for (const auto& entry : FormatRegistry()) result.push_back(entry.first);
    return result;
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

PatternFormatter::PatternFormatter(std::string pattern) : m_pattern(std::move(pattern)) {
    m_items = ParsePattern(m_pattern, &m_error);
}

std::string PatternFormatter::Format(const LogRecord& record) const {
    std::ostringstream stream;
    for (const auto& item : m_items) {
        if (item) item->Format(stream, record);
    }
    return stream.str();
}

FileSink::FileSink(std::string path) : m_path(std::move(path)) {
    Reopen();
}

/**
 * @brief 写一条日志到文件（锁内只写，Warn+ 立即刷盘）。
 *
 * ## 步骤
 * step1. 取锁；流为空或状态不 good 时直接返回（静默丢弃）；
 * step2. 一次 `stream->write()` 写出整条格式化文本；
 * step3. `Warn` 及以上立即 `flush()`，保证错误日志可被外部收集器马上读到；
 *         `Info`/`Debug` 留在流缓冲，避免高吞吐退化成每条一次系统调用。
 *
 * ## 为什么不在这里无条件 flush
 * 无条件 flush 等价于每条日志一次 `write(2)` + 一次磁盘同步；放在锁内还会把
 * 临界区拉长到 IO 完成。需要确定性落盘时由调用方显式 `Logger::Flush()`。
 */
void FileSink::Write(const LogRecord& record, std::string_view formatted) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_stream) return;
    // step2: 锁内只做一次写，不做额外 IO。
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

/**
 * @brief 创建父目录并重新打开日志文件（追加模式）。
 *
 * ## 步骤
 * step1. 取父目录并 `create_directories`（失败/异常按打开失败处理）；
 * step2. 以 `app|out` 打开，保证重启后继续追加；
 * step3. 成功才替换 `m_stream`；失败时重置为空，后续 Write 直接丢弃。
 */
bool FileSink::Reopen() {
    std::lock_guard<std::mutex> lock(m_mutex);
    try {
        // step1: 目录可能不存在；创建失败由 catch 统一处理
        const auto parent = std::filesystem::path(m_path).parent_path();
        if (!parent.empty()) std::filesystem::create_directories(parent);
        // step2: 追加模式，避免 Reopen 清空已有日志
        auto stream = std::make_unique<std::ofstream>(m_path, std::ios::app | std::ios::out);
        if (!stream->is_open()) return false;
        // step3: 只有成功打开才替换旧流
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

/**
 * @brief 写日志并在超过容量时滚动备份。
 *
 * ## 步骤
 * step1. 取锁，判断 `m_bytes + 本次长度` 是否超限；
 * step2. 超限时关闭流，从高到低重命名 `path.N-1 -> path.N`；
 * step3. 删除当前文件并以 `app` 模式重建，`m_bytes` 清零；
 * step4. 写入本次文本并累加 `m_bytes`。
 *
 * ## 边界
 * 单条记录本身就超过 `max_bytes` 时仍会写入（滚动后从空文件开始，不会死循环）。
 */
void RotatingFileSink::Write(const LogRecord&, std::string_view formatted) {
    std::lock_guard<std::mutex> lock(m_mutex);
    // step2: 先滚动再写，避免本次记录被拆到两个文件。
    if (m_max_bytes != 0 && m_bytes + formatted.size() > m_max_bytes) {
        if (auto* stream = dynamic_cast<std::ofstream*>(m_stream.get())) stream->close();
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
    if (auto* stream = dynamic_cast<std::ofstream*>(m_stream.get())) {
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

/**
 * @brief 提交一条记录：过滤 -> 格式化 -> 写 Sink。
 *
 * ## 步骤
 * step1. 原子读一次配置快照（热路径不加锁）；
 * step2. 任一组件为空或 filter 拒绝 -> 返回 false，不格式化；
 * step3. 调用户 Formatter 与 Sink —— 此时**没有持有任何 Worker 锁**；
 * step4. 返回是否真正写出。
 *
 * ## 为什么不在锁内调用用户代码
 * 自定义 Sink 可能在 Write 里再次打日志，若持锁调用会形成同一 Worker 的递归锁死。
 */
bool LogItemWorker::Submit(const LogRecord& record) {
    const auto snapshot = std::atomic_load_explicit(&m_snapshot, std::memory_order_acquire);
    if (!snapshot) return false;
    // 不在 Worker 锁内调用用户 Formatter/Sink；这样自定义 Sink 再次记录
    // 日志时不会形成同一 Worker 的递归锁死。
    // step2: 先过滤，省掉未通过记录的格式化开销。
    if (!snapshot->filter || !snapshot->formatter || !snapshot->sink ||
        !snapshot->filter->Accept(record)) return false;
    // step3: 锁外调用用户代码，允许 Sink 重入日志系统。
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

/// 移除一个自定义日志工作单元并发布新的无锁快照。
/// @param worker 要移除的工作单元，按 shared_ptr 身份比较。
/// @return 无；找不到时保持原配置不变。
void Logger::RemoveWorker(const LogItemWorker::ptr& worker) {
    if (!worker) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto iter = std::find(m_custom_workers.rbegin(), m_custom_workers.rend(), worker);
    if (iter == m_custom_workers.rend()) return;
    m_custom_workers.erase(std::next(iter).base());
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
/**
 * @brief 设置父记录器，拒绝自环与间接环。
 *
 * ## 步骤
 * step1. 取进程级父图锁，串行化所有 SetParent（父链是共享结构）；
 * step2. 从候选父节点沿 `m_parent` 向上走；
 * step3. 遇到 `this` 说明成环，直接返回、不修改；
 * step4. `visited` 去重，已有环的旧图也不会让本函数死循环；
 * step5. 检查通过后取自身锁写入 `m_parent`。
 *
 * ## 为什么需要全局锁
 * 只锁单个 Logger 无法原子地检查整条父链；两个线程同时把对方设为父会各自
 * 通过局部检查。父图锁把「检查 + 写入」变成一个临界区。
 */
void Logger::SetParent(Logger::ptr parent) {
    // 父链是有向树；禁止自环和间接环，避免 Log() 递归永不返回。
    // step1: 父图是多个 Logger 共享的结构，必须用进程级锁串行化
    static std::mutex parent_graph_mutex;
    // step1b: 先拿父图锁，再逐个取节点锁（固定顺序，避免互相等待）
    std::lock_guard<std::mutex> graph_lock(parent_graph_mutex);
    // step2: 为空表示断开父链，无需环检查
    if (parent) {
        // step4: visited 兼作「旧图已有环」的保护，避免无限上升
        const auto candidate = parent;
        std::unordered_set<const Logger*> visited;
        auto current = candidate;
        while (current) {
            if (current.get() == this ||
                !visited.insert(current.get()).second) {
                return;
            }
            std::lock_guard<std::mutex> lock(current->m_mutex);
            current = current->m_parent.lock();
        }
    }
    // step5: 检查通过才写；此时仍持有父图锁，其他 SetParent 不会插入中间态
    std::lock_guard<std::mutex> lock(m_mutex);
    m_parent = std::move(parent);
}
void Logger::SetPropagate(bool propagate) noexcept { m_propagate.store(propagate, std::memory_order_release); }
bool Logger::propagate() const noexcept { return m_propagate.load(std::memory_order_acquire); }
void Logger::Flush() const { const auto workers = std::atomic_load_explicit(&m_worker_snapshot, std::memory_order_acquire); for (const auto& worker : *workers) if (worker) worker->Flush(); }
/**
 * @brief 分发一条记录给所有 worker，并按需向父记录器传播。
 *
 * ## 步骤
 * step1. `ShouldLog()` 级别过滤（原子读，无锁）；
 * step2. 原子读 worker 快照，逐个 `Submit()`；
 * step3. 若开启 propagate，取父记录器（弱引用加锁提升）并递归 `Log()`。
 */
void Logger::Log(LogRecord record) const {
    Logger::ptr parent;
    if (!ShouldLog(record.level)) return;
    // step2: 无锁读取 worker 快照并逐个提交。
    const auto workers = std::atomic_load_explicit(&m_worker_snapshot, std::memory_order_acquire);
    for (const auto& worker : *workers) (void)worker->Submit(record);
    const bool propagate = m_propagate.load(std::memory_order_acquire);
    if (propagate) { std::lock_guard<std::mutex> lock(m_mutex); parent = m_parent.lock(); }
    // step3: 传播到父记录器（调用方负责避免环）。
    if (propagate && parent) parent->Log(std::move(record));
}

LoggerManager& LoggerManager::Instance() { static LoggerManager manager; return manager; }
LoggerManager::LoggerManager() : m_root(std::make_shared<Logger>("root")) {
    m_loggers.push_back(m_root);
    Configure(m_level, m_stdout_enabled, m_directory, m_file);
}

/**
 * @brief 按名字查找 Logger；不存在则按当前默认配置创建。
 *
 * ## 步骤
 * step1. 取注册表锁，保证「查找 + 创建 + 入表」原子；
 * step2. 线性查找同名 Logger，命中直接返回（注册表通常很小）；
 * step3. 未命中：创建 Logger 并同步当前级别；
 * step4. 用当前 pattern 建共享 `PatternFormatter`；
 * step5. 建默认 worker：FileSink 必选，stdout 可选；
 * step6. 非 root 的记录器把 root 设为父，形成传播链；
 * step7. 入表后返回，后续 `Get` 复用同一对象。
 */
Logger::ptr LoggerManager::Get(const std::string& name) {
    std::lock_guard<std::mutex> lock(m_mutex);
    // step2: 名字即身份；已存在的 Logger 不重建，保留其自定义 worker
    for (const auto& logger : m_loggers) if (logger->name() == name) return logger;
    // step3: 新 Logger 先继承当前默认级别
    auto logger = std::make_shared<Logger>(name);
    // step4: 同一 Logger 的 File/Stdout worker 共享一个 Formatter，省解析
    logger->SetLevel(m_level);
    const auto formatter = std::make_shared<PatternFormatter>(m_pattern);
    std::vector<LogItemWorker::ptr> defaults;
    defaults.push_back(std::make_shared<LogItemWorker>(std::make_shared<MinimumLevelFilter>(m_level), formatter,
                                                       std::make_shared<FileSink>((std::filesystem::path(m_directory) / m_file).string())));
    if (m_stdout_enabled) defaults.push_back(std::make_shared<LogItemWorker>(std::make_shared<MinimumLevelFilter>(m_level), formatter, std::make_shared<StdoutSink>()));
    // step5: 默认 worker 与自定义 worker 分离，后续 Configure 只重建默认部分
    logger->ConfigureDefaults(std::move(defaults));
    // step6: root 自己是根；其余默认向 root 传播
    if (name != "root") logger->SetParent(m_root);
    // step7: 入表后 Get 才能命中；shared_ptr 保证调用方持有期间不析构
    m_loggers.push_back(logger);
    return logger;
}

Logger::ptr LoggerManager::Root() { return Get("root"); }

/**
 * @brief 更新默认级别/输出位置/格式，并重建所有 Logger 的默认 worker。
 *
 * ## 步骤
 * step1. 取注册表锁，冻结 Logger 列表与默认配置；
 * step2. 空目录/空文件名/空模式回退到内置默认值；
 * step3. `make_defaults` 每次重建 FileSink（以及可选 StdoutSink）；
 * step4. 遍历所有 Logger：同步级别并整体替换默认 worker；自定义 worker 保留。
 *
 * ## 代价
 * 每次 Configure 都会新建 FileSink（重新打开文件）。热更新频率应远低于日志
 * 写入频率，否则打开文件的成本会显现。
 */
void LoggerManager::Configure(Level level, bool stdout_enabled, const std::string& directory,
                              const std::string& file, const std::string& pattern) {
    // step1: 整个重建过程持锁，避免 Get 看到一半新一半旧的默认配置
    std::lock_guard<std::mutex> lock(m_mutex);
    // step2: 空值回退保证 Configure 永远不会产出不可用的路径/模式
    m_level = level; m_stdout_enabled = stdout_enabled; m_directory = directory.empty() ? "log" : directory;
    m_file = file.empty() ? "go2cpp.log" : file; m_pattern = pattern.empty() ? "{time} [{level}] {logger} ({file}:{line}) {message}\n" : pattern;
    // step3: 每个 Logger 拿到独立 FileSink，但共享同一个 Formatter
    const auto make_defaults = [this, level]() {
        std::vector<LogItemWorker::ptr> defaults;
        const auto formatter = std::make_shared<PatternFormatter>(m_pattern);
        defaults.push_back(std::make_shared<LogItemWorker>(std::make_shared<MinimumLevelFilter>(level), formatter,
                                                           std::make_shared<FileSink>((std::filesystem::path(m_directory) / m_file).string())));
        if (m_stdout_enabled) defaults.push_back(std::make_shared<LogItemWorker>(std::make_shared<MinimumLevelFilter>(level), formatter, std::make_shared<StdoutSink>()));
        return defaults;
    };
    // step4: 只重建默认 worker；Logger 的自定义 worker 不受影响
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

/**
 * @brief 构造日志行：抓位置/时间/线程，级别未通过则不启用。
 *
 * ## 说明
 * 构造只做开销很小的字段拷贝；格式化与输出推迟到析构，避免未通过过滤时浪费。
 */
LogLine::LogLine(Logger::ptr logger, Level level, const char* file, int line, const char* function)
    : m_logger(std::move(logger)) {
    m_enabled = m_logger && m_logger->ShouldLog(level);
    if (!m_enabled) return;
    m_record.level = level; m_record.file = file ? file : ""; m_record.line = line; m_record.function = function ? function : "";
    if (m_logger) { m_record.logger = m_logger->name(); }
    m_record.timestamp_ms = CurrentTimeMs(); m_record.thread_id = CurrentThreadId();
}

/**
 * @brief 析构时把流内容提交给 Logger。
 *
 * ## 说明
 * 整体 `try/catch(...)`：日志失败不能影响业务。`m_stream.str()` 可能分配，
 * 因此这里的 catch 不是装饰。
 */
LogLine::~LogLine() noexcept {
    if (!m_enabled || !m_logger) return;
    try { m_record.message = m_stream.str(); m_logger->Log(std::move(m_record)); } catch (...) {}
}

namespace {

/// 无 record 上下文时的占位记录（批量写时底层 Sink 通常只用 formatted）。
LogRecord MakePlaceholderRecord() {
    LogRecord record;
    return record;
}

}  // namespace

BufferedSink::BufferedSink(LogSink::ptr inner, Config config)
    : m_inner(std::move(inner)), m_config(config), m_last_flush(std::chrono::steady_clock::now()) {
    if (m_config.capacity_bytes == 0) {
        // 容量为 0 会让 kBySize 立刻触发，等价于立即刷新，这里显式兜底成 1
        m_config.capacity_bytes = 1;
    }
}

bool BufferedSink::ShouldFlushLocked() const noexcept {
    const auto now = std::chrono::steady_clock::now();
    const bool size_full = m_buffer.size() >= m_config.capacity_bytes;
    const bool time_due = (now - m_last_flush) >= m_config.flush_interval;
    switch (m_config.policy) {
        case FlushPolicy::kImmediate: return true;
        case FlushPolicy::kBySize: return size_full;
        case FlushPolicy::kByTime: return time_due;
        case FlushPolicy::kBySizeOrTime: return size_full || time_due;
    }
    return false;
}

/**
 * @brief 锁外执行一次批量写。
 *
 * ## 步骤
 * step1. 空批次或 inner 为空直接返回；
 * step2. 取 `m_io_mutex`（此时 `m_mutex` 已释放，两锁不嵌套）；
 * step3. 优先 `WriteRaw()` 批量写；不支持则退化为把整批当一条 `Write()`；
 * step4. `Flush()` 并累加刷新计数。
 */
void BufferedSink::FlushBatch(std::string batch, const LogRecord& last_record) {
    if (batch.empty() || !m_inner) {
        return;
    }
    // 锁外串行化对底层 Sink 的调用。注意 m_mutex 此时已经释放，
    // 两把锁不嵌套，因此不存在死锁。
    // step2: 在此串行化对底层 Sink 的调用（Sink 的线程安全契约由实现决定）。
    std::lock_guard<std::mutex> io_lock(m_io_mutex);
    if (!m_inner->WriteRaw(batch)) {
        // 底层不支持批量：退化为把它当作一条记录写下去。
        m_inner->Write(last_record, batch);
    }
    m_inner->Flush();
    m_flush_count.fetch_add(1, std::memory_order_relaxed);
}

/**
 * @brief 写一条日志（绕过 / 立即 / 缓冲三条路径）。
 *
 * ## 步骤
 * step1. 单条记录 >= `bypass_threshold` -> 绕过缓冲直接写并 flush；
 * step2. `kImmediate` 策略 -> 直接写并 flush（等价于不缓冲）；
 * step3. 否则在 `m_mutex` 内 append，补换行，`ShouldFlushLocked()` 判定是否该刷；
 * step4. 需要刷时把缓冲 `swap` 到局部变量，出锁后 `FlushBatch()`。
 *
 * ## 并发要点
 * 锁内只碰缓冲与统计；IO 全在锁外，多个线程可各自持有自己的批次依次写入。
 */
void BufferedSink::Write(const LogRecord& record, std::string_view formatted) {
    if (!m_inner) {
        return;
    }
    // 超大单条记录：绕过缓冲，避免撑爆缓冲区或长期滞留。
    // step1: 超大单条记录绕过缓冲，避免撑爆或长期滞留。
    if (m_config.bypass_threshold > 0 && formatted.size() >= m_config.bypass_threshold) {
        std::lock_guard<std::mutex> io_lock(m_io_mutex);
        m_inner->Write(record, formatted);
        m_inner->Flush();
        m_flush_count.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // 立即策略：不缓冲。
    // step2: 立即策略不缓冲。
    if (m_config.policy == FlushPolicy::kImmediate) {
        std::lock_guard<std::mutex> io_lock(m_io_mutex);
        m_inner->Write(record, formatted);
        m_inner->Flush();
        m_flush_count.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    std::string batch;
    bool should_flush = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_buffer.append(formatted.data(), formatted.size());
        // 保证批量文本以换行结尾，便于按行解析的 Sink 使用。
        if (m_buffer.empty() || m_buffer.back() != '\n') {
            m_buffer.push_back('\n');
        }
        // step3: 只有尺寸/时间条件满足才刷（kByTime 是惰性检查）。
        should_flush = ShouldFlushLocked();
        if (should_flush) {
            batch.swap(m_buffer);
            m_last_flush = std::chrono::steady_clock::now();
        }
    }
    if (should_flush) {
        FlushBatch(std::move(batch), record);
    }
}

/**
 * @brief 把当前缓冲区整体刷出；空缓冲时只转发一次底层 `Flush`。
 *
 * ## 步骤
 * step1. 锁内 `swap` 出缓冲区并更新刷新时间，随即释放锁；
 * step2. 有数据则走 `FlushBatch`（锁外 IO）；
 * step3. 无数据也要转发 `m_inner->Flush()`，保证时间策略下底层缓冲不滞留。
 */
void BufferedSink::Flush() {
    if (!m_inner) {
        return;
    }
    // step1: 只交换缓冲区，持锁时间与批量大小无关
    std::string batch;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        batch.swap(m_buffer);
        m_last_flush = std::chrono::steady_clock::now();
    }
    // step2: 批量写走锁外路径；空批次跳过
    if (!batch.empty()) {
        FlushBatch(std::move(batch), MakePlaceholderRecord());
    } else {
        std::lock_guard<std::mutex> io_lock(m_io_mutex);
        m_inner->Flush();
    }
}

bool BufferedSink::Reopen() {
    Flush();
    if (!m_inner) {
        return false;
    }
    std::lock_guard<std::mutex> io_lock(m_io_mutex);
    return m_inner->Reopen();
}

std::size_t BufferedSink::FlushCount() const noexcept {
    return m_flush_count.load(std::memory_order_relaxed);
}

std::size_t BufferedSink::BufferedBytes() const noexcept {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_buffer.size();
}

}  // namespace go2cpp::log