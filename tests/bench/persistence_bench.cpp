/**
 * @file persistence_bench.cpp
 * @brief 事件流「追加写 + 持久化」基线压测（G2CRPLY v5 记录）。
 *
 * 目的：在**不修改** tools/replay 核心 Recorder 的前提下，量化现状：
 *   1. `recorder`：真实 `go2cpp::replay::Recorder::Write`（逐条 seq + 逐条
 *      CRC + `fwrite` 到 FILE*，默认 4 KiB libc 缓冲，无 fsync API）；
 *   2. `raw`：逐条 `write(2)`，用同一 `EncodeEvent` 编码，可叠加
 *      none / every / batch / interval 四种 fdatasync 策略；
 *   3. `raw-buffered`：用户态攒批（默认 1 MiB）后一次 `write(2)`，
 *      再叠加同样的 fsync 策略。
 *
 * 输出：一行 TSV（字段见 `kFields`）。压测口径：
 *   - seq/vtime 在写路径内生成，与 Recorder 语义一致；
 *   - 延迟用 `clock_gettime(CLOCK_MONOTONIC_RAW)` 逐事件采样（含 fsync
 *     阻塞时间，因此 interval 策略的 p99 会体现同步等待）；
 *   - RSS 取压测结束时的 VmRSS / VmHWM；
 *   - 结束后用 `DecodeEvent` 全量回读，校验 CRC、资源事件条数与 seq 单调性；
 *   - loadavg 一并输出，调用方据此判定是否采信（>2 不采信）。
 *
 * 构建（不接入顶部 CMake，避免改变默认构建行为）：
 *   see tests/bench/run_persistence_bench.sh
 */

#include "replay/format.hpp"
#include "replay/recorder.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using go2cpp::replay::Event;
using go2cpp::replay::EventType;
using go2cpp::replay::Recorder;
using go2cpp::replay::RecordOptions;

constexpr const char* kFields =
    "impl\tn\tthreads\tfsync\tbatch_bytes\tinterval_ms\twall_ns\tev_per_sec\t"
    "p50_ns\tp99_ns\tp999_ns\trss_kb\thwm_kb\tfile_bytes\trecords\tdecode_ok\t"
    "seq_monotonic\tfsync_count\tfsync_p50_ns\tfsync_p99_ns\tmax_sync_gap_ns\t"
    "load1\tvalid";

/// 单调时钟纳秒（clock_gettime 比 steady_clock 少一层抽象，约 15–20 ns）。
std::uint64_t NowNs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ULL +
           static_cast<std::uint64_t>(ts.tv_nsec);
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

