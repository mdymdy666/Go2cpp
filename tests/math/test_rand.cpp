/**
 * @file test_rand.cpp
 * @brief `go2cpp::math/rand` 的正式测试：已知向量 + 分布卡方 + 并发 + 真实场景 + 压测。
 *
 * 测试设计（按 PROJECT_RULES.md 第 3 节）：
 *   - 已知向量：seed=1 / seed=42 的前 10 个 `Uint64` 与**独立实现**（官方 xoshiro256**
 *     参考算法，Python 版）逐位对拍；
 *   - 随机分布：Intn/Int63n/Uint32/Uint64/Float64/Float32/Perm 的桶统计 + 卡方；
 *   - 并发：8 线程 × 10 万次全局 `Intn(100)`，验证全局锁下的总分布；
 *   - 真实场景：7 个（骰子、洗牌、无放回采样、掉宝概率、随机填充、负载均衡、确定性重放）；
 *   - 压测：全局/实例 `Intn(1000)` 各 1000 万次，报告吞吐。
 *
 * 卡方判定阈值取 `df + 6*sqrt(2*df)`（约 p < 1e-9 的单侧界，比常用的 p=0.001
 * 宽得多），避免把正常抖动误报为失败；同时把 chi2、阈值与最大桶偏差都打印出来。
 *
 * 注意：注释中不得出现连续的斜杠加星号（GCC -Wcomment）。
 */
#include "go2cpp/math/rand.hpp"
#include "test_math_env.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace r = go2cpp::rand;

namespace {

int g_failures = 0;

void Check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "[rand] FAILED: %s\n", what);
        ++g_failures;
    }
}

/// 官方 xoshiro256** 参考实现（Python 独立脚本）对 seed=1 生成的前 10 个输出。
const std::array<std::uint64_t, 10> kSeed1Vector = {
    0xb3f2af6d0fc710c5ULL, 0x853b559647364ceaULL, 0x92f89756082a4514ULL,
    0x642e1c7bc266a3a7ULL, 0xb27a48e29a233673ULL, 0x24c123126ffda722ULL,
    0x123004ef8df510e6ULL, 0x61954dcc47b1e89dULL, 0xddfdb48ab9ed4a21ULL,
    0x8d3cdb8c3aa5b1d0ULL,
};

/// 同一参考实现对 seed=42 生成的前 10 个输出。
const std::array<std::uint64_t, 10> kSeed42Vector = {
    0x15780b2e0c2ec716ULL, 0x6104d9866d113a7eULL, 0xae17533239e499a1ULL,
    0xecb8ad4703b360a1ULL, 0xfde6dc7fe2ec5e64ULL, 0xc50da53101795238ULL,
    0xb82154855a65ddb2ULL, 0xd99a2743ebe60087ULL, 0xc2e96e726e97647eULL,
    0x9556615f775fbc3dULL,
};

std::string Hex64(std::uint64_t value) {
    char buffer[32] = {};
    std::snprintf(buffer, sizeof(buffer), "0x%016llx",
                  static_cast<unsigned long long>(value));
    return std::string(buffer);
}

void CheckSequence(std::uint64_t seed, const std::array<std::uint64_t, 10>& expected) {
    r::Rand generator(seed);
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const std::uint64_t actual = generator.Uint64();
        if (actual != expected[i]) {
            std::fprintf(stderr, "[rand] FAILED: seed=%llu index=%zu actual=%s expected=%s\n",
                         static_cast<unsigned long long>(seed), i,
                         Hex64(actual).c_str(), Hex64(expected[i]).c_str());
            ++g_failures;
            return;
        }
    }
    std::printf("[rand][vector] seed=%llu 前 %zu 个 Uint64 与参考实现一致\n",
                static_cast<unsigned long long>(seed), expected.size());
}

template <typename Fn>
bool ThrowsInvalidArgument(Fn&& action) {
    try {
        action();
    } catch (const std::invalid_argument&) {
        return true;
    } catch (...) {
        return false;
    }
    return false;
}

struct BucketReport final {
    double chi2{0.0};
    double threshold{0.0};
    double max_deviation_percent{0.0};
    std::size_t degrees{0};
};

BucketReport AnalyzeBuckets(const std::vector<std::size_t>& counts,
                            const std::vector<double>& expected) {
    BucketReport report;
    if (counts.empty() || counts.size() != expected.size()) {
        return report;
    }
    report.degrees = counts.size() - 1;
    if (report.degrees > 0) {
        report.threshold =
            static_cast<double>(report.degrees) +
            6.0 * std::sqrt(2.0 * static_cast<double>(report.degrees));
    }
    for (std::size_t i = 0; i < counts.size(); ++i) {
        const double difference = static_cast<double>(counts[i]) - expected[i];
        report.chi2 += difference * difference / expected[i];
        const double deviation =
            std::fabs(difference) / expected[i] * 100.0;
        if (deviation > report.max_deviation_percent) {
            report.max_deviation_percent = deviation;
        }
    }
    return report;
}

