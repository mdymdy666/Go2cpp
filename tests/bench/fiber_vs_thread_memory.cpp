/**
 * @file fiber_vs_thread_memory.cpp
 * @brief Fiber 与 OS 线程的单体内存成本基准（VmRSS / VmSize / VmData 三视角）。
 *
 * ## 为什么需要它
 * 常见的「fiber 比线程省内存」说法只对了一半：
 *   - 从物理内存（VmRSS）看，一个已经触页的 fiber 栈 + FiberRecord 常常
 *     不比线程便宜，因为线程的 16KB 内核栈与 task_struct 不计入进程 RSS；
 *   - 从地址空间（VmSize）与「可创建数量」看，fiber 的预留/按需提交才体现优势。
 * 本程序把两种口径分开测，避免用单一数字下结论。
 *
 * ## 测量方法
 * 每个模式在**独立进程**中运行（由 run_memory_bench.sh 逐配置拉起），因此
 * 上一组配置的栈池/分配器缓存不会污染下一组。单次运行内按阶段采样：
 *   base    —— main() 刚进入、尚未创建任何对象；
 *   created —— N 个对象/任务已建立但栈尚未触页（fiber 的 0 栈用量基线）；
 *   touched —— N 个对象同时在栈上触页并保持驻留（fiber 用 Suspend 保持挂起，
 *              线程用自旋门保持阻塞，scheduler 任务用 channel park 驻留）。
 * 每个阶段读取 /proc/self/status 与 /proc/self/smaps_rollup 三次，逐字段取
 * 中位，降低单次采样的抖动。
 *
 * ## 触页为什么不用普通局部数组
 * 编译器会把「写了但没读」的局部数组优化掉，或把栈帧压到远小于请求值。
 * 这里用 noinline 函数 + alloca + 逐页 volatile 写，确保每一页都真正落页。
 * 触页量取「请求栈容量 - 8KB 余量」（4KB 档取 1/4），避免吃穿栈顶的
 * 上下文帧；实际触页量写入 touched_kb 列，换算时以它为透明依据。
 *
 * ## 已知边界（详见 docs/memory-benchmark.md）
 * - 线程的 16KB 内核栈、task_struct、页表内核部分都不计入 VmRSS/VmSize；
 * - glibc 会给每个 malloc 过的线程预留一个 64MiB 的 per-thread arena
 *   （PROT_NONE，不计 RSS），默认会主导线程的 VmSize；`--arena-max=1`
 *   可关闭该预留，得到「纯栈 + TLS」的可比口径；
 * - Fiber 完成后栈会进入 StackCache 复用，所以必须让 N 个 fiber 保持挂起
 *   才能测到 N 份栈；池化本身会让「结束后」的 RSS 不回落；
 * - scheduler 默认给 growable 栈预留 8MiB 虚拟地址，VmSize 会远大于 RSS；
 * - 4KB 档在 scheduler 路径下不足以承载 Channel park 的 C++ 帧，可能 SIGSEGV，
 *   脚本会把该配置记为 CRASH 而不是伪造数据。
 *
 * 用法：
 *   fiber_vs_thread_memory --mode=fiber-direct --n=1000 --stack-kb=64
 * 输出：TSV 到 stdout（表头 + base/created/touched 三行），便于脚本汇聚。
 */
#include "go2cpp/concurrency/channel.hpp"
#include "go2cpp/concurrency/fiber.hpp"
#include "go2cpp/concurrency/scheduler.hpp"

#include <malloc.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <limits.h>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

/// 防止编译器把触页写操作判定为死代码。
volatile unsigned long g_sink = 0;

/// 进程内存快照（单位 KB，-1 表示该字段不可用）。
struct Sample {
    long rss_kb{-1};
    long vsize_kb{-1};
    long vdata_kb{-1};
    long pss_kb{-1};
    long anon_kb{-1};
    long maxrss_kb{-1};
    long threads{-1};
};

