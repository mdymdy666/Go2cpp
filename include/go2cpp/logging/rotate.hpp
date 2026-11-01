#pragma once

/**
 * @file include/go2cpp/logging/rotate.hpp
 * @brief 生产级日志轮转：按大小/时间切分、保留 N 份、gzip 归档、轮转期零丢失。
 *
 * ## 模块定位
 *
 * `log.hpp` 的 `RotatingFileSink` 只做「按大小重命名 `.1`/`.2`」这一最小动作：
 * 它在写路径里同步重命名、不压缩、不区分时间周期，也不提供任何「轮转期间
 * 数据先缓存、失败可重试」的保证。本文件是**独立叠加**在其上的运维能力：
 * 不改 `log.hpp`，通过组合 `LogSink` 接入现有日志流水线。
 *
 * ## 本文件定义什么
 *
 * - `RotationPeriod`：不轮转 / 按小时 / 按天；
 * - `RotationOptions`：大小上限、保留份数、周期、gzip 开关与级别、写缓冲、
 *   轮转时是否 fsync、可注入时钟（测试用）；
 * - `detail::FileWriter`：把 `std::FILE*` 适配成 `compress::Gzip::Writer`
 *   需要的 `streams::Writer` 契约（`WriteResult Write(const char*, size_t)`）；
 * - `RotatingLogFile`：真正的轮转写入器（线程安全、失败可重试、异步压缩）；
 * - `RotatingLogSink`：`LogSink` 适配器，可直接挂到 `Logger`/`LoggerManager`。
 *
 * ## 轮转协议（为什么不会丢日志）
 *
 * 所有写入先进入**内存缓冲** `m_pending`，只有 `fwrite` 完整成功后才从缓冲移除。
 * 因此任意时刻每条记录要么在 `m_pending`，要么已经在文件里，不存在「已从缓冲
 * 删除但尚未落盘」的中间态：
 *
 * 1. `Write(data)`：持锁把 `data` 追加到 `m_pending`；达到 `write_buffer_bytes`
 *    时批量 `fwrite` + `fflush`；
 * 2. 触发轮转（大小或周期）时：先把 `m_pending` 完整写入**旧文件**并 `fsync`
 *    （默认开启），再关闭、改名为归档、打开新文件；
 * 3. 轮转期间到来的写入在锁上排队；锁内不做压缩、不做目录清理，只有
 *    `rename(2)` 与 `fopen` 两个元数据操作，临界区是微秒级；
 * 4. 归档文件的 gzip 压缩与保留策略清理全部交给**单个后台线程**串行执行，
 *    绝不阻塞写路径；压缩先写 `.gz.tmp` 再原子 `rename`，中途失败不会留下
 *    半个 `.gz`。
 *
 * 失败路径同样是「保留数据」：`fopen` 失败时 `m_pending` 继续累积并在下次
 * `Write`/`Flush` 重试；`fwrite` 短写只移除已写出的前缀；`rename` 失败时重新
 * 打开原文件继续追加。只有磁盘长期不可写、`m_pending` 超过
 * `max_pending_bytes` 这个安全阀时才会拒收新数据，并计入 `dropped_bytes`。
 *
 * ## 文件命名与保留
 *
 * 当前文件固定为构造时给的路径（例如 `logs/app.log`）；每次轮转生成
 * `logs/app.log.YYYYMMDD-HHMMSS-NNNNN`（本地时间 + 进程内单调序号，固定宽度
 * 保证字典序即时间序）。`max_files` 指**归档份数（不含当前文件）**；后台线程
 * 按文件名排序删除最旧的超额归档。压缩开启时，完整归档先变成同名 `.gz` 再
 * 删除原文件；压缩失败则保留原文件，宁可占空间也不丢内容。
 *
 * ## 性能
 *
 * - 写路径只有一次 `std::string::append` + 每 `write_buffer_bytes` 字节一次
 *   `fwrite`/`fflush`；按大小轮转的常规路径**不读时钟**（只有轮转时读一次）；
 * - 按时间（`kHourly`/`kDaily`）轮转需要每次 `Write` 取一次当前时间（vDSO，
 *   约 20 ns）；追求极致吞吐时只开大小轮转；
 * - 压缩在后台线程；轮转临界区只包含 flush/fsync/rename/fopen。
 *
 * ## 边界与陷阱
 *
 * - `RotatingLogFile` 不是 `LogSink`：它按字节写入，不格式化、不带级别；
 *   需要接日志流水线时用 `RotatingLogSink`；
 * - `Close()`/析构会**等待**后台压缩队列清空（数据完整性优先）；进程被
 *   `SIGKILL` 时未 `Flush()` 的缓冲会丢，这是所有用户态日志的共性；
 * - `max_files == 0` 表示不保留任何归档（轮转后立即删除），不是「不清理」；
 * - `max_bytes == 0` 表示不按大小轮转；两个触发条件可以同时开启；
 * - 单条记录大于 `max_bytes` 时仍会整条写入（轮转后从空文件开始，不会拆行）；
 * - 注入的 `clock` 必须线程安全；`RotatingLogFile` 在持有写锁时调用它；
 * - 压缩线程只处理归档文件，不与写路径共享任何可变状态（除原子计数）。
 *
 * ## 阅读建议
 *
 * 先看 `RotatingLogFile::Write()` 的三步（周期检查 → 大小检查 → 缓冲追加），
 * 再看 `RotateLocked()` 的「先 flush 旧文件、再 rename、再开新文件」顺序，
 * 最后看 `CompressorLoop()` 的串行队列（压缩任务在前、清理任务在后）。
 */

