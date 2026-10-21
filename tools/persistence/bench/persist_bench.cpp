/**
 * @file persist_bench.cpp
 * @brief 持久化原型压测：写入吞吐 / 追加延迟 / fsync 延迟 / 回读校验。
 *
 * 输出一行 TSV；字段见 `kFields`。压测口径与 tests/bench/persistence_bench.cpp
 * 对齐：同样的 1e6 条资源事件、同样的逐事件计时、同样的 /proc RSS 采样。
 * 区别是这里走持久化原型（tools/persistence），并额外输出：
 *   - append_ns / append_ev_per_sec：只算 Append 调用，不含 Finish；
 *   - finish_ns：封批、排空、最终 fdatasync、写 manifest 的时间；
 *   - read_ns / read_ev_per_sec：mmap 回读并逐条 CRC 校验；
 *   - sync_gap：两次 fdatasync 完成之间的最大间隔（持久窗口代理指标）。
 */

#include "persistence/reader.hpp"
#include "persistence/writer.hpp"
#include "replay/format.hpp"

#include <time.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using go2cpp::persistence::Engine;
using go2cpp::persistence::EventWriter;
using go2cpp::persistence::Producer;
using go2cpp::persistence::SessionReader;
using go2cpp::persistence::SyncPolicy;
using go2cpp::persistence::WriterOptions;
using go2cpp::persistence::WriterStats;
using go2cpp::replay::Event;
using go2cpp::replay::EventType;

constexpr const char* kFields =
    "engine\tn\tthreads\tsync\tbatch_bytes\tsync_bytes\tinterval_ms\t"
    "segment_bytes\tappend_ns\tappend_ev_per_sec\tfinish_ns\twall_ns\tev_per_sec\t"
    "p50_ns\tp99_ns\tp999_ns\trss_kb\thwm_kb\tdisk_bytes\tsegments\trecords\t"
    "checksum\tread_mode\tread_ns\tread_ev_per_sec\tread_bytes\tread_records\t"
    "read_ok\tcrc_ok\tseq_ok\tchecksum_ok\tfsync_count\tfsync_p50_ns\t"
    "fsync_p99_ns\tmax_sync_gap_ns\tload1\tvalid";

std::uint64_t NowNs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ULL +
           static_cast<std::uint64_t>(ts.tv_nsec);
}

double LoadAvg1() {
    double value = 0.0;
    std::ifstream in("/proc/loadavg");
    in >> value;
    return value;
}

struct ProcStats {
    std::uint64_t rss_kb{0};
    std::uint64_t hwm_kb{0};
};

ProcStats ReadProcStats() {
    ProcStats stats;
    std::ifstream in("/proc/self/status");
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("VmRSS:", 0) == 0) {
            stats.rss_kb = std::strtoull(line.c_str() + 6, nullptr, 10);
        } else if (line.rfind("VmHWM:", 0) == 0) {
            stats.hwm_kb = std::strtoull(line.c_str() + 6, nullptr, 10);
        }
    }
    return stats;
}

struct LatencyStats {
    std::uint64_t p50{0};
    std::uint64_t p99{0};
    std::uint64_t p999{0};
};

LatencyStats Summarize(std::vector<std::uint32_t>* samples) {
    LatencyStats out;
    if (samples->empty()) {
        return out;
    }
    std::sort(samples->begin(), samples->end());
    const auto pick = [samples](double q) {
        const std::size_t index = static_cast<std::size_t>(
            q * static_cast<double>(samples->size() - 1));
        return static_cast<std::uint64_t>((*samples)[index]);
    };
    out.p50 = pick(0.50);
    out.p99 = pick(0.99);
    out.p999 = pick(0.999);
    return out;
}

Event MakeBenchEvent(std::uint64_t index) {
    static constexpr EventType kTypes[] = {
        EventType::kResRwReadLock,  EventType::kResRwWriteLock,
        EventType::kResRwUnlock,    EventType::kResCvWait,
        EventType::kResCvNotify,    EventType::kResCvNotifyAll,
        EventType::kResWgAdd,       EventType::kResWgDone,
    };
    Event event;
    event.type = kTypes[index % (sizeof(kTypes) / sizeof(kTypes[0]))];
    event.gid = static_cast<std::uint32_t>(index % 64U);
    event.channel = go2cpp::replay::kNoChannel;
    event.resource_id = (index % 4096U) + 1U;
    event.actor_gid = index % 64U;
    event.target_gid = (index * 7U) % 64U;
    event.resource_result = static_cast<std::uint32_t>(index % 3U);
    event.address = 0x7f0000000000ULL + (index % 4096U) * 64U;
    event.waiters = static_cast<std::uint32_t>(index % 8U);
    event.res_fd = -1;
    event.res_bytes = static_cast<std::uint32_t>(index % 1024U);
    event.res_aux = index % 5U;
    event.value = static_cast<std::int64_t>(index);
    return event;
}

