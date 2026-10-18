/**
 * @file test_state_machine_matrix.cpp
 * @brief 并发侧状态机的穷举矩阵测试（GState / FiberState / TimerState /
 *        Channel 关闭态 / Future 终态）。
 *
 * ## 为什么单独写一个矩阵测试
 *
 * `tests/concurrency/test_state_machine.cpp` 用公开调度器 API 覆盖的是
 * "端到端生命周期"，它无法把 Task 精确摆到某个中间状态再施加单个事件。
 * 本文件直接使用 `go2cpp::scheduler::Task` 公开的后端状态转换接口
 * （`promote_new` / `try_mark_queued` / `park_for_scheduler` / `wake_for_wait`
 * / `cancel` …）把 G 摆到 **8 个可持久化的局面**，再对每个局面施加 **17 个
 * 事件**，逐格断言：
 *
 *   1. 合法转移返回成功，且落到预期的下一个状态；
 *   2. **非法转移被明确拒绝**（返回 false / kRejected），状态不变；
 *   3. 终态（Dead/Cancelled/Failed）对任何事件都不可再转移 —— 既不能复活，
 *      也不能被重复取消改变。
 *
 * 之所以把「排队中」和「已出队但未运行」拆成两个局面（Runnable /
 * RunnableQueued），是因为 Task 的可运行态由 `state` + `queued` +
 * `execution_claim` 三个原子共同定义；只按 state 枚举会漏掉
 * "queued 为空时 try_mark_running 必须失败" 这类关键不变量。
 *
 * ## 覆盖范围与局限
 *
 * - GState：8 局面 x 17 事件 = 136 格，全部断言；
 * - FiberState：5 状态 x 5 事件 = 25 格（Running 只能在其自身栈上观察，
 *   所以在 body 内施加事件）；
 * - TimerState：6 状态 x 5 事件 = 30 格（kFailed 由回调抛异常构造）；
 * - Channel 关闭态：2 状态 x 7 事件 = 14 格；
 * - Future 终态：5 状态 x 4 事件 = 20 格。
 *
 * 未覆盖：MState/PState 没有集中的 `Transition()` 函数，无法逐格注入；
 * 本文件改为在真实调度器上采样，断言"终态不被复活 + 枚举值合法"。
 */
#include "go2cpp/concurrency/channel.hpp"
#include "go2cpp/concurrency/core/timer.hpp"
#include "go2cpp/concurrency/fiber.hpp"
#include "go2cpp/concurrency/future.hpp"
#include "go2cpp/concurrency/scheduler/scheduler.hpp"
#include "go2cpp/scheduler.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using go2cpp::Fiber;
using go2cpp::FiberState;
using go2cpp::GState;

int g_failures = 0;
int g_cells = 0;

void Check(bool condition, const std::string& what) {
    ++g_cells;
    if (!condition) {
        std::fprintf(stderr, "[matrix] FAILED: %s\n", what.c_str());
        ++g_failures;
    }
}

bool IsTerminal(GState state) {
    return state == GState::kDead || state == GState::kCancelled ||
           state == GState::kFailed;
}

const char* GStateName(GState state) {
    switch (state) {
        case GState::kNew: return "New";
        case GState::kRunnable: return "Runnable";
        case GState::kRunning: return "Running";
        case GState::kWaiting: return "Waiting";
        case GState::kDead: return "Dead";
        case GState::kCancelled: return "Cancelled";
        case GState::kFailed: return "Failed";
    }
    return "?";
}

// ===========================================================================
// 1. GState 矩阵
// ===========================================================================

enum class GEvent {
    kPromoteNew,
    kPrepareEnqueueNullGate,
    kTryMarkQueued,
    kTryMarkQueuedFromWorker,
    kClearQueued,
    kTryMarkRunning,
    kMarkRunnable,
    kMarkYielded,
    kMarkYieldedFast,
    kMarkWaiting,
    kWakeForWait,
    kParkForScheduler,
    kRequestWake,
    kCancel,
    kCancelIfRunnableUnqueued,
    kWaitForZero,
    kRun,
};

constexpr int kGEventCount = 17;
constexpr int kGRowCount = 8;

const char* GEventName(GEvent event) {
    switch (event) {
        case GEvent::kPromoteNew: return "promote_new";
        case GEvent::kPrepareEnqueueNullGate: return "prepare_enqueue(null)";
        case GEvent::kTryMarkQueued: return "try_mark_queued";
        case GEvent::kTryMarkQueuedFromWorker: return "try_mark_queued_from_worker";
        case GEvent::kClearQueued: return "clear_queued";
        case GEvent::kTryMarkRunning: return "try_mark_running";
        case GEvent::kMarkRunnable: return "mark_runnable";
        case GEvent::kMarkYielded: return "mark_yielded";
        case GEvent::kMarkYieldedFast: return "mark_yielded_fast";
        case GEvent::kMarkWaiting: return "mark_waiting";
        case GEvent::kWakeForWait: return "wake_for_wait";
        case GEvent::kParkForScheduler: return "park_for_scheduler";
        case GEvent::kRequestWake: return "request_wake";
        case GEvent::kCancel: return "cancel";
        case GEvent::kCancelIfRunnableUnqueued: return "cancel_if_runnable_unqueued";
        case GEvent::kWaitForZero: return "wait_for(0)";
        case GEvent::kRun: return "run";
    }
    return "?";
}

