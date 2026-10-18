/**
 * @file test_bufio_stress.cpp
 * @brief `bufio::Reader` 压测：**系统调用次数**与吞吐，对比「裸 1 字节读」。
 *
 * ## 测什么
 *
 * Go `bufio` 的核心价值是「把 N 次小 read 合并成 1 次大 read」。这里用
 * **真实文件 + 真实 `read(2)`** 量化它：
 *
 * | 组 | 每次请求 | 缓冲 | 预期底层 read 次数（1 MiB） |
 * |---|---|---|---|
 * | `bare-read(1B)` | 1 字节 | 无 | 1,048,576 |
 * | `bufio(4096) ReadByte` | 1 字节 | 4096 | 256 |
 * | `bufio(4096) Read(128B)` | 128 字节 | 4096 | 256 |
 *
 * `FileSource::calls` 就是 `::read` 的调用次数，**每次都对应一次系统调用**
 * （程序不经过任何 hook/中间缓冲）。因此它既是「逻辑调用次数」也是
 * 「系统调用次数」。另外可用 strace 独立复核：
 *
 * ```
 * ./go2cpp_bufio_stress --size 65536 --mode noop   # 加载器自身的 read 基线
 * strace -c -e trace=read ./go2cpp_bufio_stress --size 65536 --mode bare
 * strace -c -e trace=read ./go2cpp_bufio_stress --size 65536 --mode bufio
 * ```
 *
 * ## 指标
 *
 * - 系统调用次数 + 与裸读的**减少比例/倍数**；
 * - 吞吐（MiB/s，取 3 轮中位数）与每字节延迟（ns）；
 * - 不同缓冲容量（512 / 4096 / 65536）的调用次数与耗时对比。
 *
 * ## 正确性
 *
 * 每条路径都逐字节比对期望模式 `(i * 31 + 7) & 0xFF`；任何不一致都算失败
 * （不会为了跑分牺牲校验）。临时文件用完删除。
 *
 * ## 用法
 * ```
 * ./go2cpp_bufio_stress                     # 默认 1 MiB、3 轮、全量对比
 * ./go2cpp_bufio_stress --size 65536 --reps 1
 * ./go2cpp_bufio_stress --mode bare|bufio|byte|noop|sweep
 * ```
 */
#include "go2cpp/bufio/bufio.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace {

namespace streams = go2cpp::streams;
using go2cpp::NewError;
using go2cpp::bufio::AsBytes;
using go2cpp::bufio::Reader;

constexpr std::size_t kDefaultSize = 1024U * 1024U;
constexpr std::size_t kDefaultBufferSize = 4096;
constexpr std::size_t kDefaultRequestSize = 128;

std::uint8_t ExpectedByte(std::size_t position) {
    return static_cast<std::uint8_t>((position * 31U + 7U) & 0xFFU);
}

/// 真实文件 Reader：每次 `Read` 恰好调用一次 `read(2)`。
class FileSource {
public:
    explicit FileSource(const std::string& path)
        : m_descriptor(::open(path.c_str(), O_RDONLY)) {}
    ~FileSource() {
        if (m_descriptor >= 0) {
            ::close(m_descriptor);
        }
    }
    FileSource(const FileSource&) = delete;
    FileSource& operator=(const FileSource&) = delete;

    bool valid() const { return m_descriptor >= 0; }

    streams::ReadResult Read(char* buffer, std::size_t size) {
        ++calls;
        const ssize_t received = ::read(m_descriptor, buffer, size);
        if (received < 0) {
            return streams::ReadResult{0, NewError("file: read failed")};
        }
        if (received == 0) {
            return streams::ReadResult{0, streams::IoEof()};
        }
        return streams::ReadResult{static_cast<std::size_t>(received), {}};
    }

    std::uint64_t calls{0};

private:
    int m_descriptor{-1};
};

struct Measurement {
    std::uint64_t calls{0};       ///< 底层 read(2) 次数
    std::size_t bytes{0};         ///< 实际消费的字节数
    std::size_t mismatches{0};    ///< 内容不一致的字节数
    std::vector<double> seconds;  ///< 每轮耗时
};

double Median(std::vector<double> values) {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const std::size_t middle = values.size() / 2;
    if (values.size() % 2 == 1) {
        return values[middle];
    }
    return (values[middle - 1] + values[middle]) / 2.0;
}

bool WriteFixture(const std::string& path, std::size_t total) {
    const int descriptor = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (descriptor < 0) {
        return false;
    }
    std::string buffer(kDefaultBufferSize, '\0');
    std::size_t position = 0;
    bool ok = true;
    while (position < total) {
        const std::size_t take = std::min(buffer.size(), total - position);
        for (std::size_t offset = 0; offset < take; ++offset) {
            buffer[offset] = static_cast<char>(ExpectedByte(position + offset));
        }
        std::size_t written = 0;
        while (written < take) {
            const ssize_t result =
                ::write(descriptor, buffer.data() + written, take - written);
            if (result <= 0) {
                ok = false;
                break;
            }
            written += static_cast<std::size_t>(result);
        }
        if (!ok) {
            break;
        }
        position += take;
    }
    ::close(descriptor);
    return ok;
}

