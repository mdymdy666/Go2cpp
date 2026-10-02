#include "go2cpp/context.hpp"

#include <chrono>
#include <iostream>

/**
 * @brief 演示 ContextRollback 的作用域回滚、保存点和提交语义。
 * @details 未提交的撤销动作会在作用域结束时执行；提交后的 Context 仍可
 *          继续接收父 Context 的取消通知，但已提交资源不会重复回滚。
 * @return 所有回滚断言通过时返回 0，否则返回 1。
 */
int main() {
    using namespace go2cpp;
    using namespace std::chrono_literals;

    const auto parent = Background();
    int live_resources = 0;

    // 新手写法：scope 离开作用域时会自动回滚尚未提交的动作。
    {
        ContextRollback scope(parent);
        ++live_resources;
        scope.record_undo([&live_resources]() noexcept { --live_resources; });

        const auto mark = scope.savepoint();
        ++live_resources;
        scope.record_undo([&live_resources]() noexcept { --live_resources; });
        if (!scope.rollback_to(mark)) {
            std::cerr << "savepoint rollback failed\n";
            return 1;
        }
        if (live_resources != 1) {
            std::cerr << "unexpected resource count after savepoint\n";
            return 1;
        }
    }
    if (live_resources != 0) {
        std::cerr << "scope did not roll back\n";
        return 1;
    }

    // 提交会丢弃补偿动作，但 child Context 仍可交给下游使用。
    auto parent_cancel = WithCancel(parent);
    auto committed = WithRollback(parent_cancel.first);
    ++live_resources;
    committed.record_undo([&live_resources]() noexcept { --live_resources; });
    if (!committed.commit() || !committed.rollback_done().WaitFor(1s)) {
        std::cerr << "commit failed\n";
        return 1;
    }
    parent_cancel.second();
    if (!committed.context()->Done().WaitFor(1s) || live_resources != 1) {
        std::cerr << "committed context behaved unexpectedly\n";
        return 1;
    }

    std::cout << "context rollback demo passed\n";
    return 0;
}
