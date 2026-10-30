/**
 * @file test_bufio_batch.cpp
 * @brief `bufio::Writer` 批量 flush（策略 / 段 / 部分写 / 并发 / 真实 socket）
 *        的正确性测试。
 *
 * ## 覆盖策略（对照 PROJECT_RULES 第 3 节）
 *
 * - **策略**：`kOnFull` / `kInterval` / `kOnExplicit` / `kHybrid` 四种全部
 *   覆盖，含 `max_batch` 硬上限、`threshold` 水位、`interval` 定时三条
 *   触发路径，以及 `SetBatchConfig` 的钳制规则。
 * - **边界**：空缓冲 Flush、刚好达到 `max_batch`、单条超过缓冲容量
 *   （直写快路径）、`threshold=1`、`max_batch=1`、部分写（每次 3/7 字节）、
 *   底层零进展、谎报写入量、注入错误。
 * - **随机**：500 轮随机容量/策略/批大小/水位/间隔/消息长度/写方式/部分写
 *   上限，逐字节比对（固定种子，可复现）。
 * - **并发**：① 4 线程各用独立 `Writer`；② 4 线程共享一个 `Writer` +
 *   外部锁（文档要求的用法），解析每条消息的线程号/序号。
 * - **真实场景 6 个**：日志行批量写、RPC 响应（头+体两次写）、传感器
 *   时间窗口、无批量接口的管道式 sink、TCP loopback 真实 `writev`、
 *   多线程共享 TCP 连接。
 *
 * ## 运行
 * ```
 * ctest --test-dir .build-bento-wsl -R go2cpp_bufio_batch --output-on-failure
 * # 或直接：
 * ./.build-bento-wsl/go2cpp_bufio_batch
 * ```
 */
#include "go2cpp/bufio/bufio.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <random>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/uio.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

namespace streams = go2cpp::streams;
using go2cpp::Is;
using go2cpp::NewError;
using go2cpp::bufio::AsBytes;
using go2cpp::bufio::BatchConfig;
using go2cpp::bufio::BatchEntry;
using go2cpp::bufio::FlushPolicy;
using go2cpp::bufio::Writer;

std::atomic<int> g_checks{0};
std::atomic<int> g_failures{0};

void CheckImpl(bool condition, const char* expression, const char* file, int line) {
    g_checks.fetch_add(1, std::memory_order_relaxed);
    if (!condition) {
        g_failures.fetch_add(1, std::memory_order_relaxed);
        std::fprintf(stderr, "[bufio-batch] FAILED %s:%d: %s\n", file, line, expression);
    }
}

#define CHECK(condition) CheckImpl((condition), #condition, __FILE__, __LINE__)

void Announce(const char* name) {
    std::printf("[bufio-batch] %s\n", name);
    std::fflush(stdout);
}

BatchConfig Config(FlushPolicy policy, std::size_t max_batch, std::size_t threshold,
                   std::chrono::microseconds interval = std::chrono::microseconds{0}) {
    BatchConfig config;
    config.policy = policy;
    config.max_batch = max_batch;
    config.threshold = threshold;
    config.interval = interval;
    return config;
}