/// 解析形如 "VmRSS:     1234 kB" 的数值（取第一个数字段）。
long ParseNumber(const std::string& line) {
    std::size_t i = 0;
    while (i < line.size() && (line[i] < '0' || line[i] > '9')) {
        ++i;
    }
    if (i >= line.size()) {
        return -1;
    }
    return std::strtol(line.c_str() + i, nullptr, 10);
}

/// 读 /proc/self/status 的进程级字段。
void ReadStatus(Sample& s) {
    std::ifstream f("/proc/self/status");
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("VmRSS:", 0) == 0) {
            s.rss_kb = ParseNumber(line);
        } else if (line.rfind("VmSize:", 0) == 0) {
            s.vsize_kb = ParseNumber(line);
        } else if (line.rfind("VmData:", 0) == 0) {
            s.vdata_kb = ParseNumber(line);
        } else if (line.rfind("VmHWM:", 0) == 0) {
            s.maxrss_kb = ParseNumber(line);
        } else if (line.rfind("Threads:", 0) == 0) {
            s.threads = ParseNumber(line);
        }
    }
}

/**
 * @brief 读 smaps_rollup 的 Rss/Pss/Anonymous。
 * @details 部分内核/容器不允许读 smaps_rollup，此时字段保持 -1 而不是报错，
 *          基准仍然给出 VmRSS/VmSize。Pss 用来识别共享页（例如多个 fiber
 *          复用同一块物理页）导致 VmRSS 高估的情况。
 */
void ReadSmapsRollup(Sample& s) {
    std::ifstream f("/proc/self/smaps_rollup");
    if (!f) {
        return;
    }
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("Rss:", 0) == 0) {
            s.rss_kb = ParseNumber(line);  // 以 smaps_rollup 为准（更精确）
        } else if (line.rfind("Pss:", 0) == 0) {
            s.pss_kb = ParseNumber(line);
        } else if (line.rfind("Anonymous:", 0) == 0) {
            s.anon_kb = ParseNumber(line);
        }
    }
}

/// 一次完整采样（status + smaps_rollup + getrusage）。
Sample TakeSample() {
    Sample s;
    ReadStatus(s);
    ReadSmapsRollup(s);
    struct rusage ru {};
    if (::getrusage(RUSAGE_SELF, &ru) == 0) {
        s.maxrss_kb = ru.ru_maxrss;  // Linux 上单位是 KB
    }
    return s;
}

/// 取一组 long 的中位数；-1（不可用）不参与排序。
long Median(std::vector<long> values) {
    values.erase(std::remove(values.begin(), values.end(), -1L), values.end());
    if (values.empty()) {
        return -1;
    }
    const auto mid = values.begin() + static_cast<long>(values.size() / 2);
    std::nth_element(values.begin(), mid, values.end());
    return *mid;
}

/// 连续采样 samples 次，逐字段取中位。
Sample MedianSample(int samples, int settle_ms) {
    if (settle_ms > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(settle_ms));
    }
    std::vector<Sample> all;
    all.reserve(static_cast<std::size_t>(std::max(1, samples)));
    for (int i = 0; i < std::max(1, samples); ++i) {
        all.push_back(TakeSample());
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    Sample out;
    auto pick = [&all](long Sample::*field) {
        std::vector<long> v;
        v.reserve(all.size());
        for (const auto& s : all) {
            v.push_back(s.*field);
        }
        return Median(std::move(v));
    };
    out.rss_kb = pick(&Sample::rss_kb);
    out.vsize_kb = pick(&Sample::vsize_kb);
    out.vdata_kb = pick(&Sample::vdata_kb);
    out.pss_kb = pick(&Sample::pss_kb);
    out.anon_kb = pick(&Sample::anon_kb);
    out.maxrss_kb = pick(&Sample::maxrss_kb);
    out.threads = pick(&Sample::threads);
    return out;
}

/**
 * @brief 在调用者栈上真实触页 bytes 字节。
 * @details noinline + alloca + 逐页 volatile 写，三者缺一不可：
 *          - noinline 防止内联后与调用者帧合并；
 *          - alloca 让触页范围随参数变化，而不是固定小数组；
 *          - volatile 写防止整个循环被优化掉。
 *          函数返回后栈指针恢复，但已写入的页保持驻留（RSS 不回落）。
 */
