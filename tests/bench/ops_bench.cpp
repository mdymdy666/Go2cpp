/**
 * @file tests/bench/ops_bench.cpp
 * @brief 周边模块（logging / config / os / container）的**汇编级**基准，
 *        以及与工业库（spdlog / fmt / rapidjson / 原生 syscall / std 容器）
 *        的同口径对比。
 *
 * ## 口径
 * - 单进程、单线程、`-O2 -DNDEBUG -pthread`，每项先跑一轮预热再计时；
 * - 计时用 `steady_clock`，取 3 轮中的最小值（best-of-3），减少调度噪声；
 * - 所有对比项**输出字节数相同或等价**，避免「少干活所以更快」的假象：
 *   - logging：同一 pattern 的等价文本（`{time} [{level}] {logger} (...)`），
 *     两侧都格式化完整一行并追加到内存缓冲；
 *   - config：同一份「60 个键」的配置内容，INI 文本 vs 等价 JSON 文本；
 *   - os：同样 64 MiB 数据，逐条 `write(2)` vs `writev(2)` 批量；
 *   - container：同一随机序列的 push/pop。
 *
 * ## 用法
 * `ops_bench <case>`，`case` 见 `main()`；无参数时跑全部并输出 `RESULT` 行。
 *
 * RESULT 行格式（供脚本解析）：
 * `RESULT <name> <ns_per_op> <ops> <bytes_per_op> <checksum>`
 */
#include "go2cpp/config/config.hpp"
#include "go2cpp/container/heap.hpp"
#include "go2cpp/container/list.hpp"
#include "go2cpp/logging/log.hpp"
#include "go2cpp/os/file.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <queue>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#if defined(OPS_BENCH_INDUSTRY)
#include "go2cpp/json.hpp"
#include <spdlog/pattern_formatter.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/spdlog.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;
using go2cpp::log::Level;
using go2cpp::log::LogItemWorker;
using go2cpp::log::LogRecord;
using go2cpp::log::Logger;
using go2cpp::log::MinimumLevelFilter;
using go2cpp::log::PatternFormatter;

std::string g_sink_buffer;  ///< 两侧共用的落点：只 append，不 flush

void ResetSinkIfLarge() {
    if (g_sink_buffer.size() > (64u << 20)) g_sink_buffer.clear();
}

double NowNs() {
    return std::chrono::duration<double, std::nano>(Clock::now().time_since_epoch()).count();
}

void Report(const char* name, double total_ns, std::size_t ops, std::size_t bytes_per_op,
            std::size_t checksum) {
    std::printf("RESULT %s %.2f %zu %zu %zu\n", name, total_ns / static_cast<double>(ops), ops,
                bytes_per_op, checksum);
    std::fflush(stdout);
}

// ---------------------------------------------------------------------------
// logging
// ---------------------------------------------------------------------------

/// 我们自己的 logger：CallbackSink 只 append，与 spdlog 的 null_mutex sink 等价。
std::shared_ptr<Logger> MakeBenchLogger(const std::string& pattern) {
    auto logger = std::make_shared<Logger>("bench");
    logger->SetLevel(Level::Trace);
    auto sink = std::make_shared<go2cpp::log::CallbackSink>(
        [](const LogRecord&, std::string_view text) {
            g_sink_buffer.append(text.data(), text.size());
            ResetSinkIfLarge();
        });
    auto worker = std::make_shared<LogItemWorker>(
        std::make_shared<MinimumLevelFilter>(Level::Trace),
        std::make_shared<PatternFormatter>(pattern), sink);
    logger->ConfigureDefaults({worker});
    return logger;
}

void BenchLogOurs(std::size_t iterations) {
    const std::string pattern = "{time} [{level}] {logger} ({file}:{line}) {message}\n";
    auto logger = MakeBenchLogger(pattern);
    const std::string user = "alice";

    // 预热
    for (std::size_t i = 0; i < 10000; ++i) {
        GO2CPP_LOG_INFO(logger) << "request id=" << i << " user=" << user << " status=ok";
    }
    g_sink_buffer.clear();

    double best = 1e30;
    for (int round = 0; round < 3; ++round) {
        g_sink_buffer.clear();
        const double start = NowNs();
        for (std::size_t i = 0; i < iterations; ++i) {
            GO2CPP_LOG_INFO(logger) << "request id=" << i << " user=" << user << " status=ok";
        }
        const double elapsed = NowNs() - start;
        best = std::min(best, elapsed);
    }
    Report("log_ours", best, iterations, g_sink_buffer.size() / std::max<std::size_t>(1, iterations),
           g_sink_buffer.size());
}

