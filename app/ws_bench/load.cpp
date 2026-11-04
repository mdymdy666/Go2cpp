/**
 * @file load.cpp
 * @brief WebSocket 长连接压测客户端（单线程 epoll，自写 RFC 6455 握手与帧）。
 *
 * @details 目标场景：N 条 idle WebSocket 连接 + 每秒一次 Ping 心跳。客户端
 *          用非阻塞 socket + 单线程 epoll 承载全部连接（20k 连接不占线程），
 *          统计四项指标并以 JSON 输出到 stdout：
 *          - 连接建立成功率（handshake_ok / target）；
 *          - 握手耗时 p50/p99（connect() 起算到收到 101 响应）；
 *          - 心跳 RTT p50/p99/p999（Ping 发出到 Pong 收齐，payload 带序号）；
 *          - 断连数、Ping 超时数、回显校验数。
 *
 * 用法：
 * ```sh
 * ./ws_bench_load --conn 20000 --url ws://127.0.0.1:8080/ws \
 *                 --duration 60 --ping-ms 1000 --label go2cpp
 * ```
 *
 * 阶段划分：先按 128 条/10ms 的节奏建连（ramp），全部连接有结果后进入
 * steady 阶段，steady 阶段持续 `--duration` 秒并在结束时输出 JSON。仅
 * steady 阶段的 Ping 样本计入 RTT 统计，避免建连风暴污染心跳指标。
 */

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <queue>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using Micros = std::uint32_t;

std::uint64_t NowNs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now().time_since_epoch())
            .count());
}

struct Options {
    std::size_t conn{1000};
    std::string url{"ws://127.0.0.1:8080/ws"};
    int duration_s{30};
    int ping_ms{1000};
    std::string label{"unknown"};
    std::size_t batch{16};
    int batch_ms{2};
    int ramp_timeout_s{30};

    std::string host{"127.0.0.1"};
    std::uint16_t port{8080};
    std::string path{"/ws"};
};

bool ParseUrl(const std::string& url, Options& options) {
    const std::string prefix = "ws://";
    if (url.compare(0, prefix.size(), prefix) != 0) {
        std::fprintf(stderr, "only ws:// is supported: %s\n", url.c_str());
        return false;
    }
    std::string rest = url.substr(prefix.size());
    const std::size_t slash = rest.find('/');
    std::string authority = slash == std::string::npos ? rest : rest.substr(0, slash);
    options.path = slash == std::string::npos ? "/" : rest.substr(slash);
    const std::size_t colon = authority.rfind(':');
    if (colon == std::string::npos) {
        options.host = authority;
        options.port = 80;
    } else {
        options.host = authority.substr(0, colon);
        options.port = static_cast<std::uint16_t>(std::atoi(authority.c_str() + colon + 1));
    }
    return !options.host.empty() && options.port != 0;
}

void PrintUsage(const char* program) {
    std::fprintf(stderr,
                 "usage: %s --conn N [--url ws://host:port/path] [--duration S]\n"
                 "          [--ping-ms N] [--label name] [--batch N] [--batch-ms N]\n"
                 "          [--ramp-timeout S]\n",
                 program);
}

bool ParseOptions(int argc, char** argv, Options& options) {
    for (int index = 1; index < argc; ++index) {
        const std::string name = argv[index];
        const bool has_value = index + 1 < argc;
        auto value = [&]() -> const char* { return has_value ? argv[++index] : nullptr; };
        if (name == "--conn") {
            const char* text = value();
            if (text == nullptr) return false;
            options.conn = static_cast<std::size_t>(std::strtoull(text, nullptr, 10));
        } else if (name == "--url") {
            const char* text = value();
            if (text == nullptr) return false;
            options.url = text;
        } else if (name == "--duration") {
            const char* text = value();
            if (text == nullptr) return false;
            options.duration_s = std::atoi(text);
        } else if (name == "--ping-ms") {
            const char* text = value();
            if (text == nullptr) return false;
            options.ping_ms = std::atoi(text);
        } else if (name == "--label") {
            const char* text = value();
            if (text == nullptr) return false;
            options.label = text;
        } else if (name == "--batch") {
            const char* text = value();
            if (text == nullptr) return false;
            options.batch = static_cast<std::size_t>(std::strtoull(text, nullptr, 10));
        } else if (name == "--batch-ms") {
            const char* text = value();
            if (text == nullptr) return false;
            options.batch_ms = std::atoi(text);
        } else if (name == "--ramp-timeout") {
            const char* text = value();
            if (text == nullptr) return false;
            options.ramp_timeout_s = std::atoi(text);
        } else if (name == "--help" || name == "-h") {
            PrintUsage(argv[0]);
            return false;
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", name.c_str());
            return false;
        }
    }
    if (options.conn == 0 || options.duration_s <= 0 || options.ping_ms <= 0) {
        return false;
    }
    return ParseUrl(options.url, options);
}

