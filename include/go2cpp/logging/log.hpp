#pragma once

// Go2Cpp 自带的轻量日志接口。
// 日志模块只依赖 C++17 标准库，借助 Filter、Formatter、Worker、Sink 四层
// 组合实现扩展。这样既能独立使用，也能通过 CallbackSink 接入 spdlog/fmt。

/**
 * @brief 轻量日志接口（Filter / Formatter / Worker / Sink 四层组合）。
 *
 * ## 模块定位
 * 日志模块对外的唯一入口头：只依赖 C++17 标准库，通过 Filter、Formatter、
 * Worker、Sink 四层组合实现扩展；`CallbackSink` 可把日志转发给 spdlog/fmt。
 *
 * ## 本文件定义什么
 * - `Level` / `LogRecord` / `LogFilter` / `MinimumLevelFilter`；
 * - `LogFormatter` / `LogFormatter::Item` / `PatternFormatter`（占位符模式）；
 * - `LogSink` 与内置 Sink：`FileSink` / `RotatingFileSink` / `StdoutSink` /
 *   `StderrSink` / `MemorySink` / `CallbackSink`，以及批量装饰器 `BufferedSink`；
 * - `LogItemWorker` / `Logger` / `LoggerManager` / `LogLine` 与 `GO2CPP_LOG_*` 宏。
 *
 * ## 阅读建议
 * 先看 `LogItemWorker::Submit()` 的三字段快照（热路径只做一次原子读），
 * 再看 `BufferedSink` 的刷新策略与「两把锁不嵌套」的设计，最后看 `Logger::Log()`
 * 的 worker 分发与父记录器传播。
 *
 * ## 边界与陷阱
 * - **`bufio::Writer` 不自动 Flush 是刻意一致的语义**：日志的 `BufferedSink` 同样只在
 *   容量/时间条件满足或显式 `Flush()` 时刷盘；不要假设「写完就落盘」，进程崩溃前
 *   必须 `Flush()`（`LoggerManager::Flush()` 会遍历所有 Logger）；
 * - **不要在持锁期间打日志**：`fprintf`/`std::ostream` 自身会加锁并可能触发刷盘，
 *   在锁内调用会把「业务锁 -> stdio 锁 -> 磁盘 IO」串成一条链，放大临界区，
 *   还可能与正在刷盘的线程互等。本模块的 Sink 在锁内只做 `write`/`append`，
 *   刷盘放在锁外或交给调用方的显式 `Flush()`；
 * - `BufferedSink::kByTime` 是**惰性检查**：长时间没有日志时缓冲不会自动刷出；
 * - 批量写时 `WriteRaw` 的 `LogRecord` 上下文只保留最后一条（见 `BufferedSink` 注释）；
 * - `LogLine` 在析构时提交，且吞掉所有异常（日志失败不能影响业务）。
 */
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
#include <type_traits>

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
/**
 * ## 定位
 * `LogLine` 构造并填充，`LogFormatter`/`LogSink` 只读；是四层流水线（Filter →
 * Formatter → Sink）之间唯一的数据契约。测试里也直接构造它验证 Formatter。
 *
 * ## 不变量
 * - 所有字符串字段都是**自有拷贝**，不引用调用方的临时内存；
 * - `timestamp_ms` 是 Unix epoch 毫秒；`line == 0` 表示未提供位置；
 * - `thread_id`/`fiber_id` 由 `LogLine` 在构造时抓取，之后不再变化。
 */
struct LogRecord {
    Level level{Level::Info};  ///< 日志级别；LogLine 构造时写，MinimumLevelFilter 与 Formatter 读
    std::string logger;  ///< 记录器名；LogLine 从 Logger::name() 拷贝，Formatter 输出
    std::string message;  ///< 消息正文；LogLine 析构时从 ostringstream 取出，Formatter/Sink 只读
    std::string file;  ///< 调用点源文件；由 GO2CPP_LOG_* 宏传入，Formatter 输出
    std::string function;  ///< 调用点函数名；由宏传入，Formatter 的 {function} 读取
    std::uint64_t timestamp_ms{0};  ///< Unix epoch 毫秒；LogLine 构造时抓取，{time} 使用
    std::uint64_t thread_id{0};  ///< 线程 ID 哈希；LogLine 构造时抓取，{thread} 使用
    std::uint64_t fiber_id{0};  ///< Fiber ID；当前由调用方填充，未填时为 0
    std::uint64_t elapse_ms{0};  ///< 业务可填的耗时毫秒；Formatter 的 {elapse} 读取
    std::string thread_name;  ///< 线程名；调用方可选填充，{thread_name} 读取
    int line{0};  ///< 调用点行号；0 表示未提供，{line} 输出 0
};

