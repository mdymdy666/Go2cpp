/**
 * @file audit_peer_hub_stress.cpp
 * @brief peer_hub（多 peer UDP 链路）审查用压测/随机/场景程序（不注册 CTest）。
 * @author go2cpp contributors
 * @date 2026-10-29
 * @copyright MIT License
 *
 * @details 与 `tests/bento/test_peer_hub.cpp`、`test_peer_hub_secure.cpp` 的分工：
 *          - 两个 `test_peer_hub*` 是注册进 CTest 的确定性回归测试（秒级）；
 *          - 本程序是**审查压测工具**：1000 peer 同时在线、10 万帧可靠转发、
 *            1000+ 随机连接/断开/加密握手/回调内删除、5 个真实场景
 *            （per-peer 独立 PSK、超时回收释放 max_peers、回调内 RemovePeer/
 *            Close/重连、反复重连的 session/conn_id 同步、60s 随机出入长稳）
 *            与 RSS/fd 泄漏检查。
 *
 *          刻意**不** include `test_util.hpp`（它会拖入 `link.hpp` 的测试辅助），
 *          以独立证明 peer_hub 只依赖 link/crypto/channel，不依赖 app 层。
 *
 * 用法：
 * @code
 *   audit_peer_hub_stress --mode=all                    # 全量（含 60s 长稳）
 *   audit_peer_hub_stress --mode=bench --peers=1000 --frames=100
 *   audit_peer_hub_stress --mode=random --ops=1500
 *   audit_peer_hub_stress --mode=scenarios
 *   audit_peer_hub_stress --mode=soak --seconds=60
 *   audit_peer_hub_stress --mode=leaks --rounds=100 --peers=16
 * @endcode
 *
 * 退出码：0 = 全部断言通过；1 = 有失败。
 */

#include <arpa/inet.h>
#include <dirent.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "go2cpp/bento/net/peer_hub.hpp"

namespace {

using go2cpp::bento::PeerHub;
using go2cpp::bento::PeerHubConfig;
using go2cpp::bento::PeerHubStats;
using go2cpp::bento::PeerInfo;
using go2cpp::bento::PeerState;
using go2cpp::bento::SendStatus;
using Clock = std::chrono::steady_clock;

long g_checks = 0;
long g_failures = 0;

#define CHECK(condition)                                                                        \
    do {                                                                                        \
        ++g_checks;                                                                             \
        if (!(condition)) {                                                                     \
            std::fprintf(stderr, "[FAIL] %s:%d: %s\n", __FILE__, __LINE__, #condition);         \
            ++g_failures;                                                                       \
        }                                                                                       \
    } while (0)

constexpr std::size_t kFrameBytes = 64;

// ---------------------------------------------------------------------------
// 小工具：时钟、RSS/fd、负载校验
// ---------------------------------------------------------------------------

std::uint64_t NowNs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
            .count());
}

double SecondsSince(std::uint64_t start_ns) {
    return static_cast<double>(NowNs() - start_ns) / 1e9;
}

/// 读取 /proc/self/status 里的某个 kB 字段（VmRSS / VmHWM）。
std::size_t StatusKb(const char* key) {
    std::ifstream status("/proc/self/status");
    std::string line;
    const std::size_t key_len = std::strlen(key);
    while (std::getline(status, line)) {
        if (line.compare(0, key_len, key) != 0) {
            continue;
        }
        const char* cursor = line.c_str() + key_len;
        while (*cursor == ':' || *cursor == ' ' || *cursor == '\t') {
            ++cursor;
        }
        return static_cast<std::size_t>(std::strtoull(cursor, nullptr, 10));
    }
    return 0;
}

std::size_t RssKb() {
    return StatusKb("VmRSS:");
}

std::size_t PeakRssKb() {
    return StatusKb("VmHWM:");
}

int FdCount() {
    DIR* dir = ::opendir("/proc/self/fd");
    if (dir == nullptr) {
        return -1;
    }
    int count = 0;
    while (::readdir(dir) != nullptr) {
        ++count;
    }
    ::closedir(dir);
    // 去掉 "."、".." 与 opendir 自己持有的 fd。
    return count - 3;
}

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

void WriteU64(std::uint8_t* out, std::uint64_t value) {
    WriteU32(out, static_cast<std::uint32_t>(value >> 32));
    WriteU32(out + 4, static_cast<std::uint32_t>(value));
}

std::uint64_t ReadU64(const std::uint8_t* in) {
    return (static_cast<std::uint64_t>(ReadU32(in)) << 32) | ReadU32(in + 4);
}

/// 负载布局：[0..4) owner（客户端序号）、[4..8) seq、[8..16) 发送时刻 ns、
/// 其余按 (owner, seq, 偏移) 填充可校验字节。
void FillPayload(std::vector<std::uint8_t>& payload, std::uint32_t owner, std::uint32_t seq,
                 std::uint64_t send_ns) {
    WriteU32(payload.data(), owner);
    WriteU32(payload.data() + 4, seq);
    WriteU64(payload.data() + 8, send_ns);
    for (std::size_t i = 16; i < payload.size(); ++i) {
        payload[i] = static_cast<std::uint8_t>((owner * 131U + seq * 17U + i * 7U) & 0xFFU);
    }
}

bool VerifyPayload(const std::uint8_t* data, std::size_t bytes, std::uint32_t* owner,
                   std::uint32_t* seq, std::uint64_t* send_ns) {
    if (bytes != kFrameBytes) {
        return false;
    }
    *owner = ReadU32(data);
    *seq = ReadU32(data + 4);
    *send_ns = ReadU64(data + 8);
    for (std::size_t i = 16; i < bytes; ++i) {
        const std::uint8_t expected =
            static_cast<std::uint8_t>(((*owner) * 131U + (*seq) * 17U + i * 7U) & 0xFFU);
        if (data[i] != expected) {
            return false;
        }
    }
    return true;
}

std::uint32_t PercentileUs(const std::vector<std::uint32_t>& sorted, double ratio) {
    if (sorted.empty()) {
        return 0;
    }
    const double index = ratio * static_cast<double>(sorted.size() - 1);
    const std::size_t offset = static_cast<std::size_t>(index + 0.5);
    return sorted[std::min(offset, sorted.size() - 1)];
}

void PrintLatency(const char* tag, const std::vector<std::uint32_t>& samples) {
    std::vector<std::uint32_t> sorted = samples;
    std::sort(sorted.begin(), sorted.end());
    const std::uint32_t p50 = PercentileUs(sorted, 0.50);
    const std::uint32_t p95 = PercentileUs(sorted, 0.95);
    const std::uint32_t p99 = PercentileUs(sorted, 0.99);
    const std::uint32_t maximum = sorted.empty() ? 0 : sorted.back();
    std::printf("[INFO] %s latency_us: p50=%u p95=%u p99=%u max=%u samples=%zu\n", tag, p50, p95,
                p99, maximum, sorted.size());
}

/// 服务端/客户端的基础配置（测试用短定时器，避免 60s 长稳被默认 5s 超时拖慢）。
PeerHubConfig MakeConfig(std::size_t max_peers, std::uint32_t peer_timeout_ms = 3000,
                         std::uint32_t handshake_timeout_ms = 2000) {
    PeerHubConfig config;
    config.bind_address = "127.0.0.1";
    config.bind_port = 0;
    config.heartbeat_interval_ms = 100;
    config.peer_timeout_ms = peer_timeout_ms;
    config.max_peers = max_peers;
    config.accept_unknown_peers = true;
    config.handshake_timeout_ms = handshake_timeout_ms;
    config.hello_initial_pto_ms = 50;
    config.hello_max_pto_ms = 200;
    config.hello_max_attempts = 8;
    return config;
}

/// 每个客户端 Hub 绑不同回环地址（127.1.x.y）。
/// 背景：UdpSocket::Open 无条件开 SO_REUSEADDR，同进程内 1000 个 bind(0)
/// 的 UDP socket 实测出现 ~18 对端口复用（生日碰撞）。复用时内核只把包投给
/// 其中一个 socket，另一个 Hub 会「假连接」——服务端记录的 conn_id 与客户端
/// 不一致，客户端 20s 只收到 1 帧后超时。压测侧用不同源 IP 绕开，
/// 服务端仍按 (IP,port) 区分 1000 个 peer。
std::string ClientBindAddress(std::size_t index) {
    const std::size_t third = index / 200 + 1;
    const std::size_t fourth = index % 200 + 1;
    return "127.1." + std::to_string(third) + "." + std::to_string(fourth);
}