double LoadAvg1() {
    double value = 0.0;
    std::ifstream in("/proc/loadavg");
    in >> value;
    return value;
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

/// 每条事件的 payload 都变化，避免固定字节让 CRC/压缩测出理想值。
Event MakeBenchEvent(std::uint64_t index, std::uint64_t vtime_ns) {
    static constexpr EventType kTypes[] = {
        EventType::kResRwReadLock,  EventType::kResRwWriteLock,
        EventType::kResRwUnlock,    EventType::kResCvWait,
        EventType::kResCvNotify,    EventType::kResCvNotifyAll,
        EventType::kResWgAdd,       EventType::kResWgDone,
    };
    const EventType type = kTypes[index % (sizeof(kTypes) / sizeof(kTypes[0]))];
    return go2cpp::replay::MakeResourceEvent(
        type,
        /*resource_id=*/(index % 4096U) + 1U,
        /*actor_gid=*/index % 64U,
        /*target_gid=*/(index * 7U) % 64U,
        /*resource_result=*/static_cast<std::uint32_t>(index % 3U),
        /*address=*/0x7f0000000000ULL + (index % 4096U) * 64U,
        /*waiters=*/static_cast<std::uint32_t>(index % 8U),
        /*res_fd=*/-1,
        /*res_bytes=*/static_cast<std::uint32_t>(index % 1024U),
        /*res_aux=*/index % 5U,
        /*value=*/static_cast<std::int64_t>(index));
}

struct Options {
    std::string impl{"recorder"};
    std::string path{"/tmp/go2cpp_persistence_bench.g2cr"};
    std::string fsync{"none"};
    std::uint64_t n{100000};
    std::uint32_t threads{1};
    std::size_t batch_bytes{1024U * 1024U};
    std::uint32_t interval_ms{100};
    bool header{false};
};

bool ParseArgs(int argc, char** argv, Options* options, std::string* error) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto value = [&arg](const char* key) -> std::string {
            const std::string prefix = std::string("--") + key + "=";
            return arg.rfind(prefix, 0) == 0 ? arg.substr(prefix.size()) : "";
        };
        std::string v;
        if (!(v = value("impl")).empty()) {
            options->impl = v;
        } else if (!(v = value("path")).empty()) {
            options->path = v;
        } else if (!(v = value("fsync")).empty()) {
            options->fsync = v;
        } else if (!(v = value("n")).empty()) {
            options->n = std::strtoull(v.c_str(), nullptr, 10);
        } else if (!(v = value("threads")).empty()) {
            options->threads = static_cast<std::uint32_t>(
                std::strtoul(v.c_str(), nullptr, 10));
        } else if (!(v = value("batch-bytes")).empty()) {
            options->batch_bytes =
                static_cast<std::size_t>(std::strtoull(v.c_str(), nullptr, 10));
        } else if (!(v = value("interval-ms")).empty()) {
            options->interval_ms = static_cast<std::uint32_t>(
                std::strtoul(v.c_str(), nullptr, 10));
        } else if (arg == "--header") {
            options->header = true;
        } else if (arg == "--help") {
            std::printf(
                "用法: %s [--impl=recorder|raw|raw-buffered] [--n=N] "
                "[--threads=T] [--fsync=none|every|batch|interval] "
                "[--batch-bytes=B] [--interval-ms=MS] [--path=P] [--header]\n",
                argv[0]);
            std::exit(0);
        } else {
            *error = "未知参数: " + arg;
            return false;
        }
    }
    if (options->threads == 0) {
        *error = "--threads 必须 >= 1";
        return false;
    }
    if (options->n == 0) {
        *error = "--n 必须 >= 1";
        return false;
    }
    if (options->fsync != "none" && options->fsync != "every" &&
        options->fsync != "batch" && options->fsync != "interval") {
        *error = "--fsync 只能是 none/every/batch/interval";
        return false;
    }
    if (options->impl != "recorder" && options->impl != "raw" &&
        options->impl != "raw-buffered") {
        *error = "--impl 只能是 recorder/raw/raw-buffered";
        return false;
    }
    if (options->impl == "recorder" && options->fsync != "none") {
        *error = "recorder 没有 fsync 接口；持久化策略请用 raw/raw-buffered";
        return false;
    }
    return true;
}

/// 全量回读校验：CRC、资源事件条数、seq 严格递增。
bool ValidateFile(const std::string& path, std::uint64_t expected_events,
                  std::uint64_t* actual_events, bool* monotonic) {
    *actual_events = 0;
    *monotonic = true;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
    go2cpp::replay::FileHeader header;
    std::string error;
    if (!go2cpp::replay::DecodeFileHeader(data.data(), data.size(), &header,
                                          &error)) {
        std::fprintf(stderr, "[bench] 解码文件头失败: %s\n", error.c_str());
        return false;
    }
    std::size_t offset = go2cpp::replay::kFileHeaderSize;
    std::uint64_t last_seq = 0;
    bool saw_first = false;
    while (offset < data.size()) {
        Event event;
        std::size_t consumed = 0;
        if (!go2cpp::replay::DecodeEvent(data.data() + offset,
                                         data.size() - offset, &event,
                                         &consumed, &error)) {
            std::fprintf(stderr, "[bench] 解码事件失败: %s\n", error.c_str());
            return false;
        }
        if (go2cpp::replay::IsResourceEventType(event.type)) {
            ++*actual_events;
            if (saw_first && event.seq <= last_seq) {
                *monotonic = false;
            }
            last_seq = event.seq;
            saw_first = true;
        }
        offset += consumed;
    }
    if (offset != data.size()) {
        return false;
    }
    return *actual_events == expected_events;
}

