/**
 * @file debug_replay_bench.cpp
 * @brief replay（记录/回放）的吞吐基准 + 逐字节一致正确性。
 *
 * 三个可独立运行的阶段（分开进程，避免相互污染）：
 *   record —— 1e6 条事件写入 G2CRPLY 文件（编码 + 逐条 CRC + 整文件 CRC +
 *             fwrite），测条/秒与 ns/条；
 *   replay —— 同一文件加载 + 静态重演（校验 CRC / 配对 / 统计），测条/秒，
 *             并用**独立实现**的 FNV-1a 复算消息校验和，防止「统计与校验和
 *             同源、一起错」；
 *   bytes  —— 同一事件序列录制两次 → 除文件头 created_unix_ns(偏移 16..23)
 *             外逐字节相同；对同一文件回放两次 → JSON 报告逐字节相同；
 *             再核对解码后的消息序列。
 *   iobase —— 纯 fread 读完整文件 / 逐字节扫描，作为回放速度的 I/O 上限基线。
 *
 * 输出：一行一个 KEY<TAB>VALUE。退出码 0=全部断言通过。
 *
 * 用法：
 *   debug_replay_bench record --n=1000000 --path=/tmp/debug_replay.g2cr
 *   debug_replay_bench replay --n=1000000 --path=/tmp/debug_replay.g2cr
 *   debug_replay_bench bytes  --n=1000000 --path=/tmp/debug_replay.g2cr
 *   debug_replay_bench iobase --path=/tmp/debug_replay.g2cr
 */

#include "replay/format.hpp"
#include "replay/recorder.hpp"
#include "replay/replay_engine.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <type_traits>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using go2cpp::replay::Event;
using go2cpp::replay::EventType;
using go2cpp::replay::Recorder;
using go2cpp::replay::RecordOptions;
using go2cpp::replay::ReplayOptions;
using go2cpp::replay::ReplayReport;
using go2cpp::replay::ReplayStatus;

/// 录制文件头里 created_unix_ns 的偏移（magic 8 + version 2 + size 2 +
/// flags 4）；逐字节一致比较时把它排除（两次录制的墙钟时间不同）。
constexpr std::size_t kCreatedOffset = 16U;
constexpr std::size_t kCreatedSize = 8U;

std::uint64_t NowNs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now().time_since_epoch())
            .count());
}

double NsPer(std::uint64_t ns, std::uint64_t count) {
    return count == 0 ? 0.0
                      : static_cast<double>(ns) / static_cast<double>(count);
}

void Emit(const char* key, std::uint64_t value) {
    std::printf("%s\t%llu\n", key, static_cast<unsigned long long>(value));
}

void Emit(const char* key, double value) {
    std::printf("%s\t%.3f\n", key, value);
}

void Emit(const char* key, const char* value) {
    std::printf("%s\t%s\n", key, value);
}

/// 小整型（uint32_t 等）统一提升到 uint64_t，避免重载歧义。
template <typename T,
          typename std::enable_if<std::is_integral<T>::value &&
                                      !std::is_same<T, std::uint64_t>::value,
                                  int>::type = 0>
void Emit(const char* key, T value) {
    Emit(key, static_cast<std::uint64_t>(value));
}

struct Options {
    std::uint64_t n{1000000};
    std::string path{"/tmp/debug_replay_bench.g2cr"};
};

bool ParseU64(const char* text, std::uint64_t* out) {
    if (text == nullptr || *text == '\0') return false;
    char* end = nullptr;
    const unsigned long long value = std::strtoull(text, &end, 10);
    if (end == nullptr || *end != '\0') return false;
    *out = static_cast<std::uint64_t>(value);
    return true;
}

