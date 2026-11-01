/**
 * @file main.cpp
 * @brief 启动开销基准的编排器。
 *
 * ## 两种运行模式
 *
 * - **父模式**（默认）：fork/exec 自身若干次，每次只让子进程测一个池，
 *   汇总成表；同时负责写 markdown 报告。
 * - **子模式**（`--child <id>`）：只跑一个探针，把结果以 `key=value` 行
 *   打到 stdout，供父模式解析。
 *
 * ## 为什么用独立进程测
 *
 * 池要么是进程级单例（`TaskBlockPool::Instance()`），要么是线程本地
 * （`FiberBin`、`WaitNode` 缓存）：同进程内第二次就不会再付初始化费用。
 * 想测「首次」，只能一次一个进程。进程自身开销（exec + 动态链接）由
 * 场景 A 的裸程序单独测出，报告中与池增量分开归因。
 */
#include "measure.hpp"
#include "pools.hpp"
#include "report.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/resource.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

using startup_cost::Baseline;
using startup_cost::Capture;
using startup_cost::HumanBytes;
using startup_cost::HumanNs;
using startup_cost::Median;
using startup_cost::MemSnapshot;
using startup_cost::MonotonicNs;
using startup_cost::Percentile;
using startup_cost::PoolCatalog;
using startup_cost::PoolInfo;
using startup_cost::PoolKind;
using startup_cost::PoolMeasurement;
using startup_cost::ProbeMode;
using startup_cost::ProbeResult;
using startup_cost::Summary;
using startup_cost::TscClock;

// ---------------------------------------------------------------------------
// 命令行
// ---------------------------------------------------------------------------

struct Options {
    std::string child_id;            ///< 非空 = 子模式
    std::string mode = "time";       ///< 子模式：mem / time
    std::size_t repeat = 0;          ///< 子模式重复次数（0 = 按池默认）
    std::string out_path = "/tmp/startup_cost.md";
    bool list = false;
    bool quick = false;              ///< 减少样本（调试）
    bool no_report = false;
};

constexpr std::size_t kLazySamples = 51;      ///< 惰性池：跨进程样本数
constexpr std::size_t kEagerFirstSamples = 21; ///< 急切池首次：跨进程样本数
constexpr std::size_t kEagerRepeat = 10000;   ///< 急切池稳态：进程内循环次数
constexpr std::size_t kJitterRepeat = 2000;   ///< JitterBuffer 800 KiB/次，降频
constexpr std::size_t kFiberRepeat = 2000;    ///< Fiber 每次 mmap，降频
constexpr std::size_t kBaselineSamples = 31;  ///< 场景 A 样本数
constexpr std::size_t kAggregateSamples = 21; ///< 场景 C 样本数

/// @brief 每个池的进程内重复次数（按单次成本分档）。
std::size_t RepeatFor(const PoolInfo& info) {
    if (std::strcmp(info.id, "jitter_buffer") == 0) {
        return kJitterRepeat;
    }
    if (std::strcmp(info.id, "fiber_bin") == 0) {
        return kFiberRepeat;
    }
    return kEagerRepeat;
}

// ---------------------------------------------------------------------------
// 子进程执行
// ---------------------------------------------------------------------------

struct ChildRun {
    bool spawned = false;
    int exit_code = -1;
    std::uint64_t wall_ns = 0;
    std::uint64_t max_rss_kib = 0;  ///< wait4().ru_maxrss
    std::string output;
};

std::string SelfExe() {
    char buffer[4096];
    const ssize_t length =
        ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (length <= 0) {
        return std::string();
    }
    buffer[length] = '\0';
    return std::string(buffer);
}

std::string DirName(const std::string& path) {
    const std::size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) {
        return std::string(".");
    }
    return path.substr(0, slash);
}