/// 只测 PatternFormatter::Format（不含 Logger 分发与 LogLine 构造）。
void BenchLogFormatOurs(std::size_t iterations) {
    const PatternFormatter formatter("{time} [{level}] {logger} ({file}:{line}) {message}\n");
    LogRecord record;
    record.level = Level::Info;
    record.logger = "bench";
    record.message = "request id=123456 user=alice status=ok";
    record.file = "tests/bench/ops_bench.cpp";
    record.function = "BenchLogOurs";
    record.line = 42;
    record.timestamp_ms = 1762000000123ull;
    record.thread_id = 139876543210ull;

    std::size_t checksum = 0;
    for (int i = 0; i < 10000; ++i) checksum += formatter.Format(record).size();

    double best = 1e30;
    for (int round = 0; round < 3; ++round) {
        checksum = 0;
        const double start = NowNs();
        for (std::size_t i = 0; i < iterations; ++i) checksum += formatter.Format(record).size();
        best = std::min(best, NowNs() - start);
    }
    Report("log_format_ours", best, iterations, checksum / std::max<std::size_t>(1, iterations),
           checksum);
}

#if defined(OPS_BENCH_INDUSTRY)
/// spdlog 的等价 sink：null_mutex（无锁），只把格式化文本 append 到内存。
class SpdMemorySink final : public spdlog::sinks::base_sink<spdlog::details::null_mutex> {
protected:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        spdlog::memory_buf_t formatted;
        formatter_->format(msg, formatted);
        g_sink_buffer.append(formatted.data(), formatted.size());
        ResetSinkIfLarge();
    }
    void flush_() override {}
};

void BenchLogSpdlog(std::size_t iterations) {
    auto sink = std::make_shared<SpdMemorySink>();
    sink->set_pattern("%Y-%m-%d %H:%M:%S.%e [%l] %n (%g:%#) %v");
    auto logger = std::make_shared<spdlog::logger>("bench", sink);
    logger->set_level(spdlog::level::trace);
    const std::string user = "alice";

    for (std::size_t i = 0; i < 10000; ++i) {
        logger->info("request id={} user={} status=ok", i, user);
    }
    g_sink_buffer.clear();

    double best = 1e30;
    for (int round = 0; round < 3; ++round) {
        g_sink_buffer.clear();
        const double start = NowNs();
        for (std::size_t i = 0; i < iterations; ++i) {
            logger->info("request id={} user={} status=ok", i, user);
        }
        best = std::min(best, NowNs() - start);
    }
    Report("log_spdlog", best, iterations,
           g_sink_buffer.size() / std::max<std::size_t>(1, iterations), g_sink_buffer.size());
}

/// spdlog 纯格式化（不含 logger 分发），与 log_format_ours 同口径。
void BenchLogFormatSpdlog(std::size_t iterations) {
    spdlog::pattern_formatter formatter("%Y-%m-%d %H:%M:%S.%e [%l] %n (%g:%#) %v");
    spdlog::details::log_msg msg;
    msg.logger_name = spdlog::string_view_t("bench", 5);
    msg.level = spdlog::level::info;
    msg.time = spdlog::log_clock::time_point(std::chrono::milliseconds(1762000000123ll));
    msg.source.filename = "tests/bench/ops_bench.cpp";
    msg.source.line = 42;
    msg.source.funcname = "BenchLogOurs";
    const std::string payload = "request id=123456 user=alice status=ok";
    msg.payload = spdlog::string_view_t(payload.data(), payload.size());

    std::size_t checksum = 0;
    spdlog::memory_buf_t formatted;
    for (int i = 0; i < 10000; ++i) {
        formatted.clear();
        formatter.format(msg, formatted);
        checksum += formatted.size();
    }
    double best = 1e30;
    for (int round = 0; round < 3; ++round) {
        checksum = 0;
        const double start = NowNs();
        for (std::size_t i = 0; i < iterations; ++i) {
            formatted.clear();
            formatter.format(msg, formatted);
            checksum += formatted.size();
        }
        best = std::min(best, NowNs() - start);
    }
    Report("log_format_spdlog", best, iterations,
           checksum / std::max<std::size_t>(1, iterations), checksum);
}
#endif  // OPS_BENCH_INDUSTRY