#include "go2cpp/compress/byte_span.hpp"
#include "go2cpp/compress/gzip.hpp"
#include "go2cpp/logging/log.hpp"
#include "go2cpp/streams.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace go2cpp::log {

/// gzip 编解码器别名：避免与 `RotationOptions::compress` 成员名冲突。
using GzipCodec = ::go2cpp::compress::Gzip;

/**
 * @brief 时间轮转周期。
 * @details `kNone` 只按大小轮转；`kHourly`/`kDaily` 按**本地时间**的自然小时/
 *   自然日切换（不是「距上次轮转 1 小时/24 小时」）。
 */
enum class RotationPeriod : std::uint8_t {
    kNone = 0,  ///< 不按时间轮转。
    kHourly,    ///< 跨本地小时即轮转。
    kDaily,     ///< 跨本地自然日即轮转。
};

/**
 * @brief `RotatingLogFile` 的构造参数。
 *
 * ## 不变量
 * - 构造完成后所有字段只读；运行期修改需要重建对象；
 * - `max_files` 是归档份数，不含当前文件；`0` 表示轮转后立刻删除归档；
 * - `compression_level` 直接传给 `Gzip::Writer`，非法值会让压缩任务失败并
 *   计入 `compress_errors`（原文件保留，不丢日志）。
 */
struct RotationOptions {
    /// 单个文件字节上限；0 表示不按大小轮转。
    std::size_t max_bytes{10u * 1024u * 1024u};
    /// 保留的归档份数（不含当前文件）；0 表示不保留。
    std::size_t max_files{5};
    /// 时间轮转周期。
    RotationPeriod period{RotationPeriod::kNone};
    /// 是否为归档生成 gzip 压缩文件。
    bool compress{true};
    /// gzip 压缩级别；非法值在压缩时变成错误计数。
    int compression_level{::go2cpp::compress::Gzip::DefaultCompression};
    /// 内存写缓冲阈值：达到该字节数才 `fwrite`（降低系统调用次数）。
    std::size_t write_buffer_bytes{64u * 1024u};
    /// `m_pending` 的安全上限：磁盘长期不可写时超过该值就拒收新数据并计数。
    std::size_t max_pending_bytes{64u * 1024u * 1024u};
    /// 轮转前是否对旧文件 `fsync`（崩溃一致性优先，默认开启）。
    bool fsync_on_rotate{true};
    /// 是否自动创建父目录。
    bool create_dirs{true};
    /// 可注入时钟；为空时用 `std::chrono::system_clock::now()`。
    std::function<std::chrono::system_clock::time_point()> clock;
};

namespace rotate_detail {

/**
 * @brief 把 `std::FILE*` 适配成 `compress::Gzip::Writer` 需要的 Writer 契约。
 *
 * @details `Gzip::Writer<WriterT>` 要求 `WriterT::Write(const char*, size_t)`
 *   返回 `streams::WriteResult`；短写必须带 `IoErrShortWrite()`，否则压缩器
 *   会把短写当成成功继续喂数据。本类只做一次 `fwrite`，不重试、不缓冲——
 *   调用方已经在压缩循环里按块写，且 `FILE*` 自带缓冲。
 */
class FileWriter final {
public:
    /// 绑定一个已打开的 `FILE*`；生命周期由调用方保证。
    explicit FileWriter(std::FILE* file) noexcept : m_file(file) {}

    /**
     * @brief 写出全部字节。
     * @return 实际写出的字节数与错误；短写时返回 `IoErrShortWrite()`。
     */
    ::go2cpp::streams::WriteResult Write(const char* data, std::size_t size) {
        if (m_file == nullptr) {
            return ::go2cpp::streams::WriteResult{0, ::go2cpp::streams::IoErrShortWrite()};
        }
        const std::size_t written = std::fwrite(data, 1, size, m_file);
        if (written != size) {
            return ::go2cpp::streams::WriteResult{written,
                                                  ::go2cpp::streams::IoErrShortWrite()};
        }
        return ::go2cpp::streams::WriteResult{written, {}};
    }

private:
    std::FILE* m_file{nullptr};  ///< 目标文件；不拥有，由压缩函数负责关闭。
};

/**
 * @brief 把 `FILE*` 缓冲刷到内核并落盘（`fflush` + `fsync`/`_commit`）。
 * @return 全部成功返回 true；任一步失败返回 false。
 */
inline bool SyncFile(std::FILE* file) noexcept {
    if (file == nullptr) {
        return false;
    }
    if (std::fflush(file) != 0) {
        return false;
    }
#if defined(_WIN32)
    return ::_commit(::_fileno(file)) == 0;
#else
    return ::fsync(::fileno(file)) == 0;
#endif
}

}  // namespace rotate_detail

