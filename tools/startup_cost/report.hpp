/**
 * @file report.hpp
 * @brief 采集结果的聚合结构与报告生成接口。
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "measure.hpp"
#include "pools.hpp"

namespace startup_cost {

/// @brief 单个池的完整测量（时间 + 内存）。
struct PoolMeasurement {
    const PoolInfo* info = nullptr;
    ProbeResult memory;                  ///< 内存模式（单次）
    ProbeResult timing;                  ///< 时间模式（内部或单次）
    std::uint64_t median_init_ns = 0;    ///< 首次/中位耗时
    std::uint64_t median_steady_ns = 0;  ///< 稳态/二次耗时（0 = 无）
    std::uint64_t p95_init_ns = 0;
    std::uint64_t min_init_ns = 0;
    std::uint64_t samples = 0;           ///< 时间样本数
    bool ok = false;
};

/// @brief 一个可执行文件的基线（启动墙钟 + 峰值 RSS）。
struct Baseline {
    bool ok = false;
    std::uint64_t min_wall_ns = 0;
    std::uint64_t median_wall_ns = 0;
    std::uint64_t max_wall_ns = 0;
    std::uint64_t max_rss_kib = 0;  ///< 来自 wait4().ru_maxrss
    std::uint64_t samples = 0;
};

/// @brief 一次完整运行的汇总。
struct Summary {
    std::string date;
    std::string host;
    std::string kernel;
    std::string build_dir;
    std::string build_type;
    std::string compiler;
    std::string cpu;
    std::uint64_t tsc_ns_per_ns = 0;  ///< 保留位（未用）
    bool tsc_valid = false;
    double ns_per_tick = 0.0;

    Baseline min_program;   ///< 场景 A：只 main + return
    Baseline empty_program; ///< 场景 A2：完整二进制，不触发任何池
    std::vector<PoolMeasurement> pools;

    ProbeResult all_eager;      ///< 场景 C：仅急切池
    ProbeResult all_pools;      ///< 场景 C：Scheduler + 全部池
    ProbeResult scheduler_only; ///< 场景 C 对照：只启动 Scheduler
    std::uint64_t all_pools_wall_ns = 0;

    std::uint64_t lazy_samples = 0;
    std::uint64_t eager_repeat = 0;
    std::uint64_t eager_first_samples = 0;
};

/// @brief 打印控制台表格。
void PrintConsole(const Summary& summary);

/// @brief 写 markdown 报告；返回是否成功。
bool WriteMarkdown(const std::string& path, const Summary& summary);

}  // namespace startup_cost