enum class Phase : std::uint8_t { Connecting, Handshaking, Open, Dead };

struct Conn {
    int fd{-1};
    std::uint32_t index{0};
    Phase phase{Phase::Connecting};
    std::string out;
    std::string in;
    std::uint64_t connect_start_ns{0};
    std::uint64_t seq{0};
    std::uint64_t ping_sent_ns{0};
    std::uint64_t next_ping_ns{0};
    bool ping_outstanding{false};
    bool want_write{true};
    std::uint64_t echo_sent{0};
};

/// 心跳定时堆条目：when 到点时给 index 连接发 Ping；懒删除，过期条目在
/// 出堆时用 next_ping_ns 是否一致来判有效。
struct PingEntry {
    std::uint64_t when{0};
    std::uint32_t index{0};
    bool operator<(const PingEntry& other) const { return when > other.when; }
};

struct Result {
    std::size_t created{0};
    std::size_t handshake_ok{0};
    std::size_t handshake_fail{0};
    std::size_t disconnects{0};
    std::size_t ping_sent{0};
    std::size_t pong_recv{0};
    std::size_t ping_timeout{0};
    std::size_t echo_ok{0};
    std::size_t create_failed{0};
    std::vector<Micros> handshake_us;
    std::vector<Micros> rtt_us;
};

std::mt19937_64 g_rng{0x9e3779b97f4a7c15ULL};

const char kBase64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/// 生成 16 字节随机数并编成 Sec-WebSocket-Key 用的 24 字符 base64。
std::string MakeWebSocketKey() {
    std::uint8_t raw[16];
    for (std::size_t i = 0; i < sizeof(raw); i += 8) {
        const std::uint64_t value = g_rng();
        std::memcpy(raw + i, &value, 8);
    }
    std::string encoded;
    encoded.reserve(24);
    for (std::size_t i = 0; i + 2 < sizeof(raw); i += 3) {
        const std::uint32_t chunk = (static_cast<std::uint32_t>(raw[i]) << 16) |
                                    (static_cast<std::uint32_t>(raw[i + 1]) << 8) |
                                    static_cast<std::uint32_t>(raw[i + 2]);
        encoded.push_back(kBase64[(chunk >> 18) & 0x3f]);
        encoded.push_back(kBase64[(chunk >> 12) & 0x3f]);
        encoded.push_back(kBase64[(chunk >> 6) & 0x3f]);
        encoded.push_back(kBase64[chunk & 0x3f]);
    }
    // 16 字节 = 5 个完整 3 字节组（15 字节）+ 1 字节尾组，尾组补 "=="。
    const std::uint32_t tail = static_cast<std::uint32_t>(raw[15]) << 16;
    encoded.push_back(kBase64[(tail >> 18) & 0x3f]);
    encoded.push_back(kBase64[(tail >> 12) & 0x3f]);
    encoded += "==";
    return encoded;
}

