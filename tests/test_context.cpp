#include "go2cpp/context.hpp"
#include "go2cpp/scheduler.hpp"
#include "test_support.hpp"

#include <atomic>
#include <chrono>
#include <stdexcept>
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
    std::atomic<bool> done_wait_result{false};
    std::thread waiter([&] {
        done_wait_result.store(child->Done().WaitFor(2s),
                               std::memory_order_release);
        done_waiter.store(true, std::memory_order_release);
    });
    std::this_thread::sleep_for(2ms);
    child_pair.second();
    child_pair.second();
    GO2CPP_JOIN_WITH_WATCHDOG(waiter, 3s);
    GO2CPP_CHECK(done_waiter.load(std::memory_order_acquire));
    GO2CPP_CHECK(done_wait_result.load(std::memory_order_acquire));
    GO2CPP_CHECK(child->IsDone());
    GO2CPP_CHECK(Is(child->Err(), CanceledError()));
    GO2CPP_CHECK(Is(child->Cause(), CanceledError()));

    const auto parent_pair = WithCancel(root);
    const auto grandchild_pair = WithCancel(parent_pair.first);
    parent_pair.second();
    GO2CPP_CHECK(grandchild_pair.first->Done().WaitFor(100ms));
    GO2CPP_CHECK(Is(grandchild_pair.first->Err(), CanceledError()));

    // Descendant cancellation is committed before parent callbacks run. A
    // callback may therefore synchronously inspect/wait on a child without
    // deadlocking the cancellation caller.
    const auto callback_parent = WithCancel(root);
    const auto callback_child = WithCancel(callback_parent.first);
    std::atomic<bool> callback_waited{false};
    callback_parent.first->Done().AddCallback([&] {
        callback_waited.store(callback_child.first->Done().WaitFor(100ms),
                              std::memory_order_release);
    });
    callback_parent.second();
    GO2CPP_CHECK(callback_waited.load(std::memory_order_acquire));

    const auto deadline_pair = WithTimeout(root, 20ms);
    GO2CPP_CHECK(!deadline_pair.first->Done().IsDone());
    GO2CPP_CHECK(deadline_pair.first->Done().WaitFor(500ms));
    GO2CPP_CHECK(Is(deadline_pair.first->Err(), DeadlineExceededError()));
    deadline_pair.second();

    const auto already_expired =
        WithDeadline(root, Context::Clock::now() - 1s);
    GO2CPP_CHECK(already_expired.first->IsDone());
    GO2CPP_CHECK(Is(already_expired.first->Err(), DeadlineExceededError()));

    // Extreme relative deadlines must saturate instead of wrapping into the
    // past and cancelling a context that was intended to live indefinitely.
    const auto enormous = WithTimeout(root, ContextDuration::max());
    GO2CPP_CHECK(enormous.first->Deadline() == ContextTimePoint::max());
    GO2CPP_CHECK(!enormous.first->IsDone());
    enormous.second();
    const auto immediate = WithTimeout(root, ContextDuration::min());
    GO2CPP_CHECK(immediate.first->IsDone());
    GO2CPP_CHECK(Is(immediate.first->Err(), DeadlineExceededError()));

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
        GO2CPP_JOIN_WITH_WATCHDOG(thread, 3s);
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

    // Done().Wait/WaitFor must park a managed G. With one P the cancelling
    // sibling can only run if the waiter releases its M instead of entering
    // a native condition_variable wait.
    Scheduler managed(1);
    managed.start();
    auto managed_cancel = WithCancel(Background());
    std::atomic<bool> managed_waited{false};
    std::atomic<bool> managed_result{false};
    auto managed_waiter = managed.spawn([&] {
        managed_waited.store(true, std::memory_order_release);
        managed_result.store(managed_cancel.first->Done().WaitFor(1s),
                             std::memory_order_release);
    });
    const auto managed_deadline = std::chrono::steady_clock::now() + 1s;
    while (!managed_waited.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < managed_deadline) {
        go2cpp_tests::yield_for_watchdog();
    }
    auto managed_canceller = managed.spawn([&] { managed_cancel.second(); });
    GO2CPP_CHECK(managed_waiter->wait_for(2s));
    GO2CPP_CHECK(managed_canceller->wait_for(2s));
    GO2CPP_CHECK(managed_result.load(std::memory_order_acquire));

    // RollbackDone 也必须让 managed G 让出 M；否则 P=1 时执行 rollback
    // 的兄弟任务无法运行，等待者会把整个调度器卡住。
    ContextRollback managed_scope(Background(), false);
    std::atomic<int> managed_undo_count{0};
    std::atomic<bool> managed_rollback_waited{false};
    GO2CPP_CHECK(managed_scope.record_undo([&] {
        managed_undo_count.fetch_add(1, std::memory_order_release);
    }));
    auto managed_rollback_waiter = managed.spawn([&] {
        managed_rollback_waited.store(
            managed_scope.rollback_done().WaitFor(1s),
            std::memory_order_release);
    });
    auto managed_rollback_worker = managed.spawn([&] {
        (void)managed_scope.rollback();
    });
    GO2CPP_CHECK(managed_rollback_waiter->wait_for(2s));
    GO2CPP_CHECK(managed_rollback_worker->wait_for(2s));
    GO2CPP_CHECK(managed_rollback_waited.load(std::memory_order_acquire));
    GO2CPP_CHECK(managed_undo_count.load(std::memory_order_acquire) == 1);
    managed.shutdown();

    // ContextRollback 只取消临时 child，不恢复 parent；undo 在锁外按 LIFO
    // 执行，RollbackDone 与 child.Done 分别表示补偿完成和取消线性化。
    {
        ContextRollback scope(root, false);
        const auto child_scope = scope.context();
        std::vector<int> order;
        GO2CPP_CHECK(scope.record_undo([&] { order.push_back(1); }));
        const auto mark = scope.savepoint();
        GO2CPP_CHECK(mark.valid());
        GO2CPP_CHECK(scope.record_undo([&] { order.push_back(2); }));
        GO2CPP_CHECK(scope.record_undo([&] { order.push_back(3); }));
        GO2CPP_CHECK(scope.rollback_to(mark));
        GO2CPP_CHECK(order.size() == 2 && order[0] == 3 && order[1] == 2);
        GO2CPP_CHECK(scope.active());
        GO2CPP_CHECK(scope.record_undo([&] { order.push_back(4); }));
        GO2CPP_CHECK(scope.rollback());
        GO2CPP_CHECK(scope.rollback_done().WaitFor(1s));
        GO2CPP_CHECK(order.size() == 4 && order[2] == 4 && order[3] == 1);
        GO2CPP_CHECK(scope.rolled_back());
        GO2CPP_CHECK(!mark.valid());
        GO2CPP_CHECK(child_scope->IsDone());
        GO2CPP_CHECK(Is(child_scope->Err(), CanceledError()));
        GO2CPP_CHECK(!root->IsDone());
        GO2CPP_CHECK(scope.rollback());
    }

    // 未提交的 scope 析构会自动回滚；父取消也会触发同一条路径。
    {
        auto parent_pair = WithCancel(root);
        ContextRollback scope(parent_pair.first);
        const auto child_scope = scope.context();
        std::atomic<int> undo_count{0};
        std::atomic<bool> undo_observed_done{false};
        GO2CPP_CHECK(scope.record_undo(
            [&] {
                undo_observed_done.store(child_scope->Done().WaitFor(100ms),
                                         std::memory_order_release);
                undo_count.fetch_add(1, std::memory_order_relaxed);
            }));
        parent_pair.second();
        GO2CPP_CHECK(child_scope->Done().WaitFor(1s));
        GO2CPP_CHECK(scope.rollback_done().WaitFor(1s));
        GO2CPP_CHECK(undo_observed_done.load(std::memory_order_acquire));
        GO2CPP_CHECK(undo_count.load(std::memory_order_relaxed) == 1);
        GO2CPP_CHECK(scope.rolled_back());
    }

    // parent 在构造前已经取消时，两个回调都走立即执行分支；scope 必须
    // 直接进入完成态，不能接受一个永远不会执行的新动作。
    {
        auto cancelled_parent = WithCancel(root);
        cancelled_parent.second();
        ContextRollback scope(cancelled_parent.first);
        GO2CPP_CHECK(scope.context()->Done().IsDone());
        GO2CPP_CHECK(scope.rollback_done().IsDone());
        GO2CPP_CHECK(scope.rolled_back());
        GO2CPP_CHECK(!scope.record_undo([] {}));
    }

    // Done 观察者运行时，rollback action 日志已经冻结；观察者不能在取消
    // 窗口中追加一个会逃过回滚的新动作。
    {
        auto parent_pair = WithCancel(root);
        ContextRollback scope(parent_pair.first);
        std::atomic<int> undo_count{0};
        std::atomic<bool> late_record_accepted{true};
        std::atomic<bool> done_callback_waited{false};
        std::atomic<bool> rollback_done_callback_waited{false};
        GO2CPP_CHECK(scope.record_undo(
            [&] { undo_count.fetch_add(1, std::memory_order_relaxed); }));
        scope.rollback_done().AddCallback([&] {
            rollback_done_callback_waited.store(
                scope.context()->Done().WaitFor(100ms),
                std::memory_order_release);
        });
        scope.context()->Done().AddCallback([&] {
            late_record_accepted.store(
                scope.record_undo([&] {
                    undo_count.fetch_add(10, std::memory_order_relaxed);
                }),
                std::memory_order_release);
            done_callback_waited.store(scope.rollback_done().WaitFor(1s),
                                       std::memory_order_release);
        });
        parent_pair.second();
        GO2CPP_CHECK(scope.rollback_done().WaitFor(1s));
        GO2CPP_CHECK(!late_record_accepted.load(std::memory_order_acquire));
        GO2CPP_CHECK(done_callback_waited.load(std::memory_order_acquire));
        GO2CPP_CHECK(
            rollback_done_callback_waited.load(std::memory_order_acquire));
        GO2CPP_CHECK(undo_count.load(std::memory_order_relaxed) == 1);
    }

    // commit 丢弃 undo，但不取消已经发布给下游的 child；之后父取消仍会
    // 正常传播，但不会重新执行已提交的补偿动作。
    {
        auto parent_pair = WithCancel(root);
        ContextRollback scope(parent_pair.first);
        const auto child_scope = scope.context();
        std::atomic<int> undo_count{0};
        GO2CPP_CHECK(scope.record_undo(
            [&] { undo_count.fetch_add(1, std::memory_order_relaxed); }));
        GO2CPP_CHECK(scope.commit());
        GO2CPP_CHECK(scope.rollback_done().IsDone());
        GO2CPP_CHECK(!child_scope->IsDone());
        parent_pair.second();
        GO2CPP_CHECK(child_scope->Done().WaitFor(1s));
        GO2CPP_CHECK(undo_count.load(std::memory_order_relaxed) == 0);
        GO2CPP_CHECK(!scope.rollback());
    }

    // undo 异常不会跳出取消线程，后续动作仍然执行，最终状态单独标记失败。
    {
        ContextRollback scope(root, false);
        const auto child_scope = scope.context();
        std::atomic<int> completed{0};
        std::atomic<bool> undo_observed_done{false};
        GO2CPP_CHECK(scope.record_undo([&] {
            undo_observed_done.store(child_scope->Done().WaitFor(100ms),
                                     std::memory_order_release);
            completed.fetch_add(1, std::memory_order_relaxed);
            throw std::runtime_error("rollback action");
        }));
        GO2CPP_CHECK(scope.record_undo(
            [&] { completed.fetch_add(10, std::memory_order_relaxed); }));
        GO2CPP_CHECK(scope.rollback());
        GO2CPP_CHECK(scope.rollback_done().WaitFor(1s));
        GO2CPP_CHECK(undo_observed_done.load(std::memory_order_acquire));
        GO2CPP_CHECK(completed.load(std::memory_order_relaxed) == 11);
        GO2CPP_CHECK(scope.had_failure());
        GO2CPP_CHECK(scope.status() == ContextRollback::Status::kFailed);
        GO2CPP_CHECK(scope.failure() != nullptr);
    }

    // commit 与 rollback 竞争时只有一个终态获胜，undo 至多执行一次。
    {
        ContextRollback scope(root, false);
        std::atomic<int> undo_count{0};
        GO2CPP_CHECK(scope.record_undo(
            [&] { undo_count.fetch_add(1, std::memory_order_relaxed); }));
        std::thread committer([&] { (void)scope.commit(); });
        std::thread rollbacker([&] { (void)scope.rollback(); });
        GO2CPP_JOIN_WITH_WATCHDOG(committer, 2s);
        GO2CPP_JOIN_WITH_WATCHDOG(rollbacker, 2s);
        GO2CPP_CHECK(!scope.active());
        GO2CPP_CHECK(undo_count.load(std::memory_order_relaxed) <= 1);
        GO2CPP_CHECK(scope.rollback_done().WaitFor(1s));
    }

    // 父取消与显式 rollback 同时发生时，前置 claim/Done 回调不能丢失
    // pending undo，也不能让 RollbackDone 永久不完成。
    for (int i = 0; i < 32; ++i) {
        auto parent_pair = WithCancel(root);
        ContextRollback scope(parent_pair.first);
        std::atomic<int> undo_count{0};
        GO2CPP_CHECK(scope.record_undo(
            [&] { undo_count.fetch_add(1, std::memory_order_relaxed); }));
        std::thread parent_canceller([cancel = parent_pair.second] { cancel(); });
        std::thread explicit_rollback([&scope] { (void)scope.rollback(); });
        GO2CPP_JOIN_WITH_WATCHDOG(parent_canceller, 2s);
        GO2CPP_JOIN_WITH_WATCHDOG(explicit_rollback, 2s);
        GO2CPP_CHECK(scope.rollback_done().WaitFor(1s));
        GO2CPP_CHECK(undo_count.load(std::memory_order_relaxed) <= 1);
    }

    // commit 与 parent cancel 的线性化顺序决定结果：先 commit 则 undo 被
    // 丢弃，先 claim 取消则 undo 必须完成；两条路径都要完成通知。
    for (int i = 0; i < 32; ++i) {
        auto parent_pair = WithCancel(root);
        ContextRollback scope(parent_pair.first);
        std::atomic<int> undo_count{0};
        GO2CPP_CHECK(scope.record_undo(
            [&] { undo_count.fetch_add(1, std::memory_order_relaxed); }));
        std::thread parent_canceller([cancel = parent_pair.second] { cancel(); });
        std::thread committer([&scope] { (void)scope.commit(); });
        GO2CPP_JOIN_WITH_WATCHDOG(parent_canceller, 2s);
        GO2CPP_JOIN_WITH_WATCHDOG(committer, 2s);
        GO2CPP_CHECK(scope.rollback_done().WaitFor(1s));
        GO2CPP_CHECK(undo_count.load(std::memory_order_relaxed) <= 1);
        GO2CPP_CHECK(!scope.active());
    }

    // 移动只转移事务所有权；源对象变为空对象，目标对象仍负责回滚。
    {
        std::atomic<int> undo_count{0};
        ContextRollback source(root, false);
        const auto child_scope = source.context();
        GO2CPP_CHECK(source.record_undo(
            [&] { undo_count.fetch_add(1, std::memory_order_relaxed); }));
        ContextRollback moved(std::move(source));
        GO2CPP_CHECK(!source.rollback());
        GO2CPP_CHECK(!child_scope->IsDone());
        GO2CPP_CHECK(moved.rollback());
        GO2CPP_CHECK(moved.rollback_done().WaitFor(1s));
        GO2CPP_CHECK(undo_count.load(std::memory_order_relaxed) == 1);
        GO2CPP_CHECK(child_scope->IsDone());

        std::atomic<int> assigned_count{0};
        ContextRollback destination(root, false);
        ContextRollback incoming(root, false);
        GO2CPP_CHECK(destination.record_undo([&] {
            assigned_count.fetch_add(10, std::memory_order_relaxed);
        }));
        GO2CPP_CHECK(incoming.record_undo([&] {
            assigned_count.fetch_add(1, std::memory_order_relaxed);
        }));
        destination = std::move(incoming);
        GO2CPP_CHECK(assigned_count.load(std::memory_order_relaxed) == 10);
        GO2CPP_CHECK(destination.rollback());
        GO2CPP_CHECK(destination.rollback_done().WaitFor(1s));
        GO2CPP_CHECK(assigned_count.load(std::memory_order_relaxed) == 11);
    }

    // 关闭自动回滚后，Context 取消只结束 child；调用方仍可稍后显式回滚。
    {
        auto parent_pair = WithTimeout(root, 30ms);
        ContextRollback scope(parent_pair.first, false);
        std::atomic<int> undo_count{0};
        GO2CPP_CHECK(scope.record_undo(
            [&] { undo_count.fetch_add(1, std::memory_order_relaxed); }));
        GO2CPP_CHECK(scope.context()->Done().WaitFor(2s));
        GO2CPP_CHECK(Is(scope.context()->Err(), DeadlineExceededError()));
        GO2CPP_CHECK(scope.active());
        GO2CPP_CHECK(!scope.rollback_done().IsDone());
        GO2CPP_CHECK(undo_count.load(std::memory_order_relaxed) == 0);
        GO2CPP_CHECK(scope.record_undo(
            [&] { undo_count.fetch_add(10, std::memory_order_relaxed); }));
        GO2CPP_CHECK(scope.rollback());
        GO2CPP_CHECK(scope.rollback_done().WaitFor(1s));
        GO2CPP_CHECK(undo_count.load(std::memory_order_relaxed) == 11);
        parent_pair.second();
    }

    // Deadline 取消会走自动回滚路径，且 deadline 错误仍保留在 child Context。
    {
        auto parent_pair = WithTimeout(root, 30ms);
        ContextRollback scope(parent_pair.first);
        std::atomic<int> undo_count{0};
        GO2CPP_CHECK(scope.record_undo(
            [&] { undo_count.fetch_add(1, std::memory_order_relaxed); }));
        GO2CPP_CHECK(scope.context()->Done().WaitFor(2s));
        GO2CPP_CHECK(scope.rollback_done().WaitFor(2s));
        GO2CPP_CHECK(scope.rolled_back());
        GO2CPP_CHECK(Is(scope.context()->Err(), DeadlineExceededError()));
        GO2CPP_CHECK(undo_count.load(std::memory_order_relaxed) == 1);
        parent_pair.second();
    }

    // Savepoint 只属于创建它的事务，不能跨事务误用。
    {
        ContextRollback first(root, false);
        ContextRollback second(root, false);
        std::atomic<int> first_count{0};
        std::atomic<int> second_count{0};
        GO2CPP_CHECK(first.record_undo(
            [&] { first_count.fetch_add(1, std::memory_order_relaxed); }));
        const auto mark = first.savepoint();
        GO2CPP_CHECK(second.record_undo(
            [&] { second_count.fetch_add(1, std::memory_order_relaxed); }));
        GO2CPP_CHECK(!second.rollback_to(mark));
        GO2CPP_CHECK(first.active());
        GO2CPP_CHECK(second.active());
        GO2CPP_CHECK(first.rollback());
        GO2CPP_CHECK(second.rollback());
        GO2CPP_CHECK(first_count.load(std::memory_order_relaxed) == 1);
        GO2CPP_CHECK(second_count.load(std::memory_order_relaxed) == 1);
    }

    // 局部回滚后即使动作数量恢复到旧深度，旧 savepoint 也不能因 ABA
    // 重新生效。
    {
        ContextRollback scope(root, false);
        GO2CPP_CHECK(scope.record_undo([] {}));
        const auto stale = scope.savepoint();
        GO2CPP_CHECK(scope.record_undo([] {}));
        GO2CPP_CHECK(scope.rollback_to(stale));
        GO2CPP_CHECK(!stale.valid());
        GO2CPP_CHECK(scope.record_undo([] {}));
        GO2CPP_CHECK(!scope.rollback_to(stale));
        GO2CPP_CHECK(scope.rollback());
    }
}
