/**
 * @file bench_bufio_batch.cpp
 * @brief `bufio::Writer` 批量 flush 的数据面基准：真实 TCP loopback 上的
 *        吞吐（msg/s）与延迟分位（p50/p99/p999）。
 *
 * ## 测什么
 *
 * 固定消息尺寸（16 / 64 / 256 B），逐条发送，对比：
 *
 * | 组 | 路径 | 系统调用 |
 * |---|---|---|
 * | `raw-send` | 裸 `send(2)`，无 bufio | 1 次/条 |
 * | `writer-flush` | `Writer::Write` + `Flush()`（单写基线） | 1 次/条 |
 * | `batch-N` | `Writer` + `max_batch=N`，自动 `FlushBatch()` | 1 次/N 条 |
 * | `policy-*` | 四种 `FlushPolicy`（64 B、`max_batch=64`） | 1 次/批 |
 *
 * 指标：
 * - **吞吐**：`消息数 / 墙钟`（仅测量窗口，不含建连与预热）；
 * - **延迟**：每条消息从 `Write` 入队到所在批 `FlushBatch` 返回的时刻
 *   （含排队时间），取 p50/p99/p999；
 * - **系统调用**：`FdSink::Write` + `WriteBatch` 计数，以及每系统调用
 *   承载的消息数。
 *
 * 吞吐轮**不打时间戳**（避免 `now()` 开销污染吞吐），延迟轮单独打点；
 * 两个窗口分开测量，口径在报告中注明。
 *
 * ## 正确性
 *
 * 每轮的数据都由读线程按消息边界解析：seq 必须严格递增、末尾填充字节必须
 * 匹配；字节数不足/多余都判失败（不会为了跑分牺牲校验）。延迟轮与吞吐轮
 * 各自校验。
 *
 * ## 用法
 * ```
 * ./.build-bento-wsl/go2cpp_bufio_batch_bench            # 全量
 * ./.build-bento-wsl/go2cpp_bufio_batch_bench --quick    # 缩短（冒烟用）
 * ```
 *
 * 结果同时写到 `/tmp/bufio_batch_bench.md`（Markdown）与
 * `/tmp/bufio_batch_raw.csv`（原始数据）。
 */
#include "go2cpp/bufio/bufio.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <netinet/in.h>
#include <netinet/tcp.h>
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
using go2cpp::NewError;
using go2cpp::bufio::BatchConfig;
using go2cpp::bufio::BatchEntry;
using go2cpp::bufio::ByteSpan;
using go2cpp::bufio::FlushPolicy;
using go2cpp::bufio::Writer;

using Clock = std::chrono::steady_clock;

enum class Mode { kRaw, kWriterFlush, kBatch };

struct CaseSpec {
    std::string name;
    Mode mode{Mode::kBatch};
    FlushPolicy policy{FlushPolicy::kOnExplicit};
    std::size_t max_batch{1};
    std::size_t threshold{4096};
    std::chrono::microseconds interval{0};
    std::size_t capacity{4096};
};

struct Stats {
    double seconds{0.0};
    std::uint64_t syscalls{0};
    std::uint64_t messages{0};
    bool verified{false};
    std::vector<std::uint32_t> latency_ns;  ///< 仅延迟轮填充
};

struct CaseResult {
    std::string name;
    std::string policy;
    std::size_t message_size{0};
    std::size_t max_batch{0};
    double throughput{0.0};  ///< msg/s
    double p50_us{0.0};
    double p99_us{0.0};
    double p999_us{0.0};
    double syscalls_per_message{0.0};
    double messages_per_syscall{0.0};
};

std::uint64_t NowNs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now().time_since_epoch())
            .count());
}

void StoreSequence(char* data, std::uint64_t value) {
    for (std::size_t index = 0; index < 8; ++index) {
        data[index] = static_cast<char>((value >> (8 * index)) & 0xFFU);
    }
}

std::uint64_t LoadSequence(const char* data) {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < 8; ++index) {
        value |= static_cast<std::uint64_t>(
                     static_cast<unsigned char>(data[index]))
                 << (8 * index);
    }
    return value;
}

