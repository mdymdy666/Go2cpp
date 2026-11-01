/**
 * @file measure.hpp
 * @brief 启动开销基准的计时与内存采样工具（header-only）。
 *
 * ## 为什么不用 std::chrono 直接测
 *
 * 池初始化的量级从「80 ns（一次 LruKCache 构造）」到「数百 µs（一个
 * Scheduler）」不等。`steady_clock` 的一次 `clock_gettime` 在 WSL2 上约
 * 20~25 ns，对 ns 级样本是可观噪声。这里用 `rdtsc`（x86 上约 7 ns）并在
 * 启动时用 `CLOCK_MONOTONIC` 校准 TSC 频率；校准误差超过 2% 时自动退回
 * `clock_gettime`，保证换机器不会静默给出错数。
 *
 * ## 为什么同时读 VmRSS 与 mallinfo2
 *
 * - `VmRSS` 是内核视角的真实常驻页，但精度是 4 KiB 页；对 WaitNode
 *   （约 200 B/节点）根本看不见；
 * - `mallinfo2().uordblks + hblkhd` 是 glibc 视角的已分配字节，精度到字节，
 *   但会被分配器缓存（free 不归还）污染；
 * - 两者一起看：RSS 说明「真正占了多少物理内存」，heap 说明「池向分配器
 *   要了多少」。只用其中一个都会误判。
 *
 * @note 本文件不依赖项目任何模块，可独立编译。
 */
#pragma once

#include <malloc.h>
#include <time.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#if defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#define STARTUP_COST_HAS_RDTSC 1
#else
#define STARTUP_COST_HAS_RDTSC 0
#endif

namespace startup_cost {

// ---------------------------------------------------------------------------
// 时间
// ---------------------------------------------------------------------------

/// @brief 单调时钟纳秒（`CLOCK_MONOTONIC`）。
inline std::uint64_t MonotonicNs() noexcept {
    timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ULL +
           static_cast<std::uint64_t>(ts.tv_nsec);
}

/// @brief 本线程 CPU 纳秒；用于区分「墙钟等待」与「真的在算」。
inline std::uint64_t ThreadCpuNs() noexcept {
    timespec ts{};
    ::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ULL +
           static_cast<std::uint64_t>(ts.tv_nsec);
}

/// @brief TSC → 纳秒换算器；校准失败时 `valid()==false`，调用方用墙钟。
class TscClock {
public:
    static TscClock Calibrate() noexcept {
        TscClock clock;
#if STARTUP_COST_HAS_RDTSC
        // 三轮 5 ms 校准，取中位；TSC 与 CLOCK_MONOTONIC 都是单调的，
        // 单轮已够准，多轮用于剔掉被调度打断的样本。
        double best = 0.0;
        std::vector<double> samples;
        samples.reserve(3);
        for (int round = 0; round < 3; ++round) {
            const std::uint64_t t0 = MonotonicNs();
            const std::uint64_t c0 = __rdtsc();
            // 忙等 5 ms：sleep 会被调度器打断，且 WSL2 的 sleep 精度差。
            while (MonotonicNs() - t0 < 5'000'000ULL) {
            }
            const std::uint64_t c1 = __rdtsc();
            const std::uint64_t t1 = MonotonicNs();
            const double ns = static_cast<double>(t1 - t0);
            const double ticks = static_cast<double>(c1 - c0);
            if (ns > 0.0 && ticks > 0.0) {
                samples.push_back(ns / ticks);
            }
        }
        if (samples.size() == 3) {
            std::sort(samples.begin(), samples.end());
            best = samples[1];
            // 三轮离散度 >2% 说明 TSC 不稳（或虚拟化时钟跳变），退回墙钟。
            if (samples[2] - samples[0] <= 0.02 * best) {
                clock.ns_per_tick_ = best;
                clock.valid_ = true;
            }
        }
#else
        (void)clock;
#endif
        return clock;
    }

    /// @brief 用父进程校准好的频率构造（避免每次 exec 重新校准）。
    static TscClock FromNsPerTick(double ns_per_tick) noexcept {
        TscClock clock;
#if STARTUP_COST_HAS_RDTSC
        if (ns_per_tick > 0.0) {
            clock.ns_per_tick_ = ns_per_tick;
            clock.valid_ = true;
        }
#else
        (void)ns_per_tick;
#endif
        return clock;
    }

    double ns_per_tick() const noexcept { return ns_per_tick_; }

    bool valid() const noexcept { return valid_; }