struct Options {
    Engine engine{Engine::kMpsc};
    SyncPolicy sync{SyncPolicy::kInterval};
    std::uint64_t n{1000000};
    std::uint32_t threads{1};
    std::size_t batch_bytes{256U * 1024U};
    std::size_t sync_bytes{4U * 1024U * 1024U};
    std::uint32_t interval_ms{100};
    std::size_t segment_bytes{8U * 1024U * 1024U};
    std::string path{"/tmp/g2c-persist-proto/session"};
    std::string read_mode{"scan"};
    std::uint64_t read_from{0};
    std::uint64_t read_to{UINT64_MAX};
    bool header{false};
};

bool ParseEngine(const std::string& text, Engine* engine) {
    if (text == "buffered") {
        *engine = Engine::kBuffered;
    } else if (text == "mpsc") {
        *engine = Engine::kMpsc;
    } else if (text == "segmented") {
        *engine = Engine::kSegmented;
    } else {
        return false;
    }
    return true;
}

bool ParseSync(const std::string& text, SyncPolicy* policy) {
    if (text == "none") {
        *policy = SyncPolicy::kNone;
    } else if (text == "batch") {
        *policy = SyncPolicy::kBatch;
    } else if (text == "interval") {
        *policy = SyncPolicy::kInterval;
    } else {
        return false;
    }
    return true;
}

bool ParseArgs(int argc, char** argv, Options* options, std::string* error) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto value = [&arg](const char* key) -> std::string {
            const std::string prefix = std::string("--") + key + "=";
            return arg.rfind(prefix, 0) == 0 ? arg.substr(prefix.size()) : "";
        };
        std::string v;
        if (!(v = value("engine")).empty()) {
            if (!ParseEngine(v, &options->engine)) {
                *error = "未知 engine: " + v;
                return false;
            }
        } else if (!(v = value("sync")).empty()) {
            if (!ParseSync(v, &options->sync)) {
                *error = "未知 sync: " + v;
                return false;
            }
        } else if (!(v = value("n")).empty()) {
            options->n = std::strtoull(v.c_str(), nullptr, 10);
        } else if (!(v = value("threads")).empty()) {
            options->threads = static_cast<std::uint32_t>(
                std::strtoul(v.c_str(), nullptr, 10));
        } else if (!(v = value("batch-bytes")).empty()) {
            options->batch_bytes =
                static_cast<std::size_t>(std::strtoull(v.c_str(), nullptr, 10));
        } else if (!(v = value("sync-bytes")).empty()) {
            options->sync_bytes =
                static_cast<std::size_t>(std::strtoull(v.c_str(), nullptr, 10));
        } else if (!(v = value("interval-ms")).empty()) {
            options->interval_ms = static_cast<std::uint32_t>(
                std::strtoul(v.c_str(), nullptr, 10));
        } else if (!(v = value("segment-bytes")).empty()) {
            options->segment_bytes =
                static_cast<std::size_t>(std::strtoull(v.c_str(), nullptr, 10));
        } else if (!(v = value("path")).empty()) {
            options->path = v;
        } else if (!(v = value("read")).empty()) {
            options->read_mode = v;
        } else if (!(v = value("read-from")).empty()) {
            options->read_from = std::strtoull(v.c_str(), nullptr, 10);
        } else if (!(v = value("read-to")).empty()) {
            options->read_to = std::strtoull(v.c_str(), nullptr, 10);
        } else if (arg == "--header") {
            options->header = true;
        } else if (arg == "--help") {
            std::printf(
                "用法: %s [--engine=buffered|mpsc|segmented] "
                "[--sync=none|batch|interval] [--n=N] [--threads=T] "
                "[--batch-bytes=B] [--sync-bytes=B] [--interval-ms=MS] "
                "[--segment-bytes=B] [--path=P] [--read=scan|range|none] "
                "[--read-from=S] [--read-to=S] [--header]\n",
                argv[0]);
            std::exit(0);
        } else {
            *error = "未知参数: " + arg;
            return false;
        }
    }
    if (options->n == 0 || options->threads == 0) {
        *error = "n/threads 必须 >= 1";
        return false;
    }
    if (options->read_mode != "scan" && options->read_mode != "range" &&
        options->read_mode != "none") {
        *error = "--read 只能是 scan/range/none";
        return false;
    }
    return true;
}

