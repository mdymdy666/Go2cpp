/**
 * @file tests/logging/test_log_rotate.cpp
 * @brief 日志轮转（`go2cpp/logging/rotate.hpp`）的场景/压测/随机测试。
 *
 * ## 覆盖
 *
 * 1. **按大小轮转 + gzip 归档 + 保留策略**：小上限触发十余次轮转，所有归档
 *    都能被独立 gzip 解压（CRC/ISIZE 校验由 `Gzip::Reader` 完成），合并后
 *    4000 行严格递增、无丢失、无乱序；
 * 2. **按天轮转（注入时钟）**：同一天不轮转、跨天自动轮转；证明周期判断
 *    不依赖系统时钟，测试可复现；
 * 3. **保留 N 份**：连续 20 次轮转后归档数量恰好等于 `max_files`，最旧的
 *    被删除且最新内容仍在；
 * 4. **不压缩**：归档保留明文，`RotatingLogSink`（`LogSink` 适配器）写入
 *    路径可用；
 * 5. **并发写不丢**：8 线程 × 5000 条随机长度记录 + 高频轮转，合并后每条
 *    id 恰好出现一次、每行完整（无撕裂）；
 * 6. **失败恢复**：目标路径先是目录（`fopen` 必失败），数据留在内存缓冲；
 *    障碍移除后同一条记录完整落盘，`dropped_bytes() == 0`；
 * 7. **1e6 条 / 轮转 10 次压测**：统计写吞吐、关闭（压缩归档）耗时，并逐行
 *    验证 0..999999 完整递增；
 * 8. **随机混合**：随机记录长度 + 随机上限 + 随机轮转，验证不丢。
 *
 * ## 运行
 * ```sh
 * g++ -std=c++17 -O2 -Iinclude tests/logging/test_log_rotate.cpp \
 *     -L.build-bento-wsl -lgo2cpp_log -lgo2cpp_compress -lgo2cpp_error \
 *     -Wl,-rpath,$PWD/.build-bento-wsl -pthread -o /tmp/test_log_rotate
 * /tmp/test_log_rotate
 * ```
 */
#include "go2cpp/compress/gzip.hpp"
#include "go2cpp/error.hpp"
#include "go2cpp/logging/rotate.hpp"
#include "go2cpp/streams.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using go2cpp::log::GzipCodec;
using go2cpp::log::RotationOptions;
using go2cpp::log::RotationPeriod;
using go2cpp::log::RotatingLogFile;
using go2cpp::log::RotatingLogSink;
using Clock = std::chrono::steady_clock;

int g_failures = 0;

/// 统一失败输出：文件:行 + 表达式。
void Check(bool condition, const char* expression, const char* file, int line) {
    if (condition) {
        return;
    }
    ++g_failures;
    std::fprintf(stderr, "[log-rotate] FAILED %s:%d: %s\n", file, line,
                 expression);
}

#define CHECK(condition) Check((condition), #condition, __FILE__, __LINE__)

/// 每个场景一个独立临时目录；析构时整体删除，保证无残留。
class TempDir final {
public:
    explicit TempDir(const char* tag) {
        std::string pattern = std::string("/tmp/go2cpp-log-rotate-") + tag +
                              "-XXXXXX";
        std::vector<char> buffer(pattern.begin(), pattern.end());
        buffer.push_back('\0');
        char* created = ::mkdtemp(buffer.data());
        if (created != nullptr) {
            m_path = created;
        }
    }