/// 追加一个带掩码的客户端帧。
void AppendFrame(std::string& out, std::uint8_t opcode, std::string_view payload) {
    out.push_back(static_cast<char>(0x80 | opcode));
    const std::size_t length = payload.size();
    if (length < 126) {
        out.push_back(static_cast<char>(0x80 | length));
    } else if (length <= 0xffff) {
        out.push_back(static_cast<char>(0x80 | 126));
        out.push_back(static_cast<char>((length >> 8) & 0xff));
        out.push_back(static_cast<char>(length & 0xff));
    } else {
        out.push_back(static_cast<char>(0x80 | 127));
        for (int shift = 56; shift >= 0; shift -= 8) {
            out.push_back(static_cast<char>((static_cast<std::uint64_t>(length) >> shift) & 0xff));
        }
    }
    std::uint8_t mask[4];
    const std::uint64_t key = g_rng();
    std::memcpy(mask, &key, sizeof(mask));
    out.append(reinterpret_cast<const char*>(mask), sizeof(mask));
    const std::size_t offset = out.size();
    out.append(payload.data(), payload.size());
    for (std::size_t i = 0; i < length; ++i) {
        out[offset + i] = static_cast<char>(static_cast<std::uint8_t>(payload[i]) ^ mask[i % 4]);
    }
}

/// 解析一个服务端帧（无掩码）；数据不足时返回 false 且不消费缓冲。
bool ParseFrame(std::string& buffer, std::uint8_t& opcode, std::string& payload) {
    if (buffer.size() < 2) {
        return false;
    }
    const std::uint8_t first = static_cast<std::uint8_t>(buffer[0]);
    const std::uint8_t second = static_cast<std::uint8_t>(buffer[1]);
    std::uint64_t length = second & 0x7f;
    std::size_t offset = 2;
    if (length == 126) {
        if (buffer.size() < 4) return false;
        length = (static_cast<std::uint64_t>(static_cast<std::uint8_t>(buffer[2])) << 8) |
                 static_cast<std::uint8_t>(buffer[3]);
        offset = 4;
    } else if (length == 127) {
        if (buffer.size() < 10) return false;
        length = 0;
        for (std::size_t i = 0; i < 8; ++i) {
            length = (length << 8) | static_cast<std::uint8_t>(buffer[2 + i]);
        }
        offset = 10;
    }
    if (length > 1024 * 1024) {
        return false;
    }
    if (buffer.size() < offset + length) {
        return false;
    }
    payload.assign(buffer, offset, static_cast<std::size_t>(length));
    opcode = first & 0x0f;
    buffer.erase(0, offset + static_cast<std::size_t>(length));
    return true;
}

Micros Percentile(std::vector<Micros>& samples, double percentile) {
    if (samples.empty()) {
        return 0;
    }
    std::sort(samples.begin(), samples.end());
    const std::size_t index = std::min<std::size_t>(
        samples.size() - 1,
        static_cast<std::size_t>(percentile * static_cast<double>(samples.size())));
    return samples[index];
}

void ReadProcessCpu(std::uint64_t& user_ticks, std::uint64_t& system_ticks) {
    user_ticks = 0;
    system_ticks = 0;
    std::FILE* file = std::fopen("/proc/self/stat", "r");
    if (file == nullptr) {
        return;
    }
    char buffer[1024];
    if (std::fgets(buffer, sizeof(buffer), file) == nullptr) {
        std::fclose(file);
        return;
    }
    std::fclose(file);
    // comm 字段可能含空格，从最后一个 ')' 之后开始切分，跳过 state 字段。
    const char* cursor = std::strrchr(buffer, ')');
    if (cursor == nullptr) {
        return;
    }
    cursor += 2;
    while (*cursor != '\0' && *cursor != ' ') {
        ++cursor;  // state
    }
    while (*cursor == ' ') {
        ++cursor;
    }
    unsigned long long fields[14] = {0};
    for (int i = 0; i < 14 && *cursor != '\0'; ++i) {
        char* end = nullptr;
        fields[i] = std::strtoull(cursor, &end, 10);
        if (end == cursor) {
            break;
        }
        cursor = end;
        while (*cursor == ' ') ++cursor;
    }
    // fields[0] 是 stat 第 4 字段（ppid）；utime/stime 是第 14/15 字段。
    user_ticks = fields[10];
    system_ticks = fields[11];
}