PeerHubConfig MakeClientConfig(std::size_t index, std::size_t max_peers = 8,
                               std::uint32_t peer_timeout_ms = 3000,
                               std::uint32_t handshake_timeout_ms = 2000) {
    PeerHubConfig config = MakeConfig(max_peers, peer_timeout_ms, handshake_timeout_ms);
    config.bind_address = ClientBindAddress(index);
    return config;
}

/// 多 Hub 交替驱动，直到谓词为真或超时。
bool PumpUntil(const std::vector<PeerHub*>& hubs, const std::function<bool()>& done,
               int timeout_ms) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(std::max(timeout_ms, 0));
    while (Clock::now() < deadline) {
        for (PeerHub* hub : hubs) {
            hub->RunOnce(0);
        }
        if (done()) {
            return true;
        }
    }
    return done();
}

void PumpFor(const std::vector<PeerHub*>& hubs, int duration_ms) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(std::max(duration_ms, 0));
    while (Clock::now() < deadline) {
        for (PeerHub* hub : hubs) {
            hub->RunOnce(0);
        }
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
}

/// 每个 Hub 的内部一致性：PeerIds/Peers/stats/peer_state 必须互相吻合。
void CheckHubInvariants(PeerHub& hub, const char* tag) {
    const std::vector<std::uint32_t> ids = hub.PeerIds();
    const std::vector<PeerInfo> peers = hub.Peers();
    CHECK(ids.size() == peers.size());
    CHECK(std::is_sorted(ids.begin(), ids.end()));
    std::size_t connected = 0;
    for (std::size_t i = 0; i < ids.size() && i < peers.size(); ++i) {
        CHECK(ids[i] == peers[i].peer_id);
        CHECK(hub.HasPeer(ids[i]));
        CHECK(hub.peer_state(ids[i]) == peers[i].state);
        if (peers[i].state == PeerState::kConnected) {
            ++connected;
        }
        if (peers[i].secure_ready) {
            CHECK(peers[i].secure);
            CHECK(hub.peer_secure_ready(ids[i]));
        }
    }
    const PeerHubStats stats = hub.stats();
    ++g_checks;
    if (stats.peers_active != connected) {
        std::fprintf(stderr, "[FAIL] %s: peers_active=%llu connected=%zu\n", tag,
                     static_cast<unsigned long long>(stats.peers_active), connected);
        ++g_failures;
    }
    // 不存在的 peer 必须报 kClosed 且发送失败。
    const std::uint32_t missing = 0xFFFFFFFFU;
    CHECK(!hub.HasPeer(missing));
    CHECK(hub.peer_state(missing) == PeerState::kClosed);
    const std::uint8_t tiny[4] = {1, 2, 3, 4};
    CHECK(!hub.SendStream(missing, 0, tiny, sizeof(tiny)));
    CHECK(!hub.SendDatagram(missing, 0, tiny, sizeof(tiny)));
    CHECK(hub.TrySendStream(missing, 0, tiny, sizeof(tiny)) == SendStatus::kError);
}

/// 小型辅助（避免在 CHECK 里写多行比较）。
void CheckEqualSize(std::size_t actual, std::size_t expected) {
    ++g_checks;
    if (actual != expected) {
        std::fprintf(stderr, "[FAIL] %s:%d: %zu != %zu\n", __FILE__, __LINE__, actual, expected);
        ++g_failures;
    }
}

// ---------------------------------------------------------------------------
// 场景 1：1000 peer 同时在线 + 10 万帧可靠转发
// ---------------------------------------------------------------------------