    ~TempDir() {
        if (!m_path.empty()) {
            std::error_code error;
            std::filesystem::remove_all(m_path, error);
        }
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::string& path() const { return m_path; }

    std::string file(const std::string& name) const {
        return m_path + "/" + name;
    }

private:
    std::string m_path;
};

std::string ReadFile(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {};
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

/// 文件读取适配 `Gzip::Reader` 的 `streams::Reader` 契约。
struct FileReader {
    std::FILE* file{nullptr};

    go2cpp::streams::ReadResult Read(char* buffer, std::size_t size) {
        const std::size_t got = std::fread(buffer, 1, size, file);
        if (got == 0) {
            if (std::ferror(file) != 0) {
                return {0, go2cpp::streams::IoErrUnexpectedEof()};
            }
            return {0, go2cpp::streams::IoEof()};
        }
        return {got, {}};
    }
};

/// 解压一个 gzip 文件；失败返回空串（调用方用 CHECK 断言非空）。
std::string Gunzip(const std::string& path) {
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return {};
    }
    FileReader source{file};
    go2cpp::compress::Gzip::Reader<FileReader> reader(source);
    std::string output;
    std::vector<char> buffer(64u * 1024u);
    for (;;) {
        const std::size_t got = reader.Read(
            go2cpp::compress::AsBytes(buffer.data(), buffer.size()));
        if (got == 0) {
            break;
        }
        output.append(buffer.data(), got);
    }
    const auto error = reader.Err();
    const bool ok = !error || go2cpp::Is(error, go2cpp::streams::IoEof());
    std::fclose(file);
    return ok ? output : std::string();
}

bool EndsWith(const std::string& text, const char* suffix) {
    const std::size_t length = std::strlen(suffix);
    return text.size() >= length &&
           text.compare(text.size() - length, length, suffix) == 0;
}

std::string ArchiveSortKey(const std::string& path) {
    return EndsWith(path, ".gz") ? path.substr(0, path.size() - 3) : path;
}

/// 列出某个日志文件的全部归档（不含当前文件），按轮转顺序排序。
std::vector<std::string> ListArchives(const std::string& base) {
    std::vector<std::string> archives;
    std::error_code error;
    const std::filesystem::path path(base);
    const std::filesystem::path directory = path.parent_path();
    const std::string prefix = path.filename().string() + ".";
    for (std::filesystem::directory_iterator it(directory, error), end;
         !error && it != end; it.increment(error)) {
        const std::string name = it->path().filename().string();
        if (name.rfind(prefix, 0) != 0) {
            continue;
        }
        if (EndsWith(name, ".tmp")) {
            continue;
        }
        archives.push_back(it->path().string());
    }
    std::sort(archives.begin(), archives.end(),
              [](const std::string& left, const std::string& right) {
                  return ArchiveSortKey(left) < ArchiveSortKey(right);
              });
    return archives;
}

std::string ArchiveContent(const std::string& path) {
    if (EndsWith(path, ".gz")) {
        return Gunzip(path);
    }
    return ReadFile(path);
}

/// 合并「全部归档 + 当前文件」的内容，顺序即轮转顺序。
std::string ReadAllLogs(const std::string& base) {
    std::string merged;
    for (const std::string& archive : ListArchives(base)) {
        merged += ArchiveContent(archive);
    }
    if (std::filesystem::exists(base)) {
        merged += ReadFile(base);
    }
    return merged;
}

/// 从一行中解析 `seq=<数字>`；格式不完整返回 false。
bool ParseSequence(const std::string& line, std::uint64_t* value) {
    if (line.rfind("seq=", 0) != 0) {
        return false;
    }
    std::size_t position = 4;
    std::uint64_t result = 0;
    std::size_t digits = 0;
    while (position < line.size() && line[position] >= '0' &&
           line[position] <= '9') {
        result = result * 10 + static_cast<std::uint64_t>(line[position] - '0');
        ++position;
        ++digits;
    }
    if (digits == 0 || position >= line.size() || line[position] != ' ') {
        return false;
    }
    *value = result;
    return true;
}

/// 逐行校验：行完整、id 从 0 连续递增、总行数等于期望。
bool CheckSequential(const std::string& text, std::uint64_t expected,
                     std::string* detail) {
    std::size_t position = 0;
    std::uint64_t index = 0;
    while (position < text.size()) {
        const std::size_t end = text.find('\n', position);
        if (end == std::string::npos) {
            *detail = "最后一行缺少换行（记录被撕裂）";
            return false;
        }
        const std::string line = text.substr(position, end - position);
        std::uint64_t value = 0;
        if (!ParseSequence(line, &value)) {
            *detail = "行格式错误: " + line.substr(0, 80);
            return false;
        }
        if (value != index) {
            *detail = "顺序错误：期望 " + std::to_string(index) + "，实际 " +
                      std::to_string(value);
            return false;
        }
        ++index;
        position = end + 1;
    }
    if (index != expected) {
        *detail = "行数错误：期望 " + std::to_string(expected) + "，实际 " +
                  std::to_string(index);
        return false;
    }
    return true;
}

void Announce(const char* name) {
    std::cout << "[test] " << name << std::endl;
}

std::string MakeLine(std::uint64_t index, std::size_t payload = 24) {
    std::string line = "seq=" + std::to_string(index) + " msg=";
    line.append(payload, static_cast<char>('a' + (index % 26)));
    line.push_back('\n');
    return line;
}

// ---------------------------------------------------------------------------
// 场景 1：按大小轮转 + gzip + 保留
// ---------------------------------------------------------------------------
void ScenarioSizeAndGzip() {
    Announce("size-rotation + gzip + retention");
    TempDir temp("size");
    const std::string base = temp.file("app.log");
    RotationOptions options;
    options.max_bytes = 4u * 1024u;
    // 本场景验证「完整序列」，必须保留全部 40 个归档；保留策略由场景 3 覆盖。
    options.max_files = 64;
    options.write_buffer_bytes = 1024;
    options.compression_level = GzipCodec::BestSpeed;

    RotatingLogFile file(base, options);
    constexpr std::uint64_t kLines = 4000;
    for (std::uint64_t i = 0; i < kLines; ++i) {
        const std::string line = MakeLine(i);
        CHECK(file.Write(line) == line.size());
    }
    CHECK(file.Flush());
    file.Close();

    CHECK(file.dropped_bytes() == 0);
    CHECK(file.unflushed_bytes() == 0);
    CHECK(file.rotations() >= 10);
    CHECK(file.compress_errors() == 0);
    CHECK(file.pruned_files() == 0);

    const auto archives = ListArchives(base);
    CHECK(!archives.empty());
    CHECK(archives.size() <= options.max_files);
    std::size_t compressed = 0;
    for (const std::string& archive : archives) {
        if (EndsWith(archive, ".gz")) {
            ++compressed;
            CHECK(!Gunzip(archive).empty());
        }
    }
    CHECK(compressed > 0);

    std::string detail;
    const std::string merged = ReadAllLogs(base);
    CHECK(CheckSequential(merged, kLines, &detail));
    if (!detail.empty()) {
        std::fprintf(stderr, "[log-rotate] %s\n", detail.c_str());
    }
}

// ---------------------------------------------------------------------------
// 场景 2：按天轮转（注入时钟）
// ---------------------------------------------------------------------------
void ScenarioDailyRotation() {
    Announce("daily rotation with injected clock");
    TempDir temp("daily");
    const std::string base = temp.file("daily.log");
    std::atomic<int> day{0};
    const auto origin = std::chrono::system_clock::from_time_t(1767225600);

    RotationOptions options;
    options.max_bytes = 0;  // 只按天
    options.max_files = 8;
    options.period = RotationPeriod::kDaily;
    options.compress = false;
    options.clock = [&day, origin]() {
        return origin + std::chrono::hours(24 * day.load());
    };

    RotatingLogFile file(base, options);
    CHECK(file.Write("day0-a\n") == 7);
    CHECK(file.Write("day0-b\n") == 7);
    CHECK(file.rotations() == 0);

    day.store(1);
    CHECK(file.Write("day1-a\n") == 7);
    CHECK(file.rotations() == 1);
    CHECK(file.Write("day1-b\n") == 7);
    CHECK(file.rotations() == 1);

    day.store(2);
    CHECK(file.Write("day2-a\n") == 7);
    CHECK(file.rotations() == 2);
    file.Close();

    CHECK(file.dropped_bytes() == 0);
    const std::string merged = ReadAllLogs(base);
    CHECK(merged == "day0-a\nday0-b\nday1-a\nday1-b\nday2-a\n");
}

// ---------------------------------------------------------------------------
// 场景 3：保留 N 份（删除最旧）
// ---------------------------------------------------------------------------
void ScenarioRetention() {
    Announce("retention keeps newest N archives");
    TempDir temp("retention");
    const std::string base = temp.file("retain.log");
    RotationOptions options;
    options.max_bytes = 200;
    options.max_files = 3;
    options.compress = false;
    options.write_buffer_bytes = 64;

    RotatingLogFile file(base, options);
    for (std::uint64_t i = 0; i < 40; ++i) {
        const std::string line = MakeLine(i, 24);  // 约 40 字节，5 条一轮转
        CHECK(file.Write(line) == line.size());
    }
    file.Close();

    const auto archives = ListArchives(base);
    CHECK(archives.size() == 3);
    CHECK(file.rotations() >= 6);
    CHECK(file.pruned_files() >= 3);
    // 最新 3 个归档 + 当前文件应能拼出最后一段连续序列。
    const std::string merged = ReadAllLogs(base);
    CHECK(!merged.empty());
    const std::size_t last_newline = merged.rfind('\n');
    CHECK(last_newline != std::string::npos);
    const std::size_t previous_newline =
        last_newline == 0 ? std::string::npos
                          : merged.rfind('\n', last_newline - 1);
    const std::string last_line =
        previous_newline == std::string::npos
            ? merged.substr(0, last_newline)
            : merged.substr(previous_newline + 1,
                            last_newline - previous_newline - 1);
    std::uint64_t last_value = 0;
    CHECK(ParseSequence(last_line, &last_value));
    CHECK(last_value == 39);
}

// ---------------------------------------------------------------------------
// 场景 4：不压缩 + LogSink 适配器
// ---------------------------------------------------------------------------
void ScenarioSinkWithoutCompression() {
    Announce("RotatingLogSink without compression");
    TempDir temp("sink");
    const std::string base = temp.file("sink.log");
    RotationOptions options;
    options.max_bytes = 0;  // 不轮转，只验证 Sink 路径
    options.compress = false;
    RotatingLogSink sink(base, options);

    go2cpp::log::LogRecord record;
    record.level = go2cpp::log::Level::Info;
    record.message = "hello";
    for (int i = 0; i < 100; ++i) {
        sink.Write(record, MakeLine(static_cast<std::uint64_t>(i)));
    }
    CHECK(sink.WriteRaw("seq=100 msg=raw-write\n"));
    sink.Flush();
    CHECK(sink.Reopen());
    const std::string after_reopen = "seq=101 msg=after-reopen\n";
    CHECK(sink.file().Write(after_reopen) == after_reopen.size());
    sink.Flush();
    sink.file().Close();

    CHECK(sink.sink_errors() == 0);
    const std::string content = ReadFile(base);
    CHECK(content.find("seq=0 msg=") == 0);
    CHECK(content.find("seq=99 msg=") != std::string::npos);
    CHECK(content.find("seq=100 msg=raw-write\n") != std::string::npos);
    CHECK(content.find("seq=101 msg=after-reopen\n") != std::string::npos);
    std::string detail;
    CHECK(CheckSequential(content, 102, &detail));
    if (!detail.empty()) {
        std::fprintf(stderr, "[log-rotate] %s\n", detail.c_str());
    }
}

// ---------------------------------------------------------------------------
// 场景 5：并发写 + 高频轮转不丢
// ---------------------------------------------------------------------------
void ScenarioConcurrentWrites() {
    Announce("8 threads x 5000 records with frequent rotation");
    TempDir temp("concurrent");
    const std::string base = temp.file("concurrent.log");
    RotationOptions options;
    // 40000 条 × 约 50 字节 ≈ 2 MB；256 KB 一轮转约 8 次，保留 16 份以覆盖全部。
    options.max_bytes = 256u * 1024u;
    options.max_files = 16;
    options.write_buffer_bytes = 2048;
    options.compression_level = GzipCodec::BestSpeed;

    RotatingLogFile file(base, options);
    constexpr int kThreads = 8;
    constexpr std::uint64_t kPerThread = 5000;
    std::vector<std::thread> writers;
    writers.reserve(kThreads);
    for (int thread_index = 0; thread_index < kThreads; ++thread_index) {
        writers.emplace_back([&file, thread_index]() {
            for (std::uint64_t i = 0; i < kPerThread; ++i) {
                const std::uint64_t id =
                    static_cast<std::uint64_t>(thread_index) * kPerThread + i;
                // 随机长度：10..90 字节，覆盖缓冲边界的各种切分。
                const std::size_t payload =
                    10 + static_cast<std::size_t>((id * 37) % 80);
                const std::string line = MakeLine(id, payload);
                (void)file.Write(line);
            }
        });
    }
    for (auto& writer : writers) {
        writer.join();
    }
    CHECK(file.Flush());
    file.Close();

    CHECK(file.dropped_bytes() == 0);
    CHECK(file.unflushed_bytes() == 0);
    CHECK(file.rotations() >= 5);
    CHECK(file.compress_errors() == 0);

    const std::string merged = ReadAllLogs(base);
    constexpr std::uint64_t kTotal = kThreads * kPerThread;
    std::vector<std::uint8_t> seen(static_cast<std::size_t>(kTotal), 0);
    std::uint64_t count = 0;
    std::uint64_t duplicates = 0;
    std::uint64_t malformed = 0;
    std::size_t position = 0;
    while (position < merged.size()) {
        const std::size_t end = merged.find('\n', position);
        if (end == std::string::npos) {
            ++malformed;
            break;
        }
        const std::string line = merged.substr(position, end - position);
        std::uint64_t value = 0;
        if (!ParseSequence(line, &value) || value >= kTotal) {
            ++malformed;
        } else if (seen[static_cast<std::size_t>(value)] != 0) {
            ++duplicates;
        } else {
            seen[static_cast<std::size_t>(value)] = 1;
        }
        ++count;
        position = end + 1;
    }
    CHECK(malformed == 0);
    CHECK(duplicates == 0);
    CHECK(count == kTotal);
}

// ---------------------------------------------------------------------------
// 场景 6：失败恢复（数据留在缓冲，不丢）
// ---------------------------------------------------------------------------
void ScenarioFailureRecovery() {
    Announce("open failure keeps buffered records, recovers later");
    TempDir temp("failure");
    const std::string base = temp.file("blocked.log");
    std::filesystem::create_directories(base);  // fopen 必失败（EISDIR）

    RotationOptions options;
    options.max_bytes = 0;
    options.compress = false;
    options.write_buffer_bytes = 8;
    RotatingLogFile file(base, options);

    CHECK(file.Write("alpha\n") == 6);
    CHECK(file.pending_bytes() == 6);
    CHECK(file.flush_errors() == 0);
    // 第二次写入超过 8 字节阈值，触发一次必然失败的刷盘。
    CHECK(file.Write("beta\n") == 5);
    CHECK(file.pending_bytes() == 11);
    CHECK(file.flush_errors() > 0);
    CHECK(file.dropped_bytes() == 0);

    std::error_code error;
    std::filesystem::remove(base, error);
    CHECK(!error);

    CHECK(file.Write("gamma\n") == 6);
    CHECK(file.Flush());
    CHECK(file.pending_bytes() == 0);
    file.Close();

    const std::string content = ReadFile(base);
    CHECK(content == "alpha\nbeta\ngamma\n");
    CHECK(file.dropped_bytes() == 0);
}

// ---------------------------------------------------------------------------
// 场景 7：1e6 条 / 轮转 10 次压测
// ---------------------------------------------------------------------------
void ScenarioMillionRecords() {
    Announce("bench: 1e6 records, >=10 rotations");
    TempDir temp("million");
    const std::string base = temp.file("million.log");
    RotationOptions options;
    options.max_bytes = 3u * 1024u * 1024u;
    options.max_files = 20;
    options.write_buffer_bytes = 256u * 1024u;
    options.compression_level = GzipCodec::BestSpeed;

    RotatingLogFile file(base, options);
    constexpr std::uint64_t kLines = 1000000;
    std::uint64_t bytes = 0;
    const auto write_start = Clock::now();
    for (std::uint64_t i = 0; i < kLines; ++i) {
        const std::string line = MakeLine(i, 24);  // 约 40 字节
        bytes += line.size();
        (void)file.Write(line);
    }
    const auto write_end = Clock::now();
    CHECK(file.Flush());
    const auto flush_end = Clock::now();
    file.Close();
    const auto close_end = Clock::now();

    const double write_seconds =
        std::chrono::duration<double>(write_end - write_start).count();
    const double flush_seconds =
        std::chrono::duration<double>(flush_end - write_end).count();
    const double close_seconds =
        std::chrono::duration<double>(close_end - flush_end).count();
    const double total_seconds =
        std::chrono::duration<double>(close_end - write_start).count();
    std::printf(
        "[bench] lines=%llu bytes=%llu rotations=%llu write=%.3fs "
        "flush=%.3fs close=%.3fs total=%.3fs throughput=%.2f MB/s\n",
        static_cast<unsigned long long>(kLines),
        static_cast<unsigned long long>(bytes),
        static_cast<unsigned long long>(file.rotations()), write_seconds,
        flush_seconds, close_seconds, total_seconds,
        static_cast<double>(bytes) / total_seconds / (1024.0 * 1024.0));

    CHECK(file.rotations() >= 10);
    CHECK(file.dropped_bytes() == 0);
    CHECK(file.unflushed_bytes() == 0);
    CHECK(file.compress_errors() == 0);
    CHECK(file.accepted_bytes() == bytes);

    std::string detail;
    const std::string merged = ReadAllLogs(base);
    CHECK(CheckSequential(merged, kLines, &detail));
    if (!detail.empty()) {
        std::fprintf(stderr, "[log-rotate] %s\n", detail.c_str());
    }
}

// ---------------------------------------------------------------------------
// 场景 8：随机混合（随机长度 + 随机上限，重复多轮）
// ---------------------------------------------------------------------------
void ScenarioRandomized() {
    Announce("randomized record sizes and rotation limits");
    for (int round = 0; round < 3; ++round) {
        TempDir temp("random");
        const std::string base = temp.file("random.log");
        RotationOptions options;
        std::uint32_t seed = 0x9e3779b9u ^ static_cast<std::uint32_t>(round);
        const auto next = [&seed]() {
            seed = seed * 1664525u + 1013904223u;
            return seed;
        };
        options.max_bytes = 512 + (next() % 4096);
        options.max_files = 4 + (next() % 6);
        options.write_buffer_bytes = 64 + (next() % 2048);
        options.compress = (next() % 2) == 0;
        constexpr std::uint64_t kLines = 20000;

        RotatingLogFile file(base, options);
        for (std::uint64_t i = 0; i < kLines; ++i) {
            const std::size_t payload = 8 + static_cast<std::size_t>(next() % 96);
            (void)file.Write(MakeLine(i, payload));
        }
        CHECK(file.Flush());
        file.Close();

        CHECK(file.dropped_bytes() == 0);
        CHECK(file.unflushed_bytes() == 0);
        CHECK(file.compress_errors() == 0);
        // 随机上限可能很小、归档保留有限：只验证未被删除的部分是完整
        // 连续子序列，且最后一条一定在。
        const std::string merged = ReadAllLogs(base);
        CHECK(!merged.empty());
        std::uint64_t last = 0;
        std::size_t position = 0;
        while (position < merged.size()) {
            const std::size_t end = merged.find('\n', position);
            CHECK(end != std::string::npos);
            if (end == std::string::npos) {
                break;
            }
            const std::string line = merged.substr(position, end - position);
            std::uint64_t value = 0;
            CHECK(ParseSequence(line, &value));
            last = value;
            position = end + 1;
        }
        CHECK(last == kLines - 1);
        CHECK(ListArchives(base).size() <= options.max_files);
    }
}

}  // namespace

int main() {
    const auto start = Clock::now();
    ScenarioSizeAndGzip();
    ScenarioDailyRotation();
    ScenarioRetention();
    ScenarioSinkWithoutCompression();
    ScenarioConcurrentWrites();
    ScenarioFailureRecovery();
    ScenarioMillionRecords();
    ScenarioRandomized();
    const double seconds =
        std::chrono::duration<double>(Clock::now() - start).count();
    if (g_failures == 0) {
        std::printf("[log-rotate] PASS (%.2fs)\n", seconds);
        return 0;
    }
    std::printf("[log-rotate] FAIL (%d checks, %.2fs)\n", g_failures, seconds);
    return 1;
}
