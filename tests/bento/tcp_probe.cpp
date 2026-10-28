/**
 * @file tcp_probe.cpp
 * @brief TCP 对照探针：固定 200 条 × 256B 消息，记录逐条发送/接收时间戳。
 * @details 单进程内起一个 accept 线程作为服务端、主线程作为客户端，连接
 *          127.0.0.1:<port>。消息载荷与 Bento/UDP 探针保持同源布局：
 *          [0..3] 序号、[4..7] 0xB3A7C0DE 魔数、[8..247] 固定 pattern、
 *          [248..255] 发送时刻（steady_clock 纳秒）。TCP 是字节流，服务端
 *          按 256B 边界重组；若字节流失序由内核保证，重组结果必须严格递增。
 *
 *          重传统计来自 getsockopt(TCP_INFO).tcpi_total_retrans，分别记录
 *          客户端（数据方向）与服务端（ACK 方向）的累计重传次数。
 *
 *          退出码：0 探针正常完成（不因丢包失败）；1 socket/连接错误；2 参数错误。
 * @author go2cpp contributors
 * @date 2026-10-28
 * @copyright MIT License
 */

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
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
    std::uint16_t port = 34570;
    std::uint32_t messages = 200;
    int timeout_ms = 60000;
    std::uint32_t seed = kDefaultSeed;
    std::string label = "tcp";
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
           (static_cast<std::uint32_t>(in[2]) << 8) | static_cast<std::uint32_t>(in[3]);
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
    // 前 8 字节与 Bento 探针一致；8..247 是固定 pattern；尾部 8B 是本探针
    // 独有的发送时间戳，接收端据此计算单向时延，不依赖跨机时钟同步。
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
        } else if (std::strcmp(argument, "--seed") == 0 && has_value) {
            options->seed = static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (std::strcmp(argument, "--label") == 0 && has_value) {
            options->label = argv[++i];
        } else if (std::strcmp(argument, "--run") == 0 && has_value) {
            options->run = std::atoi(argv[++i]);
        } else if (std::strcmp(argument, "--csv") == 0 && has_value) {
            options->csv_path = argv[++i];
        } else {
            std::fprintf(stderr, "tcp_probe: unknown or incomplete argument: %s\n", argument);
            return false;
        }
    }
    if (options->port == 0 || options->messages == 0 || options->timeout_ms <= 0 ||
        options->run <= 0 || options->label.empty()) {
        std::fprintf(stderr, "tcp_probe: invalid argument range\n");
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

int ReadTotalRetrans(int fd) {
    struct tcp_info info {};
    socklen_t length = sizeof(info);
    if (getsockopt(fd, IPPROTO_TCP, TCP_INFO, &info, &length) != 0) {
        return -1;
    }
    return static_cast<int>(info.tcpi_total_retrans);
}

int CreateListener(std::uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        std::fprintf(stderr, "tcp_probe: socket failed: %s\n", std::strerror(errno));
        return -1;
    }
    int reuse = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        std::fprintf(stderr, "tcp_probe: bind :%u failed: %s\n", port, std::strerror(errno));
        close(fd);
        return -1;
    }
    if (listen(fd, 1) != 0) {
        std::fprintf(stderr, "tcp_probe: listen failed: %s\n", std::strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

struct SharedState {
    std::vector<std::uint8_t> seen;
    std::vector<std::uint64_t> recv_ns;
    std::vector<std::uint64_t> send_ns;
    std::uint32_t delivered = 0;
    std::uint32_t duplicates = 0;
    std::uint32_t corrupt = 0;
    std::uint32_t next_expected = 0;
    bool ordered = true;
    int server_retrans = -1;
    int server_error = 0;

    void Reset(std::uint32_t messages) {
        seen.assign(messages, 0);
        recv_ns.assign(messages, 0);
        send_ns.assign(messages, 0);
        delivered = 0;
        duplicates = 0;
        corrupt = 0;
        next_expected = 0;
        ordered = true;
        server_retrans = -1;
        server_error = 0;
    }
};

void ConsumeStreamBuffer(std::uint8_t* buffer, std::size_t* used, const Options& options,
                         SharedState* state) {
    while (*used >= kMessageBytes) {
        const std::uint32_t seq = ReadU32(buffer);
        const std::uint32_t magic = ReadU32(buffer + 4);
        const std::uint64_t send_ns = ReadU64(buffer + kMessageBytes - 8);
        if (magic != kMagic || seq >= options.messages) {
            ++state->corrupt;
        } else if (!state->seen[seq]) {
            state->seen[seq] = 1;
            state->recv_ns[seq] = NowNs();
            state->send_ns[seq] = send_ns;
            if (seq != state->next_expected) {
                state->ordered = false;
            }
            state->next_expected = seq + 1;
            ++state->delivered;
        } else {
            ++state->duplicates;
        }
        std::memmove(buffer, buffer + kMessageBytes, *used - kMessageBytes);
        *used -= kMessageBytes;
    }
}

void ServerLoop(int listen_fd, const Options& options, SharedState* state,
                std::atomic<bool>* stop) {
    int connection = -1;
    const std::uint64_t accept_deadline =
        NowNs() + static_cast<std::uint64_t>(options.timeout_ms) * 1000000ULL;
    while (!stop->load() && NowNs() < accept_deadline) {
        pollfd entry {};
        entry.fd = listen_fd;
        entry.events = POLLIN;
        const int ready = poll(&entry, 1, 100);
        if (ready > 0 && (entry.revents & (POLLIN | POLLERR))) {
            connection = accept(listen_fd, nullptr, nullptr);
            break;
        }
    }
    if (connection < 0) {
        return;
    }

    const int receive_timeout_ms = 200;
    timeval timeout {};
    timeout.tv_sec = receive_timeout_ms / 1000;
    timeout.tv_usec = (receive_timeout_ms % 1000) * 1000;
    (void)setsockopt(connection, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    std::vector<std::uint8_t> buffer(kMessageBytes * 2);
    std::size_t used = 0;
    const std::uint64_t deadline =
        NowNs() + static_cast<std::uint64_t>(options.timeout_ms) * 1000000ULL;
    while (!stop->load() && NowNs() < deadline) {
        const ssize_t bytes = recv(connection, buffer.data() + used, buffer.size() - used, 0);
        if (bytes > 0) {
            used += static_cast<std::size_t>(bytes);
            ConsumeStreamBuffer(buffer.data(), &used, options, state);
            if (state->delivered >= options.messages) {
                break;
            }
            continue;
        }
        if (bytes == 0) {
            break;  // 客户端 shutdown(SHUT_WR)，字节流已按序结束。
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            continue;  // SO_RCVTIMEO 到期，回到循环检查 deadline/stop。
        }
        state->server_error = errno;
        break;
    }
    state->server_retrans = ReadTotalRetrans(connection);
    close(connection);
}

int ConnectTo(std::uint16_t port, int timeout_ms) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        std::fprintf(stderr, "tcp_probe: client socket failed: %s\n", std::strerror(errno));
        return -1;
    }
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    const int flags = fcntl(fd, F_GETFL, 0);
    (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    int result = connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    if (result != 0 && errno != EINPROGRESS) {
        std::fprintf(stderr, "tcp_probe: connect failed: %s\n", std::strerror(errno));
        close(fd);
        return -1;
    }
    pollfd entry {};
    entry.fd = fd;
    entry.events = POLLOUT;
    const int ready = poll(&entry, 1, timeout_ms);
    if (ready <= 0) {
        std::fprintf(stderr, "tcp_probe: connect timeout after %dms\n", timeout_ms);
        close(fd);
        return -1;
    }
    int socket_error = 0;
    socklen_t error_length = sizeof(socket_error);
    (void)getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_length);
    if (socket_error != 0) {
        std::fprintf(stderr, "tcp_probe: connect error: %s\n", std::strerror(socket_error));
        close(fd);
        return -1;
    }
    (void)fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);

    timeval send_timeout {};
    send_timeout.tv_sec = 5;
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout));
    int no_delay = 1;
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &no_delay, sizeof(no_delay));
    return fd;
}