void RunBenchThousandPeers(std::size_t peers, std::size_t frames_per_peer) {
    const std::size_t total_frames = peers * frames_per_peer;
    std::printf("\n[bench] peers=%zu frames/peer=%zu total=%zu\n", peers, frames_per_peer,
                total_frames);
    const std::size_t rss_before = RssKb();
    const int fd_before = FdCount();

    PeerHubConfig server_config = MakeConfig(peers + 64, 20000, 30000);
    server_config.heartbeat_interval_ms = 500;
    PeerHub server(server_config);
    std::string error;
    CHECK(server.Open(&error));
    if (!server.is_open()) {
        std::fprintf(stderr, "[FAIL] server open: %s\n", error.c_str());
        return;
    }

    const std::uint64_t connect_start = NowNs();
    std::vector<std::unique_ptr<PeerHub>> clients;
    clients.reserve(peers);
    bool created = true;
    for (std::size_t i = 0; i < peers; ++i) {
        clients.push_back(std::make_unique<PeerHub>(MakeClientConfig(i, 8, 20000, 30000)));
        if (!clients.back()->Open(&error) ||
            !clients.back()->AddPeer(1, "127.0.0.1", server.local_port(), &error)) {
            std::fprintf(stderr, "[FAIL] client %zu: %s\n", i, error.c_str());
            CHECK(false);
            created = false;
            break;
        }
        // 分批建连：真实服务端是持续运行的独立进程，客户端陆续接入。
        // 一次性把 1000 个 HELLO 全塞进服务端 rcvbuf 会溢出（rmem_max 212KB
        // 约 270 个 skb），那是驱动方式造成的丢包，不是实现缺陷。
        if (((i + 1) % 50) == 0) {
            std::vector<PeerHub*> batch_hubs{&server};
            for (auto& created_client : clients) {
                batch_hubs.push_back(created_client.get());
            }
            PumpFor(batch_hubs, 12);
        }
    }
    CHECK(created);
    if (!created) {
        return;
    }

    // 服务端校验：peer_id → client 序号绑定、每个 peer 独立按序。
    std::map<std::uint32_t, std::uint32_t> peer_owner;
    std::map<std::uint32_t, std::uint32_t> expected_seq;
    std::vector<std::uint32_t> latencies_us;
    latencies_us.reserve(total_frames);
    std::size_t delivered = 0;
    std::size_t cross_talk = 0;
    std::size_t bad_payload = 0;
    std::size_t out_of_order = 0;
    server.on_stream = [&](std::uint32_t peer_id, std::uint8_t channel, const std::uint8_t* data,
                           std::size_t bytes) {
        if (channel != 0) {
            ++bad_payload;
            return;
        }
        std::uint32_t owner = 0;
        std::uint32_t seq = 0;
        std::uint64_t send_ns = 0;
        if (!VerifyPayload(data, bytes, &owner, &seq, &send_ns)) {
            ++bad_payload;
            return;
        }
        const auto bound = peer_owner.find(peer_id);
        if (bound == peer_owner.end()) {
            peer_owner.emplace(peer_id, owner);
        } else if (bound->second != owner) {
            ++cross_talk;
            return;
        }
        std::uint32_t& expected = expected_seq[peer_id];
        if (seq != expected) {
            ++out_of_order;
            return;
        }
        ++expected;
        ++delivered;
        const std::uint64_t now = NowNs();
        if (now > send_ns) {
            const std::uint64_t delta_us = (now - send_ns) / 1000U;
            if (delta_us < 60U * 1000U * 1000U) {
                latencies_us.push_back(static_cast<std::uint32_t>(delta_us));
            }
        }
    };

    // ---- 建立 1000 条连接 ----
    const auto connect_deadline = Clock::now() + std::chrono::seconds(20);
    std::size_t active = 0;
    int round = 0;
    while (Clock::now() < connect_deadline) {
        server.RunOnce(0);
        for (auto& client : clients) {
            client->RunOnce(0);
        }
        if ((++round % 10) == 0) {
            active = server.stats().peers_active;
            bool all_clients = true;
            for (auto& client : clients) {
                if (!client->peer_connected(1)) {
                    all_clients = false;
                    break;
                }
            }
            if (active >= peers && all_clients) {
                break;
            }
        }
    }
    const double connect_seconds = SecondsSince(connect_start);
    active = server.stats().peers_active;
    std::size_t client_connected = 0;
    for (auto& client : clients) {
        if (client->peer_connected(1)) {
            ++client_connected;
        }
    }
    std::printf("[INFO] connect: server_active=%zu client_connected=%zu elapsed=%.2fs\n", active,
                client_connected, connect_seconds);
    CHECK(active == peers);
    CHECK(client_connected == peers);
    if (active != peers) {
        const PeerHubStats diag = server.stats();
        std::printf("[DIAG] server rejected=%llu timeouts=%llu unknown=%llu invalid=%llu\n",
                    static_cast<unsigned long long>(diag.peers_rejected),
                    static_cast<unsigned long long>(diag.timeouts),
                    static_cast<unsigned long long>(diag.unknown_peer_frames),
                    static_cast<unsigned long long>(diag.invalid_frames));
        std::printf("[DIAG] server frames_rx=%llu frames_tx=%llu send_err=%llu\n",
                    static_cast<unsigned long long>(diag.frames_received),
                    static_cast<unsigned long long>(diag.frames_sent),
                    static_cast<unsigned long long>(diag.send_errors));
        std::size_t state_count[5] = {0, 0, 0, 0, 0};
        for (const PeerInfo& info : server.Peers()) {
            const std::size_t index = static_cast<std::size_t>(info.state);
            if (index < 5) {
                ++state_count[index];
            }
            if (info.state != PeerState::kConnected) {
                std::printf("[DIAG] server unconnected peer id=%u state=%s addr=%s:%u "
                            "conn=%08x rx=%llu tx=%llu\n",
                            info.peer_id, go2cpp::bento::PeerStateName(info.state),
                            info.address.c_str(), info.port, info.conn_id,
                            static_cast<unsigned long long>(info.frames_received),
                            static_cast<unsigned long long>(info.frames_sent));
            }
        }
        std::printf("[DIAG] server peer states: idle=%zu connecting=%zu connected=%zu "
                    "timedout=%zu closed=%zu total=%zu\n",
                    state_count[0], state_count[1], state_count[2], state_count[3],
                    state_count[4], server.PeerIds().size());
        for (std::size_t i = 0; i < peers; ++i) {
            if (clients[i]->peer_connected(1)) {
                continue;
            }
            const std::vector<PeerInfo> info = clients[i]->Peers();
            const PeerHubStats own = clients[i]->stats();
            const std::uint16_t local_port = clients[i]->local_port();
            int same_port_clients = 0;
            for (std::size_t other = 0; other < peers; ++other) {
                if (other != i && clients[other]->local_port() == local_port) {
                    ++same_port_clients;
                }
            }
            std::uint32_t matched_id = 0;
            std::uint32_t matched_conn = 0;
            std::uint64_t matched_rx = 0;
            std::uint64_t matched_tx = 0;
            for (const PeerInfo& server_peer : server.Peers()) {
                if (server_peer.port == local_port) {
                    matched_id = server_peer.peer_id;
                    matched_conn = server_peer.conn_id;
                    matched_rx = server_peer.frames_received;
                    matched_tx = server_peer.frames_sent;
                    break;
                }
            }
            std::printf("[DIAG] client %zu local_port=%u same_port_clients=%d local_conn=%08x "
                        "match_id=%u "
                        "match_conn=%08x match_rx=%llu match_tx=%llu state=%s attempts=%u "
                        "rx=%llu tx=%llu\n",
                        i, local_port, same_port_clients,
                        info.empty() ? 0U : info.front().conn_id, matched_id,
                        matched_conn, static_cast<unsigned long long>(matched_rx),
                        static_cast<unsigned long long>(matched_tx),
                        info.empty() ? "none" : go2cpp::bento::PeerStateName(info.front().state),
                        info.empty() ? 0U : info.front().hello_attempts,
                        static_cast<unsigned long long>(own.frames_received),
                        static_cast<unsigned long long>(own.frames_sent));
        }
        return;
    }

    // ---- 10 万帧转发 ----
    const std::size_t rss_connected = RssKb();
    std::vector<std::size_t> sent(peers, 0);
    std::vector<std::uint8_t> payload(kFrameBytes, 0);
    std::size_t send_errors = 0;
    std::size_t would_block = 0;
    const std::uint64_t start = NowNs();
    const auto deadline = Clock::now() + std::chrono::seconds(180);
    // 轮转驱动 1/4 的客户端、每个入队 4 帧。若每轮把 1000 个 Hub 全 poll
    // 一遍，WSL 下 1000 次 poll 就要 ~40ms，测到的是客户端调度开销而不是
    // 服务端转发能力。轮转后每轮 250 次 poll、注入速率仍是 1000 帧/轮。
    std::size_t drive_slice = 0;
    while (Clock::now() < deadline && delivered < total_frames) {
        // 服务端每轮最多处理 256 帧；跑 4 次以匹配 1000 帧/轮的注入速率，
        // 避免把内核接收队列打爆后把丢包误读成实现问题。
        for (int slice = 0; slice < 4; ++slice) {
            server.RunOnce(0);
        }
        for (std::size_t i = drive_slice; i < peers; i += 4) {
            PeerHub& client = *clients[i];
            client.RunOnce(0);
            for (int k = 0; k < 4 && sent[i] < frames_per_peer; ++k) {
                FillPayload(payload, static_cast<std::uint32_t>(i),
                            static_cast<std::uint32_t>(sent[i]), NowNs());
                const SendStatus status =
                    client.TrySendStream(1, 0, payload.data(), payload.size());
                if (status == SendStatus::kOk) {
                    ++sent[i];
                } else if (status == SendStatus::kWouldBlock) {
                    ++would_block;
                    break;
                } else {
                    ++send_errors;
                    break;
                }
            }
        }
        drive_slice = (drive_slice + 1) % 4;
    }
    const double seconds = SecondsSince(start);
    const PeerHubStats hub_stats = server.stats();
    const std::size_t rss_after = RssKb();
    const std::size_t peak = PeakRssKb();

    std::size_t sent_total = 0;
    for (std::size_t value : sent) {
        sent_total += value;
    }
    const double frames_per_second = seconds > 0.0 ? static_cast<double>(delivered) / seconds : 0.0;
    const double mib_per_second = frames_per_second * static_cast<double>(kFrameBytes) / (1 << 20);
    std::printf(
        "[INFO] forward: delivered=%zu/%zu sent=%zu elapsed=%.2fs "
        "%.0f frames/s %.2f MiB/s would_block=%zu send_errors=%zu\n",
        delivered, total_frames, sent_total, seconds, frames_per_second, mib_per_second,
        would_block, send_errors);
    std::printf(
        "[INFO] rss: before=%zuKiB connected=%zuKiB after=%zuKiB peak=%zuKiB fd_before=%d "
        "fd_now=%d\n",
        rss_before, rss_connected, rss_after, peak, fd_before, FdCount());
    std::printf(
        "[INFO] hub: frames_rx=%llu frames_tx=%llu invalid=%llu unknown=%llu timeouts=%llu "
        "send_err=%llu recv_err=%llu\n",
        static_cast<unsigned long long>(hub_stats.frames_received),
        static_cast<unsigned long long>(hub_stats.frames_sent),
        static_cast<unsigned long long>(hub_stats.invalid_frames),
        static_cast<unsigned long long>(hub_stats.unknown_peer_frames),
        static_cast<unsigned long long>(hub_stats.timeouts),
        static_cast<unsigned long long>(hub_stats.send_errors),
        static_cast<unsigned long long>(hub_stats.recv_errors));
    PrintLatency("forward", latencies_us);

    CHECK(delivered == total_frames);
    CHECK(bad_payload == 0);
    CHECK(cross_talk == 0);
    CHECK(out_of_order == 0);
    CHECK(hub_stats.peers_active == peers);
    CHECK(hub_stats.invalid_frames == 0);
    // ---- 空闲成本：1000 个 peer 都 connected 时单次 RunOnce（Tick 为主） ----
    {
        const std::uint64_t idle_start = NowNs();
        constexpr int kIdleRuns = 20;
        for (int i = 0; i < kIdleRuns; ++i) {
            server.RunOnce(0);
        }
        const double server_us = static_cast<double>(NowNs() - idle_start) / kIdleRuns / 1000.0;
        const std::uint64_t client_start = NowNs();
        for (std::size_t i = 0; i < std::min<std::size_t>(peers, 200); ++i) {
            clients[i]->RunOnce(0);
        }
        const double client_us =
            static_cast<double>(NowNs() - client_start) / static_cast<double>(std::min<std::size_t>(peers, 200)) /
            1000.0;
        std::printf("[INFO] idle runonce: server=%.1fus client=%.1fus\n", server_us, client_us);
    }

    // ---- 10 万帧 DATAGRAM 纯转发（无 ACK/窗口限制，反映接收-分发上限） ----
    std::size_t dgram_delivered = 0;
    std::size_t dgram_bad = 0;
    std::size_t dgram_owner_mismatch = 0;
    std::map<std::uint32_t, std::uint32_t> dgram_owner;
    server.on_datagram = [&](std::uint32_t peer_id, std::uint8_t channel,
                             const std::uint8_t* data, std::size_t bytes) {
        if (channel != 1) {
            ++dgram_bad;
            return;
        }
        std::uint32_t owner = 0;
        std::uint32_t seq = 0;
        std::uint64_t send_ns = 0;
        if (!VerifyPayload(data, bytes, &owner, &seq, &send_ns)) {
            ++dgram_bad;
            return;
        }
        const auto bound = dgram_owner.find(peer_id);
        if (bound == dgram_owner.end()) {
            dgram_owner.emplace(peer_id, owner);
        } else if (bound->second != owner) {
            ++dgram_owner_mismatch;
            return;
        }
        ++dgram_delivered;
    };
    std::vector<std::size_t> dgram_sent(peers, 0);
    std::size_t dgram_sent_total = 0;
    std::uint64_t dgram_server_ns = 0;
    std::uint64_t dgram_client_poll_ns = 0;
    std::uint64_t dgram_client_send_ns = 0;
    const std::uint64_t dgram_start = NowNs();
    const auto dgram_deadline = Clock::now() + std::chrono::seconds(60);
    drive_slice = 0;
    while (Clock::now() < dgram_deadline && dgram_delivered < total_frames) {
        // 每轮只让 1/4 的 peer 各发 1 帧（250 帧），服务端立即处理 1 次
        // RunOnce（256 帧）。一次注入 1000 帧会超过内核 rcvbuf（212KB 约
        // 270 个 skb）而丢 70%+，那是驱动方式不是实现丢包。
        std::uint64_t phase_start = NowNs();
        for (int slice = 0; slice < 3; ++slice) {
            server.RunOnce(0);
        }
        dgram_server_ns += NowNs() - phase_start;
        for (std::size_t i = drive_slice; i < peers; i += 4) {
            PeerHub& client = *clients[i];
            phase_start = NowNs();
            client.RunOnce(0);
            dgram_client_poll_ns += NowNs() - phase_start;
            if (dgram_sent[i] >= frames_per_peer) {
                continue;
            }
            FillPayload(payload, static_cast<std::uint32_t>(i),
                        static_cast<std::uint32_t>(dgram_sent[i]), NowNs());
            phase_start = NowNs();
            const bool sent_ok = client.SendDatagram(1, 1, payload.data(), payload.size());
            dgram_client_send_ns += NowNs() - phase_start;
            if (!sent_ok) {
                continue;
            }
            ++dgram_sent[i];
            ++dgram_sent_total;
        }
        drive_slice = (drive_slice + 1) % 4;
    }
    const double dgram_seconds = SecondsSince(dgram_start);
    std::printf("[INFO] datagram breakdown: server=%.2fs client_poll=%.2fs client_send=%.2fs\n",
                static_cast<double>(dgram_server_ns) / 1e9,
                static_cast<double>(dgram_client_poll_ns) / 1e9,
                static_cast<double>(dgram_client_send_ns) / 1e9);
    const double dgram_per_second =
        dgram_seconds > 0.0 ? static_cast<double>(dgram_delivered) / dgram_seconds : 0.0;
    std::printf("[INFO] datagram: delivered=%zu/%zu sent=%zu elapsed=%.2fs %.0f frames/s "
                "%.2f MiB/s bad=%zu owner_mismatch=%zu\n",
                dgram_delivered, total_frames, dgram_sent_total, dgram_seconds, dgram_per_second,
                dgram_per_second * static_cast<double>(kFrameBytes) / (1 << 20), dgram_bad,
                dgram_owner_mismatch);
    CHECK(dgram_bad == 0);
    CHECK(dgram_owner_mismatch == 0);
    // UDP 语义允许丢包；本地回环要求交付率 >= 99% 才说明实现没有系统性丢帧。
    CHECK(dgram_delivered * 100 >= total_frames * 99);

    // 结束时不得有 fd 泄漏：peers 个客户端 socket + 服务端 socket 全部存活。
    CHECK(FdCount() >= static_cast<int>(peers) + 1);
}

