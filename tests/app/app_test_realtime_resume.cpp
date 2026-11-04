/**
 * @file test_realtime_resume.cpp
 * @brief 断线续传基础件 `realtime::SeqJournal` 的无框架测试：序号注入、
 *        单调性、环形淘汰、区间补发、gap 判定、随机不变量与吞吐。
 *
 * @details 不依赖第三方测试框架：断言用 `Check()` 累计。随机用例按
 *          「容量 / 总条数 / last_seq」随机组合，用独立期望模型对照
 *          `Since()` 的 gap 判定与补发区间；吞吐用例给出条/秒。
 *
 * @author go2cpp contributors
 * @date 2026-11-04
 * @copyright MIT License
 */

#include "go2cpp/app/realtime_room.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

namespace {

using go2cpp::app::realtime::SeqJournal;

int g_checks = 0;
int g_failures = 0;

void Check(bool condition, const std::string& name) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::printf("  FAIL %s\n", name.c_str());
    }
}

/// @brief 从 `{"seq":N,...}` 里取出 N；格式不符返回 0（合法序号从 1 开始）。
std::uint64_t SeqOf(const std::string& text) {
    constexpr char kPrefix[] = "{\"seq\":";
    const std::string prefix(kPrefix);
    if (text.compare(0, prefix.size(), prefix) != 0) {
        return 0;
    }
    std::size_t pos = prefix.size();
    std::uint64_t value = 0;
    while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
        value = value * 10 + static_cast<std::uint64_t>(text[pos] - '0');
        ++pos;
    }
    if (pos >= text.size() || text[pos] != ',') {
        return 0;
    }
    return value;
}

/// @brief 取补发消息的 seq 序列，便于整体比较。
std::vector<std::uint64_t> SeqsOf(const std::vector<std::string>& messages) {
    std::vector<std::uint64_t> seqs;
    seqs.reserve(messages.size());
    for (const auto& message : messages) {
        seqs.push_back(SeqOf(message));
    }
    return seqs;
}

/// @brief 1. 序号从 1 开始单调递增，且注入到 JSON 首部、原字段保留。
void TestAssignContract() {
    std::printf("\n[1] Assign：序号注入与单调递增\n");
    SeqJournal journal;
    Check(journal.latest() == 0, "空 journal latest=0");
    Check(journal.earliest() == 0, "空 journal earliest=0");
    for (std::uint64_t i = 1; i <= 5; ++i) {
        std::uint64_t seq = 0;
        const std::string message = journal.Assign(
            "{\"type\":\"danmaku\",\"n\":" + std::to_string(i) + "}", &seq);
        Check(seq == i, "Assign seq=" + std::to_string(i));
        Check(SeqOf(message) == i, "消息注入 seq=" + std::to_string(i));
        Check(message.find("\"type\":\"danmaku\"") != std::string::npos,
              "原字段保留");
        Check(message.find("\"n\":" + std::to_string(i)) != std::string::npos,
              "原字段值保留");
    }
    Check(journal.latest() == 5, "latest=5");
    Check(journal.earliest() == 1, "earliest=1");
    Check(journal.size() == 5, "size=5");
    Check(journal.capacity() == SeqJournal::kDefaultCapacity, "默认容量 1000");
}

