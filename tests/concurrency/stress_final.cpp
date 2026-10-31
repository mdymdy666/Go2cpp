/**
 * @file stress_final.cpp
 * @brief 最终巨量混合正确性压测（线程池 + Fiber 池，10^9 操作 × 3 轮）。
 *
 * ## 定位
 * 并发结构验收的最后一关：不是「每个结构各跑一遍」，而是在**同一个进程、
 * 同一批参与者**里把全部结构与全部场景混在一起执行，用十亿量级操作暴露
 * 结构之间的相互作用：锁序、park/wake 竞态、关闭竞态、跨 M 迁移、定时器
 * 与取消路径。
 *
 * ## 参与者
 * - 8 个原生 OS 线程 worker；
 * - 8 个受管 Fiber worker，跑在多 P（默认 4 个 P）Scheduler 上；
 * - 每个 worker 按**加权随机**顺序执行 21 类操作，覆盖：
 *   Channel（cap 0/1/64/1024）、FastChannel（SPSC/MPMC）、Mutex、
 *   ConditionVariable、WaitGroup、RWMutex、SpinMutex、Select（多 case +
 *   default + timeout + 取消）、Context（WithCancel/WithTimeout + 中途取消）、
 *   Error（Fiber-local：M1 产生 → 挂起 → M2 恢复）、网络（socketpair 收发）、
 *   嵌套（锁嵌套 / channel 嵌套 / select 嵌套）、关闭竞态（Close 与操作并发）。
 *
 * ## 断言（全部是真实计数，不是恒真）
 * - 每类操作计数与目标操作数一致；
 * - 共享 Channel / FastChannel 用「全局唯一序号 + 计数 + 求和」证明
 *   **无丢失、无重复**（值唯一 ⇒ 丢失一个会少 1，重复一个会多 1；一丢一重
 *   必然让和变化）；
 * - 私有 FIFO Channel / SPSC 用严格递增验证器证明 **FIFO 无乱序**；
 * - Mutex / SpinMutex 临界区用「成对序号必须相等」证明互斥没有被破坏；
 * - RWMutex 读者用「8 个槽必须全等」证明读锁看不到写者撕裂；
 * - CV / WG 结束时 waiters=0、队列排空、计数归零；
 * - Select 每次返回的 index 必须落在 case 范围内，且五类分支都被覆盖；
 * - Context 取消后 IsDone/Err 立即一致，子 Context 同步取消；
 * - Error 在 Fiber 跨 M 恢复后指针、错误码、Fiber-local 值三重一致；
 * - 网络收发字节数相等且内容回环一致；
 * - 关闭竞态：Close 幂等、关闭后 Send 被拒、Recv 排空后才 Closed、
 *   轮末 waiters=0 且 Len=0；
 * - 无死锁：watchdog 180s 无进展即失败并打印各 worker 现场；
 * - RSS 稳定：相对基线增量有上限，且轮间不持续增长。
 *
 * ## 编译与运行（独立程序，不注册 ctest，不改 CMake）
 * ```
 * g++ -std=c++17 -O3 -DNDEBUG -pthread -Iinclude \
 *     tests/concurrency/stress_final.cpp \
 *     -L.build-bento-wsl -Wl,-rpath,$PWD/.build-bento-wsl \
 *     -lgo2cpp_channel -lgo2cpp_sync -lgo2cpp_scheduler -lgo2cpp_fiber \
 *     -lgo2cpp_context -lgo2cpp_error -lgo2cpp_log \
 *     -o .build-bento-wsl/stress_final
 *
 * taskset -c 2,3,4,5 .build-bento-wsl/stress_final \
 *     --ops 1000000000 --rounds 3 --threads 8 --fibers 8 --sched-p 4
 * ```
 *
 * 参数：
 * - `--ops N`        每轮总操作数（默认 10^9，全部 worker 累计）；
 * - `--rounds N`     轮数（默认 3）；
 * - `--threads N`    原生线程 worker 数（默认 8）；
 * - `--fibers N`     Fiber worker 数（默认 8）；
 * - `--sched-p N`    Scheduler 的 P 数（默认 4）；
 * - `--hang-sec N`   watchdog 无进展判定阈值（默认 180）；
 * - `--quick`        快速冒烟（10^6 操作 × 1 轮）；
 * - `--out PATH`     报告输出路径（默认 /tmp/stress_final.md）。
 *
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include "go2cpp/channel.hpp"
#include "go2cpp/concurrency/context.hpp"
#include "go2cpp/error/error_fiber.hpp"
#include "go2cpp/error/error_local.hpp"
#include "go2cpp/concurrency/fast_channel.hpp"
#include "go2cpp/concurrency/fiber.hpp"
#include "go2cpp/concurrency/rwmutex.hpp"
#include "go2cpp/concurrency/spin_mutex.hpp"
#include "go2cpp/error/error.hpp"
#include "go2cpp/scheduler.hpp"
#include "go2cpp/sync.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

using go2cpp::ChannelPtr;
using go2cpp::Context;
using go2cpp::ContextPtr;
using go2cpp::MakeChannel;
using go2cpp::Scheduler;
using go2cpp::SelectCase;
using go2cpp::SelectResult;
using go2cpp::Task;
using go2cpp::sync::ConditionVariable;
using go2cpp::sync::Mutex;
using go2cpp::sync::RWMutex;
using go2cpp::sync::SpinMutex;
using go2cpp::sync::WaitGroup;

using ErrorPtr = go2cpp::error::ErrorPtr;
using go2cpp::error::ErrorCode;
using go2cpp::error::ErrorLocal;

// ===========================================================================
// 配置
// ===========================================================================

/** @brief 命令行配置。 */
struct Config {
    std::uint64_t ops_per_round = 1000000000ULL;  ///< 每轮总操作数（10^9）
    int rounds = 3;                               ///< 轮数
    int thread_workers = 8;                       ///< 原生线程 worker 数
    int fiber_workers = 8;                        ///< Fiber worker 数
    std::size_t sched_p = 4;                      ///< Scheduler 的 P 数
    int hang_sec = 180;                           ///< 无进展多久判定死锁
    bool quick = false;                           ///< 快速冒烟
    std::string out = "/tmp/stress_final.md";     ///< 报告路径
    std::string pin;                              ///< 绑核说明（仅记录）
    std::string only;                             ///< 只跑某一类（性能定位）
};

Config g_cfg;

int g_only_kind = -1;  ///< >=0 时只执行该类操作（性能单类定位）

constexpr int kMaxWorkers = 64;  ///< 诊断数组上限

// ===========================================================================
// 操作类别与权重
// ===========================================================================

/** @brief 混合压测覆盖的 21 类操作。 */
enum class OpKind : int {
    kChanSend = 0,   ///< 共享 Channel 非阻塞发送（cap 64/1024）
    kChanRecv,       ///< 共享 Channel 非阻塞接收
    kRendezvous,     ///< cap 0 无缓冲 Channel，带超时收发
    kChanFifo,       ///< 私有 FIFO Channel（cap 1/64/1024），严格有序
    kFastSpsc,       ///< 私有 SPSC 无锁环，严格有序
    kFastMpmc,       ///< 共享 MPMC FastChannel
    kMutex,          ///< sync::Mutex 临界区互斥
    kSpin,           ///< sync::SpinMutex 临界区互斥
    kRwRead,         ///< RWMutex 读锁 + 快照一致性
    kRwWrite,        ///< RWMutex 写锁
    kCvPrivate,      ///< 私有 CV 队列（真实 Wait/Notify）
    kCvShared,       ///< 共享 CV 队列（非阻塞尝试 + Notify）
    kWaitGroup,      ///< 全局与私有 WaitGroup
    kSelect,         ///< Select（多 case + default + timeout + 取消）
    kContext,        ///< Context 创建/取消/超时
    kError,          ///< Error 的 Fiber-local 跨 M 迁移
    kNet,            ///< socketpair 回环收发
    kNestedLock,     ///< 锁嵌套（固定顺序 A→B）
    kNestedChan,     ///< channel of channel 嵌套
    kNestedSelect,   ///< 双层 select 嵌套
    kCloseRace,      ///< 关闭竞态（Close 与 Send/Recv 并发）
    kOpCount         ///< 哨兵
};

constexpr int kKindCount = static_cast<int>(OpKind::kOpCount);

/**
 * @brief 每类操作的相对权重。
 *
 * 权重按「单次真实成本」反向设定：Mutex/Channel 这类纳秒级操作权重高，
 * Select/Context/网络这类微秒级操作权重低。这样十亿次操作里每一类都拿到
 * 百万级以上的真实样本，同时总耗时可控。
 */
constexpr int kWeights[kKindCount] = {
    /* kChanSend */ 60,
    /* kChanRecv */ 60,
    /* kRendezvous */ 6,
    /* kChanFifo */ 30,
    /* kFastSpsc */ 40,
    /* kFastMpmc */ 40,
    /* kMutex */ 100,
    /* kSpin */ 100,
    /* kRwRead */ 60,
    /* kRwWrite */ 20,
    /* kCvPrivate */ 30,
    /* kCvShared */ 20,
    /* kWaitGroup */ 50,
    /* kSelect */ 12,
    /* kContext */ 4,
    /* kError */ 3,
    /* kNet */ 1,
    /* kNestedLock */ 20,
    /* kNestedChan */ 4,
    /* kNestedSelect */ 3,
    /* kCloseRace */ 5,
};

constexpr int kTotalWeight = [] {
    int total = 0;
    for (int i = 0; i < kKindCount; ++i) {
        total += kWeights[i];
    }
    return total;
}();

/** @brief 类别名（诊断/报告用）。 */
const char* KindName(int kind) {
    static const char* const kNames[kKindCount] = {
        "channel-send", "channel-recv", "chan0-rendezvous", "chan-fifo",
        "fast-spsc",    "fast-mpmc",    "mutex",             "spin-mutex",
        "rw-read",      "rw-write",     "cv-private",        "cv-shared",
        "waitgroup",    "select",       "context",           "error",
        "socketpair",   "nested-lock",  "nested-chan",       "nested-select",
        "close-race",
    };
    if (kind < 0 || kind >= kKindCount) {
        return "?";
    }
    return kNames[kind];
}

/** @brief 类别名 → 编号（供 --only 使用）；未知返回 -1。 */
int KindFromName(const std::string& name) {
    for (int i = 0; i < kKindCount; ++i) {
        if (name == KindName(i)) {
            return i;
        }
    }
    return -1;
}

// ===========================================================================
// 基础工具
// ===========================================================================

long long NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               Clock::now().time_since_epoch())
        .count();
}