// ---------------------------------------------------------------------------
// config
// ---------------------------------------------------------------------------

/// 60 个键的配置文本（与 JSON 版本内容一一对应）。
std::string MakeIniText() {
    std::string text = "# go2cpp 配置基准\n\n[scheduler]\n";
    for (int i = 0; i < 20; ++i) {
        text += "key" + std::to_string(i) + " = value-" + std::to_string(i) + "\n";
    }
    text += "\n[log]\n";
    for (int i = 0; i < 20; ++i) {
        text += "log_key" + std::to_string(i) + " = 日志值 " + std::to_string(i) + "\n";
    }
    text += "\n[timer]\n";
    for (int i = 0; i < 20; ++i) {
        text += "timer_key" + std::to_string(i) + " = " + std::to_string(i * 7) + "\n";
    }
    return text;
}

void BenchIniOurs(std::size_t iterations) {
    const std::string text = MakeIniText();
    std::size_t checksum = 0;
    for (int i = 0; i < 100; ++i) {
        go2cpp::config::IniFile ini;
        ini.Parse(text);
        checksum += ini.Get("scheduler", "key7").size();
    }
    double best = 1e30;
    for (int round = 0; round < 3; ++round) {
        checksum = 0;
        const double start = NowNs();
        for (std::size_t i = 0; i < iterations; ++i) {
            go2cpp::config::IniFile ini;
            ini.Parse(text);
            checksum += ini.Get("scheduler", "key7").size();
            checksum += ini.Get("log", "log_key3").size();
            checksum += ini.Has("timer", "timer_key9") ? 1u : 0u;
        }
        best = std::min(best, NowNs() - start);
    }
    Report("ini_ours", best, iterations, text.size(), checksum);
}

/// 逐字节对拍：同一文本解析后取全部键值，返回拼接结果。
std::string IniDumpAll(const std::string& text) {
    go2cpp::config::IniFile ini;
    ini.Parse(text);
    static const char* kSections[] = {"scheduler", "log", "timer"};
    std::string out;
    for (const char* section : kSections) {
        for (int i = 0; i < 20; ++i) {
            for (const char* prefix : {"key", "log_key", "timer_key"}) {
                const std::string key = std::string(prefix) + std::to_string(i);
                if (ini.Has(section, key)) {
                    out += section;
                    out += '/';
                    out += key;
                    out += '=';
                    out += ini.Get(section, key);
                    out += '\n';
                }
            }
        }
    }
    return out;
}

#if defined(OPS_BENCH_INDUSTRY)
std::string MakeJsonText() {
    std::string text = "{\"scheduler\":{";
    for (int i = 0; i < 20; ++i) {
        if (i) text += ',';
        text += "\"key" + std::to_string(i) + "\":\"value-" + std::to_string(i) + "\"";
    }
    text += "},\"log\":{";
    for (int i = 0; i < 20; ++i) {
        if (i) text += ',';
        text += "\"log_key" + std::to_string(i) + "\":\"日志值 " + std::to_string(i) + "\"";
    }
    text += "},\"timer\":{";
    for (int i = 0; i < 20; ++i) {
        if (i) text += ',';
        text += "\"timer_key" + std::to_string(i) + "\":" + std::to_string(i * 7);
    }
    text += "}}";
    return text;
}

void BenchJsonNlohmann(std::size_t iterations) {
    const std::string text = MakeJsonText();
    std::size_t checksum = 0;
    for (int i = 0; i < 100; ++i) {
        auto parsed = go2cpp::Json::parse(text);
        checksum += parsed["scheduler"]["key7"].get<std::string>().size();
    }
    double best = 1e30;
    for (int round = 0; round < 3; ++round) {
        checksum = 0;
        const double start = NowNs();
        for (std::size_t i = 0; i < iterations; ++i) {
            auto parsed = go2cpp::Json::parse(text);
            checksum += parsed["scheduler"]["key7"].get<std::string>().size();
            checksum += parsed["log"]["log_key3"].get<std::string>().size();
            checksum += parsed["timer"].contains("timer_key9") ? 1u : 0u;
        }
        best = std::min(best, NowNs() - start);
    }
    Report("json_go2cpp", best, iterations, text.size(), checksum);
}
#endif  // OPS_BENCH_INDUSTRY