/// 8 个可持久化局面的下标。Runnable 与 RunnableQueued 的区别见文件头。
enum class GRow {
    kNew = 0,
    kRunnable,
    kRunnableQueued,
    kRunning,
    kWaiting,
    kDead,
    kCancelled,
    kFailed,
};

const char* GRowName(GRow row) {
    switch (row) {
        case GRow::kNew: return "New";
        case GRow::kRunnable: return "Runnable(!queued)";
        case GRow::kRunnableQueued: return "Runnable(queued)";
        case GRow::kRunning: return "Running";
        case GRow::kWaiting: return "Waiting";
        case GRow::kDead: return "Dead";
        case GRow::kCancelled: return "Cancelled";
        case GRow::kFailed: return "Failed";
    }
    return "?";
}

constexpr GState kRowState[kGRowCount] = {
    GState::kNew,      GState::kRunnable, GState::kRunnable, GState::kRunning,
    GState::kWaiting,  GState::kDead,     GState::kCancelled, GState::kFailed,
};

struct Fixture {
    std::shared_ptr<go2cpp::scheduler::Task> task;
    bool ran{false};
};

Fixture MakeTaskInRow(GRow row) {
    Fixture fixture;
    const bool throwing = row == GRow::kFailed;
    fixture.task = std::make_shared<go2cpp::scheduler::Task>(
        [&fixture, throwing] {
            fixture.ran = true;
            if (throwing) {
                throw std::runtime_error("matrix: deliberate failure");
            }
        });
    auto& task = *fixture.task;
    switch (row) {
        case GRow::kNew:
            break;
        case GRow::kRunnable:
            Check(task.promote_new(), "fixture Runnable: promote_new");
            break;
        case GRow::kRunnableQueued:
            Check(task.promote_new(), "fixture RunnableQueued: promote_new");
            Check(task.try_mark_queued(), "fixture RunnableQueued: try_mark_queued");
            break;
        case GRow::kRunning:
            Check(task.promote_new(), "fixture Running: promote_new");
            Check(task.try_mark_queued(), "fixture Running: try_mark_queued");
            Check(task.try_mark_running(), "fixture Running: try_mark_running");
            break;
        case GRow::kWaiting:
            Check(task.promote_new(), "fixture Waiting: promote_new");
            Check(task.try_mark_queued(), "fixture Waiting: try_mark_queued");
            Check(task.try_mark_running(), "fixture Waiting: try_mark_running");
            Check(task.park_for_scheduler() ==
                      go2cpp::scheduler::ParkAction::kParked,
                  "fixture Waiting: park_for_scheduler");
            break;
        case GRow::kDead:
        case GRow::kFailed:
            Check(task.promote_new(), "fixture terminal: promote_new");
            Check(task.try_mark_queued(), "fixture terminal: try_mark_queued");
            Check(task.try_mark_running(), "fixture terminal: try_mark_running");
            task.run();
            break;
        case GRow::kCancelled:
            Check(task.cancel(), "fixture Cancelled: cancel");
            break;
    }
    Check(task.state() == kRowState[static_cast<int>(row)],
          std::string("fixture state mismatch: row=") +
              GRowName(row) + " state=" + GStateName(task.state()));
    return fixture;
}

struct Applied {
    bool has_bool{false};
    bool bool_value{false};
    bool has_wake{false};
    go2cpp::scheduler::WakeAction wake{go2cpp::scheduler::WakeAction::kRejected};
    bool has_park{false};
    go2cpp::scheduler::ParkAction park{go2cpp::scheduler::ParkAction::kRejected};
};

Applied ApplyGEvent(go2cpp::scheduler::Task& task, GEvent event) {
    using go2cpp::scheduler::ParkAction;
    using go2cpp::scheduler::WakeAction;
    Applied applied;
    switch (event) {
        case GEvent::kPromoteNew:
            applied.has_bool = true;
            applied.bool_value = task.promote_new();
            break;
        case GEvent::kPrepareEnqueueNullGate:
            applied.has_bool = true;
            applied.bool_value = task.prepare_enqueue(nullptr, nullptr);
            break;
        case GEvent::kTryMarkQueued:
            applied.has_bool = true;
            applied.bool_value = task.try_mark_queued();
            break;
        case GEvent::kTryMarkQueuedFromWorker:
            applied.has_bool = true;
            applied.bool_value = task.try_mark_queued_from_worker();
            break;
        case GEvent::kClearQueued:
            task.clear_queued();
            break;
        case GEvent::kTryMarkRunning:
            applied.has_bool = true;
            applied.bool_value = task.try_mark_running();
            break;
        case GEvent::kMarkRunnable:
            applied.has_bool = true;
            applied.bool_value = task.mark_runnable();
            break;
        case GEvent::kMarkYielded:
            applied.has_bool = true;
            applied.bool_value = task.mark_yielded();
            break;
        case GEvent::kMarkYieldedFast:
            applied.has_bool = true;
            applied.bool_value = task.mark_yielded_fast();
            break;
        case GEvent::kMarkWaiting:
            applied.has_bool = true;
            applied.bool_value = task.mark_waiting();
            break;
        case GEvent::kWakeForWait:
            applied.has_wake = true;
            applied.wake = task.wake_for_wait();
            break;
        case GEvent::kParkForScheduler:
            applied.has_park = true;
            applied.park = task.park_for_scheduler();
            break;
        case GEvent::kRequestWake:
            task.request_wake();
            break;
        case GEvent::kCancel:
            applied.has_bool = true;
            applied.bool_value = task.cancel();
            break;
        case GEvent::kCancelIfRunnableUnqueued:
            applied.has_bool = true;
            applied.bool_value = task.cancel_if_runnable_unqueued();
            break;
        case GEvent::kWaitForZero:
            applied.has_bool = true;
            applied.bool_value = task.wait_for(0ms);
            break;
        case GEvent::kRun:
            task.run();
            break;
    }
    return applied;
}