void WriteCsv(const Options& options, const SharedState& state) {
    if (options.csv_path.empty()) {
        return;
    }
    FILE* file = std::fopen(options.csv_path.c_str(), "w");
    if (file == nullptr) {
        std::fprintf(stderr, "tcp_probe: cannot open csv %s: %s\n", options.csv_path.c_str(),
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
            std::uint64_t last_send_ns, int client_retrans) {
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
    const std::uint64_t start_ns = (first_send_ns != 0) ? first_send_ns : last_send_ns;
    const std::uint64_t end_ns = (last_recv_ns != 0) ? last_recv_ns : last_send_ns;
    const double duration_ms =
        (end_ns > start_ns) ? static_cast<double>(end_ns - start_ns) / 1000000.0 : 0.0;
    const double throughput_mbps =
        (duration_ms > 0.0)
            ? (static_cast<double>(state.delivered) * kMessageBytes * 8.0) /
                  (duration_ms / 1000.0) / 1000000.0
            : 0.0;
    const int client_side_retrans = (client_retrans < 0) ? 0 : client_retrans;
    const int server_side_retrans = (state.server_retrans < 0) ? 0 : state.server_retrans;

    std::printf(
        "RESULT transport=tcp scenario=%s run=%d sent=%u delivered=%u ordered=%d "
        "duplicates=%u corrupt=%u retrans=%d client_retrans=%d server_retrans=%d "
        "p50_us=%llu p95_us=%llu p50_ms=%.3f p95_ms=%.3f duration_ms=%.3f "
        "throughput_mbps=%.6f seed=%u\n",
        options.label.c_str(), options.run, options.messages, state.delivered,
        state.ordered ? 1 : 0, state.duplicates, state.corrupt,
        client_side_retrans + server_side_retrans, client_side_retrans, server_side_retrans,
        static_cast<unsigned long long>(p50_us), static_cast<unsigned long long>(p95_us),
        static_cast<double>(p50_us) / 1000.0, static_cast<double>(p95_us) / 1000.0, duration_ms,
        throughput_mbps, options.seed);
    if (state.server_error != 0) {
        std::fprintf(stderr, "tcp_probe: server recv error: %s\n", std::strerror(state.server_error));
    }
    WriteCsv(options, state);
}

}  // namespace

int main(int argc, char** argv) {
    (void)signal(SIGPIPE, SIG_IGN);
    Options options;
    if (!ParseOptions(argc, argv, &options)) {
        return 2;
    }

    const int listen_fd = CreateListener(options.port);
    if (listen_fd < 0) {
        return 1;
    }
    SharedState state;
    state.Reset(options.messages);
    std::atomic<bool> stop {false};
    std::thread server(ServerLoop, listen_fd, std::cref(options), &state, &stop);

    const int client_fd = ConnectTo(options.port, options.timeout_ms);
    if (client_fd < 0) {
        stop.store(true);
        server.join();
        close(listen_fd);
        return 1;
    }

    std::vector<std::uint8_t> payload(kMessageBytes, 0);
    std::uint64_t first_send_ns = 0;
    std::uint64_t last_send_ns = 0;
    int send_error = 0;
    for (std::uint32_t i = 0; i < options.messages; ++i) {
        const std::uint64_t send_ns = NowNs();
        BuildPayload(payload.data(), i, send_ns);
        std::size_t offset = 0;
        while (offset < payload.size()) {
            const ssize_t written =
                send(client_fd, payload.data() + offset, payload.size() - offset, 0);
            if (written > 0) {
                offset += static_cast<std::size_t>(written);
                continue;
            }
            if (written < 0 && errno == EINTR) {
                continue;
            }
            send_error = (written < 0) ? errno : EIO;
            break;
        }
        if (first_send_ns == 0) {
            first_send_ns = send_ns;
        }
        last_send_ns = send_ns;
        if (send_error != 0) {
            break;
        }
    }
    if (send_error != 0) {
        std::fprintf(stderr, "tcp_probe: send failed at message level: %s\n",
                     std::strerror(send_error));
    }
    (void)shutdown(client_fd, SHUT_WR);
    server.join();

    const int client_retrans = ReadTotalRetrans(client_fd);
    close(client_fd);
    close(listen_fd);

    Report(options, state, first_send_ns, last_send_ns, client_retrans);
    if (send_error != 0 || state.corrupt != 0) {
        return 1;
    }
    return 0;
}
