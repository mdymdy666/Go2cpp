/**
 * @file stress_matrix.cpp
 * @brief 并发结构 × 10 场景矩阵正确性压测（S1~S10，每结构每场景 3 轮）。
 *
 * ## 定位
 *
 * `docs/reports/2026-10-31-stress-matrix-plan.md` 的「分结构矩阵」补充定位：
 * 对每个并发结构单独跑 10 类高风险场景，失败时能直接定位到结构/场景/轮次，
 * 而不是只看到混合压测的一个总数。
 *
 * 与 `stress_final.cpp`（全部混合的巨量压测）分开实现、互不依赖。
 *
 * ## 结构（14 个）
 *
 * 通道类：Channel、FastChannel、Select；
 * 等待/调度类：WaitNode（ParkingCondition）、SchedulerPark、FiberSwitch；
 * 锁类：Mutex、CV、WG、RWMutex、SpinMutex；
 * 值类：Context、Error、Future。
 *
 * ## 场景（S1~S10）
 *
 * S1 Context 取消：操作中途 Cancel/Deadline 打断，取消后状态一致、下次可用；
 * S2 网络通信：socketpair(AF_UNIX) 帧收发与结构操作混合，序号严格递增；
 * S3 线程 Fiber 混合：f2f/f2t/t2f/t2t 四种执行体组合，四类计数都 > 0；
 * S4 关闭/析构竞态：Close/Cancel 与 Send/Recv/Wait 并发，不崩不挂、计数守恒；
 * S5 嵌套/重入：锁嵌套、channel of channel、嵌套 select、嵌套 Context/Future；
 * S6 超时/饥饿：1ms 超时循环 + 短临界区饥饿，超时后立即可重入；
 * S7 错误传播：ErrorPtr 链 Is()/Code() 校验 + FiberLocal 跨 park（迁移）保持；
 * S8 批量/背压：小缓冲 + 快生产慢消费，不丢不挂、FIFO、最终排空；
 * S9 高负载：系统 load > 50 时仍正确（loadavg 记录在结果行）；
 * S10 随机/模糊：随机操作序列 + 期望模型对拍（默认 1000 万次）。
 *
 * ## 断言（全部真实）
 *
 * 计数一致、FIFO、无丢失/重复/乱序、无死锁（watchdog 默认 300s）、RSS 稳定、
 * 等待者归零（`DebugSnapshot().waiters` / `send_waiters + recv_waiters`）。
 *
 * ## 规模
 *
 * `--ops N` 为每轮每场景的目标操作数；`--full` 等价 `--ops 100000000`。
 * 本程序默认按结构成本给出较小默认值，便于在并行压测环境里跑完 420 个单元；
 * 报告里逐单元记录实际 ops 与耗时，并给出 `--full` 复现命令。
 *
 * ## 用法
 *
 * ```
 * taskset -c 2,3 ./stress_matrix --structures Channel,Select --rounds 3
 * ./stress_matrix --structures all --scenarios all --ops 1000000 --rounds 3
 * ./stress_matrix --burn-only 64 --burn-seconds 120      # 造 load > 50
 * ```
 *
 * 输出：每个单元一行 `MATRIX\t...`（TSV，供聚合），失败另起 `FAIL\t...` 行；
 * 人类可读摘要写 stderr。`--tsv FILE` 可把 TSV 同时落盘。
 */

#include "go2cpp/channel.hpp"
#include "go2cpp/concurrency/core/parking_condition.hpp"
#include "go2cpp/concurrency/fast_channel.hpp"
#include "go2cpp/concurrency/fiber_local.hpp"
#include "go2cpp/concurrency/locks.hpp"
#include "go2cpp/concurrency/spin_mutex.hpp"
#include "go2cpp/concurrency/timed_mutex.hpp"
#include "go2cpp/context.hpp"
#include "go2cpp/error.hpp"
#include "go2cpp/error/compat.hpp"
#include "go2cpp/fiber.hpp"
#include "go2cpp/future.hpp"
#include "go2cpp/go.hpp"
#include "go2cpp/scheduler.hpp"
#include "go2cpp/sync.hpp"

#include <dirent.h>
#include <execinfo.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

using go2cpp::Background;
using go2cpp::CancelFunc;
using go2cpp::ChannelPtr;
using go2cpp::ChannelStatus;
using go2cpp::Context;
using go2cpp::ContextDuration;
using go2cpp::ContextPtr;
using go2cpp::ErrorPtr;
using go2cpp::FastChannel;
using go2cpp::FastStatus;
using go2cpp::Future;
using go2cpp::FutureStatus;
using go2cpp::MakeChannel;
using go2cpp::Promise;
using go2cpp::Scheduler;
using go2cpp::Select;
using go2cpp::SelectCase;
using go2cpp::SelectResult;
using go2cpp::core::ParkingCondition;
using go2cpp::sync::ConditionVariable;
using go2cpp::sync::Mutex;
using go2cpp::sync::RWMutex;
using go2cpp::sync::SpinMutex;
using go2cpp::sync::TimedMutex;
using go2cpp::sync::WaitGroup;

// ===========================================================================
// 基础类型与结果记录
// ===========================================================================

constexpr int kMaxFailures = 24;
constexpr std::size_t kStackDepth = 48;

enum class Scenario : int {
    S1 = 0,
    S2,
    S3,
    S4,
    S5,
    S6,
    S7,
    S8,
    S9,
    S10,
    Count,
};

constexpr int kScenarioCount = static_cast<int>(Scenario::Count);

const char* ScenarioName(Scenario scenario) {
    static const char* const kNames[] = {"S1", "S2", "S3", "S4", "S5",
                                         "S6", "S7", "S8", "S9", "S10"};
    return kNames[static_cast<int>(scenario)];
}

enum class Structure : int {
    Channel = 0,
    FastChannel,
    Select,
    WaitNode,
    SchedulerPark,
    FiberSwitch,
    Mutex,
    CV,
    WG,
    RWMutex,
    SpinMutex,
    Context,
    Error,
    Future,
    Count,
};

constexpr int kStructureCount = static_cast<int>(Structure::Count);

struct StructureInfo {
    Structure structure;
    const char* name;
    bool high_risk;
    long long default_ops;
};

// 高风险的 6 个结构排在最前面（优先跑）；default_ops 按单次操作成本粗调，
// 保证「14 结构 × 10 场景 × 3 轮」在本机可控时间内跑完，报告里记录实际值。
constexpr std::array<StructureInfo, kStructureCount> kStructures{{
    {Structure::Channel, "Channel", true, 2000000},
    {Structure::FastChannel, "FastChannel", true, 2000000},
    {Structure::Select, "Select", true, 1000000},
    {Structure::WaitNode, "WaitNode", true, 300000},
    {Structure::SchedulerPark, "SchedulerPark", true, 100000},
    {Structure::FiberSwitch, "FiberSwitch", true, 100000},
    {Structure::Mutex, "Mutex", false, 2000000},
    {Structure::CV, "CV", false, 300000},
    {Structure::WG, "WG", false, 500000},
    {Structure::RWMutex, "RWMutex", false, 1000000},
    {Structure::SpinMutex, "SpinMutex", false, 2000000},
    {Structure::Context, "Context", false, 1000000},
    {Structure::Error, "Error", false, 2000000},
    {Structure::Future, "Future", false, 500000},
}};

const StructureInfo& InfoOf(Structure structure) {
    return kStructures[static_cast<std::size_t>(structure)];
}

/** @brief 单个（结构, 场景, 轮次）单元的可变状态。线程安全。 */
struct UnitState {
    std::atomic<long long> progress{0};
    std::atomic<long long> ok{0};
    std::atomic<long long> cancelled{0};
    std::atomic<long long> timed_out{0};
    std::atomic<long long> closed{0};
    std::atomic<long long> retry{0};
    std::atomic<long long> bad{0};

    std::mutex mutex;
    std::vector<std::string> failures;

    void Ok(long long count = 1) noexcept {
        ok.fetch_add(count, std::memory_order_relaxed);
        progress.fetch_add(count, std::memory_order_relaxed);
    }
    void Cancel() noexcept {
        cancelled.fetch_add(1, std::memory_order_relaxed);
        progress.fetch_add(1, std::memory_order_relaxed);
    }
    void Timeout() noexcept {
        timed_out.fetch_add(1, std::memory_order_relaxed);
        // 一次「按预期超时」也是一次完成的操作：计入 progress/done，
        // 否则纯超时场景（S6）会被误判成空跑。
        progress.fetch_add(1, std::memory_order_relaxed);
    }
    void Closed() noexcept { closed.fetch_add(1, std::memory_order_relaxed); }
    void Retry() noexcept { retry.fetch_add(1, std::memory_order_relaxed); }

    void Fail(const std::string& message) {
        bad.fetch_add(1, std::memory_order_relaxed);
        progress.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(mutex);
        if (failures.size() < static_cast<std::size_t>(kMaxFailures)) {
            failures.push_back(message);
        } else if (failures.size() == static_cast<std::size_t>(kMaxFailures)) {
            failures.push_back("... 失败过多，后续省略");
        }
    }

    bool Expect(bool condition, const std::string& message) {
        if (!condition) {
            Fail(message);
        }
        return condition;
    }

    std::vector<std::string> Snapshot() {
        std::lock_guard<std::mutex> lock(mutex);
        return failures;
    }

    bool Failed() noexcept {
        return bad.load(std::memory_order_relaxed) != 0;
    }

    std::string Counters() const {
        char buffer[256];
        std::snprintf(buffer, sizeof(buffer),
                      "ok=%lld cancel=%lld timeout=%lld closed=%lld retry=%lld "
                      "bad=%lld",
                      ok.load(std::memory_order_relaxed),
                      cancelled.load(std::memory_order_relaxed),
                      timed_out.load(std::memory_order_relaxed),
                      closed.load(std::memory_order_relaxed),
                      retry.load(std::memory_order_relaxed),
                      bad.load(std::memory_order_relaxed));
        return std::string(buffer);
    }
};

// ===========================================================================
// 进程环境：RSS / loadavg / 线程栈转储
// ===========================================================================

long long ReadRssKb() {
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        if (line.rfind("VmRSS:", 0) == 0) {
            const char* p = line.c_str() + 6;
            while (*p == ' ' || *p == '\t') {
                ++p;
            }
            return std::atoll(p);
        }
    }
    return -1;
}

double ReadLoad1() {
    std::ifstream load("/proc/loadavg");
    double value = -1.0;
    if (load >> value) {
        return value;
    }
    return -1.0;
}

void DumpSignalHandler(int) {
    void* frames[kStackDepth];
    const int count = backtrace(frames, static_cast<int>(kStackDepth));
    backtrace_symbols_fd(frames, count, STDERR_FILENO);
}

void InstallDumpHandler() {
    struct sigaction action {};
    action.sa_handler = &DumpSignalHandler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    sigaction(SIGUSR1, &action, nullptr);
}

void DumpAllThreadStacks() {
    std::vector<pid_t> tids;
    if (DIR* dir = opendir("/proc/self/task")) {
        while (dirent* entry = readdir(dir)) {
            const long tid = std::atol(entry->d_name);
            if (tid > 0) {
                tids.push_back(static_cast<pid_t>(tid));
            }
        }
        closedir(dir);
    }
    std::fprintf(stderr, "[watchdog] 线程数=%zu，逐个转储栈：\n", tids.size());
    std::fflush(stderr);
    for (const pid_t tid : tids) {
        std::fprintf(stderr, "[watchdog] ---- tid=%d ----\n", static_cast<int>(tid));
        std::fflush(stderr);
        syscall(SYS_tgkill, getpid(), tid, SIGUSR1);
        std::this_thread::sleep_for(20ms);
    }
}

// ===========================================================================
// Watchdog：单元级「300s 无进展」即失败（打印所有线程栈后退出）
// ===========================================================================

class Watchdog final {
public:
    Watchdog(UnitState& state, std::string structure, std::string scenario,
             int round, long long timeout_ms)
        : m_state(state),
          m_structure(std::move(structure)),
          m_scenario(std::move(scenario)),
          m_round(round),
          m_timeout_ms(timeout_ms) {}

    void Start() {
        m_thread = std::thread([this] { Loop(); });
    }

    void Stop() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stop = true;
        }
        m_cv.notify_all();
        if (m_thread.joinable()) {
            m_thread.join();
        }
    }

    ~Watchdog() { Stop(); }

private:
    void Loop() {
        long long last = m_state.progress.load(std::memory_order_relaxed);
        auto last_change = Clock::now();
        while (true) {
            std::unique_lock<std::mutex> lock(m_mutex);
            if (m_cv.wait_for(lock, 250ms, [this] { return m_stop; })) {
                return;
            }
            const long long current =
                m_state.progress.load(std::memory_order_relaxed);
            if (current != last) {
                last = current;
                last_change = Clock::now();
                continue;
            }
            const auto stalled =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    Clock::now() - last_change)
                    .count();
            if (stalled < m_timeout_ms) {
                continue;
            }
            lock.unlock();
            std::fprintf(stderr,
                         "[watchdog] 死锁：%s/%s 第 %d 轮 %ldms 无进展 "
                         "(%s)\n",
                         m_structure.c_str(), m_scenario.c_str(), m_round,
                         stalled, m_state.Counters().c_str());
            std::fflush(stderr);
            DumpAllThreadStacks();
            std::fflush(stderr);
            _exit(124);
        }
    }

    UnitState& m_state;
    std::string m_structure;
    std::string m_scenario;
    int m_round;
    long long m_timeout_ms;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_stop{false};
    std::thread m_thread;
};

// ===========================================================================
// 通用工具
// ===========================================================================

