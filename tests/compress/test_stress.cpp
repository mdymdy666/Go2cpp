/**
 * @file test_stress.cpp
 * @brief 压测：1 MiB 低熵文本 vs 1 MiB 高熵随机字节的压缩率与吞吐。
 *
 * @details 这组数据是判断「实现是否合理」的核心：
 *
 *   - **低熵**（重复内容多的日志文本）：压缩率应远小于 1（本机实测约 0.09，
 *     即 ~11:1），级别 9 应优于级别 1；
 *   - **高熵**（`mt19937_64` 随机字节）：deflate 无法压缩，输出应略**大于**
 *     输入（stored block 有 5 字节/64 KiB 开销），比例约 1.00x；
 *   - 两者的差距必须显著。如果高熵数据也被「压」到 0.5，说明实现有 bug
 *     （例如丢数据）；如果低熵数据几乎不压缩，说明 Huffman/LZ77 没生效。
 *
 *   吞吐只做**报告**，不做严格阈值断言——不同机器（尤其 CI 容器）差异很大，
 *   用硬阈值会把环境问题误报成实现问题。唯一保留的吞吐断言是「> 1 MB/s」，
 *   用来抓「每次写 1 字节」这类灾难性退化。
 *
 *   计时方式：每种组合先预热 1 次，再重复 5 次取**中位数**，减少抖动影响。
 */
#include "compress_test_support.hpp"

#include <cstdio>

namespace {

using namespace go2cpp_compress_test;
namespace compress = go2cpp::compress;

constexpr std::size_t kDatasetSize = 1024U * 1024U;
constexpr int kIterations = 5;

struct Measurement {
  std::size_t compressed_size{0};
  double ratio{0.0};         ///< compressed / original
  double compress_mbps{0.0}; ///< MiB/s
  double decompress_mbps{0.0};
};

Measurement Measure(Format format, const std::string& data, int level) {
  Measurement measurement;
  std::vector<double> compress_ms;
  std::vector<double> decompress_ms;
  compress_ms.reserve(kIterations);
  decompress_ms.reserve(kIterations);
  for (int iteration = 0; iteration < kIterations + 1; ++iteration) {
    const auto compress_start = Clock::now();
    ErrorPtr error;
    const std::string compressed =
        CompressBytes(format, data, level, &error);
    const double compress_elapsed = MsSince(compress_start);
    COMPRESS_REQUIRE(!error);
    const auto decompress_start = Clock::now();
    ErrorPtr decompress_error;
    const std::string output =
        DecompressBytes(format, compressed, &decompress_error);
    const double decompress_elapsed = MsSince(decompress_start);
    COMPRESS_REQUIRE(!decompress_error);
    COMPRESS_REQUIRE(output == data);
    if (iteration == 0) {
      continue;  // 预热，不计入统计
    }
    compress_ms.push_back(compress_elapsed);
    decompress_ms.push_back(decompress_elapsed);
    measurement.compressed_size = compressed.size();
  }
  const double mib = static_cast<double>(data.size()) / (1024.0 * 1024.0);
  const double compress_median = MedianMs(compress_ms);
  const double decompress_median = MedianMs(decompress_ms);
  measurement.ratio =
      static_cast<double>(measurement.compressed_size) /
      static_cast<double>(data.empty() ? 1 : data.size());
  measurement.compress_mbps =
      compress_median > 0.0 ? mib / (compress_median / 1000.0) : 0.0;
  measurement.decompress_mbps =
      decompress_median > 0.0 ? mib / (decompress_median / 1000.0) : 0.0;
  return measurement;
}

double MeasureAndReport(Format format, const char* dataset, const std::string& data,
                        int level, double* compress_out,
                        double* decompress_out) {
  const Measurement measurement = Measure(format, data, level);
  if (compress_out) {
    *compress_out = measurement.compress_mbps;
  }
  if (decompress_out) {
    *decompress_out = measurement.decompress_mbps;
  }
  std::printf(
      "[compress] stress %-5s %-12s level=%2d: %7zu B, ratio=%.4f "
      "(%.2fx, %5.1f%% saved), compress=%7.2f MiB/s, decompress=%7.2f MiB/s\n",
      FormatName(format), dataset, level, measurement.compressed_size,
      measurement.ratio,
      measurement.ratio > 0.0 ? 1.0 / measurement.ratio : 0.0,
      100.0 * (1.0 - measurement.ratio), measurement.compress_mbps,
      measurement.decompress_mbps);
  // 任何组合的吞吐都不该低于 1 MiB/s（1 MiB 数据 1 秒以上 = 实现有问题）。
  COMPRESS_CHECK(measurement.compress_mbps > 1.0);
  COMPRESS_CHECK(measurement.decompress_mbps > 1.0);
  COMPRESS_CHECK(measurement.compressed_size > 0);
  return measurement.ratio;
}

}  // namespace

/// 压测入口。
void RunStressTests() {
  Announce("=== stress: 1 MiB low-entropy text vs 1 MiB high-entropy random ===");
  const std::string low = MakeLogText(kDatasetSize, 701);
  const std::string high = MakeRandomBytes(kDatasetSize, 702);
  COMPRESS_REQUIRE(low.size() == kDatasetSize);
  COMPRESS_REQUIRE(high.size() == kDatasetSize);

  const int levels[] = {compress::BestSpeed, compress::BestCompression};
  const Format formats[] = {Format::kFlate, Format::kZlib, Format::kGzip};

  double low_best_ratio = 1.0;
  double high_worst_ratio = 0.0;
  for (const Format format : formats) {
    for (const int level : levels) {
      const double low_ratio =
          MeasureAndReport(format, "low-entropy", low, level, nullptr, nullptr);
      const double high_ratio = MeasureAndReport(format, "high-entropy", high,
                                                 level, nullptr, nullptr);
      // 低熵必须真的被压缩（远小于 0.5）。
      COMPRESS_CHECK(low_ratio < 0.5);
      // 高熵：deflate 无法压缩，允许 1% 的封装开销、不允许明显膨胀。
      COMPRESS_CHECK(high_ratio > 0.95);
      COMPRESS_CHECK(high_ratio < 1.02);
      // 低熵与高熵的差距必须显著（至少 4 倍），否则说明压缩没生效。
      COMPRESS_CHECK(high_ratio > low_ratio * 4.0);
      if (low_ratio < low_best_ratio) {
        low_best_ratio = low_ratio;
      }
      if (high_ratio > high_worst_ratio) {
        high_worst_ratio = high_ratio;
      }
    }
  }
  // 级别 9 应不劣于级别 1（逐格式比较，日志文本上通常明显更好）。
  for (const Format format : formats) {
    const Measurement speed = Measure(format, low, compress::BestSpeed);
    const Measurement best = Measure(format, low, compress::BestCompression);
    COMPRESS_CHECK(best.ratio <= speed.ratio + 0.02);
    std::printf(
        "[compress] stress %-5s low-entropy level-9 vs level-1: ratio %.4f vs "
        "%.4f, compress %.2f vs %.2f MiB/s\n",
        FormatName(format), best.ratio, speed.ratio, best.compress_mbps,
        speed.compress_mbps);
  }
  std::printf(
      "[compress] stress summary: low-entropy best ratio=%.4f (~%.1fx), "
      "high-entropy worst ratio=%.4f, gap=%.1fx\n",
      low_best_ratio, low_best_ratio > 0.0 ? 1.0 / low_best_ratio : 0.0,
      high_worst_ratio,
      low_best_ratio > 0.0 ? high_worst_ratio / low_best_ratio : 0.0);
}