__attribute__((noinline)) void TouchStack(std::size_t bytes) {
    if (bytes == 0) {
        return;
    }
    volatile unsigned char* p =
        static_cast<volatile unsigned char*>(__builtin_alloca(bytes));
    const std::size_t page = 4096;
    for (std::size_t i = 0; i < bytes; i += page) {
        p[i] = static_cast<unsigned char>(i ^ 0x5AU);
    }
    p[bytes - 1] = 0xA5U;
    g_sink += p[0] + p[bytes - 1];
}

/// 请求栈容量对应的触页量：留 8KB 给上下文/帧，4KB 档只触 1/4。
std::size_t TouchBytesFor(std::size_t stack_bytes) {
    if (stack_bytes > 12288U) {
        return stack_bytes - 8192U;
    }
    return std::max<std::size_t>(1024U, stack_bytes / 4U);
}

/// 轮询等待计数达到目标；返回 false 表示超时。
bool WaitCount(const std::atomic<int>& counter, int target, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    while (counter.load(std::memory_order_acquire) < target) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

/// 模式执行结果：created/touched 快照 + 实际创建数 + 备注。
struct RunResult {
    Sample created;
    Sample touched;
    int n_ok{0};
    long stack_effective_kb{0};
    long touched_kb{0};
    std::string note;
};

// ---------------------------------------------------------------------------
// 线程模式
// ---------------------------------------------------------------------------

struct ThreadCtx {
    std::atomic<int>* ready{nullptr};
    std::atomic<int>* done{nullptr};
    std::atomic<bool>* go{nullptr};
    std::atomic<bool>* stop{nullptr};
    std::size_t touch{0};
};

void* ThreadMain(void* arg) {
    auto* ctx = static_cast<ThreadCtx*>(arg);
    ctx->ready->fetch_add(1, std::memory_order_release);
    while (!ctx->go->load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    TouchStack(ctx->touch);
    ctx->done->fetch_add(1, std::memory_order_release);
    // 必须等主线程测完 touched 再退出：线程一旦返回，内核会回收/缓存它的
    // 栈，touched 阶段就测不到 N 份驻留栈了（早期版本正是在这里测成 1 个线程）。
    while (!ctx->stop->load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    return nullptr;
}

RunResult RunThreads(long stack_kb, int n, int timeout_ms, int settle_ms,
                     int samples) {
    RunResult r;
    std::size_t stack_bytes = static_cast<std::size_t>(stack_kb) * 1024U;
    // glibc 拒绝小于 PTHREAD_STACK_MIN 的栈；4KB 档因此实际是 16KB。
    if (stack_bytes < static_cast<std::size_t>(PTHREAD_STACK_MIN)) {
        stack_bytes = static_cast<std::size_t>(PTHREAD_STACK_MIN);
    }
    pthread_attr_t attr;
    ::pthread_attr_init(&attr);
    ::pthread_attr_setstacksize(&attr, stack_bytes);
    std::size_t eff = 0;
    ::pthread_attr_getstacksize(&attr, &eff);
    r.stack_effective_kb = static_cast<long>(eff / 1024U);

    const std::size_t touch = TouchBytesFor(static_cast<std::size_t>(stack_kb) * 1024U);
    r.touched_kb = static_cast<long>(touch / 1024U);

    std::atomic<int> ready{0};
    std::atomic<int> done{0};
    std::atomic<bool> go{false};
    std::atomic<bool> stop{false};
    std::vector<ThreadCtx> ctxs(static_cast<std::size_t>(n));
    std::vector<pthread_t> tids(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        ctxs[static_cast<std::size_t>(i)] =
            ThreadCtx{&ready, &done, &go, &stop, touch};
        const int rc = ::pthread_create(&tids[static_cast<std::size_t>(i)], &attr,
                                        ThreadMain, &ctxs[static_cast<std::size_t>(i)]);
        if (rc != 0) {
            r.note = "pthread_create_failed_errno=" + std::to_string(rc);
            break;
        }
        ++r.n_ok;
    }
    ::pthread_attr_destroy(&attr);

    const bool all_ready = WaitCount(ready, r.n_ok, timeout_ms);
    r.created = MedianSample(samples, 0);
    go.store(true, std::memory_order_release);
    const bool all_done = WaitCount(done, r.n_ok, timeout_ms);
    r.touched = MedianSample(samples, settle_ms);
    stop.store(true, std::memory_order_release);
    if (!all_ready || !all_done) {
        r.note += r.note.empty() ? "" : ";";
        r.note += "wait_timeout";
    }
    for (int i = 0; i < r.n_ok; ++i) {
        ::pthread_join(tids[static_cast<std::size_t>(i)], nullptr);
    }
    return r;
}

// ---------------------------------------------------------------------------
// Fiber 模式（scheduler 之外，直接 resume/Suspend）
// ---------------------------------------------------------------------------

RunResult RunFiberDirect(long stack_kb, int n, bool growable, int settle_ms,
                         int samples) {
    RunResult r;
    const std::size_t stack_bytes = static_cast<std::size_t>(stack_kb) * 1024U;
    const std::size_t touch = TouchBytesFor(stack_bytes);
    r.touched_kb = static_cast<long>(touch / 1024U);
    r.stack_effective_kb = static_cast<long>(stack_bytes / 1024U);

    std::vector<std::unique_ptr<go2cpp::Fiber>> fibers;
    fibers.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        try {
            // growable=true 时 reserve 传 0 → 默认 8MiB 虚拟预留；
            // false 时 reserve == 初始提交量 → 固定栈布局。
            fibers.push_back(std::make_unique<go2cpp::Fiber>(
                [touch] {
                    TouchStack(touch);
                    (void)go2cpp::Fiber::Suspend();
                },
                stack_bytes, growable ? 0U : stack_bytes, growable));
        } catch (const std::exception& e) {
            r.note = std::string("fiber_create_failed:") + e.what();
            break;
        }
        ++r.n_ok;
    }
    if (r.n_ok > 0) {
        r.stack_effective_kb = static_cast<long>(fibers[0]->stack_size() / 1024U);
    }
    r.created = MedianSample(samples, 0);

    int resumed = 0;
    for (auto& f : fibers) {
        if (f->resume()) {
            ++resumed;
        }
    }
    if (resumed != r.n_ok) {
        r.note += r.note.empty() ? "" : ";";
        r.note += "resume_failed=" + std::to_string(r.n_ok - resumed);
    }
    r.touched = MedianSample(samples, settle_ms);

    // 析构会从 Suspend 点恢复 fiber 使其自然结束，再把栈交回 StackCache；
    // 这一步可能较慢，但发生在测量之后，不影响数据。
    fibers.clear();
    return r;
}

// ---------------------------------------------------------------------------
// Fiber 模式（Scheduler + Channel 门控，真实调度路径）
// ---------------------------------------------------------------------------

RunResult RunFiberScheduler(long stack_kb, int n, int timeout_ms, int settle_ms,
                            int samples) {
    RunResult r;
    const std::size_t stack_bytes = static_cast<std::size_t>(stack_kb) * 1024U;
    const std::size_t touch = TouchBytesFor(stack_bytes);
    r.touched_kb = static_cast<long>(touch / 1024U);
    r.stack_effective_kb = static_cast<long>(stack_bytes / 1024U);

    go2cpp::SchedulerConfig cfg;
    cfg.processor_count = 2;   // 固定 2 个 P，避免机器核数影响可复现性
    cfg.min_workers = 2;
    cfg.collect_metrics = false;
    cfg.fiber_stack_size = stack_bytes;
    cfg.fiber_stack_reserve_size = stack_bytes;
    cfg.fiber_stack_growable = false;
    cfg.fiber_bin_capacity = 0;  // 关闭 G 复用，避免池化掩盖分配成本

    go2cpp::Scheduler sched(cfg);
    sched.start();
    auto gate2 = go2cpp::MakeChannel<int>(0);
    auto gate3 = go2cpp::MakeChannel<int>(0);
    std::atomic<int> ready{0};
    std::atomic<int> done{0};

    for (int i = 0; i < n; ++i) {
        go2cpp::TaskOptions opt;
        opt.stack_size = stack_bytes;
        opt.stack_reserve_size = stack_bytes;
        opt.stack_growable = false;
        opt.fiber_bin_capacity = 0;
        auto task = sched.spawn(
            [gate2, gate3, &ready, &done, touch] {
                ready.fetch_add(1, std::memory_order_release);
                (void)gate2->Recv();   // 门 1：等主线程测完 created
                TouchStack(touch);
                done.fetch_add(1, std::memory_order_release);
                (void)gate3->Recv();   // 门 2：保持驻留到测量结束
            },
            opt);
        if (!task) {
            r.note = "spawn_failed";
            break;
        }
        ++r.n_ok;
    }

    const bool all_ready = WaitCount(ready, r.n_ok, timeout_ms);
    r.created = MedianSample(samples, 0);
    if (all_ready && r.n_ok > 0) {
        for (int i = 0; i < r.n_ok; ++i) {
            (void)gate2->Send(1);
        }
    }
    const bool all_done = WaitCount(done, r.n_ok, timeout_ms);
    r.touched = MedianSample(samples, settle_ms);
    if (!all_ready || !all_done) {
        r.note += r.note.empty() ? "" : ";";
        r.note += "wait_timeout";
    }

    // 关闭门 2 唤醒所有驻留任务，再关闭调度器。
    gate3->Close();
    gate2->Close();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    sched.shutdown();
    return r;
}

// ---------------------------------------------------------------------------
// mmap 对照（纯内核账户，无对象/记录开销）
// ---------------------------------------------------------------------------

RunResult RunMmap(long stack_kb, int n, int settle_ms, int samples) {
    RunResult r;
    const std::size_t stack_bytes = static_cast<std::size_t>(stack_kb) * 1024U;
    const std::size_t touch = TouchBytesFor(stack_bytes);
    r.touched_kb = static_cast<long>(touch / 1024U);
    r.stack_effective_kb = static_cast<long>(stack_bytes / 1024U);

    std::vector<void*> maps;
    maps.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        void* p = ::mmap(nullptr, stack_bytes, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) {
            r.note = "mmap_failed";
            break;
        }
        maps.push_back(p);
        ++r.n_ok;
    }
    r.created = MedianSample(samples, 0);
    for (void* p : maps) {
        std::memset(p, 0x5A, touch);
    }
    r.touched = MedianSample(samples, settle_ms);
    for (void* p : maps) {
        ::munmap(p, stack_bytes);
    }
    return r;
}

// ---------------------------------------------------------------------------
// 输出
// ---------------------------------------------------------------------------

const char* kHeader =
    "mode\tstack_kb\tphase\tn\tn_ok\tstack_effective_kb\ttouched_kb\trss_kb\t"
    "vsize_kb\tvdata_kb\tpss_kb\tanon_kb\tmaxrss_kb\tthreads\tnote";

void PrintRow(const std::string& mode, long stack_kb, const char* phase, int n,
              int n_ok, long eff_kb, long touched_kb, const Sample& s,
              const std::string& note) {
    std::printf("%s\t%ld\t%s\t%d\t%d\t%ld\t%ld\t%ld\t%ld\t%ld\t%ld\t%ld\t%ld\t%ld\t%s\n",
                mode.c_str(), stack_kb, phase, n, n_ok, eff_kb, touched_kb,
                s.rss_kb, s.vsize_kb, s.vdata_kb, s.pss_kb, s.anon_kb,
                s.maxrss_kb, s.threads, note.c_str());
}

struct Options {
    std::string mode = "fiber-direct";
    long stack_kb = 64;
    int n = 1000;
    int samples = 3;
    int settle_ms = 50;
    int timeout_ms = 120000;
    int arena_max = 0;  // >0 时 mallopt(M_ARENA_MAX, N)，1 = 关闭 per-thread arena
};

bool ParseArgs(int argc, char** argv, Options& opt) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto value_of = [&arg](const char* key) -> std::string {
            const std::string prefix = std::string(key) + "=";
            if (arg.rfind(prefix, 0) == 0) {
                return arg.substr(prefix.size());
            }
            return std::string();
        };
        const std::string mode = value_of("--mode");
        const std::string n = value_of("--n");
        const std::string stack = value_of("--stack-kb");
        const std::string samples = value_of("--samples");
        const std::string settle = value_of("--settle-ms");
        const std::string timeout = value_of("--timeout-ms");
        const std::string arena = value_of("--arena-max");
        if (!mode.empty()) {
            opt.mode = mode;
        } else if (!n.empty()) {
            opt.n = std::atoi(n.c_str());
        } else if (!stack.empty()) {
            opt.stack_kb = std::atol(stack.c_str());
        } else if (!samples.empty()) {
            opt.samples = std::atoi(samples.c_str());
        } else if (!settle.empty()) {
            opt.settle_ms = std::atoi(settle.c_str());
        } else if (!timeout.empty()) {
            opt.timeout_ms = std::atoi(timeout.c_str());
        } else if (!arena.empty()) {
            opt.arena_max = std::atoi(arena.c_str());
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
            return false;
        }
    }
    if (opt.n <= 0 || opt.stack_kb <= 0 || opt.samples <= 0) {
        std::fprintf(stderr, "n/stack-kb/samples must be positive\n");
        return false;
    }
    return true;
}

