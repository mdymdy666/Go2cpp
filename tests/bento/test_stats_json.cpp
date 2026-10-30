/**
 * @file test_stats_json.cpp
 * @brief LinkStats 聚合字段 + JSON 导出可解析性。
 * @author go2cpp contributors
 * @date 2026-10-26
 * @copyright MIT License
 */

#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "go2cpp/bento/net/link.hpp"
#include "test_util.hpp"

namespace {

using namespace go2cpp::bento;

/// 极简平坦 JSON 对象解析器：只接受 "key":number 形式，足够验证导出。
bool ParseFlatJson(const std::string& text, std::map<std::string, double>* values) {
    if (values == nullptr || text.size() < 2 || text.front() != '{' || text.back() != '}') {
        return false;
    }
    std::size_t index = 1;
    while (index + 1 < text.size()) {
        while (index < text.size() && (text[index] == ',' || text[index] == ' ')) {
            ++index;
        }
        if (index >= text.size() || text[index] == '}') {
            break;
        }
        if (text[index] != '"') {
            return false;
        }
        ++index;
        std::string key;
        while (index < text.size() && text[index] != '"') {
            key.push_back(text[index]);
            ++index;
        }
        if (index >= text.size()) {
            return false;
        }
        ++index;  // 结束引号
        if (index >= text.size() || text[index] != ':') {
            return false;
        }
        ++index;
        while (index < text.size() && text[index] == ' ') {
            ++index;
        }
        const std::size_t start = index;
        while (index < text.size()) {
            const char ch = text[index];
            const bool part = std::isdigit(static_cast<unsigned char>(ch)) != 0 || ch == '-' ||
                              ch == '+' || ch == '.' || ch == 'e' || ch == 'E';
            if (!part) {
                break;
            }
            ++index;
        }
        if (index == start) {
            return false;
        }
        try {
            (*values)[key] = std::stod(text.substr(start, index - start));
        } catch (...) {
            return false;
        }
    }
    return !values->empty();
}

bool HasNumber(const std::map<std::string, double>& values, const char* key) {
    return values.find(key) != values.end();
}

void TestStatsAndJson() {
    LinkConfig server_config;
    server_config.require_encryption = false;
    server_config.bind_address = "127.0.0.1";
    server_config.heartbeat_interval_ms = 100;
    server_config.peer_timeout_ms = 8000;
    Link server(server_config);
    std::string error;
    if (!server.Open(&error)) {
        std::fprintf(stderr, "[FAIL] server.Open: %s\n", error.c_str());
        BENTO_CHECK(false);
        return;
    }

    LinkConfig client_config;
    client_config.require_encryption = false;
    client_config.bind_address = "127.0.0.1";
    client_config.remote = "127.0.0.1";
    client_config.peer_port = server.local_port();
    client_config.heartbeat_interval_ms = 100;
    client_config.peer_timeout_ms = 8000;
    client_config.random_seed = 0x57A75U;
    Link client(client_config);
    if (!client.Open(&error)) {
        std::fprintf(stderr, "[FAIL] client.Open: %s\n", error.c_str());
        BENTO_CHECK(false);
        return;
    }

    bool client_connected = false;
    bool server_connected = false;
    client.on_connected = [&client_connected]() { client_connected = true; };
    server.on_connected = [&server_connected]() { server_connected = true; };
    client.Connect();
    const bool connected = bento_test::PumpUntil(
        client, server, [&]() { return client_connected && server_connected; }, 5000);
    BENTO_CHECK(connected);

    constexpr std::uint32_t kCount = 20;
    const std::vector<std::uint8_t> payload(128, 0x6B);
    for (std::uint32_t i = 0; i < kCount; ++i) {
        BENTO_CHECK(client.SendStream(0, payload.data(), payload.size()));
    }

    // 尚未收到 ACK：inflight 必须反映出站队列，cwnd/ssthresh/PTO 取初值。
    const LinkStats pending = client.Stats();
    BENTO_CHECK_EQ(pending.inflight, kCount);
    BENTO_CHECK(pending.cwnd_packets > 0);
    BENTO_CHECK(pending.ssthresh_packets > 0);
    BENTO_CHECK(pending.pto_ms > 0);
    BENTO_CHECK(pending.srtt_ms > 0);
    BENTO_CHECK(pending.bytes_sent > 0);

    const bool delivered = bento_test::PumpUntil(
        client, server,
        [&]() {
            return server.reliable_stats(0).messages_delivered == kCount &&
                   client.reliable_stats(0).inflight == 0;
        },
        10000);
    BENTO_CHECK(delivered);

    const LinkStats stats = client.Stats();
    BENTO_CHECK(stats.rtt_ms > 0);
    BENTO_CHECK(stats.srtt_ms > 0);
    BENTO_CHECK(stats.pto_ms > 0);
    BENTO_CHECK(stats.cwnd_packets > 0);
    BENTO_CHECK(stats.ssthresh_packets > 0);
    BENTO_CHECK_EQ(stats.inflight, 0U);
    BENTO_CHECK(stats.bytes_sent > 0);
    BENTO_CHECK(stats.bytes_received > 0);
    BENTO_CHECK(stats.bandwidth_bps > 0);
    BENTO_CHECK_EQ(stats.retransmits, 0U);
    BENTO_CHECK_EQ(stats.loss_events, 0U);
    BENTO_CHECK(std::fabs(stats.loss_rate) < 1e-9);
    const LinkStats server_stats = server.Stats();
    BENTO_CHECK(server_stats.bytes_received > 0);
    BENTO_CHECK(server_stats.frames_received > 0);

    const std::string json = client.StatsJson();
    BENTO_CHECK(!json.empty());
    BENTO_CHECK_EQ(json.front(), '{');
    BENTO_CHECK_EQ(json.back(), '}');
    BENTO_CHECK(json.find("nan") == std::string::npos);
    BENTO_CHECK(json.find("infinity") == std::string::npos);

    std::map<std::string, double> values;
    const bool parsed = ParseFlatJson(json, &values);
    BENTO_CHECK(parsed);
    BENTO_CHECK(values.size() >= 20U);
    const char* keys[] = {"rtt_ms",      "srtt_ms",     "rttvar_ms",  "pto_ms",
                          "cwnd_packets", "ssthresh_packets", "inflight", "loss",
                          "retrans",     "bytes_sent",  "bytes_received",
                          "bandwidth_bps", "loss_rate", "flow_control_blocked",
                          "max_data_sent", "max_data_received"};
    for (const char* key : keys) {
        BENTO_CHECK(HasNumber(values, key));
    }
    BENTO_CHECK(values["srtt_ms"] > 0.0);
    BENTO_CHECK(values["pto_ms"] > 0.0);
    BENTO_CHECK(values["cwnd_packets"] > 0.0);
    BENTO_CHECK(values["bytes_sent"] > 0.0);
    BENTO_CHECK(values["bandwidth_bps"] > 0.0);
    BENTO_CHECK(values["loss_rate"] >= 0.0);
    BENTO_CHECK(values["retrans"] >= 0.0);
    BENTO_CHECK(values["loss"] >= 0.0);
    BENTO_CHECK(values["srtt_ms"] == static_cast<double>(stats.srtt_ms));
    BENTO_CHECK(values["bytes_sent"] == static_cast<double>(stats.bytes_sent));
    BENTO_CHECK(values["bandwidth_bps"] == static_cast<double>(stats.bandwidth_bps));

    std::printf("[INFO] stats_json: %s\n", json.c_str());
    client.Close();
    server.Close();
}

}  // namespace

int main() {
    TestStatsAndJson();
    return bento_test::Finish("test_stats_json");
}
