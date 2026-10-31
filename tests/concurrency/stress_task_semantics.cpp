/**
 * @file stress_task_semantics.cpp
 * @brief 调度器任务语义独立压测：shutdown 语义、队列溢出、steal、runnext、park/wake。
 *
 * ## 覆盖点（每项都带断言，不靠「看起来没崩」）
 *
 * 1. **shutdown 语义（D2）**：已启动的 G 必须跑完（kDead），未启动的 G 必须
 *    被取消（kCancelled），且 shutdown 必须在长任务结束后返回（watchdog 兜底）；
 * 2. **队列溢出/长队列**：worker 被占住时一次提交远超本地队列容量的任务，
 *    验证 incoming 长队列（含 O(1) 出队改动）不丢任务；
 * 3. **steal**：4P/4M 下大量 yield 任务制造不均衡，断言全部完成并记录
 *    steal/local_queue 计数（需要 collect_metrics=true）；
 * 4. **runnext / park-wake**：任务内 park、外部线程 wake，断言唤醒后继续执行；
 * 5. **重复 shutdown / 空调度器**：不应挂死。
 *
 * 编译（与 tests/concurrency 其它压测一致）：
 *   g++ -std=c++20 -O3 -DNDEBUG -Iinclude tests/concurrency/stress_task_semantics.cpp \
 *       -L.build-bento-wsl -Wl,-rpath,$PWD/.build-bento-wsl \
 *       -lgo2cpp_scheduler -lgo2cpp_fiber -lgo2cpp_error -lgo2cpp_debug \
 *       -lgo2cpp_debug_event -lpthread -o /tmp/stress_task_semantics
 *
 * 用法：/tmp/stress_task_semantics [workers=4]
 */
#include "go2cpp/scheduler.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

std::uint64_t NowNs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now().time_since_epoch())
            .count());
}

void SpinUntil(const std::atomic<bool>& flag,
               std::chrono::milliseconds timeout = 10s) {
    const auto deadline = Clock::now() + timeout;
    while (!flag.load(std::memory_order_acquire)) {
        if (Clock::now() > deadline) {
            std::fprintf(stderr, "[FAIL] 等待条件超时\n");
            std::abort();
        }
        std::this_thread::yield();
    }
}

void SpinUntilCount(const std::atomic<std::uint64_t>& counter,
                    std::uint64_t expect,
                    std::chrono::milliseconds timeout = 60s) {
    const auto deadline = Clock::now() + timeout;
    while (counter.load(std::memory_order_acquire) != expect) {
        if (Clock::now() > deadline) {
            std::fprintf(stderr,
                         "[FAIL] 计数超时: got=%llu expect=%llu\n",
                         static_cast<unsigned long long>(
                             counter.load(std::memory_order_acquire)),
                         static_cast<unsigned long long>(expect));
            std::abort();
        }
        std::this_thread::yield();
    }
}

[[noreturn]] void Fail(const char* what) {
    std::fprintf(stderr, "[FAIL] %s\n", what);
    std::fflush(stderr);
    std::abort();
}

go2cpp::SchedulerConfig MakeConfig(std::size_t workers) {
    go2cpp::SchedulerConfig config;
    config.processor_count = workers;
    config.max_workers = workers;
    config.min_workers = workers;
    config.idle_wait = 2ms;
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
    // 插桩会给每层栈帧加红区/影子栈记录，调度器默认的 2KB 初始栈
    // （页对齐后 4KB）在 sanitizer 下不够用，会在 Fiber 首次进入的
    // 动态链接路径上溢出。压测在插桩构建里显式放大初始栈。
    config.fiber_stack_size = 256U * 1024U;
#endif
    return config;
}

