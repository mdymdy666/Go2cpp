/**
 * @file main.cpp
 * @brief P2P 打洞演示：一个 STUN echo 服务器 + 两个独立客户端进程。
 * @details 用三个进程还原「两个 NAT 后的主机通过 STUN 发现公网地址、交换候选、
 *          UDP 打洞、建立 P2P 通道」的最小闭环：
 *
 *          1. `--stun`：启动 `go2cpp::p2p::StunServer`（RFC 5389 Binding echo）；
 *          2. `--role a` / `--role b`：各自绑定**不同的回环地址**（127.0.0.1 与
 *             127.0.0.2，Linux 的 127/8 全在 lo 上），模拟两台 NAT 后的主机；
 *             它们关闭 host 候选、只通过 STUN 拿 srflx 候选，因此交换的是
 *             「NAT 公网映射地址」，与真实打洞流程一致；
 *          3. 信令用**文件邮箱**（各自把候选写进 `--signal-dir` 下的文件，再轮询
 *             读对端的文件），模拟 IM/WebSocket 等旁路；两进程几乎同时
 *             `Connect()`，`P2PChannel` 并行发 STUN Binding 检查并锁定对端地址；
 *          4. 角色 a 发 3 条消息、角色 b 逐条回 ack，最后打印候选、连接方式、
 *             RTT 与收发统计。
 *
 *          运行（见同目录 README / run_demo.sh）：
 *            ./go2cpp_p2p_demo stun --port 34780
 *            ./go2cpp_p2p_demo peer --role a --stun 127.0.0.1:34780
 *            ./go2cpp_p2p_demo peer --role b --stun 127.0.0.1:34780
 *
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include <pthread.h>
#include <signal.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include "go2cpp/p2p/candidate.hpp"
#include "go2cpp/p2p/endpoint.hpp"
#include "go2cpp/p2p/p2p_channel.hpp"
#include "go2cpp/p2p/stun_server.hpp"

namespace {

using namespace std::chrono_literals;

using go2cpp::p2p::Candidate;
using go2cpp::p2p::CandidateTypeName;
using go2cpp::p2p::Endpoint;
using go2cpp::p2p::P2PChannel;
using go2cpp::p2p::P2PConfig;
using go2cpp::p2p::P2PStateName;
using go2cpp::p2p::ParseCandidates;
using go2cpp::p2p::SerializeCandidates;
using go2cpp::p2p::StunServer;
using go2cpp::p2p::StunServerConfig;

/// @brief 单调时钟毫秒数。
std::uint64_t NowMs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

/// @brief 解析 `host:port`，失败时打印原因并返回 false。
bool ParseEndpoint(const std::string& text, Endpoint* out) {
    const auto parsed = Endpoint::Parse(text);
    if (!parsed.has_value()) {
        std::fprintf(stderr, "端点格式非法: %s（期望 a.b.c.d:port）\n", text.c_str());
        return false;
    }
    *out = *parsed;
    return true;
}

/// @brief 原子写文件：先写 `.tmp` 再 rename，避免对端读到半截候选。
bool WriteAtomically(const std::filesystem::path& path, const std::string& content) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    const std::string tmp = path.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            return false;
        }
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!out) {
            return false;
        }
    }
    std::filesystem::rename(tmp, path, ec);
    return !ec;
}

/// @brief 读取整个文本文件；文件不存在/不可读返回 false。
bool ReadFile(const std::filesystem::path& path, std::string* out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    out->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

/// @brief 等待对端候选文件出现并读出（文件信令，模拟 IM 旁路）。
bool WaitForPeerCandidates(const std::filesystem::path& path,
                           std::chrono::milliseconds timeout, std::string* text) {
    const std::uint64_t deadline = NowMs() + static_cast<std::uint64_t>(timeout.count());
    while (NowMs() < deadline) {
        if (ReadFile(path, text) && !text->empty()) {
            return true;
        }
        std::this_thread::sleep_for(20ms);
    }
    return false;
}

/// @brief `stun` 子命令：起一个 STUN echo 服务器，Ctrl-C/SIGTERM 退出。
int RunStun(std::uint16_t port) {
    // 先屏蔽信号再启动服务器线程：新线程继承屏蔽字，只有主线程的 sigwait
    // 能取到信号，STUN 的接收循环不会被信号打断。
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    if (pthread_sigmask(SIG_BLOCK, &signals, nullptr) != 0) {
        std::fprintf(stderr, "[stun] pthread_sigmask 失败\n");
        return 1;
    }

    StunServerConfig config;
    config.primary = Endpoint::Loopback(port);
    config.software = "go2cpp-stun-demo/1.0";
    StunServer server(config);
    std::string error;
    if (!server.Start(&error)) {
        std::fprintf(stderr, "[stun] 启动失败: %s\n", error.c_str());
        return 1;
    }
    std::printf("[stun] 已监听 %s（等待 Ctrl-C）\n",
                server.primary_endpoint().ToString().c_str());
    std::fflush(stdout);

    int signal_number = 0;
    if (sigwait(&signals, &signal_number) != 0) {
        std::fprintf(stderr, "[stun] sigwait 失败\n");
    }
    server.Stop();
    std::printf("[stun] 已停止：处理 Binding 请求 %llu 个，响应 %llu 个\n",
                static_cast<unsigned long long>(server.requests_served()),
                static_cast<unsigned long long>(server.responses_sent()));
    return 0;
}

/// @brief 打印候选列表（类型/地址/base/优先级）。
void PrintCandidates(const char* role, const std::vector<Candidate>& candidates) {
    std::printf("[peer %s] 收集到 %zu 个候选：\n", role, candidates.size());
    for (const Candidate& candidate : candidates) {
        std::printf("  %-5s %-21s base=%-21s priority=%u\n",
                    CandidateTypeName(candidate.type), candidate.endpoint.ToString().c_str(),
                    candidate.base.ToString().c_str(), candidate.priority);
    }
}

/// @brief 一条 peer 的完整流程：收集候选 → 文件信令 → 打洞 → 收发 → 统计。
int RunPeer(const std::string& role, const Endpoint& bind, const Endpoint& stun,
            const std::filesystem::path& signal_dir, std::chrono::milliseconds timeout,
            bool gather_host_candidates) {
    P2PConfig config;
    config.bind_endpoint = bind;
    config.stun_servers = {stun};
    // 模拟「两端都在 NAT 后」：默认只交换 STUN 反射地址（srflx），不暴露
    // host 候选。--host-candidates 则保留 host，便于对比局域网直连。
    config.gather_host_candidates = gather_host_candidates;
    config.gather_loopback_candidates = true;
    config.enable_relay = false;
    config.connect_timeout = timeout;
    config.check_interval = 50ms;
    config.keepalive_interval = 50ms;

    P2PChannel channel(config);
    std::printf("[peer %s] 绑定 %s，STUN=%s，收集候选...\n", role.c_str(),
                bind.ToString().c_str(), stun.ToString().c_str());
    const std::vector<Candidate> local = channel.GatherCandidates();
    if (local.empty()) {
        std::fprintf(stderr, "[peer %s] 没有候选：STUN 不可达或 bind 地址非法\n",
                     role.c_str());
        return 1;
    }
    PrintCandidates(role.c_str(), local);

    const std::filesystem::path mine = signal_dir / ("candidates-" + role + ".txt");
    const std::filesystem::path peer_file =
        signal_dir / ("candidates-" + std::string(role == "a" ? "b" : "a") + ".txt");
    if (!WriteAtomically(mine, SerializeCandidates(local))) {
        std::fprintf(stderr, "[peer %s] 写入候选失败: %s\n", role.c_str(),
                     mine.string().c_str());
        return 1;
    }
    std::printf("[peer %s] 候选已写入 %s，等待对端候选...\n", role.c_str(),
                mine.string().c_str());
    std::fflush(stdout);

    std::string remote_text;
    if (!WaitForPeerCandidates(peer_file, timeout, &remote_text)) {
        std::fprintf(stderr, "[peer %s] 等待对端候选超时: %s\n", role.c_str(),
                     peer_file.string().c_str());
        return 1;
    }
    std::string parse_error;
    const std::vector<Candidate> remote = ParseCandidates(remote_text, &parse_error);
    if (remote.empty()) {
        std::fprintf(stderr, "[peer %s] 解析对端候选失败: %s\n", role.c_str(),
                     parse_error.c_str());
        return 1;
    }
    for (const Candidate& candidate : remote) {
        channel.AddRemoteCandidate(candidate);
    }
    std::printf("[peer %s] 收到对端 %zu 个候选，开始打洞...\n", role.c_str(),
                remote.size());
    std::fflush(stdout);

    const std::uint64_t connect_start = NowMs();
    if (!channel.Connect(timeout)) {
        std::fprintf(stderr, "[peer %s] Connect 失败: %s\n", role.c_str(),
                     channel.LastError().c_str());
        return 1;
    }
    std::printf("[peer %s] 连接成功（%llu ms）：state=%s local=%s peer=%s relayed=%d\n",
                role.c_str(),
                static_cast<unsigned long long>(NowMs() - connect_start),
                P2PStateName(channel.state()), channel.LocalEndpoint().ToString().c_str(),
                channel.PeerEndpoint().ToString().c_str(),
                static_cast<int>(channel.IsRelayed()));
    std::fflush(stdout);

    // 给 keepalive 一个往返窗口，让统计里的 RTT 有值（而不是 0）。
    std::this_thread::sleep_for(300ms);

    if (role == "a") {
        for (int i = 0; i < 3; ++i) {
            const std::string payload = "hello-" + std::to_string(i) + "-from-a";
            if (!channel.Send(payload.data(), payload.size())) {
                std::fprintf(stderr, "[peer a] 发送失败\n");
                return 1;
            }
            std::printf("[peer a] -> %s\n", payload.c_str());
        }
        for (int i = 0; i < 3; ++i) {
            char buffer[1200] = {};
            const std::size_t received = channel.Recv(buffer, sizeof(buffer), 3000ms);
            if (received == 0) {
                std::fprintf(stderr, "[peer a] 等待 ack 超时\n");
                return 1;
            }
            const std::string reply(buffer, received);
            std::printf("[peer a] <- %s\n", reply.c_str());
            if (reply != "ack-" + std::to_string(i)) {
                std::fprintf(stderr, "[peer a] ack 内容不符\n");
                return 1;
            }
        }
    } else {
        for (int i = 0; i < 3; ++i) {
            char buffer[1200] = {};
            const std::size_t received = channel.Recv(buffer, sizeof(buffer), 5000ms);
            if (received == 0) {
                std::fprintf(stderr, "[peer b] 等待消息超时\n");
                return 1;
            }
            const std::string text(buffer, received);
            std::printf("[peer b] <- %s\n", text.c_str());
            const std::string ack = "ack-" + std::to_string(i);
            if (!channel.Send(ack.data(), ack.size())) {
                std::fprintf(stderr, "[peer b] 回复失败\n");
                return 1;
            }
            std::printf("[peer b] -> %s\n", ack.c_str());
        }
    }

    const auto stats = channel.Stats();
    std::printf(
        "[peer %s] 统计：发包=%llu 收包=%llu 业务字节=%llu/%llu 连通检查=%llu/%llu "
        "RTT=%.3fms 平均=%.3fms 序列缺口=%u\n",
        role.c_str(), static_cast<unsigned long long>(stats.packets_sent),
        static_cast<unsigned long long>(stats.packets_received),
        static_cast<unsigned long long>(stats.bytes_sent),
        static_cast<unsigned long long>(stats.bytes_received),
        static_cast<unsigned long long>(stats.connectivity_checks_sent),
        static_cast<unsigned long long>(stats.connectivity_checks_received), stats.rtt_ms,
        stats.rtt_avg_ms, stats.sequence_gaps);
    std::printf("P2P_OK role=%s relayed=%d local=%s peer=%s\n", role.c_str(),
                static_cast<int>(channel.IsRelayed()),
                channel.LocalEndpoint().ToString().c_str(),
                channel.PeerEndpoint().ToString().c_str());
    channel.Close();
    return 0;
}

/// @brief 打印用法。
void PrintUsage(const char* program) {
    std::printf(
        "用法:\n"
        "  %s stun [--port 34780]\n"
        "  %s peer --role a|b [--stun 127.0.0.1:34780] [--bind 127.0.0.1:0]\n"
        "         [--signal-dir /tmp/go2cpp_p2p_demo] [--timeout-ms 8000]\n"
        "         [--host-candidates]\n",
        program, program);
}

/// @brief 解析无符号整数，失败返回 false。
bool ParseUint(const std::string& text, unsigned long* out) {
    if (text.empty()) {
        return false;
    }
    char* end = nullptr;
    const unsigned long value = std::strtoul(text.c_str(), &end, 10);
    if (end == nullptr || *end != '\0') {
        return false;
    }
    *out = value;
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        PrintUsage(argv[0]);
        return 2;
    }
    const std::string command = argv[1];

    std::uint16_t port = 34780;
    std::string role = "a";
    std::string stun_text = "127.0.0.1:34780";
    std::string bind_text;
    std::string signal_dir = "/tmp/go2cpp_p2p_demo";
    unsigned long timeout_ms = 8000;
    bool gather_host_candidates = false;

    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        const bool has_value = (i + 1) < argc;
        if (arg == "--port" && has_value) {
            unsigned long value = 0;
            if (!ParseUint(argv[++i], &value) || value > 65535) {
                std::fprintf(stderr, "--port 非法\n");
                return 2;
            }
            port = static_cast<std::uint16_t>(value);
        } else if (arg == "--role" && has_value) {
            role = argv[++i];
            if (role != "a" && role != "b") {
                std::fprintf(stderr, "--role 只能是 a 或 b\n");
                return 2;
            }
        } else if (arg == "--stun" && has_value) {
            stun_text = argv[++i];
        } else if (arg == "--bind" && has_value) {
            bind_text = argv[++i];
        } else if (arg == "--signal-dir" && has_value) {
            signal_dir = argv[++i];
        } else if (arg == "--timeout-ms" && has_value) {
            if (!ParseUint(argv[++i], &timeout_ms) || timeout_ms == 0) {
                std::fprintf(stderr, "--timeout-ms 非法\n");
                return 2;
            }
        } else if (arg == "--host-candidates") {
            gather_host_candidates = true;
        } else if (arg == "--help" || arg == "-h") {
            PrintUsage(argv[0]);
            return 0;
        } else {
            std::fprintf(stderr, "未知参数: %s\n", arg.c_str());
            PrintUsage(argv[0]);
            return 2;
        }
    }

    if (command == "stun") {
        return RunStun(port);
    }
    if (command == "peer") {
        if (bind_text.empty()) {
            // 两个角色默认绑不同的回环地址，模拟两个不同 NAT 后的主机。
            bind_text = (role == "a") ? "127.0.0.1:0" : "127.0.0.2:0";
        }
        Endpoint bind;
        Endpoint stun;
        if (!ParseEndpoint(bind_text, &bind) || !ParseEndpoint(stun_text, &stun)) {
            return 2;
        }
        return RunPeer(role, bind, stun, std::filesystem::path(signal_dir),
                       std::chrono::milliseconds(timeout_ms), gather_host_candidates);
    }
    std::fprintf(stderr, "未知子命令: %s\n", command.c_str());
    PrintUsage(argv[0]);
    return 2;
}
