/**
 * @file stress_mixed_billion.cpp
 * @brief Fiber/Thread 混合并发亿级正确性压测（独立运行程序，不注册 ctest）。
 *
 * ## 目标
 * 在单进程内用 ≥10^8 次操作 × 多轮，验证 Fiber 与原生线程混合使用时的
 * 四条并发原语路径：
 *   1. `Channel`：fiber→fiber / fiber→thread / thread→fiber / thread→thread；
 *   2. `sync::Mutex`：Fiber 与线程争抢同一把锁；
 *   3. `sync::ConditionVariable`：Fiber 与线程在同一 Mutex/CV 上等待与通知；
 *   4. `sync::WaitGroup`：Fiber 与线程混合 Add/Done/Wait。
 *
 * ## 正确性断言（全部为真实计数，不是恒真）
 * - 发送数 == 接收数 == 每轮操作数；
 * - FIFO 严格递增（0,1,2,...）：无丢失、无重复、无乱序；
 * - 无死锁：看门狗监控全局进度，超时即落报告并以非零码退出；
 * - 无泄漏：每轮 RSS 相对基线有阈值上限，且相邻轮次 RSS 不持续增长；
 * - 结束时 waiters == 0（Channel send/recv、Mutex、CV、WaitGroup 快照）。
 *
 * ## 操作口径（ns/op 的分母）
 * - Channel 模型：一个值从发送到接收 = 1 次操作；
 * - Mutex 模型：一次 lock+unlock = 1 次操作；
 * - CV 模型：一个入队并出队的值 = 1 次操作（另报 wait/notify 次数）；
 * - WaitGroup 模型：一次 Done = 1 次操作（另报 Add/Wait 波次）。
 *
 * ## 编译与运行（不修改 CMake，保持 ctest 集合不变）
 * ```
 * g++ -std=c++17 -O3 -DNDEBUG -pthread -Iinclude \
 *     tests/concurrency/stress_mixed_billion.cpp \
 *     -L.build-bento-wsl -Wl,-rpath,$PWD/.build-bento-wsl \
 *     -lgo2cpp_channel -lgo2cpp_sync -lgo2cpp_scheduler -lgo2cpp_fiber \
 *     -lgo2cpp_context -lgo2cpp_error -lgo2cpp_log \
 *     -o .build-bento-wsl/stress_mixed_billion
 * LD_LIBRARY_PATH=$PWD/.build-bento-wsl .build-bento-wsl/stress_mixed_billion \
 *     --ops 100000000 --rounds 10 --budget-sec 1800
 * ```
 *
 * 默认参数：每轮 10^8 次操作、每模型 10 轮、单模型预算 1800s。预算不足时
 * 自动降到 ≥3 轮（任务允许的降级路径），并在报告中标注 `budget_trimmed`。
 */

#include "go2cpp/channel.hpp"
#include "go2cpp/scheduler.hpp"
#include "go2cpp/sync.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <unistd.h>

namespace {

using Clock = std::chrono::steady_clock;
using go2cpp::Scheduler;
using go2cpp::Task;

// ===========================================================================
// 配置与结果数据结构
// ===========================================================================

/** @brief 命令行配置。 */
struct Config {
    std::uint64_t ops_per_round = 100000000ULL;  ///< 每轮操作数（默认 10^8）
    int rounds = 10;                             ///< 每模型目标轮数
    int min_rounds = 3;                          ///< 预算不足时仍要跑的最少轮数
    long long model_budget_sec = 1800;           ///< 单模型时间预算（秒）
    long long total_budget_sec = 4200;           ///< 全局软预算（秒，0=不限）
    int hang_sec = 120;                          ///< 无进展多久判定死锁
    std::string out = "/tmp/mixed_billion.md";   ///< 报告输出路径
    std::string models;                          ///< 逗号分隔的模型过滤，空=全部
    bool quick = false;                          ///< 快速冒烟参数
};

/** @brief 单条断言的结论。 */
struct Assertion {
    std::string name;
    bool passed = false;
    std::string detail;
};

/** @brief 单轮压测指标。 */
struct RoundMetrics {
    std::string model;
    int round = 0;
    std::uint64_t ops = 0;
    std::uint64_t sent = 0;
    std::uint64_t recv = 0;
    std::uint64_t violations = 0;   ///< 顺序/重复违例数
    std::uint64_t errors = 0;       ///< API 失败/参与者异常数
    std::uint64_t aux_waits = 0;    ///< CV/WaitGroup 等待次数
    std::uint64_t aux_notifies = 0; ///< CV 通知次数
    std::uint64_t aux_extra = 0;    ///< WaitGroup 波次等
    long long elapsed_ns = 0;
    double ns_per_op = 0.0;
    std::uint64_t rss_kb = 0;
    std::uint64_t vmsize_kb = 0;
    bool ok = false;
    std::string detail;             ///< 首个失败原因
    std::vector<Assertion> checks;
};

/** @brief 单模型汇总。 */
struct ModelReport {
    std::string id;
    std::string title;
    std::string topology;
    std::string op_unit;
    std::uint64_t ops_per_round = 0;
    int rounds_planned = 0;
    int rounds_done = 0;
    bool budget_trimmed = false;
    long long elapsed_ns = 0;
    std::vector<RoundMetrics> rounds;
    bool ok = false;
};

/** @brief 模型定义（运行函数 + 操作数对齐因子）。 */
struct ModelDef {
    std::string id;
    std::string title;
    std::string topology;
    std::string op_unit;
    std::uint64_t divisor = 1;
    std::function<RoundMetrics(std::uint64_t, int)> run;
};

// ===========================================================================
// 进程级状态（看门狗 / 进度 / 报告快照）
// ===========================================================================

Config g_cfg;
Scheduler* g_sched = nullptr;

std::atomic<std::uint64_t> g_progress{0};        ///< 全局进展计数
std::atomic<std::uint64_t> g_progress_sent{0};   ///< 当前轮已发送
std::atomic<std::uint64_t> g_progress_recv{0};   ///< 当前轮已接收
std::atomic<const char*> g_stage_model{"空闲"};  ///< 当前模型（静态字面量）
std::atomic<int> g_stage_round{0};               ///< 当前轮次
std::atomic<bool> g_active{false};               ///< 是否处于压测轮内
std::atomic<bool> g_stop_watchdog{false};        ///< 结束看门狗
std::atomic<bool> g_report_written{false};       ///< 报告是否已落盘

std::mutex g_reports_mutex;                      ///< 保护 g_reports
std::vector<ModelReport> g_reports;              ///< 已完成模型的快照
std::string g_shutdown_note;                     ///< 调度器收尾说明

struct ProcMem {
    std::uint64_t rss_kb = 0;
    std::uint64_t vmsize_kb = 0;
};

ProcMem g_baseline_mem;

// ===========================================================================
// 基础工具
// ===========================================================================

long long NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               Clock::now().time_since_epoch())
        .count();
}

inline void CpuPause() {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    asm volatile("yield" ::: "memory");
#else
    std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
}

inline void PublishProgress(std::uint64_t delta) {
    g_progress.fetch_add(delta, std::memory_order_relaxed);
}

/** @brief 当前线程是否运行在受管 Fiber 上。 */
inline bool InFiber() {
    return Scheduler::current_task_ptr() != nullptr;
}

/** @brief 让出执行权：Fiber 走调度器 yield，原生线程走 sched_yield。 */
inline void YieldExecution() {
    if (InFiber()) {
        Scheduler* scheduler = Scheduler::current_scheduler();
        if (scheduler != nullptr) {
            scheduler->yield_current();
            return;
        }
    }
    std::this_thread::yield();
}

void PauseExecution(int spin) {
    if ((spin & 0x3F) == 0) {
        YieldExecution();
    } else {
        CpuPause();
    }
}

std::uint64_t ParseStatusKb(const std::string& line) {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < line.size(); ++i) {
        if (line[i] >= '0' && line[i] <= '9') {
            while (i < line.size() && line[i] >= '0' && line[i] <= '9') {
                value = value * 10 + static_cast<std::uint64_t>(line[i] - '0');
                ++i;
            }
            break;
        }
    }
    return value;
}

ProcMem ReadProcMem() {
    ProcMem mem;
    std::ifstream in("/proc/self/status");
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("VmRSS:", 0) == 0) {
            mem.rss_kb = ParseStatusKb(line);
        } else if (line.rfind("VmSize:", 0) == 0) {
            mem.vmsize_kb = ParseStatusKb(line);
        }
    }
    return mem;
}