void SleepMs(int milliseconds) {
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

/// 固定种子，任何失败都能复现。
std::mt19937& Rng() {
    static std::mt19937 engine(20261030U);
    return engine;
}

std::size_t RandomRange(std::size_t low, std::size_t high) {
    std::uniform_int_distribution<std::size_t> distribution(low, high);
    return distribution(Rng());
}

std::string RandomPayload(std::size_t size) {
    std::string payload(size, '\0');
    for (char& character : payload) {
        character = static_cast<char>(RandomRange('a', 'z'));
    }
    return payload;
}

// ===========================================================================
// 测试桩
// ===========================================================================

/// 内存 sink，**没有** `WriteBatch`：验证退化路径（一次 Write）。
struct MemorySink {
    std::string data;
    std::uint64_t calls{0};
    std::size_t max_write{0};  ///< 0 表示一次写完

    streams::WriteResult Write(const char* bytes, std::size_t size) {
        ++calls;
        if (max_write > 0) {
            size = std::min(size, max_write);
        }
        data.append(bytes, size);
        return streams::WriteResult{size, {}};
    }
};

/// 批量 sink：记录每次 `WriteBatch` 的段数，按 iovec 顺序拼接；可模拟部分写。
struct BatchSink {
    std::string data;
    std::uint64_t batch_calls{0};
    std::uint64_t linear_calls{0};
    std::vector<std::size_t> batch_sizes;
    std::size_t max_write{0};  ///< 单次 WriteBatch 最多写出字节数

    streams::WriteResult Write(const char* bytes, std::size_t size) {
        ++linear_calls;
        data.append(bytes, size);
        return streams::WriteResult{size, {}};
    }

    streams::WriteResult WriteBatch(const BatchEntry* entries, std::size_t count) {
        ++batch_calls;
        batch_sizes.push_back(count);
        std::size_t budget = max_write > 0 ? max_write : static_cast<std::size_t>(-1);
        std::size_t written = 0;
        for (std::size_t index = 0; index < count && budget > 0; ++index) {
            const std::size_t take = std::min(entries[index].size, budget);
            data.append(entries[index].data, take);
            written += take;
            budget -= take;
        }
        return streams::WriteResult{written, {}};
    }
};

/// 写到 `fail_after` 字节后报错的批量 sink。
struct FailingBatchSink {
    std::string data;
    std::uint64_t batch_calls{0};
    std::size_t fail_after{0};

    streams::WriteResult Write(const char* bytes, std::size_t size) {
        data.append(bytes, size);
        return streams::WriteResult{size, {}};
    }

    streams::WriteResult WriteBatch(const BatchEntry* entries, std::size_t count) {
        ++batch_calls;
        std::size_t written = 0;
        for (std::size_t index = 0; index < count; ++index) {
            if (data.size() >= fail_after) {
                if (written == 0) {
                    return streams::WriteResult{0, NewError("sink: injected failure")};
                }
                break;
            }
            const std::size_t take =
                std::min(entries[index].size, fail_after - data.size());
            data.append(entries[index].data, take);
            written += take;
            if (take < entries[index].size) {
                break;
            }
        }
        return streams::WriteResult{written, {}};
    }
};

/// 永远 (0, nil) 的违约批量 sink：验证 FlushBatch 不会死循环。
struct ZeroBatchSink {
    std::uint64_t calls{0};

    streams::WriteResult Write(const char*, std::size_t) {
        ++calls;
        return streams::WriteResult{0, {}};
    }

    streams::WriteResult WriteBatch(const BatchEntry*, std::size_t) {
        ++calls;
        return streams::WriteResult{0, {}};
    }
};

/// 谎报「写得比给的多」的违约批量 sink：验证不会下溢/越界。
struct GreedyBatchSink {
    std::uint64_t calls{0};

    streams::WriteResult Write(const char*, std::size_t size) {
        ++calls;
        return streams::WriteResult{size + 1, {}};
    }

    streams::WriteResult WriteBatch(const BatchEntry* entries, std::size_t count) {
        ++calls;
        std::size_t total = 0;
        for (std::size_t index = 0; index < count; ++index) {
            total += entries[index].size;
        }
        return streams::WriteResult{total + 1, {}};
    }
};

/// 线程安全的批量 sink（内部加锁，用于并发共享场景）。
class LockedBatchSink {
public:
    streams::WriteResult Write(const char* bytes, std::size_t size) {
        std::lock_guard<std::mutex> guard(m_mutex);
        m_data.append(bytes, size);
        return streams::WriteResult{size, {}};
    }

    streams::WriteResult WriteBatch(const BatchEntry* entries, std::size_t count) {
        std::lock_guard<std::mutex> guard(m_mutex);
        std::size_t total = 0;
        for (std::size_t index = 0; index < count; ++index) {
            m_data.append(entries[index].data, entries[index].size);
            total += entries[index].size;
        }
        return streams::WriteResult{total, {}};
    }

    std::string Snapshot() const {
        std::lock_guard<std::mutex> guard(m_mutex);
        return m_data;
    }

private:
    mutable std::mutex m_mutex;
    std::string m_data;
};

/// 真实文件描述符 sink：`Write` = send(2)，`WriteBatch` = writev(2)。
class FdSink {
public:
    explicit FdSink(int descriptor) : m_descriptor(descriptor) {}

    streams::WriteResult Write(const char* data, std::size_t size) {
        ++m_syscalls;
        const ssize_t written = ::send(m_descriptor, data, size, MSG_NOSIGNAL);
        if (written < 0) {
            return streams::WriteResult{0, NewError("fd sink: send failed")};
        }
        return streams::WriteResult{static_cast<std::size_t>(written), {}};
    }

    streams::WriteResult WriteBatch(const BatchEntry* entries, std::size_t count) {
        ++m_syscalls;
        m_iovecs.clear();
        m_iovecs.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            iovec vector{};
            vector.iov_base = const_cast<char*>(entries[index].data);
            vector.iov_len = entries[index].size;
            m_iovecs.push_back(vector);
        }
        const ssize_t written = ::writev(m_descriptor, m_iovecs.data(),
                                         static_cast<int>(m_iovecs.size()));
        if (written < 0) {
            return streams::WriteResult{0, NewError("fd sink: writev failed")};
        }
        return streams::WriteResult{static_cast<std::size_t>(written), {}};
    }

    std::uint64_t Syscalls() const noexcept { return m_syscalls; }

private:
    int m_descriptor;
    std::uint64_t m_syscalls{0};
    std::vector<iovec> m_iovecs;
};

