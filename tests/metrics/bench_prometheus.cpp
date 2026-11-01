/**
 * @file bench_prometheus.cpp
 * @brief Prometheus 导出性能基准：1e6 指标、分位数后处理、memcpy 下限对比。
 *
 * 用法：
 *   ./.build-bento-wsl/go2cpp_metrics_prometheus_bench [--series=1000000]
 *                                                      [--iterations=3]
 *
 * 输出（每项取 iterations 次的最小值；两路 Registry 测量交替执行，抵消
 * 顺序/频率漂移）：
 *   - `writer`   ：`prom::TextWriter` 直接渲染 N 条样本（纯字符串拼接下限）；
 *   - `baseline` ：`Registry::Render()`（现有实现，不含分位数追加）；
 *   - `registry` ：`prom::Render`（= baseline + 全文分位数扫描，N 为 counter 时
 *                  只是扫描开销）；
 *   - `quantile` ：1e4 个 histogram 系列 × 10 桶的文本后处理（p50/p90/p99）；
 *   - `memcpy`   ：同字节数 `std::string::append` 的纯复制吞吐（物理下限参考）。
 *
 * 报告口径：ns/指标、MB/s、相对 memcpy 的倍数。
 */

#include "go2cpp/metrics/metrics.hpp"
#include "go2cpp/metrics/prometheus.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

double SecondsSince(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

struct Measurement {
    double seconds{0.0};
    std::size_t bytes{0};
    std::size_t metrics{0};
};

void Report(const char* name, const Measurement& measured) {
    const double ns_per_metric =
        measured.metrics == 0 ? 0.0
                              : measured.seconds * 1e9 /
                                    static_cast<double>(measured.metrics);
    const double mb_per_second =
        measured.seconds == 0.0 ? 0.0
                                : static_cast<double>(measured.bytes) /
                                      measured.seconds / (1024.0 * 1024.0);
    std::printf("%-10s %12zu metrics %10.3f ms %9.2f ns/metric %9.1f MB/s\n",
                name, measured.metrics, measured.seconds * 1e3, ns_per_metric,
                mb_per_second);
}

/// 生成 1e6 条样本的纯渲染基准（不含 Registry 遍历 / 锁）。
Measurement BenchWriter(std::size_t series, int iterations) {
    constexpr std::size_t kNames = 1000;
    std::vector<std::string> names(kNames);
    for (std::size_t i = 0; i < kNames; ++i) {
        names[i] = "bench_metric_" + std::to_string(i) + "_total";
    }
    const std::size_t labels_count = std::max<std::size_t>(series / kNames, 1);
    std::vector<std::string> labels(labels_count);
    for (std::size_t i = 0; i < labels_count; ++i) {
        labels[i] = std::to_string(i);
    }

    std::string out;
    out.reserve(series * 72);

    Measurement best;
    best.seconds = 1e9;
    for (int iteration = 0; iteration < iterations; ++iteration) {
        out.clear();
        const auto start = Clock::now();
        go2cpp::metrics::prom::TextWriter writer(out);
        for (std::size_t i = 0; i < series; ++i) {
            const std::string& name = names[i % kNames];
            const std::string& label = labels[(i / kNames) % labels_count];
            const go2cpp::metrics::prom::Label tags[] = {
                {"id", label.c_str()}};
            writer.Sample(name, go2cpp::metrics::prom::LabelView(tags, 1),
                          static_cast<std::uint64_t>(i));
        }
        const double seconds = SecondsSince(start);
        if (seconds < best.seconds) {
            best.seconds = seconds;
            best.bytes = out.size();
            best.metrics = series;
        }
    }
    Report("writer", best);
    return best;
}

/// 在 Registry 里建 N 个 counter 系列；交替测 baseline / exporter 两路。
struct RegistryBench {
    Measurement baseline;
    Measurement exporter;
};

RegistryBench BenchRegistry(std::size_t series, int iterations) {
    constexpr std::size_t kFamilies = 1000;
    const std::size_t per_family = std::max<std::size_t>(series / kFamilies, 1);
    go2cpp::metrics::Registry registry;
    std::vector<std::string> label_values(per_family);
    for (std::size_t i = 0; i < per_family; ++i) {
        label_values[i] = std::to_string(i);
    }
    for (std::size_t family = 0; family < kFamilies; ++family) {
        auto& counter = registry.GetCounter(
            "registry_metric_" + std::to_string(family) + "_total", "Bench.",
            {"id"});
        for (std::size_t i = 0; i < per_family; ++i) {
            counter.With({label_values[i]}).Add(static_cast<std::uint64_t>(i));
        }
    }

    go2cpp::metrics::prom::ExporterOptions options;
    options.include_quantiles = false;
    RegistryBench result;
    result.baseline.seconds = 1e9;
    result.exporter.seconds = 1e9;
    for (int iteration = 0; iteration < iterations; ++iteration) {
        const bool baseline_first = (iteration % 2) == 0;
        for (int pass = 0; pass < 2; ++pass) {
            const bool baseline_pass = (pass == 0) == baseline_first;
            const auto start = Clock::now();
            std::string text;
            if (baseline_pass) {
                text = registry.Render();
            } else {
                text = go2cpp::metrics::prom::Render(registry, options);
            }
            const double seconds = SecondsSince(start);
            Measurement& target = baseline_pass ? result.baseline : result.exporter;
            if (seconds < target.seconds) {
                target.seconds = seconds;
                target.bytes = text.size();
                target.metrics = per_family * kFamilies;
            }
        }
    }
    Report("baseline", result.baseline);
    Report("registry", result.exporter);
    return result;
}

/// 分位数后处理：1e4 个 histogram 系列 × 10 桶。
Measurement BenchQuantiles(int iterations) {
    constexpr std::size_t kSeries = 10000;
    constexpr std::size_t kBuckets = 10;
    std::string text;
    text.reserve(kSeries * (kBuckets + 3) * 64 + 1024);
    text.append("# TYPE bench_hist histogram\n");
    for (std::size_t series = 0; series < kSeries; ++series) {
        for (std::size_t bucket = 0; bucket < kBuckets; ++bucket) {
            text.append("bench_hist_bucket{id=\"");
            text.append(std::to_string(series));
            text.append("\",le=\"");
            text.append(std::to_string(bucket + 1));
            text.append("\"} ");
            text.append(std::to_string(bucket * 7 + 1));
            text.push_back('\n');
        }
        text.append("bench_hist_bucket{id=\"");
        text.append(std::to_string(series));
        text.append("\",le=\"+Inf\"} 100\n");
        text.append("bench_hist_sum{id=\"");
        text.append(std::to_string(series));
        text.append("\"} 12.5\n");
        text.append("bench_hist_count{id=\"");
        text.append(std::to_string(series));
        text.append("\"} 100\n");
    }

    go2cpp::metrics::prom::ExporterOptions options;
    Measurement best;
    best.seconds = 1e9;
    for (int iteration = 0; iteration < iterations; ++iteration) {
        std::string output = text;
        const auto start = Clock::now();
        const std::size_t added =
            go2cpp::metrics::prom::AppendQuantileFamilies(text, options,
                                                          output);
        const double seconds = SecondsSince(start);
        if (seconds < best.seconds && added > 0) {
            best.seconds = seconds;
            best.bytes = added;
            best.metrics = kSeries;
        }
    }
    Report("quantile", best);
    std::printf(
        "           (history 输入 %.2f MB, 追加 %.2f MB, %.1f ns/series)\n",
        static_cast<double>(text.size()) / (1024.0 * 1024.0),
        static_cast<double>(best.bytes) / (1024.0 * 1024.0),
        best.seconds * 1e9 / static_cast<double>(kSeries));
    return best;
}

/// 物理下限参考：同字节数的 `std::string::append`（memcpy）。
double BenchMemcpy(std::size_t bytes, int iterations) {
    std::string source(bytes, 'x');
    std::string destination;
    destination.reserve(bytes);
    double best = 1e9;
    for (int iteration = 0; iteration < iterations; ++iteration) {
        destination.clear();
        const auto start = Clock::now();
        destination.append(source);
        const double seconds = SecondsSince(start);
        if (seconds < best) {
            best = seconds;
        }
    }
    const double mb_per_second =
        static_cast<double>(bytes) / best / (1024.0 * 1024.0);
    std::printf("%-10s %8.2f MB %10.3f ms %9.1f MB/s\n", "memcpy",
                static_cast<double>(bytes) / (1024.0 * 1024.0), best * 1e3,
                mb_per_second);
    return mb_per_second;
}

}  // namespace