void FillMessage(char* data, std::size_t size, std::uint64_t sequence) {
    StoreSequence(data, sequence);
    for (std::size_t index = 8; index < size; ++index) {
        data[index] = static_cast<char>((sequence * 31U + index) & 0xFFU);
    }
}

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
    const bool named =
        listening && ::getsockname(listener, reinterpret_cast<sockaddr*>(&address),
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
    // 不设置 TCP_NODELAY、不放大内核缓冲：与仓库既有 bench_net_batch 及
    // 裸 send 探针保持相同口径（默认 Nagle 会合并小包，这正是 bufio 批量
    // 要对比的真实场景）。
    return true;
}

/// 读线程：按消息边界消费并校验 seq 连续 / 末尾填充。
class ReaderThread {
public:
    ReaderThread(int descriptor, std::size_t message_size, std::size_t total,
                 bool verify)
        : m_descriptor(descriptor),
          m_message_size(message_size),
          m_total(total),
          m_total_bytes(total * message_size),
          m_verify(verify) {}

    void Start() {
        m_thread = std::thread([this] { Loop(); });
    }

    bool Join() {
        if (m_thread.joinable()) {
            m_thread.join();
        }
        return m_ok;
    }

    const std::string& Reason() const noexcept { return m_reason; }

private:
    void Loop() {
        std::string buffer(256 * 1024, '\0');
        std::string carry;
        std::size_t received = 0;
        std::size_t messages = 0;
        while (received < m_total_bytes) {
            const ssize_t count = ::recv(m_descriptor, buffer.data(), buffer.size(), 0);
            if (count < 0) {
                if (errno == EINTR) {
                    continue;
                }
                m_reason = "recv 失败";
                m_ok = false;
                return;
            }
            if (count == 0) {
                break;
            }
            received += static_cast<std::size_t>(count);
            carry.append(buffer.data(), static_cast<std::size_t>(count));
            std::size_t consumed = 0;
            while (carry.size() - consumed >= m_message_size) {
                const char* message = carry.data() + consumed;
                if (m_verify) {
                    const std::uint64_t actual = LoadSequence(message);
                    if (actual != messages) {
                        m_reason = "seq 不匹配: 期望 " + std::to_string(messages) +
                                   " 实际 " + std::to_string(actual);
                        m_ok = false;
                        return;
                    }
                    const char expected = static_cast<char>(
                        ((messages * 31U) + (m_message_size - 1)) & 0xFFU);
                    if (message[m_message_size - 1] != expected) {
                        m_reason = "填充不匹配: seq=" + std::to_string(messages);
                        m_ok = false;
                        return;
                    }
                }
                consumed += m_message_size;
                ++messages;
            }
            carry.erase(0, consumed);
        }
        m_ok = received == m_total_bytes && carry.empty() &&
               messages == m_total;
        if (!m_ok) {
            m_reason = "字节数/条数不符: received=" + std::to_string(received) +
                       " 期望=" + std::to_string(m_total_bytes) +
                       " messages=" + std::to_string(messages) +
                       " carry=" + std::to_string(carry.size());
        }
    }

    int m_descriptor;
    std::size_t m_message_size;
    std::size_t m_total;
    std::size_t m_total_bytes;
    bool m_verify{true};
    bool m_ok{false};
    std::string m_reason;
    std::thread m_thread;
};

/// 真实 fd 的批量 sink：Write=send(2)，WriteBatch=writev(2)。
class FdSink {
public:
    explicit FdSink(int descriptor) : m_descriptor(descriptor) {}

    streams::WriteResult Write(const char* data, std::size_t size) {
        ++m_syscalls;
        const ssize_t written = ::send(m_descriptor, data, size, MSG_NOSIGNAL);
        if (written < 0) {
            return streams::WriteResult{0, NewError("bench sink: send failed")};
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
            return streams::WriteResult{0, NewError("bench sink: writev failed")};
        }
        return streams::WriteResult{static_cast<std::size_t>(written), {}};
    }

    std::uint64_t Syscalls() const noexcept { return m_syscalls; }

private:
    int m_descriptor;
    std::uint64_t m_syscalls{0};
    std::vector<iovec> m_iovecs;
};