/**
 * @brief 日志过滤器接口。
 * @details 在格式化前判断记录是否进入某个 LogItemWorker。
 */
/**
 * ## 定位
 * `LogItemWorker::Submit` 的第一阶段：在格式化之前决定记录是否进入输出。
 * `MinimumLevelFilter` 是内置实现，`LoggerManager` 为默认 worker 创建它。
 *
 * ## 不变量
 * - `Accept` 必须是**只读**且可并发调用（实现方自行保证线程安全）；
 * - 返回 false 时，Worker 不再格式化，省掉字符串构造。
 */
class LogFilter {
public:
    using ptr = std::shared_ptr<LogFilter>;
    virtual ~LogFilter() = default;
    /** @brief 判断记录是否允许继续输出。@param record 待判断日志。 */
    virtual bool Accept(const LogRecord& record) const = 0;
};

/** @brief 按最小级别过滤日志记录的线程安全实现。 */
/**
 * ## 定位
 * 默认过滤器：被 `LoggerManager::Get/Configure` 装进每个 File/Stdout worker，
 * `Logger::SetLevel` 也会同步更新它，保证级别热更新立即生效。
 *
 * ## 不变量
 * - `m_level` 是原子量，`Accept` 无锁读；
 * - `Level::Off` 等于拒绝一切（`record.level >= Off` 不可能成立）。
 */
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
    std::atomic<Level> m_level;  ///< 最低级别；构造/SetLevel 写，Accept 原子读
};

/**
 * @brief 日志文本格式化接口。
 * @details 将结构化 LogRecord 转为可写入 Sink 的文本。
 */
/**
 * ## 定位
 * `LogItemWorker::Submit` 的第二阶段：把 `LogRecord` 渲染成文本。默认实现是
 * `PatternFormatter`，用户也可实现自定义 Formatter 或注册 `Item` 扩展字段。
 *
 * ## 不变量
 * - `Format` 只读 `record`，不得修改它；
 * - 同一个 Formatter 可能被多个 worker 共享，`Format` 必须可并发调用。
 */
class LogFormatter {
public:
    using ptr = std::shared_ptr<LogFormatter>;
    virtual ~LogFormatter() = default;

    /**
     * @brief 一个可插拔的格式化项。
     * @details
     * 格式字符串被解析成多个 Item，Formatter 只负责按顺序调用这些 Item。
     * 用户可以继承该类读取 LogRecord 的扩展字段，并通过 AddFormat 注册到
     * 全局格式项工厂表。例如注册 "g" 后，模式 "%g" 即可由用户 Item 生成。
     * Item 对象在构造 PatternFormatter 时被创建，格式化热路径只调用 Format，
     * 不会再次解析字符串或访问注册表。
     */
/**
 * ## 定位
 * `PatternFormatter` 的组成单元：每个占位符对应一个 `Item`，由工厂在构造时
 * 创建一次；用户可继承它读取 `LogRecord` 的扩展字段并注册新格式项。
 *
 * ## 不变量
 * - `Item` 构造完成后不再解析模式、不再访问注册表（热路径只调 `Format`）；
 * - 派生类覆写 `Format` 或 `format` 之一即可；默认 `Format` 转调 `format`。
 */
    class Item {
    public:
        using ptr = std::shared_ptr<Item>;
        virtual ~Item() = default;

        /**
         * @brief 将一个日志字段追加到输出流。
         * @param[out] stream 目标输出流。
         * @param[in] record 待读取的结构化日志记录。
         */
        virtual void Format(std::ostream& stream, const LogRecord& record) const {
            // 保留小写 format 入口，兼容 Sylar 风格的用户扩展类。
            format(stream, record);
        }

        /**
         * @brief Sylar 风格的小写格式化入口。
         * @details 用户可以覆写 Format 或 format 其中一个；默认实现为空。
         */
        virtual void format(std::ostream&, const LogRecord&) const {}
    };

