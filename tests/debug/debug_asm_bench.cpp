/**
 * @file debug_asm_bench.cpp
 * @brief 事件环热路径微基准 + 正确性（1e6 事件 / dump 序列化 / 落盘）。
 *
 * 为什么需要：objdump 只能回答「函数里有什么指令」，回答不了「这些指令
 * 在真实调用频率下值多少时间」。本程序把事件环的三段热路径拆开：
 *   emit  —— EventRing::Record（清 seq + 批量写槽位 + 累计计数原子）；
 *   dump  —— Snapshot + DumpText/DumpJson（序列化/flush 前的聚合与格式化）；
 *   write —— WriteToFile（文本/JSON 落盘，含 ofstream flush）。
 *
 * 同时做正确性断言（非零退出码 = 失败）：
 *   - 1e6 条事件累计计数守恒；
 *   - 快照按 (ts_ns, thread_id, seq) 严格升序、同线程 seq 唯一；
 *   - 文本行数 / JSON seq 数与 event_count 一致；
 *   - 无并发写入时两次 dump 逐字节一致（确定性）；
 *   - ReadSince 分页遍历恰好覆盖快照（不重不漏）。
 *
 * 输出：一行一个 KEY<TAB>VALUE，便于 shell 汇总。退出码 0=全部通过。
 *
 * 用法：
 *   debug_asm_bench emit  [--n=1000000] [--threads=4]
 *   debug_asm_bench dump  [--threads=64] [--iters=200]
 *   debug_asm_bench write [--iters=100] [--path=/tmp/debug_asm_dump.txt]
 *   debug_asm_bench off   （OFF 构建零记录契约）
 */