std::uint64_t RawSendAll(int descriptor, const char* data, std::size_t size) {
    std::uint64_t calls = 0;
    std::size_t sent = 0;
    while (sent < size) {
        const ssize_t written =
            ::send(descriptor, data + sent, size - sent, MSG_NOSIGNAL);
        ++calls;
        if (written <= 0) {
            break;
        }
        sent += static_cast<std::size_t>(written);
    }
    return calls;
}

/// 跑一轮：建连、启动读线程、发送 `count` 条、收尾、校验。
Stats RunRound(const CaseSpec& spec, std::size_t message_size,
               std::size_t warmup, std::size_t count, bool record_latency) {
    Stats stats;
    stats.messages = count;
    int client = -1;
    int server = -1;
    if (!MakeTcpPair(client, server)) {
        return stats;
    }
    const std::size_t total = warmup + count;
    ReaderThread reader(server, message_size, total, /*verify=*/true);
    reader.Start();

    FdSink sink(client);
    Writer<FdSink> writer(sink, spec.capacity);
    writer.SetBatchConfig(BatchConfig{spec.policy, spec.max_batch, spec.threshold,
                                      spec.interval});
    if (spec.mode == Mode::kWriterFlush) {
        writer.SetBatchConfig(BatchConfig{FlushPolicy::kOnExplicit, 1024,
                                          spec.capacity, std::chrono::microseconds{0}});
    }

    std::vector<char> message(message_size, '\0');
    std::vector<std::uint32_t> latency;
    std::vector<std::uint64_t> enqueue;
    if (record_latency) {
        latency.reserve(count);
        enqueue.reserve(std::max<std::size_t>(spec.max_batch, 1));
    }

    std::uint64_t raw_calls = 0;
    std::uint64_t sequence = 0;
    const auto send_one = [&] {
        FillMessage(message.data(), message_size, sequence);
        if (spec.mode == Mode::kRaw) {
            raw_calls += RawSendAll(client, message.data(), message_size);
        } else if (spec.mode == Mode::kWriterFlush) {
            writer.Write(ByteSpan(reinterpret_cast<std::uint8_t*>(message.data()),
                                  message_size));
            writer.Flush();
        } else {
            writer.Write(ByteSpan(reinterpret_cast<std::uint8_t*>(message.data()),
                                  message_size));
        }
        ++sequence;
    };

    // 预热与测量分开：预热不计时、不打时间戳，且按批大小对齐，
    // 保证测量窗口从批边界开始（延迟记录与自动 flush 对齐）。
    for (std::size_t index = 0; index < warmup; ++index) {
        send_one();
    }
    if (spec.mode == Mode::kBatch) {
        writer.FlushBatch();
    } else if (spec.mode == Mode::kWriterFlush) {
        writer.Flush();
    }

    const auto begin = Clock::now();
    for (std::size_t index = 0; index < count; ++index) {
        if (record_latency) {
            // 先记录入队时刻，再写：延迟 = 入队 -> 所在批 flush 完成。
            enqueue.push_back(NowNs());
        }
        send_one();
        if (spec.mode == Mode::kBatch && (index + 1) % spec.max_batch == 0 &&
            record_latency) {
            const std::uint64_t done = NowNs();
            for (std::uint64_t queued : enqueue) {
                latency.push_back(static_cast<std::uint32_t>(done - queued));
            }
            enqueue.clear();
        }
        if (record_latency && spec.mode != Mode::kBatch) {
            const std::uint64_t done = NowNs();
            latency.push_back(static_cast<std::uint32_t>(done - enqueue.back()));
            enqueue.clear();
        }
    }
    if (spec.mode == Mode::kBatch) {
        writer.FlushBatch();
        if (record_latency && !enqueue.empty()) {
            const std::uint64_t done = NowNs();
            for (std::uint64_t queued : enqueue) {
                latency.push_back(static_cast<std::uint32_t>(done - queued));
            }
            enqueue.clear();
        }
    } else if (spec.mode == Mode::kWriterFlush) {
        writer.Flush();
    }
    stats.seconds = std::chrono::duration<double>(Clock::now() - begin).count();
    stats.syscalls = spec.mode == Mode::kRaw ? raw_calls : sink.Syscalls();
    stats.latency_ns = std::move(latency);
    ::shutdown(client, SHUT_WR);
    stats.verified = reader.Join();
    ::close(client);
    ::close(server);
    if (!stats.verified) {
        std::fprintf(stderr, "[bench] 校验失败: case=%s size=%zu reason=%s\n",
                     spec.name.c_str(), message_size, reader.Reason().c_str());
    }
    return stats;
}

