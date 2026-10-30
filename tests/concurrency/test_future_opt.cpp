/**
 * @file test_future_opt.cpp
 * @brief Future 无锁状态机的正确性 / 场景 / 随机测试（独立可执行，不注册 ctest）。
 *
 * 覆盖：
 *  - 6 个真实场景：请求-响应流水线、取消树隔离、超时重试、多等待者广播、
 *    故障传播（错误/异常/断裂 Promise）、Fiber 等待与取消；
 *  - 随机 fuzz：8 线程并发随机 Get/Set/Error/Exception/Cancel/Wait；
 *  - 抢占竞态：4 线程用 4 种方式抢同一个 Promise，恰好一个成功；
 *  - 边界：零超时、已取消 Context、重复 Get、超时后重试、超时不改变状态。
 *
 * 构建（见 tests/concurrency/run_future_opt_tests.sh）：
 *   c++ -O2 -DNDEBUG -std=c++17 -Iinclude tests/concurrency/test_future_opt.cpp \
 *       -L.build-bento-wsl -Wl,-rpath,$PWD/.build-bento-wsl \
 *       -lgo2cpp_scheduler -lgo2cpp_fiber -lgo2cpp_context -lgo2cpp_future \
 *       -lgo2cpp_sync -lgo2cpp_error -lgo2cpp_log -lgo2cpp_debug_event \
 *       -lgo2cpp_debug -lgo2cpp_control_flow -lpthread -o /tmp/future_opt_test
 */
#include "go2cpp/concurrency/context.hpp"
#include "go2cpp/concurrency/future.hpp"
#include "go2cpp/scheduler.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace go2cpp;
using namespace std::chrono_literals;

// Sanitizer 会显著放大栈帧；Fiber 默认初始栈只有 2KB（按页增长），
// 在 ASan/TSan 下容易把「栈增长」路径本身压爆。这里为受管 Fiber 场景
// 打开更大的固定栈，让 sanitizer 构建测的是 Future 语义而不是栈大小。
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define FUTURE_OPT_SANITIZER_BUILD 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define FUTURE_OPT_SANITIZER_BUILD 1
#endif
#endif

#ifdef FUTURE_OPT_SANITIZER_BUILD
constexpr std::size_t kFiberStackSize = 512U * 1024U;
#else
constexpr std::size_t kFiberStackSize = 0U;  // 0 = 使用运行时默认值
#endif

std::atomic<int> g_failures{0};

void check(bool condition, const char* expression, const char* file, int line) {
    if (condition) {
        return;
    }
    g_failures.fetch_add(1, std::memory_order_relaxed);
    std::fprintf(stderr, "%s:%d: check failed: %s\n", file, line, expression);
}

#define CHECK(expr) check((expr), #expr, __FILE__, __LINE__)

bool WaitForCount(const std::atomic<int>& counter, int target,
                  std::chrono::milliseconds limit) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (counter.load(std::memory_order_acquire) < target) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

void JoinAll(std::vector<std::thread>& threads) {
    for (auto& thread : threads) {
        if (thread.joinable()) {
            thread.join();
        }
    }
}

bool IsTerminal(FutureStatus status) {
    return status == FutureStatus::kReady || status == FutureStatus::kError ||
           status == FutureStatus::kException ||
           status == FutureStatus::kCancelled;
}