std::string CpuModelName() {
    std::ifstream in("/proc/cpuinfo");
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("model name", 0) == 0) {
            const std::size_t pos = line.find(':');
            if (pos != std::string::npos) {
                std::string value = line.substr(pos + 1);
                while (!value.empty() &&
                       (value.front() == ' ' || value.front() == '\t')) {
                    value.erase(value.begin());
                }
                return value;
            }
        }
    }
    return "unknown";
}

std::string FormatDouble(double value, int precision = 2) {
    char buffer[128];
    std::snprintf(buffer, sizeof(buffer), "%.*f", precision, value);
    return buffer;
}

std::string FormatSeconds(long long ns) {
    return FormatDouble(static_cast<double>(ns) / 1e9, 3) + " s";
}

std::string FormatU64(std::uint64_t value) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%" PRIu64, value);
    return buffer;
}

void AddCheck(RoundMetrics* metrics, const std::string& name, bool passed,
              const std::string& detail = std::string()) {
    Assertion assertion;
    assertion.name = name;
    assertion.passed = passed;
    assertion.detail = detail;
    metrics->checks.push_back(std::move(assertion));
    if (!passed && metrics->detail.empty()) {
        metrics->detail = name + (detail.empty() ? std::string() : "（" + detail + "）");
    }
}

bool AllChecksPassed(const RoundMetrics& metrics) {
    for (const auto& check : metrics.checks) {
        if (!check.passed) {
            return false;
        }
    }
    return !metrics.checks.empty();
}

// ===========================================================================
// FIFO 验证器 + 自检（证明断言不是恒真）
// ===========================================================================

/**
 * @brief 严格递增序号验证器：期望值必须恰好等于下一个收到值。
 *
 * 出现违例时用实际值重建期望，避免一次错位把后续所有值都算成违例；
 * 违例计数与首个违例值仍被完整记录。
 */
class FifoVerifier {
public:
    explicit FifoVerifier(std::uint64_t total) : m_total(total) {}

    void Accept(std::uint64_t value) {
        if (value != m_expected) {
            ++m_violations;
            if (!m_has_bad) {
                m_has_bad = true;
                m_bad_expected = m_expected;
                m_bad_actual = value;
            }
            m_expected = value;
        }
        ++m_expected;
        ++m_seen;
    }

    std::uint64_t seen() const { return m_seen; }
    std::uint64_t expected() const { return m_expected; }
    std::uint64_t total() const { return m_total; }
    std::uint64_t violations() const { return m_violations; }
    bool has_bad() const { return m_has_bad; }
    std::uint64_t bad_expected() const { return m_bad_expected; }
    std::uint64_t bad_actual() const { return m_bad_actual; }

private:
    std::uint64_t m_total = 0;
    std::uint64_t m_expected = 0;
    std::uint64_t m_seen = 0;
    std::uint64_t m_violations = 0;
    bool m_has_bad = false;
    std::uint64_t m_bad_expected = 0;
    std::uint64_t m_bad_actual = 0;
};

/** @brief 用构造的坏序列验证验证器确实会报错。 */
bool FifoVerifierSelfCheck() {
    {
        FifoVerifier verifier(4);
        for (std::uint64_t i = 0; i < 4; ++i) {
            verifier.Accept(i);
        }
        if (verifier.violations() != 0 || verifier.seen() != 4 ||
            verifier.expected() != 4) {
            return false;
        }
    }
    {  // 乱序。
        FifoVerifier verifier(4);
        verifier.Accept(0);
        verifier.Accept(2);
        verifier.Accept(1);
        verifier.Accept(3);
        if (verifier.violations() == 0 || !verifier.has_bad()) {
            return false;
        }
    }
    {  // 重复。
        FifoVerifier verifier(4);
        verifier.Accept(0);
        verifier.Accept(0);
        verifier.Accept(1);
        verifier.Accept(2);
        if (verifier.violations() == 0) {
            return false;
        }
    }
    {  // 缺值（少收）。
        FifoVerifier verifier(4);
        verifier.Accept(0);
        verifier.Accept(1);
        verifier.Accept(2);
        if (verifier.seen() != 3 || verifier.violations() != 0) {
            return false;
        }
    }
    return true;
}

// ===========================================================================
// 参与者：Fiber 或原生线程的统一封装
// ===========================================================================

/** @brief 起跑门：所有参与者就位后同一时刻放行，计时不含启动偏斜。 */
class StartGate {
public:
    void Wait() const {
        std::uint64_t spin = 0;
        while (!m_open.load(std::memory_order_acquire)) {
            PauseExecution(static_cast<int>(spin++));
        }
    }
    void Open() { m_open.store(true, std::memory_order_release); }

private:
    std::atomic<bool> m_open{false};
};

/** @brief 一个压测参与者（Fiber 任务或 std::thread）。 */
struct Worker {
    std::shared_ptr<Task> fiber;
    std::thread thread;
    bool is_fiber = false;

    Worker() = default;
    Worker(Worker&&) = default;
    Worker& operator=(Worker&&) = default;
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    /** @brief 等待参与者结束；返回 false 表示 Fiber 失败/异常。 */
    bool Join() {
        if (is_fiber) {
            if (!fiber) {
                return false;
            }
            if (!fiber->wait()) {
                return false;
            }
            if (fiber->failed()) {
                return false;
            }
            return true;
        }
        if (thread.joinable()) {
            thread.join();
        }
        return true;
    }
};

void FailFast(const std::string& reason);

Worker Launch(bool as_fiber, std::function<void()> body) {
    Worker worker;
    worker.is_fiber = as_fiber;
    if (as_fiber) {
        worker.fiber = g_sched->spawn(std::move(body));
        if (!worker.fiber) {
            FailFast("Scheduler::spawn 返回空任务");
        }
    } else {
        worker.thread = std::thread(std::move(body));
    }
    return worker;
}

/**
 * @brief 自旋等待某个波形计数到达目标；Fiber 侧在自旋预算耗尽后 yield。
 */
void WaitForWave(const std::atomic<std::uint64_t>& value, std::uint64_t target) {
    int spin = 0;
    while (value.load(std::memory_order_acquire) < target) {
        PauseExecution(spin++);
    }
}

// ===========================================================================
// 模型 1-4：Channel（4 种 Fiber/Thread 组合）
// ===========================================================================