double Percentile(const std::vector<std::uint32_t>& sorted, double fraction) {
    if (sorted.empty()) {
        return 0.0;
    }
    std::size_t index = static_cast<std::size_t>(
        static_cast<double>(sorted.size() - 1) * fraction);
    if (index >= sorted.size()) {
        index = sorted.size() - 1;
    }
    return static_cast<double>(sorted[index]) / 1000.0;  // ns -> µs
}

std::vector<CaseSpec> BuildCases(std::size_t message_size) {
    std::vector<CaseSpec> cases;
    cases.push_back(CaseSpec{"raw-send", Mode::kRaw, FlushPolicy::kOnExplicit, 1,
                             4096, std::chrono::microseconds{0}, 4096});
    cases.push_back(CaseSpec{"writer-flush", Mode::kWriterFlush,
                             FlushPolicy::kOnExplicit, 1, 4096,
                             std::chrono::microseconds{0}, 4096});
    for (std::size_t batch : {8U, 16U, 32U, 64U, 128U, 256U}) {
        const std::size_t capacity = std::max<std::size_t>(
            65536, batch * message_size * 2);
        CaseSpec spec{"batch-" + std::to_string(batch), Mode::kBatch,
                      FlushPolicy::kOnExplicit, batch, capacity,
                      std::chrono::microseconds{0}, capacity};
        cases.push_back(std::move(spec));
    }
    if (message_size == 64) {
        const std::size_t capacity = 65536;
        cases.push_back(CaseSpec{"policy-onfull", Mode::kBatch,
                                 FlushPolicy::kOnFull, 64, capacity,
                                 std::chrono::microseconds{0}, capacity});
        cases.push_back(CaseSpec{"policy-interval", Mode::kBatch,
                                 FlushPolicy::kInterval, 64, capacity,
                                 std::chrono::microseconds{100}, capacity});
        cases.push_back(CaseSpec{"policy-explicit", Mode::kBatch,
                                 FlushPolicy::kOnExplicit, 64, capacity,
                                 std::chrono::microseconds{0}, capacity});
        cases.push_back(CaseSpec{"policy-hybrid", Mode::kBatch,
                                 FlushPolicy::kHybrid, 64, capacity,
                                 std::chrono::microseconds{100}, capacity});
    }
    return cases;
}

const char* PolicyName(FlushPolicy policy) {
    switch (policy) {
        case FlushPolicy::kOnFull:
            return "kOnFull";
        case FlushPolicy::kInterval:
            return "kInterval";
        case FlushPolicy::kOnExplicit:
            return "kOnExplicit";
        case FlushPolicy::kHybrid:
            return "kHybrid";
    }
    return "unknown";
}

std::size_t MessageCountFor(std::size_t message_size, bool quick) {
    std::size_t count = 0;
    if (message_size <= 16) {
        count = 400000;
    } else if (message_size <= 64) {
        count = 300000;
    } else {
        count = 150000;
    }
    return quick ? count / 8 : count;
}