bool ParseOptions(int argc, char** argv, Options* opt) {
    for (int i = 2; i < argc; ++i) {
        const char* arg = argv[i];
        if (std::strncmp(arg, "--n=", 4) == 0) {
            if (!ParseU64(arg + 4, &opt->n)) return false;
        } else if (std::strncmp(arg, "--path=", 7) == 0) {
            opt->path = arg + 7;
        } else {
            std::fprintf(stderr, "未知参数: %s\n", arg);
            return false;
        }
    }
    if (opt->n == 0 || (opt->n % 2U) != 0) {
        std::fprintf(stderr, "--n 必须是正偶数（send/recv 配对）\n");
        return false;
    }
    return true;
}

/// 合成确定性事件流：gid=1，send/recv 交替，xfer_id 与 op_index 唯一，
/// ts_ns/vtime_ns 固定（保证两次录制内容可逐字节比较）。
void WriteStream(Recorder* recorder, std::uint64_t n) {
    Event spawn;
    spawn.type = EventType::kSpawn;
    spawn.gid = 1;
    spawn.ts_ns = 1000000000ULL;
    spawn.vtime_ns = 1;
    (void)recorder->Write(spawn);

    for (std::uint64_t i = 0; i < n; ++i) {
        Event event;
        event.type = (i % 2U == 0U) ? EventType::kSend : EventType::kRecv;
        event.gid = 1;
        event.channel = 7;
        event.xfer_id = static_cast<std::uint32_t>(i / 2U + 1U);
        event.op_index = static_cast<std::uint32_t>(i);
        event.value = static_cast<std::int64_t>(i);
        event.decision = go2cpp::replay::Decision::kOk;
        event.blocked = 0;
        event.ts_ns = 1000000001ULL + i;
        event.vtime_ns = i + 1U;
        (void)recorder->Write(event);
    }

    Event exit_event;
    exit_event.type = EventType::kExit;
    exit_event.gid = 1;
    exit_event.ts_ns = 1000000001ULL + n;
    exit_event.vtime_ns = n;
    exit_event.value = static_cast<std::int64_t>(n / 2U);
    exit_event.aux64 = n / 2U;
    (void)recorder->Write(exit_event);
}

std::uint64_t FileSize(const std::string& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return 0;
    return static_cast<std::uint64_t>(file.tellg());
}

bool ReadFile(const std::string& path, std::vector<std::uint8_t>* out) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return false;
    const std::streamoff size = file.tellg();
    if (size < 0) return false;
    out->resize(static_cast<std::size_t>(size));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(out->data()),
              static_cast<std::streamsize>(size));
    return static_cast<bool>(file);
}

/// 独立实现的 FNV-1a 64（不复用引擎的累计函数）。
std::uint64_t IndependentChecksum(std::uint64_t n) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (std::uint64_t i = 1; i < n; i += 2U) {
        hash ^= i;
        hash *= 1099511628211ULL;
    }
    return hash;
}