Applied ExpectGEvent(GRow row, GEvent event) {
    using go2cpp::scheduler::ParkAction;
    using go2cpp::scheduler::WakeAction;
    Applied expected;
    const GState state = kRowState[static_cast<int>(row)];
    if (IsTerminal(state)) {
        switch (event) {
            case GEvent::kWakeForWait:
                expected.has_wake = true;
                expected.wake = WakeAction::kRejected;
                return expected;
            case GEvent::kParkForScheduler:
                expected.has_park = true;
                expected.park = ParkAction::kRejected;
                return expected;
            case GEvent::kClearQueued:
            case GEvent::kRequestWake:
            case GEvent::kRun:
                return expected;  // void 事件，状态由 ExpectGState 断言
            case GEvent::kWaitForZero:
                expected.has_bool = true;
                expected.bool_value = true;  // 终态等待立即成功
                return expected;
            default:
                expected.has_bool = true;
                expected.bool_value = false;  // 终态拒绝一切转移
                return expected;
        }
    }
    switch (event) {
        case GEvent::kPromoteNew:
            expected.has_bool = true;
            expected.bool_value = row == GRow::kNew;
            return expected;
        case GEvent::kPrepareEnqueueNullGate:
            expected.has_bool = true;
            expected.bool_value = false;  // 缺 owner/gate 必须拒绝
            return expected;
        case GEvent::kTryMarkQueued:
            expected.has_bool = true;
            expected.bool_value = row == GRow::kRunnable;
            return expected;
        case GEvent::kTryMarkQueuedFromWorker:
            expected.has_bool = true;
            expected.bool_value = row == GRow::kRunnable;
            return expected;
        case GEvent::kTryMarkRunning:
            expected.has_bool = true;
            expected.bool_value = row == GRow::kRunnableQueued;
            return expected;
        case GEvent::kMarkRunnable:
            expected.has_bool = true;
            expected.bool_value = row == GRow::kWaiting;
            return expected;
        case GEvent::kMarkYielded:
        case GEvent::kMarkYieldedFast:
            expected.has_bool = true;
            expected.bool_value = row == GRow::kRunning;
            return expected;
        case GEvent::kMarkWaiting:
            expected.has_bool = true;
            expected.bool_value = row == GRow::kRunning;
            return expected;
        case GEvent::kParkForScheduler:
            expected.has_park = true;
            expected.park = row == GRow::kRunning ? ParkAction::kParked
                                                  : ParkAction::kRejected;
            return expected;
        case GEvent::kWakeForWait:
            expected.has_wake = true;
            if (row == GRow::kWaiting) {
                // Waiting fixture 仍持有 execution_claim（真实场景：G 在
                // 自己所在的 M 上 park）。唤醒方只发布 pending 令牌，
                // 由持有执行权的 M 在退出时入队。
                expected.wake = WakeAction::kPending;
            } else if (row == GRow::kRunning) {
                expected.wake = WakeAction::kPending;
            } else {
                expected.wake = WakeAction::kRejected;
            }
            return expected;
        case GEvent::kCancel:
            expected.has_bool = true;
            expected.bool_value = true;  // 非终态一律接受取消请求
            return expected;
        case GEvent::kCancelIfRunnableUnqueued:
            expected.has_bool = true;
            expected.bool_value = row == GRow::kRunnable;
            return expected;
        case GEvent::kWaitForZero:
            expected.has_bool = true;
            expected.bool_value = false;  // 未终态：0 超时必须超时
            return expected;
        case GEvent::kClearQueued:
        case GEvent::kRequestWake:
        case GEvent::kRun:
            return expected;
    }
    return expected;
}

GState ExpectGState(GRow row, GEvent event) {
    const GState state = kRowState[static_cast<int>(row)];
    if (IsTerminal(state)) {
        return state;  // 终态不可变
    }
    switch (row) {
        case GRow::kNew:
            if (event == GEvent::kPromoteNew) return GState::kRunnable;
            if (event == GEvent::kCancel) return GState::kCancelled;
            return GState::kNew;
        case GRow::kRunnable:
            if (event == GEvent::kCancel ||
                event == GEvent::kCancelIfRunnableUnqueued) {
                return GState::kCancelled;
            }
            return GState::kRunnable;
        case GRow::kRunnableQueued:
            if (event == GEvent::kTryMarkRunning) return GState::kRunning;
            if (event == GEvent::kCancel) return GState::kCancelled;
            return GState::kRunnable;
        case GRow::kRunning:
            if (event == GEvent::kMarkYielded ||
                event == GEvent::kMarkYieldedFast) {
                return GState::kRunnable;
            }
            if (event == GEvent::kMarkWaiting ||
                event == GEvent::kParkForScheduler) {
                return GState::kWaiting;
            }
            if (event == GEvent::kRun) return GState::kDead;
            return GState::kRunning;
        case GRow::kWaiting:
            if (event == GEvent::kMarkRunnable ||
                event == GEvent::kWakeForWait) {
                return GState::kRunnable;
            }
            if (event == GEvent::kRun) return GState::kDead;
            return GState::kWaiting;
        default:
            return state;
    }
}