/// 建立 TCP loopback 连接；失败返回 false。
bool MakeTcpPair(int& client, int& server) {
    const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) {
        return false;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    socklen_t length = sizeof(address);
    const bool bound =
        ::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
    const bool listening = bound && ::listen(listener, 1) == 0;
    const bool named = listening &&
                       ::getsockname(listener, reinterpret_cast<sockaddr*>(&address),
                                     &length) == 0;
    client = named ? ::socket(AF_INET, SOCK_STREAM, 0) : -1;
    const bool connected =
        client >= 0 &&
        ::connect(client, reinterpret_cast<sockaddr*>(&address), length) == 0;
    server = connected ? ::accept(listener, nullptr, nullptr) : -1;
    if (listener >= 0) {
        ::close(listener);
    }
    if (client < 0 || server < 0) {
        if (client >= 0) {
            ::close(client);
        }
        return false;
    }
    const int one = 1;
    ::setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    ::setsockopt(server, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return true;
}

/// 从 fd 读满 `total` 字节，按 `message_size` 切分后把每条消息交给 `verify`。
template <typename Verify>
bool ReadAndVerify(int descriptor, std::size_t total, std::size_t message_size,
                   Verify verify) {
    std::string buffer(256 * 1024, '\0');
    std::string carry;
    std::size_t received = 0;
    std::size_t messages = 0;
    while (received < total) {
        const ssize_t count = ::recv(descriptor, buffer.data(), buffer.size(), 0);
        if (count < 0) {
            return false;
        }
        if (count == 0) {
            break;
        }
        received += static_cast<std::size_t>(count);
        carry.append(buffer.data(), static_cast<std::size_t>(count));
        std::size_t consumed = 0;
        while (carry.size() - consumed >= message_size) {
            if (!verify(carry.data() + consumed, messages)) {
                return false;
            }
            consumed += message_size;
            ++messages;
        }
        carry.erase(0, consumed);
    }
    return received == total && carry.empty() &&
           messages == total / message_size;
}

std::uint64_t LoadSequence(const char* data, std::size_t offset) {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < 8; ++index) {
        value |= static_cast<std::uint64_t>(
                     static_cast<unsigned char>(data[offset + index]))
                 << (8 * index);
    }
    return value;
}

void StoreSequence(char* data, std::size_t offset, std::uint64_t value) {
    for (std::size_t index = 0; index < 8; ++index) {
        data[offset + index] =
            static_cast<char>((value >> (8 * index)) & 0xFFU);
    }
}

// ===========================================================================
// 配置与基本语义
// ===========================================================================

void TestConfigDefaultsAndClamp() {
    Announce("配置：默认值 / 钳制规则");
    {
        MemorySink sink;
        Writer<MemorySink> writer(sink);
        const BatchConfig& config = writer.GetBatchConfig();
        CHECK(config.policy == FlushPolicy::kHybrid);
        CHECK(config.max_batch == 64);
        CHECK(config.threshold == 4096);
        CHECK(config.interval == std::chrono::microseconds{100});
    }
    {
        // 水位不能超过容量：容量 16 的 Writer 默认 threshold 被压到 16。
        MemorySink sink;
        Writer<MemorySink> writer(sink, 16);
        CHECK(writer.GetBatchConfig().threshold == 16);
    }
    {
        MemorySink sink;
        Writer<MemorySink> writer(sink, 64);
        writer.SetBatchConfig(Config(FlushPolicy::kOnFull, 0, 0,
                                     std::chrono::microseconds{-5}));
        const BatchConfig& config = writer.GetBatchConfig();
        CHECK(config.max_batch == 1);
        CHECK(config.threshold == 1);
        CHECK(config.interval == std::chrono::microseconds{0});
        writer.SetBatchConfig(Config(FlushPolicy::kOnFull, 999999, 999999));
        CHECK(writer.GetBatchConfig().max_batch ==
              go2cpp::bufio::kMaxBatchEntries);
        CHECK(writer.GetBatchConfig().threshold == 64);
    }
}

void TestEmptyFlush() {
    Announce("边界：空缓冲 Flush / FlushBatch 不触碰底层");
    MemorySink sink;
    Writer<MemorySink> writer(sink, 64);
    CHECK(writer.FlushBatch().ok());
    CHECK(writer.Flush().ok());
    CHECK(sink.calls == 0);
    CHECK(writer.Buffered() == 0);
    CHECK(writer.PendingBatches() == 0);
}

void TestBatchOrderAndContent() {
    Announce("批量：一次 FlushBatch 提交全部段，顺序/内容不变");
    BatchSink sink;
    Writer<BatchSink> writer(sink, 256);
    writer.SetBatchConfig(Config(FlushPolicy::kOnExplicit, 64, 256));
    std::string expected;
    for (std::size_t index = 1; index <= 10; ++index) {
        const std::string message(static_cast<std::size_t>(index),
                                  static_cast<char>('A' + index));
        expected += message;
        CHECK(writer.WriteString(message).ok());
    }
    CHECK(sink.batch_calls == 0);
    CHECK(sink.linear_calls == 0);
    CHECK(writer.PendingBatches() == 10);
    CHECK(writer.Buffered() == expected.size());
    CHECK(writer.FlushBatch().ok());
    CHECK(sink.batch_calls == 1);
    CHECK(sink.batch_sizes.size() == 1 && sink.batch_sizes[0] == 10);
    CHECK(sink.data == expected);
    CHECK(writer.Buffered() == 0);
    CHECK(writer.PendingBatches() == 0);
}