void ReportBucketsExpected(const char* what, const std::vector<std::size_t>& counts,
                           const std::vector<double>& expected) {
    const BucketReport report = AnalyzeBuckets(counts, expected);
    std::printf(
        "[rand][chi2] %-38s buckets=%zu chi2=%.2f threshold(6sigma)=%.2f max_dev=%.2f%%\n",
        what, counts.size(), report.chi2, report.threshold,
        report.max_deviation_percent);
    if (report.degrees > 0 && report.chi2 > report.threshold) {
        std::fprintf(stderr, "[rand] FAILED: chi-square for %s: %.2f > %.2f\n", what,
                     report.chi2, report.threshold);
        ++g_failures;
    }
}

void ReportBuckets(const char* what, const std::vector<std::size_t>& counts,
                   double expected) {
    ReportBucketsExpected(what, counts,
                          std::vector<double>(counts.size(), expected));
}

double Entropy(const std::vector<std::size_t>& counts, std::size_t total) {
    double entropy = 0.0;
    for (std::size_t count : counts) {
        if (count == 0) {
            continue;
        }
        const double probability =
            static_cast<double>(count) / static_cast<double>(total);
        entropy -= probability * std::log2(probability);
    }
    return entropy;
}

bool IsPermutationOf(const std::vector<std::int64_t>& values, std::size_t n) {
    if (values.size() != n) {
        return false;
    }
    std::vector<char> seen(n, 0);
    for (std::int64_t value : values) {
        if (value < 0 || static_cast<std::size_t>(value) >= n) {
            return false;
        }
        const std::size_t index = static_cast<std::size_t>(value);
        if (seen[index] != 0) {
            return false;
        }
        seen[index] = 1;
    }
    return true;
}

/// 按 Go 的 read() 语义构造参考字节流：每次 Int63() 产出 7 字节（小端）。
std::vector<r::byte> ReferenceRead(r::Rand& generator, std::size_t size) {
    std::vector<r::byte> result;
    result.reserve(size);
    while (result.size() < size) {
        std::int64_t value = generator.Int63();
        for (int byte_index = 0; byte_index < 7 && result.size() < size; ++byte_index) {
            result.push_back(static_cast<r::byte>(value));
            value >>= 8;
        }
    }
    return result;
}

void RunKnownVectors() {
    CheckSequence(1, kSeed1Vector);
    CheckSequence(42, kSeed42Vector);
}

void RunSequenceChecks() {
    std::printf("[rand][sequence] 确定性 / 种子 / 派生 API 关系\n");

    r::Rand first(7);
    r::Rand second(7);
    bool identical = true;
    for (int i = 0; i < 1000; ++i) {
        if (first.Uint64() != second.Uint64()) {
            identical = false;
            break;
        }
    }
    Check(identical, "same seed -> same sequence");

    r::Rand different(8);
    r::Rand baseline(7);
    bool differs = false;
    for (int i = 0; i < 1000; ++i) {
        if (baseline.Uint64() != different.Uint64()) {
            differs = true;
            break;
        }
    }
    Check(differs, "different seed -> different sequence");

    r::Rand reseeded(123);
    std::array<std::uint64_t, 100> original{};
    for (auto& value : original) {
        value = reseeded.Uint64();
    }
    reseeded.Seed(123);
    bool replay_ok = true;
    for (std::uint64_t value : original) {
        if (reseeded.Uint64() != value) {
            replay_ok = false;
            break;
        }
    }
    Check(replay_ok, "Seed() resets the sequence");

    r::Rand source(99);
    r::Rand copy = source;
    bool copy_ok = true;
    for (int i = 0; i < 100; ++i) {
        if (source.Uint64() != copy.Uint64()) {
            copy_ok = false;
            break;
        }
    }
    Check(copy_ok, "copy of Rand continues the same stream");

    r::Rand uint32_left(42);
    r::Rand uint32_right(42);
    bool uint32_ok = true;
    for (int i = 0; i < 100000; ++i) {
        const std::uint32_t actual = uint32_left.Uint32();
        const std::uint32_t expected =
            static_cast<std::uint32_t>(uint32_right.Int63() >> 31);
        if (actual != expected) {
            uint32_ok = false;
            break;
        }
    }
    Check(uint32_ok, "Uint32 == uint32(Int63 >> 31)");

    r::Rand int_left(42);
    r::Rand int_right(42);
    bool int_ok = true;
    for (int i = 0; i < 100000; ++i) {
        if (int_left.Int() != int_right.Int63()) {
            int_ok = false;
            break;
        }
    }
    Check(int_ok, "Int() == Int63()");

    r::Rand uint64_left(42);
    r::Rand uint64_right(42);
    bool uint64_ok = true;
    for (int i = 0; i < 100000; ++i) {
        if ((uint64_left.Uint64() >> 1) !=
            static_cast<std::uint64_t>(uint64_right.Int63())) {
            uint64_ok = false;
            break;
        }
    }
    Check(uint64_ok, "Int63 == Uint64 >> 1");
}