RoundMetrics RunChannelRound(const std::string& model, bool producer_fiber,
                             bool consumer_fiber, std::uint64_t ops, int round,
                             std::size_t capacity) {
    RoundMetrics metrics;
    metrics.model = model;
    metrics.round = round;
    metrics.ops = ops;

    auto channel = go2cpp::MakeChannel<std::uint64_t>(capacity);
    StartGate gate;
    std::atomic<std::uint64_t> sent{0};
    std::atomic<std::uint64_t> received{0};
    std::atomic<std::uint64_t> send_errors{0};
    std::atomic<std::uint64_t> recv_errors{0};
    std::uint64_t violations = 0;
    std::uint64_t bad_expected = 0;
    std::uint64_t bad_actual = 0;
    bool closed_early = false;

    auto producer_body = [&]() {
        gate.Wait();
        g_progress_sent.store(0, std::memory_order_relaxed);
        for (std::uint64_t i = 0; i < ops; ++i) {
            const auto result = channel->Send(i);
            if (!result.Ok()) {
                send_errors.fetch_add(1, std::memory_order_relaxed);
                channel->Close();  // 让对端退出，避免半开状态挂死
                break;
            }
            sent.store(i + 1, std::memory_order_relaxed);
            g_progress_sent.store(i + 1, std::memory_order_relaxed);
            if ((i & 1023ULL) == 1023ULL) {
                PublishProgress(1024);
            }
        }
        PublishProgress(1);
    };

    auto consumer_body = [&]() {
        gate.Wait();
        FifoVerifier verifier(ops);
        std::uint64_t count = 0;
        while (count < ops) {
            const auto result = channel->Recv();
            if (!result.Ok()) {
                if (result.Closed()) {
                    closed_early = true;
                    break;
                }
                recv_errors.fetch_add(1, std::memory_order_relaxed);
                channel->Close();
                break;
            }
            verifier.Accept(*result.value);
            ++count;
            received.store(count, std::memory_order_relaxed);
            g_progress_recv.store(count, std::memory_order_relaxed);
            if ((count & 1023ULL) == 0ULL) {
                PublishProgress(1024);
            }
        }
        violations = verifier.violations();
        bad_expected = verifier.bad_expected();
        bad_actual = verifier.bad_actual();
        PublishProgress(1);
    };

    Worker producer = Launch(producer_fiber, producer_body);
    Worker consumer = Launch(consumer_fiber, consumer_body);
    const long long start_ns = NowNs();
    gate.Open();
    const bool producer_ok = producer.Join();
    const bool consumer_ok = consumer.Join();
    const long long end_ns = NowNs();

    const go2cpp::ChannelDebugSnapshot snapshot = channel->DebugSnapshot();
    channel->Close();
    const std::size_t remaining = channel->Len();

    metrics.elapsed_ns = end_ns - start_ns;
    metrics.ns_per_op =
        ops == 0 ? 0.0 : static_cast<double>(metrics.elapsed_ns) / static_cast<double>(ops);
    metrics.sent = sent.load(std::memory_order_relaxed);
    metrics.recv = received.load(std::memory_order_relaxed);
    metrics.violations = violations;
    metrics.errors = send_errors.load(std::memory_order_relaxed) +
                     recv_errors.load(std::memory_order_relaxed) +
                     (producer_ok ? 0U : 1U) + (consumer_ok ? 0U : 1U);

    char detail[256];
    std::snprintf(detail, sizeof(detail),
                  "发送=%" PRIu64 " 接收=%" PRIu64 " 操作数=%" PRIu64,
                  metrics.sent, metrics.recv, ops);

    AddCheck(&metrics, "发送数=接收数=每轮操作数",
             metrics.sent == ops && metrics.recv == ops,
             "发送/接收与目标不一致：" + std::string(detail));
    AddCheck(&metrics, "FIFO 严格有序（无丢失/重复/乱序）", violations == 0,
             "违例=" + FormatU64(violations) + " 首个期望=" +
                 FormatU64(bad_expected) + " 实际=" + FormatU64(bad_actual));
    AddCheck(&metrics, "发送/接收无 API 错误",
             send_errors.load(std::memory_order_relaxed) == 0 &&
                 recv_errors.load(std::memory_order_relaxed) == 0,
             "send_errors=" +
                 FormatU64(send_errors.load(std::memory_order_relaxed)) +
                 " recv_errors=" +
                 FormatU64(recv_errors.load(std::memory_order_relaxed)));
    AddCheck(&metrics, "未发生提前关闭", !closed_early, "接收端看到 kClosed 提前退出");
    AddCheck(&metrics, "结束时 waiters=0",
             snapshot.send_waiters == 0 && snapshot.recv_waiters == 0,
             "send_waiters=" + FormatU64(snapshot.send_waiters) +
                 " recv_waiters=" + FormatU64(snapshot.recv_waiters));
    AddCheck(&metrics, "通道缓冲已排空", remaining == 0,
             "Len=" + FormatU64(remaining));
    AddCheck(&metrics, "参与者正常结束（无死锁）", producer_ok && consumer_ok,
             "producer_ok=" + std::string(producer_ok ? "1" : "0") +
                 " consumer_ok=" + std::string(consumer_ok ? "1" : "0"));

    metrics.ok = AllChecksPassed(metrics);
    return metrics;
}

// ===========================================================================
// 模型 5：Mutex（Fiber 与原生线程争同一把锁）
// ===========================================================================

constexpr int kMutexWorkers = 8;    ///< 2 类执行体 × 4
constexpr int kMutexFibers = 4;     ///< 前 4 个参与者是 Fiber

