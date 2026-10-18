/**
 * @file test_hash_stress.cpp
 * @brief 压测：1 GiB 数据流，量化 CRC32 / Adler32 / SHA-256 的吞吐（MB/s）。
 *
 * ## 条件
 * - Release（库 `-O3`，测试 TU `-O2`），单线程；
 * - 输入为 4 MiB 的确定性缓冲（LCG 生成），重复喂入到 1 GiB；不分配 1 GiB 连续内存，
 *   避免把「内存带宽」误当成「哈希吞吐」；
 * - 每个算法先跑 8 MiB 预热，再计时 1 GiB；
 * - MB/s 同时给出 MiB/s（1 MiB = 1,048,576 B）与 MB/s（1 MB = 1,000,000 B）。
 *
 * ## 验收
 * - CRC32（IEEE）查表 + slicing-by-8 的实测值应显著高于 1 GB/s；
 * - SHA-256 为纯标量实现（未用 SHA-NI），200~500 MB/s 属预期区间；
 * - 只对「灾难性退化」设下限（CRC32 ≥ 1000 MB/s、SHA-256 ≥ 100 MB/s），
 *   避免不同机器/负载下把性能抖动误报成功能失败；真实数值见输出与交付报告。
 */
#include "hash_test_support.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// 性能下限只在**未插桩**构建里断言：ASan/TSan/UBSan 会把查表与轮函数拖慢
// 2~4 倍（实测 ASan 下 CRC32 只有 800 MB/s），此时用固定吞吐阈值判失败
// 是误报，不是功能退化。插桩时仍打印实测值供人工比对。
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define GO2CPP_HASH_INSTRUMENTED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define GO2CPP_HASH_INSTRUMENTED 1
#endif
#endif
#ifndef GO2CPP_HASH_INSTRUMENTED
#define GO2CPP_HASH_INSTRUMENTED 0
#endif

namespace {

using namespace go2cpp::hash;
using Clock = go2cpp_hash_test::Clock;

constexpr std::uint64_t kGigaByte = 1024ull * 1024ull * 1024ull;
constexpr std::size_t kBufferBytes = 4u * 1024u * 1024u;

struct Measurement {
    double mib_per_second = 0.0;
    double mb_per_second = 0.0;
    std::vector<std::uint8_t> digest;
};

/// 把 `buffer` 重复喂入 `total_bytes`，计时并返回摘要。
template <typename H>
Measurement MeasureStream(ByteSpan buffer, std::uint64_t total_bytes) {
    H hash;
    const auto start = Clock::now();
    std::uint64_t written = 0;
    while (written < total_bytes) {
        hash.Write(buffer);
        written += buffer.size();
    }
    const double seconds = go2cpp_hash_test::SecondsSince(start);

    Measurement result;
    result.digest.resize(hash.Size());
    hash.Sum(ByteSpan(result.digest.data(), result.digest.size()));
    result.mib_per_second = go2cpp_hash_test::MibPerSecond(total_bytes, seconds);
    result.mb_per_second = go2cpp_hash_test::MbPerSecond(total_bytes, seconds);
    return result;
}

template <typename H>
void WarmUp(ByteSpan buffer) {
    H hash;
    for (int i = 0; i < 2; ++i) {
        hash.Write(buffer);
    }
}

void Report(const char* name, const Measurement& result,
            std::uint64_t total_bytes) {
    std::printf("[hash]   %-14s %8.2f MiB/s  %8.2f MB/s  (%llu B, digest %s)\n",
                name, result.mib_per_second, result.mb_per_second,
                static_cast<unsigned long long>(total_bytes),
                go2cpp_hash_test::Hex(AsBytes(result.digest)).c_str());
    std::fflush(stdout);
}

}  // namespace

void RunStressTests() {
    go2cpp_hash_test::Announce(
        "=== stress: 1 GiB streaming throughput (single thread, Release) ===");
    const std::vector<std::uint8_t> buffer =
        go2cpp_hash_test::Generate(kBufferBytes, 0x9e3779b97f4a7c15ULL);
    const ByteSpan span = AsBytes(buffer);

    WarmUp<Crc32>(span);
    WarmUp<Adler32>(span);
    WarmUp<Sha256>(span);
    WarmUp<Md5>(span);
    WarmUp<Sha1>(span);

    std::printf("[hash] streaming 1 GiB through a %zu-byte buffer\n",
                kBufferBytes);

    const Measurement crc = MeasureStream<Crc32>(span, kGigaByte);
    Report("crc32(IEEE)", crc, kGigaByte);

    const Measurement adler = MeasureStream<Adler32>(span, kGigaByte);
    Report("adler32", adler, kGigaByte);

    const Measurement sha256 = MeasureStream<Sha256>(span, kGigaByte);
    Report("sha256", sha256, kGigaByte);

    // MD5 / SHA-1 只作为参考，用 256 MiB 以免测试总时长过长。
    constexpr std::uint64_t kQuarterGigaByte = kGigaByte / 4;
    const Measurement md5 = MeasureStream<Md5>(span, kQuarterGigaByte);
    Report("md5 (256 MiB)", md5, kQuarterGigaByte);

    const Measurement sha1 = MeasureStream<Sha1>(span, kQuarterGigaByte);
    Report("sha1 (256 MiB)", sha1, kQuarterGigaByte);

    // 功能一致性：流式 1 GiB 的 CRC 必须等于「先算 4 MiB 缓冲的 CRC，再把同一
    // 缓冲追加 255 次」的 Update 链——防止压测循环写错（例如少喂一块）。
    std::uint32_t chained = 0;
    const std::uint32_t block_crc = Crc32::ChecksumIEEE(span);
    for (std::uint64_t done = 0; done < kGigaByte; done += buffer.size()) {
        if (done == 0) {
            chained = block_crc;
        } else {
            chained = Crc32::Update(chained, IEEETable, span);
        }
    }
    HASH_CHECK(chained ==
               ((static_cast<std::uint32_t>(crc.digest[0]) << 24) |
                (static_cast<std::uint32_t>(crc.digest[1]) << 16) |
                (static_cast<std::uint32_t>(crc.digest[2]) << 8) |
                static_cast<std::uint32_t>(crc.digest[3])));

    // 灾难性退化下限（打印的实测值才是验收依据）。插桩构建跳过，见文件头说明。
#if !GO2CPP_HASH_INSTRUMENTED
    HASH_CHECK(crc.mb_per_second >= 1000.0);
    HASH_CHECK(adler.mb_per_second >= 500.0);
    HASH_CHECK(sha256.mb_per_second >= 100.0);
    HASH_CHECK(md5.mb_per_second >= 100.0);
    HASH_CHECK(sha1.mb_per_second >= 100.0);
#else
    std::printf("[hash] instrumented build: throughput thresholds skipped\n");
#endif
}