void RunBoundsChecks() {
    std::printf("[rand][bounds] Intn / Int63n / Int63 的范围\n");
    r::Rand generator(2026);

    bool int63_ok = true;
    for (int i = 0; i < 1000000; ++i) {
        const std::int64_t value = generator.Int63();
        if (value < 0 ||
            static_cast<std::uint64_t>(value) >= (std::uint64_t{1} << 63)) {
            int63_ok = false;
            break;
        }
    }
    Check(int63_ok, "Int63 in [0, 2^63)");

    const std::array<std::int64_t, 8> intn_bounds = {
        1, 2, 6, 1000, 2147483647, (std::int64_t{1} << 31), (std::int64_t{1} << 31) + 1,
        std::numeric_limits<std::int64_t>::max()};
    bool intn_ok = true;
    for (std::int64_t bound : intn_bounds) {
        const int iterations = bound > 1000000000LL ? 10000 : 100000;
        for (int i = 0; i < iterations; ++i) {
            const std::int64_t value = generator.Intn(bound);
            if (value < 0 || value >= bound) {
                intn_ok = false;
            }
        }
    }
    Check(intn_ok, "Intn in [0, n) for all tested n");

    const std::array<std::int64_t, 7> int63n_bounds = {
        1, 2, 6, 1000, 1024, 1000000007, std::numeric_limits<std::int64_t>::max()};
    bool int63n_ok = true;
    for (std::int64_t bound : int63n_bounds) {
        const int iterations = bound > 1000000000LL ? 10000 : 100000;
        for (int i = 0; i < iterations; ++i) {
            const std::int64_t value = generator.Int63n(bound);
            if (value < 0 || value >= bound) {
                int63n_ok = false;
            }
        }
    }
    Check(int63n_ok, "Int63n in [0, n) for all tested n");

    r::Rand local(1);
    Check(ThrowsInvalidArgument([&] { local.Intn(0); }), "local Intn(0) throws");
    Check(ThrowsInvalidArgument([&] { local.Intn(-1); }), "local Intn(-1) throws");
    Check(ThrowsInvalidArgument([&] { local.Int63n(0); }), "local Int63n(0) throws");
    Check(ThrowsInvalidArgument([&] { local.Int63n(-3); }), "local Int63n(-3) throws");
    Check(ThrowsInvalidArgument([&] { local.Perm(-1); }), "local Perm(-1) throws");
    Check(ThrowsInvalidArgument(
              [&] { local.Shuffle(-1, [](std::int64_t, std::int64_t) {}); }),
          "local Shuffle(-1) throws");
    Check(ThrowsInvalidArgument([] { r::Intn(0); }), "global Intn(0) throws");
    Check(ThrowsInvalidArgument([] { r::Int63n(0); }), "global Int63n(0) throws");
    Check(ThrowsInvalidArgument([] { r::Perm(-1); }), "global Perm(-1) throws");
    Check(ThrowsInvalidArgument(
              [] { r::Shuffle(-1, [](std::int64_t, std::int64_t) {}); }),
          "global Shuffle(-1) throws");
}