bool KnownMode(const std::string& mode) {
    return mode == "thread" || mode == "fiber-direct" || mode == "fiber-growable" ||
           mode == "fiber-sched" || mode == "mmap";
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!ParseArgs(argc, argv, opt)) {
        return 2;
    }
    if (!KnownMode(opt.mode)) {
        std::fprintf(stderr, "unknown mode: %s\n", opt.mode.c_str());
        return 2;
    }
    // 必须在任何线程/分配发生前设置；mallopt 只改 malloc 行为，不影响已有映射。
    if (opt.arena_max > 0) {
        (void)::mallopt(M_ARENA_MAX, opt.arena_max);
    }

    // 基线必须尽早取：之后任何 vector/库初始化都会计入配置成本。
    const Sample base = MedianSample(opt.samples, 0);

    RunResult result;
    try {
        if (opt.mode == "thread") {
            result = RunThreads(opt.stack_kb, opt.n, opt.timeout_ms, opt.settle_ms,
                                opt.samples);
        } else if (opt.mode == "fiber-direct") {
            result = RunFiberDirect(opt.stack_kb, opt.n, false, opt.settle_ms,
                                    opt.samples);
        } else if (opt.mode == "fiber-growable") {
            result = RunFiberDirect(opt.stack_kb, opt.n, true, opt.settle_ms,
                                    opt.samples);
        } else if (opt.mode == "fiber-sched") {
            result = RunFiberScheduler(opt.stack_kb, opt.n, opt.timeout_ms,
                                       opt.settle_ms, opt.samples);
        } else {
            result = RunMmap(opt.stack_kb, opt.n, opt.settle_ms, opt.samples);
        }
    } catch (const std::exception& e) {
        // 构造/调度初始化失败也输出一行，让脚本能区分“失败”与“没跑”。
        PrintRow(opt.mode, opt.stack_kb, "error", opt.n, 0, 0, 0, base,
                 std::string("exception:") + e.what());
        return 1;
    }

    if (opt.arena_max > 0 && result.note.empty()) {
        result.note = "arena_max=" + std::to_string(opt.arena_max);
    } else if (opt.arena_max > 0) {
        result.note += ";arena_max=" + std::to_string(opt.arena_max);
    }

    std::printf("%s\n", kHeader);
    PrintRow(opt.mode, opt.stack_kb, "base", opt.n, 0, 0, 0, base, result.note);
    PrintRow(opt.mode, opt.stack_kb, "created", opt.n, result.n_ok,
             result.stack_effective_kb, result.touched_kb, result.created, result.note);
    PrintRow(opt.mode, opt.stack_kb, "touched", opt.n, result.n_ok,
             result.stack_effective_kb, result.touched_kb, result.touched, result.note);
    return 0;
}