void TestMaxBatchAutoFlush() {
    Announce("策略：段数达到 max_batch 立即批量写（硬上限）");
    BatchSink sink;
    Writer<BatchSink> writer(sink, 1024);
    writer.SetBatchConfig(Config(FlushPolicy::kOnExplicit, 8, 1024));
    std::string expected;
    for (int index = 0; index < 8; ++index) {
        const std::string message = "item-" + std::to_string(index) + ";";
        expected += message;
        CHECK(writer.WriteString(message).ok());
    }
    CHECK(sink.batch_calls == 1);
    CHECK(sink.batch_sizes.size() == 1 && sink.batch_sizes[0] == 8);
    CHECK(writer.PendingBatches() == 0);
    CHECK(writer.Buffered() == 0);
    for (int index = 8; index < 13; ++index) {
        const std::string message = "item-" + std::to_string(index) + ";";
        expected += message;
        CHECK(writer.WriteString(message).ok());
    }
    CHECK(sink.batch_calls == 1);
    CHECK(writer.PendingBatches() == 5);
    CHECK(writer.FlushBatch().ok());
    CHECK(sink.batch_calls == 2);
    CHECK(sink.batch_sizes[1] == 5);
    CHECK(sink.data == expected);
}

void TestThresholdOnFull() {
    Announce("策略 kOnFull：水位在下一次写之前触发");
    BatchSink sink;
    Writer<BatchSink> writer(sink, 256);
    writer.SetBatchConfig(Config(FlushPolicy::kOnFull, 64, 64));
    for (int index = 0; index < 4; ++index) {
        CHECK(writer.WriteString(std::string(16, 'x')).ok());
    }
    // 第 4 条写完后恰好到水位，但「满即发」发生在下一次写入之前（Go 语义）。
    CHECK(sink.batch_calls == 0);
    CHECK(writer.Buffered() == 64);
    CHECK(writer.WriteString(std::string(16, 'y')).ok());
    CHECK(sink.batch_calls == 1);
    CHECK(sink.batch_sizes[0] == 4);
    CHECK(sink.data == std::string(64, 'x'));
    CHECK(writer.Buffered() == 16);
    CHECK(writer.PendingBatches() == 1);
}

void TestExplicitPolicy() {
    Announce("策略 kOnExplicit：不自动写，只有显式 FlushBatch");
    BatchSink sink;
    Writer<BatchSink> writer(sink, 4096);
    writer.SetBatchConfig(Config(FlushPolicy::kOnExplicit, 256, 4096));
    std::string expected;
    for (int index = 0; index < 100; ++index) {
        const std::string message = "payload-" + std::to_string(index) + ";";
        expected += message;
        CHECK(writer.WriteString(message).ok());
    }
    CHECK(sink.batch_calls == 0);
    CHECK(sink.linear_calls == 0);
    CHECK(writer.PendingBatches() == 100);
    CHECK(writer.FlushBatch().ok());
    CHECK(sink.batch_calls == 1);
    CHECK(sink.batch_sizes[0] == 100);
    CHECK(sink.data == expected);
}

void TestIntervalPolicy() {
    Announce("策略 kInterval：到时间后的下一次写触发批量");
    BatchSink sink;
    Writer<BatchSink> writer(sink, 4096);
    writer.SetBatchConfig(
        Config(FlushPolicy::kInterval, 256, 4096, std::chrono::microseconds{100000}));
    for (int index = 0; index < 3; ++index) {
        CHECK(writer.WriteString(std::string(8, 'a')).ok());
    }
    CHECK(sink.batch_calls == 0);
    CHECK(writer.PendingBatches() == 3);
    SleepMs(150);
    CHECK(writer.WriteString(std::string(8, 'b')).ok());
    CHECK(sink.batch_calls == 1);
    CHECK(sink.batch_sizes[0] == 4);
    CHECK(sink.data == std::string(24, 'a') + std::string(8, 'b'));
    CHECK(writer.PendingBatches() == 0);
}

void TestHybridPolicy() {
    Announce("策略 kHybrid：满 或 超时 都触发");
    {
        BatchSink sink;
        Writer<BatchSink> writer(sink, 256);
        writer.SetBatchConfig(
            Config(FlushPolicy::kHybrid, 64, 32, std::chrono::seconds{10}));
        for (int index = 0; index < 4; ++index) {
            CHECK(writer.WriteString(std::string(8, 'c')).ok());
        }
        CHECK(sink.batch_calls == 0);
        CHECK(writer.WriteString(std::string(8, 'd')).ok());  // 写前水位 32
        CHECK(sink.batch_calls == 1);
        CHECK(sink.batch_sizes[0] == 4);
        CHECK(writer.Buffered() == 8);
    }
    {
        BatchSink sink;
        Writer<BatchSink> writer(sink, 4096);
        writer.SetBatchConfig(
            Config(FlushPolicy::kHybrid, 64, 4096, std::chrono::microseconds{50000}));
        CHECK(writer.WriteString(std::string(8, 'e')).ok());
        CHECK(writer.WriteString(std::string(8, 'f')).ok());
        CHECK(sink.batch_calls == 0);
        SleepMs(100);
        CHECK(writer.WriteString(std::string(8, 'g')).ok());
        CHECK(sink.batch_calls == 1);
        CHECK(sink.batch_sizes[0] == 3);
    }
}

void TestWriteByteCoalescing() {
    Announce("段：连续 WriteByte 合并为一段，不会线性写出");
    BatchSink sink;
    Writer<BatchSink> writer(sink, 256);
    writer.SetBatchConfig(Config(FlushPolicy::kOnFull, 64, 64));
    for (int index = 0; index < 60; ++index) {
        CHECK(writer.WriteByte(static_cast<std::uint8_t>('a' + index % 26)).ok());
    }
    CHECK(sink.batch_calls == 0);
    CHECK(writer.PendingBatches() == 1);
    CHECK(writer.Buffered() == 60);
    CHECK(writer.FlushBatch().ok());
    CHECK(sink.batch_calls == 1);
    CHECK(sink.batch_sizes[0] == 1);
    CHECK(sink.data.size() == 60);
}