/// @brief fork + exec 自身，收集 stdout、端到端墙钟与峰值 RSS。
ChildRun RunChild(const std::string& exe,
                  const std::vector<std::string>& arguments) {
    ChildRun run;
    int pipe_fds[2] = {-1, -1};
    if (::pipe(pipe_fds) != 0) {
        return run;
    }

    std::vector<std::string> owned;
    owned.reserve(arguments.size() + 1);
    owned.push_back(exe);
    owned.insert(owned.end(), arguments.begin(), arguments.end());
    std::vector<char*> argv;
    argv.reserve(owned.size() + 1);
    for (std::string& item : owned) {
        argv.push_back(const_cast<char*>(item.c_str()));
    }
    argv.push_back(nullptr);

    const std::uint64_t begin = MonotonicNs();
    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(pipe_fds[0]);
        ::close(pipe_fds[1]);
        return run;
    }
    if (pid == 0) {
        ::dup2(pipe_fds[1], STDOUT_FILENO);
        ::close(pipe_fds[0]);
        ::close(pipe_fds[1]);
        ::execv(exe.c_str(), argv.data());
        ::_exit(127);
    }

    run.spawned = true;
    ::close(pipe_fds[1]);
    char buffer[4096];
    for (;;) {
        const ssize_t got = ::read(pipe_fds[0], buffer, sizeof(buffer));
        if (got > 0) {
            run.output.append(buffer, static_cast<std::size_t>(got));
            continue;
        }
        if (got == 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        break;
    }
    ::close(pipe_fds[0]);

    int status = 0;
    struct rusage usage {};
    while (::wait4(pid, &status, 0, &usage) < 0 && errno == EINTR) {
    }
    run.wall_ns = MonotonicNs() - begin;
    run.max_rss_kib = static_cast<std::uint64_t>(usage.ru_maxrss);
    if (WIFEXITED(status)) {
        run.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        run.exit_code = 128 + WTERMSIG(status);
    }
    return run;
}

std::map<std::string, std::string> ParseKeyValues(const std::string& text) {
    std::map<std::string, std::string> values;
    std::size_t position = 0;
    while (position < text.size()) {
        std::size_t end = text.find_first_of(" \n", position);
        if (end == std::string::npos) {
            end = text.size();
        }
        const std::string token = text.substr(position, end - position);
        const std::size_t equal = token.find('=');
        if (equal != std::string::npos) {
            values[token.substr(0, equal)] = token.substr(equal + 1);
        }
        position = end + 1;
    }
    return values;
}

std::uint64_t LookupU64(const std::map<std::string, std::string>& values,
                        const char* key, std::uint64_t fallback = 0) {
    const auto it = values.find(key);
    if (it == values.end()) {
        return fallback;
    }
    return static_cast<std::uint64_t>(
        std::strtoull(it->second.c_str(), nullptr, 10));
}

std::int64_t LookupI64(const std::map<std::string, std::string>& values,
                       const char* key, std::int64_t fallback = 0) {
    const auto it = values.find(key);
    if (it == values.end()) {
        return fallback;
    }
    return static_cast<std::int64_t>(
        std::strtoll(it->second.c_str(), nullptr, 10));
}

ProbeResult ParseProbeResult(const ChildRun& run) {
    ProbeResult result;
    const std::map<std::string, std::string> values = ParseKeyValues(run.output);
    if (values.empty() || values.count("ok") == 0) {
        result.error = "子进程无有效输出（exit=" +
                       std::to_string(run.exit_code) + "）";
        return result;
    }
    result.ok = LookupU64(values, "ok") != 0;
    result.init_ns = LookupU64(values, "init_ns");
    result.use_ns = LookupU64(values, "use_ns");
    result.steady_ns = LookupU64(values, "steady_ns");
    result.min_ns = LookupU64(values, "min_ns");
    result.p95_ns = LookupU64(values, "p95_ns");
    result.max_ns = LookupU64(values, "max_ns");
    result.samples = LookupU64(values, "samples");
    result.rss_kib = LookupI64(values, "rss_kib");
    result.vm_kib = LookupI64(values, "vm_kib");
    result.hwm_kib = LookupI64(values, "hwm_kib");
    result.heap_bytes = LookupI64(values, "heap_bytes");
    result.mmap_bytes = LookupI64(values, "mmap_bytes");
    result.non_pool_wait_ns = LookupU64(values, "wait_ns");
    result.slow_samples = LookupU64(values, "slow");

    const auto stages = values.find("stages");
    if (stages != values.end() && !stages->second.empty()) {
        std::size_t position = 0;
        while (position < stages->second.size()) {
            std::size_t comma = stages->second.find(',', position);
            if (comma == std::string::npos) {
                comma = stages->second.size();
            }
            const std::string item =
                stages->second.substr(position, comma - position);
            const std::size_t colon = item.find(':');
            if (colon != std::string::npos) {
                startup_cost::StageTiming stage;
                stage.name = item.substr(0, colon);
                stage.ns = static_cast<std::uint64_t>(
                    std::strtoull(item.c_str() + colon + 1, nullptr, 10));
                result.stages.push_back(stage);
            }
            position = comma + 1;
        }
    }

    const auto error = values.find("error");
    if (error != values.end() && !error->second.empty()) {
        result.error = error->second;
    }
    return result;
}