void RunDistributionChecks() {
    std::printf("[rand][distribution] 桶统计 + 卡方（本地实例，固定种子）\n");
    constexpr int kDraws = 1000000;
    r::Rand generator(0x123456789ABCDEF0ULL);

    std::vector<std::size_t> intn(1000, 0);
    for (int i = 0; i < kDraws; ++i) {
        const std::int64_t value = generator.Intn(1000);
        ++intn[static_cast<std::size_t>(value)];
    }
    ReportBuckets("Intn(1000), 1M draws", intn, static_cast<double>(kDraws) / 1000.0);

    std::vector<std::size_t> int63n(1000, 0);
    for (int i = 0; i < kDraws; ++i) {
        const std::int64_t value = generator.Int63n(1000);
        ++int63n[static_cast<std::size_t>(value)];
    }
    ReportBuckets("Int63n(1000), 1M draws", int63n,
                  static_cast<double>(kDraws) / 1000.0);

    std::vector<std::size_t> low_byte(256, 0);
    for (int i = 0; i < kDraws; ++i) {
        const std::uint32_t value = generator.Uint32();
        ++low_byte[static_cast<std::size_t>(value & 0xFFU)];
    }
    ReportBuckets("Uint32 low byte, 1M draws", low_byte,
                  static_cast<double>(kDraws) / 256.0);

    std::vector<std::size_t> high_bits(1024, 0);
    for (int i = 0; i < kDraws; ++i) {
        const std::uint64_t value = generator.Uint64();
        ++high_bits[static_cast<std::size_t>(value >> 54)];
    }
    ReportBuckets("Uint64 high 10 bits, 1M draws", high_bits,
                  static_cast<double>(kDraws) / 1024.0);

    // Float64：10 个桶 + [0,1) 范围 + 均值
    constexpr int kFloatBuckets = 10;
    std::vector<std::size_t> float_buckets(kFloatBuckets, 0);
    double float_min = 1.0;
    double float_max = 0.0;
    double float_sum = 0.0;
    bool float_range_ok = true;
    for (int i = 0; i < kDraws; ++i) {
        const double value = generator.Float64();
        if (!(value >= 0.0 && value < 1.0)) {
            float_range_ok = false;
        }
        float_min = std::min(float_min, value);
        float_max = std::max(float_max, value);
        float_sum += value;
        ++float_buckets[static_cast<std::size_t>(value * kFloatBuckets)];
    }
    Check(float_range_ok, "Float64 in [0, 1)");
    const double float_mean = float_sum / static_cast<double>(kDraws);
    std::printf("[rand][float64] min=%.9f max=%.9f mean=%.9f\n", float_min, float_max,
                float_mean);
    Check(float_mean > 0.498 && float_mean < 0.502, "Float64 mean ~= 0.5");
    Check(float_min < 0.01 && float_max > 0.99, "Float64 covers both ends");
    ReportBuckets("Float64 into 10 buckets, 1M draws", float_buckets,
                  static_cast<double>(kDraws) / kFloatBuckets);

    std::vector<std::size_t> float32_buckets(kFloatBuckets, 0);
    double float32_sum = 0.0;
    bool float32_range_ok = true;
    for (int i = 0; i < kDraws; ++i) {
        const float value = generator.Float32();
        if (!(value >= 0.0f && value < 1.0f)) {
            float32_range_ok = false;
        }
        float32_sum += static_cast<double>(value);
        ++float32_buckets[static_cast<std::size_t>(static_cast<double>(value) *
                                                   kFloatBuckets)];
    }
    Check(float32_range_ok, "Float32 in [0, 1)");
    std::printf("[rand][float32] mean=%.9f\n", float32_sum / static_cast<double>(kDraws));
    Check(float32_sum / static_cast<double>(kDraws) > 0.495 &&
              float32_sum / static_cast<double>(kDraws) < 0.505,
          "Float32 mean ~= 0.5");
    ReportBuckets("Float32 into 10 buckets, 1M draws", float32_buckets,
                  static_cast<double>(kDraws) / kFloatBuckets);

    // Perm(52) 的首元素位置分布
    constexpr int kPermTrials = 10000;
    std::vector<std::size_t> perm_positions(52, 0);
    bool perm_ok = true;
    for (int trial = 0; trial < kPermTrials; ++trial) {
        const std::vector<std::int64_t> permutation = generator.Perm(52);
        if (!IsPermutationOf(permutation, 52)) {
            perm_ok = false;
        } else {
            ++perm_positions[static_cast<std::size_t>(permutation[0])];
        }
    }
    Check(perm_ok, "Perm(52) is a permutation in every trial");
    ReportBuckets("Perm(52) first element position, 10k", perm_positions,
                  static_cast<double>(kPermTrials) / 52.0);
}

