/**
 * @file channel_thread_mixed_retry.cpp
 * @brief 复现 2026-10-21 基线压测中「线程版 Channel 混合场景挂起」的专用装置。
 *
 * ## 原始触发条件（来自基线压测记录）
 *
 * 命令：`conc_OFF --scenario=mixed --n=2000 --lat-n=500 --config=SMOKE`
 * （OFF 构建；同一进程内先跑吞吐轮、再跑延迟轮，复用同一个 cap=64 channel）
 *
 * 拓扑：
 * - 2 个 worker 线程：`sync::Mutex` 加锁 ++counter → `Channel::Send(i)` →
 *   `{lock pc_mutex; ++flag;} ParkingCondition::notify_one()`；
 * - 1 个 consumer 线程：`Channel::Recv()` 共 n 次；
 * - 1 个 helper 线程：`ParkingCondition::wait(pc_mutex, flag != seen || stop)`；
 * - 每个 worker/consumer 完成后 `Gate::Arrive()` 并**自旋**等主线程快照；
 * - 主线程 `Gate::WaitAll(worker 数 + 1)`，读事件环后 `stop=true`、
 *   `notify_all`、`Gate::Release()`，再 join。
 *
 * 第一次挂起已定位为**测试设计 bug**：`Gate::WaitAll(worker 数 + 2)`
 * 把 helper 也计入，但 helper 只在 `stop` 后退出，而 `stop` 在 `WaitAll`
 * 之后才设置 → 必然死锁。用 `--legacy-gate-bug` 可 1 轮必现。
 *
 * 第二次挂起（把计数改成 +1 后）原因未定，本装置用于长跑复现：
 * watchdog 在 `Gate::WaitAll` 超过 `--watchdog-sec` 秒未返回时打印每个
 * 线程的 role/iter/stage 与 `Channel::DebugSnapshot`，可选 gdb bt。
 *
 * ## 用法
 * ```text
 * channel_thread_mixed_retry --rounds=100 --n=2000 --lat-n=500
 * channel_thread_mixed_retry --legacy-gate-bug --rounds=1
 * channel_thread_mixed_retry --rounds=1000 --n=100000 --cap=0 --workers=4
 * taskset -c 0 channel_thread_mixed_retry --rounds=200 --n=2000
 * ```
 *
 * ## 边界
 *
 * - 只用于复现实验，不修改 `src/**`；默认 OFF 构建（与当初一致）。
 * - stage 记录是 relaxed 原子写，会给每轮增加几十纳秒；`--no-stage`
 *   可回到更接近原始版本的时序（此时只能看 iter 与主线程阶段）。
 */

#include "go2cpp/concurrency/channel.hpp"
#include "go2cpp/concurrency/core/parking_condition.hpp"
#include "go2cpp/concurrency/scheduler.hpp"
#include "go2cpp/sync.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/prctl.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

std::uint64_t NowNs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now().time_since_epoch())
            .count());
}

constexpr int kMaxSlots = 32;

/// 每个线程一个可观测槽：role 固定，iter/stage 单调推进。
struct Slot {
    std::atomic<const char*> role{nullptr};
    std::atomic<std::uint64_t> iters{0};
    std::atomic<std::uint32_t> stage{0};
};

Slot g_slots[kMaxSlots];
std::atomic<int> g_slot_count{0};
std::atomic<std::uint32_t> g_main_stage{0};
std::atomic<const char*> g_phase{"init"};
std::atomic<bool> g_stage_enabled{true};

/// watchdog 观察的 Gate 等待窗口。
std::atomic<bool> g_gate_waiting{false};
std::atomic<std::uint64_t> g_gate_wait_start{0};
std::atomic<std::uint64_t> g_round{0};
std::atomic<bool> g_watchdog_stop{false};
std::atomic<int> g_watchdog_seconds{10};
std::atomic<bool> g_gdb_enabled{false};
std::atomic<bool> g_hang_reported{false};
std::atomic<go2cpp::Channel<int>*> g_channel{nullptr};