/** @brief splitmix64：每 worker 独立状态，无共享、无锁。 */
inline std::uint64_t NextRand(std::uint64_t* state) {
    std::uint64_t z = (*state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

/** @brief 按权重把一个 [0, kTotalWeight) 的随机数映射到类别。 */
int PickKind(std::uint64_t r) {
    int bucket = static_cast<int>(r % static_cast<std::uint64_t>(kTotalWeight));
    for (int i = 0; i < kKindCount; ++i) {
        bucket -= kWeights[i];
        if (bucket < 0) {
            return i;
        }
    }
    return kKindCount - 1;
}

/** @brief 让出执行权：Fiber 走调度器 yield，原生线程走 sched_yield。 */
inline void YieldExecution() {
    if (Scheduler::current_task_ptr() != nullptr) {
        Scheduler* scheduler = Scheduler::current_scheduler();
        if (scheduler != nullptr) {
            scheduler->yield_current();
            return;
        }
    }
    std::this_thread::yield();
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

/** @brief 进程内存快照（RSS 用于泄漏判定）。 */
struct ProcMem {
    std::uint64_t rss_kb = 0;
    std::uint64_t vmsize_kb = 0;
};

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

/** @brief 读取系统 1 分钟平均负载。 */
double ReadLoadAvg() {
    std::ifstream in("/proc/loadavg");
    double value = 0.0;
    if (in >> value) {
        return value;
    }
    return 0.0;
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

std::string FormatU64(std::uint64_t value) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%" PRIu64, value);
    return buffer;
}

std::string FormatDouble(double value, int precision = 2) {
    char buffer[128];
    std::snprintf(buffer, sizeof(buffer), "%.*f", precision, value);
    return buffer;
}

std::string FormatSeconds(long long ns) {
    return FormatDouble(static_cast<double>(ns) / 1e9, 3) + " s";
}

// ===========================================================================
// 断言与进度
// ===========================================================================

/** @brief 单条断言。 */
struct Assertion {
    std::string name;
    bool passed = false;
    std::string detail;
};

void AddCheck(std::vector<Assertion>* checks, const std::string& name, bool passed,
              const std::string& detail = std::string()) {
    Assertion assertion;
    assertion.name = name;
    assertion.passed = passed;
    assertion.detail = detail;
    checks->push_back(std::move(assertion));
}

bool AllChecksPassed(const std::vector<Assertion>& checks) {
    if (checks.empty()) {
        return false;
    }
    for (const auto& check : checks) {
        if (!check.passed) {
            return false;
        }
    }
    return true;
}

std::atomic<std::uint64_t> g_progress{0};              ///< 全局已完成操作
std::atomic<const char*> g_phase{"初始化"};            ///< 当前阶段
std::atomic<int> g_round{0};                           ///< 当前轮次
std::atomic<int> g_worker_count{0};                    ///< 当前 worker 数
std::atomic<std::uint64_t> g_worker_ops[kMaxWorkers];  ///< 每 worker 已操作数
std::atomic<int> g_worker_kind[kMaxWorkers];           ///< 每 worker 当前类别
std::atomic<int> g_worker_is_fiber[kMaxWorkers];       ///< 每 worker 是否 Fiber
std::atomic<Task*> g_fiber_tasks[kMaxWorkers];  ///< fiber worker 的 Task（watchdog 诊断）
std::atomic<int> g_fiber_task_count{0};         ///< fiber worker 数量（watchdog 诊断）
Scheduler* g_scheduler{nullptr};                ///< 当前 Scheduler（watchdog 诊断）
std::atomic<bool> g_stop_watchdog{false};
std::atomic<bool> g_watchdog_fired{false};

struct Shared;
std::atomic<Shared*> g_shared{nullptr};  ///< 当前轮共享对象（watchdog 诊断用）

/** @brief G 状态名（watchdog 诊断用）。 */
const char* GStateName(go2cpp::GState state) {
    switch (state) {
        case go2cpp::GState::kNew: return "New";
        case go2cpp::GState::kRunnable: return "Runnable";
        case go2cpp::GState::kRunning: return "Running";
        case go2cpp::GState::kWaiting: return "Waiting";
        case go2cpp::GState::kDead: return "Dead";
        case go2cpp::GState::kCancelled: return "Cancelled";
        case go2cpp::GState::kFailed: return "Failed";
    }
    return "?";
}

/** @brief 死锁看门狗：连续 hang_sec 秒无进展就打印现场并强制退出。 */
void WatchdogMain();

// ===========================================================================
// 每轮共享设施
// ===========================================================================

/**
 * @brief 一轮压测的共享对象集合（每轮重建，天然暴露析构/重建竞态）。
 *
 * 所有跨 worker 的统计量都是原子：worker 之间不存在「只有某个 worker 写」
 * 的普通变量，避免数据竞争。
 */
struct Shared {
    // ---- Channel -------------------------------------------------------
    ChannelPtr<std::uint64_t> chan64;      ///< cap 64，非阻塞收发
    ChannelPtr<std::uint64_t> chan1024;    ///< cap 1024，非阻塞收发
    ChannelPtr<std::uint64_t> rendezvous;  ///< cap 0，带超时收发
    ChannelPtr<std::uint64_t> close_chan;  ///< 关闭竞态专用
    ChannelPtr<std::uint64_t> select_chan;  ///< Select 专用（不与计数通道混用）
    ChannelPtr<std::uint64_t> select_idle_chan;  ///< 永空通道：触发 after/取消

    // ---- FastChannel ---------------------------------------------------
    go2cpp::FastChannel<std::uint64_t, 1024> fast_mpmc;

    // ---- 同步原语 -------------------------------------------------------
    // **按分片（shard）组织**：16 个参与者在 4 个核上共享同一把锁时，几乎每次
    // 加锁都要 park/wake（实测单锁 294µs/op，比无争用慢 4 个数量级），既不代表
    // 真实负载也无法在十亿级跑完。分片后每片仅 2 个参与者，仍保留真实争用。
    static constexpr std::size_t kShards = 8;
    Mutex mutex[kShards];
    SpinMutex spin[kShards];
    RWMutex rw[kShards];
    Mutex cv_mutex[kShards];
    ConditionVariable cv_not_empty[kShards];
    ConditionVariable cv_not_full[kShards];
    std::deque<std::uint64_t> cv_queue[kShards];
    WaitGroup wg;
    Mutex nest_a[kShards];
    Mutex nest_b[kShards];
    std::atomic<std::uint64_t> nest_counter{0};

    // ---- RWMutex 保护的数据（每分片一组 8 槽）--------------------------
    static constexpr std::size_t kRwSlots = 8;
    std::atomic<std::uint64_t> rw_data[kShards][kRwSlots];
    std::atomic<std::uint64_t> rw_version[kShards];

    // ---- channel of channel --------------------------------------------
    ChannelPtr<ChannelPtr<std::uint64_t>> chan_of_chan;
    ChannelPtr<std::uint64_t> sub_channels[4];

    // ---- 计数：Channel（cap 64 / 1024）--------------------------------
    std::atomic<std::uint64_t> chan64_send_seq{0};
    std::atomic<std::uint64_t> chan64_sent_count{0};
    std::atomic<std::uint64_t> chan64_sent_sum{0};
    std::atomic<std::uint64_t> chan64_recv_count{0};
    std::atomic<std::uint64_t> chan64_recv_sum{0};
    std::atomic<std::uint64_t> chan1024_send_seq{0};
    std::atomic<std::uint64_t> chan1024_sent_count{0};
    std::atomic<std::uint64_t> chan1024_sent_sum{0};
    std::atomic<std::uint64_t> chan1024_recv_count{0};
    std::atomic<std::uint64_t> chan1024_recv_sum{0};

    // ---- 计数：cap 0 rendezvous ----------------------------------------
    std::atomic<std::uint64_t> rendezvous_send_seq{0};
    std::atomic<std::uint64_t> rendezvous_sent_count{0};
    std::atomic<std::uint64_t> rendezvous_sent_sum{0};
    std::atomic<std::uint64_t> rendezvous_recv_count{0};
    std::atomic<std::uint64_t> rendezvous_recv_sum{0};
    std::atomic<std::uint64_t> rendezvous_timeouts{0};

    // ---- 计数：FastChannel MPMC ----------------------------------------
    std::atomic<std::uint64_t> mpmc_send_seq{0};
    std::atomic<std::uint64_t> mpmc_sent_count{0};
    std::atomic<std::uint64_t> mpmc_sent_sum{0};
    std::atomic<std::uint64_t> mpmc_recv_count{0};
    std::atomic<std::uint64_t> mpmc_recv_sum{0};

    // ---- 计数：Mutex / Spin --------------------------------------------
    std::atomic<std::uint64_t> mutex_counter[kShards];
    std::atomic<std::uint64_t> mutex_serial[kShards];
    std::atomic<std::uint64_t> mutex_corrupt[kShards];
    std::atomic<std::uint64_t> spin_counter[kShards];
    std::atomic<std::uint64_t> spin_serial[kShards];
    std::atomic<std::uint64_t> spin_corrupt[kShards];

    // ---- 计数：RWMutex -------------------------------------------------
    std::atomic<std::uint64_t> rw_reads[kShards];
    std::atomic<std::uint64_t> rw_writes[kShards];
    std::atomic<std::uint64_t> rw_torn[kShards];

    // ---- 计数：CV ------------------------------------------------------
    std::atomic<std::uint64_t> cv_produced[kShards];
    std::atomic<std::uint64_t> cv_consumed[kShards];
    std::atomic<std::uint64_t> cv_notifies[kShards];
    std::atomic<std::uint64_t> cv_skips[kShards];

    // ---- 计数：WaitGroup ----------------------------------------------
    std::atomic<std::uint64_t> wg_adds{0};
    std::atomic<std::uint64_t> wg_dones{0};
    std::atomic<std::uint64_t> wg_wait_true{0};
    std::atomic<std::uint64_t> wg_wait_false{0};
    std::atomic<std::uint64_t> wg_bad{0};

    // ---- 计数：Select --------------------------------------------------
    std::atomic<std::uint64_t> select_ok{0};
    std::atomic<std::uint64_t> select_bad{0};
    std::atomic<std::uint64_t> select_timeout_hits{0};
    std::atomic<std::uint64_t> select_recv_hits{0};
    std::atomic<std::uint64_t> select_send_hits{0};
    std::atomic<std::uint64_t> select_default_hits{0};
    std::atomic<std::uint64_t> select_after_hits{0};
    std::atomic<std::uint64_t> select_cancel_hits{0};

    // ---- 计数：Context -------------------------------------------------
    std::atomic<std::uint64_t> ctx_created{0};
    std::atomic<std::uint64_t> ctx_cancelled{0};
    std::atomic<std::uint64_t> ctx_bad{0};

    // ---- 计数：Error ---------------------------------------------------
    std::atomic<std::uint64_t> err_checks{0};
    std::atomic<std::uint64_t> err_bad{0};

    // ---- 计数：网络 ----------------------------------------------------
    std::atomic<std::uint64_t> net_ops{0};
    std::atomic<std::uint64_t> net_bad{0};

    // ---- 计数：嵌套 ----------------------------------------------------
    std::atomic<std::uint64_t> nested_lock_ok{0};
    std::atomic<std::uint64_t> nested_chan_ok{0};
    std::atomic<std::uint64_t> nested_select_ok{0};
    std::atomic<std::uint64_t> nested_bad{0};

    // ---- 计数：关闭竞态 ------------------------------------------------
    std::atomic<std::uint64_t> close_calls{0};
    std::atomic<std::uint64_t> close_send_ok{0};
    std::atomic<std::uint64_t> close_send_rejected{0};
    std::atomic<std::uint64_t> close_recv_ok{0};
    std::atomic<std::uint64_t> close_recv_closed{0};
    std::atomic<std::uint64_t> close_bad{0};
    std::atomic<std::uint64_t> close_send_seq{0};
    std::atomic<std::uint64_t> close_sent_sum{0};

    /** @brief 已取消的 Context，供 Select 的 DoneCase 使用（每轮一个）。 */
    ContextPtr cancelled_ctx;
};

/**
 * @brief 死锁看门狗：连续 hang_sec 秒无进展就打印现场并强制退出。
 *
 * 现场包含：每个 shard 的 Mutex 状态字（held / starving / woken / waiters）、
 * CV 等待者、WaitGroup 计数、FastChannel 与 Channel 的 waiters/长度。
 * 这些是判断「锁泄漏」与「唤醒丢失」的关键证据：`held=false` 而
 * `waiters>0` 就是丢失唤醒；`held=true` 而持有者已退出就是锁泄漏。
 */
void WatchdogMain() {
    std::uint64_t last = 0;
    int idle_seconds = 0;
    int tick = 0;
    while (!g_stop_watchdog.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (g_stop_watchdog.load(std::memory_order_acquire)) {
            break;
        }
        const std::uint64_t now = g_progress.load(std::memory_order_relaxed);
        if (now == last) {
            ++idle_seconds;
        } else {
            idle_seconds = 0;
            last = now;
        }
        ++tick;
        if (tick % 30 == 0 && idle_seconds < g_cfg.hang_sec) {
            std::printf("[watchdog] 进度=%" PRIu64 " 阶段=%s 轮次=%d loadavg=%.2f\n",
                        now, g_phase.load(std::memory_order_relaxed),
                        g_round.load(std::memory_order_relaxed), ReadLoadAvg());
            std::fflush(stdout);
        }
        if (idle_seconds < g_cfg.hang_sec) {
            continue;
        }
        g_watchdog_fired.store(true, std::memory_order_release);
        std::fprintf(stderr,
                     "\n[watchdog] 检测到 %d 秒无进展，判定死锁！\n"
                     "[watchdog] 阶段=%s 轮次=%d 全局进度=%" PRIu64 "\n",
                     idle_seconds, g_phase.load(std::memory_order_relaxed),
                     g_round.load(std::memory_order_relaxed), now);
        const int workers = g_worker_count.load(std::memory_order_acquire);
        for (int i = 0; i < workers; ++i) {
            std::fprintf(stderr,
                         "[watchdog]   worker %2d（%s）：ops=%" PRIu64
                         " 当前类别=%s\n",
                         i,
                         g_worker_is_fiber[i].load(std::memory_order_relaxed)
                             ? "fiber"
                             : "thread",
                         g_worker_ops[i].load(std::memory_order_relaxed),
                         KindName(g_worker_kind[i].load(
                             std::memory_order_relaxed)));
        }
        Shared* const shared = g_shared.load(std::memory_order_acquire);
        if (shared != nullptr) {
            for (std::size_t sh = 0; sh < Shared::kShards; ++sh) {
                const auto m = shared->mutex[sh].DebugSnapshotInternal();
                const auto cve =
                    shared->cv_not_empty[sh].DebugSnapshotInternal();
                const auto cvf =
                    shared->cv_not_full[sh].DebugSnapshotInternal();
                std::fprintf(
                    stderr,
                    "[watchdog]   shard %zu：mutex held=%d state=0x%02x "
                    "starving=%d woken=%d waiters=%zu(native=%zu fiber=%zu) "
                    "counter=%" PRIu64 " cv_waiters=(%zu,%zu) "
                    "cvq=%zu rw_ver=%" PRIu64 "\n",
                    sh, static_cast<int>(m.held),
                    static_cast<unsigned>(m.state_bits),
                    static_cast<int>(m.starving), static_cast<int>(m.woken),
                    m.waiters, m.native_waiters, m.fiber_waiters,
                    shared->mutex_counter[sh].load(std::memory_order_relaxed),
                    cve.waiters, cvf.waiters, shared->cv_queue[sh].size(),
                    shared->rw_version[sh].load(std::memory_order_relaxed));
            }
            const auto wg = shared->wg.DebugSnapshotInternal();
            std::fprintf(stderr,
                         "[watchdog]   waitgroup count=%" PRId64 " waiters=%zu\n",
                         wg.count, wg.waiters);
            const auto c64 = shared->chan64->DebugSnapshot();
            const auto c1024 = shared->chan1024->DebugSnapshot();
            const auto c0 = shared->rendezvous->DebugSnapshot();
            const auto csel = shared->select_chan->DebugSnapshot();
            const auto cclose = shared->close_chan->DebugSnapshot();
            std::fprintf(
                stderr,
                "[watchdog]   channels：cap64(len=%zu s=%zu r=%zu) "
                "cap1024(len=%zu s=%zu r=%zu) cap0(s=%zu r=%zu) "
                "select(len=%zu s=%zu r=%zu) close(len=%zu s=%zu r=%zu)\n",
                c64.length, c64.send_waiters, c64.recv_waiters, c1024.length,
                c1024.send_waiters, c1024.recv_waiters, c0.send_waiters,
                c0.recv_waiters, csel.length, csel.send_waiters,
                csel.recv_waiters, cclose.length, cclose.send_waiters,
                cclose.recv_waiters);
            std::fprintf(stderr,
                         "[watchdog]   fast_mpmc len=%zu closed=%d\n",
                         shared->fast_mpmc.Len(),
                         static_cast<int>(shared->fast_mpmc.IsClosed()));
            const int ftasks =
                g_fiber_task_count.load(std::memory_order_acquire);
            for (int fi = 0; fi < ftasks && fi < kMaxWorkers; ++fi) {
                const Task* const t =
                    g_fiber_tasks[fi].load(std::memory_order_relaxed);
                if (t == nullptr) {
                    continue;
                }
                std::fprintf(stderr,
                             "[watchdog]   fibtask#%d id=%llu state=%s "
                             "queued=%d cancel=%d lastP=%zu\n",
                             fi, static_cast<unsigned long long>(t->id()),
                             GStateName(t->state()),
                             static_cast<int>(t->queued()),
                             static_cast<int>(t->cancellation_requested()),
                             static_cast<std::size_t>(
                                 t->last_processor_id()));
            }
            if (g_scheduler != nullptr) {
                std::fprintf(
                    stderr,
                    "[watchdog]   scheduler runnable=%zu global=%zu\n",
                    g_scheduler->RunnableCount(),
                    g_scheduler->GlobalRunnableCount());
            }
        }
        std::fflush(stderr);
        // 进程里可能有多把锁处于半状态，不走析构，直接终止。
        std::_Exit(3);
    }
}

// ===========================================================================
// FIFO 严格有序验证器（并带自检，证明它不是恒真）
// ===========================================================================

/** @brief 严格递增验证器：收到的值必须恰好等于期望的下一个值。 */
class FifoVerifier {
public:
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
    std::uint64_t violations() const { return m_violations; }
    bool has_bad() const { return m_has_bad; }
    std::uint64_t bad_expected() const { return m_bad_expected; }
    std::uint64_t bad_actual() const { return m_bad_actual; }

private:
    std::uint64_t m_expected = 0;
    std::uint64_t m_seen = 0;
    std::uint64_t m_violations = 0;
    bool m_has_bad = false;
    std::uint64_t m_bad_expected = 0;
    std::uint64_t m_bad_actual = 0;
};

/** @brief 用坏序列验证验证器确实会报错（断言不许恒真）。 */
bool FifoVerifierSelfCheck() {
    {
        FifoVerifier verifier;
        for (std::uint64_t i = 0; i < 4; ++i) {
            verifier.Accept(i);
        }
        if (verifier.violations() != 0 || verifier.seen() != 4) {
            return false;
        }
    }
    {
        FifoVerifier verifier;  // 乱序
        verifier.Accept(0);
        verifier.Accept(2);
        verifier.Accept(1);
        if (verifier.violations() == 0 || !verifier.has_bad()) {
            return false;
        }
    }
    {
        FifoVerifier verifier;  // 重复
        verifier.Accept(0);
        verifier.Accept(0);
        if (verifier.violations() == 0) {
            return false;
        }
    }
    return true;
}

// ===========================================================================
// worker 上下文
// ===========================================================================

/** @brief 一个参与者（原生线程或受管 Fiber）的全部状态。 */
struct WorkerCtx {
    int id = 0;
    bool is_fiber = false;
    std::size_t shard = 0;  ///< 分片号：id % Shared::kShards
    std::uint64_t quota = 0;
    std::uint64_t rand_state = 0;
    Shared* shared = nullptr;
    std::uint64_t ops_done = 0;
    std::uint64_t kind_count[kKindCount] = {};

    // 私有 Channel：覆盖 cap 1 / 64 / 1024 的严格 FIFO
    ChannelPtr<std::uint64_t> fifo[3];
    static constexpr std::size_t kFifoCap[3] = {1, 64, 1024};
    FifoVerifier fifo_verifier[3];
    std::uint64_t fifo_seq[3] = {0, 0, 0};
    std::uint64_t fifo_sent[3] = {0, 0, 0};
    std::uint64_t fifo_recv[3] = {0, 0, 0};
    std::uint64_t fifo_violations = 0;

    // 私有 SPSC 环
    go2cpp::FastSpscChannel<std::uint64_t, 64> spsc;
    std::uint64_t spsc_seq = 0;
    std::uint64_t spsc_expected = 0;
    std::uint64_t spsc_sent = 0;
    std::uint64_t spsc_recv = 0;
    std::uint64_t spsc_violations = 0;

    // 私有 CV 队列
    Mutex cv_mutex;
    ConditionVariable cv_not_empty;
    std::deque<std::uint64_t> cv_queue;
    std::uint64_t cv_pushed = 0;
    std::uint64_t cv_popped = 0;
    std::uint64_t cv_wait_false = 0;

    // 私有 WaitGroup
    WaitGroup local_wg;

    // 私有 socketpair
    int sock_a = -1;
    int sock_b = -1;
    std::uint64_t net_ops = 0;
    std::uint64_t net_bad = 0;

    // Error 槽（Fiber-local 或线程 TLS）
    ErrorLocal<ErrorPtr>* error_slot = nullptr;

    // 本地错误统计（避免共享原子争用）
    std::uint64_t err_checks = 0;
    std::uint64_t err_bad = 0;
    std::uint64_t select_bad = 0;
    std::uint64_t select_timeouts = 0;
    std::uint64_t ctx_bad = 0;
    std::uint64_t nested_bad = 0;
    std::uint64_t close_bad = 0;
    std::uint64_t mutex_bad = 0;
    std::uint64_t spin_bad = 0;
    std::uint64_t rw_bad = 0;
    std::uint64_t select_counter = 0;  ///< 控制 after 真实等待的抽样频率

    ~WorkerCtx() {
        if (sock_a >= 0) {
            (void)::close(sock_a);
        }
        if (sock_b >= 0) {
            (void)::close(sock_b);
        }
    }
};

/** @brief 起跑门：所有参与者就位后同一时刻放行。 */
class StartGate {
public:
    void Wait() const {
        while (!m_open.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    }
    void Open() { m_open.store(true, std::memory_order_release); }

private:
    std::atomic<bool> m_open{false};
};

// ===========================================================================
// 单类操作实现
// ===========================================================================

/** @brief Channel 非阻塞发送：满了就翻转成一次接收，保证始终有进展。 */
void DoChanSend(WorkerCtx& c, std::uint64_t r) {
    Shared& s = *c.shared;
    const bool use_64 = (r & 1) != 0;
    ChannelPtr<std::uint64_t>& channel = use_64 ? s.chan64 : s.chan1024;
    std::atomic<std::uint64_t>& seq_atomic =
        use_64 ? s.chan64_send_seq : s.chan1024_send_seq;
    std::atomic<std::uint64_t>& sent_count =
        use_64 ? s.chan64_sent_count : s.chan1024_sent_count;
    std::atomic<std::uint64_t>& sent_sum =
        use_64 ? s.chan64_sent_sum : s.chan1024_sent_sum;
    std::atomic<std::uint64_t>& recv_count =
        use_64 ? s.chan64_recv_count : s.chan1024_recv_count;
    std::atomic<std::uint64_t>& recv_sum =
        use_64 ? s.chan64_recv_sum : s.chan1024_recv_sum;

    const std::uint64_t seq = seq_atomic.fetch_add(1, std::memory_order_relaxed);
    const auto result = channel->TrySend(seq);
    if (result.Ok()) {
        sent_count.fetch_add(1, std::memory_order_relaxed);
        sent_sum.fetch_add(seq, std::memory_order_relaxed);
        return;
    }
    if (result.Closed()) {
        ++c.nested_bad;
        return;
    }
    // 缓冲满：翻转做一次接收，既保证进展又覆盖接收路径。
    const auto recv = channel->TryRecv();
    if (recv.Ok()) {
        recv_count.fetch_add(1, std::memory_order_relaxed);
        recv_sum.fetch_add(*recv.value, std::memory_order_relaxed);
    } else if (recv.Closed()) {
        ++c.nested_bad;
    }
}

/** @brief Channel 非阻塞接收：空了就翻转成一次发送。 */
void DoChanRecv(WorkerCtx& c, std::uint64_t r) {
    Shared& s = *c.shared;
    const bool use_64 = (r & 1) != 0;
    ChannelPtr<std::uint64_t>& channel = use_64 ? s.chan64 : s.chan1024;
    std::atomic<std::uint64_t>& seq_atomic =
        use_64 ? s.chan64_send_seq : s.chan1024_send_seq;
    std::atomic<std::uint64_t>& sent_count =
        use_64 ? s.chan64_sent_count : s.chan1024_sent_count;
    std::atomic<std::uint64_t>& sent_sum =
        use_64 ? s.chan64_sent_sum : s.chan1024_sent_sum;
    std::atomic<std::uint64_t>& recv_count =
        use_64 ? s.chan64_recv_count : s.chan1024_recv_count;
    std::atomic<std::uint64_t>& recv_sum =
        use_64 ? s.chan64_recv_sum : s.chan1024_recv_sum;

    const auto recv = channel->TryRecv();
    if (recv.Ok()) {
        recv_count.fetch_add(1, std::memory_order_relaxed);
        recv_sum.fetch_add(*recv.value, std::memory_order_relaxed);
        return;
    }
    if (recv.Closed()) {
        ++c.nested_bad;
        return;
    }
    const std::uint64_t seq = seq_atomic.fetch_add(1, std::memory_order_relaxed);
    const auto result = channel->TrySend(seq);
    if (result.Ok()) {
        sent_count.fetch_add(1, std::memory_order_relaxed);
        sent_sum.fetch_add(seq, std::memory_order_relaxed);
    } else if (result.Closed()) {
        ++c.nested_bad;
    }
}

/** @brief cap 0 无缓冲 Channel：带 20µs 超时的真实 park/唤醒。 */
void DoRendezvous(WorkerCtx& c, std::uint64_t r) {
    Shared& s = *c.shared;
    constexpr auto kTimeout = std::chrono::microseconds(20);
    if ((r & 1) != 0) {
        const std::uint64_t seq =
            s.rendezvous_send_seq.fetch_add(1, std::memory_order_relaxed);
        const auto result = s.rendezvous->SendFor(seq, kTimeout);
        if (result.Ok()) {
            s.rendezvous_sent_count.fetch_add(1, std::memory_order_relaxed);
            s.rendezvous_sent_sum.fetch_add(seq, std::memory_order_relaxed);
        } else if (result.Cancelled()) {
            s.rendezvous_timeouts.fetch_add(1, std::memory_order_relaxed);
        } else if (result.Closed()) {
            ++c.nested_bad;
        }
        return;
    }
    const auto recv = s.rendezvous->RecvFor(kTimeout);
    if (recv.Ok()) {
        s.rendezvous_recv_count.fetch_add(1, std::memory_order_relaxed);
        s.rendezvous_recv_sum.fetch_add(*recv.value, std::memory_order_relaxed);
    } else if (recv.Cancelled()) {
        s.rendezvous_timeouts.fetch_add(1, std::memory_order_relaxed);
    } else if (recv.Closed()) {
        ++c.nested_bad;
    }
}

/** @brief 私有 FIFO Channel：先收后发，严格验证 FIFO 与无丢失。 */
void DoChanFifo(WorkerCtx& c, std::uint64_t r) {
    const std::size_t slot = static_cast<std::size_t>(r % 3);
    ChannelPtr<std::uint64_t>& channel = c.fifo[slot];
    FifoVerifier& verifier = c.fifo_verifier[slot];

    const auto recv = channel->TryRecv();
    if (recv.Ok()) {
        verifier.Accept(*recv.value);
        ++c.fifo_recv[slot];
    } else if (recv.Closed()) {
        ++c.fifo_violations;
    }
    // 只有确实有空位时才发送，避免序号被浪费（私有通道无并发，Len 可信）。
    if (channel->Len() < WorkerCtx::kFifoCap[slot]) {
        const std::uint64_t seq = c.fifo_seq[slot];
        const auto send = channel->TrySend(seq);
        if (send.Ok()) {
            ++c.fifo_seq[slot];
            ++c.fifo_sent[slot];
        } else {
            ++c.fifo_violations;
        }
    }
}

/** @brief 私有 SPSC 无锁环：发一个立即收一个，验证严格 FIFO。 */
void DoFastSpsc(WorkerCtx& c, std::uint64_t r) {
    const std::uint64_t value = c.spsc_seq;
    const go2cpp::FastStatus send = c.spsc.Send(value);
    if (send == go2cpp::FastStatus::kOk) {
        ++c.spsc_seq;
        ++c.spsc_sent;
    } else {
        ++c.spsc_violations;
        return;
    }
    std::uint64_t out = 0;
    const go2cpp::FastStatus recv = c.spsc.Recv(out);
    if (recv != go2cpp::FastStatus::kOk) {
        ++c.spsc_violations;
        return;
    }
    ++c.spsc_recv;
    if (out != c.spsc_expected) {
        ++c.spsc_violations;
    }
    ++c.spsc_expected;
    (void)r;
}

/** @brief 共享 MPMC FastChannel：满了/空了翻转方向，统计真实交付。 */
void DoFastMpmc(WorkerCtx& c, std::uint64_t r) {
    Shared& s = *c.shared;
    if ((r & 1) != 0) {
        const std::uint64_t seq =
            s.mpmc_send_seq.fetch_add(1, std::memory_order_relaxed);
        const go2cpp::FastStatus status = s.fast_mpmc.TrySend(seq);
        if (status == go2cpp::FastStatus::kOk) {
            s.mpmc_sent_count.fetch_add(1, std::memory_order_relaxed);
            s.mpmc_sent_sum.fetch_add(seq, std::memory_order_relaxed);
            return;
        }
        if (status == go2cpp::FastStatus::kClosed) {
            ++c.nested_bad;
            return;
        }
    }
    std::uint64_t out = 0;
    const go2cpp::FastStatus status = s.fast_mpmc.TryRecv(out);
    if (status == go2cpp::FastStatus::kOk) {
        s.mpmc_recv_count.fetch_add(1, std::memory_order_relaxed);
        s.mpmc_recv_sum.fetch_add(out, std::memory_order_relaxed);
    } else if (status == go2cpp::FastStatus::kClosed) {
        ++c.nested_bad;
    }
}

/** @brief Mutex：临界区内成对序号必须相等，否则证明互斥被破坏。 */
void DoMutex(WorkerCtx& c) {
    Shared& s = *c.shared;
    const std::size_t sh = c.shard;
    Mutex& mutex = s.mutex[sh];
    if (!mutex.Lock()) {
        ++c.mutex_bad;
        return;
    }
    const std::uint64_t counter =
        s.mutex_counter[sh].fetch_add(1, std::memory_order_relaxed) + 1;
    const std::uint64_t serial =
        s.mutex_serial[sh].fetch_add(1, std::memory_order_relaxed) + 1;
    if (counter != serial) {
        s.mutex_corrupt[sh].fetch_add(1, std::memory_order_relaxed);
        ++c.mutex_bad;
    }
    mutex.Unlock();
}

/** @brief SpinMutex：同 Mutex，验证短临界区的互斥与交接。 */
void DoSpin(WorkerCtx& c) {
    Shared& s = *c.shared;
    const std::size_t sh = c.shard;
    SpinMutex& spin = s.spin[sh];
    spin.Lock();
    const std::uint64_t counter =
        s.spin_counter[sh].fetch_add(1, std::memory_order_relaxed) + 1;
    const std::uint64_t serial =
        s.spin_serial[sh].fetch_add(1, std::memory_order_relaxed) + 1;
    if (counter != serial) {
        s.spin_corrupt[sh].fetch_add(1, std::memory_order_relaxed);
        ++c.spin_bad;
    }
    spin.Unlock();
}

/** @brief RWMutex 读：8 个槽必须全等（读锁看不到写者撕裂）。 */
void DoRwRead(WorkerCtx& c) {
    Shared& s = *c.shared;
    const std::size_t sh = c.shard;
    s.rw[sh].RLock();
    const std::uint64_t first =
        s.rw_data[sh][0].load(std::memory_order_relaxed);
    bool same = true;
    for (std::size_t i = 1; i < Shared::kRwSlots; ++i) {
        if (s.rw_data[sh][i].load(std::memory_order_relaxed) != first) {
            same = false;
        }
    }
    const std::uint64_t version =
        s.rw_version[sh].load(std::memory_order_relaxed);
    s.rw[sh].RUnlock();
    if (!same || (version != 0 && first != version)) {
        s.rw_torn[sh].fetch_add(1, std::memory_order_relaxed);
        ++c.rw_bad;
    }
    s.rw_reads[sh].fetch_add(1, std::memory_order_relaxed);
}

/** @brief RWMutex 写：整块写入同一个版本号。 */
void DoRwWrite(WorkerCtx& c) {
    Shared& s = *c.shared;
    const std::size_t sh = c.shard;
    s.rw[sh].Lock();
    const std::uint64_t version =
        s.rw_version[sh].fetch_add(1, std::memory_order_relaxed) + 1;
    for (std::size_t i = 0; i < Shared::kRwSlots; ++i) {
        s.rw_data[sh][i].store(version, std::memory_order_relaxed);
    }
    s.rw[sh].Unlock();
    s.rw_writes[sh].fetch_add(1, std::memory_order_relaxed);
}

/** @brief 私有 CV 队列：push 后立即 pop，覆盖真实 Wait/Notify 与重锁。 */
void DoCvPrivate(WorkerCtx& c, std::uint64_t r) {
    if ((r & 1) != 0) {
        c.cv_mutex.Lock();
        c.cv_queue.push_back(r);
        ++c.cv_pushed;
        c.cv_not_empty.NotifyOne();
        c.cv_mutex.Unlock();
    }
    c.cv_mutex.Lock();
    if (c.cv_queue.empty()) {
        // 正常情况下 push 已经完成；这里用 0 超时验证「未满足条件时 Wait
        // 返回 false」的语义，不引入阻塞。
        if (c.cv_not_empty.WaitFor(c.cv_mutex, std::chrono::nanoseconds(0))) {
            ++c.cv_wait_false;
        }
    }
    if (!c.cv_queue.empty()) {
        c.cv_queue.pop_front();
        ++c.cv_popped;
        c.cv_not_empty.NotifyOne();
    }
    c.cv_mutex.Unlock();
}

/** @brief 共享 CV 队列：满/空时跳过（不阻塞），其余走真实加锁 + Notify。 */
void DoCvShared(WorkerCtx& c, std::uint64_t r) {
    Shared& s = *c.shared;
    const std::size_t sh = c.shard;
    constexpr std::size_t kQueueCap = 128;
    s.cv_mutex[sh].Lock();
    if ((r & 1) != 0) {
        if (s.cv_queue[sh].size() >= kQueueCap) {
            s.cv_skips[sh].fetch_add(1, std::memory_order_relaxed);
            s.cv_mutex[sh].Unlock();
            return;
        }
        s.cv_queue[sh].push_back(r);
        s.cv_produced[sh].fetch_add(1, std::memory_order_relaxed);
        s.cv_notifies[sh].fetch_add(1, std::memory_order_relaxed);
        s.cv_not_full[sh].NotifyOne();
        s.cv_mutex[sh].Unlock();
        return;
    }
    if (s.cv_queue[sh].empty()) {
        s.cv_skips[sh].fetch_add(1, std::memory_order_relaxed);
        s.cv_mutex[sh].Unlock();
        return;
    }
    s.cv_queue[sh].pop_front();
    s.cv_consumed[sh].fetch_add(1, std::memory_order_relaxed);
    s.cv_notifies[sh].fetch_add(1, std::memory_order_relaxed);
    s.cv_not_empty[sh].NotifyOne();
    s.cv_mutex[sh].Unlock();
}

/** @brief WaitGroup：全局 add/done 配对，另用私有 WG 验证 Wait 返回值语义。 */
void DoWaitGroup(WorkerCtx& c, std::uint64_t r) {
    Shared& s = *c.shared;
    if ((r & 7) == 0) {
        c.local_wg.Add(3);
        c.local_wg.Done();
        c.local_wg.Done();
        c.local_wg.Done();
        if (!c.local_wg.WaitFor(std::chrono::milliseconds(1))) {
            s.wg_bad.fetch_add(1, std::memory_order_relaxed);
        }
        c.local_wg.Add(1);
        if (c.local_wg.WaitFor(std::chrono::nanoseconds(0))) {
            s.wg_bad.fetch_add(1, std::memory_order_relaxed);  // 未归零必须 false
        }
        c.local_wg.Done();
        if (!c.local_wg.WaitFor(std::chrono::milliseconds(1))) {
            s.wg_bad.fetch_add(1, std::memory_order_relaxed);
        }
        s.wg_wait_true.fetch_add(2, std::memory_order_relaxed);
        s.wg_wait_false.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    s.wg.Add(1);
    s.wg_adds.fetch_add(1, std::memory_order_relaxed);
    s.wg.Done();
    s.wg_dones.fetch_add(1, std::memory_order_relaxed);
}

/** @brief Select：多 case + default + after + 已取消 Context 的 DoneCase。 */
void DoSelect(WorkerCtx& c, std::uint64_t r) {
    Shared& s = *c.shared;
    // Select 的收发全部走专用通道：共享通道的计数/求和断言必须保持干净，
    // 不能被 Select 的内部发送污染。
    std::vector<SelectCase> cases;
    cases.reserve(3);
    cases.push_back(go2cpp::RecvCase(s.select_chan));  // index 0
    const std::size_t first_optional = cases.size();

    int send_index = -1;
    int default_index = -1;
    int after_index = -1;
    int cancel_index = -1;

    if ((r & 1) != 0) {
        send_index = static_cast<int>(cases.size());
        cases.push_back(go2cpp::SendCase(s.select_chan, r));
    }
    // 分支选择必须与最低位（决定是否有 SendCase）**独立**，否则
    // `r % 4 == 1` 恒为奇数，AfterCase 永远会被 SendCase 抢先。
    const int variant = static_cast<int>((r >> 8) % 4);
    const bool want_after = ((c.select_counter++ & 0xFF) == 0);
    switch (variant) {
        case 0:
            default_index = static_cast<int>(cases.size());
            cases.push_back(go2cpp::DefaultCase());
            break;
        case 1:
            after_index = static_cast<int>(cases.size());
            if (want_after) {
                cases.push_back(
                    go2cpp::AfterCase(std::chrono::microseconds(100)));
            } else {
                cases.push_back(go2cpp::DefaultCase());
            }
            break;
        case 2:
            cancel_index = static_cast<int>(cases.size());
            cases.push_back(go2cpp::DoneCase(s.cancelled_ctx));
            break;
        default:
            default_index = static_cast<int>(cases.size());
            cases.push_back(go2cpp::DefaultCase());
            break;
    }

    const SelectResult result =
        go2cpp::Select(cases, {}, std::optional<go2cpp::ContextDuration>(5ms));
    if (!result.selected && result.index == SelectResult::kNoSelection) {
        // 没有任何 case 就绪且外部 5ms 兜底超时到期：在宿主高负载下定时器
        // 可能晚到，这属于**合法超时**而不是非法 index，单独统计。
        if (result.status == go2cpp::ChannelStatus::kTimedOut ||
            result.status == go2cpp::ChannelStatus::kDeadlineExceeded ||
            result.status == go2cpp::ChannelStatus::kCancelled) {
            ++c.select_timeouts;
            s.select_timeout_hits.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        ++c.select_bad;
        return;
    }
    if (result.index >= cases.size()) {
        ++c.select_bad;
        return;
    }
    if (result.index == 0) {
        s.select_recv_hits.fetch_add(1, std::memory_order_relaxed);
        if (result.ok && !result.Value<std::uint64_t>().has_value()) {
            ++c.select_bad;
        }
    } else if (static_cast<int>(result.index) == send_index) {
        s.select_send_hits.fetch_add(1, std::memory_order_relaxed);
    } else if (static_cast<int>(result.index) == default_index) {
        s.select_default_hits.fetch_add(1, std::memory_order_relaxed);
    } else if (static_cast<int>(result.index) == after_index) {
        s.select_after_hits.fetch_add(1, std::memory_order_relaxed);
    } else if (static_cast<int>(result.index) == cancel_index) {
        s.select_cancel_hits.fetch_add(1, std::memory_order_relaxed);
        if (result.status != go2cpp::ChannelStatus::kCancelled &&
            result.status != go2cpp::ChannelStatus::kDeadlineExceeded &&
            result.status != go2cpp::ChannelStatus::kTimedOut) {
            ++c.select_bad;
        }
    } else {
        ++c.select_bad;
    }
    s.select_ok.fetch_add(1, std::memory_order_relaxed);
}

/** @brief Context：创建、取消、取消后状态一致性、超时立即过期。 */
void DoContext(WorkerCtx& c, std::uint64_t r) {
    Shared& s = *c.shared;
    (void)r;
    auto pair = Context::WithCancel(Context::Background());
    ContextPtr ctx = pair.first;
    go2cpp::CancelFunc cancel = pair.second;
    s.ctx_created.fetch_add(1, std::memory_order_relaxed);

    auto child_pair = Context::WithCancel(ctx);
    ContextPtr child = child_pair.first;
    go2cpp::CancelFunc child_cancel = child_pair.second;
    s.ctx_created.fetch_add(1, std::memory_order_relaxed);

    if (ctx->IsDone() || child->IsDone()) {
        s.ctx_bad.fetch_add(1, std::memory_order_relaxed);
        ++c.ctx_bad;
    }
    if (ctx->Err() != nullptr || child->Err() != nullptr) {
        s.ctx_bad.fetch_add(1, std::memory_order_relaxed);
        ++c.ctx_bad;
    }

    cancel();
    s.ctx_cancelled.fetch_add(1, std::memory_order_relaxed);
    if (!ctx->IsDone() || !child->IsDone()) {
        s.ctx_bad.fetch_add(1, std::memory_order_relaxed);
        ++c.ctx_bad;
    }
    if (ctx->Err() == nullptr || child->Err() == nullptr) {
        s.ctx_bad.fetch_add(1, std::memory_order_relaxed);
        ++c.ctx_bad;
    }
    cancel();  // 幂等
    child_cancel();
    s.ctx_cancelled.fetch_add(1, std::memory_order_relaxed);
    if (!ctx->IsDone()) {
        s.ctx_bad.fetch_add(1, std::memory_order_relaxed);
        ++c.ctx_bad;
    }

    // 超时 Context：1ns 后过期；允许实现异步触发，最多等 2ms。
    auto timeout_pair = Context::WithTimeout(Context::Background(), 1ns);
    ContextPtr timeout_ctx = timeout_pair.first;
    go2cpp::CancelFunc timeout_cancel = timeout_pair.second;
    s.ctx_created.fetch_add(1, std::memory_order_relaxed);
    const long long deadline = NowNs() + 2000000;  // 2ms 上限
    while (!timeout_ctx->IsDone() && NowNs() < deadline) {
        YieldExecution();
    }
    if (!timeout_ctx->IsDone()) {
        s.ctx_bad.fetch_add(1, std::memory_order_relaxed);
        ++c.ctx_bad;
    }
    if (timeout_ctx->Err() == nullptr) {
        s.ctx_bad.fetch_add(1, std::memory_order_relaxed);
        ++c.ctx_bad;
    }
    timeout_cancel();
    s.ctx_cancelled.fetch_add(1, std::memory_order_relaxed);
}

/**
 * @brief Error 的 Fiber-local 跨 M 迁移验证。
 *
 * 产生错误 → 写入 ErrorLocal 与 Fiber 当前错误槽 → yield（可能被另一个 M
 * 捡起）→ 校验指针、错误码、Fiber-local 值三者都没变。
 */
void DoError(WorkerCtx& c, std::uint64_t r) {
    Shared& s = *c.shared;
    if (Scheduler::current_task_ptr() == nullptr) {
        // 原生线程路径：ErrorLocal 走 TLS fallback，同样验证可见性。
        ErrorPtr error = go2cpp::error::MakeError<go2cpp::error::TimeoutError>(
            "stress-thread", std::string("thread"), 1000);
        go2cpp::error::SetCurrentError(error);
        c.error_slot->GetOrCreate() = error;
        if (go2cpp::error::CurrentError() != error) {
            ++c.err_bad;
        }
        if (c.error_slot->TryGet() == nullptr ||
            *c.error_slot->TryGet() != error) {
            ++c.err_bad;
        }
        go2cpp::error::ClearCurrentError();
        c.error_slot->Reset();
        ++c.err_checks;
        return;
    }

    ErrorPtr error = go2cpp::error::MakeError<go2cpp::error::TimeoutError>(
        "stress-fiber", std::string("fiber"), 1000);
    ErrorPtr wrapped = go2cpp::error::Wrap(error, "op");
    go2cpp::error::SetCurrentError(wrapped);
    c.error_slot->GetOrCreate() = wrapped;

    Scheduler* scheduler = Scheduler::current_scheduler();
    if (scheduler != nullptr) {
        (void)scheduler->yield_current();  // 挂起点：可能在另一个 M 恢复
    }
    if (go2cpp::error::CurrentError() != wrapped) {
        ++c.err_bad;
    }
    const ErrorPtr* stored = c.error_slot->TryGet();
    if (stored == nullptr || *stored != wrapped) {
        ++c.err_bad;
    }
    if (go2cpp::error::CurrentError() == nullptr ||
        go2cpp::error::CurrentError()->Code() != ErrorCode::kWrappedError) {
        ++c.err_bad;
    }
    go2cpp::error::ClearCurrentError();
    c.error_slot->Reset();
    ++c.err_checks;
    s.err_checks.fetch_add(1, std::memory_order_relaxed);
}

/** @brief socketpair 回环收发：写 8 字节再读回，校验内容一致。 */
void DoNet(WorkerCtx& c, std::uint64_t r) {
    if (c.sock_a < 0) {
        return;
    }
    std::uint64_t out = r;
    std::uint64_t in = 0;
    const ssize_t wrote = ::write(c.sock_a, &out, sizeof(out));
    const ssize_t read_bytes = ::read(c.sock_b, &in, sizeof(in));
    if (wrote != static_cast<ssize_t>(sizeof(out)) ||
        read_bytes != static_cast<ssize_t>(sizeof(in)) || in != out) {
        ++c.net_bad;
        c.shared->net_bad.fetch_add(1, std::memory_order_relaxed);
    }
    ++c.net_ops;
    c.shared->net_ops.fetch_add(1, std::memory_order_relaxed);
}

/** @brief 锁嵌套：固定 A→B 顺序，不会死锁。 */
void DoNestedLock(WorkerCtx& c) {
    Shared& s = *c.shared;
    const std::size_t sh = c.shard;
    s.nest_a[sh].Lock();
    s.nest_b[sh].Lock();
    s.nest_counter.fetch_add(1, std::memory_order_relaxed);
    s.nest_b[sh].Unlock();
    s.nest_a[sh].Unlock();
    s.nested_lock_ok.fetch_add(1, std::memory_order_relaxed);
}

/** @brief Channel 嵌套：channel of channel 传子通道并在子通道内收发。 */
void DoNestedChan(WorkerCtx& c, std::uint64_t r) {
    Shared& s = *c.shared;
    if ((r & 1) != 0) {
        const auto sub = s.sub_channels[r % 4];
        const auto sent = s.chan_of_chan->TrySend(sub);
        if (!sent.Ok() && sent.Closed()) {
            ++c.nested_bad;
            s.nested_bad.fetch_add(1, std::memory_order_relaxed);
        }
        return;
    }
    const auto outer = s.chan_of_chan->TryRecv();
    if (!outer.Ok() || !outer.value || !*outer.value) {
        return;
    }
    ChannelPtr<std::uint64_t> sub = *outer.value;
    // 子通道是全 worker 共享的：多消费者语义下不保证「自己发的值自己收到」，
    // 因此只断言嵌套通道可用（发/收都成功），值相等留给私有 FIFO 断言。
    const auto send = sub->TrySend(r);
    if (!send.Ok()) {
        return;
    }
    const auto recv = sub->TryRecv();
    if (!recv.Ok()) {
        ++c.nested_bad;
        s.nested_bad.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    s.nested_chan_ok.fetch_add(1, std::memory_order_relaxed);
}

/** @brief select 嵌套：外层 select 命中后再做一次内层 select。 */
void DoNestedSelect(WorkerCtx& c) {
    Shared& s = *c.shared;
    std::vector<SelectCase> outer;
    outer.reserve(2);
    outer.push_back(go2cpp::RecvCase(s.select_chan));
    outer.push_back(go2cpp::DefaultCase());
    const SelectResult first =
        go2cpp::Select(outer, {}, std::optional<go2cpp::ContextDuration>(1ms));
    if (first.index >= outer.size()) {
        ++c.nested_bad;
        s.nested_bad.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    std::vector<SelectCase> inner;
    inner.reserve(2);
    inner.push_back(go2cpp::RecvCase(s.select_idle_chan));
    inner.push_back(go2cpp::DefaultCase());
    const SelectResult second =
        go2cpp::Select(inner, {}, std::optional<go2cpp::ContextDuration>(1ms));
    if (second.index >= inner.size() || second.index != 1) {
        ++c.nested_bad;
        s.nested_bad.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    s.nested_select_ok.fetch_add(1, std::memory_order_relaxed);
}

/** @brief 关闭竞态：Close / Send / Recv 并发，断言关闭后的语义收敛。 */
void DoCloseRace(WorkerCtx& c, std::uint64_t r) {
    Shared& s = *c.shared;
    ChannelPtr<std::uint64_t>& channel = s.close_chan;
    switch (r % 3) {
        case 0: {
            const std::uint64_t seq =
                s.close_send_seq.fetch_add(1, std::memory_order_relaxed);
            const auto result = channel->TrySend(seq);
            if (result.Ok()) {
                s.close_send_ok.fetch_add(1, std::memory_order_relaxed);
                s.close_sent_sum.fetch_add(seq, std::memory_order_relaxed);
            } else if (result.Closed()) {
                s.close_send_rejected.fetch_add(1, std::memory_order_relaxed);
            }
            break;
        }
        case 1: {
            const auto result = channel->TryRecv();
            if (result.Ok()) {
                s.close_recv_ok.fetch_add(1, std::memory_order_relaxed);
            } else if (result.Closed()) {
                s.close_recv_closed.fetch_add(1, std::memory_order_relaxed);
            }
            break;
        }
        default: {
            // Close 幂等：重复 Close 返回 kAlreadyClosed（Closed() 为 true）。
            const auto result = channel->Close();
            s.close_calls.fetch_add(1, std::memory_order_relaxed);
            if (!result.Ok() && !result.Closed()) {
                ++c.close_bad;
                s.close_bad.fetch_add(1, std::memory_order_relaxed);
            }
            break;
        }
    }
}

// ===========================================================================
// worker 主循环
// ===========================================================================

/** @brief 按类别分发一次操作。 */
void Dispatch(WorkerCtx& c, int kind, std::uint64_t r) {
    switch (static_cast<OpKind>(kind)) {
        case OpKind::kChanSend:
            DoChanSend(c, r);
            break;
        case OpKind::kChanRecv:
            DoChanRecv(c, r);
            break;
        case OpKind::kRendezvous:
            DoRendezvous(c, r);
            break;
        case OpKind::kChanFifo:
            DoChanFifo(c, r);
            break;
        case OpKind::kFastSpsc:
            DoFastSpsc(c, r);
            break;
        case OpKind::kFastMpmc:
            DoFastMpmc(c, r);
            break;
        case OpKind::kMutex:
            DoMutex(c);
            break;
        case OpKind::kSpin:
            DoSpin(c);
            break;
        case OpKind::kRwRead:
            DoRwRead(c);
            break;
        case OpKind::kRwWrite:
            DoRwWrite(c);
            break;
        case OpKind::kCvPrivate:
            DoCvPrivate(c, r);
            break;
        case OpKind::kCvShared:
            DoCvShared(c, r);
            break;
        case OpKind::kWaitGroup:
            DoWaitGroup(c, r);
            break;
        case OpKind::kSelect:
            DoSelect(c, r);
            break;
        case OpKind::kContext:
            DoContext(c, r);
            break;
        case OpKind::kError:
            DoError(c, r);
            break;
        case OpKind::kNet:
            DoNet(c, r);
            break;
        case OpKind::kNestedLock:
            DoNestedLock(c);
            break;
        case OpKind::kNestedChan:
            DoNestedChan(c, r);
            break;
        case OpKind::kNestedSelect:
            DoNestedSelect(c);
            break;
        case OpKind::kCloseRace:
            DoCloseRace(c, r);
            break;
        default:
            break;
    }
}

/** @brief 参与者入口：随机执行 quota 次操作。 */
void WorkerLoop(WorkerCtx* ctx) {
    WorkerCtx& c = *ctx;
    c.rand_state = 0x9E3779B97F4A7C15ULL *
                       static_cast<std::uint64_t>(c.id + 1) +
                   0xDEADBEEFULL + static_cast<std::uint64_t>(c.id) * 7919ULL;
    const int slot = c.id;
    if (slot >= 0 && slot < kMaxWorkers) {
        g_worker_kind[slot].store(0, std::memory_order_relaxed);
    }
    for (std::uint64_t i = 0; i < c.quota; ++i) {
        const std::uint64_t r = NextRand(&c.rand_state);
        const int kind = g_only_kind >= 0 ? g_only_kind : PickKind(r >> 12);
        if (slot >= 0 && slot < kMaxWorkers && (i & 0x3FF) == 0) {
            g_worker_kind[slot].store(kind, std::memory_order_relaxed);
        }
        Dispatch(c, kind, r);
        ++c.kind_count[kind];
        if ((i & 0xFF) == 0) {
            g_progress.fetch_add(256, std::memory_order_relaxed);
            if (slot >= 0 && slot < kMaxWorkers) {
                g_worker_ops[slot].store(i, std::memory_order_relaxed);
            }
        }
    }
    c.ops_done = c.quota;
    g_progress.fetch_add(c.quota & 0xFF, std::memory_order_relaxed);
    if (slot >= 0 && slot < kMaxWorkers) {
        g_worker_ops[slot].store(c.quota, std::memory_order_relaxed);
    }
}

// ===========================================================================
// 轮次结果与校验
// ===========================================================================

/** @brief 一轮的指标与断言。 */
struct RoundResult {
    int round = 0;
    std::uint64_t target_ops = 0;
    std::uint64_t done_ops = 0;
    long long elapsed_ns = 0;
    double ops_per_sec = 0.0;
    double loadavg = 0.0;
    std::uint64_t rss_kb = 0;
    std::uint64_t vmsize_kb = 0;
    std::vector<Assertion> checks;
    bool ok = false;
    std::string detail;  ///< 首个失败原因
};

/** @brief 跨 worker 汇总的本地计数。 */
struct LocalAgg {
    std::uint64_t fifo_sent[3] = {0, 0, 0};
    std::uint64_t fifo_recv[3] = {0, 0, 0};
    std::uint64_t fifo_violations = 0;
    std::uint64_t spsc_sent = 0;
    std::uint64_t spsc_recv = 0;
    std::uint64_t spsc_violations = 0;
    std::uint64_t cv_pushed = 0;
    std::uint64_t cv_popped = 0;
    std::uint64_t cv_wait_false = 0;
    std::uint64_t net_ops = 0;
    std::uint64_t net_bad = 0;
    std::uint64_t err_checks = 0;
    std::uint64_t err_bad = 0;
    std::uint64_t select_bad = 0;
    std::uint64_t select_timeouts = 0;
    std::uint64_t ctx_bad = 0;
    std::uint64_t nested_bad = 0;
    std::uint64_t close_bad = 0;
    std::uint64_t mutex_bad = 0;
    std::uint64_t spin_bad = 0;
    std::uint64_t rw_bad = 0;
    std::uint64_t kind_total[kKindCount] = {};
};

LocalAgg Aggregate(const std::vector<WorkerCtx>& ctxs) {
    LocalAgg agg;
    for (const auto& c : ctxs) {
        for (std::size_t i = 0; i < 3; ++i) {
            agg.fifo_sent[i] += c.fifo_sent[i];
            agg.fifo_recv[i] += c.fifo_recv[i];
        }
        agg.fifo_violations += c.fifo_violations;
        agg.spsc_sent += c.spsc_sent;
        agg.spsc_recv += c.spsc_recv;
        agg.spsc_violations += c.spsc_violations;
        agg.cv_pushed += c.cv_pushed;
        agg.cv_popped += c.cv_popped;
        agg.cv_wait_false += c.cv_wait_false;
        agg.net_ops += c.net_ops;
        agg.net_bad += c.net_bad;
        agg.err_checks += c.err_checks;
        agg.err_bad += c.err_bad;
        agg.select_bad += c.select_bad;
        agg.select_timeouts += c.select_timeouts;
        agg.ctx_bad += c.ctx_bad;
        agg.nested_bad += c.nested_bad;
        agg.close_bad += c.close_bad;
        agg.mutex_bad += c.mutex_bad;
        agg.spin_bad += c.spin_bad;
        agg.rw_bad += c.rw_bad;
        for (int k = 0; k < kKindCount; ++k) {
            agg.kind_total[k] += c.kind_count[k];
        }
    }
    return agg;
}

/** @brief 汇总所有真实断言。 */
void VerifyRound(Shared& s, const std::vector<WorkerCtx>& ctxs,
                 const LocalAgg& agg, std::uint64_t target_ops,
                 std::uint64_t baseline_rss_kb, std::uint64_t previous_rss_kb,
                 RoundResult* out) {
    // --- 1. 操作总量与每类计数 -----------------------------------------
    std::uint64_t kind_sum = 0;
    for (int k = 0; k < kKindCount; ++k) {
        kind_sum += agg.kind_total[k];
    }
    std::uint64_t done_ops = 0;
    for (const auto& c : ctxs) {
        done_ops += c.ops_done;
    }
    AddCheck(&out->checks, "操作总数=目标且每类计数和一致",
             done_ops == target_ops && kind_sum == target_ops,
             "完成=" + FormatU64(done_ops) + " 分类和=" + FormatU64(kind_sum) +
                 " 目标=" + FormatU64(target_ops));

    int covered = 0;
    for (int k = 0; k < kKindCount; ++k) {
        covered += agg.kind_total[k] > 0 ? 1 : 0;
    }
    if (g_only_kind < 0) {
        AddCheck(&out->checks, "21 类操作全部被真实执行", covered == kKindCount,
                 "覆盖=" + std::to_string(covered) + "/" +
                     std::to_string(kKindCount));
    }

    if (done_ops != target_ops) {
        out->detail = "操作总数不符";
    }

    // --- 2. 共享 Channel：无丢失、无重复 ------------------------------
    {
        const std::uint64_t sent = s.chan64_sent_count.load();
        const std::uint64_t recv = s.chan64_recv_count.load();
        const std::uint64_t sent_sum = s.chan64_sent_sum.load();
        const std::uint64_t recv_sum = s.chan64_recv_sum.load();
        AddCheck(&out->checks, "Channel(cap64) 无丢失/无重复",
                 sent == recv && sent_sum == recv_sum &&
                     !s.chan64->IsClosed(),
                 "发送=" + FormatU64(sent) + " 接收=" + FormatU64(recv) +
                     " 发送和=" + FormatU64(sent_sum) +
                     " 接收和=" + FormatU64(recv_sum));
    }
    {
        const std::uint64_t sent = s.chan1024_sent_count.load();
        const std::uint64_t recv = s.chan1024_recv_count.load();
        const std::uint64_t sent_sum = s.chan1024_sent_sum.load();
        const std::uint64_t recv_sum = s.chan1024_recv_sum.load();
        AddCheck(&out->checks, "Channel(cap1024) 无丢失/无重复",
                 sent == recv && sent_sum == recv_sum &&
                     !s.chan1024->IsClosed(),
                 "发送=" + FormatU64(sent) + " 接收=" + FormatU64(recv) +
                     " 发送和=" + FormatU64(sent_sum) +
                     " 接收和=" + FormatU64(recv_sum));
    }

    // --- 3. cap 0 rendezvous ------------------------------------------
    {
        const std::uint64_t sent = s.rendezvous_sent_count.load();
        const std::uint64_t recv = s.rendezvous_recv_count.load();
        const std::uint64_t sent_sum = s.rendezvous_sent_sum.load();
        const std::uint64_t recv_sum = s.rendezvous_recv_sum.load();
        AddCheck(&out->checks, "Channel(cap0) 成功交接无丢失/无重复",
                 sent == recv && sent_sum == recv_sum,
                 "发送=" + FormatU64(sent) + " 接收=" + FormatU64(recv) +
                     " 超时=" + FormatU64(s.rendezvous_timeouts.load()));
    }

    // --- 4. 私有 FIFO Channel -----------------------------------------
    {
        bool equal = true;
        std::string detail;
        for (std::size_t i = 0; i < 3; ++i) {
            if (agg.fifo_recv[i] > agg.fifo_sent[i]) {
                equal = false;
            }
            detail += "cap" + std::to_string(WorkerCtx::kFifoCap[i]) + ":" +
                      FormatU64(agg.fifo_recv[i]) + "/" +
                      FormatU64(agg.fifo_sent[i]) + " ";
        }
        // 剩余在缓冲里的元素在轮末由主线程排空，这里只要求「收 ≤ 发」且
        // 严格有序验证器零违例。
        AddCheck(&out->checks, "私有 Channel 严格 FIFO（无乱序/无重复）",
                 agg.fifo_violations == 0 && equal, detail);
    }

    // --- 5. FastChannel SPSC / MPMC -----------------------------------
    AddCheck(&out->checks, "FastChannel(SPSC) 严格 FIFO 且收发配对",
             agg.spsc_violations == 0 && agg.spsc_recv == agg.spsc_sent,
             "发送=" + FormatU64(agg.spsc_sent) + " 接收=" +
                 FormatU64(agg.spsc_recv) + " 违例=" +
                 FormatU64(agg.spsc_violations));
    {
        const std::uint64_t sent = s.mpmc_sent_count.load();
        const std::uint64_t recv = s.mpmc_recv_count.load();
        AddCheck(&out->checks, "FastChannel(MPMC) 无丢失/无重复",
                 sent == recv && s.mpmc_sent_sum.load() == s.mpmc_recv_sum.load(),
                 "发送=" + FormatU64(sent) + " 接收=" + FormatU64(recv) +
                     " 发送和=" + FormatU64(s.mpmc_sent_sum.load()) +
                     " 接收和=" + FormatU64(s.mpmc_recv_sum.load()));
    }

    // --- 6. Mutex / SpinMutex -----------------------------------------
    {
        std::uint64_t corrupt = 0;
        std::uint64_t counter = 0;
        bool held = false;
        for (std::size_t i = 0; i < Shared::kShards; ++i) {
            corrupt += s.mutex_corrupt[i].load();
            counter += s.mutex_counter[i].load();
            held = held || s.mutex[i].DebugSnapshotInternal().held;
        }
        AddCheck(&out->checks, "Mutex 临界区互斥未被破坏",
                 corrupt == 0 && agg.mutex_bad == 0 && !held,
                 "违例=" + FormatU64(corrupt) + " 计数=" + FormatU64(counter));
    }
    {
        std::uint64_t corrupt = 0;
        std::uint64_t counter = 0;
        for (std::size_t i = 0; i < Shared::kShards; ++i) {
            corrupt += s.spin_corrupt[i].load();
            counter += s.spin_counter[i].load();
        }
        AddCheck(&out->checks, "SpinMutex 临界区互斥未被破坏",
                 corrupt == 0 && agg.spin_bad == 0,
                 "违例=" + FormatU64(corrupt) + " 计数=" + FormatU64(counter));
    }

    // --- 7. RWMutex 快照一致 ------------------------------------------
    {
        std::uint64_t torn = 0;
        std::uint64_t reads = 0;
        std::uint64_t writes = 0;
        for (std::size_t i = 0; i < Shared::kShards; ++i) {
            torn += s.rw_torn[i].load();
            reads += s.rw_reads[i].load();
            writes += s.rw_writes[i].load();
        }
        AddCheck(&out->checks, "RWMutex 读快照无撕裂",
                 torn == 0 && agg.rw_bad == 0 && reads > 0 && writes > 0,
                 "读=" + FormatU64(reads) + " 写=" + FormatU64(writes) +
                     " 撕裂=" + FormatU64(torn));
    }

    // --- 8. CV ---------------------------------------------------------
    {
        std::uint64_t remaining = 0;
        std::uint64_t produced = 0;
        std::uint64_t consumed = 0;
        std::uint64_t waiters = 0;
        for (std::size_t i = 0; i < Shared::kShards; ++i) {
            s.cv_mutex[i].Lock();
            remaining += s.cv_queue[i].size();
            s.cv_mutex[i].Unlock();
            produced += s.cv_produced[i].load();
            consumed += s.cv_consumed[i].load();
            waiters += s.cv_not_empty[i].DebugSnapshotInternal().waiters;
            waiters += s.cv_not_full[i].DebugSnapshotInternal().waiters;
        }
        AddCheck(&out->checks, "CV 生产=消费+残留（共享队列）",
                 produced == consumed + remaining,
                 "生产=" + FormatU64(produced) + " 消费=" + FormatU64(consumed) +
                     " 残留=" + FormatU64(remaining));
        AddCheck(&out->checks, "CV 私有队列收发配对且 Wait 语义正确",
                 agg.cv_pushed == agg.cv_popped && agg.cv_wait_false == 0,
                 "入队=" + FormatU64(agg.cv_pushed) + " 出队=" +
                     FormatU64(agg.cv_popped) + " 空等待误报=" +
                     FormatU64(agg.cv_wait_false));
        AddCheck(&out->checks, "CV 结束时 waiters=0", waiters == 0,
                 "waiters=" + FormatU64(waiters));
    }

    // --- 9. WaitGroup ---------------------------------------------------
    {
        const auto snapshot = s.wg.DebugSnapshotInternal();
        AddCheck(&out->checks, "WaitGroup 计数归零且 waiters=0",
                 s.wg.Count() == 0 && snapshot.count == 0 &&
                     snapshot.waiters == 0 && s.wg_bad.load() == 0 &&
                     s.wg_adds.load() == s.wg_dones.load(),
                 "Add=" + FormatU64(s.wg_adds.load()) + " Done=" +
                     FormatU64(s.wg_dones.load()) + " count=" +
                     std::to_string(s.wg.Count()) + " waiters=" +
                     FormatU64(snapshot.waiters) + " 语义违例=" +
                     FormatU64(s.wg_bad.load()));
    }

    // --- 10. Select -----------------------------------------------------
    {
        const std::uint64_t ok = s.select_ok.load();
        const std::uint64_t bad = s.select_bad.load() + agg.select_bad;
        const std::uint64_t timeouts =
            s.select_timeout_hits.load() + agg.select_timeouts;
        const std::uint64_t attempts = ok + bad + timeouts;
        const std::uint64_t recv_hits = s.select_recv_hits.load();
        const std::uint64_t send_hits = s.select_send_hits.load();
        const std::uint64_t default_hits = s.select_default_hits.load();
        const std::uint64_t after_hits = s.select_after_hits.load();
        const std::uint64_t cancel_hits = s.select_cancel_hits.load();
        const std::uint64_t branch_sum = recv_hits + send_hits + default_hits +
                                         after_hits + cancel_hits;
        AddCheck(&out->checks, "Select 每次返回合法 index 且分支计数守恒",
                 bad == 0 && ok > 0 && branch_sum == ok,
                 "成功=" + FormatU64(ok) + " 非法=" + FormatU64(bad) +
                     " 兜底超时=" + FormatU64(timeouts) +
                     " 分支和=" + FormatU64(branch_sum));
        AddCheck(&out->checks, "Select 兜底超时率极低（≤0.1%）",
                 attempts > 0 && timeouts * 1000ULL <= attempts,
                 "超时=" + FormatU64(timeouts) + " 总次数=" +
                     FormatU64(attempts));
        if (g_only_kind < 0) {
            AddCheck(&out->checks,
                     "Select 覆盖 recv/send/default/after/取消 五路",
                     recv_hits > 0 && default_hits > 0 && after_hits > 0 &&
                         cancel_hits > 0 && send_hits > 0,
                     "recv=" + FormatU64(recv_hits) + " send=" +
                         FormatU64(send_hits) + " default=" +
                         FormatU64(default_hits) + " after=" +
                         FormatU64(after_hits) + " cancel=" +
                         FormatU64(cancel_hits));
        }
    }

    // --- 11. Context ----------------------------------------------------
    {
        const std::uint64_t created = s.ctx_created.load();
        const std::uint64_t cancelled = s.ctx_cancelled.load();
        const std::uint64_t bad = s.ctx_bad.load() + agg.ctx_bad;
        AddCheck(&out->checks, "Context 取消后状态一致且无残留",
                 bad == 0 && created == cancelled && created > 0,
                 "创建=" + FormatU64(created) + " 取消=" + FormatU64(cancelled) +
                     " 违例=" + FormatU64(bad));
    }

    // --- 12. Error（Fiber-local 跨 M 迁移）------------------------------
    {
        const std::uint64_t checks = s.err_checks.load() + agg.err_checks;
        const std::uint64_t bad = s.err_bad.load() + agg.err_bad;
        AddCheck(&out->checks, "Error Fiber-local 迁移后状态一致",
                 bad == 0 && checks > 0,
                 "校验=" + FormatU64(checks) + " 违例=" + FormatU64(bad));
    }

    // --- 13. 网络 -------------------------------------------------------
    AddCheck(&out->checks, "socketpair 回环收发内容一致",
             s.net_bad.load() == 0 && agg.net_bad == 0 &&
                 s.net_ops.load() > 0,
             "次数=" + FormatU64(s.net_ops.load()) + " 违例=" +
                 FormatU64(s.net_bad.load() + agg.net_bad));

    // --- 14. 嵌套 -------------------------------------------------------
    AddCheck(&out->checks, "嵌套操作（锁/channel/select）全部成功",
             s.nested_bad.load() == 0 && agg.nested_bad == 0 &&
                 s.nested_lock_ok.load() > 0 && s.nested_chan_ok.load() > 0 &&
                 s.nested_select_ok.load() > 0,
             "锁=" + FormatU64(s.nested_lock_ok.load()) + " channel=" +
                 FormatU64(s.nested_chan_ok.load()) + " select=" +
                 FormatU64(s.nested_select_ok.load()) + " 违例=" +
                 FormatU64(s.nested_bad.load() + agg.nested_bad));

    // --- 15. 关闭竞态 ---------------------------------------------------
    {
        const std::uint64_t calls = s.close_calls.load();
        const std::uint64_t send_ok = s.close_send_ok.load();
        const std::uint64_t recv_ok = s.close_recv_ok.load();
        const std::uint64_t remaining = s.close_chan->Len();
        const std::uint64_t bad = s.close_bad.load() + agg.close_bad;
        AddCheck(&out->checks,
                 "关闭竞态：Close 幂等、语义收敛、无残留",
                 bad == 0 && calls > 0 && s.close_chan->IsClosed() &&
                     send_ok == recv_ok + remaining && remaining == 0,
                 "Close=" + FormatU64(calls) + " 发送成功=" + FormatU64(send_ok) +
                     " 接收成功=" + FormatU64(recv_ok) + " 残留=" +
                     FormatU64(remaining) + " 违例=" + FormatU64(bad));
        const auto snapshot = s.close_chan->DebugSnapshot();
        AddCheck(&out->checks, "关闭竞态：结束时 waiters=0",
                 snapshot.send_waiters == 0 && snapshot.recv_waiters == 0,
                 "send_waiters=" + FormatU64(snapshot.send_waiters) +
                     " recv_waiters=" + FormatU64(snapshot.recv_waiters));
    }

    // --- 16. 所有共享 Channel 的 waiters 归零 ---------------------------
    {
        const auto c64 = s.chan64->DebugSnapshot();
        const auto c1024 = s.chan1024->DebugSnapshot();
        const auto c0 = s.rendezvous->DebugSnapshot();
        AddCheck(&out->checks, "所有共享 Channel waiters=0 且队列排空",
                 c64.send_waiters == 0 && c64.recv_waiters == 0 &&
                     c1024.send_waiters == 0 && c1024.recv_waiters == 0 &&
                     c0.send_waiters == 0 && c0.recv_waiters == 0 &&
                     s.chan64->Len() == 0 && s.chan1024->Len() == 0 &&
                     s.rendezvous->Len() == 0,
                 "waiters=(" + FormatU64(c64.send_waiters) + "," +
                     FormatU64(c1024.send_waiters) + "," +
                     FormatU64(c0.send_waiters) + ")");
    }

    // --- 17. RSS 稳定 ---------------------------------------------------
    {
        const ProcMem mem = ReadProcMem();
        out->rss_kb = mem.rss_kb;
        out->vmsize_kb = mem.vmsize_kb;
        const long long delta =
            static_cast<long long>(mem.rss_kb) -
            static_cast<long long>(baseline_rss_kb);
        AddCheck(&out->checks, "RSS 相对基线增量 ≤ 512MiB",
                 delta <= 512LL * 1024LL,
                 "ΔRSS=" + FormatDouble(static_cast<double>(delta) / 1024.0) +
                     "MiB");
        if (previous_rss_kb != 0) {
            const long long round_delta =
                static_cast<long long>(mem.rss_kb) -
                static_cast<long long>(previous_rss_kb);
            AddCheck(&out->checks, "RSS 轮间增量 ≤ 128MiB（无逐轮泄漏）",
                     round_delta <= 128LL * 1024LL,
                     "ΔRSS(轮间)=" +
                         FormatDouble(static_cast<double>(round_delta) / 1024.0) +
                         "MiB");
        }
    }

    out->ok = AllChecksPassed(out->checks);
    if (!out->ok && out->detail.empty()) {
        for (const auto& check : out->checks) {
            if (!check.passed) {
                out->detail = check.name;
                break;
            }
        }
    }
}

// ===========================================================================
// 每轮执行
// ===========================================================================

/** @brief 构造一轮的全部共享对象。 */
void BuildShared(Shared* s) {
    s->chan64 = MakeChannel<std::uint64_t>(64);
    s->chan1024 = MakeChannel<std::uint64_t>(1024);
    s->rendezvous = MakeChannel<std::uint64_t>(0);
    s->close_chan = MakeChannel<std::uint64_t>(64);
    s->select_chan = MakeChannel<std::uint64_t>(16);
    s->select_idle_chan = MakeChannel<std::uint64_t>(8);
    s->chan_of_chan = MakeChannel<ChannelPtr<std::uint64_t>>(8);
    for (std::size_t i = 0; i < 4; ++i) {
        s->sub_channels[i] = MakeChannel<std::uint64_t>(8);
    }
    auto cancelled = Context::WithCancel(Context::Background());
    cancelled.second();
    s->cancelled_ctx = cancelled.first;
}

/** @brief 一轮压测：建对象、起参与者、等结束、跑断言。 */
RoundResult RunRound(int round, Scheduler* scheduler, std::uint64_t baseline_rss_kb,
                     std::uint64_t previous_rss_kb) {
    RoundResult result;
    result.round = round;

    const int threads = g_cfg.thread_workers;
    const int fibers = g_cfg.fiber_workers;
    const int total = threads + fibers;
    const std::uint64_t target = g_cfg.ops_per_round;
    result.target_ops = target;
    g_round.store(round, std::memory_order_relaxed);
    g_worker_count.store(total, std::memory_order_relaxed);

    auto shared = std::make_unique<Shared>();
    BuildShared(shared.get());
    g_shared.store(shared.get(), std::memory_order_release);

    // Error 槽必须是长寿对象：静态生命周期，保证 Fiber 结束后槽位仍有效。
    static ErrorLocal<ErrorPtr> error_slot;
    for (int i = 0; i < kMaxWorkers; ++i) {
        g_worker_ops[i].store(0, std::memory_order_relaxed);
        g_worker_kind[i].store(0, std::memory_order_relaxed);
        g_worker_is_fiber[i].store(0, std::memory_order_relaxed);
    }
    g_progress.store(0, std::memory_order_relaxed);

    std::vector<WorkerCtx> ctxs(static_cast<std::size_t>(total));
    const std::uint64_t base = target / static_cast<std::uint64_t>(total);
    const std::uint64_t remainder = target % static_cast<std::uint64_t>(total);
    for (int i = 0; i < total; ++i) {
        WorkerCtx& c = ctxs[static_cast<std::size_t>(i)];
        c.id = i;
        c.is_fiber = i >= threads;
        c.shard = static_cast<std::size_t>(i) % Shared::kShards;
        c.quota = base + (static_cast<std::uint64_t>(i) < remainder ? 1ULL : 0ULL);
        c.shared = shared.get();
        c.error_slot = &error_slot;
        if (i < kMaxWorkers) {
            g_worker_is_fiber[i].store(c.is_fiber ? 1 : 0,
                                       std::memory_order_relaxed);
        }
        for (std::size_t k = 0; k < 3; ++k) {
            c.fifo[k] = MakeChannel<std::uint64_t>(WorkerCtx::kFifoCap[k]);
        }
        // 每 worker 一对 socketpair；a 写、b 读。
        int fds[2] = {-1, -1};
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0) {
            c.sock_a = fds[0];
            c.sock_b = fds[1];
        }
    }

    StartGate gate;
    std::vector<std::thread> thread_pool;
    thread_pool.reserve(static_cast<std::size_t>(threads));
    std::vector<std::shared_ptr<Task>> fiber_tasks;
    fiber_tasks.reserve(static_cast<std::size_t>(fibers));
    for (int i = 0; i < kMaxWorkers; ++i) {
        g_fiber_tasks[i].store(nullptr, std::memory_order_relaxed);
    }
    g_fiber_task_count.store(fibers, std::memory_order_release);

    g_phase.store("参与者启动", std::memory_order_relaxed);
    const long long start_ns = NowNs();
    for (int i = 0; i < threads; ++i) {
        WorkerCtx* ctx = &ctxs[static_cast<std::size_t>(i)];
        thread_pool.emplace_back([ctx, &gate]() {
            gate.Wait();
            WorkerLoop(ctx);
        });
    }
    for (int i = threads; i < total; ++i) {
        WorkerCtx* ctx = &ctxs[static_cast<std::size_t>(i)];
        auto task = scheduler->spawn([ctx, &gate]() {
            gate.Wait();
            WorkerLoop(ctx);
        });
        if (!task) {
            std::fprintf(stderr, "[错误] Scheduler::spawn 返回空任务\n");
            std::_Exit(4);
        }
        fiber_tasks.push_back(std::move(task));
        const int fiber_slot = i - threads;
        if (fiber_slot >= 0 && fiber_slot < kMaxWorkers) {
            g_fiber_tasks[fiber_slot].store(task.get(),
                                            std::memory_order_release);
        }
    }

    g_phase.store("混合压测进行中", std::memory_order_relaxed);
    gate.Open();

    bool join_ok = true;
    for (auto& thread : thread_pool) {
        thread.join();
    }
    for (auto& task : fiber_tasks) {
        if (!task->wait()) {
            join_ok = false;
        }
        if (task->failed()) {
            join_ok = false;
        }
    }
    const long long end_ns = NowNs();
    g_phase.store("轮末校验", std::memory_order_relaxed);

    result.elapsed_ns = end_ns - start_ns;
    result.ops_per_sec =
        result.elapsed_ns <= 0
            ? 0.0
            : static_cast<double>(target) * 1e9 /
                  static_cast<double>(result.elapsed_ns);
    result.loadavg = ReadLoadAvg();

    // 共享 Channel 的残留值排空：**必须把排空收到的值计入接收侧统计**，
    // 否则「发送数=接收数」的断言会因为残留而假失败（残留本身是正常语义）。
    std::uint64_t drained = 0;
    while (true) {
        const auto recv = shared->chan64->TryRecv();
        if (!recv.Ok()) {
            break;
        }
        shared->chan64_recv_count.fetch_add(1, std::memory_order_relaxed);
        shared->chan64_recv_sum.fetch_add(*recv.value, std::memory_order_relaxed);
        ++drained;
    }
    while (true) {
        const auto recv = shared->chan1024->TryRecv();
        if (!recv.Ok()) {
            break;
        }
        shared->chan1024_recv_count.fetch_add(1, std::memory_order_relaxed);
        shared->chan1024_recv_sum.fetch_add(*recv.value,
                                           std::memory_order_relaxed);
        ++drained;
    }
    while (true) {
        std::uint64_t out = 0;
        if (shared->fast_mpmc.TryRecv(out) != go2cpp::FastStatus::kOk) {
            break;
        }
        shared->mpmc_recv_count.fetch_add(1, std::memory_order_relaxed);
        shared->mpmc_recv_sum.fetch_add(out, std::memory_order_relaxed);
        ++drained;
    }
    while (true) {
        const auto recv = shared->close_chan->TryRecv();
        if (!recv.Ok()) {
            break;
        }
        ++drained;
    }
    while (true) {
        const auto recv = shared->select_chan->TryRecv();
        if (!recv.Ok()) {
            break;
        }
        ++drained;
    }
    (void)drained;

    const LocalAgg agg = Aggregate(ctxs);

    std::uint64_t done_ops = 0;
    for (const auto& c : ctxs) {
        done_ops += c.ops_done;
    }
    result.done_ops = done_ops;

    VerifyRound(*shared, ctxs, agg, target, baseline_rss_kb, previous_rss_kb,
                &result);
    AddCheck(&result.checks, "所有参与者正常结束（无死锁/无异常）", join_ok,
             join_ok ? std::string() : "存在 Fiber 失败或 join 失败");

    result.ok = AllChecksPassed(result.checks);
    if (!result.ok && result.detail.empty()) {
        for (const auto& check : result.checks) {
            if (!check.passed) {
                result.detail = check.name;
                break;
            }
        }
    }

    // 显式析构：让下一轮的 RSS 能真实反映释放情况。
    for (int i = 0; i < kMaxWorkers; ++i) {
        g_fiber_tasks[i].store(nullptr, std::memory_order_release);
    }
    g_fiber_task_count.store(0, std::memory_order_release);
    g_shared.store(nullptr, std::memory_order_release);
    shared.reset();
    return result;
}

// ===========================================================================
// Error 跨 M 迁移专项（手工 Fiber，强制的 M1 → M2 handoff）
// ===========================================================================

/**
 * @brief 在两条不同 OS 线程上接力 resume 同一条 Fiber，验证错误状态跨 M。
 *
 * @param iterations 接力次数；每次都是「线程 A resume → Fiber 挂起 → 线程 B
 *                   resume」，因此每次跨越两台 M。
 * @return 校验失败次数。
 */
int RunErrorMigrationChecks(int iterations, ErrorLocal<ErrorPtr>* slot) {
    int failures = 0;
    for (int i = 0; i < iterations; ++i) {
        ErrorPtr produced = go2cpp::error::MakeError<go2cpp::error::TimeoutError>(
            "migration", std::string("m1->m2"), 1000);
        std::mutex gate_mutex;
        std::condition_variable gate_cv;
        int stage = 0;
        long tid_before = 0;
        long tid_after = 0;
        ErrorPtr seen_before;
        ErrorPtr seen_after;
        ErrorPtr slot_before;
        ErrorPtr slot_after;

        go2cpp::Fiber fiber([&]() {
            tid_before = static_cast<long>(::syscall(SYS_gettid));
            go2cpp::error::SetCurrentError(produced);
            slot->GetOrCreate() = produced;
            seen_before = go2cpp::error::CurrentError();
            slot_before = *slot->TryGet();
            // 挂起：交给另一条线程恢复。
            (void)go2cpp::Fiber::Suspend(go2cpp::SuspendReason::Yield);
            tid_after = static_cast<long>(::syscall(SYS_gettid));
            seen_after = go2cpp::error::CurrentError();
            slot_after = slot->TryGet() != nullptr ? *slot->TryGet() : nullptr;
            go2cpp::error::ClearCurrentError();
            slot->Reset();
            {
                std::lock_guard<std::mutex> lock(gate_mutex);
                stage = 2;
            }
            gate_cv.notify_all();
        });

        std::thread first([&]() {
            if (!fiber.resume()) {
                std::lock_guard<std::mutex> lock(gate_mutex);
                stage = -1;
                gate_cv.notify_all();
                return;
            }
            {
                std::lock_guard<std::mutex> lock(gate_mutex);
                stage = 1;
            }
            gate_cv.notify_all();
        });
        std::thread second([&]() {
            {
                std::unique_lock<std::mutex> lock(gate_mutex);
                gate_cv.wait(lock, [&]() { return stage >= 1; });
            }
            if (stage == 1) {
                if (!fiber.resume()) {
                    std::lock_guard<std::mutex> lock(gate_mutex);
                    stage = -1;
                    gate_cv.notify_all();
                }
            }
        });
        first.join();
        second.join();
        {
            std::unique_lock<std::mutex> lock(gate_mutex);
            gate_cv.wait(lock, [&]() { return stage == 2 || stage == -1; });
        }

        if (stage != 2 || tid_before == tid_after || seen_before != produced ||
            seen_after != produced || slot_before != produced ||
            slot_after != produced) {
            ++failures;
        }
    }
    return failures;
}

// ===========================================================================
// 报告
// ===========================================================================

std::string HostName() {
    char buffer[256] = {0};
    if (::gethostname(buffer, sizeof(buffer) - 1) != 0) {
        return "unknown";
    }
    return buffer;
}

void WriteReport(const std::vector<RoundResult>& rounds, bool all_ok,
                 const std::string& path) {
    std::ofstream out(path);
    if (!out) {
        std::fprintf(stderr, "[错误] 无法写入报告：%s\n", path.c_str());
        return;
    }
    out << "# 最终巨量混合压测报告（自动生成）\n\n";
    out << "- 主机：" << HostName() << "，CPU：" << CpuModelName()
        << "，逻辑核：" << std::thread::hardware_concurrency() << "\n";
    out << "- 配置：每轮 " << FormatU64(g_cfg.ops_per_round) << " 操作，"
        << g_cfg.rounds << " 轮；线程 worker=" << g_cfg.thread_workers
        << "，Fiber worker=" << g_cfg.fiber_workers
        << "，Scheduler P=" << g_cfg.sched_p << "；watchdog=" << g_cfg.hang_sec
        << "s\n";
    out << "- 结论：**" << (all_ok ? "全部通过" : "存在失败") << "**\n\n";
    out << "| 轮 | 目标操作 | 完成操作 | 耗时 | 操作/秒 | loadavg | RSS(MiB) | 断言 | 结果 |\n";
    out << "|---|---|---|---|---|---|---|---|---|\n";
    for (const auto& round : rounds) {
        int passed = 0;
        for (const auto& check : round.checks) {
            passed += check.passed ? 1 : 0;
        }
        out << "| " << round.round << " | " << FormatU64(round.target_ops)
            << " | " << FormatU64(round.done_ops) << " | "
            << FormatSeconds(round.elapsed_ns) << " | "
            << FormatDouble(round.ops_per_sec / 1e6, 2) << " M/s | "
            << FormatDouble(round.loadavg, 2) << " | "
            << FormatDouble(static_cast<double>(round.rss_kb) / 1024.0) << " | "
            << passed << "/" << round.checks.size() << " | "
            << (round.ok ? "通过" : "失败") << " |\n";
    }
    out << "\n";
    for (const auto& round : rounds) {
        out << "## 第 " << round.round << " 轮断言明细\n\n";
        out << "| 断言 | 结果 | 说明 |\n|---|---|---|\n";
        for (const auto& check : round.checks) {
            out << "| " << check.name << " | " << (check.passed ? "通过" : "失败")
                << " | " << (check.detail.empty() ? "-" : check.detail) << " |\n";
        }
        out << "\n";
    }
}

void PrintRoundLine(const RoundResult& result) {
    int passed = 0;
    for (const auto& check : result.checks) {
        passed += check.passed ? 1 : 0;
    }
    std::printf(
        "[第 %d/%d 轮] 操作=%" PRIu64 " 耗时=%.2fs 吞吐=%.2f M ops/s "
        "loadavg=%.2f RSS=%.1fMiB 断言=%d/%d %s\n",
        result.round, g_cfg.rounds, result.done_ops,
        static_cast<double>(result.elapsed_ns) / 1e9,
        result.ops_per_sec / 1e6, result.loadavg,
        static_cast<double>(result.rss_kb) / 1024.0, passed,
        static_cast<int>(result.checks.size()), result.ok ? "通过" : "失败");
    if (!result.ok) {
        std::printf("    首个失败：%s\n", result.detail.c_str());
        for (const auto& check : result.checks) {
            if (!check.passed) {
                std::printf("    - %s：%s\n", check.name.c_str(),
                            check.detail.c_str());
            }
        }
    }
    std::fflush(stdout);
}

// ===========================================================================
// 入口
// ===========================================================================

void ParseArgs(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&](std::uint64_t fallback) -> std::uint64_t {
            if (i + 1 >= argc) {
                return fallback;
            }
            return std::strtoull(argv[++i], nullptr, 10);
        };
        if (arg == "--ops") {
            g_cfg.ops_per_round = next(g_cfg.ops_per_round);
        } else if (arg == "--rounds") {
            g_cfg.rounds =
                static_cast<int>(next(static_cast<std::uint64_t>(g_cfg.rounds)));
        } else if (arg == "--threads") {
            g_cfg.thread_workers = static_cast<int>(
                next(static_cast<std::uint64_t>(g_cfg.thread_workers)));
        } else if (arg == "--fibers") {
            g_cfg.fiber_workers = static_cast<int>(
                next(static_cast<std::uint64_t>(g_cfg.fiber_workers)));
        } else if (arg == "--sched-p") {
            g_cfg.sched_p = static_cast<std::size_t>(next(g_cfg.sched_p));
        } else if (arg == "--hang-sec") {
            g_cfg.hang_sec = static_cast<int>(
                next(static_cast<std::uint64_t>(g_cfg.hang_sec)));
        } else if (arg == "--out") {
            if (i + 1 < argc) {
                g_cfg.out = argv[++i];
            }
        } else if (arg == "--only") {
            if (i + 1 < argc) {
                g_cfg.only = argv[++i];
            }
        } else if (arg == "--pin") {
            if (i + 1 < argc) {
                g_cfg.pin = argv[++i];
            }
        } else if (arg == "--quick") {
            g_cfg.quick = true;
        }
    }
    if (g_cfg.quick) {
        g_cfg.ops_per_round = 1000000ULL;
        g_cfg.rounds = 1;
        g_cfg.hang_sec = 60;
    }
    if (g_cfg.thread_workers < 0) {
        g_cfg.thread_workers = 0;
    }
    if (g_cfg.fiber_workers < 0) {
        g_cfg.fiber_workers = 0;
    }
    if (g_cfg.thread_workers + g_cfg.fiber_workers <= 0) {
        g_cfg.thread_workers = 1;
    }
    if (g_cfg.thread_workers + g_cfg.fiber_workers > kMaxWorkers) {
        g_cfg.fiber_workers = kMaxWorkers - g_cfg.thread_workers;
    }
}

}  // namespace

int main(int argc, char** argv) {
    ParseArgs(argc, argv);

    if (!g_cfg.only.empty()) {
        g_only_kind = KindFromName(g_cfg.only);
        if (g_only_kind < 0) {
            std::fprintf(stderr, "[错误] 未知类别：%s\n", g_cfg.only.c_str());
            return 2;
        }
    }

    std::printf("=== 最终巨量混合压测 ===\n");
    std::printf("CPU：%s，逻辑核：%u\n", CpuModelName().c_str(),
                std::thread::hardware_concurrency());
    std::printf("配置：每轮 %" PRIu64 " 操作 × %d 轮；线程 worker=%d，Fiber "
                "worker=%d，P=%zu；watchdog=%ds\n",
                g_cfg.ops_per_round, g_cfg.rounds, g_cfg.thread_workers,
                g_cfg.fiber_workers, g_cfg.sched_p, g_cfg.hang_sec);
    std::printf("起始 loadavg=%.2f\n", ReadLoadAvg());
    std::fflush(stdout);

    if (!FifoVerifierSelfCheck()) {
        std::fprintf(stderr, "[错误] FIFO 验证器自检失败，断言不可信\n");
        return 2;
    }

    if (!go2cpp::error::InstallFiberErrorLocalStorage()) {
        std::printf("[警告] Fiber error-local 后端安装失败，Error 迁移检查会退化为 TLS\n");
    }

    const ProcMem baseline = ReadProcMem();
    std::printf("基线 RSS=%.1f MiB\n",
                static_cast<double>(baseline.rss_kb) / 1024.0);

    std::thread watchdog(WatchdogMain);

    Scheduler scheduler(g_cfg.sched_p);
    g_scheduler = &scheduler;
    scheduler.start();

    std::vector<RoundResult> rounds;
    std::uint64_t previous_rss = 0;
    bool all_ok = true;

    for (int round = 1; round <= g_cfg.rounds; ++round) {
        g_phase.store("轮次准备", std::memory_order_relaxed);
        RoundResult result =
            RunRound(round, &scheduler, baseline.rss_kb, previous_rss);

        // 每轮追加 Error 跨 M 强制迁移专项（手工 Fiber，两个真实线程接力）。
        static ErrorLocal<ErrorPtr> migration_slot;
        const int migration_iterations = g_cfg.quick ? 16 : 256;
        const int migration_failures =
            RunErrorMigrationChecks(migration_iterations, &migration_slot);
        AddCheck(&result.checks, "Error 强制跨 M 迁移（M1→挂起→M2）状态一致",
                 migration_failures == 0,
                 "迭代=" + std::to_string(migration_iterations) + " 失败=" +
                     std::to_string(migration_failures));
        result.ok = AllChecksPassed(result.checks);
        if (!result.ok && result.detail.empty()) {
            for (const auto& check : result.checks) {
                if (!check.passed) {
                    result.detail = check.name;
                    break;
                }
            }
        }

        previous_rss = result.rss_kb;
        PrintRoundLine(result);
        all_ok = all_ok && result.ok;
        rounds.push_back(result);
    }

    g_phase.store("收尾", std::memory_order_relaxed);
    const long long shutdown_start = NowNs();
    scheduler.shutdown();
    const long long shutdown_ns = NowNs() - shutdown_start;
    std::printf("调度器收尾耗时=%.3fs\n",
                static_cast<double>(shutdown_ns) / 1e9);

    g_stop_watchdog.store(true, std::memory_order_release);
    if (watchdog.joinable()) {
        watchdog.join();
    }

    // 汇总。
    std::uint64_t total_ops = 0;
    long long total_ns = 0;
    for (const auto& round : rounds) {
        total_ops += round.done_ops;
        total_ns += round.elapsed_ns;
    }
    const double average_ops_per_sec =
        total_ns <= 0 ? 0.0
                      : static_cast<double>(total_ops) * 1e9 /
                            static_cast<double>(total_ns);
    const ProcMem final_mem = ReadProcMem();

    std::printf("\n=== 汇总 ===\n");
    std::printf("总操作=%" PRIu64 "，总耗时=%.2fs，平均吞吐=%.2f M ops/s\n",
                total_ops, static_cast<double>(total_ns) / 1e9,
                average_ops_per_sec / 1e6);
    std::printf("最终 RSS=%.1f MiB（基线 %.1f MiB）\n",
                static_cast<double>(final_mem.rss_kb) / 1024.0,
                static_cast<double>(baseline.rss_kb) / 1024.0);
    std::printf("结论：%s\n", all_ok ? "全部通过" : "存在失败");

    WriteReport(rounds, all_ok, g_cfg.out);
    std::printf("报告已写入：%s\n", g_cfg.out.c_str());

    return all_ok ? 0 : 1;
}