// ---------------------------------------------------------------------------
// 场景 2：1000+ 随机连接/断开/回调内删除
// ---------------------------------------------------------------------------

void RunRandomOperations(unsigned seed, int operations) {
    std::printf("\n[random] operations=%d seed=%u\n", operations, seed);
    std::mt19937 rng(seed);
    const int fd_before = FdCount();
    const std::size_t rss_before = RssKb();

    PeerHub server(MakeConfig(64, 1500, 800));
    std::string error;
    CHECK(server.Open(&error));
    if (!server.is_open()) {
        return;
    }
    constexpr int kClients = 8;
    std::vector<std::unique_ptr<PeerHub>> clients;
    for (int i = 0; i < kClients; ++i) {
        clients.push_back(std::make_unique<PeerHub>(
            MakeClientConfig(static_cast<std::size_t>(i), 8, 1500, 800)));
        CHECK(clients.back()->Open(&error));
    }

    std::size_t server_streams = 0;
    std::size_t server_datagrams = 0;
    std::size_t server_disconnects = 0;
    std::size_t callback_removals = 0;
    std::size_t client_streams = 0;

    server.on_stream = [&](std::uint32_t peer_id, std::uint8_t, const std::uint8_t*,
                           std::size_t) {
        ++server_streams;
        // 回调内删除：验证 PEB-01 的延迟销毁路径反复执行不产生 UAF。
        if ((rng() % 8U) == 0U) {
            ++callback_removals;
            (void)server.RemovePeer(peer_id);
        }
    };
    server.on_datagram = [&](std::uint32_t peer_id, std::uint8_t, const std::uint8_t*,
                             std::size_t) {
        ++server_datagrams;
        if ((rng() % 16U) == 0U) {
            ++callback_removals;
            (void)server.RemovePeer(peer_id);
        }
    };
    server.on_peer_connected = [&](std::uint32_t peer_id) {
        if ((rng() % 32U) == 0U) {
            ++callback_removals;
            (void)server.RemovePeer(peer_id);
        }
    };
    server.on_peer_disconnected = [&](std::uint32_t, const char*) { ++server_disconnects; };
    for (auto& client : clients) {
        client->on_stream = [&](std::uint32_t peer_id, std::uint8_t, const std::uint8_t*,
                                std::size_t) {
            ++client_streams;
            if ((rng() % 16U) == 0U) {
                (void)client->RemovePeer(peer_id);
            }
        };
    }

    std::vector<std::uint8_t> payload(kFrameBytes, 0x5A);
    std::size_t client_opens = 0;
    std::size_t client_closes = 0;
    std::size_t adds = 0;
    std::size_t removes = 0;
    std::size_t connects = 0;
    std::size_t sends = 0;
    std::size_t send_failures = 0;

    for (int step = 0; step < operations; ++step) {
        PeerHub& client = *clients[static_cast<std::size_t>(rng() % kClients)];
        const std::uint32_t action = rng() % 100U;
        if (action < 18U) {
            if (!client.is_open()) {
                (void)client.Open(&error);
                ++client_opens;
            } else if (!client.HasPeer(1)) {
                if (client.AddPeer(1, "127.0.0.1", server.local_port(), &error)) {
                    ++adds;
                }
            }
        } else if (action < 36U) {
            if (client.RemovePeer(1)) {
                ++removes;
            }
        } else if (action < 50U) {
            if (client.Connect(1)) {
                ++connects;
            }
        } else if (action < 70U) {
            FillPayload(payload, static_cast<std::uint32_t>(step & 0xFF),
                        static_cast<std::uint32_t>(step), NowNs());
            for (int k = 0; k < 4; ++k) {
                const SendStatus status =
                    client.TrySendStream(1, 0, payload.data(), payload.size());
                if (status == SendStatus::kOk) {
                    ++sends;
                } else if (status == SendStatus::kError) {
                    ++send_failures;
                    break;
                }
                (void)client.SendDatagram(1, 1, payload.data(), payload.size());
            }
        } else if (action < 72U) {
            // 整个 Hub 关闭后重新 Open：验证 socket/peer 表能完整重建。
            client.Close();
            ++client_closes;
        } else {
            // 驱动网络（包括服务端）。
            for (int slice = 0; slice < 3; ++slice) {
                server.RunOnce(0);
                for (auto& hub : clients) {
                    hub->RunOnce(0);
                }
            }
        }
        if ((step % 25) == 0) {
            CheckHubInvariants(server, "random/server");
            for (int i = 0; i < kClients; ++i) {
                CheckHubInvariants(*clients[static_cast<std::size_t>(i)], "random/client");
            }
        }
    }

    // 收尾：所有客户端断开，服务端应能被回收或至少状态一致。
    for (auto& client : clients) {
        client->Close();
    }
    // 服务端 peer_timeout=1500ms：留足回收窗口再断言清空。
    PumpFor({&server}, 3000);
    CHECK(server.PeerIds().empty());
    CheckHubInvariants(server, "random/final");
    server.Close();
    CHECK(server.PeerIds().empty());
    clients.clear();
    const int fd_after = FdCount();
    std::printf(
        "[INFO] random: streams=%zu datagrams=%zu client_streams=%zu cb_removals=%zu "
        "adds=%zu removes=%zu connects=%zu sends=%zu send_fail=%zu opens=%zu closes=%zu "
        "disconnects=%zu\n",
        server_streams, server_datagrams, client_streams, callback_removals, adds, removes,
        connects, sends, send_failures, client_opens, client_closes, server_disconnects);
    std::printf("[INFO] random: rss=%zuKiB (before=%zuKiB) fd=%d (before=%d)\n", RssKb(),
                rss_before, fd_after, fd_before);
    CHECK(fd_after == fd_before);
}