    /** @brief Item 工厂，参数为格式项后面的可选参数，例如 `%d{...}` 中的内容。 */
    using ItemFactory = std::function<Item::ptr(const std::string& option)>;

    /**
     * @brief 注册一个自定义格式项。
     * @param key 格式项名称，通常是一个字符，如 "g"；也支持命名项。
     * @param factory 创建 Item 的工厂，参数是格式项选项文本。
     * @return 注册成功返回 true；key 为空、工厂为空或 key 已存在时返回 false。
     * @note 内置项在首次创建 PatternFormatter 时注册；不要覆盖内置 key，
     *       如需替换请先 RemoveFormat 再注册。
     */
    static bool AddFormat(std::string key, ItemFactory factory);

    /**
     * @brief 注册不需要格式选项的简化工厂。
     * @param key 格式项名称。
     * @param factory 无参数 Item 工厂。
     * @return 注册成功返回 true；名称冲突或参数为空返回 false。
     * @details 该重载便于只输出固定业务字段的 Item，避免新手为未使用的
     *          option 参数编写样板代码；带选项的格式仍使用 ItemFactory。
     */
    static bool AddFormat(std::string key, std::function<Item::ptr()> factory);

    /** @brief 删除一个格式项注册。内置项也可以删除，删除后新 Formatter 会报告未知项。 */
    static bool RemoveFormat(const std::string& key);

    /** @brief 判断格式项是否已经注册。 */
    static bool HasFormat(const std::string& key);

    /** @brief 返回当前注册表中的格式项名称快照，主要用于调试和插件检查。 */
    static std::vector<std::string> Formats();

    /**
     * @brief 用类型直接注册格式项，减少新手编写工厂的样板代码。
     * @details 类型优先使用 `ItemType(const std::string&)`，否则使用默认构造函数。
     */
    template <class ItemType>
    static bool AddFormat(std::string key) {
        static_assert(std::is_base_of_v<Item, ItemType>,
                      "ItemType must derive from go2cpp::log::LogFormatter::Item");
        return AddFormat(std::move(key), [](const std::string& option) {
            if constexpr (std::is_constructible_v<ItemType, const std::string&>) {
                return std::make_shared<ItemType>(option);
            } else {
                return std::make_shared<ItemType>();
            }
        });
    }

    /** @brief Sylar 风格的注册别名。 */
    static bool addFormat(std::string key, ItemFactory factory) {
        return AddFormat(std::move(key), std::move(factory));
    }
    /** @brief Sylar 风格的无选项工厂注册别名。 */
    static bool addFormat(std::string key, std::function<Item::ptr()> factory) {
        return AddFormat(std::move(key), std::move(factory));
    }
    /** @brief 小写模板注册别名，兼容 Sylar 风格调用。 */
    template <class ItemType>
    static bool addFormat(std::string key) {
        return AddFormat<ItemType>(std::move(key));
    }

    /** @brief 格式化一条日志。@return 格式化后的文本。 */
    virtual std::string Format(const LogRecord& record) const = 0;
};

// 默认格式支持 {time} {level} {logger} {thread} {fiber} {file} {line}
// {function} {message} 标记；未知标记会原样保留，便于用户逐步迁移格式。
/** @brief 支持占位符模式的内置格式化器。 */
/**
 * ## 定位
 * 默认格式化器：`LoggerManager::Get/Configure` 用它构造 File/Stdout worker；
 * 配置项 `log.format` 就是它的占位符模式。
 *
 * ## 不变量
 * - `m_items` 在构造后不可变，`Format` 只顺序调用；
 * - 未知占位符原样保留并置 `m_error`，配置系统可据此拒绝错误模式。
 */
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
    /**
     * @brief 返回构造时是否发现未知或非法占位符。
     * @details 为 true 时，未知项会原样保留；配置系统可以据此拒绝错误模式。
     */
    bool HasError() const noexcept { return m_error; }

private:
    std::string m_pattern;  ///< 占位符模式原文；构造时固定，pattern() 只读
    std::vector<Item::ptr> m_items;  ///< 解析后的 Item 链；构造时生成，Format 顺序调用
    bool m_error{false};  ///< 是否遇到未知/非法占位符；构造时写，HasError 读
};