// ---------------------------------------------------------------------------
// os：syscall 批量
// ---------------------------------------------------------------------------

constexpr std::size_t kChunk = 512;               ///< 单条记录 512 B
constexpr std::size_t kBatch = 64;                ///< 一次 writev 聚合 64 条
constexpr std::size_t kOsTotalBytes = 64u << 20;  ///< 总写入 64 MiB

std::string TempPath(const char* name) {
    return std::string("/tmp/go2cpp_ops_bench_") + name + "_" + std::to_string(::getpid());
}

void BenchOsWriteNative(std::size_t /*iterations*/) {
    const std::string path = TempPath("native");
    go2cpp::os::File file;
    go2cpp::os::File::Open(path, go2cpp::os::OpenFlag::kWriteOnly | go2cpp::os::OpenFlag::kCreate |
                                      go2cpp::os::OpenFlag::kTruncate,
                           0644, &file);
    std::vector<char> buffer(kChunk, 'x');
    const std::size_t ops = kOsTotalBytes / kChunk;

    double best = 1e30;
    for (int round = 0; round < 3; ++round) {
        file.Seek(0, SEEK_SET);
        const double start = NowNs();
        for (std::size_t i = 0; i < ops; ++i) {
            const ssize_t written = file.Write(buffer.data(), buffer.size());
            if (written != static_cast<ssize_t>(kChunk)) std::abort();
        }
        best = std::min(best, NowNs() - start);
    }
    Report("os_write_single", best, ops, kChunk, kOsTotalBytes);
}

#if !defined(OPS_BASELINE)  // 基线尚无批量 API；基线只测单条 write 路径
void BenchOsWriteV(std::size_t /*iterations*/) {
    const std::string path = TempPath("writev");
    go2cpp::os::File file;
    go2cpp::os::File::Open(path, go2cpp::os::OpenFlag::kWriteOnly | go2cpp::os::OpenFlag::kCreate |
                                      go2cpp::os::OpenFlag::kTruncate,
                           0644, &file);
    std::vector<char> buffer(kChunk * kBatch, 'x');
    std::vector<struct iovec> iovecs(kBatch);
    for (std::size_t i = 0; i < kBatch; ++i) {
        iovecs[i].iov_base = buffer.data() + i * kChunk;
        iovecs[i].iov_len = kChunk;
    }
    const std::size_t ops = kOsTotalBytes / (kChunk * kBatch);

    double best = 1e30;
    for (int round = 0; round < 3; ++round) {
        file.Seek(0, SEEK_SET);
        const double start = NowNs();
        for (std::size_t i = 0; i < ops; ++i) {
            const ssize_t written = file.WriteV(iovecs.data(), static_cast<int>(iovecs.size()));
            if (written != static_cast<ssize_t>(kChunk * kBatch)) std::abort();
        }
        best = std::min(best, NowNs() - start);
    }
    Report("os_writev", best, ops, kChunk * kBatch, kOsTotalBytes);
}

void BenchOsStat(std::size_t iterations) {
    const std::string path = TempPath("stat");
    go2cpp::os::File file;
    go2cpp::os::File::Open(path, go2cpp::os::OpenFlag::kWriteOnly | go2cpp::os::OpenFlag::kCreate,
                           0644, &file);
    struct stat st {};
    struct statx stx {};
    std::size_t checksum = 0;
    for (int i = 0; i < 1000; ++i) checksum += static_cast<std::size_t>(file.Size());
    double best = 1e30;
    for (int round = 0; round < 3; ++round) {
        checksum = 0;
        const double start = NowNs();
        for (std::size_t i = 0; i < iterations; ++i) checksum += file.Stat(&st) ? 1u : 0u;
        best = std::min(best, NowNs() - start);
    }
    Report("os_fstat", best, iterations, sizeof(st), checksum);

    best = 1e30;
    for (int round = 0; round < 3; ++round) {
        checksum = 0;
        const double start = NowNs();
        for (std::size_t i = 0; i < iterations; ++i) checksum += file.Statx(&stx) ? 1u : 0u;
        best = std::min(best, NowNs() - start);
    }
    Report("os_statx", best, iterations, sizeof(stx), checksum);
    std::remove(path.c_str());
}
#endif  // OPS_BASELINE

