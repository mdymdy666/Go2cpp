/**
 * @file replay_demo.cpp
 * @brief 重放端 demo：用编译期注册的入口（`RECORD_FIBER_ENTRY`）自动重跑。
 *
 * 用法（路径取 argv[1]，缺省 `replay-demo.g2cr`）：
 * ```
 * ./go2cpp_replay_replay /tmp/fanin.g2cr
 * ```
 *
 * 环境变量：`GO2CPP_REPLAY_DROP_WAKE=N` 在重放侧丢弃第 N 次唤醒，
 * 用来演示「正常记录 + 注入丢唤醒 → 重放复现卡死」。
 */

#include "demo/scenario.hpp"
#include "replay/auto_entry.hpp"

#include <cstdio>
#include <cstdlib>

namespace {

std::uint32_t EnvU32(const char* name, std::uint32_t fallback) {
    const char* raw = std::getenv(name);
    if (raw == nullptr || *raw == '\0') {
        return fallback;
    }
    return static_cast<std::uint32_t>(std::strtoul(raw, nullptr, 10));
}

}  // namespace

int main(int argc, char** argv) {
    using namespace go2cpp::replay;

    const char* path = argc > 1 ? argv[1] : "replay-demo.g2cr";
    const std::uint32_t drop_wake_at = EnvU32("GO2CPP_REPLAY_DROP_WAKE", 0);

    ReplayOptions options;
    options.drop_wake_at = drop_wake_at;
    options.keep_timeline = false;

    const ReplayReport report = ReplayFileAuto(path, options);
    const auto received = demo::SharedResult().Snapshot();

    std::printf("%s", report.Summary().c_str());
    std::printf("[replay] consumer received=%zu checksum=%llu\n", received.size(),
                static_cast<unsigned long long>(demo::SharedResult().Checksum()));
    std::printf("[replay] recorded: messages=%llu checksum=%llu\n",
                static_cast<unsigned long long>(report.stats.messages),
                static_cast<unsigned long long>(report.stats.message_checksum));
    std::printf("[replay] replayed: checksum=%llu\n",
                static_cast<unsigned long long>(report.stats.replayed_message_checksum));

    const bool consistent =
        report.status == ReplayStatus::kOk &&
        received.size() == demo::kTotalMessages &&
        demo::SharedResult().Checksum() == report.stats.message_checksum;
    std::printf("[replay] consistency=%s\n", consistent ? "MATCH" : "MISMATCH");
    if (!consistent) {
        std::printf("[replay] status=%s error=%s\n",
                    ReplayStatusName(report.status), report.error.c_str());
    }
    return consistent ? 0 : 1;
}