/**
 * @brief 日志输出目标接口。
 * @details Sink 负责线程安全地写入文件、标准流或外部日志系统。
 */
/**
 * ## 定位
 * 流水线末端：`LogItemWorker::Submit` 调用 `Write`；`BufferedSink` 以装饰器
 * 形式包住具体 Sink，`FileSink`/`RotatingFileSink`/`StdoutSink`/`MemorySink`/
 * `CallbackSink` 是内置实现。
 *
 * ## 不变量
 * - 基类**不承诺线程安全**：是否需要内部锁由实现决定；`BufferedSink` 用
 *   `m_io_mutex` 串行化对同一 Sink 的调用；
 * - `WriteRaw` 默认返回 false，表示不支持批量，缓冲层会退化为逐条 `Write`；
 * - `Flush`/`Reopen` 默认无操作/成功，简单 Sink 无需覆写。
 */
class LogSink {
public:
    using ptr = std::shared_ptr<LogSink>;
    virtual ~LogSink() = default;
    /** @brief 写入已格式化记录。 */
    virtual void Write(const LogRecord& record,
                       std::string_view formatted) = 0;
    /**
     * @brief 批量写入已格式化的原始字节（可能是多条记录拼在一起）。
     * @details 缓冲型 Sink（`BufferedSink`）会优先走这条路径以摊薄系统调用。
     *          默认实现返回 false，表示"本 Sink 不支持批量"，此时缓冲层会退化为
     *          把整批文本当作一条记录交给 `Write`。**新增该虚函数不会破坏既有
     *          Sink 实现**，因为它们无需覆写。
     * @return 是否成功批量写入。
     */
    virtual bool WriteRaw(std::string_view /*bytes*/) { return false; }
    /** @brief 刷新缓冲区；默认无操作。 */
    virtual void Flush() {}
    /** @brief 重新打开输出资源；默认认为无需操作。 */
    virtual bool Reopen() { return true; }
};

/**
 * @brief 带缓冲的输出目标装饰器：把多条已格式化记录合并成批量写入。
 *
 * ## 为什么需要它
 *
 * `LogItemWorker::Submit` 是同步的：每条日志都会直接调用 `Sink::Write`，也就是
 * 每条日志一次系统调用（或一次文件锁）。高并发下这是主要开销之一。
 * `BufferedSink` 在中间加一层缓冲，把 N 条记录合并成一次批量写。
 *
 * ## 刷新策略
 *
 * - `kImmediate`：每条立即写，等价于不缓冲（便于对照测试）；
 * - `kBySize`：缓冲区达到 `capacity_bytes` 时刷；
 * - `kByTime`：距上次刷新超过 `flush_interval` 时刷 —— **注意这是惰性检查**，
 *   只在有 `Write` 或显式 `Flush` 时判断。若长时间没有日志，缓冲区不会被自动
 *   刷出；需要确定性刷新时请调用 `Flush()`；
 * - `kBySizeOrTime`：两者任一满足即刷。
 *
 * ## 并发安全（这是本类最需要想清楚的地方）
 *
 * - `m_mutex` 只保护缓冲区与统计字段，**持锁期间不做任何 IO**；
 * - `Write` 在锁内把缓冲区 `swap` 到局部变量，随即释放 `m_mutex`，再在锁外
 *   写底层 Sink；
 * - `m_io_mutex` 单独串行化"对底层 Sink 的调用"，因为 `LogSink` 的线程安全
 *   契约由具体实现决定，缓冲层不能假设它可以并发调用；
 * - 两把锁**不嵌套**（先释放 `m_mutex` 再取 `m_io_mutex`），因此不存在锁序死锁；
 * - 多个线程同时触发刷新时，只有一个线程拿到 `m_io_mutex`，其余线程的批量数据
 *   已经在各自的局部变量里，会依次写入 —— 顺序不保证，但不会丢、不会交错到
 *   半条记录（每次批量是一个完整字符串）。
 *
 * ## 已知限制
 *
 * - `WriteRaw` 得到的 record 上下文是最后一条记录的（批量写时无法保留每条
 *   record 的完整信息）。对只使用 `formatted` 的 Sink 无影响；若某个 Sink
 *   依赖 `LogRecord` 的字段区分每一条，请让该 Sink 覆写 `WriteRaw` 或不要包缓冲。
 */