/// @brief 2. 正常区间补发：cap 足够时 (last_seq, latest] 全部取回。
void TestReplayRange() {
    std::printf("\n[2] Since：区间补发 6..15\n");
    SeqJournal journal;
    for (std::uint64_t i = 1; i <= 15; ++i) {
        journal.Assign("{\"type\":\"danmaku\",\"n\":" + std::to_string(i) + "}",
                       nullptr);
    }
    const auto replay = journal.Since(5);
    Check(!replay.gap, "last_seq=5 不 gap");
    Check(replay.earliest == 1, "earliest=1");
    Check(replay.latest == 15, "latest=15");
    Check(replay.messages.size() == 10, "补发 10 条");

    const std::vector<std::uint64_t> expected = {6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    Check(SeqsOf(replay.messages) == expected, "补发序号 6..15 且连续");

    const auto none = journal.Since(15);
    Check(!none.gap && none.messages.empty(), "last_seq=latest 补发为空");

    const auto from_zero = journal.Since(0);
    Check(!from_zero.gap, "last_seq=0 且缓冲完整：不 gap");
    Check(from_zero.messages.size() == 15, "last_seq=0 补发 1..15");
}

/// @brief 3. 环形淘汰：旧消息滚出后，last_seq 太旧必须 gap。
void TestRingEviction() {
    std::printf("\n[3] 环形淘汰与 gap\n");
    SeqJournal journal(4);
    Check(journal.capacity() == 4, "容量 4");
    for (std::uint64_t i = 1; i <= 10; ++i) {
        journal.Assign("{\"i\":" + std::to_string(i) + "}", nullptr);
    }
    Check(journal.earliest() == 7, "earliest=7（保留 7..10）");
    Check(journal.latest() == 10, "latest=10");
    Check(journal.size() == 4, "size=4");

    const auto too_old = journal.Since(5);
    Check(too_old.gap, "last_seq=5 已滚出：gap");
    Check(too_old.messages.empty(), "gap 时无补发消息");
    Check(too_old.earliest == 7, "gap 携带 earliest=7");

    const auto edge = journal.Since(6);
    Check(!edge.gap, "last_seq=6 恰好在窗口边界：不 gap");
    Check(SeqsOf(edge.messages) == std::vector<std::uint64_t>({7, 8, 9, 10}),
          "补发 7..10");

    const auto future = journal.Since(11);
    Check(future.gap, "last_seq 大于 latest（服务重启）：gap");
}

/// @brief 4. 空 journal 与容量 0 的边界。
void TestEmptyAndZeroCapacity() {
    std::printf("\n[4] 空 journal / 容量 0 边界\n");
    SeqJournal empty;
    const auto first = empty.Since(0);
    Check(!first.gap && first.messages.empty(), "空 journal last_seq=0 不 gap 无补发");
    const auto future = empty.Since(1);
    Check(future.gap, "空 journal last_seq=1 大于 latest=0：gap");
    Check(future.earliest == 0, "空 journal gap earliest=0");

    SeqJournal zero(0);
    Check(zero.capacity() == 1, "容量 0 归一为 1");
    for (int i = 0; i < 3; ++i) {
        zero.Assign("{\"i\":1}", nullptr);
    }
    Check(zero.size() == 1 && zero.earliest() == 3, "只保留最后一条");

    SeqJournal plain(4);
    const std::string raw = plain.Assign("not-a-json-object", nullptr);
    Check(raw == "not-a-json-object", "非 JSON object 原样返回");
    Check(plain.latest() == 1, "非 object 仍占用序号");
    Check(plain.Since(0).messages.size() == 1, "非 object 也进缓冲");
}

/// @brief 5. 默认容量下 1500 条：窗口滚到 501，last_seq=500 可补 1000 条。
void TestDefaultWindowOverflow() {
    std::printf("\n[5] 默认容量 1000 的满载窗口\n");
    SeqJournal journal;
    for (std::uint64_t i = 1; i <= 1500; ++i) {
        journal.Assign("{\"i\":" + std::to_string(i) + "}", nullptr);
    }
    Check(journal.earliest() == 501, "earliest=501");
    Check(journal.size() == 1000, "缓冲恰好 1000 条");
    const auto replay = journal.Since(500);
    Check(!replay.gap, "last_seq=500 在窗口内：不 gap");
    Check(replay.messages.size() == 1000, "补发 501..1500 共 1000 条");
    Check(SeqOf(replay.messages.front()) == 501, "首条 seq=501");
    Check(SeqOf(replay.messages.back()) == 1500, "末条 seq=1500");
    const auto gap = journal.Since(499);
    Check(gap.gap && gap.earliest == 501, "last_seq=499 gap earliest=501");
}

/// @brief 6. 随机组合：容量 / 总条数 / last_seq，用独立模型对照 gap 与区间。
void TestRandomized() {
    std::printf("\n[6] 随机化：容量×条数×last_seq\n");
    std::mt19937 rng(20261104U);
    int rounds = 300;
    int failures_before = g_failures;
    for (int round = 0; round < rounds; ++round) {
        const std::size_t capacity = 1 + static_cast<std::size_t>(rng() % 8);
        const std::size_t total = static_cast<std::size_t>(rng() % 40);
        SeqJournal journal(capacity);
        for (std::size_t i = 0; i < total; ++i) {
            journal.Assign("{\"i\":" + std::to_string(i) + "}", nullptr);
        }
        const std::uint64_t latest = static_cast<std::uint64_t>(total);
        const std::uint64_t earliest =
            total == 0 ? 0
                       : (total > capacity
                              ? static_cast<std::uint64_t>(total - capacity + 1)
                              : 1);
        const std::uint64_t last_seq =
            static_cast<std::uint64_t>(rng() % (total + 4));

        const auto replay = journal.Since(last_seq);
        const bool expect_gap =
            last_seq > latest || (earliest != 0 && last_seq + 1 < earliest);
        Check(replay.gap == expect_gap,
              "round " + std::to_string(round) + " gap 判定");
        if (expect_gap) {
            Check(replay.messages.empty(), "gap 无补发");
            continue;
        }
        std::vector<std::uint64_t> expected;
        for (std::uint64_t seq = (last_seq == 0 ? 1 : last_seq + 1);
             seq <= latest; ++seq) {
            if (earliest == 0 || seq >= earliest) {
                expected.push_back(seq);
            }
        }
        Check(SeqsOf(replay.messages) == expected,
              "round " + std::to_string(round) + " 补发区间");
        Check(replay.earliest == earliest && replay.latest == latest,
              "round " + std::to_string(round) + " earliest/latest");
    }
    std::printf("  %d 轮随机组合：%s\n", rounds,
                g_failures == failures_before ? "全部通过" : "存在失败");
}

/// @brief 7. 吞吐压测：O(1) 分配 + 固定窗口，给出条/秒。
void TestThroughput() {
    std::printf("\n[7] 吞吐：20 万条 Assign（窗口 1000）\n");
    constexpr std::size_t kTotal = 200000;
    SeqJournal journal;
    const auto begin = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < kTotal; ++i) {
        journal.Assign("{\"type\":\"danmaku\",\"i\":1}", nullptr);
    }
    const auto elapsed = std::chrono::steady_clock::now() - begin;
    const auto micros =
        std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
    const double per_second =
        micros == 0 ? 0.0
                    : static_cast<double>(kTotal) * 1e6 / static_cast<double>(micros);
    std::printf("  %zu 条 / %lld us = %.0f 条/秒，缓冲 %zu 条\n", kTotal,
                static_cast<long long>(micros), per_second, journal.size());
    Check(journal.size() == SeqJournal::kDefaultCapacity, "压测后缓冲仍为 1000");
    Check(journal.latest() == kTotal, "压测后 latest 正确");
    Check(per_second > 100000.0, "吞吐 > 10 万条/秒");
}

}  // namespace

int main() {
    std::printf("== realtime resume (SeqJournal) 测试 ==\n");
    TestAssignContract();
    TestReplayRange();
    TestRingEviction();
    TestEmptyAndZeroCapacity();
    TestDefaultWindowOverflow();
    TestRandomized();
    TestThroughput();
    std::printf("\n== 结果：%d 项检查，%d 失败 ==\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