// ---------------------------------------------------------------------------
// 场景 3：每 peer 独立 PSK（一个 Hub 同时连 4 个不同 PSK 的服务端）
// ---------------------------------------------------------------------------

std::vector<std::uint8_t> MakePsk(std::uint8_t seed) {
    std::vector<std::uint8_t> key(32, 0);
    for (std::size_t i = 0; i < key.size(); ++i) {
        key[i] = static_cast<std::uint8_t>((seed + i * 7U) & 0xFFU);
    }
    return key;
}

PeerHubConfig MakeSecureConfig(const std::vector<std::uint8_t>& psk, std::size_t max_peers) {
    PeerHubConfig config = MakeConfig(max_peers, 4000, 2000);
    config.pre_shared_key = psk;
    return config;
}

void RunPerPeerPskScenario() {
    std::printf("\n[scenario] per-peer PSK x4 servers\n");
    constexpr int kServers = 4;
    std::vector<std::unique_ptr<PeerHub>> servers;
    std::vector<std::vector<std::uint8_t>> psks;
    std::vector<std::size_t> received(static_cast<std::size_t>(kServers), 0);
    std::vector<std::size_t> replied(static_cast<std::size_t>(kServers), 0);
    for (int i = 0; i < kServers; ++i) {
        psks.push_back(MakePsk(static_cast<std::uint8_t>(0x10 + i * 3)));
        servers.push_back(std::make_unique<PeerHub>(MakeSecureConfig(psks.back(), 8)));
        CHECK(servers.back()->Open());
        PeerHub& server = *servers.back();
        server.on_stream = [&, i](std::uint32_t peer_id, std::uint8_t, const std::uint8_t* data,
                                  std::size_t bytes) {
            std::uint32_t owner = 0;
            std::uint32_t seq = 0;
            std::uint64_t send_ns = 0;
            if (!VerifyPayload(data, bytes, &owner, &seq, &send_ns) ||
                owner != static_cast<std::uint32_t>(i)) {
                return;
            }
            ++received[static_cast<std::size_t>(i)];
            std::vector<std::uint8_t> reply(kFrameBytes, 0);
            FillPayload(reply, static_cast<std::uint32_t>(i), seq, NowNs());
            (void)server.SendStream(peer_id, 1, reply.data(), reply.size());
        };
    }

    PeerHubConfig psk_client_config = MakeConfig(8, 4000, 2000);
    psk_client_config.bind_address = ClientBindAddress(0);
    PeerHub client(psk_client_config);
    CHECK(client.Open());
    for (int i = 0; i < kServers; ++i) {
        const std::uint32_t peer_id = static_cast<std::uint32_t>(i + 1);
        CHECK(client.AddPeer(peer_id, "127.0.0.1",
                             servers[static_cast<std::size_t>(i)]->local_port()));
        CHECK(client.SetPeerPreSharedKey(peer_id, psks[static_cast<std::size_t>(i)].data(),
                                         psks[static_cast<std::size_t>(i)].size()));
        CHECK(client.peer_secure(peer_id));
    }
    client.on_stream = [&](std::uint32_t peer_id, std::uint8_t, const std::uint8_t*, std::size_t) {
        if (peer_id >= 1 && peer_id <= static_cast<std::uint32_t>(kServers)) {
            ++replied[peer_id - 1];
        }
    };

    std::vector<PeerHub*> hubs{&client};
    for (auto& server : servers) {
        hubs.push_back(server.get());
    }
    CHECK(PumpUntil(hubs,
                    [&]() {
                        for (int i = 0; i < kServers; ++i) {
                            if (!client.peer_connected(static_cast<std::uint32_t>(i + 1))) {
                                return false;
                            }
                            if (servers[static_cast<std::size_t>(i)]->stats().peers_active != 1) {
                                return false;
                            }
                        }
                        return true;
                    },
                    15000));
    for (int i = 0; i < kServers; ++i) {
        CHECK(client.peer_secure_ready(static_cast<std::uint32_t>(i + 1)));
        CHECK(servers[static_cast<std::size_t>(i)]->secure_stats().handshakes == 1);
        CHECK(servers[static_cast<std::size_t>(i)]->secure_stats().auth_failures == 0);
    }

    constexpr std::uint32_t kMessages = 100;
    for (std::uint32_t seq = 0; seq < kMessages; ++seq) {
        for (int i = 0; i < kServers; ++i) {
            std::vector<std::uint8_t> payload(kFrameBytes, 0);
            FillPayload(payload, static_cast<std::uint32_t>(i), seq, NowNs());
            CHECK(client.SendStream(static_cast<std::uint32_t>(i + 1), 0, payload.data(),
                                    payload.size()));
        }
        if ((seq % 20) == 19) {
            PumpFor(hubs, 5);
        }
    }
    CHECK(PumpUntil(hubs,
                    [&]() {
                        for (int i = 0; i < kServers; ++i) {
                            if (received[static_cast<std::size_t>(i)] < kMessages ||
                                replied[static_cast<std::size_t>(i)] < kMessages) {
                                return false;
                            }
                        }
                        return true;
                    },
                    20000));
    for (int i = 0; i < kServers; ++i) {
        CHECK(received[static_cast<std::size_t>(i)] == kMessages);
        CHECK(replied[static_cast<std::size_t>(i)] == kMessages);
        const auto peers = servers[static_cast<std::size_t>(i)]->Peers();
        CheckEqualSize(peers.size(), 1);
        if (!peers.empty()) {
            CHECK(peers.front().secure);
            CHECK(peers.front().secure_ready);
            CHECK(peers.front().conn_id != 0);
        }
    }

    // 第 5 个服务端用错误 PSK：只影响该 peer，其余 4 条继续可用。
    const std::vector<std::uint8_t> wrong_psk = MakePsk(0xEE);
    PeerHub wrong_server(MakeSecureConfig(MakePsk(0x77), 8));
    CHECK(wrong_server.Open());
    CHECK(client.AddPeer(5, "127.0.0.1", wrong_server.local_port()));
    CHECK(client.SetPeerPreSharedKey(5, wrong_psk.data(), wrong_psk.size()));
    std::vector<PeerHub*> hubs_with_wrong = hubs;
    hubs_with_wrong.push_back(&wrong_server);
    // 客户端 handshake_timeout_ms=2000：等它自己判定握手失败，而不是固定
    // 1200ms（否则 peer 还停在 kConnecting，断言对象不对）。
    CHECK(PumpUntil(
        hubs_with_wrong, [&]() { return client.peer_state(5) == PeerState::kTimedOut; }, 5000));
    CHECK(client.peer_state(5) == PeerState::kTimedOut);
    CHECK(wrong_server.secure_stats().handshakes == 0);
    CHECK(wrong_server.secure_stats().auth_failures > 0);
    for (int i = 0; i < kServers; ++i) {
        CHECK(client.peer_connected(static_cast<std::uint32_t>(i + 1)));
    }
    // 4 条正常会话仍能转发。
    const std::size_t before = received[0];
    std::vector<std::uint8_t> payload(kFrameBytes, 0);
    FillPayload(payload, 0, 0xFFFFU, NowNs());
    CHECK(client.SendStream(1, 0, payload.data(), payload.size()));
    CHECK(PumpUntil(hubs_with_wrong, [&]() { return received[0] > before; }, 5000));
    std::printf("[INFO] per_peer_psk: ok=%d wrong_psk_auth_fail=%llu\n", kServers,
                static_cast<unsigned long long>(wrong_server.secure_stats().auth_failures));
}

// ---------------------------------------------------------------------------
// 场景 4：超时回收释放 max_peers
// ---------------------------------------------------------------------------

