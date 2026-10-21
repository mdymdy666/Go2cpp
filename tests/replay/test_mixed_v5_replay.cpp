/**
 * @file test_mixed_v5_replay.cpp
 * @brief v5 四族（RWMutex / CV / WaitGroup / IO）混合随机回放测试。
 *
 * 100 轮，每轮：
 *   - 用固定种子生成 4 个族的合法事件块（族顺序随机、数量随机、字段随机）；
 *   - 独立录制到临时文件并回放；
 *   - 断言每轮的资源事件数、各族计数、独立校验和、seq 顺序与二次回放一致；
 *   - 打印 ``round=NN MATCH``，最终要求 100/100。
 *
 * 说明：混合测试使用**合成事件序列**而不是真实原语，原因是每轮需要独立
 * Recorder 与可复现的随机序列；真实原语的录制由前四个测试分别覆盖。
 */

#include "replay_v5_test_support.hpp"

#include <algorithm>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace go2cpp::replay;
using namespace go2cpp::replay::v5test;

constexpr int kRounds = 100;

/// 本地期望计数（与回放统计逐字段比较）。
struct ExpectedCounts {
    std::uint64_t rw_reads{0};
    std::uint64_t rw_writes{0};
    std::uint64_t rw_unlocks{0};
    std::uint64_t rw_read_blocks{0};
    std::uint64_t rw_write_blocks{0};
    std::uint64_t cv_waits{0};
    std::uint64_t cv_notifies{0};
    std::uint64_t cv_notify_alls{0};
    std::uint64_t cv_woken{0};
    std::uint64_t wg_adds{0};
    std::uint64_t wg_dones{0};
    std::uint64_t wg_waits{0};
    std::uint64_t io_reads{0};
    std::uint64_t io_writes{0};
    std::uint64_t io_waits{0};
    std::uint64_t io_wakes{0};
    std::uint64_t io_bytes{0};
};

struct RoundPlan {
    std::vector<Event> events;
    ExpectedCounts expected;
};

std::uint64_t ReadFlag() {
    return static_cast<std::uint64_t>(ResourceAuxFlags::kRead);
}
std::uint64_t WriteFlag() {
    return static_cast<std::uint64_t>(ResourceAuxFlags::kWrite);
}
std::uint64_t NotifyAllFlag() {
    return static_cast<std::uint64_t>(ResourceAuxFlags::kNotifyAll);
}