/**
 * @brief 按大小/时间轮转、保留 N 份、可选 gzip 归档的线程安全日志文件。
 *
 * ## 定位
 * 运维侧「日志轮转」的完整实现；`RotatingLogSink` 是它在日志流水线里的适配器。
 * 每次 `Write` 只做缓冲追加，真正的文件写、轮转、压缩、清理分别落在写路径
 * （持锁、微秒级）与单个后台线程（压缩/删除）。
 *
 * ## 不变量
 * - `m_pending` 中的数据一定尚未完整写入文件；`fwrite` 成功后立即从缓冲移除；
 * - `m_bytes` 是**当前文件**已写入字节数，不含 `m_pending`；
 * - 轮转顺序固定为：flush 旧文件 → fsync → close → rename → fopen 新文件；
 * - 归档文件名固定宽度、字典序等于时间序；清理只删最旧的超额归档；
 * - 后台线程独占「压缩 / 清理」两类任务，任务按 FIFO 串行执行。
 *
 * ## 线程模型
 * - `Write`/`Flush`/`Sync`/`Reopen`/`RotateNow`/`Close` 之间用 `m_mutex` 串行化；
 * - 压缩线程不获取 `m_mutex`，只读取构造期固定不变的路径/配置，并通过
 *   原子计数回报结果；
 * - 统计接口无锁（原子读），可在任意线程调用。
 */
class RotatingLogFile final {
public:
    /**
     * @brief 创建轮转写入器；不会打开文件（首次 `Write`/`Flush` 时惰性打开）。
     * @param path 当前日志文件路径。
     * @param options 轮转参数；构造后不可修改。
     */
    explicit RotatingLogFile(std::string path, RotationOptions options = {});

    /// 等价于 `Close()`：冲刷缓冲、关闭文件、等待压缩队列清空。
    ~RotatingLogFile();

    RotatingLogFile(const RotatingLogFile&) = delete;
    RotatingLogFile& operator=(const RotatingLogFile&) = delete;
    RotatingLogFile(RotatingLogFile&&) = delete;
    RotatingLogFile& operator=(RotatingLogFile&&) = delete;

    /**
     * @brief 追加一段已格式化的日志字节。
     *
     * ## 步骤
     * step1. 取锁；已 `Close()` 则直接返回 0；
     * step2. 文件未打开时惰性打开（失败不丢数据，`m_pending` 继续累积）；
     * step3. 周期检查（仅 `kHourly`/`kDaily`）：跨周期先轮转；
     * step4. 大小检查：`当前文件字节 + 待写缓冲 + 本次字节 > max_bytes` 先轮转；
     * step5. 追加到 `m_pending`，达到 `write_buffer_bytes` 时批量落盘。
     *
     * @param data 待写字节（调用方保证是完整记录，轮转绝不会拆分本次写入）。
     * @return 接受的字节数；`Close()` 后或超过 `max_pending_bytes` 时返回 0。
     * @warning 只在内存分配失败时抛出 `std::bad_alloc`；磁盘错误通过
     *          `last_error()`/`flush_errors()` 报告，不抛异常。
     */
    std::size_t Write(std::string_view data);

    /**
     * @brief 把内存缓冲写入文件并 `fflush`（不 `fsync`）。
     * @return 全部缓冲落盘返回 true；失败时未写部分保留在缓冲里。
     */
    bool Flush();

    /**
     * @brief `Flush()` + `fsync`，保证数据到达持久介质。
     * @return 全部成功返回 true。
     */
    bool Sync();

    /**
     * @brief 关闭并按原路径重新打开（不产生归档）。
     *
     * @details 用于外部 logrotate 移走当前文件后的重新绑定；缓冲会先落盘到
     *   旧文件（若仍可写），然后重新 `stat` 新文件的大小继续追加。
     */
    bool Reopen();

    /**
     * @brief 立即执行一次轮转（手动触发，例如收到 `SIGHUP`）。
     * @return 轮转成功返回 true；文件为空或 rename/打开失败返回 false。
     * @note 当前文件为空时也会生成一个空归档，便于外部工具确认轮转发生过。
     */
    bool RotateNow();

    /**
     * @brief 冲刷并关闭文件，然后等待后台压缩/清理队列处理完。
     * @details 幂等；重复调用直接返回。关闭后 `Write` 返回 0。
     */
    void Close();

    /// 是否已经 `Close()`（原子读）。
    bool closed() const noexcept { return m_closed.load(std::memory_order_acquire); }

    /// 当前日志文件路径。
    const std::string& path() const noexcept { return m_path; }
    /// 构造参数（只读）。
    const RotationOptions& options() const noexcept { return m_options; }