// ---------------------------------------------------------------------------
// raw / raw-buffered：与 Recorder 同一编码，但可控制 write 与 fsync
// ---------------------------------------------------------------------------
class RawWriter {
public:
    RawWriter(const Options& options, std::string* error)
        : m_options(options) {
        m_fd = ::open(options.path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (m_fd < 0) {
            *error = "open: " + std::string(std::strerror(errno));
            return;
        }
        m_scratch.reserve(256);
        if (options.impl == "raw-buffered") {
            m_buffer.reserve(options.batch_bytes + 4096U);
        }
        go2cpp::replay::FileHeader header;
        header.created_unix_ns = static_cast<std::uint64_t>(
            std::chrono::system_clock::now().time_since_epoch().count());
        header.producer_id = 0x67326372706C79ULL;
        m_scratch.clear();
        go2cpp::replay::EncodeFileHeader(header, m_scratch);
        WriteHeaderLocked(m_scratch.data(), m_scratch.size());

        Event meta;
        meta.type = EventType::kMeta;
        meta.gid = 64;
        meta.channel = 4;
        meta.note = "persistence-bench";
        meta.ext_flags = go2cpp::replay::kMetaHasValueCodec;
        meta.clock_mode = go2cpp::replay::kClockVirtual;
        meta.seq = m_seq++;
        meta.ts_ns = NowNs();
        EmitLocked(meta);
        m_start_ns = NowNs();
    }

    ~RawWriter() {
        if (m_fd >= 0) {
            Finish();
            ::close(m_fd);
            m_fd = -1;
        }
    }

    bool ok() const { return m_fd >= 0; }

    /// 与 Recorder 一致：seq 在锁内分配，因此文件顺序 == seq 顺序。
    bool Write(const Event& event, std::uint32_t* latency_ns,
               std::uint32_t* fsync_latency_ns) {
        const std::uint64_t begin = NowNs();
        std::lock_guard<std::mutex> lock(m_mutex);
        Event copy = event;
        copy.seq = m_seq++;
        if (copy.ts_ns == 0) {
            copy.ts_ns = NowNs();
        }
        if (copy.vtime_ns == 0) {
            copy.vtime_ns = NowNs() - m_start_ns;
        }
        EmitLocked(copy);
        // every：每条都 fsync；batch：raw 按字节阈值、buffered 每批；
        // interval：按时间（写路径内同步，p99 会体现阻塞）。
        if (m_options.fsync == "every") {
            SyncLocked(fsync_latency_ns);
        } else if (m_options.fsync == "batch" && m_options.impl == "raw" &&
                   m_unsynced_bytes >= m_options.batch_bytes) {
            SyncLocked(fsync_latency_ns);
        } else if (m_options.fsync == "interval") {
            const std::uint64_t now = NowNs();
            if (m_dirty &&
                now - m_last_sync_ns >=
                    static_cast<std::uint64_t>(m_options.interval_ms) *
                        1000000ULL) {
                SyncLocked(fsync_latency_ns);
            }
        }
        const std::uint64_t end = NowNs();
        *latency_ns = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(end - begin, 0xFFFFFFFFULL));
        return true;
    }

    void Finish() {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_finished) {
            return;
        }
        m_finished = true;
        FlushLocked();
        // kEnd 不参与整文件滚动 CRC（其 sched_type 保存该 CRC）。
        Event end;
        end.type = EventType::kEnd;
        end.seq = m_seq++;
        end.ts_ns = NowNs();
        end.gid = 0;
        end.channel = go2cpp::replay::kNoChannel;
        end.value = static_cast<std::int64_t>(m_records);
        end.sched_type = m_file_crc;
        m_scratch.clear();
        go2cpp::replay::EncodeEvent(end, m_scratch);
        WriteAllLocked(m_scratch.data(), m_scratch.size());
        if (m_dirty) {
            SyncLocked(nullptr);
        }
    }

    std::uint64_t FsyncCount() const { return m_fsync_count; }
    std::uint64_t MaxSyncGapNs() const { return m_max_sync_gap_ns; }
    std::pair<std::uint64_t, std::uint64_t> FsyncP50P99() const {
        if (m_fsync_latencies.empty()) {
            return {0, 0};
        }
        std::vector<std::uint32_t> copy = m_fsync_latencies;
        const LatencyStats stats = Summarize(&copy);
        return {stats.p50, stats.p99};
    }