// ---------------------------------------------------------------------------
// 子模式
// ---------------------------------------------------------------------------

int RunChildMode(const Options& options) {
    if (options.child_id == "__none__") {
        const MemSnapshot snapshot = Capture();
        std::printf(
            "probe=__none__ mode=mem ok=1 init_ns=0 use_ns=0 steady_ns=0 "
            "min_ns=0 p95_ns=0 max_ns=0 samples=0 rss_kib=%llu vm_kib=%llu "
            "hwm_kib=%llu heap_bytes=%llu mmap_bytes=%llu wait_ns=0 slow=0 "
            "stages=\n",
            static_cast<unsigned long long>(snapshot.rss_kib),
            static_cast<unsigned long long>(snapshot.vm_kib),
            static_cast<unsigned long long>(snapshot.hwm_kib),
            static_cast<unsigned long long>(snapshot.heap_bytes),
            static_cast<unsigned long long>(snapshot.mmap_bytes));
        return 0;
    }

    std::size_t repeat = options.repeat;
    if (repeat == 0) {
        const startup_cost::PoolInfo* info =
            startup_cost::FindPool(options.child_id);
        repeat = info != nullptr ? RepeatFor(*info) : 1000;
    }
    const ProbeMode mode =
        options.mode == "mem" ? ProbeMode::kMemory : ProbeMode::kTime;

    ProbeResult result;
    if (options.child_id == "__all__") {
        result = startup_cost::RunAllPools(true);
    } else if (options.child_id == "__all_eager__") {
        result = startup_cost::RunAllPools(false);
    } else if (options.child_id == "__scheduler__") {
        result = startup_cost::RunSchedulerOnly();
    } else {
        result = startup_cost::RunProbe(options.child_id, mode, repeat);
    }

    std::printf(
        "probe=%s mode=%s ok=%d init_ns=%llu use_ns=%llu steady_ns=%llu "
        "min_ns=%llu p95_ns=%llu max_ns=%llu samples=%llu "
        "rss_kib=%lld vm_kib=%lld hwm_kib=%lld heap_bytes=%lld "
        "mmap_bytes=%lld wait_ns=%llu slow=%llu",
        options.child_id.c_str(), options.mode.c_str(), result.ok ? 1 : 0,
        static_cast<unsigned long long>(result.init_ns),
        static_cast<unsigned long long>(result.use_ns),
        static_cast<unsigned long long>(result.steady_ns),
        static_cast<unsigned long long>(result.min_ns),
        static_cast<unsigned long long>(result.p95_ns),
        static_cast<unsigned long long>(result.max_ns),
        static_cast<unsigned long long>(result.samples),
        static_cast<long long>(result.rss_kib),
        static_cast<long long>(result.vm_kib),
        static_cast<long long>(result.hwm_kib),
        static_cast<long long>(result.heap_bytes),
        static_cast<long long>(result.mmap_bytes),
        static_cast<unsigned long long>(result.non_pool_wait_ns),
        static_cast<unsigned long long>(result.slow_samples));
    if (!result.stages.empty()) {
        std::printf(" stages=");
        for (std::size_t i = 0; i < result.stages.size(); ++i) {
            if (i != 0) {
                std::printf(",");
            }
            std::printf("%s:%llu", result.stages[i].name.c_str(),
                        static_cast<unsigned long long>(result.stages[i].ns));
        }
    } else {
        std::printf(" stages=");
    }
    std::printf("\n");
    if (!result.ok && !result.error.empty()) {
        std::fprintf(stderr, "probe failed: %s\n", result.error.c_str());
    }
    return result.ok ? 0 : 1;
}