std::uint64_t ReadStatusKb(const char* key) {
    std::FILE* file = std::fopen("/proc/self/status", "r");
    if (file == nullptr) {
        return 0;
    }
    char line[256];
    std::uint64_t value = 0;
    const std::size_t key_length = std::strlen(key);
    while (std::fgets(line, sizeof(line), file) != nullptr) {
        if (std::strncmp(line, key, key_length) == 0) {
            value = std::strtoull(line + key_length, nullptr, 10);
            break;
        }
    }
    std::fclose(file);
    return value;
}

class LoadClient {
public:
    LoadClient(const Options& options, Result& result)
        : m_options(options), m_result(result) {
        m_conns.resize(options.conn);
        m_conns.reserve(options.conn);
        m_epoll_fd = ::epoll_create1(EPOLL_CLOEXEC);
        m_server_address.sin_family = AF_INET;
        m_server_address.sin_port = htons(options.port);
        (void)::inet_pton(AF_INET, options.host.c_str(), &m_server_address.sin_addr);
    }

    ~LoadClient() {
        for (auto& conn : m_conns) {
            if (conn.fd >= 0) {
                ::close(conn.fd);
            }
        }
        if (m_epoll_fd >= 0) {
            ::close(m_epoll_fd);
        }
    }

    bool valid() const { return m_epoll_fd >= 0; }

    /**
     * @brief 跑完整个压测流程：建连 → steady 心跳 → 收尾统计。
     *
     * @return 0 表示流程完成（不代表所有连接成功）；非 0 表示 epoll 等致命错误。
     * ## 步骤
     * step1. 按批建连并注册 epoll；step2. 驱动事件循环直到全部连接有结果；
     * step3. 进入 steady 阶段，按 ping_ms 发心跳并统计 RTT；
     * step4. 到时关闭连接并汇总。
     */
    int Run() {
        const std::uint64_t start_ns = NowNs();
        std::uint64_t next_batch_ns = start_ns;
        std::uint64_t next_progress_ns = start_ns + 5ULL * 1000000000ULL;
        std::uint64_t steady_start_ns = 0;
        bool steady = false;
        const std::uint64_t ramp_timeout_ns =
            static_cast<std::uint64_t>(m_options.ramp_timeout_s) * 1000000000ULL;
        const std::uint64_t duration_ns =
            static_cast<std::uint64_t>(m_options.duration_s) * 1000000000ULL;
        const std::uint64_t ping_interval_ns =
            static_cast<std::uint64_t>(m_options.ping_ms) * 1000000ULL;
        std::vector<epoll_event> events(256);

        while (true) {
            std::uint64_t now = NowNs();
            if (!steady) {
                // 每轮事件循环最多补 4 个批次：既能在事件处理变慢时追赶，
                // 又不会把所有连接一次性 create 完而饿死 epoll（2 万次
                // connect 连续执行会让握手响应完全无人处理）。
                if (m_result.created < m_options.conn) {
                    std::size_t budget = m_options.batch * 4;
                    while (budget > 0 && NowNs() >= next_batch_ns &&
                           m_result.created < m_options.conn) {
                        const std::size_t remaining = m_options.conn - m_result.created;
                        const std::size_t count = std::min({m_options.batch, remaining, budget});
                        for (std::size_t i = 0; i < count; ++i) {
                            CreateConnection(m_result.created);
                            ++m_result.created;
                        }
                        budget -= count;
                        next_batch_ns +=
                            static_cast<std::uint64_t>(m_options.batch_ms) * 1000000ULL;
                    }
                }
                now = NowNs();
                const bool all_created = m_result.created >= m_options.conn;
                const bool all_resolved =
                    m_result.handshake_ok + m_result.handshake_fail >= m_options.conn;
                if ((all_created && all_resolved) || now - start_ns >= ramp_timeout_ns) {
                    steady = true;
                    steady_start_ns = now;
                    m_result.rtt_us.clear();
                    m_counting = true;
                    // steady 开始才发心跳：ramp 阶段的 Ping 会把建连事件循环
                    // 拖慢，也会让 RTT 样本混入“建连期排队”噪声。这里按
                    // 1s 窗口均匀抖动排程，避免 2 万条连接同一毫秒爆发。
                    for (auto& conn : m_conns) {
                        if (conn.phase == Phase::Open) {
                            SchedulePing(conn, now + (g_rng() % ping_interval_ns));
                        }
                    }
                    std::fprintf(stderr,
                                 "[ramp] done in %.2fs created=%zu ok=%zu fail=%zu\n",
                                 static_cast<double>(now - start_ns) / 1e9,
                                 m_result.created, m_result.handshake_ok,
                                 m_result.handshake_fail);
                }
            } else if (now - steady_start_ns >= duration_ns) {
                break;
            }

            const int wait_ms = steady ? 1 : 2;
            const int ready = ::epoll_wait(m_epoll_fd, events.data(),
                                           static_cast<int>(events.size()), wait_ms);
            for (int i = 0; i < ready; ++i) {
                const std::uint32_t index = events[static_cast<std::size_t>(i)].data.u32;
                HandleEvent(index, events[static_cast<std::size_t>(i)].events);
            }

            TickPings(NowNs(), ping_interval_ns);

            const std::uint64_t progress = NowNs();
            if (progress >= next_progress_ns) {
                std::fprintf(stderr,
                             "[t=%4.0fs] created=%zu open=%zu ok=%zu fail=%zu rtt_n=%zu\n",
                             static_cast<double>(progress - start_ns) / 1e9,
                             m_result.created, OpenCount(), m_result.handshake_ok,
                             m_result.handshake_fail, m_result.rtt_us.size());
                next_progress_ns = progress + 1000000000ULL;
            }
        }

        m_steady_elapsed_s = static_cast<double>(NowNs() - steady_start_ns) / 1e9;
        m_total_elapsed_s = static_cast<double>(NowNs() - start_ns) / 1e9;
        CloseAll();
        return 0;
    }