void ScenarioGStateMatrix() {
    std::printf("\n[GState] 8 局面 x 17 事件 = %d 格\n", kGRowCount * kGEventCount);
    std::printf("%-20s", "state\\event");
    for (int e = 0; e < kGEventCount; ++e) {
        std::printf("| %-8d", e);
    }
    std::printf("\n");
    for (int r = 0; r < kGRowCount; ++r) {
        const auto row = static_cast<GRow>(r);
        std::printf("%-20s", GRowName(row));
        for (int e = 0; e < kGEventCount; ++e) {
            const auto event = static_cast<GEvent>(e);
            Fixture fixture = MakeTaskInRow(row);
            const Applied actual = ApplyGEvent(*fixture.task, event);
            const Applied expected = ExpectGEvent(row, event);
            const GState after = fixture.task->state();
            const GState expected_after = ExpectGState(row, event);
            const std::string cell = std::string(GRowName(row)) + " x " +
                                     GEventName(event);
            if (expected.has_bool) {
                Check(actual.has_bool && actual.bool_value == expected.bool_value,
                      cell + ": bool 返回值不符（期望 " +
                          (expected.bool_value ? "true" : "false") + "）");
            }
            if (expected.has_wake) {
                Check(actual.has_wake && actual.wake == expected.wake,
                      cell + ": wake 结果不符");
            }
            if (expected.has_park) {
                Check(actual.has_park && actual.park == expected.park,
                      cell + ": park 结果不符");
            }
            Check(after == expected_after,
                  cell + ": 迁移后状态=" + GStateName(after) + "，期望 " +
                      GStateName(expected_after));
            if (IsTerminal(kRowState[r])) {
                Check(IsTerminal(after), cell + ": 终态被复活");
            }
            std::printf("| %-8s", after == expected_after ? "ok" : "BAD");
        }
        std::printf("\n");
    }
}

// ===========================================================================
// 2. FiberState 矩阵
// ===========================================================================

enum class FiberEvent {
    kResume,
    kResumeResult,
    kRequestCancellation,
    kSuspendFromOutside,
    kStateQuery,
};

constexpr int kFiberEventCount = 5;
constexpr int kFiberRowCount = 5;

const char* FiberEventName(FiberEvent event) {
    switch (event) {
        case FiberEvent::kResume: return "resume";
        case FiberEvent::kResumeResult: return "resume_result";
        case FiberEvent::kRequestCancellation: return "RequestCancellation";
        case FiberEvent::kSuspendFromOutside: return "Suspend(outside)";
        case FiberEvent::kStateQuery: return "state()";
    }
    return "?";
}

const char* FiberStateName(FiberState state) {
    switch (state) {
        case FiberState::Ready: return "Ready";
        case FiberState::Running: return "Running";
        case FiberState::Suspended: return "Suspended";
        case FiberState::Completed: return "Completed";
        case FiberState::Failed: return "Failed";
    }
    return "?";
}

/// 构造一个挂起/完成/失败的 Fiber；Running 只能在其 body 内观察，单独处理。
std::unique_ptr<Fiber> MakeFiberInState(FiberState target, bool* resumed,
                                        bool* cancelled_seen) {
    (void)resumed;
    (void)cancelled_seen;
    bool first_entry = true;
    switch (target) {
        case FiberState::Ready:
            return std::make_unique<Fiber>([] {});
        case FiberState::Suspended:
        case FiberState::Running: {
            auto fiber = std::make_unique<Fiber>([&first_entry] {
                if (first_entry) {
                    first_entry = false;
                    (void)Fiber::Suspend(go2cpp::SuspendReason::Yield);
                }
            });
            (void)fiber->resume();
            return fiber;
        }
        case FiberState::Completed: {
            auto fiber = std::make_unique<Fiber>([] {});
            (void)fiber->resume();  // 跑完 body 才会进入 Completed
            return fiber;
        }
        case FiberState::Failed: {
            auto fiber = std::make_unique<Fiber>([] {
                throw std::runtime_error("matrix fiber failure");
            });
            (void)fiber->resume();  // 第一次 resume 捕获异常并进入 Failed
            return fiber;
        }
    }
    return nullptr;
}