// ---------------------------------------------------------------------------
// 父模式：采集
// ---------------------------------------------------------------------------

Baseline MeasureBaseline(const std::string& exe,
                         const std::vector<std::string>& arguments,
                         std::size_t samples) {
    Baseline baseline;
    std::vector<double> walls;
    walls.reserve(samples);
    for (std::size_t i = 0; i < samples; ++i) {
        const ChildRun run = RunChild(exe, arguments);
        if (!run.spawned || run.exit_code != 0) {
            continue;
        }
        walls.push_back(static_cast<double>(run.wall_ns));
        baseline.max_rss_kib =
            std::max(baseline.max_rss_kib, run.max_rss_kib);
    }
    if (walls.empty()) {
        return baseline;
    }
    baseline.ok = true;
    baseline.samples = walls.size();
    baseline.min_wall_ns = static_cast<std::uint64_t>(
        *std::min_element(walls.begin(), walls.end()));
    baseline.median_wall_ns = static_cast<std::uint64_t>(Median(walls));
    baseline.max_wall_ns = static_cast<std::uint64_t>(
        *std::max_element(walls.begin(), walls.end()));
    return baseline;
}

void MeasurePool(const std::string& exe, PoolMeasurement& measurement,
                 bool quick) {
    const PoolInfo& info = *measurement.info;

    if (info.repeatable) {
        // 急切池：首次成本必须和稳态分开 —— 首次可能走 mmap，之后从分配器
        // 缓存复用（同一进程内重复构造只会测到后者）。
        //   首次：每次一个新进程，只构造一次，跑 21 次取中位；
        //   稳态：一个进程内循环 repeat 次取中位。
        const std::size_t first_samples = quick ? 3 : kEagerFirstSamples;
        std::vector<double> firsts;
        std::vector<double> rss;
        std::vector<double> heaps;
        std::vector<double> vms;
        firsts.reserve(first_samples);
        rss.reserve(first_samples);
        heaps.reserve(first_samples);
        vms.reserve(first_samples);
        for (std::size_t i = 0; i < first_samples; ++i) {
            const ChildRun run =
                RunChild(exe, {"--child", info.id, "--mode", "mem", "--repeat", "1"});
            const ProbeResult parsed = ParseProbeResult(run);
            if (!parsed.ok) {
                continue;
            }
            firsts.push_back(static_cast<double>(parsed.init_ns));
            rss.push_back(static_cast<double>(parsed.rss_kib));
            heaps.push_back(static_cast<double>(parsed.heap_bytes));
            vms.push_back(static_cast<double>(parsed.vm_kib));
        }
        if (!firsts.empty()) {
            measurement.median_init_ns =
                static_cast<std::uint64_t>(Median(firsts));
            measurement.p95_init_ns =
                static_cast<std::uint64_t>(Percentile(firsts, 0.95));
            measurement.min_init_ns = static_cast<std::uint64_t>(
                *std::min_element(firsts.begin(), firsts.end()));
            measurement.memory.rss_kib =
                static_cast<std::int64_t>(Median(rss));
            measurement.memory.heap_bytes =
                static_cast<std::int64_t>(Median(heaps));
            measurement.memory.vm_kib = static_cast<std::int64_t>(Median(vms));
            measurement.memory.ok = true;
        }

        const std::size_t repeat = quick ? 200 : RepeatFor(info);
        const ChildRun run =
            RunChild(exe, {"--child", info.id, "--mode", "time", "--repeat",
                           std::to_string(repeat)});
        measurement.timing = ParseProbeResult(run);
        measurement.median_steady_ns = measurement.timing.init_ns;
        measurement.samples = firsts.size();
        measurement.ok = measurement.timing.ok && measurement.memory.ok;
        return;
    }

    // 惰性池：内存跑一次（RSS 增量稳定），时间跨进程取样。
    {
        const ChildRun run = RunChild(
            exe, {"--child", info.id, "--mode", "mem", "--repeat", "1"});
        measurement.memory = ParseProbeResult(run);
    }
    const std::size_t samples = quick ? 5 : kLazySamples;
    std::vector<double> inits;
    std::vector<double> steadies;
    inits.reserve(samples);
    steadies.reserve(samples);
    bool all_ok = true;
    for (std::size_t i = 0; i < samples; ++i) {
        const ChildRun run = RunChild(
            exe, {"--child", info.id, "--mode", "time", "--repeat", "1"});
        const ProbeResult parsed = ParseProbeResult(run);
        if (!parsed.ok) {
            all_ok = false;
            continue;
        }
        inits.push_back(static_cast<double>(parsed.init_ns));
        if (parsed.steady_ns != 0) {
            steadies.push_back(static_cast<double>(parsed.steady_ns));
        }
    }
    if (!inits.empty()) {
        measurement.median_init_ns = static_cast<std::uint64_t>(Median(inits));
        measurement.median_steady_ns =
            steadies.empty() ? 0
                             : static_cast<std::uint64_t>(Median(steadies));
        // time 模式每次都要新建进程，探针内部只可能测一次「第二次使用」；
        // 拿 mem 模式里那次 second 作为稳态值（同一探针、同一代码路径）。
        if (measurement.median_steady_ns == 0 &&
            measurement.memory.steady_ns != 0) {
            measurement.median_steady_ns = measurement.memory.steady_ns;
        }
        measurement.min_init_ns = static_cast<std::uint64_t>(
            *std::min_element(inits.begin(), inits.end()));
        measurement.p95_init_ns =
            static_cast<std::uint64_t>(Percentile(inits, 0.95));
        measurement.samples = inits.size();
    }
    measurement.ok = all_ok && measurement.memory.ok;
}