    std::size_t OpenCount() const { return m_result.handshake_ok - m_result.disconnects; }

private:
    void CreateConnection(std::size_t index) {
        Conn& conn = m_conns[index];
        conn.index = static_cast<std::uint32_t>(index);
        conn.connect_start_ns = NowNs();
        const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0) {
            ++m_result.create_failed;
            ++m_result.handshake_fail;
            conn.phase = Phase::Dead;
            return;
        }
        int enabled = 1;
        (void)::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled));
        const int rc = ::connect(fd, reinterpret_cast<const sockaddr*>(&m_server_address),
                                 sizeof(m_server_address));
        if (rc != 0 && errno != EINPROGRESS) {
            ::close(fd);
            ++m_result.handshake_fail;
            conn.phase = Phase::Dead;
            return;
        }
        conn.fd = fd;
        conn.phase = Phase::Connecting;
        conn.want_write = true;
        epoll_event event{};
        event.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP;
        event.data.u32 = conn.index;
        if (::epoll_ctl(m_epoll_fd, EPOLL_CTL_ADD, fd, &event) != 0) {
            ::close(fd);
            conn.fd = -1;
            ++m_result.handshake_fail;
            conn.phase = Phase::Dead;
        }
    }

    void UpdateInterest(Conn& conn) {
        const bool want_write = !conn.out.empty();
        if (want_write == conn.want_write) {
            return;
        }
        conn.want_write = want_write;
        epoll_event event{};
        event.events = EPOLLIN | EPOLLRDHUP | (want_write ? EPOLLOUT : 0U);
        event.data.u32 = conn.index;
        (void)::epoll_ctl(m_epoll_fd, EPOLL_CTL_MOD, conn.fd, &event);
    }

    void HandleEvent(std::uint32_t index, std::uint32_t events) {
        if (index >= m_conns.size()) {
            return;
        }
        Conn& conn = m_conns[index];
        if (conn.phase == Phase::Dead || conn.fd < 0) {
            return;
        }
        if ((events & EPOLLOUT) != 0) {
            if (conn.phase == Phase::Connecting) {
                int error = 0;
                socklen_t length = sizeof(error);
                (void)::getsockopt(conn.fd, SOL_SOCKET, SO_ERROR, &error, &length);
                if (error != 0) {
                    FailConn(conn);
                    return;
                }
                SendHandshake(conn);
            }
            if (!Flush(conn)) {
                FailConn(conn);
                return;
            }
        }
        if ((events & (EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR)) != 0) {
            if (!ReadConn(conn)) {
                FailConn(conn);
                return;
            }
        }
    }

    void SendHandshake(Conn& conn) {
        std::string request;
        request.reserve(256);
        request += "GET ";
        request += m_options.path;
        request += " HTTP/1.1\r\nHost: ";
        request += m_options.host;
        request += ':';
        request += std::to_string(m_options.port);
        request += "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ";
        request += MakeWebSocketKey();
        request += "\r\nSec-WebSocket-Version: 13\r\n\r\n";
        conn.out += request;
        conn.phase = Phase::Handshaking;
    }

    bool Flush(Conn& conn) {
        while (!conn.out.empty()) {
            const ssize_t written =
                ::send(conn.fd, conn.out.data(), conn.out.size(), MSG_NOSIGNAL);
            if (written > 0) {
                conn.out.erase(0, static_cast<std::size_t>(written));
                continue;
            }
            if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                break;
            }
            return false;
        }
        UpdateInterest(conn);
        return true;
    }

    bool ReadConn(Conn& conn) {
        char buffer[64 * 1024];
        for (;;) {
            const ssize_t received = ::recv(conn.fd, buffer, sizeof(buffer), 0);
            if (received > 0) {
                conn.in.append(buffer, static_cast<std::size_t>(received));
                continue;
            }
            if (received == 0) {
                return false;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (conn.phase == Phase::Handshaking) {
            if (!ProcessHandshake(conn)) {
                return false;
            }
        }
        if (conn.phase == Phase::Open) {
            return ProcessFrames(conn);
        }
        return true;
    }

    bool ProcessHandshake(Conn& conn) {
        const std::size_t end = conn.in.find("\r\n\r\n");
        if (end == std::string::npos) {
            return conn.in.size() <= 16384;  // 响应头异常大则判失败
        }
        const std::string_view header(conn.in.data(), end + 4);
        const bool accepted = header.find(" 101 ") != std::string_view::npos &&
                              header.find("Sec-WebSocket-Accept:") != std::string_view::npos;
        if (!accepted) {
            std::fprintf(stderr, "handshake rejected: %.*s\n",
                         static_cast<int>(std::min<std::size_t>(end + 4, 160)), header.data());
            return false;
        }
        conn.in.erase(0, end + 4);
        conn.phase = Phase::Open;
        ++m_result.handshake_ok;
        const std::uint64_t elapsed_us = (NowNs() - conn.connect_start_ns) / 1000;
        m_result.handshake_us.push_back(
            static_cast<Micros>(std::min<std::uint64_t>(elapsed_us, 60ULL * 1000000ULL)));
        // 握手后立即发一条文本做回显校验（同时验证数据面）。
        AppendFrame(conn.out, 0x1, "hello-ws");
        ++conn.echo_sent;
        if (m_counting) {
            // ramp 超时后仍有连接陆续完成握手，按 1s 窗口补排心跳。
            SchedulePing(conn, NowNs() +
                                   (g_rng() % (static_cast<std::uint64_t>(m_options.ping_ms) * 1000000ULL)));
        }
        return Flush(conn);
    }

    bool ProcessFrames(Conn& conn) {
        std::uint8_t opcode = 0;
        std::string payload;
        while (ParseFrame(conn.in, opcode, payload)) {
            switch (opcode) {
                case 0x1:
                case 0x2:
                    ++m_result.echo_ok;
                    break;
                case 0x9:  // 服务端 Ping → 回 Pong
                    AppendFrame(conn.out, 0xA, payload);
                    break;
                case 0xA: {  // Pong → 计算 RTT
                    if (payload.size() == 8 && conn.ping_outstanding) {
                        std::uint64_t seq = 0;
                        for (std::size_t i = 0; i < 8; ++i) {
                            seq = (seq << 8) | static_cast<std::uint8_t>(payload[i]);
                        }
                        if (seq == conn.seq) {
                            const std::uint64_t rtt_us = (NowNs() - conn.ping_sent_ns) / 1000;
                            m_result.rtt_us.push_back(
                                static_cast<Micros>(std::min<std::uint64_t>(rtt_us, 60ULL * 1000000ULL)));
                            ++m_result.pong_recv;
                            conn.ping_outstanding = false;
                        }
                    }
                    break;
                }
                case 0x8:  // Close
                    return false;
                default:
                    break;
            }
        }
        if (!conn.out.empty()) {
            return Flush(conn);
        }
        return true;
    }

    void SchedulePing(Conn& conn, std::uint64_t when_ns) {
        conn.next_ping_ns = when_ns;
        m_pings.push(PingEntry{when_ns, conn.index});
    }

    void TickPings(std::uint64_t now, std::uint64_t interval_ns) {
        if (!m_counting) {
            return;
        }
        const std::uint64_t timeout_ns = interval_ns * 3;
        while (!m_pings.empty() && m_pings.top().when <= now) {
            const PingEntry entry = m_pings.top();
            m_pings.pop();
            if (entry.index >= m_conns.size()) {
                continue;
            }
            Conn& conn = m_conns[entry.index];
            // 懒删除：连接已死或该条目已被更新的定时替换。
            if (conn.phase != Phase::Open || conn.next_ping_ns != entry.when) {
                continue;
            }
            if (conn.ping_outstanding) {
                if (now - conn.ping_sent_ns > timeout_ns) {
                    conn.ping_outstanding = false;
                    ++m_result.ping_timeout;
                } else {
                    SchedulePing(conn, now + interval_ns);
                    continue;
                }
            }
            const std::uint64_t seq = ++conn.seq;
            char payload[8];
            for (int i = 0; i < 8; ++i) {
                payload[i] = static_cast<char>((seq >> (56 - i * 8)) & 0xff);
            }
            AppendFrame(conn.out, 0x9, std::string_view(payload, sizeof(payload)));
            ++m_result.ping_sent;
            conn.ping_sent_ns = now;
            conn.ping_outstanding = true;
            SchedulePing(conn, now + interval_ns);
            if (!Flush(conn)) {
                FailConn(conn);
            }
        }
    }

    void FailConn(Conn& conn) {
        if (conn.phase == Phase::Dead) {
            return;
        }
        if (conn.phase == Phase::Open) {
            ++m_result.disconnects;
        } else {
            ++m_result.handshake_fail;
        }
        conn.phase = Phase::Dead;
        if (conn.fd >= 0) {
            ::epoll_ctl(m_epoll_fd, EPOLL_CTL_DEL, conn.fd, nullptr);
            ::close(conn.fd);
            conn.fd = -1;
        }
    }

    void CloseAll() {
        // SO_LINGER(0) 让关闭走 RST：压测会连续跑多档，避免 2 万条连接在
        // TIME_WAIT 里占满本地临时端口（每档之间要立刻复用端口）。
        struct linger linger_option {
            1, 0
        };
        for (auto& conn : m_conns) {
            if (conn.fd < 0) {
                continue;
            }
            (void)::setsockopt(conn.fd, SOL_SOCKET, SO_LINGER, &linger_option,
                               sizeof(linger_option));
            if (conn.phase == Phase::Open) {
                conn.out.clear();
                AppendFrame(conn.out, 0x8, std::string_view("\x03\xe8", 2));
                (void)::send(conn.fd, conn.out.data(), conn.out.size(), MSG_NOSIGNAL);
            }
            ::close(conn.fd);
            conn.fd = -1;
            conn.phase = Phase::Dead;
        }
    }

    const Options& m_options;
    Result& m_result;
    std::vector<Conn> m_conns;
    int m_epoll_fd{-1};
    sockaddr_in m_server_address{};
    std::priority_queue<PingEntry> m_pings;
    bool m_counting{false};
    double m_steady_elapsed_s{0.0};
    double m_total_elapsed_s{0.0};

public:
    double steady_elapsed_s() const { return m_steady_elapsed_s; }
    double total_elapsed_s() const { return m_total_elapsed_s; }
};

}  // namespace

