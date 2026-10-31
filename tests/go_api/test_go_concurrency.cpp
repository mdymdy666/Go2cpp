/**
 * @file test_go_concurrency.cpp
 * @brief `go2cpp/go/sync.go.hpp`、`chan.go.hpp`、`context.go.hpp`、
 *        `time.go.hpp`、`error.go.hpp` 的真实场景测试。
 *
 * 覆盖：Mutex/RWMutex/WaitGroup/Once/Cond、Chan 生产者消费者与 select、
 *       Context 取消/超时/Value、Timer/Ticker/After、errors.Is/As/Join。
 */

#include "go2cpp/go/go.hpp"
#include "go_api_check.hpp"
#include "go_api_util.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

/** 场景 1：Mutex + WaitGroup 并发计数。 */
void TestMutexAndWaitGroup() {
    GO_ANNOUNCE("sync: Mutex + WaitGroup");
    go::Mutex mutex;
    std::int64_t counter = 0;
    go::WaitGroup group;
    constexpr int kThreads = 4;
    constexpr int kIterations = 5000;

    group.Add(kThreads);
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int index = 0; index < kThreads; ++index) {
        threads.emplace_back([&] {
            for (int iteration = 0; iteration < kIterations; ++iteration) {
                mutex.Lock();
                ++counter;
                mutex.Unlock();
            }
            group.Done();
        });
    }
    group.Wait();
    for (auto& thread : threads) {
        thread.join();
    }
    GO_CHECK(counter == static_cast<std::int64_t>(kThreads) * kIterations);
    GO_CHECK(group.Count() == 0);
}

