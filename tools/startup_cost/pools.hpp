/**
 * @file pools.hpp
 * @brief 池化资源清单与探针接口。
 *
 * 每个探针回答一个能被独立复核的问题：**「从进程已就绪到该池可用，多花了
 * 多少时间与多少内存」**。为此：
 *
 * - 探针只在**子进程**里跑，一次进程只测一个池 —— 池是进程级单例或
 *   TLS，跨池测会互相污染 RSS；
 * - 内存读两次（探针前后），相减得到增量；同时记 `VmHWM` 供交叉验证；
 * - 惰性池（首次使用才分配）与急切池（构造即分配）分开统计，因为它们的
 *   「启动开销」发生在不同时刻：前者在第一次 spawn，后者在进程初始化。
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "measure.hpp"

namespace startup_cost {

/// @brief 池的分配时机。
enum class PoolKind {
    kEager,  ///< 构造即分配（进程启动期付费）
    kLazy,   ///< 首次使用才分配（第一次 spawn/等待/插入时付费）
};

/// @brief 一个池的静态元数据（槽数 / 单槽大小 / 来源）。
struct PoolInfo {
    const char* id = "";           ///< 命令行 id
    const char* title = "";        ///< 中文名
    PoolKind kind = PoolKind::kEager;
    const char* source = "";       ///< 实现位置（文件:行）
    std::uint64_t slots = 0;       ///< 槽位数；0 表示不定长/按需
    std::uint64_t slot_bytes = 0;  ///< 单槽字节；0 表示无固定槽
    std::uint64_t fixed_bytes = 0; ///< 常量预分配字节（构造即分配）
    const char* note = "";         ///< 槽结构说明
    bool repeatable = false;       ///< 是否可在同一进程内重复构造取中位
};

/// @brief 全部被测池（顺序即报告顺序）。
const std::vector<PoolInfo>& PoolCatalog();

/// @brief 按 id 查池元数据；不存在返回 nullptr。
const PoolInfo* FindPool(const std::string& id);

/// @brief 场景 C 的分段耗时（用于把 park/wake 这类等待从池成本里剥出来）。
struct StageTiming {
    std::string name;
    std::uint64_t ns = 0;
};

/// @brief 一次探针的结果。
struct ProbeResult {
    bool ok = false;
    std::string error;

    /// 池自身初始化/首次分配耗时（中位）；惰性池为 0（无法剥离）。
    std::uint64_t init_ns = 0;
    /// 端到端首次可用耗时（中位）：含调度器入队/唤醒/完成。
    std::uint64_t use_ns = 0;
    /// 同一进程内第二次使用的耗时（中位）；用于估算一次性初始化成本。
    std::uint64_t steady_ns = 0;
    std::uint64_t min_ns = 0;
    std::uint64_t p95_ns = 0;
    std::uint64_t max_ns = 0;
    std::uint64_t samples = 0;
    /// @brief 明显慢于最干净样本的次数（> 3× min）：用于发现间歇性的长尾。
    std::uint64_t slow_samples = 0;

    /// 一次运行后的增量（可为负：分配器归还 / 线程退出）。
    std::int64_t rss_kib = 0;
    std::int64_t vm_kib = 0;  ///< 虚拟地址空间增量（栈 arena 预留看得见）
    std::int64_t hwm_kib = 0;
    std::int64_t heap_bytes = 0;
    std::int64_t mmap_bytes = 0;

    /// 场景 C 的分段耗时（其它探针为空）。
    std::vector<StageTiming> stages;
    /// 场景 C 中「与池无关的等待」之和（park/wake 往返、观察窗口）。
    std::uint64_t non_pool_wait_ns = 0;
};

/// @brief 探针执行模式。
enum class ProbeMode {
    kMemory,  ///< 跑 1 次，只关心内存增量
    kTime,    ///< 跑 repeat 次，只关心中位时间
};

/// @brief 安装计时器（TSC 校准结果）。
void SetClock(const TscClock& clock);

/// @brief 当前计时器。
const TscClock& Clock();

/// @brief 运行一个探针。
/// @param id 池 id（见 PoolCatalog）
/// @param mode 内存模式或时间模式
/// @param repeat 时间模式的重复次数（内存模式忽略）
ProbeResult RunProbe(const std::string& id, ProbeMode mode, std::size_t repeat);

/// @brief 全部池一起初始化（场景 C）。
/// @param include_scheduler true 表示包含 Scheduler 启动与首次 spawn。
ProbeResult RunAllPools(bool include_scheduler);

/// @brief 只启动 Scheduler、不触发任何池（场景 C 的对照组）。
ProbeResult RunSchedulerOnly();

}  // namespace startup_cost