void RunNormalExpChecks() {
    std::printf("[rand][normal] Int31 / Int31n / NormFloat64 / ExpFloat64\n");

    // Int31 的范围与派生关系。
    r::Rand generator(0x0BADC0DEULL);
    bool int31_range_ok = true;
    for (int i = 0; i < 1000000; ++i) {
        const std::int32_t value = generator.Int31();
        if (value < 0) {
            int31_range_ok = false;
            break;
        }
    }
    Check(int31_range_ok, "Int31 in [0, 2^31)");
    r::Rand int_left(42);
    r::Rand int_right(42);
    bool int31_relation_ok = true;
    for (int i = 0; i < 100000; ++i) {
        if (int_left.Int31() != static_cast<std::int32_t>(int_right.Int63() >> 32)) {
            int31_relation_ok = false;
            break;
        }
    }
    Check(int31_relation_ok, "Int31 == int32(Int63 >> 32)");

    // Int31n 的边界与均匀性。
    Check(generator.Int31n(1) == 0, "Int31n(1) == 0");
    Check(ThrowsInvalidArgument([] { r::Rand local(1); local.Int31n(0); }),
          "Int31n(0) throws");
    Check(ThrowsInvalidArgument([] { r::Rand local(1); local.Int31n(-1); }),
          "Int31n(-1) throws");
    std::vector<std::size_t> int31n(1000, 0);
    constexpr int kDraws = 1000000;
    for (int i = 0; i < kDraws; ++i) {
        const std::int32_t value = generator.Int31n(1000);
        if (value < 0 || value >= 1000) {
            Check(false, "Int31n(1000) out of range");
            break;
        }
        ++int31n[static_cast<std::size_t>(value)];
    }
    ReportBuckets("Int31n(1000), 1M draws", int31n,
                  static_cast<double>(kDraws) / 1000.0);

    // NormFloat64：均值/方差 + 两端覆盖 + 全部有限。
    constexpr int kNormalDraws = 200000;
    double norm_sum = 0.0;
    double norm_square_sum = 0.0;
    double norm_min = 1e9;
    double norm_max = -1e9;
    bool norm_finite = true;
    for (int i = 0; i < kNormalDraws; ++i) {
        const double value = generator.NormFloat64();
        if (!std::isfinite(value)) {
            norm_finite = false;
            continue;
        }
        norm_sum += value;
        norm_square_sum += value * value;
        norm_min = std::min(norm_min, value);
        norm_max = std::max(norm_max, value);
    }
    const double norm_mean = norm_sum / kNormalDraws;
    const double norm_variance = norm_square_sum / kNormalDraws - norm_mean * norm_mean;
    std::printf("[rand][normal] mean=%.6f variance=%.6f min=%.4f max=%.4f\n",
                norm_mean, norm_variance, norm_min, norm_max);
    Check(norm_finite, "NormFloat64 always finite");
    Check(std::fabs(norm_mean) < 0.02, "NormFloat64 mean ~= 0");
    Check(norm_variance > 0.95 && norm_variance < 1.05, "NormFloat64 variance ~= 1");
    Check(norm_min < -3.0 && norm_max > 3.0, "NormFloat64 covers both tails");

    // 缓存语义：Seed 必须清掉 Box–Muller 缓存的第二个值。
    r::Rand cached(5);
    (void)cached.NormFloat64();
    cached.Seed(5);
    r::Rand fresh(5);
    Check(cached.NormFloat64() == fresh.NormFloat64(),
          "Seed() clears the normal cache");
    r::Rand deterministic_left(77);
    r::Rand deterministic_right(77);
    bool norm_deterministic = true;
    for (int i = 0; i < 1000; ++i) {
        if (deterministic_left.NormFloat64() != deterministic_right.NormFloat64()) {
            norm_deterministic = false;
            break;
        }
    }
    Check(norm_deterministic, "NormFloat64 is deterministic for a fixed seed");

    // ExpFloat64：均值/方差 + 非负 + 两端覆盖。
    double exp_sum = 0.0;
    double exp_square_sum = 0.0;
    double exp_max = 0.0;
    bool exp_ok = true;
    for (int i = 0; i < kNormalDraws; ++i) {
        const double value = generator.ExpFloat64();
        if (!std::isfinite(value) || value < 0.0) {
            exp_ok = false;
            continue;
        }
        exp_sum += value;
        exp_square_sum += value * value;
        exp_max = std::max(exp_max, value);
    }
    const double exp_mean = exp_sum / kNormalDraws;
    const double exp_variance = exp_square_sum / kNormalDraws - exp_mean * exp_mean;
    std::printf("[rand][exp] mean=%.6f variance=%.6f max=%.4f\n", exp_mean,
                exp_variance, exp_max);
    Check(exp_ok, "ExpFloat64 always finite and non-negative");
    Check(std::fabs(exp_mean - 1.0) < 0.02, "ExpFloat64 mean ~= 1");
    Check(exp_variance > 0.95 && exp_variance < 1.05, "ExpFloat64 variance ~= 1");
    Check(exp_max > 5.0, "ExpFloat64 covers the tail");

    // 全局函数：播种后可复现，且范围正确。
    r::Seed(20261018);
    const double global_norm_a = r::NormFloat64();
    const double global_exp_a = r::ExpFloat64();
    const std::int32_t global_int31_a = r::Int31();
    r::Seed(20261018);
    Check(r::NormFloat64() == global_norm_a, "global NormFloat64 reproducible");
    Check(r::ExpFloat64() == global_exp_a, "global ExpFloat64 reproducible");
    Check(r::Int31() == global_int31_a, "global Int31 reproducible");
    const std::int32_t global_int31n = r::Int31n(7);
    Check(global_int31n >= 0 && global_int31n < 7, "global Int31n in range");
    Check(ThrowsInvalidArgument([] { r::Int31n(0); }), "global Int31n(0) throws");
}

