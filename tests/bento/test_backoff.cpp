/**
 * @file test_backoff.cpp
 * @brief MOD-12 指数退避：序列递增/上限/抖动范围/确定性/Reset/参数规范化。
 * @author go2cpp contributors
 * @date 2026-10-28
 * @copyright MIT License
 */

#include <array>
#include <algorithm>
#include <cstdint>
#include <cstdio>

#include "go2cpp/bento/net/backoff.hpp"
#include "test_util.hpp"

namespace {

using namespace go2cpp::bento;

void TestDoublingSequence() {
    Backoff::Config config;
    config.base_ms = 1000;
    config.max_ms = 30000;
    config.jitter_ratio = 0.0;
    config.seed = 42;
    Backoff backoff(config);

    const std::array<std::uint32_t, 8> expected{1000, 2000, 4000, 8000, 16000, 30000, 30000, 30000};
    std::uint32_t previous = 0;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const std::uint32_t actual = backoff.Next();
        BENTO_CHECK_EQ(actual, expected[i]);
        BENTO_CHECK(actual >= previous);
        previous = actual;
        BENTO_CHECK_EQ(backoff.Attempts(), static_cast<std::uint32_t>(i + 1));
    }
}

void TestMaxCapWithJitter() {
    Backoff::Config config;
    config.jitter_ratio = 0.2;
    config.seed = 7;
    Backoff backoff(config);
    for (int i = 0; i < 100; ++i) {
        const std::uint32_t delay = backoff.Next();
        BENTO_CHECK(delay >= 1);
        BENTO_CHECK(delay <= config.max_ms);
        if (i >= 5) {
            // 第 5 次名义值已到 30s 上限；之后抖动只能向下，绝不越过上限。
            BENTO_CHECK_EQ(backoff.PeekNominalMs(), config.max_ms);
            BENTO_CHECK(delay >= 24000);
        }
    }
}

void TestJitterRange() {
    const std::array<std::uint64_t, 5> seeds{1, 2, 3, 12345, 0xdeadbeefull};
    for (const std::uint64_t seed : seeds) {
        Backoff::Config config;
        config.base_ms = 1000;
        config.max_ms = 30000;
        config.jitter_ratio = 0.2;
        config.seed = seed;
        Backoff backoff(config);
        for (int i = 0; i < 12; ++i) {
            const std::uint32_t nominal = backoff.PeekNominalMs();
            const std::uint32_t delay = backoff.Next();
            // 允许 ±1ms 的取整误差。
            const double low = static_cast<double>(nominal) * 0.8 - 1.0;
            const double high =
                std::min(30000.0, static_cast<double>(nominal) * 1.2) + 1.0;
            BENTO_CHECK(static_cast<double>(delay) >= low);
            BENTO_CHECK(static_cast<double>(delay) <= high);
        }
    }
}

void TestDeterministicSeed() {
    Backoff::Config first_config;
    first_config.seed = 99;
    first_config.jitter_ratio = 0.25;
    Backoff first(first_config);
    Backoff second(first_config);
    for (int i = 0; i < 10; ++i) {
        BENTO_CHECK_EQ(first.Next(), second.Next());
    }

    Backoff::Config other_config = first_config;
    other_config.seed = 100;
    Backoff other(other_config);
    Backoff reference(first_config);
    bool any_different = false;
    for (int i = 0; i < 10; ++i) {
        if (reference.Next() != other.Next()) {
            any_different = true;
        }
    }
    BENTO_CHECK(any_different);
}

void TestReset() {
    Backoff::Config config;
    config.seed = 555;
    config.jitter_ratio = 0.3;
    Backoff backoff(config);
    std::array<std::uint32_t, 3> first{};
    for (auto& value : first) {
        value = backoff.Next();
    }
    BENTO_CHECK_EQ(backoff.Attempts(), 3U);

    backoff.Reset();
    BENTO_CHECK_EQ(backoff.Attempts(), 0U);
    BENTO_CHECK_EQ(backoff.PeekNominalMs(), config.base_ms);
    for (const std::uint32_t expected : first) {
        BENTO_CHECK_EQ(backoff.Next(), expected);
    }
}

void TestConfigNormalization() {
    Backoff::Config degenerate;
    degenerate.base_ms = 0;
    degenerate.max_ms = 0;
    degenerate.jitter_ratio = 2.5;
    Backoff clamped(degenerate);
    BENTO_CHECK_EQ(clamped.BaseMs(), 1U);
    BENTO_CHECK_EQ(clamped.MaxMs(), 1U);
    BENTO_CHECK(clamped.JitterRatio() <= 1.0);
    BENTO_CHECK(clamped.JitterRatio() >= 0.0);
    BENTO_CHECK_EQ(clamped.Next(), 1U);

    Backoff::Config negative_jitter;
    negative_jitter.base_ms = 1000;
    negative_jitter.max_ms = 30000;
    negative_jitter.jitter_ratio = -0.5;
    Backoff no_jitter(negative_jitter);
    BENTO_CHECK_EQ(no_jitter.JitterRatio(), 0.0);
    BENTO_CHECK_EQ(no_jitter.Next(), 1000U);

    // max < base：按 base 处理，否则首个退避就会超过上限，语义自相矛盾。
    Backoff::Config inverted;
    inverted.base_ms = 5000;
    inverted.max_ms = 100;
    inverted.jitter_ratio = 0.0;
    Backoff capped(inverted);
    BENTO_CHECK_EQ(capped.MaxMs(), 5000U);
    BENTO_CHECK_EQ(capped.Next(), 5000U);
}

void TestPeekNominal() {
    Backoff::Config config;
    config.base_ms = 250;
    config.max_ms = 10000;
    config.jitter_ratio = 0.0;
    Backoff backoff(config);
    BENTO_CHECK_EQ(backoff.PeekNominalMs(), 250U);
    BENTO_CHECK_EQ(backoff.Attempts(), 0U);
    BENTO_CHECK_EQ(backoff.Next(), 250U);
    BENTO_CHECK_EQ(backoff.PeekNominalMs(), 500U);
    BENTO_CHECK_EQ(backoff.Attempts(), 1U);
}

}  // namespace

int main() {
    TestDoublingSequence();
    TestMaxCapWithJitter();
    TestJitterRange();
    TestDeterministicSeed();
    TestReset();
    TestConfigNormalization();
    TestPeekNominal();
    return bento_test::Finish("test_backoff");
}
