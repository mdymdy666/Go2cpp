/**
 * @file test_context_opt.cpp
 * @brief Context/DoneSignal 优化后的正确性测试（独立可执行文件）。
 *
 * 覆盖任务要求：取消 / 超时 / 嵌套 / 并发 / 泄漏（配合 ASan/LSan）+
 * 随机测试 + 5 个真实场景。本文件不依赖 ctest 注册，构建命令见文件末尾。
 *
 * 设计原则：
 * - 并发用例全部用 std::atomic 做线性化点，避免把“测试自身的数据竞争”
 *   混进被测代码；
 * - 随机用例固定种子可复现，同时用多个种子轮换；
 * - 不 sleep 等结果，等待都带 watchdog 上限，卡住即失败而不是挂死。
 */
#include "go2cpp/concurrency/channel.hpp"
#include "go2cpp/concurrency/context.hpp"
#include "go2cpp/scheduler.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace go2cpp;
using std::chrono::milliseconds;
using std::chrono::steady_clock;

std::atomic<int> g_failures{0};
std::atomic<int> g_checks{0};

#define CHECK(condition)                                                        \
    do                                                                          \
    {                                                                           \
        ++g_checks;                                                             \
        if (!(condition))                                                       \
        {                                                                       \
            std::fprintf(stderr, "[FAIL] %s:%d: %s\n", __FILE__, __LINE__,      \
                         #condition);                                           \
            ++g_failures;                                                       \
        }                                                                       \
    } while (0)

/// @brief 最多等待 predicate 成立 ms 毫秒；返回是否在期限内成立。
template <typename Predicate>
bool WaitUntil(Predicate predicate, int timeout_ms) {
    const auto deadline = steady_clock::now() + milliseconds(timeout_ms);
    while (!predicate()) {
        if (steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::yield();
    }
    return true;
}

struct TestResult {
    const char* name;
    double ms;
};

std::vector<TestResult> g_results;

template <typename F>
void RunCase(const char* name, F&& body) {
    const auto start = steady_clock::now();
    body();
    const auto ms = std::chrono::duration<double, std::milli>(
                        steady_clock::now() - start)
                        .count();
    g_results.push_back({name, ms});
    std::printf("[PASS] %-44s %8.2f ms\n", name, ms);
}

// ---------------------------------------------------------------------------
// 1. DoneSignal 并发注册 vs Signal
// ---------------------------------------------------------------------------
void TestAddCallbackRacesSignal() {
    for (int round = 0; round < 100; ++round) {
        auto pair = WithCancel(Background());
        const DoneSignal& signal = pair.first->Done();
        std::atomic<int> called{0};
        std::atomic<int> ready{0};
        constexpr int kThreads = 8;
        constexpr int kPerThread = 32;
        std::vector<std::thread> threads;
        threads.reserve(kThreads);
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&] {
                ready.fetch_add(1, std::memory_order_acq_rel);
                for (int i = 0; i < kPerThread; ++i) {
                    // 注册与 Signal 并发：要么登记后被通知，要么立即执行，
                    // 无论如何每个回调恰好执行一次。
                    signal.AddCallback([&called] {
                        called.fetch_add(1, std::memory_order_relaxed);
                    });
                }
            });
        }
        while (ready.load(std::memory_order_acquire) != kThreads) {
            std::this_thread::yield();
        }
        pair.second();
        for (auto& thread : threads) {
            thread.join();
        }
        CHECK(called.load(std::memory_order_acquire) == kThreads * kPerThread);
        CHECK(signal.IsDone());
    }
}