/**
 * ## 定位
 * 可选装饰器：包在 `FileSink` 等真实 Sink 外层，把 N 条记录合并成一次批量写，
 * 降低高并发日志的系统调用/锁开销。`LoggerManager` 默认不启用，调用方按需包装。
 *
 * ## 状态机
 * 缓冲状态（空 / 累积中 / 达到阈值 / 刷新中 / 绕过）由 `Write`/`Flush`/
 * `FlushBatch` 驱动，状态转化表见 `docs/state-machines/BufferedSink.md`。
 *
 * ## 不变量
 * - 持 `m_mutex` 期间绝不调用底层 Sink；`m_io_mutex` 只在锁外获取；
 * - 两把锁不嵌套，因此不存在锁序死锁；
 * - `kByTime` 是惰性检查：没有新日志时不会自动刷，必须显式 `Flush()`。
 */
class BufferedSink final : public LogSink {
public:
    /** @brief 刷新策略。 */
    enum class FlushPolicy : std::uint8_t {
        kImmediate,
        kBySize,
        kByTime,
        kBySizeOrTime,
    };

    /** @brief 缓冲配置。 */
    struct Config {
        /** @brief 触发刷新的缓冲字节数（kBySize / kBySizeOrTime 使用）。 */
        std::size_t capacity_bytes{64 * 1024};
        FlushPolicy policy{FlushPolicy::kBySize};  ///< 刷新策略；ShouldFlushLocked 按它决定是否刷
        /** @brief 触发刷新的时间间隔（kByTime / kBySizeOrTime 使用）。 */
        std::chrono::milliseconds flush_interval{1000};
        /**
         * @brief 单条记录达到该字节数时绕过缓冲直接写（0 表示不绕过）。
         * @details 防止单条超大记录把缓冲区瞬间撑爆、或长时间滞留在内存里。
         */
        std::size_t bypass_threshold{0};
    };

    /**
     * @brief 创建缓冲装饰器。
     * @param inner 实际写入的目标，不能为空。
     * @param config 缓冲配置。
     */
    BufferedSink(LogSink::ptr inner, Config config);

    void Write(const LogRecord& record, std::string_view formatted) override;
    void Flush() override;
    bool Reopen() override;

    // ---- 诊断（测试与运行时观测用）----
    /** @brief 累计刷新批次数。 */
    std::size_t FlushCount() const noexcept;
    /** @brief 当前缓冲区里的字节数。 */
    std::size_t BufferedBytes() const noexcept;
    /** @brief 当前配置。 */
    const Config& config() const noexcept { return m_config; }

private:
    /// 在持锁状态下判断是否该刷。
    bool ShouldFlushLocked() const noexcept;
    /// 锁外执行一次批量写。
    void FlushBatch(std::string batch, const LogRecord& last_record);

    LogSink::ptr m_inner;  ///< 底层 Sink；Write/Flush/Reopen 转发到它，不应为空
    Config m_config;  ///< 缓冲配置拷贝；构造时固定，ShouldFlushLocked/Write 读
    /// 保护 m_buffer / m_last_flush。
    mutable std::mutex m_mutex;  ///< 保护 m_lines；Write/Snapshot/Clear 使用
    /// 串行化对 m_inner 的调用（不嵌套 m_mutex）。
    std::mutex m_io_mutex;
    std::string m_buffer;  ///< 待批量写出的文本；Write 追加，刷新时 swap 到局部变量
    std::chrono::steady_clock::time_point m_last_flush;  ///< 上次刷新时间；ShouldFlushLocked 比较，刷新后更新
    std::atomic<std::size_t> m_flush_count{0};  ///< 累计刷新批次数；FlushBatch 递增，诊断接口读取
};