private:
    void WriteHeaderLocked(const std::uint8_t* data, std::size_t size) {
        std::size_t offset = 0;
        while (offset < size) {
            const ssize_t written = ::write(m_fd, data + offset, size - offset);
            if (written <= 0) {
                if (errno == EINTR) {
                    continue;
                }
                std::fprintf(stderr, "[bench] write 失败: %s\n",
                             std::strerror(errno));
                return;
            }
            offset += static_cast<std::size_t>(written);
        }
        m_file_bytes += size;
    }

    /// 编码一条事件、更新滚动 CRC、按实现写入（逐条或追加到批量缓冲）。
    void EmitLocked(const Event& event) {
        m_scratch.clear();
        go2cpp::replay::EncodeEvent(event, m_scratch);
        m_file_crc = go2cpp::replay::Crc32(m_scratch.data(), m_scratch.size(),
                                           m_file_crc);
        if (m_options.impl == "raw") {
            WriteAllLocked(m_scratch.data(), m_scratch.size());
        } else {
            if (m_buffer.size() + m_scratch.size() > m_options.batch_bytes) {
                FlushLocked();
                if (m_options.fsync == "batch" && m_dirty) {
                    SyncLocked(nullptr);
                }
            }
            m_buffer.insert(m_buffer.end(), m_scratch.begin(), m_scratch.end());
        }
        ++m_records;
    }

    void WriteAllLocked(const std::uint8_t* data, std::size_t size) {
        std::size_t offset = 0;
        while (offset < size) {
            const ssize_t written = ::write(m_fd, data + offset, size - offset);
            if (written <= 0) {
                if (errno == EINTR) {
                    continue;
                }
                std::fprintf(stderr, "[bench] write 失败: %s\n",
                             std::strerror(errno));
                return;
            }
            offset += static_cast<std::size_t>(written);
        }
        m_file_bytes += size;
        m_unsynced_bytes += size;
        m_dirty = true;
    }

    void FlushLocked() {
        if (!m_buffer.empty()) {
            WriteAllLocked(m_buffer.data(), m_buffer.size());
            m_buffer.clear();
        }
    }

    void SyncLocked(std::uint32_t* latency_ns) {
        const std::uint64_t begin = NowNs();
        if (::fdatasync(m_fd) != 0) {
            std::fprintf(stderr, "[bench] fdatasync 失败: %s\n",
                         std::strerror(errno));
        }
        const std::uint64_t end = NowNs();
        const std::uint64_t latency = end - begin;
        if (latency_ns != nullptr) {
            *latency_ns = static_cast<std::uint32_t>(
                std::min<std::uint64_t>(latency, 0xFFFFFFFFULL));
        }
        m_fsync_latencies.push_back(static_cast<std::uint32_t>(
            std::min<std::uint64_t>(latency, 0xFFFFFFFFULL)));
        if (m_last_sync_ns != 0) {
            m_max_sync_gap_ns =
                std::max(m_max_sync_gap_ns, begin - m_last_sync_ns);
        }
        m_last_sync_ns = end;
        m_dirty = false;
        m_unsynced_bytes = 0;
        ++m_fsync_count;
    }

    const Options& m_options;
    int m_fd{-1};
    std::mutex m_mutex;
    std::vector<std::uint8_t> m_scratch;
    std::vector<std::uint8_t> m_buffer;
    std::vector<std::uint32_t> m_fsync_latencies;
    std::uint64_t m_seq{0};
    std::uint64_t m_records{0};
    std::uint64_t m_start_ns{0};
    std::uint64_t m_last_sync_ns{0};
    std::uint64_t m_max_sync_gap_ns{0};
    std::uint64_t m_file_bytes{0};
    std::uint64_t m_unsynced_bytes{0};
    std::uint32_t m_file_crc{0};
    std::uint64_t m_fsync_count{0};
    bool m_dirty{false};
    bool m_finished{false};
};

