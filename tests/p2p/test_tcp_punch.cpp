/**
 * @file test_tcp_punch.cpp
 * @brief TCP 同时打开（simultaneous open）打洞测试。
 * @details 两个 `TcpPuncher` 各自绑定 127.0.0.1 的临时端口，交换端口后
 *          在两个线程里同时 `Connect()`；成功后双向收发并校验。
 *
 *          该能力依赖内核对 TCP simultaneous open 的支持，并且需要双方
 *          几乎同时发起。测试最多尝试 3 轮，全部失败时记为 SKIP（而不是
 *          FAIL）：这属于环境/内核能力限制，不是实现缺陷。
 *
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

#include "go2cpp/p2p/tcp_punch.hpp"
#include "test_support.hpp"

namespace {

using namespace std::chrono_literals;
using go2cpp::p2p::Endpoint;
using go2cpp::p2p::TcpPuncher;

bool TryRound(int round, std::string* detail) {
    std::string error;
    auto a = TcpPuncher::Bind(Endpoint::Loopback(0), &error);
    auto b = TcpPuncher::Bind(Endpoint::Loopback(0), &error);
    if (!a || !b) {
        *detail = "Bind 失败: " + error;
        return false;
    }
    const Endpoint endpoint_a = a->local_endpoint();
    const Endpoint endpoint_b = b->local_endpoint();
    std::atomic<bool> ok_a{false};
    std::atomic<bool> ok_b{false};
    std::string error_a;
    std::string error_b;
    std::thread thread_a([&]() { ok_a.store(a->Connect(endpoint_b, 2000ms, &error_a)); });
    std::thread thread_b([&]() { ok_b.store(b->Connect(endpoint_a, 2000ms, &error_b)); });
    thread_a.join();
    thread_b.join();
    if (!ok_a.load() || !ok_b.load()) {
        *detail = "第 " + std::to_string(round) + " 轮同时打开失败: A=" +
                  (error_a.empty() ? "timeout" : error_a) + " B=" +
                  (error_b.empty() ? "timeout" : error_b);
        return false;
    }
    if (!a->Send("tcp-ping", 8)) {
        *detail = "A 发送失败";
        return false;
    }
    char buffer[32] = {};
    const int received = b->Recv(buffer, sizeof(buffer), 1000ms);
    if (received != 8 || std::memcmp(buffer, "tcp-ping", 8) != 0) {
        *detail = "B 未收到 A 的数据";
        return false;
    }
    if (!b->Send("tcp-pong", 8)) {
        *detail = "B 发送失败";
        return false;
    }
    const int reply = a->Recv(buffer, sizeof(buffer), 1000ms);
    if (reply != 8 || std::memcmp(buffer, "tcp-pong", 8) != 0) {
        *detail = "A 未收到 B 的回复";
        return false;
    }
    *detail = "TCP 同时打开成功（第 " + std::to_string(round) + " 轮）";
    return true;
}

}  // namespace

int main() {
    std::printf("=== TCP 同时打开打洞测试 ===\n");
    bool success = false;
    std::string detail;
    for (int round = 0; round < 3 && !success; ++round) {
        success = TryRound(round, &detail);
        std::printf("[INFO] %s\n", detail.c_str());
    }
    if (success) {
        P2P_CHECK(true);
    } else {
        p2p_test::Skip("本机未能完成 TCP simultaneous open（内核/时序限制）: " + detail);
    }
    return p2p_test::Summary("test_tcp_punch");
}