/** 场景 2：Once 只执行一次（多线程竞争）。 */
void TestOnce() {
    GO_ANNOUNCE("sync: Once.Do executes exactly once");
    go::Once once;
    std::atomic<int> executions{0};
    constexpr int kThreads = 8;
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int index = 0; index < kThreads; ++index) {
        threads.emplace_back([&] {
            once.Do([&] {
                executions.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::sleep_for(2ms);
            });
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    GO_CHECK(executions.load() == 1);

    // DoNoThrow：异常也算完成（贴近 Go 的 once 语义）。
    go::Once safe_once;
    std::atomic<int> calls{0};
    safe_once.DoNoThrow([&] {
        calls.fetch_add(1);
        throw std::runtime_error("boom");
    });
    safe_once.DoNoThrow([&] { calls.fetch_add(1); });
    GO_CHECK(calls.load() == 1);
}

/** 场景 3：Cond 唤醒等待者。 */
void TestCond() {
    GO_ANNOUNCE("sync: Cond wait/signal");
    go::Mutex mutex;
    go::Cond condition(mutex);
    bool ready = false;
    std::atomic<bool> woken{false};

    std::thread waiter([&] {
        mutex.Lock();
        while (!ready) {
            condition.Wait();
        }
        woken.store(true);
        mutex.Unlock();
    });

    std::this_thread::sleep_for(10ms);
    mutex.Lock();
    ready = true;
    mutex.Unlock();
    condition.Signal();
    waiter.join();
    GO_CHECK(woken.load());
}

/** 场景 4：RWMutex 读写互斥。 */
void TestRWMutex() {
    GO_ANNOUNCE("sync: RWMutex readers/writer");
    go::RWMutex mutex;
    int value = 0;
    std::atomic<int> reads{0};
    go::WaitGroup group;
    group.Add(4);

    std::vector<std::thread> threads;
    threads.reserve(4);
    for (int index = 0; index < 3; ++index) {
        threads.emplace_back([&] {
            for (int round = 0; round < 100; ++round) {
                mutex.RLock();
                const int snapshot = value;
                (void)snapshot;
                reads.fetch_add(1);
                mutex.RUnlock();
            }
            group.Done();
        });
    }
    threads.emplace_back([&] {
        for (int round = 0; round < 100; ++round) {
            mutex.Lock();
            ++value;
            mutex.Unlock();
        }
        group.Done();
    });

    group.Wait();
    for (auto& thread : threads) {
        thread.join();
    }
    GO_CHECK(value == 100);
    GO_CHECK(reads.load() == 300);
}

/** 场景 5：Chan 生产者消费者 + close + select default。 */
void TestChan() {
    GO_ANNOUNCE("chan: producer/consumer + close + select default");
    auto channel = go::Chan<int>::Make(4);
    constexpr int kProducers = 2;
    constexpr int kPerProducer = 1000;

    go::WaitGroup group;
    group.Add(kProducers);
    for (int producer = 0; producer < kProducers; ++producer) {
        std::thread([&, producer] {
            for (int index = 0; index < kPerProducer; ++index) {
                channel.Send(producer * kPerProducer + index);
            }
            group.Done();
        }).detach();
    }

    int received = 0;
    while (received < kProducers * kPerProducer) {
        auto value = go_api_test::RecvWithin(channel, 2 * go::Second);
        if (!value.has_value()) {
            break;
        }
        ++received;
    }
    GO_CHECK(received == kProducers * kPerProducer);
    group.Wait();

    channel.Close();
    int ignored = 0;
    GO_CHECK(!channel.Recv(ignored));
    GO_CHECK(!channel.Send(1));
    GO_CHECK(channel.IsClosed());
    GO_CHECK(!channel.Close());  // 重复关闭返回 false（Go 会 panic）

    // select 的 default 分支。
    auto empty = go::Chan<int>::Make(1);
    auto result = go::Select({empty.RecvCase(), go::DefaultCase()});
    GO_CHECK(result.selected);
    GO_CHECK(result.index == 1);

    // select 的 recv 分支。
    empty.Send(7);
    result = go::Select({empty.RecvCase(), go::DefaultCase()});
    GO_CHECK(result.index == 0);
    GO_CHECK(result.Value<int>().value_or(0) == 7);

    // TrySend/TryRecv。
    GO_CHECK(empty.TrySend(9));
    int out = 0;
    GO_CHECK(empty.TryRecv(out));
    GO_CHECK(out == 9);
}

/** 场景 6：Context 取消、超时、Value、DoneCase。 */
void TestContext() {
    GO_ANNOUNCE("context: WithCancel / WithTimeout / WithValue / DoneCase");
    auto [ctx, cancel] = go::WithCancel(go::Background());
    GO_CHECK(!go::Done(ctx).IsDone());
    GO_CHECK(go::Err(ctx) == nullptr);
    cancel();
    GO_CHECK(go::Done(ctx).IsDone());
    GO_CHECK(go::Err(ctx) != nullptr);

    auto timed = go::WithTimeout(go::Background(), 20 * go::Millisecond);
    GO_CHECK(go::WaitDone(timed.context, 1 * go::Second));
    GO_CHECK(go::Done(timed.context).IsDone());
    GO_CHECK(go::Err(timed.context) != nullptr);

    go2cpp::ContextKey<int> request_id("request-id");
    auto valued = go::WithValue(go::Background(), request_id, 42);
    GO_CHECK(go::Value(valued, request_id).value_or(0) == 42);
    GO_CHECK(!go::Value(valued, go2cpp::ContextKey<int>("other")).has_value());

    auto [done_ctx, done_cancel] = go::WithCancel(go::Background());
    done_cancel();
    auto result = go::Select({go::DoneCase(done_ctx), go::DefaultCase()});
    GO_CHECK(result.selected);
    GO_CHECK(result.index == 0);
}

/** 场景 7：time.After / Ticker / Timer / Sleep。 */
void TestTime() {
    GO_ANNOUNCE("time: AfterChan / NewTicker / NewTimer / Sleep");
    auto after = go::AfterChan(20 * go::Millisecond);
    auto fired = go_api_test::RecvWithin(after, 1 * go::Second);
    GO_CHECK(fired.has_value());

    auto ticker = go::NewTicker(10 * go::Millisecond);
    GO_REQUIRE(ticker != nullptr);
    auto first = go_api_test::RecvWithin(ticker->C(), 1 * go::Second);
    auto second = go_api_test::RecvWithin(ticker->C(), 1 * go::Second);
    GO_CHECK(first.has_value());
    GO_CHECK(second.has_value());
    ticker->Stop();
    GO_CHECK(!ticker->Active());

    auto timer = go::NewTimer(30 * go::Millisecond);
    timer->Stop();
    GO_CHECK(go_api_test::RecvWithin(timer->C(), 100 * go::Millisecond) ==
             std::nullopt);

    std::atomic<int> after_func_calls{0};
    auto after_func = go::AfterFunc(10 * go::Millisecond,
                                    [&] { after_func_calls.fetch_add(1); });
    GO_CHECK(after_func != nullptr);
    std::this_thread::sleep_for(150ms);
    GO_CHECK(after_func_calls.load() == 1);

    const auto start = go::Now();
    go::Sleep(10 * go::Millisecond);
    GO_CHECK(go::Since(start) >= 8 * go::Millisecond);

    // select + time.After 的超时分支。
    auto empty = go::Chan<int>::Make(1);
    const auto select_start = std::chrono::steady_clock::now();
    auto result =
        go::Select({empty.RecvCase(), go::AfterCase(30 * go::Millisecond)});
    const auto elapsed = std::chrono::steady_clock::now() - select_start;
    GO_CHECK(result.selected);
    GO_CHECK(result.index == 1);
    GO_CHECK(elapsed >= 10ms);
}

/** 场景 8：errors.New/Wrap/Is/As/Join。 */
void TestErrors() {
    GO_ANNOUNCE("errors: New / Wrap / Is / As / Join");
    auto base = go::New("base failure");
    auto wrapped = go::Wrap(base, "outer context");
    GO_CHECK(wrapped != nullptr);
    GO_CHECK(go::Is(wrapped, base));
    GO_CHECK(go::ErrorString(wrapped).find("outer context") != std::string::npos);
    GO_CHECK(go::ErrorString(wrapped).find("base failure") != std::string::npos);
    GO_CHECK(go::Wrap(wrapped, "top") != nullptr);
    GO_CHECK(go::Is(go::Wrap(wrapped, "top"), base));

    auto joined = go::Join({base, go::New("second")});
    GO_CHECK(go::Is(joined, base));
    GO_CHECK(go::ErrorString(joined).find("base failure") != std::string::npos);

    class CustomError final : public go2cpp::Error {
    public:
        std::string Message() const override { return "custom"; }
    };
    auto custom = std::make_shared<CustomError>();
    auto custom_wrapped = go::Wrap(custom, "wrapped custom");
    GO_CHECK(go::As<CustomError>(custom_wrapped) != nullptr);
    GO_CHECK(go::As<CustomError>(wrapped) == nullptr);
    GO_CHECK(go::Is(go::ErrClosed(), go::ErrClosed()));
    GO_CHECK(!go::Is(wrapped, go::ErrClosed()));
    GO_CHECK(go::HasError(wrapped));
    GO_CHECK(!go::HasError(nullptr));
}

}  // namespace

int main() {
    TestMutexAndWaitGroup();
    TestOnce();
    TestCond();
    TestRWMutex();
    TestChan();
    TestContext();
    TestTime();
    TestErrors();
    return go_api_test::ExitCode();
}