/// 多线程压测主体：T 个线程各写 count 条，逐条采样延迟。
bool RunRaw(const Options& options, RawWriter* writer,
            std::vector<std::uint32_t>* latencies,
            std::vector<std::uint32_t>* fsync_latencies,
            std::string* error) {
    std::atomic<std::uint64_t> next_index{0};
    std::vector<std::thread> workers;
    std::vector<std::string> errors(options.threads);
    std::mutex merge_mutex;
    workers.reserve(options.threads);
    for (std::uint32_t t = 0; t < options.threads; ++t) {
        const std::uint64_t count =
            options.n / options.threads +
            (t < options.n % options.threads ? 1U : 0U);
        workers.emplace_back([&, t, count] {
            std::vector<std::uint32_t> local;
            std::vector<std::uint32_t> local_sync;
            local.reserve(static_cast<std::size_t>(count));
            local_sync.reserve(64);
            std::uint64_t vtime = 0;
            for (std::uint64_t i = 0; i < count; ++i) {
                const std::uint64_t index =
                    next_index.fetch_add(1, std::memory_order_relaxed);
                Event event = MakeBenchEvent(index, vtime);
                vtime += 1000U + (index % 97U);
                std::uint32_t latency = 0;
                std::uint32_t sync_latency = 0;
                if (!writer->Write(event, &latency, &sync_latency)) {
                    errors[t] = "raw write 失败";
                    return;
                }
                local.push_back(latency);
                if (sync_latency != 0) {
                    local_sync.push_back(sync_latency);
                }
            }
            std::lock_guard<std::mutex> lock(merge_mutex);
            latencies->insert(latencies->end(), local.begin(), local.end());
            fsync_latencies->insert(fsync_latencies->end(), local_sync.begin(),
                                    local_sync.end());
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    for (const auto& message : errors) {
        if (!message.empty()) {
            *error = message;
            return false;
        }
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    std::string error;
    if (!ParseArgs(argc, argv, &options, &error)) {
        std::fprintf(stderr, "[bench] %s\n", error.c_str());
        return 2;
    }
    if (options.header) {
        std::printf("%s\n", kFields);
    }
    const double load1 = LoadAvg1();
    const std::uint64_t wall_begin = NowNs();
    std::vector<std::uint32_t> latencies;
    std::vector<std::uint32_t> fsync_latencies;
    latencies.reserve(static_cast<std::size_t>(options.n));
    fsync_latencies.reserve(1024);
    std::uint64_t fsync_count = 0;
    std::uint64_t max_sync_gap_ns = 0;
    std::uint64_t fsync_p50 = 0;
    std::uint64_t fsync_p99 = 0;
    std::string write_error;

    if (options.impl == "recorder") {
        RecordOptions record_options;
        record_options.path = options.path;
        record_options.demo_name = "persistence-bench";
        record_options.gid_count = 64;
        record_options.channel_count = 4;
        Recorder recorder(record_options);
        if (!recorder.ok()) {
            std::fprintf(stderr, "[bench] recorder: %s\n",
                         recorder.error().c_str());
            return 3;
        }
        std::atomic<std::uint64_t> next_index{0};
        std::vector<std::thread> workers;
        std::vector<std::string> errors(options.threads);
        std::mutex merge_mutex;
        for (std::uint32_t t = 0; t < options.threads; ++t) {
            const std::uint64_t count =
                options.n / options.threads +
                (t < options.n % options.threads ? 1U : 0U);
            workers.emplace_back([&, t, count] {
                std::vector<std::uint32_t> local;
                local.reserve(static_cast<std::size_t>(count));
                std::uint64_t vtime = 0;
                for (std::uint64_t i = 0; i < count; ++i) {
                    const std::uint64_t index =
                        next_index.fetch_add(1, std::memory_order_relaxed);
                    Event event = MakeBenchEvent(index, vtime);
                    vtime += 1000U + (index % 97U);
                    const std::uint64_t begin = NowNs();
                    if (!recorder.Write(event)) {
                        errors[t] = "Recorder::Write 失败";
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
        for (const auto& message : errors) {
            if (!message.empty()) {
                write_error = message;
            }
        }
        recorder.Finalize("persistence-bench");
        // Recorder 没有 fsync 接口：数据只到页缓存，持久窗口按整轮计。
        max_sync_gap_ns = NowNs() - wall_begin;
        if (!write_error.empty()) {
            std::fprintf(stderr, "[bench] %s\n", write_error.c_str());
            return 4;
        }
    } else {
        RawWriter writer(options, &error);
        if (!writer.ok()) {
            std::fprintf(stderr, "[bench] %s\n", error.c_str());
            return 3;
        }
        if (!RunRaw(options, &writer, &latencies, &fsync_latencies, &error)) {
            std::fprintf(stderr, "[bench] %s\n", error.c_str());
            return 4;
        }
        writer.Finish();
        fsync_count = writer.FsyncCount();
        max_sync_gap_ns = writer.MaxSyncGapNs();
        const auto p50p99 = writer.FsyncP50P99();
        fsync_p50 = p50p99.first;
        fsync_p99 = p50p99.second;
        if (options.fsync == "none") {
            max_sync_gap_ns = NowNs() - wall_begin;
        }
    }
    const std::uint64_t wall_end = NowNs();
    const std::uint64_t wall_ns = wall_end - wall_begin;
    const ProcStats proc = ReadProcStats();

    struct stat st {};
    std::uint64_t file_bytes = 0;
    if (::stat(options.path.c_str(), &st) == 0) {
        file_bytes = static_cast<std::uint64_t>(st.st_size);
    }
    std::uint64_t actual_events = 0;
    bool monotonic = false;
    const bool decode_ok =
        ValidateFile(options.path, options.n, &actual_events, &monotonic);
    const LatencyStats latency = Summarize(&latencies);
    const double seconds = static_cast<double>(wall_ns) / 1e9;
    const double ev_per_sec =
        seconds > 0.0 ? static_cast<double>(options.n) / seconds : 0.0;
    const int valid = load1 <= 2.0 ? 1 : 0;

    std::printf(
        "%s\t%llu\t%u\t%s\t%zu\t%u\t%llu\t%.0f\t%llu\t%llu\t%llu\t%llu\t%llu\t"
        "%llu\t%llu\t%d\t%d\t%llu\t%llu\t%llu\t%llu\t%.2f\t%d\n",
        options.impl.c_str(), static_cast<unsigned long long>(options.n),
        options.threads, options.fsync.c_str(), options.batch_bytes,
        options.interval_ms, static_cast<unsigned long long>(wall_ns),
        ev_per_sec, static_cast<unsigned long long>(latency.p50),
        static_cast<unsigned long long>(latency.p99),
        static_cast<unsigned long long>(latency.p999),
        static_cast<unsigned long long>(proc.rss_kb),
        static_cast<unsigned long long>(proc.hwm_kb),
        static_cast<unsigned long long>(file_bytes),
        static_cast<unsigned long long>(actual_events), decode_ok ? 1 : 0,
        monotonic ? 1 : 0, static_cast<unsigned long long>(fsync_count),
        static_cast<unsigned long long>(fsync_p50),
        static_cast<unsigned long long>(fsync_p99),
        static_cast<unsigned long long>(max_sync_gap_ns), load1, valid);
    if (!decode_ok || actual_events != options.n || !monotonic) {
        std::fprintf(stderr,
                     "[bench] 校验失败 decode_ok=%d records=%llu expected=%llu "
                     "monotonic=%d\n",
                     decode_ok ? 1 : 0,
                     static_cast<unsigned long long>(actual_events),
                     static_cast<unsigned long long>(options.n),
                     monotonic ? 1 : 0);
        return 5;
    }
    return 0;
}