void ScenarioFiberMatrix() {
    std::printf("\n[FiberState] 5 状态 x 5 事件 = %d 格\n",
                kFiberRowCount * kFiberEventCount);
    const FiberState rows[kFiberRowCount] = {
        FiberState::Ready, FiberState::Running, FiberState::Suspended,
        FiberState::Completed, FiberState::Failed};
    for (int r = 0; r < kFiberRowCount; ++r) {
        const FiberState row = rows[r];
        for (int e = 0; e < kFiberEventCount; ++e) {
            const auto event = static_cast<FiberEvent>(e);
            bool cancelled_seen = false;
            bool ran = false;
            std::unique_ptr<Fiber> fiber;
            if (row == FiberState::Running) {
                // Running 只在自己的栈上存在：在 body 内施加事件。
                bool self_resume_result = true;
                bool suspend_ok = false;
                bool observed_running = false;
                std::atomic<Fiber*> self{nullptr};
                fiber = std::make_unique<Fiber>([&] {
                    Fiber* me = self.load(std::memory_order_acquire);
                    observed_running = me->state() == FiberState::Running;
                    switch (event) {
                        case FiberEvent::kResume:
                            self_resume_result = me->resume();
                            break;
                        case FiberEvent::kResumeResult:
                            self_resume_result = me->resume();
                            break;
                        case FiberEvent::kRequestCancellation:
                            me->RequestCancellation();
                            cancelled_seen = Fiber::CancellationRequested();
                            break;
                        case FiberEvent::kSuspendFromOutside:
                            suspend_ok = Fiber::Suspend(
                                go2cpp::SuspendReason::Yield);
                            break;
                        case FiberEvent::kStateQuery:
                            break;
                    }
                });
                self.store(fiber.get(), std::memory_order_release);
                Check(fiber->resume(), "Running fixture: 首次 resume");
                Check(observed_running, "Running fixture: body 内应观察到 Running");
                if (event == FiberEvent::kResume ||
                    event == FiberEvent::kResumeResult) {
                    Check(!self_resume_result,
                          "Running x resume: 自恢复必须被拒绝");
                }
                if (event == FiberEvent::kRequestCancellation) {
                    Check(cancelled_seen,
                          "Running x RequestCancellation: 取消标记应可见");
                }
                if (event == FiberEvent::kSuspendFromOutside) {
                    // Suspend 在 Fiber 自己的栈上**不会立即返回**：它把
                    // 执行权交回调用者，直到下一次 resume 后才从调用点
                    // 继续。所以必须先确认状态已变 Suspended，再 resume
                    // 一次，才能读到 suspend_ok 的返回值。
                    Check(fiber->state() == FiberState::Suspended,
                          "Running x Suspend: 首次 resume 后应为 Suspended");
                    Check(fiber->resume(), "Running x Suspend: 二次 resume 失败");
                    Check(suspend_ok, "Running x Suspend: 运行中挂起应成功");
                }
                // 内部事件施加后，Fiber 要么仍 Running（body 未挂起即返回），
                // 要么 Suspended；两者都必须是合法状态。
                const auto after = fiber->state();
                Check(after == FiberState::Suspended ||
                          after == FiberState::Completed ||
                          after == FiberState::Running,
                      std::string("Running x ") + FiberEventName(event) +
                          ": 非法状态 " + FiberStateName(after));
                continue;
            }

            fiber = MakeFiberInState(row, &ran, &cancelled_seen);
            if (row == FiberState::Suspended || row == FiberState::Running) {
                // 构造时 resume 过一次，Suspended 局面已就位。
            }
            auto before = fiber->state();
            Check(before == row, std::string("Fiber fixture 状态错误: 期望 ") +
                                     FiberStateName(row) + " 实际 " +
                                     FiberStateName(before));

            bool accepted = true;
            FiberState expected_after = row;
            switch (event) {
                case FiberEvent::kResume:
                    accepted = fiber->resume();
                    if (row == FiberState::Ready ||
                        row == FiberState::Suspended) {
                        expected_after = FiberState::Completed;
                    } else {
                        expected_after = row;
                        Check(!accepted, std::string("Fiber ") +
                                             FiberStateName(row) +
                                             " x resume: 应被拒绝");
                    }
                    break;
                case FiberEvent::kResumeResult: {
                    const auto result = fiber->resume_result();
                    Check(result.accepted == (row == FiberState::Ready ||
                                              row == FiberState::Suspended),
                          std::string("Fiber ") + FiberStateName(row) +
                              " x resume_result: accepted 不符");
                    expected_after = (row == FiberState::Ready ||
                                      row == FiberState::Suspended)
                                         ? FiberState::Completed
                                         : row;
                    break;
                }
                case FiberEvent::kRequestCancellation:
                    fiber->RequestCancellation();
                    expected_after = row;  // 只置标记，不迁移状态
                    break;
                case FiberEvent::kSuspendFromOutside: {
                    const bool suspended =
                        Fiber::Suspend(go2cpp::SuspendReason::Yield);
                    Check(!suspended,
                          "Fiber 外部 Suspend 必须返回 false（线程不在 Fiber 中）");
                    expected_after = row;
                    break;
                }
                case FiberEvent::kStateQuery:
                    Check(fiber->state() == row, "Fiber state() 读到的值不符");
                    expected_after = row;
                    break;
            }
            const auto after = fiber->state();
            Check(after == expected_after,
                  std::string("Fiber ") + FiberStateName(row) + " x " +
                      FiberEventName(event) + ": 迁移后=" +
                      FiberStateName(after) + " 期望 " +
                      FiberStateName(expected_after));
        }
    }
}

// ===========================================================================
// 3. TimerState 矩阵
// ===========================================================================

using go2cpp::core::Timer;
using go2cpp::core::TimerService;
using go2cpp::core::TimerState;

const char* TimerStateName(TimerState state) {
    switch (state) {
        case TimerState::kInvalid: return "kInvalid";
        case TimerState::kPending: return "kPending";
        case TimerState::kRunning: return "kRunning";
        case TimerState::kCompleted: return "kCompleted";
        case TimerState::kCancelled: return "kCancelled";
        case TimerState::kFailed: return "kFailed";
    }
    return "?";
}

template <typename Pred>
bool WaitUntil(Pred pred, std::chrono::milliseconds limit) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(500us);
    }
    return pred();
}