CaseResult Measure(const CaseSpec& spec, std::size_t message_size, bool quick) {
    CaseResult result;
    result.name = spec.name;
    result.policy = PolicyName(spec.policy);
    result.message_size = message_size;
    result.max_batch = spec.max_batch;

    const std::size_t count = MessageCountFor(message_size, quick);
    std::size_t warmup = count / 5;
    if (spec.max_batch > 0) {
        warmup = (warmup / spec.max_batch) * spec.max_batch;
        if (warmup == 0) {
            warmup = spec.max_batch;
        }
    }
    const int throughput_rounds = quick ? 2 : 3;

    std::vector<double> throughputs;
    std::uint64_t syscalls = 0;
    bool verified = true;
    for (int round = 0; round < throughput_rounds; ++round) {
        const Stats stats = RunRound(spec, message_size, warmup, count, false);
        verified = verified && stats.verified;
        throughputs.push_back(static_cast<double>(stats.messages) / stats.seconds);
        syscalls = stats.syscalls;
    }
    std::sort(throughputs.begin(), throughputs.end());
    result.throughput = throughputs[throughputs.size() / 2];
    result.syscalls_per_message = static_cast<double>(syscalls) /
                                  static_cast<double>(count);
    result.messages_per_syscall =
        syscalls == 0 ? 0.0 : static_cast<double>(count) / static_cast<double>(syscalls);

    std::size_t latency_warmup = warmup / 2;
    if (spec.max_batch > 0) {
        latency_warmup = (latency_warmup / spec.max_batch) * spec.max_batch;
    }
    const Stats latency_stats =
        RunRound(spec, message_size, latency_warmup, count, true);
    verified = verified && latency_stats.verified;
    std::vector<std::uint32_t> sorted = latency_stats.latency_ns;
    std::sort(sorted.begin(), sorted.end());
    result.p50_us = Percentile(sorted, 0.50);
    result.p99_us = Percentile(sorted, 0.99);
    result.p999_us = Percentile(sorted, 0.999);
    if (!verified) {
        std::fprintf(stderr, "[bench] 本轮数据校验失败: %s/%zu\n", spec.name.c_str(),
                     message_size);
    }
    return result;
}

}  // namespace

int main(int argc, char** argv) {
    bool quick = false;
    for (int index = 1; index < argc; ++index) {
        if (std::string_view(argv[index]) == "--quick") {
            quick = true;
        }
    }
    const std::vector<std::size_t> sizes{16, 64, 256};
    std::vector<CaseResult> results;
    for (std::size_t message_size : sizes) {
        for (const CaseSpec& spec : BuildCases(message_size)) {
            const CaseResult result = Measure(spec, message_size, quick);
            std::printf("[bench] size=%3zu case=%-16s policy=%-11s thr=%8.2f Mmsg/s "
                        "p50=%6.2f us p99=%7.2f us p999=%8.2f us syscalls/msg=%.4f\n",
                        message_size, result.name.c_str(), result.policy.c_str(),
                        result.throughput / 1e6, result.p50_us, result.p99_us,
                        result.p999_us, result.syscalls_per_message);
            std::fflush(stdout);
            results.push_back(result);
        }
    }

    std::FILE* csv = std::fopen("/tmp/bufio_batch_raw.csv", "w");
    if (csv != nullptr) {
        std::fprintf(csv,
                     "message_size,case,policy,max_batch,throughput_msg_s,p50_us,"
                     "p99_us,p999_us,syscalls_per_message,messages_per_syscall\n");
        for (const CaseResult& result : results) {
            std::fprintf(csv, "%zu,%s,%s,%zu,%.0f,%.3f,%.3f,%.3f,%.6f,%.4f\n",
                         result.message_size, result.name.c_str(),
                         result.policy.c_str(), result.max_batch, result.throughput,
                         result.p50_us, result.p99_us, result.p999_us,
                         result.syscalls_per_message, result.messages_per_syscall);
        }
        std::fclose(csv);
    }

    std::FILE* markdown = std::fopen("/tmp/bufio_batch_bench.md", "w");
    if (markdown != nullptr) {
        std::fprintf(markdown, "# bufio 批量 flush 基准（TCP loopback）\n\n");
        for (std::size_t message_size : sizes) {
            std::fprintf(markdown,
                         "## %zu B\n\n| case | policy | max_batch | 吞吐 Mmsg/s | "
                         "p50 µs | p99 µs | p999 µs | syscalls/msg | msg/syscall |\n"
                         "|---|---|---:|---:|---:|---:|---:|---:|---:|\n",
                         message_size);
            for (const CaseResult& result : results) {
                if (result.message_size != message_size) {
                    continue;
                }
                std::fprintf(markdown,
                             "| %s | %s | %zu | %.2f | %.2f | %.2f | %.2f | %.4f | %.1f |\n",
                             result.name.c_str(), result.policy.c_str(),
                             result.max_batch, result.throughput / 1e6, result.p50_us,
                             result.p99_us, result.p999_us,
                             result.syscalls_per_message,
                             result.messages_per_syscall);
            }
            std::fprintf(markdown, "\n");
        }
        std::fclose(markdown);
    }
    std::printf("[bench] 结果已写入 /tmp/bufio_batch_bench.md 与 /tmp/bufio_batch_raw.csv\n");
    return 0;
}