void RunReadChecks() {
    std::printf("[rand][read] 字节流语义（与 Go 的 readVal/readPos 对齐）\n");

    r::Rand actual_generator(777);
    std::vector<r::byte> actual(23, 0);
    Check(actual_generator.Read(r::ByteSpan(actual)) == actual.size(),
          "Read returns len(p)");

    r::Rand reference_generator(777);
    const std::vector<r::byte> expected = ReferenceRead(reference_generator, actual.size());
    Check(actual == expected, "Read bytes == reference Int63 little-endian stream");

    r::Rand split_generator(777);
    std::vector<r::byte> part1(3, 0);
    std::vector<r::byte> part2(20, 0);
    split_generator.Read(r::ByteSpan(part1));
    split_generator.Read(r::ByteSpan(part2));
    std::vector<r::byte> joined = part1;
    joined.insert(joined.end(), part2.begin(), part2.end());
    Check(joined == actual, "split Read calls continue the byte stream");

    r::Rand empty_generator(9);
    r::Rand baseline_generator(9);
    std::vector<r::byte> nothing;
    Check(empty_generator.Read(r::ByteSpan(nothing)) == 0, "empty Read returns 0");
    Check(empty_generator.Uint64() == baseline_generator.Uint64(),
          "empty Read consumes no randomness");

    std::array<r::byte, 1024> buffer{};
    r::Seed(7);
    Check(r::Read(r::ByteSpan(buffer)) == buffer.size(), "global Read fills 1KB");
    std::size_t non_zero = 0;
    std::vector<std::size_t> byte_histogram(256, 0);
    for (r::byte value : buffer) {
        if (value != 0) {
            ++non_zero;
        }
        ++byte_histogram[static_cast<std::size_t>(value)];
    }
    std::printf("[rand][read] 1KB non-zero bytes=%zu, distinct=%zu\n", non_zero,
                static_cast<std::size_t>(
                    std::count_if(byte_histogram.begin(), byte_histogram.end(),
                                  [](std::size_t count) { return count != 0; })));
    Check(non_zero > 900, "1KB Read is not all zero");
    Check(std::count_if(byte_histogram.begin(), byte_histogram.end(),
                        [](std::size_t count) { return count != 0; }) > 200,
          "1KB Read has high byte diversity");

    std::vector<r::byte> large(1U << 20U, 0);
    r::Seed(11);
    r::Read(r::ByteSpan(large));
    std::vector<std::size_t> large_histogram(256, 0);
    for (r::byte value : large) {
        ++large_histogram[static_cast<std::size_t>(value)];
    }
    const double entropy = Entropy(large_histogram, large.size());
    std::printf("[rand][read] 1MiB entropy=%.6f bits/byte\n", entropy);
    Check(entropy > 7.9, "1MiB Read byte entropy > 7.9 bits");
    ReportBuckets("1MiB byte histogram", large_histogram,
                  static_cast<double>(large.size()) / 256.0);
}

