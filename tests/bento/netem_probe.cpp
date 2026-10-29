/**
 * @file netem_probe.cpp
 * @brief 弱网探针：在 tc netem（loss 20% / delay 200ms）下跑可靠 + 不可靠流。
 * @details 单进程双 Link：服务端监听 --port（默认 0：bind 端口 0 后由
 *          getsockname 取内核分配的随机端口），客户端连接 127.0.0.1。
 *          默认随机端口保证多测试/多 agent 并发时不会抢同一端口。可靠流
 *          必须 100% 按序到达；不可靠流允许丢包，但交付序号必须严格递增
 *          （DropStale 生效）。输出 p50/p95/p99 与重传统计供报告引用。
 *
 *          退出码：0 通过；1 断言失败/超时；2 参数错误。
 * @author go2cpp contributors
 * @date 2026-10-26
 * @copyright MIT License
 */

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include "go2cpp/bento/net/link.hpp"
#include "udp_proxy.hpp"

namespace {

using namespace go2cpp::bento;

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
        options->loss_percent > 100 || options->delay_ms < 0) {
        std::fprintf(stderr, "invalid range: port/messages/payload/loss/delay\n");
        return false;
    }
    return true;
}

void Pump(Link& client, Link& server, int duration_ms) {
    const std::uint64_t deadline = NowMs() + static_cast<std::uint64_t>(duration_ms);
    while (NowMs() < deadline) {
        client.RunOnce(1);
        server.RunOnce(1);
    }
}

std::uint64_t Percentile(std::vector<std::uint64_t> samples, double ratio) {
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

    std::vector<std::uint64_t> send_ms(options.messages, 0);
    std::vector<std::uint64_t> latency_ms(options.messages, 0);
    std::vector<bool> latency_seen(options.messages, false);
    std::uint32_t reliable_received = 0;
    std::uint32_t unreliable_received = 0;
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
        if (seq < options.messages && !latency_seen[seq]) {
            latency_seen[seq] = true;
            latency_ms[seq] = NowMs() - send_ms[seq];
        }
        ++reliable_received;
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
        ++unreliable_received;
    };

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

    std::vector<std::uint8_t> payload(options.payload_bytes, 0);
    for (std::uint32_t i = 0; i < options.messages; ++i) {
        WriteU32(payload.data(), i);
        WriteU32(payload.data() + 4, i * 2654435761U);
        for (std::size_t j = 8; j < payload.size(); ++j) {
            payload[j] = static_cast<std::uint8_t>((i * 31U + j) & 0xFFU);
        }
        send_ms[i] = NowMs();
        if (!client.SendStream(0, payload.data(), payload.size())) {
            std::fprintf(stderr, "netem_probe: SendStream failed at %u\n", i);
            return 1;
        }
        // 不可靠流故意不做重发；发送失败（netem 队列满）与丢包都计入丢失。
        (void)client.SendDatagram(1, payload.data(), payload.size());
        if ((i % 5U) == 4U) {
            Pump(client, server, 3);
        }
    }

    const std::uint64_t receive_deadline = NowMs() + static_cast<std::uint64_t>(options.timeout_ms);
    std::uint64_t next_report_ms = NowMs() + 1000;
    while (NowMs() < receive_deadline && reliable_received < options.messages) {
        client.RunOnce(1);
        server.RunOnce(1);
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
    // 多跑 500ms 收尾：让不可靠流的最后一批包与 ACK 到达。
    Pump(client, server, 500);

    std::vector<std::uint64_t> observed;
    observed.reserve(options.messages);
    for (std::uint32_t i = 0; i < options.messages; ++i) {
        if (latency_seen[i]) {
            observed.push_back(latency_ms[i]);
        }
    }
    const auto client_stats = client.reliable_stats(0);
    const auto congestion = client.congestion_stats(0);
    const auto unreliable_stats = server.unreliable_stats(1);

    std::printf(
        "netem_probe reliable: delivered=%u/%u ordered=%d retransmits=%llu "
        "fast_retransmits=%llu acks=%llu srtt_ms=%u rttvar_ms=%u pto_ms=%u "
        "rtt_ms=%u p50_ms=%llu p95_ms=%llu p99_ms=%llu pto_mode=%s\n",
        reliable_received, options.messages, stream_ordered ? 1 : 0,
        static_cast<unsigned long long>(client_stats.retransmits),
        static_cast<unsigned long long>(client_stats.fast_retransmits),
        static_cast<unsigned long long>(client_stats.acks_applied), client_stats.srtt_ms,
        client_stats.rttvar_ms, client_stats.pto_ms, client_stats.last_rtt_ms,
        static_cast<unsigned long long>(Percentile(observed, 0.50)),
        static_cast<unsigned long long>(Percentile(observed, 0.95)),
        static_cast<unsigned long long>(Percentile(observed, 0.99)),
        options.fixed_pto ? "fixed" : "dynamic");
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
        unreliable_received, options.messages, datagram_ordered ? 1 : 0,
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

    const bool reliable_ok = reliable_received == options.messages && stream_ordered &&
                             client.stats().timeouts == 0 && server.stats().timeouts == 0;
    const bool unreliable_ok = datagram_ordered && unreliable_received > 0;
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