/// @brief 场景 C：同一子命令跑多次，时间取**最小值**、内存取中位。
///
/// 为什么时间取最小而不是中位：调度干扰、缺页、宿主负载只会让一次运行
/// **更慢**，不会更快。最小值就是「最接近无干扰」的那一次，代表这个池组合
/// 的真实成本；中位在大干扰下会被整体拉高（实测同一台机器上一次 park→wake
/// 可以从 9.7 ms 漂到 19.4 ms）。分段数据也来自这一次。
ProbeResult MeasureAggregate(const std::string& exe,
                             const std::string& child_id, bool quick,
                             std::uint64_t* wall_median_ns) {
    const std::size_t samples = quick ? 3 : kAggregateSamples;
    std::vector<double> timings;
    std::vector<double> walls;
    std::vector<double> rss;
    std::vector<double> heaps;
    ProbeResult best;
    std::uint64_t best_ns = 0;
    bool have_best = false;
    for (std::size_t i = 0; i < samples; ++i) {
        const ChildRun run = RunChild(exe, {"--child", child_id});
        const ProbeResult parsed = ParseProbeResult(run);
        if (!parsed.ok) {
            continue;
        }
        timings.push_back(static_cast<double>(parsed.init_ns));
        walls.push_back(static_cast<double>(run.wall_ns));
        rss.push_back(static_cast<double>(parsed.rss_kib));
        heaps.push_back(static_cast<double>(parsed.heap_bytes));
        if (!have_best || parsed.init_ns < best_ns) {
            best = parsed;
            best_ns = parsed.init_ns;
            have_best = true;
        }
    }
    ProbeResult result = best;
    if (!timings.empty()) {
        result.ok = true;
        result.init_ns = static_cast<std::uint64_t>(
            *std::min_element(timings.begin(), timings.end()));
        result.use_ns = result.init_ns;
        result.min_ns = result.init_ns;
        result.max_ns = static_cast<std::uint64_t>(
            *std::max_element(timings.begin(), timings.end()));
        result.p95_ns = static_cast<std::uint64_t>(Percentile(timings, 0.95));
        result.samples = timings.size();
        result.rss_kib = static_cast<std::int64_t>(Median(rss));
        result.heap_bytes = static_cast<std::int64_t>(Median(heaps));
        // 长尾：比最干净样本慢 3 倍以上的次数。场景 C 的 park/wake 是双峰的
        // （要么几十 µs，要么约 10 ms），这个计数就是慢路径的发生频率。
        const double clean = static_cast<double>(result.init_ns);
        result.slow_samples = static_cast<std::uint64_t>(std::count_if(
            timings.begin(), timings.end(), [clean](double value) {
                return clean > 0.0 && value > clean * 3.0;
            }));
        if (wall_median_ns != nullptr) {
            *wall_median_ns = static_cast<std::uint64_t>(Median(walls));
        }
    } else {
        result.ok = false;
    }
    return result;
}