/// 构造一轮随机事件；返回事件序列与本地期望计数。
RoundPlan BuildRound(std::uint64_t seed, int round) {
    Rng rng(seed);
    RoundPlan plan;
    ExpectedCounts& expected = plan.expected;
    const std::uint64_t rw_id = 600U + static_cast<std::uint64_t>(round);
    const std::uint64_t cv_id = 700U + static_cast<std::uint64_t>(round);
    const std::uint64_t wg_id = 800U + static_cast<std::uint64_t>(round);
    const std::uint64_t io_id = 900U + static_cast<std::uint64_t>(round);

    std::vector<std::vector<Event>> blocks;

    // ---- RWMutex：1..4 个读操作 + 0..1 个写操作，每个操作后立即解锁 ----
    {
        std::vector<Event> block;
        const int reads = 1 + static_cast<int>(rng.Below(4U));
        for (int index = 0; index < reads; ++index) {
            const std::uint64_t actor = 1U + rng.Below(4U);
            if (rng.Chance(40U)) {
                block.push_back(MakeV5(EventType::kResRwReadLock, rw_id,
                                       actor, 0U, 1U,
                                       static_cast<std::uint32_t>(
                                           expected.rw_read_blocks + 1U),
                                       -1, 0U, ReadFlag(), 0));
                ++expected.rw_read_blocks;
            }
            block.push_back(MakeV5(EventType::kResRwReadLock, rw_id, actor, 0U,
                                   0U, 0U, -1, 0U, ReadFlag(), 0));
            ++expected.rw_reads;
            block.push_back(MakeV5(EventType::kResRwUnlock, rw_id, actor, 0U,
                                   0U, 0U, -1, 0U, ReadFlag(), 0));
            ++expected.rw_unlocks;
        }
        if (rng.Chance(60U)) {
            const std::uint64_t actor = 1U + rng.Below(4U);
            if (rng.Chance(50U)) {
                block.push_back(MakeV5(EventType::kResRwWriteLock, rw_id,
                                       actor, 0U, 1U,
                                       static_cast<std::uint32_t>(
                                           expected.rw_write_blocks + 1U),
                                       -1, 0U, WriteFlag(), 0));
                ++expected.rw_write_blocks;
            }
            block.push_back(MakeV5(EventType::kResRwWriteLock, rw_id, actor,
                                   0U, 0U, 0U, -1, 0U, WriteFlag(), 0));
            ++expected.rw_writes;
            block.push_back(MakeV5(EventType::kResRwUnlock, rw_id, actor, 0U,
                                   0U, 0U, -1, 0U, WriteFlag(), 0));
            ++expected.rw_unlocks;
        }
        blocks.push_back(std::move(block));
    }

    // ---- ConditionVariable：0..3 次等待 + 0..2 次单播 + 0..2 次广播 ----
    {
        std::vector<Event> block;
        const int waits = static_cast<int>(rng.Below(4U));
        for (int index = 0; index < waits; ++index) {
            const std::uint64_t actor = 1U + rng.Below(4U);
            block.push_back(MakeV5(EventType::kResCvWait, cv_id, actor, 0U,
                                   1U, 1U, -1, 0U, 0U, 0));
            ++expected.cv_waits;
            if (rng.Chance(70U)) {
                block.push_back(MakeV5(EventType::kResCvWait, cv_id, actor, 0U,
                                       0U, 0U, -1, 0U, 0U, 0));
            } else {
                block.push_back(MakeV5(EventType::kResCvWait, cv_id, actor, 0U,
                                       4U, 0U, -1, 0U, 0U, 0));
            }
            ++expected.cv_waits;
        }
        const int notifies = static_cast<int>(rng.Below(3U));
        for (int index = 0; index < notifies; ++index) {
            const std::uint64_t actor = 1U + rng.Below(4U);
            const bool hit = rng.Chance(60U);
            const std::uint64_t target = hit ? 1U + rng.Below(4U) : 0U;
            block.push_back(MakeV5(EventType::kResCvNotify, cv_id, actor,
                                   target, hit ? 2U : 0U, 0U, -1, 0U, 0U, 0));
            ++expected.cv_notifies;
            if (hit) {
                ++expected.cv_woken;
            }
        }
        const int notify_alls = static_cast<int>(rng.Below(3U));
        for (int index = 0; index < notify_alls; ++index) {
            const std::uint64_t actor = 1U + rng.Below(4U);
            const std::uint32_t woken = rng.Below(4U);
            block.push_back(MakeV5(
                EventType::kResCvNotifyAll, cv_id, actor, woken != 0U ? 1U : 0U,
                woken != 0U ? 2U : 0U, woken, -1, 0U, NotifyAllFlag(),
                static_cast<std::int64_t>(woken)));
            ++expected.cv_notify_alls;
            expected.cv_woken += woken;
        }
        blocks.push_back(std::move(block));
    }

    // ---- WaitGroup：0..2 次 Add + 不超过计数的 Done + 0..1 次等待 ----
    {
        std::vector<Event> block;
        std::int64_t count = 0;
        const int adds = static_cast<int>(rng.Below(3U));
        for (int index = 0; index < adds; ++index) {
            const std::int64_t delta = 1 + static_cast<std::int64_t>(rng.Below(3U));
            count += delta;
            block.push_back(MakeV5(
                EventType::kResWgAdd, wg_id, 1U + rng.Below(4U), 0U, 0U, 0U,
                -1, 0U, static_cast<std::uint64_t>(delta), count));
            ++expected.wg_adds;
        }
        const std::uint32_t max_dones =
            static_cast<std::uint32_t>(count > 0 ? count : 0);
        const std::uint32_t dones =
            max_dones == 0U ? 0U : rng.Below(max_dones + 1U);
        for (std::uint32_t index = 0; index < dones; ++index) {
            count -= 1;
            block.push_back(MakeV5(EventType::kResWgDone, wg_id,
                                   1U + rng.Below(4U), 0U, 0U, 0U, -1, 0U,
                                   0xFFFFFFFFULL, count));
            ++expected.wg_dones;
        }
        const int waits = static_cast<int>(rng.Below(2U));
        for (int index = 0; index < waits; ++index) {
            const std::uint64_t actor = 1U + rng.Below(4U);
            block.push_back(MakeV5(EventType::kResWgWait, wg_id, actor, 0U,
                                   1U, 1U, -1, 0U, 0U, count));
            ++expected.wg_waits;
            const bool notified = rng.Chance(50U);
            block.push_back(MakeV5(EventType::kResWgWait, wg_id, actor, 0U,
                                   notified ? 0U : 4U, 0U, -1, 0U, 0U, 0));
            ++expected.wg_waits;
        }
        blocks.push_back(std::move(block));
    }

    // ---- IO：0..3 个随机事件，fd/bytes 随机 ----
    {
        std::vector<Event> block;
        const int count = static_cast<int>(rng.Below(4U));
        for (int index = 0; index < count; ++index) {
            const std::uint32_t kind = rng.Below(4U);
            const std::int32_t fd =
                rng.Chance(10U) ? -1 : 3 + static_cast<std::int32_t>(rng.Below(18U));
            const std::uint32_t bytes =
                rng.Chance(50U) ? 0U : rng.Below(65U);
            if (kind == 0U) {
                block.push_back(MakeV5(EventType::kResIoRead, io_id,
                                       1U + rng.Below(4U), 0U, 0U, 0U, fd,
                                       bytes, ReadFlag(), 0));
                ++expected.io_reads;
                expected.io_bytes += bytes;
            } else if (kind == 1U) {
                block.push_back(MakeV5(EventType::kResIoWrite, io_id,
                                       1U + rng.Below(4U), 0U, 0U, 0U, fd,
                                       bytes, WriteFlag(), 0));
                ++expected.io_writes;
                expected.io_bytes += bytes;
            } else if (kind == 2U) {
                block.push_back(MakeV5(EventType::kResIoWait, io_id,
                                       1U + rng.Below(4U), 0U, 1U, 1U, fd, 0U,
                                       0U, 0));
                ++expected.io_waits;
            } else {
                block.push_back(MakeV5(EventType::kResIoWake, io_id, 0U,
                                       1U + rng.Below(4U), 0U, 0U, fd, bytes,
                                       0U, 0));
                ++expected.io_wakes;
                expected.io_bytes += bytes;
            }
        }
        blocks.push_back(std::move(block));
    }

    // 族顺序随机：逐块 Fisher-Yates（块内 lock/unlock 顺序保持）。
    for (std::size_t index = blocks.size(); index > 1; --index) {
        const std::size_t other = static_cast<std::size_t>(
            rng.Below(static_cast<std::uint32_t>(index)));
        std::swap(blocks[index - 1], blocks[other]);
    }
    for (auto& block : blocks) {
        plan.events.insert(plan.events.end(), block.begin(), block.end());
    }
    return plan;
}