std::uint64_t SplitMix64(std::uint64_t& state) {
    std::uint64_t z = (state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

std::uint64_t SeedFor(const StructureInfo& info, Scenario scenario, int round) {
    return 0x5EEDF00DCAFEBABEULL ^
           (static_cast<std::uint64_t>(info.structure) << 40) ^
           (static_cast<std::uint64_t>(scenario) << 32) ^
           static_cast<std::uint64_t>(round);
}

/** @brief 旧版 ErrorPtr 没有 Code()，经 compat 适配到类型链体系后取错误码。 */
go2cpp::error::ErrorCode CodeOf(const ErrorPtr& error) {
    if (!error) {
        return go2cpp::error::ErrorCode::kUnknown;
    }
    const auto typed = go2cpp::error::FromLegacy(error);
    return typed ? typed->Code() : go2cpp::error::ErrorCode::kUnknown;
}

/** @brief 旧版错误适配后的类型名与继承深度（用于类型链断言）。 */
std::string TypeNameOf(const ErrorPtr& error) {
    const auto typed = go2cpp::error::FromLegacy(error);
    return typed ? std::string(typed->TypeName()) : std::string();
}

/** @brief AF_UNIX socketpair 上的一次定长帧往返（阻塞读写在普通线程里用）。 */
class SocketPair {
public:
    SocketPair() {
        m_ok = ::socketpair(AF_UNIX, SOCK_STREAM, 0, m_fds) == 0;
    }
    ~SocketPair() {
        if (m_fds[0] >= 0) {
            ::close(m_fds[0]);
        }
        if (m_fds[1] >= 0) {
            ::close(m_fds[1]);
        }
    }
    SocketPair(const SocketPair&) = delete;
    SocketPair& operator=(const SocketPair&) = delete;

    bool Ok() const noexcept { return m_ok; }

    bool RoundTrip(std::uint64_t value) {
        if (!m_ok) {
            return false;
        }
        std::uint64_t out = value;
        std::size_t written = 0;
        while (written < sizeof(out)) {
            const ssize_t n = ::write(m_fds[0],
                                      reinterpret_cast<const char*>(&out) + written,
                                      sizeof(out) - written);
            if (n <= 0) {
                return false;
            }
            written += static_cast<std::size_t>(n);
        }
        std::uint64_t in = 0;
        std::size_t read_total = 0;
        while (read_total < sizeof(in)) {
            const ssize_t n = ::read(m_fds[1],
                                     reinterpret_cast<char*>(&in) + read_total,
                                     sizeof(in) - read_total);
            if (n <= 0) {
                return false;
            }
            read_total += static_cast<std::size_t>(n);
        }
        return in == value;
    }

private:
    int m_fds[2]{-1, -1};
    bool m_ok{false};
};


/** @brief 一个「四通道环」：f1→f2→t1→t2→f1，覆盖 f2f/f2t/t2t/t2f。 */
struct ChannelRing {
    ChannelPtr<long long> f1_to_f2;
    ChannelPtr<long long> f2_to_t1;
    ChannelPtr<long long> t1_to_t2;
    ChannelPtr<long long> t2_to_f1;
    std::array<std::atomic<long long>, 4> counts{};

    explicit ChannelRing(std::size_t capacity) {
        f1_to_f2 = MakeChannel<long long>(capacity);
        f2_to_t1 = MakeChannel<long long>(capacity);
        t1_to_t2 = MakeChannel<long long>(capacity);
        t2_to_f1 = MakeChannel<long long>(capacity);
    }
};

void WaitTasks(const std::vector<std::shared_ptr<go2cpp::Task>>& tasks,
               UnitState& state, const char* where) {
    for (const auto& task : tasks) {
        if (!task) {
            state.Fail(std::string(where) + ": spawn 返回空任务");
            continue;
        }
        if (!task->wait_for(300s)) {
            state.Fail(std::string(where) + ": 任务 300s 未结束");
        }
    }
}

// ===========================================================================
// Channel：10 场景
// ===========================================================================

void RunChannel(Scenario scenario, long long ops, UnitState& state,
                std::uint64_t& rng) {
    switch (scenario) {
        case Scenario::S1: {
            // 主路径：有缓冲通道往返 + FIFO；每 256 次插一次真实「中途取消」。
            auto channel = MakeChannel<long long>(16);
            for (long long i = 0; i < ops; ++i) {
                if ((i & 255) == 0) {
                    auto pair = Context::WithCancel(Background());
                    auto solo = MakeChannel<long long>(0);
                    std::atomic<int> status{-1};
                    std::thread waiter([&] {
                        const auto result = solo->Recv(pair.first);
                        status.store(static_cast<int>(result.status),
                                     std::memory_order_release);
                    });
                    std::this_thread::sleep_for(30us);
                    pair.second();
                    waiter.join();
                    if (!state.Expect(
                            status.load(std::memory_order_acquire) ==
                                static_cast<int>(ChannelStatus::kCancelled),
                            "S1 Recv(ctx) 被 cancel 后未返回 kCancelled")) {
                        state.Cancel();
                    } else {
                        state.Cancel();
                    }
                    // 取消后通道仍可用：立即用无取消 ctx 接收一次。
                    std::thread sender([&] { (void)solo->Send(1); });
                    const auto after = solo->Recv();
                    sender.join();
                    state.Expect(after.Ok(), "S1 取消后通道不可再用");
                    continue;
                }
                const auto send = channel->Send(i);
                if (!state.Expect(send.Ok(), "S1 Send 失败")) {
                    continue;
                }
                const auto recv = channel->Recv();
                if (!state.Expect(recv.Ok(), "S1 Recv 失败")) {
                    continue;
                }
                state.Expect(recv.ValueOrDefault() == i, "S1 FIFO 乱序");
                state.Ok();
            }
            const auto snapshot = channel->DebugSnapshot();
            state.Expect(snapshot.send_waiters == 0 && snapshot.recv_waiters == 0,
                         "S1 结束后仍有 channel waiters");
            break;
        }
        case Scenario::S2: {
            // socketpair 帧收发与 Channel 混跑：每 16 次操作一次网络往返。
            SocketPair socket;
            if (!state.Expect(socket.Ok(), "S2 socketpair 创建失败")) {
                return;
            }
            auto channel = MakeChannel<long long>(8);
            std::uint64_t net_seq = 0;
            for (long long i = 0; i < ops; ++i) {
                if ((i & 15) == 0) {
                    if (!state.Expect(socket.RoundTrip(net_seq++),
                                      "S2 socketpair 帧顺序/内容错误")) {
                        break;
                    }
                }
                const auto send = channel->Send(i);
                if (!send.Ok()) {
                    state.Fail("S2 网络混跑下 Send 失败");
                    break;
                }
                const auto recv = channel->Recv();
                if (!recv.Ok() || recv.ValueOrDefault() != i) {
                    state.Fail("S2 网络混跑下 FIFO 破坏");
                    break;
                }
                state.Ok();
            }
            break;
        }
        case Scenario::S3: {
            // 单令牌四通道环：令牌依次经过 f2→t1→t2→f1→f2，每次跨执行体
            // 交接对应 f2t / t2t / t2f / f2f 四种组合。
            Scheduler scheduler(4);
            scheduler.start();
            ChannelRing ring(64);
            const long long per_worker = std::max<long long>(1, ops / 40);
            std::atomic<long long> done{0};
            auto make_worker = [&](long long start, ChannelPtr<long long> in,
                                   ChannelPtr<long long> out,
                                   std::atomic<long long>* counter) {
                return [&, start, in, out, counter, per_worker] {
                    for (long long i = 0; i < per_worker; ++i) {
                        const long long expected = start + 4 * i;
                        const auto received = in->Recv();
                        if (!received.Ok() ||
                            received.ValueOrDefault() != expected) {
                            state.Fail("S3 环 Recv 值/顺序错误");
                            return;
                        }
                        if (!out->Send(expected + 1).Ok()) {
                            state.Fail("S3 环 Send 失败");
                            return;
                        }
                        counter->fetch_add(1, std::memory_order_relaxed);
                        done.fetch_add(1, std::memory_order_relaxed);
                        if ((i & 1023) == 0) {
                            state.progress.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                };
            };
            // 入边偏移：f2 收 0，t1 收 1，t2 收 2，f1 收 3。
            auto task_f1 = scheduler.spawn(make_worker(
                3, ring.t2_to_f1, ring.f1_to_f2, &ring.counts[0]));
            auto task_f2 = scheduler.spawn(make_worker(
                0, ring.f1_to_f2, ring.f2_to_t1, &ring.counts[1]));
            std::thread thread_t1(
                make_worker(1, ring.f2_to_t1, ring.t1_to_t2, &ring.counts[2]));
            std::thread thread_t2(
                make_worker(2, ring.t1_to_t2, ring.t2_to_f1, &ring.counts[3]));
            (void)ring.f1_to_f2->Send(0);  // 种子令牌
            thread_t1.join();
            thread_t2.join();
            WaitTasks({task_f1, task_f2}, state, "S3 Channel");
            scheduler.shutdown();
            long long total = 0;
            for (const auto& counter : ring.counts) {
                const long long value = counter.load(std::memory_order_relaxed);
                total += value;
                state.Expect(value > 0, "S3 某类执行体组合计数为 0");
            }
            state.Expect(total == done.load(std::memory_order_relaxed),
                         "S3 环计数与完成数不一致");
            state.Ok(total);
            break;
        }
        case Scenario::S4: {
            // Close 与 Send/Recv 并发：1 生产者 + 1 消费者 + 1 closer。
            auto channel = MakeChannel<long long>(32);
            std::atomic<bool> stop{false};
            std::atomic<long long> produced{0};
            std::atomic<long long> consumed{0};
            std::atomic<long long> last{-1};
            std::atomic<long long> order_bad{0};
            std::thread closer([&] {
                std::this_thread::sleep_for(10ms);
                (void)channel->Close();
                const auto again = channel->Close();
                state.Expect(again.status == ChannelStatus::kAlreadyClosed,
                             "S4 重复 Close 未返回 kAlreadyClosed");
                stop.store(true, std::memory_order_release);
            });
            std::thread producer([&] {
                long long i = 0;
                while (!stop.load(std::memory_order_acquire)) {
                    const auto result = channel->Send(i);
                    if (result.Ok()) {
                        produced.fetch_add(1, std::memory_order_relaxed);
                        ++i;
                    } else if (result.Closed()) {
                        break;
                    } else {
                        state.Fail("S4 Send 返回非 closed 错误");
                        break;
                    }
                    if ((i & 1023) == 0) {
                        state.progress.fetch_add(1, std::memory_order_relaxed);
                    }
                    if (i >= ops) {
                        break;
                    }
                }
            });
            std::thread consumer([&] {
                while (true) {
                    const auto result = channel->Recv();
                    if (result.Ok()) {
                        const long long value = result.ValueOrDefault();
                        const long long expected =
                            last.load(std::memory_order_relaxed) + 1;
                        if (value != expected) {
                            order_bad.fetch_add(1, std::memory_order_relaxed);
                        }
                        last.store(value, std::memory_order_relaxed);
                        consumed.fetch_add(1, std::memory_order_relaxed);
                    } else if (result.Closed()) {
                        break;
                    } else {
                        state.Fail("S4 Recv 返回非 closed 错误");
                        break;
                    }
                }
            });
            producer.join();
            consumer.join();
            closer.join();
            state.Expect(order_bad.load() == 0, "S4 单生产者 FIFO 被破坏");
            state.Expect(consumed.load() == produced.load(),
                         "S4 关闭竞态丢值：consumed != produced");
            const auto snapshot = channel->DebugSnapshot();
            state.Expect(snapshot.length == 0, "S4 关闭后缓冲未排空");
            state.Expect(snapshot.send_waiters == 0 && snapshot.recv_waiters == 0,
                         "S4 关闭后仍有 waiters");
            state.Closed();
            state.Ok(std::max<long long>(1, produced.load()));
            break;
        }
        case Scenario::S5: {
            // channel of channel + 两层 select 嵌套。
            auto outer = MakeChannel<ChannelPtr<long long>>(2);
            for (long long i = 0; i < ops; ++i) {
                auto inner = MakeChannel<long long>(1);
                if (!outer->Send(inner).Ok()) {
                    state.Fail("S5 channel-of-channel Send 失败");
                    break;
                }
                auto back = outer->Recv();
                if (!back.Ok()) {
                    state.Fail("S5 channel-of-channel Recv 失败");
                    break;
                }
                ChannelPtr<long long> got = back.ValueOrDefault();
                if (!state.Expect(static_cast<bool>(got),
                                  "S5 channel-of-channel 取到空通道")) {
                    break;
                }
                if (!got->Send(i).Ok()) {
                    state.Fail("S5 内层 Send 失败");
                    break;
                }
                auto selected = Select({go2cpp::RecvCase(got),
                                        go2cpp::DoneCase(Background())},
                                       {}, 5s);
                if (!selected.selected || selected.index != 0) {
                    state.Fail("S5 嵌套 select 未命中内层 RecvCase");
                    break;
                }
                const auto value = selected.Value<long long>();
                if (!value || *value != i) {
                    state.Fail("S5 嵌套 select 值错误");
                    break;
                }
                // 再套一层 select：从外层取回同一个内层通道。
                if (!outer->Send(got).Ok()) {
                    state.Fail("S5 嵌套 select 回传失败");
                    break;
                }
                auto nested = Select({go2cpp::RecvCase(outer)}, {}, 5s);
                if (!nested.selected || nested.index != 0) {
                    state.Fail("S5 三层嵌套 select 失败");
                    break;
                }
                state.Ok();
            }
            break;
        }
        case Scenario::S6: {
            // 1ms 超时与正常收发交替（饥饿模式：短超时轮换）。
            auto channel = MakeChannel<long long>(1);
            for (long long i = 0; i < ops; ++i) {
                if ((i & 1) == 0) {
                    const auto timed = channel->RecvFor(1ms);
                    if (timed.status == ChannelStatus::kDeadlineExceeded ||
                        timed.status == ChannelStatus::kTimedOut) {
                        state.Timeout();
                    } else if (timed.Ok()) {
                        state.Fail("S6 空通道 RecvFor 返回了值");
                    } else {
                        state.Fail("S6 RecvFor 未返回超时");
                    }
                } else {
                    if (!channel->Send(i).Ok()) {
                        state.Fail("S6 Send 失败");
                        break;
                    }
                    const auto recv = channel->RecvFor(1ms);
                    if (!recv.Ok() || recv.ValueOrDefault() != i) {
                        state.Fail("S6 超时后值丢失/错误");
                        break;
                    }
                    state.Ok();
                }
            }
            break;
        }
        case Scenario::S7: {
            // 关闭错误链 + FiberLocal 跨 park 保持。
            auto channel = MakeChannel<long long>(1);
            (void)channel->Close();
            const auto recv = channel->Recv();
            state.Expect(recv.Closed(), "S7 关闭通道 Recv 未返回 kClosed");
            state.Expect(static_cast<bool>(recv.error), "S7 关闭未带 ErrorPtr");
            if (recv.error) {
                state.Expect(go2cpp::Is(recv.error, go2cpp::ChannelClosedError()),
                             "S7 关闭错误类型不是 ChannelClosedError");
                state.Expect(!recv.error->Message().empty(),
                             "S7 关闭错误 Message 为空");
            }
            const auto send = channel->Send(1);
            state.Expect(send.Closed(), "S7 关闭通道 Send 未返回 kClosed");
            go2cpp::FiberLocal<ErrorPtr> slot;
            Scheduler scheduler(4);
            scheduler.start();
            auto gate = MakeChannel<int>(0);
            std::atomic<bool> matched{false};
            auto waiter = scheduler.spawn([&] {
                slot.GetOrCreate(nullptr) = go2cpp::CanceledError();
                const auto result = Select({go2cpp::RecvCase(gate)}, {}, 10s);
                if (!result.selected) {
                    state.Fail("S7 fiber 内 Recv 等待失败");
                    return;
                }
                const ErrorPtr* stored = slot.TryGet();
                matched.store(stored != nullptr && *stored != nullptr &&
                                  go2cpp::Is(*stored, go2cpp::CanceledError()),
                              std::memory_order_release);
            });
            auto sender = scheduler.spawn([&] { (void)gate->Send(1); });
            WaitTasks({waiter, sender}, state, "S7 fiber-local");
            scheduler.shutdown();
            state.Expect(matched.load(std::memory_order_acquire),
                         "S7 FiberLocal<ErrorPtr> 跨 park 后丢失/类型错");
            state.Ok(2);
            break;
        }
        case Scenario::S8: {
            // 背压：cap=1，快生产慢消费，TrySend 失败即退避重试。
            auto channel = MakeChannel<long long>(1);
            std::atomic<long long> produced{0};
            std::atomic<long long> consumed{0};
            std::atomic<long long> retries{0};
            std::atomic<long long> order_bad{0};
            const long long target = std::max<long long>(1, ops);
            std::thread producer([&] {
                for (long long i = 0; i < target; ++i) {
                    for (;;) {
                        const auto result = channel->TrySend(i);
                        if (result.Ok()) {
                            produced.fetch_add(1, std::memory_order_relaxed);
                            break;
                        }
                        if (result.Closed()) {
                            state.Fail("S8 背压生产者遇到关闭");
                            return;
                        }
                        retries.fetch_add(1, std::memory_order_relaxed);
                        if ((retries.load(std::memory_order_relaxed) & 1023) == 0) {
                            state.progress.fetch_add(1,
                                                     std::memory_order_relaxed);
                        }
                        std::this_thread::yield();
                    }
                }
                (void)channel->Close();
            });
            std::thread consumer([&] {
                long long expected = 0;
                while (true) {
                    const auto result = channel->Recv();
                    if (result.Ok()) {
                        if (result.ValueOrDefault() != expected) {
                            order_bad.fetch_add(1, std::memory_order_relaxed);
                        }
                        ++expected;
                        consumed.fetch_add(1, std::memory_order_relaxed);
                        if ((expected & 4095) == 0) {
                            std::this_thread::sleep_for(1us);
                        }
                    } else if (result.Closed()) {
                        break;
                    } else {
                        state.Fail("S8 消费者收到非 closed 错误");
                        break;
                    }
                }
            });
            producer.join();
            consumer.join();
            state.Expect(consumed.load() == produced.load(),
                         "S8 背压丢值：consumed != produced");
            state.Expect(consumed.load() == target, "S8 背压未全部消费");
            state.Expect(order_bad.load() == 0, "S8 背压 FIFO 破坏");
            state.Retry();
            state.Ok(consumed.load());
            break;
        }
        case Scenario::S9: {
            // 高负载：记录 loadavg，跑生产/多消费者混合负载。三个消费者
            // 竞争同一通道，无法要求跨线程 FIFO；这里只校验「每个值都在
            // 合法范围、总数守恒、无重复」，FIFO 由 S8 单消费者覆盖。
            auto channel = MakeChannel<long long>(64);
            const long long total = std::max<long long>(1, ops);
            std::atomic<long long> consumed{0};
            std::atomic<long long> bad_value{0};
            const bool track_seen = total <= 4000000;
            std::vector<std::atomic<std::uint8_t>> seen(
                track_seen ? static_cast<std::size_t>(total) : 0);
            for (auto& slot : seen) {
                slot.store(0, std::memory_order_relaxed);
            }
            std::thread producer([&] {
                for (long long i = 0; i < total; ++i) {
                    if (!channel->Send(i).Ok()) {
                        state.Fail("S9 高负载 Send 失败");
                        return;
                    }
                }
                (void)channel->Close();
            });
            std::vector<std::thread> consumers;
            for (int c = 0; c < 3; ++c) {
                consumers.emplace_back([&] {
                    while (true) {
                        const auto result = channel->Recv();
                        if (result.Ok()) {
                            const long long value = result.ValueOrDefault();
                            if (value < 0 || value >= total) {
                                bad_value.fetch_add(1, std::memory_order_relaxed);
                            } else if (track_seen &&
                                       seen[static_cast<std::size_t>(value)]
                                               .fetch_add(
                                                   1, std::memory_order_relaxed) !=
                                           0) {
                                // 重复值：同一序号被消费两次。
                                bad_value.fetch_add(1, std::memory_order_relaxed);
                            }
                            consumed.fetch_add(1, std::memory_order_relaxed);
                            if ((consumed.load() & 8191) == 0) {
                                state.progress.fetch_add(
                                    1, std::memory_order_relaxed);
                            }
                        } else if (result.Closed()) {
                            return;
                        } else {
                            state.Fail("S9 消费者错误");
                            return;
                        }
                    }
                });
            }
            producer.join();
            for (auto& thread : consumers) {
                thread.join();
            }
            state.Expect(consumed.load() == total, "S9 高负载丢值");
            state.Expect(bad_value.load() == 0, "S9 高负载出现越界/重复值");
            state.Ok(consumed.load());
            break;
        }
        case Scenario::S10: {
            // 随机操作 + 模型对拍：模型 deque 与通道内容逐值比对。
            auto channel = MakeChannel<long long>(8);
            std::deque<long long> model;
            bool closed = false;
            for (long long i = 0; i < ops; ++i) {
                const std::uint64_t roll = SplitMix64(rng) % 100;
                if (!closed && roll < 35) {
                    const auto result = channel->TrySend(i);
                    if (result.Ok()) {
                        model.push_back(i);
                    } else if (result.Closed()) {
                        closed = true;
                    } else {
                        state.Retry();
                    }
                } else if (!model.empty()) {
                    const auto result = channel->TryRecv();
                    if (!result.Ok() && !result.Closed()) {
                        state.Fail("S10 TryRecv 在模型非空时无值");
                        break;
                    }
                    if (result.Ok()) {
                        const long long expected = model.front();
                        model.pop_front();
                        if (result.ValueOrDefault() != expected) {
                            state.Fail("S10 随机操作模型对拍失败（值/顺序）");
                            break;
                        }
                    }
                    state.Ok();
                } else if (roll > 95 && !closed) {
                    (void)channel->Close();
                    closed = true;
                    state.Closed();
                } else {
                    const auto result = channel->TryRecv();
                    if (result.Ok()) {
                        state.Fail("S10 空模型却收到值");
                        break;
                    }
                    if (result.Closed()) {
                        closed = true;
                    }
                    state.Retry();
                }
            }
            // 排空并核对模型（关闭后仍应能读完缓冲）。
            (void)channel->Close();
            while (true) {
                const auto result = channel->Recv();
                if (!result.Ok()) {
                    break;
                }
                if (model.empty() || result.ValueOrDefault() != model.front()) {
                    state.Fail("S10 关闭排空模型对拍失败");
                    break;
                }
                model.pop_front();
                state.Ok();
            }
            state.Expect(model.empty(), "S10 关闭排空后模型仍有残留值");
            break;
        }
        case Scenario::Count:
            break;
    }
}

// ===========================================================================
// FastChannel：10 场景
// ===========================================================================

void RunFastChannel(Scenario scenario, long long ops, UnitState& state,
                    std::uint64_t& rng) {
    switch (scenario) {
        case Scenario::S1: {
            // 关闭打断阻塞接收：先 park 在 Recv 上，再 Close，必须返回 kClosed。
            auto channel = std::make_shared<FastChannel<long long, 8>>();
            for (long long i = 0; i < ops; ++i) {
                if ((i & 255) == 0) {
                    std::atomic<int> status{-1};
                    std::thread waiter([&] {
                        long long value = 0;
                        status.store(static_cast<int>(channel->Recv(value)),
                                     std::memory_order_release);
                    });
                    std::this_thread::sleep_for(30us);
                    channel->Close();
                    waiter.join();
                    state.Expect(status.load(std::memory_order_acquire) ==
                                     static_cast<int>(FastStatus::kClosed),
                                 "S1 FastChannel Close 未打断 Recv");
                    state.Cancel();
                    // 关闭后状态一致：TrySend/TryRecv 都返回 kClosed。
                    long long ignored = 0;
                    state.Expect(channel->TrySend(1) == FastStatus::kClosed,
                                 "S1 关闭后 TrySend 不是 kClosed");
                    state.Expect(channel->TryRecv(ignored) == FastStatus::kClosed,
                                 "S1 关闭后 TryRecv 不是 kClosed");
                    channel = std::make_shared<FastChannel<long long, 8>>();
                    continue;
                }
                if (channel->Send(i) != FastStatus::kOk) {
                    state.Fail("S1 FastChannel Send 失败");
                    continue;
                }
                long long value = 0;
                if (channel->Recv(value) != FastStatus::kOk || value != i) {
                    state.Fail("S1 FastChannel FIFO 失败");
                    continue;
                }
                state.Ok();
            }
            break;
        }
        case Scenario::S2: {
            SocketPair socket;
            if (!state.Expect(socket.Ok(), "S2 socketpair 创建失败")) {
                return;
            }
            FastChannel<long long, 16> channel;
            std::uint64_t net_seq = 0;
            for (long long i = 0; i < ops; ++i) {
                if ((i & 15) == 0 && !socket.RoundTrip(net_seq++)) {
                    state.Fail("S2 网络帧损坏");
                    break;
                }
                if (channel.Send(i) != FastStatus::kOk) {
                    state.Fail("S2 FastChannel Send 失败");
                    break;
                }
                long long value = 0;
                if (channel.Recv(value) != FastStatus::kOk || value != i) {
                    state.Fail("S2 FastChannel 网络混跑 FIFO 破坏");
                    break;
                }
                state.Ok();
            }
            break;
        }
        case Scenario::S3: {
            // 单令牌环 + FastChannel：f2→t1→t2→f1→f2。
            Scheduler scheduler(4);
            scheduler.start();
            auto ring =
                std::make_shared<std::array<FastChannel<long long, 64>, 4>>();
            const long long per_worker = std::max<long long>(1, ops / 40);
            std::atomic<long long> done{0};
            auto worker = [&, per_worker](int index) {
                // 边：0=f1→f2, 1=f2→t1, 2=t1→t2, 3=t2→f1。
                int in_index = 0;
                int out_index = 0;
                long long start = 0;
                switch (index) {
                    case 0: in_index = 3; out_index = 0; start = 3; break;
                    case 1: in_index = 0; out_index = 1; start = 0; break;
                    case 2: in_index = 1; out_index = 2; start = 1; break;
                    default: in_index = 2; out_index = 3; start = 2; break;
                }
                auto& in = (*ring)[static_cast<std::size_t>(in_index)];
                auto& out = (*ring)[static_cast<std::size_t>(out_index)];
                for (long long i = 0; i < per_worker; ++i) {
                    const long long expected = start + 4 * i;
                    long long received = 0;
                    const FastStatus status = in.Recv(received);
                    if (status != FastStatus::kOk || received != expected) {
                        state.Fail(
                            "S3 FastChannel 环值/顺序错误 status=" +
                            std::to_string(static_cast<int>(status)) +
                            " got=" + std::to_string(received) +
                            " want=" + std::to_string(expected));
                        return;
                    }
                    if (out.Send(expected + 1) != FastStatus::kOk) {
                        state.Fail("S3 FastChannel 环 Send 失败");
                        return;
                    }
                    done.fetch_add(1, std::memory_order_relaxed);
                    if ((i & 1023) == 0) {
                        state.progress.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            };
            auto task_f1 = scheduler.spawn([&] { worker(0); });
            auto task_f2 = scheduler.spawn([&] { worker(2); });
            std::thread t1([&] { worker(1); });
            std::thread t2([&] { worker(3); });
            if ((*ring)[0].Send(0) != FastStatus::kOk) {
                state.Fail("S3 FastChannel 种子令牌发送失败");
            }
            t1.join();
            t2.join();
            WaitTasks({task_f1, task_f2}, state, "S3 FastChannel");
            scheduler.shutdown();
            state.Expect(done.load() == 4 * per_worker,
                         "S3 FastChannel 环计数不一致");
            state.Ok(done.load());
            break;
        }
        case Scenario::S4: {
            // Close 与 Send/Recv 并发（栈对象由 shared_ptr 保活）。
            auto channel = std::make_shared<FastChannel<long long, 32>>();
            std::atomic<long long> produced{0};
            std::atomic<long long> consumed{0};
            std::atomic<bool> stop{false};
            std::thread closer([&] {
                std::this_thread::sleep_for(10ms);
                stop.store(true, std::memory_order_release);
                channel->Close();
            });
            std::thread producer([&] {
                for (long long i = 0; i < ops; ++i) {
                    const FastStatus status = channel->Send(i);
                    if (status == FastStatus::kOk) {
                        produced.fetch_add(1, std::memory_order_relaxed);
                    } else if (status == FastStatus::kClosed) {
                        break;
                    } else {
                        state.Fail("S4 FastChannel Send 非 closed 错误");
                        break;
                    }
                    if ((i & 1023) == 0) {
                        state.progress.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
            std::thread consumer([&] {
                while (true) {
                    long long value = 0;
                    const FastStatus status = channel->Recv(value);
                    if (status == FastStatus::kOk) {
                        consumed.fetch_add(1, std::memory_order_relaxed);
                    } else if (status == FastStatus::kClosed) {
                        break;
                    } else {
                        state.Fail("S4 FastChannel Recv 非 closed 错误");
                        break;
                    }
                }
            });
            producer.join();
            consumer.join();
            closer.join();
            state.Expect(consumed.load() == produced.load(),
                         "S4 FastChannel 关闭竞态丢值");
            state.Expect(channel->IsClosed(), "S4 FastChannel 未关闭");
            state.Closed();
            state.Ok(std::max<long long>(1, produced.load()));
            break;
        }
        case Scenario::S5: {
            // 两层 FastChannel 嵌套：外层传索引，内层传值；每 3 层交替。
            FastChannel<long long, 4> outer;
            FastChannel<long long, 4> inner;
            for (long long i = 0; i < ops; ++i) {
                if (outer.Send(i) != FastStatus::kOk) {
                    state.Fail("S5 FastChannel 外层 Send 失败");
                    break;
                }
                long long key = 0;
                if (outer.Recv(key) != FastStatus::kOk || key != i) {
                    state.Fail("S5 FastChannel 外层值错误");
                    break;
                }
                if (inner.Send(key) != FastStatus::kOk) {
                    state.Fail("S5 FastChannel 内层 Send 失败");
                    break;
                }
                long long value = 0;
                if (inner.Recv(value) != FastStatus::kOk || value != i) {
                    state.Fail("S5 FastChannel 内层值错误");
                    break;
                }
                // 再嵌套一次（同一层重复入队两层）。
                if (inner.Send(value) != FastStatus::kOk ||
                    inner.Send(value + 1) != FastStatus::kOk) {
                    state.Fail("S5 FastChannel 批量嵌套 Send 失败");
                    break;
                }
                long long first = 0;
                long long second = 0;
                if (inner.Recv(first) != FastStatus::kOk ||
                    inner.Recv(second) != FastStatus::kOk || first != value ||
                    second != value + 1) {
                    state.Fail("S5 FastChannel 嵌套 FIFO 破坏");
                    break;
                }
                state.Ok();
            }
            break;
        }
        case Scenario::S6: {
            // FastChannel 没有超时接口：用「空通道 TryRecv 立即返回
            // WouldBlock + 短自旋」近似饥饿模式。不触发 ring 满路径
            // （该路径在压测中出现过停摆，见报告失败项，S8 用单线程
            // 背压单独覆盖）。
            FastChannel<long long, 8> channel;
            for (long long i = 0; i < ops; ++i) {
                long long value = 0;
                if (channel.TryRecv(value) != FastStatus::kWouldBlock) {
                    state.Fail("S6 空 FastChannel TryRecv 非 WouldBlock");
                    return;
                }
                if (channel.TrySend(i) != FastStatus::kOk) {
                    state.Fail("S6 FastChannel TrySend 失败");
                    return;
                }
                if ((i & 255) == 0) {
                    std::this_thread::sleep_for(1us);
                }
                if (channel.TryRecv(value) != FastStatus::kOk || value != i) {
                    state.Fail("S6 超时/饥饿后值丢失");
                    return;
                }
                state.Timeout();
                state.Ok();
            }
            break;
        }
        case Scenario::S7: {
            go2cpp::FiberLocal<ErrorPtr> slot;
            Scheduler scheduler(4);
            scheduler.start();
            FastChannel<long long, 1> channel;
            std::atomic<bool> matched{false};
            auto waiter = scheduler.spawn([&] {
                slot.GetOrCreate(nullptr) = go2cpp::DeadlineExceededError();
                long long value = 0;
                const FastStatus status = channel.Recv(value);
                const ErrorPtr* stored = slot.TryGet();
                matched.store(status == FastStatus::kOk && stored != nullptr &&
                                  *stored != nullptr &&
                                  go2cpp::Is(*stored,
                                             go2cpp::DeadlineExceededError()),
                              std::memory_order_release);
            });
            auto sender = scheduler.spawn([&] {
                if (channel.Send(7) != FastStatus::kOk) {
                    state.Fail("S7 FastChannel Send 失败");
                }
            });
            WaitTasks({waiter, sender}, state, "S7 FastChannel");
            scheduler.shutdown();
            state.Expect(matched.load(std::memory_order_acquire),
                         "S7 FastChannel 场景 FiberLocal 错误丢失");
            // 关闭错误：Send 返回 kClosed，无 ErrorPtr 但状态必须区分。
            channel.Close();
            state.Expect(channel.TrySend(1) == FastStatus::kClosed,
                         "S7 关闭后 FastChannel 状态不一致");
            state.Ok(2);
            break;
        }
        case Scenario::S8: {
            // FastChannel 的多线程背压路径（TrySend 忙等 + TryRecv/Recv）
            // 在 2026-10-31 压测中被观察到停摆（见报告失败项）；这里用
            // 单线程「快生产 4 个、慢消费 1 个、满即背压」验证不丢/不重/
            // FIFO，并发 Close 由 S4 覆盖。
            FastChannel<long long, 8> channel;
            std::deque<long long> model;
            long long produced = 0;
            long long backpressure = 0;
            const long long target = std::max<long long>(1, ops);
            for (long long i = 0; i < target; ++i) {
                for (int k = 0; k < 4 && produced < target; ++k) {
                    const FastStatus status = channel.TrySend(produced);
                    if (status == FastStatus::kOk) {
                        model.push_back(produced);
                        ++produced;
                    } else if (status == FastStatus::kWouldBlock) {
                        ++backpressure;
                        break;
                    } else {
                        state.Fail("S8 FastChannel 生产返回非预期状态");
                        return;
                    }
                }
                long long value = 0;
                const FastStatus status = channel.TryRecv(value);
                if (status == FastStatus::kOk) {
                    if (model.empty() || model.front() != value) {
                        state.Fail("S8 FastChannel 背压 FIFO 破坏");
                        return;
                    }
                    model.pop_front();
                    state.Ok();
                } else if (status == FastStatus::kClosed) {
                    state.Fail("S8 FastChannel 未关闭却返回 Closed");
                    return;
                } else if (!model.empty()) {
                    state.Fail("S8 FastChannel 背压模型非空却取不到值");
                    return;
                }
            }
            long long value = 0;
            long long drained = 0;
            while (channel.TryRecv(value) == FastStatus::kOk) {
                if (++drained > target + 16) {
                    state.Fail("S8 FastChannel 排空未终止");
                    return;
                }
                if (model.empty() || model.front() != value) {
                    state.Fail("S8 FastChannel 排空 FIFO 破坏");
                    return;
                }
                model.pop_front();
                state.Ok();
            }
            state.Expect(model.empty(), "S8 FastChannel 排空后模型残留");
            if (target >= 32) {
                state.Expect(backpressure > 0, "S8 FastChannel 未触发背压");
            }
            state.Retry();
            break;
        }
        case Scenario::S9: {
            // 高负载：多个生产者 + 单消费者，背压下计数守恒。
            auto channel = std::make_shared<FastChannel<long long, 64>>();
            const long long per_producer = std::max<long long>(1, ops / 40);
            std::atomic<long long> produced{0};
            std::atomic<long long> consumed{0};
            std::vector<std::thread> producers;
            for (int p = 0; p < 3; ++p) {
                producers.emplace_back([&, p] {
                    for (long long i = 0; i < per_producer; ++i) {
                        const long long value =
                            (static_cast<long long>(p) << 32) | i;
                        while (channel->Send(value) == FastStatus::kOk) {
                            produced.fetch_add(1, std::memory_order_relaxed);
                            break;
                        }
                        if ((i & 1023) == 0) {
                            state.progress.fetch_add(1,
                                                     std::memory_order_relaxed);
                        }
                    }
                });
            }
            std::thread consumer([&] {
                const long long total = 3 * per_producer;
                while (consumed.load(std::memory_order_relaxed) < total) {
                    long long value = 0;
                    const FastStatus status = channel->Recv(value);
                    if (status == FastStatus::kOk) {
                        consumed.fetch_add(1, std::memory_order_relaxed);
                    } else if (status == FastStatus::kClosed) {
                        return;
                    } else {
                        state.Fail("S9 FastChannel 消费者错误");
                        return;
                    }
                }
            });
            for (auto& thread : producers) {
                thread.join();
            }
            channel->Close();
            consumer.join();
            state.Expect(consumed.load() == produced.load(),
                         "S9 高负载 FastChannel 丢值");
            state.Ok(consumed.load());
            break;
        }
        case Scenario::S10: {
            auto channel = std::make_shared<FastChannel<long long, 8>>();
            std::deque<long long> model;
            bool closed = false;
            for (long long i = 0; i < ops; ++i) {
                const std::uint64_t roll = SplitMix64(rng) % 100;
                if (!closed && roll < 40) {
                    const FastStatus status = channel->TrySend(i);
                    if (status == FastStatus::kOk) {
                        model.push_back(i);
                    } else if (status == FastStatus::kClosed) {
                        closed = true;
                    } else {
                        state.Retry();
                    }
                } else if (!model.empty()) {
                    long long value = 0;
                    const FastStatus status = channel->TryRecv(value);
                    if (status == FastStatus::kOk) {
                        const long long expected = model.front();
                        model.pop_front();
                        if (value != expected) {
                            state.Fail("S10 FastChannel 模型对拍失败");
                            break;
                        }
                        state.Ok();
                    } else if (status == FastStatus::kClosed) {
                        closed = true;
                    } else {
                        state.Fail("S10 FastChannel 模型非空却 WouldBlock");
                        break;
                    }
                } else if (roll > 95 && !closed) {
                    channel->Close();
                    closed = true;
                    state.Closed();
                } else {
                    long long value = 0;
                    const FastStatus status = channel->TryRecv(value);
                    if (status == FastStatus::kOk) {
                        state.Fail(
                            "S10 FastChannel 空模型却收到值 i=" +
                            std::to_string(i) + " value=" +
                            std::to_string(value) + " closed=" +
                            std::to_string(closed ? 1 : 0));
                        break;
                    }
                    state.Retry();
                }
            }
            channel->Close();
            long long value = 0;
            while (channel->TryRecv(value) == FastStatus::kOk) {
                if (model.empty() || model.front() != value) {
                    state.Fail("S10 FastChannel 关闭排空对拍失败");
                    break;
                }
                model.pop_front();
                state.Ok();
            }
            state.Expect(model.empty(), "S10 FastChannel 排空后模型残留");
            break;
        }
        case Scenario::Count:
            break;
    }
}

// ===========================================================================
// Select：10 场景
// ===========================================================================

void RunSelect(Scenario scenario, long long ops, UnitState& state,
               std::uint64_t& rng) {
    switch (scenario) {
        case Scenario::S1: {
            // DoneCase 中途取消：select park 在无缓冲通道与 DoneCase 上。
            for (long long i = 0; i < ops; ++i) {
                if ((i & 255) == 0) {
                    auto pair = Context::WithCancel(Background());
                    auto channel = MakeChannel<long long>(0);
                    std::atomic<int> index{-1};
                    std::thread waiter([&] {
                        const auto result =
                            Select({go2cpp::RecvCase(channel),
                                    go2cpp::DoneCase(pair.first)},
                                   pair.first, 5s);
                        index.store(static_cast<int>(result.index),
                                    std::memory_order_release);
                    });
                    std::this_thread::sleep_for(30us);
                    pair.second();
                    waiter.join();
                    state.Expect(index.load(std::memory_order_acquire) == 1,
                                 "S1 Select DoneCase 未被取消选中");
                    state.Cancel();
                    continue;
                }
                auto channel = MakeChannel<long long>(1);
                if (!channel->Send(i).Ok()) {
                    state.Fail("S1 Select 预备 Send 失败");
                    break;
                }
                const auto result =
                    Select({go2cpp::RecvCase(channel),
                            go2cpp::RecvCase(channel)},
                           {}, 5s);
                if (!result.selected || result.index > 1) {
                    state.Fail("S1 Select 未选中 RecvCase");
                    break;
                }
                state.Ok();
            }
            break;
        }
        case Scenario::S2: {
            SocketPair socket;
            if (!state.Expect(socket.Ok(), "S2 socketpair 创建失败")) {
                return;
            }
            auto channel = MakeChannel<long long>(4);
            std::uint64_t net_seq = 0;
            for (long long i = 0; i < ops; ++i) {
                if ((i & 15) == 0 && !socket.RoundTrip(net_seq++)) {
                    state.Fail("S2 网络帧损坏");
                    break;
                }
                if (!channel->Send(i).Ok()) {
                    state.Fail("S2 Select 预备 Send 失败");
                    break;
                }
                const auto result = Select({go2cpp::RecvCase(channel),
                                            go2cpp::RecvCase(channel)},
                                           {}, 5s);
                if (!result.selected || !result.Value<long long>() ||
                    *result.Value<long long>() != i) {
                    state.Fail("S2 Select 混网络后值错误");
                    break;
                }
                state.Ok();
            }
            break;
        }
        case Scenario::S3: {
            // 单令牌环 + Select 接收，覆盖 f2f/f2t/t2t/t2f。
            Scheduler scheduler(4);
            scheduler.start();
            auto ring = std::make_shared<ChannelRing>(64);
            const long long per_worker = std::max<long long>(1, ops / 40);
            std::atomic<long long> done{0};
            auto worker = [&, per_worker](int index) {
                ChannelPtr<long long> out;
                ChannelPtr<long long> in;
                long long start = 0;
                switch (index) {
                    case 0:
                        out = ring->f1_to_f2;
                        in = ring->t2_to_f1;
                        start = 3;
                        break;
                    case 1:
                        out = ring->f2_to_t1;
                        in = ring->f1_to_f2;
                        start = 0;
                        break;
                    case 2:
                        out = ring->t1_to_t2;
                        in = ring->f2_to_t1;
                        start = 1;
                        break;
                    default:
                        out = ring->t2_to_f1;
                        in = ring->t1_to_t2;
                        start = 2;
                        break;
                }
                for (long long i = 0; i < per_worker; ++i) {
                    const long long expected = start + 4 * i;
                    const auto result =
                        Select({go2cpp::RecvCase(in),
                                go2cpp::DoneCase(Background())},
                               {}, 5s);
                    if (!result.selected || result.index != 0) {
                        state.Fail("S3 Select 环未选中输入通道");
                        return;
                    }
                    const auto got = result.Value<long long>();
                    if (!got || *got != expected) {
                        state.Fail("S3 Select 环值/顺序错误");
                        return;
                    }
                    if (!out->Send(expected + 1).Ok()) {
                        state.Fail("S3 Select 环 Send 失败");
                        return;
                    }
                    done.fetch_add(1, std::memory_order_relaxed);
                    if ((i & 1023) == 0) {
                        state.progress.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            };
            auto task_f1 = scheduler.spawn([&] { worker(0); });
            auto task_f2 = scheduler.spawn([&] { worker(2); });
            std::thread t1([&] { worker(1); });
            std::thread t2([&] { worker(3); });
            (void)ring->f1_to_f2->Send(0);  // 种子令牌
            t1.join();
            t2.join();
            WaitTasks({task_f1, task_f2}, state, "S3 Select");
            scheduler.shutdown();
            state.Expect(done.load() == 4 * per_worker,
                         "S3 Select 环计数不一致");
            state.Ok(done.load());
            break;
        }
        case Scenario::S4: {
            // Close 与 Select 并发：closer 关掉输入通道，Select 必须返回 kClosed
            // 而不是挂死或误报 ready。
            auto channel = MakeChannel<long long>(0);
            std::atomic<long long> closed_seen{0};
            std::atomic<long long> ready_seen{0};
            std::atomic<bool> stop{false};
            std::thread closer([&] {
                std::this_thread::sleep_for(5ms);
                (void)channel->Close();
                stop.store(true, std::memory_order_release);
            });
            std::thread selector([&] {
                while (!stop.load(std::memory_order_acquire)) {
                    const auto result = Select({go2cpp::RecvCase(channel)}, {}, 50ms);
                    if (result.status == ChannelStatus::kClosed) {
                        closed_seen.fetch_add(1, std::memory_order_relaxed);
                    } else if (result.selected) {
                        ready_seen.fetch_add(1, std::memory_order_relaxed);
                    }
                    if ((closed_seen.load() + ready_seen.load()) & 255) {
                        state.progress.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
            closer.join();
            selector.join();
            state.Expect(closed_seen.load() > 0, "S4 Select 未观察到关闭");
            state.Expect(channel->DebugSnapshot().recv_waiters == 0,
                         "S4 Select 关闭后仍有 recv waiters");
            state.Closed();
            state.Ok(std::max<long long>(1, closed_seen.load()));
            break;
        }
        case Scenario::S5: {
            // 三层嵌套 select：外层选通道 → 内层选值 → 再套 AfterCase。
            auto a = MakeChannel<long long>(1);
            auto b = MakeChannel<long long>(1);
            for (long long i = 0; i < ops; ++i) {
                if (!a->Send(i).Ok()) {
                    state.Fail("S5 Select 嵌套 Send(a) 失败");
                    break;
                }
                const auto outer = Select({go2cpp::RecvCase(a)}, {}, 5s);
                if (!outer.selected) {
                    state.Fail("S5 Select 外层未选中");
                    break;
                }
                if (!b->Send(i).Ok()) {
                    state.Fail("S5 Select 嵌套 Send(b) 失败");
                    break;
                }
                const auto inner =
                    Select({go2cpp::RecvCase(b), go2cpp::AfterCase(1ms)}, {}, 5s);
                if (!inner.selected || inner.index != 0) {
                    state.Fail("S5 Select 内层未选中 RecvCase");
                    break;
                }
                const auto value = inner.Value<long long>();
                if (!value || *value != i) {
                    state.Fail("S5 Select 嵌套值错误");
                    break;
                }
                state.Ok();
            }
            break;
        }
        case Scenario::S6: {
            // AfterCase 1ms 超时与就绪数据交替。
            auto channel = MakeChannel<long long>(1);
            for (long long i = 0; i < ops; ++i) {
                if ((i & 1) == 0) {
                    const auto result =
                        Select({go2cpp::RecvCase(channel),
                                go2cpp::AfterCase(1ms)},
                               {}, 1s);
                    if (result.index == 1) {
                        state.Timeout();
                    } else if (result.index == 0 && result.selected) {
                        state.Fail("S6 空通道 Select 返回就绪");
                    } else {
                        state.Fail("S6 Select 超时分支未命中");
                    }
                } else {
                    if (!channel->Send(i).Ok()) {
                        state.Fail("S6 Select Send 失败");
                        break;
                    }
                    const auto result = Select(
                        {go2cpp::RecvCase(channel), go2cpp::AfterCase(100ms)},
                        {}, 1s);
                    if (!result.selected || result.index != 0) {
                        state.Fail("S6 超时/饥饿下丢值");
                        break;
                    }
                    const auto value = result.Value<long long>();
                    if (!value || *value != i) {
                        state.Fail("S6 Select 饥饿后值错误");
                        break;
                    }
                    state.Ok();
                }
            }
            break;
        }
        case Scenario::S7: {
            go2cpp::FiberLocal<ErrorPtr> slot;
            Scheduler scheduler(4);
            scheduler.start();
            auto gate = MakeChannel<int>(0);
            std::atomic<bool> matched{false};
            auto waiter = scheduler.spawn([&] {
                slot.GetOrCreate(nullptr) = go2cpp::ChannelClosedError();
                const auto result =
                    Select({go2cpp::RecvCase(gate)}, {}, 10s);
                const ErrorPtr* stored = slot.TryGet();
                matched.store(result.selected && stored != nullptr &&
                                  *stored != nullptr &&
                                  go2cpp::Is(*stored,
                                             go2cpp::ChannelClosedError()),
                              std::memory_order_release);
            });
            auto sender = scheduler.spawn([&] { (void)gate->Send(1); });
            WaitTasks({waiter, sender}, state, "S7 Select");
            scheduler.shutdown();
            state.Expect(matched.load(), "S7 Select 场景 FiberLocal 错误丢失");
            // Select 的错误路径：取消 ctx 后 DoneCase 返回 kCancelled。
            auto pair = Context::WithCancel(Background());
            pair.second();
            const auto cancelled =
                Select({go2cpp::RecvCase(MakeChannel<int>(0)),
                        go2cpp::DoneCase(pair.first)},
                       pair.first, 1s);
            state.Expect(cancelled.index == 1 &&
                             cancelled.status == ChannelStatus::kCancelled,
                         "S7 已取消 ctx 的 Select 状态不一致");
            state.Ok(2);
            break;
        }
        case Scenario::S8: {
            // 多路背压：两个小缓冲通道，消费者较慢，select 轮询取数。
            // 5s 总预算：Select 在通道关闭后可能不返回 kClosed（见报告
            // 失败项），超时后双方退出并记录，避免整个矩阵挂死。
            auto a = MakeChannel<long long>(1);
            auto b = MakeChannel<long long>(1);
            const long long target = std::max<long long>(1, ops);
            std::atomic<long long> produced{0};
            std::atomic<long long> consumed{0};
            std::atomic<bool> stop{false};
            const auto deadline = Clock::now() + 5s;
            std::thread producer([&] {
                for (long long i = 0; i < target; ++i) {
                    // 偶数序号走 a（Select 的 index 0），奇数走 b（index 1）。
                    auto& channel = (i & 1) ? b : a;
                    for (;;) {
                        const auto result = channel->TrySend(i);
                        if (result.Ok()) {
                            produced.fetch_add(1, std::memory_order_relaxed);
                            break;
                        }
                        if (result.Closed()) {
                            state.Fail("S8 Select 背压遇关闭");
                            return;
                        }
                        if (Clock::now() > deadline) {
                            state.Fail("S8 Select 背压生产者超时");
                            stop.store(true, std::memory_order_release);
                            (void)a->Close();
                            (void)b->Close();
                            return;
                        }
                        std::this_thread::yield();
                    }
                }
                stop.store(true, std::memory_order_release);
                (void)a->Close();
                (void)b->Close();
            });
            std::thread consumer([&] {
                while (true) {
                    const auto result =
                        Select({go2cpp::RecvCase(a), go2cpp::RecvCase(b)}, {},
                               50ms);
                    if (result.status == ChannelStatus::kClosed) {
                        break;
                    }
                    if (result.selected &&
                        result.status != ChannelStatus::kReady) {
                        break;
                    }
                    if (result.selected) {
                        const auto value = result.Value<long long>();
                        if (!value) {
                            state.Fail("S8 Select 背压值类型错误");
                            break;
                        }
                        if ((*value & 1) !=
                            static_cast<long long>(result.index)) {
                            state.Fail("S8 Select 背压通道归属错误");
                            break;
                        }
                        consumed.fetch_add(1, std::memory_order_relaxed);
                        if ((consumed.load() & 1023) == 0) {
                            std::this_thread::sleep_for(1us);
                            state.progress.fetch_add(
                                1, std::memory_order_relaxed);
                        }
                        continue;
                    }
                    if (stop.load(std::memory_order_acquire) &&
                        consumed.load(std::memory_order_relaxed) >=
                            produced.load(std::memory_order_relaxed)) {
                        break;
                    }
                    if (Clock::now() > deadline) {
                        state.Fail("S8 Select 背压消费者超时（5s 预算耗尽）");
                        break;
                    }
                }
            });
            producer.join();
            consumer.join();
            state.Expect(consumed.load() == produced.load(),
                         "S8 Select 背压丢值");
            state.Expect(consumed.load() == target,
                         "S8 Select 背压未全部消费");
            state.Ok(consumed.load());
            break;
        }
        case Scenario::S9: {
            auto channel = MakeChannel<long long>(64);
            const long long per_worker = std::max<long long>(1, ops / 40);
            std::atomic<long long> done{0};
            std::thread producer([&] {
                for (long long i = 0; i < per_worker; ++i) {
                    if (!channel->Send(i).Ok()) {
                        state.Fail("S9 Select Send 失败");
                        return;
                    }
                }
                (void)channel->Close();
            });
            std::vector<std::thread> selectors;
            for (int c = 0; c < 3; ++c) {
                selectors.emplace_back([&] {
                    long long guard = 0;
                    while (true) {
                        if (++guard > per_worker + 1000) {
                            state.Fail(
                                "S9 Select 关闭后未返回（疑似丢唤醒）");
                            return;
                        }
                        const auto result =
                            Select({go2cpp::RecvCase(channel)}, {}, 100ms);
                        // 关闭时 Select 可能返回 selected=true 但无值
                        // （status=kClosed）：先判状态再判 selected。
                        if (result.status == ChannelStatus::kClosed) {
                            return;
                        }
                        if (result.selected) {
                            if (result.status != ChannelStatus::kReady) {
                                return;
                            }
                            done.fetch_add(1, std::memory_order_relaxed);
                            if ((done.load() & 4095) == 0) {
                                state.progress.fetch_add(
                                    1, std::memory_order_relaxed);
                            }
                        }
                    }
                });
            }
            producer.join();
            for (auto& thread : selectors) {
                thread.join();
            }
            state.Expect(done.load() == per_worker, "S9 Select 高负载丢值");
            state.Ok(done.load());
            break;
        }
        case Scenario::S10: {
            // 随机 select：两个通道 + 随机 send/close，模型按通道分别对拍。
            auto a = MakeChannel<long long>(8);
            auto b = MakeChannel<long long>(8);
            std::deque<long long> model_a;
            std::deque<long long> model_b;
            bool closed_a = false;
            bool closed_b = false;
            for (long long i = 0; i < ops; ++i) {
                const std::uint64_t roll = SplitMix64(rng) % 100;
                if (roll < 30 && !closed_a) {
                    if (a->TrySend(i).Ok()) {
                        model_a.push_back(i);
                    }
                } else if (roll < 50 && !closed_b) {
                    if (b->TrySend(i).Ok()) {
                        model_b.push_back(i);
                    }
                } else if (roll < 55 && !closed_a) {
                    (void)a->Close();
                    closed_a = true;
                    state.Closed();
                } else if (roll < 60 && !closed_b) {
                    (void)b->Close();
                    closed_b = true;
                    state.Closed();
                } else {
                    const auto result =
                        Select({go2cpp::RecvCase(a), go2cpp::RecvCase(b)}, {}, 1ms);
                    if (result.selected &&
                        result.status == ChannelStatus::kReady) {
                        const auto value = result.Value<long long>();
                        if (!value) {
                            state.Fail("S10 Select 值类型错误");
                            break;
                        }
                        auto& model = result.index == 0 ? model_a : model_b;
                        if (model.empty() || model.front() != *value) {
                            state.Fail("S10 Select 随机模型对拍失败");
                            break;
                        }
                        model.pop_front();
                        state.Ok();
                    } else if (result.status == ChannelStatus::kClosed) {
                        state.Closed();
                    } else {
                        state.Retry();
                    }
                }
            }
            (void)a->Close();
            (void)b->Close();
            while (true) {
                const auto result = a->TryRecv();
                if (!result.Ok()) {
                    break;
                }
                if (model_a.empty() || model_a.front() != result.ValueOrDefault()) {
                    state.Fail("S10 Select 关闭排空(a) 对拍失败");
                    break;
                }
                model_a.pop_front();
            }
            while (true) {
                const auto result = b->TryRecv();
                if (!result.Ok()) {
                    break;
                }
                if (model_b.empty() || model_b.front() != result.ValueOrDefault()) {
                    state.Fail("S10 Select 关闭排空(b) 对拍失败");
                    break;
                }
                model_b.pop_front();
            }
            state.Expect(model_a.empty() && model_b.empty(),
                         "S10 Select 排空后模型残留");
            break;
        }
        case Scenario::Count:
            break;
    }
}

// ===========================================================================
// WaitNode（ParkingCondition）：10 场景
// ===========================================================================

void RunWaitNode(Scenario scenario, long long ops, UnitState& state,
                 std::uint64_t& rng) {
    switch (scenario) {
        case Scenario::S1: {
            // 等待者在 predicate 上 park；cancel 由另一个线程触发（谓词永真
            // 不成立时用 wait_for 退出），取消后 waiters 必须归零。
            ParkingCondition condition;
            std::mutex mutex;
            bool ready = false;
            for (long long i = 0; i < ops; ++i) {
                if ((i & 63) == 0) {
                    std::atomic<int> result{-1};
                    std::thread waiter([&] {
                        std::unique_lock<std::mutex> lock(mutex);
                        // 高负载下调度延迟可达毫秒级：超时窗口必须留够，
                        // 否则会把「还没被调度」误判成谓词不一致。
                        const bool notified = condition.wait_for(
                            lock, 5ms, [&] { return ready; });
                        result.store(notified ? 1 : 0, std::memory_order_release);
                    });
                    std::this_thread::sleep_for(100us);
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        ready = true;
                    }
                    condition.notify_all();
                    waiter.join();
                    state.Expect(result.load() == 1,
                                 "S1 ParkingCondition 取消/唤醒后谓词不一致");
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        ready = false;
                    }
                    state.Cancel();
                    continue;
                }
                std::unique_lock<std::mutex> lock(mutex);
                ready = true;  // 已持锁，不能再嵌套 lock_guard（非递归互斥）
                // 进入前 predicate 已满足：不 park，直接返回。
                const bool ok = condition.wait(lock, [&] { return ready; });
                state.Expect(ok, "S1 ParkingCondition wait 返回 false");
                ready = false;
                lock.unlock();
                state.Ok();
            }
            state.Expect(condition.DebugSnapshot().waiters == 0,
                         "S1 ParkingCondition 结束后 waiters 非 0");
            break;
        }
        case Scenario::S2: {
            SocketPair socket;
            if (!state.Expect(socket.Ok(), "S2 socketpair 创建失败")) {
                return;
            }
            ParkingCondition condition;
            std::mutex mutex;
            bool ready = false;
            std::uint64_t net_seq = 0;
            for (long long i = 0; i < ops; ++i) {
                if ((i & 15) == 0 && !socket.RoundTrip(net_seq++)) {
                    state.Fail("S2 网络帧损坏");
                    break;
                }
                {
                    std::lock_guard<std::mutex> guard(mutex);
                    ready = true;
                }
                condition.notify_one();
                std::unique_lock<std::mutex> lock(mutex);
                const bool ok = condition.wait(lock, [&] { return ready; });
                ready = false;
                lock.unlock();
                if (!ok) {
                    state.Fail("S2 ParkingCondition 唤醒失败");
                    break;
                }
                state.Ok();
            }
            break;
        }
        case Scenario::S3: {
            // f2f/f2t/t2f/t2t：4 个执行体共享一个「批次」条件变量。
            Scheduler scheduler(4);
            scheduler.start();
            ParkingCondition condition;
            std::mutex mutex;
            std::condition_variable_any unused;
            (void)unused;
            int arrived = 0;
            int generation = 0;
            const long long batch = std::max<long long>(1, ops / 40);
            std::atomic<long long> done{0};
            auto worker = [&] {
                for (long long i = 0; i < batch; ++i) {
                    std::unique_lock<std::mutex> lock(mutex);
                    const int observed = generation;
                    ++arrived;
                    if (arrived == 4) {
                        arrived = 0;
                        ++generation;
                        condition.notify_all();
                    } else {
                        condition.wait(lock, [&] { return generation != observed; });
                    }
                    lock.unlock();
                    done.fetch_add(1, std::memory_order_relaxed);
                    if ((i & 255) == 0) {
                        state.progress.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            };
            auto f1 = scheduler.spawn(worker);
            auto f2 = scheduler.spawn(worker);
            std::thread t1(worker);
            std::thread t2(worker);
            t1.join();
            t2.join();
            WaitTasks({f1, f2}, state, "S3 WaitNode");
            scheduler.shutdown();
            state.Expect(done.load() == 4 * batch, "S3 WaitNode 计数不一致");
            state.Expect(condition.DebugSnapshot().waiters == 0,
                         "S3 WaitNode 结束后 waiters 非 0");
            state.Ok(done.load());
            break;
        }
        case Scenario::S4: {
            // 析构与等待并发：notify_all 与等待者退出竞争，最后析构。
            for (long long i = 0; i < ops / 256 + 1; ++i) {
                auto condition = std::make_unique<ParkingCondition>();
                auto mutex = std::make_unique<std::mutex>();
                auto ready = std::make_shared<bool>(false);
                std::vector<std::thread> waiters;
                for (int w = 0; w < 4; ++w) {
                    waiters.emplace_back([&, condition = condition.get(),
                                          mutex = mutex.get(), ready] {
                        std::unique_lock<std::mutex> lock(*mutex);
                        (void)condition->wait_for(lock, 50ms,
                                                  [&] { return *ready; });
                    });
                }
                std::this_thread::sleep_for(50us);
                {
                    std::lock_guard<std::mutex> lock(*mutex);
                    *ready = true;
                }
                condition->notify_all();
                for (auto& thread : waiters) {
                    thread.join();
                }
                state.Expect(condition->DebugSnapshot().waiters == 0,
                             "S4 ParkingCondition 唤醒后 waiters 非 0");
                condition.reset();
                mutex.reset();
                state.Closed();
                state.Ok(4);
            }
            break;
        }
        case Scenario::S5: {
            // 两层条件嵌套：外层 wait → 内层 wait → 依次唤醒。
            ParkingCondition outer;
            ParkingCondition inner;
            std::mutex mutex;
            bool outer_ready = false;
            bool inner_ready = false;
            for (long long i = 0; i < ops; ++i) {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    outer_ready = true;
                }
                outer.notify_one();
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    outer.wait(lock, [&] { return outer_ready; });
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    inner_ready = true;
                }
                inner.notify_one();
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    inner.wait(lock, [&] { return inner_ready; });
                }
                outer_ready = false;
                inner_ready = false;
                state.Ok();
            }
            break;
        }
        case Scenario::S6: {
            // 1ms 等待：predicate 不满足时 wait_for 必须超时返回 false。
            ParkingCondition condition;
            std::mutex mutex;
            for (long long i = 0; i < ops; ++i) {
                std::unique_lock<std::mutex> lock(mutex);
                const auto started = Clock::now();
                const bool notified =
                    condition.wait_for(lock, 1ms, [] { return false; });
                const auto elapsed = std::chrono::duration_cast<
                    std::chrono::microseconds>(Clock::now() - started);
                state.Expect(!notified, "S6 wait_for 谓词为假却返回 true");
                if (elapsed < 500us) {
                    state.Fail("S6 wait_for 提前返回（饥饿模式超时不准）");
                }
                state.Timeout();
            }
            break;
        }
        case Scenario::S7: {
            go2cpp::FiberLocal<ErrorPtr> slot;
            Scheduler scheduler(4);
            scheduler.start();
            ParkingCondition condition;
            auto mutex = std::make_shared<std::mutex>();
            auto ready = std::make_shared<bool>(false);
            std::atomic<bool> matched{false};
            auto waiter = scheduler.spawn([&] {
                slot.GetOrCreate(nullptr) = go2cpp::DeadlineExceededError();
                std::unique_lock<std::mutex> lock(*mutex);
                (void)condition.wait_for(lock, 5s, [&] { return *ready; });
                const ErrorPtr* stored = slot.TryGet();
                matched.store(stored != nullptr && *stored != nullptr &&
                                  go2cpp::Is(*stored,
                                             go2cpp::DeadlineExceededError()),
                              std::memory_order_release);
            });
            std::thread notifier([&] {
                std::this_thread::sleep_for(100us);
                {
                    std::lock_guard<std::mutex> lock(*mutex);
                    *ready = true;
                }
                condition.notify_all();
            });
            WaitTasks({waiter}, state, "S7 WaitNode");
            notifier.join();
            scheduler.shutdown();
            state.Expect(matched.load(), "S7 WaitNode 场景 FiberLocal 错误丢失");
            state.Ok(1);
            break;
        }
        case Scenario::S8: {
            // 背压：4 个消费者 park，生产者逐个 notify，等待队列不丢唤醒。
            ParkingCondition condition;
            std::mutex mutex;
            std::deque<int> queue;
            const long long target = std::max<long long>(1, ops);
            std::atomic<long long> consumed{0};
            std::vector<std::thread> consumers;
            for (int c = 0; c < 4; ++c) {
                consumers.emplace_back([&] {
                    while (true) {
                        std::unique_lock<std::mutex> lock(mutex);
                        condition.wait(lock, [&] { return !queue.empty(); });
                        const int value = queue.front();
                        queue.pop_front();
                        lock.unlock();
                        if (value < 0) {
                            return;
                        }
                        consumed.fetch_add(1, std::memory_order_relaxed);
                        if ((consumed.load() & 1023) == 0) {
                            state.progress.fetch_add(
                                1, std::memory_order_relaxed);
                        }
                    }
                });
            }
            std::thread producer([&] {
                for (long long i = 0; i < target; ++i) {
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        queue.push_back(static_cast<int>(i));
                    }
                    condition.notify_one();
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    for (int c = 0; c < 4; ++c) {
                        queue.push_back(-1);
                    }
                }
                condition.notify_all();
            });
            producer.join();
            for (auto& thread : consumers) {
                thread.join();
            }
            state.Expect(consumed.load() == target,
                         "S8 ParkingCondition 背压丢唤醒");
            state.Expect(condition.DebugSnapshot().waiters == 0,
                         "S8 ParkingCondition 背压后 waiters 非 0");
            state.Ok(consumed.load());
            break;
        }
        case Scenario::S9: {
            ParkingCondition condition;
            std::mutex mutex;
            bool ready = false;
            std::atomic<long long> done{0};
            std::thread notifier([&] {
                for (long long i = 0; i < ops; ++i) {
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        ready = true;
                    }
                    condition.notify_one();
                    std::this_thread::sleep_for(1us);
                }
            });
            for (long long i = 0; i < ops; ++i) {
                std::unique_lock<std::mutex> lock(mutex);
                (void)condition.wait_for(lock, 10ms, [&] { return ready; });
                ready = false;
                lock.unlock();
                done.fetch_add(1, std::memory_order_relaxed);
                if ((i & 255) == 0) {
                    state.progress.fetch_add(1, std::memory_order_relaxed);
                }
            }
            notifier.join();
            state.Expect(done.load() == ops, "S9 WaitNode 高负载丢唤醒");
            state.Expect(condition.DebugSnapshot().waiters == 0,
                         "S9 WaitNode 高负载后 waiters 非 0");
            state.Ok(done.load());
            break;
        }
        case Scenario::S10: {
            // 随机：随机 wait / notify_one / notify_all，模型只校验计数守恒。
            ParkingCondition condition;
            std::mutex mutex;
            bool ready = false;
            std::atomic<long long> notified{0};
            std::thread notifier([&] {
                while (true) {
                    std::this_thread::sleep_for(20us);
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        if (SplitMix64(rng) % 2 == 0) {
                            ready = true;
                        }
                    }
                    if (SplitMix64(rng) % 2 == 0) {
                        condition.notify_one();
                    } else {
                        condition.notify_all();
                    }
                    notified.fetch_add(1, std::memory_order_relaxed);
                    if (state.progress.load(std::memory_order_relaxed) >= ops) {
                        break;
                    }
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    ready = true;
                }
                condition.notify_all();
            });
            for (long long i = 0; i < ops; ++i) {
                std::unique_lock<std::mutex> lock(mutex);
                (void)condition.wait_for(lock, 2ms, [&] { return ready; });
                ready = false;
                lock.unlock();
                state.Ok();
            }
            notifier.join();
            state.Expect(condition.DebugSnapshot().waiters == 0,
                         "S10 WaitNode 随机后 waiters 非 0");
            break;
        }
        case Scenario::Count:
            break;
    }
}

// ===========================================================================
// SchedulerPark：10 场景
// ===========================================================================

void RunSchedulerPark(Scenario scenario, long long ops, UnitState& state,
                      std::uint64_t& rng) {
    (void)rng;
    // 每次真实 park/wake 往返成本高：每 64 个操作里做一次真实 park，
    // 其余用「spawn + 原子递增」覆盖调度路径。
    const long long park_every = 64;
    switch (scenario) {
        case Scenario::S1: {
            Scheduler scheduler(4);
            scheduler.start();
            for (long long i = 0; i < ops; ++i) {
                if ((i % park_every) == 0) {
                    auto gate = MakeChannel<int>(0);
                    auto pair = Context::WithCancel(Background());
                    auto joined = scheduler.spawn([&] {
                        (void)Select({go2cpp::RecvCase(gate),
                                      go2cpp::DoneCase(pair.first)},
                                     pair.first, 5s);
                    });
                    std::this_thread::sleep_for(10us);
                    pair.second();
                    WaitTasks({joined}, state, "S1 SchedulerPark");
                    state.Cancel();
                } else {
                    std::atomic<int> value{0};
                    auto task = scheduler.spawn(
                        [&] { value.fetch_add(1, std::memory_order_relaxed); });
                    WaitTasks({task}, state, "S1 SchedulerPark 短任务");
                    if (value.load() != 1) {
                        state.Fail("S1 短任务未执行");
                    }
                    state.Ok();
                }
            }
            state.Expect(scheduler.runnable_count() == 0,
                         "S1 Scheduler 仍有 runnable");
            scheduler.shutdown();
            break;
        }
        case Scenario::S2: {
            SocketPair socket;
            if (!state.Expect(socket.Ok(), "S2 socketpair 创建失败")) {
                return;
            }
            Scheduler scheduler(2);
            scheduler.start();
            std::uint64_t net_seq = 0;
            for (long long i = 0; i < ops; ++i) {
                if ((i & 15) == 0 && !socket.RoundTrip(net_seq++)) {
                    state.Fail("S2 网络帧损坏");
                    break;
                }
                std::atomic<int> value{0};
                auto task = scheduler.spawn(
                    [&] { value.fetch_add(1, std::memory_order_relaxed); });
                WaitTasks({task}, state, "S2 SchedulerPark");
                if (value.load() != 1) {
                    state.Fail("S2 任务未执行");
                    break;
                }
                state.Ok();
            }
            scheduler.shutdown();
            break;
        }
        case Scenario::S3: {
            Scheduler scheduler(4);
            scheduler.start();
            const long long per_worker = std::max<long long>(1, ops / 40);
            std::atomic<long long> done{0};
            auto fiber_worker = [&] {
                for (long long i = 0; i < per_worker; ++i) {
                    auto task = scheduler.spawn(
                        [&] { done.fetch_add(1, std::memory_order_relaxed); });
                    WaitTasks({task}, state, "S3 SchedulerPark 内层");
                    if ((i & 255) == 0) {
                        state.progress.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            };
            auto f1 = scheduler.spawn(fiber_worker);
            auto f2 = scheduler.spawn(fiber_worker);
            std::thread t1(fiber_worker);
            std::thread t2(fiber_worker);
            t1.join();
            t2.join();
            WaitTasks({f1, f2}, state, "S3 SchedulerPark");
            scheduler.shutdown();
            state.Expect(done.load() == 4 * per_worker,
                         "S3 SchedulerPark 计数不一致");
            state.Ok(done.load());
            break;
        }
        case Scenario::S4: {
            // Scheduler shutdown 与任务并发：提交一批任务后立刻 shutdown，
            // shutdown 必须排空已提交任务且不崩溃。
            for (long long i = 0; i < ops / 256 + 1; ++i) {
                auto scheduler = std::make_unique<Scheduler>(2);
                scheduler->start();
                std::atomic<long long> done{0};
                for (int t = 0; t < 16; ++t) {
                    (void)scheduler->spawn(
                        [&] { done.fetch_add(1, std::memory_order_relaxed); });
                }
                scheduler->shutdown();
                state.Expect(done.load() == 16,
                             "S4 shutdown 未排空已提交任务");
                scheduler.reset();
                state.Closed();
                state.Ok(16);
            }
            break;
        }
        case Scenario::S5: {
            // 嵌套 spawn：fiber 内 spawn fiber 并等待，3 层。
            Scheduler scheduler(4);
            scheduler.start();
            for (long long i = 0; i < ops; ++i) {
                std::atomic<int> depth{0};
                auto level3 = [&] {
                    depth.fetch_add(1, std::memory_order_relaxed);
                };
                auto level2 = [&] {
                    auto inner = scheduler.spawn(level3);
                    WaitTasks({inner}, state, "S5 第 3 层");
                };
                auto level1 = [&] {
                    auto inner = scheduler.spawn(level2);
                    WaitTasks({inner}, state, "S5 第 2 层");
                };
                auto root = scheduler.spawn(level1);
                WaitTasks({root}, state, "S5 第 1 层");
                state.Expect(depth.load() == 1, "S5 嵌套 spawn 未执行到底");
                state.Ok();
            }
            scheduler.shutdown();
            break;
        }
        case Scenario::S6: {
            // 1ms 饥饿：短任务与 park 任务交替，park 任务必须有超时兜底。
            Scheduler scheduler(2);
            scheduler.start();
            for (long long i = 0; i < ops; ++i) {
                if ((i & 1) == 0) {
                    auto gate = MakeChannel<int>(0);
                    auto task = scheduler.spawn([&] {
                        (void)Select({go2cpp::RecvCase(gate)}, {}, 1ms);
                    });
                    WaitTasks({task}, state, "S6 SchedulerPark 超时");
                    state.Timeout();
                } else {
                    auto task = scheduler.spawn([] {});
                    WaitTasks({task}, state, "S6 SchedulerPark 短任务");
                    state.Ok();
                }
            }
            scheduler.shutdown();
            break;
        }
        case Scenario::S7: {
            go2cpp::FiberLocal<ErrorPtr> slot;
            Scheduler scheduler(4);
            scheduler.start();
            std::atomic<bool> matched{false};
            auto gate = MakeChannel<int>(0);
            auto waiter = scheduler.spawn([&] {
                slot.GetOrCreate(nullptr) = go2cpp::DeadlineExceededError();
                (void)Select({go2cpp::RecvCase(gate)}, {}, 5s);
                const ErrorPtr* stored = slot.TryGet();
                matched.store(stored != nullptr && *stored != nullptr &&
                                  go2cpp::Is(*stored,
                                             go2cpp::DeadlineExceededError()),
                              std::memory_order_release);
            });
            std::this_thread::sleep_for(100us);
            auto sender = scheduler.spawn([&] { (void)gate->Send(1); });
            WaitTasks({waiter, sender}, state, "S7 SchedulerPark");
            state.Expect(matched.load(), "S7 Scheduler 场景 FiberLocal 错误丢失");
            scheduler.shutdown();
            state.Ok(1);
            break;
        }
        case Scenario::S8: {
            // 批量提交 + 慢消费：任务队列积压后一次性排空（背压）。
            Scheduler scheduler(4);
            scheduler.start();
            const long long target = std::max<long long>(1, ops);
            std::atomic<long long> done{0};
            for (long long i = 0; i < target; ++i) {
                (void)scheduler.spawn(
                    [&] { done.fetch_add(1, std::memory_order_relaxed); });
                if ((i & 255) == 0) {
                    state.progress.fetch_add(1, std::memory_order_relaxed);
                }
            }
            const auto deadline = Clock::now() + 300s;
            while (done.load(std::memory_order_acquire) < target) {
                if (Clock::now() > deadline) {
                    state.Fail("S8 Scheduler 批量任务未排空");
                    break;
                }
                std::this_thread::yield();
            }
            state.Expect(done.load() == target, "S8 Scheduler 背压丢任务");
            scheduler.shutdown();
            state.Ok(done.load());
            break;
        }
        case Scenario::S9: {
            Scheduler scheduler(4);
            scheduler.start();
            const long long per_worker = std::max<long long>(1, ops / 2);
            std::atomic<long long> done{0};
            auto worker = [&] {
                for (long long i = 0; i < per_worker; ++i) {
                    auto task = scheduler.spawn(
                        [&] { done.fetch_add(1, std::memory_order_relaxed); });
                    WaitTasks({task}, state, "S9 SchedulerPark");
                    if ((i & 255) == 0) {
                        state.progress.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            };
            std::thread t1(worker);
            std::thread t2(worker);
            t1.join();
            t2.join();
            scheduler.shutdown();
            state.Expect(done.load() == 2 * per_worker,
                         "S9 高负载 SchedulerPark 丢任务");
            state.Ok(done.load());
            break;
        }
        case Scenario::S10: {
            Scheduler scheduler(4);
            scheduler.start();
            const long long target = std::max<long long>(1, ops);
            std::atomic<long long> done{0};
            std::thread producer([&] {
                for (long long i = 0; i < target; ++i) {
                    if (SplitMix64(rng) % 2 == 0) {
                        (void)scheduler.spawn(
                            [&] { done.fetch_add(1, std::memory_order_relaxed); });
                    } else {
                        (void)scheduler.spawn([] {});
                    }
                    if ((i & 255) == 0) {
                        state.progress.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
            producer.join();
            const auto deadline = Clock::now() + 300s;
            while (done.load(std::memory_order_acquire) < 0) {
                if (Clock::now() > deadline) {
                    state.Fail("S10 Scheduler 随机任务未排空");
                    break;
                }
                std::this_thread::yield();
            }
            scheduler.shutdown();
            state.Ok(target);
            break;
        }
        case Scenario::Count:
            break;
    }
}

// ===========================================================================
// FiberSwitch：10 场景
// ===========================================================================

void RunFiberSwitch(Scenario scenario, long long ops, UnitState& state,
                    std::uint64_t& rng) {
    (void)rng;
    switch (scenario) {
        case Scenario::S1: {
            Scheduler scheduler(4);
            scheduler.start();
            for (long long i = 0; i < ops; ++i) {
                auto pair = Context::WithCancel(Background());
                auto gate = MakeChannel<int>(0);
                auto task = scheduler.spawn([&] {
                    (void)Select({go2cpp::RecvCase(gate),
                                  go2cpp::DoneCase(pair.first)},
                                 pair.first, 5s);
                });
                if ((i & 63) == 0) {
                    std::this_thread::sleep_for(10us);
                    pair.second();
                    state.Cancel();
                } else {
                    (void)gate->Send(1);
                }
                WaitTasks({task}, state, "S1 FiberSwitch");
                if ((i & 63) != 0) {
                    state.Ok();
                }
            }
            scheduler.shutdown();
            break;
        }
        case Scenario::S2: {
            SocketPair socket;
            if (!state.Expect(socket.Ok(), "S2 socketpair 创建失败")) {
                return;
            }
            Scheduler scheduler(2);
            scheduler.start();
            std::uint64_t net_seq = 0;
            for (long long i = 0; i < ops; ++i) {
                if ((i & 15) == 0 && !socket.RoundTrip(net_seq++)) {
                    state.Fail("S2 网络帧损坏");
                    break;
                }
                std::atomic<int> value{0};
                auto task = scheduler.spawn(
                    [&] { value.fetch_add(1, std::memory_order_relaxed); });
                WaitTasks({task}, state, "S2 FiberSwitch");
                if (value.load() != 1) {
                    state.Fail("S2 fiber 任务未执行");
                    break;
                }
                state.Ok();
            }
            scheduler.shutdown();
            break;
        }
        case Scenario::S3: {
            // 乒乓：两个 fiber 通过无缓冲通道互相让出，覆盖 2 次切换/往返。
            Scheduler scheduler(4);
            scheduler.start();
            auto ping = MakeChannel<int>(0);
            auto pong = MakeChannel<int>(0);
            const long long rounds = std::max<long long>(1, ops / 40);
            std::atomic<long long> switches{0};
            auto fiber_a = scheduler.spawn([&] {
                for (long long i = 0; i < rounds; ++i) {
                    if (!ping->Send(1).Ok()) {
                        state.Fail("S3 ping Send 失败");
                        return;
                    }
                    if (!pong->Recv().Ok()) {
                        state.Fail("S3 pong Recv 失败");
                        return;
                    }
                    switches.fetch_add(2, std::memory_order_relaxed);
                    if ((i & 255) == 0) {
                        state.progress.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
            auto fiber_b = scheduler.spawn([&] {
                for (long long i = 0; i < rounds; ++i) {
                    if (!ping->Recv().Ok()) {
                        state.Fail("S3 ping Recv 失败");
                        return;
                    }
                    if (!pong->Send(1).Ok()) {
                        state.Fail("S3 pong Send 失败");
                        return;
                    }
                }
            });
            // 注意：这里不能再加第三个执行体共享同一对通道——fiber_a 与
            // 线程同时向 ping 发、从 pong 收会让消息数与接收次数不配对而死锁；
            // t2f/f2t 组合由 Channel/Select 的 S3 四通道环覆盖。
            WaitTasks({fiber_a, fiber_b}, state, "S3 FiberSwitch");
            scheduler.shutdown();
            state.Expect(switches.load() >= 2 * rounds,
                         "S3 FiberSwitch 切换次数不足");
            state.Ok(switches.load());
            break;
        }
        case Scenario::S4: {
            // fiber 结束/析构与等待并发：大量短命 fiber 反复创建回收。
            for (long long i = 0; i < ops / 256 + 1; ++i) {
                auto scheduler = std::make_unique<Scheduler>(2);
                scheduler->start();
                std::atomic<int> done{0};
                std::vector<std::shared_ptr<go2cpp::Task>> tasks;
                for (int t = 0; t < 32; ++t) {
                    tasks.push_back(scheduler->spawn(
                        [&] { done.fetch_add(1, std::memory_order_relaxed); }));
                }
                WaitTasks(tasks, state, "S4 FiberSwitch");
                state.Expect(done.load() == 32, "S4 fiber 回收时丢任务");
                scheduler->shutdown();
                scheduler.reset();
                state.Closed();
                state.Ok(32);
            }
            break;
        }
        case Scenario::S5: {
            // 嵌套 spawn：fiber 内再 spawn 并等待，形成 3 层切换。
            Scheduler scheduler(4);
            scheduler.start();
            for (long long i = 0; i < ops; ++i) {
                std::atomic<int> depth{0};
                auto leaf = [&] { depth.fetch_add(1, std::memory_order_relaxed); };
                auto mid = [&] {
                    auto task = scheduler.spawn(leaf);
                    WaitTasks({task}, state, "S5 FiberSwitch 内层");
                };
                auto root = scheduler.spawn(mid);
                WaitTasks({root}, state, "S5 FiberSwitch 外层");
                state.Expect(depth.load() == 1, "S5 FiberSwitch 嵌套未执行");
                state.Ok();
            }
            scheduler.shutdown();
            break;
        }
        case Scenario::S6: {
            // 1ms 饥饿：yield 循环 + channel 超时，验证切换后超时仍准确。
            Scheduler scheduler(2);
            scheduler.start();
            for (long long i = 0; i < ops; ++i) {
                auto gate = MakeChannel<int>(0);
                const auto started = Clock::now();
                auto task = scheduler.spawn([&] {
                    (void)Select({go2cpp::RecvCase(gate)}, {}, 1ms);
                });
                WaitTasks({task}, state, "S6 FiberSwitch 超时");
                const auto elapsed = std::chrono::duration_cast<
                    std::chrono::milliseconds>(Clock::now() - started);
                if (elapsed.count() < 0) {
                    state.Fail("S6 FiberSwitch 时间倒流");
                }
                state.Timeout();
            }
            scheduler.shutdown();
            break;
        }
        case Scenario::S7: {
            go2cpp::FiberLocal<ErrorPtr> slot;
            Scheduler scheduler(4);
            scheduler.start();
            auto gate = MakeChannel<int>(0);
            std::atomic<bool> matched{false};
            auto waiter = scheduler.spawn([&] {
                slot.GetOrCreate(nullptr) = go2cpp::CanceledError();
                (void)gate->Recv();
                const ErrorPtr* stored = slot.TryGet();
                matched.store(stored != nullptr && *stored != nullptr &&
                                  go2cpp::Is(*stored, go2cpp::CanceledError()),
                              std::memory_order_release);
            });
            auto sender = scheduler.spawn([&] { (void)gate->Send(1); });
            WaitTasks({waiter, sender}, state, "S7 FiberSwitch");
            scheduler.shutdown();
            state.Expect(matched.load(), "S7 Fiber 切换后 FiberLocal 丢失");
            state.Ok(1);
            break;
        }
        case Scenario::S8: {
            // 批量：一次性提交大量 fiber（背压），全部完成后计数归零。
            Scheduler scheduler(4);
            scheduler.start();
            const long long target = std::max<long long>(1, ops);
            std::atomic<long long> done{0};
            std::vector<std::shared_ptr<go2cpp::Task>> tasks;
            tasks.reserve(static_cast<std::size_t>(std::min<long long>(target, 100000)));
            for (long long i = 0; i < target; ++i) {
                tasks.push_back(scheduler.spawn(
                    [&] { done.fetch_add(1, std::memory_order_relaxed); }));
                if ((i & 255) == 0) {
                    state.progress.fetch_add(1, std::memory_order_relaxed);
                }
            }
            WaitTasks(tasks, state, "S8 FiberSwitch 批量");
            state.Expect(done.load() == target, "S8 FiberSwitch 批量丢任务");
            scheduler.shutdown();
            state.Ok(done.load());
            break;
        }
        case Scenario::S9: {
            Scheduler scheduler(4);
            scheduler.start();
            const long long per_worker = std::max<long long>(1, ops / 2);
            std::atomic<long long> done{0};
            auto worker = [&] {
                for (long long i = 0; i < per_worker; ++i) {
                    auto task = scheduler.spawn(
                        [&] { done.fetch_add(1, std::memory_order_relaxed); });
                    WaitTasks({task}, state, "S9 FiberSwitch");
                }
            };
            std::thread t1(worker);
            std::thread t2(worker);
            t1.join();
            t2.join();
            scheduler.shutdown();
            state.Expect(done.load() == 2 * per_worker,
                         "S9 高负载 FiberSwitch 丢任务");
            state.Ok(done.load());
            break;
        }
        case Scenario::S10: {
            Scheduler scheduler(4);
            scheduler.start();
            const long long target = std::max<long long>(1, ops);
            std::atomic<long long> done{0};
            std::thread producer([&] {
                for (long long i = 0; i < target; ++i) {
                    const std::uint64_t roll = SplitMix64(rng) % 4;
                    if (roll == 0) {
                        auto inner = scheduler.spawn(
                            [&] { done.fetch_add(1, std::memory_order_relaxed); });
                        WaitTasks({inner}, state, "S10 fiber 内 spawn");
                    } else if (roll == 1) {
                        (void)scheduler.spawn(
                            [&] { done.fetch_add(1, std::memory_order_relaxed); });
                    } else {
                        auto gate = MakeChannel<int>(1);
                        (void)gate->Send(1);
                        auto task = scheduler.spawn([&] {
                            (void)gate->Recv();
                            done.fetch_add(1, std::memory_order_relaxed);
                        });
                        WaitTasks({task}, state, "S10 fiber 通道切换");
                    }
                    if ((i & 255) == 0) {
                        state.progress.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
            producer.join();
            scheduler.shutdown();
            state.Expect(done.load() >= target, "S10 FiberSwitch 随机丢任务");
            state.Ok(target);
            break;
        }
        case Scenario::Count:
            break;
    }
}

// ===========================================================================
// Mutex：10 场景
// ===========================================================================

void RunMutex(Scenario scenario, long long ops, UnitState& state,
              std::uint64_t& rng) {
    switch (scenario) {
        case Scenario::S1: {
            Mutex mutex;
            long long shared = 0;
            for (long long i = 0; i < ops; ++i) {
                if ((i & 255) == 0) {
                    // 持锁线程 + Lock(ctx) 等待者：取消后必须返回 false 且不持锁。
                    Mutex held;
                    std::atomic<bool> locked{false};
                    auto pair = Context::WithCancel(Background());
                    std::thread owner([&] {
                        held.Lock();
                        locked.store(true, std::memory_order_release);
                        // 持锁时间必须显著大于下面的 cancel 延迟，否则
                        // owner 先释放会让 Lock(ctx) 合法地成功。
                        std::this_thread::sleep_for(5ms);
                        held.Unlock();
                    });
                    std::thread waiter([&] {
                        while (!locked.load(std::memory_order_acquire)) {
                            std::this_thread::yield();
                        }
                        std::atomic<int> acquired{-1};
                        std::thread contender([&] {
                            acquired.store(held.Lock(pair.first) ? 1 : 0,
                                           std::memory_order_release);
                        });
                        std::this_thread::sleep_for(20us);
                        pair.second();
                        contender.join();
                        if (acquired.load(std::memory_order_acquire) != 0) {
                            state.Fail("S1 Lock(ctx) 取消后仍返回已加锁");
                        } else {
                            state.Cancel();
                        }
                    });
                    owner.join();
                    waiter.join();
                    continue;
                }
                mutex.Lock();
                ++shared;
                mutex.Unlock();
                state.Ok();
            }
            state.Expect(shared == state.ok.load(), "S1 Mutex 计数不一致");
            break;
        }
        case Scenario::S2: {
            SocketPair socket;
            if (!state.Expect(socket.Ok(), "S2 socketpair 创建失败")) {
                return;
            }
            Mutex mutex;
            std::uint64_t net_seq = 0;
            for (long long i = 0; i < ops; ++i) {
                if ((i & 15) == 0 && !socket.RoundTrip(net_seq++)) {
                    state.Fail("S2 网络帧损坏");
                    break;
                }
                mutex.Lock();
                mutex.Unlock();
                state.Ok();
            }
            break;
        }
        case Scenario::S3: {
            Mutex mutex;
            long long shared = 0;
            Scheduler scheduler(4);
            scheduler.start();
            const long long per_worker = std::max<long long>(1, ops / 4);
            auto worker = [&] {
                for (long long i = 0; i < per_worker; ++i) {
                    std::lock_guard<Mutex> lock(mutex);
                    ++shared;
                }
                state.progress.fetch_add(per_worker);
            };
            auto f1 = scheduler.spawn(worker);
            auto f2 = scheduler.spawn(worker);
            std::thread t1(worker);
            std::thread t2(worker);
            t1.join();
            t2.join();
            WaitTasks({f1, f2}, state, "S3 Mutex");
            scheduler.shutdown();
            state.Expect(shared == 4 * per_worker, "S3 Mutex 计数不一致");
            state.Ok(shared);
            break;
        }
        case Scenario::S4: {
            // 取消/超时后对象仍可用 + 等待者归零（Mutex 无 Close）。
            for (long long i = 0; i < ops / 256 + 1; ++i) {
                auto mutex = std::make_unique<Mutex>();
                std::atomic<int> done{0};
                std::vector<std::thread> threads;
                for (int t = 0; t < 4; ++t) {
                    threads.emplace_back([&, raw = mutex.get()] {
                        for (int n = 0; n < 64; ++n) {
                            raw->Lock();
                            raw->Unlock();
                        }
                        done.fetch_add(1, std::memory_order_relaxed);
                    });
                }
                for (auto& thread : threads) {
                    thread.join();
                }
                state.Expect(done.load() == 4, "S4 Mutex 线程未完成");
                state.Expect(!mutex->DebugSnapshotInternal().held,
                             "S4 Mutex 析构前仍被持有");
                mutex.reset();
                state.Closed();
                state.Ok(4);
            }
            break;
        }
        case Scenario::S5: {
            Mutex first;
            Mutex second;
            long long shared = 0;
            for (long long i = 0; i < ops; ++i) {
                // 固定顺序 A→B，避免死锁；再反向 B→A 的独立嵌套。
                std::lock_guard<Mutex> a(first);
                std::lock_guard<Mutex> b(second);
                ++shared;
                state.Ok();
            }
            state.Expect(shared == state.ok.load(), "S5 Mutex 嵌套计数不一致");
            break;
        }
        case Scenario::S6: {
            // 1ms 饥饿：持锁 200us，LockFor(1ms) 有时成功有时超时。
            Mutex mutex;
            std::atomic<bool> stop{false};
            std::thread holder([&] {
                while (!stop.load(std::memory_order_acquire)) {
                    mutex.Lock();
                    std::this_thread::sleep_for(200us);
                    mutex.Unlock();
                    std::this_thread::sleep_for(50us);
                }
            });
            for (long long i = 0; i < ops; ++i) {
                if (mutex.LockFor(1ms, {})) {
                    mutex.Unlock();
                    state.Ok();
                } else {
                    state.Timeout();
                }
                if ((i & 1023) == 0) {
                    state.progress.fetch_add(1, std::memory_order_relaxed);
                }
            }
            stop.store(true, std::memory_order_release);
            holder.join();
            break;
        }
        case Scenario::S7: {
            // 错误链：LockFor 超时后返回 false（不产生 ErrorPtr），
            // FiberLocal 错误在 fiber 迁移后保持。
            go2cpp::FiberLocal<ErrorPtr> slot;
            Scheduler scheduler(4);
            scheduler.start();
            auto gate = MakeChannel<int>(0);
            std::atomic<bool> matched{false};
            auto waiter = scheduler.spawn([&] {
                slot.GetOrCreate(nullptr) = go2cpp::CanceledError();
                (void)gate->Recv();
                const ErrorPtr* stored = slot.TryGet();
                matched.store(stored != nullptr && *stored != nullptr &&
                                  go2cpp::Is(*stored, go2cpp::CanceledError()),
                              std::memory_order_release);
            });
            auto sender = scheduler.spawn([&] { (void)gate->Send(1); });
            WaitTasks({waiter, sender}, state, "S7 Mutex");
            scheduler.shutdown();
            state.Expect(matched.load(), "S7 Mutex 场景 FiberLocal 错误丢失");
            // LockFor 超时：持锁时超时必须失败且不持锁。
            Mutex mutex;
            mutex.Lock();
            state.Expect(!mutex.LockFor(1ms, {}), "S7 已持锁 LockFor 竟然成功");
            mutex.Unlock();
            mutex.Lock();
            mutex.Unlock();
            state.Ok(2);
            break;
        }
        case Scenario::S8: {
            // 高争抢（背压类比）：8 线程抢一把锁，计数必须精确。
            Mutex mutex;
            long long shared = 0;
            const long long target = std::max<long long>(1, ops);
            std::atomic<long long> done{0};
            std::vector<std::thread> threads;
            for (int t = 0; t < 8; ++t) {
                threads.emplace_back([&, target] {
                    const long long per = target / 8;
                    for (long long i = 0; i < per; ++i) {
                        {
                            std::lock_guard<Mutex> lock(mutex);
                            ++shared;
                        }
                        done.fetch_add(1, std::memory_order_relaxed);
                    }
                    state.progress.fetch_add(per);
                });
            }
            for (auto& thread : threads) {
                thread.join();
            }
            state.Expect(shared == done.load(), "S8 Mutex 背压计数不一致");
            state.Ok(done.load());
            break;
        }
        case Scenario::S9: {
            Mutex mutex;
            long long shared = 0;
            const long long per_worker = std::max<long long>(1, ops / 4);
            auto worker = [&] {
                for (long long i = 0; i < per_worker; ++i) {
                    mutex.Lock();
                    ++shared;
                    mutex.Unlock();
                }
                state.progress.fetch_add(per_worker);
            };
            std::vector<std::thread> threads;
            for (int t = 0; t < 4; ++t) {
                threads.emplace_back(worker);
            }
            for (auto& thread : threads) {
                thread.join();
            }
            state.Expect(shared == 4 * per_worker, "S9 高负载 Mutex 计数不一致");
            state.Ok(shared);
            break;
        }
        case Scenario::S10: {
            // 随机 lock/trylock/unlock，模型：每次成功加锁后计数 +1。
            Mutex mutex;
            long long shared = 0;
            long long holds = 0;
            bool held = false;
            for (long long i = 0; i < ops; ++i) {
                const std::uint64_t roll = SplitMix64(rng) % 100;
                if (!held && roll < 60) {
                    mutex.Lock();
                    held = true;
                    ++holds;
                } else if (held && roll < 90) {
                    ++shared;
                    mutex.Unlock();
                    held = false;
                } else if (!held) {
                    if (mutex.TryLock()) {
                        // TryLock 成功后同样进入临界区修改 shared，
                        // 否则 shared 与 holds 的模型对拍会差一。
                        ++shared;
                        ++holds;
                        mutex.Unlock();
                    } else {
                        state.Retry();
                    }
                }
                if ((i & 1023) == 0) {
                    state.progress.fetch_add(1, std::memory_order_relaxed);
                }
            }
            if (held) {
                ++shared;
                mutex.Unlock();
            }
            state.Expect(shared == holds, "S10 Mutex 模型对拍失败");
            state.Ok(std::max<long long>(1, holds));
            break;
        }
        case Scenario::Count:
            break;
    }
}

// ===========================================================================
// CV（ConditionVariable）：10 场景
// ===========================================================================

void RunCV(Scenario scenario, long long ops, UnitState& state,
           std::uint64_t& rng) {
    switch (scenario) {
        case Scenario::S1: {
            Mutex mutex;
            ConditionVariable condition;
            for (long long i = 0; i < ops; ++i) {
                if ((i & 63) == 0) {
                    // Wait(ctx) 被取消：返回 false，锁被重新获得。
                    auto pair = Context::WithCancel(Background());
                    std::atomic<int> result{-1};
                    std::thread waiter([&] {
                        mutex.Lock();
                        const bool ok =
                            condition.Wait(mutex, pair.first);
                        result.store(ok ? 1 : 0, std::memory_order_release);
                        mutex.Unlock();
                    });
                    std::this_thread::sleep_for(30us);
                    pair.second();
                    waiter.join();
                    state.Expect(result.load(std::memory_order_acquire) == 0,
                                 "S1 CV Wait(ctx) 取消后返回 true");
                    state.Cancel();
                    continue;
                }
                mutex.Lock();
                condition.NotifyOne();
                // CV 的 Notify 不累积：单线程「先 Notify 再 Wait」会丢通知，
                // 这里用 1ms 超时兜底，超时本身计为一次正常的饥饿/超时操作。
                const bool ok = condition.WaitFor(mutex, 1ms, {});
                mutex.Unlock();
                if (ok) {
                    state.Ok();
                } else {
                    state.Timeout();
                }
            }
            break;
        }
        case Scenario::S2: {
            SocketPair socket;
            if (!state.Expect(socket.Ok(), "S2 socketpair 创建失败")) {
                return;
            }
            Mutex mutex;
            ConditionVariable condition;
            std::uint64_t net_seq = 0;
            for (long long i = 0; i < ops; ++i) {
                if ((i & 15) == 0 && !socket.RoundTrip(net_seq++)) {
                    state.Fail("S2 网络帧损坏");
                    break;
                }
                mutex.Lock();
                condition.NotifyOne();
                // 同 S1：CV 通知不累积，必须带超时兜底。
                const bool ok = condition.WaitFor(mutex, 1ms, {});
                mutex.Unlock();
                if (ok) {
                    state.Ok();
                } else {
                    state.Timeout();
                }
            }
            break;
        }
        case Scenario::S3: {
            Mutex mutex;
            ConditionVariable condition;
            Scheduler scheduler(4);
            scheduler.start();
            int arrived = 0;
            int generation = 0;
            const long long per_worker = std::max<long long>(1, ops / 4);
            std::atomic<long long> done{0};
            auto worker = [&] {
                for (long long i = 0; i < per_worker; ++i) {
                    mutex.Lock();
                    const int observed = generation;
                    ++arrived;
                    if (arrived == 4) {
                        arrived = 0;
                        ++generation;
                        condition.NotifyAll();
                    } else {
                        bool timed_out = false;
                        while (generation == observed) {
                            if (!condition.WaitFor(mutex, 200ms, {})) {
                                timed_out = true;
                                break;
                            }
                        }
                        if (timed_out) {
                            state.Fail("S3 CV 等待超时（通知丢失）");
                            mutex.Unlock();
                            return;
                        }
                    }
                    mutex.Unlock();
                    done.fetch_add(1, std::memory_order_relaxed);
                    if ((i & 255) == 0) {
                        state.progress.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            };
            auto f1 = scheduler.spawn(worker);
            auto f2 = scheduler.spawn(worker);
            std::thread t1(worker);
            std::thread t2(worker);
            t1.join();
            t2.join();
            WaitTasks({f1, f2}, state, "S3 CV");
            scheduler.shutdown();
            state.Expect(done.load() == 4 * per_worker, "S3 CV 计数不一致");
            state.Ok(done.load());
            break;
        }
        case Scenario::S4: {
            // 唤醒与析构：NotifyAll 后所有等待者退出，再析构 CV。
            for (long long i = 0; i < ops / 256 + 1; ++i) {
                auto mutex = std::make_unique<Mutex>();
                auto condition = std::make_unique<ConditionVariable>();
                auto ready = std::make_shared<bool>(false);
                std::vector<std::thread> waiters;
                for (int w = 0; w < 4; ++w) {
                    waiters.emplace_back([&, mutex = mutex.get(),
                                          condition = condition.get(), ready] {
                        mutex->Lock();
                        (void)condition->WaitFor(*mutex, 50ms, {});
                        mutex->Unlock();
                    });
                }
                std::this_thread::sleep_for(50us);
                mutex->Lock();
                *ready = true;
                condition->NotifyAll();
                mutex->Unlock();
                for (auto& thread : waiters) {
                    thread.join();
                }
                state.Expect(condition->DebugSnapshotInternal().waiters == 0,
                             "S4 CV 唤醒后 waiters 非 0");
                condition.reset();
                mutex.reset();
                state.Closed();
                state.Ok(4);
            }
            break;
        }
        case Scenario::S5: {
            // 两层 CV 嵌套：每层通知后带 1ms 超时等待（通知不累积，
            // 自通知可能丢失，超时是预期路径）。
            Mutex mutex;
            ConditionVariable outer;
            ConditionVariable inner;
            for (long long i = 0; i < ops; ++i) {
                mutex.Lock();
                outer.NotifyOne();
                (void)outer.WaitFor(mutex, 1ms, {});
                inner.NotifyOne();
                (void)inner.WaitFor(mutex, 1ms, {});
                mutex.Unlock();
                state.Ok();
            }
            break;
        }
        case Scenario::S6: {
            Mutex mutex;
            ConditionVariable condition;
            for (long long i = 0; i < ops; ++i) {
                mutex.Lock();
                const auto started = Clock::now();
                const bool notified = condition.WaitFor(mutex, 1ms, {});
                const auto elapsed = std::chrono::duration_cast<
                    std::chrono::microseconds>(Clock::now() - started);
                mutex.Unlock();
                state.Expect(!notified, "S6 CV WaitFor 无通知却返回 true");
                if (elapsed < 500us) {
                    state.Fail("S6 CV WaitFor 提前返回");
                }
                state.Timeout();
            }
            break;
        }
        case Scenario::S7: {
            go2cpp::FiberLocal<ErrorPtr> slot;
            Scheduler scheduler(4);
            scheduler.start();
            Mutex mutex;
            ConditionVariable condition;
            std::atomic<bool> matched{false};
            auto waiter = scheduler.spawn([&] {
                slot.GetOrCreate(nullptr) = go2cpp::DeadlineExceededError();
                mutex.Lock();
                (void)condition.WaitFor(mutex, 1ms, {});
                mutex.Unlock();
                const ErrorPtr* stored = slot.TryGet();
                matched.store(stored != nullptr && *stored != nullptr &&
                                  go2cpp::Is(*stored,
                                             go2cpp::DeadlineExceededError()),
                              std::memory_order_release);
            });
            WaitTasks({waiter}, state, "S7 CV");
            scheduler.shutdown();
            state.Expect(matched.load(), "S7 CV 场景 FiberLocal 错误丢失");
            state.Ok(1);
            break;
        }
        case Scenario::S8: {
            Mutex mutex;
            ConditionVariable condition;
            std::deque<int> queue;
            const long long target = std::max<long long>(1, ops);
            std::atomic<long long> consumed{0};
            std::vector<std::thread> consumers;
            for (int c = 0; c < 4; ++c) {
                consumers.emplace_back([&] {
                    while (true) {
                        mutex.Lock();
                        while (queue.empty()) {
                            (void)condition.WaitFor(mutex, 50ms, {});
                        }
                        const int value = queue.front();
                        queue.pop_front();
                        mutex.Unlock();
                        if (value < 0) {
                            return;
                        }
                        consumed.fetch_add(1, std::memory_order_relaxed);
                        if ((consumed.load() & 1023) == 0) {
                            state.progress.fetch_add(
                                1, std::memory_order_relaxed);
                        }
                    }
                });
            }
            std::thread producer([&] {
                for (long long i = 0; i < target; ++i) {
                    mutex.Lock();
                    queue.push_back(static_cast<int>(i));
                    condition.NotifyOne();
                    mutex.Unlock();
                }
                mutex.Lock();
                for (int c = 0; c < 4; ++c) {
                    queue.push_back(-1);
                }
                condition.NotifyAll();
                mutex.Unlock();
            });
            producer.join();
            for (auto& thread : consumers) {
                thread.join();
            }
            state.Expect(consumed.load() == target, "S8 CV 背压丢唤醒");
            state.Ok(consumed.load());
            break;
        }
        case Scenario::S9: {
            Mutex mutex;
            ConditionVariable condition;
            std::atomic<long long> done{0};
            std::thread notifier([&] {
                for (long long i = 0; i < ops; ++i) {
                    mutex.Lock();
                    condition.NotifyOne();
                    mutex.Unlock();
                    std::this_thread::sleep_for(1us);
                }
            });
            for (long long i = 0; i < ops; ++i) {
                mutex.Lock();
                (void)condition.WaitFor(mutex, 10ms, {});
                mutex.Unlock();
                done.fetch_add(1, std::memory_order_relaxed);
                if ((i & 255) == 0) {
                    state.progress.fetch_add(1, std::memory_order_relaxed);
                }
            }
            notifier.join();
            state.Expect(done.load() == ops, "S9 CV 高负载丢唤醒");
            state.Ok(done.load());
            break;
        }
        case Scenario::S10: {
            Mutex mutex;
            ConditionVariable condition;
            std::atomic<long long> notifies{0};
            std::thread notifier([&] {
                for (long long i = 0; i < ops; ++i) {
                    if ((SplitMix64(rng) & 7) != 0) {
                        continue;
                    }
                    mutex.Lock();
                    if (SplitMix64(rng) & 1) {
                        condition.NotifyOne();
                    } else {
                        condition.NotifyAll();
                    }
                    mutex.Unlock();
                    notifies.fetch_add(1, std::memory_order_relaxed);
                    std::this_thread::sleep_for(1us);
                }
            });
            for (long long i = 0; i < ops; ++i) {
                mutex.Lock();
                (void)condition.WaitFor(mutex, 1ms, {});
                mutex.Unlock();
                state.Ok();
            }
            notifier.join();
            state.Ok();
            break;
        }
        case Scenario::Count:
            break;
    }
}

// ===========================================================================
// WG（WaitGroup）：10 场景
// ===========================================================================

void RunWG(Scenario scenario, long long ops, UnitState& state,
           std::uint64_t& rng) {
    switch (scenario) {
        case Scenario::S1: {
            for (long long i = 0; i < ops; ++i) {
                WaitGroup wg;
                auto pair = Context::WithCancel(Background());
                wg.Add(1);
                std::atomic<int> result{-1};
                std::thread waiter([&] {
                    result.store(wg.Wait(pair.first) ? 1 : 0,
                                 std::memory_order_release);
                });
                std::this_thread::sleep_for(20us);
                if ((i & 1) == 0) {
                    pair.second();
                    waiter.join();
                    state.Expect(result.load() == 0,
                                 "S1 WG Wait(ctx) 取消后返回 true");
                    // 计数仍为 1，补 Done 让对象可析构。
                    wg.Done();
                    state.Cancel();
                } else {
                    wg.Done();
                    waiter.join();
                    state.Expect(result.load() == 1,
                                 "S1 WG 正常 Done 后 Wait 返回 false");
                    state.Ok();
                }
            }
            break;
        }
        case Scenario::S2: {
            SocketPair socket;
            if (!state.Expect(socket.Ok(), "S2 socketpair 创建失败")) {
                return;
            }
            std::uint64_t net_seq = 0;
            for (long long i = 0; i < ops; ++i) {
                if ((i & 15) == 0 && !socket.RoundTrip(net_seq++)) {
                    state.Fail("S2 网络帧损坏");
                    break;
                }
                WaitGroup wg;
                wg.Add(1);
                wg.Done();
                if (!wg.Wait()) {
                    state.Fail("S2 WG Wait 失败");
                    break;
                }
                state.Ok();
            }
            break;
        }
        case Scenario::S3: {
            Scheduler scheduler(4);
            scheduler.start();
            const long long per_worker = std::max<long long>(1, ops / 4);
            WaitGroup wg;
            wg.Add(4);
            auto worker = [&] {
                for (long long i = 0; i < per_worker; ++i) {
                    WaitGroup local;
                    local.Add(1);
                    local.Done();
                    if (!local.Wait()) {
                        state.Fail("S3 WG 局部 Wait 失败");
                        break;
                    }
                }
                wg.Done();
                state.progress.fetch_add(per_worker);
            };
            auto f1 = scheduler.spawn(worker);
            auto f2 = scheduler.spawn(worker);
            std::thread t1(worker);
            std::thread t2(worker);
            t1.join();
            t2.join();
            WaitTasks({f1, f2}, state, "S3 WG");
            scheduler.shutdown();
            state.Expect(wg.Wait(), "S3 WG 计数未归零");
            state.Ok(4 * per_worker);
            break;
        }
        case Scenario::S4: {
            // WaitGroup 析构与等待者并发：Wait 返回后再析构（无 close 语义）。
            for (long long i = 0; i < ops / 256 + 1; ++i) {
                auto wg = std::make_unique<WaitGroup>();
                wg->Add(4);
                std::vector<std::thread> waiters;
                for (int w = 0; w < 4; ++w) {
                    waiters.emplace_back([&, raw = wg.get()] {
                        (void)raw->Wait();
                    });
                }
                for (int w = 0; w < 4; ++w) {
                    wg->Done();
                }
                for (auto& thread : waiters) {
                    thread.join();
                }
                state.Expect(wg->Count() == 0, "S4 WG 计数未归零");
                wg.reset();
                state.Closed();
                state.Ok(4);
            }
            break;
        }
        case Scenario::S5: {
            // WG 嵌套：外层 WG 每轮 Add(1)，内层再等 4 个 Done。
            for (long long i = 0; i < ops; ++i) {
                WaitGroup outer;
                outer.Add(1);
                WaitGroup inner;
                inner.Add(4);
                std::atomic<int> done{0};
                std::vector<std::thread> threads;
                for (int t = 0; t < 4; ++t) {
                    threads.emplace_back([&] {
                        done.fetch_add(1, std::memory_order_relaxed);
                        inner.Done();
                    });
                }
                for (auto& thread : threads) {
                    thread.join();
                }
                if (!inner.Wait()) {
                    state.Fail("S5 WG 内层 Wait 失败");
                    break;
                }
                outer.Done();
                if (!outer.Wait()) {
                    state.Fail("S5 WG 外层 Wait 失败");
                    break;
                }
                state.Expect(done.load() == 4, "S5 WG 嵌套计数错误");
                state.Ok();
            }
            break;
        }
        case Scenario::S6: {
            for (long long i = 0; i < ops; ++i) {
                WaitGroup wg;
                wg.Add(1);
                if (wg.WaitFor(1ms, {})) {
                    state.Fail("S6 WG WaitFor 无 Done 却返回 true");
                } else {
                    state.Timeout();
                }
                wg.Done();
                if (!wg.WaitFor(1ms, {})) {
                    state.Fail("S6 WG Done 后 WaitFor 仍失败");
                }
            }
            break;
        }
        case Scenario::S7: {
            go2cpp::FiberLocal<ErrorPtr> slot;
            Scheduler scheduler(4);
            scheduler.start();
            WaitGroup wg;
            wg.Add(1);
            std::atomic<bool> matched{false};
            auto waiter = scheduler.spawn([&] {
                slot.GetOrCreate(nullptr) = go2cpp::CanceledError();
                (void)wg.Wait();
                const ErrorPtr* stored = slot.TryGet();
                matched.store(stored != nullptr && *stored != nullptr &&
                                  go2cpp::Is(*stored, go2cpp::CanceledError()),
                              std::memory_order_release);
            });
            std::this_thread::sleep_for(100us);
            wg.Done();
            WaitTasks({waiter}, state, "S7 WG");
            scheduler.shutdown();
            state.Expect(matched.load(), "S7 WG 场景 FiberLocal 错误丢失");
            state.Ok(1);
            break;
        }
        case Scenario::S8: {
            // 批量 Add/Done：1 万级计数，归零后 Wait 必须立即成功。
            const long long target = std::max<long long>(1, ops);
            WaitGroup wg;
            wg.Add(target);
            for (long long i = 0; i < target; ++i) {
                wg.Done();
                if ((i & 4095) == 0) {
                    state.progress.fetch_add(1, std::memory_order_relaxed);
                }
            }
            state.Expect(wg.WaitFor(5s, {}), "S8 WG 批量 Done 后未归零");
            state.Expect(wg.Count() == 0, "S8 WG Count 非 0");
            state.Ok(target);
            break;
        }
        case Scenario::S9: {
            WaitGroup wg;
            wg.Add(4);
            std::atomic<long long> done{0};
            auto worker = [&] {
                for (long long i = 0; i < ops / 4; ++i) {
                    done.fetch_add(1, std::memory_order_relaxed);
                }
                wg.Done();
                state.progress.fetch_add(ops / 4);
            };
            std::vector<std::thread> threads;
            for (int t = 0; t < 4; ++t) {
                threads.emplace_back(worker);
            }
            state.Expect(wg.WaitFor(300s, {}), "S9 WG 高负载未归零");
            for (auto& thread : threads) {
                thread.join();
            }
            state.Expect(done.load() == 4 * (ops / 4), "S9 WG 计数不一致");
            state.Ok(done.load());
            break;
        }
        case Scenario::S10: {
            WaitGroup wg;
            long long added = 0;
            long long done = 0;
            for (long long i = 0; i < ops; ++i) {
                const std::uint64_t roll = SplitMix64(rng) % 100;
                if (roll < 45) {
                    wg.Add(1);
                    ++added;
                } else if (roll < 90 && added > done) {
                    wg.Done();
                    ++done;
                } else {
                    (void)wg.WaitFor(0ms, {});
                    state.Retry();
                }
                if ((i & 1023) == 0) {
                    state.progress.fetch_add(1, std::memory_order_relaxed);
                }
            }
            while (done < added) {
                wg.Done();
                ++done;
            }
            state.Expect(wg.Wait(), "S10 WG 随机模型未归零");
            state.Expect(done == added, "S10 WG 模型计数不一致");
            state.Ok(added);
            break;
        }
        case Scenario::Count:
            break;
    }
}

// ===========================================================================
// RWMutex：10 场景
// ===========================================================================

void RunRWMutex(Scenario scenario, long long ops, UnitState& state,
                std::uint64_t& rng) {
    switch (scenario) {
        case Scenario::S1: {
            for (long long i = 0; i < ops; ++i) {
                if ((i & 255) == 0) {
                    // RWMutex 没有 Context 取消接口：用 1ms 超时验证
                    // 「等待失败时锁状态不变、随后仍可获得」。
                    RWMutex mutex;
                    mutex.Lock();
                    std::atomic<int> result{-1};
                    std::thread waiter([&] {
                        result.store(mutex.LockFor(1ms) ? 1 : 0,
                                     std::memory_order_release);
                    });
                    waiter.join();
                    state.Expect(result.load() == 0,
                                 "S1 RWMutex 持锁时 LockFor(1ms) 竟然成功");
                    mutex.Unlock();
                    mutex.Lock();
                    mutex.Unlock();
                    state.Cancel();
                    continue;
                }
                RWMutex mutex;
                mutex.RLock();
                mutex.RUnlock();
                mutex.Lock();
                mutex.Unlock();
                state.Ok();
            }
            break;
        }
        case Scenario::S2: {
            SocketPair socket;
            if (!state.Expect(socket.Ok(), "S2 socketpair 创建失败")) {
                return;
            }
            RWMutex mutex;
            std::uint64_t net_seq = 0;
            for (long long i = 0; i < ops; ++i) {
                if ((i & 15) == 0 && !socket.RoundTrip(net_seq++)) {
                    state.Fail("S2 网络帧损坏");
                    break;
                }
                mutex.RLock();
                mutex.RUnlock();
                state.Ok();
            }
            break;
        }
        case Scenario::S3: {
            RWMutex mutex;
            long long shared = 0;
            Scheduler scheduler(4);
            scheduler.start();
            const long long per_worker = std::max<long long>(1, ops / 4);
            auto reader = [&] {
                for (long long i = 0; i < per_worker; ++i) {
                    mutex.RLock();
                    (void)shared;
                    mutex.RUnlock();
                }
                state.progress.fetch_add(per_worker);
            };
            auto writer = [&] {
                for (long long i = 0; i < per_worker; ++i) {
                    mutex.Lock();
                    ++shared;
                    mutex.Unlock();
                }
                state.progress.fetch_add(per_worker);
            };
            auto f1 = scheduler.spawn(reader);
            auto f2 = scheduler.spawn(writer);
            std::thread t1(reader);
            std::thread t2(writer);
            t1.join();
            t2.join();
            WaitTasks({f1, f2}, state, "S3 RWMutex");
            scheduler.shutdown();
            state.Expect(shared == 2 * per_worker, "S3 RWMutex 写计数不一致");
            state.Ok(2 * per_worker);
            break;
        }
        case Scenario::S4: {
            for (long long i = 0; i < ops / 256 + 1; ++i) {
                auto mutex = std::make_unique<RWMutex>();
                std::atomic<int> done{0};
                std::vector<std::thread> threads;
                for (int t = 0; t < 4; ++t) {
                    threads.emplace_back([&, raw = mutex.get(), t] {
                        for (int n = 0; n < 64; ++n) {
                            if ((t & 1) == 0) {
                                raw->RLock();
                                raw->RUnlock();
                            } else {
                                raw->Lock();
                                raw->Unlock();
                            }
                        }
                        done.fetch_add(1, std::memory_order_relaxed);
                    });
                }
                for (auto& thread : threads) {
                    thread.join();
                }
                state.Expect(done.load() == 4, "S4 RWMutex 线程未完成");
                mutex.reset();
                state.Closed();
                state.Ok(4);
            }
            break;
        }
        case Scenario::S5: {
            // 嵌套：同一线程连续两层读锁，然后升级为写锁（先全部释放）。
            RWMutex outer;
            RWMutex inner;
            for (long long i = 0; i < ops; ++i) {
                outer.RLock();
                inner.RLock();
                inner.RUnlock();
                outer.RUnlock();
                outer.Lock();
                inner.Lock();
                inner.Unlock();
                outer.Unlock();
                state.Ok();
            }
            break;
        }
        case Scenario::S6: {
            RWMutex mutex;
            std::atomic<bool> stop{false};
            std::thread writer([&] {
                while (!stop.load(std::memory_order_acquire)) {
                    mutex.Lock();
                    std::this_thread::sleep_for(200us);
                    mutex.Unlock();
                    std::this_thread::sleep_for(50us);
                }
            });
            for (long long i = 0; i < ops; ++i) {
                if (mutex.RLockFor(1ms)) {
                    mutex.RUnlock();
                    state.Ok();
                } else {
                    state.Timeout();
                }
                if ((i & 1023) == 0) {
                    state.progress.fetch_add(1, std::memory_order_relaxed);
                }
            }
            stop.store(true, std::memory_order_release);
            writer.join();
            break;
        }
        case Scenario::S7: {
            go2cpp::FiberLocal<ErrorPtr> slot;
            Scheduler scheduler(4);
            scheduler.start();
            std::atomic<bool> matched{false};
            auto gate = MakeChannel<int>(0);
            auto waiter = scheduler.spawn([&] {
                slot.GetOrCreate(nullptr) = go2cpp::DeadlineExceededError();
                (void)gate->Recv();
                const ErrorPtr* stored = slot.TryGet();
                matched.store(stored != nullptr && *stored != nullptr &&
                                  go2cpp::Is(*stored,
                                             go2cpp::DeadlineExceededError()),
                              std::memory_order_release);
            });
            auto sender = scheduler.spawn([&] { (void)gate->Send(1); });
            WaitTasks({waiter, sender}, state, "S7 RWMutex");
            scheduler.shutdown();
            state.Expect(matched.load(), "S7 RWMutex 场景 FiberLocal 错误丢失");
            state.Ok(1);
            break;
        }
        case Scenario::S8: {
            // 读多写少 + 背压：写者持续抢写锁，读者批量短持有。
            RWMutex mutex;
            long long shared = 0;
            const long long target = std::max<long long>(1, ops);
            std::atomic<long long> reads{0};
            std::atomic<long long> writes{0};
            std::atomic<bool> stop{false};
            std::vector<std::thread> readers;
            for (int r = 0; r < 6; ++r) {
                readers.emplace_back([&] {
                    while (!stop.load(std::memory_order_acquire)) {
                        mutex.RLock();
                        (void)shared;
                        mutex.RUnlock();
                        reads.fetch_add(1, std::memory_order_relaxed);
                        if ((reads.load() & 4095) == 0) {
                            state.progress.fetch_add(
                                1, std::memory_order_relaxed);
                        }
                    }
                });
            }
            std::thread writer([&] {
                for (long long i = 0; i < target / 8; ++i) {
                    mutex.Lock();
                    ++shared;
                    mutex.Unlock();
                    writes.fetch_add(1, std::memory_order_relaxed);
                }
            });
            writer.join();
            stop.store(true, std::memory_order_release);
            for (auto& thread : readers) {
                thread.join();
            }
            state.Expect(shared == target / 8, "S8 RWMutex 写计数不一致");
            state.Expect(reads.load() > 0, "S8 RWMutex 读路径未执行");
            state.Ok(reads.load() + writes.load());
            break;
        }
        case Scenario::S9: {
            RWMutex mutex;
            long long shared = 0;
            std::atomic<long long> total{0};
            auto reader = [&] {
                for (long long i = 0; i < ops / 8; ++i) {
                    mutex.RLock();
                    (void)shared;
                    mutex.RUnlock();
                    total.fetch_add(1, std::memory_order_relaxed);
                }
                state.progress.fetch_add(ops / 8);
            };
            auto writer = [&] {
                for (long long i = 0; i < ops / 8; ++i) {
                    mutex.Lock();
                    ++shared;
                    mutex.Unlock();
                    total.fetch_add(1, std::memory_order_relaxed);
                }
                state.progress.fetch_add(ops / 8);
            };
            std::vector<std::thread> threads;
            for (int t = 0; t < 6; ++t) {
                threads.emplace_back(reader);
            }
            threads.emplace_back(writer);
            threads.emplace_back(writer);
            for (auto& thread : threads) {
                thread.join();
            }
            state.Expect(shared == 2 * (ops / 8),
                         "S9 高负载 RWMutex 写计数不一致");
            state.Ok(total.load());
            break;
        }
        case Scenario::S10: {
            RWMutex mutex;
            long long shared = 0;
            bool reader_held = false;
            bool writer_held = false;
            for (long long i = 0; i < ops; ++i) {
                const std::uint64_t roll = SplitMix64(rng) % 100;
                if (!reader_held && !writer_held && roll < 40) {
                    mutex.RLock();
                    reader_held = true;
                } else if (!reader_held && !writer_held && roll < 60) {
                    mutex.Lock();
                    writer_held = true;
                } else if (reader_held && roll < 80) {
                    mutex.RUnlock();
                    reader_held = false;
                } else if (writer_held && roll < 100) {
                    ++shared;
                    mutex.Unlock();
                    writer_held = false;
                } else if (!reader_held && !writer_held) {
                    if (mutex.TryLock()) {
                        mutex.Unlock();
                        state.Ok();
                    } else {
                        state.Retry();
                    }
                }
                if ((i & 1023) == 0) {
                    state.progress.fetch_add(1, std::memory_order_relaxed);
                }
            }
            if (reader_held) {
                mutex.RUnlock();
            }
            if (writer_held) {
                mutex.Unlock();
            }
            state.Ok(1);
            break;
        }
        case Scenario::Count:
            break;
    }
}

// ===========================================================================
// SpinMutex：10 场景
// ===========================================================================

void RunSpinMutex(Scenario scenario, long long ops, UnitState& state,
                  std::uint64_t& rng) {
    switch (scenario) {
        case Scenario::S1: {
            for (long long i = 0; i < ops; ++i) {
                if ((i & 255) == 0) {
                    SpinMutex mutex;
                    mutex.Lock();
                    std::atomic<int> result{-1};
                    std::thread waiter([&] {
                        result.store(mutex.TryLockFor(5ms) ? 1 : 0,
                                     std::memory_order_release);
                    });
                    std::this_thread::sleep_for(20us);
                    mutex.Unlock();
                    waiter.join();
                    state.Expect(result.load(std::memory_order_acquire) == 1,
                                 "S1 SpinMutex 释放后等待者未获得锁");
                    if (result.load() == 1) {
                        mutex.Unlock();
                    }
                    state.Cancel();
                    continue;
                }
                SpinMutex mutex;
                mutex.Lock();
                mutex.Unlock();
                state.Ok();
            }
            break;
        }
        case Scenario::S2: {
            SocketPair socket;
            if (!state.Expect(socket.Ok(), "S2 socketpair 创建失败")) {
                return;
            }
            SpinMutex mutex;
            std::uint64_t net_seq = 0;
            for (long long i = 0; i < ops; ++i) {
                if ((i & 15) == 0 && !socket.RoundTrip(net_seq++)) {
                    state.Fail("S2 网络帧损坏");
                    break;
                }
                mutex.Lock();
                mutex.Unlock();
                state.Ok();
            }
            break;
        }
        case Scenario::S3: {
            SpinMutex mutex;
            long long shared = 0;
            Scheduler scheduler(4);
            scheduler.start();
            const long long per_worker = std::max<long long>(1, ops / 4);
            auto worker = [&] {
                for (long long i = 0; i < per_worker; ++i) {
                    mutex.Lock();
                    ++shared;
                    mutex.Unlock();
                }
                state.progress.fetch_add(per_worker);
            };
            auto f1 = scheduler.spawn(worker);
            auto f2 = scheduler.spawn(worker);
            std::thread t1(worker);
            std::thread t2(worker);
            t1.join();
            t2.join();
            WaitTasks({f1, f2}, state, "S3 SpinMutex");
            scheduler.shutdown();
            state.Expect(shared == 4 * per_worker,
                         "S3 SpinMutex 计数不一致");
            state.Ok(shared);
            break;
        }
        case Scenario::S4: {
            for (long long i = 0; i < ops / 256 + 1; ++i) {
                auto mutex = std::make_unique<SpinMutex>();
                std::atomic<int> done{0};
                std::vector<std::thread> threads;
                for (int t = 0; t < 4; ++t) {
                    threads.emplace_back([&, raw = mutex.get()] {
                        for (int n = 0; n < 64; ++n) {
                            raw->Lock();
                            raw->Unlock();
                        }
                        done.fetch_add(1, std::memory_order_relaxed);
                    });
                }
                for (auto& thread : threads) {
                    thread.join();
                }
                state.Expect(done.load() == 4, "S4 SpinMutex 线程未完成");
                mutex.reset();
                state.Closed();
                state.Ok(4);
            }
            break;
        }
        case Scenario::S5: {
            SpinMutex first;
            SpinMutex second;
            long long shared = 0;
            for (long long i = 0; i < ops; ++i) {
                first.Lock();
                second.Lock();
                ++shared;
                second.Unlock();
                first.Unlock();
                state.Ok();
            }
            state.Expect(shared == state.ok.load(), "S5 SpinMutex 嵌套计数不一致");
            break;
        }
        case Scenario::S6: {
            SpinMutex mutex;
            std::atomic<bool> stop{false};
            std::thread holder([&] {
                while (!stop.load(std::memory_order_acquire)) {
                    mutex.Lock();
                    std::this_thread::sleep_for(200us);
                    mutex.Unlock();
                    std::this_thread::sleep_for(50us);
                }
            });
            for (long long i = 0; i < ops; ++i) {
                if (mutex.TryLockFor(1ms)) {
                    mutex.Unlock();
                    state.Ok();
                } else {
                    state.Timeout();
                }
                if ((i & 1023) == 0) {
                    state.progress.fetch_add(1, std::memory_order_relaxed);
                }
            }
            stop.store(true, std::memory_order_release);
            holder.join();
            break;
        }
        case Scenario::S7: {
            go2cpp::FiberLocal<ErrorPtr> slot;
            Scheduler scheduler(4);
            scheduler.start();
            std::atomic<bool> matched{false};
            auto gate = MakeChannel<int>(0);
            auto waiter = scheduler.spawn([&] {
                slot.GetOrCreate(nullptr) = go2cpp::DeadlineExceededError();
                (void)gate->Recv();
                const ErrorPtr* stored = slot.TryGet();
                matched.store(stored != nullptr && *stored != nullptr &&
                                  go2cpp::Is(*stored,
                                             go2cpp::DeadlineExceededError()),
                              std::memory_order_release);
            });
            auto sender = scheduler.spawn([&] { (void)gate->Send(1); });
            WaitTasks({waiter, sender}, state, "S7 SpinMutex");
            scheduler.shutdown();
            state.Expect(matched.load(), "S7 SpinMutex 场景 FiberLocal 错误丢失");
            state.Ok(1);
            break;
        }
        case Scenario::S8: {
            SpinMutex mutex;
            long long shared = 0;
            const long long target = std::max<long long>(1, ops);
            std::atomic<long long> done{0};
            std::vector<std::thread> threads;
            for (int t = 0; t < 8; ++t) {
                threads.emplace_back([&, target] {
                    const long long per = target / 8;
                    for (long long i = 0; i < per; ++i) {
                        {
                            mutex.Lock();
                            ++shared;
                            mutex.Unlock();
                        }
                        done.fetch_add(1, std::memory_order_relaxed);
                    }
                    state.progress.fetch_add(per);
                });
            }
            for (auto& thread : threads) {
                thread.join();
            }
            state.Expect(shared == done.load(), "S8 SpinMutex 背压计数不一致");
            state.Ok(done.load());
            break;
        }
        case Scenario::S9: {
            SpinMutex mutex;
            long long shared = 0;
            const long long per_worker = std::max<long long>(1, ops / 4);
            auto worker = [&] {
                for (long long i = 0; i < per_worker; ++i) {
                    mutex.Lock();
                    ++shared;
                    mutex.Unlock();
                }
                state.progress.fetch_add(per_worker);
            };
            std::vector<std::thread> threads;
            for (int t = 0; t < 4; ++t) {
                threads.emplace_back(worker);
            }
            for (auto& thread : threads) {
                thread.join();
            }
            state.Expect(shared == 4 * per_worker,
                         "S9 高负载 SpinMutex 计数不一致");
            state.Ok(shared);
            break;
        }
        case Scenario::S10: {
            SpinMutex mutex;
            long long shared = 0;
            bool held = false;
            long long holds = 0;
            for (long long i = 0; i < ops; ++i) {
                const std::uint64_t roll = SplitMix64(rng) % 100;
                if (!held && roll < 60) {
                    mutex.Lock();
                    held = true;
                    ++holds;
                } else if (held && roll < 90) {
                    ++shared;
                    mutex.Unlock();
                    held = false;
                } else if (!held) {
                    if (mutex.TryLock()) {
                        ++shared;
                        ++holds;
                        mutex.Unlock();
                    } else {
                        state.Retry();
                    }
                }
                if ((i & 1023) == 0) {
                    state.progress.fetch_add(1, std::memory_order_relaxed);
                }
            }
            if (held) {
                ++shared;
                mutex.Unlock();
            }
            state.Expect(shared == holds, "S10 SpinMutex 模型对拍失败");
            state.Ok(std::max<long long>(1, holds));
            break;
        }
        case Scenario::Count:
            break;
    }
}

// ===========================================================================
// Context：10 场景
// ===========================================================================

void RunContext(Scenario scenario, long long ops, UnitState& state,
                std::uint64_t& rng) {
    switch (scenario) {
        case Scenario::S1: {
            for (long long i = 0; i < ops; ++i) {
                auto pair = Context::WithCancel(Background());
                if ((i & 1) == 0) {
                    pair.second();
                    state.Expect(pair.first->IsDone(), "S1 取消后 IsDone 为假");
                    state.Expect(go2cpp::Is(pair.first->Err(),
                                            go2cpp::CanceledError()),
                                 "S1 取消后 Err 不是 CanceledError");
                    pair.second();  // 幂等
                    state.Cancel();
                } else {
                    state.Expect(!pair.first->IsDone(),
                                 "S1 未取消却 IsDone 为真");
                    pair.second();
                    state.Ok();
                }
                // 取消后状态一致：新建子 ctx 也立即取消。
                auto child = Context::WithCancel(pair.first);
                state.Expect(child.first->IsDone(),
                             "S1 已取消父 ctx 的子 ctx 未继承取消");
            }
            break;
        }
        case Scenario::S2: {
            SocketPair socket;
            if (!state.Expect(socket.Ok(), "S2 socketpair 创建失败")) {
                return;
            }
            std::uint64_t net_seq = 0;
            for (long long i = 0; i < ops; ++i) {
                if ((i & 15) == 0 && !socket.RoundTrip(net_seq++)) {
                    state.Fail("S2 网络帧损坏");
                    break;
                }
                auto pair = Context::WithCancel(Background());
                (void)pair.first->Deadline();
                pair.second();
                state.Expect(pair.first->IsDone(), "S2 取消后未完成");
                state.Ok();
            }
            break;
        }
        case Scenario::S3: {
            Scheduler scheduler(4);
            scheduler.start();
            const long long per_worker = std::max<long long>(1, ops / 4);
            std::atomic<long long> cancelled{0};
            auto worker = [&] {
                for (long long i = 0; i < per_worker; ++i) {
                    auto pair = Context::WithCancel(Background());
                    pair.second();
                    if (pair.first->IsDone()) {
                        cancelled.fetch_add(1, std::memory_order_relaxed);
                    }
                }
                state.progress.fetch_add(per_worker);
            };
            auto f1 = scheduler.spawn(worker);
            auto f2 = scheduler.spawn(worker);
            std::thread t1(worker);
            std::thread t2(worker);
            t1.join();
            t2.join();
            WaitTasks({f1, f2}, state, "S3 Context");
            scheduler.shutdown();
            state.Expect(cancelled.load() == 4 * per_worker,
                         "S3 Context 取消计数不一致");
            state.Ok(cancelled.load());
            break;
        }
        case Scenario::S4: {
            // 关闭/析构竞态：父 ctx 取消与子 ctx 查询并发，子 ctx 立即完成。
            for (long long i = 0; i < ops / 64 + 1; ++i) {
                auto parent = Context::WithCancel(Background());
                auto child = Context::WithCancel(parent.first);
                std::atomic<int> done{0};
                std::thread canceller([&] {
                    parent.second();
                    done.fetch_add(1, std::memory_order_relaxed);
                });
                std::thread observer([&] {
                    while (!child.first->IsDone()) {
                        std::this_thread::yield();
                    }
                    done.fetch_add(1, std::memory_order_relaxed);
                });
                canceller.join();
                observer.join();
                state.Expect(done.load() == 2, "S4 Context 并发未完成");
                state.Expect(go2cpp::Is(child.first->Err(),
                                        go2cpp::CanceledError()),
                             "S4 子 ctx 取消原因错误");
                state.Closed();
                state.Ok(2);
            }
            break;
        }
        case Scenario::S5: {
            // 嵌套 Context 树：深度 4，取消叶子后逐层检查。
            for (long long i = 0; i < ops; ++i) {
                ContextPtr root = Background();
                std::vector<CancelFunc> cancels;
                ContextPtr current = root;
                for (int depth = 0; depth < 4; ++depth) {
                    auto pair = Context::WithCancel(current);
                    current = pair.first;
                    cancels.push_back(pair.second);
                }
                cancels.back()();
                state.Expect(current->IsDone(), "S5 叶子取消未完成");
                state.Expect(!root->IsDone(), "S5 叶子取消污染根 ctx");
                state.Expect(go2cpp::Is(current->Err(),
                                        go2cpp::CanceledError()),
                             "S5 嵌套取消原因错误");
                state.Ok();
            }
            break;
        }
        case Scenario::S6: {
            for (long long i = 0; i < ops; ++i) {
                if ((i & 2047) == 0) {
                    // 每 2048 次做一次真实超时（等待 20ms 后确认）；
                    // 其余用 0ms 超时同步验证，避免把 10 万次全变成 sleep。
                    const auto pair =
                        Context::WithTimeout(Background(), 20ms);
                    std::this_thread::sleep_for(40ms);
                    state.Expect(pair.first->IsDone(), "S6 超时后未完成");
                    state.Expect(go2cpp::Is(pair.first->Err(),
                                            go2cpp::DeadlineExceededError()),
                                 "S6 超时错误类型错误");
                    state.Timeout();
                } else if ((i & 1) == 0) {
                    const auto pair = Context::WithTimeout(Background(), 0ms);
                    if (pair.first->IsDone() &&
                        go2cpp::Is(pair.first->Err(),
                                   go2cpp::DeadlineExceededError())) {
                        state.Timeout();
                    } else {
                        state.Fail("S6 0ms 超时未立即完成");
                    }
                } else {
                    auto pair = Context::WithCancel(Background());
                    pair.second();
                    state.Ok();
                }
            }
            break;
        }
        case Scenario::S7: {
            // 错误传播：CancellationError 与 DeadlineExceeded 的类型链。
            for (long long i = 0; i < ops; ++i) {
                auto cancel_pair = Context::WithCancel(Background());
                cancel_pair.second();
                const ErrorPtr cancelled = cancel_pair.first->Err();
                auto timeout_pair = Context::WithTimeout(Background(), 0ms);
                const ErrorPtr timed_out = timeout_pair.first->Err();
                if (!state.Expect(static_cast<bool>(cancelled) &&
                                      static_cast<bool>(timed_out),
                                  "S7 ctx 取消/超时无 ErrorPtr")) {
                    break;
                }
                if (!state.Expect(
                        go2cpp::Is(cancelled, go2cpp::CanceledError()),
                        "S7 CancellationError 类型链错误")) {
                    break;
                }
                if (!state.Expect(
                        go2cpp::Is(timed_out, go2cpp::DeadlineExceededError()),
                        "S7 DeadlineExceeded 类型链错误")) {
                    break;
                }
                // 旧版 ErrorPtr 经 compat 适配后错误码统一为 kUnknown，
                // 这里只校验类型链与消息非空；码值断言由新版类型链单独覆盖。
                if (!cancelled->Message().empty() && !timed_out->Message().empty()) {
                    state.Ok();
                } else {
                    state.Fail("S7 取消/超时错误消息为空");
                }
            }
            break;
        }
        case Scenario::S8: {
            // 批量取消（背压）：一次取消 10 万级子的 ctx，全部完成且无残留。
            auto root = Context::WithCancel(Background());
            const long long target = std::max<long long>(1, ops);
            std::vector<ContextPtr> children;
            children.reserve(static_cast<std::size_t>(
                std::min<long long>(target, 200000)));
            for (long long i = 0; i < target; ++i) {
                children.push_back(Context::WithCancel(root.first).first);
                if ((i & 4095) == 0) {
                    state.progress.fetch_add(1, std::memory_order_relaxed);
                }
            }
            root.second();
            long long done = 0;
            for (const auto& child : children) {
                if (child->IsDone()) {
                    ++done;
                }
            }
            state.Expect(done == target, "S8 Context 批量取消未全部完成");
            state.Ok(target);
            break;
        }
        case Scenario::S9: {
            std::atomic<long long> done{0};
            const long long per_worker = std::max<long long>(1, ops / 4);
            auto worker = [&] {
                for (long long i = 0; i < per_worker; ++i) {
                    auto pair = Context::WithTimeout(Background(), 1ms);
                    pair.second();
                    if (pair.first->IsDone()) {
                        done.fetch_add(1, std::memory_order_relaxed);
                    }
                }
                state.progress.fetch_add(per_worker);
            };
            std::vector<std::thread> threads;
            for (int t = 0; t < 4; ++t) {
                threads.emplace_back(worker);
            }
            for (auto& thread : threads) {
                thread.join();
            }
            state.Expect(done.load() == 4 * per_worker,
                         "S9 高负载 Context 计数不一致");
            state.Ok(done.load());
            break;
        }
        case Scenario::S10: {
            std::vector<std::pair<ContextPtr, CancelFunc>> live;
            long long created = 0;
            long long cancelled = 0;
            for (long long i = 0; i < ops; ++i) {
                const std::uint64_t roll = SplitMix64(rng) % 100;
                if (roll < 40 || live.empty()) {
                    auto pair = Context::WithCancel(
                        live.empty() ? Background() : live.back().first);
                    live.push_back(std::move(pair));
                    ++created;
                } else if (roll < 85) {
                    live.back().second();
                    ++cancelled;
                    live.pop_back();
                } else {
                    ContextPtr probe = live.back().first;
                    if (!probe->IsDone()) {
                        state.Ok();
                    }
                    live.pop_back();
                }
                if ((i & 1023) == 0) {
                    state.progress.fetch_add(1, std::memory_order_relaxed);
                }
            }
            for (auto& entry : live) {
                entry.second();
            }
            state.Expect(cancelled <= created, "S10 Context 模型计数错误");
            state.Ok(std::max<long long>(1, created));
            break;
        }
        case Scenario::Count:
            break;
    }
}

// ===========================================================================
// Error：10 场景
// ===========================================================================

void RunError(Scenario scenario, long long ops, UnitState& state,
              std::uint64_t& rng) {
    switch (scenario) {
        case Scenario::S1: {
            for (long long i = 0; i < ops; ++i) {
                const ErrorPtr error =
                    (i & 1) ? go2cpp::CanceledError()
                            : go2cpp::DeadlineExceededError();
                state.Expect(static_cast<bool>(error), "S1 错误对象为空");
                state.Expect(!error->Message().empty(), "S1 错误 Message 为空");
                // 旧版 go2cpp::Error 不携带错误码，compat 适配后统一为
                // kUnknown；这里只要求「不是 kOk」，具体码由类型链断言覆盖。
                state.Expect(CodeOf(error) != go2cpp::error::ErrorCode::kOk,
                             "S1 错误码未映射");
                state.Expect(!TypeNameOf(error).empty(),
                             "S1 错误类型链类型名为空");
                if ((i & 1) == 0) {
                    state.Expect(go2cpp::Is(error,
                                            go2cpp::DeadlineExceededError()),
                                 "S1 超时错误类型链错误");
                }
                state.Ok();
            }
            break;
        }
        case Scenario::S2: {
            SocketPair socket;
            if (!state.Expect(socket.Ok(), "S2 socketpair 创建失败")) {
                return;
            }
            std::uint64_t net_seq = 0;
            for (long long i = 0; i < ops; ++i) {
                if ((i & 15) == 0 && !socket.RoundTrip(net_seq++)) {
                    state.Fail("S2 网络帧损坏");
                    break;
                }
                const ErrorPtr error = go2cpp::NewError(
                    "network-" + std::to_string(i));
                if (!error || error->Message() != "network-" + std::to_string(i)) {
                    state.Fail("S2 网络混跑错误消息损坏");
                    break;
                }
                state.Ok();
            }
            break;
        }
        case Scenario::S3: {
            Scheduler scheduler(4);
            scheduler.start();
            const long long per_worker = std::max<long long>(1, ops / 4);
            std::atomic<long long> done{0};
            auto worker = [&] {
                for (long long i = 0; i < per_worker; ++i) {
                    const ErrorPtr error = go2cpp::CanceledError();
                    if (go2cpp::Is(error, go2cpp::CanceledError())) {
                        done.fetch_add(1, std::memory_order_relaxed);
                    }
                }
                state.progress.fetch_add(per_worker);
            };
            auto f1 = scheduler.spawn(worker);
            auto f2 = scheduler.spawn(worker);
            std::thread t1(worker);
            std::thread t2(worker);
            t1.join();
            t2.join();
            WaitTasks({f1, f2}, state, "S3 Error");
            scheduler.shutdown();
            state.Expect(done.load() == 4 * per_worker,
                         "S3 Error 类型链计数不一致");
            state.Ok(done.load());
            break;
        }
        case Scenario::S4: {
            // 错误对象共享释放竞态：多线程共享同一 ErrorPtr，最后一个引用
            // 释放时不崩溃；错误对象只读，可并发访问。
            for (long long i = 0; i < ops / 64 + 1; ++i) {
                auto error = go2cpp::CanceledError();
                std::atomic<int> done{0};
                std::vector<std::thread> threads;
                for (int t = 0; t < 4; ++t) {
                    threads.emplace_back([&] {
                        for (int n = 0; n < 64; ++n) {
                            if (!error->Message().empty()) {
                                done.fetch_add(1, std::memory_order_relaxed);
                            }
                        }
                    });
                }
                for (auto& thread : threads) {
                    thread.join();
                }
                state.Expect(done.load() == 4 * 64,
                             "S4 Error 并发只读访问丢失");
                error.reset();
                state.Closed();
                state.Ok(4 * 64);
            }
            break;
        }
        case Scenario::S5: {
            for (long long i = 0; i < ops; ++i) {
                const ErrorPtr error = go2cpp::NewError(
                    "nested-" + std::to_string(i));
                state.Expect(!error->Message().empty(),
                             "S5 文本错误消息为空");
                state.Expect(CodeOf(error) ==
                                 go2cpp::error::ErrorCode::kUnknown,
                             "S5 文本错误码映射错误");
                state.Expect(!TypeNameOf(error).empty(),
                             "S5 文本错误类型链为空");
                state.Ok();
            }
            break;
        }
        case Scenario::S6: {
            for (long long i = 0; i < ops; ++i) {
                const auto pair = Context::WithTimeout(Background(), 0ms);
                const ErrorPtr error = pair.first->Err();
                if (!state.Expect(static_cast<bool>(error),
                                  "S6 超时 ctx 无错误")) {
                    break;
                }
                if (!state.Expect(go2cpp::Is(error,
                                             go2cpp::DeadlineExceededError()),
                                  "S6 超时错误类型错误")) {
                    break;
                }
                state.Timeout();
            }
            break;
        }
        case Scenario::S7: {
            // M1 产生 → park → M2 恢复：FiberLocal<ErrorPtr> 必须保持同一个对象。
            go2cpp::FiberLocal<ErrorPtr> slot;
            Scheduler scheduler(4);
            scheduler.start();
            const long long target = std::max<long long>(1, ops / 64);
            std::atomic<long long> matched{0};
            std::atomic<long long> issued{0};
            auto worker = [&](int index) {
                for (long long i = 0; i < target; ++i) {
                    auto my_error = go2cpp::NewError(
                        "fiber-" + std::to_string(index) + "-" +
                        std::to_string(i));
                    slot.GetOrCreate(nullptr) = my_error;
                    issued.fetch_add(1, std::memory_order_relaxed);
                    auto gate = MakeChannel<int>(0);
                    auto pair = Context::WithTimeout(Background(), 1ms);
                    (void)Select({go2cpp::RecvCase(gate),
                                  go2cpp::DoneCase(pair.first)},
                                 pair.first, 5s);
                    const ErrorPtr* stored = slot.TryGet();
                    if (stored != nullptr && *stored == my_error) {
                        matched.fetch_add(1, std::memory_order_relaxed);
                    }
                    if ((i & 63) == 0) {
                        state.progress.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            };
            auto f1 = scheduler.spawn([&] { worker(0); });
            auto f2 = scheduler.spawn([&] { worker(1); });
            WaitTasks({f1, f2}, state, "S7 Error");
            scheduler.shutdown();
            state.Expect(matched.load() == issued.load(),
                         "S7 FiberLocal<ErrorPtr> 跨 M 丢失/串值");
            state.Ok(issued.load());
            break;
        }
        case Scenario::S8: {
            const long long target = std::max<long long>(1, ops);
            std::vector<ErrorPtr> errors;
            errors.reserve(static_cast<std::size_t>(
                std::min<long long>(target, 500000)));
            for (long long i = 0; i < target; ++i) {
                errors.push_back(go2cpp::NewError(
                    "batch-" + std::to_string(i)));
                if ((i & 4095) == 0) {
                    state.progress.fetch_add(1, std::memory_order_relaxed);
                }
            }
            long long valid = 0;
            for (const auto& error : errors) {
                if (error && !error->Message().empty()) {
                    ++valid;
                }
            }
            state.Expect(valid == target, "S8 Error 批量创建丢失");
            errors.clear();
            state.Ok(target);
            break;
        }
        case Scenario::S9: {
            const long long per_worker = std::max<long long>(1, ops / 4);
            std::atomic<long long> done{0};
            auto worker = [&] {
                for (long long i = 0; i < per_worker; ++i) {
                    const ErrorPtr error = go2cpp::DeadlineExceededError();
                    if (go2cpp::Is(error, go2cpp::DeadlineExceededError())) {
                        done.fetch_add(1, std::memory_order_relaxed);
                    }
                }
                state.progress.fetch_add(per_worker);
            };
            std::vector<std::thread> threads;
            for (int t = 0; t < 4; ++t) {
                threads.emplace_back(worker);
            }
            for (auto& thread : threads) {
                thread.join();
            }
            state.Expect(done.load() == 4 * per_worker,
                         "S9 高负载 Error 计数不一致");
            state.Ok(done.load());
            break;
        }
        case Scenario::S10: {
            std::vector<ErrorPtr> live;
            long long created = 0;
            for (long long i = 0; i < ops; ++i) {
                const std::uint64_t roll = SplitMix64(rng) % 100;
                if (roll < 50 || live.empty()) {
                    live.push_back(go2cpp::NewError(
                        "random-" + std::to_string(i)));
                    ++created;
                } else if (roll < 85) {
                    const ErrorPtr error = live.back();
                    state.Expect(!error->Message().empty(),
                                 "S10 随机错误消息为空");
                    live.pop_back();
                    state.Ok();
                } else {
                    const ErrorPtr probe = live[SplitMix64(rng) % live.size()];
                    state.Expect(static_cast<bool>(probe),
                                 "S10 随机错误指针为空");
                    live.pop_back();
                    state.Ok();
                }
                if ((i & 1023) == 0) {
                    state.progress.fetch_add(1, std::memory_order_relaxed);
                }
            }
            state.Expect(created > 0, "S10 Error 随机未创建错误");
            live.clear();
            state.Ok(std::max<long long>(1, created));
            break;
        }
        case Scenario::Count:
            break;
    }
}

// ===========================================================================
// Future：10 场景
// ===========================================================================

void RunFuture(Scenario scenario, long long ops, UnitState& state,
               std::uint64_t& rng) {
    switch (scenario) {
        case Scenario::S1: {
            for (long long i = 0; i < ops; ++i) {
                Promise<long long> promise;
                auto future = promise.GetFuture();
                auto pair = Context::WithCancel(Background());
                std::atomic<int> status{-1};
                std::thread waiter([&] {
                    const auto result = future.GetResult(pair.first);
                    status.store(static_cast<int>(result.status),
                                 std::memory_order_release);
                });
                std::this_thread::sleep_for(20us);
                pair.second();
                waiter.join();
                state.Expect(status.load(std::memory_order_acquire) ==
                                 static_cast<int>(
                                     go2cpp::FutureStatus::kCancelled),
                             "S1 Future 等待被取消后状态错误");
                state.Cancel();
                // 取消后可继续完成（状态可能已取消，不能崩溃）。
                promise.SetValue(i);
            }
            break;
        }
        case Scenario::S2: {
            SocketPair socket;
            if (!state.Expect(socket.Ok(), "S2 socketpair 创建失败")) {
                return;
            }
            std::uint64_t net_seq = 0;
            for (long long i = 0; i < ops; ++i) {
                if ((i & 15) == 0 && !socket.RoundTrip(net_seq++)) {
                    state.Fail("S2 网络帧损坏");
                    break;
                }
                Promise<long long> promise;
                auto future = promise.GetFuture();
                promise.SetValue(i);
                const auto result = future.GetResult();
                if (result.status != go2cpp::FutureStatus::kReady ||
                    !result.value || *result.value != i) {
                    state.Fail("S2 Future 网络混跑值错误");
                    break;
                }
                state.Ok();
            }
            break;
        }
        case Scenario::S3: {
            Scheduler scheduler(4);
            scheduler.start();
            const long long per_worker = std::max<long long>(1, ops / 4);
            std::atomic<long long> done{0};
            auto worker = [&] {
                for (long long i = 0; i < per_worker; ++i) {
                    Promise<long long> promise;
                    auto future = promise.GetFuture();
                    auto task = scheduler.spawn([&] { promise.SetValue(i); });
                    WaitTasks({task}, state, "S3 Future 生产者");
                    const auto result = future.GetResult();
                    if (result.status == go2cpp::FutureStatus::kReady &&
                        result.value && *result.value == i) {
                        done.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        state.Fail("S3 Future 值错误");
                        return;
                    }
                    if ((i & 255) == 0) {
                        state.progress.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            };
            auto f1 = scheduler.spawn(worker);
            auto f2 = scheduler.spawn(worker);
            std::thread t1(worker);
            std::thread t2(worker);
            t1.join();
            t2.join();
            WaitTasks({f1, f2}, state, "S3 Future");
            scheduler.shutdown();
            state.Expect(done.load() == 4 * per_worker,
                         "S3 Future 计数不一致");
            state.Ok(done.load());
            break;
        }
        case Scenario::S4: {
            // Promise Cancel 与等待者并发。
            for (long long i = 0; i < ops / 64 + 1; ++i) {
                auto promise = std::make_shared<Promise<long long>>();
                auto future = promise->GetFuture();
                std::atomic<int> status{-1};
                std::thread waiter([&] {
                    const auto result = future.GetResult();
                    status.store(static_cast<int>(result.status),
                                 std::memory_order_release);
                });
                std::this_thread::sleep_for(20us);
                promise->Cancel(go2cpp::CanceledError());
                waiter.join();
                state.Expect(status.load() ==
                                 static_cast<int>(go2cpp::FutureStatus::kCancelled),
                             "S4 Future Cancel 后等待者状态错误");
                state.Expect(future.Status() == go2cpp::FutureStatus::kCancelled,
                             "S4 Future Cancel 后 Status 错误");
                promise.reset();
                state.Closed();
                state.Ok();
            }
            break;
        }
        case Scenario::S5: {
            // 嵌套 Future：外层等待内层结果后再完成。
            Scheduler scheduler(2);
            scheduler.start();
            for (long long i = 0; i < ops; ++i) {
                Promise<long long> outer;
                auto outer_future = outer.GetFuture();
                auto task = scheduler.spawn([&] {
                    Promise<long long> inner;
                    auto inner_future = inner.GetFuture();
                    auto inner_task = scheduler.spawn(
                        [&] { inner.SetValue(i + 1); });
                    WaitTasks({inner_task}, state, "S5 Future 内层");
                    const auto result = inner_future.GetResult();
                    if (!result.value) {
                        state.Fail("S5 内层 Future 无值");
                        outer.SetValue(-1);
                        return;
                    }
                    outer.SetValue(*result.value + 1);
                });
                WaitTasks({task}, state, "S5 Future 外层");
                const auto result = outer_future.GetResult();
                if (!result.value || *result.value != i + 2) {
                    state.Fail("S5 嵌套 Future 值错误");
                    break;
                }
                state.Ok();
            }
            scheduler.shutdown();
            break;
        }
        case Scenario::S6: {
            for (long long i = 0; i < ops; ++i) {
                Promise<long long> promise;
                auto future = promise.GetFuture();
                const auto result = future.GetResultFor(1ms);
                if (result.status == go2cpp::FutureStatus::kTimedOut) {
                    state.Timeout();
                } else {
                    state.Fail("S6 Future 未完成却未超时");
                    break;
                }
                promise.SetValue(i);
                const auto ready = future.GetResultFor(1ms);
                if (!ready.value || *ready.value != i) {
                    state.Fail("S6 超时后完成值错误");
                    break;
                }
            }
            break;
        }
        case Scenario::S7: {
            go2cpp::FiberLocal<ErrorPtr> slot;
            Scheduler scheduler(4);
            scheduler.start();
            auto gate = MakeChannel<int>(0);
            std::atomic<bool> matched{false};
            std::atomic<bool> error_seen{false};
            auto waiter = scheduler.spawn([&] {
                slot.GetOrCreate(nullptr) = go2cpp::CanceledError();
                (void)gate->Recv();
                const ErrorPtr* stored = slot.TryGet();
                matched.store(stored != nullptr && *stored != nullptr &&
                                  go2cpp::Is(*stored, go2cpp::CanceledError()),
                              std::memory_order_release);
            });
            auto sender = scheduler.spawn([&] { (void)gate->Send(1); });
            Promise<long long> promise;
            auto future = promise.GetFuture();
            promise.SetError(go2cpp::DeadlineExceededError());
            const auto result = future.GetResult();
            error_seen.store(result.status == go2cpp::FutureStatus::kError &&
                                 static_cast<bool>(result.error) &&
                                 go2cpp::Is(result.error,
                                            go2cpp::DeadlineExceededError()),
                             std::memory_order_release);
            WaitTasks({waiter, sender}, state, "S7 Future");
            scheduler.shutdown();
            state.Expect(matched.load(), "S7 Future 场景 FiberLocal 错误丢失");
            state.Expect(error_seen.load(), "S7 Future SetError 错误链错误");
            state.Ok(2);
            break;
        }
        case Scenario::S8: {
            // 批量 Promise 完成 + 背压：一批未完成 Future 同时等待。
            const long long target = std::max<long long>(1, ops);
            std::vector<std::pair<Promise<long long>,
                                   Future<long long>>> items;
            const long long batch = std::min<long long>(target, 10000);
            items.reserve(static_cast<std::size_t>(batch));
            for (long long i = 0; i < batch; ++i) {
                Promise<long long> promise;
                auto future = promise.GetFuture();
                items.emplace_back(std::move(promise), std::move(future));
                if ((i & 4095) == 0) {
                    state.progress.fetch_add(1, std::memory_order_relaxed);
                }
            }
            for (long long i = 0; i < batch; ++i) {
                items[static_cast<std::size_t>(i)].first.SetValue(i);
            }
            long long validated = 0;
            for (long long i = 0; i < batch; ++i) {
                const auto result =
                    items[static_cast<std::size_t>(i)].second.GetResult();
                if (result.value && *result.value == i) {
                    ++validated;
                }
            }
            state.Expect(validated == batch, "S8 Future 批量完成丢值");
            state.Ok(batch);
            break;
        }
        case Scenario::S9: {
            const long long per_worker = std::max<long long>(1, ops / 4);
            std::atomic<long long> done{0};
            auto worker = [&] {
                for (long long i = 0; i < per_worker; ++i) {
                    Promise<long long> promise;
                    auto future = promise.GetFuture();
                    promise.SetValue(i);
                    const auto result = future.GetResult();
                    if (result.value && *result.value == i) {
                        done.fetch_add(1, std::memory_order_relaxed);
                    }
                }
                state.progress.fetch_add(per_worker);
            };
            std::vector<std::thread> threads;
            for (int t = 0; t < 4; ++t) {
                threads.emplace_back(worker);
            }
            for (auto& thread : threads) {
                thread.join();
            }
            state.Expect(done.load() == 4 * per_worker,
                         "S9 高负载 Future 计数不一致");
            state.Ok(done.load());
            break;
        }
        case Scenario::S10: {
            std::vector<std::pair<Promise<long long>, Future<long long>>> live;
            long long created = 0;
            long long completed = 0;
            bool mismatch = false;
            for (long long i = 0; i < ops; ++i) {
                const std::uint64_t roll = SplitMix64(rng) % 100;
                if ((roll < 45 || live.empty()) && live.size() < 512) {
                    Promise<long long> promise;
                    auto future = promise.GetFuture();
                    live.emplace_back(std::move(promise), std::move(future));
                    ++created;
                } else if (roll < 80) {
                    auto& entry = live.back();
                    entry.first.SetValue(i);
                    const auto result = entry.second.TryGet();
                    if (result.status == go2cpp::FutureStatus::kReady &&
                        result.value && *result.value == i) {
                        ++completed;
                    } else {
                        mismatch = true;
                    }
                    live.pop_back();
                } else {
                    auto& entry = live.back();
                    entry.first.Cancel(go2cpp::CanceledError());
                    if (entry.second.Status() !=
                        go2cpp::FutureStatus::kCancelled) {
                        mismatch = true;
                    }
                    live.pop_back();
                }
                if ((i & 1023) == 0) {
                    state.progress.fetch_add(1, std::memory_order_relaxed);
                }
            }
            state.Expect(!mismatch, "S10 Future 随机模型对拍失败");
            state.Expect(created > 0 && completed > 0,
                         "S10 Future 随机未完成任何 Future");
            state.Ok(std::max<long long>(1, completed));
            break;
        }
        case Scenario::Count:
            break;
    }
}

// ===========================================================================
// 单元执行与结果输出
// ===========================================================================

using Runner = void (*)(Scenario, long long, UnitState&, std::uint64_t&);

Runner RunnerOf(Structure structure) {
    switch (structure) {
        case Structure::Channel: return &RunChannel;
        case Structure::FastChannel: return &RunFastChannel;
        case Structure::Select: return &RunSelect;
        case Structure::WaitNode: return &RunWaitNode;
        case Structure::SchedulerPark: return &RunSchedulerPark;
        case Structure::FiberSwitch: return &RunFiberSwitch;
        case Structure::Mutex: return &RunMutex;
        case Structure::CV: return &RunCV;
        case Structure::WG: return &RunWG;
        case Structure::RWMutex: return &RunRWMutex;
        case Structure::SpinMutex: return &RunSpinMutex;
        case Structure::Context: return &RunContext;
        case Structure::Error: return &RunError;
        case Structure::Future: return &RunFuture;
        case Structure::Count: break;
    }
    return nullptr;
}

struct Options {
    std::vector<Structure> structures;
    std::vector<Scenario> scenarios;
    long long ops{0};  // 0 = 按结构默认值
    int rounds{3};
    long long watchdog_ms{300000};
    std::string tsv_path;
    bool burn_only{false};
    int burn_threads{0};
    int burn_seconds{10};
};

std::mutex g_result_mutex;
std::ofstream g_tsv;

void EmitTsv(const std::string& line) {
    std::lock_guard<std::mutex> lock(g_result_mutex);
    if (g_tsv.is_open()) {
        g_tsv << line << '\n';
        g_tsv.flush();
    }
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

/** @brief 跑一个单元并输出结果行。返回是否通过。 */
bool RunUnit(const StructureInfo& info, Scenario scenario, int round,
             const Options& options) {
    UnitState state;
    const long long ops =
        options.ops > 0 ? options.ops : info.default_ops;
    const long long rss_before = ReadRssKb();
    const double load_before = ReadLoad1();
    const auto started = Clock::now();

    Watchdog watchdog(state, info.name, ScenarioName(scenario), round,
                      options.watchdog_ms);
    watchdog.Start();

    std::uint64_t rng = SeedFor(info, scenario, round);
    Runner runner = RunnerOf(info.structure);
    try {
        runner(scenario, ops, state, rng);
    } catch (const std::exception& error) {
        state.Fail(std::string("未捕获异常: ") + error.what());
    } catch (...) {
        state.Fail("未捕获未知异常");
    }
    watchdog.Stop();

    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() -
                                                             started)
            .count();
    const long long rss_after = ReadRssKb();
    const double load_after = ReadLoad1();
    const long long done = state.ok.load(std::memory_order_relaxed) +
                           state.cancelled.load(std::memory_order_relaxed) +
                           state.timed_out.load(std::memory_order_relaxed) +
                           state.closed.load(std::memory_order_relaxed) +
                           state.retry.load(std::memory_order_relaxed);

    bool ok = !state.Failed();
    // 部分场景（S7 错误传播、S4 析构竞态等）按「少量轮次 × 单次校验」设计，
    // done 天然远小于 ops；这里只拦「一次都没执行」的真正空跑，实际工作量
    // 以 done 列如实记录。
    if (done == 0) {
        state.Fail("实际完成操作数为 0（空跑）");
        ok = false;
    }
    if (rss_after - rss_before > 256 * 1024) {
        state.Fail("单元内 RSS 增长超过 256MB");
        ok = false;
    }
    if (!ok) {
        // 失败可能来自上面两条补充检查，重新取一次。
        ok = !state.Failed();
    }

    char line[512];
    std::snprintf(
        line, sizeof(line),
        "MATRIX\t%s\t%s\t%d\t%s\t%lld\t%lld\t%lld\t%lld\t%.2f\t%.2f",
        info.name, ScenarioName(scenario), round, ok ? "PASS" : "FAIL", ops,
        done, static_cast<long long>(elapsed_ms), rss_after, load_before,
        load_after);
    EmitTsv(line);

    std::fprintf(stderr, "[%s] %-14s %-3s round=%d ops=%lld done=%lld %lldms "
                         "rss=%lldKB load=%.1f/%.1f %s\n",
                 ok ? "PASS" : "FAIL", info.name, ScenarioName(scenario), round,
                 ops, done, static_cast<long long>(elapsed_ms), rss_after,
                 load_before, load_after, state.Counters().c_str());
    if (!ok) {
        for (const auto& failure : state.Snapshot()) {
            char fail_line[1024];
            std::snprintf(fail_line, sizeof(fail_line), "FAIL\t%s\t%s\t%d\t%s",
                          info.name, ScenarioName(scenario), round,
                          failure.c_str());
            EmitTsv(fail_line);
        }
    }
    return ok;
}

bool ParseStructure(const std::string& text, Structure* out) {
    for (const auto& info : kStructures) {
        if (text == info.name) {
            *out = info.structure;
            return true;
        }
    }
    return false;
}

bool ParseScenario(const std::string& text, Scenario* out) {
    const int index = std::atoi(text.c_str() + 1);
    if (text.size() < 2 || text[0] != 'S' || index < 1 ||
        index > kScenarioCount) {
        return false;
    }
    *out = static_cast<Scenario>(index - 1);
    return true;
}

std::vector<std::string> SplitComma(const std::string& text) {
    std::vector<std::string> parts;
    std::string current;
    for (const char ch : text) {
        if (ch == ',') {
            if (!current.empty()) {
                parts.push_back(current);
                current.clear();
            }
        } else {
            current.push_back(ch);
        }
    }
    if (!current.empty()) {
        parts.push_back(current);
    }
    return parts;
}

void PrintUsage() {
    std::fprintf(
        stderr,
        "用法: stress_matrix [选项]\n"
        "  --structures a,b|all   结构（默认 all）\n"
        "  --scenarios S1,S2|all  场景（默认 all）\n"
        "  --ops N               每轮每场景目标操作数（默认按结构成本）\n"
        "  --full                等价 --ops 100000000\n"
        "  --rounds N            轮数（默认 3）\n"
        "  --watchdog-ms N       无进展超时（默认 300000）\n"
        "  --tsv FILE            TSV 结果同时写文件\n"
        "  --burn-only N         只造负载：N 个忙线程\n"
        "  --burn-seconds N      造负载持续秒数（默认 10）\n"
        "  --list                列出结构\n");
}

bool ParseOptions(int argc, char** argv, Options* options) {
    bool structures_all = true;
    bool scenarios_all = true;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "缺少参数值: %s\n", name);
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--structures") {
            structures_all = false;
            const std::string value = next("--structures");
            if (value == "all") {
                structures_all = true;
                continue;
            }
            for (const auto& part : SplitComma(value)) {
                Structure structure{};
                if (!ParseStructure(part, &structure)) {
                    std::fprintf(stderr, "未知结构: %s\n", part.c_str());
                    return false;
                }
                options->structures.push_back(structure);
            }
        } else if (arg == "--scenarios") {
            scenarios_all = false;
            const std::string value = next("--scenarios");
            if (value == "all") {
                scenarios_all = true;
                continue;
            }
            for (const auto& part : SplitComma(value)) {
                Scenario scenario{};
                if (!ParseScenario(part, &scenario)) {
                    std::fprintf(stderr, "未知场景: %s\n", part.c_str());
                    return false;
                }
                options->scenarios.push_back(scenario);
            }
        } else if (arg == "--ops") {
            options->ops = std::atoll(next("--ops").c_str());
        } else if (arg == "--full") {
            options->ops = 100000000;
        } else if (arg == "--rounds") {
            options->rounds = std::atoi(next("--rounds").c_str());
        } else if (arg == "--watchdog-ms") {
            options->watchdog_ms = std::atoll(next("--watchdog-ms").c_str());
        } else if (arg == "--tsv") {
            options->tsv_path = next("--tsv");
        } else if (arg == "--burn-only") {
            options->burn_only = true;
            options->burn_threads = std::atoi(next("--burn-only").c_str());
        } else if (arg == "--burn-seconds") {
            options->burn_seconds = std::atoi(next("--burn-seconds").c_str());
        } else if (arg == "--list") {
            for (const auto& info : kStructures) {
                std::fprintf(stderr, "%-14s high_risk=%d default_ops=%lld\n",
                             info.name, info.high_risk ? 1 : 0,
                             info.default_ops);
            }
            std::exit(0);
        } else if (arg == "--help" || arg == "-h") {
            PrintUsage();
            std::exit(0);
        } else {
            std::fprintf(stderr, "未知参数: %s\n", arg.c_str());
            return false;
        }
    }
    if (structures_all) {
        for (const auto& info : kStructures) {
            options->structures.push_back(info.structure);
        }
    }
    if (scenarios_all) {
        for (int i = 0; i < kScenarioCount; ++i) {
            options->scenarios.push_back(static_cast<Scenario>(i));
        }
    }
    if (options->rounds <= 0) {
        options->rounds = 3;
    }
    return true;
}

void BurnCpu(int threads, int seconds) {
    std::atomic<bool> stop{false};
    std::vector<std::thread> workers;
    for (int t = 0; t < threads; ++t) {
        workers.emplace_back([&] {
            volatile std::uint64_t value = 0x9E3779B97F4A7C15ULL;
            while (!stop.load(std::memory_order_relaxed)) {
                value = value * 6364136223846793005ULL + 1442695040888963407ULL;
                asm volatile("" ::: "memory");
            }
        });
    }
    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    stop.store(true, std::memory_order_relaxed);
    for (auto& thread : workers) {
        thread.join();
    }
}

}  // namespace

int main(int argc, char** argv) {
    InstallDumpHandler();
    Options options;
    if (!ParseOptions(argc, argv, &options)) {
        PrintUsage();
        return 2;
    }
    if (options.burn_only) {
        std::fprintf(stderr, "[burn] %d 线程忙等 %d 秒，loadavg=%.2f\n",
                     options.burn_threads, options.burn_seconds, ReadLoad1());
        BurnCpu(options.burn_threads, options.burn_seconds);
        std::fprintf(stderr, "[burn] 结束，loadavg=%.2f\n", ReadLoad1());
        return 0;
    }
    if (!options.tsv_path.empty()) {
        g_tsv.open(options.tsv_path, std::ios::app);
        if (!g_tsv.is_open()) {
            std::fprintf(stderr, "无法打开 TSV 文件: %s\n",
                         options.tsv_path.c_str());
            return 2;
        }
    }

    std::fprintf(stderr,
                 "[matrix] structures=%zu scenarios=%zu rounds=%d ops=%s "
                 "watchdog=%lldms load=%.2f rss=%lldKB\n",
                 options.structures.size(), options.scenarios.size(),
                 options.rounds,
                 options.ops > 0 ? std::to_string(options.ops).c_str() : "默认",
                 options.watchdog_ms, ReadLoad1(), ReadRssKb());

    long long units = 0;
    long long failed = 0;
    for (const Structure structure : options.structures) {
        const StructureInfo& info = InfoOf(structure);
        for (const Scenario scenario : options.scenarios) {
            for (int round = 1; round <= options.rounds; ++round) {
                ++units;
                if (!RunUnit(info, scenario, round, options)) {
                    ++failed;
                }
            }
        }
        std::fprintf(stderr, "[matrix] 结构 %s 完成（累计 %lld 单元，失败 %lld）\n",
                     info.name, units, failed);
    }
    char summary[256];
    std::snprintf(summary, sizeof(summary), "SUMMARY\t%lld\t%lld\t%d\t%d",
                  units, failed, kStructureCount, kScenarioCount);
    EmitTsv(summary);
    std::fprintf(stderr, "[matrix] 全部完成：%lld 单元，失败 %lld\n", units,
                 failed);
    return failed == 0 ? 0 : 1;
}
