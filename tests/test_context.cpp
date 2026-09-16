#include "go2cpp/context.hpp"
#include "test_support.hpp"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

void run_context_tests() {
    go2cpp_tests::announce("context cancellation, values and deadlines");
    using namespace go2cpp;
    using namespace std::chrono_literals;

    const auto root = Background();
    root->Cancel();
    GO2CPP_CHECK(!root->IsDone());
    GO2CPP_CHECK(!TODO()->IsDone());
    const auto empty_key_context = WithValue(root, "", 7);
    GO2CPP_CHECK(std::any_cast<int>(empty_key_context->Value("")) == 7);
    GO2CPP_CHECK(!empty_key_context->Value(ContextKey<int>{}).has_value());
    ContextKey<int> key("request-id");
    const auto valued = WithValue(root, key, 42);
    const auto child_pair = WithCancel(valued);
    const auto child = child_pair.first;
    GO2CPP_CHECK(child->Value(key).has_value());
    GO2CPP_CHECK(*child->Value(key) == 42);
    GO2CPP_CHECK(!child->IsDone());

    // The key's identity allocation is retained by the context.  A later key
    // must not alias it even if the allocator would otherwise reuse an address.
    const auto retained_key_context = WithValue(root, ContextKey<int>{}, 7);
    for (int i = 0; i < 64; ++i) {
        ContextKey<int> unrelated_key;
        GO2CPP_CHECK(!retained_key_context->Value(unrelated_key).has_value());
    }

    std::atomic<bool> done_waiter{false};
    std::thread waiter([&] {
        child->Done().Wait();
        done_waiter.store(true, std::memory_order_release);
    });
    std::this_thread::sleep_for(2ms);
    child_pair.second();
    child_pair.second();
    waiter.join();
    GO2CPP_CHECK(done_waiter.load(std::memory_order_acquire));
    GO2CPP_CHECK(child->IsDone());
    GO2CPP_CHECK(Is(child->Err(), CanceledError()));
    GO2CPP_CHECK(Is(child->Cause(), CanceledError()));

    const auto parent_pair = WithCancel(root);
    const auto grandchild_pair = WithCancel(parent_pair.first);
    parent_pair.second();
    GO2CPP_CHECK(grandchild_pair.first->Done().WaitFor(100ms));
    GO2CPP_CHECK(Is(grandchild_pair.first->Err(), CanceledError()));

    const auto deadline_pair = WithTimeout(root, 20ms);
    GO2CPP_CHECK(!deadline_pair.first->Done().IsDone());
    GO2CPP_CHECK(deadline_pair.first->Done().WaitFor(500ms));
    GO2CPP_CHECK(Is(deadline_pair.first->Err(), DeadlineExceededError()));
    deadline_pair.second();

    const auto already_expired =
        WithDeadline(root, Context::Clock::now() - 1s);
    GO2CPP_CHECK(already_expired.first->IsDone());
    GO2CPP_CHECK(Is(already_expired.first->Err(), DeadlineExceededError()));

    const auto fake_now = Context::Clock::now() + 10s;
    Context::SetNowFunctionForTesting([fake_now] { return fake_now; });
    const auto fake_expired =
        WithDeadline(root, Context::Clock::now() - 1s);
    Context::ResetNowFunctionForTesting();
    GO2CPP_CHECK(fake_expired.first->IsDone());
    GO2CPP_CHECK(Is(fake_expired.first->Err(), DeadlineExceededError()));

    // Child retains a strong parent anchor, so dropping the parent handle does
    // not sever cancellation/value propagation.
    auto retained_pair = WithCancel(valued);
    auto retained = retained_pair.first;
    retained_pair.second();
    GO2CPP_CHECK(retained->Done().IsDone());

    // Concurrent cancellation is first-wins and always wakes all observers.
    const auto race_pair = WithCancel(root);
    std::vector<std::thread> cancellers;
    for (int i = 0; i < 8; ++i) {
        cancellers.emplace_back([cancel = race_pair.second] { cancel(); });
    }
    for (auto& thread : cancellers) {
        thread.join();
    }
    GO2CPP_CHECK(race_pair.first->Done().IsDone());

    // Cancellation propagation is iterative, so a deeply nested context tree
    // cannot exhaust the native C++ call stack.
    auto deep_pair = WithCancel(root);
    auto deep_leaf = deep_pair.first;
    for (int i = 0; i < 20000; ++i) {
        deep_leaf = WithCancel(deep_leaf).first;
    }
    deep_pair.second();
    GO2CPP_CHECK(deep_leaf->Done().WaitFor(2s));
    GO2CPP_CHECK(Is(deep_leaf->Err(), CanceledError()));
}
