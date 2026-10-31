#include "go2cpp/runtime.hpp"

#include <chrono>
#include <iostream>
#include <string>
#include <vector>

// Context 全面版：同一个组件的 6 个使用场景。
//   1  WithCancel        父级取消级联到整棵子树
//   2  WithTimeout       到点自动取消，Err() == DeadlineExceededError()
//   3  WithValue         类型化键沿父链查找，子级覆盖父级
//   4  WithCancelCause   Err() 与 Cause() 的区别（直接原因 vs 最初根因）
//   5  Background()      根上下文免疫取消，Cancd 是幂等的
//   6  Done() 等待       用 DoneSignal 阻塞等待完成，而不是轮询 IsDone()
//
// 每个场景独立成段；校验失败只记 ok=false，最后统一报告。

namespace {

using Ms = std::chrono::milliseconds;

bool scenario_cancel_cascade() {
    using namespace go2cpp;
    std::clog << "[full][场景1] WithCancel：父取消级联到整棵子树\n";
    const auto root = Background();
    auto level1 = WithCancel(root);
    auto level2 = WithCancel(level1.first);

    if (level2.first->IsDone()) {
        return false;                          // 取消前不应完成
    }
    level1.second();                           // 只取消中间层
    const bool cascade = level2.first->IsDone() && level1.first->IsDone();
    std::cout << "  两层都完成=" << cascade << "（预期 1）\n";
    return cascade;
}

bool scenario_timeout() {
    using namespace go2cpp;
    std::clog << "[full][场景2] WithTimeout：到点自动取消\n";
    const auto root = Background();
    auto pair = WithTimeout(root, Ms(20));
    const bool done = pair.first->Done().WaitFor(Ms(500));
    const bool deadline = Is(pair.first->Err(), DeadlineExceededError());
    // 主动再取消一次：应当是幂等的，不会改变已是 DeadlineExceeded 的状态
    pair.second();
    const bool still_deadline = Is(pair.first->Err(), DeadlineExceededError());
    std::cout << "  到期=" << done << " 是 DeadlineExceeded=" << deadline
              << " 重复取消后仍为 DeadlineExceeded=" << still_deadline
              << "（预期 1 1 1）\n";
    return done && deadline && still_deadline;
}

bool scenario_value_lookup() {
    using namespace go2cpp;
    std::clog << "[full][场景3] WithValue：类型化键沿父链查找并可覆盖\n";
    const auto root = Background();
    const ContextKey<std::string> kTrace("trace-id");
    const ContextKey<int> kAttempt("attempt");

    const auto base = WithValue(root, kTrace, std::string("t-001"));
    const auto middle = WithValue(base, kAttempt, 3);
    const auto leaf = WithValue(middle, kTrace, std::string("t-002"));  // 覆盖

    const auto leaf_trace = leaf->Value(kTrace);      // 自己的覆盖值
    const auto leaf_attempt = leaf->Value(kAttempt);  // 从父链继承
    const auto middle_trace = middle->Value(kTrace);  // 父级不受影响
    const auto none = leaf->Value(ContextKey<double>("absent"));

    const bool ok = leaf_trace && *leaf_trace == "t-002" &&
                    leaf_attempt && *leaf_attempt == 3 &&
                    middle_trace && *middle_trace == "t-001" &&
                    !none.has_value();
    std::cout << "  leaf.trace=" << (leaf_trace ? *leaf_trace : "<无>")
              << " leaf.attempt=" << (leaf_attempt ? *leaf_attempt : -1)
              << " middle.trace=" << (middle_trace ? *middle_trace : "<无>")
              << " 未设置的键为空=" << !none.has_value() << "（预期 t-002 3 t-001 1）\n";
    return ok;
}

bool scenario_err_vs_cause() {
    using namespace go2cpp;
    std::clog << "[full][场景4] WithCancelCause：Err() 是直接原因，Cause() 是最初根因\n";
    const auto root = Background();
    auto parent_pair = WithCancelCause(root);
    const auto child_pair = WithCancel(parent_pair.first);   // 普通 WithCancel 子级

    parent_pair.second(NewError("下游连接被重置"));           // 带根因地取消

    const auto parent_err = parent_pair.first->Err();
    const auto parent_cause = parent_pair.first->Cause();
    const auto child_cause = child_pair.first->Cause();

    // 两级的 Err() 都是 Canceled（表示"被取消"这件事），
    // 但 Cause() 都指向最初那个根因。
    const bool ok = Is(parent_err, CanceledError()) &&
                    parent_cause != nullptr &&
                    child_cause != nullptr &&
                    Is(child_pair.first->Err(), CanceledError());
    std::cout << "  parent.Err 是 Canceled=" << Is(parent_err, CanceledError())
              << " Cause 非空=" << (parent_cause != nullptr)
              << " 子的 Cause 继承根因=" << (child_cause != nullptr)
              << "（预期 1 1 1）\n";
    return ok;
}

bool scenario_background_is_immune() {
    using namespace go2cpp;
    std::clog << "[full][场景5] Background()：根上下文免疫取消\n";
    const auto root = Background();
    const bool before = root->IsDone();
    // Background 没有返回取消函数，无法被取消；它的 Done 也永不完成。
    const bool not_done = !root->Done().WaitFor(Ms(30));
    std::cout << "  取消前 IsDone=" << before
              << " 等待 30ms 仍未完成=" << not_done << "（预期 0 1）\n";
    return !before && not_done;
}

bool scenario_wait_done_signal() {
    using namespace go2cpp;
    std::clog << "[full][场景6] Done()：用信号等待完成，而不是轮询 IsDone()\n";
    const auto root = Background();
    auto pair = WithCancel(root);

    // 从另一个执行流里稍后取消，主执行流阻塞在 Done() 上。
    const bool done = [&] {
        Scheduler scheduler(1);
        scheduler.start();
        auto canceller = go(scheduler, [cancel = pair.second] {
            std::this_thread::sleep_for(Ms(20));
            cancel();
        });
        const bool signalled = pair.first->Done().WaitFor(Ms(500));
        canceller->wait();
        scheduler.shutdown();
        return signalled;
    }();

    const bool cancelled = Is(pair.first->Err(), CanceledError());
    std::cout << "  信号到达=" << done << " Err 是 Canceled=" << cancelled
              << "（预期 1 1）\n";
    return done && cancelled;
}

}  // namespace

/**
 * @brief Context 全面版：6 个使用场景，覆盖取消树、超时、取值、根因与等待信号。
 * @details 每个场景一个函数，独立可读；全部通过才返回 0。
 * @return 全部场景通过返回 0，否则返回 1。
 */
int main() {
    using namespace go2cpp;

    bool ok = true;
    ok = scenario_cancel_cascade() && ok;
    ok = scenario_timeout() && ok;
    ok = scenario_value_lookup() && ok;
    ok = scenario_err_vs_cause() && ok;
    ok = scenario_background_is_immune() && ok;
    ok = scenario_wait_done_signal() && ok;

    std::clog << (ok ? "[full] 6 个场景全部通过\n" : "[full] 存在失败场景\n");
    return ok ? 0 : 1;
}
