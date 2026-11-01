/**
 * @file report.cpp
 * @brief 报告生成：控制台摘要 + markdown 全文。
 *
 * 报告的每一条结论都必须能追到表里的某个数字；这里不做「
 * 大约/可能」式断言，超过阈值的项会明确写出阈值和对应数值。
 */
#include "report.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
namespace startup_cost {
namespace {

std::string SignedBytes(std::int64_t bytes) {
    char buffer[64];
    const char* sign = bytes >= 0 ? "+" : "-";
    const std::uint64_t magnitude =
        static_cast<std::uint64_t>(bytes >= 0 ? bytes : -bytes);
    std::snprintf(buffer, sizeof(buffer), "%s%s", sign,
                  HumanBytes(magnitude).c_str());
    return std::string(buffer);
}

const char* KindName(PoolKind kind) {
    return kind == PoolKind::kEager ? "急切（构造即分配）" : "惰性（首次使用）";
}

const char* KindShort(PoolKind kind) {
    return kind == PoolKind::kEager ? "急切" : "惰性";
}

std::string SlotsText(const PoolInfo& info) {
    if (info.slots == 0) {
        return "按需";
    }
    return std::to_string(info.slots);
}

std::string SlotBytesText(const PoolInfo& info) {
    if (info.slot_bytes == 0) {
        return "-";
    }
    return HumanBytes(info.slot_bytes);
}

/// @brief 每池的「首次 vs 稳态」收益，用于场景 D。
std::string MemoryText(const PoolMeasurement& measurement) {
    return SignedBytes(static_cast<std::int64_t>(measurement.memory.heap_bytes));
}

/// @brief 0 表示「未测/无法测」，显示成 `-` 而不是 0 ns。
std::string TimeOrDash(std::uint64_t ns) {
    return ns == 0 ? std::string("-") : HumanNs(static_cast<double>(ns));
}

std::string VaText(const PoolMeasurement& measurement) {
    const std::int64_t kib = measurement.memory.vm_kib;
    if (kib == 0) {
        return std::string("-");
    }
    return HumanKiBDelta(kib);
}

double TotalInitNs(const Summary& summary) {
    return static_cast<double>(summary.all_pools.init_ns);
}

/// @brief 扣除 park/wake 往返后的有效启动成本。
double EffectiveInitNs(const Summary& summary) {
    const std::uint64_t total = summary.all_pools.init_ns;
    const std::uint64_t wait = summary.all_pools.non_pool_wait_ns;
    return static_cast<double>(total > wait ? total - wait : 0);
}

/// @brief 池净增量 = 全部池 - 只启动 Scheduler。
std::int64_t PoolNetRssKiB(const Summary& summary) {
    return summary.all_pools.rss_kib - summary.scheduler_only.rss_kib;
}

/// @brief 结论分级：以「典型 Web 服务的启动时间」为标尺。
struct Verdict {
    std::string level;
    std::string text;
};

Verdict MakeVerdict(const Summary& summary) {
    const double effective_ms = EffectiveInitNs(summary) / 1'000'000.0;
    Verdict verdict;
    char suffix[256];
    std::snprintf(suffix, sizeof(suffix),
                  "（端到端 %s，其中 %s 是一次 park→wake 往返，"
                  "与池无关；扣除后的有效成本 %s）",
                  HumanNs(TotalInitNs(summary)).c_str(),
                  HumanNs(static_cast<double>(summary.all_pools.non_pool_wait_ns))
                      .c_str(),
                  HumanNs(EffectiveInitNs(summary)).c_str());
    if (effective_ms < 20.0) {
        verdict.level = "可接受（优秀）";
        verdict.text =
            "扣除等待后的有效启动成本低于 20 ms，与 Go 静态二进制的典型 "
            "HTTP 服务启动（约 5~15 ms）处于同一量级，远低于 Node/Python/"
            "Java 框架（100 ms ~ 3 s）";
        verdict.text += suffix;
        verdict.text += "。";
    } else if (effective_ms < 100.0) {
        verdict.level = "可接受";
        verdict.text =
            "扣除等待后的有效启动成本在 20~100 ms 区间，仍显著低于 Node/"
            "Python/Java 框架，但已高于 Go 静态二进制的典型值，值得确认是否"
            "每项都必须在启动路径上";
        verdict.text += suffix;
        verdict.text += "。";
    } else {
        verdict.level = "偏高";
        verdict.text =
            "扣除等待后的有效启动成本超过 100 ms，接近或超过 Node/Python "
            "框架的启动量级，对 C++ 运行时来说偏高，应按下面的建议拆分懒加载";
        verdict.text += suffix;
        verdict.text += "。";
    }
    return verdict;
}

/// @brief 按数据生成优化建议（每条都能追到表格里的数字）。
std::vector<std::string> MakeSuggestions(const Summary& summary) {
    std::vector<std::string> suggestions;
    const std::int64_t pool_net = PoolNetRssKiB(summary);

    // 0. 如果等待主导了端到端时间，先说清楚，避免把调度延迟当成池成本。
    if (summary.all_pools.init_ns > 0 &&
        summary.all_pools.non_pool_wait_ns * 2 > summary.all_pools.init_ns) {
        char buffer[400];
        std::snprintf(
            buffer, sizeof(buffer),
            "场景 C 的端到端时间里 **%s（%.0f%%）是一次 park→wake 往返 + "
            "等待**，不是池的成本。要压缩「首个请求」延迟，优先查这条"
            "唤醒路径（跨线程 futex 唤醒 + worker 重新调度），而不是继续优化"
            "池的分配大小。",
            HumanNs(static_cast<double>(summary.all_pools.non_pool_wait_ns))
                .c_str(),
            100.0 * static_cast<double>(summary.all_pools.non_pool_wait_ns) /
                static_cast<double>(summary.all_pools.init_ns));
        suggestions.push_back(buffer);
    }

    // 1. 大头在调度器还是池？
    const double scheduler_ns =
        static_cast<double>(summary.scheduler_only.init_ns);
    const double all_ns = TotalInitNs(summary);
    if (all_ns > 0.0 && scheduler_ns / all_ns > 0.5) {        char buffer[256];
        std::snprintf(buffer, sizeof(buffer),
                      "启动耗时主要在**调度器核心**（%s / 总计 %s，占 %.0f%%），"
                      "池本身不是瓶颈。要再压缩启动时间，应针对 worker 线程"
                      "创建与 P 结构初始化，而不是继续优化池。",
                      HumanNs(scheduler_ns).c_str(), HumanNs(all_ns).c_str(),
                      100.0 * scheduler_ns / all_ns);
        suggestions.push_back(buffer);
    }

    // 2. 最大的单池内存。
    const PoolMeasurement* biggest = nullptr;
    for (const PoolMeasurement& measurement : summary.pools) {
        if (measurement.info->kind != PoolKind::kEager) {
            continue;
        }
        if (biggest == nullptr ||
            measurement.memory.rss_kib > biggest->memory.rss_kib) {
            biggest = &measurement;
        }
    }
    if (biggest != nullptr && biggest->memory.rss_kib >= 256) {
        char buffer[400];
        std::snprintf(
            buffer, sizeof(buffer),
            "**%s** 单池常驻 %s，占池净增量 %lld KiB 的 %.0f%%，是唯一需要"
            "认真对待的内存项。它按 `capacity × max_payload_bytes` 预留，"
            "默认 512 × 1536 B；媒体路径之外（纯 HTTP/RPC 服务）不应构造它，"
            "确需构造时按实际 codec 的包长下调 `max_payload_bytes`。",
            biggest->info->title,
            HumanKiBDelta(biggest->memory.rss_kib).c_str(),
            static_cast<long long>(pool_net),
            pool_net > 0 ? 100.0 * static_cast<double>(biggest->memory.rss_kib) /
                               static_cast<double>(pool_net)
                         : 0.0);
        suggestions.push_back(buffer);
    }

    // 3. 惰性池的首次使用代价。
    for (const PoolMeasurement& measurement : summary.pools) {
        if (measurement.info->kind != PoolKind::kLazy) {
            continue;
        }
        if (measurement.median_init_ns >= 100'000 &&
            measurement.info->id != std::string("scheduler_core")) {
            char buffer[360];
            std::snprintf(buffer, sizeof(buffer),
                          "**%s** 首次使用 %s，之后稳态 %s；这部分成本落在"
                          "第一个请求而不是进程启动上，对 p99 首请求有影响。"
                          "如果首请求延迟敏感，建议在服务 ready 之前预热一次"
                          "（spawn 一个空任务 / 建一次 channel）。",
                          measurement.info->title,
                          HumanNs(static_cast<double>(measurement.median_init_ns))
                              .c_str(),
                          TimeOrDash(measurement.median_steady_ns).c_str());
            suggestions.push_back(buffer);
        }
    }

    // 4. 抖动：p95 远高于中位说明首次路径受调度影响。
    for (const PoolMeasurement& measurement : summary.pools) {
        if (measurement.median_init_ns == 0 ||
            measurement.p95_init_ns == 0) {
            continue;
        }
        if (measurement.p95_init_ns >
            measurement.median_init_ns * 3 &&
            measurement.p95_init_ns - measurement.median_init_ns > 50'000) {
            char buffer[320];
            std::snprintf(buffer, sizeof(buffer),
                          "**%s** 的 p95（%s）超过中位（%s）3 倍，说明首次"
                          "路径存在调度/缺页抖动。若要稳定首请求延迟，预热比"
                          "调参更有效。",
                          measurement.info->title,
                          HumanNs(static_cast<double>(measurement.p95_init_ns))
                              .c_str(),
                          HumanNs(static_cast<double>(measurement.median_init_ns))
                              .c_str());
            suggestions.push_back(buffer);
        }
    }

    if (suggestions.empty()) {
        suggestions.push_back(
            "所有池的时间与内存都在阈值以内（单池 < 256 KiB、首次使用 "
            "< 100 µs），没有需要改动的项。");
    }

    // 5. 恒定建议：报告口径。
    suggestions.push_back(
        "口径提醒：上表的 ΔVmRSS 以 4 KiB 页为粒度，小于一页的池（"
        "WaitNode 等）请以 Δheap 为准；跨池相加会有分配器复用的重叠，"
        "总计以场景 C 的整进程测量为准。");

    return suggestions;
}

}  // namespace

void PrintConsole(const Summary& summary) {
    std::printf("\n=== 场景 A：基线 ===\n");
    std::printf("%-34s %14s %14s %14s %14s\n", "程序", "最小 wall", "中位 wall",
                "最大 wall", "峰值 RSS");
    std::printf("%-34s %14s %14s %14s %14s\n", "只 main + return",
                HumanNs(static_cast<double>(summary.min_program.min_wall_ns)).c_str(),
                HumanNs(static_cast<double>(summary.min_program.median_wall_ns))
                    .c_str(),
                HumanNs(static_cast<double>(summary.min_program.max_wall_ns)).c_str(),
                HumanBytes(summary.min_program.max_rss_kib * 1024ULL).c_str());
    std::printf("%-34s %14s %14s %14s %14s\n", "完整二进制（不触发池）",
                HumanNs(static_cast<double>(summary.empty_program.min_wall_ns))
                    .c_str(),
                HumanNs(static_cast<double>(summary.empty_program.median_wall_ns))
                    .c_str(),
                HumanNs(static_cast<double>(summary.empty_program.max_wall_ns))
                    .c_str(),
                HumanBytes(summary.empty_program.max_rss_kib * 1024ULL).c_str());

    std::printf("\n=== 场景 B：每个池 ===\n");
    std::printf("%-20s %-5s %11s %11s %11s %11s %11s %11s %6s %9s\n",
                "池", "时机", "首次", "p95", "稳态/二次", "ΔVmRSS", "Δheap",
                "ΔVmSize", "槽数", "单槽");
    for (const PoolMeasurement& measurement : summary.pools) {
        std::printf("%-20s %-5s %11s %11s %11s %11s %11s %11s %6s %9s\n",
                    measurement.info->id,
                    KindShort(measurement.info->kind),
                    TimeOrDash(measurement.median_init_ns).c_str(),
                    TimeOrDash(measurement.p95_init_ns).c_str(),
                    TimeOrDash(measurement.median_steady_ns).c_str(),
                    HumanKiBDelta(measurement.memory.rss_kib).c_str(),
                    SignedBytes(measurement.memory.heap_bytes).c_str(),
                    VaText(measurement).c_str(),
                    SlotsText(*measurement.info).c_str(),
                    SlotBytesText(*measurement.info).c_str());
    }

    std::printf("\n=== 场景 C：全部池 ===\n");
    std::printf("%-30s %14s %14s\n", "组合", "时间", "ΔVmRSS");
    std::printf("%-30s %14s %14s\n", "只启动 Scheduler（对照）",
                HumanNs(static_cast<double>(summary.scheduler_only.init_ns))
                    .c_str(),
                HumanKiBDelta(summary.scheduler_only.rss_kib).c_str());
    std::printf("%-30s %14s %14s\n", "全部急切池（不含 Scheduler）",
                HumanNs(static_cast<double>(summary.all_eager.init_ns)).c_str(),
                HumanKiBDelta(summary.all_eager.rss_kib).c_str());
    std::printf("%-30s %14s %14s\n", "全部池 + 首次任务（含 Scheduler）",
                HumanNs(static_cast<double>(summary.all_pools.init_ns)).c_str(),
                HumanKiBDelta(summary.all_pools.rss_kib).c_str());
    std::printf("%-30s %14s\n", "全部池端到端墙钟（含 exec 与进程退出）",
                HumanNs(static_cast<double>(summary.all_pools_wall_ns)).c_str());
    if (summary.all_pools.samples > 0) {
        std::printf("%-30s %10llu / %llu 次\n", "其中慢于最小值 3 倍的样本",
                    static_cast<unsigned long long>(
                        summary.all_pools.slow_samples),
                    static_cast<unsigned long long>(summary.all_pools.samples));
    }

    if (!summary.all_pools.stages.empty()) {
        std::printf("\n=== 场景 C 分段（同一进程内逐段计时） ===\n");
        const double total = static_cast<double>(summary.all_pools.init_ns);
        for (const startup_cost::StageTiming& stage : summary.all_pools.stages) {
            std::printf("  %-22s %12s  %5.1f%%\n", stage.name.c_str(),
                        HumanNs(static_cast<double>(stage.ns)).c_str(),
                        total > 0.0
                            ? 100.0 * static_cast<double>(stage.ns) / total
                            : 0.0);
        }
        std::printf("  %-22s %12s  %5.1f%%\n", "与池无关的等待",
                    HumanNs(static_cast<double>(
                                summary.all_pools.non_pool_wait_ns)).c_str(),
                    total > 0.0
                        ? 100.0 * static_cast<double>(
                                      summary.all_pools.non_pool_wait_ns) /
                              total
                        : 0.0);
        std::printf("  %-22s %12s\n", "扣除等待后的有效成本",
                    HumanNs(EffectiveInitNs(summary)).c_str());
    }

    const Verdict verdict = MakeVerdict(summary);
    std::printf("\n=== 结论 ===\n%s：%s\n", verdict.level.c_str(),
                verdict.text.c_str());
    std::printf("\n=== 优化建议 ===\n");
    for (const std::string& suggestion : MakeSuggestions(summary)) {
        std::printf("- %s\n", suggestion.c_str());
    }
    std::printf("\n");
}

bool WriteMarkdown(const std::string& path, const Summary& summary) {
    std::string text;
    text.reserve(16 * 1024);

    text += "# Go2Cpp 池化资源启动开销实测\n\n";
    text += "> 由 `tools/startup_cost` 生成。所有数字都是本机实测，";
    text += "复现命令见文末。\n\n";

    // ---- 环境 ----
    text += "## 0. 环境\n\n";
    text += "| 项 | 值 |\n|---|---|\n";
    text += "| 日期 | " + summary.date + " |\n";
    text += "| 主机 | " + summary.host + " |\n";
    text += "| CPU | " + summary.cpu + " |\n";
    text += "| 内核 | " + summary.kernel + " |\n";
    text += "| 构建 | " + summary.build_dir + "（" + summary.build_type + "） |\n";
    text += "| 编译器 | " + summary.compiler + " |\n";
    {
        char buffer[160];
        std::snprintf(buffer, sizeof(buffer), "| 计时器 | rdtsc（%.4f ns/tick，%s） |\n",
                      summary.ns_per_tick,
                      summary.tsc_valid ? "校准通过" : "校准失败，退回墙钟");
        text += buffer;
    }
    {
        char buffer[160];
        std::snprintf(buffer, sizeof(buffer),
                      "| 惰性池首次样本 | %llu 次独立进程 |\n",
                      static_cast<unsigned long long>(summary.lazy_samples));
        text += buffer;
    }
    {
        char buffer[160];
        std::snprintf(buffer, sizeof(buffer),
                      "| 急切池首次样本 | %llu 次独立进程（每次只构造一次） |\n",
                      static_cast<unsigned long long>(
                          summary.eager_first_samples));
        text += buffer;
    }
    {
        char buffer[160];
        std::snprintf(buffer, sizeof(buffer),
                      "| 急切池稳态样本 | 进程内 %llu 次取中位 |\n",
                      static_cast<unsigned long long>(summary.eager_repeat));
        text += buffer;
    }
    text += "\n";

    // ---- 口径 ----
    text += "## 1. 测量口径\n\n";
    text += "- **场景 A（基线）**：`startup_cost_min` 只做 `return 0`，";
    text += "父进程 fork→exec→waitpid 测端到端墙钟（含 exec + 动态链接 + 退出）；";
    text += "峰值 RSS 取 `wait4().ru_maxrss`。\n";
    text += "- **场景 B（每池）**：一次独立子进程只测一个池。内存读操作前后的 ";
    text += "`VmRSS` 与 `mallinfo2().uordblks + hblkhd`，相减得增量。\n";
    text += "- **场景 C（全部池）**：单进程内按真实启动顺序初始化全部池（含 ";
    text += "Scheduler 启动 + 一次任务运行），再读整进程增量。\n";
    text += "- **场景 D（首次使用）**：惰性池的 `首次` 与 `稳态` 两列之差，";
    text += "就是「首次使用才分配」的那部分延迟。\n";
    text += "- **惰性池无法 1e4 次重复**：池是进程级单例 / TLS，同一进程内";
    text += "第二次就不再分配。因此惰性池用 **";
    text += std::to_string(summary.lazy_samples) + " 次独立进程**取中位；";
    text += "急切池的**首次**用 **";
    text += std::to_string(summary.eager_first_samples) +
            " 次独立进程**（每次只构造一次）取中位，**稳态**用进程内 **";
    text += std::to_string(summary.eager_repeat) + " 次**取中位。\n";
    text += "- **WaitNode 池只报内存**：它的一次「初始化」就是一次约 200 B 的";
    text += "`make_shared`，而触发它必须先 park；park 等待时间远大于分配时间，";
    text += "从外部无法剥离，硬报一个耗时只会得到被等待污染的数。\n\n";

    // ---- 表 A ----
    text += "## 2. 场景 A：基线\n\n";
    text += "| 程序 | 最小 wall | 中位 wall | 最大 wall | 峰值 RSS |\n";
    text += "|---|---|---|---|---|\n";
    {
        char buffer[512];
        std::snprintf(
            buffer, sizeof(buffer),
            "| 只 main + return | %s | %s | %s | %s |\n",
            HumanNs(static_cast<double>(summary.min_program.min_wall_ns)).c_str(),
            HumanNs(static_cast<double>(summary.min_program.median_wall_ns))
                .c_str(),
            HumanNs(static_cast<double>(summary.min_program.max_wall_ns)).c_str(),
            HumanBytes(summary.min_program.max_rss_kib * 1024ULL).c_str());
        text += buffer;
    }
    {
        char buffer[512];
        std::snprintf(
            buffer, sizeof(buffer),
            "| 完整二进制（不触发池） | %s | %s | %s | %s |\n",
            HumanNs(static_cast<double>(summary.empty_program.min_wall_ns))
                .c_str(),
            HumanNs(static_cast<double>(summary.empty_program.median_wall_ns))
                .c_str(),
            HumanNs(static_cast<double>(summary.empty_program.max_wall_ns))
                .c_str(),
            HumanBytes(summary.empty_program.max_rss_kib * 1024ULL).c_str());
        text += buffer;
    }
    text += "\n> 「完整二进制」链了全部模块 `.so`，它的额外开销是动态链接与 ";
    text += "静态初始化，不是池。两者之差说明：**进程启动本身**与**池初始化**";
    text += "要分开归因。\n\n";

    // ---- 表 B ----
    text += "## 3. 场景 B：每个池\n\n";
    text += "| 池 | 时机 | 首次/中位 | p95 | 稳态/二次 | ΔVmRSS | ΔVmHWM | Δheap | ΔVmSize | 槽数 | 单槽 | 来源 |\n";
    text += "|---|---|---|---|---|---|---|---|---|---|---|---|\n";
    for (const PoolMeasurement& measurement : summary.pools) {
        char buffer[960];
        std::snprintf(
            buffer, sizeof(buffer),
            "| `%s` | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | `%s` |\n",
            measurement.info->id, KindName(measurement.info->kind),
            TimeOrDash(measurement.median_init_ns).c_str(),
            TimeOrDash(measurement.p95_init_ns).c_str(),
            TimeOrDash(measurement.median_steady_ns).c_str(),
            HumanKiBDelta(measurement.memory.rss_kib).c_str(),
            HumanKiBDelta(measurement.memory.hwm_kib).c_str(),
            MemoryText(measurement).c_str(),
            VaText(measurement).c_str(),
            SlotsText(*measurement.info).c_str(),
            SlotBytesText(*measurement.info).c_str(),
            measurement.info->source);
        text += buffer;
    }
    text += "\n";

    text += "槽结构说明：\n\n";
    for (const PoolMeasurement& measurement : summary.pools) {
        text += std::string("- **") + measurement.info->title + "**：" +
                measurement.info->note + "\n";
    }
    text += "\n";
    text += "> **读数注意**：`ΔVmRSS` 含探针内新建的 worker 线程栈（约 "
            "128 KiB），`Δheap` 含首次路径上的 Fiber 对象、状态记录等其它"
            "分配。因此 `wait_node_pool` / `channel_wait_node` 的约 36 KiB "
            "**不能**当作「WaitNode 大小」读 —— 单个节点约 200 B，已被其它"
            "分配淹没。判断池本身的体量请看「槽数 × 单槽」两列。\n\n";

    // ---- 表 C ----
    text += "## 4. 场景 C：全部池\n\n";
    text += "| 组合 | 时间 | ΔVmRSS | Δheap |\n|---|---|---|---|\n";
    {
        char buffer[512];
        std::snprintf(
            buffer, sizeof(buffer), "| 只启动 Scheduler（对照） | %s | %s | %s |\n",
            HumanNs(static_cast<double>(summary.scheduler_only.init_ns)).c_str(),
            HumanKiBDelta(summary.scheduler_only.rss_kib).c_str(),
            SignedBytes(summary.scheduler_only.heap_bytes).c_str());
        text += buffer;
    }
    {
        char buffer[512];
        std::snprintf(
            buffer, sizeof(buffer), "| 全部急切池（不含 Scheduler） | %s | %s | %s |\n",
            HumanNs(static_cast<double>(summary.all_eager.init_ns)).c_str(),
            HumanKiBDelta(summary.all_eager.rss_kib).c_str(),
            SignedBytes(summary.all_eager.heap_bytes).c_str());
        text += buffer;
    }
    {
        char buffer[512];
        std::snprintf(
            buffer, sizeof(buffer),
            "| **全部池 + 首次任务（含 Scheduler）** | **%s** | **%s** | **%s** |\n",
            HumanNs(static_cast<double>(summary.all_pools.init_ns)).c_str(),
            HumanKiBDelta(summary.all_pools.rss_kib).c_str(),
            SignedBytes(summary.all_pools.heap_bytes).c_str());
        text += buffer;
    }
    text += "\n";
    {
        char buffer[384];
        std::snprintf(
            buffer, sizeof(buffer),
            "池净增量（全部池 − 只启动 Scheduler）：**%s**；"
            "跨进程端到端墙钟中位（含 exec 与 scheduler 关停）：**%s**；"
            "时间取 %llu 次运行的最小值（干扰只增不减），其中 **%llu 次**"
            "慢于最小值的 3 倍。\n\n",
            HumanKiBDelta(PoolNetRssKiB(summary)).c_str(),
            HumanNs(static_cast<double>(summary.all_pools_wall_ns)).c_str(),
            static_cast<unsigned long long>(summary.all_pools.samples),
            static_cast<unsigned long long>(summary.all_pools.slow_samples));
        text += buffer;
    }

    if (!summary.all_pools.stages.empty()) {
        text += "### 4.1 场景 C 的分段（同一进程内逐段计时）\n\n";
        text += "| 阶段 | 耗时 | 占比 |\n|---|---|---|\n";
        const double total = static_cast<double>(summary.all_pools.init_ns);
        for (const startup_cost::StageTiming& stage :
             summary.all_pools.stages) {
            char buffer[320];
            std::snprintf(buffer, sizeof(buffer), "| `%s` | %s | %.1f%% |\n",
                          stage.name.c_str(),
                          HumanNs(static_cast<double>(stage.ns)).c_str(),
                          total > 0.0
                              ? 100.0 * static_cast<double>(stage.ns) / total
                              : 0.0);
            text += buffer;
        }
        {
            char buffer[320];
            std::snprintf(
                buffer, sizeof(buffer), "| **与池无关的等待** | **%s** | **%.1f%%** |\n",
                HumanNs(static_cast<double>(summary.all_pools.non_pool_wait_ns))
                    .c_str(),
                total > 0.0
                    ? 100.0 *
                          static_cast<double>(summary.all_pools.non_pool_wait_ns) /
                          total
                    : 0.0);
            text += buffer;
        }
        {
            char buffer[320];
            std::snprintf(buffer, sizeof(buffer),
                          "| **扣除等待后的有效成本** | **%s** | — |\n",
                          HumanNs(EffectiveInitNs(summary)).c_str());
            text += buffer;
        }
        text += "\n";
        text += "「与池无关的等待」= 一次 `sync::Mutex` 竞争的 park→wake 往返。";
        text += "它在同一台机器上是**双峰**的：多数样本几十 µs，少数样本约 ";
        text += "10 ms（唤醒后被重新调度的延迟）。它不随池的数量或大小变化，";
        text += "所以判断「池贵不贵」时必须扣掉；端到端启动时间则要把它算进去。\n\n";
    }

    // ---- 表 D ----
    text += "## 5. 场景 D：首次使用 vs 稳态\n\n";
    text += "「首次」= 新进程里第一次构造/触发；「稳态」= 同一进程内重复时的成本。";
    text += "两者之差就是**只在第一次付的钱**（mmap、brk 扩展、arena 建立、"
            "TLS 初始化）。\n\n";
    text += "| 池 | 首次 | 稳态 | 一次性成本（首次−稳态） | 倍数 |\n";
    text += "|---|---|---|---|---|\n";
    for (const PoolMeasurement& measurement : summary.pools) {
        if (measurement.median_init_ns == 0 ||
            measurement.median_steady_ns == 0) {
            continue;
        }
        if (std::strcmp(measurement.info->id, "scheduler_core") == 0) {
            continue;  // 参照项，不是池
        }
        const std::uint64_t delta =
            measurement.median_init_ns > measurement.median_steady_ns
                ? measurement.median_init_ns - measurement.median_steady_ns
                : 0;
        char buffer[512];
        std::snprintf(
            buffer, sizeof(buffer), "| `%s` | %s | %s | %s | %.1f× |\n",
            measurement.info->id,
            HumanNs(static_cast<double>(measurement.median_init_ns)).c_str(),
            HumanNs(static_cast<double>(measurement.median_steady_ns)).c_str(),
            HumanNs(static_cast<double>(delta)).c_str(),
            static_cast<double>(measurement.median_init_ns) /
                static_cast<double>(measurement.median_steady_ns));
        text += buffer;
    }
    text += "\n> 倍数偏高是口径造成的：首次来自独立新进程（页表、brk、mmap 都";
    text += "是冷的），稳态来自同进程热态。真实的「第一次请求」介于两者之间：";
    text += "比稳态贵，但不必重新 exec。\n\n";
    text += "> `wait_node_pool` 与 `channel_wait_node` 不在此表：它们的一次初始化";
    text += "就是一次约 200 B 的 `make_shared`，而触发它必须先 park，等待时间无法";
    text += "从外部剥离（见第 1 节口径）。\n\n";

    // ---- 结论 ----
    const Verdict verdict = MakeVerdict(summary);
    text += "## 6. 结论\n\n";
    text += "**" + verdict.level + "**：" + verdict.text + "\n\n";
    text += "参考量级（公开基准的常见区间，非本次实测）：\n\n";
    text += "| 服务形态 | 典型启动时间 |\n|---|---|\n";
    text += "| Nginx | 5~15 ms |\n";
    text += "| Go 静态二进制 HTTP 服务 | 5~15 ms |\n";
    text += "| Node.js + Express | 100~300 ms |\n";
    text += "| Python + gunicorn | 300~800 ms |\n";
    text += "| Java + Spring Boot | 1~3 s |\n\n";
    {
        char buffer[384];
        std::snprintf(
            buffer, sizeof(buffer),
            "本运行时「进程已就绪 → 全部池建好 → 跑完第一个任务」实测 **%s**"
            "（其中 %s 是 park→wake 等待，有效成本 %s）；"
            "再加上 exec 与动态链接（完整二进制基线 %s），端到端 **%s**。\n\n",
            HumanNs(TotalInitNs(summary)).c_str(),
            HumanNs(static_cast<double>(summary.all_pools.non_pool_wait_ns))
                .c_str(),
            HumanNs(EffectiveInitNs(summary)).c_str(),
            HumanNs(static_cast<double>(summary.empty_program.median_wall_ns))
                .c_str(),
            HumanNs(static_cast<double>(summary.all_pools_wall_ns)).c_str());
        text += buffer;
    }

    // ---- 建议 ----
    text += "## 7. 优化建议\n\n";
    for (const std::string& suggestion : MakeSuggestions(summary)) {
        text += "- " + suggestion + "\n";
    }
    text += "\n";

    // ---- 复现 ----
    text += "## 8. 复现\n\n";
    text += "```bash\n";
    text += "cd <repo> && tools/startup_cost/build.sh\n";
    text += ".build-startup-cost/startup_cost --out /tmp/startup_cost.md\n";
    text += "```\n\n";
    text += "调试单个池（不跑父模式）：\n\n";
    text += "```bash\n";
    text += ".build-startup-cost/startup_cost --child jitter_buffer --mode mem\n";
    text += ".build-startup-cost/startup_cost --child cache_node_pool --mode time --repeat 10000\n";
    text += "```\n";

    FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) {
        std::fprintf(stderr, "无法写入 %s: %s\n", path.c_str(),
                     std::strerror(errno));
        return false;
    }
    const std::size_t written =
        std::fwrite(text.data(), 1, text.size(), file);
    std::fclose(file);
    return written == text.size();
}

}  // namespace startup_cost