    /// 成功轮转次数（rename 成功才算）。
    std::uint64_t rotations() const noexcept {
        return m_rotations.load(std::memory_order_relaxed);
    }
    /// 成功压缩成 `.gz` 的归档数量。
    std::uint64_t compressed_files() const noexcept {
        return m_compressed_files.load(std::memory_order_relaxed);
    }
    /// 被保留策略删除的归档数量。
    std::uint64_t pruned_files() const noexcept {
        return m_pruned_files.load(std::memory_order_relaxed);
    }
    /// 压缩失败次数（原文件保留）。
    std::uint64_t compress_errors() const noexcept {
        return m_compress_errors.load(std::memory_order_relaxed);
    }
    /// 写盘/刷盘失败次数。
    std::uint64_t flush_errors() const noexcept {
        return m_flush_errors.load(std::memory_order_relaxed);
    }
    /// 因 `max_pending_bytes` 安全阀拒收的字节数（正常应为 0）。
    std::uint64_t dropped_bytes() const noexcept {
        return m_dropped_bytes.load(std::memory_order_relaxed);
    }
    /// 累计接受的字节数。
    std::uint64_t accepted_bytes() const noexcept {
        return m_accepted_bytes.load(std::memory_order_relaxed);
    }
    /// 当前文件已写入字节数（不含缓冲；需取锁，便于诊断）。
    std::size_t current_file_bytes() const;
    /// 内存中待写字节数（需取锁，便于诊断）。
    std::size_t pending_bytes() const;
    /// 最近一次错误文本；无错误时为空。
    std::string last_error() const;
    /// `Close()` 时仍未能落盘的字节数（正常应为 0）。
    std::uint64_t unflushed_bytes() const noexcept {
        return m_unflushed_bytes.load(std::memory_order_relaxed);
    }

private:
    /// 后台任务：压缩一个归档，或按保留策略清理。
    struct Task {
        bool compress{false};  ///< true=压缩 `path`；false=执行一次清理。
        std::string path;      ///< 待压缩的归档路径（清理任务忽略）。
    };

    /// 惰性打开当前文件；初始化 `m_bytes`/`m_period_key`。
    bool EnsureOpenLocked();
    /// 把 `m_pending` 写入当前文件并 `fflush`；短写只移除已写前缀。
    bool FlushPendingLocked();
    /// 完整轮转流程；失败时保留可写状态，不丢缓冲。
    bool RotateLocked(std::chrono::system_clock::time_point now);
    /// 判断本次写入是否会突破 `max_bytes`（只读，不轮转）。
    bool ShouldRotateBySizeLocked(std::size_t incoming) const;
    /// 计算当前时刻所属周期键（`kNone` 返回 0）。
    std::uint64_t PeriodKeyLocked(std::chrono::system_clock::time_point now) const;
    /// 生成一个不存在的归档路径（时间戳 + 单调序号）。
    std::string NextArchivePathLocked(std::chrono::system_clock::time_point now);
    /// 取当前时间（优先用注入时钟）。
    std::chrono::system_clock::time_point Now() const;
    /// 记录错误文本（调用方必须持有 `m_mutex`）。
    void SetErrorLocked(std::string message);
    /// 确保后台线程已启动（调用方必须持有 `m_mutex`）。
    void StartCompressorLocked();
    /// 停止后台线程并等待队列清空。
    void StopCompressor();
    /// 入队一个压缩任务（调用方必须持有 `m_mutex`）。
    void EnqueueCompressLocked(std::string path);
    /// 入队一个清理任务（调用方必须持有 `m_mutex`）。
    void EnqueuePruneLocked();
    /// 后台线程主循环：串行消费任务，退出前处理完剩余任务。
    void CompressorLoop();
    /// 把一个归档压缩成 `.gz`（临时文件 + 原子 rename）。
    void CompressFile(const std::string& input);
    /// 按 `max_files` 删除最旧的超额归档。
    void PruneArchives();

    /// 当前文件路径；构造后不变。
    std::string m_path;
    /// 轮转参数；构造后不变。
    RotationOptions m_options;

    /// 保护写路径与文件状态。
    mutable std::mutex m_mutex;
    /// 当前文件句柄；未打开或打开失败时为 nullptr。
    std::FILE* m_file{nullptr};
    /// 尚未完整写入文件的字节（完整性保证的核心）。
    std::string m_pending;
    /// 当前文件已写入字节数；不含 `m_pending`。
    std::size_t m_bytes{0};
    /// 当前文件所属周期键；0 表示尚未确定。
    std::uint64_t m_period_key{0};
    /// 归档序号；同一进程内单调递增，保证同秒轮转的文件名不冲突。
    std::uint64_t m_archive_seq{0};
    /// 已关闭标志。
    std::atomic<bool> m_closed{false};
    /// 最近一次错误文本；受 `m_mutex` 保护。
    std::string m_last_error;

    /// 成功轮转次数。
    std::atomic<std::uint64_t> m_rotations{0};
    /// 成功压缩数量。
    std::atomic<std::uint64_t> m_compressed_files{0};
    /// 被清理的归档数量。
    std::atomic<std::uint64_t> m_pruned_files{0};
    /// 压缩失败数量。
    std::atomic<std::uint64_t> m_compress_errors{0};
    /// 刷盘失败数量。
    std::atomic<std::uint64_t> m_flush_errors{0};
    /// 安全阀拒收字节数。
    std::atomic<std::uint64_t> m_dropped_bytes{0};
    /// 累计接受字节数。
    std::atomic<std::uint64_t> m_accepted_bytes{0};
    /// `Close()` 时未落盘字节数。
    std::atomic<std::uint64_t> m_unflushed_bytes{0};