int main(int argc, char** argv) {
    Options options;
    if (!ParseOptions(argc, argv, options)) {
        PrintUsage(argv[0]);
        return 2;
    }

    std::signal(SIGPIPE, SIG_IGN);
    Result result;
    result.handshake_us.reserve(options.conn);
    result.rtt_us.reserve(options.conn * static_cast<std::size_t>(options.duration_s) * 1000 /
                          static_cast<std::size_t>(options.ping_ms));

    std::uint64_t cpu_start_user = 0;
    std::uint64_t cpu_start_system = 0;
    ReadProcessCpu(cpu_start_user, cpu_start_system);

    LoadClient client(options, result);
    if (!client.valid()) {
        std::fprintf(stderr, "epoll_create1 failed: %s\n", std::strerror(errno));
        return 1;
    }
    const int rc = client.Run();
    if (rc != 0) {
        return rc;
    }

    std::uint64_t cpu_end_user = 0;
    std::uint64_t cpu_end_system = 0;
    ReadProcessCpu(cpu_end_user, cpu_end_system);
    const long ticks = ::sysconf(_SC_CLK_TCK);
    const double cpu_seconds =
        static_cast<double>((cpu_end_user - cpu_start_user) + (cpu_end_system - cpu_start_system)) /
        static_cast<double>(ticks > 0 ? ticks : 100);
    const double cpu_percent =
        client.total_elapsed_s() > 0 ? 100.0 * cpu_seconds / client.total_elapsed_s() : 0.0;

    const Micros hs_p50 = Percentile(result.handshake_us, 0.50);
    const Micros hs_p99 = Percentile(result.handshake_us, 0.99);
    const Micros rtt_p50 = Percentile(result.rtt_us, 0.50);
    const Micros rtt_p99 = Percentile(result.rtt_us, 0.99);
    const Micros rtt_p999 = Percentile(result.rtt_us, 0.999);
    const std::size_t open = result.handshake_ok - result.disconnects;

    std::printf(
        "{\"label\":\"%s\",\"target\":%zu,\"created\":%zu,\"connected\":%zu,"
        "\"handshake_ok\":%zu,\"handshake_fail\":%zu,\"create_failed\":%zu,"
        "\"handshake_p50_ms\":%.3f,\"handshake_p99_ms\":%.3f,"
        "\"ping_sent\":%zu,\"pong_recv\":%zu,\"ping_timeout\":%zu,"
        "\"rtt_p50_ms\":%.3f,\"rtt_p99_ms\":%.3f,\"rtt_p999_ms\":%.3f,"
        "\"disconnects\":%zu,\"echo_ok\":%zu,\"duration_s\":%d,"
        "\"steady_s\":%.2f,\"elapsed_s\":%.2f,\"client_cpu_pct\":%.1f,"
        "\"client_maxrss_kb\":%llu}\n",
        options.label.c_str(), options.conn, result.created, open,
        result.handshake_ok, result.handshake_fail, result.create_failed,
        static_cast<double>(hs_p50) / 1000.0, static_cast<double>(hs_p99) / 1000.0,
        result.ping_sent, result.pong_recv, result.ping_timeout,
        static_cast<double>(rtt_p50) / 1000.0, static_cast<double>(rtt_p99) / 1000.0,
        static_cast<double>(rtt_p999) / 1000.0,
        result.disconnects, result.echo_ok, options.duration_s,
        client.steady_elapsed_s(), client.total_elapsed_s(), cpu_percent,
        static_cast<unsigned long long>(ReadStatusKb("VmHWM:")));
    return 0;
}