/// Gate 等待超时后打印现场并用 124 退出（脚本据 rc 判定挂起）。
void PrintSceneAndExit(const char* reason) {
    if (g_hang_reported.exchange(true)) {
        return;
    }
    std::fprintf(stderr,
                 "\n[hang] %s round=%llu phase=%s main_stage=%u\n",
                 reason,
                 static_cast<unsigned long long>(
                     g_round.load(std::memory_order_relaxed)),
                 g_phase.load(std::memory_order_relaxed),
                 g_main_stage.load(std::memory_order_relaxed));
    const int count = g_slot_count.load(std::memory_order_relaxed);
    for (int i = 0; i < count && i < kMaxSlots; ++i) {
        const char* role = g_slots[i].role.load(std::memory_order_relaxed);
        std::fprintf(stderr,
                     "  slot %-2d role=%-16s iter=%-10llu stage=%u\n", i,
                     role != nullptr ? role : "?",
                     static_cast<unsigned long long>(
                         g_slots[i].iters.load(std::memory_order_relaxed)),
                     g_slots[i].stage.load(std::memory_order_relaxed));
    }
    go2cpp::Channel<int>* channel =
        g_channel.load(std::memory_order_acquire);
    if (channel != nullptr) {
        const go2cpp::ChannelDebugSnapshot snapshot = channel->DebugSnapshot();
        std::fprintf(stderr,
                     "  channel: cap=%zu len=%zu closed=%d send_waiters=%zu "
                     "recv_waiters=%zu select_senders=%zu select_receivers=%zu "
                     "gen=%llu\n",
                     snapshot.capacity, snapshot.length,
                     snapshot.closed ? 1 : 0, snapshot.send_waiters,
                     snapshot.recv_waiters, snapshot.select_senders,
                     snapshot.select_receivers,
                     static_cast<unsigned long long>(snapshot.generation));
    }
    std::fflush(stderr);
    if (g_gdb_enabled.load(std::memory_order_relaxed)) {
        char command[512];
        std::snprintf(command, sizeof(command),
                      "gdb -p %d -batch -ex 'thread apply all bt' "
                      "> /tmp/channel_mixed_retry_bt.txt 2>&1",
                      static_cast<int>(::getpid()));
        const int gdb_rc = std::system(command);
        (void)gdb_rc;
        std::fprintf(stderr, "  gdb bt -> /tmp/channel_mixed_retry_bt.txt\n");
    }
    _exit(124);
}

void WatchdogLoop() {
    while (!g_watchdog_stop.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (!g_gate_waiting.load(std::memory_order_acquire)) {
            continue;
        }
        const std::uint64_t start =
            g_gate_wait_start.load(std::memory_order_relaxed);
        const std::uint64_t now = NowNs();
        const std::uint64_t limit =
            static_cast<std::uint64_t>(g_watchdog_seconds.load()) *
            1000000000ULL;
        if (start != 0 && now > start && now - start > limit) {
            PrintSceneAndExit("gate wait timeout");
            return;
        }
    }
}

/// RAII：把当前线程的 stage 更新为 `stage`（函数返回时保留，便于抓现场）。
void SetStage(int slot, std::uint32_t stage) {
    if (g_stage_enabled.load(std::memory_order_relaxed)) {
        g_slots[slot].stage.store(stage, std::memory_order_relaxed);
    }
}

void Touch(int slot) {
    g_slots[slot].iters.fetch_add(1, std::memory_order_relaxed);
}

int AllocSlot(const char* role) {
    const int id = g_slot_count.fetch_add(1, std::memory_order_relaxed);
    if (id < 0 || id >= kMaxSlots) {
        std::fprintf(stderr, "slot overflow\n");
        std::_Exit(3);
    }
    g_slots[id].role.store(role, std::memory_order_relaxed);
    return id;
}