// ---------------------------------------------------------------------------
// 环境信息
// ---------------------------------------------------------------------------

std::string FormatDate() {
    const std::time_t now = std::time(nullptr);
    std::tm local {};
    ::localtime_r(&now, &local);
    char buffer[64];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &local);
    return std::string(buffer);
}

std::string HostName() {
    char buffer[256] = {0};
    if (::gethostname(buffer, sizeof(buffer) - 1) != 0) {
        return "unknown";
    }
    return std::string(buffer);
}

std::string KernelText() {
    struct utsname info {};
    if (::uname(&info) != 0) {
        return "unknown";
    }
    return std::string(info.sysname) + " " + info.release;
}

std::string CpuText() {
    FILE* file = std::fopen("/proc/cpuinfo", "r");
    if (file == nullptr) {
        return "unknown";
    }
    char line[512];
    std::string model;
    while (std::fgets(line, sizeof(line), file) != nullptr) {
        if (std::strncmp(line, "model name", 10) == 0) {
            const char* colon = std::strchr(line, ':');
            if (colon != nullptr) {
                model = colon + 1;
                while (!model.empty() &&
                       (model.front() == ' ' || model.front() == '\t')) {
                    model.erase(model.begin());
                }
                while (!model.empty() &&
                       (model.back() == '\n' || model.back() == '\r')) {
                    model.pop_back();
                }
                break;
            }
        }
    }
    std::fclose(file);
    return model.empty() ? std::string("unknown") : model;
}

#ifndef STARTUP_COST_BUILD_DIR
#define STARTUP_COST_BUILD_DIR "(unknown)"
#endif
#ifndef STARTUP_COST_BUILD_TYPE
#define STARTUP_COST_BUILD_TYPE "(unknown)"
#endif

// ---------------------------------------------------------------------------
// 父模式
// ---------------------------------------------------------------------------