void RunGlobalChecks() {
    std::printf("[rand][global] 全局函数的一致性与并发安全\n");

    // 全局 Seed 后应与同种子的本地实例产生相同序列
    r::Seed(12345);
    std::vector<std::uint64_t> global_sequence(32, 0);
    for (auto& value : global_sequence) {
        value = r::Uint64();
    }
    r::Rand local(12345);
    bool parity = true;
    for (std::uint64_t value : global_sequence) {
        if (local.Uint64() != value) {
            parity = false;
            break;
        }
    }
    Check(parity, "global Seed(x) matches Rand(x) sequence");

    // 全局 Read / Perm 与本地实例一致
    r::Seed(777);
    std::vector<r::byte> global_bytes(64, 0);
    r::Read(r::ByteSpan(global_bytes));
    r::Rand local_read(777);
    std::vector<r::byte> local_bytes(64, 0);
    local_read.Read(r::ByteSpan(local_bytes));
    Check(global_bytes == local_bytes, "global Read matches local Rand");

    r::Seed(808);
    const std::vector<std::int64_t> global_perm = r::Perm(52);
    r::Rand local_perm(808);
    Check(global_perm == local_perm.Perm(52), "global Perm matches local Rand");

    // 全局 Shuffle 的 swap 回调里再调用全局随机函数不应死锁
    r::Seed(909);
    std::vector<std::int64_t> global_deck(52);
    std::iota(global_deck.begin(), global_deck.end(), 0);
    const std::vector<std::int64_t> identity_deck = global_deck;
    r::Shuffle(52, [&](std::int64_t i, std::int64_t j) {
        volatile std::int64_t nested = r::Intn(2);
        (void)nested;
        std::swap(global_deck[static_cast<std::size_t>(i)],
                  global_deck[static_cast<std::size_t>(j)]);
    });
    Check(IsPermutationOf(global_deck, 52), "global Shuffle is a permutation");
    Check(global_deck != identity_deck, "global Shuffle changed the deck");

    bool bounds_ok = true;
    for (int i = 0; i < 100000; ++i) {
        const std::int64_t value = r::Intn(100);
        if (value < 0 || value >= 100) {
            bounds_ok = false;
        }
    }
    Check(bounds_ok, "global Intn bounds");
    Check(r::Int() >= 0, "global Int non-negative");
    const std::int64_t global_int63n = r::Int63n(1000);
    Check(global_int63n >= 0 && global_int63n < 1000, "global Int63n bounds");
    const double global_float64 = r::Float64();
    Check(global_float64 >= 0.0 && global_float64 < 1.0, "global Float64 in [0,1)");
    const float global_float32 = r::Float32();
    Check(global_float32 >= 0.0f && global_float32 < 1.0f, "global Float32 in [0,1)");
    Check(r::Int63() >= 0, "global Int63 non-negative");
    Check(r::Uint32() <= std::numeric_limits<std::uint32_t>::max(),
          "global Uint32 in range");

    // 并发：8 线程 × 10 万次全局 Intn(100)
    constexpr std::size_t kThreads = 8;
    constexpr std::size_t kPerThread = 100000;
    constexpr std::size_t kBins = 100;
    std::vector<std::vector<std::size_t>> per_thread(kThreads,
                                                     std::vector<std::size_t>(kBins, 0));
    std::atomic<std::size_t> violations{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    r::Seed(2025);
    for (std::size_t thread_index = 0; thread_index < kThreads; ++thread_index) {
        threads.emplace_back([&per_thread, &violations, thread_index] {
            for (std::size_t i = 0; i < kPerThread; ++i) {
                const std::int64_t value = r::Intn(static_cast<std::int64_t>(kBins));
                if (value < 0 || value >= static_cast<std::int64_t>(kBins)) {
                    violations.fetch_add(1, std::memory_order_relaxed);
                } else {
                    ++per_thread[thread_index][static_cast<std::size_t>(value)];
                }
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    std::vector<std::size_t> combined(kBins, 0);
    for (const auto& counts : per_thread) {
        for (std::size_t i = 0; i < kBins; ++i) {
            combined[i] += counts[i];
        }
    }
    Check(violations.load() == 0, "concurrent global Intn bounds");
    ReportBuckets("8 threads x 100k global Intn(100)", combined,
                  static_cast<double>(kThreads * kPerThread) / static_cast<double>(kBins));

    // 并发 Read：8 线程各读 1000 字节，验证不崩、不为全零
    std::array<std::array<r::byte, 1000>, 8> read_buffers{};
    std::vector<std::thread> read_threads;
    read_threads.reserve(8);
    for (std::size_t i = 0; i < 8; ++i) {
        read_threads.emplace_back(
            [&read_buffers, i] { r::Read(r::ByteSpan(read_buffers[i])); });
    }
    for (auto& thread : read_threads) {
        thread.join();
    }
    bool read_nonzero = true;
    for (const auto& buffer : read_buffers) {
        if (std::all_of(buffer.begin(), buffer.end(), [](r::byte value) {
                return value == 0;
            })) {
            read_nonzero = false;
        }
    }
    Check(read_nonzero, "concurrent global Read produces non-zero data");
}

void RunRealScenarios() {
    std::printf("[rand][scenarios] 真实场景\n");

    // 场景 1：骰子 60 万次
    {
        std::vector<std::size_t> faces(6, 0);
        r::Seed(31415);
        for (int i = 0; i < 600000; ++i) {
            ++faces[static_cast<std::size_t>(r::Intn(6))];
        }
        ReportBuckets("dice: Intn(6), 600k rolls", faces, 100000.0);
        std::printf("    face counts: %zu %zu %zu %zu %zu %zu\n", faces[0], faces[1],
                    faces[2], faces[3], faces[4], faces[5]);
    }

    // 场景 2：牌堆洗牌（Perm + Shuffle）
    {
        constexpr std::size_t kCards = 52;
        constexpr int kTrials = 10000;
        std::vector<std::size_t> positions(kCards, 0);
        bool permutation_ok = true;
        r::Rand generator(424242);
        for (int trial = 0; trial < kTrials; ++trial) {
            const std::vector<std::int64_t> deck = generator.Perm(kCards);
            if (!IsPermutationOf(deck, kCards)) {
                permutation_ok = false;
            } else {
                ++positions[static_cast<std::size_t>(deck.front())];
            }
        }
        for (int trial = 0; trial < kTrials; ++trial) {
            std::vector<std::int64_t> deck(kCards);
            std::iota(deck.begin(), deck.end(), 0);
            generator.Shuffle(kCards, [&](std::int64_t i, std::int64_t j) {
                std::swap(deck[static_cast<std::size_t>(i)],
                          deck[static_cast<std::size_t>(j)]);
            });
            if (!IsPermutationOf(deck, kCards)) {
                permutation_ok = false;
            } else {
                ++positions[static_cast<std::size_t>(deck.front())];
            }
        }
        Check(permutation_ok, "Perm/Shuffle always produce permutations");
        ReportBuckets("card 0 position after Perm+Shuffle (20k)", positions,
                      static_cast<double>(2 * kTrials) / static_cast<double>(kCards));
    }

    // 场景 3：无放回采样
    {
        constexpr std::size_t kPopulation = 10000;
        constexpr std::size_t kSample = 100;
        bool sample_ok = true;
        r::Rand generator(13579);
        std::vector<char> seen(kPopulation, 0);
        for (int trial = 0; trial < 200; ++trial) {
            std::fill(seen.begin(), seen.end(), 0);
            const std::vector<std::int64_t> permutation = generator.Perm(kPopulation);
            for (std::size_t i = 0; i < kSample; ++i) {
                const std::int64_t value = permutation[i];
                if (value < 0 || static_cast<std::size_t>(value) >= kPopulation ||
                    seen[static_cast<std::size_t>(value)] != 0) {
                    sample_ok = false;
                    break;
                }
                seen[static_cast<std::size_t>(value)] = 1;
            }
        }
        Check(sample_ok, "Perm(10000) first 100 are unique samples");
        std::printf("[rand][scenario] sampling: 200 trials x 100 from 10000, "
                    "all unique=%s\n",
                    sample_ok ? "yes" : "no");
    }

    // 场景 4：游戏掉宝概率
    {
        constexpr int kRolls = 1000000;
        std::vector<std::size_t> loot(4, 0);
        r::Seed(271828);
        for (int i = 0; i < kRolls; ++i) {
            const double roll = r::Float64();
            if (roll < 0.01) {
                ++loot[0];
            } else if (roll < 0.10) {
                ++loot[1];
            } else if (roll < 0.40) {
                ++loot[2];
            } else {
                ++loot[3];
            }
        }
        ReportBucketsExpected("loot: 1% / 9% / 30% / 60%, 1M",
                              loot, {10000.0, 90000.0, 300000.0, 600000.0});
        std::printf("    loot counts: legendary=%zu epic=%zu rare=%zu common=%zu\n",
                    loot[0], loot[1], loot[2], loot[3]);
    }

    // 场景 5：随机填充（1KB 不是全零 / 1MiB 熵 / 可复现）
    {
        std::array<r::byte, 256> first{};
        std::array<r::byte, 256> second{};
        r::Seed(99);
        r::Read(r::ByteSpan(first));
        r::Seed(99);
        r::Read(r::ByteSpan(second));
        Check(first == second, "Seed + Read is reproducible");
        std::printf("[rand][scenario] fill: 256-byte replay identical=%s\n",
                    first == second ? "yes" : "no");
    }

    // 场景 6：负载均衡（8 个 worker）
    {
        std::vector<std::size_t> workers(8, 0);
        r::Seed(1618033);
        for (int i = 0; i < 800000; ++i) {
            ++workers[static_cast<std::size_t>(r::Intn(8))];
        }
        ReportBuckets("load balance: 800k requests / 8 workers", workers, 100000.0);
    }

    // 场景 7：确定性重放（游戏/回放）
    {
        r::Seed(2024);
        std::vector<std::int64_t> replay(100, 0);
        for (auto& value : replay) {
            value = r::Intn(6);
        }
        r::Seed(2024);
        bool replay_ok = true;
        for (std::int64_t value : replay) {
            if (r::Intn(6) != value) {
                replay_ok = false;
                break;
            }
        }
        Check(replay_ok, "Seed(2024) replays the same 100 dice rolls");
        std::printf("[rand][scenario] replay: Seed(2024) 100 rolls identical=%s\n",
                    replay_ok ? "yes" : "no");
    }
}

template <typename Fn>
void Benchmark(const char* name, std::size_t iterations, Fn&& draw) {
    volatile std::uint64_t sink = 0;
    const auto start = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < iterations; ++i) {
        sink = sink + static_cast<std::uint64_t>(draw());
    }
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    const double throughput = static_cast<double>(iterations) / seconds / 1e6;
    const double nanoseconds = seconds * 1e9 / static_cast<double>(iterations);
    std::printf("[rand][bench] %-28s %zu 次：%.3f s，%.2f Mops/s，%.2f ns/op（sink=%llu）\n",
                name, iterations, seconds, throughput, nanoseconds,
                static_cast<unsigned long long>(sink));
    if (!(seconds > 0.0) || !(throughput > 0.0)) {
        std::fprintf(stderr, "[rand] FAILED: invalid benchmark measurement for %s\n", name);
        ++g_failures;
    }
}

void RunBenchmarks() {
    constexpr std::size_t kIterations = 10000000;
    std::printf("[rand][bench] 压测：每项 1000 万次（单线程，输入/输出不优化掉）\n");

    r::Seed(2024);
    Benchmark("global Intn(1000)", kIterations,
              [] { return r::Intn(1000); });

    r::Rand local(2024);
    Benchmark("local Rand Intn(1000)", kIterations,
              [&local] { return local.Intn(1000); });

    r::Seed(2024);
    Benchmark("global Uint64", kIterations, [] { return r::Uint64(); });
}

}  // namespace

int main() {
    std::printf("=== go2cpp::math/rand 模块测试 ===\n");
    go2cpp_math_test::PrintEnvironment("rand");

    RunKnownVectors();
    RunSequenceChecks();
    RunBoundsChecks();
    RunDistributionChecks();
    RunNormalExpChecks();
    RunReadChecks();
    RunGlobalChecks();
    RunRealScenarios();
    RunBenchmarks();

    if (g_failures == 0) {
        std::printf("\n[rand] failures=0\n");
    } else {
        std::printf("\n[rand] failures=%d\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