    /// 保护后台任务队列。
    std::mutex m_task_mutex;
    /// 任务队列条件变量。
    std::condition_variable m_task_cv;
    /// 待处理任务（FIFO）。
    std::deque<Task> m_tasks;
    /// 后台线程退出标志。
    bool m_task_stop{false};
    /// 后台压缩线程；首次轮转入队时启动。
    std::thread m_compressor;
};

/**
 * @brief `RotatingLogFile` 的 `LogSink` 适配器：直接把格式化日志交给轮转器。
 *
 * ## 定位
 * 挂到现有 `Logger`/`LoggerManager` 时只需要这一个类；不复制 `log.hpp` 的
 * 任何逻辑。`WriteRaw` 也直接转发，因此可以被 `BufferedSink` 包在外层批量写。
 *
 * ## 不变量
 * - 不抛异常：底层 `Write` 的 `bad_alloc` 会被吞掉并计入 `flush_errors()`；
 * - `Reopen()` 转发给 `RotatingLogFile::Reopen()`；
 * - `Flush()` 只 `fflush`，需要落盘请直接调 `file().Sync()`。
 */
class RotatingLogSink final : public LogSink {
public:
    /**
     * @brief 创建 Sink。
     * @param path 当前日志文件路径。
     * @param options 轮转参数。
     */
    explicit RotatingLogSink(std::string path, RotationOptions options = {})
        : m_file(std::move(path), std::move(options)) {}

    /// 写入一条已格式化记录（忽略 `LogRecord` 上下文，只写字节）。
    void Write(const LogRecord& /*record*/, std::string_view formatted) override {
        try {
            m_file.Write(formatted);
        } catch (...) {
            // 日志 Sink 不能把异常抛回业务路径；内存分配失败时记录一次失败。
            m_sink_errors.fetch_add(1, std::memory_order_relaxed);
        }
    }