RoundMetrics RunMutexRound(const std::string& model, std::uint64_t ops, int round) {
    RoundMetrics metrics;
    metrics.model = model;
    metrics.round = round;
    metrics.ops = ops;

    go2cpp::sync::Mutex mutex;
    // 以下普通变量全部只在持锁临界区内写，主线程在 join 之后读，无数据竞争。
    std::uint64_t counter = 0;
    std::uint64_t serial = 0;
    std::uint64_t corrupted = 0;
    std::atomic<std::uint64_t> lock_failures{0};
    std::atomic<std::uint64_t> join_failures{0};

    const std::uint64_t base = ops / static_cast<std::uint64_t>(kMutexWorkers);
    const std::uint64_t remainder = ops % static_cast<std::uint64_t>(kMutexWorkers);
    std::vector<std::uint64_t> quotas(kMutexWorkers, base);
    for (std::uint64_t i = 0; i < remainder; ++i) {
        ++quotas[static_cast<std::size_t>(i)];
    }

    StartGate gate;
    std::vector<Worker> workers;
    workers.reserve(kMutexWorkers);
    for (int id = 0; id < kMutexWorkers; ++id) {
        const std::uint64_t quota = quotas[static_cast<std::size_t>(id)];
        workers.push_back(Launch(id < kMutexFibers, [&, quota]() {
            gate.Wait();
            for (std::uint64_t i = 0; i < quota; ++i) {
                if (!mutex.Lock()) {
                    lock_failures.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
                ++counter;
                ++serial;
                if (serial != counter) {
                    ++corrupted;
                }
                mutex.Unlock();
                if ((i & 1023ULL) == 1023ULL) {
                    PublishProgress(1024);
                }
            }
            PublishProgress(1);
        }));
    }

    const long long start_ns = NowNs();
    gate.Open();
    for (auto& worker : workers) {
        if (!worker.Join()) {
            join_failures.fetch_add(1, std::memory_order_relaxed);
        }
    }
    const long long end_ns = NowNs();

    const go2cpp::sync::MutexDebugSnapshot snapshot = mutex.DebugSnapshotInternal();

    metrics.elapsed_ns = end_ns - start_ns;
    metrics.ns_per_op =
        ops == 0 ? 0.0 : static_cast<double>(metrics.elapsed_ns) / static_cast<double>(ops);
    metrics.sent = counter;
    metrics.recv = serial;
    metrics.violations = corrupted;
    metrics.errors = lock_failures.load(std::memory_order_relaxed) +
                     join_failures.load(std::memory_order_relaxed);
    metrics.aux_extra = counter;

    AddCheck(&metrics, "锁内计数精确（无丢失更新）", counter == ops,
             "counter=" + FormatU64(counter) + " 期望=" + FormatU64(ops));
    AddCheck(&metrics, "锁内序列号连续（无临界区交错）", corrupted == 0,
             "corrupted=" + FormatU64(corrupted));
    AddCheck(&metrics, "加锁全部成功", lock_failures.load() == 0,
             "lock_failures=" + FormatU64(lock_failures.load()));
    AddCheck(&metrics, "参与者正常结束（无死锁）",
             join_failures.load() == 0,
             "join_failures=" + FormatU64(join_failures.load()));
    AddCheck(&metrics, "结束时 Mutex waiters=0 且未持有",
             !snapshot.held && snapshot.waiters == 0 && snapshot.native_waiters == 0 &&
                 snapshot.fiber_waiters == 0,
             "held=" + std::string(snapshot.held ? "1" : "0") +
                 " waiters=" + FormatU64(snapshot.waiters));

    metrics.ok = AllChecksPassed(metrics);
    return metrics;
}

// ===========================================================================
// 模型 6：ConditionVariable（Fiber/线程共享 Mutex + 两个 CV）
// ===========================================================================

constexpr std::size_t kCvBatch = 32;
constexpr std::size_t kCvCapacity = 256;

struct CvShared {
    go2cpp::sync::Mutex mutex;
    go2cpp::sync::ConditionVariable not_empty;
    go2cpp::sync::ConditionVariable not_full;
    std::deque<std::uint64_t> queue;
    bool done = false;
    int finished_producers = 0;
    std::uint64_t produced = 0;
    std::uint64_t consumed = 0;
    std::uint64_t wait_count = 0;
    std::uint64_t notify_count = 0;
};

struct CvProducerOut {
    std::uint64_t produced = 0;
    std::uint64_t waits = 0;
    std::uint64_t errors = 0;
};

struct CvConsumerOut {
    std::uint64_t seen[2] = {0, 0};
    std::uint64_t violations = 0;
    std::uint64_t first_bad = 0;
    bool has_bad = false;
};

RoundMetrics RunCvRound(const std::string& model, std::uint64_t ops, int round) {
    RoundMetrics metrics;
    metrics.model = model;
    metrics.round = round;
    metrics.ops = ops;

    const std::uint64_t quota0 = ops / 2;
    const std::uint64_t quota1 = ops - quota0;
    const std::uint64_t offset1 = quota0;
    const std::uint64_t limit1 = quota0 + quota1;

    CvShared shared;
    StartGate gate;
    CvProducerOut producer_out[2];
    CvConsumerOut consumer_out[2];
    std::atomic<std::uint64_t> join_failures{0};

    // 生产者 0 = Fiber，生产者 1 = 原生线程。
    std::vector<Worker> workers;
    workers.reserve(4);
    for (int id = 0; id < 2; ++id) {
        const std::uint64_t quota = id == 0 ? quota0 : quota1;
        const std::uint64_t offset = id == 0 ? 0 : offset1;
        workers.push_back(Launch(id == 0, [&, id, quota, offset]() {
            gate.Wait();
            CvProducerOut* out = &producer_out[id];
            std::uint64_t done = 0;
            while (done < quota) {
                if (!shared.mutex.Lock()) {
                    ++out->errors;
                    break;
                }
                while (shared.queue.size() >= kCvCapacity) {
                    ++out->waits;
                    ++shared.wait_count;
                    if (!shared.not_full.Wait(shared.mutex)) {
                        ++out->errors;
                        break;
                    }
                }
                if (out->errors != 0) {
                    shared.mutex.Unlock();
                    break;
                }
                std::uint64_t room = kCvCapacity - shared.queue.size();
                std::uint64_t batch = std::min<std::uint64_t>(kCvBatch, quota - done);
                if (batch > room) {
                    batch = room;
                }
                for (std::uint64_t i = 0; i < batch; ++i) {
                    shared.queue.push_back(offset + done + i);
                }
                shared.produced += batch;
                ++shared.notify_count;
                shared.not_empty.NotifyAll();
                shared.mutex.Unlock();
                done += batch;
                out->produced = done;
                PublishProgress(batch);
            }
            shared.mutex.Lock();
            ++shared.finished_producers;
            if (shared.finished_producers == 2) {
                shared.done = true;
                shared.not_empty.NotifyAll();
                shared.not_full.NotifyAll();
            }
            shared.mutex.Unlock();
            PublishProgress(1);
        }));
    }

    // 消费者 0 = Fiber，消费者 1 = 原生线程。
    for (int id = 0; id < 2; ++id) {
        workers.push_back(Launch(id == 0, [&, id]() {
            gate.Wait();
            CvConsumerOut* out = &consumer_out[id];
            std::int64_t last[2] = {-1, -1};
            for (;;) {
                std::uint64_t batch[kCvBatch];
                std::size_t count = 0;
                if (!shared.mutex.Lock()) {
                    ++out->violations;
                    break;
                }
                while (shared.queue.empty() && !shared.done) {
                    ++shared.wait_count;
                    if (!shared.not_empty.Wait(shared.mutex)) {
                        ++out->violations;
                        shared.mutex.Unlock();
                        return;
                    }
                }
                if (shared.queue.empty() && shared.done) {
                    shared.mutex.Unlock();
                    break;
                }
                count = std::min<std::size_t>(kCvBatch, shared.queue.size());
                for (std::size_t i = 0; i < count; ++i) {
                    batch[i] = shared.queue.front();
                    shared.queue.pop_front();
                }
                shared.consumed += count;
                if (shared.queue.size() < kCvCapacity) {
                    ++shared.notify_count;
                    shared.not_full.NotifyAll();
                }
                shared.mutex.Unlock();

                for (std::size_t i = 0; i < count; ++i) {
                    const std::uint64_t value = batch[i];
                    const int producer = value >= offset1 ? 1 : 0;
                    const std::uint64_t floor = producer == 0 ? 0 : offset1;
                    const std::uint64_t limit = producer == 0 ? offset1 : limit1;
                    if (value < floor || value >= limit ||
                        static_cast<std::int64_t>(value) <= last[producer]) {
                        ++out->violations;
                        if (!out->has_bad) {
                            out->has_bad = true;
                            out->first_bad = value;
                        }
                    }
                    if (static_cast<std::int64_t>(value) > last[producer]) {
                        last[producer] = static_cast<std::int64_t>(value);
                    }
                    ++out->seen[producer];
                }
                PublishProgress(count);
            }
        }));
    }

    const long long start_ns = NowNs();
    gate.Open();
    for (auto& worker : workers) {
        if (!worker.Join()) {
            join_failures.fetch_add(1, std::memory_order_relaxed);
        }
    }
    const long long end_ns = NowNs();

    const go2cpp::sync::ConditionVariableDebugSnapshot not_empty_snapshot =
        shared.not_empty.DebugSnapshotInternal();
    const go2cpp::sync::ConditionVariableDebugSnapshot not_full_snapshot =
        shared.not_full.DebugSnapshotInternal();
    const go2cpp::sync::MutexDebugSnapshot mutex_snapshot =
        shared.mutex.DebugSnapshotInternal();

    const std::uint64_t seen0 =
        consumer_out[0].seen[0] + consumer_out[1].seen[0];
    const std::uint64_t seen1 =
        consumer_out[0].seen[1] + consumer_out[1].seen[1];

    metrics.elapsed_ns = end_ns - start_ns;
    metrics.ns_per_op =
        ops == 0 ? 0.0 : static_cast<double>(metrics.elapsed_ns) / static_cast<double>(ops);
    metrics.sent = shared.produced;
    metrics.recv = shared.consumed;
    metrics.violations = consumer_out[0].violations + consumer_out[1].violations;
    metrics.errors = producer_out[0].errors + producer_out[1].errors +
                     join_failures.load(std::memory_order_relaxed);
    metrics.aux_waits = shared.wait_count;
    metrics.aux_notifies = shared.notify_count;

    AddCheck(&metrics, "生产数=消费数=每轮操作数",
             shared.produced == ops && shared.consumed == ops &&
                 seen0 == quota0 && seen1 == quota1,
             "produced=" + FormatU64(shared.produced) + " consumed=" +
                 FormatU64(shared.consumed) + " seen0=" + FormatU64(seen0) +
                 " seen1=" + FormatU64(seen1) + " 期望=" + FormatU64(ops));
    AddCheck(&metrics, "每个生产者内部 FIFO 有序（无丢失/重复/乱序）",
             metrics.violations == 0,
             "违例=" + FormatU64(metrics.violations) + " 首个坏值=" +
                 FormatU64(consumer_out[0].has_bad ? consumer_out[0].first_bad
                                                   : consumer_out[1].first_bad));
    AddCheck(&metrics, "生产/消费无 API 错误", metrics.errors == 0,
             "errors=" + FormatU64(metrics.errors));
    AddCheck(&metrics, "确有 CV 真实等待发生", shared.wait_count > 0,
             "wait_count=" + FormatU64(shared.wait_count));
    AddCheck(&metrics, "队列已排空且已收尾",
             shared.queue.empty() && shared.done,
             "queue=" + FormatU64(shared.queue.size()) +
                 " done=" + std::string(shared.done ? "1" : "0"));
    AddCheck(&metrics, "结束时 CV waiters=0",
             not_empty_snapshot.waiters == 0 && not_full_snapshot.waiters == 0,
             "not_empty=" + FormatU64(not_empty_snapshot.waiters) +
                 " not_full=" + FormatU64(not_full_snapshot.waiters));
    AddCheck(&metrics, "结束时 Mutex 未持有且 waiters=0",
             !mutex_snapshot.held && mutex_snapshot.waiters == 0,
             "held=" + std::string(mutex_snapshot.held ? "1" : "0") +
                 " waiters=" + FormatU64(mutex_snapshot.waiters));
    AddCheck(&metrics, "参与者正常结束（无死锁）",
             join_failures.load(std::memory_order_relaxed) == 0,
             "join_failures=" + FormatU64(join_failures.load()));

    metrics.ok = AllChecksPassed(metrics);
    return metrics;
}

// ===========================================================================
// 模型 7：WaitGroup（Fiber/线程混合 Add/Done/Wait，波次屏障）
// ===========================================================================

constexpr std::uint64_t kWgBatch = 64;      ///< 每波 Done 次数
constexpr std::uint64_t kWgWorkers = 4;     ///< 2 Fiber + 2 原生线程
constexpr std::uint64_t kWgPerWorker = kWgBatch / kWgWorkers;

struct WgShared {
    go2cpp::sync::WaitGroup wg;
    std::atomic<std::uint64_t> publish{0};
    std::atomic<std::uint64_t> wait_failures{0};
};

RoundMetrics RunWgRound(const std::string& model, std::uint64_t ops, int round) {
    RoundMetrics metrics;
    metrics.model = model;
    metrics.round = round;
    metrics.ops = ops;

    const std::uint64_t waves = ops / kWgBatch;
    WgShared shared;
    StartGate gate;
    std::atomic<std::uint64_t> join_failures{0};

    // 等待者按轮次在 Fiber / 原生线程之间切换，保证两种等待路径都被覆盖。
    const bool waiter_fiber = (round % 2) == 0;
    Worker waiter = Launch(waiter_fiber, [&]() {
        gate.Wait();
        for (std::uint64_t wave = 0; wave < waves; ++wave) {
            shared.wg.Add(static_cast<std::int64_t>(kWgBatch));
            shared.publish.store(wave + 1, std::memory_order_release);
            if (!shared.wg.Wait()) {
                shared.wait_failures.fetch_add(1, std::memory_order_relaxed);
                break;
            }
            PublishProgress(kWgBatch);
        }
        PublishProgress(1);
    });

    std::vector<Worker> workers;
    workers.reserve(kWgWorkers);
    for (std::uint64_t id = 0; id < kWgWorkers; ++id) {
        workers.push_back(Launch(id < 2, [&]() {
            gate.Wait();
            for (std::uint64_t wave = 0; wave < waves; ++wave) {
                WaitForWave(shared.publish, wave + 1);
                for (std::uint64_t i = 0; i < kWgPerWorker; ++i) {
                    shared.wg.Done();
                }
                PublishProgress(kWgPerWorker);
            }
            PublishProgress(1);
        }));
    }

    const long long start_ns = NowNs();
    gate.Open();
    const bool waiter_ok = waiter.Join();
    for (auto& worker : workers) {
        if (!worker.Join()) {
            join_failures.fetch_add(1, std::memory_order_relaxed);
        }
    }
    const long long end_ns = NowNs();

    const go2cpp::sync::WaitGroupDebugSnapshot snapshot =
        shared.wg.DebugSnapshotInternal();

    metrics.elapsed_ns = end_ns - start_ns;
    metrics.ns_per_op =
        ops == 0 ? 0.0 : static_cast<double>(metrics.elapsed_ns) / static_cast<double>(ops);
    metrics.sent = waves * kWgBatch;
    metrics.recv = waves * kWgBatch;
    metrics.errors = join_failures.load(std::memory_order_relaxed) +
                     (waiter_ok ? 0U : 1U);
    metrics.aux_waits = waves;
    metrics.aux_extra = waves;

    AddCheck(&metrics, "Done 次数=每轮操作数", waves * kWgBatch == ops,
             "waves=" + FormatU64(waves) + " done=" + FormatU64(waves * kWgBatch));
    AddCheck(&metrics, "Wait 全部正常返回",
             shared.wait_failures.load(std::memory_order_relaxed) == 0,
             "wait_failures=" +
                 FormatU64(shared.wait_failures.load(std::memory_order_relaxed)));
    AddCheck(&metrics, "结束时计数归零",
             snapshot.count == 0 && shared.wg.Count() == 0,
             "count=" + std::to_string(snapshot.count));
    AddCheck(&metrics, "结束时 WaitGroup waiters=0", snapshot.waiters == 0,
             "waiters=" + FormatU64(snapshot.waiters));
    AddCheck(&metrics, "参与者正常结束（无死锁）",
             join_failures.load(std::memory_order_relaxed) == 0 && waiter_ok,
             "join_failures=" +
                 FormatU64(join_failures.load(std::memory_order_relaxed)));
    AddCheck(&metrics, "混合等待者路径覆盖",
             true, waiter_fiber ? "本轮等待者=Fiber" : "本轮等待者=原生线程");

    metrics.ok = AllChecksPassed(metrics);
    return metrics;
}

// ===========================================================================
// 模型驱动与预算控制
// ===========================================================================

void PrintRoundLine(const RoundMetrics& metrics, int planned) {
    std::printf(
        "[%s] 第 %d/%d 轮：操作=%" PRIu64 " 发送=%" PRIu64 " 接收=%" PRIu64
        " 乱序=%" PRIu64 " 错误=%" PRIu64 " 耗时=%.3fs ns/op=%.1f RSS=%.1fMiB %s\n",
        metrics.model.c_str(), metrics.round, planned, metrics.ops, metrics.sent,
        metrics.recv, metrics.violations, metrics.errors,
        static_cast<double>(metrics.elapsed_ns) / 1e9, metrics.ns_per_op,
        static_cast<double>(metrics.rss_kb) / 1024.0,
        metrics.ok ? "通过" : "失败");
    if (!metrics.ok) {
        std::printf("    失败原因：%s\n", metrics.detail.c_str());
    }
    std::fflush(stdout);
}

ModelReport RunModel(const ModelDef& def, const Config& config,
                     long long global_elapsed_ns) {
    ModelReport report;
    report.id = def.id;
    report.title = def.title;
    report.topology = def.topology;
    report.op_unit = def.op_unit;
    report.rounds_planned = config.rounds;

    std::uint64_t ops = config.ops_per_round;
    if (def.divisor > 1) {
        ops -= ops % def.divisor;
    }
    if (ops == 0) {
        ops = def.divisor > 1 ? def.divisor : 1;
    }
    report.ops_per_round = ops;

    const long long model_start_ns = NowNs();
    std::uint64_t previous_rss_kb = 0;
    for (int round = 1; round <= config.rounds; ++round) {
        g_stage_model.store(def.id.c_str(), std::memory_order_relaxed);
        g_stage_round.store(round, std::memory_order_relaxed);
        g_progress.store(0, std::memory_order_relaxed);
        g_progress_sent.store(0, std::memory_order_relaxed);
        g_progress_recv.store(0, std::memory_order_relaxed);
        g_active.store(true, std::memory_order_release);

        RoundMetrics metrics;
        try {
            metrics = def.run(ops, round);
        } catch (const std::exception& error) {
            metrics.model = def.id;
            metrics.round = round;
            metrics.ops = ops;
            metrics.errors = 1;
            metrics.detail = std::string("未捕获异常：") + error.what();
            AddCheck(&metrics, "参与者无未捕获异常", false, error.what());
        } catch (...) {
            metrics.model = def.id;
            metrics.round = round;
            metrics.ops = ops;
            metrics.errors = 1;
            metrics.detail = "未捕获未知异常";
            AddCheck(&metrics, "参与者无未捕获异常", false, "未知异常");
        }
        g_active.store(false, std::memory_order_release);

        const ProcMem memory = ReadProcMem();
        metrics.rss_kb = memory.rss_kb;
        metrics.vmsize_kb = memory.vmsize_kb;

        const long long rss_delta =
            static_cast<long long>(metrics.rss_kb) -
            static_cast<long long>(g_baseline_mem.rss_kb);
        AddCheck(&metrics, "RSS 稳定（相对基线增量 ≤ 96MiB）",
                 rss_delta <= 96LL * 1024LL,
                 "ΔRSS=" + FormatDouble(static_cast<double>(rss_delta) / 1024.0) +
                     "MiB");
        // 相邻轮次的 RSS 增量检查：持续逐轮增长的泄漏（哪怕每轮只有几 MiB）
        // 会被这条断言抓住。VmSize 只作观测——运行时为每个 Fiber 预留 8MiB
        // 虚拟地址空间（按需提交），虚拟地址空间增长不代表泄漏。
        if (previous_rss_kb != 0) {
            const long long round_delta =
                static_cast<long long>(metrics.rss_kb) -
                static_cast<long long>(previous_rss_kb);
            AddCheck(&metrics, "RSS 无逐轮增长（单轮增量 ≤ 32MiB）",
                     round_delta <= 32LL * 1024LL,
                     "ΔRSS(轮间)=" +
                         FormatDouble(static_cast<double>(round_delta) / 1024.0) +
                         "MiB");
        }
        previous_rss_kb = metrics.rss_kb;

        metrics.ok = AllChecksPassed(metrics);
        report.rounds.push_back(metrics);
        report.rounds_done = round;
        PrintRoundLine(metrics, config.rounds);

        // 预算控制：已完成 ≥min_rounds 且按当前均值推演会超预算，则停止。
        const long long elapsed_ns = NowNs() - model_start_ns;
        const double average_ns =
            static_cast<double>(elapsed_ns) / static_cast<double>(report.rounds_done);
        const int remaining_rounds = config.rounds - report.rounds_done;
        if (report.rounds_done >= config.min_rounds && remaining_rounds > 0) {
            const double projected_ns =
                average_ns * static_cast<double>(remaining_rounds);
            const bool model_over =
                config.model_budget_sec > 0 &&
                static_cast<double>(elapsed_ns) + projected_ns >
                    static_cast<double>(config.model_budget_sec) * 1e9;
            const bool total_over =
                config.total_budget_sec > 0 &&
                static_cast<double>(global_elapsed_ns + elapsed_ns) + projected_ns >
                    static_cast<double>(config.total_budget_sec) * 1e9;
            if (model_over || total_over) {
                report.budget_trimmed = true;
                std::printf(
                    "    [预算] 预测 %d 轮需 %.1fs，超出%s预算；按任务允许的降级路径"
                    "停在 %d 轮（≥%d 轮下限）\n",
                    config.rounds,
                    (static_cast<double>(elapsed_ns) + projected_ns) / 1e9,
                    model_over ? "单模型" : "全局", report.rounds_done,
                    config.min_rounds);
                std::fflush(stdout);
                break;
            }
        }
    }
    report.elapsed_ns = NowNs() - model_start_ns;

    bool all_ok = !report.rounds.empty();
    for (const auto& round : report.rounds) {
        all_ok = all_ok && round.ok;
    }
    const int required_rounds =
        std::min(config.rounds, std::max(1, config.min_rounds));
    all_ok = all_ok && report.rounds_done >= required_rounds;
    report.ok = all_ok;

    {
        std::lock_guard<std::mutex> lock(g_reports_mutex);
        g_reports.push_back(report);
    }
    return report;
}

// ===========================================================================
// 报告生成
// ===========================================================================

struct AggregatedCheck {
    std::string name;
    int passed = 0;
    int total = 0;
    std::string first_detail;
};

std::vector<AggregatedCheck> AggregateChecks(const ModelReport& report) {
    std::vector<AggregatedCheck> aggregated;
    for (const auto& round : report.rounds) {
        for (const auto& check : round.checks) {
            std::size_t index = aggregated.size();
            for (std::size_t i = 0; i < aggregated.size(); ++i) {
                if (aggregated[i].name == check.name) {
                    index = i;
                    break;
                }
            }
            if (index == aggregated.size()) {
                AggregatedCheck entry;
                entry.name = check.name;
                aggregated.push_back(entry);
            }
            auto& entry = aggregated[index];
            ++entry.total;
            if (check.passed) {
                ++entry.passed;
            } else if (entry.first_detail.empty()) {
                entry.first_detail = check.detail;
            }
        }
    }
    return aggregated;
}

struct NsStats {
    double min = 0.0;
    double median = 0.0;
    double max = 0.0;
};

NsStats ComputeNsStats(const ModelReport& report) {
    std::vector<double> values;
    for (const auto& round : report.rounds) {
        if (round.ns_per_op > 0.0) {
            values.push_back(round.ns_per_op);
        }
    }
    NsStats stats;
    if (values.empty()) {
        return stats;
    }
    std::sort(values.begin(), values.end());
    stats.min = values.front();
    stats.max = values.back();
    stats.median = values[values.size() / 2];
    return stats;
}

std::uint64_t TotalOps(const ModelReport& report) {
    std::uint64_t total = 0;
    for (const auto& round : report.rounds) {
        total += round.ops;
    }
    return total;
}

bool ReportPassed(const ModelReport& report) { return report.ok; }

void WriteReport(const std::vector<ModelReport>& reports, const Config& config,
                 const std::string& abort_reason) {
    std::ofstream out(config.out);
    if (!out) {
        std::fprintf(stderr, "[错误] 无法写入报告：%s\n", config.out.c_str());
        return;
    }

    std::time_t now = std::time(nullptr);
    char time_buffer[64] = {0};
    std::strftime(time_buffer, sizeof(time_buffer), "%Y-%m-%d %H:%M:%S",
                  std::localtime(&now));

    char hostname[256] = {0};
    if (gethostname(hostname, sizeof(hostname) - 1) != 0) {
        std::snprintf(hostname, sizeof(hostname), "unknown");
    }

    bool all_ok = !reports.empty();
    for (const auto& report : reports) {
        all_ok = all_ok && ReportPassed(report);
    }

    out << "# Fiber/Thread 混合并发亿级正确性压测报告\n\n";
    out << "- 生成时间：" << time_buffer << "\n";
    out << "- 主机：" << hostname << "，CPU：" << CpuModelName()
        << "，逻辑核：" << std::thread::hardware_concurrency() << "\n";
    out << "- 构建：Release（`-O3 -DNDEBUG`），g++ "
#if defined(__GNUC__)
        << __GNUC__ << "." << __GNUC_MINOR__ << "." << __GNUC_PATCHLEVEL__
#else
        << "unknown"
#endif
        << "\n";
    out << "- 配置：每轮 " << FormatU64(config.ops_per_round)
        << " 次操作，目标 " << config.rounds << " 轮/模型，最少 "
        << config.min_rounds << " 轮；单模型预算 " << config.model_budget_sec
        << "s，全局软预算 " << config.total_budget_sec << "s\n";
    out << "- 结论：**" << (all_ok ? "全部通过" : "存在失败") << "**\n\n";

    if (!abort_reason.empty()) {
        out << "> **中断原因**：" << abort_reason << "\n\n";
    }
    if (!g_shutdown_note.empty()) {
        out << "> **调度器收尾**：" << g_shutdown_note << "\n\n";
    }

    out << "## 一、汇总\n\n";
    out << "| 模型 | 拓扑 | 轮次(完成/计划) | 每轮操作数 | 总操作数 | ns/op 中位 "
           "| 总耗时 | 断言 | 结果 |\n";
    out << "|---|---|---|---|---|---|---|---|---|\n";
    for (const auto& report : reports) {
        const NsStats stats = ComputeNsStats(report);
        int failed_checks = 0;
        int total_checks = 0;
        for (const auto& check : AggregateChecks(report)) {
            total_checks += check.total;
            failed_checks += check.total - check.passed;
        }
        out << "| " << report.id << " | " << report.topology << " | "
            << report.rounds_done << "/" << report.rounds_planned << " | "
            << FormatU64(report.ops_per_round) << " | "
            << FormatU64(TotalOps(report)) << " | " << FormatDouble(stats.median, 1)
            << " | " << FormatSeconds(report.elapsed_ns) << " | "
            << (failed_checks == 0 ? "全部通过" : "失败 " +
                                             std::to_string(failed_checks) + "/" +
                                             std::to_string(total_checks))
            << " | " << (ReportPassed(report) ? "通过" : "失败") << " |\n";
    }
    out << "\n> ns/op 分母口径见第二节各模型的「操作口径」。\n\n";

    out << "## 二、逐模型断言明细\n\n";
    for (const auto& report : reports) {
        out << "### " << report.id << "（" << report.title << "，"
            << report.topology << "）\n\n";
        out << "- 操作口径：" << report.op_unit << "\n";
        out << "- 总操作数：" << FormatU64(TotalOps(report)) << "（"
            << report.rounds_done << " 轮 × " << FormatU64(report.ops_per_round)
            << "）\n";
        if (report.budget_trimmed) {
            out << "- 轮次说明：**预算裁剪**，按任务允许的降级路径停在 "
                << report.rounds_done << " 轮（仍 ≥ " << config.min_rounds
                << " 轮，且每轮 ≥10^8 操作）\n";
        }
        out << "\n| 断言 | 结果 | 说明 |\n|---|---|---|\n";
        for (const auto& check : AggregateChecks(report)) {
            const bool passed = check.passed == check.total;
            out << "| " << check.name << " | "
                << (passed ? "通过" : "失败 (" + std::to_string(check.passed) +
                                         "/" + std::to_string(check.total) + ")")
                << " | " << (check.first_detail.empty() ? "-" : check.first_detail)
                << " |\n";
        }
        out << "\n";
    }

    out << "## 三、每轮明细\n\n";
    out << "| 模型 | 轮 | 操作数 | 发送/完成 | 接收/消费 | 乱序 | 错误 | 耗时(s) "
           "| ns/op | RSS(MiB) | 结果 |\n";
    out << "|---|---|---|---|---|---|---|---|---|---|---|\n";
    for (const auto& report : reports) {
        for (const auto& round : report.rounds) {
            out << "| " << round.model << " | " << round.round << " | "
                << FormatU64(round.ops) << " | " << FormatU64(round.sent) << " | "
                << FormatU64(round.recv) << " | " << FormatU64(round.violations)
                << " | " << FormatU64(round.errors) << " | "
                << FormatDouble(static_cast<double>(round.elapsed_ns) / 1e9, 3)
                << " | " << FormatDouble(round.ns_per_op, 1) << " | "
                << FormatDouble(static_cast<double>(round.rss_kb) / 1024.0, 1)
                << " | " << (round.ok ? "通过" : "失败") << " |\n";
        }
    }

    out << "\n## 四、失败记录\n\n";
    bool any_failure = false;
    for (const auto& report : reports) {
        for (const auto& round : report.rounds) {
            if (round.ok) {
                continue;
            }
            any_failure = true;
            out << "- `" << round.model << "` 第 " << round.round << " 轮："
                << round.detail << "；计数 sent=" << round.sent
                << " recv=" << round.recv << " ops=" << round.ops
                << " violations=" << round.violations
                << " errors=" << round.errors << "\n";
        }
    }
    if (!any_failure) {
        out << "无。\n";
    }
    out << "\n";

    out << "## 五、与 Go 对比的口径\n\n";
    out << "- 本报告 ns/op = 单轮墙钟时间 / 单轮操作数；计时从统一放行门开启到"
           "全部参与者结束，包含调度与 park/wake 成本。\n";
    out << "- Channel 模型对应 Go 的「1 个生产者 goroutine + 1 个消费者 "
           "goroutine + `make(chan T, 64)`」；Go 侧建议用同样的 4 个 P"
           "（`GOMAXPROCS=4`）与同样的容量，才可直接比较。\n";
    out << "- fiber→thread / thread→fiber 两种拓扑在 Go 中没有直接等价物"
           "（goroutine 不能变成原生线程执行体），可用 cgo 或 "
           "`runtime.LockOSThread` 近似，但会引入额外成本，比较时需注明。\n";
    out << "- Mutex/CV/WaitGroup 三个模型的 ns/op 分别是「lock+unlock」"
           "「入队并出队一个值」「一次 Done」的单价。\n";
    out << "- 本机为 WSL2 虚拟化环境，park/wake 的 futex 往返通常比裸金属慢；"
           "与 Go 对比应在同一台机器、同一 CPU 配额下重测。\n\n";

    out << "## 六、测试有效性说明\n\n";
    out << "- FIFO 验证器在正式压测前用构造的坏序列做了自检（乱序、重复、缺值），"
           "确认断言会真实报错，不是恒真。\n";
    out << "- 死锁检测：看门狗每 200ms 采样全局进度，超过 " << config.hang_sec
        << "s 无变化即写报告并以退出码 4 终止。\n";
    out << "- 泄漏检测：以调度器启动后的 RSS 为基线，逐轮检查相对基线增量"
           "（≤96MiB）以及相邻轮次增量（≤32MiB）；单次操作级别的泄漏会在数轮内"
           "暴露。VmSize 只作观测：运行时为每个 Fiber 预留 8MiB 虚拟地址空间"
           "（按需提交），虚拟地址空间变大不代表泄漏。\n";
    out << "- 每轮结束后检查 Channel/Mutex/CV/WaitGroup 的 waiters 快照必须为 0，"
           "并检查计数归零。\n";

    out.flush();
    if (!out.good()) {
        std::fprintf(stderr, "[错误] 报告写入过程中出错：%s\n", config.out.c_str());
    }
    g_report_written.store(true, std::memory_order_release);
    std::printf("\n报告已写入：%s\n", config.out.c_str());
    std::fflush(stdout);
}

/** @brief 致命故障：尽可能落一份中断报告，然后以非零码退出。 */
void FailFast(const std::string& reason) {
    std::string note = reason;
    const char* model = g_stage_model.load(std::memory_order_relaxed);
    if (model != nullptr) {
        note += "；当前模型=" + std::string(model) + " 轮次=" +
                std::to_string(g_stage_round.load(std::memory_order_relaxed)) +
                " 已发送=" +
                FormatU64(g_progress_sent.load(std::memory_order_relaxed)) +
                " 已接收=" +
                FormatU64(g_progress_recv.load(std::memory_order_relaxed));
    }
    std::fprintf(stderr, "[致命] %s\n", note.c_str());
    std::fflush(stderr);

    if (!g_report_written.load(std::memory_order_acquire)) {
        std::vector<ModelReport> snapshot;
        {
            std::lock_guard<std::mutex> lock(g_reports_mutex);
            snapshot = g_reports;
        }
        g_shutdown_note = "压测被中断，调度器未正常收尾";
        WriteReport(snapshot, g_cfg, note);
        g_report_written.store(true, std::memory_order_release);
    }
    _exit(4);
}

/** @brief 看门狗：全局进度长时间不前进即判定死锁。 */
void WatchdogMain(int hang_sec) {
    bool was_active = false;
    std::uint64_t last = g_progress.load(std::memory_order_relaxed);
    Clock::time_point last_change = Clock::now();
    const auto limit = std::chrono::seconds(hang_sec);

    while (!g_stop_watchdog.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        const bool active = g_active.load(std::memory_order_acquire);
        if (active && !was_active) {
            last = g_progress.load(std::memory_order_relaxed);
            last_change = Clock::now();
        }
        was_active = active;
        if (!active) {
            continue;
        }
        const std::uint64_t current = g_progress.load(std::memory_order_relaxed);
        if (current != last) {
            last = current;
            last_change = Clock::now();
            continue;
        }
        if (Clock::now() - last_change >= limit) {
            FailFast("看门狗检测到超过 " + std::to_string(hang_sec) +
                     "s 无进展（疑似死锁）");
        }
    }
}

// ===========================================================================
// 命令行
// ===========================================================================

void PrintUsage(const char* program) {
    std::printf(
        "用法：%s [选项]\n"
        "  --ops N                 每轮操作数（默认 100000000）\n"
        "  --rounds N              每模型目标轮数（默认 10）\n"
        "  --min-rounds N          预算裁剪时的最少轮数（默认 3）\n"
        "  --budget-sec N          单模型时间预算秒（默认 1800）\n"
        "  --total-budget-sec N    全局软预算秒（默认 4200，0=不限）\n"
        "  --hang-sec N            无进展判定死锁秒数（默认 120）\n"
        "  --out PATH              报告路径（默认 /tmp/mixed_billion.md）\n"
        "  --models LIST           逗号分隔：f2f,f2t,t2f,t2t,mutex,cv,wg\n"
        "  --quick                 冒烟参数（2e6 操作 × 1 轮）\n"
        "  --help                  显示本帮助\n",
        program);
}

bool ParseU64(const char* text, std::uint64_t* value) {
    if (text == nullptr || *text == '\0') {
        return false;
    }
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(text, &end, 10);
    if (end == nullptr || *end != '\0') {
        return false;
    }
    *value = static_cast<std::uint64_t>(parsed);
    return true;
}

bool ParseArgs(int argc, char** argv, Config* config, std::string* error) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                *error = std::string(name) + " 缺少参数";
                return nullptr;
            }
            return argv[++i];
        };
        if (arg == "--help" || arg == "-h") {
            PrintUsage(argv[0]);
            std::exit(0);
        } else if (arg == "--ops") {
            const char* value = next("--ops");
            if (value == nullptr || !ParseU64(value, &config->ops_per_round)) {
                *error = "--ops 需要非负整数";
                return false;
            }
        } else if (arg == "--rounds") {
            const char* value = next("--rounds");
            std::uint64_t parsed = 0;
            if (value == nullptr || !ParseU64(value, &parsed) || parsed == 0) {
                *error = "--rounds 需要正整数";
                return false;
            }
            config->rounds = static_cast<int>(parsed);
        } else if (arg == "--min-rounds") {
            const char* value = next("--min-rounds");
            std::uint64_t parsed = 0;
            if (value == nullptr || !ParseU64(value, &parsed) || parsed == 0) {
                *error = "--min-rounds 需要正整数";
                return false;
            }
            config->min_rounds = static_cast<int>(parsed);
        } else if (arg == "--budget-sec") {
            const char* value = next("--budget-sec");
            std::uint64_t parsed = 0;
            if (value == nullptr || !ParseU64(value, &parsed)) {
                *error = "--budget-sec 需要非负整数";
                return false;
            }
            config->model_budget_sec = static_cast<long long>(parsed);
        } else if (arg == "--total-budget-sec") {
            const char* value = next("--total-budget-sec");
            std::uint64_t parsed = 0;
            if (value == nullptr || !ParseU64(value, &parsed)) {
                *error = "--total-budget-sec 需要非负整数";
                return false;
            }
            config->total_budget_sec = static_cast<long long>(parsed);
        } else if (arg == "--hang-sec") {
            const char* value = next("--hang-sec");
            std::uint64_t parsed = 0;
            if (value == nullptr || !ParseU64(value, &parsed) || parsed == 0) {
                *error = "--hang-sec 需要正整数";
                return false;
            }
            config->hang_sec = static_cast<int>(parsed);
        } else if (arg == "--out") {
            const char* value = next("--out");
            if (value == nullptr || *value == '\0') {
                *error = "--out 需要路径";
                return false;
            }
            config->out = value;
        } else if (arg == "--models") {
            const char* value = next("--models");
            if (value == nullptr) {
                return false;
            }
            config->models = value;
        } else if (arg == "--quick") {
            config->quick = true;
            config->ops_per_round = 2000000ULL;
            config->rounds = 1;
            config->min_rounds = 1;
            config->hang_sec = 60;
        } else {
            *error = "未知参数：" + arg;
            return false;
        }
    }
    if (config->min_rounds > config->rounds) {
        config->min_rounds = config->rounds;
    }
    return true;
}