void RunTimeoutReclaimScenario() {
    std::printf("\n[scenario] timeout reclaim frees max_peers\n");
    constexpr std::size_t kMaxPeers = 32;
    PeerHub server(MakeConfig(kMaxPeers, 400, 600));
    CHECK(server.Open());
    std::size_t server_streams = 0;
    server.on_stream = [&](std::uint32_t, std::uint8_t, const std::uint8_t*, std::size_t) {
        ++server_streams;
    };

    std::vector<std::unique_ptr<PeerHub>> clients;
    for (std::size_t i = 0; i < kMaxPeers; ++i) {
        clients.push_back(std::make_unique<PeerHub>(MakeClientConfig(i, 8, 3000, 600)));
        CHECK(clients.back()->Open());
        CHECK(clients.back()->AddPeer(1, "127.0.0.1", server.local_port()));
    }
    std::vector<PeerHub*> hubs{&server};
    for (auto& client : clients) {
        hubs.push_back(client.get());
    }
    CHECK(PumpUntil(hubs, [&]() { return server.stats().peers_active == kMaxPeers; }, 20000));
    CheckEqualSize(server.PeerIds().size(), kMaxPeers);
    const std::uint64_t rejected_before = server.stats().peers_rejected;

    // 第 33 个连接：超上限被拒，客户端握手超时。
    PeerHubConfig overflow_config = MakeConfig(8, 3000, 400);
    overflow_config.bind_address = ClientBindAddress(5000);
    PeerHub overflow(overflow_config);
    CHECK(overflow.Open());
    CHECK(overflow.AddPeer(1, "127.0.0.1", server.local_port()));
    // 必须同时驱动 32 个存活客户端：服务端 peer_timeout=400ms，只驱动
    // server+overflow 会把满员的 32 个 peer 全部回收，断言「满员被拒」失效。
    std::vector<PeerHub*> hubs_with_overflow = hubs;
    hubs_with_overflow.push_back(&overflow);
    PumpFor(hubs_with_overflow, 900);
    CHECK(server.stats().peers_rejected > rejected_before);
    CHECK(server.stats().peers_active == kMaxPeers);
    CHECK(overflow.peer_state(1) == PeerState::kTimedOut);
    std::printf("[INFO] reclaim: rejected=%llu overflow_state=%s\n",
                static_cast<unsigned long long>(server.stats().peers_rejected),
                go2cpp::bento::PeerStateName(overflow.peer_state(1)));

    // 32 个被动 peer 全部静默：超时后被回收，PeerIds 清空。
    const std::uint64_t timeouts_before = server.stats().timeouts;
    CHECK(PumpUntil({&server}, [&]() { return server.PeerIds().empty(); }, 8000));
    CHECK(server.stats().timeouts >= timeouts_before + kMaxPeers);
    CHECK(server.stats().peers_active == 0);

    // 配额已释放：第 33 个 Connect 重连成功，且不再计 peers_rejected。
    const std::uint64_t rejected_after_timeout = server.stats().peers_rejected;
    CHECK(overflow.Connect(1));
    CHECK(PumpUntil(
        {&server, &overflow},
        [&]() { return overflow.peer_connected(1) && server.stats().peers_active == 1; }, 8000));
    CHECK(server.stats().peers_rejected == rejected_after_timeout);
    std::vector<std::uint8_t> payload(kFrameBytes, 0);
    FillPayload(payload, 0, 0, NowNs());
    CHECK(overflow.SendStream(1, 0, payload.data(), payload.size()));
    CHECK(PumpUntil({&server, &overflow}, [&]() { return server_streams >= 1; }, 5000));
    std::printf("[INFO] reclaim: reclaimed=%zu active_after_reconnect=%llu\n", kMaxPeers,
                static_cast<unsigned long long>(server.stats().peers_active));
}

// ---------------------------------------------------------------------------
// 场景 5：回调内 RemovePeer / Close / 重连（P0-01 不回归）
// ---------------------------------------------------------------------------

void RunCallbackRemovalScenario(int iterations) {
    std::printf("\n[scenario] callback removal/close x%d\n", iterations);

    // 5a. on_stream 内 RemovePeer（重复删除幂等）+ 回调内 SendStream。
    {
        int rounds = 0;
        for (int i = 0; i < iterations; ++i) {
            PeerHub server(MakeConfig(8, 2000, 2000));
            PeerHub client(MakeConfig(8, 2000, 2000));
            CHECK(server.Open());
            CHECK(client.Open());
            int streams = 0;
            bool reconnect_attempt = true;
            server.on_stream = [&](std::uint32_t peer_id, std::uint8_t, const std::uint8_t*,
                                   std::size_t) {
                ++streams;
                // 回调内发送（recursive_mutex 重入）必须成功。
                const std::uint8_t reply[8] = {1, 2, 3, 4, 5, 6, 7, 8};
                CHECK(server.SendStream(peer_id, 1, reply, sizeof(reply)));
                CHECK(server.RemovePeer(peer_id));
                // 重复删除必须幂等。
                CHECK(!server.RemovePeer(peer_id));
                // 删除后再发送必须被拒绝，而不是 UAF。
                CHECK(!server.SendStream(peer_id, 1, reply, sizeof(reply)));
                // 硬删除后不允许在回调里“复活”。
                reconnect_attempt = server.Connect(peer_id);
            };
            CHECK(client.AddPeer(1, "127.0.0.1", server.local_port()));
            CHECK(PumpUntil({&server, &client},
                            [&]() { return client.peer_connected(1) && server.peer_connected(1); },
                            5000));
            std::vector<std::uint8_t> payload(kFrameBytes, 0);
            FillPayload(payload, 1, 1, NowNs());
            CHECK(client.SendStream(1, 0, payload.data(), payload.size()));
            CHECK(PumpUntil({&server, &client}, [&]() { return streams >= 1; }, 5000));
            CHECK(streams == 1);
            CHECK(!reconnect_attempt);
            CHECK(!server.HasPeer(1));
            CHECK(server.PeerIds().empty());
            ++rounds;
        }
        CHECK(rounds == iterations);
    }

    // 5b. on_stream 内 Close（整表清空推迟到安全点）。
    {
        int rounds = 0;
        for (int i = 0; i < iterations / 2; ++i) {
            PeerHub server(MakeConfig(8, 2000, 2000));
            PeerHub client(MakeConfig(8, 2000, 2000));
            CHECK(server.Open());
            CHECK(client.Open());
            int streams = 0;
            server.on_stream = [&](std::uint32_t, std::uint8_t, const std::uint8_t*, std::size_t) {
                ++streams;
                server.Close();
                CHECK(!server.is_open());
            };
            CHECK(client.AddPeer(1, "127.0.0.1", server.local_port()));
            CHECK(PumpUntil({&server, &client},
                            [&]() { return client.peer_connected(1) && server.peer_connected(1); },
                            5000));
            std::vector<std::uint8_t> payload(kFrameBytes, 0);
            FillPayload(payload, 2, 2, NowNs());
            CHECK(client.SendStream(1, 0, payload.data(), payload.size()));
            CHECK(PumpUntil({&server, &client}, [&]() { return streams >= 1; }, 5000));
            CHECK(streams == 1);
            CHECK(server.PeerIds().empty());
            // Close 之后继续驱动客户端不应崩溃。
            PumpFor({&client}, 20);
            ++rounds;
        }
        CHECK(rounds == iterations / 2);
    }

    // 5c. on_peer_connected 内 RemovePeer / on_peer_disconnected 内删除自己。
    {
        int rounds = 0;
        for (int i = 0; i < iterations / 2; ++i) {
            PeerHub server(MakeConfig(8, 2000, 2000));
            PeerHub client(MakeConfig(8, 2000, 2000));
            CHECK(server.Open());
            CHECK(client.Open());
            int connected = 0;
            int disconnected = 0;
            server.on_peer_connected = [&](std::uint32_t peer_id) {
                ++connected;
                (void)server.RemovePeer(peer_id);
            };
            server.on_peer_disconnected = [&](std::uint32_t peer_id, const char*) {
                ++disconnected;
                (void)server.RemovePeer(peer_id);  // 同一 peer 的二次删除
            };
            CHECK(client.AddPeer(1, "127.0.0.1", server.local_port()));
            PumpFor({&server, &client}, 300);
            CHECK(connected == 1);
            CHECK(disconnected == 1);
            CHECK(server.PeerIds().empty());
            ++rounds;
        }
        CHECK(rounds == iterations / 2);
    }

    // 5d. 主动 peer：on_peer_disconnected 里 Connect（超时软回收可取消）。
    {
        PeerHub server(MakeConfig(8, 3000, 2000));
        PeerHub client(MakeConfig(8, 120, 2000));
        CHECK(server.Open());
        CHECK(client.Open());
        bool reconnect_in_callback = false;
        client.on_peer_disconnected = [&](std::uint32_t peer_id, const char* reason) {
            if (std::strcmp(reason, "peer timeout") == 0) {
                reconnect_in_callback = client.Connect(peer_id);
            }
        };
        CHECK(client.AddPeer(1, "127.0.0.1", server.local_port()));
        CHECK(PumpUntil({&server, &client},
                        [&]() { return client.peer_connected(1) && server.peer_connected(1); },
                        5000));
        // 只驱动客户端（服务端不发心跳）→ 客户端超时 → 回调里立即重连。
        PumpFor({&client}, 400);
        CHECK(reconnect_in_callback);
        CHECK(PumpUntil({&server, &client}, [&]() { return client.peer_connected(1); }, 5000));
        const std::vector<std::uint32_t> server_ids = server.PeerIds();
        CHECK(server_ids.size() == 1);
        if (!server_ids.empty()) {
            CHECK(server.peer_connected(server_ids.front()));
        }
    }
}