/** @brief 写入普通文件的 Sink，使用互斥锁保护流对象。 */
/**
 * @brief 写入普通文件的 Sink，使用互斥锁保护流对象。
 *
 * ## 定位
 * 默认输出目标（`LoggerManager` 为每个 Logger 建一个）；`RotatingFileSink` 在它
 * 之上加滚动。`m_mutex` 串行化同一文件的写入。
 *
 * ## 不变量
 * - 锁内只做一次 `stream->write()`，不做额外 IO；
 * - `Warn` 及以上在写完后立即 `flush()`（保证错误日志能被外部收集器马上读到），
 *   `Info`/`Debug` 留在流缓冲里，避免高吞吐退化成每条一次 flush；
 * - `Reopen()` 失败后 `m_stream == nullptr`，后续 `Write` 直接丢弃。
 */
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
    /// 日志文件路径；构造时固定，Reopen()/path() 使用。
    std::string m_path;
    /// 保护 m_stream；Write/Flush/Reopen 在同一把锁内串行化。
    std::mutex m_mutex;
    /// 输出流；Reopen() 创建/替换，Write/Flush 使用，失败时为 nullptr。
    std::unique_ptr<std::ostream> m_stream;
};

/** @brief 达到容量上限后滚动备份文件的文件 Sink。 */
/**
 * @brief 达到容量上限后滚动备份文件的文件 Sink。
 *
 * ## 定位
 * 需要按大小归档的日志文件（生产常用）；继承 `FileSink` 的锁与流管理。
 *
 * ## 不变量
 * - `m_bytes` 是当前文件的**近似**字节数（构造时用 `file_size` 初始化，保证重启后
 *   不会先突破 `max_bytes`）；
 * - 滚动时先关闭流、从高到低重命名备份、再重建文件；
 * - 单条记录超过 `max_bytes` 时仍会写入（滚动后从空文件开始，不会无限重试）。
 */
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
    /// 单文件字节上限；Write 判断是否滚动（0 表示不滚动）。
    std::size_t m_max_bytes;
    /// 保留的备份数量；滚动时重命名 .1...N。
    std::size_t m_max_files;
    /// 当前文件近似字节数；构造用 file_size 初始化，Write 累加、滚动清零。
    std::size_t m_bytes{0};
};

/** @brief 将日志写入标准输出的 Sink。 */
/**
 * ## 定位
 * `LoggerManager` 在 `stdout_enabled` 为真时为每个 Logger 额外建一个 Stdout
 * worker；examples 与测试也直接用它做可见输出。
 *
 * ## 不变量
 * - 每个实例持有自己的 `m_mutex`，只保证同一 Sink 的写入不交错；
 * - `std::cout` 是进程级共享资源，多个 Sink 实例之间仍可能交错。
 */
class StdoutSink final : public LogSink {
public:
    /** @brief 线程安全地写入标准输出。 */
    void Write(const LogRecord& record, std::string_view formatted) override;

private:
    /// 串行化同一 Sink 实例的 stdout 写入；跨实例不共享此锁。
    std::mutex m_mutex;
};

/** @brief 将日志写入标准错误的 Sink。 */
/**
 * ## 定位
 * 需要把错误日志与普通输出分流时使用（例如 CLI 的 `--quiet` 只保留 stderr）。
 * 当前 `LoggerManager` 默认不创建它，由调用方手动装进 worker。
 *
 * ## 不变量
 * 同 `StdoutSink`：实例内由 `m_mutex` 串行化，跨实例不保证顺序。
 */
class StderrSink final : public LogSink {
public:
    /** @brief 线程安全地写入标准错误。 */
    void Write(const LogRecord& record, std::string_view formatted) override;

private:
    /// 串行化同一 Sink 实例的 stderr 写入；跨实例不共享此锁。
    std::mutex m_mutex;
};

/** @brief 保存日志文本到内存，主要用于测试和诊断。 */
/**
 * ## 定位
 * 测试与诊断专用：把格式化文本留在内存里，`Snapshot()` 取快照断言；
 * 不参与生产日志落盘。
 *
 * ## 不变量
 * - `m_lines` 的读写都在 `m_mutex` 内；
 * - 保存的是**格式化后的文本**，丢失 `LogRecord` 的结构化字段。
 */
class MemorySink final : public LogSink {
public:
    /** @brief 追加一条日志到内存。 */
    void Write(const LogRecord& record, std::string_view formatted) override;
    /** @brief 获取当前日志快照。 */
    std::vector<std::string> Snapshot() const;
    /** @brief 清空已保存日志。 */
    void Clear();

private:
    mutable std::mutex m_mutex;  ///< 保护 m_lines；Write/Snapshot/Clear 使用
    std::vector<std::string> m_lines;  ///< 已保存的格式化文本；Write 追加，Snapshot 拷贝，Clear 清空
};