// ---------------------------------------------------------------------------
// container
// ---------------------------------------------------------------------------

void BenchHeap(std::size_t iterations) {
    std::mt19937 rng(12345);
    std::vector<int> values(iterations);
    for (auto& value : values) value = static_cast<int>(rng());
    std::size_t checksum = 0;

    for (int i = 0; i < 1000; ++i) {
        go2cpp::container::Heap<int> heap;
        heap.Push(1);
        checksum += static_cast<std::size_t>(heap.Pop());
    }
    double best = 1e30;
    for (int round = 0; round < 3; ++round) {
        checksum = 0;
        const double start = NowNs();
        go2cpp::container::Heap<int> heap;
        heap.Reserve(values.size());
        for (int value : values) heap.Push(value);
        while (!heap.Empty()) checksum += static_cast<std::size_t>(heap.Pop());
        best = std::min(best, (NowNs() - start) / 2.0);
    }
    Report("heap_ours", best, iterations, sizeof(int), checksum);
}

void BenchPriorityQueue(std::size_t iterations) {
    std::mt19937 rng(12345);
    std::vector<int> values(iterations);
    for (auto& value : values) value = static_cast<int>(rng());
    std::size_t checksum = 0;
    for (int i = 0; i < 1000; ++i) {
        std::priority_queue<int, std::vector<int>, std::greater<int>> queue;
        queue.push(1);
        checksum += static_cast<std::size_t>(queue.top());
        queue.pop();
    }
    double best = 1e30;
    for (int round = 0; round < 3; ++round) {
        checksum = 0;
        const double start = NowNs();
        std::priority_queue<int, std::vector<int>, std::greater<int>> queue;
        for (int value : values) queue.push(value);
        while (!queue.empty()) {
            checksum += static_cast<std::size_t>(queue.top());
            queue.pop();
        }
        best = std::min(best, (NowNs() - start) / 2.0);
    }
    Report("heap_std", best, iterations, sizeof(int), checksum);
}

}  // namespace

int main(int argc, char** argv) {
    std::size_t scale = 1;
    std::string only;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg.rfind("--scale=", 0) == 0) scale = static_cast<std::size_t>(std::stoul(arg.substr(8)));
        else only = arg;
    }
    const auto want = [&](const char* name) { return only.empty() || only == name; };

    const std::size_t log_iters = 1000000 * scale;
    const std::size_t fmt_iters = 2000000 * scale;
    const std::size_t config_iters = 20000 * scale;

    if (want("log")) {
        BenchLogOurs(log_iters);
#if defined(OPS_BENCH_INDUSTRY)
        BenchLogSpdlog(log_iters);
#endif
    }
    if (want("logfmt")) {
        BenchLogFormatOurs(fmt_iters);
#if defined(OPS_BENCH_INDUSTRY)
        BenchLogFormatSpdlog(fmt_iters);
#endif
    }
    if (want("ini")) {
        BenchIniOurs(config_iters);
        // 对拍：解析结果必须与基线逐字节一致（脚本侧再比对一次）
        const std::string dump = IniDumpAll(MakeIniText());
        std::printf("CHECK ini_dump_fnv1a %zu\n",
                    static_cast<std::size_t>(std::hash<std::string>{}(dump)));
#if defined(OPS_BENCH_INDUSTRY)
        BenchJsonNlohmann(config_iters);
#endif
    }
    if (want("os")) {
        BenchOsWriteNative(0);
#if !defined(OPS_BASELINE)
        BenchOsWriteV(0);
#endif
    }
    if (want("stat")) {
#if !defined(OPS_BASELINE)
        BenchOsStat(200000 * scale);
#endif
    }
    if (want("heap")) {
        BenchHeap(1000000 * scale);
        BenchPriorityQueue(1000000 * scale);
    }
    return 0;
}