int RunParent(const Options& options) {
    const std::string exe = SelfExe();
    if (exe.empty()) {
        std::fprintf(stderr, "无法解析 /proc/self/exe\n");
        return 1;
    }
    const std::string min_exe = DirName(exe) + "/startup_cost_min";

    // 把父进程校准好的 TSC 频率传给子进程，避免每次 exec 重新校准
    // （51 次 × 5 个惰性池 × 15 ms 会多花近 4 秒）。
    if (std::getenv("STARTUP_COST_WALL_CLOCK") == nullptr) {
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%.12f",
                      startup_cost::Clock().ns_per_tick());
        ::setenv("STARTUP_COST_TSC_NS", buffer, 1);
    } else {
        ::unsetenv("STARTUP_COST_TSC_NS");
    }

    Summary summary;
    summary.date = FormatDate();
    summary.host = HostName();
    summary.kernel = KernelText();
    summary.cpu = CpuText();
    summary.build_dir = STARTUP_COST_BUILD_DIR;
    summary.build_type = STARTUP_COST_BUILD_TYPE;
    summary.compiler = __VERSION__;
    summary.tsc_valid = startup_cost::Clock().valid();
    summary.ns_per_tick = startup_cost::Clock().ns_per_tick();
    summary.lazy_samples = options.quick ? 5 : kLazySamples;
    summary.eager_repeat = options.quick ? 200 : kEagerRepeat;
    summary.eager_first_samples = options.quick ? 3 : kEagerFirstSamples;

    std::printf("[startup-cost] 计时器: %s (%.4f ns/tick)\n",
                summary.tsc_valid ? "rdtsc" : "clock_gettime",
                summary.ns_per_tick);
    std::printf("[startup-cost] 场景 A：基线…\n");
    std::fflush(stdout);

    const std::size_t baseline_samples =
        options.quick ? 5 : kBaselineSamples;
    if (::access(min_exe.c_str(), X_OK) == 0) {
        summary.min_program =
            MeasureBaseline(min_exe, std::vector<std::string>{},
                            baseline_samples);
    } else {
        std::fprintf(stderr,
                     "[startup-cost] 未找到 %s，跳过最小程序基线\n",
                     min_exe.c_str());
    }
    summary.empty_program = MeasureBaseline(
        exe, {"--child", "__none__"}, baseline_samples);

    std::printf("[startup-cost] 场景 B：逐个池…\n");
    std::fflush(stdout);
    for (const PoolInfo& info : PoolCatalog()) {
        PoolMeasurement measurement;
        measurement.info = &info;
        MeasurePool(exe, measurement, options.quick);
        summary.pools.push_back(measurement);
        std::printf("  %-20s %12s  %12s  %s\n", info.id,
                    HumanNs(static_cast<double>(measurement.median_init_ns))
                        .c_str(),
                    startup_cost::HumanKiBDelta(measurement.memory.rss_kib)
                        .c_str(),
                    measurement.ok ? "" : "[样本缺失]");
        std::fflush(stdout);
    }

    std::printf("[startup-cost] 场景 C：全部池…\n");
    std::fflush(stdout);
    summary.all_eager = MeasureAggregate(exe, "__all_eager__", options.quick,
                                         nullptr);
    summary.all_pools = MeasureAggregate(exe, "__all__", options.quick,
                                         &summary.all_pools_wall_ns);
    summary.scheduler_only =
        MeasureAggregate(exe, "__scheduler__", options.quick, nullptr);

    startup_cost::PrintConsole(summary);

    if (!options.no_report) {
        if (startup_cost::WriteMarkdown(options.out_path, summary)) {
            std::printf("[startup-cost] 报告已写入 %s\n", options.out_path.c_str());
        } else {
            std::fprintf(stderr, "[startup-cost] 写入 %s 失败\n",
                         options.out_path.c_str());
            return 1;
        }
    }

    bool all_ok = true;
    for (const PoolMeasurement& measurement : summary.pools) {
        all_ok = all_ok && measurement.ok;
    }
    all_ok = all_ok && summary.all_pools.ok && summary.all_eager.ok &&
             summary.scheduler_only.ok;
    return all_ok ? 0 : 2;
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--child" && i + 1 < argc) {
            options.child_id = argv[++i];
        } else if (argument == "--mode" && i + 1 < argc) {
            options.mode = argv[++i];
        } else if (argument == "--repeat" && i + 1 < argc) {
            options.repeat = static_cast<std::size_t>(
                std::strtoull(argv[++i], nullptr, 10));
        } else if (argument == "--out" && i + 1 < argc) {
            options.out_path = argv[++i];
        } else if (argument == "--list") {
            options.list = true;
        } else if (argument == "--quick") {
            options.quick = true;
        } else if (argument == "--no-report") {
            options.no_report = true;
        } else if (argument == "--help" || argument == "-h") {
            std::printf(
                "用法: startup_cost [选项]\n"
                "  --child <id>   子模式：只跑一个探针（mem/time）\n"
                "  --mode mem|time\n"
                "  --repeat N     子模式重复次数\n"
                "  --out PATH     markdown 报告路径（默认 /tmp/startup_cost.md）\n"
                "  --quick        减少样本（调试）\n"
                "  --list         列出全部池 id\n");
            return 0;
        }
    }

    // 计时器：子进程继承父进程校准出的 TSC 频率。
    if (const char* ns_per_tick = std::getenv("STARTUP_COST_TSC_NS")) {
        const double value = std::strtod(ns_per_tick, nullptr);
        startup_cost::SetClock(TscClock::FromNsPerTick(value));
    }
    if (std::getenv("STARTUP_COST_WALL_CLOCK") != nullptr) {
        // 强制退回 clock_gettime：用于在 TSC 可能不同步的虚拟化环境里
        // 交叉验证长间隔测量。
        startup_cost::SetClock(TscClock{});
    }
    if (!startup_cost::Clock().valid()) {
        startup_cost::SetClock(TscClock::Calibrate());
    }

    if (options.list) {
        for (const PoolInfo& info : PoolCatalog()) {
            std::printf("%-20s %-24s %s\n", info.id, info.title, info.source);
        }
        return 0;
    }

    if (!options.child_id.empty()) {
        return RunChildMode(options);
    }
    return RunParent(options);
}