void TestLargeWriteBypass() {
    Announce("边界：单条大于缓冲容量 → 直写底层，不产生段");
    BatchSink sink;
    Writer<BatchSink> writer(sink, 16);
    std::string large = RandomPayload(100);
    CHECK(writer.Write(AsBytes(large)) == 100);
    CHECK(sink.linear_calls == 1);
    CHECK(sink.batch_calls == 0);
    CHECK(sink.data == large);
    CHECK(writer.Buffered() == 0);
    CHECK(writer.PendingBatches() == 0);
}

void TestPartialBatchWrites() {
    Announce("部分写：批量路径跨段截断后继续，数据不丢不乱序");
    BatchSink sink;
    sink.max_write = 7;
    Writer<BatchSink> writer(sink, 128);
    writer.SetBatchConfig(Config(FlushPolicy::kOnExplicit, 64, 128));
    std::string expected;
    for (int index = 0; index < 5; ++index) {
        const std::string message(10, static_cast<char>('A' + index));
        expected += message;
        CHECK(writer.WriteString(message).ok());
    }
    CHECK(writer.FlushBatch().ok());
    CHECK(sink.data == expected);
    CHECK(sink.batch_calls > 1);
    CHECK(writer.Buffered() == 0);
    CHECK(writer.PendingBatches() == 0);
}

void TestPartialLinearWrites() {
    Announce("部分写：无批量接口时 FlushBatch 退化为一次 Write 循环");
    MemorySink sink;
    sink.max_write = 3;
    Writer<MemorySink> writer(sink, 16);
    CHECK(writer.WriteString("abcdefghij").ok());
    CHECK(writer.FlushBatch().ok());
    CHECK(sink.data == "abcdefghij");
    CHECK(sink.calls == 4);
}

void TestErrorStickyBatch() {
    Announce("错误：批量写失败后粘性，已写部分不回退");
    FailingBatchSink sink;
    sink.fail_after = 10;
    Writer<FailingBatchSink> writer(sink, 128);
    writer.SetBatchConfig(Config(FlushPolicy::kOnExplicit, 64, 128));
    std::string expected;
    for (int index = 0; index < 3; ++index) {
        const std::string message(10, static_cast<char>('A' + index));
        expected += message;
        CHECK(writer.WriteString(message).ok());
    }
    const auto flushed = writer.FlushBatch();
    CHECK(!flushed.ok());
    CHECK(sink.data.size() == 10);
    CHECK(sink.data == expected.substr(0, 10));
    CHECK(writer.Err() != nullptr);
    const std::uint64_t calls_before = sink.batch_calls;
    CHECK(!writer.FlushBatch().ok());
    CHECK(sink.batch_calls == calls_before);
    const auto more = writer.WriteString("more");
    CHECK(!more.ok());
    CHECK(writer.Err() != nullptr);
}

void TestZeroProgressBatch() {
    Announce("边界：底层批量写永远 0 字节 → 100 次后短写错误");
    ZeroBatchSink sink;
    Writer<ZeroBatchSink> writer(sink, 64);
    writer.SetBatchConfig(Config(FlushPolicy::kOnExplicit, 64, 64));
    CHECK(writer.WriteString("abc").ok());
    const auto flushed = writer.FlushBatch();
    CHECK(!flushed.ok());
    CHECK(Is(flushed.Err(), streams::IoErrShortWrite()));
    CHECK(sink.calls == 100);
}

void TestGreedyBatch() {
    Announce("边界：底层谎报写入量 → 短写错误，缓冲不被破坏");
    GreedyBatchSink sink;
    Writer<GreedyBatchSink> writer(sink, 64);
    writer.SetBatchConfig(Config(FlushPolicy::kOnExplicit, 64, 64));
    CHECK(writer.WriteString("abcd").ok());
    const auto flushed = writer.FlushBatch();
    CHECK(!flushed.ok());
    CHECK(Is(flushed.Err(), streams::IoErrShortWrite()));
    CHECK(writer.Buffered() == 4);
}

// ===========================================================================
// 随机测试
// ===========================================================================