/// 裸读：每次 `read(2)` 只取 1 字节。
Measurement MeasureBare(const std::string& path, std::size_t total,
                        int repetitions) {
    Measurement measurement;
    for (int repetition = 0; repetition < repetitions; ++repetition) {
        const int descriptor = ::open(path.c_str(), O_RDONLY);
        if (descriptor < 0) {
            return measurement;
        }
        std::uint64_t calls = 0;
        std::size_t position = 0;
        std::size_t mismatches = 0;
        char byte = 0;
        const auto start = std::chrono::steady_clock::now();
        while (position < total) {
            const ssize_t received = ::read(descriptor, &byte, 1);
            ++calls;
            if (received <= 0) {
                break;
            }
            if (static_cast<std::uint8_t>(byte) != ExpectedByte(position)) {
                ++mismatches;
            }
            ++position;
        }
        const auto finish = std::chrono::steady_clock::now();
        ::close(descriptor);
        measurement.calls = calls;  // 每轮相同
        measurement.bytes = position;
        measurement.mismatches += mismatches;
        measurement.seconds.push_back(
            std::chrono::duration<double>(finish - start).count());
    }
    return measurement;
}

/// bufio + ReadByte：调用方逐字节，底层按缓冲容量批量读。
Measurement MeasureBufferedByte(const std::string& path, std::size_t total,
                                std::size_t buffer_size, int repetitions) {
    Measurement measurement;
    for (int repetition = 0; repetition < repetitions; ++repetition) {
        FileSource source(path);
        if (!source.valid()) {
            return measurement;
        }
        Reader<FileSource> reader(source, buffer_size);
        std::size_t position = 0;
        std::size_t mismatches = 0;
        const auto start = std::chrono::steady_clock::now();
        while (position < total) {
            const auto byte = reader.ReadByte();
            if (!byte.ok()) {
                break;
            }
            if (byte.Value() != ExpectedByte(position)) {
                ++mismatches;
            }
            ++position;
        }
        const auto finish = std::chrono::steady_clock::now();
        measurement.calls = source.calls;
        measurement.bytes = position;
        measurement.mismatches += mismatches;
        measurement.seconds.push_back(
            std::chrono::duration<double>(finish - start).count());
    }
    return measurement;
}

/// bufio + Read(固定请求尺寸)：模拟逐行/逐块解析。
Measurement MeasureBufferedRead(const std::string& path, std::size_t total,
                                std::size_t buffer_size,
                                std::size_t request_size, int repetitions) {
    Measurement measurement;
    std::string request(request_size, '\0');
    for (int repetition = 0; repetition < repetitions; ++repetition) {
        FileSource source(path);
        if (!source.valid()) {
            return measurement;
        }
        Reader<FileSource> reader(source, buffer_size);
        std::size_t position = 0;
        std::size_t mismatches = 0;
        const auto start = std::chrono::steady_clock::now();
        while (position < total) {
            const std::size_t got = reader.Read(AsBytes(request));
            if (got == 0) {
                break;
            }
            for (std::size_t offset = 0; offset < got; ++offset) {
                if (static_cast<std::uint8_t>(request[offset]) !=
                    ExpectedByte(position + offset)) {
                    ++mismatches;
                }
            }
            position += got;
        }
        const auto finish = std::chrono::steady_clock::now();
        measurement.calls = source.calls;
        measurement.bytes = position;
        measurement.mismatches += mismatches;
        measurement.seconds.push_back(
            std::chrono::duration<double>(finish - start).count());
    }
    return measurement;
}

double ThroughputMiB(std::size_t bytes, double seconds) {
    if (seconds <= 0.0) {
        return 0.0;
    }
    return (static_cast<double>(bytes) / (1024.0 * 1024.0)) / seconds;
}

void PrintMeasurement(const char* label, const Measurement& measurement,
                      std::size_t buffer_size, std::size_t request_size) {
    const double seconds = Median(measurement.seconds);
    std::printf(
        "[stress] %-26s 缓冲=%-6zu 请求=%-4zu 调用=%-9llu 中位耗时=%8.3f ms "
        "吞吐=%8.1f MiB/s 单次=%.1f ns\n",
        label, buffer_size, request_size,
        static_cast<unsigned long long>(measurement.calls), seconds * 1000.0,
        ThroughputMiB(measurement.bytes, seconds),
        measurement.bytes > 0
            ? seconds * 1e9 / static_cast<double>(measurement.bytes)
            : 0.0);
}

struct Options {
    std::size_t size{kDefaultSize};
    int repetitions{3};
    std::string mode{"all"};
};