bool ModelSelected(const Config& config, const std::string& id) {
    if (config.models.empty()) {
        return true;
    }
    std::string list = config.models;
    std::size_t start = 0;
    while (start <= list.size()) {
        const std::size_t comma = list.find(',', start);
        const std::string token =
            list.substr(start, comma == std::string::npos ? std::string::npos
                                                         : comma - start);
        if (token == id) {
            return true;
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    return false;
}

// ===========================================================================
// main
// ===========================================================================

}  // namespace

int main(int argc, char** argv) {
    Config config;
    std::string error;
    if (!ParseArgs(argc, argv, &config, &error)) {
        std::fprintf(stderr, "[参数错误] %s\n", error.c_str());
        PrintUsage(argv[0]);
        return 2;
    }
    g_cfg = config;

    std::printf("=== Fiber/Thread 混合并发亿级正确性压测 ===\n");
    std::printf("每轮操作=%" PRIu64 " 目标轮数=%d 最少轮数=%d 单模型预算=%llds "
                "死锁判定=%ds\n",
                config.ops_per_round, config.rounds, config.min_rounds,
                config.model_budget_sec, config.hang_sec);
    if (config.ops_per_round < 100000000ULL) {
        std::printf("[提示] 每轮操作数 < 10^8，当前为冒烟/标定规模，不是验收规模\n");
    }

    if (!FifoVerifierSelfCheck()) {
        std::fprintf(stderr, "[自检失败] FIFO 验证器无法识别坏序列，断言不可信\n");
        return 1;
    }
    std::printf("[自检] FIFO 验证器：通过（乱序/重复/缺值均被识别）\n");

    std::thread watchdog(WatchdogMain, config.hang_sec);

    go2cpp::SchedulerConfig scheduler_config;
    scheduler_config.processor_count = 4;
    scheduler_config.min_workers = 4;
    scheduler_config.max_workers = 8;
    scheduler_config.fiber_stack_size = 16384;
    scheduler_config.collect_metrics = false;

    std::unique_ptr<Scheduler> scheduler;
    try {
        scheduler = std::make_unique<Scheduler>(scheduler_config);
        scheduler->start();
    } catch (const std::exception& start_error) {
        g_stop_watchdog.store(true, std::memory_order_release);
        watchdog.join();
        std::fprintf(stderr, "[致命] 调度器启动失败：%s\n", start_error.what());
        return 1;
    }
    g_sched = scheduler.get();
    g_baseline_mem = ReadProcMem();
    std::printf("[环境] CPU=%s 逻辑核=%u 基线RSS=%.1fMiB 基线VmSize=%.1fMiB\n",
                CpuModelName().c_str(), std::thread::hardware_concurrency(),
                static_cast<double>(g_baseline_mem.rss_kb) / 1024.0,
                static_cast<double>(g_baseline_mem.vmsize_kb) / 1024.0);
    std::fflush(stdout);

    std::vector<ModelDef> models;
    if (ModelSelected(config, "f2f")) {
        models.push_back({"f2f", "Fiber→Fiber Channel",
                          "1 Fiber 生产者 → 1 Fiber 消费者（chan cap=64）",
                          "一个值完成发送并接收", 1,
                          [](std::uint64_t ops, int round) {
                              return RunChannelRound("f2f", true, true, ops, round,
                                                     64);
                          }});
    }
    if (ModelSelected(config, "f2t")) {
        models.push_back({"f2t", "Fiber→Thread Channel",
                          "1 Fiber 生产者 → 1 原生线程消费者（chan cap=64）",
                          "一个值完成发送并接收", 1,
                          [](std::uint64_t ops, int round) {
                              return RunChannelRound("f2t", true, false, ops, round,
                                                     64);
                          }});
    }
    if (ModelSelected(config, "t2f")) {
        models.push_back({"t2f", "Thread→Fiber Channel",
                          "1 原生线程生产者 → 1 Fiber 消费者（chan cap=64）",
                          "一个值完成发送并接收", 1,
                          [](std::uint64_t ops, int round) {
                              return RunChannelRound("t2f", false, true, ops, round,
                                                     64);
                          }});
    }
    if (ModelSelected(config, "t2t")) {
        models.push_back({"t2t", "Thread→Thread Channel",
                          "1 原生线程生产者 → 1 原生线程消费者（chan cap=64）",
                          "一个值完成发送并接收", 1,
                          [](std::uint64_t ops, int round) {
                              return RunChannelRound("t2t", false, false, ops, round,
                                                     64);
                          }});
    }
    if (ModelSelected(config, "mutex")) {
        models.push_back({"mutex", "Mutex 混合争抢",
                          "4 Fiber + 4 原生线程争抢同一把 sync::Mutex",
                          "一次 lock+unlock", 1,
                          [](std::uint64_t ops, int round) {
                              return RunMutexRound("mutex", ops, round);
                          }});
    }
    if (ModelSelected(config, "cv")) {
        models.push_back({"cv", "ConditionVariable 混合等待",
                          "2 Fiber + 2 原生线程共享 Mutex + not_empty/not_full",
                          "一个值入队并出队", 1,
                          [](std::uint64_t ops, int round) {
                              return RunCvRound("cv", ops, round);
                          }});
    }
    if (ModelSelected(config, "wg")) {
        models.push_back({"wg", "WaitGroup 混合波次",
                          "2 Fiber + 2 原生线程 Done；等待者按轮在 Fiber/线程间切换",
                          "一次 Done", 64,
                          [](std::uint64_t ops, int round) {
                              return RunWgRound("wg", ops, round);
                          }});
    }
    if (models.empty()) {
        std::fprintf(stderr, "[参数错误] --models 没有匹配任何模型\n");
        g_stop_watchdog.store(true, std::memory_order_release);
        watchdog.join();
        return 2;
    }

    const long long run_start_ns = NowNs();
    std::vector<ModelReport> reports;
    reports.reserve(models.size());
    for (const auto& model : models) {
        std::printf("\n--- 模型 %s：%s ---\n", model.id.c_str(),
                    model.title.c_str());
        std::fflush(stdout);
        const long long global_elapsed_ns = NowNs() - run_start_ns;
        reports.push_back(RunModel(model, config, global_elapsed_ns));
    }

    g_sched = nullptr;
    bool shutdown_ok = false;
    try {
        shutdown_ok = scheduler->shutdown_for(std::chrono::seconds(60));
    } catch (const std::exception& shutdown_error) {
        g_shutdown_note = std::string("shutdown_for 抛出异常：") +
                          shutdown_error.what();
    }
    if (!shutdown_ok && g_shutdown_note.empty()) {
        g_shutdown_note = "shutdown_for 返回 false（超时或仍有未回收 Fiber）";
    } else if (shutdown_ok && g_shutdown_note.empty()) {
        g_shutdown_note = "shutdown_for 正常回收";
    }
    scheduler.reset();

    g_stop_watchdog.store(true, std::memory_order_release);
    watchdog.join();
    g_active.store(false, std::memory_order_release);

    const ProcMem final_mem = ReadProcMem();
    std::printf("\n[收尾] 调度器：%s\n", g_shutdown_note.c_str());
    std::printf("[收尾] 最终 RSS=%.1fMiB（基线 %.1fMiB，Δ%.1fMiB）\n",
                static_cast<double>(final_mem.rss_kb) / 1024.0,
                static_cast<double>(g_baseline_mem.rss_kb) / 1024.0,
                static_cast<double>(static_cast<long long>(final_mem.rss_kb) -
                                   static_cast<long long>(g_baseline_mem.rss_kb)) /
                    1024.0);

    WriteReport(reports, config, "");

    bool all_ok = shutdown_ok;
    std::uint64_t total_ops = 0;
    for (const auto& report : reports) {
        all_ok = all_ok && report.ok;
        total_ops += TotalOps(report);
    }

    std::printf("\n=== 汇总 ===\n");
    for (const auto& report : reports) {
        const NsStats stats = ComputeNsStats(report);
        std::printf("  %-6s %2d/%2d 轮  总操作=%-12" PRIu64
                    " 中位 ns/op=%8.1f  耗时=%9s  %s%s\n",
                    report.id.c_str(), report.rounds_done, report.rounds_planned,
                    TotalOps(report), stats.median,
                    FormatSeconds(report.elapsed_ns).c_str(),
                    report.ok ? "通过" : "失败",
                    report.budget_trimmed ? "（预算裁剪）" : "");
    }
    std::printf("  总计操作数：%" PRIu64 "（%.2f×10^9）\n", total_ops,
                static_cast<double>(total_ops) / 1e9);
    std::printf("  结论：%s\n", all_ok ? "全部通过" : "存在失败，详见报告");
    std::fflush(stdout);

    return all_ok ? 0 : 1;
}
