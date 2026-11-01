/**
 * @file netem_probe.cpp
 * @brief 弱网探针：在 tc netem / 用户态代理（loss / delay）下跑可靠 + 不可靠流。
 * @details 单进程双 Link：服务端监听 --port（默认 0：bind 端口 0 后由
 *          getsockname 取内核分配的随机端口），客户端连接 127.0.0.1。
 *          默认随机端口保证多测试/多 agent 并发时不会抢同一端口。可靠流
 *          必须 100% 按序到达；不可靠流允许丢包，但交付序号必须严格递增
 *          （DropStale 生效）。
 *
 *          **延迟口径（2026-11-01 修正）**：历史版本 p50/p95 用整数毫秒
 *          统计，回环真实单向延迟 <1ms 时被量化成 0/1；且探针交替
 *          `client.RunOnce(1); server.RunOnce(1)` 会让已进内核队列的帧
 *          白等一次 1ms poll。报告里的「Bento baseline p50 1.00ms」是
 *          这两件事的叠加，不是数据面耗时。现在同时输出：
 *            - `latency_us`：微秒分位数（主指标）；
 *            - `stage_us`：SendStream 入队 / 驱动空档 / 服务端处理三段；
 *            - 旧 `p50_ms` 行保持原样（脚本兼容）。
 *
 *          三种驱动模式（--drive）：
 *            - `pump1`（默认，复现历史口径）：交替阻塞 poll 1ms；
 *            - `pump0`：交替非阻塞 RunOnce(0) 自旋（公平驱动下限）；
 *            - `thread`：双线程各跑一个 Link（生产用法，内核唤醒）。
 *
 *          `--flood` 用于 ≥1e7 帧吞吐压测：不做固定 3ms 抽水，客户端
 *          持续灌入、服务端持续抽干，只统计送达率/顺序/帧率。
 *
 *          退出码：0 通过；1 断言失败/超时；2 参数错误。
 * @author go2cpp contributors
 * @date 2026-10-26
 * @copyright MIT License
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <sched.h>

#include <memory>
#include <thread>
#include <vector>

#include "go2cpp/bento/net/link.hpp"
#include "udp_proxy.hpp"

namespace {

using namespace go2cpp::bento;

enum class Drive {
    kPump1,   ///< 交替阻塞 poll 1ms（历史探针口径）。
    kPump0,   ///< 交替非阻塞 RunOnce(0) 自旋。
    kThread,  ///< 双线程，每个 Link 一个接收线程（生产用法）。
};

struct Options {
    /// 0 = 由内核分配（默认）；非 0 才固定端口，仅用于 tc netem 这类
    /// 需要预先知道端口来安装过滤器的场景。
    std::uint16_t port = 0;
    std::uint32_t messages = 200;
    std::size_t payload_bytes = 256;
    int timeout_ms = 60000;
    int loss_percent = 0;
    int delay_ms = 0;
    std::uint32_t seed = 0x9E3779B9U;
    bool fixed_pto = false;  ///< 复现旧实现：固定 200ms 基础 PTO。
    bool no_sack = false;    ///< 关闭 SACK 多块扩展（对照旧端 32 位行为）。
    bool verbose = false;    ///< 每秒打印一次收发进度（诊断长尾卡顿）。
    Drive drive = Drive::kPump1;
    bool flood = false;      ///< 吞吐模式：不插固定抽水，持续灌入。
    int send_pump_ms = 3;    ///< 非 flood 模式下每 5 条插入的抽水时长。
    std::uint32_t warmup = 0;  ///< 头部预热消息数（不计入延迟统计）。
    bool send_datagram = true;  ///< 是否同时发不可靠流（吞吐压测可关）。
    int pin_client = -1;  ///< 客户端主线程绑核（-1 = 不绑）。
    int pin_server = -1;  ///< 服务端线程绑核（-1 = 不绑；thread 模式有效）。
    // 调参覆盖项（0 = 用库默认值）：一次构建即可扫多组弱网参数。
    std::uint32_t initial_cwnd = 0;
    std::uint32_t max_cwnd = 0;
    std::uint32_t fr_threshold = 0;
    std::uint32_t initial_rtt_ms = 0;
    std::uint32_t min_pto_ms = 0;
    std::uint32_t max_pto_ms = 0;
    std::uint32_t ack_interval_ms = 0;
};

void WriteU32(std::uint8_t* out, std::uint32_t value) {
    out[0] = static_cast<std::uint8_t>(value >> 24);
    out[1] = static_cast<std::uint8_t>(value >> 16);
    out[2] = static_cast<std::uint8_t>(value >> 8);
    out[3] = static_cast<std::uint8_t>(value);
}

std::uint32_t ReadU32(const std::uint8_t* in) {
    return (static_cast<std::uint32_t>(in[0]) << 24) | (static_cast<std::uint32_t>(in[1]) << 16) |
           (static_cast<std::uint32_t>(in[2]) << 8) | static_cast<std::uint32_t>(in[3]);
}

bool ParseDrive(const char* name, Drive* out) {
    if (std::strcmp(name, "pump1") == 0) {
        *out = Drive::kPump1;
        return true;
    }
    if (std::strcmp(name, "pump0") == 0) {
        *out = Drive::kPump0;
        return true;
    }
    if (std::strcmp(name, "thread") == 0) {
        *out = Drive::kThread;
        return true;
    }
    return false;
}

const char* DriveName(Drive drive) {
    switch (drive) {
        case Drive::kPump1:
            return "pump1";
        case Drive::kPump0:
            return "pump0";
        case Drive::kThread:
            return "thread";
    }
    return "?";
}

bool ParseOptions(int argc, char** argv, Options* options) {
    for (int i = 1; i < argc; ++i) {
        const char* argument = argv[i];
        if (std::strcmp(argument, "--port") == 0 && i + 1 < argc) {
            const unsigned long port = std::strtoul(argv[++i], nullptr, 10);
            if (port > 65535UL) {
                std::fprintf(stderr, "invalid port: %lu\n", port);
                return false;
            }
            options->port = static_cast<std::uint16_t>(port);
        } else if (std::strcmp(argument, "--messages") == 0 && i + 1 < argc) {
            options->messages = static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (std::strcmp(argument, "--payload") == 0 && i + 1 < argc) {
            options->payload_bytes = static_cast<std::size_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (std::strcmp(argument, "--timeout-ms") == 0 && i + 1 < argc) {
            options->timeout_ms = std::atoi(argv[++i]);
        } else if (std::strcmp(argument, "--loss") == 0 && i + 1 < argc) {
            options->loss_percent = std::atoi(argv[++i]);
        } else if (std::strcmp(argument, "--delay-ms") == 0 && i + 1 < argc) {
            options->delay_ms = std::atoi(argv[++i]);
        } else if (std::strcmp(argument, "--seed") == 0 && i + 1 < argc) {
            options->seed = static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (std::strcmp(argument, "--drive") == 0 && i + 1 < argc) {
            if (!ParseDrive(argv[++i], &options->drive)) {
                std::fprintf(stderr, "invalid drive: %s\n", argv[i]);
                return false;
            }
        } else if (std::strcmp(argument, "--flood") == 0) {
            options->flood = true;
        } else if (std::strcmp(argument, "--send-pump-ms") == 0 && i + 1 < argc) {
            options->send_pump_ms = std::atoi(argv[++i]);
        } else if (std::strcmp(argument, "--warmup") == 0 && i + 1 < argc) {
            options->warmup = static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (std::strcmp(argument, "--pin-client") == 0 && i + 1 < argc) {
            options->pin_client = std::atoi(argv[++i]);
        } else if (std::strcmp(argument, "--pin-server") == 0 && i + 1 < argc) {
            options->pin_server = std::atoi(argv[++i]);
        } else if (std::strcmp(argument, "--no-datagram") == 0) {
            options->send_datagram = false;
        } else if (std::strcmp(argument, "--fixed-pto") == 0) {
            options->fixed_pto = true;
        } else if (std::strcmp(argument, "--no-sack") == 0) {
            options->no_sack = true;
        } else if (std::strcmp(argument, "--verbose") == 0) {
            options->verbose = true;
        } else if (std::strcmp(argument, "--initial-cwnd") == 0 && i + 1 < argc) {
            options->initial_cwnd =
                static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (std::strcmp(argument, "--max-cwnd") == 0 && i + 1 < argc) {
            options->max_cwnd =
                static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (std::strcmp(argument, "--fr-threshold") == 0 && i + 1 < argc) {
            options->fr_threshold =
                static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (std::strcmp(argument, "--initial-rtt") == 0 && i + 1 < argc) {
            options->initial_rtt_ms =
                static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (std::strcmp(argument, "--min-pto") == 0 && i + 1 < argc) {
            options->min_pto_ms =
                static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (std::strcmp(argument, "--max-pto") == 0 && i + 1 < argc) {
            options->max_pto_ms =
                static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (std::strcmp(argument, "--ack-interval-ms") == 0 && i + 1 < argc) {
            options->ack_interval_ms =
                static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        } else {
            std::fprintf(stderr, "unknown or incomplete argument: %s\n", argument);
            return false;
        }
    }
    if (options->messages == 0 || options->payload_bytes < 8 ||
        options->payload_bytes > kMaxFramePayload || options->loss_percent < 0 ||
        options->loss_percent > 100 || options->delay_ms < 0 || options->send_pump_ms < 0 ||
        options->warmup >= options->messages) {
        std::fprintf(stderr, "invalid range: port/messages/payload/loss/delay/warmup\n");
        return false;
    }
    if (options->flood && options->drive == Drive::kThread) {
        std::fprintf(stderr, "invalid combination: --flood + --drive thread\n");
        return false;
    }
    return true;
}

/// @brief 把当前线程绑到指定 CPU（-1 表示不动）。失败只告警，不影响测量。
void PinCurrentThread(int cpu) {
    if (cpu < 0) {
        return;
    }
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (::pthread_setaffinity_np(::pthread_self(), sizeof(set), &set) != 0) {
        std::fprintf(stderr, "netem_probe: warn: pin cpu %d failed\n", cpu);
    }
}

/// @brief 微秒延迟记录器：小样本保精确分位，大样本走直方图（内存有界）。
class LatencyRecorder final {
   public:
    /// 直方图分辨率 1us，覆盖 0..19999us；更长的尾巴单独计数。
    static constexpr std::size_t kBucketCount = 20000;

    explicit LatencyRecorder(std::uint32_t samples) {
        // 200 万条以内保留逐样本数据（约 16MB）；更大规模只留直方图。
        if (samples <= 2000000U) {
            m_values.reserve(samples);
            m_want_values = true;
        }
        m_hist.assign(kBucketCount, 0);
    }

    void Record(std::uint64_t microseconds) {
        if (m_want_values) {
            m_values.push_back(microseconds);
        }
        if (microseconds < kBucketCount) {
            ++m_hist[static_cast<std::size_t>(microseconds)];
        } else {
            ++m_overflow;
        }
    }

    void Finalize() {
        if (m_want_values) {
            return;
        }
        std::uint64_t total = m_overflow;
        for (std::uint32_t bucket : m_hist) {
            total += bucket;
        }
        m_hist_count = static_cast<std::size_t>(total);
    }

    std::size_t count() const noexcept {
        return m_want_values ? m_values.size() : m_hist_count;
    }

    std::uint64_t Percentile(double ratio) const {
        if (m_want_values) {
            if (m_values.empty()) {
                return 0;
            }
            std::vector<std::uint64_t> sorted = m_values;
            std::sort(sorted.begin(), sorted.end());
            const double index = ratio * static_cast<double>(sorted.size() - 1);
            return sorted[std::min(static_cast<std::size_t>(index + 0.5), sorted.size() - 1)];
        }
        if (m_hist_count == 0) {
            return 0;
        }
        const double target = ratio * static_cast<double>(m_hist_count);
        std::uint64_t cumulative = 0;
        for (std::size_t bucket = 0; bucket < m_hist.size(); ++bucket) {
            cumulative += m_hist[bucket];
            if (static_cast<double>(cumulative) >= target) {
                return bucket;
            }
        }
        return kBucketCount;  // 溢出桶：>=20000us。
    }

    std::uint64_t Max() const {
        if (m_want_values) {
            std::uint64_t maximum = 0;
            for (std::uint64_t value : m_values) {
                maximum = std::max(maximum, value);
            }
            return maximum;
        }
        for (std::size_t bucket = m_hist.size(); bucket > 0; --bucket) {
            if (m_hist[bucket - 1] != 0) {
                return bucket - 1;
            }
        }
        return m_overflow != 0 ? kBucketCount : 0;
    }

    std::uint64_t Mean() const {
        if (!m_want_values || m_values.empty()) {
            return 0;
        }
        std::uint64_t sum = 0;
        for (std::uint64_t value : m_values) {
            sum += value;
        }
        return sum / m_values.size();
    }

   private:
    bool m_want_values = false;
    std::vector<std::uint64_t> m_values;
    std::vector<std::uint32_t> m_hist;
    std::uint64_t m_overflow = 0;
    std::size_t m_hist_count = 0;
};

std::uint64_t PercentileMs(std::vector<std::uint64_t> samples, double ratio) {
    if (samples.empty()) {
        return 0;
    }
    std::sort(samples.begin(), samples.end());
    const double index = ratio * static_cast<double>(samples.size() - 1);
    return samples[std::min(static_cast<std::size_t>(index + 0.5), samples.size() - 1)];
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    if (!ParseOptions(argc, argv, &options)) {
        return 2;
    }

    // 调参覆盖：只改显式给出的项，其余保持库默认。
    auto apply_tuning = [&options](LinkConfig& config) {
        // 弱网探针是性能对照工具，不承载生产数据；显式关闭默认加密策略
        // （require_encryption=false）以保留纯明文基线，避免与加密开销混淆。
        config.require_encryption = false;
        // 对照实验：关闭能力协商后双方只发 32 位位图 ACK（旧端行为）。
        config.enable_sack_ext = !options.no_sack;
        if (options.initial_cwnd != 0) {
            config.reliable.congestion.initial_cwnd_packets = options.initial_cwnd;
        }
        if (options.max_cwnd != 0) {
            config.reliable.congestion.max_cwnd_packets = options.max_cwnd;
        }
        if (options.fr_threshold != 0) {
            config.reliable.fast_retransmit_threshold = options.fr_threshold;
        }
        if (options.initial_rtt_ms != 0) {
            config.reliable.rtt.initial_rtt_ms = options.initial_rtt_ms;
        }
        if (options.min_pto_ms != 0) {
            config.reliable.rtt.min_pto_ms = options.min_pto_ms;
        }
        if (options.max_pto_ms != 0) {
            config.reliable.rtt.max_pto_ms = options.max_pto_ms;
        }
        if (options.ack_interval_ms != 0) {
            config.ack_interval_ms = options.ack_interval_ms;
        }
    };

    LinkConfig server_config;
    server_config.bind_address = "127.0.0.1";
    server_config.bind_port = options.port;
    server_config.heartbeat_interval_ms = 200;
    server_config.ack_interval_ms = 10;
    server_config.peer_timeout_ms = 10000;
    if (options.fixed_pto) {
        server_config.reliable.rtt.fixed_pto_ms = 200;
    }
    apply_tuning(server_config);
    Link server(server_config);
    std::string error;
    if (!server.Open(&error)) {
        std::fprintf(stderr, "netem_probe: server.Open failed: %s\n", error.c_str());
        return 1;
    }
    // bind_port == 0 时 Link 内部用 getsockname 回填实际端口；打印出来
    // 便于日志核对，也证明并发实例拿到的是不同端口。
    std::printf("netem_probe: listening on 127.0.0.1:%u\n",
                static_cast<unsigned>(server.local_port()));

    // --loss/--delay-ms 非 0 时启用用户态弱网代理（无 CAP_NET_ADMIN 时
    // 代替 tc netem）。客户端连代理，代理按方向丢包/延迟后转发。
    std::unique_ptr<bento_test::UdpProxy> proxy;
    std::uint16_t peer_port = server.local_port();
    if (options.loss_percent > 0 || options.delay_ms > 0) {
        sockaddr_in server_address{};
        if (!UdpSocket::ResolveIPv4("127.0.0.1", server.local_port(), &server_address, &error)) {
            std::fprintf(stderr, "netem_probe: resolve server failed: %s\n", error.c_str());
            return 1;
        }
        proxy = std::make_unique<bento_test::UdpProxy>(server_address, options.loss_percent,
                                                       options.delay_ms, options.seed);
        if (!proxy->Start(&error)) {
            std::fprintf(stderr, "netem_probe: proxy start failed: %s\n", error.c_str());
            return 1;
        }
        peer_port = proxy->port();
    }

    LinkConfig client_config;
    client_config.bind_address = "127.0.0.1";
    client_config.bind_port = 0;
    client_config.peer_address = "127.0.0.1";
    client_config.peer_port = peer_port;
    client_config.heartbeat_interval_ms = 200;
    client_config.ack_interval_ms = 10;
    client_config.peer_timeout_ms = 10000;
    client_config.handshake_timeout_ms = 15000;
    client_config.random_seed = 0x4E455445U;  // "NETE"
    if (options.fixed_pto) {
        client_config.reliable.rtt.fixed_pto_ms = 200;
    }
    apply_tuning(client_config);
    Link client(client_config);
    if (!client.Open(&error)) {
        std::fprintf(stderr, "netem_probe: client.Open failed: %s\n", error.c_str());
        return 1;
    }

    bool server_connected = false;
    bool client_connected = false;
    server.on_connected = [&server_connected]() { server_connected = true; };
    client.on_connected = [&client_connected]() { client_connected = true; };

    const std::uint32_t kMessages = options.messages;
    // 逐样本数组只在小规模时保留；≥200 万条走直方图，内存有界。
    const bool keep_samples = kMessages <= 2000000U;
    std::vector<std::uint64_t> send_ms(keep_samples ? kMessages : 0, 0);
    std::vector<std::uint64_t> latency_ms(keep_samples ? kMessages : 0, 0);
    std::vector<bool> latency_seen(keep_samples ? kMessages : 0, false);
    std::vector<std::uint64_t> send_us(keep_samples ? kMessages : 0, 0);
    std::vector<std::uint64_t> enqueue_us(keep_samples ? kMessages : 0, 0);
    LatencyRecorder latency_us(kMessages);
    LatencyRecorder stage_gap_us(kMessages);
    LatencyRecorder stage_proc_us(kMessages);
    LatencyRecorder stage_enqueue_us(kMessages);
    // 服务端最近一次 RunOnce 的入口时刻（on_stream 用它切分「驱动空档」与
    // 「服务端处理」）。只在单线程驱动模式下有意义。
    std::atomic<std::uint64_t> server_enter_us{0};
    std::atomic<bool> server_thread_stop{false};
    std::atomic<std::uint32_t> reliable_received{0};
    std::atomic<std::uint32_t> unreliable_received{0};
    std::uint32_t expected_stream_seq = 0;
    std::uint32_t last_datagram_seq = 0;
    bool stream_ordered = true;
    bool datagram_ordered = true;

    server.on_stream = [&](std::uint8_t channel, const std::uint8_t* data, std::size_t bytes) {
        if (channel != 0 || bytes < 8) {
            stream_ordered = false;
            return;
        }
        const std::uint32_t seq = ReadU32(data);
        if (seq != expected_stream_seq) {
            stream_ordered = false;
        }
        ++expected_stream_seq;
        const std::uint64_t delivered_us = NowUs();
        // keep_samples=false（>200 万条，走直方图）时没有逐条发送时刻，
        // 延迟与分段无从计算：直接不记，输出里 n=0，不要打印假 0 分位。
        if (keep_samples && seq >= options.warmup) {
            const std::uint64_t started_us = keep_samples ? send_us[seq] : delivered_us;
            const std::uint64_t queued_us = keep_samples ? enqueue_us[seq] : delivered_us;
            const std::uint64_t total_us = delivered_us >= started_us ? delivered_us - started_us : 0;
            latency_us.Record(total_us);
            if (keep_samples && !latency_seen[seq]) {
                latency_seen[seq] = true;
                latency_ms[seq] = total_us / 1000;  // 旧毫秒口径（截断）
            }
            stage_enqueue_us.Record(queued_us >= started_us ? queued_us - started_us : 0);
            if (options.drive != Drive::kThread) {
                const std::uint64_t enter_us = server_enter_us.load(std::memory_order_relaxed);
                const std::uint64_t gap_us = enter_us >= queued_us ? enter_us - queued_us : 0;
                const std::uint64_t proc_us = delivered_us >= enter_us ? delivered_us - enter_us : 0;
                stage_gap_us.Record(gap_us);
                stage_proc_us.Record(proc_us);
            }
        }
        reliable_received.fetch_add(1, std::memory_order_relaxed);
    };
    server.on_datagram = [&](std::uint8_t channel, const std::uint8_t* data, std::size_t bytes) {
        if (channel != 1 || bytes < 8) {
            datagram_ordered = false;
            return;
        }
        const std::uint32_t seq = ReadU32(data);
        if (last_datagram_seq != 0 && !SeqGreater(seq, last_datagram_seq)) {
            datagram_ordered = false;
        }
        last_datagram_seq = seq;
        unreliable_received.fetch_add(1, std::memory_order_relaxed);
    };

    PinCurrentThread(options.pin_client);
    client.Connect();
    const std::uint64_t handshake_deadline = NowMs() + 15000;
    while (NowMs() < handshake_deadline) {
        client.RunOnce(1);
        server.RunOnce(1);
        if (client_connected && server_connected) {
            break;
        }
    }
    if (!client_connected || !server_connected) {
        std::fprintf(stderr,
                     "netem_probe: handshake timeout (client=%d server=%d tx=%llu rx=%llu)\n",
                     client_connected ? 1 : 0, server_connected ? 1 : 0,
                     static_cast<unsigned long long>(client.stats().frames_sent),
                     static_cast<unsigned long long>(server.stats().frames_received));
        return 1;
    }

    // thread 模式：服务端独占一个接收线程（生产用法），主线程只跑客户端。
    std::thread server_thread;
    if (options.drive == Drive::kThread) {
        server_thread = std::thread([&server, &server_thread_stop, &options]() {
            PinCurrentThread(options.pin_server);
            while (!server_thread_stop.load(std::memory_order_relaxed)) {
                server.RunOnce(1);
            }
        });
    }

    std::vector<std::uint8_t> payload(options.payload_bytes, 0);
    std::uint32_t sent_messages = 0;
    const std::uint64_t send_start_us = NowUs();
    const std::uint64_t send_deadline_ms = NowMs() + static_cast<std::uint64_t>(options.timeout_ms);
    bool send_stalled = false;
    for (std::uint32_t i = 0; i < kMessages; ++i) {
        WriteU32(payload.data(), i);
        WriteU32(payload.data() + 4, i * 2654435761U);
        for (std::size_t j = 8; j < payload.size(); ++j) {
            payload[j] = static_cast<std::uint8_t>((i * 31U + j) & 0xFFU);
        }
        const std::uint64_t before_us = NowUs();
        if (keep_samples) {
            send_us[i] = before_us;
            send_ms[i] = before_us / 1000;
        }
        if (options.flood) {
            // 吞吐模式：窗口满时抽水后重试同一条消息，直到成功。
            std::uint32_t spins = 0;
            while (!client.SendStream(0, payload.data(), payload.size())) {
                client.RunOnce(0);
                if (options.drive == Drive::kPump0) {
                    server_enter_us.store(NowUs(), std::memory_order_relaxed);
                    server.RunOnce(0);
                }
                if (++spins >= 100000U || NowMs() > send_deadline_ms) {
                    send_stalled = true;
                    break;
                }
            }
            if (send_stalled) {
                std::fprintf(stderr, "netem_probe: flood send stalled at %u\n", i);
                break;
            }
        } else if (!client.SendStream(0, payload.data(), payload.size())) {
            std::fprintf(stderr, "netem_probe: SendStream failed at %u\n", i);
            return 1;
        }
        if (keep_samples) {
            enqueue_us[i] = NowUs();
        }
        // 不可靠流故意不做重发；发送失败（netem 队列满）与丢包都计入丢失。
        if (options.send_datagram) {
            (void)client.SendDatagram(1, payload.data(), payload.size());
        }
        ++sent_messages;
        if (options.flood) {
            // 吞吐模式：每 64 条只在不阻塞的前提下抽水。
            if ((i & 0x3FU) == 0x3FU) {
                client.RunOnce(0);
                if (options.drive == Drive::kPump0) {
                    server_enter_us.store(NowUs(), std::memory_order_relaxed);
                    server.RunOnce(0);
                }
            }
        } else if ((i % 5U) == 4U) {
            const std::uint64_t pump_deadline_us =
                NowUs() + static_cast<std::uint64_t>(options.send_pump_ms) * 1000ULL;
            while (NowUs() < pump_deadline_us) {
                if (options.drive == Drive::kPump1) {
                    client.RunOnce(1);
                    server_enter_us.store(NowUs(), std::memory_order_relaxed);
                    server.RunOnce(1);
                } else if (options.drive == Drive::kPump0) {
                    client.RunOnce(0);
                    server_enter_us.store(NowUs(), std::memory_order_relaxed);
                    server.RunOnce(0);
                } else {  // kThread 用阻塞 poll 让出 CPU：服务端在另一个线程，
                    // 客户端自旋会把它挤下 CPU，反而制造 ms 级醒不过来。
                    client.RunOnce(1);
                }
            }
        }
    }
    const std::uint64_t send_done_us = NowUs();

    const std::uint64_t receive_deadline = NowMs() + static_cast<std::uint64_t>(options.timeout_ms);
    std::uint64_t next_report_ms = NowMs() + 1000;
    // 吞吐模式下 +64 次自旋无新交付即视为收尾（避免 1e7 帧长时间空转）。
    std::uint32_t idle_spins = 0;
    std::uint32_t last_delivered = 0;
    while (NowMs() < receive_deadline &&
           reliable_received.load(std::memory_order_relaxed) < sent_messages) {
        if (options.drive == Drive::kPump1) {
            client.RunOnce(1);
            server_enter_us.store(NowUs(), std::memory_order_relaxed);
            server.RunOnce(1);
        } else {
            // thread 模式也阻塞等待：单核时自旋会把服务端线程饿死。
            client.RunOnce(options.drive == Drive::kThread ? 1 : 0);
            if (options.drive == Drive::kPump0) {
                server_enter_us.store(NowUs(), std::memory_order_relaxed);
                server.RunOnce(0);
            }
        }
        if (options.flood) {
            const std::uint32_t now_delivered = reliable_received.load(std::memory_order_relaxed);
            idle_spins = (now_delivered == last_delivered) ? idle_spins + 1 : 0;
            last_delivered = now_delivered;
            if (idle_spins > 20000U) {
                break;
            }
        }
        if (options.verbose && NowMs() >= next_report_ms) {
            next_report_ms += 1000;
            const ReliableChannel::Stats client_now = client.reliable_stats(0);
            const ReliableChannel::Stats server_now = server.reliable_stats(0);
            std::fprintf(stderr,
                         "[progress] t=%llu client_inflight=%zu unsent=%zu acks=%llu "
                         "retrans=%llu srtt=%u rttvar=%u pto=%u | server_delivered=%llu "
                         "server_inflight=%zu\n",
                         static_cast<unsigned long long>(NowMs()),
                         client_now.inflight, client_now.unsent,
                         static_cast<unsigned long long>(client_now.acks_applied),
                         static_cast<unsigned long long>(client_now.retransmits),
                         client_now.srtt_ms, client_now.rttvar_ms, client_now.pto_ms,
                         static_cast<unsigned long long>(server_now.messages_delivered),
                         server_now.inflight);
        }
    }
    const std::uint64_t drain_done_us = NowUs();
    // 多跑 500ms 收尾：让不可靠流的最后一批包与 ACK 到达。
    if (options.drive == Drive::kThread) {
        const std::uint64_t tail_deadline = NowMs() + 500;
        while (NowMs() < tail_deadline) {
            client.RunOnce(0);
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        server_thread_stop.store(true, std::memory_order_relaxed);
        server_thread.join();
    } else {
        const std::uint64_t tail_deadline_us = NowUs() + 500000ULL;
        while (NowUs() < tail_deadline_us) {
            client.RunOnce(0);
            server_enter_us.store(NowUs(), std::memory_order_relaxed);
            server.RunOnce(0);
        }
    }

    std::vector<std::uint64_t> observed;
    observed.reserve(keep_samples ? kMessages : 0);
    for (std::uint32_t i = 0; i < kMessages && keep_samples; ++i) {
        if (latency_seen[i]) {
            observed.push_back(latency_ms[i]);
        }
    }
    latency_us.Finalize();
    stage_gap_us.Finalize();
    stage_proc_us.Finalize();
    stage_enqueue_us.Finalize();
    const auto client_stats = client.reliable_stats(0);
    const auto congestion = client.congestion_stats(0);
    const auto unreliable_stats = server.unreliable_stats(1);
    const std::uint32_t delivered_total = reliable_received.load(std::memory_order_relaxed);
    const std::uint32_t unreliable_total = unreliable_received.load(std::memory_order_relaxed);
    const std::uint64_t p50_ms = keep_samples ? PercentileMs(observed, 0.50)
                                              : latency_us.Percentile(0.50) / 1000;
    const std::uint64_t p95_ms = keep_samples ? PercentileMs(observed, 0.95)
                                              : latency_us.Percentile(0.95) / 1000;
    const std::uint64_t p99_ms = keep_samples ? PercentileMs(observed, 0.99)
                                              : latency_us.Percentile(0.99) / 1000;

    std::printf(
        "netem_probe reliable: delivered=%u/%u ordered=%d retransmits=%llu "
        "fast_retransmits=%llu acks=%llu srtt_ms=%u rttvar_ms=%u pto_ms=%u "
        "rtt_ms=%u p50_ms=%llu p95_ms=%llu p99_ms=%llu pto_mode=%s\n",
        delivered_total, kMessages, stream_ordered ? 1 : 0,
        static_cast<unsigned long long>(client_stats.retransmits),
        static_cast<unsigned long long>(client_stats.fast_retransmits),
        static_cast<unsigned long long>(client_stats.acks_applied), client_stats.srtt_ms,
        client_stats.rttvar_ms, client_stats.pto_ms, client_stats.last_rtt_ms,
        static_cast<unsigned long long>(p50_ms), static_cast<unsigned long long>(p95_ms),
        static_cast<unsigned long long>(p99_ms),
        options.fixed_pto ? "fixed" : "dynamic");
    // 微秒口径（主指标）：把「毫秒量化 + 驱动空档」与真实数据面耗时拆开。
    std::printf(
        "netem_probe latency_us: drive=%s n=%zu p50=%llu p90=%llu p95=%llu p99=%llu "
        "max=%llu mean=%llu\n",
        DriveName(options.drive), latency_us.count(),
        static_cast<unsigned long long>(latency_us.Percentile(0.50)),
        static_cast<unsigned long long>(latency_us.Percentile(0.90)),
        static_cast<unsigned long long>(latency_us.Percentile(0.95)),
        static_cast<unsigned long long>(latency_us.Percentile(0.99)),
        static_cast<unsigned long long>(latency_us.Max()),
        static_cast<unsigned long long>(latency_us.Mean()));
    std::printf(
        "netem_probe stage_us: enqueue_p50=%llu enqueue_p95=%llu gap_p50=%llu gap_p95=%llu "
        "proc_p50=%llu proc_p95=%llu\n",
        static_cast<unsigned long long>(stage_enqueue_us.Percentile(0.50)),
        static_cast<unsigned long long>(stage_enqueue_us.Percentile(0.95)),
        static_cast<unsigned long long>(stage_gap_us.Percentile(0.50)),
        static_cast<unsigned long long>(stage_gap_us.Percentile(0.95)),
        static_cast<unsigned long long>(stage_proc_us.Percentile(0.50)),
        static_cast<unsigned long long>(stage_proc_us.Percentile(0.95)));
    std::printf(
        "netem_probe throughput: sent=%u delivered=%u ordered=%d send_ms=%llu drain_ms=%llu "
        "wall_ms=%llu frames_per_sec=%.1f\n",
        sent_messages, delivered_total, stream_ordered ? 1 : 0,
        static_cast<unsigned long long>((send_done_us - send_start_us) / 1000ULL),
        static_cast<unsigned long long>((drain_done_us - send_done_us) / 1000ULL),
        static_cast<unsigned long long>((drain_done_us - send_start_us) / 1000ULL),
        (drain_done_us > send_start_us)
            ? static_cast<double>(delivered_total) * 1000000.0 /
                  static_cast<double>(drain_done_us - send_start_us)
            : 0.0);
    std::printf(
        "netem_probe congestion: cwnd=%u ssthresh=%u effective_cwnd=%u paced=%d "
        "pacing_bps=%llu deferrals=%llu loss_events=%llu timeout_loss=%llu "
        "random_loss=%llu congestion_loss=%llu min_rtt_ms=%u loss_permille=%u "
        "bytes_sent=%llu\n",
        congestion.cwnd_packets, congestion.ssthresh_packets, congestion.effective_window,
        congestion.pacing_enabled ? 1 : 0,
        static_cast<unsigned long long>(congestion.pacing_rate_bps),
        static_cast<unsigned long long>(congestion.pacing_deferrals),
        static_cast<unsigned long long>(congestion.loss_events),
        static_cast<unsigned long long>(congestion.timeout_events),
        static_cast<unsigned long long>(congestion.random_loss_events),
        static_cast<unsigned long long>(congestion.congestion_loss_events),
        congestion.min_rtt_ms, congestion.loss_rate_permille,
        static_cast<unsigned long long>(congestion.bytes_sent));
    std::printf(
        "netem_probe unreliable: delivered=%u/%u ordered=%d stale_dropped=%llu "
        "reorder_buffered=%llu reorder_released=%llu reorder_skipped=%llu "
        "reorder_stale=%llu reorder_pending=%zu\n",
        unreliable_total, sent_messages, datagram_ordered ? 1 : 0,
        static_cast<unsigned long long>(unreliable_stats.stale_dropped),
        static_cast<unsigned long long>(unreliable_stats.reorder_buffered),
        static_cast<unsigned long long>(unreliable_stats.reorder_released),
        static_cast<unsigned long long>(unreliable_stats.reorder_skipped),
        static_cast<unsigned long long>(unreliable_stats.reorder_stale),
        unreliable_stats.reorder_pending);
    std::printf(
        "netem_probe link: frames_sent=%llu frames_received=%llu handshakes=%llu "
        "heartbeats=%llu timeouts=%llu sack_ext=%d/%d sack_acks=%llu "
        "sack_blocks=%llu pto_retrans=%llu tlp=%llu sack_acked=%llu sack_covered=%llu\n",
        static_cast<unsigned long long>(client.stats().frames_sent + server.stats().frames_sent),
        static_cast<unsigned long long>(client.stats().frames_received +
                                        server.stats().frames_received),
        static_cast<unsigned long long>(client.stats().handshakes + server.stats().handshakes),
        static_cast<unsigned long long>(client.stats().heartbeats_sent +
                                        server.stats().heartbeats_sent),
        static_cast<unsigned long long>(client.stats().timeouts + server.stats().timeouts),
        client.sack_ext_enabled() ? 1 : 0, server.sack_ext_enabled() ? 1 : 0,
        static_cast<unsigned long long>(client.stats().sack_acks_sent +
                                        server.stats().sack_acks_sent),
        static_cast<unsigned long long>(client.stats().sack_blocks_sent +
                                        server.stats().sack_blocks_sent),
        static_cast<unsigned long long>(client_stats.pto_retransmits),
        static_cast<unsigned long long>(client_stats.tlp_probes),
        static_cast<unsigned long long>(client_stats.sack_acks_with_blocks),
        static_cast<unsigned long long>(client_stats.sack_seqs_covered));
    if (proxy) {
        proxy->Stop();
        std::printf("netem_probe proxy: loss=%d%% delay=%dms forwarded=%llu dropped=%llu\n",
                    options.loss_percent, options.delay_ms,
                    static_cast<unsigned long long>(proxy->forwarded()),
                    static_cast<unsigned long long>(proxy->dropped()));
    }

    const bool reliable_ok = delivered_total == kMessages && stream_ordered &&
                             client.stats().timeouts == 0 && server.stats().timeouts == 0;
    const bool unreliable_ok =
        datagram_ordered && (!options.send_datagram || unreliable_total > 0);
    const bool link_ok = client.state() == LinkState::kConnected &&
                         server.state() == LinkState::kConnected && client.stats().timeouts == 0 &&
                         server.stats().timeouts == 0;
    if (!reliable_ok || !unreliable_ok || !link_ok) {
        std::fprintf(stderr,
                     "netem_probe: FAIL reliable_ok=%d unreliable_ok=%d link_ok=%d "
                     "client_state=%s server_state=%s\n",
                     reliable_ok ? 1 : 0, unreliable_ok ? 1 : 0, link_ok ? 1 : 0,
                     LinkStateName(client.state()), LinkStateName(server.state()));
        client.Close();
        server.Close();
        return 1;
    }
    std::printf("netem_probe: PASS\n");
    client.Close();
    server.Close();
    return 0;
}