    /// 批量写入原始字节；与 `Write` 走同一条缓冲路径。
    bool WriteRaw(std::string_view bytes) override {
        try {
            return m_file.Write(bytes) == bytes.size();
        } catch (...) {
            m_sink_errors.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
    }

    /// 冲刷内存缓冲到文件。
    void Flush() override { m_file.Flush(); }

    /// 重新打开当前文件（外部 logrotate 移动文件后使用）。
    bool Reopen() override { return m_file.Reopen(); }

    /// 底层轮转器；用于读取统计或手动 `RotateNow()`。
    RotatingLogFile& file() noexcept { return m_file; }
    /// 底层轮转器（只读）。
    const RotatingLogFile& file() const noexcept { return m_file; }
    /// Sink 层吞掉的异常次数（正常应为 0）。
    std::uint64_t sink_errors() const noexcept {
        return m_sink_errors.load(std::memory_order_relaxed);
    }

private:
    /// 实际轮转写入器。
    RotatingLogFile m_file;
    /// Sink 层捕获的异常次数。
    std::atomic<std::uint64_t> m_sink_errors{0};
};

// ============================================================================
// RotatingLogFile 实现
// ============================================================================

inline RotatingLogFile::RotatingLogFile(std::string path, RotationOptions options)
    : m_path(std::move(path)), m_options(std::move(options)) {
    m_pending.reserve(m_options.write_buffer_bytes);
}

inline RotatingLogFile::~RotatingLogFile() { Close(); }

inline std::chrono::system_clock::time_point RotatingLogFile::Now() const {
    if (m_options.clock) {
        return m_options.clock();
    }
    return std::chrono::system_clock::now();
}

inline void RotatingLogFile::SetErrorLocked(std::string message) {
    m_last_error = std::move(message);
}

inline std::uint64_t RotatingLogFile::PeriodKeyLocked(
    std::chrono::system_clock::time_point now) const {
    if (m_options.period == RotationPeriod::kNone) {
        return 0;
    }
    const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
#if defined(_WIN32)
    if (localtime_s(&local, &seconds) != 0) {
        return 0;
    }
#else
    if (::localtime_r(&seconds, &local) == nullptr) {
        return 0;
    }
#endif
    // 年份 * 1000 + 年内第几天（1..366），小时再乘 100 叠加：固定宽度，
    // 只用于相等比较，不做时间运算。
    const std::uint64_t day_key =
        static_cast<std::uint64_t>(local.tm_year + 1900) * 1000u +
        static_cast<std::uint64_t>(local.tm_yday + 1);
    if (m_options.period == RotationPeriod::kHourly) {
        return day_key * 100u + static_cast<std::uint64_t>(local.tm_hour);
    }
    return day_key;
}

inline bool RotatingLogFile::EnsureOpenLocked() {
    if (m_file != nullptr) {
        return true;
    }
    std::error_code error;
    const std::filesystem::path path(m_path);
    const std::filesystem::path parent = path.parent_path();
    if (m_options.create_dirs && !parent.empty()) {
        std::filesystem::create_directories(parent, error);
        if (error) {
            SetErrorLocked("创建日志目录失败: " + error.message());
            return false;
        }
    }
    m_file = std::fopen(m_path.c_str(), "ab");
    if (m_file == nullptr) {
        SetErrorLocked("打开日志文件失败: " + m_path);
        return false;
    }
    // 缓冲交给 FILE 自己管理；写路径已经按 write_buffer_bytes 批量喂数据。
    (void)std::setvbuf(m_file, nullptr, _IOFBF,
                       static_cast<std::size_t>(64u * 1024u));
    // 进程重启后继续追加同一文件：阈值必须包含已有字节数，否则第一次写入
    // 会暂时突破 max_bytes。
    std::uintmax_t existing = 0;
    const std::uintmax_t size = std::filesystem::file_size(m_path, error);
    if (!error) {
        existing = size;
    }
    m_bytes = static_cast<std::size_t>(existing);
    if (m_options.period != RotationPeriod::kNone) {
        m_period_key = PeriodKeyLocked(Now());
    } else {
        m_period_key = 0;
    }
    return true;
}

inline bool RotatingLogFile::FlushPendingLocked() {
    if (m_pending.empty()) {
        return true;
    }
    if (!EnsureOpenLocked()) {
        m_flush_errors.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const std::size_t written =
        std::fwrite(m_pending.data(), 1, m_pending.size(), m_file);
    m_bytes += written;
    if (written != m_pending.size()) {
        // 短写：只丢弃已经完整写出的前缀，剩余部分下次重试，绝不丢。
        m_pending.erase(0, written);
        m_flush_errors.fetch_add(1, std::memory_order_relaxed);
        SetErrorLocked("日志写入不完整（短写 " + std::to_string(written) + "/" +
                       std::to_string(written + m_pending.size()) + " 字节）");
        return false;
    }
    m_pending.clear();
    if (std::fflush(m_file) != 0) {
        m_flush_errors.fetch_add(1, std::memory_order_relaxed);
        SetErrorLocked("刷新日志文件失败: " + m_path);
        return false;
    }
    return true;
}

inline std::string RotatingLogFile::NextArchivePathLocked(
    std::chrono::system_clock::time_point now) {
    const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
#if defined(_WIN32)
    const bool converted = localtime_s(&local, &seconds) == 0;
#else
    const bool converted = ::localtime_r(&seconds, &local) != nullptr;
#endif
    char stamp[32] = {0};
    if (converted) {
        (void)std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &local);
    } else {
        (void)std::snprintf(stamp, sizeof(stamp), "%lld",
                            static_cast<long long>(seconds));
    }
    // 同秒内可能轮转多次；序号固定 5 位保证字典序与轮转顺序一致。
    for (int attempt = 0; attempt < 1000; ++attempt) {
        char sequence[16] = {0};
        (void)std::snprintf(sequence, sizeof(sequence), "%05llu",
                            static_cast<unsigned long long>(m_archive_seq++));
        std::string candidate =
            m_path + "." + stamp + "-" + std::string(sequence);
        std::error_code error;
        if (!std::filesystem::exists(candidate, error)) {
            return candidate;
        }
    }
    return {};
}

inline bool RotatingLogFile::RotateLocked(
    std::chrono::system_clock::time_point now) {
    // step1: 先把缓冲完整写入旧文件；失败则放弃本次轮转（数据仍在缓冲）。
    if (!FlushPendingLocked()) {
        return false;
    }
    if (m_file == nullptr) {
        // 没有打开过文件（例如首次写入就触发轮转）：先打开再判断。
        if (!EnsureOpenLocked()) {
            return false;
        }
    }
    // step2: 落盘旧文件（可选），关闭。
    if (m_options.fsync_on_rotate) {
        (void)rotate_detail::SyncFile(m_file);
    }
    if (std::fclose(m_file) != 0) {
        m_flush_errors.fetch_add(1, std::memory_order_relaxed);
        SetErrorLocked("关闭日志文件失败: " + m_path);
        m_file = nullptr;
        return false;
    }
    m_file = nullptr;
    // step3: 改名为归档；失败则重新打开原文件继续写，不丢日志。
    const std::string archive = NextArchivePathLocked(now);
    if (archive.empty() || std::rename(m_path.c_str(), archive.c_str()) != 0) {
        m_flush_errors.fetch_add(1, std::memory_order_relaxed);
        SetErrorLocked("轮转改名失败: " + m_path);
        (void)EnsureOpenLocked();
        return false;
    }
    m_rotations.fetch_add(1, std::memory_order_relaxed);
    m_bytes = 0;
    m_period_key = m_options.period == RotationPeriod::kNone
                       ? 0
                       : PeriodKeyLocked(now);
    // step4: 打开新文件；失败时后续 Write 会继续把数据留在缓冲里重试。
    if (!EnsureOpenLocked()) {
        return false;
    }
    // step5: 压缩与清理交给后台线程；轮转临界区到此结束。
    if (m_options.compress) {
        EnqueueCompressLocked(archive);
    }
    EnqueuePruneLocked();
    return true;
}

inline bool RotatingLogFile::ShouldRotateBySizeLocked(
    std::size_t incoming) const {
    if (m_options.max_bytes == 0) {
        return false;
    }
    const std::size_t already = m_bytes + m_pending.size();
    if (already == 0) {
        return false;
    }
    return already + incoming > m_options.max_bytes;
}

inline std::size_t RotatingLogFile::Write(std::string_view data) {
    if (data.empty()) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_closed.load(std::memory_order_relaxed)) {
        return 0;
    }
    const std::size_t incoming = data.size();
    if (!m_file) {
        (void)EnsureOpenLocked();
    }
    // step1: 周期轮转（只有按时间轮转时才读时钟）。
    bool rotated = false;
    if (m_options.period != RotationPeriod::kNone && m_file != nullptr) {
        const auto now = Now();
        const std::uint64_t key = PeriodKeyLocked(now);
        if (m_period_key != 0 && key != m_period_key) {
            rotated = RotateLocked(now);
        }
    }
    // step2: 大小轮转。轮转后本次数据写入新文件，单条记录不会被拆分。
    if (!rotated && ShouldRotateBySizeLocked(incoming)) {
        rotated = RotateLocked(Now());
    }
    (void)rotated;
    // step3: 追加到缓冲；磁盘故障时数据继续留在内存里重试。
    if (m_pending.size() + incoming > m_options.max_pending_bytes) {
        m_dropped_bytes.fetch_add(incoming, std::memory_order_relaxed);
        SetErrorLocked("日志缓冲超过安全上限，拒收新数据");
        return 0;
    }
    m_pending.append(data.data(), incoming);
    m_accepted_bytes.fetch_add(incoming, std::memory_order_relaxed);
    // step4: 达到阈值批量落盘。
    if (m_pending.size() >= m_options.write_buffer_bytes) {
        (void)FlushPendingLocked();
    }
    return incoming;
}