Options ParseOptions(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--size" && index + 1 < argc) {
            options.size = static_cast<std::size_t>(std::strtoull(argv[++index], nullptr, 10));
        } else if (argument == "--mode" && index + 1 < argc) {
            options.mode = argv[++index];
        } else if (argument == "--reps" && index + 1 < argc) {
            options.repetitions = static_cast<int>(std::strtoll(argv[++index], nullptr, 10));
            if (options.repetitions < 1) {
                options.repetitions = 1;
            }
        } else {
            std::fprintf(stderr, "[stress] 未知参数: %s\n", argv[index]);
        }
    }
    return options;
}

}  // namespace

int main(int argc, char** argv) {
    const Options options = ParseOptions(argc, argv);
    const std::string path = "/tmp/go2cpp_bufio_stress.bin";

    std::printf("[stress] ===== bufio 压测开始 =====\n");
    std::printf("[stress] 数据规模=%zu 字节, 轮数=%d, 模式=%s\n", options.size,
                options.repetitions, options.mode.c_str());

    if (options.mode == "noop") {
        // 只 open/close：给 strace 提供「解释器 + 动态加载器自身 read」的基线。
        const int descriptor = ::open(path.c_str(), O_RDONLY);
        if (descriptor >= 0) {
            ::close(descriptor);
        }
        std::printf("[stress] noop: 仅 open/close（strace 基线）\n");
        std::printf("[stress] all checks passed\n");
        return 0;
    }

    if (options.size > 0 && !WriteFixture(path, options.size)) {
        std::fprintf(stderr, "[stress] 无法生成测试文件 %s\n", path.c_str());
        return 1;
    }

    int failures = 0;
    const auto verify = [&failures, &options](const Measurement& measurement,
                                              const char* label) {
        if (measurement.bytes != options.size) {
            std::fprintf(stderr, "[stress] %s 字节数不符: %zu != %zu\n", label,
                         measurement.bytes, options.size);
            ++failures;
        }
        if (measurement.mismatches != 0) {
            std::fprintf(stderr, "[stress] %s 内容不一致: %zu 字节\n", label,
                         measurement.mismatches);
            ++failures;
        }
    };

    Measurement bare;
    Measurement buffered_byte;
    Measurement buffered_read;

    if (options.mode == "all" || options.mode == "bare") {
        bare = MeasureBare(path, options.size, options.repetitions);
        verify(bare, "bare");
        PrintMeasurement("bare-read(1B)", bare, 0, 1);
    }
    if (options.mode == "all" || options.mode == "byte") {
        buffered_byte = MeasureBufferedByte(path, options.size, kDefaultBufferSize,
                                            options.repetitions);
        verify(buffered_byte, "bufio-byte");
        PrintMeasurement("bufio ReadByte", buffered_byte, kDefaultBufferSize, 1);
    }
    if (options.mode == "all" || options.mode == "bufio") {
        buffered_read = MeasureBufferedRead(path, options.size, kDefaultBufferSize,
                                            kDefaultRequestSize,
                                            options.repetitions);
        verify(buffered_read, "bufio-read");
        PrintMeasurement("bufio Read(128B)", buffered_read, kDefaultBufferSize,
                         kDefaultRequestSize);
    }

    if (options.mode == "all") {
        // ---- 核心结论：系统调用次数对比 ----
        if (bare.calls > 0 && buffered_byte.calls > 0) {
            const double ratio =
                static_cast<double>(bare.calls) / static_cast<double>(buffered_byte.calls);
            const double reduction =
                (1.0 - static_cast<double>(buffered_byte.calls) /
                           static_cast<double>(bare.calls)) *
                100.0;
            std::printf(
                "[stress] 系统调用：bare=%llu, bufio(4096)=%llu, 减少=%.4f%%, "
                "倍数=%.1fx\n",
                static_cast<unsigned long long>(bare.calls),
                static_cast<unsigned long long>(buffered_byte.calls), reduction,
                ratio);
            std::printf(
                "[stress] 吞吐：bare=%.1f MiB/s, bufio=%.1f MiB/s, 加速=%.1fx\n",
                ThroughputMiB(bare.bytes, Median(bare.seconds)),
                ThroughputMiB(buffered_byte.bytes, Median(buffered_byte.seconds)),
                Median(bare.seconds) / Median(buffered_byte.seconds));
        }

        // ---- 缓冲容量扫描：调用次数由「请求粒度」与「缓冲容量」共同决定 ----
        const std::size_t sizes[] = {512, 4096, 65536};
        for (const std::size_t buffer_size : sizes) {
            const Measurement scan = MeasureBufferedRead(
                path, options.size, buffer_size, kDefaultRequestSize,
                options.repetitions);
            verify(scan, "sweep");
            PrintMeasurement("sweep Read(128B)", scan, buffer_size, kDefaultRequestSize);
        }
    }

    ::unlink(path.c_str());

    if (failures != 0) {
        std::fprintf(stderr, "[stress] FAILED: %d 项校验失败\n", failures);
        return 1;
    }
    std::printf("[stress] all checks passed\n");
    return 0;
}