int main(int argc, char** argv) {
    std::size_t series = 1000000;
    int iterations = 3;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg.rfind("--series=", 0) == 0) {
            series = static_cast<std::size_t>(
                std::strtoull(std::string(arg.substr(9)).c_str(), nullptr, 10));
        } else if (arg.rfind("--iterations=", 0) == 0) {
            iterations = std::atoi(std::string(arg.substr(13)).c_str());
        }
    }
    if (series == 0) {
        series = 1000000;
    }
    if (iterations <= 0) {
        iterations = 3;
    }
    // 系列数按 1000 的倍数取整，保证每族数量一致。
    series = std::max<std::size_t>(series / 1000 * 1000, 1000);

    std::printf("[bench] series=%zu iterations=%d\n", series, iterations);

    const Measurement writer = BenchWriter(series, iterations);
    const RegistryBench registry = BenchRegistry(series, iterations);
    const Measurement quantile = BenchQuantiles(iterations);
    const double memcpy_mbps = BenchMemcpy(writer.bytes, iterations);

    const double writer_mbps =
        writer.seconds == 0.0 ? 0.0
                              : static_cast<double>(writer.bytes) /
                                    writer.seconds / (1024.0 * 1024.0);
    const double registry_mbps =
        registry.exporter.seconds == 0.0
            ? 0.0
            : static_cast<double>(registry.exporter.bytes) /
                  registry.exporter.seconds / (1024.0 * 1024.0);
    const double baseline_mbps =
        registry.baseline.seconds == 0.0
            ? 0.0
            : static_cast<double>(registry.baseline.bytes) /
                  registry.baseline.seconds / (1024.0 * 1024.0);
    std::printf(
        "[bench] writer vs memcpy: %.2fx; registry vs memcpy: %.2fx; "
        "baseline vs memcpy: %.2fx\n",
        memcpy_mbps / writer_mbps, memcpy_mbps / registry_mbps,
        memcpy_mbps / baseline_mbps);
    std::printf(
        "[bench] bytes: writer=%.2f MB registry=%.2f MB baseline=%.2f MB "
        "quantile=%.2f MB\n",
        static_cast<double>(writer.bytes) / (1024.0 * 1024.0),
        static_cast<double>(registry.exporter.bytes) / (1024.0 * 1024.0),
        static_cast<double>(registry.baseline.bytes) / (1024.0 * 1024.0),
        static_cast<double>(quantile.bytes) / (1024.0 * 1024.0));
    return 0;
}