inline bool RotatingLogFile::Flush() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_closed.load(std::memory_order_relaxed)) {
        return false;
    }
    const bool flushed = FlushPendingLocked();
    if (flushed && m_file != nullptr && std::fflush(m_file) != 0) {
        m_flush_errors.fetch_add(1, std::memory_order_relaxed);
        SetErrorLocked("刷新日志文件失败: " + m_path);
        return false;
    }
    return flushed;
}

inline bool RotatingLogFile::Sync() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_closed.load(std::memory_order_relaxed)) {
        return false;
    }
    if (!FlushPendingLocked()) {
        return false;
    }
    if (m_file != nullptr && !rotate_detail::SyncFile(m_file)) {
        m_flush_errors.fetch_add(1, std::memory_order_relaxed);
        SetErrorLocked("日志 fsync 失败: " + m_path);
        return false;
    }
    return true;
}

inline bool RotatingLogFile::Reopen() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_closed.load(std::memory_order_relaxed)) {
        return false;
    }
    (void)FlushPendingLocked();
    if (m_file != nullptr) {
        (void)std::fclose(m_file);
        m_file = nullptr;
    }
    m_bytes = 0;
    m_period_key = 0;
    return EnsureOpenLocked();
}

inline bool RotatingLogFile::RotateNow() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_closed.load(std::memory_order_relaxed)) {
        return false;
    }
    return RotateLocked(Now());
}

inline void RotatingLogFile::StartCompressorLocked() {
    if (m_compressor.joinable()) {
        return;
    }
    m_task_stop = false;
    m_compressor = std::thread([this]() { CompressorLoop(); });
}

inline void RotatingLogFile::EnqueueCompressLocked(std::string path) {
    StartCompressorLocked();
    {
        std::lock_guard<std::mutex> lock(m_task_mutex);
        m_tasks.push_back(Task{true, std::move(path)});
    }
    m_task_cv.notify_one();
}

inline void RotatingLogFile::EnqueuePruneLocked() {
    StartCompressorLocked();
    {
        std::lock_guard<std::mutex> lock(m_task_mutex);
        m_tasks.push_back(Task{false, {}});
    }
    m_task_cv.notify_one();
}

inline void RotatingLogFile::StopCompressor() {
    std::thread worker;
    {
        std::lock_guard<std::mutex> lock(m_task_mutex);
        m_task_stop = true;
        worker = std::move(m_compressor);
    }
    m_task_cv.notify_all();
    if (worker.joinable()) {
        if (worker.get_id() == std::this_thread::get_id()) {
            // 防御：Close 从压缩线程内部调用时不能 join 自己。
            worker.detach();
        } else {
            worker.join();
        }
    }
}

inline void RotatingLogFile::CompressorLoop() {
    for (;;) {
        Task task;
        {
            std::unique_lock<std::mutex> lock(m_task_mutex);
            m_task_cv.wait(lock, [this]() {
                return m_task_stop || !m_tasks.empty();
            });
            if (m_tasks.empty()) {
                if (m_task_stop) {
                    break;
                }
                continue;
            }
            task = std::move(m_tasks.front());
            m_tasks.pop_front();
        }
        if (task.compress) {
            CompressFile(task.path);
        } else {
            PruneArchives();
        }
    }
}