void Cleanup(const std::string& base) {
    std::error_code ec;
    std::filesystem::remove(base, ec);
    std::filesystem::remove(base + ".manifest", ec);
    std::filesystem::remove(base + ".manifest.tmp", ec);
    const std::filesystem::path parent =
        std::filesystem::path(base).parent_path();
    const std::string prefix = std::filesystem::path(base).filename().string();
    if (parent.empty()) {
        return;
    }
    for (const auto& entry : std::filesystem::directory_iterator(parent, ec)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const std::string name = entry.path().filename().string();
        if (name.rfind(prefix + ".seg-", 0) == 0) {
            std::filesystem::remove(entry.path(), ec);
        }
    }
}

std::uint64_t DiskBytes(const std::string& base, bool segmented) {
    std::error_code ec;
    if (!segmented) {
        return static_cast<std::uint64_t>(std::filesystem::file_size(base, ec));
    }
    std::uint64_t total = 0;
    const std::filesystem::path parent =
        std::filesystem::path(base).parent_path();
    const std::string prefix = std::filesystem::path(base).filename().string();
    if (!parent.empty()) {
        for (const auto& entry :
             std::filesystem::directory_iterator(parent, ec)) {
            const std::string name = entry.path().filename().string();
            if (name.rfind(prefix + ".seg-", 0) == 0 ||
                name == prefix + ".manifest") {
                total += static_cast<std::uint64_t>(
                    std::filesystem::file_size(entry.path(), ec));
            }
        }
    }
    return total;
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    std::string error;
    if (!ParseArgs(argc, argv, &options, &error)) {
        std::fprintf(stderr, "[persist-bench] %s\n", error.c_str());
        return 2;
    }
    if (options.header) {
        std::printf("%s\n", kFields);
    }
    Cleanup(options.path);
    const double load1 = LoadAvg1();

    WriterOptions writer_options;
    writer_options.path = options.path;
    writer_options.engine = options.engine;
    writer_options.sync = options.sync;
    writer_options.batch_bytes = options.batch_bytes;
    writer_options.sync_bytes = options.sync_bytes;
    writer_options.interval_ms = options.interval_ms;
    writer_options.segment_bytes = options.segment_bytes;
    std::unique_ptr<EventWriter> writer =
        EventWriter::Create(writer_options, &error);
    if (!writer) {
        std::fprintf(stderr, "[persist-bench] 创建 writer 失败: %s\n",
                     error.c_str());
        return 3;
    }

    std::vector<Producer*> producers;
    producers.reserve(options.threads);
    for (std::uint32_t t = 0; t < options.threads; ++t) {
        producers.push_back(writer->NewProducer());
    }
    std::vector<std::uint32_t> latencies;
    latencies.reserve(static_cast<std::size_t>(options.n));
    std::mutex merge_mutex;
    std::atomic<std::uint64_t> next_index{0};
    std::vector<std::string> errors(options.threads);

    const std::uint64_t append_begin = NowNs();
    {
        std::vector<std::thread> workers;
        workers.reserve(options.threads);
        for (std::uint32_t t = 0; t < options.threads; ++t) {
            const std::uint64_t count =
                options.n / options.threads +
                (t < options.n % options.threads ? 1U : 0U);
            workers.emplace_back([&, t, count] {
                std::vector<std::uint32_t> local;
                local.reserve(static_cast<std::size_t>(count));
                for (std::uint64_t i = 0; i < count; ++i) {
                    const std::uint64_t index =
                        next_index.fetch_add(1, std::memory_order_relaxed);
                    Event event = MakeBenchEvent(index);
                    const std::uint64_t begin = NowNs();
                    if (!writer->Append(producers[t], std::move(event))) {
                        errors[t] = "Append 失败";
                        return;
                    }
                    const std::uint64_t end = NowNs();
                    local.push_back(static_cast<std::uint32_t>(
                        std::min<std::uint64_t>(end - begin, 0xFFFFFFFFULL)));
                }
                std::lock_guard<std::mutex> lock(merge_mutex);
                latencies.insert(latencies.end(), local.begin(), local.end());
            });
        }
        for (auto& worker : workers) {
            worker.join();
        }
    }
    const std::uint64_t append_end = NowNs();
    for (const auto& message : errors) {
        if (!message.empty()) {
            std::fprintf(stderr, "[persist-bench] %s\n", message.c_str());
            return 4;
        }
    }
    if (!writer->Finish(&error)) {
        std::fprintf(stderr, "[persist-bench] Finish 失败: %s\n", error.c_str());
        return 5;
    }
    const std::uint64_t wall_end = NowNs();
    const WriterStats stats = writer->Stats();
    const ProcStats proc = ReadProcStats();
    const std::uint64_t append_ns = append_end - append_begin;
    const std::uint64_t wall_ns = wall_end - append_begin;
    const double append_sec = static_cast<double>(append_ns) / 1e9;
    const double wall_sec = static_cast<double>(wall_ns) / 1e9;
    const double append_ev = append_sec > 0.0
                                 ? static_cast<double>(options.n) / append_sec
                                 : 0.0;
    const double wall_ev = wall_sec > 0.0
                               ? static_cast<double>(options.n) / wall_sec
                               : 0.0;
    const LatencyStats latency = Summarize(&latencies);
    const std::uint64_t disk_bytes =
        DiskBytes(options.path, writer->Segments().empty() == false &&
                                    options.engine != Engine::kBuffered &&
                                    options.engine != Engine::kMpsc);
    const std::uint64_t max_sync_gap =
        options.sync == SyncPolicy::kNone ? wall_ns : stats.max_sync_gap_ns;

    // 回读校验
    std::uint64_t read_ns = 0;
    std::uint64_t read_records = 0;
    std::uint64_t read_bytes = 0;
    std::uint64_t read_checksum = 0;
    int read_ok = 1;
    int crc_ok = 1;
    int seq_ok = 1;
    int checksum_ok = 1;
    std::string read_mode = options.read_mode;
    if (options.read_mode != "none") {
        std::unique_ptr<SessionReader> reader =
            SessionReader::Open(options.path, &error);
        if (!reader) {
            std::fprintf(stderr, "[persist-bench] 打开读取端失败: %s\n",
                         error.c_str());
            return 6;
        }
        go2cpp::persistence::ReadStats read;
        if (options.read_mode == "scan") {
            read = reader->ScanAll();
        } else {
            read = reader->ReadSeqRange(options.read_from, options.read_to);
        }
        read_ns = read.elapsed_ns;
        read_records = read.records;
        read_bytes = read.bytes;
        read_checksum = read.checksum;
        read_ok = read.error.empty() ? 1 : 0;
        crc_ok = read.crc_ok ? 1 : 0;
        seq_ok = read.seq_ok ? 1 : 0;
        if (options.read_mode == "scan") {
            checksum_ok = (read.checksum == stats.logical_checksum &&
                           read.records == stats.events)
                              ? 1
                              : 0;
        } else {
            checksum_ok = 1;  // 范围读只校验 CRC/seq，不要求全量 checksum
        }
        if (!read_ok || !crc_ok || !seq_ok || !checksum_ok) {
            std::fprintf(stderr,
                         "[persist-bench] 回读校验失败 mode=%s err=%s "
                         "records=%llu expected=%llu checksum=%llu expected=%llu\n",
                         read_mode.c_str(), read.error.c_str(),
                         static_cast<unsigned long long>(read_records),
                         static_cast<unsigned long long>(stats.events),
                         static_cast<unsigned long long>(read_checksum),
                         static_cast<unsigned long long>(stats.logical_checksum));
            return 7;
        }
    }
    const double read_sec = static_cast<double>(read_ns) / 1e9;
    const double read_ev =
        read_sec > 0.0 ? static_cast<double>(read_records) / read_sec : 0.0;
    const int valid = load1 <= 2.0 ? 1 : 0;

    std::printf(
        "%s\t%llu\t%u\t%s\t%zu\t%zu\t%u\t%zu\t%llu\t%.0f\t%llu\t%llu\t%.0f\t"
        "%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%zu\t%llu\t%llu\t%s\t%llu\t%.0f\t"
        "%llu\t%llu\t%d\t%d\t%d\t%d\t%llu\t%llu\t%llu\t%llu\t%.2f\t%d\n",
        go2cpp::persistence::EngineName(options.engine),
        static_cast<unsigned long long>(options.n), options.threads,
        go2cpp::persistence::SyncPolicyName(options.sync), options.batch_bytes,
        options.sync_bytes, options.interval_ms, options.segment_bytes,
        static_cast<unsigned long long>(append_ns), append_ev,
        static_cast<unsigned long long>(wall_ns - append_ns),
        static_cast<unsigned long long>(wall_ns), wall_ev,
        static_cast<unsigned long long>(latency.p50),
        static_cast<unsigned long long>(latency.p99),
        static_cast<unsigned long long>(latency.p999),
        static_cast<unsigned long long>(proc.rss_kb),
        static_cast<unsigned long long>(proc.hwm_kb),
        static_cast<unsigned long long>(disk_bytes),
        writer->Segments().size(), static_cast<unsigned long long>(stats.events),
        static_cast<unsigned long long>(stats.logical_checksum),
        read_mode.c_str(), static_cast<unsigned long long>(read_ns), read_ev,
        static_cast<unsigned long long>(read_bytes),
        static_cast<unsigned long long>(read_records), read_ok, crc_ok, seq_ok,
        checksum_ok, static_cast<unsigned long long>(stats.fsyncs),
        static_cast<unsigned long long>(stats.fsync_p50_ns),
        static_cast<unsigned long long>(stats.fsync_p99_ns),
        static_cast<unsigned long long>(max_sync_gap), load1, valid);
    return 0;
}
