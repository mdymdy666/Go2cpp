/**
 * @file udp_probe.cpp
 * @brief UDP 对照探针：与 tcp_probe / bento_netem_probe 同载荷（200×256B）。
 * @details 单进程内起一个 recvfrom 线程作为服务端、主线程作为客户端，
 *          向 127.0.0.1:<port> 逐条 sendto。UDP 无重传、无拥塞控制，
 *          送达率与 p95 直接反映链路丢包/排队延迟；输出字段与 tcp_probe
 *          完全一致，便于 compare_transports.sh 汇总成同一张表。
 *
 *          收尾窗口：客户端发完后设置 send_done，服务端在收满 200 条后
 *          立即退出；否则在 send_done 后 idle_ms（默认 1000ms）无新包时
 *          退出。timeout_ms 是服务端总等待上限，防止异常挂死。
 *
 *          退出码：0 探针正常完成（不因丢包失败）；1 socket/IO 错误；2 参数错误。
 * @author go2cpp contributors
 * @date 2026-10-28
 * @copyright MIT License
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr std::size_t kMessageBytes = 256;
constexpr std::uint32_t kMagic = 0xB3A7C0DEU;
constexpr std::uint32_t kDefaultSeed = 0x9E3779B9U;

struct Options {
    std::uint16_t port = 34571;
    std::uint32_t messages = 200;
    int timeout_ms = 60000;
    int idle_ms = 1000;
    std::uint32_t seed = kDefaultSeed;
    std::string label = "udp";
    int run = 1;
    std::string csv_path;
};

std::uint64_t NowNs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

void WriteU32(std::uint8_t* out, std::uint32_t value) {
    out[0] = static_cast<std::uint8_t>(value >> 24);
    out[1] = static_cast<std::uint8_t>(value >> 16);
    out[2] = static_cast<std::uint8_t>(value >> 8);
    out[3] = static_cast<std::uint8_t>(value);
}

std::uint32_t ReadU32(const std::uint8_t* in) {
    return (static_cast<std::uint32_t>(in[0]) << 24) |
           (static_cast<std::uint32_t>(in[1]) << 16) |
           (static_cast<std::uint32_t>(in[2]) << 8) |
           static_cast<std::uint32_t>(in[3]);
}

void WriteU64(std::uint8_t* out, std::uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        out[i] = static_cast<std::uint8_t>(value >> (56 - 8 * i));
    }
}

std::uint64_t ReadU64(const std::uint8_t* in) {
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value = (value << 8) | static_cast<std::uint64_t>(in[i]);
    }
    return value;
}

void BuildPayload(std::uint8_t* payload, std::uint32_t seq, std::uint64_t send_ns) {
    WriteU32(payload, seq);
    WriteU32(payload + 4, kMagic);
    for (std::size_t i = 8; i < kMessageBytes - 8; ++i) {
        payload[i] = static_cast<std::uint8_t>((seq * 31U + i) & 0xFFU);
    }
    WriteU64(payload + kMessageBytes - 8, send_ns);
}

bool ParseOptions(int argc, char** argv, Options* options) {
    for (int i = 1; i < argc; ++i) {
        const char* argument = argv[i];
        const bool has_value = (i + 1 < argc);
        if (std::strcmp(argument, "--port") == 0 && has_value) {
            options->port = static_cast<std::uint16_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (std::strcmp(argument, "--messages") == 0 && has_value) {
            options->messages = static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (std::strcmp(argument, "--timeout-ms") == 0 && has_value) {
            options->timeout_ms = std::atoi(argv[++i]);
        } else if (std::strcmp(argument, "--idle-ms") == 0 && has_value) {
            options->idle_ms = std::atoi(argv[++i]);
        } else if (std::strcmp(argument, "--seed") == 0 && has_value) {
            options->seed = static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (std::strcmp(argument, "--label") == 0 && has_value) {
            options->label = argv[++i];
        } else if (std::strcmp(argument, "--run") == 0 && has_value) {
            options->run = std::atoi(argv[++i]);
        } else if (std::strcmp(argument, "--csv") == 0 && has_value) {
            options->csv_path = argv[++i];
        } else {
            std::fprintf(stderr, "udp_probe: unknown or incomplete argument: %s\n", argument);
            return false;
        }
    }
    if (options->port == 0 || options->messages == 0 || options->timeout_ms <= 0 ||
        options->idle_ms < 0 || options->run <= 0 || options->label.empty()) {
        std::fprintf(stderr, "udp_probe: invalid argument range\n");
        return false;
    }
    return true;
}

std::uint64_t PercentileUs(std::vector<std::uint64_t> samples, double ratio) {
    if (samples.empty()) {
        return 0;
    }
    std::sort(samples.begin(), samples.end());
    const double index = ratio * static_cast<double>(samples.size() - 1);
    const std::size_t rounded = static_cast<std::size_t>(index + 0.5);
    return samples[std::min(rounded, samples.size() - 1)];
}

struct SharedState {
    std::vector<std::uint8_t> seen;
    std::vector<std::uint64_t> recv_ns;
    std::vector<std::uint64_t> send_ns;
    std::uint32_t delivered = 0;
    std::uint32_t duplicates = 0;
    std::uint32_t corrupt = 0;
    std::uint32_t last_seq = 0;
    bool has_last = false;
    bool ordered = true;
    int server_error = 0;

    void Reset(std::uint32_t messages) {
        seen.assign(messages, 0);
        recv_ns.assign(messages, 0);
        send_ns.assign(messages, 0);
        delivered = 0;
        duplicates = 0;
        corrupt = 0;
        last_seq = 0;
        has_last = false;
        ordered = true;
        server_error = 0;
    }
};

int CreateServerSocket(std::uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        std::fprintf(stderr, "udp_probe: server socket failed: %s\n", std::strerror(errno));
        return -1;
    }
    int reuse = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    int receive_buffer = 4 * 1024 * 1024;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &receive_buffer, sizeof(receive_buffer));
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        std::fprintf(stderr, "udp_probe: bind :%u failed: %s\n", port, std::strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

void ConsumeDatagram(const std::uint8_t* data, std::size_t bytes, const Options& options,
                     SharedState* state) {
    if (bytes != kMessageBytes) {
        ++state->corrupt;
        return;
    }
    const std::uint32_t seq = ReadU32(data);
    const std::uint32_t magic = ReadU32(data + 4);
    if (magic != kMagic || seq >= options.messages) {
        ++state->corrupt;
        return;
    }
    if (state->has_last && !(seq > state->last_seq)) {
        // 允许丢包造成的 gap，但不允许序号回退：回退说明发生了乱序。
        state->ordered = false;
    }
    state->has_last = true;
    state->last_seq = seq;
    if (state->seen[seq] != 0) {
        ++state->duplicates;
        return;
    }
    state->seen[seq] = 1;
    state->recv_ns[seq] = NowNs();
    state->send_ns[seq] = ReadU64(data + kMessageBytes - 8);
    ++state->delivered;
}

void ServerLoop(int fd, const Options& options, SharedState* state, std::atomic<bool>* stop,
                std::atomic<std::uint64_t>* send_done_ns) {
    std::uint8_t buffer[2048];
    const std::uint64_t deadline =
        NowNs() + static_cast<std::uint64_t>(options.timeout_ms) * 1000000ULL;
    std::uint64_t last_data_ns = 0;
    while (!stop->load() && NowNs() < deadline) {
        pollfd entry {};
        entry.fd = fd;
        entry.events = POLLIN;
        const int ready = poll(&entry, 1, 100);
        if (ready > 0 && (entry.revents & POLLIN)) {
            const ssize_t bytes = recvfrom(fd, buffer, sizeof(buffer), 0, nullptr, nullptr);
            if (bytes > 0) {
                last_data_ns = NowNs();
                ConsumeDatagram(buffer, static_cast<std::size_t>(bytes), options, state);
                if (state->delivered >= options.messages) {
                    break;
                }
                continue;
            }
            if (bytes < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                state->server_error = errno;
                break;
            }
        }
        const std::uint64_t sent_at = send_done_ns->load(std::memory_order_acquire);
        if (sent_at != 0) {
            const std::uint64_t quiet_from = std::max(sent_at, last_data_ns);
            if (NowNs() >= quiet_from + static_cast<std::uint64_t>(options.idle_ms) * 1000000ULL) {
                break;
            }
        }
    }
}

int CreateClientSocket() {
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        std::fprintf(stderr, "udp_probe: client socket failed: %s\n", std::strerror(errno));
        return -1;
    }
    int send_buffer = 4 * 1024 * 1024;
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &send_buffer, sizeof(send_buffer));
    return fd;
}

void WriteCsv(const Options& options, const SharedState& state) {
    if (options.csv_path.empty()) {
        return;
    }
    FILE* file = std::fopen(options.csv_path.c_str(), "w");
    if (file == nullptr) {
        std::fprintf(stderr, "udp_probe: cannot open csv %s: %s\n", options.csv_path.c_str(),
                     std::strerror(errno));
        return;
    }
    std::fprintf(file, "seq,send_ns,recv_ns,latency_us,delivered\n");
    for (std::uint32_t i = 0; i < options.messages; ++i) {
        const bool arrived = (state.recv_ns[i] != 0 && state.send_ns[i] != 0);
        const double latency_us =
            arrived ? static_cast<double>(state.recv_ns[i] - state.send_ns[i]) / 1000.0 : 0.0;
        std::fprintf(file, "%u,%llu,%llu,%.3f,%d\n", i,
                     static_cast<unsigned long long>(state.send_ns[i]),
                     static_cast<unsigned long long>(state.recv_ns[i]), latency_us,
                     arrived ? 1 : 0);
    }
    std::fclose(file);
}

void Report(const Options& options, const SharedState& state, std::uint64_t first_send_ns,
            std::uint64_t send_done_ns) {
    std::vector<std::uint64_t> latencies_us;
    latencies_us.reserve(state.delivered);
    std::uint64_t last_recv_ns = 0;
    for (std::uint32_t i = 0; i < options.messages; ++i) {
        if (state.recv_ns[i] != 0 && state.send_ns[i] != 0) {
            latencies_us.push_back((state.recv_ns[i] - state.send_ns[i]) / 1000ULL);
            last_recv_ns = std::max(last_recv_ns, state.recv_ns[i]);
        }
    }
    const std::uint64_t p50_us = PercentileUs(latencies_us, 0.50);
    const std::uint64_t p95_us = PercentileUs(latencies_us, 0.95);
    const std::uint64_t start_ns = (first_send_ns != 0) ? first_send_ns : send_done_ns;
    const std::uint64_t end_ns = (last_recv_ns != 0) ? last_recv_ns : send_done_ns;
    const double duration_ms =
        (end_ns > start_ns) ? static_cast<double>(end_ns - start_ns) / 1000000.0 : 0.0;
    const double throughput_mbps =
        (duration_ms > 0.0)
            ? (static_cast<double>(state.delivered) * kMessageBytes * 8.0) /
                  (duration_ms / 1000.0) / 1000000.0
            : 0.0;

    std::printf(
        "RESULT transport=udp scenario=%s run=%d sent=%u delivered=%u ordered=%d "
        "duplicates=%u corrupt=%u retrans=%d client_retrans=%d server_retrans=%d "
        "p50_us=%llu p95_us=%llu p50_ms=%.3f p95_ms=%.3f duration_ms=%.3f "
        "throughput_mbps=%.6f seed=%u\n",
        options.label.c_str(), options.run, options.messages, state.delivered,
        state.ordered ? 1 : 0, state.duplicates, state.corrupt, 0, 0, 0,
        static_cast<unsigned long long>(p50_us), static_cast<unsigned long long>(p95_us),
        static_cast<double>(p50_us) / 1000.0, static_cast<double>(p95_us) / 1000.0, duration_ms,
        throughput_mbps, options.seed);
    if (state.server_error != 0) {
        std::fprintf(stderr, "udp_probe: server recv error: %s\n", std::strerror(state.server_error));
    }
    WriteCsv(options, state);
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    if (!ParseOptions(argc, argv, &options)) {
        return 2;
    }

    const int server_fd = CreateServerSocket(options.port);
    if (server_fd < 0) {
        return 1;
    }
    SharedState state;
    state.Reset(options.messages);
    std::atomic<bool> stop {false};
    std::atomic<std::uint64_t> send_done_ns {0};
    std::thread server(ServerLoop, server_fd, std::cref(options), &state, &stop, &send_done_ns);

    const int client_fd = CreateClientSocket();
    if (client_fd < 0) {
        stop.store(true);
        server.join();
        close(server_fd);
        return 1;
    }
    sockaddr_in destination {};
    destination.sin_family = AF_INET;
    destination.sin_port = htons(options.port);
    destination.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    std::vector<std::uint8_t> payload(kMessageBytes, 0);
    std::uint64_t first_send_ns = 0;
    int send_error = 0;
    std::uint32_t sent = 0;
    for (std::uint32_t i = 0; i < options.messages; ++i) {
        const std::uint64_t send_ns = NowNs();
        BuildPayload(payload.data(), i, send_ns);
        const ssize_t written =
            sendto(client_fd, payload.data(), payload.size(), 0,
                   reinterpret_cast<const sockaddr*>(&destination), sizeof(destination));
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            send_error = errno;
            break;
        }
        ++sent;
        if (first_send_ns == 0) {
            first_send_ns = send_ns;
        }
    }
    const std::uint64_t send_done = NowNs();
    send_done_ns.store(send_done, std::memory_order_release);
    if (send_error != 0) {
        std::fprintf(stderr, "udp_probe: sendto failed after %u messages: %s\n", sent,
                     std::strerror(send_error));
    }
    server.join();
    close(client_fd);
    close(server_fd);

    Report(options, state, first_send_ns, send_done);
    if (send_error != 0 || state.corrupt != 0) {
        return 1;
    }
    return 0;
}