// ---------------------------------------------------------------------------
// 场景 6：反复重连的 session_id/conn_id 同步
// ---------------------------------------------------------------------------

void RunReconnectScenario(bool secure, int iterations) {
    std::printf("\n[scenario] reconnect %s x%d\n", secure ? "secure" : "plain", iterations);
    const std::vector<std::uint8_t> psk = MakePsk(0x5A);
    PeerHubConfig server_config =
        secure ? MakeSecureConfig(psk, 8) : MakeConfig(8, 3000, 2000);
    server_config.peer_timeout_ms = 3000;
    PeerHub server(server_config);
    PeerHubConfig client_config = secure ? MakeSecureConfig(psk, 8) : MakeConfig(8, 2000, 2000);
    client_config.bind_address = ClientBindAddress(0);
    client_config.peer_timeout_ms = 120;
    client_config.heartbeat_interval_ms = 30;
    PeerHub client(client_config);
    std::string error;
    CHECK(server.Open(&error));
    CHECK(client.Open(&error));

    std::size_t server_streams = 0;
    std::size_t client_streams = 0;
    server.on_stream = [&](std::uint32_t, std::uint8_t, const std::uint8_t*, std::size_t) {
        ++server_streams;
    };
    client.on_stream = [&](std::uint32_t, std::uint8_t, const std::uint8_t*, std::size_t) {
        ++client_streams;
    };
    CHECK(client.AddPeer(1, "127.0.0.1", server.local_port(), &error));
    CHECK(PumpUntil({&server, &client},
                    [&]() {
                        return client.peer_connected(1) &&
                               (!secure || client.peer_secure_ready(1)) &&
                               server.stats().peers_active == 1;
                    },
                    8000));

    int reconnects = 0;
    for (int i = 0; i < iterations; ++i) {
        const std::vector<PeerInfo> before_peers = client.Peers();
        const std::uint32_t old_conn_id = before_peers.empty() ? 0 : before_peers.front().conn_id;
        // 只驱动客户端：收不到服务端心跳 → 客户端超时（服务端 peer 远未超时）。
        PumpFor({&client}, 400);
        CHECK(client.peer_state(1) == PeerState::kTimedOut);
        CHECK(server.stats().peers_active == 1);
        CHECK(client.Connect(1));
        CHECK(PumpUntil({&server, &client},
                        [&]() {
                            return client.peer_connected(1) &&
                                   (!secure || client.peer_secure_ready(1)) &&
                                   server.stats().peers_active == 1;
                        },
                        8000));
        const std::vector<PeerInfo> client_peers = client.Peers();
        CHECK(!client_peers.empty());
        if (client_peers.empty()) {
            break;
        }
        const std::uint32_t new_conn_id = client_peers.front().conn_id;
        ++g_checks;
        if (new_conn_id == old_conn_id) {
            std::fprintf(stderr, "[FAIL] reconnect %d: conn_id not rotated (%08x)\n", i,
                         new_conn_id);
            ++g_failures;
        }
        const std::vector<std::uint32_t> server_ids = server.PeerIds();
        CheckEqualSize(server_ids.size(), 1);
        if (server_ids.size() == 1) {
            const std::vector<PeerInfo> server_peers = server.Peers();
            CHECK(!server_peers.empty());
            if (!server_peers.empty()) {
                CHECK(server_peers.front().conn_id == new_conn_id);
                if (server_peers.front().conn_id != new_conn_id) {
                    std::fprintf(stderr,
                                 "[FAIL] reconnect %d: server conn=%08x client conn=%08x\n", i,
                                 server_peers.front().conn_id, new_conn_id);
                }
            }
            const std::uint64_t stale_before =
                server.reliable_stats(server_ids.front(), 0).stale_dropped;
            const std::size_t server_before = server_streams;
            const std::size_t client_before = client_streams;
            std::vector<std::uint8_t> payload(kFrameBytes, 0);
            FillPayload(payload, static_cast<std::uint32_t>(i), 0, NowNs());
            CHECK(client.SendStream(1, 0, payload.data(), payload.size()));
            CHECK(server.SendStream(server_ids.front(), 0, payload.data(), payload.size()));
            CHECK(PumpUntil(
                {&server, &client},
                [&]() {
                    return server_streams > server_before && client_streams > client_before;
                },
                5000));
            CHECK(server.reliable_stats(server_ids.front(), 0).stale_dropped == stale_before);
        }
        ++reconnects;
    }
    CHECK(reconnects == iterations);
    std::printf(
        "[INFO] reconnect %s: done=%d server_rx=%zu client_rx=%zu conn_id_conflicts=%llu\n",
        secure ? "secure" : "plain", reconnects, server_streams, client_streams,
        static_cast<unsigned long long>(server.stats().conn_id_conflicts));
}

// ---------------------------------------------------------------------------
// 场景 7：60s 长稳（随机 peer 出入）
// ---------------------------------------------------------------------------