inline void RotatingLogFile::CompressFile(const std::string& input) {
    const std::string temporary = input + ".gz.tmp";
    const std::string output = input + ".gz";
    std::FILE* source = std::fopen(input.c_str(), "rb");
    if (source == nullptr) {
        m_compress_errors.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    std::FILE* destination = std::fopen(temporary.c_str(), "wb");
    if (destination == nullptr) {
        (void)std::fclose(source);
        m_compress_errors.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    bool ok = true;
    {
        rotate_detail::FileWriter writer(destination);
        GzipCodec::Writer<rotate_detail::FileWriter> gzip(
            writer, m_options.compression_level);
        std::vector<char> buffer(64u * 1024u);
        for (;;) {
            const std::size_t read =
                std::fread(buffer.data(), 1, buffer.size(), source);
            if (read > 0) {
                const std::size_t accepted = gzip.Write(
                    ::go2cpp::compress::AsBytes(buffer.data(), read));
                if (accepted != read || gzip.Err()) {
                    ok = false;
                    break;
                }
            }
            if (read < buffer.size()) {
                if (std::ferror(source) != 0) {
                    ok = false;
                }
                break;
            }
        }
        if (ok && gzip.Close()) {
            ok = false;
        }
        if (ok && !rotate_detail::SyncFile(destination)) {
            ok = false;
        }
    }
    (void)std::fclose(destination);
    (void)std::fclose(source);
    if (!ok) {
        (void)std::remove(temporary.c_str());
        m_compress_errors.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // 原子替换：先写 tmp，再 rename 成 .gz，最后才删除原文件。
    std::error_code error;
    std::filesystem::rename(temporary, output, error);
    if (error) {
        (void)std::remove(temporary.c_str());
        m_compress_errors.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    (void)std::remove(input.c_str());
    m_compressed_files.fetch_add(1, std::memory_order_relaxed);
}

inline void RotatingLogFile::PruneArchives() {
    std::error_code error;
    const std::filesystem::path path(m_path);
    const std::filesystem::path parent = path.parent_path().empty()
                                             ? std::filesystem::path(".")
                                             : path.parent_path();
    const std::string prefix = path.filename().string() + ".";
    std::vector<std::filesystem::path> archives;
    for (std::filesystem::directory_iterator it(parent, error), end;
         !error && it != end; it.increment(error)) {
        const std::string name = it->path().filename().string();
        if (name.rfind(prefix, 0) != 0) {
            continue;
        }
        // `.gz.tmp` 是压缩中间态，不能按归档计数或删除。
        if (name.size() >= 4 && name.compare(name.size() - 4, 4, ".tmp") == 0) {
            continue;
        }
        const bool compressed = name.size() >= 3 &&
                                name.compare(name.size() - 3, 3, ".gz") == 0;
        // 压缩开启时只清理「已经完成压缩」的 .gz：尚未压缩的原文件可能还在
        // 后台队列里等待，删除它会让压缩任务拿到不存在的输入并记一条
        // compress_errors。压缩关闭时归档本来就是原文件，直接按原文件清理。
        // 压缩失败留下的原文件不参与清理——宁可多占空间也不丢内容。
        if (m_options.compress != compressed) {
            continue;
        }
        std::error_code type_error;
        if (it->is_regular_file(type_error) && !type_error) {
            archives.push_back(it->path());
        }
    }
    if (m_options.max_files == 0) {
        for (const auto& archive : archives) {
            std::error_code remove_error;
            if (std::filesystem::remove(archive, remove_error) && !remove_error) {
                m_pruned_files.fetch_add(1, std::memory_order_relaxed);
            }
        }
        return;
    }
    // 文件名是「当前路径 + 固定宽度时间戳 + 序号」，字典序即轮转顺序。
    std::sort(archives.begin(), archives.end());
    if (archives.size() <= m_options.max_files) {
        return;
    }
    const std::size_t remove_count = archives.size() - m_options.max_files;
    for (std::size_t i = 0; i < remove_count; ++i) {
        std::error_code remove_error;
        if (std::filesystem::remove(archives[i], remove_error) &&
            !remove_error) {
            m_pruned_files.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

inline void RotatingLogFile::Close() {
    bool was_closed = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        was_closed = m_closed.exchange(true, std::memory_order_acq_rel);
        if (was_closed) {
            return;
        }
        const bool flushed = FlushPendingLocked();
        if (m_file != nullptr) {
            if (m_options.fsync_on_rotate || flushed) {
                (void)rotate_detail::SyncFile(m_file);
            }
            (void)std::fclose(m_file);
            m_file = nullptr;
        }
        m_unflushed_bytes.store(m_pending.size(), std::memory_order_relaxed);
    }
    // 后台线程可能正在压缩最后一个归档；Close 必须等它处理完，保证不丢内容。
    StopCompressor();
}

inline std::size_t RotatingLogFile::current_file_bytes() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_bytes;
}

inline std::size_t RotatingLogFile::pending_bytes() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_pending.size();
}

inline std::string RotatingLogFile::last_error() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_last_error;
}

}  // namespace go2cpp::log
