/**
 * @file test_p2p_interprocess.cpp
 * @brief 跨进程真实打洞测试：父进程提供 STUN 服务，两个子进程各自建通道。
 * @details 与单进程测试相比，这里验证的是真实的内核 socket 路径：
 *
 *          - 父进程启动本地 STUN echo 服务器；
 *          - 两次 `fork` + `execv` 拉起两个独立进程（角色 a/b），通过 pipe
 *            交换候选（模拟 IM 信令）；
 *          - 两个子进程各自 bind 127.0.0.1:0，收集候选（host 或经 STUN 的
 *            srflx），同时 Connect，然后完成一次请求-响应。
 *
 *          为什么要 execv 而不是直接 fork：父进程里 STUN 服务器有后台线程，
 *          多线程 fork 后子进程调用 malloc 可能与父进程线程持有的 malloc 锁
 *          死锁。exec 会重置地址空间，规避该风险。
 *
 *          场景：
 *          1. host 候选跨进程直连（无 STUN）；
 *          2. srflx 候选跨进程打洞（经本地 STUN，模拟 NAT 后两端）。
 *
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "go2cpp/p2p/candidate.hpp"
#include "go2cpp/p2p/endpoint.hpp"
#include "go2cpp/p2p/p2p_channel.hpp"
#include "go2cpp/p2p/stun_server.hpp"
#include "test_support.hpp"

namespace {

using namespace std::chrono_literals;
using go2cpp::p2p::Candidate;
using go2cpp::p2p::Endpoint;
using go2cpp::p2p::P2PChannel;
using go2cpp::p2p::P2PConfig;
using go2cpp::p2p::StunServer;
using go2cpp::p2p::StunServerConfig;

bool WriteAll(int fd, const void* data, std::size_t length) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    std::size_t offset = 0;
    while (offset < length) {
        const ssize_t written = ::write(fd, bytes + offset, length - offset);
        if (written <= 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        offset += static_cast<std::size_t>(written);
    }
    return true;
}

bool ReadAll(int fd, void* data, std::size_t length) {
    auto* bytes = static_cast<std::uint8_t*>(data);
    std::size_t offset = 0;
    while (offset < length) {
        const ssize_t received = ::read(fd, bytes + offset, length - offset);
        if (received <= 0) {
            if (received < 0 && errno == EINTR) {
                continue;
            }
            return false;
        }
        offset += static_cast<std::size_t>(received);
    }
    return true;
}

bool WriteMessage(int fd, const std::string& message) {
    const std::uint32_t length = static_cast<std::uint32_t>(message.size());
    return WriteAll(fd, &length, sizeof(length)) && WriteAll(fd, message.data(), message.size());
}

bool ReadMessage(int fd, std::string* message) {
    std::uint32_t length = 0;
    if (!ReadAll(fd, &length, sizeof(length)) || length > (1U << 20U)) {
        return false;
    }
    message->resize(length);
    return ReadAll(fd, message->data(), message->size());
}

int RunChild(const std::string& role, const std::string& stun_text, int read_fd, int write_fd) {
    P2PConfig config;
    config.bind_endpoint = Endpoint::Loopback(0);
    config.gather_host_candidates = true;
    config.gather_loopback_candidates = true;
    config.enable_relay = false;
    config.connect_timeout = 3000ms;
    config.check_interval = 50ms;
    if (!stun_text.empty()) {
        const auto stun = Endpoint::Parse(stun_text);
        if (!stun.has_value()) {
            std::fprintf(stderr, "[child %s] STUN 地址非法: %s\n", role.c_str(),
                         stun_text.c_str());
            return 1;
        }
        config.stun_servers.push_back(*stun);
        // 跨进程 srflx 场景：关闭 host 候选，强制用 STUN 映射地址打洞，
        // 等价于「两端都在 NAT 后、只交换公网映射」的真实流程。
        config.gather_host_candidates = false;
    }
    P2PChannel channel(config);
    const std::vector<Candidate> local = channel.GatherCandidates();
    if (local.empty()) {
        std::fprintf(stderr, "[child %s] 没有收集到候选\n", role.c_str());
        return 1;
    }
    if (!WriteMessage(write_fd, go2cpp::p2p::SerializeCandidates(local))) {
        std::fprintf(stderr, "[child %s] 写候选失败\n", role.c_str());
        return 1;
    }
    std::string remote_text;
    if (!ReadMessage(read_fd, &remote_text)) {
        std::fprintf(stderr, "[child %s] 读候选失败\n", role.c_str());
        return 1;
    }
    std::string error;
    const std::vector<Candidate> remote = go2cpp::p2p::ParseCandidates(remote_text, &error);
    if (remote.empty()) {
        std::fprintf(stderr, "[child %s] 解析候选失败: %s\n", role.c_str(), error.c_str());
        return 1;
    }
    for (const Candidate& candidate : remote) {
        channel.AddRemoteCandidate(candidate);
    }
    if (!channel.Connect(3000ms)) {
        std::fprintf(stderr, "[child %s] Connect 失败: %s\n", role.c_str(),
                     channel.LastError().c_str());
        return 1;
    }
    if (role == "a") {
        const std::string ping = "ping-from-a";
        if (!channel.Send(ping.data(), ping.size())) {
            std::fprintf(stderr, "[child a] 发送失败\n");
            return 1;
        }
        char buffer[64] = {};
        const std::size_t received = channel.Recv(buffer, sizeof(buffer), 3000ms);
        if (received != std::strlen("pong-from-b") ||
            std::memcmp(buffer, "pong-from-b", received) != 0) {
            std::fprintf(stderr, "[child a] 未收到预期回复（收到 %zu 字节）\n", received);
            return 1;
        }
    } else {
        char buffer[64] = {};
        const std::size_t received = channel.Recv(buffer, sizeof(buffer), 3000ms);
        if (received != std::strlen("ping-from-a") ||
            std::memcmp(buffer, "ping-from-a", received) != 0) {
            std::fprintf(stderr, "[child b] 未收到预期请求（收到 %zu 字节）\n", received);
            return 1;
        }
        const std::string pong = "pong-from-b";
        if (!channel.Send(pong.data(), pong.size())) {
            std::fprintf(stderr, "[child b] 回复发送失败\n");
            return 1;
        }
    }
    std::printf("[child %s] 连接成功：local=%s peer=%s relayed=%d\n", role.c_str(),
                channel.LocalEndpoint().ToString().c_str(),
                channel.PeerEndpoint().ToString().c_str(),
                static_cast<int>(channel.IsRelayed()));
    return 0;
}

struct ChildSpec {
    pid_t pid = -1;
    int read_fd = -1;
    int write_fd = -1;
};

pid_t SpawnChild(const char* executable, const std::string& role, const std::string& stun_text,
                 int read_fd, int write_fd, std::vector<std::string>* storage) {
    storage->clear();
    storage->push_back(executable);
    storage->push_back("--child");
    storage->push_back("--role=" + role);
    storage->push_back("--stun=" + stun_text);
    storage->push_back("--read-fd=" + std::to_string(read_fd));
    storage->push_back("--write-fd=" + std::to_string(write_fd));
    std::vector<char*> argv;
    argv.reserve(storage->size() + 1);
    for (std::string& item : *storage) {
        argv.push_back(item.data());
    }
    argv.push_back(nullptr);
    const pid_t pid = ::fork();
    if (pid == 0) {
        ::execv(executable, argv.data());
        std::perror("execv");
        ::_exit(127);
    }
    return pid;
}

bool RunInterprocessScenario(const StunServer* stun, bool expect_relayed,
                             const char* executable) {
    int a_to_b[2] = {-1, -1};
    int b_to_a[2] = {-1, -1};
    if (::pipe(a_to_b) != 0 || ::pipe(b_to_a) != 0) {
        std::perror("pipe");
        return false;
    }
    const std::string stun_text =
        stun != nullptr && stun->started() ? stun->primary_endpoint().ToString() : "";
    std::vector<std::string> args_a;
    std::vector<std::string> args_b;
    const pid_t pid_a = SpawnChild(executable, "a", stun_text, b_to_a[0], a_to_b[1], &args_a);
    const pid_t pid_b = SpawnChild(executable, "b", stun_text, a_to_b[0], b_to_a[1], &args_b);
    ::close(a_to_b[0]);
    ::close(a_to_b[1]);
    ::close(b_to_a[0]);
    ::close(b_to_a[1]);

    int status_a = 0;
    int status_b = 0;
    const pid_t waited_a = ::waitpid(pid_a, &status_a, 0);
    const pid_t waited_b = ::waitpid(pid_b, &status_b, 0);
    const bool ok_a = waited_a == pid_a && WIFEXITED(status_a) && WEXITSTATUS(status_a) == 0;
    const bool ok_b = waited_b == pid_b && WIFEXITED(status_b) && WEXITSTATUS(status_b) == 0;
    if (!ok_a) {
        std::fprintf(stderr, "[interprocess] 子进程 a 失败: status=%d\n", status_a);
    }
    if (!ok_b) {
        std::fprintf(stderr, "[interprocess] 子进程 b 失败: status=%d\n", status_b);
    }
    (void)expect_relayed;
    return ok_a && ok_b;
}

bool IsChildInvocation(int argc, char** argv) {
    return argc >= 2 && std::strcmp(argv[1], "--child") == 0;
}

std::string ChildArgument(int argc, char** argv, const char* prefix) {
    const std::string key(prefix);
    for (int index = 2; index < argc; ++index) {
        const std::string argument(argv[index]);
        if (argument.rfind(key, 0) == 0) {
            return argument.substr(key.size());
        }
    }
    return {};
}

std::string ExecutablePath() {
    char buffer[4096] = {};
    const ssize_t length = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (length <= 0) {
        return {};
    }
    return std::string(buffer, static_cast<std::size_t>(length));
}

}  // namespace

int main(int argc, char** argv) {
    if (IsChildInvocation(argc, argv)) {
        const std::string role = ChildArgument(argc, argv, "--role=");
        const std::string stun_text = ChildArgument(argc, argv, "--stun=");
        const int read_fd = std::atoi(ChildArgument(argc, argv, "--read-fd=").c_str());
        const int write_fd = std::atoi(ChildArgument(argc, argv, "--write-fd=").c_str());
        if (role.empty() || read_fd <= 0 || write_fd <= 0) {
            return 2;
        }
        // 子进程由 execv 启动，直接进入角色逻辑；用 _exit 避免析构继承的
        // 父进程对象（虽然 exec 已重置地址空间，保留显式退出更清晰）。
        ::_exit(RunChild(role, stun_text, read_fd, write_fd));
    }

    std::printf("=== 跨进程 P2P 打洞测试 ===\n");
    const std::string executable = ExecutablePath();
    P2P_CHECK(!executable.empty());
    if (executable.empty()) {
        return p2p_test::Summary("test_p2p_interprocess");
    }

    // 场景 1：host 候选跨进程直连（无 STUN）。
    const bool host_ok = RunInterprocessScenario(nullptr, false, executable.c_str());
    P2P_CHECK_MSG(host_ok, "跨进程 host 直连失败");
    std::printf("[INFO] 场景 1（host 跨进程直连）：%s\n", host_ok ? "PASS" : "FAIL");

    // 场景 2：经本地 STUN 的 srflx 候选跨进程打洞。
    StunServerConfig stun_config;
    stun_config.primary = Endpoint::Loopback(0);
    StunServer stun(stun_config);
    std::string error;
    const bool stun_started = stun.Start(&error);
    P2P_CHECK_MSG(stun_started, error.c_str());
    bool srflx_ok = false;
    if (stun_started) {
        srflx_ok = RunInterprocessScenario(&stun, false, executable.c_str());
        P2P_CHECK_MSG(srflx_ok, "跨进程 srflx 打洞失败");
        stun.Stop();
    }
    std::printf("[INFO] 场景 2（STUN srflx 跨进程打洞）：%s\n", srflx_ok ? "PASS" : "FAIL");

    return p2p_test::Summary("test_p2p_interprocess");
}