void ResetSlots() {
    g_slot_count.store(0, std::memory_order_relaxed);
    for (auto& slot : g_slots) {
        slot.role.store(nullptr, std::memory_order_relaxed);
        slot.iters.store(0, std::memory_order_relaxed);
        slot.stage.store(0, std::memory_order_relaxed);
    }
}

/// 完成门：worker/consumer 跑完业务后自旋，主线程读现场后放行。
struct Gate {
    std::atomic<int> arrived{0};
    std::atomic<bool> released{false};

    void Arrive() { arrived.fetch_add(1, std::memory_order_acq_rel); }
    void WaitAll(int n) {
        while (arrived.load(std::memory_order_acquire) < n) {
            std::this_thread::yield();
        }
    }
    void WaitRelease() {
        while (!released.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    }
    void Release() { released.store(true, std::memory_order_release); }
};

struct Options {
    std::uint64_t rounds{100};
    std::uint64_t n{2000};
    std::uint64_t lat_n{500};
    std::size_t cap{64};
    int workers{2};
    int watchdog_seconds{10};
    bool legacy_gate_bug{false};
    bool do_latency{true};
    bool stage{true};
    bool gdb{false};
};

/// 一轮：先吞吐轮，再延迟轮；两轮复用同一个 channel/mutex/pc（原始行为）。
bool RunRound(const Options& opt, std::uint64_t round) {
    g_round.store(round, std::memory_order_relaxed);
    ResetSlots();
    g_main_stage.store(0, std::memory_order_relaxed);
    g_phase.store("throughput", std::memory_order_relaxed);

    auto channel = go2cpp::MakeChannel<int>(opt.cap);
    g_channel.store(channel.get(), std::memory_order_release);
    go2cpp::sync::Mutex mutex;
    go2cpp::core::ParkingCondition pc;
    std::mutex pc_mutex;
    std::uint64_t flag = 0;
    std::uint64_t counter = 0;
    std::atomic<std::uint64_t> received{0};
    std::atomic<bool> stop{false};

    const std::uint64_t per =
        static_cast<std::uint64_t>(opt.workers) == 0
            ? 0
            : opt.n / static_cast<std::uint64_t>(opt.workers);
    const std::uint64_t extra =
        static_cast<std::uint64_t>(opt.workers) == 0
            ? 0
            : opt.n % static_cast<std::uint64_t>(opt.workers);

    Gate gate;
    std::vector<std::thread> workers;
    workers.reserve(static_cast<std::size_t>(opt.workers));
    for (int w = 0; w < opt.workers; ++w) {
        const std::uint64_t count =
            per + (static_cast<std::uint64_t>(w) < extra ? 1 : 0);
        workers.emplace_back([&, count] {
            const int slot = AllocSlot("worker");
            for (std::uint64_t i = 0; i < count; ++i) {
                SetStage(slot, 1);
                mutex.Lock();
                ++counter;
                SetStage(slot, 2);
                mutex.Unlock();
                SetStage(slot, 3);
                const auto send = channel->Send(static_cast<int>(i));
                if (send.status != go2cpp::ChannelStatus::kReady) {
                    std::fprintf(stderr, "[error] Send non-ready round=%llu\n",
                                 static_cast<unsigned long long>(round));
                    std::fflush(stderr);
                }
                SetStage(slot, 4);
                {
                    std::lock_guard<std::mutex> lock(pc_mutex);
                    ++flag;
                }
                SetStage(slot, 5);
                pc.notify_one();
                Touch(slot);
            }
            SetStage(slot, 6);
            gate.Arrive();
            gate.WaitRelease();
            SetStage(slot, 9);
        });
    }

    std::thread consumer([&] {
        const int slot = AllocSlot("consumer");
        for (std::uint64_t i = 0; i < opt.n; ++i) {
            SetStage(slot, 1);
            const auto result = channel->Recv();
            if (!result.Ok()) {
                std::fprintf(stderr, "[error] Recv non-ok round=%llu\n",
                             static_cast<unsigned long long>(round));
                std::fflush(stderr);
                break;
            }
            received.fetch_add(1, std::memory_order_relaxed);
            SetStage(slot, 2);
            Touch(slot);
        }
        SetStage(slot, 6);
        gate.Arrive();
        gate.WaitRelease();
        SetStage(slot, 9);
    });

    std::thread helper([&] {
        const int slot = AllocSlot("helper");
        std::uint64_t seen = 0;
        for (;;) {
            SetStage(slot, 1);
            std::unique_lock<std::mutex> lock(pc_mutex);
            pc.wait(lock, [&] { return flag != seen || stop.load(); });
            seen = flag;
            if (stop.load()) {
                break;
            }
            SetStage(slot, 2);
            Touch(slot);
        }
        SetStage(slot, 9);
    });

    // 第一次挂起的根因：helper 不参与 Arrive，却被计入 WaitAll。
    const int expected =
        opt.legacy_gate_bug ? (opt.workers + 2) : (opt.workers + 1);
    g_main_stage.store(1, std::memory_order_relaxed);
    g_gate_wait_start.store(NowNs(), std::memory_order_relaxed);
    g_gate_waiting.store(true, std::memory_order_release);
    const std::uint64_t t0 = NowNs();
    gate.WaitAll(expected);
    const std::uint64_t wall = NowNs() - t0;
    g_gate_waiting.store(false, std::memory_order_release);
    g_main_stage.store(2, std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> lock(pc_mutex);
        stop.store(true);
    }
    pc.notify_all();
    gate.Release();
    g_main_stage.store(3, std::memory_order_relaxed);
    for (auto& worker : workers) {
        worker.join();
    }
    consumer.join();
    helper.join();
    g_main_stage.store(4, std::memory_order_relaxed);

    const std::uint64_t throughput_received =
        received.load(std::memory_order_relaxed);
    if (throughput_received != opt.n || counter != opt.n) {
        std::fprintf(stderr,
                     "[error] round=%llu verify failed received=%llu "
                     "counter=%llu expected=%llu\n",
                     static_cast<unsigned long long>(round),
                     static_cast<unsigned long long>(throughput_received),
                     static_cast<unsigned long long>(counter),
                     static_cast<unsigned long long>(opt.n));
        std::fflush(stderr);
        g_channel.store(nullptr, std::memory_order_release);
        return false;
    }

    // 延迟轮：原始版本复用同一个 channel/mutex/pc，再跑一批更小的样本。
    if (opt.do_latency && opt.lat_n > 0) {
        g_phase.store("latency", std::memory_order_relaxed);
        const std::uint64_t lat_n = std::min(opt.lat_n, opt.n);
        received.store(0, std::memory_order_relaxed);
        std::atomic<bool> lat_stop{false};
        std::uint64_t lat_flag = 0;
        Gate lat_gate;
        std::vector<std::thread> lat_workers;
        const std::uint64_t lat_per =
            static_cast<std::uint64_t>(opt.workers) == 0
                ? 0
                : lat_n / static_cast<std::uint64_t>(opt.workers);
        const std::uint64_t lat_extra =
            static_cast<std::uint64_t>(opt.workers) == 0
                ? 0
                : lat_n % static_cast<std::uint64_t>(opt.workers);
        for (int w = 0; w < opt.workers; ++w) {
            const std::uint64_t count =
                lat_per + (static_cast<std::uint64_t>(w) < lat_extra ? 1 : 0);
            lat_workers.emplace_back([&, count] {
                const int slot = AllocSlot("lat_worker");
                for (std::uint64_t i = 0; i < count; ++i) {
                    SetStage(slot, 3);
                    (void)channel->Send(static_cast<int>(i));
                    SetStage(slot, 4);
                    {
                        std::lock_guard<std::mutex> lock(pc_mutex);
                        ++lat_flag;
                    }
                    SetStage(slot, 5);
                    pc.notify_one();
                    Touch(slot);
                }
                SetStage(slot, 6);
                lat_gate.Arrive();
                lat_gate.WaitRelease();
                SetStage(slot, 9);
            });
        }
        std::thread lat_consumer([&] {
            const int slot = AllocSlot("lat_consumer");
            for (std::uint64_t i = 0; i < lat_n; ++i) {
                SetStage(slot, 1);
                (void)channel->Recv();
                received.fetch_add(1, std::memory_order_relaxed);
                SetStage(slot, 2);
                Touch(slot);
            }
            SetStage(slot, 6);
            lat_gate.Arrive();
            lat_gate.WaitRelease();
            SetStage(slot, 9);
        });
        std::thread lat_helper([&] {
            const int slot = AllocSlot("lat_helper");
            std::uint64_t seen = 0;
            for (;;) {
                SetStage(slot, 1);
                std::unique_lock<std::mutex> lock(pc_mutex);
                pc.wait(lock,
                        [&] { return lat_flag != seen || lat_stop.load(); });
                seen = lat_flag;
                if (lat_stop.load()) {
                    break;
                }
                SetStage(slot, 2);
                Touch(slot);
            }
            SetStage(slot, 9);
        });

        g_main_stage.store(1, std::memory_order_relaxed);
        g_gate_wait_start.store(NowNs(), std::memory_order_relaxed);
        g_gate_waiting.store(true, std::memory_order_release);
        lat_gate.WaitAll(opt.workers + 1);
        g_gate_waiting.store(false, std::memory_order_release);
        g_main_stage.store(2, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(pc_mutex);
            lat_stop.store(true);
        }
        pc.notify_all();
        lat_gate.Release();
        g_main_stage.store(3, std::memory_order_relaxed);
        for (auto& worker : lat_workers) {
            worker.join();
        }
        lat_consumer.join();
        lat_helper.join();
        if (received.load(std::memory_order_relaxed) != lat_n) {
            std::fprintf(stderr,
                         "[error] round=%llu latency verify failed "
                         "received=%llu expected=%llu\n",
                         static_cast<unsigned long long>(round),
                         static_cast<unsigned long long>(
                             received.load(std::memory_order_relaxed)),
                         static_cast<unsigned long long>(lat_n));
            std::fflush(stderr);
            g_channel.store(nullptr, std::memory_order_release);
            return false;
        }
        std::fprintf(stdout,
                     "[round %llu] n=%llu cap=%zu workers=%d "
                     "throughput=%.3fms latency=%.3fms ok\n",
                     static_cast<unsigned long long>(round),
                     static_cast<unsigned long long>(opt.n), opt.cap,
                     opt.workers, static_cast<double>(wall) / 1e6,
                     static_cast<double>(lat_n));
    } else {
        std::fprintf(stdout,
                     "[round %llu] n=%llu cap=%zu workers=%d "
                     "throughput=%.3fms ok\n",
                     static_cast<unsigned long long>(round),
                     static_cast<unsigned long long>(opt.n), opt.cap,
                     opt.workers, static_cast<double>(wall) / 1e6);
    }
    std::fflush(stdout);
    g_channel.store(nullptr, std::memory_order_release);
    return true;
}

bool ParseU64(const char* text, std::uint64_t* out) {
    if (text == nullptr || *text == '\0') {
        return false;
    }
    char* end = nullptr;
    const unsigned long long value = std::strtoull(text, &end, 10);
    if (end == text || *end != '\0') {
        return false;
    }
    *out = static_cast<std::uint64_t>(value);
    return true;
}

void PrintUsage(const char* program) {
    std::fprintf(
        stderr,
        "usage: %s [--rounds=100] [--n=2000] [--lat-n=500] [--cap=64] "
        "[--workers=2] [--watchdog-sec=10] [--legacy-gate-bug] "
        "[--no-latency] [--no-stage] [--gdb]\n",
        program);
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        const auto value = [&](const char* name) -> const char* {
            const std::size_t length = std::strlen(name);
            if (std::strncmp(arg, name, length) == 0 && arg[length] == '=') {
                return arg + length + 1;
            }
            return nullptr;
        };
        if (const char* v = value("--rounds")) {
            if (!ParseU64(v, &opt.rounds)) {
                PrintUsage(argv[0]);
                return 2;
            }
        } else if (const char* v = value("--n")) {
            if (!ParseU64(v, &opt.n)) {
                PrintUsage(argv[0]);
                return 2;
            }
        } else if (const char* v = value("--lat-n")) {
            if (!ParseU64(v, &opt.lat_n)) {
                PrintUsage(argv[0]);
                return 2;
            }
        } else if (const char* v = value("--cap")) {
            std::uint64_t cap = 64;
            if (!ParseU64(v, &cap)) {
                PrintUsage(argv[0]);
                return 2;
            }
            opt.cap = static_cast<std::size_t>(cap);
        } else if (const char* v = value("--workers")) {
            std::uint64_t workers = 2;
            if (!ParseU64(v, &workers) || workers == 0 || workers > 8) {
                PrintUsage(argv[0]);
                return 2;
            }
            opt.workers = static_cast<int>(workers);
        } else if (const char* v = value("--watchdog-sec")) {
            std::uint64_t seconds = 10;
            if (!ParseU64(v, &seconds)) {
                PrintUsage(argv[0]);
                return 2;
            }
            opt.watchdog_seconds = static_cast<int>(seconds);
        } else if (std::strcmp(arg, "--legacy-gate-bug") == 0) {
            opt.legacy_gate_bug = true;
        } else if (std::strcmp(arg, "--no-latency") == 0) {
            opt.do_latency = false;
        } else if (std::strcmp(arg, "--no-stage") == 0) {
            opt.stage = false;
        } else if (std::strcmp(arg, "--gdb") == 0) {
            opt.gdb = true;
        } else if (std::strcmp(arg, "--help") == 0 ||
                   std::strcmp(arg, "-h") == 0) {
            PrintUsage(argv[0]);
            return 0;
        } else {
            std::fprintf(stderr, "unknown arg: %s\n", arg);
            PrintUsage(argv[0]);
            return 2;
        }
    }