// 兼容 spdlog/fmt 等第三方系统的边界：调用方只需在回调里转发格式化文本。
/** @brief 将日志转发给用户回调或第三方日志库的适配 Sink。 */
/**
 * ## 定位
 * 与 spdlog/fmt 等第三方日志系统的桥：调用方在回调里转发文本即可，无需实现
 * 完整 Sink。也便于测试注入自定义收集逻辑。
 *
 * ## 不变量
 * - `m_callback` 构造后不可变，`Write` 不加锁；
 * - 若回调访问共享状态，线程安全由回调自己负责（可能被多个日志线程并发调用）。
 */
class CallbackSink final : public LogSink {
public:
    using Callback = std::function<void(const LogRecord&, std::string_view)>;
    // 回调对象在构造后不可变，Write 不再为每条日志加内部锁；若回调
    // 访问共享状态，应由回调自己选择合适的同步策略。
    explicit CallbackSink(Callback callback) : m_callback(std::move(callback)) {}
    /** @brief 调用用户回调写入日志。 */
    void Write(const LogRecord& record, std::string_view formatted) override;

private:
    Callback m_callback;  ///< 用户回调；构造后不可变，Write 直接调用，线程安全由回调负责
};

/**
 * @brief 组合 Filter、Formatter、Sink 的日志工作单元。
 * @details Submit() 对单条记录执行过滤、格式化和输出；配置快照以共享指针
 *          原子替换，避免日志热路径长时间持有配置锁。
 *
 * ## 定位
 * 被 `Logger` 的 worker 列表持有，是「过滤 -> 格式化 -> 写 Sink」的一次完整流水线；
 * `LoggerManager` 为每个 Logger 建默认 worker（FileSink + 可选 StdoutSink）。
 *
 * ## 不变量
 * - 热路径只做一次 `atomic_load` 拿快照，配置变更走「复制-替换」（见 .cpp）；
 * - `Submit()` **不在锁内**调用用户 Formatter/Sink，避免自定义 Sink 再次打日志时
 *   递归锁死；
 * - 过滤未通过时返回 false 且不格式化（省掉字符串构造）。
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
/**
 * ## 定位
 * `LogItemWorker` 的不可变配置快照：热路径只 `atomic_load` 一次，后续过滤/
 * 格式化/写 Sink 都不再碰 `m_mutex`。
 *
 * ## 不变量
 * - 发布后不再修改；`SetFilter`/`SetFormatter`/`SetSink` 复制-替换整个对象；
 * - 三个指针都可能为空，`Submit` 会逐项检查。
 */
    struct Snapshot {
        LogFilter::ptr filter;  ///< 过滤阶段快照；Submit 先读它决定是否继续
        LogFormatter::ptr formatter;  ///< 格式化器快照；过滤通过后调用 Format
        LogSink::ptr sink;  ///< 输出目标快照；格式化后调用 Write
    };
    /// 保护快照替换；Submit 不取它，只有 SetFilter/SetFormatter/SetSink 使用。
    mutable std::mutex m_mutex;  ///< 保护 m_lines；Write/Snapshot/Clear 使用
    std::shared_ptr<const Snapshot> m_snapshot;  ///< 当前配置快照；atomic_load/store 读写，Submit 与 Set* 使用
};

