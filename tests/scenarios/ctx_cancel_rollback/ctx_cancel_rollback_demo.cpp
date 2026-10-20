/**
 * @file ctx_cancel_rollback_demo.cpp
 * @brief 场景：锁顺序死锁 + Doctor `ctx.cancel` + ContextRollback undo 解死锁。
 *
 * ## 场景结构（与 deadlock_routes 的区别）
 *
 * deadlock_routes 演示「owner 已死 → mutex-unlock 人工恢复」；本场景演示
 * **有主的锁**如何被安全解开：
 *
 * ```text
 *   线程 A(Fiber):  Lock(L1) ── 业务 ── Lock(L2, ctxA)   阻塞
 *   线程 B(Fiber):  Lock(L2) ── 业务 ── Lock(L1, ctxB)   阻塞
 *   主线程/Doctor:  deadlock-report → ctx.cancel <gidA>
 * ```
 *
 * 两把锁的持有都通过 `ContextRollback::RecordUndo` 绑定补偿动作：
 * - A 的 ctx 就是 `ContextRollback::context()`（`rollback_on_cancel=true`）；
 * - Doctor 只调用 `ctx->Cancel()`，**不调用任何 Unlock()**；
 * - `DoneSignal` 的内部回调在取消线程上同步执行 A 的 undo → `L1.Unlock()`；
 * - A 的 `Lock(L2, ctx)` 同时被取消，返回 false，`ctx->Err()` 非空；
 * - B 的 `Lock(L1)` 随即拿到 L1，完成业务并释放两把锁。
 *
 * ## 为什么这条路径没有 stale-unlock
 *
 * 强制解锁（`mutex-unlock --force`）绕过了 owner：被夺锁的 G 不知道自己
 * 已经失去临界区，之后可能继续写共享状态或重复 Unlock。本场景的
 * `--mode=force` 复现该缺陷：A 拿到 L2 后才发现 L1 已不归自己（stale），
 * 只能跳过 Unlock 并丢弃 undo —— 这正是 `ctx.cancel` 路径要避免的。
 *
 * ## 三种模式
 *
 * - `--mode=cancel`（默认）：进程内主线程扮演 Doctor 客户端，执行
 *   `deadlock-report` 与 `ctx.cancel gid=<A>`；验证 A 回滚 + B 完成。
 * - `--mode=external`：启动 Doctor socket 后停在 `WAITING_EXTERNAL`，由
 *   `run_ctx_cancel_rollback.sh` 用真实 CLI/客户端发命令。
 * - `--mode=force`：对照组，执行 `mutex-unlock name=L1 --force reason=...`，
 *   观测 stale-unlock（undo 被丢弃、A 跳过释放）。
 *
 * ## 输出约定
 *
 * stdout 是机器可读阶段行（READY / DEADLOCK_READY / DEADLOCK_REPORT /
 * ACTION / RESULT / FAIL）；stderr 是人体可读细节（含 `[undo]` 审计）。
 * 退出码 0 = 全部验收成立；2 = 没构造出死锁窗口；4 = 验收断言失败。
 */

#include "go2cpp/concurrency/core/parking_condition.hpp"
#include "go2cpp/context.hpp"
#include "go2cpp/debug/debug_dump.hpp"
#include "go2cpp/debug/diagnostic_agent.hpp"
#include "go2cpp/scheduler.hpp"
#include "go2cpp/sync.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include <unistd.h>