    // 允许 watchdog 里的 gdb 子进程 attach 本进程（ptrace_scope=1 时必要）。
    (void)::prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
    g_stage_enabled.store(opt.stage, std::memory_order_relaxed);
    g_watchdog_seconds.store(opt.watchdog_seconds,
                             std::memory_order_relaxed);
    g_gdb_enabled.store(opt.gdb, std::memory_order_relaxed);

    std::fprintf(stdout,
                 "[start] rounds=%llu n=%llu lat_n=%llu cap=%zu workers=%d "
                 "legacy_gate_bug=%d stage=%d\n",
                 static_cast<unsigned long long>(opt.rounds),
                 static_cast<unsigned long long>(opt.n),
                 static_cast<unsigned long long>(opt.lat_n), opt.cap,
                 opt.workers, opt.legacy_gate_bug ? 1 : 0,
                 opt.stage ? 1 : 0);
    std::fflush(stdout);

    std::thread watchdog(WatchdogLoop);
    bool ok = true;
    for (std::uint64_t round = 1; round <= opt.rounds; ++round) {
        if (!RunRound(opt, round)) {
            ok = false;
            break;
        }
    }
    g_watchdog_stop.store(true, std::memory_order_relaxed);
    watchdog.join();
    std::fprintf(stdout, "[done] ok=%d\n", ok ? 1 : 0);
    std::fflush(stdout);
    return ok ? 0 : 1;
}