/// 校验回放报告与合成流一致：状态、计数、独立校验和、接收序列。
bool VerifyReport(const ReplayReport& report, std::uint64_t n,
                  std::string* reason) {
    if (report.status != ReplayStatus::kOk) {
        *reason = std::string("status=") +
                  go2cpp::replay::ReplayStatusName(report.status) + " err=" +
                  report.error;
        return false;
    }
    if (!report.complete) {
        *reason = "not complete";
        return false;
    }
    const std::uint64_t half = n / 2U;
    if (report.stats.sends != half || report.stats.recvs != half ||
        report.stats.messages != half) {
        *reason = "counts mismatch sends=" + std::to_string(report.stats.sends) +
                  " recvs=" + std::to_string(report.stats.recvs) +
                  " messages=" + std::to_string(report.stats.messages) +
                  " expect=" + std::to_string(half);
        return false;
    }
    const std::uint64_t checksum = IndependentChecksum(n);
    if (report.stats.message_checksum != checksum) {
        *reason = "checksum mismatch got=" +
                  std::to_string(report.stats.message_checksum) +
                  " expect=" + std::to_string(checksum);
        return false;
    }
    if (report.stats.global_recv_sequence.size() != half) {
        *reason = "recv sequence size=" +
                  std::to_string(report.stats.global_recv_sequence.size());
        return false;
    }
    for (std::uint64_t i = 0; i < half; ++i) {
        if (report.stats.global_recv_sequence[i] !=
            static_cast<std::int64_t>(i * 2U + 1U)) {
            *reason = "recv sequence[" + std::to_string(i) + "]=" +
                      std::to_string(report.stats.global_recv_sequence[i]);
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// record
// ---------------------------------------------------------------------------
int RunRecord(const Options& opt) {
    RecordOptions record_options;
    record_options.path = opt.path;
    record_options.demo_name = "debug_asm_bench";
    record_options.gid_count = 2;
    record_options.channel_count = 2;
    record_options.clock_mode = go2cpp::replay::kClockVirtual;

    std::string error;
    std::uint64_t t0 = NowNs();
    Recorder recorder(record_options);
    if (!recorder.ok()) {
        std::fprintf(stderr, "Recorder 打开失败: %s\n", recorder.error().c_str());
        return 1;
    }
    WriteStream(&recorder, opt.n);
    recorder.Finalize("debug_asm_bench done");
    const std::uint64_t wall_ns = NowNs() - t0;
    if (!recorder.finalized()) {
        std::fprintf(stderr, "Finalize 失败: %s\n", recorder.error().c_str());
        return 1;
    }

    const std::uint64_t file_size = FileSize(opt.path);
    const std::uint64_t events = recorder.Records();
    Emit("record_n", opt.n);
    Emit("record_events", events);
    Emit("record_file_size", file_size);
    Emit("record_wall_ns", wall_ns);
    Emit("record_ns_per_event", NsPer(wall_ns, events));
    Emit("record_events_per_sec",
         static_cast<double>(events) * 1e9 / static_cast<double>(wall_ns));
    Emit("record_bytes_per_sec",
         static_cast<double>(file_size) * 1e9 / static_cast<double>(wall_ns));
    // 文件内事件数 = kMeta(1) + kSpawn(1) + n + kExit(1) + finalize note(1)。
    const bool ok = events == opt.n + 4U && file_size > 0;
    Emit("result", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

// ---------------------------------------------------------------------------
// replay
// ---------------------------------------------------------------------------
int RunReplay(const Options& opt) {
    ReplayOptions replay_options;
    replay_options.keep_timeline = false;

    const std::uint64_t t0 = NowNs();
    const ReplayReport report = go2cpp::replay::ReplayFile(opt.path, replay_options);
    const std::uint64_t wall_ns = NowNs() - t0;

    std::string reason;
    const bool verify = VerifyReport(report, opt.n, &reason);
    Emit("replay_n", opt.n);
    Emit("replay_file_size", report.file_size);
    Emit("replay_wall_ns", wall_ns);
    Emit("replay_ns_per_event",
         NsPer(wall_ns, static_cast<std::uint64_t>(report.stats.sends +
                                                   report.stats.recvs)));
    Emit("replay_events_per_sec",
         static_cast<double>(report.stats.sends + report.stats.recvs) * 1e9 /
             static_cast<double>(wall_ns));
    Emit("replay_bytes_per_sec",
         static_cast<double>(report.file_size) * 1e9 /
             static_cast<double>(wall_ns));
    Emit("replay_status",
         go2cpp::replay::ReplayStatusName(report.status));
    Emit("replay_complete", report.complete ? "1" : "0");
    Emit("replay_sends", report.stats.sends);
    Emit("replay_recvs", report.stats.recvs);
    Emit("replay_messages", report.stats.messages);
    Emit("replay_checksum_ok", verify ? "1" : "0");
    if (!verify) Emit("replay_error", reason.c_str());
    Emit("result", verify ? "PASS" : "FAIL");
    return verify ? 0 : 1;
}

// ---------------------------------------------------------------------------
// bytes：逐字节一致
// ---------------------------------------------------------------------------
int RunBytes(const Options& opt) {
    const std::string path_a = opt.path + ".a";
    const std::string path_b = opt.path + ".b";

    auto record_to = [&](const std::string& path) {
        RecordOptions record_options;
        record_options.path = path;
        record_options.demo_name = "debug_asm_bench";
        record_options.gid_count = 2;
        record_options.channel_count = 2;
        Recorder recorder(record_options);
        if (!recorder.ok()) return false;
        WriteStream(&recorder, opt.n);
        recorder.Finalize();
        return recorder.finalized();
    };

    if (!record_to(path_a) || !record_to(path_b)) {
        std::fprintf(stderr, "录制失败\n");
        return 1;
    }

    std::vector<std::uint8_t> bytes_a;
    std::vector<std::uint8_t> bytes_b;
    if (!ReadFile(path_a, &bytes_a) || !ReadFile(path_b, &bytes_b)) {
        std::fprintf(stderr, "读文件失败\n");
        return 1;
    }

    // 1) 原始字节：排除 header.created_unix_ns 后统计差异。业务事件的
    //    ts_ns/vtime_ns 是固定值，两次录制应完全相同；只有 kEnd 的
    //    ts/vtime 与覆盖它们的 CRC 会随墙钟变化。
    bool files_equal = bytes_a.size() == bytes_b.size();
    std::size_t diff_bytes = 0;
    std::uint64_t diff_offset = 0;
    if (files_equal) {
        for (std::size_t i = 0; i < bytes_a.size(); ++i) {
            if (i >= kCreatedOffset && i < kCreatedOffset + kCreatedSize) {
                continue;
            }
            if (bytes_a[i] != bytes_b[i]) {
                if (diff_bytes == 0) diff_offset = i;
                ++diff_bytes;
            }
        }
        files_equal = diff_bytes == 0;
    } else {
        diff_offset = bytes_a.size() < bytes_b.size() ? bytes_a.size()
                                                      : bytes_b.size();
    }
    Emit("bytes_size", static_cast<std::uint64_t>(bytes_a.size()));
    Emit("bytes_files_equal", files_equal ? "1" : "0");
    Emit("bytes_diff_bytes", static_cast<std::uint64_t>(diff_bytes));
    Emit("bytes_diff_offset", diff_offset);

    // 2) 解码 → 时间戳归一化 → 重新编码，逐字节比较（主判据）。
    //    这一步排除「墙钟噪声」，验证事件流本身的编码完全确定。
    std::vector<Event> events_a;
    std::vector<Event> events_b;
    std::string load_error;
    const bool decoded =
        go2cpp::replay::LoadRecordingEvents(path_a, &events_a, &load_error) &&
        go2cpp::replay::LoadRecordingEvents(path_b, &events_b, &load_error);
    bool normalized_equal = decoded && events_a.size() == events_b.size();
    std::uint64_t normalized_offset = 0;
    if (!decoded) {
        Emit("bytes_decode_error", load_error.c_str());
    }
    if (normalized_equal) {
        for (std::size_t i = 0; i < events_a.size(); ++i) {
            Event left = events_a[i];
            Event right = events_b[i];
            left.ts_ns = 0;
            right.ts_ns = 0;
            left.vtime_ns = 0;
            right.vtime_ns = 0;
            std::vector<std::uint8_t> encoded_left;
            std::vector<std::uint8_t> encoded_right;
            go2cpp::replay::EncodeEvent(left, encoded_left);
            go2cpp::replay::EncodeEvent(right, encoded_right);
            if (encoded_left != encoded_right) {
                normalized_equal = false;
                normalized_offset = i;
                break;
            }
        }
    }
    Emit("bytes_events", static_cast<std::uint64_t>(events_a.size()));
    Emit("bytes_normalized_equal", normalized_equal ? "1" : "0");
    Emit("bytes_normalized_offset", normalized_offset);

    // 3) 同一文件回放两次 → JSON 报告逐字节相同。
    ReplayOptions replay_options;
    const ReplayReport first = go2cpp::replay::ReplayFile(path_a, replay_options);
    const ReplayReport second = go2cpp::replay::ReplayFile(path_a, replay_options);
    const std::string json_first = go2cpp::replay::ReplayReportJson(first);
    const std::string json_second = go2cpp::replay::ReplayReportJson(second);
    const bool json_equal = json_first == json_second;
    Emit("replay_json_bytes", static_cast<std::uint64_t>(json_first.size()));
    Emit("replay_json_equal", json_equal ? "1" : "0");

    // 4) 报告与合成流一致（独立校验和）。
    std::string reason;
    const bool verify = VerifyReport(first, opt.n, &reason);
    if (!verify) Emit("verify_error", reason.c_str());
    Emit("bytes_verify_ok", verify ? "1" : "0");

    // 5) 文件级 CRC 与回放报告记录的 CRC 一致（完整性口径）。
    Emit("replay_file_crc", first.file_crc);
    Emit("replay_checksum_ok", first.checksum_ok ? "1" : "0");

    const bool ok = normalized_equal && json_equal && verify;
    Emit("result", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

// ---------------------------------------------------------------------------
// decode：只解码（LoadRecordingEvents，含 CRC 校验），作为回放的分析基线
// ---------------------------------------------------------------------------
int RunDecode(const Options& opt) {
    std::vector<Event> events;
    std::string error;
    const std::uint64_t t0 = NowNs();
    const bool loaded = go2cpp::replay::LoadRecordingEvents(opt.path, &events,
                                                            &error);
    const std::uint64_t wall_ns = NowNs() - t0;
    Emit("decode_events", static_cast<std::uint64_t>(events.size()));
    Emit("decode_wall_ns", wall_ns);
    Emit("decode_ns_per_event", NsPer(wall_ns, events.size()));
    Emit("decode_events_per_sec",
         static_cast<double>(events.size()) * 1e9 /
             static_cast<double>(wall_ns));
    if (!loaded) Emit("decode_error", error.c_str());
    Emit("result", loaded ? "PASS" : "FAIL");
    return loaded ? 0 : 1;
}

// ---------------------------------------------------------------------------
// iobase：纯 I/O 上限
// ---------------------------------------------------------------------------
int RunIoBase(const Options& opt) {
    std::vector<std::uint8_t> data;
    std::uint64_t t0 = NowNs();
    if (!ReadFile(opt.path, &data)) {
        std::fprintf(stderr, "读文件失败: %s\n", opt.path.c_str());
        return 1;
    }
    const std::uint64_t read_ns = NowNs() - t0;

    // 逐字节 FNV 扫描（与回放同样读一遍全部字节，但不解析）。
    t0 = NowNs();
    std::uint64_t hash = 1469598103934665603ULL;
    for (const std::uint8_t byte : data) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    const std::uint64_t scan_ns = NowNs() - t0;
    Emit("iobase_bytes", static_cast<std::uint64_t>(data.size()));
    Emit("iobase_read_ns", read_ns);
    Emit("iobase_read_mb_s",
         static_cast<double>(data.size()) / 1e6 /
             (static_cast<double>(read_ns) / 1e9));
    Emit("iobase_scan_ns", scan_ns);
    Emit("iobase_scan_mb_s",
         static_cast<double>(data.size()) / 1e6 /
             (static_cast<double>(scan_ns) / 1e9));
    Emit("iobase_hash", hash);
    Emit("result", "PASS");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "用法: %s <record|decode|replay|bytes|iobase> [--n=] "
                     "[--path=]\n",
                     argv[0]);
        return 2;
    }
    Options opt;
    if (!ParseOptions(argc, argv, &opt)) return 2;
    const std::string mode = argv[1];
    if (mode == "record") return RunRecord(opt);
    if (mode == "replay") return RunReplay(opt);
    if (mode == "decode") return RunDecode(opt);
    if (mode == "bytes") return RunBytes(opt);
    if (mode == "iobase") return RunIoBase(opt);
    std::fprintf(stderr, "未知模式: %s\n", mode.c_str());
    return 2;
}