namespace {

using namespace go2cpp;
using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// 两把锁 + 观测计数
// ---------------------------------------------------------------------------

sync::Mutex g_l1;
sync::Mutex g_l2;

std::atomic<std::uint64_t> g_gid_a{0};
std::atomic<std::uint64_t> g_gid_b{0};

/// 成功 Lock 的次数（含 undo/正常路径）；用于「无泄漏」的收支平衡。
std::atomic<int> g_locks_acquired{0};
/// 本场景代码主动 Unlock 的次数（undo 里的也算）。
std::atomic<int> g_locks_released{0};
/// Doctor `mutex-unlock --force` 释放的次数（demo 代码之外）。
std::atomic<int> g_doctor_forced_unlocks{0};
/// undo 实际执行次数。
std::atomic<int> g_undo_executed{0};
/// A/B 各自的业务写入次数（用于确认「谁完成了什么」）。
std::atomic<int> g_a_business{0};
std::atomic<int> g_b_business{0};
/// B 完成时写入的共享结果（期望恰为 1）。
std::atomic<int> g_shared_work{0};

std::atomic<bool> g_a_lock2_ok{false};
std::atomic<bool> g_a_lock2_false{false};
std::atomic<bool> g_a_ctx_err{false};
std::atomic<int> g_a_rollback_status{0};
std::atomic<std::uint64_t> g_a_undo_pending{0};
std::atomic<int> g_a_rollback_failures{0};
std::atomic<bool> g_a_completed{false};
std::atomic<bool> g_b_completed{false};
/// A 在 Lock(L2) 成功后是否仍合法持有 L1（force 模式的 stale 观测）。
std::atomic<bool> g_a_still_owns_l1{false};
std::atomic<int> g_stale_owner{0};
/// 取消与 Unlock 唤醒竞争：Lock 返回 true 但 ctx.Err() 已经非空。
std::atomic<int> g_cancel_race{0};

// ---------------------------------------------------------------------------
// 会合：双方各持第一把锁后才去拿第二把（保证确定性死锁）
// ---------------------------------------------------------------------------

std::mutex g_rendezvous_mutex;
core::ParkingCondition g_rendezvous;
std::atomic<int> g_first_locked{0};

std::uint64_t CurrentGid() {
    Task* task = Scheduler::current_task_ptr();
    return task == nullptr ? 0U : static_cast<std::uint64_t>(task->id());
}

bool RendezvousFirstLocks() {
    std::unique_lock<std::mutex> lock(g_rendezvous_mutex);
    const int arrived = g_first_locked.fetch_add(1) + 1;
    g_rendezvous.notify_all();
    if (arrived >= 2) {
        return true;
    }
    return g_rendezvous.wait_for(lock, 2000ms, [] {
        return g_first_locked.load(std::memory_order_acquire) >= 2;
    });
}

std::size_t ThreadHash() {
    return std::hash<std::thread::id>{}(std::this_thread::get_id());
}

// ---------------------------------------------------------------------------
// Worker A：Lock(L1) → 业务 → Lock(L2, ctx)（会被 Doctor 取消）
// ---------------------------------------------------------------------------

void WorkerA() {
    const std::uint64_t gid = CurrentGid();
    g_gid_a.store(gid, std::memory_order_release);
    ContextRollback rollback(Background(), /*rollback_on_cancel=*/true);
    const ContextPtr ctx = rollback.context();
    auto& doctor = debug::DiagnosticAgent::Instance();
    const bool registered = doctor.RegisterContext("worker-A", ctx, gid);
    std::fprintf(stderr,
                 "[A] gid=%llu ctx=%p registered=%d status=%s\n",
                 static_cast<unsigned long long>(gid),
                 static_cast<const void*>(ctx.get()), registered ? 1 : 0,
                 rollback.status_name());

    if (!g_l1.Lock()) {
        std::fprintf(stderr, "[A] Lock(L1) failed\n");
        return;
    }
    g_locks_acquired.fetch_add(1, std::memory_order_relaxed);
    const bool undo_recorded = rollback.RecordUndo([gid] {
        g_l1.Unlock();
        g_locks_released.fetch_add(1, std::memory_order_relaxed);
        g_undo_executed.fetch_add(1, std::memory_order_relaxed);
        std::fprintf(stderr,
                     "[undo] A: L1 released by ContextRollback undo "
                     "(cancel thread=%zu gid=%llu)\n",
                     ThreadHash(), static_cast<unsigned long long>(gid));
    });
    if (!undo_recorded) {
        std::fprintf(stderr, "[A] RecordUndo failed\n");
    }
    g_a_business.fetch_add(1, std::memory_order_relaxed);
    std::fprintf(stderr, "[A] holds L1, business done, waiting for B ...\n");

    const bool rendezvous = RendezvousFirstLocks();
    std::fprintf(stderr,
                 "[A] rendezvous=%d; Lock(L2, ctx) -> blocking\n",
                 rendezvous ? 1 : 0);

    // 关键：等待可被 ctx 取消。返回 false 表示取消/超时，此时**不得**再
    // 认为 L1 仍归自己；回滚（undo）负责释放。
    const bool locked = g_l2.Lock(ctx);
    if (locked) {
        g_locks_acquired.fetch_add(1, std::memory_order_relaxed);
    }
    const ErrorPtr error = ctx->Err();
    // 两条取消路径都要处理：
    //   (a) Lock 返回 false：等待被取消；
    //   (b) Lock 返回 true 但 ctx.Err() 非空：取消与 Unlock 唤醒竞争 ——
    //       本 G 在取消线性化之后仍拿到了锁。Go 的约定是「操作成功不豁免
    //       取消检查」，调用方必须立刻释放并放弃这次临界区。
    if (!locked || error != nullptr) {
        if (locked) {
            g_cancel_race.store(1, std::memory_order_release);
            g_a_lock2_ok.store(true, std::memory_order_release);
            g_l2.Unlock();
            g_locks_released.fetch_add(1, std::memory_order_relaxed);
            std::fprintf(stderr,
                         "[A] cancel-vs-unlock race: Lock(L2)=true but "
                         "ctx.Err set; release L2 and roll back\n");
        }
        g_a_lock2_false.store(true, std::memory_order_release);
        g_a_ctx_err.store(error != nullptr, std::memory_order_release);
        // 自动回滚（取消线程上的 undo）可能已经完成；Rollback() 幂等。
        const bool rolled = rollback.Rollback();
        g_a_rollback_status.store(static_cast<int>(rollback.status()),
                                  std::memory_order_release);
        g_a_undo_pending.store(rollback.pending_undo_count(),
                               std::memory_order_release);
        g_a_rollback_failures.store(
            static_cast<int>(rollback.failures().size()),
            std::memory_order_release);
        std::fprintf(stderr,
                     "[A] Lock(L2)=false ctx.Err=%d(%s) rollback=%d "
                     "status=%s pending_undo=%zu failures=%zu\n",
                     error != nullptr ? 1 : 0,
                     error != nullptr ? error->Message().c_str() : "none",
                     rolled ? 1 : 0, rollback.status_name(),
                     rollback.pending_undo_count(),
                     rollback.failures().size());
        (void)doctor.UnregisterContext("worker-A");
        g_a_completed.store(true, std::memory_order_release);
        return;
    }

    // 只有 force 对照组会走到这里：A 拿到了 L2，但 L1 可能已被强制夺走。
    g_a_lock2_ok.store(true, std::memory_order_release);
    const auto snapshot = g_l1.DebugSnapshotInternal();
    const bool still_owns =
        snapshot.held &&
        (!snapshot.owner_known || snapshot.owner_gid == gid);
    g_a_still_owns_l1.store(still_owns, std::memory_order_release);
    if (!still_owns) {
        // 强制解锁把 L1 交给了别人：绝不能 Unlock（会抢走新持有者的锁），
        // undo 也必须丢弃。业务已经继续执行 —— 这正是 stale-unlock 的危害。
        g_stale_owner.store(1, std::memory_order_release);
        (void)rollback.Commit();
        g_a_business.fetch_add(10, std::memory_order_relaxed);
        std::fprintf(stderr,
                     "[A] STALE: L1 no longer owned (held=%d owner_gid=%llu); "
                     "skip Unlock + drop undo (force-unlock path)\n",
                     snapshot.held ? 1 : 0,
                     static_cast<unsigned long long>(snapshot.owner_gid));
    } else {
        (void)rollback.Commit();
        g_l1.Unlock();
        g_locks_released.fetch_add(1, std::memory_order_relaxed);
        g_a_business.fetch_add(1, std::memory_order_relaxed);
    }
    g_l2.Unlock();
    g_locks_released.fetch_add(1, std::memory_order_relaxed);
    g_a_rollback_status.store(static_cast<int>(rollback.status()),
                              std::memory_order_release);
    g_a_undo_pending.store(rollback.pending_undo_count(),
                           std::memory_order_release);
    (void)doctor.UnregisterContext("worker-A");
    g_a_completed.store(true, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// Worker B：Lock(L2) → 业务 → Lock(L1, ctx)（在 A 回滚后成功）
// ---------------------------------------------------------------------------

void WorkerB() {
    const std::uint64_t gid = CurrentGid();
    g_gid_b.store(gid, std::memory_order_release);
    ContextRollback rollback(Background(), /*rollback_on_cancel=*/true);
    const ContextPtr ctx = rollback.context();
    std::fprintf(stderr, "[B] gid=%llu ctx=%p\n",
                 static_cast<unsigned long long>(gid),
                 static_cast<const void*>(ctx.get()));

    if (!g_l2.Lock()) {
        std::fprintf(stderr, "[B] Lock(L2) failed\n");
        return;
    }
    g_locks_acquired.fetch_add(1, std::memory_order_relaxed);
    const bool undo_recorded = rollback.RecordUndo([gid] {
        g_l2.Unlock();
        g_locks_released.fetch_add(1, std::memory_order_relaxed);
        g_undo_executed.fetch_add(1, std::memory_order_relaxed);
        std::fprintf(stderr, "[undo] B: L2 released by ContextRollback undo "
                             "(thread=%zu gid=%llu)\n",
                     ThreadHash(), static_cast<unsigned long long>(gid));
    });
    if (!undo_recorded) {
        std::fprintf(stderr, "[B] RecordUndo failed\n");
    }
    g_b_business.fetch_add(1, std::memory_order_relaxed);

    const bool rendezvous = RendezvousFirstLocks();
    std::fprintf(stderr,
                 "[B] holds L2, rendezvous=%d; Lock(L1, ctx) -> blocking\n",
                 rendezvous ? 1 : 0);

    const bool locked = g_l1.Lock(ctx);
    if (!locked) {
        const ErrorPtr error = ctx->Err();
        (void)rollback.Rollback();
        std::fprintf(stderr,
                     "[B] Lock(L1)=false ctx.Err=%d rollback status=%s\n",
                     error != nullptr ? 1 : 0, rollback.status_name());
        g_b_completed.store(true, std::memory_order_release);
        return;
    }
    g_locks_acquired.fetch_add(1, std::memory_order_relaxed);
    // B 完成业务：写入共享结果（期望值 1）。
    g_shared_work.fetch_add(1, std::memory_order_relaxed);
    g_b_business.fetch_add(1, std::memory_order_relaxed);
    // 成功路径提交：undo 被丢弃，由本 G 自己按 LIFO 释放。
    const bool committed = rollback.Commit();
    std::fprintf(stderr,
                 "[B] COMPLETED: acquired L1 after the peer released it "
                 "(commit=%d undo_executed=%d)\n",
                 committed ? 1 : 0,
                 g_undo_executed.load(std::memory_order_relaxed));
    g_b_completed.store(true, std::memory_order_release);
    g_l1.Unlock();
    g_locks_released.fetch_add(1, std::memory_order_relaxed);
    g_l2.Unlock();
    g_locks_released.fetch_add(1, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// 输出工具
// ---------------------------------------------------------------------------

std::string SnapshotText(const sync::MutexDebugSnapshot& snapshot) {
    char buffer[256];
    std::snprintf(buffer, sizeof(buffer),
                  "held=%d waiters=%zu native_waiters=%zu fiber_waiters=%zu "
                  "owner_gid=%llu owner_known=%d",
                  snapshot.held ? 1 : 0, snapshot.waiters,
                  snapshot.native_waiters, snapshot.fiber_waiters,
                  static_cast<unsigned long long>(snapshot.owner_gid),
                  snapshot.owner_known ? 1 : 0);
    return std::string(buffer);
}

/// 从 `deadlock-report` 文本里取 `verdict=` 的值。
std::string ExtractVerdict(const std::string& payload) {
    const std::string needle = "verdict=";
    const std::size_t position = payload.find(needle);
    if (position == std::string::npos) {
        return "unknown";
    }
    const std::size_t begin = position + needle.size();
    std::size_t end = begin;
    while (end < payload.size() && payload[end] != '\n' &&
           payload[end] != '\r') {
        ++end;
    }
    return payload.substr(begin, end - begin);
}

void PrintIndented(const std::string& payload, const char* tag) {
    std::fprintf(stderr, "---- %s ----\n%s\n---- /%s ----\n", tag,
                 payload.c_str(), tag);
}

struct Config {
    std::string mode{"cancel"};
    std::string socket_path;
    int wait_ms{20000};
};

}  // namespace

// ---------------------------------------------------------------------------
// main：编排（检测死锁 → 命令 → 验证）
// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    Config config;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        std::string inline_value;
        const std::size_t equals = arg.find('=');
        if (equals != std::string::npos) {
            inline_value = arg.substr(equals + 1);
            arg = arg.substr(0, equals);
        }
        const auto next = [&]() -> std::string {
            return i + 1 < argc ? argv[++i] : std::string{};
        };
        const auto value = [&]() -> std::string {
            return inline_value.empty() ? next() : inline_value;
        };
        if (arg == "--mode") {
            config.mode = value();
        } else if (arg == "--socket") {
            config.socket_path = value();
        } else if (arg == "--wait-ms") {
            config.wait_ms = std::atoi(value().c_str());
        } else if (arg == "--help" || arg == "-h") {
            std::printf(
                "usage: %s [--mode=cancel|external|force] [--socket PATH] "
                "[--wait-ms N]\n",
                argv[0]);
            return 0;
        } else {
            std::fprintf(stderr, "[demo] unknown argument: %s\n", arg.c_str());
            return 2;
        }
    }
    if (config.mode != "cancel" && config.mode != "external" &&
        config.mode != "force") {
        std::fprintf(stderr, "[demo] bad --mode: %s\n", config.mode.c_str());
        return 2;
    }

    // 让 Mutex 记录近似 owner（gid），死锁窗口检测与 stale 判定都依赖它。
    sync::SetHeldLockParkDetection(true);

    SchedulerConfig scheduler_config;
    scheduler_config.processor_count = 2;
    scheduler_config.min_workers = 2;
    scheduler_config.max_workers = 4;
    scheduler_config.collect_metrics = true;
    Scheduler scheduler(scheduler_config);
    scheduler.start();

    debug::DiagnosticAgent& doctor = debug::DiagnosticAgent::Instance();
    if (!doctor.RegisterScheduler("scheduler", scheduler) ||
        !doctor.RegisterMutex("L1", g_l1) ||
        !doctor.RegisterMutex("L2", g_l2)) {
        std::fprintf(stderr, "[demo] doctor registration failed\n");
        return 1;
    }
    debug::DiagnosticAgentConfig doctor_config;
    doctor_config.socket_path = config.socket_path;
    doctor_config.recv_timeout_ms = 20;
    doctor_config.detect_interval = std::chrono::milliseconds(50);
    doctor_config.deadlock_threshold = std::chrono::milliseconds(300);
    doctor_config.auto_force_wake = false;
    if (!doctor.Start(doctor_config)) {
        std::fprintf(stderr, "[demo] doctor start failed: %s\n",
                     doctor.last_error().c_str());
        return 1;
    }

    std::printf("READY pid=%d mode=%s doctor_socket=%s\n",
                static_cast<int>(::getpid()), config.mode.c_str(),
                doctor.socket_path().c_str());
    std::fflush(stdout);

    auto task_a = scheduler.spawn([] { WorkerA(); });
    auto task_b = scheduler.spawn([] { WorkerB(); });
    if (!task_a || !task_b) {
        std::printf("FAIL spawn task_a=%d task_b=%d\n", task_a ? 1 : 0,
                    task_b ? 1 : 0);
        return 1;
    }

    // ---- 阶段 1：等待确定性死锁窗口（各持一把 + 各等一把）----
    bool deadlock_window = false;
    const auto window_deadline =
        std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < window_deadline) {
        const auto l1 = g_l1.DebugSnapshotInternal();
        const auto l2 = g_l2.DebugSnapshotInternal();
        const std::uint64_t gid_a = g_gid_a.load(std::memory_order_acquire);
        const std::uint64_t gid_b = g_gid_b.load(std::memory_order_acquire);
        if (gid_a != 0U && gid_b != 0U && l1.held && l2.held &&
            l1.waiters >= 1U && l2.waiters >= 1U &&
            l1.owner_gid == gid_a && l2.owner_gid == gid_b) {
            deadlock_window = true;
            break;
        }
        std::this_thread::sleep_for(2ms);
    }
    const std::uint64_t gid_a = g_gid_a.load(std::memory_order_acquire);
    const std::uint64_t gid_b = g_gid_b.load(std::memory_order_acquire);
    if (!deadlock_window) {
        std::printf(
            "FAIL deadlock-window-not-reached gid_a=%llu gid_b=%llu "
            "L1=[%s] L2=[%s]\n",
            static_cast<unsigned long long>(gid_a),
            static_cast<unsigned long long>(gid_b),
            SnapshotText(g_l1.DebugSnapshotInternal()).c_str(),
            SnapshotText(g_l2.DebugSnapshotInternal()).c_str());
        std::fflush(stdout);
        doctor.Stop();
        doctor.ResetRegistrations();
        scheduler.shutdown();
        return 2;
    }
    std::printf(
        "DEADLOCK_READY gid_a=%llu gid_b=%llu L1.owner=%llu L2.owner=%llu "
        "L1.waiters=%zu L2.waiters=%zu\n",
        static_cast<unsigned long long>(gid_a),
        static_cast<unsigned long long>(gid_b),
        static_cast<unsigned long long>(
            g_l1.DebugSnapshotInternal().owner_gid),
        static_cast<unsigned long long>(
            g_l2.DebugSnapshotInternal().owner_gid),
        g_l1.DebugSnapshotInternal().waiters,
        g_l2.DebugSnapshotInternal().waiters);
    std::fflush(stdout);

    // ---- 阶段 2：外部模式等待 CLI；内部模式自己发 Doctor 命令 ----
    if (config.mode == "external") {
        std::printf("WAITING_EXTERNAL gid_a=%llu gid_b=%llu\n",
                    static_cast<unsigned long long>(gid_a),
                    static_cast<unsigned long long>(gid_b));
        std::fflush(stdout);
        const auto external_deadline =
            std::chrono::steady_clock::now() +
            std::chrono::milliseconds(config.wait_ms);
        while (std::chrono::steady_clock::now() < external_deadline) {
            if (g_a_completed.load(std::memory_order_acquire) ||
                g_a_lock2_false.load(std::memory_order_acquire)) {
                break;
            }
            std::this_thread::sleep_for(5ms);
        }
    } else {
        // 等待 Doctor 的全局无进展检测越过阈值（deadlock-report 的 verdict）。
        std::string verdict = "unknown";
        std::string report;
        const auto report_deadline =
            std::chrono::steady_clock::now() + 5s;
        while (std::chrono::steady_clock::now() < report_deadline) {
            const auto result = doctor.Execute("deadlock-report");
            if (result.ok) {
                report = result.payload;
                verdict = ExtractVerdict(report);
                if (verdict == "deadlock") {
                    break;
                }
            }
            std::this_thread::sleep_for(50ms);
        }
        PrintIndented(report, "deadlock-report");
        std::printf("DEADLOCK_REPORT verdict=%s\n", verdict.c_str());
        std::fflush(stdout);

        if (config.mode == "cancel") {
            const std::string command =
                "ctx.cancel gid=" + std::to_string(gid_a);
            const auto result = doctor.Execute(command);
            PrintIndented(result.payload, "ctx.cancel");
            std::printf("ACTION command=%s ok=%d\n", command.c_str(),
                        result.ok ? 1 : 0);
            std::fflush(stdout);
        } else {
            const auto result = doctor.Execute(
                "mutex-unlock name=L1 --force reason=contrast-force-path");
            if (result.ok) {
                g_doctor_forced_unlocks.fetch_add(1, std::memory_order_relaxed);
            }
            PrintIndented(result.payload, "mutex-unlock --force");
            std::printf("ACTION command=mutex-unlock --force ok=%d\n",
                        result.ok ? 1 : 0);
            std::fflush(stdout);
        }
    }

    // ---- 阶段 3：等待 A/B 结束 ----
    const auto finish_deadline =
        std::chrono::steady_clock::now() +
        std::chrono::milliseconds(config.wait_ms);
    bool a_done = false;
    bool b_done = false;
    while (std::chrono::steady_clock::now() < finish_deadline) {
        a_done = a_done ||
                 task_a->state() == GState::kDead ||
                 task_a->state() == GState::kCancelled ||
                 task_a->state() == GState::kFailed;
        b_done = b_done ||
                 task_b->state() == GState::kDead ||
                 task_b->state() == GState::kCancelled ||
                 task_b->state() == GState::kFailed;
        if (a_done && b_done) {
            break;
        }
        std::this_thread::sleep_for(5ms);
    }
    // 二次确认（阻塞等待，防止状态跃迁的瞬间误判）。
    if (a_done) {
        a_done = task_a->wait_for(1s);
    }
    if (b_done) {
        b_done = task_b->wait_for(1s);
    }

    // ---- 阶段 4：无死锁残留 + 无泄漏验证 ----
    std::string final_verdict = "unknown";
    {
        const auto report_deadline =
            std::chrono::steady_clock::now() + 2s;
        while (std::chrono::steady_clock::now() < report_deadline) {
            const auto result = doctor.Execute("deadlock-report");
            if (result.ok) {
                final_verdict = ExtractVerdict(result.payload);
                if (final_verdict != "deadlock") {
                    break;
                }
            }
            std::this_thread::sleep_for(50ms);
        }
    }
    const auto l1_final = g_l1.DebugSnapshotInternal();
    const auto l2_final = g_l2.DebugSnapshotInternal();
    const int acquired = g_locks_acquired.load(std::memory_order_acquire);
    const int released = g_locks_released.load(std::memory_order_acquire);
    const int force_unlocks =
        g_doctor_forced_unlocks.load(std::memory_order_acquire);
    const int undo_executed =
        g_undo_executed.load(std::memory_order_acquire);
    const bool balanced = acquired == released + force_unlocks;
    // 权威队列长度是 `waiters`（等待队列 vector 的 size）。`native_waiters`
    // 只是原生等待者的派生计数器：取消与 Unlock 在极窄窗口内竞争时，
    // sync.cpp 的既有实现可能把它减成 SIZE_MAX（本任务禁止改 sync.cpp，
    // 见报告「已知缺陷」一节）。这里把下溢单独报告，不当作锁泄漏。
    const bool native_underflow =
        l1_final.native_waiters > 1024U || l2_final.native_waiters > 1024U;
    if (native_underflow) {
        std::fprintf(stderr,
                     "[warn] native_waiters underflow observed "
                     "(L1=%zu L2=%zu)；waiters 权威计数仍为 L1=%zu L2=%zu\n",
                     l1_final.native_waiters, l2_final.native_waiters,
                     l1_final.waiters, l2_final.waiters);
    }
    const bool locks_clean =
        !l1_final.held && l1_final.waiters == 0U && !l2_final.held &&
        l2_final.waiters == 0U;
    const bool no_deadlock_left = final_verdict != "deadlock";

    const bool cancel_ok =
        g_a_lock2_false.load(std::memory_order_acquire) &&
        g_a_ctx_err.load(std::memory_order_acquire) &&
        g_a_rollback_status.load(std::memory_order_acquire) ==
            static_cast<int>(ContextRollback::Status::kRolledBack) &&
        g_a_undo_pending.load(std::memory_order_acquire) == 0U &&
        g_a_rollback_failures.load(std::memory_order_acquire) == 0 &&
        undo_executed == 1 && g_stale_owner.load() == 0 &&
        g_shared_work.load(std::memory_order_acquire) == 1 &&
        g_b_business.load(std::memory_order_acquire) == 2;
    const bool force_ok =
        g_a_lock2_ok.load(std::memory_order_acquire) &&
        undo_executed == 0 && g_stale_owner.load() == 1 &&
        !g_a_still_owns_l1.load(std::memory_order_acquire) &&
        g_shared_work.load(std::memory_order_acquire) == 1;
    const bool mode_ok = config.mode == "force" ? force_ok : cancel_ok;
    const bool pass = a_done && b_done && mode_ok && balanced && locks_clean &&
                      no_deadlock_left &&
                      g_a_completed.load(std::memory_order_acquire) &&
                      g_b_completed.load(std::memory_order_acquire);

    std::printf(
        "RESULT mode=%s pass=%d a_completed=%d b_completed=%d "
        "a_lock2_ok=%d a_lock2_false=%d ctx_err=%d undo_executed=%d "
        "rollback_status=%s pending_undo=%llu rollback_failures=%d "
        "stale=%d cancel_race=%d a_still_owns_l1=%d "
        "locks_acquired=%d locks_released=%d "
        "force_unlocks=%d balanced=%d l1_held=%d l1_waiters=%zu "
        "l1_native_waiters=%zu l2_held=%d l2_waiters=%zu "
        "l2_native_waiters=%zu native_underflow=%d shared_work=%d "
        "final_verdict=%s\n",
        config.mode.c_str(), pass ? 1 : 0,
        g_a_completed.load() ? 1 : 0, g_b_completed.load() ? 1 : 0,
        g_a_lock2_ok.load() ? 1 : 0, g_a_lock2_false.load() ? 1 : 0,
        g_a_ctx_err.load() ? 1 : 0, undo_executed,
        ContextRollback::StatusName(static_cast<ContextRollback::Status>(
            g_a_rollback_status.load())),
        static_cast<unsigned long long>(g_a_undo_pending.load()),
        g_a_rollback_failures.load(), g_stale_owner.load(),
        g_cancel_race.load(std::memory_order_acquire),
        g_a_still_owns_l1.load() ? 1 : 0, acquired, released, force_unlocks,
        balanced ? 1 : 0, l1_final.held ? 1 : 0, l1_final.waiters,
        l1_final.native_waiters, l2_final.held ? 1 : 0, l2_final.waiters,
        l2_final.native_waiters, native_underflow ? 1 : 0,
        g_shared_work.load(std::memory_order_acquire),
        final_verdict.c_str());
    std::fflush(stdout);

    doctor.Stop();
    doctor.ResetRegistrations();
    scheduler.shutdown();
    return pass ? 0 : 4;
}