#include "go2cpp/debug/debug_event_ring.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using go2cpp::debug::EventCursor;
using go2cpp::debug::EventRecord;
using go2cpp::debug::EventRing;
using go2cpp::debug::SchedEventCode;

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
    std::uint32_t threads{4};
    std::uint32_t iters{200};
    std::uint32_t per_thread{320};  // 256 普通 + 64 关键
    std::string path{"/tmp/debug_asm_dump.txt"};
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
        } else if (std::strncmp(arg, "--threads=", 10) == 0) {
            std::uint64_t value = 0;
            if (!ParseU64(arg + 10, &value)) return false;
            opt->threads = static_cast<std::uint32_t>(value);
        } else if (std::strncmp(arg, "--iters=", 8) == 0) {
            std::uint64_t value = 0;
            if (!ParseU64(arg + 8, &value)) return false;
            opt->iters = static_cast<std::uint32_t>(value);
        } else if (std::strncmp(arg, "--path=", 7) == 0) {
            opt->path = arg + 7;
        } else {
            std::fprintf(stderr, "未知参数: %s\n", arg);
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// emit：单线程 1e6 + 多线程 1e6
// ---------------------------------------------------------------------------
int RunEmit(const Options& opt) {
    EventRing::ResetTotals();
    EventRing::Reset();

    // 物理下限：同样次数的空循环（只有循环控制，没有事件写入）。
    const std::uint64_t n = opt.n;
    std::uint64_t t0 = NowNs();
    for (std::uint64_t i = 0; i < n; ++i) {
        __asm__ __volatile__("" ::: "memory");
    }
    const std::uint64_t loop_ns = NowNs() - t0;
    Emit("loop_ns", loop_ns);
    Emit("loop_ns_per_iter", NsPer(loop_ns, n));

    // 单线程稳态：预热 1000 条后计时。
    for (std::uint32_t w = 0; w < 1000; ++w) {
        EventRing::Record(SchedEventCode::kMutexUnlock, w, 0);
    }
    const std::uint64_t single_before = EventRing::TotalCountAll();
    t0 = NowNs();
    for (std::uint64_t i = 0; i < n; ++i) {
        EventRing::Record(SchedEventCode::kMutexUnlock, i, 0);
    }
    const std::uint64_t single_ns = NowNs() - t0;
    const std::uint64_t single_delta =
        EventRing::TotalCountAll() - single_before;
    Emit("single_n", n);
    Emit("single_wall_ns", single_ns);
    Emit("single_ns_per_event", NsPer(single_ns, n));
    Emit("single_count_delta", single_delta);
    Emit("single_count_ok", single_delta == n ? "1" : "0");

    // 多线程：threads 个线程各 n/threads 条，同一屏障起跑。
    EventRing::ResetTotals();
    const std::uint32_t threads = opt.threads == 0 ? 1 : opt.threads;
    const std::uint64_t per = n / threads;
    std::atomic<bool> go{false};
    std::atomic<std::uint64_t> done{0};
    std::vector<std::thread> pool;
    pool.reserve(threads);
    for (std::uint32_t t = 0; t < threads; ++t) {
        pool.emplace_back([&go, &done, per]() {
            while (!go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (std::uint64_t i = 0; i < per; ++i) {
                EventRing::Record(SchedEventCode::kMutexUnlock, i, 0);
            }
            done.fetch_add(per, std::memory_order_relaxed);
        });
    }
    const std::uint64_t multi_before = EventRing::TotalCountAll();
    t0 = NowNs();
    go.store(true, std::memory_order_release);
    for (auto& thread : pool) thread.join();
    const std::uint64_t multi_ns = NowNs() - t0;
    const std::uint64_t multi_delta =
        EventRing::TotalCountAll() - multi_before;
    const std::uint64_t expected = per * threads;
    Emit("multi_threads", threads);
    Emit("multi_n", expected);
    Emit("multi_wall_ns", multi_ns);
    Emit("multi_ns_per_event", NsPer(multi_ns, expected));
    Emit("multi_count_delta", multi_delta);
    Emit("multi_count_ok", multi_delta == expected ? "1" : "0");
    Emit("multi_ops_per_sec",
         static_cast<double>(expected) * 1e9 /
             static_cast<double>(multi_ns == 0 ? 1 : multi_ns));

    const bool ok = single_delta == n && multi_delta == expected;
    Emit("result", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

// ---------------------------------------------------------------------------
// dump：多线程填满两段环后，测快照与序列化
// ---------------------------------------------------------------------------
int RunDump(const Options& opt) {
    const std::uint32_t threads = opt.threads == 0 ? 1 : opt.threads;
    if (threads > 120) {
        std::fprintf(stderr, "--threads 上限 120（kMaxThreads=128）\n");
        return 2;
    }
    EventRing::ResetTotals();
    EventRing::Reset();

    std::atomic<std::uint32_t> ready{0};
    std::atomic<bool> release{false};
    std::vector<std::thread> pool;
    pool.reserve(threads);
    for (std::uint32_t t = 0; t < threads; ++t) {
        pool.emplace_back([&ready, &release]() {
            // 普通区 256 条 + 关键区 64 条，两段都写满。
            for (std::uint32_t i = 0; i < 256; ++i) {
                EventRing::Record(SchedEventCode::kMutexUnlock, i, 0);
            }
            for (std::uint32_t i = 0; i < 64; ++i) {
                EventRing::Record(SchedEventCode::kWakeEnqueued, i, 0);
            }
            ready.fetch_add(1, std::memory_order_release);
            while (!release.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
        });
    }
    while (ready.load(std::memory_order_acquire) < threads) {
        std::this_thread::yield();
    }

    const std::vector<EventRecord> snapshot = EventRing::Snapshot(0);
    const std::uint64_t events = snapshot.size();
    Emit("dump_threads", threads);
    Emit("dump_events", events);
    Emit("dump_expected_events",
         static_cast<std::uint64_t>(threads) * opt.per_thread);
    Emit("dump_events_ok",
         events == static_cast<std::uint64_t>(threads) * opt.per_thread ? "1"
                                                                       : "0");

    // 排序 + 唯一性校验。
    bool sorted = true;
    bool unique = true;
    for (std::size_t i = 1; i < snapshot.size(); ++i) {
        const EventRecord& a = snapshot[i - 1];
        const EventRecord& b = snapshot[i];
        const bool less = a.ts_ns < b.ts_ns ||
                          (a.ts_ns == b.ts_ns && a.thread_id < b.thread_id) ||
                          (a.ts_ns == b.ts_ns && a.thread_id == b.thread_id &&
                           a.seq < b.seq);
        const bool equal = a.ts_ns == b.ts_ns &&
                           a.thread_id == b.thread_id && a.seq == b.seq;
        if (!less && !equal) sorted = false;
        if (equal) unique = false;
    }
    Emit("snapshot_sorted", sorted ? "1" : "0");
    Emit("snapshot_unique", unique ? "1" : "0");

    // 快照计时（重复取同一组数据）。
    std::uint64_t snapshot_bytes = 0;
    std::uint64_t t0 = NowNs();
    for (std::uint32_t i = 0; i < opt.iters; ++i) {
        const std::vector<EventRecord> again = EventRing::Snapshot(0);
        snapshot_bytes += again.size();
    }
    const std::uint64_t snapshot_ns = NowNs() - t0;
    Emit("snapshot_calls", opt.iters);
    Emit("snapshot_ns_per_call", NsPer(snapshot_ns, opt.iters));
    Emit("snapshot_events_per_call",
         static_cast<double>(snapshot_bytes) / opt.iters);

    // DumpText / DumpJson 计时。
    const std::string text = EventRing::DumpText(0);
    const std::string json = EventRing::DumpJson(0);
    Emit("text_bytes", static_cast<std::uint64_t>(text.size()));
    Emit("json_bytes", static_cast<std::uint64_t>(json.size()));

    std::uint64_t text_bytes = 0;
    t0 = NowNs();
    for (std::uint32_t i = 0; i < opt.iters; ++i) {
        text_bytes += EventRing::DumpText(0).size();
    }
    const std::uint64_t text_ns = NowNs() - t0;
    std::uint64_t json_bytes = 0;
    t0 = NowNs();
    for (std::uint32_t i = 0; i < opt.iters; ++i) {
        json_bytes += EventRing::DumpJson(0).size();
    }
    const std::uint64_t json_ns = NowNs() - t0;
    Emit("text_ns_per_call", NsPer(text_ns, opt.iters));
    Emit("json_ns_per_call", NsPer(json_ns, opt.iters));
    Emit("text_mb_per_sec",
         static_cast<double>(text_bytes) / 1e6 /
             (static_cast<double>(text_ns) / 1e9));
    Emit("json_mb_per_sec",
         static_cast<double>(json_bytes) / 1e6 /
             (static_cast<double>(json_ns) / 1e9));
    Emit("text_ns_per_event", NsPer(text_ns, opt.iters * events));
    Emit("json_ns_per_event", NsPer(json_ns, opt.iters * events));

    // 文本内容校验：event_count 行 + 事件行数。
    const std::string marker = "# seq ts_ns tid code gid arg2 pid\n";
    const std::size_t marker_pos = text.find(marker);
    std::size_t text_rows = 0;
    if (marker_pos != std::string::npos) {
        const std::size_t begin = marker_pos + marker.size();
        for (std::size_t i = begin; i < text.size(); ++i) {
            if (text[i] == '\n') ++text_rows;
        }
    }
    bool text_count_ok = false;
    {
        const std::string key = "event_count=";
        const std::size_t pos = text.find(key);
        if (pos != std::string::npos) {
            const std::uint64_t parsed =
                std::strtoull(text.c_str() + pos + key.size(), nullptr, 10);
            text_count_ok = parsed == events && text_rows == events;
        }
    }
    Emit("text_rows", static_cast<std::uint64_t>(text_rows));
    Emit("text_count_ok", text_count_ok ? "1" : "0");

    // JSON 内容校验：seq 出现次数 == 事件数。
    std::size_t json_seq = 0;
    {
        const std::string key = "\"seq\":";
        std::size_t pos = 0;
        while ((pos = json.find(key, pos)) != std::string::npos) {
            ++json_seq;
            pos += key.size();
        }
    }
    Emit("json_seq_count", static_cast<std::uint64_t>(json_seq));
    Emit("json_count_ok", json_seq == events ? "1" : "0");

    // 确定性：无并发写入时两次 dump 逐字节一致。
    const bool stable = EventRing::DumpText(0) == text &&
                        EventRing::DumpJson(0) == json;
    Emit("dump_stable", stable ? "1" : "0");

    // ReadSince 分页遍历：从 {0,0,0} 起每页 97 条，累计应恰好覆盖快照。
    std::uint64_t read_total = 0;
    bool read_order_ok = true;
    EventCursor cursor{0, 0, 0};
    for (;;) {
        const std::vector<EventRecord> page = EventRing::ReadSince(cursor, 97);
        if (page.empty()) break;
        for (const EventRecord& record : page) {
            if (!go2cpp::debug::EventAfterCursor(record, cursor)) {
                read_order_ok = false;
            }
        }
        cursor = go2cpp::debug::CursorOf(page.back());
        read_total += page.size();
        if (read_total > events + 1000) break;  // 防御：游标不前进
    }
    Emit("read_since_total", read_total);
    Emit("read_since_ok",
         (read_total == events && read_order_ok) ? "1" : "0");

    release.store(true, std::memory_order_release);
    for (auto& thread : pool) thread.join();

    const bool ok =
        events == static_cast<std::uint64_t>(threads) * opt.per_thread &&
        sorted && unique && text_count_ok && json_seq == events && stable &&
        read_total == events && read_order_ok;
    Emit("result", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

// ---------------------------------------------------------------------------
// write：WriteToFile 落盘吞吐
// ---------------------------------------------------------------------------
int RunWrite(const Options& opt) {
    EventRing::Reset();
    for (std::uint32_t i = 0; i < 320; ++i) {
        EventRing::Record(i < 256 ? SchedEventCode::kMutexUnlock
                                  : SchedEventCode::kWakeEnqueued,
                          i, 0);
    }
    const std::uint64_t events = EventRing::Snapshot(0).size();
    Emit("write_events", events);

    std::string error;
    const std::uint64_t text_bytes = EventRing::DumpText(0).size();
    std::uint64_t t0 = NowNs();
    for (std::uint32_t i = 0; i < opt.iters; ++i) {
        if (!EventRing::WriteToFile(opt.path, false, &error, 0)) {
            std::fprintf(stderr, "WriteToFile(text) 失败: %s\n", error.c_str());
            return 1;
        }
    }
    const std::uint64_t text_ns = NowNs() - t0;
    Emit("write_text_iters", opt.iters);
    Emit("write_text_bytes", text_bytes);
    Emit("write_text_ns_per_call", NsPer(text_ns, opt.iters));
    Emit("write_text_mb_per_sec",
         static_cast<double>(text_bytes) * opt.iters / 1e6 /
             (static_cast<double>(text_ns) / 1e9));

    const std::string json_path = opt.path + ".json";
    const std::uint64_t json_bytes = EventRing::DumpJson(0).size();
    t0 = NowNs();
    for (std::uint32_t i = 0; i < opt.iters; ++i) {
        if (!EventRing::WriteToFile(json_path, true, &error, 0)) {
            std::fprintf(stderr, "WriteToFile(json) 失败: %s\n", error.c_str());
            return 1;
        }
    }
    const std::uint64_t json_ns = NowNs() - t0;
    Emit("write_json_iters", opt.iters);
    Emit("write_json_bytes", json_bytes);
    Emit("write_json_ns_per_call", NsPer(json_ns, opt.iters));
    Emit("write_json_mb_per_sec",
         static_cast<double>(json_bytes) * opt.iters / 1e6 /
             (static_cast<double>(json_ns) / 1e9));

    // 落盘字节数必须与内存序列化字节数一致（无截断/无额外包装）。
    bool size_ok = true;
    for (const std::string& path : {opt.path, json_path}) {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) {
            size_ok = false;
            continue;
        }
        const std::streamoff size = file.tellg();
        const std::uint64_t expected =
            path == opt.path ? text_bytes : json_bytes;
        if (static_cast<std::uint64_t>(size) != expected) size_ok = false;
    }
    Emit("write_size_ok", size_ok ? "1" : "0");
    Emit("result", size_ok ? "PASS" : "FAIL");
    return size_ok ? 0 : 1;
}

// ---------------------------------------------------------------------------
// off：OFF 构建的零记录契约
// ---------------------------------------------------------------------------
int RunOff() {
    Emit("enabled", EventRing::Enabled() ? "1" : "0");
    EventRing::ResetTotals();
    for (std::uint32_t i = 0; i < 100000; ++i) {
        EventRing::Record(SchedEventCode::kMutexUnlock, i, 0);
    }
    Emit("off_total_count", EventRing::TotalCountAll());
    Emit("off_snapshot",
         static_cast<std::uint64_t>(EventRing::Snapshot(0).size()));
    const bool ok = !EventRing::Enabled() && EventRing::TotalCountAll() == 0 &&
                    EventRing::Snapshot(0).empty();
    Emit("result", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "用法: %s <emit|dump|write|off> [--n=] [--threads=] "
                     "[--iters=] [--path=]\n",
                     argv[0]);
        return 2;
    }
    Options opt;
    if (!ParseOptions(argc, argv, &opt)) return 2;
    const std::string mode = argv[1];
    if (mode == "emit") return RunEmit(opt);
    if (mode == "dump") return RunDump(opt);
    if (mode == "write") return RunWrite(opt);
    if (mode == "off") return RunOff();
    std::fprintf(stderr, "未知模式: %s\n", mode.c_str());
    return 2;
}