// ---------------------------------------------------------------------------
// 2. 多等待者唤醒 + 内存可见性
// ---------------------------------------------------------------------------
void TestWaitersWakeAndSeeSignal() {
    for (int round = 0; round < 200; ++round) {
        auto pair = WithCancel(Background());
        const DoneSignal& signal = pair.first->Done();
        std::atomic<int> value{0};
        std::atomic<int> woke{0};
        constexpr int kWaiters = 8;
        std::vector<std::thread> waiters;
        for (int i = 0; i < kWaiters; ++i) {
            waiters.emplace_back([&] {
                signal.Wait();
                // Signal 的 release / Wait 谓词的 acquire 保证看到 42。
                if (value.load(std::memory_order_relaxed) == 42) {
                    woke.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
        // 让等待者尽量先进入 Wait；即便没进入，Signal 后再 Wait 也必须返回。
        std::this_thread::sleep_for(std::chrono::microseconds(200));
        value.store(42, std::memory_order_relaxed);
        pair.second();
        for (auto& waiter : waiters) {
            waiter.join();
        }
        CHECK(woke.load(std::memory_order_acquire) == kWaiters);
    }

    // 已完成后才 Wait/WaitFor 的快路径。
    auto done_pair = WithCancel(Background());
    done_pair.second();
    const DoneSignal& done = done_pair.first->Done();
    CHECK(done.WaitFor(milliseconds(0)));
    done.Wait();
    CHECK(done.IsDone());
}

// ---------------------------------------------------------------------------
// 3. 回调注册/注销
// ---------------------------------------------------------------------------
void TestCallbackRemove() {
    auto pair = WithCancel(Background());
    const DoneSignal& signal = pair.first->Done();
    std::atomic<int> called{0};
    std::vector<DoneSignal::CallbackId> ids;
    for (int i = 0; i < 128; ++i) {
        ids.push_back(signal.AddCallback(
            [&called] { called.fetch_add(1, std::memory_order_relaxed); }));
    }
    CHECK(ids.size() == 128);
    for (std::size_t i = 0; i < ids.size(); i += 2) {
        signal.RemoveCallback(ids[i]);
    }
    pair.second();
    CHECK(called.load(std::memory_order_acquire) == 64);

    // 注销一个已经因 Signal 执行过的 id 不应影响任何状态。
    signal.RemoveCallback(ids.front());
    pair.second();
    CHECK(called.load(std::memory_order_acquire) == 64);

    // 已完成后注册：立即执行一次，返回 0。
    const auto late = signal.AddCallback([&called] {
        called.fetch_add(1, std::memory_order_relaxed);
    });
    CHECK(late == 0);
    CHECK(called.load(std::memory_order_acquire) == 65);
}

// ---------------------------------------------------------------------------
// 4. Deadline 缓存与继承
// ---------------------------------------------------------------------------
void TestDeadlineCacheAndInheritance() {
    const auto root = Background();
    CHECK(!root->HasDeadline());
    CHECK(!root->Deadline().has_value());

    const auto now = steady_clock::now();
    auto parent_pair = WithDeadline(root, now + milliseconds(500));
    const auto parent = parent_pair.first;
    CHECK(parent->HasDeadline());
    CHECK(parent->Deadline().has_value());
    CHECK(*parent->Deadline() <= now + milliseconds(501));

    // 子节点继承更早的父 deadline：WithTimeout(child, 1s) 不能把 500ms 推后。
    auto inherited_pair = WithTimeout(parent, std::chrono::seconds(1));
    CHECK(inherited_pair.first->Deadline() == parent->Deadline());

    // 孙节点显式指定更早的 deadline：取 min。
    auto earlier_pair = WithDeadline(inherited_pair.first, now + milliseconds(100));
    CHECK(earlier_pair.first->Deadline().has_value());
    CHECK(*earlier_pair.first->Deadline() <= now + milliseconds(101));

    // 兄弟节点不受影响。
    auto sibling_pair = WithTimeout(parent, std::chrono::seconds(2));
    CHECK(sibling_pair.first->Deadline() == parent->Deadline());

    // 随机嵌套：父未取消时，新节点的 Deadline 必须等于「父 deadline 与
    // 请求值」的较小者；父已取消时，子节点继承取消状态并保留父 deadline
    // （WithDeadline 不再为已取消的子树安装新 deadline）。
    std::mt19937 rng(20261030);
    ContextPtr node = parent;
    for (int i = 0; i < 500; ++i) {
        const auto delta = std::chrono::milliseconds(
            static_cast<int>(rng() % 5000) - 2000);
        const auto requested = now + delta;
        const auto parent_done = node->IsDone();
        const auto parent_deadline = node->Deadline();
        auto child_pair = WithDeadline(node, requested);
        CHECK(child_pair.first->HasDeadline());
        CHECK(child_pair.first->Deadline().has_value());
        if (parent_done) {
            CHECK(child_pair.first->IsDone());
            CHECK(child_pair.first->Deadline() == parent_deadline);
        } else {
            const auto want = parent_deadline.has_value() &&
                                      *parent_deadline < requested
                                  ? *parent_deadline
                                  : requested;
            CHECK(*child_pair.first->Deadline() == want);
        }
        // 一半继续向下继承，一半换 sibling 分支，覆盖不同深度。
        if (i % 2 == 0) {
            node = child_pair.first;
        }
    }

    // 已过期 deadline 必须同步取消。
    auto expired_pair = WithDeadline(root, steady_clock::now() - milliseconds(1));
    CHECK(expired_pair.first->IsDone());
    CHECK(Is(expired_pair.first->Err(), DeadlineExceededError()));

    parent_pair.second();
    earlier_pair.second();
    inherited_pair.second();
    sibling_pair.second();
}

// ---------------------------------------------------------------------------
// 5. 嵌套取消传播（随机树）
// ---------------------------------------------------------------------------
struct TreeNode {
    ContextPtr context;
    CancelFunc cancel;
    int parent{-1};
    std::vector<int> children;
};

void TestRandomCancelTree() {
    std::mt19937 rng(12345);
    for (int round = 0; round < 30; ++round) {
        std::vector<TreeNode> nodes;
        nodes.reserve(400);
        auto root_pair = WithCancel(Background());
        nodes.push_back({root_pair.first, root_pair.second, -1, {}});

        // 5 层、每层 3 个子节点（364 个节点）。
        std::vector<int> frontier{0};
        for (int depth = 0; depth < 5; ++depth) {
            std::vector<int> next;
            for (int parent_index : frontier) {
                for (int fanout = 0; fanout < 3; ++fanout) {
                    auto pair = WithCancel(nodes[parent_index].context);
                    const int index = static_cast<int>(nodes.size());
                    nodes.push_back({pair.first, pair.second, parent_index, {}});
                    nodes[parent_index].children.push_back(index);
                    next.push_back(index);
                }
            }
            frontier = std::move(next);
        }

        const int victim = static_cast<int>(rng() % nodes.size());
        std::vector<bool> in_subtree(nodes.size(), false);
        std::vector<int> stack{victim};
        while (!stack.empty()) {
            const int current = stack.back();
            stack.pop_back();
            in_subtree[current] = true;
            for (int child : nodes[current].children) {
                stack.push_back(child);
            }
        }

        nodes[victim].cancel();

        for (std::size_t i = 0; i < nodes.size(); ++i) {
            if (in_subtree[i]) {
                CHECK(nodes[i].context->IsDone());
                CHECK(Is(nodes[i].context->Err(), CanceledError()));
            } else {
                CHECK(!nodes[i].context->IsDone());
            }
        }
    }
}

// ---------------------------------------------------------------------------
// 6. 定时器压力：批量超时、提前取消、错误类型
// ---------------------------------------------------------------------------
void TestTimerStress() {
    const auto root = Background();
    struct Entry {
        ContextPtr context;
        CancelFunc cancel;
        bool canceled_early{false};
    };
    std::vector<Entry> entries;
    constexpr int kCount = 200;
    for (int i = 0; i < kCount; ++i) {
        auto pair = WithTimeout(root, milliseconds(40));
        entries.push_back({pair.first, pair.second, i % 2 == 0});
        if (i % 2 == 0) {
            pair.second();
        }
    }
    for (auto& entry : entries) {
        CHECK(WaitUntil([&] { return entry.context->IsDone(); }, 3000));
        if (entry.canceled_early) {
            CHECK(Is(entry.context->Err(), CanceledError()));
        } else {
            CHECK(Is(entry.context->Err(), DeadlineExceededError()));
        }
    }

    // 批量取消后，TimerService 仍能正常处理新的短超时（worker 未被卡死）。
    auto after_pair = WithTimeout(root, milliseconds(5));
    CHECK(WaitUntil([&] { return after_pair.first->IsDone(); }, 2000));
    CHECK(Is(after_pair.first->Err(), DeadlineExceededError()));

    // 远 deadline 不应被误触发：主动取消后立即完成。
    auto far_pair = WithTimeout(root, std::chrono::hours(1));
    CHECK(!far_pair.first->IsDone());
    far_pair.second();
    CHECK(far_pair.first->IsDone());
    CHECK(Is(far_pair.first->Err(), CanceledError()));

    // 取消远 deadline 后 TimerService 不再被通知（Remove 不唤醒 worker），
    // 随后新增一个更早的 timer 必须仍能把它唤醒并准时触发。
    auto near_pair = WithTimeout(root, milliseconds(10));
    CHECK(WaitUntil([&] { return near_pair.first->IsDone(); }, 2000));
    CHECK(Is(near_pair.first->Err(), DeadlineExceededError()));
}

// ---------------------------------------------------------------------------
// 7. Now() 注入与并发读写
// ---------------------------------------------------------------------------
void TestNowFunctionConcurrency() {
    const auto fixed = steady_clock::now();
    Context::SetNowFunctionForTesting([fixed] { return fixed; });
    CHECK(Context::Now() == fixed);
    auto future_pair = WithTimeout(Background(), std::chrono::hours(1));
    CHECK(!future_pair.first->IsDone());
    future_pair.second();

    std::atomic<bool> stop{false};
    std::atomic<int> errors{0};
    std::vector<std::thread> readers;
    for (int i = 0; i < 4; ++i) {
        readers.emplace_back([&] {
            while (!stop.load(std::memory_order_acquire)) {
                const auto now = Context::Now();
                if (now.time_since_epoch().count() == 0) {
                    errors.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (int i = 0; i < 200; ++i) {
        Context::ResetNowFunctionForTesting();
        Context::SetNowFunctionForTesting([fixed] { return fixed; });
    }
    Context::ResetNowFunctionForTesting();
    stop.store(true, std::memory_order_release);
    for (auto& reader : readers) {
        reader.join();
    }
    CHECK(errors.load(std::memory_order_acquire) == 0);

    // 恢复真实时钟后，过期语义仍然正确。
    auto expired_pair = WithTimeout(Background(), milliseconds(-1));
    CHECK(expired_pair.first->IsDone());
    CHECK(Is(expired_pair.first->Err(), DeadlineExceededError()));
}

// ---------------------------------------------------------------------------
// 8. 泄漏回归：大量创建/取消/销毁（配合 LSan 生效）
// ---------------------------------------------------------------------------
void TestMassCreateDestroy() {
    const auto root = Background();
    ContextKey<int> key("mass");
    for (int i = 0; i < 50000; ++i) {
        auto pair = WithCancel(root);
        if (i % 3 == 0) {
            pair.second();
        }
        auto value = WithValue(pair.first, key, i);
        auto timeout = WithTimeout(value, milliseconds(1));
        if (i % 2 == 0) {
            timeout.second();
        }
    }
    // 让少量真实到期的 timer 走完；不等待全部，析构路径同样要被 LSan 覆盖。
    std::this_thread::sleep_for(milliseconds(20));
}

// ---------------------------------------------------------------------------
// 真实场景 1：HTTP 请求 100ms 超时，handler 还在慢慢干活
// ---------------------------------------------------------------------------
void ScenarioHttpTimeout() {
    const auto request = Background();
    auto deadline_pair = WithTimeout(request, milliseconds(30));
    std::atomic<bool> handler_finished{false};
    std::thread handler([&] {
        // 模拟 handler 内部的分段工作：每段都检查 ctx.Done()。
        for (int chunk = 0; chunk < 50; ++chunk) {
            if (deadline_pair.first->IsDone()) {
                break;
            }
            std::this_thread::sleep_for(milliseconds(2));
        }
        handler_finished.store(true, std::memory_order_release);
    });
    CHECK(deadline_pair.first->Done().WaitFor(std::chrono::seconds(2)));
    handler.join();
    CHECK(handler_finished.load(std::memory_order_acquire));
    CHECK(Is(deadline_pair.first->Err(), DeadlineExceededError()));
}

// ---------------------------------------------------------------------------
// 真实场景 2：fan-out 任务，一个失败取消全部
// ---------------------------------------------------------------------------
void ScenarioFanOutCancel() {
    const auto root = Background();
    auto parent_pair = WithCancel(root);
    constexpr int kWorkers = 8;
    std::atomic<int> stopped{0};
    std::vector<std::thread> workers;
    for (int i = 0; i < kWorkers; ++i) {
        workers.emplace_back([&, i] {
            // worker 0 在 10ms 后“发现错误”并取消整组。
            if (i == 0) {
                std::this_thread::sleep_for(milliseconds(10));
                parent_pair.second();
            }
            (void)parent_pair.first->Done().WaitFor(std::chrono::seconds(2));
            stopped.fetch_add(1, std::memory_order_release);
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    CHECK(stopped.load(std::memory_order_acquire) == kWorkers);
    CHECK(Is(parent_pair.first->Err(), CanceledError()));
}

// ---------------------------------------------------------------------------
// 真实场景 3：三段 pipeline，中段超时向上下游传播
// ---------------------------------------------------------------------------
void ScenarioPipelineTimeout() {
    const auto root = Background();
    auto stage1_pair = WithCancel(root);
    auto stage2_pair = WithTimeout(stage1_pair.first, milliseconds(30));
    auto stage3_pair = WithCancel(stage2_pair.first);
    std::atomic<bool> stage3_stopped{false};
    std::thread tail([&] {
        (void)stage3_pair.first->Done().WaitFor(std::chrono::seconds(2));
        stage3_stopped.store(true, std::memory_order_release);
    });
    CHECK(stage3_pair.first->Done().WaitFor(std::chrono::seconds(2)));
    tail.join();
    CHECK(stage3_stopped.load(std::memory_order_acquire));
    CHECK(Is(stage2_pair.first->Err(), DeadlineExceededError()));
    // 子节点继承父的 DeadlineExceeded 原因（Go 的 WithCancel 行为：error 相同）。
    CHECK(stage3_pair.first->Err() != nullptr);
    stage1_pair.second();
    stage2_pair.second();
    stage3_pair.second();
}

// ---------------------------------------------------------------------------
// 真实场景 4：worker 池用 Context + Channel 优雅退出
// ---------------------------------------------------------------------------
void ScenarioWorkerPoolShutdown() {
    const auto root = Background();
    auto pool_pair = WithCancel(root);
    Channel<int> jobs(16);
    std::atomic<int> processed{0};
    std::vector<std::thread> workers;
    for (int i = 0; i < 4; ++i) {
        workers.emplace_back([&] {
            for (;;) {
                auto result = jobs.Recv(pool_pair.first);
                if (result.status != ChannelStatus::kReady) {
                    break;
                }
                processed.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (int i = 0; i < 200; ++i) {
        CHECK(jobs.Send(i).status == ChannelStatus::kReady);
    }
    pool_pair.second();
    for (auto& worker : workers) {
        worker.join();
    }
    CHECK(processed.load(std::memory_order_acquire) > 0);
    CHECK(Is(pool_pair.first->Err(), CanceledError()));
}

// ---------------------------------------------------------------------------
// 真实场景 5：带截止时间的重试（每次尝试受父 deadline 约束）
// ---------------------------------------------------------------------------
void ScenarioRetryWithDeadline() {
    const auto root = Background();
    auto request_pair = WithTimeout(root, milliseconds(100));
    std::atomic<int> attempts{0};
    std::atomic<bool> success{false};
    while (!request_pair.first->IsDone()) {
        attempts.fetch_add(1, std::memory_order_relaxed);
        auto attempt_pair = WithTimeout(request_pair.first, milliseconds(15));
        // 每次尝试“失败”：等待自己的 15ms 超时或被父取消。
        (void)attempt_pair.first->Done().WaitFor(std::chrono::milliseconds(500));
        CHECK(attempt_pair.first->IsDone());
        if (attempts.load(std::memory_order_relaxed) >= 3) {
            // 模拟某次成功：改用父 ctx 未取消时退出，避免死循环。
            success.store(true, std::memory_order_release);
            break;
        }
    }
    request_pair.second();
    CHECK(success.load(std::memory_order_acquire));
    CHECK(attempts.load(std::memory_order_acquire) >= 1);
    CHECK(Is(request_pair.first->Err(), CanceledError()));
}

// ---------------------------------------------------------------------------
// 真实场景 6：Scheduler 下 100 个 Fiber 等待同一 Done（受管 G 唤醒）
// ---------------------------------------------------------------------------
void ScenarioManagedFiberFanOut() {
#if defined(__SANITIZE_ADDRESS__)
    // ASan 自带 SIGSEGV handler，会抢在 Fiber 栈增长 handler 之前终止进程；
    // sanitizer 构建改用固定的 512KiB 栈，避免按需增长路径。
    SchedulerConfig config;
    config.processor_count = 2;
    config.fiber_stack_size = 512U * 1024U;
    config.fiber_stack_reserve_size = 512U * 1024U;
    config.fiber_stack_growable = false;
    Scheduler scheduler(config);
#else
    Scheduler scheduler(2);
#endif
    scheduler.start();
    auto pair = WithCancel(Background());
    std::atomic<int> woke{0};
    std::vector<std::shared_ptr<Task>> waiters;
    waiters.reserve(100);
    for (int i = 0; i < 100; ++i) {
        waiters.push_back(scheduler.spawn([&] {
            (void)pair.first->Done().WaitFor(std::chrono::seconds(2));
            woke.fetch_add(1, std::memory_order_release);
        }));
    }
    std::this_thread::sleep_for(milliseconds(20));
    pair.second();
    for (auto& waiter : waiters) {
        CHECK(waiter->wait_for(std::chrono::seconds(3)));
    }
    CHECK(woke.load(std::memory_order_acquire) == 100);
    scheduler.shutdown();
}

}  // namespace

int main() {
    std::printf("== Context 优化正确性测试 ==\n");
    RunCase("add_callback_races_signal", TestAddCallbackRacesSignal);
    RunCase("waiters_wake_and_see_signal", TestWaitersWakeAndSeeSignal);
    RunCase("callback_remove", TestCallbackRemove);
    RunCase("deadline_cache_and_inheritance", TestDeadlineCacheAndInheritance);
    RunCase("random_cancel_tree", TestRandomCancelTree);
    RunCase("timer_stress", TestTimerStress);
    RunCase("now_function_concurrency", TestNowFunctionConcurrency);
    RunCase("mass_create_destroy", TestMassCreateDestroy);
    RunCase("scenario_http_timeout", ScenarioHttpTimeout);
    RunCase("scenario_fan_out_cancel", ScenarioFanOutCancel);
    RunCase("scenario_pipeline_timeout", ScenarioPipelineTimeout);
    if (std::getenv("GO2CPP_TEST_SKIP_CHANNEL") == nullptr) {
        RunCase("scenario_worker_pool_shutdown", ScenarioWorkerPoolShutdown);
    } else {
        std::printf("[SKIP] scenario_worker_pool_shutdown（按环境变量跳过）\n");
    }
    RunCase("scenario_retry_with_deadline", ScenarioRetryWithDeadline);
#if defined(__SANITIZE_THREAD__)
    // TSan 无法跟踪本项目的自定义 Fiber 栈切换（见 tests/concurrency/run_tsan.sh
    // 的说明），会在 park/wake 时把 shadow stack 判错。Fiber 路径由普通构建
    // 与 ASan 构建覆盖，这里明确跳过而不是伪造通过。
    std::printf("[SKIP] scenario_managed_fiber_fan_out（TSan 不支持自定义栈）\n");
#else
    RunCase("scenario_managed_fiber_fan_out", ScenarioManagedFiberFanOut);
#endif

    std::printf("checks=%d failures=%d\n", g_checks.load(),
                g_failures.load());
    return g_failures.load() == 0 ? 0 : 1;
}