void RunSoakScenario(int seconds, unsigned seed) {
    std::printf("\n[scenario] soak %ds seed=%u\n", seconds, seed);
    std::mt19937 rng(seed);
    constexpr int kClients = 8;
    PeerHub server(MakeConfig(64, 1500, 600));
    std::string error;
    CHECK(server.Open(&error));
    if (!server.is_open()) {
        return;
    }
    std::vector<std::unique_ptr<PeerHub>> clients;
    for (int i = 0; i < kClients; ++i) {
        clients.push_back(std::make_unique<PeerHub>(
            MakeClientConfig(static_cast<std::size_t>(i), 8, 1500, 600)));
        CHECK(clients.back()->Open(&error));
    }
    std::size_t server_streams = 0;
    std::size_t server_datagrams = 0;
    server.on_stream = [&](std::uint32_t, std::uint8_t, const std::uint8_t*, std::size_t) {
        ++server_streams;
    };
    server.on_datagram = [&](std::uint32_t, std::uint8_t, const std::uint8_t*, std::size_t) {
        ++server_datagrams;
    };

    const auto start = Clock::now();
    const auto deadline = start + std::chrono::seconds(std::max(seconds, 1));
    std::size_t rss_baseline = 0;
    int fd_baseline = 0;
    std::uint64_t next_operation = 0;
    std::uint64_t next_sample = 0;
    std::vector<std::uint8_t> payload(kFrameBytes, 0);
    std::size_t adds = 0;
    std::size_t removes = 0;
    std::size_t connects = 0;
    std::size_t sends = 0;
    std::uint64_t tick = 0;
    std::uint64_t last_report = 0;
    while (Clock::now() < deadline) {
        const std::uint64_t now_ns = NowNs();
        if (now_ns >= next_operation) {
            next_operation = now_ns + 30ULL * 1000ULL * 1000ULL;  // 30ms 一次操作
            const int index = static_cast<int>(rng() % kClients);
            PeerHub& client = *clients[static_cast<std::size_t>(index)];
            const std::uint32_t action = rng() % 100U;
            if (!client.is_open()) {
                (void)client.Open(&error);
            } else if (!client.HasPeer(1)) {
                if (client.AddPeer(1, "127.0.0.1", server.local_port(), &error)) {
                    ++adds;
                }
            } else if (action < 35U) {
                if (client.RemovePeer(1)) {
                    ++removes;
                }
            } else if (action < 55U) {
                if (client.Connect(1)) {
                    ++connects;
                }
            } else {
                for (int k = 0; k < 8; ++k) {
                    FillPayload(payload, static_cast<std::uint32_t>(index),
                                static_cast<std::uint32_t>(tick), NowNs());
                    if (client.SendStream(1, 0, payload.data(), payload.size())) {
                        ++sends;
                    }
                    (void)client.SendDatagram(1, 1, payload.data(), payload.size());
                }
            }
        }
        server.RunOnce(0);
        for (auto& client : clients) {
            client->RunOnce(0);
        }
        ++tick;
        if (now_ns >= next_sample) {
            next_sample = now_ns + 5ULL * 1000ULL * 1000ULL * 1000ULL;
            const PeerHubStats stats = server.stats();
            if (rss_baseline == 0) {
                // 前 5s 作为预热基线：std::map/vector 的分配器尚未稳定。
                rss_baseline = RssKb();
                fd_baseline = FdCount();
            }
            std::printf(
                "[INFO] soak t=%llus active=%llu peers=%zu rss=%zuKiB fd=%d timeouts=%llu "
                "unknown=%llu invalid=%llu rx=%zu\n",
                static_cast<unsigned long long>(last_report + 5),
                static_cast<unsigned long long>(stats.peers_active), server.PeerIds().size(),
                RssKb(), FdCount(), static_cast<unsigned long long>(stats.timeouts),
                static_cast<unsigned long long>(stats.unknown_peer_frames),
                static_cast<unsigned long long>(stats.invalid_frames), server_streams);
            last_report += 5;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    const std::size_t rss_end = RssKb();
    const int fd_end = FdCount();
    const PeerHubStats stats = server.stats();
    std::printf(
        "[INFO] soak done: adds=%zu removes=%zu connects=%zu sends=%zu rx=%zu dgram=%zu "
        "rss=%zuKiB (base=%zuKiB) fd=%d (base=%d)\n",
        adds, removes, connects, sends, server_streams, server_datagrams, rss_end, rss_baseline,
        fd_end, fd_baseline);
    std::printf("[INFO] soak stats: timeouts=%llu rejected=%llu unknown=%llu invalid=%llu\n",
                static_cast<unsigned long long>(stats.timeouts),
                static_cast<unsigned long long>(stats.peers_rejected),
                static_cast<unsigned long long>(stats.unknown_peer_frames),
                static_cast<unsigned long long>(stats.invalid_frames));
    CheckHubInvariants(server, "soak/server");
    // 内存与 fd 不得随 churn 单调增长（阈值放宽到 32MiB 以吸收分配器保留）。
    ++g_checks;
    if (rss_baseline != 0 && rss_end > rss_baseline + 32U * 1024U) {
        std::fprintf(stderr, "[FAIL] soak: rss grew %zuKiB -> %zuKiB\n", rss_baseline, rss_end);
        ++g_failures;
    }
    CHECK(fd_end == fd_baseline);
    CHECK(server_streams > 0);

    // 收尾：关闭客户端，服务端超时回收全部 peer。
    for (auto& client : clients) {
        client->Close();
    }
    CHECK(PumpUntil({&server}, [&]() { return server.PeerIds().empty(); }, 8000));
    CHECK(server.stats().peers_active == 0);
    server.Close();
    clients.clear();
}

// ---------------------------------------------------------------------------
// 场景 8：重复建连/销毁的泄漏检查（ASan/valgrind 入口）
// ---------------------------------------------------------------------------

void RunLeakLoop(int rounds, std::size_t peers) {
    std::printf("\n[leaks] rounds=%d peers=%zu\n", rounds, peers);
    const int fd_before = FdCount();
    std::size_t rss_after_warmup = 0;
    int successful_rounds = 0;
    for (int round = 0; round < rounds; ++round) {
        {
            PeerHub server(MakeConfig(peers + 8, 1000, 1000));
            std::string error;
            CHECK(server.Open(&error));
            std::vector<std::unique_ptr<PeerHub>> clients;
            for (std::size_t i = 0; i < peers; ++i) {
                clients.push_back(std::make_unique<PeerHub>(MakeClientConfig(i, 8, 1000, 1000)));
                CHECK(clients.back()->Open(&error));
                CHECK(clients.back()->AddPeer(1, "127.0.0.1", server.local_port(), &error));
            }
            std::vector<PeerHub*> hubs{&server};
            for (auto& client : clients) {
                hubs.push_back(client.get());
            }
            CHECK(PumpUntil(hubs, [&]() { return server.stats().peers_active == peers; }, 10000));
            std::vector<std::uint8_t> payload(kFrameBytes, 0);
            for (std::size_t i = 0; i < peers; ++i) {
                FillPayload(payload, static_cast<std::uint32_t>(i), 0, NowNs());
                CHECK(clients[i]->SendStream(1, 0, payload.data(), payload.size()));
            }
            PumpUntil(hubs, [&]() { return server.stats().frames_received > 0; }, 2000);
            // 两种销毁路径：RemovePeer 与 Close。
            for (std::size_t i = 0; i < peers; i += 2) {
                (void)clients[i]->RemovePeer(1);
            }
            for (std::size_t i = 1; i < peers; i += 2) {
                (void)clients[i]->Close();
            }
            PumpFor(hubs, 20);
        }
        if (round == 2) {
            rss_after_warmup = RssKb();
        }
        ++successful_rounds;
    }
    const int fd_after = FdCount();
    const std::size_t rss_end = RssKb();
    std::printf("[INFO] leaks: rounds=%d rss=%zuKiB (warmup=%zuKiB) fd=%d (before=%d)\n",
                successful_rounds, rss_end, rss_after_warmup, fd_after, fd_before);
    CHECK(fd_after == fd_before);
    ++g_checks;
    if (rss_after_warmup != 0 && rss_end > rss_after_warmup + 16U * 1024U) {
        std::fprintf(stderr, "[FAIL] leaks: rss grew %zuKiB -> %zuKiB\n", rss_after_warmup,
                     rss_end);
        ++g_failures;
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::string mode = "all";
    std::size_t peers = 1000;
    std::size_t frames = 100;
    int operations = 1500;
    int seconds = 60;
    int rounds = 50;
    unsigned seed = 20261029U;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto next = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "[FAIL] %s 需要参数\n", name);
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg.rfind("--mode=", 0) == 0) {
            mode = arg.substr(7);
        } else if (arg == "--mode") {
            mode = next("--mode");
        } else if (arg.rfind("--peers=", 0) == 0) {
            peers = static_cast<std::size_t>(std::strtoull(arg.c_str() + 8, nullptr, 10));
        } else if (arg == "--peers") {
            peers = static_cast<std::size_t>(std::strtoull(next("--peers").c_str(), nullptr, 10));
        } else if (arg.rfind("--frames=", 0) == 0) {
            frames = static_cast<std::size_t>(std::strtoull(arg.c_str() + 9, nullptr, 10));
        } else if (arg == "--frames") {
            frames = static_cast<std::size_t>(std::strtoull(next("--frames").c_str(), nullptr, 10));
        } else if (arg.rfind("--ops=", 0) == 0) {
            operations = std::atoi(arg.c_str() + 6);
        } else if (arg == "--ops") {
            operations = std::atoi(next("--ops").c_str());
        } else if (arg.rfind("--seconds=", 0) == 0) {
            seconds = std::atoi(arg.c_str() + 10);
        } else if (arg == "--seconds") {
            seconds = std::atoi(next("--seconds").c_str());
        } else if (arg.rfind("--rounds=", 0) == 0) {
            rounds = std::atoi(arg.c_str() + 9);
        } else if (arg == "--rounds") {
            rounds = std::atoi(next("--rounds").c_str());
        } else if (arg.rfind("--seed=", 0) == 0) {
            seed = static_cast<unsigned>(std::strtoul(arg.c_str() + 7, nullptr, 10));
        } else if (arg == "--seed") {
            seed = static_cast<unsigned>(std::strtoul(next("--seed").c_str(), nullptr, 10));
        } else {
            std::fprintf(stderr, "unknown arg: %s\n", arg.c_str());
            return 2;
        }
    }
    if (mode != "all" && mode != "bench" && mode != "random" && mode != "scenarios" &&
        mode != "soak" && mode != "leaks") {
        std::fprintf(stderr, "unknown mode: %s\n", mode.c_str());
        return 2;
    }
    if (peers == 0) {
        peers = 1;
    }
    std::printf("[INFO] audit_peer_hub_stress mode=%s peers=%zu frames=%zu ops=%d seed=%u\n",
                mode.c_str(), peers, frames, operations, seed);
    std::printf("[INFO] RSS=%zuKiB fd=%d\n", RssKb(), FdCount());
    const std::uint64_t begin = NowNs();

    if (mode == "bench" || mode == "all") {
        RunBenchThousandPeers(peers, frames);
    }
    if (mode == "random" || mode == "all") {
        RunRandomOperations(seed, operations);
    }
    if (mode == "scenarios" || mode == "all") {
        RunPerPeerPskScenario();
        RunTimeoutReclaimScenario();
        RunCallbackRemovalScenario(120);
        RunReconnectScenario(false, 10);
        RunReconnectScenario(true, 6);
        if (mode == "all" && seconds > 0) {
            RunSoakScenario(seconds, seed);
        }
    }
    if (mode == "soak") {
        RunSoakScenario(seconds, seed);
    }
    if (mode == "leaks") {
        RunLeakLoop(rounds, peers);
    }

    const double elapsed = SecondsSince(begin);
    std::printf("\n[SUMMARY] checks=%ld failures=%ld elapsed=%.2fs peak_rss=%zuKiB\n", g_checks,
                g_failures, elapsed, PeakRssKb());
    if (g_failures != 0) {
        std::fprintf(stderr, "[FAIL] %ld 处断言失败\n", g_failures);
        return 1;
    }
    std::printf("[PASS] audit_peer_hub_stress\n");
    return 0;
}