/// 每轮计数比较；返回是否全部匹配。
bool CountsMatch(const ReplayStats& stats, const ExpectedCounts& expected) {
    return stats.res_rw_reads == expected.rw_reads &&
           stats.res_rw_writes == expected.rw_writes &&
           stats.res_rw_unlocks == expected.rw_unlocks &&
           stats.res_rw_read_blocks == expected.rw_read_blocks &&
           stats.res_rw_write_blocks == expected.rw_write_blocks &&
           stats.res_cv_waits == expected.cv_waits &&
           stats.res_cv_notifies == expected.cv_notifies &&
           stats.res_cv_notify_alls == expected.cv_notify_alls &&
           stats.res_cv_woken == expected.cv_woken &&
           stats.res_wg_adds == expected.wg_adds &&
           stats.res_wg_dones == expected.wg_dones &&
           stats.res_wg_waits == expected.wg_waits &&
           stats.res_io_reads == expected.io_reads &&
           stats.res_io_writes == expected.io_writes &&
           stats.res_io_waits == expected.io_waits &&
           stats.res_io_wakes == expected.io_wakes &&
           stats.res_io_bytes == expected.io_bytes;
}

void TestMixedRounds() {
    int matched = 0;
    std::uint64_t total_events = 0;
    std::uint64_t checksum_chain = 1469598103934665603ULL;
    const std::uint64_t start_ns = NowNs();
    for (int round = 0; round < kRounds; ++round) {
        const std::string path = TempPath("mixed");
        const std::uint64_t seed =
            0x5EED1234ULL + static_cast<std::uint64_t>(round) * 0x9E3779B9ULL;
        const RoundPlan plan = BuildRound(seed, round);
        const RecordedFile recorded =
            RecordAndReplay(path, plan.events, false, "mixed-round");
        const ReplayReport& report = recorded.report;
        const std::uint64_t independent = ResourceChecksum(plan.events);
        const bool ok =
            report.status == ReplayStatus::kOk && report.complete &&
            report.stats.resource_events == plan.events.size() &&
            report.stats.resource_checksum == independent &&
            CountsMatch(report.stats, plan.expected);
        GO2CPP_CHECK(ok);
        if (!ok) {
            std::printf(
                "[mixed] round=%03d MISMATCH events=%llu expected=%llu "
                "checksum=%llu independent=%llu\n",
                round, static_cast<unsigned long long>(
                           report.stats.resource_events),
                static_cast<unsigned long long>(plan.events.size()),
                static_cast<unsigned long long>(
                    report.stats.resource_checksum),
                static_cast<unsigned long long>(independent));
            PrintStats("mixed", report);
        } else {
            ++matched;
            std::printf("[mixed] round=%03d MATCH events=%llu checksum=%llu\n",
                        round,
                        static_cast<unsigned long long>(
                            report.stats.resource_events),
                        static_cast<unsigned long long>(
                            report.stats.resource_checksum));
        }
        ExpectConsistent(path, plan.events, report, "mixed");
        checksum_chain = FnvStep(checksum_chain, report.stats.resource_checksum);
        total_events += report.stats.resource_events;
        RemoveFile(path);
    }
    const std::uint64_t elapsed_ns = NowNs() - start_ns;
    std::printf(
        "[mixed] summary matched=%d/%d events=%llu checksum_chain=%llu "
        "elapsed_ms=%.3f\n",
        matched, kRounds, static_cast<unsigned long long>(total_events),
        static_cast<unsigned long long>(checksum_chain),
        static_cast<double>(elapsed_ns) / 1e6);
    GO2CPP_CHECK(matched == kRounds);
}

}  // namespace

void run_mixed_v5_replay_tests() {
    go2cpp_tests::announce("mixed_v5_replay");
    TestMixedRounds();
}