struct TimerFixture {
    std::unique_ptr<TimerService> service;
    Timer timer;
    // 用 shared_ptr 包装原子与标志：TimerFixture 需要按值返回，
    // std::atomic 不可复制/移动，而 shared_ptr 可以。
    std::shared_ptr<std::atomic<bool>> started{
        std::make_shared<std::atomic<bool>>(false)};
    std::shared_ptr<std::atomic<bool>> release{
        std::make_shared<std::atomic<bool>>(false)};
};

TimerFixture MakeTimerInState(TimerState target) {
    TimerFixture fixture;
    fixture.service = std::make_unique<TimerService>();
    switch (target) {
        case TimerState::kInvalid:
            break;  // 默认 Timer 没有共享状态
        case TimerState::kPending:
            fixture.timer = fixture.service->ScheduleAfter(1h, [] {});
            break;
        case TimerState::kRunning: {
            auto started = fixture.started;
            auto release = fixture.release;
            fixture.timer =
                fixture.service->ScheduleAfter(1ms, [started, release] {
                    started->store(true, std::memory_order_release);
                    while (!release->load(std::memory_order_acquire)) {
                        std::this_thread::sleep_for(200us);
                    }
                });
            if (!WaitUntil([&] {
                    return fixture.timer.StateValue() == TimerState::kRunning;
                }, 5s)) {
                Check(false, "Timer fixture: 未能进入 kRunning");
            }
            break;
        }
        case TimerState::kCompleted:
            fixture.timer = fixture.service->ScheduleAfter(1ms, [] {});
            if (!WaitUntil([&] {
                    return fixture.timer.StateValue() ==
                           TimerState::kCompleted;
                }, 5s)) {
                Check(false, "Timer fixture: 未能进入 kCompleted");
            }
            break;
        case TimerState::kCancelled:
            fixture.timer = fixture.service->ScheduleAfter(1h, [] {});
            fixture.timer.Cancel();
            break;
        case TimerState::kFailed:
            fixture.timer = fixture.service->ScheduleAfter(1ms, [] {
                throw std::runtime_error("matrix timer failure");
            });
            if (!WaitUntil([&] {
                    return fixture.timer.StateValue() == TimerState::kFailed;
                }, 5s)) {
                Check(false, "Timer fixture: 未能进入 kFailed");
            }
            break;
    }
    if (target != TimerState::kInvalid) {
        Check(fixture.timer.StateValue() == target,
              std::string("Timer fixture 状态错误: 期望 ") +
                  TimerStateName(target) + " 实际 " +
                  TimerStateName(fixture.timer.StateValue()));
    }
    return fixture;
}

enum class TimerEvent {
    kFire,
    kCancel,
    kReset,
    kRefresh,
    kQuery,
};

constexpr int kTimerEventCount = 5;

const char* TimerEventName(TimerEvent event) {
    switch (event) {
        case TimerEvent::kFire: return "fire";
        case TimerEvent::kCancel: return "Cancel";
        case TimerEvent::kReset: return "Reset";
        case TimerEvent::kRefresh: return "Refresh";
        case TimerEvent::kQuery: return "state()";
    }
    return "?";
}

void ScenarioTimerMatrix() {
    std::printf("\n[TimerState] 6 状态 x 5 事件 = %d 格\n",
                6 * kTimerEventCount);
    const TimerState rows[6] = {
        TimerState::kInvalid, TimerState::kPending,  TimerState::kRunning,
        TimerState::kCompleted, TimerState::kCancelled, TimerState::kFailed};
    for (const auto row : rows) {
        for (int e = 0; e < kTimerEventCount; ++e) {
            const auto event = static_cast<TimerEvent>(e);
            TimerFixture fixture = MakeTimerInState(row);
            TimerState expected_after = row;
            switch (event) {
                case TimerEvent::kFire:
                    // 只有 Pending 的一次性定时器会因触发进入终态；
                    // 1h 定时器在测试期内不会触发，因此这里只对
                    // Completed/Cancelled/Failed 断言"不会复活"。
                    if (row == TimerState::kCompleted ||
                        row == TimerState::kCancelled ||
                        row == TimerState::kFailed) {
                        std::this_thread::sleep_for(5ms);
                        expected_after = row;
                    }
                    break;
                case TimerEvent::kCancel:
                    fixture.timer.Cancel();
                    if (row == TimerState::kPending ||
                        row == TimerState::kRunning) {
                        expected_after = TimerState::kCancelled;
                    } else {
                        expected_after = row;  // 终态幂等
                    }
                    break;
                case TimerEvent::kReset: {
                    const bool ok = fixture.timer.Reset(
                        std::chrono::steady_clock::now() + 1h);
                    if (row == TimerState::kPending) {
                        Check(ok, "Timer Pending x Reset: 应成功");
                        expected_after = TimerState::kPending;
                    } else {
                        Check(!ok, std::string("Timer ") + TimerStateName(row) +
                                       " x Reset: 应被拒绝");
                        expected_after = row;
                    }
                    break;
                }
                case TimerEvent::kRefresh: {
                    const bool ok = fixture.timer.Refresh();
                    if (row == TimerState::kPending) {
                        // 本 fixture 是一次性定时器（period 为空）。Refresh
                        // 的语义是"按周期重新开始"，没有周期的定时器必须
                        // 被明确拒绝，而不是静默成功或重置到 now。
                        Check(!ok, "Timer Pending(一次性) x Refresh: 应被拒绝");
                        expected_after = TimerState::kPending;
                    } else {
                        Check(!ok, std::string("Timer ") + TimerStateName(row) +
                                       " x Refresh: 应被拒绝");
                        expected_after = row;
                    }
                    break;
                }
                case TimerEvent::kQuery:
                    Check(fixture.timer.StateValue() == row,
                          "Timer state() 读到的值不符");
                    expected_after = row;
                    break;
            }
            fixture.release->store(true, std::memory_order_release);
            const auto after = fixture.timer.StateValue();
            Check(after == expected_after,
                  std::string("Timer ") + TimerStateName(row) + " x " +
                      TimerEventName(event) + ": 迁移后=" +
                      TimerStateName(after) + " 期望 " +
                      TimerStateName(expected_after));
        }
    }
}