// ---------------------------------------------------------------------------
// 场景 1：请求-响应流水线。4 个生产者各发 250 个请求，每个请求一个
// Promise/Future；4 个工作者等待响应并校验值。
// ---------------------------------------------------------------------------
void ScenarioRequestPipeline() {
    constexpr int kProducers = 4;
    constexpr int kWorkers = 4;
    constexpr int kPerProducer = 250;
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<std::pair<Future<int>, int>> queue;
    bool producers_done = false;
    std::atomic<int> completed{0};
    std::atomic<int> mismatch{0};

    std::vector<std::thread> producers;
    for (int index = 0; index < kProducers; ++index) {
        producers.emplace_back([&, index] {
            for (int i = 0; i < kPerProducer; ++i) {
                auto pair = MakePromise<int>();
                const int payload = index * kPerProducer + i;
                Future<int> future = std::move(pair.second);
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    queue.emplace_back(std::move(future), payload);
                }
                cv.notify_one();
                // 模拟异步计算后再完成。
                std::this_thread::sleep_for(
                    std::chrono::microseconds((index * 7 + i) % 20));
                if (!pair.first.SetValue(payload * 2)) {
                    mismatch.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    std::vector<std::thread> workers;
    for (int index = 0; index < kWorkers; ++index) {
        workers.emplace_back([&] {
            for (;;) {
                std::pair<Future<int>, int> request;
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    cv.wait(lock, [&] { return !queue.empty() || producers_done; });
                    if (queue.empty()) {
                        return;
                    }
                    request = std::move(queue.front());
                    queue.pop_front();
                }
                const auto result = request.first.GetResultFor(2s);
                if (!result.Ok() || !result.Value() ||
                    *result.Value() != request.second * 2) {
                    mismatch.fetch_add(1, std::memory_order_relaxed);
                }
                completed.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    JoinAll(producers);
    {
        std::lock_guard<std::mutex> lock(mutex);
        producers_done = true;
    }
    cv.notify_all();
    JoinAll(workers);

    CHECK(queue.empty());
    CHECK(completed.load() == kProducers * kPerProducer);
    CHECK(mismatch.load() == 0);
    std::printf("[opt] 场景1 请求-响应流水线: completed=%d mismatch=%d\n",
                completed.load(), mismatch.load());
}

// ---------------------------------------------------------------------------
// 场景 2：取消树隔离。root 下 4 个子 Context，各 4 个等待者。取消一个子
// 节点只影响它自己的等待者；取消 root 才级联到全部。
// ---------------------------------------------------------------------------
void ScenarioCancelTree() {
    auto root_pair = WithCancel(Background());
    const auto root = root_pair.first;
    constexpr int kChildren = 4;
    constexpr int kWaiters = 4;

    struct ChildState {
        ContextPtr context;
        CancelFunc cancel;
        std::vector<Future<int>> futures;
        std::vector<std::unique_ptr<Promise<int>>> promises;
        std::atomic<int> cancelled{0};
    };

    std::vector<std::unique_ptr<ChildState>> children;
    std::vector<std::thread> waiters;
    for (int index = 0; index < kChildren; ++index) {
        auto pair = WithCancel(root);
        auto state = std::make_unique<ChildState>();
        state->context = pair.first;
        state->cancel = pair.second;
        ChildState* const raw = state.get();
        for (int w = 0; w < kWaiters; ++w) {
            auto promise = std::make_unique<Promise<int>>();
            state->futures.push_back(promise->GetFuture());
            state->promises.push_back(std::move(promise));
        }
        for (const auto& future : state->futures) {
            waiters.emplace_back([raw, future] {
                const auto status = future.Wait(raw->context);
                if (status == FutureStatus::kCancelled ||
                    status == FutureStatus::kDeadlineExceeded) {
                    raw->cancelled.fetch_add(1, std::memory_order_acq_rel);
                }
            });
        }
        children.push_back(std::move(state));
    }

    std::this_thread::sleep_for(30ms);
    children[0]->cancel();
    CHECK(WaitForCount(children[0]->cancelled, kWaiters, 2s));
    for (int index = 1; index < kChildren; ++index) {
        CHECK(children[index]->cancelled.load() == 0);
    }

    root_pair.second();
    for (int index = 1; index < kChildren; ++index) {
        CHECK(WaitForCount(children[index]->cancelled, kWaiters, 2s));
    }
    JoinAll(waiters);

    // 取消只影响等待结果，不改变 Future 状态：Promise 仍可完成。
    int ready_after_cancel = 0;
    for (auto& state : children) {
        for (auto& promise : state->promises) {
            CHECK(promise->SetValue(1));
        }
    }
    for (auto& state : children) {
        for (const auto& future : state->futures) {
            if (future.GetResultFor(0ms).Ok()) {
                ++ready_after_cancel;
            }
        }
    }
    CHECK(ready_after_cancel == kChildren * kWaiters);
    std::printf("[opt] 场景2 取消树: 子节点隔离/父级级联/完成后仍可读 ok=%d\n",
                ready_after_cancel);
}

// ---------------------------------------------------------------------------
// 场景 3：超时重试。慢服务 30ms 后完成；调用方每 5ms 超时一次并重试。
// ---------------------------------------------------------------------------
void ScenarioTimeoutRetry() {
    auto pair = MakePromise<int>();
    auto future = pair.second;
    std::thread slow([promise = std::move(pair.first)]() mutable {
        std::this_thread::sleep_for(30ms);
        (void)promise.SetValue(88);
    });

    int timeouts = 0;
    bool ready = false;
    for (int attempt = 0; attempt < 200; ++attempt) {
        const auto result = future.GetResultFor(5ms);
        if (result.Ok()) {
            ready = result.Value() && *result.Value() == 88;
            break;
        }
        CHECK(result.TimedOut());
        // 超时不改变 Future 状态。
        CHECK(future.Status() == FutureStatus::kTimedOut);
        ++timeouts;
    }
    slow.join();
    CHECK(ready);
    CHECK(timeouts >= 1);
    CHECK(future.GetResultFor(0ms).Ok());
    std::printf("[opt] 场景3 超时重试: timeouts=%d 最终 ready\n", timeouts);
}

// ---------------------------------------------------------------------------
// 场景 4：多等待者广播。16 个线程等待同一个 Future，一次 SetValue 全部唤醒，
// 且值一致；完成后重复 Get 不被消费。
// ---------------------------------------------------------------------------
void ScenarioMultiWaiter() {
    constexpr int kWaiters = 16;
    auto pair = MakePromise<int>();
    auto future = pair.second;
    std::atomic<int> ready{0};
    std::atomic<int> wrong{0};
    std::vector<std::thread> waiters;
    for (int index = 0; index < kWaiters; ++index) {
        waiters.emplace_back([future, &ready, &wrong] {
            const auto result = future.GetResultFor(3s);
            if (!result.Ok() || !result.Value() || *result.Value() != 7) {
                wrong.fetch_add(1, std::memory_order_relaxed);
            }
            ready.fetch_add(1, std::memory_order_release);
        });
    }
    std::this_thread::sleep_for(20ms);
    CHECK(pair.first.SetValue(7));
    CHECK(WaitForCount(ready, kWaiters, 3s));
    JoinAll(waiters);
    CHECK(ready.load() == kWaiters);
    CHECK(wrong.load() == 0);
    CHECK(future.GetOrThrow() == 7);
    std::printf("[opt] 场景4 多等待者广播: ready=%d wrong=%d\n",
                ready.load(), wrong.load());
}

// ---------------------------------------------------------------------------
// 场景 5：故障传播。错误 / 异常 / 断裂 Promise 三条路径 + 并发读取方。
// ---------------------------------------------------------------------------
void ScenarioFailurePropagation() {
    auto error_pair = MakePromise<int>();
    auto error = NewError("scenario error");
    CHECK(error_pair.first.SetError(error));
    bool error_threw = false;
    try {
        (void)error_pair.second.GetOrThrow();
    } catch (const FutureError& failure) {
        error_threw = failure.status() == FutureStatus::kError &&
                      Is(failure.error(), error);
    }
    CHECK(error_threw);

    auto exception_pair = MakePromise<int>();
    try {
        throw std::runtime_error("scenario exception");
    } catch (...) {
        CHECK(exception_pair.first.SetException(std::current_exception()));
    }
    bool exception_threw = false;
    try {
        (void)exception_pair.second.GetOrThrow();
    } catch (const std::runtime_error& payload) {
        exception_threw = std::string(payload.what()) == "scenario exception";
    }
    CHECK(exception_threw);

    Future<void> abandoned;
    {
        Promise<void> local;
        abandoned = local.GetFuture();
    }
    CHECK(abandoned.GetResult().status == FutureStatus::kError);
    CHECK(Is(abandoned.GetResult().error, BrokenPromiseError()));

    // 并发读取方共享同一个错误 Future。
    constexpr int kReaders = 16;
    std::atomic<int> threw{0};
    std::vector<std::thread> readers;
    for (int index = 0; index < kReaders; ++index) {
        readers.emplace_back([future = error_pair.second, &threw] {
            try {
                (void)future.GetOrThrow();
            } catch (const FutureError&) {
                threw.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    JoinAll(readers);
    CHECK(threw.load() == kReaders);
    std::printf("[opt] 场景5 故障传播: error/exception/broken + 并发读者=%d\n",
                threw.load());
}

// ---------------------------------------------------------------------------
// 场景 6：受管 Fiber 等待与取消。
// ---------------------------------------------------------------------------
void ScenarioFiber() {
    SchedulerConfig config;
    config.processor_count = 2;
    config.max_workers = 2;
    config.idle_wait = 1ms;
    if (kFiberStackSize != 0) {
        config.fiber_stack_size = kFiberStackSize;
        config.fiber_stack_growable = false;
    }
    Scheduler scheduler(config);
    scheduler.start();

    constexpr int kTasks = 8;
    auto pair = MakePromise<int>();
    std::atomic<int> ready{0};
    std::vector<std::shared_ptr<Task>> tasks;
    for (int index = 0; index < kTasks; ++index) {
        tasks.push_back(scheduler.spawn([future = pair.second, &ready] {
            const auto result = future.GetResultFor(2s);
            if (result.Ok() && result.Value() && *result.Value() == 9) {
                ready.fetch_add(1, std::memory_order_relaxed);
            }
        }));
    }
    std::this_thread::sleep_for(30ms);
    CHECK(pair.first.SetValue(9));
    for (const auto& task : tasks) {
        CHECK(task->wait_for(3s));
    }
    CHECK(ready.load() == kTasks);

    auto cancel_pair = MakePromise<int>();
    auto cancel_context = WithCancel(Background());
    std::atomic<int> cancelled{0};
    std::vector<std::shared_ptr<Task>> cancel_tasks;
    for (int index = 0; index < kTasks; ++index) {
        cancel_tasks.push_back(scheduler.spawn(
            [future = cancel_pair.second, context = cancel_context.first,
             &cancelled] {
                const auto status = future.Wait(context);
                if (status == FutureStatus::kCancelled ||
                    status == FutureStatus::kDeadlineExceeded) {
                    cancelled.fetch_add(1, std::memory_order_relaxed);
                }
            }));
    }
    std::this_thread::sleep_for(30ms);
    cancel_context.second();
    for (const auto& task : cancel_tasks) {
        CHECK(task->wait_for(3s));
    }
    CHECK(cancelled.load() == kTasks);
    CHECK(cancel_pair.first.SetValue(1));

    scheduler.shutdown();
    std::printf("[opt] 场景6 Fiber: ready=%d cancelled=%d\n", ready.load(),
                cancelled.load());
}

// ---------------------------------------------------------------------------
// 随机 fuzz：8 线程对同一个 Future 随机执行 Get / Status / TryGet / 短超时
// Wait / 4 种完成方式。每轮恰好一个完成者成功，终态可重复读取。
// ---------------------------------------------------------------------------
void ScenarioRandomFuzz() {
    constexpr int kThreads = 8;
    constexpr int kRounds = 300;
    std::atomic<Promise<int>*> slot_promise{nullptr};
    std::atomic<Future<int>*> slot_future{nullptr};
    std::atomic<std::uint32_t> phase{0};
    std::atomic<int> done{0};
    std::atomic<int> winners{0};
    std::atomic<int> invariant_bad{0};
    std::atomic<bool> stop{false};

    std::vector<std::thread> workers;
    for (int index = 0; index < kThreads; ++index) {
        workers.emplace_back([&, index] {
            std::mt19937 rng(static_cast<std::uint32_t>(index) * 2654435761U +
                             97U);
            std::uint32_t seen = 0;
            while (!stop.load(std::memory_order_acquire)) {
                const std::uint32_t current =
                    phase.load(std::memory_order_acquire);
                Promise<int>* promise = slot_promise.load(std::memory_order_acquire);
                Future<int>* future = slot_future.load(std::memory_order_acquire);
                if (current == seen || promise == nullptr || future == nullptr) {
                    std::this_thread::yield();
                    continue;
                }
                seen = current;
                switch (rng() % 8U) {
                case 0: {
                    const auto result = future->TryGet();
                    (void)result;
                    break;
                }
                case 1:
                    (void)future->Status();
                    break;
                case 2: {
                    const auto result = future->GetResultFor(0ms);
                    (void)result;
                    break;
                }
                case 3: {
                    const auto result = future->GetResultFor(
                        std::chrono::microseconds(rng() % 40U));
                    if (result.Ok() && (!result.Value() || *result.Value() != 1)) {
                        invariant_bad.fetch_add(1, std::memory_order_relaxed);
                    }
                    break;
                }
                case 4:
                    if (promise->SetValue(1)) {
                        winners.fetch_add(1, std::memory_order_acq_rel);
                    }
                    break;
                case 5:
                    if (promise->SetError(NewError("fuzz error"))) {
                        winners.fetch_add(1, std::memory_order_acq_rel);
                    }
                    break;
                case 6:
                    try {
                        throw std::runtime_error("fuzz exception");
                    } catch (...) {
                        if (promise->SetException(std::current_exception())) {
                            winners.fetch_add(1, std::memory_order_acq_rel);
                        }
                    }
                    break;
                default:
                    if (promise->Cancel()) {
                        winners.fetch_add(1, std::memory_order_acq_rel);
                    }
                    break;
                }
                done.fetch_add(1, std::memory_order_acq_rel);
            }
        });
    }

    int bad_rounds = 0;
    int no_winner_rounds = 0;
    for (int round = 1; round <= kRounds; ++round) {
        auto pair = MakePromise<int>();
        winners.store(0);
        done.store(0);
        slot_promise.store(&pair.first);
        slot_future.store(&pair.second);
        phase.store(static_cast<std::uint32_t>(round),
                    std::memory_order_release);
        if (!WaitForCount(done, kThreads, 3s)) {
            ++bad_rounds;
            break;
        }
        const int round_winners = winners.load();
        if (round_winners > 1) {
            ++bad_rounds;
        }
        if (round_winners == 0) {
            // 8 个线程全部随机选中只读操作的概率约 1/256，属正常情况；
            // 补一次完成，不把“本轮没有写者”算作失败。
            ++no_winner_rounds;
            (void)pair.first.SetValue(1);
        }
        const auto status = pair.second.Status();
        if (!IsTerminal(status)) {
            ++bad_rounds;
        } else {
            const auto result = pair.second.GetResultFor(0ms);
            if (result.status != status) {
                ++bad_rounds;
            }
            if (result.status == FutureStatus::kReady &&
                (!result.Value() || *result.Value() != 1)) {
                ++bad_rounds;
            }
        }
        slot_promise.store(nullptr);
        slot_future.store(nullptr);
    }
    stop.store(true, std::memory_order_release);
    JoinAll(workers);
    CHECK(bad_rounds == 0);
    CHECK(invariant_bad.load() == 0);
    std::printf("[opt] 随机 fuzz: rounds=%d threads=%d bad=%d no_winner=%d\n",
                kRounds, kThreads, bad_rounds, no_winner_rounds);
}

// ---------------------------------------------------------------------------
// 抢占竞态：4 线程用 4 种完成方式抢同一个 Promise。
// ---------------------------------------------------------------------------
void ScenarioPublishRace() {
    constexpr int kRounds = 500;
    constexpr int kWriters = 4;
    int winners_total = 0;
    int bad_state = 0;
    for (int round = 0; round < kRounds; ++round) {
        auto pair = MakePromise<int>();
        auto shared_promise =
            std::make_shared<Promise<int>>(std::move(pair.first));
        auto future = pair.second;
        std::atomic<int> winners{0};
        std::vector<std::thread> writers;
        for (int index = 0; index < kWriters; ++index) {
            writers.emplace_back([shared_promise, &winners, index] {
                bool ok = false;
                switch (index) {
                case 0:
                    ok = shared_promise->SetValue(3);
                    break;
                case 1:
                    ok = shared_promise->SetError(NewError("race"));
                    break;
                case 2:
                    ok = shared_promise->Cancel();
                    break;
                default:
                    try {
                        throw std::runtime_error("race");
                    } catch (...) {
                        ok = shared_promise->SetException(
                            std::current_exception());
                    }
                    break;
                }
                if (ok) {
                    winners.fetch_add(1, std::memory_order_acq_rel);
                }
            });
        }
        JoinAll(writers);
        winners_total += winners.load();
        const auto status = future.Status();
        if (!IsTerminal(status)) {
            ++bad_state;
        }
        if (!future.GetResultFor(0ms).Ready()) {
            ++bad_state;
        }
    }
    CHECK(winners_total == kRounds);
    CHECK(bad_state == 0);
    std::printf("[opt] 抢占竞态: rounds=%d winners=%d bad=%d\n", kRounds,
                winners_total, bad_state);
}

// ---------------------------------------------------------------------------
// 边界：零超时、已取消 Context 快路径、无效 Future。
// ---------------------------------------------------------------------------
void ScenarioBoundaries() {
    Future<int> invalid;
    CHECK(!invalid.valid());
    CHECK(invalid.Status() == FutureStatus::kInvalid);
    CHECK(invalid.Wait() == FutureStatus::kInvalid);
    CHECK(invalid.GetResult().status == FutureStatus::kInvalid);

    auto pair = MakePromise<int>();
    auto future = pair.second;
    CHECK(future.GetResultFor(0ms).TimedOut());
    CHECK(future.Status() == FutureStatus::kTimedOut);

    auto context_pair = WithCancel(Background());
    context_pair.second();
    CHECK(future.Wait(context_pair.first) == FutureStatus::kCancelled);

    auto deadline_pair = WithTimeout(Background(), 1ms);
    const auto deadline_status = future.Wait(deadline_pair.first);
    CHECK(deadline_status == FutureStatus::kDeadlineExceeded ||
          deadline_status == FutureStatus::kCancelled);

    CHECK(pair.first.SetValue(11));
    CHECK(!pair.first.SetValue(12));
    CHECK(!pair.first.SetError(NewError("late")));
    CHECK(!pair.first.Cancel());
    CHECK(future.Status() == FutureStatus::kReady);
    CHECK(future.GetResultFor(0ms).Ok());
    std::printf("[opt] 边界: 无效/零超时/已取消 Context/单次发布 ok\n");
}

}  // namespace

int main() {
    std::printf("== Future 无锁优化正确性测试 ==\n");
    ScenarioBoundaries();
    ScenarioRequestPipeline();
    ScenarioCancelTree();
    ScenarioTimeoutRetry();
    ScenarioMultiWaiter();
    ScenarioFailurePropagation();
    ScenarioFiber();
    ScenarioPublishRace();
    ScenarioRandomFuzz();

    if (g_failures.load() != 0) {
        std::printf("FAILED: %d 项检查失败\n", g_failures.load());
        return 1;
    }
    std::printf("ALL FUTURE OPT CHECKS PASSED\n");
    return 0;
}