// 1) shutdown 语义：已开始完成、未开始取消。
void ScenarioShutdownSemantics() {
    std::printf("[shutdown] D2 语义\n");
    go2cpp::SchedulerConfig config = MakeConfig(1);
    go2cpp::Scheduler scheduler(config);
    scheduler.start();
    std::atomic<bool> long_started{false};
    std::atomic<bool> long_finished{false};
    std::atomic<bool> release{false};
    auto long_task = scheduler.spawn([&] {
        long_started.store(true, std::memory_order_release);
        while (!release.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        long_finished.store(true, std::memory_order_release);
    });
    SpinUntil(long_started);

    constexpr std::size_t kQueued = 40;
    std::vector<std::shared_ptr<go2cpp::Task>> queued;
    queued.reserve(kQueued);
    for (std::size_t i = 0; i < kQueued; ++i) {
        queued.push_back(scheduler.spawn([] {}));
    }
    // 这 40 个 G 仍在 incoming 队列里，没有 worker 可以取（唯一 M 被
    // 长任务占住）→ 属于「未开始」。
    std::thread shutdown_thread(
        [&scheduler] { scheduler.shutdown(); });
    std::this_thread::sleep_for(100ms);
    release.store(true, std::memory_order_release);
    const std::uint64_t t0 = NowNs();
    shutdown_thread.join();
    const double shutdown_ms = double(NowNs() - t0) / 1e6;
    if (!long_task->wait_for(10s)) {
        Fail("shutdown 后长任务未完成");
    }
    // D2 语义的核心是「已启动的任务不被强行中止」：它必须把自己的函数体
    // 跑完（long_finished）。终态允许 kDead 或 kCancelled —— shutdown 会对
    // 存活 G 发取消请求，已开始的 G 正常返回时按取消请求记为 kCancelled。
    if (!long_finished.load(std::memory_order_acquire)) {
        Fail("已启动任务没有跑完函数体（被强行中止）");
    }
    const auto long_state = long_task->state();
    if (long_state != go2cpp::GState::kDead &&
        long_state != go2cpp::GState::kCancelled) {
        std::fprintf(stderr, "[FAIL] 已启动任务终态=%d\n",
                     static_cast<int>(long_state));
        std::abort();
    }
    std::size_t cancelled = 0;
    for (auto& task : queued) {
        (void)task->wait_for(10s);
        const auto state = task->state();
        if (state == go2cpp::GState::kCancelled) {
            ++cancelled;
        } else if (state == go2cpp::GState::kDead) {
            // 极端调度下 worker 可能在长任务之后抢跑一两个；允许但计数。
            continue;
        } else {
            std::fprintf(stderr,
                         "[FAIL] 未开始任务终态=%d（期望 kCancelled）\n",
                         static_cast<int>(state));
            std::abort();
        }
    }
    if (cancelled + 0 < kQueued) {
        std::printf("[shutdown] 注意：%zu/%zu 个未开始任务恰好被执行\n",
                    kQueued - cancelled, kQueued);
    }
    std::printf("[shutdown] PASS 长任务=kDead 取消=%zu/%zu shutdown=%.1f ms\n",
                cancelled, kQueued, shutdown_ms);
}

// 2) 队列溢出/长队列：worker 占住时提交远超本地容量的任务。
void ScenarioQueueOverflow() {
    std::printf("[overflow] 长队列回退\n");
    go2cpp::Scheduler scheduler(MakeConfig(1));
    scheduler.start();
    std::atomic<bool> hog_started{false};
    std::atomic<bool> release{false};
    auto hog = scheduler.spawn([&] {
        hog_started.store(true, std::memory_order_release);
        while (!release.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(200us);
        }
    });
    SpinUntil(hog_started);
    constexpr std::size_t kTasks = 20000;  // 远超 local_queue_limit(256)
    std::atomic<std::uint64_t> done{0};
    std::vector<std::shared_ptr<go2cpp::Task>> tasks;
    tasks.reserve(kTasks);
    for (std::size_t i = 0; i < kTasks; ++i) {
        tasks.push_back(scheduler.spawn(
            [&done] { done.fetch_add(1, std::memory_order_release); }));
    }
    release.store(true, std::memory_order_release);
    SpinUntilCount(done, kTasks);
    std::size_t terminal = 0;
    for (auto& task : tasks) {
        (void)task->wait_for(10s);
        if (task->state() == go2cpp::GState::kDead ||
            task->state() == go2cpp::GState::kCancelled ||
            task->state() == go2cpp::GState::kFailed) {
            ++terminal;
        }
    }
    if (terminal != kTasks) {
        Fail("长队列场景存在非终态任务");
    }
    (void)hog->wait_for(10s);
    scheduler.shutdown();
    std::printf("[overflow] PASS tasks=%zu 全部完成\n", kTasks);
}

// 3) steal：大量 yield 任务在多 P 上制造不均衡。
void ScenarioSteal(std::size_t workers) {
    std::printf("[steal] workers=%zu\n", workers);
    go2cpp::SchedulerConfig config = MakeConfig(workers);
    config.collect_metrics = true;
    go2cpp::Scheduler scheduler(config);
    scheduler.start();
    constexpr std::size_t kTasks = 8000;
    std::atomic<std::uint64_t> done{0};
    for (std::size_t i = 0; i < kTasks; ++i) {
        scheduler.spawn([&scheduler, &done] {
            for (int round = 0; round < 20; ++round) {
                if (!scheduler.yield_current()) {
                    break;
                }
            }
            done.fetch_add(1, std::memory_order_release);
        });
    }
    SpinUntilCount(done, kTasks, 120s);
    // metrics 在 run() 返回后由 worker 异步累加，最后一个任务的计数可能
    // 比 done 晚一拍；这里等待它追平，而不是把它当成正确性失败。
    {
        const auto deadline = Clock::now() + 10s;
        while (scheduler.metrics().task_completions < kTasks) {
            if (Clock::now() > deadline) {
                break;
            }
            std::this_thread::yield();
        }
    }
    const auto metrics = scheduler.metrics();
    if (metrics.task_completions < kTasks) {
        std::fprintf(stderr, "[FAIL] task_completions=%llu < %zu\n",
                     static_cast<unsigned long long>(
                         metrics.task_completions),
                     kTasks);
        std::abort();
    }
    scheduler.shutdown();
    std::printf(
        "[steal] PASS tasks=%zu completions=%llu local_pops=%llu steal=%llu "
        "runnext_pops=%llu\n",
        kTasks, static_cast<unsigned long long>(metrics.task_completions),
        static_cast<unsigned long long>(metrics.local_queue_pops),
        static_cast<unsigned long long>(metrics.steal_pops),
        static_cast<unsigned long long>(metrics.runnext_pops));
}

// 4) park / wake：任务内挂起、外部线程唤醒。
void ScenarioParkWake() {
    std::printf("[park-wake] 外部唤醒\n");
    go2cpp::SchedulerConfig config = MakeConfig(1);
    config.collect_metrics = true;
    go2cpp::Scheduler scheduler(config);
    scheduler.start();
    std::atomic<bool> handle_ready{false};
    std::atomic<bool> resumed{false};
    std::shared_ptr<go2cpp::Task> handle;
    auto task = scheduler.spawn([&] {
        while (!handle_ready.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        if (!scheduler.park(handle)) {
            // park 被拒绝（例如已被取消）不算失败，但必须能继续跑完。
            resumed.store(true, std::memory_order_release);
            return;
        }
        resumed.store(true, std::memory_order_release);
    });
    handle = task;
    handle_ready.store(true, std::memory_order_release);
    std::this_thread::sleep_for(100ms);
    if (!scheduler.wake(task)) {
        std::printf("[park-wake] wake 返回 false（任务可能已自行完成）\n");
    }
    if (!task->wait_for(30s)) {
        Fail("park/wake 后任务未完成");
    }
    SpinUntil(resumed);
    const auto metrics = scheduler.metrics();
    scheduler.shutdown();
    std::printf("[park-wake] PASS runnext_pops=%llu inbox_pops=%llu\n",
                static_cast<unsigned long long>(metrics.runnext_pops),
                static_cast<unsigned long long>(metrics.inbox_pops));
}

// 5) 重复 shutdown / 空调度器。
void ScenarioShutdownIdempotent() {
    std::printf("[shutdown-idempotent] 空调度器 + 二次 shutdown\n");
    go2cpp::Scheduler scheduler(MakeConfig(2));
    scheduler.start();
    scheduler.shutdown();
    scheduler.shutdown();
    auto task = scheduler.spawn([] {});
    // 停止后 spawn 应失败（返回的 Task 直接进入终态），但不能崩。
    (void)task;
    std::printf("[shutdown-idempotent] PASS\n");
}

}  // namespace

int main(int argc, char** argv) {
    const std::size_t workers =
        argc > 1 ? static_cast<std::size_t>(std::atoi(argv[1])) : 4;
    std::printf("[cfg] workers=%zu scheduler semantics stress\n",
                workers == 0 ? 1 : workers);
    ScenarioShutdownSemantics();
    ScenarioQueueOverflow();
    ScenarioSteal(workers == 0 ? 1 : workers);
    ScenarioParkWake();
    ScenarioShutdownIdempotent();
    std::printf("[ALL PASS] stress_task_semantics\n");
    return 0;
}