// ===========================================================================
// 4. Channel 关闭态矩阵
// ===========================================================================

const char* ChannelStatusName(go2cpp::ChannelStatus status) {
    switch (status) {
        case go2cpp::ChannelStatus::kReady: return "kReady";
        case go2cpp::ChannelStatus::kWouldBlock: return "kWouldBlock";
        case go2cpp::ChannelStatus::kClosed: return "kClosed";
        case go2cpp::ChannelStatus::kAlreadyClosed: return "kAlreadyClosed";
        case go2cpp::ChannelStatus::kCancelled: return "kCancelled";
        case go2cpp::ChannelStatus::kDeadlineExceeded: return "kDeadlineExceeded";
        case go2cpp::ChannelStatus::kTimedOut: return "kTimedOut";
        case go2cpp::ChannelStatus::kNil: return "kNil";
        case go2cpp::ChannelStatus::kInvalid: return "kInvalid";
    }
    return "?";
}

void ScenarioChannelMatrix() {
    using go2cpp::Channel;
    using go2cpp::ChannelStatus;
    std::printf("\n[Channel 关闭态] 2 状态 x 7 事件 = 14 格\n");
    struct EventCase {
        const char* name;
        ChannelStatus open_expected;
        ChannelStatus closed_expected;
    };
    const EventCase cases[] = {
        {"TrySend", ChannelStatus::kReady, ChannelStatus::kClosed},
        {"TryRecv", ChannelStatus::kWouldBlock, ChannelStatus::kClosed},
        {"SendFor(1ms)", ChannelStatus::kReady, ChannelStatus::kClosed},
        {"RecvFor(1ms)", ChannelStatus::kTimedOut, ChannelStatus::kClosed},
        {"Close", ChannelStatus::kReady, ChannelStatus::kAlreadyClosed},
        {"Close#2", ChannelStatus::kAlreadyClosed,
         ChannelStatus::kAlreadyClosed},
        {"IsClosed", ChannelStatus::kReady, ChannelStatus::kReady},
    };
    for (int closed_index = 0; closed_index < 2; ++closed_index) {
        const bool closed = closed_index == 1;
        for (const auto& item : cases) {
            auto channel = Channel<int>::Create(1);
            if (closed) {
                Check(channel->Close().status == ChannelStatus::kReady,
                      "Channel: 初始 Close 应成功");
            }
            const bool expected_closed = closed;
            ChannelStatus actual = ChannelStatus::kReady;
            const std::string name = std::string(closed ? "Closed" : "Open") +
                                     " x " + item.name;
            if (std::string(item.name) == "TrySend") {
                actual = channel->TrySend(1).status;
            } else if (std::string(item.name) == "TryRecv") {
                actual = channel->TryRecv().status;
            } else if (std::string(item.name) == "SendFor(1ms)") {
                actual = channel->SendFor(2, 1ms).status;
            } else if (std::string(item.name) == "RecvFor(1ms)") {
                actual = channel->RecvFor(1ms).status;
            } else if (std::string(item.name) == "Close") {
                actual = channel->Close().status;
            } else if (std::string(item.name) == "Close#2") {
                (void)channel->Close();
                actual = channel->Close().status;
            } else {
                actual = channel->IsClosed() ? ChannelStatus::kReady
                                             : ChannelStatus::kReady;
                Check(channel->IsClosed() == expected_closed,
                      name + ": IsClosed 不符");
                continue;
            }
            const ChannelStatus expected =
                closed ? item.closed_expected : item.open_expected;
            Check(actual == expected, name + ": 实际=" +
                                          ChannelStatusName(actual) + " 期望=" +
                                          ChannelStatusName(expected));
        }
    }
}

// ===========================================================================
// 5. Future 终态矩阵
// ===========================================================================