    /// @brief 读一次当前时刻（纳秒）；TSC 不可用时退回墙钟。
    std::uint64_t NowNs() const noexcept {
#if STARTUP_COST_HAS_RDTSC
        if (valid_) {
            return static_cast<std::uint64_t>(
                static_cast<double>(__rdtsc()) * ns_per_tick_);
        }
#endif
        return MonotonicNs();
    }

private:
    double ns_per_tick_{1.0};
    bool valid_{false};
};

// ---------------------------------------------------------------------------
// 内存
// ---------------------------------------------------------------------------

/// @brief 一次内存快照。
struct MemSnapshot {
    std::uint64_t vm_kib = 0;      ///< /proc/self/status VmSize
    std::uint64_t rss_kib = 0;     ///< /proc/self/status VmRSS
    std::uint64_t hwm_kib = 0;     ///< /proc/self/status VmHWM
    std::uint64_t heap_bytes = 0;  ///< mallinfo2 uordblks + hblkhd
    std::uint64_t brk_bytes = 0;   ///< mallinfo2 arena（brk 区）
    std::uint64_t mmap_bytes = 0;  ///< mallinfo2 hblkhd
};

/// @brief 从 /proc/self/status 读一个以 KiB 为单位的字段，失败返回 0。
inline std::uint64_t StatusKiB(const char* key) noexcept {
    FILE* status = std::fopen("/proc/self/status", "r");
    if (status == nullptr) {
        return 0;
    }
    char line[256];
    std::uint64_t value = 0;
    const std::size_t key_length = std::strlen(key);
    while (std::fgets(line, sizeof(line), status) != nullptr) {
        if (std::strncmp(line, key, key_length) == 0) {
            (void)std::sscanf(line + key_length, "%llu",
                              reinterpret_cast<unsigned long long*>(&value));
            break;
        }
    }
    std::fclose(status);
    return value;
}

/// @brief 采集当前进程的内存快照。
inline MemSnapshot Capture() noexcept {
    MemSnapshot snapshot;
    snapshot.vm_kib = StatusKiB("VmSize:");
    snapshot.rss_kib = StatusKiB("VmRSS:");
    snapshot.hwm_kib = StatusKiB("VmHWM:");
    const struct mallinfo2 info = ::mallinfo2();
    snapshot.brk_bytes = static_cast<std::uint64_t>(info.arena);
    snapshot.mmap_bytes = static_cast<std::uint64_t>(info.hblkhd);
    snapshot.heap_bytes = static_cast<std::uint64_t>(info.uordblks) +
                          static_cast<std::uint64_t>(info.hblkhd);
    return snapshot;
}

// ---------------------------------------------------------------------------
// 统计
// ---------------------------------------------------------------------------

/// @brief 返回排序后的中位；空返回 0。
inline double Median(std::vector<double> values) {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const std::size_t n = values.size();
    if ((n & 1U) != 0U) {
        return values[n / 2];
    }
    return 0.5 * (values[n / 2 - 1] + values[n / 2]);
}

/// @brief 返回排序后的第 p 百分位（p ∈ [0,1]）；空返回 0。
inline double Percentile(std::vector<double> values, double p) {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    if (p <= 0.0) {
        return values.front();
    }
    if (p >= 1.0) {
        return values.back();
    }
    const double pos = p * static_cast<double>(values.size() - 1);
    const std::size_t lo = static_cast<std::size_t>(pos);
    const std::size_t hi = std::min(lo + 1, values.size() - 1);
    const double frac = pos - static_cast<double>(lo);
    return values[lo] * (1.0 - frac) + values[hi] * frac;
}

// ---------------------------------------------------------------------------
// 人类可读格式
// ---------------------------------------------------------------------------

inline std::string HumanBytes(std::uint64_t bytes) {
    char buffer[64];
    if (bytes < 1024ULL) {
        std::snprintf(buffer, sizeof(buffer), "%llu B",
                      static_cast<unsigned long long>(bytes));
    } else if (bytes < 1024ULL * 1024ULL) {
        std::snprintf(buffer, sizeof(buffer), "%.1f KiB",
                      static_cast<double>(bytes) / 1024.0);
    } else if (bytes < 1024ULL * 1024ULL * 1024ULL) {
        std::snprintf(buffer, sizeof(buffer), "%.2f MiB",
                      static_cast<double>(bytes) / (1024.0 * 1024.0));
    } else {
        std::snprintf(buffer, sizeof(buffer), "%.2f GiB",
                      static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
    }
    return std::string(buffer);
}

inline std::string HumanNs(double ns) {
    char buffer[64];
    if (ns < 1000.0) {
        std::snprintf(buffer, sizeof(buffer), "%.0f ns", ns);
    } else if (ns < 1'000'000.0) {
        std::snprintf(buffer, sizeof(buffer), "%.2f µs", ns / 1000.0);
    } else if (ns < 1'000'000'000.0) {
        std::snprintf(buffer, sizeof(buffer), "%.2f ms", ns / 1'000'000.0);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%.3f s", ns / 1'000'000'000.0);
    }
    return std::string(buffer);
}

/// @brief 带符号的 KiB 差值格式（RSS 可能为负：分配器归还）。
inline std::string HumanKiBDelta(std::int64_t kib) {
    char buffer[64];
    const char* sign = kib >= 0 ? "+" : "-";
    const std::uint64_t magnitude =
        static_cast<std::uint64_t>(kib >= 0 ? kib : -kib);
    std::snprintf(buffer, sizeof(buffer), "%s%s", sign,
                  HumanBytes(magnitude * 1024ULL).c_str());
    return std::string(buffer);
}

}  // namespace startup_cost