void TestRandomIntegrity() {
    Announce("随机：500 轮 随机容量/策略/批大小/水位/写方式/部分写");
    const FlushPolicy policies[4] = {FlushPolicy::kOnFull, FlushPolicy::kInterval,
                                     FlushPolicy::kOnExplicit, FlushPolicy::kHybrid};
    const std::size_t capacities[5] = {16, 32, 64, 256, 1024};
    const std::size_t batches[4] = {1, 2, 8, 64};
    const std::size_t partial_limits[3] = {0, 3, 17};

    for (int round = 0; round < 500; ++round) {
        const std::size_t capacity = capacities[RandomRange(0, 4)];
        const FlushPolicy policy = policies[RandomRange(0, 3)];
        const std::size_t max_batch = batches[RandomRange(0, 3)];
        const std::size_t threshold = RandomRange(1, capacity);
        BatchConfig config = Config(policy, max_batch, threshold,
                                    std::chrono::microseconds{RandomRange(0, 1000)});

        const bool use_batch_sink = RandomRange(0, 1) == 1;
        const std::size_t partial = partial_limits[RandomRange(0, 2)];
        BatchSink batch_sink;
        MemorySink memory_sink;
        batch_sink.max_write = partial;
        memory_sink.max_write = partial;

        std::string expected;
        if (use_batch_sink) {
            Writer<BatchSink> writer(batch_sink, capacity);
            writer.SetBatchConfig(config);
            for (int step = 0; step < 100; ++step) {
                const std::size_t choice = RandomRange(0, 99);
                if (choice < 50) {
                    const std::string message = RandomPayload(RandomRange(1, 80));
                    expected += message;
                    CHECK(writer.WriteString(message).ok());
                } else if (choice < 75) {
                    std::string message = RandomPayload(RandomRange(1, 80));
                    expected += message;
                    CHECK(writer.Write(AsBytes(message)) == message.size());
                } else {
                    const auto value = static_cast<std::uint8_t>(RandomRange(0, 255));
                    expected.push_back(static_cast<char>(value));
                    CHECK(writer.WriteByte(value).ok());
                }
            }
            if (RandomRange(0, 1) == 1) {
                CHECK(writer.Flush().ok());
            } else {
                CHECK(writer.FlushBatch().ok());
            }
            CHECK(batch_sink.data == expected);
            CHECK(writer.Buffered() == 0);
            CHECK(writer.PendingBatches() == 0);
        } else {
            Writer<MemorySink> writer(memory_sink, capacity);
            writer.SetBatchConfig(config);
            for (int step = 0; step < 100; ++step) {
                const std::size_t choice = RandomRange(0, 99);
                if (choice < 50) {
                    const std::string message = RandomPayload(RandomRange(1, 80));
                    expected += message;
                    CHECK(writer.WriteString(message).ok());
                } else if (choice < 75) {
                    std::string message = RandomPayload(RandomRange(1, 80));
                    expected += message;
                    CHECK(writer.Write(AsBytes(message)) == message.size());
                } else {
                    const auto value = static_cast<std::uint8_t>(RandomRange(0, 255));
                    expected.push_back(static_cast<char>(value));
                    CHECK(writer.WriteByte(value).ok());
                }
            }
            CHECK(writer.FlushBatch().ok());
            CHECK(memory_sink.data == expected);
            CHECK(writer.Buffered() == 0);
            CHECK(writer.PendingBatches() == 0);
        }
    }
}

// ===========================================================================
// 并发
// ===========================================================================