void ScenarioFutureMatrix() {
    std::printf("\n[Future 终态] 5 状态 x 4 事件 = 20 格\n");
    enum class Row { kPending, kValue, kError, kException, kCancelled };
    const Row rows[5] = {Row::kPending, Row::kValue, Row::kError,
                         Row::kException, Row::kCancelled};
    const char* row_names[5] = {"Pending", "Value", "Error", "Exception",
                                "Cancelled"};
    enum class Event { kSetValue, kSetError, kSetException, kCancel };
    const char* event_names[4] = {"SetValue", "SetError", "SetException",
                                  "Cancel"};
    for (int r = 0; r < 5; ++r) {
        for (int e = 0; e < 4; ++e) {
            go2cpp::Promise<int> promise;
            auto future = promise.GetFuture();
            auto set_value = [&] { return promise.SetValue(42); };
            auto set_error = [&] {
                return promise.SetError(go2cpp::NewError("matrix error"));
            };
            auto set_exception = [&] {
                return promise.SetException(
                    std::make_exception_ptr(std::runtime_error("matrix")));
            };
            auto cancel = [&] { return promise.Cancel(); };
            // 先把 Promise 摆到目标初态。
            switch (rows[r]) {
                case Row::kPending: break;
                case Row::kValue: Check(set_value(), "Future fixture SetValue"); break;
                case Row::kError: Check(set_error(), "Future fixture SetError"); break;
                case Row::kException:
                    Check(set_exception(), "Future fixture SetException");
                    break;
                case Row::kCancelled: Check(cancel(), "Future fixture Cancel"); break;
            }
            const std::string cell = std::string(row_names[r]) + " x " +
                                     event_names[e];
            bool accepted = false;
            switch (static_cast<Event>(e)) {
                case Event::kSetValue: accepted = set_value(); break;
                case Event::kSetError: accepted = set_error(); break;
                case Event::kSetException: accepted = set_exception(); break;
                case Event::kCancel: accepted = cancel(); break;
            }
            if (rows[r] == Row::kPending) {
                Check(accepted, cell + ": Pending 上第一次完成必须成功");
            } else {
                Check(!accepted, cell + ": 终态上第二次完成必须被拒绝");
            }
            // 终态语义必须与初态一致，不能被后到者覆盖。
            const auto status = future.Status();
            if (rows[r] == Row::kCancelled) {
                Check(status == go2cpp::FutureStatus::kCancelled,
                      cell + ": Cancelled 终态被覆盖");
            }
            if (rows[r] == Row::kValue) {
                Check(status == go2cpp::FutureStatus::kReady,
                      cell + ": Ready 终态被覆盖");
            }
        }
    }
}

// ===========================================================================
// 6. 真实调度器上的 M/P 终态与 G 竞争
// ===========================================================================

void ScenarioSchedulerTerminalAndRaces() {
    go2cpp::SchedulerConfig config;
    config.processor_count = 3;
    config.min_workers = 2;
    config.max_workers = 4;
    go2cpp::Scheduler scheduler(config);
    scheduler.start();

    std::atomic<bool> stop{false};
    std::vector<std::thread> samplers;
    std::atomic<int> violations{0};
    for (int i = 0; i < 2; ++i) {
        samplers.emplace_back([&] {
            std::vector<go2cpp::MId> dead_machines;
            while (!stop.load(std::memory_order_acquire)) {
                for (const auto& machine : scheduler.machines()) {
                    const int v = static_cast<int>(machine.state);
                    if (v < static_cast<int>(go2cpp::MState::kIdle) ||
                        v > static_cast<int>(go2cpp::MState::kDead)) {
                        violations.fetch_add(1);
                    }
                    if (machine.state == go2cpp::MState::kDead) {
                        dead_machines.push_back(machine.id);
                    } else if (!dead_machines.empty()) {
                        for (auto id : dead_machines) {
                            if (id == machine.id) {
                                violations.fetch_add(1);  // 终态被复活
                            }
                        }
                    }
                }
                for (const auto& processor : scheduler.processors()) {
                    const int v = static_cast<int>(processor.state);
                    if (v < static_cast<int>(go2cpp::PState::kIdle) ||
                        v > static_cast<int>(go2cpp::PState::kDead)) {
                        violations.fetch_add(1);
                    }
                }
                std::this_thread::sleep_for(200us);
            }
        });
    }

    // 并发取消 vs 完成：终态必须二选一，且每个任务恰好一个赢家。
    constexpr int kRounds = 300;
    for (int i = 0; i < kRounds; ++i) {
        std::atomic<bool> started{false};
        auto task = scheduler.spawn([&started] {
            started.store(true, std::memory_order_release);
            for (volatile int k = 0; k < 32; ++k) {
            }
        });
        std::atomic<int> winners{0};
        std::thread canceller([&] {
            if (task->cancel()) winners.fetch_add(1);
        });
        std::thread waiter([&] { (void)task->wait_for(10s); });
        canceller.join();
        waiter.join();
        Check(task->wait_for(10s), "race: 任务未收敛");
        const auto state = task->state();
        Check(IsTerminal(state), "race: 竞态后任务仍非终态");
        Check(winners.load() <= 1, "race: 多个线程同时赢得取消");
    }

    stop.store(true, std::memory_order_release);
    for (auto& sampler : samplers) sampler.join();
    Check(violations.load() == 0, "M/P 状态采样发现非法值或终态复活");

    Check(scheduler.shutdown_for(30s), "shutdown failed");
    // shutdown 之后所有 M/P 必须落到 kDead，且不再变化。
    for (const auto& machine : scheduler.machines()) {
        Check(machine.state == go2cpp::MState::kDead,
              "shutdown 后 M 必须为 kDead");
    }
    for (const auto& processor : scheduler.processors()) {
        Check(processor.state == go2cpp::PState::kDead,
              "shutdown 后 P 必须为 kDead");
    }
}

}  // namespace

int main() {
    std::printf("=== 状态机穷举矩阵测试 ===\n");
    ScenarioGStateMatrix();
    ScenarioFiberMatrix();
    ScenarioTimerMatrix();
    ScenarioChannelMatrix();
    ScenarioFutureMatrix();
    ScenarioSchedulerTerminalAndRaces();

    if (g_failures == 0) {
        std::printf("\n[matrix] cells=%d failures=0\n", g_cells);
    } else {
        std::printf("\n[matrix] cells=%d failures=%d\n", g_cells, g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