/**
 * @brief 同名日志记录器。
 * @details 管理多个 LogItemWorker、日志级别和可选父记录器；Log() 只负责
 *          分发，具体输出由工作单元完成。
 *
 * ## 定位
 * 同名日志记录器：持有多个 worker，按级别过滤后分发；`LoggerManager::Get()` 创建，
 * `GO2CPP_LOG_*` 宏通过 `LogLine` 间接使用它。
 *
 * ## 不变量
 * - `m_level` 是原子量，`ShouldLog()` 无锁读；
 * - `m_worker_snapshot` 是原子共享指针，配置变更时整体替换，日志线程无锁读取；
 * - 只有 `m_propagate` 为真且父记录器仍存活（`weak_ptr` 未过期）才向上传播。
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
    /**
     * @brief 移除一个自定义工作单元。
     * @param worker 要移除的工作单元，按 shared_ptr 身份匹配；未找到时不改变配置。
     */
    void RemoveWorker(const LogItemWorker::ptr& worker);
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
    /// 记录器名；构造时固定，name()/LogRecord.logger 使用。
    std::string m_name;
    /// 保护 m_workers/m_default_workers/m_custom_workers/m_parent 的修改；日志热路径不取它。
    mutable std::mutex m_mutex;  ///< 保护 m_lines；Write/Snapshot/Clear 使用
    std::atomic<Level> m_level{Level::Warn};  ///< 最低级别；SetLevel 写，ShouldLog/level 原子读
    std::vector<LogItemWorker::ptr> m_workers;  ///< 默认 + 自定义 worker 的合并列表；仅在 m_mutex 内重建
    std::vector<LogItemWorker::ptr> m_default_workers;  ///< 默认 worker；ConfigureDefaults/AddDefaultWorker 维护
    std::vector<LogItemWorker::ptr> m_custom_workers;  ///< 自定义 worker；AddWorker/RemoveWorker/ClearWorkers 维护
    /// 父记录器（弱引用，避免环）；SetParent 写，Log 传播时读。
    std::weak_ptr<Logger> m_parent;
    std::atomic<bool> m_propagate{false};  ///< 是否向父记录器传播；SetPropagate 写，Log 原子读
    // m_workers 只在配置时修改；日志线程通过原子快照无锁读取。
    /// worker 列表快照；配置变更时整体替换，Log/Flush 原子读取。
    std::shared_ptr<const std::vector<LogItemWorker::ptr>> m_worker_snapshot;
};

/** @brief 全局 Logger 注册表及默认输出配置管理器。 */
/**
 * @brief 全局 Logger 注册表及默认输出配置管理器。
 *
 * ## 定位
 * 进程级单例（`Instance()`）：维护 Logger 注册表、根记录器与默认配置
 * （级别/是否 stdout/目录/文件名/格式）。
 *
 * ## 不变量
 * - 注册表操作全部在 `m_mutex` 内；
 * - `Configure()` 会重建每个 Logger 的默认 worker（已有的自定义 worker 保留）；
 * - 根 Logger 不可删除（`Remove("root")` 被忽略）。
 */
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
    /// 保护 m_loggers/m_root/默认配置；Get/Configure/Remove/List 使用。
    mutable std::mutex m_mutex;
    /// 已注册 Logger；Get/Remove/List 在 m_mutex 内读写。
    std::vector<Logger::ptr> m_loggers;
    /// 根记录器；构造时创建，其它 Logger 默认以它为父。
    Logger::ptr m_root;
    Level m_level{Level::Warn};  ///< 默认最低级别；Configure 写，Get 建新 Logger 时读
    bool m_stdout_enabled{false};  ///< 默认是否追加 Stdout worker；Configure 写，Get 读
    /// 默认日志目录；Configure 写，Get 拼文件路径。
    std::string m_directory{"log"};
    /// 默认文件名；Configure 写，Get 拼文件路径。
    std::string m_file{"go2cpp.log"};
    /// 默认格式模式；为空时用 PatternFormatter 的内置默认值。
    std::string m_pattern;  ///< 占位符模式原文；构造时固定，pattern() 只读
};

/**
 * @brief 流式日志临时对象。
 * @details 构造时记录调用位置，析构时将 stream() 中的文本提交给 Logger；
 *          级别被过滤时 stream 仍可安全使用但不会产生输出。
 *
 * ## 定位
 * `GO2CPP_LOG_INFO(logger) << ...` 的临时对象：构造时抓时间/线程/位置，析构时
 * 把流里的文本提交给 Logger。
 *
 * ## 不变量
 * - 级别被过滤时 `m_enabled == false`，流仍可写但析构不提交；
 * - 析构是 `noexcept` 且内部 `try/catch(...)`，日志异常绝不外泄；
 *   因此不要在日志表达式里做有副作用且可能抛异常的重要操作。
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
    /// 目标记录器；构造时绑定，析构时提交。
    Logger::ptr m_logger;
    /// 构造时填好级别/位置/时间，析构前补 message。
    LogRecord m_record;
    std::ostringstream m_stream;  ///< 消息拼接流；stream() 返回引用，析构时 str() 提交
    /// 级别是否通过；false 时析构不提交（但流仍可写）。
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