void TestConcurrentIndependentWriters() {
    Announce("并发：4 线程各自独立 Writer，互不干扰");
    constexpr int kThreads = 4;
    constexpr int kMessages = 2000;
    std::vector<std::string> results(kThreads);
    std::vector<int> failures(kThreads, 0);
    std::vector<std::thread> threads;
    for (int thread_index = 0; thread_index < kThreads; ++thread_index) {
        threads.emplace_back([thread_index, &results, &failures] {
            MemorySink sink;
            Writer<MemorySink> writer(sink, 4096);
            writer.SetBatchConfig(
                Config(thread_index % 2 == 0 ? FlushPolicy::kOnFull
                                             : FlushPolicy::kHybrid,
                       32, 512, std::chrono::microseconds{50}));
            std::string expected;
            for (int index = 0; index < kMessages; ++index) {
                const std::string message =
                    "t" + std::to_string(thread_index) + "-" +
                    std::to_string(index) + "\n";
                expected += message;
                if (!writer.WriteString(message).ok()) {
                    ++failures[thread_index];
                }
            }
            if (!writer.Flush().ok()) {
                ++failures[thread_index];
            }
            results[thread_index] = std::move(sink.data);
            if (results[thread_index] != expected) {
                ++failures[thread_index];
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    for (int thread_index = 0; thread_index < kThreads; ++thread_index) {
        CHECK(failures[thread_index] == 0);
    }
}

void TestConcurrentSharedWriter() {
    Announce("并发：4 线程共享 Writer + 外部锁（文档约定用法）");
    constexpr int kThreads = 4;
    constexpr int kMessages = 1000;
    constexpr std::size_t kMessageSize = 32;
    LockedBatchSink sink;
    Writer<LockedBatchSink> writer(sink, 4096);
    writer.SetBatchConfig(
        Config(FlushPolicy::kHybrid, 16, 1024, std::chrono::microseconds{50}));
    std::mutex writer_mutex;
    std::vector<std::thread> threads;
    for (int thread_index = 0; thread_index < kThreads; ++thread_index) {
        threads.emplace_back([thread_index, &writer, &writer_mutex] {
            for (int index = 0; index < kMessages; ++index) {
                std::string message(kMessageSize, static_cast<char>('a' + thread_index));
                std::memcpy(message.data(), &thread_index, sizeof(thread_index));
                std::memcpy(message.data() + sizeof(thread_index), &index, sizeof(index));
                std::lock_guard<std::mutex> guard(writer_mutex);
                CHECK(writer.WriteString(message).ok());
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    {
        std::lock_guard<std::mutex> guard(writer_mutex);
        CHECK(writer.FlushBatch().ok());
    }
    const std::string data = sink.Snapshot();
    CHECK(data.size() == static_cast<std::size_t>(kThreads) * kMessages * kMessageSize);
    std::vector<int> expected_index(kThreads, 0);
    bool valid = true;
    for (std::size_t offset = 0; offset + kMessageSize <= data.size();
         offset += kMessageSize) {
        int thread_index = 0;
        int message_index = 0;
        std::memcpy(&thread_index, data.data() + offset, sizeof(thread_index));
        std::memcpy(&message_index, data.data() + offset + sizeof(thread_index),
                    sizeof(message_index));
        if (thread_index < 0 || thread_index >= kThreads) {
            valid = false;
            break;
        }
        if (message_index != expected_index[thread_index]) {
            valid = false;
            break;
        }
        ++expected_index[thread_index];
        for (std::size_t index = sizeof(int) * 2; index < kMessageSize; ++index) {
            if (data[offset + index] != static_cast<char>('a' + thread_index)) {
                valid = false;
                break;
            }
        }
        if (!valid) {
            break;
        }
    }
    CHECK(valid);
    for (int thread_index = 0; thread_index < kThreads; ++thread_index) {
        CHECK(expected_index[thread_index] == kMessages);
    }
}

// ===========================================================================
// 真实 socket 场景
// ===========================================================================

void TestTcpLoopbackBatch() {
    Announce("场景：TCP loopback 真实 writev 批量（序连续/内容完整）");
    int client = -1;
    int server = -1;
    if (!MakeTcpPair(client, server)) {
        CHECK(false);
        return;
    }
    constexpr std::size_t kMessages = 2000;
    constexpr std::size_t kMessageSize = 64;
    FdSink sink(client);
    Writer<FdSink> writer(sink, 4096);
    writer.SetBatchConfig(
        Config(FlushPolicy::kHybrid, 32, 4096, std::chrono::microseconds{500}));
    std::thread reader([server] {
        const bool ok = ReadAndVerify(
            server, kMessages * kMessageSize, kMessageSize,
            [](const char* data, std::size_t index) {
                if (LoadSequence(data, 0) != index) {
                    return false;
                }
                for (std::size_t offset = 8; offset < kMessageSize; ++offset) {
                    if (data[offset] != static_cast<char>((index * 31 + offset) & 0xFF)) {
                        return false;
                    }
                }
                return true;
            });
        CHECK(ok);
        ::shutdown(server, SHUT_RDWR);
    });
    for (std::size_t index = 0; index < kMessages; ++index) {
        std::string message(kMessageSize, '\0');
        StoreSequence(message.data(), 0, index);
        for (std::size_t offset = 8; offset < kMessageSize; ++offset) {
            message[offset] = static_cast<char>((index * 31 + offset) & 0xFF);
        }
        CHECK(writer.WriteString(message).ok());
    }
    CHECK(writer.FlushBatch().ok());
    CHECK(sink.Syscalls() < 200);
    ::shutdown(client, SHUT_WR);
    reader.join();
    ::close(client);
    ::close(server);
}

void TestScenarioLogLines() {
    Announce("场景：日志行批量写（5000 行，回读逐行校验）");
    MemorySink sink;
    Writer<MemorySink> writer(sink, 4096);
    writer.SetBatchConfig(
        Config(FlushPolicy::kHybrid, 32, 4096, std::chrono::microseconds{100}));
    std::string expected;
    for (int index = 0; index < 5000; ++index) {
        const std::string line = "[INFO] record " + std::to_string(index) + "\n";
        expected += line;
        CHECK(writer.WriteString(line).ok());
    }
    CHECK(writer.Flush().ok());
    CHECK(sink.data == expected);
    CHECK(sink.calls < 600);
    CHECK(writer.Buffered() == 0);
}

void TestScenarioRpcResponse() {
    Announce("场景：RPC 小响应（长度头 + body 两次写，按 16 条批量）");
    BatchSink sink;
    Writer<BatchSink> writer(sink, 2048);
    writer.SetBatchConfig(Config(FlushPolicy::kOnFull, 16, 2048));
    std::string expected;
    for (int index = 0; index < 64; ++index) {
        const std::string body = "rpc-body-" + std::to_string(index) + ";";
        char header[4];
        const auto length = static_cast<std::uint32_t>(body.size());
        std::memcpy(header, &length, sizeof(length));
        expected.append(header, sizeof(header));
        expected += body;
        CHECK(writer.WriteString(std::string_view(header, sizeof(header))).ok());
        CHECK(writer.WriteString(body).ok());
    }
    CHECK(writer.FlushBatch().ok());
    CHECK(sink.data == expected);
    CHECK(writer.PendingBatches() == 0);
}

void TestScenarioSensorWindow() {
    Announce("场景：传感器时间窗口（kInterval 100ms，空闲后写触发）");
    BatchSink sink;
    Writer<BatchSink> writer(sink, 4096);
    writer.SetBatchConfig(
        Config(FlushPolicy::kInterval, 1024, 4096, std::chrono::microseconds{100000}));
    for (int index = 0; index < 10; ++index) {
        CHECK(writer.WriteString(std::string(16, static_cast<char>('0' + index))).ok());
    }
    CHECK(sink.batch_calls == 0);
    CHECK(writer.PendingBatches() == 10);
    SleepMs(150);
    CHECK(writer.WriteString(std::string(16, 'X')).ok());
    CHECK(sink.batch_calls == 1);
    CHECK(sink.batch_sizes[0] == 11);
    CHECK(sink.data.size() == 176);
}

void TestScenarioPipeSink() {
    Announce("场景：无批量接口的管道式 sink（一次 Write 合并 200 KiB）");
    MemorySink sink;
    Writer<MemorySink> writer(sink, 8192);
    writer.SetBatchConfig(Config(FlushPolicy::kOnFull, 64, 8192));
    std::mt19937 engine(7U);
    std::uniform_int_distribution<std::size_t> distribution(1, 200);
    std::string expected;
    for (int index = 0; index < 3000; ++index) {
        std::string chunk =
            RandomPayload(static_cast<std::size_t>(distribution(engine)));
        expected += chunk;
        CHECK(writer.Write(AsBytes(chunk)) == chunk.size());
    }
    CHECK(writer.Flush().ok());
    CHECK(sink.data == expected);
    CHECK(sink.calls < 200);
}

void TestScenarioSharedTcpConnection() {
    Announce("场景：多线程共享 TCP 连接（各自 Writer + 共享加锁 sink）");
    int client = -1;
    int server = -1;
    if (!MakeTcpPair(client, server)) {
        CHECK(false);
        return;
    }
    constexpr int kThreads = 2;
    constexpr int kMessagesPerThread = 500;
    constexpr std::size_t kMessageSize = 32;
    FdSink sink(client);
    std::mutex sink_mutex;
    std::vector<std::thread> writers;
    for (int thread_index = 0; thread_index < kThreads; ++thread_index) {
        writers.emplace_back([thread_index, &sink, &sink_mutex] {
            Writer<FdSink> writer(sink, 2048);
            writer.SetBatchConfig(Config(FlushPolicy::kOnFull, 16, 2048));
            for (int index = 0; index < kMessagesPerThread; ++index) {
                std::string message(kMessageSize, '\0');
                std::memcpy(message.data(), &thread_index, sizeof(thread_index));
                std::memcpy(message.data() + sizeof(thread_index), &index,
                            sizeof(index));
                for (std::size_t offset = sizeof(int) * 2; offset < kMessageSize;
                     ++offset) {
                    message[offset] = static_cast<char>('A' + thread_index);
                }
                {
                    std::lock_guard<std::mutex> guard(sink_mutex);
                    CHECK(writer.WriteString(message).ok());
                }
            }
            std::lock_guard<std::mutex> guard(sink_mutex);
            CHECK(writer.FlushBatch().ok());
        });
    }
    const std::size_t total = static_cast<std::size_t>(kThreads) * kMessagesPerThread *
                              kMessageSize;
    std::vector<int> expected_index(kThreads, 0);
    bool valid = true;
    std::thread reader([&server, total, &valid, &expected_index] {
        valid = ReadAndVerify(
            server, total, kMessageSize,
            [&expected_index](const char* data, std::size_t) {
                int thread_index = 0;
                int message_index = 0;
                std::memcpy(&thread_index, data, sizeof(thread_index));
                std::memcpy(&message_index, data + sizeof(thread_index),
                            sizeof(message_index));
                if (thread_index < 0 || thread_index >= kThreads) {
                    return false;
                }
                if (message_index != expected_index[thread_index]) {
                    return false;
                }
                ++expected_index[thread_index];
                for (std::size_t offset = sizeof(int) * 2; offset < kMessageSize;
                     ++offset) {
                    if (data[offset] != static_cast<char>('A' + thread_index)) {
                        return false;
                    }
                }
                return true;
            });
        ::shutdown(server, SHUT_RDWR);
    });
    for (std::thread& thread : writers) {
        thread.join();
    }
    ::shutdown(client, SHUT_WR);
    reader.join();
    CHECK(valid);
    for (int thread_index = 0; thread_index < kThreads; ++thread_index) {
        CHECK(expected_index[thread_index] == kMessagesPerThread);
    }
    ::close(client);
    ::close(server);
}

}  // namespace

int main() {
    TestConfigDefaultsAndClamp();
    TestEmptyFlush();
    TestBatchOrderAndContent();
    TestMaxBatchAutoFlush();
    TestThresholdOnFull();
    TestExplicitPolicy();
    TestIntervalPolicy();
    TestHybridPolicy();
    TestWriteByteCoalescing();
    TestLargeWriteBypass();
    TestPartialBatchWrites();
    TestPartialLinearWrites();
    TestErrorStickyBatch();
    TestZeroProgressBatch();
    TestGreedyBatch();
    TestRandomIntegrity();
    TestConcurrentIndependentWriters();
    TestConcurrentSharedWriter();
    TestTcpLoopbackBatch();
    TestScenarioLogLines();
    TestScenarioRpcResponse();
    TestScenarioSensorWindow();
    TestScenarioPipeSink();
    TestScenarioSharedTcpConnection();

    std::printf("[bufio-batch] ===== checks=%d failures=%d =====\n",
                g_checks.load(), g_failures.load());
    if (g_failures.load() != 0) {
        std::fprintf(stderr, "[bufio-batch] FAILED: %d 个断言未通过\n",
                     g_failures.load());
        return 1;
    }
    std::printf("[bufio-batch] all checks passed\n");
    return 0;
}
